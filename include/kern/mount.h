/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mount
 */

#ifndef ZEDBSD_KERN_MOUNT_H
#define ZEDBSD_KERN_MOUNT_H

#include "kern/disk.h"
#include "kern/atomic.h"
#include "kern/backing-claim.h"
#include "kern/lock.h"
#include "kern/waitq.h"
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX		256U
#endif
#ifndef NAME_MAX
#define NAME_MAX		255U
#endif
#ifndef ZEDBSD_PATH_MAX
#define ZEDBSD_PATH_MAX		PATH_MAX
#endif

#define MOUNT_MAX		64U
#define MOUNT_READ_ONLY		0x00000001U
#define MOUNT_NOSUID		0x00000002U
#define MOUNT_PRIVATE_INTERNAL	0x00000002U
#define FILESYSTEM_NODEV	0x00000001U

struct inode;
struct mount;
struct componentname;
struct dirent;
struct statvfs;
struct quota_control;
struct snapshot_control;
struct block_identity;

struct path {
	struct mount *p_mount;
	struct inode *p_inode;
};

enum mount_state {
	MOUNT_STATE_FREE = 0,
	MOUNT_STATE_PREPARING,
	MOUNT_STATE_LIVE,
	MOUNT_STATE_DYING,
	MOUNT_STATE_DEAD,
};

struct filesystem_type {
	const char *fs_name;
	unsigned fs_flags;
	int (*probe)(struct disk *);
	/*
	 * Return 0 after recognizing the disk and filling only filesystem-owned
	 * TYPE/UUID/LABEL metadata, or EOPNOTSUPP for a format mismatch.  Other
	 * errno values report bounded metadata-read or validation failures.
	 */
	int (*identify)(struct disk *, struct block_identity *);
	int (*mount)(struct mount *);
	int (*sync)(struct mount *);
	int (*statvfs)(struct mount *, struct statvfs *);
	int (*quotactl)(struct mount *, struct quota_control *);
	int (*snapshotctl)(struct mount *, struct snapshot_control *);

	/*
	 * Last failure-capable step before namespace/inode state is destroyed.
	 */
	int (*prepare_unmount)(struct mount *);
	void (*unmount)(struct mount *);
	struct inode *(*alloc_inode)(struct mount *);
	void (*free_inode)(struct inode *);
};

struct mount {
	char m_path[ZEDBSD_PATH_MAX];
	char m_name[NAME_MAX + 1U];
	unsigned m_flags;
	refcount_t m_refs;
	struct mutex m_lock;
	/*
	 * Serializes generic permission/type checks with one namespace commit.
	 * Filesystem-private namespace locks are acquired inside this lock.
	 */
	struct mutex m_vfs_transaction_storage;
	struct mutex *m_vfs_transaction_lock;
	struct wait_queue m_waitq;
	enum mount_state m_state;
	unsigned m_internal_flags;
	/* Held from writable mount preparation through LIVE publication. */
	struct backing_mutation_guard m_backing_guard;
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

struct fat_mount_args {
	const char *fspec;
};

int
filesystem_register(
	const struct filesystem_type *type);

/* Dispatch registered identity callbacks without mounting the disk. */
int
filesystem_identify(
	struct disk *disk,
	struct block_identity *identity);

void
mount_reset(void);

void
path_init(
	struct path *path);

void
mount_ref(
	struct mount *mountp);

void
mount_release(
	struct mount *mountp);

void
path_set(
	struct path *path,
	struct mount *mountp,
	struct inode *inode);

void
path_ref(
	struct path *path);

void
path_release(
	struct path *path);

int
path_equal(
	const struct path *left,
	const struct path *right);

int
mount_rootfs(void);

struct inode *
mount_root_inode(void);

int
mount_root_create(
	const char *type_name,
	int flags,
	void *data,
	struct mount **result);

struct mount *
mount_root_get_ref(void);

int
mount_at(
	const char *type_name,
	const struct path *directory,
	const char *name,
	int flags,
	void *data,
	struct mount **result);

int
mount_bind_at(
	const struct path *source,
	const struct path *directory,
	const char *name,
	struct mount **result);

int
mount_private(
	const char *type_name,
	struct disk *disk,
	int flags,
	void *data,
	struct mount **result);

int
mount_private_lookup(
	struct mount *mountp,
	const char *relative,
	struct path *result);

int
mount_private_promote_root(
	struct mount *mountp,
	struct mount **result);

int
unmount_private(
	struct mount *mountp);

int
mount_is_private(
	const struct mount *mountp);

int
mount_sync(
	struct mount *mountp);

int
mount_sync_all(void);

int
mount_disk_writable_busy(
	struct disk *disk);

int
mount_statvfs(
	struct mount *mountp,
	struct statvfs *result);

int
mount_quotactl(
	struct mount *mountp,
	struct quota_control *request);

int
mount_snapshotctl(
	struct mount *mountp,
	struct snapshot_control *request);

int
mount(
	const char *type_name,
	const char *dir,
	int flags,
	void *data);

int
unmount(
	const char *dir,
	int flags);

struct mount *
mount_find_ref(
	const char *path);

struct mount *
mount_for_inode(
	const struct inode *inode);

int
mount_follow(
	struct inode *inode,
	struct inode **result);

int
mount_cross_parent(
	struct inode *inode,
	struct inode **result);

int
mount_lookup_child(
	const struct path *directory,
	const struct componentname *component,
	struct path *result);

int
mount_cross_path_parent(
	const struct path *current,
	struct path *result);

int
mount_readdir_child(
	const struct path *directory,
	unsigned *cursor,
	struct dirent *entry);

int
mount_child_shadows(
	const struct path *directory,
	const char *name);

void
mount_vfs_transaction_enter(
	struct mount *mountp);

void
mount_vfs_transaction_leave(
	struct mount *mountp);

unsigned
mount_count(void);

#endif
