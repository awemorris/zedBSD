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
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ordering modifiers of a key (and the global ones). */
#define KEY_BLANKS	0x01	/* b */
#define KEY_DICTIONARY	0x02	/* d */
#define KEY_FOLD	0x04	/* f */
#define KEY_PRINTABLE	0x08	/* i */
#define KEY_NUMERIC	0x10	/* n */
#define KEY_REVERSE	0x20	/* r */

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

/* One line of input. */
struct line {
	char *text;
	size_t length;
};

/* The options, the keys and the lines. */
struct sort {
	int check;		/* -c: 1, -C: 2 */
	int unique;
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
	int first;
	int index;
	int compare;
	int status;

	/* The options; the comparison reads them through sorting. */
	memset(&sort, 0, sizeof(sort));
	sort.separator = -1;
	first = read_options(argc, argv, &sort);
	sorting = &sort;

	/* Every input, whole; standard input when there is none. */
	name = "-";
	if (first >= argc)
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

/* Reads the options; returns the index of the first operand. */
static int
read_options(
	int argc,
	char **argv,
	struct sort *sort)
{
	const char *word;
	const char *letter;
	const char *argument;
	int index;
	int flag;

	/* Each option word. */
	for (index = 1; index < argc; index++) {
		/* An operand, or - alone, ends the options; so does --. */
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* Each letter of the word. */
		for (letter = word + 1; *letter != '\0'; letter++) {
			/* The modifiers that apply to every key. */
			flag = modifier_flag(*letter);
			if (flag != 0) {
				sort->flags |= flag;
				continue;
			}

			/* Letters that take nothing. */
			if (*letter == 'c' || *letter == 'C') {
				sort->check = 1;
				if (*letter == 'C')
					sort->check = 2;
				continue;
			}

			/* -u keeps one of equal lines. */
			if (*letter == 'u') {
				sort->unique = 1;
				continue;
			}

			/* -m is taken as a plain sort; the rest take an argument. */
			if (*letter == 'm')
				continue;
			if (*letter != 'o' && *letter != 't' && *letter != 'k')
				usage();

			/* -o, -t and -k take the rest of the word or the next. */
			argument = letter + 1;
			if (*argument == '\0') {
				if (index + 1 >= argc)
					usage();
				index++;
				argument = argv[index];
			}

			/* -o names the output, and -t the field separator. */
			if (*letter == 'o')
				sort->output = argument;
			if (*letter == 't') {
				if (argument[0] == '\0' || argument[1] != '\0') {
					fprintf(stderr, "sort: multi-character "
						"tab '%s'\n", argument);
					exit(2);
				}

				/* The separator. */
				sort->separator = (unsigned char)argument[0];
			}

			/* -k adds a key. */
			if (*letter == 'k')
				parse_key(sort, argument);
			break;
		}
	}

	/* Succeeded: the first operand. */
	return index;
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
		/* The next byte; a newline or the end ends a line. */
		value = getc(stream);
		if (value == EOF || value == '\n') {
			if (value == '\n' || length > 0)
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

	/* Numeric or text. */
	if ((flags & KEY_NUMERIC) != 0)
		result = compare_numbers(left, right);
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

		/* The line and its newline. */
		fwrite(sort->lines[index].text, 1, sort->lines[index].length,
		       stream);
		putc('\n', stream);
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
	fprintf(stderr, "usage: sort [-m] [-o output] [-bdfinru] [-t char] "
		"[-k keydef]... [file...]\n"
		"       sort -c|-C [-bdfinru] [-t char] [-k keydef] [file]\n");
	exit(2);
}
