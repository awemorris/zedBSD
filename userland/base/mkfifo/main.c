/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Makes FIFO special files (POSIX XCU mkfifo).
 *
 *	mkfifo [-m mode] file...
 *
 * Each FIFO is made with rw for all, less the file mode creation mask; -m
 * gives it exactly the mode instead, a symbolic mode counting from a=rw.
 */

#include "userland/base/chmod/mode.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(void);

/*
 * Runs mkfifo.
 */
int
main(
	int argc,
	char **argv)
{
	const char *mode_text;
	mode_t mask;
	mode_t mode;
	int option;
	int index;
	int failed;
	int status;

	/* Without -m the mode is rw for all less the mask. */
	mask = umask(0);
	umask(mask);
	mode = 0666 & ~mask;
	mode_text = NULL;

	/* Reads -m. */
	for (;;) {
		option = getopt(argc, argv, "m:");
		if (option == -1)
			break;

		/* -m is the only option. */
		if (option != 'm')
			usage();
		mode_text = optarg;
	}

	/* At least one file is named. */
	if (optind >= argc)
		usage();

	/* Computes the -m mode from a=rw. */
	if (mode_text != NULL) {
		status = mode_apply(mode_text, 0666, mask, 0, &mode);
		if (status != 0) {
			fprintf(stderr, "mkfifo: invalid mode: '%s'\n", mode_text);
			return 1;
		}

		/* A FIFO takes permission bits only, no set-ID or sticky bit. */
		if ((mode & ~(mode_t)0777) != 0) {
			fprintf(stderr, "mkfifo: mode must specify only file permission bits\n");
			return 1;
		}
	}

	/* Makes each FIFO; a failure is remembered and the rest go on. */
	failed = 0;
	for (index = optind; index < argc; index++) {
		status = mkfifo(argv[index], mode);
		if (status != 0) {
			fprintf(stderr, "mkfifo: %s: %s\n", argv[index], strerror(errno));
			failed = 1;
			continue;
		}

		/* -m sets the mode exactly, whatever the mask. */
		if (mode_text != NULL) {
			status = chmod(argv[index], mode);
			if (status != 0) {
				fprintf(stderr, "mkfifo: %s: %s\n", argv[index], strerror(errno));
				failed = 1;
			}
		}
	}

	/* Reports whether any FIFO could not be made. */
	if (failed)
		return 1;

	/* Succeeded: every FIFO exists. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: mkfifo [-m mode] file...\n");
	exit(1);
}
