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

enum zedbsd_heap_event {
	ZEDBSD_HEAP_ALLOCATED = 0,
	ZEDBSD_HEAP_FREED,
};

typedef void (*zedbsd_heap_observer_fn)(void *context, void *pointer,
					size_t size,
					enum zedbsd_heap_event event);

struct heap_block;
typedef size_t (*zedbsd_heap_grow_fn)(void *context, void *end,
				      size_t minimum_size);

/*
 * Allocator state is explicit.  Long-lived kernel users must use an explicit
 * instance; the active instance exists only for libc/Noct compatibility.
 */
struct zedbsd_heap {
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
	zedbsd_heap_observer_fn observer;
	void *observer_context;
	zedbsd_heap_grow_fn grow;
	void *grow_context;
};

void zedbsd_heap_init_instance(struct zedbsd_heap *heap, void *base, size_t size);
void zedbsd_heap_reset_instance(struct zedbsd_heap *heap);
void zedbsd_heap_set_failure_after_instance(struct zedbsd_heap *heap,
					    size_t successful_allocations);
void zedbsd_heap_set_observer_instance(struct zedbsd_heap *heap,
				       zedbsd_heap_observer_fn observer,
				       void *context);
void zedbsd_heap_set_grow_instance(struct zedbsd_heap *heap,
				   zedbsd_heap_grow_fn grow, void *context);
void *zedbsd_heap_alloc(struct zedbsd_heap *heap, size_t size);
void *zedbsd_heap_calloc(struct zedbsd_heap *heap, size_t count, size_t size);
void *zedbsd_heap_realloc(struct zedbsd_heap *heap, void *pointer, size_t size);
void zedbsd_heap_free(struct zedbsd_heap *heap, void *pointer);
size_t zedbsd_heap_current_instance(const struct zedbsd_heap *heap);
size_t zedbsd_heap_peak_instance(const struct zedbsd_heap *heap);
size_t zedbsd_heap_largest_failed_instance(const struct zedbsd_heap *heap);
size_t zedbsd_heap_error_count_instance(const struct zedbsd_heap *heap);
size_t zedbsd_heap_largest_free_instance(const struct zedbsd_heap *heap);
int zedbsd_heap_validate_instance(const struct zedbsd_heap *heap);

/* Switch libc compatibility calls to heap and return the previous instance. */
struct zedbsd_heap *zedbsd_heap_set_active(struct zedbsd_heap *heap);
struct zedbsd_heap *zedbsd_heap_get_active(void);

void zedbsd_heap_init(void *base, size_t size);
void zedbsd_heap_reset(void);
void zedbsd_heap_set_failure_after(size_t successful_allocations);
void zedbsd_heap_set_observer(zedbsd_heap_observer_fn observer, void *context);
void *zedbsd_malloc(size_t size);
void *zedbsd_calloc(size_t count, size_t size);
void *zedbsd_realloc(void *pointer, size_t size);
char *zedbsd_strdup(const char *string);
void zedbsd_free(void *pointer);
size_t zedbsd_heap_current(void);
size_t zedbsd_heap_peak(void);
size_t zedbsd_heap_error_count(void);
size_t zedbsd_heap_largest_free(void);
int zedbsd_heap_validate(void);

#endif
