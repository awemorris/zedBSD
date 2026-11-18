/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The structure of a message as IMAP's BODYSTRUCTURE gives it (RFC 3501
 * 7.4.2; ws177-p016; mail.h), for a message larger than Mail fetches
 * whole: the section, type, character set and transfer encoding of its
 * first text/plain part and of its first text/html part, and the name and
 * size of the first file it carries, so that only the words are fetched
 * and the file's size is the real one.
 *
 * The text is read as IMAP's values: a list in parentheses, a quoted
 * string, NIL, or an atom or a number.  A literal in it is not read (the
 * caller then fetches the message as before).
 */

#include "mail.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How deep multiparts are followed. */
#define STRUCTURE_DEPTH_MAX	8

/* The longest value kept, with its NUL. */
#define STRUCTURE_VALUE_MAX	256U

/* What a read of the structure's text carries: the text, where it is, and whether something did not read. */
struct structure_reader {
	const char *text;
	size_t at;
	int failed;
};

static void structure_body(struct structure_reader *reader, const char *section, int depth, struct ml_structure *structure);
static void structure_part(struct structure_reader *reader, const char *section, struct ml_structure *structure);
static void structure_parameters(struct structure_reader *reader, char *charset, size_t charset_size, char *name, size_t name_size);
static void structure_disposition(struct structure_reader *reader, int *attachment, char *name, size_t name_size);
static int structure_value(struct structure_reader *reader, char *value, size_t size);
static void structure_skip(struct structure_reader *reader);
static void structure_spaces(struct structure_reader *reader);
static int structure_open(struct structure_reader *reader);
static int structure_close(struct structure_reader *reader);
static void structure_lower(char *text);
static void structure_copy(char *to, size_t size, const char *from);

/*
 * Reads a BODYSTRUCTURE's value (from its opening parenthesis).  Returns
 * 0 with the structure, or EPROTO when it does not read.
 */
int
ml_structure_parse(
	const char *text,
	struct ml_structure *structure)
{
	struct structure_reader reader;

	/* Nothing yet. */
	memset(structure, 0, sizeof(*structure));
	reader.text = text;
	reader.at = 0;
	reader.failed = 0;

	/* The message's body, section "" at the top. */
	structure_spaces(&reader);
	structure_body(&reader, "", 0, structure);
	if (reader.failed)
		return EPROTO;

	/* Succeeded: the structure is read. */
	return 0;
}

/* Reads one body: a multipart's list of bodies and its subtype, or one part. */
static void
structure_body(
	struct structure_reader *reader,
	const char *section,
	int depth,
	struct ml_structure *structure)
{
	char inner[sizeof(structure->text.section)];
	int opened;
	int number;

	/* Its parenthesis. */
	opened = structure_open(reader);
	if (!opened || depth > STRUCTURE_DEPTH_MAX) {
		reader->failed = 1;
		return;
	}

	/* One part: its fields (a lone part at the top is section 1). */
	structure_spaces(reader);
	if (reader->text[reader->at] != '(') {
		if (section[0] == '\0') {
			structure_part(reader, "1", structure);
			return;
		}

		/* A part inside a multipart, by its section. */
		structure_part(reader, section, structure);
		return;
	}

	/* A multipart: each body in it, numbered from 1 under its section. */
	number = 1;
	while (!reader->failed && reader->text[reader->at] == '(') {
		if (section[0] == '\0') {
			(void)snprintf(inner, sizeof(inner), "%d", number);
		} else {
			(void)snprintf(inner, sizeof(inner), "%s.%d", section, number);
		}

		/* The body under that section. */
		structure_body(reader, inner, depth + 1, structure);
		structure_spaces(reader);
		number++;
	}

	/* Its subtype and extensions, up to its parenthesis. */
	while (!reader->failed && reader->text[reader->at] != ')') {
		structure_skip(reader);
		structure_spaces(reader);
	}

	/* The end of the list. */
	(void)structure_close(reader);
}

/* Reads one part's fields (its opening parenthesis taken) and notes what it is. */
static void
structure_part(
	struct structure_reader *reader,
	const char *section,
	struct ml_structure *structure)
{
	char type[STRUCTURE_VALUE_MAX];
	char subtype[STRUCTURE_VALUE_MAX];
	char charset[STRUCTURE_VALUE_MAX];
	char name[STRUCTURE_VALUE_MAX];
	char encoding[STRUCTURE_VALUE_MAX];
	char size_text[STRUCTURE_VALUE_MAX];
	struct ml_structure_part *part;
	size_t size;
	int attachment;
	int is_text;
	int same;
	int field;

	/* Its type and subtype, its parameters, its ID and description (not kept), its encoding and size. */
	(void)structure_value(reader, type, sizeof(type));
	(void)structure_value(reader, subtype, sizeof(subtype));
	structure_parameters(reader, charset, sizeof(charset), name, sizeof(name));
	structure_skip(reader);
	structure_skip(reader);
	(void)structure_value(reader, encoding, sizeof(encoding));
	(void)structure_value(reader, size_text, sizeof(size_text));
	size = (size_t)strtoul(size_text, NULL, 10);
	structure_lower(type);
	structure_lower(subtype);
	structure_lower(encoding);

	/* The extensions: a text's lines, a message's envelope, body and lines, then MD5 and the disposition. */
	same = strcmp(type, "text");
	is_text = same == 0;
	attachment = 0;
	field = 0;
	structure_spaces(reader);
	while (!reader->failed && reader->text[reader->at] != ')' && reader->text[reader->at] != '\0') {
		/* The disposition is the field after MD5 (after a text's lines; a message's three fields are skipped too). */
		same = strcmp(type, "message");
		if ((is_text && field == 2) || (same == 0 && field == 4) || (!is_text && same != 0 && field == 1)) {
			structure_disposition(reader, &attachment, name, sizeof(name));
		} else {
			structure_skip(reader);
		}

		/* The next field. */
		structure_spaces(reader);
		field++;
	}

	/* The end of the part. */
	(void)structure_close(reader);
	if (reader->failed)
		return;

	/* The first plain text or HTML that is not attached. */
	part = NULL;
	if (is_text && !attachment) {
		same = strcmp(subtype, "plain");
		if (same == 0 && structure->text.section[0] == '\0')
			part = &structure->text;
		same = strcmp(subtype, "html");
		if (same == 0 && structure->html.section[0] == '\0')
			part = &structure->html;
	}

	/* Its section, type, character set and encoding. */
	if (part != NULL) {
		structure_copy(part->section, sizeof(part->section), section);
		(void)snprintf(part->type, sizeof(part->type), "%.30s/%.30s", type, subtype);
		structure_copy(part->charset, sizeof(part->charset), charset);
		structure_copy(part->encoding, sizeof(part->encoding), encoding);
		return;
	}

	/* The first file: its name and its size as decoded. */
	if (structure->file_name[0] != '\0')
		return;
	if (name[0] == '\0')
		structure_copy(name, sizeof(name), "attachment");
	structure_copy(structure->file_name, sizeof(structure->file_name), name);

	/* Base64 is four characters for three bytes, in lines of 76 characters and a line end. */
	same = strcmp(encoding, "base64");
	if (same == 0)
		size = (size - size / 78U * 2U) / 4U * 3U;
	structure->file_size = size;
}

/* Reads a part's parameters (NIL, or a list of names and values): its charset and its name. */
static void
structure_parameters(
	struct structure_reader *reader,
	char *charset,
	size_t charset_size,
	char *name,
	size_t name_size)
{
	char key[STRUCTURE_VALUE_MAX];
	char value[STRUCTURE_VALUE_MAX];
	int opened;
	int same;

	/* None yet; NIL is none. */
	charset[0] = '\0';
	name[0] = '\0';
	structure_spaces(reader);
	opened = structure_open(reader);
	if (!opened) {
		structure_skip(reader);
		return;
	}

	/* Each name and its value. */
	structure_spaces(reader);
	while (!reader->failed && reader->text[reader->at] != ')') {
		(void)structure_value(reader, key, sizeof(key));
		(void)structure_value(reader, value, sizeof(value));
		structure_lower(key);

		/* The character set. */
		same = strcmp(key, "charset");
		if (same == 0)
			structure_copy(charset, charset_size, value);

		/* A name (a file's). */
		same = strcmp(key, "name");
		if (same == 0)
			structure_copy(name, name_size, value);
		structure_spaces(reader);
	}

	/* The end of the list. */
	(void)structure_close(reader);
}

/* Reads a disposition (NIL, or ("attachment" (names and values))): whether it is attached, and its file name. */
static void
structure_disposition(
	struct structure_reader *reader,
	int *attachment,
	char *name,
	size_t name_size)
{
	char kind[STRUCTURE_VALUE_MAX];
	char key[STRUCTURE_VALUE_MAX];
	char value[STRUCTURE_VALUE_MAX];
	int opened;
	int same;

	/* NIL is none. */
	opened = structure_open(reader);
	if (!opened) {
		structure_skip(reader);
		return;
	}

	/* Its kind. */
	(void)structure_value(reader, kind, sizeof(kind));
	structure_lower(kind);
	same = strcmp(kind, "attachment");
	if (same == 0)
		*attachment = 1;

	/* Its parameters: the file name. */
	structure_spaces(reader);
	opened = structure_open(reader);
	if (opened) {
		structure_spaces(reader);
		while (!reader->failed && reader->text[reader->at] != ')') {
			(void)structure_value(reader, key, sizeof(key));
			(void)structure_value(reader, value, sizeof(value));
			structure_lower(key);
			same = strcmp(key, "filename");
			if (same == 0)
				structure_copy(name, name_size, value);
			structure_spaces(reader);
		}

		/* The parameters' end. */
		(void)structure_close(reader);
	} else {
		structure_skip(reader);
	}

	/* The rest of it, up to its parenthesis. */
	structure_spaces(reader);
	while (!reader->failed && reader->text[reader->at] != ')' && reader->text[reader->at] != '\0') {
		structure_skip(reader);
		structure_spaces(reader);
	}

	/* The disposition's end. */
	(void)structure_close(reader);
}

/*
 * Reads one value that is not a list: a quoted string (its escapes taken
 * off), NIL (empty), or an atom or number.  Returns 1 with it, 0 when a
 * list or the end stands there (nothing is read then; a literal fails).
 */
static int
structure_value(
	struct structure_reader *reader,
	char *value,
	size_t size)
{
	const char *text;
	size_t length;
	int c;

	/* Nothing yet. */
	value[0] = '\0';
	structure_spaces(reader);
	text = reader->text;
	c = (unsigned char)text[reader->at];

	/* A list, a list's end or the end is not a value. */
	if (c == '(' || c == ')' || c == '\0')
		return 0;

	/* A literal is not read. */
	if (c == '{') {
		reader->failed = 1;
		return 0;
	}

	/* A quoted string. */
	length = 0;
	if (c == '"') {
		reader->at++;
		while (text[reader->at] != '\0' && text[reader->at] != '"') {
			if (text[reader->at] == '\\' && text[reader->at + 1U] != '\0')
				reader->at++;
			if (length + 1U < size) {
				value[length] = text[reader->at];
				length++;
			}

			/* The next byte. */
			reader->at++;
		}

		/* Its closing quote. */
		if (text[reader->at] == '"')
			reader->at++;
		value[length] = '\0';
		return 1;
	}

	/* An atom or a number, up to a space or a parenthesis. */
	while (text[reader->at] != '\0' && text[reader->at] != ' ' && text[reader->at] != '(' && text[reader->at] != ')') {
		if (length + 1U < size) {
			value[length] = text[reader->at];
			length++;
		}

		/* The next byte. */
		reader->at++;
	}

	/* Its end; NIL is empty. */
	value[length] = '\0';
	if (length == 3U && value[0] == 'N' && value[1] == 'I' && value[2] == 'L')
		value[0] = '\0';
	return 1;
}

/* Skips one value: a list with everything in it, or a value that is not one. */
static void
structure_skip(
	struct structure_reader *reader)
{
	char ignored[STRUCTURE_VALUE_MAX];
	int depth;
	int opened;
	int read;

	/* A value that is not a list. */
	structure_spaces(reader);
	opened = structure_open(reader);
	if (!opened) {
		read = structure_value(reader, ignored, sizeof(ignored));
		if (!read && reader->text[reader->at] != ')')
			reader->failed = 1;
		return;
	}

	/* A list: its values and lists, to its own parenthesis. */
	depth = 1;
	while (depth > 0 && !reader->failed) {
		structure_spaces(reader);
		opened = structure_open(reader);
		if (opened) {
			depth++;
			continue;
		}

		/* A list's end. */
		if (reader->text[reader->at] == ')') {
			reader->at++;
			depth--;
			continue;
		}

		/* A value in it. */
		read = structure_value(reader, ignored, sizeof(ignored));
		if (!read)
			reader->failed = 1;
	}
}

/* Skips the spaces at the reader's place. */
static void
structure_spaces(
	struct structure_reader *reader)
{
	/* Each space. */
	while (reader->text[reader->at] == ' ')
		reader->at++;
}

/* Takes an opening parenthesis at the reader's place; 1 when it was there. */
static int
structure_open(
	struct structure_reader *reader)
{
	/* Not one. */
	structure_spaces(reader);
	if (reader->text[reader->at] != '(')
		return 0;

	/* Taken. */
	reader->at++;
	return 1;
}

/* Takes a closing parenthesis at the reader's place; a missing one fails the read. */
static int
structure_close(
	struct structure_reader *reader)
{
	/* Not one. */
	structure_spaces(reader);
	if (reader->text[reader->at] != ')') {
		reader->failed = 1;
		return 0;
	}

	/* Taken. */
	reader->at++;
	return 1;
}

/* Turns a text's ASCII capitals into small letters. */
static void
structure_lower(
	char *text)
{
	size_t index;

	/* Each byte. */
	for (index = 0; text[index] != '\0'; index++) {
		if (text[index] >= 'A' && text[index] <= 'Z')
			text[index] = (char)(text[index] - 'A' + 'a');
	}
}

/* Copies a string into a room, cut to it. */
static void
structure_copy(
	char *to,
	size_t size,
	const char *from)
{
	size_t length;

	/* No room at all. */
	if (size == 0U)
		return;

	/* As much as fits, and the NUL. */
	length = strlen(from);
	if (length >= size)
		length = size - 1U;
	memcpy(to, from, length);
	to[length] = '\0';
}
