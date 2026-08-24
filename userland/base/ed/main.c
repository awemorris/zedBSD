/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static void
editor_error(struct editor *editor, const char *message)
{
	puts("?");
	if (editor->verbose && message != NULL)
		fprintf(stderr, "ed: %s\n", message);
	editor->error = 1;
}

static int
editor_snapshot(struct editor *editor)
{
	struct ed_buffer snapshot;

	ed_buffer_init(&snapshot);
	if (ed_buffer_copy(&snapshot, &editor->buffer) != 0) {
		editor_error(editor, "out of memory");
		return -1;
	}
	ed_buffer_move(&editor->undo, &snapshot);
	editor->have_undo = 1;
	return 0;
}

static int
parse_address(struct editor *editor, const char **cursor, size_t *address)
{
	const char *text = *cursor;
	unsigned long long value = 0;

	if (*text == '.') {
		if (editor->buffer.current == SIZE_MAX)
			return -1;
		*address = editor->buffer.current;
		*cursor = text + 1;
		return 1;
	}
	if (*text == '$') {
		if (editor->buffer.count == 0)
			return -1;
		*address = editor->buffer.count - 1;
		*cursor = text + 1;
		return 1;
	}
	if (*text < '0' || *text > '9')
		return 0;
	while (*text >= '0' && *text <= '9') {
		unsigned digit = (unsigned)(*text - '0');

		if (value > (SIZE_MAX - digit) / 10U)
			return -1;
		value = value * 10U + digit;
		text++;
	}
	if (value == 0 || value > editor->buffer.count)
		return -1;
	*address = (size_t)value - 1;
	*cursor = text;
	return 1;
}

static int
parse_range(struct editor *editor, const char **cursor, struct range *range)
{
	const char *text = *cursor;
	int first;

	memset(range, 0, sizeof(*range));
	if (*text == '%') {
		if (editor->buffer.count == 0)
			return -1;
		range->first = 0;
		range->last = editor->buffer.count - 1;
		range->addressed = 1;
		*cursor = text + 1;
		return 0;
	}
	first = parse_address(editor, &text, &range->first);
	if (first < 0)
		return -1;
	if (first == 0) {
		range->first = editor->buffer.current;
		range->last = editor->buffer.current;
		range->addressed = 0;
		*cursor = text;
		return 0;
	}
	range->last = range->first;
	range->addressed = 1;
	if (*text == ',' || *text == ';') {
		int second;

		text++;
		second = parse_address(editor, &text, &range->last);
		if (second <= 0)
			return -1;
		if (range->first > range->last)
			return -1;
	}
	*cursor = text;
	return 0;
}

static int
print_range(struct editor *editor, size_t first, size_t last, int numbered)
{
	size_t index;

	if (editor->buffer.count == 0 || first > last ||
	    last >= editor->buffer.count)
		return -1;
	for (index = first; index <= last; index++) {
		if (numbered && printf("%zu\t", index + 1) < 0)
			return -1;
		if (puts(editor->buffer.line[index]) == EOF)
			return -1;
	}
	editor->buffer.current = last;
	return 0;
}

static int
read_input_lines(struct editor *editor, FILE *stream, size_t position)
{
	char *line = NULL;
	size_t capacity = 0;
	long length;

	while ((length = command_read_line(stream, &line, &capacity)) > 0) {
		if (length != 0 && line[length - 1] == '\n')
			line[--length] = '\0';
		if (strcmp(line, ".") == 0)
			break;
		if (ed_buffer_insert(&editor->buffer, position++, line) != 0) {
			free(line);
			return -1;
		}
	}
	free(line);
	return length < 0 ? -1 : 0;
}

static int
read_file_at(struct editor *editor, const char *path, size_t position)
{
	FILE *stream = fopen(path, "r");
	char *line = NULL;
	size_t capacity = 0;
	long length;
	int status = 0;

	if (stream == NULL)
		return -1;
	while ((length = command_read_line(stream, &line, &capacity)) > 0) {
		if (line[length - 1] == '\n')
			line[--length] = '\0';
		if (ed_buffer_insert(&editor->buffer, position++, line) != 0) {
			status = -1;
			break;
		}
	}
	if (length < 0 || ferror(stream) || fclose(stream) != 0)
		status = -1;
	free(line);
	return status;
}

static int
write_atomic(struct editor *editor, const char *path, size_t first, size_t last)
{
	size_t path_length = strlen(path);
	char *temporary;
	int descriptor;
	size_t index;
	int status = -1;

	if (path_length > SIZE_MAX - 16) {
		errno = EOVERFLOW;
		return -1;
	}
	temporary = malloc(path_length + 16);
	if (temporary == NULL)
		return -1;
	memcpy(temporary, path, path_length);
	strcpy(temporary + path_length, ".ed.XXXXXX");
	descriptor = mkstemp(temporary);
	if (descriptor < 0)
		goto done;
	for (index = first; index <= last; index++) {
		if (command_write_all(descriptor, editor->buffer.line[index],
				      strlen(editor->buffer.line[index])) !=
			0 ||
		    command_write_all(descriptor, "\n", 1) != 0)
			goto close_file;
	}
	if (fsync(descriptor) != 0)
		goto close_file;
	if (close(descriptor) != 0) {
		descriptor = -1;
		goto close_file;
	}
	descriptor = -1;
	if (rename(temporary, path) != 0)
		goto close_file;
	status = 0;

close_file:
	if (descriptor >= 0)
		(void)close(descriptor);
	if (status != 0)
		(void)unlink(temporary);
done:
	free(temporary);
	return status;
}

static int
builder_append(struct text_builder *builder, const char *text, size_t length)
{
	size_t needed;
	char *grown;

	if (length > SIZE_MAX - builder->length - 1) {
		errno = EOVERFLOW;
		return -1;
	}
	needed = builder->length + length + 1;
	if (needed > builder->capacity) {
		size_t capacity =
		    builder->capacity == 0 ? 128 : builder->capacity;

		while (capacity < needed) {
			if (capacity > SIZE_MAX / 2) {
				capacity = needed;
				break;
			}
			capacity *= 2;
		}
		grown = realloc(builder->text, capacity);
		if (grown == NULL)
			return -1;
		builder->text = grown;
		builder->capacity = capacity;
	}
	memcpy(builder->text + builder->length, text, length);
	builder->length += length;
	builder->text[builder->length] = '\0';
	return 0;
}

static char *
delimited_copy(const char **cursor, char delimiter)
{
	struct text_builder builder = {0};
	const char *text = *cursor;

	while (*text != '\0' && *text != delimiter) {
		if (*text == '\\' && text[1] != '\0') {
			if (builder_append(&builder, text, 2) != 0)
				goto fail;
			text += 2;
		} else {
			if (builder_append(&builder, text, 1) != 0)
				goto fail;
			text++;
		}
	}
	if (*text != delimiter)
		goto fail;
	*cursor = text + 1;
	if (builder.text == NULL)
		builder.text = strdup("");
	return builder.text;

fail:
	free(builder.text);
	return NULL;
}

static int
append_replacement(struct text_builder *builder, const char *replacement,
		   const char *source, const regmatch_t matches[10])
{
	const char *cursor = replacement;

	while (*cursor != '\0') {
		int group = -1;

		if (*cursor == '&')
			group = 0;
		else if (*cursor == '\\' && cursor[1] >= '0' &&
			 cursor[1] <= '9') {
			group = cursor[1] - '0';
			cursor++;
		} else if (*cursor == '\\' && cursor[1] != '\0') {
			cursor++;
			if (builder_append(builder, cursor++, 1) != 0)
				return -1;
			continue;
		}
		if (group >= 0) {
			if (matches[group].rm_so >= 0 &&
			    builder_append(builder,
					   source + matches[group].rm_so,
					   (size_t)(matches[group].rm_eo -
						    matches[group].rm_so)) != 0)
				return -1;
			cursor++;
		} else if (builder_append(builder, cursor++, 1) != 0)
			return -1;
	}
	return 0;
}

static int
substitute_line(struct editor *editor, size_t index, regex_t *expression,
		const char *replacement, int global)
{
	const char *line = editor->buffer.line[index];
	size_t offset = 0;
	struct text_builder output = {0};
	int changed = 0;

	for (;;) {
		regmatch_t matches[10];
		int flags = offset == 0 ? 0 : REG_NOTBOL;
		int status =
		    regexec(expression, line + offset, 10, matches, flags);

		if (status == REG_NOMATCH)
			break;
		if (status != 0)
			goto fail;
		if (builder_append(&output, line + offset,
				   (size_t)matches[0].rm_so) != 0 ||
		    append_replacement(&output, replacement, line + offset,
				       matches) != 0)
			goto fail;
		offset += (size_t)matches[0].rm_eo;
		changed = 1;
		if (!global)
			break;
		if (matches[0].rm_so == matches[0].rm_eo) {
			if (line[offset] == '\0')
				break;
			if (builder_append(&output, line + offset, 1) != 0)
				goto fail;
			offset++;
		}
	}
	if (!changed) {
		free(output.text);
		return 0;
	}
	if (builder_append(&output, line + offset, strlen(line + offset)) != 0)
		goto fail;
	if (ed_buffer_replace(&editor->buffer, index, output.text) != 0)
		goto fail;
	free(output.text);
	return 1;

fail:
	free(output.text);
	return -1;
}

static int
substitute_range(struct editor *editor, struct range range, const char *command,
		 int snapshot)
{
	char delimiter = *command++;
	char *pattern;
	char *replacement;
	regex_t expression;
	int global;
	int print;
	int status;
	size_t index;
	int changed = 0;

	if (delimiter == '\0' || delimiter == '\\' || delimiter == '\n')
		return -1;
	pattern = delimited_copy(&command, delimiter);
	replacement =
	    pattern == NULL ? NULL : delimited_copy(&command, delimiter);
	if (pattern == NULL || replacement == NULL) {
		free(pattern);
		free(replacement);
		return -1;
	}
	global = strchr(command, 'g') != NULL;
	print = strchr(command, 'p') != NULL;
	status = regcomp(&expression, pattern, 0);
	free(pattern);
	if (status != 0) {
		free(replacement);
		return -1;
	}
	if (snapshot && editor_snapshot(editor) != 0) {
		regfree(&expression);
		free(replacement);
		return -1;
	}
	for (index = range.first; index <= range.last; index++) {
		status = substitute_line(editor, index, &expression,
					 replacement, global);
		if (status < 0)
			break;
		if (status > 0) {
			changed = 1;
			if (print &&
			    print_range(editor, index, index, 0) != 0) {
				status = -1;
				break;
			}
		}
	}
	regfree(&expression);
	free(replacement);
	if (status < 0 || !changed)
		return -1;
	editor->modified = 1;
	editor->buffer.current = range.last;
	return 0;
}

static int
global_command(struct editor *editor, const char *command, int invert)
{
	char delimiter = *command++;
	char *pattern;
	regex_t expression;
	size_t *selected;
	size_t count = 0;
	size_t index;
	int status;

	if (delimiter == '\0')
		return -1;
	pattern = delimited_copy(&command, delimiter);
	if (pattern == NULL)
		return -1;
	status = regcomp(&expression, pattern, 0);
	free(pattern);
	if (status != 0)
		return -1;
	selected = calloc(editor->buffer.count, sizeof(*selected));
	if (selected == NULL && editor->buffer.count != 0) {
		regfree(&expression);
		return -1;
	}
	for (index = 0; index < editor->buffer.count; index++) {
		int matched = regexec(&expression, editor->buffer.line[index],
				      0, NULL, 0) == 0;

		if (matched != invert)
			selected[count++] = index;
	}
	regfree(&expression);
	if (*command == 's') {
		if (editor_snapshot(editor) != 0) {
			free(selected);
			return -1;
		}
		for (index = 0; index < count; index++) {
			struct range range = {selected[index], selected[index],
					      1};

			if (substitute_range(editor, range, command + 1, 0) !=
			    0) {
				free(selected);
				return -1;
			}
		}
	} else if (strcmp(command, "p") == 0 || *command == '\0') {
		for (index = 0; index < count; index++)
			if (print_range(editor, selected[index],
					selected[index], 0) != 0) {
				free(selected);
				return -1;
			}
	} else if (strcmp(command, "d") == 0) {
		if (editor_snapshot(editor) != 0) {
			free(selected);
			return -1;
		}
		index = count;
		while (index != 0) {
			index--;
			if (ed_buffer_delete(&editor->buffer, selected[index],
					     selected[index]) != 0) {
				free(selected);
				return -1;
			}
		}
		editor->modified = count != 0;
	} else {
		free(selected);
		return -1;
	}
	free(selected);
	return 0;
}

static int
execute_command(struct editor *editor, FILE *stream, char *line, int *quit)
{
	const char *cursor = line;
	struct range range;
	char command;

	if (parse_range(editor, &cursor, &range) != 0)
		return -1;
	command = *cursor == '\0' ? 'p' : *cursor++;
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	if (!range.addressed && editor->buffer.current == SIZE_MAX &&
	    strchr("aiQqHhfrweE", command) == NULL)
		return -1;
	switch (command) {
	case 'a': {
		size_t position =
		    editor->buffer.current == SIZE_MAX ? 0 : range.last + 1;

		if (editor_snapshot(editor) != 0 ||
		    read_input_lines(editor, stream, position) != 0)
			return -1;
		editor->modified = 1;
		return 0;
	}
	case 'i': {
		size_t position =
		    editor->buffer.current == SIZE_MAX ? 0 : range.first;

		if (editor_snapshot(editor) != 0 ||
		    read_input_lines(editor, stream, position) != 0)
			return -1;
		editor->modified = 1;
		return 0;
	}
	case 'c':
		if (editor_snapshot(editor) != 0 ||
		    ed_buffer_delete(&editor->buffer, range.first,
				     range.last) != 0 ||
		    read_input_lines(editor, stream, range.first) != 0)
			return -1;
		editor->modified = 1;
		return 0;
	case 'd':
		if (editor_snapshot(editor) != 0 ||
		    ed_buffer_delete(&editor->buffer, range.first,
				     range.last) != 0)
			return -1;
		editor->modified = 1;
		return 0;
	case 'p':
		return print_range(editor, range.first, range.last, 0);
	case 'n':
		return print_range(editor, range.first, range.last, 1);
	case '=':
		if (range.addressed)
			printf("%zu\n", range.last + 1);
		else
			printf("%zu\n", editor->buffer.count);
		return ferror(stdout) ? -1 : 0;
	case 's':
		return substitute_range(editor, range, cursor, 1);
	case 'g':
		return global_command(editor, cursor, 0);
	case 'v':
		return global_command(editor, cursor, 1);
	case 'u': {
		struct ed_buffer previous;

		if (!editor->have_undo)
			return -1;
		ed_buffer_init(&previous);
		if (ed_buffer_copy(&previous, &editor->buffer) != 0)
			return -1;
		ed_buffer_move(&editor->buffer, &editor->undo);
		ed_buffer_move(&editor->undo, &previous);
		editor->modified = 1;
		return 0;
	}
	case 'r': {
		const char *path = *cursor == '\0' ? editor->filename : cursor;
		size_t position =
		    editor->buffer.current == SIZE_MAX ? 0 : range.last + 1;

		if (path == NULL || editor_snapshot(editor) != 0 ||
		    read_file_at(editor, path, position) != 0)
			return -1;
		editor->modified = 1;
		return 0;
	}
	case 'w': {
		const char *path = *cursor == '\0' ? editor->filename : cursor;
		size_t first = range.addressed ? range.first : 0;
		size_t last =
		    range.addressed ? range.last : editor->buffer.count - 1;

		if (path == NULL || editor->buffer.count == 0 ||
		    write_atomic(editor, path, first, last) != 0)
			return -1;
		if (!range.addressed) {
			free(editor->filename);
			editor->filename = strdup(path);
			if (editor->filename == NULL)
				return -1;
			editor->modified = 0;
		}
		return 0;
	}
	case 'f':
		if (*cursor != '\0') {
			char *name = strdup(cursor);

			if (name == NULL)
				return -1;
			free(editor->filename);
			editor->filename = name;
		}
		if (editor->filename == NULL)
			return -1;
		puts(editor->filename);
		return ferror(stdout) ? -1 : 0;
	case 'H':
		editor->verbose = !editor->verbose;
		return 0;
	case 'h':
		return 0;
	case 'q':
		if (editor->modified)
			return -1;
		*quit = 1;
		return 0;
	case 'Q':
		*quit = 1;
		return 0;
	default:
		return -1;
	}
}

static int
editor_loop(struct editor *editor, FILE *stream)
{
	char *line = NULL;
	size_t capacity = 0;
	long length;
	int quit = 0;

	while (!quit) {
		if (editor->prompt != NULL) {
			fputs(editor->prompt, stdout);
			fflush(stdout);
		}
		length = command_read_line(stream, &line, &capacity);
		if (length <= 0)
			break;
		if (line[length - 1] == '\n')
			line[--length] = '\0';
		if (execute_command(editor, stream, line, &quit) != 0)
			editor_error(editor, strerror(errno));
	}
	free(line);
	if (length < 0)
		editor_error(editor, strerror(errno));
	return editor->error ? 1 : 0;
}

static void
usage(void)
{
	fprintf(stderr, "usage: ed [-s] [-p string] [file]\n");
}

int
main(int argc, char **argv)
{
	struct editor editor;
	const char *initial = NULL;
	int index = 1;
	int status;

	memset(&editor, 0, sizeof(editor));
	ed_buffer_init(&editor.buffer);
	ed_buffer_init(&editor.undo);
	while (index < argc && argv[index][0] == '-') {
		if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		}
		if (strcmp(argv[index], "-s") == 0 ||
		    strcmp(argv[index], "-") == 0)
			index++;
		else if (strcmp(argv[index], "-p") == 0 && index + 1 < argc) {
			editor.prompt = argv[index + 1];
			index += 2;
		} else {
			usage();
			return 2;
		}
	}
	if (index < argc)
		initial = argv[index++];
	if (index != argc) {
		usage();
		return 2;
	}
	if (initial != NULL) {
		editor.filename = strdup(initial);
		if (editor.filename == NULL ||
		    read_file_at(&editor, initial, 0) != 0) {
			fprintf(stderr, "ed: %s: %s\n", initial,
				strerror(errno));
			ed_buffer_free(&editor.buffer);
			free(editor.filename);
			return 1;
		}
		editor.modified = 0;
	}
	status = editor_loop(&editor, stdin);
	ed_buffer_free(&editor.buffer);
	ed_buffer_free(&editor.undo);
	free(editor.filename);
	return status;
}
