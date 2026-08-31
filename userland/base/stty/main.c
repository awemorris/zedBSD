/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD stty userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <termios.h>

/*
 * Runs the stty command.
 */
int
main(
	int argc,
	char **argv)
{
	int on;
	const char *n;
	struct termios t;
	int i;

	/* Handles the tcgetattr condition. */
	if (tcgetattr(0, &t)) {
		command_error("stty", NULL);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the selected command-line operation. */
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-a"))) {
		printf("speed %u baud; ispeed %u baud;\n",
		       (unsigned)cfgetospeed(&t), (unsigned)cfgetispeed(&t));
		printf(
		    "%sicanon %secho %sisig %siexten %sixon %sopost\n",
		    t.c_lflag & ICANON ? "" : "-", t.c_lflag & ECHO ? "" : "-",
		    t.c_lflag & ISIG ? "" : "-", t.c_lflag & IEXTEN ? "" : "-",
		    t.c_iflag & IXON ? "" : "-", t.c_oflag & OPOST ? "" : "-");

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining command-line operand. */
	for (i = 1; i < argc; ++i) {
		on = argv[i][0] != '-';
		n = on ? argv[i] : argv[i] + 1;

		/* Selects the matching value. */
		if (!strcmp(n, "echo")) {
			/* Handles the on condition. */
			if (on)
				t.c_lflag |= ECHO;
			else
				t.c_lflag &= ~ECHO;
		} else if (!strcmp(n, "icanon")) {
			/* Handles the on condition. */
			if (on)
				t.c_lflag |= ICANON;
			else
				t.c_lflag &= ~ICANON;
		} else if (!strcmp(n, "isig")) {
			/* Handles the on condition. */
			if (on)
				t.c_lflag |= ISIG;
			else
				t.c_lflag &= ~ISIG;
		} else if (!strcmp(n, "ixon")) {
			/* Handles the on condition. */
			if (on)
				t.c_iflag |= IXON;
			else
				t.c_iflag &= ~IXON;
		} else if (!strcmp(n, "opost")) {
			/* Handles the on condition. */
			if (on)
				t.c_oflag |= OPOST;
			else
				t.c_oflag &= ~OPOST;
		} else if (!strcmp(n, "raw")) {
			t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
			t.c_iflag = 0;
			t.c_oflag = 0;
			t.c_cc[VMIN] = 1;
			t.c_cc[VTIME] = 0;
		} else {
			fprintf(stderr, "stty: invalid argument: %s\n",
				argv[i]);

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Handles the tcsetattr condition. */
	if (tcsetattr(0, TCSADRAIN, &t)) {
		command_error("stty", NULL);

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}
