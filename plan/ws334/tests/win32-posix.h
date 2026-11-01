/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a few of libbrowser's host tests (plan/ws074/tests/host-*.c) take from the C library and
 * the Windows C runtime does not have, for the Windows build of those tests
 * (plan/ws334/tests/host-tests-windows.sh includes this file with -include).
 *
 * setenv: _putenv_s.  The tests set XDG_DATA_HOME, which only the POSIX build reads.
 * realpath: the full path in the form a file: URL takes after "file://" (a slash, the drive, slashes).
 */

#ifndef WS334_WIN32_POSIX_H
#define WS334_WIN32_POSIX_H

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static inline int
setenv(
	const char *name,
	const char *value,
	int overwrite)
{
	if (overwrite == 0 && getenv(name) != NULL)
		return 0;
	return _putenv_s(name, value) == 0 ? 0 : -1;
}

static inline char *
realpath(
	const char *path,
	char *resolved)
{
	char full[PATH_MAX];
	size_t index;

	if (_fullpath(full, path, sizeof(full)) == NULL)
		return NULL;
	if (resolved == NULL)
		resolved = malloc(strlen(full) + 2U);
	if (resolved == NULL)
		return NULL;
	resolved[0] = '/';
	for (index = 0; full[index] != '\0'; index++)
		resolved[index + 1U] = full[index] == '\\' ? '/' : full[index];
	resolved[index + 1U] = '\0';
	return resolved;
}

#endif
