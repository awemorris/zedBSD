/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * name cache
 */

#ifndef ZEDBSD_KERN_NAMECACHE_H
#define ZEDBSD_KERN_NAMECACHE_H

#include "kern/inode.h"

#define NAMECACHE_MAX	128U

int
namecache_lookup(
	struct inode *parent,
	const struct componentname *name,
	struct inode **result);

/*
 * The sequence is sampled before the filesystem lookup.  EAGAIN means a
 * namespace mutation superseded that lookup, so its result was not cached.
 */
int
namecache_enter(
	struct inode *parent,
	const struct componentname *name,
	struct inode *child,
	uint64_t observed_sequence);

void
namecache_remove(
	struct inode *parent,
	const struct componentname *name);

void
namecache_purge_inode(
	struct inode *inode);
void
namecache_purge_mount(
	struct mount *mountp);

void
namecache_reset(void);

unsigned
namecache_count(void);

#endif
