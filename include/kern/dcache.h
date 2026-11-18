/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Data cache maintenance for drivers.
 *
 * A device that does not snoop the CPU caches reads memory as it is in RAM
 * and writes it behind the caches.  Before such a device reads a buffer the
 * CPU wrote, the buffer is cleaned; before the CPU reads what the device
 * wrote, the buffer is invalidated.  Drivers reach the HAL's cache operations
 * through these calls.  Only architectures whose HAL maintains the data cache
 * build them.
 */

#ifndef KERN_KERN_DCACHE_H
#define KERN_KERN_DCACHE_H

#include <stddef.h>

/* Writes every dirty cache line of a range back to memory. */
void kern_dcache_clean_range(const void *address, size_t size);

/*
 * Discards every cache line of a range, writing back dirty bytes first, so the
 * next read comes from memory.
 */
void kern_dcache_invalidate_range(const void *address, size_t size);

#endif
