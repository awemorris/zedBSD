/* Legacy temporary pathname generation; callers should prefer mkstemp. SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	if (prefix == NULL) prefix = "";
	for (index = 0; index < 4; index++) {
		const char *dir = candidates[index];
		size_t dir_length, prefix_length;
		if (dir == NULL || *dir == '\0' || access(dir, W_OK | X_OK) != 0) continue;
		dir_length = strlen(dir);
		prefix_length = strlen(prefix);
		if (prefix_length > 5) prefix_length = 5;
		result = malloc(dir_length + prefix_length + 19);
		if (result == NULL) return NULL;
		for (attempt = 0; attempt < TMP_MAX; attempt++) {
			uint32_t random = arc4random();
			snprintf(result, dir_length + prefix_length + 19, "%s/%.*s%08lx%04x",
			    dir, (int)prefix_length, prefix, (unsigned long)random, attempt);
			if (access(result, F_OK) != 0 && errno == ENOENT) return result;
		}
		free(result);
	}
	errno = EEXIST;
	return NULL;
}
