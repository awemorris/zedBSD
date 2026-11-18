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
 * keys; this list numbers the same lines for fc.  HISTSIZE bounds both.
 *
 * An interactive shell reading a terminal also keeps the lines in a file
 * (HISTFILE, or $HOME/.sh_history when it is unset; an empty HISTFILE keeps
 * none): it reads the file when it starts, so a new shell can recall what
 * earlier ones ran, and adds each line to the file as soon as it is read.
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

/* The history file in the home directory when HISTFILE is unset. */
#define HISTORY_FILE_NAME ".sh_history"

/* How many HISTSIZEs of lines the history file may grow to before it is cut down. */
#define HISTORY_FILE_SLACK 2

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

/*
 * Set once sh_history_load() has run: the shell is interactive on a
 * terminal, and each line it reads goes to the history file.  A shell that
 * never loads the file (a script, -c, input that is no terminal) never
 * writes it either.
 */
static int history_file_enabled;

static int history_limit(void);
static char *history_file_path(void);
static char *history_file_read(const char *path, size_t *length);
static void history_file_rewrite(const char *path);
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
 * Reads the history file into both histories, for an interactive shell on a
 * terminal, and turns on adding each line read to the file.
 *
 * Only the newest HISTSIZE lines are kept; a file that has grown past twice
 * that is cut down to them.  A missing or unreadable file is no error.
 */
void
sh_history_load(
	void)
{
	char *path;
	char *text;
	char *line;
	char *end;
	size_t length;
	size_t position;
	int lines;
	int skip;
	int limit;

	/* The line editor keeps as many lines as the fc list, and lines are saved from now on. */
	limit = history_limit();
	stifle_history(limit);
	history_file_enabled = 1;

	/* No HISTFILE and no home directory, or an empty HISTFILE, is no file. */
	path = history_file_path();
	if (path == NULL)
		return;

	/* The whole file; one that cannot be read gives nothing. */
	text = history_file_read(path, &length);
	if (text == NULL) {
		free(path);
		return;
	}

	/* Counts the lines, so that only the newest ones are added. */
	lines = 0;
	for (position = 0; position < length; position++) {
		if (text[position] == '\n')
			lines++;
	}

	/* A last line the file does not end is a line too. */
	if (length > 0 && text[length - 1] != '\n')
		lines++;

	/* The lines older than the newest HISTSIZE are passed over. */
	skip = 0;
	if (lines > limit)
		skip = lines - limit;

	/* Each line after them goes into both histories. */
	line = text;
	while (line < text + length) {
		end = memchr(line, '\n', (size_t)(text + length - line));
		if (end == NULL)
			end = text + length;
		*end = '\0';

		/* A passed-over line only counts down. */
		if (skip > 0) {
			skip--;
		} else {
			sh_history_add(line);
			add_history(line);
		}

		/* The next line starts after the newline. */
		line = end + 1;
	}

	/* A file past twice the limit is cut down to the lines kept. */
	if (lines > HISTORY_FILE_SLACK * limit)
		history_file_rewrite(path);

	/* The file's text and name are no longer needed. */
	free(text);
	free(path);
}

/*
 * Adds a line read from the terminal to the end of the history file, when
 * the shell keeps one.  A file that cannot be written is left as it is.
 */
void
sh_history_save(
	const char *line)
{
	char *path;
	char *record;
	size_t length;
	ssize_t written;
	int descriptor;

	/* Only a shell that loaded the file writes it. */
	if (!history_file_enabled)
		return;

	/* A blank line is not kept, as sh_history_add does not keep it. */
	length = strlen(line);
	while (length > 0 && line[length - 1] == '\n')
		length--;
	if (length == 0)
		return;

	/* No file is kept without a HISTFILE or a home directory. */
	path = history_file_path();
	if (path == NULL)
		return;

	/* The line with its newline, so that one write adds all of it. */
	record = sh_malloc(length + 1U);
	memcpy(record, line, length);
	record[length] = '\n';

	/* Appends it, making the file readable by its owner only. */
	descriptor = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	free(path);
	if (descriptor < 0) {
		free(record);
		return;
	}

	/* One write, so that lines from shells running side by side do not mix. */
	do
		written = write(descriptor, record, length + 1U);
	while (written < 0 && errno == EINTR);
	(void)close(descriptor);
	free(record);
}

/*
 * Tells the line editor that HISTSIZE changed, so that it keeps as many
 * lines as the fc list does.
 */
void
sh_history_size_changed(
	const char *name)
{
	int limit;

	/* The hook is registered for HISTSIZE alone, so the name says nothing new. */
	(void)name;

	/* The editor follows only once the shell keeps a history at all. */
	if (!history_file_enabled)
		return;

	/* The editor keeps the new number of lines. */
	limit = history_limit();
	stifle_history(limit);
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
 * Returns the history file's path in allocated memory, or NULL when there
 * is none: HISTFILE is empty, or it is unset and HOME is unset or empty.
 */
static char *
history_file_path(
	void)
{
	const char *file;
	const char *home;
	char *path;
	size_t length;

	/* HISTFILE names the file; set but empty, it asks for none. */
	file = sh_var_get("HISTFILE");
	if (file != NULL) {
		if (file[0] == '\0')
			return NULL;
		path = sh_strdup(file);
		return path;
	}

	/* Without it, the file is in the home directory. */
	home = sh_var_get("HOME");
	if (home == NULL || home[0] == '\0')
		return NULL;

	/* The home directory, a slash and the file's name. */
	length = strlen(home) + strlen(HISTORY_FILE_NAME) + 2U;
	path = sh_malloc(length);
	snprintf(path, length, "%s/%s", home, HISTORY_FILE_NAME);

	/* Succeeded: the path, which the caller frees. */
	return path;
}

/*
 * Reads a whole file into allocated memory and gives its length; returns
 * NULL when it cannot be opened or read.
 */
static char *
history_file_read(
	const char *path,
	size_t *length)
{
	char *text;
	size_t capacity;
	ssize_t count;
	int descriptor;

	/* Opens it for reading. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return NULL;

	/* Reads all of it, growing the buffer as it fills. */
	capacity = HISTORY_READ_CHUNK;
	*length = 0;
	text = sh_malloc(capacity);
	for (;;) {
		/* A full buffer doubles before the next read. */
		if (*length == capacity) {
			capacity *= 2U;
			text = sh_realloc(text, capacity);
		}

		/* The next part of the file, retrying an interrupted read. */
		do
			count = read(descriptor, text + *length, capacity - *length);
		while (count < 0 && errno == EINTR);
		if (count <= 0)
			break;

		/* The part read joins the text. */
		*length += (size_t)count;
	}

	/* The file is read; a failed read gives nothing. */
	(void)close(descriptor);
	if (count < 0) {
		free(text);
		return NULL;
	}

	/* Succeeded: the text, which the caller frees. */
	return text;
}

/*
 * Replaces the history file with the lines the fc list kept, through a new
 * file renamed over it so that the old file stays whole if writing fails.
 */
static void
history_file_rewrite(
	const char *path)
{
	char *temporary;
	char *record;
	size_t size;
	size_t length;
	ssize_t written;
	int descriptor;
	int index;
	int renamed;

	/* The new file sits next to the old one, named after this shell. */
	size = strlen(path) + 32U;
	temporary = sh_malloc(size);
	snprintf(temporary, size, "%s.%ld", path, (long)getpid());
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC, 0600);
	if (descriptor < 0) {
		free(temporary);
		return;
	}

	/* Each line the fc list kept, which are the newest of the file, with its newline. */
	for (index = 0; index < history_count; index++) {
		length = strlen(history_entries[index].text);
		record = sh_malloc(length + 1U);
		memcpy(record, history_entries[index].text, length);
		record[length] = '\n';
		do
			written = write(descriptor, record, length + 1U);
		while (written < 0 && errno == EINTR);
		free(record);

		/* A failed or short write leaves the old file in place. */
		if (written != (ssize_t)(length + 1U)) {
			(void)close(descriptor);
			(void)unlink(temporary);
			free(temporary);
			return;
		}
	}

	/* The new file takes the old one's place. */
	(void)close(descriptor);
	renamed = rename(temporary, path);
	if (renamed != 0)
		(void)unlink(temporary);
	free(temporary);
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
