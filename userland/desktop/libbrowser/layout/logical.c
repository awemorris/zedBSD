/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writing modes: a box whose writing mode differs from the coordinate
 * system its parent is laid out in is laid out in its own logical
 * coordinates, with the horizontal algorithms.
 *
 * The coordinate system of a layout is the tree's logical_mode (horizontal
 * at the root).  A vertical box starts a domain: it and its descendants of
 * the same writing mode get logical styles (width and height swapped,
 * block-start on top and line-left on the left, the writing mode
 * horizontal), the block, inline, flex and grid layouts run unchanged, and
 * the domain's geometry is turned back into physical coordinates (vertical-lr
 * transposes, vertical-rl also mirrors the block axis) before the physical
 * styles come back.  Lines keep their logical coordinates and are painted
 * vertical.  A descendant of another writing mode is the root of a domain
 * of its own; the outer domain moves only its box.  An orthogonal root's
 * inline size is its containing block's block size when that is definite,
 * otherwise the viewport's.
 * https://drafts.csswg.org/css-writing-modes-4/#abstract-box
 * https://drafts.csswg.org/css-writing-modes-4/#orthogonal-flows
 */

#include "layout/layout.h"

#include <errno.h>
#include <string.h>

static int logical_vertical(int mode);
static layout_unit logical_containing_extent(const struct layout_tree *tree, const struct layout_box *box, int own);
static int logical_fits_content(const struct layout_box *box);
static void logical_sides_to_logical(struct css_length *sides, int mode);
static void logical_floats_to_logical(float *sides, int mode);
static void logical_ints_to_logical(int *sides, int mode);
static void logical_colors_to_logical(uint32_t *sides, int mode);
static void logical_units_to_physical(layout_unit *sides, int mode);
static void logical_units_to_logical(layout_unit *sides, int mode);
static void logical_style(struct css_style *style, int mode);
static int logical_convert(struct layout_tree *tree, struct layout_box *box, int mode);
static void logical_unflip(struct layout_box *box, int mode, layout_unit parent_width, int root);
static void logical_box_to_physical(struct layout_box *box, int mode, layout_unit parent_width, int place);
static void logical_box_to_logical(struct layout_box *box, int mode);
static void logical_static_to_physical(struct layout_box *child, int mode, layout_unit width);
static void logical_restore(struct layout_box *box);

/*
 * Reports the writing mode a box was given (its physical style's, when a
 * domain has its logical style in place).
 */
int
layout_physical_mode(
	const struct layout_box *box)
{
	/* The style the page gave the box. */
	if (box->physical_style != NULL)
		return box->physical_style->writing_mode;

	/* Succeeded: the box's own style is physical. */
	return box->style.writing_mode;
}

/*
 * Tells whether a box is laid out in a coordinate system of its own: its
 * writing mode is not the one its parent is laid out in.
 */
int
layout_logical_wanted(
	const struct layout_tree *tree,
	const struct layout_box *box)
{
	const struct layout_box *child;
	int display;
	int mode;

	/* A box of the domain being laid out uses the domain's coordinates. */
	if (box->physical_style != NULL)
		return 0;

	/* Otherwise its writing mode against the system it is laid out in. */
	mode = box->style.writing_mode;
	if (mode == tree->logical_mode)
		return 0;

	/*
	 * A vertical flex or grid container in horizontal coordinates with
	 * horizontal (orthogonal) items places its items in physical
	 * coordinates.
	 */
	display = box->style.display;
	if (tree->logical_mode == CSS_WRITING_HORIZONTAL_TB &&
	    (display == CSS_DISPLAY_GRID || display == CSS_DISPLAY_INLINE_GRID ||
	    display == CSS_DISPLAY_FLEX || display == CSS_DISPLAY_INLINE_FLEX)) {
		for (child = box->first_child; child != NULL; child = child->next) {
			if (child->style.writing_mode == CSS_WRITING_HORIZONTAL_TB)
				return 0;
		}
	}

	/*
	 * The items of a horizontal flex or grid container, and the boxes out of
	 * the flow it places, keep the container's own handling of their writing
	 * modes.
	 */
	if (tree->logical_mode == CSS_WRITING_HORIZONTAL_TB && box->parent != NULL) {
		display = box->parent->style.display;
		if (display == CSS_DISPLAY_FLEX || display == CSS_DISPLAY_INLINE_FLEX)
			return 0;
		if (display == CSS_DISPLAY_GRID || display == CSS_DISPLAY_INLINE_GRID)
			return 0;
	}

	/* Succeeded: the box needs its own coordinates. */
	return 1;
}

/*
 * Lays out a box in its own writing mode's coordinates (layout_block's
 * job) and leaves its box in the coordinates of the system its parent is
 * laid out in.
 */
int
layout_logical(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit containing_width)
{
	layout_unit saved_containing_height;
	layout_unit available;
	int saved_definite;
	int saved_mode;
	int outer;
	int own;
	int fit;
	int error;

	/* The two systems: the parent's and the box's. */
	outer = tree->logical_mode;
	own = box->style.writing_mode;

	/* A vertical box's domain gets its logical styles. */
	if (logical_vertical(own)) {
		error = logical_convert(tree, box, own);
		if (error != 0)
			return error;
	}

	/*
	 * The room along the box's inline axis: the containing block's size in
	 * that direction when it is known (its block size in the parent's
	 * system when the two are orthogonal), otherwise the viewport's.
	 */
	available = containing_width;
	if (logical_vertical(own) != logical_vertical(outer)) {
		if (box->out_of_flow) {
			available = logical_containing_extent(tree, box, own);
		} else if (tree->containing_height_definite) {
			available = tree->containing_height;
		} else if (logical_vertical(own)) {
			available = tree->viewport_height;
		} else {
			available = tree->viewport_width;
		}
	}

	/* The parent's inline size is the box's block-axis containing size when they are orthogonal. */
	saved_containing_height = tree->containing_height;
	saved_definite = tree->containing_height_definite;
	saved_mode = tree->logical_mode;
	if (logical_vertical(own) != logical_vertical(outer)) {
		tree->containing_height = containing_width;
		tree->containing_height_definite = 1;
	}
	tree->logical_mode = own;

	/*
	 * The layout in the box's own coordinates (as the root of a domain it
	 * owns a formatting context).  An orthogonal block in a block container
	 * is as wide as its content (fit-content in the room) rather than the
	 * whole room.
	 */
	box->logical_root = 1;
	fit = 0;
	if (logical_vertical(own) != logical_vertical(outer) && box->parent != NULL)
		fit = logical_fits_content(box);
	if (fit) {
		error = layout_shrink_to_fit(tree, box, available);
	} else {
		error = layout_block(tree, box, available);
	}
	box->logical_root = 0;
	tree->logical_mode = saved_mode;
	tree->containing_height = saved_containing_height;
	tree->containing_height_definite = saved_definite;

	/* The domain back to physical coordinates, and its styles back. */
	if (logical_vertical(own)) {
		logical_unflip(box, own, 0, 1);
		logical_restore(box);
	}
	if (error != 0)
		return error;

	/* The box in its parent's system, whose domain moves it as one box. */
	if (logical_vertical(outer))
		logical_box_to_logical(box, outer);

	/* An orthogonal root's margins do not collapse with its parent's. */
	box->collapsed_top = box->margin[CSS_TOP];
	box->collapsed_bottom = box->margin[CSS_BOTTOM];

	/* Succeeded: the box is laid out. */
	return 0;
}

/*
 * Tells whether an orthogonal box's auto inline size fits its content: a
 * float, an inline block, a box out of the flow, or a block-level box of a
 * block container (a flex or grid item is sized by its container).
 */
static int
logical_fits_content(
	const struct layout_box *box)
{
	int display;

	/* An inline size of its own is kept. */
	if (box->style.width.unit != CSS_UNIT_AUTO)
		return 0;

	/* A replaced box has its own size. */
	if (box->replaced)
		return 0;

	/* Floats, inline blocks and boxes out of the flow shrink to fit their content. */
	if (box->floating != CSS_FLOAT_NONE || box->out_of_flow || box->atomic)
		return 1;

	/* Flex and grid items are sized by their containers. */
	display = box->parent->style.display;
	if (display == CSS_DISPLAY_FLEX || display == CSS_DISPLAY_INLINE_FLEX)
		return 0;
	if (display == CSS_DISPLAY_GRID || display == CSS_DISPLAY_INLINE_GRID)
		return 0;

	/* Succeeded: the box fits its content. */
	return 1;
}

/*
 * The size along an orthogonal box's inline axis of the containing block a
 * box out of the flow is placed in: the padding box of its nearest
 * positioned ancestor, or the viewport's.
 */
static layout_unit
logical_containing_extent(
	const struct layout_tree *tree,
	const struct layout_box *box,
	int own)
{
	const struct layout_box *ancestor;
	int positioned;

	/* The nearest positioned ancestor's padding box. */
	for (ancestor = box->parent; ancestor != NULL; ancestor = ancestor->parent) {
		positioned = layout_is_positioned(ancestor);
		if (!positioned)
			continue;
		if (logical_vertical(own))
			return ancestor->padding[CSS_TOP] + ancestor->height + ancestor->padding[CSS_BOTTOM];
		return ancestor->padding[CSS_LEFT] + ancestor->width + ancestor->padding[CSS_RIGHT];
	}

	/* Otherwise the viewport. */
	if (logical_vertical(own))
		return tree->viewport_height;
	return tree->viewport_width;
}

/* Tells whether a writing mode lays lines out vertically. */
static int
logical_vertical(
	int mode)
{
	/* Only horizontal-tb is horizontal. */
	if (mode == CSS_WRITING_HORIZONTAL_TB)
		return 0;

	/* vertical-lr and vertical-rl (and the sideways modes, laid out as they are). */
	return 1;
}

/*
 * Gives a box and the descendants of its writing mode their logical
 * styles, keeping the physical ones in the tree's arena.
 */
static int
logical_convert(
	struct layout_tree *tree,
	struct layout_box *box,
	int mode)
{
	struct layout_box *child;
	struct css_style *kept;
	int error;
	int own;

	/* The physical style, kept. */
	kept = wb_arena_alloc(&tree->arena, sizeof(*kept));
	if (kept == NULL)
		return ENOMEM;
	memcpy(kept, &box->style, sizeof(*kept));
	box->physical_style = kept;

	/* The logical one in its place (a max-content width measured in physical coordinates no longer holds). */
	logical_style(&box->style, mode);
	box->max_content_known = 0;

	/*
	 * The children of the box's own writing mode or the other vertical one
	 * (whose box the box's mode places, and whose contents run their own
	 * way); an orthogonal one starts a domain of its own.
	 */
	own = kept->writing_mode;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->physical_style != NULL)
			continue;
		if (child->style.writing_mode != own &&
		    (!logical_vertical(child->style.writing_mode) || !logical_vertical(own)))
			continue;
		error = logical_convert(tree, child, own);
		if (error != 0)
			return error;
	}

	/* Succeeded: the domain has its logical styles. */
	return 0;
}

/* Turns a physical style into the logical one of a vertical writing mode. */
static void
logical_style(
	struct css_style *style,
	int mode)
{
	struct css_length length;
	int keyword;

	/* The sizes: the inline size is the height, the block size the width. */
	length = style->width;
	style->width = style->height;
	style->height = length;
	length = style->min_width;
	style->min_width = style->min_height;
	style->min_height = length;
	length = style->max_width;
	style->max_width = style->max_height;
	style->max_height = length;

	/* The four sides of the margins, paddings, insets and borders. */
	logical_sides_to_logical(style->margin, mode);
	logical_sides_to_logical(style->padding, mode);
	logical_sides_to_logical(style->offset, mode);
	logical_floats_to_logical(style->border_width, mode);
	logical_ints_to_logical(style->border_style, mode);
	logical_colors_to_logical(style->border_color, mode);

	/* Overflow per axis. */
	keyword = style->overflow_x;
	style->overflow_x = style->overflow_y;
	style->overflow_y = keyword;

	/* The ratio is block over inline now. */
	if (style->aspect_ratio > 0.0f)
		style->aspect_ratio = 1.0f / style->aspect_ratio;

	/* The layouts see a horizontal box. */
	style->writing_mode = CSS_WRITING_HORIZONTAL_TB;
}

/*
 * Reorders four physical sides (top, right, bottom, left) into logical
 * ones in the same order: block-start, line-right, block-end, line-left.
 * Vertical-lr's block-start is the left, vertical-rl's the right; the
 * line-left is the top for both.
 */
static void
logical_sides_to_logical(
	struct css_length *sides,
	int mode)
{
	struct css_length physical[4];

	/* The physical sides, then each logical one from its side. */
	memcpy(physical, sides, sizeof(physical));
	sides[CSS_LEFT] = physical[CSS_TOP];
	sides[CSS_RIGHT] = physical[CSS_BOTTOM];
	if (mode == CSS_WRITING_VERTICAL_LR) {
		sides[CSS_TOP] = physical[CSS_LEFT];
		sides[CSS_BOTTOM] = physical[CSS_RIGHT];
	} else {
		sides[CSS_TOP] = physical[CSS_RIGHT];
		sides[CSS_BOTTOM] = physical[CSS_LEFT];
	}
}

/* Reorders four physical border widths into logical ones (as logical_sides_to_logical). */
static void
logical_floats_to_logical(
	float *sides,
	int mode)
{
	float physical[4];

	/* The physical sides, then each logical one from its side. */
	memcpy(physical, sides, sizeof(physical));
	sides[CSS_LEFT] = physical[CSS_TOP];
	sides[CSS_RIGHT] = physical[CSS_BOTTOM];
	if (mode == CSS_WRITING_VERTICAL_LR) {
		sides[CSS_TOP] = physical[CSS_LEFT];
		sides[CSS_BOTTOM] = physical[CSS_RIGHT];
	} else {
		sides[CSS_TOP] = physical[CSS_RIGHT];
		sides[CSS_BOTTOM] = physical[CSS_LEFT];
	}
}

/* Reorders four physical border styles into logical ones (as logical_sides_to_logical). */
static void
logical_ints_to_logical(
	int *sides,
	int mode)
{
	int physical[4];

	/* The physical sides, then each logical one from its side. */
	memcpy(physical, sides, sizeof(physical));
	sides[CSS_LEFT] = physical[CSS_TOP];
	sides[CSS_RIGHT] = physical[CSS_BOTTOM];
	if (mode == CSS_WRITING_VERTICAL_LR) {
		sides[CSS_TOP] = physical[CSS_LEFT];
		sides[CSS_BOTTOM] = physical[CSS_RIGHT];
	} else {
		sides[CSS_TOP] = physical[CSS_RIGHT];
		sides[CSS_BOTTOM] = physical[CSS_LEFT];
	}
}

/* Reorders four physical border colors into logical ones (as logical_sides_to_logical). */
static void
logical_colors_to_logical(
	uint32_t *sides,
	int mode)
{
	uint32_t physical[4];

	/* The physical sides, then each logical one from its side. */
	memcpy(physical, sides, sizeof(physical));
	sides[CSS_LEFT] = physical[CSS_TOP];
	sides[CSS_RIGHT] = physical[CSS_BOTTOM];
	if (mode == CSS_WRITING_VERTICAL_LR) {
		sides[CSS_TOP] = physical[CSS_LEFT];
		sides[CSS_BOTTOM] = physical[CSS_RIGHT];
	} else {
		sides[CSS_TOP] = physical[CSS_RIGHT];
		sides[CSS_BOTTOM] = physical[CSS_LEFT];
	}
}

/* Reorders four laid out logical sides back into physical ones (the inverse of logical_sides_to_logical). */
static void
logical_units_to_physical(
	layout_unit *sides,
	int mode)
{
	layout_unit logical[4];

	/* The logical sides, then each physical one from its side. */
	memcpy(logical, sides, sizeof(logical));
	sides[CSS_TOP] = logical[CSS_LEFT];
	sides[CSS_BOTTOM] = logical[CSS_RIGHT];
	if (mode == CSS_WRITING_VERTICAL_LR) {
		sides[CSS_LEFT] = logical[CSS_TOP];
		sides[CSS_RIGHT] = logical[CSS_BOTTOM];
	} else {
		sides[CSS_RIGHT] = logical[CSS_TOP];
		sides[CSS_LEFT] = logical[CSS_BOTTOM];
	}
}

/* Reorders four laid out physical sides into logical ones. */
static void
logical_units_to_logical(
	layout_unit *sides,
	int mode)
{
	layout_unit physical[4];

	/* The physical sides, then each logical one from its side. */
	memcpy(physical, sides, sizeof(physical));
	sides[CSS_LEFT] = physical[CSS_TOP];
	sides[CSS_RIGHT] = physical[CSS_BOTTOM];
	if (mode == CSS_WRITING_VERTICAL_LR) {
		sides[CSS_TOP] = physical[CSS_LEFT];
		sides[CSS_BOTTOM] = physical[CSS_RIGHT];
	} else {
		sides[CSS_TOP] = physical[CSS_RIGHT];
		sides[CSS_BOTTOM] = physical[CSS_LEFT];
	}
}

/*
 * Turns a laid out domain back into physical coordinates, from its root
 * down: each box's size, sides and (except the root's, which its parent
 * places) position, its lines painted vertical, and the static places of
 * its children out of the flow.  parent_width is the physical width of
 * the parent's content box (vertical-rl mirrors against it).
 */
static void
logical_unflip(
	struct layout_box *box,
	int mode,
	layout_unit parent_width,
	int root)
{
	struct layout_box *child;
	layout_unit logical_height;
	size_t index;
	int own;

	/* The block size the lines and the children are laid out in, before it becomes the width. */
	logical_height = box->height;

	/* The box itself, placed by its parent's mode; its contents run by its own. */
	logical_box_to_physical(box, mode, parent_width, !root);
	own = box->physical_style->writing_mode;

	/* Its lines are drawn down the page; vertical-rl stacks them from the right. */
	if (box->children_inline && box->line_count > 0) {
		box->vertical_inline = 1;
		if (own == CSS_WRITING_VERTICAL_RL) {
			for (index = 0; index < box->line_count; index++)
				box->lines[index].y = logical_height - box->lines[index].y - box->lines[index].height;
		}
	}

	/* Each child: one of the domain wholly, another domain's root (already physical inside) as one box. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->physical_style != NULL) {
			logical_unflip(child, own, box->width, 0);
		} else if (!child->out_of_flow) {
			logical_box_to_physical(child, own, box->width, 1);
		}

		/* A child out of the flow keeps its static place and the area its parent gave it. */
		if (child->out_of_flow)
			logical_static_to_physical(child, own, box->width);
	}
}

/*
 * Turns the static place a child out of the flow got in a vertical
 * domain into physical coordinates in its parent's content box (width
 * wide): the point or the area a grid gave it, the area's alignment, and
 * the containing block a grid's lines give it.
 */
static void
logical_static_to_physical(
	struct layout_box *child,
	int mode,
	layout_unit width)
{
	layout_unit left;
	layout_unit top;
	layout_unit area_width;
	layout_unit area_height;
	layout_unit edge;
	int flag;
	int align;
	int safe;

	/* The static area (a point when it has no size): from the logical one, across then down. */
	area_width = child->static_x_area_width;
	area_height = child->static_y_area_height;
	left = child->static_x;
	if (child->static_x_from_right)
		left -= area_width;

	/* A grid's lines give the area's block size when the static area does not. */
	if (area_height == 0 && child->cb_set && !child->cb_top_auto && !child->cb_bottom_auto)
		area_height = child->cb_bottom - child->cb_top;
	top = child->static_y;
	child->static_x = top;
	child->static_x_from_right = 0;
	if (mode == CSS_WRITING_VERTICAL_RL) {
		/* Vertical-rl starts its blocks at the right: the place names the area's right edge. */
		child->static_x = width - top;
		child->static_x_from_right = 1;
	}
	child->static_y = left;
	child->static_x_area_width = area_height;
	child->static_y_area_height = area_width;

	/*
	 * The alignments change axes.  A block-end alignment goes to the right
	 * in vertical-lr (aligned from the right edge); in vertical-rl the place
	 * already names the right edge, where a start goes.
	 */
	align = child->static_x_align;
	if (align == CSS_ALIGN_RIGHT)
		align = CSS_ALIGN_END;
	else if (align == CSS_ALIGN_LEFT)
		align = CSS_ALIGN_START;
	child->static_x_align = child->static_y_align;
	child->static_y_align = align;
	child->static_x_align_from_right = 0;
	if (mode == CSS_WRITING_VERTICAL_LR &&
	    (child->static_x_align == CSS_ALIGN_END || child->static_x_align == CSS_ALIGN_SELF_END))
		child->static_x_align_from_right = 1;
	safe = child->static_x_safe;
	child->static_x_safe = child->static_y_safe;
	child->static_y_safe = safe;

	/* A safe overflow falls back to the block start: the right edge in vertical-rl. */
	if (mode == CSS_WRITING_VERTICAL_RL && child->static_x_safe != 0)
		child->static_x_safe = child->static_x_safe == 1 ? 2 : 1;

	/* The containing block from the grid's lines. */
	if (!child->cb_set)
		return;
	left = child->cb_left;
	top = child->cb_top;
	edge = child->cb_right;
	child->cb_left = top;
	child->cb_right = child->cb_bottom;
	child->cb_top = left;
	child->cb_bottom = edge;
	flag = child->cb_left_auto;
	child->cb_left_auto = child->cb_top_auto;
	child->cb_top_auto = flag;
	flag = child->cb_right_auto;
	child->cb_right_auto = child->cb_bottom_auto;
	child->cb_bottom_auto = flag;
	if (mode == CSS_WRITING_VERTICAL_RL) {
		edge = child->cb_left;
		child->cb_left = width - child->cb_right;
		child->cb_right = width - edge;
		flag = child->cb_left_auto;
		child->cb_left_auto = child->cb_right_auto;
		child->cb_right_auto = flag;
	}
}

/*
 * Turns one laid out box's own geometry from a vertical writing mode's
 * logical coordinates into physical ones: its size and sides, and with
 * place its position in its parent's content box (parent_width wide).
 */
static void
logical_box_to_physical(
	struct layout_box *box,
	int mode,
	layout_unit parent_width,
	int place)
{
	layout_unit x;
	layout_unit y;
	layout_unit outer_height;
	layout_unit size;

	/* The logical position and the logical border-box block size. */
	x = box->x;
	y = box->y;
	outer_height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];

	/* The size: the inline size is the height. */
	size = box->width;
	box->width = box->height;
	box->height = size;

	/* The sides. */
	logical_units_to_physical(box->margin, mode);
	logical_units_to_physical(box->border, mode);
	logical_units_to_physical(box->padding, mode);

	/* The position: the block axis across, the inline axis down. */
	if (!place)
		return;
	box->y = x;
	box->x = y;
	if (mode == CSS_WRITING_VERTICAL_RL)
		box->x = parent_width - y - outer_height;
}

/*
 * Puts a laid out box (physical inside) into the logical coordinates of
 * the vertical domain around it, so that the domain's layout places it as
 * one box: its size and sides (its position is the domain's to give).
 */
static void
logical_box_to_logical(
	struct layout_box *box,
	int mode)
{
	layout_unit size;

	/* The size: the height is the inline size. */
	size = box->width;
	box->width = box->height;
	box->height = size;

	/* The sides. */
	logical_units_to_logical(box->margin, mode);
	logical_units_to_logical(box->border, mode);
	logical_units_to_logical(box->padding, mode);
}

/* Gives a domain its physical styles back. */
static void
logical_restore(
	struct layout_box *box)
{
	struct layout_box *child;

	/* The box's own style. */
	if (box->physical_style == NULL)
		return;
	memcpy(&box->style, box->physical_style, sizeof(box->style));
	box->physical_style = NULL;
	box->max_content_known = 0;

	/* The children of the domain. */
	for (child = box->first_child; child != NULL; child = child->next)
		logical_restore(child);
}
