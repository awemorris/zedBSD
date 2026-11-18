/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The region list of an address space: range checks, insertion and
 * lookup, the per-region page index, and the search for free ranges.
 */

#include "kern/vmspace.h"
#include "kern/vm-device.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/page.h"
#include "kern/sched.h"
#include "kern/swap.h"
#include "kern/vm-commit.h"
#include "kern/vm-lock.h"
#include "kern/vm-object.h"
#include "kern/vm-reclaim.h"
#include <uapi/errno.h>
#include <uapi/mman.h>
#include <kern/kcrt.h>
#include "kern/clock.h"
#include "vmspace-internal.h"

/*
 * Tests whether a page-aligned range lies inside the user address range.
 */
int
vmspace_range_valid(
	uintptr_t start,
	size_t size)
{
	vmspace_layout_init();

	if (size == 0)
		return 0;

	if ((start & (PAGE_SIZE - 1U)) != 0)
		return 0;

	if ((size & (PAGE_SIZE - 1U)) != 0)
		return 0;

	if (start < vm_layout.user_minimum)
		return 0;

	if (start >= vm_layout.user_limit)
		return 0;

	if (size > vm_layout.user_limit - start)
		return 0;

	return 1;
}

/*
 * Tests whether a range intersects any region.
 */
int
vmspace_overlaps(
	struct vmspace *vm,
	uintptr_t start,
	size_t size)
{
	struct vm_region *region;
	uintptr_t end;

	/* Looks for a region that shares any byte with the range. */
	end = start + size;
	for (region = vm->regions; region != NULL; region = region->next) {
		if (start < region->start + region->size && region->start < end)
			return 1;
	}

	/* Reports a range no region touches. */
	return 0;
}

/*
 * Links a region into the address-sorted list.
 */
void
vmspace_insert_region(
	struct vmspace *vm,
	struct vm_region *region)
{
	struct vm_region **link;

	link = &vm->regions;
	while (*link != NULL && (*link)->start < region->start)
		link = &(*link)->next;
	region->next = *link;
	*link = region;
}

/*
 * Finds the region that contains a whole range; the caller holds the VM lock.
 */
struct vm_region *
vmspace_find_region_locked(
	struct vmspace *vm,
	uintptr_t address,
	size_t size)
{
	struct vm_region *region;

	/* Rejects a malformed query. */
	if (vm == NULL || size == 0 || address > UINTPTR_MAX - size)
		return NULL;

	/* Looks for the region that contains the whole range. */
	for (region = vm->regions; region != NULL; region = region->next) {
		if (address >= region->start &&
		    address + size <= region->start + region->size)
			return region;
	}

	/* Reports a range no single region covers. */
	return NULL;
}

/*
 * Finds the page record of an address within a region.
 */
struct vm_page *
vmspace_find_page(
	struct vm_region *region,
	uintptr_t address)
{
	struct vm_page *page;
	size_t bucket;

	/* Looks for the page that starts at the containing page address. */
	address &= ~(uintptr_t)(PAGE_SIZE - 1U);

	/* Walks one bucket of the index when the region has one. */
	if (region->page_index != NULL) {
		bucket = (address / PAGE_SIZE) & (region->page_index_size - 1U);
		for (page = region->page_index[bucket]; page != NULL; page = page->index_next) {
			if (page->address == address)
				return page;
		}

		/* The index holds every page of the region. */
		return NULL;
	}

	/* Otherwise walks the region's list. */
	for (page = region->pages; page != NULL; page = page->next) {
		if (page->address == address)
			return page;
	}

	/* No page starts there. */
	return NULL;
}

/*
 * Rebuilds a region's page index from its page list.
 *
 * The index has two buckets for every page, rounded up to a power of two and
 * at most VM_REGION_INDEX_MAXIMUM, once the region has enough pages to make
 * the list slow; below that the region has no index and faults walk the list
 * (BUG-033: a walk made faulting in a large library quadratic).  The new
 * buckets are had before the old ones go: without memory for them, keep
 * leaves a valid old index in place (its chains grow longer), and the next
 * try waits until the region has twice the pages (BUG-053).  Without keep
 * (the old index no longer matches the list) a failure leaves the region
 * without an index.  Returns 1 when the index was rebuilt, 0 otherwise.
 * The caller holds the VM lock.
 */
int
vmspace_region_page_index_rebuild(
	struct vm_region *region,
	int keep)
{
	struct vm_page **index;
	struct vm_page *page;
	size_t count;
	size_t size;
	size_t bucket;

	/* Counts the pages. */
	count = 0;
	for (page = region->pages; page != NULL; page = page->next)
		count++;
	region->page_count = count;

	/* A region with few pages keeps to its list. */
	if (count < VM_REGION_INDEX_MINIMUM) {
		vmspace_region_page_index_free(region);
		region->page_index_retry = 0;
		return 0;
	}

	/* Sizes the buckets to twice the pages, a power of two, within the maximum. */
	size = VM_REGION_INDEX_MINIMUM;
	while (size < count * 2U && size < VM_REGION_INDEX_MAXIMUM)
		size *= 2U;

	/* The new buckets, before the old ones go. */
	index = kern_calloc(size, sizeof(*index));
	if (index == NULL) {
		/* Not tried again until the region has twice the pages. */
		region->page_index_retry = count * 2U;

		/* A valid old index serves on; otherwise the list does. */
		if (!keep)
			vmspace_region_page_index_free(region);
		return 0;
	}

	/* The old index goes. */
	vmspace_region_page_index_free(region);
	region->page_index_retry = 0;

	/* Chains every page into its bucket. */
	for (page = region->pages; page != NULL; page = page->next) {
		bucket = (page->address / PAGE_SIZE) & (size - 1U);
		page->index_next = index[bucket];
		index[bucket] = page;
	}

	/* Publishes the index. */
	region->page_index = index;
	region->page_index_size = size;

	/* Succeeded: every page is in the new index. */
	return 1;
}

/*
 * Adds a page the caller has just linked into a region's list to the index.
 *
 * An index that the page count has outgrown is rebuilt twice as large (up to
 * the maximum), which also takes in the new page; when that cannot be had,
 * the page joins the old index.  The caller holds the VM lock.
 */
void
vmspace_region_page_index_insert(
	struct vm_region *region,
	struct vm_page *page)
{
	size_t bucket;
	int rebuilt;

	/* Counts the page. */
	region->page_count++;

	/* Builds an index once the region has enough pages (and is not waiting after a failure). */
	if (region->page_index == NULL) {
		if (region->page_count >= VM_REGION_INDEX_MINIMUM &&
		    region->page_count >= region->page_index_retry)
			(void)vmspace_region_page_index_rebuild(region, 0);
		return;
	}

	/* Rebuilds an index the count has outgrown, unless it is at its largest or waiting after a failure. */
	if (region->page_count > region->page_index_size &&
	    region->page_index_size < VM_REGION_INDEX_MAXIMUM &&
	    region->page_count >= region->page_index_retry) {
		rebuilt = vmspace_region_page_index_rebuild(region, 1);
		if (rebuilt)
			return;
	}

	/* Chains the page into its bucket. */
	bucket = (page->address / PAGE_SIZE) & (region->page_index_size - 1U);
	page->index_next = region->page_index[bucket];
	region->page_index[bucket] = page;
}

/*
 * Takes a page the caller is unlinking from a region's list out of the index.
 */
void
vmspace_region_page_index_remove(
	struct vm_region *region,
	struct vm_page *page)
{
	struct vm_page **link;
	size_t bucket;

	/* Uncounts the page. */
	if (region->page_count == 0)
		HAL_FATAL("VM region page count underflow");
	region->page_count--;

	/* A region without an index has nothing more to update. */
	if (region->page_index == NULL)
		return;

	/* Unchains the page from its bucket. */
	bucket = (page->address / PAGE_SIZE) & (region->page_index_size - 1U);
	link = &region->page_index[bucket];
	while (*link != NULL && *link != page)
		link = &(*link)->index_next;
	if (*link != page)
		HAL_FATAL("VM page missing from its region index");

	/* Leaves the page out of the chain. */
	*link = page->index_next;
	page->index_next = NULL;
}

/*
 * Frees a region's page index; the pages stay on the list.
 */
void
vmspace_region_page_index_free(
	struct vm_region *region)
{
	/* Gives the buckets back and leaves the region without an index. */
	kern_free(region->page_index);
	region->page_index = NULL;
	region->page_index_size = 0;
}

/*
 * Finds a free aligned range between two bounds; the caller holds the VM lock.
 */
int
vmspace_find_free_range_bounded_locked(
	struct vmspace *vm,
	uintptr_t minimum,
	uintptr_t maximum,
	size_t size,
	size_t alignment,
	uintptr_t *mapped)
{
	uintptr_t start;
	uintptr_t limit;
	struct vm_region *region;
	int moved;

	vmspace_layout_init();

	/* Validates the bounds and rounds the size up to pages. */
	if (vm == NULL ||
	    mapped == NULL ||
	    size == 0 ||
	    alignment < PAGE_SIZE ||
	    (alignment & (alignment - 1U)) != 0 ||
	    minimum < vm_layout.user_minimum ||
	    maximum > vm_layout.user_limit ||
	    minimum >= maximum ||
	    size > (size_t)(maximum - minimum))
		return EINVAL;

	if (size > SIZE_MAX - (PAGE_SIZE - 1U))
		return EOVERFLOW;

	/* Rounds the size up to whole pages at the full address width. */
	size = (size + PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	limit = maximum;
	if (vm->stack_guard_bottom != 0 && vm->stack_guard_bottom < limit)
		limit = vm->stack_guard_bottom;

	start = minimum;
	if (start > UINTPTR_MAX - (alignment - 1U))
		return ENOMEM;

	start = (start + alignment - 1U) & ~(uintptr_t)(alignment - 1U);

	/* Slides the candidate past every region it collides with. */
	for (;;) {
		moved = 0;

		if (start >= limit || size > limit - start)
			return ENOMEM;

		for (region = vm->regions; region != NULL; region = region->next) {
			if (start + size <= region->start)
				break;

			/* Moves the candidate past a region it overlaps. */
			if (start < region->start + region->size &&
			    region->start < start + size) {
				if (region->start > UINTPTR_MAX - region->size ||
				    region->start + region->size >
				    UINTPTR_MAX - (alignment - 1U))
					return ENOMEM;
				start = (region->start + region->size + alignment - 1U) &
					~(uintptr_t)(alignment - 1U);
				moved = 1;
				break;
			}
		}

		if (!moved)
			break;
	}

	*mapped = start;
	return 0;
}

/*
 * Finds a free aligned range at or above a hint in the mmap area; the caller
 * holds the VM lock.
 */
int
vmspace_find_free_range_locked(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	size_t alignment,
	uintptr_t *mapped)
{
	int error;

	/* Clamps the hint into the mapping area of the layout. */
	vmspace_layout_init();
	if (hint < vm_layout.mmap_base)
		hint = vm_layout.mmap_base;
	if (hint >= vm_layout.user_limit)
		return ENOMEM;

	/* Searches from the hint up to the end of the user area. */

	/* Reports why the search failed. */
	error = vmspace_find_free_range_bounded_locked(vm, hint, vm_layout.user_limit, size, alignment, mapped);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps anonymous memory at a free range near a hint; the caller holds the VM
 * locks.
 */
int
vmspace_map_find_locked(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	uintptr_t *mapped)
{
	uintptr_t start;
	int error;

	error = vmspace_find_free_range_locked(vm, hint, size, PAGE_SIZE, &start);
	if (error == 0)
		error = vmspace_map_anon_locked(vm, start,
		    (size + PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U), prot, NULL);
	if (error == 0)
		*mapped = start;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a private file region at a free range near a hint; the caller holds
 * the VM locks.
 */
int
vmspace_map_file_find_locked(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	struct vm_object *object,
	uintptr_t *mapped)
{
	uintptr_t start;
	size_t rounded;
	int error;

	/* Finds a free range large enough for the mapping. */
	error = vmspace_find_free_range_locked(vm, hint, size, PAGE_SIZE, &start);
	if (error != 0)
		return error;

	/* Maps the file there, clamping the data to the mapping. */
	rounded = (size + PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);

	if (data_size > size)
		data_size = size;

	/* Maps the region, which takes the object reference. */
	error = vmspace_map_file_locked(vm, start, rounded, prot, file, offset, start, data_size, object, NULL);
	if (error == 0)
		*mapped = start;

	/* Reports why the mapping failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a shared file region at a free range near a hint; the caller holds
 * the VM locks.
 */
int
vmspace_map_file_shared_find_locked(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	struct vm_object *object,
	uintptr_t *mapped)
{
	uintptr_t start;
	size_t rounded;
	int error;

	/* Finds a valid page-aligned destination before publishing a region. */
	error = vmspace_find_free_range_locked(vm, hint, size, PAGE_SIZE, &start);
	if (error != 0)
		return error;

	/* Bounds the file portion to the requested mapping length. */
	rounded = (size + PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	if (data_size > size)
		data_size = size;

	/* Publishes the prepared object only after address selection succeeds. */
	error = vmspace_map_file_shared_locked(
		vm, start, rounded, prot, file, offset, data_size, object, NULL);
	if (error == 0)
		*mapped = start;

	/* Reports the mapping outcome to the prepared-object owner. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
