/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * zedBSD freestanding C library
 */

#include "libc/heap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HEAP_ALIGNMENT 8U
#define HEAP_MAGIC 0x42393848U
#define HEAP_FREE 0x46524545U
#define HEAP_USED 0x55534544U

struct heap_block {
	uint32_t magic;
	uint32_t state;
	size_t capacity;
	size_t used;
	struct heap_block *previous_physical;
	struct heap_block *next_physical;
	struct heap_block *previous_free;
	struct heap_block *next_free;
};

static struct zedbsd_heap default_heap;
static struct zedbsd_heap *active_heap = &default_heap;

static size_t
aligned_size(size_t size)
{
	if (size > SIZE_MAX - (HEAP_ALIGNMENT - 1U))
		return 0;
	return (size + HEAP_ALIGNMENT - 1U) & ~(HEAP_ALIGNMENT - 1U);
}

static size_t
block_header_size(void)
{
	return aligned_size(sizeof(struct heap_block));
}

static uint8_t *
block_payload(struct heap_block *block)
{
	return (uint8_t *)block + block_header_size();
}

static void
remove_free(struct zedbsd_heap *heap, struct heap_block *block)
{
	if (block->previous_free != NULL)
		block->previous_free->next_free = block->next_free;
	else
		heap->free_list = block->next_free;
	if (block->next_free != NULL)
		block->next_free->previous_free = block->previous_free;
	block->previous_free = NULL;
	block->next_free = NULL;
}

static void
insert_free(struct zedbsd_heap *heap, struct heap_block *block)
{
	block->previous_free = NULL;
	block->next_free = heap->free_list;
	if (heap->free_list != NULL)
		heap->free_list->previous_free = block;
	heap->free_list = block;
}

static struct heap_block *
split_block(struct zedbsd_heap *heap, struct heap_block *block, size_t capacity)
{
	struct heap_block *tail;
	size_t header = block_header_size();

	if (capacity > block->capacity ||
	    block->capacity - capacity < header + HEAP_ALIGNMENT)
		return NULL;
	tail = (struct heap_block *)(block_payload(block) + capacity);
	tail->magic = HEAP_MAGIC;
	tail->state = HEAP_FREE;
	tail->capacity = block->capacity - capacity - header;
	tail->used = 0;
	tail->previous_physical = block;
	tail->next_physical = block->next_physical;
	if (tail->next_physical != NULL)
		tail->next_physical->previous_physical = tail;
	tail->previous_free = NULL;
	tail->next_free = NULL;
	block->next_physical = tail;
	block->capacity = capacity;
	insert_free(heap, tail);
	return tail;
}

static void
merge_with_next(struct zedbsd_heap *heap, struct heap_block *block)
{
	struct heap_block *next = block->next_physical;

	if (next == NULL || next->state != HEAP_FREE)
		return;
	remove_free(heap, next);
	block->capacity += block_header_size() + next->capacity;
	block->next_physical = next->next_physical;
	if (block->next_physical != NULL)
		block->next_physical->previous_physical = block;
	next->magic = 0;
}

static struct heap_block *
pointer_block(const struct zedbsd_heap *heap, void *pointer)
{
	struct heap_block *block;
	struct heap_block *cursor;
	uint8_t *bytes = pointer;

	if (heap == NULL || pointer == NULL || heap->begin == NULL ||
	    bytes < heap->begin + block_header_size() || bytes >= heap->end)
		return NULL;
	block = (struct heap_block *)(bytes - block_header_size());
	if ((uintptr_t)block % HEAP_ALIGNMENT != 0 ||
	    block->magic != HEAP_MAGIC || block_payload(block) != bytes)
		return NULL;
	for (cursor = heap->first; cursor != NULL;
	     cursor = cursor->next_physical)
		if (cursor == block)
			return block;
	return NULL;
}

void
zedbsd_heap_init_instance(struct zedbsd_heap *heap, void *base, size_t size)
{
	uintptr_t raw = (uintptr_t)base;
	uintptr_t aligned;
	size_t skipped;
	size_t usable;

	if (heap == NULL)
		return;
	memset(heap, 0, sizeof(*heap));
	heap->original_base = base;
	heap->original_size = size;
	heap->fail_after = SIZE_MAX;
	if (base == NULL || raw > UINTPTR_MAX - (HEAP_ALIGNMENT - 1U))
		return;
	aligned = (raw + HEAP_ALIGNMENT - 1U) &
		~(uintptr_t)(HEAP_ALIGNMENT - 1U);
	skipped = (size_t)(aligned - raw);
	if (skipped > size)
		return;
	usable = (size - skipped) & ~(HEAP_ALIGNMENT - 1U);
	if (usable < block_header_size() + HEAP_ALIGNMENT ||
	    usable > UINTPTR_MAX - aligned)
		return;
	heap->begin = (uint8_t *)aligned;
	heap->end = heap->begin + usable;
	heap->first = (struct heap_block *)heap->begin;
	heap->first->magic = HEAP_MAGIC;
	heap->first->state = HEAP_FREE;
	heap->first->capacity = usable - block_header_size();
	heap->first->used = 0;
	heap->first->previous_physical = NULL;
	heap->first->next_physical = NULL;
	heap->first->previous_free = NULL;
	heap->first->next_free = NULL;
	heap->free_list = heap->first;
}

void
zedbsd_heap_reset_instance(struct zedbsd_heap *heap)
{
	void *base;
	size_t size;
	size_t failure;

	if (heap == NULL)
		return;
	base = heap->original_base;
	size = heap->original_size;
	failure = heap->fail_after;
	zedbsd_heap_init_instance(heap, base, size);
	heap->fail_after = failure;
}

void
zedbsd_heap_set_failure_after_instance(struct zedbsd_heap *heap,
				       size_t successful_allocations)
{
	if (heap == NULL)
		return;
	heap->fail_after = successful_allocations;
	heap->successful_allocations = 0;
}

void
zedbsd_heap_set_observer_instance(struct zedbsd_heap *heap,
				  zedbsd_heap_observer_fn observer,
				  void *context)
{
	if (heap == NULL)
		return;
	heap->observer = observer;
	heap->observer_context = context;
}

void *
zedbsd_heap_alloc(struct zedbsd_heap *heap, size_t size)
{
	struct heap_block *block;
	size_t requested = size;
	size_t capacity;

	if (heap == NULL)
		return NULL;
	if (size == 0)
		size = 1;
	capacity = aligned_size(size);
	if (capacity == 0 ||
	    heap->successful_allocations >= heap->fail_after) {
		if (requested > heap->largest_failed_allocation)
			heap->largest_failed_allocation = requested;
		return NULL;
	}
	for (block = heap->free_list; block != NULL; block = block->next_free)
		if (block->capacity >= capacity)
			break;
	if (block == NULL) {
		if (requested > heap->largest_failed_allocation)
			heap->largest_failed_allocation = requested;
		return NULL;
	}
	remove_free(heap, block);
	(void)split_block(heap, block, capacity);
	block->state = HEAP_USED;
	block->used = requested;
	heap->current_bytes += requested;
	if (heap->current_bytes > heap->peak_bytes)
		heap->peak_bytes = heap->current_bytes;
	heap->successful_allocations++;
	if (heap->observer != NULL)
		heap->observer(heap->observer_context, block_payload(block),
			       requested, ZEDBSD_HEAP_ALLOCATED);
	return block_payload(block);
}

void *
zedbsd_heap_calloc(struct zedbsd_heap *heap, size_t count, size_t size)
{
	void *pointer;
	size_t total;

	if (count != 0 && size > SIZE_MAX / count) {
		if (heap != NULL && SIZE_MAX > heap->largest_failed_allocation)
			heap->largest_failed_allocation = SIZE_MAX;
		return NULL;
	}
	total = count * size;
	pointer = zedbsd_heap_alloc(heap, total);
	if (pointer != NULL)
		memset(pointer, 0, total);
	return pointer;
}

void
zedbsd_heap_free(struct zedbsd_heap *heap, void *pointer)
{
	struct heap_block *block;
	struct heap_block *previous;

	if (pointer == NULL)
		return;
	if (heap == NULL)
		return;
	block = pointer_block(heap, pointer);
	if (block == NULL || block->state != HEAP_USED) {
		heap->errors++;
		return;
	}
	if (heap->observer != NULL)
		heap->observer(heap->observer_context, pointer, block->used,
			       ZEDBSD_HEAP_FREED);
	heap->current_bytes -= block->used;
	block->used = 0;
	block->state = HEAP_FREE;
	merge_with_next(heap, block);
	previous = block->previous_physical;
	if (previous != NULL && previous->state == HEAP_FREE) {
		remove_free(heap, previous);
		previous->capacity += block_header_size() + block->capacity;
		previous->next_physical = block->next_physical;
		if (previous->next_physical != NULL)
			previous->next_physical->previous_physical = previous;
		block->magic = 0;
		block = previous;
	}
	insert_free(heap, block);
}

void *
zedbsd_heap_realloc(struct zedbsd_heap *heap, void *pointer, size_t size)
{
	struct heap_block *block;
	struct heap_block *tail;
	void *replacement;
	size_t capacity;
	size_t old_used;

	if (pointer == NULL)
		return zedbsd_heap_alloc(heap, size);
	if (size == 0) {
		zedbsd_heap_free(heap, pointer);
		return NULL;
	}
	if (heap == NULL)
		return NULL;
	block = pointer_block(heap, pointer);
	if (block == NULL || block->state != HEAP_USED) {
		heap->errors++;
		return NULL;
	}
	capacity = aligned_size(size);
	if (capacity == 0)
		return NULL;
	old_used = block->used;
	if (capacity <= block->capacity) {
		tail = split_block(heap, block, capacity);
		if (tail != NULL)
			merge_with_next(heap, tail);
		block->used = size;
		heap->current_bytes = heap->current_bytes - old_used + size;
		if (heap->current_bytes > heap->peak_bytes)
			heap->peak_bytes = heap->current_bytes;
		return pointer;
	}
	if (block->next_physical != NULL &&
	    block->next_physical->state == HEAP_FREE &&
	    block->capacity + block_header_size() +
	    block->next_physical->capacity >= capacity) {
		merge_with_next(heap, block);
		(void)split_block(heap, block, capacity);
		block->used = size;
		heap->current_bytes = heap->current_bytes - old_used + size;
		if (heap->current_bytes > heap->peak_bytes)
			heap->peak_bytes = heap->current_bytes;
		return pointer;
	}
	replacement = zedbsd_heap_alloc(heap, size);
	if (replacement == NULL)
		return NULL;
	memcpy(replacement, pointer, old_used < size ? old_used : size);
	zedbsd_heap_free(heap, pointer);
	return replacement;
}

size_t
zedbsd_heap_current_instance(const struct zedbsd_heap *heap)
{
	return heap != NULL ? heap->current_bytes : 0;
}

size_t
zedbsd_heap_peak_instance(const struct zedbsd_heap *heap)
{
	return heap != NULL ? heap->peak_bytes : 0;
}

size_t
zedbsd_heap_largest_failed_instance(const struct zedbsd_heap *heap)
{
	return heap != NULL ? heap->largest_failed_allocation : 0;
}

size_t
zedbsd_heap_error_count_instance(const struct zedbsd_heap *heap)
{
	return heap != NULL ? heap->errors : 0;
}

size_t
zedbsd_heap_largest_free_instance(const struct zedbsd_heap *heap)
{
	struct heap_block *block;
	size_t largest = 0;

	if (heap == NULL)
		return 0;
	for (block = heap->free_list; block != NULL; block = block->next_free)
		if (block->capacity > largest)
			largest = block->capacity;
	return largest;
}

int
zedbsd_heap_validate_instance(const struct zedbsd_heap *heap)
{
	struct heap_block *block;
	struct heap_block *previous = NULL;
	struct heap_block *free_block;
	struct heap_block *previous_free = NULL;
	struct heap_block *slow;
	struct heap_block *fast;
	size_t used = 0;
	size_t span = 0;
	size_t physical_free_count = 0;
	size_t list_free_count = 0;

	if (heap == NULL)
		return 0;
	if (heap->first == NULL)
		return heap->begin == NULL && heap->free_list == NULL;
	for (block = heap->first; block != NULL; block = block->next_physical) {
		if (block->magic != HEAP_MAGIC ||
		    block->previous_physical != previous ||
		    (uint8_t *)block != heap->begin + span ||
		    (block->state != HEAP_FREE && block->state != HEAP_USED))
			return 0;
		if (block_header_size() >
		    (size_t)(heap->end - (uint8_t *)block) ||
		    block->capacity >
		    (size_t)(heap->end - (uint8_t *)block) - block_header_size())
			return 0;
		span += block_header_size() + block->capacity;
		if (block->state == HEAP_USED) {
			if (block->used > block->capacity)
				return 0;
			used += block->used;
		} else {
			physical_free_count++;
			if (block->next_physical != NULL &&
			    block->next_physical->state == HEAP_FREE)
				return 0;
		}
		previous = block;
	}
	slow = heap->free_list;
	fast = heap->free_list;
	while (fast != NULL && fast->next_free != NULL) {
		slow = slow->next_free;
		fast = fast->next_free->next_free;
		if (slow == fast)
			return 0;
	}
	for (free_block = heap->free_list; free_block != NULL;
	     free_block = free_block->next_free) {
		int found = 0;
		if (free_block->magic != HEAP_MAGIC ||
		    free_block->state != HEAP_FREE ||
		    free_block->previous_free != previous_free)
			return 0;
		for (block = heap->first; block != NULL;
		     block = block->next_physical)
			if (block == free_block)
				found++;
		if (found != 1)
			return 0;
		previous_free = free_block;
		list_free_count++;
		if (list_free_count > physical_free_count)
			return 0;
	}
	return heap->begin + span == heap->end &&
		used == heap->current_bytes &&
		list_free_count == physical_free_count;
}

struct zedbsd_heap *
zedbsd_heap_set_active(struct zedbsd_heap *heap)
{
	struct zedbsd_heap *previous = active_heap;

	active_heap = heap != NULL ? heap : &default_heap;
	return previous;
}

struct zedbsd_heap *
zedbsd_heap_get_active(void)
{
	return active_heap;
}

void zedbsd_heap_init(void *base, size_t size)
{
	zedbsd_heap_init_instance(active_heap, base, size);
}
void zedbsd_heap_reset(void) { zedbsd_heap_reset_instance(active_heap); }
void zedbsd_heap_set_failure_after(size_t n)
{
	zedbsd_heap_set_failure_after_instance(active_heap, n);
}
void zedbsd_heap_set_observer(zedbsd_heap_observer_fn observer, void *context)
{
	zedbsd_heap_set_observer_instance(active_heap, observer, context);
}
void *zedbsd_malloc(size_t size) { return zedbsd_heap_alloc(active_heap, size); }
void *zedbsd_calloc(size_t count, size_t size)
{
	return zedbsd_heap_calloc(active_heap, count, size);
}
void *zedbsd_realloc(void *pointer, size_t size)
{
	return zedbsd_heap_realloc(active_heap, pointer, size);
}
void zedbsd_free(void *pointer) { zedbsd_heap_free(active_heap, pointer); }
size_t zedbsd_heap_current(void)
{
	return zedbsd_heap_current_instance(active_heap);
}
size_t zedbsd_heap_peak(void) { return zedbsd_heap_peak_instance(active_heap); }
size_t zedbsd_heap_error_count(void)
{
	return zedbsd_heap_error_count_instance(active_heap);
}
size_t zedbsd_heap_largest_free(void)
{
	return zedbsd_heap_largest_free_instance(active_heap);
}
int zedbsd_heap_validate(void)
{
	return zedbsd_heap_validate_instance(active_heap);
}

char *
zedbsd_strdup(const char *string)
{
	size_t length;
	char *copy;

	if (string == NULL)
		return NULL;
	length = strlen(string);
	if (length == SIZE_MAX)
		return NULL;
	copy = zedbsd_malloc(length + 1U);
	if (copy != NULL)
		memcpy(copy, string, length + 1U);
	return copy;
}

void *noct_pc98be_malloc(size_t size) { return zedbsd_malloc(size); }
void *noct_pc98be_calloc(size_t count, size_t size)
{
	return zedbsd_calloc(count, size);
}
void *noct_pc98be_realloc(void *pointer, size_t size)
{
	return zedbsd_realloc(pointer, size);
}
char *noct_pc98be_strdup(const char *string) { return zedbsd_strdup(string); }
void noct_pc98be_free(void *pointer) { zedbsd_free(pointer); }

/* Standard names are real symbols; Noct's generated sources redefine these
 * locally and therefore cannot use global preprocessor aliases. */
void *malloc(size_t size) { return zedbsd_malloc(size); }
void *calloc(size_t count, size_t size) { return zedbsd_calloc(count, size); }
void *realloc(void *pointer, size_t size)
{
	return zedbsd_realloc(pointer, size);
}
void free(void *pointer) { zedbsd_free(pointer); }
