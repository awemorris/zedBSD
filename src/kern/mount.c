/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/mount.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/namecache.h"
#include "kern/namei.h"
#include "kern/rootfs.h"

#include <errno.h>
#include <string.h>

#define FILESYSTEM_MAX 8U
#define MOUNT_BIND_INTERNAL 0x00000001U

static struct mount mounts[MOUNT_MAX] __attribute__((section(".vfs_bss")));
static uint8_t mount_used[MOUNT_MAX] __attribute__((section(".vfs_bss")));
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
			mounts[i].m_usecount = 1;
			return &mounts[i];
		}
	}
	return NULL;
}

static void
mount_free(struct mount *mountp)
{
	unsigned i;
	for (i = 0; i < MOUNT_MAX; i++) {
		if (&mounts[i] != mountp)
			continue;
		memset(mountp, 0, sizeof(*mountp));
		mount_used[i] = 0;
		return;
	}
}

void path_init(struct path *path)
{
	if (path != NULL)
		memset(path, 0, sizeof(*path));
}

void path_set(struct path *path, struct mount *mountp, struct inode *inode)
{
	if (path == NULL)
		return;
	path->p_mount = mountp;
	path->p_inode = inode;
	if (mountp != NULL)
		mountp->m_usecount++;
	if (inode != NULL)
		inode_ref(inode);
}

void path_ref(struct path *path)
{
	if (path != NULL)
		path_set(path, path->p_mount, path->p_inode);
}

void path_release(struct path *path)
{
	if (path == NULL)
		return;
	if (path->p_inode != NULL)
		inode_release(path->p_inode);
	if (path->p_mount != NULL && path->p_mount->m_usecount != 0)
		path->p_mount->m_usecount--;
	path_init(path);
}

int path_equal(const struct path *left, const struct path *right)
{
	return left != NULL && right != NULL &&
		left->p_mount == right->p_mount && left->p_inode == right->p_inode;
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
		if ((type->fs_flags & FILESYSTEM_NODEV) != 0) {
			if (!strcmp(name, type->fs_name))
				return type;
			continue;
		}
		if (disk == NULL)
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

static int
mount_filesystem(struct mount *mountp, const char *type_name, int flags,
		 void *data)
{
	const struct fat_mount_args *args = data;
	const struct filesystem_type *type;
	struct disk *disk;
	int error;

	disk = args != NULL && args->fspec != NULL ? disk_find(args->fspec) : NULL;
	type = find_type(type_name, disk, &error);
	if (type == NULL)
		return disk == NULL && !strcmp(type_name, "auto") ? ENXIO : error;
	if (!(type->fs_flags & FILESYSTEM_NODEV) && disk == NULL)
		return ENXIO;
	if (disk != NULL) {
		error = disk_open(disk);
		if (error != 0)
			return error;
	}
	mountp->m_flags = (unsigned)flags;
	mountp->m_disk = disk;
	mountp->m_type = type;
	mountp->m_data = data;
	error = type->mount(mountp);
	if (error != 0 || mountp->m_root == NULL) {
		if (disk != NULL)
			disk_close(disk);
		return error != 0 ? error : EIO;
	}
	return 0;
}

int
mount_root_create(const char *type_name, int flags, void *data,
		  struct mount **result)
{
	struct mount *mountp;
	int error;
	if (root_mount != NULL || type_name == NULL)
		return root_mount != NULL ? EBUSY : EINVAL;
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	strcpy(mountp->m_path, "/");
	error = mount_filesystem(mountp, type_name, flags, data);
	if (error != 0) {
		mount_free(mountp);
		return error;
	}
	mount_head = root_mount = mountp;
	if (result != NULL)
		*result = mountp;
	return 0;
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
	mount_head = root_mount = mountp;
	return 0;
}

struct mount *mount_root_get(void) { return root_mount; }
struct inode *mount_root_inode(void)
{
	return root_mount != NULL ? root_mount->m_root : NULL;
}

static int
valid_component(const char *name)
{
	size_t length;
	if (name == NULL || name[0] == '\0' || !strcmp(name, ".") ||
	    !strcmp(name, "..") || strchr(name, '/') != NULL)
		return 0;
	length = strlen(name);
	return length <= NAME_MAX;
}

static void
link_child(struct mount *parent, struct mount *child)
{
	struct mount **link = &parent->m_children;
	while (*link != NULL)
		link = &(*link)->m_sibling;
	*link = child;
}

static void
link_global(struct mount *mountp)
{
	struct mount **link = &mount_head;
	while (*link != NULL)
		link = &(*link)->m_next;
	*link = mountp;
}

static int
set_mount_path(struct mount *mountp, const struct path *directory,
	       const char *name)
{
	const char *base = directory->p_mount->m_path;
	size_t base_length = strlen(base), name_length = strlen(name);
	if (base_length + (base_length > 1U ? 1U : 0U) + name_length >=
	    sizeof(mountp->m_path))
		return ENAMETOOLONG;
	strcpy(mountp->m_path, base);
	if (base_length > 1U)
		strcat(mountp->m_path, "/");
	strcat(mountp->m_path, name);
	strcpy(mountp->m_name, name);
	return 0;
}

int
mount_at(const char *type_name, const struct path *directory,
	 const char *name, int flags, void *data, struct mount **result)
{
	struct mount *mountp;
	int error;
	if (type_name == NULL || directory == NULL || directory->p_mount == NULL ||
	    directory->p_inode == NULL || directory->p_inode->i_type != INODE_DIR ||
	    !valid_component(name))
		return EINVAL;
	{
		struct componentname component = { name, strlen(name), 0 };
		struct path existing;
		if (mount_lookup_child(directory, &component, &existing) == 0) {
			path_release(&existing);
			return EBUSY;
		}
	}
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	error = set_mount_path(mountp, directory, name);
	if (error != 0)
		goto fail;
	path_set(&mountp->m_cover, directory->p_mount, directory->p_inode);
	mountp->m_parent = directory->p_mount;
	error = mount_filesystem(mountp, type_name, flags, data);
	if (error != 0)
		goto fail_cover;
	link_child(directory->p_mount, mountp);
	link_global(mountp);
	if (result != NULL)
		*result = mountp;
	return 0;
fail_cover:
	path_release(&mountp->m_cover);
fail:
	mount_free(mountp);
	return error;
}

int
mount_bind_at(const struct path *source, const struct path *directory,
	      const char *name, struct mount **result)
{
	struct mount *mountp;
	int error;
	if (source == NULL || source->p_mount == NULL || source->p_inode == NULL ||
	    directory == NULL || directory->p_mount == NULL ||
	    directory->p_inode == NULL || directory->p_inode->i_type != INODE_DIR ||
	    !valid_component(name))
		return EINVAL;
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	error = set_mount_path(mountp, directory, name);
	if (error != 0)
		goto fail;
	path_set(&mountp->m_cover, directory->p_mount, directory->p_inode);
	mountp->m_parent = directory->p_mount;
	mountp->m_bind_source = source->p_mount;
	mountp->m_bind_source->m_usecount++;
	mountp->m_internal_flags = MOUNT_BIND_INTERNAL;
	mountp->m_flags = source->p_mount->m_flags;
	mountp->m_type = source->p_inode->i_mount->m_type;
	mountp->m_root = source->p_inode;
	inode_ref(mountp->m_root);
	link_child(directory->p_mount, mountp);
	link_global(mountp);
	if (result != NULL)
		*result = mountp;
	return 0;
fail:
	mount_free(mountp);
	return error;
}

int
mount(const char *type_name, const char *dir, int flags, void *data)
{
	struct path root;
	if (root_mount == NULL || dir == NULL || dir[0] != '/' || dir[1] == '\0' ||
	    strchr(dir + 1, '/') != NULL)
		return EINVAL;
	path_set(&root, root_mount, root_mount->m_root);
	{
		int error = mount_at(type_name, &root, dir + 1, flags, data, NULL);
		path_release(&root);
		return error;
	}
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
mount_lookup_child(const struct path *directory,
		   const struct componentname *component, struct path *result)
{
	struct mount *child;
	if (directory == NULL || component == NULL || result == NULL)
		return EINVAL;
	for (child = directory->p_mount->m_children; child != NULL;
	     child = child->m_sibling) {
		size_t length = strlen(child->m_name);
		if (child->m_cover.p_mount == directory->p_mount &&
		    child->m_cover.p_inode == directory->p_inode &&
		    length == component->cn_namelen &&
		    !memcmp(child->m_name, component->cn_nameptr, length)) {
			path_set(result, child, child->m_root);
			return 0;
		}
	}
	return ENOENT;
}

int
mount_cross_path_parent(const struct path *current, struct path *result)
{
	if (current == NULL || current->p_mount == NULL || result == NULL)
		return EINVAL;
	if (current->p_inode != current->p_mount->m_root ||
	    current->p_mount == root_mount || current->p_mount->m_cover.p_inode == NULL)
		return ENOENT;
	path_set(result, current->p_mount->m_cover.p_mount,
		 current->p_mount->m_cover.p_inode);
	return 0;
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

int
mount_readdir_child(const struct path *directory, unsigned *cursor,
		    struct dirent *entry)
{
	struct mount *child;
	unsigned index = 0;
	if (directory == NULL || cursor == NULL || entry == NULL)
		return EINVAL;
	for (child = directory->p_mount->m_children; child != NULL;
	     child = child->m_sibling) {
		if (child->m_cover.p_mount != directory->p_mount ||
		    child->m_cover.p_inode != directory->p_inode)
			continue;
		if (index++ != *cursor)
			continue;
		memset(entry, 0, sizeof(*entry));
		entry->d_ino = child->m_root->i_ino;
		entry->d_type = INODE_DIR;
		strcpy(entry->d_name, child->m_name);
		(*cursor)++;
		return 0;
	}
	return ENOENT;
}

int
mount_child_shadows(const struct path *directory, const char *name)
{
	struct componentname component;
	struct path found;
	int error;
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = mount_lookup_child(directory, &component, &found);
	if (error == 0)
		path_release(&found);
	return error == 0;
}

static void
unlink_child(struct mount *mountp)
{
	struct mount **link;
	if (mountp->m_parent == NULL)
		return;
	for (link = &mountp->m_parent->m_children; *link != NULL;
	     link = &(*link)->m_sibling)
		if (*link == mountp) {
			*link = mountp->m_sibling;
			return;
		}
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
	if (mountp->m_children != NULL || mountp->m_usecount != 1)
		return EBUSY;
	if ((mountp->m_internal_flags & MOUNT_BIND_INTERNAL) == 0) {
		namecache_purge_mount(mountp);
		if (mountp->m_type->unmount != NULL) {
			error = mountp->m_type->unmount(mountp);
			if (error != 0)
				return error;
		}
		inode_cache_purge_mount(mountp);
		if (mountp->m_disk != NULL)
			disk_close(mountp->m_disk);
	} else if (mountp->m_bind_source != NULL &&
		   mountp->m_bind_source->m_usecount != 0) {
		mountp->m_bind_source->m_usecount--;
	}
	unlink_child(mountp);
	for (link = &mount_head; *link != NULL; link = &(*link)->m_next)
		if (*link == mountp) {
			*link = mountp->m_next;
			break;
		}
	path_release(&mountp->m_cover);
	if (mountp->m_root != NULL)
		inode_release(mountp->m_root);
	mount_free(mountp);
	return 0;
}
