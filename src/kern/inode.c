/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

extern int posix_acl_chmod(struct inode *, mode_t) __attribute__((weak));
extern int posix_acl_inherit(struct inode *, struct inode *, mode_t *)
    __attribute__((weak));
extern int vm_object_inode_io_wait(struct inode *) __attribute__((weak));
extern int vm_object_inode_resize_active(struct inode *)
    __attribute__((weak));
extern int vm_object_resize_begin(struct inode *, off_t,
    struct vm_object_resize *) __attribute__((weak));
extern int vm_object_resize_prepare(struct vm_object_resize *)
    __attribute__((weak));
extern void vm_object_resize_commit(struct vm_object_resize *, off_t)
    __attribute__((weak));
extern void vm_object_resize_abort(struct vm_object_resize *)
    __attribute__((weak));

#define INODE_COMMON_MAX 256U
#define INODE_CACHE_MAX 512U
#define VFS_BSS __attribute__((section(".vfs_bss")))
#define INODE_HIGH __attribute__((section(".hightext")))

static struct inode common_pool[INODE_COMMON_MAX] VFS_BSS;
static uint8_t common_used[INODE_COMMON_MAX] VFS_BSS;
static struct inode *inode_cache[INODE_CACHE_MAX] VFS_BSS;
static struct spinlock inode_cache_lock = {
	{ 0 }, LOCK_RANK_INODE, "inode cache", 0, 0
};
#define INODE_CACHE_RESERVED ((struct inode *)(uintptr_t)1U)

mode_t
inode_type_mode(enum inode_type type)
{
	switch (type) {
	case INODE_REG: return S_IFREG;
	case INODE_DIR: return S_IFDIR;
	case INODE_BLOCK: return S_IFBLK;
	case INODE_CHAR: return S_IFCHR;
	case INODE_SYMLINK: return S_IFLNK;
	case INODE_SOCKET: return S_IFSOCK;
	case INODE_FIFO: return S_IFIFO;
	default: return 0;
	}
}

static int
common_index(const struct inode *inode)
{
	unsigned i;
	for (i = 0; i < INODE_COMMON_MAX; i++)
		if (&common_pool[i] == inode)
			return (int)i;
	return -1;
}

static int
cache_index(const struct inode *inode)
{
	unsigned i;
	for (i = 0; i < INODE_CACHE_MAX; i++)
		if (inode_cache[i] == inode)
			return (int)i;
	return -1;
}

static void
destroy_inode(struct inode *inode)
{
	struct mount *mountp;
	void *special;
	void (*special_destroy)(void *);
	int pindex;
	unsigned long irq;

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

static int
reserve_cache_slot(struct inode **victim)
{
	unsigned i;
	unsigned long irq = spin_lock_irqsave(&inode_cache_lock);
	*victim = NULL;
	for (i = 0; i < INODE_CACHE_MAX; i++)
		if (inode_cache[i] == NULL) {
			inode_cache[i] = INODE_CACHE_RESERVED;
			spin_unlock_irqrestore(&inode_cache_lock, irq);
			return (int)i;
		}
	for (i = 0; i < INODE_CACHE_MAX; i++) {
		struct inode *inode = inode_cache[i];
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

struct inode *
inode_alloc(struct mount *mountp)
{
	struct inode *inode = NULL;
	struct inode *victim;
	unsigned long irq;
	int slot;
	unsigned i;

	slot = reserve_cache_slot(&victim);
	if (slot < 0)
		return NULL;
	if (victim != NULL)
		destroy_inode(victim);
	if (mountp != NULL && mountp->m_type != NULL &&
	    mountp->m_type->alloc_inode != NULL)
		inode = mountp->m_type->alloc_inode(mountp);
	else {
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
	memset(inode, 0, sizeof(*inode));
	inode->i_mount = mountp;
	inode->i_dirseq = 1;
	/* One cache reference and one reference returned to the caller. */
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

void
inode_free(struct inode *inode)
{
	int cindex;
	unsigned long irq;

	if (inode == NULL || refcount_load(&inode->i_refs) != 1 ||
	    (inode->i_flags & INODE_DIRTY))
		return;
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

int
inode_get(struct mount *mountp, ino_t ino, struct inode **result)
{
	unsigned i;
	unsigned long irq;
	if (mountp == NULL || result == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&inode_cache_lock);
	for (i = 0; i < INODE_CACHE_MAX; i++) {
		struct inode *inode = inode_cache[i];
		if (inode != NULL && inode != INODE_CACHE_RESERVED &&
		    inode->i_mount == mountp &&
		    inode->i_ino == ino && !(inode->i_flags & INODE_DEAD)) {
			inode_ref(inode);
			*result = inode;
			spin_unlock_irqrestore(&inode_cache_lock, irq);
			return 0;
		}
	}
	spin_unlock_irqrestore(&inode_cache_lock, irq);
	return ENOENT;
}

void inode_ref(struct inode *inode)
{
	if (inode != NULL)
		refcount_get(&inode->i_refs);
}

void inode_release(struct inode *inode)
{
	if (inode != NULL) {
		unsigned remaining = refcount_put_not_last(&inode->i_refs);
		if (remaining == 1 &&
		    (inode->i_flags & INODE_DEAD) != 0 &&
		    (inode->i_flags & (INODE_DIRTY | INODE_ROOT)) == 0)
			inode_free(inode);
	}
}

void
inode_cache_purge_mount(struct mount *mountp)
{
	for (;;) {
		struct inode *victim = NULL;
		unsigned long irq = spin_lock_irqsave(&inode_cache_lock);
		unsigned i;
		for (i = 0; i < INODE_CACHE_MAX; i++) {
			struct inode *inode = inode_cache[i];
			if (inode != NULL && inode != INODE_CACHE_RESERVED &&
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

INODE_HIGH int
inode_cache_mount_busy(struct mount *mountp)
{
	unsigned i;
	unsigned long irq;
	int busy = 0;
	irq = spin_lock_irqsave(&inode_cache_lock);
	for (i = 0; i < INODE_CACHE_MAX; i++)
		if (inode_cache[i] != NULL &&
		    inode_cache[i] != INODE_CACHE_RESERVED &&
		    inode_cache[i]->i_mount == mountp &&
		    refcount_load(&inode_cache[i]->i_refs) >
		    (inode_cache[i] == mountp->m_root ? 2U : 1U)) {
			busy = 1;
			break;
		}
	spin_unlock_irqrestore(&inode_cache_lock, irq);
	return busy ? EBUSY : 0;
}

INODE_HIGH unsigned
inode_cache_mount_count(struct mount *mountp)
{
	unsigned i, count = 0;
	unsigned long irq = spin_lock_irqsave(&inode_cache_lock);
	for (i = 0; i < INODE_CACHE_MAX; i++)
		if (inode_cache[i] != NULL && inode_cache[i] != INODE_CACHE_RESERVED &&
		    inode_cache[i]->i_mount == mountp)
			count++;
	spin_unlock_irqrestore(&inode_cache_lock, irq);
	return count;
}

void
inode_cache_reset(void)
{
	namecache_reset();
	for (;;) {
		struct inode *victim = NULL;
		unsigned long irq = spin_lock_irqsave(&inode_cache_lock);
		unsigned i;
		for (i = 0; i < INODE_CACHE_MAX; i++) {
			struct inode *inode = inode_cache[i];
			if (inode != NULL && inode != INODE_CACHE_RESERVED &&
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

static int readonly(const struct inode *inode)
{
	return inode != NULL && inode->i_mount != NULL &&
	       (inode->i_mount->m_flags & MOUNT_READ_ONLY);
}

int
inode_lookup(struct inode *directory, const struct componentname *name,
	     struct inode **result)
{
	struct inode *child;
	int error;
	if (directory == NULL || name == NULL || result == NULL ||
	    name->cn_namelen == 0 || name->cn_namelen > NAME_MAX)
		return EINVAL;
	if (directory->i_type != INODE_DIR)
		return ENOTDIR;
	if ((directory->i_flags & INODE_NOCACHE_CHILDREN) == 0 &&
	    namecache_lookup(directory, name, result) == 0)
		return 0;
	if (directory->i_op == NULL || directory->i_op->lookup == NULL)
		return EOPNOTSUPP;
	error = directory->i_op->lookup(directory, name, &child);
	if (error != 0)
		return error;
	if (child == NULL || child->i_mount == NULL || child->i_type == INODE_NONE) {
		if (child != NULL)
			inode_release(child);
		return EIO;
	}
	if ((directory->i_flags & INODE_NOCACHE_CHILDREN) == 0)
		(void)namecache_enter(directory, name, child);
	*result = child;
	return 0;
}

int
inode_lookup_casefold(struct inode *directory,
		      const struct componentname *name, struct inode **result)
{
	if (directory == NULL || name == NULL || result == NULL ||
	    name->cn_namelen == 0 || name->cn_namelen > NAME_MAX)
		return EINVAL;
	if (directory->i_type != INODE_DIR)
		return ENOTDIR;
	if (directory->i_op == NULL || directory->i_op->lookup_casefold == NULL)
		return EOPNOTSUPP;
	return directory->i_op->lookup_casefold(directory, name, result);
}

int
inode_getattr(struct inode *inode, struct stat *status)
{
	if (inode == NULL || status == NULL)
		return EINVAL;
	if (inode->i_op != NULL && inode->i_op->getattr != NULL)
		return inode->i_op->getattr(inode, status);
	memset(status, 0, sizeof(*status));
	status->st_dev = inode->i_mount != NULL && inode->i_mount->m_disk != NULL ?
		inode->i_mount->m_disk->d_dev : 0;
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
	status->st_blocks = inode->i_size > 0 ?
	    (blkcnt_t)(((uint64_t)inode->i_size + 511U) / 512U) : 0;
	return 0;
}

void
inode_touch(struct inode *inode, unsigned mask)
{
	struct inode_time now;
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

void
inode_dir_changed(struct inode *inode)
{
	uint64_t sequence;

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

/* Enter the regular-file content domain before changing privilege metadata.
 * A content/resize owner deliberately drops i_io while revoking mappings and
 * writing old dirty data, so taking the mutex alone would enter the middle of
 * its transaction.  Wait for the publication gate, acquire i_io, then recheck
 * to close that hand-off window. */
static int
inode_content_io_lock(struct inode *inode)
{
	int error;

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

int inode_setattr(struct inode *i, const struct stat *s, unsigned mask)
{
	const unsigned valid = INODE_ATTR_MODE | INODE_ATTR_UID |
		INODE_ATTR_GID | INODE_ATTR_SIZE | INODE_ATTR_ATIME |
		INODE_ATTR_MTIME | INODE_ATTR_CTIME |
		INODE_ATTR_ATIME_NOW | INODE_ATTR_MTIME_NOW;
	struct stat requested;
	struct inode_time now;
	int held_io = 0;
	int error;

	if (i == NULL || s == NULL || (mask & ~valid) != 0)
		return EINVAL;
	if (readonly(i))
		return EROFS;
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
	/* EOF changes use the same VM-visible transaction as ftruncate and
	 * extending write.  Filesystem setattr callbacks therefore never mutate
	 * size behind an already-published shared object's generation. */
	if ((mask & INODE_ATTR_SIZE) != 0) {
		error = inode_truncate(i, requested.st_size);
		if (error != 0)
			return error;
		mask &= ~INODE_ATTR_SIZE;
		if (mask == 0)
			return 0;
	}
	/* Mode and ownership participate in the same regular-inode I/O domain as
	 * writes and truncate.  This prevents chmod/chown from restoring set-id
	 * bits between a writer's privilege-bit invalidation and its backend
	 * content mutation.  Stacked filesystems naturally acquire outer then
	 * content-inode locks through their setattr callback. */
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
	return error;
}
static int
creation_request_valid(const struct inode_creation_request *request)
{
	if (request == NULL ||
	    request->origin < INODE_CREATION_USER ||
	    request->origin > INODE_CREATION_PRESERVE ||
	    request->type <= INODE_NONE || request->type > INODE_FIFO ||
	    (request->mode & S_IFMT) != 0)
		return 0;
	if (request->type != INODE_CHAR && request->type != INODE_BLOCK &&
	    request->rdev != 0)
		return 0;
	if ((request->type == INODE_SOCKET) != (request->special != NULL))
		return 0;
	if (request->origin == INODE_CREATION_PRESERVE)
		return request->source != NULL &&
		    request->source->i_type == request->type;
	return request->source == NULL;
}

int
inode_creation_request_user(struct inode *parent,
	const struct ucred *credential, enum inode_type type, mode_t mode,
	dev_t rdev, void *special, struct inode_creation_request *request)
{
	int error;

	if (parent == NULL || credential == NULL || request == NULL ||
	    type <= INODE_NONE || type > INODE_FIFO || (mode & S_IFMT) != 0)
		return EINVAL;
	memset(request, 0, sizeof(*request));
	/* Authorization and the set-GID/GID snapshot are one metadata
	 * observation.  inode_setattr() publishes parent mode and ownership under
	 * i_io_lock; taking the same domain prevents a child from combining an
	 * old S_ISGID bit with a newly published parent GID. */
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
	request->gid = (parent->i_mode & S_ISGID) != 0 ?
	    parent->i_gid : credential->egid;
	if (type == INODE_DIR && (parent->i_mode & S_ISGID) != 0)
		request->mode |= S_ISGID;
	request->rdev = rdev;
	request->special = special;
	mutex_unlock(&parent->i_io_lock);
	return creation_request_valid(request) ? 0 : EINVAL;
}

int
inode_creation_request_system(enum inode_type type, mode_t mode, uid_t uid,
	gid_t gid, dev_t rdev, struct inode_creation_request *request)
{
	if (request == NULL || type <= INODE_NONE || type > INODE_FIFO ||
	    (mode & S_IFMT) != 0)
		return EINVAL;
	memset(request, 0, sizeof(*request));
	request->origin = INODE_CREATION_SYSTEM;
	request->type = type;
	request->mode = mode & 07777U;
	request->uid = uid;
	request->gid = gid;
	request->rdev = rdev;
	return creation_request_valid(request) ? 0 : EINVAL;
}

int
inode_creation_request_preserve(const struct inode *source,
	struct inode_creation_request *request)
{
	if (source == NULL || request == NULL || source->i_type <= INODE_NONE ||
	    source->i_type > INODE_FIFO)
		return EINVAL;
	memset(request, 0, sizeof(*request));
	request->origin = INODE_CREATION_PRESERVE;
	request->type = source->i_type;
	request->mode = source->i_mode & 07777U;
	request->uid = source->i_uid;
	request->gid = source->i_gid;
	request->rdev = source->i_rdev;
	request->special = source->i_type == INODE_SOCKET ?
	    source->i_special : NULL;
	request->source = source;
	return creation_request_valid(request) ? 0 : EINVAL;
}

static int
inode_creation_preserve_acl(struct inode *source, struct inode *child,
	const char *name)
{
	struct posix_acl acl;
	int error;

	error = posix_acl_load(source, name, &acl);
	if (error == ENODATA || error == EOPNOTSUPP)
		return 0;
	if (error != 0)
		return error;
	return posix_acl_store(child, name, &acl);
}

int
inode_creation_prepare(struct inode *parent, struct inode *child,
	const struct inode_creation_request *request)
{
	mode_t inherited;
	int error;

	if (parent == NULL || child == NULL || !creation_request_valid(request) ||
	    child->i_type != request->type ||
	    (request->special != NULL && request->type != INODE_SOCKET))
		return EINVAL;
	child->i_mode = inode_type_mode(request->type) |
	    (request->mode & 07777U);
	child->i_uid = request->uid;
	child->i_gid = request->gid;
	child->i_rdev = request->rdev;
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
	if (request->special != NULL) {
		mutex_lock(&child->i_lock);
		if (child->i_special != NULL)
			error = EADDRINUSE;
		else {
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

int inode_create(struct inode *i, const struct componentname *n,
		 const struct inode_creation_request *request, struct inode **r)
{
	int error;
	if (i == NULL || n == NULL || r == NULL ||
	    !creation_request_valid(request) || request->type != INODE_REG)
		return EINVAL;
	if (readonly(i)) return EROFS;
	error = i->i_op != NULL && i->i_op->create != NULL ?
		i->i_op->create(i, n, request, r) : EOPNOTSUPP;
	if (error == 0) {
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}
	return error;
}
int inode_mkdir(struct inode *i, const struct componentname *n,
		const struct inode_creation_request *request, struct inode **r)
{
	int error;
	if (i == NULL || n == NULL || r == NULL ||
	    !creation_request_valid(request) || request->type != INODE_DIR)
		return EINVAL;
	if (readonly(i)) return EROFS;
	error = i->i_op != NULL && i->i_op->mkdir != NULL ?
		i->i_op->mkdir(i, n, request, r) : EOPNOTSUPP;
	if (error == 0) {
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}
	return error;
}
int inode_mknod(struct inode *i, const struct componentname *n,
		const struct inode_creation_request *request, struct inode **r)
{
	int error;
	if (i == NULL || n == NULL || r == NULL ||
	    !creation_request_valid(request))
		return EINVAL;
	if (request->type != INODE_FIFO && request->type != INODE_SOCKET &&
	    request->type != INODE_CHAR && request->type != INODE_BLOCK)
		return EOPNOTSUPP;
	if (readonly(i))
		return EROFS;
	error = i->i_op != NULL && i->i_op->mknod != NULL ?
		i->i_op->mknod(i, n, request, r) :
		EOPNOTSUPP;
	if (error == 0) {
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}
	return error;
}
int inode_unlink(struct inode *i, const struct componentname *n)
{
	struct inode *target;
	struct backing_mutation_guard guard;
	int error;
	if (i == NULL || n == NULL) return EINVAL;
	if (readonly(i)) return EROFS;
	error = inode_lookup(i, n, &target);
	if (error != 0) return error;
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
	if ((target->i_flags & (INODE_ROOT | INODE_MOUNTPOINT |
	    INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		backing_mutation_end(&guard);
		inode_release(target);
		return EBUSY;
	}
	inode_release(target);
	error = i->i_op != NULL && i->i_op->unlink != NULL ?
		i->i_op->unlink(i, n) : EOPNOTSUPP;
	backing_mutation_end(&guard);
	if (error == 0) {
		namecache_remove(i, n);
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}
	return error;
}
int inode_rmdir(struct inode *i, const struct componentname *n)
{
	struct inode *target;
	int error;
	if (i == NULL || n == NULL) return EINVAL;
	if (readonly(i)) return EROFS;
	error = inode_lookup(i, n, &target);
	if (error != 0) return error;
	if (target->i_type != INODE_DIR) {
		inode_release(target);
		return ENOTDIR;
	}
	if ((target->i_flags & (INODE_ROOT | INODE_MOUNTPOINT |
	    INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		inode_release(target);
		return EBUSY;
	}
	inode_release(target);
	error = i->i_op != NULL && i->i_op->rmdir != NULL ?
		i->i_op->rmdir(i, n) : EOPNOTSUPP;
	if (error == 0) {
		namecache_remove(i, n);
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}
	return error;
}

static int
inode_same(const struct inode *left, const struct inode *right)
{
	return left == right || (left != NULL && right != NULL &&
	    left->i_mount == right->i_mount && left->i_ino == right->i_ino);
}

/*
 * Advance one component towards the root.  The VFS transaction lock held by
 * the rename caller keeps every observed ".." entry stable until the final
 * filesystem rename commits.  References are transferred through *cursor.
 */
static int
inode_parent_step(struct inode **cursor, int *at_root)
{
	static const struct componentname dotdot = {
		.cn_nameptr = "..",
		.cn_namelen = 2,
		.cn_flags = COMPONENT_DOTDOT,
	};
	struct inode *current, *parent;
	int error;

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

/*
 * A directory cannot become a child of itself.  Walk the destination parent
 * chain in the generic layer so every filesystem gets identical semantics.
 * The second cursor is Floyd cycle detection: a malformed pre-existing tree
 * is reported as EIO instead of making rename loop forever.
 */
static int
inode_rename_ancestor_check(struct inode *source, struct inode *new_parent)
{
	struct inode *current = new_parent, *fast = new_parent;
	int error = 0, at_root;

	inode_ref(current);
	inode_ref(fast);
	for (;;) {
		unsigned step;

		if (inode_same(current, source)) {
			error = EINVAL;
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
	return error;
}

int inode_rename(struct inode *od, const struct componentname *on,
		 struct inode *nd, const struct componentname *nn, unsigned flags)
{
	struct inode *source, *target;
	struct backing_mutation_guard source_guard, target_guard;
	int target_guarded = 0;
	int error;
	if (od == NULL || on == NULL || nd == NULL || nn == NULL || flags != 0)
		return EINVAL;
	if (od->i_mount != nd->i_mount)
		return EXDEV;
	if (readonly(od) || readonly(nd))
		return EROFS;
	error = inode_lookup(od, on, &source);
	if (error != 0)
		return error;
	error = backing_mutation_begin_inode(source, &source_guard);
	if (error != 0) {
		inode_release(source);
		return error;
	}
	if ((source->i_flags & (INODE_ROOT | INODE_MOUNTPOINT |
	    INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		backing_mutation_end(&source_guard);
		inode_release(source);
		return EBUSY;
	}
	if (source->i_type == INODE_DIR && !inode_same(od, nd)) {
		error = inode_rename_ancestor_check(source, nd);
		if (error != 0) {
			backing_mutation_end(&source_guard);
			inode_release(source);
			return error;
		}
	}
	error = inode_lookup(nd, nn, &target);
	if (error == 0) {
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
		if ((target->i_flags & (INODE_ROOT | INODE_MOUNTPOINT |
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
	error = od->i_op != NULL && od->i_op->rename != NULL ?
		od->i_op->rename(od, on, nd, nn, flags) : EOPNOTSUPP;
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
	return error;
}
int inode_link(struct inode *directory, const struct componentname *name,
	       struct inode *target)
{
	struct backing_mutation_guard guard;
	int error;
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
	error = backing_mutation_begin_inode(target, &guard);
	if (error != 0)
		return error;
	error = directory->i_op != NULL && directory->i_op->link != NULL ?
		directory->i_op->link(directory, name, target) : EOPNOTSUPP;
	backing_mutation_end(&guard);
	if (error == 0) {
		inode_dir_changed(directory);
		target->i_linkcount++;
		inode_touch(target, INODE_ATTR_CTIME);
		inode_touch(directory, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}
	return error;
}
int inode_symlink(struct inode *directory, const struct componentname *name,
		  const char *target,
		  const struct inode_creation_request *request,
		  struct inode **result)
{
	int error;
	if (directory == NULL || name == NULL || target == NULL || result == NULL ||
	    !creation_request_valid(request) || request->type != INODE_SYMLINK)
		return EINVAL;
	if (directory->i_type != INODE_DIR)
		return ENOTDIR;
	if (target[0] == '\0')
		return ENOENT;
	if (readonly(directory))
		return EROFS;
	error = directory->i_op != NULL && directory->i_op->symlink != NULL ?
		directory->i_op->symlink(directory, name, target, request, result) :
		EOPNOTSUPP;
	if (error == 0) {
		inode_dir_changed(directory);
		inode_touch(directory, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}
	return error;
}
ssize_t inode_readlink(struct inode *inode, char *buffer, size_t capacity)
{
	if (inode == NULL || buffer == NULL)
		return -EINVAL;
	if (inode->i_type != INODE_SYMLINK)
		return -EINVAL;
	return inode->i_op != NULL && inode->i_op->readlink != NULL ?
		inode->i_op->readlink(inode, buffer, capacity) : -EOPNOTSUPP;
}

static int
inode_vm_resize_available(void)
{
	return vm_object_inode_io_wait != NULL &&
	    vm_object_inode_resize_active != NULL &&
	    vm_object_resize_begin != NULL &&
	    vm_object_resize_prepare != NULL &&
	    vm_object_resize_commit != NULL &&
	    vm_object_resize_abort != NULL;
}

static int
inode_truncate_transaction_impl(struct inode *i,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	struct vm_object_resize resize;
	struct inode_truncate_result local_result;
	int delegated;
	int vm_resize;
	int error;

	if (result == NULL)
		result = &local_result;
	memset(result, 0, sizeof(*result));
	result->actual_size = i != NULL ? i->i_size : 0;
	if (i == NULL || request == NULL || request->size < 0)
		return EINVAL;
	if (i->i_flags & (INODE_SWAPFILE | INODE_LOOPFILE)) return EBUSY;
	if (readonly(i)) return EROFS;
	delegated = i->i_op != NULL && i->i_op->truncate_limited != NULL;
	retry:
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
	/* RLIMIT_FSIZE constrains growth, not shrinking or replacement of data in
	 * a file which was already larger when the process limit was lowered.
	 * Check under i_io_lock so another writer cannot change the comparison
	 * between validation and the filesystem truncate transaction. */
	if (!delegated && request->size > i->i_size &&
	    (uint64_t)request->size > request->growth_limit) {
		result->limit_exceeded = 1;
		result->actual_size = i->i_size;
		mutex_unlock(&i->i_io_lock);
		return EFBIG;
	}
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
		/* Fault I/O which predates begin may already be committed to taking
		 * i_io_lock, so preparation must wait without holding it. */
		mutex_unlock(&i->i_io_lock);
		error = vm_object_resize_prepare(&resize);
		mutex_lock(&i->i_io_lock);
		if (error != 0) {
			vm_object_resize_abort(&resize);
			mutex_unlock(&i->i_io_lock);
			return error;
		}
	}
	/* Exclude exec/content publication while removing privilege bits.  Doing
	 * this immediately before the backend mutation prevents an executable
	 * image from observing new bytes with the old set-id mode. */
	if (!delegated &&
	    (request->credential != NULL || request->content_change)) {
		error = request->content_change ?
		    vfs_clear_setid_on_content_change(i) :
		    vfs_clear_setid_on_write(i, request->credential);
		if (error != 0) {
			if (resize.active)
				vm_object_resize_abort(&resize);
			mutex_unlock(&i->i_io_lock);
			return error;
		}
	}
	if (delegated) {
		struct inode_truncate_result inner;

		memset(&inner, 0, sizeof(inner));
		inner.actual_size = i->i_size;
		error = i->i_op->truncate_limited(i, request, &inner);
		/* A stacking backend must report the final content inode's size on
		 * every outcome.  Publish it even after EIO: the mutation may have
		 * crossed its irreversible backend boundary before failing. */
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
		error = i->i_op != NULL && i->i_op->truncate != NULL ?
			i->i_op->truncate(i, request->size) : EOPNOTSUPP;
		if (error == 0) {
			i->i_size = request->size;
			result->actual_size = request->size;
			if (resize.active)
				vm_object_resize_commit(&resize, request->size);
			inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
		} else {
			/* Set-id removal is irreversible even if the backend reports a
			 * later error.  The plain backend contract has not published a
			 * changed EOF, so abort only the tentative resize state. */
			if (resize.active) {
				i->i_size = resize.old_size;
				vm_object_resize_abort(&resize);
			}
			result->actual_size = i->i_size;
		}
	}
	mutex_unlock(&i->i_io_lock);
	return error;
}

int
inode_truncate_transaction(struct inode *i,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	struct backing_mutation_guard guard;
	int error;

	if (i == NULL || request == NULL)
		return inode_truncate_transaction_impl(i, request, result);
	error = backing_mutation_begin_inode(i, &guard);
	if (error != 0)
		return error;
	error = inode_truncate_transaction_impl(i, request, result);
	backing_mutation_end(&guard);
	return error;
}

static int
inode_truncate_limited_impl(struct inode *i, off_t size,
	uint64_t growth_limit, const struct ucred *cred, int content_change,
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
	return error;
}

int
inode_truncate_limited_cred(struct inode *i, off_t size,
	uint64_t growth_limit, const struct ucred *cred, int *limit_exceeded)
{
	return inode_truncate_limited_impl(i, size, growth_limit, cred, 0,
	    limit_exceeded);
}

int
inode_truncate_limited(struct inode *i, off_t size, uint64_t growth_limit,
	int *limit_exceeded)
{
	return inode_truncate_limited_impl(i, size, growth_limit, NULL, 0,
	    limit_exceeded);
}

int
inode_truncate(struct inode *i, off_t size)
{
	return inode_truncate_limited_impl(i, size, UINT64_MAX, NULL, 0, NULL);
}

int
inode_truncate_content_change(struct inode *i, off_t size)
{
	return inode_truncate_limited_impl(i, size, UINT64_MAX, NULL, 1, NULL);
}

static int
xattr_name_valid(const char *name)
{
	size_t length;
	if (name == NULL)
		return 0;
	for (length = 0; length <= INODE_XATTR_NAME_MAX && name[length] != '\0';
	    length++) ;
	return length != 0 && length <= INODE_XATTR_NAME_MAX;
}

ssize_t
inode_getxattr(struct inode *inode, const char *name, void *value, size_t size)
{
	if (inode == NULL || !xattr_name_valid(name) ||
	    (value == NULL && size != 0) || size > INODE_XATTR_SIZE_MAX)
		return -EINVAL;
	return inode->i_op != NULL && inode->i_op->getxattr != NULL ?
		inode->i_op->getxattr(inode, name, value, size) : -EOPNOTSUPP;
}

int
inode_setxattr(struct inode *inode, const char *name, const void *value,
	size_t size, unsigned flags)
{
	int error;
	if (inode == NULL || !xattr_name_valid(name) ||
	    (value == NULL && size != 0) || size > INODE_XATTR_SIZE_MAX ||
	    (flags & ~(INODE_XATTR_CREATE | INODE_XATTR_REPLACE)) != 0 ||
	    flags == (INODE_XATTR_CREATE | INODE_XATTR_REPLACE))
		return EINVAL;
	if (readonly(inode))
		return EROFS;
	error = inode->i_op != NULL && inode->i_op->setxattr != NULL ?
		inode->i_op->setxattr(inode, name, value, size, flags) :
		EOPNOTSUPP;
	if (error == 0)
		inode_touch(inode, INODE_ATTR_CTIME);
	return error;
}

ssize_t
inode_listxattr(struct inode *inode, char *list, size_t size)
{
	if (inode == NULL || (list == NULL && size != 0) ||
	    size > INODE_XATTR_SIZE_MAX)
		return -EINVAL;
	return inode->i_op != NULL && inode->i_op->listxattr != NULL ?
		inode->i_op->listxattr(inode, list, size) : -EOPNOTSUPP;
}

int
inode_removexattr(struct inode *inode, const char *name)
{
	int error;
	if (inode == NULL || !xattr_name_valid(name))
		return EINVAL;
	if (readonly(inode))
		return EROFS;
	error = inode->i_op != NULL && inode->i_op->removexattr != NULL ?
		inode->i_op->removexattr(inode, name) : EOPNOTSUPP;
	if (error == 0)
		inode_touch(inode, INODE_ATTR_CTIME);
	return error;
}

int inode_sync(struct inode *i)
{
	if (i == NULL) return EINVAL;
	return i->i_op != NULL && i->i_op->sync != NULL ? i->i_op->sync(i) : 0;
}

unsigned
inode_cache_count(void)
{
	unsigned i, count = 0;
	unsigned long irq = spin_lock_irqsave(&inode_cache_lock);
	for (i = 0; i < INODE_CACHE_MAX; i++)
		count += inode_cache[i] != NULL &&
		    inode_cache[i] != INODE_CACHE_RESERVED;
	spin_unlock_irqrestore(&inode_cache_lock, irq);
	return count;
}
