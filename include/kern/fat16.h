/*
 * Boots FAT12/FAT16 driver
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_FAT16_H
#define BOOTS_FAT16_H

#include "kern/fs.h"

extern const struct boots_filesystem_driver boots_fat12_driver;
extern const struct boots_filesystem_driver boots_fat16_driver;

enum boots_fs_result boots_fat16_stat_location(
	struct boots_filesystem *, const char *, struct boots_dirent *,
	uint32_t *, uint16_t *, uint32_t *, uint8_t *);

#endif
