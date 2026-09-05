/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * inode
 */

#ifndef ZEDBSD_KERN_INODE_H
#define ZEDBSD_KERN_INODE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <kern/atomic.h>
#include <kern/lock.h>

/*
 * Strict host C modes hide these non-POSIX feature-test spellings.
 */
#ifndef S_IFMT
#define S_IFMT		0170000U
#define S_IFSOCK	0140000U
#define S_IFLNK		0120000U
#define S_IFREG		0100000U
#define S_IFBLK		0060000U
#define S_IFDIR		0040000U
#define S_IFCHR		0020000U
#define S_IFIFO		0010000U
#endif

#ifndef S_ISUID
#define S_ISUID		0004000U
#endif

#ifndef S_ISGID
#define S_ISGID		0002000U
#endif

#ifndef S_ISVTX
#define S_ISVTX		0001000U
#endif

struct componentname;
struct file_ops;
struct mount;
struct ucred;
struct inode;

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

enum inode_creation_origin {
	INODE_CREATION_INVALID = 0,
	INODE_CREATION_USER,
	INODE_CREATION_SYSTEM,
	INODE_CREATION_PRESERVE,
};

/*
 * Fully resolved attributes for one new inode.  The caller applies umask
 * before constructing this request.  Filesystems must call
 * inode_creation_prepare() after allocating the child and before publishing
 * its directory entry.
 */
struct inode_creation_request {
	enum inode_creation_origin origin;
	enum inode_type type;
	mode_t mode;
	uid_t uid;
	gid_t gid;
	dev_t rdev;
	void *special;
	const struct inode *source;
};

#define INODE_ROOT		0x00000001U
#define INODE_DIRTY		0x00000002U
#define INODE_DEAD		0x00000004U
#define INODE_SWAPFILE		0x00000010U
#define INODE_LOOPFILE		0x00000020U
#define INODE_NOCACHE_CHILDREN	0x00000040U

#define INODE_ATTR_MODE		0x00000001U
#define INODE_ATTR_UID		0x00000002U
#define INODE_ATTR_GID		0x00000004U
#define INODE_ATTR_SIZE		0x00000008U
#define INODE_ATTR_ATIME	0x00000010U
#define INODE_ATTR_MTIME	0x00000020U
#define INODE_ATTR_CTIME	0x00000040U
#define INODE_ATTR_ATIME_NOW	0x00000080U
#define INODE_ATTR_MTIME_NOW	0x00000100U

#define INODE_XATTR_CREATE	0x00000001U
#define INODE_XATTR_REPLACE	0x00000002U
#define INODE_XATTR_NAME_MAX	255U
#define INODE_XATTR_SIZE_MAX	(64U * 1024U)

struct inode_time {
	time_t tv_sec;
	long tv_nsec;
};

/*
 * A truncate request crosses stacking filesystems as one content
 * transaction.  The innermost content inode is authoritative for both the
 * growth-limit comparison and the resulting size.  `actual_size` is valid on
 * every return, including a backend error after a partial mutation.
 */
struct inode_truncate_request {
	off_t size;
	uint64_t growth_limit;
	const struct ucred *credential;
	unsigned content_change;
};

struct inode_truncate_result {
	off_t actual_size;
	int limit_exceeded;
};

struct inode_ops {
	int (*lookup)(struct inode *, const struct componentname *, struct inode **);
	int (*lookup_casefold)(struct inode *, const struct componentname *, struct inode **);
	int (*create)(struct inode *, const struct componentname *,
	    const struct inode_creation_request *, struct inode **);
	int (*mkdir)(struct inode *, const struct componentname *,
	    const struct inode_creation_request *, struct inode **);
	int (*mknod)(struct inode *, const struct componentname *,
	    const struct inode_creation_request *, struct inode **);
	int (*unlink)(struct inode *, const struct componentname *);
	int (*rmdir)(struct inode *, const struct componentname *);
	int (*rename)(struct inode *, const struct componentname *, struct inode *, const struct componentname *, unsigned);
	int (*link)(struct inode *, const struct componentname *, struct inode *);
	int (*symlink)(struct inode *, const struct componentname *, const char *,
	    const struct inode_creation_request *, struct inode **);
	ssize_t (*readlink)(struct inode *, char *, size_t);
	int (*getattr)(struct inode *, struct stat *);
	/* Materialize stacking-filesystem state before metadata/content I/O
	 * locks. Repeated calls do not mutate an already materialized namespace;
	 * an inner update owning i_io_lock must never acquire the namespace gate. */
	int (*prepare_mutation)(struct inode *);
	int (*setattr)(struct inode *, const struct stat *, unsigned);
	int (*truncate)(struct inode *, off_t);
	int (*truncate_limited)(struct inode *, const struct inode_truncate_request *, struct inode_truncate_result *);
	ssize_t (*getxattr)(struct inode *, const char *, void *, size_t);
	int (*setxattr)(struct inode *, const char *, const void *, size_t, unsigned);
	ssize_t (*listxattr)(struct inode *, char *, size_t);
	int (*removexattr)(struct inode *, const char *);
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

	/*
	 * Generic state must not be stored in i_data: that pointer belongs to
	 * the filesystem.  i_special is used by FIFO/socket nodes and the
	 * record-lock pointer is independent of either special-node type.
	 */
	void *i_special;

	void (*i_special_destroy)(void *);
	void *i_record_locks;
	refcount_t i_refs;

	/*
	 * Serializes one externally visible regular-file I/O operation.
	 */
	struct mutex i_io_lock;

	/*
	 * Shared-VM EOF transaction state.  i_vm_lock protects the fields below
	 * and is the condition lock for i_vm_waitq.  The VM object registry and
	 * object lock remain responsible for object lifetime/page state; this
	 * inode-local gate also covers the interval in which no object exists
	 * yet.
	 */
	struct spinlock i_vm_lock;

	struct wait_queue i_vm_waitq;
	unsigned i_vm_resize_active;
	uint64_t i_vm_resize_generation;
	off_t i_vm_resize_old_size;
	off_t i_vm_resize_target_size;

	/*
	 * Every regular-file content transaction, including same-EOF writes,
	 * participates in this gate.  Odd/even is not exposed; generation
	 * merely distinguishes a stale fault/read reservation from the current
	 * owner.
	 */
	unsigned i_vm_content_active;

	unsigned i_vm_content_readers;
	uint64_t i_vm_content_generation;
	off_t i_vm_content_start;
	off_t i_vm_content_end;
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

	/*
	 * Changes whenever this directory's visible namespace is committed.
	 */
	volatile uint64_t i_dirseq;

	unsigned i_flags;
	struct inode *i_hash_next;
	struct inode *i_mount_next;
};

struct inode *
inode_alloc(
	struct mount *mount);

void
inode_free(
	struct inode *inode);

int
inode_get(
	struct mount *mount,
	ino_t ino,
	struct inode **result);

void
inode_ref(
	struct inode *inode);

void
inode_release(
	struct inode *inode);

void
inode_cache_purge_mount(
	struct mount *mount);

int
inode_cache_mount_busy(
	struct mount *mount);

unsigned
inode_cache_mount_count(
	struct mount *mount);

unsigned
inode_cache_count(void);

void
inode_cache_reset(void);

int
inode_lookup(
	struct inode *directory,
	const struct componentname *name,
	struct inode **result);

int
inode_lookup_casefold(
	struct inode *directory,
	const struct componentname *name,
	struct inode **result);

int
inode_getattr(
	struct inode *inode,
	struct stat *status);

int
inode_setattr(
	struct inode *i,
	const struct stat *s,
	unsigned mask);

int
inode_create(
	struct inode *i,
	const struct componentname *n,
	const struct inode_creation_request *request,
	struct inode **r);

int
inode_mkdir(
	struct inode *i,
	const struct componentname *n,
	const struct inode_creation_request *request,
	struct inode **r);

int
inode_mknod(
	struct inode *i,
	const struct componentname *n,
	const struct inode_creation_request *request,
	struct inode **r);

/* Authorize creation and resolve parent-derived attributes under the
 * parent's metadata lock.  The returned request no longer borrows a
 * credential and may be passed to a backend after the lock is released. */
int
inode_creation_request_user(
	struct inode *parent,
	const struct ucred *credential,
	enum inode_type type,
	mode_t mode,
	dev_t rdev,
	void *special,
	struct inode_creation_request *request);

int
inode_creation_request_system(
	enum inode_type type,
	mode_t mode,
	uid_t uid,
	gid_t gid,
	dev_t rdev,
	struct inode_creation_request *request);

int
inode_creation_request_preserve(
	const struct inode *source,
	struct inode_creation_request *request);

int
inode_creation_prepare(
	struct inode *parent,
	struct inode *child,
	const struct inode_creation_request *request);

int
inode_unlink(
	struct inode *i,
	const struct componentname *n);

int
inode_rmdir(
	struct inode *i,
	const struct componentname *n);
/* Namespace mutators join the VFS transaction themselves. Callers doing
 * permission checks must hold it across those checks and the mutation. */
int
inode_rename(
	struct inode *od,
	const struct componentname *on,
	struct inode *nd,
	const struct componentname *nn,
	unsigned flags);

/* Caller holds the VFS transaction; malformed parent chains return EIO. */
int
inode_is_ancestor(
	struct inode *ancestor,
	struct inode *descendant,
	int *result);

int
inode_link(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target);

int
inode_symlink(
	struct inode *directory,
	const struct componentname *name,
	const char *target,
	const struct inode_creation_request *request,
	struct inode **result);

ssize_t
inode_readlink(
	struct inode *inode,
	char *buffer,
	size_t capacity);

int
inode_truncate(
	struct inode *i,
	off_t size);
/*
 * Internal content-owner mutation (for example an overlay upper inode).
 * There is no originating credential at this layer, so executable set-id
 * metadata is invalidated unconditionally in the same I/O transaction.
 */
int
inode_truncate_content_change(
	struct inode *i,
	off_t size);

/*
 * Apply a process growth ceiling in the same i_io transaction as the size
 * change.  limit_exceeded distinguishes RLIMIT_FSIZE from a backend EFBIG.
 */
int
inode_truncate_limited(
	struct inode *i,
	off_t size,
	uint64_t growth_limit,
	int *limit_exceeded);

/*
 * Credential-aware mutation additionally clears executable set-id state
 * while the inode content transaction excludes exec and mapped writers.
 */
int
inode_truncate_limited_cred(
	struct inode *i,
	off_t size,
	uint64_t growth_limit,
	const struct ucred *cred,
	int *limit_exceeded);

/*
 * Stacking backends forward the complete request recursively.
 */
int
inode_truncate_transaction(
	struct inode *i,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result);

ssize_t
inode_getxattr(
	struct inode *inode,
	const char *name,
	void *value,
	size_t size);

int
inode_setxattr(
	struct inode *inode,
	const char *name,
	const void *value,
	size_t size,
	unsigned flags);

ssize_t
inode_listxattr(
	struct inode *inode,
	char *list,
	size_t size);
int
inode_removexattr(
	struct inode *inode,
	const char *name);

int
inode_sync(
	struct inode *i);

void
inode_touch(
	struct inode *inode,
	unsigned mask);

void
inode_dir_changed(
	struct inode *inode);

mode_t
inode_type_mode(
	enum inode_type type);

#endif
