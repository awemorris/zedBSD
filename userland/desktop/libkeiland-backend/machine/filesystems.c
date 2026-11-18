/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The file systems' sizes as Settings' Storage shows them (ws188-p002,
 * moved from Settings' look.c): the file system of each of the usual
 * places, once for a file system, with its total, available and used
 * bytes.
 *
 * statvfs and stat are POSIX and the same on zedBSD, Linux and FreeBSD.
 * A file system is told apart by the device stat gives its place, not by
 * statvfs's f_fsid, which some file systems leave 0 (Linux) or number
 * apart from the disks (zedBSD's mounts without a disk).  A network mount
 * may make statvfs wait, so the compositor calls this on its machine
 * thread.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* The places whose file systems are read, in the order shown. */
static const char *const filesystems_places[] = { "/", "/home", "/usr", "/var", "/tmp", "/boot" };

/*
 * Reads the sizes of the usual places' file systems, each file system
 * once; returns how many were copied.
 */
size_t
kl_backend_filesystems_read(
	struct kl_backend_filesystem *list,
	size_t capacity)
{
	struct kl_backend_filesystem *filesystem;
	struct statvfs sizes;
	struct stat place_status;
	dev_t seen[sizeof(filesystems_places) / sizeof(filesystems_places[0])];
	size_t count;
	size_t place;
	size_t index;
	uint64_t free_bytes;
	int status;
	int known;

	/* Each place, until the list is full. */
	count = 0;
	for (place = 0; place < sizeof(filesystems_places) / sizeof(filesystems_places[0]); place++) {
		if (count == capacity)
			break;

		/* The device the place is on; a place that is not there is passed over. */
		status = stat(filesystems_places[place], &place_status);
		if (status != 0)
			continue;

		/* A file system already listed is not listed again. */
		known = 0;
		for (index = 0; index < count; index++) {
			if (seen[index] == place_status.st_dev)
				known = 1;
		}

		/* One already listed is passed over. */
		if (known != 0)
			continue;

		/* Its sizes; one without a size is passed over. */
		status = statvfs(filesystems_places[place], &sizes);
		if (status != 0)
			continue;
		if (sizes.f_blocks == 0U)
			continue;

		/* The file system, its total and what is still free for a user and in all. */
		seen[count] = place_status.st_dev;
		filesystem = &list[count];
		memset(filesystem, 0, sizeof(*filesystem));
		kl_backend_machine_copy(filesystem->path, sizeof(filesystem->path), filesystems_places[place], strlen(filesystems_places[place]));
		filesystem->total = (uint64_t)sizes.f_blocks * (uint64_t)sizes.f_frsize;
		filesystem->available = (uint64_t)sizes.f_bavail * (uint64_t)sizes.f_frsize;
		free_bytes = (uint64_t)sizes.f_bfree * (uint64_t)sizes.f_frsize;
		filesystem->used = filesystem->total - free_bytes;
		count++;
	}

	/* Succeeded: the file systems that could be read. */
	return count;
}
