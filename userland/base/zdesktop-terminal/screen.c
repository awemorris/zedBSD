/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The character grid of zdesktop-terminal and the VT100 interpreter that
 * fills it from what the shell writes.
 *
 * It started as zterm's (userland/X11/zterm) and takes the same subset:
 * cursor motion, erasing, colours, UTF-8; with the scrolling region, line
 * and character insertion and deletion, the cursor's visibility, 256 and
 * direct colours, and OSC strings (which are read and dropped) added for
 * the programs a shell runs.
 */

#include "terminal.h"

#include <string.h>

/* The parser's states. */
#define SCREEN_TEXT		0
#define SCREEN_ESCAPE		1
#define SCREEN_CSI		2
#define SCREEN_OSC		3
#define SCREEN_OSC_ESCAPE	4
#define SCREEN_CHARSET		5

/*
 * The 16 colours of the SGR sequences 30-37 and 90-97, as 0xRRGGBB.
 *
 * They are the xterm defaults, a little softened for the dark background.
 */
static const uint32_t screen_palette[16] = {
	0x2e3440U, 0xd0605aU, 0x8fbf6aU, 0xe0b860U,
	0x6a93d8U, 0xb888c8U, 0x6ac0c8U, 0xdcdfe6U,
	0x5c6478U, 0xf07a72U, 0xa8d886U, 0xf2cf7aU,
	0x88aef0U, 0xd0a4e0U, 0x88dae0U, 0xffffffU
};

static void screen_blank(struct terminal_screen *screen, unsigned column, unsigned row);
static void screen_erase(struct terminal_screen *screen, unsigned row, unsigned first, unsigned last);
static void screen_byte(struct terminal_screen *screen, unsigned char byte);
static void screen_escape(struct terminal_screen *screen, unsigned char byte);
static void screen_csi_byte(struct terminal_screen *screen, unsigned char byte);
static void screen_csi(struct terminal_screen *screen, unsigned char final);
static void screen_sgr(struct terminal_screen *screen);
static uint32_t screen_colour_256(int index);
static void screen_mode(struct terminal_screen *screen, int set);
static int screen_parameter(const struct terminal_screen *screen, int index, int fallback);
static void screen_line_feed(struct terminal_screen *screen);
static void screen_reverse_index(struct terminal_screen *screen);
static void screen_scroll_up(struct terminal_screen *screen, unsigned top, unsigned bottom, unsigned count);
static void screen_scroll_down(struct terminal_screen *screen, unsigned top, unsigned bottom, unsigned count);
static void screen_utf8(struct terminal_screen *screen, unsigned char byte);
static void screen_put(struct terminal_screen *screen, uint32_t codepoint);
static int screen_wide(uint32_t codepoint);
static void screen_move(struct terminal_screen *screen, unsigned column, unsigned row);

/*
 * Starts an empty grid of the given size with the cursor at the top left.
 */
void
terminal_screen_init(
	struct terminal_screen *screen,
	unsigned columns,
	unsigned rows)
{
	unsigned row;

	/* The whole state starts from zero: text, no parameters, the cursor at the origin. */
	memset(screen, 0, sizeof(*screen));

	/* The grid's size, bounded by what the grid holds. */
	if (columns == 0U)
		columns = 1U;
	if (rows == 0U)
		rows = 1U;
	if (columns > TERMINAL_MAX_COLUMNS)
		columns = TERMINAL_MAX_COLUMNS;
	if (rows > TERMINAL_MAX_ROWS)
		rows = TERMINAL_MAX_ROWS;
	screen->columns = columns;
	screen->rows = rows;

	/* The default colours, a visible cursor and the whole grid scrolling. */
	screen->foreground = TERMINAL_FOREGROUND;
	screen->background = TERMINAL_BACKGROUND;
	screen->cursor_visible = 1;
	screen->scroll_top = 0U;
	screen->scroll_bottom = rows - 1U;

	/* Every cell blank. */
	for (row = 0U; row < rows; row++)
		screen_erase(screen, row, 0U, columns - 1U);

	/* The first frame shows the empty grid. */
	screen->changed = 1;
}

/*
 * Gives the grid a new size, keeping the cells at the top left that still fit.
 */
void
terminal_screen_resize(
	struct terminal_screen *screen,
	unsigned columns,
	unsigned rows)
{
	static struct terminal_cell old[TERMINAL_MAX_COLUMNS * TERMINAL_MAX_ROWS];
	unsigned old_columns;
	unsigned old_rows;
	unsigned copy_columns;
	unsigned copy_rows;
	unsigned row;

	/* The new size, bounded like the first. */
	if (columns == 0U)
		columns = 1U;
	if (rows == 0U)
		rows = 1U;
	if (columns > TERMINAL_MAX_COLUMNS)
		columns = TERMINAL_MAX_COLUMNS;
	if (rows > TERMINAL_MAX_ROWS)
		rows = TERMINAL_MAX_ROWS;

	/* An unchanged size keeps everything. */
	if (columns == screen->columns && rows == screen->rows)
		return;

	/* Keeps the old cells aside while the grid is laid out again. */
	old_columns = screen->columns;
	old_rows = screen->rows;
	memcpy(old, screen->cells, (size_t)old_columns * old_rows * sizeof(old[0]));

	/* Blanks the new grid. */
	screen->columns = columns;
	screen->rows = rows;
	for (row = 0U; row < rows; row++)
		screen_erase(screen, row, 0U, columns - 1U);

	/* Copies back the part of the old grid that fits, row by row. */
	copy_columns = columns;
	if (old_columns < copy_columns)
		copy_columns = old_columns;
	copy_rows = rows;
	if (old_rows < copy_rows)
		copy_rows = old_rows;
	for (row = 0U; row < copy_rows; row++)
		memcpy(&screen->cells[row * columns], &old[row * old_columns], (size_t)copy_columns * sizeof(old[0]));

	/* The cursor and the scrolling region stay inside the grid. */
	if (screen->cursor_column >= columns)
		screen->cursor_column = columns - 1U;
	if (screen->cursor_row >= rows)
		screen->cursor_row = rows - 1U;
	screen->scroll_top = 0U;
	screen->scroll_bottom = rows - 1U;

	/* The resized grid is drawn anew. */
	screen->changed = 1;
}

/*
 * Interprets bytes the shell wrote.
 */
void
terminal_screen_write(
	struct terminal_screen *screen,
	const unsigned char *bytes,
	size_t length)
{
	size_t index;

	/* Each byte in order: a control, part of a sequence or part of a character. */
	for (index = 0U; index < length; index++)
		screen_byte(screen, bytes[index]);

	/* Anything written may have changed the grid. */
	if (length != 0U)
		screen->changed = 1;
}

/*
 * Returns the cell at a column and a row, which must be inside the grid.
 */
struct terminal_cell *
terminal_screen_cell(
	struct terminal_screen *screen,
	unsigned column,
	unsigned row)
{
	/* The cells are stored row after row. */
	return &screen->cells[row * screen->columns + column];
}

/* Makes one cell blank in the current background. */
static void
screen_blank(
	struct terminal_screen *screen,
	unsigned column,
	unsigned row)
{
	struct terminal_cell *cell;

	/* A space in the colours new characters get. */
	cell = terminal_screen_cell(screen, column, row);
	cell->codepoint = ' ';
	cell->foreground = screen->foreground;
	cell->background = screen->background;
	cell->continuation = 0;
}

/* Blanks the cells of a row from first to last, inclusive and clipped to the grid. */
static void
screen_erase(
	struct terminal_screen *screen,
	unsigned row,
	unsigned first,
	unsigned last)
{
	unsigned column;

	/* A row or a start outside the grid has nothing to erase. */
	if (row >= screen->rows || first >= screen->columns)
		return;

	/* The range ends at the grid's right edge. */
	if (last >= screen->columns)
		last = screen->columns - 1U;

	/* Blanks each cell of the range. */
	for (column = first; column <= last; column++)
		screen_blank(screen, column, row);
}

/* Interprets one byte in the parser's current state. */
static void
screen_byte(
	struct terminal_screen *screen,
	unsigned char byte)
{
	/* Routes the byte by the parser's state. */
	switch (screen->parser_state) {
	case SCREEN_ESCAPE:
		screen_escape(screen, byte);
		return;
	case SCREEN_CSI:
		screen_csi_byte(screen, byte);
		return;
	case SCREEN_OSC:
		/* An OSC string (a window title, a colour) ends with BEL or ESC \ and is dropped. */
		if (byte == 0x07U) {
			screen->parser_state = SCREEN_TEXT;
		} else if (byte == 0x1bU) {
			screen->parser_state = SCREEN_OSC_ESCAPE;
		}

		/* Every other byte of the string is dropped. */
		return;
	case SCREEN_OSC_ESCAPE:
		/* The byte after ESC ends the string whatever it is. */
		screen->parser_state = SCREEN_TEXT;
		return;
	case SCREEN_CHARSET:
		/* The character set a G0 or G1 designation names is ignored: UTF-8 is the only one. */
		screen->parser_state = SCREEN_TEXT;
		return;
	default:
		break;
	}

	/* Text: the C0 controls first, then the bytes of characters. */
	if (byte == 0x1bU) {
		screen->utf8_remaining = 0U;
		screen->parser_state = SCREEN_ESCAPE;
	} else if (byte == '\r') {
		screen->cursor_column = 0U;
	} else if (byte == '\n' || byte == 0x0bU || byte == 0x0cU) {
		screen_line_feed(screen);
	} else if (byte == '\b') {
		/* Backspace stops at the left edge. */
		if (screen->cursor_column != 0U)
			screen->cursor_column--;
	} else if (byte == '\t') {
		/* A tab moves to the next multiple of eight, or the last column. */
		screen->cursor_column = (screen->cursor_column + 8U) & ~7U;
		if (screen->cursor_column >= screen->columns)
			screen->cursor_column = screen->columns - 1U;
	} else if (byte >= 0x20U) {
		screen_utf8(screen, byte);
	}
}

/* Interprets the byte after ESC. */
static void
screen_escape(
	struct terminal_screen *screen,
	unsigned char byte)
{
	int index;

	/* Most escapes are one byte long and return to text. */
	screen->parser_state = SCREEN_TEXT;

	/* Routes the escape by its byte. */
	switch (byte) {
	case '[':
		/* A control sequence: its parameters start empty. */
		screen->parser_state = SCREEN_CSI;
		screen->parameter_count = 0;
		screen->private_mode = 0;
		for (index = 0; index < TERMINAL_PARAMETERS; index++)
			screen->parameters[index] = -1;
		break;
	case ']':
		screen->parser_state = SCREEN_OSC;
		break;
	case '(':
	case ')':
		screen->parser_state = SCREEN_CHARSET;
		break;
	case '7':
		screen->saved_column = screen->cursor_column;
		screen->saved_row = screen->cursor_row;
		break;
	case '8':
		screen_move(screen, screen->saved_column, screen->saved_row);
		break;
	case 'D':
		screen_line_feed(screen);
		break;
	case 'E':
		screen->cursor_column = 0U;
		screen_line_feed(screen);
		break;
	case 'M':
		screen_reverse_index(screen);
		break;
	case 'c':
		/* A full reset: default colours, an empty grid, the cursor home. */
		terminal_screen_init(screen, screen->columns, screen->rows);
		break;
	default:
		break;
	}
}

/* Collects one byte of a control sequence, running it at its final byte. */
static void
screen_csi_byte(
	struct terminal_screen *screen,
	unsigned char byte)
{
	int *value;

	/* A digit adds to the current parameter. */
	if (byte >= '0' && byte <= '9') {
		/* The first digit starts the parameter; a parameter past the kept ones is dropped. */
		if (screen->parameter_count >= TERMINAL_PARAMETERS)
			return;
		value = &screen->parameters[screen->parameter_count];
		if (*value < 0)
			*value = 0;
		if (*value < 100000)
			*value = *value * 10 + (byte - '0');
		return;
	}

	/* A separator starts the next parameter (a colon, as in 38:2:r:g:b, counts as one). */
	if (byte == ';' || byte == ':') {
		if (screen->parameter_count < TERMINAL_PARAMETERS)
			screen->parameter_count++;
		return;
	}

	/* A private marker (?, >, =) changes what the final byte means. */
	if (byte == '?' || byte == '>' || byte == '=') {
		screen->private_mode = byte;
		return;
	}

	/* Intermediate bytes (space to /) are accepted and ignored. */
	if (byte >= 0x20U && byte <= 0x2fU)
		return;

	/* A control byte inside a sequence acts as it does in text. */
	if (byte < 0x20U) {
		/* ESC abandons the sequence and starts a new escape. */
		if (byte == 0x1bU) {
			screen->parser_state = SCREEN_ESCAPE;
			return;
		}

		/* Any other control runs as text would run it, and the sequence goes on. */
		screen->parser_state = SCREEN_TEXT;
		screen_byte(screen, byte);
		screen->parser_state = SCREEN_CSI;
		return;
	}

	/* The final byte: the last parameter counts, and the sequence runs. */
	if (screen->parameter_count < TERMINAL_PARAMETERS)
		screen->parameter_count++;
	screen->parser_state = SCREEN_TEXT;
	screen_csi(screen, byte);
}

/* Runs a complete control sequence. */
static void
screen_csi(
	struct terminal_screen *screen,
	unsigned char final)
{
	unsigned count;
	unsigned row;
	unsigned column;
	unsigned last;
	int mode;

	/* The modes (h, l) are the only private sequences taken; any other private one is ignored. */
	if (screen->private_mode != 0 && final != 'h' && final != 'l')
		return;

	/* Most sequences take a count that defaults to one. */
	count = (unsigned)screen_parameter(screen, 0, 1);
	if (count == 0U)
		count = 1U;

	/* Runs the sequence the final byte names; the others are ignored. */
	switch (final) {
	case 'A':
		/* Up, stopping at the top. */
		if (count > screen->cursor_row)
			count = screen->cursor_row;
		screen->cursor_row -= count;
		break;
	case 'B':
	case 'e':
		/* Down, stopping at the bottom. */
		screen_move(screen, screen->cursor_column, screen->cursor_row + count);
		break;
	case 'C':
	case 'a':
		/* Right, stopping at the right edge. */
		screen_move(screen, screen->cursor_column + count, screen->cursor_row);
		break;
	case 'D':
		/* Left, stopping at the left edge. */
		if (count > screen->cursor_column)
			count = screen->cursor_column;
		screen->cursor_column -= count;
		break;
	case 'E':
		/* Down to the start of a line. */
		screen_move(screen, 0U, screen->cursor_row + count);
		break;
	case 'F':
		/* Up to the start of a line. */
		if (count > screen->cursor_row)
			count = screen->cursor_row;
		screen_move(screen, 0U, screen->cursor_row - count);
		break;
	case 'G':
	case '`':
		/* To a column of the current row. */
		screen_move(screen, count - 1U, screen->cursor_row);
		break;
	case 'd':
		/* To a row, keeping the column. */
		screen_move(screen, screen->cursor_column, count - 1U);
		break;
	case 'H':
	case 'f':
		/* To a row and a column, both counted from one. */
		row = (unsigned)screen_parameter(screen, 0, 1);
		column = (unsigned)screen_parameter(screen, 1, 1);
		if (row == 0U)
			row = 1U;
		if (column == 0U)
			column = 1U;
		screen_move(screen, column - 1U, row - 1U);
		break;
	case 'J':
		/* Erases in the display: 0 from the cursor on, 1 up to the cursor, 2 and 3 all of it. */
		mode = screen_parameter(screen, 0, 0);
		if (mode == 1) {
			for (row = 0U; row < screen->cursor_row; row++)
				screen_erase(screen, row, 0U, screen->columns - 1U);
			screen_erase(screen, screen->cursor_row, 0U, screen->cursor_column);
		} else if (mode == 2 || mode == 3) {
			for (row = 0U; row < screen->rows; row++)
				screen_erase(screen, row, 0U, screen->columns - 1U);
		} else {
			screen_erase(screen, screen->cursor_row, screen->cursor_column, screen->columns - 1U);
			for (row = screen->cursor_row + 1U; row < screen->rows; row++)
				screen_erase(screen, row, 0U, screen->columns - 1U);
		}

		break;
	case 'K':
		/* Erases in the line: 0 from the cursor on, 1 up to the cursor, 2 all of it. */
		mode = screen_parameter(screen, 0, 0);
		if (mode == 1) {
			screen_erase(screen, screen->cursor_row, 0U, screen->cursor_column);
		} else if (mode == 2) {
			screen_erase(screen, screen->cursor_row, 0U, screen->columns - 1U);
		} else {
			screen_erase(screen, screen->cursor_row, screen->cursor_column, screen->columns - 1U);
		}

		break;
	case 'X':
		/* Erases characters from the cursor on, without moving it. */
		screen_erase(screen, screen->cursor_row, screen->cursor_column, screen->cursor_column + count - 1U);
		break;
	case 'L':
		/* Inserts blank lines at the cursor's row, inside the scrolling region. */
		if (screen->cursor_row >= screen->scroll_top && screen->cursor_row <= screen->scroll_bottom)
			screen_scroll_down(screen, screen->cursor_row, screen->scroll_bottom, count);
		break;
	case 'M':
		/* Deletes lines at the cursor's row, inside the scrolling region. */
		if (screen->cursor_row >= screen->scroll_top && screen->cursor_row <= screen->scroll_bottom)
			screen_scroll_up(screen, screen->cursor_row, screen->scroll_bottom, count);
		break;
	case 'S':
		/* Scrolls the region up. */
		screen_scroll_up(screen, screen->scroll_top, screen->scroll_bottom, count);
		break;
	case 'T':
		/* Scrolls the region down. */
		screen_scroll_down(screen, screen->scroll_top, screen->scroll_bottom, count);
		break;
	case 'P':
		/* Deletes characters at the cursor; the rest of the row moves left. */
		last = screen->columns - 1U;
		if (count > screen->columns - screen->cursor_column)
			count = screen->columns - screen->cursor_column;
		for (column = screen->cursor_column; column + count <= last; column++)
			*terminal_screen_cell(screen, column, screen->cursor_row) = *terminal_screen_cell(screen, column + count, screen->cursor_row);
		screen_erase(screen, screen->cursor_row, screen->columns - count, last);
		break;
	case '@':
		/* Inserts blanks at the cursor; the rest of the row moves right. */
		if (count > screen->columns - screen->cursor_column)
			count = screen->columns - screen->cursor_column;
		for (column = screen->columns - 1U; column >= screen->cursor_column + count; column--)
			*terminal_screen_cell(screen, column, screen->cursor_row) = *terminal_screen_cell(screen, column - count, screen->cursor_row);
		screen_erase(screen, screen->cursor_row, screen->cursor_column, screen->cursor_column + count - 1U);
		break;
	case 'm':
		screen_sgr(screen);
		break;
	case 'r':
		/* The scrolling region, from the top row to the bottom row, counted from one; the cursor goes home. */
		row = (unsigned)screen_parameter(screen, 0, 1);
		last = (unsigned)screen_parameter(screen, 1, (int)screen->rows);
		if (row == 0U)
			row = 1U;
		if (last == 0U || last > screen->rows)
			last = screen->rows;
		if (row < last) {
			screen->scroll_top = row - 1U;
			screen->scroll_bottom = last - 1U;
		}

		/* Setting the region sends the cursor home. */
		screen_move(screen, 0U, 0U);
		break;
	case 's':
		screen->saved_column = screen->cursor_column;
		screen->saved_row = screen->cursor_row;
		break;
	case 'u':
		screen_move(screen, screen->saved_column, screen->saved_row);
		break;
	case 'h':
		screen_mode(screen, 1);
		break;
	case 'l':
		screen_mode(screen, 0);
		break;
	default:
		break;
	}
}

/* Sets the colours and attributes of new characters (SGR, CSI ... m). */
static void
screen_sgr(
	struct terminal_screen *screen)
{
	int index;
	int value;
	uint32_t red;
	uint32_t green;
	uint32_t blue;

	/* Each parameter in turn; an empty list is a reset. */
	for (index = 0; index < screen->parameter_count; index++) {
		value = screen->parameters[index];
		if (value < 0)
			value = 0;

		/* Routes the parameter by its number. */
		if (value == 0) {
			/* Everything back to the defaults. */
			screen->foreground = TERMINAL_FOREGROUND;
			screen->background = TERMINAL_BACKGROUND;
			screen->inverse = 0;
			screen->bold = 0;
		} else if (value == 1) {
			screen->bold = 1;
		} else if (value == 22) {
			screen->bold = 0;
		} else if (value == 7) {
			screen->inverse = 1;
		} else if (value == 27) {
			screen->inverse = 0;
		} else if (value >= 30 && value <= 37) {
			screen->foreground = screen_palette[value - 30];
		} else if (value >= 40 && value <= 47) {
			screen->background = screen_palette[value - 40];
		} else if (value >= 90 && value <= 97) {
			screen->foreground = screen_palette[value - 90 + 8];
		} else if (value >= 100 && value <= 107) {
			screen->background = screen_palette[value - 100 + 8];
		} else if (value == 39) {
			screen->foreground = TERMINAL_FOREGROUND;
		} else if (value == 49) {
			screen->background = TERMINAL_BACKGROUND;
		} else if ((value == 38 || value == 48) && index + 2 < screen->parameter_count && screen->parameters[index + 1] == 5) {
			/* An indexed colour: 38;5;n or 48;5;n. */
			if (value == 38) {
				screen->foreground = screen_colour_256(screen->parameters[index + 2]);
			} else {
				screen->background = screen_colour_256(screen->parameters[index + 2]);
			}

			/* The colour's two parameters are used up. */
			index += 2;
		} else if ((value == 38 || value == 48) && index + 4 < screen->parameter_count && screen->parameters[index + 1] == 2) {
			/* A direct colour: 38;2;r;g;b or 48;2;r;g;b. */
			red = (uint32_t)screen->parameters[index + 2] & 0xffU;
			green = (uint32_t)screen->parameters[index + 3] & 0xffU;
			blue = (uint32_t)screen->parameters[index + 4] & 0xffU;
			if (value == 38) {
				screen->foreground = (red << 16) | (green << 8) | blue;
			} else {
				screen->background = (red << 16) | (green << 8) | blue;
			}

			/* The colour's four parameters are used up. */
			index += 4;
		}
	}
}

/* Returns one of the 256 xterm colours as 0xRRGGBB. */
static uint32_t
screen_colour_256(
	int index)
{
	static const uint32_t levels[6] = { 0x00U, 0x5fU, 0x87U, 0xafU, 0xd7U, 0xffU };
	uint32_t grey;
	int cube;

	/* The first 16 are the palette. */
	if (index >= 0 && index < 16)
		return screen_palette[index];

	/* 16 to 231 are a 6x6x6 cube of red, green and blue levels. */
	if (index >= 16 && index < 232) {
		cube = index - 16;
		return (levels[cube / 36] << 16) | (levels[(cube / 6) % 6] << 8) | levels[cube % 6];
	}

	/* 232 to 255 are a ramp of greys. */
	if (index >= 232 && index < 256) {
		grey = 8U + 10U * (uint32_t)(index - 232);
		return (grey << 16) | (grey << 8) | grey;
	}

	/* Anything else is the default foreground. */
	return TERMINAL_FOREGROUND;
}

/* Sets or resets the modes a CSI ... h or l names; only the cursor's visibility is kept. */
static void
screen_mode(
	struct terminal_screen *screen,
	int set)
{
	int index;
	int value;

	/* Each mode the sequence names. */
	for (index = 0; index < screen->parameter_count; index++) {
		value = screen->parameters[index];

		/* DECTCEM (? 25) shows or hides the cursor; the alternate screen (? 1049, ? 47) clears the grid. */
		if (screen->private_mode == '?' && value == 25) {
			screen->cursor_visible = set;
		} else if (screen->private_mode == '?' && (value == 1049 || value == 47 || value == 1047)) {
			terminal_screen_init(screen, screen->columns, screen->rows);
		}
	}
}

/* Returns a parameter of the sequence, or the fallback when it was left out. */
static int
screen_parameter(
	const struct terminal_screen *screen,
	int index,
	int fallback)
{
	/* A parameter past the ones given, or an empty one, takes the fallback. */
	if (index >= screen->parameter_count || screen->parameters[index] < 0)
		return fallback;

	/* Reports the parameter as given. */
	return screen->parameters[index];
}

/* Moves the cursor down a row, scrolling the region at its bottom. */
static void
screen_line_feed(
	struct terminal_screen *screen)
{
	/* At the region's bottom the region scrolls; elsewhere the cursor moves down. */
	if (screen->cursor_row == screen->scroll_bottom) {
		screen_scroll_up(screen, screen->scroll_top, screen->scroll_bottom, 1U);
	} else if (screen->cursor_row + 1U < screen->rows) {
		screen->cursor_row++;
	}
}

/* Moves the cursor up a row, scrolling the region down at its top. */
static void
screen_reverse_index(
	struct terminal_screen *screen)
{
	/* At the region's top the region scrolls down; elsewhere the cursor moves up. */
	if (screen->cursor_row == screen->scroll_top) {
		screen_scroll_down(screen, screen->scroll_top, screen->scroll_bottom, 1U);
	} else if (screen->cursor_row != 0U) {
		screen->cursor_row--;
	}
}

/* Moves the rows from top to bottom up by count, blanking the rows that open at the bottom. */
static void
screen_scroll_up(
	struct terminal_screen *screen,
	unsigned top,
	unsigned bottom,
	unsigned count)
{
	unsigned row;

	/* A region outside the grid does not scroll. */
	if (bottom >= screen->rows || top > bottom)
		return;

	/* More than the region scrolls the whole region away. */
	if (count > bottom - top + 1U)
		count = bottom - top + 1U;

	/* The rows that stay move up. */
	for (row = top; row + count <= bottom; row++)
		memcpy(terminal_screen_cell(screen, 0U, row), terminal_screen_cell(screen, 0U, row + count), (size_t)screen->columns * sizeof(struct terminal_cell));

	/* The rows that opened are blank. */
	for (row = bottom + 1U - count; row <= bottom; row++)
		screen_erase(screen, row, 0U, screen->columns - 1U);
}

/* Moves the rows from top to bottom down by count, blanking the rows that open at the top. */
static void
screen_scroll_down(
	struct terminal_screen *screen,
	unsigned top,
	unsigned bottom,
	unsigned count)
{
	unsigned row;

	/* A region outside the grid does not scroll. */
	if (bottom >= screen->rows || top > bottom)
		return;

	/* More than the region scrolls the whole region away. */
	if (count > bottom - top + 1U)
		count = bottom - top + 1U;

	/* The rows that stay move down, starting from the bottom. */
	for (row = bottom; row >= top + count; row--)
		memcpy(terminal_screen_cell(screen, 0U, row), terminal_screen_cell(screen, 0U, row - count), (size_t)screen->columns * sizeof(struct terminal_cell));

	/* The rows that opened are blank. */
	for (row = top; row < top + count; row++)
		screen_erase(screen, row, 0U, screen->columns - 1U);
}

/* Collects one byte of UTF-8 text, placing each complete character. */
static void
screen_utf8(
	struct terminal_screen *screen,
	unsigned char byte)
{
	uint32_t codepoint;

	/* A new character: one byte, or the first of a sequence. */
	if (screen->utf8_remaining == 0U) {
		/* Classifies the lead byte; an overlong or out-of-range lead is a replacement character. */
		if (byte < 0x80U) {
			screen_put(screen, byte);
		} else if (byte >= 0xc2U && byte <= 0xdfU) {
			screen->utf8_value = byte & 0x1fU;
			screen->utf8_minimum = 0x80U;
			screen->utf8_remaining = 1U;
		} else if (byte >= 0xe0U && byte <= 0xefU) {
			screen->utf8_value = byte & 0x0fU;
			screen->utf8_minimum = 0x800U;
			screen->utf8_remaining = 2U;
		} else if (byte >= 0xf0U && byte <= 0xf4U) {
			screen->utf8_value = byte & 0x07U;
			screen->utf8_minimum = 0x10000U;
			screen->utf8_remaining = 3U;
		} else {
			screen_put(screen, 0xfffdU);
		}

		/* The lead byte is taken; the rest of its sequence follows. */
		return;
	}

	/* A byte that does not continue the sequence ends it with a replacement and starts again. */
	if ((byte & 0xc0U) != 0x80U) {
		screen->utf8_remaining = 0U;
		screen_put(screen, 0xfffdU);
		screen_utf8(screen, byte);
		return;
	}

	/* Adds six bits; the character is complete when no bytes remain. */
	screen->utf8_value = (screen->utf8_value << 6) | (byte & 0x3fU);
	screen->utf8_remaining--;
	if (screen->utf8_remaining != 0U)
		return;

	/* An overlong form, a surrogate or a value past Unicode is a replacement character. */
	codepoint = screen->utf8_value;
	if (codepoint < screen->utf8_minimum || codepoint > 0x10ffffU)
		codepoint = 0xfffdU;
	if (codepoint >= 0xd800U && codepoint <= 0xdfffU)
		codepoint = 0xfffdU;
	screen_put(screen, codepoint);
}

/* Places a character at the cursor and moves past it, wrapping at the right edge. */
static void
screen_put(
	struct terminal_screen *screen,
	uint32_t codepoint)
{
	struct terminal_cell *cell;
	unsigned width;
	uint32_t foreground;
	uint32_t background;
	int wide;

	/* A wide character takes two cells. */
	width = 1U;
	wide = screen_wide(codepoint);
	if (wide)
		width = 2U;

	/* A character that does not fit on the row starts the next one. */
	if (screen->cursor_column + width > screen->columns) {
		screen->cursor_column = 0U;
		screen_line_feed(screen);
	}

	/* The colours, swapped when inverse is on. */
	foreground = screen->foreground;
	background = screen->background;
	if (screen->inverse) {
		foreground = screen->background;
		background = screen->foreground;
	}

	/* The character's cell. */
	cell = terminal_screen_cell(screen, screen->cursor_column, screen->cursor_row);
	cell->codepoint = codepoint;
	cell->foreground = foreground;
	cell->background = background;
	cell->continuation = 0;

	/* The right half of a wide character. */
	if (width == 2U) {
		cell = terminal_screen_cell(screen, screen->cursor_column + 1U, screen->cursor_row);
		cell->codepoint = 0U;
		cell->foreground = foreground;
		cell->background = background;
		cell->continuation = 1;
	}

	/* The cursor moves past it, wrapping at the right edge. */
	screen->cursor_column += width;
	if (screen->cursor_column >= screen->columns) {
		screen->cursor_column = 0U;
		screen_line_feed(screen);
	}
}

/* Tells whether a character is drawn two cells wide (East Asian wide and fullwidth). */
static int
screen_wide(
	uint32_t codepoint)
{
	/* Hangul Jamo, and the angle brackets. */
	if (codepoint >= 0x1100U && codepoint <= 0x115fU)
		return 1;
	if (codepoint == 0x2329U || codepoint == 0x232aU)
		return 1;

	/* CJK radicals to Yi, Hangul syllables, compatibility ideographs and forms. */
	if (codepoint >= 0x2e80U && codepoint <= 0xa4cfU)
		return 1;
	if (codepoint >= 0xac00U && codepoint <= 0xd7a3U)
		return 1;
	if (codepoint >= 0xf900U && codepoint <= 0xfaffU)
		return 1;
	if (codepoint >= 0xfe10U && codepoint <= 0xfe6fU)
		return 1;

	/* Fullwidth forms, and the ideographs of the supplementary planes. */
	if (codepoint >= 0xff01U && codepoint <= 0xff60U)
		return 1;
	if (codepoint >= 0xffe0U && codepoint <= 0xffe6U)
		return 1;
	if (codepoint >= 0x20000U && codepoint <= 0x3fffdU)
		return 1;

	/* Everything else is one cell. */
	return 0;
}

/* Moves the cursor to a column and a row, clipped to the grid. */
static void
screen_move(
	struct terminal_screen *screen,
	unsigned column,
	unsigned row)
{
	/* The grid's last column and row are as far as the cursor goes. */
	if (column >= screen->columns)
		column = screen->columns - 1U;
	if (row >= screen->rows)
		row = screen->rows - 1U;

	/* The new position. */
	screen->cursor_column = column;
	screen->cursor_row = row;
}
