/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_KERN_FAT_VFS_H
#define BOOTS_KERN_FAT_VFS_H

#include "kern/file.h"
#include "kern/mount.h"

struct fat_inode_info {
	struct inode fi_inode;
	uint32_t fi_first_cluster;
	uint32_t fi_dirent_lba;
	uint16_t fi_dirent_offset;
	uint8_t fi_attributes;
	uint8_t fi_flags;
};

static inline struct fat_inode_info *fat_inode(struct inode *inode)
{
	return (struct fat_inode_info *)inode;
}

extern const struct filesystem_type fat_filesystem_type;
int fat_file_contiguous_block(struct file *, struct disk **, uint64_t *);

#endif
