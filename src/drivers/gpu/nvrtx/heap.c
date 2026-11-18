/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The range allocator of the nvrtx driver (the VRAM and the BAR1 window).
 *
 * The free runs are an array sorted by address.  A run is taken from the
 * top of the highest free run that holds it, so the VRAM fills from the top
 * down, away from the firmware's picture at the bottom; a freed run is
 * merged with the runs on either side.  Nothing here touches the hardware
 * or takes a lock: the caller serializes the calls.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-mem.h"

static int heap_carve(struct nvrtx_heap *heap, uint32_t index, uint64_t address, uint64_t size);
static void heap_remove(struct nvrtx_heap *heap, uint32_t index);
static int heap_insert(struct nvrtx_heap *heap, uint32_t index, uint64_t base, uint64_t size);

/*
 * Starts a range whose every address from base up to top is free.
 *
 * An empty or reversed range has no free run at all.
 */
void
nvrtx_heap_init(
	struct nvrtx_heap *heap,
	uint64_t base,
	uint64_t top)
{
	/* Records the range's bounds. */
	heap->base = base;
	heap->top = top;
	heap->count = 0U;
	heap->free_bytes = 0U;

	/* An empty range keeps no run. */
	if (top <= base)
		return;

	/* The whole range is one free run. */
	heap->free[0].base = base;
	heap->free[0].size = top - base;
	heap->count = 1U;
	heap->free_bytes = top - base;
}

/*
 * Takes a run of size bytes on an align boundary, as high as one fits.
 *
 * align is a power of two (0 means 1).  Returns EINVAL for an empty size
 * or an align that is not a power of two, ENOSPC when no free run holds the
 * run or the run would split a free run once more than the array holds.
 */
int
nvrtx_heap_alloc(
	struct nvrtx_heap *heap,
	uint64_t size,
	uint64_t align,
	uint64_t *address)
{
	struct nvrtx_extent *extent;
	uint64_t end;
	uint64_t candidate;
	uint32_t index;
	int error;

	/* A run has bytes, and its boundary is a power of two. */
	if (align == 0U)
		align = 1U;
	if (size == 0U)
		return EINVAL;
	if ((align & (align - 1U)) != 0U)
		return EINVAL;

	/* Walks the free runs from the highest down; the first that holds the run gives its top. */
	for (index = heap->count; index > 0U; index--) {
		extent = &heap->free[index - 1U];
		if (size > extent->size)
			continue;

		/* The run ends at the free run's end, moved down to the boundary. */
		end = extent->base + extent->size;
		candidate = (end - size) & ~(align - 1U);
		if (candidate < extent->base)
			continue;

		/* Takes the run out of the free run. */
		error = heap_carve(heap, index - 1U, candidate, size);
		if (error != 0)
			return error;

		/* Succeeded: the run is the caller's. */
		*address = candidate;
		return 0;
	}

	/* No free run holds it. */
	return ENOSPC;
}

/*
 * Takes a run at a fixed address, which must be wholly free.
 *
 * Returns EINVAL for an empty or wrapping run, EBUSY when any of it is not
 * free, or ENOSPC when it would split a free run once more than the array
 * holds.
 */
int
nvrtx_heap_reserve(
	struct nvrtx_heap *heap,
	uint64_t address,
	uint64_t size)
{
	struct nvrtx_extent *extent;
	uint32_t index;
	int error;

	/* A run has bytes and does not wrap. */
	if (size == 0U || address + size < address)
		return EINVAL;

	/* Finds the free run that holds the whole of it. */
	for (index = 0U; index < heap->count; index++) {
		extent = &heap->free[index];
		if (address < extent->base)
			continue;
		if (address + size > extent->base + extent->size)
			continue;

		/* Takes the run out of that free run. */
		error = heap_carve(heap, index, address, size);
		if (error != 0)
			return error;

		/* Succeeded: the run is the caller's. */
		return 0;
	}

	/* Some of it is taken, or it lies outside the range. */
	return EBUSY;
}

/*
 * Gives a run back, merging it with the free runs it touches.
 *
 * Returns EINVAL for a run that is empty, outside the range or overlaps a
 * free run (a double free), or ENOSPC when it would be one hole more than the
 * array holds (the run then stays taken).
 */
int
nvrtx_heap_free(
	struct nvrtx_heap *heap,
	uint64_t address,
	uint64_t size)
{
	struct nvrtx_extent *before;
	struct nvrtx_extent *after;
	uint32_t index;
	int joins_before;
	int joins_after;
	int error;

	/* The run lies inside the range. */
	if (size == 0U || address + size < address)
		return EINVAL;
	if (address < heap->base || address + size > heap->top)
		return EINVAL;

	/* Finds the first free run above the run. */
	for (index = 0U; index < heap->count; index++) {
		if (heap->free[index].base > address)
			break;
	}

	/*
	 * The free runs on either side must not overlap it; when one ends
	 * where it starts, or starts where it ends, the run joins it.
	 */
	before = NULL;
	after = NULL;
	joins_before = 0;
	joins_after = 0;
	if (index > 0U) {
		before = &heap->free[index - 1U];
		if (before->base + before->size > address)
			return EINVAL;
		if (before->base + before->size == address)
			joins_before = 1;
	}

	/* The same for the free run above. */
	if (index < heap->count) {
		after = &heap->free[index];
		if (address + size > after->base)
			return EINVAL;
		if (address + size == after->base)
			joins_after = 1;
	}

	/* Merges the run into its neighbours, or adds it as a run of its own. */
	if (joins_before && joins_after) {
		before->size += size + after->size;
		heap_remove(heap, index);
	} else if (joins_before) {
		before->size += size;
	} else if (joins_after) {
		after->base = address;
		after->size += size;
	} else {
		error = heap_insert(heap, index, address, size);
		if (error != 0)
			return error;
	}

	/* Succeeded: the run is free again. */
	heap->free_bytes += size;
	return 0;
}

/* Takes a run out of the free run at index, which holds it, splitting the free run when the run is in its middle. */
static int
heap_carve(
	struct nvrtx_heap *heap,
	uint32_t index,
	uint64_t address,
	uint64_t size)
{
	struct nvrtx_extent *extent;
	uint64_t head;
	uint64_t tail;
	int error;

	/* What stays free below and above the run. */
	extent = &heap->free[index];
	head = address - extent->base;
	tail = extent->base + extent->size - (address + size);

	/* The free run goes, shrinks from one end, or splits in two. */
	if (head == 0U && tail == 0U) {
		heap_remove(heap, index);
	} else if (head == 0U) {
		extent->base += size;
		extent->size -= size;
	} else if (tail == 0U) {
		extent->size = head;
	} else {
		error = heap_insert(heap, index + 1U, address + size, tail);
		if (error != 0)
			return error;
		heap->free[index].size = head;
	}

	/* Succeeded: the run is no longer free. */
	heap->free_bytes -= size;
	return 0;
}

/* Removes the free run at index, moving the higher ones down. */
static void
heap_remove(
	struct nvrtx_heap *heap,
	uint32_t index)
{
	uint32_t at;

	/* Moves each higher run down one place. */
	for (at = index; at + 1U < heap->count; at++)
		heap->free[at] = heap->free[at + 1U];

	heap->count--;
}

/* Inserts a free run at index, moving the higher ones up; ENOSPC when the array is full. */
static int
heap_insert(
	struct nvrtx_heap *heap,
	uint32_t index,
	uint64_t base,
	uint64_t size)
{
	uint32_t at;

	/* The array holds a bounded number of holes. */
	if (heap->count >= NVRTX_HEAP_EXTENTS_MAX)
		return ENOSPC;

	/* Moves each higher run up one place. */
	for (at = heap->count; at > index; at--)
		heap->free[at] = heap->free[at - 1U];

	/* Fills the opened place. */
	heap->free[index].base = base;
	heap->free[index].size = size;
	heap->count++;

	/* Succeeded: the run is in the array. */
	return 0;
}
