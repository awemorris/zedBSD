/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The copy and the fill GCC calls for a structure's assignment or
 * initialization, which the second stage has no C library to take from.
 */

#include "bootloader/mac68k/stage2.h"

void *memcpy(void *target, const void *source, size_t length);
void *memset(void *target, int value, size_t length);

/*
 * Copies bytes between buffers that do not overlap.
 */
void *
memcpy(
	void *target,
	const void *source,
	size_t length)
{
	/* Succeeded: the target, as the C library's reports it. */
	mac68k_copy_bytes(target, source, (uint32_t)length);
	return target;
}

/*
 * Fills a buffer with one byte value.
 */
void *
memset(
	void *target,
	int value,
	size_t length)
{
	/* Succeeded: the target, as the C library's reports it. */
	mac68k_fill_bytes(target, value, (uint32_t)length);
	return target;
}
