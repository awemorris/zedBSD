/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * POSIX process credentials.
 *
 * A credential is a reference-counted record of user and group identity.
 * The permission checks built on it cover file access through the mode
 * bits or a POSIX ACL, directory entry changes, ownership changes, set-id
 * clearing, and the extended attribute namespaces.
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

extern unsigned long spin_lock_irqsave(struct spinlock *) __attribute__((weak));
extern void spin_unlock_irqrestore(struct spinlock *, unsigned long) __attribute__((weak));

static int xattr_namespace_access(const struct inode *inode, const struct ucred *cred, const char *name, int write_access);

/*
 * Allocates the root credential with every identity zero.
 */
struct ucred *
cred_alloc_root(
	void)
{
	struct ucred *cred;

	/* A zeroed record is the root credential. */
	cred = kern_calloc(1, sizeof(*cred));
	if (cred != NULL)
		refcount_init(&cred->refs, 1);

	/* Reports the credential, or none. */
	return cred;
}

/*
 * Copies a credential into a new record with one reference.
 */
struct ucred *
cred_copy(
	const struct ucred *source)
{
	struct ucred *cred;

	/* There is nothing to copy without a source. */
	if (source == NULL)
		return NULL;

	/* Copies every field, then resets the reference count. */
	cred = kern_calloc(1, sizeof(*cred));
	if (cred != NULL) {
		memcpy(cred, source, sizeof(*cred));
		refcount_init(&cred->refs, 1);
	}

	/* Reports the copy, or none. */
	return cred;
}

/*
 * Takes a reference on a credential.
 */
void
cred_ref(
	struct ucred *cred)
{
	/* Ignores a missing credential. */
	if (cred != NULL)
		refcount_get(&cred->refs);
}

/*
 * Drops a reference on a credential and frees the last one.
 */
void
cred_release(
	struct ucred *cred)
{
	/* Frees the record with the last reference. */
	if (cred != NULL && refcount_put(&cred->refs))
		kern_free(cred);
}

/*
 * Tests whether a credential has the effective user identity of root.
 */
int
cred_is_superuser(
	const struct ucred *cred)
{
	/* A missing credential has no privilege. */
	if (cred == NULL)
		return 0;

	/* Reports whether the effective user is root. */
	if (cred->euid == 0)
		return 1;
	return 0;
}

/*
 * Tests whether a credential belongs to a group.
 *
 * Both the effective group and the supplementary groups count.
 */
int
cred_in_group(
	const struct ucred *cred,
	gid_t group)
{
	unsigned i;

	/* A missing credential belongs to no group. */
	if (cred == NULL)
		return 0;

	/* Checks the effective group, then the supplementary groups. */
	if (cred->egid == group)
		return 1;
	for (i = 0; i < cred->ngroups; i++) {
		if (cred->groups[i] == group)
			return 1;
	}

	/* Reports no membership. */
	return 0;
}

/*
 * Reads the credential of the current process without a reference.
 */
const struct ucred *
cred_current(
	void)
{
	struct thread *thread;

	/* There is no credential without a current thread and process. */
	thread = thread_current();
	if (thread == NULL)
		return NULL;
	if (thread->proc == NULL)
		return NULL;

	/* Reports the process credential. */
	return thread->proc->cred;
}

/*
 * Takes a reference on the credential of the current process.
 */
struct ucred *
cred_current_ref(
	void)
{
	struct thread *thread;
	struct ucred *cred;

	/* There is no credential without a current thread. */
	thread = thread_current();
	if (thread == NULL)
		return NULL;

	/* References the process credential. */
	cred = cred_process_ref(thread->proc);

	/* Reports the referenced credential, or none. */
	return cred;
}

/*
 * Takes a reference on the credential of a process.
 *
 * The process lock is taken through weak references so that host tests
 * without a lock implementation still work.
 */
struct ucred *
cred_process_ref(
	struct process *process)
{
	struct ucred *cred;
	unsigned long irq;

	/* There is no credential without a process. */
	if (process == NULL)
		return NULL;

	/* Samples and references the credential under the process lock. */
	if (spin_lock_irqsave != NULL)
		irq = spin_lock_irqsave(&process->lock);
	else
		irq = 0;
	cred = process->cred;
	if (cred != NULL)
		cred_ref(cred);
	if (spin_unlock_irqrestore != NULL)
		spin_unlock_irqrestore(&process->lock, irq);

	/* Reports the referenced credential, or none. */
	return cred;
}

/*
 * Checks access to an inode for a credential.
 *
 * The superuser passes everything except executing a regular file with no
 * execute bit.  An access ACL decides when the inode has one; otherwise
 * the owner, group, or other mode bits apply.
 */
int
vfs_access(
	const struct inode *inode,
	const struct ucred *cred,
	int requested)
{
	struct posix_acl acl;
	mode_t bits;
	unsigned shift;
	int error;

	/* Rejects a missing operand or an unknown request bit. */
	if (inode == NULL ||
	    cred == NULL ||
	    (requested & ~(R_OK|W_OK|X_OK)) != 0)
		return EINVAL;

	/* Existence needs no permission. */
	if (requested == F_OK)
		return 0;

	/* The superuser is refused only execution of a non-executable file. */
	if (cred_is_superuser(cred)) {
		if ((requested & X_OK) != 0 &&
		    inode->i_type == INODE_REG &&
		    (inode->i_mode & 0111U) == 0)
			return EACCES;
		return 0;
	}

	/* An access ACL decides when the inode has one. */
	error = posix_acl_load((struct inode *)(uintptr_t)inode,
	    POSIX_ACL_XATTR_ACCESS, &acl);
	if (error == 0) {
		error = posix_acl_check_access(&acl, inode, cred, requested);
		return error;
	}

	if (error != ENODATA && error != EOPNOTSUPP)
		return error;

	/* Selects the owner, group, or other bits of the mode. */
	if (cred->euid == inode->i_uid)
		shift = 6U;
	else if (cred_in_group(cred, inode->i_gid))
		shift = 3U;
	else
		shift = 0U;

	/* Every requested access must be granted by those bits. */
	bits = (inode->i_mode >> shift) & 7U;
	if (((requested & R_OK) != 0 && (bits & 4U) == 0) ||
	    ((requested & W_OK) != 0 && (bits & 2U) == 0) ||
	    ((requested & X_OK) != 0 && (bits & 1U) == 0))
		return EACCES;

	/* Reports permitted access. */
	return 0;
}

/*
 * Checks that a credential may create an entry in a directory.
 */
int
vfs_may_create(
	const struct inode *parent,
	const struct ucred *cred)
{
	int error;

	/* Rejects a missing operand or a parent that is not a directory. */
	if (parent == NULL || cred == NULL)
		return EINVAL;
	if (parent->i_type != INODE_DIR)
		return ENOTDIR;

	/* Creation needs write and search permission on the directory. */
	error = vfs_access(parent, cred, W_OK | X_OK);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Checks that a credential may remove an entry from a directory.
 *
 * In a sticky directory only the superuser, the directory owner, and the
 * entry owner may remove.
 */
int
vfs_may_remove(
	const struct inode *parent,
	const struct inode *victim,
	const struct ucred *cred)
{
	int error;

	/* Rejects a missing operand. */
	if (parent == NULL || victim == NULL || cred == NULL)
		return EINVAL;

	/* Removal needs the same directory permission as creation. */
	error = vfs_may_create(parent, cred);
	if (error != 0)
		return error;

	/* Applies the sticky bit restriction. */
	if ((parent->i_mode & S_ISVTX) != 0 &&
	    !cred_is_superuser(cred) &&
	    cred->euid != parent->i_uid &&
	    cred->euid != victim->i_uid)
		return EPERM;

	/* Reports permitted removal. */
	return 0;
}

/*
 * Checks that a credential may rename an entry.
 *
 * The source must be removable from its directory, and the target either
 * removable from the new directory or creatable there.
 */
int
vfs_may_rename(
	const struct inode *old_parent,
	const struct inode *source,
	const struct inode *new_parent,
	const struct inode *target,
	const struct ucred *cred)
{
	int error;

	/* The source must be removable. */
	error = vfs_may_remove(old_parent, source, cred);
	if (error != 0)
		return error;

	/* The destination must be replaceable or creatable. */
	if (target != NULL)
		error = vfs_may_remove(new_parent, target, cred);
	else
		error = vfs_may_create(new_parent, cred);

	/* Reports the destination check. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Checks that a credential may change the ownership of an inode.
 *
 * Without privilege the owner may only keep the user and hand the group
 * to one of its own groups.
 */
int
vfs_may_chown(
	const struct inode *inode,
	const struct ucred *cred,
	uid_t uid,
	gid_t gid)
{
	/* Rejects a missing operand. */
	if (inode == NULL || cred == NULL)
		return EINVAL;

	/* The superuser may make any change. */
	if (cred_is_superuser(cred))
		return 0;

	/* The owner may keep the user and move the group within its groups. */
	if (cred->euid != inode->i_uid ||
	    (uid != (uid_t)-1 && uid != inode->i_uid) ||
	    (gid != (gid_t)-1 &&
	     gid != inode->i_gid &&
	     !cred_in_group(cred, gid)))
		return EPERM;

	/* Reports a permitted change. */
	return 0;
}

/*
 * Clears the set-id bits of a regular file whose content changed.
 */
int
vfs_clear_setid_on_content_change(
	struct inode *inode)
{
	struct stat status;
	int error;

	/* Rejects a missing inode. */
	if (inode == NULL)
		return EINVAL;

	/* Only a regular file with a set-id bit needs a change. */
	if (inode->i_type != INODE_REG ||
	    (inode->i_mode & (S_ISUID | S_ISGID)) == 0)
		return 0;

	/* Rewrites the mode without the set-id bits. */
	error = inode_getattr(inode, &status);
	if (error != 0)
		return error;
	status.st_mode &= ~(mode_t)(S_ISUID | S_ISGID);

	/* Reports the mode update. */
	error = inode_setattr(inode, &status, INODE_ATTR_MODE);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Clears the set-id bits after a write by an unprivileged credential.
 */
int
vfs_clear_setid_on_write(
	struct inode *inode,
	const struct ucred *cred)
{
	int error;

	/* Rejects a missing operand. */
	if (inode == NULL || cred == NULL)
		return EINVAL;

	/* A superuser write keeps the bits. */
	if (cred_is_superuser(cred))
		return 0;

	/* Clears the bits as for any content change. */
	error = vfs_clear_setid_on_content_change(inode);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads an extended attribute after checking the namespace permission.
 *
 * A negative result carries the error.
 */
ssize_t
vfs_getxattr(
	struct inode *inode,
	const struct ucred *cred,
	const char *name,
	void *value,
	size_t size)
{
	ssize_t result;
	int error;

	/* Checks read permission for the attribute's namespace. */
	error = xattr_namespace_access(inode, cred, name, 0);
	if (error != 0)
		return -error;

	/* Reads the attribute. */
	result = inode_getxattr(inode, name, value, size);

	/* Reports the attribute size or error. */
	return result;
}

/*
 * Writes an extended attribute after checking the namespace permission.
 */
int
vfs_setxattr(
	struct inode *inode,
	const struct ucred *cred,
	const char *name,
	const void *value,
	size_t size,
	unsigned flags)
{
	int error;

	/* Checks write permission for the attribute's namespace. */
	error = xattr_namespace_access(inode, cred, name, 1);
	if (error != 0)
		return error;

	/* Writes the attribute. */

	/* Reports why the write failed. */
	error = inode_setxattr(inode, name, value, size, flags);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Lists the extended attributes a credential may see.
 *
 * The superuser sees every name; anyone else sees only the user
 * namespace.  A negative result carries the error.
 */
ssize_t
vfs_listxattr(
	struct inode *inode,
	const struct ucred *cred,
	char *list,
	size_t size)
{
	char *all;
	ssize_t total;
	ssize_t loaded;
	size_t offset;
	size_t visible;
	size_t length;

	visible = 0;

	/* Rejects a missing operand. */
	if (inode == NULL || cred == NULL)
		return -EINVAL;

	/* The superuser gets the unfiltered list. */
	if (cred_is_superuser(cred)) {
		total = inode_listxattr(inode, list, size);
		return total;
	}

	/* Loads the whole list into a private buffer. */
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
		if (loaded < 0)
			return loaded;
		return -EIO;
	}

	/* Copies out the terminated names in the user namespace. */
	offset = 0;
	while (offset < (size_t)total) {
		length = 0;
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

	/* A buffer too small for the visible names is an error. */
	if (list != NULL && size < visible)
		return -ERANGE;

	/* Reports the visible list size. */
	return (ssize_t)visible;
}

/*
 * Removes an extended attribute after checking the namespace permission.
 */
int
vfs_removexattr(
	struct inode *inode,
	const struct ucred *cred,
	const char *name)
{
	int error;

	/* Checks write permission for the attribute's namespace. */
	error = xattr_namespace_access(inode, cred, name, 1);
	if (error != 0)
		return error;

	/* Removes the attribute. */
	error = inode_removexattr(inode, name);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Checks the permission an attribute namespace requires for an access. */
static int
xattr_namespace_access(
	const struct inode *inode,
	const struct ucred *cred,
	const char *name,
	int write_access)
{
	int requested;
	int error;

	/* Rejects a missing operand. */
	if (inode == NULL || cred == NULL || name == NULL)
		return EINVAL;

	/*
	 * Filesystem control metadata is never a user-managed xattr,
	 * including for uid 0.  The UFS control path calls its backend
	 * directly.
	 */
	if (strcmp(name, "system.zedbsd.quota") == 0)
		return EPERM;

	/* The user namespace follows the file's own permissions. */
	if (strncmp(name, "user.", 5) == 0 && name[5] != '\0') {
		if (write_access)
			requested = W_OK;
		else
			requested = R_OK;
		error = vfs_access(inode, cred, requested);
		return error;
	}

	/* The system and security namespaces are for the superuser. */
	if ((strncmp(name, "system.", 7) == 0 && name[7] != '\0') ||
	    (strncmp(name, "security.", 9) == 0 && name[9] != '\0')) {
		if (cred_is_superuser(cred))
			return 0;
		return EPERM;
	}

	/* Reports an unknown namespace. */
	return EOPNOTSUPP;
}
