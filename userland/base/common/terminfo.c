/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland terminfo support.
 */

#include "userland/base/common/terminfo.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int terminal_name_valid(const char *name);
static char *trim(char *text);
static int decode_string(const char *source, char *result, size_t capacity);
static int hex_value(unsigned char character);
static int write_encoded(FILE *stream, const char *text);
static int append_text(char *output, size_t capacity, size_t *used, const char *text);
static int pop(long stack[32], size_t *depth, long *value);
static int push(long stack[32], size_t *depth, long value);
static int binary_operation(char operation, long left, long right, long *result);

/*
 * Implements the terminfo find operation.
 */
const struct terminfo_capability *
terminfo_find(
	const struct terminfo *terminal,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < terminal->count; index++) {
		/* Selects the matching value. */
		if (strcmp(terminal->capabilities[index].name, name) == 0)
			return &terminal->capabilities[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the terminfo load operation.
 */
int
terminfo_load(
	struct terminfo *terminal,
	const char *name,
	const char *directory)
{
	int function_result;
	int length;
	char *end;
	char *text;
	char *equals;
	char *separator;
	struct terminfo_capability *capability;
	char path[PATH_MAX + 1U];
	char line[1024];
	FILE *stream;
	unsigned long line_number;
	int version;

	line_number = 0;
	version = 0;

	memset(terminal, 0, sizeof(*terminal));

	/* Handles a failed terminal name valid operation. */
	if (!terminal_name_valid(name)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the directory availability. */
	if (directory == NULL || *directory == '\0')
		directory = "/lib/terminfo";

	length = snprintf(path, sizeof(path), "%s/%s.zti", directory, name);

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= sizeof(path)) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	stream = fopen(path, "r");

	/* Handles the stream availability. */
	if (stream == NULL)
		return -1;

	/* Process input until it is exhausted. */
	while (fgets(line, sizeof(line), stream) != NULL) {
		line_number++;

		/* Handles a failed strchr operation. */
		if (strchr(line, '\n') == NULL && !feof(stream))
			goto invalid;
		text = trim(line);

		/* Validates the current text. */
		if (*text == '\0' || *text == '#')
			continue;

		/* Handles the version condition. */
		if (!version) {
			/* Selects the matching value. */
			if (strcmp(text, "ZEDTERM 1") != 0)
				goto invalid;
			version = 1;
			continue;
		}
		equals = strchr(text, '=');
		separator = strchr(text, ':');

		/* Handles the equals availability. */
		if (equals == NULL || separator == NULL || separator > equals)
			goto invalid;
		*separator++ = '\0';
		*equals++ = '\0';
		/* Selects the matching value. */
		if (strcmp(text, "name") == 0) {
			/* Handles a failed strlen operation. */
			if (strlen(equals) >= sizeof(terminal->name) ||
			    terminal->name[0] != '\0')
				goto invalid;
			(void)strcpy(terminal->name, equals);
			continue;
		}

		/* Handles a failed strlen operation. */
		if (terminal->count == TERMINFO_CAPACITY ||
		    *separator == '\0' ||
		    strlen(separator) >= TERMINFO_NAME_LENGTH ||
		    terminfo_find(terminal, separator) != NULL)
			goto invalid;
		capability = &terminal->capabilities[terminal->count];
		(void)strcpy(capability->name, separator);

		/* Selects the matching value. */
		if (strcmp(text, "bool") == 0) {
			capability->kind = TERMINFO_BOOLEAN;

			/* Selects the matching value. */
			if (strcmp(equals, "0") != 0 &&
			    strcmp(equals, "1") != 0)
				goto invalid;
			capability->number = equals[0] - '0';
		} else if (strcmp(text, "num") == 0) {
			errno = 0;
			capability->kind = TERMINFO_NUMBER;
			capability->number = strtol(equals, &end, 10);

			/* Handles the reported system error. */
			if (errno != 0 || *equals == '\0' || *end != '\0' ||
			    capability->number < 0)
				goto invalid;
		} else if (strcmp(text, "str") == 0) {
			capability->kind = TERMINFO_STRING;

			/* Handles a failed decode string operation. */
			if (!decode_string(equals, capability->string,
					   sizeof(capability->string)))
				goto invalid;
		} else {
			goto invalid;
		}
		terminal->count++;
	}

	/* Handles an operation failure. */
	if (ferror(stream) || !version || terminal->name[0] == '\0')
		goto invalid;

	/* Obtains the fclose result. */
	function_result = fclose(stream);

	/* Returns the computed result. */
	return function_result;

invalid:
	fprintf(stderr, "terminfo: %s:%lu: invalid description\n", path,
		line_number);
	(void)fclose(stream);
	errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the terminfo write source operation.
 */
int
terminfo_write_source(
	FILE *stream,
	const struct terminfo *terminal,
	const char *name)
{
	int function_result;
	const struct terminfo_capability *capability;
	size_t index;

	/* Handles a failed terminal name valid operation. */
	if (stream == NULL || terminal == NULL || !terminal_name_valid(name)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed fprintf operation. */
	if (fprintf(stream, "%s|%s,\n", name, terminal->name) < 0)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < terminal->count; index++) {
		capability = &terminal->capabilities[index];

		/* Handles the capability condition. */
		if (capability->kind == TERMINFO_BOOLEAN && !capability->number)
			continue;

		/* Handles a failed fprintf operation. */
		if (fprintf(stream, "\t%s", capability->name) < 0)
			return -1;

		/* Handles the capability condition. */
		if (capability->kind == TERMINFO_NUMBER) {
			/* Handles a failed fprintf operation. */
			if (fprintf(stream, "#%ld", capability->number) < 0)
				return -1;
		} else if (capability->kind == TERMINFO_STRING) {
			/* Handles the end-of-file condition. */
			if (fputc('=', stream) == EOF ||
			    write_encoded(stream, capability->string) != 0)

				/* Reports operation failure. */
				return -1;
		}

		/* Handles the end-of-file condition. */
		if (fputs(",\n", stream) == EOF)
			return -1;
	}

	/* Computes the function result. */
	function_result = ferror(stream) ? -1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the terminfo expand operation.
 */
int
terminfo_expand(
	const char *format,
	const long supplied[9],
	char *output,
	size_t capacity)
{
	struct conditional *conditional_local;
	struct conditional *conditional_local1;
	const char *end_local;
	char *end_local2;
	long value_local;
	long value_local3;
	int width;
	char operation;
	long left;
	long right;
	char number[64];
	int length;

	struct conditional {
		int parent_active;
		int result;
		int saw_then;
	} conditionals[16];
	long parameters[9];
	long variables[26] = {0};
	long stack[32];
	char literal[2];
	size_t conditional_depth = 0;
	size_t depth = 0;
	size_t used = 0;
	int active = 1;

	/* Handles the format availability. */
	if (format == NULL || output == NULL || capacity == 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	errno = 0;
	memcpy(parameters, supplied, sizeof(parameters));

	/* Continue while the operation condition remains true. */
	while (*format != '\0') {
		/* Handles the format condition. */
		if (*format != '%') {
			literal[0] = *format++;
			literal[1] = '\0';

			/* Handles a failed append text operation. */
			if (active &&
			    !append_text(output, capacity, &used, literal))

				/* Reports operation failure. */
				return -1;
			continue;
		}
		format++;

		/* Handles the format condition. */
		if (*format == '\0')
			goto invalid;

		operation = *format++;

		/* Validates the selected operation. */
		if (operation == '?') {
			/* Handles the conditional depth condition. */
			if (conditional_depth ==
			    sizeof(conditionals) /
				sizeof(conditionals[0])) {
				errno = EOVERFLOW;
				goto invalid;
			}
			conditionals[conditional_depth].parent_active =
			    active;
			conditionals[conditional_depth].result = 0;
			conditionals[conditional_depth].saw_then = 0;
			conditional_depth++;
			continue;
		}

		/* Validates the selected operation. */
		if (operation == 't') {
			/* Handles the conditional depth condition. */
			if (conditional_depth == 0)
				goto invalid;
			conditional_local =
			    &conditionals[conditional_depth - 1U];

			/* Handles the conditional local condition. */
			if (conditional_local->saw_then)
				goto invalid;

			/* Handles the conditional local condition. */
			if (conditional_local->parent_active) {
				/* Handles a failed pop operation. */
				if (!pop(stack, &depth, &left))
					goto invalid;
				conditional_local->result = left != 0;
			}
			conditional_local->saw_then = 1;
			active = conditional_local->parent_active &&
				 conditional_local->result;
			continue;
		}

		/* Validates the selected operation. */
		if (operation == 'e') {
			/* Handles the conditional depth condition. */
			if (conditional_depth == 0)
				goto invalid;
			conditional_local1 =
			    &conditionals[conditional_depth - 1U];

			/* Handles the conditional local1 condition. */
			if (!conditional_local1->saw_then)
				goto invalid;
			active = conditional_local1->parent_active &&
				 !conditional_local1->result;
			continue;
		}

		/* Validates the selected operation. */
		if (operation == ';') {
			/* Handles the conditional depth condition. */
			if (conditional_depth == 0 ||
			    !conditionals[conditional_depth - 1U]
				 .saw_then)
				goto invalid;
			active = conditionals[conditional_depth - 1U]
				     .parent_active;
			conditional_depth--;
			continue;
		}

		/* Handles the active condition. */
		if (!active) {
			/* Validates the selected operation. */
			if (operation == 'p' || operation == 'P' ||
			    operation == 'g') {
				/* Handles the format condition. */
				if (*format == '\0')
					goto invalid;
				format++;
			} else if (operation == '{') {
				end_local = strchr(format, '}');

				/* Handles the end local availability. */
				if (end_local == NULL)
					goto invalid;
				format = end_local + 1;
			} else if ((operation == '2' ||
				    operation == '3') &&
				   *format == 'd')
				format++;
			continue;
		}

		/* Dispatch the selected operation case. */
		switch (operation) {
		case '%':
			/* Handles a failed append text operation. */
			if (!append_text(output, capacity, &used, "%"))
				return -1;
			break;
		case 'i':
			/* Handles the builtin add overflow condition. */
			if (__builtin_add_overflow(parameters[0], 1L,
						   &parameters[0]) ||
			    __builtin_add_overflow(parameters[1], 1L,
						   &parameters[1])) {
				errno = EOVERFLOW;
				goto invalid;
			}
			break;
		case 'p':
			/* Handles a failed push operation. */
			if (*format < '1' || *format > '9' ||
			    !push(stack, &depth,
				  parameters[*format++ - '1']))
				goto invalid;
			break;
		case '{':

		errno = 0;
		value_local = strtol(format, &end_local2, 10);

		/* Handles the reported system error. */
		if (errno != 0 || end_local2 == format ||
		    *end_local2 != '}' || !push(stack, &depth, value_local))
			goto invalid;
		format = end_local2 + 1;
		break;
		case 'P':
			/* Handles a failed pop operation. */
			if (*format < 'a' || *format > 'z' ||
			    !pop(stack, &depth,
				 &variables[*format++ - 'a']))
				goto invalid;
			break;
		case 'g':
			/* Handles a failed push operation. */
			if (*format < 'a' || *format > 'z' ||
			    !push(stack, &depth,
				  variables[*format++ - 'a']))
				goto invalid;
			break;
		case 'd':
			/* Handles a failed pop operation. */
			if (!pop(stack, &depth, &left))
				goto invalid;
			length = snprintf(number, sizeof(number), "%ld",
					  left);

			/* Handles a failed append text operation. */
			if (length < 0 ||
			    (size_t)length >= sizeof(number) ||
			    !append_text(output, capacity, &used,
					 number))

				/* Reports operation failure. */
				return -1;
			break;
		case '2':
		case '3':

		width = format[-1] - '0';

		/* Handles a failed pop operation. */
		if (*format != 'd' ||
		    !pop(stack, &depth, &left))
			goto invalid;
		format++;
		length = snprintf(
		    number, sizeof(number),
		    width == 2 ? "%02ld" : "%03ld", left);

		/* Handles a failed append text operation. */
		if (length < 0 ||
		    (size_t)length >= sizeof(number) ||
		    !append_text(output, capacity, &used,
				 number))

			/* Reports operation failure. */
			return -1;
		break;
		case 'c':
			/* Handles a failed pop operation. */
			if (!pop(stack, &depth, &left) || left <= 0 ||
                                    left > (long)UCHAR_MAX ||
                                    used + 1U >= capacity)
				goto invalid;
			output[used++] = (char)left;
			break;
		case '!':
		case '~':
		/* Handles a failed pop operation. */
		if (!pop(stack, &depth, &left) ||
		    !push(stack, &depth,
			  operation == '!' ? !left : ~left))
			goto invalid;
		break;
		case '+':
		case '-':
		case '*':
		case '/':
		case 'm':
		case '&':
		case '|':
		case '^':
		case '=':
		case '>':
		case '<':
		case 'A':
		case 'O':

		/* Handles a failed pop operation. */
		if (!pop(stack, &depth, &right) ||
		    !pop(stack, &depth, &left))
			goto invalid;

		/* Handles a failed binary operation operation. */
		if (!binary_operation(operation, left, right,
				      &value_local3))
			goto invalid;

		/* Handles a failed push operation. */
		if (!push(stack, &depth, value_local3))
			goto invalid;
		break;
		default:
			goto invalid;
		}
	}

	/* Handles the conditional depth condition. */
	if (conditional_depth != 0)
		goto invalid;

	/* Checks the current capacity usage. */
	if (used > INT_MAX) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	output[used] = '\0';

	/* Returns the computed result. */
	return (int)used;

invalid:

	/* Handles the reported system error. */
	if (errno == 0)
		errno = EINVAL;

	/* Reports operation failure. */
	return -1;
}

/* Supports the terminal name valid operation. */
static int
terminal_name_valid(
	const char *name)
{
	/* Handles the name availability. */
	if (name == NULL || *name == '\0')
		return 0;

	/* Process each element required by the operation. */
	for (; *name != '\0'; name++) {
		/* Handles a failed isalnum operation. */
		if (!isalnum((unsigned char)*name) && *name != '-' &&
		    *name != '_' && *name != '.' && *name != '+')
			/* Reports successful completion. */
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the trim operation. */
static char *
trim(
	char *text)
{
	char *end;

	/* Continue while the operation condition remains true. */
	while (isspace((unsigned char)*text))
		text++;

	/* Continue while the operation condition remains true. */
	end = text + strlen(text);
	while (end != text && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	/* Returns the computed result. */
	return text;
}

/* Supports the decode string operation. */
static int
decode_string(
	const char *source,
	char *result,
	size_t capacity)
{
	unsigned number;
	unsigned digits;
	int first;
	unsigned char value;
	size_t used;

	used = 0;

	/* Continue while the operation condition remains true. */
	while (*source != '\0') {
		value = (unsigned char)*source++;

		/* Validates the current value. */
		if (value == '^' && *source != '\0') {
			value = (unsigned char)*source++ & 0x1fU;
		} else if (value == '\\') {
			/* Handles the source condition. */
			if (*source == '\0')
				return 0;
			value = (unsigned char)*source++;

			/* Validates the current value. */
			if (value == 'E' || value == 'e')
				value = 0x1bU;
			else if (value == 'a')
				value = '\a';
			else if (value == 'n')
				value = '\n';
			else if (value == 'r')
				value = '\r';
			else if (value == 't')
				value = '\t';
			else if (value == 'b')
				value = '\b';
			else if (value == 'f')
				value = '\f';
			else if (value == 's')
				value = ' ';
			else if (value == 'x') {
				first = hex_value((unsigned char)source[0]);

				/* Handles a failed hex value operation. */
				if (first < 0 ||
				    hex_value((unsigned char)source[1]) < 0)

					/* Reports successful completion. */
					return 0;
				value =
				    (unsigned char)(first * 16 +
						    hex_value((unsigned char)
								  source[1]));
				source += 2;
			} else if (value >= '0' && value <= '7') {
				number = value - '0';
				digits = 1;

				/* Continue while the operation condition remains true. */
				while (digits < 3U && *source >= '0' &&
				       *source <= '7') {
					number = number * 8U +
						 (unsigned)(*source++ - '0');
					digits++;
				}
				value = (unsigned char)number;
			}
		}

		/* Validates the current value. */
		if (value == '\0' || used + 1U >= capacity)
			return 0;
		result[used++] = (char)value;
	}
	result[used] = '\0';

	/* Reports operation failure. */
	return 1;
}

/* Supports the hex value operation. */
static int
hex_value(
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

/* Supports the write encoded operation. */
static int
write_encoded(
	FILE *stream,
	const char *text)
{
	unsigned char value;

	/* Process each element required by the operation. */
	for (; *text != '\0'; text++) {
		value = (unsigned char)*text;

		/* Validates the current value. */
		if (value == 0x1bU) {
			/* Handles the end-of-file condition. */
			if (fputs("\\E", stream) == EOF)
				return -1;
		} else if (value == '\n') {
			/* Handles the end-of-file condition. */
			if (fputs("\\n", stream) == EOF)
				return -1;
		} else if (value == '\r') {
			/* Handles the end-of-file condition. */
			if (fputs("\\r", stream) == EOF)
				return -1;
		} else if (value == '\t') {
			/* Handles the end-of-file condition. */
			if (fputs("\\t", stream) == EOF)
				return -1;
		} else if (value == '\b') {
			/* Handles the end-of-file condition. */
			if (fputs("\\b", stream) == EOF)
				return -1;
		} else if (value == '\\' || value == ',' || value == '^') {
			/* Handles the end-of-file condition. */
			if (fputc('\\', stream) == EOF ||
			    fputc((int)value, stream) == EOF)

				/* Reports operation failure. */
				return -1;
		} else if (value < 0x20U || value == 0x7fU) {
			/* Handles a failed fprintf operation. */
			if (fprintf(stream, "\\%03o", (unsigned)value) < 0)
				return -1;
		} else if (fputc((int)value, stream) == EOF)

			/* Reports operation failure. */
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the append text operation. */
static int
append_text(
	char *output,
	size_t capacity,
	size_t *used,
	const char *text)
{
	size_t length;

	length = strlen(text);

	/* Checks the current data length. */
	if (length >= capacity - *used) {
		errno = EOVERFLOW;

		/* Reports successful completion. */
		return 0;
	}
	memcpy(output + *used, text, length);
	*used += length;
	/* Reports operation failure. */
	return 1;
}

/* Supports the pop operation. */
static int
pop(
	long stack[32],
	size_t *depth,
	long *value)
{
	/* Handles the depth condition. */
	if (*depth == 0) {
		errno = EINVAL;

		/* Reports successful completion. */
		return 0;
	}
	*value = stack[--*depth];
	/* Reports operation failure. */
	return 1;
}

/* Supports the push operation. */
static int
push(
	long stack[32],
	size_t *depth,
	long value)
{
	/* Handles the depth condition. */
	if (*depth == 32U) {
		errno = EOVERFLOW;

		/* Reports successful completion. */
		return 0;
	}
	stack[(*depth)++] = value;

	/* Reports operation failure. */
	return 1;
}

/* Supports the binary operation operation. */
static int
binary_operation(
	char operation,
	long left,
	long right,
	long *result)
{
	/* Dispatch the selected operation case. */
	switch (operation) {
	case '+':
		/* Handles the builtin add overflow condition. */
		if (__builtin_add_overflow(left, right, result))
			goto overflow;

		/* Reports operation failure. */
		return 1;
	case '-':
		/* Handles the builtin sub overflow condition. */
		if (__builtin_sub_overflow(left, right, result))
			goto overflow;

		/* Reports operation failure. */
		return 1;
	case '*':
		/* Handles the builtin mul overflow condition. */
		if (__builtin_mul_overflow(left, right, result))
			goto overflow;

		/* Reports operation failure. */
		return 1;
	case '/':
	case 'm':
		/* Handles the right condition. */
		if (right == 0) {
			errno = EINVAL;

			/* Reports successful completion. */
			return 0;
		}

		/* Handles the left condition. */
		if (left == LONG_MIN && right == -1)
			goto overflow;
		*result = operation == '/' ? left / right : left % right;
		/* Reports operation failure. */
		return 1;
	case '&':
		*result = left & right;
		/* Reports operation failure. */
		return 1;
	case '|':
		*result = left | right;
		/* Reports operation failure. */
		return 1;
	case '^':
		*result = left ^ right;
		/* Reports operation failure. */
		return 1;
	case '=':
		*result = left == right;
		/* Reports operation failure. */
		return 1;
	case '>':
		*result = left > right;
		/* Reports operation failure. */
		return 1;
	case '<':
		*result = left < right;
		/* Reports operation failure. */
		return 1;
	case 'A':
		*result = left && right;
		/* Reports operation failure. */
		return 1;
	case 'O':
		*result = left || right;
		/* Reports operation failure. */
		return 1;
	default:
		errno = EINVAL;

		/* Reports successful completion. */
		return 0;
	}

overflow:
	errno = EOVERFLOW;

	/* Reports successful completion. */
	return 0;
}
