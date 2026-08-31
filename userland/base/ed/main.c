/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ed userland command.
 */

#include "userland/base/common/command.h"
#include "userland/base/ed/editor.h"

#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct editor {
	struct ed_buffer buffer;
	struct ed_buffer undo;
	int have_undo;
	int modified;
	int error;
	int verbose;
	const char *prompt;
	char *filename;
};

struct range {
	size_t first;
	size_t last;
	int addressed;
};

struct text_builder {
	char *text;
	size_t length;
	size_t capacity;
};

static void usage(void);
static int read_file_at(struct editor *editor, const char *path, size_t position);
static int editor_loop(struct editor *editor, FILE *stream);
static int execute_command(struct editor *editor, FILE *stream, char *line, int *quit);
static int parse_range(struct editor *editor, const char **cursor, struct range *range);
static int parse_address(struct editor *editor, const char **cursor, size_t *address);
static int editor_snapshot(struct editor *editor);
static void editor_error(struct editor *editor, const char *message);
static int read_input_lines(struct editor *editor, FILE *stream, size_t position);
static int print_range(struct editor *editor, size_t first, size_t last, int numbered);
static int substitute_range(struct editor *editor, struct range range, const char *command, int snapshot);
static char *delimited_copy(const char **cursor, char delimiter);
static int builder_append(struct text_builder *builder, const char *text, size_t length);
static int substitute_line(struct editor *editor, size_t index, regex_t *expression, const char *replacement, int global);
static int append_replacement(struct text_builder *builder, const char *replacement, const char *source, const regmatch_t matches[10]);
static int global_command(struct editor *editor, const char *command, int invert);
static int write_atomic(struct editor *editor, const char *path, size_t first, size_t last);

/*
 * Runs the ed command.
 */
int
main(
	int argc,
	char **argv)
{
	struct editor editor;
	const char *initial;
	int index;
	int status;

	initial = NULL;
	index = 1;

	memset(&editor, 0, sizeof(editor));
	ed_buffer_init(&editor.buffer);
	ed_buffer_init(&editor.undo);

	/* Process each remaining command-line operand. */
	while (index < argc && argv[index][0] == '-') {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		}

		/* Handles the selected command-line operation. */
		if (strcmp(argv[index], "-s") == 0 ||
		    strcmp(argv[index], "-") == 0)
			index++;
		else if (strcmp(argv[index], "-p") == 0 && index + 1 < argc) {
			editor.prompt = argv[index + 1];
			index += 2;
		} else {
			usage();

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (index < argc)
		initial = argv[index++];

	/* Validates the command-line arguments. */
	if (index != argc) {
		usage();

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the initial availability. */
	if (initial != NULL) {
		editor.filename = strdup(initial);

		/* Handles a failed read file at operation. */
		if (editor.filename == NULL ||
		    read_file_at(&editor, initial, 0) != 0) {
			fprintf(stderr, "ed: %s: %s\n", initial,
				strerror(errno));
			ed_buffer_free(&editor.buffer);
			free(editor.filename);

			/* Reports operation failure. */
			return 1;
		}
		editor.modified = 0;
	}
	status = editor_loop(&editor, stdin);
	ed_buffer_free(&editor.buffer);
	ed_buffer_free(&editor.undo);
	free(editor.filename);

	/* Returns the computed result. */
	return status;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr, "usage: ed [-s] [-p string] [file]\n");
}

/* Supports the read file at operation. */
static int
read_file_at(
	struct editor *editor,
	const char *path,
	size_t position)
{
	FILE *stream;
	char *line;
	size_t capacity;
	long length;
	int status;

	stream = fopen(path, "r");
	line = NULL;
	capacity = 0;
	status = 0;

	/* Handles the stream availability. */
	if (stream == NULL)
		return -1;

	/* Process each remaining element. */
	while ((length = command_read_line(stream, &line, &capacity)) > 0) {
		/* Handles the line condition. */
		if (line[length - 1] == '\n')
			line[--length] = '\0';

		/* Handles a failed ed buffer insert operation. */
		if (ed_buffer_insert(&editor->buffer, position++, line) != 0) {
			status = -1;
			break;
		}
	}

	/* Handles an operation failure. */
	if (length < 0 || ferror(stream) || fclose(stream) != 0)
		status = -1;
	free(line);

	/* Returns the computed result. */
	return status;
}

/* Supports the editor loop operation. */
static int
editor_loop(
	struct editor *editor,
	FILE *stream)
{
	char *line;
	size_t capacity;
	long length;
	int quit;

	line = NULL;
	capacity = 0;
	quit = 0;

	/* Continue while the operation condition remains true. */
	while (!quit) {
		/* Handles the prompt availability. */
		if (editor->prompt != NULL) {
			fputs(editor->prompt, stdout);
			fflush(stdout);
		}
		length = command_read_line(stream, &line, &capacity);

		/* Checks the current data length. */
		if (length <= 0)
			break;

		/* Handles the line condition. */
		if (line[length - 1] == '\n')
			line[--length] = '\0';

		/* Handles a failed execute command operation. */
		if (execute_command(editor, stream, line, &quit) != 0)
			editor_error(editor, strerror(errno));
	}
	free(line);

	/* Checks the current data length. */
	if (length < 0)
		editor_error(editor, strerror(errno));

	/* Returns the computed result. */
	return editor->error ? 1 : 0;
}

/* Supports the execute command operation. */
static int
execute_command(
	struct editor *editor,
	FILE *stream,
	char *line,
	int *quit)
{
	int function_result;
	size_t position_local;
	size_t position_local1;
	const char *path_local;
	size_t position_local2;
	const char *path_local3;
	struct ed_buffer previous;
	size_t first;
	size_t last;
	char *name;
	const char *cursor;
	struct range range;
	char command;

	cursor = line;

	/* Handles a failed parse range operation. */
	if (parse_range(editor, &cursor, &range) != 0)
		return -1;

	/* Continue while the operation condition remains true. */
	command = *cursor == '\0' ? 'p' : *cursor++;
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;

	/* Handles a failed strchr operation. */
	if (!range.addressed && editor->buffer.current == SIZE_MAX &&
	    strchr("aiQqHhfrweE", command) == NULL)

		/* Reports operation failure. */
		return -1;

	/* Dispatch the selected operation case. */
	switch (command) {
	case 'a':
				position_local = editor->buffer.current == SIZE_MAX ? 0 : range.last + 1;

	/* Handles a failed editor snapshot operation. */
	if (editor_snapshot(editor) != 0 ||
		    read_input_lines(editor, stream, position_local) != 0)

		/* Reports operation failure. */
		return -1;
	editor->modified = 1;

	/* Reports successful completion. */
	return 0;
	case 'i':
				position_local1 = editor->buffer.current == SIZE_MAX ? 0 : range.first;

	/* Handles a failed editor snapshot operation. */
	if (editor_snapshot(editor) != 0 ||
		    read_input_lines(editor, stream, position_local1) != 0)

		/* Reports operation failure. */
		return -1;
	editor->modified = 1;

	/* Reports successful completion. */
	return 0;
	case 'c':
		/* Handles a failed editor snapshot operation. */
		if (editor_snapshot(editor) != 0 ||
		    ed_buffer_delete(&editor->buffer, range.first,
				     range.last) != 0 ||
		    read_input_lines(editor, stream, range.first) != 0)

			/* Reports operation failure. */
			return -1;
		editor->modified = 1;

		/* Reports successful completion. */
		return 0;
	case 'd':
		/* Handles a failed editor snapshot operation. */
		if (editor_snapshot(editor) != 0 ||
		    ed_buffer_delete(&editor->buffer, range.first,
				     range.last) != 0)

			/* Reports operation failure. */
			return -1;
		editor->modified = 1;

		/* Reports successful completion. */
		return 0;
	case 'p':
		/* Obtains the print range result. */
		function_result = print_range(editor, range.first, range.last, 0);

		/* Returns the computed result. */
		return function_result;
	case 'n':
		/* Obtains the print range result. */
		function_result = print_range(editor, range.first, range.last, 1);

		/* Returns the computed result. */
		return function_result;
	case '=':
		/* Handles the range condition. */
		if (range.addressed)
			printf("%zu\n", range.last + 1);
		else
			printf("%zu\n", editor->buffer.count);

		/* Computes the function result. */
		function_result = ferror(stdout) ? -1 : 0;

		/* Returns the computed result. */
		return function_result;
	case 's':
		/* Obtains the substitute range result. */
		function_result = substitute_range(editor, range, cursor, 1);

		/* Returns the computed result. */
		return function_result;
	case 'g':
		/* Obtains the global command result. */
		function_result = global_command(editor, cursor, 0);

		/* Returns the computed result. */
		return function_result;
	case 'v':
		/* Obtains the global command result. */
		function_result = global_command(editor, cursor, 1);

		/* Returns the computed result. */
		return function_result;
	case 'u':

	/* Handles the editor condition. */
	if (!editor->have_undo)
		return -1;
	ed_buffer_init(&previous);

	/* Handles a failed ed buffer copy operation. */
	if (ed_buffer_copy(&previous, &editor->buffer) != 0)
		return -1;
	ed_buffer_move(&editor->buffer, &editor->undo);
	ed_buffer_move(&editor->undo, &previous);
	editor->modified = 1;

	/* Reports successful completion. */
	return 0;
	case 'r':
				path_local = *cursor == '\0' ? editor->filename : cursor;
				position_local2 = editor->buffer.current == SIZE_MAX ? 0 : range.last + 1;

		/* Handles a failed editor snapshot operation. */
		if (path_local == NULL || editor_snapshot(editor) != 0 ||
		    read_file_at(editor, path_local, position_local2) != 0)

		/* Reports operation failure. */
		return -1;
	editor->modified = 1;

	/* Reports successful completion. */
	return 0;
	case 'w':
				path_local3 = *cursor == '\0' ? editor->filename : cursor;
			first = range.addressed ? range.first : 0;
			last = range.addressed ? range.last : editor->buffer.count - 1;

		/* Handles a failed write atomic operation. */
		if (path_local3 == NULL || editor->buffer.count == 0 ||
		    write_atomic(editor, path_local3, first, last) != 0)

		/* Reports operation failure. */
		return -1;

	/* Handles the range condition. */
	if (!range.addressed) {
		free(editor->filename);
			editor->filename = strdup(path_local3);

		/* Handles the filename availability. */
		if (editor->filename == NULL)
			return -1;
		editor->modified = 0;
	}

	/* Reports successful completion. */
	return 0;
	case 'f':
		/* Checks the current cursor position. */
		if (*cursor != '\0') {
						name = strdup(cursor);

			/* Handles the name availability. */
			if (name == NULL)
				return -1;
			free(editor->filename);
			editor->filename = name;
		}

		/* Handles the filename availability. */
		if (editor->filename == NULL)
			return -1;
		puts(editor->filename);

		/* Computes the function result. */
		function_result = ferror(stdout) ? -1 : 0;

		/* Returns the computed result. */
		return function_result;
	case 'H':
		editor->verbose = !editor->verbose;

		/* Reports successful completion. */
		return 0;
	case 'h':
		/* Reports successful completion. */
		return 0;
	case 'q':
		/* Handles the editor condition. */
		if (editor->modified)
			return -1;
		*quit = 1;
		/* Reports successful completion. */
		return 0;
	case 'Q':
		*quit = 1;
		/* Reports successful completion. */
		return 0;
	default:
		/* Reports operation failure. */
		return -1;
	}
}

/* Supports the parse range operation. */
static int
parse_range(
	struct editor *editor,
	const char **cursor,
	struct range *range)
{
	int second;
	const char *text;
	int first;

	text = *cursor;

	memset(range, 0, sizeof(*range));

	/* Validates the current text. */
	if (*text == '%') {
		/* Handles the editor condition. */
		if (editor->buffer.count == 0)
			return -1;
		range->first = 0;
		range->last = editor->buffer.count - 1;
		range->addressed = 1;
		*cursor = text + 1;
		/* Reports successful completion. */
		return 0;
	}
	first = parse_address(editor, &text, &range->first);

	/* Handles the first condition. */
	if (first < 0)
		return -1;

	/* Handles the first condition. */
	if (first == 0) {
		range->first = editor->buffer.current;
		range->last = editor->buffer.current;
		range->addressed = 0;
		*cursor = text;
		/* Reports successful completion. */
		return 0;
	}
	range->last = range->first;
	range->addressed = 1;

	/* Validates the current text. */
	if (*text == ',' || *text == ';') {

		text++;
		second = parse_address(editor, &text, &range->last);

		/* Handles the second condition. */
		if (second <= 0)
			return -1;

		/* Handles the range condition. */
		if (range->first > range->last)
			return -1;
	}
	*cursor = text;
	/* Reports successful completion. */
	return 0;
}

/* Supports the parse address operation. */
static int
parse_address(
	struct editor *editor,
	const char **cursor,
	size_t *address)
{
	unsigned digit;
	const char *text;
	unsigned long long value;

	text = *cursor;
	value = 0;

	/* Validates the current text. */
	if (*text == '.') {
		/* Handles the editor condition. */
		if (editor->buffer.current == SIZE_MAX)
			return -1;
		*address = editor->buffer.current;
		*cursor = text + 1;
		/* Reports operation failure. */
		return 1;
	}

	/* Validates the current text. */
	if (*text == '$') {
		/* Handles the editor condition. */
		if (editor->buffer.count == 0)
			return -1;
		*address = editor->buffer.count - 1;
		*cursor = text + 1;
		/* Reports operation failure. */
		return 1;
	}

	/* Validates the current text. */
	if (*text < '0' || *text > '9')
		return 0;

	/* Continue while the operation condition remains true. */
	while (*text >= '0' && *text <= '9') {

		digit = (unsigned)(*text - '0');

		/* Validates the current value. */
		if (value > (SIZE_MAX - digit) / 10U)
			return -1;
		value = value * 10U + digit;
		text++;
	}

	/* Validates the current value. */
	if (value == 0 || value > editor->buffer.count)
		return -1;
	*address = (size_t)value - 1;
	*cursor = text;
	/* Reports operation failure. */
	return 1;
}

/* Supports the editor snapshot operation. */
static int
editor_snapshot(
	struct editor *editor)
{
	struct ed_buffer snapshot;

	ed_buffer_init(&snapshot);

	/* Handles a failed ed buffer copy operation. */
	if (ed_buffer_copy(&snapshot, &editor->buffer) != 0) {
		editor_error(editor, "out of memory");

		/* Reports operation failure. */
		return -1;
	}
	ed_buffer_move(&editor->undo, &snapshot);
	editor->have_undo = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the editor error operation. */
static void
editor_error(
	struct editor *editor,
	const char *message)
{
	puts("?");

	/* Handles the message availability. */
	if (editor->verbose && message != NULL)
		fprintf(stderr, "ed: %s\n", message);
	editor->error = 1;
}

/* Supports the read input lines operation. */
static int
read_input_lines(
	struct editor *editor,
	FILE *stream,
	size_t position)
{
	char *line;
	size_t capacity;
	long length;

	line = NULL;
	capacity = 0;

	/* Process each remaining element. */
	while ((length = command_read_line(stream, &line, &capacity)) > 0) {
		/* Checks the current data length. */
		if (length != 0 && line[length - 1] == '\n')
			line[--length] = '\0';

		/* Selects the matching value. */
		if (strcmp(line, ".") == 0)
			break;

		/* Handles a failed ed buffer insert operation. */
		if (ed_buffer_insert(&editor->buffer, position++, line) != 0) {
			free(line);

			/* Reports operation failure. */
			return -1;
		}
	}
	free(line);

	/* Returns the computed result. */
	return length < 0 ? -1 : 0;
}

/* Supports the print range operation. */
static int
print_range(
	struct editor *editor,
	size_t first,
	size_t last,
	int numbered)
{
	size_t index;

	/* Handles the editor condition. */
	if (editor->buffer.count == 0 || first > last ||
	    last >= editor->buffer.count)

		/* Reports operation failure. */
		return -1;

	/* Process each remaining element. */
	for (index = first; index <= last; index++) {
		/* Handles a failed printf operation. */
		if (numbered && printf("%zu\t", index + 1) < 0)
			return -1;

		/* Handles the end-of-file condition. */
		if (puts(editor->buffer.line[index]) == EOF)
			return -1;
	}
	editor->buffer.current = last;

	/* Reports successful completion. */
	return 0;
}

/* Supports the substitute range operation. */
static int
substitute_range(
	struct editor *editor,
	struct range range,
	const char *command,
	int snapshot)
{
	char delimiter;
	char *pattern;
	char *replacement;
	regex_t expression;
	int global;
	int print;
	int status;
	size_t index;
	int changed;

	delimiter = *command++;
	changed = 0;

	/* Handles the delimiter condition. */
	if (delimiter == '\0' || delimiter == '\\' || delimiter == '\n')
		return -1;
	pattern = delimited_copy(&command, delimiter);
	replacement =
	    pattern == NULL ? NULL : delimited_copy(&command, delimiter);

	/* Handles the pattern availability. */
	if (pattern == NULL || replacement == NULL) {
		free(pattern);
		free(replacement);

		/* Reports operation failure. */
		return -1;
	}
	global = strchr(command, 'g') != NULL;
	print = strchr(command, 'p') != NULL;
	status = regcomp(&expression, pattern, 0);
	free(pattern);

	/* Checks the operation status. */
	if (status != 0) {
		free(replacement);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed editor snapshot operation. */
	if (snapshot && editor_snapshot(editor) != 0) {
		regfree(&expression);
		free(replacement);

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index = range.first; index <= range.last; index++) {
		status = substitute_line(editor, index, &expression,
					 replacement, global);

		/* Checks the operation status. */
		if (status < 0)
			break;

		/* Checks the operation status. */
		if (status > 0) {
			changed = 1;

			/* Handles a failed print range operation. */
			if (print &&
			    print_range(editor, index, index, 0) != 0) {
				status = -1;
				break;
			}
		}
	}
	regfree(&expression);
	free(replacement);

	/* Checks the operation status. */
	if (status < 0 || !changed)
		return -1;
	editor->modified = 1;
	editor->buffer.current = range.last;

	/* Reports successful completion. */
	return 0;
}

/* Supports the delimited copy operation. */
static char *
delimited_copy(
	const char **cursor,
	char delimiter)
{
	struct text_builder builder = {0};
	const char *text;

	text = *cursor;

	/* Continue while the operation condition remains true. */
	while (*text != '\0' && *text != delimiter) {
		/* Validates the current text. */
		if (*text == '\\' && text[1] != '\0') {
			/* Handles a failed builder append operation. */
			if (builder_append(&builder, text, 2) != 0)
				goto fail;
			text += 2;
		} else {
			/* Handles a failed builder append operation. */
			if (builder_append(&builder, text, 1) != 0)
				goto fail;
			text++;
		}
	}

	/* Validates the current text. */
	if (*text != delimiter)
		goto fail;
	*cursor = text + 1;
	/* Handles the text availability. */
	if (builder.text == NULL)
		builder.text = strdup("");

	/* Returns the computed result. */
	return builder.text;

fail:
	free(builder.text);

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the builder append operation. */
static int
builder_append(
	struct text_builder *builder,
	const char *text,
	size_t length)
{
	size_t capacity;
	size_t needed;
	char *grown;

	/* Checks the current data length. */
	if (length > SIZE_MAX - builder->length - 1) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	needed = builder->length + length + 1;

	/* Handles the needed condition. */
	if (needed > builder->capacity) {
				capacity = builder->capacity == 0 ? 128 : builder->capacity;

		/* Continue while the operation condition remains true. */
		while (capacity < needed) {
			/* Handles the capacity condition. */
			if (capacity > SIZE_MAX / 2) {
				capacity = needed;
				break;
			}
			capacity *= 2;
		}
		grown = realloc(builder->text, capacity);

		/* Handles the grown availability. */
		if (grown == NULL)
			return -1;
		builder->text = grown;
		builder->capacity = capacity;
	}
	memcpy(builder->text + builder->length, text, length);
	builder->length += length;
	builder->text[builder->length] = '\0';

	/* Reports successful completion. */
	return 0;
}

/* Supports the substitute line operation. */
static int
substitute_line(
	struct editor *editor,
	size_t index,
	regex_t *expression,
	const char *replacement,
	int global)
{
	int flags;
	regmatch_t matches[10];
	int status;
	const char *line = editor->buffer.line[index];
	size_t offset = 0;
	struct text_builder output = {0};
	int changed = 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

				flags = offset == 0 ? 0 : REG_NOTBOL;
				status = regexec(expression, line + offset, 10, matches, flags);

		/* Checks the operation status. */
		if (status == REG_NOMATCH)
			break;

		/* Checks the operation status. */
		if (status != 0)
			goto fail;

		/* Handles a failed builder append operation. */
		if (builder_append(&output, line + offset,
				   (size_t)matches[0].rm_so) != 0 ||
		    append_replacement(&output, replacement, line + offset,
				       matches) != 0)
			goto fail;
		offset += (size_t)matches[0].rm_eo;
		changed = 1;

		/* Handles the global condition. */
		if (!global)
			break;

		/* Handles the matches condition. */
		if (matches[0].rm_so == matches[0].rm_eo) {
			/* Handles the line condition. */
			if (line[offset] == '\0')
				break;

			/* Handles a failed builder append operation. */
			if (builder_append(&output, line + offset, 1) != 0)
				goto fail;
			offset++;
		}
	}

	/* Handles the changed condition. */
	if (!changed) {
		free(output.text);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed builder append operation. */
	if (builder_append(&output, line + offset, strlen(line + offset)) != 0)
		goto fail;

	/* Handles a failed ed buffer replace operation. */
	if (ed_buffer_replace(&editor->buffer, index, output.text) != 0)
		goto fail;
	free(output.text);

	/* Reports operation failure. */
	return 1;

fail:
	free(output.text);

	/* Reports operation failure. */
	return -1;
}

/* Supports the append replacement operation. */
static int
append_replacement(
	struct text_builder *builder,
	const char *replacement,
	const char *source,
	const regmatch_t matches[10])
{
	int group;
	const char *cursor;

	cursor = replacement;

	/* Continue while the operation condition remains true. */
	while (*cursor != '\0') {

		group = -1;

		/* Checks the current cursor position. */
		if (*cursor == '&')
			group = 0;
		else if (*cursor == '\\' && cursor[1] >= '0' &&
			 cursor[1] <= '9') {
			group = cursor[1] - '0';
			cursor++;
		} else if (*cursor == '\\' && cursor[1] != '\0') {
			cursor++;

			/* Handles a failed builder append operation. */
			if (builder_append(builder, cursor++, 1) != 0)
				return -1;
			continue;
		}

		/* Handles the group condition. */
		if (group >= 0) {
			/* Handles a failed builder append operation. */
			if (matches[group].rm_so >= 0 &&
			    builder_append(builder,
					   source + matches[group].rm_so,
					   (size_t)(matches[group].rm_eo -
						    matches[group].rm_so)) != 0)

				/* Reports operation failure. */
				return -1;
			cursor++;
		} else if (builder_append(builder, cursor++, 1) != 0)

			/* Reports operation failure. */
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the global command operation. */
static int
global_command(
	struct editor *editor,
	const char *command,
	int invert)
{
	int matched;
	char delimiter;
	char *pattern;
	regex_t expression;
	size_t *selected;
	size_t count;
	size_t index;
	int status;
	struct range range;

	delimiter = *command++;
	count = 0;

	/* Handles the delimiter condition. */
	if (delimiter == '\0')
		return -1;
	pattern = delimited_copy(&command, delimiter);

	/* Handles the pattern availability. */
	if (pattern == NULL)
		return -1;
	status = regcomp(&expression, pattern, 0);
	free(pattern);

	/* Checks the operation status. */
	if (status != 0)
		return -1;
	selected = calloc(editor->buffer.count, sizeof(*selected));

	/* Handles the selected availability. */
	if (selected == NULL && editor->buffer.count != 0) {
		regfree(&expression);

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index = 0; index < editor->buffer.count; index++) {
				matched = regexec(&expression, editor->buffer.line[index],
				      0, NULL, 0) == 0;

		/* Handles the matched condition. */
		if (matched != invert)
			selected[count++] = index;
	}
	regfree(&expression);

	/* Handles the command condition. */
	if (*command == 's') {
		/* Handles a failed editor snapshot operation. */
		if (editor_snapshot(editor) != 0) {
			free(selected);

			/* Reports operation failure. */
			return -1;
		}

		/* Process each remaining element. */
		for (index = 0; index < count; index++) {
			range.first = selected[index];
			range.last = selected[index];
		range.addressed = 1;

			/* Handles a failed substitute range operation. */
			if (substitute_range(editor, range, command + 1, 0) !=
			    0) {
				free(selected);

				/* Reports operation failure. */
				return -1;
			}
		}
	} else if (strcmp(command, "p") == 0 || *command == '\0') {
		/* Process each remaining element. */
		for (index = 0; index < count; index++)

			/* Handles a failed print range operation. */
			if (print_range(editor, selected[index],
					selected[index], 0) != 0) {
				free(selected);

				/* Reports operation failure. */
				return -1;
			}
	} else if (strcmp(command, "d") == 0) {
		/* Handles a failed editor snapshot operation. */
		if (editor_snapshot(editor) != 0) {
			free(selected);

			/* Reports operation failure. */
			return -1;
		}

		/* Process each remaining element. */
		index = count;
		while (index != 0) {
			index--;

			/* Handles a failed ed buffer delete operation. */
			if (ed_buffer_delete(&editor->buffer, selected[index],
					     selected[index]) != 0) {
				free(selected);

				/* Reports operation failure. */
				return -1;
			}
		}
		editor->modified = count != 0;
	} else {
		free(selected);

		/* Reports operation failure. */
		return -1;
	}
	free(selected);

	/* Reports successful completion. */
	return 0;
}

/* Supports the write atomic operation. */
static int
write_atomic(
	struct editor *editor,
	const char *path,
	size_t first,
	size_t last)
{
	size_t path_length;
	char *temporary;
	int descriptor;
	size_t index;
	int status;

	path_length = strlen(path);
	status = -1;

	/* Handles the path length condition. */
	if (path_length > SIZE_MAX - 16) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	temporary = malloc(path_length + 16);

	/* Handles the temporary availability. */
	if (temporary == NULL)
		return -1;
	memcpy(temporary, path, path_length);
	strcpy(temporary + path_length, ".ed.XXXXXX");
	descriptor = mkstemp(temporary);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		goto done;

	/* Process each remaining element. */
	for (index = first; index <= last; index++) {
		/* Handles a failed command write all operation. */
		if (command_write_all(descriptor, editor->buffer.line[index],
				      strlen(editor->buffer.line[index])) !=
			0 ||
		    command_write_all(descriptor, "\n", 1) != 0)
			goto close_file;
	}

	/* Handles a failed fsync operation. */
	if (fsync(descriptor) != 0)
		goto close_file;

	/* Handles a failed close operation. */
	if (close(descriptor) != 0) {
		descriptor = -1;
		goto close_file;
	}
	descriptor = -1;

	/* Handles a failed rename operation. */
	if (rename(temporary, path) != 0)
		goto close_file;
	status = 0;

close_file:

	/* Checks the file descriptor. */
	if (descriptor >= 0)
		(void)close(descriptor);

	/* Checks the operation status. */
	if (status != 0)
		(void)unlink(temporary);
done:
	free(temporary);

	/* Returns the computed result. */
	return status;
}
