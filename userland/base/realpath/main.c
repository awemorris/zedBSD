/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
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

#define SYMLINK_LIMIT	40

static char *
copy_string(const char *text)
{
	size_t length = strlen(text) + 1U;
	char *copy = malloc(length);

	if (copy != NULL)
		memcpy(copy, text, length);
	return copy;
}

static int
append_component(char **path, const char *component, size_t length)
{
	size_t old_length = strlen(*path);
	int slash = old_length > 1U;
	char *larger = realloc(*path, old_length + (size_t)slash + length + 1U);

	if (larger == NULL)
		return 0;
	*path = larger;
	if (slash)
		larger[old_length++] = '/';
	memcpy(larger + old_length, component, length);
	larger[old_length + length] = '\0';
	return 1;
}

static void
pop_component(char *path)
{
	char *slash;

	if (!strcmp(path, "/"))
		return;
	slash = strrchr(path, '/');
	if (slash == path)
		path[1] = '\0';
	else if (slash != NULL)
		*slash = '\0';
}

static char *
absolute_pending(const char *operand)
{
	char current[PATH_MAX + 1U];
	size_t current_length;
	size_t operand_length;
	char *pending;

	if (operand[0] == '/')
		return copy_string(operand);
	if (getcwd(current, sizeof(current)) == NULL)
		return NULL;
	current_length = strlen(current);
	operand_length = strlen(operand);
	if (current_length > SIZE_MAX - operand_length - 2U) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	pending = malloc(current_length + operand_length + 2U);
	if (pending != NULL)
		(void)snprintf(pending, current_length + operand_length + 2U,
		    "%s/%s", current, operand);
	return pending;
}

static char *
resolve(const char *operand, int allow_missing_final)
{
	char *pending;
	char *resolved;
	int links = 0;

	if (operand[0] == '\0') {
		errno = ENOENT;
		return NULL;
	}
	pending = absolute_pending(operand);
	if (pending == NULL)
		return NULL;
	resolved = copy_string("/");
	if (resolved == NULL)
		goto failed;
	for (;;) {
		char *cursor = pending;
		char *component;
		size_t length;
		struct stat status;
		int had_separator;

		while (*cursor == '/')
			cursor++;
		if (*cursor == '\0')
			break;
		component = cursor;
		while (*cursor != '\0' && *cursor != '/')
			cursor++;
		length = (size_t)(cursor - component);
		had_separator = *cursor == '/';
		while (*cursor == '/')
			cursor++;
		if (length == 1U && component[0] == '.') {
			memmove(pending, cursor, strlen(cursor) + 1U);
			continue;
		}
		if (length == 2U && component[0] == '.' && component[1] == '.') {
			pop_component(resolved);
			memmove(pending, cursor, strlen(cursor) + 1U);
			continue;
		}
		if (!append_component(&resolved, component, length))
			goto failed_resolved;
		if (lstat(resolved, &status) != 0) {
			if (allow_missing_final && errno == ENOENT && *cursor == '\0')
				break;
			goto failed_resolved;
		}
		if (had_separator && *cursor == '\0' && !S_ISDIR(status.st_mode)) {
			errno = ENOTDIR;
			goto failed_resolved;
		}
		if (S_ISLNK(status.st_mode)) {
			char target[PATH_MAX + 1U];
			ssize_t target_length;
			char *next;
			size_t rest = strlen(cursor);

			if (++links > SYMLINK_LIMIT) {
				errno = ELOOP;
				goto failed_resolved;
			}
			target_length = readlink(resolved, target,
			    sizeof(target) - 1U);
			if (target_length < 0)
				goto failed_resolved;
			target[target_length] = '\0';
			pop_component(resolved);
			next = malloc((size_t)target_length + (rest != 0) + rest + 1U);
			if (next == NULL)
				goto failed_resolved;
			memcpy(next, target, (size_t)target_length);
			if (rest != 0)
				next[target_length++] = '/';
			memcpy(next + target_length, cursor, rest + 1U);
			free(pending);
			pending = next;
			if (target[0] == '/') {
				free(resolved);
				resolved = copy_string("/");
				if (resolved == NULL)
					goto failed;
			}
			continue;
		}
		if (*cursor != '\0' && !S_ISDIR(status.st_mode)) {
			errno = ENOTDIR;
			goto failed_resolved;
		}
		memmove(pending, cursor, strlen(cursor) + 1U);
	}
	free(pending);
	return resolved;

failed_resolved:
	free(resolved);
failed:
	free(pending);
	return NULL;
}

int
main(int argc, char **argv)
{
	int allow_missing = 0;
	int index = 1;
	char *path;

	while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
		if (!strcmp(argv[index], "--")) {
			index++;
			break;
		}
		if (!strcmp(argv[index], "-E"))
			allow_missing = 1;
		else if (!strcmp(argv[index], "-e"))
			allow_missing = 0;
		else
			goto usage;
		index++;
	}
	if (argc - index != 1)
		goto usage;
	path = resolve(argv[index], allow_missing);
	if (path == NULL) {
		command_error("realpath", argv[index]);
		return 1;
	}
	puts(path);
	free(path);
	return ferror(stdout) ? 1 : 0;

usage:
	fprintf(stderr, "usage: realpath [-E|-e] file\n");
	return 2;
}
