/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The host test's stand-in for the kernel's allocator: the host's. */

#ifndef KERN_KERN_KMEM_H
#define KERN_KERN_KMEM_H

#include <stddef.h>

void *
kern_malloc(
	size_t size);

void *
kern_calloc(
	size_t count,
	size_t size);

void
kern_free(
	void *pointer);

#endif
