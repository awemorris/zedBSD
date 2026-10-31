/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The garbage-collected heap: cells in 64 KiB blocks of one size each, or
 * in an allocation of their own when large; non-moving mark and sweep.
 *
 * A collection marks from the atoms, the registered root slots and
 * tracers, and every word of the C stack between the collector and the
 * stack base that points into a live cell; then it traces the marked cells
 * through their types and sweeps the unmarked ones onto the free lists.
 * Because cells never move, a pointer the collector only guessed at (a
 * stack word that merely looks like a pointer) costs nothing but a cell
 * kept a little longer.
 */

#include "vm/internal.h"
#include "base/os-system.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The least number of bytes allocated between two collections. */
#define HEAP_MIN_THRESHOLD	(8U * 1024U * 1024U)

/* The room kept before a large cell for its header, a multiple of the cell alignment. */
#define HEAP_LARGE_HEADER	32U

/* The size of the block-address set when the first block arrives. */
#define HEAP_BLOCK_SET_MIN	64U

/*
 * The sizes of the small cells, smallest first.
 *
 * Every size is a multiple of the cell alignment.  The steps widen as the
 * sizes grow so a cell wastes at most about a quarter of its size.
 */
static const uint32_t heap_class_sizes[VM_SIZE_CLASSES] = {
	16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256,
	320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 2048, 3072, 4096
};

static int heap_size_class(size_t size, unsigned *size_class);
static int heap_add_block(struct vm_heap *heap, unsigned size_class);
static int heap_block_set_insert(struct vm_heap *heap, struct vm_block *block);
static struct vm_block *heap_block_of(const struct vm_heap *heap, uintptr_t word);
static int heap_rebuild_block_set(struct vm_heap *heap);
static void *heap_alloc_large(struct vm_heap *heap, size_t size);
static struct vm_large *heap_large_of(const struct vm_heap *heap, uintptr_t word);
static struct vm_cell *heap_large_cell(struct vm_large *large);
static void heap_note_range(struct vm_heap *heap, uintptr_t start, uintptr_t end);
static void heap_mark_roots(struct vm_heap *heap);
static void heap_scan_stack(struct vm_heap *heap) __attribute__((noinline));
static void heap_scan_range(struct vm_heap *heap, const unsigned char *low, const unsigned char *high);
static void heap_drain(struct vm_heap *heap);
static void heap_sweep(struct vm_heap *heap);
static void heap_sweep_blocks(struct vm_heap *heap);
static void heap_sweep_large(struct vm_heap *heap);
static int heap_bit(const uint8_t *bits, uint32_t index);
static void heap_set_bit(uint8_t *bits, uint32_t index);
static void heap_clear_bit(uint8_t *bits, uint32_t index);
static unsigned char *heap_block_cell(struct vm_block *block, uint32_t index);

/*
 * Creates an empty heap.
 *
 * limit caps the bytes the live cells may take; an allocation that would
 * pass it after a collection fails.  Zero means no limit.
 */
int
vm_heap_create(
	struct vm_heap **heap,
	size_t limit)
{
	struct vm_heap *created;

	/* Allocates the heap's record. */
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;

	/* Starts with no cells, the smallest threshold and empty root lists. */
	created->threshold = HEAP_MIN_THRESHOLD;
	created->limit = limit;
	created->lowest = (uintptr_t)-1;
	created->highest = 0;
	wb_vector_init(&created->roots, sizeof(struct vm_cell **));
	wb_vector_init(&created->tracers, sizeof(struct vm_tracer_entry));
	wb_vector_init(&created->mark_stack, sizeof(struct vm_cell *));

	/* Succeeded: the heap is ready for its first cell. */
	*heap = created;
	return 0;
}

/*
 * Destroys a heap and every cell in it, finalizing each.
 */
void
vm_heap_destroy(
	struct vm_heap *heap)
{
	struct vm_block *block;
	struct vm_block *next;
	struct vm_cell *cell;
	uint32_t index;
	size_t item;
	int in_use;

	/* A NULL heap is nothing to destroy. */
	if (heap == NULL)
		return;

	/* Finalizes and frees the blocks of small cells. */
	block = heap->blocks;
	while (block != NULL) {
		next = block->next;
		for (index = 0; index < block->cell_count; index++) {
			/* Only a cell in use has anything to finalize. */
			in_use = heap_bit(block->allocated, index);
			if (!in_use)
				continue;

			/* Lets the cell's type free what it owns. */
			cell = (struct vm_cell *)heap_block_cell(block, index);
			if (cell->type != NULL && cell->type->finalize != NULL)
				cell->type->finalize(heap, cell);
		}

		/* Returns the block to the system. */
		wb_os_aligned_free(block);
		block = next;
	}

	/* Finalizes and frees the large cells. */
	for (item = 0; item < heap->large_count; item++) {
		cell = heap_large_cell(heap->large[item]);
		if (cell->type != NULL && cell->type->finalize != NULL)
			cell->type->finalize(heap, cell);

		/* Returns the cell's allocation to the system. */
		wb_os_aligned_free(heap->large[item]);
	}

	/* Frees the tables and the record. */
	vm_atom_table_release(heap);
	free(heap->large);
	free(heap->block_set);
	wb_vector_release(&heap->roots);
	wb_vector_release(&heap->tracers);
	wb_vector_release(&heap->mark_stack);
	free(heap);
}

/*
 * Records where the C stack the collector scans ends.
 *
 * base is the frame address (__builtin_frame_address(0)) of the outermost
 * function that holds cells: main, or a thread's start function.  The
 * address of one of that function's variables is not enough, because the
 * compiler may place its other variables above it.  Without a base the
 * stack is not scanned, and only roots and tracers keep cells alive.
 */
void
vm_heap_set_stack_base(
	struct vm_heap *heap,
	const void *base)
{
	/* Remembers the base for every later collection. */
	heap->stack_base = base;
}

/*
 * Allocates a cell of size bytes of the given type, cleared to zero.
 *
 * May collect first.  Returns NULL when memory runs out or the live bytes
 * would pass the heap's limit.  size includes the cell header.
 */
void *
vm_heap_alloc(
	struct vm_heap *heap,
	const struct vm_cell_type *type,
	size_t size)
{
	struct vm_block *block;
	struct vm_cell *cell;
	unsigned size_class;
	void **free_list;
	uint32_t index;
	int small;
	int error;

	assert(!heap->collecting);
	assert(size >= sizeof(struct vm_cell));

	/* Collects when enough has been allocated since the last collection. */
	if (heap->allocated_since >= heap->threshold)
		vm_heap_collect(heap);

	/* Refuses a cell that would take the live bytes past the limit. */
	if (heap->limit != 0 && heap->live_bytes + size > heap->limit) {
		vm_heap_collect(heap);
		if (heap->live_bytes + size > heap->limit)
			return NULL;
	}

	/* A large cell gets an allocation of its own. */
	small = heap_size_class(size, &size_class);
	if (!small) {
		cell = heap_alloc_large(heap, size);
		if (cell == NULL)
			return NULL;

		/* Succeeded: the large cell has its type. */
		cell->type = type;
		return cell;
	}

	/* Takes a new block when the size class has no free cell. */
	free_list = &heap->free_lists[size_class];
	if (*free_list == NULL) {
		error = heap_add_block(heap, size_class);
		if (error != 0)
			return NULL;
	}

	/* Unlinks the first free cell and marks it in use in its block. */
	cell = *free_list;
	*free_list = *(void **)cell;
	block = heap_block_of(heap, (uintptr_t)cell);
	index = (uint32_t)(((unsigned char *)cell - (unsigned char *)block - block->first_offset) / block->cell_size);
	heap_set_bit(block->allocated, index);
	block->used++;

	/* Clears the cell and gives it its type. */
	memset(cell, 0, heap_class_sizes[size_class]);
	cell->type = type;

	/* Counts the cell for the collector's thresholds. */
	heap->live_bytes += heap_class_sizes[size_class];
	heap->live_cells++;
	heap->allocated_since += heap_class_sizes[size_class];

	/* Succeeded: the cell lives while something reaches it. */
	return cell;
}

/*
 * Collects the heap: marks everything reachable and frees the rest.
 */
void
vm_heap_collect(
	struct vm_heap *heap)
{
	/* A collection never nests (a finalizer or tracer cannot start one). */
	if (heap->collecting)
		return;
	heap->collecting = 1;

	/*
	 * Spills the callee-saved registers into this frame, so a cell held only
	 * in a register is on the stack the scan below walks.
	 */
	__builtin_unwind_init();

	/* Marks from the roots, the tracers and the stack, and follows the cells marked. */
	heap_mark_roots(heap);
	heap_scan_stack(heap);
	heap_drain(heap);

	/* Frees what was not marked and sets the next threshold. */
	heap_sweep(heap);
	heap->threshold = heap->live_bytes;
	if (heap->threshold < HEAP_MIN_THRESHOLD)
		heap->threshold = HEAP_MIN_THRESHOLD;
	heap->allocated_since = 0;
	heap->collections++;

	/* The heap may allocate again. */
	heap->collecting = 0;
}

/*
 * Marks a cell reachable, to be traced before the collection ends.
 *
 * Only a trace function or a tracer calls this, during a collection.
 */
void
vm_heap_mark(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_block *block;
	struct vm_large *large;
	uint32_t index;
	int marked;
	int error;

	/* A missing reference marks nothing. */
	if (cell == NULL)
		return;

	/* Finds the cell's mark and stops when it is already set. */
	block = heap_block_of(heap, (uintptr_t)cell);
	if (block != NULL) {
		index = (uint32_t)(((unsigned char *)cell - (unsigned char *)block - block->first_offset) / block->cell_size);
		marked = heap_bit(block->marked, index);
		if (marked)
			return;

		/* The mark says the cell is reached and queued; it is traced once. */
		heap_set_bit(block->marked, index);
	} else {
		/* Only a registered large allocation is a GC cell; manual realms are external. */
		large = heap_large_of(heap, (uintptr_t)cell);
		if (large == NULL || heap_large_cell(large) != cell)
			return;
		if (large->marked)
			return;

		/* The mark says the cell is reached and queued; it is traced once. */
		large->marked = 1;
	}

	/* Queues the cell for tracing; a collector without memory cannot go on. */
	error = wb_vector_push(&heap->mark_stack, &cell);
	if (error != 0) {
		fprintf(stderr, "browser: out of memory while collecting\n");
		abort();
	}
}

/*
 * Marks the cell a word points into, if it points into a live cell.
 *
 * The stack scan uses this, and so can a subsystem that holds cells in
 * words it cannot tell from numbers.
 */
void
vm_heap_mark_word(
	struct vm_heap *heap,
	uintptr_t word)
{
	struct vm_cell *cell;

	/* Finds the cell, if any, and marks it. */
	cell = vm_heap_find_cell(heap, word);
	if (cell != NULL)
		vm_heap_mark(heap, cell);
}

/*
 * Registers a slot whose cell every collection keeps alive.
 *
 * The slot is read at each collection, so it may change between them.
 */
int
vm_heap_add_root(
	struct vm_heap *heap,
	struct vm_cell **slot)
{
	int error;

	/* Adds the slot to the list. */
	error = wb_vector_push(&heap->roots, &slot);
	if (error != 0)
		return error;

	/* Succeeded: the slot is a root until removed. */
	return 0;
}

/*
 * Stops treating a slot as a root.
 */
void
vm_heap_remove_root(
	struct vm_heap *heap,
	struct vm_cell **slot)
{
	struct vm_cell ***slots;
	size_t index;

	/* Finds the slot and moves the last one into its place. */
	slots = heap->roots.items;
	for (index = 0; index < heap->roots.count; index++) {
		/* Keeps looking past other slots. */
		if (slots[index] != slot)
			continue;

		/* Closes the gap with the last slot. */
		slots[index] = slots[heap->roots.count - 1U];
		wb_vector_pop(&heap->roots);
		return;
	}
}

/*
 * Registers a function that marks the cells a subsystem holds outside the
 * heap; it is called with context at every collection.
 */
int
vm_heap_add_tracer(
	struct vm_heap *heap,
	vm_tracer tracer,
	void *context)
{
	struct vm_tracer_entry entry;
	int error;

	/* Adds the tracer to the list. */
	entry.tracer = tracer;
	entry.context = context;
	error = wb_vector_push(&heap->tracers, &entry);
	if (error != 0)
		return error;

	/* Succeeded: the tracer runs until removed. */
	return 0;
}

/*
 * Removes a tracer registered with the same function and context.
 */
void
vm_heap_remove_tracer(
	struct vm_heap *heap,
	vm_tracer tracer,
	void *context)
{
	struct vm_tracer_entry *entries;
	size_t index;

	/* Finds the tracer and moves the last one into its place. */
	entries = heap->tracers.items;
	for (index = 0; index < heap->tracers.count; index++) {
		/* Keeps looking past other tracers. */
		if (entries[index].tracer != tracer || entries[index].context != context)
			continue;

		/* Closes the gap with the last tracer. */
		entries[index] = entries[heap->tracers.count - 1U];
		wb_vector_pop(&heap->tracers);
		return;
	}
}

/*
 * Finds the live cell a word points into (its start or any byte inside).
 *
 * Returns NULL when the word points at no cell in use.
 */
struct vm_cell *
vm_heap_find_cell(
	struct vm_heap *heap,
	uintptr_t word)
{
	struct vm_block *block;
	struct vm_large *large;
	uintptr_t offset;
	uint32_t index;
	int in_use;

	/* A word outside every cell's range is rejected at once. */
	if (word < heap->lowest || word >= heap->highest)
		return NULL;

	/* A word inside a block names the cell at its offset, if that cell is in use. */
	block = heap_block_of(heap, word);
	if (block != NULL) {
		offset = word - (uintptr_t)block;
		if (offset < block->first_offset)
			return NULL;

		/* The cell at that offset must exist and be in use. */
		index = (uint32_t)((offset - block->first_offset) / block->cell_size);
		if (index >= block->cell_count)
			return NULL;
		in_use = heap_bit(block->allocated, index);
		if (!in_use)
			return NULL;

		/* Succeeded: the word is inside this small cell. */
		return (struct vm_cell *)heap_block_cell(block, index);
	}

	/* Otherwise the word may point into a large cell. */
	large = heap_large_of(heap, word);
	if (large == NULL)
		return NULL;

	/* Succeeded: the word is inside a large cell. */
	return heap_large_cell(large);
}

/*
 * Reports the heap's counts.
 */
void
vm_heap_stats(
	const struct vm_heap *heap,
	struct vm_heap_stats *stats)
{
	/* Copies the counts the heap keeps. */
	stats->live_bytes = heap->live_bytes;
	stats->live_cells = heap->live_cells;
	stats->heap_bytes = heap->block_count * (size_t)VM_BLOCK_SIZE;
	stats->collections = heap->collections;
	stats->freed_cells = heap->freed_cells;
}

/* Finds the smallest size class that holds size bytes; returns 0 when the cell is large. */
static int
heap_size_class(
	size_t size,
	unsigned *size_class)
{
	unsigned index;

	/* A size past the largest class is a large cell. */
	if (size > VM_SMALL_MAX)
		return 0;

	/* Walks the classes from the smallest. */
	for (index = 0; index < VM_SIZE_CLASSES; index++) {
		/* Stops at the first class big enough. */
		if (heap_class_sizes[index] >= size) {
			*size_class = index;
			return 1;
		}
	}

	/* The largest class is VM_SMALL_MAX, so this is not reached. */
	return 0;
}

/* Adds a block for a size class and puts all its cells on the class's free list. */
static int
heap_add_block(
	struct vm_heap *heap,
	unsigned size_class)
{
	struct vm_block *block;
	void *memory;
	uint32_t index;
	int error;

	/* Takes an aligned block, so a cell's block is its address rounded down. */
	error = wb_os_aligned_alloc(VM_BLOCK_SIZE, VM_BLOCK_SIZE, &memory);
	if (error != 0)
		return ENOMEM;
	block = memory;

	/* Describes the block's cells. */
	memset(block, 0, sizeof(*block));
	block->cell_size = heap_class_sizes[size_class];
	block->first_offset = (uint32_t)((sizeof(*block) + VM_CELL_ALIGN - 1U) & ~(size_t)(VM_CELL_ALIGN - 1U));
	block->cell_count = (VM_BLOCK_SIZE - block->first_offset) / block->cell_size;
	block->size_class = size_class;

	/* Makes the block findable from a stack word. */
	error = heap_block_set_insert(heap, block);
	if (error != 0) {
		wb_os_aligned_free(block);
		return error;
	}

	/* Links the block and widens the range of cell addresses. */
	block->next = heap->blocks;
	heap->blocks = block;
	heap->block_count++;
	heap_note_range(heap, (uintptr_t)block, (uintptr_t)block + VM_BLOCK_SIZE);

	/* Chains the cells onto the free list, the lowest address first. */
	index = block->cell_count;
	while (index > 0) {
		index--;
		*(void **)heap_block_cell(block, index) = heap->free_lists[size_class];
		heap->free_lists[size_class] = heap_block_cell(block, index);
	}

	/* Succeeded: the class has free cells. */
	return 0;
}

/* Adds a block to the set of block addresses, growing the set when it is half full. */
static int
heap_block_set_insert(
	struct vm_heap *heap,
	struct vm_block *block)
{
	struct vm_block **old_set;
	size_t old_capacity;
	size_t capacity;
	size_t slot;
	size_t index;
	int error;

	/* Grows the set when adding would fill more than half of it. */
	if ((heap->block_count + 1U) * 2U > heap->block_set_capacity) {
		old_set = heap->block_set;
		old_capacity = heap->block_set_capacity;
		capacity = old_capacity * 2U;
		if (capacity < HEAP_BLOCK_SET_MIN)
			capacity = HEAP_BLOCK_SET_MIN;

		/* Takes the larger, empty set; the old one stays in use if that fails. */
		heap->block_set = calloc(capacity, sizeof(*heap->block_set));
		if (heap->block_set == NULL) {
			heap->block_set = old_set;
			return ENOMEM;
		}

		/* Publishes the larger set's size. */
		heap->block_set_capacity = capacity;

		/* Moves the known blocks into the larger set. */
		for (index = 0; index < old_capacity; index++) {
			/* Empty slots carry nothing over. */
			if (old_set[index] == NULL)
				continue;

			/* Puts the block in the first free slot from its hash. */
			slot = ((uintptr_t)old_set[index] / VM_BLOCK_SIZE) & (capacity - 1U);
			while (heap->block_set[slot] != NULL)
				slot = (slot + 1U) & (capacity - 1U);
			heap->block_set[slot] = old_set[index];
		}

		/* Frees the old set. */
		free(old_set);
	}

	/* Puts the block in the first free slot from its hash. */
	slot = ((uintptr_t)block / VM_BLOCK_SIZE) & (heap->block_set_capacity - 1U);
	while (heap->block_set[slot] != NULL)
		slot = (slot + 1U) & (heap->block_set_capacity - 1U);
	heap->block_set[slot] = block;

	/* Succeeded: the block is findable. */
	error = 0;
	return error;
}

/* Finds the block a word points into, or NULL when the word is in no block. */
static struct vm_block *
heap_block_of(
	const struct vm_heap *heap,
	uintptr_t word)
{
	struct vm_block *candidate;
	size_t slot;

	/* A heap without blocks has none to find. */
	if (heap->block_set_capacity == 0)
		return NULL;

	/* Probes the set from the hash of the word's 64 KiB region. */
	candidate = (struct vm_block *)(word & ~(uintptr_t)(VM_BLOCK_SIZE - 1U));
	slot = (word / VM_BLOCK_SIZE) & (heap->block_set_capacity - 1U);
	while (heap->block_set[slot] != NULL) {
		/* The region is a block of this heap. */
		if (heap->block_set[slot] == candidate)
			return candidate;

		/* Moves to the next slot of the probe. */
		slot = (slot + 1U) & (heap->block_set_capacity - 1U);
	}

	/* The region is not one of the heap's blocks. */
	return NULL;
}

/* Rebuilds the block-address set from the list of blocks (after blocks were freed). */
static int
heap_rebuild_block_set(
	struct vm_heap *heap)
{
	struct vm_block *block;
	size_t count;
	int error;

	/* Empties the set and forgets the count, which the inserts rebuild. */
	if (heap->block_set != NULL)
		memset(heap->block_set, 0, heap->block_set_capacity * sizeof(*heap->block_set));
	count = heap->block_count;
	heap->block_count = 0;

	/* Inserts every block that is left. */
	for (block = heap->blocks; block != NULL; block = block->next) {
		error = heap_block_set_insert(heap, block);
		if (error != 0)
			return error;

		/* Counts the block again. */
		heap->block_count++;
	}

	/* Succeeded: the set matches the list again. */
	assert(heap->block_count == count);
	return 0;
}

/* Allocates a large cell in an allocation of its own and records it in address order. */
static void *
heap_alloc_large(
	struct vm_heap *heap,
	size_t size)
{
	struct vm_large **grown;
	struct vm_large *large;
	struct vm_cell *cell;
	size_t capacity;
	size_t position;
	void *memory;
	int error;

	/* Refuses a size whose header would overflow it. */
	if (size > (size_t)-1 - HEAP_LARGE_HEADER)
		return NULL;

	/* Makes room in the sorted list first, so nothing needs undoing after the allocation. */
	if (heap->large_count == heap->large_capacity) {
		capacity = heap->large_capacity * 2U;
		if (capacity < 16U)
			capacity = 16U;

		/* Moves the list to larger storage. */
		grown = realloc(heap->large, capacity * sizeof(*grown));
		if (grown == NULL)
			return NULL;

		/* Publishes the larger list. */
		heap->large = grown;
		heap->large_capacity = capacity;
	}

	/* Takes the memory, aligned for a cell after the header. */
	error = wb_os_aligned_alloc(VM_CELL_ALIGN, HEAP_LARGE_HEADER + size, &memory);
	if (error != 0)
		return NULL;
	large = memory;
	memset(large, 0, HEAP_LARGE_HEADER + size);
	large->size = size;
	cell = heap_large_cell(large);

	/* Inserts the cell in address order. */
	position = heap->large_count;
	while (position > 0 && heap->large[position - 1U] > large) {
		heap->large[position] = heap->large[position - 1U];
		position--;
	}

	/* Records the cell at its place. */
	heap->large[position] = large;
	heap->large_count++;

	/* Widens the range of cell addresses and counts the bytes. */
	heap_note_range(heap, (uintptr_t)cell, (uintptr_t)cell + size);
	heap->live_bytes += size;
	heap->live_cells++;
	heap->allocated_since += size;

	/* Succeeded: the large cell is zero apart from its type. */
	return cell;
}

/* Finds the large cell a word points into, by binary search over the sorted list. */
static struct vm_large *
heap_large_of(
	const struct vm_heap *heap,
	uintptr_t word)
{
	struct vm_large *large;
	uintptr_t start;
	size_t low;
	size_t high;
	size_t middle;

	/* Finds the last large cell that starts at or below the word. */
	low = 0;
	high = heap->large_count;
	while (low < high) {
		middle = low + (high - low) / 2U;
		start = (uintptr_t)heap_large_cell(heap->large[middle]);
		if (start <= word) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}

	/* No large cell starts at or below the word. */
	if (low == 0)
		return NULL;

	/* The word must lie inside that cell. */
	large = heap->large[low - 1U];
	start = (uintptr_t)heap_large_cell(large);
	if (word >= start + large->size)
		return NULL;

	/* Succeeded: the word is inside this large cell. */
	return large;
}

/* Finds the cell of a large allocation, after its header. */
static struct vm_cell *
heap_large_cell(
	struct vm_large *large)
{
	/* Reports the address after the header. */
	return (struct vm_cell *)((unsigned char *)large + HEAP_LARGE_HEADER);
}

/* Widens the range of addresses any cell may have. */
static void
heap_note_range(
	struct vm_heap *heap,
	uintptr_t start,
	uintptr_t end)
{
	/* Lowers the low end and raises the high end as needed. */
	if (start < heap->lowest)
		heap->lowest = start;
	if (end > heap->highest)
		heap->highest = end;
}

/* Clears the marks and marks from the atoms, the root slots and the tracers. */
static void
heap_mark_roots(
	struct vm_heap *heap)
{
	struct vm_tracer_entry *entries;
	struct vm_cell ***slots;
	struct vm_block *block;
	size_t index;

	/* Clears every mark left from the last collection. */
	for (block = heap->blocks; block != NULL; block = block->next)
		memset(block->marked, 0, sizeof(block->marked));
	for (index = 0; index < heap->large_count; index++)
		heap->large[index]->marked = 0;

	/* The atoms live as long as the heap. */
	vm_atom_table_trace(heap);

	/* Marks the cells in the root slots. */
	slots = heap->roots.items;
	for (index = 0; index < heap->roots.count; index++)
		vm_heap_mark(heap, *slots[index]);

	/* Lets each subsystem mark the cells it holds. */
	entries = heap->tracers.items;
	for (index = 0; index < heap->tracers.count; index++)
		entries[index].tracer(heap, entries[index].context);
}

/* Marks every cell a word of the C stack points into, from this frame to the stack base. */
static void
heap_scan_stack(
	struct vm_heap *heap)
{
	const unsigned char *here;
	const unsigned char *low;
	const unsigned char *high;

	/* Without a base there is no stack to scan. */
	if (heap->stack_base == NULL)
		return;

	/*
	 * The stack runs between this frame and the base, whichever way it grows.
	 * The frame address is taken rather than a local's, which a sanitizer may
	 * move to a stack of its own.
	 */
	here = __builtin_frame_address(0);
	low = here;
	high = heap->stack_base;
	if (low > high) {
		low = heap->stack_base;
		high = here;
	}

	/* Marks from each word in the range. */
	heap_scan_range(heap, low, high);
}

/*
 * Marks every cell a word in [low, high) points into.
 *
 * The words read are other functions' frames, which the address sanitizer
 * would call out of bounds; the scan is exempt from it.
 */
__attribute__((no_sanitize_address))
static void
heap_scan_range(
	struct vm_heap *heap,
	const unsigned char *low,
	const unsigned char *high)
{
	const unsigned char *cursor;
	uintptr_t word;

	/* Rounds the start up to a word boundary. */
	cursor = (const unsigned char *)(((uintptr_t)low + sizeof(uintptr_t) - 1U) & ~(uintptr_t)(sizeof(uintptr_t) - 1U));

	/* Reads every aligned word and marks the cell it points into. */
	while (cursor + sizeof(uintptr_t) <= high) {
		word = *(const uintptr_t *)cursor;
		vm_heap_mark_word(heap, word);
		cursor += sizeof(uintptr_t);
	}
}

/* Traces the marked cells until no cell waits. */
static void
heap_drain(
	struct vm_heap *heap)
{
	struct vm_cell *cell;

	/* Takes the most recently marked cell and lets its type mark what it refers to. */
	while (heap->mark_stack.count > 0) {
		cell = *(struct vm_cell **)wb_vector_at(&heap->mark_stack, heap->mark_stack.count - 1U);
		wb_vector_pop(&heap->mark_stack);
		if (cell->type != NULL && cell->type->trace != NULL)
			cell->type->trace(heap, cell);
	}
}

/* Frees every unmarked cell and recounts the live bytes. */
static void
heap_sweep(
	struct vm_heap *heap)
{
	/* Starts the counts from nothing; the sweeps add up what stays. */
	heap->live_bytes = 0;
	heap->live_cells = 0;

	/* Sweeps the small and the large cells. */
	heap_sweep_blocks(heap);
	heap_sweep_large(heap);
}

/* Sweeps the blocks: finalizes dead cells, frees empty blocks, rebuilds the free lists. */
static void
heap_sweep_blocks(
	struct vm_heap *heap)
{
	struct vm_block **link;
	struct vm_block *block;
	struct vm_cell *cell;
	uint32_t index;
	unsigned size_class;
	int freed_block;
	int in_use;
	int marked;
	int error;

	/* The free lists are rebuilt from the blocks that stay. */
	for (size_class = 0; size_class < VM_SIZE_CLASSES; size_class++)
		heap->free_lists[size_class] = NULL;

	/* Walks the blocks, keeping the link to each so an empty one can be unlinked. */
	freed_block = 0;
	link = &heap->blocks;
	while (*link != NULL) {
		block = *link;
		block->used = 0;

		/* Finalizes the cells in use that were not marked. */
		for (index = 0; index < block->cell_count; index++) {
			/* A free cell has nothing to finalize. */
			in_use = heap_bit(block->allocated, index);
			if (!in_use)
				continue;

			/* A marked cell stays. */
			marked = heap_bit(block->marked, index);
			if (marked) {
				block->used++;
				continue;
			}

			/* A dead cell is finalized and forgotten. */
			cell = (struct vm_cell *)heap_block_cell(block, index);
			if (cell->type != NULL && cell->type->finalize != NULL)
				cell->type->finalize(heap, cell);
			heap_clear_bit(block->allocated, index);
			heap->freed_cells++;
		}

		/* A block with no cell left goes back to the system. */
		if (block->used == 0) {
			*link = block->next;
			heap->block_count--;
			wb_os_aligned_free(block);
			freed_block = 1;
			continue;
		}

		/* Chains the block's free cells onto its class's list, the lowest address first. */
		index = block->cell_count;
		while (index > 0) {
			index--;
			in_use = heap_bit(block->allocated, index);
			if (in_use)
				continue;

			/* Pushes the free cell onto the list. */
			*(void **)heap_block_cell(block, index) = heap->free_lists[block->size_class];
			heap->free_lists[block->size_class] = heap_block_cell(block, index);
		}

		/* Counts what stays. */
		heap->live_bytes += (size_t)block->used * block->cell_size;
		heap->live_cells += block->used;
		link = &block->next;
	}

	/* A freed block must leave the address set; a set that cannot be rebuilt is fatal. */
	if (freed_block) {
		error = heap_rebuild_block_set(heap);
		if (error != 0) {
			fprintf(stderr, "browser: out of memory while collecting\n");
			abort();
		}
	}
}

/* Sweeps the large cells: finalizes and frees the unmarked ones and closes the gaps. */
static void
heap_sweep_large(
	struct vm_heap *heap)
{
	struct vm_large *large;
	struct vm_cell *cell;
	size_t kept;
	size_t index;

	/* Keeps the marked cells in order and frees the others. */
	kept = 0;
	for (index = 0; index < heap->large_count; index++) {
		large = heap->large[index];

		/* A marked cell stays in the list. */
		if (large->marked) {
			heap->large[kept] = large;
			kept++;
			heap->live_bytes += large->size;
			heap->live_cells++;
			continue;
		}

		/* A dead cell is finalized and freed. */
		cell = heap_large_cell(large);
		if (cell->type != NULL && cell->type->finalize != NULL)
			cell->type->finalize(heap, cell);
		wb_os_aligned_free(large);
		heap->freed_cells++;
	}

	/* Publishes the shorter list. */
	heap->large_count = kept;
}

/* Reads one bit of a block's bitmap. */
static int
heap_bit(
	const uint8_t *bits,
	uint32_t index)
{
	/* A set bit reads as one. */
	if ((bits[index / 8U] & (1U << (index % 8U))) != 0)
		return 1;

	/* A clear bit reads as zero. */
	return 0;
}

/* Sets one bit of a block's bitmap. */
static void
heap_set_bit(
	uint8_t *bits,
	uint32_t index)
{
	/* Sets the bit. */
	bits[index / 8U] = (uint8_t)(bits[index / 8U] | (1U << (index % 8U)));
}

/* Clears one bit of a block's bitmap. */
static void
heap_clear_bit(
	uint8_t *bits,
	uint32_t index)
{
	/* Clears the bit. */
	bits[index / 8U] = (uint8_t)(bits[index / 8U] & ~(1U << (index % 8U)));
}

/* Finds the address of a cell of a block by its index. */
static unsigned char *
heap_block_cell(
	struct vm_block *block,
	uint32_t index)
{
	/* Reports the cell's address. */
	return (unsigned char *)block + block->first_offset + (size_t)index * block->cell_size;
}
