/*
 * zedBSD FAT family VFS interface
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_FAT_H
#define ZEDBSD_FAT_H

#include "kern/file.h"
#include "kern/mount.h"

#include <stdint.h>

enum bootfat_type {
	ZEDBSD_FAT12 = 12,
	ZEDBSD_FAT16 = 16,
	ZEDBSD_FAT32 = 32,
};

extern const struct filesystem_type fat_filesystem_type;

int fat_probe_type(struct disk *disk, enum bootfat_type *type);

typedef int (*fat_extent_cb)(uint64_t, uint64_t, uint32_t, void *);

int fat_file_extents(struct file *file, fat_extent_cb callback, void *context);

int fat_file_contiguous_block(struct file *file, struct disk **disk,
			      uint64_t *block);

int fat_file_backing_identity(struct inode *inode, struct disk **disk,
			      uint64_t *object);

#endif
