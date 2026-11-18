/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Converts spaces to tabs (POSIX XCU unexpand).
 *
 *	unexpand [-a|-t tablist] [file...]
 *
 * The blanks (spaces and tabs) at the start of each line are rewritten
 * with as many tabs as reach the tab stops they cover, and spaces for the
 * rest.  -a does the same for every run of blanks in a line, except that a
 * single space just before a tab stop stays a space.  -t sets the stops as
 * expand -t does and implies -a; blanks past the last stop of a list stay
 * as they are.  The columns count as in expand.
 */

#include "userland/base/expand/tabs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The state of one line being rewritten.
 *
 * column is the column after what has been read; the blanks read since
 * pending_start have not been written yet, and pending_tab says whether a
 * tab was among them.  converting is cleared at the first non-blank of a
 * line unless -a applies.
 */
struct unexpand_line {
	size_t column;
	size_t pending_start;
	int pending_tab;
	int converting;
};

static int unexpand_operand(const char *path, const struct tab_stops *tabs, int all);
static int unexpand_stream(FILE *input, const struct tab_stops *tabs, int all);
static void take_blank(struct unexpand_line *line, const struct tab_stops *tabs, int blank);
static void flush_pending(struct unexpand_line *line);
static void usage(void);

/*
 * Runs unexpand.
 */
int
main(
	int argc,
	char **argv)
{
	struct tab_stops tabs;
	int option;
	int all;
	int index;
	int failed;
	int status;

	/* Reads -a and -t; -t implies -a. */
	tab_stops_default(&tabs);
	all = 0;
	for (;;) {
		option = getopt(argc, argv, "at:");
		if (option == -1)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'a':
			all = 1;
			break;
		case 't':
			status = tab_stops_parse("unexpand", optarg, &tabs);
			if (status != 0)
				return 1;
			all = 1;
			break;
		default:
			usage();
			break;
		}
	}

	/* Rewrites each file, or standard input. */
	failed = 0;
	if (optind >= argc) {
		status = unexpand_operand("-", &tabs, all);
		if (status != 0)
			failed = 1;
	}

	/* Each file named, in turn. */
	for (index = optind; index < argc; index++) {
		status = unexpand_operand(argv[index], &tabs, all);
		if (status != 0)
			failed = 1;
	}

	/* A failed write is an error too. */
	status = fflush(stdout);
	if (status != 0) {
		fprintf(stderr, "unexpand: write error: %s\n", strerror(errno));
		return 1;
	}

	/* Reports whether any file could not be read. */
	if (failed)
		return 1;

	/* Succeeded: every file was rewritten. */
	return 0;
}

/* Opens one operand, - being standard input, and rewrites it. */
static int
unexpand_operand(
	const char *path,
	const struct tab_stops *tabs,
	int all)
{
	FILE *input;
	int status;
	int compare;

	/* - is standard input. */
	compare = strcmp(path, "-");
	if (compare == 0) {
		status = unexpand_stream(stdin, tabs, all);
		if (status != 0) {
			fprintf(stderr, "unexpand: standard input: %s\n", strerror(errno));
			return -1;
		}

		/* Succeeded: standard input was rewritten. */
		return 0;
	}

	/* Opens the file. */
	input = fopen(path, "r");
	if (input == NULL) {
		fprintf(stderr, "unexpand: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Rewrites it and closes it. */
	status = unexpand_stream(input, tabs, all);
	if (status != 0)
		fprintf(stderr, "unexpand: %s: %s\n", path, strerror(errno));
	fclose(input);

	/* Reports a read error. */
	if (status != 0)
		return -1;

	/* Succeeded: the file was rewritten. */
	return 0;
}

/*
 * Copies a stream to standard output with runs of blanks rewritten.
 * Returns -1 after a read error.
 */
static int
unexpand_stream(
	FILE *input,
	const struct tab_stops *tabs,
	int all)
{
	struct unexpand_line line;
	int byte;
	int failed;
	int status;

	/* Starts the first line. */
	memset(&line, 0, sizeof(line));
	line.converting = 1;

	/* Reads byte by byte. */
	for (;;) {
		byte = getc(input);
		if (byte == EOF)
			break;

		/* A blank where conversion applies waits to be rewritten. */
		if ((byte == ' ' || byte == '\t') && line.converting) {
			take_blank(&line, tabs, byte);
			continue;
		}

		/* Anything else first writes the blanks that wait. */
		flush_pending(&line);
		putchar(byte);

		/* Moves the column; a newline starts a new line. */
		if (byte == '\n') {
			line.column = 0;
			line.converting = 1;
		} else if (byte == '\b') {
			if (line.column > 0)
				line.column--;
		} else if (byte == '\t') {
			/* A tab written as it is still moves the column, by one past the last stop. */
			status = tab_stops_next(tabs, line.column, &line.column);
			if (status != 0)
				line.column++;
		} else {
			line.column++;
			if (!all && byte != ' ')
				line.converting = 0;
		}

		/* The next blanks wait from here. */
		line.pending_start = line.column;
		line.pending_tab = 0;
	}

	/* Writes the blanks left at the end of the input. */
	flush_pending(&line);

	/* Tells a read error from the end of the file. */
	failed = ferror(input);
	if (failed)
		return -1;

	/* Succeeded: the whole stream was copied. */
	return 0;
}

/*
 * Takes one blank of a run.  When the run reaches a tab stop it is
 * written as a tab, unless it is a single space.
 */
static void
take_blank(
	struct unexpand_line *line,
	const struct tab_stops *tabs,
	int blank)
{
	size_t stop;
	int status;

	/* The next stop after where the waiting run starts. */
	status = tab_stops_next(tabs, line->pending_start, &stop);

	/* Past the last stop of a list, blanks are written as they are. */
	if (status != 0) {
		flush_pending(line);
		putchar(blank);
		line->column++;
		line->pending_start = line->column;
		return;
	}

	/* Moves the column: a tab to the next stop, a space by one. */
	if (blank == '\t') {
		(void)tab_stops_next(tabs, line->column, &line->column);
		line->pending_tab = 1;
	} else {
		line->column++;
	}

	/* A run that reaches the stop is written as a tab, or a lone space. */
	if (line->column >= stop) {
		if (line->column - line->pending_start == 1 && !line->pending_tab)
			putchar(' ');
		else
			putchar('\t');
		line->pending_start = stop;
		line->pending_tab = 0;
	}
}

/* Writes the blanks that wait as spaces. */
static void
flush_pending(
	struct unexpand_line *line)
{
	size_t column;

	/* One space for each column the run covers. */
	for (column = line->pending_start; column < line->column; column++)
		putchar(' ');

	/* Nothing waits any more. */
	line->pending_start = line->column;
	line->pending_tab = 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: unexpand [-a|-t tablist] [file...]\n");
	exit(1);
}
