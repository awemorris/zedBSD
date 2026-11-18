/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hierarchical memory filesystem.
 *
 * Every node keeps its directory entries, extended attributes, and symlink
 * target in kernel memory and a regular file's data in whole physical
 * pages found through a page index (tmpfs-pages.c), charged against the
 * mount's node and byte quotas and the system commit limit.  Directory
 * entries carry monotonic cookies so that readdir survives concurrent
 * renames.
 */

#include "kern/mount.h"
#include "kern/tmpfs.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/namei.h"
#include "kern/page.h"
#include "kern/pipe.h"
#include "kern/pmem.h"
#include "kern/vm-commit.h"
#include "kern/vm-reclaim.h"
#include "tmpfs-pages.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stdint.h>
#include <uapi/statvfs.h>
#include <hal/hal.h>

/*
 * The smallest node quota a mount gets.  The quota itself is the share of
 * the inode cache that all tmpfs mounts may hold together (below).
 */
#define TMPFS_DEFAULT_NODES 1024U

/*
 * The smallest byte quota a mount gets.  The quota itself is half of
 * physical memory, the Linux default (BUG-052); the pages are charged to
 * the commit limit as they are written.
 */
#define TMPFS_MINIMUM_BYTES ((uint64_t)32U * 1024U * 1024U)

/*
 * How many data pages one step of a write allocates before it takes the
 * inode lock.  The frames are taken without the lock, because taking one
 * may reclaim memory; the step bounds what a write holds unused.
 */
#define TMPFS_WRITE_BATCH 16U
#ifdef KERN_USER_ABI_LP64
#define TMPFS_OFF_MAX ((off_t)INT64_MAX)
#else
#define TMPFS_OFF_MAX ((off_t)INT32_MAX)
#endif

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
	struct tmpfs_xattr *xattrs;
	char *symlink;
	size_t symlink_length;

	/*
	 * A regular file's data pages, under inode->i_lock.  The zeroed node of
	 * kern_calloc() is an empty index; pages.count is what stat reports.
	 */
	struct tmpfs_pages pages;
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

/*
 * The nodes all tmpfs mounts hold together, roots included.
 *
 * Every tmpfs node keeps an inode in the inode cache for as long as it
 * exists, so together they must leave the cache room for every other
 * filesystem (BUG-029: a full /tmp made even /dev/null fail to open).
 * tmpfs_nodes_lock protects it; charge_node() and the mount raise it,
 * uncharge_node() lowers it.
 */
static size_t tmpfs_nodes_in_use;

/*
 * Serializes changes of tmpfs_nodes_in_use for the kernel's lifetime.  It
 * is taken inside a mount's quota lock, so it ranks above it.
 */
static struct spinlock tmpfs_nodes_lock = {
	{ 0 }, LOCK_RANK_SWAP, "tmpfs nodes", 0, 0
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
static size_t tmpfs_nodes_limit(void);
static int charge_shared_node(void);
static void uncharge_shared_node(void);
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
static uint64_t tmpfs_default_bytes(void);
static size_t count_missing_pages(struct tmpfs_node *node, uint64_t offset, size_t length, size_t *chunk);
static void take_frames(struct tmpfs_state *state, struct kern_pmem *frames, size_t wanted, size_t *taken, int *error);
static void return_frames(struct tmpfs_state *state, struct kern_pmem *frames, size_t first, size_t count);
static size_t fill_chunk(struct inode *inode, struct tmpfs_node *node, uint64_t offset, const uint8_t *in, size_t length, struct kern_pmem *frames, size_t frame_count, size_t *used, int *error);
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
static void tmpfs_retire_namespace(struct inode *inode);
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
	.retire_namespace = tmpfs_retire_namespace,
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
		if (kern_strcmp((*link)->name, name) == 0)
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
		kern_memcpy(value, attribute->value, length);

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
	name_length = kern_strlen(name);

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

	kern_memcpy(replacement->name, name, name_length + 1U);
	if (size != 0)
		kern_memcpy(replacement->value, value, size);
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
			kern_memcpy(list, attribute->name, attribute->name_length + 1U);
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
	if (kern_memcmp(component->cn_nameptr, entry->name, entry->length) != 0)
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

	/* Takes one node from the mount's quota and from the shared one. */
	error = 0;
	mutex_lock(&state->quota_lock);

	if (state->used_nodes >= state->max_nodes)
		error = ENOSPC;
	else
		error = charge_shared_node();
	if (error == 0)
		state->used_nodes++;

	mutex_unlock(&state->quota_lock);

	/* Reports whether the quota allowed it. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports how many nodes all tmpfs mounts may hold together.
 *
 * It is the inode cache less an eighth, which stays for the inodes of every
 * other filesystem, of devices, pipes and sockets.
 */
static size_t
tmpfs_nodes_limit(void)
{
	size_t capacity;

	/* The cache's slots, less the part kept for everything else. */
	capacity = inode_cache_capacity();

	/* Succeeded: the tmpfs share of the cache. */
	return capacity - capacity / 8U;
}

/* Takes one node from the share of the inode cache all tmpfs mounts hold. */
static int
charge_shared_node(void)
{
	unsigned long irq;
	size_t limit;
	int error;

	/* Refuses a node once the share is used up. */
	limit = tmpfs_nodes_limit();
	irq = spin_lock_irqsave(&tmpfs_nodes_lock);

	/* Counts the node unless the share is full. */
	error = 0;
	if (tmpfs_nodes_in_use >= limit)
		error = ENOSPC;
	else
		tmpfs_nodes_in_use++;

	/* Lets other mounts count their nodes again. */
	spin_unlock_irqrestore(&tmpfs_nodes_lock, irq);

	/* Reports a share that was used up. */
	if (error != 0)
		return error;

	/* Succeeded: the node is counted in the share. */
	return 0;
}

/* Gives one node back to the share of the inode cache. */
static void
uncharge_shared_node(void)
{
	unsigned long irq;

	/* Lowers the count under the lock. */
	irq = spin_lock_irqsave(&tmpfs_nodes_lock);

	/* The count never goes below zero. */
	if (tmpfs_nodes_in_use != 0)
		tmpfs_nodes_in_use--;

	/* Lets other mounts count their nodes again. */
	spin_unlock_irqrestore(&tmpfs_nodes_lock, irq);
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

	/* Gives the node back to the share of the inode cache. */
	uncharge_shared_node();
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

	if (state->used_bytes > state->max_bytes - KERN_PAGE_SIZE)
		error = ENOSPC;
	else
		state->used_bytes += KERN_PAGE_SIZE;

	mutex_unlock(&state->quota_lock);

	/* Then the commit limit, undoing the quota charge on failure. */
	if (error == 0) {
		error = vm_commit_reserve(KERN_PAGE_SIZE);
		if (error != 0) {
			mutex_lock(&state->quota_lock);
			state->used_bytes -= KERN_PAGE_SIZE;
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
	vm_commit_release(KERN_PAGE_SIZE);
	mutex_lock(&state->quota_lock);

	if (state->used_bytes >= KERN_PAGE_SIZE)
		state->used_bytes -= KERN_PAGE_SIZE;

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
	kern_memcpy(entry->name, component->cn_nameptr, component->cn_namelen);
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
	inode_namespace_ref(inode);
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
		node->symlink_length = kern_strlen(target);
		node->symlink = kern_malloc(node->symlink_length + 1U);
		if (node->symlink == NULL) {
			discard_unpublished(inode);
			return ENOMEM;
		}

		kern_memcpy(node->symlink, target, node->symlink_length + 1U);
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
		kern_memcpy(buffer, node->symlink, length);

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
	inode_namespace_ref(target);
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
	inode_namespace_release(entry->inode);
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
	kern_memcpy(entry->name, new_component->cn_nameptr, entry->length);
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
		inode_namespace_release(replaced->inode);
		kern_free(replaced);
	}

	/* Reports the completed rename. */
	return 0;
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
	const uint8_t *data;
	uint8_t *out;
	size_t done;
	uint64_t absolute;
	uint64_t index;
	size_t within;
	size_t count;

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
		index = absolute / KERN_PAGE_SIZE;
		within = (size_t)(absolute % KERN_PAGE_SIZE);
		count = KERN_PAGE_SIZE - within;
		if (count > length - done)
			count = length - done;

		/* A page holds data; a hole reads as zeros. */
		data = tmpfs_pages_lookup(&node->pages, index);
		if (data != NULL)
			kern_memcpy(out + done, data + within, count);
		else
			kern_memset(out + done, 0, count);
		done += count;
	}

	mutex_unlock(&inode->i_lock);

	/* Reports the bytes read. */
	return (ssize_t)done;
}

/*
 * Writes file data at an offset or at the end, allocating pages as needed.
 *
 * The write goes in steps of at most TMPFS_WRITE_BATCH pages.  Each step
 * counts the pages it is missing under the inode lock, takes and charges
 * frames for them without the lock (taking one may reclaim memory), and
 * copies under the lock again.  The caller's I/O lock serializes writes
 * of the inode, so the steps of one write are not interleaved with another.
 */
static ssize_t
tmpfs_write_at(
	struct inode *inode,
	const void *buffer,
	size_t length,
	off_t offset,
	int append)
{
	struct kern_pmem frames[TMPFS_WRITE_BATCH];
	struct tmpfs_node *node;
	const uint8_t *in;
	size_t done;
	size_t chunk;
	size_t missing;
	size_t taken;
	size_t used;
	size_t copied;
	int frame_error;
	int fill_error;
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
	if (append) {
		/* Samples the size the append starts at. */
		mutex_lock(&inode->i_lock);

		offset = inode->i_size;

		mutex_unlock(&inode->i_lock);

		/* Refuses an append past the offset limit. */
		if ((uint64_t)length >
		    (uint64_t)TMPFS_OFF_MAX - (uint64_t)offset)
			return -EFBIG;
	}

	/* Writes one step at a time. */
	while (done < length) {
		/* Counts the pages this step must add. */
		mutex_lock(&inode->i_lock);

		missing = count_missing_pages(node, (uint64_t)offset + done, length - done, &chunk);

		mutex_unlock(&inode->i_lock);

		/* Takes and charges a frame for each, without the lock. */
		take_frames(node->state, frames, missing, &taken, &frame_error);

		/* Copies the step, using the frames for the missing pages. */
		mutex_lock(&inode->i_lock);

		copied = fill_chunk(inode, node, (uint64_t)offset + done, in + done, chunk, frames, taken, &used, &fill_error);

		mutex_unlock(&inode->i_lock);

		/* Frames the step did not need go back, with their charges. */
		return_frames(node->state, frames, used, taken - used);
		done += copied;

		/*
		 * A step that stopped short for want of memory or quota ends the
		 * write: a short write reports what was written, an empty one why.
		 * One that stopped only because a page vanished between the count
		 * and the copy (a truncation) counts again in the next step.
		 */
		if (copied < chunk) {
			error = fill_error;
			if (error == 0)
				error = frame_error;
			if (error != 0) {
				if (done != 0)
					return (ssize_t)done;
				return -(ssize_t)error;
			}
		}
	}

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
	uint8_t *tail;
	uint64_t first_removed;
	size_t within;
	size_t freed;

	node = tmpfs_node(inode);

	/* Rejects a non-regular inode or a negative size. */
	if (node == NULL || inode->i_type != INODE_REG || size < 0)
		return EINVAL;

	/* The first page wholly past the new size, and where the size falls in its page. */
	first_removed = ((uint64_t)size + KERN_PAGE_SIZE - 1U) / KERN_PAGE_SIZE;
	within = (size_t)((uint64_t)size % KERN_PAGE_SIZE);

	/* Frees every page past the new size and publishes the size. */
	mutex_lock(&inode->i_lock);

	/* The pages wholly past the new size, and the tables they leave empty, go. */
	freed = tmpfs_pages_truncate(&node->pages, first_removed);

	/* Zeroes the tail of the last page so a later extension reads zeros. */
	if (within != 0) {
		tail = tmpfs_pages_lookup(&node->pages, first_removed - 1U);
		if (tail != NULL)
			kern_memset(tail + within, 0, KERN_PAGE_SIZE - within);
	}

	inode->i_size = size;

	mutex_unlock(&inode->i_lock);

	/* Gives the freed pages back to the quota and the commit limit. */
	while (freed != 0) {
		uncharge_page(node->state);
		freed--;
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
	kern_memset(status, 0, sizeof(*status));
	status->st_dev = mount_device_number(inode->i_mount);
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
	status->st_blksize = KERN_PAGE_SIZE;
	if (node != NULL) {
		status->st_blocks = (blkcnt_t)(node->pages.count *
		    (KERN_PAGE_SIZE / 512U));
	} else {
		status->st_blocks = 0;
	}
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
	kern_memset(entry, 0, sizeof(*entry));
	if (cookie == 0) {
		entry->d_ino = file->f_inode->i_ino;
		entry->d_type = INODE_DIR;
		kern_strcpy(entry->d_name, ".");
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
		kern_strcpy(entry->d_name, "..");
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
	kern_strcpy(entry->d_name, best->name);
	file->f_offset = (off_t)best->cookie;

	mutex_unlock(&node->state->namespace_lock);

	*eof = 0;

	/* Reports the next entry. */
	return 0;
}

/* Drops directory-entry owners once the whole mount has passed teardown checks. */
static void
tmpfs_retire_namespace(
	struct inode *inode)
{
	struct tmpfs_node *node;
	struct tmpfs_dirent *entry;
	struct tmpfs_dirent *next;

	/* The admitted DYING mount has no external users or namespace mutations. */
	node = tmpfs_node(inode);
	if (node == NULL)
		return;

	entry = node->children;
	node->children = NULL;

	/* Cache owners keep linked nodes alive until the subsequent inode purge. */
	while (entry != NULL) {
		next = entry->next;
		inode_namespace_release(entry->inode);
		kern_free(entry);
		entry = next;
	}
}

/* Frees everything a dead inode's node holds. */
static void
tmpfs_reclaim(
	struct inode *inode)
{
	struct tmpfs_node *node;
	struct tmpfs_xattr *attribute;
	struct tmpfs_xattr *next_attribute;
	size_t freed;

	/* Ignores an inode without a node. */
	node = tmpfs_node(inode);
	if (node == NULL)
		return;

	/* Frees the pages, returning their charges. */
	freed = tmpfs_pages_truncate(&node->pages, 0);
	while (freed != 0) {
		uncharge_page(node->state);
		freed--;
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
	int error;

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
	state->max_nodes = tmpfs_nodes_limit();
	if (state->max_nodes < TMPFS_DEFAULT_NODES)
		state->max_nodes = TMPFS_DEFAULT_NODES;
	state->max_bytes = tmpfs_default_bytes();

	/* The root is the mount's first node, counted in the shared share too. */
	error = charge_shared_node();
	if (error != 0) {
		kern_free(node);
		kern_free(state);
		return error;
	}

	state->used_nodes = 1;
	root = inode_alloc(mountp);
	if (root == NULL) {
		uncharge_shared_node();
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

	kern_memset(result, 0, sizeof(*result));
	result->f_bsize = KERN_PAGE_SIZE;
	result->f_frsize = KERN_PAGE_SIZE;
	result->f_blocks = state->max_bytes / KERN_PAGE_SIZE;
	result->f_bfree = result->f_blocks -
	    state->used_bytes / KERN_PAGE_SIZE;
	result->f_bavail = result->f_bfree;
	result->f_files = state->max_nodes;
	result->f_ffree = state->max_nodes - state->used_nodes;
	result->f_favail = result->f_ffree;
	result->f_namemax = NAME_MAX;

	mutex_unlock(&state->quota_lock);

	/* Reports the filled statistics. */
	return 0;
}

/* Returns a mount's byte quota: half of physical memory, at least the minimum. */
static uint64_t
tmpfs_default_bytes(
	void)
{
	struct kern_memstat memory;
	uint64_t half;

	/* Reads the machine's physical memory. */
	kern_memstat(&memory);
	half = (uint64_t)memory.physical_total / 2U;

	/* A small machine still gets the minimum. */
	if (half < TMPFS_MINIMUM_BYTES)
		half = TMPFS_MINIMUM_BYTES;

	/* Reports the quota in whole pages. */
	half = half - half % KERN_PAGE_SIZE;
	return half;
}

/*
 * Counts the pages one write step must add.
 *
 * The step starts at offset and covers at most TMPFS_WRITE_BATCH pages of
 * the length left; its byte length is stored in chunk.  The caller holds
 * inode->i_lock.
 */
static size_t
count_missing_pages(
	struct tmpfs_node *node,
	uint64_t offset,
	size_t length,
	size_t *chunk)
{
	uint64_t index;
	size_t within;
	size_t count;
	size_t covered;
	size_t missing;
	unsigned step;
	void *data;

	/* Nothing is covered or missing yet. */
	covered = 0;
	missing = 0;

	/* Looks at each page of the step. */
	for (step = 0; step < TMPFS_WRITE_BATCH && covered < length; step++) {
		index = (offset + covered) / KERN_PAGE_SIZE;
		within = (size_t)((offset + covered) % KERN_PAGE_SIZE);
		count = KERN_PAGE_SIZE - within;
		if (count > length - covered)
			count = length - covered;

		/* A hole needs a page. */
		data = tmpfs_pages_lookup(&node->pages, index);
		if (data == NULL)
			missing++;
		covered += count;
	}

	/* Succeeded: the step's length and the pages it needs. */
	*chunk = covered;
	return missing;
}

/*
 * Takes and charges frames for a write step.
 *
 * Each frame is charged to the mount's quota and the commit limit, then
 * taken zeroed from physical memory, reclaiming private pages when memory
 * is short.  It stops at the first failure, stores its reason in error and
 * how many frames it took in taken.  The caller holds no inode lock.
 */
static void
take_frames(
	struct tmpfs_state *state,
	struct kern_pmem *frames,
	size_t wanted,
	size_t *taken,
	int *error)
{
	void *page;
	size_t count;
	int charge_error;
	int frame_error;

	/* No frame is taken and nothing has failed yet. */
	count = 0;
	*error = 0;

	/* Charges and takes one frame at a time. */
	while (count < wanted) {
		/* Charges the quota and the commit limit first. */
		charge_error = charge_page(state);
		if (charge_error != 0) {
			*error = charge_error;
			break;
		}

		/* Then takes the frame, giving the charge back when there is none. */
		frame_error = vm_reclaim_frame_private(&frames[count]);
		if (frame_error != 0) {
			uncharge_page(state);
			*error = ENOMEM;
			break;
		}

		/* A new page reads as zeros where the write does not cover it. */
		page = kern_pmem_to_kernel(frames[count].paddr);
		kern_memset(page, 0, KERN_PAGE_SIZE);
		count++;
	}

	/* Succeeded or stopped: the frames taken. */
	*taken = count;
}

/* Frees frames a write step did not use, giving back their charges. */
static void
return_frames(
	struct tmpfs_state *state,
	struct kern_pmem *frames,
	size_t first,
	size_t count)
{
	size_t index;
	int error;

	/* Frees each unused frame and its charge. */
	for (index = first; index < first + count; index++) {
		error = kern_pmem_free(&frames[index]);
		if (error != 0)
			HAL_FATAL("tmpfs frame free failed");
		uncharge_page(state);
	}
}

/*
 * Copies one write step into the file.
 *
 * A page the file lacks takes the next of the step's frames.  The copy
 * stops at a hole with no frame left, or when the page index cannot grow
 * (error).  Publishes every completed prefix in the file size, stores how
 * many frames went into the file in used, and reports the bytes copied.
 * The caller holds inode->i_lock.
 */
static size_t
fill_chunk(
	struct inode *inode,
	struct tmpfs_node *node,
	uint64_t offset,
	const uint8_t *in,
	size_t length,
	struct kern_pmem *frames,
	size_t frame_count,
	size_t *used,
	int *error)
{
	uint8_t *data;
	uint64_t index;
	size_t within;
	size_t count;
	size_t done;
	size_t frames_used;
	int insert_error;

	/* Nothing is copied, no frame used and nothing has failed yet. */
	done = 0;
	frames_used = 0;
	*error = 0;

	/* Copies page by page. */
	while (done < length) {
		index = (offset + done) / KERN_PAGE_SIZE;
		within = (size_t)((offset + done) % KERN_PAGE_SIZE);
		count = KERN_PAGE_SIZE - within;
		if (count > length - done)
			count = length - done;

		/* A hole takes the next frame, or ends the step when none is left. */
		data = tmpfs_pages_lookup(&node->pages, index);
		if (data == NULL) {
			if (frames_used == frame_count)
				break;

			/* The frame becomes the file's page at this number. */
			insert_error = tmpfs_pages_insert(&node->pages, index, frames[frames_used].paddr);
			if (insert_error != 0) {
				*error = insert_error;
				break;
			}

			/* The page's bytes are the frame's. */
			data = kern_pmem_to_kernel(frames[frames_used].paddr);
			frames_used++;
		}

		/* Copies the bytes of this page. */
		kern_memcpy(data + within, in + done, count);
		done += count;

		/*
		 * Publishes every completed prefix before a later page can fail.
		 * A zero-length write must leave EOF unchanged.
		 */
		if ((off_t)(offset + done) > inode->i_size)
			inode->i_size = (off_t)(offset + done);
	}

	/* Succeeded or stopped: the bytes copied and the frames used. */
	*used = frames_used;
	return done;
}
