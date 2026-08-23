/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * X68000 native 1024-byte-block partition table.
 */

#ifndef ZEDBSD_KERN_X68K_PARTITION_H
#define ZEDBSD_KERN_X68K_PARTITION_H

#include <kern/partition.h>

#define X68K_PARTITION_BOOT_BYTES	4096U
#define X68K_PARTITION_COUNT	8U

int
x68k_partition_decode(
	const uint8_t *boot_area,
	size_t size,
	uint64_t disk_sectors,
	struct partition *entries,
	unsigned capacity);

extern const struct partition_scheme partition_scheme_x68k;

#endif
