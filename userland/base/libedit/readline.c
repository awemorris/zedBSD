/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A deliberately small Readline-compatible line editor and history.  The
 * package is named libedit, while its public header and archive names
 * follow Readline.
 *
 * readline() puts the terminal in raw mode and edits one line with emacs
 * keys (the arrows, Ctrl-A, Ctrl-E and the like) or, when rl_editing_mode
 * is 0, with the vi keys of XCU sh's vi-mode.  The history keeps the last
 * HISTORY_MAX lines that the caller adds.
 */

#include "readline/readline.h"
#include "readline/history.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* How many lines the history keeps; the oldest goes when it is full. */
#define HISTORY_MAX 32

/* The first size of a line buffer, which doubles as the line grows. */
#define LINE_INITIAL 128U

/* The largest count a vi command takes; more digits are ignored. */
#define VI_COUNT_MAX 9999U

/*
 * An editing key of emacs mode: an arrow, Home, End or Delete as a
 * terminal sends it, or the control key that means the same.
 */
enum edit_key {
	EDIT_NONE,
	EDIT_UP,
	EDIT_DOWN,
	EDIT_LEFT,
	EDIT_RIGHT,
	EDIT_HOME,
	EDIT_END,
	EDIT_DELETE
};

/*
 * What one key does to the line: the line goes on being edited, is
 * complete, is canceled with Ctrl-C, ends the input with Ctrl-D, or could
 * not be edited for want of memory.
 */
enum line_result {
	LINE_CONTINUE,
	LINE_ACCEPT,
	LINE_CANCEL,
	LINE_END,
	LINE_NOMEM
};

/*
 * The line a key edits.
 *
 * Its members point at the locals of readline(), so that a key can grow
 * the buffer, change the text and move the cursor; one instance lives for
 * one call of readline().
 */
struct edit_line {
	char **text;
	size_t *capacity;
	size_t *length;
	size_t *point;
};

/*
 * What the vi editor remembers between keys.
 *
 * The mode, a count and an operator waiting for its motion belong to the
 * line being edited; the yanked text outlives the line so that p can put
 * it into the next one.
 */
struct vi_state {
	int command;			/* 1 in command mode, 0 in insert mode */
	int escape;			/* 1 after ESC, 2 after ESC [ */
	unsigned replace_count;		/* characters r replaces; 0 when r is not waiting */
	unsigned count;			/* the count typed so far; 0 for none */
	unsigned operator_count;	/* the count typed before the waiting operator */
	unsigned char pending_operator;	/* d, c or y waiting for a motion; 0 for none */
	char *yank;			/* the text p and P put; NULL before any */
	char *undo;			/* the line before the last change; NULL for none */
	size_t undo_point;		/* the cursor in the undo line */
};

/*
 * The line being edited, as GNU Readline publishes it to the caller.
 *
 * readline() keeps it pointing at its buffer while it edits and leaves it
 * pointing at the line it returns, or NULL at the end of the input.
 */
char *rl_line_buffer;

/* The cursor in rl_line_buffer, kept with it. */
int rl_point;

/* The length of rl_line_buffer, kept with it. */
int rl_end;

/*
 * The editing mode, as GNU Readline names it: 1 for emacs, 0 for vi.
 *
 * The caller sets it before readline(); the shell follows set -o vi.
 */
int rl_editing_mode = 1;

/*
 * The number the oldest history entry has, as GNU Readline counts; it
 * moves up by one each time a full history drops its oldest entry.
 */
int history_base = 1;

/* How many entries history_entries holds. */
int history_length;

/*
 * The history, oldest first.
 *
 * The first history_length entries hold lines this file allocated; the
 * rest are empty.
 */
static HIST_ENTRY history_entries[HISTORY_MAX];

/*
 * The entry the history keys have walked to.
 *
 * history_length stands for the line being typed, past the newest entry;
 * readline() puts it back there when a line starts.
 */
static int history_position;

/*
 * The first character a key changed, which the display redraws from.
 *
 * readline() sets it to (size_t)-1 (nothing changed) before each key, and
 * line_changed() lowers it as the key edits the line.
 */
static size_t line_changed_from;

/*
 * The vi editor's memory between keys.
 *
 * readline() is not reentrant, so one instance serves every line.
 * vi_begin_line() resets it when a line starts, all but the yanked text.
 */
static struct vi_state vi_state;

static char *duplicate(const char *text);
static int write_all(const char *bytes, size_t size);
static enum line_result emacs_key(const struct edit_line *edit, unsigned char byte);
static enum edit_key control_key(unsigned char byte);
static enum edit_key escape_key(void);
static int emacs_edit_key(const struct edit_line *edit, enum edit_key key);
static int replace_line(char **line, size_t *capacity, size_t *length, size_t *point, const char *replacement);
static int grow(char **line, size_t *capacity, size_t need);
static int line_insert(const struct edit_line *edit, size_t position, const char *text, size_t count);
static void line_delete(const struct edit_line *edit, size_t start, size_t end);
static void line_changed(size_t position);
static void update_display(const char *line, size_t old_length, size_t old_point, size_t length, size_t point, size_t changed_from);
static void move_cursor(size_t from, size_t to);
static void cursor_step(size_t columns, char direction);
static void vi_begin_line(void);
static enum line_result vi_key(const struct edit_line *edit, unsigned char byte);
static unsigned char vi_arrow(unsigned char letter);
static enum line_result vi_insert_key(const struct edit_line *edit, unsigned char byte);
static enum line_result vi_command_key(const struct edit_line *edit, unsigned char byte);
static enum line_result vi_edit_command(const struct edit_line *edit, unsigned char byte, unsigned count);
static void vi_operator_motion(const struct edit_line *edit, unsigned char motion, unsigned count);
static void vi_operate(const struct edit_line *edit, unsigned char operator_key, size_t target, int inclusive);
static int vi_motion(const struct edit_line *edit, unsigned char motion, unsigned count, size_t *target, int *inclusive);
static size_t vi_left(const struct edit_line *edit, unsigned count);
static size_t vi_first_nonblank(const struct edit_line *edit);
static int vi_class(char character, int big);
static size_t vi_word_forward(const struct edit_line *edit, size_t position, int big);
static size_t vi_word_backward(const struct edit_line *edit, size_t position, int big);
static size_t vi_word_end(const struct edit_line *edit, size_t position, int big);
static size_t vi_current_word_end(const struct edit_line *edit, size_t position, int big);
static void vi_keep(const struct edit_line *edit, size_t start, size_t end);
static void vi_save_undo(const struct edit_line *edit);
static int vi_undo(const struct edit_line *edit);
static int vi_put(const struct edit_line *edit, unsigned char command, unsigned count);
static void vi_replace_characters(const struct edit_line *edit, unsigned char byte, unsigned count);
static void vi_toggle_case(const struct edit_line *edit, unsigned count);
static int vi_history(const struct edit_line *edit, int older, unsigned count);
static int vi_oldest_history(const struct edit_line *edit);

/*
 * Starts walking the history from the line being typed.
 */
void
using_history(
	void)
{
	/* The walk starts past the newest entry. */
	history_position = history_length;
}

/*
 * Empties the history.
 */
void
clear_history(
	void)
{
	int index;

	/* Every line the history holds is freed. */
	for (index = 0; index < history_length; index++) {
		free(history_entries[index].line);
		history_entries[index].line = NULL;
		history_entries[index].timestamp = NULL;
	}

	/* The history starts again from its first number. */
	history_length = 0;
	history_position = 0;
	history_base = 1;
}

/*
 * Adds a line to the history as its newest entry.
 *
 * An empty line is not kept.  A full history drops its oldest entry.
 */
void
add_history(
	const char *line)
{
	char *copy;

	/* An empty line is not worth recalling. */
	if (line == NULL || line[0] == '\0')
		return;

	/* The history keeps a copy; without memory the line is not kept. */
	copy = duplicate(line);
	if (copy == NULL)
		return;

	/* A full history drops its oldest entry, and the numbers move up. */
	if (history_length == HISTORY_MAX) {
		free(history_entries[0].line);
		memmove(&history_entries[0], &history_entries[1], (HISTORY_MAX - 1U) * sizeof(history_entries[0]));
		history_length--;
		history_base++;
	}

	/* The line is the newest entry, and the walk starts past it. */
	history_entries[history_length].line = copy;
	history_entries[history_length].timestamp = NULL;
	history_length++;
	history_position = history_length;
}

/*
 * Moves the history walk to an entry; returns 1, or 0 for a position
 * outside the history.
 */
int
history_set_pos(
	int position)
{
	/* A position past the line being typed is refused. */
	if (position < 0 || position > history_length)
		return 0;

	/* Succeeded: the walk is at the entry. */
	history_position = position;
	return 1;
}

/*
 * Returns the entry the history walk is at, or NULL on the line being
 * typed.
 */
HIST_ENTRY *
current_history(
	void)
{
	/* The line being typed is not an entry. */
	if (history_position < 0 || history_position >= history_length)
		return NULL;

	/* Succeeded: the entry. */
	return &history_entries[history_position];
}

/*
 * Walks the history one entry older and returns it, or NULL at the oldest.
 */
HIST_ENTRY *
previous_history(
	void)
{
	/* Nothing is older than the oldest entry. */
	if (history_position <= 0)
		return NULL;

	/* Succeeded: the older entry. */
	history_position--;
	return &history_entries[history_position];
}

/*
 * Walks the history one entry newer and returns it, or NULL on reaching
 * the line being typed.
 */
HIST_ENTRY *
next_history(
	void)
{
	HIST_ENTRY *entry;

	/* Nothing is newer than the line being typed. */
	if (history_position >= history_length)
		return NULL;

	/* The newer entry, which past the newest is the line being typed. */
	history_position++;
	entry = current_history();
	if (entry == NULL)
		return NULL;

	/* Succeeded: the newer entry. */
	return entry;
}

/*
 * Reads a line from the terminal with editing.
 *
 * The prompt is written first.  The line comes back without its newline,
 * in memory the caller frees; NULL means the end of the input (Ctrl-D on
 * an empty line, or a read that fails).  A terminal is put in raw mode for
 * the editing and restored before the return.
 */
char *
readline(
	const char *prompt)
{
	struct termios saved;
	struct termios raw;
	struct edit_line edit;
	enum line_result result;
	unsigned char byte;
	ssize_t received;
	char *line;
	size_t capacity;
	size_t length;
	size_t point;
	size_t old_length;
	size_t old_point;
	int attributes_error;
	int terminal;

	/* No prompt writes nothing. */
	if (prompt == NULL)
		prompt = "";

	/* The buffer starts empty. */
	capacity = LINE_INITIAL;
	length = 0;
	point = 0;
	line = malloc(capacity);
	if (line == NULL)
		return NULL;

	/* The caller sees the empty line, and the history walk starts past the newest entry. */
	line[0] = '\0';
	rl_line_buffer = line;
	rl_point = 0;
	rl_end = 0;
	history_position = history_length;

	/* The keys work on the locals of this call. */
	edit.text = &line;
	edit.capacity = &capacity;
	edit.length = &length;
	edit.point = &point;
	vi_begin_line();

	/* A terminal goes to raw mode: bytes one at a time, no echo, no signals from keys. */
	attributes_error = tcgetattr(STDIN_FILENO, &saved);
	terminal = 0;
	if (attributes_error == 0) {
		terminal = 1;
		raw = saved;
		raw.c_lflag &= ~(ICANON | ECHO | ISIG);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		attributes_error = tcsetattr(STDIN_FILENO, TCSANOW, &raw);
		if (attributes_error != 0)
			terminal = 0;
	}

	/* The prompt. */
	(void)write_all(prompt, strlen(prompt));

	/* Each key edits the line until one ends it. */
	for (;;) {
		received = read(STDIN_FILENO, &byte, 1);
		if (received < 0 && errno == EINTR)
			continue;

		/* The end of the input, or a failed read, ends the line. */
		if (received <= 0) {
			result = LINE_END;
			break;
		}

		/* The key edits the line in the current mode. */
		old_length = length;
		old_point = point;
		line_changed_from = (size_t)-1;
		if (rl_editing_mode == 0) {
			result = vi_key(&edit, byte);
		} else {
			result = emacs_key(&edit, byte);
		}

		/* The caller and the terminal see the line as the key left it. */
		rl_line_buffer = line;
		rl_point = (int)point;
		rl_end = (int)length;
		update_display(line, old_length, old_point, length, point, line_changed_from);

		/* Any result but going on ends the line. */
		if (result != LINE_CONTINUE)
			break;
	}

	/* Enter goes to the next line; Ctrl-C leaves an empty line. */
	if (result == LINE_ACCEPT) {
		(void)write_all("\n", 1);
	} else if (result == LINE_CANCEL) {
		length = 0;
		point = 0;
		line[0] = '\0';
		(void)write_all("^C\n", 3);
	}

	/* The terminal goes back to how the caller had it. */
	if (terminal)
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);

	/* The end of the input: no line. */
	if (result == LINE_END) {
		free(line);
		rl_line_buffer = NULL;
		return NULL;
	}

	/* Succeeded: the line, which the caller frees. */
	line[length] = '\0';
	rl_line_buffer = line;
	rl_point = (int)point;
	rl_end = (int)length;
	return line;
}

/* Returns a copy of a string in allocated memory, or NULL without memory. */
static char *
duplicate(
	const char *text)
{
	size_t size;
	char *copy;

	/* Room for the text and its terminating null. */
	size = strlen(text) + 1U;
	copy = malloc(size);
	if (copy == NULL)
		return NULL;

	/* Succeeded: the copy. */
	memcpy(copy, text, size);
	return copy;
}

/* Writes bytes to standard output; returns 1, or 0 when the write fails. */
static int
write_all(
	const char *bytes,
	size_t size)
{
	ssize_t done;

	/* Writes until every byte is out; a signal only interrupts a write. */
	while (size != 0) {
		done = write(STDOUT_FILENO, bytes, size);
		if (done < 0 && errno == EINTR)
			continue;

		/* The output is gone or refuses more. */
		if (done <= 0)
			return 0;

		/* The rest of the bytes. */
		bytes += done;
		size -= (size_t)done;
	}

	/* Succeeded. */
	return 1;
}

/* Takes one byte in emacs mode. */
static enum line_result
emacs_key(
	const struct edit_line *edit,
	unsigned char byte)
{
	enum edit_key key;
	char character;
	int done;

	/* An escape sequence or a control key may name an editing key. */
	if (byte == 0x1b) {
		key = escape_key();
	} else {
		key = control_key(byte);
	}

	/* An editing key moves the cursor, deletes or walks the history. */
	if (key != EDIT_NONE) {
		done = emacs_edit_key(edit, key);
		if (!done)
			return LINE_NOMEM;
		return LINE_CONTINUE;
	}

	/* Enter completes the line. */
	if (byte == '\r' || byte == '\n')
		return LINE_ACCEPT;

	/* Ctrl-C cancels the line. */
	if (byte == 3)
		return LINE_CANCEL;

	/* Ctrl-D ends the input on an empty line, and otherwise deletes under the cursor. */
	if (byte == 4) {
		if (*edit->length == 0)
			return LINE_END;
		line_delete(edit, *edit->point, *edit->point + 1U);
		return LINE_CONTINUE;
	}

	/* Backspace erases the character before the cursor. */
	if (byte == 8 || byte == 0x7f) {
		if (*edit->point > 0)
			line_delete(edit, *edit->point - 1U, *edit->point);
		return LINE_CONTINUE;
	}

	/* Ctrl-K erases from the cursor to the end of the line. */
	if (byte == 11) {
		line_delete(edit, *edit->point, *edit->length);
		return LINE_CONTINUE;
	}

	/* Ctrl-U erases everything before the cursor. */
	if (byte == 21) {
		line_delete(edit, 0, *edit->point);
		return LINE_CONTINUE;
	}

	/* Other control characters are not text. */
	if (byte < 0x20)
		return LINE_CONTINUE;

	/* The character goes in at the cursor. */
	character = (char)byte;
	done = line_insert(edit, *edit->point, &character, 1);
	if (!done)
		return LINE_NOMEM;

	/* Succeeded. */
	return LINE_CONTINUE;
}

/* Translates a control key of emacs mode to the editing key it means. */
static enum edit_key
control_key(
	unsigned char byte)
{
	/* Ctrl-A, B, E, F, N and P, as in Emacs and GNU Readline. */
	switch (byte) {
	case 1:
		return EDIT_HOME;
	case 2:
		return EDIT_LEFT;
	case 5:
		return EDIT_END;
	case 6:
		return EDIT_RIGHT;
	case 14:
		return EDIT_DOWN;
	case 16:
		return EDIT_UP;
	default:
		break;
	}

	/* Not an editing key. */
	return EDIT_NONE;
}

/*
 * Reads the rest of an escape sequence after its ESC and returns the
 * editing key it names, or EDIT_NONE for any other sequence.
 */
static enum edit_key
escape_key(
	void)
{
	unsigned char byte;
	ssize_t received;

	/* A sequence the keys send starts with ESC [. */
	received = read(STDIN_FILENO, &byte, 1);
	if (received != 1 || byte != '[')
		return EDIT_NONE;

	/* The letter or digit after the bracket names the key. */
	received = read(STDIN_FILENO, &byte, 1);
	if (received != 1)
		return EDIT_NONE;

	/* The arrows, Home and End, and the 3 that starts Delete (ESC [ 3 ~). */
	switch (byte) {
	case 'A':
		return EDIT_UP;
	case 'B':
		return EDIT_DOWN;
	case 'C':
		return EDIT_RIGHT;
	case 'D':
		return EDIT_LEFT;
	case 'H':
		return EDIT_HOME;
	case 'F':
		return EDIT_END;
	case '3':
		break;
	default:
		return EDIT_NONE;
	}

	/* Delete ends with a tilde. */
	received = read(STDIN_FILENO, &byte, 1);
	if (received != 1 || byte != '~')
		return EDIT_NONE;

	/* Succeeded: Delete. */
	return EDIT_DELETE;
}

/* Carries out an editing key of emacs mode; returns 0 without memory. */
static int
emacs_edit_key(
	const struct edit_line *edit,
	enum edit_key key)
{
	HIST_ENTRY *entry;
	int replaced;

	/* Chooses what the key does. */
	switch (key) {
	case EDIT_HOME:
		*edit->point = 0;
		break;
	case EDIT_END:
		*edit->point = *edit->length;
		break;
	case EDIT_LEFT:
		if (*edit->point > 0)
			(*edit->point)--;
		break;
	case EDIT_RIGHT:
		if (*edit->point < *edit->length)
			(*edit->point)++;
		break;
	case EDIT_DELETE:
		line_delete(edit, *edit->point, *edit->point + 1U);
		break;
	case EDIT_UP:
		/* The older entry replaces the line, with the cursor at its end. */
		entry = previous_history();
		if (entry == NULL)
			break;
		replaced = replace_line(edit->text, edit->capacity, edit->length, edit->point, entry->line);
		if (!replaced)
			return 0;
		line_changed(0);
		break;
	case EDIT_DOWN:
		/* The newer entry replaces the line; past the newest the line is empty. */
		entry = next_history();
		if (entry != NULL) {
			replaced = replace_line(edit->text, edit->capacity, edit->length, edit->point, entry->line);
			if (!replaced)
				return 0;
			line_changed(0);
		} else if (history_position == history_length) {
			(*edit->text)[0] = '\0';
			*edit->length = 0;
			*edit->point = 0;
			line_changed(0);
		}

		break;
	default:
		break;
	}

	/* Succeeded. */
	return 1;
}

/*
 * Replaces the text of a line, growing its buffer, and puts the cursor at
 * the end; returns 0 without memory.
 */
static int
replace_line(
	char **line,
	size_t *capacity,
	size_t *length,
	size_t *point,
	const char *replacement)
{
	size_t size;
	int grown;

	/* Room for the replacement and its terminating null. */
	size = strlen(replacement);
	grown = grow(line, capacity, size + 1U);
	if (!grown)
		return 0;

	/* Succeeded: the new text, with the cursor after it. */
	memcpy(*line, replacement, size + 1U);
	*length = size;
	*point = size;
	return 1;
}

/*
 * Makes a line buffer hold at least need bytes, doubling its size; returns
 * 0 without memory, leaving the buffer as it was.
 */
static int
grow(
	char **line,
	size_t *capacity,
	size_t need)
{
	char *larger;
	size_t size;

	/* A buffer that is big enough stays. */
	if (need <= *capacity)
		return 1;

	/* The size doubles until it is enough, unless it would overflow. */
	size = *capacity;
	while (size < need) {
		if (size > (size_t)-1 / 2U)
			return 0;
		size *= 2U;
	}

	/* The larger buffer. */
	larger = realloc(*line, size);
	if (larger == NULL)
		return 0;

	/* Succeeded: the buffer is at least need bytes. */
	*line = larger;
	*capacity = size;
	return 1;
}

/* Inserts text at a position and leaves the cursor after it; returns 0 without memory. */
static int
line_insert(
	const struct edit_line *edit,
	size_t position,
	const char *text,
	size_t count)
{
	int grown;

	/* Room for the text and the terminating null. */
	grown = grow(edit->text, edit->capacity, *edit->length + count + 1U);
	if (!grown)
		return 0;

	/* The rest of the line moves after the text. */
	memmove(*edit->text + position + count, *edit->text + position, *edit->length - position + 1U);
	memcpy(*edit->text + position, text, count);
	*edit->length += count;
	*edit->point = position + count;
	line_changed(position);

	/* Succeeded. */
	return 1;
}

/* Removes the characters from start up to end and leaves the cursor at start. */
static void
line_delete(
	const struct edit_line *edit,
	size_t start,
	size_t end)
{
	/* Nothing past the end of the line is removed. */
	if (end > *edit->length)
		end = *edit->length;
	if (end <= start)
		return;

	/* The rest of the line moves over them. */
	memmove(*edit->text + start, *edit->text + end, *edit->length - end + 1U);
	*edit->length -= end - start;
	*edit->point = start;
	line_changed(start);
}

/* Notes that the line changed from a position on, for the redraw. */
static void
line_changed(
	size_t position)
{
	/* The redraw starts at the earliest change the key made. */
	if (position < line_changed_from)
		line_changed_from = position;
}

/*
 * Brings the terminal from the line it shows to the line as it is now: the
 * text from changed_from on is written again ((size_t)-1 when only the
 * cursor moved), spaces cover what a shorter line leaves, and the cursor
 * goes to point.
 */
static void
update_display(
	const char *line,
	size_t old_length,
	size_t old_point,
	size_t length,
	size_t point,
	size_t changed_from)
{
	size_t spaces;
	size_t drawn_to;

	/* Only the cursor moves. */
	if (changed_from == (size_t)-1) {
		move_cursor(old_point, point);
		return;
	}

	/* The text from the first change on is written again. */
	move_cursor(old_point, changed_from);
	(void)write_all(line + changed_from, length - changed_from);
	drawn_to = length;

	/* Spaces cover the end of a line that became shorter. */
	if (old_length > length) {
		for (spaces = old_length - length; spaces != 0U; spaces--)
			(void)write_all(" ", 1);
		drawn_to = old_length;
	}

	/* The cursor goes back to its place. */
	move_cursor(drawn_to, point);
}

/* Moves the terminal's cursor from one column of the line to another. */
static void
move_cursor(
	size_t from,
	size_t to)
{
	/* Left to an earlier column, right to a later one. */
	if (to < from) {
		cursor_step(from - to, 'D');
	} else {
		cursor_step(to - from, 'C');
	}
}

/*
 * Moves the terminal's cursor by columns with ESC [ n D (left) or
 * ESC [ n C (right).
 */
static void
cursor_step(
	size_t columns,
	char direction)
{
	char sequence[3U * sizeof(size_t) + 4U];
	size_t at;

	/* No move writes nothing. */
	if (columns == 0U)
		return;

	/* The sequence is built from its end: the letter, the digits, ESC [. */
	at = sizeof(sequence);
	at--;
	sequence[at] = direction;
	do {
		at--;
		sequence[at] = (char)('0' + columns % 10U);
		columns /= 10U;
	} while (columns != 0U);
	at--;
	sequence[at] = '[';
	at--;
	sequence[at] = '\033';

	/* The terminal gets the sequence. */
	(void)write_all(sequence + at, sizeof(sequence) - at);
}

/*
 * The vi editing mode (XCU sh, Command Line Editing (vi-mode)).
 *
 * A line starts in insert mode; ESC enters command mode, where keys move
 * the cursor, change the text through motions and operators, and walk the
 * history.  A count typed before a command repeats it.  An ESC followed by
 * [ and a letter is an arrow key as a terminal sends it.
 */

/* Starts the vi editor on a new line, in insert mode. */
static void
vi_begin_line(
	void)
{
	/* Nothing is pending from the line before. */
	vi_state.command = 0;
	vi_state.escape = 0;
	vi_state.replace_count = 0;
	vi_state.count = 0;
	vi_state.operator_count = 0;
	vi_state.pending_operator = 0;

	/* u works within one line; the yanked text stays for the next. */
	free(vi_state.undo);
	vi_state.undo = NULL;
	vi_state.undo_point = 0;
}

/* Takes one byte in vi mode and reports what it did to the line. */
static enum line_result
vi_key(
	const struct edit_line *edit,
	unsigned char byte)
{
	enum line_result result;

	/* Enter completes the line in either mode. */
	if (byte == '\r' || byte == '\n')
		return LINE_ACCEPT;

	/* Ctrl-C cancels the line in either mode. */
	if (byte == 3)
		return LINE_CANCEL;

	/* Ctrl-D on an empty line ends the input. */
	if (byte == 4 && *edit->length == 0)
		return LINE_END;

	/* ESC [ is the start of an arrow key. */
	if (vi_state.escape == 1 && byte == '[') {
		vi_state.escape = 2;
		return LINE_CONTINUE;
	}

	/* The arrow key's letter becomes the command that moves the same way. */
	if (vi_state.escape == 2) {
		vi_state.escape = 0;
		vi_state.command = 1;
		byte = vi_arrow(byte);
		if (byte == 0)
			return LINE_CONTINUE;
	}

	/* Any other byte ends an escape sequence. */
	vi_state.escape = 0;

	/* Insert mode takes text, command mode takes commands. */
	if (vi_state.command) {
		result = vi_command_key(edit, byte);
	} else {
		result = vi_insert_key(edit, byte);
	}

	/* In command mode the cursor rests on a character of the line. */
	if (vi_state.command && *edit->point >= *edit->length) {
		*edit->point = 0;
		if (*edit->length > 0)
			*edit->point = *edit->length - 1U;
	}

	/* Reports what the key did to the line. */
	return result;
}

/* Translates the letter of an arrow key to a vi command, or to 0. */
static unsigned char
vi_arrow(
	unsigned char letter)
{
	/* Up and down walk the history; right and left move the cursor. */
	switch (letter) {
	case 'A':
		return 'k';
	case 'B':
		return 'j';
	case 'C':
		return 'l';
	case 'D':
		return 'h';
	default:
		break;
	}

	/* Not an arrow key: nothing to do. */
	return 0;
}

/* Takes one byte in insert mode. */
static enum line_result
vi_insert_key(
	const struct edit_line *edit,
	unsigned char byte)
{
	size_t start;
	char character;
	int inserted;

	/* ESC leaves insert mode onto the last character typed. */
	if (byte == 0x1b) {
		vi_state.command = 1;
		vi_state.escape = 1;
		if (*edit->point > 0)
			(*edit->point)--;
		return LINE_CONTINUE;
	}

	/* Backspace erases the character before the cursor. */
	if (byte == 8 || byte == 0x7f) {
		if (*edit->point > 0)
			line_delete(edit, *edit->point - 1U, *edit->point);
		return LINE_CONTINUE;
	}

	/* Ctrl-U erases everything before the cursor. */
	if (byte == 21) {
		line_delete(edit, 0, *edit->point);
		return LINE_CONTINUE;
	}

	/* Ctrl-W erases the word before the cursor. */
	if (byte == 23) {
		start = vi_word_backward(edit, *edit->point, 0);
		line_delete(edit, start, *edit->point);
		return LINE_CONTINUE;
	}

	/* Other control characters are not text. */
	if (byte < 0x20)
		return LINE_CONTINUE;

	/* The character goes in at the cursor. */
	character = (char)byte;
	inserted = line_insert(edit, *edit->point, &character, 1);
	if (!inserted)
		return LINE_NOMEM;

	/* Succeeded. */
	return LINE_CONTINUE;
}

/* Takes one byte in command mode. */
static enum line_result
vi_command_key(
	const struct edit_line *edit,
	unsigned char byte)
{
	enum line_result result;
	unsigned count;
	size_t target;
	int inclusive;
	int moved;
	int digit;

	/* r takes this byte as the character that replaces. */
	if (vi_state.replace_count != 0) {
		count = vi_state.replace_count;
		vi_state.replace_count = 0;
		vi_replace_characters(edit, byte, count);
		return LINE_CONTINUE;
	}

	/* A digit adds to the count; a 0 that starts no count is a motion. */
	digit = 0;
	if (byte >= '1' && byte <= '9')
		digit = 1;
	else if (byte == '0' && vi_state.count > 0)
		digit = 1;
	if (digit) {
		if (vi_state.count < VI_COUNT_MAX)
			vi_state.count = vi_state.count * 10U + (unsigned)(byte - '0');
		return LINE_CONTINUE;
	}

	/* The count belongs to this command and is used up by it. */
	count = vi_state.count;
	vi_state.count = 0;
	if (count == 0)
		count = 1;

	/* ESC cancels an operator that waits for its motion. */
	if (byte == 0x1b) {
		vi_state.pending_operator = 0;
		vi_state.escape = 1;
		return LINE_CONTINUE;
	}

	/* An operator that waits takes this key as its motion. */
	if (vi_state.pending_operator != 0) {
		vi_operator_motion(edit, byte, count);
		return LINE_CONTINUE;
	}

	/* A motion moves the cursor. */
	moved = vi_motion(edit, byte, count, &target, &inclusive);
	if (moved) {
		*edit->point = target;
		return LINE_CONTINUE;
	}

	/* Anything else edits the line or walks the history. */
	result = vi_edit_command(edit, byte, count);
	if (result != LINE_CONTINUE)
		return result;

	/* Succeeded. */
	return LINE_CONTINUE;
}

/* Carries out the command keys that are neither counts nor motions. */
static enum line_result
vi_edit_command(
	const struct edit_line *edit,
	unsigned char byte,
	unsigned count)
{
	size_t start;
	int done;

	/* Chooses the command by its key. */
	switch (byte) {
	case 'd':
	case 'c':
	case 'y':
		/* The operator waits for the motion that bounds its text. */
		vi_state.pending_operator = byte;
		vi_state.operator_count = count;
		break;
	case 'D':
	case 'C':
		/* The rest of the line goes; C then inserts in its place. */
		vi_save_undo(edit);
		vi_keep(edit, *edit->point, *edit->length);
		line_delete(edit, *edit->point, *edit->length);
		if (byte == 'C')
			vi_state.command = 0;
		break;
	case 'S':
		/* The whole line goes, and insert mode starts on the empty line. */
		vi_save_undo(edit);
		vi_keep(edit, 0, *edit->length);
		line_delete(edit, 0, *edit->length);
		vi_state.command = 0;
		break;
	case 'i':
		/* Insert before the cursor. */
		vi_save_undo(edit);
		vi_state.command = 0;
		break;
	case 'a':
		/* Insert after the cursor. */
		vi_save_undo(edit);
		if (*edit->length > 0)
			(*edit->point)++;
		vi_state.command = 0;
		break;
	case 'I':
		/* Insert before the first character that is not a blank. */
		vi_save_undo(edit);
		*edit->point = vi_first_nonblank(edit);
		vi_state.command = 0;
		break;
	case 'A':
		/* Insert at the end of the line. */
		vi_save_undo(edit);
		*edit->point = *edit->length;
		vi_state.command = 0;
		break;
	case 'x':
	case 's':
		/* The characters under and after the cursor go; s then inserts. */
		vi_save_undo(edit);
		vi_keep(edit, *edit->point, *edit->point + count);
		line_delete(edit, *edit->point, *edit->point + count);
		if (byte == 's')
			vi_state.command = 0;
		break;
	case 'X':
		/* The characters before the cursor go. */
		start = vi_left(edit, count);
		vi_save_undo(edit);
		vi_keep(edit, start, *edit->point);
		line_delete(edit, start, *edit->point);
		break;
	case 'r':
		/* The next byte replaces count characters. */
		vi_state.replace_count = count;
		break;
	case '~':
		/* The case of count characters is inverted. */
		vi_save_undo(edit);
		vi_toggle_case(edit, count);
		break;
	case 'p':
	case 'P':
		/* The yanked text goes in after (p) or before (P) the cursor. */
		done = vi_put(edit, byte, count);
		if (!done)
			return LINE_NOMEM;
		break;
	case 'u':
		/* The last change is undone. */
		done = vi_undo(edit);
		if (!done)
			return LINE_NOMEM;
		break;
	case 'k':
	case '-':
		/* An older history entry replaces the line. */
		done = vi_history(edit, 1, count);
		if (!done)
			return LINE_NOMEM;
		break;
	case 'j':
	case '+':
		/* A newer history entry replaces the line. */
		done = vi_history(edit, 0, count);
		if (!done)
			return LINE_NOMEM;
		break;
	case 'G':
		/* The oldest history entry replaces the line. */
		done = vi_oldest_history(edit);
		if (!done)
			return LINE_NOMEM;
		break;
	case '#':
		/* The line becomes a comment and is complete. */
		done = line_insert(edit, 0, "#", 1);
		if (!done)
			return LINE_NOMEM;
		return LINE_ACCEPT;
	default:
		break;
	}

	/* Succeeded. */
	return LINE_CONTINUE;
}

/* Applies the waiting operator to the text that a motion key bounds. */
static void
vi_operator_motion(
	const struct edit_line *edit,
	unsigned char motion,
	unsigned count)
{
	unsigned char operator_key;
	unsigned total;
	unsigned step;
	size_t target;
	int inclusive;
	int moved;
	int word_class;
	int big;

	/* The operator is used up by this key, whatever the key turns out to be. */
	operator_key = vi_state.pending_operator;
	vi_state.pending_operator = 0;
	total = count * vi_state.operator_count;

	/* The operator doubled (dd, cc, yy) covers the whole line. */
	if (motion == operator_key) {
		*edit->point = 0;
		vi_operate(edit, operator_key, *edit->length, 0);
		return;
	}

	/* On a word, cw changes only to the end of that word, as ce would. */
	word_class = 0;
	big = 0;
	if (motion == 'W')
		big = 1;
	if (*edit->point < *edit->length)
		word_class = vi_class((*edit->text)[*edit->point], big);
	if (operator_key == 'c' && word_class != 0) {
		if (motion == 'w' || motion == 'W') {
			target = vi_current_word_end(edit, *edit->point, big);
			for (step = 1; step < total; step++)
				target = vi_word_end(edit, target, big);
			vi_operate(edit, operator_key, target, 1);
			return;
		}
	}

	/* A key that is no motion cancels the operator. */
	moved = vi_motion(edit, motion, total, &target, &inclusive);
	if (!moved)
		return;

	/* The text between the cursor and where the motion goes. */
	vi_operate(edit, operator_key, target, inclusive);
}

/* Applies d, c or y to the text between the cursor and a target. */
static void
vi_operate(
	const struct edit_line *edit,
	unsigned char operator_key,
	size_t target,
	int inclusive)
{
	size_t start;
	size_t end;

	/* The text runs from the nearer of the cursor and the target. */
	start = *edit->point;
	end = target;
	if (target < start) {
		start = target;
		end = *edit->point;
	}

	/* An inclusive motion covers the character it lands on. */
	if (inclusive && end < *edit->length)
		end++;

	/* y keeps the text and leaves the line alone. */
	if (operator_key == 'y') {
		vi_keep(edit, start, end);
		*edit->point = start;
		return;
	}

	/* d and c remove the text, and c goes on to insert in its place. */
	vi_save_undo(edit);
	vi_keep(edit, start, end);
	line_delete(edit, start, end);
	if (operator_key == 'c')
		vi_state.command = 0;
}

/*
 * Finds where a motion key takes the cursor.  inclusive is set for a motion
 * whose last character is part of what an operator covers (e, E, $).
 * Returns 0 when the key is not a motion.
 */
static int
vi_motion(
	const struct edit_line *edit,
	unsigned char motion,
	unsigned count,
	size_t *target,
	int *inclusive)
{
	size_t position;
	unsigned step;
	int big;

	/* The motion starts at the cursor; W, B and E go by big words. */
	*inclusive = 0;
	position = *edit->point;
	big = 0;
	if (motion == 'W' || motion == 'B' || motion == 'E')
		big = 1;

	/* Chooses the motion by its key. */
	switch (motion) {
	case 'h':
	case 8:
	case 0x7f:
		position = vi_left(edit, count);
		break;
	case 'l':
	case ' ':
		position += count;
		if (position > *edit->length)
			position = *edit->length;
		break;
	case '0':
		position = 0;
		break;
	case '^':
		position = vi_first_nonblank(edit);
		break;
	case '$':
		position = 0;
		if (*edit->length > 0)
			position = *edit->length - 1U;
		*inclusive = 1;
		break;
	case 'w':
	case 'W':
		for (step = 0; step < count; step++)
			position = vi_word_forward(edit, position, big);
		break;
	case 'b':
	case 'B':
		for (step = 0; step < count; step++)
			position = vi_word_backward(edit, position, big);
		break;
	case 'e':
	case 'E':
		for (step = 0; step < count; step++)
			position = vi_word_end(edit, position, big);
		*inclusive = 1;
		break;
	default:
		return 0;
	}

	/* Succeeded: where the cursor goes. */
	*target = position;
	return 1;
}

/* Returns the position count characters left of the cursor. */
static size_t
vi_left(
	const struct edit_line *edit,
	unsigned count)
{
	/* Not past the start of the line. */
	if (count > *edit->point)
		return 0;

	/* Succeeded. */
	return *edit->point - count;
}

/* Returns the position of the first character that is not a blank. */
static size_t
vi_first_nonblank(
	const struct edit_line *edit)
{
	size_t position;
	int character_class;

	/* Past the leading blanks. */
	position = 0;
	while (position < *edit->length) {
		character_class = vi_class((*edit->text)[position], 1);
		if (character_class != 0)
			break;
		position++;
	}

	/* Succeeded. */
	return position;
}

/*
 * Classifies a character for word motions: 0 for a blank, 1 for a word
 * character, 2 for punctuation.  A big word (W, B, E) is anything but
 * blanks.
 */
static int
vi_class(
	char character,
	int big)
{
	/* Blanks separate every kind of word. */
	if (character == ' ' || character == '\t')
		return 0;

	/* A big word takes in everything else. */
	if (big)
		return 1;

	/* Letters, digits and _ make a word. */
	if (character >= 'a' && character <= 'z')
		return 1;
	if (character >= 'A' && character <= 'Z')
		return 1;
	if (character >= '0' && character <= '9')
		return 1;
	if (character == '_')
		return 1;

	/* Anything else is punctuation, a word of its own kind. */
	return 2;
}

/* Returns where the next word starts (w, W). */
static size_t
vi_word_forward(
	const struct edit_line *edit,
	size_t position,
	int big)
{
	int start_class;
	int character_class;

	/* The end of the line has no word after it. */
	if (position >= *edit->length)
		return *edit->length;

	/* Past the rest of the word under the cursor, if it is on one. */
	start_class = vi_class((*edit->text)[position], big);
	while (start_class != 0 && position < *edit->length) {
		character_class = vi_class((*edit->text)[position], big);
		if (character_class != start_class)
			break;
		position++;
	}

	/* Past the blanks before the next word. */
	while (position < *edit->length) {
		character_class = vi_class((*edit->text)[position], big);
		if (character_class != 0)
			break;
		position++;
	}

	/* Succeeded: the start of the next word, or the end of the line. */
	return position;
}

/* Returns where the word before a position starts (b, B). */
static size_t
vi_word_backward(
	const struct edit_line *edit,
	size_t position,
	int big)
{
	int word_class;
	int character_class;

	/* Nothing lies before the start of the line. */
	if (position == 0)
		return 0;

	/* Back over the blanks before the position. */
	position--;
	while (position > 0) {
		character_class = vi_class((*edit->text)[position], big);
		if (character_class != 0)
			break;
		position--;
	}

	/* Back to the first character of that word. */
	word_class = vi_class((*edit->text)[position], big);
	while (position > 0) {
		character_class = vi_class((*edit->text)[position - 1U], big);
		if (character_class != word_class)
			break;
		position--;
	}

	/* Succeeded: the start of the word. */
	return position;
}

/* Returns where the word after a position ends (e, E), inclusive. */
static size_t
vi_word_end(
	const struct edit_line *edit,
	size_t position,
	int big)
{
	size_t end;
	int character_class;

	/* The last character has no word after it. */
	if (position + 1U >= *edit->length)
		return position;

	/* Past the blanks after the position. */
	position++;
	while (position < *edit->length) {
		character_class = vi_class((*edit->text)[position], big);
		if (character_class != 0)
			break;
		position++;
	}

	/* Only blanks follow: the last character. */
	if (position >= *edit->length)
		return *edit->length - 1U;

	/* Succeeded: the end of that word. */
	end = vi_current_word_end(edit, position, big);
	return end;
}

/* Returns the last character of the word a position is in. */
static size_t
vi_current_word_end(
	const struct edit_line *edit,
	size_t position,
	int big)
{
	int word_class;
	int character_class;

	/* Forward while the next character is of the same kind. */
	word_class = vi_class((*edit->text)[position], big);
	while (position + 1U < *edit->length) {
		character_class = vi_class((*edit->text)[position + 1U], big);
		if (character_class != word_class)
			break;
		position++;
	}

	/* Succeeded. */
	return position;
}



/* Keeps the characters from start up to end for p and P. */
static void
vi_keep(
	const struct edit_line *edit,
	size_t start,
	size_t end)
{
	char *copy;
	size_t count;

	/* Nothing past the end of the line is kept, and nothing empty. */
	if (end > *edit->length)
		end = *edit->length;
	if (end <= start)
		return;

	/* A copy replaces what was kept before; without memory the old stays. */
	count = end - start;
	copy = malloc(count + 1U);
	if (copy == NULL)
		return;

	/* The copy becomes the yanked text. */
	memcpy(copy, *edit->text + start, count);
	copy[count] = '\0';
	free(vi_state.yank);
	vi_state.yank = copy;
}

/* Keeps the line as it is, for u to bring back. */
static void
vi_save_undo(
	const struct edit_line *edit)
{
	char *copy;

	/* A copy of the text; without memory the older copy stays. */
	copy = malloc(*edit->length + 1U);
	if (copy == NULL)
		return;

	/* The copy becomes what u brings back. */
	memcpy(copy, *edit->text, *edit->length + 1U);
	free(vi_state.undo);
	vi_state.undo = copy;
	vi_state.undo_point = *edit->point;
}

/* Brings back the line as it was before the last change; returns 0 without memory. */
static int
vi_undo(
	const struct edit_line *edit)
{
	char *current;
	size_t current_point;
	int replaced;

	/* Nothing has changed on this line. */
	if (vi_state.undo == NULL)
		return 1;

	/* The line as it is now becomes what the next u brings back. */
	current = malloc(*edit->length + 1U);
	if (current == NULL)
		return 0;

	/* It is copied with its cursor. */
	memcpy(current, *edit->text, *edit->length + 1U);
	current_point = *edit->point;

	/* The saved line replaces it, with the cursor where it was. */
	replaced = replace_line(edit->text, edit->capacity, edit->length, edit->point, vi_state.undo);
	if (!replaced) {
		free(current);
		return 0;
	}

	/* The cursor goes back, and the current line is kept for the next u. */
	*edit->point = vi_state.undo_point;
	free(vi_state.undo);
	vi_state.undo = current;
	vi_state.undo_point = current_point;
	line_changed(0);

	/* Succeeded. */
	return 1;
}

/* Puts the yanked text count times after (p) or before (P) the cursor. */
static int
vi_put(
	const struct edit_line *edit,
	unsigned char command,
	unsigned count)
{
	size_t position;
	size_t size;
	unsigned step;
	int inserted;

	/* Nothing has been yanked. */
	if (vi_state.yank == NULL)
		return 1;

	/* p puts after the character under the cursor, P before it. */
	vi_save_undo(edit);
	position = *edit->point;
	if (command == 'p' && *edit->length > 0)
		position++;

	/* Each copy goes after the one before. */
	size = strlen(vi_state.yank);
	for (step = 0; step < count; step++) {
		inserted = line_insert(edit, position, vi_state.yank, size);
		if (!inserted)
			return 0;
		position += size;
	}

	/* The cursor rests on the last character put. */
	if (*edit->point > 0)
		(*edit->point)--;

	/* Succeeded. */
	return 1;
}

/* Replaces count characters from the cursor with one character. */
static void
vi_replace_characters(
	const struct edit_line *edit,
	unsigned char byte,
	unsigned count)
{
	/* A control character (ESC among them) cancels r, as does a count too long. */
	if (byte < 0x20)
		return;
	if (*edit->point + count > *edit->length)
		return;

	/* The characters change in place and the cursor rests on the last. */
	vi_save_undo(edit);
	memset(*edit->text + *edit->point, byte, count);
	line_changed(*edit->point);
	*edit->point += count - 1U;
}

/* Inverts the case of count characters from the cursor, moving past them. */
static void
vi_toggle_case(
	const struct edit_line *edit,
	unsigned count)
{
	unsigned step;
	char character;

	/* The line is redrawn from the cursor. */
	line_changed(*edit->point);

	/* Each character up to the end of the line. */
	for (step = 0; step < count; step++) {
		if (*edit->point >= *edit->length)
			break;

		/* A lower-case letter becomes upper case and the reverse. */
		character = (*edit->text)[*edit->point];
		if (character >= 'a' && character <= 'z')
			character = (char)(character - 'a' + 'A');
		else if (character >= 'A' && character <= 'Z')
			character = (char)(character - 'A' + 'a');
		(*edit->text)[*edit->point] = character;
		(*edit->point)++;
	}
}

/*
 * Replaces the line with the entry count steps older or newer in the
 * history.  Past the newest entry the line is empty.  Returns 0 without
 * memory.
 */
static int
vi_history(
	const struct edit_line *edit,
	int older,
	unsigned count)
{
	const char *entry;
	int start;
	int replaced;
	unsigned step;

	/* Walks count steps, stopping at either end. */
	start = history_position;
	for (step = 0; step < count; step++) {
		if (older) {
			previous_history();
		} else {
			next_history();
		}
	}

	/* No entry to walk to leaves the line as it is. */
	if (history_position == start)
		return 1;

	/* Past the newest entry the line is empty. */
	if (history_position == history_length) {
		(*edit->text)[0] = '\0';
		*edit->length = 0;
		*edit->point = 0;
		line_changed(0);
		return 1;
	}

	/* The entry replaces the line, with the cursor at its start. */
	entry = history_entries[history_position].line;
	replaced = replace_line(edit->text, edit->capacity, edit->length, edit->point, entry);
	if (!replaced)
		return 0;

	/* The cursor starts at the beginning of the entry. */
	*edit->point = 0;
	line_changed(0);

	/* Succeeded. */
	return 1;
}

/* Replaces the line with the oldest history entry; returns 0 without memory. */
static int
vi_oldest_history(
	const struct edit_line *edit)
{
	const char *entry;
	int replaced;

	/* An empty history leaves the line as it is. */
	if (history_length == 0)
		return 1;

	/* The oldest entry replaces the line, with the cursor at its start. */
	history_position = 0;
	entry = history_entries[0].line;
	replaced = replace_line(edit->text, edit->capacity, edit->length, edit->point, entry);
	if (!replaced)
		return 0;

	/* The cursor starts at the beginning of the entry. */
	*edit->point = 0;
	line_changed(0);

	/* Succeeded. */
	return 1;
}

