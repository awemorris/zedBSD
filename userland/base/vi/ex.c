/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The commands typed after a colon: going to a line, writing, quitting,
 * editing another file and showing the file's status.
 */

#include "userland/base/vi/vi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest command name read after the colon. */
#define EX_NAME_LENGTH 16U

static int name_is(const char *name, const char *short_name, const char *long_name);
static void go_to_line(struct vi_editor *editor, const char *text);
static void quit(struct vi_editor *editor, int force);
static void write_and_quit(struct vi_editor *editor, const char *argument, int force, int only_if_modified);
static void edit(struct vi_editor *editor, const char *argument, int force);

/*
 * Runs one command typed after a colon.
 */
void
vi_ex_command(
	struct vi_editor *editor,
	const char *line)
{
	char name[EX_NAME_LENGTH];
	char argument[VI_INPUT_LENGTH];
	size_t length;
	size_t end;
	int is_command;
	int force;

	/* Skips the blanks before the command. */
	while (*line == ' ' || *line == '\t')
		line++;

	/* An empty command does nothing. */
	if (*line == '\0')
		return;

	/* A line number, $ or . alone goes to that line. */
	if (*line >= '0' && *line <= '9') {
		go_to_line(editor, line);
		return;
	}

	/* The last line and the current line have names of their own. */
	if (*line == '$' || *line == '.') {
		go_to_line(editor, line);
		return;
	}

	/* Reads the command's name: the letters at the start. */
	length = 0;
	while (*line >= 'a' && *line <= 'z') {
		/* A name longer than any command is not a command. */
		if (length + 1 >= sizeof(name)) {
			vi_message(editor, "Unknown command");
			return;
		}

		/* Takes the letter into the name. */
		name[length] = *line;
		length++;
		line++;
	}

	/* Ends the name. */
	name[length] = '\0';

	/* A ! right after the name forces the command. */
	force = 0;
	if (*line == '!') {
		force = 1;
		line++;
	}

	/* The rest, without the blanks around it, is the argument. */
	while (*line == ' ' || *line == '\t')
		line++;
	(void)snprintf(argument, sizeof(argument), "%s", line);
	end = strlen(argument);
	while (end > 0) {
		if (argument[end - 1] != ' ' && argument[end - 1] != '\t')
			break;

		/* Drops one blank from the end. */
		end--;
	}

	/* Cuts the blanks off the end of the argument. */
	argument[end] = '\0';

	/* A line with no letters at its start has no command to run. */
	if (length == 0) {
		vi_message(editor, "Unknown command");
		return;
	}

	/* :w writes the buffer, to the named file if there is one. */
	is_command = name_is(name, "w", "write");
	if (is_command) {
		if (argument[0] == '\0')
			(void)vi_file_write(editor, NULL, force);
		else
			(void)vi_file_write(editor, argument, force);

		/* The command is done. */
		return;
	}

	/* :q leaves the editor. */
	is_command = name_is(name, "q", "quit");
	if (is_command) {
		quit(editor, force);
		return;
	}

	/* :wq writes the buffer and leaves. */
	is_command = name_is(name, "wq", "wq");
	if (is_command) {
		write_and_quit(editor, argument, force, 0);
		return;
	}

	/* :x writes the buffer if it changed and leaves. */
	is_command = name_is(name, "x", "xit");
	if (is_command) {
		write_and_quit(editor, argument, force, 1);
		return;
	}

	/* :e reads another file in place of the buffer. */
	is_command = name_is(name, "e", "edit");
	if (is_command) {
		edit(editor, argument, force);
		return;
	}

	/* :f shows the file's status. */
	is_command = name_is(name, "f", "file");
	if (is_command) {
		vi_ex_file_status(editor);
		return;
	}

	/* Names the command vi does not have. */
	(void)snprintf(editor->message, sizeof(editor->message), "Unknown command: %s", name);
}

/*
 * Shows the file's name, whether it was changed, and where the cursor is.
 */
void
vi_ex_file_status(
	struct vi_editor *editor)
{
	const char *name;
	const char *modified;
	unsigned long line;
	unsigned long count;
	int modified_flag;

	/* A buffer without a file has no name to show. */
	name = editor->file_name;
	if (name == NULL)
		name = "[No Name]";

	/* A changed buffer says so. */
	modified = "";
	modified_flag = vi_undo_modified(editor);
	if (modified_flag)
		modified = " [Modified]";

	/* Shows the line as one of the total and as a percentage. */
	line = (unsigned long)editor->cursor.line + 1;
	count = (unsigned long)editor->buffer.count;
	(void)snprintf(
		editor->message,
		sizeof(editor->message),
		"\"%s\"%s line %lu of %lu --%lu%%--",
		name,
		modified,
		line,
		count,
		line * 100 / count);
}

/* Tells whether a typed name is a command, in its short form, its long form, or between. */
static int
name_is(
	const char *name,
	const char *short_name,
	const char *long_name)
{
	size_t length;
	size_t short_length;
	size_t long_length;
	int compare;

	/* The name must be at least the short form and at most the long one. */
	length = strlen(name);
	short_length = strlen(short_name);
	long_length = strlen(long_name);
	if (length < short_length)
		return 0;
	if (length > long_length)
		return 0;

	/* It must start the long form. */
	compare = strncmp(name, long_name, length);
	if (compare != 0)
		return 0;

	/* And it must start with the short form. */
	compare = strncmp(name, short_name, short_length);
	if (compare != 0)
		return 0;

	/* The name is the command. */
	return 1;
}

/* Moves the cursor to the line a number, $ or . names. */
static void
go_to_line(
	struct vi_editor *editor,
	const char *text)
{
	unsigned long number;
	char *end;

	/* Reads the line: $ is the last, . the current, a number counts from one. */
	if (*text == '$') {
		number = (unsigned long)editor->buffer.count;
		end = (char *)text + 1;
	} else if (*text == '.') {
		number = (unsigned long)editor->cursor.line + 1;
		end = (char *)text + 1;
	} else {
		number = strtoul(text, &end, 10);
	}

	/* Only blanks may follow the line. */
	while (*end == ' ' || *end == '\t')
		end++;

	/* Refuses anything else after the line. */
	if (*end != '\0') {
		vi_message(editor, "Unknown command");
		return;
	}

	/* A line past either end goes to that end. */
	if (number == 0)
		number = 1;
	if (number > (unsigned long)editor->buffer.count)
		number = (unsigned long)editor->buffer.count;

	/* Puts the cursor on the line, at its first non-blank character. */
	editor->cursor.line = (size_t)number - 1;
	vi_cursor_first_nonblank(editor);
}

/* Leaves the editor, unless changes would be lost and the command was not forced. */
static void
quit(
	struct vi_editor *editor,
	int force)
{
	int modified;

	/* Unwritten changes stop a plain :q. */
	modified = vi_undo_modified(editor);
	if (modified && !force) {
		vi_message(editor, "No write since last change (add ! to override)");
		return;
	}

	/* Leaves: nothing is lost, or the loss was asked for. */
	editor->quit = 1;
}

/* Writes the buffer and leaves (:wq), or writes only when it changed and leaves (:x). */
static void
write_and_quit(
	struct vi_editor *editor,
	const char *argument,
	int force,
	int only_if_modified)
{
	const char *path;
	int modified;
	int error;

	/* Writes to the named file, or to the buffer's own when none is named. */
	path = NULL;
	if (argument[0] != '\0')
		path = argument;

	/* :x on an unchanged buffer, with no other file named, leaves without writing. */
	modified = vi_undo_modified(editor);
	if (only_if_modified && path == NULL) {
		if (!modified) {
			editor->quit = 1;
			return;
		}
	}

	/* Writes, and leaves only when the write succeeded. */
	error = vi_file_write(editor, path, force);
	if (error != 0)
		return;

	/* Leaves: the buffer is written. */
	editor->quit = 1;
}

/* Edits another file, or the same one read again, in place of the buffer. */
static void
edit(
	struct vi_editor *editor,
	const char *argument,
	int force)
{
	const char *path;
	char *name;
	int modified;
	int error;

	/* Unwritten changes stop a plain :e. */
	modified = vi_undo_modified(editor);
	if (modified && !force) {
		vi_message(editor, "No write since last change (add ! to override)");
		return;
	}

	/* Without a name, the buffer's own file is read again. */
	path = argument;
	if (argument[0] == '\0')
		path = editor->file_name;

	/* A buffer that has no file has nothing to read again. */
	if (path == NULL) {
		vi_message(editor, "No file name");
		return;
	}

	/* Keeps a copy of the new name before the buffer changes. */
	name = strdup(path);
	if (name == NULL) {
		vi_message(editor, "Out of memory");
		return;
	}

	/* Reads the file; a failure leaves the buffer and its name as they were. */
	error = vi_file_load(editor, name);
	if (error != 0) {
		free(name);
		return;
	}

	/* Takes the new name as the buffer's file. */
	free(editor->file_name);
	editor->file_name = name;
}
