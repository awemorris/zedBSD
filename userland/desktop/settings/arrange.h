/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Display page's arrangement (arrange.c, ws113-p006): the displays'
 * rectangles of the desktop's logical plane drawn scaled into a box, and
 * a display dragged to a new place that snaps to the others: next to one
 * of them, sharing a length of its edge, overlapping none, and level with
 * its top or bottom (or left or right) when it comes near.  It knows
 * nothing of the page, so the host tests run it alone.
 */

#ifndef SE_ARRANGE_H
#define SE_ARRANGE_H

#include <stdint.h>

/* The most displays an arrangement holds. */
#define SE_ARRANGE_MAX		8U

/* A display's rectangle of the plane. */
struct se_arrange_rect {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
};

/*
 * The plane drawn into a box: a box's point (bx, by) is the plane's
 * (origin_x + (bx - box_x) / scale, origin_y + (by - box_y) / scale).
 */
struct se_arrange_view {
	int box_x;
	int box_y;
	int box_width;
	int box_height;
	float scale;
	float origin_x;
	float origin_y;
};

void se_arrange_fit(const struct se_arrange_rect *rects, unsigned count, int box_x, int box_y, int box_width, int box_height, struct se_arrange_view *view);
void se_arrange_to_box(const struct se_arrange_view *view, const struct se_arrange_rect *rect, int *x, int *y, int *width, int *height);
void se_arrange_from_box(const struct se_arrange_view *view, int x, int y, int32_t *plane_x, int32_t *plane_y);
void se_arrange_snap(const struct se_arrange_rect *rects, unsigned count, unsigned moving, int32_t want_x, int32_t want_y, int32_t *x, int32_t *y);

#endif
