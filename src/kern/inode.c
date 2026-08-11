/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/inode.h"
#include "kern/mount.h"
#include "kern/namecache.h"
#include "kern/namei.h"

#include <errno.h>
#include <string.h>

#define INODE_POOL_MAX 256U
#define VFS_BSS __attribute__((section(".vfs_bss")))

static struct inode common_pool[INODE_POOL_MAX] VFS_BSS;
static uint8_t common_used[INODE_POOL_MAX] VFS_BSS;
static struct inode *inode_cache[INODE_POOL_MAX] VFS_BSS;

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
	for (i = 0; i < INODE_POOL_MAX; i++)
		if (&common_pool[i] == inode)
			return (int)i;
	return -1;
}

static int
cache_index(const struct inode *inode)
{
	unsigned i;
	for (i = 0; i < INODE_POOL_MAX; i++)
		if (inode_cache[i] == inode)
			return (int)i;
	return -1;
}

static int
cache_slot(void)
{
	unsigned i;
	for (i = 0; i < INODE_POOL_MAX; i++)
		if (inode_cache[i] == NULL)
			return (int)i;
	for (i = 0; i < INODE_POOL_MAX; i++) {
		struct inode *inode = inode_cache[i];
		if (inode->i_usecount == 1 &&
		    !(inode->i_flags & (INODE_DIRTY | INODE_ROOT))) {
			inode_free(inode);
			return (int)i;
		}
	}
	return -1;
}

struct inode *
inode_alloc(struct mount *mountp)
{
	struct inode *inode = NULL;
	int slot;
	unsigned i;

	slot = cache_slot();
	if (slot < 0)
		return NULL;
	if (mountp != NULL && mountp->m_type != NULL &&
	    mountp->m_type->alloc_inode != NULL)
		inode = mountp->m_type->alloc_inode(mountp);
	else {
		for (i = 0; i < INODE_POOL_MAX; i++) {
			if (!common_used[i]) {
				common_used[i] = 1;
				inode = &common_pool[i];
				break;
			}
		}
	}
	if (inode == NULL)
		return NULL;
	memset(inode, 0, sizeof(*inode));
	inode->i_mount = mountp;
	/* One cache reference and one reference returned to the caller. */
	inode->i_usecount = 2;
	inode_cache[slot] = inode;
	return inode;
}

void
inode_free(struct inode *inode)
{
	int cindex, pindex;
	struct mount *mountp;

	if (inode == NULL || inode->i_usecount != 1 ||
	    (inode->i_flags & INODE_DIRTY))
		return;
	cindex = cache_index(inode);
	if (cindex < 0)
		return;
	inode_cache[cindex] = NULL;
	inode->i_usecount = 0;
	if (inode->i_op != NULL && inode->i_op->reclaim != NULL)
		inode->i_op->reclaim(inode);
	mountp = inode->i_mount;
	pindex = common_index(inode);
	if (pindex >= 0) {
		memset(inode, 0, sizeof(*inode));
		common_used[pindex] = 0;
	} else if (mountp != NULL && mountp->m_type != NULL &&
		   mountp->m_type->free_inode != NULL) {
		mountp->m_type->free_inode(inode);
	}
}

int
inode_get(struct mount *mountp, ino_t ino, struct inode **result)
{
	unsigned i;
	if (mountp == NULL || result == NULL)
		return EINVAL;
	for (i = 0; i < INODE_POOL_MAX; i++) {
		struct inode *inode = inode_cache[i];
		if (inode != NULL && inode->i_mount == mountp &&
		    inode->i_ino == ino && !(inode->i_flags & INODE_DEAD)) {
			inode_ref(inode);
			*result = inode;
			return 0;
		}
	}
	return ENOENT;
}

void inode_ref(struct inode *inode)
{
	if (inode != NULL && inode->i_usecount != 0)
		inode->i_usecount++;
}

void inode_release(struct inode *inode)
{
	if (inode != NULL && inode->i_usecount > 1)
		inode->i_usecount--;
}

void
inode_cache_purge_mount(struct mount *mountp)
{
	unsigned i;
	for (i = 0; i < INODE_POOL_MAX; i++)
		if (inode_cache[i] != NULL && inode_cache[i]->i_mount == mountp &&
		    inode_cache[i]->i_usecount == 1)
			inode_free(inode_cache[i]);
}

void
inode_cache_reset(void)
{
	unsigned i;
	namecache_reset();
	for (i = 0; i < INODE_POOL_MAX; i++)
		if (inode_cache[i] != NULL && inode_cache[i]->i_usecount == 1)
			inode_free(inode_cache[i]);
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
	if (namecache_lookup(directory, name, result) == 0)
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
	(void)namecache_enter(directory, name, child);
	*result = child;
	return 0;
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
	return 0;
}

int inode_setattr(struct inode *i, const struct stat *s, unsigned mask)
{
	if (i == NULL || s == NULL) return EINVAL;
	if (readonly(i)) return EROFS;
	return i->i_op != NULL && i->i_op->setattr != NULL ?
		i->i_op->setattr(i, s, mask) : EOPNOTSUPP;
}
int inode_create(struct inode *i, const struct componentname *n, mode_t m,
		 struct inode **r)
{
	if (i == NULL || n == NULL || r == NULL) return EINVAL;
	if (readonly(i)) return EROFS;
	return i->i_op != NULL && i->i_op->create != NULL ?
		i->i_op->create(i, n, m, r) : EOPNOTSUPP;
}
int inode_mkdir(struct inode *i, const struct componentname *n, mode_t m,
		struct inode **r)
{
	if (i == NULL || n == NULL || r == NULL) return EINVAL;
	if (readonly(i)) return EROFS;
	return i->i_op != NULL && i->i_op->mkdir != NULL ?
		i->i_op->mkdir(i, n, m, r) : EOPNOTSUPP;
}
int inode_unlink(struct inode *i, const struct componentname *n)
{
	if (i == NULL || n == NULL) return EINVAL;
	if (readonly(i)) return EROFS;
	return i->i_op != NULL && i->i_op->unlink != NULL ?
		i->i_op->unlink(i, n) : EOPNOTSUPP;
}
int inode_rmdir(struct inode *i, const struct componentname *n)
{
	if (i == NULL || n == NULL) return EINVAL;
	if (readonly(i)) return EROFS;
	return i->i_op != NULL && i->i_op->rmdir != NULL ?
		i->i_op->rmdir(i, n) : EOPNOTSUPP;
}
int inode_truncate(struct inode *i, off_t size)
{
	if (i == NULL || size < 0) return EINVAL;
	if (readonly(i)) return EROFS;
	return i->i_op != NULL && i->i_op->truncate != NULL ?
		i->i_op->truncate(i, size) : EOPNOTSUPP;
}
int inode_sync(struct inode *i)
{
	if (i == NULL) return EINVAL;
	return i->i_op != NULL && i->i_op->sync != NULL ? i->i_op->sync(i) : 0;
}
