/* Canonical pathname construction with symlink resolution delegated to VFS. SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *
realpath(const char *path, char *resolved)
{
	char absolute[PATH_MAX], work[PATH_MAX], rebuilt[PATH_MAX], link[PATH_MAX];
	char *output = resolved, *component, *cursor;
	unsigned links = 0;
	if (path == NULL || *path == '\0') { errno = ENOENT; return NULL; }
	if (*path == '/') {
		if (strlen(path) >= sizeof(work)) { errno = ENAMETOOLONG; return NULL; }
		strcpy(work, path);
	} else {
		if (getcwd(work, sizeof(work)) == NULL) return NULL;
		if (strlen(work) + strlen(path) + 2 > sizeof(work)) { errno = ENAMETOOLONG; return NULL; }
		if (strcmp(work, "/")) strcat(work, "/");
		strcat(work, path);
	}
	absolute[0] = '/'; absolute[1] = '\0';
	cursor = work;
	while (*cursor != '\0') {
		struct stat status;
		char parent[PATH_MAX];
		const char *remaining;
		while (*cursor == '/') cursor++;
		if (*cursor == '\0') break;
		component = cursor;
		while (*cursor != '\0' && *cursor != '/') cursor++;
		if (*cursor != '\0') *cursor++ = '\0';
		remaining = cursor;
		if (!strcmp(component, ".")) continue;
		if (!strcmp(component, "..")) {
			char *slash;
			if (!strcmp(absolute, "/")) continue;
			slash = strrchr(absolute, '/');
			if (slash == absolute) slash[1] = '\0'; else *slash = '\0';
			continue;
		}
		strcpy(parent, absolute);
		if (strlen(absolute) + strlen(component) + 2 > sizeof(absolute)) { errno = ENAMETOOLONG; return NULL; }
		if (strcmp(absolute, "/")) strcat(absolute, "/");
		strcat(absolute, component);
		if (lstat(absolute, &status) != 0) return NULL;
		if (S_ISLNK(status.st_mode)) {
			ssize_t length;
			if (++links > 40) { errno = ELOOP; return NULL; }
			length = readlink(absolute, link, sizeof(link) - 1);
			if (length < 0) return NULL;
			link[length] = '\0';
			if (strlen(link) + strlen(remaining) + 2 > sizeof(rebuilt)) {
				errno = ENAMETOOLONG;
				return NULL;
			}
			rebuilt[0] = '\0';
			strcpy(rebuilt, link);
			if (*remaining != '\0') {
				if (*rebuilt != '\0' && rebuilt[strlen(rebuilt)-1] != '/') strcat(rebuilt, "/");
				strcat(rebuilt, remaining);
			}
			strcpy(work, rebuilt);
			cursor = work;
			if (*link == '/') { absolute[0]='/'; absolute[1]='\0'; }
			else strcpy(absolute, parent);
			continue;
		}
		if (*remaining != '\0' && !S_ISDIR(status.st_mode)) {
			errno = ENOTDIR;
			return NULL;
		}
	}
	if (output == NULL) {
		output = malloc(strlen(absolute) + 1);
		if (output == NULL) return NULL;
	}
	strcpy(output, absolute);
	return output;
}
