/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Invokes a utility immune to hangups (POSIX XCU nohup).
 *
 *	nohup utility [argument...]
 *
 * SIGHUP is ignored and the utility is run in nohup's place.  When
 * standard output is a terminal, the utility's output is appended to
 * nohup.out in the current directory, or to $HOME/nohup.out when that
 * cannot be opened, and the file used is named on standard error.  When
 * standard error is a terminal it goes where standard output goes.  A
 * terminal on standard input is replaced by /dev/null.
 *
 * nohup exits with 126 when the utility was found but could not be run
 * and with 127 when it could not be found or nohup itself failed.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The status when the utility was found but could not be run. */
#define NOHUP_STATUS_NOT_RUNNABLE 126

/* The status when the utility could not be found or nohup failed. */
#define NOHUP_STATUS_FAILED 127

static int redirect_input(void);
static int redirect_output(void);
static int open_output_file(char *name, size_t size);
static int is_open(int descriptor);

/*
 * Runs nohup.
 */
int
main(
	int argc,
	char **argv)
{
	void (*previous)(int);
	int first;
	int error;
	int compare;

	/* Only -- may come before the utility. */
	first = 1;
	if (argc > 1) {
		compare = strcmp(argv[1], "--");
		if (compare == 0)
			first = 2;
	}

	/* Refuses a missing utility. */
	if (first >= argc) {
		fprintf(stderr, "usage: nohup utility [argument...]\n");
		return NOHUP_STATUS_FAILED;
	}

	/* Makes the utility immune to the hangup of its terminal. */
	previous = signal(SIGHUP, SIG_IGN);
	if (previous == SIG_ERR) {
		fprintf(stderr, "nohup: SIGHUP: %s\n", strerror(errno));
		return NOHUP_STATUS_FAILED;
	}

	/* Keeps the utility off a terminal on standard input. */
	error = redirect_input();
	if (error != 0)
		return NOHUP_STATUS_FAILED;

	/* Sends terminal output to nohup.out. */
	error = redirect_output();
	if (error != 0)
		return NOHUP_STATUS_FAILED;

	/* Runs the utility in nohup's place. */
	execvp(argv[first], argv + first);

	/* The exec failed: tells whether the utility was found at all. */
	error = errno;
	fprintf(stderr, "nohup: %s: %s\n", argv[first], strerror(error));
	if (error == ENOENT)
		return NOHUP_STATUS_FAILED;

	/* The utility exists but could not be run. */
	return NOHUP_STATUS_NOT_RUNNABLE;
}

/* Replaces a terminal on standard input with /dev/null. */
static int
redirect_input(void)
{
	int terminal;
	int descriptor;
	int duplicated;

	/* Leaves any other standard input alone. */
	terminal = isatty(STDIN_FILENO);
	if (!terminal)
		return 0;

	/* Opens /dev/null in its place. */
	descriptor = open("/dev/null", O_RDONLY);
	if (descriptor < 0) {
		fprintf(stderr, "nohup: /dev/null: %s\n", strerror(errno));
		return -1;
	}

	/* Puts it on standard input. */
	duplicated = dup2(descriptor, STDIN_FILENO);
	if (duplicated < 0) {
		fprintf(stderr, "nohup: standard input: %s\n", strerror(errno));
		return -1;
	}

	/* Closes the spare descriptor. */
	if (descriptor != STDIN_FILENO)
		close(descriptor);

	/* Succeeded: standard input reads nothing. */
	return 0;
}

/*
 * Appends terminal output to nohup.out.  Standard output goes there when
 * it is a terminal; standard error goes there when it is a terminal and
 * standard output is a terminal or closed, and otherwise follows standard
 * output.
 */
static int
redirect_output(void)
{
	char name[PATH_MAX];
	int output_terminal;
	int error_terminal;
	int output_open;
	int descriptor;
	int duplicated;

	/* Learns which of the two outputs are terminals. */
	output_terminal = isatty(STDOUT_FILENO);
	error_terminal = isatty(STDERR_FILENO);
	output_open = is_open(STDOUT_FILENO);

	/* Nothing to do when neither output is a terminal. */
	if (!output_terminal && !error_terminal)
		return 0;

	/* Standard error follows an open standard output that is no terminal. */
	if (!output_terminal && output_open) {
		duplicated = dup2(STDOUT_FILENO, STDERR_FILENO);
		if (duplicated < 0) {
			fprintf(stderr, "nohup: standard error: %s\n", strerror(errno));
			return -1;
		}

		/* Succeeded: both outputs go where standard output goes. */
		return 0;
	}

	/* Opens nohup.out, here or in the home directory. */
	descriptor = open_output_file(name, sizeof(name));
	if (descriptor < 0)
		return -1;

	/* Names the file while standard error is still the terminal. */
	if (output_terminal)
		fprintf(stderr, "nohup: appending output to %s\n", name);

	/* Puts the file on standard output when that is the terminal. */
	if (output_terminal) {
		duplicated = dup2(descriptor, STDOUT_FILENO);
		if (duplicated < 0) {
			fprintf(stderr, "nohup: standard output: %s\n", strerror(errno));
			return -1;
		}
	}

	/* Puts the file on standard error when that is a terminal. */
	if (error_terminal) {
		duplicated = dup2(descriptor, STDERR_FILENO);
		if (duplicated < 0)
			return -1;
	}

	/* Closes the spare descriptor. */
	if (descriptor > STDERR_FILENO)
		close(descriptor);

	/* Succeeded: terminal output goes to the file. */
	return 0;
}

/*
 * Opens nohup.out for appending, creating it readable and writable by the
 * owner only, in the current directory or else in $HOME.  Stores the name
 * used.
 */
static int
open_output_file(
	char *name,
	size_t size)
{
	const char *home;
	int descriptor;
	int written;
	int error;

	/* Tries the current directory first. */
	snprintf(name, size, "nohup.out");
	descriptor = open(name, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
	if (descriptor >= 0)
		return descriptor;
	error = errno;

	/* Then the home directory. */
	home = getenv("HOME");
	if (home != NULL && home[0] != '\0') {
		written = snprintf(name, size, "%s/nohup.out", home);
		if (written > 0 && (size_t)written < size) {
			descriptor = open(name, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
			if (descriptor >= 0)
				return descriptor;
			error = errno;
		}
	}

	/* Neither could be opened; the utility is not run. */
	fprintf(stderr, "nohup: cannot open nohup.out: %s\n", strerror(error));
	return -1;
}

/* Tells whether a descriptor is open. */
static int
is_open(
	int descriptor)
{
	int flags;

	/* A closed descriptor has no descriptor flags. */
	flags = fcntl(descriptor, F_GETFD);
	if (flags < 0)
		return 0;

	/* Succeeded: the descriptor is open. */
	return 1;
}
