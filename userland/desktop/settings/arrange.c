/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Display page's arrangement (ws113-p006; arrange.h says what and
 * why).
 */

#include "arrange.h"

#include <stddef.h>

/* The share of the drawing's room the displays fill, the rest left around them for a drag. */
#define ARRANGE_FILL		0.7f

/* How near (a share of the moving display's size) an edge comes before the display is made level with it. */
#define ARRANGE_LEVEL		8

static int arrange_overlaps(const struct se_arrange_rect *rects, unsigned count, unsigned moving, int32_t x, int32_t y);
static int32_t arrange_clamp(int64_t value, int64_t low, int64_t high);
static int32_t arrange_level(int32_t at, uint32_t size, int32_t other, uint32_t other_size, uint32_t near);
static void arrange_try(const struct se_arrange_rect *rects, unsigned count, unsigned moving, int32_t x, int32_t y, int32_t want_x, int32_t want_y, int64_t *best, int32_t *best_x, int32_t *best_y);

/*
 * Fits the displays into a box: scaled so that they fill ARRANGE_FILL of
 * it, centred.  Without a display the scale is one, the origin the box's.
 */
void
se_arrange_fit(
	const struct se_arrange_rect *rects,
	unsigned count,
	int box_x,
	int box_y,
	int box_width,
	int box_height,
	struct se_arrange_view *view)
{
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;
	float scale_x;
	float scale_y;
	unsigned index;

	/* The box, and the mapping of no display. */
	view->box_x = box_x;
	view->box_y = box_y;
	view->box_width = box_width;
	view->box_height = box_height;
	view->scale = 1.0f;
	view->origin_x = 0.0f;
	view->origin_y = 0.0f;
	if (count == 0U || box_width <= 0 || box_height <= 0)
		return;

	/* The rectangle around every display. */
	left = rects[0].x;
	top = rects[0].y;
	right = (int64_t)rects[0].x + rects[0].width;
	bottom = (int64_t)rects[0].y + rects[0].height;
	for (index = 1U; index < count; index++) {
		if (rects[index].x < left)
			left = rects[index].x;
		if (rects[index].y < top)
			top = rects[index].y;
		if ((int64_t)rects[index].x + rects[index].width > right)
			right = (int64_t)rects[index].x + rects[index].width;
		if ((int64_t)rects[index].y + rects[index].height > bottom)
			bottom = (int64_t)rects[index].y + rects[index].height;
	}

	/* The scale at which it fills the share of the box, the smaller of the two ways. */
	if (right <= left || bottom <= top)
		return;
	scale_x = (float)box_width * ARRANGE_FILL / (float)(right - left);
	scale_y = (float)box_height * ARRANGE_FILL / (float)(bottom - top);
	view->scale = scale_x;
	if (scale_y < scale_x)
		view->scale = scale_y;

	/* Centred: the box's corner is that far before the rectangle's. */
	view->origin_x = (float)left - ((float)box_width / view->scale - (float)(right - left)) / 2.0f;
	view->origin_y = (float)top - ((float)box_height / view->scale - (float)(bottom - top)) / 2.0f;
}

/* Gives a display's rectangle in the box. */
void
se_arrange_to_box(
	const struct se_arrange_view *view,
	const struct se_arrange_rect *rect,
	int *x,
	int *y,
	int *width,
	int *height)
{
	/* Scaled from the origin. */
	*x = view->box_x + (int)(((float)rect->x - view->origin_x) * view->scale + 0.5f);
	*y = view->box_y + (int)(((float)rect->y - view->origin_y) * view->scale + 0.5f);
	*width = (int)((float)rect->width * view->scale + 0.5f);
	*height = (int)((float)rect->height * view->scale + 0.5f);
}

/* Gives the plane's point under a point of the box. */
void
se_arrange_from_box(
	const struct se_arrange_view *view,
	int x,
	int y,
	int32_t *plane_x,
	int32_t *plane_y)
{
	float scale;

	/* Back through the scale (one without a mapping). */
	scale = view->scale;
	if (scale <= 0.0f)
		scale = 1.0f;
	*plane_x = (int32_t)(view->origin_x + (float)(x - view->box_x) / scale);
	*plane_y = (int32_t)(view->origin_y + (float)(y - view->box_y) / scale);
}

/*
 * Snaps a display (rects[moving], wanted at want_x, want_y) to the place
 * nearest the wanted one that is next to another display: right or left
 * of it with their rows sharing a length, or below or above it with their
 * columns sharing one, overlapping no display, and level with the other's
 * edge when within an eighth of its size.  Alone, it stays where it is.
 */
void
se_arrange_snap(
	const struct se_arrange_rect *rects,
	unsigned count,
	unsigned moving,
	int32_t want_x,
	int32_t want_y,
	int32_t *x,
	int32_t *y)
{
	const struct se_arrange_rect *self;
	const struct se_arrange_rect *other;
	int64_t best;
	int32_t side_x;
	int32_t side_y;
	unsigned index;

	/* Without another display there is nothing to snap to. */
	self = &rects[moving];
	*x = self->x;
	*y = self->y;
	best = -1;
	if (count < 2U || moving >= count)
		return;

	/* Each other display's four sides. */
	for (index = 0U; index < count; index++) {
		if (index == moving)
			continue;
		other = &rects[index];

		/* Right and left of it: rows that share a length, level with its top or bottom when near. */
		side_y = arrange_clamp(want_y, (int64_t)other->y - self->height + 1, (int64_t)other->y + other->height - 1);
		side_y = arrange_level(side_y, self->height, other->y, other->height, self->height / ARRANGE_LEVEL);
		arrange_try(rects, count, moving, (int32_t)((int64_t)other->x + other->width), side_y, want_x, want_y, &best, x, y);
		arrange_try(rects, count, moving, (int32_t)((int64_t)other->x - self->width), side_y, want_x, want_y, &best, x, y);

		/* Below and above it: columns that share a length, level with its left or right when near. */
		side_x = arrange_clamp(want_x, (int64_t)other->x - self->width + 1, (int64_t)other->x + other->width - 1);
		side_x = arrange_level(side_x, self->width, other->x, other->width, self->width / ARRANGE_LEVEL);
		arrange_try(rects, count, moving, side_x, (int32_t)((int64_t)other->y + other->height), want_x, want_y, &best, x, y);
		arrange_try(rects, count, moving, side_x, (int32_t)((int64_t)other->y - self->height), want_x, want_y, &best, x, y);
	}
}

/* Tells whether the moving display at (x, y) would overlap another. */
static int
arrange_overlaps(
	const struct se_arrange_rect *rects,
	unsigned count,
	unsigned moving,
	int32_t x,
	int32_t y)
{
	const struct se_arrange_rect *self;
	const struct se_arrange_rect *other;
	unsigned index;

	/* Each other display. */
	self = &rects[moving];
	for (index = 0U; index < count; index++) {
		if (index == moving)
			continue;
		other = &rects[index];

		/* Apart along x, or along y. */
		if ((int64_t)x + self->width <= other->x || (int64_t)other->x + other->width <= x)
			continue;
		if ((int64_t)y + self->height <= other->y || (int64_t)other->y + other->height <= y)
			continue;

		/* They overlap. */
		return 1;
	}

	/* None overlaps. */
	return 0;
}

/* Keeps a value within [low, high]. */
static int32_t
arrange_clamp(
	int64_t value,
	int64_t low,
	int64_t high)
{
	/* Below, above, or within. */
	if (value < low)
		return (int32_t)low;
	if (value > high)
		return (int32_t)high;
	return (int32_t)value;
}

/* Moves a coordinate level with the other's start or end when it comes within `near`. */
static int32_t
arrange_level(
	int32_t at,
	uint32_t size,
	int32_t other,
	uint32_t other_size,
	uint32_t near)
{
	int64_t start_gap;
	int64_t end_gap;

	/* The gaps to the other's start and to its end. */
	start_gap = (int64_t)at - other;
	if (start_gap < 0)
		start_gap = -start_gap;
	end_gap = ((int64_t)at + size) - ((int64_t)other + other_size);
	if (end_gap < 0)
		end_gap = -end_gap;

	/* Level with the start, then the end. */
	if (start_gap <= (int64_t)near)
		return other;
	if (end_gap <= (int64_t)near)
		return (int32_t)((int64_t)other + other_size - size);

	/* Succeeded: where it was. */
	return at;
}

/* Takes a place as the best so far when it overlaps nothing and is nearer the wanted one. */
static void
arrange_try(
	const struct se_arrange_rect *rects,
	unsigned count,
	unsigned moving,
	int32_t x,
	int32_t y,
	int32_t want_x,
	int32_t want_y,
	int64_t *best,
	int32_t *best_x,
	int32_t *best_y)
{
	int64_t dx;
	int64_t dy;
	int64_t distance;
	int overlaps;

	/* A place that overlaps a display is none. */
	overlaps = arrange_overlaps(rects, count, moving, x, y);
	if (overlaps)
		return;

	/* The square of its distance from the wanted place. */
	dx = (int64_t)x - want_x;
	dy = (int64_t)y - want_y;
	distance = dx * dx + dy * dy;
	if (*best >= 0 && distance >= *best)
		return;

	/* Succeeded: the best so far. */
	*best = distance;
	*best_x = x;
	*best_y = y;
}
