/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reader of the record files vectors.py makes from the published
 * test vectors ($WS335_VECTORS/rec/<name>.vec): records of "key = value"
 * lines separated by blank lines; lines starting with '#' are comments.
 *
 *   struct vec_file file;
 *   if (vec_open(&file, "wycheproof-x25519_test") != 0) ... (a FAIL: the
 *       vectors must be fetched first)
 *   while (vec_next(&file)) {
 *       const char *result = vec_get(&file, "result");
 *       size_t n = vec_hex(&file, "shared", buffer, sizeof(buffer));
 *   }
 *   vec_close(&file);
 *
 * Lines may be long (a certificate in hex); the reader grows its buffer.
 */

#ifndef WS335_TESTS_VECFILE_H
#define WS335_TESTS_VECFILE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

/* The most fields of one record (a limbo case has a few dozen). */
#define VEC_FIELDS	128

/*
 * An open record file and its current record: text holds the record's
 * lines (each key and value NUL-terminated in place), keys and values
 * point into it, count fields.
 */
struct vec_file {
	FILE *stream;
	char *text;
	size_t size;
	size_t used;
	const char *keys[VEC_FIELDS];
	const char *values[VEC_FIELDS];
	size_t count;
	unsigned long records;
};

/* Opens $WS335_VECTORS/rec/<name>.vec; returns 0, or -1 (printed) when it is missing. */
static int __attribute__((unused))
vec_open(
	struct vec_file *file,
	const char *name)
{
	const char *directory;
	char path[1024];

	/* The cache, fetched by fetch.sh. */
	memset(file, 0, sizeof(*file));
	directory = getenv("WS335_VECTORS");
	if (directory == NULL)
		directory = "build/ws335-vectors";
	snprintf(path, sizeof(path), "%s/rec/%s.vec", directory, name);
	file->stream = fopen(path, "r");
	if (file->stream == NULL) {
		printf("FAIL cannot open %s (run plan/ws335/tests/fetch.sh vectors)\n", path);
		return -1;
	}
	return 0;
}

/* Reads one line into the text buffer at file->used; returns its length, or -1 at the end. */
static long __attribute__((unused))
vec_line(
	struct vec_file *file)
{
	int character;
	size_t start;

	/* Characters up to the newline, growing the buffer. */
	start = file->used;
	for (;;) {
		character = fgetc(file->stream);
		if (character == EOF && file->used == start)
			return -1;
		if (character == EOF || character == '\n')
			break;
		if (file->used + 2U > file->size) {
			file->size = file->size == 0U ? 4096U : file->size * 2U;
			file->text = realloc(file->text, file->size);
			if (file->text == NULL) {
				printf("FAIL out of memory reading vectors\n");
				exit(EXIT_FAILURE);
			}
		}
		file->text[file->used++] = (char)character;
	}
	if (file->used + 1U > file->size) {
		file->size = file->size == 0U ? 4096U : file->size * 2U;
		file->text = realloc(file->text, file->size);
		if (file->text == NULL) {
			printf("FAIL out of memory reading vectors\n");
			exit(EXIT_FAILURE);
		}
	}
	file->text[file->used++] = '\0';
	return (long)(file->used - start - 1U);
}

/* Reads the next record; returns 1, or 0 at the end of the file. */
static int __attribute__((unused))
vec_next(
	struct vec_file *file)
{
	size_t offsets[VEC_FIELDS];
	size_t start;
	size_t index;
	long length;
	char *line;
	char *equals;

	/* Lines until a blank line after at least one field. */
	file->used = 0;
	file->count = 0;
	for (;;) {
		start = file->used;
		length = vec_line(file);
		if (length < 0)
			break;
		if (length == 0) {
			file->used = start;
			if (file->count > 0)
				break;
			continue;
		}
		if (file->text[start] == '#') {
			file->used = start;
			continue;
		}
		if (file->count < VEC_FIELDS)
			offsets[file->count++] = start;
	}
	if (file->count == 0)
		return 0;

	/* Split each line at " = " (pointers are taken now: the buffer no longer moves). */
	for (index = 0; index < file->count; index++) {
		line = file->text + offsets[index];
		equals = strstr(line, " =");
		file->keys[index] = line;
		file->values[index] = "";
		if (equals != NULL) {
			*equals = '\0';
			file->values[index] = equals[2] == ' ' ? equals + 3 : equals + 2;
		}
	}
	file->records++;
	return 1;
}

/* Returns the value of key in the current record, or NULL. */
static const char * __attribute__((unused))
vec_get(
	const struct vec_file *file,
	const char *key)
{
	size_t index;

	/* The first field with the key. */
	for (index = 0; index < file->count; index++)
		if (strcmp(file->keys[index], key) == 0)
			return file->values[index];
	return NULL;
}

/* Decodes the hex value of key into out; returns its size, or (size_t)-1 when absent or malformed. */
static size_t __attribute__((unused))
vec_hex(
	const struct vec_file *file,
	const char *key,
	unsigned char *out,
	size_t room)
{
	const char *value;

	/* The field, as hex. */
	value = vec_get(file, key);
	if (value == NULL)
		return (size_t)-1;
	return hex_decode(value, out, room);
}

/* Closes the file and frees the buffer. */
static void __attribute__((unused))
vec_close(
	struct vec_file *file)
{
	/* Both may be absent after a failed open. */
	if (file->stream != NULL)
		fclose(file->stream);
	free(file->text);
	memset(file, 0, sizeof(*file));
}

#endif
