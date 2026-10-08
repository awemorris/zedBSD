/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mounted file systems a user may keep files on (ws188-p004, moved
 * from the file manager's places.c of ws071): which file system types are
 * the system's virtual ones and are left out, shared by every operating
 * system's mount table (machine/mounts-mntent.c on zedBSD and Linux,
 * mounts-freebsd.c on FreeBSD).
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <string.h>

/*
 * The virtual file systems' types: the kernel's and the devices' trees,
 * memory, control files, and the read-only images and overlays that are
 * no place for a user's files (Linux's snaps are squashfs, often a hundred
 * of them).
 */
static const char *const mounts_virtual[] = {
	"tmpfs", "devfs", "proc", "procfs", "sysfs", "devpts", "kernfs", "fdesc", "fdescfs", "linprocfs", "linsysfs",
	"swap", "bind", "cgroup", "cgroup2", "efivarfs", "securityfs", "pstore", "bpf", "tracefs", "debugfs", "mqueue",
	"hugetlbfs", "fusectl", "configfs", "autofs", "binfmt_misc", "nsfs", "rpc_pipefs", "overlay", "squashfs"
};

/*
 * Tells whether a file system's type is one a user may keep files on.
 */
int
kl_backend_mounts_keep(
	const char *type)
{
	size_t index;
	int same;

	/* Each virtual type. */
	for (index = 0; index < sizeof(mounts_virtual) / sizeof(mounts_virtual[0]); index++) {
		/* One of them is left out. */
		same = strcmp(type, mounts_virtual[index]);
		if (same == 0)
			return 0;
	}

	/* A file system of files. */
	return 1;
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
