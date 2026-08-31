/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD gettext userland command.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <libintl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct options {
	const char *domain;
	int escape;
	int join;
	int newline;
};

static const char *program_basename(const char *path);
static int parse_options(int argc, char **argv, int plural, struct options *options, int *first_operand);
static char *expand_escapes(const char *source);
static int hexadecimal_value(unsigned char character);
static const char *translated(const char *domain, const char *singular, const char *plural, unsigned long count);
static int write_message(const char *message);

/*
 * Runs the gettext command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *singular;
	const char *multiple;
	const char *message;
	char *expanded;
	char *end;
	unsigned long count;
	struct options options;
	const char *program;
	const char *directory;
	char *allocated[2] = {NULL, NULL};
	int plural;
	int first;
	int operands;
	int result;

	program = program_basename(argv[0]);
	plural = !strcmp(program, "ngettext");
	result = 1;

	/* Validates the command-line arguments. */
	if (parse_options(argc, argv, plural, &options, &first) != 0)
		goto usage;
	operands = argc - first;

	/* Handles the plural condition. */
	if ((!plural && !options.join && operands != 1 && operands != 2) ||
	    (!plural && options.join && operands < 1) ||
	    (!plural && !options.join && !options.newline) ||
	    (plural && operands != 3 && operands != 4))
		goto usage;

	/* Checks the selected options. */
	if (!options.join &&
	    ((!plural && operands == 2) || (plural && operands == 4)))
		options.domain = argv[first++];
	(void)setlocale(LC_ALL, "");
	directory = getenv("TEXTDOMAINDIR");

	/* Handles the domain availability. */
	if (options.domain == NULL)
		options.domain = getenv("TEXTDOMAIN");

	/* Handles the directory availability. */
	if (directory != NULL && options.domain != NULL)
		(void)bindtextdomain(options.domain, directory);

	/* Handles the plural condition. */
	if (!plural && options.join) {
		/* Process each remaining command-line operand. */
		for (; first < argc; first++) {
			message = argv[first];
			expanded = NULL;

			/* Checks the selected options. */
			if (options.escape) {
				expanded = expand_escapes(message);

				/* Handles the expanded availability. */
				if (expanded == NULL)
					goto failure;
				message = expanded;
			}
			message = translated(options.domain, message, NULL, 1);

			/* Validates the command-line arguments. */
			if ((first != argc - operands &&
			     write_message(" ") != 0) ||
			    write_message(message) != 0) {
				free(expanded);
				goto failure;
			}
			free(expanded);
		}

		/* Handles a failed write message operation. */
		if (options.newline && write_message("\n") != 0)
			goto failure;

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the selected options. */
	if (options.escape) {
		allocated[0] = expand_escapes(argv[first]);
		allocated[1] = plural ? expand_escapes(argv[first + 1]) : NULL;

		/* Handles the allocated condition. */
		if (allocated[0] == NULL || (plural && allocated[1] == NULL))
			goto failure;
	}

	/* Handles the plural condition. */
	if (plural) {
		singular = allocated[0] != NULL ? allocated[0] : argv[first];
		multiple = allocated[1] != NULL ? allocated[1] : argv[first + 1];

		errno = 0;
		count = strtoul(argv[first + 2], &end, 10);

		/* Validates the command-line arguments. */
		if (errno != 0 || *argv[first + 2] == '\0' ||
		    argv[first + 2][0] == '-' || *end != '\0')
			goto usage_free;

		/* Handles a failed write message operation. */
		if (write_message(translated(options.domain, singular, multiple,
					     count)) != 0)
			goto failure;
	} else if (write_message(translated(options.domain,
					    allocated[0] != NULL ? allocated[0]
								 : argv[first],
					    NULL, 1)) != 0) {
		goto failure;
	}
	result = 0;
failure:

	/* Checks the operation result. */
	if (result != 0)
		command_error(program, NULL);
	free(allocated[0]);
	free(allocated[1]);

	/* Returns the computed result. */
	return result;
usage_free:
	free(allocated[0]);
	free(allocated[1]);
usage:
	fprintf(
	    stderr,
	    plural
		? "usage: ngettext [-e|-E] [-d textdomain] [textdomain] msgid "
		  "msgid_plural n\n"
		: "usage: gettext [-e|-E] [-d textdomain] [textdomain] msgid\n"
		  "       gettext [-e|-E] [-n] -s [-d textdomain] msgid ...\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the program basename operation. */
static const char *
program_basename(
	const char *path)
{
	const char *slash;

	slash = strrchr(path, '/');

	/* Returns the computed result. */
	return slash != NULL ? slash + 1 : path;
}

/* Supports the parse options operation. */
static int
parse_options(
	int argc,
	char **argv,
	int plural,
	struct options *options,
	int *first_operand)
{
	const char *option;
	unsigned position;
	int index;

	memset(options, 0, sizeof(*options));

	/* Process each remaining command-line operand. */
	options->escape = -1;
	options->newline = 1;
	for (index = 1; index < argc; index++) {
		option = argv[index];

		/* Handles the option condition. */
		if (option[0] != '-' || option[1] == '\0')
			break;

		/* Selects the matching value. */
		if (!strcmp(option, "--")) {
			index++;
			break;
		}

		/* Process each element required by the operation. */
		for (position = 1; option[position] != '\0'; position++) {
			/* Dispatch the selected command-line option. */
			switch (option[position]) {
			case 'e':
				/* Checks the selected options. */
				if (options->escape == 0)
					return -1;
				options->escape = 1;
				break;
			case 'E':
				/* Checks the selected options. */
				if (options->escape == 1)
					return -1;
				options->escape = 0;
				break;
			case 'n':
				/* Handles the plural condition. */
				if (plural)
					return -1;
				options->newline = 0;
				break;
			case 's':
				/* Handles the plural condition. */
				if (plural)
					return -1;
				options->join = 1;
				break;
			case 'd':
				/* Handles the option condition. */
				if (option[position + 1U] != '\0') {
					options->domain =
					    option + position + 1U;
					position =
					    (unsigned)strlen(option) - 1U;
				} else if (++index < argc) {
					options->domain = argv[index];
				} else {
					/* Reports operation failure. */
					return -1;
				}
				break;
			default:
				/* Reports operation failure. */
				return -1;
			}
		}
	}

	/* Checks the selected options. */
	if (options->escape < 0)
		options->escape = options->join ? 0 : 1;
	*first_operand = index;
	/* Reports successful completion. */
	return 0;
}

/* Supports the expand escapes operation. */
static char *
expand_escapes(
	const char *source)
{
	int digit;
	unsigned value;
	unsigned digits;
	char *result;
	char *destination;

	result = malloc(strlen(source) + 1U);
	destination = result;

	/* Handles the result availability. */
	if (result == NULL)
		return NULL;

	/* Continue while the operation condition remains true. */
	while (*source != '\0') {
		/* Handles the source condition. */
		if (*source != '\\') {
			*destination++ = *source++;
			continue;
		}
		source++;

		/* Handles the source condition. */
		if (*source == '\0') {
			*destination++ = '\\';
			break;
		}

		/* Dispatch the selected operation case. */
		switch (*source) {
		case 'a':
			*destination++ = '\a';
			source++;
			continue;
		case 'b':
			*destination++ = '\b';
			source++;
			continue;
		case 'f':
			*destination++ = '\f';
			source++;
			continue;
		case 'n':
			*destination++ = '\n';
			source++;
			continue;
		case 'r':
			*destination++ = '\r';
			source++;
			continue;
		case 't':
			*destination++ = '\t';
			source++;
			continue;
		case 'v':
			*destination++ = '\v';
			source++;
			continue;
		case '\\':
			*destination++ = '\\';
			source++;
			continue;
		case '\'':
			*destination++ = '\'';
			source++;
			continue;
		case '"':
			*destination++ = '"';
			source++;
			continue;
		default:
			break;
		}

		/* Continue while the operation condition remains true. */
		value = 0;
		digits = 0;
		while (digits < 3U && *source >= '0' && *source <= '7') {
			value = value * 8U + (unsigned)(*source++ - '0');
			digits++;
		}

		/* Handles the digits condition. */
		if (digits != 0U) {
			*destination++ = (char)(unsigned char)value;
			continue;
		}

		/* Handles the source condition. */
		if (*source == 'x') {
			source++;

			/* Continue while the operation condition remains true. */
			value = 0;
			digits = 0;
			while ((digit = hexadecimal_value(
				    (unsigned char)*source)) >= 0) {
				value = value * 16U + (unsigned)digit;
				source++;
				digits++;
			}

			/* Handles the digits condition. */
			if (digits != 0U) {
				*destination++ = (char)(unsigned char)value;
				continue;
			}
			*destination++ = '\\';
			*destination++ = 'x';
			continue;
		}
		*destination++ = '\\';
		*destination++ = *source++;
	}
	*destination = '\0';
	/* Returns the computed result. */
	return result;
}

/* Supports the hexadecimal value operation. */
static int
hexadecimal_value(
	unsigned char character)
{
	/* Classifies the current input character. */
	if (character >= '0' && character <= '9')
		return character - '0';

	/* Classifies the current input character. */
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;

	/* Classifies the current input character. */
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;

	/* Reports operation failure. */
	return -1;
}

/* Supports the translated operation. */
static const char *
translated(
	const char *domain,
	const char *singular,
	const char *plural,
	unsigned long count)
{
	const char *function_result;

	/* Computes the function result. */
	function_result = plural == NULL ? dgettext(domain, singular)
			      : dngettext(domain, singular, plural, count);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the write message operation. */
static int
write_message(
	const char *message)
{
	int function_result;

	/* Obtains the command write all result. */
	function_result = command_write_all(STDOUT_FILENO, message, strlen(message));

	/* Returns the computed result. */
	return function_result;
}
