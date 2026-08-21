/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/vm-reclaim.h"
#include "kern/vmspace.h"
#include "kern/swap.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/page.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define PAGE_SIZE ZEDBSD_PAGE_SIZE

static struct vm_private_page *page_queue;
static struct mutex reclaim_lock;
struct vm_reclaim_stats vm_reclaim_counters;
#define stats vm_reclaim_counters

__attribute__((weak)) int vm_object_reclaim_one(void) { return ENOMEM; }
__attribute__((weak)) unsigned vm_object_page_count(void) { return 0; }

static uint32_t mapping_prot(const struct vm_page *page)
{
	uint32_t prot = page->region->prot;
	if (page->flags & VM_MAPPING_COW)
		prot &= ~HAL_SPACE_WRITE;
	return prot;
}

void vm_reclaim_init(void)
{
	page_queue = NULL;
	memset(&stats, 0, sizeof(stats));
	(void)mutex_init(&reclaim_lock, LOCK_RANK_VMSPACE, "VM reclaim");
}

static void queue_remove(struct vm_private_page *backing)
{
	struct vm_private_page **link = &page_queue;
	while (*link != NULL && *link != backing)
		link = &(*link)->queue_next;
	if (*link == backing)
		*link = backing->queue_next;
}

void vm_page_track(struct vm_page *page)
{
	struct vm_private_page *backing;
	if (page == NULL || (backing = page->private_page) == NULL)
		return;
	mutex_lock(&reclaim_lock);
	backing->queue_next = page_queue;
	page_queue = backing;
	if (backing->flags & VM_PAGE_RESIDENT)
		stats.resident++;
	mutex_unlock(&reclaim_lock);
}

void vm_page_untrack(struct vm_page *page)
{
	struct vm_private_page *backing;
	struct vm_page **link;
	int last;
	if (page == NULL || (backing = page->private_page) == NULL)
		return;
	mutex_lock(&reclaim_lock);
	link = &backing->mappings;
	while (*link != NULL && *link != page)
		link = &(*link)->private_next;
	if (*link == page)
		*link = page->private_next;
	page->private_page = NULL;
	page->private_next = NULL;
	last = refcount_put(&backing->refs);
	if (last) {
		queue_remove(backing);
		if ((backing->flags & VM_PAGE_RESIDENT) && stats.resident)
			stats.resident--;
		if ((backing->flags & VM_PAGE_SWAPPED) && stats.swapped)
			stats.swapped--;
	}
	mutex_unlock(&reclaim_lock);
	if (!last)
		return;
	if (backing->flags & VM_PAGE_RESIDENT)
		(void)hal_pmem_free(&backing->pmem);
	if ((backing->flags & VM_PAGE_SWAPPED) && swap_system_backend() != NULL)
		swap_free_slot(swap_system_backend(), backing->swap_slot);
	kern_free(backing);
}

void vm_page_replace_private(struct vm_page *page,
	struct vm_private_page *fresh)
{
	struct vm_private_page *old;
	struct vm_page **link;
	int last;
	if (page == NULL || fresh == NULL || page->private_page == NULL)
		HAL_FATAL("invalid private page replacement");
	old = page->private_page;
	mutex_lock(&reclaim_lock);
	link = &old->mappings;
	while (*link != NULL && *link != page)
		link = &(*link)->private_next;
	if (*link != page)
		HAL_FATAL("private page mapping not linked");
	*link = page->private_next;
	last = refcount_put(&old->refs);
	page->private_page = fresh;
	page->private_next = fresh->mappings;
	fresh->mappings = page;
	fresh->queue_next = page_queue;
	page_queue = fresh;
	if (fresh->flags & VM_PAGE_RESIDENT)
		stats.resident++;
	if (last) {
		queue_remove(old);
		if ((old->flags & VM_PAGE_RESIDENT) && stats.resident)
			stats.resident--;
		if ((old->flags & VM_PAGE_SWAPPED) && stats.swapped)
			stats.swapped--;
	}
	mutex_unlock(&reclaim_lock);
	if (!last)
		return;
	if (old->flags & VM_PAGE_RESIDENT)
		(void)hal_pmem_free(&old->pmem);
	if ((old->flags & VM_PAGE_SWAPPED) && swap_system_backend() != NULL)
		swap_free_slot(swap_system_backend(), old->swap_slot);
	kern_free(old);
}

int vm_page_share_private(struct vm_page *source, struct vm_page *copy)
{
	struct vm_private_page *backing;
	int error = 0;
	if (source == NULL || copy == NULL || source->private_page == NULL)
		return EINVAL;
	mutex_lock(&reclaim_lock);
	backing = source->private_page;
	refcount_get(&backing->refs);
	copy->private_page = backing;
	copy->private_next = backing->mappings;
	backing->mappings = copy;
	source->flags |= VM_MAPPING_COW;
	copy->flags |= VM_MAPPING_COW;
	if ((source->flags & VM_MAPPING_MAPPED) != 0 &&
	    hal_page_prot(source->vm->space, (void *)source->address,
	    PAGE_SIZE, mapping_prot(source)) != HAL_OK)
		error = ENOMEM;
	if (error != 0) {
		backing->mappings = copy->private_next;
		copy->private_page = NULL;
		copy->private_next = NULL;
		(void)refcount_put(&backing->refs);
	}
	mutex_unlock(&reclaim_lock);
	return error;
}

int vm_page_cow_reuse(struct vm_page *page)
{
	int result = EAGAIN;
	if (page == NULL || page->private_page == NULL)
		return EINVAL;
	mutex_lock(&reclaim_lock);
	if (refcount_load(&page->private_page->refs) == 1) {
		page->flags &= ~VM_MAPPING_COW;
		if ((page->flags & VM_MAPPING_MAPPED) != 0 &&
		    hal_page_prot(page->vm->space, (void *)page->address,
		    PAGE_SIZE, mapping_prot(page)) != HAL_OK) {
			page->flags |= VM_MAPPING_COW;
			result = ENOMEM;
		} else
			result = 0;
	}
	mutex_unlock(&reclaim_lock);
	return result;
}

static int backing_wired_or_avoided(struct vm_private_page *backing,
	struct vm_page *avoid)
{
	struct vm_page *page;
	for (page = backing->mappings; page != NULL; page = page->private_next)
		if (page == avoid || page->wire_count != 0)
			return 1;
	return 0;
}

static uint32_t backing_pte_flags(struct vm_private_page *backing)
{
	struct vm_page *page;
	uint32_t combined = 0, flags;
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (!(page->flags & VM_MAPPING_MAPPED))
			continue;
		flags = 0;
		if (hal_page_query(page->vm->space, (void *)page->address,
		    &flags) == HAL_OK)
			combined |= flags;
	}
	return combined;
}

static void clear_accessed(struct vm_private_page *backing)
{
	struct vm_page *page;
	for (page = backing->mappings; page != NULL; page = page->private_next)
		if (page->flags & VM_MAPPING_MAPPED)
			(void)hal_page_clear_flags(page->vm->space,
			    (void *)page->address, HAL_PAGE_ACCESSED);
}

static int unmap_all(struct vm_private_page *backing)
{
	struct vm_page *page, *rollback;
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (!(page->flags & VM_MAPPING_MAPPED))
			continue;
		if (hal_page_unmap(page->vm->space, (void *)page->address,
		    PAGE_SIZE) != HAL_OK) {
			for (rollback = backing->mappings; rollback != page;
			     rollback = rollback->private_next)
				if (!(rollback->flags & VM_MAPPING_MAPPED) &&
				    hal_page_map(rollback->vm->space,
					    (void *)rollback->address,
					    backing->pmem.paddr, PAGE_SIZE,
					    mapping_prot(rollback)) == HAL_OK)
					rollback->flags |= VM_MAPPING_MAPPED;
			return EIO;
		}
		page->flags &= ~VM_MAPPING_MAPPED;
	}
	return 0;
}

static void remap_all(struct vm_private_page *backing)
{
	struct vm_page *page;
	for (page = backing->mappings; page != NULL; page = page->private_next)
		if (!(page->flags & VM_MAPPING_MAPPED) &&
		    hal_page_map(page->vm->space, (void *)page->address,
		    backing->pmem.paddr, PAGE_SIZE,
		    mapping_prot(page)) == HAL_OK)
			page->flags |= VM_MAPPING_MAPPED;
}

static int swap_out_backing(struct vm_private_page *backing)
{
	struct swap_backend *backend = swap_system_backend();
	uint32_t slot;
	int error;
	if (PAGE_SIZE != SWAP_PAGE_SIZE || backend == NULL)
		return ENOSPC;
	error = swap_alloc_slot(backend, &slot);
	if (error != 0)
		return error;
	backing->flags |= VM_PAGE_BUSY;
	if (unmap_all(backing) != 0) {
		swap_free_slot(backend, slot);
		backing->flags &= ~VM_PAGE_BUSY;
		return EIO;
	}
	error = swap_write_page(backend, slot, (void *)backing->pmem.vaddr);
	if (error != 0) {
		remap_all(backing);
		swap_free_slot(backend, slot);
		backing->flags &= ~VM_PAGE_BUSY;
		stats.io_errors++;
		return error;
	}
	(void)hal_pmem_free(&backing->pmem);
	backing->swap_slot = slot;
	backing->flags &= ~(VM_PAGE_RESIDENT | VM_PAGE_DIRTY | VM_PAGE_BUSY);
	backing->flags |= VM_PAGE_SWAPPED;
	if (stats.resident) stats.resident--;
	stats.swapped++;
	stats.page_outs++;
	stats.reclaims++;
	return 0;
}

static int discard_file_backing(struct vm_private_page *backing)
{
	struct vm_page *page, *next;
	if (unmap_all(backing) != 0)
		return EIO;
	for (page = backing->mappings; page != NULL; page = next) {
		struct vm_page **link = &page->region->pages;
		next = page->private_next;
		while (*link != NULL && *link != page)
			link = &(*link)->next;
		if (*link == page)
			*link = page->next;
		vm_page_free_metadata(page);
	}
	queue_remove(backing);
	if (stats.resident) stats.resident--;
	(void)hal_pmem_free(&backing->pmem);
	kern_free(backing);
	stats.reclaims++;
	return 0;
}

int vm_reclaim_one(struct vm_page *avoid)
{
	unsigned pass;
	int result = ENOMEM;
	mutex_lock(&reclaim_lock);
	for (pass = 0; pass < 2; pass++) {
		struct vm_private_page *backing;
		for (backing = page_queue; backing != NULL;
		     backing = backing->queue_next) {
			uint32_t flags;
			if (!(backing->flags & VM_PAGE_RESIDENT) ||
			    (backing->flags & VM_PAGE_BUSY) ||
			    backing_wired_or_avoided(backing, avoid))
				continue;
			flags = backing_pte_flags(backing);
			if (pass == 0 && (flags & HAL_PAGE_ACCESSED)) {
				clear_accessed(backing);
				continue;
			}
			if (!(flags & HAL_PAGE_DIRTY) &&
			    !(backing->flags & VM_PAGE_DIRTY) &&
			    backing->mappings != NULL &&
			    backing->mappings->region->backing == VM_BACKING_FILE &&
			    discard_file_backing(backing) == 0) {
				result = 0;
				goto out;
			}
			/* Shared private pages have one swap identity. */
			if (swap_out_backing(backing) == 0) {
				result = 0;
				goto out;
			}
		}
	}
	if (vm_object_reclaim_one() == 0) {
		stats.reclaims++;
		result = 0;
	}
out:
	mutex_unlock(&reclaim_lock);
	return result;
}

void vm_page_note_in(struct vm_page *page)
{
	if (page == NULL)
		return;
	mutex_lock(&reclaim_lock);
	if (stats.swapped) stats.swapped--;
	stats.resident++;
	stats.page_ins++;
	mutex_unlock(&reclaim_lock);
}

void vm_reclaim_note_fault(void)
{
	mutex_lock(&reclaim_lock);
	stats.faults++;
	mutex_unlock(&reclaim_lock);
}

void vm_reclaim_get_stats(struct vm_reclaim_stats *output)
{
	struct vm_private_page *backing;
	if (output == NULL)
		return;
	mutex_lock(&reclaim_lock);
	memcpy(output, &stats, sizeof(*output));
	output->anonymous_resident = output->file_resident = 0;
	output->wired = output->busy = output->dirty = output->clean = 0;
	for (backing = page_queue; backing != NULL; backing = backing->queue_next) {
		struct vm_page *page;
		uint32_t flags = backing_pte_flags(backing);
		if (backing->flags & VM_PAGE_BUSY)
			output->busy++;
		for (page = backing->mappings; page != NULL;
		     page = page->private_next)
			if (page->wire_count != 0) { output->wired++; break; }
		if (!(backing->flags & VM_PAGE_RESIDENT))
			continue;
		page = backing->mappings;
		if (page != NULL && page->region->backing == VM_BACKING_ANON)
			output->anonymous_resident++;
		else
			output->file_resident++;
		if ((backing->flags & VM_PAGE_DIRTY) || (flags & HAL_PAGE_DIRTY))
			output->dirty++;
		else
			output->clean++;
	}
	output->file_resident += vm_object_page_count();
	output->resident += vm_object_page_count();
	mutex_unlock(&reclaim_lock);
}
