/*
 * zedBSD disk-label driver schemes.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DRIVERS_DISKLABEL_H
#define ZEDBSD_DRIVERS_DISKLABEL_H

#include <kern/partition.h>

#define X68K_PARTITION_BOOT_BYTES	4096U
#define X68K_PARTITION_COUNT		8U

extern const struct partition_scheme partition_scheme_mbr;
extern const struct partition_scheme partition_scheme_gpt;
extern const struct partition_scheme partition_scheme_pcat_auto;
extern const struct partition_scheme partition_scheme_pc98;
extern const struct partition_scheme partition_scheme_pc98_auto;
extern const struct partition_scheme partition_scheme_sun;
extern const struct partition_scheme partition_scheme_x68k;

int
x68k_partition_decode(
	const uint8_t *boot_area,
	size_t size,
	uint64_t disk_sectors,
	struct partition *entries,
	unsigned capacity);

#endif
