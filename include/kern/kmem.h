/*
 * Kernel memory allocation
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_KERN_KMEM_H
#define ZEDBSD_KERN_KMEM_H

#include <stddef.h>

void *kern_malloc(size_t size);
void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);

struct kern_memory_stats {
	size_t heap_fixed;
	size_t heap_current;
	size_t heap_peak;
	size_t heap_largest_free;
	size_t heap_largest_failed;
	size_t image_bytes;
};

void kern_memory_get_stats(struct kern_memory_stats *);

#endif
