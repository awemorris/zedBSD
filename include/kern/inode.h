/*
 * inode
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_INODE_H
#define ZEDBSD_KERN_INODE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <kern/atomic.h>
#include <kern/lock.h>

/* Strict host C modes hide these non-POSIX feature-test spellings. */
#ifndef S_IFMT
#define S_IFMT   0170000U
#define S_IFSOCK 0140000U
#define S_IFLNK  0120000U
#define S_IFREG  0100000U
#define S_IFBLK  0060000U
#define S_IFDIR  0040000U
#define S_IFCHR  0020000U
#define S_IFIFO  0010000U
#endif
#ifndef S_ISUID
#define S_ISUID 0004000U
#endif
#ifndef S_ISGID
#define S_ISGID 0002000U
#endif
#ifndef S_ISVTX
#define S_ISVTX 0001000U
#endif

struct componentname;
struct file_ops;
struct mount;

enum inode_type {
	INODE_NONE,
	INODE_REG,
	INODE_DIR,
	INODE_BLOCK,
	INODE_CHAR,
	INODE_SYMLINK,
	INODE_SOCKET,
	INODE_FIFO,
};

#define INODE_ROOT       0x00000001U
#define INODE_DIRTY      0x00000002U
#define INODE_DEAD       0x00000004U
#define INODE_MOUNTPOINT 0x00000008U
#define INODE_SWAPFILE   0x00000010U
#define INODE_LOOPFILE   0x00000020U
#define INODE_NOCACHE_CHILDREN 0x00000040U

#define INODE_ATTR_MODE       0x00000001U
#define INODE_ATTR_UID        0x00000002U
#define INODE_ATTR_GID        0x00000004U
#define INODE_ATTR_SIZE       0x00000008U
#define INODE_ATTR_ATIME      0x00000010U
#define INODE_ATTR_MTIME      0x00000020U
#define INODE_ATTR_CTIME      0x00000040U
#define INODE_ATTR_ATIME_NOW  0x00000080U
#define INODE_ATTR_MTIME_NOW  0x00000100U

struct inode;

struct inode_time {
	time_t tv_sec;
	long tv_nsec;
};

struct inode_ops {
	int (*lookup)(struct inode *, const struct componentname *,
		      struct inode **);
	int (*lookup_casefold)(struct inode *, const struct componentname *,
			       struct inode **);
	int (*create)(struct inode *, const struct componentname *, mode_t,
		      struct inode **);
	int (*mkdir)(struct inode *, const struct componentname *, mode_t,
		     struct inode **);
	int (*unlink)(struct inode *, const struct componentname *);
	int (*rmdir)(struct inode *, const struct componentname *);
	int (*rename)(struct inode *, const struct componentname *,
		      struct inode *, const struct componentname *, unsigned);
	int (*link)(struct inode *, const struct componentname *, struct inode *);
	int (*symlink)(struct inode *, const struct componentname *, const char *,
		       struct inode **);
	ssize_t (*readlink)(struct inode *, char *, size_t);
	int (*getattr)(struct inode *, struct stat *);
	int (*setattr)(struct inode *, const struct stat *, unsigned);
	int (*truncate)(struct inode *, off_t);
	int (*sync)(struct inode *);
	void (*reclaim)(struct inode *);
};

struct inode {
	enum inode_type i_type;
	ino_t i_ino;
	struct mount *i_mount;
	const struct inode_ops *i_op;
	const struct file_ops *i_fop;
	void *i_data;
	refcount_t i_refs;
	struct mutex i_lock;
	nlink_t i_linkcount;
	mode_t i_mode;
	uid_t i_uid;
	gid_t i_gid;
	off_t i_size;
	dev_t i_rdev;
	struct inode_time i_atime;
	struct inode_time i_mtime;
	struct inode_time i_ctime;
	/* Changes whenever this directory's visible namespace is committed. */
	uint64_t i_dirseq;
	unsigned i_flags;
	struct inode *i_hash_next;
	struct inode *i_mount_next;
};

struct inode *inode_alloc(struct mount *mount);
void inode_free(struct inode *inode);
int inode_get(struct mount *mount, ino_t ino, struct inode **result);
void inode_ref(struct inode *inode);
void inode_release(struct inode *inode);
void inode_cache_purge_mount(struct mount *mount);
int inode_cache_mount_busy(struct mount *mount);
unsigned inode_cache_mount_count(struct mount *mount);
void inode_cache_reset(void);

int inode_lookup(struct inode *, const struct componentname *, struct inode **);
int inode_lookup_casefold(struct inode *, const struct componentname *,
			  struct inode **);
int inode_getattr(struct inode *, struct stat *);
int inode_setattr(struct inode *, const struct stat *, unsigned);
int inode_create(struct inode *, const struct componentname *, mode_t,
		 struct inode **);
int inode_mkdir(struct inode *, const struct componentname *, mode_t,
		struct inode **);
int inode_unlink(struct inode *, const struct componentname *);
int inode_rmdir(struct inode *, const struct componentname *);
int inode_rename(struct inode *, const struct componentname *, struct inode *,
		 const struct componentname *, unsigned);
int inode_link(struct inode *, const struct componentname *, struct inode *);
int inode_symlink(struct inode *, const struct componentname *, const char *,
		  struct inode **);
ssize_t inode_readlink(struct inode *, char *, size_t);
int inode_truncate(struct inode *, off_t);
int inode_sync(struct inode *);
void inode_touch(struct inode *, unsigned);
void inode_dir_changed(struct inode *);

mode_t inode_type_mode(enum inode_type type);

#endif
