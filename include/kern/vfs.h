/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_KERN_VFS_H
#define BOOTS_KERN_VFS_H

#include "kern/boot.h"
#include "kern/namei.h"

extern struct fs_context kern_fs_context;
int kern_vfs_init(const struct boots_handoff *, const struct boots_device *,
		  unsigned);

#endif
