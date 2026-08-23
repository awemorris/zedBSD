/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * VFS
 */

#ifndef ZEDBSD_KERN_VFS_H
#define ZEDBSD_KERN_VFS_H

#include "kern/boot.h"
#include "kern/namei.h"

extern struct cwdinfo kern_cwdinfo;

int
kern_vfs_init(
	const struct boot_handoff *,
	const struct boot_device *,
	unsigned);

#endif
