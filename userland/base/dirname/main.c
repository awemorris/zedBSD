/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Returns the directory portion of a pathname (POSIX XCU dirname).
 *
 *	dirname [-z] string...
 *
 * For each string, dirname writes it without its last component: the
 * trailing slashes and the last component are removed, then the slashes
 * before it.  A string without a slash gives ".", and one of slashes only
 * gives "/".
 *
 * POSIX names one string; several are taken as GNU dirname takes them,
 * each result on its own line (the user's decision for WS045, 2026-09-27).
 * GNU's -z (--zero) ends each result with a NUL byte instead of a
 * newline.  The options end at the first string, or at --.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/kern/os-name.h"

/* The codes of the long options that have no letter. */
#define OPTION_HELP 256
#define OPTION_VERSION 257

/*
 * The options written in full, read by the scan of the command line; a
 * long option shares its code with its letter.
 */
static const struct command_long_option dirname_long_options[] = {
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{"zero", COMMAND_VALUE_NONE, 'z'},
	{NULL, 0, 0}
};

static const char *directory_of(const char *path, size_t *length);
static void usage(void);

/*
 * Runs dirname.
 */
int
main(
	int argc,
	char **argv)
{
	struct command_options scan;
	const char *directory;
	size_t length;
	int option;
	int end;
	int index;
	int status;

	/* Reads -z; the strings are left from argv[1] on. */
	end = '\n';
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "dirname";
	scan.letters = "z";
	scan.names = dirname_long_options;
	command_options_start(&scan);
	for (;;) {
		option = command_options_next(&scan);
		if (option == COMMAND_OPTION_END)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'z':
			end = '\0';
			break;
		case OPTION_VERSION:
			printf("dirname (" ZEDBSD_OS_NAME ") 1.0\n");
			return 0;
		default:
			usage();
			break;
		}
	}

	/* At least one string. */
	if (scan.operand_count < 1)
		usage();

	/* Writes the directory of each string. */
	for (index = 1; index <= scan.operand_count; index++) {
		directory = directory_of(argv[index], &length);
		fwrite(directory, 1, length, stdout);
		putchar(end);
	}

	/* A failed write is a failure. */
	status = fflush(stdout);
	if (status != 0) {
		command_error("dirname", "standard output");
		return 1;
	}

	/* Succeeded: every directory was written. */
	return 0;
}

/*
 * Finds the directory of a pathname: the start of the pathname and the
 * length of its directory, or "." when it has no slash.
 */
static const char *
directory_of(
	const char *path,
	size_t *length)
{
	size_t end;

	/* Drops the trailing slashes; slashes only are the root. */
	end = strlen(path);
	while (end > 1 && path[end - 1] == '/')
		end--;
	if (end == 1 && path[0] == '/') {
		*length = 1;
		return path;
	}

	/* Drops the last component; none left (no slash) is ".". */
	while (end > 0 && path[end - 1] != '/')
		end--;
	if (end == 0) {
		*length = 1;
		return ".";
	}

	/* Succeeded: the slashes before it go too, keeping one for the root. */
	while (end > 1 && path[end - 1] == '/')
		end--;
	*length = end;
	return path;
}

/* Writes the usage message and exits. */
static void
usage(void)
{
	/* Names the form. */
	fprintf(stderr, "usage: dirname [-z] string...\n");
	exit(1);
}
