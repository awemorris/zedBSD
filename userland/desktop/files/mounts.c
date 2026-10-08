/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mounted file systems as the desktop last told them (ws188-p004): the
 * file manager reads no mount table itself (Guardrail "app と設定"; the
 * user's decision of 2026-10-08), the compositor does (libkeiland's
 * kl_system_machine_mounts, asked by main.c).  main.c puts each answer
 * here (fm_mounts_set); Places and the Trash go through it with the
 * iterator of mounts.h, each over a copy taken when it starts.  Until the
 * first answer the list is empty (as a table that could not be read).
 *
 * Only the main thread uses it.
 */

#include "mounts.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * One walk of the mounts: its own copy of the list, how many, and the
 * next to give.
 */
struct fm_mounts {
	struct kl_machine_mount *list;
	size_t count;
	size_t at;
};

/*
 * The mounts of the desktop's last answer and how many; empty (count 0)
 * until the first answer.  The main thread's alone.
 */
static struct kl_machine_mount mounts_known[KL_MACHINE_MOUNTS_MAX];
static size_t mounts_known_count;

/*
 * Puts the desktop's last answer in place of the mounts known.
 */
void
fm_mounts_set(
	const struct kl_machine_mount *list,
	size_t count)
{
	/* As many as the table holds. */
	if (count > KL_MACHINE_MOUNTS_MAX)
		count = KL_MACHINE_MOUNTS_MAX;
	memcpy(mounts_known, list, count * sizeof(mounts_known[0]));
	mounts_known_count = count;
}

/*
 * Starts a walk over a copy of the mounts known.
 */
int
fm_mounts_open(
	struct fm_mounts **result)
{
	struct fm_mounts *mounts;

	/* A missing place for the walk. */
	if (result == NULL)
		return EINVAL;

	/* The walk. */
	mounts = calloc(1, sizeof(*mounts));
	if (mounts == NULL)
		return ENOMEM;

	/* Its copy of the list (none to copy when none is known). */
	if (mounts_known_count != 0U) {
		mounts->list = calloc(mounts_known_count, sizeof(mounts->list[0]));
		if (mounts->list == NULL) {
			free(mounts);
			return ENOMEM;
		}
		memcpy(mounts->list, mounts_known, mounts_known_count * sizeof(mounts->list[0]));
		mounts->count = mounts_known_count;
	}

	/* Succeeded: the caller owns the walk. */
	*result = mounts;
	return 0;
}

/*
 * Gives the next mount of the walk: 1 with it, 0 at the end, -1 (errno
 * EINVAL) for a missing walk or record.
 */
int
fm_mounts_next(
	struct fm_mounts *mounts,
	struct fm_mount *mount)
{
	/* A missing walk or record is a refusal. */
	if (mounts == NULL || mount == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* The end of the walk. */
	if (mounts->at == mounts->count)
		return 0;

	/* The next mount, its strings the walk's until it is closed. */
	mount->path = mounts->list[mounts->at].path;
	mount->type = mounts->list[mounts->at].type;
	mounts->at++;

	/* Succeeded: one mount. */
	return 1;
}

/*
 * Ends a walk.
 */
void
fm_mounts_close(
	struct fm_mounts *mounts)
{
	/* No walk. */
	if (mounts == NULL)
		return;

	/* Its copy and itself. */
	free(mounts->list);
	free(mounts);
}
