/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Grid layout, the common part of CSS Grid Layout 1: the
 * explicit column and row templates (lengths, percentages, fr, auto,
 * minmax()'s minimum), items placed by line numbers (negative from the
 * end) and spans or else row after row in the first cells that are free
 * (with row dense backfilling earlier holes),
 * implicit auto tracks for what goes past the templates, row/column sparse
 * and dense auto-placement, the gaps, and
 * each item laid out as a block in its area and aligned in it
 * (align-self, align-items).  Columns are sized in three steps: the fixed
 * ones, then the auto ones at their items' content width, then the fr
 * ones sharing what is left (auto columns share it when there is no fr);
 * rows are as tall as their items unless the template fixes them.  Direct
 * absolute children with definite lines use their area's static origin in
 * horizontal LTR/RTL grids, without taking part in track sizing.  Not in
 * this pass: named lines and areas, full inline-axis
 * alignment, order, full baseline alignment, subgrid, and auto-fill and auto-fit.
 */

#include "layout/layout.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* The most implicit rows a grid places items in (items past them go on the last). */
#define GRID_ROWS_MAX	4096

/*
 * One item on its way through the algorithm: its box, and its area's
 * first column and row and how many of each it spans.
 */
struct grid_item {
	struct layout_box *box;
	int column;
	int column_span;
	int row;
	int row_span;
};

/*
 * The grid being laid out: its items, its columns' and rows' sizes and
 * offsets, and which cells are taken (rows of column_count cells).
 */
struct grid_state {
	struct grid_item *items;
	size_t item_count;
	int column_count;
	int row_count;
	layout_unit *columns;
	layout_unit *rows;
	unsigned char *taken;
	int taken_rows;
	/* Tracks of an auto-fit repeat that no item uses: they take no room (NULL when none). */
	unsigned char *column_collapsed;
	unsigned char *row_collapsed;
	/* What a fit-content() column may grow to with free space (0 for other columns; NULL when none). */
	layout_unit *column_fit;
};

/*
 * The part of an item's style that its layout in its area overrides, kept
 * to be put back afterwards.
 */
struct grid_sizes {
	int box_sizing;
	struct css_length width;
	struct css_length min_width;
	struct css_length max_width;
	struct css_length height;
	struct css_length min_height;
	struct css_length max_height;
	struct css_length margin[4];
};

static int grid_span_has_fixed_rows(const struct css_style *style, const struct grid_item *item, const struct grid_state *grid, layout_unit gap, layout_unit available);
static int grid_fixed_span_minimum(const struct layout_box *box, int64_t *minimum);
static int grid_column_auto_minimum(struct layout_box *box, layout_unit containing_width, layout_unit *minimum);
static int grid_track_floor(const struct css_track *track, layout_unit whole, layout_unit *floor);
static int grid_row_stretches(const struct css_track *track);
static int grid_fixed_area_height(const struct layout_box *box, const struct grid_item *item, layout_unit gap, layout_unit *height);
static void grid_maximize(const struct css_style *style, int columns, layout_unit *sizes, int count, const unsigned char *collapsed, layout_unit whole, layout_unit free_space);
static int grid_save_used_tracks(struct layout_tree *tree, struct layout_box *box, const struct grid_state *grid);
static int grid_copy_used_axis(struct layout_tree *tree, const layout_unit *sizes, int count, double **copy);
static int grid_collect(struct layout_box *box, struct grid_state *grid);
static int grid_vertical_fixed(struct layout_tree *tree, struct layout_box *box, int *handled);
static int grid_vertical_auto(struct layout_tree *tree, struct layout_box *box, int *handled);
static void grid_resolve(const struct css_grid_place *place, int explicit_count, int *start, int *span);
static void grid_static_position(struct layout_box *box, const struct grid_state *grid,
    layout_unit column_gap, layout_unit row_gap, layout_unit shift, layout_unit row_shift);
static void grid_static_vertical_start(struct layout_box *box);
static void grid_abspos_areas(struct layout_box *box, const struct grid_state *grid,
    layout_unit column_gap, layout_unit row_gap, layout_unit shift, layout_unit row_shift);
static int grid_place(struct grid_state *grid, int auto_flow);
static int grid_place_row(struct grid_state *grid, int dense);
static int grid_fits(const struct grid_state *grid, int row, int column, int row_span, int column_span);
static int grid_take(struct grid_state *grid, int row, int column, int row_span, int column_span);
static const struct css_track *grid_column_track(const struct css_style *style, int column);
static const struct css_track *grid_row_track(const struct css_style *style, int row);
static int grid_size_columns(struct layout_tree *tree, struct layout_box *box, struct grid_state *grid, layout_unit gap);
static int grid_item_width(struct layout_tree *tree, struct layout_box *item, layout_unit *width);
static int grid_item_minimum_width(struct layout_tree *tree, struct layout_box *item, layout_unit *width);
static int grid_column_stretches(const struct layout_box *box, const struct css_track *track);
static int grid_lay_item(struct layout_tree *tree, struct layout_box *item, layout_unit width, int justify);
static void grid_size_rows(struct layout_box *box, struct grid_state *grid, layout_unit gap);
static int grid_relay_item(struct layout_tree *tree, struct layout_box *item, layout_unit width, layout_unit height, int align);
static layout_unit grid_clamp_height(const struct layout_box *item, layout_unit content, layout_unit area, layout_unit frame);
static layout_unit grid_clamp_height(const struct layout_box *item, layout_unit content, layout_unit area, layout_unit frame);
static layout_unit grid_outer_height(const struct layout_box *item);
static int grid_row_baseline(const struct layout_box *box, const struct grid_item *item, layout_unit *above);
static void grid_row_baselines(const struct layout_box *box, const struct grid_state *grid, int row, layout_unit *first_above, layout_unit *first_below, layout_unit *last_above, layout_unit *last_below);
static int grid_fixed_column_baseline(const struct layout_box *box, const struct grid_item *item, layout_unit *right_extent);
static layout_unit grid_track_length(const struct css_length *length, layout_unit whole, int measuring);
static void grid_release(struct grid_state *grid);
static void grid_expand_repeats(struct layout_tree *tree, struct layout_box *box);
static int grid_collapse_tracks(struct layout_box *box, struct grid_state *grid);
static void grid_distribute(int value, int reversed, layout_unit free_space, int tracks, layout_unit *offset, layout_unit *extra);
static int grid_live_tracks(const unsigned char *collapsed, int count);
static layout_unit grid_fixed_row_gap(const struct layout_box *box);
static int grid_ratio_width(const struct layout_box *box, const struct grid_item *item, layout_unit gap, layout_unit *width);
static int grid_content_stretches(int value);

/* The size one track of a repeat is counted at: its definite maximum, else its minimum. */
static layout_unit
grid_repeat_track(
	const struct css_track *track,
	layout_unit basis)
{
	const struct css_length *length;

	length = &track->size;
	if (track->kind != CSS_TRACK_LENGTH)
		length = &track->minimum;
	if (length->unit == CSS_UNIT_PX)
		return layout_from_px(length->value);
	if (length->unit == CSS_UNIT_PERCENT && basis >= 0)
		return (layout_unit)((float)basis * length->value / 100.0f) + layout_from_px(length->offset);
	return 0;
}

/*
 * Puts an auto-fill or auto-fit repeat's tracks in a template as many
 * times as fill the available size (once when it is indefinite).
 */
static void
grid_expand_axis(
	struct css_track *tracks,
	int *count,
	struct css_track_repeat *repeat,
	layout_unit available,
	layout_unit gap)
{
	layout_unit others;
	layout_unit pattern;
	layout_unit denominator;
	layout_unit numerator;
	int others_count;
	int times;
	int index;
	int tail;

	if (repeat->length <= 0 || repeat->at + repeat->length > *count)
		return;

	/* A template that is already expanded returns to its single repeat first. */
	if (repeat->count > 1) {
		tail = *count - (repeat->at + repeat->length * repeat->count);
		memmove(&tracks[repeat->at + repeat->length],
		    &tracks[repeat->at + repeat->length * repeat->count],
		    (size_t)tail * sizeof(tracks[0]));
		*count -= repeat->length * (repeat->count - 1);
	}
	repeat->count = 1;

	/* The repeats that fit: the other tracks and the gaps come out of the room first. */
	times = 1;
	if (available >= 0) {
		others = 0;
		others_count = 0;
		for (index = 0; index < *count; index++) {
			if (index >= repeat->at && index < repeat->at + repeat->length)
				continue;
			others += grid_repeat_track(&tracks[index], available);
			others_count++;
		}
		pattern = 0;
		for (index = repeat->at; index < repeat->at + repeat->length; index++)
			pattern += grid_repeat_track(&tracks[index], available);
		denominator = pattern + gap * (layout_unit)repeat->length;
		numerator = available - others - gap * (layout_unit)(others_count - 1);
		if (denominator > 0 && numerator > denominator)
			times = (int)(numerator / denominator);
		if (times < 1)
			times = 1;
	}
	if ((*count - repeat->length) + times * repeat->length > CSS_TRACKS)
		times = (CSS_TRACKS - (*count - repeat->length)) / repeat->length;
	if (times <= 1)
		return;

	/* Shifts the tracks after the pattern and copies the pattern. */
	tail = *count - (repeat->at + repeat->length);
	memmove(&tracks[repeat->at + repeat->length * times],
	    &tracks[repeat->at + repeat->length], (size_t)tail * sizeof(tracks[0]));
	for (index = 1; index < times; index++)
		memcpy(&tracks[repeat->at + repeat->length * index], &tracks[repeat->at],
		    (size_t)repeat->length * sizeof(tracks[0]));
	*count += repeat->length * (times - 1);
	repeat->count = times;
}

/* Expands the auto-fill and auto-fit repeats of a horizontal grid against its definite sizes. */
static void
grid_expand_repeats(
	struct layout_tree *tree,
	struct layout_box *box)
{
	int measuring;

	layout_unit available;
	layout_unit gap;
	layout_unit frame;
	int definite;

	measuring = tree->measuring;

	/* The columns: a block-level grid fills its container; others need a width of their own. */
	gap = 0;
	if (box->style.column_gap.unit == CSS_UNIT_PX)
		gap = layout_from_px(box->style.column_gap.value);
	if (box->style.column_gap.unit == CSS_UNIT_PERCENT)
		gap = (layout_unit)((float)box->width * box->style.column_gap.value / 100.0f) +
		    layout_from_px(box->style.column_gap.offset);
	available = -1;
	definite = box->style.width.unit != CSS_UNIT_AUTO ||
	    (box->style.display == CSS_DISPLAY_GRID && box->floating == CSS_FLOAT_NONE &&
	    !box->out_of_flow);
	if (definite && !measuring)
		available = box->width;
	if (available < 0 && box->style.max_width.unit == CSS_UNIT_PX)
		available = layout_from_px(box->style.max_width.value);
	if (available < 0 && box->style.min_width.unit == CSS_UNIT_PX)
		available = layout_from_px(box->style.min_width.value);
	if (available < 0 && box->style.min_width.unit == CSS_UNIT_PERCENT && box->parent != NULL)
		available = (layout_unit)((float)box->parent->width * box->style.min_width.value / 100.0f);
	grid_expand_axis(box->style.columns, &box->style.column_count, &box->style.column_repeat,
	    available, gap);

	/* The rows need a definite height. */
	gap = 0;
	available = -1;
	if (box->style.row_gap.unit == CSS_UNIT_PX)
		gap = layout_from_px(box->style.row_gap.value);
	frame = 0;
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
		frame = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	if (box->style.height.unit == CSS_UNIT_PX) {
		/* A pixel height, within its pixel minimum and maximum. */
		available = layout_from_px(box->style.height.value);
		if (box->style.max_height.unit == CSS_UNIT_PX && available > layout_from_px(box->style.max_height.value))
			available = layout_from_px(box->style.max_height.value);
		if (box->style.min_height.unit == CSS_UNIT_PX && available < layout_from_px(box->style.min_height.value))
			available = layout_from_px(box->style.min_height.value);
		available -= frame;
		if (available < 0)
			available = 0;
	} else if (box->style.max_height.unit == CSS_UNIT_PX) {
		available = layout_from_px(box->style.max_height.value);
	} else if (box->style.min_height.unit == CSS_UNIT_PX) {
		available = layout_from_px(box->style.min_height.value);
	} else if (box->style.min_height.unit == CSS_UNIT_PERCENT && tree->containing_height_definite) {
		available = (layout_unit)((float)tree->containing_height * box->style.min_height.value / 100.0f);
	}
	grid_expand_axis(box->style.rows, &box->style.row_count, &box->style.row_repeat,
	    available, gap);
}

/* Marks the tracks of an auto-fit repeat that hold no item (the others keep their sizes). */
static int
grid_collapse_tracks(
	struct layout_box *box,
	struct grid_state *grid)
{
	const struct grid_item *item;
	size_t index;
	int track;
	int last;

	/* Columns. */
	if (box->style.column_repeat.fit && box->style.column_repeat.count > 0) {
		grid->column_collapsed = calloc((size_t)grid->column_count + 1U, 1U);
		if (grid->column_collapsed == NULL)
			return ENOMEM;
		last = box->style.column_repeat.at +
		    box->style.column_repeat.length * box->style.column_repeat.count;
		for (track = box->style.column_repeat.at; track < last && track < grid->column_count; track++)
			grid->column_collapsed[track] = 1;
		for (index = 0; index < grid->item_count; index++) {
			item = &grid->items[index];
			for (track = item->column; track < item->column + item->column_span &&
			    track < grid->column_count; track++)
				grid->column_collapsed[track] = 0;
		}
	}

	/* Rows. */
	if (box->style.row_repeat.fit && box->style.row_repeat.count > 0) {
		grid->row_collapsed = calloc((size_t)grid->row_count + 1U, 1U);
		if (grid->row_collapsed == NULL)
			return ENOMEM;
		last = box->style.row_repeat.at +
		    box->style.row_repeat.length * box->style.row_repeat.count;
		for (track = box->style.row_repeat.at; track < last && track < grid->row_count; track++)
			grid->row_collapsed[track] = 1;
		for (index = 0; index < grid->item_count; index++) {
			item = &grid->items[index];
			for (track = item->row; track < item->row + item->row_span &&
			    track < grid->row_count; track++)
				grid->row_collapsed[track] = 0;
		}
	}
	return 0;
}

/* Counts the tracks an auto-fit repeat did not collapse (the ones content distribution spaces). */
static int
grid_live_tracks(
	const unsigned char *collapsed,
	int count)
{
	int live;
	int index;

	/* Every track without a collapsed mark. */
	live = 0;
	for (index = 0; index < count; index++) {
		if (collapsed == NULL || !collapsed[index])
			live++;
	}
	return live;
}

/* Normal content distribution stretches the auto tracks. */
static int
grid_content_stretches(
	int value)
{
	return value == CSS_ALIGN_NORMAL || value == CSS_ALIGN_STRETCH;
}

/*
 * Resolves a content-distribution value over the free space of a track
 * list: the physical offset of the first track (from the left or top) and
 * the extra space added to each gap.  A reversed list starts at the far end.
 */
static void
grid_distribute(
	int value,
	int reversed,
	layout_unit free_space,
	int tracks,
	layout_unit *offset,
	layout_unit *extra)
{
	*offset = 0;
	*extra = 0;
	if (tracks <= 0)
		return;

	/* Distribution needs positive space; an overflow falls back as the specification says. */
	if (free_space <= 0 || (tracks == 1 && value == CSS_ALIGN_SPACE_BETWEEN)) {
		if (value == CSS_ALIGN_SPACE_BETWEEN)
			value = CSS_ALIGN_START;
		else if (value == CSS_ALIGN_SPACE_AROUND || value == CSS_ALIGN_SPACE_EVENLY)
			value = free_space < 0 ? CSS_ALIGN_START : CSS_ALIGN_CENTER;
	}
	switch (value) {
	case CSS_ALIGN_RIGHT:
		*offset = free_space;
		break;
	case CSS_ALIGN_END:
		if (!reversed)
			*offset = free_space;
		break;
	case CSS_ALIGN_START:
	case CSS_ALIGN_NORMAL:
	case CSS_ALIGN_STRETCH:
		if (reversed)
			*offset = free_space;
		break;
	case CSS_ALIGN_CENTER:
		*offset = free_space / 2;
		break;
	case CSS_ALIGN_SPACE_BETWEEN:
		*extra = free_space / (layout_unit)(tracks - 1);
		break;
	case CSS_ALIGN_SPACE_AROUND:
		*extra = free_space / (layout_unit)tracks;
		*offset = *extra / 2;
		break;
	case CSS_ALIGN_SPACE_EVENLY:
		*extra = free_space / (layout_unit)(tracks + 1);
		*offset = *extra;
		break;
	default:
		break;
	}
}

/* Existing in-flow Grid paths use their logical start and end positions. */
static int
grid_in_flow_align(int align)
{
	if (align == CSS_ALIGN_SELF_START)
		return CSS_ALIGN_START;
	if (align == CSS_ALIGN_SELF_END)
		return CSS_ALIGN_END;
	return align;
}

/*
 * Lays out the content of a grid container: its in-flow children as grid
 * items in their areas; sets their positions (their border boxes relative
 * to the container's content box), the container's content height, and
 * the width its columns need at their content's sizes (for the
 * containers around it that shrink to fit).
 */
int
layout_grid(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct grid_state grid;
	struct grid_item *item;
	struct layout_box *child;
	layout_unit column_gap;
	layout_unit row_gap;
	layout_unit row_gap_base;
	layout_unit x;
	layout_unit y;
	layout_unit area_width;
	layout_unit area_height;
	layout_unit outer;
	layout_unit outer_width;
	layout_unit frame;
	layout_unit room;
	layout_unit used;
	layout_unit shift;
	layout_unit row_shift;
	layout_unit distribution;
	layout_unit baseline;
	layout_unit first_above;
	layout_unit first_below;
	layout_unit last_above;
	layout_unit last_below;
	layout_unit right_extent;
	layout_unit shared_right_extent;
	layout_unit free_height;
	layout_unit saved_height;
	size_t index;
	size_t peer;
	int position;
	int justify;
	int align;
	int baseline_group;
	int auto_top;
	int auto_bottom;
	int auto_block_margin;
	int saved_definite;
	int handled;
	int error;

	/* Only a final layout can replace a previously published used-track snapshot. */
	if (tree->measuring == 0) {
		box->grid_used_valid = 0;
		box->grid_used_columns = NULL;
		box->grid_used_rows = NULL;
		box->grid_used_column_count = 0;
		box->grid_used_row_count = 0;
	}

	/* The children out of the flow have their static place at the content box's corner. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (!child->out_of_flow)
			continue;
		child->static_x = 0;
		child->static_y = 0;
		child->static_x_from_right = 0;
		child->static_x_area_width = 0;
		child->static_x_align = CSS_ALIGN_AUTO;
		child->static_x_align_from_right = 0;
		child->static_y_area_height = 0;
		child->static_y_align = CSS_ALIGN_AUTO;
		child->static_grid_area = 0;
		child->cb_set = 0;
		child->static_unsafe = 1;
		child->static_x_safe = 0;
		child->static_y_safe = 0;
	}

	/* Fixed vertical grids map their logical tracks to physical axes before laying out items. */
	handled = 0;
	error = grid_vertical_fixed(tree, box, &handled);
	if (error != 0)
		return error;
	if (handled)
		return 0;
	error = grid_vertical_auto(tree, box, &handled);
	if (error != 0)
		return error;
	if (handled)
		return 0;

	/* Automatic repeats take their track counts from the definite size. */
	grid_expand_repeats(tree, box);

	/* Named lines and spans become line numbers once the repeats have their counts. */
	layout_grid_names(box);

	/* The items, placed in their cells. */
	memset(&grid, 0, sizeof(grid));
	error = grid_collect(box, &grid);
	if (error == 0)
		error = grid_place(&grid, box->style.grid_auto_flow);
	if (error == 0)
		error = grid_collapse_tracks(box, &grid);
	if (error != 0) {
		grid_release(&grid);
		return error;
	}

	/* The gaps between the tracks. */
	column_gap = 0;
	if (box->style.column_gap.unit == CSS_UNIT_PX)
		column_gap = layout_from_px(box->style.column_gap.value);
	if (box->style.column_gap.unit == CSS_UNIT_PERCENT && tree->measuring)
		column_gap = layout_from_px(box->style.column_gap.offset);
	else if (box->style.column_gap.unit == CSS_UNIT_PERCENT)
		column_gap = (layout_unit)((float)box->width * box->style.column_gap.value / 100.0f) +
		    layout_from_px(box->style.column_gap.offset);
	row_gap = 0;
	if (box->style.row_gap.unit == CSS_UNIT_PX)
		row_gap = layout_from_px(box->style.row_gap.value);
	if (box->style.row_gap.unit == CSS_UNIT_PERCENT && box->style.height.unit == CSS_UNIT_PX) {
		/* A percentage row gap is of the definite content height. */
		row_gap = layout_from_px(box->style.height.value);
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
			row_gap -= box->border[CSS_TOP] + box->padding[CSS_TOP] +
			    box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		if (row_gap < 0)
			row_gap = 0;
		row_gap = (layout_unit)((float)row_gap * box->style.row_gap.value / 100.0f) +
		    layout_from_px(box->style.row_gap.offset);
	}

	/* An item with a vertical writing mode lays its lines along the row's height. */
	for (index = 0; index < grid.item_count; index++) {
		item = &grid.items[index];
		child = item->box;
		child->item_row = item->row + 1;
		child->item_column = item->column + 1;
		child->grid_orthogonal = 0;
		child->grid_room = -1;
		if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB ||
		    child->style.writing_mode == CSS_WRITING_HORIZONTAL_TB ||
		    child->replaced || child->atomic)
			continue;
		child->grid_orthogonal = 1;
		if (item->row_span == 1 && item->row < box->style.row_count &&
		    grid_row_track(&box->style, item->row)->kind == CSS_TRACK_LENGTH &&
		    grid_row_track(&box->style, item->row)->size.unit == CSS_UNIT_PX)
			child->grid_room = layout_from_px(grid_row_track(&box->style, item->row)->size.value);
	}

	row_gap_base = row_gap;

	/* The columns' widths. */
	error = grid_size_columns(tree, box, &grid, column_gap);
	if (error != 0) {
		grid_release(&grid);
		return error;
	}

	/* A collapsed track and its gap take no room together. */
	for (position = 0; grid.column_collapsed != NULL && position < grid.column_count; position++) {
		if (grid.column_collapsed[position])
			grid.columns[position] = -column_gap;
	}

	/* The columns side by side: where they start (justify-content moves a grid narrower than its box). */
	used = column_gap * (layout_unit)(grid.column_count - 1);
	for (position = 0; position < grid.column_count; position++)
		used += grid.columns[position];
	box->grid_content = used;
	room = box->width - used;
	/* A safe alignment that would overflow falls back to the start. */
	grid_distribute((box->style.flex_keywords & 16u) != 0 && room < 0 ? CSS_ALIGN_START : box->style.justify_content,
	    box->style.direction == CSS_DIRECTION_RTL &&
	    box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB,
	    room, grid_live_tracks(grid.column_collapsed, grid.column_count), &shift, &distribution);
	column_gap += distribution;

	/* A collapsed track still takes its widened gap with it. */
	for (position = 0; grid.column_collapsed != NULL && position < grid.column_count; position++) {
		if (grid.column_collapsed[position])
			grid.columns[position] = -column_gap;
	}

	/* Each item laid out in its area's width. */
	for (index = 0; index < grid.item_count; index++) {
		item = &grid.items[index];
		area_width = column_gap * (layout_unit)(item->column_span - 1);
		for (position = item->column; position < item->column + item->column_span; position++)
			area_width += grid.columns[position];
		justify = item->box->style.justify_self;
		if (justify == CSS_ALIGN_AUTO)
			justify = box->style.justify_items;
		if (justify == CSS_ALIGN_NORMAL)
			justify = item->box->replaced ? CSS_ALIGN_START : CSS_ALIGN_STRETCH;
		if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB ||
		    (item->box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB &&
		    !item->box->grid_orthogonal) ||
		    item->box->style.min_width.unit != CSS_UNIT_AUTO ||
		    item->box->style.max_width.unit != CSS_UNIT_NONE)
			justify = CSS_ALIGN_STRETCH;
		if (item->box->style.margin[CSS_LEFT].unit == CSS_UNIT_AUTO ||
		    item->box->style.margin[CSS_RIGHT].unit == CSS_UNIT_AUTO)
			justify = CSS_ALIGN_START;

		/* Percentage heights are of the grid area: its fixed rows' height, or indefinite while the rows are sized. */
		saved_height = tree->containing_height;
		saved_definite = tree->containing_height_definite;
		if (box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB) {
			tree->containing_height_definite = grid_fixed_area_height(box, item, row_gap, &tree->containing_height);
			if (!tree->containing_height_definite)
				tree->containing_height = 0;
		}
		error = grid_lay_item(tree, item->box, area_width, justify);
		tree->containing_height = saved_height;
		tree->containing_height_definite = saved_definite;
		if (error != 0) {
			grid_release(&grid);
			return error;
		}
	}

	/* The rows' heights from the items. */
	grid_size_rows(box, &grid, row_gap);
	for (position = 0; grid.row_collapsed != NULL && position < grid.row_count; position++) {
		if (grid.row_collapsed[position])
			grid.rows[position] = -row_gap;
	}
	row_shift = 0;
	if (box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
	    box->style.height.unit == CSS_UNIT_PX) {
		layout_unit available_height;

		available_height = layout_from_px(box->style.height.value);
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
			available_height -= box->border[CSS_TOP] + box->padding[CSS_TOP] +
			    box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		used = 0;
		if (grid.row_count > 0)
			used = row_gap * (layout_unit)(grid.row_count - 1);
		for (position = 0; position < grid.row_count; position++)
			used += grid.rows[position];
		if (available_height < 0)
			available_height = 0;
		grid_distribute((box->style.flex_keywords & 32u) != 0 && available_height < used ? CSS_ALIGN_START :
		    box->style.align_content, 0, available_height - used,
		    grid_live_tracks(grid.row_collapsed, grid.row_count), &row_shift, &distribution);
		row_gap += distribution;
		for (position = 0; grid.row_collapsed != NULL && position < grid.row_count; position++) {
			if (grid.row_collapsed[position])
				grid.rows[position] = -row_gap;
		}
	}

	/*
	 * A percentage row gap of an indefinite height sized the rows as zero;
	 * the items are placed with it resolved against the height that gave.
	 */
	if (box->style.row_gap.unit == CSS_UNIT_PERCENT && box->style.height.unit != CSS_UNIT_PX && grid.row_count > 1) {
		used = 0;
		for (position = 0; position < grid.row_count; position++)
			used += grid.rows[position];
		row_gap = (layout_unit)((float)used * box->style.row_gap.value / 100.0f) +
		    layout_from_px(box->style.row_gap.offset);
		if (row_gap < 0)
			row_gap = 0;
	}

	/* Absolute children use valid definite grid areas without sizing the tracks. */
	grid_static_position(box, &grid, column_gap, row_gap, shift, row_shift);
	grid_abspos_areas(box, &grid, column_gap, row_gap, shift, row_shift);

	/* Each item in its area, aligned in it across the rows. */
	for (index = 0; index < grid.item_count; index++) {
		item = &grid.items[index];
		child = item->box;

		/* The area's width, then its corner and height. */
		area_width = column_gap * (layout_unit)(item->column_span - 1);
		for (position = item->column; position < item->column + item->column_span; position++)
			area_width += grid.columns[position];
		x = shift;
		if (box->style.direction == CSS_DIRECTION_RTL &&
		    box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB) {
			for (position = item->column + item->column_span;
			     position < grid.column_count; position++)
				x += grid.columns[position] + column_gap;
		} else {
			for (position = 0; position < item->column; position++)
				x += grid.columns[position] + column_gap;
		}
		y = row_shift;
		for (position = 0; position < item->row; position++)
			y += grid.rows[position] + row_gap;
		area_height = row_gap * (layout_unit)(item->row_span - 1);
		for (position = item->row; position < item->row + item->row_span; position++)
			area_height += grid.rows[position];
		/* The item's alignment: its align-self, or the container's align-items. */
		align = grid_in_flow_align(child->style.align_self);
		if (child->grid_orthogonal && child->style.direction == CSS_DIRECTION_RTL) {
			/* The item's inline axis is vertical and runs from the bottom. */
			if (child->style.align_self == CSS_ALIGN_SELF_START)
				align = CSS_ALIGN_END;
			else if (child->style.align_self == CSS_ALIGN_SELF_END)
				align = CSS_ALIGN_START;
		}
		if (align == CSS_ALIGN_AUTO)
			align = box->style.align_items;
		if (align == CSS_ALIGN_NORMAL)
			align = child->replaced ? CSS_ALIGN_START : CSS_ALIGN_STRETCH;
		/* Definite horizontal rows distribute block-axis auto margins after sizing. */
		auto_top = child->style.margin[CSS_TOP].unit == CSS_UNIT_AUTO;
		auto_bottom = child->style.margin[CSS_BOTTOM].unit == CSS_UNIT_AUTO;
		auto_block_margin = 0;
		if ((auto_top || auto_bottom) && box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
		    item->row_span == 1 && item->row < box->style.row_count &&
		    box->style.rows[item->row].kind == CSS_TRACK_LENGTH) {
			if (box->style.rows[item->row].size.unit == CSS_UNIT_PX ||
			    (box->style.rows[item->row].size.unit == CSS_UNIT_PERCENT &&
			    box->style.height.unit == CSS_UNIT_PX))
				auto_block_margin = 1;
		}
		/* Any other row of one item has its final height by now too. */
		if ((auto_top || auto_bottom) && box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
		    child->style.writing_mode == CSS_WRITING_HORIZONTAL_TB && item->row_span == 1)
			auto_block_margin = 1;
		if (auto_block_margin)
			align = CSS_ALIGN_START;
		baseline_group = 0;
		if (!auto_block_margin)
			baseline_group = grid_row_baseline(box, item, &baseline);
		if (baseline_group != 0)
			align = CSS_ALIGN_BASELINE;
		else if (align == CSS_ALIGN_BASELINE)
			align = CSS_ALIGN_START;

		/* A definite grid area must reach percentage descendants of a stretched item. */
		error = grid_relay_item(tree, child, area_width, area_height, align);
		if (error != 0) {
			grid_release(&grid);
			return error;
		}

		/* A stretching item without a height of its own fills the area's height. */
		frame = child->border[CSS_TOP] + child->padding[CSS_TOP] + child->padding[CSS_BOTTOM] + child->border[CSS_BOTTOM];
		if (align == CSS_ALIGN_STRETCH && child->style.height.unit == CSS_UNIT_AUTO) {
			child->height = area_height - child->margin[CSS_TOP] - child->margin[CSS_BOTTOM] - frame;
			if (child->height < 0)
				child->height = 0;
			child->height = grid_clamp_height(child, child->height, area_height, frame);
		}

		/* Its place, moved down by the room it leaves when it is at the end or the center. */
		child->x = x + child->margin[CSS_LEFT];
		if (box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
		    (child->style.width.unit != CSS_UNIT_AUTO ||
		    ((child->style.writing_mode == CSS_WRITING_HORIZONTAL_TB || child->grid_orthogonal) &&
		    child->style.min_width.unit == CSS_UNIT_AUTO &&
		    child->style.max_width.unit == CSS_UNIT_NONE &&
		    child->style.margin[CSS_LEFT].unit != CSS_UNIT_AUTO &&
		    child->style.margin[CSS_RIGHT].unit != CSS_UNIT_AUTO))) {
			justify = child->style.justify_self;
			if (justify == CSS_ALIGN_AUTO)
				justify = box->style.justify_items;
			if (justify == CSS_ALIGN_NORMAL)
				justify = child->replaced ? CSS_ALIGN_START : CSS_ALIGN_STRETCH;
			if (justify == CSS_ALIGN_STRETCH)
				justify = CSS_ALIGN_START;
			if (justify == CSS_ALIGN_SELF_START && child->grid_orthogonal)
				justify = child->style.writing_mode == CSS_WRITING_VERTICAL_RL ?
				    CSS_ALIGN_RIGHT : CSS_ALIGN_LEFT;
			else if (justify == CSS_ALIGN_SELF_END && child->grid_orthogonal)
				justify = child->style.writing_mode == CSS_WRITING_VERTICAL_RL ?
				    CSS_ALIGN_LEFT : CSS_ALIGN_RIGHT;
			if (justify == CSS_ALIGN_SELF_START)
				justify = child->style.direction == CSS_DIRECTION_RTL ? CSS_ALIGN_RIGHT : CSS_ALIGN_LEFT;
			if (justify == CSS_ALIGN_SELF_END)
				justify = child->style.direction == CSS_DIRECTION_RTL ? CSS_ALIGN_LEFT : CSS_ALIGN_RIGHT;
			outer_width = child->margin[CSS_LEFT] + child->border[CSS_LEFT] +
			    child->padding[CSS_LEFT] + child->width + child->padding[CSS_RIGHT] +
			    child->border[CSS_RIGHT] + child->margin[CSS_RIGHT];
			if (area_width > outer_width &&
			    (justify == CSS_ALIGN_RIGHT ||
			    (justify == CSS_ALIGN_END && box->style.direction == CSS_DIRECTION_LTR) ||
			    (justify == CSS_ALIGN_START && box->style.direction == CSS_DIRECTION_RTL)))
				child->x += area_width - outer_width;
			if (area_width > outer_width && justify == CSS_ALIGN_CENTER)
				child->x += (area_width - outer_width) / 2;
		}
		if (grid_fixed_column_baseline(box, item, &right_extent)) {
			/* Orthogonal empty items share a border-left baseline in this column. */
			shared_right_extent = 0;
			for (peer = 0; peer < grid.item_count; peer++) {
				if (grid.items[peer].column != item->column)
					continue;
				if (!grid_fixed_column_baseline(box, &grid.items[peer], &right_extent))
					continue;
				if (right_extent > shared_right_extent)
					shared_right_extent = right_extent;
			}
			child->x = x + area_width - shared_right_extent;
		}
		child->y = y + child->margin[CSS_TOP];
		if (auto_block_margin) {
			/* Auto margins absorb only positive space left in the final row. */
			free_height = area_height - grid_outer_height(child);
			if (free_height > 0 && auto_top)
				child->y += auto_bottom ? free_height / 2 : free_height;
		}
		if (baseline_group != 0) {
			/* Members of the row share their group's baseline: the first group's below the row's top, the last's above its bottom. */
			grid_row_baselines(box, &grid, item->row, &first_above, &first_below, &last_above, &last_below);
			if (baseline_group == 1)
				child->y += first_above - baseline;
			else
				child->y += area_height - last_below - baseline;
		}
		outer = grid_outer_height(child);
		if (align == CSS_ALIGN_END && area_height > outer)
			child->y += area_height - outer;
		if (align == CSS_ALIGN_CENTER && area_height > outer)
			child->y += (area_height - outer) / 2;
	}

	/* The container's content height: its rows and the gaps between them. */
	row_gap = row_gap_base;
	box->height = row_gap * (layout_unit)(grid.row_count - 1);
	if (grid.row_count == 0)
		box->height = 0;
	for (position = 0; position < grid.row_count; position++)
		box->height += grid.rows[position];

	/* Keep the actual resolved tracks before the transient sizing arrays leave. */
	error = grid_save_used_tracks(tree, box, &grid);
	if (error != 0) {
		grid_release(&grid);
		return error;
	}

	/* The state is no longer needed. */
	grid_release(&grid);

	/* Succeeded: the container's content is laid out. */
	return 0;
}

/* Publishes final horizontal used tracks under the layout arena's lifetime. */
static int
grid_save_used_tracks(
	struct layout_tree *tree,
	struct layout_box *box,
	const struct grid_state *grid)
{
	double *columns;
	double *rows;
	int column_count;
	int row_count;
	int error;

	/* Intrinsic measurement and the separate vertical solvers do not publish this snapshot. */
	if (tree->measuring != 0)
		return 0;

	/* Vertical resolved values require their own logical-axis snapshot in a later partial scope. */
	if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB)
		return 0;

	/* Empty-grid algorithm placeholders are not real implicit tracks. */
	column_count = grid->column_count;
	row_count = grid->row_count;
	if (grid->item_count == 0) {
		/* Explicit tracks remain real even when no item occupies them. */
		if (box->style.column_count == 0)
			column_count = 0;

		/* The independent row axis can also have no explicit or implicit track. */
		if (box->style.row_count == 0)
			row_count = 0;
	}

	/* Both independent arrays must be complete before the box marks its snapshot valid. */
	error = grid_copy_used_axis(tree, grid->columns, column_count, &columns);
	if (error != 0)
		return error;

	/* A later failure leaves only arena-owned unpublished scratch storage. */
	error = grid_copy_used_axis(tree, grid->rows, row_count, &rows);
	if (error != 0)
		return error;

	/* These arrays survive sizing-state release and leave with the entire layout arena. */
	box->grid_used_columns = columns;
	box->grid_used_rows = rows;
	box->grid_used_column_count = column_count;
	box->grid_used_row_count = row_count;
	box->grid_used_valid = 1;

	/* Succeeded: the snapshot describes this final horizontal Grid layout. */
	return 0;
}

/* Copies one resolved track axis to arena-owned CSS-pixel values. */
static int
grid_copy_used_axis(
	struct layout_tree *tree,
	const layout_unit *sizes,
	int count,
	double **copy)
{
	double *pixels;
	layout_unit length;
	int index;

	/* Empty axes have no allocation and serialize as none. */
	*copy = NULL;
	if (count == 0)
		return 0;

	/* Reject an invalid native count before sizing the arena allocation. */
	if (count < 0)
		return EINVAL;

	/* Preserve the multiplication bound on both 32-bit and 64-bit hosts. */
	if ((size_t)count > SIZE_MAX / sizeof(*pixels))
		return ENOMEM;

	/* The layout arena retains these values until its complete generation is released. */
	pixels = wb_arena_alloc(&tree->arena, (size_t)count * sizeof(*pixels));
	if (pixels == NULL)
		return ENOMEM;

	/* Collapsed tracks use negative gap placeholders internally but resolve to zero CSS pixels. */
	for (index = 0; index < count; index++) {
		length = sizes[index];
		if (length < 0)
			length = 0;

		/* Convert actual solver units without including distribution gaps in track sizes. */
		pixels[index] = layout_to_px(length);
	}

	/* Succeeded: the caller receives the complete arena-owned axis. */
	*copy = pixels;
	return 0;
}

/* Lays out two implicit auto tracks on each physical axis of a definite vertical Grid. */
static int
grid_vertical_auto(
	struct layout_tree *tree,
	struct layout_box *box,
	int *handled)
{
	struct grid_state grid;
	struct grid_item *placed;
	struct layout_box *child;
	struct layout_box *text_box;
	struct css_length saved_height;
	const struct css_track *track;
	const struct layout_line *line;
	const struct layout_fragment *last;
	layout_unit columns[2];
	layout_unit rows[2];
	layout_unit natural_height[4];
	layout_unit width;
	layout_unit height;
	layout_unit area_width;
	layout_unit area_height;
	layout_unit outer_width;
	layout_unit outer_height;
	layout_unit content_height;
	layout_unit remaining;
	int occupied[4];
	size_t index;
	size_t line_index;
	size_t text_index;
	int align;
	int justify;
	int side;
	int cell;
	int error;

	/* Leaves other Grid forms to the existing layout path. */
	*handled = 0;
	if (box->style.display != CSS_DISPLAY_INLINE_GRID ||
	    (box->style.writing_mode != CSS_WRITING_VERTICAL_LR &&
	    box->style.writing_mode != CSS_WRITING_VERTICAL_RL) ||
	    box->style.direction != CSS_DIRECTION_LTR ||
	    box->style.width.unit != CSS_UNIT_PX ||
	    box->style.height.unit != CSS_UNIT_PX ||
	    box->style.box_sizing != CSS_BOX_SIZING_CONTENT ||
	    box->style.column_count != 0 || box->style.row_count != 0 ||
	    box->style.column_gap.value != 0 || box->style.row_gap.value != 0)
		return 0;

	/* Padding and borders require a separate content-box mapping. */
	for (side = 0; side < 4; side++) {
		if (box->border[side] != 0 || box->padding[side] != 0)
			return 0;
	}

	/* Every item must use the bounded text and box model. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->replaced ||
		    (child->style.writing_mode != box->style.writing_mode &&
		    child->style.writing_mode != CSS_WRITING_HORIZONTAL_TB) ||
		    child->style.width.unit != CSS_UNIT_AUTO ||
		    child->style.height.unit != CSS_UNIT_AUTO)
			return 0;

		/* The measured path uses one unwrapped sideways Latin text run. */
		text_box = child->first_child;
		if (text_box == NULL || text_box->kind != LAYOUT_TEXT ||
		    text_box->next != NULL ||
		    (child->style.writing_mode == box->style.writing_mode &&
		    child->style.text_orientation == CSS_TEXT_ORIENTATION_UPRIGHT))
			return 0;

		/* Other scripts need upright glyph metrics and shaping. */
		for (text_index = 0; text_index < text_box->text_length; text_index++) {
			if (text_box->text[text_index] > 0x7fU)
				return 0;
		}

		/* The physical frame and margins must have definite nonnegative sizes. */
		layout_box_model(child, 0);
		if (child->style.box_sizing != CSS_BOX_SIZING_CONTENT)
			return 0;
		for (side = 0; side < 4; side++) {
			if (child->style.margin[side].unit != CSS_UNIT_PX ||
			    child->style.padding[side].unit != CSS_UNIT_PX ||
			    child->margin[side] < 0 || child->border[side] < 0 ||
			    child->padding[side] < 0)
				return 0;
		}
	}

	/* Collects the items before the common placer resolves their implicit tracks. */
	memset(&grid, 0, sizeof(grid));
	error = grid_collect(box, &grid);
	if (error != 0) {
		grid_release(&grid);
		return error;
	}

	/* Resolves each explicit line against the two implicit auto tracks. */
	error = grid_place(&grid, box->style.grid_auto_flow);
	if (error != 0) {
		grid_release(&grid);
		return error;
	}

	/* A larger or incomplete grid belongs to the general path. */
	if (grid.column_count != 2 || grid.row_count != 2 || grid.item_count != 4) {
		grid_release(&grid);
		return 0;
	}

	/* Every implicit track must have the default auto sizing function. */
	for (cell = 0; cell < 2; cell++) {
		track = grid_column_track(&box->style, cell);
		if (track->kind != CSS_TRACK_AUTO) {
			grid_release(&grid);
			return 0;
		}
		track = grid_row_track(&box->style, cell);
		if (track->kind != CSS_TRACK_AUTO) {
			grid_release(&grid);
			return 0;
		}
	}

	/* One item per cell keeps the item and track contributions unambiguous. */
	memset(occupied, 0, sizeof(occupied));
	for (index = 0; index < grid.item_count; index++) {
		placed = &grid.items[index];
		if (placed->column_span != 1 || placed->row_span != 1 ||
		    placed->column < 0 || placed->column >= 2 ||
		    placed->row < 0 || placed->row >= 2) {
			grid_release(&grid);
			return 0;
		}
		cell = placed->row * 2 + placed->column;
		if (occupied[cell]) {
			grid_release(&grid);
			return 0;
		}
		occupied[cell] = 1;

		/* The bounded painter and placement handle only start and stretch. */
		child = placed->box;
		align = grid_in_flow_align(child->style.align_self);
		if (align == CSS_ALIGN_AUTO)
			align = box->style.align_items;
		if (align == CSS_ALIGN_NORMAL)
			align = CSS_ALIGN_STRETCH;
		justify = child->style.justify_self;
		if (justify == CSS_ALIGN_AUTO)
			justify = box->style.justify_items;
		if (justify == CSS_ALIGN_NORMAL)
			justify = CSS_ALIGN_STRETCH;
		if ((align != CSS_ALIGN_START && align != CSS_ALIGN_STRETCH) ||
		    (justify != CSS_ALIGN_START && justify != CSS_ALIGN_STRETCH)) {
			grid_release(&grid);
			return 0;
		}
	}

	/* Measure each item's physical content extents before sharing free track room. */
	columns[0] = 0;
	columns[1] = 0;
	rows[0] = 0;
	rows[1] = 0;
	for (index = 0; index < grid.item_count; index++) {
		placed = &grid.items[index];
		child = placed->box;
		saved_height = child->style.height;

		/* A definite inline room gives a vertical text run its physical line width. */
		if (child->style.writing_mode == box->style.writing_mode) {
			child->style.height.unit = CSS_UNIT_PX;
			content_height = layout_from_px(box->style.height.value) -
			    child->margin[CSS_TOP] - child->margin[CSS_BOTTOM] -
			    child->border[CSS_TOP] - child->border[CSS_BOTTOM] -
			    child->padding[CSS_TOP] - child->padding[CSS_BOTTOM];
			if (content_height < 0)
				content_height = 0;
			child->style.height.value = layout_to_px(content_height);
			child->style.height.offset = 0;
			child->max_content_known = 0;
		}
		error = layout_max_content(tree, child, &width);
		child->style.height = saved_height;
		if (error != 0) {
			grid_release(&grid);
			return error;
		}
		height = child->height;
		if (child->style.writing_mode == box->style.writing_mode) {
			height = 0;

			/* The last glyph of each line gives its natural inline advance. */
			for (line_index = 0; line_index < child->line_count; line_index++) {
				line = &child->lines[line_index];
				if (line->fragment_count == 0)
					continue;
				last = &line->fragments[line->fragment_count - 1U];
				if (last->x + last->width > height)
					height = last->x + last->width;
			}
		}
		natural_height[index] = height;
		outer_width = width + child->margin[CSS_LEFT] + child->margin[CSS_RIGHT] +
		    child->border[CSS_LEFT] + child->border[CSS_RIGHT] +
		    child->padding[CSS_LEFT] + child->padding[CSS_RIGHT];
		outer_height = height + child->margin[CSS_TOP] + child->margin[CSS_BOTTOM] +
		    child->border[CSS_TOP] + child->border[CSS_BOTTOM] +
		    child->padding[CSS_TOP] + child->padding[CSS_BOTTOM];
		if (outer_width > rows[placed->row])
			rows[placed->row] = outer_width;
		if (outer_height > columns[placed->column])
			columns[placed->column] = outer_height;
	}

	/* Each auto track receives an equal share after its content minimum. */
	remaining = box->width - rows[0] - rows[1];
	if (remaining > 0) {
		rows[0] += remaining / 2;
		rows[1] += remaining - remaining / 2;
	}
	remaining = layout_from_px(box->style.height.value) - columns[0] - columns[1];
	if (remaining > 0) {
		columns[0] += remaining / 2;
		columns[1] += remaining - remaining / 2;
	}
	box->height = layout_from_px(box->style.height.value);
	box->grid_content = rows[0] + rows[1];

	/* Lay each item out in its physical row width and column height. */
	for (index = 0; index < grid.item_count; index++) {
		placed = &grid.items[index];
		child = placed->box;
		area_width = rows[placed->row];
		area_height = columns[placed->column];
		align = grid_in_flow_align(child->style.align_self);
		if (align == CSS_ALIGN_AUTO)
			align = box->style.align_items;
		if (align == CSS_ALIGN_NORMAL)
			align = CSS_ALIGN_STRETCH;
		justify = child->style.justify_self;
		if (justify == CSS_ALIGN_AUTO)
			justify = box->style.justify_items;
		if (justify == CSS_ALIGN_NORMAL)
			justify = CSS_ALIGN_STRETCH;
		saved_height = child->style.height;
		if (child->style.writing_mode == box->style.writing_mode) {
			child->style.height.unit = CSS_UNIT_PX;
			content_height = area_height - child->margin[CSS_TOP] -
			    child->margin[CSS_BOTTOM] - child->border[CSS_TOP] -
			    child->border[CSS_BOTTOM] - child->padding[CSS_TOP] -
			    child->padding[CSS_BOTTOM];
			if (content_height < 0)
				content_height = 0;
			child->style.height.value = layout_to_px(content_height);
			child->style.height.offset = 0;
		}
		error = grid_lay_item(tree, child, area_width, align);
		if (error == 0 && child->style.writing_mode == CSS_WRITING_HORIZONTAL_TB)
			error = grid_relay_item(tree, child, area_width, area_height, justify);
		child->style.height = saved_height;
		if (error != 0) {
			grid_release(&grid);
			return error;
		}
		if (child->style.writing_mode == box->style.writing_mode && justify == CSS_ALIGN_START)
			child->height = natural_height[index];
		if (box->style.writing_mode == CSS_WRITING_VERTICAL_RL) {
			/* The row's block-start edge is on the physical right. */
			child->x = box->width - (placed->row == 0 ? 0 : rows[0]) -
			    child->margin[CSS_RIGHT] - child->border[CSS_LEFT] -
			    child->padding[CSS_LEFT] - child->width -
			    child->padding[CSS_RIGHT] - child->border[CSS_RIGHT];
		} else {
			child->x = (placed->row == 0 ? 0 : rows[0]) + child->margin[CSS_LEFT];
		}
		child->y = (placed->column == 0 ? 0 : columns[0]) + child->margin[CSS_TOP];
	}

	/* The common Grid state no longer owns any output. */
	grid_release(&grid);
	*handled = 1;

	/* Succeeded: the physical axis mapping consumed this Grid. */
	return 0;
}

/* Lays out bounded fixed vertical tracks in physical x/y axes. */
static int
grid_vertical_fixed(
	struct layout_tree *tree,
	struct layout_box *box,
	int *handled)
{
	struct grid_state grid;
	struct grid_item *placed;
	struct layout_box *child;
	layout_unit columns[CSS_TRACKS + 1];
	layout_unit rows[CSS_TRACKS + 1];
	layout_unit area_width;
	layout_unit area_height;
	size_t index;
	int column;
	int row;
	int align;
	int justify;
	int side;
	int error;

	/* Other writing modes and Grid forms retain their existing layout path. */
	*handled = 0;
	if ((box->style.writing_mode != CSS_WRITING_VERTICAL_LR &&
	    box->style.writing_mode != CSS_WRITING_VERTICAL_RL) ||
	    box->style.direction != CSS_DIRECTION_LTR ||
	    box->style.display != CSS_DISPLAY_INLINE_GRID ||
	    box->style.height.unit != CSS_UNIT_AUTO ||
	    box->style.column_count == 0 || box->style.row_count == 0 ||
	    box->style.column_gap.value != 0 || box->style.row_gap.value != 0)
		return 0;

	/* The explicit tracks must have definite pixel sizes. */
	for (column = 0; column < box->style.column_count; column++) {
		if (box->style.columns[column].kind != CSS_TRACK_LENGTH ||
		    box->style.columns[column].size.unit != CSS_UNIT_PX)
			return 0;
	}
	for (row = 0; row < box->style.row_count; row++) {
		if (box->style.rows[row].kind != CSS_TRACK_LENGTH ||
		    box->style.rows[row].size.unit != CSS_UNIT_PX)
			return 0;
	}

	/* An inline grid can be laid out again at its shrink-to-fit physical width. */
	area_width = 0;
	for (row = 0; row < box->style.row_count; row++)
		area_width += layout_from_px(box->style.rows[row].size.value);
	if (box->style.width.unit != CSS_UNIT_AUTO &&
	    (box->style.width.unit != CSS_UNIT_PX ||
	    layout_from_px(box->style.width.value) != area_width))
		return 0;

	/* Out-of-flow or separately sized children still use the established path. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->replaced ||
		    (child->style.writing_mode != box->style.writing_mode &&
		    child->style.writing_mode != CSS_WRITING_HORIZONTAL_TB) ||
		    child->style.width.unit != CSS_UNIT_AUTO ||
		    child->style.height.unit != CSS_UNIT_AUTO)
			return 0;
		for (side = 0; side < 4; side++) {
			if (child->style.margin[side].unit == CSS_UNIT_AUTO)
				return 0;
		}
	}

	/* Placement remains the common logical grid algorithm. */
	memset(&grid, 0, sizeof(grid));
	error = grid_collect(box, &grid);
	if (error == 0)
		error = grid_place(&grid, box->style.grid_auto_flow);
	if (error != 0) {
		grid_release(&grid);
		return error;
	}
	if (grid.column_count != box->style.column_count ||
	    grid.row_count != box->style.row_count) {
		grid_release(&grid);
		return 0;
	}

	/* This first physical mapping covers one track per item and start/stretch only. */
	for (index = 0; index < grid.item_count; index++) {
		placed = &grid.items[index];
		if (placed->column_span != 1 || placed->row_span != 1) {
			grid_release(&grid);
			return 0;
		}
		child = placed->box;
		align = grid_in_flow_align(child->style.align_self);
		if (align == CSS_ALIGN_AUTO)
			align = box->style.align_items;
		if (align == CSS_ALIGN_NORMAL)
			align = CSS_ALIGN_STRETCH;
		justify = child->style.justify_self;
		if (justify == CSS_ALIGN_AUTO)
			justify = box->style.justify_items;
		if (justify == CSS_ALIGN_NORMAL)
			justify = CSS_ALIGN_STRETCH;
		if ((align != CSS_ALIGN_START && align != CSS_ALIGN_STRETCH) ||
		    (justify != CSS_ALIGN_START && justify != CSS_ALIGN_STRETCH)) {
			grid_release(&grid);
			return 0;
		}
	}

	/* Columns advance downwards; rows advance in the writing mode's block direction. */
	columns[0] = 0;
	for (column = 0; column < grid.column_count; column++)
		columns[column + 1] = columns[column] + layout_from_px(box->style.columns[column].size.value);
	rows[0] = 0;
	for (row = 0; row < grid.row_count; row++)
		rows[row + 1] = rows[row] + layout_from_px(box->style.rows[row].size.value);
	box->width = rows[grid.row_count];
	box->height = columns[grid.column_count];
	box->grid_content = box->width;

	/* Each item receives the physical width of its row and height of its column. */
	for (index = 0; index < grid.item_count; index++) {
		placed = &grid.items[index];
		child = placed->box;
		area_width = rows[placed->row + 1] - rows[placed->row];
		area_height = columns[placed->column + 1] - columns[placed->column];
		align = grid_in_flow_align(child->style.align_self);
		if (align == CSS_ALIGN_AUTO)
			align = box->style.align_items;
		if (align == CSS_ALIGN_NORMAL)
			align = CSS_ALIGN_STRETCH;
		justify = child->style.justify_self;
		if (justify == CSS_ALIGN_AUTO)
			justify = box->style.justify_items;
		if (justify == CSS_ALIGN_NORMAL)
			justify = CSS_ALIGN_STRETCH;
		error = grid_lay_item(tree, child, area_width, align);
		if (error != 0) {
			grid_release(&grid);
			return error;
		}
		error = grid_relay_item(tree, child, area_width, area_height, justify);
		if (error != 0) {
			grid_release(&grid);
			return error;
		}
		if (box->style.writing_mode == CSS_WRITING_VERTICAL_RL) {
			/* The row's block-start edge is on the physical right. */
			child->x = box->width - rows[placed->row] - child->margin[CSS_RIGHT] -
			    child->border[CSS_LEFT] - child->padding[CSS_LEFT] - child->width -
			    child->padding[CSS_RIGHT] - child->border[CSS_RIGHT];
		} else {
			child->x = rows[placed->row] + child->margin[CSS_LEFT];
		}
		child->y = columns[placed->column] + child->margin[CSS_TOP];
	}

	/* Succeeded: the physical Grid path consumed this bounded case. */
	grid_release(&grid);
	*handled = 1;
	return 0;
}

/* Gathers a container's in-flow children as items, with the places their styles give on each axis. */
static int
grid_collect(
	struct layout_box *box,
	struct grid_state *grid)
{
	struct layout_box *child;
	struct grid_item *item;
	size_t count;
	size_t index;

	/* Counts the in-flow children. */
	count = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (!child->out_of_flow)
			count++;
	}

	/* The array (one place at least, so that none is not NULL). */
	grid->items = calloc(count + 1U, sizeof(*grid->items));
	if (grid->items == NULL)
		return ENOMEM;

	/* The columns and rows of the explicit grid: the templates', or the areas' when they have more. */
	grid->column_count = box->style.column_count;
	grid->row_count = box->style.row_count;
	if (box->style.areas != NULL && box->style.areas->columns > grid->column_count)
		grid->column_count = box->style.areas->columns;
	if (box->style.areas != NULL && box->style.areas->rows > grid->row_count)
		grid->row_count = box->style.areas->rows;

	/* Each child, its places resolved against the explicit grid. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow)
			continue;
		item = &grid->items[grid->item_count];
		item->box = child;
		grid_resolve(&child->style.grid_column, grid->column_count, &item->column, &item->column_span);
		grid_resolve(&child->style.grid_row, grid->row_count, &item->row, &item->row_span);
		grid->item_count++;
	}

	/* The items in order-modified document order (a stable sort by order). */
	for (index = 1; index < grid->item_count; index++) {
		struct grid_item moving;
		size_t place;

		moving = grid->items[index];
		place = index;
		while (place > 0 && grid->items[place - 1U].box->style.order > moving.box->style.order) {
			grid->items[place] = grid->items[place - 1U];
			place--;
		}
		grid->items[place] = moving;
	}

	/* Succeeded: the items are gathered. */
	return 0;
}

/*
 * Resolves an item's place on one axis to its first track (-1 for auto)
 * and its span: lines count from 1, negative lines from the end of the
 * explicit tracks; a start and an end line give the span between them.
 */
static void
grid_resolve(
	const struct css_grid_place *place,
	int explicit_count,
	int *start,
	int *span)
{
	int first;
	int last;

	/* The start line's track, or none. */
	first = -1;
	if (place->start > 0)
		first = place->start - 1;
	if (place->start < 0)
		first = explicit_count + 1 + place->start;
	if (place->start < 0 && first < 0)
		first = 0;

	/* The end line's track (the one after the area), or none. */
	last = -1;
	if (place->end > 0)
		last = place->end - 1;
	if (place->end < 0)
		last = explicit_count + 1 + place->end;
	if (place->end < 0 && last < 0)
		last = 0;

	/* The span: between the lines, or the one given, or one. */
	*span = place->start_span;
	if (place->end_span > *span)
		*span = place->end_span;
	if (*span < 1)
		*span = 1;
	if (first >= 0 && last >= 0) {
		if (last < first) {
			*span = first - last;
			first = last;
		} else if (last > first) {
			*span = last - first;
		}
	} else if (first < 0 && last >= 0) {
		/* Only the end is known: the area ends there. */
		first = last - *span;
		if (first < 0)
			first = 0;
	}

	/* The first track, or -1 for auto. */
	*start = first;
}

/* Gives bounded vertical Grid absolute items their physical area and alignment. */
static void
grid_static_vertical_start(
	struct layout_box *box)
{
	struct layout_box *child;
	layout_unit x;
	layout_unit y;
	layout_unit width;
	int column;
	int column_span;
	int row;
	int row_span;
	int align;
	int justify;
	int index;
	int framed;
	int horizontal_child;

	/* Only fixed two-by-two LTR tracks have this definite physical mapping. */
	if (box->style.position != CSS_POSITION_RELATIVE ||
	    box->style.display != CSS_DISPLAY_INLINE_GRID ||
	    box->style.direction != CSS_DIRECTION_LTR ||
	    box->style.column_count != 2 || box->style.row_count != 2 ||
	    box->style.column_gap.value != 0 || box->style.row_gap.value != 0)
		return;
	for (index = 0; index < 2; index++) {
		if (box->style.columns[index].kind != CSS_TRACK_LENGTH ||
		    box->style.columns[index].size.unit != CSS_UNIT_PX ||
		    box->style.rows[index].kind != CSS_TRACK_LENGTH ||
		    box->style.rows[index].size.unit != CSS_UNIT_PX)
			return;
	}
	framed = 0;
	for (index = 0; index < 4; index++) {
		if (box->border[index] != 0 || box->padding[index] != 0)
			framed = 1;
	}
	width = layout_from_px(box->style.rows[0].size.value + box->style.rows[1].size.value);
	if (box->width != width)
		return;

	/* Absolute children keep their sizing and are positioned after Grid layout. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->style.position != CSS_POSITION_ABSOLUTE ||
		    child->style.width.unit != CSS_UNIT_AUTO ||
		    child->style.height.unit != CSS_UNIT_AUTO)
			continue;
		horizontal_child = !framed &&
		    child->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
		    ((child->style.direction == CSS_DIRECTION_LTR &&
		    child->style.grid_row.start == 1 &&
		    child->style.grid_row.end == 2 &&
		    child->style.offset[CSS_TOP].unit == CSS_UNIT_AUTO &&
		    child->style.offset[CSS_BOTTOM].unit == CSS_UNIT_AUTO &&
		    ((child->style.grid_column.start == 1 &&
		    child->style.grid_column.end == 2 &&
		    child->style.offset[CSS_LEFT].unit == CSS_UNIT_PX &&
		    child->style.offset[CSS_RIGHT].unit == CSS_UNIT_AUTO) ||
		    (child->style.grid_column.start == 2 &&
		    child->style.grid_column.end == 3 &&
		    child->style.offset[CSS_LEFT].unit == CSS_UNIT_AUTO &&
		    child->style.offset[CSS_RIGHT].unit == CSS_UNIT_PX))) ||
		    (child->style.direction == CSS_DIRECTION_RTL &&
		    child->style.grid_row.start == 2 &&
		    child->style.grid_row.end == 3 &&
		    child->style.offset[CSS_TOP].unit == CSS_UNIT_PX &&
		    child->style.offset[CSS_RIGHT].unit == CSS_UNIT_AUTO &&
		    child->style.offset[CSS_BOTTOM].unit == CSS_UNIT_AUTO &&
		    ((box->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
		    child->style.grid_column.start == 1 &&
		    child->style.grid_column.end == 2 &&
		    child->style.offset[CSS_LEFT].unit == CSS_UNIT_AUTO) ||
		    (box->style.writing_mode == CSS_WRITING_VERTICAL_LR &&
		    child->style.grid_column.start == 2 &&
		    child->style.grid_column.end == 3 &&
		    child->style.offset[CSS_LEFT].unit == CSS_UNIT_PX))));
		/* These horizontal children have no frame in this bounded mapping. */
		for (index = 0; horizontal_child && index < 4; index++) {
			if (child->border[index] != 0 || child->padding[index] != 0)
				horizontal_child = 0;
		}
		if (child->style.writing_mode != box->style.writing_mode &&
		    !horizontal_child &&
		    !(((box->style.writing_mode == CSS_WRITING_VERTICAL_LR &&
		    child->style.writing_mode == CSS_WRITING_VERTICAL_RL) ||
		    (box->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
		    child->style.writing_mode == CSS_WRITING_VERTICAL_LR)) &&
		    child->style.direction == CSS_DIRECTION_LTR))
			continue;
		/* Explicit insets still use the same definite Grid area. */
		grid_resolve(&child->style.grid_column, 2, &column, &column_span);
		grid_resolve(&child->style.grid_row, 2, &row, &row_span);
		if (column < 0 || column >= 2 || row < 0 || row >= 2 ||
		    column_span != 1 || row_span != 1)
			continue;
		for (index = 0; index < 4; index++) {
			if (child->style.offset[index].unit != CSS_UNIT_AUTO)
				break;
		}
		if (index != 4 && child->style.grid_column.start > 0 &&
		    child->style.grid_column.end > 0 &&
		    child->style.grid_row.start > 0 &&
		    child->style.grid_row.end > 0)
			child->static_grid_area = 1;
		if (framed && index == 4 &&
		    (child->style.writing_mode != box->style.writing_mode ||
		    child->style.direction != CSS_DIRECTION_LTR))
			continue;
		if (framed && index != 4 && !child->static_grid_area)
			continue;
		align = child->style.align_self;
		if (align == CSS_ALIGN_AUTO)
			align = box->style.align_items;
		justify = child->style.justify_self;
		if (justify == CSS_ALIGN_AUTO)
			justify = box->style.justify_items;
		/* Invalid align-self:left/right falls back to normal, the static start edge. */
		if (align == CSS_ALIGN_NORMAL)
			align = CSS_ALIGN_START;
		if (justify == CSS_ALIGN_NORMAL)
			justify = CSS_ALIGN_START;
		if ((align != CSS_ALIGN_START && align != CSS_ALIGN_END &&
		    align != CSS_ALIGN_CENTER && align != CSS_ALIGN_LEFT &&
		    align != CSS_ALIGN_RIGHT && align != CSS_ALIGN_SELF_START &&
		    align != CSS_ALIGN_SELF_END) ||
		    (justify != CSS_ALIGN_START && justify != CSS_ALIGN_END &&
		    justify != CSS_ALIGN_CENTER && justify != CSS_ALIGN_LEFT &&
		    justify != CSS_ALIGN_RIGHT && justify != CSS_ALIGN_SELF_START &&
		    justify != CSS_ALIGN_SELF_END))
			continue;

		/* Logical columns descend; logical rows go right or left. */
		y = column == 0 ? 0 : layout_from_px(box->style.columns[0].size.value);
		x = row == 0 ? 0 : layout_from_px(box->style.rows[0].size.value);
		if (box->style.writing_mode == CSS_WRITING_VERTICAL_RL) {
			child->static_x = box->width - x;
			child->static_x_from_right = 1;
		} else {
			child->static_x = x;
		}
		child->static_y = y;
		if (align == CSS_ALIGN_SELF_START)
			child->static_x_align = child->style.writing_mode ==
			    CSS_WRITING_VERTICAL_RL ? CSS_ALIGN_RIGHT : CSS_ALIGN_LEFT;
		else if (align == CSS_ALIGN_SELF_END)
			child->static_x_align = child->style.writing_mode ==
			    CSS_WRITING_VERTICAL_RL ? CSS_ALIGN_LEFT : CSS_ALIGN_RIGHT;
		else
			child->static_x_align = align;
		child->static_x_area_width = layout_from_px(box->style.rows[row].size.value);
		child->static_x_align_from_right =
		    child->static_x_align == CSS_ALIGN_RIGHT ||
		    (child->static_x_align == CSS_ALIGN_END &&
		    box->style.writing_mode == CSS_WRITING_VERTICAL_LR);
		child->static_y_align =
		    (child->style.direction == CSS_DIRECTION_RTL &&
		    justify == CSS_ALIGN_SELF_START) ? CSS_ALIGN_END :
		    (child->style.direction == CSS_DIRECTION_RTL &&
		    justify == CSS_ALIGN_SELF_END) ? CSS_ALIGN_START :
		    justify == CSS_ALIGN_RIGHT ? CSS_ALIGN_END :
		    justify == CSS_ALIGN_LEFT ? CSS_ALIGN_START : justify;
		child->static_y_area_height =
		    layout_from_px(box->style.columns[column].size.value);
	}
}

/* Gives one direct absolute child the origin of its definite grid area. */
static void
grid_static_child(
	struct layout_box *box,
	struct layout_box *child,
	const struct grid_state *grid,
	layout_unit column_gap,
	layout_unit row_gap,
	layout_unit shift,
	layout_unit row_shift)
{
	layout_unit x;
	layout_unit y;
	int column;
	int column_span;
	int row;
	int row_span;
	int index;
	int justify;
	int align;
	int full_inline;
	int full_block;
	int auto_start_row;
	int auto_end_row;
	int explicit_inset;
	int orthogonal_rtl_inset;
	int orthogonal_frame_auto;
	int orthogonal_y_align;
	int parent_frame;
	int child_frame;
	layout_unit available_height;

	explicit_inset = child->style.offset[CSS_LEFT].unit != CSS_UNIT_AUTO ||
	    child->style.offset[CSS_RIGHT].unit != CSS_UNIT_AUTO ||
	    child->style.offset[CSS_TOP].unit != CSS_UNIT_AUTO ||
	    child->style.offset[CSS_BOTTOM].unit != CSS_UNIT_AUTO;
	if (explicit_inset &&
	    (child->style.grid_column.start <= 0 ||
	    child->style.grid_column.end <= 0 ||
	    child->style.grid_row.start <= 0 ||
	    child->style.grid_row.end <= 0))
		return;

	/* An absolute box never creates a track or enters auto-placement. */
	grid_resolve(&child->style.grid_column, box->style.column_count,
	    &column, &column_span);
	grid_resolve(&child->style.grid_row, box->style.row_count,
	    &row, &row_span);
	available_height = 0;
	if (box->style.align_content == CSS_ALIGN_CENTER &&
	    box->style.height.unit == CSS_UNIT_PX &&
	    box->padding[CSS_TOP] == 0 && box->padding[CSS_BOTTOM] == 0 &&
	    box->border[CSS_TOP] == 0 && box->border[CSS_BOTTOM] == 0)
		available_height = layout_from_px(box->style.height.value);
	full_block = available_height > 0 && child->style.grid_row.start == 0 &&
	    child->style.grid_row.end == 0 &&
	    child->style.grid_row.start_span == 0 &&
	    child->style.grid_row.end_span == 0;
	auto_start_row = available_height > 0 && child->style.grid_row.start == 0 &&
	    child->style.grid_row.end == 1;
	auto_end_row = available_height > 0 && child->style.grid_row.start == 2 &&
	    child->style.grid_row.end == 0;
	full_inline = child->style.grid_column.start == 0 &&
	    child->style.grid_column.end == 0 &&
	    child->style.grid_column.start_span == 0 &&
	    child->style.grid_column.end_span == 0;
	if (full_inline && (box->padding[CSS_LEFT] != 0 ||
	    box->padding[CSS_RIGHT] != 0 || box->border[CSS_LEFT] != 0 ||
	    box->border[CSS_RIGHT] != 0))
		return;
	if (column < 0 && !full_inline)
		return;
	if (row < 0 && !full_block)
		return;
	if (column_span < 1)
		return;
	if (row_span < 1)
		return;
	if (!full_inline && column + column_span > box->style.column_count)
		return;
	if (!full_block && row + row_span > box->style.row_count)
		return;
	justify = child->style.justify_self;
	if (justify == CSS_ALIGN_AUTO)
		justify = box->style.justify_items;
	parent_frame = 0;
	child_frame = 0;
	orthogonal_frame_auto = box->style.display == CSS_DISPLAY_INLINE_GRID &&
	    (box->style.direction == CSS_DIRECTION_LTR ||
	    box->style.direction == CSS_DIRECTION_RTL) &&
	    box->style.column_count == 2 && box->style.row_count == 2 &&
	    box->style.column_gap.value == 0 && box->style.row_gap.value == 0 &&
	    column_span == 1 && row == 0 && row_span == 1 &&
	    child->style.grid_column.start == column + 1 &&
	    child->style.grid_column.end == column + 2 &&
	    child->style.grid_row.start == 1 && child->style.grid_row.end == 2 &&
	    ((column == 0 &&
	    child->style.writing_mode == CSS_WRITING_VERTICAL_LR &&
	    justify == CSS_ALIGN_RIGHT) ||
	    (column == 1 &&
	    child->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
	    justify == CSS_ALIGN_CENTER)) &&
	    child->style.direction == box->style.direction &&
	    (child->style.width.unit == CSS_UNIT_AUTO ||
	    child->style.width.unit == CSS_UNIT_PX) &&
	    child->style.height.unit == CSS_UNIT_AUTO &&
	    !explicit_inset;
	for (index = 0; orthogonal_frame_auto && index < 2; index++) {
		if (box->style.columns[index].kind != CSS_TRACK_LENGTH ||
		    box->style.columns[index].size.unit != CSS_UNIT_PX ||
		    box->style.rows[index].kind != CSS_TRACK_LENGTH ||
		    box->style.rows[index].size.unit != CSS_UNIT_PX)
			orthogonal_frame_auto = 0;
	}
	if (orthogonal_frame_auto) {
		for (index = 0; index < 4; index++) {
			if (box->border[index] != 0 || box->padding[index] != 0)
				parent_frame = 1;
			if ((child->style.border_style[index] != CSS_BORDER_NONE &&
			    child->style.border_width[index] > 0) ||
			    (child->style.padding[index].unit == CSS_UNIT_PX &&
			    child->style.padding[index].value != 0))
				child_frame = 1;
		}
		orthogonal_frame_auto = parent_frame && child_frame;
	}
	orthogonal_rtl_inset = box->style.display == CSS_DISPLAY_INLINE_GRID &&
	    box->style.direction == CSS_DIRECTION_RTL &&
	    box->style.column_count == 2 && box->style.row_count == 2 &&
	    box->style.column_gap.value == 0 && box->style.row_gap.value == 0 &&
	    column_span == 1 && row_span == 1 &&
	    child->style.direction == CSS_DIRECTION_RTL &&
	    child->style.width.unit == CSS_UNIT_AUTO &&
	    child->style.height.unit == CSS_UNIT_AUTO &&
	    child->style.offset[CSS_BOTTOM].unit == CSS_UNIT_AUTO &&
	    ((row == 0 && column == 0 &&
	    child->style.writing_mode == CSS_WRITING_VERTICAL_LR &&
	    child->style.offset[CSS_TOP].unit == CSS_UNIT_AUTO &&
	    child->style.offset[CSS_LEFT].unit == CSS_UNIT_PX &&
	    child->style.offset[CSS_RIGHT].unit == CSS_UNIT_AUTO) ||
	    (row == 0 && column == 1 &&
	    child->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
	    child->style.offset[CSS_TOP].unit == CSS_UNIT_AUTO &&
	    child->style.offset[CSS_RIGHT].unit == CSS_UNIT_PX &&
	    child->style.offset[CSS_LEFT].unit == CSS_UNIT_AUTO) ||
	    (row == 1 && column == 1 &&
	    child->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
	    child->style.offset[CSS_LEFT].unit == CSS_UNIT_PX &&
	    child->style.offset[CSS_TOP].unit == CSS_UNIT_PX &&
	    child->style.offset[CSS_RIGHT].unit == CSS_UNIT_AUTO));
	/* Only definite pixel tracks supply this orthogonal area. */
	for (index = 0; orthogonal_rtl_inset && index < 2; index++) {
		if (box->style.columns[index].kind != CSS_TRACK_LENGTH ||
		    box->style.columns[index].size.unit != CSS_UNIT_PX ||
		    box->style.rows[index].kind != CSS_TRACK_LENGTH ||
		    box->style.rows[index].size.unit != CSS_UNIT_PX)
			orthogonal_rtl_inset = 0;
	}
	/* A framed area stays on its established RTL path. */
	for (index = 0; orthogonal_rtl_inset && index < 4; index++) {
		if (box->border[index] != 0 || box->padding[index] != 0 ||
		    child->border[index] != 0 || child->padding[index] != 0)
			orthogonal_rtl_inset = 0;
	}
	if (box->style.direction == CSS_DIRECTION_RTL &&
	    justify != CSS_ALIGN_START && justify != CSS_ALIGN_END &&
	    justify != CSS_ALIGN_SELF_END && justify != CSS_ALIGN_CENTER &&
	    justify != CSS_ALIGN_LEFT && justify != CSS_ALIGN_RIGHT &&
	    justify != CSS_ALIGN_SELF_START &&
	    !(justify == CSS_ALIGN_NORMAL && orthogonal_rtl_inset))
		return;

	/* Track offsets include the container's content distribution and gaps. */
	x = shift;
	if (full_inline) {
		x = box->style.direction == CSS_DIRECTION_RTL ? box->width : 0;
		child->static_x_from_right = box->style.direction == CSS_DIRECTION_RTL;
	} else if (box->style.direction == CSS_DIRECTION_RTL) {
		/* The right edge includes the current column and every track after it. */
		for (index = column; index < grid->column_count; index++) {
			x += grid->columns[index];
			if (index + 1 < grid->column_count)
				x += column_gap;
		}
		child->static_x_from_right = 1;
	} else {
		for (index = 0; index < column; index++)
			x += grid->columns[index] + column_gap;
	}
	/* An auto start line reaches the padding edge, before content distribution. */
	if (child->style.grid_column.start == 0 &&
	    child->style.grid_column.end == 1)
		x = box->style.direction == CSS_DIRECTION_RTL ? box->width : 0;
	y = child->style.grid_row.start > 0 ? row_shift : 0;
	for (index = 0; !full_block && index < row; index++)
		y += grid->rows[index] + row_gap;
	child->static_x = x;
	child->static_y = y;
	if ((child->style.writing_mode == CSS_WRITING_HORIZONTAL_TB ||
	    orthogonal_frame_auto) &&
	    (justify == CSS_ALIGN_END || justify == CSS_ALIGN_SELF_END ||
	    justify == CSS_ALIGN_CENTER || justify == CSS_ALIGN_LEFT ||
	    justify == CSS_ALIGN_RIGHT || justify == CSS_ALIGN_SELF_START)) {
		child->static_x_align = justify;
		child->static_x_safe = (child->style.justify_self != CSS_ALIGN_AUTO ?
		    (child->style.flex_keywords & 0x1000u) : (box->style.flex_keywords & 0x2000u)) != 0;
		if (justify == CSS_ALIGN_SELF_END)
			child->static_x_align_from_right =
			    child->style.direction == CSS_DIRECTION_LTR;
		else if (justify == CSS_ALIGN_SELF_START)
			child->static_x_align_from_right =
			    child->style.direction == CSS_DIRECTION_RTL;
		else
			child->static_x_align_from_right =
			    justify == CSS_ALIGN_RIGHT ||
			    (justify == CSS_ALIGN_END && box->style.direction == CSS_DIRECTION_LTR);
		if (full_inline) {
			child->static_x_area_width = box->width;
		} else {
			for (index = column; index < column + column_span; index++) {
				child->static_x_area_width += grid->columns[index];
				if (index + 1 < column + column_span)
					child->static_x_area_width += column_gap;
			}
		}
		/* An auto end line reaches the opposite padding edge. */
		if (child->style.grid_column.start == 2 &&
		    child->style.grid_column.end == 0)
			child->static_x_area_width = box->style.direction == CSS_DIRECTION_RTL ?
			    x : box->width - x;
		if (child->static_x_area_width < 0)
			child->static_x_area_width = 0;
	}
	align = grid_in_flow_align(child->style.align_self);
	if (align == CSS_ALIGN_AUTO)
		align = box->style.align_items;
	orthogonal_y_align = box->style.display == CSS_DISPLAY_INLINE_GRID &&
	    (box->style.direction == CSS_DIRECTION_LTR ||
	    box->style.direction == CSS_DIRECTION_RTL) &&
	    box->style.column_count == 2 && box->style.row_count == 2 &&
	    box->style.column_gap.value == 0 && box->style.row_gap.value == 0 &&
	    box->style.columns[0].kind == CSS_TRACK_LENGTH &&
	    box->style.columns[1].kind == CSS_TRACK_LENGTH &&
	    box->style.rows[0].kind == CSS_TRACK_LENGTH &&
	    box->style.rows[1].kind == CSS_TRACK_LENGTH &&
	    box->style.columns[0].size.unit == CSS_UNIT_PX &&
	    box->style.columns[1].size.unit == CSS_UNIT_PX &&
	    box->style.rows[0].size.unit == CSS_UNIT_PX &&
	    box->style.rows[1].size.unit == CSS_UNIT_PX &&
	    column == 1 && column_span == 1 && row == 0 && row_span == 1 &&
	    child->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
	    child->style.direction == box->style.direction &&
	    child->style.width.unit == CSS_UNIT_AUTO &&
	    child->style.height.unit == CSS_UNIT_AUTO &&
	    child->style.offset[CSS_RIGHT].unit == CSS_UNIT_PX &&
	    child->style.offset[CSS_TOP].unit == CSS_UNIT_AUTO &&
	    child->style.offset[CSS_BOTTOM].unit == CSS_UNIT_AUTO;
	/* The orthogonal Y alignment applies to this frameless fixed area only. */
	for (index = 0; orthogonal_y_align && index < 4; index++) {
		if (box->border[index] != 0 || box->padding[index] != 0 ||
		    child->border[index] != 0 || child->padding[index] != 0)
			orthogonal_y_align = 0;
	}
	if ((child->style.writing_mode == CSS_WRITING_HORIZONTAL_TB ||
	    orthogonal_y_align || (orthogonal_frame_auto && column == 1)) &&
	    (align == CSS_ALIGN_END || align == CSS_ALIGN_SELF_END ||
	    align == CSS_ALIGN_CENTER)) {
		child->static_y_align = align;
		child->static_y_safe = (child->style.align_self != CSS_ALIGN_AUTO ?
		    (child->style.flex_keywords & 128u) : (box->style.flex_keywords & 64u)) != 0;
		if (full_block) {
			child->static_y_area_height = available_height;
		} else if (auto_start_row) {
			child->static_y_area_height = row_shift;
		} else if (auto_end_row) {
			child->static_y_area_height = available_height - y;
		} else {
			for (index = row; index < row + row_span; index++) {
				child->static_y_area_height += grid->rows[index];
				if (index + 1 < row + row_span)
					child->static_y_area_height += row_gap;
			}
		}
		if (child->static_y_area_height < 0)
			child->static_y_area_height = 0;
	}
	/* Explicit insets use this definite Grid area as their containing block. */
	if (explicit_inset) {
		child->static_grid_area = 1;
		child->static_x_area_width = 0;
		for (index = column; index < column + column_span; index++) {
			child->static_x_area_width += grid->columns[index];
			if (index + 1 < column + column_span)
				child->static_x_area_width += column_gap;
		}
		child->static_y_area_height = 0;
		for (index = row; index < row + row_span; index++) {
			child->static_y_area_height += grid->rows[index];
			if (index + 1 < row + row_span)
				child->static_y_area_height += row_gap;
		}
	}
}

/* Sets how an absolute child is aligned in its static-position rectangle: justify-self across and align-self down. */
static void
grid_static_alignment(
	struct layout_box *box,
	struct layout_box *child)
{
	int justify;
	int align;
	int rtl;

	rtl = box->style.direction == CSS_DIRECTION_RTL;
	justify = child->style.justify_self;
	if (justify == CSS_ALIGN_AUTO)
		justify = box->style.justify_items;
	align = grid_in_flow_align(child->style.align_self);
	if (align == CSS_ALIGN_AUTO)
		align = box->style.align_items;
	align = grid_in_flow_align(align);
	child->static_x_align = CSS_ALIGN_AUTO;
	child->static_y_align = CSS_ALIGN_AUTO;
	if (justify == CSS_ALIGN_END || justify == CSS_ALIGN_SELF_END ||
	    justify == CSS_ALIGN_CENTER || justify == CSS_ALIGN_LEFT ||
	    justify == CSS_ALIGN_RIGHT || justify == CSS_ALIGN_SELF_START) {
		child->static_x_align = justify;
		child->static_x_safe = (child->style.justify_self != CSS_ALIGN_AUTO ?
		    (child->style.flex_keywords & 0x1000u) : (box->style.flex_keywords & 0x2000u)) != 0;
		if (justify == CSS_ALIGN_SELF_END)
			child->static_x_align_from_right = child->style.direction == CSS_DIRECTION_LTR;
		else if (justify == CSS_ALIGN_SELF_START)
			child->static_x_align_from_right = child->style.direction == CSS_DIRECTION_RTL;
		else
			child->static_x_align_from_right = justify == CSS_ALIGN_RIGHT ||
			    (justify == CSS_ALIGN_END && !rtl);
	}
	if (align == CSS_ALIGN_END || align == CSS_ALIGN_CENTER) {
		child->static_y_align = align;
		child->static_y_safe = (child->style.align_self != CSS_ALIGN_AUTO ?
		    (child->style.flex_keywords & 128u) : (box->style.flex_keywords & 64u)) != 0;
	}
}

/*
 * The static position of an absolute child of a grid container that is not
 * its containing block: the content box is the rectangle, and the child's
 * grid area is not used.
 */
static void
grid_static_content_box(
	struct layout_box *box,
	struct layout_box *child,
	layout_unit content_height)
{
	int rtl;

	rtl = box->style.direction == CSS_DIRECTION_RTL;
	child->static_x = rtl ? box->width : 0;
	child->static_y = 0;
	child->static_x_from_right = rtl;
	child->static_x_area_width = box->width;
	child->static_y_area_height = content_height;
	grid_static_alignment(box, child);
}

/* Gives each direct absolute child the origin of its definite grid area. */
static void
grid_static_position(
	struct layout_box *box,
	const struct grid_state *grid,
	layout_unit column_gap,
	layout_unit row_gap,
	layout_unit shift,
	layout_unit row_shift)
{
	struct layout_box *child;
	layout_unit content_height;
	int own_block;
	int index;

	/* Other writing directions retain their existing static-position path. */
	if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB) {
		if (box->style.position == CSS_POSITION_RELATIVE)
			grid_static_vertical_start(box);
		return;
	}
	if (box->style.direction != CSS_DIRECTION_LTR &&
	    box->style.direction != CSS_DIRECTION_RTL)
		return;

	/* A direct absolute child can use a definite explicit area without becoming a grid item. */
	/* A container that is not a containing block does not use the child's grid area. */
	own_block = box->style.position != CSS_POSITION_STATIC;
	content_height = box->style.height.unit == CSS_UNIT_PX ?
	    layout_from_px(box->style.height.value) : 0;
	if (box->style.height.unit != CSS_UNIT_PX) {
		for (index = 0; index < grid->row_count; index++)
			content_height += grid->rows[index];
		if (grid->row_count > 1)
			content_height += row_gap * (layout_unit)(grid->row_count - 1);
	}
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->style.position != CSS_POSITION_ABSOLUTE)
			continue;
		if (!own_block)
			grid_static_content_box(box, child, content_height);
		else
			grid_static_child(box, child, grid, column_gap, row_gap, shift, row_shift);
	}
}

/*
 * Finds the lines an absolute child's placement names, as 1-based line
 * numbers (0 when the edge is auto).  A line outside the implicit grid
 * is auto, and a span next to a definite line counts from it.
 */
static void
grid_abspos_lines(
	const struct css_grid_place *place,
	int explicit_count,
	int tracks,
	int *first,
	int *last)
{
	int start;
	int end;

	start = 0;
	end = 0;
	if (place->start > 0)
		start = place->start;
	else if (place->start < 0)
		start = explicit_count + 2 + place->start;
	if (place->end > 0)
		end = place->end;
	else if (place->end < 0)
		end = explicit_count + 2 + place->end;
	if (start == 0 && end != 0 && place->start_span > 0)
		start = end - place->start_span;
	if (end == 0 && start != 0 && place->end_span > 0)
		end = start + place->end_span;
	if (start != 0 && end != 0 && end < start) {
		int swap;

		swap = start;
		start = end;
		end = swap;
	}
	if (start < 1 || start > tracks + 1)
		start = 0;
	if (end < 1 || end > tracks + 1)
		end = 0;
	*first = start;
	*last = end;
}

/*
 * Gives each absolute child of a positioned grid the containing block its
 * lines name: the tracks' edges, or the padding edge where a line is auto.
 */
static void
grid_abspos_areas(
	struct layout_box *box,
	const struct grid_state *grid,
	layout_unit column_gap,
	layout_unit row_gap,
	layout_unit shift,
	layout_unit row_shift)
{
	struct layout_box *child;
	layout_unit begin;
	layout_unit left;
	layout_unit right;
	layout_unit top;
	layout_unit bottom;
	layout_unit total;
	int first_column;
	int last_column;
	int first_row;
	int last_row;
	int index;
	int rtl;

	/* The grid is the containing block only when positioned; other flows keep their paths. */
	if (box->style.position == CSS_POSITION_STATIC ||
	    box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB ||
	    (box->style.direction != CSS_DIRECTION_LTR && box->style.direction != CSS_DIRECTION_RTL))
		return;
	rtl = box->style.direction == CSS_DIRECTION_RTL;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->style.position != CSS_POSITION_ABSOLUTE || child->static_grid_area)
			continue;
		if (child->style.writing_mode != CSS_WRITING_HORIZONTAL_TB)
			continue;
		grid_abspos_lines(&child->style.grid_column, box->style.column_count,
		    grid->column_count, &first_column, &last_column);
		grid_abspos_lines(&child->style.grid_row, box->style.row_count,
		    grid->row_count, &first_row, &last_row);
		/* The columns: logical offsets from the start of the track strip. */
		total = column_gap * (layout_unit)(grid->column_count - 1);
		for (index = 0; index < grid->column_count; index++)
			total += grid->columns[index];
		left = 0;
		right = 0;
		begin = 0;
		child->cb_left_auto = 0;
		child->cb_right_auto = 0;
		for (index = 0; index < grid->column_count; index++) {
			if (first_column == index + 1)
				left = begin;
			begin += grid->columns[index];
			if (last_column == index + 2)
				right = begin;
			begin += column_gap;
		}
		if (first_column == grid->column_count + 1)
			left = total;
		if (last_column == 1)
			right = 0;
		if (rtl) {
			/* The strip runs from the right: logical start is the physical right. */
			layout_unit physical_left;
			layout_unit physical_right;
			int left_auto;
			int right_auto;

			left_auto = last_column == 0;
			right_auto = first_column == 0;
			physical_left = shift + total - right;
			physical_right = shift + total - left;
			child->cb_left_auto = left_auto;
			child->cb_right_auto = right_auto;
			child->cb_left = left_auto ? -box->padding[CSS_LEFT] : physical_left;
			child->cb_right = right_auto ? box->width + box->padding[CSS_RIGHT] : physical_right;
		} else {
			child->cb_left_auto = first_column == 0;
			child->cb_right_auto = last_column == 0;
			child->cb_left = child->cb_left_auto ? -box->padding[CSS_LEFT] : shift + left;
			child->cb_right = child->cb_right_auto ? box->width + box->padding[CSS_RIGHT] : shift + right;
		}

		/* The rows. */
		total = row_gap * (layout_unit)(grid->row_count - 1);
		for (index = 0; index < grid->row_count; index++)
			total += grid->rows[index];
		top = 0;
		bottom = 0;
		begin = 0;
		for (index = 0; index < grid->row_count; index++) {
			if (first_row == index + 1)
				top = begin;
			begin += grid->rows[index];
			if (last_row == index + 2)
				bottom = begin;
			begin += row_gap;
		}
		if (first_row == grid->row_count + 1)
			top = total;
		if (last_row == 1)
			bottom = 0;
		child->cb_top_auto = first_row == 0;
		child->cb_bottom_auto = last_row == 0;
		child->cb_top = child->cb_top_auto ? -box->padding[CSS_TOP] : row_shift + top;
		child->cb_bottom = child->cb_bottom_auto ? 0 : row_shift + bottom;
		child->cb_set = 1;

		/* The static position starts at the area's start corner. */
		child->static_x = rtl ? child->cb_right : child->cb_left;
		child->static_x_from_right = rtl;
		child->static_x_area_width = child->cb_right - child->cb_left;
		if (child->static_x_area_width < 0)
			child->static_x_area_width = 0;
		child->static_y = child->cb_top;
		if (!child->cb_bottom_auto) {
			child->static_y_area_height = child->cb_bottom - child->cb_top;
			if (child->static_y_area_height < 0)
				child->static_y_area_height = 0;
		}
		grid_static_alignment(box, child);
	}
}

/*
 * Places every item in the cells: the ones with both lines given first,
 * then the ones with a row, then the rest row after row in the first free
 * cells after the last one placed.  The grid grows to hold them.
 */
static int
grid_place(
	struct grid_state *grid,
	int auto_flow)
{
	struct grid_item *item;
	size_t index;
	int exchanged;
	int error;

	/* Column flow is row flow with the grid's two axes exchanged. */
	if (!(auto_flow & CSS_GRID_FLOW_COLUMN))
		return grid_place_row(grid, auto_flow & CSS_GRID_FLOW_DENSE);
	for (index = 0; index < grid->item_count; index++) {
		item = &grid->items[index];
		exchanged = item->column;
		item->column = item->row;
		item->row = exchanged;
		exchanged = item->column_span;
		item->column_span = item->row_span;
		item->row_span = exchanged;
	}
	exchanged = grid->column_count;
	grid->column_count = grid->row_count;
	grid->row_count = exchanged;

	/* The occupancy map is only needed inside placement, in its transposed axes. */
	error = grid_place_row(grid, auto_flow & CSS_GRID_FLOW_DENSE);
	exchanged = grid->column_count;
	grid->column_count = grid->row_count;
	grid->row_count = exchanged;
	for (index = 0; index < grid->item_count; index++) {
		item = &grid->items[index];
		exchanged = item->column;
		item->column = item->row;
		item->row = exchanged;
		exchanged = item->column_span;
		item->column_span = item->row_span;
		item->row_span = exchanged;
	}
	return error;
}

/* Places items in row order, optionally starting each search at the first cell. */
static int
grid_place_row(
	struct grid_state *grid,
	int dense)
{
	struct grid_item *item;
	size_t index;
	int cursor_row;
	int cursor_column;
	int error;
	int pass;
	int fits;

	/* The columns: the template's, and as many more as the items given a column need. */
	for (index = 0; index < grid->item_count; index++) {
		item = &grid->items[index];
		if (item->column >= 0 && item->column + item->column_span > grid->column_count)
			grid->column_count = item->column + item->column_span;
		if (item->column_span > grid->column_count)
			grid->column_count = item->column_span;
	}

	/* A grid has one column at least. */
	if (grid->column_count < 1)
		grid->column_count = 1;

	/* An auto item's span is at most the columns there are. */
	for (index = 0; index < grid->item_count; index++) {
		item = &grid->items[index];
		if (item->column_span > grid->column_count)
			item->column_span = grid->column_count;
	}

	/* The items with a column and a row, then those with a row only (in the first free columns of their rows). */
	for (pass = 0; pass < 2; pass++) {
		for (index = 0; index < grid->item_count; index++) {
			item = &grid->items[index];
			if (item->row < 0)
				continue;
			if (pass == 0 && item->column < 0)
				continue;
			if (pass == 1 && item->column >= 0)
				continue;

			/* An item with a row only takes the first columns free in it. */
			if (item->column < 0) {
				item->column = 0;
				fits = grid_fits(grid, item->row, item->column, item->row_span, item->column_span);
				while (!fits && item->column + item->column_span < grid->column_count) {
					item->column++;
					fits = grid_fits(grid, item->row, item->column, item->row_span, item->column_span);
				}
			}

			/* The item takes its cells. */
			error = grid_take(grid, item->row, item->column, item->row_span, item->column_span);
			if (error != 0)
				return error;
		}
	}

	/* The rest, row after row from the last one placed. */
	cursor_row = 0;
	cursor_column = 0;
	for (index = 0; index < grid->item_count; index++) {
		item = &grid->items[index];
		if (item->row >= 0)
			continue;
		if (dense) {
			cursor_row = 0;
			cursor_column = 0;
		}

		/* A column given: the first row from the cursor where the area is free. */
		if (item->column >= 0) {
			if (item->column < cursor_column)
				cursor_row++;
			fits = grid_fits(grid, cursor_row, item->column, item->row_span, item->column_span);
			while (!fits && cursor_row < GRID_ROWS_MAX) {
				cursor_row++;
				fits = grid_fits(grid, cursor_row, item->column, item->row_span, item->column_span);
			}

			/* The cursor is at the item's column. */
			cursor_column = item->column;
		} else {
			/* No column: the first free cells from the cursor, row after row. */
			fits = 0;
			while (!fits && cursor_row < GRID_ROWS_MAX) {
				if (cursor_column + item->column_span > grid->column_count) {
					cursor_row++;
					cursor_column = 0;
					continue;
				}

				/* The area at the cursor, or the next column. */
				fits = grid_fits(grid, cursor_row, cursor_column, item->row_span, item->column_span);
				if (!fits)
					cursor_column++;
			}
		}

		/* The item takes its cells, and the cursor moves past them. */
		if (item->column < 0)
			item->column = cursor_column;
		item->row = cursor_row;
		error = grid_take(grid, item->row, item->column, item->row_span, item->column_span);
		if (error != 0)
			return error;
		cursor_column = item->column + item->column_span;
	}

	/* The rows: the template's, and as many more as the items took. */
	if (grid->taken_rows > grid->row_count)
		grid->row_count = grid->taken_rows;

	/* Succeeded: every item has its area. */
	return 0;
}

/* Tells whether an area's cells are all free (rows past the grid's are free). */
static int
grid_fits(
	const struct grid_state *grid,
	int row,
	int column,
	int row_span,
	int column_span)
{
	int y;
	int x;

	/* Each cell of the area inside the rows there are. */
	for (y = row; y < row + row_span && y < grid->taken_rows; y++) {
		for (x = column; x < column + column_span; x++) {
			if (grid->taken[(size_t)y * (size_t)grid->column_count + (size_t)x])
				return 0;
		}
	}

	/* The area is free. */
	return 1;
}

/* Marks an area's cells taken, growing the rows to hold it (an area past the most rows is held in the last). */
static int
grid_take(
	struct grid_state *grid,
	int row,
	int column,
	int row_span,
	int column_span)
{
	unsigned char *taken;
	int rows;
	int y;
	int x;

	/* The area stays inside the most rows. */
	if (row >= GRID_ROWS_MAX)
		row = GRID_ROWS_MAX - 1;
	if (row + row_span > GRID_ROWS_MAX)
		row_span = GRID_ROWS_MAX - row;
	if (column + column_span > grid->column_count)
		column_span = grid->column_count - column;

	/* More rows when the area reaches past them. */
	rows = row + row_span;
	if (rows > grid->taken_rows) {
		taken = realloc(grid->taken, (size_t)rows * (size_t)grid->column_count);
		if (taken == NULL)
			return ENOMEM;
		memset(taken + (size_t)grid->taken_rows * (size_t)grid->column_count, 0,
		    (size_t)(rows - grid->taken_rows) * (size_t)grid->column_count);
		grid->taken = taken;
		grid->taken_rows = rows;
	}

	/* The cells. */
	for (y = row; y < row + row_span; y++) {
		for (x = column; x < column + column_span; x++)
			grid->taken[(size_t)y * (size_t)grid->column_count + (size_t)x] = 1;
	}

	/* Succeeded: the area is taken. */
	return 0;
}

/* Returns a declared column track, cycling implicit sizes after the explicit grid. */
static const struct css_track *
grid_column_track(
	const struct css_style *style,
	int column)
{
	if (column < style->column_count)
		return &style->columns[column];
	return &style->auto_columns[(column - style->column_count) % style->auto_column_count];
}

/* Returns a declared row track, cycling implicit sizes after the explicit grid. */
static const struct css_track *
grid_row_track(
	const struct css_style *style,
	int row)
{
	if (row < style->row_count)
		return &style->rows[row];
	return &style->auto_rows[(row - style->row_count) % style->auto_row_count];
}

/*
 * Sizes the columns: fixed tracks at their lengths, auto tracks at their
 * one-column items' content widths (widened for the items spanning them),
 * then the fr tracks sharing what is left of the container's width (at
 * least their minimum), or the auto tracks sharing it when there is no fr.
 * While a container is measured its fr tracks are sized as auto ones.
 */
static int
grid_size_columns(
	struct layout_tree *tree,
	struct layout_box *box,
	struct grid_state *grid,
	layout_unit gap)
{
	const struct css_track *track;
	struct grid_item *item;
	layout_unit available;
	layout_unit width;
	layout_unit used;
	layout_unit share;
	layout_unit remainder;
	layout_unit spanned;
	float fr_total;
	float fr_sum;
	size_t index;
	int column;
	int autos;
	int last_auto;
	int stretches;
	int intrinsic_minimum;
	int error;

	/* The row of widths. */
	grid->columns = calloc((size_t)grid->column_count, sizeof(*grid->columns));
	if (grid->columns == NULL)
		return ENOMEM;

	/* An empty implicit grid has no track for its auto-column size to occupy. */
	if (grid->item_count == 0 && box->style.column_count == 0)
		return 0;

	/* The fixed tracks, and the least of the fr tracks. */
	fr_total = 0;
	for (column = 0; column < grid->column_count; column++) {
		/* Resolves the current track before excluding collapsed repeat slots. */
		track = grid_column_track(&box->style, column);
		if (grid->column_collapsed != NULL && grid->column_collapsed[column])
			continue;

		/* Fixed tracks resolve their existing absolute lower floor here (a smaller minmax() minimum when it can grow later). */
		if (track->kind == CSS_TRACK_LENGTH) {
			grid->columns[column] = grid_track_length(&track->size, box->width, tree->measuring);
			if (tree->measuring == 0 && grid_track_floor(track, box->width, &width) && width < grid->columns[column])
				grid->columns[column] = width;
		}

		/* Definite flexible tracks start from their lower breadth. */
		if (track->kind == CSS_TRACK_FR && tree->measuring == 0) {
			fr_total += track->fr;
			grid->columns[column] = grid_track_length(&track->minimum, box->width, tree->measuring);
		}
	}

	/* A fixed track with an automatic or intrinsic minimum holds its one-column items' minimum contributions. */
	for (index = 0; index < grid->item_count; index++) {
		item = &grid->items[index];
		if (item->column_span != 1)
			continue;
		track = grid_column_track(&box->style, item->column);
		if (track->kind != CSS_TRACK_LENGTH)
			continue;
		if (track->minimum_rule == CSS_TRACK_MINIMUM_MAX_CONTENT || track->minimum_rule == CSS_TRACK_MINIMUM_MIN_CONTENT) {
			if (track->minimum_rule == CSS_TRACK_MINIMUM_MAX_CONTENT)
				error = grid_item_width(tree, item->box, &width);
			else
				error = grid_item_minimum_width(tree, item->box, &width);
			if (error != 0)
				return error;
			if (width > grid->columns[item->column])
				grid->columns[item->column] = width;
			continue;
		}
		if (track->minimum_rule != CSS_TRACK_MINIMUM_AUTO)
			continue;
		if (grid_column_auto_minimum(item->box, box->width, &width) && width > grid->columns[item->column])
			grid->columns[item->column] = width;
	}

	/* The auto tracks (and the fr ones while measuring): their one-column items' widths. */
	for (index = 0; index < grid->item_count; index++) {
		/* Only one-column items set this first contribution bound. */
		item = &grid->items[index];
		if (item->column_span != 1)
			continue;

		/* Fixed tracks do not collect intrinsic contributions. */
		column = item->column;
		track = grid_column_track(&box->style, column);
		if (track->kind == CSS_TRACK_LENGTH)
			continue;

		/* Definite flexible tracks receive their free-space share later. */
		if (track->kind == CSS_TRACK_FR && tree->measuring == 0)
			continue;

		/* Plain min-content columns use the breakable non-replaced contribution. */
		intrinsic_minimum = 0;
		if (box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
		    track->maximum_rule == CSS_TRACK_MAXIMUM_MIN_CONTENT &&
		    track->minimum_rule == CSS_TRACK_MINIMUM_NONE &&
		    !item->box->replaced)
			intrinsic_minimum = 1;

		/* Other breadths retain their existing maximum-content contribution. */
		if (grid_ratio_width(box, item, grid_fixed_row_gap(box), &width)) {
			/* A ratio and a height the fixed rows resolve give the width. */
		} else if (intrinsic_minimum) {
			error = grid_item_minimum_width(tree, item->box, &width);
			if (error != 0)
				return error;
		} else {
			error = grid_item_width(tree, item->box, &width);
			if (error != 0)
				return error;
		}

		/* Keeps the largest contribution occupying this column. */
		if (width > grid->columns[column])
			grid->columns[column] = width;
	}

	/*
	 * A fit-content() column starts at its items' minimum contributions (a
	 * pixel min-width, else min-content) and may grow with free space to its
	 * content, no wider than its limit.
	 */
	for (column = 0; column < grid->column_count; column++) {
		track = grid_column_track(&box->style, column);
		if (track->kind != CSS_TRACK_AUTO || track->maximum_rule != CSS_TRACK_MAXIMUM_FIT_CONTENT ||
		    (track->size.unit != CSS_UNIT_PX && (track->size.unit != CSS_UNIT_PERCENT || tree->measuring != 0)))
			continue;
		used = grid_track_length(&track->size, box->width, tree->measuring);
		share = 0;
		for (index = 0; index < grid->item_count; index++) {
			item = &grid->items[index];
			if (item->column != column || item->column_span != 1)
				continue;
			if (item->box->style.min_width.unit == CSS_UNIT_PX) {
				width = layout_from_px(item->box->style.min_width.value);
			} else {
				error = grid_item_minimum_width(tree, item->box, &width);
				if (error != 0)
					return error;
			}
			if (width > share)
				share = width;
		}
		if (grid->column_fit == NULL) {
			grid->column_fit = calloc((size_t)grid->column_count, sizeof(*grid->column_fit));
			if (grid->column_fit == NULL)
				return ENOMEM;
		}
		if (used > grid->columns[column])
			used = grid->columns[column];
		grid->column_fit[column] = used > share ? used : share;
		grid->columns[column] = share;
	}

	/* An intrinsic track's fixed minmax() minimum is its floor. */
	for (column = 0; column < grid->column_count; column++) {
		track = grid_column_track(&box->style, column);
		if (track->kind != CSS_TRACK_AUTO || track->minimum_rule != CSS_TRACK_MINIMUM_LENGTH ||
		    (track->minimum.unit != CSS_UNIT_PX && (track->minimum.unit != CSS_UNIT_PERCENT || tree->measuring != 0)))
			continue;
		width = grid_track_length(&track->minimum, box->width, tree->measuring);
		if (width > grid->columns[column])
			grid->columns[column] = width;
	}

	/* An item spanning auto tracks widens them, equally, to hold it. */
	for (index = 0; index < grid->item_count; index++) {
		/* Single-track contributions were already collected above. */
		item = &grid->items[index];
		if (item->column_span < 2)
			continue;

		/* The spanned width and how many auto tracks share any more. */
		spanned = gap * (layout_unit)(item->column_span - 1);
		autos = 0;
		for (column = item->column; column < item->column + item->column_span; column++) {
			/* Accumulates the current capacity of every crossed track. */
			spanned += grid->columns[column];
			track = grid_column_track(&box->style, column);

			/* The existing span distributor includes coarse intrinsic tracks. */
			if (track->kind == CSS_TRACK_AUTO)
				autos++;
		}

		/* Spanning only fixed or fr tracks widens nothing. */
		if (autos == 0)
			continue;

		/* The item's width against it. */
		error = grid_item_width(tree, item->box, &width);
		if (error != 0)
			return error;

		/* Spans that already fit need no extra capacity. */
		if (width <= spanned)
			continue;

		/* Distributes the remaining contribution through the existing span algorithm. */
		share = (width - spanned) / autos;
		for (column = item->column; column < item->column + item->column_span; column++) {
			/* Fixed and flexible crossed tracks retain their previous breadths. */
			track = grid_column_track(&box->style, column);
			if (track->kind == CSS_TRACK_AUTO)
				grid->columns[column] += share;
		}
	}

	/* What is left of the container's width after the tracks and the gaps. */
	available = box->width - gap * (layout_unit)(grid->column_count - 1);
	used = 0;
	for (column = 0; column < grid->column_count; column++) {
		/* Resolves the current track before excluding collapsed repeat slots. */
		track = grid_column_track(&box->style, column);
		if (grid->column_collapsed != NULL && grid->column_collapsed[column]) {
			available += gap;
			continue;
		}

		/* Definite flexible tracks receive their free-space share later. */
		if (track->kind == CSS_TRACK_FR && tree->measuring == 0)
			continue;

		/* Non-flexible columns occupy this part of the available axis. */
		used += grid->columns[column];
	}

	/* fit-content() columns grow toward their content by the free space, in order. */
	for (column = 0; grid->column_fit != NULL && column < grid->column_count; column++) {
		if (grid->column_fit[column] <= grid->columns[column])
			continue;
		width = grid->column_fit[column] - grid->columns[column];
		if (tree->measuring == 0 && width > available - used)
			width = available - used;
		if (width <= 0)
			continue;
		grid->columns[column] += width;
		used += width;
	}

	/* Fixed minmax() tracks grow toward their maxima by the free space before the fr tracks take what is left. */
	if (tree->measuring == 0 && available > used) {
		grid_maximize(&box->style, 1, grid->columns, grid->column_count, grid->column_collapsed, box->width, available - used);
		used = 0;
		for (column = 0; column < grid->column_count; column++) {
			track = grid_column_track(&box->style, column);
			if ((grid->column_collapsed != NULL && grid->column_collapsed[column]) || track->kind == CSS_TRACK_FR)
				continue;
			used += grid->columns[column];
		}
	}

	/* The fr tracks share it by their factors (a sum under one shares only that much of it), each at least its minimum. */
	if (fr_total > 0) {
		/* Retains the original factor sum before normalizing the denominator. */
		fr_sum = fr_total;

		/* Fractional totals below one intentionally leave some space undistributed. */
		if (fr_total < 1.0f)
			fr_total = 1.0f;

		/* Assigns each non-collapsed flexible track its factor-weighted share. */
		for (column = 0; column < grid->column_count; column++) {
			/* Resolves the factor only for an actual flexible track. */
			track = grid_column_track(&box->style, column);
			if (track->kind != CSS_TRACK_FR ||
			    (grid->column_collapsed != NULL && grid->column_collapsed[column]))
				continue;

			/* Flexible growth never reduces the track below its initial minimum. */
			width = (layout_unit)((float)(available - used) * track->fr / fr_total);
			if (width > grid->columns[column])
				grid->columns[column] = width;
		}

		/* A sum of factors below one leaves room for the auto tracks. */
		used = 0;
		for (column = 0; column < grid->column_count; column++) {
			/* Collapsed slots occupy none of the grown axis. */
			if (grid->column_collapsed != NULL && grid->column_collapsed[column])
				continue;

			/* Recounts actual breadths after flexible growth. */
			used += grid->columns[column];
		}

		/* A complete flexible factor sum consumed the distributable space. */
		if (fr_sum >= 1.0f)
			return 0;
	}

	/* Without fr tracks, the auto tracks share the room left (not while the container is measured). */
	stretches = grid_content_stretches(box->style.justify_content);
	if (tree->measuring != 0 ||
	    available <= used ||
	    !stretches)
		return 0;

	/* Counts only automatic maxima eligible for alignment stretch. */
	autos = 0;
	last_auto = -1;
	for (column = 0; column < grid->column_count; column++) {
		/* Intrinsic maximum keywords are not automatic stretch targets. */
		track = grid_column_track(&box->style, column);
		stretches = grid_column_stretches(box, track);
		if (stretches &&
		    !(grid->column_collapsed != NULL && grid->column_collapsed[column])) {
			autos++;
			last_auto = column;
		}
	}

	/* Without auto tracks the room stays empty. */
	if (autos == 0)
		return 0;

	/* Shares free space while reserving the subpixel remainder for the last auto track. */
	share = (available - used) / autos;
	remainder = available - used - share * autos;
	for (column = 0; column < grid->column_count; column++) {
		/* Intrinsic maximum keywords are not automatic stretch targets. */
		track = grid_column_track(&box->style, column);
		stretches = grid_column_stretches(box, track);
		if (stretches &&
		    !(grid->column_collapsed != NULL && grid->column_collapsed[column]))
			grid->columns[column] += share;
	}

	/* Publishes the reserved remainder without stretching an intrinsic maximum. */
	grid->columns[last_auto] += remainder;

	/* Succeeded: the columns are sized. */
	return 0;
}

/* Keeps intrinsic maximum breadths out of the automatic column stretch step. */
static int
grid_column_stretches(
	const struct layout_box *box,
	const struct css_track *track)
{
	/* Only the coarse automatic kind ever participated in this stretch step. */
	if (track->kind != CSS_TRACK_AUTO)
		return 0;

	/* Horizontal intrinsic maxima have their own growth limit instead of auto stretch. */
	if (box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
	    (track->maximum_rule == CSS_TRACK_MAXIMUM_MIN_CONTENT ||
	     track->maximum_rule == CSS_TRACK_MAXIMUM_MAX_CONTENT ||
	     track->maximum_rule == CSS_TRACK_MAXIMUM_FIT_CONTENT))
		return 0;

	/* Succeeded: this column retains the ordinary automatic stretch path. */
	return 1;
}

/* Tells whether a row takes a share of the stretch: an auto maximum, not a content-sized one. */
static int
grid_row_stretches(
	const struct css_track *track)
{
	return track->kind == CSS_TRACK_AUTO &&
	    track->maximum_rule != CSS_TRACK_MAXIMUM_MIN_CONTENT &&
	    track->maximum_rule != CSS_TRACK_MAXIMUM_MAX_CONTENT &&
	    track->maximum_rule != CSS_TRACK_MAXIMUM_FIT_CONTENT;
}

/* Measures a non-replaced single-column item's constrained min-content margin box. */
static int
grid_item_minimum_width(
	struct layout_tree *tree,
	struct layout_box *item,
	layout_unit *width)
{
	layout_unit measured;
	int64_t content;
	int64_t frame;
	int64_t outside;
	int64_t bound;
	int error;

	/* Definite preferred widths contribute their own size before min/max constraints. */
	if (item->style.width.unit == CSS_UNIT_PX) {
		content = layout_from_px(item->style.width.value);
	} else {
		/* Takes every available line break using the existing intrinsic layout path. */
		error = layout_min_content(tree, item, &measured);
		if (error != 0)
			return error;

		/* Keeps the measured content width separate from the box's frame. */
		content = measured;
	}

	/* Restores the ordinary margin/frame calculation after intrinsic child layout. */
	layout_box_model(item, 0);
	frame = (int64_t)item->border[CSS_LEFT] + item->padding[CSS_LEFT] +
		item->padding[CSS_RIGHT] + item->border[CSS_RIGHT];
	outside = frame + item->margin[CSS_LEFT] + item->margin[CSS_RIGHT];

	/* A definite border-box preferred width includes the frame already counted outside. */
	if (item->style.width.unit == CSS_UNIT_PX &&
	    item->style.box_sizing == CSS_BOX_SIZING_BORDER)
		content -= frame;

	/* Definite maximum constraints cap the contribution before a stronger minimum is applied. */
	if (item->style.max_width.unit == CSS_UNIT_PX) {
		bound = layout_from_px(item->style.max_width.value);

		/* Border-box bounds constrain the same content width as the preferred size. */
		if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
			bound -= frame;

		/* Keeps content within the explicit maximum. */
		if (content > bound)
			content = bound;
	}

	/* Definite minima take precedence when the maximum is smaller. */
	if (item->style.min_width.unit == CSS_UNIT_PX) {
		bound = layout_from_px(item->style.min_width.value);

		/* Converts border-box minima into the content coordinate system. */
		if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
			bound -= frame;

		/* Preserves the explicit minimum contribution. */
		if (content < bound)
			content = bound;
	}

	/* A content box never has negative width even when its frame exceeds a bound. */
	if (content < 0)
		content = 0;

	/* Accumulates the margin box widely before publishing the signed layout unit. */
	content += outside;
	if (content < 0)
		content = 0;

	/* Oversized contributions cannot overflow the native track accumulator here. */
	if (content > INT_MAX)
		content = INT_MAX;

	/* Succeeded: the constrained min-content margin box is copied to the caller. */
	*width = (layout_unit)content;
	return 0;
}

/* Measures an item's margin box at its content's width (or at its own width, when it has one in pixels). */
static int
grid_item_width(
	struct layout_tree *tree,
	struct layout_box *item,
	layout_unit *width)
{
	layout_unit content;
	layout_unit outside;
	int error;

	/* Its margins, borders and paddings. */
	layout_box_model(item, 0);
	outside = item->margin[CSS_LEFT] + item->border[CSS_LEFT] + item->padding[CSS_LEFT] +
	    item->padding[CSS_RIGHT] + item->border[CSS_RIGHT] + item->margin[CSS_RIGHT];

	/* A width of its own in pixels. */
	if (item->style.width.unit == CSS_UNIT_PX) {
		content = layout_from_px(item->style.width.value);
		if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
			content -= item->border[CSS_LEFT] + item->padding[CSS_LEFT] + item->padding[CSS_RIGHT] + item->border[CSS_RIGHT];
		*width = content + outside;
		return 0;
	}

	/* Otherwise its content's width without a limit. */
	error = layout_max_content(tree, item, &content);
	if (error != 0)
		return error;
	layout_box_model(item, 0);
	*width = content + outside;
	return 0;
}

/* The row gap when it is a length (zero otherwise). */
static layout_unit
grid_fixed_row_gap(
	const struct layout_box *box)
{
	/* A pixel gap. */
	if (box->style.row_gap.unit == CSS_UNIT_PX)
		return layout_from_px(box->style.row_gap.value);
	return 0;
}

/*
 * Finds the width an item with an aspect-ratio and an auto width
 * contributes when its height is a length, or a percentage of rows that
 * all have fixed sizes: the height through the ratio, with the margins,
 * borders and paddings.
 */
static int
grid_ratio_width(
	const struct layout_box *box,
	const struct grid_item *item,
	layout_unit gap,
	layout_unit *width)
{
	const struct css_track *track;
	struct layout_box *child;
	layout_unit rows;
	layout_unit height;
	layout_unit frame_x;
	layout_unit frame_y;
	int row;

	/* An auto width, a ratio of its own and not a replaced box. */
	child = item->box;
	if (child->style.width.unit != CSS_UNIT_AUTO || child->style.aspect_ratio <= 0.0f || child->replaced)
		return 0;

	/* The height: a length, or a percentage of the fixed rows the item spans. */
	if (child->style.height.unit == CSS_UNIT_PX) {
		height = layout_from_px(child->style.height.value);
	} else if (child->style.height.unit == CSS_UNIT_PERCENT) {
		rows = gap * (layout_unit)(item->row_span - 1);
		for (row = item->row; row < item->row + item->row_span; row++) {
			track = grid_row_track(&box->style, row);
			if (track->kind != CSS_TRACK_LENGTH || track->size.unit != CSS_UNIT_PX)
				return 0;
			rows += layout_from_px(track->size.value);
		}
		height = (layout_unit)((float)rows * child->style.height.value / 100.0f) + layout_from_px(child->style.height.offset);
	} else {
		return 0;
	}

	/* Through the ratio (which relates the border boxes under border-box). */
	layout_box_model(child, 0);
	frame_x = child->border[CSS_LEFT] + child->padding[CSS_LEFT] + child->padding[CSS_RIGHT] + child->border[CSS_RIGHT];
	frame_y = child->border[CSS_TOP] + child->padding[CSS_TOP] + child->padding[CSS_BOTTOM] + child->border[CSS_BOTTOM];
	if (child->style.box_sizing == CSS_BOX_SIZING_BORDER)
		*width = (layout_unit)((float)height * child->style.aspect_ratio);
	else
		*width = (layout_unit)((float)height * child->style.aspect_ratio) + frame_x;
	(void)frame_y;
	*width += child->margin[CSS_LEFT] + child->margin[CSS_RIGHT];

	/* Succeeded: the width is known. */
	return 1;
}

/*
 * Lays an item out as a block in its area's width. An auto-width item
 * stretches by default; non-stretch self-alignment uses its max-content
 * width capped by the area. Its style is put back afterwards.
 */
static int
grid_lay_item(
	struct layout_tree *tree,
	struct layout_box *item,
	layout_unit width,
	int justify)
{
	struct grid_sizes saved;
	layout_unit content;
	layout_unit intrinsic;
	layout_unit minimum;
	layout_unit remaining;
	layout_unit frame;
	layout_unit sizing;
	int left_auto;
	int right_auto;
	int side;
	int error;

	/* An item with a width of its own is laid out in the area as a block is. */
	if (item->style.width.unit != CSS_UNIT_AUTO) {
		error = layout_block(tree, item, width);
		return error;
	}
	if (item->replaced && justify != CSS_ALIGN_STRETCH) {
		error = layout_block(tree, item, width);
		return error;
	}
	intrinsic = 0;
	if (justify != CSS_ALIGN_STRETCH) {
		error = layout_max_content(tree, item, &intrinsic);
		if (error != 0)
			return error;
	}

	/* Keeps the style's sizes and margins, which the layout at the area's width overrides. */
	saved.box_sizing = item->style.box_sizing;
	saved.width = item->style.width;
	saved.min_width = item->style.min_width;
	saved.max_width = item->style.max_width;
	memcpy(saved.margin, item->style.margin, sizeof(saved.margin));
	left_auto = item->style.margin[CSS_LEFT].unit == CSS_UNIT_AUTO;
	right_auto = item->style.margin[CSS_RIGHT].unit == CSS_UNIT_AUTO;

	/* The content width that fills the area: the area less the margins (auto ones are none), borders and paddings. */
	layout_box_model(item, width);
	for (side = CSS_RIGHT; side <= CSS_LEFT; side += 2) {
		if (item->style.margin[side].unit == CSS_UNIT_AUTO) {
			item->style.margin[side].unit = CSS_UNIT_PX;
			item->style.margin[side].value = 0;
			item->style.margin[side].offset = 0;
			item->margin[side] = 0;
		}
	}

	/* What is left of the area for the content. */
	content = width - item->margin[CSS_LEFT] - item->margin[CSS_RIGHT] - item->border[CSS_LEFT] - item->padding[CSS_LEFT] -
	    item->padding[CSS_RIGHT] - item->border[CSS_RIGHT];
	if (content < 0)
		content = 0;

	/* A stretched width stays within the item's pixel maximum and minimum. */
	if (justify == CSS_ALIGN_STRETCH) {
		frame = item->border[CSS_LEFT] + item->padding[CSS_LEFT] + item->padding[CSS_RIGHT] + item->border[CSS_RIGHT];
		sizing = saved.box_sizing == CSS_BOX_SIZING_BORDER ? frame : 0;
		if (saved.max_width.unit == CSS_UNIT_PX && content > layout_from_px(saved.max_width.value) - sizing)
			content = layout_from_px(saved.max_width.value) - sizing;
		if (saved.min_width.unit == CSS_UNIT_PX && content < layout_from_px(saved.min_width.value) - sizing)
			content = layout_from_px(saved.min_width.value) - sizing;
		if (content < 0)
			content = 0;
	}
	if (justify != CSS_ALIGN_STRETCH && content > intrinsic)
		content = intrinsic;
	if (justify != CSS_ALIGN_STRETCH && content < intrinsic) {
		/* A line that cannot break may be wider than its grid area. */
		error = layout_min_content(tree, item, &minimum);
		if (error != 0) {
			item->style.box_sizing = saved.box_sizing;
			item->style.width = saved.width;
			item->style.min_width = saved.min_width;
			item->style.max_width = saved.max_width;
			memcpy(item->style.margin, saved.margin, sizeof(saved.margin));
			return error;
		}
		if (content < minimum)
			content = minimum;
		if (content > intrinsic)
			content = intrinsic;
	}

	/* The layout at that width. */
	item->style.box_sizing = CSS_BOX_SIZING_CONTENT;
	item->style.width.unit = CSS_UNIT_PX;
	item->style.width.value = layout_to_px(content);
	item->style.width.offset = 0;
	item->style.min_width.unit = CSS_UNIT_PX;
	item->style.min_width.value = 0;
	item->style.max_width.unit = CSS_UNIT_NONE;
	error = layout_block(tree, item, width);
	if (error == 0 && (left_auto || right_auto)) {
		/* Auto margins absorb only the area left after the item's content-sized box. */
		remaining = width - item->margin[CSS_LEFT] - item->border[CSS_LEFT] -
		    item->padding[CSS_LEFT] - item->width - item->padding[CSS_RIGHT] -
		    item->border[CSS_RIGHT] - item->margin[CSS_RIGHT];
		if (remaining < 0)
			remaining = 0;
		if (left_auto && right_auto) {
			item->margin[CSS_LEFT] = remaining / 2;
			item->margin[CSS_RIGHT] = remaining - item->margin[CSS_LEFT];
		} else if (left_auto) {
			item->margin[CSS_LEFT] = remaining;
		} else {
			item->margin[CSS_RIGHT] = remaining;
		}
	}

	/* The style is the item's own again. */
	item->style.box_sizing = saved.box_sizing;
	item->style.width = saved.width;
	item->style.min_width = saved.min_width;
	item->style.max_width = saved.max_width;
	memcpy(item->style.margin, saved.margin, sizeof(saved.margin));

	/* Reports an item that could not be laid out. */
	if (error != 0)
		return error;

	/* Succeeded: the item's width follows its self-alignment. */
	return 0;
}

/* Tests whether all crossed maxima remain fixed after supported minimum constraints. */
static int
grid_span_has_fixed_rows(
	const struct css_style *style,
	const struct grid_item *item,
	const struct grid_state *grid,
	layout_unit gap,
	layout_unit available)
{
	const struct css_track *track;
	int64_t capacity;
	int64_t minimum;
	int span;
	int end;
	int row;
	int automatic;
	int known;

	/* The separate vertical sizing paths retain their existing behavior in this partial scope. */
	if (style->writing_mode != CSS_WRITING_HORIZONTAL_TB)
		return 0;

	/* Only placed items with an existing row range can constrain this span. */
	if (item->row < 0 || item->row >= grid->row_count)
		return 0;

	/* Bound the endpoint before adding indices, even for a very large authored span. */
	span = item->row_span;
	if (span <= 0)
		return 0;

	/* No row past the actual grid can participate in sizing this item. */
	if (span > grid->row_count - item->row)
		span = grid->row_count - item->row;

	/* Wide capacity arithmetic preserves every existing track and gap without index overflow. */
	capacity = (int64_t)gap * (span - 1);
	automatic = 0;
	end = item->row + span;
	for (row = item->row; row < end; row++) {
		track = grid_row_track(style, row);
		if (track->kind != CSS_TRACK_LENGTH)
			return 0;

		/* A min-content or max-content floor needs its complete intrinsic sizing algorithm. */
		if (track->minimum_rule == CSS_TRACK_MINIMUM_MIN_CONTENT ||
		    track->minimum_rule == CSS_TRACK_MINIMUM_MAX_CONTENT)
			return 0;

		/* An unresolved percentage lower bound needs its complete area-dependent calculation. */
		if (track->minimum_rule == CSS_TRACK_MINIMUM_LENGTH && track->minimum.unit != CSS_UNIT_PX)
			return 0;

		/* Automatic minima can raise a fixed maximum and require a contribution proof below. */
		if (track->minimum_rule == CSS_TRACK_MINIMUM_AUTO)
			automatic = 1;

		/* A resolved absolute maximum has the capacity already held by the row. */
		if (track->size.unit == CSS_UNIT_PX) {
			capacity += grid->rows[row];
			continue;
		}

		/* A percentage maximum is fixed only with a definite content-height basis. */
		if (track->size.unit == CSS_UNIT_PERCENT && available >= 0) {
			capacity += grid->rows[row];
			continue;
		}

		/* An unresolved maximum cannot establish the fixed-only overflow rule. */
		return 0;
	}

	/* A supported automatic contribution must fit before a fixed maximum can be preserved. */
	if (automatic) {
		known = grid_fixed_span_minimum(item->box, &minimum);
		if (!known)
			return 0;

		/* Existing minimum growth remains responsible for content larger than this capacity. */
		if (minimum > capacity)
			return 0;
	}

	/* Succeeded: excess content can overflow without discarding a known minimum. */
	return 1;
}

/* Bounds the unavoidable outer height for supported automatic-minimum cases. */
static int
grid_fixed_span_minimum(
	const struct layout_box *box,
	int64_t *minimum)
{
	int64_t frame;
	int64_t outside;
	int64_t content;
	layout_unit given;

	/* Ratio-dependent and replaced automatic minima retain their complete existing sizing path. */
	if (box->style.aspect_ratio > 0.0f)
		return 0;

	/* Intrinsic replacement dimensions require their own contribution calculation. */
	if (box->replaced)
		return 0;

	/* Percentage and intrinsic minima depend on the final area and cannot prove this local bound. */
	if (box->style.min_height.unit != CSS_UNIT_AUTO && box->style.min_height.unit != CSS_UNIT_PX)
		return 0;

	/* Borders, padding and margins cannot disappear when the automatic minimum grows. */
	frame = (int64_t)box->border[CSS_TOP] + box->padding[CSS_TOP] +
		box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	outside = frame + box->margin[CSS_TOP] + box->margin[CSS_BOTTOM];

	/* A definite preferred height has already passed its native minimum/maximum constraints. */
	if (box->style.height.unit == CSS_UNIT_PX) {
		content = box->height;
	} else if (box->style.height.unit == CSS_UNIT_AUTO) {
		/* Auto preferred height leaves only its definite minimum and unavoidable frame. */
		content = 0;
		if (box->style.min_height.unit == CSS_UNIT_PX) {
			given = layout_from_px(box->style.min_height.value);
			content = given;

			/* A border-box minimum already includes the frame in its authored length. */
			if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
				content -= frame;
		}
	} else {
		/* Preferred percentages and intrinsic sizes cannot prove this local bound. */
		return 0;
	}

	/* A frame can exceed a border-box minimum without creating negative content. */
	if (content < 0)
		content = 0;

	/* Succeeded: this contribution includes all unavoidable outer-height constraints. */
	*minimum = content + outside;
	return 1;
}

/*
 * Finds the minimum contribution of an item to a column whose minimum is
 * auto: its outer width at its preferred width, or at its minimum width
 * when the preferred one is auto.  An item with neither has a content
 * minimum that a fixed maximum clamps, so it adds nothing (0 is returned).
 * https://drafts.csswg.org/css-grid-2/#min-size-auto
 */
static int
grid_column_auto_minimum(
	struct layout_box *box,
	layout_unit containing_width,
	layout_unit *minimum)
{
	const struct css_length *given;
	layout_unit frame;
	layout_unit content;

	/* The preferred width when it is a length, else the minimum width. */
	given = &box->style.width;
	if (given->unit == CSS_UNIT_AUTO)
		given = &box->style.min_width;
	if (given->unit != CSS_UNIT_PX)
		return 0;

	/* The margins, borders and padding around it. */
	layout_box_model(box, containing_width);
	frame = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	content = layout_from_px(given->value);
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
		content -= frame;
	if (content < 0)
		content = 0;

	/* Succeeded: the outer width. */
	*minimum = content + frame + box->margin[CSS_LEFT] + box->margin[CSS_RIGHT];
	return 1;
}

/*
 * Sizes the rows: a fixed track at its length, the others as tall as
 * their one-row items' margin boxes; an item spanning rows makes the last
 * of them taller when they do not hold it.
 */
static void
grid_size_rows(
	struct layout_box *box,
	struct grid_state *grid,
	layout_unit gap)
{
	const struct css_track *track;
	struct grid_item *item;
	layout_unit available;
	layout_unit height;
	layout_unit minimum;
	layout_unit spanned;
	layout_unit used;
	layout_unit first_above;
	layout_unit first_below;
	layout_unit last_above;
	layout_unit last_below;
	int64_t contribution;
	float fr_total;
	size_t index;
	int row;
	int fixed_rows;

	/* The column of heights (none when the grid has no rows). */
	grid->rows = calloc((size_t)grid->row_count + 1U, sizeof(*grid->rows));
	if (grid->rows == NULL) {
		grid->row_count = 0;
		return;
	}

	/* A pixel height is the definite content height available to the tracks. */
	available = -1;
	if (box->style.height.unit == CSS_UNIT_PX) {
		available = layout_from_px(box->style.height.value);
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER) {
			available -= box->border[CSS_TOP] + box->padding[CSS_TOP] +
			    box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		}

		/* No available content height is negative. */
		if (available < 0)
			available = 0;
	}

	/* The fixed rows, and each fractional row's minimum. */
	fr_total = 0;
	for (row = 0; row < grid->row_count; row++) {
		track = grid_row_track(&box->style, row);
		if (grid->row_collapsed != NULL && grid->row_collapsed[row])
			continue;
		if (track->kind == CSS_TRACK_LENGTH) {
			grid->rows[row] = grid_track_length(&track->size, available, available < 0);
			if (available >= 0 && grid_track_floor(track, available, &minimum) && minimum < grid->rows[row])
				grid->rows[row] = minimum;
		}
		if (track->kind == CSS_TRACK_FR && available >= 0) {
			fr_total += track->fr;
			grid->rows[row] = grid_track_length(&track->minimum, available, 0);
		}
	}

	/* A fixed row with an automatic minimum holds its one-row items' minimum contributions. */
	for (index = 0; index < grid->item_count; index++) {
		item = &grid->items[index];
		if (item->row_span != 1 || item->row >= grid->row_count)
			continue;
		track = grid_row_track(&box->style, item->row);
		if (track->kind != CSS_TRACK_LENGTH || track->minimum_rule != CSS_TRACK_MINIMUM_AUTO)
			continue;
		if (grid_fixed_span_minimum(item->box, &contribution) && contribution > grid->rows[item->row])
			grid->rows[item->row] = (layout_unit)contribution;
	}

	/* The auto rows, and fractional rows without a definite height: their one-row items. */
	for (index = 0; index < grid->item_count; index++) {
		item = &grid->items[index];
		if (item->row_span != 1 || item->row >= grid->row_count)
			continue;
		track = grid_row_track(&box->style, item->row);
		if (track->kind == CSS_TRACK_LENGTH || (track->kind == CSS_TRACK_FR && available >= 0))
			continue;

		/* The item's outer height raises its auto row. */
		height = grid_outer_height(item->box);
		if (height > grid->rows[item->row])
			grid->rows[item->row] = height;
	}

	/* An intrinsic row holds its baseline groups, each member shifted to its group's baseline. */
	for (row = 0; row < grid->row_count; row++) {
		track = grid_row_track(&box->style, row);
		if (track->kind == CSS_TRACK_LENGTH || (track->kind == CSS_TRACK_FR && available >= 0))
			continue;
		grid_row_baselines(box, grid, row, &first_above, &first_below, &last_above, &last_below);
		if (first_above + first_below > grid->rows[row])
			grid->rows[row] = first_above + first_below;
		if (last_above + last_below > grid->rows[row])
			grid->rows[row] = last_above + last_below;
	}

	/* The items spanning rows. */
	for (index = 0; index < grid->item_count; index++) {
		item = &grid->items[index];
		if (item->row_span < 2)
			continue;

		/* Definite fixed tracks retain their maxima even when a spanning item's content is taller. */
		fixed_rows = grid_span_has_fixed_rows(&box->style, item, grid, gap, available);
		if (fixed_rows)
			continue;

		/* Sum occupied rows and internal gaps to find this span's current capacity. */
		spanned = gap * (layout_unit)(item->row_span - 1);
		for (row = item->row; row < item->row + item->row_span && row < grid->row_count; row++)
			spanned += grid->rows[row];
		height = grid_outer_height(item->box);
		row = item->row + item->row_span - 1;
		if (height > spanned && row < grid->row_count)
			grid->rows[row] += height - spanned;
	}

	/* Fixed minmax() rows grow toward their maxima by the definite free space. */
	if (available >= 0) {
		used = gap * (layout_unit)(grid->row_count - 1);
		for (row = 0; row < grid->row_count; row++) {
			track = grid_row_track(&box->style, row);
			if (grid->row_collapsed != NULL && grid->row_collapsed[row]) {
				used -= gap;
				continue;
			}
			if (track->kind != CSS_TRACK_FR)
				used += grid->rows[row];
		}
		if (available > used)
			grid_maximize(&box->style, 0, grid->rows, grid->row_count, grid->row_collapsed, available, available - used);
	}

	/* Fractional rows share the definite room left after fixed and auto rows and the gaps. */
	if (fr_total > 0) {
		used = gap * (layout_unit)(grid->row_count - 1);
		for (row = 0; row < grid->row_count; row++) {
			track = grid_row_track(&box->style, row);
			if (grid->row_collapsed != NULL && grid->row_collapsed[row]) {
				used -= gap;
				continue;
			}
			if (track->kind == CSS_TRACK_FR)
				continue;
			used += grid->rows[row];
		}

		/* A sum below one leaves the corresponding share unused. */
		if (fr_total < 1.0f)
			fr_total = 1.0f;
		for (row = 0; row < grid->row_count; row++) {
			track = grid_row_track(&box->style, row);
			if (track->kind != CSS_TRACK_FR ||
			    (grid->row_collapsed != NULL && grid->row_collapsed[row]))
				continue;
			minimum = (layout_unit)((float)(available - used) * track->fr / fr_total);
			if (minimum > grid->rows[row])
				grid->rows[row] = minimum;
		}
	}

	/* A definite height gives its free space equally to the auto rows. */
	if (available < 0 || !grid_content_stretches(box->style.align_content))
		return;
	used = gap * (layout_unit)(grid->row_count - 1);
	height = 0;
	for (row = 0; row < grid->row_count; row++) {
		track = grid_row_track(&box->style, row);
		if (grid->row_collapsed != NULL && grid->row_collapsed[row]) {
			used -= gap;
			continue;
		}
		used += grid->rows[row];
		if (grid_row_stretches(track))
			height++;
	}
	if (height == 0 || available <= used)
		return;
	minimum = (available - used) / height;
	spanned = available - used - minimum * height;
	for (row = 0; row < grid->row_count; row++) {
		track = grid_row_track(&box->style, row);
		if (!grid_row_stretches(track) ||
		    (grid->row_collapsed != NULL && grid->row_collapsed[row]))
			continue;
		grid->rows[row] += minimum;
		height--;
		if (height == 0)
			grid->rows[row] += spanned;
	}
}

/*
 * Lays a grid item out again at the area's definite cross size when it is
 * stretched, or when its percentage height can use the area.  Placement
 * used to change only item->height, leaving its percentage descendants at
 * the intrinsic height from the first pass.
 */
static int
grid_relay_item(
	struct layout_tree *tree,
	struct layout_box *item,
	layout_unit width,
	layout_unit height,
	int align)
{
	struct grid_sizes saved;
	layout_unit frame;
	layout_unit content;
	layout_unit ratio_width;
	layout_unit ratio_height;
	int justify;
	int error;

	/* The target content height, from the percentage or the area's stretch. */
	frame = item->border[CSS_TOP] + item->padding[CSS_TOP] +
	    item->padding[CSS_BOTTOM] + item->border[CSS_BOTTOM];
	if (item->style.height.unit == CSS_UNIT_PERCENT) {
		content = (layout_unit)((float)height * item->style.height.value / 100.0f) +
		    layout_from_px(item->style.height.offset);
		if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
			content -= frame;
	} else if (item->style.height.unit == CSS_UNIT_AUTO && align == CSS_ALIGN_STRETCH) {
		content = height - item->margin[CSS_TOP] - item->margin[CSS_BOTTOM] - frame;
	} else {
		return 0;
	}

	/* No content size is negative. */
	if (content < 0)
		content = 0;

	/* The definite height keeps within the item's own maximum and minimum heights. */
	content = grid_clamp_height(item, content, height, frame);

	/* Keep the item's style while the definite content box is laid out. */
	saved.box_sizing = item->style.box_sizing;
	saved.width = item->style.width;
	saved.min_width = item->style.min_width;
	saved.max_width = item->style.max_width;
	saved.height = item->style.height;
	saved.min_height = item->style.min_height;
	saved.max_height = item->style.max_height;
	memcpy(saved.margin, item->style.margin, sizeof(saved.margin));
	item->style.box_sizing = CSS_BOX_SIZING_CONTENT;
	item->style.width.unit = CSS_UNIT_PX;
	item->style.width.value = layout_to_px(item->width);
	ratio_width = item->natural_width;
	ratio_height = item->natural_height;
	if (item->image != NULL) {
		ratio_width = (layout_unit)item->image->width;
		ratio_height = (layout_unit)item->image->height;
	}
	justify = item->style.justify_self;
	if (justify == CSS_ALIGN_AUTO && item->parent != NULL)
		justify = item->parent->style.justify_items;
	if (justify == CSS_ALIGN_NORMAL || justify == CSS_ALIGN_AUTO)
		justify = item->replaced ? CSS_ALIGN_START : CSS_ALIGN_STRETCH;
	if (item->replaced && saved.width.unit == CSS_UNIT_AUTO && ratio_width > 0 &&
	    ratio_height > 0 && align == CSS_ALIGN_STRETCH && justify != CSS_ALIGN_STRETCH &&
	    item->style.height.unit == CSS_UNIT_AUTO) {
		/* A stretched replaced item keeps its aspect ratio in the other axis. */
		item->style.width.value = layout_to_px(
		    (layout_unit)((float)content * (float)ratio_width / (float)ratio_height));
	}
	item->style.width.offset = 0;
	item->style.min_width.unit = CSS_UNIT_PX;
	item->style.min_width.value = 0;
	item->style.max_width.unit = CSS_UNIT_NONE;
	item->style.height.unit = CSS_UNIT_PX;
	item->style.height.value = layout_to_px(content);
	item->style.height.offset = 0;
	item->style.min_height.unit = CSS_UNIT_PX;
	item->style.min_height.value = 0;
	item->style.max_height.unit = CSS_UNIT_NONE;
	item->style.margin[CSS_LEFT].unit = CSS_UNIT_PX;
	item->style.margin[CSS_LEFT].value = layout_to_px(item->margin[CSS_LEFT]);
	item->style.margin[CSS_LEFT].offset = 0;
	item->style.margin[CSS_RIGHT].unit = CSS_UNIT_PX;
	item->style.margin[CSS_RIGHT].value = layout_to_px(item->margin[CSS_RIGHT]);
	item->style.margin[CSS_RIGHT].offset = 0;
	error = layout_block(tree, item, width);
	item->style.box_sizing = saved.box_sizing;
	item->style.width = saved.width;
	item->style.min_width = saved.min_width;
	item->style.max_width = saved.max_width;
	item->style.height = saved.height;
	item->style.min_height = saved.min_height;
	item->style.max_height = saved.max_height;
	memcpy(item->style.margin, saved.margin, sizeof(saved.margin));
	return error;
}

/* Keeps a definite content height within the item's max-height and min-height (lengths and percentages of the area). */
static layout_unit
grid_clamp_height(
	const struct layout_box *item,
	layout_unit content,
	layout_unit area,
	layout_unit frame)
{
	layout_unit limit;

	/* The maximum, a length or a percentage of the area. */
	if (item->style.max_height.unit == CSS_UNIT_PX || item->style.max_height.unit == CSS_UNIT_PERCENT) {
		if (item->style.max_height.unit == CSS_UNIT_PX)
			limit = layout_from_px(item->style.max_height.value);
		else
			limit = (layout_unit)((float)area * item->style.max_height.value / 100.0f);
		limit += layout_from_px(item->style.max_height.offset);
		if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
			limit -= frame;
		if (limit < 0)
			limit = 0;
		if (content > limit)
			content = limit;
	}

	/* The minimum. */
	if (item->style.min_height.unit == CSS_UNIT_PX || item->style.min_height.unit == CSS_UNIT_PERCENT) {
		if (item->style.min_height.unit == CSS_UNIT_PX)
			limit = layout_from_px(item->style.min_height.value);
		else
			limit = (layout_unit)((float)area * item->style.min_height.value / 100.0f);
		limit += layout_from_px(item->style.min_height.offset);
		if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
			limit -= frame;
		if (limit > content)
			content = limit;
	}
	return content;
}

/* Measures an item's margin box's height. */
static layout_unit
grid_outer_height(
	const struct layout_box *item)
{
	/* The content, paddings, borders and margins. */
	return item->margin[CSS_TOP] + item->border[CSS_TOP] + item->padding[CSS_TOP] + item->height +
	    item->padding[CSS_BOTTOM] + item->border[CSS_BOTTOM] + item->margin[CSS_BOTTOM];
}

/*
 * Tells whether a Grid item of one row takes part in its row's first (1) or
 * last (2) baseline alignment (align-self baseline or last baseline, without
 * an auto margin across), and how far below its margin box's top that
 * baseline is (its border box's bottom when it has none).
 */
static int
grid_row_baseline(
	const struct layout_box *box,
	const struct grid_item *item,
	layout_unit *above)
{
	const struct layout_box *child;
	layout_unit baseline;
	int align;
	int last;
	int found;

	child = item->box;
	/* The current group shares a horizontal block axis (a vertical grid's, in its logical layout). */
	if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB)
		return 0;

	/* One row, without an auto margin across. */
	if (item->row_span != 1)
		return 0;
	if (child->style.margin[CSS_TOP].unit == CSS_UNIT_AUTO ||
	    child->style.margin[CSS_BOTTOM].unit == CSS_UNIT_AUTO)
		return 0;

	/* A percentage height in an intrinsic row would depend on the row its alignment sizes. */
	if ((child->style.height.unit == CSS_UNIT_PERCENT || child->style.min_height.unit == CSS_UNIT_PERCENT ||
	    child->style.max_height.unit == CSS_UNIT_PERCENT) &&
	    grid_row_track(&box->style, item->row)->kind != CSS_TRACK_LENGTH)
		return 0;

	/* Only members with a baseline preference join the row's group. */
	align = grid_in_flow_align(child->style.align_self);
	last = (child->style.last_baselines & 8u) != 0;
	if (align == CSS_ALIGN_AUTO) {
		align = box->style.align_items;
		last = (box->style.last_baselines & 4u) != 0;
	}
	if (align != CSS_ALIGN_BASELINE && !last)
		return 0;

	/* Its first (or last) line's baseline, or its lower border edge (always, for an orthogonal item). */
	found = 0;
	if (child->style.writing_mode == CSS_WRITING_HORIZONTAL_TB)
		found = last ? layout_last_baseline(child, &baseline) : layout_first_baseline(child, &baseline);
	if (!found)
		baseline = grid_outer_height(child) - child->margin[CSS_TOP] - child->margin[CSS_BOTTOM];
	*above = child->margin[CSS_TOP] + baseline;

	/* Succeeded: the item's group. */
	return last ? 2 : 1;
}

/* Finds a row's baseline groups: how far their baselines lie below the row's top (first) and above its bottom (last). */
static void
grid_row_baselines(
	const struct layout_box *box,
	const struct grid_state *grid,
	int row,
	layout_unit *first_above,
	layout_unit *first_below,
	layout_unit *last_above,
	layout_unit *last_below)
{
	layout_unit above;
	layout_unit outer;
	size_t index;
	int group;

	/* Each member of the row. */
	*first_above = *first_below = *last_above = *last_below = 0;
	for (index = 0; index < grid->item_count; index++) {
		if (grid->items[index].row != row)
			continue;
		group = grid_row_baseline(box, &grid->items[index], &above);
		if (group == 0)
			continue;
		outer = grid_outer_height(grid->items[index].box);
		if (group == 1) {
			if (above > *first_above)
				*first_above = above;
			if (outer - above > *first_below)
				*first_below = outer - above;
		} else {
			if (above > *last_above)
				*last_above = above;
			if (outer - above > *last_below)
				*last_below = outer - above;
		}
	}
}

/* Finds an empty orthogonal item's extent right of its border-left baseline. */
static int
grid_fixed_column_baseline(
	const struct layout_box *box,
	const struct grid_item *item,
	layout_unit *right_extent)
{
	const struct layout_box *child;
	int justify;

	child = item->box;
	if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB ||
	    child->style.writing_mode != CSS_WRITING_VERTICAL_RL)
		return 0;
	if (box->style.column_count != 1 || item->column != 0 || item->column_span != 1 ||
	    box->style.columns[0].kind != CSS_TRACK_LENGTH ||
	    box->style.columns[0].size.unit != CSS_UNIT_PX)
		return 0;
	if (child->first_child != NULL || child->line_count != 0 ||
	    child->style.width.unit != CSS_UNIT_PX ||
	    child->style.margin[CSS_LEFT].unit == CSS_UNIT_AUTO ||
	    child->style.margin[CSS_RIGHT].unit == CSS_UNIT_AUTO)
		return 0;

	justify = child->style.justify_self;
	if (justify == CSS_ALIGN_AUTO)
		justify = box->style.justify_items;
	if (justify != CSS_ALIGN_BASELINE)
		return 0;

	*right_extent = child->border[CSS_LEFT] + child->padding[CSS_LEFT] +
	    child->width + child->padding[CSS_RIGHT] + child->border[CSS_RIGHT] +
	    child->margin[CSS_RIGHT];
	return 1;
}

/* Finds the height of an item's rows when every one is a fixed pixel track (gaps included). */
static int
grid_fixed_area_height(
	const struct layout_box *box,
	const struct grid_item *item,
	layout_unit gap,
	layout_unit *height)
{
	const struct css_track *track;
	int row;

	/* Each row the item spans. */
	*height = gap * (layout_unit)(item->row_span - 1);
	for (row = item->row; row < item->row + item->row_span; row++) {
		track = grid_row_track(&box->style, row);
		if (track->kind != CSS_TRACK_LENGTH || track->size.unit != CSS_UNIT_PX ||
		    track->minimum_rule != CSS_TRACK_MINIMUM_NONE)
			return 0;
		*height += layout_from_px(track->size.value);
	}

	/* Succeeded: the area's height is definite. */
	return 1;
}

/* Finds a fixed track's minmax() minimum when it is a length of its own (its floor while it grows). */
static int
grid_track_floor(
	const struct css_track *track,
	layout_unit whole,
	layout_unit *floor)
{
	/* Only a fixed maximum with a fixed minimum. */
	if (track->kind != CSS_TRACK_LENGTH || track->minimum_rule != CSS_TRACK_MINIMUM_LENGTH)
		return 0;
	if (track->minimum.unit != CSS_UNIT_PX && track->minimum.unit != CSS_UNIT_PERCENT)
		return 0;

	/* Succeeded: the minimum. */
	*floor = grid_track_length(&track->minimum, whole, 0);
	return 1;
}

/*
 * Grows the fixed minmax() tracks of an axis from their minima toward their
 * maxima, equally, by at most the free space (the maximize step).
 */
static void
grid_maximize(
	const struct css_style *style,
	int columns,
	layout_unit *sizes,
	int count,
	const unsigned char *collapsed,
	layout_unit whole,
	layout_unit free_space)
{
	const struct css_track *track;
	layout_unit floor;
	layout_unit limit;
	layout_unit share;
	layout_unit grow;
	int growing;
	int index;

	/* Rounds of equal shares until the space or the room to grow runs out. */
	while (free_space > 0) {
		growing = 0;
		for (index = 0; index < count; index++) {
			track = columns ? grid_column_track(style, index) : grid_row_track(style, index);
			if ((collapsed != NULL && collapsed[index]) || !grid_track_floor(track, whole, &floor))
				continue;
			if (sizes[index] < grid_track_length(&track->size, whole, 0))
				growing++;
		}
		if (growing == 0)
			return;
		share = free_space / growing;
		if (share == 0)
			share = free_space;
		for (index = 0; index < count && free_space > 0; index++) {
			track = columns ? grid_column_track(style, index) : grid_row_track(style, index);
			if ((collapsed != NULL && collapsed[index]) || !grid_track_floor(track, whole, &floor))
				continue;
			limit = grid_track_length(&track->size, whole, 0);
			if (sizes[index] >= limit)
				continue;
			grow = limit - sizes[index];
			if (grow > share)
				grow = share;
			if (grow > free_space)
				grow = free_space;
			sizes[index] += grow;
			free_space -= grow;
		}
	}
}

/* Resolves a track's nonnegative length: pixels or a percentage of the definite whole. */
static layout_unit
grid_track_length(
	const struct css_length *length,
	layout_unit whole,
	int measuring)
{
	layout_unit resolved;

	/* Pixels. */
	if (length->unit == CSS_UNIT_PX) {
		if (length->value < 0)
			return 0;
		return layout_from_px(length->value);
	}

	/* A percentage of a definite whole. */
	if (length->unit == CSS_UNIT_PERCENT && measuring == 0) {
		resolved = (layout_unit)((float)whole * length->value / 100.0f) + layout_from_px(length->offset);
		if (resolved < 0)
			return 0;
		return resolved;
	}

	/* Nothing definite. */
	return 0;
}

/* Frees what a layout's state holds. */
static void
grid_release(
	struct grid_state *grid)
{
	/* The arrays. */
	free(grid->items);
	free(grid->columns);
	free(grid->rows);
	free(grid->taken);
	free(grid->column_collapsed);
	free(grid->row_collapsed);
	free(grid->column_fit);
}
