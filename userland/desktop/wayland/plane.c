/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The plane of the displays shown at once (ws113-p007; plane.h says what
 * and why).
 */

#include "plane.h"

#include <stddef.h>

static int plane_inside(const struct kwl_plane_rect *rect, int32_t x, int32_t y);
static void plane_clamp(const struct kwl_plane_rect *rect, int32_t x, int32_t y, int32_t *clamped_x, int32_t *clamped_y);
static int32_t plane_span(int64_t value, int64_t low, int64_t high);
static int64_t plane_centre(int32_t start, uint32_t size);

/* Finds the output whose rectangle holds a point: its slot, or -1 for none. */
int
kwl_plane_at(
	const struct kwl_plane_rect *outputs,
	unsigned count,
	int32_t x,
	int32_t y)
{
	unsigned slot;
	int inside;

	/* The first output shown that holds it (they do not overlap). */
	for (slot = 0U; slot < count; slot++) {
		inside = plane_inside(&outputs[slot], x, y);
		if (inside)
			return (int)slot;
	}

	/* None. */
	return -1;
}

/*
 * Moves a point of the output `current` by (dx, dy): into the output that
 * holds the new point, or, where none does (past an edge no output
 * shares, or across a corner), to the nearest point of an output shown,
 * the current one on a tie.  Returns the output it is on.
 */
unsigned
kwl_plane_move(
	const struct kwl_plane_rect *outputs,
	unsigned count,
	unsigned current,
	int32_t x,
	int32_t y,
	int32_t dx,
	int32_t dy,
	int32_t *moved_x,
	int32_t *moved_y)
{
	int64_t best;
	int64_t far_x;
	int64_t far_y;
	int64_t distance;
	int32_t want_x;
	int32_t want_y;
	int32_t near_x;
	int32_t near_y;
	unsigned chosen;
	unsigned slot;
	int found;

	/* The point wanted, within the plane's range. */
	want_x = plane_span((int64_t)x + dx, INT32_MIN / 2, INT32_MAX / 2);
	want_y = plane_span((int64_t)y + dy, INT32_MIN / 2, INT32_MAX / 2);
	*moved_x = want_x;
	*moved_y = want_y;

	/* An output holds it. */
	found = kwl_plane_at(outputs, count, want_x, want_y);
	if (found >= 0)
		return (unsigned)found;

	/* The current output when it is shown (else the anchor): its nearest point first. */
	if (current >= count || outputs[current].width == 0U || outputs[current].height == 0U)
		current = KWL_PLANE_ANCHOR;
	plane_clamp(&outputs[current], want_x, want_y, moved_x, moved_y);
	far_x = (int64_t)*moved_x - want_x;
	far_y = (int64_t)*moved_y - want_y;
	best = far_x * far_x + far_y * far_y;
	chosen = current;

	/* Another output's nearer point. */
	for (slot = 0U; slot < count; slot++) {
		if (slot == current || outputs[slot].width == 0U || outputs[slot].height == 0U)
			continue;
		plane_clamp(&outputs[slot], want_x, want_y, &near_x, &near_y);
		far_x = (int64_t)near_x - want_x;
		far_y = (int64_t)near_y - want_y;
		distance = far_x * far_x + far_y * far_y;
		if (distance >= best)
			continue;

		/* The nearest so far. */
		best = distance;
		chosen = slot;
		*moved_x = near_x;
		*moved_y = near_y;
	}

	/* Succeeded: the output it is on. */
	return chosen;
}

/*
 * Finds the output beside `current` the other way (direction -1 left, +1
 * right): of the outputs shown whose middles lie that way, the one whose
 * middle is nearest.  Returns its slot, or -1 for none.
 */
int
kwl_plane_neighbour(
	const struct kwl_plane_rect *outputs,
	unsigned count,
	unsigned current,
	int direction)
{
	int64_t middle_x;
	int64_t middle_y;
	int64_t along;
	int64_t across;
	int64_t distance;
	int64_t best;
	unsigned slot;
	int found;

	/* From the current output's middle. */
	if (current >= count || outputs[current].width == 0U)
		return -1;
	middle_x = plane_centre(outputs[current].x, outputs[current].width);
	middle_y = plane_centre(outputs[current].y, outputs[current].height);
	found = -1;
	best = -1;

	/* Each other output shown that way. */
	for (slot = 0U; slot < count; slot++) {
		if (slot == current || outputs[slot].width == 0U || outputs[slot].height == 0U)
			continue;
		along = (plane_centre(outputs[slot].x, outputs[slot].width) - middle_x) * direction;
		if (along <= 0)
			continue;

		/* The nearest middle. */
		across = plane_centre(outputs[slot].y, outputs[slot].height) - middle_y;
		distance = along * along + across * across;
		if (best >= 0 && distance >= best)
			continue;
		best = distance;
		found = (int)slot;
	}

	/* Succeeded: the output, or -1. */
	return found;
}

/*
 * Carries a window (its place and size) from one output to another: at
 * the same share of the way across and down, then kept inside it (from
 * its left and from `top`, the least distance of the window's place below
 * the output's top, when it is larger).
 */
void
kwl_plane_carry(
	const struct kwl_plane_rect *from,
	const struct kwl_plane_rect *to,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height,
	int32_t top,
	int32_t *carried_x,
	int32_t *carried_y)
{
	int64_t place_x;
	int64_t place_y;
	int64_t lowest;
	int64_t highest;

	/* The same share of the way (the output's own place without a size to measure by). */
	place_x = to->x;
	place_y = to->y;
	if (from->width > 0U)
		place_x = (int64_t)to->x + ((int64_t)x - from->x) * to->width / from->width;
	if (from->height > 0U)
		place_y = (int64_t)to->y + ((int64_t)y - from->y) * to->height / from->height;

	/* Across: inside, or from its left when wider. */
	highest = (int64_t)to->x + to->width - width;
	if (highest < to->x)
		highest = to->x;
	*carried_x = plane_span(place_x, to->x, highest);

	/* Down: below `top`, and inside, or from `top` when taller. */
	lowest = (int64_t)to->y + top;
	highest = (int64_t)to->y + to->height - height;
	if (highest < lowest)
		highest = lowest;
	*carried_y = plane_span(place_y, lowest, highest);
}

/* Records where a widget is drawn on an output's bar: its left and its bar's top in the plane. */
void
kwl_plane_place(
	struct kwl_plane_places *places,
	unsigned slot,
	int32_t x,
	int32_t top)
{
	/* No such output. */
	if (slot >= KWL_PLANE_SLOTS)
		return;

	/* Its place, which the bit says is there. */
	places->x[slot] = x;
	places->top[slot] = top;
	places->placed |= 1U << slot;
}

/*
 * Gives where a widget was last drawn on an output's bar.  Returns 1 when
 * it was drawn there, 0 (and nothing given) when it never was.
 */
int
kwl_plane_placed(
	const struct kwl_plane_places *places,
	unsigned slot,
	int32_t *x,
	int32_t *top)
{
	/* No such output, or never drawn on it. */
	if (slot >= KWL_PLANE_SLOTS)
		return 0;
	if ((places->placed & (1U << slot)) == 0U)
		return 0;

	/* Succeeded: its place. */
	*x = places->x[slot];
	*top = places->top[slot];
	return 1;
}

/* Tells whether a shown output's rectangle (half-open) holds a point. */
static int
plane_inside(
	const struct kwl_plane_rect *rect,
	int32_t x,
	int32_t y)
{
	/* Not shown, or outside either way. */
	if (rect->width == 0U || rect->height == 0U)
		return 0;
	if (x < rect->x || (int64_t)x >= (int64_t)rect->x + rect->width)
		return 0;
	if (y < rect->y || (int64_t)y >= (int64_t)rect->y + rect->height)
		return 0;

	/* Succeeded: inside. */
	return 1;
}

/* Gives the point of a shown output's rectangle nearest a point. */
static void
plane_clamp(
	const struct kwl_plane_rect *rect,
	int32_t x,
	int32_t y,
	int32_t *clamped_x,
	int32_t *clamped_y)
{
	/* Each axis kept within the rectangle's pixels. */
	*clamped_x = plane_span(x, rect->x, (int64_t)rect->x + rect->width - 1);
	*clamped_y = plane_span(y, rect->y, (int64_t)rect->y + rect->height - 1);
}

/* Keeps a value within [low, high]. */
static int32_t
plane_span(
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

/* The middle of a span, doubled (to stay whole). */
static int64_t
plane_centre(
	int32_t start,
	uint32_t size)
{
	/* Twice the start, and the size. */
	return 2 * (int64_t)start + size;
}
