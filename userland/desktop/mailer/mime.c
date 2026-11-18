/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading a message (WS169 p003; mail.h): RFC 5322's header fields
 * (unfolded, their RFC 2047 encoded words decoded), the sender, the
 * receivers, the subject, the ID and the date, and MIME's body: a
 * multipart's text/plain part (else its text/html part with the tags
 * taken out), decoded from quoted-printable or base64 and from UTF-8,
 * US-ASCII or ISO-8859-1 to UTF-8, with the name and size of the first
 * file it carries.  Base64 encoding is here too, for compose.c and
 * SMTP's AUTH PLAIN.
 *
 * ws177-p016: ISO-2022-JP, Shift_JIS and EUC-JP are read to UTF-8
 * (jis.c), in the words and in the encoded words of the header fields.
 * Other character sets are kept as their bytes.
 */

#include "mail.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How deep multiparts are followed. */
#define MIME_DEPTH_MAX		8

/* The longest header field value kept, with its NUL. */
#define MIME_FIELD_MAX		2048U

/* The ways a part's bytes are encoded (Content-Transfer-Encoding). */
enum mime_encoding {
	MIME_ENCODING_PLAIN,
	MIME_ENCODING_QUOTED,
	MIME_ENCODING_BASE64
};

/*
 * A text being built: its bytes (allocated, NUL ended once anything is
 * appended), how many there are, and the room.
 */
struct mime_text {
	char *bytes;
	size_t length;
	size_t capacity;
};

/*
 * One part of a message as far as the body search needs it: where its
 * header fields and its body are in the raw message, its type
 * ("text/plain", lower case), its character set and boundary, how its
 * body is encoded, and the file name it gives (empty for none) and
 * whether it is an attachment by its disposition.
 */
struct mime_part {
	const char *header;
	size_t header_length;
	const char *body;
	size_t body_length;
	char type[64];
	char charset[32];
	char boundary[128];
	enum mime_encoding encoding;
	char file_name[ML_TEXT_MAX];
	int attachment;
};

/* The month names of the date, in order. */
static const char *const mime_months[12] = {
	"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
};

/* Base64's alphabet. */
static const char mime_base64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int mime_split(const char *raw, size_t length, struct mime_part *part);
static int mime_field(const char *header, size_t length, const char *name, char *value, size_t size);
static void mime_part_describe(struct mime_part *part);
static int mime_parameter(const char *value, const char *name, char *found, size_t size);
static int mime_find_body(const struct mime_part *part, int depth, struct ml_parsed *parsed, struct mime_text *plain, struct mime_text *html);
static int mime_decode_part(const struct mime_part *part, struct mime_text *text);
static int mime_to_utf8(const char *bytes, size_t length, const char *charset, struct mime_text *text);
static int mime_html_to_text(const char *html, size_t length, struct mime_text *text);
static int mime_html_breaks(const char *tag);
static int mime_html_hidden(const char *tag);
static size_t mime_html_entity(const char *html, size_t length, struct mime_text *text, int *error);
static int mime_words(const char *value, char *decoded, size_t size);
static const char *mime_word_end(const char *at);
static int mime_word(const char *word, size_t length, struct mime_text *text);
static void mime_address(const char *value, char *name, size_t name_size, char *address, size_t address_size);
static time_t mime_date(const char *value);
static int mime_quoted_decode(const char *bytes, size_t length, struct mime_text *text, int underscore_space);
static int mime_base64_decode(const char *bytes, size_t length, struct mime_text *text);
static int mime_base64_value(int c);
static int mime_append(struct mime_text *text, const char *bytes, size_t length);
static int mime_append_code_point(struct mime_text *text, unsigned long code_point);
static int mime_hex(int c);
static int mime_same(const char *a, const char *b, size_t length);
static int mime_lower(int c);
static void mime_copy(char *to, size_t size, const char *from, size_t length);
static void mime_trim(char *text);
static int mime_finish(struct mime_text *plain, struct ml_parsed *parsed);

/*
 * Reads a raw message into its parts the view shows.  Returns 0 or ENOMEM
 * (a message it cannot read well still gives what it could).
 */
int
ml_mime_parse(
	const char *raw,
	size_t length,
	struct ml_parsed *parsed)
{
	struct mime_part part;
	struct mime_text plain;
	struct mime_text html;
	char value[MIME_FIELD_MAX];
	int found;
	int error;

	/* Nothing yet. */
	memset(parsed, 0, sizeof(parsed[0]));
	memset(&plain, 0, sizeof(plain));
	memset(&html, 0, sizeof(html));

	/* The header fields and the body of the whole message. */
	(void)mime_split(raw, length, &part);

	/* The sender: its name and address. */
	found = mime_field(part.header, part.header_length, "from", value, sizeof(value));
	if (found)
		mime_address(value, parsed->from_name, sizeof(parsed->from_name), parsed->from_address, sizeof(parsed->from_address));

	/* The receivers. */
	found = mime_field(part.header, part.header_length, "to", value, sizeof(value));
	if (found)
		(void)mime_words(value, parsed->to, sizeof(parsed->to));

	/* Those in copy. */
	found = mime_field(part.header, part.header_length, "cc", value, sizeof(value));
	if (found)
		(void)mime_words(value, parsed->cc, sizeof(parsed->cc));

	/* The subject. */
	found = mime_field(part.header, part.header_length, "subject", value, sizeof(value));
	if (found)
		(void)mime_words(value, parsed->subject, sizeof(parsed->subject));

	/* The message's ID, for a reply. */
	found = mime_field(part.header, part.header_length, "message-id", value, sizeof(value));
	if (found)
		mime_copy(parsed->message_id, sizeof(parsed->message_id), value, strlen(value));

	/* The date. */
	found = mime_field(part.header, part.header_length, "date", value, sizeof(value));
	if (found)
		parsed->date = mime_date(value);

	/* The words: a text/plain part, else a text/html part as text. */
	mime_part_describe(&part);
	error = mime_find_body(&part, 0, parsed, &plain, &html);
	if (error == 0 && plain.length == 0U && html.length != 0U)
		error = mime_html_to_text(html.bytes, html.length, &plain);
	free(html.bytes);
	if (error != 0) {
		free(plain.bytes);
		return error;
	}

	/* The words with line feeds, and the sign-in code in them. */
	error = mime_finish(&plain, parsed);
	if (error != 0)
		return error;

	/* Succeeded: the message is read. */
	return 0;
}

/*
 * Reads a message larger than Mail fetches whole from its pieces
 * (imap.c's ml_imap_fetch_large): its header, its structure, and the
 * bytes of the part of its words (NULL for none) as the server keeps them
 * (still in their transfer encoding and character set).  Returns 0 or
 * ENOMEM.
 */
int
ml_mime_parse_large(
	const char *header,
	size_t header_length,
	const struct ml_structure *structure,
	const struct ml_structure_part *part,
	const char *body,
	size_t body_length,
	struct ml_parsed *parsed)
{
	struct mime_part words;
	struct mime_text decoded;
	struct mime_text plain;
	int same;
	int error;

	/* The header fields (the sender, the subject, the date ...), without words. */
	error = ml_mime_parse(header, header_length, parsed);
	if (error != 0)
		return error;
	ml_mime_release(parsed);
	parsed->code[0] = '\0';

	/* The file it carries, with its real size. */
	if (structure->file_name[0] != '\0') {
		(void)mime_words(structure->file_name, parsed->file_name, sizeof(parsed->file_name));
		parsed->file_size = structure->file_size;
	}

	/* The part of the words: its type, character set and transfer encoding. */
	memset(&decoded, 0, sizeof(decoded));
	memset(&plain, 0, sizeof(plain));
	if (part != NULL && body != NULL) {
		memset(&words, 0, sizeof(words));
		words.body = body;
		words.body_length = body_length;
		mime_copy(words.type, sizeof(words.type), part->type, strlen(part->type));
		mime_copy(words.charset, sizeof(words.charset), part->charset, strlen(part->charset));
		words.encoding = MIME_ENCODING_PLAIN;
		same = mime_same(part->encoding, "quoted-printable", 16U);
		if (same)
			words.encoding = MIME_ENCODING_QUOTED;
		same = mime_same(part->encoding, "base64", 6U);
		if (same)
			words.encoding = MIME_ENCODING_BASE64;

		/* Decoded, and HTML made text. */
		error = mime_decode_part(&words, &decoded);
		same = mime_same(words.type, "text/html", 9U);
		if (error == 0 && same) {
			error = mime_html_to_text(decoded.bytes, decoded.length, &plain);
			free(decoded.bytes);
		} else {
			plain = decoded;
		}

		/* A part that could not be decoded. */
		if (error != 0) {
			free(plain.bytes);
			return error;
		}
	}

	/* The words with line feeds, and the sign-in code in them. */
	error = mime_finish(&plain, parsed);
	if (error != 0)
		return error;

	/* Succeeded: the message is read. */
	return 0;
}

/*
 * Frees what a read message holds.
 */
void
ml_mime_release(
	struct ml_parsed *parsed)
{
	/* The words. */
	free(parsed->body);
	parsed->body = NULL;
}

/*
 * Takes the addresses out of a list as a user writes it ("Ben <ben@x>,
 * aiko@y"): the part in angle brackets, else the whole item.  Returns 0,
 * or ENOSPC when there are more than capacity.
 */
int
ml_mime_address_list(
	const char *list,
	char (*addresses)[ML_TEXT_MAX],
	size_t capacity,
	size_t *count)
{
	char item[ML_TEXT_MAX];
	char name[ML_TEXT_MAX];
	size_t start;
	size_t index;
	int quoted;
	int ends;

	/* Each item, split at a comma outside quotes. */
	*count = 0;
	start = 0;
	quoted = 0;
	for (index = 0;; index++) {
		/* A quote opens or closes a name. */
		if (list[index] == '"')
			quoted = !quoted;

		/* An item ends at an unquoted comma, a semicolon, or the list's end. */
		ends = 0;
		if (list[index] == '\0')
			ends = 1;
		else if (!quoted && (list[index] == ',' || list[index] == ';'))
			ends = 1;
		if (!ends)
			continue;

		/* The item's address; an empty item is skipped. */
		mime_copy(item, sizeof(item), list + start, index - start);
		mime_address(item, name, sizeof(name), addresses[*count], ML_TEXT_MAX);
		if (addresses[*count][0] != '\0') {
			(*count)++;
			if (*count == capacity && list[index] != '\0')
				return ENOSPC;
		}

		/* The list's end. */
		if (list[index] == '\0')
			break;
		start = index + 1U;
	}

	/* Succeeded: the addresses are taken. */
	return 0;
}

/*
 * Encodes bytes in base64 into a room; returns the text's length, or 0
 * when the room is too small.
 */
size_t
ml_base64_encode(
	const unsigned char *bytes,
	size_t length,
	char *text,
	size_t size)
{
	unsigned long group;
	size_t needed;
	size_t index;
	size_t at;

	/* Four characters for each three bytes, and the NUL. */
	needed = (length + 2U) / 3U * 4U;
	if (needed + 1U > size)
		return 0;

	/* Each group of three bytes. */
	at = 0;
	for (index = 0; index < length; index += 3U) {
		/* The group's 24 bits, missing bytes as zero. */
		group = (unsigned long)bytes[index] << 16;
		if (index + 1U < length)
			group |= (unsigned long)bytes[index + 1U] << 8;
		if (index + 2U < length)
			group |= (unsigned long)bytes[index + 2U];

		/* Four characters, '=' for the missing bytes. */
		text[at] = mime_base64_alphabet[(group >> 18) & 0x3fU];
		text[at + 1U] = mime_base64_alphabet[(group >> 12) & 0x3fU];
		text[at + 2U] = '=';
		text[at + 3U] = '=';
		if (index + 1U < length)
			text[at + 2U] = mime_base64_alphabet[(group >> 6) & 0x3fU];
		if (index + 2U < length)
			text[at + 3U] = mime_base64_alphabet[group & 0x3fU];
		at += 4U;
	}

	/* The text's end. */
	text[at] = '\0';
	return at;
}

/*
 * Finishes the words of a message: CR LF made line feeds, an empty text
 * when there are none, given to the message (plain is freed), and the
 * sign-in code found in them.  Returns 0 or ENOMEM.
 */
static int
mime_finish(
	struct mime_text *plain,
	struct ml_parsed *parsed)
{
	struct mime_text crlf_free;
	size_t index;
	int error;

	/* The line ends made line feeds. */
	memset(&crlf_free, 0, sizeof(crlf_free));
	error = 0;
	for (index = 0; index < plain->length; index++) {
		/* A CR before a line feed goes. */
		if (plain->bytes[index] == '\r' && index + 1U < plain->length && plain->bytes[index + 1U] == '\n')
			continue;

		/* Every other byte stays. */
		error = mime_append(&crlf_free, plain->bytes + index, 1U);
		if (error != 0)
			break;
	}

	/* The text with its CRs is not needed after. */
	free(plain->bytes);
	plain->bytes = NULL;

	/* An empty body is an empty text. */
	if (error == 0 && crlf_free.bytes == NULL)
		error = mime_append(&crlf_free, "", 0U);
	if (error != 0) {
		free(crlf_free.bytes);
		return error;
	}

	/* The sign-in code in it. */
	parsed->body = crlf_free.bytes;
	(void)ml_code_find(parsed->subject, parsed->body, parsed->code, sizeof(parsed->code));

	/* Succeeded: the words are the message's. */
	return 0;
}

/* Splits a message or a part at the empty line between its header fields and its body. */
static int
mime_split(
	const char *raw,
	size_t length,
	struct mime_part *part)
{
	size_t index;

	/* Nothing yet: everything is header until the empty line. */
	memset(part, 0, sizeof(part[0]));
	part->header = raw;
	part->header_length = length;
	part->body = raw + length;
	part->body_length = 0;

	/* A part that starts with its empty line has no header fields. */
	if (length >= 2U && raw[0] == '\r' && raw[1] == '\n') {
		part->header_length = 0;
		part->body = raw + 2;
		part->body_length = length - 2U;
		return 0;
	}

	/* The same with a bare line feed. */
	if (length >= 1U && raw[0] == '\n') {
		part->header_length = 0;
		part->body = raw + 1;
		part->body_length = length - 1U;
		return 0;
	}

	/* The first line end followed by another (with or without CRs). */
	for (index = 0; index + 1U < length; index++) {
		/* Not a line end. */
		if (raw[index] != '\n')
			continue;

		/* LF LF. */
		if (raw[index + 1U] == '\n') {
			part->header_length = index + 1U;
			part->body = raw + index + 2U;
			part->body_length = length - index - 2U;
			return 0;
		}

		/* LF CR LF. */
		if (index + 2U < length && raw[index + 1U] == '\r' && raw[index + 2U] == '\n') {
			part->header_length = index + 1U;
			part->body = raw + index + 3U;
			part->body_length = length - index - 3U;
			return 0;
		}
	}

	/* No empty line: all header (a truncated message). */
	return ENOENT;
}

/*
 * Finds a header field by its name (any case) and gives its value,
 * unfolded and trimmed.  Returns 1 when found, 0 when not.
 */
static int
mime_field(
	const char *header,
	size_t length,
	const char *name,
	char *value,
	size_t size)
{
	size_t name_length;
	size_t line;
	size_t at;
	size_t kept;
	int same;

	/* Each line that starts a field. */
	name_length = strlen(name);
	line = 0;
	while (line < length) {
		/* The field's name and its colon at the line's start. */
		same = 0;
		if (line + name_length < length && header[line + name_length] == ':')
			same = mime_same(header + line, name, name_length);

		/* Not this field: on to the next line. */
		if (!same) {
			while (line < length && header[line] != '\n')
				line++;
			line++;
			continue;
		}

		/* Its value: this line and the lines that continue it (starting with a space or a tab), without the line ends. */
		kept = 0;
		at = line + name_length + 1U;
		while (at < length) {
			/* A line end: the field goes on only when the next line starts with a space or a tab. */
			if (header[at] == '\r') {
				/* A CR is dropped. */
				at++;
				continue;
			}

			/* A line feed before a space or a tab folds the field; any other ends it. */
			if (header[at] == '\n') {
				if (at + 1U < length && (header[at + 1U] == ' ' || header[at + 1U] == '\t')) {
					at++;
					continue;
				}

				/* The field's end. */
				break;
			}

			/* A byte of the value, while there is room. */
			if (kept + 1U < size) {
				value[kept] = header[at];
				kept++;
			}

			/* The next byte. */
			at++;
		}

		/* Found: trimmed. */
		value[kept] = '\0';
		mime_trim(value);
		return 1;
	}

	/* Not found. */
	return 0;
}

/* Reads a part's type, character set, boundary, encoding, file name and disposition from its header fields. */
static void
mime_part_describe(
	struct mime_part *part)
{
	char value[MIME_FIELD_MAX];
	char name[ML_TEXT_MAX];
	size_t index;
	int found;
	int same;

	/* The type, by default text/plain in US-ASCII. */
	mime_copy(part->type, sizeof(part->type), "text/plain", 10U);
	mime_copy(part->charset, sizeof(part->charset), "us-ascii", 8U);
	found = mime_field(part->header, part->header_length, "content-type", value, sizeof(value));
	if (found) {
		/* The type before its parameters, in lower case. */
		index = strcspn(value, "; \t");
		mime_copy(part->type, sizeof(part->type), value, index);
		for (index = 0; part->type[index] != '\0'; index++)
			part->type[index] = (char)mime_lower((unsigned char)part->type[index]);

		/* The parameters that matter. */
		(void)mime_parameter(value, "charset", part->charset, sizeof(part->charset));
		(void)mime_parameter(value, "boundary", part->boundary, sizeof(part->boundary));
		found = mime_parameter(value, "name", name, sizeof(name));
		if (found)
			(void)mime_words(name, part->file_name, sizeof(part->file_name));
	}

	/* The transfer encoding. */
	part->encoding = MIME_ENCODING_PLAIN;
	found = mime_field(part->header, part->header_length, "content-transfer-encoding", value, sizeof(value));
	if (found) {
		/* Quoted-printable. */
		same = mime_same(value, "quoted-printable", 16U);
		if (same)
			part->encoding = MIME_ENCODING_QUOTED;

		/* Base64. */
		same = mime_same(value, "base64", 6U);
		if (same)
			part->encoding = MIME_ENCODING_BASE64;
	}

	/* The disposition: an attachment, and its file name. */
	found = mime_field(part->header, part->header_length, "content-disposition", value, sizeof(value));
	if (found) {
		/* An attachment by its disposition. */
		same = mime_same(value, "attachment", 10U);
		if (same)
			part->attachment = 1;

		/* Its file name. */
		found = mime_parameter(value, "filename", name, sizeof(name));
		if (found)
			(void)mime_words(name, part->file_name, sizeof(part->file_name));
	}
}

/*
 * Finds a parameter of a field ("; name=value" or "; name="value"") and
 * gives its value.  Returns 1 when found.
 */
static int
mime_parameter(
	const char *value,
	const char *name,
	char *found,
	size_t size)
{
	const char *at;
	size_t name_length;
	size_t length;
	int same;

	/* Each parameter after a semicolon. */
	name_length = strlen(name);
	for (at = strchr(value, ';'); at != NULL; at = strchr(at + 1, ';')) {
		/* Its name, after the spaces. */
		at++;
		while (*at == ' ' || *at == '\t')
			at++;
		same = mime_same(at, name, name_length);
		if (!same || at[name_length] != '=')
			continue;

		/* A quoted value. */
		at += name_length + 1U;
		if (*at == '"') {
			at++;
			length = strcspn(at, "\"");
			mime_copy(found, size, at, length);
			return 1;
		}

		/* A plain value up to a semicolon or a space. */
		length = strcspn(at, "; \t");
		mime_copy(found, size, at, length);
		return 1;
	}

	/* Not there. */
	return 0;
}

/*
 * Walks a part for the words: a text/plain part not attached goes to
 * plain (the first only), a text/html one to html, a multipart's parts
 * are walked, and the first file carried is named.
 */
static int
mime_find_body(
	const struct mime_part *part,
	int depth,
	struct ml_parsed *parsed,
	struct mime_text *plain,
	struct mime_text *html)
{
	struct mime_part inner;
	char delimiter[132];
	size_t delimiter_length;
	const char *end;
	const char *at;
	const char *next;
	size_t estimate;
	int is_multipart;
	int is_plain;
	int is_html;
	int is_file;
	int differs;
	int error;

	/* Too deep: nothing more. */
	if (depth > MIME_DEPTH_MAX)
		return 0;

	/* A multipart: each part between its boundaries. */
	is_multipart = mime_same(part->type, "multipart/", 10U);
	if (is_multipart && part->boundary[0] != '\0') {
		/* The delimiter line "--boundary". */
		(void)snprintf(delimiter, sizeof(delimiter), "--%s", part->boundary);
		delimiter_length = strlen(delimiter);
		end = part->body + part->body_length;

		/* The first delimiter. */
		at = part->body;
		next = NULL;
		while (at + delimiter_length <= end) {
			/* A delimiter at a line's start. */
			differs = memcmp(at, delimiter, delimiter_length);
			if (differs == 0 && (at == part->body || at[-1] == '\n')) {
				next = at;
				break;
			}

			/* The next byte. */
			at++;
		}

		/* Each part, from after a delimiter's line to the next delimiter. */
		while (next != NULL) {
			/* The closing delimiter "--boundary--" ends the parts. */
			at = next + delimiter_length;
			if (at + 2 <= end && at[0] == '-' && at[1] == '-')
				break;

			/* The part starts after the delimiter's line. */
			while (at < end && *at != '\n')
				at++;
			if (at < end)
				at++;

			/* The next delimiter (at a line's start) ends it; none: the rest (a truncated message). */
			next = NULL;
			for (end = at; end + delimiter_length <= part->body + part->body_length; end++) {
				/* Only at a line's start. */
				if (end[-1] != '\n')
					continue;

				/* The delimiter. */
				differs = memcmp(end, delimiter, delimiter_length);
				if (differs == 0) {
					next = end;
					break;
				}
			}

			/* None: the part runs to the end. */
			if (next == NULL)
				end = part->body + part->body_length;

			/* The part, without the line end before the delimiter. */
			(void)mime_split(at, (size_t)(end - at), &inner);
			mime_part_describe(&inner);
			error = mime_find_body(&inner, depth + 1, parsed, plain, html);
			if (error != 0)
				return error;
			end = part->body + part->body_length;
		}

		/* Every part walked. */
		return 0;
	}

	/* The kind of part. */
	is_plain = mime_same(part->type, "text/plain", 10U);
	is_html = mime_same(part->type, "text/html", 9U);

	/* A file: attached, or a part not of text with a name or inside a multipart. */
	is_file = part->attachment;
	if (!is_plain && !is_html && part->file_name[0] != '\0')
		is_file = 1;
	if (!is_plain && !is_html && depth > 0)
		is_file = 1;

	/* A file carried: named (the first), its size estimated from its encoded bytes. */
	if (is_file) {
		if (parsed->file_name[0] == '\0') {
			mime_copy(parsed->file_name, sizeof(parsed->file_name), part->file_name, strlen(part->file_name));
			if (parsed->file_name[0] == '\0')
				mime_copy(parsed->file_name, sizeof(parsed->file_name), "attachment", 10U);
			estimate = part->body_length;
			if (part->encoding == MIME_ENCODING_BASE64)
				estimate = part->body_length / 4U * 3U;
			parsed->file_size = estimate;
		}

		/* A file is not the words. */
		return 0;
	}

	/* The first plain text. */
	if (is_plain && plain->length == 0U) {
		error = mime_decode_part(part, plain);
		if (error != 0)
			return error;
		return 0;
	}

	/* The first HTML. */
	if (is_html && html->length == 0U) {
		error = mime_decode_part(part, html);
		if (error != 0)
			return error;
		return 0;
	}

	/* Anything else is not shown. */
	return 0;
}

/* Decodes a text part's body (its transfer encoding, then its character set) into UTF-8. */
static int
mime_decode_part(
	const struct mime_part *part,
	struct mime_text *text)
{
	struct mime_text decoded;
	int error;

	/* The transfer encoding. */
	memset(&decoded, 0, sizeof(decoded));
	if (part->encoding == MIME_ENCODING_QUOTED)
		error = mime_quoted_decode(part->body, part->body_length, &decoded, 0);
	else if (part->encoding == MIME_ENCODING_BASE64)
		error = mime_base64_decode(part->body, part->body_length, &decoded);
	else
		error = mime_append(&decoded, part->body, part->body_length);
	if (error != 0) {
		free(decoded.bytes);
		return error;
	}

	/* The character set. */
	error = mime_to_utf8(decoded.bytes, decoded.length, part->charset, text);
	free(decoded.bytes);
	if (error != 0)
		return error;

	/* Succeeded: the text is UTF-8. */
	return 0;
}

/*
 * Appends bytes of a character set as UTF-8: UTF-8 and US-ASCII as they
 * are, ISO-8859-1 by its code points, the Japanese sets by jis.c, others
 * as they are.
 */
static int
mime_to_utf8(
	const char *bytes,
	size_t length,
	const char *charset,
	struct mime_text *text)
{
	unsigned long code_point;
	size_t index;
	int japanese;
	int state;
	int latin;
	int read;
	int error;

	/* A Japanese set: each character read, as its code point. */
	japanese = ml_jis_charset(charset);
	if (japanese != ML_JIS_NONE) {
		index = 0;
		state = 0;
		for (;;) {
			read = ml_jis_next(japanese, (const unsigned char *)bytes, length, &index, &state, &code_point);
			if (!read)
				break;
			error = mime_append_code_point(text, code_point);
			if (error != 0)
				return error;
		}

		/* An empty text is still a text. */
		error = mime_append(text, "", 0U);
		if (error != 0)
			return error;
		return 0;
	}

	/* ISO-8859-1 and its kin (Windows-1252 is near enough for the letters). */
	latin = mime_same(charset, "iso-8859-1", 10U);
	if (!latin)
		latin = mime_same(charset, "latin1", 6U);
	if (!latin)
		latin = mime_same(charset, "windows-1252", 12U);

	/* Not Latin-1: the bytes as they are. */
	if (!latin) {
		error = mime_append(text, bytes, length);
		if (error != 0)
			return error;
		return 0;
	}

	/* Each byte its code point. */
	for (index = 0; index < length; index++) {
		error = mime_append_code_point(text, (unsigned char)bytes[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text is UTF-8. */
	return 0;
}

/* Turns HTML into text: tags out (a line break for <br>, <p>, <div>, <tr>, <li>), <style> and <script> left out, entities decoded. */
static int
mime_html_to_text(
	const char *html,
	size_t length,
	struct mime_text *text)
{
	char tag[16];
	size_t index;
	size_t tag_length;
	size_t used;
	int skipping;
	int closing;
	int breaks;
	int hidden;
	int error;

	/* Each byte: a tag, an entity, or text. */
	skipping = 0;
	index = 0;
	error = 0;
	while (index < length && error == 0) {
		/* A tag: its name decides a line break or a skipped block. */
		if (html[index] == '<') {
			index++;
			closing = 0;
			if (index < length && html[index] == '/') {
				closing = 1;
				index++;
			}

			/* The tag's name, in lower case. */
			tag_length = 0;
			while (index < length && tag_length + 1U < sizeof(tag) && html[index] != '>' && html[index] != ' ' && html[index] != '/') {
				tag[tag_length] = (char)mime_lower((unsigned char)html[index]);
				tag_length++;
				index++;
			}

			/* Its end, and the rest of the tag. */
			tag[tag_length] = '\0';
			while (index < length && html[index] != '>')
				index++;
			index++;

			/* A style or a script is not shown. */
			hidden = mime_html_hidden(tag);
			if (hidden) {
				skipping = !closing;
				continue;
			}

			/* A line break, and a block that starts or ends a line (once, not at the start). */
			if (skipping)
				continue;
			breaks = mime_html_breaks(tag);
			if (breaks == 2)
				error = mime_append(text, "\n", 1U);
			else if (breaks == 1 && text->length > 0U && text->bytes[text->length - 1U] != '\n')
				error = mime_append(text, "\n", 1U);
			continue;
		}

		/* Inside a style or a script. */
		if (skipping) {
			index++;
			continue;
		}

		/* An entity. */
		if (html[index] == '&') {
			used = mime_html_entity(html + index, length - index, text, &error);
			index += used;
			continue;
		}

		/* A line end of the source is a space (HTML's lines are not the text's). */
		if (html[index] == '\r' || html[index] == '\n') {
			error = mime_append(text, " ", 1U);
			index++;
			continue;
		}

		/* A byte of text. */
		error = mime_append(text, html + index, 1U);
		index++;
	}

	/* A failure to grow the text. */
	if (error != 0)
		return error;

	/* The spaces and line ends at its end go. */
	while (text->length > 0U && (text->bytes[text->length - 1U] == '\n' || text->bytes[text->length - 1U] == ' ')) {
		text->length--;
		text->bytes[text->length] = '\0';
	}

	/* Succeeded: the text is made. */
	return 0;
}

/* Tells whether an HTML tag's content is not text (style, script). */
static int
mime_html_hidden(
	const char *tag)
{
	int same;

	/* A style. */
	same = strcmp(tag, "style");
	if (same == 0)
		return 1;

	/* A script. */
	same = strcmp(tag, "script");
	if (same == 0)
		return 1;

	/* Text. */
	return 0;
}

/* Tells how an HTML tag breaks the text: 2 a line break (br), 1 a block (p, div, tr, li, h1-h6), 0 none. */
static int
mime_html_breaks(
	const char *tag)
{
	static const char *const blocks[] = { "p", "div", "tr", "li", "h1", "h2", "h3", "h4", "h5", "h6", "table", "ul", "ol" };
	size_t index;
	int same;

	/* A line break. */
	same = strcmp(tag, "br");
	if (same == 0)
		return 2;

	/* A block. */
	for (index = 0; index < sizeof(blocks) / sizeof(blocks[0]); index++) {
		same = strcmp(tag, blocks[index]);
		if (same == 0)
			return 1;
	}

	/* Inline. */
	return 0;
}

/* Decodes one entity at the start of html into the text; returns the bytes it took. */
static size_t
mime_html_entity(
	const char *html,
	size_t length,
	struct mime_text *text,
	int *error)
{
	static const char *const names[] = { "amp", "lt", "gt", "quot", "apos", "nbsp" };
	static const char *const values[] = { "&", "<", ">", "\"", "'", " " };
	unsigned long code_point;
	size_t name_length;
	size_t end;
	size_t index;
	int differs;
	int digit;

	/* The entity's end. */
	end = 1;
	while (end < length && end < 12U && html[end] != ';')
		end++;

	/* No end: the ampersand is text. */
	if (end >= length || html[end] != ';') {
		*error = mime_append(text, "&", 1U);
		return 1;
	}

	/* A number: &#NN; or &#xHH;. */
	if (html[1] == '#') {
		code_point = 0;
		for (index = 2; index < end; index++) {
			/* A hexadecimal number. */
			if (index == 2 && (html[index] == 'x' || html[index] == 'X'))
				continue;
			digit = mime_hex((unsigned char)html[index]);
			if (digit < 0)
				break;
			if (html[2] == 'x' || html[2] == 'X')
				code_point = code_point * 16UL + (unsigned long)digit;
			else
				code_point = code_point * 10UL + (unsigned long)digit;
		}

		/* Its character. */
		*error = mime_append_code_point(text, code_point);
		return end + 1U;
	}

	/* A name. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		/* Not this name's length. */
		name_length = strlen(names[index]);
		if (name_length != end - 1U)
			continue;

		/* This name. */
		differs = memcmp(html + 1, names[index], name_length);
		if (differs == 0) {
			*error = mime_append(text, values[index], strlen(values[index]));
			return end + 1U;
		}
	}

	/* An entity not known: kept as text. */
	*error = mime_append(text, html, end + 1U);
	return end + 1U;
}

/* Decodes a field's value with its encoded words ("=?UTF-8?B?...?=") into UTF-8 in a room. */
static int
mime_words(
	const char *value,
	char *decoded,
	size_t size)
{
	struct mime_text text;
	const char *at;
	const char *end;
	const char *gap;
	int last_was_word;
	int error;

	/* Each encoded word decoded, the text between them as it is (only spaces between two words go). */
	memset(&text, 0, sizeof(text));
	at = value;
	last_was_word = 0;
	error = 0;
	while (*at != '\0' && error == 0) {
		/* An encoded word. */
		end = mime_word_end(at);
		if (end != NULL) {
			error = mime_word(at, (size_t)(end + 2 - at), &text);
			at = end + 2;
			last_was_word = 1;
			continue;
		}

		/* Spaces between two words go. */
		if (last_was_word && (*at == ' ' || *at == '\t')) {
			gap = at;
			while (*gap == ' ' || *gap == '\t')
				gap++;
			end = mime_word_end(gap);
			if (end != NULL) {
				at = gap;
				continue;
			}
		}

		/* A byte of text. */
		error = mime_append(&text, at, 1U);
		at++;
		last_was_word = 0;
	}

	/* The text into the room. */
	decoded[0] = '\0';
	if (text.bytes != NULL)
		mime_copy(decoded, size, text.bytes, text.length);
	free(text.bytes);

	/* A failure to grow the text. */
	if (error != 0)
		return error;

	/* Succeeded: the value is decoded. */
	return 0;
}

/*
 * Finds where an encoded word "=?charset?E?text?=" starting at a text
 * ends: the "?=" after its text, or NULL when the text does not start
 * with a whole encoded word.
 */
static const char *
mime_word_end(
	const char *at)
{
	const char *charset_end;
	const char *end;

	/* Its start. */
	if (at[0] != '=' || at[1] != '?')
		return NULL;

	/* The charset, up to its question mark. */
	charset_end = strchr(at + 2, '?');
	if (charset_end == NULL || charset_end == at + 2)
		return NULL;

	/* The encoding, B or Q, between two question marks. */
	if (charset_end[1] == '\0' || charset_end[2] != '?')
		return NULL;

	/* The text's end. */
	end = strstr(charset_end + 3, "?=");
	return end;
}

/* Decodes one encoded word "=?charset?B|Q?text?=" into the text. */
static int
mime_word(
	const char *word,
	size_t length,
	struct mime_text *text)
{
	struct mime_text bytes;
	char charset[32];
	const char *first;
	const char *second;
	int encoding;
	int error;

	/* The charset and the encoding between the question marks. */
	first = strchr(word + 2, '?');
	second = strchr(first + 1, '?');
	mime_copy(charset, sizeof(charset), word + 2, (size_t)(first - word - 2));
	encoding = mime_lower((unsigned char)first[1]);

	/* The encoded text, decoded. */
	memset(&bytes, 0, sizeof(bytes));
	if (encoding == 'b')
		error = mime_base64_decode(second + 1, (size_t)(word + length - 2 - (second + 1)), &bytes);
	else
		error = mime_quoted_decode(second + 1, (size_t)(word + length - 2 - (second + 1)), &bytes, 1);
	if (error == 0)
		error = mime_to_utf8(bytes.bytes, bytes.length, charset, text);
	free(bytes.bytes);
	if (error != 0)
		return error;

	/* Succeeded: the word is text. */
	return 0;
}

/* Reads an address field ("Name <addr>", "\"Name\" <addr>" or "addr") into its name and address. */
static void
mime_address(
	const char *value,
	char *name,
	size_t name_size,
	char *address,
	size_t address_size)
{
	char decoded[ML_TEXT_MAX];
	const char *open;
	const char *close;
	size_t length;

	/* Nothing yet. */
	name[0] = '\0';
	address[0] = '\0';

	/* The address in angle brackets, and the name before it. */
	open = strchr(value, '<');
	close = NULL;
	if (open != NULL)
		close = strchr(open, '>');
	if (open != NULL && close != NULL) {
		mime_copy(address, address_size, open + 1, (size_t)(close - open - 1));
		mime_trim(address);
		length = (size_t)(open - value);
		mime_copy(decoded, sizeof(decoded), value, length);
	} else {
		/* The whole value is the address. */
		mime_copy(address, address_size, value, strlen(value));
		mime_trim(address);
		decoded[0] = '\0';
	}

	/* The name, its quotes taken off and its words decoded. */
	mime_trim(decoded);
	length = strlen(decoded);
	if (length >= 2U && decoded[0] == '"' && decoded[length - 1U] == '"') {
		memmove(decoded, decoded + 1, length - 2U);
		decoded[length - 2U] = '\0';
	}

	/* Its encoded words. */
	(void)mime_words(decoded, name, name_size);

	/* No name: the address stands for it. */
	if (name[0] == '\0')
		mime_copy(name, name_size, address, strlen(address));
}

/* Reads a date ("Mon, 5 Oct 2026 09:41:00 +0900") as a time; 0 when it cannot be read. */
static time_t
mime_date(
	const char *value)
{
	struct tm parts;
	char month[4];
	char zone[8];
	const char *at;
	time_t moment;
	long offset;
	int matched;
	int day;
	int year;
	int hour;
	int minute;
	int second;
	int index;
	int same;
	size_t zone_length;

	/* After a day's name and its comma. */
	at = strchr(value, ',');
	if (at == NULL)
		at = value;
	else
		at++;

	/* The day, the month, the year and the time (the seconds may be missing). */
	second = 0;
	zone[0] = '\0';
	matched = sscanf(at, " %d %3s %d %d:%d:%d %7s", &day, month, &year, &hour, &minute, &second, zone);
	if (matched < 5)
		return 0;
	if (matched == 5) {
		matched = sscanf(at, " %d %3s %d %d:%d %7s", &day, month, &year, &hour, &minute, zone);
		second = 0;
	}

	/* The month's number. */
	memset(&parts, 0, sizeof(parts));
	parts.tm_mon = -1;
	for (index = 0; index < 12; index++) {
		same = mime_same(month, mime_months[index], 3U);
		if (same)
			parts.tm_mon = index;
	}

	/* A month not known. */
	if (parts.tm_mon < 0)
		return 0;

	/* A two-digit year is of this century or the last (RFC 5322 section 4.3). */
	if (year < 50)
		year += 2000;
	else if (year < 1000)
		year += 1900;

	/* The time in UTC before the zone. */
	parts.tm_year = year - 1900;
	parts.tm_mday = day;
	parts.tm_hour = hour;
	parts.tm_min = minute;
	parts.tm_sec = second;
	moment = timegm(&parts);

	/* The zone's offset, +hhmm or -hhmm (a name such as GMT is UTC). */
	offset = 0;
	zone_length = strlen(zone);
	if ((zone[0] == '+' || zone[0] == '-') && zone_length == 5U) {
		offset = ((long)(zone[1] - '0') * 10L + (long)(zone[2] - '0')) * 3600L + ((long)(zone[3] - '0') * 10L + (long)(zone[4] - '0')) * 60L;
		if (zone[0] == '-')
			offset = -offset;
	}

	/* The moment in UTC. */
	return moment - (time_t)offset;
}

/* Decodes quoted-printable (with underscore_space, an encoded word's Q: "_" is a space). */
static int
mime_quoted_decode(
	const char *bytes,
	size_t length,
	struct mime_text *text,
	int underscore_space)
{
	char byte;
	size_t index;
	int high;
	int low;
	int error;

	/* Each byte: "=XX", a soft line break "=" at a line's end, "_", or itself. */
	error = 0;
	for (index = 0; index < length && error == 0; index++) {
		/* An escape. */
		if (bytes[index] == '=') {
			/* A soft line break (with or without its CR). */
			if (index + 1U < length && bytes[index + 1U] == '\n') {
				index++;
				continue;
			}

			/* With its CR. */
			if (index + 2U < length && bytes[index + 1U] == '\r' && bytes[index + 2U] == '\n') {
				index += 2U;
				continue;
			}

			/* Two hexadecimal digits. */
			high = -1;
			low = -1;
			if (index + 2U < length) {
				high = mime_hex((unsigned char)bytes[index + 1U]);
				low = mime_hex((unsigned char)bytes[index + 2U]);
			}

			/* A byte by its digits; anything else stands as itself. */
			if (high >= 0 && low >= 0) {
				byte = (char)(high * 16 + low);
				error = mime_append(text, &byte, 1U);
				index += 2U;
				continue;
			}
		}

		/* An encoded word's underscore. */
		if (underscore_space && bytes[index] == '_') {
			error = mime_append(text, " ", 1U);
			continue;
		}

		/* The byte itself. */
		error = mime_append(text, bytes + index, 1U);
	}

	/* A failure to grow the text. */
	if (error != 0)
		return error;

	/* Succeeded: the bytes are decoded. */
	return 0;
}

/* Decodes base64, skipping line ends and anything not of its alphabet. */
static int
mime_base64_decode(
	const char *bytes,
	size_t length,
	struct mime_text *text)
{
	unsigned long group;
	char out[3];
	size_t index;
	int count;
	int value;
	int error;

	/* Each character of the alphabet, four making three bytes. */
	group = 0;
	count = 0;
	error = 0;
	for (index = 0; index < length && error == 0; index++) {
		/* Padding ends the data. */
		if (bytes[index] == '=')
			break;

		/* Anything not of the alphabet (line ends) is skipped. */
		value = mime_base64_value((unsigned char)bytes[index]);
		if (value < 0)
			continue;

		/* Six more bits. */
		group = (group << 6) | (unsigned long)value;
		count++;
		if (count == 4) {
			out[0] = (char)((group >> 16) & 0xffU);
			out[1] = (char)((group >> 8) & 0xffU);
			out[2] = (char)(group & 0xffU);
			error = mime_append(text, out, 3U);
			group = 0;
			count = 0;
		}
	}

	/* The last bytes of a group cut by the padding. */
	if (error == 0 && count == 2) {
		out[0] = (char)((group >> 4) & 0xffU);
		error = mime_append(text, out, 1U);
	} else if (error == 0 && count == 3) {
		out[0] = (char)((group >> 10) & 0xffU);
		out[1] = (char)((group >> 2) & 0xffU);
		error = mime_append(text, out, 2U);
	}

	/* A failure to grow the text. */
	if (error != 0)
		return error;

	/* Succeeded: the bytes are decoded. */
	return 0;
}

/* Reports a base64 character's value, or -1 for one not of the alphabet. */
static int
mime_base64_value(
	int c)
{
	/* The capitals. */
	if (c >= 'A' && c <= 'Z')
		return c - 'A';

	/* The small letters. */
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;

	/* The digits. */
	if (c >= '0' && c <= '9')
		return c - '0' + 52;

	/* The two signs. */
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;

	/* Not of the alphabet. */
	return -1;
}

/* Appends bytes to a text, growing its room; it stays NUL ended. */
static int
mime_append(
	struct mime_text *text,
	const char *bytes,
	size_t length)
{
	size_t capacity;
	char *grown;

	/* Room for the bytes and the NUL. */
	if (text->length + length + 1U > text->capacity) {
		capacity = text->capacity * 2U;
		if (capacity < 256U)
			capacity = 256U;
		while (capacity < text->length + length + 1U)
			capacity *= 2U;
		grown = realloc(text->bytes, capacity);
		if (grown == NULL)
			return ENOMEM;
		text->bytes = grown;
		text->capacity = capacity;
	}

	/* The bytes and the NUL. */
	memcpy(text->bytes + text->length, bytes, length);
	text->length += length;
	text->bytes[text->length] = '\0';
	return 0;
}

/* Appends a code point as UTF-8. */
static int
mime_append_code_point(
	struct mime_text *text,
	unsigned long code_point)
{
	char bytes[4];
	size_t length;

	/* Not a character: the replacement character. */
	if (code_point > 0x10ffffUL || (code_point >= 0xd800UL && code_point <= 0xdfffUL))
		code_point = 0xfffdUL;

	/* One to four bytes. */
	if (code_point < 0x80UL) {
		bytes[0] = (char)code_point;
		length = 1;
	} else if (code_point < 0x800UL) {
		bytes[0] = (char)(0xc0UL | (code_point >> 6));
		bytes[1] = (char)(0x80UL | (code_point & 0x3fUL));
		length = 2;
	} else if (code_point < 0x10000UL) {
		bytes[0] = (char)(0xe0UL | (code_point >> 12));
		bytes[1] = (char)(0x80UL | ((code_point >> 6) & 0x3fUL));
		bytes[2] = (char)(0x80UL | (code_point & 0x3fUL));
		length = 3;
	} else {
		bytes[0] = (char)(0xf0UL | (code_point >> 18));
		bytes[1] = (char)(0x80UL | ((code_point >> 12) & 0x3fUL));
		bytes[2] = (char)(0x80UL | ((code_point >> 6) & 0x3fUL));
		bytes[3] = (char)(0x80UL | (code_point & 0x3fUL));
		length = 4;
	}

	/* Appended. */
	return mime_append(text, bytes, length);
}

/* Reports a hexadecimal digit's value, or -1. */
static int
mime_hex(
	int c)
{
	/* A decimal digit. */
	if (c >= '0' && c <= '9')
		return c - '0';

	/* A small letter. */
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;

	/* A capital. */
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	/* Not a digit. */
	return -1;
}

/* Tells whether the first length bytes of a text are a name, in any case (the text may be shorter). */
static int
mime_same(
	const char *a,
	const char *b,
	size_t length)
{
	size_t index;
	int left;
	int right;

	/* Each byte in lower case. */
	for (index = 0; index < length; index++) {
		left = mime_lower((unsigned char)a[index]);
		right = mime_lower((unsigned char)b[index]);
		if (left != right)
			return 0;

		/* Both ended together. */
		if (left == '\0')
			return 1;
	}

	/* The same. */
	return 1;
}

/* Reports an ASCII capital made small, anything else as it is. */
static int
mime_lower(
	int c)
{
	/* A capital. */
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 'a';

	/* Anything else. */
	return c;
}

/* Copies length bytes into a room, cut to it, NUL ended. */
static void
mime_copy(
	char *to,
	size_t size,
	const char *from,
	size_t length)
{
	/* No more than fits. */
	if (length >= size)
		length = size - 1U;
	memcpy(to, from, length);
	to[length] = '\0';
}

/* Takes the spaces and tabs off both ends of a text. */
static void
mime_trim(
	char *text)
{
	size_t start;
	size_t length;

	/* The leading ones. */
	start = 0;
	while (text[start] == ' ' || text[start] == '\t')
		start++;
	length = strlen(text + start);
	memmove(text, text + start, length + 1U);

	/* The trailing ones. */
	while (length > 0U && (text[length - 1U] == ' ' || text[length - 1U] == '\t' || text[length - 1U] == '\r' || text[length - 1U] == '\n')) {
		length--;
		text[length] = '\0';
	}
}
