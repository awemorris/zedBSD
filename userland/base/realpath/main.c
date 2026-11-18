/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD realpath userland command.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYMLINK_LIMIT 40

static char *resolve(const char *operand, int allow_missing_final);
static char *absolute_pending(const char *operand);
static char *copy_string(const char *text);
static void pop_component(char *path);
static int append_component(char **path, const char *component, size_t length);

/*
 * Runs the realpath command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int allow_missing;
	int index;
	char *path;

	allow_missing = 0;
	index = 1;

	/* Process each remaining command-line operand. */
	while (index < argc && argv[index][0] == '-' &&
	       argv[index][1] != '\0') {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[index], "--")) {
			index++;
			break;
		}

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[index], "-E"))
			allow_missing = 1;
		else if (!strcmp(argv[index], "-e"))
			allow_missing = 0;
		else
			goto usage;
		index++;
	}

	/* Validates the command-line arguments. */
	if (argc - index != 1)
		goto usage;
	path = resolve(argv[index], allow_missing);

	/* Handles the path availability. */
	if (path == NULL) {
		command_error("realpath", argv[index]);

		/* Reports operation failure. */
		return 1;
	}
	puts(path);
	free(path);

	/* Computes the function result. */
	function_result = ferror(stdout) ? 1 : 0;

	/* Returns the computed result. */
	return function_result;

usage:
	fprintf(stderr, "usage: realpath [-E|-e] file\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the resolve operation. */
static char *
resolve(
	const char *operand,
	int allow_missing_final)
{
	char target[PATH_MAX + 1U];
	ssize_t target_length;
	char *next;
	size_t rest;
	char *cursor;
	char *component;
	size_t length;
	struct stat status;
	int had_separator;
	char *pending;
	char *resolved;
	int links;

	links = 0;

	/* Handles the operand condition. */
	if (operand[0] == '\0') {
		errno = ENOENT;

		/* Reports that no result is available. */
		return NULL;
	}
	pending = absolute_pending(operand);

	/* Handles the pending availability. */
	if (pending == NULL)
		return NULL;
	resolved = copy_string("/");

	/* Handles the resolved availability. */
	if (resolved == NULL)
		goto failed;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		cursor = pending;

		/* Continue while the operation condition remains true. */
		while (*cursor == '/')
			cursor++;

		/* Checks the current cursor position. */
		if (*cursor == '\0')
			break;

		/* Continue while the operation condition remains true. */
		component = cursor;
		while (*cursor != '\0' && *cursor != '/')
			cursor++;

		/* Continue while the operation condition remains true. */
		length = (size_t)(cursor - component);
		had_separator = *cursor == '/';
		while (*cursor == '/')
			cursor++;

		/* Checks the current data length. */
		if (length == 1U && component[0] == '.') {
			memmove(pending, cursor, strlen(cursor) + 1U);
			continue;
		}

		/* Checks the current data length. */
		if (length == 2U && component[0] == '.' &&
		    component[1] == '.') {
			pop_component(resolved);
			memmove(pending, cursor, strlen(cursor) + 1U);
			continue;
		}

		/* Handles a failed append component operation. */
		if (!append_component(&resolved, component, length))
			goto failed_resolved;

		/* Handles a failed lstat operation. */
		if (lstat(resolved, &status) != 0) {
			/* Handles the reported system error. */
			if (allow_missing_final && errno == ENOENT &&
			    *cursor == '\0')
				break;
			goto failed_resolved;
		}

		/* Handles a failed S ISDIR operation. */
		if (had_separator && *cursor == '\0' &&
		    !S_ISDIR(status.st_mode)) {
			errno = ENOTDIR;
			goto failed_resolved;
		}

		/* Checks the operation status. */
		if (S_ISLNK(status.st_mode)) {
			rest = strlen(cursor);

			/* Handles the links condition. */
			if (++links > SYMLINK_LIMIT) {
				errno = ELOOP;
				goto failed_resolved;
			}
			target_length =
			    readlink(resolved, target, sizeof(target) - 1U);

			/* Handles the target length condition. */
			if (target_length < 0)
				goto failed_resolved;
			target[target_length] = '\0';
			pop_component(resolved);
			next = malloc((size_t)target_length + (rest != 0) +
				      rest + 1U);

			/* Handles the next availability. */
			if (next == NULL)
				goto failed_resolved;
			memcpy(next, target, (size_t)target_length);

			/* Handles the rest condition. */
			if (rest != 0)
				next[target_length++] = '/';
			memcpy(next + target_length, cursor, rest + 1U);
			free(pending);
			pending = next;

			/* Handles the target condition. */
			if (target[0] == '/') {
				free(resolved);
				resolved = copy_string("/");

				/* Handles the resolved availability. */
				if (resolved == NULL)
					goto failed;
			}
			continue;
		}

		/* Handles a failed S ISDIR operation. */
		if (*cursor != '\0' && !S_ISDIR(status.st_mode)) {
			errno = ENOTDIR;
			goto failed_resolved;
		}
		memmove(pending, cursor, strlen(cursor) + 1U);
	}
	free(pending);

	/* Returns the computed result. */
	return resolved;

failed_resolved:
	free(resolved);
failed:
	free(pending);

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the absolute pending operation. */
static char *
absolute_pending(
	const char *operand)
{
	char *function_result;
	char current[PATH_MAX + 1U];
	size_t current_length;
	size_t operand_length;
	char *pending;

	/* Handles the operand condition. */
	if (operand[0] == '/') {
		/* Obtains the copy string result. */
		function_result = copy_string(operand);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed getcwd operation. */
	if (getcwd(current, sizeof(current)) == NULL)
		return NULL;
	current_length = strlen(current);
	operand_length = strlen(operand);

	/* Handles the current length condition. */
	if (current_length > SIZE_MAX - operand_length - 2U) {
		errno = ENAMETOOLONG;

		/* Reports that no result is available. */
		return NULL;
	}
	pending = malloc(current_length + operand_length + 2U);

	/* Handles the pending availability. */
	if (pending != NULL) {
		(void)snprintf(pending, current_length + operand_length + 2U,
			       "%s/%s", current, operand);
	}

	/* Returns the computed result. */
	return pending;
}

/* Supports the copy string operation. */
static char *
copy_string(
	const char *text)
{
	size_t length;
	char *copy;

	length = strlen(text) + 1U;
	copy = malloc(length);

	/* Handles the copy availability. */
	if (copy != NULL)
		memcpy(copy, text, length);

	/* Returns the computed result. */
	return copy;
}

/* Supports the pop component operation. */
static void
pop_component(
	char *path)
{
	char *slash;

	/* Selects the matching value. */
	if (!strcmp(path, "/"))
		return;
	slash = strrchr(path, '/');

	/* Handles the slash condition. */
	if (slash == path)
		path[1] = '\0';
	else if (slash != NULL)
		*slash = '\0';
}

/* Supports the append component operation. */
static int
append_component(
	char **path,
	const char *component,
	size_t length)
{
	size_t old_length;
	int slash;
	char *larger;

	old_length = strlen(*path);
	slash = old_length > 1U;
	larger = realloc(*path, old_length + (size_t)slash + length + 1U);

	/* Handles the larger availability. */
	if (larger == NULL)
		return 0;
	*path = larger;
	/* Handles the slash condition. */
	if (slash)
		larger[old_length++] = '/';
	memcpy(larger + old_length, component, length);
	larger[old_length + length] = '\0';

	/* Reports operation failure. */
	return 1;
}
