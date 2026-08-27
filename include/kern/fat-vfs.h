/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * VFAT
 */

#ifndef ZEDBSD_KERN_FAT_VFS_H
#define ZEDBSD_KERN_FAT_VFS_H

#include "kern/file.h"
#include "kern/fat.h"
#include "kern/mount.h"

struct fat_inode_info {
	struct inode fi_inode;
	uint32_t fi_first_cluster;
	uint32_t fi_dirent_lba;
	uint16_t fi_dirent_offset;
	uint8_t fi_attributes;
	uint8_t fi_flags;
};

static inline struct fat_inode_info *
fat_inode(
	struct inode *inode)
{
	return (struct fat_inode_info *)inode;
}

extern const struct filesystem_type fat_filesystem_type;

int
fat_probe_type(
	struct disk *disk,
	enum bootfat_type *type);
typedef int (
	*fat_extent_cb)(
	uint64_t,
	uint64_t,
	uint32_t,
	void *);

int
fat_file_extents(
	struct file *file,
	fat_extent_cb callback,
	void *context);

int
fat_file_contiguous_block(
	struct file *file,
	struct disk **disk,
	uint64_t *block);

#endif
