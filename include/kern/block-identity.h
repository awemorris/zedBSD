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
	struct disk *,
	struct block_identity *);

int
block_identity_resolve(
	const char *,
	struct disk **);

#endif
