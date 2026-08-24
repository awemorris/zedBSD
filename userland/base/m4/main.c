/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/command.h"
#include "userland/base/m4/m4.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M4_INPUT_MAX (16U * 1024U * 1024U)

static int
read_stream(FILE *stream, char **result, size_t *result_length)
{
	char buffer[4096];
	char *text = NULL;
	size_t length = 0;
	size_t capacity = 0;
	size_t count;

	while ((count = fread(buffer, 1, sizeof(buffer), stream)) != 0) {
		char *grown;
		size_t needed;

		if (count > M4_INPUT_MAX - length) {
			errno = EOVERFLOW;
			goto fail;
		}
		needed = length + count + 1;
		if (needed > capacity) {
			capacity = capacity == 0 ? 4096 : capacity;
			while (capacity < needed)
				capacity *= 2;
			grown = realloc(text, capacity);
			if (grown == NULL)
				goto fail;
			text = grown;
		}
		memcpy(text + length, buffer, count);
		length += count;
	}
	if (ferror(stream))
		goto fail;
	if (text == NULL) {
		text = malloc(1);
		if (text == NULL)
			return -1;
	}
	text[length] = '\0';
	*result = text;
	*result_length = length;
	return 0;

fail:
	free(text);
	return -1;
}

static int
process_file(struct m4_context *context, const char *path)
{
	FILE *stream = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
	char *text;
	size_t length;
	int result;

	if (stream == NULL) {
		fprintf(stderr, "m4: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (read_stream(stream, &text, &length) != 0) {
		fprintf(stderr, "m4: %s: %s\n", path, strerror(errno));
		if (stream != stdin)
			(void)fclose(stream);
		return -1;
	}
	if (stream != stdin && fclose(stream) != 0) {
		fprintf(stderr, "m4: %s: %s\n", path, strerror(errno));
		free(text);
		return -1;
	}
	result = m4_process(context, path, text, length);
	free(text);
	if (result != 0)
		fprintf(stderr, "m4: %s\n", m4_error(context));
	return result;
}

static int
option_definition(struct m4_context *context, const char *option)
{
	char *copy = strdup(option);
	char *equals;
	int result;

	if (copy == NULL)
		return -1;
	equals = strchr(copy, '=');
	if (equals != NULL)
		*equals++ = '\0';
	if (*copy == '\0') {
		free(copy);
		return -1;
	}
	result = m4_define(context, copy, equals == NULL ? "" : equals);
	free(copy);
	return result;
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: m4 [-s] [-D name[=value]] [-U name] [file ...]\n");
}

int
main(int argc, char **argv)
{
	struct m4_context *context = m4_context_create();
	int index = 1;
	int status = 0;

	if (context == NULL) {
		fprintf(stderr, "m4: out of memory\n");
		return 1;
	}
	while (index < argc && argv[index][0] == '-') {
		const char *argument = argv[index];

		if (strcmp(argument, "--") == 0) {
			index++;
			break;
		}
		if (strcmp(argument, "-s") == 0) {
			index++;
			continue;
		}
		if (argument[1] == 'D' || argument[1] == 'U') {
			const char *value = argument + 2;
			int define = argument[1] == 'D';

			if (*value == '\0' && ++index < argc)
				value = argv[index];
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
	if (index == argc)
		status = process_file(context, "-") != 0;
	else
		for (; index < argc; index++)
			if (process_file(context, argv[index]) != 0) {
				status = 1;
				break;
			}
	if (status == 0 && m4_finish(context, 1) != 0) {
		fprintf(stderr, "m4: output failed: %s\n", strerror(errno));
		status = 1;
	}
done:
	m4_context_destroy(context);
	return status;
}
