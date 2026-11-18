/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Data cache maintenance for drivers.
 *
 * Built only for architectures whose HAL implements hal_dcache_*.
 */

#include <kern/dcache.h>
#include <hal/hal.h>

/*
 * Writes every dirty cache line of a range back to memory.
 */
void
kern_dcache_clean_range(
	const void *address,
	size_t size)
{
	/* Cleans the range to the point of coherency. */
	hal_dcache_clean_range((uintptr_t)address, size);
}

/*
 * Discards every cache line of a range so the next read comes from memory.
 */
void
kern_dcache_invalidate_range(
	const void *address,
	size_t size)
{
	/* Cleans and invalidates, so bytes sharing an edge line are kept. */
	hal_dcache_invalidate_range((uintptr_t)address, size);
}
