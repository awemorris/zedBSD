/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A deliberately small Readline-compatible editor.  The package is named
 * libedit, while its public header and archive names follow Readline.
 */

#include "readline/readline.h"
#include "readline/history.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define HISTORY_MAX 32
#define LINE_INITIAL 128U

char *rl_line_buffer;
int rl_point;
int rl_end;
int history_base = 1;
int history_length;

static HIST_ENTRY history_entries[HISTORY_MAX];
static int history_position;

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

static char *duplicate(const char *text);
static int write_all(const char *bytes, size_t size);
static enum edit_key escape_key(void);
static int replace_line(char **line, size_t *capacity, size_t *length, size_t *point, const char *replacement);
static int grow(char **line, size_t *capacity, size_t need);
static void update_display(const char *line, size_t old_length, size_t old_point, size_t length, size_t point, size_t changed_from);
static void move_cursor(size_t from, size_t to);
static void cursor_left(size_t columns);
static void cursor_right(size_t columns);

/*
 * Implements the using history operation.
 */
void
using_history(
	void)
{
	history_position = history_length;
}

/*
 * Implements the clear history operation.
 */
void
clear_history(
	void)
{
	int index;

	/* Process each remaining element. */
	for (index = 0; index < history_length; index++) {
		free(history_entries[index].line);
		history_entries[index].line = NULL;
		history_entries[index].timestamp = NULL;
	}
	history_length = 0;
	history_position = 0;
	history_base = 1;
}

/*
 * Implements the add history operation.
 */
void
add_history(
	const char *line)
{
	char *copy;

	/* Handles the line availability. */
	if (line == NULL || line[0] == '\0')
		return;
	copy = duplicate(line);

	/* Handles the copy availability. */
	if (copy == NULL)
		return;

	/* Handles the history length condition. */
	if (history_length == HISTORY_MAX) {
		free(history_entries[0].line);
		memmove(&history_entries[0], &history_entries[1],
			(HISTORY_MAX - 1U) * sizeof(history_entries[0]));
		history_length--;
		history_base++;
	}
	history_entries[history_length].line = copy;
	history_entries[history_length].timestamp = NULL;
	history_length++;
	history_position = history_length;
}

/*
 * Implements the history set pos operation.
 */
int
history_set_pos(
	int position)
{
	/* Handles the position condition. */
	if (position < 0 || position > history_length)
		return 0;
	history_position = position;

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the current history operation.
 */
HIST_ENTRY *
current_history(
	void)
{
	/* Returns the computed result. */
	return history_position >= 0 && history_position < history_length
		   ? &history_entries[history_position]
		   : NULL;
}

/*
 * Implements the previous history operation.
 */
HIST_ENTRY *
previous_history(
	void)
{
	/* Handles the history position condition. */
	if (history_position <= 0)
		return NULL;
	history_position--;

	/* Returns the computed result. */
	return &history_entries[history_position];
}

/*
 * Implements the next history operation.
 */
HIST_ENTRY *
next_history(
	void)
{
	HIST_ENTRY *function_result;

	/* Handles the history position condition. */
	if (history_position >= history_length)
		return NULL;
	history_position++;

	/* Obtains the current history result. */
	function_result = current_history();

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the readline operation.
 */
char *
readline(
	const char *prompt)
{
	unsigned char byte;
	ssize_t count;
	enum edit_key key;
	struct termios saved, raw;
	char *line;
	size_t capacity, length, point;
	int terminal;

	capacity = LINE_INITIAL;
	length = 0;
	point = 0;

	/* Handles the prompt availability. */
	if (prompt == NULL)
		prompt = "";
	line = malloc(capacity);

	/* Handles the line availability. */
	if (line == NULL)
		return NULL;
	line[0] = '\0';
	rl_line_buffer = line;
	rl_point = rl_end = 0;
	history_position = history_length;
	terminal = tcgetattr(STDIN_FILENO, &saved) == 0;

	/* Checks the terminal state. */
	if (terminal) {
		raw = saved;
		raw.c_lflag &= ~(ICANON | ECHO | ISIG);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;

		/* Handles a failed tcsetattr operation. */
		if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
			terminal = 0;
	}
	(void)write_all(prompt, strlen(prompt));

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		HIST_ENTRY *entry;
		size_t old_length;
		size_t old_point;
		size_t changed_from;

		count = read(STDIN_FILENO, &byte, 1);
		key = EDIT_NONE;
		old_length = length;
		old_point = point;
		changed_from = (size_t)-1;

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0) {
			/* Checks the terminal state. */
			if (terminal)
				(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
			free(line);
			rl_line_buffer = NULL;

			/* Reports that no result is available. */
			return NULL;
		}

		/* Classifies the current byte. */
		if (byte == 0x1b)
			key = escape_key();
		else if (byte == 1)
			key = EDIT_HOME;
		else if (byte == 2)
			key = EDIT_LEFT;
		else if (byte == 5)
			key = EDIT_END;
		else if (byte == 6)
			key = EDIT_RIGHT;
		else if (byte == 14)
			key = EDIT_DOWN;
		else if (byte == 16)
			key = EDIT_UP;

		/* Handles the selected key. */
		if (key == EDIT_HOME)
			point = 0;
		else if (key == EDIT_END)
			point = length;
		else if (key == EDIT_LEFT && point != 0)
			point--;
		else if (key == EDIT_RIGHT && point < length)
			point++;
		else if (key == EDIT_DELETE && point < length) {
			memmove(line + point, line + point + 1U,
				length - point);
			length--;
			changed_from = point;
		} else if (key == EDIT_UP) {
			entry = previous_history();

			/* Handles a failed replace line operation. */
			if (entry != NULL &&
			    !replace_line(&line, &capacity, &length, &point,
					  entry->line))
				break;

			/* Handles the entry availability. */
			if (entry != NULL)
				changed_from = 0;
		} else if (key == EDIT_DOWN) {
			entry = next_history();

			/* Handles the entry availability. */
			if (entry != NULL) {
				/* Handles a failed replace line operation. */
				if (!replace_line(&line, &capacity, &length,
						  &point, entry->line))
					break;
				changed_from = 0;
			} else if (history_position == history_length) {
				length = point = 0;
				line[0] = '\0';
				changed_from = 0;
			}
		} else if (key != EDIT_NONE) {
			/* Cursor-only key. */
		} else if (byte == '\r' || byte == '\n') {
			(void)write_all("\n", 1);
			break;
		} else if (byte == 3) {
			length = point = 0;
			line[0] = '\0';
			(void)write_all("^C\n", 3);
			break;
		} else if (byte == 4) {
			/* Checks the current data length. */
			if (length == 0) {
				/* Checks the terminal state. */
				if (terminal) {
					(void)tcsetattr(STDIN_FILENO, TCSANOW,
							&saved);
				}
				free(line);
				rl_line_buffer = NULL;

				/* Reports that no result is available. */
				return NULL;
			}

			/* Handles the point condition. */
			if (point < length) {
				memmove(line + point, line + point + 1U,
					length - point);
				length--;
				changed_from = point;
			}
		} else if (byte == 8 || byte == 0x7f) {
			/* Handles the point condition. */
			if (point != 0) {
				memmove(line + point - 1U, line + point,
					length - point + 1U);
				point--;
				length--;
				changed_from = point;
			}
		} else if (byte == 11) {
			length = point;
			line[length] = '\0';
			changed_from = point;
		} else if (byte == 21) {
			memmove(line, line + point, length - point + 1U);
			length -= point;
			point = 0;
			changed_from = 0;
		} else if (byte >= 0x20 && byte != 0x7f) {
			/* Handles a failed grow operation. */
			if (!grow(&line, &capacity, length + 2U))
				break;
			memmove(line + point + 1U, line + point,
				length - point + 1U);
			changed_from = point;
			line[point++] = (char)byte;
			length++;
		} else {
			continue;
		}
		rl_line_buffer = line;
		rl_point = (int)point;
		rl_end = (int)length;
		update_display(line, old_length, old_point, length, point,
			       changed_from);
	}

	/* Checks the terminal state. */
	if (terminal)
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	line[length] = '\0';
	rl_line_buffer = line;
	rl_point = (int)point;
	rl_end = (int)length;

	/* Returns the computed result. */
	return line;
}

/* Supports the duplicate operation. */
static char *
duplicate(
	const char *text)
{
	size_t size;
	char *copy;

	size = strlen(text) + 1U;
	copy = malloc(size);

	/* Handles the copy availability. */
	if (copy != NULL)
		memcpy(copy, text, size);

	/* Returns the computed result. */
	return copy;
}

/* Supports the write all operation. */
static int
write_all(
	const char *bytes,
	size_t size)
{
	ssize_t done;

	/* Process each remaining element. */
	while (size != 0) {
		done = write(STDOUT_FILENO, bytes, size);

		/* Handles the done condition. */
		if (done < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* Reports successful completion. */
			return 0;
		}

		/* Handles the done condition. */
		if (done == 0)
			return 0;
		bytes += done;
		size -= (size_t)done;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the escape key operation. */
static enum edit_key
escape_key(
	void)
{
	unsigned char byte;

	/* Handles a failed read operation. */
	if (read(STDIN_FILENO, &byte, 1) != 1 || byte != '[')
		return EDIT_NONE;

	/* Handles a failed read operation. */
	if (read(STDIN_FILENO, &byte, 1) != 1)
		return EDIT_NONE;

	/* Dispatch the selected operation case. */
	switch (byte) {
	case 'A':
		/* Returns the computed result. */
		return EDIT_UP;
	case 'B':
		/* Returns the computed result. */
		return EDIT_DOWN;
	case 'C':
		/* Returns the computed result. */
		return EDIT_RIGHT;
	case 'D':
		/* Returns the computed result. */
		return EDIT_LEFT;
	case 'H':
		/* Returns the computed result. */
		return EDIT_HOME;
	case 'F':
		/* Returns the computed result. */
		return EDIT_END;
	case '3':
		/* Handles a failed read operation. */
		if (read(STDIN_FILENO, &byte, 1) == 1 && byte == '~')
			return EDIT_DELETE;

		/* Returns the computed result. */
		return EDIT_NONE;
	default:
		/* Returns the computed result. */
		return EDIT_NONE;
	}
}

/* Supports the replace line operation. */
static int
replace_line(
	char **line,
	size_t *capacity,
	size_t *length,
	size_t *point,
	const char *replacement)
{
	size_t size;

	size = strlen(replacement);

	/* Handles a failed grow operation. */
	if (!grow(line, capacity, size + 1U))
		return 0;
	memcpy(*line, replacement, size + 1U);
	*length = size;
	*point = size;
	/* Reports operation failure. */
	return 1;
}

/* Supports the grow operation. */
static int
grow(
	char **line,
	size_t *capacity,
	size_t need)
{
	char *larger;
	size_t size;

	size = *capacity;

	/* Handles the need condition. */
	if (need <= size)
		return 1;

	/* Process each remaining element. */
	while (size < need) {
		/* Checks the current data size. */
		if (size > (size_t)-1 / 2U)
			return 0;
		size *= 2U;
	}
	larger = realloc(*line, size);

	/* Handles the larger availability. */
	if (larger == NULL)
		return 0;
	*line = larger;
	*capacity = size;
	/* Reports operation failure. */
	return 1;
}

/* Supports the update display operation. */
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

	/* Handles the changed from condition. */
	if (changed_from == (size_t)-1) {
		move_cursor(old_point, point);

		/* Returns the computed result. */
		return;
	}
	move_cursor(old_point, changed_from);
	(void)write_all(line + changed_from, length - changed_from);

	/* Handles the old length condition. */
	if (old_length > length) {
		/* Continue while the operation condition remains true. */
		spaces = old_length - length;
		while (spaces-- != 0U)
			(void)write_all(" ", 1);
	}
	drawn_to = old_length > length ? old_length : length;
	move_cursor(drawn_to, point);
}

/* Supports the move cursor operation. */
static void
move_cursor(
	size_t from,
	size_t to)
{
	/* Handles the to condition. */
	if (to < from)
		cursor_left(from - to);
	else
		cursor_right(to - from);
}

/* Supports the cursor left operation. */
static void
cursor_left(
	size_t columns)
{
	char sequence[3U * sizeof(size_t) + 4U];
	size_t at = sizeof(sequence);

	/* Handles the columns condition. */
	if (columns == 0U)
		return;
	sequence[--at] = 'D';
	do {
		sequence[--at] = (char)('0' + columns % 10U);
		columns /= 10U;
	} while (columns != 0U);
	sequence[--at] = '[';
	sequence[--at] = '\033';
	(void)write_all(sequence + at, sizeof(sequence) - at);
}

/* Supports the cursor right operation. */
static void
cursor_right(
	size_t columns)
{
	char sequence[3U * sizeof(size_t) + 4U];
	size_t at = sizeof(sequence);

	/* Handles the columns condition. */
	if (columns == 0U)
		return;
	sequence[--at] = 'C';
	do {
		sequence[--at] = (char)('0' + columns % 10U);
		columns /= 10U;
	} while (columns != 0U);
	sequence[--at] = '[';
	sequence[--at] = '\033';
	(void)write_all(sequence + at, sizeof(sequence) - at);
}
