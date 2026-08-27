/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

int
service_console_print_help(FILE *stream)
{
	if (stream == NULL) {
		errno = EINVAL;
		return -1;
	}
	return fprintf(stream,
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
}

static enum console_line_result
read_console_line(FILE *input, char *line, size_t capacity)
{
	size_t length = 0;
	int invalid = 0, too_long = 0;
	int character;

	for (;;) {
		character = fgetc(input);
		if (character == EOF) {
			if (ferror(input))
				return CONSOLE_LINE_ERROR;
			if (length == 0 && !invalid && !too_long)
				return CONSOLE_LINE_EOF;
			break;
		}
		if (character == '\n')
			break;
		if (character != ' ' && character != '\t' &&
		    (character < 0x21 || character > 0x7e))
			invalid = 1;
		if (length + 1U < capacity)
			line[length++] = (char)character;
		else
			too_long = 1;
	}
	line[length] = '\0';
	if (too_long)
		return CONSOLE_LINE_TOO_LONG;
	if (invalid)
		return CONSOLE_LINE_INVALID;
	return CONSOLE_LINE_READY;
}

static int
tokenize(char *line, char **arguments, size_t capacity)
{
	char *cursor = line;
	size_t count = 0;

	for (;;) {
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		if (*cursor == '\0')
			return (int)count;
		if (count == capacity)
			return -1;
		arguments[count++] = cursor;
		while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t')
			cursor++;
		if (*cursor != '\0')
			*cursor++ = '\0';
	}
}

static int
console_context_valid(const struct service_command_context *context,
		      FILE *input)
{
	return context != NULL && input != NULL && context->output != NULL &&
	       context->error != NULL;
}

int
service_console_run(struct service_command_context *context, FILE *input)
{
	char line[SERVICE_CONSOLE_LINE_CAPACITY];
	char *arguments[SERVICE_CONSOLE_ARGUMENT_MAX];

	if (!console_context_valid(context, input)) {
		errno = EINVAL;
		return 1;
	}
	if (context->effective_uid != 0) {
		(void)fprintf(context->error,
			      "service: effective UID 0 is required\n");
		return 1;
	}
	if (fprintf(context->output,
		    "zedBSD Service Console\nType '?' for help.\n") < 0)
		return 1;
	for (;;) {
		enum console_line_result line_result;
		int argument_count;

		if (fprintf(context->output, "service> ") < 0 ||
		    fflush(context->output) != 0)
			return 1;
		line_result = read_console_line(input, line, sizeof(line));
		if (line_result == CONSOLE_LINE_EOF)
			return 0;
		if (line_result == CONSOLE_LINE_ERROR) {
			(void)fprintf(context->error,
				      "service: console input failed: %s\n",
				      strerror(errno));
			return 1;
		}
		if (line_result == CONSOLE_LINE_TOO_LONG) {
			if (fprintf(context->error,
				    "service: input line too long\n") < 0)
				return 1;
			continue;
		}
		if (line_result == CONSOLE_LINE_INVALID) {
			if (fprintf(context->error,
				    "service: invalid input character\n") < 0)
				return 1;
			continue;
		}
		argument_count =
		    tokenize(line, arguments, SERVICE_CONSOLE_ARGUMENT_MAX);
		if (argument_count < 0) {
			if (fprintf(context->error,
				    "service: too many command fields\n") < 0)
				return 1;
			continue;
		}
		if (argument_count == 0)
			continue;
		if (strcmp(arguments[0], "exit") == 0 ||
		    strcmp(arguments[0], "quit") == 0) {
			if (argument_count == 1)
				return 0;
			if (fprintf(context->error,
				    "service: %s takes no arguments\n",
				    arguments[0]) < 0)
				return 1;
			continue;
		}
		if (strcmp(arguments[0], "help") == 0 ||
		    strcmp(arguments[0], "?") == 0) {
			if (argument_count == 1) {
				if (service_console_print_help(
					context->output) != 0)
					return 1;
			} else if (fprintf(context->error,
					   "service: %s takes no arguments\n",
					   arguments[0]) < 0) {
				return 1;
			}
			continue;
		}
		(void)service_command_dispatch(context, argument_count,
					       arguments);
		if (ferror(context->output) || ferror(context->error))
			return 1;
	}
}
