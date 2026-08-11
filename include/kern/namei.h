/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

#define COMPONENT_LAST   0x0001U
#define COMPONENT_DOT    0x0002U
#define COMPONENT_DOTDOT 0x0004U

struct componentname {
	const char *cn_nameptr;
	size_t cn_namelen;
	unsigned cn_flags;
};

struct fs_context {
	struct inode *fc_root;
	struct inode *fc_cwd;
	char fc_cwd_path[BOOTS_PATH_MAX];
};

int namei_at(struct fs_context *, const char *, struct inode **);
int namei_parent_at(struct fs_context *, const char *, struct inode **,
		    struct componentname *, char [NAME_MAX + 1U]);
int fs_context_init(struct fs_context *, struct inode *);
void fs_context_destroy(struct fs_context *);
int fs_chdir(struct fs_context *, const char *);
const char *fs_getcwd(const struct fs_context *);

#endif
