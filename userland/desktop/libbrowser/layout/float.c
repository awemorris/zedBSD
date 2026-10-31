/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Floats (CSS 2 §9.5): the floats of a block formatting context, the
 * placing of a float as high and as far to its side as it fits, the room
 * a line box has between the floats beside it, and clearance.
 *
 * A block formatting context keeps its floats' margin boxes in its own
 * coordinates (from its content box's top left).  While a block inside it
 * is laid out, the tree knows where that block's content box is in those
 * coordinates (origin_x, origin_y), so the block can ask about the floats
 * in its own coordinates.  A block's position is known before its content
 * is laid out only up to the collapsing of its top margin with its first
 * child's; that part of the position is left out (floats may then sit a
 * margin's height off).
 */

#include "layout/layout.h"

#include <errno.h>
#include <string.h>

/*
 * One float of a formatting context: its side and its margin box in the
 * context's coordinates.
 */
struct float_rect {
	int side;
	layout_unit x;
	layout_unit y;
	layout_unit width;
	layout_unit height;
};

static int float_overlaps(const struct float_rect *rect, layout_unit top, layout_unit bottom);
static void float_room(const struct layout_tree *tree, layout_unit top, layout_unit bottom, layout_unit width, layout_unit *left, layout_unit *right);
static layout_unit float_next_bottom(const struct layout_tree *tree, layout_unit top);

/*
 * Starts a new block formatting context for a box whose content box is at
 * the tree's origin: saves the one it is in (into context) and makes the
 * tree's floats an empty list (which the caller owns until
 * layout_context_end).
 */
void
layout_context_begin(
	struct layout_tree *tree,
	struct layout_context *context,
	struct wb_vector *floats)
{
	/* The context the box is in. */
	context->floats = tree->floats;
	context->origin_x = tree->origin_x;
	context->origin_y = tree->origin_y;

	/* The new one, whose coordinates start at the box's content box. */
	wb_vector_init(floats, sizeof(struct float_rect));
	tree->floats = floats;
	tree->origin_x = 0;
	tree->origin_y = 0;
}

/*
 * Ends a block formatting context: frees its floats and returns to the
 * context it was in.
 */
void
layout_context_end(
	struct layout_tree *tree,
	const struct layout_context *context)
{
	/* The floats go with the context. */
	if (tree->floats != NULL)
		wb_vector_release(tree->floats);

	/* The outer context again. */
	tree->floats = context->floats;
	tree->origin_x = context->origin_x;
	tree->origin_y = context->origin_y;
}

/*
 * Places a laid out float in the current block's content box: no higher
 * than y_min, as high as it fits beside the other floats, at its side;
 * sets its x and y (its border box, relative to the content box) and adds
 * it to the context.
 */
int
layout_place_float(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit y_min,
	layout_unit width)
{
	struct float_rect rect;
	layout_unit outer_width;
	layout_unit outer_height;
	layout_unit top;
	layout_unit left;
	layout_unit right;
	layout_unit clear;
	layout_unit next;
	int tries;
	int error;

	/* The margin box. */
	outer_width = box->margin[CSS_LEFT] + box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width +
	    box->padding[CSS_RIGHT] + box->border[CSS_RIGHT] + box->margin[CSS_RIGHT];
	outer_height = box->margin[CSS_TOP] + box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height +
	    box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM] + box->margin[CSS_BOTTOM];

	/* No higher than asked, nor above an earlier float, nor above what it clears. */
	top = y_min;
	clear = layout_clearance(tree, box->style.clear);
	if (clear > top)
		top = clear;
	if (tree->floats != NULL) {
		size_t at;

		for (at = 0; at < tree->floats->count; at++) {
			const struct float_rect *earlier;

			earlier = wb_vector_at(tree->floats, at);
			if (earlier->y - tree->origin_y > top)
				top = earlier->y - tree->origin_y;
		}
	}

	/* Moves down past floats until the box fits between the others (or is alone on its band). */
	for (tries = 0; tries < 1000; tries++) {
		float_room(tree, top, top + outer_height, width, &left, &right);
		if (right - left >= outer_width)
			break;
		next = float_next_bottom(tree, top);
		if (next <= top)
			break;
		top = next;
	}

	/* The side it goes to. */
	rect.side = box->floating;
	rect.y = tree->origin_y + top;
	rect.width = outer_width;
	rect.height = outer_height;
	if (box->floating == CSS_FLOAT_RIGHT) {
		rect.x = tree->origin_x + right - outer_width;
	} else {
		rect.x = tree->origin_x + left;
	}

	/* The context keeps it. */
	if (tree->floats != NULL) {
		error = wb_vector_push(tree->floats, &rect);
		if (error != 0)
			return error;
	}

	/* Succeeded: the border box is placed in the content box. */
	box->x = rect.x - tree->origin_x + box->margin[CSS_LEFT];
	box->y = top + box->margin[CSS_TOP];
	return 0;
}

/*
 * Reports the room a line has between the floats beside a band of the
 * current block's content box (from top to bottom): its left and right
 * edges in the content box.
 */
void
layout_line_room(
	const struct layout_tree *tree,
	layout_unit top,
	layout_unit bottom,
	layout_unit width,
	layout_unit *left,
	layout_unit *right)
{
	/* The same room a float would have. */
	float_room(tree, top, bottom, width, left, right);
}

/*
 * Reports the top of the next band of the current block's content box
 * where a float ends below top, or top when no float ends below it.
 */
layout_unit
layout_below_float(
	const struct layout_tree *tree,
	layout_unit top)
{
	layout_unit next;

	/* The nearest float bottom below the top. */
	next = float_next_bottom(tree, top);

	/* Reports it. */
	return next;
}

/*
 * Reports how far down a box that clears a side (CSS_FLOAT_LEFT,
 * CSS_FLOAT_RIGHT or CSS_CLEAR_BOTH) must start, in the current block's
 * content box: below every float of that side; a very high value when it
 * clears nothing.
 */
layout_unit
layout_clearance(
	const struct layout_tree *tree,
	int clear)
{
	const struct float_rect *rect;
	layout_unit bottom;
	size_t index;

	/* Nothing to clear. */
	bottom = -((layout_unit)1 << 30);
	if (clear == CSS_FLOAT_NONE || tree->floats == NULL)
		return bottom;

	/* The lowest bottom of the floats of the side. */
	for (index = 0; index < tree->floats->count; index++) {
		rect = wb_vector_at(tree->floats, index);
		if (clear != CSS_CLEAR_BOTH && rect->side != clear)
			continue;
		if (rect->y + rect->height - tree->origin_y > bottom)
			bottom = rect->y + rect->height - tree->origin_y;
	}

	/* Reports it. */
	return bottom;
}

/*
 * Reports the bottom of the lowest float of the current context, in the
 * current block's content box (zero when there is none).
 */
layout_unit
layout_floats_bottom(
	const struct layout_tree *tree)
{
	const struct float_rect *rect;
	layout_unit bottom;
	size_t index;

	/* The lowest bottom. */
	bottom = 0;
	if (tree->floats == NULL)
		return bottom;
	for (index = 0; index < tree->floats->count; index++) {
		rect = wb_vector_at(tree->floats, index);
		if (rect->y + rect->height - tree->origin_y > bottom)
			bottom = rect->y + rect->height - tree->origin_y;
	}

	/* Reports it. */
	return bottom;
}

/* Tells whether a float's margin box reaches into a band (top inclusive, bottom exclusive). */
static int
float_overlaps(
	const struct float_rect *rect,
	layout_unit top,
	layout_unit bottom)
{
	/* A float that ends above the band, or starts below it, does not. */
	if (rect->y + rect->height <= top)
		return 0;
	if (rect->y >= bottom)
		return 0;

	/* It does. */
	return 1;
}

/* Finds the room between the floats beside a band of the current content box (edges in the content box). */
static void
float_room(
	const struct layout_tree *tree,
	layout_unit top,
	layout_unit bottom,
	layout_unit width,
	layout_unit *left,
	layout_unit *right)
{
	const struct float_rect *rect;
	layout_unit edge;
	size_t index;
	int overlaps;

	/* The whole width when no float is beside the band. */
	*left = 0;
	*right = width;
	if (tree->floats == NULL)
		return;

	/* Each float beside the band narrows its side. */
	if (bottom <= top)
		bottom = top + 1;
	for (index = 0; index < tree->floats->count; index++) {
		rect = wb_vector_at(tree->floats, index);
		overlaps = float_overlaps(rect, tree->origin_y + top, tree->origin_y + bottom);
		if (!overlaps)
			continue;

		/* A left float pushes the left edge right; a right one the right edge left. */
		if (rect->side == CSS_FLOAT_LEFT) {
			edge = rect->x + rect->width - tree->origin_x;
			if (edge > *left)
				*left = edge;
		} else {
			edge = rect->x - tree->origin_x;
			if (edge < *right)
				*right = edge;
		}
	}
}

/* Finds the nearest bottom of a float below top in the current content box, or top when there is none. */
static layout_unit
float_next_bottom(
	const struct layout_tree *tree,
	layout_unit top)
{
	const struct float_rect *rect;
	layout_unit bottom;
	layout_unit best;
	size_t index;
	int found;

	/* The smallest bottom below the top. */
	best = top;
	found = 0;
	if (tree->floats == NULL)
		return best;
	for (index = 0; index < tree->floats->count; index++) {
		rect = wb_vector_at(tree->floats, index);
		bottom = rect->y + rect->height - tree->origin_y;
		if (bottom <= top)
			continue;
		if (!found || bottom < best) {
			best = bottom;
			found = 1;
		}
	}

	/* The band below, or the same top. */
	return best;
}
