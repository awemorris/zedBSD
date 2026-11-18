/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD tabs userland command.
 *
 * The stops come from one predefined form (-a, -a2, -c, -c2, -c3, -f, -p,
 * -s, -u), a uniform distance (-1 to -9; -0 sets none), or a list of
 * columns (n, n,+m, n m..., in one operand or several).  The terminal's
 * stops are cleared (tbc) and each new one is set by moving the cursor to
 * it (cuf) and setting it there (hts).  The width is COLUMNS, else the
 * terminal's own (TIOCGWINSZ), else the terminfo entry's cols; a stop past
 * it is not set.  Without TERM the entry is vt100's (ws001-p043).
 */

#include "userland/base/common/command.h"
#include "userland/base/common/terminfo.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The terminal type when TERM is not set (XCU: an unspecified default). */
#define TABS_DEFAULT_TYPE "vt100"

/* The widest terminal the stops are set for. */
#define TABS_WIDTH_MAX 4096U

/* The width when nothing gives one. */
#define TABS_WIDTH_DEFAULT 80U

/* The distance of the stops without an option (tabs -8). */
#define TABS_DEFAULT_UNIFORM 8U

/* One predefined form: its option and its columns. */
struct tabs_form {
	const char *option;
	const unsigned *columns;
	size_t count;
};

/* The stops being made: the columns, their count, and the width. */
struct tabs_stops {
	unsigned *columns;
	size_t count;
	unsigned width;
};

static const unsigned form_assembler[] = {1, 10, 16, 36, 72};
static const unsigned form_assembler2[] = {1, 10, 16, 40, 72};
static const unsigned form_cobol[] = {1, 8, 12, 16, 20, 55};
static const unsigned form_cobol2[] = {1, 6, 10, 14, 49};
static const unsigned form_cobol3[] = {1, 6, 10, 14, 18, 22, 26, 30, 34, 38, 42, 46, 50, 54, 58, 62, 67};
static const unsigned form_fortran[] = {1, 7, 11, 15, 19, 23};
static const unsigned form_pl1[] = {1, 5, 9, 13, 17, 21, 25, 29, 33, 37, 41, 45, 49, 53, 57, 61};
static const unsigned form_snobol[] = {1, 10, 55};
static const unsigned form_univac[] = {1, 12, 20, 44};

/* The predefined forms of XCU. */
static const struct tabs_form tabs_forms[] = {
	{"-a", form_assembler, sizeof(form_assembler) / sizeof(form_assembler[0])},
	{"-a2", form_assembler2, sizeof(form_assembler2) / sizeof(form_assembler2[0])},
	{"-c", form_cobol, sizeof(form_cobol) / sizeof(form_cobol[0])},
	{"-c2", form_cobol2, sizeof(form_cobol2) / sizeof(form_cobol2[0])},
	{"-c3", form_cobol3, sizeof(form_cobol3) / sizeof(form_cobol3[0])},
	{"-f", form_fortran, sizeof(form_fortran) / sizeof(form_fortran[0])},
	{"-p", form_pl1, sizeof(form_pl1) / sizeof(form_pl1[0])},
	{"-s", form_snobol, sizeof(form_snobol) / sizeof(form_snobol[0])},
	{"-u", form_univac, sizeof(form_univac) / sizeof(form_univac[0])},
	{NULL, NULL, 0}
};

static const struct tabs_form *find_form(const char *option);
static unsigned terminal_width(const struct terminfo *terminal);
static int explicit_stops(const char *text, struct tabs_stops *stops, unsigned *previous);
static int append_stop(struct tabs_stops *stops, unsigned column);
static void uniform_stops(struct tabs_stops *stops, unsigned every);
static int set_stops(const struct terminfo *terminal, const struct tabs_stops *stops);
static int emit_capability(const struct terminfo_capability *capability, long parameter);
static void usage(void);

/*
 * Runs the tabs command.
 */
int
main(
	int argc,
	char **argv)
{
	const struct tabs_form *form;
	const struct tabs_form *found;
	struct terminfo terminal;
	struct tabs_stops stops;
	const char *type;
	const char *directory;
	unsigned uniform;
	unsigned previous;
	size_t item;
	int index;
	int error;
	int compared;
	int valid;

	/* The options: a form or a distance (the last wins), and -T. */
	type = NULL;
	form = NULL;
	uniform = TABS_DEFAULT_UNIFORM;
	index = 1;
	while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
		/* "--" ends them. */
		compared = strcmp(argv[index], "--");
		if (compared == 0) {
			index++;
			break;
		}

		/* -T type, which needs its operand. */
		compared = strcmp(argv[index], "-T");
		if (compared == 0 && index + 1 >= argc) {
			usage();
			return 2;
		}

		/* The type taken. */
		if (compared == 0) {
			type = argv[index + 1];
			index += 2;
			continue;
		}

		/* A distance -0 to -9. */
		if (argv[index][1] >= '0' && argv[index][1] <= '9' && argv[index][2] == '\0') {
			uniform = (unsigned)(argv[index][1] - '0');
			form = NULL;
			index++;
			continue;
		}

		/* A predefined form. */
		found = find_form(argv[index]);
		if (found == NULL) {
			usage();
			return 2;
		}

		/* The last one wins. */
		form = found;
		index++;
	}

	/* The terminal: -T, TERM, or the default. */
	if (type == NULL)
		type = getenv("TERM");
	if (type == NULL || type[0] == '\0')
		type = TABS_DEFAULT_TYPE;
	directory = getenv("TERMINFO");
	error = terminfo_load(&terminal, type, directory);
	if (error != 0) {
		fprintf(stderr, "tabs: %s: unknown or invalid terminal\n", type);
		return 3;
	}

	/* Room for a stop at every column. */
	stops.width = terminal_width(&terminal);
	stops.count = 0;
	stops.columns = calloc(stops.width, sizeof(stops.columns[0]));
	if (stops.columns == NULL) {
		fprintf(stderr, "tabs: %s\n", strerror(ENOMEM));
		return 1;
	}

	/* The stops: the operands' list, a form, or the distance. */
	valid = 1;
	if (index < argc) {
		previous = 0;
		for (; index < argc && valid; index++)
			valid = explicit_stops(argv[index], &stops, &previous);
		if (previous == 0)
			valid = 0;
	} else if (form != NULL) {
		for (item = 0; item < form->count && valid; item++)
			valid = append_stop(&stops, form->columns[item]);
	} else {
		uniform_stops(&stops, uniform);
	}

	/* A list that is not one. */
	if (!valid) {
		free(stops.columns);
		usage();
		return 2;
	}

	/* The terminal's stops. */
	error = set_stops(&terminal, &stops);
	free(stops.columns);
	if (error != 0) {
		fprintf(stderr, "tabs: terminal does not support tab programming\n");
		return 1;
	}

	/* Set. */
	return 0;
}

/* The predefined form of an option, or NULL. */
static const struct tabs_form *
find_form(
	const char *option)
{
	size_t index;
	int compared;

	/* Each form. */
	for (index = 0; tabs_forms[index].option != NULL; index++) {
		compared = strcmp(tabs_forms[index].option, option);
		if (compared == 0)
			return &tabs_forms[index];
	}

	/* None. */
	return NULL;
}

/*
 * The width the stops are set for: COLUMNS when it is a number, else the
 * terminal's on standard output, else the entry's cols, else 80; at most
 * TABS_WIDTH_MAX.
 */
static unsigned
terminal_width(
	const struct terminfo *terminal)
{
	const struct terminfo_capability *columns;
	struct winsize window;
	const char *text;
	unsigned long value;
	char *end;
	int error;

	/* COLUMNS. */
	text = getenv("COLUMNS");
	if (text != NULL && text[0] >= '1' && text[0] <= '9') {
		errno = 0;
		value = strtoul(text, &end, 10);
		if (errno == 0 && *end == '\0' && value > TABS_WIDTH_MAX)
			return TABS_WIDTH_MAX;
		if (errno == 0 && *end == '\0')
			return (unsigned)value;
	}

	/* The terminal's own. */
	memset(&window, 0, sizeof(window));
	error = ioctl(STDOUT_FILENO, TIOCGWINSZ, &window);
	if (error == 0 && window.ws_col > TABS_WIDTH_MAX)
		return TABS_WIDTH_MAX;
	if (error == 0 && window.ws_col > 0)
		return window.ws_col;

	/* The entry's. */
	columns = terminfo_find(terminal, "cols");
	if (columns != NULL && columns->kind == TERMINFO_NUMBER) {
		/* A number, at most the widest. */
		if (columns->number > (long)TABS_WIDTH_MAX)
			return TABS_WIDTH_MAX;
		if (columns->number > 0)
			return (unsigned)columns->number;
	}

	/* The usual width. */
	return TABS_WIDTH_DEFAULT;
}

/*
 * Reads one operand of the list: columns separated by commas or blanks,
 * each after the first of the whole list either a column or +n (n past the
 * one before).  previous is the last column read (0 before the first).
 * Returns 1, or 0 for a list that is not one (a column not past the one
 * before, a zero, or other text).
 */
static int
explicit_stops(
	const char *text,
	struct tabs_stops *stops,
	unsigned *previous)
{
	const char *cursor;
	unsigned long value;
	char *end;
	int relative;
	int added;

	/* Each column. */
	cursor = text;
	for (;;) {
		/* The separators before it. */
		while (*cursor == ',' || *cursor == ' ' || *cursor == '\t')
			cursor++;
		if (*cursor == '\0')
			break;

		/* +n, after the first. */
		relative = 0;
		if (*cursor == '+')
			relative = 1;
		if (relative && *previous == 0)
			return 0;
		if (relative)
			cursor++;

		/* The number. */
		if (*cursor < '0' || *cursor > '9')
			return 0;
		errno = 0;
		value = strtoul(cursor, &end, 10);
		if (errno != 0 || value > UINT_MAX)
			return 0;
		if (*end != '\0' && *end != ',' && *end != ' ' && *end != '\t')
			return 0;

		/* The column, past the one before. */
		if (relative && value > UINT_MAX - *previous)
			return 0;
		if (relative)
			value += *previous;
		if (value == 0 || value <= *previous)
			return 0;
		added = append_stop(stops, (unsigned)value);
		if (!added)
			return 0;
		*previous = (unsigned)value;
		cursor = end;
	}

	/* Read through. */
	return 1;
}

/*
 * Adds a stop at a column (1 and the columns past the width are not set:
 * the cursor is at 1 already, and the terminal has no column there).
 * Returns 1, or 0 for a column not past the last one.
 */
static int
append_stop(
	struct tabs_stops *stops,
	unsigned column)
{
	/* In order. */
	if (stops->count > 0 && column <= stops->columns[stops->count - 1U])
		return 0;

	/* Within the width. */
	if (column > 1U && column <= stops->width)
		stops->columns[stops->count++] = column;
	return 1;
}

/* Adds a stop every so many columns (none for 0). */
static void
uniform_stops(
	struct tabs_stops *stops,
	unsigned every)
{
	unsigned column;

	/* None for 0. */
	if (every == 0)
		return;

	/* 1 + every, 1 + 2 * every, ... within the width. */
	for (column = 1U + every; column <= stops->width; column += every)
		stops->columns[stops->count++] = column;
}

/*
 * Clears the terminal's stops and sets the new ones from the left
 * margin.  Returns 0, or -1 when the terminal cannot.
 */
static int
set_stops(
	const struct terminfo *terminal,
	const struct tabs_stops *stops)
{
	const struct terminfo_capability *carriage;
	const struct terminfo_capability *clear;
	const struct terminfo_capability *forward;
	const struct terminfo_capability *set;
	unsigned position;
	size_t index;
	int error;

	/* The capabilities. */
	carriage = terminfo_find(terminal, "cr");
	clear = terminfo_find(terminal, "tbc");
	forward = terminfo_find(terminal, "cuf");
	set = terminfo_find(terminal, "hts");

	/* The old stops go. */
	error = emit_capability(carriage, 0);
	if (error == 0)
		error = emit_capability(clear, 0);

	/* Each new one (past column 1, so the move is never 0): there, then set. */
	position = 1;
	for (index = 0; index < stops->count && error == 0; index++) {
		error = emit_capability(forward, (long)(stops->columns[index] - position));
		if (error == 0)
			error = emit_capability(set, 0);
		position = stops->columns[index];
	}

	/* Back to the margin. */
	if (error == 0)
		error = emit_capability(carriage, 0);
	return error;
}

/* Writes a string capability with one parameter; 0, or -1 when there is none or it cannot be written. */
static int
emit_capability(
	const struct terminfo_capability *capability,
	long parameter)
{
	long parameters[9] = {parameter, 0};
	char expanded[1024];
	int length;
	int written;

	/* A string. */
	if (capability == NULL || capability->kind != TERMINFO_STRING)
		return -1;

	/* Expanded and written. */
	length = terminfo_expand(capability->string, parameters, expanded, sizeof(expanded));
	if (length < 0)
		return -1;
	written = command_write_all(STDOUT_FILENO, expanded, strlen(expanded));
	if (written != 0)
		return -1;
	return 0;
}

/* Writes the usage lines. */
static void
usage(void)
{
	/* The two forms of the command. */
	fprintf(stderr, "usage: tabs [-0..-9|-a|-a2|-c|-c2|-c3|-f|-p|-s|-u] [-T type]\n"
			"       tabs [-T type] n[[sep[+]n]...]\n");
}
