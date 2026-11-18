/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Selects or rejects lines common to two files (POSIX XCU comm).
 *
 *	comm [-123] file1 file2
 *
 * Both files must be sorted in the collating sequence of the current
 * locale.  Lines only in file1 are written in column 1, lines only in
 * file2 in column 2, and lines in both in column 3; a column is preceded
 * by one tab for each earlier column that is written.  -1, -2 and -3
 * suppress their columns.  - names standard input.
 */

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * One input file and the line read from it.
 *
 * line holds the current line without its newline; done says the file
 * has no more lines.
 */
struct comm_input {
	const char *name;
	FILE *stream;
	char *line;
	size_t capacity;
	size_t length;
	int done;
};

static int open_input(struct comm_input *input, const char *name);
static int read_line(struct comm_input *input);
static void write_column(int column, const int *show, const struct comm_input *input);
static void usage(void);

/*
 * Runs comm.
 */
int
main(
	int argc,
	char **argv)
{
	struct comm_input first;
	struct comm_input second;
	int show[4];
	int option;
	int compare;
	int status;
	int failed;

	/* Collates in the locale of the environment. */
	setlocale(LC_ALL, "");

	/* Reads -1, -2 and -3. */
	show[1] = 1;
	show[2] = 1;
	show[3] = 1;
	for (;;) {
		option = getopt(argc, argv, "123");
		if (option == -1)
			break;

		/* Suppresses the column named. */
		if (option == '1')
			show[1] = 0;
		else if (option == '2')
			show[2] = 0;
		else if (option == '3')
			show[3] = 0;
		else
			usage();
	}

	/* Exactly two files. */
	if (argc - optind != 2)
		usage();

	/* Opens both files. */
	status = open_input(&first, argv[optind]);
	if (status != 0)
		return 1;
	status = open_input(&second, argv[optind + 1]);
	if (status != 0)
		return 1;

	/* Reads the first line of each. */
	failed = 0;
	status = read_line(&first);
	if (status == 0)
		status = read_line(&second);
	if (status != 0)
		failed = 1;

	/* Merges the two files line by line. */
	while (!failed && (!first.done || !second.done)) {
		/* Compares the current lines; a finished file sorts last. */
		if (first.done)
			compare = 1;
		else if (second.done)
			compare = -1;
		else
			compare = strcoll(first.line, second.line);

		/* Writes the smaller line in its column, or a common line. */
		if (compare < 0) {
			write_column(1, show, &first);
			status = read_line(&first);
		} else if (compare > 0) {
			write_column(2, show, &second);
			status = read_line(&second);
		} else {
			write_column(3, show, &first);
			status = read_line(&first);
			if (status == 0)
				status = read_line(&second);
		}

		/* A read error ends the merge. */
		if (status != 0)
			failed = 1;
	}

	/* A failed write is an error too. */
	status = fflush(stdout);
	if (status != 0) {
		fprintf(stderr, "comm: write error: %s\n", strerror(errno));
		return 1;
	}

	/* Reports a read error. */
	if (failed)
		return 1;

	/* Succeeded: both files were compared. */
	return 0;
}

/* Opens one input file, - being standard input. */
static int
open_input(
	struct comm_input *input,
	const char *name)
{
	int compare;

	/* Nothing read yet. */
	memset(input, 0, sizeof(*input));
	input->name = name;

	/* - is standard input. */
	compare = strcmp(name, "-");
	if (compare == 0) {
		input->stream = stdin;
		return 0;
	}

	/* Opens the file. */
	input->stream = fopen(name, "r");
	if (input->stream == NULL) {
		fprintf(stderr, "comm: %s: %s\n", name, strerror(errno));
		return -1;
	}

	/* Succeeded: the file is open. */
	return 0;
}

/*
 * Reads the next line of an input, without its newline.  At the end sets
 * done; returns -1 after a read error.
 */
static int
read_line(
	struct comm_input *input)
{
	ssize_t got;
	int failed;

	/* Reads one line. */
	got = getline(&input->line, &input->capacity, input->stream);
	if (got < 0) {
		input->done = 1;
		failed = ferror(input->stream);
		if (failed) {
			fprintf(stderr, "comm: %s: %s\n", input->name, strerror(errno));
			return -1;
		}

		/* The end of the file is not an error. */
		return 0;
	}

	/* Drops the newline. */
	input->length = (size_t)got;
	if (input->length > 0 && input->line[input->length - 1] == '\n') {
		input->length--;
		input->line[input->length] = '\0';
	}

	/* Succeeded: a line. */
	return 0;
}

/*
 * Writes a line in a column, after one tab for each earlier column that
 * is shown.
 */
static void
write_column(
	int column,
	const int *show,
	const struct comm_input *input)
{
	int earlier;

	/* A suppressed column writes nothing. */
	if (!show[column])
		return;

	/* One tab for each earlier column shown. */
	for (earlier = 1; earlier < column; earlier++) {
		if (show[earlier])
			putchar('\t');
	}

	/* The line and its newline. */
	fwrite(input->line, 1, input->length, stdout);
	putchar('\n');
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: comm [-123] file1 file2\n");
	exit(1);
}
