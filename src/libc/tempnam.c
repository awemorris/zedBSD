/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Legacy temporary pathname generation, callers should prefer mkstemp.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Makes and opens a new file from a template whose last six characters,
 * before a suffix of suffix characters, are XXXXXX.  flags adds open
 * flags such as O_CLOEXEC or O_APPEND to O_RDWR | O_CREAT | O_EXCL.
 */
int
mkostemps(char *path, int suffix, int flags)
{
	size_t length;
	size_t end;
	unsigned attempt;

	if (path == NULL || suffix < 0 ||
	    (flags & ~(O_APPEND | O_CLOEXEC | O_SYNC)) != 0 ||
	    (length = strlen(path)) < 6U + (size_t)suffix ||
	    memcmp(path + length - (size_t)suffix - 6U, "XXXXXX", 6U) != 0) {
		errno = EINVAL;
		return -1;
	}
	end = length - (size_t)suffix;
	for (attempt = 0; attempt < TMP_MAX; attempt++) {
		static const char alphabet[] =
		    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ012345"
		    "6789";
		uint32_t random = arc4random() ^ attempt;
		unsigned index;
		int descriptor;

		for (index = 0; index < 6U; index++) {
			path[end - 6U + index] =
			    alphabet[random % (sizeof(alphabet) - 1U)];
			random =
			    random / (sizeof(alphabet) - 1U) ^ arc4random();
		}
		descriptor = open(path, O_RDWR | O_CREAT | O_EXCL | flags, 0600);
		if (descriptor >= 0)
			return descriptor;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}

/* Makes and opens a new file from a template ending in XXXXXX, with flags. */
int
mkostemp(char *path, int flags)
{
	return mkostemps(path, 0, flags);
}

int
mkstemp(char *path)
{
	return mkostemps(path, 0, 0);
}

char *
tempnam(const char *directory, const char *prefix)
{
	const char *candidates[4];
	char *result;
	unsigned attempt, index;
	candidates[0] = getenv("TMPDIR");
	candidates[1] = directory;
	candidates[2] = "/tmp";
	candidates[3] = ".";
	if (prefix == NULL)
		prefix = "";
	for (index = 0; index < 4; index++) {
		const char *dir = candidates[index];
		size_t dir_length, prefix_length;
		if (dir == NULL || *dir == '\0' ||
		    access(dir, W_OK | X_OK) != 0)
			continue;
		dir_length = strlen(dir);
		prefix_length = strlen(prefix);
		if (prefix_length > 5)
			prefix_length = 5;
		result = malloc(dir_length + prefix_length + 19);
		if (result == NULL)
			return NULL;
		for (attempt = 0; attempt < TMP_MAX; attempt++) {
			uint32_t random = arc4random();
			snprintf(result, dir_length + prefix_length + 19,
				 "%s/%.*s%08lx%04x", dir, (int)prefix_length,
				 prefix, (unsigned long)random, attempt);
			if (access(result, F_OK) != 0 && errno == ENOENT)
				return result;
		}
		free(result);
	}
	errno = EEXIST;
	return NULL;
}
