/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * inode
 */

#ifndef KERN_KERN_INODE_H
#define KERN_KERN_INODE_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/stat.h>
#include <uapi/types.h>
#include <kern/atomic.h>
#include <kern/io-error.h>
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

/* An i_descriptor_alias value: /dev/fd/N, a link to what N holds. */
#define INODE_DESCRIPTOR_ALIAS_NUMBER	1U

/* An i_descriptor_alias value: /dev/stdin and the like, links to fd/N. */
#define INODE_DESCRIPTOR_ALIAS_STANDARD	2U

struct componentname;
struct file_ops;
struct io_context;
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

/* How an inode_data_extent's bytes are held: inode_data_extent.kind. */
#define INODE_DATA_HOLE		0U	/* no block; the bytes read as zeroes */
#define INODE_DATA_MAPPED	1U	/* an allocated block holds the bytes */
#define INODE_DATA_NEW		2U	/* a block this call allocated; what the disk holds there is not the file's */

/* inode_data_extent.flags. */
/* The run shares a cache line of its disk with another owner's bytes, or does not start or end on a line. */
#define INODE_DATA_SHARED_LINE	0x00000001U
/*
 * Writing the run back: the file system has to write these bytes itself (a
 * snapshot keeps their old contents, or the journal carries them), so the
 * page cache writes them through the file's write.
 */
#define INODE_DATA_THROUGH_FILE	0x00000002U
/*
 * Writing the run back: the file system orders the run's new blocks before
 * a commit of its journal, which waits for the write; the caller reports
 * the end of its write of the run through data_written().
 */
#define INODE_DATA_ORDERED	0x00000004U

/*
 * How data_map() is asked to map: to read the bytes; to write new bytes
 * into the cache's pages (the holes of the run are allocated, and the
 * file's size grows to the end of the run); or to write the bytes of the
 * cache's pages back over their blocks (no block is allocated; a hole
 * stays a hole, which the file's write fills).
 */
#define INODE_DATA_READ		0U
#define INODE_DATA_ALLOCATE	1U
#define INODE_DATA_WRITEBACK	2U

/*
 * One run of a regular file's bytes as data_map() reports it: the bytes
 * [offset, offset + length) of the file, held by a hole or by the
 * consecutive 512-byte sectors of the mount's disk that start at sector.
 */
struct inode_data_extent {
	off_t offset;
	uint64_t length;
	uint64_t sector;
	unsigned kind;
	unsigned flags;
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
	/* Infallible: drops directory-entry owners after final unmount admission. */
	void (*retire_namespace)(struct inode *);

	/*
	 * Maps the regular file's bytes that start at offset (on a 512-byte
	 * boundary, before the end of file) to one run of at most length
	 * bytes, so that the VM page cache moves them between its pages and
	 * the mount's disk itself.  The run is never empty and never reaches
	 * past the end of file; the caller asks again for the rest.  Only the
	 * holder of the inode's i_io_lock asks.  EOPNOTSUPP when the file's
	 * data does not go through the page cache this way (the caller then
	 * reads, or writes, through the file's own read or write).  To write
	 * back, EROFS when the volume takes no write.
	 */
	int (*data_map)(struct inode *, off_t offset, uint64_t length, unsigned how,
	    const struct io_context *, struct inode_data_extent *);

	/*
	 * Ends the write-back of a run that data_map(INODE_DATA_WRITEBACK)
	 * reported with INODE_DATA_ORDERED, exactly once, with the run as it
	 * was reported: error 0 once the bytes reached the disk; an error when
	 * they did not: the write failed, or (EBUSY) the bytes go to the file's
	 * own write instead, which reports them to the file system itself.
	 * It is called before the caller does anything else that may wait for
	 * the file system, since a commit may be waiting for it.
	 */
	void (*data_written)(struct inode *, const struct inode_data_extent *, int error);
};

/*
 * Where an inode's storage came from, so that it goes back to the same
 * place.  Zero is storage a filesystem allocated itself (or embedded).
 */
enum inode_storage {
	INODE_STORAGE_FILESYSTEM = 0,
	INODE_STORAGE_POOL,
	INODE_STORAGE_HEAP
};

struct inode {
	struct io_error_state i_write_error;
	enum inode_type i_type;
	/* Set once by inode_alloc; destroy_inode gives the storage back. */
	enum inode_storage i_storage;
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

	/*
	 * Set on a node that stands for one of the caller's own descriptors,
	 * which is what /dev/fd/N is.  Opening such a node must not make a new
	 * open file description: it must hand back the one the caller already
	 * holds, the way dup does, so that the two descriptors share a file
	 * offset, share the status flags, and share the locks that belong to
	 * the description.  Copying the offset across at open time would not
	 * do -- the two would diverge on the first read -- and re-opening the
	 * underlying object would re-check permission, could widen the access
	 * mode, and is not possible at all for a pipe, a socket, or a file
	 * that has been unlinked.  A pipe is the common case, because that is
	 * what a shell's process substitution passes this way.
	 *
	 * The open path, not the filesystem, performs the substitution: only
	 * it owns the descriptor table.  The descriptor number is i_rdev's
	 * minor, so nothing has to be carried from the filesystem to the open
	 * path beyond this flag.  Solaris signals the same thing by setting
	 * VDUP on the vnode at every open; FreeBSD and NetBSD instead return
	 * a reserved errno and leave the number in a per-thread field, which
	 * their own comments call a kludge.  Here it is a property of the
	 * node, decided when the node is made and never changed.
	 *
	 * The value also says how lstat and readlink present the node, which
	 * is as a symbolic link, the way Linux does (BUG-061): a walk that
	 * does not follow links must not see /dev/fd/N as the directory the
	 * descriptor holds, or it enters a cycle.  INODE_DESCRIPTOR_ALIAS_NUMBER
	 * marks /dev/fd/N, a link to the file the descriptor holds, and
	 * INODE_DESCRIPTOR_ALIAS_STANDARD marks /dev/stdin, /dev/stdout and
	 * /dev/stderr, links to fd/N.  Zero means an ordinary node.
	 */
	unsigned i_descriptor_alias;
	refcount_t i_refs;
	/* Directory-entry owners, classified within i_refs; changed under VFS transactions. */
	atomic_uint_t i_namespace_refs;

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

	/*
	 * The exec images that map this file's pages, and the opens and
	 * truncations that may change its content.  Each excludes the
	 * other; i_vm_lock protects both counts.
	 */
	unsigned i_exec_text;
	unsigned i_write_access;
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

void inode_namespace_ref(struct inode *inode);
void inode_namespace_release(struct inode *inode);
void inode_cache_retire_namespace(struct mount *mount);

void
inode_cache_purge_mount(
	struct mount *mount);

int
inode_cache_mount_busy(
	struct mount *mount);

/* Caller reserves DYING mount admission through subsequent discard/teardown. */
int inode_cache_mount_revoked_check(struct mount *mount);
/* Irreversible local disposal after all-owner preflight; returns dirty inode count. */
unsigned inode_cache_discard_mount_dirty(struct mount *mount);

unsigned
inode_cache_mount_count(
	struct mount *mount);

unsigned
inode_cache_count(void);

void
inode_cache_reset(void);

unsigned
inode_cache_capacity(void);

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
 * Takes write access to a regular file, refusing one an exec image maps
 * with ETXTBSY; other inode types take nothing.
 */
int
inode_write_access_begin(
	struct inode *inode);

/* Gives back write access taken with inode_write_access_begin(). */
void
inode_write_access_end(
	struct inode *inode);

/*
 * Marks a regular file as mapped by an exec image, refusing one that some
 * holder may write with ETXTBSY.
 */
int
inode_exec_text_begin(
	struct inode *inode);

/* Ends an exec image's mapping of a file. */
void
inode_exec_text_end(
	struct inode *inode);

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
