/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The command history of an interactive shell, and the fc builtin
 * (POSIX XCU fc): list, edit and run again the commands typed.
 *
 * The line editor keeps its own history for recalling lines with the arrow
 * keys; this list numbers the same lines for fc.  HISTSIZE bounds it.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/vars.h"

#include <errno.h>
#include <fcntl.h>
#include <readline/history.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* How many lines are kept when HISTSIZE does not say. */
#define HISTORY_DEFAULT 128

/* How many lines fc -l lists when no range is given. */
#define HISTORY_LIST 16

/* How much of the edited file is read at a time. */
#define HISTORY_READ_CHUNK 512

/*
 * One line of the history, with the number fc names it by.
 *
 * The text belongs to the entry until the entry leaves the history.
 */
struct history_entry {
	int number;
	char *text;
};

/*
 * The lines, oldest first.
 *
 * Entries leave from the front when HISTSIZE is reached; their numbers keep
 * counting, so a number names the same line for as long as it is kept.
 */
static struct history_entry *history_entries;
static int history_count;
static int history_capacity;

/* The number the next line gets; it only grows. */
static int history_next = 1;

static int history_limit(void);
static int history_find(const char *text, int *found);
static int history_range(int argc, char **argv, int index, int default_first, int *first, int *last);
static void history_drop_fc(void);
static int fc_list(int first, int last, int reverse, int numbers);
static int fc_edit(int first, int last, int reverse, const char *editor);
static int fc_write_file(const char *path, int descriptor, int first, int last);
static char *fc_read_file(const char *path);
static int fc_substitute(int argc, char **argv, int index);
static char *replace_first(const char *text, const char *old_text, const char *new_text);

/*
 * Adds a line to the history (its newline, if any, left off).
 */
void
sh_history_add(
	const char *line)
{
	size_t length;
	int limit;

	/* Blank lines are not kept. */
	length = strlen(line);
	while (length > 0 && line[length - 1] == '\n')
		length--;
	if (length == 0)
		return;

	/* The oldest lines go while the list is full. */
	limit = history_limit();
	while (history_count >= limit && history_count > 0) {
		free(history_entries[0].text);
		memmove(history_entries, history_entries + 1,
			(size_t)(history_count - 1) * sizeof(*history_entries));
		history_count--;
	}

	/* Grows the array when it is full. */
	if (history_count == history_capacity) {
		if (history_capacity == 0)
			history_capacity = 32;
		else
			history_capacity *= 2;
		history_entries = sh_realloc(history_entries,
					     (size_t)history_capacity *
					     sizeof(*history_entries));
	}

	/* The line becomes the newest entry, with the next number. */
	history_entries[history_count].number = history_next++;
	history_entries[history_count].text = sh_strndup(line, length);
	history_count++;
}

/*
 * Implements fc: fc -l [-nr] [first [last]], fc -s [old=new] [first], and
 * fc [-r] [-e editor] [first [last]].
 */
int
sh_fc_builtin(
	int argc,
	char **argv)
{
	const char *editor;
	const char *word;
	int list;
	int numbers;
	int reverse;
	int substitute;
	int index;
	int option;
	int first;
	int last;
	int ranged;
	int status;

	/* Reads the options: -l, -n, -r, -s and -e editor. */
	list = 0;
	numbers = 1;
	reverse = 0;
	substitute = 0;
	editor = NULL;
	for (index = 1; index < argc; index++) {
		word = argv[index];

		/* A word that is not an option, or is a negative number, ends them. */
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] >= '0' && word[1] <= '9')
			break;
		if (word[1] == '-' && word[2] == '\0') {
			index++;
			break;
		}

		/* Each letter of the word. */
		for (option = 1; word[option] != '\0'; option++) {
			/* Dispatches on the letter. */
			switch (word[option]) {
			case 'l':
				list = 1;
				break;
			case 'n':
				numbers = 0;
				break;
			case 'r':
				reverse = 1;
				break;
			case 's':
				substitute = 1;
				break;
			case 'e':
				if (word[option + 1] != '\0') {
					editor = word + option + 1;
				} else if (index + 1 < argc) {
					index++;
					editor = argv[index];
				} else {
					fprintf(stderr, "fc: -e needs an "
						"editor\n");
					return 2;
				}

				/* The rest of the word was the option's argument. */
				option = (int)strlen(word) - 1;
				break;
			default:
				fprintf(stderr, "fc: illegal option -%c\n",
					word[option]);
				return 2;
			}
		}
	}

	/* -s runs a command again at once. */
	if (substitute) {
		status = fc_substitute(argc, argv, index);
		return status;
	}

	/* The range; for a listing the default is the last sixteen. */
	if (list) {
		ranged = history_range(argc, argv, index, HISTORY_LIST, &first, &last);
	} else {
		ranged = history_range(argc, argv, index, 1, &first, &last);
	}

	/* A range that does not resolve was reported. */
	if (ranged != 0)
		return 1;

	/* -l lists; otherwise the range is edited and run. */
	if (list)
		status = fc_list(first, last, reverse, numbers);
	else
		status = fc_edit(first, last, reverse, editor);

	/* Succeeded: the status of the listing or of what was run. */
	return status;
}

/* Returns how many lines HISTSIZE lets the history keep. */
static int
history_limit(
	void)
{
	const char *value;
	int limit;

	/* HISTSIZE, when it is set to a positive number. */
	value = sh_var_get("HISTSIZE");
	if (value == NULL)
		return HISTORY_DEFAULT;
	limit = atoi(value);
	if (limit <= 0)
		return HISTORY_DEFAULT;

	/* Succeeded: the limit. */
	return limit;
}

/*
 * Finds the entry a first or last operand names: a number (negative counts
 * back from the newest), or the newest line starting with a string.
 * Returns 0 with the index in *found.
 */
static int
history_find(
	const char *text,
	int *found)
{
	char *end;
	long number;
	int index;
	int compare;

	/* A number: an entry number, or an offset back from the newest. */
	number = strtol(text, &end, 10);
	if (*text != '\0' && *end == '\0') {
		/* A negative number counts back from the newest. */
		if (number < 0) {
			index = history_count + (int)number;
			if (index < 0)
				index = 0;
			*found = index;
			return 0;
		}

		/* A positive one names the first entry at or after it. */
		for (index = 0; index < history_count; index++) {
			if (history_entries[index].number >= number) {
				*found = index;
				return 0;
			}
		}

		/* A number past the newest is the newest. */
		*found = history_count - 1;
		return 0;
	}

	/* A string: the newest line that starts with it. */
	for (index = history_count - 1; index >= 0; index--) {
		compare = strncmp(history_entries[index].text, text,
				  strlen(text));
		if (compare == 0) {
			*found = index;
			return 0;
		}
	}

	/* No entry starts with the text. */
	fprintf(stderr, "fc: %s: not found in history\n", text);

	/* No line starts with it. */
	return 1;
}

/*
 * Reads the first and last operands of fc.  Without first, it is the entry
 * default_first back from the newest; without last, it is the newest (for
 * a listing) or first (for an edit).
 */
static int
history_range(
	int argc,
	char **argv,
	int index,
	int default_first,
	int *first,
	int *last)
{
	int found;

	/* The fc command itself is the newest entry, and is no target. */
	history_drop_fc();
	if (history_count == 0) {
		fprintf(stderr, "fc: history is empty\n");
		return 1;
	}

	/* The first entry: the operand, or counted back from the newest. */
	if (index < argc) {
		found = history_find(argv[index], first);
		if (found != 0)
			return 1;
	} else {
		*first = history_count - default_first;
		if (*first < 0)
			*first = 0;
	}

	/* The last entry: the operand, the newest, or the first. */
	if (index + 1 < argc) {
		found = history_find(argv[index + 1], last);
		if (found != 0)
			return 1;
	} else if (default_first > 1) {
		*last = history_count - 1;
	} else {
		*last = *first;
	}

	/* Succeeded: both entries are known. */
	return 0;
}

/* Takes the fc command that is running out of the history. */
static void
history_drop_fc(
	void)
{
	int compare;

	/* The newest entry, when it is an fc command. */
	if (history_count == 0)
		return;
	compare = strncmp(history_entries[history_count - 1].text, "fc", 2);
	if (compare != 0)
		return;

	/* It goes; its number is not given again. */
	free(history_entries[history_count - 1].text);
	history_count--;
}

/* Lists the entries from first to last (fc -l). */
static int
fc_list(
	int first,
	int last,
	int reverse,
	int numbers)
{
	int entry;
	int step;

	/* -r lists the other way round. */
	if (reverse) {
		entry = first;
		first = last;
		last = entry;
	}

	/* Each entry, with its number unless -n, walking toward last. */
	step = 1;
	if (first > last)
		step = -1;
	for (entry = first;
	     ;
	     entry += step) {
		if (numbers) {
			printf("%d\t%s\n", history_entries[entry].number, history_entries[entry].text);
		} else {
			printf("\t%s\n", history_entries[entry].text);
		}

		/* The walk ends at last. */
		if (entry == last)
			break;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Edits the entries from first to last in a temporary file with the editor
 * (-e, FCEDIT, or ed), then runs what the file holds.
 */
static int
fc_edit(
	int first,
	int last,
	int reverse,
	const char *editor)
{
	char path[] = "/tmp/fcXXXXXX";
	char *command;
	char *text;
	size_t length;
	int descriptor;
	int entry;
	int written;
	int status;

	/* The editor: -e, then FCEDIT, then ed. */
	if (editor == NULL)
		editor = sh_var_get("FCEDIT");
	if (editor == NULL || editor[0] == '\0')
		editor = "ed";

	/* -r writes the other way round. */
	if (reverse) {
		entry = first;
		first = last;
		last = entry;
	}

	/* Writes the entries to a new file. */
	descriptor = mkstemp(path);
	if (descriptor < 0) {
		fprintf(stderr, "fc: cannot make a file: %s\n",
			strerror(errno));
		return 1;
	}

	/* The entries go into the file. */
	written = fc_write_file(path, descriptor, first, last);
	if (written != 0)
		return 1;

	/* Runs the editor on it. */
	length = strlen(editor) + strlen(path) + 2U;
	command = sh_temp_own(sh_malloc(length));
	snprintf(command, length, "%s %s", editor, path);
	status = sh_eval_string(command, 0);
	if (status != 0) {
		(void)unlink(path);
		return status;
	}

	/* Reads back what it holds. */
	text = fc_read_file(path);
	if (text == NULL)
		return 1;

	/* Shows and runs it; it becomes the newest entry. */
	fputs(text, stderr);
	sh_history_add(text);
	status = sh_eval_string(text, 0);

	/* Succeeded: the status of what was run. */
	return status;
}

/* Writes the entries from first to last to a file opened on descriptor. */
static int
fc_write_file(
	const char *path,
	int descriptor,
	int first,
	int last)
{
	FILE *file;
	int entry;
	int step;

	/* A stream on the descriptor. */
	file = fdopen(descriptor, "w");
	if (file == NULL) {
		(void)close(descriptor);
		(void)unlink(path);
		return 1;
	}

	/* One entry per line, walking toward last. */
	step = 1;
	if (first > last)
		step = -1;
	for (entry = first;
	     ;
	     entry += step) {
		fprintf(file, "%s\n", history_entries[entry].text);
		if (entry == last)
			break;
	}

	/* The file is complete. */
	fclose(file);

	/* Succeeded. */
	return 0;
}

/* Reads a file the editor changed, and removes it.  Returns NULL on failure. */
static char *
fc_read_file(
	const char *path)
{
	char chunk[HISTORY_READ_CHUNK];
	char *text;
	size_t length;
	size_t capacity;
	ssize_t count;
	int descriptor;

	/* Opens it; it is not needed after this. */
	descriptor = open(path, O_RDONLY);
	(void)unlink(path);
	if (descriptor < 0)
		return NULL;

	/* Reads all of it into a temporary buffer. */
	capacity = HISTORY_READ_CHUNK;
	length = 0;
	text = sh_temp_own(sh_malloc(capacity));
	for (;;) {
		count = read(descriptor, chunk, sizeof(chunk));
		if (count <= 0)
			break;
		if (length + (size_t)count + 1U > capacity) {
			while (length + (size_t)count + 1U > capacity)
				capacity *= 2U;
			text = sh_temp_grow(text, length, capacity);
		}

		/* The chunk joins the text. */
		memcpy(text + length, chunk, (size_t)count);
		length += (size_t)count;
	}

	/* The file has been read whole. */
	(void)close(descriptor);
	text[length] = '\0';

	/* Succeeded: the text, freed with the command. */
	return text;
}

/* Runs an entry again, with old=new replaced in it (fc -s). */
static int
fc_substitute(
	int argc,
	char **argv,
	int index)
{
	const char *equals;
	char *old_text;
	char *text;
	int entry;
	int ranged;
	int status;

	/* old=new comes first, when given. */
	old_text = NULL;
	equals = NULL;
	if (index < argc)
		equals = strchr(argv[index], '=');
	if (equals != NULL) {
		old_text = sh_temp_own(sh_strndup(argv[index],
				       (size_t)(equals - argv[index])));
		index++;
	}

	/* The entry: the operand, or the newest. */
	ranged = history_range(argc, argv, index, 1, &entry, &entry);
	if (ranged != 0)
		return 1;

	/* Its text, with the replacement made. */
	text = sh_temp_own(sh_strdup(history_entries[entry].text));
	if (old_text != NULL)
		text = replace_first(text, old_text, equals + 1);

	/* Shows and runs it, and it becomes the newest entry. */
	fprintf(stderr, "%s\n", text);
	sh_history_add(text);
	add_history(text);
	status = sh_eval_string(text, 0);

	/* Succeeded: the status of what was run. */
	return status;
}

/* Replaces the first occurrence of a string in a text. */
static char *
replace_first(
	const char *text,
	const char *old_text,
	const char *new_text)
{
	const char *found;
	char *result;
	size_t length;

	/* An empty or absent string replaces nothing. */
	if (old_text[0] == '\0')
		return (char *)text;
	found = strstr(text, old_text);
	if (found == NULL)
		return (char *)text;

	/* The text before, the new text, the text after. */
	length = strlen(text) - strlen(old_text) + strlen(new_text) + 1U;
	result = sh_temp_own(sh_malloc(length));
	snprintf(result, length, "%.*s%s%s", (int)(found - text), text,
		 new_text, found + strlen(old_text));

	/* Succeeded: the new text, freed with the command. */
	return result;
}
