/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * rootfs
 */

#ifndef ZEDBSD_KERN_ROOTFS_H
#define ZEDBSD_KERN_ROOTFS_H

#include "kern/inode.h"

struct filesystem_type;

extern const struct filesystem_type rootfs_type;

int
rootfs_add_mountpoint(
	struct mount *root_mount,
	const char *name,
	struct inode **result);

int
rootfs_remove_mountpoint(
	struct inode *inode);

void
rootfs_reset(void);

#endif
