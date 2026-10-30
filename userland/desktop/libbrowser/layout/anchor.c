/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Anchor positioning: the insets and sizes of a box out of the flow that
 * name the place or the size of another box (its anchor) are finished
 * when the box is placed, after the boxes before it.
 *
 * An anchor is the last box in tree order with the anchor name that is
 * not the box or inside it, that is inside the box's containing block
 * (unless that is the viewport's), and that, when it is out of the flow
 * itself, comes before the box in tree order or has a containing block
 * of its own (it is placed first then).  An anchor() of an inset is the distance from
 * the containing block's edge on the inset's side to the anchor's side it
 * asks for; anchor-size() is the anchor's border box size.  A function
 * without an anchor, or whose side is not on the inset's axis, takes its
 * fallback; without one the whole value is invalid (an inset is auto, a
 * size its initial value).
 * https://drafts.csswg.org/css-anchor-position-1/#acceptable-anchor-element
 * https://drafts.csswg.org/css-anchor-position-1/#anchor-pos
 */

#include "layout/layout.h"

#include <string.h>

static int anchor_value(const struct layout_tree *tree, const struct layout_box *box, const struct css_anchor_expr *expr, int slot, layout_unit cb_start, layout_unit cb_size, layout_unit *value);
static const struct css_style *anchor_cb_style(const struct layout_tree *tree, const struct layout_box *box);
static int anchor_axis_reversed(const struct css_style *style, int vertical);
static int anchor_query(const struct layout_tree *tree, const struct layout_box *box, const struct css_anchor_query *query, int slot, layout_unit cb_start, layout_unit cb_size, layout_unit *value);
static int anchor_find(const struct layout_tree *tree, const struct layout_box *box, const struct vm_string *name, struct layout_rect *rect);
static int anchor_find_state(const struct layout_tree *tree, const struct layout_box *box, const struct vm_string *name, struct layout_rect *rect, const struct layout_box **anchor, int *clipped);
static void anchor_search(const struct layout_tree *tree, const struct layout_box *walk, const struct layout_box *box, const struct layout_box *containing, const struct vm_string *name, int inside, int *passed, const struct layout_box **found, int depth);
static int anchor_chain_before(const struct layout_box *candidate, const struct layout_box *box, const struct layout_box *containing);
static const struct layout_box *anchor_scope_root(const struct layout_box *box, const struct vm_string *name);
static const struct layout_box *anchor_up(const struct layout_box *box);
static int anchor_outside(const struct layout_box *scroller, const struct layout_rect *rect);
static int anchor_scrolls(const struct layout_box *box);
static int anchor_has_name(const struct layout_box *box, const struct vm_string *name);
static void anchor_flip(struct layout_tree *tree, struct layout_box *box, int how);
static const struct css_anchor_expr *anchor_turn(struct layout_tree *tree, const struct css_anchor_expr *expr, int how);
static layout_unit anchor_inset(const struct css_length *length, layout_unit size);
static void anchor_area_align(struct layout_box *box, int across, int align, int first, int last, layout_unit start, layout_unit end, layout_unit anchor_start, layout_unit anchor_size);

/*
 * Finishes a box's insets and sizes that hold anchor functions against
 * its containing block (x, y, width and height in layout units from the
 * document's top left): each becomes pixels, or auto (none for a maximum)
 * when it is invalid.
 */
void
layout_anchor_resolve(
	const struct layout_tree *tree,
	struct layout_box *box,
	layout_unit x,
	layout_unit y,
	layout_unit width,
	layout_unit height)
{
	struct css_length *length;
	layout_unit start;
	layout_unit size;
	layout_unit value;
	int positioned;
	int valid;
	int slot;

	/* Only a box out of the flow uses anchor functions. */
	positioned = box->style.position == CSS_POSITION_ABSOLUTE || box->style.position == CSS_POSITION_FIXED;
	for (slot = 0; slot < CSS_ANCHOR_SLOTS; slot++) {
		/* The length of the slot. */
		switch (slot) {
		case CSS_ANCHOR_SLOT_WIDTH:
			length = &box->style.width;
			break;
		case CSS_ANCHOR_SLOT_HEIGHT:
			length = &box->style.height;
			break;
		case CSS_ANCHOR_SLOT_MIN_WIDTH:
			length = &box->style.min_width;
			break;
		case CSS_ANCHOR_SLOT_MIN_HEIGHT:
			length = &box->style.min_height;
			break;
		case CSS_ANCHOR_SLOT_MAX_WIDTH:
			length = &box->style.max_width;
			break;
		case CSS_ANCHOR_SLOT_MAX_HEIGHT:
			length = &box->style.max_height;
			break;
		case CSS_ANCHOR_SLOT_MARGIN_TOP:
		case CSS_ANCHOR_SLOT_MARGIN_RIGHT:
		case CSS_ANCHOR_SLOT_MARGIN_BOTTOM:
		case CSS_ANCHOR_SLOT_MARGIN_LEFT:
			length = &box->style.margin[slot - CSS_ANCHOR_SLOT_MARGIN_TOP];
			break;
		default:
			length = &box->style.offset[slot];
			break;
		}
		if (length->unit != CSS_UNIT_ANCHOR)
			continue;

		/* The axis of the slot: the containing block's start and size along it. */
		start = x;
		size = width;
		if (slot == CSS_ANCHOR_SLOT_TOP || slot == CSS_ANCHOR_SLOT_BOTTOM || slot == CSS_ANCHOR_SLOT_HEIGHT ||
		    slot == CSS_ANCHOR_SLOT_MIN_HEIGHT || slot == CSS_ANCHOR_SLOT_MAX_HEIGHT ||
		    slot == CSS_ANCHOR_SLOT_MARGIN_TOP || slot == CSS_ANCHOR_SLOT_MARGIN_BOTTOM) {
			start = y;
			size = height;
		}

		/* The value in pixels, or the property's invalid value. */
		valid = 0;
		value = 0;
		if (positioned && box->style.anchor[slot] != NULL)
			valid = anchor_value(tree, box, box->style.anchor[slot], slot, start, size, &value);
		length->value = layout_to_px(value);
		length->offset = 0;
		length->unit = CSS_UNIT_PX;
		if (!valid)
			length->unit = slot == CSS_ANCHOR_SLOT_MAX_WIDTH || slot == CSS_ANCHOR_SLOT_MAX_HEIGHT ? CSS_UNIT_NONE : CSS_UNIT_AUTO;
		if (!valid && slot >= CSS_ANCHOR_SLOT_MARGIN_TOP) {
			length->unit = CSS_UNIT_PX;
			length->value = 0.0f;
		}
	}
}

/* Tells whether a box has a length that holds anchor functions. */
int
layout_anchor_wanted(
	const struct layout_box *box)
{
	int side;

	/* The insets, then the sizes. */
	for (side = 0; side < 4; side++) {
		if (box->style.offset[side].unit == CSS_UNIT_ANCHOR || box->style.margin[side].unit == CSS_UNIT_ANCHOR)
			return 1;
	}
	return box->style.width.unit == CSS_UNIT_ANCHOR || box->style.height.unit == CSS_UNIT_ANCHOR ||
	    box->style.min_width.unit == CSS_UNIT_ANCHOR || box->style.min_height.unit == CSS_UNIT_ANCHOR ||
	    box->style.max_width.unit == CSS_UNIT_ANCHOR || box->style.max_height.unit == CSS_UNIT_ANCHOR;
}

/* Works out a sum: its pixels, its percentage of the containing block, and its anchor functions. */
static int
anchor_value(
	const struct layout_tree *tree,
	const struct layout_box *box,
	const struct css_anchor_expr *expr,
	int slot,
	layout_unit cb_start,
	layout_unit cb_size,
	layout_unit *value)
{
	layout_unit total;
	layout_unit part;
	int index;
	int valid;

	/* The lengths. */
	total = layout_from_px(expr->px) + (layout_unit)((float)cb_size * expr->percent / 100.0f);

	/* Each function, or its fallback. */
	for (index = 0; index < expr->count; index++) {
		valid = anchor_query(tree, box, &expr->terms[index], slot, cb_start, cb_size, &part);
		if (!valid) {
			if (!expr->terms[index].has_fallback)
				return 0;
			part = layout_from_px(expr->terms[index].fallback_px) +
			    (layout_unit)((float)cb_size * expr->terms[index].fallback_percent / 100.0f);
		}
		total += (layout_unit)((float)part * expr->terms[index].coef);
	}

	/* Succeeded: the value is known. */
	*value = total;
	return 1;
}

/* The style whose writing mode the logical keywords of a box's anchor functions follow: its containing block's. */
static const struct css_style *
anchor_cb_style(
	const struct layout_tree *tree,
	const struct layout_box *box)
{
	const struct layout_box *containing;

	containing = layout_containing_box(box);
	if (containing != NULL)
		return &containing->style;
	return &tree->root->style;
}

/* Tells whether an axis (vertical or horizontal) of a style's writing mode runs against the page's (up or leftward). */
static int
anchor_axis_reversed(
	const struct css_style *style,
	int vertical)
{
	int rtl;

	rtl = style->direction == CSS_DIRECTION_RTL;
	if (style->writing_mode == CSS_WRITING_HORIZONTAL_TB)
		return vertical ? 0 : rtl;

	/* A vertical mode's block axis is horizontal (leftward for rl), its inline axis vertical (upward for rtl and sideways-lr). */
	if (!vertical)
		return style->writing_mode == CSS_WRITING_VERTICAL_RL;
	return rtl != (style->writing_sideways == 1);
}

/* Works out one anchor() or anchor-size() for a slot (0 when it has no valid answer). */
static int
anchor_query(
	const struct layout_tree *tree,
	const struct layout_box *box,
	const struct css_anchor_query *query,
	int slot,
	layout_unit cb_start,
	layout_unit cb_size,
	layout_unit *value)
{
	struct layout_rect rect;
	layout_unit edge;
	layout_unit anchor_start;
	layout_unit anchor_size;
	const struct css_style *own_style;
	int vertical;
	int reversed;
	int side;
	int far;
	int found;

	/* The anchor. */
	vertical = slot == CSS_ANCHOR_SLOT_TOP || slot == CSS_ANCHOR_SLOT_BOTTOM || slot == CSS_ANCHOR_SLOT_HEIGHT ||
	    slot == CSS_ANCHOR_SLOT_MIN_HEIGHT || slot == CSS_ANCHOR_SLOT_MAX_HEIGHT ||
	    slot == CSS_ANCHOR_SLOT_MARGIN_TOP || slot == CSS_ANCHOR_SLOT_MARGIN_BOTTOM;
	found = anchor_find(tree, box, query->name != NULL ? query->name : box->style.position_anchor, &rect);
	if (!found)
		return 0;

	/* Whether the slot's inset is measured from the far edge (right or bottom). */
	far = slot == CSS_ANCHOR_SLOT_RIGHT || slot == CSS_ANCHOR_SLOT_BOTTOM;
	anchor_start = vertical ? rect.y : rect.x;
	anchor_size = vertical ? rect.height : rect.width;

	/* anchor-size(): the anchor's size on the asked axis (the slot's own axis by default). */
	side = query->side;
	if (side >= CSS_ANCHOR_WIDTH) {
		if (side == CSS_ANCHOR_SIZE_DEFAULT)
			side = vertical ? CSS_ANCHOR_HEIGHT : CSS_ANCHOR_WIDTH;
		/* The block and inline sizes follow the containing block's writing mode (the box's own for self-). */
		if (side == CSS_ANCHOR_BLOCK || side == CSS_ANCHOR_SELF_BLOCK || side == CSS_ANCHOR_INLINE || side == CSS_ANCHOR_SELF_INLINE) {
			own_style = (side == CSS_ANCHOR_SELF_BLOCK || side == CSS_ANCHOR_SELF_INLINE) ? &box->style : anchor_cb_style(tree, box);
			side = ((side == CSS_ANCHOR_BLOCK || side == CSS_ANCHOR_SELF_BLOCK) ==
			    (own_style->writing_mode == CSS_WRITING_HORIZONTAL_TB)) ? CSS_ANCHOR_HEIGHT : CSS_ANCHOR_WIDTH;
		}
		*value = side == CSS_ANCHOR_HEIGHT ? rect.height : rect.width;
		return 1;
	}

	/* anchor() is only for the insets. */
	if (slot > CSS_ANCHOR_SLOT_LEFT)
		return 0;

	/* The anchor's edge the side names along the slot's axis. */
	switch (side) {
	case CSS_ANCHOR_TOP:
	case CSS_ANCHOR_BOTTOM:
		if (!vertical)
			return 0;
		edge = anchor_start + (side == CSS_ANCHOR_BOTTOM ? anchor_size : 0);
		break;
	case CSS_ANCHOR_LEFT:
	case CSS_ANCHOR_RIGHT:
		if (vertical)
			return 0;
		edge = anchor_start + (side == CSS_ANCHOR_RIGHT ? anchor_size : 0);
		break;
	case CSS_ANCHOR_START:
	case CSS_ANCHOR_SELF_START:
		reversed = anchor_axis_reversed(side == CSS_ANCHOR_SELF_START ? &box->style : anchor_cb_style(tree, box), vertical);
		edge = anchor_start + (reversed ? anchor_size : 0);
		break;
	case CSS_ANCHOR_END:
	case CSS_ANCHOR_SELF_END:
		reversed = anchor_axis_reversed(side == CSS_ANCHOR_SELF_END ? &box->style : anchor_cb_style(tree, box), vertical);
		edge = anchor_start + (reversed ? 0 : anchor_size);
		break;
	case CSS_ANCHOR_CENTER:
		edge = anchor_start + anchor_size / 2;
		break;
	case CSS_ANCHOR_PERCENT:
		reversed = anchor_axis_reversed(anchor_cb_style(tree, box), vertical);
		edge = anchor_start + (layout_unit)((float)anchor_size * (reversed ? 100.0f - query->percent : query->percent) / 100.0f);
		break;
	case CSS_ANCHOR_INSIDE:
		edge = anchor_start + (far ? anchor_size : 0);
		break;
	case CSS_ANCHOR_OUTSIDE:
		edge = anchor_start + (far ? 0 : anchor_size);
		break;
	default:
		return 0;
	}

	/* The distance from the containing block's edge on the inset's side. */
	*value = far ? cb_start + cb_size - edge : edge - cb_start;
	return 1;
}

/* Finds the anchor of a name for a box and its border box (0 when there is none). */
static int
anchor_find(
	const struct layout_tree *tree,
	const struct layout_box *box,
	const struct vm_string *name,
	struct layout_rect *rect)
{
	return anchor_find_state(tree, box, name, rect, NULL, NULL);
}

/* Finds the anchor as anchor_find does, and tells the box it is and whether a scroll container it is in (the box's containing block is not) clips it away. */
static int
anchor_find_state(
	const struct layout_tree *tree,
	const struct layout_box *box,
	const struct vm_string *name,
	struct layout_rect *rect,
	const struct layout_box **anchor,
	int *clipped)
{
	const struct layout_box *containing;
	const struct layout_box *found;
	const struct layout_box *candidates[1];
	const struct layout_box *up;
	const struct layout_box *inner;
	struct layout_rect original;
	double scroll_x;
	double scroll_y;
	int passed;

	/* A name, and the tree. */
	if (name == NULL || tree->root == NULL)
		return 0;

	/* The last acceptable box with the name in tree order. */
	containing = layout_containing_box(box);
	candidates[0] = NULL;
	passed = 0;
	anchor_search(tree, tree->root, box, containing, name, containing == NULL, &passed, candidates, 0);

	found = candidates[0];
	if (found == NULL || found->node == NULL)
		return 0;

	/* An anchor out of the flow that is not placed yet is placed first. */
	if (found->out_of_flow && found->placed_pass != tree->position_pass &&
	    layout_position_box((struct layout_tree *)tree, (struct layout_box *)found) != 0)
		return 0;

	/* Its rectangle: a block's border box, an inline box's lines. */
	if (found->kind == LAYOUT_BLOCK || found->kind == LAYOUT_REPLACED || found->atomic) {
		rect->x = found->x;
		rect->y = found->y;
		rect->width = found->border[CSS_LEFT] + found->padding[CSS_LEFT] + found->width +
		    found->padding[CSS_RIGHT] + found->border[CSS_RIGHT];
		rect->height = found->border[CSS_TOP] + found->padding[CSS_TOP] + found->height +
		    found->padding[CSS_BOTTOM] + found->border[CSS_BOTTOM];
	} else if (!layout_node_bounds(tree, found->node, rect)) {
		return 0;
	}

	/* Where the scroll puts the anchor: by the scroll of the scroll containers it is in that the box's containing block is not in. */
	original = *rect;
	for (up = anchor_up(found); up != NULL; up = anchor_up(up)) {
		if (!layout_scroll_offset(up, &scroll_x, &scroll_y))
			continue;
		for (inner = containing; inner != NULL && inner != up; inner = inner->parent)
			;
		if (inner != NULL)
			continue;
		rect->x -= layout_from_px((float)scroll_x);
		rect->y -= layout_from_px((float)scroll_y);
	}

	/* Whether a scroll container in between shows none of it. */
	if (anchor != NULL)
		*anchor = found;
	if (clipped != NULL) {
		*clipped = 0;
		for (up = anchor_up(found); up != NULL; up = anchor_up(up)) {
			if (!anchor_scrolls(up) || up->parent == NULL)
				continue;
			for (inner = containing; inner != NULL && inner != up; inner = inner->parent)
				;
			if (inner != NULL)
				continue;
			if (layout_scroll_offset(up, &scroll_x, &scroll_y)) {
				original.x -= layout_from_px((float)scroll_x);
				original.y -= layout_from_px((float)scroll_y);
			}
			if (anchor_outside(up, &original))
				*clipped = 1;
		}
	}
	return 1;
}

/* The next box up a box's chain of containing blocks: its parent, or for a box out of the flow where it is placed (NULL at the top). */
static const struct layout_box *
anchor_up(
	const struct layout_box *box)
{
	if (box->out_of_flow)
		return layout_containing_box(box);
	return box->parent;
}

/* Tells whether a rectangle (in the frame the scroll container's own box is in) lies wholly outside the container's padding box. */
static int
anchor_outside(
	const struct layout_box *scroller,
	const struct layout_rect *rect)
{
	layout_unit left;
	layout_unit top;
	layout_unit right;
	layout_unit bottom;

	/* The padding box, which clips what scrolls in it. */
	left = scroller->x + scroller->border[CSS_LEFT];
	top = scroller->y + scroller->border[CSS_TOP];
	right = left + scroller->padding[CSS_LEFT] + scroller->width + scroller->padding[CSS_RIGHT];
	bottom = top + scroller->padding[CSS_TOP] + scroller->height + scroller->padding[CSS_BOTTOM];
	return rect->x + rect->width <= left || rect->x >= right || rect->y + rect->height <= top || rect->y >= bottom;
}

/*
 * Tells whether position-visibility hides a box out of the flow: with anchors-visible, its default anchor is
 * clipped away by a scroll container in between or is hidden itself.
 */
int
layout_anchor_hides(
	const struct layout_tree *tree,
	const struct layout_box *box)
{
	struct layout_rect rect;
	const struct layout_box *anchor;
	const struct layout_box *up;
	int clipped;

	/* Only anchors-visible and only for a box with a default anchor. */
	if ((box->style.position_visibility & 2) == 0 || box->style.position_anchor == NULL)
		return 0;
	anchor = NULL;
	clipped = 0;
	if (!anchor_find_state(tree, box, box->style.position_anchor, &rect, &anchor, &clipped) || anchor == NULL)
		return 0;
	for (up = anchor; up != NULL; up = up->parent) {
		if (up->anchor_hidden)
			return 1;
	}
	return clipped;
}

/*
 * Searches the boxes under walk for anchors of a name: inside the
 * containing block (inside tells whether walk is), never the box or what
 * is inside it, and a box out of the flow only before the box in tree
 * order or with a containing block of its own (it is placed first then).
 */
static void
anchor_search(
	const struct layout_tree *tree,
	const struct layout_box *walk,
	const struct layout_box *box,
	const struct layout_box *containing,
	const struct vm_string *name,
	int inside,
	int *passed,
	const struct layout_box **found,
	int depth)
{
	const struct layout_box *child;
	int child_inside;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return;

	/* The box and what is inside it are passed, not searched. */
	for (child = walk->first_child; child != NULL; child = child->next) {
		if (child == box) {
			*passed = 1;
			continue;
		}
		child_inside = inside || walk == containing;
		if (child_inside && anchor_has_name(child, name) && anchor_scope_root(child, name) == anchor_scope_root(box, name) &&
		    anchor_chain_before(child, box, containing) &&
		    (!child->out_of_flow || !*passed || (layout_containing_box(child) != containing && !anchor_scrolls(layout_containing_box(child)))))
			found[0] = child;
		anchor_search(tree, child, box, containing, name, child_inside, passed, found, depth + 1);
	}
}

/*
 * Tells whether the last box of a candidate's chain of containing blocks below the box's containing block,
 * when it is out of the flow, comes before the box in tree order (true when it is not or is the candidate).
 */
static int
anchor_chain_before(
	const struct layout_box *candidate,
	const struct layout_box *box,
	const struct layout_box *containing)
{
	const struct layout_box *up;
	const struct layout_box *outer;
	const struct layout_box *left;
	const struct layout_box *right;
	const struct layout_box *walk;

	/* The last box of the candidate's chain of containing blocks before the box's containing block. */
	outer = candidate;
	for (up = candidate; up != NULL; ) {
		up = up->out_of_flow ? layout_containing_box(up) : up->parent;
		if (up == NULL || up == containing)
			break;
		outer = up;
	}
	if (!outer->out_of_flow || outer == candidate)
		return 1;

	/* An ancestor of the box (or the box) is before it. */
	for (up = box; up != NULL; up = up->parent) {
		if (up == outer)
			return 1;
	}

	/* Otherwise the first of the two siblings under their closest common ancestor decides. */
	for (left = outer; left != NULL; left = left->parent) {
		for (right = box; right != NULL && right->parent != left->parent; right = right->parent)
			;
		if (right == NULL)
			continue;
		if (left == right)
			return 1;
		for (walk = left->parent != NULL ? left->parent->first_child : NULL; walk != NULL; walk = walk->next) {
			if (walk == left)
				return 1;
			if (walk == right)
				return 0;
		}
		return 1;
	}
	return 1;
}

/*
 * Finds the nearest box (itself included) whose anchor-scope scopes a name: only boxes under the same
 * scope can see each other's anchors of that name (NULL when none scopes it).
 */
static const struct layout_box *
anchor_scope_root(
	const struct layout_box *box,
	const struct vm_string *name)
{
	const struct css_anchor_names *scope;
	const struct layout_box *up;
	int index;

	/* The box and its ancestors. */
	for (up = box; up != NULL; up = up->parent) {
		if (up->style.anchor_scope_all)
			return up;
		scope = up->style.anchor_scope;
		if (scope == NULL)
			continue;
		for (index = 0; index < scope->count; index++) {
			if (scope->names[index] == name || vm_string_equal(scope->names[index], name))
				return up;
		}
	}
	return NULL;
}

/* Tells whether a box is a scroll container (an anchor out of the flow in one that follows its box in tree order is not taken). */
static int
anchor_scrolls(
	const struct layout_box *box)
{
	return box != NULL && (box->style.overflow_x != CSS_OVERFLOW_VISIBLE || box->style.overflow_y != CSS_OVERFLOW_VISIBLE);
}

/* Tells whether a box has an anchor name. */
static int
anchor_has_name(
	const struct layout_box *box,
	const struct vm_string *name)
{
	const struct css_anchor_names *names;
	int index;

	/* Each of its names (a text box shares its element's style, not its names). */
	names = box->style.anchor_names;
	if (names == NULL || box->kind == LAYOUT_TEXT)
		return 0;
	for (index = 0; index < names->count; index++) {
		if (names->names[index] == name || vm_string_equal(names->names[index], name))
			return 1;
	}
	return 0;
}

/* The box a box out of the flow is placed in (NULL for the viewport). */
const struct layout_box *
layout_containing_box(
	const struct layout_box *box)
{
	const struct layout_box *ancestor;

	/* The nearest ancestor that contains the box: transformed for a fixed box, positioned or transformed otherwise. */
	for (ancestor = box->parent; ancestor != NULL; ancestor = ancestor->parent) {
		if (box->style.position == CSS_POSITION_FIXED ? layout_contains_fixed(ancestor) : layout_contains_absolute(ancestor))
			return ancestor;

		/* A positioned inline box contains the boxes out of the flow inside it too. */
		if (box->style.position != CSS_POSITION_FIXED && ancestor->kind == LAYOUT_INLINE &&
		    ancestor->style.position != CSS_POSITION_STATIC)
			return ancestor;
	}
	return NULL;
}

/*
 * Places a box with a position-area: its containing block becomes the
 * area of the 3x3 grid that the anchor's edges and the containing block's
 * make, and along an axis whose insets are auto it is aligned there
 * (stretch fills the area; normal aligns toward the anchor, and centers
 * on it across span-all).  x, y, width and height are the containing
 * block's.
 */
void
layout_anchor_area(
	const struct layout_tree *tree,
	struct layout_box *box,
	layout_unit x,
	layout_unit y,
	layout_unit width,
	layout_unit height)
{
	struct layout_rect rect;
	layout_unit across[4];
	layout_unit down[4];
	layout_unit left;
	layout_unit right;
	layout_unit top;
	layout_unit bottom;
	int x0;
	int x1;
	int y0;
	int y1;
	int first;
	int found;

	/* The area's regions, and the anchor. */
	if (!css_position_area_regions(box->style.position_area, box->style.writing_mode, box->style.direction, &x0, &x1, &y0, &y1))
		return;
	found = anchor_find(tree, box, box->style.position_anchor, &rect);
	if (!found)
		return;

	/* As a fallback turned it: over the diagonal, then down and across. */
	if ((box->anchor_flip & 4) != 0) {
		first = x0;
		x0 = y0;
		y0 = first;
		first = x1;
		x1 = y1;
		y1 = first;
	}
	if ((box->anchor_flip & 1) != 0) {
		first = y0;
		y0 = 2 - y1;
		y1 = 2 - first;
	}
	if ((box->anchor_flip & 2) != 0) {
		first = x0;
		x0 = 2 - x1;
		x1 = 2 - first;
	}

	/* The grid's lines: the containing block's edges and the anchor's. */
	across[0] = x;
	across[1] = rect.x;
	across[2] = rect.x + rect.width;
	across[3] = x + width;
	down[0] = y;
	down[1] = rect.y;
	down[2] = rect.y + rect.height;
	down[3] = y + height;
	left = across[x0];
	right = across[x1 + 1];
	top = down[y0];
	bottom = down[y1 + 1];
	if (right < left)
		right = left;
	if (bottom < top)
		bottom = top;

	/* The area is the containing block. */
	box->area_bounds[0] = across[0];
	box->area_bounds[1] = across[3];
	box->area_bounds[2] = down[0];
	box->area_bounds[3] = down[3];
	box->cb_set = 1;
	box->cb_left = left;
	box->cb_right = right;
	box->cb_top = top;
	box->cb_bottom = bottom;
	box->cb_left_auto = 0;
	box->cb_right_auto = 0;
	box->cb_top_auto = 0;
	box->cb_bottom_auto = 0;

	/* Insets of zero against the area leave the box its alignment there, as auto ones do. */
	if (box->style.width.unit != CSS_UNIT_AUTO && box->style.offset[CSS_LEFT].unit == CSS_UNIT_PX && box->style.offset[CSS_LEFT].value == 0.0f &&
	    box->style.offset[CSS_RIGHT].unit == CSS_UNIT_PX && box->style.offset[CSS_RIGHT].value == 0.0f) {
		box->style.offset[CSS_LEFT].unit = CSS_UNIT_AUTO;
		box->style.offset[CSS_RIGHT].unit = CSS_UNIT_AUTO;
	}
	if (box->style.height.unit != CSS_UNIT_AUTO && box->style.offset[CSS_TOP].unit == CSS_UNIT_PX && box->style.offset[CSS_TOP].value == 0.0f &&
	    box->style.offset[CSS_BOTTOM].unit == CSS_UNIT_PX && box->style.offset[CSS_BOTTOM].value == 0.0f) {
		box->style.offset[CSS_TOP].unit = CSS_UNIT_AUTO;
		box->style.offset[CSS_BOTTOM].unit = CSS_UNIT_AUTO;
	}

	/* Each axis with auto insets is aligned in it. */
	if (box->style.offset[CSS_LEFT].unit == CSS_UNIT_AUTO && box->style.offset[CSS_RIGHT].unit == CSS_UNIT_AUTO)
		anchor_area_align(box, 1, box->style.justify_self, x0, x1, left, right, rect.x, rect.width);
	if (box->style.offset[CSS_TOP].unit == CSS_UNIT_AUTO && box->style.offset[CSS_BOTTOM].unit == CSS_UNIT_AUTO)
		anchor_area_align(box, 0, box->style.align_self, y0, y1, top, bottom, rect.y, rect.height);
}

/*
 * Aligns a box along one axis of its position-area (from start to end,
 * covering regions first to last), across when across is set.
 */
static void
anchor_area_align(
	struct layout_box *box,
	int across,
	int align,
	int first,
	int last,
	layout_unit start,
	layout_unit end,
	layout_unit anchor_start,
	layout_unit anchor_size)
{
	layout_unit center;
	layout_unit half;
	int side;

	/* stretch fills the area: zero insets from it. */
	if (align == CSS_ALIGN_STRETCH) {
		if (across) {
			box->style.offset[CSS_LEFT].unit = CSS_UNIT_PX;
			box->style.offset[CSS_LEFT].value = 0;
			box->style.offset[CSS_RIGHT] = box->style.offset[CSS_LEFT];
		} else {
			box->style.offset[CSS_TOP].unit = CSS_UNIT_PX;
			box->style.offset[CSS_TOP].value = 0;
			box->style.offset[CSS_BOTTOM] = box->style.offset[CSS_TOP];
		}
		return;
	}

	/* The side: normal aligns toward the anchor (center for the anchor's own region). */
	side = CSS_ALIGN_START;
	if (align == CSS_ALIGN_END || align == CSS_ALIGN_SELF_END || align == CSS_ALIGN_FLEX_END)
		side = CSS_ALIGN_END;
	else if (align == CSS_ALIGN_CENTER)
		side = CSS_ALIGN_CENTER;
	else if (align == CSS_ALIGN_AUTO || align == CSS_ALIGN_NORMAL) {
		if (last <= 1 && first == 0 && last != first)
			side = CSS_ALIGN_END;
		else if (last == 0)
			side = CSS_ALIGN_END;
		else if (first == 1 && last == 1)
			side = CSS_ALIGN_CENTER;
		else if (first == 0 && last == 2) {
			/* span-all: centered on the anchor, as far as the area goes both ways. */
			side = CSS_ALIGN_CENTER;
			center = anchor_start + anchor_size / 2;
			half = center - start < end - center ? center - start : end - center;
			if (half < 0)
				half = 0;
			start = center - half;
			end = center + half;
		}
	}

	/* A box that overflows its area stays inside the containing block (the one the area was cut from). */
	if (align == CSS_ALIGN_AUTO || align == CSS_ALIGN_NORMAL) {
		if (across) {
			box->area_clamp_x = 1;
			box->area_clamp[0] = box->area_bounds[0];
			box->area_clamp[1] = box->area_bounds[1];
		} else {
			box->area_clamp_y = 1;
			box->area_clamp[2] = box->area_bounds[2];
			box->area_clamp[3] = box->area_bounds[3];
		}
	}

	/* The static place: the area and the side. */
	if (across) {
		box->static_x = start;
		box->static_x_area_width = end - start;
		box->static_x_from_right = 0;
		box->static_x_align = side == CSS_ALIGN_START ? CSS_ALIGN_AUTO : side;
		box->static_x_align_from_right = side == CSS_ALIGN_END;
	} else {
		box->static_y = start;
		box->static_y_area_height = end - start;
		box->static_y_align = side == CSS_ALIGN_START ? CSS_ALIGN_AUTO : side;
	}
}

/*
 * Gives a box the style of one of its position-try-fallbacks entries
 * (its @position-try rule's properties, its position-area, then its
 * try-tactics in order: flip-block and flip-y swap the box's top and
 * bottom, flip-inline and flip-x its left and right, flip-start its top
 * with its left and its width with its height).  Returns 0 when the entry
 * has no style to try (a name no rule has).
 */
int
layout_anchor_try(
	struct layout_tree *tree,
	struct layout_box *box,
	int entry)
{
	const struct css_try_entry *item;
	const struct css_try_style *rule;
	struct css_style *style;
	int tactic;

	/* The entry. */
	style = &box->style;
	item = &style->position_try->entries[entry];
	if (item->name != NULL) {
		rule = style->try_styles[entry];
		if (rule == NULL)
			return 0;
		memcpy(style->offset, rule->offset, sizeof(style->offset));
		memcpy(style->margin, rule->margin, sizeof(style->margin));
		style->width = rule->width;
		style->height = rule->height;
		style->min_width = rule->min_width;
		style->min_height = rule->min_height;
		style->max_width = rule->max_width;
		style->max_height = rule->max_height;
		memcpy(style->anchor, rule->anchor, sizeof(style->anchor));
		style->position_area = rule->position_area;
		style->position_anchor = rule->position_anchor;
		style->justify_self = rule->justify_self;
		style->align_self = rule->align_self;
	}
	if (item->area >= 0)
		style->position_area = item->area;

	/* The tactics. */
	box->anchor_flip = 0;
	for (tactic = 0; tactic < item->tactic_count; tactic++) {
		switch (item->tactics[tactic]) {
		case 0:
		case 4:
			anchor_flip(tree, box, 0);
			break;
		case 1:
		case 3:
			anchor_flip(tree, box, 1);
			break;
		default:
			anchor_flip(tree, box, 2);
			break;
		}
	}

	/* Succeeded: the box has the entry's style. */
	return 1;
}

/* Flips a box's style down (0: top with bottom), across (1: left with right), or over the diagonal (2: top with left). */
static void
anchor_flip(
	struct layout_tree *tree,
	struct layout_box *box,
	int how)
{
	struct css_style *style;
	struct css_length length;
	const struct css_anchor_expr *expr;
	int pairs[6][2];
	int count;
	int index;
	int align;

	/* The sides and sizes that change places. */
	style = &box->style;
	if (how == 0) {
		pairs[0][0] = CSS_TOP;
		pairs[0][1] = CSS_BOTTOM;
		count = 1;
	} else if (how == 1) {
		pairs[0][0] = CSS_LEFT;
		pairs[0][1] = CSS_RIGHT;
		count = 1;
	} else {
		pairs[0][0] = CSS_TOP;
		pairs[0][1] = CSS_LEFT;
		pairs[1][0] = CSS_BOTTOM;
		pairs[1][1] = CSS_RIGHT;
		count = 2;
	}

	/* The insets (with their anchor functions turned) and the margins. */
	for (index = 0; index < count; index++) {
		length = style->offset[pairs[index][0]];
		style->offset[pairs[index][0]] = style->offset[pairs[index][1]];
		style->offset[pairs[index][1]] = length;
		expr = style->anchor[pairs[index][0]];
		style->anchor[pairs[index][0]] = anchor_turn(tree, style->anchor[pairs[index][1]], how);
		style->anchor[pairs[index][1]] = anchor_turn(tree, expr, how);
		length = style->margin[pairs[index][0]];
		style->margin[pairs[index][0]] = style->margin[pairs[index][1]];
		style->margin[pairs[index][1]] = length;
	}

	/* Over the diagonal the sizes change places too, and the two self-alignments. */
	if (how == 2) {
		length = style->width;
		style->width = style->height;
		style->height = length;
		length = style->min_width;
		style->min_width = style->min_height;
		style->min_height = length;
		length = style->max_width;
		style->max_width = style->max_height;
		style->max_height = length;
		expr = style->anchor[CSS_ANCHOR_SLOT_WIDTH];
		style->anchor[CSS_ANCHOR_SLOT_WIDTH] = anchor_turn(tree, style->anchor[CSS_ANCHOR_SLOT_HEIGHT], how);
		style->anchor[CSS_ANCHOR_SLOT_HEIGHT] = anchor_turn(tree, expr, how);
		expr = style->anchor[CSS_ANCHOR_SLOT_MIN_WIDTH];
		style->anchor[CSS_ANCHOR_SLOT_MIN_WIDTH] = anchor_turn(tree, style->anchor[CSS_ANCHOR_SLOT_MIN_HEIGHT], how);
		style->anchor[CSS_ANCHOR_SLOT_MIN_HEIGHT] = anchor_turn(tree, expr, how);
		expr = style->anchor[CSS_ANCHOR_SLOT_MAX_WIDTH];
		style->anchor[CSS_ANCHOR_SLOT_MAX_WIDTH] = anchor_turn(tree, style->anchor[CSS_ANCHOR_SLOT_MAX_HEIGHT], how);
		style->anchor[CSS_ANCHOR_SLOT_MAX_HEIGHT] = anchor_turn(tree, expr, how);
		align = style->align_self;
		style->align_self = style->justify_self;
		style->justify_self = align;
	} else {
		/* The self-alignment along the flipped axis turns round. */
		align = how == 0 ? style->align_self : style->justify_self;
		if (align == CSS_ALIGN_START)
			align = CSS_ALIGN_END;
		else if (align == CSS_ALIGN_END)
			align = CSS_ALIGN_START;
		else if (align == CSS_ALIGN_SELF_START)
			align = CSS_ALIGN_SELF_END;
		else if (align == CSS_ALIGN_SELF_END)
			align = CSS_ALIGN_SELF_START;
		if (how == 0)
			style->align_self = align;
		else
			style->justify_self = align;
	}

	/* A position-area turns the same way. */
	if (how == 2)
		box->anchor_flip = ((box->anchor_flip & 1) << 1) | ((box->anchor_flip & 2) >> 1) | ((box->anchor_flip & 4) ^ 4);
	else
		box->anchor_flip ^= how == 0 ? 1 : 2;
}

/* Turns the anchor functions of a sum the way a flip turns the box (a copy in the tree's arena; NULL stays NULL). */
static const struct css_anchor_expr *
anchor_turn(
	struct layout_tree *tree,
	const struct css_anchor_expr *expr,
	int how)
{
	static const int down[CSS_ANCHOR_SIZE_DEFAULT + 1] = {
		CSS_ANCHOR_BOTTOM, CSS_ANCHOR_RIGHT, CSS_ANCHOR_TOP, CSS_ANCHOR_LEFT, CSS_ANCHOR_END, CSS_ANCHOR_START,
		CSS_ANCHOR_SELF_END, CSS_ANCHOR_SELF_START, CSS_ANCHOR_CENTER, CSS_ANCHOR_PERCENT, CSS_ANCHOR_INSIDE,
		CSS_ANCHOR_OUTSIDE, CSS_ANCHOR_WIDTH, CSS_ANCHOR_HEIGHT, CSS_ANCHOR_BLOCK, CSS_ANCHOR_INLINE,
		CSS_ANCHOR_SELF_BLOCK, CSS_ANCHOR_SELF_INLINE, CSS_ANCHOR_SIZE_DEFAULT
	};
	static const int across[CSS_ANCHOR_SIZE_DEFAULT + 1] = {
		CSS_ANCHOR_TOP, CSS_ANCHOR_LEFT, CSS_ANCHOR_BOTTOM, CSS_ANCHOR_RIGHT, CSS_ANCHOR_END, CSS_ANCHOR_START,
		CSS_ANCHOR_SELF_END, CSS_ANCHOR_SELF_START, CSS_ANCHOR_CENTER, CSS_ANCHOR_PERCENT, CSS_ANCHOR_INSIDE,
		CSS_ANCHOR_OUTSIDE, CSS_ANCHOR_WIDTH, CSS_ANCHOR_HEIGHT, CSS_ANCHOR_BLOCK, CSS_ANCHOR_INLINE,
		CSS_ANCHOR_SELF_BLOCK, CSS_ANCHOR_SELF_INLINE, CSS_ANCHOR_SIZE_DEFAULT
	};
	static const int diagonal[CSS_ANCHOR_SIZE_DEFAULT + 1] = {
		CSS_ANCHOR_LEFT, CSS_ANCHOR_BOTTOM, CSS_ANCHOR_RIGHT, CSS_ANCHOR_TOP, CSS_ANCHOR_START, CSS_ANCHOR_END,
		CSS_ANCHOR_SELF_START, CSS_ANCHOR_SELF_END, CSS_ANCHOR_CENTER, CSS_ANCHOR_PERCENT, CSS_ANCHOR_INSIDE,
		CSS_ANCHOR_OUTSIDE, CSS_ANCHOR_HEIGHT, CSS_ANCHOR_WIDTH, CSS_ANCHOR_INLINE, CSS_ANCHOR_BLOCK,
		CSS_ANCHOR_SELF_INLINE, CSS_ANCHOR_SELF_BLOCK, CSS_ANCHOR_SIZE_DEFAULT
	};
	struct css_anchor_expr *turned;
	const int *map;
	int index;

	/* Nothing to turn. */
	if (expr == NULL)
		return NULL;
	turned = wb_arena_alloc(&tree->arena, sizeof(*turned));
	if (turned == NULL)
		return expr;
	*turned = *expr;

	/* Each function's side (a percentage from the other end when the axis turns round). */
	map = how == 0 ? down : how == 1 ? across : diagonal;
	for (index = 0; index < turned->count; index++) {
		if (turned->terms[index].side < 0 || turned->terms[index].side > CSS_ANCHOR_SIZE_DEFAULT)
			continue;
		if (turned->terms[index].side == CSS_ANCHOR_PERCENT && how != 2)
			turned->terms[index].percent = 100.0f - turned->terms[index].percent;
		turned->terms[index].side = map[turned->terms[index].side];
	}
	return turned;
}

/*
 * Centers a box with anchor-center on its anchor along each axis: the
 * containing block less the axis's insets (auto as zero) is where it is
 * sized and kept, and its static place is centered on the anchor's center
 * (x, y, width and height are the containing block's).
 */
void
layout_anchor_center(
	const struct layout_tree *tree,
	struct layout_box *box,
	layout_unit x,
	layout_unit y,
	layout_unit width,
	layout_unit height)
{
	struct layout_rect rect;
	layout_unit room;
	layout_unit start;
	layout_unit end;
	int across;
	int down;

	/* The axes that center, and the anchor. */
	box->anchor_clamp_x = 0;
	box->anchor_clamp_y = 0;
	across = box->style.justify_self == CSS_ALIGN_ANCHOR_CENTER;
	down = box->style.align_self == CSS_ALIGN_ANCHOR_CENTER;
	if ((!across && !down) || box->style.position_area >= 0)
		return;
	if (!anchor_find(tree, box, box->style.position_anchor, &rect))
		return;
	room = (width > height ? width : height) * 4 + LAYOUT_UNIT * 1024;
	if (!box->cb_set) {
		box->cb_set = 1;
		box->cb_left_auto = 1;
		box->cb_right_auto = 1;
		box->cb_top_auto = 1;
		box->cb_bottom_auto = 1;
	}

	/* Across: the room between the insets, and a wide area centered on the anchor. */
	if (across) {
		start = x + anchor_inset(&box->style.offset[CSS_LEFT], width);
		end = x + width - anchor_inset(&box->style.offset[CSS_RIGHT], width);
		box->cb_left = start;
		box->cb_right = end;
		box->cb_left_auto = 0;
		box->cb_right_auto = 0;
		box->style.offset[CSS_LEFT].unit = CSS_UNIT_AUTO;
		box->style.offset[CSS_RIGHT].unit = CSS_UNIT_AUTO;
		box->static_x = rect.x + rect.width / 2 - room / 2;
		box->static_x_area_width = room;
		box->static_x_from_right = 0;
		box->static_x_align = CSS_ALIGN_CENTER;
		box->static_x_align_from_right = 0;
		box->anchor_clamp_x = 1;
		box->anchor_clamp[0] = start;
		box->anchor_clamp[1] = end;
	}

	/* Down, the same. */
	if (down) {
		start = y + anchor_inset(&box->style.offset[CSS_TOP], height);
		end = y + height - anchor_inset(&box->style.offset[CSS_BOTTOM], height);
		box->cb_top = start;
		box->cb_bottom = end;
		box->cb_top_auto = 0;
		box->cb_bottom_auto = 0;
		box->style.offset[CSS_TOP].unit = CSS_UNIT_AUTO;
		box->style.offset[CSS_BOTTOM].unit = CSS_UNIT_AUTO;
		box->static_y = rect.y + rect.height / 2 - room / 2;
		box->static_y_area_height = room;
		box->static_y_align = CSS_ALIGN_CENTER;
		box->anchor_clamp_y = 1;
		box->anchor_clamp[2] = start;
		box->anchor_clamp[3] = end;
	}
}

/* An inset in layout units (a percentage of a size; auto is zero). */
static layout_unit
anchor_inset(
	const struct css_length *length,
	layout_unit size)
{
	/* Pixels, a percentage, or nothing. */
	if (length->unit == CSS_UNIT_PX)
		return layout_from_px(length->value);
	if (length->unit == CSS_UNIT_PERCENT)
		return (layout_unit)((float)size * length->value / 100.0f) + layout_from_px(length->offset);
	return 0;
}
