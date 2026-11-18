/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of the game mode (scanout-rules.h, ws122-p005b): a fullscreen
 * video or game, alone on the output, goes straight to the display.
 */

#include "scanout-rules.h"

#include <stddef.h>

/* The answers' words, by their number. */
static const char *const scanout_reason_names[] = {
	"direct", "no-window", "not-fullscreen", "content", "buffer", "size", "overlay", "shot", "pointer", "refused", "rotated"
};

/*
 * Decides one pass: the first reason the image is not shown straight, or
 * KWL_SCANOUT_DIRECT.
 */
unsigned
kwl_scanout_decide(
	const struct kwl_scanout_facts *facts)
{
	/* A fullscreen window on top. */
	if (!facts->window)
		return KWL_SCANOUT_NO_WINDOW;
	if (!facts->fullscreen)
		return KWL_SCANOUT_NOT_FULLSCREEN;

	/* That asks for it: a video or a game (the user's decision: only what the application asks for). */
	if (facts->content_type != KWL_SCANOUT_CONTENT_VIDEO && facts->content_type != KWL_SCANOUT_CONTENT_GAME)
		return KWL_SCANOUT_CONTENT;

	/* Its image a GPU buffer. */
	if (!facts->gpu_buffer)
		return KWL_SCANOUT_BUFFER;

	/*
	 * A rotated output is composed (WS217): the backend's claim shows the
	 * image unturned, and the turn's game pause comes later (ws217-p012).
	 */
	if (facts->scanout_rotation != 0U)
		return KWL_SCANOUT_ROTATED;
	if (facts->turn_rotation != 0U)
		return KWL_SCANOUT_ROTATED;

	/* Of the output's size. */
	if (!facts->size_matches)
		return KWL_SCANOUT_SIZE;

	/* Nothing over it, no screenshot waiting for a composed frame. */
	if (facts->overlay)
		return KWL_SCANOUT_OVERLAY;
	if (facts->shot)
		return KWL_SCANOUT_SHOT;

	/*
	 * The pointer still (a moving one is drawn over a video), and a
	 * display that has not refused it.  Over a game no cursor is shown on
	 * any platform, so its moving pointer, which a game moves all the
	 * time, keeps it straight.
	 */
	if (facts->content_type != KWL_SCANOUT_CONTENT_GAME &&
	    facts->pointer_still_ms < KWL_SCANOUT_POINTER_IDLE_MS)
		return KWL_SCANOUT_POINTER;
	if (facts->refused)
		return KWL_SCANOUT_REFUSED;

	/* Succeeded: straight to the display. */
	return KWL_SCANOUT_DIRECT;
}

/*
 * Names an answer for the log.
 */
const char *
kwl_scanout_reason_name(
	unsigned reason)
{
	/* One of the answers. */
	if (reason < sizeof(scanout_reason_names) / sizeof(scanout_reason_names[0]))
		return scanout_reason_names[reason];

	/* Any other. */
	return "unknown";
}
