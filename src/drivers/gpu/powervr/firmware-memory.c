/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The memory the PowerVR firmware runs in.
 *
 * The core does not snoop the processor's caches, so every block the host
 * shares with the firmware is reached by the host through an uncached
 * alias of its run; the cached direct map of the run is never touched
 * while the alias exists.  The firmware's processor reaches the same run
 * through its page table, and the physical addresses it is handed are
 * those of the run, never of the alias.
 */

#include "drivers/gpu/powervr/powervr-private.h"
#include "drivers/gpu/powervr/mips-table.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>

static int make_run(struct powervr_heap *heap, uint32_t size, struct powervr_object *object);
static void drop_run(struct powervr_object *object);
static void clear_view(volatile uint8_t *view, uint32_t size);
static int place_block(struct powervr_heap *heap, uint32_t size, uint32_t heap_offset, uint32_t *placed);
static void release_place(struct powervr_heap *heap, uint32_t heap_offset, uint32_t size);

/*
 * Sets up the heap and its empty page table.
 */
int
powervr_heap_init(
	struct powervr_heap *heap,
	unsigned bus_bits,
	uint64_t dma_limit)
{
	int error;

	/* Starts with no table and nothing placed. */
	kern_memset(heap, 0, sizeof(*heap));
	heap->bus_bits = bus_bits;
	heap->dma_limit = dma_limit;

	/* Makes the table: one contiguous run of its four pages, cleared, so no page is valid. */
	error = make_run(heap, POWERVR_MIPS_TABLE_BYTES, &heap->table);
	if (error != 0)
		return error;

	/* Succeeded: the heap is empty and its table is ready to be named at boot. */
	return 0;
}

/*
 * Releases the page table; every block must be gone first.
 */
void
powervr_heap_fini(
	struct powervr_heap *heap)
{
	/* Returns the table's run. */
	drop_run(&heap->table);
}

/*
 * Makes a zeroed block of at least size bytes and maps it, at heap_offset
 * or anywhere.
 */
int
powervr_object_create(
	struct powervr_heap *heap,
	uint32_t size,
	uint32_t heap_offset,
	enum powervr_mips_caching caching,
	struct powervr_object *object)
{
	uint32_t rounded;
	uint32_t placed;
	int error;

	/* Refuses an empty block, and rounds the rest to whole pages. */
	if (size == 0U || size > POWERVR_HEAP_BYTES)
		return EINVAL;
	rounded = (size + POWERVR_MIPS_PAGE_BYTES - 1U) & ~(POWERVR_MIPS_PAGE_BYTES - 1U);

	/* Finds the block's place in the heap, and holds it. */
	error = place_block(heap, rounded, heap_offset, &placed);
	if (error != 0)
		return error;

	/* Takes a contiguous run the core can reach, and its cleared uncached view. */
	error = make_run(heap, rounded, object);
	if (error != 0) {
		release_place(heap, placed, rounded);
		return error;
	}

	/* Maps the run at its place; the processor caches only what the caller allows. */
	error = powervr_mips_map((volatile uint32_t *)heap->table.view, placed, object->run.paddr, rounded, caching, heap->bus_bits);
	if (error != 0) {
		drop_run(object);
		release_place(heap, placed, rounded);
		return error;
	}

	/* Succeeded: the block is mapped and zeroed. */
	object->heap_offset = placed;
	return 0;
}

/*
 * Unmaps a block and returns its memory; a block that was never made is
 * left alone.
 */
void
powervr_object_destroy(
	struct powervr_heap *heap,
	struct powervr_object *object)
{
	uint32_t heap_offset;
	uint32_t size;

	/* A block with no view holds nothing. */
	if (object->view == NULL)
		return;

	/* Removes the block from the processor's table, returns the run, then frees its place. */
	heap_offset = object->heap_offset;
	size = object->size;
	powervr_mips_unmap((volatile uint32_t *)heap->table.view, heap_offset, size);
	drop_run(object);
	release_place(heap, heap_offset, size);
}

/*
 * Returns a block's memory without touching the page table it was mapped
 * in, for a block outliving its heap: the firmware is stopped and the
 * table gone or made anew.
 */
void
powervr_object_drop(
	struct powervr_object *object)
{
	/* Ends the view and returns the run; a block with no view holds nothing. */
	drop_run(object);
}

/*
 * Reports the firmware processor's address of a byte of a block.
 */
uint32_t
powervr_object_address(
	const struct powervr_object *object,
	uint32_t offset)
{
	uint32_t address;

	/* The heap offset of the byte, in the processor's window. */
	address = powervr_mips_address(object->heap_offset + offset);

	/* Succeeded: the processor's address. */
	return address;
}

/*
 * Reports the physical address of a byte of a block.
 */
uint64_t
powervr_object_physical(
	const struct powervr_object *object,
	uint32_t offset)
{
	/* The run is contiguous, so the byte lies at the same offset in it. */
	return (uint64_t)object->run.paddr + offset;
}

/*
 * Stores a 32-bit field into a block.
 */
void
powervr_object_put32(
	struct powervr_object *object,
	uint32_t offset,
	uint32_t value)
{
	/* One aligned plain store through the uncached view. */
	*(volatile uint32_t *)(object->view + offset) = value;
}

/*
 * Stores a 64-bit field into a block.
 */
void
powervr_object_put64(
	struct powervr_object *object,
	uint32_t offset,
	uint64_t value)
{
	/* One aligned plain store through the uncached view. */
	*(volatile uint64_t *)(object->view + offset) = value;
}

/*
 * Loads a 32-bit field from a block.
 */
uint32_t
powervr_object_get32(
	const struct powervr_object *object,
	uint32_t offset)
{
	uint32_t value;

	/* One aligned plain load through the uncached view. */
	value = *(const volatile uint32_t *)(object->view + offset);

	/* Succeeded: the field as the firmware left it. */
	return value;
}

/*
 * Copies bytes of the kernel's into a block.
 */
void
powervr_object_fill(
	struct powervr_object *object,
	uint32_t offset,
	const uint8_t *bytes,
	size_t length)
{
	size_t index;

	/* Byte stores only: the alias takes no wider access an unaligned source would need. */
	for (index = 0; index < length; index++)
		object->view[offset + index] = bytes[index];
}

/* Takes a contiguous run below the core's limit and its cleared uncached view. */
static int
make_run(
	struct powervr_heap *heap,
	uint32_t size,
	struct powervr_object *object)
{
	void *view;
	int error;

	/* Starts empty, so a failure leaves a block that holds nothing. */
	kern_memset(object, 0, sizeof(*object));

	/* Takes the run, page aligned, where the core can name every byte. */
	error = kern_pmem_alloc_limited(size, POWERVR_MIPS_PAGE_BYTES, heap->dma_limit, 0, &object->run);
	if (error != 0)
		return error;

	/* Makes the uncached view; the run's cached lines are pushed out first. */
	error = kern_pmem_map_uncached(&object->run, &view);
	if (error != 0) {
		(void)kern_pmem_free(&object->run);
		kern_memset(object, 0, sizeof(*object));
		return error;
	}

	/* Clears the run through the view, so the firmware never reads stale bytes. */
	object->view = view;
	object->size = size;
	clear_view(object->view, size);

	/* Succeeded: the run is the caller's. */
	return 0;
}

/* Ends a run's view and returns the run. */
static void
drop_run(
	struct powervr_object *object)
{
	/* A block with no view holds nothing. */
	if (object->view == NULL)
		return;

	/* Ends the view, which drops the run's lines again, then frees the run. */
	(void)kern_pmem_unmap_uncached((void *)object->view, object->size);
	(void)kern_pmem_free(&object->run);
	kern_memset(object, 0, sizeof(*object));
}

/* Clears a view a word at a time. */
static void
clear_view(
	volatile uint8_t *view,
	uint32_t size)
{
	volatile uint64_t *words;
	uint32_t index;

	/* The view is page aligned, so whole words cover it. */
	words = (volatile uint64_t *)view;
	for (index = 0; index < size / 8U; index++)
		words[index] = 0;
}

/*
 * Finds a block's place: the one asked for, or the first free run of the
 * general part; a place in the general part is held until released.
 */
static int
place_block(
	struct powervr_heap *heap,
	uint32_t size,
	uint32_t heap_offset,
	uint32_t *placed)
{
	uint32_t first;
	uint32_t pages;
	int error;

	/* A block anywhere takes the first run of the general part that fits. */
	pages = size / POWERVR_MIPS_PAGE_BYTES;
	if (heap_offset == POWERVR_HEAP_ANYWHERE) {
		error = powervr_heap_pages_take(heap->pages, pages, &first);
		if (error != 0)
			return error;
		*placed = first * POWERVR_MIPS_PAGE_BYTES;
		return 0;
	}

	/* Refuses a place that is not a page or runs past the heap. */
	if ((heap_offset % POWERVR_MIPS_PAGE_BYTES) != 0U)
		return EINVAL;
	if (heap_offset >= POWERVR_HEAP_BYTES || size > POWERVR_HEAP_BYTES - heap_offset)
		return EINVAL;

	/* A place in the general part must be free, and is held from here. */
	if (heap_offset < POWERVR_HEAP_GENERAL_BYTES) {
		if (size > POWERVR_HEAP_GENERAL_BYTES - heap_offset)
			return EINVAL;
		error = powervr_heap_pages_claim(heap->pages, heap_offset / POWERVR_MIPS_PAGE_BYTES, pages);
		if (error != 0)
			return error;
	}

	/* Succeeded: the block goes where it was asked to. */
	*placed = heap_offset;
	return 0;
}

/* Frees a block's place in the general part; places above it were never held. */
static void
release_place(
	struct powervr_heap *heap,
	uint32_t heap_offset,
	uint32_t size)
{
	/* Only the general part is tracked. */
	if (heap_offset >= POWERVR_HEAP_GENERAL_BYTES)
		return;

	/* Gives back the place's pages. */
	powervr_heap_pages_give(heap->pages, heap_offset / POWERVR_MIPS_PAGE_BYTES, size / POWERVR_MIPS_PAGE_BYTES);
}
