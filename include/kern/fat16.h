/*
 * FAT12/FAT16 driver
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_FAT16_H
#define ZEDBSD_FAT16_H

#include "kern/fs.h"

extern const struct zedbsd_filesystem_driver zedbsd_fat12_driver;
extern const struct zedbsd_filesystem_driver zedbsd_fat16_driver;

typedef int (*zedbsd_fat_extent_cb)(uint64_t, uint64_t, uint32_t, void *);
int zedbsd_fat_file_extents(struct zedbsd_file *, zedbsd_fat_extent_cb,
			    void *);

enum zedbsd_fs_result zedbsd_fat_stat_location(
	struct zedbsd_filesystem *, const char *, struct zedbsd_dirent *,
	uint32_t *, uint16_t *, uint32_t *, uint8_t *);
enum zedbsd_fs_result zedbsd_fat_stat_location_casefold(
	struct zedbsd_filesystem *, const char *, struct zedbsd_dirent *,
	uint32_t *, uint16_t *, uint32_t *, uint8_t *);

#endif
