/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inode cache and the generic inode operations.
 *
 * Every live inode sits in one cache slot; a slot is reclaimed from an
 * unreferenced clean inode when the cache is full.  The generic
 * operations validate a request, check the read-only state of the
 * mount, call the filesystem's callback, and then maintain the
 * timestamps, the directory sequence, and the name cache so that every
 * filesystem gets the same semantics.
 */

#include "kern/inode.h"
#include "kern/backing-claim.h"
#include "kern/cred.h"
#include "kern/atomic.h"
#include "kern/mount.h"
#include "kern/namecache.h"
#include "kern/namei.h"
#include "kern/clock.h"
#include "kern/record-lock.h"
#include "kern/posix-acl.h"
#include "kern/vm-object.h"

#include <errno.h>
#include <string.h>

extern int posix_acl_chmod(struct inode *inode, mode_t mode) __attribute__((weak));
extern int posix_acl_inherit(struct inode *parent, struct inode *child,
    mode_t *mode) __attribute__((weak));
extern int vm_object_inode_io_wait(struct inode *inode) __attribute__((weak));
extern int vm_object_inode_resize_active(struct inode *inode)
    __attribute__((weak));
extern int vm_object_resize_begin(struct inode *inode, off_t size,
    struct vm_object_resize *resize) __attribute__((weak));
extern int vm_object_resize_prepare(struct vm_object_resize *resize)
    __attribute__((weak));
extern void vm_object_resize_commit(struct vm_object_resize *resize, off_t size)
    __attribute__((weak));
extern void vm_object_resize_abort(struct vm_object_resize *resize)
    __attribute__((weak));

#define INODE_COMMON_MAX 256U
#define INODE_CACHE_MAX 512U
#define VFS_BSS __attribute__((section(".vfs_bss")))
#define INODE_HIGH __attribute__((section(".hightext")))
#define INODE_CACHE_RESERVED ((struct inode *)(uintptr_t)1U)

static struct inode common_pool[INODE_COMMON_MAX] VFS_BSS;
static uint8_t common_used[INODE_COMMON_MAX] VFS_BSS;
static struct inode *inode_cache[INODE_CACHE_MAX] VFS_BSS;
static struct spinlock inode_cache_lock = {
	{ 0 }, LOCK_RANK_INODE, "inode cache", 0, 0
};

static int inode_create_locked(struct inode *i, const struct componentname *n, const struct inode_creation_request *request, struct inode **r);
static int inode_mkdir_locked(struct inode *i, const struct componentname *n, const struct inode_creation_request *request, struct inode **r);
static int inode_mknod_locked(struct inode *i, const struct componentname *n, const struct inode_creation_request *request, struct inode **r);
static int inode_unlink_locked(struct inode *i, const struct componentname *n);
static int inode_rmdir_locked(struct inode *i, const struct componentname *n);
static int inode_rename_locked(struct inode *od, const struct componentname *on, struct inode *nd, const struct componentname *nn, unsigned flags);
static int inode_link_locked(struct inode *directory, const struct componentname *name, struct inode *target);
static int inode_symlink_locked(struct inode *directory, const struct componentname *name, const char *target, const struct inode_creation_request *request, struct inode **result);
static int common_index(const struct inode *inode);
static int cache_index(const struct inode *inode);
static void destroy_inode(struct inode *inode);
static int reserve_cache_slot(struct inode **victim);
static int readonly(const struct inode *inode);
static int inode_content_io_lock(struct inode *inode);
static int creation_request_valid(const struct inode_creation_request *request);
static int inode_creation_preserve_acl(struct inode *source, struct inode *child, const char *name);
static int inode_same(const struct inode *left, const struct inode *right);
static int inode_parent_step(struct inode **cursor, int *at_root);
static int inode_vm_resize_available(void);
static int inode_truncate_transaction_impl(struct inode *i, const struct inode_truncate_request *request, struct inode_truncate_result *result);
static int inode_truncate_limited_impl(struct inode *i, off_t size, uint64_t growth_limit, const struct ucred *cred, int content_change, int *limit_exceeded);
static int xattr_name_valid(const char *name);
static int inode_namespace_enter(struct inode *directory, const struct componentname *name, int creation, int *entered);

/*
 * Maps an inode type to its stat mode bits.
 */
mode_t
inode_type_mode(
	enum inode_type type)
{
	switch (type) {
	case INODE_REG:
		return S_IFREG;
	case INODE_DIR:
		return S_IFDIR;
	case INODE_BLOCK:
		return S_IFBLK;
	case INODE_CHAR:
		return S_IFCHR;
	case INODE_SYMLINK:
		return S_IFLNK;
	case INODE_SOCKET:
		return S_IFSOCK;
	case INODE_FIFO:
		return S_IFIFO;
	default:
		return 0;
	}
}

/*
 * Allocates an inode for a mount and enters it in the cache with one
 * cache reference and one caller reference.
 */
struct inode *
inode_alloc(
	struct mount *mountp)
{
	struct inode *inode;
	struct inode *victim;
	unsigned long irq;
	int slot;
	unsigned i;

	inode = NULL;

	/* Reserves a cache slot, evicting a clean inode when needed. */
	slot = reserve_cache_slot(&victim);
	if (slot < 0)
		return NULL;
	if (victim != NULL)
		destroy_inode(victim);

	/* The filesystem allocates its own inodes; others come from the pool. */
	if (mountp != NULL && mountp->m_type != NULL &&
	    mountp->m_type->alloc_inode != NULL) {
		inode = mountp->m_type->alloc_inode(mountp);
	} else {
		irq = spin_lock_irqsave(&inode_cache_lock);
		for (i = 0; i < INODE_COMMON_MAX; i++) {
			if (!common_used[i]) {
				common_used[i] = 1;
				inode = &common_pool[i];
				break;
			}
		}

		spin_unlock_irqrestore(&inode_cache_lock, irq);
	}

	if (inode == NULL) {
		irq = spin_lock_irqsave(&inode_cache_lock);
		inode_cache[slot] = NULL;
		spin_unlock_irqrestore(&inode_cache_lock, irq);
		return NULL;
	}

	/* One cache reference and one reference returned to the caller. */
	memset(inode, 0, sizeof(*inode));
	inode->i_mount = mountp;
	inode->i_dirseq = 1;
	refcount_init(&inode->i_refs, 2);
	(void)mutex_init(&inode->i_io_lock, LOCK_RANK_INODE_IO, "inode I/O");
	spin_init(&inode->i_vm_lock, LOCK_RANK_VM_RESIZE, "inode VM resize");
	waitq_init(&inode->i_vm_waitq, "inode VM resize");
	(void)mutex_init(&inode->i_lock, LOCK_RANK_INODE, "inode");
	irq = spin_lock_irqsave(&inode_cache_lock);

	inode_cache[slot] = inode;

	spin_unlock_irqrestore(&inode_cache_lock, irq);

	return inode;
}

/*
 * Removes a clean inode held only by the cache and destroys it.
 */
void
inode_free(
	struct inode *inode)
{
	int cindex;
	unsigned long irq;

	if (inode == NULL ||
	    refcount_load(&inode->i_refs) != 1 ||
	    (inode->i_flags & INODE_DIRTY))
		return;

	/* Rechecks the reference count under the cache lock. */
	irq = spin_lock_irqsave(&inode_cache_lock);

	cindex = cache_index(inode);
	if (cindex < 0 || refcount_load(&inode->i_refs) != 1) {
		spin_unlock_irqrestore(&inode_cache_lock, irq);
		return;
	}

	inode_cache[cindex] = NULL;
	(void)refcount_put(&inode->i_refs);

	spin_unlock_irqrestore(&inode_cache_lock, irq);

	destroy_inode(inode);
}

/*
 * Finds a cached inode of a mount by number and takes a reference.
 */
int
inode_get(
	struct mount *mountp,
	ino_t ino,
	struct inode **result)
{
	unsigned i;
	unsigned long irq;
	struct inode *inode;

	/* Rejects a query without a mount or a result. */
	if (mountp == NULL || result == NULL)
		return EINVAL;

	/* Reports a live cached inode of that mount and number. */
	irq = spin_lock_irqsave(&inode_cache_lock);

	for (i = 0; i < INODE_CACHE_MAX; i++) {
		inode = inode_cache[i];
		if (inode != NULL &&
		    inode != INODE_CACHE_RESERVED &&
		    inode->i_mount == mountp &&
		    inode->i_ino == ino &&
		    !(inode->i_flags & INODE_DEAD)) {
			inode_ref(inode);
			*result = inode;
			spin_unlock_irqrestore(&inode_cache_lock, irq);
			return 0;
		}
	}

	spin_unlock_irqrestore(&inode_cache_lock, irq);

	return ENOENT;
}

/*
 * Takes a reference on an inode.
 */
void
inode_ref(
	struct inode *inode)
{
	if (inode != NULL)
		refcount_get(&inode->i_refs);
}

/*
 * Drops a reference on an inode, freeing a dead one the cache alone
 * still holds.
 */
void
inode_release(
	struct inode *inode)
{
	unsigned remaining;

	/* Frees a dead, clean inode once only the cache still holds it. */
	if (inode != NULL) {
		remaining = refcount_put_not_last(&inode->i_refs);
		if (remaining == 1 &&
		    (inode->i_flags & INODE_DEAD) != 0 &&
		    (inode->i_flags & (INODE_DIRTY | INODE_ROOT)) == 0)
			inode_free(inode);
	}
}

/*
 * Destroys every clean, unreferenced cached inode of a mount.
 */
void
inode_cache_purge_mount(
	struct mount *mountp)
{
	struct inode *victim;
	unsigned long irq;
	unsigned i;
	struct inode *inode;

	/* Takes one victim per pass so destruction runs without the lock. */
	for (;;) {
		victim = NULL;
		irq = spin_lock_irqsave(&inode_cache_lock);
		for (i = 0; i < INODE_CACHE_MAX; i++) {
			inode = inode_cache[i];
			if (inode != NULL &&
			    inode != INODE_CACHE_RESERVED &&
			    inode->i_mount == mountp &&
			    refcount_load(&inode->i_refs) == 1 &&
			    (inode->i_flags & INODE_DIRTY) == 0) {
				inode_cache[i] = NULL;
				(void)refcount_put(&inode->i_refs);
				victim = inode;
				break;
			}
		}

		spin_unlock_irqrestore(&inode_cache_lock, irq);
		if (victim == NULL)
			break;
		destroy_inode(victim);
	}
}

/*
 * Tests whether any inode of a mount is referenced beyond the cache.
 */
INODE_HIGH int
inode_cache_mount_busy(
	struct mount *mountp)
{
	unsigned i;
	unsigned long irq;
	struct inode *inode;
	unsigned allowed;
	int busy;

	busy = 0;

	/* The root inode also carries the mount's own reference. */
	irq = spin_lock_irqsave(&inode_cache_lock);

	for (i = 0; i < INODE_CACHE_MAX; i++) {
		inode = inode_cache[i];
		if (inode == NULL ||
		    inode == INODE_CACHE_RESERVED ||
		    inode->i_mount != mountp)
			continue;
		if (inode == mountp->m_root)
			allowed = 2U;
		else
			allowed = 1U;
		if (refcount_load(&inode->i_refs) > allowed) {
			busy = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&inode_cache_lock, irq);

	if (busy)
		return EBUSY;
	return 0;
}

/*
 * Counts the cached inodes of a mount.
 */
INODE_HIGH unsigned
inode_cache_mount_count(
	struct mount *mountp)
{
	unsigned i;
	unsigned count;
	unsigned long irq;

	/* Counts the cached inodes that belong to the mount. */
	count = 0;
	irq = spin_lock_irqsave(&inode_cache_lock);

	for (i = 0; i < INODE_CACHE_MAX; i++) {
		if (inode_cache[i] != NULL &&
		    inode_cache[i] != INODE_CACHE_RESERVED &&
		    inode_cache[i]->i_mount == mountp)
			count++;
	}

	spin_unlock_irqrestore(&inode_cache_lock, irq);

	/* Reports the count. */
	return count;
}

/*
 * Destroys every clean, unreferenced cached inode of every mount.
 */
void
inode_cache_reset(
	void)
{
	struct inode *victim;
	unsigned long irq;
	unsigned i;
	struct inode *inode;

	namecache_reset();

	/* Takes one victim per pass so destruction runs without the lock. */
	for (;;) {
		victim = NULL;
		irq = spin_lock_irqsave(&inode_cache_lock);
		for (i = 0; i < INODE_CACHE_MAX; i++) {
			inode = inode_cache[i];
			if (inode != NULL &&
			    inode != INODE_CACHE_RESERVED &&
			    refcount_load(&inode->i_refs) == 1 &&
			    (inode->i_flags & INODE_DIRTY) == 0) {
				inode_cache[i] = NULL;
				(void)refcount_put(&inode->i_refs);
				victim = inode;
				break;
			}
		}

		spin_unlock_irqrestore(&inode_cache_lock, irq);
		if (victim == NULL)
			break;
		destroy_inode(victim);
	}
}

/*
 * Looks a name up in a directory through the name cache and the
 * filesystem.
 */
int
inode_lookup(
	struct inode *directory,
	const struct componentname *name,
	struct inode **result)
{
	struct inode *child;
	uint64_t sequence;
	int error;

	/* Rejects a malformed name or a non-directory. */
	if (directory == NULL ||
	    name == NULL ||
	    result == NULL ||
	    name->cn_namelen == 0 ||
	    name->cn_namelen > NAME_MAX)
		return EINVAL;
	if (directory->i_type != INODE_DIR)
		return ENOTDIR;

	/* Answers from the name cache where the directory allows it. */
	if ((directory->i_flags & INODE_NOCACHE_CHILDREN) == 0 &&
	    namecache_lookup(directory, name, result) == 0)
		return 0;
	if (directory->i_op == NULL || directory->i_op->lookup == NULL)
		return EOPNOTSUPP;

	/* A filesystem answer is validated and cached. */
	sequence = atomic_u64_load_acquire(&directory->i_dirseq);
	error = directory->i_op->lookup(directory, name, &child);
	if (error != 0)
		return error;
	if (child == NULL || child->i_mount == NULL || child->i_type == INODE_NONE) {
		if (child != NULL)
			inode_release(child);
		return EIO;
	}

	if ((directory->i_flags & INODE_NOCACHE_CHILDREN) == 0)
		(void)namecache_enter(directory, name, child, sequence);
	*result = child;
	return 0;
}

/*
 * Looks a name up in a directory ignoring case, where the filesystem
 * supports it.
 */
int
inode_lookup_casefold(
	struct inode *directory,
	const struct componentname *name,
	struct inode **result)
{
	int error;

	/* Rejects a malformed name or a non-directory. */
	if (directory == NULL ||
	    name == NULL ||
	    result == NULL ||
	    name->cn_namelen == 0 ||
	    name->cn_namelen > NAME_MAX)
		return EINVAL;
	if (directory->i_type != INODE_DIR)
		return ENOTDIR;

	/* Only a filesystem that folds case can answer this. */
	if (directory->i_op == NULL || directory->i_op->lookup_casefold == NULL)
		return EOPNOTSUPP;

	/* Reports why the lookup failed. */
	error = directory->i_op->lookup_casefold(directory, name, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Fills a stat record from the filesystem or from the generic fields.
 */
int
inode_getattr(
	struct inode *inode,
	struct stat *status)
{
	int error;

	/* A filesystem that keeps its own attributes answers for itself. */
	if (inode == NULL || status == NULL)
		return EINVAL;
	if (inode->i_op != NULL && inode->i_op->getattr != NULL) {
		error = inode->i_op->getattr(inode, status);
		return error;
	}

	/* Derives the record from the cached inode fields. */
	memset(status, 0, sizeof(*status));
	if (inode->i_mount != NULL && inode->i_mount->m_disk != NULL)
		status->st_dev = inode->i_mount->m_disk->d_dev;
	else
		status->st_dev = 0;
	status->st_ino = inode->i_ino;
	status->st_mode = inode->i_mode;
	status->st_nlink = inode->i_linkcount;
	status->st_uid = inode->i_uid;
	status->st_gid = inode->i_gid;
	status->st_rdev = inode->i_rdev;
	status->st_size = inode->i_size;
	status->st_atime = inode->i_atime.tv_sec;
	status->st_mtime = inode->i_mtime.tv_sec;
	status->st_ctime = inode->i_ctime.tv_sec;
	status->st_blksize = 512;
	if (inode->i_size > 0)
		status->st_blocks =
		    (blkcnt_t)(((uint64_t)inode->i_size + 511U) / 512U);
	else
		status->st_blocks = 0;
	return 0;
}

/*
 * Sets the selected timestamps of an inode to the current time.
 */
void
inode_touch(
	struct inode *inode,
	unsigned mask)
{
	struct inode_time now;

	/* Stamps the requested timestamps with one reading of the clock. */
	if (inode == NULL)
		return;
	clock_realtime(&now.tv_sec, &now.tv_nsec);
	if (mask & INODE_ATTR_ATIME)
		inode->i_atime = now;
	if (mask & INODE_ATTR_MTIME)
		inode->i_mtime = now;
	if (mask & INODE_ATTR_CTIME)
		inode->i_ctime = now;
}

/*
 * Advances a directory's change sequence, purging its name cache
 * entries when the sequence wraps.
 */
void
inode_dir_changed(
	struct inode *inode)
{
	uint64_t sequence;

	/* Advances the directory sequence, purging rather than reusing it. */
	if (inode == NULL || inode->i_type != INODE_DIR)
		return;
	sequence = atomic_u64_load_acquire(&inode->i_dirseq);
	if (sequence == UINT64_MAX) {
		namecache_purge_inode(inode);
		atomic_u64_store_release(&inode->i_dirseq, 1);
	} else {
		atomic_u64_store_release(&inode->i_dirseq, sequence + 1U);
	}
}

/*
 * Changes the attributes of an inode.
 *
 * A size change runs as a truncate transaction.  Mode and ownership
 * changes take the inode I/O lock so they cannot restore set-id bits
 * between a writer's privilege check and its content mutation.
 */
int
inode_setattr(
	struct inode *i,
	const struct stat *s,
	unsigned mask)
{
	const unsigned valid = INODE_ATTR_MODE | INODE_ATTR_UID |
		INODE_ATTR_GID | INODE_ATTR_SIZE | INODE_ATTR_ATIME |
		INODE_ATTR_MTIME | INODE_ATTR_CTIME |
		INODE_ATTR_ATIME_NOW | INODE_ATTR_MTIME_NOW;
	struct stat requested;
	struct inode_time now;
	int held_io;
	int error;

	held_io = 0;

	if (i == NULL || s == NULL || (mask & ~valid) != 0)
		return EINVAL;

	/* Refuses a change on a read-only filesystem. */
	if (readonly(i))
		return EROFS;

	/* Resolves the "now" markers into explicit timestamps. */
	requested = *s;
	if (mask & (INODE_ATTR_ATIME_NOW | INODE_ATTR_MTIME_NOW)) {
		clock_realtime(&now.tv_sec, &now.tv_nsec);
		if (mask & INODE_ATTR_ATIME_NOW) {
			requested.st_atime = now.tv_sec;
#ifdef ZEDBSD_SYS_STAT_H
			requested.st_atim.tv_nsec = now.tv_nsec;
#endif
			mask |= INODE_ATTR_ATIME;
		}

		if (mask & INODE_ATTR_MTIME_NOW) {
			requested.st_mtime = now.tv_sec;
#ifdef ZEDBSD_SYS_STAT_H
			requested.st_mtim.tv_nsec = now.tv_nsec;
#endif
			mask |= INODE_ATTR_MTIME;
		}

		mask &= ~(INODE_ATTR_ATIME_NOW | INODE_ATTR_MTIME_NOW);
	}

	if ((mask & INODE_ATTR_MODE) != 0 &&
	    (requested.st_mode & S_IFMT) != 0 &&
	    (requested.st_mode & S_IFMT) != (i->i_mode & S_IFMT))
		return EINVAL;

	/*
	 * EOF changes use the same VM-visible transaction as ftruncate and
	 * extending write.  Filesystem setattr callbacks therefore never
	 * mutate size behind an already-published shared object's
	 * generation.
	 */
	if ((mask & INODE_ATTR_SIZE) != 0) {
		error = inode_truncate(i, requested.st_size);
		if (error != 0)
			return error;
		mask &= ~INODE_ATTR_SIZE;
		if (mask == 0)
			return 0;
	}

	/*
	 * Copy-up can create/rename upper entries. Complete it before i_io_lock,
	 * because namespace creation takes the transaction gate before this lock
	 * when observing a parent's ownership and set-GID attributes.
	 */
	if (i->i_op != NULL && i->i_op->prepare_mutation != NULL) {
		error = i->i_op->prepare_mutation(i);
		if (error != 0)
			return error;
	}

	/*
	 * Mode and ownership participate in the same regular-inode I/O
	 * domain as writes and truncate.  This prevents chmod/chown from
	 * restoring set-id bits between a writer's privilege-bit
	 * invalidation and its backend content mutation.  Stacked
	 * filesystems naturally acquire outer then content-inode locks
	 * through their setattr callback.
	 */
	if ((mask & (INODE_ATTR_MODE | INODE_ATTR_UID | INODE_ATTR_GID)) != 0 &&
	    !mutex_owned(&i->i_io_lock)) {
		error = inode_content_io_lock(i);
		if (error != 0)
			return error;
		held_io = 1;
	}

	if (i->i_op == NULL || i->i_op->setattr == NULL) {
		error = EOPNOTSUPP;
		goto out;
	}

	error = i->i_op->setattr(i, &requested, mask);
	if (error != 0)
		goto out;
	if ((mask & INODE_ATTR_MODE) != 0 && posix_acl_chmod != NULL) {
		error = posix_acl_chmod(i, requested.st_mode);
		if (error != 0)
			goto out;
	}

	/* Mirrors the committed attributes into the cached fields. */
	if (mask & INODE_ATTR_MODE)
		i->i_mode = (i->i_mode & S_IFMT) |
			(requested.st_mode & ~S_IFMT);
	if (mask & INODE_ATTR_UID)
		i->i_uid = requested.st_uid;
	if (mask & INODE_ATTR_GID)
		i->i_gid = requested.st_gid;
	if (mask & INODE_ATTR_ATIME) {
		i->i_atime.tv_sec = requested.st_atime;
#ifdef ZEDBSD_SYS_STAT_H
		i->i_atime.tv_nsec = requested.st_atim.tv_nsec;
#else
		i->i_atime.tv_nsec = 0;
#endif
	}

	if (mask & INODE_ATTR_MTIME) {
		i->i_mtime.tv_sec = requested.st_mtime;
#ifdef ZEDBSD_SYS_STAT_H
		i->i_mtime.tv_nsec = requested.st_mtim.tv_nsec;
#else
		i->i_mtime.tv_nsec = 0;
#endif
	}

	if (mask & INODE_ATTR_CTIME) {
		i->i_ctime.tv_sec = requested.st_ctime;
#ifdef ZEDBSD_SYS_STAT_H
		i->i_ctime.tv_nsec = requested.st_ctim.tv_nsec;
#else
		i->i_ctime.tv_nsec = 0;
#endif
	} else if (mask != 0) {
		inode_touch(i, INODE_ATTR_CTIME);
	}

out:
	if (held_io)
		mutex_unlock(&i->i_io_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Builds a creation request on behalf of a user, checking that the
 * credential may create in the parent and applying set-group-id
 * inheritance.
 */
int
inode_creation_request_user(
	struct inode *parent,
	const struct ucred *credential,
	enum inode_type type,
	mode_t mode,
	dev_t rdev,
	void *special,
	struct inode_creation_request *request)
{
	int error;

	/* Rejects an unknown type or a mode carrying type bits. */
	if (parent == NULL ||
	    credential == NULL ||
	    request == NULL ||
	    type <= INODE_NONE ||
	    type > INODE_FIFO ||
	    (mode & S_IFMT) != 0)
		return EINVAL;
	memset(request, 0, sizeof(*request));

	/*
	 * Authorization and the set-GID/GID snapshot are one metadata
	 * observation.  inode_setattr() publishes parent mode and ownership
	 * under i_io_lock; taking the same domain prevents a child from
	 * combining an old S_ISGID bit with a newly published parent GID.
	 */
	error = inode_content_io_lock(parent);
	if (error != 0)
		return error;
	error = vfs_may_create(parent, credential);
	if (error != 0) {
		mutex_unlock(&parent->i_io_lock);
		return error;
	}

	request->origin = INODE_CREATION_USER;
	request->type = type;
	request->mode = mode & 07777U;
	request->uid = credential->euid;
	if ((parent->i_mode & S_ISGID) != 0)
		request->gid = parent->i_gid;
	else
		request->gid = credential->egid;
	if (type == INODE_DIR && (parent->i_mode & S_ISGID) != 0)
		request->mode |= S_ISGID;
	request->rdev = rdev;
	request->special = special;

	mutex_unlock(&parent->i_io_lock);

	if (!creation_request_valid(request))
		return EINVAL;
	return 0;
}

/*
 * Builds a creation request on behalf of the kernel with explicit
 * ownership.
 */
int
inode_creation_request_system(
	enum inode_type type,
	mode_t mode,
	uid_t uid,
	gid_t gid,
	dev_t rdev,
	struct inode_creation_request *request)
{
	if (request == NULL ||
	    type <= INODE_NONE ||
	    type > INODE_FIFO ||
	    (mode & S_IFMT) != 0)
		return EINVAL;
	memset(request, 0, sizeof(*request));
	request->origin = INODE_CREATION_SYSTEM;
	request->type = type;
	request->mode = mode & 07777U;
	request->uid = uid;
	request->gid = gid;
	request->rdev = rdev;
	if (!creation_request_valid(request))
		return EINVAL;
	return 0;
}

/*
 * Builds a creation request that copies the identity of an existing
 * inode.
 */
int
inode_creation_request_preserve(
	const struct inode *source,
	struct inode_creation_request *request)
{
	if (source == NULL ||
	    request == NULL ||
	    source->i_type <= INODE_NONE ||
	    source->i_type > INODE_FIFO)
		return EINVAL;
	memset(request, 0, sizeof(*request));
	request->origin = INODE_CREATION_PRESERVE;
	request->type = source->i_type;
	request->mode = source->i_mode & 07777U;
	request->uid = source->i_uid;
	request->gid = source->i_gid;
	request->rdev = source->i_rdev;
	if (source->i_type == INODE_SOCKET)
		request->special = source->i_special;
	else
		request->special = NULL;
	request->source = source;
	if (!creation_request_valid(request))
		return EINVAL;
	return 0;
}

/*
 * Applies a creation request to a newly created child: mode, ownership,
 * device number, ACLs, the special endpoint, and timestamps.
 */
int
inode_creation_prepare(
	struct inode *parent,
	struct inode *child,
	const struct inode_creation_request *request)
{
	mode_t inherited;
	int error;

	/* Rejects a request the child does not match. */
	if (parent == NULL ||
	    child == NULL ||
	    !creation_request_valid(request) ||
	    child->i_type != request->type ||
	    (request->special != NULL && request->type != INODE_SOCKET))
		return EINVAL;

	/* Gives the child the requested identity and permissions. */
	child->i_mode = inode_type_mode(request->type) |
	    (request->mode & 07777U);
	child->i_uid = request->uid;
	child->i_gid = request->gid;
	child->i_rdev = request->rdev;

	/* A preserved child copies the source ACLs; a new one inherits. */
	if (request->origin == INODE_CREATION_PRESERVE &&
	    (request->type == INODE_REG || request->type == INODE_DIR)) {
		error = inode_creation_preserve_acl((struct inode *)request->source,
		    child, POSIX_ACL_XATTR_ACCESS);
		if (error == 0 && request->type == INODE_DIR)
			error = inode_creation_preserve_acl(
			    (struct inode *)request->source, child,
			    POSIX_ACL_XATTR_DEFAULT);
		if (error != 0)
			return error;
	} else if (request->origin != INODE_CREATION_PRESERVE &&
	    (request->type == INODE_REG || request->type == INODE_DIR) &&
	    posix_acl_inherit != NULL) {
		inherited = child->i_mode;
		error = posix_acl_inherit(parent, child, &inherited);
		if (error != 0)
			return error;
		child->i_mode = inherited;
	}

	/* A socket binds its endpoint exactly once. */
	if (request->special != NULL) {
		mutex_lock(&child->i_lock);
		if (child->i_special != NULL) {
			error = EADDRINUSE;
		} else {
			child->i_special = request->special;
			error = 0;
		}

		mutex_unlock(&child->i_lock);
		if (error != 0)
			return error;
	}

	if (request->origin == INODE_CREATION_PRESERVE) {
		child->i_atime = request->source->i_atime;
		child->i_mtime = request->source->i_mtime;
		child->i_ctime = request->source->i_ctime;
	} else {
		inode_touch(child, INODE_ATTR_ATIME | INODE_ATTR_MTIME |
		    INODE_ATTR_CTIME);
	}

	return 0;
}

/*
 * Reads the target of a symbolic link.
 */
ssize_t
inode_readlink(
	struct inode *inode,
	char *buffer,
	size_t capacity)
{
	ssize_t count;

	/* Only a symbolic link has a target to read. */
	if (inode == NULL || buffer == NULL)
		return -EINVAL;
	if (inode->i_type != INODE_SYMLINK)
		return -EINVAL;

	/* Asks the filesystem for the target. */
	if (inode->i_op != NULL && inode->i_op->readlink != NULL)
		count = inode->i_op->readlink(inode, buffer, capacity);
	else
		count = -EOPNOTSUPP;

	/* Reports the target length, or the negated error. */
	return count;
}

/*
 * Runs a truncate transaction under the inode's backing guard.
 */
int
inode_truncate_transaction(
	struct inode *i,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	struct backing_mutation_guard guard;
	int error;

	/* A malformed request is refused by the implementation itself. */
	if (i == NULL || request == NULL) {
		error = inode_truncate_transaction_impl(i, request, result);
		return error;
	}

	/* Excludes conflicting claims on the file for the whole truncate. */
	error = backing_mutation_begin_inode(i, &guard);
	if (error != 0)
		return error;
	error = inode_truncate_transaction_impl(i, request, result);
	backing_mutation_end(&guard);

	/* Reports why the truncate failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Truncates a file under a growth limit with a credential.
 */
int
inode_truncate_limited_cred(
	struct inode *i,
	off_t size,
	uint64_t growth_limit,
	const struct ucred *cred,
	int *limit_exceeded)
{
	int error;

	/* Reports the failure. */
	error = inode_truncate_limited_impl(i, size, growth_limit, cred, 0,
	    limit_exceeded);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Truncates a file under a growth limit.
 */
int
inode_truncate_limited(
	struct inode *i,
	off_t size,
	uint64_t growth_limit,
	int *limit_exceeded)
{
	int error;

	/* Reports the failure. */
	error = inode_truncate_limited_impl(i, size, growth_limit, NULL, 0,
	    limit_exceeded);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Truncates a file without a growth limit.
 */
int
inode_truncate(
	struct inode *i,
	off_t size)
{
	int error;

	/* Reports the failure. */
	error = inode_truncate_limited_impl(i, size, UINT64_MAX, NULL, 0, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Truncates a file as a content change that clears set-id bits.
 */
int
inode_truncate_content_change(
	struct inode *i,
	off_t size)
{
	int error;

	/* Reports the failure. */
	error = inode_truncate_limited_impl(i, size, UINT64_MAX, NULL, 1, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads an extended attribute.
 */
ssize_t
inode_getxattr(
	struct inode *inode,
	const char *name,
	void *value,
	size_t size)
{
	ssize_t count;

	/* Rejects a malformed name or an oversized buffer. */
	if (inode == NULL ||
	    !xattr_name_valid(name) ||
	    (value == NULL && size != 0) ||
	    size > INODE_XATTR_SIZE_MAX)
		return -EINVAL;

	/* Only a filesystem that stores attributes can answer. */
	if (inode->i_op != NULL && inode->i_op->getxattr != NULL)
		count = inode->i_op->getxattr(inode, name, value, size);
	else
		count = -EOPNOTSUPP;

	/* Reports the attribute length, or the negated error. */
	return count;
}

/*
 * Writes an extended attribute.
 */
int
inode_setxattr(
	struct inode *inode,
	const char *name,
	const void *value,
	size_t size,
	unsigned flags)
{
	int error;

	/* Rejects a malformed name, an oversized value, or contradictory flags. */
	if (inode == NULL ||
	    !xattr_name_valid(name) ||
	    (value == NULL && size != 0) ||
	    size > INODE_XATTR_SIZE_MAX ||
	    (flags & ~(INODE_XATTR_CREATE | INODE_XATTR_REPLACE)) != 0 ||
	    flags == (INODE_XATTR_CREATE | INODE_XATTR_REPLACE))
		return EINVAL;

	/* Refuses a change on a read-only filesystem. */
	if (readonly(inode))
		return EROFS;

	/* Only a filesystem that stores attributes can accept one. */
	if (inode->i_op != NULL && inode->i_op->setxattr != NULL)
		error = inode->i_op->setxattr(inode, name, value, size, flags);
	else
		error = EOPNOTSUPP;

	/* A stored attribute changes the inode's status time. */
	if (error == 0)
		inode_touch(inode, INODE_ATTR_CTIME);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Lists the extended attribute names.
 */
ssize_t
inode_listxattr(
	struct inode *inode,
	char *list,
	size_t size)
{
	ssize_t count;

	/* Rejects a missing or oversized buffer. */
	if (inode == NULL ||
	    (list == NULL && size != 0) ||
	    size > INODE_XATTR_SIZE_MAX)
		return -EINVAL;

	/* Only a filesystem that stores attributes can list them. */
	if (inode->i_op != NULL && inode->i_op->listxattr != NULL)
		count = inode->i_op->listxattr(inode, list, size);
	else
		count = -EOPNOTSUPP;

	/* Reports the list length, or the negated error. */
	return count;
}

/*
 * Removes an extended attribute.
 */
int
inode_removexattr(
	struct inode *inode,
	const char *name)
{
	int error;

	if (inode == NULL || !xattr_name_valid(name))
		return EINVAL;

	/* Refuses a change on a read-only filesystem. */
	if (readonly(inode))
		return EROFS;

	/* Delegates to the filesystem. */
	if (inode->i_op != NULL && inode->i_op->removexattr != NULL)
		error = inode->i_op->removexattr(inode, name);
	else
		error = EOPNOTSUPP;

	/* A stored change updates the inode status time. */
	if (error == 0)
		inode_touch(inode, INODE_ATTR_CTIME);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes an inode back through the filesystem.
 */
int
inode_sync(
	struct inode *i)
{
	int error;

	/* A filesystem without a sync operation has nothing to do. */
	if (i == NULL)
		return EINVAL;
	if (i->i_op != NULL && i->i_op->sync != NULL)
		error = i->i_op->sync(i);
	else
		error = 0;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Counts the cached inodes.
 */
unsigned
inode_cache_count(
	void)
{
	unsigned i;
	unsigned count;
	unsigned long irq;

	/* Counts the occupied slots of the inode cache. */
	count = 0;
	irq = spin_lock_irqsave(&inode_cache_lock);

	for (i = 0; i < INODE_CACHE_MAX; i++) {
		if (inode_cache[i] != NULL &&
		    inode_cache[i] != INODE_CACHE_RESERVED)
			count++;
	}

	spin_unlock_irqrestore(&inode_cache_lock, irq);

	/* Reports the count. */
	return count;
}

/*
 * Creates a regular inode under namespace admission.
 */
int
inode_create(
	struct inode *i,
	const struct componentname *n,
	const struct inode_creation_request *request,
	struct inode **r)
{
	int entered;
	int error;

	/* Reserves namespace admission before calling the backend. */
	error = inode_namespace_enter(i, n, 1, &entered);

	if (error == 0)
		error = inode_create_locked(i, n, request, r);
	if (entered)
		mount_vfs_transaction_leave(i->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Links an inode under namespace admission.
 */
int
inode_link(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target)
{
	int entered;
	int error;

	/* Reserves namespace admission before calling the backend. */
	error = inode_namespace_enter(directory, name, 1, &entered);

	if (error == 0)
		error = inode_link_locked(directory, name, target);
	if (entered)
		mount_vfs_transaction_leave(directory->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Creates a directory under namespace admission.
 */
int
inode_mkdir(
	struct inode *i,
	const struct componentname *n,
	const struct inode_creation_request *request,
	struct inode **r)
{
	int entered;
	int error;

	/* Reserves namespace admission before calling the backend. */
	error = inode_namespace_enter(i, n, 1, &entered);

	if (error == 0)
		error = inode_mkdir_locked(i, n, request, r);
	if (entered)
		mount_vfs_transaction_leave(i->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Creates a special inode under namespace admission.
 */
int
inode_mknod(
	struct inode *i,
	const struct componentname *n,
	const struct inode_creation_request *request,
	struct inode **r)
{
	int entered;
	int error;

	/* Reserves namespace admission before calling the backend. */
	error = inode_namespace_enter(i, n, 1, &entered);

	if (error == 0)
		error = inode_mknod_locked(i, n, request, r);
	if (entered)
		mount_vfs_transaction_leave(i->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Renames an entry while retaining both namespace reservations.
 */
int
inode_rename(
	struct inode *od,
	const struct componentname *on,
	struct inode *nd,
	const struct componentname *nn,
	unsigned flags)
{
	int entered, error;

	/* A rename never crosses a mount. */
	if (od == NULL || nd == NULL || nn == NULL || flags != 0)
		return EINVAL;
	if (od->i_mount != nd->i_mount)
		return EXDEV;

	/* Enters the namespace transaction and validates the destination. */
	error = inode_namespace_enter(od, on, 0, &entered);
	if (error == 0 && readonly(nd))
		error = EROFS;
	if (error == 0 && (nd->i_flags & INODE_DEAD) != 0)
		error = ENOENT;
	if (error == 0)
		error = mount_namespace_check_name(nd, nn);

	/* Performs the rename and leaves the transaction. */
	if (error == 0)
		error = inode_rename_locked(od, on, nd, nn, flags);
	if (entered)
		mount_vfs_transaction_leave(od->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Removes a directory while retaining namespace admission.
 */
int
inode_rmdir(
	struct inode *i,
	const struct componentname *n)
{
	int entered;
	int error;

	/* Reserves namespace admission before calling the backend. */
	error = inode_namespace_enter(i, n, 0, &entered);

	if (error == 0)
		error = inode_rmdir_locked(i, n);
	if (entered)
		mount_vfs_transaction_leave(i->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Creates a symbolic link under namespace admission.
 */
int
inode_symlink(
	struct inode *directory,
	const struct componentname *name,
	const char *target,
	const struct inode_creation_request *request,
	struct inode **result)
{
	int entered;
	int error;

	/* Reserves namespace admission before calling the backend. */
	error = inode_namespace_enter(directory, name, 1, &entered);

	if (error == 0)
		error = inode_symlink_locked(directory, name, target, request, result);
	if (entered)
		mount_vfs_transaction_leave(directory->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Unlinks a name while retaining namespace admission.
 */
int
inode_unlink(
	struct inode *i,
	const struct componentname *n)
{
	int entered;
	int error;

	/* Reserves namespace admission before calling the backend. */
	error = inode_namespace_enter(i, n, 0, &entered);

	if (error == 0)
		error = inode_unlink_locked(i, n);
	if (entered)
		mount_vfs_transaction_leave(i->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Checks ancestry without permitting malformed parent cycles.
 */
int
inode_is_ancestor(
	struct inode *source,
	struct inode *new_parent,
	int *result)
{
	struct inode *current;
	struct inode *fast;
	int error;
	int at_root;
	unsigned step;

	if (source == NULL || new_parent == NULL || result == NULL)
		return EINVAL;
	*result = 0;

	current = new_parent;
	fast = new_parent;
	error = 0;

	/*
	 * A directory cannot become a child of itself.  Walk the
	 * destination parent chain in the generic layer so every filesystem
	 * gets identical semantics.  The second cursor is Floyd cycle
	 * detection: a malformed pre-existing tree is reported as EIO
	 * instead of making rename loop forever.
	 */
	inode_ref(current);
	inode_ref(fast);
	for (;;) {
		if (inode_same(current, source)) {
			*result = 1;
			break;
		}

		error = inode_parent_step(&current, &at_root);
		if (error != 0 || at_root)
			break;
		if (fast == NULL)
			continue;
		for (step = 0; step < 2; step++) {
			error = inode_parent_step(&fast, &at_root);
			if (error != 0)
				goto out;
			if (at_root) {
				inode_release(fast);
				fast = NULL;
				break;
			}
		}

		if (fast != NULL && inode_same(current, fast)) {
			error = EIO;
			break;
		}
	}

out:
	inode_release(current);
	if (fast != NULL)
		inode_release(fast);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Creates a regular file in a directory.
 */
static int
inode_create_locked(
	struct inode *i,
	const struct componentname *n,
	const struct inode_creation_request *request,
	struct inode **r)
{
	int error;

	/* Rejects a malformed request. */
	if (i == NULL ||
	    n == NULL ||
	    r == NULL ||
	    !creation_request_valid(request) ||
	    request->type != INODE_REG)
		return EINVAL;

	/* Refuses a change on a read-only filesystem. */
	if (readonly(i))
		return EROFS;

	/* Delegates to the filesystem. */
	if (i->i_op != NULL && i->i_op->create != NULL)
		error = i->i_op->create(i, n, request, r);
	else
		error = EOPNOTSUPP;

	/* A successful change updates the directory. */
	if (error == 0) {
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Creates a directory in a directory.
 */
static int
inode_mkdir_locked(
	struct inode *i,
	const struct componentname *n,
	const struct inode_creation_request *request,
	struct inode **r)
{
	int error;

	/* Rejects a malformed request. */
	if (i == NULL ||
	    n == NULL ||
	    r == NULL ||
	    !creation_request_valid(request) ||
	    request->type != INODE_DIR)
		return EINVAL;

	/* Refuses a change on a read-only filesystem. */
	if (readonly(i))
		return EROFS;

	/* Delegates to the filesystem. */
	if (i->i_op != NULL && i->i_op->mkdir != NULL)
		error = i->i_op->mkdir(i, n, request, r);
	else
		error = EOPNOTSUPP;

	/* A successful change updates the directory. */
	if (error == 0) {
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Creates a FIFO, socket, or device node in a directory.
 */
static int
inode_mknod_locked(
	struct inode *i,
	const struct componentname *n,
	const struct inode_creation_request *request,
	struct inode **r)
{
	int error;

	/* Rejects a malformed request. */
	if (i == NULL ||
	    n == NULL ||
	    r == NULL ||
	    !creation_request_valid(request))
		return EINVAL;
	if (request->type != INODE_FIFO &&
	    request->type != INODE_SOCKET &&
	    request->type != INODE_CHAR &&
	    request->type != INODE_BLOCK)
		return EOPNOTSUPP;
	if (readonly(i))
		return EROFS;

	/* Delegates to the filesystem. */
	if (i->i_op != NULL && i->i_op->mknod != NULL)
		error = i->i_op->mknod(i, n, request, r);
	else
		error = EOPNOTSUPP;

	/* A successful change updates the directory. */
	if (error == 0) {
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Removes a non-directory name from a directory.
 */
static int
inode_unlink_locked(
	struct inode *i,
	const struct componentname *n)
{
	struct inode *target;
	struct backing_mutation_guard guard;
	int error;

	if (i == NULL || n == NULL)
		return EINVAL;

	/* Refuses a change on a read-only filesystem. */
	if (readonly(i))
		return EROFS;

	/* The target must be an ordinary object with no special role. */
	error = inode_lookup(i, n, &target);
	if (error != 0)
		return error;

	/* Preserves mounted and bound namespace anchors. */
	error = mount_namespace_check_inode(target);
	if (error != 0) {
		inode_release(target);
		return error;
	}

	error = backing_mutation_begin_inode(target, &guard);
	if (error != 0) {
		inode_release(target);
		return error;
	}

	if (target->i_type == INODE_DIR) {
		backing_mutation_end(&guard);
		inode_release(target);
		return EPERM;
	}

	if ((target->i_flags & (INODE_ROOT |
	    INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		backing_mutation_end(&guard);
		inode_release(target);
		return EBUSY;
	}

	inode_release(target);
	if (i->i_op != NULL && i->i_op->unlink != NULL)
		error = i->i_op->unlink(i, n);
	else
		error = EOPNOTSUPP;
	backing_mutation_end(&guard);
	if (error == 0) {
		namecache_remove(i, n);
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Removes a directory name from a directory.
 */
static int
inode_rmdir_locked(
	struct inode *i,
	const struct componentname *n)
{
	struct inode *target;
	int error;

	if (i == NULL || n == NULL)
		return EINVAL;

	/* Refuses a change on a read-only filesystem. */
	if (readonly(i))
		return EROFS;

	/* The target must be a directory with no special role. */
	error = inode_lookup(i, n, &target);
	if (error != 0)
		return error;

	/* Preserves mounted and bound namespace anchors. */
	error = mount_namespace_check_inode(target);
	if (error != 0) {
		inode_release(target);
		return error;
	}

	if (target->i_type != INODE_DIR) {
		inode_release(target);
		return ENOTDIR;
	}

	if ((target->i_flags & (INODE_ROOT |
	    INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		inode_release(target);
		return EBUSY;
	}

	inode_release(target);
	if (i->i_op != NULL && i->i_op->rmdir != NULL)
		error = i->i_op->rmdir(i, n);
	else
		error = EOPNOTSUPP;
	if (error == 0) {
		namecache_remove(i, n);
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Renames a name within one mount.
 *
 * A directory is checked against becoming its own descendant, a
 * replaced target must match the source's kind, and both objects are
 * guarded against backing reclaim across the filesystem rename.
 */
static int
inode_rename_locked(
	struct inode *od,
	const struct componentname *on,
	struct inode *nd,
	const struct componentname *nn,
	unsigned flags)
{
	struct inode *source;
	struct inode *target;
	struct backing_mutation_guard source_guard;
	struct backing_mutation_guard target_guard;
	int target_guarded;
	int error;
	int ancestor;

	target_guarded = 0;

	/* A rename never crosses a mount or touches a read-only one. */
	if (od == NULL || on == NULL || nd == NULL || nn == NULL || flags != 0)
		return EINVAL;
	if (od->i_mount != nd->i_mount)
		return EXDEV;
	if (readonly(od) || readonly(nd))
		return EROFS;

	/* Guards the source and checks it may move. */
	error = inode_lookup(od, on, &source);
	if (error != 0)
		return error;
	error = mount_namespace_check_inode(source);
	if (error != 0) {
		inode_release(source);
		return error;
	}

	error = backing_mutation_begin_inode(source, &source_guard);
	if (error != 0) {
		inode_release(source);
		return error;
	}

	if ((source->i_flags & (INODE_ROOT |
	    INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		backing_mutation_end(&source_guard);
		inode_release(source);
		return EBUSY;
	}

	if (source->i_type == INODE_DIR && !inode_same(od, nd)) {
		error = inode_is_ancestor(source, nd, &ancestor);
		if (error == 0 && ancestor)
			error = EINVAL;
		if (error != 0) {
			backing_mutation_end(&source_guard);
			inode_release(source);
			return error;
		}
	}

	/* Guards a replaced target and checks it matches the source's kind. */
	error = inode_lookup(nd, nn, &target);
	if (error == 0) {
		error = mount_namespace_check_inode(target);
		if (error != 0) {
			inode_release(target);
			backing_mutation_end(&source_guard);
			inode_release(source);
			return error;
		}

		if (target == source || (target->i_mount == source->i_mount &&
		    target->i_ino == source->i_ino)) {
			inode_release(target);
			backing_mutation_end(&source_guard);
			inode_release(source);
			return 0;
		}

		error = backing_mutation_begin_inode(target, &target_guard);
		if (error != 0) {
			inode_release(target);
			backing_mutation_end(&source_guard);
			inode_release(source);
			return error;
		}

		target_guarded = 1;
		if ((target->i_flags & (INODE_ROOT |
		    INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
			inode_release(target);
			backing_mutation_end(&target_guard);
			backing_mutation_end(&source_guard);
			inode_release(source);
			return EBUSY;
		}

		if (source->i_type == INODE_DIR && target->i_type != INODE_DIR) {
			inode_release(target);
			backing_mutation_end(&target_guard);
			backing_mutation_end(&source_guard);
			inode_release(source);
			return ENOTDIR;
		}

		if (source->i_type != INODE_DIR && target->i_type == INODE_DIR) {
			inode_release(target);
			backing_mutation_end(&target_guard);
			backing_mutation_end(&source_guard);
			inode_release(source);
			return EISDIR;
		}

		inode_release(target);
	} else if (error != ENOENT) {
		backing_mutation_end(&source_guard);
		inode_release(source);
		return error;
	}

	/* Runs the filesystem rename and updates both directories. */
	if (od->i_op != NULL && od->i_op->rename != NULL)
		error = od->i_op->rename(od, on, nd, nn, flags);
	else
		error = EOPNOTSUPP;
	if (error == 0) {
		namecache_remove(od, on);
		namecache_remove(nd, nn);
		inode_dir_changed(od);
		if (!inode_same(nd, od))
			inode_dir_changed(nd);
		/* A directory's visible ".." entry changes on reparenting. */
		if (source->i_type == INODE_DIR && !inode_same(od, nd))
			inode_dir_changed(source);
		inode_touch(od, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
		if (!inode_same(nd, od))
			inode_touch(nd, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}

	if (target_guarded)
		backing_mutation_end(&target_guard);
	backing_mutation_end(&source_guard);
	inode_release(source);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Adds a hard link to a non-directory within one mount.
 */
static int
inode_link_locked(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target)
{
	struct backing_mutation_guard guard;
	int error;

	/* A link stays inside one mount and never names a directory. */
	if (directory == NULL || name == NULL || target == NULL)
		return EINVAL;
	if (directory->i_type != INODE_DIR)
		return ENOTDIR;
	if (target->i_type == INODE_DIR)
		return EPERM;
	if (directory->i_mount != target->i_mount)
		return EXDEV;
	if (readonly(directory))
		return EROFS;

	/* Excludes conflicting claims on the target for the link. */
	error = backing_mutation_begin_inode(target, &guard);
	if (error != 0)
		return error;

	/* Delegates to the filesystem. */
	if (directory->i_op != NULL && directory->i_op->link != NULL)
		error = directory->i_op->link(directory, name, target);
	else
		error = EOPNOTSUPP;
	backing_mutation_end(&guard);

	/* A successful link updates the directory and the target. */
	if (error == 0) {
		inode_dir_changed(directory);
		target->i_linkcount++;
		inode_touch(target, INODE_ATTR_CTIME);
		inode_touch(directory, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Creates a symbolic link in a directory.
 */
static int
inode_symlink_locked(
	struct inode *directory,
	const struct componentname *name,
	const char *target,
	const struct inode_creation_request *request,
	struct inode **result)
{
	int error;

	/* Requires a directory and a non-empty link target. */
	if (directory == NULL ||
	    name == NULL ||
	    target == NULL ||
	    result == NULL ||
	    !creation_request_valid(request) ||
	    request->type != INODE_SYMLINK)
		return EINVAL;
	if (directory->i_type != INODE_DIR)
		return ENOTDIR;
	if (target[0] == '\0')
		return ENOENT;
	if (readonly(directory))
		return EROFS;

	/* Delegates to the filesystem. */
	if (directory->i_op != NULL && directory->i_op->symlink != NULL)
		error = directory->i_op->symlink(directory, name, target, request,
		    result);
	else
		error = EOPNOTSUPP;

	/* A successful change updates the directory. */
	if (error == 0) {
		inode_dir_changed(directory);
		inode_touch(directory, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the pool index of a pool inode, or -1. */
static int
common_index(
	const struct inode *inode)
{
	unsigned i;

	for (i = 0; i < INODE_COMMON_MAX; i++) {
		if (&common_pool[i] == inode)
			return (int)i;
	}

	return -1;
}

/* Reports the cache slot of an inode, or -1. */
static int
cache_index(
	const struct inode *inode)
{
	unsigned i;

	for (i = 0; i < INODE_CACHE_MAX; i++) {
		if (inode_cache[i] == inode)
			return (int)i;
	}

	return -1;
}

/* Tears down an inode that has left the cache. */
static void
destroy_inode(
	struct inode *inode)
{
	struct mount *mountp;
	void *special;
	int pindex;
	unsigned long irq;

	void (*special_destroy)(void *);

	/* Destroys the special endpoint, then lets the filesystem reclaim. */
	record_lock_inode_destroy(inode);
	mutex_lock(&inode->i_lock);

	special = inode->i_special;
	special_destroy = inode->i_special_destroy;
	inode->i_special = NULL;
	inode->i_special_destroy = NULL;

	mutex_unlock(&inode->i_lock);

	if (special != NULL && special_destroy != NULL)
		special_destroy(special);
	if (inode->i_op != NULL && inode->i_op->reclaim != NULL)
		inode->i_op->reclaim(inode);

	/* Returns the storage to the pool or to the filesystem. */
	mountp = inode->i_mount;
	pindex = common_index(inode);
	if (pindex >= 0) {
		memset(inode, 0, sizeof(*inode));
		irq = spin_lock_irqsave(&inode_cache_lock);
		common_used[pindex] = 0;
		spin_unlock_irqrestore(&inode_cache_lock, irq);
	} else if (mountp != NULL && mountp->m_type != NULL &&
	    mountp->m_type->free_inode != NULL) {
		mountp->m_type->free_inode(inode);
	}
}

/* Reserves a cache slot, evicting a clean unreferenced inode if needed. */
static int
reserve_cache_slot(
	struct inode **victim)
{
	unsigned i;
	unsigned long irq;
	struct inode *inode;

	irq = spin_lock_irqsave(&inode_cache_lock);

	*victim = NULL;

	/* A free slot is reserved directly. */
	for (i = 0; i < INODE_CACHE_MAX; i++) {
		if (inode_cache[i] == NULL) {
			inode_cache[i] = INODE_CACHE_RESERVED;
			spin_unlock_irqrestore(&inode_cache_lock, irq);
			return (int)i;
		}
	}

	/* Otherwise a clean inode held only by the cache is evicted. */
	for (i = 0; i < INODE_CACHE_MAX; i++) {
		inode = inode_cache[i];
		if (inode != INODE_CACHE_RESERVED && refcount_load(&inode->i_refs) == 1 &&
		    !(inode->i_flags & (INODE_DIRTY | INODE_ROOT))) {
			inode_cache[i] = INODE_CACHE_RESERVED;
			(void)refcount_put(&inode->i_refs);
			*victim = inode;
			spin_unlock_irqrestore(&inode_cache_lock, irq);
			return (int)i;
		}
	}

	spin_unlock_irqrestore(&inode_cache_lock, irq);

	return -1;
}

/* Tests whether an inode lives on a read-only mount. */
static int
readonly(
	const struct inode *inode)
{
	if (inode == NULL)
		return 0;
	if (inode->i_mount == NULL)
		return 0;
	if ((inode->i_mount->m_flags & MOUNT_READ_ONLY) != 0)
		return 1;
	return 0;
}

/* Takes a regular file's I/O lock outside any content or resize transaction. */
static int
inode_content_io_lock(
	struct inode *inode)
{
	int error;

	/*
	 * Enter the regular-file content domain before changing privilege
	 * metadata.  A content/resize owner deliberately drops i_io while
	 * revoking mappings and writing old dirty data, so taking the mutex
	 * alone would enter the middle of its transaction.  Wait for the
	 * publication gate, acquire i_io, then recheck to close that
	 * hand-off window.
	 */
	if (vm_object_inode_io_wait == NULL ||
	    vm_object_inode_resize_active == NULL) {
		mutex_lock(&inode->i_io_lock);
		return 0;
	}

	for (;;) {
		error = vm_object_inode_io_wait(inode);
		if (error != 0)
			return error;
		mutex_lock(&inode->i_io_lock);
		if (!vm_object_inode_resize_active(inode))
			return 0;
		mutex_unlock(&inode->i_io_lock);
	}
}

/* Tests whether a creation request is internally consistent. */
static int
creation_request_valid(
	const struct inode_creation_request *request)
{
	if (request == NULL ||
	    request->origin < INODE_CREATION_USER ||
	    request->origin > INODE_CREATION_PRESERVE ||
	    request->type <= INODE_NONE ||
	    request->type > INODE_FIFO ||
	    (request->mode & S_IFMT) != 0)
		return 0;
	if (request->type != INODE_CHAR && request->type != INODE_BLOCK &&
	    request->rdev != 0)
		return 0;
	if ((request->type == INODE_SOCKET) != (request->special != NULL))
		return 0;

	/* Only a preserving request names a source, and it must match. */
	if (request->origin == INODE_CREATION_PRESERVE) {
		if (request->source == NULL)
			return 0;
		if (request->source->i_type != request->type)
			return 0;
		return 1;
	}

	if (request->source != NULL)
		return 0;
	return 1;
}

/* Copies one ACL attribute from a source inode to a new child. */
static int
inode_creation_preserve_acl(
	struct inode *source,
	struct inode *child,
	const char *name)
{
	struct posix_acl acl;
	int error;

	/* A source without this ACL leaves the child without one. */
	error = posix_acl_load(source, name, &acl);
	if (error == ENODATA || error == EOPNOTSUPP)
		return 0;
	if (error != 0)
		return error;

	/* Copies the ACL onto the child. */

	/* Reports the failure. */
	error = posix_acl_store(child, name, &acl);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Tests whether two inode pointers name the same object. */
static int
inode_same(
	const struct inode *left,
	const struct inode *right)
{
	if (left == right)
		return 1;
	if (left == NULL || right == NULL)
		return 0;
	if (left->i_mount != right->i_mount)
		return 0;
	if (left->i_ino != right->i_ino)
		return 0;
	return 1;
}

/* Moves a cursor one component towards the root, transferring the reference. */
static int
inode_parent_step(
	struct inode **cursor,
	int *at_root)
{
	static const struct componentname dotdot = {
		.cn_nameptr = "..",
		.cn_namelen = 2,
		.cn_flags = COMPONENT_DOTDOT,
	};
	struct inode *current;
	struct inode *parent;
	int error;

	/*
	 * The VFS transaction lock held by the rename caller keeps every
	 * observed ".." entry stable until the final filesystem rename
	 * commits.
	 */
	if (cursor == NULL || *cursor == NULL || at_root == NULL)
		return EINVAL;
	current = *cursor;
	*at_root = 0;
	if (current->i_mount == NULL || current->i_mount->m_root == NULL)
		return EIO;
	if (inode_same(current, current->i_mount->m_root)) {
		*at_root = 1;
		return 0;
	}

	/* The parent must be a directory of the same mount other than itself. */
	error = inode_lookup(current, &dotdot, &parent);
	if (error != 0)
		return error;
	if (parent->i_type != INODE_DIR || parent->i_mount != current->i_mount) {
		inode_release(parent);
		return EIO;
	}

	if (inode_same(parent, current)) {
		inode_release(parent);
		return EIO;
	}

	inode_release(current);
	*cursor = parent;
	return 0;
}

/* Tests whether the VM object resize protocol is linked in. */
static int
inode_vm_resize_available(
	void)
{
	if (vm_object_inode_io_wait == NULL)
		return 0;
	if (vm_object_inode_resize_active == NULL)
		return 0;
	if (vm_object_resize_begin == NULL)
		return 0;
	if (vm_object_resize_prepare == NULL)
		return 0;
	if (vm_object_resize_commit == NULL)
		return 0;
	if (vm_object_resize_abort == NULL)
		return 0;
	return 1;
}

/* Runs a truncate transaction under the inode I/O lock with a published resize. */
static int
inode_truncate_transaction_impl(
	struct inode *i,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	struct vm_object_resize resize;
	struct inode_truncate_result local_result;
	struct inode_truncate_result inner;
	int delegated;
	int vm_resize;
	int error;

	/* Reports the current size even on a rejected request. */
	if (result == NULL)
		result = &local_result;
	memset(result, 0, sizeof(*result));
	if (i != NULL)
		result->actual_size = i->i_size;
	else
		result->actual_size = 0;
	if (i == NULL || request == NULL || request->size < 0)
		return EINVAL;
	if (i->i_flags & (INODE_SWAPFILE | INODE_LOOPFILE))
		return EBUSY;
	if (readonly(i))
		return EROFS;

	/* Prepares copy-up before entering the inode I/O domain. */
	if (i->i_op != NULL && i->i_op->prepare_mutation != NULL) {
		error = i->i_op->prepare_mutation(i);
		result->actual_size = i->i_size;
		if (error != 0)
			return error;
	}

	delegated = i->i_op != NULL && i->i_op->truncate_limited != NULL;
retry:
	/* Takes the I/O lock outside any published resize. */
	vm_resize = inode_vm_resize_available();
	if (vm_resize) {
		error = vm_object_inode_io_wait(i);
		if (error != 0)
			return error;
	}

	mutex_lock(&i->i_io_lock);

	/* Close the publication-to-i_io acquisition window. */
	if (vm_resize && vm_object_inode_resize_active(i)) {
		mutex_unlock(&i->i_io_lock);
		goto retry;
	}

	/*
	 * RLIMIT_FSIZE constrains growth, not shrinking or replacement of
	 * data in a file which was already larger when the process limit
	 * was lowered.  Check under i_io_lock so another writer cannot
	 * change the comparison between validation and the filesystem
	 * truncate transaction.
	 */
	if (!delegated && request->size > i->i_size &&
	    (uint64_t)request->size > request->growth_limit) {
		result->limit_exceeded = 1;
		result->actual_size = i->i_size;
		mutex_unlock(&i->i_io_lock);
		return EFBIG;
	}

	/* Publishes and prepares the resize. */
	memset(&resize, 0, sizeof(resize));
	if (vm_resize) {
		error = vm_object_resize_begin(i, request->size, &resize);
		if (error == EBUSY || error == EAGAIN) {
			mutex_unlock(&i->i_io_lock);
			goto retry;
		}

		if (error != 0) {
			mutex_unlock(&i->i_io_lock);
			return error;
		}
	}

	if (resize.active) {
		/*
		 * Fault I/O which predates begin may already be committed to
		 * taking i_io_lock, so preparation must wait without holding
		 * it.
		 */
		mutex_unlock(&i->i_io_lock);
		error = vm_object_resize_prepare(&resize);
		mutex_lock(&i->i_io_lock);
		if (error != 0) {
			vm_object_resize_abort(&resize);
			mutex_unlock(&i->i_io_lock);
			return error;
		}
	}

	/*
	 * Exclude exec/content publication while removing privilege bits.
	 * Doing this immediately before the backend mutation prevents an
	 * executable image from observing new bytes with the old set-id
	 * mode.
	 */
	if (!delegated &&
	    (request->credential != NULL || request->content_change)) {
		if (request->content_change)
			error = vfs_clear_setid_on_content_change(i);
		else
			error = vfs_clear_setid_on_write(i, request->credential);
		if (error != 0) {
			if (resize.active)
				vm_object_resize_abort(&resize);
			mutex_unlock(&i->i_io_lock);
			return error;
		}
	}

	/* Runs the filesystem truncate and commits the published size. */
	if (delegated) {
		memset(&inner, 0, sizeof(inner));
		inner.actual_size = i->i_size;
		error = i->i_op->truncate_limited(i, request, &inner);

		/*
		 * A stacking backend must report the final content inode's
		 * size on every outcome.  Publish it even after EIO: the
		 * mutation may have crossed its irreversible backend boundary
		 * before failing.
		 */
		if (inner.actual_size < 0) {
			if (error == 0)
				error = EIO;
			inner.actual_size = i->i_size;
		}

		i->i_size = inner.actual_size;
		result->actual_size = inner.actual_size;
		result->limit_exceeded = inner.limit_exceeded;
		if (resize.active)
			vm_object_resize_commit(&resize, inner.actual_size);
		if (error == 0)
			inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	} else {
		if (i->i_op != NULL && i->i_op->truncate != NULL)
			error = i->i_op->truncate(i, request->size);
		else
			error = EOPNOTSUPP;
		if (error == 0) {
			i->i_size = request->size;
			result->actual_size = request->size;
			if (resize.active)
				vm_object_resize_commit(&resize, request->size);
			inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
		} else {
			/*
			 * Set-id removal is irreversible even if the backend
			 * reports a later error.  The plain backend contract
			 * has not published a changed EOF, so abort only the
			 * tentative resize state.
			 */
			if (resize.active) {
				i->i_size = resize.old_size;
				vm_object_resize_abort(&resize);
			}

			result->actual_size = i->i_size;
		}
	}

	mutex_unlock(&i->i_io_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Builds a truncate request and runs it as a transaction. */
static int
inode_truncate_limited_impl(
	struct inode *i,
	off_t size,
	uint64_t growth_limit,
	const struct ucred *cred,
	int content_change,
	int *limit_exceeded)
{
	const struct inode_truncate_request request = {
		.size = size,
		.growth_limit = growth_limit,
		.credential = cred,
		.content_change = content_change != 0,
	};
	struct inode_truncate_result result;
	int error;

	error = inode_truncate_transaction(i, &request, &result);
	if (limit_exceeded != NULL)
		*limit_exceeded = result.limit_exceeded;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Tests whether an extended attribute name is non-empty and bounded. */
static int
xattr_name_valid(
	const char *name)
{
	size_t length;

	/* Measures the name without reading past the permitted length. */
	if (name == NULL)
		return 0;
	length = 0;
	while (length <= INODE_XATTR_NAME_MAX && name[length] != '\0')
		length++;

	/* An empty or overlong name is not a valid attribute name. */
	if (length == 0)
		return 0;
	if (length > INODE_XATTR_NAME_MAX)
		return 0;

	/* Reports a usable name. */
	return 1;
}

/*
 * Join before checking attachments and retain the sleeping gate through the
 * backend commit. The outer syscall/copy-up owner keeps its own acquisition.
 */
static int
inode_namespace_enter(
	struct inode *directory,
	const struct componentname *name,
	int creation,
	int *entered)
{
	int error;

	*entered = 0;
	if (directory == NULL || name == NULL)
		return EINVAL;
	*entered = mount_vfs_transaction_join(directory->i_mount);
	if (readonly(directory))
		return EROFS;
	if ((directory->i_flags & INODE_DEAD) != 0)
		return ENOENT;
	error = mount_namespace_check_name(directory, name);
	return creation && error == EBUSY ? EEXIST : error;
}
