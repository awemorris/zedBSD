/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/* Moves names, optionally refusing replacement under the namespace lock. */
#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *leaf(const char *path);

int
main(int argc, char **argv)
{
	char target[1024];
	const char *to;
	struct stat st;
	int i, first, failed, isdir, literal, noreplace, conflict_fails;
	int error;

	failed = 0;
	literal = 0;
	noreplace = 0;
	conflict_fails = 0;
	first = 1;

	/* Options determine both the destination policy and the exit contract.
	 */
	while (first < argc && argv[first][0] == '-') {
		to = argv[first++];
		if (!strcmp(to, "--"))
			break;
		if (!strcmp(to, "-T") || !strcmp(to, "--no-target-directory")) {
			literal = 1;
		} else if (!strcmp(to, "-n") || !strcmp(to, "--no-clobber") ||
			   !strcmp(to, "--update=none")) {
			noreplace = 1;
			conflict_fails = 0;
		} else if (!strcmp(to, "--update=none-fail")) {
			noreplace = 1;
			conflict_fails = 1;
		} else if (!strcmp(to, "-f") || !strcmp(to, "--force")) {
			noreplace = 0;
			conflict_fails = 0;
		} else {
			fprintf(stderr, "mv: unsupported option: %s\n", to);
			return 1;
		}
	}
	if (argc - first < 2) {
		fprintf(stderr, "usage: mv [-T] [-n|--update=none-fail] "
				"source... destination\n");
		return 1;
	}

	/* Exact-path mode never follows a destination into a directory. */
	isdir = 0;
	if (!literal)
		isdir = stat(argv[argc - 1], &st) == 0 && S_ISDIR(st.st_mode);
	if (argc - first > 2 && !isdir) {
		fprintf(stderr, "mv: destination is not a directory\n");
		return 1;
	}

	for (i = first; i < argc - 1; i++) {
		to = argv[argc - 1];
		if (isdir) {
			if (snprintf(target, sizeof(target), "%s/%s", to,
				     leaf(argv[i])) >= (int)sizeof(target)) {
				errno = ENAMETOOLONG;
				command_error("mv", argv[i]);
				failed = 1;
				continue;
			}
			to = target;
		}

		/* No-clobber is one kernel operation, including case aliases.
		 */
		if (noreplace)
			error = renameat2(AT_FDCWD, argv[i], AT_FDCWD, to,
					  RENAME_NOREPLACE);
		else
			error = rename(argv[i], to);
		if (error != 0) {
			if (noreplace && errno == EEXIST && !conflict_fails)
				continue;
			command_error("mv", to);
			failed = 1;
		}
	}
	return failed;
}

static const char *
leaf(const char *path)
{
	const char *p;

	p = strrchr(path, '/');
	return p != NULL ? p + 1 : path;
}
