/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/mount.h"
#include "kern/inode.h"
#include "kern/namecache.h"
#include "kern/namei.h"
#include "kern/rootfs.h"

#include <errno.h>
#include <string.h>

#define FILESYSTEM_MAX 8U

static struct mount mounts[MOUNT_MAX] __attribute__((section(".vfs_bss")));
static uint8_t mount_used[MOUNT_MAX]
	__attribute__((section(".vfs_bss")));
static struct mount *mount_head;
static struct mount *root_mount;
static const struct filesystem_type *filesystems[FILESYSTEM_MAX]
	__attribute__((section(".vfs_bss")));
static unsigned filesystem_count;

static struct mount *
mount_alloc(void)
{
	unsigned i;
	for (i = 0; i < MOUNT_MAX; i++) {
		if (!mount_used[i]) {
			mount_used[i] = 1;
			memset(&mounts[i], 0, sizeof(mounts[i]));
			return &mounts[i];
		}
	}
	return NULL;
}

static void
mount_free(struct mount *mountp)
{
	unsigned i;
	for (i = 0; i < MOUNT_MAX; i++)
		if (&mounts[i] == mountp) {
			memset(mountp, 0, sizeof(*mountp));
			mount_used[i] = 0;
			return;
		}
}

int
filesystem_register(const struct filesystem_type *type)
{
	unsigned i;
	if (type == NULL || type->fs_name == NULL || type->mount == NULL)
		return EINVAL;
	for (i = 0; i < filesystem_count; i++)
		if (!strcmp(filesystems[i]->fs_name, type->fs_name))
			return EEXIST;
	if (filesystem_count >= FILESYSTEM_MAX)
		return ENOSPC;
	filesystems[filesystem_count++] = type;
	return 0;
}

void
mount_reset(void)
{
	namecache_reset();
	inode_cache_reset();
	rootfs_reset();
	memset(mounts, 0, sizeof(mounts));
	memset(mount_used, 0, sizeof(mount_used));
	memset(filesystems, 0, sizeof(filesystems));
	mount_head = NULL;
	root_mount = NULL;
	filesystem_count = 0;
}

int
mount_rootfs(void)
{
	struct mount *mountp;
	int error;
	if (root_mount != NULL)
		return EBUSY;
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	strcpy(mountp->m_path, "/");
	mountp->m_flags = MOUNT_READ_ONLY;
	mountp->m_type = &rootfs_type;
	error = rootfs_type.mount(mountp);
	if (error != 0) {
		mount_free(mountp);
		return error;
	}
	mount_head = mountp;
	root_mount = mountp;
	return 0;
}

struct inode *mount_root_inode(void)
{
	return root_mount != NULL ? root_mount->m_root : NULL;
}

static const struct filesystem_type *
find_type(const char *name, struct disk *disk, int *probe_error)
{
	unsigned i;
	*probe_error = EOPNOTSUPP;
	for (i = 0; i < filesystem_count; i++) {
		const struct filesystem_type *type = filesystems[i];
		int error;
		if (strcmp(name, "auto") && strcmp(name, type->fs_name))
			continue;
		if (type->probe == NULL)
			return type;
		error = type->probe(disk);
		if (error == 0)
			return type;
		if (error != EOPNOTSUPP)
			*probe_error = error;
		if (strcmp(name, "auto"))
			break;
	}
	return NULL;
}

int
mount(const char *type_name, const char *dir, int flags, void *data)
{
	const struct fat_mount_args *args = data;
	const struct filesystem_type *type;
	struct mount *mountp;
	struct inode *mountpoint;
	struct disk *disk;
	const char *basename;
	int error;

	if (root_mount == NULL || type_name == NULL || dir == NULL ||
	    args == NULL || args->fspec == NULL || dir[0] != '/' ||
	    dir[1] == '\0' || strchr(dir + 1, '/') != NULL)
		return EINVAL;
	if (mount_find(dir) != NULL)
		return EBUSY;
	disk = disk_find(args->fspec);
	if (disk == NULL)
		return ENXIO;
	type = find_type(type_name, disk, &error);
	if (type == NULL)
		return error;
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	basename = dir + 1;
	error = rootfs_add_mountpoint(root_mount, basename, &mountpoint);
	if (error != 0) {
		mount_free(mountp);
		return error;
	}
	error = disk_open(disk);
	if (error != 0)
		goto fail_mountpoint;
	strcpy(mountp->m_path, dir);
	mountp->m_flags = (unsigned)flags;
	mountp->m_disk = disk;
	mountp->m_type = type;
	mountp->m_mountpoint = mountpoint;
	mountp->m_parent = root_mount;
	error = type->mount(mountp);
	if (error != 0 || mountp->m_root == NULL) {
		if (error == 0)
			error = EIO;
		disk_close(disk);
		goto fail_mountpoint;
	}
	mountp->m_next = mount_head->m_next;
	mount_head->m_next = mountp;
	return 0;

fail_mountpoint:
	(void)rootfs_remove_mountpoint(mountpoint);
	inode_release(mountpoint);
	inode_free(mountpoint);
	mount_free(mountp);
	return error;
}

int
unmount(const char *dir, int flags)
{
	struct mount *mountp = mount_find(dir);
	struct mount **link;
	int error;
	(void)flags;
	if (mountp == NULL || mountp == root_mount)
		return mountp == NULL ? ENOENT : EBUSY;
	namecache_purge_mount(mountp);
	/* cache + mount owner is the only safe root reference here. */
	if (mountp->m_root->i_usecount != 2)
		return EBUSY;
	if (mountp->m_type->unmount != NULL) {
		error = mountp->m_type->unmount(mountp);
		if (error != 0)
			return error;
	}
	for (link = &mount_head; *link != NULL; link = &(*link)->m_next)
		if (*link == mountp) {
			*link = mountp->m_next;
			break;
		}
	(void)rootfs_remove_mountpoint(mountp->m_mountpoint);
	inode_release(mountp->m_mountpoint);
	inode_free(mountp->m_mountpoint);
	inode_release(mountp->m_root);
	inode_cache_purge_mount(mountp);
	disk_close(mountp->m_disk);
	mount_free(mountp);
	return 0;
}

struct mount *
mount_find(const char *path)
{
	struct mount *mountp;
	if (path == NULL)
		return NULL;
	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next)
		if (!strcmp(mountp->m_path, path))
			return mountp;
	return NULL;
}

struct mount *mount_for_inode(const struct inode *inode)
{
	return inode != NULL ? inode->i_mount : NULL;
}

int
mount_follow(struct inode *inode, struct inode **result)
{
	struct mount *mountp;
	if (inode == NULL || result == NULL)
		return EINVAL;
	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next) {
		if (mountp->m_mountpoint == inode) {
			inode_ref(mountp->m_root);
			*result = mountp->m_root;
			return 0;
		}
	}
	return ENOENT;
}

int
mount_cross_parent(struct inode *inode, struct inode **result)
{
	struct mount *mountp;
	struct componentname dotdot = { "..", 2, COMPONENT_DOTDOT };
	if (inode == NULL || result == NULL)
		return EINVAL;
	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next)
		if (mountp->m_root == inode && mountp->m_mountpoint != NULL)
			return inode_lookup(mountp->m_mountpoint, &dotdot, result);
	return ENOENT;
}
