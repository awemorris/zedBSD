/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Sorts, merges or checks text files (POSIX XCU sort).
 *
 *	sort [-m] [-o output] [-bdfinru] [-t char] [-k keydef]... [file...]
 *	sort -c|-C [-bdfinru] [-t char] [-k keydef] [file]
 *
 * Lines are compared by their keys in order; lines whose keys are all equal
 * are compared as whole lines (bytes, C locale), unless -u, which keeps only
 * the first of lines with equal keys.  With no -k the key is the whole line.
 *
 * A keydef is field_start[type][,field_end[type]], each field.char counted
 * from 1; an end char of 0 (or none) is the end of the field.  Without -t a
 * field is a run of non-blanks with the blanks before it; with -t fields
 * are separated by the character.  The types are b (leading blanks
 * ignored), d (only blanks and alphanumerics count), f (case folded), i
 * (only printable characters count), n (numeric), r (reversed); a key with
 * no type of its own takes the global options.
 *
 * -m merges files that are already sorted; since sorting everything gives
 * the same order, it is done the same way.  The output (-o) is written only
 * after all input is read, so it may be one of the inputs.
 *
 * GNU's extensions: the types V (version), h (human numbers, 2K < 1M), g
 * (general numbers, with exponents) and M (month names), -s (equal keys keep
 * their input order), -z (lines end with a NUL byte), the long options, and
 * options after operands (unless POSIXLY_CORRECT is set).
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/kern/os-name.h"

/* The ordering modifiers of a key (and the global ones). */
#define KEY_BLANKS	0x01	/* b */
#define KEY_DICTIONARY	0x02	/* d */
#define KEY_FOLD	0x04	/* f */
#define KEY_PRINTABLE	0x08	/* i */
#define KEY_NUMERIC	0x10	/* n */
#define KEY_REVERSE	0x20	/* r */
#define KEY_VERSION	0x40	/* V (GNU) */
#define KEY_HUMAN	0x80	/* h (GNU) */
#define KEY_GENERAL	0x100	/* g (GNU) */
#define KEY_MONTH	0x200	/* M (GNU) */

/* The codes of the long options that have no letter. */
#define OPTION_SORT	256
#define OPTION_CHECK	257
#define OPTION_IGNORED	258
#define OPTION_VERSION	259
#define OPTION_HELP	260

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option sort_long_options[] = {
	{"buffer-size", COMMAND_VALUE_REQUIRED, 'S'},
	{"check", COMMAND_VALUE_OPTIONAL, OPTION_CHECK},
	{"debug", COMMAND_VALUE_NONE, OPTION_IGNORED},
	{"dictionary-order", COMMAND_VALUE_NONE, 'd'},
	{"field-separator", COMMAND_VALUE_REQUIRED, 't'},
	{"general-numeric-sort", COMMAND_VALUE_NONE, 'g'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"human-numeric-sort", COMMAND_VALUE_NONE, 'h'},
	{"ignore-case", COMMAND_VALUE_NONE, 'f'},
	{"ignore-leading-blanks", COMMAND_VALUE_NONE, 'b'},
	{"ignore-nonprinting", COMMAND_VALUE_NONE, 'i'},
	{"key", COMMAND_VALUE_REQUIRED, 'k'},
	{"merge", COMMAND_VALUE_NONE, 'm'},
	{"month-sort", COMMAND_VALUE_NONE, 'M'},
	{"numeric-sort", COMMAND_VALUE_NONE, 'n'},
	{"output", COMMAND_VALUE_REQUIRED, 'o'},
	{"parallel", COMMAND_VALUE_REQUIRED, OPTION_IGNORED},
	{"reverse", COMMAND_VALUE_NONE, 'r'},
	{"sort", COMMAND_VALUE_REQUIRED, OPTION_SORT},
	{"stable", COMMAND_VALUE_NONE, 's'},
	{"temporary-directory", COMMAND_VALUE_REQUIRED, 'T'},
	{"unique", COMMAND_VALUE_NONE, 'u'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{"version-sort", COMMAND_VALUE_NONE, 'V'},
	{"zero-terminated", COMMAND_VALUE_NONE, 'z'},
	{NULL, 0, 0}
};

/* A key: where it starts and ends, and how it is compared. */
struct key {
	unsigned long start_field;
	unsigned long start_char;
	unsigned long end_field;	/* 0: the end of the line */
	unsigned long end_char;		/* 0: the end of the field */
	int flags;
	int start_blanks;		/* b on the start */
	int end_blanks;			/* b on the end */
};

/* One line of input, and where it was in the input (for -s). */
struct line {
	char *text;
	size_t length;
	size_t order;
};

/* The options, the keys and the lines. */
struct sort {
	int check;		/* -c: 1, -C: 2 */
	int unique;
	int stable;		/* -s: no last-resort comparison */
	int delimiter;		/* the end of a line: newline, or NUL (-z) */
	int separator;		/* -t, or -1 for blanks */
	int flags;		/* the global modifiers */
	const char *output;
	struct key *keys;
	size_t key_count;
	size_t key_capacity;
	struct line *lines;
	size_t line_count;
	size_t line_capacity;
};

/* A part of a line a key selects. */
struct span {
	const char *text;
	size_t length;
};

static struct sort *sorting;

static int read_options(int argc, char **argv, struct sort *sort);
static void apply_option(struct sort *sort, int code, const char *value);
static void apply_separator(struct sort *sort, const char *value);
static void apply_sort_word(struct sort *sort, const char *word);
static void apply_check_word(struct sort *sort, const char *word);
static int modifier_flag(char letter);
static void parse_key(struct sort *sort, const char *text);
static const char *parse_position(const char *cursor, unsigned long *field, unsigned long *character, int *flags, int *blanks);
static void read_input(struct sort *sort, FILE *stream);
static void add_line(struct sort *sort, const char *text, size_t length);
static int compare_lines(const void *left, const void *right);
static int compare_keys(const struct sort *sort, const struct line *left, const struct line *right);
static int compare_whole(const struct line *left, const struct line *right);
static struct span key_span(const struct sort *sort, const struct key *key, const struct line *line);
static size_t field_start(const struct sort *sort, const struct line *line, unsigned long field);
static size_t field_end(const struct sort *sort, const struct line *line, size_t start);
static size_t skip_blanks(const struct line *line, size_t position, size_t end);
static int compare_spans(struct span left, struct span right, int flags);
static int compare_text(struct span left, struct span right, int flags);
static int compare_version(struct span left, struct span right);
static int version_dots(struct span span);
static size_t version_prefix(struct span span);
static int compare_version_part(const char *left, size_t left_length, const char *right, size_t right_length);
static int version_order(const char *text, size_t position, size_t length);
static int compare_human(struct span left, struct span right);
static int unit_order(struct span span);
static int compare_general(struct span left, struct span right);
static int general_value(struct span span, double *value);
static int compare_month(struct span left, struct span right);
static int month_number(struct span span);
static int is_digit(char value);
static int is_letter(char value);
static int compare_numbers(struct span left, struct span right);
static int number_parts(struct span span, int *negative, const char **integer, size_t *integer_length, const char **fraction, size_t *fraction_length);
static int compare_magnitudes(const char *left_integer, size_t left_integer_length, const char *left_fraction, size_t left_fraction_length, const char *right_integer, size_t right_integer_length, const char *right_fraction, size_t right_fraction_length);
static int ignored(unsigned char value, int flags);
static int fold(unsigned char value, int flags);
static int is_blank(char value);
static int check_order(struct sort *sort, const char *name);
static void write_output(struct sort *sort);
static void *allocate(void *memory, size_t size);
static void usage(void);

/*
 * Runs sort.
 */
int
main(
	int argc,
	char **argv)
{
	static struct sort sort;
	FILE *stream;
	const char *name;
	int count;
	int first;
	int index;
	int compare;
	int status;

	/* The options; the comparison reads them through sorting. */
	memset(&sort, 0, sizeof(sort));
	sort.separator = -1;
	sort.delimiter = '\n';
	count = read_options(argc, argv, &sort);
	first = 1;
	argc = first + count;
	sorting = &sort;

	/* Every input, whole; standard input when there is none. */
	name = "-";
	if (count == 0)
		read_input(&sort, stdin);
	for (index = first; index < argc; index++) {
		name = argv[index];
		stream = stdin;
		compare = strcmp(name, "-");
		if (compare != 0)
			stream = fopen(name, "r");
		if (stream == NULL) {
			fprintf(stderr, "sort: cannot read: %s: %s\n", name,
				strerror(errno));
			return 2;
		}

		/* The lines of the file. */
		read_input(&sort, stream);
		if (stream != stdin)
			fclose(stream);
	}

	/* -c and -C check the order instead of sorting. */
	if (sort.check) {
		status = check_order(&sort, name);
		return status;
	}

	/* Succeeded: sorted and written. */
	qsort(sort.lines, sort.line_count, sizeof(*sort.lines), compare_lines);
	write_output(&sort);
	return 0;
}

/*
 * Reads the options; returns the number of operands, which are left in
 * argv from argv[1] on.
 */
static int
read_options(
	int argc,
	char **argv,
	struct sort *sort)
{
	struct command_options scan;
	int code;

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "sort";
	scan.letters = "bdfghiMnrVcCmsuzk:o:t:S:T:";
	scan.names = sort_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;
		if (code == COMMAND_OPTION_ERROR)
			usage();
		apply_option(sort, code, scan.value);
	}

	/* Succeeded: the operands follow argv[0]. */
	return scan.operand_count;
}

/* Applies one option. */
static void
apply_option(
	struct sort *sort,
	int code,
	const char *value)
{
	int flag;

	/* The modifiers that apply to every key. */
	flag = modifier_flag((char)code);
	if (code < 256 && flag != 0) {
		sort->flags |= flag;
		return;
	}

	/* The option of its code. */
	switch (code) {
	case 'c':
		sort->check = 1;
		break;
	case 'C':
		sort->check = 2;
		break;
	case OPTION_CHECK:
		apply_check_word(sort, value);
		break;
	case 'u':
		sort->unique = 1;
		break;
	case 's':
		sort->stable = 1;
		break;
	case 'z':
		sort->delimiter = '\0';
		break;
	case 'o':
		sort->output = value;
		break;
	case 't':
		apply_separator(sort, value);
		break;
	case 'k':
		parse_key(sort, value);
		break;
	case OPTION_SORT:
		apply_sort_word(sort, value);
		break;
	case 'm':
	case 'S':
	case 'T':
	case OPTION_IGNORED:
		/* -m is a plain sort; memory, temporary files and threads are sort's own. */
		break;
	case OPTION_VERSION:
		printf("sort (" ZEDBSD_OS_NAME ") 1.0\n");
		exit(0);
	case OPTION_HELP:
		usage();
		break;
	default:
		usage();
	}
}

/* Sets the field separator of -t, which is one character. */
static void
apply_separator(
	struct sort *sort,
	const char *value)
{
	/* One character only. */
	if (value[0] == '\0' || value[1] != '\0') {
		fprintf(stderr, "sort: multi-character tab '%s'\n", value);
		exit(2);
	}

	/* The separator. */
	sort->separator = (unsigned char)value[0];
}

/* Applies --sort=WORD, another name for a type. */
static void
apply_sort_word(
	struct sort *sort,
	const char *word)
{
	int differs;

	/* Each word and its type. */
	differs = strcmp(word, "general-numeric");
	if (differs == 0) {
		sort->flags |= KEY_GENERAL;
		return;
	}

	/* human-numeric. */
	differs = strcmp(word, "human-numeric");
	if (differs == 0) {
		sort->flags |= KEY_HUMAN;
		return;
	}

	/* month. */
	differs = strcmp(word, "month");
	if (differs == 0) {
		sort->flags |= KEY_MONTH;
		return;
	}

	/* numeric. */
	differs = strcmp(word, "numeric");
	if (differs == 0) {
		sort->flags |= KEY_NUMERIC;
		return;
	}

	/* version. */
	differs = strcmp(word, "version");
	if (differs == 0) {
		sort->flags |= KEY_VERSION;
		return;
	}

	/* Any other word. */
	fprintf(stderr, "sort: invalid argument '%s' for '--sort'\n", word);
	exit(2);
}

/* Applies --check[=WORD]: quiet and silent are -C, the rest -c. */
static void
apply_check_word(
	struct sort *sort,
	const char *word)
{
	int quiet;
	int silent;

	/* No word, or diagnose-first: -c. */
	sort->check = 1;
	if (word == NULL)
		return;

	/* quiet or silent: -C. */
	quiet = strcmp(word, "quiet");
	silent = strcmp(word, "silent");
	if (quiet == 0 || silent == 0)
		sort->check = 2;
}

/* Returns the flag of a modifier letter, or 0 when it is none. */
static int
modifier_flag(
	char letter)
{
	/* Each modifier. */
	switch (letter) {
	case 'b':
		return KEY_BLANKS;
	case 'd':
		return KEY_DICTIONARY;
	case 'f':
		return KEY_FOLD;
	case 'i':
		return KEY_PRINTABLE;
	case 'n':
		return KEY_NUMERIC;
	case 'r':
		return KEY_REVERSE;
	case 'V':
		return KEY_VERSION;
	case 'h':
		return KEY_HUMAN;
	case 'g':
		return KEY_GENERAL;
	case 'M':
		return KEY_MONTH;
	default:
		break;
	}

	/* Not a modifier. */
	return 0;
}

/* Parses a keydef: field_start[type][,field_end[type]]. */
static void
parse_key(
	struct sort *sort,
	const char *text)
{
	struct key key;
	const char *cursor;

	/* The start. */
	memset(&key, 0, sizeof(key));
	key.start_char = 1;
	cursor = parse_position(text, &key.start_field, &key.start_char,
				&key.flags, &key.start_blanks);
	if (key.start_field == 0 || key.start_char == 0) {
		fprintf(stderr, "sort: invalid key '%s'\n", text);
		exit(2);
	}

	/* The end, after a comma. */
	if (*cursor == ',') {
		cursor++;
		cursor = parse_position(cursor, &key.end_field, &key.end_char,
					&key.flags, &key.end_blanks);
		if (key.end_field == 0) {
			fprintf(stderr, "sort: invalid key '%s'\n", text);
			exit(2);
		}
	}

	/* Nothing may follow the key. */
	if (*cursor != '\0') {
		fprintf(stderr, "sort: invalid key '%s'\n", text);
		exit(2);
	}

	/* Kept. */
	if (sort->key_count == sort->key_capacity) {
		sort->key_capacity = sort->key_capacity * 2U + 4U;
		sort->keys = allocate(sort->keys,
		    sort->key_capacity * sizeof(*sort->keys));
	}

	/* The key, added. */
	sort->keys[sort->key_count] = key;
	sort->key_count++;
}

/* Parses field[.char][modifiers]. */
static const char *
parse_position(
	const char *cursor,
	unsigned long *field,
	unsigned long *character,
	int *flags,
	int *blanks)
{
	int flag;

	/* The field. */
	*field = 0;
	while (*cursor >= '0' && *cursor <= '9') {
		*field = *field * 10UL + (unsigned long)(*cursor - '0');
		cursor++;
	}

	/* The character, after a dot. */
	if (*cursor == '.') {
		cursor++;
		*character = 0;
		while (*cursor >= '0' && *cursor <= '9') {
			*character = *character * 10UL +
			    (unsigned long)(*cursor - '0');
			cursor++;
		}
	}

	/* The modifiers; b belongs to this end only. */
	for (;;) {
		flag = modifier_flag(*cursor);
		if (flag == 0)
			break;
		if (flag == KEY_BLANKS)
			*blanks = 1;
		else
			*flags |= flag;
		cursor++;
	}

	/* Succeeded: after the position. */
	return cursor;
}

/* Reads every line of an input. */
static void
read_input(
	struct sort *sort,
	FILE *stream)
{
	char *text;
	size_t length;
	size_t capacity;
	int value;

	/* No line yet. */
	text = NULL;
	length = 0;
	capacity = 0;
	for (;;) {
		/* The next byte; a newline (NUL with -z) or the end ends a line. */
		value = getc(stream);
		if (value == EOF || value == sort->delimiter) {
			if (value == sort->delimiter || length > 0)
				add_line(sort, text, length);
			length = 0;
			if (value == EOF)
				break;
			continue;
		}

		/* Room for it. */
		if (length + 1U > capacity) {
			capacity = capacity * 2U + 128U;
			text = allocate(text, capacity);
		}

		/* The byte. */
		text[length] = (char)value;
		length++;
	}

	/* The line's buffer goes. */
	free(text);
}

/* Adds a copy of a line. */
static void
add_line(
	struct sort *sort,
	const char *text,
	size_t length)
{
	struct line *line;

	/* Room for one more. */
	if (sort->line_count == sort->line_capacity) {
		sort->line_capacity = sort->line_capacity * 2U + 256U;
		sort->lines = allocate(sort->lines,
		    sort->line_capacity * sizeof(*sort->lines));
	}

	/* The copy, terminated. */
	line = &sort->lines[sort->line_count];
	line->text = allocate(NULL, length + 1U);
	if (length > 0)
		memcpy(line->text, text, length);
	line->text[length] = '\0';
	line->length = length;
	line->order = sort->line_count;
	sort->line_count++;
}

/* Compares two lines for qsort: the keys, then the whole line. */
static int
compare_lines(
	const void *left,
	const void *right)
{
	const struct line *a;
	const struct line *b;
	int result;

	/* The two lines. */
	a = left;
	b = right;

	/* The keys. */
	result = compare_keys(sorting, a, b);
	if (result != 0 || sorting->unique)
		return result;

	/* -s: equal keys keep the order of the input. */
	if (sorting->stable) {
		if (a->order < b->order)
			return -1;
		if (a->order > b->order)
			return 1;
		return 0;
	}

	/* The whole line as a last resort, reversed with a global -r. */
	result = compare_whole(a, b);
	if ((sorting->flags & KEY_REVERSE) != 0)
		return -result;
	return result;
}

/* Compares two lines by their keys (the whole line when there is none). */
static int
compare_keys(
	const struct sort *sort,
	const struct line *left,
	const struct line *right)
{
	struct span a;
	struct span b;
	struct key whole;
	const struct key *key;
	size_t index;
	int flags;
	int result;
	int whole_result;

	/* No -k: the whole line with the global modifiers. */
	if (sort->key_count == 0) {
		memset(&whole, 0, sizeof(whole));
		whole.start_field = 1;
		whole.start_char = 1;
		whole.flags = sort->flags;
		if ((sort->flags & KEY_BLANKS) != 0)
			whole.start_blanks = 1;
		a = key_span(sort, &whole, left);
		b = key_span(sort, &whole, right);
		whole_result = compare_spans(a, b, sort->flags);
		return whole_result;
	}

	/* Each key in order. */
	for (index = 0; index < sort->key_count; index++) {
		/* A key without its own modifiers takes the global ones. */
		key = &sort->keys[index];
		flags = key->flags;
		if (flags == 0 && !key->start_blanks && !key->end_blanks)
			flags = sort->flags;
		a = key_span(sort, key, left);
		b = key_span(sort, key, right);
		result = compare_spans(a, b, flags);
		if (result != 0)
			return result;
	}

	/* Equal keys. */
	return 0;
}

/* Compares two whole lines as bytes. */
static int
compare_whole(
	const struct line *left,
	const struct line *right)
{
	size_t length;
	int result;

	/* The common part, then the shorter first. */
	length = left->length;
	if (right->length < length)
		length = right->length;
	result = memcmp(left->text, right->text, length);
	if (result != 0)
		return result;
	if (left->length < right->length)
		return -1;
	if (left->length > right->length)
		return 1;
	return 0;
}

/* Returns the part of a line a key selects. */
static struct span
key_span(
	const struct sort *sort,
	const struct key *key,
	const struct line *line)
{
	struct span span;
	size_t start;
	size_t end;
	size_t end_field_start;
	int blanks;

	/* The start: the field, blanks skipped with b, then the character. */
	start = field_start(sort, line, key->start_field);
	blanks = key->start_blanks || ((sort->flags & KEY_BLANKS) != 0 &&
				       key->flags == 0 && !key->end_blanks);
	if (blanks)
		start = skip_blanks(line, start, line->length);
	start += key->start_char - 1U;
	if (start > line->length)
		start = line->length;

	/* The end: the end of the line, of the field, or a character in it. */
	end = line->length;
	if (key->end_field != 0) {
		end_field_start = field_start(sort, line, key->end_field);
		if (key->end_char == 0) {
			end = field_end(sort, line, end_field_start);
		} else {
			blanks = key->end_blanks ||
			    ((sort->flags & KEY_BLANKS) != 0 && key->flags == 0 &&
			     !key->start_blanks);
			if (blanks)
				end_field_start = skip_blanks(line, end_field_start, line->length);
			end = end_field_start + key->end_char;
		}
	}

	/* The span stays inside the line. */
	if (end > line->length)
		end = line->length;
	if (end < start)
		end = start;

	/* Succeeded. */
	span.text = line->text + start;
	span.length = end - start;
	return span;
}

/*
 * Returns where a field (from 1) starts.  Without -t a field starts with the
 * blanks before it; with -t just after the separator.
 */
static size_t
field_start(
	const struct sort *sort,
	const struct line *line,
	unsigned long field)
{
	unsigned long current;
	size_t position;

	/* Past the fields before it. */
	position = 0;
	for (current = 1; current < field; current++) {
		/* Past the current field. */
		position = field_end(sort, line, position);
		if (position >= line->length)
			return line->length;

		/* With -t, past the separator. */
		if (sort->separator >= 0)
			position++;
	}

	/* Succeeded. */
	return position;
}

/* Returns where the field that starts at start ends. */
static size_t
field_end(
	const struct sort *sort,
	const struct line *line,
	size_t start)
{
	size_t position;
	int blank;

	/* With -t: up to the separator. */
	position = start;
	if (sort->separator >= 0) {
		while (position < line->length &&
		       (unsigned char)line->text[position] != sort->separator)
			position++;
		return position;
	}

	/* Without: the blanks, then the non-blanks. */
	position = skip_blanks(line, position, line->length);
	for (; position < line->length; position++) {
		blank = is_blank(line->text[position]);
		if (blank)
			break;
	}

	/* Succeeded. */
	return position;
}

/* Returns the position after the blanks at position. */
static size_t
skip_blanks(
	const struct line *line,
	size_t position,
	size_t end)
{
	int blank;

	/* Each blank. */
	for (; position < end; position++) {
		blank = is_blank(line->text[position]);
		if (!blank)
			break;
	}

	/* Succeeded. */
	return position;
}

/* Compares two key spans with their modifiers. */
static int
compare_spans(
	struct span left,
	struct span right,
	int flags)
{
	int result;

	/* Numeric, one of GNU's types, or text. */
	if ((flags & KEY_NUMERIC) != 0)
		result = compare_numbers(left, right);
	else if ((flags & KEY_HUMAN) != 0)
		result = compare_human(left, right);
	else if ((flags & KEY_GENERAL) != 0)
		result = compare_general(left, right);
	else if ((flags & KEY_MONTH) != 0)
		result = compare_month(left, right);
	else if ((flags & KEY_VERSION) != 0)
		result = compare_version(left, right);
	else
		result = compare_text(left, right, flags);

	/* Succeeded: reversed with r. */
	if ((flags & KEY_REVERSE) != 0)
		return -result;
	return result;
}

/* Compares text, skipping what d and i ignore and folding with f. */
static int
compare_text(
	struct span left,
	struct span right,
	int flags)
{
	size_t a;
	size_t b;
	int skip;
	int x;
	int y;

	/* From the start of both. */
	a = 0;
	b = 0;
	for (;;) {
		/* The characters that count. */
		while (a < left.length) {
			skip = ignored((unsigned char)left.text[a], flags);
			if (!skip)
				break;
			a++;
		}
		while (b < right.length) {
			skip = ignored((unsigned char)right.text[b], flags);
			if (!skip)
				break;
			b++;
		}

		/* The end of either. */
		if (a >= left.length || b >= right.length)
			break;

		/* The next pair. */
		x = fold((unsigned char)left.text[a], flags);
		y = fold((unsigned char)right.text[b], flags);
		if (x != y)
			return x - y;
		a++;
		b++;
	}

	/* Succeeded: the shorter first. */
	if (a < left.length)
		return 1;
	if (b < right.length)
		return -1;
	return 0;
}

/*
 * Compares numbers: blanks, an optional -, digits, a . and digits.  A key
 * that is no number is 0.  The digits are compared as strings, so there is
 * no limit on their size.
 */
static int
compare_numbers(
	struct span left,
	struct span right)
{
	const char *left_integer;
	const char *left_fraction;
	const char *right_integer;
	const char *right_fraction;
	size_t left_integer_length;
	size_t left_fraction_length;
	size_t right_integer_length;
	size_t right_fraction_length;
	int left_negative;
	int right_negative;
	int left_nonzero;
	int right_nonzero;
	int result;

	/* The parts of each. */
	left_nonzero = number_parts(left, &left_negative, &left_integer,
				    &left_integer_length, &left_fraction,
				    &left_fraction_length);
	right_nonzero = number_parts(right, &right_negative, &right_integer,
				     &right_integer_length, &right_fraction,
				     &right_fraction_length);

	/* Zero has no sign. */
	if (!left_nonzero)
		left_negative = 0;
	if (!right_nonzero)
		right_negative = 0;

	/* A negative number is before a positive one. */
	if (left_negative && !right_negative)
		return -1;
	if (!left_negative && right_negative)
		return 1;

	/* Succeeded: the magnitudes, turned round for negatives. */
	result = compare_magnitudes(left_integer, left_integer_length,
				    left_fraction, left_fraction_length,
				    right_integer, right_integer_length,
				    right_fraction, right_fraction_length);
	if (left_negative)
		return -result;
	return result;
}

/*
 * Splits a number into its sign, integer digits (without leading zeros)
 * and fraction digits (without trailing zeros).  Returns 1 when it is not
 * zero.
 */
static int
number_parts(
	struct span span,
	int *negative,
	const char **integer,
	size_t *integer_length,
	const char **fraction,
	size_t *fraction_length)
{
	size_t position;
	size_t start;
	int blank;

	/* Leading blanks and a -. */
	position = 0;
	for (; position < span.length; position++) {
		blank = is_blank(span.text[position]);
		if (!blank)
			break;
	}

	/* The sign. */
	*negative = 0;
	if (position < span.length && span.text[position] == '-') {
		*negative = 1;
		position++;
	}

	/* The integer digits, less leading zeros. */
	while (position < span.length && span.text[position] == '0')
		position++;
	start = position;
	while (position < span.length && span.text[position] >= '0' &&
	       span.text[position] <= '9')
		position++;
	*integer = span.text + start;
	*integer_length = position - start;

	/* The fraction digits, less trailing zeros. */
	*fraction = span.text + position;
	*fraction_length = 0;
	if (position < span.length && span.text[position] == '.') {
		position++;
		start = position;
		while (position < span.length && span.text[position] >= '0' &&
		       span.text[position] <= '9')
			position++;
		*fraction = span.text + start;
		*fraction_length = position - start;
		while (*fraction_length > 0 &&
		       (*fraction)[*fraction_length - 1U] == '0')
			(*fraction_length)--;
	}

	/* Succeeded: whether any digit is not zero. */
	if (*integer_length > 0 || *fraction_length > 0)
		return 1;
	return 0;
}

/* Compares two non-negative numbers given as digit strings. */
static int
compare_magnitudes(
	const char *left_integer,
	size_t left_integer_length,
	const char *left_fraction,
	size_t left_fraction_length,
	const char *right_integer,
	size_t right_integer_length,
	const char *right_fraction,
	size_t right_fraction_length)
{
	size_t index;
	char a;
	char b;
	int result;

	/* More integer digits is larger; then digit by digit. */
	if (left_integer_length != right_integer_length) {
		if (left_integer_length < right_integer_length)
			return -1;
		return 1;
	}

	/* The same length: digit by digit. */
	result = memcmp(left_integer, right_integer, left_integer_length);
	if (result != 0)
		return result;

	/* The fractions, a missing digit counting as 0. */
	for (index = 0; index < left_fraction_length ||
	     index < right_fraction_length; index++) {
		a = '0';
		b = '0';
		if (index < left_fraction_length)
			a = left_fraction[index];
		if (index < right_fraction_length)
			b = right_fraction[index];
		if (a != b)
			return a - b;
	}

	/* Equal. */
	return 0;
}

/*
 * Compares two versions (GNU's V), as GNU sort does: a name's suffixes
 * (such as .tar.gz) are left out at first; runs of digits compare as
 * numbers, and other characters one by one, letters before other
 * characters and ~ before anything, even the end.  A name that starts with
 * a dot comes first.
 */
static int
compare_version(
	struct span left,
	struct span right)
{
	size_t left_prefix;
	size_t right_prefix;
	int left_dots;
	int right_dots;
	int result;

	/* An empty version comes first. */
	if (left.length == 0 && right.length == 0)
		return 0;
	if (left.length == 0)
		return -1;
	if (right.length == 0)
		return 1;

	/* A hidden name (a leading dot) comes before others. */
	if (left.text[0] == '.' && right.text[0] != '.')
		return -1;
	if (left.text[0] != '.' && right.text[0] == '.')
		return 1;

	/* Among hidden names . comes first, then .. . */
	if (left.text[0] == '.') {
		left_dots = version_dots(left);
		right_dots = version_dots(right);
		if (left_dots != right_dots)
			return right_dots - left_dots;
	}

	/* The names without their suffixes. */
	left_prefix = version_prefix(left);
	right_prefix = version_prefix(right);
	result = compare_version_part(left.text, left_prefix, right.text,
				      right_prefix);
	if (result != 0)
		return result;

	/* Without suffixes, the first comparison was of the whole names. */
	if (left_prefix == left.length && right_prefix == right.length)
		return 0;

	/* Succeeded: the whole names. */
	result = compare_version_part(left.text, left.length, right.text,
				      right.length);
	return result;
}

/* Returns 2 for the name ., 1 for .., and 0 for any other name. */
static int
version_dots(
	struct span span)
{
	/* . */
	if (span.length == 1U && span.text[0] == '.')
		return 2;

	/* .. */
	if (span.length == 2U && span.text[0] == '.' && span.text[1] == '.')
		return 1;

	/* Any other name. */
	return 0;
}

/*
 * Returns the length of a name without its suffixes: the dot-words at its
 * end that start with a letter or ~ (such as .tar.gz), never the first
 * character.
 */
static size_t
version_prefix(
	struct span span)
{
	size_t prefix;
	size_t position;
	int letter;
	int digit;

	/* Each character; a run of suffixes that reaches the end is cut. */
	prefix = 0;
	position = 0;
	while (position < span.length) {
		/* The character is part of the name. */
		position++;
		prefix = position;

		/* Suffixes that follow: .word, while they last. */
		while (position + 1U < span.length && span.text[position] == '.') {
			letter = is_letter(span.text[position + 1U]);
			if (!letter && span.text[position + 1U] != '~')
				break;

			/* The word of the suffix: letters, digits and ~. */
			position += 2U;
			while (position < span.length) {
				letter = is_letter(span.text[position]);
				digit = is_digit(span.text[position]);
				if (!letter && !digit && span.text[position] != '~')
					break;
				position++;
			}
		}
	}

	/* Succeeded: where the suffixes start. */
	return prefix;
}

/*
 * Compares two parts of versions: the characters before each run of digits
 * one by one, then the runs as numbers.
 */
static int
compare_version_part(
	const char *left,
	size_t left_length,
	const char *right,
	size_t right_length)
{
	size_t a;
	size_t b;
	int first_difference;
	int left_order;
	int right_order;
	int digit_a;
	int digit_b;

	/* Each pair of a text run and a number run. */
	a = 0;
	b = 0;
	while (a < left_length || b < right_length) {
		/* The characters up to the digits, in version order. */
		for (;;) {
			digit_a = 1;
			if (a < left_length)
				digit_a = is_digit(left[a]);
			digit_b = 1;
			if (b < right_length)
				digit_b = is_digit(right[b]);
			if (digit_a && digit_b)
				break;

			/* A character that orders differently decides. */
			left_order = version_order(left, a, left_length);
			right_order = version_order(right, b, right_length);
			if (left_order != right_order)
				return left_order - right_order;
			a++;
			b++;
		}

		/* The numbers, without their leading zeros. */
		while (a < left_length && left[a] == '0')
			a++;
		while (b < right_length && right[b] == '0')
			b++;

		/* The digits side by side; the first difference counts. */
		first_difference = 0;
		for (;;) {
			digit_a = 0;
			if (a < left_length)
				digit_a = is_digit(left[a]);
			digit_b = 0;
			if (b < right_length)
				digit_b = is_digit(right[b]);
			if (!digit_a || !digit_b)
				break;
			if (first_difference == 0)
				first_difference = left[a] - right[b];
			a++;
			b++;
		}

		/* A longer number is larger. */
		if (digit_a)
			return 1;
		if (digit_b)
			return -1;

		/* As long: the first differing digit. */
		if (first_difference != 0)
			return first_difference;
	}

	/* Equal. */
	return 0;
}

/*
 * Returns where a character of a version sorts: the end before letters,
 * ~ before the end, letters by their code, other characters after all
 * letters.  A digit is 0 (the text run is over).
 */
static int
version_order(
	const char *text,
	size_t position,
	size_t length)
{
	int letter;
	int digit;
	unsigned char value;

	/* The end. */
	if (position >= length)
		return -1;
	value = (unsigned char)text[position];

	/* A digit, a letter, a ~, or anything else. */
	digit = is_digit((char)value);
	if (digit)
		return 0;
	letter = is_letter((char)value);
	if (letter)
		return value;
	if (value == '~')
		return -2;

	/* Anything else sorts after every letter. */
	return value + 256;
}

/*
 * Compares human numbers (GNU's h): the unit (none, K, M, G, T, P, E, Z,
 * Y, R, Q) first, negative units before, then the numbers.
 */
static int
compare_human(
	struct span left,
	struct span right)
{
	int left_unit;
	int right_unit;
	int result;

	/* The units. */
	left_unit = unit_order(left);
	right_unit = unit_order(right);
	if (left_unit != right_unit)
		return left_unit - right_unit;

	/* Succeeded: the same unit, so the numbers. */
	result = compare_numbers(left, right);
	return result;
}

/*
 * Returns the order of a human number's unit: 0 for none, 1 for K and so
 * on, negative for a negative number.
 */
static int
unit_order(
	struct span span)
{
	static const char units[] = "KMGTPEZYRQ";
	const char *found;
	size_t position;
	int negative;
	int blank;
	int digit;
	int order;

	/* Leading blanks. */
	position = 0;
	while (position < span.length) {
		blank = is_blank(span.text[position]);
		if (!blank)
			break;
		position++;
	}

	/* The sign. */
	negative = 0;
	if (position < span.length && span.text[position] == '-') {
		negative = 1;
		position++;
	}

	/* The digits and a fraction. */
	while (position < span.length) {
		digit = is_digit(span.text[position]);
		if (!digit && span.text[position] != '.')
			break;
		position++;
	}

	/* The unit after them (k is K). */
	order = 0;
	if (position < span.length && span.text[position] != '\0') {
		found = strchr(units, span.text[position]);
		if (span.text[position] == 'k')
			found = units;
		if (found != NULL)
			order = (int)(found - units) + 1;
	}

	/* Succeeded: negative for a negative number. */
	if (negative)
		return -order;
	return order;
}

/*
 * Compares general numbers (GNU's g), read as floating point with an
 * exponent; what is no number sorts before every number.
 */
static int
compare_general(
	struct span left,
	struct span right)
{
	double left_value;
	double right_value;
	int left_number;
	int right_number;

	/* The values, where they are numbers. */
	left_number = general_value(left, &left_value);
	right_number = general_value(right, &right_value);

	/* What is no number comes first. */
	if (!left_number && !right_number)
		return 0;
	if (!left_number)
		return -1;
	if (!right_number)
		return 1;

	/* Succeeded: the values. */
	if (left_value < right_value)
		return -1;
	if (left_value > right_value)
		return 1;
	return 0;
}

/* Reads a general number from the start of a span.  Returns 0 for none. */
static int
general_value(
	struct span span,
	double *value)
{
	char buffer[128];
	char *end;
	size_t length;

	/* A copy that ends, as strtod needs. */
	length = span.length;
	if (length >= sizeof(buffer))
		length = sizeof(buffer) - 1U;
	memcpy(buffer, span.text, length);
	buffer[length] = '\0';

	/* The number; a NaN is no number. */
	*value = strtod(buffer, &end);
	if (end == buffer)
		return 0;
	if (*value != *value)
		return 0;

	/* Succeeded. */
	return 1;
}

/*
 * Compares month names (GNU's M): JAN to DEC in any case, after blanks;
 * what is no month comes first.
 */
static int
compare_month(
	struct span left,
	struct span right)
{
	int left_month;
	int right_month;

	/* The months, 0 for none. */
	left_month = month_number(left);
	right_month = month_number(right);

	/* Succeeded: in the order of the year. */
	return left_month - right_month;
}

/* Returns the month (1 to 12) a span starts with, or 0. */
static int
month_number(
	struct span span)
{
	static const char months[] = "JANFEBMARAPRMAYJUNJULAUGSEPOCTNOVDEC";
	char name[3];
	size_t position;
	size_t index;
	int blank;
	int same;

	/* Leading blanks. */
	position = 0;
	while (position < span.length) {
		blank = is_blank(span.text[position]);
		if (!blank)
			break;
		position++;
	}

	/* Three letters, in upper case. */
	if (span.length - position < 3U)
		return 0;
	for (index = 0; index < 3U; index++)
		name[index] = (char)fold((unsigned char)span.text[position + index], KEY_FOLD);

	/* The month they name. */
	for (index = 0; index < 12U; index++) {
		same = memcmp(name, months + index * 3U, 3U);
		if (same == 0)
			return (int)index + 1;
	}

	/* No month. */
	return 0;
}

/* Reports whether a character is a decimal digit. */
static int
is_digit(
	char value)
{
	/* 0 to 9. */
	if (value >= '0' && value <= '9')
		return 1;
	return 0;
}

/* Reports whether a character is an ASCII letter. */
static int
is_letter(
	char value)
{
	/* a to z and A to Z. */
	if (value >= 'a' && value <= 'z')
		return 1;
	if (value >= 'A' && value <= 'Z')
		return 1;
	return 0;
}

/* Reports whether d or i makes a character not count. */
static int
ignored(
	unsigned char value,
	int flags)
{
	int alphanumeric;

	/* d: only blanks and alphanumerics count. */
	if ((flags & KEY_DICTIONARY) != 0) {
		alphanumeric = (value >= '0' && value <= '9') ||
		    (value >= 'a' && value <= 'z') ||
		    (value >= 'A' && value <= 'Z');
		if (!alphanumeric && value != ' ' && value != '\t')
			return 1;
	}

	/* i: only printable characters count. */
	if ((flags & KEY_PRINTABLE) != 0) {
		if (value < 0x20U || value >= 0x7fU)
			return 1;
	}

	/* It counts. */
	return 0;
}

/* Folds lower case to upper case with f. */
static int
fold(
	unsigned char value,
	int flags)
{
	/* f, and a lower-case letter. */
	if ((flags & KEY_FOLD) != 0 && value >= 'a' && value <= 'z')
		return value - 'a' + 'A';
	return value;
}

/* Reports whether a character is a blank. */
static int
is_blank(
	char value)
{
	/* Space and tab. */
	if (value == ' ' || value == '\t')
		return 1;
	return 0;
}

/*
 * -c and -C: checks that the input is sorted (and with -u, has no equal
 * keys).  -c reports the first line out of order.  Returns 0 when sorted.
 */
static int
check_order(
	struct sort *sort,
	const char *name)
{
	size_t index;
	int result;

	/* Each line after the first. */
	for (index = 1; index < sort->line_count; index++) {
		/* A line before the one before it, or equal with -u. */
		result = compare_lines(&sort->lines[index - 1U],
				       &sort->lines[index]);
		if (result < 0)
			continue;
		if (result == 0 && !sort->unique)
			continue;
		if (sort->check == 1)
			fprintf(stderr, "sort: %s:%lu: disorder: %s\n", name, (unsigned long)index + 1UL, sort->lines[index].text);
		return 1;
	}

	/* Sorted. */
	return 0;
}

/* Writes the sorted lines, the first of equal keys only with -u. */
static void
write_output(
	struct sort *sort)
{
	FILE *stream;
	size_t index;
	int result;

	/* The output: standard output, or the -o file. */
	stream = stdout;
	if (sort->output != NULL) {
		stream = fopen(sort->output, "w");
		if (stream == NULL) {
			fprintf(stderr, "sort: cannot create %s: %s\n",
				sort->output, strerror(errno));
			exit(2);
		}
	}

	/* Each line, to the output. */
	for (index = 0; index < sort->line_count; index++) {
		/* -u drops a line equal to the one before. */
		if (sort->unique && index > 0) {
			result = compare_lines(&sort->lines[index - 1U],
					       &sort->lines[index]);
			if (result == 0)
				continue;
		}

		/* The line and its newline (NUL with -z). */
		fwrite(sort->lines[index].text, 1, sort->lines[index].length,
		       stream);
		putc(sort->delimiter, stream);
	}

	/* The output is done with. */
	if (stream != stdout)
		fclose(stream);
}

/* Allocates or resizes memory, ending sort when there is none. */
static void *
allocate(
	void *memory,
	size_t size)
{
	void *result;

	/* The memory. */
	result = realloc(memory, size);
	if (result == NULL) {
		fprintf(stderr, "sort: out of memory\n");
		exit(2);
	}

	/* Succeeded. */
	return result;
}

/* Reports the usage and ends sort. */
static void
usage(
	void)
{
	/* The forms. */
	fprintf(stderr, "usage: sort [-m] [-o output] [-bdfghiMnrsuVz] "
		"[-t char] [-k keydef]... [file...]\n"
		"       sort -c|-C [-bdfghiMnrsuVz] [-t char] [-k keydef] "
		"[file]\n");
	exit(2);
}
