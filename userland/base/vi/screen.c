/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The characters of a line, their widths on the screen, and the drawing
 * of the buffer on the terminal.
 *
 * Text is UTF-8 whatever the locale says.  A byte that does not start a
 * valid sequence is one character of its own, shown as <HH>.
 */

#include "userland/base/vi/vi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The value vi_text_code gives for a byte that is not valid UTF-8. */
#define SCREEN_INVALID_CODE 0xffffffffUL

/* The most bytes of combining marks kept with one character. */
#define SCREEN_COMBINING_LENGTH 32U

/* The most bytes one cell of a drawn row can take. */
#define SCREEN_CELL_BYTES 48U

/*
 * The kinds of character a line holds, which differ in how they are drawn.
 */
enum unit_kind {
	UNIT_TEXT,
	UNIT_TAB,
	UNIT_CONTROL,
	UNIT_INVALID
};

/*
 * One character as the cursor moves over it: a character with the
 * combining marks after it, a tab, a control character, or a byte or
 * sequence that cannot be shown as it is.
 */
struct unit {
	enum unit_kind kind;
	size_t length;
	int width;
	unsigned long code;
};

/*
 * The walk of one line across the rows of the screen.
 *
 * logical is the column the line would reach on an endless row, which
 * places tab stops.  row and column are where the next cell goes once the
 * line is cut into rows of the screen width.
 */
struct layout {
	const char *text;
	size_t length;
	size_t offset;
	size_t logical;
	int row;
	int column;
	int columns;
};

/*
 * A row being drawn: the bytes that show it and the columns it fills.
 */
struct row_text {
	char *text;
	size_t length;
	size_t capacity;
	int width;
};

static size_t decode(const char *text, size_t length, size_t offset, unsigned long *code);
static void read_unit(const char *text, size_t length, size_t offset, size_t logical, struct unit *unit);
static int code_width(unsigned long code);
static int place(struct layout *layout, int width, int *row, int *column);
static void layout_start(struct layout *layout, const struct vi_line *line, int columns);
static int line_rows(const struct vi_editor *editor, size_t index);
static void cursor_place(const struct vi_editor *editor, int *row, int *column);
static void scroll_to_cursor(struct vi_editor *editor);
static int prepare_rows(struct vi_editor *editor);
static void row_add(struct row_text *row, const char *bytes, size_t length, int width);
static void row_add_unit(struct row_text *row, const char *text, const struct unit *unit, int piece);
static void draw_line(const struct vi_editor *editor, size_t index, int skip, struct row_text *rows, int first, int limit);
static void draw_status(const struct vi_editor *editor, struct row_text *row);
static void show_row(struct vi_editor *editor, int index, const struct row_text *row);

/*
 * Finds where the character after the one at an offset starts.
 */
size_t
vi_text_next(
	const char *text,
	size_t length,
	size_t offset)
{
	struct unit unit;

	/* The end of the line has no next character. */
	if (offset >= length)
		return length;

	/* Steps over the character and its combining marks. */
	read_unit(text, length, offset, 0, &unit);

	/* Succeeded: the offset of the next character. */
	return offset + unit.length;
}

/*
 * Finds where the character before an offset starts.
 *
 * UTF-8 cannot always be read backward when invalid bytes are about, so
 * the line is walked from its start.
 */
size_t
vi_text_previous(
	const char *text,
	size_t length,
	size_t offset)
{
	size_t current;
	size_t next;

	/* Nothing comes before the start of the line. */
	if (offset == 0)
		return 0;

	/* Walks the characters until the one that reaches the offset. */
	current = 0;
	for (;;) {
		next = vi_text_next(text, length, current);
		if (next >= offset)
			break;
		if (next == current)
			break;
		current = next;
	}

	/* Succeeded: the start of the character before the offset. */
	return current;
}

/*
 * Gives the code point of the character at an offset, or 0xffffffff for a
 * byte that is not valid UTF-8.
 */
unsigned long
vi_text_code(
	const char *text,
	size_t length,
	size_t offset)
{
	unsigned long code;
	size_t used;

	/* The end of the line reads as nothing. */
	if (offset >= length)
		return 0;

	/* Decodes the sequence at the offset. */
	used = decode(text, length, offset, &code);
	if (used == 0)
		return SCREEN_INVALID_CODE;

	/* Succeeded: the code point. */
	return code;
}

/*
 * Gives the column at which the character at an offset starts, as if the
 * screen were endlessly wide.
 */
size_t
vi_text_column(
	const char *text,
	size_t length,
	size_t offset)
{
	struct unit unit;
	size_t current;
	size_t column;

	/* Adds up the widths of the characters before the offset. */
	current = 0;
	column = 0;
	while (current < offset) {
		if (current >= length)
			break;
		read_unit(text, length, current, column, &unit);
		current += unit.length;
		column += (size_t)unit.width;
	}

	/* Succeeded: the column. */
	return column;
}

/*
 * Finds the character that covers a column, as if the screen were
 * endlessly wide.  A column past the end of the line gives the line's
 * length.
 */
size_t
vi_text_offset_at_column(
	const char *text,
	size_t length,
	size_t column)
{
	struct unit unit;
	size_t current;
	size_t reached;

	/* Walks until the character whose cells include the column. */
	current = 0;
	reached = 0;
	while (current < length) {
		read_unit(text, length, current, reached, &unit);
		if (reached + (size_t)unit.width > column)
			return current;
		reached += (size_t)unit.width;
		current += unit.length;
	}

	/* The column lies past the end of the line. */
	return length;
}

/*
 * Forgets what the screen shows, so that the next update draws all of it.
 */
void
vi_screen_invalidate(
	struct vi_editor *editor)
{
	int index;

	/* Marks every remembered row as unknown. */
	for (index = 0; index < editor->screen_rows; index++)
		editor->screen[index].valid = 0;
}

/*
 * Releases the remembered rows of the screen.
 */
void
vi_screen_free(
	struct vi_editor *editor)
{
	int index;

	/* Releases the text of each row, then the row table. */
	for (index = 0; index < editor->screen_rows; index++)
		free(editor->screen[index].text);
	free(editor->screen);
	editor->screen = NULL;
	editor->screen_rows = 0;
}

/*
 * Brings the terminal up to date with the buffer, the cursor and the
 * bottom line.
 *
 * Only rows that differ from what was last drawn are written.  When
 * memory runs short the screen is left as it was.
 */
void
vi_screen_update(
	struct vi_editor *editor)
{
	struct row_text *rows;
	int text_rows;
	int row;
	int column;
	int index;
	int status;
	size_t line;
	int skip;
	int used;
	int needed;

	/* Makes the row table match the window, clearing the screen when it changed. */
	status = prepare_rows(editor);
	if (status != 0)
		return;

	/* Scrolls so that the cursor is on the screen. */
	text_rows = editor->terminal.rows - 1;
	scroll_to_cursor(editor);

	/* Makes the rows the new screen is drawn into. */
	rows = calloc((size_t)editor->terminal.rows, sizeof(*rows));
	if (rows == NULL)
		return;

	/* Draws the lines from the top of the screen down. */
	line = editor->top;
	skip = (int)editor->top_skip;
	used = 0;
	editor->bottom = editor->top;
	while (used < text_rows) {
		/* The end of the buffer ends the drawing. */
		if (line >= editor->buffer.count)
			break;

		/* A line that does not fit below the others is shown as @ rows. */
		needed = line_rows(editor, line) - skip;
		if (needed > text_rows - used) {
			/* The top line may be drawn in part; any other line that does not fit is shown as @ rows. */
			if (line != editor->top) {
				for (index = used; index < text_rows; index++)
					row_add(&rows[index], "@", 1, 1);
				used = text_rows;
				break;
			}
		}

		/* Draws the line's rows that fall on the screen. */
		draw_line(editor, line, skip, rows, used, text_rows);
		used += needed;

		/* Remembers the last line drawn whole, for H, L and the page keys. */
		if (used <= text_rows)
			editor->bottom = line;
		line++;
		skip = 0;
	}

	/* Rows below the end of the buffer show ~. */
	for (index = used; index < text_rows; index++)
		row_add(&rows[index], "~", 1, 1);

	/* The last row shows the message, the typed : line or the mode. */
	draw_status(editor, &rows[text_rows]);

	/* Writes the rows that changed, with the cursor hidden meanwhile. */
	vi_terminal_command(&editor->terminal, editor->terminal.hide_cursor_string);
	for (index = 0; index < editor->terminal.rows; index++)
		show_row(editor, index, &rows[index]);

	/* Puts the cursor where the next key acts. */
	if (editor->mode == VI_MODE_LINE) {
		row = text_rows;
		column = rows[text_rows].width;
		if (column >= editor->terminal.columns)
			column = editor->terminal.columns - 1;
	} else {
		cursor_place(editor, &row, &column);
	}

	/* Shows the cursor at the place found, and sends all that was collected. */
	vi_terminal_move(&editor->terminal, row, column);
	vi_terminal_command(&editor->terminal, editor->terminal.show_cursor_string);
	vi_terminal_flush(&editor->terminal);

	/* Releases the drawn rows. */
	for (index = 0; index < editor->terminal.rows; index++)
		free(rows[index].text);
	free(rows);
}

/*
 * Scrolls so that the cursor's line is near the middle of the screen,
 * for a jump that lands far from what was shown.
 */
void
vi_screen_center(
	struct vi_editor *editor)
{
	int text_rows;
	int above;
	int rows;
	int cursor_row;
	int cursor_column;

	/* The last row is the status line; the rest show text. */
	text_rows = editor->terminal.rows - 1;

	/* Starts with the cursor's line at the top. */
	editor->top = editor->cursor.line;
	editor->top_skip = 0;

	/* A line taller than the screen shows the part with the cursor. */
	cursor_place(editor, &cursor_row, &cursor_column);
	if (cursor_row >= text_rows) {
		editor->top_skip = (size_t)(cursor_row - text_rows + 1);
		return;
	}

	/* Brings earlier lines in until they fill half of the screen. */
	above = 0;
	while (editor->top > 0) {
		rows = line_rows(editor, editor->top - 1);
		if (above + rows > text_rows / 2)
			break;
		above += rows;
		editor->top--;
	}
}

/*
 * Decodes the UTF-8 sequence at an offset.  Returns its length with the
 * code point, or 0 when the byte there does not start a valid sequence.
 */
static size_t
decode(
	const char *text,
	size_t length,
	size_t offset,
	unsigned long *code)
{
	const unsigned char *bytes;
	unsigned long value;
	unsigned long minimum;
	size_t needed;
	size_t index;

	/* Reads the bytes from the offset on. */
	bytes = (const unsigned char *)text + offset;

	/* One byte below 0x80 is a character of its own. */
	if (bytes[0] < 0x80) {
		*code = bytes[0];
		return 1;
	}

	/* The first byte tells how many bytes follow and what the smallest valid value is. */
	if (bytes[0] < 0xc2) {
		return 0;
	} else if (bytes[0] < 0xe0) {
		needed = 2;
		value = bytes[0] & 0x1fUL;
		minimum = 0x80;
	} else if (bytes[0] < 0xf0) {
		needed = 3;
		value = bytes[0] & 0x0fUL;
		minimum = 0x800;
	} else if (bytes[0] < 0xf5) {
		needed = 4;
		value = bytes[0] & 0x07UL;
		minimum = 0x10000;
	} else {
		return 0;
	}

	/* A sequence cut short by the end of the line is invalid. */
	if (needed > length - offset)
		return 0;

	/* Each following byte must be a continuation byte. */
	for (index = 1; index < needed; index++) {
		if ((bytes[index] & 0xc0) != 0x80)
			return 0;
		value = (value << 6) | (bytes[index] & 0x3fUL);
	}

	/* Refuses an overlong form, a surrogate, and a value past Unicode. */
	if (value < minimum)
		return 0;
	if (value >= 0xd800 && value <= 0xdfff)
		return 0;
	if (value > 0x10ffff)
		return 0;

	/* Tells the caller the code point. */
	*code = value;

	/* Succeeded: the length of the sequence. */
	return needed;
}

/*
 * Reads the character at an offset: its kind, its length with any
 * combining marks after it, and its width at the given endless column.
 */
static void
read_unit(
	const char *text,
	size_t length,
	size_t offset,
	size_t logical,
	struct unit *unit)
{
	unsigned long code;
	unsigned long mark;
	size_t used;
	size_t next;
	int width;
	int mark_width;

	/* Decodes the first character. */
	used = decode(text, length, offset, &code);
	unit->code = code;

	/* A byte that is not valid UTF-8 is shown as <HH>. */
	if (used == 0) {
		unit->kind = UNIT_INVALID;
		unit->length = 1;
		unit->width = 4;
		unit->code = SCREEN_INVALID_CODE;
		return;
	}

	/* A tab reaches the next tab stop. */
	if (code == '\t') {
		unit->kind = UNIT_TAB;
		unit->length = 1;
		unit->width = (int)(VI_TAB_WIDTH - logical % VI_TAB_WIDTH);
		return;
	}

	/* An ASCII control character is shown as ^X. */
	if (code < 0x20 || code == 0x7f) {
		unit->kind = UNIT_CONTROL;
		unit->length = 1;
		unit->width = 2;
		return;
	}

	/* A character the terminal cannot show, such as a C1 control, shows its bytes. */
	width = code_width(code);
	if (width < 0) {
		unit->kind = UNIT_INVALID;
		unit->length = used;
		unit->width = 4 * (int)used;
		return;
	}

	/* A combining mark with nothing before it is shown on a space. */
	if (width == 0)
		width = 1;

	/* Takes the combining marks that follow into the same character. */
	next = offset + used;
	while (next < length) {
		used = decode(text, length, next, &mark);
		if (used == 0)
			break;
		if (mark < 0x300)
			break;
		mark_width = code_width(mark);
		if (mark_width != 0)
			break;
		if (next + used - offset > SCREEN_COMBINING_LENGTH)
			break;
		next += used;
	}

	/* The character with its combining marks is one unit as wide as its base. */
	unit->kind = UNIT_TEXT;
	unit->length = next - offset;
	unit->width = width;
}

/* Gives the columns a code point takes on the terminal, or -1 if it cannot be shown. */
static int
code_width(
	unsigned long code)
{
	int width;

	/* The C library knows the widths, CJK being two columns. */
	width = wcwidth((wchar_t)code);

	/* Succeeded: the width. */
	return width;
}

/*
 * Places a cell of the given width on the rows of a line, starting a new
 * row when it does not fit.  Returns the columns left blank at the end
 * of the row it skipped, which the drawing fills with spaces.
 */
static int
place(
	struct layout *layout,
	int width,
	int *row,
	int *column)
{
	int skipped;

	/* A cell that would cross the right edge starts the next row. */
	skipped = 0;
	if (layout->column + width > layout->columns) {
		skipped = layout->columns - layout->column;
		layout->row++;
		layout->column = 0;
	}

	/* Tells the caller the cell's place, then moves past the cell. */
	*row = layout->row;
	*column = layout->column;
	layout->column += width;

	/* Succeeded: the blank columns left behind. */
	return skipped;
}

/* Starts the walk of a line across rows of the given width. */
static void
layout_start(
	struct layout *layout,
	const struct vi_line *line,
	int columns)
{
	/* The walk starts at the first cell of the first row. */
	memset(layout, 0, sizeof(*layout));
	layout->text = line->text;
	layout->length = line->length;
	layout->columns = columns;
}

/* Counts the rows a line takes on the screen. */
static int
line_rows(
	const struct vi_editor *editor,
	size_t index)
{
	struct layout layout;
	struct unit unit;
	int row;
	int column;
	int piece;
	int pieces;
	int width;

	/* Walks the characters of the line, placing each of their cells. */
	layout_start(&layout, &editor->buffer.lines[index], editor->terminal.columns);
	while (layout.offset < layout.length) {
		read_unit(layout.text, layout.length, layout.offset, layout.logical, &unit);

		/* A tab is spaces, and an unshowable sequence is one <HH> per byte. */
		pieces = 1;
		width = unit.width;
		if (unit.kind == UNIT_TAB) {
			pieces = unit.width;
			width = 1;
		} else if (unit.kind == UNIT_INVALID) {
			pieces = (int)unit.length;
			width = 4;
		}

		/* Places each cell of the character. */
		for (piece = 0; piece < pieces; piece++)
			(void)place(&layout, width, &row, &column);

		/* Moves on to the next character. */
		layout.offset += unit.length;
		layout.logical += (size_t)unit.width;
	}

	/* The rows used, which is one for an empty line. */
	return layout.row + 1;
}

/* Finds the row, counted from the top of the screen, and the column of the cursor. */
static void
cursor_place(
	const struct vi_editor *editor,
	int *row,
	int *column)
{
	struct layout layout;
	struct unit unit;
	size_t line;
	int above;
	int piece;
	int pieces;
	int width;
	int cell_row;
	int cell_column;

	/* Adds up the rows of the lines above the cursor's line. */
	above = -(int)editor->top_skip;
	for (line = editor->top; line < editor->cursor.line; line++)
		above += line_rows(editor, line);

	/* Walks the cursor's line up to the cursor. */
	layout_start(&layout, &editor->buffer.lines[editor->cursor.line], editor->terminal.columns);
	while (layout.offset < editor->cursor.offset) {
		if (layout.offset >= layout.length)
			break;

		/* Reads the character and places each of its cells. */
		read_unit(layout.text, layout.length, layout.offset, layout.logical, &unit);
		pieces = 1;
		width = unit.width;
		if (unit.kind == UNIT_TAB) {
			pieces = unit.width;
			width = 1;
		} else if (unit.kind == UNIT_INVALID) {
			pieces = (int)unit.length;
			width = 4;
		}

		/* Places each cell of the character. */
		for (piece = 0; piece < pieces; piece++)
			(void)place(&layout, width, &cell_row, &cell_column);

		/* Moves on to the next character. */
		layout.offset += unit.length;
		layout.logical += (size_t)unit.width;
	}

	/* The cursor sits on the first cell of its character, which may start a new row. */
	width = 1;
	if (layout.offset < layout.length) {
		read_unit(layout.text, layout.length, layout.offset, layout.logical, &unit);
		width = unit.width;
		if (unit.kind == UNIT_TAB)
			width = 1;
		else if (unit.kind == UNIT_INVALID)
			width = 4;
		(void)place(&layout, width, &cell_row, &cell_column);
	} else if (layout.column >= layout.columns) {
		/* At the end of a full row the cursor stays on its last column. */
		cell_row = layout.row;
		cell_column = layout.columns - 1;
	} else {
		cell_row = layout.row;
		cell_column = layout.column;
	}

	/* Tells the caller the cursor's place on the screen. */
	*row = above + cell_row;
	*column = cell_column;
}

/* Moves the top of the screen so that the cursor is on one of the text rows. */
static void
scroll_to_cursor(
	struct vi_editor *editor)
{
	int text_rows;
	int row;
	int column;
	int rows;

	/* The last row is the status line; the rest show text. */
	text_rows = editor->terminal.rows - 1;

	/* A cursor above the screen: a near one scrolls back, a far one is centered. */
	if (editor->cursor.line < editor->top) {
		if (editor->top - editor->cursor.line > (size_t)text_rows) {
			vi_screen_center(editor);
			return;
		}

		/* Scrolls back so that the cursor's line is the top one. */
		editor->top = editor->cursor.line;
		editor->top_skip = 0;
	}

	/* A cursor far below the screen is centered rather than scrolled to. */
	if (editor->cursor.line - editor->top > (size_t)text_rows) {
		vi_screen_center(editor);
		return;
	}

	/* A cursor in the hidden upper part of the top line scrolls back within it. */
	cursor_place(editor, &row, &column);
	if (row < 0) {
		editor->top_skip = (size_t)((int)editor->top_skip + row);
		return;
	}

	/* A cursor below the screen scrolls the screen forward a row at a time. */
	while (row >= text_rows) {
		if (editor->top == editor->cursor.line) {
			/* Only the cursor's own line is left; hides more of its upper part. */
			editor->top_skip += (size_t)(row - text_rows + 1);
			break;
		}

		/* Takes the top line off the screen. */
		rows = line_rows(editor, editor->top) - (int)editor->top_skip;
		row -= rows;
		editor->top++;
		editor->top_skip = 0;
	}
}

/*
 * Makes the table of remembered rows match the window size.  A changed
 * size clears the terminal and forgets every row.  Returns 0, or -1 when
 * memory runs short.
 */
static int
prepare_rows(
	struct vi_editor *editor)
{
	struct vi_screen_row *rows;

	/* A table of the right size is kept. */
	if (editor->screen_rows == editor->terminal.rows)
		return 0;

	/* Makes a new table, then drops the old one. */
	rows = calloc((size_t)editor->terminal.rows, sizeof(*rows));
	if (rows == NULL)
		return -1;

	/* Drops the old table and keeps the new one. */
	vi_screen_free(editor);
	editor->screen = rows;
	editor->screen_rows = editor->terminal.rows;

	/* The old contents of the terminal no longer line up with any row. */
	vi_terminal_command(&editor->terminal, editor->terminal.clear_string);

	/* Succeeded. */
	return 0;
}

/* Adds bytes that fill the given columns to a row being drawn. */
static void
row_add(
	struct row_text *row,
	const char *bytes,
	size_t length,
	int width)
{
	char *grown;
	size_t capacity;

	/* Grows the row's storage when the bytes do not fit. */
	if (row->length + length > row->capacity) {
		capacity = row->capacity * 2 + length + 64;
		grown = realloc(row->text, capacity);
		if (grown == NULL)
			return;
		row->text = grown;
		row->capacity = capacity;
	}

	/* Adds the bytes and counts their columns. */
	memcpy(row->text + row->length, bytes, length);
	row->length += length;
	row->width += width;
}

/*
 * Adds one cell of a character to a row being drawn: the character
 * itself, one space of a tab, ^X for a control character, or <HH> for
 * one byte of an unshowable sequence.
 */
static void
row_add_unit(
	struct row_text *row,
	const char *text,
	const struct unit *unit,
	int piece)
{
	char cell[SCREEN_CELL_BYTES];
	int length;
	int base_width;

	/* Chooses what the cell shows by the kind of character. */
	switch (unit->kind) {
	case UNIT_TAB:
		row_add(row, " ", 1, 1);
		break;
	case UNIT_CONTROL:
		cell[0] = '^';
		cell[1] = (char)(unit->code ^ 0x40);
		row_add(row, cell, 2, 2);
		break;
	case UNIT_INVALID:
		length = snprintf(cell, sizeof(cell), "<%02X>", (unsigned char)text[piece]);
		row_add(row, cell, (size_t)length, 4);
		break;
	case UNIT_TEXT:
		/* A combining mark that starts the line is drawn on a space. */
		base_width = code_width(unit->code);
		if (base_width == 0)
			row_add(row, " ", 1, 0);
		row_add(row, text, unit->length, unit->width);
		break;
	}
}

/*
 * Draws the rows of one line onto the screen rows from first on, leaving
 * out the first skip rows of the line and stopping at the limit.
 */
static void
draw_line(
	const struct vi_editor *editor,
	size_t index,
	int skip,
	struct row_text *rows,
	int first,
	int limit)
{
	struct layout layout;
	struct unit unit;
	int piece;
	int pieces;
	int width;
	int row;
	int column;
	int skipped;
	int target;

	/* Walks the characters of the line, drawing each cell on its row. */
	layout_start(&layout, &editor->buffer.lines[index], editor->terminal.columns);
	while (layout.offset < layout.length) {
		read_unit(layout.text, layout.length, layout.offset, layout.logical, &unit);
		pieces = 1;
		width = unit.width;
		if (unit.kind == UNIT_TAB) {
			pieces = unit.width;
			width = 1;
		} else if (unit.kind == UNIT_INVALID) {
			pieces = (int)unit.length;
			width = 4;
		}

		/* Draws each cell, filling the end of a row a wide cell did not fit on. */
		for (piece = 0; piece < pieces; piece++) {
			skipped = place(&layout, width, &row, &column);
			target = first + row - skip;
			if (skipped > 0) {
				if (target - 1 >= first && target - 1 < limit) {
					while (skipped > 0) {
						row_add(&rows[target - 1], " ", 1, 1);
						skipped--;
					}
				}
			}

			/* Draws the cell when its row is one of those on the screen. */
			if (target >= first && target < limit)
				row_add_unit(&rows[target], layout.text + layout.offset, &unit, piece);
		}

		/* Moves on to the next character. */
		layout.offset += unit.length;
		layout.logical += (size_t)unit.width;
	}
}

/* Draws the bottom line: the typed : or / line, a message, or the mode. */
static void
draw_status(
	const struct vi_editor *editor,
	struct row_text *row)
{
	const char *text;
	size_t length;
	size_t start;
	struct unit unit;
	size_t offset;
	size_t shown;
	int limit;
	char prompt[1];

	/* The last column is left empty so the terminal never scrolls. */
	limit = editor->terminal.columns - 1;

	/* Chooses the text of the line. */
	text = "";
	start = 0;
	if (editor->mode == VI_MODE_LINE) {
		prompt[0] = editor->input_prompt;
		row_add(row, prompt, 1, 1);
		text = editor->input;
		length = editor->input_length;

		/* A typed line wider than the screen shows its end. */
		for (;;) {
			shown = vi_text_column(text + start, length - start, length - start);
			if (shown + 1 <= (size_t)limit)
				break;
			start = vi_text_next(text, length, start);
		}
	} else if (editor->message[0] != '\0') {
		text = editor->message;
	} else if (editor->mode == VI_MODE_INSERT) {
		text = "-- INSERT --";
	}

	/* Draws the characters that fit. */
	length = strlen(text);
	if (editor->mode == VI_MODE_LINE)
		length = editor->input_length;
	offset = start;
	while (offset < length) {
		read_unit(text, length, offset, (size_t)row->width, &unit);
		if (row->width + unit.width > limit)
			break;
		row_add_unit(row, text + offset, &unit, 0);
		offset += unit.length;
	}
}

/* Writes one row to the terminal if it differs from what the terminal shows. */
static void
show_row(
	struct vi_editor *editor,
	int index,
	const struct row_text *row)
{
	struct vi_screen_row *shown;
	char *copy;
	int same;
	int difference;

	/* Finds what the terminal was last told this row shows. */
	shown = &editor->screen[index];

	/* A row that already shows the same text is left alone. */
	same = 0;
	if (shown->valid && shown->length == row->length) {
		difference = 0;
		if (row->length != 0)
			difference = memcmp(shown->text, row->text, row->length);
		if (difference == 0)
			same = 1;
	}

	/* Skips the writing of a row that is already right. */
	if (same)
		return;

	/*
	 * Writes the row.  A row that fills the width is not cleared after,
	 * since clearing at the right edge would erase its last cell.
	 */
	vi_terminal_move(&editor->terminal, index, 0);
	if (row->length != 0)
		vi_terminal_put(&editor->terminal, row->text, row->length);
	if (row->width < editor->terminal.columns)
		vi_terminal_command(&editor->terminal, editor->terminal.clear_line_string);

	/* Remembers what the row now shows; without memory it is redrawn next time. */
	copy = malloc(row->length + 1);
	if (copy == NULL) {
		shown->valid = 0;
		return;
	}

	/* Keeps the copy as what the row shows. */
	if (row->length != 0)
		memcpy(copy, row->text, row->length);
	free(shown->text);
	shown->text = copy;
	shown->length = row->length;
	shown->width = row->width;
	shown->valid = 1;
}
