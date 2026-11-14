/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a few of libbrowser's host tests (the host-*.c files) take from the C library and
 * the Windows C runtime does not have, for the Windows build of those tests
 * (host-tests-windows.sh in this directory includes this file with -include).
 *
 * setenv: _putenv_s.  The tests set XDG_DATA_HOME, which only the POSIX build reads.
 * realpath: the full path with '/' for '\\' ("C:/dir/file"): an absolute path on Windows, which the
 * tests also put after "file://" (a drive letter where a file: URL's host would be is the path's, as
 * the URL standard parses it).
 */

#ifndef BROWSER_WIN32_TEST_POSIX_H
#define BROWSER_WIN32_TEST_POSIX_H

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static inline int
setenv(
	const char *name,
	const char *value,
	int overwrite)
{
	const char *existing;
	errno_t status;

	/* An existing variable stays unless the caller asks to replace it. */
	existing = getenv(name);
	if (overwrite == 0 && existing != NULL)
		return 0;
	status = _putenv_s(name, value);
	if (status != 0)
		return -1;
	return 0;
}

static inline char *
realpath(
	const char *path,
	char *resolved)
{
	char full[PATH_MAX];
	char *made;
	size_t index;

	/* The full path, with the system's own call. */
	made = _fullpath(full, path, sizeof(full));
	if (made == NULL)
		return NULL;
	if (resolved == NULL)
		resolved = malloc(strlen(full) + 1U);
	if (resolved == NULL)
		return NULL;
	for (index = 0; full[index] != '\0'; index++) {
		resolved[index] = full[index];
		if (full[index] == '\\')
			resolved[index] = '/';
	}

	/* End the text. */
	resolved[index] = '\0';
	return resolved;
}

#endif
