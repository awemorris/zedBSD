/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD crontab userland command.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CRON_SPOOL "/var/spool/cron"

/*
 * Runs the crontab command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	char path[256], temporary[288], buffer[1024];
	FILE *input, *output;
	int descriptor;

	snprintf(path, sizeof(path), "%s/%lu", CRON_SPOOL,
		 (unsigned long)getuid());

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "-l") == 0) {
		input = fopen(path, "r");

		/* Handles the input availability. */
		if (input == NULL)
			return 1;

		/* Process input until it is exhausted. */
		while (fgets(buffer, sizeof(buffer), input) != NULL)
			fputs(buffer, stdout);

		/* Computes the function result. */
		function_result = ferror(input) || fclose(input) != 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "-r") == 0) {
		/* Computes the function result. */
		function_result = unlink(path) != 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Validates the command-line arguments. */
	if (argc > 2) {
		fprintf(stderr, "usage: crontab [-l|-r|file]\n");

		/* Reports operation failure. */
		return 2;
	}
	input = argc == 2 ? fopen(argv[1], "r") : stdin;

	/* Handles the input availability. */
	if (input == NULL) {
		fprintf(stderr, "crontab: %s: %s\n", argv[1], strerror(errno));

		/* Reports operation failure. */
		return 1;
	}
	(void)mkdir("/var/spool", 0755);
	(void)mkdir(CRON_SPOOL, 01777);
	snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
		 (long)getpid());
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);

	/* Handles a failed fdopen operation. */
	if (descriptor < 0 || (output = fdopen(descriptor, "w")) == NULL)
		return 1;

	/* Process input until it is exhausted. */
	while (fgets(buffer, sizeof(buffer), input) != NULL) {
		/* Handles a failed strchr operation. */
		if (strchr(buffer, '\n') == NULL && !feof(input)) {
			fprintf(stderr, "crontab: line too long\n");
			unlink(temporary);

			/* Reports operation failure. */
			return 1;
		}

		/* Handles the end-of-file condition. */
		if (fputs(buffer, output) == EOF)
			break;
	}

	/* Handles an operation failure. */
	if (ferror(input) || ferror(output) || fflush(output) != 0 ||
	    fsync(descriptor) != 0 || fclose(output) != 0 ||
	    rename(temporary, path) != 0) {
		unlink(temporary);

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the current input. */
	if (input != stdin)
		fclose(input);

	/* Reports successful completion. */
	return 0;
}
