/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads and applies the -t tablist of expand and unexpand.
 *
 * A tablist is one positive number, the distance between repeating tab
 * stops, or several positive ascending numbers separated by commas or
 * blanks, the columns of the stops.  A column number n in the list is the
 * position after n characters, which is column n counted from 0.
 */

#include "userland/base/expand/tabs.h"
#include <stdio.h>

/* The distance between tab stops without -t. */
#define TAB_DEFAULT_DISTANCE 8

static int is_separator(int letter);

/*
 * Parses a tablist.  Writes a diagnostic and returns -1 for an invalid
 * one.
 */
int
tab_stops_parse(
	const char *program,
	const char *text,
	struct tab_stops *tabs)
{
	const char *cursor;
	size_t value;
	int digits;
	int separator;

	/* Reads the numbers one by one. */
	tabs->count = 0;
	cursor = text;
	for (;;) {
		/* Skips the separators before a number. */
		for (;;) {
			separator = is_separator(*cursor);
			if (!separator)
				break;
			cursor++;
		}

		/* The list ends here. */
		if (*cursor == '\0')
			break;

		/* Reads the digits of the number. */
		value = 0;
		digits = 0;
		while (*cursor >= '0' && *cursor <= '9') {
			value = value * 10 + (size_t)(*cursor - '0');
			digits++;
			cursor++;

			/* A column this far is refused rather than wrapped. */
			if (value > 1000000000U) {
				fprintf(stderr, "%s: tab stop is too large: '%s'\n", program, text);
				return -1;
			}
		}

		/* Anything but a digit or a separator is an error. */
		if (digits == 0) {
			fprintf(stderr, "%s: invalid character in tab list: '%s'\n", program, text);
			return -1;
		}

		/* The number must end at a separator or at the end. */
		separator = is_separator(*cursor);
		if (*cursor != '\0' && !separator) {
			fprintf(stderr, "%s: invalid character in tab list: '%s'\n", program, text);
			return -1;
		}

		/* A stop at column 0 means nothing. */
		if (value == 0) {
			fprintf(stderr, "%s: tab size cannot be 0\n", program);
			return -1;
		}

		/* The stops must ascend. */
		if (tabs->count > 0 && value <= tabs->stops[tabs->count - 1]) {
			fprintf(stderr, "%s: tab sizes must be ascending\n", program);
			return -1;
		}

		/* The list has room for a limited number of stops. */
		if (tabs->count >= TAB_STOPS_MAX) {
			fprintf(stderr, "%s: too many tab stops: '%s'\n", program, text);
			return -1;
		}

		/* Keeps the stop. */
		tabs->stops[tabs->count] = value;
		tabs->count++;
	}

	/* An empty list keeps the default stops, as on other systems. */
	if (tabs->count == 0)
		tab_stops_default(tabs);

	/* Succeeded: the stops. */
	return 0;
}

/* Sets the default stops: every eight columns. */
void
tab_stops_default(
	struct tab_stops *tabs)
{
	/* One repeating distance. */
	tabs->stops[0] = TAB_DEFAULT_DISTANCE;
	tabs->count = 1;
}

/*
 * Finds the first tab stop after a column.  Returns 0 and stores it, or
 * -1 when the column is at or past the last stop of a list.
 */
int
tab_stops_next(
	const struct tab_stops *tabs,
	size_t column,
	size_t *next)
{
	size_t index;

	/* Repeating stops are the next multiple of the distance. */
	if (tabs->count == 1) {
		*next = (column / tabs->stops[0] + 1) * tabs->stops[0];
		return 0;
	}

	/* Listed stops: the first one past the column. */
	for (index = 0; index < tabs->count; index++) {
		if (tabs->stops[index] > column) {
			*next = tabs->stops[index];
			return 0;
		}
	}

	/* Past the last stop there is none. */
	return -1;
}

/* Tells whether a character separates the numbers of a tablist. */
static int
is_separator(
	int letter)
{
	/* Commas and blanks separate. */
	if (letter == ',')
		return 1;
	if (letter == ' ')
		return 1;
	if (letter == '\t')
		return 1;

	/* Anything else does not. */
	return 0;
}
