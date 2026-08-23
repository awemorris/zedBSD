/* SUSv4 path component helpers. SPDX-License-Identifier: Zlib */
#include <libgen.h>
#include <stddef.h>
#include <string.h>

static char dot[] = ".";
static char slash[] = "/";

char *
basename(char *path)
{
	char *end, *start;

	if (path == NULL || *path == '\0')
		return dot;
	end = path + strlen(path);
	while (end > path + 1 && end[-1] == '/')
		end--;
	*end = '\0';
	if (end == path + 1 && path[0] == '/')
		return path;
	start = end;
	while (start > path && start[-1] != '/')
		start--;
	return start;
}

char *
dirname(char *path)
{
	char *end, *component;

	if (path == NULL || *path == '\0')
		return dot;
	end = path + strlen(path);
	while (end > path && end[-1] == '/')
		end--;
	if (end == path)
		return slash;
	component = end;
	while (component > path && component[-1] != '/')
		component--;
	if (component == path)
		return dot;
	while (component > path + 1 && component[-1] == '/')
		component--;
	*component = '\0';
	return path;
}
