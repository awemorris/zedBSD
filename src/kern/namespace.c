/*
 * zedBSD mounted-filesystem namespace
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/namespace.h"

#include <stddef.h>
#include <string.h>

#define ZEDBSD_DIRECTORY_ATTRIBUTE 0x10U

struct resolved_path {
	struct bootfs *filesystem;
	const char *relative;
};

static int mount_index(const struct bootfs_namespace *namespace,
		       const char *name, size_t length)
{
	unsigned index;

	if (namespace == NULL || name == NULL)
		return -1;
	for (index = 0; index < namespace->count; index++)
		if (strlen(namespace->mounts[index].name) == length &&
		    !memcmp(namespace->mounts[index].name, name, length))
			return (int)index;
	return -1;
}

static enum bootfs_result resolve(struct bootfs_namespace *namespace,
				     const char *path,
				     struct resolved_path *resolved)
{
	const char *name;
	const char *separator;
	size_t length;
	int index;

	if (namespace == NULL || path == NULL || resolved == NULL)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (path[0] != '/') {
		if (namespace->default_mount < 0 ||
		    (unsigned)namespace->default_mount >= namespace->count)
			return ZEDBSD_FS_NOT_FOUND;
		resolved->filesystem =
			&namespace->mounts[namespace->default_mount].filesystem;
		resolved->relative = path;
		if (path[0] == '.' && path[1] == '/')
			resolved->relative += 2;
		return ZEDBSD_FS_OK;
	}
	name = path + 1;
	separator = strchr(name, '/');
	length = separator != NULL ? (size_t)(separator - name) : strlen(name);
	if (length == 0)
		return ZEDBSD_FS_INVALID_PATH;
	index = mount_index(namespace, name, length);
	if (index < 0)
		return ZEDBSD_FS_NOT_FOUND;
	resolved->filesystem = &namespace->mounts[index].filesystem;
	resolved->relative = separator != NULL ? separator + 1 : "";
	return ZEDBSD_FS_OK;
}

static void lower_ascii(char *text)
{
	while (*text != '\0') {
		if (*text >= 'A' && *text <= 'Z')
			*text = (char)(*text - 'A' + 'a');
		text++;
	}
}

static const char *directory_path(const char *path,
				  char normalized[ZEDBSD_PATH_MAX])
{
	size_t length = strlen(path);

	if (length >= ZEDBSD_PATH_MAX)
		return NULL;
	memcpy(normalized, path, length + 1U);
	/* Noct and ordinary UNIX callers spell directories with a trailing
	 * slash.  FAT's internal resolver expects the final component without
	 * one, so normalize that representation at the namespace boundary. */
	while (length > 0 && normalized[length - 1U] == '/')
		normalized[--length] = '\0';
	return normalized;
}

void bootfs_namespace_init(struct bootfs_namespace *namespace)
{
	if (namespace == NULL)
		return;
	memset(namespace, 0, sizeof(*namespace));
	namespace->default_mount = -1;
}

int bootfs_namespace_mount(struct bootfs_namespace *namespace,
			   const char *name,
			   const struct bootfs *filesystem)
{
	size_t length;
	int existing;
	unsigned index;

	if (namespace == NULL || name == NULL || filesystem == NULL ||
	    filesystem->driver == NULL)
		return 0;
	length = strlen(name);
	if (length == 0 || length >= ZEDBSD_NAMESPACE_NAME_MAX ||
	    strchr(name, '/') != NULL || strchr(name, '\\') != NULL)
		return 0;
	existing = mount_index(namespace, name, length);
	if (existing >= 0)
		index = (unsigned)existing;
	else {
		if (namespace->count >= ZEDBSD_NAMESPACE_MAX_MOUNTS)
			return 0;
		index = namespace->count++;
	}
	memset(&namespace->mounts[index], 0, sizeof(namespace->mounts[index]));
	memcpy(namespace->mounts[index].name, name, length + 1U);
	namespace->mounts[index].filesystem = *filesystem;
	if (namespace->default_mount < 0)
		namespace->default_mount = (int)index;
	return 1;
}

int bootfs_namespace_set_default(struct bootfs_namespace *namespace,
				 const char *name)
{
	int index;

	if (name == NULL)
		return 0;
	index = mount_index(namespace, name, strlen(name));
	if (index < 0)
		return 0;
	namespace->default_mount = index;
	return 1;
}

const char *bootfs_namespace_default_name(
	const struct bootfs_namespace *namespace)
{
	if (namespace == NULL || namespace->default_mount < 0 ||
	    (unsigned)namespace->default_mount >= namespace->count)
		return NULL;
	return namespace->mounts[namespace->default_mount].name;
}

enum bootfs_result bootfs_namespace_open_result(
	struct bootfs_namespace *namespace, const char *path,
	struct bootfs_file *file)
{
	struct resolved_path resolved;
	enum bootfs_result result = resolve(namespace, path, &resolved);

	return result != ZEDBSD_FS_OK ? result :
		bootfs_open_result(resolved.filesystem, resolved.relative, file);
}

enum bootfs_result bootfs_namespace_create_result(
	struct bootfs_namespace *namespace, const char *path,
	struct bootfs_file *file)
{
	struct resolved_path resolved;
	enum bootfs_result result = resolve(namespace, path, &resolved);

	return result != ZEDBSD_FS_OK ? result :
		bootfs_create_result(resolved.filesystem, resolved.relative,
					file);
}

enum bootfs_result bootfs_namespace_stat_result(
	struct bootfs_namespace *namespace, const char *path,
	struct bootfs_dirent *entry)
{
	struct resolved_path resolved;
	char normalized[ZEDBSD_PATH_MAX];
	const char *relative;
	enum bootfs_result result;

	if (namespace == NULL || path == NULL || entry == NULL)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!strcmp(path, "/")) {
		memset(entry, 0, sizeof(*entry));
		entry->attributes = ZEDBSD_DIRECTORY_ATTRIBUTE;
		return ZEDBSD_FS_OK;
	}
	result = resolve(namespace, path, &resolved);
	if (result != ZEDBSD_FS_OK)
		return result;
	if (resolved.relative[0] == '\0') {
		memset(entry, 0, sizeof(*entry));
		entry->attributes = ZEDBSD_DIRECTORY_ATTRIBUTE;
		return ZEDBSD_FS_OK;
	}
	relative = directory_path(resolved.relative, normalized);
	if (relative == NULL)
		return ZEDBSD_FS_INVALID_PATH;
	result = bootfs_stat_result(resolved.filesystem, relative, entry);
	if (result == ZEDBSD_FS_OK)
		lower_ascii(entry->name);
	return result;
}

enum bootfs_result bootfs_namespace_readdir_result(
	struct bootfs_namespace *namespace, const char *path, unsigned index,
	struct bootfs_dirent *entry)
{
	struct resolved_path resolved;
	char normalized[ZEDBSD_PATH_MAX];
	const char *relative;
	enum bootfs_result result;

	if (namespace == NULL || path == NULL || entry == NULL)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!strcmp(path, "/")) {
		if (index >= namespace->count)
			return ZEDBSD_FS_NOT_FOUND;
		memset(entry, 0, sizeof(*entry));
		strncpy(entry->name, namespace->mounts[index].name,
			sizeof(entry->name) - 1U);
		entry->attributes = ZEDBSD_DIRECTORY_ATTRIBUTE;
		return ZEDBSD_FS_OK;
	}
	result = resolve(namespace, path, &resolved);
	if (result != ZEDBSD_FS_OK)
		return result;
	relative = directory_path(resolved.relative, normalized);
	if (relative == NULL)
		return ZEDBSD_FS_INVALID_PATH;
	result = bootfs_readdir_result(resolved.filesystem, relative, index,
					  entry);
	if (result == ZEDBSD_FS_OK)
		lower_ascii(entry->name);
	return result;
}
