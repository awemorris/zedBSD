/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fixed kernel heap allocator.
 *
 * Copied from src/libc/heap.c and reduced for the kernel; the two are maintained
 * separately from now on.  The kernel keeps only what kern_malloc() uses:
 * first-fit allocation from one fixed range, release with merging of free
 * neighbours, the statistics, and the validators.  Growing the range, failure
 * injection, realloc, aligned allocation and the C library entry points stay
 * in libc.
 *
 * Every block starts with a header on the 16-byte grid, followed by its
 * payload.  The physical chain links every block in address order, and the
 * free list links the free ones in no particular order.
 */

#include <stdint.h>

#include <kern/kcrt.h>

#include "heap.h"

/*
 * What every allocation is aligned to.
 *
 * An allocator must return memory that any object may be placed in, which
 * means the strictest alignment the machine asks for of an ordinary type.
 * On x86 that is sixteen: the SSE registers are that wide, and the aligned
 * forms of the instructions that move them fault on anything less.  Eight
 * was enough for as long as nothing put such a type on the heap, which is
 * a thing that holds until it does not: a compiler allocating a thirty-two
 * byte object and clearing it with one aligned store found it.
 */
#define KERN_HEAP_ALIGNMENT 16U

/* The magic of a live block header; a merged-away header has it cleared. */
#define KERN_HEAP_MAGIC 0x42393848U

/* The state of a block on the free list. */
#define KERN_HEAP_FREE 0x46524545U

/* The state of a block handed out to a caller. */
#define KERN_HEAP_USED 0x55534544U

/*
 * The header in front of every block of a heap.
 *
 * capacity is the payload size the block owns; used is what the caller asked
 * for, which the statistics count.  A block is on the physical chain from its
 * creation until it is merged into a neighbour, and on the free list exactly
 * while its state is KERN_HEAP_FREE.
 */
struct kern_heap_block {
	uint32_t magic;
	uint32_t state;
	size_t capacity;
	size_t used;
	struct kern_heap_block *previous_physical;
	struct kern_heap_block *next_physical;
	struct kern_heap_block *previous_free;
	struct kern_heap_block *next_free;
};

static size_t kern_heap_aligned_size(size_t size);
static size_t kern_heap_header_size(void);
static uint8_t *kern_heap_payload(struct kern_heap_block *block);
static void kern_heap_remove_free(struct kern_heap *heap, struct kern_heap_block *block);
static void kern_heap_insert_free(struct kern_heap *heap, struct kern_heap_block *block);
static void kern_heap_split(struct kern_heap *heap, struct kern_heap_block *block, size_t capacity);
static void kern_heap_merge_next(struct kern_heap *heap, struct kern_heap_block *block);
static struct kern_heap_block *kern_heap_owner(const struct kern_heap *heap, void *pointer);
static int kern_heap_validate_chain(const struct kern_heap *heap, size_t *free_count);
static int kern_heap_validate_free_list(const struct kern_heap *heap, size_t free_count);

/*
 * Initializes a heap over one range of memory.
 *
 * The range is trimmed to the 16-byte grid.  A range too small for one block
 * leaves an empty heap that fails every allocation.
 */
void
kern_heap_init(
	struct kern_heap *heap,
	void *base,
	size_t size)
{
	uintptr_t raw;
	uintptr_t aligned;
	size_t skipped;
	size_t usable;
	size_t header;

	/* Starts from an empty heap. */
	kern_memset(heap, 0, sizeof(*heap));
	header = kern_heap_header_size();

	/* Refuses a missing range, or one whose start cannot be aligned. */
	raw = (uintptr_t)base;
	if (base == NULL || raw > UINTPTR_MAX - (KERN_HEAP_ALIGNMENT - 1U))
		return;

	/* Trims the range to the grid and refuses one too small for a block. */
	aligned = (raw + KERN_HEAP_ALIGNMENT - 1U) & ~(uintptr_t)(KERN_HEAP_ALIGNMENT - 1U);
	skipped = (size_t)(aligned - raw);
	if (skipped > size)
		return;

	usable = (size - skipped) & ~(size_t)(KERN_HEAP_ALIGNMENT - 1U);
	if (usable < header + KERN_HEAP_ALIGNMENT)
		return;

	/* Makes the whole range one free block. */
	heap->begin = (uint8_t *)aligned;
	heap->end = heap->begin + usable;
	heap->first = (struct kern_heap_block *)heap->begin;
	heap->first->magic = KERN_HEAP_MAGIC;
	heap->first->state = KERN_HEAP_FREE;
	heap->first->capacity = usable - header;
	heap->first->used = 0;
	heap->first->previous_physical = NULL;
	heap->first->next_physical = NULL;
	heap->first->previous_free = NULL;
	heap->first->next_free = NULL;
	heap->free_list = heap->first;
}

/*
 * Allocates a block from a heap.
 *
 * The first free block that is large enough is used, and its unused tail is
 * split off when it can hold a block of its own.  A request of zero bytes
 * gets the smallest block.
 */
void *
kern_heap_alloc(
	struct kern_heap *heap,
	size_t size)
{
	struct kern_heap_block *block;
	uint8_t *payload;
	size_t requested;
	size_t capacity;

	/* Rounds the request up to the grid, refusing one that overflows. */
	requested = size;
	if (size == 0)
		size = 1;
	capacity = kern_heap_aligned_size(size);
	if (capacity == 0) {
		if (requested > heap->largest_failed_allocation)
			heap->largest_failed_allocation = requested;
		return NULL;
	}

	/* Finds the first free block that is large enough. */
	for (block = heap->free_list; block != NULL; block = block->next_free) {
		if (block->capacity >= capacity)
			break;
	}

	/* Records the largest request the heap could not serve. */
	if (block == NULL) {
		if (requested > heap->largest_failed_allocation)
			heap->largest_failed_allocation = requested;
		return NULL;
	}

	/* Takes the block off the free list and returns its tail to it. */
	kern_heap_remove_free(heap, block);
	kern_heap_split(heap, block, capacity);

	/* Hands the block out and counts the requested bytes as in use. */
	block->state = KERN_HEAP_USED;
	block->used = requested;
	heap->current_bytes += requested;
	if (heap->current_bytes > heap->peak_bytes)
		heap->peak_bytes = heap->current_bytes;

	/* Tells the observer about the allocation. */
	payload = kern_heap_payload(block);
	if (heap->observer != NULL)
		heap->observer(heap->observer_context, payload, requested, KERN_HEAP_EVENT_ALLOCATED);

	/* Succeeded: reports the payload of the block. */
	return payload;
}

/*
 * Returns a block to a heap.
 *
 * The block is merged with a free neighbour on either side.  A pointer the
 * heap did not hand out, or one already released, is counted in errors and
 * otherwise ignored.
 */
void
kern_heap_free(
	struct kern_heap *heap,
	void *pointer)
{
	struct kern_heap_block *block;
	struct kern_heap_block *previous;

	/* Ignores a null pointer. */
	if (pointer == NULL)
		return;

#ifdef KERN_KERNEL_HEAP_TRACE
	/* Records the pointer before the heap walks its chain for it. */
	kern_heap_trace_pointer_walk(pointer, __builtin_return_address(0));
#endif

	/* Finds the block and counts a foreign or repeated release as an error. */
	block = kern_heap_owner(heap, pointer);
	if (block == NULL || block->state != KERN_HEAP_USED) {
		heap->errors++;
		return;
	}

	/* Tells the observer about the release. */
	if (heap->observer != NULL)
		heap->observer(heap->observer_context, pointer, block->used, KERN_HEAP_EVENT_FREED);

	/* Stops counting the block's bytes as in use and marks it free. */
	heap->current_bytes -= block->used;
	block->used = 0;
	block->state = KERN_HEAP_FREE;

	/* Absorbs a free block that follows. */
	kern_heap_merge_next(heap, block);

	/* Lets a free block that precedes absorb this one. */
	previous = block->previous_physical;
	if (previous != NULL && previous->state == KERN_HEAP_FREE) {
		kern_heap_remove_free(heap, previous);
		previous->capacity += kern_heap_header_size() + block->capacity;
		previous->next_physical = block->next_physical;
		if (previous->next_physical != NULL)
			previous->next_physical->previous_physical = previous;
		block->magic = 0;
		block = previous;
	}

	/* Puts the merged block on the free list. */
	kern_heap_insert_free(heap, block);
}

/*
 * Reports the bytes the callers of a heap hold.
 */
size_t
kern_heap_current(
	const struct kern_heap *heap)
{
	/* Reports the requested bytes of every block in use. */
	return heap->current_bytes;
}

/*
 * Reports the most bytes the callers of a heap have held at once.
 */
size_t
kern_heap_peak(
	const struct kern_heap *heap)
{
	/* Reports the high-water mark of the bytes in use. */
	return heap->peak_bytes;
}

/*
 * Reports the largest request a heap could not serve.
 */
size_t
kern_heap_largest_failed(
	const struct kern_heap *heap)
{
	/* Reports the largest refused request, or zero. */
	return heap->largest_failed_allocation;
}

/*
 * Reports the payload size of the largest free block of a heap.
 */
size_t
kern_heap_largest_free(
	const struct kern_heap *heap)
{
	struct kern_heap_block *block;
	size_t largest;

	/* Finds the largest free block. */
	largest = 0;
	for (block = heap->free_list; block != NULL; block = block->next_free) {
		if (block->capacity > largest)
			largest = block->capacity;
	}

	/* Reports the largest payload, or zero when nothing is free. */
	return largest;
}

/*
 * Checks every invariant of a heap.
 *
 * The chain must tile the range exactly with valid headers, no two free
 * blocks may be neighbours, the bytes in use must match the counter, and the
 * free list must hold every free block once and nothing else.  Reports
 * nonzero for a consistent heap.
 */
int
kern_heap_validate(
	const struct kern_heap *heap)
{
	size_t free_count;
	int valid;

	/* Accepts an empty heap only when nothing points into it. */
	if (heap->first == NULL) {
		if (heap->begin != NULL)
			return 0;
		if (heap->free_list != NULL)
			return 0;
		return 1;
	}

	/* Checks the physical chain and counts its free blocks. */
	valid = kern_heap_validate_chain(heap, &free_count);
	if (!valid)
		return 0;

	/* Checks the free list against that count. */
	valid = kern_heap_validate_free_list(heap, free_count);
	if (!valid)
		return 0;

	/* Succeeded: the heap is consistent. */
	return 1;
}

#ifdef KERN_KERNEL_HEAP_TRACE
/*
 * Installs the function told about every allocation and release.
 */
void
kern_heap_set_observer(
	struct kern_heap *heap,
	kern_heap_observer_fn observer,
	void *context)
{
	/* Records the observer and the context it is called with. */
	heap->observer = observer;
	heap->observer_context = context;
}

/*
 * Checks a heap without following any link it has not proved.
 *
 * This is the validator of the heap trace build, which runs it on every lock
 * transition to catch the first corruption.  Each link is compared with the
 * address it must have before it is followed, and every walk is bounded by
 * the number of blocks the range can hold, so a cycle or a foreign link is
 * reported instead of followed.  Reports nonzero for a consistent heap.
 */
int
kern_heap_trace_validate(
	const struct kern_heap *heap)
{
	const struct kern_heap_block *block;
	const struct kern_heap_block *free_block;
	const struct kern_heap_block *previous;
	const struct kern_heap_block *previous_free;
	uintptr_t expected;
	uintptr_t end;
	size_t block_limit;
	size_t free_count;
	size_t free_steps;
	size_t physical_steps;
	size_t used;
	size_t header;
	int found;

	/* Accepts an empty heap only when it has no chain. */
	if (heap->begin == NULL) {
		if (heap->first != NULL)
			return 0;
		return 1;
	}

	/* Bounds every walk by the number of blocks the range can hold. */
	header = kern_heap_header_size();
	expected = (uintptr_t)heap->begin;
	end = (uintptr_t)heap->end;
	block_limit = (end - expected) / header + 1U;

	/* Walks the chain, checking each link against where it must point. */
	previous = NULL;
	used = 0;
	free_count = 0;
	physical_steps = 0;
	for (block = heap->first; block != NULL; block = block->next_physical) {
		/* Refuses a chain longer than the range can hold. */
		physical_steps++;
		if (physical_steps > block_limit)
			return 0;

		/* Refuses a block that is not where the previous one ends. */
		if ((uintptr_t)block != expected)
			return 0;
		if (expected % KERN_HEAP_ALIGNMENT != 0)
			return 0;
		if (end - expected < header)
			return 0;

		/* Refuses a header that is not consistent with its place. */
		if (block->magic != KERN_HEAP_MAGIC)
			return 0;
		if (block->previous_physical != previous)
			return 0;
		if (block->capacity > end - expected - header)
			return 0;
		if (block->used > block->capacity)
			return 0;
		if (block->state != KERN_HEAP_FREE && block->state != KERN_HEAP_USED)
			return 0;

		/* Counts the bytes in use and the free blocks. */
		if (block->state == KERN_HEAP_USED) {
			used += block->used;
		} else {
			free_count++;
		}

		expected += header + block->capacity;
		previous = block;
	}

	/* Refuses a chain that does not end at the range end or miscounts its bytes. */
	if (expected != end)
		return 0;
	if (used != heap->current_bytes)
		return 0;

	/* Walks the free list, finding each entry on the proved chain. */
	previous_free = NULL;
	free_steps = 0;
	for (free_block = heap->free_list; free_block != NULL; free_block = free_block->next_free) {
		/* Refuses a free list longer than the chain's free blocks. */
		free_steps++;
		if (free_steps > block_limit)
			return 0;
		if (free_steps > free_count)
			return 0;

		/* Finds the entry on the chain before reading it. */
		found = 0;
		physical_steps = 0;
		for (block = heap->first; block != NULL; block = block->next_physical) {
			physical_steps++;
			if (physical_steps > block_limit)
				return 0;
			if (block == free_block) {
				found = 1;
				break;
			}
		}

		/* Refuses an entry off the chain, not free, or badly linked back. */
		if (!found)
			return 0;
		if (free_block->state != KERN_HEAP_FREE)
			return 0;
		if (free_block->previous_free != previous_free)
			return 0;

		previous_free = free_block;
	}

	/* Refuses a free list that misses a free block. */
	if (free_steps != free_count)
		return 0;

	/* Succeeded: the heap is consistent. */
	return 1;
}
#endif

/* Rounds a size up to the grid, or reports zero when that overflows. */
static size_t
kern_heap_aligned_size(
	size_t size)
{
	/* Refuses a size that overflows when rounded up. */
	if (size > SIZE_MAX - (KERN_HEAP_ALIGNMENT - 1U))
		return 0;

	/* Reports the rounded size. */
	return (size + KERN_HEAP_ALIGNMENT - 1U) & ~(size_t)(KERN_HEAP_ALIGNMENT - 1U);
}

/* Reports the size of a block header on the grid. */
static size_t
kern_heap_header_size(
	void)
{
	size_t size;

	/* Rounds the header up to the grid. */
	size = kern_heap_aligned_size(sizeof(struct kern_heap_block));

	/* Reports the header size. */
	return size;
}

/* Reports where the payload of a block starts. */
static uint8_t *
kern_heap_payload(
	struct kern_heap_block *block)
{
	size_t header;

	/* Measures the header the payload follows. */
	header = kern_heap_header_size();

	/* Reports the first byte after the header. */
	return (uint8_t *)block + header;
}

/* Unlinks a block from the free list. */
static void
kern_heap_remove_free(
	struct kern_heap *heap,
	struct kern_heap_block *block)
{
	/* Links the neighbours on the free list to each other. */
	if (block->previous_free != NULL) {
		block->previous_free->next_free = block->next_free;
	} else {
		heap->free_list = block->next_free;
	}

	if (block->next_free != NULL)
		block->next_free->previous_free = block->previous_free;

	/* Leaves the block with no free-list links. */
	block->previous_free = NULL;
	block->next_free = NULL;
}

/* Links a block at the head of the free list. */
static void
kern_heap_insert_free(
	struct kern_heap *heap,
	struct kern_heap_block *block)
{
	/* Puts the block in front of the current head. */
	block->previous_free = NULL;
	block->next_free = heap->free_list;
	if (heap->free_list != NULL)
		heap->free_list->previous_free = block;
	heap->free_list = block;
}

/* Splits the tail beyond capacity off a block as a new free block. */
static void
kern_heap_split(
	struct kern_heap *heap,
	struct kern_heap_block *block,
	size_t capacity)
{
	struct kern_heap_block *tail;
	size_t header;

	/* Keeps the whole block when the tail could not hold a block of its own. */
	header = kern_heap_header_size();
	if (capacity > block->capacity)
		return;
	if (block->capacity - capacity < header + KERN_HEAP_ALIGNMENT)
		return;

	/* Builds the tail block right after the kept payload. */
	tail = (struct kern_heap_block *)(kern_heap_payload(block) + capacity);
	tail->magic = KERN_HEAP_MAGIC;
	tail->state = KERN_HEAP_FREE;
	tail->capacity = block->capacity - capacity - header;
	tail->used = 0;
	tail->previous_free = NULL;
	tail->next_free = NULL;

	/* Links the tail into the physical chain after the block. */
	tail->previous_physical = block;
	tail->next_physical = block->next_physical;
	if (tail->next_physical != NULL)
		tail->next_physical->previous_physical = tail;
	block->next_physical = tail;
	block->capacity = capacity;

	/* Makes the tail available. */
	kern_heap_insert_free(heap, tail);
}

/* Absorbs the next block into a block when the next one is free. */
static void
kern_heap_merge_next(
	struct kern_heap *heap,
	struct kern_heap_block *block)
{
	struct kern_heap_block *next;

	/* Keeps the blocks apart unless the next one is free. */
	next = block->next_physical;
	if (next == NULL || next->state != KERN_HEAP_FREE)
		return;

	/* Takes the next block off the free list and joins its range. */
	kern_heap_remove_free(heap, next);
	block->capacity += kern_heap_header_size() + next->capacity;
	block->next_physical = next->next_physical;
	if (block->next_physical != NULL)
		block->next_physical->previous_physical = block;

	/* Clears the absorbed header so a stale pointer to it is not accepted. */
	next->magic = 0;
}

/*
 * Finds the block whose payload a pointer is, or none.
 *
 * The header before the pointer is trusted when it carries the magic and
 * its physical neighbours point back at it: a stale or foreign pointer
 * fails one of those.  The walk of the whole chain that this once did
 * made every free cost the number of blocks in the heap; it remains only
 * in the traced build, where the heap is checked for corruption anyway.
 */
static struct kern_heap_block *
kern_heap_owner(
	const struct kern_heap *heap,
	void *pointer)
{
	struct kern_heap_block *block;
	struct kern_heap_block *neighbour;
	uint8_t *bytes;
	size_t header;
#ifdef KERN_KERNEL_HEAP_TRACE
	struct kern_heap_block *cursor;
	size_t limit;
	size_t steps;
#endif

	/* Refuses a pointer outside the payload area of the range. */
	bytes = pointer;
	header = kern_heap_header_size();
	if (heap->begin == NULL)
		return NULL;
	if (bytes < heap->begin + header)
		return NULL;
	if (bytes >= heap->end)
		return NULL;

	/* Refuses a pointer that is not right after a live header. */
	block = (struct kern_heap_block *)(bytes - header);
	if ((uintptr_t)block % KERN_HEAP_ALIGNMENT != 0)
		return NULL;
	if (block->magic != KERN_HEAP_MAGIC)
		return NULL;

	/* Refuses a header whose predecessor does not know it. */
	neighbour = block->previous_physical;
	if (neighbour == NULL) {
		if (heap->first != block)
			return NULL;
	} else if (neighbour->next_physical != block) {
		return NULL;
	}

	/* Refuses a header whose successor does not know it. */
	neighbour = block->next_physical;
	if (neighbour != NULL && neighbour->previous_physical != block)
		return NULL;

#ifdef KERN_KERNEL_HEAP_TRACE
	/* Bounds the walk by the number of blocks the range can hold. */
	limit = (size_t)(heap->end - heap->begin) / header + 1U;
	steps = 0;

	/* Accepts the header only when it is on the chain. */
	for (cursor = heap->first; cursor != NULL; cursor = cursor->next_physical) {
		/* Stops a walk longer than the range can hold. */
		steps++;
		if (steps > limit)
			return NULL;

		/* Reports the block found on the chain. */
		if (cursor == block)
			return block;
	}

	/* Reports a header that looked live but is not on the chain. */
	return NULL;
#else
	/* Reports the block the links vouch for. */
	return block;
#endif
}

/* Checks the physical chain and counts its free blocks. */
static int
kern_heap_validate_chain(
	const struct kern_heap *heap,
	size_t *free_count)
{
	struct kern_heap_block *block;
	struct kern_heap_block *previous;
	size_t header;
	size_t room;
	size_t used;
	size_t span;

	/* Walks the chain, which must tile the range from its start. */
	header = kern_heap_header_size();
	previous = NULL;
	used = 0;
	span = 0;
	*free_count = 0;
	for (block = heap->first; block != NULL; block = block->next_physical) {
		/* Refuses a header that is not live, linked back, and in place. */
		if (block->magic != KERN_HEAP_MAGIC)
			return 0;
		if (block->previous_physical != previous)
			return 0;
		if ((uint8_t *)block != heap->begin + span)
			return 0;
		if (block->state != KERN_HEAP_FREE && block->state != KERN_HEAP_USED)
			return 0;

		/* Refuses a block that reaches past the range end. */
		room = (size_t)(heap->end - (uint8_t *)block);
		if (header > room)
			return 0;
		if (block->capacity > room - header)
			return 0;

		span += header + block->capacity;

		/* Counts a block in use, or a free block that has no free neighbour after it. */
		if (block->state == KERN_HEAP_USED) {
			if (block->used > block->capacity)
				return 0;
			used += block->used;
		} else {
			(*free_count)++;
			if (block->next_physical != NULL && block->next_physical->state == KERN_HEAP_FREE)
				return 0;
		}

		previous = block;
	}

	/* Refuses a chain that leaves part of the range uncovered. */
	if (heap->begin + span != heap->end)
		return 0;

	/* Refuses a chain whose bytes in use disagree with the counter. */
	if (used != heap->current_bytes)
		return 0;

	/* Succeeded: the chain tiles the range. */
	return 1;
}

/* Checks that the free list holds each free block of the chain once. */
static int
kern_heap_validate_free_list(
	const struct kern_heap *heap,
	size_t free_count)
{
	struct kern_heap_block *block;
	struct kern_heap_block *free_block;
	struct kern_heap_block *previous_free;
	struct kern_heap_block *slow;
	struct kern_heap_block *fast;
	size_t list_count;
	size_t found;

	/* Refuses a free list that loops, before walking it in full. */
	slow = heap->free_list;
	fast = heap->free_list;
	while (fast != NULL && fast->next_free != NULL) {
		slow = slow->next_free;
		fast = fast->next_free->next_free;
		if (slow == fast)
			return 0;
	}

	/* Walks the free list, finding each entry once on the chain. */
	previous_free = NULL;
	list_count = 0;
	for (free_block = heap->free_list; free_block != NULL; free_block = free_block->next_free) {
		/* Refuses an entry that is not a free, correctly linked block. */
		if (free_block->magic != KERN_HEAP_MAGIC)
			return 0;
		if (free_block->state != KERN_HEAP_FREE)
			return 0;
		if (free_block->previous_free != previous_free)
			return 0;

		/* Refuses an entry that is not on the chain exactly once. */
		found = 0;
		for (block = heap->first; block != NULL; block = block->next_physical) {
			if (block == free_block)
				found++;
		}

		if (found != 1)
			return 0;

		/* Refuses a free list longer than the chain's free blocks. */
		previous_free = free_block;
		list_count++;
		if (list_count > free_count)
			return 0;
	}

	/* Refuses a free list that misses a free block. */
	if (list_count != free_count)
		return 0;

	/* Succeeded: the free list matches the chain. */
	return 1;
}
