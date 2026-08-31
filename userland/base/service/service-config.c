/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland service service config support.
 */

#include "userland/base/service/service-config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define RC_LINE_MAX 1024

static int parse_line(char *line, char **key, char **value);

/*
 * Implements the service name valid operation.
 */
int
service_name_valid(
	const char *name)
{
	unsigned char character;
	size_t length;

	length = 0;

	/* Handles the name availability. */
	if (name == NULL || !((name[0] >= 'A' && name[0] <= 'Z') ||
			      (name[0] >= 'a' && name[0] <= 'z') ||
			      (name[0] >= '0' && name[0] <= '9')))

		/* Reports successful completion. */
		return 0;

	/* Process each remaining element. */
	while (name[length] != '\0') {
				character = (unsigned char)name[length];

		/* Classifies the current input character. */
		if (!((character >= 'A' && character <= 'Z') ||
		      (character >= 'a' && character <= 'z') ||
		      (character >= '0' && character <= '9')) &&
		    character != '_' && character != '-')

			/* Reports successful completion. */
			return 0;

		/* Checks the current data length. */
		if (++length > 63)
			return 0;
	}

	/* Returns the computed result. */
	return length != 0;
}

/*
 * Implements the assignment get operation.
 */
int
assignment_get(
	const char *path,
	const char *wanted,
	char *output,
	size_t capacity)
{
	char *key, *value;
	int result;
	FILE *stream;
	char line[RC_LINE_MAX];
	int found;

	found = 0;

	/* Handles the path availability. */
	if (path == NULL || wanted == NULL || output == NULL || capacity == 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	stream = fopen(path, "r");

	/* Handles the stream availability. */
	if (stream == NULL)
		return -1;

	/* Process input until it is exhausted. */
	while (fgets(line, sizeof(line), stream) != NULL) {
		/* Handles a failed strchr operation. */
		if (strchr(line, '\n') == NULL && !feof(stream)) {
			errno = EOVERFLOW;
			found = -1;
			break;
		}
		result = parse_line(line, &key, &value);

		/* Checks the operation result. */
		if (result < 0) {
			errno = EINVAL;
			found = -1;
			break;
		}

		/* Checks the operation result. */
		if (result == 0 || strcmp(key, wanted) != 0)
			continue;

		/* Handles a failed strlen operation. */
		if (found != 0 || strlen(value) >= capacity) {
			errno = found != 0 ? EEXIST : EOVERFLOW;
			found = -1;
			break;
		}
		strcpy(output, value);
		found = 1;
	}

	/* Handles an operation failure. */
	if (ferror(stream) && found >= 0) {
		errno = EIO;
		found = -1;
	}

	/* Handles a failed fclose operation. */
	if (fclose(stream) != 0 && found >= 0)
		found = -1;

	/* Handles the found condition. */
	if (found == 0)
		errno = ENOENT;

	/* Returns the computed result. */
	return found == 1 ? 0 : -1;
}

/* Supports the parse line operation. */
static int
parse_line(
	char *line,
	char **key,
	char **value)
{
	int quote;
	char *cursor, *equals, *end;

	cursor = line;

	/* Continue while the operation condition remains true. */
	while (isspace((unsigned char)*cursor))
		cursor++;

	/* Checks the current cursor position. */
	if (*cursor == '\0' || *cursor == '#')
		return 0;
	equals = strchr(cursor, '=');

	/* Handles the equals availability. */
	if (equals == NULL)
		return -1;

	/* Continue while the operation condition remains true. */
	end = equals;
	while (end > cursor && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	/* Handles a failed service name valid operation. */
	if (!service_name_valid(cursor))
		return -1;

	/* Continue while the operation condition remains true. */
	*equals++ = '\0';
	while (isspace((unsigned char)*equals))
		equals++;

	/* Continue while the operation condition remains true. */
	end = equals + strlen(equals);
	while (end > equals && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	/* Handles the equals condition. */
	if (*equals == '\'' || *equals == '"') {
				quote = (unsigned char)*equals++;
		end = equals + strlen(equals);

		/* Checks the current endpoint. */
		if (end == equals || end[-1] != quote)
			return -1;
		*--end = '\0';
		/* Handles a failed strchr operation. */
		if (strchr(equals, quote) != NULL)
			return -1;
	} else if (strchr(equals, '#') != NULL || strchr(equals, ';') != NULL ||
		   strchr(equals, '`') != NULL) {
		/* Reports operation failure. */
		return -1;
	}
	*key = cursor;
	*value = equals;
	/* Reports operation failure. */
	return 1;
}
