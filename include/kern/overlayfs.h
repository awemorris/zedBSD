/*
 * Overlay filesystem
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_KERN_OVERLAYFS_H
#define ZEDBSD_KERN_OVERLAYFS_H

#include "kern/mount.h"

enum overlay_mount_flags {
	OVERLAY_READ_ONLY = 0x0001U,
	OVERLAY_READ_WRITE = 0x0002U,
};

struct overlay_mount_args {
	struct path upper;
	struct path lower;
	unsigned flags;
};

int overlayfs_init(void);
int overlay_mount_at(struct mount *, const char *,
		     const struct overlay_mount_args *, struct mount **);

#endif
