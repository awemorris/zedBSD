/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mount table of zedBSD and Linux (ws188-p004, moved from the file
 * manager's mntent/mounts-mntent.c): the mntent table (MOUNTED), each file
 * system with files, the user's mounts before the system's trees (the
 * table is read twice).  The compositor reads it on its machine thread
 * alone: getmntent keeps its record in static storage.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <mntent.h>
#include <stdio.h>

static int mounts_pass(struct kl_backend_mount *list, size_t capacity, size_t *count, unsigned *skipped, int system);

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
	int error;

	/* Nothing yet. */
	*count = 0;
	*skipped = 0;

	/* The mounts outside the system's trees. */
	error = mounts_pass(list, capacity, count, skipped, 0);
	if (error != 0)
		return error;

	/* Then those in them, as room is left. */
	error = mounts_pass(list, capacity, count, skipped, 1);
	if (error != 0)
		return error;

	/* Succeeded: the mounts read. */
	return 0;
}

/* Reads the table once, adding the mounts of files that are (system 1) or are not (0) in the system's trees. */
static int
mounts_pass(
	struct kl_backend_mount *list,
	size_t capacity,
	size_t *count,
	unsigned *skipped,
	int system)
{
	struct mntent *entry;
	FILE *table;
	int inside;
	int keep;
	int added;
	int error;

	/* The table; one that cannot be opened is an error, not an empty table. */
	table = setmntent(MOUNTED, "r");
	if (table == NULL) {
		error = errno;
		if (error == 0)
			error = EIO;
		return error;
	}

	/* Each mount of the table. */
	for (;;) {
		entry = getmntent(table);
		if (entry == NULL)
			break;

		/* A pseudo file system holds no files. */
		keep = kl_backend_mounts_keep(entry->mnt_type);
		if (!keep)
			continue;

		/* Only this pass's side of the system's trees. */
		inside = kl_backend_mounts_system(entry->mnt_dir);
		if (inside != system)
			continue;

		/* No room: left out and counted. */
		if (*count == capacity) {
			(*skipped)++;
			continue;
		}

		/* The mount; one whose path does not fit is left out. */
		added = kl_backend_mounts_add(&list[*count], entry->mnt_dir, entry->mnt_type);
		if (!added) {
			(*skipped)++;
			continue;
		}
		(*count)++;
	}

	/* The table is let go. */
	(void)endmntent(table);

	/* Succeeded: this pass's mounts are added. */
	return 0;
}
