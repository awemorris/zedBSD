/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/vm-reclaim.h"
#include "kern/vmspace.h"
#include "kern/swap.h"
#include "kern/kmem.h"
#include "kern/page.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define PAGE_SIZE ZEDBSD_PAGE_SIZE

static struct vm_page *page_queue;
struct vm_reclaim_stats vm_reclaim_counters;
#define stats vm_reclaim_counters

/* vm-reclaim-host-test deliberately links without the object subsystem. */
__attribute__((weak)) int vm_object_reclaim_one(void) { return ENOMEM; }
__attribute__((weak)) unsigned vm_object_page_count(void) { return 0; }

void vm_page_track(struct vm_page *page)
{
	if (page == NULL)
		return;
	page->queue_next = page_queue;
	page_queue = page;
	if (page->flags & VM_PAGE_RESIDENT) stats.resident++;
}

void vm_page_untrack(struct vm_page *page)
{
	struct vm_page **link = &page_queue;
	while (*link != NULL && *link != page)
		link = &(*link)->queue_next;
	if (*link == page)
		*link = page->queue_next;
	if (page != NULL && (page->flags & VM_PAGE_RESIDENT) && stats.resident)
		stats.resident--;
	if (page != NULL && (page->flags & VM_PAGE_SWAPPED) && stats.swapped)
		stats.swapped--;
}

static void unlink_region_page(struct vm_page *page)
{
	struct vm_page **link = &page->region->pages;
	while (*link != NULL && *link != page)
		link = &(*link)->next;
	if (*link == page)
		*link = page->next;
}

static int discard_page(struct vm_page *page)
{
	if (hal_page_unmap(page->vm->space, (void *)page->address,
			   PAGE_SIZE) != HAL_OK)
		return EIO;
	unlink_region_page(page);
	vm_page_untrack(page);
	(void)hal_pmem_free(&page->pmem);
	kern_free(page);
	stats.reclaims++;
	return 0;
}

static int swap_out_page(struct vm_page *page)
{
	struct swap_backend *backend = swap_system_backend();
	uint32_t slot;
	int error;

	if (PAGE_SIZE != SWAP_PAGE_SIZE || backend == NULL ||
	    page->wire_count != 0)
		return ENOSPC;
	error = swap_alloc_slot(backend, &slot);
	if (error != 0)
		return error;
	page->flags |= VM_PAGE_BUSY;
	if (hal_page_unmap(page->vm->space, (void *)page->address,
			   PAGE_SIZE) != HAL_OK) {
		swap_free_slot(backend, slot);
		page->flags &= ~VM_PAGE_BUSY;
		return EIO;
	}
	error = swap_write_page(backend, slot, (void *)page->pmem.vaddr);
	if (error != 0) {
		(void)hal_page_map(page->vm->space, (void *)page->address,
			page->pmem.paddr, PAGE_SIZE, page->region->prot);
		swap_free_slot(backend, slot);
		page->flags &= ~VM_PAGE_BUSY;
		stats.io_errors++;
		return error;
	}
	(void)hal_pmem_free(&page->pmem);
	page->swap_slot = slot;
	page->flags &= ~(VM_PAGE_RESIDENT | VM_PAGE_DIRTY | VM_PAGE_BUSY);
	page->flags |= VM_PAGE_SWAPPED;
	if (stats.resident) stats.resident--;
	stats.swapped++;
	stats.page_outs++;
	stats.reclaims++;
	return 0;
}

int vm_reclaim_one(struct vm_page *avoid)
{
	unsigned pass;

	for (pass = 0; pass < 2; pass++) {
		struct vm_page *page = page_queue;
		while (page != NULL) {
			struct vm_page *next = page->queue_next;
			uint32_t flags = 0;

			if (page == avoid || !(page->flags & VM_PAGE_RESIDENT) ||
			    (page->flags & VM_PAGE_BUSY) || page->wire_count != 0) {
				page = next;
				continue;
			}
			if (hal_page_query(page->vm->space, (void *)page->address,
					   &flags) != HAL_OK) {
				page = next;
				continue;
			}
			if (pass == 0 && (flags & HAL_PAGE_ACCESSED)) {
				(void)hal_page_clear_flags(page->vm->space,
					(void *)page->address, HAL_PAGE_ACCESSED);
				page = next;
				continue;
			}
			if ((flags & HAL_PAGE_DIRTY) || (page->flags & VM_PAGE_DIRTY)) {
				if (swap_out_page(page) == 0)
					return 0;
			} else if (discard_page(page) == 0) {
				return 0;
			}
			page = next;
		}
	}
	if (vm_object_reclaim_one() == 0) {
		stats.reclaims++;
		return 0;
	}
	return ENOMEM;
}

void vm_page_note_in(struct vm_page *page)
{
	if (page == NULL)
		return;
	if (stats.swapped) stats.swapped--;
	stats.resident++;
	stats.page_ins++;
}

void vm_reclaim_note_fault(void)
{
	stats.faults++;
}

void vm_reclaim_get_stats(struct vm_reclaim_stats *output)
{
	struct vm_page *page;
	if (output == NULL)
		return;
	memcpy(output, &stats, sizeof(*output));
	output->anonymous_resident = output->file_resident = 0;
	output->wired = output->busy = output->dirty = output->clean = 0;
	for (page = page_queue; page != NULL; page = page->queue_next) {
		uint32_t flags = 0;
		if (page->flags & VM_PAGE_BUSY)
			output->busy++;
		if (page->wire_count != 0)
			output->wired++;
		if (!(page->flags & VM_PAGE_RESIDENT))
			continue;
		if (page->region->backing == VM_BACKING_ANON)
			output->anonymous_resident++;
		else
			output->file_resident++;
		(void)hal_page_query(page->vm->space, (void *)page->address,
				     &flags);
		if ((page->flags & VM_PAGE_DIRTY) || (flags & HAL_PAGE_DIRTY))
			output->dirty++;
		else
			output->clean++;
	}
	output->file_resident += vm_object_page_count();
	output->resident += vm_object_page_count();
}
