/* Legacy temporary pathname generation; callers should prefer mkstemp.
 * SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
mkstemp(char *path)
{
	size_t length;
	unsigned attempt;

	if (path == NULL || (length = strlen(path)) < 6U ||
	    memcmp(path + length - 6U, "XXXXXX", 6U) != 0) {
		errno = EINVAL;
		return -1;
	}
	for (attempt = 0; attempt < TMP_MAX; attempt++) {
		static const char alphabet[] =
		    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ012345"
		    "6789";
		uint32_t random = arc4random() ^ attempt;
		unsigned index;
		int descriptor;

		for (index = 0; index < 6U; index++) {
			path[length - 6U + index] =
			    alphabet[random % (sizeof(alphabet) - 1U)];
			random =
			    random / (sizeof(alphabet) - 1U) ^ arc4random();
		}
		descriptor = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (descriptor >= 0)
			return descriptor;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
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
