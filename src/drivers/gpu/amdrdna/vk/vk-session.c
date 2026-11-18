/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's sessions: one for each open of the node, with its objects and
 * the scratch its commands decode into.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk.h"

/*
 * Opens an executor session for an owner (the node's session).
 */
int
amdrdna_vk_session_open(
	void *owner,
	struct amdrdna_vk_session **session)
{
	struct amdrdna_vk_session *created;

	*session = NULL;

	/* Allocates the session. */
	created = kern_calloc(1U, sizeof(*created));
	if (created == NULL)
		return ENOMEM;

	/* Allocates its scratch. */
	created->arena.base = kern_calloc(1U, AMDRDNA_VK_ARENA_BYTES);
	if (created->arena.base == NULL) {
		kern_free(created);
		return ENOMEM;
	}

	/* No object yet; the scratch's size and the owner. */
	created->arena.size = AMDRDNA_VK_ARENA_BYTES;
	created->owner = owner;

	/* Succeeded: the session is the caller's. */
	*session = created;
	return 0;
}

/*
 * Closes an executor session: removes its objects and frees it.
 *
 * The caller has ended every command of the session first.  When payloads
 * are still held elsewhere the session stays (EBUSY); a destructor's error
 * is reported after the session is freed.
 */
int
amdrdna_vk_session_close(
	struct amdrdna_vk_session **session)
{
	struct amdrdna_vk_session *closing;
	int error;

	/* A session never opened has nothing to close. */
	closing = *session;
	if (closing == NULL)
		return 0;

	/* Removes every object, then gives back the submissions' run. */
	error = amdrdna_vk_objects_close(closing);
	amdrdna_vk_execute_close(closing);
	if (closing->live_objects != 0U) {
		/* Payloads still held keep the session for their destructors. */
		if (error != 0)
			return error;
		return EBUSY;
	}

	/* Frees the scratch and the session. */
	kern_free(closing->arena.base);
	kern_free(closing);
	*session = NULL;

	/* Reports a destructor that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the session is gone. */
	return 0;
}

/*
 * Takes a cleared run of the session's scratch for a record the current
 * command replies with, aligned to 8 bytes; it lives until the command
 * returns.  Returns NULL when the scratch is full.
 */
void *
amdrdna_vk_arena_take(
	struct amdrdna_vk_session *session,
	size_t bytes)
{
	struct i915_wire_arena *arena;
	size_t start;
	size_t length;

	arena = &session->arena;

	/* The run and its start rounded to 8 bytes. */
	length = (bytes + 7U) & ~(size_t)7U;
	start = (arena->used + 7U) & ~(size_t)7U;

	/* The run must fit behind the start. */
	if (start > arena->size || length > arena->size - start)
		return NULL;

	/* Takes it and clears it. */
	arena->used = start + length;
	kern_memset(arena->base + start, 0, length);

	/* Succeeded: the run is the command's. */
	return arena->base + start;
}
