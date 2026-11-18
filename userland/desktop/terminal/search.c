/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Finding text in the terminal (ws128-p006, Edit > Find): in the lines the
 * scrollback keeps and on the screen, by their numbers (a line is numbered
 * from the first the screen ever showed, as the selection's are).
 *
 * A line is read as its characters in order, a wide character once and
 * an empty cell as a space, so a text is found across the two cells of a
 * wide character but never inside one.  Letters of ASCII are found
 * whatever their case.  A text is not found across the end of a line.
 */

#include "terminal.h"
#include "width.h"

#include <string.h>

/*
 * The characters of one line as the search reads them: each character's
 * code point (ASCII letters folded to lower case), the column it starts
 * in and how many cells it takes.
 */
struct search_text {
	uint32_t codepoints[TERMINAL_MAX_COLUMNS];
	unsigned columns[TERMINAL_MAX_COLUMNS];
	unsigned widths[TERMINAL_MAX_COLUMNS];
	size_t count;
};

static int search_read(struct terminal_screen *screen, unsigned long line, struct search_text *text);
static int search_matches(const struct search_text *text, size_t at, const uint32_t *query, size_t count);
static uint32_t search_fold(uint32_t codepoint);
static void search_bounds(const struct terminal_screen *screen, unsigned long *oldest, unsigned long *newest);

/*
 * Reports the column of the search bar's caret (ws090-p022): after
 * "Find: " and the cells of the text looked for (UTF-8, length bytes), as
 * the bar draws them; the input method's composed text starts there.
 */
unsigned
terminal_search_bar_column(
	const char *text,
	size_t length,
	int ambiguous_wide)
{
	uint32_t query[TERMINAL_SEARCH_LENGTH];
	size_t count;
	size_t i;
	unsigned column;
	int wide;

	/* "Find: ", then each character's cells. */
	count = terminal_search_decode(text, length, query, TERMINAL_SEARCH_LENGTH);
	column = 6U;
	for (i = 0; i < count; i++) {
		wide = terminal_width_wide(query[i], ambiguous_wide);
		column++;
		if (wide)
			column++;
	}

	/* Succeeded: the caret's column. */
	return column;
}

/*
 * Reads a text (UTF-8, length bytes) into the code points the search
 * looks for, ASCII letters folded to lower case; a byte that does not
 * start a whole character is left out.  Returns how many there are (at
 * most capacity).
 */
size_t
terminal_search_decode(
	const char *text,
	size_t length,
	uint32_t *query,
	size_t capacity)
{
	const unsigned char *bytes;
	uint32_t codepoint;
	size_t at;
	size_t count;
	unsigned remaining;
	unsigned index;

	/* Each character in turn while there is room. */
	bytes = (const unsigned char *)text;
	at = 0;
	count = 0;
	while (at < length && count < capacity) {
		/* The first byte says how many follow. */
		codepoint = bytes[at];
		remaining = 0U;
		if (codepoint >= 0xf0U) {
			codepoint &= 0x07U;
			remaining = 3U;
		} else if (codepoint >= 0xe0U) {
			codepoint &= 0x0fU;
			remaining = 2U;
		} else if (codepoint >= 0xc0U) {
			codepoint &= 0x1fU;
			remaining = 1U;
		} else if (codepoint >= 0x80U) {
			/* A byte that does not start a character is left out. */
			at++;
			continue;
		}

		/* The bytes that follow, while they are there. */
		at++;
		for (index = 0U; index < remaining && at < length; index++) {
			codepoint = (codepoint << 6) | (bytes[at] & 0x3fU);
			at++;
		}

		/* A character cut short is left out. */
		if (index < remaining)
			break;

		/* One more character to look for. */
		query[count] = search_fold(codepoint);
		count++;
	}

	/* Reports how many characters there are. */
	return count;
}

/*
 * Finds the next match of a text from a place: with direction 1 the
 * nearest match that starts before (line, column), going back through
 * the scrollback; with direction -1 the nearest that starts after it,
 * going toward the newest line.  A place past either end starts at that
 * end.  Returns 1 with the match's line, first column and width in cells,
 * or 0 when there is none.
 */
int
terminal_search_find(
	struct terminal_screen *screen,
	const uint32_t *query,
	size_t count,
	unsigned long line,
	unsigned column,
	int direction,
	unsigned long *found_line,
	unsigned *found_column,
	unsigned *cells)
{
	static struct search_text text;
	unsigned long oldest;
	unsigned long newest;
	unsigned long current;
	size_t at;
	size_t best;
	int have;
	int read;
	int match;
	int limited;

	/* An empty text matches nothing. */
	if (count == 0)
		return 0;

	/* The lines kept, and where the search starts among them. */
	search_bounds(screen, &oldest, &newest);
	current = line;
	limited = 1;
	if (direction > 0 && current > newest) {
		current = newest;
		limited = 0;
	} else if (direction < 0 && current < oldest) {
		current = oldest;
		limited = 0;
	}

	/* Each line from the start, one way, until a match. */
	for (;;) {
		/* A line no longer kept ends the search that way. */
		if (current < oldest || current > newest)
			return 0;
		read = search_read(screen, current, &text);

		/* The match nearest the start in this line: the last before it going back, the first after it going on. */
		have = 0;
		best = 0;
		for (at = 0; read != 0 && at + count <= text.count; at++) {
			/* A match before the starting column, going back, or after it, going on, counts in the starting line. */
			if (limited && current == line) {
				if (direction > 0 && text.columns[at] >= column)
					break;
				if (direction < 0 && text.columns[at] <= column)
					continue;
			}

			/* Whether the text is here. */
			match = search_matches(&text, at, query, count);
			if (match == 0)
				continue;
			have = 1;
			best = at;

			/* Going on, the first match is the nearest. */
			if (direction < 0)
				break;
		}

		/* A match: where it is and how many cells it covers. */
		if (have) {
			*found_line = current;
			*found_column = text.columns[best];
			*cells = text.columns[best + count - 1U] + text.widths[best + count - 1U] - text.columns[best];
			return 1;
		}

		/* The next line that way; the ends stop the search. */
		if (direction > 0) {
			if (current == oldest)
				return 0;
			current--;
		} else {
			if (current == newest)
				return 0;
			current++;
		}
	}
}

/*
 * Marks the cells of one line that a match of a text covers (marks has a
 * byte for each column of the screen, set to 1 under a match and 0
 * elsewhere).  Returns how many matches the line has.
 */
int
terminal_search_line(
	struct terminal_screen *screen,
	const uint32_t *query,
	size_t count,
	unsigned long line,
	unsigned char *marks)
{
	static struct search_text text;
	unsigned column;
	unsigned last;
	size_t at;
	int found;
	int read;
	int match;

	/* Nothing is marked yet. */
	memset(marks, 0, screen->columns);
	found = 0;

	/* The line's characters; a line no longer kept, or no text, has no match. */
	read = search_read(screen, line, &text);
	if (read == 0 || count == 0)
		return 0;

	/* Each place the text starts, its cells marked. */
	for (at = 0; at + count <= text.count; at++) {
		match = search_matches(&text, at, query, count);
		if (match == 0)
			continue;

		/* The cells from the first character's to the end of the last's. */
		last = text.columns[at + count - 1U] + text.widths[at + count - 1U];
		for (column = text.columns[at]; column < last && column < screen->columns; column++)
			marks[column] = 1;
		found++;
	}

	/* Reports how many matches the line has. */
	return found;
}

/*
 * Moves the view so that a line is shown: a line already in the window
 * stays where it is; another comes to the window's middle, as far as the
 * scrollback and the live screen allow.
 */
void
terminal_search_show(
	struct terminal_screen *screen,
	unsigned long line)
{
	unsigned long top;
	unsigned long wanted_top;
	unsigned long wanted_view;
	long change;

	/* The first line the window shows now. */
	top = screen->scrolled - screen->view;

	/* A line in the window needs no move. */
	if (line >= top && line < top + screen->rows)
		return;

	/* The view that puts the line in the middle (the live screen at most, the oldest line kept at least). */
	wanted_top = 0;
	if (line > screen->rows / 2U)
		wanted_top = line - screen->rows / 2U;
	wanted_view = 0;
	if (wanted_top < screen->scrolled)
		wanted_view = screen->scrolled - wanted_top;
	if (wanted_view > screen->history_count)
		wanted_view = screen->history_count;

	/* The view moves by the difference. */
	change = (long)wanted_view - (long)screen->view;
	(void)terminal_screen_scroll_view(screen, (int)change);
}

/*
 * Reads a line's characters as the search sees them; returns 0 for a line
 * the terminal no longer keeps.
 */
static int
search_read(
	struct terminal_screen *screen,
	unsigned long line,
	struct search_text *text)
{
	const struct terminal_cell *cell;
	const struct terminal_cell *next;
	unsigned column;
	uint32_t codepoint;

	/* Nothing read yet. */
	text->count = 0;

	/* Each cell that starts a character. */
	for (column = 0U; column < screen->columns; column++) {
		cell = terminal_screen_line_cell(screen, column, line);
		if (cell == NULL)
			return 0;

		/* The right half of a wide character was read with its left. */
		if (cell->continuation != 0)
			continue;

		/* An empty cell reads as a space. */
		codepoint = cell->codepoint;
		if (codepoint == 0U)
			codepoint = ' ';

		/* The character, where it starts, and its width (two when the next cell is its right half). */
		text->codepoints[text->count] = search_fold(codepoint);
		text->columns[text->count] = column;
		text->widths[text->count] = 1U;
		if (column + 1U < screen->columns) {
			next = terminal_screen_line_cell(screen, column + 1U, line);
			if (next != NULL && next->continuation != 0)
				text->widths[text->count] = 2U;
		}

		/* One more character read. */
		text->count++;
	}

	/* Succeeded: the line is read. */
	return 1;
}

/* Tells whether the text is at a place of a line's characters. */
static int
search_matches(
	const struct search_text *text,
	size_t at,
	const uint32_t *query,
	size_t count)
{
	size_t index;

	/* Each character of the text against the line's. */
	for (index = 0; index < count; index++) {
		if (text->codepoints[at + index] != query[index])
			return 0;
	}

	/* Every character is the same. */
	return 1;
}

/* Folds an ASCII capital letter to its small letter; anything else is itself. */
static uint32_t
search_fold(
	uint32_t codepoint)
{
	/* A capital letter of ASCII. */
	if (codepoint >= 'A' && codepoint <= 'Z')
		return codepoint - 'A' + 'a';

	/* Any other character. */
	return codepoint;
}

/* Writes the numbers of the oldest line the terminal keeps and of the newest (the screen's last row). */
static void
search_bounds(
	const struct terminal_screen *screen,
	unsigned long *oldest,
	unsigned long *newest)
{
	/* The scrollback's oldest line, and the screen's bottom row. */
	*oldest = screen->scrolled - screen->history_count;
	*newest = screen->scrolled + screen->rows - 1U;
}
