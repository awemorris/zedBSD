/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Boots freestanding C library
 */

#ifndef BOOTS_HEAP_H
#define BOOTS_HEAP_H

#include <stddef.h>
#include <stdint.h>

enum boots_heap_event {
	BOOTS_HEAP_ALLOCATED = 0,
	BOOTS_HEAP_FREED,
};

typedef void (*boots_heap_observer_fn)(void *context, void *pointer,
					size_t size,
					enum boots_heap_event event);

struct heap_block;

/*
 * Allocator state is explicit.  Long-lived kernel users must use an explicit
 * instance; the active instance exists only for libc/Noct compatibility.
 */
struct boots_heap {
	void *original_base;
	size_t original_size;
	uint8_t *begin;
	uint8_t *end;
	struct heap_block *first;
	struct heap_block *free_list;
	size_t current_bytes;
	size_t peak_bytes;
	size_t errors;
	size_t fail_after;
	size_t successful_allocations;
	boots_heap_observer_fn observer;
	void *observer_context;
};

void boots_heap_init_instance(struct boots_heap *heap, void *base, size_t size);
void boots_heap_reset_instance(struct boots_heap *heap);
void boots_heap_set_failure_after_instance(struct boots_heap *heap,
					    size_t successful_allocations);
void boots_heap_set_observer_instance(struct boots_heap *heap,
				       boots_heap_observer_fn observer,
				       void *context);
void *boots_heap_alloc(struct boots_heap *heap, size_t size);
void *boots_heap_calloc(struct boots_heap *heap, size_t count, size_t size);
void *boots_heap_realloc(struct boots_heap *heap, void *pointer, size_t size);
void boots_heap_free(struct boots_heap *heap, void *pointer);
size_t boots_heap_current_instance(const struct boots_heap *heap);
size_t boots_heap_peak_instance(const struct boots_heap *heap);
size_t boots_heap_error_count_instance(const struct boots_heap *heap);
size_t boots_heap_largest_free_instance(const struct boots_heap *heap);
int boots_heap_validate_instance(const struct boots_heap *heap);

/* Switch libc compatibility calls to heap and return the previous instance. */
struct boots_heap *boots_heap_set_active(struct boots_heap *heap);
struct boots_heap *boots_heap_get_active(void);

void boots_heap_init(void *base, size_t size);
void boots_heap_reset(void);
void boots_heap_set_failure_after(size_t successful_allocations);
void boots_heap_set_observer(boots_heap_observer_fn observer, void *context);
void *boots_malloc(size_t size);
void *boots_calloc(size_t count, size_t size);
void *boots_realloc(void *pointer, size_t size);
char *boots_strdup(const char *string);
void boots_free(void *pointer);
size_t boots_heap_current(void);
size_t boots_heap_peak(void);
size_t boots_heap_error_count(void);
size_t boots_heap_largest_free(void);
int boots_heap_validate(void);

#endif
