/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Flexible box layout (ws074-p035), the common part of CSS Flexible Box
 * Layout 1: rows and columns (and their reverses), wrapping into lines,
 * the flex factors and basis, gaps, auto margins, justify-content,
 * align-items and align-self.  Each item is laid out as a block at the
 * main size the flexing gives it.  Not in this pass: align-content other
 * than start, wrap-reverse's order of lines, baselines (as start),
 * and percentages of an indefinite height.  An item whose main-axis
 * minimum is auto (the initial value) and whose content does not scroll
 * shrinks no smaller than its content (a row's min-content width, a
 * column's content height), or its main size when that is given in pixels
 * and smaller (the automatic minimum size), measured only for a line that
 * has to shrink. Non-replaced
 * preferred-ratio items instead establish their content-based minimum in
 * either axis before line collection, even with a zero flex basis.
 */

#include "layout/layout.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * One item on its way through the algorithm: its box, the frame (borders
 * and paddings) and margins along the main axis, which of those margins
 * are auto, its hypothetical and then its final content size along the
 * main axis, and the limits of that size.
 */
struct flex_item {
	struct layout_box *box;
	layout_unit frame;
	layout_unit margin_start;
	layout_unit margin_end;
	int auto_start;
	int auto_end;
	layout_unit base;
	layout_unit size;
	layout_unit minimum;
	layout_unit maximum;
	int auto_minimum;
	int frozen;
	int violation;
};

/*
 * The part of an item's style that its layout at the flexed size
 * overrides, kept to be put back afterwards.
 */
struct flex_sizes {
	int box_sizing;
	struct css_length width;
	struct css_length min_width;
	struct css_length max_width;
	struct css_length height;
	struct css_length min_height;
	struct css_length max_height;
	struct css_length margin[4];
};

static int flex_collect(struct layout_box *box, struct flex_item **items, size_t *count);
static int flex_content_basis(struct layout_tree *tree, struct layout_box *container, struct layout_box *child, int row, layout_unit *basis);
static int flex_base_size(struct layout_tree *tree, struct layout_box *box, struct flex_item *item, int row, layout_unit available);
static layout_unit flex_ratio_definite_length(const struct css_length *length, layout_unit basis);
static layout_unit flex_ratio_cross_size(struct layout_tree *tree, const struct layout_box *container, const struct layout_box *child, int row);
static layout_unit flex_ratio_main_size(const struct layout_box *box, int row, layout_unit cross);
static int flex_ratio_minimum(struct layout_tree *tree, struct layout_box *container, struct flex_item *item, int row, layout_unit available);
static int flex_auto_minimums(struct layout_tree *tree, struct layout_box *box, int row, struct flex_item *items, size_t count, layout_unit available, layout_unit gap);
static int flex_content_height(struct layout_tree *tree, struct layout_box *box, struct layout_box *child, layout_unit *height);
static int flex_min_content(struct layout_tree *tree, struct layout_box *box, layout_unit *width);
static void flex_grow(struct flex_item *items, size_t count, layout_unit available, layout_unit gap);
static void flex_shrink(struct flex_item *items, size_t count, layout_unit available, layout_unit gap);
static void flex_resolve(struct flex_item *items, size_t count, layout_unit available, layout_unit gap);
static int flex_lay_item(struct layout_tree *tree, struct layout_box *box, struct flex_item *item, int row);
static int flex_lay_sized(struct layout_tree *tree, struct layout_box *box, struct flex_item *item, int row);
static int flex_relay_cross(struct layout_tree *tree, struct layout_box *box, struct flex_item *items, size_t count, int row, layout_unit line_cross, layout_unit percentage_basis);
static layout_unit flex_divide_nearest(layout_unit numerator, layout_unit denominator);
static int flex_relay_column(struct layout_tree *tree, struct layout_box *box, struct flex_item *items, size_t count, layout_unit line_cross);
static void flex_static_positions(struct layout_box *box, int row, layout_unit content_height);
static void flex_place_line(struct layout_box *box, struct flex_item *items, size_t count, int row, layout_unit available, layout_unit gap, layout_unit cross_start, layout_unit line_cross);
static layout_unit flex_cross_outer(const struct layout_box *item, int row);
static int flex_align_of(const struct layout_box *box, const struct layout_box *item);
static int flex_baseline_above(const struct layout_box *box, const struct layout_box *item, int row, layout_unit *above);
static layout_unit flex_length(const struct css_length *length, layout_unit basis);

/* One flex line while the container's lines are being sized. */
struct flex_line {
	size_t start;
	size_t end;
	layout_unit cross;
};

/*
 * Lays out the content of a flex container: its in-flow children as flex
 * items, in lines along its main axis; sets their positions (their border
 * boxes relative to the container's content box) and the container's
 * content height.
 */
int
layout_flex(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct flex_item *items;
	struct layout_box *child;
	layout_unit available;
	layout_unit gap_main;
	layout_unit gap_cross;
	layout_unit used;
	layout_unit outer;
	layout_unit cross;
	layout_unit line_cross;
	layout_unit max_above[2];
	layout_unit max_below[2];
	layout_unit above;
	int group;
	layout_unit cross_cursor;
	layout_unit sizing;
	layout_unit percentage_basis;
	size_t count;
	size_t start;
	size_t end;
	size_t index;
	size_t place;
	struct flex_line *lines;
	size_t line_count;
	layout_unit cross_size;
	layout_unit row_basis;
	layout_unit free_space;
	layout_unit line_gap_extra;
	layout_unit content_cross;
	int first_line;
	int mirror;
	int value;
	int flex;
	int row;
	int wrap;
	int error;

	/* The children out of the flow have their static place at the content box's corner. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (!child->out_of_flow)
			continue;
		child->static_x = 0;
		child->static_y = 0;
	}

	/* The items, in their order. */
	error = flex_collect(box, &items, &count);
	if (error != 0)
		return error;

	/* The axes: the main size available (a column's only with a height of its own), and the gaps. */
	row = 0;
	if (box->style.flex_direction == CSS_FLEX_ROW || box->style.flex_direction == CSS_FLEX_ROW_REVERSE)
		row = 1;
	if (box->style.writing_mode == CSS_WRITING_VERTICAL_LR)
		row = !row;
	wrap = 0;
	if (box->style.flex_wrap != CSS_FLEX_NOWRAP)
		wrap = 1;
	available = box->width;
	/* A percentage row gap is of the definite height; an indefinite one counts as zero. */
	row_basis = 0;
	if (box->style.height.unit == CSS_UNIT_PX) {
		row_basis = layout_from_px(box->style.height.value);
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
			row_basis -= box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		if (row_basis < 0)
			row_basis = 0;
	}
	gap_main = flex_length(&box->style.column_gap, box->width);
	gap_cross = flex_length(&box->style.row_gap, row_basis);
	if (!row) {
		available = -1;
		if (box->style.height.unit == CSS_UNIT_PX) {
			sizing = 0;
			if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
				sizing = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
			available = layout_from_px(box->style.height.value) - sizing;
		} else if (wrap && box->style.max_height.unit == CSS_UNIT_PX) {
			/* A wrapping column breaks its lines at its maximum height. */
			sizing = 0;
			if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
				sizing = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
			available = layout_from_px(box->style.max_height.value) - sizing;
			if (available < 0)
				available = 0;
		}

		/* A column's main axis is vertical: the gaps swap. */
		gap_main = flex_length(&box->style.row_gap, row_basis);
		gap_cross = flex_length(&box->style.column_gap, box->width);
	}

	/* Each item's hypothetical main size. */
	for (index = 0; index < count; index++) {
		error = flex_base_size(tree, box, &items[index], row, available);
		if (error != 0) {
			free(items);
			return error;
		}
	}
	/* An auto-height, unwrapped column uses its minimum height for free-space placement. */
	if (!row && !wrap && box->style.height.unit == CSS_UNIT_AUTO &&
	    box->style.min_height.unit == CSS_UNIT_PX && box->style.max_height.unit == CSS_UNIT_NONE) {
		sizing = 0;
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
			sizing = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		available = layout_from_px(box->style.min_height.value) - sizing;
		if (available < 0)
			available = 0;
		used = 0;
		for (index = 0; index < count; index++)
			used += items[index].size + items[index].frame + items[index].margin_start + items[index].margin_end;
		if (count > 1)
			used += gap_main * (layout_unit)(count - 1U);
		if (available < used)
			available = used;
	}

	/* Line by line: the items that fit, flexed and laid out; their cross sizes decide the lines'. */
	lines = NULL;
	line_count = 0;
	if (count != 0) {
		lines = calloc(count, sizeof(*lines));
		if (lines == NULL) {
			free(items);
			return ENOMEM;
		}
	}
	start = 0;
	while (start < count) {
		/* The items of the line: all of them without wrapping or a size to wrap in. */
		end = start;
		used = 0;
		while (end < count) {
			outer = items[end].size + items[end].frame + items[end].margin_start + items[end].margin_end;
			if (end > start)
				outer += gap_main;
			if (wrap && available >= 0 && end > start && used + outer > available)
				break;
			used += outer;
			end++;
		}

		/* The automatic minimums of a line that has to shrink. */
		error = flex_auto_minimums(tree, box, row, items + start, end - start, available, gap_main);
		if (error != 0) {
			free(lines);
			free(items);
			return error;
		}

		/* The free space shared, then each item laid out at its size. */
		flex_resolve(items + start, end - start, available, gap_main);
		line_cross = 0;
		max_above[0] = max_above[1] = 0;
		max_below[0] = max_below[1] = 0;
		for (index = start; index < end; index++) {
			error = flex_lay_item(tree, box, &items[index], row);
			if (error != 0) {
				free(lines);
				free(items);
				return error;
			}

			/* The line is as thick as its thickest item. */
			cross = flex_cross_outer(items[index].box, row);
			if (cross > line_cross)
				line_cross = cross;

			/* Baseline-aligned items need room above and below their shared baseline (each group its own). */
			group = flex_baseline_above(box, items[index].box, row, &above);
			if (group != 0) {
				if (above > max_above[group - 1])
					max_above[group - 1] = above;
				if (cross - above > max_below[group - 1])
					max_below[group - 1] = cross - above;
			}
		}
		for (group = 0; group < 2; group++) {
			if (max_above[group] + max_below[group] > line_cross)
				line_cross = max_above[group] + max_below[group];
		}

		/* A single line of a row fills a container of a height of its own; a column's line fills its width. */
		if (!wrap && row && box->style.height.unit == CSS_UNIT_PX) {
			sizing = 0;
			if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
				sizing = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
			line_cross = layout_from_px(box->style.height.value) - sizing;
		}

		/* A column's single line is as wide as the container. */
		if (!wrap && !row)
			line_cross = box->width;

		lines[line_count].start = start;
		lines[line_count].end = end;
		lines[line_count].cross = line_cross;
		line_count++;
		start = end;
	}

	/* A wrapping container with a definite cross size shares its free space among the lines (align-content). */
	cross_size = -1;
	if (row && box->style.height.unit == CSS_UNIT_PX) {
		sizing = 0;
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
			sizing = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		cross_size = layout_from_px(box->style.height.value) - sizing;
	}
	if (!row)
		cross_size = box->width;
	cross_cursor = 0;
	line_gap_extra = 0;
	if (wrap && cross_size >= 0 && line_count != 0) {
		used = gap_cross * (layout_unit)(line_count - 1U);
		for (index = 0; index < line_count; index++)
			used += lines[index].cross;
		free_space = cross_size - used;
		value = box->style.align_content;
		flex = (box->style.flex_keywords & 2u) != 0;
		if (box->style.flex_wrap == CSS_FLEX_WRAP_REVERSE && !flex) {
			/* start and end follow the writing mode, which wrap-reverse turns around. */
			if (value == CSS_ALIGN_START)
				value = CSS_ALIGN_END;
			else if (value == CSS_ALIGN_END)
				value = CSS_ALIGN_START;
		}
		if (free_space < 0 && (value == CSS_ALIGN_SPACE_BETWEEN || value == CSS_ALIGN_STRETCH ||
		    value == CSS_ALIGN_NORMAL))
			value = CSS_ALIGN_START;
		if (free_space < 0 && (value == CSS_ALIGN_SPACE_AROUND || value == CSS_ALIGN_SPACE_EVENLY))
			value = CSS_ALIGN_START;
		switch (value) {
		case CSS_ALIGN_STRETCH:
		case CSS_ALIGN_NORMAL:
			for (index = 0; index < line_count; index++)
				lines[index].cross += flex_divide_nearest(free_space, (layout_unit)line_count);
			break;
		case CSS_ALIGN_END:
			cross_cursor = free_space;
			break;
		case CSS_ALIGN_CENTER:
			cross_cursor = free_space / 2;
			break;
		case CSS_ALIGN_SPACE_BETWEEN:
			if (line_count > 1)
				line_gap_extra = flex_divide_nearest(free_space, (layout_unit)(line_count - 1U));
			break;
		case CSS_ALIGN_SPACE_AROUND:
			cross_cursor = flex_divide_nearest(free_space, (layout_unit)(line_count * 2U));
			line_gap_extra = flex_divide_nearest(free_space, (layout_unit)line_count);
			break;
		case CSS_ALIGN_SPACE_EVENLY:
			cross_cursor = flex_divide_nearest(free_space, (layout_unit)(line_count + 1U));
			line_gap_extra = cross_cursor;
			break;
		default:
			break;
		}
	}

	/* Each line: its items stretched across and placed. */
	first_line = 1;
	content_cross = 0;
	for (index = 0; index < line_count; index++) {
		line_cross = lines[index].cross;
		percentage_basis = -1;
		if (!wrap && row && box->style.height.unit == CSS_UNIT_PX)
			percentage_basis = line_cross;
		error = flex_relay_cross(tree, box, items + lines[index].start, lines[index].end - lines[index].start,
		    row, line_cross, percentage_basis);
		if (error != 0) {
			free(lines);
			free(items);
			return error;
		}

		/* The items' places along both axes. */
		if (!first_line) {
			cross_cursor += gap_cross + line_gap_extra;
			content_cross += gap_cross;
		}
		first_line = 0;
		for (place = lines[index].start; place < lines[index].end; place++) {
			items[place].box->item_row = (int)index + 1;
			items[place].box->item_column = (int)(place - lines[index].start) + 1;
		}
		flex_place_line(box, items + lines[index].start, lines[index].end - lines[index].start, row, available,
		    gap_main, cross_cursor, line_cross);
		cross_cursor += line_cross;
		content_cross += line_cross;
	}

	/* wrap-reverse stacks the lines from the far cross edge: mirrors every item across the container's cross extent. */
	mirror = (box->style.flex_wrap == CSS_FLEX_WRAP_REVERSE) !=
	    (!row && box->style.direction == CSS_DIRECTION_RTL &&
	    box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB);
	if (mirror && line_count != 0) {
		layout_unit extent;

		extent = cross_size >= 0 ? cross_size : cross_cursor;
		for (index = 0; index < count; index++) {
			child = items[index].box;
			cross = flex_cross_outer(child, row);
			if (row)
				child->y = extent - (child->y - child->margin[CSS_TOP] + cross) + child->margin[CSS_TOP];
			else
				child->x = extent - (child->x - child->margin[CSS_LEFT] + cross) + child->margin[CSS_LEFT];
		}
	}
	free(lines);

	/* The container's content height: its lines' (a row's), or its items' (a column's). */
	box->height = content_cross;
	if (!row) {
		box->height = 0;
		for (index = 0; index < count; index++) {
			child = items[index].box;
			outer = child->y + child->border[CSS_TOP] + child->padding[CSS_TOP] + child->height +
			    child->padding[CSS_BOTTOM] + child->border[CSS_BOTTOM] + child->margin[CSS_BOTTOM];
			if (outer > box->height)
				box->height = outer;
		}
	}

	/* The items are placed; the absolute children take the place a sole item would. */
	sizing = box->height;
	if (box->style.height.unit == CSS_UNIT_PX) {
		sizing = layout_from_px(box->style.height.value);
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
			sizing -= box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	}
	flex_static_positions(box, row, sizing);
	free(items);

	/* Succeeded: the container's content is laid out. */
	return 0;
}

/* The sides a static position can take along one physical axis. */
enum flex_side {
	FLEX_SIDE_START,
	FLEX_SIDE_CENTER,
	FLEX_SIDE_END
};

/* Flips a side. */
static int
flex_side_flip(
	int side)
{
	if (side == FLEX_SIDE_START)
		return FLEX_SIDE_END;
	if (side == FLEX_SIDE_END)
		return FLEX_SIDE_START;
	return side;
}

/*
 * Gives the absolute children of a flex container the static position of
 * a sole flex item: justify-content along the main axis and align-self
 * across it.  Sides are physical (start is left or top).
 */
static void
flex_static_positions(
	struct layout_box *box,
	int row,
	layout_unit content_height)
{
	struct layout_box *child;
	int rtl;
	int reverse;
	int wrap_reverse;
	int inline_horizontal;
	int inline_start;
	int block_start;
	int main_start;
	int cross_start;
	int main_axis_start;
	int cross_axis_start;
	int main_horizontal;
	int main_side;
	int cross_side;
	int horizontal;
	int vertical;
	int value;
	int flex;
	int safe_main;
	int safe_cross;
	int child_inline_start;
	int child_block_start;
	int physical;
	int physical_vertical;
	int physical_rl;

	if ((box->style.direction != CSS_DIRECTION_LTR && box->style.direction != CSS_DIRECTION_RTL))
		return;
	rtl = box->style.direction == CSS_DIRECTION_RTL;
	reverse = box->style.flex_direction == CSS_FLEX_ROW_REVERSE ||
	    box->style.flex_direction == CSS_FLEX_COLUMN_REVERSE;
	wrap_reverse = box->style.flex_wrap == CSS_FLEX_WRAP_REVERSE;

	/* The physical sides where the inline axis and the block axis start. */
	inline_horizontal = box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB;
	inline_start = rtl ? FLEX_SIDE_END : FLEX_SIDE_START;
	block_start = box->style.writing_mode == CSS_WRITING_VERTICAL_RL ? FLEX_SIDE_END : FLEX_SIDE_START;

	/* A row's main axis is the inline axis. */
	main_axis_start = row ? inline_start : block_start;
	cross_axis_start = row ? block_start : inline_start;
	main_horizontal = row ? inline_horizontal : !inline_horizontal;
	main_start = reverse ? flex_side_flip(main_axis_start) : main_axis_start;

	/*
	 * A vertical container laid out in its logical coordinates: left and
	 * right still name the physical sides, which its block axis (vertical
	 * here) runs between, from the right in vertical-rl.
	 */
	physical = layout_physical_mode(box);
	physical_vertical = physical != box->style.writing_mode;
	physical_rl = physical == CSS_WRITING_VERTICAL_RL;
	cross_start = wrap_reverse ? flex_side_flip(cross_axis_start) : cross_axis_start;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (!child->out_of_flow || child->style.position != CSS_POSITION_ABSOLUTE)
			continue;

		/* justify-content decides the side along the main axis. */
		value = box->style.justify_content;
		flex = (box->style.flex_keywords & 1u) != 0;
		main_side = main_start;
		switch (value) {
		case CSS_ALIGN_CENTER:
		case CSS_ALIGN_SPACE_AROUND:
		case CSS_ALIGN_SPACE_EVENLY:
			main_side = FLEX_SIDE_CENTER;
			break;
		case CSS_ALIGN_START:
			main_side = flex ? main_start : main_axis_start;
			break;
		case CSS_ALIGN_END:
			main_side = flex ? flex_side_flip(main_start) : flex_side_flip(main_axis_start);
			break;
		case CSS_ALIGN_LEFT:
			main_side = (main_horizontal || !inline_horizontal) ? FLEX_SIDE_START : main_axis_start;
			if (physical_vertical && !main_horizontal)
				main_side = physical_rl ? FLEX_SIDE_END : FLEX_SIDE_START;
			break;
		case CSS_ALIGN_RIGHT:
			main_side = (main_horizontal || !inline_horizontal) ? FLEX_SIDE_END : main_axis_start;
			if (physical_vertical && !main_horizontal)
				main_side = physical_rl ? FLEX_SIDE_START : FLEX_SIDE_END;
			break;
		default:
			main_side = main_start;
			break;
		}

		/* align-self decides the side across it. */
		value = child->style.align_self;
		flex = (child->style.flex_keywords & 8u) != 0;
		if (value == CSS_ALIGN_AUTO) {
			value = box->style.align_items;
			flex = (box->style.flex_keywords & 4u) != 0;
		}
		child_inline_start = child->style.direction == CSS_DIRECTION_RTL ? FLEX_SIDE_END : FLEX_SIDE_START;
		child_block_start = child->style.writing_mode == CSS_WRITING_VERTICAL_RL ? FLEX_SIDE_END : FLEX_SIDE_START;
		switch (value) {
		case CSS_ALIGN_CENTER:
			cross_side = FLEX_SIDE_CENTER;
			break;
		case CSS_ALIGN_BASELINE:
			cross_side = cross_axis_start;
			break;
		case CSS_ALIGN_START:
			cross_side = flex ? cross_start : cross_axis_start;
			break;
		case CSS_ALIGN_END:
			cross_side = flex ? flex_side_flip(cross_start) : flex_side_flip(cross_axis_start);
			break;
		case CSS_ALIGN_SELF_START:
			cross_side = row ? child_block_start : child_inline_start;
			break;
		case CSS_ALIGN_SELF_END:
			cross_side = flex_side_flip(row ? child_block_start : child_inline_start);
			break;
		default:
			cross_side = cross_start;
			break;
		}

		/* The horizontal and vertical sides. */
		horizontal = main_horizontal ? main_side : cross_side;
		vertical = main_horizontal ? cross_side : main_side;
		child->static_x = 0;
		child->static_y = 0;
		child->static_x_from_right = 0;
		child->static_x_align = CSS_ALIGN_AUTO;
		child->static_x_align_from_right = 0;
		child->static_y_align = CSS_ALIGN_AUTO;
		child->static_x_area_width = box->width;
		child->static_y_area_height = content_height;
		child->static_unsafe = 1;
		child->static_x_safe = 0;
		child->static_y_safe = 0;
		safe_main = (box->style.flex_keywords & 16u) != 0;
		safe_cross = (child->style.align_self != CSS_ALIGN_AUTO) ? (child->style.flex_keywords & 128u) != 0 : (box->style.flex_keywords & 64u) != 0;
		child->static_x_safe = main_horizontal ? safe_main : safe_cross;
		child->static_y_safe = main_horizontal ? safe_cross : safe_main;
		/* A safe overflow falls back to the start edge of the axis: 2 when that is the right or bottom one. */
		if (child->static_x_safe && (main_horizontal ? main_axis_start : cross_axis_start) == FLEX_SIDE_END)
			child->static_x_safe = 2;
		if (child->static_y_safe && (main_horizontal ? cross_axis_start : main_axis_start) == FLEX_SIDE_END)
			child->static_y_safe = 2;
		if (horizontal == FLEX_SIDE_CENTER) {
			child->static_x_align = CSS_ALIGN_CENTER;
		} else if (horizontal == FLEX_SIDE_END) {
			child->static_x_align = CSS_ALIGN_RIGHT;
			child->static_x_align_from_right = 1;
		}
		if (vertical == FLEX_SIDE_CENTER)
			child->static_y_align = CSS_ALIGN_CENTER;
		else if (vertical == FLEX_SIDE_END)
			child->static_y_align = CSS_ALIGN_END;
	}
}

/*
 * Measures the width a laid out row flex container's content needs: its
 * items' hypothetical margin boxes side by side, with the gaps between.
 */
layout_unit
layout_flex_content_width(
	const struct layout_box *box)
{
	const struct layout_box *child;
	layout_unit width;
	layout_unit gap;
	int items;

	/* Each item's margin box before it flexed. */
	width = 0;
	items = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow)
			continue;
		width += child->flex_hypothetical;
		items++;
	}

	/* The gaps between them. */
	gap = flex_length(&box->style.column_gap, 0);
	if (items > 1)
		width += gap * (items - 1);

	/* Reports the width. */
	return width;
}

/* Gathers a container's in-flow children, stably sorted by order, into a new array. */
static int
flex_collect(
	struct layout_box *box,
	struct flex_item **items,
	size_t *count)
{
	struct flex_item *list;
	struct flex_item moving;
	struct layout_box *child;
	size_t made;
	size_t index;
	size_t place;

	/* Counts the in-flow children. */
	made = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (!child->out_of_flow)
			made++;
	}

	/* The array (one place at least, so that none is not NULL). */
	list = calloc(made + 1U, sizeof(*list));
	if (list == NULL)
		return ENOMEM;

	/* Each child, inserted after the ones of the same or a smaller order. */
	made = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow)
			continue;
		list[made].box = child;
		place = made;
		while (place > 0 && list[place - 1U].box->style.order > child->style.order) {
			moving = list[place - 1U];
			list[place] = moving;
			place--;
		}

		/* The child takes the place made for it. */
		list[place].box = child;
		made++;
	}

	/* The other fields start empty. */
	for (index = 0; index < made; index++) {
		child = list[index].box;
		memset(&list[index], 0, sizeof(list[index]));
		list[index].box = child;
	}

	/* Succeeded: the items are gathered. */
	*items = list;
	*count = made;
	return 0;
}

/* Measures a content basis without borrowing or caching the preferred main size. */
static int
flex_content_basis(
	struct layout_tree *tree,
	struct layout_box *container,
	struct layout_box *child,
	int row,
	layout_unit *basis)
{
	struct css_length preferred;
	struct css_length minimum;
	struct css_length maximum;
	struct layout_context context;
	struct wb_vector floats;
	layout_unit cached_width;
	int cached_known;
	int error;

	/* Saves the preferred size, main limits, and cache owned by ordinary layout. */
	cached_width = child->max_content;
	cached_known = child->max_content_known;
	preferred = child->style.width;
	minimum = child->style.min_width;
	maximum = child->style.max_width;
	if (!row) {
		preferred = child->style.height;
		minimum = child->style.min_height;
		maximum = child->style.max_height;
	}

	/* Content is measured before main-axis limits or preferred sizes apply. */
	child->max_content_known = 0;
	if (row) {
		child->style.width.unit = CSS_UNIT_AUTO;
		child->style.min_width.unit = CSS_UNIT_AUTO;
		child->style.max_width.unit = CSS_UNIT_NONE;
	} else {
		child->style.height.unit = CSS_UNIT_AUTO;
		child->style.min_height.unit = CSS_UNIT_AUTO;
		child->style.max_height.unit = CSS_UNIT_NONE;
	}

	/* Isolates intrinsic width or natural height from the surrounding float context. */
	if (row) {
		error = layout_max_content(tree, child, basis);
	} else {
		tree->measuring++;
		layout_context_begin(tree, &context, &floats);
		error = layout_block(tree, child, container->width);
		layout_context_end(tree, &context);
		tree->measuring--;

		/* Reads the measured height only after the complete layout succeeded. */
		if (error == 0)
			*basis = child->height;
	}

	/* Restores ordinary layout state even when the measurement failed. */
	child->max_content = cached_width;
	child->max_content_known = cached_known;
	if (row) {
		child->style.width = preferred;
		child->style.min_width = minimum;
		child->style.max_width = maximum;
	} else {
		child->style.height = preferred;
		child->style.min_height = minimum;
		child->style.max_height = maximum;
	}

	/* Propagates a failed intrinsic measurement after restoring borrowed state. */
	if (error != 0)
		return error;

	/* Succeeded: the base contains only the unconstrained content measurement. */
	return 0;
}

/*
 * Finds an item's frame and margins along the main axis and its
 * hypothetical main size: its flex basis (or its width or height), or its
 * content's size, held between its minimum and maximum.
 */
static int
flex_base_size(
	struct layout_tree *tree,
	struct layout_box *box,
	struct flex_item *item,
	int row,
	layout_unit available)
{
	struct layout_box *child;
	const struct css_length *basis;
	layout_unit sizing;
	layout_unit cross;
	layout_unit resolved;
	int start_side;
	int end_side;
	int error;

	/* The item's box model against the container's width. */
	child = item->box;
	layout_box_model(child, box->width);

	/* The sides the main axis starts and ends at: left and right for a row, top and bottom for a column. */
	start_side = CSS_TOP;
	end_side = CSS_BOTTOM;
	if (row) {
		start_side = CSS_LEFT;
		end_side = CSS_RIGHT;
	}

	/* Its frame and margins along the main axis (an auto margin counts as none until the free space is shared). */
	item->frame = child->border[start_side] + child->padding[start_side] + child->padding[end_side] + child->border[end_side];
	item->margin_start = child->margin[start_side];
	item->margin_end = child->margin[end_side];

	/* An auto margin at the start takes a share of the free space later. */
	item->auto_start = 0;
	if (child->style.margin[start_side].unit == CSS_UNIT_AUTO)
		item->auto_start = 1;

	/* So does one at the end. */
	item->auto_end = 0;
	if (child->style.margin[end_side].unit == CSS_UNIT_AUTO)
		item->auto_end = 1;

	/* Under border-box sizing the given sizes include the frame. */
	sizing = 0;
	if (child->style.box_sizing == CSS_BOX_SIZING_BORDER)
		sizing = item->frame;

	/* The basis: flex-basis, or the width or height when it is auto. */
	basis = &child->style.flex_basis;
	if (basis->unit == CSS_UNIT_AUTO) {
		basis = &child->style.width;

		/* A column borrows its preferred height instead of its width. */
		if (!row)
			basis = &child->style.height;
	}

	/*
	 * A definite basis: pixels, or a percentage of a definite main size (a
	 * container being measured has none: its width is only the measuring
	 * width).
	 */
	item->base = -1;
	if (basis->unit == CSS_UNIT_PX)
		item->base = layout_from_px(basis->value) - sizing;

	/* A percentage math result is clamped only after its basis becomes definite. */
	if (basis->unit == CSS_UNIT_PERCENT &&
	    available >= 0 &&
	    tree->measuring == 0) {
		resolved = flex_length(basis, available);

		/* A negative used length is zero before the box frame is removed. */
		if (resolved < 0)
			resolved = 0;

		/* The base is measured inside the frame for border-box sizing. */
		item->base = resolved - sizing;
	}

	/* A definite cross size supplies the content basis through the ratio. */
	if (item->base < 0) {
		/* Replaced content keeps its separate intrinsic sizing path. */
		if (!child->replaced) {
			/* A preferred ratio can transfer a definite cross size. */
			if (child->style.aspect_ratio > 0.0f) {
				/* Include single-line stretch before any flexing takes place. */
				cross = flex_ratio_cross_size(tree, box, child, row);
				if (cross >= 0)
					item->base = flex_ratio_main_size(child, row, cross);
			}
		}
	}

	/* An image's natural ratio transfers a definite cross size too. */
	if (item->base < 0 && child->replaced && child->style.aspect_ratio <= 0.0f &&
	    child->image != NULL && child->image->width > 0 && child->image->height > 0) {
		cross = flex_ratio_cross_size(tree, box, child, row);
		if (cross >= 0) {
			if (child->style.box_sizing == CSS_BOX_SIZING_BORDER && row)
				cross -= child->border[CSS_TOP] + child->padding[CSS_TOP] + child->padding[CSS_BOTTOM] +
				    child->border[CSS_BOTTOM];
			else if (child->style.box_sizing == CSS_BOX_SIZING_BORDER)
				cross -= child->border[CSS_LEFT] + child->padding[CSS_LEFT] + child->padding[CSS_RIGHT] +
				    child->border[CSS_RIGHT];
			if (cross < 0)
				cross = 0;
			if (row)
				item->base = (layout_unit)((double)cross * child->image->width / child->image->height);
			else
				item->base = (layout_unit)((double)cross * child->image->height / child->image->width);
		}
	}

	/* An explicit content basis ignores the non-replaced preferred main size. */
	if (item->base < 0 &&
	    basis->unit == CSS_UNIT_CONTENT &&
	    !child->replaced) {
		error = flex_content_basis(tree, box, child, row, &item->base);
		if (error != 0)
			return error;
	}

	/* Otherwise the content's size: its widest line, or its height at the container's width. */
	if (item->base < 0 && row) {
		/* Replaced items obtain their width from ordinary replaced layout. */
		if (child->replaced) {
			error = layout_block(tree, child, box->width);
			if (error != 0)
				return error;

			/* Uses the replaced content width as its initial base. */
			item->base = child->width;
		} else {
			/* The content's width without a limit (measured once, its percentages indefinite). */
			error = layout_max_content(tree, child, &item->base);
			if (error != 0)
				return error;
		}
	} else if (item->base < 0) {
		error = layout_block(tree, child, box->width);
		if (error != 0)
			return error;

		/* Uses the natural block height as the column content size. */
		item->base = child->height;
	}

	/* A size is never negative. */
	if (item->base < 0)
		item->base = 0;

	/* The limits: min-width and max-width (min-height and max-height for a column). */
	item->minimum = 0;
	item->maximum = -1;
	item->auto_minimum = 0;
	if (row &&
	    child->style.min_width.unit == CSS_UNIT_AUTO &&
	    (child->style.overflow_x == CSS_OVERFLOW_VISIBLE || child->style.overflow_x == CSS_OVERFLOW_CLIP))
		item->auto_minimum = 1;
	if (!row && child->style.aspect_ratio <= 0.0f && child->style.writing_mode == box->style.writing_mode &&
	    child->style.min_height.unit == CSS_UNIT_AUTO &&
	    (child->style.overflow_y == CSS_OVERFLOW_VISIBLE || child->style.overflow_y == CSS_OVERFLOW_CLIP))
		item->auto_minimum = 1;

	/* Resolves the main-axis limits after the unconstrained base is measured. */
	if (row) {
		/* An explicit width minimum limits the eventual hypothetical size. */
		if (child->style.min_width.unit == CSS_UNIT_PX || child->style.min_width.unit == CSS_UNIT_PERCENT)
			item->minimum = flex_length(&child->style.min_width, box->width) - sizing;

		/* A finite width maximum limits the eventual hypothetical size. */
		if (child->style.max_width.unit == CSS_UNIT_PX || child->style.max_width.unit == CSS_UNIT_PERCENT)
			item->maximum = flex_length(&child->style.max_width, box->width) - sizing;
	} else {
		/* Explicit height limits apply on a column main axis (percentages of a definite height). */
		if (child->style.min_height.unit == CSS_UNIT_PX ||
		    (child->style.min_height.unit == CSS_UNIT_PERCENT && available >= 0))
			item->minimum = flex_length(&child->style.min_height, available) - sizing;

		/* A finite height maximum is independent of the measured base. */
		if (child->style.max_height.unit == CSS_UNIT_PX ||
		    (child->style.max_height.unit == CSS_UNIT_PERCENT && available >= 0))
			item->maximum = flex_length(&child->style.max_height, available) - sizing;
	}

	/* Ratio items need their content floor before line breaking or flex growth. */
	error = flex_ratio_minimum(tree, box, item, row, available);
	if (error != 0)
		return error;

	/* Neither is the minimum. */
	if (item->minimum < 0)
		item->minimum = 0;

	/* The base stays unconstrained; the hypothetical size is limited for line breaking. */
	item->size = item->base;
	if (item->maximum >= 0 && item->size > item->maximum)
		item->size = item->maximum;

	/* The minimum overrides a conflicting maximum in the hypothetical size. */
	if (item->size < item->minimum)
		item->size = item->minimum;

	/* The container's measure of its content counts the item's margin box at that size. */
	child->flex_hypothetical = item->size + item->frame + item->margin_start + item->margin_end;

	/* Succeeded: the item has its hypothetical size. */
	return 0;
}

/*
 * Grows unfrozen items from their bases, freezing a max-size violation and
 * redistributing the resulting leftover space until no item crosses a limit.
 */
static void
flex_grow(
	struct flex_item *items,
	size_t count,
	layout_unit available,
	layout_unit gap)
{
	layout_unit used;
	layout_unit remaining;
	layout_unit share;
	layout_unit target;
	int64_t total_violation;
	float total;
	float weight;
	size_t index;
	size_t round;
	int sign;

	/* Inflexible items and bases already above a maximum keep their hypothetical size. */
	for (index = 0; index < count; index++) {
		items[index].frozen = items[index].box->style.flex_grow <= 0 ||
		    items[index].base > items[index].size;
	}

	/* Each violation freezes at least one item, so the bounded loop terminates. */
	for (round = 0; round <= count; round++) {
		used = 0;
		total = 0;
		for (index = 0; index < count; index++) {
			used += items[index].frame + items[index].margin_start + items[index].margin_end;
			if (items[index].frozen) {
				used += items[index].size;
			} else {
				used += items[index].base;
				total += items[index].box->style.flex_grow;
			}
		}
		if (count > 1)
			used += gap * (layout_unit)(count - 1U);
		remaining = available - used;
		if (remaining <= 0 || total <= 0) {
			for (index = 0; index < count; index++) {
				if (!items[index].frozen) {
					items[index].size = items[index].base;
				if (items[index].size < items[index].minimum)
						items[index].size = items[index].minimum;
				if (items[index].maximum >= 0 && items[index].size > items[index].maximum)
						items[index].size = items[index].maximum;
				}
			}
			return;
		}

		/* Factors below one leave a proportional part of the free space unused. */
		weight = total;
		if (weight < 1)
			weight = 1;
		total_violation = 0;
		for (index = 0; index < count; index++) {
			if (items[index].frozen)
				continue;
			share = (layout_unit)((float)remaining * items[index].box->style.flex_grow / weight);
			target = items[index].base + share;
			items[index].size = target;
			items[index].violation = 0;
			if (items[index].size < items[index].minimum) {
				items[index].size = items[index].minimum;
				items[index].violation = 1;
			}
			if (items[index].maximum >= 0 && items[index].size > items[index].maximum) {
				items[index].size = items[index].maximum;
				items[index].violation = -1;
			}
			total_violation += (int64_t)items[index].size - target;
		}

		/* Freeze only the min or max violations that dominate this round. */
		if (total_violation == 0)
			return;
		sign = total_violation > 0 ? 1 : -1;
		for (index = 0; index < count; index++) {
			if (items[index].violation == sign)
				items[index].frozen = 1;
		}
	}
}

/*
 * Shrinks the items of a line that do not fit (CSS Flexbox 9.7): the
 * space missing is taken from the items that are not frozen, in
 * proportion to their flex-shrink times their base; an item that would go
 * below its minimum is held there and frozen, and the rest share what is
 * still missing, until no item goes below its minimum.
 */
static void
flex_shrink(
	struct flex_item *items,
	size_t count,
	layout_unit available,
	layout_unit gap)
{
	layout_unit used;
	layout_unit missing;
	layout_unit share;
	float total;
	float weight;
	size_t index;
	size_t round;
	int violated;

	/* Inflexible items and bases below a minimum keep their hypothetical size. */
	for (index = 0; index < count; index++) {
		items[index].frozen = items[index].box->style.flex_shrink <= 0 ||
		    items[index].base < items[index].size;
	}

	/* At most one round per item, as each round freezes one at least. */
	for (round = 0; round <= count; round++) {
		/* The space the frozen items take at their sizes and the others at their bases. */
		used = 0;
		total = 0;
		for (index = 0; index < count; index++) {
			used += items[index].frame + items[index].margin_start + items[index].margin_end;
			if (items[index].frozen) {
				used += items[index].size;
			} else {
				used += items[index].base;
				total += items[index].box->style.flex_shrink * (float)items[index].base;
			}
		}

		/* The gaps take room too, and the rest is what is missing. */
		if (count > 1)
			used += gap * (layout_unit)(count - 1U);
		missing = used - available;

		/* Enough room now, or nothing left that shrinks: the others keep their bases. */
		if (missing <= 0 || total <= 0) {
			for (index = 0; index < count; index++) {
				if (!items[index].frozen) {
					items[index].size = items[index].base;
				if (items[index].size < items[index].minimum)
						items[index].size = items[index].minimum;
				if (items[index].maximum >= 0 && items[index].size > items[index].maximum)
						items[index].size = items[index].maximum;
				}
			}

			/* The sizes stand. */
			return;
		}

		/* Each other item's part of the missing space; the ones below their minimums are held and frozen. */
		violated = 0;
		for (index = 0; index < count; index++) {
			if (items[index].frozen)
				continue;
			weight = items[index].box->style.flex_shrink * (float)items[index].base;
			share = (layout_unit)((float)missing * weight / total);
			items[index].size = items[index].base - share;
			if (items[index].size < items[index].minimum) {
				items[index].size = items[index].minimum;
				items[index].frozen = 1;
				violated = 1;
			}
			if (items[index].maximum >= 0 && items[index].size > items[index].maximum) {
				items[index].size = items[index].maximum;
				items[index].frozen = 1;
				violated = 1;
			}
		}

		/* No item held: the sizes stand. */
		if (!violated)
			return;
	}
}

/* Resolves only lengths whose percentage basis is definite. */
static layout_unit
flex_ratio_definite_length(
	const struct css_length *length,
	layout_unit basis)
{
	layout_unit resolved;

	/* Pixel lengths do not depend on a containing size. */
	if (length->unit == CSS_UNIT_PX) {
		resolved = layout_from_px(length->value);
		return resolved;
	}

	/* Percentages need a definite basis, including when that basis is zero. */
	if (length->unit != CSS_UNIT_PERCENT)
		return -1;

	/* An indefinite containing dimension cannot resolve this percentage. */
	if (basis < 0)
		return -1;

	/* Succeeded: includes the absolute term of a calculated percentage. */
	resolved = flex_length(length, basis);
	return resolved;
}

/* Finds the definite cross size, including an eligible single-line stretch. */
static layout_unit
flex_ratio_cross_size(
	struct layout_tree *tree,
	const struct layout_box *container,
	const struct layout_box *child,
	int row)
{
	const struct css_length *length;
	const struct css_length *minimum;
	const struct css_length *maximum;
	layout_unit basis;
	layout_unit cross;
	layout_unit frame;
	layout_unit limit;
	int first;
	int last;
	int align;

	/* A row's cross percentages need the container's definite height. */
	length = &child->style.height;
	minimum = &child->style.min_height;
	maximum = &child->style.max_height;
	first = CSS_TOP;
	last = CSS_BOTTOM;
	basis = -1;
	if (row) {
		if (tree->containing_height_definite)
			basis = tree->containing_height;
	} else {
		/* A column's cross size uses the container's content width. */
		length = &child->style.width;
		minimum = &child->style.min_width;
		maximum = &child->style.max_width;
		first = CSS_LEFT;
		last = CSS_RIGHT;
		basis = container->width;
	}

	/* Intrinsic measurement does not establish a percentage or stretch basis. */
	if (tree->measuring != 0)
		basis = -1;

	/* A definite author-specified cross size needs no stretch inference. */
	cross = flex_ratio_definite_length(length, basis);
	frame = child->border[first] + child->padding[first] + child->padding[last] + child->border[last];
	if (cross < 0) {
		/* Only an automatic cross size can stretch. */
		if (length->unit != CSS_UNIT_AUTO)
			return -1;

		/* Multiple lines determine their cross sizes after main sizing. */
		if (container->style.flex_wrap != CSS_FLEX_NOWRAP)
			return -1;

		/* An indefinite container does not determine the item's cross size. */
		if (basis < 0)
			return -1;

		/* Non-stretch alignment uses a content-based cross size. */
		align = flex_align_of(container, child);
		if (align != CSS_ALIGN_STRETCH)
			return -1;

		/* Auto cross margins consume the line's space instead of stretching. */
		if (child->style.margin[first].unit == CSS_UNIT_AUTO)
			return -1;

		/* Either cross margin can prevent stretch. */
		if (child->style.margin[last].unit == CSS_UNIT_AUTO)
			return -1;

		/* Fit the margin box to the container, then express the sizing box. */
		cross = basis - child->margin[first] - child->margin[last];
		if (child->style.box_sizing == CSS_BOX_SIZING_CONTENT)
			cross -= frame;
	}

	/* The used cross size obeys a definite maximum. */
	limit = flex_ratio_definite_length(maximum, basis);
	if (limit >= 0) {
		if (cross > limit)
			cross = limit;
	}

	/* Its definite minimum wins over that maximum. */
	limit = flex_ratio_definite_length(minimum, basis);
	if (limit >= 0) {
		if (cross < limit)
			cross = limit;
	}

	/* Border-box sizing cannot make the content inside the frame negative. */
	if (child->style.box_sizing == CSS_BOX_SIZING_BORDER) {
		if (cross < frame)
			cross = frame;
	}

	/* Content-box sizes likewise stop at zero. */
	if (cross < 0)
		cross = 0;

	/* Succeeded: returns the definite cross size in the ratio's sizing box. */
	return cross;
}

/* Converts the ratio box's cross size to its content size on the main axis. */
static layout_unit
flex_ratio_main_size(
	const struct layout_box *box,
	int row,
	layout_unit cross)
{
	layout_unit frame;
	layout_unit main;

	/* Apply the preferred width-to-height ratio in the main-axis direction. */
	if (row) {
		main = (layout_unit)((float)cross * box->style.aspect_ratio);
	} else {
		main = (layout_unit)((float)cross / box->style.aspect_ratio);
	}

	/* Border-box ratios include the main-axis frame in the converted size. */
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER) {
		if (row) {
			frame = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
		} else {
			frame = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		}

		/* Leave only the content inside the main-axis frame. */
		main -= frame;
	}

	/* A frame larger than its ratio box leaves no content size. */
	if (main < 0)
		main = 0;

	/* Succeeded: returns the main-axis content size. */
	return main;
}

/* Computes a non-replaced ratio item's automatic main-axis minimum. */
static int
flex_ratio_minimum(
	struct layout_tree *tree,
	struct layout_box *container,
	struct flex_item *item,
	int row,
	layout_unit available)
{
	struct layout_box *child;
	const struct css_length *main_size;
	const struct css_length *cross_minimum;
	const struct css_length *cross_maximum;
	struct css_length saved_height;
	struct css_length saved_minimum;
	struct css_length saved_maximum;
	layout_unit least;
	layout_unit cross_basis;
	layout_unit cross_limit_min;
	layout_unit cross_limit_max;
	layout_unit preferred_cross;
	layout_unit preferred_main;
	layout_unit main_basis;
	layout_unit cross;
	layout_unit converted;
	layout_unit given;
	float saved_ratio;
	int overflow;
	int error;

	/* Other items retain the existing intrinsic-size algorithm. */
	child = item->box;
	if (child->replaced)
		return 0;

	/* A box without a preferred ratio has no transferred size suggestion. */
	if (child->style.aspect_ratio <= 0.0f)
		return 0;

	/* Select the physical dimensions used by this flex axis. */
	main_size = &child->style.width;
	cross_minimum = &child->style.min_height;
	cross_maximum = &child->style.max_height;
	overflow = child->style.overflow_x;
	if (row) {
		/* An explicit main-axis minimum replaces the automatic minimum. */
		if (child->style.min_width.unit != CSS_UNIT_AUTO)
			return 0;
	} else {
		/* A column uses height as its main dimension. */
		if (child->style.min_height.unit != CSS_UNIT_AUTO)
			return 0;

		/* Read the column's width constraints as cross-axis constraints. */
		main_size = &child->style.height;
		cross_minimum = &child->style.min_width;
		cross_maximum = &child->style.max_width;
		overflow = child->style.overflow_y;
	}

	/* Scrollable overflow keeps the automatic minimum at zero. */
	if (overflow != CSS_OVERFLOW_VISIBLE) {
		if (overflow != CSS_OVERFLOW_CLIP)
			return 0;
	}

	/* Cross percentages resolve only against an established container size. */
	cross_basis = -1;
	if (tree->measuring == 0) {
		if (row) {
			/* A row needs the container's definite content height. */
			if (tree->containing_height_definite)
				cross_basis = tree->containing_height;
		} else {
			cross_basis = container->width;
		}
	}

	/* Intrinsic measurement does not make a percentage main size definite. */
	main_basis = available;
	if (tree->measuring != 0)
		main_basis = -1;

	/* Resolve author constraints before measurement temporarily changes style. */
	cross_limit_min = flex_ratio_definite_length(cross_minimum, cross_basis);
	cross_limit_max = flex_ratio_definite_length(cross_maximum, cross_basis);
	preferred_cross = flex_ratio_cross_size(tree, container, child, row);
	preferred_main = flex_ratio_definite_length(main_size, main_basis);

	/* Measure actual content without letting the ratio manufacture content. */
	saved_ratio = child->style.aspect_ratio;
	child->style.aspect_ratio = 0.0f;
	if (row) {
		/* Find the narrowest line the contents can occupy. */
		error = flex_min_content(tree, child, &least);
	} else {
		/* Keep the cross width while measuring the contents' natural height. */
		saved_height = child->style.height;
		saved_minimum = child->style.min_height;
		saved_maximum = child->style.max_height;
		child->style.height.unit = CSS_UNIT_AUTO;
		child->style.min_height.unit = CSS_UNIT_AUTO;
		child->style.max_height.unit = CSS_UNIT_NONE;

		/* Lay out the content with no preferred or constrained main size. */
		error = layout_block(tree, child, container->width);
		least = child->height;
		child->style.height = saved_height;
		child->style.min_height = saved_minimum;
		child->style.max_height = saved_maximum;
	}

	/* Restore author style even when the measurement failed. */
	child->style.aspect_ratio = saved_ratio;
	if (error != 0)
		return error;

	/* A definite cross maximum caps the content suggestion through the ratio. */
	if (cross_limit_max >= 0) {
		cross = cross_limit_max;
		converted = flex_ratio_main_size(child, row, cross);
		if (least > converted)
			least = converted;
	}

	/* A definite cross minimum wins over the cross maximum. */
	if (cross_limit_min >= 0) {
		cross = cross_limit_min;
		converted = flex_ratio_main_size(child, row, cross);
		if (least < converted)
			least = converted;
	}

	/* Non-replaced content cannot shrink below a definite cross size's ratio. */
	if (preferred_cross >= 0) {
		cross = preferred_cross;

		/* Respect a definite maximum before applying the stronger minimum. */
		if (cross_limit_max >= 0) {
			given = cross_limit_max;
			if (cross > given)
				cross = given;
		}

		/* An author-specified minimum can exceed the specified cross size. */
		if (cross_limit_min >= 0) {
			given = cross_limit_min;
			if (cross < given)
				cross = given;
		}

		/* Choose the larger content or transferred suggestion. */
		converted = flex_ratio_main_size(child, row, cross);
		if (least < converted)
			least = converted;
	}

	/* A definite preferred main size caps both suggestions. */
	if (preferred_main >= 0) {
		given = preferred_main;

		/* Preferred main sizes include the frame under border-box sizing. */
		if (child->style.box_sizing == CSS_BOX_SIZING_BORDER)
			given -= item->frame;

		/* Honor the author's smaller preferred size. */
		if (least > given)
			least = given;
	}

	/* A definite main maximum also caps the automatic minimum. */
	if (item->maximum >= 0) {
		if (least > item->maximum)
			least = item->maximum;
	}

	/* Publish the floor before hypothetical sizing; basis zero is not a cap. */
	if (least > item->minimum)
		item->minimum = least;

	/* The legacy shrink-only pass must not replace the ratio-aware minimum. */
	item->auto_minimum = 0;

	/* Succeeded: the item has its content-based automatic minimum. */
	return 0;
}

/*
 * Finds the automatic minimum size of the items of a line whose items do
 * not fit (the others keep a minimum of zero, as they do not shrink): the
 * content's min-content width, or the width given in pixels when that is
 * narrower, within max-width.
 */
static int
flex_auto_minimums(
	struct layout_tree *tree,
	struct layout_box *box,
	int row,
	struct flex_item *items,
	size_t count,
	layout_unit available,
	layout_unit gap)
{
	struct layout_box *child;
	const struct css_length *size;
	layout_unit used;
	layout_unit least;
	layout_unit given;
	size_t index;
	int error;

	/* A row line without a definite size, or that fits, does not shrink (a column's minimums always hold). */
	if (row && available < 0)
		return 0;
	used = 0;
	for (index = 0; index < count; index++)
		used += items[index].size + items[index].frame + items[index].margin_start + items[index].margin_end;
	if (count > 1)
		used += gap * (layout_unit)(count - 1U);
	if (row && used <= available)
		return 0;

	/* Each item whose minimum is automatic. */
	for (index = 0; index < count; index++) {
		if (!items[index].auto_minimum)
			continue;
		child = items[index].box;

		/* The content's narrowest width, or a column item's content height at the container's width. */
		if (row)
			error = flex_min_content(tree, child, &least);
		else
			error = flex_content_height(tree, box, child, &least);
		if (error != 0)
			return error;

		/* A size in pixels that is smaller is the minimum instead. */
		size = row ? &child->style.width : &child->style.height;
		if (size->unit == CSS_UNIT_PX || (!row && size->unit == CSS_UNIT_PERCENT && available >= 0)) {
			given = layout_from_px(size->value);
			if (size->unit == CSS_UNIT_PERCENT)
				given = (layout_unit)((float)available * size->value / 100.0f) + layout_from_px(size->offset);
			if (child->style.box_sizing == CSS_BOX_SIZING_BORDER)
				given -= items[index].frame;
			if (given < least)
				least = given;
		}

		/* Within the maximum, and in a row never above the base the item would shrink from. */
		if (items[index].maximum >= 0 && least > items[index].maximum)
			least = items[index].maximum;
		if (row && least > items[index].base)
			least = items[index].base;
		if (least > items[index].minimum)
			items[index].minimum = least;

		/* The hypothetical size holds the minimum. */
		if (items[index].size < items[index].minimum)
			items[index].size = items[index].minimum;
	}

	/* Succeeded: the minimums are known. */
	return 0;
}

/*
 * Measures a column item's content height: the item laid out at the
 * container's width with its height, minimum and maximum auto (its
 * content-based minimum height), and its style put back.
 */
static int
flex_content_height(
	struct layout_tree *tree,
	struct layout_box *box,
	struct layout_box *child,
	layout_unit *height)
{
	struct css_length saved_height;
	struct css_length saved_minimum;
	struct css_length saved_maximum;
	int error;

	/* The height left to the content. */
	saved_height = child->style.height;
	saved_minimum = child->style.min_height;
	saved_maximum = child->style.max_height;
	child->style.height.unit = CSS_UNIT_AUTO;
	child->style.min_height.unit = CSS_UNIT_AUTO;
	child->style.max_height.unit = CSS_UNIT_NONE;
	error = layout_block(tree, child, box->width);
	*height = child->height;
	child->style.height = saved_height;
	child->style.min_height = saved_minimum;
	child->style.max_height = saved_maximum;
	return error;
}

/*
 * Measures a box's min-content width: its content laid out with a line at
 * every opportunity (as the intrinsic min-content width is measured).
 */
static int
flex_min_content(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit *width)
{
	struct layout_context context;
	struct wb_vector floats;
	struct css_length given;
	int error;

	/* A replaced box's content is its image, as wide as it is. */
	*width = 0;
	if (box->replaced) {
		*width = box->width;
		return 0;
	}

	/* The content at no width, with an auto width, while the tree measures. */
	given = box->style.width;
	box->style.width.unit = CSS_UNIT_AUTO;
	tree->measuring++;
	layout_context_begin(tree, &context, &floats);
	error = layout_block(tree, box, 0);
	layout_context_end(tree, &context);
	tree->measuring--;
	box->style.width = given;
	if (error != 0)
		return error;

	/* Succeeded: the widest thing that could not be broken. */
	*width = layout_content_width(box, 0);
	return 0;
}

/*
 * Shares a line's free space among its items: growing by flex-grow when
 * there is room, shrinking by flex-shrink weighted by the basis when there
 * is too little (redistributing after an item reaches its limit), then giving what is left
 * to auto margins.  Without a definite main size nothing flexes.
 */
static void
flex_resolve(
	struct flex_item *items,
	size_t count,
	layout_unit available,
	layout_unit gap)
{
	layout_unit used;
	layout_unit free_space;
	layout_unit share;
	size_t index;
	int autos;

	/* The space the items take as they are. */
	used = 0;
	for (index = 0; index < count; index++)
		used += items[index].size + items[index].frame + items[index].margin_start + items[index].margin_end;
	if (count > 1)
		used += gap * (layout_unit)(count - 1U);
	if (available < 0)
		return;
	free_space = available - used;

	/* A clamped item freezes and the rest share its leftover space. */
	if (free_space > 0)
		flex_grow(items, count, available, gap);

	/* Too little room: the items shrink in proportion to their factors times their bases, those held at their minimums frozen. */
	if (free_space < 0)
		flex_shrink(items, count, available, gap);

	/* What is still free goes to the auto margins. */
	used = 0;
	autos = 0;
	for (index = 0; index < count; index++) {
		used += items[index].size + items[index].frame + items[index].margin_start + items[index].margin_end;
		autos += items[index].auto_start + items[index].auto_end;
	}

	/* The gaps between the items take room too. */
	if (count > 1)
		used += gap * (layout_unit)(count - 1U);
	free_space = available - used;
	if (free_space <= 0 || autos == 0)
		return;

	/* Each auto margin takes an equal part. */
	share = free_space / autos;
	for (index = 0; index < count; index++) {
		if (items[index].auto_start)
			items[index].margin_start += share;
		if (items[index].auto_end)
			items[index].margin_end += share;
	}
}

/*
 * Lays an item out as a block at its final main size, with its style's
 * sizes and margins as they were afterwards: the next layout of the item
 * (the real one after a measurement, or the next line's) starts from its
 * own style again, not from the size this one gave it.
 */
static int
flex_lay_item(
	struct layout_tree *tree,
	struct layout_box *box,
	struct flex_item *item,
	int row)
{
	struct flex_sizes saved;
	struct layout_box *child;
	int error;

	/* Keeps the style's sizes and margins, which the layout at the flexed size overrides. */
	child = item->box;
	saved.box_sizing = child->style.box_sizing;
	saved.width = child->style.width;
	saved.min_width = child->style.min_width;
	saved.max_width = child->style.max_width;
	saved.height = child->style.height;
	saved.min_height = child->style.min_height;
	saved.max_height = child->style.max_height;
	memcpy(saved.margin, child->style.margin, sizeof(saved.margin));

	/* The layout at the flexed size. */
	error = flex_lay_sized(tree, box, item, row);

	/* The style is the item's own again, whatever the layout reported. */
	child->style.box_sizing = saved.box_sizing;
	child->style.width = saved.width;
	child->style.min_width = saved.min_width;
	child->style.max_width = saved.max_width;
	child->style.height = saved.height;
	child->style.min_height = saved.min_height;
	child->style.max_height = saved.max_height;
	memcpy(child->style.margin, saved.margin, sizeof(saved.margin));

	/* Reports an item that could not be laid out. */
	if (error != 0)
		return error;

	/* Succeeded: the item is laid out at its size. */
	return 0;
}

/*
 * Lays an item out as a block at its final main size: its width (a row's
 * item) or height (a column's) set to that size, its main-axis margins to
 * the shared ones; a column's item that does not stretch shrinks to fit.
 */
static int
flex_lay_sized(
	struct layout_tree *tree,
	struct layout_box *box,
	struct flex_item *item,
	int row)
{
	struct layout_box *child;
	layout_unit size;
	int align;
	int error;

	/*
	 * The flexed size is the content box's and its limits are applied
	 * already.  Keep the item's box sizing so that its untouched cross
	 * size retains the author's meaning; express the main size as a border
	 * box when that is what layout_block will consume.
	 */
	child = item->box;
	size = item->size;
	if (child->style.box_sizing == CSS_BOX_SIZING_BORDER)
		size += item->frame;

	/* A row's item: its width and its horizontal margins. */
	if (row) {
		child->style.width.unit = CSS_UNIT_PX;
		child->style.width.value = layout_to_px(size);
		child->style.width.offset = 0;
		child->style.min_width.unit = CSS_UNIT_PX;
		child->style.min_width.value = 0;
		child->style.max_width.unit = CSS_UNIT_NONE;
		child->style.margin[CSS_LEFT].unit = CSS_UNIT_PX;
		child->style.margin[CSS_LEFT].value = layout_to_px(item->margin_start);
		child->style.margin[CSS_LEFT].offset = 0;
		child->style.margin[CSS_RIGHT].unit = CSS_UNIT_PX;
		child->style.margin[CSS_RIGHT].value = layout_to_px(item->margin_end);
		child->style.margin[CSS_RIGHT].offset = 0;
		error = layout_block(tree, child, box->width);
		return error;
	}

	/* A column's item: its height, its vertical margins, and its width by its alignment. */
	child->style.height.unit = CSS_UNIT_PX;
	child->style.height.value = layout_to_px(size);
	child->style.min_height.unit = CSS_UNIT_PX;
	child->style.min_height.value = 0;
	child->style.max_height.unit = CSS_UNIT_NONE;
	child->style.margin[CSS_TOP].unit = CSS_UNIT_PX;
	child->style.margin[CSS_TOP].value = layout_to_px(item->margin_start);
	child->style.margin[CSS_TOP].offset = 0;
	child->style.margin[CSS_BOTTOM].unit = CSS_UNIT_PX;
	child->style.margin[CSS_BOTTOM].value = layout_to_px(item->margin_end);
	child->style.margin[CSS_BOTTOM].offset = 0;
	align = flex_align_of(box, child);
	if ((align == CSS_ALIGN_STRETCH && box->style.flex_wrap == CSS_FLEX_NOWRAP) ||
	    child->style.width.unit != CSS_UNIT_AUTO) {
		error = layout_block(tree, child, box->width);
		return error;
	}

	/* An item that does not stretch is as wide as its content. */
	error = layout_shrink_to_fit(tree, child, box->width);
	if (error != 0)
		return error;

	/* Succeeded: the item is laid out. */
	return 0;
}

/* Stretches a wrapping column's auto-width items to their line's width. */
static int
flex_relay_column(
	struct layout_tree *tree,
	struct layout_box *box,
	struct flex_item *items,
	size_t count,
	layout_unit line_cross)
{
	struct flex_sizes saved;
	struct layout_box *child;
	layout_unit frame;
	layout_unit width;
	size_t index;
	int error;

	for (index = 0; index < count; index++) {
		child = items[index].box;
		if (flex_align_of(box, child) != CSS_ALIGN_STRETCH || child->style.width.unit != CSS_UNIT_AUTO)
			continue;
		frame = child->border[CSS_LEFT] + child->padding[CSS_LEFT] + child->padding[CSS_RIGHT] + child->border[CSS_RIGHT];
		width = line_cross - child->margin[CSS_LEFT] - child->margin[CSS_RIGHT] - frame;
		if (width < 0)
			width = 0;

		/* The flexed height stays; the width is the line's. */
		saved.box_sizing = child->style.box_sizing;
		saved.width = child->style.width;
		saved.min_width = child->style.min_width;
		saved.max_width = child->style.max_width;
		saved.height = child->style.height;
		saved.min_height = child->style.min_height;
		saved.max_height = child->style.max_height;
		memcpy(saved.margin, child->style.margin, sizeof(saved.margin));
		child->style.box_sizing = CSS_BOX_SIZING_CONTENT;
		child->style.width.unit = CSS_UNIT_PX;
		child->style.width.value = layout_to_px(width);
		child->style.width.offset = 0;
		child->style.min_width.unit = CSS_UNIT_PX;
		child->style.min_width.value = 0;
		child->style.max_width.unit = CSS_UNIT_NONE;
		child->style.height.unit = CSS_UNIT_PX;
		child->style.height.value = layout_to_px(child->height);
		child->style.height.offset = 0;
		child->style.min_height.unit = CSS_UNIT_PX;
		child->style.min_height.value = 0;
		child->style.max_height.unit = CSS_UNIT_NONE;
		child->style.margin[CSS_TOP].unit = CSS_UNIT_PX;
		child->style.margin[CSS_TOP].value = layout_to_px(items[index].margin_start);
		child->style.margin[CSS_TOP].offset = 0;
		child->style.margin[CSS_BOTTOM].unit = CSS_UNIT_PX;
		child->style.margin[CSS_BOTTOM].value = layout_to_px(items[index].margin_end);
		child->style.margin[CSS_BOTTOM].offset = 0;
		error = layout_block(tree, child, box->width);
		child->style.box_sizing = saved.box_sizing;
		child->style.width = saved.width;
		child->style.min_width = saved.min_width;
		child->style.max_width = saved.max_width;
		child->style.height = saved.height;
		child->style.min_height = saved.min_height;
		child->style.max_height = saved.max_height;
		memcpy(child->style.margin, saved.margin, sizeof(saved.margin));
		if (error != 0)
			return error;
	}
	return 0;
}

/*
 * Lays row items out again after the line's used cross size is known.  A
 * stretched auto-height item used to have only its box height changed at
 * placement time, after its descendants had been laid out.  Giving the
 * item a temporary definite height here lets percentage-height descendants
 * use the stretched size.  A percentage item gets the same treatment when
 * the flex container has a definite height.
 */
static int
flex_relay_cross(
	struct layout_tree *tree,
	struct layout_box *box,
	struct flex_item *items,
	size_t count,
	int row,
	layout_unit line_cross,
	layout_unit percentage_basis)
{
	struct flex_sizes saved;
	struct layout_box *child;
	layout_unit frame;
	layout_unit height;
	layout_unit ratio_cross;
	size_t index;
	int align;
	int error;

	/* A single-line column's cross axis is width, which the normal width layout already resolves. */
	if (!row && box->style.flex_wrap == CSS_FLEX_NOWRAP)
		return 0;
	if (!row)
		return flex_relay_column(tree, box, items, count, line_cross);

	/* Each item that needs the definite cross size is laid out again. */
	for (index = 0; index < count; index++) {
		child = items[index].box;
		align = flex_align_of(box, child);
		frame = child->border[CSS_TOP] + child->padding[CSS_TOP] +
		    child->padding[CSS_BOTTOM] + child->border[CSS_BOTTOM];

		/* A definite percentage, or an automatic cross size stretched to the line. */
		if (child->style.height.unit == CSS_UNIT_PERCENT && percentage_basis >= 0) {
			height = flex_length(&child->style.height, percentage_basis);
			if (child->style.box_sizing == CSS_BOX_SIZING_BORDER)
				height -= frame;
		} else if (child->style.height.unit == CSS_UNIT_AUTO && align == CSS_ALIGN_STRETCH) {
			/* Ratio items with auto cross margins keep their intrinsic height. */
			if (!child->replaced) {
				if (child->style.aspect_ratio > 0.0f) {
					/* An auto margin on either side prevents stretch. */
					if (child->style.margin[CSS_TOP].unit == CSS_UNIT_AUTO)
						continue;

					/* The bottom margin has the same ownership of free space. */
					if (child->style.margin[CSS_BOTTOM].unit == CSS_UNIT_AUTO)
						continue;
				}
			}

			/* Ordinary line stretch remains available when cross sizing is indefinite. */
			height = line_cross - child->margin[CSS_TOP] - child->margin[CSS_BOTTOM] - frame;

			/* Preserve the ratio calculation's constrained definite cross size. */
			if (!child->replaced) {
				if (child->style.aspect_ratio > 0.0f) {
					/* The shared helper applies cross min/max and box sizing. */
					ratio_cross = flex_ratio_cross_size(tree, box, child, row);
					if (ratio_cross >= 0) {
						height = ratio_cross;

						/* layout_block receives a content-box height below. */
						if (child->style.box_sizing == CSS_BOX_SIZING_BORDER)
							height -= frame;
					}
				}
			}
		} else {
			continue;
		}

		/* No cross size is negative. */
		if (height < 0)
			height = 0;

		/* Keep the flexed content width while giving layout_block a definite content height. */
		saved.box_sizing = child->style.box_sizing;
		saved.width = child->style.width;
		saved.min_width = child->style.min_width;
		saved.max_width = child->style.max_width;
		saved.height = child->style.height;
		saved.min_height = child->style.min_height;
		saved.max_height = child->style.max_height;
		memcpy(saved.margin, child->style.margin, sizeof(saved.margin));
		child->style.box_sizing = CSS_BOX_SIZING_CONTENT;
		child->style.width.unit = CSS_UNIT_PX;
		child->style.width.value = layout_to_px(child->width);
		child->style.width.offset = 0;
		child->style.min_width.unit = CSS_UNIT_PX;
		child->style.min_width.value = 0;
		child->style.max_width.unit = CSS_UNIT_NONE;
		child->style.margin[CSS_LEFT].unit = CSS_UNIT_PX;
		child->style.margin[CSS_LEFT].value = layout_to_px(items[index].margin_start);
		child->style.margin[CSS_LEFT].offset = 0;
		child->style.margin[CSS_RIGHT].unit = CSS_UNIT_PX;
		child->style.margin[CSS_RIGHT].value = layout_to_px(items[index].margin_end);
		child->style.margin[CSS_RIGHT].offset = 0;
		child->style.height.unit = CSS_UNIT_PX;
		child->style.height.value = layout_to_px(height);
		child->style.height.offset = 0;
		child->style.min_height.unit = CSS_UNIT_PX;
		child->style.min_height.value = 0;
		child->style.max_height.unit = CSS_UNIT_NONE;
		error = layout_block(tree, child, box->width);
		child->style.box_sizing = saved.box_sizing;
		child->style.width = saved.width;
		child->style.min_width = saved.min_width;
		child->style.max_width = saved.max_width;
		child->style.height = saved.height;
		child->style.min_height = saved.min_height;
		child->style.max_height = saved.max_height;
		memcpy(child->style.margin, saved.margin, sizeof(saved.margin));
		if (error != 0)
			return error;
	}

	/* Succeeded: every applicable item and its descendants use the cross size. */
	return 0;
}

/* A positive distributed gap uses the same nearest layout unit as a CSS calc() length. */
static layout_unit
flex_divide_nearest(
	layout_unit numerator,
	layout_unit denominator)
{
	return numerator / denominator +
	    (numerator % denominator >= denominator / 2 + denominator % 2);
}

/*
 * Places a line's laid out items: along the main axis by justify-content
 * (and reversed for a reverse direction), across the line by their
 * alignment, a stretching item made as thick as the line.
 */
static void
flex_place_line(
	struct layout_box *box,
	struct flex_item *items,
	size_t count,
	int row,
	layout_unit available,
	layout_unit gap,
	layout_unit cross_start,
	layout_unit line_cross)
{
	struct layout_box *child;
	layout_unit used;
	layout_unit free_space;
	layout_unit cursor;
	layout_unit between;
	layout_unit main_outer;
	layout_unit cross_outer;
	layout_unit offset;
	layout_unit frame;
	layout_unit sizing;
	layout_unit reverse_extent;
	size_t index;
	int reverse;
	layout_unit above;
	layout_unit max_above;
	layout_unit max_below;
	int group;
	int align;
	int auto_first;
	int auto_last;

	/*
	 * The line's shared baselines: the first group's as far below the line's
	 * top as any member's lies below its margin box's top, the last group's as
	 * far above the line's bottom as any member's lies above its margin box's bottom.
	 */
	max_above = 0;
	max_below = 0;
	for (index = 0; index < count; index++) {
		group = flex_baseline_above(box, items[index].box, row, &above);
		if (group == 1 && above > max_above)
			max_above = above;
		if (group == 2 && flex_cross_outer(items[index].box, row) - above > max_below)
			max_below = flex_cross_outer(items[index].box, row) - above;
	}

	/* Keep overflow signed for end and center; only an indefinite size has no remaining space. */
	used = 0;
	for (index = 0; index < count; index++)
		used += items[index].size + items[index].frame + items[index].margin_start + items[index].margin_end;
	if (count > 1)
		used += gap * (layout_unit)(count - 1U);
	free_space = 0;
	if (available >= 0)
		free_space = available - used;
	reverse_extent = available;
	if (reverse_extent < 0)
		reverse_extent = used;

	/* justify-content: where the first item starts and what goes between them. */
	cursor = 0;
	between = gap;
	switch (box->style.justify_content) {
	case CSS_ALIGN_END:
		cursor = free_space;
		break;
	case CSS_ALIGN_CENTER:
		cursor = free_space / 2;
		break;
	case CSS_ALIGN_SPACE_BETWEEN:
		if (free_space > 0 && count > 1)
			between = gap + flex_divide_nearest(free_space, (layout_unit)(count - 1U));
		break;
	case CSS_ALIGN_SPACE_AROUND:
		if (free_space > 0) {
			cursor = flex_divide_nearest(free_space, (layout_unit)(count * 2U));
			between = gap + flex_divide_nearest(free_space, (layout_unit)count);
		}
		break;
	case CSS_ALIGN_SPACE_EVENLY:
		if (free_space > 0) {
			cursor = flex_divide_nearest(free_space, (layout_unit)(count + 1U));
			between = gap + flex_divide_nearest(free_space, (layout_unit)(count + 1U));
		}
		break;
	default:
		break;
	}

	/* A reverse direction runs from the far end; so does a row of a right-to-left container (ws074-p073), the other way. */
	reverse = 0;
	if (box->style.flex_direction == CSS_FLEX_ROW_REVERSE || box->style.flex_direction == CSS_FLEX_COLUMN_REVERSE)
		reverse = 1;
	if ((box->style.flex_direction == CSS_FLEX_ROW ||
	    box->style.flex_direction == CSS_FLEX_ROW_REVERSE) &&
	    box->style.direction == CSS_DIRECTION_RTL)
		reverse = !reverse;
	/* Give reverse lines the residual end gap after rounded distributed gaps. */
	if (reverse && free_space > 0 && count > 1 &&
	    (box->style.justify_content == CSS_ALIGN_SPACE_BETWEEN ||
	    box->style.justify_content == CSS_ALIGN_SPACE_AROUND ||
	    box->style.justify_content == CSS_ALIGN_SPACE_EVENLY))
		cursor = free_space - cursor - (layout_unit)(count - 1U) * (between - gap);

	/* Column axes fall back to start; row axes use physical or line left/right. */
	if ((box->style.flex_direction == CSS_FLEX_COLUMN ||
	    box->style.flex_direction == CSS_FLEX_COLUMN_REVERSE) &&
	    (box->style.justify_content == CSS_ALIGN_LEFT ||
	    box->style.justify_content == CSS_ALIGN_RIGHT)) {
		cursor = 0;
		if (reverse)
			cursor = free_space;
	} else if (box->style.justify_content == CSS_ALIGN_LEFT) {
		cursor = 0;
		if (reverse)
			cursor = free_space;
	} else if (box->style.justify_content == CSS_ALIGN_RIGHT) {
		cursor = free_space;
		if (reverse)
			cursor = 0;
	}

	/* Each item along the main axis, then across the line. */
	for (index = 0; index < count; index++) {
		child = items[index].box;
		main_outer = items[index].size + items[index].frame + items[index].margin_start + items[index].margin_end;
		offset = cursor + items[index].margin_start;
		if (reverse)
			offset = reverse_extent - cursor - main_outer + items[index].margin_start;

		/* Across: stretched, at the start, the end or the center of the line. */
		align = flex_align_of(box, child);
		cross_outer = flex_cross_outer(child, row);
		if (row && align == CSS_ALIGN_STRETCH && child->style.height.unit == CSS_UNIT_AUTO) {
			/* Ratio items already have their constrained size from cross relayout. */
			if (child->style.aspect_ratio <= 0.0f || child->replaced) {
				frame = child->border[CSS_TOP] + child->padding[CSS_TOP] + child->padding[CSS_BOTTOM] + child->border[CSS_BOTTOM];
				child->height = line_cross - child->margin[CSS_TOP] - child->margin[CSS_BOTTOM] - frame;

				/* The stretched height stays within the item's pixel maximum and minimum. */
				sizing = child->style.box_sizing == CSS_BOX_SIZING_BORDER ? frame : 0;
				if (child->style.max_height.unit == CSS_UNIT_PX &&
				    child->height > layout_from_px(child->style.max_height.value) - sizing)
					child->height = layout_from_px(child->style.max_height.value) - sizing;
				if (child->style.min_height.unit == CSS_UNIT_PX &&
				    child->height < layout_from_px(child->style.min_height.value) - sizing)
					child->height = layout_from_px(child->style.min_height.value) - sizing;

				/* Content inside a larger frame cannot have a negative height. */
				if (child->height < 0)
					child->height = 0;
			}

			/* Placement sees the actual cross size, including any cross constraints. */
			cross_outer = flex_cross_outer(child, row);
		} else if (!row && align == CSS_ALIGN_STRETCH && child->replaced && child->style.width.unit == CSS_UNIT_AUTO &&
		    child->style.margin[CSS_LEFT].unit != CSS_UNIT_AUTO && child->style.margin[CSS_RIGHT].unit != CSS_UNIT_AUTO) {
			/* A replaced item of a column stretches across too, whatever its ratio, within its pixel limits. */
			frame = child->border[CSS_LEFT] + child->padding[CSS_LEFT] + child->padding[CSS_RIGHT] + child->border[CSS_RIGHT];
			child->width = line_cross - child->margin[CSS_LEFT] - child->margin[CSS_RIGHT] - frame;
			sizing = child->style.box_sizing == CSS_BOX_SIZING_BORDER ? frame : 0;
			if (child->style.max_width.unit == CSS_UNIT_PX &&
			    child->width > layout_from_px(child->style.max_width.value) - sizing)
				child->width = layout_from_px(child->style.max_width.value) - sizing;
			if (child->style.min_width.unit == CSS_UNIT_PX &&
			    child->width < layout_from_px(child->style.min_width.value) - sizing)
				child->width = layout_from_px(child->style.min_width.value) - sizing;
			if (child->width < 0)
				child->width = 0;
			cross_outer = flex_cross_outer(child, row);
		}

		/* The item's place along the cross axis within the line. */
		if (row) {
			child->x = offset;
			child->y = cross_start + child->margin[CSS_TOP];
		} else {
			child->y = offset;
			child->x = cross_start + child->margin[CSS_LEFT];
		}

		/* Auto margins across take the room the line leaves (shared when both are auto), before any alignment. */
		auto_first = child->style.margin[row ? CSS_TOP : CSS_LEFT].unit == CSS_UNIT_AUTO;
		auto_last = child->style.margin[row ? CSS_BOTTOM : CSS_RIGHT].unit == CSS_UNIT_AUTO;
		if (auto_first || auto_last) {
			frame = line_cross - cross_outer;
			if (frame < 0)
				frame = 0;
			if (!auto_first)
				frame = 0;
			else if (auto_last)
				frame = frame / 2;
			if (row)
				child->y += frame;
			else
				child->x += frame;
		} else if ((group = flex_baseline_above(box, child, row, &above)) != 0) {
			/* A baseline-aligned item moves down until its baseline is its group's (before wrap-reverse mirrors the line). */
			if (group == 1)
				frame = max_above - above;
			else
				frame = line_cross - max_below - above;
			if (box->style.flex_wrap == CSS_FLEX_WRAP_REVERSE)
				frame = line_cross - frame - cross_outer;
			child->y += frame;
		} else if (align == CSS_ALIGN_END || align == CSS_ALIGN_CENTER) {
			/* At the end or the center, the item moves by the room the line leaves (half of it). */
			frame = line_cross - cross_outer;
			if (align == CSS_ALIGN_CENTER)
				frame = frame / 2;
			if (row)
				child->y += frame;
			else
				child->x += frame;
		}

		/* The next item starts past this one and what goes between. */
		cursor += main_outer + between;
	}
}

/* Measures an item's margin box across the main axis (its height in a row, its width in a column). */
static layout_unit
flex_cross_outer(
	const struct layout_box *item,
	int row)
{
	/* A row's items are measured down, a column's across. */
	if (row) {
		return item->margin[CSS_TOP] + item->border[CSS_TOP] + item->padding[CSS_TOP] + item->height +
		    item->padding[CSS_BOTTOM] + item->border[CSS_BOTTOM] + item->margin[CSS_BOTTOM];
	}

	/* A column's item across. */
	return item->margin[CSS_LEFT] + item->border[CSS_LEFT] + item->padding[CSS_LEFT] + item->width +
	    item->padding[CSS_RIGHT] + item->border[CSS_RIGHT] + item->margin[CSS_RIGHT];
}

/* Picks an item's alignment across the line: its align-self, or the container's align-items (baseline as start). */
static int
flex_align_of(
	const struct layout_box *box,
	const struct layout_box *item)
{
	int align;

	/* auto defers to the container. */
	align = item->style.align_self;
	if (align == CSS_ALIGN_AUTO)
		align = box->style.align_items;
	if (align == CSS_ALIGN_SELF_START)
		align = CSS_ALIGN_START;
	if (align == CSS_ALIGN_SELF_END)
		align = CSS_ALIGN_END;
	if (align == CSS_ALIGN_NORMAL)
		align = CSS_ALIGN_STRETCH;

	/* Baselines are not measured in this pass. */
	if (align == CSS_ALIGN_BASELINE)
		align = CSS_ALIGN_START;

	/* Reports the alignment. */
	return align;
}

/*
 * Tells whether an item of a row takes part in its line's first (1) or
 * last (2) baseline alignment (align-self baseline or last baseline,
 * without an auto margin across), and how far below its margin box's top
 * that baseline is (the bottom of its border box when it has none).
 */
static int
flex_baseline_above(
	const struct layout_box *box,
	const struct layout_box *item,
	int row,
	layout_unit *above)
{
	layout_unit baseline;
	int align;
	int last;
	int found;

	/* Only a row's items on a horizontal line (not one laid out logically for a vertical mode), aligned by their baselines. */
	if (!row || box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB ||
	    layout_physical_mode(box) != CSS_WRITING_HORIZONTAL_TB ||
	    item->style.writing_mode != CSS_WRITING_HORIZONTAL_TB)
		return 0;
	align = item->style.align_self;
	last = (item->style.last_baselines & 8u) != 0;
	if (align == CSS_ALIGN_AUTO) {
		align = box->style.align_items;
		last = (box->style.last_baselines & 4u) != 0;
	}
	if (align != CSS_ALIGN_BASELINE && !last)
		return 0;
	if (item->style.margin[CSS_TOP].unit == CSS_UNIT_AUTO || item->style.margin[CSS_BOTTOM].unit == CSS_UNIT_AUTO)
		return 0;

	/* Its first (or last) baseline, or its border box's bottom. */
	found = last ? layout_last_baseline(item, &baseline) : layout_first_baseline(item, &baseline);
	if (!found)
		baseline = item->border[CSS_TOP] + item->padding[CSS_TOP] + item->height + item->padding[CSS_BOTTOM] +
		    item->border[CSS_BOTTOM];
	*above = item->margin[CSS_TOP] + baseline;
	return last ? 2 : 1;
}

/* Resolves a length (pixels, or a percentage with its added pixels) against a basis; anything else is zero. */
static layout_unit
flex_length(
	const struct css_length *length,
	layout_unit basis)
{
	layout_unit value;

	/* Pixels, or a percentage of the basis. */
	value = 0;
	if (length->unit == CSS_UNIT_PX)
		value = layout_from_px(length->value);
	if (length->unit == CSS_UNIT_PERCENT)
		value = (layout_unit)((float)basis * length->value / 100.0f) + layout_from_px(length->offset);

	/* Reports the length. */
	return value;
}
