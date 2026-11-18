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
 * HISTORY_MAX lines that the caller adds, or as many as stifle_history()
 * names.
 */

#include "readline/readline.h"
#include "readline/history.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* How many lines the history keeps until stifle_history() names another number. */
#define HISTORY_MAX 32

/* The first size of a line buffer, which doubles as the line grows. */
#define LINE_INITIAL 128U

/* The largest count a vi command takes; more digits are ignored. */
#define VI_COUNT_MAX 9999U

/* The width a list of matches is laid out for when the terminal does not say. */
#define COMPLETION_WIDTH 80U

/* The blanks between two columns of a list of matches. */
#define COMPLETION_GAP 2U

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
 * The caller's completion function, called for Tab; NULL (the default)
 * leaves Tab doing nothing.  readline.h describes what it returns.
 */
rl_completion_func_t *rl_attempted_completion_function;

/* The characters that end a word going back from the cursor, GNU Readline's default. */
static char completion_default_breaks[] = " \t\n\"\\'`@$><=;|&{(";

/*
 * The characters that separate the word to complete from what is before
 * it.  The caller may point it at its own set.
 */
char *rl_completer_word_break_characters = completion_default_breaks;

/*
 * Tells whether the character at an index of the line is quoted, so that a
 * break character is part of the word; NULL treats none as quoted.
 */
rl_linebuf_func_t *rl_char_is_quoted_p;

/*
 * What follows a single match; '\0' for nothing.  It is set back to ' '
 * before each call of the completion function, which may change it.
 */
int rl_completion_append_character = ' ';

/* How many matches are listed without asking first. */
int rl_completion_query_items = 100;

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
 * The array grows as lines are added, up to history_limit entries.  The
 * first history_length entries hold lines this file allocated; the rest of
 * the history_capacity entries are empty.
 */
static HIST_ENTRY *history_entries;

/* How many entries the history_entries array has room for. */
static int history_capacity;

/*
 * How many lines the history keeps: HISTORY_MAX, or what stifle_history()
 * set.  When a line is added to a full history the oldest one goes.
 */
static int history_limit = HISTORY_MAX;

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

/*
 * The prompt of the line being edited, written again after a list of
 * matches.  It points at the caller's string for one call of readline().
 */
static const char *completion_prompt;

/*
 * Where the terminal's cursor is, as a row and a column counted from the
 * start of the prompt (BUG-173): the line is drawn by display columns, a
 * wide character taking two and a line longer than the terminal wrapping
 * to the next row, so that the cursor can be brought back across rows.
 * Set when the prompt is written; kept by every move and redraw of one
 * call of readline().
 */
static size_t display_row;
static size_t display_col;

/* Where the text drawn last ends (the same count), so that only a shorter line clears what is past its end. */
static size_t display_end_row;
static size_t display_end_col;

/*
 * Whether the key before this one was a Tab, and whether this one is.
 * readline() moves the second to the first before each key; a Tab that
 * leaves the line as it was lists the matches when the first is set.
 */
static int completion_previous_tab;
static int completion_this_tab;

/*
 * Set by a Tab that rings the bell; readline() rings it after the line is
 * drawn, so that the bell follows what the Tab changed.
 */
static int completion_bell;

static void history_drop_oldest(void);
static int history_grow(void);
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
static void update_display(const char *line, size_t length, size_t point, size_t changed_from);
static void redraw(const char *line, size_t length, size_t point, size_t from);
static void prompt_drawn(void);
static void line_position(const char *line, size_t upto, size_t width, size_t *row, size_t *col);
static void text_advance(const char *text, size_t size, size_t width, size_t *row, size_t *col);
static size_t char_width(const unsigned char *bytes, size_t size, size_t *used);
static size_t terminal_columns(void);
static void cursor_to(size_t row, size_t col);
static void cursor_step(size_t columns, char direction);
static size_t char_next(const char *text, size_t length, size_t at);
static size_t char_previous(const char *text, size_t at);
static int char_incomplete(const char *text, size_t at);
static enum line_result complete_key(const struct edit_line *edit);
static size_t complete_word_start(const struct edit_line *edit);
static int complete_replace(const struct edit_line *edit, size_t start, const char *replacement);
static int complete_append(const struct edit_line *edit, int character);
static void complete_list(const struct edit_line *edit, char **matches);
static int complete_ask(size_t count);
static void complete_columns(char **matches, size_t count);
static void complete_free(char **matches);
static void write_bell(void);
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
	int grown;

	/* An empty line is not worth recalling. */
	if (line == NULL || line[0] == '\0')
		return;

	/* A history stifled to nothing keeps no line. */
	if (history_limit == 0)
		return;

	/* The history keeps a copy; without memory the line is not kept. */
	copy = duplicate(line);
	if (copy == NULL)
		return;

	/* A full history drops its oldest entry, and the numbers move up. */
	if (history_length >= history_limit)
		history_drop_oldest();

	/* Room for one more entry; without memory the line is not kept. */
	grown = history_grow();
	if (!grown) {
		free(copy);
		return;
	}

	/* The line is the newest entry, and the walk starts past it. */
	history_entries[history_length].line = copy;
	history_entries[history_length].timestamp = NULL;
	history_length++;
	history_position = history_length;
}

/*
 * Keeps at most max entries from now on (a negative max counts as 0), and
 * drops the oldest entries over it now.
 */
void
stifle_history(
	int max)
{
	/* A negative limit keeps nothing, as GNU Readline treats it. */
	if (max < 0)
		max = 0;

	/* The oldest entries over the new limit go, and the numbers move up. */
	history_limit = max;
	while (history_length > history_limit)
		history_drop_oldest();

	/* The walk starts again past the newest entry. */
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
	size_t end_row;
	size_t end_col;
	int attributes_error;
	int terminal;
	int partial;

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

	/* The prompt, which a list of matches writes again; no Tab has come yet. */
	completion_prompt = prompt;
	completion_previous_tab = 0;
	completion_this_tab = 0;
	(void)write_all(prompt, strlen(prompt));
	prompt_drawn();

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

		/* The key edits the line in the current mode; a Tab marks itself as one. */
		line_changed_from = (size_t)-1;
		completion_previous_tab = completion_this_tab;
		completion_this_tab = 0;
		completion_bell = 0;
		if (rl_editing_mode == 0) {
			result = vi_key(&edit, byte);
		} else {
			result = emacs_key(&edit, byte);
		}

		/* The caller sees the line as the key left it; the terminal too, unless a character is only partly typed. */
		rl_line_buffer = line;
		rl_point = (int)point;
		rl_end = (int)length;
		partial = char_incomplete(line, point);
		if (!partial)
			update_display(line, length, point, line_changed_from);

		/* A Tab that found nothing to do rings the bell after the line is drawn. */
		if (completion_bell)
			write_bell();

		/* Any result but going on ends the line. */
		if (result != LINE_CONTINUE)
			break;
	}

	/* The cursor goes to the end of the line (its last row) before the next line starts. */
	line_position(line, length, terminal_columns(), &end_row, &end_col);
	cursor_to(end_row, end_col);

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

/* Frees the oldest history entry and moves the others down; the numbers move up. */
static void
history_drop_oldest(
	void)
{
	/* An empty history has nothing to drop. */
	if (history_length == 0)
		return;

	/* The oldest line goes and the newer entries take its place. */
	free(history_entries[0].line);
	memmove(&history_entries[0], &history_entries[1], (size_t)(history_length - 1) * sizeof(history_entries[0]));
	history_length--;
	history_entries[history_length].line = NULL;
	history_entries[history_length].timestamp = NULL;
	history_base++;
}

/* Makes room for one more history entry; returns 1, or 0 without memory. */
static int
history_grow(
	void)
{
	HIST_ENTRY *larger;
	int capacity;

	/* An array with a free entry stays. */
	if (history_length < history_capacity)
		return 1;

	/* The array doubles, from room for HISTORY_MAX lines, but not past the limit. */
	capacity = HISTORY_MAX;
	if (history_capacity > 0) {
		capacity = history_capacity;
		if (capacity > history_limit / 2)
			capacity = history_limit;
		else
			capacity *= 2;
	}

	/* The limit may be below the first size, but never below one more entry. */
	if (capacity <= history_length)
		capacity = history_length + 1;

	/* The larger array. */
	larger = realloc(history_entries, (size_t)capacity * sizeof(history_entries[0]));
	if (larger == NULL)
		return 0;

	/* Succeeded: the array has room for one more entry. */
	history_entries = larger;
	history_capacity = capacity;
	return 1;
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
	enum line_result result;
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
		line_delete(edit, *edit->point, char_next(*edit->text, *edit->length, *edit->point));
		return LINE_CONTINUE;
	}

	/* Backspace erases the character before the cursor (all its bytes). */
	if (byte == 8 || byte == 0x7f) {
		if (*edit->point > 0)
			line_delete(edit, char_previous(*edit->text, *edit->point), *edit->point);
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

	/* Tab completes the word before the cursor. */
	if (byte == '\t') {
		result = complete_key(edit);
		return result;
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
		*edit->point = char_previous(*edit->text, *edit->point);
		break;
	case EDIT_RIGHT:
		*edit->point = char_next(*edit->text, *edit->length, *edit->point);
		break;
	case EDIT_DELETE:
		line_delete(edit, *edit->point, char_next(*edit->text, *edit->length, *edit->point));
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
 * Brings the terminal to the line as it is now: the whole line is drawn
 * again when it changed (changed_from is (size_t)-1 when only the cursor
 * moved), and the cursor goes to point.
 */
static void
update_display(
	const char *line,
	size_t length,
	size_t point,
	size_t changed_from)
{
	size_t row;
	size_t col;

	/* Only the cursor moves. */
	if (changed_from == (size_t)-1) {
		line_position(line, point, terminal_columns(), &row, &col);
		cursor_to(row, col);
		return;
	}

	/* The line again from its first change. */
	redraw(line, length, point, changed_from);
}

/*
 * Draws the line again from a byte offset on (the text before it is as it
 * was drawn): the text, the rest of the screen below it cleared when the
 * line became shorter, and the cursor at point.  A line that ends exactly
 * at the terminal's right edge goes on to the next row, so that the cursor
 * is never left in the pending wrap.
 */
static void
redraw(
	const char *line,
	size_t length,
	size_t point,
	size_t from)
{
	size_t width;
	size_t row;
	size_t col;

	/* From the offset (on a character's start), to the end. */
	width = terminal_columns();
	if (from > length)
		from = length;
	while (from > 0U && ((unsigned char)line[from] & 0xc0U) == 0x80U)
		from--;
	line_position(line, from, width, &row, &col);
	cursor_to(row, col);
	(void)write_all(line + from, length - from);

	/* Where the text ends: the next row's start when it filled its last row. */
	row = 0;
	col = 0;
	text_advance(completion_prompt, strlen(completion_prompt), width, &row, &col);
	text_advance(line, length, width, &row, &col);
	if (col >= width) {
		(void)write_all("\r\n", 2);
		row++;
		col = 0;
	}

	/* The cursor is at the text's end. */
	display_row = row;
	display_col = col;

	/* What a longer line left is cleared (from here to the end of the screen). */
	if (display_end_row > row || (display_end_row == row && display_end_col > col))
		(void)write_all("\033[J", 3);
	display_end_row = row;
	display_end_col = col;

	/* The cursor at point. */
	line_position(line, point, terminal_columns(), &row, &col);
	cursor_to(row, col);
}

/*
 * Notes that the prompt was just written from the first column: the cursor
 * is where it ended (on the next row when it filled its last one).
 */
static void
prompt_drawn(
	void)
{
	size_t width;

	/* The prompt's end. */
	width = terminal_columns();
	display_row = 0;
	display_col = 0;
	text_advance(completion_prompt, strlen(completion_prompt), width, &display_row, &display_col);

	/* A prompt that filled its last row leaves the cursor on the next one. */
	if (display_col >= width) {
		(void)write_all("\r\n", 2);
		display_row++;
		display_col = 0;
	}

	/* No text is drawn after it yet. */
	display_end_row = display_row;
	display_end_col = display_col;
}

/*
 * Tells the row and column of a byte offset of the line, counted from the
 * start of the prompt; an offset at a full row's end is the next row's start.
 */
static void
line_position(
	const char *line,
	size_t upto,
	size_t width,
	size_t *row,
	size_t *col)
{
	/* The prompt, then the line's text before the offset. */
	*row = 0;
	*col = 0;
	text_advance(completion_prompt, strlen(completion_prompt), width, row, col);
	text_advance(line, upto, width, row, col);

	/* The pending wrap is the next row's start. */
	if (*col >= width) {
		(*row)++;
		*col = 0;
	}
}

/*
 * Moves a row and a column over text as a terminal writes it: each
 * character takes its display width, and one that does not fit in what is
 * left of the row starts the next row.
 */
static void
text_advance(
	const char *text,
	size_t size,
	size_t width,
	size_t *row,
	size_t *col)
{
	size_t at;
	size_t used;
	size_t cells;

	/* Each character. */
	at = 0;
	while (at < size) {
		cells = char_width((const unsigned char *)text + at, size - at, &used);
		at += used;
		if (cells == 0U)
			continue;

		/* Not enough room left: the next row. */
		if (*col + cells > width) {
			(*row)++;
			*col = 0;
		}

		/* The character's cells. */
		*col += cells;
	}
}

/*
 * Tells the display width of the character at bytes (0, 1 or 2 columns)
 * and how many bytes it takes: UTF-8 decoded, an escape sequence (CSI)
 * taking no column, a control character none, a byte that is no UTF-8 one
 * column.  The wide ranges are libc's wcwidth's (src/libc/wide.c), so that
 * the editor counts as the system's terminal draws.
 */
static size_t
char_width(
	const unsigned char *bytes,
	size_t size,
	size_t *used)
{
	unsigned long value;
	size_t count;
	size_t index;

	/* An escape sequence: ESC [ parameters and a final letter. */
	*used = 1;
	if (bytes[0] == 0x1bU) {
		if (size >= 2U && bytes[1] == '[') {
			index = 2;
			while (index < size && (bytes[index] < 0x40U || bytes[index] > 0x7eU))
				index++;
			if (index < size)
				index++;
			*used = index;
		}

		/* No column. */
		return 0;
	}

	/* ASCII: a control character takes none, the others one. */
	if (bytes[0] < 0x80U) {
		if (bytes[0] < 0x20U || bytes[0] == 0x7fU)
			return 0;
		return 1;
	}

	/* The lead byte says how many continuation bytes follow; a stray byte is one column. */
	count = 0;
	value = 0;
	if ((bytes[0] & 0xe0U) == 0xc0U) {
		count = 1;
		value = bytes[0] & 0x1fU;
	} else if ((bytes[0] & 0xf0U) == 0xe0U) {
		count = 2;
		value = bytes[0] & 0x0fU;
	} else if ((bytes[0] & 0xf8U) == 0xf0U) {
		count = 3;
		value = bytes[0] & 0x07U;
	}

	/* A stray byte, or a sequence cut short, is one column. */
	if (count == 0U || count >= size)
		return 1;
	for (index = 1; index <= count; index++) {
		if ((bytes[index] & 0xc0U) != 0x80U)
			return 1;
		value = (value << 6) | (bytes[index] & 0x3fU);
	}

	/* The whole character is used. */
	*used = count + 1U;

	/* Combining marks take none. */
	if ((value >= 0x0300UL && value <= 0x036fUL) ||
	    (value >= 0x1ab0UL && value <= 0x1affUL) ||
	    (value >= 0x1dc0UL && value <= 0x1dffUL) ||
	    (value >= 0x20d0UL && value <= 0x20ffUL) ||
	    (value >= 0xfe00UL && value <= 0xfe0fUL) ||
	    (value >= 0xfe20UL && value <= 0xfe2fUL))
		return 0;

	/* East Asian wide and fullwidth characters take two. */
	if ((value >= 0x1100UL && value <= 0x115fUL) ||
	    (value >= 0x2e80UL && value <= 0xa4cfUL) ||
	    (value >= 0xac00UL && value <= 0xd7a3UL) ||
	    (value >= 0xf900UL && value <= 0xfaffUL) ||
	    (value >= 0xfe10UL && value <= 0xfe6fUL) ||
	    (value >= 0xff01UL && value <= 0xff60UL) ||
	    (value >= 0xffe0UL && value <= 0xffe6UL) ||
	    (value >= 0x1f300UL && value <= 0x1faffUL) ||
	    (value >= 0x20000UL && value <= 0x3fffdUL))
		return 2;

	/* Succeeded: one column. */
	return 1;
}

/* Tells the terminal's width in columns, or the usual 80 when it does not say. */
static size_t
terminal_columns(
	void)
{
	struct winsize size;
	int error;

	/* What the terminal says. */
	memset(&size, 0, sizeof(size));
	error = ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
	if (error != 0 || size.ws_col == 0)
		return COMPLETION_WIDTH;

	/* Succeeded: its width. */
	return size.ws_col;
}

/*
 * Moves the terminal's cursor to a row and a column of the line: up or down
 * (ESC [ n A, ESC [ n B), then left or right (ESC [ n D, ESC [ n C).
 */
static void
cursor_to(
	size_t row,
	size_t col)
{
	/* The row. */
	if (row < display_row) {
		cursor_step(display_row - row, 'A');
	} else {
		cursor_step(row - display_row, 'B');
	}

	/* The column. */
	if (col < display_col) {
		cursor_step(display_col - col, 'D');
	} else {
		cursor_step(col - display_col, 'C');
	}

	/* Succeeded: the cursor is there. */
	display_row = row;
	display_col = col;
}

/* Gives the offset of the character after the one at an offset (over UTF-8's continuation bytes). */
static size_t
char_next(
	const char *text,
	size_t length,
	size_t at)
{
	/* At the end, nothing comes after. */
	if (at >= length)
		return length;

	/* One byte, then its continuation bytes. */
	at++;
	while (at < length && ((unsigned char)text[at] & 0xc0U) == 0x80U)
		at++;

	/* Succeeded: the next character's offset. */
	return at;
}

/* Gives the offset of the character before an offset (over UTF-8's continuation bytes). */
static size_t
char_previous(
	const char *text,
	size_t at)
{
	/* At the start, nothing comes before. */
	if (at == 0U)
		return 0;

	/* One byte back, then back over continuation bytes to the lead byte. */
	at--;
	while (at > 0U && ((unsigned char)text[at] & 0xc0U) == 0x80U)
		at--;

	/* Succeeded: the previous character's offset. */
	return at;
}

/*
 * Tells whether the bytes just before an offset are a UTF-8 character
 * still being typed (its lead byte without all its continuation bytes).
 */
static int
char_incomplete(
	const char *text,
	size_t at)
{
	size_t lead;
	size_t need;
	unsigned char byte;

	/* The lead byte of the last character before the offset. */
	if (at == 0U)
		return 0;
	lead = char_previous(text, at);
	byte = (unsigned char)text[lead];

	/* How many bytes it needs. */
	need = 1;
	if ((byte & 0xe0U) == 0xc0U)
		need = 2;
	else if ((byte & 0xf0U) == 0xe0U)
		need = 3;
	else if ((byte & 0xf8U) == 0xf0U)
		need = 4;

	/* Fewer bytes than it needs are there. */
	if (at - lead < need)
		return 1;

	/* Succeeded: complete. */
	return 0;
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
 * Completes the word before the cursor (Tab).
 *
 * One match replaces the word and is followed by the append character.
 * Several put what they have in common in its place; when that leaves the
 * line as it was and the key before was a Tab too, they are listed.  The
 * bell rings (after the line is drawn) for no match and for matches not
 * listed.
 */
static enum line_result
complete_key(
	const struct edit_line *edit)
{
	char **matches;
	char *word;
	size_t start;
	size_t length;
	int changed;
	int appended;
	int previous_tab;

	/* This key is a Tab, and whether the one before was is kept for the decision below. */
	previous_tab = completion_previous_tab;
	completion_this_tab = 1;

	/* Without a completion function Tab does nothing. */
	if (rl_attempted_completion_function == NULL)
		return LINE_CONTINUE;

	/* The word runs back from the cursor to a break character. */
	start = complete_word_start(edit);
	length = *edit->point - start;
	word = malloc(length + 1U);
	if (word == NULL)
		return LINE_NOMEM;
	memcpy(word, *edit->text + start, length);
	word[length] = '\0';

	/* The function sees the whole line, and the append character starts as a blank. */
	rl_line_buffer = *edit->text;
	rl_point = (int)*edit->point;
	rl_end = (int)*edit->length;
	rl_completion_append_character = ' ';
	matches = rl_attempted_completion_function(word, (int)start, (int)*edit->point);
	free(word);

	/* No match: only the bell. */
	if (matches == NULL) {
		completion_bell = 1;
		return LINE_CONTINUE;
	}

	/* An empty array is no match either. */
	if (matches[0] == NULL) {
		complete_free(matches);
		completion_bell = 1;
		return LINE_CONTINUE;
	}

	/* The match, or what the matches have in common, takes the word's place. */
	changed = complete_replace(edit, start, matches[0]);
	if (changed < 0) {
		complete_free(matches);
		return LINE_NOMEM;
	}

	/* One match is followed by the append character. */
	if (matches[1] == NULL) {
		appended = complete_append(edit, rl_completion_append_character);
		complete_free(matches);
		if (!appended)
			return LINE_NOMEM;
		return LINE_CONTINUE;
	}

	/* Several matches: a second Tab that changes nothing lists them, otherwise the bell. */
	if (!changed && previous_tab) {
		complete_list(edit, matches);
	} else {
		completion_bell = 1;
	}

	/* Succeeded: the matches are no longer needed. */
	complete_free(matches);
	return LINE_CONTINUE;
}

/* Returns where the word before the cursor starts: after the last unquoted break character. */
static size_t
complete_word_start(
	const struct edit_line *edit)
{
	const char *breaks;
	const char *found;
	size_t position;
	char character;
	int quoted;

	/* No break characters make the whole line before the cursor the word. */
	breaks = rl_completer_word_break_characters;
	if (breaks == NULL)
		breaks = "";

	/* Back from the cursor until a break character that is not quoted. */
	position = *edit->point;
	while (position > 0) {
		character = (*edit->text)[position - 1U];
		found = NULL;
		if (character != '\0')
			found = strchr(breaks, character);

		/* A break character ends the word unless the caller calls it quoted. */
		if (found != NULL) {
			quoted = 0;
			if (rl_char_is_quoted_p != NULL)
				quoted = rl_char_is_quoted_p(*edit->text, (int)(position - 1U));
			if (!quoted)
				break;
		}

		/* The character belongs to the word. */
		position--;
	}

	/* Succeeded: the start of the word. */
	return position;
}

/*
 * Replaces the text from start to the cursor with a replacement; returns 1
 * when the line changed, 0 when it already held the replacement, and -1
 * without memory.
 */
static int
complete_replace(
	const struct edit_line *edit,
	size_t start,
	const char *replacement)
{
	size_t length;
	size_t size;
	int compare;
	int inserted;

	/* The same text is no change. */
	length = *edit->point - start;
	size = strlen(replacement);
	if (size == length) {
		compare = memcmp(*edit->text + start, replacement, size);
		if (compare == 0)
			return 0;
	}

	/* The word goes and the replacement goes in with the cursor after it. */
	line_delete(edit, start, *edit->point);
	*edit->point = start;
	inserted = line_insert(edit, start, replacement, size);
	if (!inserted)
		return -1;

	/* Succeeded: the line changed. */
	return 1;
}

/*
 * Puts the append character after a single match, or steps over it when the
 * line already has it there; returns 0 without memory.
 */
static int
complete_append(
	const struct edit_line *edit,
	int character)
{
	char text;
	int inserted;

	/* '\0' appends nothing. */
	if (character == '\0')
		return 1;

	/* The same character already under the cursor is stepped over. */
	text = (char)character;
	if (*edit->point < *edit->length && (*edit->text)[*edit->point] == text) {
		(*edit->point)++;
		return 1;
	}

	/* The character goes in at the cursor. */
	inserted = line_insert(edit, *edit->point, &text, 1);
	if (!inserted)
		return 0;

	/* Succeeded. */
	return 1;
}

/*
 * Lists the matches under the line, asking first when there are many, then
 * writes the prompt and the line again with the cursor where it was.
 */
static void
complete_list(
	const struct edit_line *edit,
	char **matches)
{
	size_t count;
	size_t row;
	size_t col;
	int wanted;

	/* The matches to list are those after the replacement. */
	count = 0;
	while (matches[count + 1U] != NULL)
		count++;

	/* The list starts on the line after the one being edited (after its last row). */
	line_position(*edit->text, *edit->length, terminal_columns(), &row, &col);
	cursor_to(row, col);
	(void)write_all("\r\n", 2);

	/* Many matches are listed only when the answer is yes. */
	wanted = 1;
	if (count > (size_t)rl_completion_query_items)
		wanted = complete_ask(count);
	if (wanted)
		complete_columns(matches + 1, count);

	/* The prompt and the line again, with the cursor back at its place. */
	(void)write_all(completion_prompt, strlen(completion_prompt));
	prompt_drawn();
	redraw(*edit->text, *edit->length, *edit->point, 0);
}

/* Asks whether to list many matches; returns 1 for y, Y or a blank. */
static int
complete_ask(
	size_t count)
{
	char question[64];
	unsigned char answer;
	ssize_t received;
	int length;

	/* The question, as GNU Readline puts it. */
	length = snprintf(question, sizeof(question), "Display all %lu possibilities? (y or n)", (unsigned long)count);
	if (length > 0)
		(void)write_all(question, (size_t)length);

	/* One key answers it, and the list or the line starts below the question. */
	do
		received = read(STDIN_FILENO, &answer, 1);
	while (received < 0 && errno == EINTR);
	(void)write_all("\n", 1);

	/* A failed read is a no. */
	if (received != 1)
		return 0;

	/* y, Y and a blank are yes. */
	if (answer == 'y' || answer == 'Y' || answer == ' ')
		return 1;

	/* Anything else is no. */
	return 0;
}

/*
 * Writes matches in columns as wide as the longest match and a gap, as many
 * as the terminal's width holds, filled top to bottom and then left to right.
 */
static void
complete_columns(
	char **matches,
	size_t count)
{
	struct winsize size;
	size_t width;
	size_t longest;
	size_t length;
	size_t columns;
	size_t rows;
	size_t row;
	size_t column;
	size_t index;
	size_t pad;
	int size_error;

	/* The terminal's width, or the usual 80 columns when it does not say. */
	width = COMPLETION_WIDTH;
	size_error = ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
	if (size_error == 0 && size.ws_col > 0)
		width = size.ws_col;

	/* The longest match sets the width of a column. */
	longest = 0;
	for (index = 0; index < count; index++) {
		length = strlen(matches[index]);
		if (length > longest)
			longest = length;
	}

	/* As many columns as fit, at least one, and the rows they need. */
	columns = width / (longest + COMPLETION_GAP);
	if (columns == 0)
		columns = 1;
	rows = (count + columns - 1U) / columns;

	/* Each row takes one match from each column. */
	for (row = 0; row < rows; row++) {
		for (column = 0; column < columns; column++) {
			/* A column past the last match ends the row. */
			index = column * rows + row;
			if (index >= count)
				break;
			length = strlen(matches[index]);
			(void)write_all(matches[index], length);

			/* Blanks up to the next column, when there is a match in it. */
			if (index + rows < count) {
				for (pad = length; pad < longest + COMPLETION_GAP; pad++)
					(void)write_all(" ", 1);
			}
		}

		/* The row ends. */
		(void)write_all("\n", 1);
	}
}

/* Frees an array of matches the completion function returned, and its strings. */
static void
complete_free(
	char **matches)
{
	size_t index;

	/* Each string, then the array. */
	for (index = 0; matches[index] != NULL; index++)
		free(matches[index]);
	free(matches);
}

/* Rings the terminal's bell. */
static void
write_bell(
	void)
{
	/* BEL. */
	(void)write_all("\a", 1);
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
	enum line_result result;
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

	/* Tab completes the word before the cursor, as in emacs mode. */
	if (byte == '\t') {
		result = complete_key(edit);
		return result;
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

