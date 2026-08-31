/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD tr userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int set(const char *s, unsigned char *out, size_t *count);

/*
 * Runs the tr command.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned char c;
	unsigned char buf[4096], out[4096];
	ssize_t n;
	size_t used;
	int del, squeeze, ai;
	unsigned char a[256], b[256], map[256], mark[256], last;
	size_t na, nb, i;
	int have;

	del = 0;
	squeeze = 0;
	ai = 1;
	memset(mark, 0, sizeof(mark));

	/* Process each remaining command-line operand. */
	last = 0;
	nb = 0;
	have = 0;
	while (ai < argc && argv[ai][0] == '-') {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[ai], "-d"))
			del = 1;
		else if (!strcmp(argv[ai], "-s"))
			squeeze = 1;
		else
			break;
		++ai;
	}

	/* Validates the command-line arguments. */
	if (ai >= argc || (!del && ai + 1 >= argc) ||
	    set(argv[ai], a, &na) != 0 ||
	    (!del && set(argv[ai + 1], b, &nb) != 0)) {
		fprintf(stderr, "usage: tr [-d] [-s] string1 [string2]\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Process each element required by the operation. */
	for (i = 0; i < 256; ++i)

	/* Process each element required by the operation. */
		map[i] = (unsigned char)i;
	for (i = 0; i < na; ++i) {
		mark[a[i]] = 1;

		/* Handles the del condition. */
		if (!del)
			map[a[i]] = b[i < nb ? i : nb - 1];
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		n = read(0, buf, sizeof(buf));
		used = 0;

		/* Checks the current item count. */
		if (n < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;
			command_error("tr", NULL);

			/* Reports operation failure. */
			return 1;
		}

		/* Checks the current item count. */
		if (!n)
			break;

		/* Process each remaining element. */
		for (i = 0; i < (size_t)n; ++i) {
			c = buf[i];

			/* Handles the del condition. */
			if (del && mark[c])
				continue;
			c = map[c];

			/* Validates the command-line arguments. */
			if (squeeze && have && c == last &&
			    (mark[c] || (!del && strchr(argv[ai + 1], c))))
				continue;
			out[used++] = c;
			last = c;
			have = 1;
		}

		/* Handles a failed command write all operation. */
		if (command_write_all(1, out, used) != 0) {
			command_error("tr", NULL);

			/* Reports operation failure. */
			return 1;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the set operation. */
static int
set(
	const char *s,
	unsigned char *out,
	size_t *count)
{
	unsigned char a, z;
	size_t n, i;

	/* Process each element required by the operation. */
	n = 0;
	for (i = 0; s[i]; ++i) {
		a = (unsigned char)s[i];
		z = a;

		/* Checks the current string state. */
		if (s[i + 1] == '-' && s[i + 2]) {
			z = (unsigned char)s[i + 2];
			i += 2;

			/* Handles the z condition. */
			if (z < a)
				return -1;
		}

		/* Continue until the operation reaches a terminal state. */
		for (;;) {
			/* Checks the current item count. */
			if (n == 256)
				return -1;
			out[n++] = a;

			/* Handles the a condition. */
			if (a == z)
				break;
			++a;
		}
	}
	*count = n;
	/* Reports successful completion. */
	return 0;
}
