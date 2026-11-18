/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The commands typed in command mode: movement, insertion, the d, c and y
 * operators, put, undo, repeat and the page keys.
 */

#include "userland/base/vi/vi.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The classes of character a word is made of.  A word is a run of one
 * class; blanks separate words, and an empty line counts as a word.
 */
enum char_class {
	CLASS_BLANK,
	CLASS_EMPTY,
	CLASS_WORD,
	CLASS_PUNCTUATION,
	CLASS_HIRAGANA,
	CLASS_KATAKANA,
	CLASS_IDEOGRAPH
};

/*
 * Where a movement ends and how an operator treats the text it crosses.
 *
 * A line-wise movement makes an operator act on whole lines.  An
 * inclusive one takes in the character at the end; an exclusive one
 * stops before it.  keep_column says that the movement went up or down
 * and the cursor should keep aiming at the column it had.
 */
struct motion {
	struct vi_position target;
	int linewise;
	int inclusive;
	int keep_column;
};

static int read_count(struct vi_editor *editor, int key, unsigned long *count, int *has_count);
static int read_character(struct vi_editor *editor, int key, char *text, size_t *length);
static struct vi_line *line_at(struct vi_editor *editor, size_t index);
static size_t last_offset(const struct vi_line *line);
static size_t first_nonblank(const struct vi_line *line);
static enum char_class class_at(struct vi_editor *editor, struct vi_position position, int big);
static int step_forward(struct vi_editor *editor, struct vi_position *position);
static int step_backward(struct vi_editor *editor, struct vi_position *position);
static int position_before(struct vi_position first, struct vi_position second);
static int motion_word_forward(struct vi_editor *editor, unsigned long count, int big, int for_operator, struct motion *motion);
static void motion_buffer_end(struct vi_editor *editor, int for_operator, struct motion *motion);
static int motion_word_end(struct vi_editor *editor, unsigned long count, int big, struct motion *motion);
static int motion_word_backward(struct vi_editor *editor, unsigned long count, int big, struct motion *motion);
static int motion_find(struct vi_editor *editor, int key, unsigned long count, int repeat, struct motion *motion);
static int motion_search(struct vi_editor *editor, int key, unsigned long count, struct motion *motion);
static int motion_vertical(struct vi_editor *editor, size_t line, struct motion *motion);
static int motion(struct vi_editor *editor, int key, unsigned long count, int has_count, int for_operator, struct motion *motion);
static int text_between(struct vi_editor *editor, struct vi_position start, struct vi_position end, int linewise, struct vi_register *copy);
static void register_replace(struct vi_editor *editor, struct vi_register *copy);
static int delete_lines(struct vi_editor *editor, size_t first, size_t last);
static int delete_between(struct vi_editor *editor, struct vi_position start, struct vi_position end);
static void operator(struct vi_editor *editor, int operator_key, unsigned long count);
static void insert_mode(struct vi_editor *editor, unsigned long count);
static void insert_repeat(struct vi_editor *editor, unsigned long count);
static void insert_key(struct vi_editor *editor, int key);
static int insert_text(struct vi_editor *editor, const char *text, size_t length);
static void command_put(struct vi_editor *editor, int before, unsigned long count);
static void command_delete_characters(struct vi_editor *editor, unsigned long count, int backward);
static void command_replace(struct vi_editor *editor, unsigned long count);
static void command_join(struct vi_editor *editor, unsigned long count);
static void command_toggle_case(struct vi_editor *editor, unsigned long count);
static void command_undo(struct vi_editor *editor, unsigned long count, int forward);
static void command_repeat(struct vi_editor *editor, unsigned long count, int has_count);
static void command_page(struct vi_editor *editor, int key, unsigned long count);
static void command_open(struct vi_editor *editor, int above);
static void command_line(struct vi_editor *editor);
static void record_key(struct vi_editor *editor, int key);
static int key_is_text(int key);
static void report_error(struct vi_editor *editor, int error);
static int key_ends_input(int key);
static int word_ends_at(struct vi_editor *editor, struct vi_position position, int big);

/*
 * Runs one command, given its first key.
 *
 * The command reads any further keys it needs.  The main loop opens the
 * undo group before and closes it after.
 */
void
vi_command(
	struct vi_editor *editor,
	int key)
{
	struct motion moved;
	struct vi_line *line;
	unsigned long count;
	int has_count;
	int status;
	int next;

	/* Reads a count typed before the command. */
	key = read_count(editor, key, &count, &has_count);
	if (key == VI_KEY_STOP)
		return;

	/* Remembers which recorded keys were the count, so . can replace it. */
	editor->record.count_end = 0;
	if (has_count) {
		if (editor->record.length != 0)
			editor->record.count_end = editor->record.length - 1;
	}

	/* Chooses what the key does. */
	switch (key) {
	case 'i':
		insert_mode(editor, count);
		break;
	case 'a':
		/* Appends after the character under the cursor. */
		line = line_at(editor, editor->cursor.line);
		editor->cursor.offset = vi_text_next(line->text, line->length, editor->cursor.offset);
		insert_mode(editor, count);
		break;
	case 'I':
		editor->cursor.offset = first_nonblank(line_at(editor, editor->cursor.line));
		insert_mode(editor, count);
		break;
	case 'A':
		editor->cursor.offset = line_at(editor, editor->cursor.line)->length;
		insert_mode(editor, count);
		break;
	case 'o':
		command_open(editor, 0);
		break;
	case 'O':
		command_open(editor, 1);
		break;
	case 'd':
	case 'c':
	case 'y':
		operator(editor, key, count);
		break;
	case 'D':
		editor->pending_key = '$';
		operator(editor, 'd', count);
		break;
	case 'C':
		editor->pending_key = '$';
		operator(editor, 'c', count);
		break;
	case 's':
		editor->pending_key = 'l';
		operator(editor, 'c', count);
		break;
	case 'S':
		editor->pending_key = 'c';
		operator(editor, 'c', count);
		break;
	case 'Y':
		editor->pending_key = 'y';
		operator(editor, 'y', count);
		break;
	case 'x':
	case VI_KEY_DELETE:
		command_delete_characters(editor, count, 0);
		break;
	case 'X':
		command_delete_characters(editor, count, 1);
		break;
	case 'r':
		command_replace(editor, count);
		break;
	case 'J':
		command_join(editor, count);
		break;
	case '~':
		command_toggle_case(editor, count);
		break;
	case 'p':
		command_put(editor, 0, count);
		break;
	case 'P':
		command_put(editor, 1, count);
		break;
	case 'u':
		command_undo(editor, count, 0);
		break;
	case VI_KEY_CONTROL_R:
		command_undo(editor, count, 1);
		break;
	case '.':
		command_repeat(editor, count, has_count);
		break;
	case ':':
		command_line(editor);
		break;
	case 'Z':
		/* ZZ writes and quits; ZQ quits without writing. */
		next = vi_editor_key(editor);
		if (next == 'Z')
			vi_ex_command(editor, "x");
		else if (next == 'Q')
			vi_ex_command(editor, "q!");
		else
			vi_bell(editor);
		break;
	case VI_KEY_CONTROL_F:
	case VI_KEY_CONTROL_B:
	case VI_KEY_CONTROL_D:
	case VI_KEY_CONTROL_U:
	case VI_KEY_PAGE_UP:
	case VI_KEY_PAGE_DOWN:
		command_page(editor, key, count);
		break;
	case VI_KEY_CONTROL_L:
		/* Clears the terminal and draws everything again. */
		vi_terminal_command(&editor->terminal, editor->terminal.clear_string);
		vi_screen_invalidate(editor);
		break;
	case VI_KEY_CONTROL_G:
		vi_ex_file_status(editor);
		break;
	case VI_KEY_CONTROL_Z:
		vi_suspend(editor);
		break;
	case VI_KEY_ESCAPE:
	case VI_KEY_CONTROL_C:
		/* Nothing is pending in command mode, so the key only rings. */
		vi_bell(editor);
		break;
	case VI_KEY_STOP:
		break;
	default:
		/* Any other key is a movement or an unknown key. */
		status = motion(editor, key, count, has_count, 0, &moved);
		if (status != 0) {
			vi_bell(editor);
			break;
		}

		/* Moves the cursor. */
		editor->cursor = moved.target;
		vi_cursor_clamp(editor);

		/* j and k keep aiming at the remembered column; $ aims at every line's end. */
		if (moved.keep_column)
			return;
		line = line_at(editor, editor->cursor.line);
		editor->want_column = vi_text_column(line->text, line->length, editor->cursor.offset);
		if (key == '$' || key == VI_KEY_END)
			editor->want_column = SIZE_MAX;
		return;
	}

	/* Every command but a movement leaves the cursor on a character, aiming at its column. */
	vi_cursor_clamp(editor);
	line = line_at(editor, editor->cursor.line);
	editor->want_column = vi_text_column(line->text, line->length, editor->cursor.offset);
}

/*
 * Keeps the keys of a command that changed the buffer for the . command.
 *
 * Called by the main loop after each command.
 */
void
vi_command_finish(
	struct vi_editor *editor)
{
	/* Recording ends with the command. */
	editor->recording = 0;

	/* A command that changed nothing, or had too many keys, leaves the last change as it was. */
	if (!editor->changed)
		return;
	if (editor->record.overflow)
		return;

	/* The command's keys become the change . repeats. */
	editor->repeat = editor->record;
}

/*
 * Reads the next key for a command.
 *
 * Keys fed back by the . command come first.  Otherwise the screen is
 * brought up to date, unless more keys are already waiting, and a key is
 * read from the terminal, with signals handled while waiting.  Returns
 * VI_KEY_STOP when the editor must stop.
 */
int
vi_editor_key(
	struct vi_editor *editor)
{
	int key;
	int pending;
	int stop;

	/* A stopping editor reads no more keys. */
	if (editor->quit)
		return VI_KEY_STOP;

	/* A key put back by the command itself comes first. */
	if (editor->pending_key != 0) {
		key = editor->pending_key;
		editor->pending_key = 0;
		return key;
	}

	/* Keys fed back by the . command come next, and are recorded like typed ones. */
	if (editor->replay.position < editor->replay.length) {
		key = editor->replay.keys[editor->replay.position];
		editor->replay.position++;
		record_key(editor, key);
		return key;
	}

	/* Waits for a key from the terminal. */
	for (;;) {
		/* Handles the signals that arrived since the last wait, stopping if one asks to. */
		stop = vi_signal_pending(editor);
		if (stop)
			return VI_KEY_STOP;

		/* Draws the screen unless pasted or typed-ahead keys are waiting. */
		pending = vi_terminal_input_pending(&editor->terminal);
		if (!pending)
			vi_screen_update(editor);

		/* Reads a key from the terminal. */
		key = vi_terminal_read_key(&editor->terminal);
		if (key == VI_KEY_STOP) {
			/* The terminal went away; the editor stops. */
			editor->quit = 1;
			editor->exit_status = 1;
			return VI_KEY_STOP;
		}

		/* A key ends the wait. */
		if (key >= 0)
			break;

		/* A signal ended the wait; the next turn handles it. */
	}

	/* Records the key for the . command. */
	record_key(editor, key);

	/* Succeeded: the key. */
	return key;
}

/*
 * Sets the message shown on the bottom line until the next key.
 */
void
vi_message(
	struct vi_editor *editor,
	const char *text)
{
	/* Copies the message, cut to the room there is. */
	(void)snprintf(editor->message, sizeof(editor->message), "%s", text);
}

/*
 * Rings the terminal's bell for a key that could not be done.
 */
void
vi_bell(
	struct vi_editor *editor)
{
	/* The bell goes out with the next screen update. */
	vi_terminal_command(&editor->terminal, editor->terminal.bell_string);
}

/*
 * Keeps the cursor on the buffer: on an existing line and, in command
 * mode, on the start of a character of the line.
 */
void
vi_cursor_clamp(
	struct vi_editor *editor)
{
	struct vi_line *line;
	size_t last;
	size_t next;
	size_t start;

	/* The cursor's line must exist. */
	if (editor->cursor.line >= editor->buffer.count)
		editor->cursor.line = editor->buffer.count - 1;

	/* Finds the cursor's line. */
	line = line_at(editor, editor->cursor.line);

	/* Insert mode may rest at the end of the line; command mode rests on the last character. */
	if (editor->mode == VI_MODE_INSERT) {
		if (editor->cursor.offset > line->length)
			editor->cursor.offset = line->length;
	} else {
		last = last_offset(line);
		if (editor->cursor.offset > last)
			editor->cursor.offset = last;
	}

	/* The cursor must sit on the first byte of a character: the start of the one that ends after it. */
	if (editor->cursor.offset < line->length) {
		next = vi_text_next(line->text, line->length, editor->cursor.offset);
		start = vi_text_previous(line->text, line->length, next);
		editor->cursor.offset = start;
	}
}

/*
 * Moves the cursor to the first character of its line that is not blank.
 */
void
vi_cursor_first_nonblank(
	struct vi_editor *editor)
{
	/* Finds the first character after the leading blanks. */
	editor->cursor.offset = first_nonblank(line_at(editor, editor->cursor.line));
}

/*
 * Reads a line typed on the bottom row after a : or a /.
 *
 * Returns 1 when RETURN ends the line, or 0 when ESC, CONTROL-C or erasing
 * past the start abandons it.  The line is left in the editor's input.
 */
int
vi_read_line(
	struct vi_editor *editor,
	char prompt)
{
	enum vi_mode previous;
	char text[8];
	size_t length;
	size_t start;
	int key;
	int result;
	int abandon;
	int typed;

	/* Shows the prompt with an empty line after it. */
	previous = editor->mode;
	editor->mode = VI_MODE_LINE;
	editor->input_prompt = prompt;
	editor->input_length = 0;
	editor->input[0] = '\0';

	/* Takes keys until the line is ended or abandoned. */
	result = 0;
	for (;;) {
		key = vi_editor_key(editor);

		/* RETURN ends the line; ESC and CONTROL-C abandon it, as does a stopping editor. */
		if (key == VI_KEY_RETURN || key == VI_KEY_NEWLINE) {
			result = 1;
			break;
		}

		/* ESC, CONTROL-C and a stopping editor abandon the line. */
		abandon = key_ends_input(key);
		if (abandon)
			break;

		/* Erasing removes one character; erasing an empty line abandons it. */
		if (key == VI_KEY_BACKSPACE || key == VI_KEY_ERASE) {
			if (editor->input_length == 0)
				break;
			start = vi_text_previous(editor->input, editor->input_length, editor->input_length);
			editor->input_length = start;
			editor->input[start] = '\0';
			continue;
		}

		/* CONTROL-U empties the line. */
		if (key == VI_KEY_CONTROL_U) {
			editor->input_length = 0;
			editor->input[0] = '\0';
			continue;
		}

		/* Keys without a character of their own are refused. */
		typed = key_is_text(key);
		if (!typed) {
			vi_bell(editor);
			continue;
		}

		/* Adds the character, whole, if it fits. */
		result = read_character(editor, key, text, &length);
		if (result != 0) {
			result = 0;
			continue;
		}

		/* Refuses a character that does not fit. */
		if (editor->input_length + length >= sizeof(editor->input)) {
			vi_bell(editor);
			continue;
		}

		/* Adds the character to the line. */
		memcpy(editor->input + editor->input_length, text, length);
		editor->input_length += length;
		editor->input[editor->input_length] = '\0';
	}

	/* Goes back to the mode the line was typed from. */
	editor->mode = previous;

	/* Succeeded: whether the line was ended rather than abandoned. */
	return result;
}

/* Reads the digits of a count and returns the first key after them. */
static int
read_count(
	struct vi_editor *editor,
	int key,
	unsigned long *count,
	int *has_count)
{
	unsigned long value;

	/* A command without a count acts once. */
	*count = 1;
	*has_count = 0;
	value = 0;

	/* Takes digits; a 0 that starts the number is the 0 command instead. */
	for (;;) {
		if (key < '0' || key > '9')
			break;
		if (key == '0' && !*has_count)
			break;

		/* Adds the digit, holding the count at its limit. */
		value = value * 10 + (unsigned long)(key - '0');
		if (value > VI_COUNT_LIMIT)
			value = VI_COUNT_LIMIT;
		*has_count = 1;
		key = vi_editor_key(editor);
	}

	/* A count of zero is not possible here, since the first digit is never 0. */
	if (*has_count)
		*count = value;

	/* Succeeded: the key after the count. */
	return key;
}

/*
 * Reads a whole UTF-8 character that starts with the given key, taking
 * its following bytes from the next keys.  A key that is not a following
 * byte ends the character early and is put back.  Returns 0 with the
 * character, or -1 when the key has no character (ESC, a special key).
 */
static int
read_character(
	struct vi_editor *editor,
	int key,
	char *text,
	size_t *length)
{
	size_t needed;
	int next;

	/* Keys without a byte of their own, and ESC, have no character. */
	if (key > 0xff)
		return -1;
	if (key == VI_KEY_ESCAPE)
		return -1;

	/* The first byte tells how many bytes the character has. */
	needed = 1;
	if (key >= 0xf0 && key < 0xf5)
		needed = 4;
	else if (key >= 0xe0 && key < 0xf0)
		needed = 3;
	else if (key >= 0xc2 && key < 0xe0)
		needed = 2;

	/* Takes the following bytes. */
	text[0] = (char)key;
	*length = 1;
	while (*length < needed) {
		next = vi_editor_key(editor);
		if (next < 0x80 || next > 0xbf) {
			/* Not a following byte: the character ends here and the key is read again. */
			if (next != VI_KEY_STOP)
				editor->pending_key = next;
			break;
		}

		/* Takes the following byte into the character. */
		text[*length] = (char)next;
		(*length)++;
	}

	/* Succeeded: the bytes of the character. */
	return 0;
}

/* Gives the line at an index of the buffer. */
static struct vi_line *
line_at(
	struct vi_editor *editor,
	size_t index)
{
	/* The table holds the lines in order. */
	return &editor->buffer.lines[index];
}

/* Gives the offset of the last character of a line, or 0 for an empty line. */
static size_t
last_offset(
	const struct vi_line *line)
{
	size_t offset;

	/* An empty line has its cursor at 0. */
	if (line->length == 0)
		return 0;

	/* Steps back from the end over one character. */
	offset = vi_text_previous(line->text, line->length, line->length);

	/* Succeeded: the start of the last character. */
	return offset;
}

/* Gives the offset of the first character of a line that is not a space or a tab. */
static size_t
first_nonblank(
	const struct vi_line *line)
{
	size_t offset;

	/* Skips the leading spaces and tabs. */
	offset = 0;
	while (offset < line->length) {
		if (line->text[offset] != ' ' && line->text[offset] != '\t')
			break;
		offset++;
	}

	/* A line of blanks keeps the cursor on its last character. */
	if (offset == line->length)
		offset = last_offset(line);

	/* Succeeded: the offset. */
	return offset;
}

/*
 * Gives the class of the character at a position for the word movements.
 *
 * The end of a line reads as a blank (the newline), and an empty line has
 * its own class.  With big set, every character that is not blank is of
 * one class.
 */
static enum char_class
class_at(
	struct vi_editor *editor,
	struct vi_position position,
	int big)
{
	struct vi_line *line;
	unsigned long code;

	/* Finds the line of the position. */
	line = line_at(editor, position.line);

	/* An empty line, and the newline at the end of a line. */
	if (line->length == 0)
		return CLASS_EMPTY;
	if (position.offset >= line->length)
		return CLASS_BLANK;

	/* Spaces, tabs and the ideographic space are blanks. */
	code = vi_text_code(line->text, line->length, position.offset);
	if (code == ' ')
		return CLASS_BLANK;
	if (code == '\t')
		return CLASS_BLANK;
	if (code == 0x3000)
		return CLASS_BLANK;

	/* A big word is any run of characters that are not blank. */
	if (big)
		return CLASS_WORD;

	/* ASCII splits into letters, digits and underscore against punctuation. */
	if (code < 0x80) {
		if (code == '_')
			return CLASS_WORD;
		if (code >= '0' && code <= '9')
			return CLASS_WORD;
		if (code >= 'a' && code <= 'z')
			return CLASS_WORD;
		if (code >= 'A' && code <= 'Z')
			return CLASS_WORD;
		return CLASS_PUNCTUATION;
	}

	/* Japanese splits by script, so a w steps from kana to kanji. */
	if (code >= 0x3001 && code <= 0x303f)
		return CLASS_PUNCTUATION;
	if (code >= 0x3040 && code <= 0x309f)
		return CLASS_HIRAGANA;
	if (code >= 0x30a0 && code <= 0x30ff)
		return CLASS_KATAKANA;
	if (code >= 0x3400 && code <= 0x4dbf)
		return CLASS_IDEOGRAPH;
	if (code >= 0x4e00 && code <= 0x9fff)
		return CLASS_IDEOGRAPH;
	if (code >= 0xf900 && code <= 0xfaff)
		return CLASS_IDEOGRAPH;
	if (code >= 0x20000 && code <= 0x3ffff)
		return CLASS_IDEOGRAPH;
	if (code >= 0xff01 && code <= 0xff0f)
		return CLASS_PUNCTUATION;
	if (code >= 0xff1a && code <= 0xff20)
		return CLASS_PUNCTUATION;

	/* Any other character, invalid bytes included, is part of a word. */
	return CLASS_WORD;
}

/*
 * Moves a position one character forward, through the newline at the end
 * of each line.  Returns 0, or -1 at the end of the buffer.
 */
static int
step_forward(
	struct vi_editor *editor,
	struct vi_position *position)
{
	struct vi_line *line;

	/* Finds the line of the position. */
	line = line_at(editor, position->line);

	/* Within the line, steps over one character, onto the newline at the end. */
	if (position->offset < line->length) {
		position->offset = vi_text_next(line->text, line->length, position->offset);
		return 0;
	}

	/* From the newline, goes to the start of the next line. */
	if (position->line + 1 >= editor->buffer.count)
		return -1;

	/* Moves to the start of the next line. */
	position->line++;
	position->offset = 0;

	/* Succeeded. */
	return 0;
}

/*
 * Moves a position one character back, onto the newline of the previous
 * line from the start of a line.  Returns 0, or -1 at the start of the
 * buffer.
 */
static int
step_backward(
	struct vi_editor *editor,
	struct vi_position *position)
{
	struct vi_line *line;

	/* Within the line, steps back over one character. */
	if (position->offset > 0) {
		line = line_at(editor, position->line);
		position->offset = vi_text_previous(line->text, line->length, position->offset);
		return 0;
	}

	/* From the start of a line, goes to the newline of the line before. */
	if (position->line == 0)
		return -1;

	/* Moves to the end of the line before. */
	position->line--;
	position->offset = line_at(editor, position->line)->length;

	/* Succeeded. */
	return 0;
}

/* Tells whether one position comes before another in the buffer. */
static int
position_before(
	struct vi_position first,
	struct vi_position second)
{
	/* The line decides, and on the same line the offset. */
	if (first.line < second.line)
		return 1;
	if (first.line > second.line)
		return 0;
	if (first.offset < second.offset)
		return 1;

	/* Not before: the same place or after. */
	return 0;
}

/*
 * Moves to the start of the next word, count times.
 *
 * An operator does not reach into the next line: dw on the last word of a
 * line stops at the end of the line, as in every vi.
 */
static int
motion_word_forward(
	struct vi_editor *editor,
	unsigned long count,
	int big,
	int for_operator,
	struct motion *motion)
{
	struct vi_position position;
	enum char_class start_class;
	enum char_class class;
	struct vi_line *line;
	int status;

	/* Starts from the cursor. */
	position = editor->cursor;

	/* Steps over one word and the blanks after it, count times. */
	while (count > 0) {
		count--;

		/* Leaves the word under the cursor, or the empty line. */
		start_class = class_at(editor, position, big);
		if (start_class == CLASS_EMPTY) {
			status = step_forward(editor, &position);
			if (status != 0) {
				motion_buffer_end(editor, for_operator, motion);
				return 0;
			}
		} else if (start_class != CLASS_BLANK) {
			for (;;) {
				line = line_at(editor, position.line);
				if (position.offset >= line->length)
					break;
				class = class_at(editor, position, big);
				if (class != start_class)
					break;
				status = step_forward(editor, &position);
				if (status != 0) {
					motion_buffer_end(editor, for_operator, motion);
					return 0;
				}
			}
		}

		/* An operator's last word stops at the end of its line. */
		if (for_operator && count == 0) {
			line = line_at(editor, position.line);
			if (position.line == editor->cursor.line && position.offset >= line->length) {
				motion->target = position;
				return 0;
			}
		}

		/* Skips blanks and newlines, stopping at an empty line. */
		for (;;) {
			class = class_at(editor, position, big);
			if (class != CLASS_BLANK)
				break;
			status = step_forward(editor, &position);
			if (status != 0) {
				motion_buffer_end(editor, for_operator, motion);
				return 0;
			}
		}
	}

	/* An operator that crossed into a later line acts up to the end of the first. */
	if (for_operator && position.line != editor->cursor.line) {
		position.line = editor->cursor.line;
		position.offset = line_at(editor, position.line)->length;
	}

	/* Tells where the movement ends. */
	motion->target = position;

	/* Succeeded. */
	return 0;
}

/*
 * Ends a forward word movement that ran off the end of the buffer: on the
 * last character, or past it for an operator, so dw takes the last word.
 */
static void
motion_buffer_end(
	struct vi_editor *editor,
	int for_operator,
	struct motion *motion)
{
	struct vi_line *line;

	/* The last line of the buffer. */
	motion->target.line = editor->buffer.count - 1;
	line = line_at(editor, motion->target.line);

	/* Its last character, or its end for an operator. */
	motion->target.offset = last_offset(line);
	if (for_operator)
		motion->target.offset = line->length;
}

/* Moves to the end of the word, or of the next word, count times. */
static int
motion_word_end(
	struct vi_editor *editor,
	unsigned long count,
	int big,
	struct motion *motion)
{
	struct vi_position position;
	struct vi_position next;
	enum char_class class;
	enum char_class next_class;
	int status;

	/* Starts from the cursor. */
	position = editor->cursor;

	/* Moves one character, skips blanks, then runs to the end of the word, count times. */
	while (count > 0) {
		count--;

		/* Always moves at least one character. */
		status = step_forward(editor, &position);
		if (status != 0)
			return -1;

		/* Skips blanks, newlines and empty lines. */
		for (;;) {
			class = class_at(editor, position, big);
			if (class != CLASS_BLANK && class != CLASS_EMPTY)
				break;
			status = step_forward(editor, &position);
			if (status != 0)
				return -1;
		}

		/* Runs to the last character of the same class on the line. */
		for (;;) {
			next = position;
			status = step_forward(editor, &next);
			if (status != 0)
				break;
			if (next.line != position.line)
				break;
			next_class = class_at(editor, next, big);
			if (next_class != class)
				break;
			position = next;
		}
	}

	/* Ends on the last character of the word, which the movement takes in. */
	motion->target = position;
	motion->inclusive = 1;

	/* Succeeded. */
	return 0;
}

/* Moves to the start of the word, or of the previous word, count times. */
static int
motion_word_backward(
	struct vi_editor *editor,
	unsigned long count,
	int big,
	struct motion *motion)
{
	struct vi_position position;
	struct vi_position previous;
	enum char_class class;
	enum char_class previous_class;
	int status;

	/* Starts from the cursor. */
	position = editor->cursor;

	/* Moves one character, skips blanks back, then runs to the start of the word, count times. */
	while (count > 0) {
		count--;

		/* Always moves at least one character. */
		status = step_backward(editor, &position);
		if (status != 0)
			return -1;

		/* Skips blanks and newlines back, stopping at an empty line. */
		for (;;) {
			class = class_at(editor, position, big);
			if (class != CLASS_BLANK)
				break;
			status = step_backward(editor, &position);
			if (status != 0) {
				position.line = 0;
				position.offset = 0;
				motion->target = position;
				return 0;
			}
		}

		/* An empty line is a word of its own. */
		if (class == CLASS_EMPTY)
			continue;

		/* Runs back to the first character of the same class on the line. */
		while (position.offset > 0) {
			previous = position;
			(void)step_backward(editor, &previous);
			previous_class = class_at(editor, previous, big);
			if (previous_class != class)
				break;
			position = previous;
		}
	}

	/* Tells where the movement ends. */
	motion->target = position;

	/* Succeeded. */
	return 0;
}

/*
 * Moves to a character on the line with f, F, t or T, or repeats the last
 * such movement with ; or ,.
 */
static int
motion_find(
	struct vi_editor *editor,
	int key,
	unsigned long count,
	int repeat,
	struct motion *motion)
{
	struct vi_line *line;
	size_t offset;
	size_t found;
	int forward;
	int before;
	int status;
	int compare;
	int typed;

	/* Finds the cursor's line. */
	line = line_at(editor, editor->cursor.line);

	/* A new search reads the character to look for. */
	if (!repeat) {
		typed = vi_editor_key(editor);
		status = read_character(editor, typed, editor->find_text, &editor->find_length);
		if (status != 0)
			return -1;
		editor->find_key = key;
	} else if (editor->find_key == 0) {
		return -1;
	}

	/* f and t look forward; F and T look back.  , turns the direction around. */
	key = editor->find_key;
	forward = 0;
	if (key == 'f' || key == 't')
		forward = 1;
	if (repeat == ',') {
		if (forward)
			forward = 0;
		else
			forward = 1;
	}

	/* t and T stop beside the character instead of on it. */
	before = 0;
	if (key == 't' || key == 'T')
		before = 1;

	/* Finds the character count times, stepping past the one beside a repeated t. */
	offset = editor->cursor.offset;
	found = offset;
	if (repeat && before) {
		if (forward)
			offset = vi_text_next(line->text, line->length, offset);
		else
			offset = vi_text_previous(line->text, line->length, offset);
	}
	while (count > 0) {
		/* Steps one character in the direction of the search. */
		if (forward) {
			offset = vi_text_next(line->text, line->length, offset);
			if (offset >= line->length)
				return -1;
		} else {
			if (offset == 0)
				return -1;
			offset = vi_text_previous(line->text, line->length, offset);
		}

		/* Counts a match when the character there is the one looked for. */
		if (line->length - offset < editor->find_length)
			continue;
		compare = memcmp(line->text + offset, editor->find_text, editor->find_length);
		if (compare == 0) {
			found = offset;
			count--;
		}
	}

	/* t and T stop beside the character. */
	if (before) {
		if (forward)
			found = vi_text_previous(line->text, line->length, found);
		else
			found = vi_text_next(line->text, line->length, found);
	}

	/* Tells where the movement ends; f and t take in the character they stop on. */
	motion->target.line = editor->cursor.line;
	motion->target.offset = found;
	if (forward)
		motion->inclusive = 1;

	/* Succeeded. */
	return 0;
}

/* Moves to a match of a pattern with /, ?, n or N. */
static int
motion_search(
	struct vi_editor *editor,
	int key,
	unsigned long count,
	struct motion *motion)
{
	struct vi_position position;
	int backward;
	int accepted;
	int status;

	/* / and ? read a new pattern; an empty one reuses the last. */
	if (key == '/' || key == '?') {
		backward = 0;
		if (key == '?')
			backward = 1;
		accepted = vi_read_line(editor, (char)key);
		if (!accepted)
			return -2;
		status = vi_search_set(editor, editor->input, backward);
		if (status != 0)
			return -2;
	} else if (!editor->search_compiled) {
		vi_message(editor, "No previous pattern");
		return -2;
	}

	/* n searches the way the pattern was typed; N the other way. */
	backward = editor->search_backward;
	if (key == 'N') {
		if (backward)
			backward = 0;
		else
			backward = 1;
	}

	/* Finds the match count times. */
	position = editor->cursor;
	while (count > 0) {
		status = vi_search_find(editor, position, backward, &position);
		if (status != 0)
			return -2;
		count--;
	}

	/* Tells where the movement ends. */
	motion->target = position;

	/* Succeeded. */
	return 0;
}

/* Moves to a line, aiming at the remembered column. */
static int
motion_vertical(
	struct vi_editor *editor,
	size_t line,
	struct motion *motion)
{
	struct vi_line *target;

	/* Finds the character under the remembered column of the target line. */
	target = line_at(editor, line);
	motion->target.line = line;
	if (editor->want_column == SIZE_MAX) {
		motion->target.offset = last_offset(target);
	} else {
		motion->target.offset = vi_text_offset_at_column(target->text, target->length, editor->want_column);
	}

	/* A vertical movement takes whole lines and keeps aiming at the column. */
	motion->linewise = 1;
	motion->keep_column = 1;

	/* Succeeded. */
	return 0;
}

/*
 * Works out where a movement key goes.  Returns 0 with the motion, -1 for
 * a key that is no movement or cannot move, or -2 when a message already
 * says why.
 */
static int
motion(
	struct vi_editor *editor,
	int key,
	unsigned long count,
	int has_count,
	int for_operator,
	struct motion *motion)
{
	struct vi_line *line;
	size_t offset;
	size_t last;
	size_t index;
	size_t last_line;
	int big;
	int next;
	int status;

	/* Starts from the cursor, character-wise and exclusive. */
	memset(motion, 0, sizeof(*motion));
	motion->target = editor->cursor;
	line = line_at(editor, editor->cursor.line);
	last_line = editor->buffer.count - 1;

	/* Chooses the movement by its key. */
	switch (key) {
	case 'h':
	case VI_KEY_LEFT:
	case VI_KEY_BACKSPACE:
	case VI_KEY_ERASE:
		/* Back count characters on the line. */
		if (editor->cursor.offset == 0)
			return -1;
		offset = editor->cursor.offset;
		while (count > 0 && offset > 0) {
			offset = vi_text_previous(line->text, line->length, offset);
			count--;
		}

		/* Tells where the movement ends. */
		motion->target.offset = offset;
		return 0;
	case 'l':
	case ' ':
	case VI_KEY_RIGHT:
		/* Forward count characters; an operator may reach the end of the line. */
		offset = editor->cursor.offset;
		last = last_offset(line);
		if (!for_operator) {
			/* A movement without an operator cannot go past the last character. */
			if (offset >= last)
				return -1;
		}
		while (count > 0 && offset < line->length) {
			offset = vi_text_next(line->text, line->length, offset);
			count--;
		}

		/* Without an operator the cursor stays on the last character of the line. */
		if (!for_operator) {
			if (offset > last)
				offset = last;
		}

		/* Tells where the movement ends. */
		motion->target.offset = offset;
		return 0;
	case '0':
	case VI_KEY_HOME:
		motion->target.offset = 0;
		return 0;
	case '^':
		motion->target.offset = first_nonblank(line);
		return 0;
	case '$':
	case VI_KEY_END:
		/* The last character of the line count-1 lines down. */
		index = editor->cursor.line + (size_t)(count - 1);
		if (index > last_line)
			return -1;
		motion->target.line = index;
		motion->target.offset = last_offset(line_at(editor, index));
		motion->inclusive = 1;
		return 0;
	case 'j':
	case VI_KEY_DOWN:
	case VI_KEY_NEWLINE:
		/* The last line has no line below to move to. */
		if (editor->cursor.line == last_line)
			return -1;
		index = editor->cursor.line + (size_t)count;
		if (index > last_line || index < editor->cursor.line)
			index = last_line;
		status = motion_vertical(editor, index, motion);
		return status;
	case 'k':
	case VI_KEY_UP:
		/* The first line has no line above to move to. */
		if (editor->cursor.line == 0)
			return -1;
		index = 0;
		if ((size_t)count < editor->cursor.line)
			index = editor->cursor.line - (size_t)count;
		status = motion_vertical(editor, index, motion);
		return status;
	case '+':
	case VI_KEY_RETURN:
		/* The first character of a later line. */
		if (editor->cursor.line == last_line)
			return -1;
		index = editor->cursor.line + (size_t)count;
		if (index > last_line || index < editor->cursor.line)
			index = last_line;
		motion->target.line = index;
		motion->target.offset = first_nonblank(line_at(editor, index));
		motion->linewise = 1;
		return 0;
	case '-':
		/* The first character of an earlier line. */
		if (editor->cursor.line == 0)
			return -1;
		index = 0;
		if ((size_t)count < editor->cursor.line)
			index = editor->cursor.line - (size_t)count;
		motion->target.line = index;
		motion->target.offset = first_nonblank(line_at(editor, index));
		motion->linewise = 1;
		return 0;
	case 'w':
	case 'W':
		/* The capital letters move by words of anything but blanks. */
		big = 0;
		if (key == 'W')
			big = 1;
		status = motion_word_forward(editor, count, big, for_operator, motion);
		return status;
	case 'e':
	case 'E':
		big = 0;
		if (key == 'E')
			big = 1;
		status = motion_word_end(editor, count, big, motion);
		return status;
	case 'b':
	case 'B':
		big = 0;
		if (key == 'B')
			big = 1;
		status = motion_word_backward(editor, count, big, motion);
		return status;
	case 'G':
		/* Line count, or the last line without a count. */
		index = last_line;
		if (has_count) {
			index = (size_t)count - 1;
			if (index > last_line)
				index = last_line;
		}

		/* Tells where the movement ends: the first character of the line. */
		motion->target.line = index;
		motion->target.offset = first_nonblank(line_at(editor, index));
		motion->linewise = 1;
		return 0;
	case 'g':
		/* gg: line count, or the first line without a count. */
		next = vi_editor_key(editor);
		if (next != 'g')
			return -1;
		index = 0;
		if (has_count) {
			index = (size_t)count - 1;
			if (index > last_line)
				index = last_line;
		}

		/* Tells where the movement ends: the first character of the line. */
		motion->target.line = index;
		motion->target.offset = first_nonblank(line_at(editor, index));
		motion->linewise = 1;
		return 0;
	case 'H':
	case 'M':
	case 'L':
		/* A line of the screen: count from the top or the bottom, or the middle. */
		if (key == 'H') {
			index = editor->top + (size_t)(count - 1);
			if (index > editor->bottom)
				index = editor->bottom;
		} else if (key == 'L') {
			index = editor->top;
			if ((size_t)(count - 1) < editor->bottom - editor->top)
				index = editor->bottom - (size_t)(count - 1);
		} else {
			index = editor->top + (editor->bottom - editor->top) / 2;
		}

		/* Holds the line to the buffer. */
		if (index > last_line)
			index = last_line;
		motion->target.line = index;
		motion->target.offset = first_nonblank(line_at(editor, index));
		motion->linewise = 1;
		return 0;
	case 'f':
	case 'F':
	case 't':
	case 'T':
		status = motion_find(editor, key, count, 0, motion);
		return status;
	case ';':
	case ',':
		status = motion_find(editor, key, count, key, motion);
		return status;
	case '/':
	case '?':
	case 'n':
	case 'N':
		status = motion_search(editor, key, count, motion);
		return status;
	default:
		return -1;
	}
}

/*
 * Copies the text between two positions into a register.  Line-wise, the
 * lines from the start's to the end's are copied whole; otherwise the
 * text from start up to, not including, end.
 */
static int
text_between(
	struct vi_editor *editor,
	struct vi_position start,
	struct vi_position end,
	int linewise,
	struct vi_register *copy)
{
	struct vi_line *line;
	size_t index;
	size_t size;
	size_t from;
	size_t to;
	char *text;

	/* Measures the text: each line's part and a newline between or after lines. */
	size = 0;
	for (index = start.line; index <= end.line; index++) {
		line = line_at(editor, index);
		from = 0;
		to = line->length;
		if (!linewise && index == start.line)
			from = start.offset;
		if (!linewise && index == end.line)
			to = end.offset;
		size += to - from;
		if (linewise || index != end.line)
			size++;
	}

	/* Allocates the copy, never empty so that it can always be released. */
	text = malloc(size + 1);
	if (text == NULL)
		return ENOMEM;

	/* Copies each line's part. */
	size = 0;
	for (index = start.line; index <= end.line; index++) {
		line = line_at(editor, index);
		from = 0;
		to = line->length;
		if (!linewise && index == start.line)
			from = start.offset;
		if (!linewise && index == end.line)
			to = end.offset;
		memcpy(text + size, line->text + from, to - from);
		size += to - from;
		if (linewise || index != end.line) {
			text[size] = '\n';
			size++;
		}
	}

	/* Ends the copy with a NUL. */
	text[size] = '\0';

	/* Hands the copy to the caller. */
	copy->text = text;
	copy->length = size;
	copy->linewise = linewise;

	/* Succeeded: the caller owns the copy. */
	return 0;
}

/* Makes a copy the register p and P put, dropping what it held. */
static void
register_replace(
	struct vi_editor *editor,
	struct vi_register *copy)
{
	/* The old text goes; the register takes over the copy. */
	free(editor->yank.text);
	editor->yank = *copy;
}

/* Deletes whole lines, keeping one empty line when all of them go. */
static int
delete_lines(
	struct vi_editor *editor,
	size_t first,
	size_t last)
{
	int error;

	/* Deleting every line keeps the first, emptied, since the buffer always has a line. */
	if (first == 0 && last + 1 == editor->buffer.count) {
		/* The lines after the first go in one step. */
		if (last > 0) {
			error = vi_edit(editor, VI_CHANGE_DELETE_LINE, 1, 0, "", last);
			if (error != 0)
				return error;
		}

		/* The first line's text goes. */
		error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, 0, 0, "", editor->buffer.lines[0].length);
		if (error != 0)
			return error;
		return 0;
	}

	/* Otherwise the lines go in one step. */
	error = vi_edit(editor, VI_CHANGE_DELETE_LINE, first, 0, "", last - first + 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Deletes the text from start up to, not including, end, joining the lines it spans. */
static int
delete_between(
	struct vi_editor *editor,
	struct vi_position start,
	struct vi_position end)
{
	size_t length;
	size_t middle;
	int error;

	/* Within one line, the bytes between go. */
	if (start.line == end.line) {
		error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, start.line, start.offset, "", end.offset - start.offset);
		if (error != 0)
			return error;
		return 0;
	}

	/* The rest of the first line goes. */
	length = line_at(editor, start.line)->length - start.offset;
	error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, start.line, start.offset, "", length);
	if (error != 0)
		return error;

	/* The lines in between go whole, in one step. */
	middle = end.line - start.line - 1;
	if (middle > 0) {
		error = vi_edit(editor, VI_CHANGE_DELETE_LINE, start.line + 1, 0, "", middle);
		if (error != 0)
			return error;
	}

	/* The start of the last line goes, and what is left joins the first. */
	error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, start.line + 1, 0, "", end.offset);
	if (error != 0)
		return error;

	/* Joins what is left of the last line onto the first. */
	error = vi_edit(editor, VI_CHANGE_JOIN_LINE, start.line, 0, "", 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Runs the d, c or y operator: reads the movement or the doubled key that
 * says what it acts on, copies that text to the register, and deletes it
 * for d and c, starting insert mode for c.
 */
static void
operator(
	struct vi_editor *editor,
	int operator_key,
	unsigned long count)
{
	struct motion moved;
	struct vi_register copy;
	struct vi_position start;
	struct vi_position end;
	struct vi_line *line;
	unsigned long inner_count;
	int has_inner_count;
	int at_word_end;
	int reversed;
	int empty;
	int big;
	int key;
	int status;
	int error;
	int class;

	/* Reads the movement, with a count of its own that multiplies the first. */
	key = vi_editor_key(editor);
	key = read_count(editor, key, &inner_count, &has_inner_count);
	if (key == VI_KEY_STOP)
		return;
	count = count * inner_count;
	if (count > VI_COUNT_LIMIT)
		count = VI_COUNT_LIMIT;

	/* The doubled key (dd, cc, yy) acts on count whole lines. */
	if (key == operator_key) {
		/* Refuses a count of more lines than the buffer has from the cursor on. */
		if (editor->cursor.line + (size_t)(count - 1) >= editor->buffer.count) {
			vi_bell(editor);
			return;
		}

		/* Describes the movement as the whole lines down from the cursor's. */
		memset(&moved, 0, sizeof(moved));
		moved.target.line = editor->cursor.line + (size_t)(count - 1);
		moved.linewise = 1;
	} else {
		/* cw changes to the end of the word, unless the cursor is on a blank. */
		at_word_end = 0;
		if (operator_key == 'c') {
			if (key == 'w' || key == 'W') {
				big = 0;
				if (key == 'W')
					big = 1;
				class = class_at(editor, editor->cursor, big);
				if (class != CLASS_BLANK && class != CLASS_EMPTY) {
					key = 'e';
					if (big)
						key = 'E';
					at_word_end = word_ends_at(editor, editor->cursor, big);
				}
			}
		}

		/* Works out where the movement goes; a failed one changes nothing. */
		status = motion(editor, key, count, has_inner_count, 1, &moved);
		if (status != 0) {
			/* A key that is no movement rings the bell; a movement that failed with a message says why itself. */
			if (status == -1)
				vi_bell(editor);
			return;
		}

		/* cw on the last character of a word changes that character alone. */
		if (at_word_end && count == 1)
			moved.target = editor->cursor;
	}

	/* Puts the two ends in order. */
	start = editor->cursor;
	end = moved.target;
	reversed = position_before(end, start);
	if (reversed) {
		start = moved.target;
		end = editor->cursor;
	}

	/* An inclusive movement takes in the character at its end. */
	if (!moved.linewise && moved.inclusive) {
		line = line_at(editor, end.line);
		end.offset = vi_text_next(line->text, line->length, end.offset);
	}

	/* Line-wise, the lines are taken from their starts. */
	if (moved.linewise) {
		start.offset = 0;
		end.offset = 0;
	}

	/* A character-wise movement that covers nothing does nothing, except start c's insertion. */
	empty = 0;
	if (!moved.linewise) {
		if (start.line == end.line && start.offset == end.offset)
			empty = 1;
	}

	/* Refuses an empty text, except for c, which still starts typing. */
	if (empty && operator_key != 'c') {
		vi_bell(editor);
		return;
	}

	/* Copies the text into the register. */
	error = text_between(editor, start, end, moved.linewise, &copy);
	if (error != 0) {
		report_error(editor, error);
		return;
	}

	/* Makes the copy what p and P put. */
	register_replace(editor, &copy);

	/* y only copies; the cursor goes to the start of the text, keeping its column for lines. */
	if (operator_key == 'y') {
		if (moved.linewise) {
			editor->cursor.line = start.line;
		} else {
			editor->cursor = start;
		}

		/* y is done once the text is copied. */
		return;
	}

	/* d and c delete the text. */
	if (moved.linewise) {
		if (operator_key == 'c') {
			/* c keeps the first line, emptied, to type into. */
			error = 0;
			if (end.line > start.line)
				error = delete_lines(editor, start.line + 1, end.line);
			if (error == 0)
				error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, start.line, 0, "", line_at(editor, start.line)->length);
		} else {
			error = delete_lines(editor, start.line, end.line);
		}
	} else {
		error = delete_between(editor, start, end);
	}

	/* Reports a delete the buffer could not take. */
	if (error != 0) {
		report_error(editor, error);
		return;
	}

	/* The cursor goes to where the text was. */
	editor->cursor = start;
	if (moved.linewise && operator_key == 'd') {
		vi_cursor_clamp(editor);
		vi_cursor_first_nonblank(editor);
	}

	/* c goes on to insert in place of the text. */
	if (operator_key == 'c')
		insert_mode(editor, 1);
}

/*
 * Takes typed text into the buffer until ESC.  With a count, text typed
 * on one line is put in count times in all (5i- makes -----).
 */
static void
insert_mode(
	struct vi_editor *editor,
	unsigned long count)
{
	struct vi_line *line;
	int key;
	int done;

	/* The cursor may now rest at the end of the line; the typed text starts here. */
	editor->mode = VI_MODE_INSERT;
	editor->insert_start = editor->cursor;

	/* Takes keys until ESC or CONTROL-C, or until the editor stops. */
	for (;;) {
		key = vi_editor_key(editor);
		done = key_ends_input(key);
		if (done)
			break;
		insert_key(editor, key);
	}

	/* A count puts the typed text in again. */
	if (count > 1 && key != VI_KEY_STOP)
		insert_repeat(editor, count);

	/* Back in command mode the cursor steps back onto the last character typed. */
	editor->mode = VI_MODE_COMMAND;
	line = line_at(editor, editor->cursor.line);
	if (editor->cursor.offset > 0)
		editor->cursor.offset = vi_text_previous(line->text, line->length, editor->cursor.offset);
	vi_cursor_clamp(editor);
}

/* Does what one key typed in insert mode does. */
static void
insert_key(
	struct vi_editor *editor,
	int key)
{
	struct vi_line *line;
	struct motion moved;
	char text[8];
	size_t length;
	size_t start;
	size_t offset;
	enum char_class class;
	enum char_class previous_class;
	struct vi_position position;
	int status;
	int error;
	int typed;

	/* Finds the cursor's line. */
	line = line_at(editor, editor->cursor.line);
	error = 0;

	/* Chooses what the key does. */
	switch (key) {
	case VI_KEY_RETURN:
	case VI_KEY_NEWLINE:
		/* Splits the line at the cursor. */
		error = vi_edit(editor, VI_CHANGE_SPLIT_LINE, editor->cursor.line, editor->cursor.offset, "", 0);
		if (error == 0) {
			editor->cursor.line++;
			editor->cursor.offset = 0;
		}

		break;
	case VI_KEY_BACKSPACE:
	case VI_KEY_ERASE:
		/* Erases the character before the cursor, or joins with the line above. */
		if (editor->cursor.offset > 0) {
			start = vi_text_previous(line->text, line->length, editor->cursor.offset);
			error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, editor->cursor.line, start, "", editor->cursor.offset - start);
			if (error == 0)
				editor->cursor.offset = start;
		} else if (editor->cursor.line > 0) {
			offset = line_at(editor, editor->cursor.line - 1)->length;
			error = vi_edit(editor, VI_CHANGE_JOIN_LINE, editor->cursor.line - 1, 0, "", 0);
			if (error == 0) {
				editor->cursor.line--;
				editor->cursor.offset = offset;
			}
		} else {
			vi_bell(editor);
		}

		break;
	case VI_KEY_DELETE:
		/* Deletes the character under the cursor, or joins with the line below. */
		if (editor->cursor.offset < line->length) {
			offset = vi_text_next(line->text, line->length, editor->cursor.offset);
			error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, editor->cursor.line, editor->cursor.offset, "", offset - editor->cursor.offset);
		} else if (editor->cursor.line + 1 < editor->buffer.count) {
			error = vi_edit(editor, VI_CHANGE_JOIN_LINE, editor->cursor.line, 0, "", 0);
		} else {
			vi_bell(editor);
		}

		break;
	case VI_KEY_CONTROL_W:
		/* Erases the word before the cursor, and the blanks after it. */
		position = editor->cursor;
		while (position.offset > 0) {
			offset = vi_text_previous(line->text, line->length, position.offset);
			position.offset = offset;
			class = class_at(editor, position, 0);
			if (class != CLASS_BLANK) {
				position.offset = offset;
				break;
			}
		}

		/* Finds the class of the word the blanks led back to. */
		class = class_at(editor, position, 0);
		while (position.offset > 0) {
			offset = vi_text_previous(line->text, line->length, position.offset);
			position.offset = offset;
			previous_class = class_at(editor, position, 0);
			if (previous_class != class) {
				position.offset = vi_text_next(line->text, line->length, offset);
				break;
			}
		}

		/* Erases the word and the blanks after it, if there is any. */
		if (position.offset < editor->cursor.offset) {
			error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, editor->cursor.line, position.offset, "", editor->cursor.offset - position.offset);
			if (error == 0)
				editor->cursor.offset = position.offset;
		}

		break;
	case VI_KEY_CONTROL_U:
		/* Erases everything before the cursor on the line. */
		if (editor->cursor.offset > 0) {
			error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, editor->cursor.line, 0, "", editor->cursor.offset);
			if (error == 0)
				editor->cursor.offset = 0;
		}

		break;
	case VI_KEY_LEFT:
	case VI_KEY_RIGHT:
	case VI_KEY_UP:
	case VI_KEY_DOWN:
	case VI_KEY_HOME:
	case VI_KEY_END:
		/* A movement ends one undoable change and starts the next. */
		vi_undo_end(editor);
		(void)vi_undo_begin(editor);

		/* Moves the cursor, which may rest at the end of the line. */
		if (key == VI_KEY_LEFT) {
			editor->cursor.offset = vi_text_previous(line->text, line->length, editor->cursor.offset);
		} else if (key == VI_KEY_RIGHT) {
			editor->cursor.offset = vi_text_next(line->text, line->length, editor->cursor.offset);
		} else if (key == VI_KEY_HOME) {
			editor->cursor.offset = 0;
		} else if (key == VI_KEY_END) {
			editor->cursor.offset = line->length;
		} else {
			status = motion(editor, key, 1, 0, 0, &moved);
			if (status == 0)
				editor->cursor = moved.target;
		}

		/* Remembers the column, except after a vertical movement, which keeps aiming at the old one. */
		if (key != VI_KEY_UP && key != VI_KEY_DOWN) {
			line = line_at(editor, editor->cursor.line);
			editor->want_column = vi_text_column(line->text, line->length, editor->cursor.offset);
		}

		/* Text typed after a movement is what a count repeats. */
		editor->insert_start = editor->cursor;
		break;
	default:
		/* A typed character goes in at the cursor; other control keys are refused. */
		typed = key_is_text(key);
		if (!typed) {
			vi_bell(editor);
			break;
		}

		/* Reads the whole character the key starts. */
		status = read_character(editor, key, text, &length);
		if (status != 0)
			break;
		error = insert_text(editor, text, length);
		break;
	}

	/* Tells of an edit the buffer could not take. */
	if (error != 0)
		report_error(editor, error);
}

/* Inserts text without newlines at the cursor and moves the cursor after it. */
static int
insert_text(
	struct vi_editor *editor,
	const char *text,
	size_t length)
{
	int error;

	/* Puts the text in at the cursor. */
	error = vi_edit(editor, VI_CHANGE_INSERT_TEXT, editor->cursor.line, editor->cursor.offset, text, length);
	if (error != 0)
		return error;

	/* Moves the cursor past the text. */
	editor->cursor.offset += length;

	/* Succeeded. */
	return 0;
}

/*
 * Puts the register's text after the cursor with p or before it with P,
 * count times.
 */
static void
command_put(
	struct vi_editor *editor,
	int before,
	unsigned long count)
{
	struct vi_register *yank;
	struct vi_line *line;
	struct vi_position at;
	size_t index;
	size_t start;
	size_t first_line;
	size_t first_offset;
	size_t size;
	char *block;
	int newlines;
	int error;

	/* Finds the register. */
	yank = &editor->yank;

	/* An empty register has nothing to put. */
	if (yank->text == NULL) {
		vi_bell(editor);
		return;
	}

	/* Line-wise text goes in as whole lines below or above the cursor's line. */
	if (yank->linewise) {
		at.line = editor->cursor.line + 1;
		if (before)
			at.line = editor->cursor.line;
		first_line = at.line;

		/* The copies go in as one block of lines, which must stay within the file size limit. */
		if (yank->length > VI_FILE_SIZE_LIMIT / count) {
			report_error(editor, EFBIG);
			return;
		}

		/* Allocates a block that holds all the copies. */
		size = yank->length * (size_t)count;
		block = malloc(size + 1);
		if (block == NULL) {
			report_error(editor, ENOMEM);
			return;
		}

		/* Fills the block with the copies. */
		for (index = 0; index < (size_t)count; index++)
			memcpy(block + index * yank->length, yank->text, yank->length);
		error = vi_edit(editor, VI_CHANGE_INSERT_LINE, at.line, 0, block, size);
		free(block);
		if (error != 0) {
			report_error(editor, error);
			return;
		}

		/* The cursor goes to the first line put in. */
		editor->cursor.line = first_line;
		vi_cursor_first_nonblank(editor);
		return;
	}

	/* Character-wise text goes in after or at the cursor's character. */
	line = line_at(editor, editor->cursor.line);
	at = editor->cursor;
	if (!before && line->length != 0)
		at.offset = vi_text_next(line->text, line->length, at.offset);
	first_line = at.line;
	first_offset = at.offset;

	/* Inserts each piece between newlines, splitting the line at each newline. */
	newlines = 0;
	while (count > 0) {
		count--;
		start = 0;
		for (index = 0; index <= yank->length; index++) {
			if (index < yank->length && yank->text[index] != '\n')
				continue;

			/* The piece before the newline, or before the end. */
			if (index > start) {
				error = vi_edit(editor, VI_CHANGE_INSERT_TEXT, at.line, at.offset, yank->text + start, index - start);
				if (error != 0) {
					report_error(editor, error);
					return;
				}

				/* Moves past the piece put in. */
				at.offset += index - start;
			}

			/* The newline splits the line. */
			if (index < yank->length) {
				error = vi_edit(editor, VI_CHANGE_SPLIT_LINE, at.line, at.offset, "", 0);
				if (error != 0) {
					report_error(editor, error);
					return;
				}

				/* Moves to the start of the new line. */
				at.line++;
				at.offset = 0;
				newlines = 1;
			}

			/* The next piece starts after the newline. */
			start = index + 1;
		}
	}

	/* Text of one line leaves the cursor on its last character; more lines, at its start. */
	if (newlines) {
		editor->cursor.line = first_line;
		editor->cursor.offset = first_offset;
	} else {
		line = line_at(editor, at.line);
		editor->cursor.line = at.line;
		editor->cursor.offset = vi_text_previous(line->text, line->length, at.offset);
	}
}

/* Deletes count characters under and after the cursor (x), or before it (X). */
static void
command_delete_characters(
	struct vi_editor *editor,
	unsigned long count,
	int backward)
{
	struct vi_line *line;
	struct vi_position start;
	struct vi_position end;
	struct vi_register copy;
	int error;

	/* Finds the cursor's line, which the characters are taken from. */
	line = line_at(editor, editor->cursor.line);
	start = editor->cursor;
	end = editor->cursor;

	/* Measures the characters, stopping at the line's ends. */
	if (backward) {
		while (count > 0 && start.offset > 0) {
			start.offset = vi_text_previous(line->text, line->length, start.offset);
			count--;
		}
	} else {
		while (count > 0 && end.offset < line->length) {
			end.offset = vi_text_next(line->text, line->length, end.offset);
			count--;
		}
	}

	/* Nothing to delete rings the bell. */
	if (start.offset == end.offset) {
		vi_bell(editor);
		return;
	}

	/* The characters go to the register, then out of the line. */
	error = text_between(editor, start, end, 0, &copy);
	if (error != 0) {
		report_error(editor, error);
		return;
	}

	/* Makes the copy what p and P put. */
	register_replace(editor, &copy);

	/* Takes the characters out of the line. */
	error = delete_between(editor, start, end);
	if (error != 0) {
		report_error(editor, error);
		return;
	}

	/* The cursor goes to where the characters were. */
	editor->cursor = start;
}

/* Replaces count characters with one typed character (r); r RETURN splits the line. */
static void
command_replace(
	struct vi_editor *editor,
	unsigned long count)
{
	struct vi_line *line;
	struct vi_position end;
	char text[8];
	size_t length;
	unsigned long replaced;
	int key;
	int status;
	int error;

	/* Finds the cursor's line. */
	line = line_at(editor, editor->cursor.line);

	/* Reads the replacing character; ESC cancels. */
	key = vi_editor_key(editor);
	if (key == VI_KEY_ESCAPE || key == VI_KEY_STOP)
		return;

	/* Measures count characters, which must all be on the line. */
	replaced = count;
	end = editor->cursor;
	while (count > 0) {
		/* Refuses a count of more characters than the line has. */
		if (end.offset >= line->length) {
			vi_bell(editor);
			return;
		}

		/* Steps over one character. */
		end.offset = vi_text_next(line->text, line->length, end.offset);
		count--;
	}

	/* RETURN replaces the characters with a line break. */
	if (key == VI_KEY_RETURN || key == VI_KEY_NEWLINE) {
		error = delete_between(editor, editor->cursor, end);
		if (error == 0)
			error = vi_edit(editor, VI_CHANGE_SPLIT_LINE, editor->cursor.line, editor->cursor.offset, "", 0);
		if (error != 0) {
			report_error(editor, error);
			return;
		}

		/* The cursor goes to the start of the new line. */
		editor->cursor.line++;
		editor->cursor.offset = 0;
		return;
	}

	/* Reads the whole character the key starts. */
	status = read_character(editor, key, text, &length);
	if (status != 0) {
		vi_bell(editor);
		return;
	}

	/* Deletes the characters and puts the new one in for each. */
	count = replaced;
	error = delete_between(editor, editor->cursor, end);
	if (error != 0) {
		report_error(editor, error);
		return;
	}

	/* Puts the new character in once for each deleted one. */
	end = editor->cursor;
	while (count > 0) {
		error = vi_edit(editor, VI_CHANGE_INSERT_TEXT, end.line, end.offset, text, length);
		if (error != 0) {
			report_error(editor, error);
			return;
		}

		/* Moves past the character put in. */
		end.offset += length;
		count--;
	}

	/* The cursor ends on the last replaced character. */
	editor->cursor.offset = end.offset - length;
}

/* Joins count lines into one (J), at least two, with one space between their texts. */
static void
command_join(
	struct vi_editor *editor,
	unsigned long count)
{
	struct vi_line *first;
	struct vi_line *next;
	size_t length;
	size_t blanks;
	int add_space;
	int error;

	/* J joins count lines, which is count - 1 joins, and at least one. */
	if (count > 1)
		count--;

	/* There must be a line below to join. */
	if (editor->cursor.line + 1 >= editor->buffer.count) {
		vi_bell(editor);
		return;
	}

	/* Joins the line below onto the cursor's line, count times. */
	while (count > 0) {
		count--;
		if (editor->cursor.line + 1 >= editor->buffer.count)
			break;

		/* Drops the leading blanks of the line below. */
		next = line_at(editor, editor->cursor.line + 1);
		blanks = 0;
		while (blanks < next->length) {
			if (next->text[blanks] != ' ' && next->text[blanks] != '\t')
				break;
			blanks++;
		}

		/* Deletes the blanks, if there are any. */
		if (blanks > 0) {
			error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, editor->cursor.line + 1, 0, "", blanks);
			if (error != 0) {
				report_error(editor, error);
				return;
			}
		}

		/* A space goes between two texts, unless one side is empty, the first ends in a blank, or the second starts with ). */
		first = line_at(editor, editor->cursor.line);
		next = line_at(editor, editor->cursor.line + 1);
		length = first->length;
		add_space = 1;
		if (length == 0)
			add_space = 0;
		else if (next->length == 0)
			add_space = 0;
		else if (first->text[length - 1] == ' ' || first->text[length - 1] == '\t')
			add_space = 0;
		else if (next->text[0] == ')')
			add_space = 0;

		/* Joins the two lines. */
		error = vi_edit(editor, VI_CHANGE_JOIN_LINE, editor->cursor.line, 0, "", 0);
		if (error != 0) {
			report_error(editor, error);
			return;
		}

		/* Puts the space in between, when the texts need one. */
		if (add_space) {
			error = vi_edit(editor, VI_CHANGE_INSERT_TEXT, editor->cursor.line, length, " ", 1);
			if (error != 0) {
				report_error(editor, error);
				return;
			}
		}

		/* The cursor goes to the join. */
		editor->cursor.offset = length;
	}
}

/* Switches the case of count ASCII letters from the cursor on (~), moving past them. */
static void
command_toggle_case(
	struct vi_editor *editor,
	unsigned long count)
{
	struct vi_line *line;
	size_t offset;
	char letter;
	int error;

	/* An empty line has nothing to switch. */
	line = line_at(editor, editor->cursor.line);
	if (line->length == 0) {
		vi_bell(editor);
		return;
	}

	/* Switches each character in turn, stopping at the end of the line. */
	offset = editor->cursor.offset;
	while (count > 0) {
		line = line_at(editor, editor->cursor.line);
		if (offset >= line->length)
			break;
		count--;

		/* Only ASCII letters change; everything else is stepped over. */
		letter = line->text[offset];
		if (letter >= 'a' && letter <= 'z')
			letter = (char)(letter - 'a' + 'A');
		else if (letter >= 'A' && letter <= 'Z')
			letter = (char)(letter - 'A' + 'a');
		if (letter != line->text[offset]) {
			error = vi_edit(editor, VI_CHANGE_DELETE_TEXT, editor->cursor.line, offset, "", 1);
			if (error == 0)
				error = vi_edit(editor, VI_CHANGE_INSERT_TEXT, editor->cursor.line, offset, &letter, 1);
			if (error != 0) {
				report_error(editor, error);
				return;
			}

			/* Finds the line again, since the edit changed it. */
			line = line_at(editor, editor->cursor.line);
		}

		/* Moves on to the next character. */
		offset = vi_text_next(line->text, line->length, offset);
	}

	/* The cursor ends after the last character switched. */
	editor->cursor.offset = offset;
}

/* Takes back count changes (u), or makes count of them again (CONTROL-R). */
static void
command_undo(
	struct vi_editor *editor,
	unsigned long count,
	int forward)
{
	int error;

	/* Steps through the history count times. */
	while (count > 0) {
		count--;
		if (forward)
			error = vi_undo_forward(editor);
		else
			error = vi_undo_back(editor);

		/* The end of the history stops the steps. */
		if (error == ENOENT) {
			if (forward)
				vi_message(editor, "Already at newest change");
			else
				vi_message(editor, "Already at oldest change");
			break;
		}

		/* Reports any other failure of the history. */
		if (error != 0) {
			report_error(editor, error);
			break;
		}
	}

	/* The history's cursor may lie past a line that no longer exists. */
	vi_cursor_clamp(editor);
}

/*
 * Repeats the last change (.) by feeding its keys back in, with the new
 * count in place of the old one when a count is typed.
 */
static void
command_repeat(
	struct vi_editor *editor,
	unsigned long count,
	int has_count)
{
	char digits[24];
	struct vi_keys *replay;
	struct vi_keys *repeat;
	size_t index;
	int length;

	/* Finds the keys to feed back and the keys of the change. */
	replay = &editor->replay;
	repeat = &editor->repeat;

	/* Nothing has been changed yet. */
	if (repeat->length == 0) {
		vi_bell(editor);
		return;
	}

	/* The count: the new one when typed, or the change's own. */
	replay->length = 0;
	replay->position = 0;
	if (has_count) {
		length = snprintf(digits, sizeof(digits), "%lu", count);
		for (index = 0; index < (size_t)length; index++) {
			replay->keys[replay->length] = digits[index];
			replay->length++;
		}
	} else {
		for (index = 0; index < repeat->count_end; index++) {
			replay->keys[replay->length] = repeat->keys[index];
			replay->length++;
		}
	}

	/* Then the keys of the change after its count, as many as fit. */
	for (index = repeat->count_end; index < repeat->length; index++) {
		if (replay->length >= VI_RECORD_LENGTH)
			break;
		replay->keys[replay->length] = repeat->keys[index];
		replay->length++;
	}
}

/* Scrolls by pages (CONTROL-F, CONTROL-B) or half pages (CONTROL-D, CONTROL-U). */
static void
command_page(
	struct vi_editor *editor,
	int key,
	unsigned long count)
{
	size_t last_line;
	size_t step;
	size_t text_rows;

	/* Finds the last line, and how many rows show text. */
	last_line = editor->buffer.count - 1;
	text_rows = (size_t)editor->terminal.rows - 1;

	/* Chooses the direction and the size of the step. */
	switch (key) {
	case VI_KEY_CONTROL_F:
	case VI_KEY_PAGE_DOWN:
		/* A page forward starts at the last line the screen showed whole. */
		if (editor->bottom >= last_line) {
			vi_bell(editor);
			return;
		}

		/* A page is the lines the screen showed whole, at least one. */
		step = editor->bottom - editor->top;
		if (step == 0)
			step = 1;
		editor->top += step * (size_t)count;
		if (editor->top > last_line)
			editor->top = last_line;
		editor->top_skip = 0;
		editor->cursor.line = editor->top;
		break;
	case VI_KEY_CONTROL_B:
	case VI_KEY_PAGE_UP:
		/* A page back keeps two lines of the old screen. */
		if (editor->top == 0) {
			vi_bell(editor);
			return;
		}

		/* A page is the screen less two lines, at least one. */
		step = 1;
		if (text_rows > 2)
			step = text_rows - 2;
		step = step * (size_t)count;
		if (editor->top > step)
			editor->top -= step;
		else
			editor->top = 0;
		editor->top_skip = 0;
		editor->cursor.line = editor->top + text_rows - 2;
		if (text_rows < 2)
			editor->cursor.line = editor->top;
		if (editor->cursor.line > last_line)
			editor->cursor.line = last_line;
		break;
	case VI_KEY_CONTROL_D:
		/* Half a page down, or count lines. */
		if (editor->cursor.line >= last_line) {
			vi_bell(editor);
			return;
		}

		/* A half page is half the rows, or count lines. */
		step = text_rows / 2;
		if (count > 1)
			step = (size_t)count;
		if (step == 0)
			step = 1;
		editor->cursor.line += step;
		if (editor->cursor.line > last_line)
			editor->cursor.line = last_line;
		editor->top += step;
		if (editor->top > editor->cursor.line)
			editor->top = editor->cursor.line;
		editor->top_skip = 0;
		break;
	default:
		/* Half a page up, or count lines. */
		if (editor->cursor.line == 0) {
			vi_bell(editor);
			return;
		}

		/* A half page is half the rows, or count lines. */
		step = text_rows / 2;
		if (count > 1)
			step = (size_t)count;
		if (step == 0)
			step = 1;
		if (editor->cursor.line > step)
			editor->cursor.line -= step;
		else
			editor->cursor.line = 0;
		if (editor->top > step)
			editor->top -= step;
		else
			editor->top = 0;
		editor->top_skip = 0;
		break;
	}

	/* The cursor goes to the first character of its new line. */
	vi_cursor_first_nonblank(editor);
}

/* Opens a new line below the cursor's (o) or above it (O) and inserts there. */
static void
command_open(
	struct vi_editor *editor,
	int above)
{
	size_t index;
	int error;

	/* Adds the empty line. */
	index = editor->cursor.line + 1;
	if (above)
		index = editor->cursor.line;
	error = vi_edit(editor, VI_CHANGE_INSERT_LINE, index, 0, "\n", 1);
	if (error != 0) {
		report_error(editor, error);
		return;
	}

	/* Types into the new line. */
	editor->cursor.line = index;
	editor->cursor.offset = 0;
	insert_mode(editor, 1);
}

/* Reads a : command line and runs it. */
static void
command_line(
	struct vi_editor *editor)
{
	int accepted;

	/* An abandoned line runs nothing. */
	accepted = vi_read_line(editor, ':');
	if (!accepted)
		return;

	/* Runs the typed command. */
	vi_ex_command(editor, editor->input);
}

/* Adds a key to the keys of the running command, for the . command. */
static void
record_key(
	struct vi_editor *editor,
	int key)
{
	struct vi_keys *record;

	/* Finds the keys recorded for the running command. */
	record = &editor->record;

	/* Only keys of a recorded command are kept. */
	if (!editor->recording)
		return;

	/* A command with too many keys cannot be repeated. */
	if (record->length >= VI_RECORD_LENGTH) {
		record->overflow = 1;
		return;
	}

	/* Adds the key. */
	record->keys[record->length] = key;
	record->length++;
}

/* Tells whether a key types a character: any byte but the control characters, tab excepted. */
static int
key_is_text(
	int key)
{
	/* The special keys have no byte. */
	if (key > 0xff)
		return 0;

	/* A tab is typed text; the other control characters are commands. */
	if (key == VI_KEY_TAB)
		return 1;
	if (key < 0x20)
		return 0;

	/* Any other byte is text, a UTF-8 byte included. */
	return 1;
}

/* Tells whether a key ends typing: ESC, CONTROL-C, or the editor stopping. */
static int
key_ends_input(
	int key)
{
	/* The keys that end insert mode and the bottom line. */
	if (key == VI_KEY_ESCAPE)
		return 1;
	if (key == VI_KEY_CONTROL_C)
		return 1;
	if (key == VI_KEY_STOP)
		return 1;

	/* Any other key goes on. */
	return 0;
}

/* Tells whether the character at a position is the last of its word on the line. */
static int
word_ends_at(
	struct vi_editor *editor,
	struct vi_position position,
	int big)
{
	struct vi_position next;
	enum char_class class;
	enum char_class next_class;
	int status;

	/* The next character must exist, be on the same line, and be of the same class. */
	class = class_at(editor, position, big);
	next = position;
	status = step_forward(editor, &next);
	if (status != 0)
		return 1;
	if (next.line != position.line)
		return 1;
	next_class = class_at(editor, next, big);
	if (next_class != class)
		return 1;

	/* The word goes on past the position. */
	return 0;
}

/* Tells why an edit could not be made. */
static void
report_error(
	struct vi_editor *editor,
	int error)
{
	/* A file grown past the limit, or memory run out. */
	if (error == EFBIG)
		vi_message(editor, "File too large");
	else
		vi_message(editor, "Out of memory");
}

/*
 * Puts the text typed since insert mode began in count - 1 more times
 * after it.  Only text typed on one line, after the last movement, is
 * repeated; anything else is left as typed.
 */
static void
insert_repeat(
	struct vi_editor *editor,
	unsigned long count)
{
	struct vi_line *line;
	struct vi_position start;
	size_t length;
	size_t index;
	char *block;
	int error;

	/* Finds where the typed text began. */
	start = editor->insert_start;

	/* The typed text must be on the cursor's line, before the cursor. */
	if (start.line != editor->cursor.line)
		return;
	if (start.offset >= editor->cursor.offset)
		return;

	/* Copies the typed text count - 1 times, within the file size limit. */
	line = line_at(editor, editor->cursor.line);
	length = editor->cursor.offset - start.offset;
	count--;
	if (length > VI_FILE_SIZE_LIMIT / count) {
		report_error(editor, EFBIG);
		return;
	}

	/* Allocates the block of copies. */
	block = malloc(length * (size_t)count + 1);
	if (block == NULL) {
		report_error(editor, ENOMEM);
		return;
	}

	/* Fills the block with the copies. */
	for (index = 0; index < (size_t)count; index++)
		memcpy(block + index * length, line->text + start.offset, length);

	/* Puts the copies in after the typed text. */
	error = vi_edit(editor, VI_CHANGE_INSERT_TEXT, editor->cursor.line, editor->cursor.offset, block, length * (size_t)count);
	free(block);
	if (error != 0) {
		report_error(editor, error);
		return;
	}

	/* The cursor goes after the copies. */
	editor->cursor.offset += length * (size_t)count;
}
