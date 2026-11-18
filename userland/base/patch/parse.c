/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads the differences of a patch file.
 *
 * A patch may hold normal, context, unified or ed differences for one
 * file or several, among other text that is skipped.  Context and
 * unified differences name their files in header lines ("*** old" and
 * "--- new", or "--- old" and "+++ new"); an "Index:" line may name the
 * file too.  Each hunk becomes the old lines and the new lines it
 * replaces them with, and a note of which lines at its ends are context.
 */

#include "userland/base/patch/patch.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What a line of a patch starts. */
#define LINE_TEXT 0
#define LINE_INDEX 1
#define LINE_UNIFIED_HEADER 2
#define LINE_CONTEXT_HEADER 3
#define LINE_UNIFIED_HUNK 4
#define LINE_CONTEXT_HUNK 5
#define LINE_NORMAL_HUNK 6
#define LINE_ED_COMMAND 7

/*
 * Where the reader is in the patch.
 *
 * files grows as file headers and headerless hunks are met; current is
 * the file the next hunk belongs to, or NULL.
 */
struct patch_reader {
	const struct patch_lines *patch;
	long line;
	int forced;
	struct patch_file *files;
	long count;
	long capacity;
	struct patch_file *current;
	char *pending_index;
};

static int start_file(struct patch_reader *reader, const char *old_name, const char *new_name);
static struct patch_hunk *new_hunk(struct patch_reader *reader, int format);
static int read_unified_hunk(struct patch_reader *reader);
static int read_context_hunk(struct patch_reader *reader);
static int read_normal_hunk(struct patch_reader *reader);
static int read_ed_command(struct patch_reader *reader);
static int parse_range(const char *text, int counted, long *first, long *last, const char **end);
static int line_kind(const char *text, const char *next, int forced);
static int is_normal_command(const char *text);
static int is_ed_command(const char *text);
static const char *skip_range(const char *cursor);
static int starts_with(const char *text, const char *prefix);
static char *header_name(const char *text);
static void count_context(struct patch_hunk *hunk, const char *marks, long count);

/* Adds a copy of a line to lines. */
int
patch_lines_add(
	struct patch_lines *lines,
	const char *text,
	size_t length)
{
	char **grown;
	char *copy;
	long capacity;

	/* Grows the array when it is full. */
	if (lines->count >= lines->capacity) {
		capacity = lines->capacity * 2;
		if (capacity < 64)
			capacity = 64;
		grown = realloc(lines->items, (size_t)capacity * sizeof(*grown));
		if (grown == NULL)
			return -1;
		lines->items = grown;
		lines->capacity = capacity;
	}

	/* Copies the line. */
	copy = malloc(length + 1);
	if (copy == NULL)
		return -1;
	memcpy(copy, text, length);
	copy[length] = '\0';

	/* Succeeded: the line is added. */
	lines->items[lines->count] = copy;
	lines->count++;
	return 0;
}

/* Frees lines. */
void
patch_lines_free(
	struct patch_lines *lines)
{
	long index;

	/* Frees each line and the array. */
	for (index = 0; index < lines->count; index++)
		free(lines->items[index]);
	free(lines->items);
	memset(lines, 0, sizeof(*lines));
}

/*
 * Loads a file as lines.  A path of - reads standard input.  Returns -1
 * with errno set on failure.
 */
int
patch_lines_load(
	const char *path,
	struct patch_lines *lines)
{
	char *data;
	char *grown;
	size_t size;
	size_t capacity;
	size_t start;
	size_t index;
	ssize_t got;
	int descriptor;
	int status;
	int compare;

	/* Opens the file, or takes standard input. */
	memset(lines, 0, sizeof(*lines));
	descriptor = STDIN_FILENO;
	compare = strcmp(path, "-");
	if (compare != 0) {
		descriptor = open(path, O_RDONLY);
		if (descriptor < 0)
			return -1;
	}

	/* Reads all of it. */
	capacity = 65536;
	size = 0;
	data = malloc(capacity);
	if (data == NULL) {
		errno = ENOMEM;
		return -1;
	}

	/* Reads until the end of the file. */
	for (;;) {
		/* Grows the buffer when it is full. */
		if (size == capacity) {
			capacity *= 2;
			grown = realloc(data, capacity);
			if (grown == NULL) {
				free(data);
				errno = ENOMEM;
				return -1;
			}

			/* Uses the larger buffer. */
			data = grown;
		}

		/* Reads what fits; the end of the file ends the loop. */
		got = read(descriptor, data + size, capacity - size);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			free(data);
			return -1;
		}

		/* Nothing more to read. */
		if (got == 0)
			break;
		size += (size_t)got;
	}

	/* Closes a file it opened. */
	if (descriptor != STDIN_FILENO)
		close(descriptor);

	/* Splits it into lines; the last may lack its newline. */
	start = 0;
	for (index = 0; index < size; index++) {
		if (data[index] != '\n')
			continue;
		status = patch_lines_add(lines, data + start, index - start);
		if (status != 0) {
			free(data);
			errno = ENOMEM;
			return -1;
		}

		/* The next line starts after the newline. */
		start = index + 1;
	}

	/* The last line may lack its newline. */
	if (start < size) {
		status = patch_lines_add(lines, data + start, size - start);
		if (status != 0) {
			free(data);
			errno = ENOMEM;
			return -1;
		}

		/* Notes the missing newline. */
		lines->last_unterminated = 1;
	}

	/* Succeeded: the file as lines. */
	free(data);
	return 0;
}

/*
 * Reads every difference of a patch into files.  forced_format, when not
 * PATCH_UNKNOWN, is the kind -c, -e, -n or -u asked for.  Returns -1
 * without memory or for a malformed hunk.
 */
int
patch_read(
	const struct patch_lines *patch,
	int forced_format,
	struct patch_file **files,
	long *count)
{
	struct patch_reader reader;
	const char *text;
	const char *next;
	int status;
	int kind;

	/* Starts at the first line with no file. */
	memset(&reader, 0, sizeof(reader));
	reader.patch = patch;
	reader.forced = forced_format;

	/* Looks at each line for a header or the start of a hunk. */
	while (reader.line < patch->count) {
		text = patch->items[reader.line];
		next = NULL;
		if (reader.line + 1 < patch->count)
			next = patch->items[reader.line + 1];
		kind = line_kind(text, next, reader.forced);

		/* Reads what the line starts; other text is skipped. */
		status = 0;
		switch (kind) {
		case LINE_INDEX:
			free(reader.pending_index);
			reader.pending_index = header_name(text + 7);
			reader.line++;
			break;
		case LINE_UNIFIED_HEADER:
		case LINE_CONTEXT_HEADER:
			status = start_file(&reader, text + 4, next + 4);
			reader.line += 2;
			break;
		case LINE_UNIFIED_HUNK:
			status = read_unified_hunk(&reader);
			break;
		case LINE_CONTEXT_HUNK:
			status = read_context_hunk(&reader);
			break;
		case LINE_NORMAL_HUNK:
			status = read_normal_hunk(&reader);
			break;
		case LINE_ED_COMMAND:
			status = read_ed_command(&reader);
			break;
		default:
			reader.line++;
			break;
		}

		/* A malformed hunk or a lack of memory ends the reading. */
		if (status != 0) {
			free(reader.pending_index);
			patch_files_free(reader.files, reader.count);
			return -1;
		}
	}

	/* Succeeded: the files and their hunks. */
	free(reader.pending_index);
	*files = reader.files;
	*count = reader.count;
	return 0;
}

/* Frees what patch_read() made. */
void
patch_files_free(
	struct patch_file *files,
	long count)
{
	long index;
	long hunk;

	/* Frees each file's names and hunks. */
	for (index = 0; index < count; index++) {
		free(files[index].old_name);
		free(files[index].new_name);
		free(files[index].index_name);
		for (hunk = 0; hunk < files[index].hunk_count; hunk++) {
			patch_lines_free(&files[index].hunks[hunk].old);
			patch_lines_free(&files[index].hunks[hunk].new);
			free(files[index].hunks[hunk].command);
		}

		/* Then the hunk array. */
		free(files[index].hunks);
	}

	/* Then the file array. */
	free(files);
}

/* Starts the differences of a new file with the names of its headers. */
static int
start_file(
	struct patch_reader *reader,
	const char *old_name,
	const char *new_name)
{
	struct patch_file *grown;
	struct patch_file *file;
	long capacity;

	/* Grows the array when it is full. */
	if (reader->count >= reader->capacity) {
		capacity = reader->capacity * 2;
		if (capacity < 8)
			capacity = 8;
		grown = realloc(reader->files, (size_t)capacity * sizeof(*grown));
		if (grown == NULL)
			return -1;
		reader->files = grown;
		reader->capacity = capacity;
	}

	/* The new file, with the Index: name that came before it. */
	file = &reader->files[reader->count];
	memset(file, 0, sizeof(*file));
	reader->count++;
	if (old_name != NULL)
		file->old_name = header_name(old_name);
	if (new_name != NULL)
		file->new_name = header_name(new_name);
	file->index_name = reader->pending_index;
	reader->pending_index = NULL;
	reader->current = file;
	return 0;
}

/*
 * Adds an empty hunk of a kind to the current file, starting a file
 * without names when there is none or when the kind changes.
 */
static struct patch_hunk *
new_hunk(
	struct patch_reader *reader,
	int format)
{
	struct patch_hunk *grown;
	struct patch_hunk *hunk;
	struct patch_file *file;
	long capacity;
	int status;

	/* Hunks without headers belong to a file without names. */
	if (reader->current == NULL || (reader->current->format != PATCH_UNKNOWN && reader->current->format != format)) {
		status = start_file(reader, NULL, NULL);
		if (status != 0)
			return NULL;
	}

	/* The hunk belongs to the current file. */
	file = reader->current;
	file->format = format;

	/* Grows the array when it is full. */
	if (file->hunk_count >= file->hunk_capacity) {
		capacity = file->hunk_capacity * 2;
		if (capacity < 8)
			capacity = 8;
		grown = realloc(file->hunks, (size_t)capacity * sizeof(*grown));
		if (grown == NULL)
			return NULL;
		file->hunks = grown;
		file->hunk_capacity = capacity;
	}

	/* The hunk, empty; its text starts at the current line. */
	hunk = &file->hunks[file->hunk_count];
	memset(hunk, 0, sizeof(*hunk));
	file->hunk_count++;
	return hunk;
}

/* Reads a unified hunk: "@@ -a,b +c,d @@" and its lines. */
static int
read_unified_hunk(
	struct patch_reader *reader)
{
	const struct patch_lines *patch;
	struct patch_hunk *hunk;
	const char *text;
	const char *cursor;
	char marks[4096];
	long old_first;
	long old_last;
	long new_first;
	long new_last;
	long old_count;
	long new_count;
	long old_seen;
	long new_seen;
	long mark_count;
	int status;
	int last_mark;
	long start;

	/* The ranges: first line and count, the count 1 when left out. */
	patch = reader->patch;
	start = reader->line;
	text = patch->items[reader->line];
	status = parse_range(text + 4, 1, &old_first, &old_last, &cursor);
	if (status != 0 || *cursor != ' ' || cursor[1] != '+')
		return -1;
	status = parse_range(cursor + 2, 1, &new_first, &new_last, &cursor);
	if (status != 0)
		return -1;
	old_count = old_last;
	new_count = new_last;
	reader->line++;

	/* The hunk. */
	hunk = new_hunk(reader, PATCH_UNIFIED);
	if (hunk == NULL)
		return -1;
	hunk->raw_first = start;
	hunk->old_start = old_first;
	hunk->new_start = new_first;

	/* Reads lines until both counts are met. */
	old_seen = 0;
	new_seen = 0;
	mark_count = 0;
	last_mark = ' ';
	while (reader->line < patch->count && (old_seen < old_count || new_seen < new_count)) {
		text = patch->items[reader->line];

		/* A note marks the line before it as having no newline. */
		if (text[0] == '\\') {
			if (last_mark != '+')
				hunk->old.last_unterminated = 1;
			if (last_mark != '-')
				hunk->new.last_unterminated = 1;
			reader->line++;
			continue;
		}

		/* Context goes to both sides; an empty line is empty context. */
		if (text[0] == ' ' || text[0] == '\0') {
			cursor = text;
			if (text[0] == ' ')
				cursor = text + 1;
			status = patch_lines_add(&hunk->old, cursor, strlen(cursor));
			if (status == 0)
				status = patch_lines_add(&hunk->new, cursor, strlen(cursor));
			old_seen++;
			new_seen++;
			last_mark = ' ';
		} else if (text[0] == '-') {
			status = patch_lines_add(&hunk->old, text + 1, strlen(text + 1));
			old_seen++;
			last_mark = '-';
		} else if (text[0] == '+') {
			status = patch_lines_add(&hunk->new, text + 1, strlen(text + 1));
			new_seen++;
			last_mark = '+';
		} else {
			/* The hunk ended early: it is malformed. */
			return -1;
		}

		/* Stops on a lack of memory. */
		if (status != 0)
			return -1;

		/* Keeps the marks for the context count. */
		if ((size_t)mark_count < sizeof(marks))
			marks[mark_count] = (char)last_mark;
		mark_count++;
		reader->line++;
	}

	/* A note may follow the last line. */
	if (reader->line < patch->count && patch->items[reader->line][0] == '\\') {
		if (last_mark != '+')
			hunk->old.last_unterminated = 1;
		if (last_mark != '-')
			hunk->new.last_unterminated = 1;
		reader->line++;
	}

	/* The counts must have been met. */
	if (old_seen != old_count || new_seen != new_count)
		return -1;

	/* Succeeded: the context at both ends is counted. */
	if (mark_count > (long)sizeof(marks))
		mark_count = (long)sizeof(marks);
	count_context(hunk, marks, mark_count);
	hunk->raw_end = reader->line;
	return 0;
}

/*
 * Reads a context hunk: "***************", "*** a,b ****" and the old
 * lines, "--- c,d ----" and the new lines.  A side whose lines do not
 * change is left out and made from the other side's context.
 */
static int
read_context_hunk(
	struct patch_reader *reader)
{
	const struct patch_lines *patch;
	struct patch_hunk *hunk;
	struct patch_lines old_marked;
	struct patch_lines new_marked;
	const char *text;
	const char *cursor;
	char marks[4096];
	long old_first;
	long old_last;
	long new_first;
	long new_last;
	long index;
	long mark_count;
	int status;
	int old_unterminated;
	int new_unterminated;
	int matched;
	long start;

	/* The old range. */
	patch = reader->patch;
	start = reader->line;
	reader->line++;
	if (reader->line >= patch->count)
		return -1;
	text = patch->items[reader->line];
	matched = starts_with(text, "*** ");
	if (!matched)
		return -1;
	status = parse_range(text + 4, 0, &old_first, &old_last, &cursor);
	if (status != 0)
		return -1;
	reader->line++;

	/* The old lines, with their marks, up to the new range. */
	memset(&old_marked, 0, sizeof(old_marked));
	memset(&new_marked, 0, sizeof(new_marked));
	old_unterminated = 0;
	new_unterminated = 0;
	while (reader->line < patch->count) {
		text = patch->items[reader->line];
		matched = starts_with(text, "--- ");
		if (matched)
			break;
		if (text[0] == '\\') {
			old_unterminated = 1;
			reader->line++;
			continue;
		}

		/* Each old line is a mark and a blank. */
		if (text[0] == '\0' || text[1] != ' ')
			return -1;
		status = patch_lines_add(&old_marked, text, strlen(text));
		if (status != 0)
			return -1;
		reader->line++;
	}

	/* The new range. */
	if (reader->line >= patch->count)
		return -1;
	text = patch->items[reader->line];
	status = parse_range(text + 4, 0, &new_first, &new_last, &cursor);
	if (status != 0)
		return -1;
	reader->line++;

	/* The new lines, with their marks, up to the next hunk or file. */
	while (reader->line < patch->count) {
		text = patch->items[reader->line];
		if (text[0] == '\\') {
			new_unterminated = 1;
			reader->line++;
			continue;
		}

		/* New lines until something else. */
		if (text[0] == '\0' || text[1] != ' ')
			break;
		if (text[0] != ' ' && text[0] != '+' && text[0] != '!')
			break;
		status = patch_lines_add(&new_marked, text, strlen(text));
		if (status != 0)
			return -1;
		reader->line++;
	}

	/* The hunk. */
	hunk = new_hunk(reader, PATCH_CONTEXT);
	if (hunk == NULL)
		return -1;
	hunk->raw_first = start;
	hunk->raw_end = reader->line;
	hunk->old_start = old_first;
	hunk->new_start = new_first;

	/* The old lines, or the new side's context when the old side is left out. */
	mark_count = 0;
	if (old_marked.count > 0) {
		for (index = 0; index < old_marked.count; index++) {
			status = patch_lines_add(&hunk->old, old_marked.items[index] + 2, strlen(old_marked.items[index] + 2));
			if (status != 0)
				return -1;
			if ((size_t)mark_count < sizeof(marks))
				marks[mark_count] = old_marked.items[index][0];
			mark_count++;
		}
	} else {
		for (index = 0; index < new_marked.count; index++) {
			if (new_marked.items[index][0] != ' ')
				continue;
			status = patch_lines_add(&hunk->old, new_marked.items[index] + 2, strlen(new_marked.items[index] + 2));
			if (status != 0)
				return -1;
		}
	}

	/* The new lines, or the old side's context when the new side is left out. */
	if (new_marked.count > 0) {
		for (index = 0; index < new_marked.count; index++) {
			status = patch_lines_add(&hunk->new, new_marked.items[index] + 2, strlen(new_marked.items[index] + 2));
			if (status != 0)
				return -1;
		}

		/* Marks from the new side when the old side was left out. */
		if (old_marked.count == 0) {
			for (index = 0; index < new_marked.count && (size_t)mark_count < sizeof(marks); index++) {
				marks[mark_count] = new_marked.items[index][0];
				mark_count++;
			}
		}
	} else {
		for (index = 0; index < old_marked.count; index++) {
			if (old_marked.items[index][0] != ' ')
				continue;
			status = patch_lines_add(&hunk->new, old_marked.items[index] + 2, strlen(old_marked.items[index] + 2));
			if (status != 0)
				return -1;
		}
	}

	/*
	 * The missing newlines of each side; a side left out shares the
	 * other's only when its last line is context.
	 */
	hunk->old.last_unterminated = old_unterminated;
	hunk->new.last_unterminated = new_unterminated;
	if (old_marked.count == 0 && new_marked.count > 0 && new_marked.items[new_marked.count - 1][0] == ' ')
		hunk->old.last_unterminated = new_unterminated;
	if (new_marked.count == 0 && old_marked.count > 0 && old_marked.items[old_marked.count - 1][0] == ' ')
		hunk->new.last_unterminated = old_unterminated;

	/* The context at both ends; the old start of an empty side is the line before. */
	if (mark_count > (long)sizeof(marks))
		mark_count = (long)sizeof(marks);
	count_context(hunk, marks, mark_count);
	patch_lines_free(&old_marked);
	patch_lines_free(&new_marked);
	return 0;
}

/*
 * Reads a normal hunk: "NaM", "NdM" or "NcM" with ranges, then "< "
 * lines, "---" for c, and "> " lines.
 */
static int
read_normal_hunk(
	struct patch_reader *reader)
{
	const struct patch_lines *patch;
	struct patch_hunk *hunk;
	const char *text;
	const char *cursor;
	long old_first;
	long old_last;
	long new_first;
	long new_last;
	int letter;
	int status;
	int matched;
	int compare;
	long start;

	/* The command: an old range, a letter, a new range. */
	patch = reader->patch;
	start = reader->line;
	text = patch->items[reader->line];
	status = parse_range(text, 0, &old_first, &old_last, &cursor);
	if (status != 0)
		return -1;
	letter = *cursor;
	status = parse_range(cursor + 1, 0, &new_first, &new_last, &cursor);
	if (status != 0)
		return -1;
	reader->line++;

	/* The hunk; a is after its old line, d and c start at it. */
	hunk = new_hunk(reader, PATCH_NORMAL);
	if (hunk == NULL)
		return -1;
	hunk->raw_first = start;
	hunk->old_start = old_first;
	hunk->new_start = new_first;

	/* The old lines of d and c. */
	while (reader->line < patch->count) {
		text = patch->items[reader->line];
		if (text[0] == '\\') {
			hunk->old.last_unterminated = 1;
			reader->line++;
			continue;
		}

		/* Old lines until something else. */
		matched = starts_with(text, "< ");
		if (!matched)
			break;
		status = patch_lines_add(&hunk->old, text + 2, strlen(text + 2));
		if (status != 0)
			return -1;
		reader->line++;
	}

	/* The separator of c. */
	if (letter == 'c' && reader->line < patch->count) {
		compare = strcmp(patch->items[reader->line], "---");
		if (compare == 0)
			reader->line++;
	}

	/* The new lines of a and c. */
	while (reader->line < patch->count) {
		text = patch->items[reader->line];
		if (text[0] == '\\') {
			hunk->new.last_unterminated = 1;
			reader->line++;
			continue;
		}

		/* New lines until something else. */
		matched = starts_with(text, "> ");
		if (!matched)
			break;
		status = patch_lines_add(&hunk->new, text + 2, strlen(text + 2));
		if (status != 0)
			return -1;
		reader->line++;
	}

	/* The counts must agree with the ranges. */
	if (letter != 'a' && hunk->old.count != old_last - old_first + 1)
		return -1;
	if (letter != 'd' && hunk->new.count != new_last - new_first + 1)
		return -1;

	/* Succeeded: a normal hunk has no context. */
	hunk->raw_end = reader->line;
	return 0;
}

/*
 * Reads one ed command and, for a, c and i, the text after it up to the
 * line holding only a dot.
 */
static int
read_ed_command(
	struct patch_reader *reader)
{
	const struct patch_lines *patch;
	struct patch_hunk *hunk;
	const char *text;
	size_t length;
	int status;
	int letter;
	int compare;

	/* The command. */
	patch = reader->patch;
	text = patch->items[reader->line];
	hunk = new_hunk(reader, PATCH_ED);
	if (hunk == NULL)
		return -1;
	hunk->command = strdup(text);
	if (hunk->command == NULL)
		return -1;
	reader->line++;

	/* a, c and i take text up to a lone dot. */
	length = strlen(text);
	letter = text[length - 1];
	if (letter != 'a' && letter != 'c' && letter != 'i')
		return 0;
	while (reader->line < patch->count) {
		text = patch->items[reader->line];
		reader->line++;
		compare = strcmp(text, ".");
		if (compare == 0)
			return 0;
		status = patch_lines_add(&hunk->new, text, strlen(text));
		if (status != 0)
			return -1;
	}

	/* The text never ended. */
	return -1;
}

/*
 * Parses a range "a" or "a,b"; stores the first number, the second (or
 * the first again), and where the text after it starts.  In a unified
 * (counted) range the second number is a count and is 1 when left out.
 */
static int
parse_range(
	const char *text,
	int counted,
	long *first,
	long *last,
	const char **end)
{
	char *after;
	long value;

	/* The first number. */
	errno = 0;
	value = strtol(text, &after, 10);
	if (after == text || errno != 0 || value < 0)
		return -1;
	*first = value;
	*last = value;

	/* An optional second number after a comma. */
	if (*after == ',') {
		text = after + 1;
		value = strtol(text, &after, 10);
		if (after == text || errno != 0 || value < 0)
			return -1;
		*last = value;
	} else if (counted) {
		/* A unified range without a count counts one line. */
		*last = 1;
	}

	/* Succeeded: the range. */
	*end = after;
	return 0;
}

/*
 * Tells what a line starts, given the line after it and the kind of
 * difference -c, -e, -n or -u forces.
 */
static int
line_kind(
	const char *text,
	const char *next,
	int forced)
{
	int first;
	int second;
	int stars;

	/* An Index: line names the file of the next differences. */
	first = starts_with(text, "Index: ");
	if (first)
		return LINE_INDEX;

	/* The headers of unified and of context differences start a file. */
	if (next != NULL) {
		first = starts_with(text, "--- ");
		second = starts_with(next, "+++ ");
		if (first && second)
			return LINE_UNIFIED_HEADER;
		first = starts_with(text, "*** ");
		second = starts_with(next, "--- ");
		stars = starts_with(text, "***************");
		if (first && second && !stars)
			return LINE_CONTEXT_HEADER;
	}

	/* The start of a unified or a context hunk. */
	first = starts_with(text, "@@ -");
	if (first)
		return LINE_UNIFIED_HUNK;
	first = starts_with(text, "***************");
	if (first)
		return LINE_CONTEXT_HUNK;

	/* A normal command, unless -e; an ed command, unless another kind is forced. */
	if (forced != PATCH_ED) {
		first = is_normal_command(text);
		if (first)
			return LINE_NORMAL_HUNK;
	}

	/* An ed command, unless another kind is forced. */
	if (forced == PATCH_ED || forced == PATCH_UNKNOWN) {
		first = is_ed_command(text);
		if (first)
			return LINE_ED_COMMAND;
	}

	/* Anything else is text around the differences. */
	return LINE_TEXT;
}

/* Tells whether a line is a normal-format command, "NaM" and the like. */
static int
is_normal_command(
	const char *text)
{
	const char *cursor;

	/* A range, a letter, then another range. */
	cursor = skip_range(text);
	if (cursor == NULL)
		return 0;
	if (*cursor != 'a' && *cursor != 'c' && *cursor != 'd')
		return 0;
	cursor = skip_range(cursor + 1);
	if (cursor == NULL)
		return 0;

	/* Nothing may follow. */
	if (*cursor != '\0')
		return 0;
	return 1;
}

/*
 * Tells whether a line is an ed command diff -e writes: "Na", "N,Md",
 * "N,Mc", and the "a" and "s/.//" that follow a lone dot.
 */
static int
is_ed_command(
	const char *text)
{
	const char *cursor;
	int compare;

	/* The substitution and the bare append that fix a lone dot. */
	compare = strcmp(text, "s/.//");
	if (compare == 0)
		return 1;
	compare = strcmp(text, "a");
	if (compare == 0)
		return 1;

	/* An address or two, then a, c, d or i. */
	cursor = skip_range(text);
	if (cursor == NULL)
		return 0;
	if (*cursor != 'a' && *cursor != 'c' && *cursor != 'd' && *cursor != 'i')
		return 0;

	/* Nothing may follow. */
	if (cursor[1] != '\0')
		return 0;
	return 1;
}

/*
 * Skips a range, digits and maybe a comma and digits; returns what
 * follows, or NULL when there is no range.
 */
static const char *
skip_range(
	const char *cursor)
{
	const char *start;

	/* The first number. */
	start = cursor;
	while (*cursor >= '0' && *cursor <= '9')
		cursor++;
	if (cursor == start)
		return NULL;

	/* An optional second number. */
	if (*cursor != ',')
		return cursor;
	cursor++;
	start = cursor;
	while (*cursor >= '0' && *cursor <= '9')
		cursor++;
	if (cursor == start)
		return NULL;
	return cursor;
}

/* Tells whether text starts with a prefix. */
static int
starts_with(
	const char *text,
	const char *prefix)
{
	size_t length;
	int compare;

	/* Compares the prefix. */
	length = strlen(prefix);
	compare = strncmp(text, prefix, length);
	if (compare == 0)
		return 1;
	return 0;
}

/*
 * Takes the file name of a header line: up to a tab when there is one,
 * otherwise up to the first blank.
 */
static char *
header_name(
	const char *text)
{
	const char *end;
	char *name;
	size_t length;

	/* The name ends at a tab, or at a blank without one. */
	end = strchr(text, '\t');
	if (end == NULL)
		end = text + strcspn(text, " ");
	length = (size_t)(end - text);

	/* Copies it. */
	name = malloc(length + 1);
	if (name == NULL)
		return NULL;
	memcpy(name, text, length);
	name[length] = '\0';
	return name;
}

/*
 * Counts the context lines at both ends of a hunk from the marks of its
 * lines, a space marking context.
 */
static void
count_context(
	struct patch_hunk *hunk,
	const char *marks,
	long count)
{
	long index;

	/* The context at the start. */
	hunk->leading = 0;
	for (index = 0; index < count && marks[index] == ' '; index++)
		hunk->leading++;

	/* The context at the end, unless all of it is context. */
	hunk->trailing = 0;
	if (hunk->leading == count)
		return;
	for (index = count - 1; index >= 0 && marks[index] == ' '; index--)
		hunk->trailing++;
}
