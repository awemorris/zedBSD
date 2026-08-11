/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

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
	for (n = 0; n < BOOTS_PATH_MAX && path[n] != '\0'; n++)
		;
	if (n == 0)
		return ENOENT;
	if (n == BOOTS_PATH_MAX)
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
namei_at(struct cwdinfo *context, const char *path, struct inode **result)
{
	struct inode *current;
	size_t length, position = 0;
	int error, trailing;

	if (context == NULL || context->root == NULL ||
	    context->cwd == NULL || result == NULL)
		return EINVAL;
	error = path_length(path, &length);
	if (error != 0)
		return error;
	trailing = path[length - 1U] == '/';
	current = path[0] == '/' ? context->root : context->cwd;
	inode_ref(current);
	while (position < length) {
		struct componentname component;
		struct inode *next;
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
		if (component_is(component.cn_nameptr,
				 component.cn_namelen, ".")) {
			component.cn_flags |= COMPONENT_DOT;
			continue;
		}
		if (component_is(component.cn_nameptr,
				 component.cn_namelen, "..")) {
			component.cn_flags |= COMPONENT_DOTDOT;
			if (current == context->root)
				continue;
			error = mount_cross_parent(current, &next);
			if (error != 0)
				error = inode_lookup(current, &component, &next);
		} else {
			error = inode_lookup(current, &component, &next);
		}
		if (error != 0)
			goto fail;
		inode_release(current);
		current = next;
		if (!(component.cn_flags & COMPONENT_DOTDOT)) {
			error = mount_follow(current, &next);
			if (error == 0) {
				inode_release(current);
				current = next;
			} else if (error != ENOENT) {
				goto fail;
			}
		}
	}
	if (trailing && current->i_type != INODE_DIR) {
		error = ENOTDIR;
		goto fail;
	}
	*result = current;
	return 0;

fail:
	inode_release(current);
	return error;
}

int
namei_parent_at(struct cwdinfo *context, const char *path,
		struct inode **parent, struct componentname *last,
		char storage[NAME_MAX + 1U])
{
	char prefix[BOOTS_PATH_MAX];
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
	error = namei_at(context, prefix, parent);
	if (error == 0 && (*parent)->i_type != INODE_DIR) {
		inode_release(*parent);
		return ENOTDIR;
	}
	return error;
}

int
cwdinfo_init(struct cwdinfo *context, struct inode *root)
{
	if (context == NULL || root == NULL || root->i_type != INODE_DIR)
		return EINVAL;
	memset(context, 0, sizeof(*context));
	inode_ref(root);
	inode_ref(root);
	context->usecount = 1;
	context->root = root;
	context->cwd = root;
	context->cwd_path[0] = '/';
	context->cwd_path[1] = '\0';
	return 0;
}

void
cwdinfo_destroy(struct cwdinfo *context)
{
	if (context == NULL)
		return;
	inode_release(context->root);
	inode_release(context->cwd);
	memset(context, 0, sizeof(*context));
}

static int
normalized_path(const struct cwdinfo *context, const char *path,
		char output[BOOTS_PATH_MAX])
{
	char joined[BOOTS_PATH_MAX];
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
		size_t start, part;
		while (in < length && joined[in] == '/') in++;
		if (in == length) break;
		start = in;
		while (in < length && joined[in] != '/') in++;
		part = in - start;
		if (part == 1 && joined[start] == '.') continue;
		if (part == 2 && joined[start] == '.' && joined[start + 1U] == '.') {
			if (out > 1) {
				while (out > 1 && output[out - 1U] != '/') out--;
				if (out > 1) out--;
			}
			continue;
		}
		if (out > 1) output[out++] = '/';
		if (out + part >= BOOTS_PATH_MAX) return ENAMETOOLONG;
		memcpy(output + out, joined + start, part);
		out += part;
	}
	output[out] = '\0';
	return 0;
}

int
fs_chdir(struct cwdinfo *context, const char *path)
{
	struct inode *directory;
	char normalized[BOOTS_PATH_MAX];
	int error = namei_at(context, path, &directory);
	if (error != 0)
		return error;
	if (directory->i_type != INODE_DIR) {
		inode_release(directory);
		return ENOTDIR;
	}
	error = normalized_path(context, path, normalized);
	if (error != 0) {
		inode_release(directory);
		return error;
	}
	inode_release(context->cwd);
	context->cwd = directory;
	strcpy(context->cwd_path, normalized);
	return 0;
}

const char *
fs_getcwd(const struct cwdinfo *context)
{
	return context != NULL && context->cwd != NULL ?
		context->cwd_path : NULL;
}
