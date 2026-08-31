/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland service service console support.
 */

#include "userland/base/service/service-console.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

enum console_line_result {
	CONSOLE_LINE_READY,
	CONSOLE_LINE_EOF,
	CONSOLE_LINE_TOO_LONG,
	CONSOLE_LINE_INVALID,
	CONSOLE_LINE_ERROR,
};

static int console_context_valid(const struct service_command_context *context, FILE *input);
static enum console_line_result read_console_line(FILE *input, char *line, size_t capacity);
static int tokenize(char *line, char **arguments, size_t capacity);

/*
 * Implements the service console print help operation.
 */
int
service_console_print_help(
	FILE *stream)
{
	int function_result;

	/* Handles the stream availability. */
	if (stream == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Computes the function result. */
	function_result = fprintf(stream,
		       "Commands:\n"
		       "  show [NAME]    show all services or one service\n"
		       "  list           show all services\n"
		       "  status NAME    show one service\n"
		       "  start NAME     start a service for this boot\n"
		       "  stop NAME      stop a service for this boot\n"
		       "  restart NAME   restart a service for this boot\n"
		       "  enable NAME    enable a service persistently\n"
		       "  disable NAME   disable a service persistently\n"
		       "  reload         reload persistent policy\n"
		       "  help, ?        show this help\n"
		       "  exit, quit     leave the console\n") < 0
		   ? -1
		   : 0;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the service console run operation.
 */
int
service_console_run(
	struct service_command_context *context,
	FILE *input)
{
	enum console_line_result line_result;
	int argument_count;
	char line[SERVICE_CONSOLE_LINE_CAPACITY];
	char *arguments[SERVICE_CONSOLE_ARGUMENT_MAX];

	/* Handles a failed console context valid operation. */
	if (!console_context_valid(context, input)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the context condition. */
	if (context->effective_uid != 0) {
		(void)fprintf(context->error,
			      "service: effective UID 0 is required\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed fprintf operation. */
	if (fprintf(context->output,
		    "zedBSD Service Console\nType '?' for help.\n") < 0)

		/* Reports operation failure. */
		return 1;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed fprintf operation. */
		if (fprintf(context->output, "service> ") < 0 ||
		    fflush(context->output) != 0)

			/* Reports operation failure. */
			return 1;
		line_result = read_console_line(input, line, sizeof(line));

		/* Handles the line result condition. */
		if (line_result == CONSOLE_LINE_EOF)
			return 0;

		/* Handles an operation failure. */
		if (line_result == CONSOLE_LINE_ERROR) {
			(void)fprintf(context->error,
				      "service: console input failed: %s\n",
				      strerror(errno));

			/* Reports operation failure. */
			return 1;
		}

		/* Handles the line result condition. */
		if (line_result == CONSOLE_LINE_TOO_LONG) {
			/* Handles an operation failure. */
			if (fprintf(context->error,
				    "service: input line too long\n") < 0)

				/* Reports operation failure. */
				return 1;
			continue;
		}

		/* Handles the line result condition. */
		if (line_result == CONSOLE_LINE_INVALID) {
			/* Handles an operation failure. */
			if (fprintf(context->error,
				    "service: invalid input character\n") < 0)

				/* Reports operation failure. */
				return 1;
			continue;
		}
		argument_count =
		    tokenize(line, arguments, SERVICE_CONSOLE_ARGUMENT_MAX);

		/* Handles the argument count condition. */
		if (argument_count < 0) {
			/* Handles an operation failure. */
			if (fprintf(context->error,
				    "service: too many command fields\n") < 0)

				/* Reports operation failure. */
				return 1;
			continue;
		}

		/* Handles the argument count condition. */
		if (argument_count == 0)
			continue;

		/* Selects the matching value. */
		if (strcmp(arguments[0], "exit") == 0 ||
		    strcmp(arguments[0], "quit") == 0) {
			/* Handles the argument count condition. */
			if (argument_count == 1)
				return 0;

			/* Handles an operation failure. */
			if (fprintf(context->error,
				    "service: %s takes no arguments\n",
				    arguments[0]) < 0)

				/* Reports operation failure. */
				return 1;
			continue;
		}

		/* Selects the matching value. */
		if (strcmp(arguments[0], "help") == 0 ||
		    strcmp(arguments[0], "?") == 0) {
			/* Handles the argument count condition. */
			if (argument_count == 1) {
				/* Handles a failed service console print help operation. */
				if (service_console_print_help(
					context->output) != 0)

					/* Reports operation failure. */
					return 1;
			} else if (fprintf(context->error,
					   "service: %s takes no arguments\n",
					   arguments[0]) < 0) {
				/* Reports operation failure. */
				return 1;
			}
			continue;
		}
		(void)service_command_dispatch(context, argument_count,
					       arguments);

		/* Handles an operation failure. */
		if (ferror(context->output) || ferror(context->error))
			return 1;
	}
}

/* Supports the console context valid operation. */
static int
console_context_valid(
	const struct service_command_context *context,
	FILE *input)
{
	/* Returns the computed result. */
	return context != NULL && input != NULL && context->output != NULL &&
	       context->error != NULL;
}

/* Supports the read console line operation. */
static enum console_line_result
read_console_line(
	FILE *input,
	char *line,
	size_t capacity)
{
	size_t length;
	int invalid, too_long;
	int character;

	length = 0;
	invalid = 0;
	too_long = 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		character = fgetc(input);

		/* Handles the end-of-file condition. */
		if (character == EOF) {
			/* Handles an operation failure. */
			if (ferror(input))
				return CONSOLE_LINE_ERROR;

			/* Checks the current data length. */
			if (length == 0 && !invalid && !too_long)
				return CONSOLE_LINE_EOF;
			break;
		}

		/* Classifies the current input character. */
		if (character == '\n')
			break;

		/* Classifies the current input character. */
		if (character != ' ' && character != '\t' &&
		    (character < 0x21 || character > 0x7e))
			invalid = 1;

		/* Checks the current data length. */
		if (length + 1U < capacity)
			line[length++] = (char)character;
		else
			too_long = 1;
	}
	line[length] = '\0';

	/* Handles the too long condition. */
	if (too_long)
		return CONSOLE_LINE_TOO_LONG;

	/* Handles the invalid condition. */
	if (invalid)
		return CONSOLE_LINE_INVALID;

	/* Returns the computed result. */
	return CONSOLE_LINE_READY;
}

/* Supports the tokenize operation. */
static int
tokenize(
	char *line,
	char **arguments,
	size_t capacity)
{
	char *cursor;
	size_t count;

	cursor = line;
	count = 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Continue while the operation condition remains true. */
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;

		/* Checks the current cursor position. */
		if (*cursor == '\0')
			return (int)count;

		/* Checks the remaining item count. */
		if (count == capacity)
			return -1;

		/* Continue while the operation condition remains true. */
		arguments[count++] = cursor;
		while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t')
			cursor++;

		/* Checks the current cursor position. */
		if (*cursor != '\0')
			*cursor++ = '\0';
	}
}
