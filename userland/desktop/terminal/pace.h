/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pacing of the terminal's frames, as libkeiland paces the frames of
 * a window that asks for KL_PACING_FRAME (ws210-p014 F2.4): a frame is
 * drawn only once the compositor has taken the one before (its frame
 * callback came), so that no more than one frame goes ahead of the
 * compositor.  The changes made meanwhile (a selection dragged, a shell's
 * output) are not drawn one by one; the next frame draws the latest state.
 *
 * The terminal shows its frames itself with Vulkan (KL_PRESENT_NONE), so
 * it cannot ask libkeiland for the pacing; the window asks for the frame
 * callbacks (window.c) and this file keeps the state.  Nothing here speaks
 * Wayland.
 *
 * The pointer's, the fingers', the pen's and a drop's moves, the wheel and
 * the shells' output wait for the compositor; any other input (a key, a
 * button, a menu's choice, a resize) has its frame drawn at once, once.
 * A compositor that does not answer for TERMINAL_PACE_WATCHDOG_MS is drawn
 * for anyway, so that a callback lost (a frame the window system did not
 * commit) never stops the drawing.
 */

#ifndef TERMINAL_PACE_H
#define TERMINAL_PACE_H

#include <stdint.h>

#include <keiland/keiland.h>

/* How long the compositor's frame callback is waited for before a frame is drawn anyway, in milliseconds. */
#define TERMINAL_PACE_WATCHDOG_MS	1000U

/*
 * The pacing of the window's frames: a frame callback asked for with the
 * frame last shown and not yet answered (asked, at asked_ms), a frame
 * shown again while it was (overdue: the next waits for the answer), and
 * an input since the last frame that does not wait (urgent).
 */
struct terminal_pace {
	int asked;
	int overdue;
	int urgent;
	uint64_t asked_ms;
};

/*
 * Tells whether a window input may wait for the compositor's next frame
 * (1), or asks for a frame at once (0): the moves and the wheel, whose
 * latest state stands for them all, wait; every other kind does not.
 */
static __inline int
terminal_pace_waits(
	unsigned kind)
{
	/* The moves and the wheel. */
	switch (kind) {
	case KL_WINDOW_MOTION:
	case KL_WINDOW_LEAVE:
	case KL_WINDOW_AXIS:
	case KL_WINDOW_TOUCH_MOTION:
	case KL_WINDOW_DROP_MOTION:
	case KL_WINDOW_DROP_ACTION:
	case KL_WINDOW_TABLET_MOTION:
	case KL_WINDOW_TABLET_HOVER:
		return 1;
	default:
		break;
	}

	/* Reports an input that asks for its frame at once. */
	return 0;
}

/* Notes a window input: one that does not wait has the next frame drawn at once. */
static __inline void
terminal_pace_input(
	struct terminal_pace *pace,
	unsigned kind)
{
	int waits;

	/* The moves and the wheel wait for the compositor. */
	waits = terminal_pace_waits(kind);
	if (!waits)
		pace->urgent = 1;
}

/*
 * Tells when the next frame may be drawn, changing nothing: 0 now, or a
 * positive number of milliseconds (rounded up) after which it is drawn
 * even without the compositor's answer (which wakes the wait sooner).
 */
static __inline int
terminal_pace_wait(
	const struct terminal_pace *pace,
	uint64_t now_ms)
{
	uint64_t elapsed;

	/* The compositor took the last frame: the next may be drawn. */
	if (!pace->asked)
		return 0;

	/* A compositor that has not answered for long is drawn for anyway. */
	elapsed = 0U;
	if (now_ms > pace->asked_ms)
		elapsed = now_ms - pace->asked_ms;
	if (elapsed >= TERMINAL_PACE_WATCHDOG_MS)
		return 0;

	/* An input that does not wait is drawn at once, unless a frame already went ahead of the answer. */
	if (pace->urgent && !pace->overdue)
		return 0;

	/* Reports the milliseconds until the answer is waited for no longer. */
	return (int)(TERMINAL_PACE_WATCHDOG_MS - elapsed);
}

/*
 * Tells whether the callback awaited is to be given up before the next
 * frame asks for its own (1): it went unanswered past the watchdog.
 */
static __inline int
terminal_pace_stale(
	const struct terminal_pace *pace,
	uint64_t now_ms)
{
	/* Nothing awaited. */
	if (!pace->asked)
		return 0;

	/* Unanswered for too long: the commit it rode on may never be shown. */
	if (now_ms >= pace->asked_ms + TERMINAL_PACE_WATCHDOG_MS)
		return 1;

	/* Reports a callback still worth waiting for. */
	return 0;
}

/* Notes the frame callback asked for at now_ms with the frame about to be shown. */
static __inline void
terminal_pace_ask(
	struct terminal_pace *pace,
	uint64_t now_ms)
{
	/* Awaited from now, nothing shown ahead of it yet. */
	pace->asked = 1;
	pace->overdue = 0;
	pace->asked_ms = now_ms;
}

/*
 * Follows a frame's showing: asked says whether its callback was asked
 * for with it (terminal_pace_ask), error whether it was shown (0).
 * Returns 1 when that callback is to be forgotten (the frame was not
 * shown, so it would wait for a later commit), else 0.  A frame shown
 * while an earlier one's callback is awaited makes the next frame wait for
 * that callback.
 */
static __inline int
terminal_pace_shown(
	struct terminal_pace *pace,
	int asked,
	int error)
{
	/* Not shown: the input that asked for the frame still does. */
	if (error != 0) {
		if (asked)
			return 1;
		return 0;
	}

	/* Shown: what did not wait has its frame. */
	pace->urgent = 0;

	/* Shown ahead of the frame awaited. */
	if (!asked && pace->asked)
		pace->overdue = 1;
	return 0;
}

/* Notes that no frame callback is awaited any more (answered, or given up): the next frame may be drawn at once. */
static __inline void
terminal_pace_clear(
	struct terminal_pace *pace)
{
	/* Nothing awaited. */
	pace->asked = 0;
	pace->overdue = 0;
}

#endif
