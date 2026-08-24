/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Block device filesystem and partition identity.
 */

#ifndef ZEDBSD_KERN_BLOCK_IDENTITY_H
#define ZEDBSD_KERN_BLOCK_IDENTITY_H

#include <kern/disk.h>
#include <zedbsd/blkid.h>

int
block_identity_get(
	struct disk *disk,
	struct block_identity *id);

int
block_identity_resolve(
	const char *selector,
	struct disk **result);

#endif
