/* zedBSD credential and VFS permission tests. SPDX-License-Identifier: Zlib */
#include "kern/cred.h"
#include "kern/inode.h"
#include "kern/posix-acl.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct thread;

static struct posix_acl stored_acl;
static int stored_acl_present;

void *kern_malloc(size_t size) { return malloc(size); }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void kern_free(void *pointer) { free(pointer); }
struct thread *thread_current(void) { return NULL; }

int
inode_getattr(struct inode *inode, struct stat *status)
{
	memset(status, 0, sizeof(*status));
	status->st_mode = inode->i_mode;
	status->st_uid = inode->i_uid;
	status->st_gid = inode->i_gid;
	return 0;
}

int
inode_setattr(struct inode *inode, const struct stat *status, unsigned mask)
{
	if ((mask & INODE_ATTR_MODE) != 0)
		inode->i_mode = status->st_mode;
	return 0;
}

ssize_t inode_getxattr(struct inode *inode, const char *name, void *value,
	size_t size)
{
	static const char payload[] = "ok";
	size_t bytes;
	(void)inode;
	if (strcmp(name, POSIX_ACL_XATTR_ACCESS) == 0) {
		if (!stored_acl_present)
			return -ENODATA;
		bytes = sizeof(stored_acl.version) + sizeof(stored_acl.count) +
		    stored_acl.count * sizeof(stored_acl.entries[0]);
		if (value != NULL && size < bytes)
			return -ERANGE;
		if (value != NULL)
			memcpy(value, &stored_acl, bytes);
		return (ssize_t)bytes;
	}
	if (value != NULL && size < sizeof(payload)) return -ERANGE;
	if (value != NULL) memcpy(value, payload, sizeof(payload));
	return (ssize_t)sizeof(payload);
}
int inode_setxattr(struct inode *inode, const char *name, const void *value,
	size_t size, unsigned flags)
{
	(void)inode;(void)flags;
	if (strcmp(name, POSIX_ACL_XATTR_ACCESS) == 0) {
		if (value == NULL || size > sizeof(stored_acl))
			return EINVAL;
		memset(&stored_acl, 0, sizeof(stored_acl));
		memcpy(&stored_acl, value, size);
		stored_acl_present = 1;
	}
	return 0;
}
ssize_t inode_listxattr(struct inode *inode, char *list, size_t size)
{
	static const char names[] = "user.visible\0system.hidden\0";
	(void)inode;
	if (list != NULL && size < sizeof(names) - 1U) return -ERANGE;
	if (list != NULL) memcpy(list, names, sizeof(names) - 1U);
	return (ssize_t)(sizeof(names) - 1U);
}
int inode_removexattr(struct inode *inode, const char *name)
{
	(void)inode;
	if (strcmp(name, POSIX_ACL_XATTR_ACCESS) == 0) {
		if (!stored_acl_present)
			return ENODATA;
		stored_acl_present = 0;
	}
	return 0;
}

int
main(void)
{
	struct inode directory, file;
	struct ucred owner, group, other, root;

	memset(&directory, 0, sizeof(directory));
	memset(&file, 0, sizeof(file));
	memset(&owner, 0, sizeof(owner));
	memset(&group, 0, sizeof(group));
	memset(&other, 0, sizeof(other));
	memset(&root, 0, sizeof(root));
	directory.i_type = INODE_DIR;
	directory.i_mode = S_IFDIR | 0770U;
	directory.i_uid = 100;
	directory.i_gid = 200;
	file.i_type = INODE_REG;
	file.i_mode = S_IFREG | 0640U;
	file.i_uid = 100;
	file.i_gid = 200;
	owner.euid = 100;
	owner.egid = 300;
	group.euid = 101;
	group.egid = 300;
	group.groups[0] = 200;
	group.ngroups = 1;
	other.euid = 102;
	other.egid = 300;
	root.euid = 0;

	assert(vfs_access(&file, &owner, R_OK | W_OK) == 0);
	assert(vfs_access(&file, &group, R_OK) == 0);
	assert(vfs_access(&file, &group, W_OK) == EACCES);
	assert(vfs_access(&file, &other, R_OK) == EACCES);
	assert(vfs_access(&file, &root, R_OK | W_OK) == 0);
	assert(vfs_access(&file, &root, X_OK) == EACCES);
	assert(vfs_may_create(&directory, &group) == 0);

	directory.i_mode |= S_ISVTX;
	assert(vfs_may_remove(&directory, &file, &owner) == 0);
	assert(vfs_may_remove(&directory, &file, &other) == EACCES);
	directory.i_mode = S_IFDIR | S_ISVTX | 0777U;
	assert(vfs_may_remove(&directory, &file, &other) == EPERM);
	assert(vfs_may_remove(&directory, &file, &root) == 0);
	directory.i_uid = other.euid;
	assert(vfs_may_remove(&directory, &file, &other) == 0);

	file.i_mode = S_IFREG | S_ISUID | S_ISGID | 0755U;
	assert(vfs_clear_setid_on_write(&file, &owner) == 0);
	assert((file.i_mode & (S_ISUID | S_ISGID)) == 0);
	file.i_mode = S_IFREG | S_ISUID | 0755U;
	assert(vfs_clear_setid_on_write(&file, &root) == 0);
	assert((file.i_mode & S_ISUID) != 0);

	assert(vfs_may_chown(&file, &owner, file.i_uid, file.i_gid) == 0);
	assert(vfs_may_chown(&file, &other, file.i_uid, file.i_gid) == EPERM);
	assert(vfs_may_chown(&file, &root, 999, 999) == 0);

	file.i_mode = S_IFREG | 0640U;
	assert(vfs_getxattr(&file, &owner, "user.note", NULL, 0) == 3);
	assert(vfs_getxattr(&file, &other, "user.note", NULL, 0) == -EACCES);
	assert(vfs_setxattr(&file, &owner, "user.note", "x", 1, 0) == 0);
	assert(vfs_setxattr(&file, &group, "user.note", "x", 1, 0) == EACCES);
	assert(vfs_getxattr(&file, &owner, "system.acl", NULL, 0) == -EPERM);
	assert(vfs_getxattr(&file, &root, "system.acl", NULL, 0) == 3);
	assert(vfs_getxattr(&file, &root, "system.zedbsd.quota", NULL, 0) ==
	    -EPERM);
	assert(vfs_setxattr(&file, &root, "system.zedbsd.quota", "x", 1, 0) ==
	    EPERM);
	assert(vfs_removexattr(&file, &root, "system.zedbsd.quota") == EPERM);
	{
		char names[64];
		ssize_t needed = vfs_listxattr(&file, &owner, NULL, 0);
		assert(needed == (ssize_t)sizeof("user.visible"));
		memset(names, 0, sizeof(names));
		assert(vfs_listxattr(&file, &owner, names, sizeof(names)) == needed);
		assert(strcmp(names, "user.visible") == 0);
		assert(vfs_listxattr(&file, &root, NULL, 0) ==
		    (ssize_t)(sizeof("user.visible\0system.hidden\0") - 1U));
	}
	assert(vfs_removexattr(&file, &other, "security.label") == EPERM);
	assert(vfs_removexattr(&file, &root, "security.label") == 0);

	/* Named users and groups are restricted by the ACL mask. */
	memset(&stored_acl, 0, sizeof(stored_acl));
	stored_acl.version = POSIX_ACL_VERSION;
	stored_acl.count = 6;
	stored_acl.entries[0] = (struct posix_acl_entry){
	    POSIX_ACL_USER_OBJ, 6, POSIX_ACL_UNDEFINED_ID };
	stored_acl.entries[1] = (struct posix_acl_entry){
	    POSIX_ACL_USER, 4, 102 };
	stored_acl.entries[2] = (struct posix_acl_entry){
	    POSIX_ACL_GROUP_OBJ, 0, POSIX_ACL_UNDEFINED_ID };
	stored_acl.entries[3] = (struct posix_acl_entry){
	    POSIX_ACL_GROUP, 6, 200 };
	stored_acl.entries[4] = (struct posix_acl_entry){
	    POSIX_ACL_MASK, 4, POSIX_ACL_UNDEFINED_ID };
	stored_acl.entries[5] = (struct posix_acl_entry){
	    POSIX_ACL_OTHER, 0, POSIX_ACL_UNDEFINED_ID };
	assert(posix_acl_validate(&stored_acl) == 0);
	stored_acl_present = 1;
	file.i_mode = S_IFREG | 0640U;
	assert(vfs_access(&file, &owner, R_OK | W_OK) == 0);
	assert(vfs_access(&file, &other, R_OK) == 0);
	assert(vfs_access(&file, &other, W_OK) == EACCES);
	assert(vfs_access(&file, &group, R_OK) == 0);
	assert(vfs_access(&file, &group, W_OK) == EACCES);
	assert(posix_acl_chmod(&file, 0620U) == 0);
	assert(stored_acl.entries[0].permissions == 6);
	assert(stored_acl.entries[4].permissions == 2);
	stored_acl.entries[1].id = POSIX_ACL_UNDEFINED_ID;
	assert(posix_acl_validate(&stored_acl) == EINVAL);
	return 0;
}
