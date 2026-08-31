/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD chmod userland command.
 */

#include "userland/base/common/command.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHMOD_PATH_CAPACITY 1024U
#define CHMOD_DEPTH_LIMIT 64

static int apply_path(const char *path, const char *spec, int numeric, mode_t numeric_mode, mode_t mask, int recursive, int depth);
static int symbolic_mode(const char *spec, mode_t original, mode_t umask_value, int directory, mode_t *result);
static mode_t copied_bits(mode_t mode, char source, int who);
static mode_t class_mask(int who);
static int join_path(const char *a, const char *b, char *out, size_t cap);

/*
 * Runs the chmod command.
 */
int
main(
	int argc,
	char **argv)
{
	int recursive, index, failed, numeric;
	unsigned parsed;
	mode_t mask;
	const char *spec;

	recursive = 0;
	index = 1;
	failed = 0;
	parsed = 0;

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "-R")) {
		recursive = 1;
		index++;
	}

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "--"))
		index++;

	/* Validates the command-line arguments. */
	if (index + 1 >= argc) {
		fprintf(stderr, "usage: chmod [-R] mode file...\n");

		/* Reports operation failure. */
		return 1;
	}
	spec = argv[index++];
	numeric = command_parse_mode(spec, &parsed) == 0;
	mask = umask(0);
	(void)umask(mask);

	/* Process each remaining command-line operand. */
	for (; index < argc; index++) {
		/* Validates the command-line arguments. */
		if (!apply_path(argv[index], spec, numeric, (mode_t)parsed,
				mask, recursive, 0))
			failed = 1;
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the apply path operation. */
static int
apply_path(
	const char *path,
	const char *spec,
	int numeric,
	mode_t numeric_mode,
	mode_t mask,
	int recursive,
	int depth)
{
	char child[CHMOD_PATH_CAPACITY];
	DIR *d;
	struct dirent *entry;
	struct stat status;
	mode_t mode;
	int ok;

	ok = 1;

	/* Handles the depth condition. */
	if (depth > CHMOD_DEPTH_LIMIT) {
		errno = ELOOP;
		command_error("chmod", path);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the lstat condition. */
	if (lstat(path, &status)) {
		command_error("chmod", path);

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the operation status. */
	if (S_ISLNK(status.st_mode))
		return 1;

	/* Handles the numeric condition. */
	if (numeric)
		mode = numeric_mode;
	else if (!symbolic_mode(spec, status.st_mode, mask,
				S_ISDIR(status.st_mode), &mode)) {
		fprintf(stderr, "chmod: invalid mode: %s\n", spec);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the chmod condition. */
	if (chmod(path, mode)) {
		command_error("chmod", path);
		ok = 0;
	}

	/* Handles the recursive condition. */
	if (recursive && S_ISDIR(status.st_mode)) {
		d = opendir(path);

		/* Checks the current descriptor. */
		if (!d) {
			command_error("chmod", path);

			/* Reports successful completion. */
			return 0;
		}
		while ((entry = readdir(d)) != NULL) {
			/* Selects the matching value. */
			if (!strcmp(entry->d_name, ".") ||
			    !strcmp(entry->d_name, ".."))
				continue;

			/* Handles a failed join path operation. */
			if (!join_path(path, entry->d_name, child,
				       sizeof(child)) ||
			    !apply_path(child, spec, numeric, numeric_mode,
					mask, 1, depth + 1))
				ok = 0;
		}

		/* Handles the closedir condition. */
		if (closedir(d)) {
			command_error("chmod", path);
			ok = 0;
		}
	}

	/* Returns the computed result. */
	return ok;
}

/* Supports the symbolic mode operation. */
static int
symbolic_mode(
	const char *spec,
	mode_t original,
	mode_t umask_value,
	int directory,
	mode_t *result)
{
	int who, explicit_who;
	char operation;
	mode_t bits, affected;
	const char *p;
	mode_t mode;

	/* Continue while the operation condition remains true. */
	p = spec;
	mode = original;
	while (*p) {
		/* Continue while the operation condition remains true. */
		who = 0;
		explicit_who = 0;
		bits = 0;
		while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
			explicit_who = 1;

			/* Checks the current pointer. */
			if (*p == 'u')
				who |= 1;
			else if (*p == 'g')
				who |= 2;
			else if (*p == 'o')
				who |= 4;
			else
				who = 7;
			p++;
		}

		/* Checks the selected user entry. */
		if (!who)
			who = 7;
		operation = *p++;

		/* Validates the selected operation. */
		if (operation != '+' && operation != '-' && operation != '=')
			return 0;

		/* Continue while the operation condition remains true. */
		while (*p && *p != ',') {
			/* Dispatch the selected operation case. */
			switch (*p) {
			case 'r':
				/* Checks the selected user entry. */
				if (who & 1)
					bits |= S_IRUSR;

				/* Checks the selected user entry. */
				if (who & 2)
					bits |= S_IRGRP;

				/* Checks the selected user entry. */
				if (who & 4)
					bits |= S_IROTH;
				break;
			case 'w':
				/* Checks the selected user entry. */
				if (who & 1)
					bits |= S_IWUSR;

				/* Checks the selected user entry. */
				if (who & 2)
					bits |= S_IWGRP;

				/* Checks the selected user entry. */
				if (who & 4)
					bits |= S_IWOTH;
				break;
			case 'x':
				/* Checks the selected user entry. */
				if (who & 1)
					bits |= S_IXUSR;

				/* Checks the selected user entry. */
				if (who & 2)
					bits |= S_IXGRP;

				/* Checks the selected user entry. */
				if (who & 4)
					bits |= S_IXOTH;
				break;
			case 'X':
				/* Handles the directory condition. */
				if (directory ||
				    (original &
				     (S_IXUSR | S_IXGRP | S_IXOTH))) {
					/* Checks the selected user entry. */
					if (who & 1)
						bits |= S_IXUSR;

					/* Checks the selected user entry. */
					if (who & 2)
						bits |= S_IXGRP;

					/* Checks the selected user entry. */
					if (who & 4)
						bits |= S_IXOTH;
				}
				break;
			case 's':
				/* Checks the selected user entry. */
				if (who & 1)
					bits |= S_ISUID;

				/* Checks the selected user entry. */
				if (who & 2)
					bits |= S_ISGID;
				break;
			case 't':
				/* Checks the selected user entry. */
				if (who & 4)
					bits |= S_ISVTX;
				break;
			case 'u':
			case 'g':
			case 'o':
				bits |= copied_bits(mode, *p, who);
				break;
			default:
				/* Reports successful completion. */
				return 0;
			}
			p++;
		}
		affected = class_mask(who);

		/* Handles the explicit who condition. */
		if (!explicit_who)
			bits &= ~umask_value;

		/* Validates the selected operation. */
		if (operation == '+')
			mode |= bits;
		else if (operation == '-') {
			mode &= ~bits;
		} else {
			mode &= ~affected;
			mode |= bits;
		}

		/* Checks the current pointer. */
		if (*p == ',') {
			p++;

			/* Checks the current pointer. */
			if (!*p)
				return 0;
		}
	}
	*result = mode;
	/* Reports operation failure. */
	return 1;
}

/* Supports the copied bits operation. */
static mode_t
copied_bits(
	mode_t mode,
	char source,
	int who)
{
	mode_t triad = source == 'u'   ? (mode & S_IRWXU) >> 6
		       : source == 'g' ? (mode & S_IRWXG) >> 3
				       : mode & S_IRWXO;
	mode_t out;

	out = 0;

	/* Checks the selected user entry. */
	if (who & 1)
		out |= triad << 6;

	/* Checks the selected user entry. */
	if (who & 2)
		out |= triad << 3;

	/* Checks the selected user entry. */
	if (who & 4)
		out |= triad;

	/* Returns the computed result. */
	return out;
}

/* Supports the class mask operation. */
static mode_t
class_mask(
	int who)
{
	mode_t mask;

	mask = 0;

	/* Checks the selected user entry. */
	if (who & 1)
		mask |= S_IRWXU | S_ISUID;

	/* Checks the selected user entry. */
	if (who & 2)
		mask |= S_IRWXG | S_ISGID;

	/* Checks the selected user entry. */
	if (who & 4)
		mask |= S_IRWXO | S_ISVTX;

	/* Returns the computed result. */
	return mask;
}

/* Supports the join path operation. */
static int
join_path(
	const char *a,
	const char *b,
	char *out,
	size_t cap)
{
	size_t x = strlen(a), y = strlen(b);
	int slash = x && a[x - 1] != '/';

	/* Checks the current horizontal value. */
	if (x + (size_t)slash + y + 1U > cap) {
		errno = ENAMETOOLONG;

		/* Reports successful completion. */
		return 0;
	}
	memcpy(out, a, x);

	/* Handles the slash condition. */
	if (slash)
		out[x++] = '/';
	memcpy(out + x, b, y + 1U);

	/* Reports operation failure. */
	return 1;
}
