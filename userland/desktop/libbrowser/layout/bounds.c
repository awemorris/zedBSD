/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rectangle a node takes on the laid out page (ws074-p056): the union
 * of the border boxes of the blocks it makes and of the pieces of lines
 * its text and inline replaced boxes make.  The focus ring is drawn
 * around it, and a focused element is scrolled into view by it.
 *
 * An inline element makes no box of its own with a place (its text's
 * fragments have the places), so the lines of every block are searched for
 * the fragments of the node's descendants.
 */

#include "layout/layout.h"

static void bounds_walk(const struct layout_box *box, const struct dom_node *node, struct layout_rect *rect, int *found, int depth);
static void bounds_lines(const struct layout_box *box, const struct dom_node *node, struct layout_rect *rect, int *found);
static void bounds_add(struct layout_rect *rect, int *found, layout_unit x, layout_unit y, layout_unit width, layout_unit height);
static const struct layout_box *bounds_find(const struct layout_box *box, const struct dom_node *node, int depth);

/*
 * Finds a node's rectangle in layout units from the document's top left.
 *
 * Returns whether any native box exists; all empty boxes retain their first rectangle.
 */
int
layout_node_bounds(
	const struct layout_tree *tree,
	const struct dom_node *node,
	struct layout_rect *rect)
{
	int found;

	/* Nothing is found until a box is. */
	found = 0;
	rect->x = 0;
	rect->y = 0;
	rect->width = 0;
	rect->height = 0;

	/* The whole box tree. */
	if (tree->root != NULL)
		bounds_walk(tree->root, node, rect, &found, 0);

	/* No native box is different from a first empty rectangle preserved by the walk. */
	if (found == 0)
		return 0;

	/* Succeeded: a positive union or actual first empty fragment supplied geometry. */
	return 1;
}

/*
 * Finds the first native box of a node in tree order, or NULL when absent.
 *
 * The caret measures its form control through this first box's used style.
 */
const struct layout_box *
layout_box_of(
	const struct layout_tree *tree,
	const struct dom_node *node)
{
	const struct layout_box *found;

	/* The whole box tree. */
	found = NULL;
	if (tree->root != NULL)
		found = bounds_find(tree->root, node, 0);

	/* Succeeded: reports the first actual box or normal absence. */
	return found;
}

/* Searches a box and its descendants for the first box of a node. */
static const struct layout_box *
bounds_find(
	const struct layout_box *box,
	const struct dom_node *node,
	int depth)
{
	const struct layout_box *child;
	const struct layout_box *found;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return NULL;

	/* The box itself. */
	if (box->node == node)
		return box;

	/* Its children in order. */
	for (child = box->first_child;
	     child != NULL;
	     child = child->next) {
		found = bounds_find(child, node, depth + 1);
		if (found != NULL)
			return found;
	}

	/* Succeeded: this bounded native subtree has no box for the requested node. */
	return NULL;
}

/* Adds a box's part of a node's rectangle (its border box, or its lines' fragments), and its children's. */
static void
bounds_walk(
	const struct layout_box *box,
	const struct dom_node *node,
	struct layout_rect *rect,
	int *found,
	int depth)
{
	const struct layout_box *child;
	layout_unit width;
	layout_unit height;
	int inside;
	int in_line;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return;

	/* A block of the node or of its descendants adds its border box. */
	inside = 0;
	if (box->node != NULL)
		inside = dom_is_inclusive_ancestor(node, box->node);

	/* A replaced box among inline content is a piece of its line, which the lines add. */
	in_line = 0;
	if (box->kind == LAYOUT_REPLACED &&
	    box->parent != NULL &&
	    box->parent->children_inline)
		in_line = 1;

	/* The blocks and the replaced boxes that stand as blocks have places of their own. */
	if (inside &&
	    !in_line &&
	    (box->kind == LAYOUT_BLOCK || box->kind == LAYOUT_REPLACED)) {
		width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
		height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		bounds_add(rect, found, box->x, box->y, width, height);
	}

	/* A block of lines adds the fragments of the node's text. */
	if (box->children_inline)
		bounds_lines(box, node, rect, found);

	/* The children, which may hold more of the node (floats and positioned boxes among inline content too). */
	for (child = box->first_child;
	     child != NULL;
	     child = child->next)
		bounds_walk(child, node, rect, found, depth + 1);

	/* Succeeded: every bounded descendant supplied its native box or line geometry. */
	return;
}

/* Adds the fragments of a block's lines that belong to a node or its descendants. */
static void
bounds_lines(
	const struct layout_box *box,
	const struct dom_node *node,
	struct layout_rect *rect,
	int *found)
{
	const struct layout_line *line;
	const struct layout_fragment *fragment;
	layout_unit left;
	layout_unit top;
	layout_unit x;
	layout_unit y;
	size_t index;
	size_t item;
	int inside;

	/* The content box's origin, which the lines are placed from. */
	left = box->x + box->border[CSS_LEFT] + box->padding[CSS_LEFT];
	top = box->y + box->border[CSS_TOP] + box->padding[CSS_TOP];

	/* Each fragment of each line whose box is the node's. */
	for (index = 0; index < box->line_count; index++) {
		line = &box->lines[index];

		/* Each actual fragment may contribute only its selected native node descendants. */
		for (item = 0; item < line->fragment_count; item++) {
			fragment = &line->fragments[item];

			/* A fragment of another node's text is passed by. */
			inside = 0;
			if (fragment->box != NULL && fragment->box->node != NULL)
				inside = dom_is_inclusive_ancestor(node, fragment->box->node);
			if (!inside)
				continue;

			/* Vertical lines advance in physical y and occupy their line's physical x band. */
			if (box->vertical_inline) {
				x = left + line->y;
				y = top + line->left + fragment->x;
				bounds_add(rect, found, x, y, line->height, fragment->width);
				continue;
			}

			/* The fragment's run, from its ascent above the baseline to its descent below. */
			x = left + line->left + fragment->x;
			y = top + line->y + line->baseline + fragment->shift - fragment->ascent;

			/* An inline replaced box's piece is its margin box: the box itself is inside the margins. */
			if (fragment->box != NULL && fragment->box->kind == LAYOUT_REPLACED && fragment->box->node == node) {
				bounds_add(rect, found, x + fragment->box->margin[CSS_LEFT], y + fragment->box->margin[CSS_TOP],
				    fragment->width - fragment->box->margin[CSS_LEFT] - fragment->box->margin[CSS_RIGHT],
				    fragment->ascent + fragment->descent - fragment->box->margin[CSS_TOP] - fragment->box->margin[CSS_BOTTOM]);
				continue;
			}
			bounds_add(rect, found, x, y, fragment->width, fragment->ascent + fragment->descent);
		}
	}

	/* Succeeded: all actual line fragments were considered without creating boxes. */
	return;
}

/* Unions positive rectangles while preserving the first actual empty rectangle as a fallback. */
static void
bounds_add(
	struct layout_rect *rect,
	int *found,
	layout_unit x,
	layout_unit y,
	layout_unit width,
	layout_unit height)
{
	layout_unit right;
	layout_unit bottom;

	/* A negative native extent cannot supply a valid border rectangle. */
	if (width < 0 || height < 0)
		return;

	/* All-empty results keep the first actual fragment, without enlarging a positive union. */
	if (width == 0 || height == 0) {
		/* A negative marker records fallback geometry until a positive-area rectangle replaces it. */
		if (*found == 0) {
			rect->x = x;
			rect->y = y;
			rect->width = width;
			rect->height = height;
			*found = -1;
		}

		/* Later empty fragments cannot enlarge or replace the chosen rectangle. */
		return;
	}

	/* The first positive rectangle replaces any provisional empty fragment. */
	if (*found <= 0) {
		rect->x = x;
		rect->y = y;
		rect->width = width;
		rect->height = height;
		*found = 1;
		return;
	}

	/* The union's far edges. */
	right = rect->x + rect->width;
	bottom = rect->y + rect->height;
	if (x + width > right)
		right = x + width;

	/* The farther vertical edge expands the accumulated positive union. */
	if (y + height > bottom)
		bottom = y + height;

	/* Its near edges, and the size between them. */
	if (x < rect->x)
		rect->x = x;

	/* The smaller vertical origin preserves all accumulated positive rectangles. */
	if (y < rect->y)
		rect->y = y;

	/* Publishes the extents between the final near and far edges. */
	rect->width = right - rect->x;
	rect->height = bottom - rect->y;

	/* Succeeded: the positive union includes this native rectangle. */
	return;
}
