/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static void
source_free(struct locale_source *source)
{
	unsigned key;

	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++)
		free(source->values[key]);
}

static int
source_defaults(struct locale_source *source, int utf8)
{
	unsigned key;

	source->utf8 = utf8;
	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++) {
		const char *value =
		    utf8 ? metadata[key].utf8_value : metadata[key].c_value;

		source->values[key] = strdup(value);
		if (source->values[key] == NULL)
			return -1;
	}
	return 0;
}

static int
source_set(struct locale_source *source, enum zedbsd_locale_key key,
	   const char *value, int duplicate_error)
{
	char *copy;

	if (duplicate_error && source->seen_keys[key]) {
		errno = EEXIST;
		return -1;
	}
	copy = strdup(value);
	if (copy == NULL)
		return -1;
	free(source->values[key]);
	source->values[key] = copy;
	source->seen_keys[key] = 1;
	return 0;
}

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

static void
strip_comment(char *line, int comment)
{
	int quoted = 0;
	int escaped = 0;
	char *cursor;

	for (cursor = line; *cursor != '\0'; cursor++) {
		if (escaped) {
			escaped = 0;
			continue;
		}
		if (*cursor == '\\' || *cursor == '/') {
			escaped = 1;
			continue;
		}
		if (*cursor == '"')
			quoted = !quoted;
		else if (!quoted &&
			 (unsigned char)*cursor == (unsigned char)comment) {
			*cursor = '\0';
			break;
		}
	}
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
append_utf8(char **output, uint32_t value)
{
	if (value == 0 || value > 0x10ffffU ||
	    (value >= 0xd800U && value <= 0xdfffU))
		return 0;
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
	return 1;
}

static char *
decode_string(const char *input, int utf8)
{
	size_t length = strlen(input);
	char *result = malloc(length * 4U + 1U);
	char *output = result;
	const char *end = input + length;
	int quoted = length >= 2U && input[0] == '"' && end[-1] == '"';

	if (result == NULL)
		return NULL;
	if (quoted) {
		input++;
		end--;
	}
	while (input < end) {
		if (*input == '<' && end - input >= 4 && input[1] == 'U') {
			const char *cursor = input + 2;
			uint32_t value = 0;
			unsigned digits = 0;

			while (cursor < end && *cursor != '>') {
				int digit = hex_value((unsigned char)*cursor++);

				if (digit < 0 || digits++ >= 8U ||
				    value >
					(UINT32_MAX - (unsigned)digit) / 16U)
					goto invalid;
				value = value * 16U + (unsigned)digit;
			}
			if (cursor == end || digits < 4U ||
			    (!utf8 && value > 0x7fU) ||
			    !append_utf8(&output, value))
				goto invalid;
			input = cursor + 1;
			continue;
		}
		if ((*input == '\\' || *input == '/') && input + 1 < end) {
			int escape;

			input++;
			escape = (unsigned char)*input++;

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
		if (!utf8 && (unsigned char)*input > 0x7fU)
			goto invalid;
		*output++ = *input++;
	}
	*output = '\0';
	return result;

invalid:
	free(result);
	errno = EINVAL;
	return NULL;
}

static enum zedbsd_locale_key
key_find(const char *keyword, int category)
{
	unsigned key;

	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++)
		if (metadata[key].category == category &&
		    strcmp(metadata[key].keyword, keyword) == 0)
			return (enum zedbsd_locale_key)key;
	return ZEDBSD_LOCALE_KEY_INVALID;
}

static int
split_values(char *text, char **values, size_t maximum)
{
	size_t count = 0;
	char *begin = text;
	char *cursor;
	int quoted = 0;
	int escaped = 0;

	for (cursor = text;; cursor++) {
		int terminator;

		if (escaped)
			escaped = 0;
		else if (*cursor == '\\' || *cursor == '/')
			escaped = 1;
		else if (*cursor == '"')
			quoted = !quoted;
		else if ((!quoted && *cursor == ';') || *cursor == '\0') {
			terminator = (unsigned char)*cursor;
			if (count == maximum)
				return -1;
			if (terminator != '\0')
				*cursor = '\0';
			values[count++] = trim(begin);
			if (terminator == '\0')
				break;
			begin = cursor + 1;
		}
	}
	return quoted ? -1 : (int)count;
}

static int
parse_group(struct locale_source *source, enum zedbsd_locale_key first,
	    size_t expected, char *text)
{
	char *parts[16];
	int count = split_values(text, parts, sizeof(parts) / sizeof(parts[0]));
	int index;

	if (count != (int)expected)
		return -1;
	for (index = 0; index < count; index++) {
		char *decoded = decode_string(parts[index], source->utf8);

		if (decoded == NULL ||
		    source_set(source, first + index, decoded, 1) != 0) {
			free(decoded);
			return -1;
		}
		free(decoded);
	}
	return 0;
}

static int
parse_grouping(struct locale_source *source, enum zedbsd_locale_key key,
	       char *text)
{
	char *parts[32];
	char grouping[33];
	int count = split_values(text, parts, 32U);
	int index;

	if (count <= 0)
		return -1;
	for (index = 0; index < count; index++) {
		char *end;
		long value;

		errno = 0;
		value = strtol(parts[index], &end, 10);
		if (errno != 0 || *trim(end) != '\0' || value <= 0 ||
		    value > CHAR_MAX)
			return -1;
		grouping[index] = (char)value;
	}
	grouping[count] = '\0';
	return source_set(source, key, grouping, 1);
}

static int
category_index(const char *name)
{
	int category;

	for (category = 0; category < 6; category++)
		if (strcmp(name, category_names[category]) == 0)
			return category;
	return -1;
}

static int
copy_builtin(struct locale_source *source, int category, const char *name)
{
	int utf8;
	unsigned key;

	if (strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0)
		utf8 = 0;
	else if (strcmp(name, "C.UTF-8") == 0 || strcmp(name, "C.utf8") == 0)
		utf8 = 1;
	else {
		errno = ENOENT;
		return -1;
	}
	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++)
		if (metadata[key].category == category &&
		    source_set(source, (enum zedbsd_locale_key)key,
			       utf8 ? metadata[key].utf8_value
				    : metadata[key].c_value,
			       0) != 0)
			return -1;
	return 0;
}

static int
parse_source(struct locale_source *source, FILE *stream, const char *path)
{
	char line[8192];
	unsigned long line_number = 0;
	int current = -1;
	int comment = '#';
	int failed = 0;

	while (fgets(line, sizeof(line), stream) != NULL) {
		char *keyword;
		char *value;
		char *space;
		int category;

		line_number++;
		if (strchr(line, '\n') == NULL && !feof(stream)) {
			fprintf(stderr, "localedef: %s:%lu: line is too long\n",
				path, line_number);
			return -1;
		}
		strip_comment(line, comment);
		keyword = trim(line);
		if (*keyword == '\0')
			continue;
		space = keyword;
		while (*space != '\0' && !isspace((unsigned char)*space))
			space++;
		if (*space != '\0')
			*space++ = '\0';
		value = trim(space);
		if (current < 0 && strcmp(keyword, "comment_char") == 0) {
			if (value[0] == '\0' || value[1] != '\0')
				goto invalid;
			comment = (unsigned char)value[0];
			continue;
		}
		if ((category = category_index(keyword)) >= 0) {
			if (current >= 0 || *value != '\0' ||
			    source->seen_categories[category])
				goto invalid;
			current = category;
			source->seen_categories[category] = 1;
			continue;
		}
		if (strcmp(keyword, "END") == 0) {
			if (current < 0 ||
			    strcmp(value, category_names[current]) != 0)
				goto invalid;
			current = -1;
			continue;
		}
		if (current < 0)
			goto invalid;
		if (strcmp(keyword, "copy") == 0) {
			char *decoded = decode_string(value, 1);

			if (decoded == NULL ||
			    copy_builtin(source, current, decoded) != 0) {
				free(decoded);
				goto invalid;
			}
			free(decoded);
			continue;
		}
		if (current == LC_TIME && strcmp(keyword, "abday") == 0) {
			if (parse_group(source, ZEDBSD_LOCALE_KEY_ABDAY_1, 7U,
					value) != 0)
				goto invalid;
			continue;
		}
		if (current == LC_TIME && strcmp(keyword, "day") == 0) {
			if (parse_group(source, ZEDBSD_LOCALE_KEY_DAY_1, 7U,
					value) != 0)
				goto invalid;
			continue;
		}
		if (current == LC_TIME && strcmp(keyword, "abmon") == 0) {
			if (parse_group(source, ZEDBSD_LOCALE_KEY_ABMON_1, 12U,
					value) != 0)
				goto invalid;
			continue;
		}
		if (current == LC_TIME && strcmp(keyword, "mon") == 0) {
			if (parse_group(source, ZEDBSD_LOCALE_KEY_MON_1, 12U,
					value) != 0)
				goto invalid;
			continue;
		}
		if (current == LC_TIME && strcmp(keyword, "am_pm") == 0) {
			if (parse_group(source, ZEDBSD_LOCALE_KEY_AM_STR, 2U,
					value) != 0)
				goto invalid;
			continue;
		}
		if ((current == LC_NUMERIC &&
		     strcmp(keyword, "grouping") == 0) ||
		    (current == LC_MONETARY &&
		     strcmp(keyword, "mon_grouping") == 0)) {
			if (parse_grouping(source,
					   current == LC_NUMERIC
					       ? ZEDBSD_LOCALE_KEY_GROUPING
					       : ZEDBSD_LOCALE_KEY_MON_GROUPING,
					   value) != 0)
				goto invalid;
			continue;
		}
		if (current == LC_COLLATE &&
		    strcmp(keyword, "order_end") == 0 && *value == '\0')
			continue;
		{
			enum zedbsd_locale_key key = key_find(keyword, current);
			char *decoded;

			if (key == ZEDBSD_LOCALE_KEY_INVALID)
				goto invalid;
			decoded = decode_string(value, source->utf8);
			if (decoded == NULL ||
			    source_set(source, key, decoded, 1) != 0) {
				free(decoded);
				goto invalid;
			}
			free(decoded);
			continue;
		}

	invalid:
		fprintf(stderr,
			"localedef: %s:%lu: invalid or unsupported %s\n", path,
			line_number, keyword);
		failed = 1;
	}
	if (current >= 0) {
		fprintf(stderr, "localedef: %s: missing END %s\n", path,
			category_names[current]);
		failed = 1;
	}
	if (ferror(stream))
		failed = 1;
	return failed ? -1 : 0;
}

static int
parse_charmap(const char *path, int *utf8)
{
	FILE *stream = fopen(path, "r");
	char line[4096];
	int found = 0;

	if (stream == NULL)
		return -1;
	while (fgets(line, sizeof(line), stream) != NULL) {
		char *text = trim(line);

		if (strncmp(text, "<code_set_name>", 15) != 0)
			continue;
		text = trim(text + 15);
		if (text[0] == '"') {
			text++;
			if (strchr(text, '"') != NULL)
				*strchr(text, '"') = '\0';
		}
		if (strcmp(text, "UTF-8") == 0 || strcmp(text, "UTF8") == 0)
			*utf8 = 1;
		else if (strcmp(text, "US-ASCII") == 0 ||
			 strcmp(text, "ASCII") == 0)
			*utf8 = 0;
		else {
			fclose(stream);
			errno = ENOTSUP;
			return -1;
		}
		found = 1;
		break;
	}
	if (fclose(stream) != 0)
		return -1;
	if (!found) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int
encode_source(struct locale_source *source, unsigned char **result,
	      size_t *result_size)
{
	size_t strings = 0;
	size_t total;
	size_t offset;
	unsigned key;
	unsigned char *data;

	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++) {
		size_t length = strlen(source->values[key]) + 1U;

		if (length > UINT32_MAX - strings) {
			errno = EFBIG;
			return -1;
		}
		strings += length;
	}
	total = ZEDBSD_LOCALE_HEADER_SIZE +
		(ZEDBSD_LOCALE_KEY_COUNT - 1U) * ZEDBSD_LOCALE_ENTRY_SIZE +
		strings;
	if (total > UINT32_MAX) {
		errno = EFBIG;
		return -1;
	}
	data = calloc(1, total);
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
	for (key = 1; key < ZEDBSD_LOCALE_KEY_COUNT; key++) {
		unsigned char *entry = data + ZEDBSD_LOCALE_HEADER_SIZE +
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
	return 0;
}

static int
write_atomic(const char *path, const unsigned char *data, size_t size)
{
	char temporary[PATH_MAX + 1U];
	int descriptor;
	int length;
	int error = 0;

	length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
			  (long)getpid());
	if (length < 0 || (size_t)length >= sizeof(temporary)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	descriptor =
	    open(temporary,
		 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
	if (descriptor < 0)
		return -1;
	if (command_write_all(descriptor, data, size) != 0 ||
	    fsync(descriptor) != 0)
		error = errno;
	if (close(descriptor) != 0 && error == 0)
		error = errno;
	if (error == 0 && rename(temporary, path) == 0)
		return 0;
	if (error == 0)
		error = errno;
	(void)unlink(temporary);
	errno = error;
	return -1;
}

int
main(int argc, char **argv)
{
	struct locale_source source = {0};
	const char *charmap = NULL;
	const char *input = NULL;
	const char *output;
	unsigned char *encoded = NULL;
	size_t encoded_size = 0;
	FILE *stream;
	int utf8 = 0;
	int index = 1;
	int result = 1;

	while (index < argc && argv[index][0] == '-' &&
	       argv[index][1] != '\0') {
		if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		}
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
	if (index + 1 != argc)
		goto usage;
	output = argv[index];
	if (charmap != NULL && parse_charmap(charmap, &utf8) != 0) {
		fprintf(stderr, "localedef: %s: %s\n", charmap,
			strerror(errno));
		goto done;
	}
	if (source_defaults(&source, utf8) != 0)
		goto done;
	stream = input == NULL || strcmp(input, "-") == 0 ? stdin
							  : fopen(input, "r");
	if (stream == NULL) {
		fprintf(stderr, "localedef: %s: %s\n", input, strerror(errno));
		goto done;
	}
	if (parse_source(&source, stream,
			 input != NULL ? input : "standard input") != 0) {
		if (stream != stdin)
			(void)fclose(stream);
		goto done;
	}
	if (stream != stdin && fclose(stream) != 0)
		goto done;
	if (encode_source(&source, &encoded, &encoded_size) != 0 ||
	    write_atomic(output, encoded, encoded_size) != 0) {
		fprintf(stderr, "localedef: %s: %s\n", output, strerror(errno));
		goto done;
	}
	result = 0;
done:
	free(encoded);
	source_free(&source);
	return result;

usage:
	fprintf(
	    stderr,
	    "usage: localedef [-c] [-v] [-f charmap] [-i sourcefile] name\n");
	return 2;
}
