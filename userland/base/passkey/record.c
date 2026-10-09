/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The records of /etc/passkey (passkey.h; ws172-p002):
 *
 *   # zedBSD passkey 1
 *   <name>:<uid>:pin:<SHA-512 crypt hash>
 *   <name>:<uid>:fido2:<id>:<COSE key>:<count>:<relying party>:<label>:<date>
 *
 * A line counts for an account only while both its name and its user ID
 * are the account's.  Lines of other kinds, comments and malformed lines
 * are kept as they are when the file is rewritten.
 */

#include "passkey.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int record_line_is(const char *line, size_t length, const char *name, const char *uid, const char *kind);
static int record_append(char *output, size_t capacity, size_t *used, const char *text, size_t length);
static int record_rewrite(const char *text, size_t length, const char *name, const char *kind, const char *field, int every,
    const char *added, char *output, size_t capacity, size_t *written);
static int record_field_is(const char *line, size_t length, const char *field);

/* Gives the version the file's first line names (1 for a file without it, or an empty one). */
int
passkey_record_version(
	const char *text,
	size_t length)
{
	size_t header;

	/* The header and its number. */
	header = strlen(PASSKEY_HEADER);
	if (length < header || strncmp(text, PASSKEY_HEADER, header) != 0)
		return PASSKEY_VERSION;
	return atoi(text + header);
}

/*
 * Copies the index-th line (from 0) of an account's lines of a kind,
 * without its line end.  Returns 0, ENOENT, or ENAMETOOLONG.
 */
int
passkey_record_find(
	const char *text,
	size_t length,
	const char *name,
	uid_t uid,
	const char *kind,
	unsigned index,
	char *line,
	size_t size)
{
	char number[24];
	const char *start;
	const char *end;
	size_t line_length;
	unsigned found;

	/* Each line. */
	snprintf(number, sizeof(number), "%u", (unsigned)uid);
	found = 0U;
	start = text;
	while (start < text + length) {
		end = memchr(start, '\n', (size_t)(text + length - start));
		if (end == NULL)
			end = text + length;
		line_length = (size_t)(end - start);

		/* The account's line of the kind. */
		if (record_line_is(start, line_length, name, number, kind)) {
			if (found == index) {
				if (line_length >= size)
					return ENAMETOOLONG;
				memcpy(line, start, line_length);
				line[line_length] = '\0';
				return 0;
			}
			found++;
		}
		start = end + 1;
	}

	/* None. */
	return ENOENT;
}

/* Counts an account's lines of a kind. */
int
passkey_record_count(
	const char *text,
	size_t length,
	const char *name,
	uid_t uid,
	const char *kind)
{
	char line[PASSKEY_REQUEST_MAX];
	int count;

	/* Each one found. */
	for (count = 0; passkey_record_find(text, length, name, uid, kind, (unsigned)count, line, sizeof(line)) == 0; count++)
		continue;
	return count;
}

/*
 * Writes the file again: the header first, every line kept but the lines
 * of the name (whatever their user ID) of the kind (every kind when kind
 * is NULL), and added (a whole line without its end) at the end when it is
 * not NULL.  Returns 0 or ENOSPC.
 */
int
passkey_record_replace(
	const char *text,
	size_t length,
	const char *name,
	const char *kind,
	const char *added,
	char *output,
	size_t capacity,
	size_t *written)
{
	int error;

	/* Every line of the name and kind goes. */
	error = record_rewrite(text, length, name, kind, NULL, 1, added, output, capacity, written);
	if (error != 0)
		return error;

	/* Succeeded: the new text. */
	return 0;
}

/*
 * Writes the file again for one credential (ws172-p003): every line kept
 * but the name's lines of the kind whose first field after the kind is
 * field (none when field is NULL), and added at the end when it is not
 * NULL.  So a security key's line is added (field NULL), its count changed
 * (field its ID, added its new line) or removed (added NULL).  Returns 0
 * or ENOSPC.
 */
int
passkey_record_edit(
	const char *text,
	size_t length,
	const char *name,
	const char *kind,
	const char *field,
	const char *added,
	char *output,
	size_t capacity,
	size_t *written)
{
	int error;

	/* Only the credential's line goes. */
	error = record_rewrite(text, length, name, kind, field, 0, added, output, capacity, written);
	if (error != 0)
		return error;

	/* Succeeded: the new text. */
	return 0;
}

/*
 * Writes the header, the lines kept and added: a line of the name and kind
 * goes when every is set, or when its field after the kind is field.
 */
static int
record_rewrite(
	const char *text,
	size_t length,
	const char *name,
	const char *kind,
	const char *field,
	int every,
	const char *added,
	char *output,
	size_t capacity,
	size_t *written)
{
	char header[64];
	const char *start;
	const char *end;
	size_t line_length;
	size_t used;
	size_t header_length;
	int is_header;
	int dropped;
	int error;

	/* The header. */
	used = 0U;
	snprintf(header, sizeof(header), "%s%d\n", PASSKEY_HEADER, PASSKEY_VERSION);
	error = record_append(output, capacity, &used, header, strlen(header));
	if (error != 0)
		return error;

	/* The lines kept (the old header is written again above). */
	header_length = strlen(PASSKEY_HEADER);
	start = text;
	while (start < text + length) {
		end = memchr(start, '\n', (size_t)(text + length - start));
		if (end == NULL)
			end = text + length;
		line_length = (size_t)(end - start);

		/* An empty line and the old header are not kept. */
		is_header = line_length >= header_length && strncmp(start, PASSKEY_HEADER, header_length) == 0;
		if (line_length == 0U || is_header) {
			start = end + 1;
			continue;
		}

		/* The lines that go. */
		dropped = record_line_is(start, line_length, name, NULL, kind);
		if (dropped && !every)
			dropped = field != NULL && record_field_is(start, line_length, field);
		if (!dropped) {
			error = record_append(output, capacity, &used, start, line_length);
			if (error == 0)
				error = record_append(output, capacity, &used, "\n", 1U);
			if (error != 0)
				return error;
		}

		/* The next line. */
		start = end + 1;
	}

	/* The new line. */
	if (added != NULL) {
		error = record_append(output, capacity, &used, added, strlen(added));
		if (error == 0)
			error = record_append(output, capacity, &used, "\n", 1U);
		if (error != 0)
			return error;
	}

	/* Succeeded: the new text. */
	*written = used;
	return 0;
}

/* Tells whether a line's fourth field (the first after name, uid and kind) is field. */
static int
record_field_is(
	const char *line,
	size_t length,
	const char *field)
{
	const char *start;
	const char *end;
	unsigned colons;
	size_t field_length;

	/* After the third colon. */
	start = line;
	for (colons = 0U; colons < 3U; colons++) {
		start = memchr(start, ':', (size_t)(line + length - start));
		if (start == NULL)
			return 0;
		start++;
	}

	/* Up to the next colon or the end. */
	end = memchr(start, ':', (size_t)(line + length - start));
	if (end == NULL)
		end = line + length;
	field_length = strlen(field);
	if ((size_t)(end - start) != field_length || memcmp(start, field, field_length) != 0)
		return 0;

	/* The field is the one. */
	return 1;
}

/*
 * Copies a line's field (from 0: name, uid, kind, then the kind's own)
 * without its colons.  Returns 0, ENOENT for a line with fewer fields, or
 * ENAMETOOLONG.
 */
int
passkey_record_field(
	const char *line,
	unsigned index,
	char *field,
	size_t size)
{
	const char *start;
	const char *end;
	unsigned colons;
	size_t length;

	/* After the index-th colon. */
	start = line;
	for (colons = 0U; colons < index; colons++) {
		start = strchr(start, ':');
		if (start == NULL)
			return ENOENT;
		start++;
	}

	/* Up to the next colon or the end. */
	end = strchr(start, ':');
	if (end == NULL)
		end = start + strlen(start);
	length = (size_t)(end - start);
	if (length >= size)
		return ENAMETOOLONG;
	memcpy(field, start, length);
	field[length] = '\0';

	/* Succeeded: the field. */
	return 0;
}

/*
 * Writes a key's reference (ws172-p003): the 64-bit FNV-1a hash of its
 * credential ID's text, as 16 small hexadecimal digits.  It names one of
 * an account's few keys in a short line (Settings' list, remove-fido2);
 * it is not a secret and protects nothing.
 */
void
passkey_record_ref(
	const char *id,
	char *ref,
	size_t size)
{
	uint64_t hash;
	size_t index;

	/* FNV-1a over the bytes. */
	hash = 0xcbf29ce484222325ULL;
	for (index = 0U; id[index] != '\0'; index++) {
		hash ^= (uint64_t)(unsigned char)id[index];
		hash *= 0x100000001b3ULL;
	}

	/* Its digits. */
	(void)snprintf(ref, size, "%016llx", (unsigned long long)hash);
}

/*
 * Tells whether a line is name's (and uid's, unless uid is NULL) of a kind
 * (any kind when kind is NULL).
 */
static int
record_line_is(
	const char *line,
	size_t length,
	const char *name,
	const char *uid,
	const char *kind)
{
	const char *colon;
	const char *second;
	const char *third;
	size_t name_length;

	/* name: */
	name_length = strlen(name);
	if (length <= name_length || memcmp(line, name, name_length) != 0 || line[name_length] != ':')
		return 0;

	/* uid: */
	colon = line + name_length;
	second = memchr(colon + 1, ':', (size_t)(line + length - colon - 1));
	if (second == NULL)
		return 0;
	if (uid != NULL && ((size_t)(second - colon - 1) != strlen(uid) || memcmp(colon + 1, uid, strlen(uid)) != 0))
		return 0;

	/* kind: (or the end) */
	if (kind == NULL)
		return 1;
	third = memchr(second + 1, ':', (size_t)(line + length - second - 1));
	if (third == NULL)
		third = line + length;
	if ((size_t)(third - second - 1) != strlen(kind) || memcmp(second + 1, kind, strlen(kind)) != 0)
		return 0;

	/* The account's line of the kind. */
	return 1;
}

/* Appends bytes; returns 0 or ENOSPC. */
static int
record_append(
	char *output,
	size_t capacity,
	size_t *used,
	const char *text,
	size_t length)
{
	if (length > capacity - *used)
		return ENOSPC;
	memcpy(output + *used, text, length);
	*used += length;
	return 0;
}

/* Sets an account's options to the defaults: every method, the key's PIN and touch asked (ws199-p001). */
void
passkey_options_default(
	struct passkey_options *options)
{
	/* The strongest. */
	memset(options, 0, sizeof(*options));
	snprintf(options->methods, sizeof(options->methods), "%s", PASSKEY_METHODS_DEFAULT);
	options->key_pin = 1;
	options->key_touch = 1;
}

/*
 * Reads an account's options line (ws199-p001 section 2): the defaults
 * when it has none, two or more, or one that does not read.  Returns 1
 * when a line was taken, 0 when the defaults are.
 */
int
passkey_options_read(
	const char *text,
	size_t length,
	const char *name,
	uid_t uid,
	struct passkey_options *options)
{
	char line[PASSKEY_FIELD_MAX];
	char field[PASSKEY_METHODS_MAX + 16U];
	struct passkey_options read;
	unsigned index;
	int count;
	int error;
	int same;
	int fits;

	/* The defaults, unless exactly one line reads. */
	passkey_options_default(options);
	count = passkey_record_count(text, length, name, uid, "options");
	if (count != 1)
		return 0;
	error = passkey_record_find(text, length, name, uid, "options", 0U, line, sizeof(line));
	if (error != 0)
		return 0;

	/* Its three fields after the kind, each a word=value. */
	passkey_options_default(&read);
	read.key_pin = -1;
	read.key_touch = -1;
	read.methods[0] = '\0';
	for (index = 3U; index < 6U; index++) {
		error = passkey_record_field(line, index, field, sizeof(field));
		if (error != 0)
			return 0;
		same = strncmp(field, "methods=", 8U);
		fits = strlen(field + 8) < sizeof(read.methods);
		if (same == 0 && fits) {
			snprintf(read.methods, sizeof(read.methods), "%.*s", (int)(sizeof(read.methods) - 1U), field + 8);
			continue;
		}

		/* key-pin=. */
		same = strcmp(field, "key-pin=0") == 0 || strcmp(field, "key-pin=1") == 0;
		if (same) {
			read.key_pin = field[8] - '0';
			continue;
		}

		/* key-touch=. */
		same = strcmp(field, "key-touch=0") == 0 || strcmp(field, "key-touch=1") == 0;
		if (same) {
			read.key_touch = field[10] - '0';
			continue;
		}

		/* A field that is none of them. */
		return 0;
	}

	/* Each field once, and no touch left out while the PIN is asked. */
	if (read.methods[0] == '\0' || read.key_pin < 0 || read.key_touch < 0)
		return 0;
	if (read.key_touch == 0 && read.key_pin == 1)
		return 0;

	/* Succeeded: the line's. */
	*options = read;
	return 1;
}

/* Writes an account's options line (without its end).  Returns 0, or ENAMETOOLONG. */
int
passkey_options_line(
	const char *name,
	uid_t uid,
	const struct passkey_options *options,
	char *line,
	size_t size)
{
	int written;

	/* The line. */
	written = snprintf(line, size, "%s:%u:options:methods=%s:key-pin=%d:key-touch=%d", name, (unsigned)uid, options->methods,
	    options->key_pin != 0, options->key_touch != 0);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded. */
	return 0;
}

/* Tells whether options are the defaults (their line need not be kept). */
int
passkey_options_is_default(
	const struct passkey_options *options)
{
	int same;

	/* Every method, the PIN and the touch. */
	same = strcmp(options->methods, PASSKEY_METHODS_DEFAULT);
	return same == 0 && options->key_pin && options->key_touch;
}
