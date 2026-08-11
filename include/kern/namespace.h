/*
 * Boots mounted-filesystem namespace
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_NAMESPACE_H
#define BOOTS_NAMESPACE_H

#include "kern/fs.h"

#define BOOTS_NAMESPACE_MAX_MOUNTS 12U
#define BOOTS_NAMESPACE_NAME_MAX 16U

struct boots_namespace_mount {
	char name[BOOTS_NAMESPACE_NAME_MAX];
	struct boots_filesystem filesystem;
};

struct boots_namespace {
	struct boots_namespace_mount mounts[BOOTS_NAMESPACE_MAX_MOUNTS];
	unsigned count;
	int default_mount;
};

void boots_namespace_init(struct boots_namespace *namespace);
int boots_namespace_mount(struct boots_namespace *namespace,
			   const char *name,
			   const struct boots_filesystem *filesystem);
int boots_namespace_set_default(struct boots_namespace *namespace,
				 const char *name);
const char *boots_namespace_default_name(
	const struct boots_namespace *namespace);

enum boots_fs_result boots_namespace_open_result(
	struct boots_namespace *namespace, const char *path,
	struct boots_file *file);
enum boots_fs_result boots_namespace_create_result(
	struct boots_namespace *namespace, const char *path,
	struct boots_file *file);
enum boots_fs_result boots_namespace_stat_result(
	struct boots_namespace *namespace, const char *path,
	struct boots_dirent *entry);
enum boots_fs_result boots_namespace_readdir_result(
	struct boots_namespace *namespace, const char *path, unsigned index,
	struct boots_dirent *entry);

#endif
