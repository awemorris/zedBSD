/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Searches files for lines that match patterns (POSIX XCU grep, with the
 * GNU extensions that scripts use).
 *
 *	grep [options] pattern_list [file...]
 *	grep [options] -e pattern_list [-e ...] [-f file]... [file...]
 *
 * A pattern list is patterns separated by newlines; a line is selected when
 * any pattern matches it.  Patterns are basic regular expressions, extended
 * ones with -E, or fixed strings with -F.  An empty pattern matches every
 * line.
 *
 * This file reads the command line and walks the files: the operands, and
 * with -r the files below the directories among them (a walk that the
 * --include, --exclude and --exclude-dir globs prune).  search.c searches
 * each file.  Options may follow operands, as GNU grep takes them, unless
 * POSIXLY_CORRECT is set.
 *
 * The exit status is 0 when a line was selected, 1 when none was, and 2 for
 * an error (a file that could not be read, a bad pattern); with -q a
 * selected line makes it 0 whatever else happened.
 */

#include "userland/base/grep/grep.h"
#include "userland/base/common/command.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* The codes of the long options that have no letter. */
#define OPTION_INCLUDE		256
#define OPTION_EXCLUDE		257
#define OPTION_EXCLUDE_DIR	258
#define OPTION_EXCLUDE_FROM	259
#define OPTION_LABEL		260
#define OPTION_COLOR		261
#define OPTION_BINARY_FILES	262
#define OPTION_GROUP_SEPARATOR	263
#define OPTION_NO_SEPARATOR	264
#define OPTION_LINE_BUFFERED	265
#define OPTION_HELP		266
#define OPTION_NO_IGNORE_CASE	267

/* The letters grep takes, as command_options_next reads them. */
#define GREP_LETTERS "A:B:C:D:EFGHIJLPRTUVZabcd:e:f:hilm:noqrsuvwxyz"

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option grep_long_options[] = {
	{"after-context", COMMAND_VALUE_REQUIRED, 'A'},
	{"basic-regexp", COMMAND_VALUE_NONE, 'G'},
	{"before-context", COMMAND_VALUE_REQUIRED, 'B'},
	{"binary-files", COMMAND_VALUE_REQUIRED, OPTION_BINARY_FILES},
	{"byte-offset", COMMAND_VALUE_NONE, 'b'},
	{"color", COMMAND_VALUE_OPTIONAL, OPTION_COLOR},
	{"colour", COMMAND_VALUE_OPTIONAL, OPTION_COLOR},
	{"context", COMMAND_VALUE_REQUIRED, 'C'},
	{"count", COMMAND_VALUE_NONE, 'c'},
	{"dereference-recursive", COMMAND_VALUE_NONE, 'R'},
	{"devices", COMMAND_VALUE_REQUIRED, 'D'},
	{"directories", COMMAND_VALUE_REQUIRED, 'd'},
	{"exclude", COMMAND_VALUE_REQUIRED, OPTION_EXCLUDE},
	{"exclude-dir", COMMAND_VALUE_REQUIRED, OPTION_EXCLUDE_DIR},
	{"exclude-from", COMMAND_VALUE_REQUIRED, OPTION_EXCLUDE_FROM},
	{"extended-regexp", COMMAND_VALUE_NONE, 'E'},
	{"file", COMMAND_VALUE_REQUIRED, 'f'},
	{"files-with-matches", COMMAND_VALUE_NONE, 'l'},
	{"files-without-match", COMMAND_VALUE_NONE, 'L'},
	{"fixed-strings", COMMAND_VALUE_NONE, 'F'},
	{"group-separator", COMMAND_VALUE_REQUIRED, OPTION_GROUP_SEPARATOR},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"ignore-case", COMMAND_VALUE_NONE, 'i'},
	{"include", COMMAND_VALUE_REQUIRED, OPTION_INCLUDE},
	{"invert-match", COMMAND_VALUE_NONE, 'v'},
	{"label", COMMAND_VALUE_REQUIRED, OPTION_LABEL},
	{"line-buffered", COMMAND_VALUE_NONE, OPTION_LINE_BUFFERED},
	{"line-number", COMMAND_VALUE_NONE, 'n'},
	{"line-regexp", COMMAND_VALUE_NONE, 'x'},
	{"max-count", COMMAND_VALUE_REQUIRED, 'm'},
	{"no-filename", COMMAND_VALUE_NONE, 'h'},
	{"no-group-separator", COMMAND_VALUE_NONE, OPTION_NO_SEPARATOR},
	{"no-ignore-case", COMMAND_VALUE_NONE, OPTION_NO_IGNORE_CASE},
	{"no-messages", COMMAND_VALUE_NONE, 's'},
	{"null", COMMAND_VALUE_NONE, 'Z'},
	{"null-data", COMMAND_VALUE_NONE, 'z'},
	{"only-matching", COMMAND_VALUE_NONE, 'o'},
	{"perl-regexp", COMMAND_VALUE_NONE, 'P'},
	{"quiet", COMMAND_VALUE_NONE, 'q'},
	{"recursive", COMMAND_VALUE_NONE, 'r'},
	{"regexp", COMMAND_VALUE_REQUIRED, 'e'},
	{"silent", COMMAND_VALUE_NONE, 'q'},
	{"text", COMMAND_VALUE_NONE, 'a'},
	{"version", COMMAND_VALUE_NONE, 'V'},
	{"with-filename", COMMAND_VALUE_NONE, 'H'},
	{"word-regexp", COMMAND_VALUE_NONE, 'w'},
	{NULL, 0, 0}
};

/*
 * What the walk over the files has found so far: whether a line was
 * selected, and whether an error was met.  -q ends the walk at the first
 * selected line, so the walk asks whether it is done before each file.
 */
struct walk_state {
	const struct grep_options *options;
	int selected;
	int error;
	int done;

	/* -d skip: directory operands are passed over without a message. */
	int skip_directories;
};

static int read_options(int argc, char **argv, struct grep_options *options, struct walk_state *walk);
static int apply_option(struct grep_options *options, struct walk_state *walk, int code, const char *value);
static int apply_context(struct grep_options *options, int code, const char *value);
static int apply_mode_value(struct grep_options *options, struct walk_state *walk, int code, const char *value);
static int parse_count(const char *text, unsigned long *count);
static void add_glob(struct grep_globs *globs, const char *glob);
static int add_glob_file(struct grep_globs *globs, const char *name);
static void add_pattern_list(struct grep_options *options, const char *list, size_t length);
static int add_pattern_file(struct grep_options *options, const char *name);
static void add_pattern(struct grep_options *options, const char *text, size_t length);
static int compile_patterns(struct grep_options *options);
static void search_operand(struct walk_state *walk, const char *name);
static void search_file(struct walk_state *walk, const char *path, int show_names);
static void search_standard_input(struct walk_state *walk);
static void walk_directory(struct walk_state *walk, const char *path, int implicit);
static void walk_entry(struct walk_state *walk, const char *path, const char *base);
static char *join_path(const char *directory, const char *name, int implicit);
static int glob_matches(const struct grep_globs *globs, const char *name, int suffixes);
static int file_chosen(const struct grep_options *options, const char *name, int suffixes);
static void report(const struct grep_options *options, const char *name, int error);
static void version(void);
static void usage(int status);

/*
 * Runs grep.
 */
int
main(
	int argc,
	char **argv)
{
	struct grep_options options;
	struct walk_state walk;
	int operands;
	int index;
	int ok;

	/* The options, and the pattern list when no -e or -f gave one. */
	memset(&options, 0, sizeof(options));
	memset(&walk, 0, sizeof(walk));
	options.names = GREP_NAMES_AUTO;
	options.max_count = -1;
	options.group_separator = "--";
	operands = read_options(argc, argv, &options, &walk);
	index = 1;
	if (!options.have_patterns) {
		if (operands == 0)
			usage(2);
		add_pattern_list(&options, argv[index], strlen(argv[index]));
		index++;
		operands--;
	}

	/* The patterns, compiled. */
	ok = compile_patterns(&options);
	if (!ok)
		return 2;

	/* Names go before the lines when there are several files. */
	options.show_names = 0;
	if (operands > 1)
		options.show_names = 1;
	if (options.names == GREP_NAMES_ALWAYS)
		options.show_names = 1;
	if (options.names == GREP_NAMES_NEVER)
		options.show_names = 0;

	/* The walk over the files. */
	walk.options = &options;
	if (operands == 0 && options.recursive != GREP_RECURSE_NONE) {
		/* -r with no operand searches the working directory. */
		walk_directory(&walk, ".", 1);
	} else if (operands == 0) {
		/* Otherwise standard input. */
		search_standard_input(&walk);
	}

	/* Each operand, until -q has its answer. */
	for (; operands > 0 && !walk.done; operands--) {
		search_operand(&walk, argv[index]);
		index++;
	}

	/* A selected line with -q hides an error. */
	fflush(stdout);
	if (walk.selected && options.output == GREP_OUTPUT_QUIET)
		return 0;

	/* An error. */
	if (walk.error)
		return 2;

	/* No line selected. */
	if (!walk.selected)
		return 1;

	/* Succeeded: a line was selected. */
	return 0;
}

/*
 * Allocates or resizes memory, ending grep when there is none.
 */
void *
grep_allocate(
	void *memory,
	size_t size)
{
	void *resized;

	/* The memory, at least one byte of it. */
	if (size == 0)
		size = 1;
	resized = realloc(memory, size);
	if (resized == NULL) {
		fprintf(stderr, "grep: out of memory\n");
		exit(2);
	}

	/* Succeeded. */
	return resized;
}

/*
 * Reads the options; returns the number of operands, which are left in
 * argv from argv[1] on.
 */
static int
read_options(
	int argc,
	char **argv,
	struct grep_options *options,
	struct walk_state *walk)
{
	struct command_options scan;
	int code;
	int ok;

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "grep";
	scan.letters = GREP_LETTERS;
	scan.names = grep_long_options;
	scan.numbers = 1;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;
		if (code == COMMAND_OPTION_ERROR)
			usage(2);

		/* The option's effect. */
		ok = apply_option(options, walk, code, scan.value);
		if (!ok)
			usage(2);
	}

	/* Succeeded: the operands follow argv[0]. */
	return scan.operand_count;
}

/* Applies one option.  Returns 0 for a value it cannot take. */
static int
apply_option(
	struct grep_options *options,
	struct walk_state *walk,
	int code,
	const char *value)
{
	int ok;

	/* The option of its code. */
	switch (code) {
	case 'E':
		options->mode = GREP_MODE_EXTENDED;
		break;
	case 'F':
		options->mode = GREP_MODE_FIXED;
		break;
	case 'G':
		options->mode = GREP_MODE_BASIC;
		break;
	case 'P':
		fprintf(stderr, "grep: Perl matching not supported\n");
		exit(2);
	case 'c':
		options->output = GREP_OUTPUT_COUNT;
		break;
	case 'l':
		options->output = GREP_OUTPUT_NAMES;
		break;
	case 'L':
		options->output = GREP_OUTPUT_UNMATCHED;
		break;
	case 'q':
		options->output = GREP_OUTPUT_QUIET;
		break;
	case 'i':
	case 'y':
		options->ignore_case = 1;
		break;
	case OPTION_NO_IGNORE_CASE:
		options->ignore_case = 0;
		break;
	case 'n':
		options->line_numbers = 1;
		break;
	case 'b':
		options->byte_offset = 1;
		break;
	case 's':
		options->no_messages = 1;
		break;
	case 'v':
		options->invert = 1;
		break;
	case 'x':
		options->whole_line = 1;
		break;
	case 'w':
		options->whole_word = 1;
		break;
	case 'o':
		options->only_matching = 1;
		break;
	case 'H':
		options->names = GREP_NAMES_ALWAYS;
		break;
	case 'h':
		options->names = GREP_NAMES_NEVER;
		break;
	case 'Z':
		options->null_after_name = 1;
		break;
	case 'z':
		options->null_data = 1;
		break;
	case 'a':
		options->binary_files = GREP_BINARY_TEXT;
		break;
	case 'I':
		options->binary_files = GREP_BINARY_SKIP;
		break;
	case 'r':
		options->recursive = GREP_RECURSE;
		break;
	case 'R':
		options->recursive = GREP_RECURSE_FOLLOW;
		break;
	case 'e':
		/* A pattern list. */
		add_pattern_list(options, value, strlen(value));
		options->have_patterns = 1;
		break;
	case 'f':
		/* A file of patterns. */
		ok = add_pattern_file(options, value);
		if (!ok)
			exit(2);
		options->have_patterns = 1;
		break;
	case OPTION_INCLUDE:
		add_glob(&options->include, value);
		break;
	case OPTION_EXCLUDE:
		add_glob(&options->exclude, value);
		break;
	case OPTION_EXCLUDE_DIR:
		add_glob(&options->exclude_dir, value);
		break;
	case OPTION_EXCLUDE_FROM:
		ok = add_glob_file(&options->exclude, value);
		if (!ok)
			exit(2);
		break;
	case OPTION_LABEL:
		options->label = value;
		break;
	case OPTION_GROUP_SEPARATOR:
		options->group_separator = value;
		break;
	case OPTION_NO_SEPARATOR:
		options->group_separator = NULL;
		break;
	case OPTION_LINE_BUFFERED:
		setvbuf(stdout, NULL, _IOLBF, 0);
		break;
	case 'U':
	case 'u':
	case 'J':
	case 'T':
		/* Binary-mode and tab options of other systems change nothing. */
		break;
	case 'V':
		version();
		break;
	case OPTION_HELP:
		usage(0);
		break;
	case 'A':
	case 'B':
	case 'C':
	case 'm':
	case COMMAND_OPTION_NUMBER:
		ok = apply_context(options, code, value);
		return ok;
	case 'd':
	case 'D':
	case OPTION_COLOR:
	case OPTION_BINARY_FILES:
		ok = apply_mode_value(options, walk, code, value);
		return ok;
	default:
		return 0;
	}

	/* Succeeded. */
	return 1;
}

/* Applies -A, -B, -C, -NUM or -m, which take a count. */
static int
apply_context(
	struct grep_options *options,
	int code,
	const char *value)
{
	unsigned long count;
	int ok;

	/* The count. */
	ok = parse_count(value, &count);
	if (!ok) {
		fprintf(stderr, "grep: %s: invalid context length argument\n",
			value);
		return 0;
	}

	/* -m: the most lines to select in each file. */
	if (code == 'm') {
		options->max_count = (long)count;
		return 1;
	}

	/* The lines after, before, or both. */
	options->context = 1;
	if (code == 'A') {
		options->after = count;
	} else if (code == 'B') {
		options->before = count;
	} else {
		options->after = count;
		options->before = count;
	}

	/* Succeeded. */
	return 1;
}

/*
 * Applies the options that take a word: -d and -D (what to do with
 * directories and devices), --color and --binary-files.
 */
static int
apply_mode_value(
	struct grep_options *options,
	struct walk_state *walk,
	int code,
	const char *value)
{
	int differs;

	/* --color: colours are never written, whatever is asked. */
	if (code == OPTION_COLOR)
		return 1;

	/* -D: devices are read (the only way this grep has). */
	if (code == 'D')
		return 1;

	/* -d recurse is -r, -d skip passes directories over. */
	if (code == 'd') {
		differs = strcmp(value, "recurse");
		if (differs == 0)
			options->recursive = GREP_RECURSE;
		differs = strcmp(value, "skip");
		if (differs == 0)
			walk->skip_directories = 1;
		return 1;
	}

	/* --binary-files=text, without-match or binary. */
	differs = strcmp(value, "text");
	if (differs == 0) {
		options->binary_files = GREP_BINARY_TEXT;
		return 1;
	}

	/* -I's word. */
	differs = strcmp(value, "without-match");
	if (differs == 0) {
		options->binary_files = GREP_BINARY_SKIP;
		return 1;
	}

	/* The default's word. */
	differs = strcmp(value, "binary");
	if (differs == 0) {
		options->binary_files = GREP_BINARY_REPORT;
		return 1;
	}

	/* Any other word. */
	fprintf(stderr, "grep: unknown binary-files type\n");
	return 0;
}

/* Reads a decimal count.  Returns 0 when the text is not one. */
static int
parse_count(
	const char *text,
	unsigned long *count)
{
	unsigned long value;
	const char *cursor;

	/* At least one digit, and nothing but digits. */
	if (*text == '\0')
		return 0;
	value = 0;
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			return 0;
		value = value * 10UL + (unsigned long)(*cursor - '0');
	}

	/* Succeeded. */
	*count = value;
	return 1;
}

/* Adds a glob to a list. */
static void
add_glob(
	struct grep_globs *globs,
	const char *glob)
{
	/* Room for one more. */
	if (globs->count == globs->capacity) {
		globs->capacity = globs->capacity * 2U + 8U;
		globs->items = grep_allocate(globs->items,
		    globs->capacity * sizeof(*globs->items));
	}

	/* The glob; the argument it points into lives as long as grep. */
	globs->items[globs->count] = glob;
	globs->count++;
}

/* Adds the globs of a file, one per line.  Returns 0 after a message. */
static int
add_glob_file(
	struct grep_globs *globs,
	const char *name)
{
	char *line;
	size_t capacity;
	long length;
	FILE *stream;

	/* The file. */
	stream = fopen(name, "r");
	if (stream == NULL) {
		fprintf(stderr, "grep: %s: %s\n", name, strerror(errno));
		return 0;
	}

	/* Each line a glob, less its newline; the copies live as long as grep. */
	line = NULL;
	capacity = 0;
	for (;;) {
		length = command_read_line(stream, &line, &capacity);
		if (length <= 0)
			break;
		if (line[length - 1] == '\n')
			line[length - 1] = '\0';
		add_glob(globs, line);
		line = NULL;
		capacity = 0;
	}

	/* Succeeded: the file is done with. */
	free(line);
	fclose(stream);
	return 1;
}

/* Adds each pattern of a newline-separated list. */
static void
add_pattern_list(
	struct grep_options *options,
	const char *list,
	size_t length)
{
	const char *newline;
	size_t part;

	/* Each line of the list is a pattern. */
	for (;;) {
		/* The pattern up to the next newline, or to the end. */
		newline = memchr(list, '\n', length);
		if (newline == NULL) {
			add_pattern(options, list, length);
			return;
		}

		/* The pattern before the newline; the rest after it. */
		part = (size_t)(newline - list);
		add_pattern(options, list, part);
		list = newline + 1;
		length -= part + 1U;
	}
}

/* Adds the patterns of a file, one per line.  Returns 0 after a message. */
static int
add_pattern_file(
	struct grep_options *options,
	const char *name)
{
	char *line;
	size_t capacity;
	long length;
	FILE *stream;
	int compare;

	/* The file; - is standard input. */
	stream = stdin;
	compare = strcmp(name, "-");
	if (compare != 0)
		stream = fopen(name, "r");
	if (stream == NULL) {
		fprintf(stderr, "grep: %s: %s\n", name, strerror(errno));
		return 0;
	}

	/* Each line a pattern, less its newline. */
	line = NULL;
	capacity = 0;
	for (;;) {
		length = command_read_line(stream, &line, &capacity);
		if (length <= 0)
			break;
		if (line[length - 1] == '\n')
			length--;
		add_pattern(options, line, (size_t)length);
	}

	/* Succeeded: the file is done with. */
	free(line);
	if (stream != stdin)
		fclose(stream);
	return 1;
}

/* Adds one pattern. */
static void
add_pattern(
	struct grep_options *options,
	const char *text,
	size_t length)
{
	struct grep_pattern *pattern;

	/* Room for one more. */
	if (options->count == options->capacity) {
		options->capacity = options->capacity * 2U + 8U;
		options->patterns = grep_allocate(options->patterns,
		    options->capacity * sizeof(*options->patterns));
	}

	/* The pattern's text. */
	pattern = &options->patterns[options->count];
	memset(pattern, 0, sizeof(*pattern));
	pattern->text = grep_allocate(NULL, length + 1U);
	memcpy(pattern->text, text, length);
	pattern->text[length] = '\0';
	pattern->length = length;
	if (length == 0)
		pattern->empty = 1;
	options->count++;
}

/* Compiles the patterns as regexes (not -F).  Returns 0 after a message. */
static int
compile_patterns(
	struct grep_options *options)
{
	char message[256];
	struct grep_pattern *pattern;
	size_t index;
	int flags;
	int error;

	/* Fixed strings need no compiling. */
	if (options->mode == GREP_MODE_FIXED)
		return 1;

	/* The flags. */
	flags = 0;
	if (options->mode == GREP_MODE_EXTENDED)
		flags |= REG_EXTENDED;
	if (options->ignore_case)
		flags |= REG_ICASE;

	/* Each pattern; an empty one matches every line without a regex. */
	for (index = 0; index < options->count; index++) {
		pattern = &options->patterns[index];
		if (pattern->empty)
			continue;
		error = regcomp(&pattern->regex, pattern->text, flags);
		if (error != 0) {
			regerror(error, &pattern->regex, message,
				 sizeof(message));
			fprintf(stderr, "grep: %s\n", message);
			return 0;
		}
	}

	/* Succeeded. */
	return 1;
}

/*
 * Searches one operand: standard input for -, the files below a directory
 * with -r, or a file.
 */
static void
search_operand(
	struct walk_state *walk,
	const char *name)
{
	const struct grep_options *options;
	struct stat status;
	int compare;
	int chosen;
	int result;

	/* - is standard input. */
	options = walk->options;
	compare = strcmp(name, "-");
	if (compare == 0) {
		search_standard_input(walk);
		return;
	}

	/* The operand's type: a directory is walked with -r. */
	result = stat(name, &status);
	if (result == 0 && (status.st_mode & S_IFMT) == S_IFDIR) {
		/* --exclude-dir passes it over. */
		chosen = glob_matches(&options->exclude_dir, name, 1);
		if (chosen)
			return;

		/* -d skip passes it over without a word. */
		if (walk->skip_directories)
			return;

		/* Walked with -r; searched, which reports it, without. */
		if (options->recursive != GREP_RECURSE_NONE) {
			walk_directory(walk, name, 0);
			return;
		}
	}

	/* --include and --exclude choose among the files named. */
	chosen = file_chosen(options, name, 1);
	if (!chosen)
		return;

	/* The file, named when there are several operands or -H. */
	search_file(walk, name, options->show_names);
}

/* Searches a file, named or not, and records what came of it. */
static void
search_file(
	struct walk_state *walk,
	const char *path,
	int show_names)
{
	int descriptor;
	int result;

	/* The file. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0) {
		report(walk->options, path, errno);
		walk->error = 1;
		return;
	}

	/* Its lines. */
	result = grep_search(walk->options, descriptor, path, show_names);
	close(descriptor);

	/* What came of it; -q needs no more once a line is selected. */
	if (result < 0)
		walk->error = 1;
	if (result > 0)
		walk->selected = 1;
	if (result > 0 && walk->options->output == GREP_OUTPUT_QUIET)
		walk->done = 1;
}

/* Searches standard input, under its --label name. */
static void
search_standard_input(
	struct walk_state *walk)
{
	const char *name;
	int result;

	/* The name it goes by. */
	name = "(standard input)";
	if (walk->options->label != NULL)
		name = walk->options->label;

	/* Its lines. */
	result = grep_search(walk->options, STDIN_FILENO, name,
			     walk->options->show_names);

	/* What came of it; -q needs no more once a line is selected. */
	if (result < 0)
		walk->error = 1;
	if (result > 0)
		walk->selected = 1;
	if (result > 0 && walk->options->output == GREP_OUTPUT_QUIET)
		walk->done = 1;
}

/*
 * Walks a directory for -r: every file below it, in the order the
 * directory lists them.  implicit is set for the working directory that -r
 * searches when there is no operand, whose files are named without ./.
 */
static void
walk_directory(
	struct walk_state *walk,
	const char *path,
	int implicit)
{
	struct dirent *entry;
	DIR *directory;
	char *child;
	int dot;
	int dotdot;

	/* The directory. */
	directory = opendir(path);
	if (directory == NULL) {
		report(walk->options, path, errno);
		walk->error = 1;
		return;
	}

	/* Each entry but . and .., until -q has its answer. */
	while (!walk->done) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		dot = strcmp(entry->d_name, ".");
		dotdot = strcmp(entry->d_name, "..");
		if (dot == 0 || dotdot == 0)
			continue;

		/* The entry, by the path from the operand. */
		child = join_path(path, entry->d_name, implicit);
		walk_entry(walk, child, entry->d_name);
		free(child);
	}

	/* The directory is done with. */
	closedir(directory);
}

/*
 * Searches one entry found by the walk: a directory is walked in turn, a
 * regular file searched; a symbolic link is followed only with -R, and
 * devices, FIFOs and sockets are passed over.
 */
static void
walk_entry(
	struct walk_state *walk,
	const char *path,
	const char *base)
{
	const struct grep_options *options;
	struct stat status;
	int show_names;
	int result;
	int chosen;

	/* The entry's type, through a link with -R only. */
	options = walk->options;
	if (options->recursive == GREP_RECURSE_FOLLOW)
		result = stat(path, &status);
	else
		result = lstat(path, &status);
	if (result != 0) {
		report(options, path, errno);
		walk->error = 1;
		return;
	}

	/* A directory, unless --exclude-dir names it. */
	if ((status.st_mode & S_IFMT) == S_IFDIR) {
		chosen = glob_matches(&options->exclude_dir, base, 0);
		if (!chosen)
			walk_directory(walk, path, 0);
		return;
	}

	/* Anything but a regular file is passed over. */
	if ((status.st_mode & S_IFMT) != S_IFREG)
		return;

	/* A file that --include and --exclude choose. */
	chosen = file_chosen(options, base, 0);
	if (!chosen)
		return;

	/* Succeeded: searched, and named unless -h. */
	show_names = 1;
	if (options->names == GREP_NAMES_NEVER)
		show_names = 0;
	search_file(walk, path, show_names);
}

/* Returns a directory's path joined with an entry's name, allocated. */
static char *
join_path(
	const char *directory,
	const char *name,
	int implicit)
{
	char *path;
	size_t directory_length;
	size_t name_length;

	/* The working directory of an implicit -r adds nothing. */
	name_length = strlen(name);
	if (implicit) {
		path = grep_allocate(NULL, name_length + 1U);
		memcpy(path, name, name_length + 1U);
		return path;
	}

	/* The directory without a trailing slash, a slash, and the name. */
	directory_length = strlen(directory);
	while (directory_length > 1 && directory[directory_length - 1U] == '/')
		directory_length--;
	path = grep_allocate(NULL, directory_length + name_length + 2U);
	memcpy(path, directory, directory_length);
	path[directory_length] = '/';
	if (directory_length == 1 && directory[0] == '/')
		directory_length = 0;

	/* Succeeded: the name after the slash. */
	memcpy(path + directory_length + 1U, name, name_length + 1U);
	return path;
}

/*
 * Reports whether a glob of a list matches a name.  With suffixes, as for
 * an operand, a glob may match any part of the name that follows a slash.
 */
static int
glob_matches(
	const struct grep_globs *globs,
	const char *name,
	int suffixes)
{
	const char *part;
	size_t index;
	int result;

	/* Each glob, against the name and, with suffixes, each tail of it. */
	for (index = 0; index < globs->count; index++) {
		part = name;
		for (;;) {
			result = fnmatch(globs->items[index], part, 0);
			if (result == 0)
				return 1;

			/* The next part after a slash, when suffixes count. */
			if (!suffixes)
				break;
			part = strchr(part, '/');
			if (part == NULL)
				break;
			part++;
		}
	}

	/* No glob matches. */
	return 0;
}

/*
 * Reports whether --include and --exclude let a file be searched: no
 * --exclude glob matches it, and an --include glob does when there are any.
 */
static int
file_chosen(
	const struct grep_options *options,
	const char *name,
	int suffixes)
{
	int matched;

	/* An excluded file. */
	matched = glob_matches(&options->exclude, name, suffixes);
	if (matched)
		return 0;

	/* With --include, only the files it names. */
	if (options->include.count == 0)
		return 1;
	matched = glob_matches(&options->include, name, suffixes);
	if (!matched)
		return 0;

	/* Succeeded: an included file. */
	return 1;
}

/* Reports a file that could not be searched, unless -s. */
static void
report(
	const struct grep_options *options,
	const char *name,
	int error)
{
	/* Messages about files are what -s silences. */
	if (options->no_messages)
		return;
	fflush(stdout);
	fprintf(stderr, "grep: %s: %s\n", name, strerror(error));
}

/* Writes the version and ends grep. */
static void
version(
	void)
{
	/* The name and where it comes from. */
	printf("grep (" ZEDBSD_OS_NAME ") 1.0\n");
	exit(0);
}

/* Reports the usage (on standard output for --help) and ends grep. */
static void
usage(
	int status)
{
	FILE *stream;

	/* --help writes to standard output and succeeds. */
	stream = stderr;
	if (status == 0)
		stream = stdout;

	/* The two forms and the options. */
	fprintf(stream, "usage: grep [-EFGHILRZabchilnoqrsvwxz] [-A num] "
		"[-B num] [-C num] [-m num]\n"
		"            [--include=glob] [--exclude=glob] "
		"[--exclude-dir=glob] [--label=name]\n"
		"            -e pattern_list [-f pattern_file] [file...]\n"
		"       grep [options] pattern_list [file...]\n");
	exit(status);
}
