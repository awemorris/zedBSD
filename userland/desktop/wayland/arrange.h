/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arrangement of windows (arrange.c, WS181, the 2026-10-07 UAT): the
 * seven layouts of the arrangement menu (side by side, stacked, one on the
 * left, one on the right, one on the top, one on the bottom, a grid), the
 * slots each makes of the work area
 * for a number of windows, which window goes to which slot (the one
 * nearest to where it is, the person's "roughly from where they are"), and
 * where a window of limited size goes in its slot (ws177-p035).
 *
 * It knows nothing of the server: the caller gives the area and the
 * windows' centres, and places the windows (arrange-shell.c).  So the host
 * tests run it alone.
 */

#ifndef KWL_ARRANGE_H
#define KWL_ARRANGE_H

#include <stdint.h>

/* The layouts, in the menu's order (its rows of two, ws181-p006: the grid alone on the last). */
#define KWL_ARRANGE_COLUMNS		0U
#define KWL_ARRANGE_ROWS		1U
#define KWL_ARRANGE_LEFT_MAIN		2U
#define KWL_ARRANGE_RIGHT_MAIN		3U
#define KWL_ARRANGE_TOP_MAIN		4U
#define KWL_ARRANGE_BOTTOM_MAIN		5U
#define KWL_ARRANGE_GRID		6U
#define KWL_ARRANGE_LAYOUTS		7U

/* The most windows a layout takes (the grid's), and the margin and gap around and between its slots. */
#define KWL_ARRANGE_MAX			9U
#define KWL_ARRANGE_MARGIN		8
#define KWL_ARRANGE_GAP			8

/* A rectangle of the output: a slot (a window's whole frame, its title bar included) or the work area. */
struct kwl_arrange_rect {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
};

const char *kwl_arrange_name(unsigned layout);
unsigned kwl_arrange_limit(unsigned layout);
unsigned kwl_arrange_slots(unsigned layout, unsigned count, const struct kwl_arrange_rect *area, struct kwl_arrange_rect *slots);
void kwl_arrange_assign(const int32_t centres[][2], unsigned count, const struct kwl_arrange_rect *slots, unsigned *order);
int kwl_arrange_fit(const struct kwl_arrange_rect *body, const int32_t limits[4], struct kwl_arrange_rect *placed);

#endif
