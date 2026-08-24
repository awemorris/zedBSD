/*
 * zedBSD freestanding C library
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "libc/heap.h"

#include <errno.h>
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

static struct heap_allocator default_heap;
static struct heap_allocator *active_heap = &default_heap;

__attribute__((weak)) void __libc_heap_lock(void) { }
__attribute__((weak)) void __libc_heap_unlock(void) { }

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
remove_free(struct heap_allocator *heap, struct heap_block *block)
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
insert_free(struct heap_allocator *heap, struct heap_block *block)
{
	block->previous_free = NULL;
	block->next_free = heap->free_list;
	if (heap->free_list != NULL)
		heap->free_list->previous_free = block;
	heap->free_list = block;
}

static struct heap_block *
split_block(struct heap_allocator *heap, struct heap_block *block, size_t capacity)
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
merge_with_next(struct heap_allocator *heap, struct heap_block *block)
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
pointer_block(const struct heap_allocator *heap, void *pointer)
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
heap_allocator_init(struct heap_allocator *heap, void *base, size_t size)
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
heap_allocator_reset(struct heap_allocator *heap)
{
	void *base;
	size_t size;
	size_t failure;
	heap_grow_fn grow;
	void *grow_context;

	if (heap == NULL)
		return;
	base = heap->original_base;
	size = heap->original_size;
	failure = heap->fail_after;
	grow = heap->grow;
	grow_context = heap->grow_context;
	heap_allocator_init(heap, base, size);
	heap->fail_after = failure;
	heap->grow = grow;
	heap->grow_context = grow_context;
}

void
heap_allocator_set_failure_after(struct heap_allocator *heap,
				       size_t successful_allocations)
{
	if (heap == NULL)
		return;
	heap->fail_after = successful_allocations;
	heap->successful_allocations = 0;
}

void
heap_allocator_set_observer(struct heap_allocator *heap,
				  heap_observer_fn observer,
				  void *context)
{
	if (heap == NULL)
		return;
	heap->observer = observer;
	heap->observer_context = context;
}

void
heap_allocator_set_grow(struct heap_allocator *heap,
			      heap_grow_fn grow, void *context)
{
	if (heap == NULL)
		return;
	heap->grow = grow;
	heap->grow_context = context;
}

static int
extend_heap(struct heap_allocator *heap, size_t minimum)
{
	struct heap_block *last;
	size_t added;

	if (heap->grow == NULL || heap->end == NULL)
		return 0;
	added = heap->grow(heap->grow_context, heap->end, minimum);
	if (added < minimum || (added & (HEAP_ALIGNMENT - 1U)) != 0 ||
	    added > UINTPTR_MAX - (uintptr_t)heap->end ||
	    heap->original_size > SIZE_MAX - added)
		return 0;
	for (last = heap->first; last != NULL && last->next_physical != NULL;
	     last = last->next_physical)
		;
	if (last == NULL)
		return 0;
	if (last->state == HEAP_FREE) {
		last->capacity += added;
	} else {
		struct heap_block *block;
		if (added < block_header_size() + HEAP_ALIGNMENT)
			return 0;
		block = (struct heap_block *)heap->end;
		block->magic = HEAP_MAGIC;
		block->state = HEAP_FREE;
		block->capacity = added - block_header_size();
		block->used = 0;
		block->previous_physical = last;
		block->next_physical = NULL;
		block->previous_free = NULL;
		block->next_free = NULL;
		last->next_physical = block;
		insert_free(heap, block);
	}
	heap->end += added;
	heap->original_size += added;
	return 1;
}

void *
heap_allocator_alloc(struct heap_allocator *heap, size_t size)
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
	if (block == NULL && capacity <= SIZE_MAX - block_header_size() &&
	    extend_heap(heap, capacity + block_header_size()))
		for (block = heap->free_list; block != NULL;
		     block = block->next_free)
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
heap_allocator_aligned_alloc(struct heap_allocator *heap, size_t alignment,
    size_t size)
{
	struct heap_block *block;
	struct heap_block *aligned_block;
	uint8_t *payload;
	uintptr_t aligned_payload;
	size_t requested = size;
	size_t capacity;
	size_t extension;

	if (heap == NULL || alignment == 0 ||
	    (alignment & (alignment - 1U)) != 0)
		return NULL;
	if (alignment <= HEAP_ALIGNMENT)
		return heap_allocator_alloc(heap, size);
	if (size == 0)
		size = 1;
	capacity = aligned_size(size);
	if (capacity == 0 ||
	    heap->successful_allocations >= heap->fail_after)
		goto fail;

retry:
	for (block = heap->free_list; block != NULL; block = block->next_free) {
		uintptr_t first;
		uintptr_t end;
		size_t prefix;

		payload = block_payload(block);
		if ((uintptr_t)payload > UINTPTR_MAX - (alignment - 1U))
			continue;
		first = ((uintptr_t)payload + alignment - 1U) &
		    ~(uintptr_t)(alignment - 1U);
		prefix = (size_t)(first - (uintptr_t)payload);
		if (prefix != 0 && prefix < block_header_size() + HEAP_ALIGNMENT) {
			if (first > UINTPTR_MAX - alignment)
				continue;
			first += alignment;
		}
		if (first > UINTPTR_MAX - capacity)
			continue;
		end = first + capacity;
		if (end <= (uintptr_t)payload + block->capacity) {
			aligned_payload = first;
			break;
		}
	}
	if (block == NULL) {
		if (capacity > SIZE_MAX - alignment ||
		    capacity + alignment > SIZE_MAX - block_header_size())
			goto fail;
		extension = capacity + alignment + block_header_size();
		if (!extend_heap(heap, extension))
			goto fail;
		goto retry;
	}

	remove_free(heap, block);
	payload = block_payload(block);
	if (aligned_payload == (uintptr_t)payload) {
		aligned_block = block;
	} else {
		struct heap_block *next = block->next_physical;
		uint8_t *old_end = payload + block->capacity;

		aligned_block = (struct heap_block *)(aligned_payload -
		    block_header_size());
		block->capacity = (size_t)((uint8_t *)aligned_block - payload);
		block->next_physical = aligned_block;
		aligned_block->magic = HEAP_MAGIC;
		aligned_block->state = HEAP_FREE;
		aligned_block->capacity = (size_t)(old_end -
		    (uint8_t *)aligned_payload);
		aligned_block->used = 0;
		aligned_block->previous_physical = block;
		aligned_block->next_physical = next;
		aligned_block->previous_free = NULL;
		aligned_block->next_free = NULL;
		if (next != NULL)
			next->previous_physical = aligned_block;
		insert_free(heap, block);
	}
	(void)split_block(heap, aligned_block, capacity);
	aligned_block->state = HEAP_USED;
	aligned_block->used = requested;
	heap->current_bytes += requested;
	if (heap->current_bytes > heap->peak_bytes)
		heap->peak_bytes = heap->current_bytes;
	heap->successful_allocations++;
	if (heap->observer != NULL)
		heap->observer(heap->observer_context, block_payload(aligned_block),
		    requested, ZEDBSD_HEAP_ALLOCATED);
	return block_payload(aligned_block);

fail:
	if (requested > heap->largest_failed_allocation)
		heap->largest_failed_allocation = requested;
	return NULL;
}

void *
heap_allocator_calloc(struct heap_allocator *heap, size_t count, size_t size)
{
	void *pointer;
	size_t total;

	if (count != 0 && size > SIZE_MAX / count) {
		if (heap != NULL && SIZE_MAX > heap->largest_failed_allocation)
			heap->largest_failed_allocation = SIZE_MAX;
		return NULL;
	}
	total = count * size;
	pointer = heap_allocator_alloc(heap, total);
	if (pointer != NULL)
		memset(pointer, 0, total);
	return pointer;
}

void
heap_allocator_free(struct heap_allocator *heap, void *pointer)
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
heap_allocator_realloc(struct heap_allocator *heap, void *pointer, size_t size)
{
	struct heap_block *block;
	struct heap_block *tail;
	void *replacement;
	size_t capacity;
	size_t old_used;

	if (pointer == NULL)
		return heap_allocator_alloc(heap, size);
	if (size == 0) {
		heap_allocator_free(heap, pointer);
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
	replacement = heap_allocator_alloc(heap, size);
	if (replacement == NULL)
		return NULL;
	memcpy(replacement, pointer, old_used < size ? old_used : size);
	heap_allocator_free(heap, pointer);
	return replacement;
}

size_t
heap_allocator_current(const struct heap_allocator *heap)
{
	return heap != NULL ? heap->current_bytes : 0;
}

size_t
heap_allocator_peak(const struct heap_allocator *heap)
{
	return heap != NULL ? heap->peak_bytes : 0;
}

size_t
heap_allocator_largest_failed(const struct heap_allocator *heap)
{
	return heap != NULL ? heap->largest_failed_allocation : 0;
}

size_t
heap_allocator_error_count(const struct heap_allocator *heap)
{
	return heap != NULL ? heap->errors : 0;
}

size_t
heap_allocator_largest_free(const struct heap_allocator *heap)
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
heap_allocator_validate(const struct heap_allocator *heap)
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

struct heap_allocator *
heap_active_set(struct heap_allocator *heap)
{
	struct heap_allocator *previous = active_heap;

	active_heap = heap != NULL ? heap : &default_heap;
	return previous;
}

struct heap_allocator *
heap_active_get(void)
{
	return active_heap;
}

void heap_active_init(void *base, size_t size)
{
	heap_allocator_init(active_heap, base, size);
}
void heap_active_reset(void) { heap_allocator_reset(active_heap); }
void heap_active_set_failure_after(size_t n)
{
	heap_allocator_set_failure_after(active_heap, n);
}
void heap_active_set_observer(heap_observer_fn observer, void *context)
{
	heap_allocator_set_observer(active_heap, observer, context);
}
void *heap_alloc_active(size_t size)
{
	void *result;
	__libc_heap_lock();
	result = heap_allocator_alloc(active_heap, size);
	__libc_heap_unlock();
	return result;
}
void *heap_aligned_alloc_active(size_t alignment, size_t size)
{
	void *result;
	__libc_heap_lock();
	result = heap_allocator_aligned_alloc(active_heap, alignment, size);
	__libc_heap_unlock();
	return result;
}
void *heap_calloc_active(size_t count, size_t size)
{
	void *result;
	__libc_heap_lock();
	result = heap_allocator_calloc(active_heap, count, size);
	__libc_heap_unlock();
	return result;
}
void *heap_realloc_active(void *pointer, size_t size)
{
	void *result;
	__libc_heap_lock();
	result = heap_allocator_realloc(active_heap, pointer, size);
	__libc_heap_unlock();
	return result;
}
void heap_free_active(void *pointer)
{
	__libc_heap_lock();
	heap_allocator_free(active_heap, pointer);
	__libc_heap_unlock();
}
size_t heap_active_current(void)
{
	return heap_allocator_current(active_heap);
}
size_t heap_active_peak(void) { return heap_allocator_peak(active_heap); }
size_t heap_active_error_count(void)
{
	return heap_allocator_error_count(active_heap);
}
size_t heap_active_largest_free(void)
{
	return heap_allocator_largest_free(active_heap);
}
int heap_active_validate(void)
{
	return heap_allocator_validate(active_heap);
}

char *
heap_strdup_active(const char *string)
{
	size_t length;
	char *copy;

	if (string == NULL)
		return NULL;
	length = strlen(string);
	if (length == SIZE_MAX)
		return NULL;
	copy = heap_alloc_active(length + 1U);
	if (copy != NULL)
		memcpy(copy, string, length + 1U);
	return copy;
}

/* Standard names are real symbols; Noct's generated sources redefine these
 * locally and therefore cannot use global preprocessor aliases. */
void *
malloc(size_t size)
{
	void *result = heap_alloc_active(size);

	if (result == NULL && size != 0)
		errno = ENOMEM;
	return result;
}

void *
calloc(size_t count, size_t size)
{
	void *result = heap_calloc_active(count, size);

	if (result == NULL && count != 0 && size != 0)
		errno = ENOMEM;
	return result;
}

void *
realloc(void *pointer, size_t size)
{
	void *result = heap_realloc_active(pointer, size);

	if (result == NULL && size != 0)
		errno = ENOMEM;
	return result;
}

void
free(void *pointer)
{
	heap_free_active(pointer);
}
