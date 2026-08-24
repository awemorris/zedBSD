/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
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

static const char *
program_basename(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash != NULL ? slash + 1 : path;
}

static int
hexadecimal_value(unsigned char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	return -1;
}

static char *
expand_escapes(const char *source)
{
	char *result = malloc(strlen(source) + 1U);
	char *destination = result;

	if (result == NULL)
		return NULL;
	while (*source != '\0') {
		unsigned value;
		unsigned digits;

		if (*source != '\\') {
			*destination++ = *source++;
			continue;
		}
		source++;
		if (*source == '\0') {
			*destination++ = '\\';
			break;
		}
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
		value = 0;
		digits = 0;
		while (digits < 3U && *source >= '0' && *source <= '7') {
			value = value * 8U + (unsigned)(*source++ - '0');
			digits++;
		}
		if (digits != 0U) {
			*destination++ = (char)(unsigned char)value;
			continue;
		}
		if (*source == 'x') {
			int digit;

			source++;
			value = 0;
			digits = 0;
			while ((digit = hexadecimal_value(
				    (unsigned char)*source)) >= 0) {
				value = value * 16U + (unsigned)digit;
				source++;
				digits++;
			}
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
	return result;
}

static int
parse_options(int argc, char **argv, int plural, struct options *options,
	      int *first_operand)
{
	int index;

	memset(options, 0, sizeof(*options));
	options->escape = -1;
	options->newline = 1;
	for (index = 1; index < argc; index++) {
		const char *option = argv[index];
		unsigned position;

		if (option[0] != '-' || option[1] == '\0')
			break;
		if (!strcmp(option, "--")) {
			index++;
			break;
		}
		for (position = 1; option[position] != '\0'; position++) {
			switch (option[position]) {
			case 'e':
				if (options->escape == 0)
					return -1;
				options->escape = 1;
				break;
			case 'E':
				if (options->escape == 1)
					return -1;
				options->escape = 0;
				break;
			case 'n':
				if (plural)
					return -1;
				options->newline = 0;
				break;
			case 's':
				if (plural)
					return -1;
				options->join = 1;
				break;
			case 'd':
				if (option[position + 1U] != '\0') {
					options->domain =
					    option + position + 1U;
					position =
					    (unsigned)strlen(option) - 1U;
				} else if (++index < argc) {
					options->domain = argv[index];
				} else {
					return -1;
				}
				break;
			default:
				return -1;
			}
		}
	}
	if (options->escape < 0)
		options->escape = options->join ? 0 : 1;
	*first_operand = index;
	return 0;
}

static const char *
translated(const char *domain, const char *singular, const char *plural,
	   unsigned long count)
{
	return plural == NULL ? dgettext(domain, singular)
			      : dngettext(domain, singular, plural, count);
}

static int
write_message(const char *message)
{
	return command_write_all(STDOUT_FILENO, message, strlen(message));
}

int
main(int argc, char **argv)
{
	struct options options;
	const char *program = program_basename(argv[0]);
	const char *directory;
	char *allocated[2] = {NULL, NULL};
	int plural = !strcmp(program, "ngettext");
	int first;
	int operands;
	int result = 1;

	if (parse_options(argc, argv, plural, &options, &first) != 0)
		goto usage;
	operands = argc - first;
	if ((!plural && !options.join && operands != 1 && operands != 2) ||
	    (!plural && options.join && operands < 1) ||
	    (!plural && !options.join && !options.newline) ||
	    (plural && operands != 3 && operands != 4))
		goto usage;
	if (!options.join &&
	    ((!plural && operands == 2) || (plural && operands == 4)))
		options.domain = argv[first++];
	(void)setlocale(LC_ALL, "");
	directory = getenv("TEXTDOMAINDIR");
	if (options.domain == NULL)
		options.domain = getenv("TEXTDOMAIN");
	if (directory != NULL && options.domain != NULL)
		(void)bindtextdomain(options.domain, directory);
	if (!plural && options.join) {
		for (; first < argc; first++) {
			const char *message = argv[first];
			char *expanded = NULL;

			if (options.escape) {
				expanded = expand_escapes(message);
				if (expanded == NULL)
					goto failure;
				message = expanded;
			}
			message = translated(options.domain, message, NULL, 1);
			if ((first != argc - operands &&
			     write_message(" ") != 0) ||
			    write_message(message) != 0) {
				free(expanded);
				goto failure;
			}
			free(expanded);
		}
		if (options.newline && write_message("\n") != 0)
			goto failure;
		return 0;
	}
	if (options.escape) {
		allocated[0] = expand_escapes(argv[first]);
		allocated[1] = plural ? expand_escapes(argv[first + 1]) : NULL;
		if (allocated[0] == NULL || (plural && allocated[1] == NULL))
			goto failure;
	}
	if (plural) {
		char *end;
		unsigned long count;
		const char *singular =
		    allocated[0] != NULL ? allocated[0] : argv[first];
		const char *multiple =
		    allocated[1] != NULL ? allocated[1] : argv[first + 1];

		errno = 0;
		count = strtoul(argv[first + 2], &end, 10);
		if (errno != 0 || *argv[first + 2] == '\0' ||
		    argv[first + 2][0] == '-' || *end != '\0')
			goto usage_free;
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
	if (result != 0)
		command_error(program, NULL);
	free(allocated[0]);
	free(allocated[1]);
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
	return 2;
}
