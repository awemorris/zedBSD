/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD patch userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runs the patch command.
 */
int
main(
	int argc,
	char **argv)
{
	char *e, *q;
	FILE *p, *out;
	char *l;
	char target[512], temp[520];
	size_t cap;
	long n;
	int hunk;

	p = stdin;
	out = NULL;
	l = NULL;
	memset(target, 0, sizeof(target));
	cap = 0;
	hunk = 0;

	/* Validates the command-line arguments. */
	if (argc > 2) {
		fprintf(stderr, "usage: patch [patch-file]\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Validates the command-line arguments. */
	if (argc == 2 && (p = fopen(argv[1], "r")) == NULL) {
		command_error("patch", argv[1]);

		/* Reports operation failure. */
		return 1;
	}
	while ((n = command_read_line(p, &l, &cap)) > 0) {
		/* Selects the matching prefix. */
		if (!strncmp(l, "+++ ", 4)) {
			/* Continue while the operation condition remains true. */
						e = l + 4;
			q = e;
			while (*q && *q != '\t' && *q != '\n' && *q != ' ')
				q++;

			/* Handles the q condition. */
			if ((size_t)(q - e) >= sizeof(target)) {
				fprintf(stderr, "patch: path too long\n");

				/* Reports operation failure. */
				return 1;
			}
			memcpy(target, e, (size_t)(q - e));
			target[q - e] = 0;

			/* Selects the matching prefix. */
			if (!strncmp(target, "b/", 2))
				memmove(target, target + 2,
					strlen(target + 2) + 1);

			/* Handles a failed strstr operation. */
			if (target[0] == '/' || strstr(target, "../")) {
				fprintf(stderr, "patch: unsafe path\n");

				/* Reports operation failure. */
				return 1;
			}
			snprintf(temp, sizeof(temp), "%s.patch.tmp", target);
			out = fopen(temp, "w");

			/* Handles the out condition. */
			if (!out) {
				command_error("patch", temp);

				/* Reports operation failure. */
				return 1;
			}
		} else if (!strncmp(l, "@@ ", 3))
			hunk = 1;
		else if (hunk && out && (l[0] == '+' || l[0] == ' ')) {
			/* Handles a failed fwrite operation. */
			if (fwrite(l + 1, 1, (size_t)n - 1, out) !=
			    (size_t)n - 1) {
				command_error("patch", temp);

				/* Reports operation failure. */
				return 1;
			}
		} else if (hunk && out && l[0] == '-') {
		}
	}

	/* Handles the out condition. */
	if (!out) {
		fprintf(stderr, "patch: no unified patch found\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the fclose condition. */
	if (fclose(out)) {
		command_error("patch", temp);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the rename condition. */
	if (rename(temp, target)) {
		command_error("patch", target);

		/* Reports operation failure. */
		return 1;
	}
	free(l);

	/* Checks the current pointer. */
	if (p != stdin)
		fclose(p);

	/* Reports successful completion. */
	return 0;
}
