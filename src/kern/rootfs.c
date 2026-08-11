/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/rootfs.h"
#include "kern/file.h"
#include "kern/mount.h"
#include "kern/namecache.h"
#include "kern/namei.h"

#include <errno.h>
#include <string.h>

#define ROOTFS_NODE_MAX (MOUNT_MAX + 1U)

struct rootfs_node {
	struct inode *inode;
	struct rootfs_node *parent;
	char name[NAME_MAX + 1U];
};

static struct rootfs_node nodes[ROOTFS_NODE_MAX]
	__attribute__((section(".vfs_bss")));
static unsigned node_count;

static int
component_equal(const struct componentname *name, const char *text)
{
	size_t length = strlen(text);
	return length == name->cn_namelen &&
	       memcmp(name->cn_nameptr, text, length) == 0;
}

static int
rootfs_lookup(struct inode *directory, const struct componentname *name,
	      struct inode **result)
{
	struct rootfs_node *node = directory->i_data;
	unsigned i;
	if (component_equal(name, ".")) {
		inode_ref(directory);
		*result = directory;
		return 0;
	}
	if (component_equal(name, "..")) {
		struct inode *parent = node->parent != NULL ?
			node->parent->inode : directory;
		inode_ref(parent);
		*result = parent;
		return 0;
	}
	if (node != &nodes[0])
		return ENOENT;
	for (i = 1; i < node_count; i++) {
		if (nodes[i].inode != NULL && component_equal(name, nodes[i].name)) {
			inode_ref(nodes[i].inode);
			*result = nodes[i].inode;
			return 0;
		}
	}
	return ENOENT;
}

static int
rootfs_getattr(struct inode *inode, struct stat *status)
{
	memset(status, 0, sizeof(*status));
	status->st_ino = inode->i_ino;
	status->st_mode = inode->i_mode;
	status->st_nlink = inode->i_linkcount;
	return 0;
}

static int
rootfs_readdir(struct file *file, struct dirent *entry, int *eof)
{
	struct rootfs_node *node = file->f_inode->i_data;
	unsigned index = (unsigned)file->f_offset;
	if (node != &nodes[0]) {
		*eof = 1;
		return 0;
	}
	while (index + 1U < node_count && nodes[index + 1U].inode == NULL)
		index++;
	if (index + 1U >= node_count) {
		*eof = 1;
		return 0;
	}
	index++;
	memset(entry, 0, sizeof(*entry));
	entry->d_ino = nodes[index].inode->i_ino;
	entry->d_type = INODE_DIR;
	strcpy(entry->d_name, nodes[index].name);
	file->f_offset = (off_t)index;
	*eof = 0;
	return 0;
}

static const struct inode_ops rootfs_inode_ops = {
	.lookup = rootfs_lookup,
	.getattr = rootfs_getattr,
};

static const struct file_ops rootfs_file_ops = {
	.readdir = rootfs_readdir,
};

static int
rootfs_mount_impl(struct mount *mountp)
{
	struct inode *root = inode_alloc(mountp);
	if (root == NULL)
		return ENOSPC;
	memset(nodes, 0, sizeof(nodes));
	node_count = 1;
	root->i_type = INODE_DIR;
	root->i_ino = 1;
	root->i_op = &rootfs_inode_ops;
	root->i_fop = &rootfs_file_ops;
	root->i_data = &nodes[0];
	root->i_linkcount = 1;
	root->i_mode = S_IFDIR | 0555U;
	root->i_flags = INODE_ROOT;
	nodes[0].inode = root;
	nodes[0].parent = &nodes[0];
	nodes[0].name[0] = '\0';
	mountp->m_root = root;
	return 0;
}

const struct filesystem_type rootfs_type = {
	.fs_name = "rootfs",
	.mount = rootfs_mount_impl,
};

int
rootfs_add_mountpoint(struct mount *root_mount, const char *name,
		      struct inode **result)
{
	struct inode *inode;
	struct rootfs_node *node;
	size_t length;
	unsigned i;
	if (root_mount == NULL || name == NULL || result == NULL ||
	    root_mount->m_type != &rootfs_type)
		return EINVAL;
	length = strlen(name);
	if (length == 0 || length > NAME_MAX || strchr(name, '/') != NULL)
		return EINVAL;
	for (i = 1; i < node_count; i++)
		if (nodes[i].inode != NULL && !strcmp(nodes[i].name, name))
			return EEXIST;
	if (node_count >= ROOTFS_NODE_MAX)
		return ENOSPC;
	inode = inode_alloc(root_mount);
	if (inode == NULL)
		return ENOSPC;
	node = &nodes[node_count];
	memset(node, 0, sizeof(*node));
	node->inode = inode;
	node->parent = &nodes[0];
	strcpy(node->name, name);
	inode->i_type = INODE_DIR;
	inode->i_ino = 2U + node_count;
	inode->i_op = &rootfs_inode_ops;
	inode->i_fop = &rootfs_file_ops;
	inode->i_data = node;
	inode->i_linkcount = 1;
	inode->i_mode = S_IFDIR | 0555U;
	inode->i_flags = INODE_MOUNTPOINT;
	node_count++;
	*result = inode;
	return 0;
}

int
rootfs_remove_mountpoint(struct inode *inode)
{
	struct rootfs_node *node;
	if (inode == NULL || inode->i_mount == NULL ||
	    inode->i_mount->m_type != &rootfs_type ||
	    !(inode->i_flags & INODE_MOUNTPOINT))
		return EINVAL;
	node = inode->i_data;
	namecache_purge_inode(inode);
	inode->i_flags |= INODE_DEAD;
	node->inode = NULL;
	return 0;
}

void
rootfs_reset(void)
{
	memset(nodes, 0, sizeof(nodes));
	node_count = 0;
}
