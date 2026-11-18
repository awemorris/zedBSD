/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the lid does (ws132-p008; lid.h says what and why).  A lid that
 * reports the same state twice changes nothing the second time.
 */

#include "lid.h"

/*
 * Takes the lid's closing: the screen goes out, and a session (not the
 * login screen) not yet locked is locked by it.  Returns the actions.
 */
unsigned
kwl_lid_close(
	struct kwl_lid *lid,
	uint64_t now_ms,
	int session,
	int locked)
{
	/* Already closed: nothing. */
	if (lid->closed)
		return 0;

	/* Closed now; a lock already standing is not the lid's. */
	lid->closed = 1;
	lid->closed_ms = now_ms;
	lid->lock_is_lid = 0;
	if (!session || locked)
		return KWL_LID_SCREEN_OFF;

	/* Succeeded: out, and locked by the lid. */
	lid->lock_is_lid = 1;
	return KWL_LID_SCREEN_OFF | KWL_LID_LOCK;
}

/*
 * Takes the lid's opening: the screen lights, and the closing's own lock,
 * still standing, goes without the password when the lid opens within
 * KWL_LID_GRACE_MS of the closing.  Returns the actions.
 */
unsigned
kwl_lid_open(
	struct kwl_lid *lid,
	uint64_t now_ms,
	int locked)
{
	uint64_t elapsed;
	unsigned lock_is_lid;

	/* Already open: nothing. */
	if (!lid->closed)
		return 0;

	/* Open now; the lid's lock is forgotten whatever happens to it. */
	lid->closed = 0;
	lock_is_lid = lid->lock_is_lid;
	lid->lock_is_lid = 0;

	/* Another lock, or none standing: only the light. */
	if (!lock_is_lid || !locked)
		return KWL_LID_SCREEN_ON;

	/* Too late (or a clock that went back): the password is asked. */
	if (now_ms < lid->closed_ms)
		return KWL_LID_SCREEN_ON;
	elapsed = now_ms - lid->closed_ms;
	if (elapsed > KWL_LID_GRACE_MS)
		return KWL_LID_SCREEN_ON;

	/* Succeeded: lit and unlocked. */
	return KWL_LID_SCREEN_ON | KWL_LID_UNLOCK;
}

/*
 * Forgets the lid's lock when the session could not be locked (no session
 * manager to unlock it).
 */
void
kwl_lid_lock_failed(
	struct kwl_lid *lid)
{
	/* No lock is the lid's. */
	lid->lock_is_lid = 0;
}

/*
 * Forgets the lid's lock when the session was unlocked another way (the
 * password on the lock screen).
 */
void
kwl_lid_unlocked(
	struct kwl_lid *lid)
{
	/* A later lock is not the lid's. */
	lid->lock_is_lid = 0;
}
