/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mounted file systems (ws188-p004): which file system types are
 * pseudo ones and are left out, and which mounts are the system's own and
 * come last, shared by every operating system's mount table
 * (machine/mounts-mntent.c on zedBSD and Linux, mounts-freebsd.c on
 * FreeBSD).
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <string.h>

/*
 * The pseudo file systems' types: views of the kernel, the processes and
 * the devices, and control files, which hold no files of anyone's.  tmpfs,
 * overlays and images are file systems of files (zedBSD's root is an
 * overlay, a user's own tmpfs keeps a Trash); whether they are shown is the
 * program's choice.
 */
static const char *const mounts_pseudo[] = {
	"proc", "procfs", "sysfs", "devfs", "devpts", "kernfs", "fdesc", "fdescfs", "linprocfs", "linsysfs", "swap",
	"cgroup", "cgroup2", "nsfs", "mqueue", "mqueuefs", "tracefs", "debugfs", "securityfs", "pstore", "bpf",
	"efivarfs", "configfs", "fusectl", "binfmt_misc", "rpc_pipefs", "hugetlbfs", "autofs"
};

/* The system's trees, whose mounts come last in the list. */
static const char *const mounts_system_trees[] = { "/dev", "/proc", "/sys", "/run", "/snap", "/var/lib" };

/*
 * Tells whether a file system's type has files (not a pseudo one).
 */
int
kl_backend_mounts_keep(
	const char *type)
{
	size_t index;
	int same;

	/* Each pseudo type. */
	for (index = 0; index < sizeof(mounts_pseudo) / sizeof(mounts_pseudo[0]); index++) {
		/* One of them is left out. */
		same = strcmp(type, mounts_pseudo[index]);
		if (same == 0)
			return 0;
	}

	/* A file system of files. */
	return 1;
}

/*
 * Tells whether a mount is in one of the system's trees (the tree itself
 * or below it).
 */
int
kl_backend_mounts_system(
	const char *path)
{
	size_t index;
	size_t length;
	int same;

	/* Each tree. */
	for (index = 0; index < sizeof(mounts_system_trees) / sizeof(mounts_system_trees[0]); index++) {
		/* The tree's name a whole leading part of the path. */
		length = strlen(mounts_system_trees[index]);
		same = strncmp(path, mounts_system_trees[index], length);
		if (same != 0)
			continue;

		/* The tree itself, or a path below it. */
		if (path[length] == '\0' || path[length] == '/')
			return 1;
	}

	/* Not the system's. */
	return 0;
}

/*
 * Copies one mount; a path or a type that does not fit whole is refused
 * (0), never cut.
 */
int
kl_backend_mounts_add(
	struct kl_backend_mount *mount,
	const char *path,
	const char *type)
{
	size_t path_length;
	size_t type_length;

	/* Both whole, or the mount is left out. */
	path_length = strlen(path);
	type_length = strlen(type);
	if (path_length >= sizeof(mount->path) || type_length >= sizeof(mount->type))
		return 0;

	/* The mount. */
	memset(mount, 0, sizeof(*mount));
	memcpy(mount->path, path, path_length + 1U);
	memcpy(mount->type, type, type_length + 1U);

	/* Succeeded: copied. */
	return 1;
}
