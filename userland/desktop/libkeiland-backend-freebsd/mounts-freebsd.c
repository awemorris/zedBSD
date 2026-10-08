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
 * with files, the user's mounts before the system's trees.  The compositor
 * reads it on its machine thread.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <sys/param.h>
#include <sys/mount.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

static void mounts_pass(const struct statfs *snapshot, size_t received, struct kl_backend_mount *list, size_t capacity, size_t *count, unsigned *skipped, int system);

/*
 * Reads the mounted file systems: the user's first, then the system's
 * trees'.
 */
int
kl_backend_mounts_read(
	struct kl_backend_mount *list,
	size_t capacity,
	size_t *count,
	unsigned *skipped)
{
	struct statfs *snapshot;
	size_t room;
	int total;
	int received;
	int error;

	/* Nothing yet; how many mounts there are now. */
	*count = 0;
	*skipped = 0;
	total = getfsstat(NULL, 0, MNT_NOWAIT);
	if (total < 0)
		return errno;
	if (total == 0)
		return EIO;

	/* Room for them and one more (a mount added meanwhile shows in the count). */
	room = (size_t)total + 1U;
	if (room > (size_t)LONG_MAX / sizeof(struct statfs))
		return EOVERFLOW;
	snapshot = calloc(room, sizeof(struct statfs));
	if (snapshot == NULL)
		return ENOMEM;

	/* The kernel's records. */
	received = getfsstat(snapshot, (long)(room * sizeof(struct statfs)), MNT_NOWAIT);
	if (received < 0) {
		error = errno;
		free(snapshot);
		return error;
	}

	/* The mounts outside the system's trees, then those in them. */
	mounts_pass(snapshot, (size_t)received, list, capacity, count, skipped, 0);
	mounts_pass(snapshot, (size_t)received, list, capacity, count, skipped, 1);

	/* The records are let go. */
	free(snapshot);

	/* Succeeded: the mounts read. */
	return 0;
}

/* Adds the records of files that are (system 1) or are not (0) in the system's trees, while there is room. */
static void
mounts_pass(
	const struct statfs *snapshot,
	size_t received,
	struct kl_backend_mount *list,
	size_t capacity,
	size_t *count,
	unsigned *skipped,
	int system)
{
	size_t index;
	int inside;
	int keep;
	int added;

	/* Each record. */
	for (index = 0; index < received; index++) {
		/* A pseudo file system holds no files. */
		keep = kl_backend_mounts_keep(snapshot[index].f_fstypename);
		if (!keep)
			continue;

		/* Only this pass's side of the system's trees. */
		inside = kl_backend_mounts_system(snapshot[index].f_mntonname);
		if (inside != system)
			continue;

		/* No room: left out and counted. */
		if (*count == capacity) {
			(*skipped)++;
			continue;
		}

		/* The mount; one whose path does not fit is left out. */
		added = kl_backend_mounts_add(&list[*count], snapshot[index].f_mntonname, snapshot[index].f_fstypename);
		if (!added) {
			(*skipped)++;
			continue;
		}
		(*count)++;
	}
}
