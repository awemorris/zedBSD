/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * namei
 */

#ifndef ZEDBSD_KERN_NAMEI_H
#define ZEDBSD_KERN_NAMEI_H

#include "kern/inode.h"
#include "kern/lock.h"
#include "kern/mount.h"
#include <limits.h>

#ifndef NAME_MAX
#define NAME_MAX		255U
#endif
#ifndef PATH_MAX
#define PATH_MAX		256U
#endif
#ifndef ZEDBSD_PATH_MAX
# define ZEDBSD_PATH_MAX	PATH_MAX
#endif

#define COMPONENT_LAST		0x0001U
#define COMPONENT_DOT		0x0002U
#define COMPONENT_DOTDOT	0x0004U

#define NAMEI_NOFOLLOW_FINAL	0x0001U
#define ZEDBSD_SYMLOOP_MAX	40U

struct componentname {
	const char *cn_nameptr;
	size_t cn_namelen;
	unsigned cn_flags;
};

struct cwdinfo {
	refcount_t refs;
	struct spinlock lock;
	unsigned flags;
	struct path root;
	struct path cwd;
};

int
namei_at(
	struct cwdinfo *,
	const char *,
	struct inode **);

int
namei_path_at(
	struct cwdinfo *,
	const char *,
	struct path *);

int
namei_path_flags_at(
	struct cwdinfo *,
	const char *,
	unsigned,
	struct path *);

int
namei_parent_at(
	struct cwdinfo *,
	const char *,
	struct inode **,
	struct componentname *,
	char[NAME_MAX + 1U]);

int
namei_parent_path_at(
	struct cwdinfo *,
	const char *,
	struct path *,
	struct componentname *,
	char[NAME_MAX + 1U]);

int
cwdinfo_init(
	struct cwdinfo *,
	const struct path *);

int
cwdinfo_clone(
	const struct cwdinfo *,
	struct cwdinfo **);

void
cwdinfo_retain(
	struct cwdinfo *);

void
cwdinfo_release(
	struct cwdinfo *);

void
cwdinfo_destroy(
	struct cwdinfo *);

int
fs_chdir(
	struct cwdinfo *,
	const char *);

int
fs_chdir_path(
	struct cwdinfo *,
	const struct path *);

int
fs_getcwd(
	const struct cwdinfo *,
	char *,
	size_t);

#endif
