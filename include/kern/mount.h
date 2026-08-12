/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_MOUNT_H
#define ZEDBSD_KERN_MOUNT_H

#include "kern/disk.h"
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 256U
#endif
#ifndef NAME_MAX
#define NAME_MAX 255U
#endif
#ifndef ZEDBSD_PATH_MAX
#define ZEDBSD_PATH_MAX PATH_MAX
#endif

#define MOUNT_MAX 64U
#define MOUNT_READ_ONLY 0x00000001U
#define FILESYSTEM_NODEV 0x00000001U

struct inode;
struct mount;
struct componentname;
struct dirent;

struct path {
	struct mount *p_mount;
	struct inode *p_inode;
};

struct filesystem_type {
	const char *fs_name;
	unsigned fs_flags;
	int (*probe)(struct disk *);
	int (*mount)(struct mount *);
	int (*unmount)(struct mount *);
	struct inode *(*alloc_inode)(struct mount *);
	void (*free_inode)(struct inode *);
};

struct mount {
	char m_path[ZEDBSD_PATH_MAX];
	char m_name[NAME_MAX + 1U];
	unsigned m_flags;
	unsigned m_usecount;
	unsigned m_internal_flags;
	struct disk *m_disk;
	const struct filesystem_type *m_type;
	struct inode *m_root;
	struct inode *m_mountpoint;
	struct mount *m_parent;
	struct path m_cover;
	struct mount *m_bind_source;
	struct mount *m_children;
	struct mount *m_sibling;
	void *m_data;
	struct mount *m_next;
};

struct fat_mount_args { const char *fspec; };

int filesystem_register(const struct filesystem_type *);
void mount_reset(void);
void path_init(struct path *);
void path_set(struct path *, struct mount *, struct inode *);
void path_ref(struct path *);
void path_release(struct path *);
int path_equal(const struct path *, const struct path *);
int mount_rootfs(void);
struct inode *mount_root_inode(void);
int mount_root_create(const char *, int, void *, struct mount **);
struct mount *mount_root_get(void);
int mount_at(const char *, const struct path *, const char *, int, void *,
	     struct mount **);
int mount_bind_at(const struct path *, const struct path *, const char *,
		  struct mount **);
int mount(const char *, const char *, int, void *);
int unmount(const char *, int);
struct mount *mount_find(const char *);
struct mount *mount_for_inode(const struct inode *);
int mount_follow(struct inode *, struct inode **);
int mount_cross_parent(struct inode *, struct inode **);
int mount_lookup_child(const struct path *, const struct componentname *,
		       struct path *);
int mount_cross_path_parent(const struct path *, struct path *);
int mount_readdir_child(const struct path *, unsigned *, struct dirent *);
int mount_child_shadows(const struct path *, const char *);

#endif
