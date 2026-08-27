/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/service/service-config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define RC_LINE_MAX 1024

int
service_name_valid(const char *name)
{
	size_t length = 0;

	if (name == NULL || !((name[0] >= 'A' && name[0] <= 'Z') ||
			      (name[0] >= 'a' && name[0] <= 'z') ||
			      (name[0] >= '0' && name[0] <= '9')))
		return 0;
	while (name[length] != '\0') {
		unsigned char character = (unsigned char)name[length];
		if (!((character >= 'A' && character <= 'Z') ||
		      (character >= 'a' && character <= 'z') ||
		      (character >= '0' && character <= '9')) &&
		    character != '_' && character != '-')
			return 0;
		if (++length > 63)
			return 0;
	}
	return length != 0;
}

static int
parse_line(char *line, char **key, char **value)
{
	char *cursor = line, *equals, *end;

	while (isspace((unsigned char)*cursor))
		cursor++;
	if (*cursor == '\0' || *cursor == '#')
		return 0;
	equals = strchr(cursor, '=');
	if (equals == NULL)
		return -1;
	end = equals;
	while (end > cursor && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	if (!service_name_valid(cursor))
		return -1;
	*equals++ = '\0';
	while (isspace((unsigned char)*equals))
		equals++;
	end = equals + strlen(equals);
	while (end > equals && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	if (*equals == '\'' || *equals == '"') {
		int quote = (unsigned char)*equals++;
		end = equals + strlen(equals);
		if (end == equals || end[-1] != quote)
			return -1;
		*--end = '\0';
		if (strchr(equals, quote) != NULL)
			return -1;
	} else if (strchr(equals, '#') != NULL || strchr(equals, ';') != NULL ||
		   strchr(equals, '`') != NULL) {
		return -1;
	}
	*key = cursor;
	*value = equals;
	return 1;
}

int
assignment_get(const char *path, const char *wanted, char *output,
	       size_t capacity)
{
	FILE *stream;
	char line[RC_LINE_MAX];
	int found = 0;

	if (path == NULL || wanted == NULL || output == NULL || capacity == 0) {
		errno = EINVAL;
		return -1;
	}
	stream = fopen(path, "r");
	if (stream == NULL)
		return -1;
	while (fgets(line, sizeof(line), stream) != NULL) {
		char *key, *value;
		int result;
		if (strchr(line, '\n') == NULL && !feof(stream)) {
			errno = EOVERFLOW;
			found = -1;
			break;
		}
		result = parse_line(line, &key, &value);
		if (result < 0) {
			errno = EINVAL;
			found = -1;
			break;
		}
		if (result == 0 || strcmp(key, wanted) != 0)
			continue;
		if (found != 0 || strlen(value) >= capacity) {
			errno = found != 0 ? EEXIST : EOVERFLOW;
			found = -1;
			break;
		}
		strcpy(output, value);
		found = 1;
	}
	if (ferror(stream) && found >= 0) {
		errno = EIO;
		found = -1;
	}
	if (fclose(stream) != 0 && found >= 0)
		found = -1;
	if (found == 0)
		errno = ENOENT;
	return found == 1 ? 0 : -1;
}
