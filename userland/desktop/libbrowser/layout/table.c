/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Table layout (ws074-p037), CSS 2's automatic table layout in short: the
 * captions above the table, then the rows one under another (a row group
 * is the box around its rows); each row's cells in the columns (a cell
 * spans colspan of them), the columns as wide as their cells' content
 * allows between the narrowest (min-content) and the widest (max-content)
 * the table's width leaves, a column of a cell with a width in pixels at
 * that width and one with a percentage at that share of the table while
 * the others take the rest (an auto table widens to let the percentages
 * hold), the spacing between the
 * cells, each row as tall as its tallest cell, cells spanning rows holding
 * enough height across those rows, and a cell's content moved down in it by
 * its vertical-align (middle, bottom).  A table of an auto
 * width is as wide as its columns want, at most its containing block's
 * width.  Collapsed borders are approximated: the cells overlap by the
 * first cell's border (one line where they meet, when the borders are
 * alike) and the table's edges have no spacing.  Not in this pass: the
 * fixed layout, the resolution of collapsed borders, column boxes and
 * baselines.
 */

#include "layout/layout.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most columns a cell spans. */
#define TABLE_SPAN_MAX	1000

/*
 * One cell on its way through the algorithm: its box, its row's index and
 * first column, and how many columns it spans.
 */
struct table_cell {
	struct layout_box *box;
	size_t row;
	int column;
	int span;
	int row_span;
	layout_unit wanted;
};

/*
 * A table's layout: the spacing between the cells (negative where
 * collapsed borders overlap) and at the table's edges, its rows (the
 * boxes) and cells in order, and each column's narrowest, widest, whether
 * a cell gave it a width in pixels, the share of the table a cell gave it
 * (0 for none), and its width.
 */
struct table_state {
	layout_unit across;
	layout_unit down;
	layout_unit edge_x;
	layout_unit edge_y;
	struct wb_vector rows;
	struct wb_vector cells;
	int columns;
	layout_unit *least;
	layout_unit *most;
	int *fixed;
	float *percent;
	layout_unit *widths;
	/* The columns run from the right (direction: rtl), over the width the table's cells span. */
	int rtl;
	layout_unit span_width;
};

/*
 * The part of a cell's style that its layout in its column overrides,
 * kept to be put back afterwards.
 */
struct table_sizes {
	int box_sizing;
	struct css_length width;
	struct css_length min_width;
	struct css_length max_width;
	struct css_length height;
	struct css_length min_height;
	struct css_length max_height;
	struct css_length margin[4];
};

static int table_collect(struct layout_box *box, struct table_state *state);
static int table_collect_row(struct layout_box *row, struct table_state *state);
static int table_group_pass(const struct layout_box *group);
static int table_slot_taken(const struct table_state *state, size_t row, int column);
static int table_span(const struct layout_box *cell, const char *name);
static int table_measure(struct layout_tree *tree, struct layout_box *box, struct table_state *state);
static int table_cell_widths(struct layout_tree *tree, struct layout_box *cell, layout_unit containing, layout_unit *least, layout_unit *most, int *fixed, float *percent);
static layout_unit table_wanted(const struct table_state *state, layout_unit spacing, layout_unit sum_most);
static void table_spread(layout_unit *widths, int first, int span, layout_unit spacing, layout_unit wanted);
static void table_columns(struct table_state *state, layout_unit room);
static int table_lay_rows(struct layout_tree *tree, struct table_state *state, layout_unit used, layout_unit *cursor, layout_unit total);
static void table_place_groups(struct layout_box *box, layout_unit edge, layout_unit used, layout_unit cursor);
static void table_collapse(struct table_state *state);
static int table_lay_cell(struct layout_tree *tree, struct layout_box *cell, layout_unit width, layout_unit *wanted);
static void table_shift(struct layout_box *cell, layout_unit offset);
static void table_release(struct table_state *state);
static layout_unit table_frame(const struct layout_box *box);
static layout_unit table_frame_down(const struct layout_box *box);

/*
 * Lays out the content of a table (its box model and width set by the
 * block layout): its captions, row groups, rows and cells, their places
 * relative to the table's content box, the table's width when it is auto,
 * and its content height.
 */
int
layout_table(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct table_state state;
	struct layout_box *child;
	layout_unit spacing;
	layout_unit sum_least;
	layout_unit sum_most;
	layout_unit used;
	layout_unit containing;
	layout_unit cursor;
	layout_unit captions;
	layout_unit rows_start;
	layout_unit caption_least;
	layout_unit least;
	layout_unit total;
	layout_unit sizing;
	int column;
	int error;

	/* The spacing between the cells and at the edges. */
	memset(&state, 0, sizeof(state));
	state.across = layout_from_px(box->style.border_spacing[0]);
	state.down = layout_from_px(box->style.border_spacing[1]);
	state.rtl = box->style.direction == CSS_DIRECTION_RTL;

	/* The rows and cells (collapsed borders overlap instead of the spacing), and each column's narrowest and widest. */
	wb_vector_init(&state.rows, sizeof(struct layout_box *));
	wb_vector_init(&state.cells, sizeof(struct table_cell));
	error = table_collect(box, &state);
	state.edge_x = state.across;
	state.edge_y = state.down;
	if (box->style.border_collapse)
		table_collapse(&state);
	if (error == 0) {
		state.least = calloc((size_t)state.columns + 1U, sizeof(*state.least));
		state.most = calloc((size_t)state.columns + 1U, sizeof(*state.most));
		state.fixed = calloc((size_t)state.columns + 1U, sizeof(*state.fixed));
		state.percent = calloc((size_t)state.columns + 1U, sizeof(*state.percent));
		state.widths = calloc((size_t)state.columns + 1U, sizeof(*state.widths));
		if (state.least == NULL || state.most == NULL || state.fixed == NULL || state.percent == NULL || state.widths == NULL)
			error = ENOMEM;
	}

	/* The columns' narrowest and widest. */
	if (error == 0)
		error = table_measure(tree, box, &state);
	if (error != 0) {
		table_release(&state);
		return error;
	}

	/* The table's width: its columns' wants (the percentages holding) within the room, at least their least. */
	spacing = 0;
	if (state.columns > 0)
		spacing = 2 * state.edge_x + state.across * (layout_unit)(state.columns - 1);
	sum_least = spacing;
	sum_most = spacing;
	for (column = 0; column < state.columns; column++) {
		sum_least += state.least[column];
		sum_most += state.most[column];
	}

	/* The widest caption's narrowest width: the table is at least as wide. */
	caption_least = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->style.display != CSS_DISPLAY_TABLE_CAPTION)
			continue;
		error = layout_min_content(tree, child, &least);
		if (error != 0) {
			table_release(&state);
			return error;
		}
		least += child->margin[CSS_LEFT] + child->margin[CSS_RIGHT] + table_frame(child);
		if (least > caption_least)
			caption_least = least;
	}

	/* An auto table is as wide as it wants (at most the room), and never narrower than its columns' least or its captions'. */
	box->grid_content = table_wanted(&state, spacing, sum_most);
	if (box->grid_content < caption_least)
		box->grid_content = caption_least;
	used = box->width;
	if (box->style.width.unit == CSS_UNIT_AUTO && used > box->grid_content)
		used = box->grid_content;
	if (used < sum_least)
		used = sum_least;
	if (box->style.width.unit == CSS_UNIT_AUTO && used < caption_least && caption_least <= box->width)
		used = caption_least;

	/* A narrower (or wider) table takes its auto margins again in the same containing block. */
	if (used != box->width) {
		containing = box->margin[CSS_LEFT] + table_frame(box) + box->width + box->margin[CSS_RIGHT];
		box->width = used;
		layout_auto_margins(box, containing);
	}

	/* The columns' widths in what the spacing leaves. */
	table_columns(&state, used - spacing);

	/* The captions above the rows (those of caption-side: bottom are laid out for their height, and placed below the rows). */
	cursor = 0;
	captions = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->style.display != CSS_DISPLAY_TABLE_CAPTION)
			continue;
		error = layout_block(tree, child, used);
		if (error != 0)
			break;
		captions += child->margin[CSS_TOP] + table_frame_down(child) + child->height + child->margin[CSS_BOTTOM];
		if (child->style.caption_side != 0) {
			continue;
		}
		child->x = child->margin[CSS_LEFT];
		child->y = cursor + child->margin[CSS_TOP];
		cursor = child->y + table_frame_down(child) + child->height + child->margin[CSS_BOTTOM];
	}

	/* A height of the table is the least its rows fill: the rows share what the captions leave. */
	total = -1;
	sizing = 0;
	if (box->style.box_sizing != CSS_BOX_SIZING_BORDER)
		sizing = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	if (box->style.height.unit == CSS_UNIT_PX)
		total = layout_from_px(box->style.height.value) - table_frame_down(box) + sizing - (box->table_wrapper_height ? captions : 0);
	if (box->style.min_height.unit == CSS_UNIT_PX &&
	    layout_from_px(box->style.min_height.value) - table_frame_down(box) + sizing > total)
		total = layout_from_px(box->style.min_height.value) - table_frame_down(box) + sizing;

	/* The rows, then the row groups around them. */
	rows_start = cursor;
	if (error == 0)
		error = table_lay_rows(tree, &state, used, &cursor, total);
	table_release(&state);
	if (error != 0)
		return error;

	/* A table with a height is that tall below its captions even when it has no rows. */
	if (total > 0 && cursor - rows_start < total)
		cursor = rows_start + total;
	table_place_groups(box, state.edge_x, used, cursor);

	/* The captions below the rows. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->style.display != CSS_DISPLAY_TABLE_CAPTION || child->style.caption_side == 0)
			continue;
		child->x = child->margin[CSS_LEFT];
		child->y = cursor + child->margin[CSS_TOP];
		cursor = child->y + table_frame_down(child) + child->height + child->margin[CSS_BOTTOM];
	}

	/* Succeeded: the table is as tall as its captions, rows and spacing. */
	box->height = cursor;
	return 0;
}

/* Gathers a table's rows (those of its row groups too) and their cells in order; counts the columns. */
static int
table_collect(
	struct layout_box *box,
	struct table_state *state)
{
	struct layout_box *child;
	struct layout_box *row;
	int pass;
	int error;

	/* The rows among the table's children, and in its row groups: the head's first, the foot's last. */
	for (pass = 0; pass < 3; pass++) {
		for (child = box->first_child; child != NULL; child = child->next) {
			if (child->out_of_flow)
				continue;
			if (child->style.display == CSS_DISPLAY_TABLE_ROW) {
				if (pass != 1)
					continue;
				error = table_collect_row(child, state);
				if (error != 0)
					return error;
				continue;
			}

			/* A row group's rows. */
			if (child->style.display != CSS_DISPLAY_TABLE_ROW_GROUP)
				continue;
			if (table_group_pass(child) != pass)
				continue;
			for (row = child->first_child; row != NULL; row = row->next) {
				if (row->out_of_flow || row->style.display != CSS_DISPLAY_TABLE_ROW)
					continue;
				error = table_collect_row(row, state);
				if (error != 0)
					return error;
			}
		}
	}

	/* Succeeded: the rows and cells are gathered. */
	return 0;
}

/* Tells when a row group's rows go: 0 for a table head (first), 2 for a table foot (last), 1 otherwise. */
static int
table_group_pass(
	const struct layout_box *group)
{
	const struct dom_element *element;

	/* Only an HTML thead or tfoot. */
	if (group->node == NULL || group->node->type != DOM_ELEMENT)
		return 1;
	element = (const struct dom_element *)group->node;
	if (element->ns != DOM_NS_HTML)
		return 1;
	if (element->tag == DOM_TAG_THEAD)
		return 0;
	if (element->tag == DOM_TAG_TFOOT)
		return 2;
	return 1;
}

/* Adds a row and its cells, left to right; the widest row makes the columns. */
static int
table_collect_row(
	struct layout_box *row,
	struct table_state *state)
{
	struct table_cell cell;
	struct layout_box *child;
	size_t row_number;
	int offset;
	int column;
	int taken;
	int error;

	/* The row. */
	error = wb_vector_push(&state->rows, &row);
	if (error != 0)
		return error;
	row_number = state->rows.count - 1U;

	/* Its cells. */
	column = 0;
	for (child = row->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->floating != CSS_FLOAT_NONE)
			continue;
		cell.span = table_span(child, "colspan");
		for (;;) {
			/* Skips columns occupied by a cell from an earlier row. */
			taken = table_slot_taken(state, row_number, column);
			while (taken) {
				column++;
				taken = table_slot_taken(state, row_number, column);
			}

			/* Finds whether the complete colspan is free here. */
			for (offset = 0; offset < cell.span; offset++) {
				taken = table_slot_taken(state, row_number, column + offset);
				if (taken)
					break;
			}

			/* A free run is the cell's place; otherwise resumes after the obstruction. */
			if (offset == cell.span)
				break;
			column += offset + 1;
		}

		/* Remembers the cell and the rows and columns it covers. */
		cell.box = child;
		cell.row = row_number;
		cell.column = column;
		cell.row_span = table_span(child, "rowspan");
		cell.wanted = 0;
		error = wb_vector_push(&state->cells, &cell);
		if (error != 0)
			return error;
		column += cell.span;
	}

	/* The columns. */
	if (column > state->columns)
		state->columns = column;

	/* Succeeded: the row and its cells are gathered. */
	return 0;
}

/* Says whether a column in a row is already covered by a cell. */
static int
table_slot_taken(
	const struct table_state *state,
	size_t row,
	int column)
{
	const struct table_cell *cell;
	size_t index;

	/* A cell covers its columns from its first row through its row span. */
	for (index = 0; index < state->cells.count; index++) {
		cell = wb_vector_at(&state->cells, index);
		if (cell->row > row)
			break;
		if (row - cell->row >= (size_t)cell->row_span)
			continue;
		if (column >= cell->column && column < cell->column + cell->span)
			return 1;
	}

	/* No preceding cell covers the slot. */
	return 0;
}

/* Reads the number of columns or rows a cell spans: its attribute, or one. */
static int
table_span(
	const struct layout_box *cell,
	const char *name)
{
	const struct vm_string *attribute;
	size_t index;
	uint16_t unit;
	int span;

	/* An anonymous cell, or a cell without the attribute, spans one. */
	if (cell->node == NULL || cell->node->type != DOM_ELEMENT)
		return 1;
	attribute = dom_attribute_ascii((const struct dom_element *)cell->node, name);
	if (attribute == NULL)
		return 1;

	/* The digits at its start. */
	span = 0;
	for (index = 0; index < attribute->length; index++) {
		unit = vm_string_at(attribute, index);
		if (unit < '0' || unit > '9')
			break;
		span = span * 10 + (int)(unit - '0');
		if (span > TABLE_SPAN_MAX)
			span = TABLE_SPAN_MAX;
	}

	/* One at least. */
	if (span < 1)
		span = 1;
	return span;
}

/*
 * Measures each column's narrowest and widest: the one-column cells'
 * min-content and max-content widths (a width of its own in pixels counts
 * as both, and fixes the column), then the spanning cells widening their
 * columns equally.
 */
static int
table_measure(
	struct layout_tree *tree,
	struct layout_box *box,
	struct table_state *state)
{
	struct table_cell *cell;
	layout_unit narrow;
	layout_unit wide;
	size_t index;
	float percent;
	int column;
	int fixed;
	int pass;
	int error;

	/* The one-column cells first, then the spanning ones. */
	for (pass = 0; pass < 2; pass++) {
		for (index = 0; index < state->cells.count; index++) {
			cell = wb_vector_at(&state->cells, index);
			if (pass == 0 && cell->span != 1)
				continue;
			if (pass == 1 && cell->span == 1)
				continue;
			if (cell->column + cell->span > state->columns)
				continue;

			/* The cell's two widths. */
			error = table_cell_widths(tree, cell->box, box->width, &narrow, &wide, &fixed, &percent);
			if (error != 0)
				return error;

			/* One column takes them; spanned columns widen to hold them. */
			column = cell->column;
			if (cell->span == 1) {
				if (narrow > state->least[column])
					state->least[column] = narrow;
				if (wide > state->most[column])
					state->most[column] = wide;
				if (fixed)
					state->fixed[column] = 1;
				if (percent > state->percent[column])
					state->percent[column] = percent;
			} else {
				table_spread(state->least, column, cell->span, state->across, narrow);
				table_spread(state->most, column, cell->span, state->across, wide);
			}
		}
	}

	/* No column is wider at its least than at its most. */
	for (column = 0; column < state->columns; column++) {
		if (state->most[column] < state->least[column])
			state->most[column] = state->least[column];
	}

	/* Succeeded: the columns are measured. */
	return 0;
}

/*
 * Measures a cell's border box at its min-content and max-content widths
 * (or its own width in pixels, which fixes it), and the share of the
 * table its percentage width asks for (0 for none).
 */
static int
table_cell_widths(
	struct layout_tree *tree,
	struct layout_box *cell,
	layout_unit containing,
	layout_unit *least,
	layout_unit *most,
	int *fixed,
	float *percent)
{
	struct layout_context context;
	struct wb_vector floats;
	struct css_length width;
	layout_unit frame;
	layout_unit content;
	int error;

	/* The widest: its content without a limit (measured with an auto width). */
	*fixed = 0;
	*percent = 0.0f;
	width = cell->style.width;
	cell->style.width.unit = CSS_UNIT_AUTO;
	error = layout_max_content(tree, cell, &content);
	if (error != 0) {
		cell->style.width = width;
		return error;
	}

	/* Its borders and paddings count too. */
	layout_box_model(cell, containing);
	frame = table_frame(cell);
	*most = content + frame;

	/* The narrowest: its content laid out as narrow as it goes. */
	tree->measuring++;
	layout_context_begin(tree, &context, &floats);
	error = layout_block(tree, cell, 0);
	layout_context_end(tree, &context);
	tree->measuring--;
	cell->style.width = width;
	if (error != 0)
		return error;
	*least = layout_content_width(cell, 0) + frame;

	/* A width of its own in pixels is both (at least the narrowest). */
	if (width.unit == CSS_UNIT_PX) {
		content = layout_from_px(width.value);
		if (cell->style.box_sizing != CSS_BOX_SIZING_BORDER)
			content += frame;
		if (content > *least)
			*least = content;
		*most = *least;
		*fixed = 1;
	}

	/* A percentage is a share of the table (at most all of it). */
	if (width.unit == CSS_UNIT_PERCENT && width.value > 0.0f) {
		*percent = width.value / 100.0f;
		if (*percent > 1.0f)
			*percent = 1.0f;
	}

	/* The widest is never narrower than the narrowest. */
	if (*most < *least)
		*most = *least;

	/* Succeeded: the widths are measured. */
	return 0;
}

/* Widens spanned columns equally so that they and the spacing between them hold a width. */
static void
table_spread(
	layout_unit *widths,
	int first,
	int span,
	layout_unit spacing,
	layout_unit wanted)
{
	layout_unit have;
	layout_unit share;
	int column;

	/* What the columns hold now. */
	have = spacing * (layout_unit)(span - 1);
	for (column = first; column < first + span; column++)
		have += widths[column];
	if (have >= wanted)
		return;

	/* Each takes an equal part of what is missing. */
	share = (wanted - have + span - 1) / span;
	for (column = first; column < first + span; column++)
		widths[column] += share;
}

/*
 * Measures how wide an auto table wants to be: its columns at their
 * widest, widened so that each percentage column's widest is its share,
 * and the other columns' widest the share the percentages leave.
 */
static layout_unit
table_wanted(
	const struct table_state *state,
	layout_unit spacing,
	layout_unit sum_most)
{
	layout_unit wanted;
	layout_unit rest;
	layout_unit width;
	float shares;
	int column;

	/* Each percentage column's widest is its share of the columns' room. */
	wanted = sum_most;
	rest = 0;
	shares = 0.0f;
	for (column = 0; column < state->columns; column++) {
		if (state->percent[column] <= 0.0f) {
			rest += state->most[column];
			continue;
		}

		/* A percentage column. */
		shares += state->percent[column];
		width = (layout_unit)((float)state->most[column] / state->percent[column]) + spacing;
		if (width > wanted)
			wanted = width;
	}

	/* The other columns take what the percentages leave. */
	if (shares > 0.0f && shares < 1.0f && rest > 0) {
		width = (layout_unit)((float)rest / (1.0f - shares)) + spacing;
		if (width > wanted)
			wanted = width;
	}

	/* The width. */
	return wanted;
}

/*
 * Shares a room among the columns: a percentage column its share (at
 * least its narrowest) first; the rest among the others, all of them at
 * their widest and what is left to the columns without a width of their
 * own (to all when every column has one) in proportion to their widest
 * (equally when they want nothing) when it is enough; between their
 * narrowest and widest in proportion to the difference when it is not;
 * their narrowest when even that does not fit.  What no other column
 * takes goes to the percentage columns.
 */
static void
table_columns(
	struct table_state *state,
	layout_unit room)
{
	layout_unit sum_least;
	layout_unit sum_most;
	layout_unit sum_free;
	layout_unit sum_shares;
	layout_unit extra;
	int free_count;
	int others;
	int any_free;
	int column;

	/* The percentage columns first. */
	sum_shares = 0;
	others = 0;
	for (column = 0; column < state->columns; column++) {
		if (state->percent[column] <= 0.0f) {
			others++;
			continue;
		}

		/* Its share, at least its narrowest. */
		state->widths[column] = (layout_unit)((float)room * state->percent[column]);
		if (state->widths[column] < state->least[column])
			state->widths[column] = state->least[column];
		sum_shares += state->widths[column];
	}

	/* What the others share. */
	room -= sum_shares;

	/* Without other columns, the percentage columns take what is left in proportion to their widths. */
	if (others == 0) {
		if (room > 0 && sum_shares > 0) {
			for (column = 0; column < state->columns; column++)
				state->widths[column] += (layout_unit)((float)room * (float)state->widths[column] / (float)sum_shares);
		}

		/* Nothing else to share. */
		return;
	}

	/* The sums of the other columns, and those that take the rest. */
	sum_least = 0;
	sum_most = 0;
	sum_free = 0;
	free_count = 0;
	for (column = 0; column < state->columns; column++) {
		if (state->percent[column] > 0.0f)
			continue;
		sum_least += state->least[column];
		sum_most += state->most[column];
		if (!state->fixed[column]) {
			sum_free += state->most[column];
			free_count++;
		}
	}

	/* Without a free column, every other column takes the rest. */
	any_free = free_count > 0;
	if (!any_free) {
		sum_free = sum_most;
		free_count = others;
	}

	/* Room for every column at its widest: the rest to the free columns. */
	if (room >= sum_most) {
		extra = room - sum_most;
		for (column = 0; column < state->columns; column++) {
			if (state->percent[column] > 0.0f)
				continue;
			state->widths[column] = state->most[column];
			if (any_free && state->fixed[column])
				continue;
			if (sum_free > 0) {
				state->widths[column] += (layout_unit)((float)extra * (float)state->most[column] / (float)sum_free);
			} else {
				state->widths[column] += extra / free_count;
			}
		}

		/* The room is shared. */
		return;
	}

	/* Room between the least and the most: each column in proportion to its difference. */
	if (room > sum_least && sum_most > sum_least) {
		for (column = 0; column < state->columns; column++) {
			if (state->percent[column] > 0.0f)
				continue;
			state->widths[column] = state->least[column] +
			    (layout_unit)((float)(room - sum_least) * (float)(state->most[column] - state->least[column]) / (float)(sum_most - sum_least));
		}

		/* The room is shared. */
		return;
	}

	/* Not even that: each at its least. */
	for (column = 0; column < state->columns; column++) {
		if (state->percent[column] <= 0.0f)
			state->widths[column] = state->least[column];
	}
}

/*
 * Lays out the rows from a place down the table's content box: each row's
 * cells in their columns, the row as tall as its tallest cell (or its own
 * height), the cells as tall as the row with their content moved down by
 * their vertical-align, and the spacing around the rows; the place moves
 * below them.
 */
static int
table_lay_rows(
	struct layout_tree *tree,
	struct table_state *state,
	layout_unit used,
	layout_unit *cursor,
	layout_unit total)
{
	struct table_cell *cell;
	struct layout_box *row;
	layout_unit *heights;
	layout_unit x;
	layout_unit width;
	layout_unit height;
	layout_unit wanted;
	layout_unit target;
	layout_unit offset;
	layout_unit baseline;
	layout_unit *row_baselines;
	layout_unit have;
	layout_unit missing;
	size_t index;
	size_t number;
	size_t last_row;
	int column;
	int align;
	int error;

	/* Each row starts at the height it explicitly asks for. */
	heights = calloc(state->rows.count + 1U, sizeof(*heights));
	if (heights == NULL)
		return ENOMEM;
	for (number = 0; number < state->rows.count; number++) {
		row = *(struct layout_box **)wb_vector_at(&state->rows, number);
		if (row->style.height.unit == CSS_UNIT_PX)
			heights[number] = layout_from_px(row->style.height.value);
	}

	/* Lay every cell out at its width, and let one-row cells size their row. */
	for (index = 0; index < state->cells.count; index++) {
		cell = wb_vector_at(&state->cells, index);
		x = 0;
		for (column = 0; column < cell->column; column++)
			x += state->widths[column] + state->across;
		width = state->across * (layout_unit)(cell->span - 1);
		for (column = cell->column;
		    column < cell->column + cell->span && column < state->columns;
		    column++)
			width += state->widths[column];
		error = table_lay_cell(tree, cell->box, width, &wanted);
		if (error != 0) {
			free(heights);
			return error;
		}

		/* A table of direction rtl has its first column at the right. */
		if (state->rtl)
			x = used - 2 * state->edge_x - x - width;

		/* Keeps the cell at its horizontal grid position until rows are placed. */
		cell->box->x = x;
		cell->box->y = 0;
		if (wanted < cell->box->height)
			wanted = cell->box->height;
		cell->wanted = wanted + table_frame_down(cell->box);
		if (cell->row_span == 1 && cell->wanted > heights[cell->row])
			heights[cell->row] = cell->wanted;
	}

	/* A spanning cell may make the last row it reaches taller. */
	for (index = 0; index < state->cells.count; index++) {
		cell = wb_vector_at(&state->cells, index);
		if (cell->row_span == 1)
			continue;
		last_row = cell->row + (size_t)cell->row_span;
		if (last_row > state->rows.count)
			last_row = state->rows.count;
		have = state->down * (layout_unit)(last_row - cell->row - 1U);
		for (number = cell->row; number < last_row; number++)
			have += heights[number];
		if (have < cell->wanted) {
			missing = cell->wanted - have;
			heights[last_row - 1U] += missing;
		}
	}

	/* A table with a height of its own: the rows grow in proportion to share what its content leaves (the first row's share when none has a height). */
	if (total >= 0 && state->rows.count != 0) {
		layout_unit sum;
		layout_unit extra;
		layout_unit given;

		sum = state->edge_y + state->edge_y + state->down * (layout_unit)(state->rows.count - 1U);
		for (number = 0; number < state->rows.count; number++)
			sum += heights[number];
		extra = total - sum;
		if (extra > 0) {
			layout_unit heights_sum;
			size_t auto_count;
			size_t last_taker;
			int *takes;

			/* Rows with no height of their own (the row's, or a cell's) take the extra first; with none, every row shares it. */
			takes = calloc(state->rows.count + 1U, sizeof(*takes));
			if (takes == NULL) {
				free(heights);
				return ENOMEM;
			}
			for (number = 0; number < state->rows.count; number++) {
				row = *(struct layout_box **)wb_vector_at(&state->rows, number);
				takes[number] = row->style.height.unit == CSS_UNIT_AUTO;
			}
			for (index = 0; index < state->cells.count; index++) {
				cell = wb_vector_at(&state->cells, index);
				if (cell->box->style.height.unit != CSS_UNIT_AUTO)
					takes[cell->row] = 0;
			}
			auto_count = 0;
			for (number = 0; number < state->rows.count; number++) {
				if (takes[number])
					auto_count++;
			}
			if (auto_count == 0) {
				for (number = 0; number < state->rows.count; number++)
					takes[number] = 1;
				auto_count = state->rows.count;
			}
			heights_sum = 0;
			last_taker = 0;
			for (number = 0; number < state->rows.count; number++) {
				if (takes[number]) {
					heights_sum += heights[number];
					last_taker = number;
				}
			}
			given = 0;
			for (number = 0; number < state->rows.count; number++) {
				layout_unit share;

				if (!takes[number])
					continue;
				if (number == last_taker) {
					share = extra - given;
				} else if (heights_sum > 0) {
					share = (layout_unit)((float)extra * (float)heights[number] / (float)heights_sum);
				} else {
					share = extra / (layout_unit)auto_count;
				}
				heights[number] += share;
				given += share;
			}
			free(takes);
		}
	}

	/* The spacing above the first row. */
	if (state->rows.count != 0)
		*cursor += state->edge_y;

	/* Place the rows from top to bottom. */
	for (number = 0; number < state->rows.count; number++) {
		row = *(struct layout_box **)wb_vector_at(&state->rows, number);
		height = heights[number];

		/* The row's box, in the table's content box inside the spacing, with no margins, borders or paddings of its own. */
		memset(row->margin, 0, sizeof(row->margin));
		memset(row->border, 0, sizeof(row->border));
		memset(row->padding, 0, sizeof(row->padding));
		row->x = state->edge_x;
		row->y = *cursor;
		row->width = used - 2 * state->edge_x;
		if (row->width < 0)
			row->width = 0;
		row->height = height;
		row->collapsed_top = 0;
		row->collapsed_bottom = 0;
		*cursor += height;
		if (number + 1U < state->rows.count) {
			*cursor += state->down;
		} else {
			*cursor += state->edge_y;
		}
	}

	/* The baseline of each row: the farthest down the first baselines of its baseline-aligned cells lie. */
	row_baselines = calloc(state->rows.count + 1U, sizeof(*row_baselines));
	if (row_baselines == NULL) {
		free(heights);
		return ENOMEM;
	}
	for (index = 0; index < state->cells.count; index++) {
		cell = wb_vector_at(&state->cells, index);
		if (cell->box->style.vertical_align != CSS_VALIGN_BASELINE || cell->row_span != 1 || cell->row >= state->rows.count)
			continue;
		if (layout_first_baseline(cell->box, &offset) && offset > row_baselines[cell->row])
			row_baselines[cell->row] = offset;
	}

	/* Each cell fills all the rows it spans, with its content vertically aligned. */
	for (index = 0; index < state->cells.count; index++) {
		cell = wb_vector_at(&state->cells, index);
		last_row = cell->row + (size_t)cell->row_span;
		if (last_row > state->rows.count)
			last_row = state->rows.count;
		height = state->down * (layout_unit)(last_row - cell->row - 1U);
		for (number = cell->row; number < last_row; number++)
			height += heights[number];
		target = height - table_frame_down(cell->box);
		if (target < 0)
			target = 0;
		offset = 0;
		align = cell->box->style.vertical_align;
		if (align == CSS_VALIGN_MIDDLE)
			offset = (target - cell->box->height) / 2;
		if (align == CSS_VALIGN_BOTTOM)
			offset = target - cell->box->height;
		if (align == CSS_VALIGN_BASELINE && cell->row_span == 1 && cell->row < state->rows.count &&
		    layout_first_baseline(cell->box, &baseline) && row_baselines[cell->row] > baseline)
			offset = row_baselines[cell->row] - baseline;
		if (offset > 0)
			table_shift(cell->box, offset);
		cell->box->height = target;
	}

	/* The row heights are no longer needed. */
	free(row_baselines);
	free(heights);

	/* Succeeded: the rows are laid out. */
	return 0;
}

/*
 * Places each row group around its rows (as wide as they are, from the
 * top of the first to the bottom of the last; an empty one at the bottom),
 * the rows then relative to it.
 */
static void
table_place_groups(
	struct layout_box *box,
	layout_unit edge,
	layout_unit used,
	layout_unit cursor)
{
	struct layout_box *group;
	struct layout_box *row;
	layout_unit top;
	layout_unit bottom;
	int any;

	/* Each row group. */
	for (group = box->first_child; group != NULL; group = group->next) {
		if (group->out_of_flow || group->style.display != CSS_DISPLAY_TABLE_ROW_GROUP)
			continue;

		/* The extent of its rows. */
		any = 0;
		top = cursor;
		bottom = cursor;
		for (row = group->first_child; row != NULL; row = row->next) {
			if (row->out_of_flow || row->style.display != CSS_DISPLAY_TABLE_ROW)
				continue;
			if (!any || row->y < top)
				top = row->y;
			if (!any || row->y + row->height > bottom)
				bottom = row->y + row->height;
			any = 1;
		}

		/* The group's box, with no margins, borders or paddings of its own. */
		memset(group->margin, 0, sizeof(group->margin));
		memset(group->border, 0, sizeof(group->border));
		memset(group->padding, 0, sizeof(group->padding));
		group->x = edge;
		group->y = top;
		group->width = used - 2 * edge;
		if (group->width < 0)
			group->width = 0;
		group->height = bottom - top;
		group->collapsed_top = 0;
		group->collapsed_bottom = 0;

		/* Its rows, relative to it. */
		for (row = group->first_child; row != NULL; row = row->next) {
			if (row->out_of_flow || row->style.display != CSS_DISPLAY_TABLE_ROW)
				continue;
			row->x -= edge;
			row->y -= top;
		}
	}
}

/*
 * Makes the spacing of a table whose borders collapse: none at the edges,
 * and the cells overlapping by the first cell's right and bottom borders
 * (the line between two cells drawn once when their borders are alike).
 */
static void
table_collapse(
	struct table_state *state)
{
	const struct table_cell *cell;
	const struct css_style *style;

	/* No overlap without cells. */
	state->across = 0;
	state->down = 0;
	state->edge_x = 0;
	state->edge_y = 0;
	if (state->cells.count == 0)
		return;

	/* The first cell's borders. */
	cell = wb_vector_at(&state->cells, 0);
	style = &cell->box->style;
	if (style->border_style[CSS_RIGHT] != CSS_BORDER_NONE)
		state->across = -layout_from_px(style->border_width[CSS_RIGHT]);
	if (style->border_style[CSS_BOTTOM] != CSS_BORDER_NONE)
		state->down = -layout_from_px(style->border_width[CSS_BOTTOM]);
}

/*
 * Lays a cell out as a block whose border box is a width, as tall as its
 * content (its style's sizes and margins are put back afterwards);
 * reports the content height its own height in pixels asks for (0 when
 * none).
 */
static int
table_lay_cell(
	struct layout_tree *tree,
	struct layout_box *cell,
	layout_unit width,
	layout_unit *wanted)
{
	struct table_sizes saved;
	layout_unit content;
	int side;
	int error;

	/* Keeps the style's sizes and margins, which the layout in the column overrides. */
	saved.box_sizing = cell->style.box_sizing;
	saved.width = cell->style.width;
	saved.min_width = cell->style.min_width;
	saved.max_width = cell->style.max_width;
	saved.height = cell->style.height;
	saved.min_height = cell->style.min_height;
	saved.max_height = cell->style.max_height;
	memcpy(saved.margin, cell->style.margin, sizeof(saved.margin));

	/* The content width the border box leaves; a cell has no margins, and its content's height (the row's is set later). */
	layout_box_model(cell, width);
	content = width - table_frame(cell);
	if (content < 0)
		content = 0;
	cell->style.box_sizing = CSS_BOX_SIZING_CONTENT;
	cell->style.width.unit = CSS_UNIT_PX;
	cell->style.width.value = layout_to_px(content);
	cell->style.width.offset = 0;
	cell->style.min_width.unit = CSS_UNIT_PX;
	cell->style.min_width.value = 0;
	cell->style.max_width.unit = CSS_UNIT_NONE;
	cell->style.height.unit = CSS_UNIT_AUTO;
	cell->style.min_height.unit = CSS_UNIT_PX;
	cell->style.min_height.value = 0;
	cell->style.max_height.unit = CSS_UNIT_NONE;
	for (side = 0; side < 4; side++) {
		cell->style.margin[side].unit = CSS_UNIT_PX;
		cell->style.margin[side].value = 0;
		cell->style.margin[side].offset = 0;
	}

	/* The layout in the column. */
	error = layout_block(tree, cell, width);

	/* The style is the cell's own again. */
	cell->style.box_sizing = saved.box_sizing;
	cell->style.width = saved.width;
	cell->style.min_width = saved.min_width;
	cell->style.max_width = saved.max_width;
	cell->style.height = saved.height;
	cell->style.min_height = saved.min_height;
	cell->style.max_height = saved.max_height;
	memcpy(cell->style.margin, saved.margin, sizeof(saved.margin));

	/* Reports a cell that could not be laid out. */
	if (error != 0)
		return error;

	/* The content height its own height asks for (the frame comes off a border-box height). */
	*wanted = 0;
	if (saved.height.unit == CSS_UNIT_PX) {
		*wanted = layout_from_px(saved.height.value);
		if (saved.box_sizing == CSS_BOX_SIZING_BORDER)
			*wanted -= table_frame_down(cell);
		if (*wanted < 0)
			*wanted = 0;
	}

	/* Succeeded: the cell is laid out in its column. */
	return 0;
}

/* Moves a laid out cell's content down by an offset: its lines, or its child boxes (floats and boxes out of the flow too). */
static void
table_shift(
	struct layout_box *cell,
	layout_unit offset)
{
	struct layout_box *child;
	size_t index;

	/* The lines of a cell of inline content. */
	if (cell->children_inline) {
		for (index = 0; index < cell->line_count; index++)
			cell->lines[index].y += offset;
	}

	/* The child boxes (the floats and inline blocks of inline content are placed relative to the cell too). */
	for (child = cell->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow) {
			child->static_y += offset;
			continue;
		}

		/* A box in the flow, a float or an inline block. */
		if (!cell->children_inline || child->floating != CSS_FLOAT_NONE || child->atomic)
			child->y += offset;
	}
}

/* Frees a table's layout state. */
static void
table_release(
	struct table_state *state)
{
	/* The columns, rows and cells. */
	free(state->least);
	free(state->most);
	free(state->fixed);
	free(state->percent);
	free(state->widths);
	wb_vector_release(&state->rows);
	wb_vector_release(&state->cells);
}

/* Measures a box's borders and paddings across. */
static layout_unit
table_frame(
	const struct layout_box *box)
{
	/* The four. */
	return box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
}

/* Measures a box's borders and paddings down. */
static layout_unit
table_frame_down(
	const struct layout_box *box)
{
	/* The four. */
	return box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
}
