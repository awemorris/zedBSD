/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Numbers lines (POSIX XCU nl).
 *
 *	nl [-p] [-b type] [-d delim] [-f type] [-h type] [-i incr] [-l num]
 *	   [-n format] [-s sep] [-v startnum] [-w width] [file]
 *
 * The input is read as logical pages of a header, a body and a footer,
 * started by lines that hold only the delimiter three, two or one times
 * (\:\:\:, \:\: and \:).  Such a line is written as an empty line and,
 * unless -p is given, starts the numbering again at startnum, as other
 * systems do for each section.  Lines before any delimiter are body lines.
 *
 * Which lines of a section are numbered is its type: a for all, t for
 * those that are not empty (the body's default), n for none (the header's
 * and footer's default), or pBRE for those the basic regular expression
 * matches.  -l num with type a numbers only every num-th of a run of empty
 * lines.  A number is written in width columns in the format ln, rn or rz
 * (left, right, right with zeros) and followed by sep; a line that is not
 * numbered gets as many spaces instead.
 */

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The sections of a logical page. */
#define NL_HEADER 0
#define NL_BODY 1
#define NL_FOOTER 2
#define NL_SECTIONS 3

/* The kinds of numbering a section may have. */
#define NL_TYPE_ALL 0
#define NL_TYPE_TEXT 1
#define NL_TYPE_NONE 2
#define NL_TYPE_PATTERN 3

/*
 * How one section numbers its lines.
 *
 * pattern is compiled only for NL_TYPE_PATTERN and lives for the run.
 */
struct nl_type {
	int kind;
	regex_t pattern;
};

/*
 * What one run of nl does, and where it is.
 *
 * One instance lives for the run.  number is the next line number; blanks
 * counts the empty lines in a row, for -l.
 */
struct nl_state {
	struct nl_type types[NL_SECTIONS];
	char delimiter[3];
	long increment;
	long blank_group;
	const char *format;
	const char *separator;
	long start;
	int width;
	int keep_numbers;
	int section;
	long number;
	long blanks;
};

static int read_options(int argc, char **argv, struct nl_state *state);
static int read_type(const char *text, struct nl_type *type);
static int read_number(const char *text, long *value, int positive);
static int number_stream(FILE *input, struct nl_state *state);
static int delimiter_section(const struct nl_state *state, const char *line, size_t length);
static int wants_number(struct nl_state *state, const char *line, size_t length);
static void write_number(const struct nl_state *state);
static void usage(void);

/*
 * Runs nl.
 */
int
main(
	int argc,
	char **argv)
{
	struct nl_state state;
	FILE *input;
	int first;
	int status;
	int compare;
	int written;

	/* Reads the options; at most one file follows them. */
	first = read_options(argc, argv, &state);
	if (argc - first > 1)
		usage();

	/* Opens the file, or reads standard input for none or -. */
	input = stdin;
	if (first < argc) {
		compare = strcmp(argv[first], "-");
		if (compare != 0) {
			input = fopen(argv[first], "r");
			if (input == NULL) {
				fprintf(stderr, "nl: %s: %s\n", argv[first], strerror(errno));
				return 1;
			}
		}
	}

	/* Numbers the lines. */
	status = number_stream(input, &state);
	if (status != 0)
		fprintf(stderr, "nl: read error: %s\n", strerror(errno));

	/* A failed write is an error too. */
	fflush(stdout);
	written = ferror(stdout);
	if (written != 0) {
		fprintf(stderr, "nl: write error: %s\n", strerror(errno));
		return 1;
	}

	/* Reports a read error. */
	if (status != 0)
		return 1;

	/* Succeeded: every line was written. */
	return 0;
}

/*
 * Reads the options and returns the index of the file operand.  An
 * invalid option ends nl with a diagnostic.
 */
static int
read_options(
	int argc,
	char **argv,
	struct nl_state *state)
{
	int option;
	int status;
	long width;
	size_t length;
	int ln;
	int rn;
	int rz;

	/* The defaults: body t, header and footer n, \: as the delimiter. */
	memset(state, 0, sizeof(*state));
	state->types[NL_HEADER].kind = NL_TYPE_NONE;
	state->types[NL_BODY].kind = NL_TYPE_TEXT;
	state->types[NL_FOOTER].kind = NL_TYPE_NONE;
	strcpy(state->delimiter, "\\:");
	state->increment = 1;
	state->blank_group = 1;
	state->format = "rn";
	state->separator = "\t";
	state->start = 1;
	state->width = 6;

	/* Reads each option. */
	for (;;) {
		option = getopt(argc, argv, "b:d:f:h:i:l:n:ps:v:w:");
		if (option == -1)
			break;

		/* Records what the option asks for. */
		status = 0;
		switch (option) {
		case 'b':
			status = read_type(optarg, &state->types[NL_BODY]);
			break;
		case 'h':
			status = read_type(optarg, &state->types[NL_HEADER]);
			break;
		case 'f':
			status = read_type(optarg, &state->types[NL_FOOTER]);
			break;
		case 'd':
			/* One or two characters; one alone keeps : as the second. */
			length = strlen(optarg);
			if (length == 0 || length > 2) {
				status = -1;
				break;
			}

			/* Takes the characters given. */
			state->delimiter[0] = optarg[0];
			if (length == 2)
				state->delimiter[1] = optarg[1];
			break;
		case 'i':
			status = read_number(optarg, &state->increment, 0);
			break;
		case 'l':
			status = read_number(optarg, &state->blank_group, 1);
			break;
		case 'n':
			/* The format is one of three names. */
			ln = strcmp(optarg, "ln");
			rn = strcmp(optarg, "rn");
			rz = strcmp(optarg, "rz");
			if (ln != 0 && rn != 0 && rz != 0)
				status = -1;
			state->format = optarg;
			break;
		case 'p':
			state->keep_numbers = 1;
			break;
		case 's':
			state->separator = optarg;
			break;
		case 'v':
			status = read_number(optarg, &state->start, 0);
			break;
		case 'w':
			status = read_number(optarg, &width, 1);
			if (status == 0 && width > 1000)
				status = -1;
			state->width = (int)width;
			break;
		default:
			usage();
			break;
		}

		/* An invalid option argument ends the run. */
		if (status != 0) {
			fprintf(stderr, "nl: invalid argument to -%c: '%s'\n", option, optarg);
			exit(1);
		}
	}

	/* Numbering starts in the body at startnum. */
	state->section = NL_BODY;
	state->number = state->start;

	/* Reports where the operand starts. */
	return optind;
}

/* Parses a section type: a, t, n or pBRE. */
static int
read_type(
	const char *text,
	struct nl_type *type)
{
	int status;

	/* A pattern type compiles its expression. */
	if (text[0] == 'p') {
		status = regcomp(&type->pattern, text + 1, REG_NOSUB);
		if (status != 0)
			return -1;
		type->kind = NL_TYPE_PATTERN;
		return 0;
	}

	/* The other types are one letter. */
	if (text[0] != '\0' && text[1] == '\0') {
		if (text[0] == 'a') {
			type->kind = NL_TYPE_ALL;
			return 0;
		} else if (text[0] == 't') {
			type->kind = NL_TYPE_TEXT;
			return 0;
		} else if (text[0] == 'n') {
			type->kind = NL_TYPE_NONE;
			return 0;
		}
	}

	/* Anything else is not a type. */
	return -1;
}

/* Parses a decimal number, which must be positive when asked. */
static int
read_number(
	const char *text,
	long *value,
	int positive)
{
	char *end;
	long number;

	/* Converts the whole text. */
	errno = 0;
	number = strtol(text, &end, 10);
	if (end == text || *end != '\0' || errno != 0)
		return -1;

	/* Refuses zero or less where a positive number is needed. */
	if (positive && number <= 0)
		return -1;

	/* Succeeded: the number. */
	*value = number;
	return 0;
}

/* Reads lines and writes them, numbered by the section they are in. */
static int
number_stream(
	FILE *input,
	struct nl_state *state)
{
	char *line;
	size_t capacity;
	ssize_t got;
	size_t length;
	int section;
	int numbered;
	int failed;

	/* Reads each line. */
	line = NULL;
	capacity = 0;
	for (;;) {
		got = getline(&line, &capacity, input);
		if (got < 0)
			break;

		/* The line without its newline. */
		length = (size_t)got;
		if (length > 0 && line[length - 1] == '\n')
			length--;

		/* A delimiter line starts a section and is written empty. */
		section = delimiter_section(state, line, length);
		if (section >= 0) {
			state->section = section;
			if (!state->keep_numbers)
				state->number = state->start;
			state->blanks = 0;
			putchar('\n');
			continue;
		}

		/* Writes the number, or spaces as wide, then the line. */
		numbered = wants_number(state, line, length);
		if (numbered) {
			write_number(state);
			state->number += state->increment;
		} else {
			printf("%*s", state->width + (int)strlen(state->separator), "");
		}

		/* Then the line itself. */
		fwrite(line, 1, length, stdout);
		putchar('\n');
	}

	/* Tells a read error from the end of the file. */
	free(line);
	failed = ferror(input);
	if (failed)
		return -1;

	/* Succeeded: every line was numbered. */
	return 0;
}

/*
 * Tells which section a delimiter line starts, or -1 for another line.
 * The delimiter three times is the header, twice the body, once the
 * footer.
 */
static int
delimiter_section(
	const struct nl_state *state,
	const char *line,
	size_t length)
{
	size_t count;
	size_t index;

	/* The line must be whole delimiters, one to three of them. */
	if (length == 0 || length % 2 != 0 || length > 6)
		return -1;
	count = length / 2;
	for (index = 0; index < length; index += 2) {
		if (line[index] != state->delimiter[0])
			return -1;
		if (line[index + 1] != state->delimiter[1])
			return -1;
	}

	/* Three is the header, two the body, one the footer. */
	if (count == 3)
		return NL_HEADER;
	if (count == 2)
		return NL_BODY;
	return NL_FOOTER;
}

/*
 * Tells whether a line of the current section is numbered.  For type a
 * with -l, only every blank_group-th empty line in a row is.
 */
static int
wants_number(
	struct nl_state *state,
	const char *line,
	size_t length)
{
	struct nl_type *type;
	char *copy;
	int status;

	/* Counts empty lines in a row; any other line ends the run. */
	type = &state->types[state->section];
	if (length == 0)
		state->blanks++;
	else
		state->blanks = 0;

	/* Decides by the section's type. */
	switch (type->kind) {
	case NL_TYPE_ALL:
		/* An empty line counts only at the end of each group of -l. */
		if (length == 0 && state->blanks % state->blank_group != 0)
			return 0;
		return 1;
	case NL_TYPE_TEXT:
		if (length == 0)
			return 0;
		return 1;
	case NL_TYPE_PATTERN:
		/* The expression is matched against the line without its newline. */
		copy = malloc(length + 1);
		if (copy == NULL)
			return 0;
		memcpy(copy, line, length);
		copy[length] = '\0';
		status = regexec(&type->pattern, copy, 0, NULL, 0);
		free(copy);
		if (status == 0)
			return 1;
		return 0;
	default:
		return 0;
	}
}

/* Writes the current number in its format and width, then the separator. */
static void
write_number(
	const struct nl_state *state)
{
	int compare;

	/* ln: left, rz: right with zeros, rn: right with spaces. */
	compare = strcmp(state->format, "ln");
	if (compare == 0) {
		printf("%-*ld", state->width, state->number);
	} else {
		compare = strcmp(state->format, "rz");
		if (compare == 0)
			printf("%0*ld", state->width, state->number);
		else
			printf("%*ld", state->width, state->number);
	}

	/* Then the separator. */
	fputs(state->separator, stdout);
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr,
		"usage: nl [-p] [-b type] [-d delim] [-f type] [-h type] [-i incr]\n"
		"          [-l num] [-n format] [-s sep] [-v startnum] [-w width] [file]\n");
	exit(1);
}
