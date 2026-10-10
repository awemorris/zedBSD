/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bMessage of the Message Access Profile (ws197-p003, see bmsg.h).
 */

#include "userland/base/bluetoothd/bmsg.h"
#include "userland/base/bluetoothd/mms.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The lines that open and close the parts of a bMessage. */
#define BMSG_NOT_MARKER			0
#define BMSG_BEGIN_BMSG			1
#define BMSG_END_BMSG			2
#define BMSG_BEGIN_VCARD		3
#define BMSG_END_VCARD			4
#define BMSG_BEGIN_BENV			5
#define BMSG_END_BENV			6
#define BMSG_BEGIN_BBODY		7
#define BMSG_END_BBODY			8
#define BMSG_BEGIN_MSG			9
#define BMSG_END_MSG			10

/* Whose vCard is being read: none, the originator's, the first recipient's, or one not kept. */
#define BMSG_VCARD_NONE			0
#define BMSG_VCARD_ORIGINATOR		1
#define BMSG_VCARD_RECIPIENT		2
#define BMSG_VCARD_IGNORED		3

/* The longest LENGTH read, in digits. */
#define BMSG_LENGTH_DIGITS		6U

/* The bytes of BEGIN:MSG and END:MSG lines with their CRLF, which LENGTH counts as the profile says. */
#define BMSG_BEGIN_MSG_BYTES		11U
#define BMSG_END_MSG_BYTES		11U

/* U+FFFD, which stands for a malformed sequence of the text. */
#define BMSG_REPLACEMENT		"\xef\xbf\xbd"
#define BMSG_REPLACEMENT_BYTES		3U

/*
 * One line of the input: where it starts, where its text ends (its line
 * break and a carriage return before it left out), and where the next
 * line starts.
 */
struct bmsg_line {
	size_t start;
	size_t end;
	size_t next;
};

/*
 * The reading of one bMessage: the input and where the reading is, the
 * part being read (the depth of envelopes, whose vCard, whether inside
 * the body), what has been found so far, and what the body's properties
 * said about its text.  It lives for one btd_bmsg_parse.
 */
struct bmsg_reader {
	const uint8_t *bytes;
	size_t length;
	size_t at;
	int depth;
	int vcard;
	int in_body;
	int seen_originator;
	int seen_recipient;
	int text_found;
	int ended;
	int has_length;
	size_t length_value;
	int has_charset;
	int charset_utf8;
	int has_encoding;
	struct btd_bmsg *message;
};

/* A word of a bMessage, and the value the reader keeps for it. */
struct bmsg_word {
	const char *name;
	int value;
};

/* The marker lines. */
static const struct bmsg_word bmsg_markers[] = {
	{ "BEGIN:BMSG", BMSG_BEGIN_BMSG },
	{ "END:BMSG", BMSG_END_BMSG },
	{ "BEGIN:VCARD", BMSG_BEGIN_VCARD },
	{ "END:VCARD", BMSG_END_VCARD },
	{ "BEGIN:BENV", BMSG_BEGIN_BENV },
	{ "END:BENV", BMSG_END_BENV },
	{ "BEGIN:BBODY", BMSG_BEGIN_BBODY },
	{ "END:BBODY", BMSG_END_BBODY },
	{ "BEGIN:MSG", BMSG_BEGIN_MSG },
	{ "END:MSG", BMSG_END_MSG },
	{ NULL, 0 }
};

/* The types a message's TYPE names. */
static const struct bmsg_word bmsg_types[] = {
	{ "SMS_GSM", BTD_MAP_TYPE_SMS_GSM },
	{ "SMS_CDMA", BTD_MAP_TYPE_SMS_CDMA },
	{ "MMS", BTD_MAP_TYPE_MMS },
	{ "EMAIL", BTD_MAP_TYPE_EMAIL },
	{ "IM", BTD_MAP_TYPE_IM },
	{ NULL, 0 }
};

static int bmsg_line_next(struct bmsg_reader *reader, struct bmsg_line *line);
static void bmsg_trim(const uint8_t *bytes, size_t *start, size_t *end);
static int bmsg_word(const struct bmsg_word *words, const uint8_t *bytes, size_t start, size_t end, int otherwise);
static int bmsg_same(const uint8_t *bytes, size_t start, size_t end, const char *word);
static int bmsg_marker(const struct bmsg_reader *reader, const struct bmsg_line *line);
static int bmsg_structure(struct bmsg_reader *reader, const struct bmsg_line *line, int marker);
static void bmsg_property(struct bmsg_reader *reader, const struct bmsg_line *line);
static void bmsg_vcard_property(struct bmsg_reader *reader, const uint8_t *name_bytes, size_t name_start, size_t name_end, size_t value_start, size_t value_end);
static void bmsg_body_property(struct bmsg_reader *reader, size_t name_start, size_t name_end, size_t value_start, size_t value_end);
static void bmsg_message_property(struct bmsg_reader *reader, size_t name_start, size_t name_end, size_t value_start, size_t value_end);
static int bmsg_text(struct bmsg_reader *reader, const struct bmsg_line *line);
static int bmsg_text_by_length(struct bmsg_reader *reader, size_t begin, size_t body_start, size_t *end_line);
static int bmsg_text_by_scan(struct bmsg_reader *reader, size_t body_start, size_t *end_line);
static int bmsg_end_line(const struct bmsg_reader *reader, size_t line_start, size_t body_start);
static size_t bmsg_line_start_before(const struct bmsg_reader *reader, size_t at);
static int bmsg_take_text(struct bmsg_reader *reader, size_t body_start, size_t end_line);
static int bmsg_put_text(struct btd_bmsg *message, const uint8_t *bytes, size_t length);
static size_t bmsg_utf8_length(const uint8_t *bytes, size_t available);
static void bmsg_copy(char *text, size_t size, const uint8_t *bytes, size_t start, size_t end);
static void bmsg_name_from_n(char *text, size_t size, const uint8_t *bytes, size_t start, size_t end);
static int bmsg_put(uint8_t *output, size_t size, size_t *used, const void *data, size_t length);
static int bmsg_put_string(uint8_t *output, size_t size, size_t *used, const char *text);
static int bmsg_put_body(uint8_t *output, size_t size, size_t *used, const uint8_t *text, size_t length);
static size_t bmsg_body_bytes(const uint8_t *text, size_t length);
static int bmsg_escaped_line(const uint8_t *text, size_t length, size_t at);

/*
 * Reads a bMessage into message.  Returns 0, EINVAL for one malformed
 * (no BEGIN:BMSG first or END:BMSG at its end, parts out of order,
 * envelopes nested deeper than three, no text, a text not in UTF-8), or
 * E2BIG for one past BTD_BMSG_INPUT_MAX.
 */
int
btd_bmsg_parse(
	const uint8_t *input,
	size_t length,
	struct btd_bmsg *message)
{
	struct bmsg_reader reader;
	struct bmsg_line line;
	int marker;
	int error;

	/* Nothing known of the message yet. */
	memset(message, 0, sizeof(*message));
	message->status = BTD_BMSG_STATUS_UNKNOWN;
	message->type = BTD_MAP_TYPE_OTHER;

	/* A bMessage within the size the daemon reads. */
	if (length > BTD_BMSG_INPUT_MAX)
		return E2BIG;

	/* The reading starts at the first line. */
	memset(&reader, 0, sizeof(reader));
	reader.bytes = input;
	reader.length = length;
	reader.message = message;

	/* The first line opens the bMessage. */
	error = bmsg_line_next(&reader, &line);
	if (error != 0)
		return EINVAL;
	marker = bmsg_marker(&reader, &line);
	if (marker != BMSG_BEGIN_BMSG)
		return EINVAL;

	/* Each line until the bMessage closes. */
	while (!reader.ended) {
		/* A bMessage that never closes. */
		error = bmsg_line_next(&reader, &line);
		if (error != 0) {
			memset(message, 0, sizeof(*message));
			return EINVAL;
		}

		/* A marker changes the part being read; another line is a property of it. */
		marker = bmsg_marker(&reader, &line);
		if (marker == BMSG_NOT_MARKER) {
			bmsg_property(&reader, &line);
			continue;
		}

		/* The part the marker opens or closes. */
		error = bmsg_structure(&reader, &line, marker);
		if (error != 0) {
			memset(message, 0, sizeof(*message));
			return error;
		}
	}

	/* A bMessage without a text. */
	if (!reader.text_found) {
		memset(message, 0, sizeof(*message));
		return EINVAL;
	}

	/* Succeeded: the message read. */
	return 0;
}

/* Tells whether a number is one a message may be sent to: 1 to 32 of 0-9, +, * and #. */
int
btd_bmsg_number_ok(
	const char *number)
{
	size_t length;
	size_t index;
	char letter;

	/* 1 to 32 characters. */
	length = strlen(number);
	if (length == 0U || length > BTD_BMSG_NUMBER_MAX)
		return 0;

	/* Each one a digit or a dialling sign. */
	for (index = 0U; index < length; index++) {
		letter = number[index];
		if (letter >= '0' && letter <= '9')
			continue;

		/* A sign the network dials. */
		if (letter == '+' ||
		    letter == '*' ||
		    letter == '#')
			continue;

		/* Anything else is not a number. */
		return 0;
	}

	/* Succeeded: a number. */
	return 1;
}

/* Tells whether a text is well-formed UTF-8 without a NUL. */
int
btd_bmsg_utf8_ok(
	const uint8_t *text,
	size_t length)
{
	size_t at;
	size_t character;

	/* Each character. */
	at = 0U;
	while (at < length) {
		/* A NUL is not taken in a text. */
		if (text[at] == 0U)
			return 0;

		/* A well-formed character. */
		character = bmsg_utf8_length(text + at, length - at);
		if (character == 0U)
			return 0;
		at += character;
	}

	/* Succeeded: UTF-8 throughout. */
	return 1;
}

/*
 * Builds the bMessage that sends text to number (MAP section 3.1.3): no
 * originator, an empty folder, one recipient's vCard 2.1, the text in
 * UTF-8 with its line breaks made CRLF and each line starting with
 * END:MSG (after any slashes) given one more slash, and LENGTH counted
 * from BEGIN:MSG to the CRLF after END:MSG.  type is BTD_MAP_TYPE_SMS_GSM
 * or BTD_MAP_TYPE_SMS_CDMA.  Returns 0 with the bytes used, EINVAL for a
 * number, a text or a type that cannot be sent, or ENOBUFS when size has
 * no room.
 */
int
btd_bmsg_build(
	const char *number,
	const uint8_t *text,
	size_t length,
	int type,
	uint8_t *output,
	size_t size,
	size_t *used)
{
	char line[96];
	const char *type_name;
	size_t body;
	int number_ok;
	int text_ok;
	int written;
	int error;

	/* A number the message can go to. */
	number_ok = btd_bmsg_number_ok(number);
	if (!number_ok)
		return EINVAL;

	/* A text of 1 to 8192 bytes of UTF-8 without a NUL. */
	if (length == 0U || length > BTD_BMSG_SEND_MAX)
		return EINVAL;
	text_ok = btd_bmsg_utf8_ok(text, length);
	if (!text_ok)
		return EINVAL;

	/* The type's name. */
	if (type == BTD_MAP_TYPE_SMS_GSM) {
		type_name = "SMS_GSM";
	} else if (type == BTD_MAP_TYPE_SMS_CDMA) {
		type_name = "SMS_CDMA";
	} else {
		return EINVAL;
	}

	/* The message's properties, then the envelope and the recipient's vCard. */
	*used = 0U;
	written = snprintf(line, sizeof(line), "BEGIN:BMSG\r\nVERSION:1.0\r\nSTATUS:READ\r\nTYPE:%s\r\nFOLDER:\r\n", type_name);
	error = bmsg_put(output, size, used, line, (size_t)written);
	if (error != 0)
		return error;
	error = bmsg_put_string(output, size, used, "BEGIN:BENV\r\nBEGIN:VCARD\r\nVERSION:2.1\r\nN:\r\nTEL:");
	if (error != 0)
		return error;
	error = bmsg_put_string(output, size, used, number);
	if (error != 0)
		return error;
	error = bmsg_put_string(output, size, used, "\r\nEND:VCARD\r\n");
	if (error != 0)
		return error;

	/* The body's properties: the text's bytes as sent, and the two marker lines around it. */
	body = bmsg_body_bytes(text, length);
	written = snprintf(line, sizeof(line), "BEGIN:BBODY\r\nCHARSET:UTF-8\r\nLENGTH:%lu\r\nBEGIN:MSG\r\n",
		(unsigned long)(BMSG_BEGIN_MSG_BYTES + body + BMSG_END_MSG_BYTES));
	error = bmsg_put(output, size, used, line, (size_t)written);
	if (error != 0)
		return error;

	/* The text. */
	error = bmsg_put_body(output, size, used, text, length);
	if (error != 0)
		return error;

	/* The end of the text, the body, the envelope and the bMessage. */
	error = bmsg_put_string(output, size, used, "\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n");
	if (error != 0)
		return error;

	/* Succeeded: the bMessage built. */
	return 0;
}

/*
 * Reads the line at the reading and moves past it.  Returns 0, or ENOENT
 * when the input has ended.
 */
static int
bmsg_line_next(
	struct bmsg_reader *reader,
	struct bmsg_line *line)
{
	const uint8_t *newline;

	/* The input has ended. */
	if (reader->at >= reader->length)
		return ENOENT;

	/* The line up to its line feed, or to the input's end. */
	line->start = reader->at;
	newline = memchr(reader->bytes + reader->at, '\n', reader->length - reader->at);
	if (newline == NULL) {
		line->end = reader->length;
		line->next = reader->length;
	} else {
		line->end = (size_t)(newline - reader->bytes);
		line->next = line->end + 1U;
	}

	/* The carriage return of a CRLF is not part of the line. */
	if (line->end > line->start && reader->bytes[line->end - 1U] == '\r')
		line->end--;

	/* Succeeded: past the line. */
	reader->at = line->next;
	return 0;
}

/* Narrows start and end past the spaces and tabs at both ends. */
static void
bmsg_trim(
	const uint8_t *bytes,
	size_t *start,
	size_t *end)
{
	/* The spaces before. */
	while (*start < *end &&
	       (bytes[*start] == ' ' ||
		bytes[*start] == '\t'))
		(*start)++;

	/* The spaces after. */
	while (*end > *start &&
	       (bytes[*end - 1U] == ' ' ||
		bytes[*end - 1U] == '\t'))
		(*end)--;
}

/* Gives the value of the word from start to end in a table (case not minded), or otherwise. */
static int
bmsg_word(
	const struct bmsg_word *words,
	const uint8_t *bytes,
	size_t start,
	size_t end,
	int otherwise)
{
	size_t index;
	int same;

	/* Each word of the table. */
	for (index = 0U; words[index].name != NULL; index++) {
		same = bmsg_same(bytes, start, end, words[index].name);
		if (same)
			return words[index].value;
	}

	/* A word the table does not know. */
	return otherwise;
}

/* Tells whether the bytes from start to end are word, ignoring the case of ASCII letters. */
static int
bmsg_same(
	const uint8_t *bytes,
	size_t start,
	size_t end,
	const char *word)
{
	size_t length;
	size_t index;
	uint8_t a;
	uint8_t b;

	/* The same length. */
	length = strlen(word);
	if (end - start != length)
		return 0;

	/* Each letter, both in lower case. */
	for (index = 0U; index < length; index++) {
		a = bytes[start + index];
		b = (uint8_t)word[index];
		if (a >= 'A' && a <= 'Z')
			a = (uint8_t)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = (uint8_t)(b - 'A' + 'a');

		/* A difference. */
		if (a != b)
			return 0;
	}

	/* Succeeded: the same word. */
	return 1;
}

/* Gives the marker a line is (its spaces at both ends not minded), or BMSG_NOT_MARKER. */
static int
bmsg_marker(
	const struct bmsg_reader *reader,
	const struct bmsg_line *line)
{
	size_t start;
	size_t end;
	int marker;

	/* The line without its spaces. */
	start = line->start;
	end = line->end;
	bmsg_trim(reader->bytes, &start, &end);

	/* The marker it is. */
	marker = bmsg_word(bmsg_markers, reader->bytes, start, end, BMSG_NOT_MARKER);

	/* The marker, or none. */
	return marker;
}

/*
 * Takes a marker line: opens or closes the part it names, in the order
 * the profile gives them.  Returns 0, or EINVAL for a part out of order
 * or envelopes nested too deep.
 */
static int
bmsg_structure(
	struct bmsg_reader *reader,
	const struct bmsg_line *line,
	int marker)
{
	int error;

	/* Inside a vCard only its end counts. */
	if (reader->vcard != BMSG_VCARD_NONE) {
		if (marker == BMSG_END_VCARD)
			reader->vcard = BMSG_VCARD_NONE;
		return 0;
	}

	/* Each marker. */
	switch (marker) {
	case BMSG_BEGIN_VCARD:
		/* A vCard is not part of a body. */
		if (reader->in_body)
			return EINVAL;

		/* Outside the envelopes: the originator's, the first one only. */
		if (reader->depth == 0) {
			reader->vcard = BMSG_VCARD_IGNORED;
			if (!reader->seen_originator)
				reader->vcard = BMSG_VCARD_ORIGINATOR;
			reader->seen_originator = 1;
			return 0;
		}

		/* In the outer envelope: its first recipient (the message's final one). */
		reader->vcard = BMSG_VCARD_IGNORED;
		if (reader->depth == 1 && !reader->seen_recipient) {
			reader->vcard = BMSG_VCARD_RECIPIENT;
			reader->seen_recipient = 1;
		}

		/* The vCard is read on. */
		return 0;
	case BMSG_BEGIN_BENV:
		/* Another envelope inside, at most three deep. */
		if (reader->in_body)
			return EINVAL;
		reader->depth++;
		if (reader->depth > BTD_BMSG_ENVELOPES_MAX)
			return EINVAL;
		return 0;
	case BMSG_END_BENV:
		/* An envelope closed, its body closed before it. */
		if (reader->in_body || reader->depth == 0)
			return EINVAL;
		reader->depth--;
		return 0;
	case BMSG_BEGIN_BBODY:
		/* A body inside an envelope, not inside another body. */
		if (reader->in_body || reader->depth == 0)
			return EINVAL;
		reader->in_body = 1;
		reader->has_length = 0;
		reader->has_charset = 0;
		reader->charset_utf8 = 0;
		reader->has_encoding = 0;
		return 0;
	case BMSG_END_BBODY:
		/* A body closed. */
		if (!reader->in_body)
			return EINVAL;
		reader->in_body = 0;
		return 0;
	case BMSG_BEGIN_MSG:
		/* A text inside a body. */
		if (!reader->in_body)
			return EINVAL;

		/* The text, found and kept. */
		error = bmsg_text(reader, line);
		if (error != 0)
			return error;
		return 0;
	case BMSG_END_MSG:
		/* The end of a text, read past by the text itself. */
		if (!reader->in_body)
			return EINVAL;
		return 0;
	case BMSG_END_BMSG:
		/* The bMessage closed, every envelope and body closed before it. */
		if (reader->in_body || reader->depth != 0)
			return EINVAL;
		reader->ended = 1;
		return 0;
	default:
		/* A second BEGIN:BMSG, or an END:VCARD outside a vCard. */
		return EINVAL;
	}
}

/* Takes a line NAME[;PARAMETERS]:VALUE of the part being read; a line without a ':' is ignored. */
static void
bmsg_property(
	struct bmsg_reader *reader,
	const struct bmsg_line *line)
{
	const uint8_t *colon;
	const uint8_t *semicolon;
	size_t name_start;
	size_t name_end;
	size_t value_start;
	size_t value_end;

	/* The ':' that ends the name and its parameters. */
	colon = memchr(reader->bytes + line->start, ':', line->end - line->start);
	if (colon == NULL)
		return;

	/* The name, before its parameters. */
	name_start = line->start;
	name_end = (size_t)(colon - reader->bytes);
	semicolon = memchr(reader->bytes + name_start, ';', name_end - name_start);
	if (semicolon != NULL)
		name_end = (size_t)(semicolon - reader->bytes);
	bmsg_trim(reader->bytes, &name_start, &name_end);

	/* The value, without its spaces at both ends. */
	value_start = (size_t)(colon - reader->bytes) + 1U;
	value_end = line->end;
	bmsg_trim(reader->bytes, &value_start, &value_end);

	/* A vCard's property, a body's, or the message's. */
	if (reader->vcard != BMSG_VCARD_NONE) {
		bmsg_vcard_property(reader, reader->bytes, name_start, name_end, value_start, value_end);
	} else if (reader->in_body) {
		bmsg_body_property(reader, name_start, name_end, value_start, value_end);
	} else if (reader->depth == 0) {
		bmsg_message_property(reader, name_start, name_end, value_start, value_end);
	}
}

/* Keeps the number and the name of a vCard that is kept (TEL, FN, else N; the first of each). */
static void
bmsg_vcard_property(
	struct bmsg_reader *reader,
	const uint8_t *name_bytes,
	size_t name_start,
	size_t name_end,
	size_t value_start,
	size_t value_end)
{
	struct btd_bmsg *message;
	char *number;
	char *name;
	int is_tel;
	int is_fn;
	int is_n;

	/* The fields of the vCard kept. */
	message = reader->message;
	if (reader->vcard == BMSG_VCARD_ORIGINATOR) {
		number = message->originator_number;
		name = message->originator_name;
	} else if (reader->vcard == BMSG_VCARD_RECIPIENT) {
		number = message->recipient_number;
		name = message->recipient_name;
	} else {
		return;
	}

	/* The first number. */
	is_tel = bmsg_same(name_bytes, name_start, name_end, "TEL");
	if (is_tel) {
		if (number[0] == '\0')
			bmsg_copy(number, BTD_MAPXML_TEXT_SIZE, reader->bytes, value_start, value_end);
		return;
	}

	/* The formatted name, before any other. */
	is_fn = bmsg_same(name_bytes, name_start, name_end, "FN");
	if (is_fn) {
		if (value_end > value_start)
			bmsg_copy(name, BTD_MAPXML_TEXT_SIZE, reader->bytes, value_start, value_end);
		return;
	}

	/* The structured name, when no formatted name came first. */
	is_n = bmsg_same(name_bytes, name_start, name_end, "N");
	if (is_n && name[0] == '\0')
		bmsg_name_from_n(name, BTD_MAPXML_TEXT_SIZE, reader->bytes, value_start, value_end);
}

/* Keeps what a body's property says of its text: LENGTH, CHARSET, ENCODING. */
static void
bmsg_body_property(
	struct bmsg_reader *reader,
	size_t name_start,
	size_t name_end,
	size_t value_start,
	size_t value_end)
{
	size_t index;
	size_t number;
	int same;

	/* The text's length: up to six decimal digits, else not counted. */
	same = bmsg_same(reader->bytes, name_start, name_end, "LENGTH");
	if (same) {
		reader->has_length = 0;
		if (value_end == value_start || value_end - value_start > BMSG_LENGTH_DIGITS)
			return;

		/* Each digit. */
		number = 0U;
		for (index = value_start; index < value_end; index++) {
			if (reader->bytes[index] < '0' || reader->bytes[index] > '9')
				return;
			number = number * 10U + (size_t)(reader->bytes[index] - '0');
		}

		/* The length counted. */
		reader->has_length = 1;
		reader->length_value = number;
		return;
	}

	/* The text's character set: UTF-8 is taken. */
	same = bmsg_same(reader->bytes, name_start, name_end, "CHARSET");
	if (same) {
		reader->has_charset = 1;
		reader->charset_utf8 = bmsg_same(reader->bytes, value_start, value_end, "UTF-8");
		return;
	}

	/* An encoding: a text in the network's own form when no character set is said. */
	same = bmsg_same(reader->bytes, name_start, name_end, "ENCODING");
	if (same)
		reader->has_encoding = 1;
}

/* Keeps the message's STATUS, TYPE and FOLDER. */
static void
bmsg_message_property(
	struct bmsg_reader *reader,
	size_t name_start,
	size_t name_end,
	size_t value_start,
	size_t value_end)
{
	struct btd_bmsg *message;
	int same;

	/* Whether it was read. */
	message = reader->message;
	same = bmsg_same(reader->bytes, name_start, name_end, "STATUS");
	if (same) {
		same = bmsg_same(reader->bytes, value_start, value_end, "READ");
		if (same) {
			message->status = BTD_BMSG_STATUS_READ;
			return;
		}

		/* Unread, or not said. */
		same = bmsg_same(reader->bytes, value_start, value_end, "UNREAD");
		if (same)
			message->status = BTD_BMSG_STATUS_UNREAD;
		return;
	}

	/* Its type. */
	same = bmsg_same(reader->bytes, name_start, name_end, "TYPE");
	if (same) {
		message->type = bmsg_word(bmsg_types, reader->bytes, value_start, value_end, BTD_MAP_TYPE_OTHER);
		return;
	}

	/* Its folder. */
	same = bmsg_same(reader->bytes, name_start, name_end, "FOLDER");
	if (same)
		bmsg_copy(message->folder, sizeof(message->folder), reader->bytes, value_start, value_end);
}

/*
 * Takes the text that starts at a BEGIN:MSG line: finds its END:MSG line
 * by LENGTH, else by scanning, keeps the first text of the message, and
 * leaves the reading at the END:MSG line.  Returns 0, or EINVAL for a
 * text not in UTF-8 or without an END:MSG line.
 */
static int
bmsg_text(
	struct bmsg_reader *reader,
	const struct bmsg_line *line)
{
	size_t begin;
	size_t end;
	size_t body_start;
	size_t end_line;
	int form;
	int error;

	/* Only a text in UTF-8: said so, or not said and not in the network's own form. */
	if (reader->message->type != BTD_MAP_TYPE_MMS && reader->has_charset && !reader->charset_utf8)
		return EINVAL;
	if (reader->message->type != BTD_MAP_TYPE_MMS && !reader->has_charset && reader->has_encoding)
		return EINVAL;

	/* Where BEGIN:MSG starts (its B) and where the text after its line starts. */
	begin = line->start;
	end = line->end;
	bmsg_trim(reader->bytes, &begin, &end);
	body_start = line->next;

	/* The END:MSG line by LENGTH, in each way phones count it, else the first one. */
	form = 0;
	if (reader->has_length)
		form = bmsg_text_by_length(reader, begin, body_start, &end_line);

	/* No count fit, or none was given: the first END:MSG line. */
	if (form == 0) {
		error = bmsg_text_by_scan(reader, body_start, &end_line);
		if (error != 0)
			return error;
		form = BTD_BMSG_FORM_SCAN;
	}

	/* The first text of the message is kept; another part is passed over. */
	if (!reader->text_found) {
		reader->message->form = form;
		error = bmsg_take_text(reader, body_start, end_line);
		if (error != 0)
			return error;
		reader->text_found = 1;
	}

	/* Succeeded: the reading goes on at the END:MSG line. */
	reader->at = end_line;
	return 0;
}

/*
 * Finds the END:MSG line by the body's LENGTH counted in each way phones
 * count it: from BEGIN:MSG to the line break after END:MSG (the profile's
 * way), as the text alone, or from BEGIN:MSG to END:MSG without the last
 * line break.  Gives the form that fit (BTD_BMSG_FORM_*) with where the
 * END:MSG line starts, or 0 when none fit.
 */
static int
bmsg_text_by_length(
	struct bmsg_reader *reader,
	size_t begin,
	size_t body_start,
	size_t *end_line)
{
	size_t counted;
	size_t start;
	int found;
	int same;

	/* The profile's way: LENGTH ends just after END:MSG and its line break. */
	if (reader->length_value <= reader->length - begin) {
		counted = begin + reader->length_value;

		/* END:MSG with CRLF before the end. */
		if (counted >= 9U) {
			same = memcmp(reader->bytes + counted - 9U, "END:MSG\r\n", 9U);
			if (same == 0) {
				start = bmsg_line_start_before(reader, counted - 9U);
				found = bmsg_end_line(reader, start, body_start);
				if (found) {
					*end_line = start;
					return BTD_BMSG_FORM_SPEC;
				}
			}
		}

		/* END:MSG with a line feed alone before the end. */
		if (counted >= 8U) {
			same = memcmp(reader->bytes + counted - 8U, "END:MSG\n", 8U);
			if (same == 0) {
				start = bmsg_line_start_before(reader, counted - 8U);
				found = bmsg_end_line(reader, start, body_start);
				if (found) {
					*end_line = start;
					return BTD_BMSG_FORM_SPEC;
				}
			}
		}
	}

	/* The text alone: LENGTH bytes after the BEGIN:MSG line, then a line break or END:MSG. */
	if (reader->length_value <= reader->length - body_start) {
		counted = body_start + reader->length_value;

		/* The text, then CRLF and the END:MSG line. */
		if (reader->length - counted >= 2U &&
		    reader->bytes[counted] == '\r' &&
		    reader->bytes[counted + 1U] == '\n') {
			found = bmsg_end_line(reader, counted + 2U, body_start);
			if (found) {
				*end_line = counted + 2U;
				return BTD_BMSG_FORM_TEXT;
			}
		}

		/* The text, then a line feed and the END:MSG line. */
		if (reader->length - counted >= 1U && reader->bytes[counted] == '\n') {
			found = bmsg_end_line(reader, counted + 1U, body_start);
			if (found) {
				*end_line = counted + 1U;
				return BTD_BMSG_FORM_TEXT;
			}
		}

		/* The text with its last line break counted, then the END:MSG line. */
		found = bmsg_end_line(reader, counted, body_start);
		if (found) {
			*end_line = counted;
			return BTD_BMSG_FORM_TEXT;
		}
	}

	/* The last line break not counted: LENGTH ends just after END:MSG. */
	if (reader->length_value <= reader->length - begin &&
	    reader->length_value >= 7U) {
		counted = begin + reader->length_value;
		same = memcmp(reader->bytes + counted - 7U, "END:MSG", 7U);
		if (same == 0) {
			start = bmsg_line_start_before(reader, counted - 7U);
			found = bmsg_end_line(reader, start, body_start);
			if (found) {
				*end_line = start;
				return BTD_BMSG_FORM_NO_BREAK;
			}
		}
	}

	/* No count fit. */
	return 0;
}

/*
 * Finds the first END:MSG line after the BEGIN:MSG line: one at the
 * start of its line, else one with spaces before it.  Returns 0 with
 * where the line starts, or EINVAL when there is none.
 */
static int
bmsg_text_by_scan(
	struct bmsg_reader *reader,
	size_t body_start,
	size_t *end_line)
{
	const uint8_t *newline;
	size_t start;
	size_t text_start;
	size_t end;
	int pass;
	int same;

	/* First a line that is END:MSG from its first byte, then one with spaces before it. */
	for (pass = 0; pass < 2; pass++) {
		/* Each line after the BEGIN:MSG line. */
		start = body_start;
		while (start < reader->length) {
			/* The line's end. */
			newline = memchr(reader->bytes + start, '\n', reader->length - start);
			end = reader->length;
			if (newline != NULL)
				end = (size_t)(newline - reader->bytes);

			/* The line without its carriage return and spaces, which only the second pass allows before it. */
			if (end > start && reader->bytes[end - 1U] == '\r')
				end--;
			text_start = start;
			bmsg_trim(reader->bytes, &text_start, &end);
			same = bmsg_same(reader->bytes, text_start, end, "END:MSG");
			if (same &&
			    (pass == 1 ||
			     text_start == start)) {
				*end_line = start;
				return 0;
			}

			/* The next line. */
			if (newline == NULL)
				break;
			start = (size_t)(newline - reader->bytes) + 1U;
		}
	}

	/* A text without its end. */
	return EINVAL;
}

/*
 * Tells whether the line at line_start (the start of a line, at or after
 * the BEGIN:MSG line's break) is END:MSG, spaces around it not minded.
 */
static int
bmsg_end_line(
	const struct bmsg_reader *reader,
	size_t line_start,
	size_t body_start)
{
	const uint8_t *newline;
	size_t start;
	size_t end;
	int same;

	/* A line after the BEGIN:MSG line. */
	if (line_start < body_start || line_start > reader->length)
		return 0;

	/* The start of a line. */
	if (line_start != 0U && reader->bytes[line_start - 1U] != '\n')
		return 0;

	/* The line up to its line feed. */
	newline = memchr(reader->bytes + line_start, '\n', reader->length - line_start);
	end = reader->length;
	if (newline != NULL)
		end = (size_t)(newline - reader->bytes);

	/* The line without its spaces and carriage return is END:MSG. */
	if (end > line_start && reader->bytes[end - 1U] == '\r')
		end--;
	start = line_start;
	bmsg_trim(reader->bytes, &start, &end);
	same = bmsg_same(reader->bytes, start, end, "END:MSG");
	if (!same)
		return 0;

	/* Succeeded: an END:MSG line. */
	return 1;
}

/* Gives the start of the line whose text starts at at, stepping back over the spaces before it. */
static size_t
bmsg_line_start_before(
	const struct bmsg_reader *reader,
	size_t at)
{
	/* The spaces before the text. */
	while (at > 0U &&
	       (reader->bytes[at - 1U] == ' ' ||
		reader->bytes[at - 1U] == '\t'))
		at--;

	/* The start of the line, if the spaces began it. */
	return at;
}

/*
 * Keeps the text from body_start to the line break before the END:MSG
 * line at end_line: a slash taken off each line that is slashes and
 * END:MSG, each malformed sequence or NUL made U+FFFD, the whole cut at
 * BTD_BMSG_TEXT_MAX without splitting a character.
 */
static int
bmsg_take_text(
	struct bmsg_reader *reader,
	size_t body_start,
	size_t end_line)
{
	struct btd_bmsg *message;
	size_t body_end;
	size_t at;
	size_t character;
	int line_start;
	int escaped;
	int error;

	/* The text ends before the END:MSG line's break (CRLF or LF); an empty text has none. */
	message = reader->message;
	body_end = body_start;
	if (end_line > body_start) {
		body_end = end_line - 1U;
		if (body_end > body_start && reader->bytes[body_end - 1U] == '\r')
			body_end--;
	}

	/* MMS selects decoded plain text before the SMS text limit can hide a later part. */
	if (message->type == BTD_MAP_TYPE_MMS) {
		error = btd_mms_text(reader->bytes + body_start, body_end - body_start, message->text, sizeof(message->text), &message->text_length, &message->truncated);
		if (error != 0)
			return error;
	} else {
		/* Each character of the text. */
		at = body_start;
		line_start = 1;
		while (at < body_end) {
			/* A line that is slashes and END:MSG loses its first slash. */
			if (line_start && reader->bytes[at] == '/') {
				escaped = bmsg_escaped_line(reader->bytes + body_start, body_end - body_start, at - body_start);
				if (escaped)
					at++;
			}

			/* A character, or U+FFFD for a NUL or a malformed sequence. */
			character = bmsg_utf8_length(reader->bytes + at, body_end - at);
			if (character == 0U || reader->bytes[at] == 0U) {
				error = bmsg_put_text(message, (const uint8_t *)BMSG_REPLACEMENT, BMSG_REPLACEMENT_BYTES);
				character = 1U;
			} else {
				error = bmsg_put_text(message, reader->bytes + at, character);
			}

			/* A text past the room is cut here. */
			if (error != 0) {
				message->truncated = 1;
				break;
			}

			/* The next character, a line feed starting a line. */
			line_start = 0;
			if (reader->bytes[at] == '\n')
				line_start = 1;
			at += character;
		}

		/* The text ends with a NUL. */
		message->text[message->text_length] = '\0';
	}

	/* Succeeded: the selected text body is UTF-8. */
	return 0;
}

/* Adds a character to the message's text.  Returns 0, or ENOBUFS when it would pass the room. */
static int
bmsg_put_text(
	struct btd_bmsg *message,
	const uint8_t *bytes,
	size_t length)
{
	/* A character past the room. */
	if (length > BTD_BMSG_TEXT_MAX - message->text_length)
		return ENOBUFS;

	/* Succeeded: the character added. */
	memcpy(message->text + message->text_length, bytes, length);
	message->text_length += length;
	return 0;
}

/*
 * Gives the bytes of the well-formed UTF-8 character at bytes (1 to 4),
 * or 0 for a malformed one (an overlong form, a surrogate, past U+10FFFF,
 * cut short).
 */
static size_t
bmsg_utf8_length(
	const uint8_t *bytes,
	size_t available)
{
	uint8_t first;
	uint8_t low;
	uint8_t high;
	size_t length;
	size_t index;

	/* One byte. */
	first = bytes[0];
	if (first < 0x80U)
		return 1U;

	/* The length a first byte gives, and the range of its second byte. */
	low = 0x80U;
	high = 0xbfU;
	if (first >= 0xc2U && first <= 0xdfU) {
		length = 2U;
	} else if (first == 0xe0U) {
		length = 3U;
		low = 0xa0U;
	} else if (first >= 0xe1U && first <= 0xecU) {
		length = 3U;
	} else if (first == 0xedU) {
		length = 3U;
		high = 0x9fU;
	} else if (first >= 0xeeU && first <= 0xefU) {
		length = 3U;
	} else if (first == 0xf0U) {
		length = 4U;
		low = 0x90U;
	} else if (first >= 0xf1U && first <= 0xf3U) {
		length = 4U;
	} else if (first == 0xf4U) {
		length = 4U;
		high = 0x8fU;
	} else {
		return 0U;
	}

	/* A character cut short. */
	if (available < length)
		return 0U;

	/* The second byte within its range. */
	if (bytes[1] < low || bytes[1] > high)
		return 0U;

	/* Each later byte a continuation byte. */
	for (index = 2U; index < length; index++) {
		if ((bytes[index] & 0xc0U) != 0x80U)
			return 0U;
	}

	/* Succeeded: a well-formed character. */
	return length;
}

/* Copies the bytes from start to end into a text of size bytes, cut so a character is not split. */
static void
bmsg_copy(
	char *text,
	size_t size,
	const uint8_t *bytes,
	size_t start,
	size_t end)
{
	size_t kept;

	/* As much as fits with its NUL. */
	kept = btd_mapxml_utf8_cut((const char *)bytes + start, end - start, size - 1U);
	memcpy(text, bytes + start, kept);
	text[kept] = '\0';
}

/* Makes a name of a vCard's N (family;given;...): the given name, a space, the family name. */
static void
bmsg_name_from_n(
	char *text,
	size_t size,
	const uint8_t *bytes,
	size_t start,
	size_t end)
{
	const uint8_t *semicolon;
	size_t family_end;
	size_t given_start;
	size_t given_end;
	size_t family_length;
	size_t given_length;
	size_t used;
	size_t kept;

	/* The family name, up to the first ';'. */
	semicolon = memchr(bytes + start, ';', end - start);
	family_end = end;
	given_start = end;
	given_end = end;
	if (semicolon != NULL) {
		family_end = (size_t)(semicolon - bytes);
		given_start = family_end + 1U;

		/* The given name, up to the next ';'. */
		semicolon = memchr(bytes + given_start, ';', end - given_start);
		if (semicolon != NULL)
			given_end = (size_t)(semicolon - bytes);
	}

	/* The given name first. */
	used = 0U;
	given_length = given_end - given_start;
	kept = btd_mapxml_utf8_cut((const char *)bytes + given_start, given_length, size - 1U);
	memcpy(text, bytes + given_start, kept);
	used = kept;

	/* A space between the two names, when both are there and the space fits. */
	family_length = family_end - start;
	if (used > 0U &&
	    family_length > 0U &&
	    used + 1U < size - 1U) {
		text[used] = ' ';
		used++;
	}

	/* The family name after it. */
	kept = btd_mapxml_utf8_cut((const char *)bytes + start, family_length, size - 1U - used);
	memcpy(text + used, bytes + start, kept);
	used += kept;
	text[used] = '\0';
}

/* Adds bytes to the bMessage being built.  Returns 0, or ENOBUFS when size has no room. */
static int
bmsg_put(
	uint8_t *output,
	size_t size,
	size_t *used,
	const void *data,
	size_t length)
{
	/* No room. */
	if (length > size - *used)
		return ENOBUFS;

	/* Succeeded: added. */
	memcpy(output + *used, data, length);
	*used += length;
	return 0;
}

/* Adds a string, without its NUL, to the bMessage being built.  Returns 0, or ENOBUFS. */
static int
bmsg_put_string(
	uint8_t *output,
	size_t size,
	size_t *used,
	const char *text)
{
	size_t length;
	int error;

	/* The string's bytes. */
	length = strlen(text);
	error = bmsg_put(output, size, used, text, length);
	if (error != 0)
		return error;

	/* Succeeded: added. */
	return 0;
}

/*
 * Adds the text of a bMessage being built: each line break made CRLF (a
 * lone CR or LF too) and each line that is slashes and END:MSG given one
 * more slash.  Returns 0, or ENOBUFS.
 */
static int
bmsg_put_body(
	uint8_t *output,
	size_t size,
	size_t *used,
	const uint8_t *text,
	size_t length)
{
	size_t at;
	int line_start;
	int escaped;
	int error;

	/* Each byte of the text. */
	at = 0U;
	line_start = 1;
	while (at < length) {
		/* A line that is slashes and END:MSG gets one more slash. */
		if (line_start) {
			escaped = bmsg_escaped_line(text, length, at);
			if (escaped) {
				error = bmsg_put(output, size, used, "/", 1U);
				if (error != 0)
					return error;
			}
		}

		/* A line break as CRLF: CRLF, a lone CR, a lone LF. */
		line_start = 0;
		if (text[at] == '\r' || text[at] == '\n') {
			error = bmsg_put(output, size, used, "\r\n", 2U);
			if (error != 0)
				return error;

			/* The LF of a CRLF goes with its CR. */
			if (text[at] == '\r' &&
			    at + 1U < length &&
			    text[at + 1U] == '\n')
				at++;
			at++;
			line_start = 1;
			continue;
		}

		/* Another byte as it is. */
		error = bmsg_put(output, size, used, text + at, 1U);
		if (error != 0)
			return error;
		at++;
	}

	/* Succeeded: the text added. */
	return 0;
}

/* Gives the bytes bmsg_put_body adds for a text: its line breaks as CRLF and its escaping slashes. */
static size_t
bmsg_body_bytes(
	const uint8_t *text,
	size_t length)
{
	size_t at;
	size_t count;
	int line_start;
	int escaped;

	/* Each byte of the text, as bmsg_put_body adds it. */
	at = 0U;
	count = 0U;
	line_start = 1;
	while (at < length) {
		/* An escaping slash. */
		if (line_start) {
			escaped = bmsg_escaped_line(text, length, at);
			if (escaped)
				count++;
		}

		/* A line break, two bytes. */
		line_start = 0;
		if (text[at] == '\r' || text[at] == '\n') {
			count += 2U;
			if (text[at] == '\r' &&
			    at + 1U < length &&
			    text[at + 1U] == '\n')
				at++;
			at++;
			line_start = 1;
			continue;
		}

		/* Another byte. */
		count++;
		at++;
	}

	/* The bytes the text takes. */
	return count;
}

/*
 * Tells whether the line that starts at at in text is any number of
 * slashes then END:MSG at its start (the line a text escapes, MAP section
 * 3.1.3).  A line that is END:MSG with nothing after it on the line, or
 * with more, counts the same: only its start is looked at.
 */
static int
bmsg_escaped_line(
	const uint8_t *text,
	size_t length,
	size_t at)
{
	size_t slashes;
	int same;

	/* The slashes at the start. */
	slashes = 0U;
	while (at + slashes < length && text[at + slashes] == '/')
		slashes++;

	/* END:MSG after them. */
	if (length - at - slashes < 7U)
		return 0;
	same = memcmp(text + at + slashes, "END:MSG", 7U);
	if (same != 0)
		return 0;

	/* Succeeded: a line to escape (with no slash), or to unescape (with some) as the caller reads it. */
	return 1;
}
