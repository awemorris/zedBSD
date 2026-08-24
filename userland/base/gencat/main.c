/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static void
catalog_free(struct catalog *catalog)
{
	size_t index;

	for (index = 0; index < catalog->count; index++)
		free(catalog->items[index].text);
	free(catalog->items);
	memset(catalog, 0, sizeof(*catalog));
}

static struct message *
catalog_find(struct catalog *catalog, uint32_t set, uint32_t number)
{
	size_t index;

	for (index = 0; index < catalog->count; index++)
		if (catalog->items[index].set == set &&
		    catalog->items[index].number == number)
			return &catalog->items[index];
	return NULL;
}

static int
catalog_set(struct catalog *catalog, uint32_t set, uint32_t number,
	    const char *text)
{
	struct message *message = catalog_find(catalog, set, number);
	char *copy = strdup(text);

	if (copy == NULL)
		return -1;
	if (message != NULL) {
		free(message->text);
		message->text = copy;
		return 0;
	}
	if (catalog->count == catalog->capacity) {
		size_t capacity =
		    catalog->capacity == 0 ? 16U : catalog->capacity * 2U;
		void *items;

		if (capacity < catalog->capacity ||
		    capacity > SIZE_MAX / sizeof(*catalog->items)) {
			free(copy);
			errno = ENOMEM;
			return -1;
		}
		items =
		    realloc(catalog->items, capacity * sizeof(*catalog->items));
		if (items == NULL) {
			free(copy);
			return -1;
		}
		catalog->items = items;
		catalog->capacity = capacity;
	}
	message = &catalog->items[catalog->count++];
	message->set = set;
	message->number = number;
	message->text = copy;
	return 0;
}

static void
catalog_delete(struct catalog *catalog, uint32_t set, uint32_t number)
{
	struct message *message = catalog_find(catalog, set, number);

	if (message == NULL)
		return;
	free(message->text);
	*message = catalog->items[--catalog->count];
}

static void
catalog_delete_set(struct catalog *catalog, uint32_t set)
{
	size_t index = 0;

	while (index < catalog->count) {
		if (catalog->items[index].set == set) {
			free(catalog->items[index].text);
			catalog->items[index] =
			    catalog->items[--catalog->count];
		} else
			index++;
	}
}

static int
message_compare(const void *left_pointer, const void *right_pointer)
{
	const struct message *left = left_pointer;
	const struct message *right = right_pointer;

	if (left->set != right->set)
		return left->set < right->set ? -1 : 1;
	if (left->number != right->number)
		return left->number < right->number ? -1 : 1;
	return 0;
}

static int
read_all(int descriptor, unsigned char *data, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t count = read(descriptor, data + done, size - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		done += (size_t)count;
	}
	return 0;
}

static int
catalog_load(struct catalog *catalog, const char *path)
{
	unsigned char *data;
	off_t end;
	uint32_t count;
	uint32_t entries;
	uint32_t strings;
	uint32_t index;
	int descriptor;

	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (descriptor < 0)
		return errno == ENOENT ? 0 : -1;
	end = lseek(descriptor, 0, SEEK_END);
	if (end < 0 || (uint64_t)end > UINT32_MAX ||
	    lseek(descriptor, 0, SEEK_SET) != 0) {
		(void)close(descriptor);
		errno = EINVAL;
		return -1;
	}
	data = malloc(end != 0 ? (size_t)end : 1U);
	if (data == NULL) {
		(void)close(descriptor);
		return -1;
	}
	if (read_all(descriptor, data, (size_t)end) != 0 ||
	    close(descriptor) != 0)
		goto failed;
	if ((size_t)end < ZEDBSD_CATALOG_HEADER_SIZE ||
	    memcmp(data, ZEDBSD_CATALOG_MAGIC, ZEDBSD_CATALOG_MAGIC_SIZE) !=
		0 ||
	    zedbsd_catalog_get32(data + 8U) != ZEDBSD_CATALOG_VERSION ||
	    zedbsd_catalog_get32(data + 12U) != ZEDBSD_CATALOG_HEADER_SIZE)
		goto invalid;
	count = zedbsd_catalog_get32(data + 16U);
	entries = zedbsd_catalog_get32(data + 20U);
	strings = zedbsd_catalog_get32(data + 24U);
	if (count > UINT32_MAX / ZEDBSD_CATALOG_ENTRY_SIZE ||
	    entries > (uint32_t)end || strings > (uint32_t)end ||
	    count * ZEDBSD_CATALOG_ENTRY_SIZE > (uint32_t)end - entries ||
	    strings < entries + count * ZEDBSD_CATALOG_ENTRY_SIZE)
		goto invalid;
	for (index = 0; index < count; index++) {
		const unsigned char *entry =
		    data + entries + index * ZEDBSD_CATALOG_ENTRY_SIZE;
		uint32_t set = zedbsd_catalog_get32(entry);
		uint32_t number = zedbsd_catalog_get32(entry + 4U);
		uint32_t offset = zedbsd_catalog_get32(entry + 8U);
		uint32_t length = zedbsd_catalog_get32(entry + 12U);

		if (set == 0 || number == 0 || offset < strings ||
		    length == UINT32_MAX || offset > (uint32_t)end ||
		    length + 1U > (uint32_t)end - offset ||
		    data[offset + length] != '\0' ||
		    catalog_set(catalog, set, number, (char *)data + offset) !=
			0)
			goto invalid;
	}
	free(data);
	return 0;

invalid:
	errno = EINVAL;
failed:
	free(data);
	return -1;
}

static int
catalog_encode(struct catalog *catalog, unsigned char **result, size_t *size)
{
	size_t strings = 0;
	size_t total;
	size_t offset;
	size_t index;
	unsigned char *data;

	qsort(catalog->items, catalog->count, sizeof(*catalog->items),
	      message_compare);
	for (index = 0; index < catalog->count; index++) {
		size_t length = strlen(catalog->items[index].text) + 1U;

		if (length > UINT32_MAX - strings) {
			errno = EFBIG;
			return -1;
		}
		strings += length;
	}
	if (catalog->count > UINT32_MAX / ZEDBSD_CATALOG_ENTRY_SIZE ||
	    catalog->count * ZEDBSD_CATALOG_ENTRY_SIZE >
		UINT32_MAX - ZEDBSD_CATALOG_HEADER_SIZE ||
	    strings > UINT32_MAX - ZEDBSD_CATALOG_HEADER_SIZE -
			  catalog->count * ZEDBSD_CATALOG_ENTRY_SIZE) {
		errno = EFBIG;
		return -1;
	}
	total = ZEDBSD_CATALOG_HEADER_SIZE +
		catalog->count * ZEDBSD_CATALOG_ENTRY_SIZE + strings;
	data = calloc(1, total != 0 ? total : 1U);
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
	for (index = 0; index < catalog->count; index++) {
		unsigned char *entry = data + ZEDBSD_CATALOG_HEADER_SIZE +
				       index * ZEDBSD_CATALOG_ENTRY_SIZE;
		size_t length = strlen(catalog->items[index].text);

		zedbsd_catalog_put32(entry, catalog->items[index].set);
		zedbsd_catalog_put32(entry + 4U, catalog->items[index].number);
		zedbsd_catalog_put32(entry + 8U, (uint32_t)offset);
		zedbsd_catalog_put32(entry + 12U, (uint32_t)length);
		memcpy(data + offset, catalog->items[index].text, length + 1U);
		offset += length + 1U;
	}
	*result = data;
	*size = total;
	return 0;
}

static char *
logical_line(FILE *stream, unsigned long *line_number)
{
	char *line = NULL;
	size_t used = 0;
	size_t capacity = 0;
	int character;

	for (;;) {
		character = fgetc(stream);
		if (character == EOF) {
			if (used == 0) {
				free(line);
				return NULL;
			}
			break;
		}
		if (used + 2U > capacity) {
			size_t wanted = capacity == 0 ? 128U : capacity * 2U;
			char *replacement;

			if (wanted < capacity) {
				free(line);
				errno = ENOMEM;
				return NULL;
			}
			replacement = realloc(line, wanted);
			if (replacement == NULL) {
				free(line);
				return NULL;
			}
			line = replacement;
			capacity = wanted;
		}
		if (character == '\n') {
			(*line_number)++;
			if (used != 0 && line[used - 1U] == '\\') {
				used--;
				continue;
			}
			break;
		}
		line[used++] = (char)character;
	}
	line[used] = '\0';
	return line;
}

static int
positive_number(const char *text, char **end, uint32_t *result)
{
	unsigned long value;

	if (!isdigit((unsigned char)*text))
		return 0;
	errno = 0;
	value = strtoul(text, end, 10);
	if (errno == ERANGE || value == 0 || value > UINT32_MAX)
		return 0;
	*result = (uint32_t)value;
	return 1;
}

static char *
message_decode(const char *text, int quote)
{
	char *result = malloc(strlen(text) + 1U);
	char *output = result;
	int quoted = quote != 0 && (unsigned char)*text == (unsigned char)quote;

	if (result == NULL)
		return NULL;
	if (quoted)
		text++;
	while (*text != '\0') {
		unsigned value = 0;
		unsigned digits = 0;

		if (quoted && (unsigned char)*text == (unsigned char)quote) {
			text++;
			while (isspace((unsigned char)*text))
				text++;
			if (*text != '\0')
				goto invalid;
			*output = '\0';
			return result;
		}
		if (*text != '\\') {
			*output++ = *text++;
			continue;
		}
		text++;
		if (*text >= '0' && *text <= '7') {
			while (digits < 3U && *text >= '0' && *text <= '7') {
				value = value * 8U + (unsigned)(*text++ - '0');
				digits++;
			}
			if (value == 0)
				goto invalid;
			*output++ = (char)(unsigned char)value;
			continue;
		}
		if (*text == '\0')
			goto invalid;
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
	if (quoted)
		goto invalid;
	*output = '\0';
	return result;

invalid:
	free(result);
	errno = EINVAL;
	return NULL;
}

static int
parse_file(struct catalog *catalog, FILE *stream, const char *path)
{
	uint32_t current_set = NL_SETD;
	unsigned long line_number = 1;
	int quote = 0;
	int failed = 0;

	for (;;) {
		unsigned long source_line = line_number;
		char *line = logical_line(stream, &line_number);
		char *cursor;
		uint32_t number;

		if (line == NULL)
			break;
		cursor = line;
		while (isspace((unsigned char)*cursor))
			cursor++;
		if (*cursor == '\0') {
			free(line);
			continue;
		}
		if (*cursor == '$') {
			cursor++;
			if (!strncmp(cursor, "set", 3) &&
			    isspace((unsigned char)cursor[3])) {
				char *end;

				cursor += 3;
				while (isspace((unsigned char)*cursor))
					cursor++;
				if (!positive_number(cursor, &end, &number) ||
				    (*end != '\0' &&
				     !isspace((unsigned char)*end)))
					goto invalid;
				current_set = number;
			} else if (!strncmp(cursor, "delset", 6) &&
				   isspace((unsigned char)cursor[6])) {
				char *end;

				cursor += 6;
				while (isspace((unsigned char)*cursor))
					cursor++;
				if (!positive_number(cursor, &end, &number))
					goto invalid;
				while (isspace((unsigned char)*end))
					end++;
				if (*end != '\0')
					goto invalid;
				catalog_delete_set(catalog, number);
			} else if (!strncmp(cursor, "quote", 5) &&
				   (cursor[5] == '\0' ||
				    isspace((unsigned char)cursor[5]))) {
				cursor += 5;
				while (isspace((unsigned char)*cursor))
					cursor++;
				quote = (unsigned char)*cursor;
				if (*cursor != '\0' && cursor[1] != '\0')
					goto invalid;
			}
			free(line);
			continue;
		}
		{
			char *end;
			char *decoded;

			if (!positive_number(cursor, &end, &number) ||
			    (*end != '\0' && !isspace((unsigned char)*end)))
				goto invalid;
			while (isspace((unsigned char)*end))
				end++;
			if (*end == '\0') {
				catalog_delete(catalog, current_set, number);
				free(line);
				continue;
			}
			decoded = message_decode(end, quote);
			if (decoded == NULL ||
			    catalog_set(catalog, current_set, number,
					decoded) != 0) {
				free(decoded);
				goto invalid;
			}
			free(decoded);
			free(line);
			continue;
		}

	invalid:
		fprintf(stderr, "gencat: %s:%lu: invalid message source\n",
			path, source_line);
		free(line);
		failed = 1;
	}
	if (ferror(stream)) {
		fprintf(stderr, "gencat: %s: %s\n", path, strerror(errno));
		failed = 1;
	}
	return failed ? -1 : 0;
}

static int
catalog_write(const char *path, const unsigned char *data, size_t size)
{
	char temporary[PATH_MAX + 1U];
	int descriptor;
	int length;
	int operation_error = 0;
	int saved_errno;

	if (!strcmp(path, "-"))
		return command_write_all(STDOUT_FILENO, data, size);
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
		operation_error = errno;
	if (close(descriptor) != 0 && operation_error == 0)
		operation_error = errno;
	if (operation_error == 0 && rename(temporary, path) == 0)
		return 0;
	saved_errno = operation_error != 0 ? operation_error : errno;
	(void)unlink(temporary);
	errno = saved_errno;
	return -1;
}

int
main(int argc, char **argv)
{
	struct catalog catalog = {0};
	unsigned char *encoded = NULL;
	size_t encoded_size = 0;
	int failed = 0;
	int index;

	if (argc < 3) {
		fprintf(stderr, "usage: gencat catfile msgfile ...\n");
		return 2;
	}
	if (strcmp(argv[1], "-") != 0 && catalog_load(&catalog, argv[1]) != 0) {
		fprintf(stderr, "gencat: %s: %s\n", argv[1], strerror(errno));
		failed = 1;
		goto done;
	}
	for (index = 2; index < argc; index++) {
		FILE *stream =
		    !strcmp(argv[index], "-") ? stdin : fopen(argv[index], "r");

		if (stream == NULL) {
			fprintf(stderr, "gencat: %s: %s\n", argv[index],
				strerror(errno));
			failed = 1;
			continue;
		}
		if (parse_file(&catalog, stream, argv[index]) != 0)
			failed = 1;
		if (stream != stdin && fclose(stream) != 0)
			failed = 1;
	}
	if (!failed && catalog_encode(&catalog, &encoded, &encoded_size) != 0) {
		fprintf(stderr, "gencat: %s\n", strerror(errno));
		failed = 1;
	}
	if (!failed && catalog_write(argv[1], encoded, encoded_size) != 0) {
		fprintf(stderr, "gencat: %s: %s\n", argv[1], strerror(errno));
		failed = 1;
	}
done:
	free(encoded);
	catalog_free(&catalog);
	return failed;
}
