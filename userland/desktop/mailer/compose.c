/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writing a message (WS169 p003, RFC 5322 and MIME; mail.h): the header
 * fields (From with the user's name, To, Cc, the subject as an encoded
 * word when it is not ASCII, the date in UTC, a new Message-ID, and
 * In-Reply-To and References for a reply) and the words as UTF-8 text
 * in quoted-printable, every line ended with CR LF.
 *
 * ws189-p004: a message with files attached is multipart/mixed: the words
 * as the first part, then each file in base64 (lines of 76), its name a
 * quoted string, or in RFC 2231's form (filename*=UTF-8''%XX) when it is
 * not plain ASCII.
 *
 * ws177-p016: the names of To and Cc that are not ASCII are encoded words
 * too ("=?UTF-8?B?...?= <addr>"), a long encoded value is split into words
 * of whole characters, and the long fields are folded (a line end and a
 * space) before 78 characters, at the commas of a list and the spaces of
 * a subject.
 */

#include "mail.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The longest line of quoted-printable, before its soft line break. */
#define COMPOSE_LINE_MAX	75U

/* The longest line of a header field before it is folded (RFC 5322 2.1.1 asks for 78). */
#define COMPOSE_FIELD_MAX	78U

/* The longest line of base64 (RFC 2045 section 6.8). */
#define COMPOSE_BASE64_LINE	76U

/* The most bytes of text in one encoded word (60 of base64, a word of 72 with its "=?UTF-8?B?" and "?="). */
#define COMPOSE_WORD_BYTES	45U

/* The days and months of a date (RFC 5322 section 3.3). */
static const char *const compose_days[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const compose_months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* The hexadecimal digits of quoted-printable. */
static const char compose_hex[] = "0123456789ABCDEF";

/*
 * A message being written: its bytes (allocated), how many there are, the
 * room, and whether growing it failed (later appends do nothing then).
 */
struct compose_text {
	char *bytes;
	size_t length;
	size_t capacity;
	int failed;
};

/* How many Message-IDs this program made, so that two made in one second differ. */
static unsigned long compose_serial;

static void compose_append(struct compose_text *text, const char *bytes, size_t length);
static void compose_string(struct compose_text *text, const char *string);
static void compose_field(struct compose_text *text, const char *name, const char *value);
static size_t compose_encoded(struct compose_text *text, const char *value, size_t column);
static void compose_addresses(struct compose_text *text, const char *name, const char *value);
static size_t compose_address(struct compose_text *text, const char *item, size_t length, size_t column);
static void compose_subject(struct compose_text *text, const char *subject);
static void compose_field_bytes(struct compose_text *text, const char *bytes, size_t length);
static void compose_body(struct compose_text *text, const char *body);
static int compose_is_ascii(const char *text);
static void compose_attachment(struct compose_text *text, const char *boundary, const struct ml_attachment *attachment);
static void compose_parameter(struct compose_text *text, const char *name, const char *value);
static void compose_base64(struct compose_text *text, const unsigned char *data, size_t length);

/*
 * Writes a message from an account: to, cc (may be empty), its subject
 * and its words, a reply to a message ID (NULL or empty for none), dated
 * now.  Returns 0 with the bytes (the caller frees them), or ENOMEM.
 */
int
ml_compose(
	const struct ml_account_config *account,
	const char *to,
	const char *cc,
	const char *subject,
	const char *body,
	const char *reply_to_id,
	time_t now,
	char **raw,
	size_t *length)
{
	int error;

	/* The same message with no file attached. */
	error = ml_compose_with(account, to, cc, subject, body, reply_to_id, NULL, 0U, now, raw, length);
	if (error != 0)
		return error;

	/* Succeeded: the message is written. */
	return 0;
}

/*
 * Writes a message as ml_compose does, with files attached (ws189-p004):
 * multipart/mixed when there are any.  Returns 0 with the bytes (the
 * caller frees them), EINVAL for more than ML_ATTACH_MAX files, or ENOMEM.
 */
int
ml_compose_with(
	const struct ml_account_config *account,
	const char *to,
	const char *cc,
	const char *subject,
	const char *body,
	const char *reply_to_id,
	const struct ml_attachment *attachments,
	size_t count,
	time_t now,
	char **raw,
	size_t *length)
{
	struct compose_text text;
	struct tm parts;
	const char *domain;
	char line[ML_TEXT_MAX * 2U];
	char boundary[96];
	size_t index;
	int ascii;

	/* Nothing yet, and no more files than a message takes. */
	memset(&text, 0, sizeof(text));
	*raw = NULL;
	*length = 0;
	if (count > ML_ATTACH_MAX)
		return EINVAL;
	boundary[0] = '\0';

	/* From: the user's name (quoted, or an encoded word) and address. */
	compose_string(&text, "From: ");
	ascii = compose_is_ascii(account->name);
	if (account->name[0] != '\0' && ascii) {
		(void)snprintf(line, sizeof(line), "\"%s\" ", account->name);
		compose_string(&text, line);
	} else if (account->name[0] != '\0') {
		(void)compose_encoded(&text, account->name, 6U);
		compose_string(&text, " ");
	}

	/* The address. */
	(void)snprintf(line, sizeof(line), "<%s>\r\n", account->address);
	compose_string(&text, line);

	/* The receivers, their names encoded when they are not ASCII. */
	compose_addresses(&text, "To", to);
	if (cc[0] != '\0')
		compose_addresses(&text, "Cc", cc);

	/* The subject, encoded words when it is not ASCII, folded when it is long. */
	compose_subject(&text, subject);

	/* The date, in UTC. */
	(void)gmtime_r(&now, &parts);
	(void)snprintf(line, sizeof(line), "Date: %s, %d %s %d %02d:%02d:%02d +0000\r\n",
	    compose_days[parts.tm_wday], parts.tm_mday, compose_months[parts.tm_mon], parts.tm_year + 1900,
	    parts.tm_hour, parts.tm_min, parts.tm_sec);
	compose_string(&text, line);

	/* A new ID at the address's domain. */
	domain = strchr(account->address, '@');
	if (domain == NULL)
		domain = "@localhost";
	compose_serial++;
	(void)snprintf(line, sizeof(line), "Message-ID: <%lld.%ld.%lu%s>\r\n", (long long)now, (long)getpid(), compose_serial, domain);
	compose_string(&text, line);

	/* The message replied to. */
	if (reply_to_id != NULL && reply_to_id[0] != '\0') {
		compose_field(&text, "In-Reply-To", reply_to_id);
		compose_field(&text, "References", reply_to_id);
	}

	/* A message with files: the parts' boundary, a line no file's base64 or the words' quoted-printable can hold ("=_" starts no such line). */
	compose_string(&text, "MIME-Version: 1.0\r\n");
	if (count > 0U) {
		(void)snprintf(boundary, sizeof(boundary), "=_keiland_%lld_%ld_%lu", (long long)now, (long)getpid(), compose_serial);
		(void)snprintf(line, sizeof(line), "Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\n", boundary);
		compose_string(&text, line);
		(void)snprintf(line, sizeof(line), "--%s\r\n", boundary);
		compose_string(&text, line);
	}

	/* The words: UTF-8 text in quoted-printable. */
	compose_string(&text, "Content-Type: text/plain; charset=utf-8\r\n");
	compose_string(&text, "Content-Transfer-Encoding: quoted-printable\r\n");
	compose_string(&text, "\r\n");
	compose_body(&text, body);

	/* Each file, then the parts' end. */
	for (index = 0; index < count; index++)
		compose_attachment(&text, boundary, &attachments[index]);
	if (count > 0U) {
		(void)snprintf(line, sizeof(line), "--%s--\r\n", boundary);
		compose_string(&text, line);
	}

	/* Growing failed somewhere. */
	if (text.failed) {
		free(text.bytes);
		return ENOMEM;
	}

	/* Succeeded: the message is written. */
	*raw = text.bytes;
	*length = text.length;
	return 0;
}

/* Appends bytes, growing the room; a failure is kept and later appends do nothing. */
static void
compose_append(
	struct compose_text *text,
	const char *bytes,
	size_t length)
{
	size_t capacity;
	char *grown;

	/* An earlier failure. */
	if (text->failed)
		return;

	/* Room for the bytes and a NUL. */
	if (text->length + length + 1U > text->capacity) {
		capacity = text->capacity * 2U;
		if (capacity < 1024U)
			capacity = 1024U;
		while (capacity < text->length + length + 1U)
			capacity *= 2U;
		grown = realloc(text->bytes, capacity);
		if (grown == NULL) {
			text->failed = 1;
			return;
		}

		/* The grown room. */
		text->bytes = grown;
		text->capacity = capacity;
	}

	/* The bytes and the NUL. */
	memcpy(text->bytes + text->length, bytes, length);
	text->length += length;
	text->bytes[text->length] = '\0';
}

/* Appends a string. */
static void
compose_string(
	struct compose_text *text,
	const char *string)
{
	/* Its bytes. */
	compose_append(text, string, strlen(string));
}

/* Appends a field "Name: value" and its line end (line ends in the value become spaces). */
static void
compose_field(
	struct compose_text *text,
	const char *name,
	const char *value)
{
	size_t index;

	/* The name. */
	compose_string(text, name);
	compose_string(text, ": ");

	/* The value, without line ends (they would start a new field). */
	for (index = 0; value[index] != '\0'; index++) {
		if (value[index] == '\r' || value[index] == '\n')
			compose_append(text, " ", 1U);
		else
			compose_append(text, value + index, 1U);
	}

	/* The line end. */
	compose_string(text, "\r\n");
}

/*
 * Appends a value as encoded words "=?UTF-8?B?...?=", each of at most
 * COMPOSE_WORD_BYTES bytes of whole characters, folded onto a new line
 * when the line would grow past COMPOSE_FIELD_MAX.  column is where the
 * line is; returns where it is after.
 */
static size_t
compose_encoded(
	struct compose_text *text,
	const char *value,
	size_t column)
{
	char encoded[COMPOSE_WORD_BYTES * 2U];
	size_t start;
	size_t end;
	size_t total;
	size_t length;

	/* Each piece of whole characters. */
	total = strlen(value);
	start = 0;
	while (start < total) {
		/* As many bytes as fit, back to the start of a character. */
		end = start + COMPOSE_WORD_BYTES;
		if (end > total)
			end = total;
		while (end < total && end > start && ((unsigned char)value[end] & 0xc0U) == 0x80U)
			end--;
		if (end == start)
			end = start + 1U;

		/* Its word, on a new line when this one is full. */
		length = ml_base64_encode((const unsigned char *)value + start, end - start, encoded, sizeof(encoded));
		if (column != 0U && column + length + 12U > COMPOSE_FIELD_MAX) {
			compose_string(text, "\r\n ");
			column = 1;
		} else if (start != 0U) {
			compose_string(text, " ");
			column++;
		}

		/* The word. */
		compose_string(text, "=?UTF-8?B?");
		compose_append(text, encoded, length);
		compose_string(text, "?=");
		column += length + 12U;

		/* The next piece. */
		start = end;
	}

	/* Where the line is. */
	return column;
}

/*
 * Appends a field of receivers ("To", "Cc") as the user wrote it: each
 * item between commas outside quotes, a name that is not ASCII as encoded
 * words, the items folded at their commas when the line grows long.
 */
static void
compose_addresses(
	struct compose_text *text,
	const char *name,
	const char *value)
{
	size_t column;
	size_t start;
	size_t index;
	int first;
	int quoted;
	int ends;

	/* The name. */
	compose_string(text, name);
	compose_string(text, ": ");
	column = strlen(name) + 2U;

	/* Each item, split at a comma outside quotes. */
	first = 1;
	start = 0;
	quoted = 0;
	for (index = 0;; index++) {
		/* A quote opens or closes a name. */
		if (value[index] == '"')
			quoted = !quoted;

		/* An item ends at an unquoted comma or the value's end. */
		ends = 0;
		if (value[index] == '\0') {
			ends = 1;
		} else if (!quoted && value[index] == ',') {
			ends = 1;
		}

		/* Inside an item: on. */
		if (!ends)
			continue;

		/* Its spaces around taken off; an empty item is skipped. */
		while (start < index && (value[start] == ' ' || value[start] == '\t'))
			start++;
		if (start < index) {
			/* After the first, a comma, and a new line when the next item would not fit. */
			if (!first) {
				compose_string(text, ",");
				column++;
				if (column + (index - start) + 1U > COMPOSE_FIELD_MAX) {
					compose_string(text, "\r\n");
					column = 0;
				}

				/* The space before it. */
				compose_string(text, " ");
				column++;
			}

			/* The item. */
			column = compose_address(text, value + start, index - start, column);
			first = 0;
		}

		/* The value's end. */
		if (value[index] == '\0')
			break;
		start = index + 1U;
	}

	/* The line end. */
	compose_string(text, "\r\n");
}

/*
 * Appends one receiver: as written when it is ASCII, else its name before
 * "<" as encoded words (its quotes taken off) and the address after.
 * Line ends in it become spaces.  Returns where the line is after.
 */
static size_t
compose_address(
	struct compose_text *text,
	const char *item,
	size_t length,
	size_t column)
{
	char name[ML_TEXT_MAX];
	const char *angle;
	size_t name_length;
	size_t index;
	int ascii;

	/* The item's text, its line ends as spaces. */
	if (length >= sizeof(name))
		length = sizeof(name) - 1U;
	for (index = 0; index < length; index++) {
		name[index] = item[index];
		if (item[index] == '\r' || item[index] == '\n')
			name[index] = ' ';
	}

	/* Its end, and whether it is ASCII. */
	name[length] = '\0';
	ascii = compose_is_ascii(name);

	/* ASCII, or no address in angle brackets: as written. */
	angle = strchr(name, '<');
	if (ascii || angle == NULL) {
		compose_string(text, name);
		return column + length;
	}

	/* The name before the angle bracket, without its spaces and quotes. */
	name_length = (size_t)(angle - name);
	while (name_length > 0U && (name[name_length - 1U] == ' ' || name[name_length - 1U] == '"'))
		name_length--;
	index = 0;
	while (index < name_length && (name[index] == ' ' || name[index] == '"'))
		index++;

	/* The name encoded, then the address. */
	name[name_length] = '\0';
	column = compose_encoded(text, name + index, column);
	compose_string(text, " ");
	compose_string(text, angle);

	/* Where the line is. */
	return column + 1U + strlen(angle);
}

/* Appends the subject: encoded words when it is not ASCII, else its words folded at spaces before the line grows too long. */
static void
compose_subject(
	struct compose_text *text,
	const char *subject)
{
	size_t column;
	size_t start;
	size_t end;
	int ascii;

	/* The field's name. */
	compose_string(text, "Subject: ");
	column = 9;

	/* Not ASCII: encoded words. */
	ascii = compose_is_ascii(subject);
	if (!ascii) {
		(void)compose_encoded(text, subject, column);
		compose_string(text, "\r\n");
		return;
	}

	/* ASCII: word by word, a space folded into a line end when the next word would not fit. */
	start = 0;
	while (subject[start] != '\0') {
		/* The word and the spaces before it. */
		end = start;
		while (subject[end] == ' ')
			end++;
		while (subject[end] != '\0' && subject[end] != ' ')
			end++;

		/* Folded before its space when it would not fit (never before the first word). */
		if (start != 0U && column + (end - start) > COMPOSE_FIELD_MAX) {
			compose_string(text, "\r\n");
			column = 0;
		}

		/* The word, its line ends as spaces. */
		compose_field_bytes(text, subject + start, end - start);
		column += end - start;
		start = end;
	}

	/* The line end. */
	compose_string(text, "\r\n");
}

/* Appends bytes of a field's value, a line end in them as a space (it would start a new field). */
static void
compose_field_bytes(
	struct compose_text *text,
	const char *bytes,
	size_t length)
{
	size_t index;

	/* Each byte. */
	for (index = 0; index < length; index++) {
		if (bytes[index] == '\r' || bytes[index] == '\n') {
			compose_append(text, " ", 1U);
		} else {
			compose_append(text, bytes + index, 1U);
		}
	}
}

/* Appends the words in quoted-printable, each line ended with CR LF. */
static void
compose_body(
	struct compose_text *text,
	const char *body)
{
	unsigned char byte;
	char escape[3];
	size_t column;
	size_t index;
	int literal;

	/* Each byte. */
	column = 0;
	for (index = 0; body[index] != '\0'; index++) {
		byte = (unsigned char)body[index];

		/* A line end: CR LF (a CR of the text is dropped). */
		if (byte == '\r')
			continue;
		if (byte == '\n') {
			compose_string(text, "\r\n");
			column = 0;
			continue;
		}

		/* A byte that stands for itself: printable ASCII but '=', and a space or tab not at a line's end. */
		literal = 0;
		if (byte >= 33U && byte <= 126U && byte != '=')
			literal = 1;
		if ((byte == ' ' || byte == '\t') && body[index + 1U] != '\n' && body[index + 1U] != '\r' && body[index + 1U] != '\0')
			literal = 1;

		/* A soft line break before the line grows too long. */
		if (column + 3U > COMPOSE_LINE_MAX) {
			compose_string(text, "=\r\n");
			column = 0;
		}

		/* The byte, or its escape. */
		if (literal) {
			compose_append(text, body + index, 1U);
			column++;
		} else {
			escape[0] = '=';
			escape[1] = compose_hex[byte >> 4];
			escape[2] = compose_hex[byte & 0x0fU];
			compose_append(text, escape, 3U);
			column += 3U;
		}
	}

	/* The last line's end. */
	compose_string(text, "\r\n");
}

/* Tells whether a text is all ASCII. */
static int
compose_is_ascii(
	const char *text)
{
	size_t index;

	/* Each byte below 128. */
	for (index = 0; text[index] != '\0'; index++) {
		if ((unsigned char)text[index] >= 0x80U)
			return 0;
	}

	/* All ASCII. */
	return 1;
}

/*
 * Writes one file of the message as a part: its boundary, its type and
 * name, its disposition with its name, and its bytes in base64.
 */
static void
compose_attachment(
	struct compose_text *text,
	const char *boundary,
	const struct ml_attachment *attachment)
{
	char line[ML_TEXT_MAX];
	const char *type;

	/* The part's start. */
	(void)snprintf(line, sizeof(line), "--%s\r\n", boundary);
	compose_string(text, line);

	/* Its type, with its name. */
	type = attachment->type;
	if (type[0] == '\0')
		type = "application/octet-stream";
	compose_string(text, "Content-Type: ");
	compose_string(text, type);
	compose_parameter(text, "name", attachment->name);
	compose_string(text, "\r\n");

	/* Its disposition, with its name again. */
	compose_string(text, "Content-Disposition: attachment");
	compose_parameter(text, "filename", attachment->name);
	compose_string(text, "\r\n");

	/* Its bytes in base64, after a blank line. */
	compose_string(text, "Content-Transfer-Encoding: base64\r\n\r\n");
	compose_base64(text, attachment->data, attachment->length);
}

/*
 * Writes a parameter of a header field on a line of its own (folded): as
 * a quoted string when its value is printable ASCII without '"' and '\\',
 * else in RFC 2231's form, UTF-8 with the other bytes as %XX.
 */
static void
compose_parameter(
	struct compose_text *text,
	const char *name,
	const char *value)
{
	const unsigned char *byte;
	char escape[3];
	int plain;

	/* Whether the value can be a quoted string. */
	plain = 1;
	for (byte = (const unsigned char *)value; *byte != '\0'; byte++) {
		/* A control, a byte past ASCII, a quote or a backslash cannot. */
		if (*byte < 32U || *byte > 126U || *byte == '"' || *byte == '\\') {
			plain = 0;
			break;
		}
	}

	/* A quoted string. */
	compose_string(text, ";\r\n\t");
	compose_string(text, name);
	if (plain) {
		compose_string(text, "=\"");
		compose_string(text, value);
		compose_string(text, "\"");
		return;
	}

	/* RFC 2231: the charset, no language, then the bytes, unreserved ones as they are. */
	compose_string(text, "*=UTF-8''");
	for (byte = (const unsigned char *)value; *byte != '\0'; byte++) {
		/* Letters, digits and "-._~" stand for themselves. */
		if ((*byte >= 'a' && *byte <= 'z') ||
		    (*byte >= 'A' && *byte <= 'Z') ||
		    (*byte >= '0' && *byte <= '9') ||
		    *byte == '-' || *byte == '.' || *byte == '_' || *byte == '~') {
			compose_append(text, (const char *)byte, 1U);
			continue;
		}

		/* Any other byte as %XX. */
		escape[0] = '%';
		escape[1] = compose_hex[*byte >> 4];
		escape[2] = compose_hex[*byte & 0x0fU];
		compose_append(text, escape, 3U);
	}
}

/* Writes bytes in base64, lines of COMPOSE_BASE64_LINE ended with CR LF. */
static void
compose_base64(
	struct compose_text *text,
	const unsigned char *data,
	size_t length)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char quad[4];
	uint32_t group;
	size_t index;
	size_t column;
	size_t left;

	/* Each group of three bytes as four characters, the last padded with '='. */
	column = 0;
	for (index = 0; index < length; index += 3U) {
		left = length - index;
		group = (uint32_t)data[index] << 16;
		if (left > 1U)
			group |= (uint32_t)data[index + 1U] << 8;
		if (left > 2U)
			group |= (uint32_t)data[index + 2U];
		quad[0] = alphabet[(group >> 18) & 0x3fU];
		quad[1] = alphabet[(group >> 12) & 0x3fU];
		quad[2] = '=';
		quad[3] = '=';
		if (left > 1U)
			quad[2] = alphabet[(group >> 6) & 0x3fU];
		if (left > 2U)
			quad[3] = alphabet[group & 0x3fU];
		compose_append(text, quad, 4U);

		/* A full line ends. */
		column += 4U;
		if (column >= COMPOSE_BASE64_LINE) {
			compose_string(text, "\r\n");
			column = 0;
		}
	}

	/* The last line's end. */
	if (column != 0U || length == 0U)
		compose_string(text, "\r\n");
}
