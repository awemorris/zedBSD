/* zedBSD credential and VFS permission tests. SPDX-License-Identifier: Zlib */
#include "kern/cred.h"
#include "kern/inode.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct thread;

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
	return 0;
}
