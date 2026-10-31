/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The host test's stand-in for the kernel's mount header: only the sync of every volume. */

#ifndef KERN_KERN_MOUNT_H
#define KERN_KERN_MOUNT_H

int
mount_sync_all(void);

#endif
