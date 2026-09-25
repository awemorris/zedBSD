/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Translates, deletes or squeezes characters (POSIX XCU tr).
 *
 *	tr [-c|-C] [-s] string1 string2
 *	tr -s [-c|-C] string1
 *	tr -d [-c|-C] string1
 *	tr -ds [-c|-C] string1 string2
 *
 * A string is characters, escapes (\\ \a \b \f \n \r \t \v and \ooo),
 * ranges (a-z), classes ([:alpha:] and the rest, in byte order),
 * equivalence classes ([=c=]) and repeats ([x*n], and [x*] in string2 to
 * fill it to the length of string1).  -c and -C take the characters not in
 * string1, in byte order (the C locale has no collation of its own).
 *
 * Translating maps each character of string1 to the one at the same place
 * in string2; a shorter string2 is extended with its last character, as GNU
 * tr does.  -d deletes the characters of string1.  -s squeezes each run of
 * a character of the last string given into one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An expanded string: its characters, and where a [x*] fill stands. */
struct set {
	unsigned char characters[4096];
	size_t length;
	long fill_at;		/* index of the [x*] fill, or -1 */
	unsigned char fill;
};

/* A character class and what it holds. */
struct class_name {
	const char *name;
	int (*test)(int value);
};

static int is_alnum(int value);
static int is_alpha(int value);
static int is_blank(int value);
static int is_cntrl(int value);
static int is_digit(int value);
static int is_graph(int value);
static int is_lower(int value);
static int is_print(int value);
static int is_punct(int value);
static int is_space(int value);
static int is_upper(int value);
static int is_xdigit(int value);

/* The classes of [:name:]. */
static const struct class_name classes[] = {
	{ "alnum", is_alnum },
	{ "alpha", is_alpha },
	{ "blank", is_blank },
	{ "cntrl", is_cntrl },
	{ "digit", is_digit },
	{ "graph", is_graph },
	{ "lower", is_lower },
	{ "print", is_print },
	{ "punct", is_punct },
	{ "space", is_space },
	{ "upper", is_upper },
	{ "xdigit", is_xdigit },
	{ NULL, NULL }
};

static void expand(const char *text, struct set *set, int second);
static size_t read_character(const char *text, unsigned char *value);
static size_t read_bracket(const char *text, struct set *set, int second);
static size_t read_repeat(const char *text, struct set *set, int second);
static void add_class(const char *name, size_t length, struct set *set);
static void add(struct set *set, unsigned char value);
static void complement(struct set *set);
static void fill(struct set *set, size_t length);
static void fatal(const char *message);
static void usage(void);

/*
 * Runs tr.
 */
int
main(
	int argc,
	char **argv)
{
	struct set first;
	struct set second;
	unsigned char map[256];
	unsigned char remove[256];
	unsigned char squeeze[256];
	const char *letter;
	size_t index;
	int invert;
	int delete;
	int squeezing;
	int translate;
	int operands;
	int value;
	int last;
	int argument;

	/* The options. */
	invert = 0;
	delete = 0;
	squeezing = 0;
	for (argument = 1; argument < argc; argument++) {
		if (argv[argument][0] != '-' || argv[argument][1] == '\0')
			break;
		if (argv[argument][1] == '-' && argv[argument][2] == '\0') {
			argument++;
			break;
		}

		/* Each letter of the options. */
		for (letter = argv[argument] + 1; *letter != '\0'; letter++) {
			if (*letter == 'c' || *letter == 'C')
				invert = 1;
			else if (*letter == 'd')
				delete = 1;
			else if (*letter == 's')
				squeezing = 1;
			else
				usage();
		}
	}

	/* One string, or two: translating needs two, -d alone takes one. */
	operands = argc - argument;
	translate = !delete && operands == 2;
	if (operands < 1 || operands > 2)
		usage();
	if (delete && !squeezing && operands != 1)
		usage();
	if (delete && squeezing && operands != 2)
		usage();
	if (!delete && !squeezing && operands != 2)
		usage();

	/* The strings, expanded. */
	expand(argv[argument], &first, 0);
	if (invert)
		complement(&first);
	memset(&second, 0, sizeof(second));
	second.fill_at = -1;
	if (operands == 2)
		expand(argv[argument + 1], &second, 1);

	/* The maps: translation, deletion and squeezing. */
	for (index = 0; index < 256U; index++)
		map[index] = (unsigned char)index;
	memset(remove, 0, sizeof(remove));
	memset(squeeze, 0, sizeof(squeeze));
	if (translate) {
		if (second.length == 0 && second.fill_at < 0)
			fatal("when not truncating set1, string2 must be non-empty");
		fill(&second, first.length);
		for (index = 0; index < first.length; index++)
			map[first.characters[index]] = second.characters[index];
	}

	/* -d: the characters of string1 go. */
	if (delete) {
		for (index = 0; index < first.length; index++)
			remove[first.characters[index]] = 1;
	}

	/* -s: the characters that squeeze, of string2 or else of string1. */
	if (squeezing && operands == 2) {
		for (index = 0; index < second.length; index++)
			squeeze[second.characters[index]] = 1;
	} else if (squeezing) {
		for (index = 0; index < first.length; index++)
			squeeze[first.characters[index]] = 1;
	}

	/* Each byte: deleted, translated, and squeezed against the last. */
	last = -1;
	for (;;) {
		value = getchar();
		if (value == EOF)
			break;
		if (remove[value])
			continue;
		value = map[value];
		if (squeeze[value] && value == last)
			continue;
		putchar(value);
		last = value;
	}

	/* Succeeded. */
	return 0;
}

/* Expands a string into its characters. */
static void
expand(
	const char *text,
	struct set *set,
	int second)
{
	unsigned char start;
	unsigned char end;
	size_t used;
	unsigned value;

	/* An empty set, without a fill. */
	memset(set, 0, sizeof(*set));
	set->fill_at = -1;
	while (*text != '\0') {
		/* A bracket construct: [:class:], [=c=] or [x*n]. */
		if (*text == '[') {
			used = read_bracket(text, set, second);
			if (used > 0) {
				text += used;
				continue;
			}
		}

		/* A character, and a range when a - and another follow. */
		used = read_character(text, &start);
		text += used;
		if (text[0] == '-' && text[1] != '\0') {
			used = read_character(text + 1, &end);
			if (end < start)
				fatal("range-endpoints are in reverse collating sequence order");
			for (value = start; value <= end; value++)
				add(set, (unsigned char)value);
			text += 1 + used;
			continue;
		}

		/* A plain character. */
		add(set, start);
	}
}

/* Reads one character or escape; returns how many bytes it took. */
static size_t
read_character(
	const char *text,
	unsigned char *value)
{
	size_t digits;
	unsigned number;

	/* An ordinary character. */
	if (text[0] != '\\' || text[1] == '\0') {
		*value = (unsigned char)text[0];
		return 1;
	}

	/* \ooo: one to three octal digits. */
	if (text[1] >= '0' && text[1] <= '7') {
		number = 0;
		for (digits = 1; digits <= 3U && text[digits] >= '0' &&
		     text[digits] <= '7'; digits++)
			number = number * 8U + (unsigned)(text[digits] - '0');
		*value = (unsigned char)number;
		return digits;
	}

	/* The C escapes, or the character itself. */
	switch (text[1]) {
	case 'a':
		*value = '\a';
		break;
	case 'b':
		*value = '\b';
		break;
	case 'f':
		*value = '\f';
		break;
	case 'n':
		*value = '\n';
		break;
	case 'r':
		*value = '\r';
		break;
	case 't':
		*value = '\t';
		break;
	case 'v':
		*value = '\v';
		break;
	default:
		*value = (unsigned char)text[1];
		break;
	}

	/* Succeeded: a backslash and one character. */
	return 2;
}

/*
 * Reads [:class:], [=c=] or [x*n] at text.  Returns how many bytes it took,
 * or 0 when the [ is an ordinary character.
 */
static size_t
read_bracket(
	const char *text,
	struct set *set,
	int second)
{
	const char *close;
	unsigned char value;
	size_t used;
	size_t repeat;

	/* [:class:]. */
	if (text[1] == ':') {
		close = strstr(text + 2, ":]");
		if (close == NULL)
			return 0;
		add_class(text + 2, (size_t)(close - (text + 2)), set);
		return (size_t)(close - text) + 2U;
	}

	/* [=c=]. */
	if (text[1] == '=') {
		used = read_character(text + 2, &value);
		if (text[2 + used] != '=' || text[3 + used] != ']')
			return 0;
		add(set, value);
		return 4U + used;
	}

	/* Succeeded: [x*n], or nothing. */
	repeat = read_repeat(text, set, second);
	return repeat;
}

/*
 * Reads [x*n] (n decimal, or octal with a leading 0) or, in string2, [x*]
 * to fill.  Returns how many bytes it took, or 0 when it is none.
 */
static size_t
read_repeat(
	const char *text,
	struct set *set,
	int second)
{
	const char *cursor;
	unsigned char value;
	unsigned long count;
	unsigned long index;
	unsigned base;
	size_t used;

	/* The character and the *. */
	used = read_character(text + 1, &value);
	cursor = text + 1 + used;
	if (*cursor != '*')
		return 0;
	cursor++;

	/* The count: decimal, octal after a 0. */
	base = 10;
	if (*cursor == '0')
		base = 8;
	count = 0;
	while (*cursor >= '0' && *cursor <= '9') {
		count = count * base + (unsigned long)(*cursor - '0');
		cursor++;
	}

	/* The construct must close. */
	if (*cursor != ']')
		return 0;

	/* [x*] and [x*0] fill string2; elsewhere they are errors. */
	if (count == 0) {
		if (!second)
			fatal("the [c*] repeat construct may not appear in string1");
		set->fill_at = (long)set->length;
		set->fill = value;
		return (size_t)(cursor - text) + 1U;
	}

	/* Succeeded: the character count times. */
	for (index = 0; index < count; index++)
		add(set, value);
	return (size_t)(cursor - text) + 1U;
}

/* Adds the characters of a class, in byte order. */
static void
add_class(
	const char *name,
	size_t length,
	struct set *set)
{
	size_t index;
	size_t name_length;
	unsigned value;
	int compare;
	int member;

	/* Each class, by its name. */
	for (index = 0; classes[index].name != NULL; index++) {
		/* The class of that name. */
		name_length = strlen(classes[index].name);
		if (name_length != length)
			continue;
		compare = strncmp(classes[index].name, name, length);
		if (compare != 0)
			continue;

		/* Its members. */
		for (value = 0; value < 256U; value++) {
			member = classes[index].test((int)value);
			if (member)
				add(set, (unsigned char)value);
		}

		/* Found: done. */
		return;
	}

	/* No such class. */
	fatal("invalid character class");
}

/* Adds one character to a set. */
static void
add(
	struct set *set,
	unsigned char value)
{
	/* Within the room of the set. */
	if (set->length >= sizeof(set->characters))
		fatal("string too long");
	set->characters[set->length] = value;
	set->length++;
}

/* Replaces a set with the characters not in it, in byte order. */
static void
complement(
	struct set *set)
{
	unsigned char present[256];
	size_t index;
	unsigned value;

	/* What is in it. */
	memset(present, 0, sizeof(present));
	for (index = 0; index < set->length; index++)
		present[set->characters[index]] = 1;

	/* Succeeded: what is not. */
	set->length = 0;
	for (value = 0; value < 256U; value++) {
		if (!present[value])
			add(set, (unsigned char)value);
	}
}

/*
 * Makes string2 as long as string1: the [x*] fill where it stands, or the
 * last character repeated.
 */
static void
fill(
	struct set *set,
	size_t length)
{
	size_t missing;
	size_t at;

	/* Long enough already. */
	if (set->length >= length)
		return;
	missing = length - set->length;

	/* A [x*]: its characters go where it stood. */
	if (set->fill_at >= 0) {
		at = (size_t)set->fill_at;
		memmove(set->characters + at + missing, set->characters + at,
			set->length - at);
		memset(set->characters + at, set->fill, missing);
		set->length += missing;
		return;
	}

	/* Succeeded: the last character repeated. */
	while (set->length < length)
		add(set, set->characters[set->length - 1U]);
}

/* Reports whether a byte is an upper-case letter (C locale). */
static int
is_upper(
	int value)
{
	/* The class. */
	if (value >= 'A' && value <= 'Z')
		return 1;
	return 0;
}

/* Reports whether a byte is a lower-case letter (C locale). */
static int
is_lower(
	int value)
{
	/* The class. */
	if (value >= 'a' && value <= 'z')
		return 1;
	return 0;
}

/* Reports whether a byte is a decimal digit (C locale). */
static int
is_digit(
	int value)
{
	/* The class. */
	if (value >= '0' && value <= '9')
		return 1;
	return 0;
}

/* Reports whether a byte is a letter (C locale). */
static int
is_alpha(
	int value)
{
	int upper;
	int lower;

	/* The class. */
	upper = is_upper(value);
	lower = is_lower(value);
	if (upper || lower)
		return 1;
	return 0;
}

/* Reports whether a byte is a letter or a digit (C locale). */
static int
is_alnum(
	int value)
{
	int alpha;
	int digit;

	/* The class. */
	alpha = is_alpha(value);
	digit = is_digit(value);
	if (alpha || digit)
		return 1;
	return 0;
}

/* Reports whether a byte is a space or a tab (C locale). */
static int
is_blank(
	int value)
{
	/* The class. */
	if (value == ' ' || value == '\t')
		return 1;
	return 0;
}

/* Reports whether a byte is a control character (C locale). */
static int
is_cntrl(
	int value)
{
	/* The class. */
	if (value < 0x20 || value == 0x7f)
		return 1;
	return 0;
}

/* Reports whether a byte is printable, space included (C locale). */
static int
is_print(
	int value)
{
	/* The class. */
	if (value >= 0x20 && value < 0x7f)
		return 1;
	return 0;
}

/* Reports whether a byte is printable, space excluded (C locale). */
static int
is_graph(
	int value)
{
	/* The class. */
	if (value > 0x20 && value < 0x7f)
		return 1;
	return 0;
}

/* Reports whether a byte is printable, not space and not alphanumeric (C locale). */
static int
is_punct(
	int value)
{
	int graph;
	int alnum;

	/* The class. */
	graph = is_graph(value);
	alnum = is_alnum(value);
	if (graph && !alnum)
		return 1;
	return 0;
}

/* Reports whether a byte is white space (C locale). */
static int
is_space(
	int value)
{
	/* The class. */
	if (value == ' ' || (value >= '\t' && value <= '\r'))
		return 1;
	return 0;
}

/* Reports whether a byte is a hexadecimal digit (C locale). */
static int
is_xdigit(
	int value)
{
	int digit;

	/* The class. */
	digit = is_digit(value);
	if (digit)
		return 1;
	if (value >= 'a' && value <= 'f')
		return 1;
	if (value >= 'A' && value <= 'F')
		return 1;
	return 0;
}

/* Reports an error and ends tr. */
static void
fatal(
	const char *message)
{
	/* The message. */
	fprintf(stderr, "tr: %s\n", message);
	exit(1);
}

/* Reports the usage and ends tr. */
static void
usage(
	void)
{
	/* The forms. */
	fprintf(stderr, "usage: tr [-c|-C] [-s] string1 string2\n"
		"       tr -s [-c|-C] string1\n"
		"       tr -d [-c|-C] string1\n"
		"       tr -ds [-c|-C] string1 string2\n");
	exit(1);
}
