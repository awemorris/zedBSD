/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/namei.h"
#include "kern/mount.h"

#include <errno.h>
#include <string.h>

static int
path_length(const char *path, size_t *length)
{
	size_t n;
	if (path == NULL)
		return EINVAL;
	for (n = 0; n < ZEDBSD_PATH_MAX && path[n] != '\0'; n++)
		;
	if (n == 0)
		return ENOENT;
	if (n == ZEDBSD_PATH_MAX)
		return ENAMETOOLONG;
	*length = n;
	return 0;
}

static int
component_is(const char *name, size_t length, const char *literal)
{
	size_t i;
	for (i = 0; i < length && literal[i] != '\0'; i++)
		if (name[i] != literal[i])
			return 0;
	return i == length && literal[i] == '\0';
}

int
namei_path_at(struct cwdinfo *context, const char *path, struct path *result)
{
	struct path current;
	size_t length, position = 0;
	int error, trailing;

	if (context == NULL || context->root.p_inode == NULL ||
	    context->cwd.p_inode == NULL || result == NULL)
		return EINVAL;
	error = path_length(path, &length);
	if (error != 0)
		return error;
	trailing = path[length - 1U] == '/';
	if (path[0] == '/')
		path_set(&current, context->root.p_mount, context->root.p_inode);
	else
		path_set(&current, context->cwd.p_mount, context->cwd.p_inode);
	while (position < length) {
		struct componentname component;
		struct path next_path;
		struct inode *next_inode;
		size_t start;
		while (position < length && path[position] == '/')
			position++;
		if (position == length)
			break;
		start = position;
		while (position < length && path[position] != '/')
			position++;
		component.cn_nameptr = path + start;
		component.cn_namelen = position - start;
		component.cn_flags = 0;
		if (component.cn_namelen > NAME_MAX) {
			error = ENAMETOOLONG;
			goto fail;
		}
		if (position == length)
			component.cn_flags |= COMPONENT_LAST;
		if (component_is(component.cn_nameptr, component.cn_namelen, ".")) {
			component.cn_flags |= COMPONENT_DOT;
			continue;
		}
		if (component_is(component.cn_nameptr, component.cn_namelen, "..")) {
			component.cn_flags |= COMPONENT_DOTDOT;
			if (path_equal(&current, &context->root))
				continue;
			error = mount_cross_path_parent(&current, &next_path);
			if (error == 0) {
				path_release(&current);
				current = next_path;
				continue;
			}
			error = inode_lookup(current.p_inode, &component, &next_inode);
			if (error != 0)
				goto fail;
			path_set(&next_path, current.p_mount, next_inode);
			inode_release(next_inode);
			path_release(&current);
			current = next_path;
			continue;
		}
		error = mount_lookup_child(&current, &component, &next_path);
		if (error == 0) {
			path_release(&current);
			current = next_path;
			continue;
		}
		if (error != ENOENT)
			goto fail;
		error = inode_lookup(current.p_inode, &component, &next_inode);
		if (error != 0)
			goto fail;
		path_set(&next_path, current.p_mount, next_inode);
		inode_release(next_inode);
		path_release(&current);
		current = next_path;
	}
	if (trailing && current.p_inode->i_type != INODE_DIR) {
		error = ENOTDIR;
		goto fail;
	}
	*result = current;
	return 0;
fail:
	path_release(&current);
	return error;
}

int
namei_at(struct cwdinfo *context, const char *path, struct inode **result)
{
	struct path found;
	int error;
	if (result == NULL)
		return EINVAL;
	error = namei_path_at(context, path, &found);
	if (error != 0)
		return error;
	*result = found.p_inode;
	found.p_inode = NULL;
	path_release(&found);
	return 0;
}

int
namei_parent_path_at(struct cwdinfo *context, const char *path,
		     struct path *parent, struct componentname *last,
		     char storage[NAME_MAX + 1U])
{
	char prefix[ZEDBSD_PATH_MAX];
	size_t length, start, parent_length;
	int error;

	if (context == NULL || parent == NULL || last == NULL || storage == NULL)
		return EINVAL;
	error = path_length(path, &length);
	if (error != 0)
		return error;
	while (length > 1 && path[length - 1U] == '/')
		length--;
	start = length;
	while (start > 0 && path[start - 1U] != '/')
		start--;
	if (length - start == 0 || length - start > NAME_MAX)
		return length - start > NAME_MAX ? ENAMETOOLONG : EINVAL;
	memcpy(storage, path + start, length - start);
	storage[length - start] = '\0';
	if (!strcmp(storage, ".") || !strcmp(storage, ".."))
		return EINVAL;
	last->cn_nameptr = storage;
	last->cn_namelen = length - start;
	last->cn_flags = COMPONENT_LAST;
	if (start == 0) {
		prefix[0] = '.';
		prefix[1] = '\0';
	} else {
		parent_length = start;
		while (parent_length > 1 && path[parent_length - 1U] == '/')
			parent_length--;
		memcpy(prefix, path, parent_length);
		prefix[parent_length] = '\0';
	}
	error = namei_path_at(context, prefix, parent);
	if (error == 0 && parent->p_inode->i_type != INODE_DIR) {
		path_release(parent);
		return ENOTDIR;
	}
	return error;
}

int
namei_parent_at(struct cwdinfo *context, const char *path,
		struct inode **parent, struct componentname *last,
		char storage[NAME_MAX + 1U])
{
	struct path found;
	int error;
	if (parent == NULL)
		return EINVAL;
	error = namei_parent_path_at(context, path, &found, last, storage);
	if (error != 0)
		return error;
	*parent = found.p_inode;
	found.p_inode = NULL;
	path_release(&found);
	return 0;
}

int
cwdinfo_init(struct cwdinfo *context, const struct path *root)
{
	if (context == NULL || root == NULL || root->p_mount == NULL ||
	    root->p_inode == NULL || root->p_inode->i_type != INODE_DIR)
		return EINVAL;
	memset(context, 0, sizeof(*context));
	context->usecount = 1;
	path_set(&context->root, root->p_mount, root->p_inode);
	path_set(&context->cwd, root->p_mount, root->p_inode);
	context->cwd_path[0] = '/';
	context->cwd_path[1] = '\0';
	return 0;
}

void
cwdinfo_destroy(struct cwdinfo *context)
{
	if (context == NULL)
		return;
	path_release(&context->root);
	path_release(&context->cwd);
	memset(context, 0, sizeof(*context));
}

static int
normalized_path(const struct cwdinfo *context, const char *path,
		char output[ZEDBSD_PATH_MAX])
{
	char joined[ZEDBSD_PATH_MAX];
	size_t in = 0, out = 1, length, base;
	int error = path_length(path, &length);
	if (error != 0)
		return error;
	if (path[0] == '/') {
		if (length >= sizeof(joined))
			return ENAMETOOLONG;
		memcpy(joined, path, length + 1U);
	} else {
		base = strlen(context->cwd_path);
		if (base + (base > 1 ? 1U : 0U) + length >= sizeof(joined))
			return ENAMETOOLONG;
		memcpy(joined, context->cwd_path, base);
		if (base > 1)
			joined[base++] = '/';
		memcpy(joined + base, path, length + 1U);
		length += base;
	}
	output[0] = '/';
	while (in < length) {
		size_t start, component_length;
		while (in < length && joined[in] == '/')
			in++;
		if (in == length)
			break;
		start = in;
		while (in < length && joined[in] != '/')
			in++;
		component_length = in - start;
		if (component_is(joined + start, component_length, "."))
			continue;
		if (component_is(joined + start, component_length, "..")) {
			if (out > 1) {
				out--;
				while (out > 1 && output[out - 1U] != '/')
					out--;
			}
			continue;
		}
		if (out > 1) {
			if (out + 1U >= ZEDBSD_PATH_MAX)
				return ENAMETOOLONG;
			output[out++] = '/';
		}
		if (component_length >= ZEDBSD_PATH_MAX - out)
			return ENAMETOOLONG;
		memcpy(output + out, joined + start, component_length);
		out += component_length;
	}
	output[out] = '\0';
	return 0;
}

int
fs_chdir(struct cwdinfo *context, const char *path)
{
	struct path directory;
	char normalized[ZEDBSD_PATH_MAX];
	int error = namei_path_at(context, path, &directory);
	if (error != 0)
		return error;
	if (directory.p_inode->i_type != INODE_DIR) {
		path_release(&directory);
		return ENOTDIR;
	}
	error = normalized_path(context, path, normalized);
	if (error != 0) {
		path_release(&directory);
		return error;
	}
	path_release(&context->cwd);
	context->cwd = directory;
	strcpy(context->cwd_path, normalized);
	return 0;
}

const char *
fs_getcwd(const struct cwdinfo *context)
{
	return context != NULL && context->cwd.p_inode != NULL ?
		context->cwd_path : NULL;
}
