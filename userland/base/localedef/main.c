/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD localedef userland command.
 */

#include "userland/base/common/command.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libc/include/zedbsd/locale-format.h"

struct key_metadata {
	int category;
	const char *keyword;
	const char *c_value;
	const char *utf8_value;
};

static const struct key_metadata metadata[ZEDBSD_LOCALE_KEY_COUNT] = {
    [ZEDBSD_LOCALE_KEY_INVALID] = {-1, "", "", ""},
#define LOCALE_METADATA(name, category, keyword, c_value, utf8_value)          \
	[ZEDBSD_LOCALE_KEY_##name] = {category, keyword, c_value, utf8_value},
    ZEDBSD_LOCALE_KEYS(LOCALE_METADATA)
#undef LOCALE_METADATA
};

static const char *const category_names[] = {
    "LC_CTYPE",	  "LC_NUMERIC",	 "LC_TIME",
    "LC_COLLATE", "LC_MONETARY", "LC_MESSAGES",
};

struct locale_source {
	char *values[ZEDBSD_LOCALE_KEY_COUNT];
	unsigned seen_keys[ZEDBSD_LOCALE_KEY_COUNT];
	unsigned seen_categories[6];
	int utf8;
	int force;
	int warnings;
};

static int parse_charmap(const char *path, int *utf8);
static char *trim(char *text);
static int source_defaults(struct locale_source *source, int utf8);
static int parse_source(struct locale_source *source, FILE *stream, const char *path);
static void strip_comment(char *line, int comment);
static int category_index(const char *name);
static char *decode_string(const char *input, int utf8);
static int hex_value(unsigned char character);
static int append_utf8(char **output, uint32_t value);
static int copy_builtin(struct locale_source *source, int category, const char *name);
static int source_set(struct locale_source *source, enum zedbsd_locale_key key, const char *value, int duplicate_error);
static int parse_group(struct locale_source *source, enum zedbsd_locale_key first, size_t expected, char *text);
static int split_values(char *text, char **values, size_t maximum);
static int parse_grouping(struct locale_source *source, enum zedbsd_locale_key key, char *text);
static enum zedbsd_locale_key key_find(const char *keyword, int category);
static int encode_source(struct locale_source *source, unsigned char **result, size_t *result_size);
static int write_atomic(const char *path, const unsigned char *data, size_t size);
static void source_free(struct locale_source *source);

/*
 * Runs the localedef command.
 */
int
main(
	int argc,
	char **argv)
{
	struct locale_source source = {0};
	const char *charmap;
	const char *input;
	const char *output;
	unsigned char *encoded;
	size_t encoded_size;
	FILE *stream;
	int utf8;
	int index;
	int result;

	charmap = NULL;
	input = NULL;
	encoded = NULL;
	encoded_size = 0;
	utf8 = 0;
	index = 1;
	result = 1;

	/* Process each remaining command-line operand. */
	while (index < argc && argv[index][0] == '-' &&
	       argv[index][1] != '\0') {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		}

		/* Handles the selected command-line operation. */
		if (strcmp(argv[index], "-c") == 0)
			source.force = 1;
		else if (strcmp(argv[index], "-v") == 0)
			;
		else if (strcmp(argv[index], "-f") == 0 && ++index < argc)
			charmap = argv[index];
		else if (strcmp(argv[index], "-i") == 0 && ++index < argc)
			input = argv[index];
		else
			goto usage;
		index++;
	}

	/* Validates the command-line arguments. */
	if (index + 1 != argc)
		goto usage;
	output = argv[index];

	/* Handles a failed parse charmap operation. */
	if (charmap != NULL && parse_charmap(charmap, &utf8) != 0) {
		fprintf(stderr, "localedef: %s: %s\n", charmap,
			strerror(errno));
		goto done;
	}

	/* Handles a failed source defaults operation. */
	if (source_defaults(&source, utf8) != 0)
		goto done;
	stream = input == NULL || strcmp(input, "-") == 0 ? stdin
							  : fopen(input, "r");

	/* Handles the stream availability. */
	if (stream == NULL) {
		fprintf(stderr, "localedef: %s: %s\n", input, strerror(errno));
		goto done;
	}

	/* Handles a failed parse source operation. */
	if (parse_source(&source, stream,
			 input != NULL ? input : "standard input") != 0) {
		/* Handles the stream condition. */
		if (stream != stdin)
			(void)fclose(stream);
		goto done;
	}

	/* Handles a failed fclose operation. */
	if (stream != stdin && fclose(stream) != 0)
		goto done;

	/* Handles a failed encode source operation. */
	if (encode_source(&source, &encoded, &encoded_size) != 0 ||
	    write_atomic(output, encoded, encoded_size) != 0) {
		fprintf(stderr, "localedef: %s: %s\n", output, strerror(errno));
		goto done;
	}
	result = 0;
done:
	free(encoded);
	source_free(&source);

	/* Returns the computed result. */
	return result;

usage:
	fprintf(
	    stderr,
	    "usage: localedef [-c] [-v] [-f charmap] [-i sourcefile] name\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the parse charmap operation. */
static int
parse_charmap(
	const char *path,
	int *utf8)
{
	char *text;
	FILE *stream;
	char line[4096];
	int found;

	stream = fopen(path, "r");
	found = 0;

	/* Handles the stream availability. */
	if (stream == NULL)
		return -1;

	/* Process input until it is exhausted. */
	while (fgets(line, sizeof(line), stream) != NULL) {

		text = trim(line);

		/* Selects the matching prefix. */
		if (strncmp(text, "<code_set_name>", 15) != 0)
			continue;
		text = trim(text + 15);

		/* Validates the current text. */
		if (text[0] == '"') {
			text++;

			/* Handles a failed strchr operation. */
			if (strchr(text, '"') != NULL)
				*strchr(text, '"') = '\0';
		}

		/* Selects the matching value. */
		if (strcmp(text, "UTF-8") == 0 || strcmp(text, "UTF8") == 0)
			*utf8 = 1;
		else if (strcmp(text, "US-ASCII") == 0 ||
			 strcmp(text, "ASCII") == 0)
			*utf8 = 0;
		else {
			fclose(stream);
			errno = ENOTSUP;

			/* Reports operation failure. */
			return -1;
		}
		found = 1;
		break;
	}

	/* Handles a failed fclose operation. */
	if (fclose(stream) != 0)
		return -1;

	/* Handles the found condition. */
	if (!found) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
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

/* Supports the source defaults operation. */
static int
source_defaults(
	struct locale_source *source,
	int utf8)
{
	const char *value;
	unsigned key;

	/* Process each remaining element. */
	source->utf8 = utf8;
	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++) {
				value = utf8 ? metadata[key].utf8_value : metadata[key].c_value;

		source->values[key] = strdup(value);

		/* Handles the source condition. */
		if (source->values[key] == NULL)
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the parse source operation. */
static int
parse_source(
	struct locale_source *source,
	FILE *stream,
	const char *path)
{
	char *decoded_local;
	char *decoded_local1;
	enum zedbsd_locale_key key;
	char *keyword;
	char *value;
	char *space;
	int category;
	char line[8192];
	unsigned long line_number;
	int current;
	int comment;
	int failed;

	line_number = 0;
	current = -1;
	comment = '#';
	failed = 0;

	/* Process input until it is exhausted. */
	while (fgets(line, sizeof(line), stream) != NULL) {

		line_number++;

		/* Handles a failed strchr operation. */
		if (strchr(line, '\n') == NULL && !feof(stream)) {
			fprintf(stderr, "localedef: %s:%lu: line is too long\n",
				path, line_number);

			/* Reports operation failure. */
			return -1;
		}
		strip_comment(line, comment);
		keyword = trim(line);

		/* Handles the keyword condition. */
		if (*keyword == '\0')
			continue;

		/* Continue while the operation condition remains true. */
		space = keyword;
		while (*space != '\0' && !isspace((unsigned char)*space))
			space++;

		/* Handles the space condition. */
		if (*space != '\0')
			*space++ = '\0';
		value = trim(space);

		/* Handles the current condition. */
		if (current < 0 && strcmp(keyword, "comment_char") == 0) {
			/* Validates the current value. */
			if (value[0] == '\0' || value[1] != '\0')
				goto invalid;
			comment = (unsigned char)value[0];
			continue;
		}

		/* Handles a failed category index operation. */
		if ((category = category_index(keyword)) >= 0) {
			/* Handles the current condition. */
			if (current >= 0 || *value != '\0' ||
			    source->seen_categories[category])
				goto invalid;
			current = category;
			source->seen_categories[category] = 1;
			continue;
		}

		/* Selects the matching value. */
		if (strcmp(keyword, "END") == 0) {
			/* Handles the current condition. */
			if (current < 0 ||
			    strcmp(value, category_names[current]) != 0)
				goto invalid;
			current = -1;
			continue;
		}

		/* Handles the current condition. */
		if (current < 0)
			goto invalid;

		/* Selects the matching value. */
		if (strcmp(keyword, "copy") == 0) {
						decoded_local = decode_string(value, 1);

			/* Handles a failed copy builtin operation. */
			if (decoded_local == NULL ||
			    copy_builtin(source, current, decoded_local) != 0) {
				free(decoded_local);
				goto invalid;
			}
			free(decoded_local);
			continue;
		}

		/* Handles the current condition. */
		if (current == LC_TIME && strcmp(keyword, "abday") == 0) {
			/* Handles a failed parse group operation. */
			if (parse_group(source, ZEDBSD_LOCALE_KEY_ABDAY_1, 7U,
					value) != 0)
				goto invalid;
			continue;
		}

		/* Handles the current condition. */
		if (current == LC_TIME && strcmp(keyword, "day") == 0) {
			/* Handles a failed parse group operation. */
			if (parse_group(source, ZEDBSD_LOCALE_KEY_DAY_1, 7U,
					value) != 0)
				goto invalid;
			continue;
		}

		/* Handles the current condition. */
		if (current == LC_TIME && strcmp(keyword, "abmon") == 0) {
			/* Handles a failed parse group operation. */
			if (parse_group(source, ZEDBSD_LOCALE_KEY_ABMON_1, 12U,
					value) != 0)
				goto invalid;
			continue;
		}

		/* Handles the current condition. */
		if (current == LC_TIME && strcmp(keyword, "mon") == 0) {
			/* Handles a failed parse group operation. */
			if (parse_group(source, ZEDBSD_LOCALE_KEY_MON_1, 12U,
					value) != 0)
				goto invalid;
			continue;
		}

		/* Handles the current condition. */
		if (current == LC_TIME && strcmp(keyword, "am_pm") == 0) {
			/* Handles a failed parse group operation. */
			if (parse_group(source, ZEDBSD_LOCALE_KEY_AM_STR, 2U,
					value) != 0)
				goto invalid;
			continue;
		}

		/* Handles the current condition. */
		if ((current == LC_NUMERIC &&
		     strcmp(keyword, "grouping") == 0) ||
		    (current == LC_MONETARY &&
		     strcmp(keyword, "mon_grouping") == 0)) {
			/* Handles a failed parse grouping operation. */
			if (parse_grouping(source,
					   current == LC_NUMERIC
					       ? ZEDBSD_LOCALE_KEY_GROUPING
					       : ZEDBSD_LOCALE_KEY_MON_GROUPING,
					   value) != 0)
				goto invalid;
			continue;
		}

		/* Handles the current condition. */
		if (current == LC_COLLATE &&
		    strcmp(keyword, "order_end") == 0 && *value == '\0')
			continue;

		key = key_find(keyword, current);

		/* Handles the selected key. */
		if (key == ZEDBSD_LOCALE_KEY_INVALID)
			goto invalid;
		decoded_local1 = decode_string(value, source->utf8);

		/* Handles a failed source set operation. */
		if (decoded_local1 == NULL ||
		    source_set(source, key, decoded_local1, 1) != 0) {
			free(decoded_local1);
			goto invalid;
		}
		free(decoded_local1);
		continue;

	invalid:
		fprintf(stderr,
			"localedef: %s:%lu: invalid or unsupported %s\n", path,
			line_number, keyword);
		failed = 1;
	}

	/* Handles the current condition. */
	if (current >= 0) {
		fprintf(stderr, "localedef: %s: missing END %s\n", path,
			category_names[current]);
		failed = 1;
	}

	/* Handles an operation failure. */
	if (ferror(stream))
		failed = 1;

	/* Returns the computed result. */
	return failed ? -1 : 0;
}

/* Supports the strip comment operation. */
static void
strip_comment(
	char *line,
	int comment)
{
	int quoted;
	int escaped;
	char *cursor;

	quoted = 0;
	escaped = 0;

	/* Process each element required by the operation. */
	for (cursor = line; *cursor != '\0'; cursor++) {
		/* Handles the escaped condition. */
		if (escaped) {
			escaped = 0;
			continue;
		}

		/* Checks the current cursor position. */
		if (*cursor == '\\' || *cursor == '/') {
			escaped = 1;
			continue;
		}

		/* Checks the current cursor position. */
		if (*cursor == '"')
			quoted = !quoted;
		else if (!quoted &&
			 (unsigned char)*cursor == (unsigned char)comment) {
			*cursor = '\0';
			break;
		}
	}
}

/* Supports the category index operation. */
static int
category_index(
	const char *name)
{
	int category;

	/* Process each element required by the operation. */
	for (category = 0; category < 6; category++)

		/* Selects the matching value. */
		if (strcmp(name, category_names[category]) == 0)
			return category;

	/* Reports operation failure. */
	return -1;
}

/* Supports the decode string operation. */
static char *
decode_string(
	const char *input,
	int utf8)
{
	int digit;
	const char *cursor;
	uint32_t value;
	unsigned digits;
	int escape;
	size_t length;
	char *result;
	char *output;
	const char *end;
	int quoted;

	length = strlen(input);
	result = malloc(length * 4U + 1U);
	output = result;
	end = input + length;
	quoted = length >= 2U && input[0] == '"' && end[-1] == '"';

	/* Handles the result availability. */
	if (result == NULL)
		return NULL;

	/* Handles the quoted condition. */
	if (quoted) {
		input++;
		end--;
	}
	while (input < end) {
		/* Validates the current input. */
		if (*input == '<' && end - input >= 4 && input[1] == 'U') {
						cursor = input + 2;
						value = 0;
						digits = 0;

			/* Continue while the operation condition remains true. */
			while (cursor < end && *cursor != '>') {

				digit = hex_value((unsigned char)*cursor++);

				/* Handles the digit condition. */
				if (digit < 0 || digits++ >= 8U ||
				    value >
					(UINT32_MAX - (unsigned)digit) / 16U)
					goto invalid;
				value = value * 16U + (unsigned)digit;
			}

			/* Handles a failed append utf8 operation. */
			if (cursor == end || digits < 4U ||
			    (!utf8 && value > 0x7fU) ||
			    !append_utf8(&output, value))
				goto invalid;
			input = cursor + 1;
			continue;
		}

		/* Validates the current input. */
		if ((*input == '\\' || *input == '/') && input + 1 < end) {

			input++;
			escape = (unsigned char)*input++;

			/* Dispatch the selected operation case. */
			switch (escape) {
			case 'n':
				*output++ = '\n';
				break;
			case 't':
				*output++ = '\t';
				break;
			case 'r':
				*output++ = '\r';
				break;
			case 'b':
				*output++ = '\b';
				break;
			case 'f':
				*output++ = '\f';
				break;
			case 'v':
				*output++ = '\v';
				break;
			default:
				*output++ = (char)escape;
				break;
			}
			continue;
		}

		/* Handles the utf8 condition. */
		if (!utf8 && (unsigned char)*input > 0x7fU)
			goto invalid;
		*output++ = *input++;
	}
	*output = '\0';
	/* Returns the computed result. */
	return result;

invalid:
	free(result);
	errno = EINVAL;

	/* Reports that no result is available. */
	return NULL;
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

/* Supports the append utf8 operation. */
static int
append_utf8(
	char **output,
	uint32_t value)
{
	/* Validates the current value. */
	if (value == 0 || value > 0x10ffffU ||
	    (value >= 0xd800U && value <= 0xdfffU))

		/* Reports successful completion. */
		return 0;

	/* Validates the current value. */
	if (value < 0x80U)
		*(*output)++ = (char)value;
	else if (value < 0x800U) {
		*(*output)++ = (char)(0xc0U | (value >> 6));
		*(*output)++ = (char)(0x80U | (value & 0x3fU));
	} else if (value < 0x10000U) {
		*(*output)++ = (char)(0xe0U | (value >> 12));
		*(*output)++ = (char)(0x80U | ((value >> 6) & 0x3fU));
		*(*output)++ = (char)(0x80U | (value & 0x3fU));
	} else {
		*(*output)++ = (char)(0xf0U | (value >> 18));
		*(*output)++ = (char)(0x80U | ((value >> 12) & 0x3fU));
		*(*output)++ = (char)(0x80U | ((value >> 6) & 0x3fU));
		*(*output)++ = (char)(0x80U | (value & 0x3fU));
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the copy builtin operation. */
static int
copy_builtin(
	struct locale_source *source,
	int category,
	const char *name)
{
	int utf8;
	unsigned key;

	/* Selects the matching value. */
	if (strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0)
		utf8 = 0;
	else if (strcmp(name, "C.UTF-8") == 0 || strcmp(name, "C.utf8") == 0)
		utf8 = 1;
	else {
		errno = ENOENT;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++)

		/* Handles a failed source set operation. */
		if (metadata[key].category == category &&
		    source_set(source, (enum zedbsd_locale_key)key,
			       utf8 ? metadata[key].utf8_value
				    : metadata[key].c_value,
			       0) != 0)

			/* Reports operation failure. */
			return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the source set operation. */
static int
source_set(
	struct locale_source *source,
	enum zedbsd_locale_key key,
	const char *value,
	int duplicate_error)
{
	char *copy;

	/* Handles an operation failure. */
	if (duplicate_error && source->seen_keys[key]) {
		errno = EEXIST;

		/* Reports operation failure. */
		return -1;
	}
	copy = strdup(value);

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;
	free(source->values[key]);
	source->values[key] = copy;
	source->seen_keys[key] = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the parse group operation. */
static int
parse_group(
	struct locale_source *source,
	enum zedbsd_locale_key first,
	size_t expected,
	char *text)
{
	char *decoded;
	char *parts[16];
	int count;
	int index;

	count = split_values(text, parts, sizeof(parts) / sizeof(parts[0]));

	/* Checks the remaining item count. */
	if (count != (int)expected)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
				decoded = decode_string(parts[index], source->utf8);

		/* Handles a failed source set operation. */
		if (decoded == NULL ||
		    source_set(source, first + index, decoded, 1) != 0) {
			free(decoded);

			/* Reports operation failure. */
			return -1;
		}
		free(decoded);
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the split values operation. */
static int
split_values(
	char *text,
	char **values,
	size_t maximum)
{
	int terminator;
	size_t count;
	char *begin;
	char *cursor;
	int quoted;
	int escaped;

	count = 0;
	begin = text;
	quoted = 0;
	escaped = 0;

	/* Process each element required by the operation. */
	for (cursor = text;; cursor++) {
		/* Handles the escaped condition. */
		if (escaped)
			escaped = 0;
		else if (*cursor == '\\' || *cursor == '/')
			escaped = 1;
		else if (*cursor == '"')
			quoted = !quoted;
		else if ((!quoted && *cursor == ';') || *cursor == '\0') {
			terminator = (unsigned char)*cursor;

			/* Checks the remaining item count. */
			if (count == maximum)
				return -1;

			/* Handles the terminator condition. */
			if (terminator != '\0')
				*cursor = '\0';
			values[count++] = trim(begin);

			/* Handles the terminator condition. */
			if (terminator == '\0')
				break;
			begin = cursor + 1;
		}
	}

	/* Returns the computed result. */
	return quoted ? -1 : (int)count;
}

/* Supports the parse grouping operation. */
static int
parse_grouping(
	struct locale_source *source,
	enum zedbsd_locale_key key,
	char *text)
{
	int function_result;
	char *end;
	long value;
	char *parts[32];
	char grouping[33];
	int count;
	int index;

	count = split_values(text, parts, 32U);

	/* Checks the remaining item count. */
	if (count <= 0)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {

		errno = 0;
		value = strtol(parts[index], &end, 10);

		/* Handles the reported system error. */
		if (errno != 0 || *trim(end) != '\0' || value <= 0 ||
		    value > CHAR_MAX)

			/* Reports operation failure. */
			return -1;
		grouping[index] = (char)value;
	}
	grouping[count] = '\0';

	/* Obtains the source set result. */
	function_result = source_set(source, key, grouping, 1);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the key find operation. */
static enum zedbsd_locale_key
key_find(
	const char *keyword,
	int category)
{
	unsigned key;

	/* Process each remaining element. */
	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++)

		/* Handles the metadata condition. */
		if (metadata[key].category == category &&
		    strcmp(metadata[key].keyword, keyword) == 0)

			/* Returns the computed result. */
			return (enum zedbsd_locale_key)key;

	/* Returns the computed result. */
	return ZEDBSD_LOCALE_KEY_INVALID;
}

/* Supports the encode source operation. */
static int
encode_source(
	struct locale_source *source,
	unsigned char **result,
	size_t *result_size)
{
	size_t length;
	unsigned char *entry;
	size_t strings;
	size_t total;
	size_t offset;
	unsigned key;
	unsigned char *data;

	strings = 0;

	/* Process each remaining element. */
	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++) {
				length = strlen(source->values[key]) + 1U;

		/* Checks the current data length. */
		if (length > UINT32_MAX - strings) {
			errno = EFBIG;

			/* Reports operation failure. */
			return -1;
		}
		strings += length;
	}
	total = ZEDBSD_LOCALE_HEADER_SIZE +
		(ZEDBSD_LOCALE_KEY_COUNT - 1U) * ZEDBSD_LOCALE_ENTRY_SIZE +
		strings;

	/* Handles the total condition. */
	if (total > UINT32_MAX) {
		errno = EFBIG;

		/* Reports operation failure. */
		return -1;
	}
	data = calloc(1, total);

	/* Handles the data availability. */
	if (data == NULL)
		return -1;
	memcpy(data, ZEDBSD_LOCALE_MAGIC, ZEDBSD_LOCALE_MAGIC_SIZE);
	zedbsd_locale_put32(data + 8U, ZEDBSD_LOCALE_VERSION);
	zedbsd_locale_put32(data + 12U, ZEDBSD_LOCALE_HEADER_SIZE);
	zedbsd_locale_put32(data + 16U, ZEDBSD_LOCALE_KEY_COUNT - 1U);
	zedbsd_locale_put32(data + 20U, ZEDBSD_LOCALE_HEADER_SIZE);
	offset = ZEDBSD_LOCALE_HEADER_SIZE +
		 (ZEDBSD_LOCALE_KEY_COUNT - 1U) * ZEDBSD_LOCALE_ENTRY_SIZE;
	zedbsd_locale_put32(data + 24U, (uint32_t)offset);

	/* Process each remaining element. */
	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++) {
				entry = data + ZEDBSD_LOCALE_HEADER_SIZE +
				       (key - 1U) * ZEDBSD_LOCALE_ENTRY_SIZE;
		size_t length = strlen(source->values[key]);

		zedbsd_locale_put32(entry, key);
		zedbsd_locale_put32(entry + 4U, metadata[key].category);
		zedbsd_locale_put32(entry + 8U, (uint32_t)offset);
		zedbsd_locale_put32(entry + 12U, (uint32_t)length);
		memcpy(data + offset, source->values[key], length + 1U);
		offset += length + 1U;
	}
	*result = data;
	*result_size = total;
	/* Reports successful completion. */
	return 0;
}

/* Supports the write atomic operation. */
static int
write_atomic(
	const char *path,
	const unsigned char *data,
	size_t size)
{
	char temporary[PATH_MAX + 1U];
	int descriptor;
	int length;
	int error;

	error = 0;

	length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
			  (long)getpid());

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= sizeof(temporary)) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	descriptor =
	    open(temporary,
		 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles a failed command write all operation. */
	if (command_write_all(descriptor, data, size) != 0 ||
	    fsync(descriptor) != 0)
		error = errno;

	/* Handles an operation failure. */
	if (close(descriptor) != 0 && error == 0)
		error = errno;

	/* Handles an operation failure. */
	if (error == 0 && rename(temporary, path) == 0)
		return 0;

	/* Handles an operation failure. */
	if (error == 0)
		error = errno;
	(void)unlink(temporary);
	errno = error;

	/* Reports operation failure. */
	return -1;
}

/* Supports the source free operation. */
static void
source_free(
	struct locale_source *source)
{
	unsigned key;

	/* Process each remaining element. */
	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++)
		free(source->values[key]);
}
