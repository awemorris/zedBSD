/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of the game mode (scanout-rules.c, ws122-p005b): when a
 * fullscreen window's image is shown without composing.  The caller
 * (scanout.c) gathers the facts of a pass; the rules say whether the image
 * goes straight to the display, or the first reason it does not.  It knows
 * nothing of the server, so the host tests run it alone.
 */

#ifndef KWL_SCANOUT_RULES_H
#define KWL_SCANOUT_RULES_H

#include <stdint.h>

/* How long the pointer stays still before the game mode comes back (milliseconds; a moving pointer over a video is drawn composed). */
#define KWL_SCANOUT_POINTER_IDLE_MS	2000U

/* The content types the game mode takes (wp_content_type_v1's video and game). */
#define KWL_SCANOUT_CONTENT_VIDEO	2U
#define KWL_SCANOUT_CONTENT_GAME	3U

/* The answers: straight to the display, or why not (the first reason in this order). */
#define KWL_SCANOUT_DIRECT		0U
#define KWL_SCANOUT_NO_WINDOW		1U
#define KWL_SCANOUT_NOT_FULLSCREEN	2U
#define KWL_SCANOUT_CONTENT		3U
#define KWL_SCANOUT_BUFFER		4U
#define KWL_SCANOUT_SIZE		5U
#define KWL_SCANOUT_OVERLAY		6U
#define KWL_SCANOUT_SHOT		7U
#define KWL_SCANOUT_POINTER		8U
#define KWL_SCANOUT_REFUSED		9U
#define KWL_SCANOUT_ROTATED		10U

/*
 * The facts of one pass: whether there is a window on top, it is
 * fullscreen, its content type, its image is a GPU buffer (not shared
 * memory, a client's own), the output's rotation (WS217: the display
 * hardware's 180, the compositor's turn of 90 or 270), the image is of the
 * output's size, something is drawn over it (an overlay, a popup, a menu,
 * the system bar, an animation, the lock), a screenshot waits for a
 * composed frame, how long the pointer has been still, and whether the
 * display already refused this window.
 */
struct kwl_scanout_facts {
	unsigned window;
	unsigned fullscreen;
	uint32_t content_type;
	unsigned gpu_buffer;
	unsigned scanout_rotation;
	unsigned turn_rotation;
	unsigned size_matches;
	unsigned overlay;
	unsigned shot;
	uint64_t pointer_still_ms;
	unsigned refused;
};

/* The answer for a pass's facts (KWL_SCANOUT_*). */
unsigned kwl_scanout_decide(const struct kwl_scanout_facts *facts);

/* The answer's word for the log ("direct", "overlay", ...). */
const char *kwl_scanout_reason_name(unsigned reason);

#endif
