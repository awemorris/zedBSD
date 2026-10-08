/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mount table of zedBSD and Linux (ws188-p004, moved from the file
 * manager's mntent/mounts-mntent.c): the mntent table (MOUNTED), each
 * file system a user may keep files on.  The compositor reads it on its
 * machine thread.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <mntent.h>
#include <stdio.h>

/*
 * Reads the mounted file systems a user may keep files on.
 */
size_t
kl_backend_mounts_read(
	struct kl_backend_mount *list,
	size_t capacity,
	unsigned *skipped)
{
	struct mntent *entry;
	FILE *table;
	size_t count;
	int keep;
	int added;

	/* Nothing yet; without the table, nothing at all. */
	*skipped = 0;
	count = 0;
	table = setmntent(MOUNTED, "r");
	if (table == NULL)
		return 0;

	/* Each mount of the table. */
	for (;;) {
		entry = getmntent(table);
		if (entry == NULL)
			break;

		/* A virtual file system is no place for files. */
		keep = kl_backend_mounts_keep(entry->mnt_type);
		if (!keep)
			continue;

		/* No room: left out and counted. */
		if (count == capacity) {
			(*skipped)++;
			continue;
		}

		/* The mount; one whose path does not fit is left out. */
		added = kl_backend_mounts_add(&list[count], entry->mnt_dir, entry->mnt_type);
		if (!added) {
			(*skipped)++;
			continue;
		}
		count++;
	}

	/* The table is let go. */
	(void)endmntent(table);

	/* Succeeded: the mounts read. */
	return count;
}
