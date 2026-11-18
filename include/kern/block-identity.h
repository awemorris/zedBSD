/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Block device filesystem and partition identity.
 */

#ifndef KERN_KERN_BLOCK_IDENTITY_H
#define KERN_KERN_BLOCK_IDENTITY_H

#include <kern/disk.h>
#include <uapi/blkid.h>

int
block_identity_get(
	struct disk *disk,
	struct block_identity *id);

int
block_identity_resolve(
	const char *selector,
	struct disk **result);

#endif
