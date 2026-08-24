/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * FAT12/FAT16 driver
 */

#ifndef ZEDBSD_FAT16_H
#define ZEDBSD_FAT16_H

#include "kern/fs.h"

extern const struct bootfs_driver bootfat12_driver;
extern const struct bootfs_driver bootfat16_driver;

typedef int (*bootfat_extent_fn)(uint64_t, uint64_t, uint32_t, void *);

int
bootfat_file_extents(
	struct bootfs_file *file,
	bootfat_extent_fn callback,
	void *context);

enum bootfs_result
bootfat_stat_location(
	struct bootfs *filesystem,
	const char *path,
	struct bootfs_dirent *entry,
	uint32_t *lba,
	uint16_t *offset,
	uint32_t *first_cluster,
	uint8_t *attributes);

enum bootfs_result
bootfat_stat_location_casefold(
	struct bootfs *filesystem,
	const char *path,
	struct bootfs_dirent *entry,
	uint32_t *lba,
	uint16_t *offset,
	uint32_t *first_cluster,
	uint8_t *attributes);

enum bootfs_result
bootfat_discard_chain_result(
	struct bootfs *filesystem,
	uint32_t first_cluster);

#endif
