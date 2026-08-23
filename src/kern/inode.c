/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/inode.h"
#include "kern/mount.h"
#include "kern/namecache.h"
#include "kern/namei.h"
#include "kern/clock.h"
#include "kern/record-lock.h"
#include "kern/posix-acl.h"

#include <errno.h>
#include <string.h>

extern int posix_acl_chmod(struct inode *, mode_t) __attribute__((weak));
extern int posix_acl_inherit(struct inode *, struct inode *, mode_t *)
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
	if (inode == NULL || inode->i_type != INODE_DIR)
		return;
	if (inode->i_dirseq == UINT64_MAX) {
		namecache_purge_inode(inode);
		inode->i_dirseq = 1;
	} else {
		inode->i_dirseq++;
		if (inode->i_dirseq == 0)
			inode->i_dirseq = 1;
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
	if (i->i_op == NULL || i->i_op->setattr == NULL)
		return EOPNOTSUPP;
	error = i->i_op->setattr(i, &requested, mask);
	if (error != 0)
		return error;
	if ((mask & INODE_ATTR_MODE) != 0 && posix_acl_chmod != NULL) {
		error = posix_acl_chmod(i, requested.st_mode);
		if (error != 0)
			return error;
	}
	if (mask & INODE_ATTR_MODE)
		i->i_mode = (i->i_mode & S_IFMT) |
			(requested.st_mode & ~S_IFMT);
	if (mask & INODE_ATTR_UID)
		i->i_uid = requested.st_uid;
	if (mask & INODE_ATTR_GID)
		i->i_gid = requested.st_gid;
	if (mask & INODE_ATTR_SIZE)
		i->i_size = requested.st_size;
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
	return 0;
}
int inode_create(struct inode *i, const struct componentname *n, mode_t m,
		 struct inode **r)
{
	int error;
	if (i == NULL || n == NULL || r == NULL) return EINVAL;
	if (readonly(i)) return EROFS;
	error = i->i_op != NULL && i->i_op->create != NULL ?
		i->i_op->create(i, n, m, r) : EOPNOTSUPP;
	if (error == 0 && posix_acl_inherit != NULL) {
		mode_t inherited = (*r)->i_mode;
		error = posix_acl_inherit(i, *r, &inherited);
		if (error == 0 && inherited != (*r)->i_mode) {
			struct stat status;
			memset(&status, 0, sizeof(status));status.st_mode = inherited;
			error = inode_setattr(*r, &status, INODE_ATTR_MODE);
		}
		if (error != 0) {
			if (i->i_op != NULL && i->i_op->unlink != NULL)
				(void)i->i_op->unlink(i, n);
			(*r)->i_flags |= INODE_DEAD;
			inode_release(*r);*r = NULL;
		}
	}
	if (error == 0) {
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
		inode_touch(*r, INODE_ATTR_ATIME | INODE_ATTR_MTIME |
			INODE_ATTR_CTIME);
	}
	return error;
}
int inode_mkdir(struct inode *i, const struct componentname *n, mode_t m,
		struct inode **r)
{
	int error;
	if (i == NULL || n == NULL || r == NULL) return EINVAL;
	if (readonly(i)) return EROFS;
	error = i->i_op != NULL && i->i_op->mkdir != NULL ?
		i->i_op->mkdir(i, n, m, r) : EOPNOTSUPP;
	if (error == 0 && posix_acl_inherit != NULL) {
		mode_t inherited = (*r)->i_mode;
		error = posix_acl_inherit(i, *r, &inherited);
		if (error == 0 && inherited != (*r)->i_mode) {
			struct stat status;
			memset(&status, 0, sizeof(status));status.st_mode = inherited;
			error = inode_setattr(*r, &status, INODE_ATTR_MODE);
		}
		if (error != 0) {
			if (i->i_op != NULL && i->i_op->rmdir != NULL)
				(void)i->i_op->rmdir(i, n);
			(*r)->i_flags |= INODE_DEAD;
			inode_release(*r);*r = NULL;
		}
	}
	if (error == 0) {
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
		inode_touch(*r, INODE_ATTR_ATIME | INODE_ATTR_MTIME |
			INODE_ATTR_CTIME);
	}
	return error;
}
int inode_mknod(struct inode *i, const struct componentname *n,
		enum inode_type type, mode_t m, dev_t dev, struct inode **r)
{
	int error;
	mode_t expected;
	if (i == NULL || n == NULL || r == NULL)
		return EINVAL;
	if (type != INODE_FIFO && type != INODE_SOCKET &&
	    type != INODE_CHAR && type != INODE_BLOCK)
		return EOPNOTSUPP;
	expected = inode_type_mode(type);
	if ((m & S_IFMT) != 0 && (m & S_IFMT) != expected)
		return EINVAL;
	if (readonly(i))
		return EROFS;
	error = i->i_op != NULL && i->i_op->mknod != NULL ?
		i->i_op->mknod(i, n, type, expected | (m & 07777U), dev, r) :
		EOPNOTSUPP;
	if (error == 0) {
		inode_dir_changed(i);
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
		inode_touch(*r, INODE_ATTR_ATIME | INODE_ATTR_MTIME |
			INODE_ATTR_CTIME);
	}
	return error;
}
int inode_unlink(struct inode *i, const struct componentname *n)
{
	struct inode *target;
	int error;
	if (i == NULL || n == NULL) return EINVAL;
	if (readonly(i)) return EROFS;
	error = inode_lookup(i, n, &target);
	if (error != 0) return error;
	if (target->i_type == INODE_DIR) {
		inode_release(target);
		return EPERM;
	}
	if ((target->i_flags & (INODE_ROOT | INODE_MOUNTPOINT |
	    INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		inode_release(target);
		return EBUSY;
	}
	inode_release(target);
	error = i->i_op != NULL && i->i_op->unlink != NULL ?
		i->i_op->unlink(i, n) : EOPNOTSUPP;
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
int inode_rename(struct inode *od, const struct componentname *on,
		 struct inode *nd, const struct componentname *nn, unsigned flags)
{
	struct inode *source, *target;
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
	if ((source->i_flags & (INODE_ROOT | INODE_MOUNTPOINT |
	    INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		inode_release(source);
		return EBUSY;
	}
	error = inode_lookup(nd, nn, &target);
	if (error == 0) {
		if (target == source || (target->i_mount == source->i_mount &&
		    target->i_ino == source->i_ino)) {
			inode_release(target);
			inode_release(source);
			return 0;
		}
		if ((target->i_flags & (INODE_ROOT | INODE_MOUNTPOINT |
		    INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
			inode_release(target);
			inode_release(source);
			return EBUSY;
		}
		if (source->i_type == INODE_DIR && target->i_type != INODE_DIR) {
			inode_release(target);
			inode_release(source);
			return ENOTDIR;
		}
		if (source->i_type != INODE_DIR && target->i_type == INODE_DIR) {
			inode_release(target);
			inode_release(source);
			return EISDIR;
		}
		inode_release(target);
	} else if (error != ENOENT) {
		inode_release(source);
		return error;
	}
	error = od->i_op != NULL && od->i_op->rename != NULL ?
		od->i_op->rename(od, on, nd, nn, flags) : EOPNOTSUPP;
	inode_release(source);
	if (error == 0) {
		namecache_remove(od, on);
		namecache_remove(nd, nn);
		inode_dir_changed(od);
		if (nd != od)
			inode_dir_changed(nd);
		inode_touch(od, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
		if (nd != od)
			inode_touch(nd, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}
	return error;
}
int inode_link(struct inode *directory, const struct componentname *name,
	       struct inode *target)
{
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
	error = directory->i_op != NULL && directory->i_op->link != NULL ?
		directory->i_op->link(directory, name, target) : EOPNOTSUPP;
	if (error == 0) {
		inode_dir_changed(directory);
		target->i_linkcount++;
		inode_touch(target, INODE_ATTR_CTIME);
		inode_touch(directory, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	}
	return error;
}
int inode_symlink(struct inode *directory, const struct componentname *name,
		  const char *target, struct inode **result)
{
	int error;
	if (directory == NULL || name == NULL || target == NULL || result == NULL)
		return EINVAL;
	if (directory->i_type != INODE_DIR)
		return ENOTDIR;
	if (target[0] == '\0')
		return ENOENT;
	if (readonly(directory))
		return EROFS;
	error = directory->i_op != NULL && directory->i_op->symlink != NULL ?
		directory->i_op->symlink(directory, name, target, result) :
		EOPNOTSUPP;
	if (error == 0) {
		inode_dir_changed(directory);
		inode_touch(directory, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
		inode_touch(*result, INODE_ATTR_ATIME | INODE_ATTR_MTIME |
			INODE_ATTR_CTIME);
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
int inode_truncate(struct inode *i, off_t size)
{
	int error;
	if (i == NULL || size < 0) return EINVAL;
	if (i->i_flags & (INODE_SWAPFILE | INODE_LOOPFILE)) return EBUSY;
	if (readonly(i)) return EROFS;
	mutex_lock(&i->i_io_lock);
	error = i->i_op != NULL && i->i_op->truncate != NULL ?
		i->i_op->truncate(i, size) : EOPNOTSUPP;
	if (error == 0)
		inode_touch(i, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	mutex_unlock(&i->i_io_lock);
	return error;
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
