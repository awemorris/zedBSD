/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads MMS text/plain parts from MAP's MIME body. RFC 2045 supplies the
 * transfer encodings; RFC 2046 supplies multipart delimiters. This original
 * reader bounds headers, nesting, parts and output. It retains image/video leaves and ignores SMIL, HTML and text attachments.
 * It never falls back to displaying MIME source.
 */

#include "mms.h"

#include <stdio.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Limits on unfolded headers and multipart traversal within one bMessage. */
#define MMS_HEADER_MAX 1024U
#define MMS_DEPTH_MAX 4U
#define MMS_PARTS_MAX 64U

/* MIME properties needed to select and decode one part. */
struct mms_headers {
	char type[MMS_HEADER_MAX];
	char encoding[80];
	char disposition[MMS_HEADER_MAX];
	size_t body;
};

/* One extraction accumulates UTF-8 text and counts all visited MIME parts. */
struct mms_reader {
	char *output;
	size_t size;
	size_t used;
	unsigned parts;
	int found;
	int truncated;
	struct mms_document *document;
};

static int mms_utf8_ok(const uint8_t *input, size_t length);
static int mms_media(struct mms_reader *reader, const uint8_t *input, size_t length, const struct mms_headers *headers);
static int mms_write_base64(FILE *output, const uint8_t *input, size_t length);
static int mms_line(const uint8_t *input, size_t length, size_t *at, size_t *start, size_t *end);
static int mms_headers_read(const uint8_t *input, size_t length, struct mms_headers *headers);
static int mms_header_keep(struct mms_headers *headers, const char *line);
static int mms_parameter(const char *type, const char *name, char *value, size_t size);
static int mms_token(const char *value, const char *token);
static int mms_part(struct mms_reader *reader, const uint8_t *input, size_t length, unsigned depth);
static int mms_multipart(struct mms_reader *reader, const uint8_t *input, size_t length, const char *boundary, unsigned depth);
static int mms_decode(const uint8_t *input, size_t length, const char *encoding, uint8_t *output, size_t *used);
static int mms_base64(unsigned byte);
static int mms_hex(unsigned byte);
static int mms_append(struct mms_reader *reader, const uint8_t *input, size_t length, const char *charset);

/*
 * Extracts text into a NUL-terminated UTF-8 buffer. Reports ENODATA for media
 * without plain text, ENOTSUP for an unsupported encoding, EINVAL for broken
 * MIME, or an allocation error. A successful long text sets truncated.
 */
int
mms_extract_text(
	const uint8_t *input,
	size_t length,
	char *output,
	size_t size,
	size_t *used,
	int *truncated)
{
	struct mms_reader reader;
	int error;

	/* Requires a complete bounded body and space for its terminator. */
	if (input == NULL || output == NULL || used == NULL || truncated == NULL ||
	    size == 0U || length > MMS_INPUT_MAX)
		return EINVAL;

	/* Initializes the extraction without exposing partial results on failure. */
	memset(&reader, 0, sizeof(reader));
	reader.output = output;
	reader.size = size;
	output[0] = '\0';
	*used = 0U;
	*truncated = 0;
	error = mms_part(&reader, input, length, 0U);
	if (error != 0) {
		output[0] = '\0';
		return error;
	}

	/* A multimedia-only message has no text to import. */
	if (!reader.found)
		return ENODATA;

	/* MIME text commonly ends in a transport line break, which is not a glyph. */
	while (reader.used > 0U && reader.output[reader.used - 1U] == '\n')
		reader.used--;

	/* Succeeded: exposes only the selected decoded parts. */
	output[reader.used] = '\0';
	*used = reader.used;
	*truncated = reader.truncated;
	return 0;
}

/*
 * Reads plain text and independently owned image/video parts from one MIME body.
 */
int
mms_parse(
	const uint8_t *input,
	size_t length,
	struct mms_document *document)
{
	struct mms_reader reader;
	int error;

	/* Requires one complete bounded body and a fresh output document. */
	if (input == NULL || document == NULL || length > MMS_INPUT_MAX)
		return EINVAL;

	/* Initializes ownership before any leaf allocates its decoded bytes. */
	memset(document, 0, sizeof(*document));
	memset(&reader, 0, sizeof(reader));
	reader.output = document->text;
	reader.size = sizeof(document->text);
	reader.document = document;
	error = mms_part(&reader, input, length, 0U);
	if (error != 0) {
		mms_release(document);
		return error;
	}

	/* Refuses a message with no usable text or media. */
	if (!reader.found && document->count == 0U)
		return ENODATA;

	/* Removes transport-only trailing newlines from the caption. */
	while (reader.used > 0U && document->text[reader.used - 1U] == '\n')
		reader.used--;

	/* Succeeded: the caller owns every decoded media part. */
	document->text[reader.used] = '\0';
	document->text_length = reader.used;
	document->truncated = reader.truncated;
	return 0;
}

/*
 * Releases the independent byte buffers of a parsed document.
 */
void
mms_release(
	struct mms_document *document)
{
	size_t index;

	/* Frees only the published prefix of allocated leaves. */
	for (index = 0U; index < document->count; index++)
		free(document->media[index].data);
	memset(document, 0, sizeof(*document));
}

/*
 * Writes one original image/video and an optional UTF-8 caption as MIME.
 */
int
mms_write(
	FILE *output,
	const char *type,
	const char *name,
	const uint8_t *data,
	size_t length,
	const char *caption)
{
	size_t span;
	size_t caption_length;
	int selected;
	int written;
	int error;
	int valid;

	/* Requires one nonempty bounded media payload. */
	if (output == NULL || type == NULL || name == NULL || data == NULL || length == 0U || length > MMS_MEDIA_MAX)
		return EINVAL;

	/* Accepts media tokens without allowing folded or injected headers. */
	selected = strncmp(type, "image/", 6U);
	if (selected != 0)
		selected = strncmp(type, "video/", 6U);
	span = strspn(type, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/.-+");
	if (selected != 0 || type[span] != '\0' || span >= MMS_TYPE_MAX)
		return EINVAL;

	/* Keeps a quoted filename on one line without escaped delimiters. */
	span = strcspn(name, "\r\n\"\\");
	if (name[span] != '\0' || span == 0U || span >= MMS_NAME_MAX)
		return EINVAL;

	/* Validates the optional caption before producing any bytes. */
	if (caption == NULL)
		caption = "";
	caption_length = strlen(caption);
	valid = mms_utf8_ok((const uint8_t *)caption, caption_length);
	if (!valid || caption_length > MMS_TEXT_MAX)
		return EINVAL;

	/* Uses a delimiter with punctuation absent from base64 leaf bodies. */
	written = fprintf(output, "MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"=_Keiland_MMS_1\"\r\n\r\n");
	if (written < 0)
		return EIO;

	/* Writes the text part only when the message has a caption. */
	if (caption_length != 0U) {
		written = fprintf(output, "--=_Keiland_MMS_1\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n");
		if (written < 0)
			return EIO;
		error = mms_write_base64(output, (const uint8_t *)caption, caption_length);
		if (error != 0)
			return error;
	}

	/* Writes the original binary file rather than transcoding it. */
	written = fprintf(output, "--=_Keiland_MMS_1\r\nContent-Type: %s; name=\"%s\"\r\nContent-Disposition: inline; filename=\"%s\"\r\nContent-Transfer-Encoding: base64\r\n\r\n", type, name, name);
	if (written < 0)
		return EIO;
	error = mms_write_base64(output, data, length);
	if (error != 0)
		return error;

	/* Terminates the multipart and exposes write failures. */
	written = fprintf(output, "--=_Keiland_MMS_1--\r\n");
	if (written < 0)
		return EIO;

	/* Succeeded: the stream contains one complete MIME message. */
	return 0;
}

/* Validates UTF-8 scalar values and excludes embedded NUL in captions. */
static int
mms_utf8_ok(
	const uint8_t *input,
	size_t length)
{
	size_t at;
	size_t count;
	size_t index;
	unsigned first;
	unsigned low;
	unsigned high;

	/* Walks complete scalar values without accepting overlong encodings. */
	at = 0U;
	while (at < length) {
		first = input[at];
		count = 1U;
		low = 0x80U;
		high = 0xbfU;
		if (first == 0U)
			return 0;
		if (first >= 0xc2U && first <= 0xdfU) {
			count = 2U;
		} else if (first >= 0xe0U && first <= 0xefU) {
			count = 3U;
			if (first == 0xe0U)
				low = 0xa0U;
			if (first == 0xedU)
				high = 0x9fU;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			count = 4U;
			if (first == 0xf0U)
				low = 0x90U;
			if (first == 0xf4U)
				high = 0x8fU;
		} else if (first >= 0x80U) {
			return 0;
		}

		/* Requires all continuation bytes, excluding surrogates and values above Unicode. */
		if (count > length - at)
			return 0;
		if (count > 1U && (input[at + 1U] < low || input[at + 1U] > high))
			return 0;
		for (index = 2U; index < count; index++) {
			if ((input[at + index] & 0xc0U) != 0x80U)
				return 0;
		}

		/* Advances only after the entire scalar has been accepted. */
		at += count;
	}

	/* Succeeded: every caption character is representable. */
	return 1;
}

/* Decodes and publishes one independently owned image or video leaf. */
static int
mms_media(
	struct mms_reader *reader,
	const uint8_t *input,
	size_t length,
	const struct mms_headers *headers)
{
	struct mms_document *document;
	struct mms_media *media;
	uint8_t *data;
	size_t used;
	size_t span;
	size_t index;
	int error;
	int written;

	/* A text-only caller deliberately does not allocate media. */
	document = reader->document;
	if (document == NULL)
		return 0;

	/* Bounds the independently owned leaves before allocation. */
	if (document->count >= MMS_MEDIA_COUNT)
		return E2BIG;
	data = malloc(length + 1U);
	if (data == NULL)
		return ENOMEM;

	/* Decodes binary bytes without applying caption or NUL rules. */
	error = mms_decode(input, length, headers->encoding, data, &used);
	if (error != 0) {
		free(data);
		return error;
	}

	/* Bounds total decoded bytes and refuses empty media. */
	if (used == 0U || used > MMS_MEDIA_MAX - document->bytes) {
		free(data);
		return E2BIG;
	}

	/* Retains the media token separately from its MIME parameters. */
	media = &document->media[document->count];
	span = strcspn(headers->type, "; \t");
	if (span == 0U || span >= sizeof(media->type)) {
		free(data);
		return EOVERFLOW;
	}

	/* Preserves a display name, never using it as a filesystem path. */
	memcpy(media->type, headers->type, span);
	media->type[span] = '\0';

	/* MIME type tokens are case-insensitive; consumers use a canonical lowercase token. */
	for (index = 0U; index < span; index++) {
		if (media->type[index] >= 'A' && media->type[index] <= 'Z')
			media->type[index] += 'a' - 'A';
	}

	/* Takes a source filename from standard MIME parameters when available. */
	error = mms_parameter(headers->disposition, "filename", media->name, sizeof(media->name));
	if (error == ENOENT)
		error = mms_parameter(headers->type, "name", media->name, sizeof(media->name));
	if (error == ENOENT || (error == 0 && media->name[0] == '\0')) {
		written = snprintf(media->name, sizeof(media->name), "attachment-%zu", document->count + 1U);
		if (written < 0)
			error = EIO;
		else
			error = 0;
	}

	/* Refuses malformed filename parameters before publishing ownership. */
	if (error != 0) {
		free(data);
		return error;
	}

	/* Keeps control characters out of the displayed filename. */
	for (index = 0U; media->name[index] != '\0'; index++) {
		if ((unsigned char)media->name[index] < 0x20U || media->name[index] == 0x7f)
			media->name[index] = '_';
	}

	/* The published prefix owns data until the caller releases the document. */
	media->data = data;
	media->length = used;
	document->bytes += used;
	document->count++;

	/* Succeeded: this leaf is ready for persistence or display. */
	return 0;
}

/* Writes canonical base64 lines of at most 76 characters. */
static int
mms_write_base64(
	FILE *output,
	const uint8_t *input,
	size_t length)
{
	const char *alphabet;
	char line[78];
	size_t at;
	size_t used;
	size_t remaining;
	size_t written;
	unsigned bits;

	/* Uses the RFC 2045 alphabet and wraps only between complete quartets. */
	alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	used = 0U;
	for (at = 0U; at < length; at += remaining) {
		remaining = length - at;
		if (remaining > 3U)
			remaining = 3U;
		bits = (unsigned)input[at] << 16;
		if (remaining > 1U)
			bits |= (unsigned)input[at + 1U] << 8;
		if (remaining > 2U)
			bits |= input[at + 2U];
		line[used++] = alphabet[(bits >> 18) & 63U];
		line[used++] = alphabet[(bits >> 12) & 63U];
		line[used++] = '=';
		if (remaining > 1U)
			line[used - 1U] = alphabet[(bits >> 6) & 63U];
		line[used++] = '=';
		if (remaining > 2U)
			line[used - 1U] = alphabet[bits & 63U];

		/* Flushes each full line and the final partial line. */
		if (used == 76U || at + remaining == length) {
			line[used++] = '\r';
			line[used++] = '\n';
			written = fwrite(line, 1U, used, output);
			if (written != used)
				return EIO;
			used = 0U;
		}
	}

	/* Succeeded: the encoded leaf ends at a MIME line boundary. */
	return 0;
}

/* Reads one MIME line, excluding CRLF, advancing even an unterminated last line. */
static int
mms_line(
	const uint8_t *input,
	size_t length,
	size_t *at,
	size_t *start,
	size_t *end)
{
	/* End of this bounded entity. */
	if (*at >= length)
		return 0;

	/* Finds the line's next separator within the body span. */
	*start = *at;
	while (*at < length && input[*at] != '\n')
		(*at)++;
	*end = *at;

	/* Excludes the carriage return of CRLF. */
	if (*end > *start && input[*end - 1U] == '\r')
		(*end)--;

	/* Consumes the line feed, if this is not the last unterminated line. */
	if (*at < length)
		(*at)++;

	/* Succeeded: one line is available. */
	return 1;
}

/* Reads and unfolds MIME headers before the required empty separator line. */
static int
mms_headers_read(
	const uint8_t *input,
	size_t length,
	struct mms_headers *headers)
{
	char line[MMS_HEADER_MAX];
	size_t at;
	size_t start;
	size_t end;
	size_t used;
	size_t skip;
	unsigned count;
	int found;
	int error;

	/* Defaults a missing Content-Type to plain text, without guessing media. */
	memset(headers, 0, sizeof(*headers));
	memcpy(headers->type, "text/plain", 11U);
	at = 0U;
	used = 0U;
	count = 0U;

	/* Unfolds each field into a bounded temporary line. */
	for (;;) {
		found = mms_line(input, length, &at, &start, &end);
		if (!found)
			return EINVAL;

		/* A blank line terminates headers and fixes the body span. */
		if (start == end) {
			line[used] = '\0';
			error = mms_header_keep(headers, line);
			if (error != 0)
				return error;
			headers->body = at;
			break;
		}

		/* Processes the preceding field before starting the next field. */
		skip = start;
		if (input[start] != ' ' && input[start] != '\t') {
			line[used] = '\0';
			error = mms_header_keep(headers, line);
			if (error != 0)
				return error;
			used = 0U;
			count++;
			if (count > 128U)
				return EOVERFLOW;
		} else {
			/* A continuation needs a preceding header to continue. */
			if (used == 0U)
				return EINVAL;

			/* Replaces folding whitespace with one separating space. */
			while (skip < end && (input[skip] == ' ' || input[skip] == '\t'))
				skip++;
			if (used + 1U >= sizeof(line))
				return EOVERFLOW;
			line[used++] = ' ';
		}

		/* Refuses overlong headers rather than interpreting a shortened value. */
		if (end - skip >= sizeof(line) - used)
			return EOVERFLOW;
		memcpy(line + used, input + skip, end - skip);
		used += end - skip;
	}

	/* Succeeded: the empty separator leaves the complete body span. */
	return 0;
}

/* Retains only the MIME headers used by the plain-text reader. */
static int
mms_header_keep(
	struct mms_headers *headers,
	const char *line)
{
	const char *colon;
	const char *value;
	char *destination;
	size_t size;
	size_t name_length;
	size_t length;
	int same;

	/* No preceding field at the beginning of the header section. */
	if (line[0] == '\0')
		return 0;

	/* Separates a complete field name from its value. */
	colon = strchr(line, ':');
	if (colon == NULL)
		return EINVAL;
	name_length = (size_t)(colon - line);
	value = colon + 1U;

	/* Removes whitespace surrounding the field value. */
	while (*value == ' ' || *value == '\t')
		value++;
	length = strlen(value);
	while (length > 0U && (value[length - 1U] == ' ' || value[length - 1U] == '\t'))
		length--;

	/* Chooses a known header, preserving exact values for parameter parsing. */
	destination = NULL;
	size = 0U;
	same = strncasecmp(line, "Content-Type", name_length);
	if (name_length == 12U && same == 0) {
		destination = headers->type;
		size = sizeof(headers->type);
	} else {
		/* Transfer encoding identifies how this leaf body must be decoded. */
		same = strncasecmp(line, "Content-Transfer-Encoding", name_length);
		if (name_length == 25U && same == 0) {
			destination = headers->encoding;
			size = sizeof(headers->encoding);
		} else {
			/* Explicit attachments must not be rendered as conversation text. */
			same = strncasecmp(line, "Content-Disposition", name_length);
			if (name_length == 19U && same == 0) {
				destination = headers->disposition;
				size = sizeof(headers->disposition);
			}
		}
	}

	/* Unneeded headers remain outside the text output. */
	if (destination == NULL)
		return 0;

	/* Rejects a truncated property before copying it. */
	if (length >= size)
		return EOVERFLOW;

	/* Succeeded: the selected property is available to the part reader. */
	memcpy(destination, value, length);
	destination[length] = '\0';
	return 0;
}

/* Compares a MIME token case-insensitively, permitting its parameter suffix. */
static int
mms_token(
	const char *value,
	const char *token)
{
	size_t length;
	int same;

	/* Compares the whole token before accepting a parameter delimiter. */
	length = strlen(token);
	same = strncasecmp(value, token, length);
	if (same != 0)
		return 0;
	if (value[length] != '\0' && value[length] != ';' && value[length] != ' ' && value[length] != '\t')
		return 0;

	/* Succeeded: this is the requested MIME token. */
	return 1;
}

/* Extracts a quoted or unquoted parameter without treating embedded semicolons as delimiters. */
static int
mms_parameter(
	const char *type,
	const char *name,
	char *value,
	size_t size)
{
	const char *at;
	const char *start;
	size_t name_length;
	size_t wanted_length;
	size_t used;
	int quoted;
	int selected;
	int same;

	/* Walks parameters following the media token. */
	value[0] = '\0';
	wanted_length = strlen(name);
	selected = 0;
	at = strchr(type, ';');
	while (at != NULL && *at != '\0') {
		/* Reads the parameter name without surrounding whitespace. */
		at++;
		while (*at == ' ' || *at == '\t')
			at++;
		start = at;
		while (*at != '\0' && *at != '=' && *at != ';' && *at != ' ' && *at != '\t')
			at++;
		name_length = (size_t)(at - start);
		while (*at == ' ' || *at == '\t')
			at++;
		if (*at != '=')
			return EINVAL;
		at++;
		while (*at == ' ' || *at == '\t')
			at++;
		same = strncasecmp(start, name, name_length);
		selected = 0;
		if (same == 0 && name_length == wanted_length)
			selected = 1;

		/* Reads a complete value, unescaping quoted pairs. */
		quoted = 0;
		if (*at == '"') {
			quoted = 1;
			at++;
		}

		/* Copies only the requested value, with its terminator reserved. */
		used = 0U;
		while (*at != '\0') {
			if (quoted && *at == '"')
				break;
			if (!quoted && (*at == ';' || *at == ' ' || *at == '\t'))
				break;
			if (quoted && *at == '\\') {
				at++;
				if (*at == '\0')
					return EINVAL;
			}

			/* Refuses shortening the requested parameter. */
			if (selected) {
				if (used + 1U >= size)
					return EOVERFLOW;
				value[used++] = *at;
			}

			/* Advances within this parameter's complete value. */
			at++;
		}

		/* A quoted value must close before another parameter. */
		if (quoted) {
			if (*at != '"')
				return EINVAL;
			at++;
		}

		/* Separates the next parameter from trailing whitespace. */
		while (*at == ' ' || *at == '\t')
			at++;
		if (*at != '\0' && *at != ';')
			return EINVAL;
		if (selected) {
			value[used] = '\0';
			break;
		}
	}

	/* The requested property was not supplied. */
	if (!selected)
		return ENOENT;

	/* Succeeded: the complete requested parameter is available. */
	return 0;
}

/* Selects one MIME entity, recursively inspecting bounded multipart bodies. */
static int
mms_part(
	struct mms_reader *reader,
	const uint8_t *input,
	size_t length,
	unsigned depth)
{
	struct mms_headers headers;
	char boundary[72];
	char charset[80];
	uint8_t *decoded;
	size_t used;
	int selected;
	int error;

	/* Stops pathological nesting and excessive part counts within the bounded message. */
	reader->parts++;
	if (depth > MMS_DEPTH_MAX || reader->parts > MMS_PARTS_MAX)
		return EOVERFLOW;

	/* Reads the entity's MIME properties before selecting its payload. */
	error = mms_headers_read(input, length, &headers);
	if (error != 0)
		return error;

	/*
	 * MAP phones also wrap leaf and WAP content types in boundary-delimited
	 * MIME. Honor the explicit boundary before classifying a leaf, otherwise
	 * text exposes part headers and images retain encoded MIME instead of pixels.
	 */
	error = mms_parameter(headers.type, "boundary", boundary, sizeof(boundary));
	if (error == 0) {
		/* An empty delimiter cannot identify complete parts. */
		if (boundary[0] == '\0')
			return EINVAL;

		/* Traverses the framed leaves independently of the outer media type. */
		error = mms_multipart(reader, input + headers.body, length - headers.body, boundary, depth);
		if (error != 0)
			return error;

		/* Succeeded: only the selected decoded leaves have been retained. */
		return 0;
	}

	/* Malformed boundary parameters cannot be treated as displayable text. */
	if (error != ENOENT)
		return error;

	/* Standard multipart types require an explicit delimiter. */
	selected = strncasecmp(headers.type, "multipart/", 10U);
	if (selected == 0)
		return EINVAL;

	/* Retains image and video parts independently of their attachment disposition. */
	selected = strncasecmp(headers.type, "image/", 6U);
	if (selected != 0)
		selected = strncasecmp(headers.type, "video/", 6U);
	if (selected == 0) {
		error = mms_media(reader, input + headers.body, length - headers.body, &headers);
		if (error != 0)
			return error;
		return 0;
	}

	/* Skips explicit attachments, including text attachments. */
	selected = mms_token(headers.disposition, "attachment");
	if (selected)
		return 0;

	/* Never exposes HTML, SMIL or multimedia as raw text. */
	selected = mms_token(headers.type, "text/plain");
	if (!selected)
		return 0;

	/* Accepts UTF-8 by default, and explicit ASCII or Latin-1. */
	error = mms_parameter(headers.type, "charset", charset, sizeof(charset));
	if (error == ENOENT) {
		memcpy(charset, "utf-8", 6U);
	} else if (error != 0) {
		return error;
	}

	/* Decodes into a bounded owned workspace before validating its text. */
	decoded = malloc(length + 1U);
	if (decoded == NULL)
		return ENOMEM;
	error = mms_decode(input + headers.body, length - headers.body, headers.encoding, decoded, &used);
	if (error == 0)
		error = mms_append(reader, decoded, used, charset);
	free(decoded);

	/* Reports an undecodable text part without exposing its MIME source. */
	if (error != 0)
		return error;

	/* Succeeded: this plain-text part is accounted for. */
	return 0;
}

/* Traverses whole-line multipart delimiters and requires a closing delimiter. */
static int
mms_multipart(
	struct mms_reader *reader,
	const uint8_t *input,
	size_t length,
	const char *boundary,
	unsigned depth)
{
	size_t at;
	size_t start;
	size_t end;
	size_t part;
	size_t part_end;
	size_t boundary_length;
	size_t tail;
	int active;
	int closed;
	int line;
	int same;
	int error;

	/* Finds delimiters without interpreting preamble or epilogue as text. */
	at = 0U;
	part = 0U;
	active = 0;
	closed = 0;
	boundary_length = strlen(boundary);
	for (;;) {
		line = mms_line(input, length, &at, &start, &end);
		if (!line)
			break;
		if (end - start < boundary_length + 2U || input[start] != '-' || input[start + 1U] != '-')
			continue;
		same = memcmp(input + start + 2U, boundary, boundary_length);
		if (same != 0)
			continue;

		/* A longer prefix is body text rather than a delimiter line. */
		tail = start + boundary_length + 2U;
		closed = 0;
		if (end - tail >= 2U && input[tail] == '-' && input[tail + 1U] == '-') {
			closed = 1;
			tail += 2U;
		}

		/* Allows only transport padding after the complete delimiter. */
		while (tail < end && (input[tail] == ' ' || input[tail] == '\t'))
			tail++;
		if (tail != end)
			continue;

		/* The delimiter's preceding CRLF belongs to it, not the leaf body. */
		if (active) {
			part_end = start;
			if (part_end > part && input[part_end - 1U] == '\n')
				part_end--;
			if (part_end > part && input[part_end - 1U] == '\r')
				part_end--;
			error = mms_part(reader, input + part, part_end - part, depth + 1U);
			if (error != 0)
				return error;
		}

		/* A complete closing delimiter terminates this multipart entity. */
		if (closed)
			break;
		part = at;
		active = 1;
	}

	/* An incomplete multipart must not produce a successful text import. */
	if (!closed)
		return EINVAL;

	/* Succeeded: a complete closing delimiter ended this entity. */
	return 0;
}

/* Converts one Base64 alphabet byte to its six-bit value, or -1. */
static int
mms_base64(
	unsigned byte)
{
	/* The three contiguous alphabet ranges. */
	if (byte >= 'A' && byte <= 'Z')
		return (int)(byte - 'A');
	if (byte >= 'a' && byte <= 'z')
		return (int)(byte - 'a' + 26U);
	if (byte >= '0' && byte <= '9')
		return (int)(byte - '0' + 52U);
	if (byte == '+')
		return 62;
	if (byte == '/')
		return 63;

	/* This is not a Base64 alphabet byte. */
	return -1;
}

/* Converts one hexadecimal digit used by quoted-printable to its value, or -1. */
static int
mms_hex(
	unsigned byte)
{
	/* The decimal and letter digit ranges. */
	if (byte >= '0' && byte <= '9')
		return (int)(byte - '0');
	if (byte >= 'A' && byte <= 'F')
		return (int)(byte - 'A' + 10U);
	if (byte >= 'a' && byte <= 'f')
		return (int)(byte - 'a' + 10U);

	/* This byte cannot form a quoted-printable escape. */
	return -1;
}

/* Decodes supported leaf transfer encodings, writing no more bytes than the input length. */
static int
mms_decode(
	const uint8_t *input,
	size_t length,
	const char *encoding,
	uint8_t *output,
	size_t *used)
{
	int group[4];
	unsigned count;
	unsigned byte;
	size_t at;
	int base64;
	int qp;
	int identity;
	int low;
	int high;
	int padded;

	/* Classifies the encoding before copying any bytes. */
	base64 = mms_token(encoding, "base64");
	qp = mms_token(encoding, "quoted-printable");
	identity = 0;
	if (encoding[0] == '\0')
		identity = 1;
	if (!identity)
		identity = mms_token(encoding, "7bit");
	if (!identity)
		identity = mms_token(encoding, "8bit");
	if (!identity)
		identity = mms_token(encoding, "binary");
	if (!base64 && !qp && !identity)
		return ENOTSUP;

	/* Converts each byte, validating complete escape or Base64 groups. */
	*used = 0U;
	count = 0U;
	padded = 0;
	for (at = 0U; at < length; at++) {
		byte = input[at];
		if (base64) {
			/* Whitespace may separate Base64 groups and their padding. */
			if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n')
				continue;
			if (padded)
				return EINVAL;
			low = -2;
			if (byte != '=')
				low = mms_base64(byte);
			if (low == -1)
				return EINVAL;
			group[count++] = low;
			if (count < 4U)
				continue;

			/* Padding is permitted only in the last two positions. */
			if (group[0] < 0 || group[1] < 0 ||
			    (group[2] == -2 && group[3] != -2))
				return EINVAL;
			output[(*used)++] = (uint8_t)((group[0] << 2) | (group[1] >> 4));
			if (group[2] >= 0) {
				output[(*used)++] = (uint8_t)((group[1] << 4) | (group[2] >> 2));
				if (group[3] >= 0)
					output[(*used)++] = (uint8_t)((group[2] << 6) | group[3]);
			}

			/* The final padded quartet forbids subsequent non-whitespace bytes. */
			if (group[3] == -2)
				padded = 1;
			count = 0U;
		} else if (qp && byte == '=') {
			/* A soft line break continues the encoded text. */
			if (at + 1U < length && input[at + 1U] == '\n') {
				at++;
				continue;
			}

			/* CRLF is the canonical soft line break. */
			if (at + 2U < length && input[at + 1U] == '\r' && input[at + 2U] == '\n') {
				at += 2U;
				continue;
			}

			/* An escaped byte requires two hexadecimal digits. */
			if (at + 2U >= length)
				return EINVAL;
			high = mms_hex(input[at + 1U]);
			low = mms_hex(input[at + 2U]);
			if (high < 0 || low < 0)
				return EINVAL;
			output[(*used)++] = (uint8_t)((high << 4) | low);
			at += 2U;
		} else {
			/* Identity and literal quoted-printable bytes remain unchanged. */
			output[(*used)++] = (uint8_t)byte;
		}
	}

	/* A partial Base64 quartet is not a complete transfer representation. */
	if (count != 0U)
		return EINVAL;

	/* Succeeded: only decoded payload bytes have been produced. */
	return 0;
}

/* Appends one validated text part, converting Latin-1 and preserving complete UTF-8 characters. */
static int
mms_append(
	struct mms_reader *reader,
	const uint8_t *input,
	size_t length,
	const char *charset)
{
	uint8_t character[2];
	size_t at;
	size_t count;
	unsigned byte;
	int latin;
	int utf8;
	int ascii;
	int valid;

	/* Recognizes only character sets whose bytes can be converted completely. */
	latin = mms_token(charset, "iso-8859-1");
	utf8 = mms_token(charset, "utf-8");
	ascii = mms_token(charset, "us-ascii");
	if (!latin && !utf8 && !ascii)
		return ENOTSUP;

	/* Validates the complete text before exposing even its first bytes. */
	valid = mms_utf8_ok(input, length);
	if (!latin && !valid)
		return EINVAL;

	/* Refuses embedded NUL and non-ASCII bytes in explicit ASCII text. */
	for (at = 0U; at < length; at++) {
		if (input[at] == 0U || (ascii && input[at] >= 0x80U))
			return EINVAL;
	}

	/* Separates distinct text parts without inserting an initial newline. */
	if (reader->found && reader->used > 0U && !reader->truncated) {
		if (reader->used + 1U >= reader->size) {
			reader->truncated = 1;
		} else {
			reader->output[reader->used++] = '\n';
		}
	}

	/* Copies each complete character until the output limit is reached. */
	reader->found = 1;
	for (at = 0U; at < length; at += count) {
		byte = input[at];
		count = 1U;

		/* Normalizes CRLF and bare CR before they reach a text renderer. */
		if (byte == '\r') {
			if (at + 1U < length && input[at + 1U] == '\n')
				count = 2U;
			if (reader->truncated || reader->used + 1U >= reader->size) {
				reader->truncated = 1;
				break;
			}

			/* One logical newline replaces either wire representation. */
			reader->output[reader->used++] = '\n';
			continue;
		}

		/* Determines the complete output character's byte count. */
		if (latin && byte >= 0x80U) {
			character[0] = (uint8_t)(0xc0U | (byte >> 6));
			character[1] = (uint8_t)(0x80U | (byte & 0x3fU));
			count = 2U;
		} else if (utf8 && byte >= 0x80U) {
			/* Complete UTF-8 was already validated above. */
			count = 2U;
			if (byte >= 0xe0U)
				count = 3U;
			if (byte >= 0xf0U)
				count = 4U;
		}

		/* Long bodies stop at character boundaries and still report a valid text. */
		if (reader->truncated || count >= reader->size - reader->used) {
			reader->truncated = 1;
			break;
		}

		/* Latin-1 expands one source byte; UTF-8 retains its original byte count. */
		if (latin && byte >= 0x80U) {
			memcpy(reader->output + reader->used, character, 2U);
			reader->used += 2U;
			count = 1U;
		} else {
			memcpy(reader->output + reader->used, input + at, count);
			reader->used += count;
		}
	}

	/* Succeeded: this text part has been selected without its MIME headers. */
	return 0;
}
