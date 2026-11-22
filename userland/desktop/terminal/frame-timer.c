/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pacing of terminal's frames by the clock (pace.h), for a window
 * system that gives the program no frame callback: a frame shown makes
 * the next wait FRAME_TIMER_MS, unless an input that does not wait (a key,
 * a button) asks for its frame at once.
 */

#include "terminal.h"

/* How long a frame shown holds the next back, in milliseconds (about 60 frames a second). */
#define FRAME_TIMER_MS		16U

/*
 * Tells when the next frame may be drawn, changing nothing: 0 now, or the
 * milliseconds until the last frame's time is up.
 */
int
terminal_window_frame_wait(
	const struct terminal_window *window,
	uint64_t now)
{
	uint64_t elapsed;

	/* No frame holds the next back. */
	if (!window->pace.asked)
		return 0;

	/* The last frame's time is up. */
	elapsed = 0U;
	if (now > window->pace.asked_ms)
		elapsed = now - window->pace.asked_ms;
	if (elapsed >= FRAME_TIMER_MS)
		return 0;

	/* An input that does not wait is drawn at once. */
	if (window->pace.urgent)
		return 0;

	/* Reports the milliseconds until the last frame's time is up. */
	return (int)(FRAME_TIMER_MS - elapsed);
}

/*
 * Notes the frame about to be shown, whose time holds the next back;
 * returns 1 (it is always noted).
 */
int
terminal_window_frame_ask(
	struct terminal_window *window,
	uint64_t now)
{
	/* The frame's time starts now. */
	terminal_pace_ask(&window->pace, now);

	/* Succeeded: noted. */
	return 1;
}

/*
 * Follows a frame's showing: a frame shown has drawn what did not wait,
 * and a frame not shown holds nothing back.
 */
void
terminal_window_frame_shown(
	struct terminal_window *window,
	int asked,
	int error)
{
	int forget;

	/* The pace after the frame; a frame not shown forgets its time. */
	forget = terminal_pace_shown(&window->pace, asked, error);
	if (forget)
		terminal_window_frame_forget(window);
}

/*
 * Stops holding the next frame back: it may be drawn at once.
 */
void
terminal_window_frame_forget(
	struct terminal_window *window)
{
	/* Nothing awaited. */
	terminal_pace_clear(&window->pace);
}
