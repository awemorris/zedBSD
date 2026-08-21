/*
 * mounted-filesystem namespace
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_NAMESPACE_H
#define ZEDBSD_NAMESPACE_H

#include "kern/fs.h"

#define ZEDBSD_NAMESPACE_MAX_MOUNTS 12U
#define ZEDBSD_NAMESPACE_NAME_MAX 16U

struct bootfs_namespace_mount {
	char name[ZEDBSD_NAMESPACE_NAME_MAX];
	struct bootfs filesystem;
};

struct bootfs_namespace {
	struct bootfs_namespace_mount mounts[ZEDBSD_NAMESPACE_MAX_MOUNTS];
	unsigned count;
	int default_mount;
};

void bootfs_namespace_init(struct bootfs_namespace *namespace);
int bootfs_namespace_mount(struct bootfs_namespace *namespace,
			   const char *name,
			   const struct bootfs *filesystem);
int bootfs_namespace_set_default(struct bootfs_namespace *namespace,
				 const char *name);
const char *bootfs_namespace_default_name(
	const struct bootfs_namespace *namespace);

enum bootfs_result bootfs_namespace_open_result(
	struct bootfs_namespace *namespace, const char *path,
	struct bootfs_file *file);
enum bootfs_result bootfs_namespace_create_result(
	struct bootfs_namespace *namespace, const char *path,
	struct bootfs_file *file);
enum bootfs_result bootfs_namespace_stat_result(
	struct bootfs_namespace *namespace, const char *path,
	struct bootfs_dirent *entry);
enum bootfs_result bootfs_namespace_readdir_result(
	struct bootfs_namespace *namespace, const char *path, unsigned index,
	struct bootfs_dirent *entry);

#endif
