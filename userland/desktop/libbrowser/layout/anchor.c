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
 * (unless that is the viewport's), and that is already placed when it is
 * out of the flow itself.  An anchor() of an inset is the distance from
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
static int anchor_query(const struct layout_tree *tree, const struct layout_box *box, const struct css_anchor_query *query, int slot, layout_unit cb_start, layout_unit cb_size, layout_unit *value);
static int anchor_find(const struct layout_tree *tree, const struct layout_box *box, const struct vm_string *name, struct layout_rect *rect);
static void anchor_search(const struct layout_box *walk, const struct layout_box *box, const struct layout_box *containing, const struct vm_string *name, int inside, int *passed, const struct layout_box **found, int depth);
static int anchor_has_name(const struct layout_box *box, const struct vm_string *name);
static const struct layout_box *anchor_containing(const struct layout_box *box);
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
		    slot == CSS_ANCHOR_SLOT_MIN_HEIGHT || slot == CSS_ANCHOR_SLOT_MAX_HEIGHT) {
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
		if (box->style.offset[side].unit == CSS_UNIT_ANCHOR)
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
	int vertical;
	int side;
	int far;
	int found;

	/* The anchor. */
	found = anchor_find(tree, box, query->name != NULL ? query->name : box->style.position_anchor, &rect);
	if (!found)
		return 0;

	/* The axis of the slot, and whether its inset is measured from the far edge (right or bottom). */
	vertical = slot == CSS_ANCHOR_SLOT_TOP || slot == CSS_ANCHOR_SLOT_BOTTOM || slot == CSS_ANCHOR_SLOT_HEIGHT ||
	    slot == CSS_ANCHOR_SLOT_MIN_HEIGHT || slot == CSS_ANCHOR_SLOT_MAX_HEIGHT;
	far = slot == CSS_ANCHOR_SLOT_RIGHT || slot == CSS_ANCHOR_SLOT_BOTTOM;
	anchor_start = vertical ? rect.y : rect.x;
	anchor_size = vertical ? rect.height : rect.width;

	/* anchor-size(): the anchor's size on the asked axis (the slot's own axis by default). */
	side = query->side;
	if (side >= CSS_ANCHOR_WIDTH) {
		if (side == CSS_ANCHOR_SIZE_DEFAULT)
			side = vertical ? CSS_ANCHOR_HEIGHT : CSS_ANCHOR_WIDTH;
		if (side == CSS_ANCHOR_BLOCK || side == CSS_ANCHOR_SELF_BLOCK)
			side = CSS_ANCHOR_HEIGHT;
		if (side == CSS_ANCHOR_INLINE || side == CSS_ANCHOR_SELF_INLINE)
			side = CSS_ANCHOR_WIDTH;
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
		edge = anchor_start;
		break;
	case CSS_ANCHOR_END:
	case CSS_ANCHOR_SELF_END:
		edge = anchor_start + anchor_size;
		break;
	case CSS_ANCHOR_CENTER:
		edge = anchor_start + anchor_size / 2;
		break;
	case CSS_ANCHOR_PERCENT:
		edge = anchor_start + (layout_unit)((float)anchor_size * query->percent / 100.0f);
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
	const struct layout_box *containing;
	const struct layout_box *found;
	int passed;

	/* A name, and the tree. */
	if (name == NULL || tree->root == NULL)
		return 0;

	/* The last acceptable box with the name in tree order. */
	containing = anchor_containing(box);
	found = NULL;
	passed = 0;
	anchor_search(tree->root, box, containing, name, containing == NULL, &passed, &found, 0);
	if (found == NULL || found->node == NULL)
		return 0;

	/* Its rectangle. */
	return layout_node_bounds(tree, found->node, rect);
}

/*
 * Searches the boxes under walk for anchors of a name: inside the
 * containing block (inside tells whether walk is), never the box or what
 * is inside it, and a box out of the flow only before the box.
 */
static void
anchor_search(
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
		if (child_inside && anchor_has_name(child, name) && (!child->out_of_flow || !*passed))
			*found = child;
		anchor_search(child, box, containing, name, child_inside, passed, found, depth + 1);
	}
}

/* Tells whether a box has an anchor name. */
static int
anchor_has_name(
	const struct layout_box *box,
	const struct vm_string *name)
{
	const struct css_anchor_names *names;
	int index;

	/* Each of its names. */
	names = box->style.anchor_names;
	if (names == NULL)
		return 0;
	for (index = 0; index < names->count; index++) {
		if (names->names[index] == name || vm_string_equal(names->names[index], name))
			return 1;
	}
	return 0;
}

/* The box a box out of the flow is placed in (NULL for the viewport). */
static const struct layout_box *
anchor_containing(
	const struct layout_box *box)
{
	const struct layout_box *ancestor;

	/* The nearest ancestor that contains the box: transformed for a fixed box, positioned or transformed otherwise. */
	for (ancestor = box->parent; ancestor != NULL; ancestor = ancestor->parent) {
		if (box->style.position == CSS_POSITION_FIXED ? layout_contains_fixed(ancestor) : layout_contains_absolute(ancestor))
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
	int found;

	/* The area's regions, and the anchor. */
	if (!css_position_area_regions(box->style.position_area, box->style.writing_mode, box->style.direction, &x0, &x1, &y0, &y1))
		return;
	found = anchor_find(tree, box, box->style.position_anchor, &rect);
	if (!found)
		return;

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
	box->cb_set = 1;
	box->cb_left = left;
	box->cb_right = right;
	box->cb_top = top;
	box->cb_bottom = bottom;
	box->cb_left_auto = 0;
	box->cb_right_auto = 0;
	box->cb_top_auto = 0;
	box->cb_bottom_auto = 0;

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
