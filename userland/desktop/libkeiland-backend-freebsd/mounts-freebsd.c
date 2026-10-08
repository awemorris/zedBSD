/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mount table of FreeBSD (ws188-p004, moved from the file manager's
 * freebsd/mounts-freebsd.c): the kernel's list of mounts (getfsstat,
 * without asking the file systems for fresh statistics), each file system
 * a user may keep files on.  The compositor reads it on its machine thread.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <sys/param.h>
#include <sys/mount.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

/*
 * Reads the mounted file systems a user may keep files on.
 */
size_t
kl_backend_mounts_read(
	struct kl_backend_mount *list,
	size_t capacity,
	unsigned *skipped)
{
	struct statfs *snapshot;
	size_t room;
	size_t count;
	size_t index;
	int total;
	int received;
	int keep;
	int added;

	/* Nothing yet; how many mounts there are now. */
	*skipped = 0;
	count = 0;
	total = getfsstat(NULL, 0, MNT_NOWAIT);
	if (total <= 0)
		return 0;

	/* Room for them and one more (a mount added meanwhile shows in the count). */
	room = (size_t)total + 1U;
	if (room > (size_t)LONG_MAX / sizeof(struct statfs))
		return 0;
	snapshot = calloc(room, sizeof(struct statfs));
	if (snapshot == NULL)
		return 0;

	/* The kernel's records. */
	received = getfsstat(snapshot, (long)(room * sizeof(struct statfs)), MNT_NOWAIT);
	if (received < 0) {
		free(snapshot);
		return 0;
	}

	/* Each file system of files, while there is room. */
	for (index = 0; index < (size_t)received; index++) {
		/* A virtual file system is no place for files. */
		keep = kl_backend_mounts_keep(snapshot[index].f_fstypename);
		if (!keep)
			continue;

		/* No room: left out and counted. */
		if (count == capacity) {
			(*skipped)++;
			continue;
		}

		/* The mount; one whose path does not fit is left out. */
		added = kl_backend_mounts_add(&list[count], snapshot[index].f_mntonname, snapshot[index].f_fstypename);
		if (!added) {
			(*skipped)++;
			continue;
		}
		count++;
	}

	/* The records are let go. */
	free(snapshot);

	/* Succeeded: the mounts read. */
	return count;
}
