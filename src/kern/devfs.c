/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/devfs.h"
#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/mount.h"
#include "kern/namei.h"

#include <errno.h>
#include <string.h>

struct devfs_node {
	const struct cdev *device;
	struct inode *inode;
};

static struct devfs_node nodes[CDEV_MAX] __attribute__((section(".vfs_bss")));
static unsigned node_count __attribute__((section(".vfs_bss")));
static struct inode *devfs_root __attribute__((section(".vfs_bss")));

static int component_equal(const struct componentname *component,
			   const char *text)
{
	size_t length = strlen(text);
	return component->cn_namelen == length &&
		!memcmp(component->cn_nameptr, text, length);
}

static int devfs_lookup(struct inode *directory,
			const struct componentname *component,
			struct inode **result)
{
	unsigned i;
	if (component_equal(component, ".") || component_equal(component, "..")) {
		inode_ref(directory);
		*result = directory;
		return 0;
	}
	for (i = 0; i < node_count; i++)
		if (component_equal(component, nodes[i].device->name)) {
			inode_ref(nodes[i].inode);
			*result = nodes[i].inode;
			return 0;
		}
	return ENOENT;
}

static int devfs_getattr(struct inode *inode, struct stat *status)
{
	memset(status, 0, sizeof(*status));
	status->st_ino = inode->i_ino;
	status->st_mode = inode->i_mode;
	status->st_nlink = inode->i_linkcount;
	status->st_rdev = inode->i_rdev;
	return 0;
}

static int devfs_readdir(struct file *file, struct dirent *entry, int *eof)
{
	unsigned index = (unsigned)file->f_offset;
	if (index >= node_count) {
		*eof = 1;
		return 0;
	}
	memset(entry, 0, sizeof(*entry));
	entry->d_ino = nodes[index].inode->i_ino;
	entry->d_type = INODE_CHAR;
	strcpy(entry->d_name, nodes[index].device->name);
	file->f_offset++;
	*eof = 0;
	return 0;
}

static const struct inode_ops devfs_inode_ops = {
	.lookup = devfs_lookup,
	.getattr = devfs_getattr,
};

static const struct file_ops devfs_directory_ops = {
	.readdir = devfs_readdir,
};

static int devfs_mount_impl(struct mount *mountp)
{
	unsigned i;
	memset(nodes, 0, sizeof(nodes));
	node_count = cdev_count();
	devfs_root = inode_alloc(mountp);
	if (devfs_root == NULL)
		return ENOSPC;
	devfs_root->i_type = INODE_DIR;
	devfs_root->i_ino = 1;
	devfs_root->i_op = &devfs_inode_ops;
	devfs_root->i_fop = &devfs_directory_ops;
	devfs_root->i_linkcount = 1;
	devfs_root->i_mode = S_IFDIR | 0555U;
	devfs_root->i_flags = INODE_ROOT;
	for (i = 0; i < node_count; i++) {
		struct inode *inode = inode_alloc(mountp);
		if (inode == NULL)
			return ENOSPC;
		nodes[i].device = cdev_at(i);
		nodes[i].inode = inode;
		inode->i_type = INODE_CHAR;
		inode->i_ino = 2U + i;
		inode->i_op = &devfs_inode_ops;
		inode->i_fop = &cdev_file_ops;
		inode->i_data = (void *)nodes[i].device;
		inode->i_linkcount = 1;
		inode->i_mode = S_IFCHR | 0666U;
		inode->i_rdev = nodes[i].device->rdev;
	}
	mountp->m_root = devfs_root;
	return 0;
}

const struct filesystem_type devfs_type = {
	.fs_name = "devfs",
	.fs_flags = FILESYSTEM_NODEV,
	.mount = devfs_mount_impl,
};
