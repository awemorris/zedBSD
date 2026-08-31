/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD du userland command.
 */

#include "userland/base/common/command.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned long long walk(const char *p, int all, int *bad);

/*
 * Runs the du command.
 */
int
main(
	int argc,
	char **argv)
{
	int all, i, bad;

	all = 0;
	i = 1;
	bad = 0;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-a")) {
		all = 1;
		i++;
	}

	/* Validates the command-line arguments. */
	if (i == argc) {
		argv[--i] = (char *)".";
		argc = i + 1;
	}

	/* Process each remaining command-line operand. */
	for (; i < argc; i++)
		printf("%llu\t%s\n", walk(argv[i], all, &bad), argv[i]);

	/* Returns the computed result. */
	return bad;
}

/* Supports the walk operation. */
static unsigned long long
walk(
	const char *p,
	int all,
	int *bad)
{
	unsigned long long n;
	char *q;
	size_t a, b;
	struct stat st;
	DIR *d;
	struct dirent *e;
	unsigned long long total;

	/* Handles the lstat condition. */
	if (lstat(p, &st)) {
		command_error("du", p);
		*bad = 1;
		/* Reports successful completion. */
		return 0;
	}
	total = (unsigned long long)(st.st_blocks < 0 ? 0 : st.st_blocks);

	/* Handles a failed S ISDIR operation. */
	if (!S_ISDIR(st.st_mode))
		return total;
	d = opendir(p);

	/* Checks the current descriptor. */
	if (!d) {
		command_error("du", p);
		*bad = 1;
		/* Returns the computed result. */
		return total;
	}
	while ((e = readdir(d))) {
		/* Selects the matching value. */
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		a = strlen(p);
		b = strlen(e->d_name);
		q = malloc(a + b + 2);

		/* Handles the q condition. */
		if (!q) {
			*bad = 1;
			break;
		}
		memcpy(q, p, a);

		/* Handles the a condition. */
		if (a && p[a - 1] != '/')
			q[a++] = '/';
		memcpy(q + a, e->d_name, b + 1);

		n = walk(q, all, bad);
		total += n;

		/* Handles the all condition. */
		if (all)
			printf("%llu\t%s\n", n, q);
		free(q);
	}
	closedir(d);

	/* Returns the computed result. */
	return total;
}
