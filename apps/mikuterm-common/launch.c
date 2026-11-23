/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The screen a new tab shows before it connects (launch.h): its rows and
 * the line under them as the keys edit them, the screen drawn as a
 * terminal's text, and what wsl.exe wrote (its own messages in UTF-16,
 * the programs it runs in UTF-8) read as text and as the list of WSL's
 * distributions.
 *
 * The screen counts a character as one cell: the hosts, the
 * distributions and the messages are names and English text.
 */

#include "launch.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The longest text of the note over the hosts, in lines, shown on the screen. */
#define LAUNCH_NOTICE_LINES	6U

/* The most lines the message over the prompt is wrapped into. */
#define LAUNCH_STATUS_LINES	3U

/* What starts and ends a paste the terminal brackets (CSI 200 ~, CSI 201 ~). */
#define LAUNCH_PASTE_START	"200"
#define LAUNCH_PASTE_END	"201"

/* The Unicode replacement character, for a UTF-16 unit that is not a character. */
#define LAUNCH_REPLACEMENT	0xfffdUL

/*
 * The text a screen is drawn into: the room, how much is used, and
 * whether something did not fit (the text then ends where it stopped).
 */
struct launch_writer {
	char *out;
	size_t size;
	size_t used;
	int full;
};

static void launch_sequence(struct launch_screen *screen, unsigned char final);
static void launch_typed(struct launch_screen *screen, unsigned char byte);
static int launch_number(struct launch_screen *screen, unsigned digit);
static int launch_letter(struct launch_screen *screen, unsigned char byte);
static void launch_append(struct launch_screen *screen, unsigned char byte);
static void launch_erase(struct launch_screen *screen);
static void launch_put(struct launch_writer *writer, const char *text);
static void launch_put_bytes(struct launch_writer *writer, const char *bytes, size_t size);
static unsigned launch_put_cells(struct launch_writer *writer, const char *text, size_t size, unsigned cells);
static void launch_pad(struct launch_writer *writer, unsigned cells);
static unsigned launch_cells(const char *text, size_t size);
static unsigned launch_notice_lines(const char *notice);
static size_t launch_wrap(const char *text, size_t size, unsigned cells);
static unsigned launch_row_width(const struct launch_row *row);
static void launch_draw_row(struct launch_writer *writer, const struct launch_screen *screen, unsigned index, unsigned width, unsigned budget);
static void launch_trim(const char **start, const char **end);
static int launch_blank(char character);
static int launch_ipv4(const char *text, size_t size);
static size_t launch_utf8(unsigned long codepoint, char *bytes);
static void launch_text_add(char *text, size_t text_size, size_t *length, const char *bytes, size_t size);

/* Starts a screen with a heading and no rows. */
void
launch_init(
	struct launch_screen *screen,
	const char *heading)
{
	/* Nothing chosen, typed or said. */
	memset(screen, 0, sizeof(*screen));
	(void)snprintf(screen->heading, sizeof(screen->heading), "%s", heading);
}

/*
 * Adds a row at the end: its key, what it is, its label and a note (NULL:
 * none).  The first row is chosen.  Returns 0, or ENOSPC when the screen
 * is full.
 */
int
launch_add(
	struct launch_screen *screen,
	const char *key,
	unsigned kind,
	const char *label,
	const char *note)
{
	struct launch_row *row;

	/* A full screen takes no more. */
	if (screen->count == LAUNCH_ROWS_MAX)
		return ENOSPC;

	/* The row, cut to fit. */
	row = &screen->rows[screen->count];
	memset(row, 0, sizeof(*row));
	(void)snprintf(row->key, sizeof(row->key), "%s", key);
	row->kind = kind;
	(void)snprintf(row->label, sizeof(row->label), "%s", label);
	if (note != NULL)
		(void)snprintf(row->note, sizeof(row->note), "%s", note);
	screen->count++;

	/* The first row is the one chosen, its label on the line. */
	if (screen->count == 1U)
		launch_choose(screen, 0U);

	/* Succeeded: the row is on the screen. */
	return 0;
}

/* Chooses a row: its label is the line again, not edited. */
void
launch_choose(
	struct launch_screen *screen,
	unsigned index)
{
	size_t length;

	/* A row the screen does not have. */
	if (index >= screen->count)
		return;

	/* The row, and its label on the line. */
	screen->chosen = index;
	length = strlen(screen->rows[index].label);
	if (length >= sizeof(screen->line))
		length = sizeof(screen->line) - 1U;
	memcpy(screen->line, screen->rows[index].label, length);
	screen->line[length] = '\0';
	screen->line_length = length;
	screen->edited = 0;
	screen->number = 0U;
}

/* Sets the message shown over the line (NULL or empty: none). */
void
launch_say(
	struct launch_screen *screen,
	const char *text)
{
	/* The message, cut to fit. */
	screen->status[0] = '\0';
	if (text != NULL)
		(void)snprintf(screen->status, sizeof(screen->status), "%s", text);
}

/*
 * Takes the keys the tab sent, until one of them ends the line: Enter,
 * Ctrl+C or Ctrl+D (the user gives up), or Escape alone.  *used says how
 * many bytes were taken (the ones after the ending key are the caller's
 * to give again).  Returns LAUNCH_EVENT_*.
 */
unsigned
launch_keys(
	struct launch_screen *screen,
	const unsigned char *bytes,
	size_t size,
	size_t *used)
{
	size_t index;
	unsigned char byte;

	/* Each byte until one ends the line. */
	for (index = 0U; index < size; index++) {
		byte = bytes[index];

		/* After ESC: a CSI or SS3 sequence starts, or the ESC was the Escape key and this byte is a key of its own. */
		if (screen->escape == LAUNCH_ESCAPE_START) {
			screen->escape = LAUNCH_ESCAPE_NONE;
			if (byte == '[' || byte == 'O') {
				screen->escape = LAUNCH_ESCAPE_INSIDE;
				screen->sequence_length = 0U;
				continue;
			}

			/* The Escape key; the byte after it is the caller's to give again. */
			*used = index;
			return LAUNCH_EVENT_ESCAPE;
		}

		/* Inside a sequence: its parameters are kept until its final byte says what it is. */
		if (screen->escape == LAUNCH_ESCAPE_INSIDE) {
			if (byte >= 0x40U && byte <= 0x7eU) {
				screen->escape = LAUNCH_ESCAPE_NONE;
				screen->sequence[screen->sequence_length] = '\0';
				launch_sequence(screen, byte);
			} else if (screen->sequence_length + 1U < sizeof(screen->sequence)) {
				screen->sequence[screen->sequence_length] = (char)byte;
				screen->sequence_length++;
			}

			/* The sequence's byte is taken. */
			continue;
		}

		/* What the key does. */
		switch (byte) {
		case 0x1bU:
			/* ESC: a sequence starts, or the Escape key when nothing follows it (a key's sequence comes whole). */
			screen->escape = LAUNCH_ESCAPE_START;
			if (index + 1U == size) {
				screen->escape = LAUNCH_ESCAPE_NONE;
				*used = size;
				return LAUNCH_EVENT_ESCAPE;
			}

			/* The sequence goes on in the next byte. */
			break;
		case '\r':
		case '\n':
			/* Enter takes the line; a line break pasted is passed over. */
			if (screen->pasting)
				break;
			*used = index + 1U;
			return LAUNCH_EVENT_ENTER;
		case 0x03U:
		case 0x04U:
			/* Ctrl+C, Ctrl+D: the user gives up. */
			*used = index + 1U;
			return LAUNCH_EVENT_CANCEL;
		case 0x08U:
		case 0x7fU:
			/* Backspace: the line's last character goes. */
			launch_erase(screen);
			break;
		case 0x15U:
			/* Ctrl+U: the whole line goes. */
			screen->line_length = 0U;
			screen->line[0] = '\0';
			screen->edited = 1;
			screen->number = 0U;
			break;
		default:
			/* Text, or a row's key; other control characters are not text. */
			if (byte >= 0x20U)
				launch_typed(screen, byte);
			break;
		}
	}

	/* Every byte taken, nothing ended. */
	*used = size;
	return LAUNCH_EVENT_NONE;
}

/*
 * Tells what Enter takes: the index of a row (the chosen one while the
 * line is not edited, or the row whose label the line is), -1 for a
 * line typed by hand (screen->line), or -2 when the line is empty.
 */
int
launch_row_taken(
	const struct launch_screen *screen)
{
	unsigned index;
	int same;

	/* The chosen row, as long as its label was not edited. */
	if (!screen->edited && screen->count != 0U)
		return (int)screen->chosen;

	/* An empty line takes nothing. */
	if (screen->line_length == 0U)
		return -2;

	/* A row whose label the line is. */
	for (index = 0U; index < screen->count; index++) {
		/* The same text. */
		same = strcmp(screen->rows[index].label, screen->line);
		if (same == 0)
			return (int)index;
	}

	/* A host typed by hand. */
	return -1;
}

/*
 * Draws the screen whole into out (size bytes, a NUL after the text) for
 * a grid of so many columns and rows: the title, the notice (the
 * settings' problems; NULL or empty: none), the heading, as many rows as
 * fit round the chosen one, the message and the line, with the cursor at
 * the line's end.  Nothing reaches the last column or goes past the last
 * row, so the grid never scrolls.  Returns the length of the text.
 */
size_t
launch_draw(
	const struct launch_screen *screen,
	const char *title,
	const char *notice,
	unsigned columns,
	unsigned rows,
	char *out,
	size_t size)
{
	struct launch_writer writer;
	const char *start;
	const char *end;
	unsigned budget;
	unsigned notice_lines;
	unsigned status_lines;
	unsigned status_cells;
	unsigned fixed;
	unsigned shown;
	unsigned first;
	unsigned width;
	unsigned row_width;
	unsigned index;
	unsigned line_cells;
	unsigned skip;
	size_t offset;
	size_t length;
	size_t taken;

	/* The text from the start; a row ends before the last column. */
	writer.out = out;
	writer.size = size;
	writer.used = 0U;
	writer.full = 0;
	budget = 1U;
	if (columns > 1U)
		budget = columns - 1U;

	/* The paste bracketed while the screen shows, the cursor hidden while it is drawn, the grid emptied. */
	launch_put(&writer, "\033[0m\033[?2004h\033[?25l\033[H\033[J");

	/* The title, then a blank line. */
	launch_put(&writer, "\033[1m");
	(void)launch_put_cells(&writer, title, strlen(title), budget);
	launch_put(&writer, "\033[0m\r\n\r\n");

	/* The notice's lines, as many as are shown, then a blank line. */
	notice_lines = 0U;
	if (notice != NULL)
		notice_lines = launch_notice_lines(notice);
	start = notice;
	for (index = 0U; index < notice_lines; index++) {
		/* One line, without its CR. */
		end = strchr(start, '\n');
		if (end == NULL)
			end = start + strlen(start);
		offset = (size_t)(end - start);
		if (offset != 0U && start[offset - 1U] == '\r')
			offset--;
		(void)launch_put_cells(&writer, start, offset, budget);
		launch_put(&writer, "\r\n");
		start = end;
		if (*start == '\n')
			start++;
	}

	/* The blank line after a notice. */
	if (notice_lines != 0U)
		launch_put(&writer, "\r\n");

	/* The heading. */
	(void)launch_put_cells(&writer, screen->heading, strlen(screen->heading), budget);
	launch_put(&writer, "\r\n");

	/* As many rows as the grid has room for, round the chosen one. */
	status_cells = launch_cells(screen->status, strlen(screen->status));
	status_lines = 1U;
	if (status_cells > budget)
		status_lines = (status_cells + budget - 1U) / budget;
	if (status_lines > LAUNCH_STATUS_LINES)
		status_lines = LAUNCH_STATUS_LINES;
	fixed = 2U + notice_lines + 1U + status_lines + 1U;
	if (notice_lines != 0U)
		fixed++;
	shown = 1U;
	if (rows > fixed)
		shown = rows - fixed;
	if (shown > screen->count)
		shown = screen->count;
	first = 0U;
	if (shown != 0U && screen->chosen >= shown / 2U)
		first = screen->chosen - shown / 2U;
	if (first + shown > screen->count)
		first = screen->count - shown;

	/* The rows' common width, so the chosen one's reverse is as wide as the longest. */
	width = 0U;
	for (index = 0U; index < screen->count; index++) {
		/* The widest "[key] label". */
		row_width = launch_row_width(&screen->rows[index]);
		if (row_width > width)
			width = row_width;
	}

	/* The rows shown. */
	for (index = first; index < first + shown; index++) {
		launch_draw_row(&writer, screen, index, width, budget);
		launch_put(&writer, "\r\n");
	}

	/* The message, in yellow, over the prompt, wrapped at its spaces (blank when none). */
	offset = 0U;
	length = strlen(screen->status);
	for (index = 0U; index < status_lines && offset < length; index++) {
		/* One line's words, and the spaces after them passed over. */
		if (index != 0U)
			launch_put(&writer, "\r\n");
		taken = launch_wrap(screen->status + offset, length - offset, budget);
		launch_put(&writer, "\033[33m");
		(void)launch_put_cells(&writer, screen->status + offset, taken, budget);
		launch_put(&writer, "\033[0m");
		offset += taken;
		while (offset < length && screen->status[offset] == ' ')
			offset++;
	}

	/* The message's line ends, empty or not. */
	launch_put(&writer, "\r\n");

	/* The prompt and the line's end that fits after it; the cursor shown after it. */
	launch_put(&writer, ">>> ");
	line_cells = launch_cells(screen->line, screen->line_length);
	skip = 0U;
	if (budget > 4U && line_cells > budget - 4U)
		skip = line_cells - (budget - 4U);
	offset = 0U;
	while (skip != 0U && offset < screen->line_length) {
		/* One character's bytes. */
		offset++;
		while (offset < screen->line_length && ((unsigned char)screen->line[offset] & 0xc0U) == 0x80U)
			offset++;
		skip--;
	}

	/* The line's end that fits, and the cursor after it. */
	launch_put_bytes(&writer, screen->line + offset, screen->line_length - offset);
	launch_put(&writer, "\033[?25h");

	/* Succeeded: the screen's text (cut short when the room ran out). */
	return writer.used;
}

/*
 * Writes into out (size bytes, a NUL after the text) what leaves the
 * screen for the connection: the paste not bracketed any more, the
 * colours back and the grid empty.  Returns the length of the text.
 */
size_t
launch_leave(
	char *out,
	size_t size)
{
	struct launch_writer writer;

	/* The terminal as the connection expects it. */
	writer.out = out;
	writer.size = size;
	writer.used = 0U;
	writer.full = 0;
	launch_put(&writer, "\033[0m\033[?2004l\033[H\033[J\033[?25h");

	/* Succeeded: the text. */
	return writer.used;
}

/*
 * Reads what wsl.exe wrote as UTF-8 text into text (text_size bytes with
 * its NUL): its own messages and lists are UTF-16 (little-endian, with or
 * without a byte order mark), a program it runs writes UTF-8, and
 * WSL_UTF8=1 makes its own UTF-8 too.  NULs are left out; a UTF-16 unit
 * that is not a character becomes U+FFFD.
 */
void
launch_output_text(
	const unsigned char *bytes,
	size_t size,
	char *text,
	size_t text_size)
{
	unsigned long unit;
	unsigned long low;
	unsigned long codepoint;
	unsigned zeros;
	unsigned pairs;
	size_t position;
	size_t index;
	size_t length;
	size_t encoded;
	char character[4];
	int wide;

	/* Nothing yet. */
	length = 0U;
	if (text_size == 0U)
		return;
	text[0] = '\0';

	/* UTF-16 when it starts with its byte order mark or most of its first units' high bytes are 0 (ASCII text). */
	wide = 0;
	index = 0U;
	if (size >= 2U &&
	    bytes[0] == 0xffU &&
	    bytes[1] == 0xfeU) {
		wide = 1;
		index = 2U;
	} else {
		zeros = 0U;
		pairs = 0U;
		for (position = 0U; position + 1U < size && pairs < 32U; position += 2U) {
			/* A unit whose high byte is 0 and low byte is not. */
			pairs++;
			if (bytes[position + 1U] == 0U && bytes[position] != 0U)
				zeros++;
		}

		/* Most units of ASCII text. */
		if (pairs != 0U && zeros * 2U > pairs)
			wide = 1;
	}

	/* UTF-8, as it is, without a byte order mark or NULs. */
	if (!wide) {
		if (size >= 3U &&
		    bytes[0] == 0xefU &&
		    bytes[1] == 0xbbU &&
		    bytes[2] == 0xbfU)
			index = 3U;

		/* The bytes after the mark, each but a NUL. */
		for (; index < size; index++) {
			/* A NUL is left out. */
			if (bytes[index] != 0U)
				launch_text_add(text, text_size, &length, (const char *)&bytes[index], 1U);
		}

		/* The UTF-8 text is read. */
		return;
	}

	/* UTF-16, unit by unit, a surrogate pair one character. */
	for (; index + 1U < size; index += 2U) {
		/* The unit, and the low half after a high surrogate. */
		unit = (unsigned long)bytes[index] | ((unsigned long)bytes[index + 1U] << 8);
		codepoint = unit;
		if (unit >= 0xd800UL && unit <= 0xdbffUL) {
			codepoint = LAUNCH_REPLACEMENT;
			if (index + 3U < size) {
				low = (unsigned long)bytes[index + 2U] | ((unsigned long)bytes[index + 3U] << 8);
				if (low >= 0xdc00UL && low <= 0xdfffUL) {
					codepoint = 0x10000UL + ((unit - 0xd800UL) << 10) + (low - 0xdc00UL);
					index += 2U;
				}
			}
		} else if (unit >= 0xdc00UL && unit <= 0xdfffUL) {
			codepoint = LAUNCH_REPLACEMENT;
		}

		/* The character in UTF-8; a NUL is left out. */
		if (codepoint == 0UL)
			continue;
		encoded = launch_utf8(codepoint, character);
		launch_text_add(text, text_size, &length, character, encoded);
	}
}

/*
 * Reads the distributions from the text of "wsl.exe -l -q": one name a
 * line, blank lines and spaces round a name left out, a name with a
 * space, a quote or a control character in it refused (WSL's names have
 * none).  Writes at most so many names; returns how many.
 */
unsigned
launch_names(
	const char *text,
	char names[][LAUNCH_NAME_MAX],
	unsigned most)
{
	const char *start;
	const char *end;
	const char *next;
	size_t length;
	size_t index;
	unsigned count;
	int refused;

	/* Each line in turn. */
	count = 0U;
	start = text;
	while (*start != '\0' && count < most) {
		/* The line's end, and where the next starts. */
		end = strchr(start, '\n');
		if (end == NULL)
			end = start + strlen(start);
		next = end;
		if (*next == '\n')
			next++;

		/* The name without the spaces and CR round it. */
		launch_trim(&start, &end);
		length = (size_t)(end - start);

		/* A name of the characters WSL allows, that fits. */
		refused = 0;
		if (length == 0U || length >= LAUNCH_NAME_MAX)
			refused = 1;
		for (index = 0U; index < length && !refused; index++) {
			/* A space, a quote or a control character. */
			if ((unsigned char)start[index] <= 0x20U ||
			    start[index] == '"' ||
			    start[index] == 0x7f)
				refused = 1;
		}

		/* The name kept. */
		if (!refused) {
			memcpy(names[count], start, length);
			names[count][length] = '\0';
			count++;
		}

		/* The next line. */
		start = next;
	}

	/* Succeeded: the names read. */
	return count;
}

/*
 * Writes the first line of a text that is not blank into line (size bytes
 * with its NUL), without the spaces round it, cut to fit.  Returns 0, or
 * ENOENT when every line is blank.
 */
int
launch_first_line(
	const char *text,
	char *line,
	size_t size)
{
	const char *start;
	const char *end;
	size_t length;
	int found;

	/* Each line until one is not blank. */
	found = 0;
	start = text;
	end = text;
	while (*start != '\0') {
		/* The line without the spaces and CR round it. */
		end = strchr(start, '\n');
		if (end == NULL)
			end = start + strlen(start);
		launch_trim(&start, &end);

		/* A line with text ends the search. */
		if (end > start) {
			found = 1;
			break;
		}

		/* The next line. */
		start = strchr(start, '\n');
		if (start == NULL)
			break;
		start++;
	}

	/* No line has text. */
	if (!found) {
		line[0] = '\0';
		return ENOENT;
	}

	/* The line, cut to fit. */
	length = (size_t)(end - start);
	if (length >= size)
		length = size - 1U;
	memcpy(line, start, length);
	line[length] = '\0';

	/* Succeeded: the first line with text. */
	return 0;
}

/* Carries out a CSI or SS3 sequence the keys sent: Up and Down choose a row, a paste's brackets mark its text. */
static void
launch_sequence(
	struct launch_screen *screen,
	unsigned char final)
{
	int same;

	/* What the sequence is. */
	switch (final) {
	case 'A':
		/* Up: the row above, the line its label again. */
		if (screen->chosen > 0U)
			launch_choose(screen, screen->chosen - 1U);
		else
			launch_choose(screen, screen->chosen);
		break;
	case 'B':
		/* Down: the row below. */
		if (screen->chosen + 1U < screen->count)
			launch_choose(screen, screen->chosen + 1U);
		else
			launch_choose(screen, screen->chosen);
		break;
	case '~':
		/*
		 * The start and end of a paste: what comes between is text, never
		 * a row's key, and it takes the place of a row's label not edited
		 * (a host pasted is a host of its own).
		 */
		same = strcmp(screen->sequence, LAUNCH_PASTE_START);
		if (same == 0) {
			screen->pasting = 1;
			if (!screen->edited) {
				screen->line_length = 0U;
				screen->line[0] = '\0';
				screen->edited = 1;
				screen->number = 0U;
			}
		}

		/* The end of a paste: keys are keys again. */
		same = strcmp(screen->sequence, LAUNCH_PASTE_END);
		if (same == 0)
			screen->pasting = 0;
		break;
	default:
		/* Other keys (Left, Right, Home, a function key) do nothing here. */
		break;
	}
}

/*
 * Takes a character typed: while the line is the chosen row's label, a
 * row's number or letter chooses that row, and any other character
 * starts a line of its own in the label's place (a host typed by hand);
 * on a line already edited (and in a paste) it is added to the line.
 */
static void
launch_typed(
	struct launch_screen *screen,
	unsigned char byte)
{
	int taken;

	/* A row's key, while the line is not edited and not pasted into. */
	if (!screen->edited && !screen->pasting) {
		/* A digit is part of a row's number. */
		if (byte >= '0' && byte <= '9') {
			(void)launch_number(screen, (unsigned)(byte - '0'));
			return;
		}

		/* A letter is a row's key when one row has it. */
		taken = launch_letter(screen, byte);
		if (taken)
			return;

		/* Another character: the label goes, the line is the user's. */
		screen->line_length = 0U;
		screen->line[0] = '\0';
	}

	/* Text on the line. */
	launch_append(screen, byte);
}

/*
 * Chooses the row whose number the digits typed so far make with this
 * one (12 after 1), or else this digit's own row.  A number no row has is
 * passed over.  Returns 1 when a row was chosen, 0 when not.
 */
static int
launch_number(
	struct launch_screen *screen,
	unsigned digit)
{
	char key[LAUNCH_KEY_MAX];
	unsigned candidates[2];
	unsigned tried;
	unsigned index;
	int same;

	/* The digits so far with this one, then this one alone. */
	candidates[0] = digit;
	candidates[1] = digit;
	if (screen->number != 0U && screen->number < 100U)
		candidates[0] = screen->number * 10U + digit;

	/* The first of them a row has. */
	for (tried = 0U; tried < 2U; tried++) {
		/* The number as a key. */
		(void)snprintf(key, sizeof(key), "%u", candidates[tried]);
		for (index = 0U; index < screen->count; index++) {
			/* The row with that key: chosen, its number kept for a digit more. */
			same = strcmp(screen->rows[index].key, key);
			if (same == 0) {
				launch_choose(screen, index);
				screen->number = candidates[tried];
				return 1;
			}
		}
	}

	/* No row has the number. */
	return 0;
}

/* Chooses the row whose key is this letter; returns 1 when there is one, 0 when not. */
static int
launch_letter(
	struct launch_screen *screen,
	unsigned char byte)
{
	unsigned index;

	/* The row with the one-letter key. */
	for (index = 0U; index < screen->count; index++) {
		/* The same letter, case and all. */
		if (screen->rows[index].key[0] == (char)byte && screen->rows[index].key[1] == '\0') {
			launch_choose(screen, index);
			return 1;
		}
	}

	/* No row has the letter. */
	return 0;
}

/* Adds a byte to the line while it has room; the line is edited from now on. */
static void
launch_append(
	struct launch_screen *screen,
	unsigned char byte)
{
	/* Edited, even when the line has no room left. */
	screen->edited = 1;
	screen->number = 0U;
	if (screen->line_length + 1U >= sizeof(screen->line))
		return;

	/* The byte and the NUL after it. */
	screen->line[screen->line_length] = (char)byte;
	screen->line_length++;
	screen->line[screen->line_length] = '\0';
}

/* Takes the line's last character (all its UTF-8 bytes) off; the line is edited from now on. */
static void
launch_erase(
	struct launch_screen *screen)
{
	/* Edited, even when nothing is left to take. */
	screen->edited = 1;
	screen->number = 0U;
	if (screen->line_length == 0U)
		return;

	/* The bytes that continue a character, then its first. */
	while (screen->line_length > 1U && ((unsigned char)screen->line[screen->line_length - 1U] & 0xc0U) == 0x80U)
		screen->line_length--;
	screen->line_length--;
	screen->line[screen->line_length] = '\0';
}

/* Adds a text to the screen's text. */
static void
launch_put(
	struct launch_writer *writer,
	const char *text)
{
	/* All its bytes. */
	launch_put_bytes(writer, text, strlen(text));
}

/* Adds bytes to the screen's text while they fit, the text kept ended by a NUL. */
static void
launch_put_bytes(
	struct launch_writer *writer,
	const char *bytes,
	size_t size)
{
	/* Bytes that do not fit whole are left out, and so is everything after them. */
	if (writer->full || writer->size == 0U)
		return;
	if (writer->used + size >= writer->size) {
		writer->full = 1;
		writer->out[writer->used] = '\0';
		return;
	}

	/* The bytes, and the NUL after them. */
	memcpy(writer->out + writer->used, bytes, size);
	writer->used += size;
	writer->out[writer->used] = '\0';
}

/*
 * Adds as many whole characters of a text (size bytes) as fit in so many
 * cells; control characters are left out.  Returns the cells used.
 */
static unsigned
launch_put_cells(
	struct launch_writer *writer,
	const char *text,
	size_t size,
	unsigned cells)
{
	size_t start;
	size_t end;
	unsigned used;

	/* Each character while there are cells. */
	used = 0U;
	start = 0U;
	while (start < size && used < cells) {
		/* The character's bytes. */
		end = start + 1U;
		while (end < size && ((unsigned char)text[end] & 0xc0U) == 0x80U)
			end++;

		/* A control character takes no cell and is not shown. */
		if ((unsigned char)text[start] >= 0x20U && text[start] != 0x7f) {
			launch_put_bytes(writer, text + start, end - start);
			used++;
		}

		/* The next character. */
		start = end;
	}

	/* Succeeded: the cells the text took. */
	return used;
}

/* Adds so many spaces. */
static void
launch_pad(
	struct launch_writer *writer,
	unsigned cells)
{
	unsigned index;

	/* One space a cell. */
	for (index = 0U; index < cells; index++)
		launch_put_bytes(writer, " ", 1U);
}

/* Counts the characters of a text (size bytes): its cells. */
static unsigned
launch_cells(
	const char *text,
	size_t size)
{
	size_t index;
	unsigned cells;

	/* Each byte that starts a character. */
	cells = 0U;
	for (index = 0U; index < size; index++) {
		/* Not a byte that continues one. */
		if (((unsigned char)text[index] & 0xc0U) != 0x80U)
			cells++;
	}

	/* Succeeded: the cells. */
	return cells;
}

/* Counts the lines of a notice that are shown (at most LAUNCH_NOTICE_LINES; a last line break starts no line). */
static unsigned
launch_notice_lines(
	const char *notice)
{
	const char *at;
	unsigned lines;

	/* An empty notice has none. */
	if (notice[0] == '\0')
		return 0U;

	/* One line, and one more after each line break that has text after it. */
	lines = 1U;
	at = notice;
	while (lines < LAUNCH_NOTICE_LINES) {
		/* The next line break, and whether text follows it. */
		at = strchr(at, '\n');
		if (at == NULL || at[1] == '\0')
			break;
		at++;
		lines++;
	}

	/* Succeeded: the lines shown. */
	return lines;
}

/*
 * Tells how many bytes of a text (size bytes) go on a line of so many
 * cells: all of it when it fits, else up to the last space that fits (the
 * space left for the next line), or as many characters as fit when no
 * space does.
 */
static size_t
launch_wrap(
	const char *text,
	size_t size,
	unsigned cells)
{
	size_t index;
	size_t space;
	unsigned used;

	/* The characters that fit, and the last space among them. */
	used = 0U;
	space = 0U;
	for (index = 0U; index < size; index++) {
		/* A byte that starts a character takes a cell; one more than fits ends the line. */
		if (((unsigned char)text[index] & 0xc0U) == 0x80U)
			continue;
		if (used == cells)
			break;
		if (text[index] == ' ')
			space = index;
		used++;
	}

	/* All of it fits. */
	if (index == size)
		return size;

	/* Up to the last space, or else the characters that fit. */
	if (space != 0U)
		return space;

	/* Succeeded: as many characters as fit. */
	return index;
}

/* Tells how many cells a row's "[key] label" takes. */
static unsigned
launch_row_width(
	const struct launch_row *row)
{
	unsigned cells;

	/* The brackets and the space, the key and the label. */
	cells = 3U + launch_cells(row->key, strlen(row->key));
	cells += launch_cells(row->label, strlen(row->label));

	/* Succeeded: the width. */
	return cells;
}

/*
 * Draws one row: a space, "[key] label" (in the chosen colours, as wide
 * as the widest row, when it is the chosen one), and its note in grey,
 * all within so many cells.
 */
static void
launch_draw_row(
	struct launch_writer *writer,
	const struct launch_screen *screen,
	unsigned index,
	unsigned width,
	unsigned budget)
{
	const struct launch_row *row;
	unsigned room;
	unsigned used;

	/* The space before the row. */
	row = &screen->rows[index];
	if (budget < 2U)
		return;
	launch_put(writer, " ");
	room = budget - 1U;

	/* The chosen row's colours. */
	if (index == screen->chosen)
		launch_put(writer, "\033[38;2;" LAUNCH_CHOSEN_TEXT ";48;2;" LAUNCH_CHOSEN_BACK "m");

	/* The key in brackets, the label and, on the chosen row, the spaces to the common width. */
	used = launch_put_cells(writer, "[", 1U, room);
	used += launch_put_cells(writer, row->key, strlen(row->key), room - used);
	used += launch_put_cells(writer, "] ", 2U, room - used);
	used += launch_put_cells(writer, row->label, strlen(row->label), room - used);
	if (index == screen->chosen &&
	    width > used &&
	    width <= room) {
		launch_pad(writer, width - used);
		used = width;
	}

	/* The colours back after the chosen row. */
	if (index == screen->chosen)
		launch_put(writer, "\033[0m");

	/* The note, in grey, after two spaces. */
	if (row->note[0] != '\0' && used + 2U < room) {
		launch_pad(writer, 2U);
		used += 2U;
		launch_put(writer, "\033[90m");
		(void)launch_put_cells(writer, row->note, strlen(row->note), room - used);
		launch_put(writer, "\033[0m");
	}
}

/*
 * Writes the first IPv4 address of a text into address (size bytes with
 * its NUL, at least LAUNCH_IPV4_MAX): the words of "hostname -I" (the
 * addresses separated by blanks), the first in dotted form that is not a
 * loopback (127.x.x.x).  Returns 0, or ENOENT when there is none.
 */
int
launch_first_ipv4(
	const char *text,
	char *address,
	size_t size)
{
	size_t start;
	size_t end;
	size_t length;
	int blank;
	int valid;
	int loopback;

	/* Each word in turn. */
	address[0] = '\0';
	start = 0U;
	while (text[start] != '\0') {
		/* Blanks and line breaks between the words are passed over. */
		blank = launch_blank(text[start]);
		if (blank || text[start] == '\n') {
			start++;
			continue;
		}

		/* The word, up to the next blank or line break. */
		end = start;
		while (text[end] != '\0' && text[end] != '\n') {
			/* A blank ends the word. */
			blank = launch_blank(text[end]);
			if (blank)
				break;
			end++;
		}
		length = end - start;

		/* An IPv4 address that fits and is not on the loopback is the answer. */
		valid = launch_ipv4(text + start, length);
		loopback = 0;
		if (length >= 4U && strncmp(text + start, "127.", 4U) == 0)
			loopback = 1;
		if (valid && !loopback && length < size) {
			memcpy(address, text + start, length);
			address[length] = '\0';
			return 0;
		}

		/* The next word. */
		start = end;
	}

	/* No address. */
	return ENOENT;
}

/* Moves the start of a text past its blanks and its end back before them (spaces, tabs, CRs). */
static void
launch_trim(
	const char **start,
	const char **end)
{
	int blank;

	/* The blanks at the start. */
	while (*start < *end) {
		/* A blank is passed over. */
		blank = launch_blank(**start);
		if (!blank)
			break;
		(*start)++;
	}

	/* The blanks at the end. */
	while (*end > *start) {
		/* A blank is left out. */
		blank = launch_blank((*end)[-1]);
		if (!blank)
			break;
		(*end)--;
	}
}

/* Tells whether a character is a blank round a name: a space, a tab or a CR (1), or not (0). */
static int
launch_blank(
	char character)
{
	/* The three blanks. */
	if (character == ' ')
		return 1;
	if (character == '\t')
		return 1;
	if (character == '\r')
		return 1;

	/* Anything else is text. */
	return 0;
}

/* Tells whether a text (size bytes) is an IPv4 address in dotted form, four numbers of 0 to 255 (1), or not (0). */
static int
launch_ipv4(
	const char *text,
	size_t size)
{
	size_t index;
	unsigned parts;
	unsigned digits;
	unsigned value;

	/* Each character: digits, and a dot between the numbers. */
	parts = 1U;
	digits = 0U;
	value = 0U;
	for (index = 0U; index < size; index++) {
		/* A dot ends a number that has digits. */
		if (text[index] == '.') {
			if (digits == 0U || parts == 4U)
				return 0;
			parts++;
			digits = 0U;
			value = 0U;
			continue;
		}

		/* A digit, three at most, the number at most 255. */
		if (text[index] < '0' || text[index] > '9')
			return 0;
		value = value * 10U + (unsigned)(text[index] - '0');
		digits++;
		if (digits > 3U || value > 255U)
			return 0;
	}

	/* Four numbers, the last with digits. */
	if (parts != 4U || digits == 0U)
		return 0;

	/* An address. */
	return 1;
}

/* Writes a character's UTF-8 bytes (one to four); returns how many. */
static size_t
launch_utf8(
	unsigned long codepoint,
	char *bytes)
{
	/* One byte for ASCII. */
	if (codepoint < 0x80UL) {
		bytes[0] = (char)codepoint;
		return 1U;
	}

	/* Two bytes. */
	if (codepoint < 0x800UL) {
		bytes[0] = (char)(0xc0UL | (codepoint >> 6));
		bytes[1] = (char)(0x80UL | (codepoint & 0x3fUL));
		return 2U;
	}

	/* Three bytes. */
	if (codepoint < 0x10000UL) {
		bytes[0] = (char)(0xe0UL | (codepoint >> 12));
		bytes[1] = (char)(0x80UL | ((codepoint >> 6) & 0x3fUL));
		bytes[2] = (char)(0x80UL | (codepoint & 0x3fUL));
		return 3U;
	}

	/* Succeeded: four bytes. */
	bytes[0] = (char)(0xf0UL | (codepoint >> 18));
	bytes[1] = (char)(0x80UL | ((codepoint >> 12) & 0x3fUL));
	bytes[2] = (char)(0x80UL | ((codepoint >> 6) & 0x3fUL));
	bytes[3] = (char)(0x80UL | (codepoint & 0x3fUL));
	return 4U;
}

/* Adds bytes to a text while they fit whole, the text kept ended by a NUL. */
static void
launch_text_add(
	char *text,
	size_t text_size,
	size_t *length,
	const char *bytes,
	size_t size)
{
	/* Bytes that do not fit whole are left out. */
	if (*length + size >= text_size)
		return;

	/* The bytes and the NUL. */
	memcpy(text + *length, bytes, size);
	*length += size;
	text[*length] = '\0';
}
