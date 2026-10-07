/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lock screen's unlocking moves and its grace (ws187-p002, the
 * 2026-10-08 user: "画面の下部（下端でなくてよい）から上にスワイプする
 * とロック解除でき、手動ロックされた場合を除き、ロックから一定時間なら
 * スワイプのみで認証不要 ... マウスホイールを上方向に回転でロック解除も
 * 実装").
 *
 * A swipe is a press in the lower third of the output (not only at its
 * edge) that goes up far enough, more up than across, before it lets go.
 * On a touch pad two fingers moving up far enough are one, once a touch.
 * The wheel turned up two notches in a row is one: a single notch, a
 * mouse merely knocked, does not open a recently locked session.
 *
 * A lock its user chose (Super+L, App Home's Lock Screen) always asks for
 * the secret; one the session made itself (the lid, sleep, idleness) opens
 * on a swipe alone for a while after it locked.  The while is measured on
 * the wall clock, which goes on while the machine sleeps; a clock that
 * went back gives no grace.
 */

#include "lock-swipe.h"

#include <stddef.h>
#include <string.h>

/* The lower part a swipe starts in: below this share of the height (percent). */
#define LOCK_SWIPE_LOWER	67

/* How far up a swipe must go: this share of the height (percent), and at least this far (pixels). */
#define LOCK_SWIPE_SHARE	12
#define LOCK_SWIPE_LEAST	80

/* The wheel's notches up that unlock, and the longest pause between two of them (milliseconds). */
#define LOCK_WHEEL_NOTCHES	2U
#define LOCK_WHEEL_PAUSE_MS	800U

/* Two fingers' travel up on a touch pad that unlocks (micrometres). */
#define LOCK_PAD_UM		10000

/*
 * The reasons of the locks the session makes itself (backend-host.c's lid,
 * sleep.c's sleep, an idle lock): every other reason is a lock the user
 * chose, or one not known here, and asks for the secret.
 */
static const char *const lock_automatic[] = {
	"lid",
	"sleep",
	"idle"
};

/* Forgets every move followed (the lock begins, or a move has unlocked). */
void
kwl_lock_swipe_reset(
	struct kwl_lock_swipe *swipe)
{
	/* Nothing pressed, turned or moved. */
	memset(swipe, 0, sizeof(*swipe));
}

/*
 * Takes a press at a place of an output of a height: one in the lower part
 * is followed.  Reports whether it is.
 */
int
kwl_lock_swipe_press(
	struct kwl_lock_swipe *swipe,
	int32_t x,
	int32_t y,
	int32_t height)
{
	/* A press above the lower part is no swipe. */
	swipe->pressing = 0U;
	if ((int64_t)y * 100 < (int64_t)height * LOCK_SWIPE_LOWER)
		return 0;

	/* Succeeded: followed from here, nothing travelled yet. */
	swipe->pressing = 1U;
	swipe->start_x = x;
	swipe->start_y = y;
	swipe->up = 0;
	swipe->across = 0;
	return 1;
}

/* Takes the pressed pointer's place: how far up and across it has gone from the press. */
void
kwl_lock_swipe_motion(
	struct kwl_lock_swipe *swipe,
	int32_t x,
	int32_t y)
{
	/* Only a press being followed. */
	if (!swipe->pressing)
		return;

	/* Up is towards the top; across either way. */
	swipe->up = swipe->start_y - y;
	swipe->across = x - swipe->start_x;
	if (swipe->across < 0)
		swipe->across = -swipe->across;
}

/*
 * Takes the press's release: reports whether it swiped (went up the
 * distance, more up than across).
 */
int
kwl_lock_swipe_release(
	struct kwl_lock_swipe *swipe,
	int32_t height)
{
	int32_t distance;

	/* Only a press being followed, and it is followed no more. */
	if (!swipe->pressing)
		return 0;
	swipe->pressing = 0U;

	/* Short of the distance. */
	distance = kwl_lock_swipe_distance(height);
	if (swipe->up < distance)
		return 0;

	/* More across than up is no swipe up. */
	if (swipe->across >= swipe->up)
		return 0;

	/* Succeeded: a swipe up. */
	return 1;
}

/* Gives how far up a swipe must go on an output of a height. */
int32_t
kwl_lock_swipe_distance(
	int32_t height)
{
	int32_t distance;

	/* A share of the height, at least the least. */
	distance = height * LOCK_SWIPE_SHARE / 100;
	if (distance < LOCK_SWIPE_LEAST)
		distance = LOCK_SWIPE_LEAST;

	/* Succeeded: the distance. */
	return distance;
}

/*
 * Takes the wheel's notches (positive down, as the seat counts them) at a
 * time: reports whether it has now turned up enough notches in a row.
 */
int
kwl_lock_swipe_wheel(
	struct kwl_lock_swipe *swipe,
	int32_t vertical,
	uint64_t now_ms)
{
	/* Down, or no vertical turn at all, starts the count again. */
	if (vertical >= 0) {
		swipe->wheel_notches = 0U;
		return 0;
	}

	/* A pause too long since the last notch starts the count again. */
	if (swipe->wheel_notches != 0U && now_ms - swipe->wheel_ms > LOCK_WHEEL_PAUSE_MS)
		swipe->wheel_notches = 0U;

	/* These notches up, now. */
	swipe->wheel_notches += (unsigned)(-vertical);
	swipe->wheel_ms = now_ms;

	/* Not enough yet. */
	if (swipe->wheel_notches < LOCK_WHEEL_NOTCHES)
		return 0;

	/* Succeeded: turned up enough; the next turn counts afresh. */
	swipe->wheel_notches = 0U;
	return 1;
}

/*
 * Takes two fingers' travel on a touch pad (micrometres, positive down, the
 * fingers' own way whatever the scrolling's direction): reports whether
 * they have now gone up far enough, once a touch.
 */
int
kwl_lock_swipe_pad(
	struct kwl_lock_swipe *swipe,
	int64_t down_um)
{
	/* A touch that unlocked already does nothing more. */
	if (swipe->pad_spent)
		return 0;

	/* Up adds, down takes away, never below nothing. */
	swipe->pad_up_um -= down_um;
	if (swipe->pad_up_um < 0)
		swipe->pad_up_um = 0;

	/* Not far enough yet. */
	if (swipe->pad_up_um < LOCK_PAD_UM)
		return 0;

	/* Succeeded: far enough; this touch is spent. */
	swipe->pad_spent = 1U;
	return 1;
}

/*
 * Takes the end of a touch pad's gesture up (two fingers from the bottom
 * edge, or three fingers up) with its travel along its way: reports
 * whether it went far enough, once a touch.
 */
int
kwl_lock_swipe_pad_gesture(
	struct kwl_lock_swipe *swipe,
	int64_t travel_um)
{
	/* A touch that unlocked already does nothing more. */
	if (swipe->pad_spent)
		return 0;

	/* Not far enough. */
	if (travel_um < LOCK_PAD_UM)
		return 0;

	/* Succeeded: far enough; this touch is spent. */
	swipe->pad_spent = 1U;
	return 1;
}

/* Takes the end of a touch on the pad: the next touch counts afresh. */
void
kwl_lock_swipe_pad_end(
	struct kwl_lock_swipe *swipe)
{
	/* No travel, and not spent. */
	swipe->pad_up_um = 0;
	swipe->pad_spent = 0U;
}

/*
 * Reports whether a lock's reason is one the user chose (or one not known
 * here), which always asks for the secret, rather than one the session made
 * itself.
 */
int
kwl_lock_reason_manual(
	const char *reason)
{
	size_t index;
	int differs;

	/* No reason given is not known. */
	if (reason == NULL)
		return 1;

	/* Each reason of the session's own locks. */
	for (index = 0; index < sizeof(lock_automatic) / sizeof(lock_automatic[0]); index++) {
		differs = strcmp(reason, lock_automatic[index]);
		if (differs == 0)
			return 0;
	}

	/* Succeeded: a lock the user chose, or one not known. */
	return 1;
}

/*
 * Reports whether a lock made at locked_at (wall clock seconds) is still in
 * its grace at now: a lock the session made itself, at most grace seconds
 * ago.
 */
int
kwl_lock_grace(
	unsigned manual,
	int64_t locked_at,
	int64_t now,
	int64_t grace)
{
	/* A lock the user chose has no grace. */
	if (manual)
		return 0;

	/* A clock that went back gives none either. */
	if (now < locked_at)
		return 0;

	/* Past the grace. */
	if (now - locked_at > grace)
		return 0;

	/* Succeeded: within the grace. */
	return 1;
}
