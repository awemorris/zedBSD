/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD split userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Runs the split command.
 */
int
main(
	int argc,
	char **argv)
{
	size_t p;
	unsigned char c;
	ssize_t n;
	unsigned long long limit, used, index;
	int bytes, i, in, out;
	const char *prefix;
	char name[512];

	limit = 1000;
	used = 0;
	index = 0;
	bytes = 0;
	i = 1;
	out = -1;
	prefix = "x";

	/* Handles the selected command-line operation. */
	if (i < argc && (!strcmp(argv[i], "-l") || !strcmp(argv[i], "-b"))) {
		bytes = argv[i][1] == 'b';

		/* Validates the command-line arguments. */
		if (++i >= argc || command_parse_ull(argv[i++], &limit) ||
		    !limit)

			/* Reports operation failure. */
			return 2;
	}
	in = i < argc && !strcmp(argv[i], "-") ? 0
	     : i < argc			       ? open(argv[i++], O_RDONLY)
					       : 0;

	/* Handles the in condition. */
	if (in < 0) {
		command_error("split", argv[i - 1]);

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (i < argc)
		prefix = argv[i++];

	/* Validates the command-line arguments. */
	if (i != argc) {
		fprintf(stderr, "usage: split [-l n|-b n] [file [prefix]]\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		n = read(in, &c, 1);

		/* Checks the current item count. */
		if (n < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;
			command_error("split", NULL);

			/* Reports operation failure. */
			return 1;
		}

		/* Checks the current item count. */
		if (!n)
			break;

		/* Handles the out condition. */
		if (out < 0) {
			p = strlen(prefix);

			/* Checks the current pointer. */
			if (p + 3 > sizeof(name) || index >= 26ULL * 26ULL) {
				fprintf(stderr, "split: too many files\n");

				/* Reports operation failure. */
				return 1;
			}
			memcpy(name, prefix, p);
			name[p] = (char)('a' + index / 26);
			name[p + 1] = (char)('a' + index % 26);
			name[p + 2] = 0;
			out = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);

			/* Handles the out condition. */
			if (out < 0) {
				command_error("split", name);

				/* Reports operation failure. */
				return 1;
			}
			index++;
			used = 0;
		}

		/* Handles the command write all condition. */
		if (command_write_all(out, &c, 1)) {
			command_error("split", name);

			/* Reports operation failure. */
			return 1;
		}
		used += bytes ? 1 : (c == '\n');

		/* Checks the current capacity usage. */
		if (used >= limit) {
			/* Handles the close condition. */
			if (close(out)) {
				command_error("split", name);

				/* Reports operation failure. */
				return 1;
			}
			out = -1;
		}
	}

	/* Handles a failed close operation. */
	if (out >= 0 && close(out)) {
		command_error("split", name);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the in condition. */
	if (in)
		close(in);

	/* Reports successful completion. */
	return 0;
}
