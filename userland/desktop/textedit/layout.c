/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rows Text Editor lays the text out in (plan/ws092/design.md
 * section 8).
 *
 * Text is set in cells of the monospaced font: most characters take one,
 * wide ones (CJK, full width, the common emoji) two, a tab reaches the
 * next tab stop, and a control character shows as ^X in two.  With word
 * wrap on, a line longer than the columns breaks after its last blank
 * that fits (or where it crosses, without one); a blank may hang past the
 * edge.  How many rows each line takes is kept, with the row each line
 * starts on; a line's row starts are worked out again when needed.
 */

#include "textedit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The first room of the tables. */
#define LAYOUT_INITIAL		256U

/* The columns a line has when it is not wrapped (no line is this wide). */
#define LAYOUT_UNWRAPPED	((unsigned)-1)

static int layout_room(struct te_layout *layout, size_t count);
static int layout_break_room(struct te_layout *layout, size_t count);
static size_t layout_line_rows(struct te_layout *layout, const struct te_buffer *buffer, size_t line, int keep);
static int layout_wide(uint32_t codepoint);
static void layout_number(struct te_layout *layout, size_t from);

/*
 * Starts an empty layout, wrapped at 80 columns with tab stops every 4.
 */
void
te_layout_init(
	struct te_layout *layout)
{
	/* Nothing laid out yet. */
	memset(layout, 0, sizeof(*layout));
	layout->wrap = 1;
	layout->columns = 80U;
	layout->tab = 4U;
}

/*
 * Frees a layout's tables.
 */
void
te_layout_free(
	struct te_layout *layout)
{
	/* The tables and the scratch. */
	free(layout->rows);
	free(layout->first_row);
	free(layout->breaks);
	memset(layout, 0, sizeof(*layout));
}

/*
 * Lays the whole document out again: wrapped at columns (at least 1) or
 * not wrapped.
 *
 * Returns 0, or ENOMEM.
 */
int
te_layout_reset(
	struct te_layout *layout,
	const struct te_buffer *buffer,
	int wrap,
	unsigned columns)
{
	size_t line;
	int error;

	/* The way lines are set. */
	layout->wrap = wrap;
	layout->columns = columns;
	if (layout->columns == 0U)
		layout->columns = 1U;
	if (!wrap)
		layout->columns = LAYOUT_UNWRAPPED;
	layout->widest = 0;

	/* Room for every line. */
	error = layout_room(layout, buffer->line_count);
	if (error != 0)
		return error;
	layout->lines = buffer->line_count;

	/* Each line's rows, and where each starts. */
	for (line = 0; line < layout->lines; line++)
		layout->rows[line] = layout_line_rows(layout, buffer, line, 0);
	layout_number(layout, 0);

	/* Succeeded: the rows are known. */
	return 0;
}

/*
 * Lays out again after an edit: the old_count lines from first became the
 * new_count lines from first (the rest moved but kept their rows).
 *
 * Returns 0, or ENOMEM (the layout should then be reset).
 */
int
te_layout_update(
	struct te_layout *layout,
	const struct te_buffer *buffer,
	size_t first,
	size_t old_count,
	size_t new_count)
{
	size_t lines;
	size_t index;
	int error;

	/* Room for the lines the edit added. */
	lines = layout->lines - old_count + new_count;
	error = layout_room(layout, lines);
	if (error != 0)
		return error;

	/* The lines after the edit move to their new places. */
	memmove(layout->rows + first + new_count, layout->rows + first + old_count, (layout->lines - first - old_count) * sizeof(layout->rows[0]));
	layout->lines = lines;

	/* The edited lines' rows. */
	for (index = first; index < first + new_count; index++)
		layout->rows[index] = layout_line_rows(layout, buffer, index, 0);

	/* Where each line starts, from the first edited one. */
	layout_number(layout, first);

	/* Succeeded: the layout follows the text. */
	return 0;
}

/*
 * Reports how many cells a character takes at a column (a tab reaches the
 * next stop).
 */
unsigned
te_layout_cells(
	uint32_t codepoint,
	unsigned column,
	unsigned tab)
{
	int wide;

	/* A tab reaches the next stop. */
	if (codepoint == '\t')
		return tab - column % tab;

	/* A control character shows as ^X. */
	if (codepoint < 0x20U || codepoint == 0x7fU)
		return 2U;

	/* A wide character takes two cells. */
	wide = layout_wide(codepoint);
	if (wide)
		return 2U;

	/* Anything else takes one. */
	return 1U;
}

/*
 * Gives where each row of a line starts (breaks[0] is the line's start),
 * in memory the layout keeps until its next call, and reports how many
 * rows the line takes.
 */
size_t
te_layout_breaks(
	struct te_layout *layout,
	const struct te_buffer *buffer,
	size_t line,
	const size_t **breaks)
{
	size_t rows;

	/* The line's rows, keeping their starts. */
	rows = layout_line_rows(layout, buffer, line, 1);

	/* Succeeded: the starts. */
	*breaks = layout->breaks;
	return rows;
}

/*
 * Reports which line a row belongs to (the last line past the last row).
 */
size_t
te_layout_line_of_row(
	const struct te_layout *layout,
	size_t row)
{
	size_t low;
	size_t high;
	size_t middle;

	/* The last line starting at or before the row. */
	low = 0;
	high = layout->lines;
	while (high - low > 1U) {
		middle = low + (high - low) / 2U;
		if (layout->first_row[middle] <= row) {
			low = middle;
		} else {
			high = middle;
		}
	}

	/* Succeeded: the line. */
	return low;
}

/*
 * Gives the bytes a row shows, from start up to end (the next row's start,
 * or the line's end for its last row).
 */
void
te_layout_row_range(
	struct te_layout *layout,
	const struct te_buffer *buffer,
	size_t row,
	size_t *start,
	size_t *end)
{
	const size_t *breaks;
	size_t line;
	size_t rows;
	size_t index;

	/* The row's line and its row starts. */
	line = te_layout_line_of_row(layout, row);
	rows = te_layout_breaks(layout, buffer, line, &breaks);
	index = row - layout->first_row[line];
	if (index >= rows)
		index = rows - 1U;

	/* The row's start, and where the next begins or the line ends. */
	*start = breaks[index];
	*end = te_buffer_line_end(buffer, line);
	if (index + 1U < rows)
		*end = breaks[index + 1U];
}

/*
 * Gives the row and the cell column a position is shown at.  A position
 * where a row breaks is shown at the start of the next row.
 */
void
te_layout_place(
	struct te_layout *layout,
	const struct te_buffer *buffer,
	size_t position,
	size_t *row,
	size_t *column)
{
	const size_t *breaks;
	size_t line;
	size_t rows;
	size_t index;

	/* The position's line and its row starts. */
	line = te_buffer_line_of(buffer, position);
	rows = te_layout_breaks(layout, buffer, line, &breaks);

	/* The last row starting at or before the position. */
	index = 0;
	while (index + 1U < rows && breaks[index + 1U] <= position)
		index++;

	/* Succeeded: the row, and the cells from its start. */
	*row = layout->first_row[line] + index;
	*column = te_layout_columns_between(layout, buffer, breaks[index], position);
}

/*
 * Reports the position in a row nearest to a cell column (the boundary
 * between characters closest to it).  A row that is not a line's last
 * gives no position past its last character.
 */
size_t
te_layout_position(
	struct te_layout *layout,
	const struct te_buffer *buffer,
	size_t row,
	size_t column)
{
	uint32_t codepoint;
	size_t start;
	size_t end;
	size_t line;
	size_t last;
	size_t line_end;
	size_t position;
	size_t next;
	size_t cells;
	unsigned width;

	/* A row past the last is the end of the document. */
	if (row >= layout->total) {
		position = te_buffer_length(buffer);
		return position;
	}

	/* The row's bytes; a wrapped row ends before its last character. */
	te_layout_row_range(layout, buffer, row, &start, &end);
	line = te_layout_line_of_row(layout, row);
	line_end = te_buffer_line_end(buffer, line);
	last = end;
	if (end != line_end && end > start)
		last = te_buffer_prev_char(buffer, end);

	/* The characters until the column is passed. */
	position = start;
	cells = 0;
	while (position < last) {
		codepoint = te_buffer_char(buffer, position, &next);
		width = te_layout_cells(codepoint, (unsigned)cells, layout->tab);

		/* The column is in this character: the nearer of its two ends. */
		if (cells + width > column) {
			if (column - cells >= (width + 1U) / 2U)
				position = next;
			return position;
		}

		/* The next character. */
		cells += width;
		position = next;
	}

	/* Past the row's text: its end. */
	return position;
}

/*
 * Reports how many cells the characters from start up to end take, when
 * start begins a row.
 */
size_t
te_layout_columns_between(
	const struct te_layout *layout,
	const struct te_buffer *buffer,
	size_t start,
	size_t end)
{
	uint32_t codepoint;
	size_t position;
	size_t next;
	size_t cells;

	/* Each character's cells. */
	cells = 0;
	position = start;
	while (position < end) {
		codepoint = te_buffer_char(buffer, position, &next);
		cells += te_layout_cells(codepoint, (unsigned)cells, layout->tab);
		position = next;
	}

	/* Succeeded: the cells. */
	return cells;
}

/* Makes the tables hold count lines; returns 0 or ENOMEM. */
static int
layout_room(
	struct te_layout *layout,
	size_t count)
{
	size_t capacity;
	size_t *rows;
	size_t *first_row;

	/* Tables with the room stay. */
	if (count <= layout->capacity)
		return 0;

	/* Doubled until they fit. */
	capacity = layout->capacity;
	if (capacity == 0U)
		capacity = LAYOUT_INITIAL;
	while (capacity < count)
		capacity *= 2U;

	/* The rows of each line. */
	rows = realloc(layout->rows, capacity * sizeof(rows[0]));
	if (rows == NULL)
		return ENOMEM;
	layout->rows = rows;

	/* The row each line starts on. */
	first_row = realloc(layout->first_row, capacity * sizeof(first_row[0]));
	if (first_row == NULL)
		return ENOMEM;
	layout->first_row = first_row;

	/* Succeeded: the tables have the room. */
	layout->capacity = capacity;
	return 0;
}

/* Makes the scratch hold count row starts; returns 0 or ENOMEM. */
static int
layout_break_room(
	struct te_layout *layout,
	size_t count)
{
	size_t capacity;
	size_t *breaks;

	/* A scratch with the room stays. */
	if (count <= layout->break_capacity)
		return 0;

	/* Doubled until it fits. */
	capacity = layout->break_capacity;
	if (capacity == 0U)
		capacity = LAYOUT_INITIAL;
	while (capacity < count)
		capacity *= 2U;

	/* The larger scratch. */
	breaks = realloc(layout->breaks, capacity * sizeof(breaks[0]));
	if (breaks == NULL)
		return ENOMEM;

	/* Succeeded: the scratch has the room. */
	layout->breaks = breaks;
	layout->break_capacity = capacity;
	return 0;
}

/*
 * Works out the rows of a line, keeping their starts in the scratch when
 * keep is set (without memory for them the line is one row), and notes the
 * line's width when it is not wrapped.  Reports how many rows it takes.
 */
static size_t
layout_line_rows(
	struct te_layout *layout,
	const struct te_buffer *buffer,
	size_t line,
	int keep)
{
	uint32_t codepoint;
	size_t position;
	size_t end;
	size_t next;
	size_t row_start;
	size_t blank_end;
	size_t rows;
	size_t column;
	unsigned width;
	int blank;
	int error;

	/* The first row starts with the line, with no blank in it yet. */
	position = te_buffer_line_start(buffer, line);
	end = te_buffer_line_end(buffer, line);
	rows = 1U;
	row_start = position;
	blank_end = position;
	blank = 0;
	column = 0;

	/* Its start is kept, when asked for. */
	if (keep) {
		error = layout_break_room(layout, 1U);
		if (error == 0) {
			layout->breaks[0] = position;
		} else {
			keep = 0;
		}
	}

	/* Each character, starting a row where one crosses the edge. */
	while (position < end) {
		codepoint = te_buffer_char(buffer, position, &next);
		width = te_layout_cells(codepoint, (unsigned)column, layout->tab);

		/* A character that crosses the edge (a blank may hang past it) starts a row: after the row's last blank, or at itself. */
		if (column > 0U &&
		    column + width > layout->columns &&
		    codepoint != ' ') {
			row_start = position;
			if (blank)
				row_start = blank_end;
			rows++;

			/* The row's start is kept, when there is room. */
			if (keep) {
				error = layout_break_room(layout, rows);
				if (error == 0) {
					layout->breaks[rows - 1U] = row_start;
				} else {
					keep = 0;
				}
			}

			/* The characters from the row's start to here are in the new row; this one is tried again. */
			blank = 0;
			column = te_layout_columns_between(layout, buffer, row_start, position);
			continue;
		}

		/* The character joins the row; a blank is where the row may break later. */
		column += width;
		if (codepoint == ' ' || codepoint == '\t') {
			blank = 1;
			blank_end = next;
		}

		/* The next character. */
		position = next;
	}

	/* The widest line, for scrolling across when lines are not wrapped. */
	if (!layout->wrap && column > layout->widest)
		layout->widest = column;

	/* Succeeded: the rows. */
	return rows;
}

/* Tells whether a character is wide: East Asian Wide or Fullwidth, and the common emoji. */
static int
layout_wide(
	uint32_t codepoint)
{
	/* Below the Hangul Jamo nothing is wide. */
	if (codepoint < 0x1100U)
		return 0;

	/* Hangul Jamo, the CJK radicals to Yi, Hangul syllables, and CJK compatibility ideographs. */
	if (codepoint <= 0x115fU)
		return 1;
	if (codepoint >= 0x2e80U && codepoint <= 0xa4cfU && codepoint != 0x303fU)
		return 1;
	if (codepoint >= 0xac00U && codepoint <= 0xd7a3U)
		return 1;
	if (codepoint >= 0xf900U && codepoint <= 0xfaffU)
		return 1;

	/* Vertical forms, CJK compatibility forms, and the full width forms. */
	if (codepoint >= 0xfe10U && codepoint <= 0xfe19U)
		return 1;
	if (codepoint >= 0xfe30U && codepoint <= 0xfe6fU)
		return 1;
	if (codepoint >= 0xff00U && codepoint <= 0xff60U)
		return 1;
	if (codepoint >= 0xffe0U && codepoint <= 0xffe6U)
		return 1;

	/* The emoji of the pictographs, emoticons, transport and supplemental blocks. */
	if (codepoint >= 0x1f300U && codepoint <= 0x1f64fU)
		return 1;
	if (codepoint >= 0x1f680U && codepoint <= 0x1f6ffU)
		return 1;
	if (codepoint >= 0x1f900U && codepoint <= 0x1f9ffU)
		return 1;

	/* The CJK ideographs of the supplementary planes. */
	if (codepoint >= 0x20000U && codepoint <= 0x3fffdU)
		return 1;

	/* Narrow. */
	return 0;
}

/* Works out the row each line starts on, from a line whose start is known on (or the first). */
static void
layout_number(
	struct te_layout *layout,
	size_t from)
{
	size_t line;

	/* The first line starts on row 0; each later one after the one before it. */
	if (from == 0U && layout->lines > 0U)
		layout->first_row[0] = 0;
	if (from == 0U)
		from = 1U;
	for (line = from; line < layout->lines; line++)
		layout->first_row[line] = layout->first_row[line - 1U] + layout->rows[line - 1U];

	/* The total, after the last line. */
	layout->total = 0;
	if (layout->lines > 0U)
		layout->total = layout->first_row[layout->lines - 1U] + layout->rows[layout->lines - 1U];
}
