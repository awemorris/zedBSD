/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/terminfo.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *
trim(char *text)
{
	char *end;

	while (isspace((unsigned char)*text))
		text++;
	end = text + strlen(text);
	while (end != text && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return text;
}

static int
terminal_name_valid(const char *name)
{
	if (name == NULL || *name == '\0')
		return 0;
	for (; *name != '\0'; name++)
		if (!isalnum((unsigned char)*name) && *name != '-' &&
		    *name != '_' && *name != '.' && *name != '+')
			return 0;
	return 1;
}

static int
hex_value(unsigned char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	return -1;
}

static int
decode_string(const char *source, char *result, size_t capacity)
{
	size_t used = 0;

	while (*source != '\0') {
		unsigned char value = (unsigned char)*source++;

		if (value == '^' && *source != '\0') {
			value = (unsigned char)*source++ & 0x1fU;
		} else if (value == '\\') {
			int first;

			if (*source == '\0')
				return 0;
			value = (unsigned char)*source++;
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
				if (first < 0 ||
				    hex_value((unsigned char)source[1]) < 0)
					return 0;
				value =
				    (unsigned char)(first * 16 +
						    hex_value((unsigned char)
								  source[1]));
				source += 2;
			} else if (value >= '0' && value <= '7') {
				unsigned number = value - '0';
				unsigned digits = 1;

				while (digits < 3U && *source >= '0' &&
				       *source <= '7') {
					number = number * 8U +
						 (unsigned)(*source++ - '0');
					digits++;
				}
				value = (unsigned char)number;
			}
		}
		if (value == '\0' || used + 1U >= capacity)
			return 0;
		result[used++] = (char)value;
	}
	result[used] = '\0';
	return 1;
}

const struct terminfo_capability *
terminfo_find(const struct terminfo *terminal, const char *name)
{
	size_t index;

	for (index = 0; index < terminal->count; index++)
		if (strcmp(terminal->capabilities[index].name, name) == 0)
			return &terminal->capabilities[index];
	return NULL;
}

int
terminfo_load(struct terminfo *terminal, const char *name,
	      const char *directory)
{
	char path[PATH_MAX + 1U];
	char line[1024];
	FILE *stream;
	unsigned long line_number = 0;
	int version = 0;

	memset(terminal, 0, sizeof(*terminal));
	if (!terminal_name_valid(name)) {
		errno = EINVAL;
		return -1;
	}
	if (directory == NULL || *directory == '\0')
		directory = "/lib/terminfo";
	{
		int length =
		    snprintf(path, sizeof(path), "%s/%s.zti", directory, name);

		if (length < 0 || (size_t)length >= sizeof(path)) {
			errno = ENAMETOOLONG;
			return -1;
		}
	}
	stream = fopen(path, "r");
	if (stream == NULL)
		return -1;
	while (fgets(line, sizeof(line), stream) != NULL) {
		char *text;
		char *equals;
		char *separator;
		struct terminfo_capability *capability;

		line_number++;
		if (strchr(line, '\n') == NULL && !feof(stream))
			goto invalid;
		text = trim(line);
		if (*text == '\0' || *text == '#')
			continue;
		if (!version) {
			if (strcmp(text, "ZEDTERM 1") != 0)
				goto invalid;
			version = 1;
			continue;
		}
		equals = strchr(text, '=');
		separator = strchr(text, ':');
		if (equals == NULL || separator == NULL || separator > equals)
			goto invalid;
		*separator++ = '\0';
		*equals++ = '\0';
		if (strcmp(text, "name") == 0) {
			if (strlen(equals) >= sizeof(terminal->name) ||
			    terminal->name[0] != '\0')
				goto invalid;
			(void)strcpy(terminal->name, equals);
			continue;
		}
		if (terminal->count == TERMINFO_CAPACITY ||
		    *separator == '\0' ||
		    strlen(separator) >= TERMINFO_NAME_LENGTH ||
		    terminfo_find(terminal, separator) != NULL)
			goto invalid;
		capability = &terminal->capabilities[terminal->count];
		(void)strcpy(capability->name, separator);
		if (strcmp(text, "bool") == 0) {
			capability->kind = TERMINFO_BOOLEAN;
			if (strcmp(equals, "0") != 0 &&
			    strcmp(equals, "1") != 0)
				goto invalid;
			capability->number = equals[0] - '0';
		} else if (strcmp(text, "num") == 0) {
			char *end;

			errno = 0;
			capability->kind = TERMINFO_NUMBER;
			capability->number = strtol(equals, &end, 10);
			if (errno != 0 || *equals == '\0' || *end != '\0' ||
			    capability->number < 0)
				goto invalid;
		} else if (strcmp(text, "str") == 0) {
			capability->kind = TERMINFO_STRING;
			if (!decode_string(equals, capability->string,
					   sizeof(capability->string)))
				goto invalid;
		} else
			goto invalid;
		terminal->count++;
	}
	if (ferror(stream) || !version || terminal->name[0] == '\0')
		goto invalid;
	return fclose(stream);

invalid:
	fprintf(stderr, "terminfo: %s:%lu: invalid description\n", path,
		line_number);
	(void)fclose(stream);
	errno = EINVAL;
	return -1;
}

static int
write_encoded(FILE *stream, const char *text)
{
	for (; *text != '\0'; text++) {
		unsigned char value = (unsigned char)*text;

		if (value == 0x1bU) {
			if (fputs("\\E", stream) == EOF)
				return -1;
		} else if (value == '\n') {
			if (fputs("\\n", stream) == EOF)
				return -1;
		} else if (value == '\r') {
			if (fputs("\\r", stream) == EOF)
				return -1;
		} else if (value == '\t') {
			if (fputs("\\t", stream) == EOF)
				return -1;
		} else if (value == '\b') {
			if (fputs("\\b", stream) == EOF)
				return -1;
		} else if (value == '\\' || value == ',' || value == '^') {
			if (fputc('\\', stream) == EOF ||
			    fputc((int)value, stream) == EOF)
				return -1;
		} else if (value < 0x20U || value == 0x7fU) {
			if (fprintf(stream, "\\%03o", (unsigned)value) < 0)
				return -1;
		} else if (fputc((int)value, stream) == EOF)
			return -1;
	}
	return 0;
}

int
terminfo_write_source(FILE *stream, const struct terminfo *terminal,
		      const char *name)
{
	size_t index;

	if (stream == NULL || terminal == NULL || !terminal_name_valid(name)) {
		errno = EINVAL;
		return -1;
	}
	if (fprintf(stream, "%s|%s,\n", name, terminal->name) < 0)
		return -1;
	for (index = 0; index < terminal->count; index++) {
		const struct terminfo_capability *capability =
		    &terminal->capabilities[index];

		if (capability->kind == TERMINFO_BOOLEAN && !capability->number)
			continue;
		if (fprintf(stream, "\t%s", capability->name) < 0)
			return -1;
		if (capability->kind == TERMINFO_NUMBER) {
			if (fprintf(stream, "#%ld", capability->number) < 0)
				return -1;
		} else if (capability->kind == TERMINFO_STRING) {
			if (fputc('=', stream) == EOF ||
			    write_encoded(stream, capability->string) != 0)
				return -1;
		}
		if (fputs(",\n", stream) == EOF)
			return -1;
	}
	return ferror(stream) ? -1 : 0;
}

static int
push(long stack[32], size_t *depth, long value)
{
	if (*depth == 32U) {
		errno = EOVERFLOW;
		return 0;
	}
	stack[(*depth)++] = value;
	return 1;
}

static int
pop(long stack[32], size_t *depth, long *value)
{
	if (*depth == 0) {
		errno = EINVAL;
		return 0;
	}
	*value = stack[--*depth];
	return 1;
}

static int
append_text(char *output, size_t capacity, size_t *used, const char *text)
{
	size_t length = strlen(text);

	if (length >= capacity - *used) {
		errno = EOVERFLOW;
		return 0;
	}
	memcpy(output + *used, text, length);
	*used += length;
	return 1;
}

static int
binary_operation(char operation, long left, long right, long *result)
{
	switch (operation) {
	case '+':
		if (__builtin_add_overflow(left, right, result))
			goto overflow;
		return 1;
	case '-':
		if (__builtin_sub_overflow(left, right, result))
			goto overflow;
		return 1;
	case '*':
		if (__builtin_mul_overflow(left, right, result))
			goto overflow;
		return 1;
	case '/':
	case 'm':
		if (right == 0) {
			errno = EINVAL;
			return 0;
		}
		if (left == LONG_MIN && right == -1)
			goto overflow;
		*result = operation == '/' ? left / right : left % right;
		return 1;
	case '&':
		*result = left & right;
		return 1;
	case '|':
		*result = left | right;
		return 1;
	case '^':
		*result = left ^ right;
		return 1;
	case '=':
		*result = left == right;
		return 1;
	case '>':
		*result = left > right;
		return 1;
	case '<':
		*result = left < right;
		return 1;
	case 'A':
		*result = left && right;
		return 1;
	case 'O':
		*result = left || right;
		return 1;
	default:
		errno = EINVAL;
		return 0;
	}

overflow:
	errno = EOVERFLOW;
	return 0;
}

int
terminfo_expand(const char *format, const long supplied[9], char *output,
		size_t capacity)
{
	struct conditional {
		int parent_active;
		int result;
		int saw_then;
	} conditionals[16];
	long parameters[9];
	long variables[26] = {0};
	long stack[32];
	size_t conditional_depth = 0;
	size_t depth = 0;
	size_t used = 0;
	int active = 1;

	if (format == NULL || output == NULL || capacity == 0) {
		errno = EINVAL;
		return -1;
	}
	errno = 0;
	memcpy(parameters, supplied, sizeof(parameters));
	while (*format != '\0') {
		long left;
		long right;
		char number[64];
		int length;

		if (*format != '%') {
			char literal[2] = {*format++, '\0'};

			if (active &&
			    !append_text(output, capacity, &used, literal))
				return -1;
			continue;
		}
		format++;
		if (*format == '\0')
			goto invalid;
		{
			char operation = *format++;

			if (operation == '?') {
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
			if (operation == 't') {
				struct conditional *conditional;

				if (conditional_depth == 0)
					goto invalid;
				conditional =
				    &conditionals[conditional_depth - 1U];
				if (conditional->saw_then)
					goto invalid;
				if (conditional->parent_active) {
					if (!pop(stack, &depth, &left))
						goto invalid;
					conditional->result = left != 0;
				}
				conditional->saw_then = 1;
				active = conditional->parent_active &&
					 conditional->result;
				continue;
			}
			if (operation == 'e') {
				struct conditional *conditional;

				if (conditional_depth == 0)
					goto invalid;
				conditional =
				    &conditionals[conditional_depth - 1U];
				if (!conditional->saw_then)
					goto invalid;
				active = conditional->parent_active &&
					 !conditional->result;
				continue;
			}
			if (operation == ';') {
				if (conditional_depth == 0 ||
				    !conditionals[conditional_depth - 1U]
					 .saw_then)
					goto invalid;
				active = conditionals[conditional_depth - 1U]
					     .parent_active;
				conditional_depth--;
				continue;
			}
			if (!active) {
				if (operation == 'p' || operation == 'P' ||
				    operation == 'g') {
					if (*format == '\0')
						goto invalid;
					format++;
				} else if (operation == '{') {
					const char *end = strchr(format, '}');

					if (end == NULL)
						goto invalid;
					format = end + 1;
				} else if ((operation == '2' ||
					    operation == '3') &&
					   *format == 'd')
					format++;
				continue;
			}
			switch (operation) {
			case '%':
				if (!append_text(output, capacity, &used, "%"))
					return -1;
				break;
			case 'i':
				if (__builtin_add_overflow(parameters[0], 1L,
							   &parameters[0]) ||
				    __builtin_add_overflow(parameters[1], 1L,
							   &parameters[1])) {
					errno = EOVERFLOW;
					goto invalid;
				}
				break;
			case 'p':
				if (*format < '1' || *format > '9' ||
				    !push(stack, &depth,
					  parameters[*format++ - '1']))
					goto invalid;
				break;
			case '{': {
				char *end;
				long value;

				errno = 0;
				value = strtol(format, &end, 10);
				if (errno != 0 || end == format ||
				    *end != '}' || !push(stack, &depth, value))
					goto invalid;
				format = end + 1;
				break;
			}
			case 'P':
				if (*format < 'a' || *format > 'z' ||
				    !pop(stack, &depth,
					 &variables[*format++ - 'a']))
					goto invalid;
				break;
			case 'g':
				if (*format < 'a' || *format > 'z' ||
				    !push(stack, &depth,
					  variables[*format++ - 'a']))
					goto invalid;
				break;
			case 'd':
				if (!pop(stack, &depth, &left))
					goto invalid;
				length = snprintf(number, sizeof(number), "%ld",
						  left);
				if (length < 0 ||
				    (size_t)length >= sizeof(number) ||
				    !append_text(output, capacity, &used,
						 number))
					return -1;
				break;
			case '2':
			case '3': {
				int width = format[-1] - '0';

				if (*format != 'd' ||
				    !pop(stack, &depth, &left))
					goto invalid;
				format++;
				length = snprintf(
				    number, sizeof(number),
				    width == 2 ? "%02ld" : "%03ld", left);
				if (length < 0 ||
				    (size_t)length >= sizeof(number) ||
				    !append_text(output, capacity, &used,
						 number))
					return -1;
				break;
			}
			case 'c':
				if (!pop(stack, &depth, &left) || left <= 0 ||
                                    left > (long)UCHAR_MAX ||
                                    used + 1U >= capacity)
					goto invalid;
				output[used++] = (char)left;
				break;
			case '!':
			case '~': {
				if (!pop(stack, &depth, &left) ||
				    !push(stack, &depth,
					  operation == '!' ? !left : ~left))
					goto invalid;
				break;
			}
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
			case 'O': {
				long value;

				if (!pop(stack, &depth, &right) ||
				    !pop(stack, &depth, &left))
					goto invalid;
				if (!binary_operation(operation, left, right,
						      &value))
					goto invalid;
				if (!push(stack, &depth, value))
					goto invalid;
				break;
			}
			default:
				goto invalid;
			}
		}
	}
	if (conditional_depth != 0)
		goto invalid;
	if (used > INT_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	output[used] = '\0';
	return (int)used;

invalid:
	if (errno == 0)
		errno = EINVAL;
	return -1;
}
