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
#include "layout/grid-track.h"

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
	/* The height the rows gave before percentage rows were resolved against it (-1 when the rows' own sum is the height). */
	layout_unit intrinsic_height;
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

static int grid_fixed_rows_height(const struct layout_box *box, const struct grid_item *item, layout_unit gap, layout_unit *height);
static int grid_normal_starts(const struct layout_box *box);
static int grid_fixed_area_height(const struct layout_box *box, const struct grid_item *item, layout_unit gap, layout_unit *height);
static int grid_save_used_tracks(struct layout_tree *tree, struct layout_box *box, const struct grid_state *grid);
static int grid_copy_used_axis(struct layout_tree *tree, const layout_unit *sizes, int count, double **copy);
static int grid_collect(struct layout_box *box, struct grid_state *grid);
static int grid_vertical_fixed(struct layout_tree *tree, struct layout_box *box, int *handled);
static int grid_vertical_auto(struct layout_tree *tree, struct layout_box *box, int *handled);
static void grid_resolve(const struct css_grid_place *place, int explicit_count, int *start, int *span);
static void grid_static_position(struct layout_box *box, const struct grid_state *grid,
    layout_unit column_gap, layout_unit row_gap, layout_unit shift, layout_unit row_shift);
static void grid_static_vertical_start(struct layout_box *box);
static void grid_abspos_one(struct layout_box *box, struct layout_box *child, const struct grid_state *grid, layout_unit column_gap, layout_unit row_gap, layout_unit shift, layout_unit row_shift, layout_unit off_x, layout_unit off_y, int direct);
static void grid_abspos_deeper(struct layout_box *box, struct layout_box *walk, const struct grid_state *grid, layout_unit column_gap, layout_unit row_gap, layout_unit shift, layout_unit row_shift, layout_unit off_x, layout_unit off_y, int depth);
static void grid_abspos_areas(struct layout_box *box, const struct grid_state *grid,
    layout_unit column_gap, layout_unit row_gap, layout_unit shift, layout_unit row_shift);
static int grid_place(struct grid_state *grid, int auto_flow);
static int grid_place_row(struct grid_state *grid, int dense);
static int grid_fits(const struct grid_state *grid, int row, int column, int row_span, int column_span);
static int grid_take(struct grid_state *grid, int row, int column, int row_span, int column_span);
static const struct css_track *grid_column_track(const struct css_style *style, int column);
static const struct css_track *grid_row_track(const struct css_style *style, int row);
static int grid_size_columns(struct layout_tree *tree, struct layout_box *box, struct grid_state *grid, layout_unit gap);
static int grid_lay_item(struct layout_tree *tree, struct layout_box *item, layout_unit width, int justify);
static void grid_size_rows(struct layout_tree *tree, struct layout_box *box, struct grid_state *grid, layout_unit gap);
static int grid_subgrid_axis(const struct layout_box *box, int columns);
static int grid_subgrid_prepare(struct layout_tree *tree, struct layout_box *sub, const struct layout_box *parent, int column_first, int row_first);
static int grid_subgrid_preset(struct layout_tree *tree, struct layout_box *sub, int columns, const layout_unit *sizes, int first, int tracks, layout_unit gap);
static int grid_relay_item(struct layout_tree *tree, struct layout_box *item, layout_unit width, layout_unit height, int align);
static layout_unit grid_clamp_height(const struct layout_box *item, layout_unit content, layout_unit area, layout_unit frame);
static layout_unit grid_clamp_height(const struct layout_box *item, layout_unit content, layout_unit area, layout_unit frame);
static layout_unit grid_outer_height(const struct layout_box *item);
static int grid_row_baseline(const struct layout_box *box, const struct grid_item *item, layout_unit *above);
static void grid_row_baselines(const struct layout_box *box, const struct grid_state *grid, int row, layout_unit *first_above, layout_unit *first_below, layout_unit *last_above, layout_unit *last_below);
static int grid_fixed_column_baseline(const struct layout_box *box, const struct grid_item *item, layout_unit *right_extent);
static int grid_parent_height(const struct layout_tree *tree, const struct layout_box *box, layout_unit *basis);
static int grid_column_baseline(const struct layout_box *box, const struct grid_item *item, layout_unit *before, layout_unit *after, int *flush_right);
static void grid_column_baselines(const struct layout_box *box, const struct grid_state *grid, int column, layout_unit *left_before, layout_unit *left_after, layout_unit *right_before, layout_unit *right_after);
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

	layout_unit size;
	layout_unit least;

	length = &track->size;
	if (track->kind != CSS_TRACK_LENGTH)
		length = &track->minimum;
	size = 0;
	if (length->unit == CSS_UNIT_PX)
		size = layout_from_px(length->value);
	else if (length->unit == CSS_UNIT_PERCENT && basis >= 0)
		size = (layout_unit)((float)basis * length->value / 100.0f) + layout_from_px(length->offset);

	/* A maximum below the minimum counts as the minimum. */
	if (track->kind == CSS_TRACK_LENGTH && track->minimum_rule != 0) {
		least = 0;
		if (track->minimum.unit == CSS_UNIT_PX)
			least = layout_from_px(track->minimum.value);
		else if (track->minimum.unit == CSS_UNIT_PERCENT && basis >= 0)
			least = (layout_unit)((float)basis * track->minimum.value / 100.0f) + layout_from_px(track->minimum.offset);
		if (least > size)
			size = least;
	}
	return size;
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
	layout_unit gap,
	int at_least)
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

		/* Sized by a minimum alone, the room has to be reached: the next repeat that covers it. */
		if (at_least && denominator > 0 && numerator > 0 && numerator > denominator * (layout_unit)times)
			times++;
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
	layout_unit basis;
	layout_unit minimum;
	layout_unit maximum;
	int definite;
	int definite_width;
	int at_least;

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
	definite_width = available >= 0;
	if (available < 0 && box->style.max_width.unit == CSS_UNIT_PX)
		available = layout_from_px(box->style.max_width.value);
	if (available < 0 && box->style.max_width.unit == CSS_UNIT_PERCENT && box->parent != NULL)
		available = (layout_unit)((float)box->parent->width * box->style.max_width.value / 100.0f);

	/* A minimum larger than the maximum (or alone) is the size the repeats fill. */
	minimum = -1;
	if (box->style.min_width.unit == CSS_UNIT_PX)
		minimum = layout_from_px(box->style.min_width.value);
	if (box->style.min_width.unit == CSS_UNIT_PERCENT && box->parent != NULL)
		minimum = (layout_unit)((float)box->parent->width * box->style.min_width.value / 100.0f);

	/* An aspect-ratio carries a pixel minimum height over to a minimum width. */
	if (!definite_width && box->style.aspect_ratio > 0.0f && box->style.height.unit == CSS_UNIT_AUTO &&
	    box->style.min_height.unit == CSS_UNIT_PX) {
		layout_unit transferred;

		transferred = (layout_unit)((float)layout_from_px(box->style.min_height.value) * box->style.aspect_ratio);
		if (transferred > minimum)
			minimum = transferred;
	}
	at_least = 0;
	if (!definite_width && minimum > available) {
		/* A maximum, even a smaller one, keeps the repeats from overflowing: only a minimum alone has to be reached. */
		at_least = available < 0 && !box->max_width_hidden;
		available = minimum;
	}
	grid_expand_axis(box->style.columns, &box->style.column_count, &box->style.column_repeat,
	    available, gap, at_least);

	/* The rows need a definite height. */
	gap = 0;
	available = -1;
	at_least = 0;
	if (box->style.row_gap.unit == CSS_UNIT_PX)
		gap = layout_from_px(box->style.row_gap.value);
	frame = 0;
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
		frame = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	/* The maximum and the minimum height, in pixels or a percentage of a height that is known (-1: none). */
	maximum = -1;
	if (box->style.max_height.unit == CSS_UNIT_PX)
		maximum = layout_from_px(box->style.max_height.value);
	else if (box->style.max_height.unit == CSS_UNIT_PERCENT && grid_parent_height(tree, box, &basis))
		maximum = (layout_unit)((float)basis * box->style.max_height.value / 100.0f);
	minimum = -1;
	if (box->style.min_height.unit == CSS_UNIT_PX)
		minimum = layout_from_px(box->style.min_height.value);
	else if (box->style.min_height.unit == CSS_UNIT_PERCENT && grid_parent_height(tree, box, &basis))
		minimum = (layout_unit)((float)basis * box->style.min_height.value / 100.0f);

	/* A pixel height is held within them; without one the maximum, or the minimum when it is larger or alone, is the room. */
	if (box->style.height.unit == CSS_UNIT_PX) {
		available = layout_from_px(box->style.height.value);
		if (maximum >= 0 && available > maximum)
			available = maximum;
		if (minimum >= 0 && available < minimum)
			available = minimum;
		available -= frame;
		if (available < 0)
			available = 0;
	} else {
		available = maximum;
		at_least = 0;
		if (minimum > available) {
			at_least = available < 0;
			available = minimum;
		}
	}
	grid_expand_axis(box->style.rows, &box->style.row_count, &box->style.row_repeat,
	    available, gap, at_least);
}

/* Finds the height a percentage height of a grid is of: its parent's, when that is a length (the tree's when the parent's is not one). */
static int
grid_parent_height(
	const struct layout_tree *tree,
	const struct layout_box *box,
	layout_unit *basis)
{
	const struct layout_box *parent;
	layout_unit frame;

	/* A parent with a height in pixels. */
	parent = box->parent;
	if (parent != NULL && parent->style.height.unit == CSS_UNIT_PX) {
		frame = 0;
		if (parent->style.box_sizing == CSS_BOX_SIZING_BORDER)
			frame = parent->border[CSS_TOP] + parent->padding[CSS_TOP] + parent->padding[CSS_BOTTOM] + parent->border[CSS_BOTTOM];
		*basis = layout_from_px(parent->style.height.value) - frame;
		if (*basis < 0)
			*basis = 0;
		return 1;
	}

	/* Otherwise what the tree knows. */
	if (tree->containing_height_definite) {
		*basis = tree->containing_height;
		return 1;
	}
	return 0;
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
	layout_unit col_before;
	layout_unit col_after;
	layout_unit left_before;
	layout_unit left_after;
	layout_unit right_before;
	layout_unit right_after;
	int col_right;
	layout_unit shared_right_extent;
	struct css_length subgrid_width;
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
		child->area_from_anchor = 0;
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

	/* An item that is a subgrid takes the tracks it spans from this grid: its own items are placed against them. */
	for (index = 0; index < grid.item_count; index++) {
		child = grid.items[index].box;
		child->subgrid_column_span = 0;
		child->subgrid_row_span = 0;
		child->subgrid_columns = NULL;
		child->subgrid_rows = NULL;
		if (grid_subgrid_axis(child, 1))
			child->subgrid_column_span = grid.items[index].column_span;
		if (grid_subgrid_axis(child, 0))
			child->subgrid_row_span = grid.items[index].row_span;
		if (child->subgrid_column_span > 0 || child->subgrid_row_span > 0) {
			error = grid_subgrid_prepare(tree, child, box, grid.items[index].column, grid.items[index].row);
			if (error != 0) {
				grid_release(&grid);
				return error;
			}
		}
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
	} else if (box->style.row_gap.unit == CSS_UNIT_PERCENT) {
		/* An indefinite height sizes the rows with the fixed part of the gap alone. */
		row_gap = layout_from_px(box->style.row_gap.offset);
	}

	/* An item with a vertical writing mode lays its lines along the row's height. */
	for (index = 0; index < grid.item_count; index++) {
		item = &grid.items[index];
		child = item->box;
		child->item_row = item->row + 1;
		child->item_column = item->column + 1;
		child->item_row_span = item->row_span;
		child->item_column_span = item->column_span;
		child->grid_orthogonal = 0;
		child->grid_room = -1;
		if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB ||
		    child->style.writing_mode == CSS_WRITING_HORIZONTAL_TB ||
		    child->replaced || child->atomic)
			continue;
		child->grid_orthogonal = 1;
		if (item->row_span == 1 &&
		    grid_row_track(&box->style, item->row)->kind == CSS_TRACK_LENGTH &&
		    grid_row_track(&box->style, item->row)->size.unit == CSS_UNIT_PX)
			child->grid_room = layout_from_px(grid_row_track(&box->style, item->row)->size.value);
		else if (item->row_span == 1 && grid.row_count == 1 && box->style.height.unit == CSS_UNIT_PX &&
		    grid_row_track(&box->style, item->row)->kind == CSS_TRACK_AUTO) {
			/* The only row, left to stretch in a grid with a height of its own, is as tall as that. */
			child->grid_room = layout_from_px(box->style.height.value);
			if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
				child->grid_room -= box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] +
				    box->border[CSS_BOTTOM];
			if (child->grid_room < 0)
				child->grid_room = 0;
		}
	}

	/* A subgrid lays its tracks with its own gap (the parent's when it gave none) once the parent has given the tracks. */
	if (box->subgrid_columns != NULL)
		column_gap = box->subgrid_column_gap;
	if (box->subgrid_rows != NULL)
		row_gap = box->subgrid_row_gap;
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
			justify = grid_normal_starts(item->box) ? CSS_ALIGN_START : CSS_ALIGN_STRETCH;
		if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB ||
		    (item->box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB &&
		    !item->box->grid_orthogonal) ||
		    item->box->style.min_width.unit != CSS_UNIT_AUTO ||
		    item->box->style.max_width.unit != CSS_UNIT_NONE)
			justify = CSS_ALIGN_STRETCH;
		if (item->box->style.margin[CSS_LEFT].unit == CSS_UNIT_AUTO ||
		    item->box->style.margin[CSS_RIGHT].unit == CSS_UNIT_AUTO)
			justify = CSS_ALIGN_START;

		/* A subgrid in columns is given the columns it spans. */
		if (item->box->subgrid_column_span > 0) {
			error = grid_subgrid_preset(tree, item->box, 1, grid.columns, item->column, grid.column_count, column_gap);
			if (error != 0) {
				grid_release(&grid);
				return error;
			}
		}

		/* Percentage heights are of the grid area: its fixed rows' height, or indefinite while the rows are sized. */
		saved_height = tree->containing_height;
		saved_definite = tree->containing_height_definite;
		if (box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB) {
			tree->containing_height_definite = grid_fixed_area_height(box, item, row_gap, &tree->containing_height);
			if (!tree->containing_height_definite && item->box->grid_orthogonal && item->box->grid_room >= 0) {
				/* An orthogonal item's lines run along the row, which a grid with a height of its own stretches to. */
				tree->containing_height = item->box->grid_room;
				tree->containing_height_definite = 1;
			}
			if (!tree->containing_height_definite)
				tree->containing_height = 0;
		}

		/* A subgrid in columns fills the area it spans whatever its width says. */
		subgrid_width = item->box->style.width;
		if (item->box->subgrid_column_span > 0) {
			item->box->style.width.unit = CSS_UNIT_AUTO;
			justify = CSS_ALIGN_STRETCH;
		}
		error = grid_lay_item(tree, item->box, area_width, justify);
		item->box->style.width = subgrid_width;
		tree->containing_height = saved_height;
		tree->containing_height_definite = saved_definite;
		if (error != 0) {
			grid_release(&grid);
			return error;
		}
	}

	/* The rows' heights from the items. */
	grid_size_rows(tree, box, &grid, row_gap);
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
		used = row_gap * (layout_unit)(grid.row_count - 1);
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
			align = grid_normal_starts(child) ? CSS_ALIGN_START : CSS_ALIGN_STRETCH;
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

		/* A subgrid in rows is given the rows it spans. */
		if (child->subgrid_row_span > 0) {
			error = grid_subgrid_preset(tree, child, 0, grid.rows, item->row, grid.row_count, row_gap);
			if (error != 0) {
				grid_release(&grid);
				return error;
			}
		}

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
				justify = grid_normal_starts(child) ? CSS_ALIGN_START : CSS_ALIGN_STRETCH;
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
		/* Vertical items of a column that align by baseline share it: the left group flush left, the right group flush right. */
		if (grid_column_baseline(box, item, &col_before, &col_after, &col_right)) {
			grid_column_baselines(box, &grid, item->column, &left_before, &left_after, &right_before, &right_after);
			if (col_right)
				child->x = x + area_width - right_after - col_before + child->margin[CSS_LEFT];
			else
				child->x = x + left_before - col_before + child->margin[CSS_LEFT];
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
			grid_row_baselines(box, &grid, baseline_group == 1 ? item->row : item->row + item->row_span - 1,
			    &first_above, &first_below, &last_above, &last_below);
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
	if (grid.intrinsic_height >= 0)
		box->height = grid.intrinsic_height;

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
	int raw_align;
	int orthogonal;
	int rtl;

	rtl = box->style.direction == CSS_DIRECTION_RTL;
	justify = child->style.justify_self;
	if (justify == CSS_ALIGN_AUTO)
		justify = box->style.justify_items;
	align = grid_in_flow_align(child->style.align_self);
	if (align == CSS_ALIGN_AUTO)
		align = box->style.align_items;
	align = grid_in_flow_align(align);

	/* An item whose writing mode is orthogonal has its inline axis along the column axis: self-start and self-end follow its direction there. */
	orthogonal = child->style.writing_mode != CSS_WRITING_HORIZONTAL_TB &&
	    box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB;
	raw_align = child->style.align_self;
	if (raw_align == CSS_ALIGN_AUTO)
		raw_align = box->style.align_items;
	if (orthogonal && raw_align == CSS_ALIGN_SELF_END)
		align = child->style.direction == CSS_DIRECTION_RTL ? CSS_ALIGN_START : CSS_ALIGN_END;
	else if (orthogonal && raw_align == CSS_ALIGN_SELF_START)
		align = child->style.direction == CSS_DIRECTION_RTL ? CSS_ALIGN_END : CSS_ALIGN_START;
	child->static_x_align = CSS_ALIGN_AUTO;
	child->static_y_align = CSS_ALIGN_AUTO;
	if (justify == CSS_ALIGN_END || justify == CSS_ALIGN_SELF_END ||
	    justify == CSS_ALIGN_CENTER || justify == CSS_ALIGN_LEFT ||
	    justify == CSS_ALIGN_RIGHT || justify == CSS_ALIGN_SELF_START) {
		child->static_x_align = justify;
		child->static_x_safe = (child->style.justify_self != CSS_ALIGN_AUTO ?
		    (child->style.flex_keywords & 0x1000u) : (box->style.flex_keywords & 0x2000u)) != 0;
		if (justify == CSS_ALIGN_SELF_END && orthogonal)
			child->static_x_align_from_right = child->style.writing_mode == CSS_WRITING_VERTICAL_LR;
		else if (justify == CSS_ALIGN_SELF_START && orthogonal)
			child->static_x_align_from_right = child->style.writing_mode == CSS_WRITING_VERTICAL_RL;
		else if (justify == CSS_ALIGN_SELF_END)
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

	/* The grid is the containing block only when positioned; other flows keep their paths. */
	if (box->style.position == CSS_POSITION_STATIC ||
	    box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB ||
	    (box->style.direction != CSS_DIRECTION_LTR && box->style.direction != CSS_DIRECTION_RTL))
		return;
	for (child = box->first_child; child != NULL; child = child->next)
		grid_abspos_one(box, child, grid, column_gap, row_gap, shift, row_shift, 0, 0, 1);

	/* The boxes out of the flow deeper in the grid items that the grid contains are placed in their areas too. */
	grid_abspos_deeper(box, box, grid, column_gap, row_gap, shift, row_shift, 0, 0, 0);
}

/*
 * Finds the boxes out of the flow below the grid's items whose containing block is the grid (the grid-placement
 * properties name their area), with the offset of their parent's content box from the grid's.
 */
static void
grid_abspos_deeper(
	struct layout_box *box,
	struct layout_box *walk,
	const struct grid_state *grid,
	layout_unit column_gap,
	layout_unit row_gap,
	layout_unit shift,
	layout_unit row_shift,
	layout_unit off_x,
	layout_unit off_y,
	int depth)
{
	struct layout_box *child;

	if (depth > 32)
		return;
	for (child = walk->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow) {
			if (walk != box && child->style.position == CSS_POSITION_ABSOLUTE && layout_containing_box(child) == box)
				grid_abspos_one(box, child, grid, column_gap, row_gap, shift, row_shift, off_x, off_y, 0);
			continue;
		}
		if (child->kind != LAYOUT_BLOCK && child->kind != LAYOUT_ANONYMOUS_BLOCK)
			continue;

		/* A grid inside lays its own items out; what its lines name (a subgrid's) is not the outer grid's. */
		if (child->style.display == CSS_DISPLAY_GRID || child->style.display == CSS_DISPLAY_INLINE_GRID ||
		    child->style.column_subgrid != NULL || child->style.row_subgrid != NULL)
			continue;
		grid_abspos_deeper(box, child, grid, column_gap, row_gap, shift, row_shift,
		    off_x + child->x + child->border[CSS_LEFT] + child->padding[CSS_LEFT] + (walk == box ? 0 : 0),
		    off_y + child->y + child->border[CSS_TOP] + child->padding[CSS_TOP], depth + 1);
	}
}

/*
 * Gives a box out of the flow the grid area its grid-placement properties name as its containing block (and, for a
 * child of the grid, the static position at the area's start corner).  off_x and off_y are the offsets of its parent's
 * content box from the grid's, which the cb of a deeper box is relative to.
 */
static void
grid_abspos_one(
	struct layout_box *box,
	struct layout_box *child,
	const struct grid_state *grid,
	layout_unit column_gap,
	layout_unit row_gap,
	layout_unit shift,
	layout_unit row_shift,
	layout_unit off_x,
	layout_unit off_y,
	int direct)
{
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

	rtl = box->style.direction == CSS_DIRECTION_RTL;
	{
		if (child->style.position != CSS_POSITION_ABSOLUTE || child->static_grid_area)
			return;
		if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB)
			return;
		grid_abspos_lines(&child->style.grid_column, box->style.column_count,
		    grid->column_count, &first_column, &last_column);
		grid_abspos_lines(&child->style.grid_row, box->style.row_count,
		    grid->row_count, &first_row, &last_row);

		/* A deeper box that names no line keeps the grid's padding box, and one that names lines by name leaves them to a subgrid. */
		if (!direct && first_column == 0 && last_column == 0 && first_row == 0 && last_row == 0)
			return;
		if (!direct && (child->style.grid_column.start_name != NULL || child->style.grid_column.end_name != NULL ||
		    child->style.grid_row.start_name != NULL || child->style.grid_row.end_name != NULL))
			return;

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
			child->cb_left = (left_auto ? -box->padding[CSS_LEFT] : physical_left) - off_x;
			child->cb_right = (right_auto ? box->width + box->padding[CSS_RIGHT] : physical_right) - off_x;
		} else {
			child->cb_left_auto = first_column == 0;
			child->cb_right_auto = last_column == 0;
			child->cb_left = (child->cb_left_auto ? -box->padding[CSS_LEFT] : shift + left) - off_x;
			child->cb_right = (child->cb_right_auto ? box->width + box->padding[CSS_RIGHT] : shift + right) - off_x;
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
		child->cb_top = (child->cb_top_auto ? -box->padding[CSS_TOP] : row_shift + top) - off_y;
		child->cb_bottom = (child->cb_bottom_auto ? 0 : row_shift + bottom) - off_y;
		child->cb_set = 1;
		child->area_from_anchor = 0;

		if (!direct)
			return;

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
 * One entry of an axis' sizing: a box (or a stand-in of a fixed size) with
 * the tracks it covers and what its edges add; the items of a subgrid
 * stand in the parent's list at the parent's track numbers.
 */
struct grid_flat {
	struct layout_box *box;
	int start;
	int span;
	int row;
	int row_span;
	layout_unit extra;
	/* At least zero: a stand-in of this size, without a box. */
	layout_unit fixed_size;
};

/* The entries of one axis' sizing and how many tracks the axis has. */
struct grid_flat_list {
	struct grid_flat *entries;
	size_t count;
	size_t capacity;
	int tracks;
};

/* Tells whether a grid item takes its tracks in an axis from the grid it is in. */
static int
grid_subgrid_axis(
	const struct layout_box *box,
	int columns)
{
	/* Only an in-flow grid container. */
	if (box->out_of_flow || box->replaced)
		return 0;
	if (box->style.display != CSS_DISPLAY_GRID && box->style.display != CSS_DISPLAY_INLINE_GRID)
		return 0;

	/* A box with layout or paint containment is an independent formatting context, which is no subgrid. */
	if ((layout_containment(box) & 6U) != 0)
		return 0;

	/* A subgrid whose writing mode is orthogonal to its parent's keeps tracks of its own. */
	if (box->parent != NULL &&
	    (box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB) !=
	    (box->parent->style.writing_mode == CSS_WRITING_HORIZONTAL_TB))
		return 0;

	/* The axis declared as a subgrid. */
	if (columns)
		return box->style.column_subgrid != NULL;
	return box->style.row_subgrid != NULL;
}

/* Finds how much margin, border and padding a box has on the start (or end) side of an axis. */
static layout_unit
grid_edge(
	const struct layout_box *box,
	int columns,
	int start)
{
	int side;

	/* The side: the left or top for the start, the right or bottom for the end (a right to left grid starts at the right). */
	if (columns) {
		side = start ? CSS_LEFT : CSS_RIGHT;
		if (box->style.direction == CSS_DIRECTION_RTL)
			side = start ? CSS_RIGHT : CSS_LEFT;
	} else {
		side = start ? CSS_TOP : CSS_BOTTOM;
	}
	return box->margin[side] + box->border[side] + box->padding[side];
}

/* Finds the gap a box lays its tracks of an axis with: its own when it gave a length, else the one of the grid it is in. */
static layout_unit
grid_gap_of(
	const struct layout_box *box,
	int columns,
	layout_unit parent_gap)
{
	const struct css_length *gap;

	/* A gap in pixels. */
	gap = columns ? &box->style.column_gap : &box->style.row_gap;
	if (gap->unit == CSS_UNIT_PX)
		return layout_from_px(gap->value);
	return parent_gap;
}

/* Adds an entry to a list (the tracks it covers are held within the axis). */
static int
grid_flat_add(
	struct grid_flat_list *list,
	struct layout_box *box,
	int start,
	int span,
	int row,
	int row_span,
	layout_unit extra,
	layout_unit fixed_size)
{
	struct grid_flat *grown;
	size_t capacity;

	/* A place outside the axis counts for nothing. */
	if (start < 0 || start >= list->tracks)
		return 0;
	if (span < 1)
		span = 1;
	if (start + span > list->tracks)
		span = list->tracks - start;

	/* Room for one more. */
	if (list->count == list->capacity) {
		capacity = list->capacity == 0 ? 16U : list->capacity * 2U;
		grown = realloc(list->entries, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		list->entries = grown;
		list->capacity = capacity;
	}

	/* The entry. */
	list->entries[list->count].box = box;
	list->entries[list->count].start = start;
	list->entries[list->count].span = span;
	list->entries[list->count].row = row;
	list->entries[list->count].row_span = row_span;
	list->entries[list->count].extra = extra;
	list->entries[list->count].fixed_size = fixed_size;
	list->count++;
	return 0;
}

/*
 * Adds the items of a subgrid to a parent's list, at the parent's track
 * numbers: what the subgrid's edges (margin, border, padding) add goes to
 * the items on its first and last tracks, and half of what its gap differs
 * from the parent's to the items on each interior side.  A track at an edge
 * with no item of the subgrid on it gets a stand-in for the edge alone.
 */
static int
grid_flatten(
	struct grid_flat_list *list,
	struct layout_box *sub,
	int columns,
	int first,
	layout_unit parent_gap,
	layout_unit extra_start,
	layout_unit extra_end,
	layout_unit inherited)
{
	struct layout_box *child;
	layout_unit edge_start;
	layout_unit edge_end;
	layout_unit gap;
	layout_unit half;
	layout_unit start_extra;
	layout_unit end_extra;
	int count;
	int start;
	int span;
	int touched_start;
	int touched_end;
	int error;

	/* The subgrid's tracks, its edges and its gap. */
	count = columns ? sub->subgrid_column_span : sub->subgrid_row_span;
	layout_box_model(sub, 0);
	edge_start = grid_edge(sub, columns, 1) + extra_start;
	edge_end = grid_edge(sub, columns, 0) + extra_end;
	gap = grid_gap_of(sub, columns, parent_gap);
	half = inherited + (gap - parent_gap) / 2;

	/* Each item of the subgrid. */
	touched_start = 0;
	touched_end = 0;
	for (child = sub->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow)
			continue;
		start = (columns ? child->item_column : child->item_row) - 1;
		span = columns ? child->item_column_span : child->item_row_span;
		if (start < 0 || start >= count)
			continue;
		if (span < 1)
			span = 1;
		if (start + span > count)
			span = count - start;

		/* What each side adds: the edge on the subgrid's first and last tracks, else half the gap's difference. */
		start_extra = half;
		end_extra = half;
		if (start == 0) {
			start_extra = edge_start;
			touched_start = 1;
		}
		if (start + span == count) {
			end_extra = edge_end;
			touched_end = 1;
		}

		/* A subgrid inside passes its sides on; any other item stands in the list. */
		if ((columns && child->subgrid_column_span > 0) || (!columns && child->subgrid_row_span > 0)) {
			error = grid_flatten(list, child, columns, first + start, gap, start_extra, end_extra, half);
		} else {
			error = grid_flat_add(list, child, first + start, span, child->item_row - 1,
			    child->item_row_span, start_extra + end_extra, -1);
		}
		if (error != 0)
			return error;
	}

	/* An edge no item reaches still takes room. */
	if (count == 1 && !touched_start && !touched_end)
		return grid_flat_add(list, NULL, first, 1, 0, 1, 0, edge_start + edge_end);
	if (!touched_start) {
		error = grid_flat_add(list, NULL, first, 1, 0, 1, 0, edge_start);
		if (error != 0)
			return error;
	}
	if (!touched_end)
		return grid_flat_add(list, NULL, first + count - 1, 1, 0, 1, 0, edge_end);
	return 0;
}

/* Makes the list of an axis' entries: the grid's items, and the items of its subgrids in place of them. */
static int
grid_flat_build(
	struct grid_flat_list *list,
	const struct grid_state *grid,
	int columns,
	layout_unit gap)
{
	const struct grid_item *item;
	size_t index;
	int error;

	/* Each item of the grid. */
	list->entries = NULL;
	list->count = 0;
	list->capacity = 0;
	list->tracks = columns ? grid->column_count : grid->row_count;
	for (index = 0; index < grid->item_count; index++) {
		item = &grid->items[index];
		if (columns && item->box->subgrid_column_span > 0) {
			error = grid_flatten(list, item->box, 1, item->column, gap, 0, 0, 0);
		} else if (!columns && item->box->subgrid_row_span > 0) {
			error = grid_flatten(list, item->box, 0, item->row, gap, 0, 0, 0);
		} else if (columns) {
			error = grid_flat_add(list, item->box, item->column, item->column_span, item->row, item->row_span, 0, -1);
		} else {
			error = grid_flat_add(list, item->box, item->row, item->row_span, item->column, item->column_span, 0, -1);
		}
		if (error != 0)
			return error;
	}
	return 0;
}

/* Adds a line's names (atoms) to another's, once each and no more than a line keeps. */
static void
grid_names_merge(
	struct css_line_names *line,
	const struct css_line_names *names)
{
	int index;
	int have;

	/* Each name not there yet. */
	for (index = 0; index < names->count; index++) {
		for (have = 0; have < line->count; have++) {
			if (line->names[have] == names->names[index])
				break;
		}
		if (have == line->count && line->count < CSS_LINE_NAMES) {
			line->names[line->count] = names->names[index];
			line->count++;
		}
	}
}

/*
 * Gives a subgrid's axis the names of its lines: the list it declares
 * (laid out over the lines it spans, a repeat(auto-fill) taking what the
 * others leave) and the names of the parent's lines that are the same
 * lines.
 */
static int
grid_subgrid_line_names(
	struct layout_tree *tree,
	struct layout_box *sub,
	int columns,
	const struct layout_box *parent,
	int first)
{
	const struct css_subgrid_names *declared;
	const struct css_line_names *outer;
	struct css_line_names *lines;
	int line;
	int count;

	/* The array of the lines it spans plus the last. */
	count = columns ? sub->subgrid_column_span : sub->subgrid_row_span;
	if (count > CSS_TRACKS)
		count = CSS_TRACKS;
	count++;
	lines = wb_arena_zalloc(&tree->arena, ((size_t)CSS_TRACKS + 1U) * sizeof(*lines));
	if (lines == NULL)
		return ENOMEM;

	/* The declared names over the lines. */
	declared = columns ? sub->style.column_subgrid_names : sub->style.row_subgrid_names;
	css_subgrid_lines(declared, count, lines);

	/* The parent's names of the same lines. */
	outer = columns ? parent->style.column_names : parent->style.row_names;
	for (line = 0; outer != NULL && line < count && first + line <= CSS_TRACKS; line++)
		grid_names_merge(&lines[line], &outer[first + line]);

	/* Kept for the placement of its items. */
	if (columns)
		sub->style.column_names = lines;
	else
		sub->style.row_names = lines;
	return 0;
}

/*
 * Places the items of a subgrid against the tracks it spans (its explicit
 * grid is those tracks), recording where each went and which of them are
 * subgrids in turn.
 */
static int
grid_subgrid_prepare(
	struct layout_tree *tree,
	struct layout_box *sub,
	const struct layout_box *parent,
	int column_first,
	int row_first)
{
	struct grid_state grid;
	struct grid_item *item;
	struct layout_box *child;
	size_t index;
	int count;
	int track;
	int error;

	/* The tracks it spans are its explicit grid: one auto track each. */
	if (sub->subgrid_column_span > 0) {
		count = sub->subgrid_column_span;
		if (count > CSS_TRACKS)
			count = CSS_TRACKS;
		sub->style.column_count = count;
		memset(&sub->style.column_repeat, 0, sizeof(sub->style.column_repeat));
		for (track = 0; track < count; track++) {
			memset(&sub->style.columns[track], 0, sizeof(sub->style.columns[track]));
			sub->style.columns[track].kind = CSS_TRACK_AUTO;
			sub->style.columns[track].minimum.unit = CSS_UNIT_AUTO;
		}
	}
	if (sub->subgrid_row_span > 0) {
		count = sub->subgrid_row_span;
		if (count > CSS_TRACKS)
			count = CSS_TRACKS;
		sub->style.row_count = count;
		memset(&sub->style.row_repeat, 0, sizeof(sub->style.row_repeat));
		for (track = 0; track < count; track++) {
			memset(&sub->style.rows[track], 0, sizeof(sub->style.rows[track]));
			sub->style.rows[track].kind = CSS_TRACK_AUTO;
			sub->style.rows[track].minimum.unit = CSS_UNIT_AUTO;
		}
	}

	/* The line names of the axes it shares: its own list, and the parent's lines it spans. */
	if (sub->subgrid_column_span > 0) {
		error = grid_subgrid_line_names(tree, sub, 1, parent, column_first);
		if (error != 0)
			return error;
	}
	if (sub->subgrid_row_span > 0) {
		error = grid_subgrid_line_names(tree, sub, 0, parent, row_first);
		if (error != 0)
			return error;
	}

	/* The items placed. */
	layout_grid_names(sub);
	memset(&grid, 0, sizeof(grid));
	error = grid_collect(sub, &grid);
	if (error == 0)
		error = grid_place(&grid, sub->style.grid_auto_flow);
	if (error != 0) {
		grid_release(&grid);
		return error;
	}

	/* Where each went, and the subgrids among them. */
	for (index = 0; index < grid.item_count; index++) {
		item = &grid.items[index];
		child = item->box;
		child->item_row = item->row + 1;
		child->item_column = item->column + 1;
		child->item_row_span = item->row_span;
		child->item_column_span = item->column_span;
		child->subgrid_column_span = 0;
		child->subgrid_row_span = 0;
		child->subgrid_columns = NULL;
		child->subgrid_rows = NULL;
		if (grid_subgrid_axis(child, 1))
			child->subgrid_column_span = item->column_span;
		if (grid_subgrid_axis(child, 0))
			child->subgrid_row_span = item->row_span;
		if (child->subgrid_column_span > 0 || child->subgrid_row_span > 0) {
			error = grid_subgrid_prepare(tree, child, sub, item->column, item->row);
			if (error != 0)
				break;
		}
	}
	grid_release(&grid);
	return error;
}

/*
 * Gives a subgrid the tracks it spans of its parent's axis: each track of
 * the parent, less the subgrid's edges at its first and last, with half
 * the gap's difference added on each interior side.
 */
static int
grid_subgrid_preset(
	struct layout_tree *tree,
	struct layout_box *sub,
	int columns,
	const layout_unit *sizes,
	int first,
	int tracks,
	layout_unit gap)
{
	layout_unit *given;
	layout_unit own_gap;
	layout_unit half;
	layout_unit size;
	int count;
	int index;

	/* The tracks it spans that the parent has. */
	count = columns ? sub->subgrid_column_span : sub->subgrid_row_span;
	if (first + count > tracks)
		count = tracks - first;
	if (count < 1)
		return 0;
	given = wb_arena_alloc(&tree->arena, (size_t)count * sizeof(*given));
	if (given == NULL)
		return ENOMEM;

	/* Each track as the subgrid has it. */
	layout_box_model(sub, 0);
	own_gap = grid_gap_of(sub, columns, gap);
	half = (gap - own_gap) / 2;
	for (index = 0; index < count; index++) {
		size = sizes[first + index];
		if (index == 0)
			size -= grid_edge(sub, columns, 1);
		if (index == count - 1)
			size -= grid_edge(sub, columns, 0);
		if (index > 0)
			size += half;
		if (index < count - 1)
			size += half;
		given[index] = size;
	}

	/* Kept for its layout. */
	if (columns) {
		sub->subgrid_columns = given;
		sub->subgrid_column_gap = own_gap;
	} else {
		sub->subgrid_rows = given;
		sub->subgrid_row_gap = own_gap;
	}
	return 0;
}

/*
 * Describes one declared track to the track sizing algorithm: its minimum
 * and maximum sizing functions, with a percentage resolved against the
 * definite whole (or taken as auto when the whole is not known yet).
 */
static void
grid_track_spec_of(
	const struct css_track *track,
	layout_unit whole,
	struct grid_track_spec *spec)
{
	/* A track starts as auto and auto. */
	memset(spec, 0, sizeof(*spec));
	spec->minimum = GRID_TRACK_AUTO;
	spec->maximum = GRID_TRACK_AUTO;

	/* The maximum, and the minimum a plain length also has. */
	if (track->kind == CSS_TRACK_LENGTH) {
		if (track->size.unit == CSS_UNIT_PX || (track->size.unit == CSS_UNIT_PERCENT && whole >= 0)) {
			spec->maximum = GRID_TRACK_LENGTH;
			spec->maximum_length = grid_track_length(&track->size, whole, 0);
			spec->minimum = GRID_TRACK_LENGTH;
			spec->minimum_length = spec->maximum_length;
		}
	} else if (track->kind == CSS_TRACK_FR) {
		spec->maximum = GRID_TRACK_FLEX;
		spec->flex = track->fr;
	} else if (track->maximum_rule == CSS_TRACK_MAXIMUM_MIN_CONTENT) {
		/* A plain min-content is minmax(min-content, min-content). */
		spec->maximum = GRID_TRACK_MIN_CONTENT;
		if (track->minimum_rule == CSS_TRACK_MINIMUM_NONE)
			spec->minimum = GRID_TRACK_MIN_CONTENT;
	} else if (track->maximum_rule == CSS_TRACK_MAXIMUM_MAX_CONTENT) {
		/* A plain max-content is minmax(max-content, max-content). */
		spec->maximum = GRID_TRACK_MAX_CONTENT;
		if (track->minimum_rule == CSS_TRACK_MINIMUM_NONE)
			spec->minimum = GRID_TRACK_MAX_CONTENT;
	} else if (track->maximum_rule == CSS_TRACK_MAXIMUM_FIT_CONTENT) {
		/* fit-content(P) with a percentage that is not known yet is max-content. */
		spec->maximum = GRID_TRACK_MAX_CONTENT;
		if (track->size.unit == CSS_UNIT_PX || (track->size.unit == CSS_UNIT_PERCENT && whole >= 0)) {
			spec->maximum = GRID_TRACK_FIT_CONTENT;
			spec->maximum_length = grid_track_length(&track->size, whole, 0);
		}
	}

	/* The minimum a minmax() gave. */
	if (track->minimum_rule == CSS_TRACK_MINIMUM_LENGTH) {
		spec->minimum = GRID_TRACK_AUTO;
		if (track->minimum.unit == CSS_UNIT_PX || (track->minimum.unit == CSS_UNIT_PERCENT && whole >= 0)) {
			spec->minimum = GRID_TRACK_LENGTH;
			spec->minimum_length = grid_track_length(&track->minimum, whole, 0);
		}
	} else if (track->minimum_rule == CSS_TRACK_MINIMUM_AUTO) {
		spec->minimum = GRID_TRACK_AUTO;
	} else if (track->minimum_rule == CSS_TRACK_MINIMUM_MIN_CONTENT) {
		spec->minimum = GRID_TRACK_MIN_CONTENT;
	} else if (track->minimum_rule == CSS_TRACK_MINIMUM_MAX_CONTENT) {
		spec->minimum = GRID_TRACK_MAX_CONTENT;
	}
}

/* Resolves a length or percentage bound of an item (a percentage of the definite whole; -1 when it is not known). */
static layout_unit
grid_item_bound(
	const struct css_length *length,
	layout_unit whole)
{
	/* A length. */
	if (length->unit == CSS_UNIT_PX)
		return layout_from_px(length->value) + layout_from_px(length->offset);

	/* A percentage of the whole, when that is known. */
	if (length->unit == CSS_UNIT_PERCENT && whole >= 0)
		return (layout_unit)((float)whole * length->value / 100.0f) + layout_from_px(length->offset);

	/* Not a bound that can be resolved. */
	return -1;
}

/*
 * Measures what an item contributes to a column: its margin box's width
 * when its content is sized at its min-content or max-content width (or its
 * own width, when that is a length), held within its minimum and maximum
 * widths.  The minimum contribution of an item whose preferred width is
 * auto is its minimum width, or its automatic minimum.
 */
static int
grid_column_item_contribution(
	struct layout_tree *tree,
	const struct grid_item *grid_item,
	int kind,
	int automatic,
	layout_unit *fixed,
	layout_unit *outer)
{
	struct layout_box *item;
	layout_unit content;
	layout_unit frame;
	layout_unit margins;
	layout_unit bound;
	layout_unit ratio_width;
	layout_unit basis;
	layout_unit rows;
	struct css_length saved_height;
	int definite;
	int error;

	/* A percentage minimum or maximum depends on the area the contribution decides, so it counts as none. */
	item = grid_item->box;
	*fixed = -1;
	basis = -1;

	/* The frame and margins around the content. */
	layout_box_model(item, 0);
	frame = item->border[CSS_LEFT] + item->padding[CSS_LEFT] + item->padding[CSS_RIGHT] + item->border[CSS_RIGHT];
	margins = item->margin[CSS_LEFT] + item->margin[CSS_RIGHT];

	/* A width the fixed rows and the ratio give. */
	if (kind != GRID_CONTRIBUTION_MINIMUM &&
	    grid_ratio_width(item->parent, grid_item, grid_fixed_row_gap(item->parent), &ratio_width)) {
		*outer = ratio_width;
		return 0;
	}

	/* The preferred width, when it is a length. */
	definite = item->style.width.unit == CSS_UNIT_PX;

	/* The minimum contribution of an item with an auto width is its minimum width, when that is a length. */
	if (kind == GRID_CONTRIBUTION_MINIMUM && !definite) {
		bound = grid_item_bound(&item->style.min_width, basis);
		if (bound >= 0) {
			if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
				bound -= frame;
			if (bound < 0)
				bound = 0;
			*outer = bound + frame + margins;
			return 0;
		}

		/* An intrinsic minimum width is the content's width of that kind. */
		if (item->style.min_width.unit == CSS_UNIT_MIN_CONTENT) {
			kind = GRID_CONTRIBUTION_MIN_CONTENT;
		} else if (item->style.min_width.unit == CSS_UNIT_MAX_CONTENT || item->style.min_width.unit == CSS_UNIT_FIT_CONTENT) {
			kind = GRID_CONTRIBUTION_MAX_CONTENT;
		} else if (!automatic || item->style.overflow_x != CSS_OVERFLOW_VISIBLE ||
		    (item->style.min_width.unit != CSS_UNIT_AUTO && item->style.min_width.unit != CSS_UNIT_PERCENT)) {
			/* No automatic minimum: a scroll container, or an item that does not span an auto minimum. */
			*outer = 0;
			return 0;
		} else {
			/* The automatic minimum is the content's min-content width, or the width the ratio gives. */
			*fixed = frame + margins;
			if (grid_ratio_width(item->parent, grid_item, grid_fixed_row_gap(item->parent), &ratio_width)) {
				*outer = ratio_width;
				return 0;
			}
			kind = GRID_CONTRIBUTION_MIN_CONTENT;
		}
	}

	/* A percentage height resolves against the fixed rows the item spans, which are known before the columns are. */
	saved_height = item->style.height;
	if (item->style.height.unit == CSS_UNIT_PERCENT && grid_fixed_rows_height(item->parent, grid_item, grid_fixed_row_gap(item->parent), &rows)) {
		item->style.height.value = layout_to_px((layout_unit)((float)rows * item->style.height.value / 100.0f) + layout_from_px(item->style.height.offset));
		item->style.height.offset = 0;
		item->style.height.unit = CSS_UNIT_PX;
	}

	/* The content's width. */
	error = 0;
	if (definite) {
		content = layout_from_px(item->style.width.value);
		if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
			content -= frame;
	} else if (kind == GRID_CONTRIBUTION_MAX_CONTENT) {
		error = layout_max_content(tree, item, &content);
		if (error == 0)
			layout_box_model(item, 0);
	} else {
		error = layout_min_content(tree, item, &content);
		if (error == 0)
			layout_box_model(item, 0);
	}
	item->style.height = saved_height;
	if (error != 0)
		return error;

	/* Held within the maximum and minimum widths. */
	bound = grid_item_bound(&item->style.max_width, basis);
	if (bound >= 0) {
		if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
			bound -= frame;
		if (content > bound)
			content = bound;
	}
	bound = grid_item_bound(&item->style.min_width, basis);
	if (bound >= 0) {
		if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
			bound -= frame;
		if (content < bound)
			content = bound;
	}
	if (content < 0)
		content = 0;

	/* Succeeded: the margin box. */
	*outer = content + frame + margins;
	return 0;
}

/* What the callback of the column sizing needs to answer for an entry. */
struct grid_column_context {
	struct layout_tree *tree;
	const struct grid_flat *flat;
	layout_unit whole;
	int error;
};

/* Answers the track sizing for a column entry (a failure is kept in the context and answered as nothing). */
static layout_unit
grid_column_answer(
	void *context,
	size_t index,
	int kind,
	int automatic,
	layout_unit *fixed)
{
	struct grid_column_context *state;
	const struct grid_flat *entry;
	struct grid_item stand_in;
	layout_unit outer;
	int error;

	/* A stand-in for a subgrid's edge has its size. */
	state = context;
	entry = &state->flat[index];
	*fixed = -1;
	if (entry->fixed_size >= 0)
		return entry->fixed_size;

	/* The box's contribution. */
	outer = 0;
	if (state->error != 0)
		return 0;
	memset(&stand_in, 0, sizeof(stand_in));
	stand_in.box = entry->box;
	stand_in.column = entry->start;
	stand_in.column_span = entry->span;
	stand_in.row = entry->row < 0 ? 0 : entry->row;
	stand_in.row_span = entry->row_span < 1 ? 1 : entry->row_span;
	error = grid_column_item_contribution(state->tree, &stand_in, kind, automatic, fixed, &outer);
	if (error != 0) {
		state->error = error;
		return 0;
	}

	/* What the subgrid's edges and gap add. */
	outer += entry->extra;
	if (outer < 0)
		outer = 0;
	if (*fixed >= 0 && entry->extra > 0)
		*fixed += entry->extra;
	return outer;
}

/*
 * Sizes the columns with the track sizing algorithm: the declared tracks
 * and the items' column contributions go in, and the widths come out.  A
 * container being measured is sized under a min-content or max-content
 * constraint, otherwise at its width.
 */
static int
grid_size_columns(
	struct layout_tree *tree,
	struct layout_box *box,
	struct grid_state *grid,
	layout_unit gap)
{
	struct grid_column_context context;
	struct grid_track_input input;
	struct grid_track_spec *tracks;
	struct grid_track_item *items;
	struct grid_flat_list list;
	layout_unit whole;
	layout_unit floor_unused;
	layout_unit outer_unused;
	layout_unit group_floor;
	layout_unit left_before;
	layout_unit left_after;
	layout_unit right_before;
	layout_unit right_after;
	size_t index;
	int constraint;
	int column;
	int error;

	/* The row of widths. */
	grid->columns = calloc((size_t)grid->column_count + 1U, sizeof(*grid->columns));
	if (grid->columns == NULL)
		return ENOMEM;

	/* A subgrid has the tracks its parent gave it. */
	if (box->subgrid_columns != NULL) {
		for (column = 0; column < grid->column_count && column < box->subgrid_column_span; column++)
			grid->columns[column] = box->subgrid_columns[column];
		return 0;
	}

	/* An empty implicit grid has no track for its auto-column size to occupy. */
	if (grid->column_count == 0 || (grid->item_count == 0 && box->style.column_count == 0))
		return 0;

	/* The constraint: the box's width, or the content's size while the tree measures it. */
	constraint = GRID_TRACK_DEFINITE;
	whole = box->width;
	if (tree->measuring != 0 && box->width <= 0) {
		constraint = GRID_TRACK_MIN_CONSTRAINT;
		whole = -1;
	} else if (tree->measuring != 0 && box->width >= ((layout_unit)1 << 23)) {
		constraint = GRID_TRACK_MAX_CONSTRAINT;
		whole = -1;
	}

	/* The tracks and the items in the form the algorithm takes. */
	error = grid_flat_build(&list, grid, 1, gap);

	/* Vertical items that align by a baseline along the row share it within a column, which the column must hold. */
	for (index = 0; error == 0 && index < grid->item_count; index++) {
		if (!grid->items[index].box->grid_orthogonal || grid->items[index].column_span != 1)
			continue;
		error = grid_column_item_contribution(tree, &grid->items[index], GRID_CONTRIBUTION_MAX_CONTENT, 0, &floor_unused, &outer_unused);
	}
	for (column = 0; error == 0 && column < grid->column_count; column++) {
		grid_column_baselines(box, grid, column, &left_before, &left_after, &right_before, &right_after);
		group_floor = left_before + left_after;
		if (right_before + right_after > group_floor)
			group_floor = right_before + right_after;
		if (group_floor > 0)
			error = grid_flat_add(&list, NULL, column, 1, 0, 1, 0, group_floor);
	}
	tracks = calloc((size_t)grid->column_count, sizeof(*tracks));
	items = calloc(list.count + 1U, sizeof(*items));
	if (error != 0 || tracks == NULL || items == NULL) {
		free(list.entries);
		free(tracks);
		free(items);
		return error != 0 ? error : ENOMEM;
	}
	for (column = 0; column < grid->column_count; column++) {
		grid_track_spec_of(grid_column_track(&box->style, column), whole, &tracks[column]);
		if (grid->column_collapsed != NULL && grid->column_collapsed[column])
			tracks[column].collapsed = 1;
	}
	for (index = 0; index < list.count; index++) {
		items[index].start = list.entries[index].start;
		items[index].span = list.entries[index].span;
	}

	/* The sizing. */
	memset(&context, 0, sizeof(context));
	context.tree = tree;
	context.flat = list.entries;
	context.whole = whole;
	memset(&input, 0, sizeof(input));
	input.tracks = tracks;
	input.count = grid->column_count;
	input.items = items;
	input.item_count = list.count;
	input.gap = gap;
	input.constraint = constraint;
	input.available = whole;
	input.container_minimum = -1;
	input.container_maximum = -1;
	input.stretch_auto = grid_content_stretches(box->style.justify_content);
	input.contribution = grid_column_answer;
	input.context = &context;
	error = grid_track_size(&input, grid->columns);
	free(list.entries);
	free(tracks);
	free(items);
	if (error != 0)
		return error;

	/* A failure while measuring an item. */
	return context.error;
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
	float ratio;
	int row;

	/* An auto width and a ratio: the style's for a box that is not replaced, the picture's for one that is. */
	child = item->box;
	ratio = child->style.aspect_ratio;
	if (child->replaced) {
		if (child->style.aspect_ratio > 0.0f || !layout_replaced_ratio(child, &ratio))
			return 0;
	}
	if (child->style.width.unit != CSS_UNIT_AUTO || ratio <= 0.0f)
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
	} else if (child->style.height.unit == CSS_UNIT_AUTO && child->style.margin[CSS_TOP].unit != CSS_UNIT_AUTO &&
	    child->style.margin[CSS_BOTTOM].unit != CSS_UNIT_AUTO) {
		int align;

		/* An item stretched in fixed rows is as tall as they are (less its margins). */
		align = child->style.align_self;
		if (align == CSS_ALIGN_AUTO)
			align = box->style.align_items;
		if (align != CSS_ALIGN_STRETCH)
			return 0;
		if (!grid_fixed_rows_height(box, item, gap, &rows))
			return 0;
		layout_box_model(child, 0);
		height = rows - child->margin[CSS_TOP] - child->margin[CSS_BOTTOM];
		if (child->style.box_sizing != CSS_BOX_SIZING_BORDER)
			height -= child->border[CSS_TOP] + child->padding[CSS_TOP] + child->padding[CSS_BOTTOM] + child->border[CSS_BOTTOM];
		if (height < 0)
			height = 0;
	} else {
		return 0;
	}

	/* Through the ratio (which relates the border boxes under border-box). */
	layout_box_model(child, 0);
	frame_x = child->border[CSS_LEFT] + child->padding[CSS_LEFT] + child->padding[CSS_RIGHT] + child->border[CSS_RIGHT];
	frame_y = child->border[CSS_TOP] + child->padding[CSS_TOP] + child->padding[CSS_BOTTOM] + child->border[CSS_BOTTOM];

	/* The height keeps within its pixel limits first, and the width within its own after. */
	if (child->replaced && child->style.max_height.unit == CSS_UNIT_PX &&
	    height > layout_from_px(child->style.max_height.value) - (child->style.box_sizing == CSS_BOX_SIZING_BORDER ? frame_y : 0))
		height = layout_from_px(child->style.max_height.value) - (child->style.box_sizing == CSS_BOX_SIZING_BORDER ? frame_y : 0);
	if (child->replaced && child->style.min_height.unit == CSS_UNIT_PX &&
	    height < layout_from_px(child->style.min_height.value) - (child->style.box_sizing == CSS_BOX_SIZING_BORDER ? frame_y : 0))
		height = layout_from_px(child->style.min_height.value) - (child->style.box_sizing == CSS_BOX_SIZING_BORDER ? frame_y : 0);
	if (child->style.box_sizing == CSS_BOX_SIZING_BORDER)
		*width = (layout_unit)((float)height * ratio);
	else
		*width = (layout_unit)((float)height * ratio) + frame_x;
	if (child->replaced && child->style.max_width.unit == CSS_UNIT_PX &&
	    *width > layout_from_px(child->style.max_width.value) + (child->style.box_sizing == CSS_BOX_SIZING_BORDER ? 0 : frame_x))
		*width = layout_from_px(child->style.max_width.value) + (child->style.box_sizing == CSS_BOX_SIZING_BORDER ? 0 : frame_x);
	if (child->replaced && child->style.min_width.unit == CSS_UNIT_PX &&
	    *width < layout_from_px(child->style.min_width.value) + (child->style.box_sizing == CSS_BOX_SIZING_BORDER ? 0 : frame_x))
		*width = layout_from_px(child->style.min_width.value) + (child->style.box_sizing == CSS_BOX_SIZING_BORDER ? 0 : frame_x);
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
	struct css_length saved_height;
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
		if (saved.min_width.unit == CSS_UNIT_PERCENT) {
			/* A percentage minimum is of the area. */
			minimum = (layout_unit)((float)width * saved.min_width.value / 100.0f) +
			    layout_from_px(saved.min_width.offset) - sizing;
			if (content < minimum)
				content = minimum;
		}
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

	/* A picture stretched across takes its height from its ratio. */
	saved_height = item->style.height;
	if (item->replaced && justify == CSS_ALIGN_STRETCH && item->style.height.unit == CSS_UNIT_AUTO) {
		layout_unit ratio_width;
		layout_unit ratio_height;

		ratio_width = item->natural_width;
		ratio_height = item->natural_height;
		if (item->image != NULL) {
			ratio_width = (layout_unit)item->image->width;
			ratio_height = (layout_unit)item->image->height;
		}
		if (ratio_width > 0 && ratio_height > 0) {
			item->style.height.unit = CSS_UNIT_PX;
			item->style.height.value = layout_to_px((layout_unit)((float)content * (float)ratio_height / (float)ratio_width));
			item->style.height.offset = 0;
		}
	}
	error = layout_block(tree, item, width);
	item->style.height = saved_height;
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

/* What the callback of the row sizing needs to answer for an entry. */
struct grid_row_context {
	const struct grid_flat *flat;
};

/*
 * Answers the track sizing for a row entry: its margin box's height once
 * laid out in its columns (a stand-in is a baseline group of a row or a
 * subgrid's edge).  The minimum contribution of an item with an auto height
 * is its minimum height, or its automatic minimum.
 */
static layout_unit
grid_row_answer(
	void *context,
	size_t index,
	int kind,
	int automatic,
	layout_unit *fixed)
{
	const struct grid_row_context *state;
	const struct grid_flat *entry;
	const struct layout_box *item;
	layout_unit outer;
	layout_unit frame;
	layout_unit bound;

	/* A stand-in has its size. */
	state = context;
	entry = &state->flat[index];
	*fixed = -1;
	if (entry->fixed_size >= 0)
		return entry->fixed_size;

	/* The item's margin box, with what a subgrid's edges and gap add. */
	item = entry->box;
	outer = grid_outer_height(item) + entry->extra;
	if (outer < 0)
		outer = 0;
	if (kind != GRID_CONTRIBUTION_MINIMUM)
		return outer;

	/* A height that is a length is the least the item can be. */
	if (item->style.height.unit == CSS_UNIT_PX)
		return outer;

	/* A minimum height that is a length is the least. */
	if (item->style.min_height.unit == CSS_UNIT_PX) {
		frame = item->border[CSS_TOP] + item->padding[CSS_TOP] + item->padding[CSS_BOTTOM] + item->border[CSS_BOTTOM];
		bound = layout_from_px(item->style.min_height.value);
		if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
			bound -= frame;
		if (bound < 0)
			bound = 0;
		return bound + frame + item->margin[CSS_TOP] + item->margin[CSS_BOTTOM] + entry->extra;
	}

	/* A scroll container, or an item with another kind of minimum, has none from its content. */
	if (!automatic || item->style.overflow_y != CSS_OVERFLOW_VISIBLE ||
	    (item->style.min_height.unit != CSS_UNIT_AUTO && item->style.min_height.unit != CSS_UNIT_PERCENT))
		return 0;

	/* Otherwise the automatic minimum is its content's height, which a fixed maximum caps. */
	*fixed = item->border[CSS_TOP] + item->padding[CSS_TOP] + item->padding[CSS_BOTTOM] + item->border[CSS_BOTTOM] +
	    item->margin[CSS_TOP] + item->margin[CSS_BOTTOM];
	if (entry->extra > 0)
		*fixed += entry->extra;
	return outer;
}

/*
 * Finds the definite content height of a container (-1 when its height is
 * not one) and its minimum and maximum heights (-1 for none).
 */
static layout_unit
grid_row_space(
	struct layout_tree *tree,
	const struct layout_box *box,
	layout_unit *minimum,
	layout_unit *maximum)
{
	layout_unit frame;
	layout_unit basis;
	layout_unit available;

	/* The frame a border-box size includes. */
	frame = 0;
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
		frame = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];

	/* The minimum and the maximum, in pixels or a percentage of a height that is known. */
	*maximum = -1;
	if (box->style.max_height.unit == CSS_UNIT_PX)
		*maximum = layout_from_px(box->style.max_height.value) - frame;
	else if (box->style.max_height.unit == CSS_UNIT_PERCENT && grid_parent_height(tree, box, &basis))
		*maximum = (layout_unit)((float)basis * box->style.max_height.value / 100.0f) - frame;
	*minimum = -1;
	if (box->style.min_height.unit == CSS_UNIT_PX)
		*minimum = layout_from_px(box->style.min_height.value) - frame;
	else if (box->style.min_height.unit == CSS_UNIT_PERCENT && grid_parent_height(tree, box, &basis))
		*minimum = (layout_unit)((float)basis * box->style.min_height.value / 100.0f) - frame;
	if (*maximum >= 0 && *maximum < *minimum)
		*maximum = *minimum;
	if (*maximum < -1)
		*maximum = 0;

	/* A height in pixels is the definite size, held within them. */
	available = -1;
	if (box->style.height.unit == CSS_UNIT_PX) {
		available = layout_from_px(box->style.height.value) - frame;
		if (*maximum >= 0 && available > *maximum)
			available = *maximum;
		if (*minimum >= 0 && available < *minimum)
			available = *minimum;
		if (available < 0)
			available = 0;
	}
	return available;
}

/*
 * Sizes the rows with the track sizing algorithm: the declared tracks and
 * the heights of the items laid out in their columns go in, and the row
 * heights come out.  A height that is not definite sizes the rows under a
 * max-content constraint; a percentage row of such a grid is then sized
 * again against the height that gave.
 */
static void
grid_size_rows(
	struct layout_tree *tree,
	struct layout_box *box,
	struct grid_state *grid,
	layout_unit gap)
{
	struct grid_row_context context;
	struct grid_track_input input;
	struct grid_track_spec *tracks;
	struct grid_track_item *items;
	struct grid_flat_list list;
	const struct css_track *track;
	layout_unit first_above;
	layout_unit first_below;
	layout_unit last_above;
	layout_unit last_below;
	layout_unit available;
	layout_unit minimum;
	layout_unit maximum;
	layout_unit total;
	layout_unit group_floor;
	size_t index;
	int percentages;
	int again;
	int row;
	int error;

	/* The column of heights (none when the grid has no rows). */
	grid->intrinsic_height = -1;
	grid->rows = calloc((size_t)grid->row_count + 1U, sizeof(*grid->rows));
	if (grid->rows == NULL) {
		grid->row_count = 0;
		return;
	}
	if (grid->row_count == 0)
		return;

	/* A subgrid has the tracks its parent gave it. */
	if (box->subgrid_rows != NULL) {
		for (row = 0; row < grid->row_count && row < box->subgrid_row_span; row++)
			grid->rows[row] = box->subgrid_rows[row];
		return;
	}

	/* The room: the definite height, or the container's minimum and maximum. */
	available = grid_row_space(tree, box, &minimum, &maximum);

	/* The tracks, the items (a subgrid's in its place), and the baseline groups of each row as stand-ins of their own. */
	error = grid_flat_build(&list, grid, 0, gap);
	percentages = 0;
	for (row = 0; row < grid->row_count && error == 0; row++) {
		track = grid_row_track(&box->style, row);
		if (track->size.unit == CSS_UNIT_PERCENT || track->minimum.unit == CSS_UNIT_PERCENT || track->kind == CSS_TRACK_FR)
			percentages = 1;
		grid_row_baselines(box, grid, row, &first_above, &first_below, &last_above, &last_below);
		group_floor = first_above + first_below;
		if (last_above + last_below > group_floor)
			group_floor = last_above + last_below;
		if (group_floor > 0)
			error = grid_flat_add(&list, NULL, row, 1, 0, 1, 0, group_floor);
	}
	tracks = calloc((size_t)grid->row_count, sizeof(*tracks));
	items = calloc(list.count + 1U, sizeof(*items));
	if (error != 0 || tracks == NULL || items == NULL) {
		free(list.entries);
		free(tracks);
		free(items);
		return;
	}
	for (index = 0; index < list.count; index++) {
		items[index].start = list.entries[index].start;
		items[index].span = list.entries[index].span;
	}

	/* The sizing; a percentage row of an indefinite height is sized once as auto and again against the height that gave. */
	again = 0;
	for (;;) {
		for (row = 0; row < grid->row_count; row++) {
			grid_track_spec_of(grid_row_track(&box->style, row), available, &tracks[row]);
			if (grid->row_collapsed != NULL && grid->row_collapsed[row])
				tracks[row].collapsed = 1;
		}
		memset(&context, 0, sizeof(context));
		context.flat = list.entries;
		memset(&input, 0, sizeof(input));
		input.tracks = tracks;
		input.count = grid->row_count;
		input.items = items;
		input.item_count = list.count;
		input.gap = gap;
		input.constraint = available >= 0 ? GRID_TRACK_DEFINITE : GRID_TRACK_MAX_CONSTRAINT;
		input.available = available;
		input.container_minimum = minimum;
		input.container_maximum = maximum;
		input.stretch_auto = grid_content_stretches(box->style.align_content);
		input.contribution = grid_row_answer;
		input.context = &context;
		error = grid_track_size(&input, grid->rows);
		if (error != 0 || again || available >= 0 || !percentages)
			break;

		/* The height the rows gave, within the container's limits, is what the percentages are of. */
		total = gap * (layout_unit)(grid->row_count - 1);
		for (row = 0; row < grid->row_count; row++) {
			if (grid->row_collapsed != NULL && grid->row_collapsed[row])
				total -= gap;
			else
				total += grid->rows[row];
		}
		if (maximum >= 0 && total > maximum)
			total = maximum;
		if (minimum >= 0 && total < minimum)
			total = minimum;
		available = total;
		grid->intrinsic_height = total;
		again = 1;
	}
	free(list.entries);
	free(tracks);
	free(items);
	if (error != 0) {
		for (row = 0; row < grid->row_count; row++)
			grid->rows[row] = 0;
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

	/* The target content height: the whole area for a subgrid in rows, else from the percentage or the area's stretch. */
	frame = item->border[CSS_TOP] + item->padding[CSS_TOP] +
	    item->padding[CSS_BOTTOM] + item->border[CSS_BOTTOM];
	if (item->subgrid_row_span > 0) {
		content = height - item->margin[CSS_TOP] - item->margin[CSS_BOTTOM] - frame;
	} else if (item->style.height.unit == CSS_UNIT_PERCENT) {
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
		justify = grid_normal_starts(item) ? CSS_ALIGN_START : CSS_ALIGN_STRETCH;
	if (item->style.margin[CSS_LEFT].unit == CSS_UNIT_AUTO || item->style.margin[CSS_RIGHT].unit == CSS_UNIT_AUTO)
		justify = CSS_ALIGN_START;
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

	/* Without an auto margin across. */
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
	int span;
	int group;

	/* Each member of the row: an item spanning rows takes part in its first row's first group and its last row's last group, and only reaches into those rows by the part towards them. */
	*first_above = *first_below = *last_above = *last_below = 0;
	for (index = 0; index < grid->item_count; index++) {
		group = grid_row_baseline(box, &grid->items[index], &above);
		if (group == 0)
			continue;
		span = grid->items[index].row_span;
		if (group == 1 && grid->items[index].row != row)
			continue;
		if (group == 2 && grid->items[index].row + span - 1 != row)
			continue;
		outer = grid_outer_height(grid->items[index].box);
		if (group == 1) {
			if (above > *first_above)
				*first_above = above;
			if (span == 1 && outer - above > *first_below)
				*first_below = outer - above;
		} else {
			if (span == 1 && above > *last_above)
				*last_above = above;
			if (outer - above > *last_below)
				*last_below = outer - above;
		}
	}
}

/*
 * Tells whether a vertical item of a horizontal grid aligns by baseline
 * along the row (justify-self baseline or last baseline), and where its
 * baseline is: how far right of its margin box's left edge (before) and how
 * far its margin box reaches left of it (after).  The item's start side (the
 * first baseline's) or end side (the last's) is the side its group is
 * flush to: flush_right is set for the right one.
 */
static int
grid_column_baseline(
	const struct layout_box *box,
	const struct grid_item *item,
	layout_unit *before,
	layout_unit *after,
	int *flush_right)
{
	const struct layout_box *child;
	layout_unit baseline;
	int justify;
	int last;
	int found;

	/* Only a vertical item of a horizontal grid, in one column. */
	child = item->box;
	if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB || !child->grid_orthogonal || item->column_span != 1)
		return 0;
	if (child->style.margin[CSS_LEFT].unit == CSS_UNIT_AUTO || child->style.margin[CSS_RIGHT].unit == CSS_UNIT_AUTO)
		return 0;

	/* A baseline preference along the row. */
	justify = child->style.justify_self;
	last = (child->style.last_baselines & 0x100u) != 0;
	if (justify == CSS_ALIGN_AUTO) {
		justify = box->style.justify_items;
		last = (box->style.last_baselines & 0x200u) != 0;
	}
	if (justify != CSS_ALIGN_BASELINE && !last)
		return 0;

	/* The line's central baseline, or the border box's left (under) edge when it has no line. */
	found = layout_vertical_baseline(child, last, &baseline);
	if (!found)
		baseline = 0;
	*before = child->margin[CSS_LEFT] + baseline;
	*after = child->margin[CSS_LEFT] + child->border[CSS_LEFT] + child->padding[CSS_LEFT] + child->width +
	    child->padding[CSS_RIGHT] + child->border[CSS_RIGHT] + child->margin[CSS_RIGHT] - *before;

	/* The first baseline's group is flush to the item's block-start side, the last's to its block-end side. */
	*flush_right = (child->style.writing_mode == CSS_WRITING_VERTICAL_RL) != (last != 0);
	return 1;
}

/* Finds a column's baseline groups: the farthest their baselines lie from the left edge (left group) and from the right edge (right group), and the room beyond. */
static void
grid_column_baselines(
	const struct layout_box *box,
	const struct grid_state *grid,
	int column,
	layout_unit *left_before,
	layout_unit *left_after,
	layout_unit *right_before,
	layout_unit *right_after)
{
	layout_unit before;
	layout_unit after;
	size_t index;
	int right;

	*left_before = *left_after = *right_before = *right_after = 0;
	for (index = 0; index < grid->item_count; index++) {
		if (grid->items[index].column != column)
			continue;
		if (!grid_column_baseline(box, &grid->items[index], &before, &after, &right))
			continue;
		if (right) {
			if (before > *right_before)
				*right_before = before;
			if (after > *right_after)
				*right_after = after;
		} else {
			if (before > *left_before)
				*left_before = before;
			if (after > *left_after)
				*left_after = after;
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
}

/* Tells whether a normal alignment of a grid item behaves as start: a replaced item does, a checkbox or a radio button stretches like the other controls. */
static int
grid_normal_starts(
	const struct layout_box *box)
{
	return box->replaced && box->control != DOM_CONTROL_CHECKBOX && box->control != DOM_CONTROL_RADIO;
}

/* Adds up the fixed (pixel) rows an item spans and the gaps between them; tells whether every one of them is fixed. */
static int
grid_fixed_rows_height(
	const struct layout_box *box,
	const struct grid_item *item,
	layout_unit gap,
	layout_unit *height)
{
	const struct css_track *track;
	layout_unit rows;
	int row;

	rows = gap * (layout_unit)(item->row_span - 1);
	for (row = item->row; row < item->row + item->row_span; row++) {
		track = grid_row_track(&box->style, row);
		if (track->kind != CSS_TRACK_LENGTH || track->size.unit != CSS_UNIT_PX)
			return 0;
		rows += layout_from_px(track->size.value);
	}
	*height = rows;
	return 1;
}
