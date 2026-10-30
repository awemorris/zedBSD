/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Hit testing: the box under a point of the laid out page.
 *
 * A point on a line's fragment hits the text box the fragment was cut
 * from, which is what a click on a link needs; a point on no text hits the
 * deepest block around it.  The positioned boxes are searched first, the
 * one painted last first (layout_stacking_order), then the normal flow,
 * then the positioned boxes painted below it.  An inline block is searched
 * like a float among its line's content.
 */

#include "layout/layout.h"

/* What a search looks for: text only, or text and then the deepest block. */
#define HIT_TEXT	0
#define HIT_ANY		1

static const struct layout_box *hit_box(const struct layout_box *box, layout_unit x, layout_unit y, int depth);
static const struct layout_box *hit_block(const struct layout_box *box, layout_unit x, layout_unit y, int depth);
static int hit_is_items(const struct layout_box *box);
static const struct layout_box *hit_items(const struct layout_box *box, layout_unit x, layout_unit y, int depth, int text);
static int hit_before(const struct layout_box *box, const struct layout_box *first, const struct layout_box *second);
static const struct layout_box *hit_lines(const struct layout_box *box, layout_unit x, layout_unit y);
static const struct layout_box *hit_in(const struct layout_box *box, layout_unit x, layout_unit y, int what);
static const struct layout_box *hit_inline_floats(const struct layout_box *box, layout_unit x, layout_unit y, int depth);
static const struct layout_box *hit_inline_blocks(const struct layout_box *box, layout_unit x, layout_unit y, int depth);
static const struct layout_box *hit_ordered(const struct layout_tree *tree, layout_unit x, layout_unit y, int what);

/*
 * Finds the box of the text under a point in document coordinates (layout
 * units); NULL when the point is on no text.
 */
const struct layout_box *
layout_hit(
	const struct layout_tree *tree,
	layout_unit x,
	layout_unit y)
{
	const struct layout_box *found;

	/* An empty page has nothing to hit. */
	if (tree->root == NULL)
		return NULL;

	/* Searches the layers from the top. */
	found = hit_ordered(tree, x, y, HIT_TEXT);

	/* Reports the box, or NULL. */
	return found;
}

/*
 * Finds the DOM node under a point in document coordinates (layout
 * units): the text there, or else the deepest block whose border box holds
 * the point (NULL outside the root's box).  A click's target is this
 * node's element.
 */
struct dom_node *
layout_hit_node(
	const struct layout_tree *tree,
	layout_unit x,
	layout_unit y)
{
	const struct layout_box *found;

	/* The text under the point, or the deepest block around it, from the top layer down. */
	found = NULL;
	if (tree->root != NULL)
		found = hit_ordered(tree, x, y, HIT_ANY);

	/* An anonymous block stands for its parent's node. */
	while (found != NULL && found->node == NULL)
		found = found->parent;
	if (found == NULL)
		return NULL;

	/* The node. */
	return found->node;
}

/* Searches a block and its descendants for the text under a point. */
static const struct layout_box *
hit_box(
	const struct layout_box *box,
	layout_unit x,
	layout_unit y,
	int depth)
{
	const struct layout_box *child;
	const struct layout_box *found;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return NULL;

	/* Only blocks hold lines or blocks. */
	if (box->kind != LAYOUT_BLOCK && box->kind != LAYOUT_ANONYMOUS_BLOCK)
		return NULL;

	/* A block of lines: a float among its content, or the fragment under the point. */
	if (box->children_inline) {
		found = hit_inline_floats(box, x, y, depth);
		if (found != NULL)
			return found;
		found = hit_lines(box, x, y);
		return found;
	}

	/* The items of a flex or grid container are searched from the one painted last. */
	if (hit_is_items(box))
		return hit_items(box, x, y, depth, 1);

	/* A block of blocks: the first child with text under the point. */
	for (child = box->first_child; child != NULL; child = child->next) {
		found = hit_box(child, x, y, depth + 1);
		if (found != NULL)
			return found;
	}

	/* Nothing under the point in this block. */
	return NULL;
}

/* Finds the fragment of a block's lines under a point and reports its box. */
static const struct layout_box *
hit_lines(
	const struct layout_box *box,
	layout_unit x,
	layout_unit y)
{
	const struct layout_line *line;
	const struct layout_fragment *fragment;
	layout_unit left;
	layout_unit top;
	layout_unit start;
	size_t index;
	size_t item;

	/* The content box's origin, which the lines are placed from. */
	left = box->x + box->border[CSS_LEFT] + box->padding[CSS_LEFT];
	top = box->y + box->border[CSS_TOP] + box->padding[CSS_TOP];

	/* The line whose band holds the point. */
	for (index = 0; index < box->line_count; index++) {
		line = &box->lines[index];

		/* The line occupies an x band vertically and a y band horizontally. */
		if (box->vertical_inline) {
			if (x < left + line->y || x >= left + line->y + line->height)
				continue;
		} else {
			if (y < top + line->y || y >= top + line->y + line->height)
				continue;
		}

		/* The fragment of that line whose run holds the point (an inline block's text was searched before). */
		for (item = 0; item < line->fragment_count; item++) {
			fragment = &line->fragments[item];

			/* Atomic content has a separately tested box. */
			if (fragment->box->atomic)
				continue;

			/* The run's logical advance maps to the physical y or x axis. */
			if (box->vertical_inline) {
				start = top + line->left + fragment->x;
				if (y >= start && y < start + fragment->width)
					return fragment->box;
			} else {
				start = left + line->left + fragment->x;
				if (x >= start && x < start + fragment->width)
					return fragment->box;
			}
		}

		/* The point is on the line but on no fragment. */
		return NULL;
	}

	/* The point is on none of the lines. */
	return NULL;
}

/* Finds the deepest block box whose border box holds a point, from a block down. */
static const struct layout_box *
hit_block(
	const struct layout_box *box,
	layout_unit x,
	layout_unit y,
	int depth)
{
	const struct layout_box *child;
	const struct layout_box *found;
	layout_unit width;
	layout_unit height;

	/* Stops at the depth the layout stops at, and at boxes that are not blocks. */
	if (depth > LAYOUT_DEPTH_MAX)
		return NULL;
	if (box->kind != LAYOUT_BLOCK && box->kind != LAYOUT_ANONYMOUS_BLOCK)
		return NULL;

	/* The border box must hold the point. */
	width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	if (x < box->x || x >= box->x + width)
		return NULL;
	if (y < box->y || y >= box->y + height)
		return NULL;

	/* The items of a flex or grid container are painted one by one in order, the last on top: it is searched first. */
	if (!box->children_inline && hit_is_items(box)) {
		found = hit_items(box, x, y, depth, 0);
		if (found != NULL)
			return found;
		return box;
	}

	/* A child block that holds it is deeper. */
	for (child = box->first_child; !box->children_inline && child != NULL; child = child->next) {
		found = hit_block(child, x, y, depth + 1);
		if (found != NULL)
			return found;
	}

	/* So is an inline block among its lines' content. */
	if (box->children_inline) {
		found = hit_inline_blocks(box, x, y, depth + 1);
		if (found != NULL)
			return found;
	}

	/* The block itself. */
	return box;
}

/* Tells whether a block is a flex or grid container. */
static int
hit_is_items(
	const struct layout_box *box)
{
	return box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX ||
	    box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID;
}

/* Finds the deepest block holding a point among a container's items, searching them from the last in order painting (order, then document order). */
static const struct layout_box *
hit_items(
	const struct layout_box *box,
	layout_unit x,
	layout_unit y,
	int depth,
	int text)
{
	const struct layout_box *found;
	const struct layout_box *best;
	const struct layout_box *child;
	const struct layout_box *limit_child;
	int limit_order;
	int limit_set;
	int order;

	/* Each step takes the item that paints last among those not yet searched. */
	limit_set = 0;
	limit_child = NULL;
	limit_order = 0;
	for (;;) {
		best = NULL;
		order = 0;
		for (child = box->first_child; child != NULL; child = child->next) {
			if (limit_set && (child == limit_child || child->style.order > limit_order || (child->style.order == limit_order && !hit_before(box, child, limit_child))))
				continue;
			if (best == NULL || child->style.order > order || (child->style.order == order && hit_before(box, best, child))) {
				best = child;
				order = child->style.order;
			}
		}
		if (best == NULL)
			return NULL;
		found = text ? hit_box(best, x, y, depth + 1) : hit_block(best, x, y, depth + 1);
		if (found != NULL)
			return found;

		/* An item over the point without text of its own covers the ones under it. */
		if (text && x >= best->x && x < best->x + best->border[CSS_LEFT] + best->padding[CSS_LEFT] + best->width + best->padding[CSS_RIGHT] + best->border[CSS_RIGHT] &&
		    y >= best->y && y < best->y + best->border[CSS_TOP] + best->padding[CSS_TOP] + best->height + best->padding[CSS_BOTTOM] + best->border[CSS_BOTTOM])
			return NULL;
		limit_set = 1;
		limit_child = best;
		limit_order = best->style.order;
	}
}

/* Tells whether a child comes before another among a box's children. */
static int
hit_before(
	const struct layout_box *box,
	const struct layout_box *first,
	const struct layout_box *second)
{
	const struct layout_box *child;

	for (child = box->first_child; child != NULL; child = child->next) {
		if (child == first)
			return 1;
		if (child == second)
			return 0;
	}
	return 0;
}

/* Finds the deepest block holding a point inside the inline blocks among a block's inline content. */
static const struct layout_box *
hit_inline_blocks(
	const struct layout_box *box,
	layout_unit x,
	layout_unit y,
	int depth)
{
	const struct layout_box *child;
	const struct layout_box *found;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return NULL;

	/* An inline block is searched like a block; an inline box is searched through. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->floating != CSS_FLOAT_NONE)
			continue;
		if (child->atomic) {
			found = hit_block(child, x, y, depth + 1);
		} else {
			found = hit_inline_blocks(child, x, y, depth + 1);
		}

		/* The first block found. */
		if (found != NULL)
			return found;
	}

	/* No inline block holds the point. */
	return NULL;
}

/* Searches one layer (a positioned box, or the root's normal flow) for text, then for a block when asked. */
static const struct layout_box *
hit_in(
	const struct layout_box *box,
	layout_unit x,
	layout_unit y,
	int what)
{
	const struct layout_box *found;

	/* The text first. */
	found = hit_box(box, x, y, 0);
	if (found != NULL)
		return found;

	/* Then the deepest block, when that is wanted. */
	if (what == HIT_ANY)
		found = hit_block(box, x, y, 0);

	/* The box found, or NULL. */
	return found;
}

/* Searches the layers from the one painted last down to the first, the normal flow in its place among them. */
static const struct layout_box *
hit_ordered(
	const struct layout_tree *tree,
	layout_unit x,
	layout_unit y,
	int what)
{
	const struct layout_box *found;
	const struct layout_box *layer;
	struct wb_vector order;
	size_t flow_index;
	size_t index;
	int error;

	/* The positioned boxes in painting order (without them, only the flow is searched). */
	wb_vector_init(&order, sizeof(const struct layout_box *));
	error = layout_stacking_order(tree, &order, &flow_index);
	if (error != 0) {
		wb_vector_clear(&order);
		flow_index = 0;
	}

	/* From the top: the layers above the flow, the flow, then the layers below it. */
	found = NULL;
	for (index = order.count; found == NULL && index > flow_index; index--) {
		layer = *(const struct layout_box **)wb_vector_at(&order, index - 1U);
		found = hit_in(layer, x, y, what);
	}

	/* The flow, then the layers under it. */
	if (found == NULL)
		found = hit_in(tree->root, x, y, what);
	for (index = flow_index; found == NULL && index > 0; index--) {
		layer = *(const struct layout_box **)wb_vector_at(&order, index - 1U);
		found = hit_in(layer, x, y, what);
	}

	/* The box found, or NULL. */
	wb_vector_release(&order);
	return found;
}


/* Searches the floats and the inline blocks among a block's inline content for the text under a point. */
static const struct layout_box *
hit_inline_floats(
	const struct layout_box *box,
	layout_unit x,
	layout_unit y,
	int depth)
{
	const struct layout_box *child;
	const struct layout_box *found;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return NULL;

	/* A float or an inline block is searched like a block; an inline box is searched through. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow)
			continue;
		if (child->floating != CSS_FLOAT_NONE || child->atomic) {
			found = hit_box(child, x, y, depth + 1);
		} else {
			found = hit_inline_floats(child, x, y, depth + 1);
		}

		/* The first text found. */
		if (found != NULL)
			return found;
	}

	/* No float has text under the point. */
	return NULL;
}
