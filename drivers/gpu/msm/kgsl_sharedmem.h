/* Copyright (c) 2002,2007-2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#ifndef __KGSL_SHAREDMEM_H
#define __KGSL_SHAREDMEM_H

#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include "kgsl_mmu.h"
#include <linux/slab.h>
#include <linux/kmemleak.h>

#include "kgsl_log.h"

struct kgsl_device;
struct kgsl_process_private;

#define KGSL_CACHE_OP_INV       0x01
#define KGSL_CACHE_OP_FLUSH     0x02
#define KGSL_CACHE_OP_CLEAN     0x03

extern struct kgsl_memdesc_ops kgsl_page_alloc_ops;

int kgsl_sharedmem_page_alloc(struct kgsl_memdesc *memdesc,
			   struct kgsl_pagetable *pagetable, size_t size);

int kgsl_sharedmem_page_alloc_user(struct kgsl_memdesc *memdesc,
				struct kgsl_pagetable *pagetable,
				size_t size);

int kgsl_sharedmem_alloc_coherent(struct kgsl_memdesc *memdesc, size_t size);

int kgsl_sharedmem_ebimem_user(struct kgsl_memdesc *memdesc,
			     struct kgsl_pagetable *pagetable,
			     size_t size);

int kgsl_sharedmem_ebimem(struct kgsl_memdesc *memdesc,
			struct kgsl_pagetable *pagetable,
			size_t size);

void kgsl_sharedmem_free(struct kgsl_memdesc *memdesc);

int kgsl_sharedmem_readl(const struct kgsl_memdesc *memdesc,
			uint32_t *dst,
			unsigned int offsetbytes);

int kgsl_sharedmem_writel(const struct kgsl_memdesc *memdesc,
			unsigned int offsetbytes,
			uint32_t src);

int kgsl_sharedmem_set(const struct kgsl_memdesc *memdesc,
			unsigned int offsetbytes, unsigned int value,
			unsigned int sizebytes);

void kgsl_cache_range_op(struct kgsl_memdesc *memdesc, int op);

void kgsl_process_init_sysfs(struct kgsl_process_private *private);
void kgsl_process_uninit_sysfs(struct kgsl_process_private *private);

int kgsl_sharedmem_init_sysfs(void);
void kgsl_sharedmem_uninit_sysfs(void);

/*
 * kgsl_memdesc_get_align - Get alignment flags from a memdesc
 * @memdesc - the memdesc
 *
 * Returns the alignment requested, as power of 2 exponent.
 */
static inline int
kgsl_memdesc_get_align(const struct kgsl_memdesc *memdesc)
{
	return (memdesc->flags & KGSL_MEMALIGN_MASK) >> KGSL_MEMALIGN_SHIFT;
}

/*
 * kgsl_memdesc_set_align - Set alignment flags of a memdesc
 * @memdesc - the memdesc
 * @align - alignment requested, as a power of 2 exponent.
 */
static inline int
kgsl_memdesc_set_align(struct kgsl_memdesc *memdesc, unsigned int align)
{
	if (align > 32) {
		KGSL_CORE_ERR("Alignment too big, restricting to 2^32\n");
		align = 32;
	}

	memdesc->flags &= ~KGSL_MEMALIGN_MASK;
	memdesc->flags |= (align << KGSL_MEMALIGN_SHIFT) & KGSL_MEMALIGN_MASK;
	return 0;
}

static inline unsigned int kgsl_get_sg_pa(struct scatterlist *sg)
{
	/*
	 * Try sg_dma_address first to support ion carveout
	 * regions which do not work with sg_phys().
	 */
	unsigned int pa = sg_dma_address(sg);
	if (pa == 0)
		pa = sg_phys(sg);
	return pa;
}

int
kgsl_sharedmem_map_vma(struct vm_area_struct *vma,
			const struct kgsl_memdesc *memdesc);

/*
 * For relatively small sglists, it is preferable to use kzalloc
 * rather than going down the vmalloc rat hole.  If the size of
 * the sglist is < PAGE_SIZE use kzalloc otherwise fallback to
 * vmalloc
 */

static inline void *kgsl_sg_alloc(unsigned int sglen)
{
	if ((sglen * sizeof(struct scatterlist)) <  PAGE_SIZE)
		return kzalloc(sglen * sizeof(struct scatterlist), GFP_KERNEL);
	else {
		void *ptr = vmalloc(sglen * sizeof(struct scatterlist));
		if (ptr)
			memset(ptr, 0, sglen * sizeof(struct scatterlist));

		return ptr;
	}
}

static inline void kgsl_sg_free(void *ptr, unsigned int sglen)
{
	if ((sglen * sizeof(struct scatterlist)) < PAGE_SIZE)
		kfree(ptr);
	else
		vfree(ptr);
}

static inline int
memdesc_sg_phys(struct kgsl_memdesc *memdesc,
		unsigned int physaddr, unsigned int size)
{
	memdesc->sg = kgsl_sg_alloc(1);
	if (!memdesc->sg)
		return -ENOMEM;

	kmemleak_not_leak(memdesc->sg);

	memdesc->sglen = 1;
	sg_init_table(memdesc->sg, 1);
	memdesc->sg[0].length = size;
	memdesc->sg[0].offset = 0;
	memdesc->sg[0].dma_address = physaddr;
	return 0;
}

static inline int
kgsl_allocate(struct kgsl_memdesc *memdesc,
		struct kgsl_pagetable *pagetable, size_t size)
{
	if (kgsl_mmu_get_mmutype() == KGSL_MMU_TYPE_NONE)
		return kgsl_sharedmem_ebimem(memdesc, pagetable, size);
	memdesc->flags |= (KGSL_MEMTYPE_KERNEL << KGSL_MEMTYPE_SHIFT);
	return kgsl_sharedmem_page_alloc(memdesc, pagetable, size);
}

static inline int
kgsl_allocate_user(struct kgsl_memdesc *memdesc,
		struct kgsl_pagetable *pagetable,
		size_t size, unsigned int flags)
{
	int ret;

	memdesc->flags = flags;

	if (kgsl_mmu_get_mmutype() == KGSL_MMU_TYPE_NONE)
		ret = kgsl_sharedmem_ebimem_user(memdesc, pagetable, size);
	else
		ret = kgsl_sharedmem_page_alloc_user(memdesc, pagetable, size);

	return ret;
}

static inline int
kgsl_allocate_contiguous(struct kgsl_memdesc *memdesc, size_t size)
{
	int ret  = kgsl_sharedmem_alloc_coherent(memdesc, size);
	if (!ret && (kgsl_mmu_get_mmutype() == KGSL_MMU_TYPE_NONE))
		memdesc->gpuaddr = memdesc->physaddr;

	memdesc->flags |= (KGSL_MEMTYPE_KERNEL << KGSL_MEMTYPE_SHIFT);
	return ret;
}

static inline int kgsl_sg_size(struct scatterlist *sg, int sglen)
{
	int i, size = 0;
	struct scatterlist *s;

	for_each_sg(sg, s, sglen, i) {
		size += s->length;
	}

	return size;
}
#endif /* __KGSL_SHAREDMEM_H */
