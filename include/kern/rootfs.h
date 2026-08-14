/*
 * rootfs
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_ROOTFS_H
#define ZEDBSD_KERN_ROOTFS_H

#include "kern/inode.h"

struct filesystem_type;

extern const struct filesystem_type rootfs_type;
int rootfs_add_mountpoint(struct mount *, const char *, struct inode **);
int rootfs_remove_mountpoint(struct inode *);
void rootfs_reset(void);

#endif
