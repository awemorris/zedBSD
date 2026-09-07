/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Physical writeback domains are distinct from logical block addressing. */
#include <kern/writeback.h>
#include <kern/disk.h>
#include <kern/loop.h>
#include <kern/io-context.h>
#include <errno.h>
#include <stddef.h>

extern int loop_backing_disk_ref(struct disk *, struct disk **) __attribute__((weak));

/*
 * Resolves partition and file-backed layers to one pinned physical cache domain.
 * All intermediate admissions stay pinned until resolution ends, closing detach
 * and partition-reload races. No block offsets or authorization are rewritten.
 */
int
writeback_domain_acquire(
	struct disk *disk,
	struct disk **leaf)
{
	struct disk *tokens[IO_CONTEXT_DEPTH_MAX];
	struct disk *token;
	struct disk *backing;
	unsigned count;
	unsigned index;
	int error;

	/* Rejects a missing output and admits the initial mounted device. */
	if (leaf == NULL)
		return EINVAL;
	*leaf = NULL;
	count = 0;
	error = disk_cache_acquire(disk, &token);
	if (error != 0)
		return error;

	/* Holds a bounded chain while discovering each explicit backing relation. */
	for (;;) {
		for (index = 0; index < count; index++) {
			if (tokens[index] == token)
				break;
		}
		if (index != count || count == IO_CONTEXT_DEPTH_MAX) {
			disk_cache_release(token);
			error = ELOOP;
			break;
		}
		tokens[count++] = token;
		backing = NULL;
		if (loop_backing_disk_ref != NULL)
			error = loop_backing_disk_ref(token, &backing);
		else
			error = EOPNOTSUPP;
		if (error == EOPNOTSUPP) {
			*leaf = token;
			count--;
			error = 0;
			break;
		}
		if (error != 0)
			break;

		/* Converts the temporary backing reference into lifecycle admission. */
		error = disk_cache_acquire(backing, &token);
		disk_release(backing);
		if (error != 0)
			break;
	}

	/* Retires intermediate admissions, preserving only the successful output. */
	while (count != 0)
		disk_cache_release(tokens[--count]);
	return error;
}
