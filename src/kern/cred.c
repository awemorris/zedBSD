/*
 * POSIX process credentials
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/cred.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/process.h"
#include "kern/posix-acl.h"
#include "kern/thread.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

extern unsigned long spin_lock_irqsave(struct spinlock *)
    __attribute__((weak));
extern void spin_unlock_irqrestore(struct spinlock *, unsigned long)
    __attribute__((weak));

struct ucred *
cred_alloc_root(void)
{
	struct ucred *cred = kern_calloc(1, sizeof(*cred));
	if (cred != NULL)
		refcount_init(&cred->refs, 1);
	return cred;
}

struct ucred *
cred_copy(const struct ucred *source)
{
	struct ucred *cred;
	if (source == NULL)
		return NULL;
	cred = kern_calloc(1, sizeof(*cred));
	if (cred != NULL) {
		memcpy(cred, source, sizeof(*cred));
		refcount_init(&cred->refs, 1);
	}
	return cred;
}

void cred_ref(struct ucred *cred)
{ if (cred != NULL) refcount_get(&cred->refs); }
void cred_release(struct ucred *cred)
{
	if (cred != NULL && refcount_put(&cred->refs))
		kern_free(cred);
}
int cred_is_superuser(const struct ucred *cred)
{ return cred != NULL && cred->euid == 0; }
int cred_in_group(const struct ucred *cred, gid_t group)
{
	unsigned i;
	if (cred == NULL)
		return 0;
	if (cred->egid == group)
		return 1;
	for (i = 0; i < cred->ngroups; i++)
		if (cred->groups[i] == group)
			return 1;
	return 0;
}

const struct ucred *
cred_current(void)
{
	struct thread *thread = thread_current();
	return thread != NULL && thread->proc != NULL ? thread->proc->cred : NULL;
}

struct ucred *
cred_current_ref(void)
{
	struct thread *thread = thread_current();
	struct process *process;
	struct ucred *cred;
	unsigned long irq;
	if (thread == NULL || (process = thread->proc) == NULL)
		return NULL;
	irq = spin_lock_irqsave != NULL ? spin_lock_irqsave(&process->lock) : 0;
	cred = process->cred;
	if (cred != NULL)
		cred_ref(cred);
	if (spin_unlock_irqrestore != NULL)
		spin_unlock_irqrestore(&process->lock, irq);
	return cred;
}

int
vfs_access(const struct inode *inode, const struct ucred *cred, int requested)
{
	struct posix_acl acl;
	mode_t bits;
	unsigned shift;
	int error;
	if (inode == NULL || cred == NULL || (requested & ~(R_OK|W_OK|X_OK)) != 0)
		return EINVAL;
	if (requested == F_OK)
		return 0;
	if (cred_is_superuser(cred)) {
		if ((requested & X_OK) != 0 && inode->i_type == INODE_REG &&
		    (inode->i_mode & 0111U) == 0)
			return EACCES;
		return 0;
	}
	error = posix_acl_load((struct inode *)(uintptr_t)inode,
	    POSIX_ACL_XATTR_ACCESS, &acl);
	if (error == 0)
		return posix_acl_check_access(&acl, inode, cred, requested);
	if (error != ENODATA && error != EOPNOTSUPP)
		return error;
	shift = cred->euid == inode->i_uid ? 6U :
	    (cred_in_group(cred, inode->i_gid) ? 3U : 0U);
	bits = (inode->i_mode >> shift) & 7U;
	if (((requested & R_OK) != 0 && (bits & 4U) == 0) ||
	    ((requested & W_OK) != 0 && (bits & 2U) == 0) ||
	    ((requested & X_OK) != 0 && (bits & 1U) == 0))
		return EACCES;
	return 0;
}

int
vfs_may_create(const struct inode *parent, const struct ucred *cred)
{
	if (parent == NULL || cred == NULL)
		return EINVAL;
	if (parent->i_type != INODE_DIR)
		return ENOTDIR;
	return vfs_access(parent, cred, W_OK | X_OK);
}

int
vfs_may_remove(const struct inode *parent, const struct inode *victim,
	       const struct ucred *cred)
{
	int error;

	if (parent == NULL || victim == NULL || cred == NULL)
		return EINVAL;
	error = vfs_may_create(parent, cred);
	if (error != 0)
		return error;
	if ((parent->i_mode & S_ISVTX) != 0 && !cred_is_superuser(cred) &&
	    cred->euid != parent->i_uid && cred->euid != victim->i_uid)
		return EPERM;
	return 0;
}

int
vfs_may_rename(const struct inode *old_parent, const struct inode *source,
	       const struct inode *new_parent, const struct inode *target,
	       const struct ucred *cred)
{
	int error;

	error = vfs_may_remove(old_parent, source, cred);
	if (error != 0)
		return error;
	return target != NULL ? vfs_may_remove(new_parent, target, cred) :
		vfs_may_create(new_parent, cred);
}

int
vfs_may_chown(const struct inode *inode, const struct ucred *cred,
	      uid_t uid, gid_t gid)
{
	if (inode == NULL || cred == NULL)
		return EINVAL;
	if (cred_is_superuser(cred))
		return 0;
	if (cred->euid != inode->i_uid ||
	    (uid != (uid_t)-1 && uid != inode->i_uid) ||
	    (gid != (gid_t)-1 && gid != inode->i_gid &&
	     !cred_in_group(cred, gid)))
		return EPERM;
	return 0;
}

int
vfs_clear_setid_on_write(struct inode *inode, const struct ucred *cred)
{
	struct stat status;
	int error;

	if (inode == NULL || cred == NULL)
		return EINVAL;
	if (cred_is_superuser(cred) || inode->i_type != INODE_REG ||
	    (inode->i_mode & (S_ISUID | S_ISGID)) == 0)
		return 0;
	error = inode_getattr(inode, &status);
	if (error != 0)
		return error;
	status.st_mode &= ~(mode_t)(S_ISUID | S_ISGID);
	return inode_setattr(inode, &status, INODE_ATTR_MODE);
}

static int
xattr_namespace_access(const struct inode *inode, const struct ucred *cred,
	const char *name, int write_access)
{
	if (inode == NULL || cred == NULL || name == NULL)
		return EINVAL;
	/* Filesystem control metadata is never a user-managed xattr, including
	 * for uid 0.  The UFS2 control path calls its backend directly. */
	if (strcmp(name, "system.zedbsd.quota") == 0)
		return EPERM;
	if (strncmp(name, "user.", 5) == 0 && name[5] != '\0')
		return vfs_access(inode, cred, write_access ? W_OK : R_OK);
	if ((strncmp(name, "system.", 7) == 0 && name[7] != '\0') ||
	    (strncmp(name, "security.", 9) == 0 && name[9] != '\0'))
		return cred_is_superuser(cred) ? 0 : EPERM;
	return EOPNOTSUPP;
}

ssize_t
vfs_getxattr(struct inode *inode, const struct ucred *cred, const char *name,
	void *value, size_t size)
{
	int error = xattr_namespace_access(inode, cred, name, 0);
	return error == 0 ? inode_getxattr(inode, name, value, size) : -error;
}

int
vfs_setxattr(struct inode *inode, const struct ucred *cred, const char *name,
	const void *value, size_t size, unsigned flags)
{
	int error = xattr_namespace_access(inode, cred, name, 1);
	return error == 0 ? inode_setxattr(inode, name, value, size, flags) : error;
}

ssize_t
vfs_listxattr(struct inode *inode, const struct ucred *cred, char *list,
	size_t size)
{
	char *all;
	ssize_t total, loaded;
	size_t offset, visible = 0;
	if (inode == NULL || cred == NULL)
		return -EINVAL;
	if (cred_is_superuser(cred))
		return inode_listxattr(inode, list, size);
	total = inode_listxattr(inode, NULL, 0);
	if (total <= 0)
		return total;
	if ((size_t)total > INODE_XATTR_SIZE_MAX)
		return -EIO;
	all = kern_malloc((size_t)total);
	if (all == NULL)
		return -ENOMEM;
	loaded = inode_listxattr(inode, all, (size_t)total);
	if (loaded != total) {
		kern_free(all);
		return loaded < 0 ? loaded : -EIO;
	}
	for (offset = 0; offset < (size_t)total;) {
		size_t length = 0;
		while (offset + length < (size_t)total && all[offset + length] != '\0')
			length++;
		if (offset + length >= (size_t)total) {
			kern_free(all);
			return -EIO;
		}
		length++;
		if (length > 6 && memcmp(all + offset, "user.", 5) == 0) {
			if (list != NULL && visible + length <= size)
				memcpy(list + visible, all + offset, length);
			visible += length;
		}
		offset += length;
	}
	kern_free(all);
	if (list != NULL && size < visible)
		return -ERANGE;
	return (ssize_t)visible;
}

int
vfs_removexattr(struct inode *inode, const struct ucred *cred,
	const char *name)
{
	int error = xattr_namespace_access(inode, cred, name, 1);
	return error == 0 ? inode_removexattr(inode, name) : error;
}
