/*
 * zedBSD FAT family VFS interface
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_FAT_H
#define KERN_FAT_H

#include "kern/boot.h"
#include "kern/file.h"
#include "kern/mount.h"

#include <stdint.h>

/* enum bootfat_type is declared in <kern/boot.h>; the boot source names it. */

extern const struct filesystem_type drv_fat_filesystem_type;

/*
 * Tells whether a filesystem type is FAT's.
 *
 * A kernel built without FAT has no FAT mount, so there the answer is
 * always no and nothing names the absent type.
 */
static inline int
drv_fat_is_filesystem_type(
	const struct filesystem_type *type)
{
#if CONFIG_DRIVER_FS_FAT
	/* The type is FAT's own. */
	if (type == &drv_fat_filesystem_type)
		return 1;
#else
	(void)type;
#endif

	/* Any other type, or none. */
	return 0;
}

int drv_fat_probe_type(struct disk *disk, enum bootfat_type *type);

typedef file_extent_cb fat_extent_cb;

int drv_fat_file_extents(struct file *file, fat_extent_cb callback, void *context);

struct fat_loop_extent {
	uint64_t file_block, disk_block;
	uint32_t count;
};
/* Borrowed immutable map; caller owns it through unbind. Ordinary file I/O
 * still owns the VM/content lease, FAT owns slot coherence and parent I/O. */
int drv_fat_file_set_loop_map(struct file *file,
	const struct fat_loop_extent *map, unsigned count);

int drv_fat_file_backing_identity(struct inode *inode, struct disk **disk,
			      uint64_t *object);

#endif
