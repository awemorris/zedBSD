/*
 * FAT12/FAT16 driver
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_FAT16_H
#define ZEDBSD_FAT16_H

#include "kern/fs.h"

extern const struct bootfs_driver bootfat12_driver;
extern const struct bootfs_driver bootfat16_driver;

typedef int (*bootfat_extent_fn)(uint64_t, uint64_t, uint32_t, void *);
int bootfat_file_extents(struct bootfs_file *, bootfat_extent_fn,
			    void *);

enum bootfs_result bootfat_stat_location(
	struct bootfs *, const char *, struct bootfs_dirent *,
	uint32_t *, uint16_t *, uint32_t *, uint8_t *);
enum bootfs_result bootfat_stat_location_casefold(
	struct bootfs *, const char *, struct bootfs_dirent *,
	uint32_t *, uint16_t *, uint32_t *, uint8_t *);
enum bootfs_result bootfat_discard_chain_result(
	struct bootfs *, uint32_t);

#endif
