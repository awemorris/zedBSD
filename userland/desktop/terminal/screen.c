/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The character grid of terminal and the VT100 interpreter that
 * fills it from what the shell writes.
 *
 * It started as zterm's (userland/x11/zterm) and takes the same subset:
 * cursor motion, erasing, colours, UTF-8; with the scrolling region, line
 * and character insertion and deletion, the cursor's visibility, 256 and
 * direct colours, and OSC strings added for the programs a shell runs:
 * OSC 0 and 2 set the title (the tab's, ws035-p091), the others are
 * dropped.
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
 * The final bytes of the control sequences that move the cursor or edit
 * the grid, and so cancel a pending wrap; SGR, the modes and the reports
 * keep it, as on xterm (ws128-p010).
 */
#define SCREEN_WRAP_CANCELS	"ABCDEFGHJKLMPSTXadefru`@"

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
static void screen_scroll_up(struct terminal_screen *screen, unsigned top, unsigned bottom, unsigned count, int keep);
static void screen_keep_line(struct terminal_screen *screen, unsigned row);
static void screen_range_moved(struct terminal_screen *screen, unsigned top, unsigned bottom);
static void screen_clear_grid(struct terminal_screen *screen);
static void screen_scroll_down(struct terminal_screen *screen, unsigned top, unsigned bottom, unsigned count);
static void screen_utf8(struct terminal_screen *screen, unsigned char byte);
static void screen_put(struct terminal_screen *screen, uint32_t codepoint);
static void screen_move(struct terminal_screen *screen, unsigned column, unsigned row);
static void screen_osc(struct terminal_screen *screen);

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
 * Sets whether Ambiguous-width characters written from now on take two
 * cells (ws128-p009).
 *
 * The cells already on the screen and in the scrollback keep the width
 * they were written with, so nothing shown moves; only new output follows
 * the new setting.
 */
void
terminal_screen_set_ambiguous_wide(
	struct terminal_screen *screen,
	int ambiguous_wide)
{
	/* The width screen_put gives the next Ambiguous characters. */
	screen->ambiguous_wide = 0;
	if (ambiguous_wide)
		screen->ambiguous_wide = 1;
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

	/* The cursor and the scrolling region stay inside the grid, and a wrap pending at the old margin is dropped. */
	if (screen->cursor_column >= columns)
		screen->cursor_column = columns - 1U;
	if (screen->cursor_row >= rows)
		screen->cursor_row = rows - 1U;
	screen->wrap_pending = 0;
	screen->scroll_top = 0U;
	screen->scroll_bottom = rows - 1U;

	/*
	 * The rows were laid out again, so a range on them no longer names the
	 * same text, and the view returns to the live screen (ws035-p114).
	 */
	screen->range = 0;
	screen->view = 0U;

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

/*
 * Returns the cell at a column of a line (ws035-p114): a line of the
 * scrollback or of the screen, numbered from the first line the screen
 * ever showed.  Returns NULL for a line the terminal no longer keeps or
 * does not show yet, and for a column past the grid.
 */
struct terminal_cell *
terminal_screen_line_cell(
	struct terminal_screen *screen,
	unsigned column,
	unsigned long line)
{
	struct terminal_cell *cell;
	unsigned long oldest;
	unsigned long index;
	unsigned slot;

	/* A column past the grid has no cell. */
	if (column >= screen->columns)
		return NULL;

	/* A line below the screen's last row is not shown yet. */
	if (line >= screen->scrolled + screen->rows)
		return NULL;

	/* A line of the screen is the cell of its row. */
	if (line >= screen->scrolled) {
		cell = terminal_screen_cell(screen, column, (unsigned)(line - screen->scrolled));
		return cell;
	}

	/* A line older than the oldest kept has gone. */
	oldest = screen->scrolled - screen->history_count;
	if (line < oldest)
		return NULL;

	/* A line of the scrollback is in its slot of the ring, counted from the oldest. */
	index = line - oldest;
	slot = (unsigned)((screen->history_first + index) % TERMINAL_HISTORY);
	cell = &screen->history[(size_t)slot * TERMINAL_MAX_COLUMNS + column];

	/* Succeeded: the scrollback's cell. */
	return cell;
}

/*
 * Gives the line a row of the window shows (ws035-p114): the screen's row
 * when the view is on the live screen, a line of the scrollback when it is
 * back.
 */
unsigned long
terminal_screen_view_line(
	const struct terminal_screen *screen,
	unsigned row)
{
	/* Reports the line: the view's first line, then one per row. */
	return screen->scrolled - screen->view + row;
}

/*
 * Moves the view back into the scrollback (positive lines) or toward the
 * live screen (negative), within what is kept.  Returns nonzero when the
 * view moved.
 */
int
terminal_screen_scroll_view(
	struct terminal_screen *screen,
	int lines)
{
	unsigned old_view;
	unsigned step;

	/* Whole lines from here: the fingers' offset within a line goes (ws081-p011). */
	if (screen->view_offset != 0) {
		screen->view_offset = 0;
		screen->changed = 1;
	}

	/*
	 * Back goes no further than the oldest line kept, forward no further
	 * than the live screen.
	 */
	old_view = screen->view;
	if (lines > 0) {
		step = (unsigned)lines;
		if (step > screen->history_count - screen->view)
			step = screen->history_count - screen->view;
		screen->view += step;
	} else if (lines < 0) {
		step = 0U - (unsigned)lines;
		if (step > screen->view)
			step = screen->view;
		screen->view -= step;
	}

	/* A view that did not move changes nothing. */
	if (screen->view == old_view)
		return 0;

	/* Succeeded: the window shows other lines. */
	screen->changed = 1;
	return 1;
}

/*
 * Tells whether a cell of a line is in the range selected with the pointer
 * (ws035-p093; lines since ws035-p114).
 */
int
terminal_screen_in_range(
	const struct terminal_screen *screen,
	unsigned column,
	unsigned long line)
{
	/* Without a range, none is. */
	if (!screen->range)
		return 0;

	/* Lines before the first or after the last are not. */
	if (line < screen->range_from[1] || line > screen->range_to[1])
		return 0;

	/* On the first line, from its first cell; on the last, up to its last. */
	if (line == screen->range_from[1] && column < screen->range_from[0])
		return 0;
	if (line == screen->range_to[1] && column > screen->range_to[0])
		return 0;

	/* Succeeded: in it. */
	return 1;
}

/*
 * Writes the grid's text as UTF-8, a line per row without its trailing
 * spaces and without the empty rows at the bottom, into a buffer (cut at
 * its size): the range selected with the pointer when there is one and
 * the whole screen is not selected (its lines may be in the scrollback,
 * ws035-p114), else the whole grid.  Returns the number of bytes written,
 * without a terminator.
 */
size_t
terminal_screen_text(
	struct terminal_screen *screen,
	char *text,
	size_t size)
{
	struct terminal_cell *cell;
	unsigned char bytes[4];
	uint32_t codepoint;
	size_t length;
	size_t kept;
	size_t start;
	size_t count;
	unsigned column;
	unsigned long line;
	unsigned long first;
	unsigned long last;
	int only_range;
	int inside;

	/* The range's lines when it is the range that is copied, else the screen's. */
	only_range = 0;
	if (screen->range && !screen->selected)
		only_range = 1;
	if (only_range) {
		first = screen->range_from[1];
		last = screen->range_to[1];
	} else {
		first = screen->scrolled;
		last = screen->scrolled + screen->rows - 1U;
	}

	/* Each line, then a line break; kept is the length up to the end of the last line with text. */
	length = 0;
	kept = 0;
	start = 0;
	for (line = first; line <= last; line++) {
		/* Each cell of the line, as the character it shows. */
		for (column = 0U; column < screen->columns; column++) {
			/*
			 * A line no longer kept has no cells, a continuation is the
			 * right half of a wide character written already, and a cell
			 * out of the range is left out.
			 */
			cell = terminal_screen_line_cell(screen, column, line);
			if (cell == NULL)
				break;
			if (cell->continuation)
				continue;
			inside = terminal_screen_in_range(screen, column, line);
			if (only_range && !inside)
				continue;

			/* A cell never written shows a space. */
			codepoint = cell->codepoint;
			if (codepoint == 0U)
				codepoint = ' ';

			/* The character as UTF-8. */
			if (codepoint < 0x80U) {
				bytes[0] = (unsigned char)codepoint;
				count = 1;
			} else if (codepoint < 0x800U) {
				bytes[0] = (unsigned char)(0xc0U | (codepoint >> 6));
				bytes[1] = (unsigned char)(0x80U | (codepoint & 0x3fU));
				count = 2;
			} else if (codepoint < 0x10000U) {
				bytes[0] = (unsigned char)(0xe0U | (codepoint >> 12));
				bytes[1] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU));
				bytes[2] = (unsigned char)(0x80U | (codepoint & 0x3fU));
				count = 3;
			} else {
				bytes[0] = (unsigned char)(0xf0U | (codepoint >> 18));
				bytes[1] = (unsigned char)(0x80U | ((codepoint >> 12) & 0x3fU));
				bytes[2] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU));
				bytes[3] = (unsigned char)(0x80U | (codepoint & 0x3fU));
				count = 4;
			}

			/* Into the buffer while it has room. */
			if (length + count > size)
				return kept;
			memcpy(text + length, bytes, count);
			length += count;
		}

		/* The row's trailing spaces go (the row started after the last line break). */
		while (length > start && text[length - 1U] == ' ')
			length--;

		/* A row with text is kept up to its end. */
		if (length > start)
			kept = length;

		/* Each row ends with a line break, and the next row starts after it. */
		if (length + 1U > size)
			return kept;
		text[length] = '\n';
		length++;
		start = length;
	}

	/* Succeeded: the text up to the end of the last row that had any. */
	return kept;
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
		/* An OSC string (a window title, a colour) ends with BEL or ESC \. */
		if (byte == 0x07U) {
			screen->parser_state = SCREEN_TEXT;
			screen_osc(screen);
			return;
		}

		/* ESC starts its end. */
		if (byte == 0x1bU) {
			screen->parser_state = SCREEN_OSC_ESCAPE;
			return;
		}

		/* Another byte is kept while there is room. */
		if (screen->osc_length + 1U < TERMINAL_OSC)
			screen->osc[screen->osc_length++] = (char)byte;
		return;
	case SCREEN_OSC_ESCAPE:
		/* The byte after ESC ends the string whatever it is. */
		screen->parser_state = SCREEN_TEXT;
		screen_osc(screen);
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
		/* A carriage return goes to the first column; a wrap pending at the last one is not done. */
		screen->wrap_pending = 0;
		screen->cursor_column = 0U;
	} else if (byte == '\n' || byte == 0x0bU || byte == 0x0cU) {
		screen_line_feed(screen);
	} else if (byte == '\b') {
		/* Backspace stops at the left edge; from a pending wrap it goes to the column before the last, as on xterm. */
		screen->wrap_pending = 0;
		if (screen->cursor_column != 0U)
			screen->cursor_column--;
	} else if (byte == '\t') {
		/* A tab moves to the next multiple of eight, or the last column. */
		screen->wrap_pending = 0;
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
		screen->osc_length = 0U;
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
	const char *cancels;
	unsigned count;
	unsigned row;
	unsigned column;
	unsigned last;
	int mode;

	/* The modes (h, l) are the only private sequences taken; any other private one is ignored. */
	if (screen->private_mode != 0 && final != 'h' && final != 'l')
		return;

	/* A sequence that moves the cursor or edits the grid cancels a pending wrap. */
	cancels = strchr(SCREEN_WRAP_CANCELS, final);
	if (cancels != NULL)
		screen->wrap_pending = 0;

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
			screen_scroll_up(screen, screen->cursor_row, screen->scroll_bottom, count, 0);
		break;
	case 'S':
		/* Scrolls the region up. */
		screen_scroll_up(screen, screen->scroll_top, screen->scroll_bottom, count, 1);
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

		/* DECTCEM (? 25) shows or hides the cursor; the alternate screen (? 1049, ? 47) clears the grid, keeping the scrollback. */
		if (screen->private_mode == '?' && value == 25) {
			screen->cursor_visible = set;
		} else if (screen->private_mode == '?' && (value == 1049 || value == 47 || value == 1047)) {
			screen_clear_grid(screen);
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
	/* The row below is where the cursor shows, so a wrap pending at the last column is not done first. */
	screen->wrap_pending = 0;

	/* At the region's bottom the region scrolls; elsewhere the cursor moves down. */
	if (screen->cursor_row == screen->scroll_bottom) {
		screen_scroll_up(screen, screen->scroll_top, screen->scroll_bottom, 1U, 1);
	} else if (screen->cursor_row + 1U < screen->rows) {
		screen->cursor_row++;
	}
}

/* Moves the cursor up a row, scrolling the region down at its top. */
static void
screen_reverse_index(
	struct terminal_screen *screen)
{
	/* A move up cancels a pending wrap. */
	screen->wrap_pending = 0;

	/* At the region's top the region scrolls down; elsewhere the cursor moves up. */
	if (screen->cursor_row == screen->scroll_top) {
		screen_scroll_down(screen, screen->scroll_top, screen->scroll_bottom, 1U);
	} else if (screen->cursor_row != 0U) {
		screen->cursor_row--;
	}
}

/*
 * Moves the rows from top to bottom up by count, blanking the rows that
 * open at the bottom.  With keep, the whole screen scrolling up keeps the
 * rows that leave its top in the scrollback (ws035-p114).
 */
static void
screen_scroll_up(
	struct terminal_screen *screen,
	unsigned top,
	unsigned bottom,
	unsigned count,
	int keep)
{
	unsigned row;
	int whole;

	/* A region outside the grid does not scroll. */
	if (bottom >= screen->rows || top > bottom)
		return;

	/* More than the region scrolls the whole region away. */
	if (count > bottom - top + 1U)
		count = bottom - top + 1U;

	/*
	 * The whole screen's lines keep their numbers as they go into the
	 * scrollback, so a range on them follows its text; a region's scroll
	 * moves text under a range, which is dropped.
	 */
	whole = 0;
	if (keep && top == 0U && bottom + 1U == screen->rows)
		whole = 1;
	if (whole) {
		for (row = 0U; row < count; row++)
			screen_keep_line(screen, row);
	} else {
		screen_range_moved(screen, top, bottom);
	}

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

	/* The text moves under a range on the region, which is dropped (ws035-p114). */
	screen_range_moved(screen, top, bottom);

	/* The rows that stay move down, starting from the bottom. */
	for (row = bottom; row >= top + count; row--)
		memcpy(terminal_screen_cell(screen, 0U, row), terminal_screen_cell(screen, 0U, row - count), (size_t)screen->columns * sizeof(struct terminal_cell));

	/* The rows that opened are blank. */
	for (row = top; row < top + count; row++)
		screen_erase(screen, row, 0U, screen->columns - 1U);
}

/*
 * Keeps a row that is leaving the top of the screen as the newest line of
 * the scrollback (ws035-p114); a full scrollback drops its oldest line.
 */
static void
screen_keep_line(
	struct terminal_screen *screen,
	unsigned row)
{
	struct terminal_cell *kept;
	unsigned long oldest;
	unsigned slot;
	unsigned column;

	/* The slot after the newest while the ring has room, else the oldest's, whose line is dropped. */
	if (screen->history_count < TERMINAL_HISTORY) {
		slot = (screen->history_first + screen->history_count) % TERMINAL_HISTORY;
		screen->history_count++;
	} else {
		slot = screen->history_first;
		screen->history_first = (screen->history_first + 1U) % TERMINAL_HISTORY;
	}

	/* The row's cells, and blanks past the grid's width for a wider grid later. */
	kept = &screen->history[(size_t)slot * TERMINAL_MAX_COLUMNS];
	memcpy(kept, terminal_screen_cell(screen, 0U, row), (size_t)screen->columns * sizeof(struct terminal_cell));
	for (column = screen->columns; column < TERMINAL_MAX_COLUMNS; column++) {
		kept[column].codepoint = ' ';
		kept[column].foreground = TERMINAL_FOREGROUND;
		kept[column].background = TERMINAL_BACKGROUND;
		kept[column].continuation = 0;
	}

	/*
	 * One more line has scrolled off: the screen's rows are numbered one
	 * further on.  A view back in the scrollback goes one line further back
	 * so that it stays on the same text, unless it is at the oldest line.
	 */
	screen->scrolled++;
	if (screen->view != 0U && screen->view < screen->history_count)
		screen->view++;

	/* A range that started on the line just dropped has lost its text. */
	oldest = screen->scrolled - screen->history_count;
	if (screen->range && screen->range_from[1] < oldest)
		screen->range = 0;
}

/*
 * Drops the range selected with the pointer when it has a line in a part of
 * the screen whose text moves without its line numbers (a region scrolled,
 * lines inserted or deleted; ws035-p114).
 */
static void
screen_range_moved(
	struct terminal_screen *screen,
	unsigned top,
	unsigned bottom)
{
	/* No range, nothing to drop. */
	if (!screen->range)
		return;

	/* A range wholly above or wholly below the rows keeps its text. */
	if (screen->range_to[1] < screen->scrolled + top)
		return;
	if (screen->range_from[1] > screen->scrolled + bottom)
		return;

	/* Succeeded: the range's text moved, so the range goes. */
	screen->range = 0;
}

/*
 * Empties the grid for the alternate screen (? 1049, ? 47, ? 1047): the
 * default colours, the cursor home and the whole grid scrolling, with the
 * scrollback kept and the view on the live screen (ws035-p114).
 */
static void
screen_clear_grid(
	struct terminal_screen *screen)
{
	unsigned row;

	/* The state a new grid has. */
	screen->foreground = TERMINAL_FOREGROUND;
	screen->background = TERMINAL_BACKGROUND;
	screen->inverse = 0;
	screen->bold = 0;
	screen->cursor_column = 0U;
	screen->cursor_row = 0U;
	screen->wrap_pending = 0;
	screen->saved_column = 0U;
	screen->saved_row = 0U;
	screen->cursor_visible = 1;
	screen->scroll_top = 0U;
	screen->scroll_bottom = screen->rows - 1U;

	/* Every cell blank. */
	for (row = 0U; row < screen->rows; row++)
		screen_erase(screen, row, 0U, screen->columns - 1U);

	/* Nothing selected, the live screen shown and drawn anew. */
	screen->selected = 0;
	screen->range = 0;
	screen->view = 0U;
	screen->changed = 1;
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

/* Places a character at the cursor and moves past it, leaving a wrap pending at the right edge. */
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

	/*
	 * A wide character takes two cells, and so does an Ambiguous one while
	 * the setting is on (ws128-p009).  The width is decided here, once, and
	 * kept in the cells: a later change of the setting leaves the
	 * characters already placed as they are.
	 */
	width = 1U;
	wide = terminal_width_wide(codepoint, screen->ambiguous_wide);
	if (wide)
		width = 2U;

	/* A wrap left pending by a character in the last column is done now, before this one is placed. */
	if (screen->wrap_pending) {
		screen->cursor_column = 0U;
		screen_line_feed(screen);
	}

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

	/*
	 * The cursor moves past it.  A character that ends in the last column
	 * leaves the cursor there and the wrap pending, so a program that fills
	 * the row and then writes CR LF (Emacs's menu bar and mode line,
	 * BUG-150) moves down one row, not two, and a full last row does not
	 * scroll the screen.
	 */
	if (screen->cursor_column + width >= screen->columns) {
		screen->cursor_column = screen->columns - 1U;
		screen->wrap_pending = 1;
	} else {
		screen->cursor_column += width;
	}
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

	/* The new position, with no wrap pending at the old one. */
	screen->cursor_column = column;
	screen->cursor_row = row;
	screen->wrap_pending = 0;
}

/*
 * Carries out a finished OSC string: "0;TEXT" and "2;TEXT" set the title
 * (its control characters left out); the others are dropped.
 */
static void
screen_osc(
	struct terminal_screen *screen)
{
	const char *text;
	size_t used;
	size_t index;
	unsigned char byte;

	/* The string, and whether it sets the title. */
	screen->osc[screen->osc_length] = '\0';
	text = screen->osc;
	if ((text[0] != '0' && text[0] != '2') || text[1] != ';')
		return;

	/* The title's text, without control characters, as far as it fits. */
	used = 0U;
	for (index = 2U; text[index] != '\0' && used + 1U < sizeof(screen->title); index++) {
		byte = (unsigned char)text[index];
		if (byte < 0x20U || byte == 0x7fU)
			continue;
		screen->title[used++] = (char)byte;
	}

	/* A cut inside a character leaves none of it. */
	byte = (unsigned char)text[index];
	if ((byte & 0xc0U) == 0x80U) {
		while (used > 0U && ((unsigned char)screen->title[used - 1U] & 0xc0U) == 0x80U)
			used--;
		if (used > 0U && (unsigned char)screen->title[used - 1U] >= 0xc0U)
			used--;
	}

	/* Succeeded: the new title (the main loop shows it). */
	screen->title[used] = '\0';
}
