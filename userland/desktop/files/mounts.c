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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How old the mounts known may be before a walk asks for a new reading, in milliseconds. */
#define MOUNTS_FRESH_MS		2000U

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
 * When the mounts known came (CLOCK_MONOTONIC, ms; 0: none yet), and
 * whether a walk found them older than MOUNTS_FRESH_MS and wants a new
 * reading (main.c takes it with fm_mounts_wanted).  So a mount made
 * elsewhere (a terminal, the network) shows the next time Places or the
 * Trash is gone through, and the walks the answer itself starts ask for
 * nothing.
 */
static uint64_t mounts_known_ms;
static int mounts_wanted;

static uint64_t mounts_now(void);

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
	mounts_known_ms = mounts_now();
}

/*
 * Tells whether a walk wants a new reading of the mounts, once.
 */
int
fm_mounts_wanted(
	void)
{
	int wanted;

	/* Taken: asked once. */
	wanted = mounts_wanted;
	mounts_wanted = 0;

	/* Succeeded: whether a reading is wanted. */
	return wanted;
}

/*
 * Starts a walk over a copy of the mounts known.
 */
int
fm_mounts_open(
	struct fm_mounts **result)
{
	struct fm_mounts *mounts;
	uint64_t now;

	/* A missing place for the walk. */
	if (result == NULL)
		return EINVAL;

	/* Mounts never told, or told a while ago, are asked for again. */
	now = mounts_now();
	if (mounts_known_ms == 0U || now - mounts_known_ms >= MOUNTS_FRESH_MS)
		mounts_wanted = 1;

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

/* Reads the monotonic clock in milliseconds (0 when it cannot be read). */
static uint64_t
mounts_now(
	void)
{
	struct timespec now;
	int error;

	/* A clock that fails reads as zero. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: the time. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
