/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * position: sticky against the scroll of the page (ws074-p235).
 *
 * A sticky box is laid out where it would be in the flow (it is a positioned
 * box); this pass then moves it, with everything inside it, so that its
 * margin box stays inside the scrollport shrunk by its top, right, bottom and
 * left offsets, as trail as its parent's content box allows.  The scrollport is
 * the viewport at the page's scroll, or, for a box inside an element that
 * clips, that element's padding box (the engine scrolls only the page, so
 * such an element is taken at scroll zero).  The pass can be run again for
 * another scroll without laying the page out again: each box remembers how
 * trail it is moved.
 */

#include "layout/layout.h"

#include <errno.h>
#include <string.h>

/* The most nested boxes searched. */
#define STICKY_DEPTH	256

static int sticky_collect(struct layout_tree *tree, struct layout_box *box, struct wb_vector *found, int depth);
static void sticky_shift(struct layout_box *box, layout_unit dx, layout_unit dy, int depth);
static int sticky_edge(const struct css_length *length, layout_unit size, layout_unit *result);
static layout_unit sticky_clamp(layout_unit shift, layout_unit low, layout_unit high);
static void sticky_axis(layout_unit start, layout_unit end, layout_unit port_start, layout_unit port_end, const struct css_length *lead, const struct css_length *trail, layout_unit limit_start, layout_unit limit_end, layout_unit *shift);

/*
 * Finds the sticky boxes of a laid out tree (in tree order) and places them
 * for a page scrolled by scroll_y.
 */
int
layout_sticky_build(
	struct layout_tree *tree,
	layout_unit scroll_y)
{
	struct wb_vector found;
	size_t index;
	int error;

	/* The boxes, in tree order. */
	tree->sticky = NULL;
	tree->sticky_count = 0;
	tree->sticky_scroll = -1;
	if (tree->root == NULL)
		return 0;
	wb_vector_init(&found, sizeof(struct layout_box *));
	error = sticky_collect(tree, tree->root, &found, 0);
	if (error != 0) {
		wb_vector_release(&found);
		return error;
	}
	if (found.count == 0) {
		wb_vector_release(&found);
		return 0;
	}

	/* They live as long as the tree. */
	tree->sticky = wb_arena_alloc(&tree->arena, found.count * sizeof(struct layout_box *));
	if (tree->sticky == NULL) {
		wb_vector_release(&found);
		return ENOMEM;
	}
	for (index = 0; index < found.count; index++)
		tree->sticky[index] = *(struct layout_box **)wb_vector_at(&found, index);
	tree->sticky_count = found.count;
	wb_vector_release(&found);

	/* Placed for the scroll. */
	layout_sticky_apply(tree, scroll_y);
	return 0;
}

/*
 * Moves the sticky boxes for a page scrolled by scroll_y; reports whether
 * any box moved (the display list is then built again).
 */
int
layout_sticky_apply(
	struct layout_tree *tree,
	layout_unit scroll_y)
{
	struct layout_box *box;
	struct layout_box *block;
	struct layout_box *clip;
	layout_unit port_left;
	layout_unit port_right;
	layout_unit port_top;
	layout_unit port_bottom;
	layout_unit natural_x;
	layout_unit natural_y;
	layout_unit left;
	layout_unit right;
	layout_unit top;
	layout_unit bottom;
	layout_unit dx;
	layout_unit dy;
	size_t index;
	int moved;

	/* The same scroll leaves the boxes where they are. */
	moved = 0;
	if (tree->sticky_count == 0 || tree->sticky_scroll == scroll_y)
		return 0;
	tree->sticky_scroll = scroll_y;
	for (index = 0; index < tree->sticky_count; index++) {
		box = tree->sticky[index];

		/* A fixed box is moved down by the scroll so that it stays in the viewport (one an anchor places scrolls with the anchor). */
		if (box->style.position == CSS_POSITION_FIXED) {
			dy = box->anchored ? 0 : scroll_y;
			if (dy == box->sticky_dy)
				continue;
			sticky_shift(box, 0, dy - box->sticky_dy, 0);
			box->sticky_dy = dy;
			moved = 1;
			continue;
		}

		/* The nearest block around it gives the limit, the nearest clipping box the scrollport. */
		block = box->parent;
		while (block != NULL && block->kind != LAYOUT_BLOCK && block->kind != LAYOUT_ANONYMOUS_BLOCK)
			block = block->parent;
		if (block == NULL)
			continue;
		clip = block;
		while (clip != NULL && !layout_clips(clip))
			clip = clip->parent;
		if (clip != NULL) {
			port_left = clip->x + clip->border[CSS_LEFT];
			port_right = port_left + clip->padding[CSS_LEFT] + clip->width + clip->padding[CSS_RIGHT];
			port_top = clip->y + clip->border[CSS_TOP];
			port_bottom = port_top + clip->padding[CSS_TOP] + clip->height + clip->padding[CSS_BOTTOM];
		} else {
			port_left = 0;
			port_right = tree->viewport_width;
			port_top = scroll_y;
			port_bottom = scroll_y + tree->viewport_height;
		}

		/* Where the margin box would be without the move, and the room its parent's content box gives. */
		natural_x = box->x - box->sticky_dx;
		natural_y = box->y - box->sticky_dy;
		left = block->x + block->border[CSS_LEFT] + block->padding[CSS_LEFT];
		right = left + block->width;
		top = block->y + block->border[CSS_TOP] + block->padding[CSS_TOP];
		bottom = top + block->height;
		dx = 0;
		dy = 0;
		sticky_axis(natural_x - box->margin[CSS_LEFT],
		    natural_x + box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT] + box->margin[CSS_RIGHT],
		    port_left, port_right, &box->style.offset[CSS_LEFT], &box->style.offset[CSS_RIGHT], left, right, &dx);
		sticky_axis(natural_y - box->margin[CSS_TOP],
		    natural_y + box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM] + box->margin[CSS_BOTTOM],
		    port_top, port_bottom, &box->style.offset[CSS_TOP], &box->style.offset[CSS_BOTTOM], top, bottom, &dy);
		if (dx == box->sticky_dx && dy == box->sticky_dy)
			continue;

		/* The box and what is inside it. */
		sticky_shift(box, dx - box->sticky_dx, dy - box->sticky_dy, 0);
		box->sticky_dx = dx;
		box->sticky_dy = dy;
		moved = 1;
	}
	return moved;
}

/* Finds the sticky boxes under a box, in tree order. */
static int
sticky_collect(
	struct layout_tree *tree,
	struct layout_box *box,
	struct wb_vector *found,
	int depth)
{
	struct layout_box *child;
	int error;

	if (depth > STICKY_DEPTH)
		return 0;

	/* A block or an inline block with the position (the lines of an inline box are not boxes to move); a fixed box stays where the viewport is. */
	if (((box->style.position == CSS_POSITION_STICKY && !box->out_of_flow) ||
	    (box->style.position == CSS_POSITION_FIXED && box->out_of_flow)) &&
	    (box->kind == LAYOUT_BLOCK || box->kind == LAYOUT_ANONYMOUS_BLOCK)) {
		box->sticky_dx = 0;
		box->sticky_dy = 0;
		error = wb_vector_push(found, &box);
		if (error != 0)
			return error;
	}
	for (child = box->first_child; child != NULL; child = child->next) {
		error = sticky_collect(tree, child, found, depth + 1);
		if (error != 0)
			return error;
	}
	return 0;
}

/*
 * Moves a box and the boxes that are placed absolutely inside it.  A fixed
 * box stays; an inline box has no place of its own (its lines move with the
 * block that holds them).
 */
static void
sticky_shift(
	struct layout_box *box,
	layout_unit dx,
	layout_unit dy,
	int depth)
{
	struct layout_box *child;

	if (depth > STICKY_DEPTH)
		return;
	if (box->kind != LAYOUT_INLINE && box->kind != LAYOUT_TEXT) {
		box->x += dx;
		box->y += dy;
		box->static_x += dx;
		box->static_y += dy;
		box->cb_left += dx;
		box->cb_right += dx;
		box->cb_top += dy;
		box->cb_bottom += dy;
	}
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow && child->style.position == CSS_POSITION_FIXED)
			continue;
		sticky_shift(child, dx, dy, depth + 1);
	}
}

/* Resolves an offset against the scrollport's size; reports whether it is not auto. */
static int
sticky_edge(
	const struct css_length *length,
	layout_unit size,
	layout_unit *result)
{
	if (length->unit == CSS_UNIT_PX) {
		*result = layout_from_px(length->value);
		return 1;
	}
	if (length->unit == CSS_UNIT_PERCENT) {
		*result = (layout_unit)((float)size * length->value / 100.0f) + layout_from_px(length->offset);
		return 1;
	}
	return 0;
}

/* Keeps a shift between two limits (the low one is not above the high one). */
static layout_unit
sticky_clamp(
	layout_unit shift,
	layout_unit low,
	layout_unit high)
{
	if (shift > high)
		shift = high;
	if (shift < low)
		shift = low;
	return shift;
}

/*
 * One axis: the margin box from start to end moves into the scrollport
 * shrunk by the lead and trail offsets, the trail offset first so that the lead
 * one wins; then the limits (the parent's content box) hold it.
 */
static void
sticky_axis(
	layout_unit start,
	layout_unit end,
	layout_unit port_start,
	layout_unit port_end,
	const struct css_length *lead,
	const struct css_length *trail,
	layout_unit limit_start,
	layout_unit limit_end,
	layout_unit *shift)
{
	layout_unit inset;
	layout_unit moved;

	moved = 0;
	if (sticky_edge(trail, port_end - port_start, &inset) && end > port_end - inset)
		moved = port_end - inset - end;
	if (sticky_edge(lead, port_end - port_start, &inset) && start + moved < port_start + inset)
		moved = port_start + inset - start;
	if (moved != 0)
		moved = sticky_clamp(moved, limit_start - start < 0 ? limit_start - start : 0, limit_end - end > 0 ? limit_end - end : 0);
	*shift = moved;
}
