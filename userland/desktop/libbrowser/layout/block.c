/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Block layout: the widths of CSS 2's visual formatting model, block boxes
 * stacked in normal flow, and the collapsing of their vertical margins
 * (between siblings, through empty blocks, and between a block and its
 * first and last children).  Boxes out of the flow are passed by, with the
 * static position they would have had.
 */

#include "layout/layout.h"

static layout_unit block_resolve(const struct css_length *length, layout_unit containing_width);
static void block_width(struct layout_box *box, layout_unit containing_width);
static int block_children(struct layout_tree *tree, struct layout_box *box);
static layout_unit block_collapse(layout_unit first, layout_unit second);
static layout_unit block_outer_height(const struct layout_box *box);
static int block_owns_context(const struct layout_box *box);
static int block_content(struct layout_tree *tree, struct layout_box *box);
static int block_intrinsic(struct layout_tree *tree, struct layout_box *box, layout_unit containing_width);
static int block_is_intrinsic(const struct css_length *length);

/*
 * Lays out a block box in a containing block of a width: its box model,
 * its content (lines or blocks) and its height.  The box's position is
 * set by its parent, which puts the tree's origin at the box's margin box
 * left and border box top in the block formatting context first (float.c).
 */
int
layout_block(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit containing_width)
{
	struct layout_context context;
	struct wb_vector floats;
	layout_unit saved_x;
	layout_unit saved_y;
	layout_unit height;
	layout_unit basis;
	layout_unit sizing;
	layout_unit specified_height;
	layout_unit content_height;
	layout_unit saved_containing_height;
	struct css_length saved_style_height;
	layout_unit ratio_height;
	int ratio_floor;
	int own_context;
	int intrinsic;
	int height_definite;
	int saved_height_definite;
	int wanted;
	int error;

	/* A box of another writing mode than the system it is laid out in has coordinates of its own. */
	wanted = layout_logical_wanted(tree, box);
	if (wanted) {
		error = layout_logical(tree, box, containing_width);
		return error;
	}

	/* A width of max-content, min-content or fit-content is measured first (ws074-p074). */
	intrinsic = block_is_intrinsic(&box->style.width);
	if (intrinsic && !box->replaced) {
		error = block_intrinsic(tree, box, containing_width);
		return error;
	}

	/* The margins, borders and paddings. */
	layout_box_model(box, containing_width);

	/* A replaced box (an <img> as a block) is sized by its image, and has no content to lay out. */
	if (box->replaced) {
		layout_replaced_size(box, containing_width, tree->containing_height, tree->containing_height_definite);
		layout_auto_margins(box, containing_width);
		box->collapsed_top = box->margin[CSS_TOP];
		box->collapsed_bottom = box->margin[CSS_BOTTOM];
		return 0;
	}

	/* The content width. */
	block_width(box, containing_width);

	/* Under border-box sizing an explicit height includes this vertical frame. */
	sizing = 0;
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
		sizing = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];

	/* A pixel height is definite; a percentage is definite only when its containing block's height is. */
	height_definite = 0;
	specified_height = 0;
	if (box->style.height.unit == CSS_UNIT_PX) {
		specified_height = layout_from_px(box->style.height.value);
		height_definite = 1;
	} else if (box->style.height.unit == CSS_UNIT_PERCENT && tree->containing_height_definite) {
		specified_height = (layout_unit)((float)tree->containing_height * box->style.height.value / 100.0f) +
		    layout_from_px(box->style.height.offset);
		height_definite = 1;
	}

	/*
	 * An auto height from aspect-ratio and the width: definite when the box
	 * scrolls or clips (its automatic minimum is zero), otherwise a floor the
	 * content may exceed.
	 */
	ratio_height = -1;
	ratio_floor = 0;
	if (!height_definite && box->style.aspect_ratio > 0.0f && box->style.height.unit != CSS_UNIT_PX) {
		layout_unit frame_x;

		frame_x = 0;
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
			frame_x = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
		ratio_height = (layout_unit)((float)(box->width + frame_x) / box->style.aspect_ratio) - sizing;
		if (ratio_height < 0)
			ratio_height = 0;
		if (box->style.overflow_x == CSS_OVERFLOW_VISIBLE && box->style.overflow_y == CSS_OVERFLOW_VISIBLE) {
			ratio_floor = 1;
		} else {
			specified_height = ratio_height + sizing;
			height_definite = 1;
		}
	}

	/* A definite specified size becomes the nonnegative content height. */
	if (height_definite) {
		height = specified_height - sizing;
		if (height < 0)
			height = 0;
		specified_height = height;
	}

	/*
	 * Children resolve percentage heights against this content height.  An
	 * inline with block children is represented as a block in this pass, but
	 * it does not establish a containing block: keep the nearest block
	 * ancestor's definite height through that generated wrapper.  A flex or
	 * grid item that started inline is blockified by its container and does
	 * establish one.
	 */
	saved_containing_height = tree->containing_height;
	saved_height_definite = tree->containing_height_definite;
	if (!height_definite && box->kind == LAYOUT_BLOCK && box->style.display == CSS_DISPLAY_INLINE &&
	    box->parent != NULL && box->parent->style.display != CSS_DISPLAY_FLEX &&
	    box->parent->style.display != CSS_DISPLAY_INLINE_FLEX &&
	    box->parent->style.display != CSS_DISPLAY_GRID &&
	    box->parent->style.display != CSS_DISPLAY_INLINE_GRID) {
		tree->containing_height = saved_containing_height;
		tree->containing_height_definite = saved_height_definite;
	} else {
		tree->containing_height = specified_height;
		tree->containing_height_definite = height_definite;
	}

	/* Descendants see a resolved percentage as pixels, or an indefinite percentage as auto. */
	saved_style_height = box->style.height;
	if (height_definite && saved_style_height.unit == CSS_UNIT_PERCENT) {
		box->style.height.unit = CSS_UNIT_PX;
		box->style.height.value = layout_to_px(specified_height + sizing);
		box->style.height.offset = 0;
	} else if (!height_definite && saved_style_height.unit == CSS_UNIT_PERCENT) {
		box->style.height.unit = CSS_UNIT_AUTO;
		box->style.height.value = 0;
		box->style.height.offset = 0;
	}

	/* A box that starts a formatting context lays its content out in it; another moves the origin to its content box. */
	saved_x = tree->origin_x;
	saved_y = tree->origin_y;
	own_context = block_owns_context(box);
	if (own_context) {
		layout_context_begin(tree, &context, &floats);
	} else {
		tree->origin_x += box->margin[CSS_LEFT] + box->border[CSS_LEFT] + box->padding[CSS_LEFT];
		tree->origin_y += box->border[CSS_TOP] + box->padding[CSS_TOP];
	}

	/* The content; a formatting context of its own holds its floats too. */
	error = block_content(tree, box);
	if (error == 0 && own_context) {
		height = layout_floats_bottom(tree);
		if (height > box->height)
			box->height = height;
	}

	/* The formatting context and the origin are the caller's again. */
	if (own_context)
		layout_context_end(tree, &context);
	tree->origin_x = saved_x;
	tree->origin_y = saved_y;
	tree->containing_height = saved_containing_height;
	tree->containing_height_definite = saved_height_definite;
	box->style.height = saved_style_height;

	/* Propagates a content that could not be laid out. */
	if (error != 0)
		return error;

	/* An explicit height replaces the content's. */
	content_height = box->height;
	if (height_definite)
		box->height = specified_height;

	/* An aspect-ratio height is the least a box that shows its overflow has. */
	if (ratio_floor && box->height < ratio_height)
		box->height = ratio_height;

	/* max-height lowers a taller box before min-height, which wins when the constraints conflict. */
	if (box->style.max_height.unit == CSS_UNIT_PX ||
	    (box->style.max_height.unit == CSS_UNIT_PERCENT && saved_height_definite)) {
		basis = 0;
		if (box->style.max_height.unit == CSS_UNIT_PERCENT)
			basis = saved_containing_height;
		height = block_resolve(&box->style.max_height, basis) - sizing;
		if (height < 0)
			height = 0;
		if (box->height > height)
			box->height = height;
	}

	/* min-height raises a shorter box, including one lowered by a smaller max-height. */
	if (box->style.min_height.unit == CSS_UNIT_PX ||
	    (box->style.min_height.unit == CSS_UNIT_PERCENT && saved_height_definite)) {
		basis = 0;
		if (box->style.min_height.unit == CSS_UNIT_PERCENT)
			basis = saved_containing_height;
		height = block_resolve(&box->style.min_height, basis) - sizing;
		if (box->height < height)
			box->height = height;
	}

	/* A <button> centers its content in the height it was given beyond it. */
	layout_center_button(box, content_height);

	/* Succeeded: the box has its size. */
	return 0;
}

/*
 * Centers a <button>'s content vertically: a button is taller than its
 * content when it has a height or a min-height, and its content (the in-flow
 * children and the lines) sits in the middle of the room, not at the top.
 */
void
layout_center_button(
	struct layout_box *box,
	layout_unit content_height)
{
	struct dom_element *element;
	struct layout_box *child;
	layout_unit shift;
	size_t index;

	/* Only an HTML button, taller than its content. */
	if (box->node == NULL || box->node->type != DOM_ELEMENT)
		return;
	element = (struct dom_element *)box->node;
	if (element->ns != DOM_NS_HTML || element->tag != DOM_TAG_BUTTON)
		return;
	if (box->height <= content_height)
		return;
	shift = (box->height - content_height) / 2;

	/* The lines, the in-flow children, and the static positions of the others. */
	for (index = 0; index < box->line_count; index++)
		box->lines[index].y += shift;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow) {
			child->static_y += shift;
			continue;
		}
		child->y += shift;
	}
}

/*
 * Lays out a box whose width is an intrinsic size (ws074-p074): its
 * content's width without a limit (max-content), with a line at every
 * opportunity (min-content), or the room between them the containing
 * block leaves (fit-content), then the box at that width as if it had
 * been given in pixels (its style keeps the keyword).
 */
static int
block_intrinsic(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit containing_width)
{
	struct layout_context context;
	struct wb_vector floats;
	struct css_length width;
	layout_unit most;
	layout_unit least;
	layout_unit room;
	layout_unit used;
	int error;

	/* The content measured with an auto width. */
	width = box->style.width;
	box->style.width.unit = CSS_UNIT_AUTO;
	error = layout_max_content(tree, box, &most);
	if (error != 0) {
		box->style.width = width;
		return error;
	}

	/* The widest word, for min-content and fit-content: the content laid out as narrow as it goes. */
	least = most;
	if (width.unit != CSS_UNIT_MAX_CONTENT) {
		tree->measuring++;
		layout_context_begin(tree, &context, &floats);
		error = layout_block(tree, box, 0);
		layout_context_end(tree, &context);
		if (error == 0)
			least = layout_content_width(box, 0);
		tree->measuring--;
		if (error != 0) {
			box->style.width = width;
			return error;
		}
	}

	/* The width: max-content, min-content, or the room clamped between them. */
	used = most;
	if (width.unit == CSS_UNIT_MIN_CONTENT)
		used = least;
	if (width.unit == CSS_UNIT_FIT_CONTENT) {
		layout_box_model(box, containing_width);
		room = containing_width - box->margin[CSS_LEFT] - box->margin[CSS_RIGHT] - box->border[CSS_LEFT] - box->padding[CSS_LEFT] -
		    box->padding[CSS_RIGHT] - box->border[CSS_RIGHT];
		used = room;
		if (used > most)
			used = most;
		if (used < least)
			used = least;
	}

	/* The box at that content width (its borders and paddings outside it). */
	box->style.width.unit = CSS_UNIT_PX;
	box->style.width.value = layout_to_px(used);
	box->style.width.offset = 0;
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER) {
		layout_box_model(box, containing_width);
		box->style.width.value = layout_to_px(used + box->border[CSS_LEFT] + box->padding[CSS_LEFT] +
		    box->padding[CSS_RIGHT] + box->border[CSS_RIGHT]);
	}

	/* The layout at that width, the style's keyword put back after it. */
	error = layout_block(tree, box, containing_width);
	box->style.width = width;
	if (error != 0)
		return error;

	/* Succeeded: the box has its intrinsic width. */
	return 0;
}

/* Tells whether a width is one of the intrinsic keywords. */
static int
block_is_intrinsic(
	const struct css_length *length)
{
	/* max-content, min-content and fit-content. */
	if (length->unit == CSS_UNIT_MAX_CONTENT)
		return 1;
	if (length->unit == CSS_UNIT_MIN_CONTENT)
		return 1;
	if (length->unit == CSS_UNIT_FIT_CONTENT)
		return 1;

	/* Any other width. */
	return 0;
}

/*
 * Resolves a box's margins, borders and paddings against its containing
 * block's width.
 */
void
layout_box_model(
	struct layout_box *box,
	layout_unit containing_width)
{
	int side;

	/* Every side: percentages of the containing width, borders only where they are drawn. */
	for (side = 0; side < 4; side++) {
		box->margin[side] = block_resolve(&box->style.margin[side], containing_width);
		box->padding[side] = block_resolve(&box->style.padding[side], containing_width);
		box->border[side] = 0;
		if (box->style.border_style[side] != CSS_BORDER_NONE)
			box->border[side] = layout_from_px(box->style.border_width[side]);
	}
}

/*
 * Shares what a sized box leaves of its containing block's width among
 * its auto horizontal margins: both center it, one takes it all (a
 * float's are zero).
 */
void
layout_auto_margins(
	struct layout_box *box,
	layout_unit containing_width)
{
	layout_unit frame;
	layout_unit room;
	int left_auto;
	int right_auto;
	int rtl;

	/* The borders and paddings around the content. */
	frame = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];

	/*
	 * Auto margins share what is left of a sized box: both center it, one
	 * takes it all (a float's and an inline block's are zero).
	 */
	left_auto = 0;
	if (box->style.margin[CSS_LEFT].unit == CSS_UNIT_AUTO && box->floating == CSS_FLOAT_NONE)
		left_auto = 1;
	right_auto = 0;
	if (box->style.margin[CSS_RIGHT].unit == CSS_UNIT_AUTO && box->floating == CSS_FLOAT_NONE)
		right_auto = 1;
	if (box->atomic) {
		left_auto = 0;
		right_auto = 0;
	}

	/* The room the sized box leaves, which the auto margins share. */
	room = containing_width - box->width - frame;
	if (room < 0)
		room = 0;

	/*
	 * Without an auto margin, a box in a right-to-left containing block
	 * keeps its right margin and the left one takes the rest (ws074-p073):
	 * the box stands at the right.  A flex or grid item's margins are its
	 * container's to give.
	 */
	rtl = 0;
	if (box->parent != NULL && box->parent->style.direction == CSS_DIRECTION_RTL)
		rtl = 1;
	if (rtl && (box->parent->style.display == CSS_DISPLAY_FLEX ||
	    box->parent->style.display == CSS_DISPLAY_INLINE_FLEX ||
	    box->parent->style.display == CSS_DISPLAY_GRID ||
	    box->parent->style.display == CSS_DISPLAY_INLINE_GRID))
		rtl = 0;
	if (rtl && !left_auto && !right_auto && box->floating == CSS_FLOAT_NONE && !box->atomic && !box->out_of_flow) {
		box->margin[CSS_LEFT] = containing_width - box->width - frame - box->margin[CSS_RIGHT];
		return;
	}

	/* The auto margins take the room. */
	if (left_auto && right_auto) {
		box->margin[CSS_LEFT] = room / 2;
		box->margin[CSS_RIGHT] = room - room / 2;
	} else if (left_auto) {
		box->margin[CSS_LEFT] = room - box->margin[CSS_RIGHT];
	} else if (right_auto) {
		box->margin[CSS_RIGHT] = room - box->margin[CSS_LEFT];
	}
}

/* Lays out a block's content: lines of inline content, or the child blocks. */
static int
block_content(
	struct layout_tree *tree,
	struct layout_box *box)
{
	int error;

	/* The margins the content may collapse with. */
	box->collapsed_top = box->margin[CSS_TOP];
	box->collapsed_bottom = box->margin[CSS_BOTTOM];

	/* The flex items, the grid items, the lines, or the children. */
	if (box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX) {
		error = layout_flex(tree, box);
	} else if (box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID) {
		error = layout_grid(tree, box);
	} else if (box->style.display == CSS_DISPLAY_TABLE || box->style.display == CSS_DISPLAY_INLINE_TABLE) {
		error = layout_table(tree, box);
	} else if (box->children_inline) {
		error = layout_inline(tree, box);
	} else {
		error = block_children(tree, box);
	}

	/* Propagates a content that could not be laid out. */
	if (error != 0)
		return error;

	/* Succeeded: the content is laid out. */
	return 0;
}

/* Tells whether a box starts a block formatting context of its own (the root, floats, boxes out of the flow, clipping boxes, inline blocks, cells, flex). */
static int
block_owns_context(
	const struct layout_box *box)
{
	int clips;

	/* The root, the root of a writing mode's domain, a float, a box out of the flow. */
	if (box->parent == NULL)
		return 1;
	if (box->logical_root)
		return 1;
	if (layout_physical_mode(box) != layout_physical_mode(box->parent))
		return 1;
	if (box->floating != CSS_FLOAT_NONE)
		return 1;
	if (box->out_of_flow)
		return 1;

	/* A box that clips its overflow. */
	clips = layout_clips(box);
	if (clips)
		return 1;

	/* The displays that make a formatting context (an inline block, and the table cells laid out as blocks in this pass). */
	if (box->atomic)
		return 1;
	if (box->style.display == CSS_DISPLAY_INLINE_BLOCK)
		return 1;
	if (box->style.display == CSS_DISPLAY_TABLE_CELL)
		return 1;
	if (box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX)
		return 1;
	if (box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID)
		return 1;
	if (box->style.display == CSS_DISPLAY_TABLE || box->style.display == CSS_DISPLAY_INLINE_TABLE)
		return 1;
	if (box->style.display == CSS_DISPLAY_TABLE_CAPTION)
		return 1;

	/* A flex or grid item. */
	if (box->parent->style.display == CSS_DISPLAY_FLEX || box->parent->style.display == CSS_DISPLAY_INLINE_FLEX)
		return 1;
	if (box->parent->style.display == CSS_DISPLAY_GRID ||
	    box->parent->style.display == CSS_DISPLAY_INLINE_GRID)
		return 1;

	/* A block in its parent's context. */
	return 0;
}

/* Resolves a computed length against the containing block's width (auto and none resolve to 0). */
static layout_unit
block_resolve(
	const struct css_length *length,
	layout_unit containing_width)
{
	layout_unit value;

	/* Pixels, a percentage of the width, or nothing. */
	value = 0;
	if (length->unit == CSS_UNIT_PX)
		value = layout_from_px(length->value);
	if (length->unit == CSS_UNIT_PERCENT)
		value = (layout_unit)((float)containing_width * length->value / 100.0f) + layout_from_px(length->offset);

	/* Reports the length. */
	return value;
}

/* Computes a block's content width and its horizontal margins (auto margins center a sized box). */
static void
block_width(
	struct layout_box *box,
	layout_unit containing_width)
{
	layout_unit frame;
	layout_unit room;
	layout_unit width;
	layout_unit sizing;

	/* The borders and paddings around the content. */
	frame = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];

	/*
	 * Under box-sizing: border-box the given widths size the border box, so
	 * the frame comes off them to give the content's.
	 */
	sizing = 0;
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
		sizing = frame;

	/* An auto width fills the containing block, unless an aspect-ratio makes it from a pixel height. */
	if (box->style.width.unit == CSS_UNIT_AUTO && box->style.aspect_ratio > 0.0f && box->style.height.unit == CSS_UNIT_PX) {
		width = (layout_unit)((float)layout_from_px(box->style.height.value) * box->style.aspect_ratio) - sizing;
	} else if (box->style.width.unit == CSS_UNIT_AUTO) {
		width = containing_width - box->margin[CSS_LEFT] - box->margin[CSS_RIGHT] - frame;
	} else {
		width = block_resolve(&box->style.width, containing_width) - sizing;
	}

	/* min-width and max-width. */
	if (box->style.max_width.unit == CSS_UNIT_PX || box->style.max_width.unit == CSS_UNIT_PERCENT) {
		room = block_resolve(&box->style.max_width, containing_width) - sizing;
		if (width > room)
			width = room;
	}

	/* min-width wins over max-width, and no width is negative. */
	room = block_resolve(&box->style.min_width, containing_width) - sizing;
	if (width < room)
		width = room;
	if (width < 0)
		width = 0;
	box->width = width;

	/* Auto margins share what is left. */
	layout_auto_margins(box, containing_width);
}

/*
 * Lays out a block's block children one under another, collapsing the
 * margins that meet.
 */
static int
block_children(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct layout_box *child;
	layout_unit cursor;
	layout_unit pending;
	layout_unit child_top;
	layout_unit child_bottom;
	layout_unit origin_x;
	layout_unit origin_y;
	layout_unit estimate;
	layout_unit clearance;
	layout_unit room_left;
	layout_unit room_right;
	layout_unit room_width;
	int own_context;
	int collapse_top;
	int collapse_bottom;
	int first;
	layout_unit collapsed_y;
	int empty;
	int error;

	/*
	 * The box's own top margin meets its first child's unless a border,
	 * padding, or the root separates them; likewise at the bottom when the
	 * height is auto.
	 */
	collapse_top = 0;
	if (box->parent != NULL && !block_owns_context(box) &&
	    box->border[CSS_TOP] == 0 && box->padding[CSS_TOP] == 0)
		collapse_top = 1;
	collapse_bottom = 0;
	if (box->parent != NULL && !block_owns_context(box) &&
	    box->border[CSS_BOTTOM] == 0 && box->padding[CSS_BOTTOM] == 0 &&
	    box->style.height.unit == CSS_UNIT_AUTO)
		collapse_bottom = 1;

	/* Stacks the children, carrying the margin that has not been placed yet. */
	cursor = 0;
	pending = 0;
	first = 1;
	origin_x = tree->origin_x;
	origin_y = tree->origin_y;
	for (child = box->first_child; child != NULL; child = child->next) {
		/* A box out of the flow takes no room; its static position is where the next block would start. */
		if (child->out_of_flow) {
			child->static_x = 0;
			child->static_y = cursor + pending;
			if (first && collapse_top)
				child->static_y = 0;
			continue;
		}

		/* A float shrinks to fit and goes to its side, no higher than where the next block would start. */
		if (child->floating != CSS_FLOAT_NONE) {
			tree->origin_x = origin_x;
			tree->origin_y = origin_y + cursor + pending;
			error = layout_shrink_to_fit(tree, child, box->width);
			tree->origin_x = origin_x;
			tree->origin_y = origin_y;
			if (error == 0)
				error = layout_place_float(tree, child, cursor + pending, box->width);
			if (error != 0)
				return error;
			continue;
		}

		/* The child's border box starts about here (before its margin collapses with its first child's). */
		estimate = cursor + block_collapse(pending, block_resolve(&child->style.margin[CSS_TOP], box->width));
		if (first && collapse_top)
			estimate = 0;
		clearance = layout_clearance(tree, child->style.clear);
		if (estimate < clearance)
			estimate = clearance;

		/* A child with its own formatting context keeps out of the floats beside its top: it gets the room between them. */
		room_left = 0;
		room_width = box->width;
		own_context = block_owns_context(child);
		if (own_context) {
			layout_line_room(tree, estimate, estimate + 1, box->width, &room_left, &room_right);
			room_width = room_right - room_left;
			if (room_width < 0)
				room_width = 0;
		}

		/* Lays the child out in that width, where the floats beside it are. */
		tree->origin_x = origin_x + room_left;
		tree->origin_y = origin_y + estimate;
		error = layout_block(tree, child, room_width);
		tree->origin_x = origin_x;
		tree->origin_y = origin_y;
		if (error != 0)
			return error;
		child->x = room_left + child->margin[CSS_LEFT];
		child_top = child->collapsed_top;
		child_bottom = child->collapsed_bottom;

		/* An empty block's margins collapse through it into the margin carried on. */
		empty = 0;
		if (child->height == 0 && child->border[CSS_TOP] == 0 && child->border[CSS_BOTTOM] == 0 &&
		    child->padding[CSS_TOP] == 0 && child->padding[CSS_BOTTOM] == 0)
			empty = 1;

		/* An empty block that clears floats is pushed below them, and its margins no longer collapse through (a clearfix). */
		collapsed_y = cursor + block_collapse(pending, child_top);
		if (empty && collapsed_y < clearance)
			empty = 0;
		if (empty) {
			/*
			 * Its border edge sits where the margins met so far and its own top
			 * margin put it, as if a bottom border held its bottom margin back.
			 */
			if (first && collapse_top) {
				child->y = 0;
			} else {
				child->y = cursor + block_collapse(pending, child_top);
			}

			/* All of its margins join the margin carried on. */
			pending = block_collapse(pending, block_collapse(child_top, child_bottom));
			continue;
		}

		/* The first child's top margin joins the box's own when they meet; otherwise it joins the carried one. */
		if (first && collapse_top) {
			box->collapsed_top = block_collapse(box->collapsed_top, block_collapse(pending, child_top));
			child->y = 0;
		} else {
			child->y = cursor + block_collapse(pending, child_top);
		}

		/* A child that clears floats starts below them (its margins then no longer collapse through). */
		if (child->y < clearance) {
			child->y = clearance;
			if (first && collapse_top)
				box->collapsed_top = box->margin[CSS_TOP];
		}

		/* The children after this one are not the first to meet the box's top margin. */
		first = 0;

		/* The cursor moves past the child's border box; its bottom margin is carried to the next. */
		cursor = child->y + block_outer_height(child);
		pending = child_bottom;
	}

	/* The last margin joins the box's own bottom margin, or stays inside the box. */
	if (collapse_bottom) {
		box->collapsed_bottom = block_collapse(box->collapsed_bottom, pending);
		box->height = cursor;
	} else {
		box->height = cursor + pending;
	}

	/* A box whose children were all empty collapses its own margins too. */
	if (first && collapse_top && collapse_bottom)
		box->collapsed_top = block_collapse(box->collapsed_top, pending);

	/* Succeeded: the children are placed. */
	return 0;
}

/* Collapses two margins: the largest positive plus the most negative. */
static layout_unit
block_collapse(
	layout_unit first,
	layout_unit second)
{
	layout_unit positive;
	layout_unit negative;

	/* The largest of the positive ones. */
	positive = 0;
	if (first > positive)
		positive = first;
	if (second > positive)
		positive = second;

	/* The most negative of the negative ones. */
	negative = 0;
	if (first < negative)
		negative = first;
	if (second < negative)
		negative = second;

	/* Their sum is the collapsed margin. */
	return positive + negative;
}

/* Reports a box's border-box height. */
static layout_unit
block_outer_height(
	const struct layout_box *box)
{
	/* The content, paddings and borders. */
	return box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
}
