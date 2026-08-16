/*
 * name cache
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_NAMECACHE_H
#define ZEDBSD_KERN_NAMECACHE_H

#include "kern/inode.h"

#define NAMECACHE_MAX 128U

int namecache_lookup(struct inode *, const struct componentname *,
		     struct inode **);
int namecache_enter(struct inode *, const struct componentname *,
		    struct inode *);
void namecache_remove(struct inode *, const struct componentname *);
void namecache_purge_inode(struct inode *);
void namecache_purge_mount(struct mount *);
void namecache_reset(void);
unsigned namecache_count(void);

#endif
