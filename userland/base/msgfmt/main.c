/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD msgfmt userland command.
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

static int parse_options(int argc, char **argv, struct options *options, int *first);
static char *input_path(const struct options *options, const char *operand);
static char *duplicate_string(const char *string);
static void diagnostic(const char *path, unsigned long line, const char *message);
static int parse_stream(struct catalogs *catalogs, FILE *stream, const char *path, const struct options *options);
static struct catalog *catalog_get(struct catalogs *catalogs, const char *domain);
static struct catalog *catalog_find(struct catalogs *catalogs, const char *domain);
static void *resize_array(void *pointer, size_t count, size_t size);
static int read_line(FILE *stream, char **line, size_t *capacity, size_t *length);
static int parse_directive(struct parser *parser, char *line);
static int parse_value(char **destination, const char *text);
static int append_quoted(char **destination, const char *text, const char **end_pointer);
static int append_byte(char **string, size_t *length, unsigned value);
static int hexadecimal_value(unsigned char character);
static int message_complete(struct parser *parser);
static int formats_compatible(const char *source, const char *translation);
static int format_signature(const char *format, char signature[128]);
static void sort_signature(char *signature, size_t length);
static void message_discard(struct message *message);
static int translation_slot(struct message *message, size_t index, char ***slot);
static int write_catalog(struct catalog *catalog, const char *path);
static int write_u32(FILE *stream, uint32_t value);
static size_t original_length(const struct message *message);
static size_t translation_length(const struct message *message);
static char *default_output_name(const char *domain, int strict);
static void catalogs_discard(struct catalogs *catalogs);
static int message_compare(const void *left, const void *right);

/*
 * Runs the msgfmt command.
 */
int
main(
	int argc,
	char **argv)
{
	char *path;
	FILE *stream;
	struct catalog *source;
	size_t message_index;
	struct catalog combined;
	size_t total;
	struct catalog *catalog;
	char *output;
	size_t messages;
	struct catalogs catalogs;
	struct options options;
	size_t catalog_index;
	int first, operand;
	int result;

	result = 1;

	memset(&catalogs, 0, sizeof(catalogs));

	/* Validates the command-line arguments. */
	if (parse_options(argc, argv, &options, &first) != 0) {
		fprintf(stderr, "usage: msgfmt [-cfSv] [-D directory] [-o "
				"outputfile] pathname ...\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Process each remaining command-line operand. */
	for (operand = first; operand < argc; operand++) {
		path = input_path(&options, argv[operand]);

		/* Handles the path availability. */
		if (path == NULL)
			goto done;
		stream = !strcmp(path, "-") ? stdin : fopen(path, "r");

		/* Handles the stream availability. */
		if (stream == NULL) {
			diagnostic(path, 0, strerror(errno));
			free(path);
			goto done;
		}

		/* Handles a failed parse stream operation. */
		if (parse_stream(&catalogs, stream, path, &options) != 0) {
			/* Handles the stream condition. */
			if (stream != stdin)
				(void)fclose(stream);
			free(path);
			goto done;
		}

		/* Handles a failed fclose operation. */
		if (stream != stdin && fclose(stream) != 0) {
			free(path);
			goto done;
		}
		free(path);
	}

	/* Handles the output availability. */
	if (options.output != NULL) {
		total = 0;

		memset(&combined, 0, sizeof(combined));

		/* Process each remaining element. */
		combined.domain = (char *)"messages";
		for (catalog_index = 0; catalog_index < catalogs.count;
		     catalog_index++)
			total += catalogs.items[catalog_index].count;
		combined.messages =
		    resize_array(NULL, total, sizeof(*combined.messages));

		/* Handles the messages availability. */
		if (total != 0 && combined.messages == NULL)
			goto done;

		/* Process each remaining element. */
		for (catalog_index = 0; catalog_index < catalogs.count;
		     catalog_index++) {
			source = &catalogs.items[catalog_index];

			/* Process each remaining element. */
			for (message_index = 0; message_index < source->count;
			     message_index++) {
				combined.messages[combined.count++] =
				    source->messages[message_index];
			}
			source->count = 0;
		}

		/* Handles a failed write catalog operation. */
		if (write_catalog(&combined, options.output) != 0) {
			/* Process each remaining element. */
			for (catalog_index = 0; catalog_index < combined.count;
			     catalog_index++) {
				message_discard(
				    &combined.messages[catalog_index]);
			}
			free(combined.messages);
			goto done;
		}

		/* Process each remaining element. */
		for (catalog_index = 0; catalog_index < combined.count;
		     catalog_index++)
			message_discard(&combined.messages[catalog_index]);
		free(combined.messages);
	} else {
		/* Process each remaining element. */
		for (catalog_index = 0; catalog_index < catalogs.count;
		     catalog_index++) {
			catalog = &catalogs.items[catalog_index];

			/* Handles the catalog condition. */
			if (catalog->count == 0)
				continue;
			output = default_output_name(catalog->domain,
						     options.strict);

			/* Handles a failed write catalog operation. */
			if (output == NULL ||
			    write_catalog(catalog, output) != 0) {
				free(output);
				goto done;
			}
			free(output);
		}
	}

	/* Checks the selected options. */
	if (options.verbose) {
		messages = 0;

		/* Process each remaining element. */
		for (catalog_index = 0; catalog_index < catalogs.count;
		     catalog_index++)
			messages += catalogs.items[catalog_index].count;
		fprintf(stderr, "%lu translated messages.\n",
			(unsigned long)messages);
	}
	result = 0;
done:
	catalogs_discard(&catalogs);

	/* Returns the computed result. */
	return result;
}

/* Supports the parse options operation. */
static int
parse_options(
	int argc,
	char **argv,
	struct options *options,
	int *first)
{
	const char *value;
	char option_name;
	const char *argument;
	unsigned position;
	int index;

	memset(options, 0, sizeof(*options));

	/* Process each remaining command-line operand. */
	for (index = 1; index < argc; index++) {
		argument = argv[index];

		/* Handles the argument condition. */
		if (argument[0] != '-' || argument[1] == '\0')
			break;

		/* Selects the matching value. */
		if (!strcmp(argument, "--")) {
			index++;
			break;
		}

		/* Process each element required by the operation. */
		for (position = 1; argument[position] != '\0'; position++) {
			option_name = argument[position];

			/* Dispatch the selected operation case. */
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
			case 'o':

			/* Handles the argument condition. */
			if (argument[position + 1U] != '\0') {
				value = argument + position + 1U;
				position =
				    (unsigned)strlen(argument) - 1U;
			} else if (++index < argc) {
				value = argv[index];
			} else {
				/* Reports operation failure. */
				return -1;
			}

			/* Handles the option name condition. */
			if (option_name == 'D')
				options->directory = value;
			else
				options->output = value;
			break;
			default:
				/* Reports operation failure. */
				return -1;
			}
		}
	}
	*first = index;
	/* Returns the computed result. */
	return index < argc ? 0 : -1;
}

/* Supports the input path operation. */
static char *
input_path(
	const struct options *options,
	const char *operand)
{
	char *function_result;
	size_t directory_length;
	size_t operand_length;
	char *path;

	/* Handles the directory availability. */
	if (options->directory == NULL || operand[0] == '/' ||
	    !strcmp(operand, "-")) {
		/* Obtains the duplicate string result. */
		function_result = duplicate_string(operand);

		/* Returns the computed result. */
		return function_result;
	}

	directory_length = strlen(options->directory);
	operand_length = strlen(operand);
	path = malloc(directory_length + operand_length + 2U);

	/* Handles the path availability. */
	if (path == NULL)
		return NULL;
	memcpy(path, options->directory, directory_length);
	path[directory_length] = '/';
	memcpy(path + directory_length + 1U, operand, operand_length + 1U);

	/* Returns the computed result. */
	return path;
}

/* Supports the duplicate string operation. */
static char *
duplicate_string(
	const char *string)
{
	size_t length;
	char *copy;

	length = strlen(string) + 1U;
	copy = malloc(length);

	/* Handles the copy availability. */
	if (copy != NULL)
		memcpy(copy, string, length);

	/* Returns the computed result. */
	return copy;
}

/* Supports the diagnostic operation. */
static void
diagnostic(
	const char *path,
	unsigned long line,
	const char *message)
{
	/* Handles the line condition. */
	if (line != 0)
		fprintf(stderr, "msgfmt: %s:%lu: %s\n", path, line, message);
	else
		fprintf(stderr, "msgfmt: %s: %s\n", path, message);
}

/* Supports the parse stream operation. */
static int
parse_stream(
	struct catalogs *catalogs,
	FILE *stream,
	const char *path,
	const struct options *options)
{
	struct parser parser;
	char *line;
	size_t capacity;
	size_t length;
	int line_result;
	int result;

	line = NULL;
	capacity = 0;
	result = -1;

	memset(&parser, 0, sizeof(parser));
	parser.catalogs = catalogs;
	parser.catalog = catalog_get(catalogs, "messages");
	parser.use_fuzzy = options->use_fuzzy;
	parser.check = options->check;
	parser.path = path;

	/* Handles the catalog availability. */
	if (parser.catalog == NULL)
		goto done;

	/* Process each remaining element. */
	while ((line_result = read_line(stream, &line, &capacity, &length)) >
	       0) {
		parser.line++;

		/* Process each remaining element. */
		while (length != 0 && line[length - 1] == '\r')
			line[--length] = '\0';

		/* Handles a failed parse directive operation. */
		if (parse_directive(&parser, line) != 0)
			goto done;
	}

	/* Handles the line result condition. */
	if (line_result < 0) {
		diagnostic(path, parser.line, "input error");
		goto done;
	}

	/* Handles a failed message complete operation. */
	if (message_complete(&parser) != 0)
		goto done;
	result = 0;
done:
	message_discard(&parser.message);
	free(line);

	/* Returns the computed result. */
	return result;
}

/* Supports the catalog get operation. */
static struct catalog *
catalog_get(
	struct catalogs *catalogs,
	const char *domain)
{
	size_t capacity;
	void *items;
	struct catalog *catalog;

	catalog = catalog_find(catalogs, domain);

	/* Handles the catalog availability. */
	if (catalog != NULL)
		return catalog;

	/* Handles the catalogs condition. */
	if (catalogs->count == catalogs->capacity) {
		capacity = catalogs->capacity == 0 ? 4U : catalogs->capacity * 2U;
		items = resize_array(catalogs->items, capacity,
			   sizeof(*catalogs->items));

		/* Handles the items availability. */
		if (items == NULL)
			return NULL;
		catalogs->items = items;
		catalogs->capacity = capacity;
	}
	catalog = &catalogs->items[catalogs->count++];
	memset(catalog, 0, sizeof(*catalog));
	catalog->domain = duplicate_string(domain);

	/* Handles the domain availability. */
	if (catalog->domain == NULL) {
		catalogs->count--;

		/* Reports that no result is available. */
		return NULL;
	}

	/* Returns the computed result. */
	return catalog;
}

/* Supports the catalog find operation. */
static struct catalog *
catalog_find(
	struct catalogs *catalogs,
	const char *domain)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < catalogs->count; index++) {
		/* Selects the matching value. */
		if (!strcmp(catalogs->items[index].domain, domain))
			return &catalogs->items[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the resize array operation. */
static void *
resize_array(
	void *pointer,
	size_t count,
	size_t size)
{
	void *function_result;

	/* Checks the current data size. */
	if (size != 0 && count > SIZE_MAX / size) {
		errno = ENOMEM;

		/* Reports that no result is available. */
		return NULL;
	}

	/* Obtains the realloc result. */
	function_result = realloc(pointer, count * size);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the read line operation. */
static int
read_line(
	FILE *stream,
	char **line,
	size_t *capacity,
	size_t *length)
{
	size_t new_capacity;
	char *new_line;
	int character;

	/* Process input until it is exhausted. */
	*length = 0;
	while ((character = fgetc(stream)) != EOF) {
		/* Checks the current data length. */
		if (*length + 1U >= *capacity) {
			new_capacity = *capacity == 0 ? 128U : *capacity * 2U;
			new_line = resize_array(*line, new_capacity, 1U);

			/* Handles the new line availability. */
			if (new_line == NULL)
				return -1;
			*line = new_line;
			*capacity = new_capacity;
		}

		/* Classifies the current input character. */
		if (character == '\n')
			break;
		(*line)[(*length)++] = (char)character;
	}

	/* Handles the end-of-file condition. */
	if (character == EOF && ferror(stream))
		return -1;

	/* Handles the end-of-file condition. */
	if (character == EOF && *length == 0)
		return 0;
	(*line)[*length] = '\0';

	/* Reports operation failure. */
	return 1;
}

/* Supports the parse directive operation. */
static int
parse_directive(
	struct parser *parser,
	char *line)
{
	char *domain;
	char *end;
	unsigned long number;
	char *cursor;
	char *keyword;
	char *value;

	cursor = line;

	/* Continue while the operation condition remains true. */
	while (isspace((unsigned char)*cursor))
		cursor++;

	/* Checks the current cursor position. */
	if (*cursor == '\0') {
		parser->active = NULL;

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the current cursor position. */
	if (*cursor == '#') {
		/* Checks the current cursor position. */
		if (cursor[1] == ',') {
			/* Handles a failed strstr operation. */
			if (strstr(cursor + 2, "fuzzy") != NULL)
				parser->fuzzy = 1;

			/* Handles a failed strstr operation. */
			if (strstr(cursor + 2, "c-format") != NULL &&
			    strstr(cursor + 2, "no-c-format") == NULL)
				parser->c_format = 1;
		}

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the current cursor position. */
	if (*cursor == '"') {
		/* Handles a failed parse value operation. */
		if (parser->active == NULL ||
		    parse_value(parser->active, cursor) != 0)
			goto syntax;

		/* Reports successful completion. */
		return 0;
	}

	/* Continue while the operation condition remains true. */
	keyword = cursor;
	while (*cursor != '\0' && !isspace((unsigned char)*cursor))
		cursor++;

	/* Checks the current cursor position. */
	if (*cursor == '\0')
		goto syntax;
	*cursor++ = '\0';
	value = cursor;

	/* Selects the matching value. */
	if (!strcmp(keyword, "domain")) {
		domain = NULL;

		/* Handles a failed message complete operation. */
		if (message_complete(parser) != 0 ||
		    parse_value(&domain, value) != 0 || domain[0] == '\0') {
			free(domain);
			goto syntax;
		}
		parser->catalog = catalog_get(parser->catalogs, domain);
		free(domain);

		/* Handles the catalog availability. */
		if (parser->catalog == NULL)
			return -1;
		parser->fuzzy = 0;
		parser->active = NULL;

		/* Reports successful completion. */
		return 0;
	}

	/* Selects the matching value. */
	if (!strcmp(keyword, "msgid")) {
		/* Handles a failed message complete operation. */
		if (message_complete(parser) != 0)
			return -1;
		parser->message.fuzzy = parser->fuzzy;
		parser->message.c_format = parser->c_format;
		parser->fuzzy = 0;
		parser->c_format = 0;
		parser->active = &parser->message.identifier;
	} else if (!strcmp(keyword, "msgid_plural")) {
		/* Handles the identifier availability. */
		if (parser->message.identifier == NULL)
			goto syntax;
		parser->active = &parser->message.plural_identifier;
	} else if (!strncmp(keyword, "msgstr[", 7)) {
		number = strtoul(keyword + 7, &end, 10);

		/* Handles a failed translation slot operation. */
		if (*end != ']' || end[1] != '\0' || number > SIZE_MAX ||
		    translation_slot(&parser->message, (size_t)number,
				     &parser->active) != 0)
			goto syntax;
	} else if (!strcmp(keyword, "msgstr")) {
		/* Handles a failed translation slot operation. */
		if (translation_slot(&parser->message, 0, &parser->active) != 0)
			return -1;
	} else {
		goto syntax;
	}

	/* Handles the active availability. */
	if (*parser->active != NULL)
		goto syntax;

	/* Handles a failed parse value operation. */
	if (parse_value(parser->active, value) != 0)
		goto syntax;

	/* Reports successful completion. */
	return 0;
syntax:
	diagnostic(parser->path, parser->line,
		   "invalid portable object syntax");

	/* Reports operation failure. */
	return -1;
}

/* Supports the parse value operation. */
static int
parse_value(
	char **destination,
	const char *text)
{
	const char *cursor;
	int found;

	cursor = text;
	found = 0;

	/* Continue while the operation condition remains true. */
	while (isspace((unsigned char)*cursor))
		cursor++;

	/* Continue while the operation condition remains true. */
	while (*cursor == '"') {
		/* Handles a failed append quoted operation. */
		if (append_quoted(destination, cursor, &cursor) != 0)
			return -1;

		/* Continue while the operation condition remains true. */
		found = 1;
		while (isspace((unsigned char)*cursor))
			cursor++;
	}

	/* Returns the computed result. */
	return found && *cursor == '\0' ? 0 : -1;
}

/* Supports the append quoted operation. */
static int
append_quoted(
	char **destination,
	const char *text,
	const char **end_pointer)
{
	unsigned digits_local;
	unsigned digits_local1;
	int digit;
	unsigned value;
	size_t length = *destination != NULL ? strlen(*destination) : 0;
	const char *cursor;
	char *empty;

	cursor = text;

	/* Handles the destination availability. */
	if (*destination == NULL) {
		empty = malloc(1);

		/* Handles the empty availability. */
		if (empty == NULL)
			return -1;
		empty[0] = '\0';
		*destination = empty;
	}

	while (isspace((unsigned char)*cursor))
		cursor++;

	/* Checks the current cursor position. */
	if (*cursor != '"')
		return -1;
	cursor++;

	/* Continue while the operation condition remains true. */
	while (*cursor != '\0' && *cursor != '"') {
		/* Checks the current cursor position. */
		if (*cursor != '\\') {
			/* Handles a failed append byte operation. */
			if (append_byte(destination, &length,
					(unsigned char)*cursor++) != 0)

				/* Reports operation failure. */
				return -1;
			continue;
		}
		cursor++;

		/* Checks the current cursor position. */
		if (*cursor == '\0')
			return -1;

		/* Dispatch the selected operation case. */
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
			/* Checks the current cursor position. */
			if (*cursor >= '0' && *cursor <= '7') {
				digits_local = 0;

				/* Continue while the operation condition remains true. */
				value = 0;
				while (digits_local < 3U && *cursor >= '0' &&
				       *cursor <= '7') {
					value = value * 8U +
						(unsigned)(*cursor++ - '0');
					digits_local++;
				}
			} else if (*cursor == 'x') {
				digits_local1 = 0;

				cursor++;

				/* Continue while the operation condition remains true. */
				value = 0;
				while ((digit = hexadecimal_value(
					    (unsigned char)*cursor)) >= 0) {
					value = value * 16U + (unsigned)digit;
					cursor++;
					digits_local1++;
				}

				/* Handles the digits local1 condition. */
				if (digits_local1 == 0)
					return -1;
			} else {
				value = (unsigned char)*cursor++;
			}
			break;
		}

		/* Handles a failed append byte operation. */
		if (append_byte(destination, &length, value) != 0)
			return -1;
	}

	/* Checks the current cursor position. */
	if (*cursor != '"')
		return -1;
	*end_pointer = cursor + 1;
	/* Reports successful completion. */
	return 0;
}

/* Supports the append byte operation. */
static int
append_byte(
	char **string,
	size_t *length,
	unsigned value)
{
	char *result;

	result = resize_array(*string, *length + 2U, 1U);

	/* Handles the result availability. */
	if (result == NULL)
		return -1;
	result[(*length)++] = (char)(unsigned char)value;
	result[*length] = '\0';
	*string = result;
	/* Reports successful completion. */
	return 0;
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

/* Supports the message complete operation. */
static int
message_complete(
	struct parser *parser)
{
	const char *source;
	size_t capacity;
	void *messages;
	struct message *destination;
	size_t index;

	parser->active = NULL;

	/* Handles the identifier availability. */
	if (parser->message.identifier == NULL)
		return 0;

	/* Checks the parser state. */
	if (parser->message.translation_count == 0 ||
	    parser->message.translations[0] == NULL) {
		diagnostic(parser->path, parser->line,
			   "msgid has no corresponding msgstr");

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the plural identifier availability. */
	if (parser->message.plural_identifier != NULL &&
	    parser->message.translation_count < 2U) {
		diagnostic(parser->path, parser->line,
			   "plural msgid has fewer than two msgstr forms");

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index = 0; index < parser->message.translation_count; index++) {
		/* Checks the parser state. */
		if (parser->message.translations[index] == NULL) {
			diagnostic(parser->path, parser->line,
				   "plural msgstr indexes are not contiguous");

			/* Reports operation failure. */
			return -1;
		}
	}

	/* Checks the parser state. */
	if (parser->check && parser->message.c_format) {
		/* Process each remaining element. */
		for (index = 0; index < parser->message.translation_count;
		     index++) {
			source = index == 0 ||
		    parser->message.plural_identifier == NULL
		? parser->message.identifier
		: parser->message.plural_identifier;

			/* Handles a failed formats compatible operation. */
			if (!formats_compatible(
				source, parser->message.translations[index])) {
				diagnostic(
				    parser->path, parser->line,
				    "msgid and msgstr format arguments differ");

				/* Reports operation failure. */
				return -1;
			}
		}
	}

	/* Checks the parser state. */
	if (parser->message.identifier[0] != '\0') {
		/* Process each remaining element. */
		for (index = 0; index < parser->message.translation_count;
		     index++) {
			/* Checks the parser state. */
			if (parser->message.translations[index][0] == '\0') {
				message_discard(&parser->message);

				/* Reports successful completion. */
				return 0;
			}
		}
	}

	/* Checks the parser state. */
	if (parser->message.fuzzy && !parser->use_fuzzy) {
		message_discard(&parser->message);

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining element. */
	for (index = 0; index < parser->catalog->count; index++) {
		/* Selects the matching value. */
		if (!strcmp(parser->catalog->messages[index].identifier,
			    parser->message.identifier)) {
			message_discard(&parser->message);

			/* Reports successful completion. */
			return 0;
		}
	}

	/* Checks the parser state. */
	if (parser->catalog->count == parser->catalog->capacity) {
		capacity = parser->catalog->capacity == 0
		      ? 16U
		      : parser->catalog->capacity * 2U;
		messages = resize_array(parser->catalog->messages, capacity,
		 sizeof(*parser->catalog->messages));

		/* Handles the messages availability. */
		if (messages == NULL)
			return -1;
		parser->catalog->messages = messages;
		parser->catalog->capacity = capacity;
	}
	destination = &parser->catalog->messages[parser->catalog->count++];
	*destination = parser->message;
	memset(&parser->message, 0, sizeof(parser->message));

	/* Reports successful completion. */
	return 0;
}

/* Supports the formats compatible operation. */
static int
formats_compatible(
	const char *source,
	const char *translation)
{
	int function_result;
	char source_signature[128];
	char translation_signature[128];

	/* Computes the function result. */
	function_result = format_signature(source, source_signature) == 0 &&
	       format_signature(translation, translation_signature) == 0 &&
	       !strcmp(source_signature, translation_signature);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the format signature operation. */
static int
format_signature(
	const char *format,
	char signature[128])
{
	const char *begin;
	int length;
	char type;
	size_t count;

	count = 0;

	/* Continue while the operation condition remains true. */
	while (*format != '\0') {
		length = 0;

		/* Handles the format condition. */
		if (*format++ != '%')
			continue;

		/* Handles the format condition. */
		if (*format == '%') {
			format++;
			continue;
		}

		/* Continue while the operation condition remains true. */
		begin = format;
		while (isdigit((unsigned char)*format))
			format++;

		/* Handles the format condition. */
		if (*format != '$')
			format = begin;
		else
			format++;

		/* Continue while the operation condition remains true. */
		while (*format != '\0' && strchr("-+ #0'", *format) != NULL)
			format++;

		/* Handles the format condition. */
		if (*format == '*') {
			/* Checks the remaining item count. */
			if (count + 1U >= 128U)
				return -1;
			signature[count++] = 'i';
			format++;

			/* Continue while the operation condition remains true. */
			while (isdigit((unsigned char)*format))
				format++;

			/* Handles the format condition. */
			if (*format == '$')
				format++;
		} else {
			/* Continue while the operation condition remains true. */
			while (isdigit((unsigned char)*format))
				format++;
		}

		/* Handles the format condition. */
		if (*format == '.') {
			format++;

			/* Handles the format condition. */
			if (*format == '*') {
				/* Checks the remaining item count. */
				if (count + 1U >= 128U)
					return -1;
				signature[count++] = 'i';
				format++;

				/* Continue while the operation condition remains true. */
				while (isdigit((unsigned char)*format))
					format++;

				/* Handles the format condition. */
				if (*format == '$')
					format++;
			} else {
				/* Continue while the operation condition remains true. */
				while (isdigit((unsigned char)*format))
					format++;
			}
		}

		/* Handles the format condition. */
		if (*format == 'h') {
			length = 1;
			format++;

			/* Handles the format condition. */
			if (*format == 'h')
				format++;
		} else if (*format == 'l') {
			length = 2;
			format++;

			/* Handles the format condition. */
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

		/* Checks the remaining item count. */
		if (count + 1U >= 128U || *format == '\0')
			return -1;

		/* Dispatch the selected operation case. */
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
			/* Reports operation failure. */
			return -1;
		}
		signature[count++] = type;
	}
	sort_signature(signature, count);
	signature[count] = '\0';

	/* Reports successful completion. */
	return 0;
}

/* Supports the sort signature operation. */
static void
sort_signature(
	char *signature,
	size_t length)
{
	char value;
	size_t position;
	size_t index;

	/* Process each remaining element. */
	for (index = 1; index < length; index++) {
		value = signature[index];
		position = index;

		/* Continue while the operation condition remains true. */
		while (position != 0 && signature[position - 1U] > value) {
			signature[position] = signature[position - 1U];
			position--;
		}
		signature[position] = value;
	}
}

/* Supports the message discard operation. */
static void
message_discard(
	struct message *message)
{
	size_t index;

	free(message->identifier);
	free(message->plural_identifier);

	/* Process each remaining element. */
	for (index = 0; index < message->translation_count; index++)
		free(message->translations[index]);
	free(message->translations);
	memset(message, 0, sizeof(*message));
}

/* Supports the translation slot operation. */
static int
translation_slot(
	struct message *message,
	size_t index,
	char ***slot)
{
	size_t count;
	char **translations;
	size_t position;

	/* Checks the current index. */
	if (index >= message->translation_count) {
		count = index + 1U;
		translations = resize_array(message->translations, count,
				   sizeof(*translations));

		/* Handles the translations availability. */
		if (translations == NULL)
			return -1;

		/* Process each remaining element. */
		for (position = message->translation_count; position < count;
		     position++)
			translations[position] = NULL;
		message->translations = translations;
		message->translation_count = count;
	}
	*slot = &message->translations[index];
	/* Reports successful completion. */
	return 0;
}

/* Supports the write catalog operation. */
static int
write_catalog(
	struct catalog *catalog,
	const char *path)
{
	size_t length_local;
	size_t length_local1;
	const struct message *message_local;
	size_t length_local2;
	const struct message *message_local3;
	size_t form;
	FILE *stream;
	uint64_t original_offset, translation_offset, string_offset;
	uint64_t cursor;
	size_t index;
	int failed;

	stream = !strcmp(path, "-") ? stdout : fopen(path, "wb");
	failed = 0;

	/* Handles the stream availability. */
	if (stream == NULL) {
		diagnostic(path, 0, strerror(errno));

		/* Reports operation failure. */
		return -1;
	}
	qsort(catalog->messages, catalog->count, sizeof(*catalog->messages),
	      message_compare);
	original_offset = 7U * 4U;
	translation_offset = original_offset + catalog->count * 8U;
	string_offset = translation_offset + catalog->count * 8U;

	/* Handles the catalog condition. */
	if (catalog->count > UINT32_MAX || string_offset > UINT32_MAX)
		failed = 1;
	failed |= write_u32(stream, 0x950412deU);
	failed |= write_u32(stream, 0);
	failed |= write_u32(stream, (uint32_t)catalog->count);
	failed |= write_u32(stream, (uint32_t)original_offset);
	failed |= write_u32(stream, (uint32_t)translation_offset);
	failed |= write_u32(stream, 0);
	failed |= write_u32(stream, 0);

	/* Process each remaining element. */
	cursor = string_offset;
	for (index = 0; index < catalog->count; index++) {
		length_local = original_length(&catalog->messages[index]);

		/* Handles the length local condition. */
		if (length_local > UINT32_MAX || cursor > UINT32_MAX)
			failed = 1;
		failed |= write_u32(stream, (uint32_t)length_local);
		failed |= write_u32(stream, (uint32_t)cursor);
		cursor += length_local + 1U;
	}

	/* Process each remaining element. */
	for (index = 0; index < catalog->count; index++) {
		length_local1 = translation_length(&catalog->messages[index]);

		/* Handles the length local1 condition. */
		if (length_local1 > UINT32_MAX || cursor > UINT32_MAX)
			failed = 1;
		failed |= write_u32(stream, (uint32_t)length_local1);
		failed |= write_u32(stream, (uint32_t)cursor);
		cursor += length_local1 + 1U;
	}

	/* Process each remaining element. */
	for (index = 0; index < catalog->count; index++) {
		message_local = &catalog->messages[index];
		length_local2 = strlen(message_local->identifier);

		/* Handles a failed fwrite operation. */
		if (fwrite(message_local->identifier, 1, length_local2, stream) != length_local2)
			failed = 1;

		/* Handles the end-of-file condition. */
		if (message_local->plural_identifier != NULL &&
		    (fputc('\0', stream) == EOF ||
		     fputs(message_local->plural_identifier, stream) == EOF))
			failed = 1;

		/* Handles the end-of-file condition. */
		if (fputc('\0', stream) == EOF)
			failed = 1;
	}

	/* Process each remaining element. */
	for (index = 0; index < catalog->count; index++) {
		message_local3 = &catalog->messages[index];

		/* Process each remaining element. */
		for (form = 0; form < message_local3->translation_count; form++) {
			/* Handles the end-of-file condition. */
			if (form != 0 && fputc('\0', stream) == EOF)
				failed = 1;

			/* Handles the end-of-file condition. */
			if (message_local3->translations[form] != NULL &&
			    fputs(message_local3->translations[form], stream) == EOF)
				failed = 1;
		}

		/* Handles the end-of-file condition. */
		if (fputc('\0', stream) == EOF)
			failed = 1;
	}

	/* Handles a failed fflush operation. */
	if (fflush(stream) != 0)
		failed = 1;

	/* Handles a failed fclose operation. */
	if (stream != stdout && fclose(stream) != 0)
		failed = 1;

	/* Handles an operation failure. */
	if (failed) {
		diagnostic(path, 0, "cannot write messages object");

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the write u32 operation. */
static int
write_u32(
	FILE *stream,
	uint32_t value)
{
	int function_result;
	unsigned char bytes[4];

	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);

	/* Computes the function result. */
	function_result = fwrite(bytes, 1, sizeof(bytes), stream) == sizeof(bytes) ? 0
									: -1;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the original length operation. */
static size_t
original_length(
	const struct message *message)
{
	size_t function_result;

	/* Computes the function result. */
	function_result = strlen(message->identifier) +
	       (message->plural_identifier != NULL
		    ? strlen(message->plural_identifier) + 1U
		    : 0U);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the translation length operation. */
static size_t
translation_length(
	const struct message *message)
{
	size_t index;
	size_t length;

	length = 0;

	/* Process each remaining element. */
	for (index = 0; index < message->translation_count; index++) {
		/* Checks the current index. */
		if (index != 0)
			length++;

		/* Handles the message condition. */
		if (message->translations[index] != NULL)
			length += strlen(message->translations[index]);
	}

	/* Returns the computed result. */
	return length;
}

/* Supports the default output name operation. */
static char *
default_output_name(
	const char *domain,
	int strict)
{
	char *function_result;
	size_t length;
	int has_suffix;
	char *name;

	length = strlen(domain);
	has_suffix = length >= 3U && !strcmp(domain + length - 3U, ".mo");

	/* Handles the strict condition. */
	if (!strict || has_suffix) {
		/* Obtains the duplicate string result. */
		function_result = duplicate_string(domain);

		/* Returns the computed result. */
		return function_result;
	}

	name = malloc(length + 4U);

	/* Handles the name availability. */
	if (name != NULL) {
		memcpy(name, domain, length);
		memcpy(name + length, ".mo", 4U);
	}

	/* Returns the computed result. */
	return name;
}

/* Supports the catalogs discard operation. */
static void
catalogs_discard(
	struct catalogs *catalogs)
{
	struct catalog *catalog;
	size_t catalog_index, message_index;

	/* Process each remaining element. */
	for (catalog_index = 0; catalog_index < catalogs->count;
	     catalog_index++) {
		catalog = &catalogs->items[catalog_index];

		/* Process each remaining element. */
		for (message_index = 0; message_index < catalog->count;
		     message_index++)
			message_discard(&catalog->messages[message_index]);
		free(catalog->messages);
		free(catalog->domain);
	}
	free(catalogs->items);
}

/* Supports the message compare operation. */
static int
message_compare(
	const void *left,
	const void *right)
{
	int function_result;
	const struct message *a;
	const struct message *b;

	a = left;
	b = right;

	/* Obtains the strcmp result. */
	function_result = strcmp(a->identifier, b->identifier);

	/* Returns the computed result. */
	return function_result;
}
