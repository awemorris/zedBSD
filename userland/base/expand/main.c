/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Converts tabs to spaces (POSIX XCU expand).
 *
 *	expand [-t tablist] [file...]
 *
 * Each tab becomes the spaces up to the next tab stop: every eight columns,
 * every n columns for -t n, or the listed columns for -t with a list,
 * after whose last stop a tab becomes one space.  A backspace moves the
 * column back by one and a newline starts again at column 0; every other
 * byte takes one column (the POSIX locale).  - or no file reads standard
 * input.
 */

#include "userland/base/expand/tabs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int expand_stream(FILE *input, const struct tab_stops *tabs);
static int expand_operand(const char *path, const struct tab_stops *tabs);
static void usage(void);

/*
 * Runs expand.
 */
int
main(
	int argc,
	char **argv)
{
	struct tab_stops tabs;
	int option;
	int index;
	int failed;
	int status;

	/* Reads -t; without it the stops are every eight columns. */
	tab_stops_default(&tabs);
	for (;;) {
		option = getopt(argc, argv, "t:");
		if (option == -1)
			break;

		/* -t is the only option. */
		if (option != 't')
			usage();
		status = tab_stops_parse("expand", optarg, &tabs);
		if (status != 0)
			return 1;
	}

	/* Expands each file, or standard input. */
	failed = 0;
	if (optind >= argc) {
		status = expand_operand("-", &tabs);
		if (status != 0)
			failed = 1;
	}

	/* Each file named, in turn. */
	for (index = optind; index < argc; index++) {
		status = expand_operand(argv[index], &tabs);
		if (status != 0)
			failed = 1;
	}

	/* A failed write is an error too. */
	status = fflush(stdout);
	if (status != 0) {
		fprintf(stderr, "expand: write error: %s\n", strerror(errno));
		return 1;
	}

	/* Reports whether any file could not be read. */
	if (failed)
		return 1;

	/* Succeeded: every file was expanded. */
	return 0;
}

/* Opens one operand, - being standard input, and expands it. */
static int
expand_operand(
	const char *path,
	const struct tab_stops *tabs)
{
	FILE *input;
	int status;
	int compare;

	/* - is standard input. */
	compare = strcmp(path, "-");
	if (compare == 0) {
		status = expand_stream(stdin, tabs);
		if (status != 0) {
			fprintf(stderr, "expand: standard input: %s\n", strerror(errno));
			return -1;
		}

		/* Succeeded: standard input was expanded. */
		return 0;
	}

	/* Opens the file. */
	input = fopen(path, "r");
	if (input == NULL) {
		fprintf(stderr, "expand: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Expands it and closes it. */
	status = expand_stream(input, tabs);
	if (status != 0)
		fprintf(stderr, "expand: %s: %s\n", path, strerror(errno));
	fclose(input);

	/* Reports a read error. */
	if (status != 0)
		return -1;

	/* Succeeded: the file was expanded. */
	return 0;
}

/*
 * Copies a stream to standard output with its tabs expanded.  Returns -1
 * after a read error.
 */
static int
expand_stream(
	FILE *input,
	const struct tab_stops *tabs)
{
	size_t column;
	size_t next;
	int byte;
	int status;
	int failed;

	/* Reads byte by byte, keeping the column of the next one. */
	column = 0;
	for (;;) {
		byte = getc(input);
		if (byte == EOF)
			break;

		/* A tab becomes spaces up to the next stop. */
		if (byte == '\t') {
			status = tab_stops_next(tabs, column, &next);
			if (status != 0)
				next = column + 1;
			while (column < next) {
				putchar(' ');
				column++;
			}

			/* The tab has been replaced. */
			continue;
		}

		/* Copies any other byte, moving the column as it moves. */
		putchar(byte);
		if (byte == '\n')
			column = 0;
		else if (byte == '\b' && column > 0)
			column--;
		else if (byte != '\b')
			column++;
	}

	/* Tells a read error from the end of the file. */
	failed = ferror(input);
	if (failed)
		return -1;

	/* Succeeded: the whole stream was copied. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: expand [-t tablist] [file...]\n");
	exit(1);
}
