/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct message {
	char *identifier;
	char *plural_identifier;
	char **translations;
	size_t translation_count;
	int fuzzy;
	int c_format;
};

struct catalog {
	char *domain;
	struct message *messages;
	size_t count;
	size_t capacity;
};

struct catalogs {
	struct catalog *items;
	size_t count;
	size_t capacity;
};

struct parser {
	struct catalogs *catalogs;
	struct catalog *catalog;
	struct message message;
	char **active;
	int fuzzy;
	int c_format;
	int use_fuzzy;
	int check;
	const char *path;
	unsigned long line;
};

struct options {
	const char *directory;
	const char *output;
	int check;
	int strict;
	int use_fuzzy;
	int verbose;
};

static void
diagnostic(const char *path, unsigned long line, const char *message)
{
	if (line != 0)
		fprintf(stderr, "msgfmt: %s:%lu: %s\n", path, line, message);
	else
		fprintf(stderr, "msgfmt: %s: %s\n", path, message);
}

static void *
resize_array(void *pointer, size_t count, size_t size)
{
	if (size != 0 && count > SIZE_MAX / size) {
		errno = ENOMEM;
		return NULL;
	}
	return realloc(pointer, count * size);
}

static char *
duplicate_string(const char *string)
{
	size_t length = strlen(string) + 1U;
	char *copy = malloc(length);

	if (copy != NULL)
		memcpy(copy, string, length);
	return copy;
}

static struct catalog *
catalog_find(struct catalogs *catalogs, const char *domain)
{
	size_t index;

	for (index = 0; index < catalogs->count; index++)
		if (!strcmp(catalogs->items[index].domain, domain))
			return &catalogs->items[index];
	return NULL;
}

static struct catalog *
catalog_get(struct catalogs *catalogs, const char *domain)
{
	struct catalog *catalog = catalog_find(catalogs, domain);

	if (catalog != NULL)
		return catalog;
	if (catalogs->count == catalogs->capacity) {
		size_t capacity =
		    catalogs->capacity == 0 ? 4U : catalogs->capacity * 2U;
		void *items = resize_array(catalogs->items, capacity,
					   sizeof(*catalogs->items));

		if (items == NULL)
			return NULL;
		catalogs->items = items;
		catalogs->capacity = capacity;
	}
	catalog = &catalogs->items[catalogs->count++];
	memset(catalog, 0, sizeof(*catalog));
	catalog->domain = duplicate_string(domain);
	if (catalog->domain == NULL) {
		catalogs->count--;
		return NULL;
	}
	return catalog;
}

static void
message_discard(struct message *message)
{
	size_t index;

	free(message->identifier);
	free(message->plural_identifier);
	for (index = 0; index < message->translation_count; index++)
		free(message->translations[index]);
	free(message->translations);
	memset(message, 0, sizeof(*message));
}

static void
sort_signature(char *signature, size_t length)
{
	size_t index;

	for (index = 1; index < length; index++) {
		char value = signature[index];
		size_t position = index;

		while (position != 0 && signature[position - 1U] > value) {
			signature[position] = signature[position - 1U];
			position--;
		}
		signature[position] = value;
	}
}

static int
format_signature(const char *format, char signature[128])
{
	size_t count = 0;

	while (*format != '\0') {
		const char *begin;
		int length = 0;
		char type;

		if (*format++ != '%')
			continue;
		if (*format == '%') {
			format++;
			continue;
		}
		begin = format;
		while (isdigit((unsigned char)*format))
			format++;
		if (*format != '$')
			format = begin;
		else
			format++;
		while (*format != '\0' && strchr("-+ #0'", *format) != NULL)
			format++;
		if (*format == '*') {
			if (count + 1U >= 128U)
				return -1;
			signature[count++] = 'i';
			format++;
			while (isdigit((unsigned char)*format))
				format++;
			if (*format == '$')
				format++;
		} else {
			while (isdigit((unsigned char)*format))
				format++;
		}
		if (*format == '.') {
			format++;
			if (*format == '*') {
				if (count + 1U >= 128U)
					return -1;
				signature[count++] = 'i';
				format++;
				while (isdigit((unsigned char)*format))
					format++;
				if (*format == '$')
					format++;
			} else {
				while (isdigit((unsigned char)*format))
					format++;
			}
		}
		if (*format == 'h') {
			length = 1;
			format++;
			if (*format == 'h')
				format++;
		} else if (*format == 'l') {
			length = 2;
			format++;
			if (*format == 'l') {
				length = 3;
				format++;
			}
		} else if (*format != '\0' && strchr("jzt", *format) != NULL) {
			length = 3;
			format++;
		} else if (*format == 'L') {
			length = 4;
			format++;
		}
		if (count + 1U >= 128U || *format == '\0')
			return -1;
		switch (*format++) {
		case 'd':
		case 'i':
			type = length == 2 ? 'l' : length >= 3 ? 'q' : 'i';
			break;
		case 'o':
		case 'u':
		case 'x':
		case 'X':
			type = length == 2 ? 'L' : length >= 3 ? 'Q' : 'I';
			break;
		case 'a':
		case 'A':
		case 'e':
		case 'E':
		case 'f':
		case 'F':
		case 'g':
		case 'G':
			type = length == 4 ? 'D' : 'd';
			break;
		case 'c':
			type = length == 2 ? 'w' : 'i';
			break;
		case 's':
			type = length == 2 ? 'W' : 'p';
			break;
		case 'p':
			type = 'p';
			break;
		case 'n':
			type = length == 2 ? 'n' : length >= 3 ? 'N' : 'm';
			break;
		default:
			return -1;
		}
		signature[count++] = type;
	}
	sort_signature(signature, count);
	signature[count] = '\0';
	return 0;
}

static int
formats_compatible(const char *source, const char *translation)
{
	char source_signature[128];
	char translation_signature[128];

	return format_signature(source, source_signature) == 0 &&
	       format_signature(translation, translation_signature) == 0 &&
	       !strcmp(source_signature, translation_signature);
}

static int
message_complete(struct parser *parser)
{
	struct message *destination;
	size_t index;

	parser->active = NULL;
	if (parser->message.identifier == NULL)
		return 0;
	if (parser->message.translation_count == 0 ||
	    parser->message.translations[0] == NULL) {
		diagnostic(parser->path, parser->line,
			   "msgid has no corresponding msgstr");
		return -1;
	}
	if (parser->message.plural_identifier != NULL &&
	    parser->message.translation_count < 2U) {
		diagnostic(parser->path, parser->line,
			   "plural msgid has fewer than two msgstr forms");
		return -1;
	}
	for (index = 0; index < parser->message.translation_count; index++) {
		if (parser->message.translations[index] == NULL) {
			diagnostic(parser->path, parser->line,
				   "plural msgstr indexes are not contiguous");
			return -1;
		}
	}
	if (parser->check && parser->message.c_format) {
		for (index = 0; index < parser->message.translation_count;
		     index++) {
			const char *source =
			    index == 0 ||
				    parser->message.plural_identifier == NULL
				? parser->message.identifier
				: parser->message.plural_identifier;

			if (!formats_compatible(
				source, parser->message.translations[index])) {
				diagnostic(
				    parser->path, parser->line,
				    "msgid and msgstr format arguments differ");
				return -1;
			}
		}
	}
	if (parser->message.identifier[0] != '\0') {
		for (index = 0; index < parser->message.translation_count;
		     index++)
			if (parser->message.translations[index][0] == '\0') {
				message_discard(&parser->message);
				return 0;
			}
	}
	if (parser->message.fuzzy && !parser->use_fuzzy) {
		message_discard(&parser->message);
		return 0;
	}
	for (index = 0; index < parser->catalog->count; index++)
		if (!strcmp(parser->catalog->messages[index].identifier,
			    parser->message.identifier)) {
			message_discard(&parser->message);
			return 0;
		}
	if (parser->catalog->count == parser->catalog->capacity) {
		size_t capacity = parser->catalog->capacity == 0
				      ? 16U
				      : parser->catalog->capacity * 2U;
		void *messages =
		    resize_array(parser->catalog->messages, capacity,
				 sizeof(*parser->catalog->messages));

		if (messages == NULL)
			return -1;
		parser->catalog->messages = messages;
		parser->catalog->capacity = capacity;
	}
	destination = &parser->catalog->messages[parser->catalog->count++];
	*destination = parser->message;
	memset(&parser->message, 0, sizeof(parser->message));
	return 0;
}

static int
append_byte(char **string, size_t *length, unsigned value)
{
	char *result = resize_array(*string, *length + 2U, 1U);

	if (result == NULL)
		return -1;
	result[(*length)++] = (char)(unsigned char)value;
	result[*length] = '\0';
	*string = result;
	return 0;
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

static int
append_quoted(char **destination, const char *text, const char **end_pointer)
{
	size_t length = *destination != NULL ? strlen(*destination) : 0;
	const char *cursor = text;
	char *empty;

	if (*destination == NULL) {
		empty = malloc(1);
		if (empty == NULL)
			return -1;
		empty[0] = '\0';
		*destination = empty;
	}

	while (isspace((unsigned char)*cursor))
		cursor++;
	if (*cursor != '"')
		return -1;
	cursor++;
	while (*cursor != '\0' && *cursor != '"') {
		unsigned value;

		if (*cursor != '\\') {
			if (append_byte(destination, &length,
					(unsigned char)*cursor++) != 0)
				return -1;
			continue;
		}
		cursor++;
		if (*cursor == '\0')
			return -1;
		switch (*cursor) {
		case 'a':
			value = '\a';
			cursor++;
			break;
		case 'b':
			value = '\b';
			cursor++;
			break;
		case 'f':
			value = '\f';
			cursor++;
			break;
		case 'n':
			value = '\n';
			cursor++;
			break;
		case 'r':
			value = '\r';
			cursor++;
			break;
		case 't':
			value = '\t';
			cursor++;
			break;
		case 'v':
			value = '\v';
			cursor++;
			break;
		case '\\':
			value = '\\';
			cursor++;
			break;
		case '"':
			value = '"';
			cursor++;
			break;
		default:
			if (*cursor >= '0' && *cursor <= '7') {
				unsigned digits = 0;

				value = 0;
				while (digits < 3U && *cursor >= '0' &&
				       *cursor <= '7') {
					value = value * 8U +
						(unsigned)(*cursor++ - '0');
					digits++;
				}
			} else if (*cursor == 'x') {
				int digit;
				unsigned digits = 0;

				cursor++;
				value = 0;
				while ((digit = hexadecimal_value(
					    (unsigned char)*cursor)) >= 0) {
					value = value * 16U + (unsigned)digit;
					cursor++;
					digits++;
				}
				if (digits == 0)
					return -1;
			} else {
				value = (unsigned char)*cursor++;
			}
			break;
		}
		if (append_byte(destination, &length, value) != 0)
			return -1;
	}
	if (*cursor != '"')
		return -1;
	*end_pointer = cursor + 1;
	return 0;
}

static int
parse_value(char **destination, const char *text)
{
	const char *cursor = text;
	int found = 0;

	while (isspace((unsigned char)*cursor))
		cursor++;
	while (*cursor == '"') {
		if (append_quoted(destination, cursor, &cursor) != 0)
			return -1;
		found = 1;
		while (isspace((unsigned char)*cursor))
			cursor++;
	}
	return found && *cursor == '\0' ? 0 : -1;
}

static int
translation_slot(struct message *message, size_t index, char ***slot)
{
	if (index >= message->translation_count) {
		size_t count = index + 1U;
		char **translations = resize_array(message->translations, count,
						   sizeof(*translations));
		size_t position;

		if (translations == NULL)
			return -1;
		for (position = message->translation_count; position < count;
		     position++)
			translations[position] = NULL;
		message->translations = translations;
		message->translation_count = count;
	}
	*slot = &message->translations[index];
	return 0;
}

static int
parse_directive(struct parser *parser, char *line)
{
	char *cursor = line;
	char *keyword;
	char *value;

	while (isspace((unsigned char)*cursor))
		cursor++;
	if (*cursor == '\0') {
		parser->active = NULL;
		return 0;
	}
	if (*cursor == '#') {
		if (cursor[1] == ',') {
			if (strstr(cursor + 2, "fuzzy") != NULL)
				parser->fuzzy = 1;
			if (strstr(cursor + 2, "c-format") != NULL &&
			    strstr(cursor + 2, "no-c-format") == NULL)
				parser->c_format = 1;
		}
		return 0;
	}
	if (*cursor == '"') {
		if (parser->active == NULL ||
		    parse_value(parser->active, cursor) != 0)
			goto syntax;
		return 0;
	}
	keyword = cursor;
	while (*cursor != '\0' && !isspace((unsigned char)*cursor))
		cursor++;
	if (*cursor == '\0')
		goto syntax;
	*cursor++ = '\0';
	value = cursor;
	if (!strcmp(keyword, "domain")) {
		char *domain = NULL;

		if (message_complete(parser) != 0 ||
		    parse_value(&domain, value) != 0 || domain[0] == '\0') {
			free(domain);
			goto syntax;
		}
		parser->catalog = catalog_get(parser->catalogs, domain);
		free(domain);
		if (parser->catalog == NULL)
			return -1;
		parser->fuzzy = 0;
		parser->active = NULL;
		return 0;
	}
	if (!strcmp(keyword, "msgid")) {
		if (message_complete(parser) != 0)
			return -1;
		parser->message.fuzzy = parser->fuzzy;
		parser->message.c_format = parser->c_format;
		parser->fuzzy = 0;
		parser->c_format = 0;
		parser->active = &parser->message.identifier;
	} else if (!strcmp(keyword, "msgid_plural")) {
		if (parser->message.identifier == NULL)
			goto syntax;
		parser->active = &parser->message.plural_identifier;
	} else if (!strncmp(keyword, "msgstr[", 7)) {
		char *end;
		unsigned long number = strtoul(keyword + 7, &end, 10);

		if (*end != ']' || end[1] != '\0' || number > SIZE_MAX ||
		    translation_slot(&parser->message, (size_t)number,
				     &parser->active) != 0)
			goto syntax;
	} else if (!strcmp(keyword, "msgstr")) {
		if (translation_slot(&parser->message, 0, &parser->active) != 0)
			return -1;
	} else {
		goto syntax;
	}
	if (*parser->active != NULL)
		goto syntax;
	if (parse_value(parser->active, value) != 0)
		goto syntax;
	return 0;
syntax:
	diagnostic(parser->path, parser->line,
		   "invalid portable object syntax");
	return -1;
}

static int
read_line(FILE *stream, char **line, size_t *capacity, size_t *length)
{
	int character;

	*length = 0;
	while ((character = fgetc(stream)) != EOF) {
		if (*length + 1U >= *capacity) {
			size_t new_capacity =
			    *capacity == 0 ? 128U : *capacity * 2U;
			char *new_line = resize_array(*line, new_capacity, 1U);

			if (new_line == NULL)
				return -1;
			*line = new_line;
			*capacity = new_capacity;
		}
		if (character == '\n')
			break;
		(*line)[(*length)++] = (char)character;
	}
	if (character == EOF && ferror(stream))
		return -1;
	if (character == EOF && *length == 0)
		return 0;
	(*line)[*length] = '\0';
	return 1;
}

static int
parse_stream(struct catalogs *catalogs, FILE *stream, const char *path,
	     const struct options *options)
{
	struct parser parser;
	char *line = NULL;
	size_t capacity = 0;
	size_t length;
	int line_result;
	int result = -1;

	memset(&parser, 0, sizeof(parser));
	parser.catalogs = catalogs;
	parser.catalog = catalog_get(catalogs, "messages");
	parser.use_fuzzy = options->use_fuzzy;
	parser.check = options->check;
	parser.path = path;
	if (parser.catalog == NULL)
		goto done;
	while ((line_result = read_line(stream, &line, &capacity, &length)) >
	       0) {
		parser.line++;
		while (length != 0 && line[length - 1] == '\r')
			line[--length] = '\0';
		if (parse_directive(&parser, line) != 0)
			goto done;
	}
	if (line_result < 0) {
		diagnostic(path, parser.line, "input error");
		goto done;
	}
	if (message_complete(&parser) != 0)
		goto done;
	result = 0;
done:
	message_discard(&parser.message);
	free(line);
	return result;
}

static int
message_compare(const void *left, const void *right)
{
	const struct message *a = left;
	const struct message *b = right;

	return strcmp(a->identifier, b->identifier);
}

static int
write_u32(FILE *stream, uint32_t value)
{
	unsigned char bytes[4];

	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);
	return fwrite(bytes, 1, sizeof(bytes), stream) == sizeof(bytes) ? 0
									: -1;
}

static size_t
original_length(const struct message *message)
{
	return strlen(message->identifier) +
	       (message->plural_identifier != NULL
		    ? strlen(message->plural_identifier) + 1U
		    : 0U);
}

static size_t
translation_length(const struct message *message)
{
	size_t index;
	size_t length = 0;

	for (index = 0; index < message->translation_count; index++) {
		if (index != 0)
			length++;
		if (message->translations[index] != NULL)
			length += strlen(message->translations[index]);
	}
	return length;
}

static int
write_catalog(struct catalog *catalog, const char *path)
{
	FILE *stream = !strcmp(path, "-") ? stdout : fopen(path, "wb");
	uint64_t original_offset, translation_offset, string_offset;
	uint64_t cursor;
	size_t index;
	int failed = 0;

	if (stream == NULL) {
		diagnostic(path, 0, strerror(errno));
		return -1;
	}
	qsort(catalog->messages, catalog->count, sizeof(*catalog->messages),
	      message_compare);
	original_offset = 7U * 4U;
	translation_offset = original_offset + catalog->count * 8U;
	string_offset = translation_offset + catalog->count * 8U;
	if (catalog->count > UINT32_MAX || string_offset > UINT32_MAX)
		failed = 1;
	failed |= write_u32(stream, 0x950412deU);
	failed |= write_u32(stream, 0);
	failed |= write_u32(stream, (uint32_t)catalog->count);
	failed |= write_u32(stream, (uint32_t)original_offset);
	failed |= write_u32(stream, (uint32_t)translation_offset);
	failed |= write_u32(stream, 0);
	failed |= write_u32(stream, 0);
	cursor = string_offset;
	for (index = 0; index < catalog->count; index++) {
		size_t length = original_length(&catalog->messages[index]);

		if (length > UINT32_MAX || cursor > UINT32_MAX)
			failed = 1;
		failed |= write_u32(stream, (uint32_t)length);
		failed |= write_u32(stream, (uint32_t)cursor);
		cursor += length + 1U;
	}
	for (index = 0; index < catalog->count; index++) {
		size_t length = translation_length(&catalog->messages[index]);

		if (length > UINT32_MAX || cursor > UINT32_MAX)
			failed = 1;
		failed |= write_u32(stream, (uint32_t)length);
		failed |= write_u32(stream, (uint32_t)cursor);
		cursor += length + 1U;
	}
	for (index = 0; index < catalog->count; index++) {
		const struct message *message = &catalog->messages[index];
		size_t length = strlen(message->identifier);

		if (fwrite(message->identifier, 1, length, stream) != length)
			failed = 1;
		if (message->plural_identifier != NULL &&
		    (fputc('\0', stream) == EOF ||
		     fputs(message->plural_identifier, stream) == EOF))
			failed = 1;
		if (fputc('\0', stream) == EOF)
			failed = 1;
	}
	for (index = 0; index < catalog->count; index++) {
		const struct message *message = &catalog->messages[index];
		size_t form;

		for (form = 0; form < message->translation_count; form++) {
			if (form != 0 && fputc('\0', stream) == EOF)
				failed = 1;
			if (message->translations[form] != NULL &&
			    fputs(message->translations[form], stream) == EOF)
				failed = 1;
		}
		if (fputc('\0', stream) == EOF)
			failed = 1;
	}
	if (fflush(stream) != 0)
		failed = 1;
	if (stream != stdout && fclose(stream) != 0)
		failed = 1;
	if (failed) {
		diagnostic(path, 0, "cannot write messages object");
		return -1;
	}
	return 0;
}

static int
parse_options(int argc, char **argv, struct options *options, int *first)
{
	int index;

	memset(options, 0, sizeof(*options));
	for (index = 1; index < argc; index++) {
		const char *argument = argv[index];
		unsigned position;

		if (argument[0] != '-' || argument[1] == '\0')
			break;
		if (!strcmp(argument, "--")) {
			index++;
			break;
		}
		for (position = 1; argument[position] != '\0'; position++) {
			char option_name = argument[position];

			switch (argument[position]) {
			case 'c':
				options->check = 1;
				break;
			case 'f':
				options->use_fuzzy = 1;
				break;
			case 'S':
				options->strict = 1;
				break;
			case 'v':
				options->verbose = 1;
				break;
			case 'D':
			case 'o': {
				const char *value;

				if (argument[position + 1U] != '\0') {
					value = argument + position + 1U;
					position =
					    (unsigned)strlen(argument) - 1U;
				} else if (++index < argc) {
					value = argv[index];
				} else {
					return -1;
				}
				if (option_name == 'D')
					options->directory = value;
				else
					options->output = value;
				break;
			}
			default:
				return -1;
			}
		}
	}
	*first = index;
	return index < argc ? 0 : -1;
}

static char *
input_path(const struct options *options, const char *operand)
{
	size_t directory_length;
	size_t operand_length;
	char *path;

	if (options->directory == NULL || operand[0] == '/' ||
	    !strcmp(operand, "-"))
		return duplicate_string(operand);
	directory_length = strlen(options->directory);
	operand_length = strlen(operand);
	path = malloc(directory_length + operand_length + 2U);
	if (path == NULL)
		return NULL;
	memcpy(path, options->directory, directory_length);
	path[directory_length] = '/';
	memcpy(path + directory_length + 1U, operand, operand_length + 1U);
	return path;
}

static char *
default_output_name(const char *domain, int strict)
{
	size_t length = strlen(domain);
	int has_suffix = length >= 3U && !strcmp(domain + length - 3U, ".mo");
	char *name;

	if (!strict || has_suffix)
		return duplicate_string(domain);
	name = malloc(length + 4U);
	if (name != NULL) {
		memcpy(name, domain, length);
		memcpy(name + length, ".mo", 4U);
	}
	return name;
}

static void
catalogs_discard(struct catalogs *catalogs)
{
	size_t catalog_index, message_index;

	for (catalog_index = 0; catalog_index < catalogs->count;
	     catalog_index++) {
		struct catalog *catalog = &catalogs->items[catalog_index];

		for (message_index = 0; message_index < catalog->count;
		     message_index++)
			message_discard(&catalog->messages[message_index]);
		free(catalog->messages);
		free(catalog->domain);
	}
	free(catalogs->items);
}

int
main(int argc, char **argv)
{
	struct catalogs catalogs;
	struct options options;
	size_t catalog_index;
	int first, operand;
	int result = 1;

	memset(&catalogs, 0, sizeof(catalogs));
	if (parse_options(argc, argv, &options, &first) != 0) {
		fprintf(stderr, "usage: msgfmt [-cfSv] [-D directory] [-o "
				"outputfile] pathname ...\n");
		return 2;
	}
	for (operand = first; operand < argc; operand++) {
		char *path = input_path(&options, argv[operand]);
		FILE *stream;

		if (path == NULL)
			goto done;
		stream = !strcmp(path, "-") ? stdin : fopen(path, "r");
		if (stream == NULL) {
			diagnostic(path, 0, strerror(errno));
			free(path);
			goto done;
		}
		if (parse_stream(&catalogs, stream, path, &options) != 0) {
			if (stream != stdin)
				(void)fclose(stream);
			free(path);
			goto done;
		}
		if (stream != stdin && fclose(stream) != 0) {
			free(path);
			goto done;
		}
		free(path);
	}
	if (options.output != NULL) {
		struct catalog combined;
		size_t total = 0;

		memset(&combined, 0, sizeof(combined));
		combined.domain = (char *)"messages";
		for (catalog_index = 0; catalog_index < catalogs.count;
		     catalog_index++)
			total += catalogs.items[catalog_index].count;
		combined.messages =
		    resize_array(NULL, total, sizeof(*combined.messages));
		if (total != 0 && combined.messages == NULL)
			goto done;
		for (catalog_index = 0; catalog_index < catalogs.count;
		     catalog_index++) {
			struct catalog *source = &catalogs.items[catalog_index];
			size_t message_index;

			for (message_index = 0; message_index < source->count;
			     message_index++)
				combined.messages[combined.count++] =
				    source->messages[message_index];
			source->count = 0;
		}
		if (write_catalog(&combined, options.output) != 0) {
			for (catalog_index = 0; catalog_index < combined.count;
			     catalog_index++)
				message_discard(
				    &combined.messages[catalog_index]);
			free(combined.messages);
			goto done;
		}
		for (catalog_index = 0; catalog_index < combined.count;
		     catalog_index++)
			message_discard(&combined.messages[catalog_index]);
		free(combined.messages);
	} else {
		for (catalog_index = 0; catalog_index < catalogs.count;
		     catalog_index++) {
			struct catalog *catalog =
			    &catalogs.items[catalog_index];
			char *output;

			if (catalog->count == 0)
				continue;
			output = default_output_name(catalog->domain,
						     options.strict);
			if (output == NULL ||
			    write_catalog(catalog, output) != 0) {
				free(output);
				goto done;
			}
			free(output);
		}
	}
	if (options.verbose) {
		size_t messages = 0;

		for (catalog_index = 0; catalog_index < catalogs.count;
		     catalog_index++)
			messages += catalogs.items[catalog_index].count;
		fprintf(stderr, "%lu translated messages.\n",
			(unsigned long)messages);
	}
	result = 0;
done:
	catalogs_discard(&catalogs);
	return result;
}
