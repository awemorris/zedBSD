/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
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

static char *
duplicate(const char *text)
{
	size_t size = strlen(text) + 1U;
	char *copy = malloc(size);
	if (copy != NULL)
		memcpy(copy, text, size);
	return copy;
}

void
using_history(void)
{
	history_position = history_length;
}

void
clear_history(void)
{
	int index;
	for (index = 0; index < history_length; index++) {
		free(history_entries[index].line);
		history_entries[index].line = NULL;
		history_entries[index].timestamp = NULL;
	}
	history_length = 0;
	history_position = 0;
	history_base = 1;
}

void
add_history(const char *line)
{
	char *copy;
	if (line == NULL || line[0] == '\0')
		return;
	copy = duplicate(line);
	if (copy == NULL)
		return;
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

int
history_set_pos(int position)
{
	if (position < 0 || position > history_length)
		return 0;
	history_position = position;
	return 1;
}

HIST_ENTRY *
current_history(void)
{
	return history_position >= 0 && history_position < history_length ?
	    &history_entries[history_position] : NULL;
}

HIST_ENTRY *
previous_history(void)
{
	if (history_position <= 0)
		return NULL;
	history_position--;
	return &history_entries[history_position];
}

HIST_ENTRY *
next_history(void)
{
	if (history_position >= history_length)
		return NULL;
	history_position++;
	return current_history();
}

static int
write_all(const char *bytes, size_t size)
{
	while (size != 0) {
		ssize_t done = write(STDOUT_FILENO, bytes, size);
		if (done < 0) {
			if (errno == EINTR)
				continue;
			return 0;
		}
		if (done == 0)
			return 0;
		bytes += done;
		size -= (size_t)done;
	}
	return 1;
}

static void
redraw(const char *prompt, const char *line, size_t length, size_t point)
{
	size_t back = length - point + 1U;
	(void)write_all("\r", 1);
	(void)write_all(prompt, strlen(prompt));
	(void)write_all(line, length);
	(void)write_all(" ", 1);
	while (back-- != 0)
		(void)write_all("\b", 1);
}

static int
grow(char **line, size_t *capacity, size_t need)
{
	char *larger;
	size_t size = *capacity;
	if (need <= size)
		return 1;
	while (size < need) {
		if (size > (size_t)-1 / 2U)
			return 0;
		size *= 2U;
	}
	larger = realloc(*line, size);
	if (larger == NULL)
		return 0;
	*line = larger;
	*capacity = size;
	return 1;
}

static int
replace_line(char **line, size_t *capacity, size_t *length, size_t *point,
    const char *replacement)
{
	size_t size = strlen(replacement);
	if (!grow(line, capacity, size + 1U))
		return 0;
	memcpy(*line, replacement, size + 1U);
	*length = size;
	*point = size;
	return 1;
}

enum edit_key {
	EDIT_NONE, EDIT_UP, EDIT_DOWN, EDIT_LEFT, EDIT_RIGHT, EDIT_HOME,
	EDIT_END, EDIT_DELETE
};

static enum edit_key
escape_key(void)
{
	unsigned char byte;
	if (read(STDIN_FILENO, &byte, 1) != 1 || byte != '[')
		return EDIT_NONE;
	if (read(STDIN_FILENO, &byte, 1) != 1)
		return EDIT_NONE;
	switch (byte) {
	case 'A': return EDIT_UP;
	case 'B': return EDIT_DOWN;
	case 'C': return EDIT_RIGHT;
	case 'D': return EDIT_LEFT;
	case 'H': return EDIT_HOME;
	case 'F': return EDIT_END;
	case '3':
		if (read(STDIN_FILENO, &byte, 1) == 1 && byte == '~')
			return EDIT_DELETE;
		return EDIT_NONE;
	default: return EDIT_NONE;
	}
}

char *
readline(const char *prompt)
{
	struct termios saved, raw;
	char *line;
	size_t capacity = LINE_INITIAL, length = 0, point = 0;
	int terminal;
	if (prompt == NULL)
		prompt = "";
	line = malloc(capacity);
	if (line == NULL)
		return NULL;
	line[0] = '\0';
	rl_line_buffer = line;
	rl_point = rl_end = 0;
	history_position = history_length;
	terminal = tcgetattr(STDIN_FILENO, &saved) == 0;
	if (terminal) {
		raw = saved;
		raw.c_lflag &= ~(ICANON | ECHO | ISIG);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
			terminal = 0;
	}
	(void)write_all(prompt, strlen(prompt));
	for (;;) {
		unsigned char byte;
		ssize_t count = read(STDIN_FILENO, &byte, 1);
		enum edit_key key = EDIT_NONE;
		HIST_ENTRY *entry;
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (terminal)
				(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
			free(line);
			rl_line_buffer = NULL;
			return NULL;
		}
		if (byte == 0x1b)
			key = escape_key();
		else if (byte == 1) key = EDIT_HOME;
		else if (byte == 2) key = EDIT_LEFT;
		else if (byte == 5) key = EDIT_END;
		else if (byte == 6) key = EDIT_RIGHT;
		else if (byte == 14) key = EDIT_DOWN;
		else if (byte == 16) key = EDIT_UP;
		if (key == EDIT_HOME) point = 0;
		else if (key == EDIT_END) point = length;
		else if (key == EDIT_LEFT && point != 0) point--;
		else if (key == EDIT_RIGHT && point < length) point++;
		else if (key == EDIT_DELETE && point < length) {
			memmove(line + point, line + point + 1U, length - point);
			length--;
		} else if (key == EDIT_UP) {
			entry = previous_history();
			if (entry != NULL && !replace_line(&line, &capacity, &length,
			    &point, entry->line))
				break;
		} else if (key == EDIT_DOWN) {
			entry = next_history();
			if (entry != NULL) {
				if (!replace_line(&line, &capacity, &length, &point,
				    entry->line))
					break;
			} else if (history_position == history_length) {
				length = point = 0;
				line[0] = '\0';
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
			if (length == 0) {
				if (terminal)
					(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
				free(line);
				rl_line_buffer = NULL;
				return NULL;
			}
			if (point < length) {
				memmove(line + point, line + point + 1U, length - point);
				length--;
			}
		} else if (byte == 8 || byte == 0x7f) {
			if (point != 0) {
				memmove(line + point - 1U, line + point, length - point + 1U);
				point--;
				length--;
			}
		} else if (byte == 11) {
			length = point;
			line[length] = '\0';
		} else if (byte == 21) {
			memmove(line, line + point, length - point + 1U);
			length -= point;
			point = 0;
		} else if (byte >= 0x20 && byte != 0x7f) {
			if (!grow(&line, &capacity, length + 2U))
				break;
			memmove(line + point + 1U, line + point, length - point + 1U);
			line[point++] = (char)byte;
			length++;
		} else {
			continue;
		}
		rl_line_buffer = line;
		rl_point = (int)point;
		rl_end = (int)length;
		redraw(prompt, line, length, point);
	}
	if (terminal)
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	line[length] = '\0';
	rl_line_buffer = line;
	rl_point = (int)point;
	rl_end = (int)length;
	return line;
}
