/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYMLINK_LIMIT 40

static char *copy_string(const char *text)
{
	size_t length = strlen(text) + 1U;
	char *copy = malloc(length);
	if (copy != NULL) memcpy(copy, text, length);
	return copy;
}

static int append_component(char **path, const char *component, size_t length)
{
	size_t old = strlen(*path);
	int slash = old > 1U;
	char *larger = realloc(*path, old + (size_t)slash + length + 1U);
	if (larger == NULL) return 0;
	*path = larger;
	if (slash) larger[old++] = '/';
	memcpy(larger + old, component, length);
	larger[old + length] = '\0';
	return 1;
}

static void pop_component(char *path)
{
	char *slash;
	if (!strcmp(path, "/")) return;
	slash = strrchr(path, '/');
	if (slash == path) path[1] = '\0';
	else if (slash != NULL) *slash = '\0';
}

static char *resolve(const char *operand)
{
	char cwd[PATH_MAX];
	char *pending, *resolved;
	int links = 0;
	if (operand[0] == '/') pending = copy_string(operand);
	else {
		size_t cwd_length, operand_length = strlen(operand);
		if (getcwd(cwd, sizeof(cwd)) == NULL) return NULL;
		cwd_length = strlen(cwd);
		pending = malloc(cwd_length + 1U + operand_length + 1U);
		if (pending != NULL)
			snprintf(pending, cwd_length + 1U + operand_length + 1U,
			    "%s/%s", cwd, operand);
	}
	if (pending == NULL) return NULL;
	resolved = copy_string("/");
	if (resolved == NULL) { free(pending); return NULL; }
	for (;;) {
		char *cursor = pending;
		char *component;
		size_t length;
		struct stat status;
		while (*cursor == '/') cursor++;
		if (*cursor == '\0') break;
		component = cursor;
		while (*cursor != '\0' && *cursor != '/') cursor++;
		length = (size_t)(cursor - component);
		while (*cursor == '/') cursor++;
		if (length == 1U && component[0] == '.') {
			memmove(pending, cursor, strlen(cursor) + 1U);
			continue;
		}
		if (length == 2U && component[0] == '.' && component[1] == '.') {
			pop_component(resolved);
			memmove(pending, cursor, strlen(cursor) + 1U);
			continue;
		}
		if (!append_component(&resolved, component, length)) goto failed;
		if (lstat(resolved, &status) != 0) goto failed;
		if (S_ISLNK(status.st_mode)) {
			char target[PATH_MAX];
			ssize_t target_length;
			char *next;
			size_t rest = strlen(cursor);
			if (++links > SYMLINK_LIMIT) { errno = ELOOP; goto failed; }
			target_length = readlink(resolved, target, sizeof(target) - 1U);
			if (target_length < 0) goto failed;
			target[target_length] = '\0';
			pop_component(resolved);
			next = malloc((size_t)target_length + (rest != 0) + rest + 1U);
			if (next == NULL) goto failed;
			memcpy(next, target, (size_t)target_length);
			if (rest != 0) next[target_length++] = '/';
			memcpy(next + target_length, cursor, rest + 1U);
			free(pending);
			pending = next;
			if (target[0] == '/') { free(resolved); resolved = copy_string("/"); }
			if (resolved == NULL) goto failed;
			continue;
		}
		memmove(pending, cursor, strlen(cursor) + 1U);
	}
	free(pending);
	return resolved;
failed:
	free(pending);
	free(resolved);
	return NULL;
}

int main(int argc, char **argv)
{
	int index, failed = 0;
	if (argc > 1 && !strcmp(argv[1], "--")) { argv++; argc--; }
	if (argc < 2) { fprintf(stderr, "usage: realpath path...\n"); return 1; }
	for (index = 1; index < argc; index++) {
		char *path = resolve(argv[index]);
		if (path == NULL) { command_error("realpath", argv[index]); failed = 1; }
		else { puts(path); free(path); }
	}
	return failed;
}
