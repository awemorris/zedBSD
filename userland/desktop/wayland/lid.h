/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the lid does (lid.c, ws132-p008; the 2026-10-05 user decision D2 of
 * plan/ws132/phase001, until the machine can sleep in S0i3, WS052): closing
 * it puts the screen out and locks the session; opening it within
 * KWL_LID_GRACE_MS of the closing lights the screen and unlocks the session
 * without the password, but only a lock the closing made, still standing.
 * A later opening leaves the lock screen up.
 *
 * It knows nothing of the server: the caller hands it the lid's changes
 * with the time and whether a session is locked, and carries out the
 * actions it gives (backend-host.c).  So the host tests run it alone.
 */

#ifndef KWL_LID_H
#define KWL_LID_H

#include <stdint.h>

/* How long after the closing an opening unlocks without the password (milliseconds; D2: 15 minutes). */
#define KWL_LID_GRACE_MS	(15U * 60U * 1000U)

/* The actions a change asks for, as bits. */
#define KWL_LID_SCREEN_OFF	0x1U
#define KWL_LID_LOCK		0x2U
#define KWL_LID_SCREEN_ON	0x4U
#define KWL_LID_UNLOCK		0x8U

/* The lid as the compositor follows it: whether it is closed, when it closed, and whether the lock standing is the closing's own. */
struct kwl_lid {
	unsigned closed;
	uint64_t closed_ms;
	unsigned lock_is_lid;
};

unsigned kwl_lid_close(struct kwl_lid *lid, uint64_t now_ms, int session, int locked);
unsigned kwl_lid_open(struct kwl_lid *lid, uint64_t now_ms, int locked);
void kwl_lid_lock_failed(struct kwl_lid *lid);
void kwl_lid_unlocked(struct kwl_lid *lid);

#endif
