/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_KERN_MOUNT_H
#define BOOTS_KERN_MOUNT_H

#include "kern/disk.h"
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 256U
#endif
#ifndef BOOTS_PATH_MAX
#define BOOTS_PATH_MAX PATH_MAX
#endif

#define MOUNT_MAX 64U
#define MOUNT_READ_ONLY 0x00000001U

struct inode;
struct mount;

struct filesystem_type {
	const char *fs_name;
	int (*probe)(struct disk *);
	int (*mount)(struct mount *);
	int (*unmount)(struct mount *);
	struct inode *(*alloc_inode)(struct mount *);
	void (*free_inode)(struct inode *);
};

struct mount {
	char m_path[BOOTS_PATH_MAX];
	unsigned m_flags;
	struct disk *m_disk;
	const struct filesystem_type *m_type;
	struct inode *m_root;
	struct inode *m_mountpoint;
	struct mount *m_parent;
	void *m_data;
	struct mount *m_next;
};

struct fat_mount_args { const char *fspec; };

int filesystem_register(const struct filesystem_type *);
void mount_reset(void);
int mount_rootfs(void);
struct inode *mount_root_inode(void);
int mount(const char *, const char *, int, void *);
int unmount(const char *, int);
struct mount *mount_find(const char *);
struct mount *mount_for_inode(const struct inode *);
int mount_follow(struct inode *, struct inode **);
int mount_cross_parent(struct inode *, struct inode **);

#endif
