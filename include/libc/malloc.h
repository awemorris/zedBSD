/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The allocator's header of the BSD and GNU libraries, which is not in
 * POSIX: the functions of <stdlib.h>, and two more.
 *
 * memalign() allocates a block at an address that is a multiple of a power
 * of two (posix_memalign() and aligned_alloc() are the standard ways), and
 * malloc_usable_size() reports how many bytes of a block its owner may
 * use, at least the size it asked for.  A block from either is freed with
 * free().
 */

#ifndef LIBC_MALLOC_H
#define LIBC_MALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdlib.h>

void *memalign(size_t alignment, size_t size);
size_t malloc_usable_size(void *pointer);

#ifdef __cplusplus
}
#endif

#endif
