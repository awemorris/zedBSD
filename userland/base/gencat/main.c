/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD gencat userland command.
 */

#include "userland/base/common/command.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <nl_types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "libc/include/zedbsd/catalog-format.h"

struct message {
	uint32_t set;
	uint32_t number;
	char *text;
};

struct catalog {
	struct message *items;
	size_t count;
	size_t capacity;
};

static int catalog_load(struct catalog *catalog, const char *path);
static int read_all(int descriptor, unsigned char *data, size_t size);
static int catalog_set(struct catalog *catalog, uint32_t set, uint32_t number, const char *text);
static struct message *catalog_find(struct catalog *catalog, uint32_t set, uint32_t number);
static int parse_file(struct catalog *catalog, FILE *stream, const char *path);
static char *logical_line(FILE *stream, unsigned long *line_number);
static int positive_number(const char *text, char **end, uint32_t *result);
static void catalog_delete_set(struct catalog *catalog, uint32_t set);
static void catalog_delete(struct catalog *catalog, uint32_t set, uint32_t number);
static char *message_decode(const char *text, int quote);
static int catalog_encode(struct catalog *catalog, unsigned char **result, size_t *size);
static int catalog_write(const char *path, const unsigned char *data, size_t size);
static void catalog_free(struct catalog *catalog);
static int message_compare(const void *left_pointer, const void *right_pointer);

/*
 * Runs the gencat command.
 */
int
main(
	int argc,
	char **argv)
{
	FILE *stream;
	struct catalog catalog = {0};
	unsigned char *encoded;
	size_t encoded_size;
	int failed;
	int index;

	encoded = NULL;
	encoded_size = 0;
	failed = 0;

	/* Validates the command-line arguments. */
	if (argc < 3) {
		fprintf(stderr, "usage: gencat catfile msgfile ...\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the selected command-line operation. */
	if (strcmp(argv[1], "-") != 0 && catalog_load(&catalog, argv[1]) != 0) {
		fprintf(stderr, "gencat: %s: %s\n", argv[1], strerror(errno));
		failed = 1;
		goto done;
	}

	/* Process each remaining command-line operand. */
	for (index = 2; index < argc; index++) {
		stream = !strcmp(argv[index], "-") ? stdin : fopen(argv[index], "r");

		/* Handles the stream availability. */
		if (stream == NULL) {
			fprintf(stderr, "gencat: %s: %s\n", argv[index],
				strerror(errno));
			failed = 1;
			continue;
		}

		/* Validates the command-line arguments. */
		if (parse_file(&catalog, stream, argv[index]) != 0)
			failed = 1;

		/* Handles a failed fclose operation. */
		if (stream != stdin && fclose(stream) != 0)
			failed = 1;
	}

	/* Handles an operation failure. */
	if (!failed && catalog_encode(&catalog, &encoded, &encoded_size) != 0) {
		fprintf(stderr, "gencat: %s\n", strerror(errno));
		failed = 1;
	}

	/* Validates the command-line arguments. */
	if (!failed && catalog_write(argv[1], encoded, encoded_size) != 0) {
		fprintf(stderr, "gencat: %s: %s\n", argv[1], strerror(errno));
		failed = 1;
	}
done:
	free(encoded);
	catalog_free(&catalog);

	/* Returns the computed result. */
	return failed;
}

/* Supports the catalog load operation. */
static int
catalog_load(
	struct catalog *catalog,
	const char *path)
{
	const unsigned char *entry;
	uint32_t set;
	uint32_t number;
	uint32_t offset;
	uint32_t length;
	unsigned char *data;
	off_t end;
	uint32_t count;
	uint32_t entries;
	uint32_t strings;
	uint32_t index;
	int descriptor;

	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return errno == ENOENT ? 0 : -1;
	end = lseek(descriptor, 0, SEEK_END);

	/* Handles a failed lseek operation. */
	if (end < 0 || (uint64_t)end > UINT32_MAX ||
	    lseek(descriptor, 0, SEEK_SET) != 0) {
		(void)close(descriptor);
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	data = malloc(end != 0 ? (size_t)end : 1U);

	/* Handles the data availability. */
	if (data == NULL) {
		(void)close(descriptor);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed read all operation. */
	if (read_all(descriptor, data, (size_t)end) != 0 ||
	    close(descriptor) != 0)
		goto failed;

	/* Handles a failed zedbsd catalog get32 operation. */
	if ((size_t)end < ZEDBSD_CATALOG_HEADER_SIZE ||
	    memcmp(data, ZEDBSD_CATALOG_MAGIC, ZEDBSD_CATALOG_MAGIC_SIZE) !=
		0 ||
	    zedbsd_catalog_get32(data + 8U) != ZEDBSD_CATALOG_VERSION ||
	    zedbsd_catalog_get32(data + 12U) != ZEDBSD_CATALOG_HEADER_SIZE)
		goto invalid;
	count = zedbsd_catalog_get32(data + 16U);
	entries = zedbsd_catalog_get32(data + 20U);
	strings = zedbsd_catalog_get32(data + 24U);

	/* Checks the remaining item count. */
	if (count > UINT32_MAX / ZEDBSD_CATALOG_ENTRY_SIZE ||
	    entries > (uint32_t)end || strings > (uint32_t)end ||
	    count * ZEDBSD_CATALOG_ENTRY_SIZE > (uint32_t)end - entries ||
	    strings < entries + count * ZEDBSD_CATALOG_ENTRY_SIZE)
		goto invalid;

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		entry = data + entries + index * ZEDBSD_CATALOG_ENTRY_SIZE;
		set = zedbsd_catalog_get32(entry);
		number = zedbsd_catalog_get32(entry + 4U);
		offset = zedbsd_catalog_get32(entry + 8U);
		length = zedbsd_catalog_get32(entry + 12U);

		/* Handles a failed catalog set operation. */
		if (set == 0 || number == 0 || offset < strings ||
		    length == UINT32_MAX || offset > (uint32_t)end ||
		    length + 1U > (uint32_t)end - offset ||
		    data[offset + length] != '\0' ||
		    catalog_set(catalog, set, number, (char *)data + offset) !=
			0)
			goto invalid;
	}
	free(data);

	/* Reports successful completion. */
	return 0;

invalid:
	errno = EINVAL;
failed:
	free(data);

	/* Reports operation failure. */
	return -1;
}

/* Supports the read all operation. */
static int
read_all(
	int descriptor,
	unsigned char *data,
	size_t size)
{
	ssize_t count;
	size_t done;

	done = 0;

	/* Process each remaining element. */
	while (done < size) {
		count = read(descriptor, data + done, size - done);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0)
			return -1;
		done += (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the catalog set operation. */
static int
catalog_set(
	struct catalog *catalog,
	uint32_t set,
	uint32_t number,
	const char *text)
{
	size_t capacity;
	void *items;
	struct message *message;
	char *copy;

	message = catalog_find(catalog, set, number);
	copy = strdup(text);

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;

	/* Handles the message availability. */
	if (message != NULL) {
		free(message->text);
		message->text = copy;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the catalog condition. */
	if (catalog->count == catalog->capacity) {
		capacity = catalog->capacity == 0 ? 16U : catalog->capacity * 2U;

		/* Handles the capacity condition. */
		if (capacity < catalog->capacity ||
		    capacity > SIZE_MAX / sizeof(*catalog->items)) {
			free(copy);
			errno = ENOMEM;

			/* Reports operation failure. */
			return -1;
		}
		items =
		    realloc(catalog->items, capacity * sizeof(*catalog->items));

		/* Handles the items availability. */
		if (items == NULL) {
			free(copy);

			/* Reports operation failure. */
			return -1;
		}
		catalog->items = items;
		catalog->capacity = capacity;
	}
	message = &catalog->items[catalog->count++];
	message->set = set;
	message->number = number;
	message->text = copy;

	/* Reports successful completion. */
	return 0;
}

/* Supports the catalog find operation. */
static struct message *
catalog_find(
	struct catalog *catalog,
	uint32_t set,
	uint32_t number)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < catalog->count; index++) {
		/* Handles the catalog condition. */
		if (catalog->items[index].set == set &&
		    catalog->items[index].number == number)

			/* Returns the computed result. */
			return &catalog->items[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the parse file operation. */
static int
parse_file(
	struct catalog *catalog,
	FILE *stream,
	const char *path)
{
	char *end_local;
	char *end_local1;
	char *end_local2;
	char *decoded;
	unsigned long source_line;
	char *line;
	char *cursor;
	uint32_t number;
	uint32_t current_set;
	unsigned long line_number;
	int quote;
	int failed;

	current_set = NL_SETD;
	line_number = 1;
	quote = 0;
	failed = 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		source_line = line_number;
		line = logical_line(stream, &line_number);

		/* Handles the line availability. */
		if (line == NULL)
			break;

		/* Continue while the operation condition remains true. */
		cursor = line;
		while (isspace((unsigned char)*cursor))
			cursor++;

		/* Checks the current cursor position. */
		if (*cursor == '\0') {
			free(line);
			continue;
		}

		/* Checks the current cursor position. */
		if (*cursor == '$') {
			cursor++;

			/* Handles a failed isspace operation. */
			if (!strncmp(cursor, "set", 3) &&
			    isspace((unsigned char)cursor[3])) {
				cursor += 3;

				/* Continue while the operation condition remains true. */
				while (isspace((unsigned char)*cursor))
					cursor++;

				/* Handles a failed positive number operation. */
				if (!positive_number(cursor, &end_local, &number) ||
				    (*end_local != '\0' &&
				     !isspace((unsigned char)*end_local)))
					goto invalid;
				current_set = number;
			} else if (!strncmp(cursor, "delset", 6) &&
				   isspace((unsigned char)cursor[6])) {
				cursor += 6;

				/* Continue while the operation condition remains true. */
				while (isspace((unsigned char)*cursor))
					cursor++;

				/* Handles a failed positive number operation. */
				if (!positive_number(cursor, &end_local1, &number))
					goto invalid;

				/* Continue while the operation condition remains true. */
				while (isspace((unsigned char)*end_local1))
					end_local1++;

				/* Handles the end local1 condition. */
				if (*end_local1 != '\0')
					goto invalid;
				catalog_delete_set(catalog, number);
			} else if (!strncmp(cursor, "quote", 5) &&
				   (cursor[5] == '\0' ||
				    isspace((unsigned char)cursor[5]))) {
				cursor += 5;

				/* Continue while the operation condition remains true. */
				while (isspace((unsigned char)*cursor))
					cursor++;
				quote = (unsigned char)*cursor;

				/* Checks the current cursor position. */
				if (*cursor != '\0' && cursor[1] != '\0')
					goto invalid;
			}
			free(line);
			continue;
		}

		/* Handles a failed positive number operation. */
		if (!positive_number(cursor, &end_local2, &number) ||
		    (*end_local2 != '\0' && !isspace((unsigned char)*end_local2)))
			goto invalid;

		/* Continue while the operation condition remains true. */
		while (isspace((unsigned char)*end_local2))
			end_local2++;

		/* Handles the end local2 condition. */
		if (*end_local2 == '\0') {
			catalog_delete(catalog, current_set, number);
			free(line);
			continue;
		}
		decoded = message_decode(end_local2, quote);

		/* Handles a failed catalog set operation. */
		if (decoded == NULL ||
		    catalog_set(catalog, current_set, number,
				decoded) != 0) {
			free(decoded);
			goto invalid;
		}
		free(decoded);
		free(line);
		continue;

	invalid:
		fprintf(stderr, "gencat: %s:%lu: invalid message source\n",
			path, source_line);
		free(line);
		failed = 1;
	}

	/* Handles an operation failure. */
	if (ferror(stream)) {
		fprintf(stderr, "gencat: %s: %s\n", path, strerror(errno));
		failed = 1;
	}

	/* Returns the computed result. */
	return failed ? -1 : 0;
}

/* Supports the logical line operation. */
static char *
logical_line(
	FILE *stream,
	unsigned long *line_number)
{
	size_t wanted;
	char *replacement;
	char *line;
	size_t used;
	size_t capacity;
	int character;

	line = NULL;
	used = 0;
	capacity = 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		character = fgetc(stream);

		/* Handles the end-of-file condition. */
		if (character == EOF) {
			/* Checks the current capacity usage. */
			if (used == 0) {
				free(line);

				/* Reports that no result is available. */
				return NULL;
			}
			break;
		}

		/* Checks the current capacity usage. */
		if (used + 2U > capacity) {
			wanted = capacity == 0 ? 128U : capacity * 2U;

			/* Handles the wanted condition. */
			if (wanted < capacity) {
				free(line);
				errno = ENOMEM;

				/* Reports that no result is available. */
				return NULL;
			}
			replacement = realloc(line, wanted);

			/* Handles the replacement availability. */
			if (replacement == NULL) {
				free(line);

				/* Reports that no result is available. */
				return NULL;
			}
			line = replacement;
			capacity = wanted;
		}

		/* Classifies the current input character. */
		if (character == '\n') {
			(*line_number)++;

			/* Checks the current capacity usage. */
			if (used != 0 && line[used - 1U] == '\\') {
				used--;
				continue;
			}
			break;
		}
		line[used++] = (char)character;
	}
	line[used] = '\0';

	/* Returns the computed result. */
	return line;
}

/* Supports the positive number operation. */
static int
positive_number(
	const char *text,
	char **end,
	uint32_t *result)
{
	unsigned long value;

	/* Handles a failed isdigit operation. */
	if (!isdigit((unsigned char)*text))
		return 0;
	errno = 0;
	value = strtoul(text, end, 10);

	/* Handles the reported system error. */
	if (errno == ERANGE || value == 0 || value > UINT32_MAX)
		return 0;
	*result = (uint32_t)value;
	/* Reports operation failure. */
	return 1;
}

/* Supports the catalog delete set operation. */
static void
catalog_delete_set(
	struct catalog *catalog,
	uint32_t set)
{
	size_t index;

	index = 0;

	/* Process each remaining element. */
	while (index < catalog->count) {
		/* Handles the catalog condition. */
		if (catalog->items[index].set == set) {
			free(catalog->items[index].text);
			catalog->items[index] =
			    catalog->items[--catalog->count];
		} else {
			index++;
		}
	}
}

/* Supports the catalog delete operation. */
static void
catalog_delete(
	struct catalog *catalog,
	uint32_t set,
	uint32_t number)
{
	struct message *message;

	message = catalog_find(catalog, set, number);

	/* Handles the message availability. */
	if (message == NULL)
		return;
	free(message->text);
	*message = catalog->items[--catalog->count];
}

/* Supports the message decode operation. */
static char *
message_decode(
	const char *text,
	int quote)
{
	unsigned value;
	unsigned digits;
	char *result;
	char *output;
	int quoted = quote != 0 && (unsigned char)*text == (unsigned char)quote;

	result = malloc(strlen(text) + 1U);
	output = result;

	/* Handles the result availability. */
	if (result == NULL)
		return NULL;

	/* Handles the quoted condition. */
	if (quoted)
		text++;

	/* Continue while the operation condition remains true. */
	while (*text != '\0') {
		value = 0;
		digits = 0;

		/* Handles the quoted condition. */
		if (quoted && (unsigned char)*text == (unsigned char)quote) {
			text++;

			/* Continue while the operation condition remains true. */
			while (isspace((unsigned char)*text))
				text++;

			/* Validates the current text. */
			if (*text != '\0')
				goto invalid;
			*output = '\0';
			/* Returns the computed result. */
			return result;
		}

		/* Validates the current text. */
		if (*text != '\\') {
			*output++ = *text++;
			continue;
		}
		text++;

		/* Validates the current text. */
		if (*text >= '0' && *text <= '7') {
			/* Continue while the operation condition remains true. */
			while (digits < 3U && *text >= '0' && *text <= '7') {
				value = value * 8U + (unsigned)(*text++ - '0');
				digits++;
			}

			/* Validates the current value. */
			if (value == 0)
				goto invalid;
			*output++ = (char)(unsigned char)value;
			continue;
		}

		/* Validates the current text. */
		if (*text == '\0')
			goto invalid;

		/* Dispatch the selected operation case. */
		switch (*text++) {
		case 'n':
			*output++ = '\n';
			break;
		case 't':
			*output++ = '\t';
			break;
		case 'v':
			*output++ = '\v';
			break;
		case 'b':
			*output++ = '\b';
			break;
		case 'r':
			*output++ = '\r';
			break;
		case 'f':
			*output++ = '\f';
			break;
		default:
			*output++ = text[-1];
			break;
		}
	}

	/* Handles the quoted condition. */
	if (quoted)
		goto invalid;
	*output = '\0';
	/* Returns the computed result. */
	return result;

invalid:
	free(result);
	errno = EINVAL;

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the catalog encode operation. */
static int
catalog_encode(
	struct catalog *catalog,
	unsigned char **result,
	size_t *size)
{
	size_t length;
	unsigned char *entry;
	size_t strings;
	size_t total;
	size_t offset;
	size_t index;
	unsigned char *data;

	strings = 0;

	qsort(catalog->items, catalog->count, sizeof(*catalog->items),
	      message_compare);

	/* Process each remaining element. */
	for (index = 0; index < catalog->count; index++) {
		length = strlen(catalog->items[index].text) + 1U;

		/* Checks the current data length. */
		if (length > UINT32_MAX - strings) {
			errno = EFBIG;

			/* Reports operation failure. */
			return -1;
		}
		strings += length;
	}

	/* Handles the catalog condition. */
	if (catalog->count > UINT32_MAX / ZEDBSD_CATALOG_ENTRY_SIZE ||
	    catalog->count * ZEDBSD_CATALOG_ENTRY_SIZE >
		UINT32_MAX - ZEDBSD_CATALOG_HEADER_SIZE ||
	    strings > UINT32_MAX - ZEDBSD_CATALOG_HEADER_SIZE -
			  catalog->count * ZEDBSD_CATALOG_ENTRY_SIZE) {
		errno = EFBIG;

		/* Reports operation failure. */
		return -1;
	}
	total = ZEDBSD_CATALOG_HEADER_SIZE +
		catalog->count * ZEDBSD_CATALOG_ENTRY_SIZE + strings;
	data = calloc(1, total != 0 ? total : 1U);

	/* Handles the data availability. */
	if (data == NULL)
		return -1;
	memcpy(data, ZEDBSD_CATALOG_MAGIC, ZEDBSD_CATALOG_MAGIC_SIZE);
	zedbsd_catalog_put32(data + 8U, ZEDBSD_CATALOG_VERSION);
	zedbsd_catalog_put32(data + 12U, ZEDBSD_CATALOG_HEADER_SIZE);
	zedbsd_catalog_put32(data + 16U, (uint32_t)catalog->count);
	zedbsd_catalog_put32(data + 20U, ZEDBSD_CATALOG_HEADER_SIZE);
	zedbsd_catalog_put32(data + 24U, ZEDBSD_CATALOG_HEADER_SIZE +
					     (uint32_t)catalog->count *
						 ZEDBSD_CATALOG_ENTRY_SIZE);
	offset = ZEDBSD_CATALOG_HEADER_SIZE +
		 catalog->count * ZEDBSD_CATALOG_ENTRY_SIZE;

	/* Process each remaining element. */
	for (index = 0; index < catalog->count; index++) {
		entry = data + ZEDBSD_CATALOG_HEADER_SIZE +
		       index * ZEDBSD_CATALOG_ENTRY_SIZE;
		length = strlen(catalog->items[index].text);

		zedbsd_catalog_put32(entry, catalog->items[index].set);
		zedbsd_catalog_put32(entry + 4U, catalog->items[index].number);
		zedbsd_catalog_put32(entry + 8U, (uint32_t)offset);
		zedbsd_catalog_put32(entry + 12U, (uint32_t)length);
		memcpy(data + offset, catalog->items[index].text, length + 1U);
		offset += length + 1U;
	}
	*result = data;
	*size = total;
	/* Reports successful completion. */
	return 0;
}

/* Supports the catalog write operation. */
static int
catalog_write(
	const char *path,
	const unsigned char *data,
	size_t size)
{
	int function_result;
	char temporary[PATH_MAX + 1U];
	int descriptor;
	int length;
	int operation_error;
	int saved_errno;

	operation_error = 0;

	/* Selects the matching value. */
	if (!strcmp(path, "-")) {
		/* Obtains the command write all result. */
		function_result = command_write_all(STDOUT_FILENO, data, size);

		/* Returns the computed result. */
		return function_result;
	}

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
		operation_error = errno;

	/* Handles an operation failure. */
	if (close(descriptor) != 0 && operation_error == 0)
		operation_error = errno;

	/* Handles an operation failure. */
	if (operation_error == 0 && rename(temporary, path) == 0)
		return 0;
	saved_errno = operation_error != 0 ? operation_error : errno;
	(void)unlink(temporary);
	errno = saved_errno;

	/* Reports operation failure. */
	return -1;
}

/* Supports the catalog free operation. */
static void
catalog_free(
	struct catalog *catalog)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < catalog->count; index++)
		free(catalog->items[index].text);
	free(catalog->items);
	memset(catalog, 0, sizeof(*catalog));
}

/* Supports the message compare operation. */
static int
message_compare(
	const void *left_pointer,
	const void *right_pointer)
{
	const struct message *left;
	const struct message *right;

	left = left_pointer;
	right = right_pointer;

	/* Handles the left condition. */
	if (left->set != right->set)
		return left->set < right->set ? -1 : 1;

	/* Handles the left condition. */
	if (left->number != right->number)
		return left->number < right->number ? -1 : 1;

	/* Reports successful completion. */
	return 0;
}
