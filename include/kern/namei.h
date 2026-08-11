/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_KERN_NAMEI_H
#define BOOTS_KERN_NAMEI_H

#include "kern/inode.h"
#include <limits.h>

#ifndef NAME_MAX
#define NAME_MAX 255U
#endif
#ifndef PATH_MAX
#define PATH_MAX 256U
#endif
#ifndef BOOTS_PATH_MAX
#define BOOTS_PATH_MAX PATH_MAX
#endif

#define COMPONENT_LAST 0x0001U
#define COMPONENT_DOT 0x0002U
#define COMPONENT_DOTDOT 0x0004U

struct componentname {
	const char *cn_nameptr;
	size_t cn_namelen;
	unsigned cn_flags;
};

struct cwdinfo {
	unsigned usecount;
	unsigned flags;
	struct inode *root;
	struct inode *cwd;
	char cwd_path[BOOTS_PATH_MAX];
};

int namei_at(struct cwdinfo *, const char *, struct inode **);
int namei_parent_at(struct cwdinfo *, const char *, struct inode **,
		    struct componentname *, char [NAME_MAX + 1U]);
int cwdinfo_init(struct cwdinfo *, struct inode *);
int cwdinfo_clone(const struct cwdinfo *, struct cwdinfo **);
void cwdinfo_retain(struct cwdinfo *);
void cwdinfo_release(struct cwdinfo *);
void cwdinfo_destroy(struct cwdinfo *);
int fs_chdir(struct cwdinfo *, const char *);
const char *fs_getcwd(const struct cwdinfo *);

#endif
