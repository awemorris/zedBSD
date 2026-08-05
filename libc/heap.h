/*
 * Boots freestanding C library
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_HEAP_H
#define BOOTS_HEAP_H

#include <stddef.h>

enum boots_heap_event {
	BOOTS_HEAP_ALLOCATED = 0,
	BOOTS_HEAP_FREED,
};

typedef void (*boots_heap_observer_fn)(void *context, void *pointer,
					size_t size,
					enum boots_heap_event event);

void boots_heap_init(void *base, size_t size);
void boots_heap_reset(void);
void boots_heap_set_failure_after(size_t successful_allocations);
void boots_heap_set_observer(boots_heap_observer_fn observer,
			      void *context);
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
