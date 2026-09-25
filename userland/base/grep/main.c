/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Searches files for lines that match patterns (POSIX XCU grep).
 *
 *	grep [-E|-F] [-c|-l|-q] [-insvx] -e pattern_list [-e ...] [-f file]... [file...]
 *	grep [-E|-F] [-c|-l|-q] [-insvx] pattern_list [file...]
 *
 * A pattern list is patterns separated by newlines; a line is selected when
 * any pattern matches it.  Patterns are basic regular expressions, extended
 * ones with -E, or fixed strings with -F.  An empty pattern matches every
 * line.  With -x a pattern must match the whole line: the regex matcher is
 * leftmost-longest, so a whole-line match is one that starts at the start
 * of the line and ends at its end.
 *
 * The exit status is 0 when a line was selected, 1 when none was, and 2 for
 * an error (a file that could not be read, a bad pattern); with -q a
 * selected line makes it 0 whatever else happened.
 */

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How patterns are read. */
#define MODE_BASIC	0	/* basic regular expressions */
#define MODE_EXTENDED	1	/* -E: extended regular expressions */
#define MODE_FIXED	2	/* -F: fixed strings */

/* What is written for the selected lines. */
#define OUTPUT_LINES	0	/* the lines */
#define OUTPUT_COUNT	1	/* -c: how many */
#define OUTPUT_NAMES	2	/* -l: the names of the files with any */
#define OUTPUT_QUIET	3	/* -q: nothing */

/* A pattern: its text, and the compiled regex when it is one. */
struct pattern {
	char *text;
	size_t length;
	regex_t regex;
	int empty;
};

/* The options and the patterns. */
struct options {
	int mode;
	int output;
	int ignore_case;
	int line_numbers;
	int no_messages;
	int invert;
	int whole_line;
	int show_names;

	struct pattern *patterns;
	size_t count;
	size_t capacity;
	int have_patterns;
};

/* A line being read. */
struct line {
	char *data;
	size_t length;
	size_t capacity;
};

static int read_options(int argc, char **argv, struct options *options);
static int option_letters(int argc, char **argv, int *index, struct options *options);
static const char *option_argument(int argc, char **argv, int *index, const char *rest);
static void add_pattern_list(struct options *options, const char *list, size_t length);
static void add_pattern_file(struct options *options, const char *name);
static void add_pattern(struct options *options, const char *text, size_t length);
static int compile_patterns(struct options *options);
static int search(struct options *options, FILE *stream, const char *name);
static int line_selected(const struct options *options, const struct line *line);
static int pattern_matches(const struct options *options, const struct pattern *pattern, const struct line *line);
static int fixed_matches(const struct options *options, const struct pattern *pattern, const struct line *line);
static int same_text(const char *left, const char *right, size_t length, int ignore_case);
static int read_line(FILE *stream, struct line *line);
static void print_line(const struct options *options, const char *name, unsigned long number, const struct line *line);
static int lower(int value);
static void *allocate(void *memory, size_t size);
static void usage(void);

/*
 * Runs grep.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	FILE *stream;
	const char *name;
	int index;
	int selected;
	int error;
	int status;
	int files;
	int compare;
	int ok;

	/* The options, and the pattern list when no -e or -f gave one. */
	memset(&options, 0, sizeof(options));
	index = read_options(argc, argv, &options);
	if (!options.have_patterns) {
		if (index >= argc)
			usage();
		add_pattern_list(&options, argv[index], strlen(argv[index]));
		index++;
	}

	/* The patterns, compiled. */
	ok = compile_patterns(&options);
	if (!ok)
		return 2;

	/* Names go before the lines when there are several files. */
	files = argc - index;
	if (files > 1)
		options.show_names = 1;

	/* Standard input when there is no file. */
	selected = 0;
	error = 0;
	if (files == 0)
		selected = search(&options, stdin, "(standard input)");

	/* Each file named. */
	for (; index < argc; index++) {
		/* - is standard input. */
		name = argv[index];
		compare = strcmp(name, "-");
		if (compare == 0) {
			selected |= search(&options, stdin, "(standard input)");
			continue;
		}

		/* A file, or a message (unless -s) and an error. */
		stream = fopen(name, "r");
		if (stream == NULL) {
			if (!options.no_messages)
				fprintf(stderr, "grep: %s: %s\n", name, strerror(errno));
			error = 1;
			continue;
		}

		/* The lines of the file. */
		selected |= search(&options, stream, name);
		fclose(stream);

		/* -q stops at the first selected line. */
		if (selected && options.output == OUTPUT_QUIET)
			break;
	}

	/* Succeeded: the status. */
	fflush(stdout);
	status = 1;
	if (selected)
		status = 0;
	if (error && !(selected && options.output == OUTPUT_QUIET))
		status = 2;
	return status;
}

/* Reads the options; returns the index of the first operand. */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	const char *word;
	int index;
	int ok;

	/* Each option word. */
	for (index = 1; index < argc; index++) {
		/* An operand, or - alone, ends the options. */
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;

		/* -- ends them too. */
		if (word[1] == '-' && word[2] == '\0') {
			index++;
			break;
		}

		/* The letters of the word. */
		ok = option_letters(argc, argv, &index, options);
		if (!ok)
			usage();
	}

	/* Succeeded: the first operand. */
	return index;
}

/*
 * Reads the letters of one option word; -e and -f take the rest of the
 * word or the next word.  Returns 0 for an unknown letter.
 */
static int
option_letters(
	int argc,
	char **argv,
	int *index,
	struct options *options)
{
	const char *letter;
	const char *argument;

	/* Each letter of the word. */
	for (letter = argv[*index] + 1; *letter != '\0'; letter++) {
		switch (*letter) {
		case 'E':
			options->mode = MODE_EXTENDED;
			break;
		case 'F':
			options->mode = MODE_FIXED;
			break;
		case 'c':
			options->output = OUTPUT_COUNT;
			break;
		case 'l':
			options->output = OUTPUT_NAMES;
			break;
		case 'q':
			options->output = OUTPUT_QUIET;
			break;
		case 'i':
			options->ignore_case = 1;
			break;
		case 'n':
			options->line_numbers = 1;
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
		case 'e':
			/* A pattern list. */
			argument = option_argument(argc, argv, index, letter + 1);
			add_pattern_list(options, argument, strlen(argument));
			options->have_patterns = 1;
			return 1;
		case 'f':
			/* A file of patterns. */
			argument = option_argument(argc, argv, index, letter + 1);
			add_pattern_file(options, argument);
			options->have_patterns = 1;
			return 1;
		default:
			fprintf(stderr, "grep: invalid option -- '%c'\n", *letter);
			return 0;
		}
	}

	/* Succeeded. */
	return 1;
}

/* Returns an option's argument: the rest of its word, or the next word. */
static const char *
option_argument(
	int argc,
	char **argv,
	int *index,
	const char *rest)
{
	/* The rest of the word. */
	if (*rest != '\0')
		return rest;

	/* The next word. */
	if (*index + 1 >= argc) {
		fprintf(stderr, "grep: option requires an argument\n");
		usage();
	}

	/* The next word is the argument. */
	(*index)++;

	/* Succeeded. */
	return argv[*index];
}

/* Adds each pattern of a newline-separated list. */
static void
add_pattern_list(
	struct options *options,
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

/* Adds the patterns of a file, one per line. */
static void
add_pattern_file(
	struct options *options,
	const char *name)
{
	struct line line;
	FILE *stream;
	int compare;
	int read;

	/* The file; - is standard input. */
	stream = stdin;
	compare = strcmp(name, "-");
	if (compare != 0)
		stream = fopen(name, "r");
	if (stream == NULL) {
		fprintf(stderr, "grep: %s: %s\n", name, strerror(errno));
		exit(2);
	}

	/* Each line a pattern. */
	memset(&line, 0, sizeof(line));
	for (;;) {
		read = read_line(stream, &line);
		if (!read)
			break;
		add_pattern(options, line.data, line.length);
	}

	/* The file is done with. */
	free(line.data);
	if (stream != stdin)
		fclose(stream);
}

/* Adds one pattern. */
static void
add_pattern(
	struct options *options,
	const char *text,
	size_t length)
{
	struct pattern *pattern;

	/* Room for one more. */
	if (options->count == options->capacity) {
		options->capacity = options->capacity * 2U + 8U;
		options->patterns = allocate(options->patterns,
		    options->capacity * sizeof(*options->patterns));
	}

	/* The pattern's text. */
	pattern = &options->patterns[options->count];
	memset(pattern, 0, sizeof(*pattern));
	pattern->text = allocate(NULL, length + 1U);
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
	struct options *options)
{
	char message[256];
	struct pattern *pattern;
	size_t index;
	int flags;
	int error;

	/* Fixed strings need no compiling. */
	if (options->mode == MODE_FIXED)
		return 1;

	/* The flags. */
	flags = 0;
	if (options->mode == MODE_EXTENDED)
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
 * Searches one input, writing what the options ask for.  Returns 1 when a
 * line was selected.
 */
static int
search(
	struct options *options,
	FILE *stream,
	const char *name)
{
	struct line line;
	unsigned long number;
	unsigned long count;
	int selected;
	int read;

	/* No lines yet. */
	memset(&line, 0, sizeof(line));
	number = 0;
	count = 0;
	for (;;) {
		/* The next line. */
		read = read_line(stream, &line);
		if (!read)
			break;
		number++;

		/* Selected: matched, or not matched with -v. */
		selected = line_selected(options, &line);
		if (!selected)
			continue;
		count++;

		/* -q and -l need only the first. */
		if (options->output == OUTPUT_QUIET)
			break;
		if (options->output == OUTPUT_NAMES)
			break;
		if (options->output == OUTPUT_LINES)
			print_line(options, name, number, &line);
	}

	/* The line's buffer goes. */
	free(line.data);

	/* The count, or the name. */
	if (options->output == OUTPUT_COUNT) {
		if (options->show_names)
			printf("%s:", name);
		printf("%lu\n", count);
	}

	/* -l writes the name of a file with a selected line. */
	if (options->output == OUTPUT_NAMES && count > 0)
		printf("%s\n", name);

	/* Succeeded: whether any line was selected. */
	if (count > 0)
		return 1;
	return 0;
}

/* Reports whether a line is selected: any pattern matches (none, with -v). */
static int
line_selected(
	const struct options *options,
	const struct line *line)
{
	size_t index;
	int matched;

	/* Any pattern. */
	matched = 0;
	for (index = 0; index < options->count; index++) {
		matched = pattern_matches(options, &options->patterns[index],
					  line);
		if (matched)
			break;
	}

	/* -v turns the answer round. */
	if (options->invert && matched)
		return 0;
	if (options->invert)
		return 1;

	/* Succeeded: whether a pattern matched. */
	return matched;
}

/* Reports whether one pattern matches a line (the whole of it, with -x). */
static int
pattern_matches(
	const struct options *options,
	const struct pattern *pattern,
	const struct line *line)
{
	regmatch_t match;
	int result;
	int fixed;

	/* An empty pattern matches every line (only empty ones with -x). */
	if (pattern->empty) {
		if (options->whole_line && line->length != 0)
			return 0;
		return 1;
	}

	/* A fixed string. */
	if (options->mode == MODE_FIXED) {
		fixed = fixed_matches(options, pattern, line);
		return fixed;
	}

	/* A regex; with -x the leftmost-longest match must be the line. */
	result = regexec(&pattern->regex, line->data, 1, &match, 0);
	if (result != 0)
		return 0;
	if (!options->whole_line)
		return 1;
	if (match.rm_so != 0)
		return 0;
	if ((size_t)match.rm_eo != line->length)
		return 0;

	/* Succeeded: the whole line. */
	return 1;
}

/* Reports whether a fixed string is in a line (is the line, with -x). */
static int
fixed_matches(
	const struct options *options,
	const struct pattern *pattern,
	const struct line *line)
{
	size_t start;
	int same;
	int whole;

	/* -x: the line is the string. */
	if (options->whole_line) {
		if (line->length != pattern->length)
			return 0;
		whole = same_text(line->data, pattern->text, pattern->length, options->ignore_case);
		return whole;
	}

	/* Anywhere in the line. */
	if (pattern->length > line->length)
		return 0;
	for (start = 0; start + pattern->length <= line->length; start++) {
		same = same_text(line->data + start, pattern->text,
				 pattern->length, options->ignore_case);
		if (same)
			return 1;
	}

	/* Not found. */
	return 0;
}

/* Compares length bytes, ignoring case when asked. */
static int
same_text(
	const char *left,
	const char *right,
	size_t length,
	int ignore_case)
{
	size_t index;
	int a;
	int b;

	/* Each byte in turn. */
	for (index = 0; index < length; index++) {
		/* Each byte, folded when case is ignored. */
		a = (unsigned char)left[index];
		b = (unsigned char)right[index];
		if (ignore_case) {
			a = lower(a);
			b = lower(b);
		}

		/* Bytes that differ. */
		if (a != b)
			return 0;
	}

	/* Succeeded: the same. */
	return 1;
}

/*
 * Reads one line, without its newline.  A last line without a newline is
 * still a line.  Returns 0 at the end of the input.
 */
static int
read_line(
	FILE *stream,
	struct line *line)
{
	int value;
	int newline;

	/* Bytes up to the newline. */
	line->length = 0;
	newline = 0;
	for (;;) {
		/* The next byte. */
		value = getc(stream);
		if (value == EOF)
			break;
		if (value == '\n') {
			newline = 1;
			break;
		}

		/* Room for it and a NUL. */
		if (line->length + 2U > line->capacity) {
			line->capacity = line->capacity * 2U + 128U;
			line->data = allocate(line->data, line->capacity);
		}

		/* The byte. */
		line->data[line->length] = (char)value;
		line->length++;
	}

	/* The end of the input. */
	if (line->length == 0 && !newline)
		return 0;

	/* Succeeded: the line, terminated. */
	if (line->data == NULL) {
		line->capacity = 128U;
		line->data = allocate(NULL, line->capacity);
	}

	/* Succeeded: the line, ending with a NUL. */
	line->data[line->length] = '\0';
	return 1;
}

/* Writes a selected line, after its file name and number when asked. */
static void
print_line(
	const struct options *options,
	const char *name,
	unsigned long number,
	const struct line *line)
{
	/* The name and the number. */
	if (options->show_names)
		printf("%s:", name);
	if (options->line_numbers)
		printf("%lu:", number);

	/* The line. */
	fwrite(line->data, 1, line->length, stdout);
	putchar('\n');
}

/* Folds an ASCII letter to lower case. */
static int
lower(
	int value)
{
	/* A to Z. */
	if (value >= 'A' && value <= 'Z')
		return value - 'A' + 'a';
	return value;
}

/* Allocates or resizes memory, ending grep when there is none. */
static void *
allocate(
	void *memory,
	size_t size)
{
	void *result;

	/* The memory. */
	result = realloc(memory, size);
	if (result == NULL) {
		fprintf(stderr, "grep: out of memory\n");
		exit(2);
	}

	/* Succeeded. */
	return result;
}

/* Reports the usage and ends grep. */
static void
usage(
	void)
{
	/* The two forms. */
	fprintf(stderr, "usage: grep [-E|-F] [-c|-l|-q] [-insvx] "
		"-e pattern_list [-f pattern_file] [file...]\n"
		"       grep [-E|-F] [-c|-l|-q] [-insvx] pattern_list "
		"[file...]\n");
	exit(2);
}
