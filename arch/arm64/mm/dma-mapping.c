/*
 * SWIOTLB-based DMA API implementation
 *
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/gfp.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/genalloc.h>
#include <linux/dma-mapping.h>
#include <linux/dma-contiguous.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>
#include <linux/swiotlb.h>
#include <linux/amba/bus.h>

#include <asm/cacheflush.h>

struct dma_map_ops *dma_ops;
EXPORT_SYMBOL(dma_ops);

static pgprot_t __get_dma_pgprot(struct dma_attrs *attrs, pgprot_t prot,
				 bool coherent)
{
	if (dma_get_attr(DMA_ATTR_WRITE_COMBINE, attrs))
		return pgprot_writecombine(prot);
	else if (!coherent)
		return pgprot_dmacoherent(prot);
	return prot;
}

static struct gen_pool *atomic_pool;

#define DEFAULT_DMA_COHERENT_POOL_SIZE  SZ_256K
static size_t atomic_pool_size __initdata = DEFAULT_DMA_COHERENT_POOL_SIZE;

static int __init early_coherent_pool(char *p)
{
	atomic_pool_size = memparse(p, &p);
	return 0;
}
early_param("coherent_pool", early_coherent_pool);

static void *__alloc_from_pool(size_t size, struct page **ret_page)
{
	unsigned long val;
	void *ptr = NULL;

	if (!atomic_pool) {
		WARN(1, "coherent pool not initialised!\n");
		return NULL;
	}

	val = gen_pool_alloc(atomic_pool, size);
	if (val) {
		phys_addr_t phys = gen_pool_virt_to_phys(atomic_pool, val);

		*ret_page = phys_to_page(phys);
		ptr = (void *)val;
	}

	return ptr;
}

static bool __in_atomic_pool(void *start, size_t size)
{
	return addr_in_gen_pool(atomic_pool, (unsigned long)start, size);
}

static int __free_from_pool(void *start, size_t size)
{
	if (!__in_atomic_pool(start, size))
		return 0;

	gen_pool_free(atomic_pool, (unsigned long)start, size);

	return 1;
}

static void *__dma_alloc_coherent(struct device *dev, size_t size,
				  dma_addr_t *dma_handle, gfp_t flags,
				  struct dma_attrs *attrs)
{
	if (IS_ENABLED(CONFIG_ZONE_DMA) &&
	    dev->coherent_dma_mask <= DMA_BIT_MASK(32))
		flags |= GFP_DMA;

	if (IS_ENABLED(CONFIG_DMA_CMA) && (flags & __GFP_WAIT)) {
		struct page *page;

		page = dma_alloc_from_contiguous(dev,
						PAGE_ALIGN(size) >> PAGE_SHIFT,
							get_order(size));
		if (page) {
			*dma_handle = phys_to_dma(dev, page_to_phys(page));
			return page_address(page);
		} else if (dev_get_cma_priv_area(dev)) {
			pr_err("%s: failed to allocate from dma-contiguous\n",
								__func__);
			return NULL;
		}
	}

	return swiotlb_alloc_coherent(dev, size, dma_handle, flags);
}

static void __dma_free_coherent(struct device *dev, size_t size,
				void *vaddr, dma_addr_t dma_handle,
				struct dma_attrs *attrs)
{
	bool freed;
	phys_addr_t paddr = dma_to_phys(dev, dma_handle);

	if (dev == NULL) {
		WARN_ONCE(1, "Use an actual device structure for DMA allocation\n");
		return;
	}

	freed = dma_release_from_contiguous(dev,
					phys_to_page(paddr),
					PAGE_ALIGN(size) >> PAGE_SHIFT);
	if (!freed)
		swiotlb_free_coherent(dev, size, vaddr, dma_handle);
}

static void *__dma_alloc_noncoherent(struct device *dev, size_t size,
				     dma_addr_t *dma_handle, gfp_t flags,
				     struct dma_attrs *attrs)
{
	void *ptr, *coherent_ptr;
	struct page *page;

	size = PAGE_ALIGN(size);

	if (!(flags & __GFP_WAIT)) {
		struct page *page = NULL;
		void *addr = __alloc_from_pool(size, &page);

		if (addr)
			*dma_handle = phys_to_dma(dev, page_to_phys(page));

		return addr;

	}
	if (IS_ENABLED(CONFIG_DMA_CMA)) {
		struct page *page;

		page = dma_alloc_from_contiguous(dev, size >> PAGE_SHIFT,
							get_order(size));
		if (page) {
			*dma_handle = phys_to_dma(dev, page_to_phys(page));
			ptr = page_address(page);
		} else if (dev_get_cma_priv_area(dev)) {
			pr_err("%s: failed to allocate from dma-contiguous\n",
								__func__);
			return NULL;
		} else {
			ptr = __dma_alloc_coherent(
					dev, size, dma_handle, flags, attrs);
		}
	} else {
		ptr = __dma_alloc_coherent(dev, size, dma_handle, flags, attrs);
	}

	if (!ptr)
		goto no_mem;

	/* remove any dirty cache lines on the kernel alias */
	__dma_flush_range(ptr, ptr + size);

	/* create a coherent mapping */
	page = virt_to_page(ptr);
	coherent_ptr = dma_common_contiguous_remap(page, size, VM_USERMAP,
				__get_dma_pgprot(attrs,
					__pgprot(PROT_NORMAL_NC), false),
					NULL);
	if (!coherent_ptr)
		goto no_map;

	return coherent_ptr;

no_map:
	__dma_free_coherent(dev, size, ptr, *dma_handle, attrs);
no_mem:
	*dma_handle = ~0;
	return NULL;
}

static void __dma_free_noncoherent(struct device *dev, size_t size,
				   void *vaddr, dma_addr_t dma_handle,
				   struct dma_attrs *attrs)
{
	void *swiotlb_addr = phys_to_virt(dma_to_phys(dev, dma_handle));

	if (__free_from_pool(vaddr, size))
		return;
	vunmap(vaddr);

	if (dev == NULL) {
		WARN_ONCE(1, "Use an actual device structure for DMA allocation\n");
		return;
	}

	if (IS_ENABLED(CONFIG_DMA_CMA)) {
		phys_addr_t paddr = dma_to_phys(dev, dma_handle);

		if (dma_release_from_contiguous(dev,
					phys_to_page(paddr),
					PAGE_ALIGN(size) >> PAGE_SHIFT))
			return;
	}

	swiotlb_free_coherent(dev, size, swiotlb_addr, dma_handle);
}

static dma_addr_t __swiotlb_map_page(struct device *dev, struct page *page,
				     unsigned long offset, size_t size,
				     enum dma_data_direction dir,
				     struct dma_attrs *attrs)
{
	dma_addr_t dev_addr;

	dev_addr = swiotlb_map_page(dev, page, offset, size, dir, attrs);
	__dma_map_area(phys_to_virt(dma_to_phys(dev, dev_addr)), size, dir);

	return dev_addr;
}


static void __swiotlb_unmap_page(struct device *dev, dma_addr_t dev_addr,
				 size_t size, enum dma_data_direction dir,
				 struct dma_attrs *attrs)
{
	__dma_unmap_area(phys_to_virt(dma_to_phys(dev, dev_addr)), size, dir);
	swiotlb_unmap_page(dev, dev_addr, size, dir, attrs);
}

static int __swiotlb_map_sg_attrs(struct device *dev, struct scatterlist *sgl,
				  int nelems, enum dma_data_direction dir,
				  struct dma_attrs *attrs)
{
	struct scatterlist *sg;
	int i, ret;

	ret = swiotlb_map_sg_attrs(dev, sgl, nelems, dir, attrs);
	if (!dma_get_attr(DMA_ATTR_SKIP_CPU_SYNC, attrs)) {
		for_each_sg(sgl, sg, ret, i)
			__dma_map_area(phys_to_virt(dma_to_phys(dev, sg->dma_address)),
					sg->length, dir);
	}

	return ret;
}

static void __swiotlb_unmap_sg_attrs(struct device *dev,
				     struct scatterlist *sgl, int nelems,
				     enum dma_data_direction dir,
				     struct dma_attrs *attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nelems, i)
		__dma_unmap_area(phys_to_virt(dma_to_phys(dev, sg->dma_address)),
				 sg->length, dir);
	swiotlb_unmap_sg_attrs(dev, sgl, nelems, dir, attrs);
}

static void __swiotlb_sync_single_for_cpu(struct device *dev,
					  dma_addr_t dev_addr, size_t size,
					  enum dma_data_direction dir)
{
	__dma_unmap_area(phys_to_virt(dma_to_phys(dev, dev_addr)), size, dir);
	swiotlb_sync_single_for_cpu(dev, dev_addr, size, dir);
}

static void __swiotlb_sync_single_for_device(struct device *dev,
					     dma_addr_t dev_addr, size_t size,
					     enum dma_data_direction dir)
{
	swiotlb_sync_single_for_device(dev, dev_addr, size, dir);
	__dma_map_area(phys_to_virt(dma_to_phys(dev, dev_addr)), size, dir);
}

static void __swiotlb_sync_sg_for_cpu(struct device *dev,
				      struct scatterlist *sgl, int nelems,
				      enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nelems, i)
		__dma_unmap_area(phys_to_virt(dma_to_phys(dev, sg->dma_address)),
				 sg->length, dir);
	swiotlb_sync_sg_for_cpu(dev, sgl, nelems, dir);
}

static void __swiotlb_sync_sg_for_device(struct device *dev,
					 struct scatterlist *sgl, int nelems,
					 enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	swiotlb_sync_sg_for_device(dev, sgl, nelems, dir);
	for_each_sg(sgl, sg, nelems, i)
		__dma_map_area(phys_to_virt(dma_to_phys(dev, sg->dma_address)),
			       sg->length, dir);
}

/* vma->vm_page_prot must be set appropriately before calling this function */
static int __dma_common_mmap(struct device *dev, struct vm_area_struct *vma,
			     void *cpu_addr, dma_addr_t dma_addr, size_t size)
{
	int ret = -ENXIO;
	unsigned long nr_vma_pages = (vma->vm_end - vma->vm_start) >>
					PAGE_SHIFT;
	unsigned long nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	unsigned long pfn = dma_to_phys(dev, dma_addr) >> PAGE_SHIFT;
	unsigned long off = vma->vm_pgoff;

	if (dma_mmap_from_coherent(dev, vma, cpu_addr, size, &ret))
		return ret;

	if (off < nr_pages && nr_vma_pages <= (nr_pages - off)) {
		ret = remap_pfn_range(vma, vma->vm_start,
				      pfn + off,
				      vma->vm_end - vma->vm_start,
				      vma->vm_page_prot);
	}

	return ret;
}

static int __swiotlb_mmap_noncoherent(struct device *dev,
		struct vm_area_struct *vma,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		struct dma_attrs *attrs)
{
	vma->vm_page_prot = __get_dma_pgprot(attrs, vma->vm_page_prot, false);
	return __dma_common_mmap(dev, vma, cpu_addr, dma_addr, size);
}

static int __swiotlb_mmap_coherent(struct device *dev,
		struct vm_area_struct *vma,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		struct dma_attrs *attrs)
{
	/* Just use whatever page_prot attributes were specified */
	return __dma_common_mmap(dev, vma, cpu_addr, dma_addr, size);
}

struct dma_map_ops noncoherent_swiotlb_dma_ops = {
	.alloc = __dma_alloc_noncoherent,
	.free = __dma_free_noncoherent,
	.mmap = __swiotlb_mmap_noncoherent,
	.map_page = __swiotlb_map_page,
	.unmap_page = __swiotlb_unmap_page,
	.map_sg = __swiotlb_map_sg_attrs,
	.unmap_sg = __swiotlb_unmap_sg_attrs,
	.sync_single_for_cpu = __swiotlb_sync_single_for_cpu,
	.sync_single_for_device = __swiotlb_sync_single_for_device,
	.sync_sg_for_cpu = __swiotlb_sync_sg_for_cpu,
	.sync_sg_for_device = __swiotlb_sync_sg_for_device,
	.dma_supported = swiotlb_dma_supported,
	.mapping_error = swiotlb_dma_mapping_error,
};
EXPORT_SYMBOL(noncoherent_swiotlb_dma_ops);

struct dma_map_ops coherent_swiotlb_dma_ops = {
	.alloc = __dma_alloc_coherent,
	.free = __dma_free_coherent,
	.mmap = __swiotlb_mmap_coherent,
	.map_page = swiotlb_map_page,
	.unmap_page = swiotlb_unmap_page,
	.map_sg = swiotlb_map_sg_attrs,
	.unmap_sg = swiotlb_unmap_sg_attrs,
	.sync_single_for_cpu = swiotlb_sync_single_for_cpu,
	.sync_single_for_device = swiotlb_sync_single_for_device,
	.sync_sg_for_cpu = swiotlb_sync_sg_for_cpu,
	.sync_sg_for_device = swiotlb_sync_sg_for_device,
	.dma_supported = swiotlb_dma_supported,
	.mapping_error = swiotlb_dma_mapping_error,
};
EXPORT_SYMBOL(coherent_swiotlb_dma_ops);

static void *arm_exynos_dma_mcode_alloc(struct device *dev, size_t size,
	dma_addr_t *handle, gfp_t gfp, struct dma_attrs *attrs);
static void arm_exynos_dma_mcode_free(struct device *dev, size_t size, void *cpu_addr,
				  dma_addr_t handle, struct dma_attrs *attrs);

struct dma_map_ops arm_exynos_dma_mcode_ops = {
	.alloc			= arm_exynos_dma_mcode_alloc,
	.free			= arm_exynos_dma_mcode_free,
};
EXPORT_SYMBOL(arm_exynos_dma_mcode_ops);

static void *arm_exynos_dma_mcode_alloc(struct device *dev, size_t size,
	dma_addr_t *handle, gfp_t gfp, struct dma_attrs *attrs)
{
	void *addr;

	if (!*handle)
		return NULL;

	addr = ioremap(*handle, size);

	return addr;
}

static void arm_exynos_dma_mcode_free(struct device *dev, size_t size, void *cpu_addr,
				  dma_addr_t handle, struct dma_attrs *attrs)
{
	iounmap(cpu_addr);
}

static int dma_bus_notifier(struct notifier_block *nb,
			    unsigned long event, void *_dev)
{
	struct device *dev = _dev;

	if (event != BUS_NOTIFY_ADD_DEVICE)
		return NOTIFY_DONE;

	if (of_property_read_bool(dev->of_node, "dma-coherent"))
		set_dma_ops(dev, &coherent_swiotlb_dma_ops);

	return NOTIFY_OK;
}

static struct notifier_block platform_bus_nb = {
	.notifier_call = dma_bus_notifier,
};

static struct notifier_block amba_bus_nb = {
	.notifier_call = dma_bus_notifier,
};

extern int swiotlb_late_init_with_default_size(size_t default_size);

static int __init atomic_pool_init(void)
{
	pgprot_t prot = __pgprot(PROT_NORMAL_NC);
	unsigned long nr_pages = atomic_pool_size >> PAGE_SHIFT;
	struct page *page;
	void *addr;
	unsigned int pool_size_order = get_order(atomic_pool_size);

	if (dev_get_cma_area(NULL))
		page = dma_alloc_from_contiguous(NULL, nr_pages,
							pool_size_order);
	else
		page = alloc_pages(GFP_DMA, pool_size_order);

	if (page) {
		int ret;
		void *page_addr = page_address(page);

		memset(page_addr, 0, atomic_pool_size);
		__dma_flush_range(page_addr, page_addr + atomic_pool_size);

		atomic_pool = gen_pool_create(PAGE_SHIFT, -1);
		if (!atomic_pool)
			goto free_page;

		addr = dma_common_contiguous_remap(page, atomic_pool_size,
					VM_USERMAP, prot, atomic_pool_init);

		if (!addr)
			goto destroy_genpool;

		ret = gen_pool_add_virt(atomic_pool, (unsigned long)addr,
					page_to_phys(page),
					atomic_pool_size, -1);
		if (ret)
			goto remove_mapping;

		gen_pool_set_algo(atomic_pool,
				  gen_pool_first_fit_order_align,
				  (void *)PAGE_SHIFT);

		pr_info("DMA: preallocated %zu KiB pool for atomic allocations\n",
			atomic_pool_size / 1024);
		return 0;
	}
	goto out;

remove_mapping:
	dma_common_free_remap(addr, atomic_pool_size, VM_USERMAP);
destroy_genpool:
	gen_pool_destroy(atomic_pool);
	atomic_pool = NULL;
free_page:
	if (!dma_release_from_contiguous(NULL, page, nr_pages))
		__free_pages(page, pool_size_order);
out:
	pr_err("DMA: failed to allocate %zu KiB pool for atomic coherent allocation\n",
		atomic_pool_size / 1024);
	return -ENOMEM;
}

static int __init swiotlb_late_init(void)
{
	size_t swiotlb_size = min(SZ_64M, MAX_ORDER_NR_PAGES << PAGE_SHIFT);

	/*
	 * These must be registered before of_platform_populate().
	 */
	bus_register_notifier(&platform_bus_type, &platform_bus_nb);
	bus_register_notifier(&amba_bustype, &amba_bus_nb);

	dma_ops = &noncoherent_swiotlb_dma_ops;

	return swiotlb_late_init_with_default_size(swiotlb_size);
}

static int __init arm64_dma_init(void)
{
	int ret = 0;

	ret |= swiotlb_late_init();
	ret |= atomic_pool_init();

	return ret;
}
arch_initcall(arm64_dma_init);

#define PREALLOC_DMA_DEBUG_ENTRIES	4096

static int __init dma_debug_do_init(void)
{
	dma_debug_init(PREALLOC_DMA_DEBUG_ENTRIES);
	return 0;
}
fs_initcall(dma_debug_do_init);
