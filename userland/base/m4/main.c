/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD m4 userland command.
 */

#include "userland/base/common/command.h"
#include "userland/base/m4/m4.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M4_INPUT_MAX (16U * 1024U * 1024U)

static int option_definition(struct m4_context *context, const char *option);
static void usage(void);
static int process_file(struct m4_context *context, const char *path);
static int read_stream(FILE *stream, char **result, size_t *result_length);

/*
 * Runs the m4 command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *value;
	const char *argument;
	struct m4_context *context;
	int index;
	int status;
	int define;

	context = m4_context_create();
	index = 1;
	status = 0;

	/* Handles the context availability. */
	if (context == NULL) {
		fprintf(stderr, "m4: out of memory\n");

		/* Reports operation failure. */
		return 1;
	}
	while (index < argc && argv[index][0] == '-') {
		argument = argv[index];

		/* Selects the matching value. */
		if (strcmp(argument, "--") == 0) {
			index++;
			break;
		}

		/* Selects the matching value. */
		if (strcmp(argument, "-s") == 0) {
			index++;
			continue;
		}

		/* Handles the argument condition. */
		if (argument[1] == 'D' || argument[1] == 'U') {
			value = argument + 2;
			define = argument[1] == 'D';

			/* Validates the command-line arguments. */
			if (*value == '\0' && ++index < argc)
				value = argv[index];

			/* Handles a failed option definition operation. */
			if (*value == '\0' ||
			    (define ? option_definition(context, value)
				    : m4_undefine(context, value)) != 0) {
				usage();
				status = 2;
				goto done;
			}
			index++;
			continue;
		}
		usage();
		status = 2;
		goto done;
	}

	/* Validates the command-line arguments. */
	if (index == argc)
		status = process_file(context, "-") != 0;
	else

		/* Process each remaining command-line operand. */
		for (; index < argc; index++) {
			/* Validates the command-line arguments. */
			if (process_file(context, argv[index]) != 0) {
				status = 1;
				break;
			}
		}

	/* Handles a failed m4 finish operation. */
	if (status == 0 && m4_finish(context, 1) != 0) {
		fprintf(stderr, "m4: output failed: %s\n", strerror(errno));
		status = 1;
	}
done:
	m4_context_destroy(context);

	/* Returns the computed result. */
	return status;
}

/* Supports the option definition operation. */
static int
option_definition(
	struct m4_context *context,
	const char *option)
{
	char *copy;
	char *equals;
	int result;

	copy = strdup(option);

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;
	equals = strchr(copy, '=');

	/* Handles the equals availability. */
	if (equals != NULL)
		*equals++ = '\0';
	/* Handles the copy condition. */
	if (*copy == '\0') {
		free(copy);

		/* Reports operation failure. */
		return -1;
	}
	result = m4_define(context, copy, equals == NULL ? "" : equals);
	free(copy);

	/* Returns the computed result. */
	return result;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr,
		"usage: m4 [-s] [-D name[=value]] [-U name] [file ...]\n");
}

/* Supports the process file operation. */
static int
process_file(
	struct m4_context *context,
	const char *path)
{
	FILE *stream = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
	char *text;
	size_t length;
	int result;

	/* Handles the stream availability. */
	if (stream == NULL) {
		fprintf(stderr, "m4: %s: %s\n", path, strerror(errno));

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed read stream operation. */
	if (read_stream(stream, &text, &length) != 0) {
		fprintf(stderr, "m4: %s: %s\n", path, strerror(errno));

		/* Handles the stream condition. */
		if (stream != stdin)
			(void)fclose(stream);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed fclose operation. */
	if (stream != stdin && fclose(stream) != 0) {
		fprintf(stderr, "m4: %s: %s\n", path, strerror(errno));
		free(text);

		/* Reports operation failure. */
		return -1;
	}
	result = m4_process(context, path, text, length);
	free(text);

	/* Checks the operation result. */
	if (result != 0)
		fprintf(stderr, "m4: %s\n", m4_error(context));

	/* Returns the computed result. */
	return result;
}

/* Supports the read stream operation. */
static int
read_stream(
	FILE *stream,
	char **result,
	size_t *result_length)
{
	char *grown;
	size_t needed;
	char buffer[4096];
	char *text;
	size_t length;
	size_t capacity;
	size_t count;

	text = NULL;
	length = 0;
	capacity = 0;

	/* Process input until it is exhausted. */
	while ((count = fread(buffer, 1, sizeof(buffer), stream)) != 0) {
		/* Checks the remaining item count. */
		if (count > M4_INPUT_MAX - length) {
			errno = EOVERFLOW;
			goto fail;
		}
		needed = length + count + 1;

		/* Handles the needed condition. */
		if (needed > capacity) {
			/* Continue while the operation condition remains true. */
			capacity = capacity == 0 ? 4096 : capacity;
			while (capacity < needed)
				capacity *= 2;
			grown = realloc(text, capacity);

			/* Handles the grown availability. */
			if (grown == NULL)
				goto fail;
			text = grown;
		}
		memcpy(text + length, buffer, count);
		length += count;
	}

	/* Handles an operation failure. */
	if (ferror(stream))
		goto fail;

	/* Handles the text availability. */
	if (text == NULL) {
		text = malloc(1);

		/* Handles the text availability. */
		if (text == NULL)
			return -1;
	}
	text[length] = '\0';
	*result = text;
	*result_length = length;
	/* Reports successful completion. */
	return 0;

fail:
	free(text);

	/* Reports operation failure. */
	return -1;
}
