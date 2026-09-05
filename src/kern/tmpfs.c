/*
 * Hierarchical memory filesystem.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
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

static const struct inode_ops tmpfs_inode_ops;
static const struct file_ops tmpfs_regular_ops;
static const struct file_ops tmpfs_directory_ops;

static struct tmpfs_node *
tmpfs_node(struct inode *inode)
{
	return inode != NULL ? inode->i_data : NULL;
}

static struct tmpfs_xattr **
tmpfs_find_xattr(struct tmpfs_node *node, const char *name)
{
	struct tmpfs_xattr **link;
	for (link = &node->xattrs; *link != NULL; link = &(*link)->next)
		if (strcmp((*link)->name, name) == 0)
			break;
	return link;
}

static ssize_t
tmpfs_getxattr(struct inode *inode, const char *name, void *value, size_t size)
{
	struct tmpfs_node *node = tmpfs_node(inode);
	struct tmpfs_xattr *attribute;
	size_t length;
	if (node == NULL)
		return -EIO;
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
	return (ssize_t)length;
}

static void
tmpfs_free_xattr(struct tmpfs_xattr *attribute)
{
	if (attribute != NULL) {
		kern_free(attribute->value);
		kern_free(attribute->name);
		kern_free(attribute);
	}
}

static int
tmpfs_setxattr(struct inode *inode, const char *name, const void *value,
	size_t size, unsigned flags)
{
	struct tmpfs_node *node = tmpfs_node(inode);
	struct tmpfs_xattr **link, *old, *replacement;
	size_t name_length = strlen(name);
	if (node == NULL)
		return EIO;
	replacement = kern_calloc(1, sizeof(*replacement));
	if (replacement == NULL)
		return ENOMEM;
	replacement->name = kern_malloc(name_length + 1U);
	replacement->value = size != 0 ? kern_malloc(size) : NULL;
	if (replacement->name == NULL || (size != 0 && replacement->value == NULL)) {
		tmpfs_free_xattr(replacement);
		return ENOMEM;
	}
	memcpy(replacement->name, name, name_length + 1U);
	if (size != 0)
		memcpy(replacement->value, value, size);
	replacement->name_length = name_length;
	replacement->value_length = size;
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
	replacement->next = old != NULL ? old->next : NULL;
	*link = replacement;
	mutex_unlock(&inode->i_lock);
	tmpfs_free_xattr(old);
	return 0;
}

static ssize_t
tmpfs_listxattr(struct inode *inode, char *list, size_t size)
{
	struct tmpfs_node *node = tmpfs_node(inode);
	struct tmpfs_xattr *attribute;
	size_t needed = 0;
	if (node == NULL)
		return -EIO;
	mutex_lock(&inode->i_lock);
	for (attribute = node->xattrs; attribute != NULL;
	    attribute = attribute->next)
		needed += attribute->name_length + 1U;
	if (list != NULL && size < needed) {
		mutex_unlock(&inode->i_lock);
		return -ERANGE;
	}
	if (list != NULL)
		for (attribute = node->xattrs; attribute != NULL;
		    attribute = attribute->next) {
			memcpy(list, attribute->name, attribute->name_length + 1U);
			list += attribute->name_length + 1U;
		}
	mutex_unlock(&inode->i_lock);
	return (ssize_t)needed;
}

static int
tmpfs_removexattr(struct inode *inode, const char *name)
{
	struct tmpfs_node *node = tmpfs_node(inode);
	struct tmpfs_xattr **link, *attribute;
	if (node == NULL)
		return EIO;
	mutex_lock(&inode->i_lock);
	link = tmpfs_find_xattr(node, name);
	attribute = *link;
	if (attribute != NULL)
		*link = attribute->next;
	mutex_unlock(&inode->i_lock);
	if (attribute == NULL)
		return ENODATA;
	tmpfs_free_xattr(attribute);
	return 0;
}

static int
component_valid(const struct componentname *component)
{
	return component != NULL && component->cn_namelen != 0 &&
	    component->cn_namelen <= NAME_MAX &&
	    !(component->cn_namelen == 1 && component->cn_nameptr[0] == '.') &&
	    !(component->cn_namelen == 2 && component->cn_nameptr[0] == '.' &&
	    component->cn_nameptr[1] == '.');
}

static int
component_equal(const struct componentname *component,
	const struct tmpfs_dirent *entry)
{
	return component->cn_namelen == entry->length &&
	    memcmp(component->cn_nameptr, entry->name, entry->length) == 0;
}

static struct tmpfs_dirent **
find_entry_link(struct tmpfs_node *directory,
	const struct componentname *component)
{
	struct tmpfs_dirent **link;
	for (link = &directory->children; *link != NULL; link = &(*link)->next)
		if (component_equal(component, *link))
			break;
	return link;
}

static int
charge_node(struct tmpfs_state *state)
{
	int error = 0;
	mutex_lock(&state->quota_lock);
	if (state->used_nodes >= state->max_nodes)
		error = ENOSPC;
	else
		state->used_nodes++;
	mutex_unlock(&state->quota_lock);
	return error;
}

static void
uncharge_node(struct tmpfs_state *state)
{
	mutex_lock(&state->quota_lock);
	if (state->used_nodes != 0)
		state->used_nodes--;
	mutex_unlock(&state->quota_lock);
}

static int
charge_page(struct tmpfs_state *state)
{
	int error = 0;
	mutex_lock(&state->quota_lock);
	if (state->used_bytes > state->max_bytes - ZEDBSD_PAGE_SIZE)
		error = ENOSPC;
	else
		state->used_bytes += ZEDBSD_PAGE_SIZE;
	mutex_unlock(&state->quota_lock);
	if (error == 0) {
		error = vm_commit_reserve(ZEDBSD_PAGE_SIZE);
		if (error != 0) {
			mutex_lock(&state->quota_lock);
			state->used_bytes -= ZEDBSD_PAGE_SIZE;
			mutex_unlock(&state->quota_lock);
		}
	}
	return error;
}

static void
uncharge_page(struct tmpfs_state *state)
{
	vm_commit_release(ZEDBSD_PAGE_SIZE);
	mutex_lock(&state->quota_lock);
	if (state->used_bytes >= ZEDBSD_PAGE_SIZE)
		state->used_bytes -= ZEDBSD_PAGE_SIZE;
	mutex_unlock(&state->quota_lock);
}

static struct tmpfs_dirent *
allocate_entry(const struct componentname *component, struct inode *inode)
{
	struct tmpfs_dirent *entry = kern_calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;
	entry->inode = inode;
	entry->length = component->cn_namelen;
	memcpy(entry->name, component->cn_nameptr, component->cn_namelen);
	entry->name[component->cn_namelen] = '\0';
	return entry;
}

static int
allocate_node(struct inode *directory,
	const struct inode_creation_request *request, struct inode **result)
{
	struct tmpfs_node *parent = tmpfs_node(directory);
	struct tmpfs_state *state = parent != NULL ? parent->state : NULL;
	struct tmpfs_node *node;
	struct inode *inode;
	enum inode_type type;
	int error;
	if (state == NULL || request == NULL)
		return EINVAL;
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
	node->state = state;
	node->inode = inode;
	node->parent = type == INODE_DIR ? directory : NULL;
	inode->i_type = type;
	inode->i_ino = state->next_ino++;
	inode->i_op = &tmpfs_inode_ops;
	inode->i_fop = type == INODE_DIR ? &tmpfs_directory_ops :
	    type == INODE_REG ? &tmpfs_regular_ops :
	    type == INODE_FIFO ? &fifo_file_ops : NULL;
	inode->i_data = node;
	inode->i_linkcount = type == INODE_DIR ? 2 : 1;
	*result = inode;
	return 0;
}

static void
discard_unpublished(struct inode *inode)
{
	if (inode == NULL)
		return;
	inode->i_linkcount = 0;
	inode->i_flags |= INODE_DEAD;
	inode_release(inode);
}

static int
publish_new(struct inode *directory, const struct componentname *component,
	struct inode *inode)
{
	struct tmpfs_node *parent = tmpfs_node(directory);
	struct tmpfs_state *state = parent->state;
	struct tmpfs_dirent *entry = allocate_entry(component, inode);
	struct tmpfs_dirent **link;
	if (entry == NULL)
		return ENOMEM;
	mutex_lock(&state->namespace_lock);
	link = find_entry_link(parent, component);
	if (*link != NULL) {
		mutex_unlock(&state->namespace_lock);
		kern_free(entry);
		return EEXIST;
	}
	entry->cookie = state->next_cookie++;
	inode_ref(inode); /* namespace reference */
	*link = entry;
	if (inode->i_type == INODE_DIR)
		directory->i_linkcount++;
	mutex_unlock(&state->namespace_lock);
	return 0;
}

static int
tmpfs_make(struct inode *directory, const struct componentname *component,
	const struct inode_creation_request *request, const char *target,
	struct inode **result)
{
	struct inode *inode = NULL;
	struct tmpfs_node *node;
	int error;
	if (directory == NULL || directory->i_type != INODE_DIR ||
	    request == NULL || result == NULL || !component_valid(component))
		return EINVAL;
	*result = NULL;
	error = allocate_node(directory, request, &inode);
	if (error != 0)
		return error;
	error = inode_creation_prepare(directory, inode, request);
	if (error != 0) {
		discard_unpublished(inode);
		return error;
	}
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
	error = publish_new(directory, component, inode);
	if (error != 0) {
		discard_unpublished(inode);
		return error;
	}
	*result = inode;
	return 0;
}

static int
tmpfs_lookup(struct inode *directory, const struct componentname *component,
	struct inode **result)
{
	struct tmpfs_node *node = tmpfs_node(directory);
	struct tmpfs_dirent **link;
	if (node == NULL || result == NULL)
		return EINVAL;
	if (component->cn_namelen == 1 && component->cn_nameptr[0] == '.') {
		inode_ref(directory);
		*result = directory;
		return 0;
	}
	if (component->cn_namelen == 2 && component->cn_nameptr[0] == '.' &&
	    component->cn_nameptr[1] == '.') {
		struct inode *parent = node->parent != NULL ? node->parent : directory;
		inode_ref(parent);
		*result = parent;
		return 0;
	}
	mutex_lock(&node->state->namespace_lock);
	link = find_entry_link(node, component);
	if (*link == NULL) {
		mutex_unlock(&node->state->namespace_lock);
		return ENOENT;
	}
	inode_ref((*link)->inode);
	*result = (*link)->inode;
	mutex_unlock(&node->state->namespace_lock);
	return 0;
}

static int
tmpfs_create(struct inode *directory, const struct componentname *component,
	const struct inode_creation_request *request, struct inode **result)
{
	if (request == NULL || request->type != INODE_REG)
		return EINVAL;
	return tmpfs_make(directory, component, request, NULL, result);
}

static int
tmpfs_mkdir(struct inode *directory, const struct componentname *component,
	const struct inode_creation_request *request, struct inode **result)
{
	if (request == NULL || request->type != INODE_DIR)
		return EINVAL;
	return tmpfs_make(directory, component, request, NULL, result);
}

static int
tmpfs_mknod(struct inode *directory, const struct componentname *component,
	const struct inode_creation_request *request, struct inode **result)
{
	if (request == NULL)
		return EINVAL;
	if (request->type != INODE_FIFO && request->type != INODE_SOCKET &&
	    request->type != INODE_CHAR && request->type != INODE_BLOCK)
		return EOPNOTSUPP;
	return tmpfs_make(directory, component, request, NULL, result);
}

static int
tmpfs_symlink(struct inode *directory, const struct componentname *component,
	const char *target, const struct inode_creation_request *request,
	struct inode **result)
{
	if (request == NULL || request->type != INODE_SYMLINK)
		return EINVAL;
	return tmpfs_make(directory, component, request, target, result);
}

static ssize_t
tmpfs_readlink(struct inode *inode, char *buffer, size_t capacity)
{
	struct tmpfs_node *node = tmpfs_node(inode);
	size_t length;
	if (node == NULL || node->symlink == NULL)
		return -EINVAL;
	length = node->symlink_length < capacity ? node->symlink_length : capacity;
	if (length != 0)
		memcpy(buffer, node->symlink, length);
	return (ssize_t)length;
}

static int
tmpfs_link(struct inode *directory, const struct componentname *component,
	struct inode *target)
{
	struct tmpfs_node *parent = tmpfs_node(directory);
	struct tmpfs_dirent *entry;
	struct tmpfs_dirent **link;
	if (parent == NULL || target == NULL || target->i_mount != directory->i_mount ||
	    target->i_type == INODE_DIR || !component_valid(component))
		return target != NULL && target->i_type == INODE_DIR ? EPERM : EINVAL;
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
	return 0;
}

static int
detach_entry(struct inode *directory, const struct componentname *component,
	int directory_only)
{
	struct tmpfs_node *parent = tmpfs_node(directory);
	struct tmpfs_dirent **link, *entry;
	struct tmpfs_node *child;
	if (parent == NULL)
		return EINVAL;
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
	inode_release(entry->inode);
	kern_free(entry);
	return 0;
}

static int
tmpfs_unlink(struct inode *directory, const struct componentname *component)
{
	return detach_entry(directory, component, 0);
}

static int
tmpfs_rmdir(struct inode *directory, const struct componentname *component)
{
	return detach_entry(directory, component, 1);
}

static int
tmpfs_rename(struct inode *old_directory,
	const struct componentname *old_component, struct inode *new_directory,
	const struct componentname *new_component, unsigned flags)
{
	struct tmpfs_node *old_parent = tmpfs_node(old_directory);
	struct tmpfs_node *new_parent = tmpfs_node(new_directory);
	struct tmpfs_dirent **old_link, **new_link, *entry, *replaced = NULL;
	struct tmpfs_node *moved;
	if (flags != 0 || old_parent == NULL || new_parent == NULL ||
	    old_parent->state != new_parent->state ||
	    !component_valid(old_component) || !component_valid(new_component))
		return EINVAL;
	mutex_lock(&old_parent->state->namespace_lock);
	old_link = find_entry_link(old_parent, old_component);
	if (*old_link == NULL) {
		mutex_unlock(&old_parent->state->namespace_lock);
		return ENOENT;
	}
	entry = *old_link;
	moved = tmpfs_node(entry->inode);
	new_link = find_entry_link(new_parent, new_component);
	if (*new_link == entry) {
		mutex_unlock(&old_parent->state->namespace_lock);
		return 0;
	}
	if (*new_link != NULL) {
		struct tmpfs_node *target = tmpfs_node((*new_link)->inode);
		if (((*new_link)->inode->i_type == INODE_DIR) !=
		    (entry->inode->i_type == INODE_DIR)) {
			int error = entry->inode->i_type == INODE_DIR ? ENOTDIR : EISDIR;
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
	/* Re-find old link: removing destination may have changed the same list. */
	old_link = find_entry_link(old_parent, old_component);
	entry = *old_link;
	*old_link = entry->next;
	entry->length = new_component->cn_namelen;
	memcpy(entry->name, new_component->cn_nameptr, entry->length);
	entry->name[entry->length] = '\0';
	entry->cookie = old_parent->state->next_cookie++;
	entry->next = new_parent->children;
	new_parent->children = entry;
	if (entry->inode->i_type == INODE_DIR && old_directory != new_directory) {
		if (old_directory->i_linkcount != 0)
			old_directory->i_linkcount--;
		new_directory->i_linkcount++;
		moved->parent = new_directory;
	}
	mutex_unlock(&old_parent->state->namespace_lock);
	if (replaced != NULL) {
		inode_release(replaced->inode);
		kern_free(replaced);
	}
	return 0;
}

static struct tmpfs_page **
find_page_link(struct tmpfs_node *node, uint64_t index)
{
	struct tmpfs_page **link;
	for (link = &node->pages; *link != NULL && (*link)->index < index;
	    link = &(*link)->next)
		;
	return link;
}

static ssize_t
tmpfs_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	struct inode *inode = file->f_inode;
	struct tmpfs_node *node = tmpfs_node(inode);
	uint8_t *out = buffer;
	size_t done = 0;
	if (node == NULL || offset < 0)
		return -EINVAL;
	mutex_lock(&inode->i_lock);
	if (offset >= inode->i_size)
		length = 0;
	else if ((uint64_t)length > (uint64_t)(inode->i_size - offset))
		length = (size_t)(inode->i_size - offset);
	while (done < length) {
		uint64_t absolute = (uint64_t)offset + done;
		uint64_t index = absolute / ZEDBSD_PAGE_SIZE;
		size_t within = (size_t)(absolute % ZEDBSD_PAGE_SIZE);
		size_t count = ZEDBSD_PAGE_SIZE - within;
		struct tmpfs_page **link = find_page_link(node, index);
		if (count > length - done)
			count = length - done;
		if (*link != NULL && (*link)->index == index)
			memcpy(out + done, (*link)->data + within, count);
		else
			memset(out + done, 0, count);
		done += count;
	}
	mutex_unlock(&inode->i_lock);
	return (ssize_t)done;
}

static ssize_t
tmpfs_write_at(struct inode *inode, const void *buffer, size_t length,
	off_t offset, int append)
{
	struct tmpfs_node *node = tmpfs_node(inode);
	const uint8_t *in = buffer;
	size_t done = 0;
	if (node == NULL || offset < 0 ||
	    (uint64_t)length > (uint64_t)TMPFS_OFF_MAX - (uint64_t)offset)
		return -EFBIG;
	mutex_lock(&inode->i_lock);
	if (append) {
		offset = inode->i_size;
		if ((uint64_t)length >
		    (uint64_t)TMPFS_OFF_MAX - (uint64_t)offset) {
			mutex_unlock(&inode->i_lock);
			return -EFBIG;
		}
	}
	while (done < length) {
		uint64_t absolute = (uint64_t)offset + done;
		uint64_t index = absolute / ZEDBSD_PAGE_SIZE;
		size_t within = (size_t)(absolute % ZEDBSD_PAGE_SIZE);
		size_t count = ZEDBSD_PAGE_SIZE - within;
		struct tmpfs_page **link = find_page_link(node, index);
		struct tmpfs_page *page;
		int error;
		if (count > length - done)
			count = length - done;
		if (*link == NULL || (*link)->index != index) {
			error = charge_page(node->state);
			if (error != 0) {
				mutex_unlock(&inode->i_lock);
				return done != 0 ? (ssize_t)done : -(ssize_t)error;
			}
			page = kern_calloc(1, sizeof(*page));
			if (page == NULL) {
				uncharge_page(node->state);
				mutex_unlock(&inode->i_lock);
				return done != 0 ? (ssize_t)done : -ENOMEM;
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
		/* Publish every completed prefix before a later allocation can fail.
		 * A zero-length write must leave EOF unchanged. */
		if ((off_t)((uint64_t)offset + done) > inode->i_size)
			inode->i_size = (off_t)((uint64_t)offset + done);
	}
	mutex_unlock(&inode->i_lock);
	return (ssize_t)done;
}

static ssize_t
tmpfs_pwrite(struct file *file, const void *buffer, size_t length, off_t offset)
{
	return tmpfs_write_at(file->f_inode, buffer, length, offset, 0);
}

static ssize_t
tmpfs_read(struct file *file, void *buffer, size_t length)
{
	ssize_t result = tmpfs_pread(file, buffer, length, file->f_offset);
	if (result > 0)
		file->f_offset += result;
	return result;
}

static ssize_t
tmpfs_write(struct file *file, const void *buffer, size_t length)
{
	off_t offset = file->f_offset;
	ssize_t result = tmpfs_write_at(file->f_inode, buffer, length, offset,
	    (file_status_flags_get(file) & O_APPEND) != 0);
	if (result > 0)
		file->f_offset = (file_status_flags_get(file) & O_APPEND) != 0 ?
		    file->f_inode->i_size : offset + result;
	return result;
}

static int
tmpfs_truncate(struct inode *inode, off_t size)
{
	struct tmpfs_node *node = tmpfs_node(inode);
	struct tmpfs_page **link, *free_list = NULL;
	uint64_t last_index;
	if (node == NULL || inode->i_type != INODE_REG || size < 0)
		return EINVAL;
	mutex_lock(&inode->i_lock);
	last_index = size == 0 ? 0 : ((uint64_t)size - 1U) / ZEDBSD_PAGE_SIZE;
	for (link = &node->pages; *link != NULL;) {
		struct tmpfs_page *page = *link;
		if (size == 0 || page->index > last_index) {
			*link = page->next;
			page->next = free_list;
			free_list = page;
			node->allocated_pages--;
		} else {
			link = &page->next;
		}
	}
	if (size != 0 && ((uint64_t)size % ZEDBSD_PAGE_SIZE) != 0) {
		struct tmpfs_page **tail = find_page_link(node, last_index);
		if (*tail != NULL && (*tail)->index == last_index)
			memset((*tail)->data + ((size_t)size % ZEDBSD_PAGE_SIZE), 0,
			    ZEDBSD_PAGE_SIZE - ((size_t)size % ZEDBSD_PAGE_SIZE));
	}
	inode->i_size = size;
	mutex_unlock(&inode->i_lock);
	while (free_list != NULL) {
		struct tmpfs_page *next = free_list->next;
		kern_free(free_list);
		uncharge_page(node->state);
		free_list = next;
	}
	return 0;
}

static int
tmpfs_getattr(struct inode *inode, struct stat *status)
{
	struct tmpfs_node *node = tmpfs_node(inode);
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
	status->st_blocks = node != NULL ?
	    (blkcnt_t)(node->allocated_pages * (ZEDBSD_PAGE_SIZE / 512U)) : 0;
	return 0;
}

static int
tmpfs_setattr(struct inode *inode, const struct stat *status, unsigned mask)
{
	if ((mask & INODE_ATTR_SIZE) != 0)
		return tmpfs_truncate(inode, status->st_size);
	return 0;
}

static int
tmpfs_readdir(struct file *file, struct dirent *entry, int *eof)
{
	struct tmpfs_node *node = tmpfs_node(file->f_inode);
	struct tmpfs_dirent *current, *best = NULL;
	uint64_t cookie = (uint64_t)file->f_offset;
	if (node == NULL)
		return EINVAL;
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
		entry->d_ino = node->parent != NULL ? node->parent->i_ino :
		    file->f_inode->i_ino;
		entry->d_type = INODE_DIR;
		strcpy(entry->d_name, "..");
		file->f_offset = 2;
		*eof = 0;
		return 0;
	}
	mutex_lock(&node->state->namespace_lock);
	for (current = node->children; current != NULL; current = current->next)
		if (current->cookie > cookie &&
		    (best == NULL || current->cookie < best->cookie))
			best = current;
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
	return 0;
}

static void
tmpfs_reclaim(struct inode *inode)
{
	struct tmpfs_node *node = tmpfs_node(inode);
	struct tmpfs_page *page;
	struct tmpfs_xattr *attribute;
	if (node == NULL)
		return;
	page = node->pages;
	while (page != NULL) {
		struct tmpfs_page *next = page->next;
		kern_free(page);
		uncharge_page(node->state);
		page = next;
	}
	attribute = node->xattrs;
	while (attribute != NULL) {
		struct tmpfs_xattr *next = attribute->next;
		tmpfs_free_xattr(attribute);
		attribute = next;
	}
	kern_free(node->symlink);
	uncharge_node(node->state);
	kern_free(node);
	inode->i_data = NULL;
}

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

static int
tmpfs_mount_impl(struct mount *mountp)
{
	struct tmpfs_state *state;
	struct tmpfs_node *node;
	struct inode *root;
	state = kern_calloc(1, sizeof(*state));
	node = kern_calloc(1, sizeof(*node));
	if (state == NULL || node == NULL) {
		kern_free(node);
		kern_free(state);
		return ENOMEM;
	}
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
	return 0;
}

static void
tmpfs_unmount(struct mount *mountp)
{
	struct tmpfs_state *state = mountp->m_data;
	if (state != NULL)
		kern_free(state);
	mountp->m_data = NULL;
}

static int
tmpfs_statvfs(struct mount *mountp, struct statvfs *result)
{
	struct tmpfs_state *state = mountp != NULL ? mountp->m_data : NULL;
	if (state == NULL || result == NULL)
		return EINVAL;
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
	return 0;
}

const struct filesystem_type tmpfs_type = {
	.fs_name = "tmpfs",
	.fs_flags = FILESYSTEM_NODEV,
	.mount = tmpfs_mount_impl,
	.statvfs = tmpfs_statvfs,
	.unmount = tmpfs_unmount,
};
