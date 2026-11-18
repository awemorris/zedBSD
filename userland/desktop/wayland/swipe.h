/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One swipe of two fingers on the touch pad as one step (swipe.c,
 * ws142-p009, BUG-215 and BUG-216): while Wiseview or the switcher shows,
 * the two fingers' scrolling is not the client's; from the fingers landing
 * to their lifting it is one swipe, which becomes one step left or right,
 * or a swipe down or up, once its travel passes KWL_SWIPE_STEP_UM, and
 * nothing more until the fingers lift.
 *
 * It knows nothing of the server: the caller hands it the fingers' travel
 * (micrometres, the way the fingers went) and the swipe's end, and acts on
 * what it decides (shell.c, switcher-shell.c).  So the host tests run it
 * alone.
 */

#ifndef KWL_SWIPE_H
#define KWL_SWIPE_H

#include <stdint.h>

/* The travel that decides a swipe (micrometres; the edges' gestures need as much, p007). */
#define KWL_SWIPE_STEP_UM	8000

/* What a swipe decided: nothing yet, a step to the left or the right, down or up. */
#define KWL_SWIPE_NONE		0U
#define KWL_SWIPE_LEFT		1U
#define KWL_SWIPE_RIGHT		2U
#define KWL_SWIPE_DOWN		3U
#define KWL_SWIPE_UP		4U

/*
 * A swipe under way: the fingers' travel across and down since it began
 * (micrometres, right and down positive), and whether it has decided (it
 * then decides nothing more until it ends).
 */
struct kwl_swipe {
	int32_t across_um;
	int32_t down_um;
	unsigned decided;
};

unsigned kwl_swipe_take(struct kwl_swipe *swipe, int32_t across_um, int32_t down_um);
void kwl_swipe_end(struct kwl_swipe *swipe);
const char *kwl_swipe_name(unsigned direction);

#endif
