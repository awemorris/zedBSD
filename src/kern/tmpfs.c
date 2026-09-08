/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hierarchical memory filesystem.
 *
 * Every node keeps its directory entries, sparse data pages, extended
 * attributes, and symlink target in kernel memory, charged against the
 * mount's node and byte quotas and the system commit limit.  Directory
 * entries carry monotonic cookies so that readdir survives concurrent
 * renames.
 */

#include "kern/tmpfs.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/namei.h"
#include "kern/page.h"
#include "kern/pipe.h"
#include "kern/vm-commit.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/statvfs.h>

#define TMPFS_DEFAULT_NODES 1024U
#define TMPFS_DEFAULT_BYTES (32U * 1024U * 1024U)
#ifdef ZEDBSD_USER_ABI_LP64
#define TMPFS_OFF_MAX ((off_t)INT64_MAX)
#else
#define TMPFS_OFF_MAX ((off_t)INT32_MAX)
#endif

struct tmpfs_page {
	struct tmpfs_page *next;
	uint64_t index;
	uint8_t data[ZEDBSD_PAGE_SIZE];
};

struct tmpfs_dirent {
	struct tmpfs_dirent *next;
	struct inode *inode;
	uint64_t cookie;
	size_t length;
	char name[NAME_MAX + 1U];
};

struct tmpfs_xattr {
	struct tmpfs_xattr *next;
	size_t name_length;
	size_t value_length;
	char *name;
	void *value;
};

struct tmpfs_state;
struct tmpfs_node {
	struct tmpfs_state *state;
	struct inode *inode;
	struct inode *parent;
	struct tmpfs_dirent *children;
	struct tmpfs_page *pages;
	struct tmpfs_xattr *xattrs;
	char *symlink;
	size_t symlink_length;
	size_t allocated_pages;
};

struct tmpfs_state {
	struct mutex namespace_lock;
	struct mutex quota_lock;
	ino_t next_ino;
	uint64_t next_cookie;
	size_t max_nodes;
	size_t used_nodes;
	uint64_t max_bytes;
	uint64_t used_bytes;
};

static struct tmpfs_node * tmpfs_node(struct inode *inode);
static struct tmpfs_xattr ** tmpfs_find_xattr(struct tmpfs_node *node, const char *name);
static ssize_t tmpfs_getxattr(struct inode *inode, const char *name, void *value, size_t size);
static void tmpfs_free_xattr(struct tmpfs_xattr *attribute);
static int tmpfs_setxattr(struct inode *inode, const char *name, const void *value, size_t size, unsigned flags);
static ssize_t tmpfs_listxattr(struct inode *inode, char *list, size_t size);
static int tmpfs_removexattr(struct inode *inode, const char *name);
static int component_valid(const struct componentname *component);
static int component_equal(const struct componentname *component, const struct tmpfs_dirent *entry);
static struct tmpfs_dirent ** find_entry_link(struct tmpfs_node *directory, const struct componentname *component);
static int charge_node(struct tmpfs_state *state);
static void uncharge_node(struct tmpfs_state *state);
static int charge_page(struct tmpfs_state *state);
static void uncharge_page(struct tmpfs_state *state);
static struct tmpfs_dirent * allocate_entry(const struct componentname *component, struct inode *inode);
static int allocate_node(struct inode *directory, const struct inode_creation_request *request, struct inode **result);
static void discard_unpublished(struct inode *inode);
static int publish_new(struct inode *directory, const struct componentname *component, struct inode *inode);
static int tmpfs_make(struct inode *directory, const struct componentname *component, const struct inode_creation_request *request, const char *target, struct inode **result);
static int tmpfs_lookup(struct inode *directory, const struct componentname *component, struct inode **result);
static int tmpfs_create(struct inode *directory, const struct componentname *component, const struct inode_creation_request *request, struct inode **result);
static int tmpfs_mkdir(struct inode *directory, const struct componentname *component, const struct inode_creation_request *request, struct inode **result);
static int tmpfs_mknod(struct inode *directory, const struct componentname *component, const struct inode_creation_request *request, struct inode **result);
static int tmpfs_symlink(struct inode *directory, const struct componentname *component, const char *target, const struct inode_creation_request *request, struct inode **result);
static ssize_t tmpfs_readlink(struct inode *inode, char *buffer, size_t capacity);
static int tmpfs_link(struct inode *directory, const struct componentname *component, struct inode *target);
static int detach_entry(struct inode *directory, const struct componentname *component, int directory_only);
static int tmpfs_unlink(struct inode *directory, const struct componentname *component);
static int tmpfs_rmdir(struct inode *directory, const struct componentname *component);
static int tmpfs_rename(struct inode *old_directory, const struct componentname *old_component, struct inode *new_directory, const struct componentname *new_component, unsigned flags);
static struct tmpfs_page ** find_page_link(struct tmpfs_node *node, uint64_t index);
static ssize_t tmpfs_pread(struct file *file, void *buffer, size_t length, off_t offset);
static ssize_t tmpfs_write_at(struct inode *inode, const void *buffer, size_t length, off_t offset, int append);
static ssize_t tmpfs_pwrite(struct file *file, const void *buffer, size_t length, off_t offset);
static ssize_t tmpfs_read(struct file *file, void *buffer, size_t length);
static ssize_t tmpfs_write(struct file *file, const void *buffer, size_t length);
static int tmpfs_truncate(struct inode *inode, off_t size);
static int tmpfs_getattr(struct inode *inode, struct stat *status);
static int tmpfs_setattr(struct inode *inode, const struct stat *status, unsigned mask);
static int tmpfs_readdir(struct file *file, struct dirent *entry, int *eof);
static void tmpfs_reclaim(struct inode *inode);
static int tmpfs_mount_impl(struct mount *mountp);
static void tmpfs_unmount(struct mount *mountp);
static int tmpfs_statvfs(struct mount *mountp, struct statvfs *result);

static const struct inode_ops tmpfs_inode_ops = {
	.lookup = tmpfs_lookup,
	.create = tmpfs_create,
	.mkdir = tmpfs_mkdir,
	.mknod = tmpfs_mknod,
	.unlink = tmpfs_unlink,
	.rmdir = tmpfs_rmdir,
	.rename = tmpfs_rename,
	.link = tmpfs_link,
	.symlink = tmpfs_symlink,
	.readlink = tmpfs_readlink,
	.getattr = tmpfs_getattr,
	.setattr = tmpfs_setattr,
	.truncate = tmpfs_truncate,
	.getxattr = tmpfs_getxattr,
	.setxattr = tmpfs_setxattr,
	.listxattr = tmpfs_listxattr,
	.removexattr = tmpfs_removexattr,
	.reclaim = tmpfs_reclaim,
};

static const struct file_ops tmpfs_directory_ops = {
	.readdir = tmpfs_readdir,
};

static const struct file_ops tmpfs_regular_ops = {
	.read = tmpfs_read,
	.write = tmpfs_write,
	.pread = tmpfs_pread,
	.pwrite = tmpfs_pwrite,
};

const struct filesystem_type tmpfs_type = {
	.fs_name = "tmpfs",
	.fs_flags = FILESYSTEM_NODEV,
	.mount = tmpfs_mount_impl,
	.statvfs = tmpfs_statvfs,
	.unmount = tmpfs_unmount,
};

/* Reports the tmpfs node of an inode, or NULL. */
static struct tmpfs_node *
tmpfs_node(
	struct inode *inode)
{
	if (inode == NULL)
		return NULL;
	return inode->i_data;
}

/* Finds the link holding a named attribute, or the list tail when absent. */
static struct tmpfs_xattr **
tmpfs_find_xattr(
	struct tmpfs_node *node,
	const char *name)
{
	struct tmpfs_xattr **link;

	for (link = &node->xattrs; *link != NULL; link = &(*link)->next) {
		if (strcmp((*link)->name, name) == 0)
			break;
	}

	return link;
}

/* Copies an attribute value, or reports its length without a buffer. */
static ssize_t
tmpfs_getxattr(
	struct inode *inode,
	const char *name,
	void *value,
	size_t size)
{
	struct tmpfs_node *node;
	struct tmpfs_xattr *attribute;
	size_t length;

	/* Rejects an inode without a node. */
	node = tmpfs_node(inode);
	if (node == NULL)
		return -EIO;

	/* Looks the attribute up under the inode lock. */
	mutex_lock(&inode->i_lock);

	attribute = *tmpfs_find_xattr(node, name);
	if (attribute == NULL) {
		mutex_unlock(&inode->i_lock);
		return -ENODATA;
	}

	length = attribute->value_length;
	if (value != NULL && size < length) {
		mutex_unlock(&inode->i_lock);
		return -ERANGE;
	}

	if (value != NULL && length != 0)
		memcpy(value, attribute->value, length);

	mutex_unlock(&inode->i_lock);

	/* Reports the value length. */
	return (ssize_t)length;
}

/* Frees an attribute and its name and value. */
static void
tmpfs_free_xattr(
	struct tmpfs_xattr *attribute)
{
	if (attribute != NULL) {
		kern_free(attribute->value);
		kern_free(attribute->name);
		kern_free(attribute);
	}
}

/* Sets an attribute, honoring the create-only and replace-only flags. */
static int
tmpfs_setxattr(
	struct inode *inode,
	const char *name,
	const void *value,
	size_t size,
	unsigned flags)
{
	struct tmpfs_node *node;
	struct tmpfs_xattr **link;
	struct tmpfs_xattr *old;
	struct tmpfs_xattr *replacement;
	size_t name_length;

	node = tmpfs_node(inode);
	name_length = strlen(name);

	/* Rejects an inode without a node. */
	if (node == NULL)
		return EIO;

	/* Builds the replacement attribute before taking the lock. */
	replacement = kern_calloc(1, sizeof(*replacement));
	if (replacement == NULL)
		return ENOMEM;
	replacement->name = kern_malloc(name_length + 1U);
	if (size != 0)
		replacement->value = kern_malloc(size);
	else
		replacement->value = NULL;
	if (replacement->name == NULL || (size != 0 && replacement->value == NULL)) {
		tmpfs_free_xattr(replacement);
		return ENOMEM;
	}

	memcpy(replacement->name, name, name_length + 1U);
	if (size != 0)
		memcpy(replacement->value, value, size);
	replacement->name_length = name_length;
	replacement->value_length = size;

	/* Swaps it into the list under the inode lock. */
	mutex_lock(&inode->i_lock);

	link = tmpfs_find_xattr(node, name);
	old = *link;
	if ((flags & INODE_XATTR_CREATE) != 0 && old != NULL) {
		mutex_unlock(&inode->i_lock);
		tmpfs_free_xattr(replacement);
		return EEXIST;
	}

	if ((flags & INODE_XATTR_REPLACE) != 0 && old == NULL) {
		mutex_unlock(&inode->i_lock);
		tmpfs_free_xattr(replacement);
		return ENODATA;
	}

	if (old != NULL)
		replacement->next = old->next;
	else
		replacement->next = NULL;
	*link = replacement;

	mutex_unlock(&inode->i_lock);

	tmpfs_free_xattr(old);

	/* Reports the stored attribute. */
	return 0;
}

/* Lists the attribute names, or reports the space they need. */
static ssize_t
tmpfs_listxattr(
	struct inode *inode,
	char *list,
	size_t size)
{
	struct tmpfs_node *node;
	struct tmpfs_xattr *attribute;
	size_t needed;

	node = tmpfs_node(inode);
	needed = 0;

	/* Rejects an inode without a node. */
	if (node == NULL)
		return -EIO;

	/* Sizes the list, then copies the names when they fit. */
	mutex_lock(&inode->i_lock);

	for (attribute = node->xattrs; attribute != NULL;
	     attribute = attribute->next)
		needed += attribute->name_length + 1U;
	if (list != NULL && size < needed) {
		mutex_unlock(&inode->i_lock);
		return -ERANGE;
	}

	if (list != NULL) {
		for (attribute = node->xattrs; attribute != NULL;
		     attribute = attribute->next) {
			memcpy(list, attribute->name, attribute->name_length + 1U);
			list += attribute->name_length + 1U;
		}
	}

	mutex_unlock(&inode->i_lock);

	/* Reports the list length. */
	return (ssize_t)needed;
}

/* Removes a named attribute. */
static int
tmpfs_removexattr(
	struct inode *inode,
	const char *name)
{
	struct tmpfs_node *node;
	struct tmpfs_xattr **link;
	struct tmpfs_xattr *attribute;

	/* Rejects an inode without a node. */
	node = tmpfs_node(inode);
	if (node == NULL)
		return EIO;

	/* Unlinks the attribute under the lock and frees it outside. */
	mutex_lock(&inode->i_lock);

	link = tmpfs_find_xattr(node, name);
	attribute = *link;
	if (attribute != NULL)
		*link = attribute->next;

	mutex_unlock(&inode->i_lock);

	if (attribute == NULL)
		return ENODATA;
	tmpfs_free_xattr(attribute);

	/* Reports the removed attribute. */
	return 0;
}

/* Tests that a component is a usable entry name, not empty, dot, or dot-dot. */
static int
component_valid(
	const struct componentname *component)
{
	/* Rejects a missing, empty, or overlong name. */
	if (component == NULL)
		return 0;
	if (component->cn_namelen == 0)
		return 0;
	if (component->cn_namelen > NAME_MAX)
		return 0;

	/* Rejects the two special names. */
	if (component->cn_namelen == 1 && component->cn_nameptr[0] == '.')
		return 0;
	if (component->cn_namelen == 2 &&
	    component->cn_nameptr[0] == '.' &&
	    component->cn_nameptr[1] == '.')
		return 0;

	/* Reports a usable name. */
	return 1;
}

/* Tests whether a component names a directory entry. */
static int
component_equal(
	const struct componentname *component,
	const struct tmpfs_dirent *entry)
{
	if (component->cn_namelen != entry->length)
		return 0;
	if (memcmp(component->cn_nameptr, entry->name, entry->length) != 0)
		return 0;
	return 1;
}

/* Finds the link holding a named entry, or the list tail when absent. */
static struct tmpfs_dirent **
find_entry_link(
	struct tmpfs_node *directory,
	const struct componentname *component)
{
	struct tmpfs_dirent **link;

	for (link = &directory->children; *link != NULL; link = &(*link)->next) {
		if (component_equal(component, *link))
			break;
	}

	return link;
}

/* Charges one node against the mount's node quota. */
static int
charge_node(
	struct tmpfs_state *state)
{
	int error;

	/* Takes one node from the mount's quota. */
	error = 0;
	mutex_lock(&state->quota_lock);

	if (state->used_nodes >= state->max_nodes)
		error = ENOSPC;
	else
		state->used_nodes++;

	mutex_unlock(&state->quota_lock);

	/* Reports whether the quota allowed it. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Returns one node to the mount's node quota. */
static void
uncharge_node(
	struct tmpfs_state *state)
{
	mutex_lock(&state->quota_lock);

	if (state->used_nodes != 0)
		state->used_nodes--;

	mutex_unlock(&state->quota_lock);
}

/* Charges one page against the byte quota and the system commit limit. */
static int
charge_page(
	struct tmpfs_state *state)
{
	int error;

	/* Charges the mount quota first. */
	error = 0;
	mutex_lock(&state->quota_lock);

	if (state->used_bytes > state->max_bytes - ZEDBSD_PAGE_SIZE)
		error = ENOSPC;
	else
		state->used_bytes += ZEDBSD_PAGE_SIZE;

	mutex_unlock(&state->quota_lock);

	/* Then the commit limit, undoing the quota charge on failure. */
	if (error == 0) {
		error = vm_commit_reserve(ZEDBSD_PAGE_SIZE);
		if (error != 0) {
			mutex_lock(&state->quota_lock);
			state->used_bytes -= ZEDBSD_PAGE_SIZE;
			mutex_unlock(&state->quota_lock);
		}
	}

	/* Reports why the charge failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Returns one page to the commit limit and the byte quota. */
static void
uncharge_page(
	struct tmpfs_state *state)
{
	vm_commit_release(ZEDBSD_PAGE_SIZE);
	mutex_lock(&state->quota_lock);

	if (state->used_bytes >= ZEDBSD_PAGE_SIZE)
		state->used_bytes -= ZEDBSD_PAGE_SIZE;

	mutex_unlock(&state->quota_lock);
}

/* Allocates a directory entry naming an inode. */
static struct tmpfs_dirent *
allocate_entry(
	const struct componentname *component,
	struct inode *inode)
{
	struct tmpfs_dirent *entry;

	/* Allocates the directory entry. */
	entry = kern_calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;

	/* Stores the name as a terminated copy beside the inode. */
	entry->inode = inode;
	entry->length = component->cn_namelen;
	memcpy(entry->name, component->cn_nameptr, component->cn_namelen);
	entry->name[component->cn_namelen] = '\0';

	/* Reports the new entry. */
	return entry;
}

/* Allocates an unpublished inode and node of the requested type. */
static int
allocate_node(
	struct inode *directory,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct tmpfs_node *parent;
	struct tmpfs_state *state;
	struct tmpfs_node *node;
	struct inode *inode;
	enum inode_type type;
	int error;

	parent = tmpfs_node(directory);
	if (parent != NULL)
		state = parent->state;
	else
		state = NULL;

	/* Rejects a directory outside tmpfs or a missing request. */
	if (state == NULL || request == NULL)
		return EINVAL;

	/* Charges the quota and allocates the node and inode. */
	type = request->type;
	error = charge_node(state);
	if (error != 0)
		return error;
	node = kern_calloc(1, sizeof(*node));
	if (node == NULL) {
		uncharge_node(state);
		return ENOMEM;
	}

	inode = inode_alloc(directory->i_mount);
	if (inode == NULL) {
		kern_free(node);
		uncharge_node(state);
		return ENOSPC;
	}

	/* Links the node and inode; a directory starts with two links. */
	node->state = state;
	node->inode = inode;
	if (type == INODE_DIR)
		node->parent = directory;
	else
		node->parent = NULL;
	inode->i_type = type;
	inode->i_ino = state->next_ino++;
	inode->i_op = &tmpfs_inode_ops;
	if (type == INODE_DIR)
		inode->i_fop = &tmpfs_directory_ops;
	else if (type == INODE_REG)
		inode->i_fop = &tmpfs_regular_ops;
	else if (type == INODE_FIFO)
		inode->i_fop = &fifo_file_ops;
	else
		inode->i_fop = NULL;
	inode->i_data = node;
	if (type == INODE_DIR)
		inode->i_linkcount = 2;
	else
		inode->i_linkcount = 1;
	*result = inode;

	/* Reports the allocated inode. */
	return 0;
}

/* Drops an inode that was never published in a directory. */
static void
discard_unpublished(
	struct inode *inode)
{
	/* Ignores a missing inode. */
	if (inode == NULL)
		return;

	/* Marks it dead so the release reclaims it. */
	inode->i_linkcount = 0;
	inode->i_flags |= INODE_DEAD;
	inode_release(inode);
}

/* Publishes a new inode under a name in a directory. */
static int
publish_new(
	struct inode *directory,
	const struct componentname *component,
	struct inode *inode)
{
	struct tmpfs_node *parent;
	struct tmpfs_state *state;
	struct tmpfs_dirent *entry;
	struct tmpfs_dirent **link;

	parent = tmpfs_node(directory);
	state = parent->state;

	/* Rejects a failed entry allocation. */
	entry = allocate_entry(component, inode);
	if (entry == NULL)
		return ENOMEM;

	/* Links the entry unless the name is taken. */
	mutex_lock(&state->namespace_lock);

	link = find_entry_link(parent, component);
	if (*link != NULL) {
		mutex_unlock(&state->namespace_lock);
		kern_free(entry);
		return EEXIST;
	}

	entry->cookie = state->next_cookie++;

	/* The namespace holds its own reference to the inode. */
	inode_ref(inode);
	*link = entry;
	if (inode->i_type == INODE_DIR)
		directory->i_linkcount++;

	mutex_unlock(&state->namespace_lock);

	/* Reports the published inode. */
	return 0;
}

/* Creates and publishes an inode of any type, with a symlink target when given. */
static int
tmpfs_make(
	struct inode *directory,
	const struct componentname *component,
	const struct inode_creation_request *request,
	const char *target,
	struct inode **result)
{
	struct inode *inode;
	struct tmpfs_node *node;
	int error;

	inode = NULL;

	/* Rejects a missing operand or an unusable name. */
	if (directory == NULL ||
	    directory->i_type != INODE_DIR ||
	    request == NULL ||
	    result == NULL ||
	    !component_valid(component))
		return EINVAL;
	*result = NULL;

	/* Allocates the inode and applies the creation request. */
	error = allocate_node(directory, request, &inode);
	if (error != 0)
		return error;
	error = inode_creation_prepare(directory, inode, request);
	if (error != 0) {
		discard_unpublished(inode);
		return error;
	}

	/* Stores the symlink target. */
	node = tmpfs_node(inode);
	if (request->type == INODE_SYMLINK) {
		node->symlink_length = strlen(target);
		node->symlink = kern_malloc(node->symlink_length + 1U);
		if (node->symlink == NULL) {
			discard_unpublished(inode);
			return ENOMEM;
		}

		memcpy(node->symlink, target, node->symlink_length + 1U);
		inode->i_size = (off_t)node->symlink_length;
	}

	/* Publishes the inode under its name. */
	error = publish_new(directory, component, inode);
	if (error != 0) {
		discard_unpublished(inode);
		return error;
	}

	*result = inode;

	/* Reports the created inode. */
	return 0;
}

/* Looks a name up in a directory, handling dot and dot-dot. */
static int
tmpfs_lookup(
	struct inode *directory,
	const struct componentname *component,
	struct inode **result)
{
	struct tmpfs_node *node;
	struct tmpfs_dirent **link;
	struct inode *parent;

	/* Rejects a directory outside tmpfs or a missing result. */
	node = tmpfs_node(directory);
	if (node == NULL || result == NULL)
		return EINVAL;

	/* Dot is the directory itself; dot-dot its parent, or itself at the root. */
	if (component->cn_namelen == 1 && component->cn_nameptr[0] == '.') {
		inode_ref(directory);
		*result = directory;
		return 0;
	}

	if (component->cn_namelen == 2 &&
	    component->cn_nameptr[0] == '.' &&
	    component->cn_nameptr[1] == '.') {
		if (node->parent != NULL)
			parent = node->parent;
		else
			parent = directory;
		inode_ref(parent);
		*result = parent;
		return 0;
	}

	/* Searches the entries under the namespace lock. */
	mutex_lock(&node->state->namespace_lock);

	link = find_entry_link(node, component);
	if (*link == NULL) {
		mutex_unlock(&node->state->namespace_lock);
		return ENOENT;
	}

	inode_ref((*link)->inode);
	*result = (*link)->inode;

	mutex_unlock(&node->state->namespace_lock);

	/* Reports the referenced inode. */
	return 0;
}

/* Creates a regular file. */
static int
tmpfs_create(
	struct inode *directory,
	const struct componentname *component,
	const struct inode_creation_request *request,
	struct inode **result)
{
	int error;

	if (request == NULL || request->type != INODE_REG)
		return EINVAL;

	/* Reports the failure. */
	error = tmpfs_make(directory, component, request, NULL, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a directory. */
static int
tmpfs_mkdir(
	struct inode *directory,
	const struct componentname *component,
	const struct inode_creation_request *request,
	struct inode **result)
{
	int error;

	if (request == NULL || request->type != INODE_DIR)
		return EINVAL;

	/* Reports the failure. */
	error = tmpfs_make(directory, component, request, NULL, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a FIFO, socket, or device node. */
static int
tmpfs_mknod(
	struct inode *directory,
	const struct componentname *component,
	const struct inode_creation_request *request,
	struct inode **result)
{
	int error;

	/* Only the special file types are supported. */
	if (request == NULL)
		return EINVAL;
	if (request->type != INODE_FIFO &&
	    request->type != INODE_SOCKET &&
	    request->type != INODE_CHAR &&
	    request->type != INODE_BLOCK)
		return EOPNOTSUPP;

	/* Reports the failure. */
	error = tmpfs_make(directory, component, request, NULL, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a symbolic link. */
static int
tmpfs_symlink(
	struct inode *directory,
	const struct componentname *component,
	const char *target,
	const struct inode_creation_request *request,
	struct inode **result)
{
	int error;

	if (request == NULL || request->type != INODE_SYMLINK)
		return EINVAL;

	/* Reports the failure. */
	error = tmpfs_make(directory, component, request, target, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Copies a symlink target, truncated to the buffer. */
static ssize_t
tmpfs_readlink(
	struct inode *inode,
	char *buffer,
	size_t capacity)
{
	struct tmpfs_node *node;
	size_t length;

	/* Rejects an inode that is not a tmpfs symlink. */
	node = tmpfs_node(inode);
	if (node == NULL || node->symlink == NULL)
		return -EINVAL;

	/* Copies as much as fits. */
	if (node->symlink_length < capacity)
		length = node->symlink_length;
	else
		length = capacity;
	if (length != 0)
		memcpy(buffer, node->symlink, length);

	/* Reports the copied length. */
	return (ssize_t)length;
}

/* Adds a hard link to a non-directory inode of the same mount. */
static int
tmpfs_link(
	struct inode *directory,
	const struct componentname *component,
	struct inode *target)
{
	struct tmpfs_node *parent;
	struct tmpfs_dirent *entry;
	struct tmpfs_dirent **link;

	/* Rejects a directory target with EPERM and anything else unusable with EINVAL. */
	parent = tmpfs_node(directory);
	if (parent == NULL ||
	    target == NULL ||
	    target->i_mount != directory->i_mount ||
	    target->i_type == INODE_DIR ||
	    !component_valid(component)) {
		if (target != NULL && target->i_type == INODE_DIR)
			return EPERM;
		return EINVAL;
	}

	/* Links the entry unless the name is taken. */
	entry = allocate_entry(component, target);
	if (entry == NULL)
		return ENOMEM;
	mutex_lock(&parent->state->namespace_lock);

	link = find_entry_link(parent, component);
	if (*link != NULL) {
		mutex_unlock(&parent->state->namespace_lock);
		kern_free(entry);
		return EEXIST;
	}

	entry->cookie = parent->state->next_cookie++;
	inode_ref(target);
	*link = entry;

	/* inode_link() publishes the successful link-count increment. */

	mutex_unlock(&parent->state->namespace_lock);

	/* Reports the added link. */
	return 0;
}

/* Removes a directory entry, as unlink or, with directory_only, as rmdir. */
static int
detach_entry(
	struct inode *directory,
	const struct componentname *component,
	int directory_only)
{
	struct tmpfs_node *parent;
	struct tmpfs_dirent **link;
	struct tmpfs_dirent *entry;
	struct tmpfs_node *child;

	/* Rejects a directory outside tmpfs. */
	parent = tmpfs_node(directory);
	if (parent == NULL)
		return EINVAL;

	/* Finds the entry and checks its type against the operation. */
	mutex_lock(&parent->state->namespace_lock);

	link = find_entry_link(parent, component);
	entry = *link;
	if (entry == NULL) {
		mutex_unlock(&parent->state->namespace_lock);
		return ENOENT;
	}

	child = tmpfs_node(entry->inode);
	if (directory_only && entry->inode->i_type != INODE_DIR) {
		mutex_unlock(&parent->state->namespace_lock);
		return ENOTDIR;
	}

	if (!directory_only && entry->inode->i_type == INODE_DIR) {
		mutex_unlock(&parent->state->namespace_lock);
		return EISDIR;
	}

	if (directory_only && child->children != NULL) {
		mutex_unlock(&parent->state->namespace_lock);
		return ENOTEMPTY;
	}

	/* Unlinks the entry and drops the link counts it held. */
	*link = entry->next;
	if (entry->inode->i_linkcount != 0)
		entry->inode->i_linkcount--;
	if (directory_only) {
		if (directory->i_linkcount != 0)
			directory->i_linkcount--;
		if (entry->inode->i_linkcount != 0)
			entry->inode->i_linkcount--;
	}

	if (entry->inode->i_linkcount == 0)
		entry->inode->i_flags |= INODE_DEAD;

	mutex_unlock(&parent->state->namespace_lock);

	/* Drops the namespace reference and frees the entry. */
	inode_release(entry->inode);
	kern_free(entry);

	/* Reports the removed entry. */
	return 0;
}

/* Removes a non-directory entry. */
static int
tmpfs_unlink(
	struct inode *directory,
	const struct componentname *component)
{
	int error;

	/* Reports the failure. */
	error = detach_entry(directory, component, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Removes an empty directory. */
static int
tmpfs_rmdir(
	struct inode *directory,
	const struct componentname *component)
{
	int error;

	/* Reports the failure. */
	error = detach_entry(directory, component, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Renames an entry within the mount, replacing a compatible target. */
static int
tmpfs_rename(
	struct inode *old_directory,
	const struct componentname *old_component,
	struct inode *new_directory,
	const struct componentname *new_component,
	unsigned flags)
{
	struct tmpfs_node *old_parent;
	struct tmpfs_node *new_parent;
	struct tmpfs_dirent **old_link;
	struct tmpfs_dirent **new_link;
	struct tmpfs_dirent *entry;
	struct tmpfs_dirent *replaced;
	struct tmpfs_node *moved;
	struct tmpfs_node *target;
	int error;

	old_parent = tmpfs_node(old_directory);
	new_parent = tmpfs_node(new_directory);
	replaced = NULL;

	/* Rejects flags, directories outside one tmpfs, or unusable names. */
	if (flags != 0 ||
	    old_parent == NULL ||
	    new_parent == NULL ||
	    old_parent->state != new_parent->state ||
	    !component_valid(old_component) ||
	    !component_valid(new_component))
		return EINVAL;

	/* Finds the source entry. */
	mutex_lock(&old_parent->state->namespace_lock);

	old_link = find_entry_link(old_parent, old_component);
	if (*old_link == NULL) {
		mutex_unlock(&old_parent->state->namespace_lock);
		return ENOENT;
	}

	entry = *old_link;
	moved = tmpfs_node(entry->inode);

	/* A rename onto itself is a no-op. */
	new_link = find_entry_link(new_parent, new_component);
	if (*new_link == entry) {
		mutex_unlock(&old_parent->state->namespace_lock);
		return 0;
	}

	/* Unlinks a replaced target of matching kind that is not a full directory. */
	if (*new_link != NULL) {
		target = tmpfs_node((*new_link)->inode);
		if (((*new_link)->inode->i_type == INODE_DIR) !=
		    (entry->inode->i_type == INODE_DIR)) {
			if (entry->inode->i_type == INODE_DIR)
				error = ENOTDIR;
			else
				error = EISDIR;
			mutex_unlock(&old_parent->state->namespace_lock);
			return error;
		}

		if ((*new_link)->inode == entry->inode) {
			mutex_unlock(&old_parent->state->namespace_lock);
			return 0;
		}

		if ((*new_link)->inode->i_type == INODE_DIR &&
		    target->children != NULL) {
			mutex_unlock(&old_parent->state->namespace_lock);
			return ENOTEMPTY;
		}

		replaced = *new_link;
		*new_link = replaced->next;
		if (replaced->inode->i_linkcount != 0)
			replaced->inode->i_linkcount--;
		if (replaced->inode->i_type == INODE_DIR &&
		    replaced->inode->i_linkcount != 0)
			replaced->inode->i_linkcount--;
		if (replaced->inode->i_type == INODE_DIR &&
		    new_directory->i_linkcount != 0)
			new_directory->i_linkcount--;
		if (replaced->inode->i_linkcount == 0)
			replaced->inode->i_flags |= INODE_DEAD;
	}

	/* Re-finds the old link: removing the target may have changed the same list. */
	old_link = find_entry_link(old_parent, old_component);
	entry = *old_link;
	*old_link = entry->next;

	/* Moves the entry under its new name with a fresh cookie. */
	entry->length = new_component->cn_namelen;
	memcpy(entry->name, new_component->cn_nameptr, entry->length);
	entry->name[entry->length] = '\0';
	entry->cookie = old_parent->state->next_cookie++;
	entry->next = new_parent->children;
	new_parent->children = entry;

	/* A moved directory re-parents and moves its dot-dot link. */
	if (entry->inode->i_type == INODE_DIR && old_directory != new_directory) {
		if (old_directory->i_linkcount != 0)
			old_directory->i_linkcount--;
		new_directory->i_linkcount++;
		moved->parent = new_directory;
	}

	mutex_unlock(&old_parent->state->namespace_lock);

	/* Drops the replaced target outside the lock. */
	if (replaced != NULL) {
		inode_release(replaced->inode);
		kern_free(replaced);
	}

	/* Reports the completed rename. */
	return 0;
}

/* Finds the link at or after a page index in the sorted page list. */
static struct tmpfs_page **
find_page_link(
	struct tmpfs_node *node,
	uint64_t index)
{
	struct tmpfs_page **link;

	link = &node->pages;
	while (*link != NULL && (*link)->index < index)
		link = &(*link)->next;
	return link;
}

/* Reads file data at an offset, treating missing pages as zeros. */
static ssize_t
tmpfs_pread(
	struct file *file,
	void *buffer,
	size_t length,
	off_t offset)
{
	struct inode *inode;
	struct tmpfs_node *node;
	uint8_t *out;
	size_t done;
	uint64_t absolute;
	uint64_t index;
	size_t within;
	size_t count;
	struct tmpfs_page **link;

	inode = file->f_inode;
	node = tmpfs_node(inode);
	out = buffer;
	done = 0;

	/* Rejects an inode outside tmpfs or a negative offset. */
	if (node == NULL || offset < 0)
		return -EINVAL;

	/* Clamps the read to the file size. */
	mutex_lock(&inode->i_lock);

	if (offset >= inode->i_size)
		length = 0;
	else if ((uint64_t)length > (uint64_t)(inode->i_size - offset))
		length = (size_t)(inode->i_size - offset);

	/* Copies page by page, zero-filling holes. */
	while (done < length) {
		absolute = (uint64_t)offset + done;
		index = absolute / ZEDBSD_PAGE_SIZE;
		within = (size_t)(absolute % ZEDBSD_PAGE_SIZE);
		count = ZEDBSD_PAGE_SIZE - within;
		link = find_page_link(node, index);
		if (count > length - done)
			count = length - done;
		if (*link != NULL && (*link)->index == index)
			memcpy(out + done, (*link)->data + within, count);
		else
			memset(out + done, 0, count);
		done += count;
	}

	mutex_unlock(&inode->i_lock);

	/* Reports the bytes read. */
	return (ssize_t)done;
}

/* Writes file data at an offset or at the end, allocating pages as needed. */
static ssize_t
tmpfs_write_at(
	struct inode *inode,
	const void *buffer,
	size_t length,
	off_t offset,
	int append)
{
	struct tmpfs_node *node;
	const uint8_t *in;
	size_t done;
	uint64_t absolute;
	uint64_t index;
	size_t within;
	size_t count;
	struct tmpfs_page **link;
	struct tmpfs_page *page;
	int error;

	node = tmpfs_node(inode);
	in = buffer;
	done = 0;

	/* Rejects an inode outside tmpfs or a write past the offset limit. */
	if (node == NULL ||
	    offset < 0 ||
	    (uint64_t)length > (uint64_t)TMPFS_OFF_MAX - (uint64_t)offset)
		return -EFBIG;

	/* An append starts at the current size. */
	mutex_lock(&inode->i_lock);

	if (append) {
		offset = inode->i_size;
		if ((uint64_t)length >
		    (uint64_t)TMPFS_OFF_MAX - (uint64_t)offset) {
			mutex_unlock(&inode->i_lock);
			return -EFBIG;
		}
	}

	/* Copies page by page, allocating and charging missing pages. */
	while (done < length) {
		absolute = (uint64_t)offset + done;
		index = absolute / ZEDBSD_PAGE_SIZE;
		within = (size_t)(absolute % ZEDBSD_PAGE_SIZE);
		count = ZEDBSD_PAGE_SIZE - within;
		link = find_page_link(node, index);
		if (count > length - done)
			count = length - done;
		if (*link == NULL || (*link)->index != index) {
			/* A short write reports what was done before the failure. */
			error = charge_page(node->state);
			if (error != 0) {
				mutex_unlock(&inode->i_lock);
				if (done != 0)
					return (ssize_t)done;
				return -(ssize_t)error;
			}

			page = kern_calloc(1, sizeof(*page));
			if (page == NULL) {
				uncharge_page(node->state);
				mutex_unlock(&inode->i_lock);
				if (done != 0)
					return (ssize_t)done;
				return -ENOMEM;
			}

			page->index = index;
			page->next = *link;
			*link = page;
			node->allocated_pages++;
		} else {
			page = *link;
		}

		memcpy(page->data + within, in + done, count);
		done += count;
		/*
		 * Publish every completed prefix before a later allocation can fail.
		 * A zero-length write must leave EOF unchanged. */
		if ((off_t)((uint64_t)offset + done) > inode->i_size)
			inode->i_size = (off_t)((uint64_t)offset + done);
	}

	mutex_unlock(&inode->i_lock);

	/* Reports the bytes written. */
	return (ssize_t)done;
}

/* Writes at an explicit offset. */
static ssize_t
tmpfs_pwrite(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t result;

	result = tmpfs_write_at(file->f_inode, buffer, length, offset, 0);
	return result;
}

/* Reads at the file position and advances it. */
static ssize_t
tmpfs_read(
	struct file *file,
	void *buffer,
	size_t length)
{
	ssize_t result;

	result = tmpfs_pread(file, buffer, length, file->f_offset);
	if (result > 0)
		file->f_offset += result;
	return result;
}

/* Writes at the file position, or at the end in append mode, and advances it. */
static ssize_t
tmpfs_write(
	struct file *file,
	const void *buffer,
	size_t length)
{
	off_t offset;
	ssize_t result;

	offset = file->f_offset;

	/* An append leaves the position at the new end of the file. */
	result = tmpfs_write_at(file->f_inode, buffer, length, offset,
	    (file_status_flags_get(file) & O_APPEND) != 0);
	if (result > 0) {
		if ((file_status_flags_get(file) & O_APPEND) != 0)
			file->f_offset = file->f_inode->i_size;
		else
			file->f_offset = offset + result;
	}

	return result;
}

/* Truncates or extends a regular file, freeing pages past the new size. */
static int
tmpfs_truncate(
	struct inode *inode,
	off_t size)
{
	struct tmpfs_node *node;
	struct tmpfs_page **link;
	struct tmpfs_page *free_list;
	uint64_t last_index;
	struct tmpfs_page *page;
	struct tmpfs_page **tail;
	struct tmpfs_page *next;

	node = tmpfs_node(inode);
	free_list = NULL;

	/* Rejects a non-regular inode or a negative size. */
	if (node == NULL || inode->i_type != INODE_REG || size < 0)
		return EINVAL;

	/* Moves every page past the new size to the free list. */
	mutex_lock(&inode->i_lock);

	if (size == 0)
		last_index = 0;
	else
		last_index = ((uint64_t)size - 1U) / ZEDBSD_PAGE_SIZE;
	for (link = &node->pages; *link != NULL;) {
		page = *link;
		if (size == 0 || page->index > last_index) {
			*link = page->next;
			page->next = free_list;
			free_list = page;
			node->allocated_pages--;
		} else {
			link = &page->next;
		}
	}

	/* Zeroes the tail of the last page so a later extension reads zeros. */
	if (size != 0 && ((uint64_t)size % ZEDBSD_PAGE_SIZE) != 0) {
		tail = find_page_link(node, last_index);
		if (*tail != NULL && (*tail)->index == last_index)
			memset((*tail)->data + ((size_t)size % ZEDBSD_PAGE_SIZE), 0,
			    ZEDBSD_PAGE_SIZE - ((size_t)size % ZEDBSD_PAGE_SIZE));
	}

	inode->i_size = size;

	mutex_unlock(&inode->i_lock);

	/* Frees the pages outside the lock. */
	while (free_list != NULL) {
		next = free_list->next;
		kern_free(free_list);
		uncharge_page(node->state);
		free_list = next;
	}

	/* Reports the completed truncation. */
	return 0;
}

/* Fills a stat structure from the inode and its page count. */
static int
tmpfs_getattr(
	struct inode *inode,
	struct stat *status)
{
	struct tmpfs_node *node;

	/* Copies the inode's attributes into the caller's record. */
	node = tmpfs_node(inode);
	memset(status, 0, sizeof(*status));
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
	status->st_blksize = ZEDBSD_PAGE_SIZE;
	if (node != NULL)
		status->st_blocks = (blkcnt_t)(node->allocated_pages *
		    (ZEDBSD_PAGE_SIZE / 512U));
	else
		status->st_blocks = 0;
	return 0;
}

/* Applies a size change; the generic layer handles the other attributes. */
static int
tmpfs_setattr(
	struct inode *inode,
	const struct stat *status,
	unsigned mask)
{
	int error;

	if ((mask & INODE_ATTR_SIZE) != 0) {
		error = tmpfs_truncate(inode, status->st_size);
		return error;
	}

	return 0;
}

/* Reads the next directory entry by cookie order. */
static int
tmpfs_readdir(
	struct file *file,
	struct dirent *entry,
	int *eof)
{
	struct tmpfs_node *node;
	struct tmpfs_dirent *current;
	struct tmpfs_dirent *best;
	uint64_t cookie;

	node = tmpfs_node(file->f_inode);
	best = NULL;
	cookie = (uint64_t)file->f_offset;

	/* Rejects a directory outside tmpfs. */
	if (node == NULL)
		return EINVAL;

	/* Cookies zero and one are dot and dot-dot. */
	memset(entry, 0, sizeof(*entry));
	if (cookie == 0) {
		entry->d_ino = file->f_inode->i_ino;
		entry->d_type = INODE_DIR;
		strcpy(entry->d_name, ".");
		file->f_offset = 1;
		*eof = 0;
		return 0;
	}

	if (cookie == 1) {
		if (node->parent != NULL)
			entry->d_ino = node->parent->i_ino;
		else
			entry->d_ino = file->f_inode->i_ino;
		entry->d_type = INODE_DIR;
		strcpy(entry->d_name, "..");
		file->f_offset = 2;
		*eof = 0;
		return 0;
	}

	/* Finds the entry with the smallest cookie above the position. */
	mutex_lock(&node->state->namespace_lock);

	for (current = node->children; current != NULL; current = current->next) {
		if (current->cookie > cookie &&
		    (best == NULL || current->cookie < best->cookie))
			best = current;
	}

	if (best == NULL) {
		mutex_unlock(&node->state->namespace_lock);
		*eof = 1;
		return 0;
	}

	entry->d_ino = best->inode->i_ino;
	entry->d_type = best->inode->i_type;
	strcpy(entry->d_name, best->name);
	file->f_offset = (off_t)best->cookie;

	mutex_unlock(&node->state->namespace_lock);

	*eof = 0;

	/* Reports the next entry. */
	return 0;
}

/* Frees everything a dead inode's node holds. */
static void
tmpfs_reclaim(
	struct inode *inode)
{
	struct tmpfs_node *node;
	struct tmpfs_page *page;
	struct tmpfs_xattr *attribute;
	struct tmpfs_page *next_page;
	struct tmpfs_xattr *next_attribute;

	/* Ignores an inode without a node. */
	node = tmpfs_node(inode);
	if (node == NULL)
		return;

	/* Frees the pages, returning their charges. */
	page = node->pages;
	while (page != NULL) {
		next_page = page->next;
		kern_free(page);
		uncharge_page(node->state);
		page = next_page;
	}

	/* Frees the attributes. */
	attribute = node->xattrs;
	while (attribute != NULL) {
		next_attribute = attribute->next;
		tmpfs_free_xattr(attribute);
		attribute = next_attribute;
	}

	/* Frees the symlink target and the node itself. */
	kern_free(node->symlink);
	uncharge_node(node->state);
	kern_free(node);
	inode->i_data = NULL;
}

/* Mounts an empty tmpfs with its root directory. */
static int
tmpfs_mount_impl(
	struct mount *mountp)
{
	struct tmpfs_state *state;
	struct tmpfs_node *node;
	struct inode *root;

	/* Allocates the mount state and the root node together. */
	state = kern_calloc(1, sizeof(*state));
	node = kern_calloc(1, sizeof(*node));
	if (state == NULL || node == NULL) {
		kern_free(node);
		kern_free(state);
		return ENOMEM;
	}

	/* The root counts as the first node; cookies below three are reserved. */
	(void)mutex_init(&state->namespace_lock, LOCK_RANK_NAMESPACE,
	    "tmpfs namespace");
	(void)mutex_init(&state->quota_lock, LOCK_RANK_VM_OBJECT, "tmpfs quota");
	state->next_ino = 2;
	state->next_cookie = 3;
	state->max_nodes = TMPFS_DEFAULT_NODES;
	state->max_bytes = TMPFS_DEFAULT_BYTES;
	state->used_nodes = 1;
	root = inode_alloc(mountp);
	if (root == NULL) {
		kern_free(node);
		kern_free(state);
		return ENOSPC;
	}

	/* The root is its own parent and is world-writable with the sticky bit. */
	node->state = state;
	node->inode = root;
	node->parent = root;
	root->i_type = INODE_DIR;
	root->i_ino = 1;
	root->i_op = &tmpfs_inode_ops;
	root->i_fop = &tmpfs_directory_ops;
	root->i_data = node;
	root->i_linkcount = 2;
	root->i_mode = S_IFDIR | 01777U;
	root->i_flags = INODE_ROOT;
	mountp->m_data = state;
	mountp->m_root = root;

	/* Reports the mounted filesystem. */
	return 0;
}

/* Frees the mount state. */
static void
tmpfs_unmount(
	struct mount *mountp)
{
	struct tmpfs_state *state;

	state = mountp->m_data;
	if (state != NULL)
		kern_free(state);
	mountp->m_data = NULL;
}

/* Reports the quota usage in page-sized blocks and nodes. */
static int
tmpfs_statvfs(
	struct mount *mountp,
	struct statvfs *result)
{
	struct tmpfs_state *state;

	if (mountp != NULL)
		state = mountp->m_data;
	else
		state = NULL;

	/* Rejects a missing mount state or result. */
	if (state == NULL || result == NULL)
		return EINVAL;

	/* Samples the quotas under their lock. */
	mutex_lock(&state->quota_lock);

	memset(result, 0, sizeof(*result));
	result->f_bsize = ZEDBSD_PAGE_SIZE;
	result->f_frsize = ZEDBSD_PAGE_SIZE;
	result->f_blocks = state->max_bytes / ZEDBSD_PAGE_SIZE;
	result->f_bfree = result->f_blocks -
	    state->used_bytes / ZEDBSD_PAGE_SIZE;
	result->f_bavail = result->f_bfree;
	result->f_files = state->max_nodes;
	result->f_ffree = state->max_nodes - state->used_nodes;
	result->f_favail = result->f_ffree;
	result->f_namemax = NAME_MAX;

	mutex_unlock(&state->quota_lock);

	/* Reports the filled statistics. */
	return 0;
}
