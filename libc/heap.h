/*
 * zedBSD freestanding C library
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_HEAP_H
#define ZEDBSD_HEAP_H

#include <stddef.h>
#include <stdint.h>

enum heap_event {
	ZEDBSD_HEAP_ALLOCATED = 0,
	ZEDBSD_HEAP_FREED,
};

typedef void (*heap_observer_fn)(void *context, void *pointer,
					size_t size,
					enum heap_event event);

struct heap_block;
typedef size_t (*heap_grow_fn)(void *context, void *end,
				      size_t minimum_size);

/*
 * Allocator state is explicit.  Long-lived kernel users must use an explicit
 * instance; the active instance exists only for libc/Noct compatibility.
 */
struct heap_allocator {
	void *original_base;
	size_t original_size;
	uint8_t *begin;
	uint8_t *end;
	struct heap_block *first;
	struct heap_block *free_list;
	size_t current_bytes;
	size_t peak_bytes;
	size_t largest_failed_allocation;
	size_t errors;
	size_t fail_after;
	size_t successful_allocations;
	heap_observer_fn observer;
	void *observer_context;
	heap_grow_fn grow;
	void *grow_context;
};

void heap_allocator_init(struct heap_allocator *heap, void *base, size_t size);
void heap_allocator_reset(struct heap_allocator *heap);
void heap_allocator_set_failure_after(struct heap_allocator *heap,
					    size_t successful_allocations);
void heap_allocator_set_observer(struct heap_allocator *heap,
				       heap_observer_fn observer,
				       void *context);
void heap_allocator_set_grow(struct heap_allocator *heap,
				   heap_grow_fn grow, void *context);
void *heap_allocator_alloc(struct heap_allocator *heap, size_t size);
void *heap_allocator_aligned_alloc(struct heap_allocator *heap,
    size_t alignment, size_t size);
void *heap_allocator_calloc(struct heap_allocator *heap, size_t count, size_t size);
void *heap_allocator_realloc(struct heap_allocator *heap, void *pointer, size_t size);
void heap_allocator_free(struct heap_allocator *heap, void *pointer);
size_t heap_allocator_current(const struct heap_allocator *heap);
size_t heap_allocator_peak(const struct heap_allocator *heap);
size_t heap_allocator_largest_failed(const struct heap_allocator *heap);
size_t heap_allocator_error_count(const struct heap_allocator *heap);
size_t heap_allocator_largest_free(const struct heap_allocator *heap);
int heap_allocator_validate(const struct heap_allocator *heap);

/* Switch libc compatibility calls to heap and return the previous instance. */
struct heap_allocator *heap_active_set(struct heap_allocator *heap);
struct heap_allocator *heap_active_get(void);

void heap_active_init(void *base, size_t size);
void heap_active_reset(void);
void heap_active_set_failure_after(size_t successful_allocations);
void heap_active_set_observer(heap_observer_fn observer, void *context);
void *heap_alloc_active(size_t size);
void *heap_aligned_alloc_active(size_t alignment, size_t size);
void *heap_calloc_active(size_t count, size_t size);
void *heap_realloc_active(void *pointer, size_t size);
char *heap_strdup_active(const char *string);
void heap_free_active(void *pointer);
size_t heap_active_current(void);
size_t heap_active_peak(void);
size_t heap_active_error_count(void);
size_t heap_active_largest_free(void);
int heap_active_validate(void);

#endif
