/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_KERN_VFS_H
#define ZEDBSD_KERN_VFS_H

#include "kern/boot.h"
#include "kern/namei.h"

extern struct cwdinfo kern_cwdinfo;
int kern_vfs_init(const struct zedbsd_handoff *, const struct zedbsd_device *,
		  unsigned);

#endif
