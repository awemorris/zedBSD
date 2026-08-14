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

struct zedbsd_namespace_mount {
	char name[ZEDBSD_NAMESPACE_NAME_MAX];
	struct zedbsd_filesystem filesystem;
};

struct zedbsd_namespace {
	struct zedbsd_namespace_mount mounts[ZEDBSD_NAMESPACE_MAX_MOUNTS];
	unsigned count;
	int default_mount;
};

void zedbsd_namespace_init(struct zedbsd_namespace *namespace);
int zedbsd_namespace_mount(struct zedbsd_namespace *namespace,
			   const char *name,
			   const struct zedbsd_filesystem *filesystem);
int zedbsd_namespace_set_default(struct zedbsd_namespace *namespace,
				 const char *name);
const char *zedbsd_namespace_default_name(
	const struct zedbsd_namespace *namespace);

enum zedbsd_fs_result zedbsd_namespace_open_result(
	struct zedbsd_namespace *namespace, const char *path,
	struct zedbsd_file *file);
enum zedbsd_fs_result zedbsd_namespace_create_result(
	struct zedbsd_namespace *namespace, const char *path,
	struct zedbsd_file *file);
enum zedbsd_fs_result zedbsd_namespace_stat_result(
	struct zedbsd_namespace *namespace, const char *path,
	struct zedbsd_dirent *entry);
enum zedbsd_fs_result zedbsd_namespace_readdir_result(
	struct zedbsd_namespace *namespace, const char *path, unsigned index,
	struct zedbsd_dirent *entry);

#endif
