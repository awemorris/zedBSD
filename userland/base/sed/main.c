/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD sed userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int subst(char *line, const char *old, const char *rep, int global);

/*
 * Runs the sed command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	FILE *f;
	char sep;
	const char *a, *b, *e;
	char work[4096];
	int changed;
	char *l;
	size_t cap;
	long n;
	int noauto, i;
	const char *script;
	char old[256], rep[256];
	int global, print, del;

	noauto = 0;
	i = 1;
	global = 0;
	print = 0;
	del = 0;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-n")) {
		noauto = 1;
		i++;
	}

	/* Validates the command-line arguments. */
	if (i >= argc) {
		fprintf(stderr, "usage: sed [-n] script [file ...]\n");

		/* Reports operation failure. */
		return 2;
	}
	script = argv[i++];

	/* Handles the script condition. */
	if (script[0] == 's' && script[1]) {
		sep = script[1];
		a = script + 2;
		b = strchr(a, sep);

		/* Handles a failed strchr operation. */
		if (!b || (e = strchr(b + 1, sep)) == NULL ||
		    (size_t)(b - a) >= sizeof(old) ||
		    (size_t)(e - b - 1) >= sizeof(rep)) {
			fprintf(stderr, "sed: invalid substitute\n");

			/* Reports operation failure. */
			return 2;
		}
		memcpy(old, a, (size_t)(b - a));
		old[b - a] = 0;
		memcpy(rep, b + 1, (size_t)(e - b - 1));
		rep[e - b - 1] = 0;
		global = strchr(e + 1, 'g') != NULL;
		print = strchr(e + 1, 'p') != NULL;
	} else if (!strcmp(script, "p"))
		print = 1;
	else if (!strcmp(script, "d")) {
		del = 1;
	} else {
		fprintf(stderr, "sed: unsupported script\n");

		/* Reports operation failure. */
		return 2;
	}
	do {
				f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");
				l = NULL;
				cap = 0;

		/* Checks the current file state. */
		if (!f) {
			command_error("sed", argv[i]);

			/* Reports operation failure. */
			return 1;
		}
		while ((n = command_read_line(f, &l, &cap)) > 0) {
			changed = 0;

			/* Handles the script condition. */
			if (script[0] == 's') {
				/* Checks the current item count. */
				if ((size_t)n >= sizeof(work)) {
					fprintf(stderr, "sed: line too long\n");

					/* Reports operation failure. */
					return 1;
				}
				memcpy(work, l, (size_t)n + 1);
				changed = subst(work, old, rep, global);

				/* Handles the noauto condition. */
				if (!noauto)
					fwrite(work, 1, strlen(work), stdout);

				/* Handles the print condition. */
				if (print && changed)
					fwrite(work, 1, strlen(work), stdout);
			} else if (!del && (!noauto || print))
				fwrite(l, 1, (size_t)n, stdout);
		}
		free(l);

		/* Checks the current file state. */
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);

	/* Obtains the ferror result. */
	function_result = ferror(stdout);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the subst operation. */
static int
subst(
	char *line,
	const char *old,
	const char *rep,
	int global)
{
	char *end;
	size_t tail;
	char *p;
	int changed;
	size_t ol, rl;

	p = strstr(line, old);
	changed = 0;
	ol = strlen(old);
	rl = strlen(rep);

	/* Handles the ol condition. */
	if (!ol)
		return 0;

	/* Continue while the operation condition remains true. */
	while (p) {
		tail = strlen(p + ol);

		/* Handles the rl condition. */
		if (rl > ol) {
			end = line + strlen(line);

			/* Checks the current endpoint. */
			if ((size_t)(end - line) + rl - ol >= 4095)
				break;
		}
		memmove(p + rl, p + ol, tail + 1);
		memcpy(p, rep, rl);
		changed = 1;

		/* Handles the global condition. */
		if (!global)
			break;
		p = strstr(p + rl, old);
	}

	/* Returns the computed result. */
	return changed;
}
