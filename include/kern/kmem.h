/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Persistent kernel allocation, independent of the active Noct arena.
 */
#ifndef BOOTS_KERN_KMEM_H
#define BOOTS_KERN_KMEM_H

#include <stddef.h>

void *kern_malloc(size_t size);
void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);

#endif
