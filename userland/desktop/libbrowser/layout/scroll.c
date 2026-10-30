/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scroll of the elements that scroll their content (overflow other than
 * visible and clip): after the page is laid out and its boxes out of the
 * flow are placed, the content of each container moves against the container
 * by the distance it is scrolled.  The container's own box, its border and
 * its clip stay.  A box out of the flow moves with the container when its
 * containing block is the container or inside it; one placed against
 * something outside does not.  A box that is placed by an anchor follows the
 * scroll of the scroll containers the anchor is in, except the ones its
 * containing block is in too.
 * https://drafts.csswg.org/css-anchor-position-1/#scroll
 * https://drafts.csswg.org/cssom-view-1/#scrolling
 */

#include "layout/layout.h"

#include <errno.h>
#include <string.h>

/* The most nested boxes searched. */
#define SCROLL_DEPTH	256

static int scroll_collect(const struct layout_box *box, struct wb_vector *containers, int depth);
static void scroll_shift(struct layout_box *box, const struct layout_box *root, layout_unit dx, layout_unit dy, int self, int depth);
static int scroll_inside(const struct layout_box *box, const struct layout_box *ancestor);

/*
 * Moves the content of the scroll containers, and the boxes that follow an
 * anchor's scroll, of a laid out tree.
 */
int
layout_scroll_apply(
	struct layout_tree *tree)
{
	struct wb_vector containers;
	const struct layout_box *box;
	double x;
	double y;
	size_t index;
	int error;

	/* The containers that are scrolled. */
	if (tree->root == NULL)
		return 0;
	wb_vector_init(&containers, sizeof(const struct layout_box *));
	error = scroll_collect(tree->root, &containers, 0);
	if (error != 0 || containers.count == 0) {
		wb_vector_release(&containers);
		return error;
	}

	/* The content of each container. */
	for (index = 0; index < containers.count; index++) {
		box = *(const struct layout_box **)wb_vector_at(&containers, index);
		layout_scroll_offset(box, &x, &y);
		scroll_shift((struct layout_box *)box, box, -layout_from_px((float)x), -layout_from_px((float)y), 0, 0);
	}
	wb_vector_release(&containers);
	return 0;
}

/* Collects the scrolled containers in tree order. */
static int
scroll_collect(
	const struct layout_box *box,
	struct wb_vector *containers,
	int depth)
{
	const struct layout_box *child;
	double x;
	double y;
	int error;

	if (depth > SCROLL_DEPTH)
		return 0;
	if (layout_scroll_offset(box, &x, &y)) {
		error = wb_vector_push(containers, &box);
		if (error != 0)
			return error;
	}
	for (child = box->first_child; child != NULL; child = child->next) {
		error = scroll_collect(child, containers, depth + 1);
		if (error != 0)
			return error;
	}
	return 0;
}

/* Tells whether a box is an element that scrolls its content and is scrolled, and by how much. */
int
layout_scroll_offset(
	const struct layout_box *box,
	double *x,
	double *y)
{
	const struct dom_element *element;

	*x = 0.0;
	*y = 0.0;
	if (box->parent == NULL || box->node == NULL || box->node->type != DOM_ELEMENT)
		return 0;
	if (box->kind != LAYOUT_BLOCK && box->kind != LAYOUT_ANONYMOUS_BLOCK)
		return 0;
	if (box->style.overflow_x == CSS_OVERFLOW_VISIBLE && box->style.overflow_y == CSS_OVERFLOW_VISIBLE)
		return 0;
	element = (const struct dom_element *)box->node;
	if (element->scroll_x == 0.0 && element->scroll_y == 0.0)
		return 0;
	*x = element->scroll_x;
	*y = element->scroll_y;
	return 1;
}

/* Tells whether a box is a box or inside it. */
static int
scroll_inside(
	const struct layout_box *box,
	const struct layout_box *ancestor)
{
	for (; box != NULL; box = box->parent) {
		if (box == ancestor)
			return 1;
	}
	return 0;
}

/*
 * Moves the boxes below a box (and the box too, when self is set): the ones in the flow, and the ones out of the flow
 * whose containing block is inside root.  An inline box has no place of its own, and a fixed box stays.
 */
static void
scroll_shift(
	struct layout_box *box,
	const struct layout_box *root,
	layout_unit dx,
	layout_unit dy,
	int self,
	int depth)
{
	struct layout_box *child;
	const struct layout_box *containing;

	if (depth > SCROLL_DEPTH)
		return;
	if (self && box->kind != LAYOUT_INLINE && box->kind != LAYOUT_TEXT) {
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
		if (child->out_of_flow) {
			if (child->style.position == CSS_POSITION_FIXED)
				continue;
			containing = layout_containing_box(child);
			if (containing == NULL || !scroll_inside(containing, root))
				continue;
		}
		scroll_shift(child, root, dx, dy, 1, depth + 1);
	}
}
