/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * data: URLs (the Fetch Standard's data: URL processor): the MIME type
 * before the comma (parsed and serialized as the MIME Sniffing Standard
 * does), and the body after it, percent-decoded and, with ";base64", the
 * Infra Standard's forgiving-base64 decoded.
 */

#include "net/net.h"

#include <errno.h>
#include <string.h>

/* The MIME type of a data: URL whose own does not parse. */
#define DATA_DEFAULT_MIME "text/plain;charset=US-ASCII"

/* The whitespace data_trim takes away: ASCII whitespace, or HTTP whitespace. */
#define DATA_ASCII_SPACE 0
#define DATA_HTTP_SPACE 1

/*
 * The MIME type being parsed: its text, where the parser stands, and the
 * serialization being written (parameters is where its parameters start).
 */
struct data_mime_parser {
	const char *text;
	size_t end;
	size_t position;
	struct wb_buffer *out;
	size_t parameters;
};

static int data_is_space(int c, int kind);
static void data_trim(const char *text, size_t *start, size_t *end, int kind);
static int data_all(const char *text, size_t length, int (*test)(int));
static int data_is_token(int c);
static int data_is_quoted_token(int c);
static int data_append_lower(struct wb_buffer *out, const char *text, size_t length);
static int data_find_base64(const char *input, size_t start, size_t end, size_t *cut);
static int data_base64(const unsigned char *text, size_t length, struct wb_buffer *out);
static int data_mime(const char *text, size_t length, struct wb_buffer *out);
static int data_parameter(struct data_mime_parser *parser, struct wb_buffer *value);
static int data_add_parameter(struct data_mime_parser *parser, size_t name_start, size_t name_end, const struct wb_buffer *value);
static int data_quoted(const char *text, size_t end, size_t *position, struct wb_buffer *value);
static int data_has_parameter(const struct wb_buffer *serialized, size_t start, const char *name, size_t length);

/*
 * Processes a data URL into its MIME type and body.
 * Returns EINVAL for invalid URL or body syntax.
 */
int
net_data_parse(
	const struct net_url *url,
	struct net_data *data)
{
	struct wb_buffer serialized;
	struct wb_buffer decoded;
	struct wb_buffer mime;
	const char *input;
	size_t length;
	size_t comma;
	size_t start;
	size_t end;
	size_t cut;
	int base64;
	int differs;
	int error;

	/* Prepares every output and temporary owner before fallible decoding. */
	wb_buffer_init(&data->mime);
	wb_buffer_init(&data->body);
	wb_buffer_init(&serialized);
	wb_buffer_init(&decoded);
	wb_buffer_init(&mime);
	error = net_url_serialize(url, 1, &serialized);
	if (error != 0)
		goto release_buffers;

	/* Requires the serialized data scheme before interpreting its opaque path. */
	input = wb_buffer_string(&serialized);
	differs = strncmp(input, "data:", 5);
	if (differs != 0) {
		error = EINVAL;
		goto release_buffers;
	}

	/* Separates the MIME declaration from the body at the first comma. */
	input += 5;
	length = serialized.length - 5U;
	comma = 0;
	while (comma < length && input[comma] != ',')
		comma++;
	if (comma == length) {
		error = EINVAL;
		goto release_buffers;
	}

	/* Removes surrounding ASCII whitespace from the declaration. */
	start = 0;
	end = comma;
	data_trim(input, &start, &end, DATA_ASCII_SPACE);

	/* Percent-decodes the complete body before any MIME fallback can apply. */
	error = net_percent_decode(input + comma + 1U, length - comma - 1U, &data->body);
	if (error != 0)
		goto release_buffers;

	/* A trailing base64 marker selects forgiving decoding of those actual bytes. */
	base64 = data_find_base64(input, start, end, &cut);
	if (base64) {
		error = data_base64(data->body.data, data->body.length, &decoded);
		if (error != 0)
			goto release_buffers;

		/* Replaces percent-decoded input only after base64 decoding succeeds. */
		wb_buffer_clear(&data->body);
		error = wb_buffer_append(&data->body, decoded.data, decoded.length);
		if (error != 0)
			goto release_buffers;

		/* Excludes the marker from the MIME declaration. */
		end = cut;
	}

	/* A declaration starting with parameters uses the text/plain essence. */
	if (start < end && input[start] == ';') {
		error = wb_buffer_append_string(&mime, "text/plain");
		if (error != 0)
			goto release_buffers;
	}

	/* Copies the remaining declaration independently of the decoded body. */
	error = wb_buffer_append(&mime, input + start, end - start);
	if (error != 0)
		goto release_buffers;

	/* Only malformed MIME syntax uses the default, never earlier body/allocation errors. */
	error = data_mime(wb_buffer_string(&mime), mime.length, &data->mime);
	if (error != 0) {
		if (error != EINVAL)
			goto release_buffers;

		/* Replaces partial MIME serialization with the specified default. */
		wb_buffer_clear(&data->mime);
		error = wb_buffer_append_string(&data->mime, DATA_DEFAULT_MIME);
		if (error != 0)
			goto release_buffers;
	}

release_buffers:
	/* Releases temporaries and refuses partial outputs on either failure path. */
	wb_buffer_release(&mime);
	wb_buffer_release(&decoded);
	wb_buffer_release(&serialized);
	if (error != 0) {
		net_data_release(data);
		return error;
	}

	/* Succeeded: the MIME type and the body are there. */
	return 0;
}

/*
 * Frees a data: URL's MIME type and body.
 */
void
net_data_release(
	struct net_data *data)
{
	/* The two buffers. */
	wb_buffer_release(&data->mime);
	wb_buffer_release(&data->body);

	/* Succeeded: no decoded MIME or body allocation remains owned. */
	return;
}

/* Tells whether a byte is ASCII whitespace (with form feed) or HTTP whitespace (without). */
static int
data_is_space(
	int c,
	int kind)
{
	/* Tab, line feed, carriage return and space are both. */
	if (c == 0x09 ||
	    c == 0x0a ||
	    c == 0x0d ||
	    c == 0x20)
		return 1;

	/* Form feed is ASCII whitespace only. */
	if (c == 0x0c && kind == DATA_ASCII_SPACE)
		return 1;
	return 0;
}

/* Moves the ends of a range of text in past whitespace of a kind. */
static void
data_trim(
	const char *text,
	size_t *start,
	size_t *end,
	int kind)
{
	int space;

	/* From the start. */
	while (*start < *end) {
		space = data_is_space((unsigned char)text[*start], kind);
		if (!space)
			break;
		(*start)++;
	}

	/* From the end. */
	while (*end > *start) {
		space = data_is_space((unsigned char)text[*end - 1U], kind);
		if (!space)
			break;
		(*end)--;
	}

	/* Succeeded: both range boundaries exclude the selected whitespace. */
	return;
}

/* Tells whether every byte of a text passes a test. */
static int
data_all(
	const char *text,
	size_t length,
	int (*test)(int))
{
	size_t index;
	int passes;

	/* Each byte. */
	for (index = 0; index < length; index++) {
		passes = test((unsigned char)text[index]);
		if (!passes)
			return 0;
	}

	/* All of them pass. */
	return 1;
}

/* Tells whether a byte is an HTTP token code point. */
static int
data_is_token(
	int c)
{
	const char *found;

	/* Letters and digits. */
	if ((c >= 'a' && c <= 'z') ||
	    (c >= 'A' && c <= 'Z') ||
	    (c >= '0' && c <= '9'))
		return 1;

	/* The punctuation tokens allow. */
	if (c == 0)
		return 0;
	found = strchr("!#$%&'*+-.^_`|~", c);
	if (found != NULL)
		return 1;
	return 0;
}

/* Tells whether a byte is an HTTP quoted-string token code point. */
static int
data_is_quoted_token(
	int c)
{
	/* Tab, the printable ASCII range, and the Latin-1 range. */
	if (c == 0x09 ||
	    (c >= 0x20 && c <= 0x7e) ||
	    (c >= 0x80 && c <= 0xff))
		return 1;
	return 0;
}

/* Appends a text with its ASCII letters in lower case. */
static int
data_append_lower(
	struct wb_buffer *out,
	const char *text,
	size_t length)
{
	size_t index;
	int c;
	int error;

	/* Each byte, folded. */
	for (index = 0; index < length; index++) {
		c = (unsigned char)text[index];
		if (c >= 'A' && c <= 'Z')
			c += 0x20;
		error = wb_buffer_append_byte(out, (unsigned char)c);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text is appended. */
	return 0;
}

/* Finds ";base64" (spaces allowed before base64, any case) at the end of a type; *cut is where the type then ends. */
static int
data_find_base64(
	const char *input,
	size_t start,
	size_t end,
	size_t *cut)
{
	size_t at;
	int differs;

	/* The six letters at the end. */
	*cut = end;
	if (end - start < 6U)
		return 0;
	at = end - 6U;
	differs = strncmp(input + at + 4U, "64", 2);
	if (differs != 0)
		return 0;
	if ((input[at] | 0x20) != 'b' || (input[at + 1U] | 0x20) != 'a' || (input[at + 2U] | 0x20) != 's' ||
	    (input[at + 3U] | 0x20) != 'e')
		return 0;

	/* Spaces, then a semicolon, before them. */
	while (at > start && input[at - 1U] == ' ')
		at--;
	if (at == start || input[at - 1U] != ';')
		return 0;

	/* Succeeded: the type ends before the semicolon. */
	*cut = at - 1U;
	return 1;
}

/* Decodes forgiving base64 (whitespace skipped, one or two = at the end allowed); EINVAL when it is not base64. */
static int
data_base64(
	const unsigned char *text,
	size_t length,
	struct wb_buffer *out)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	struct wb_buffer clean;
	const char *found;
	uint32_t bits;
	size_t index;
	int space;
	int count;
	int error;

	/* The characters without whitespace. */
	wb_buffer_init(&clean);
	error = 0;
	for (index = 0; index < length; index++) {
		space = data_is_space(text[index], DATA_ASCII_SPACE);
		if (!space) {
			error = wb_buffer_append_byte(&clean, text[index]);
			if (error != 0)
				goto release_clean;
		}
	}

	/* One or two = at the end of a whole number of quartets go. */
	if (clean.length % 4U == 0U &&
	    clean.length > 0 &&
	    clean.data[clean.length - 1U] == '=') {
		clean.length--;
		if (clean.length > 0 && clean.data[clean.length - 1U] == '=')
			clean.length--;
	}

	/* A lone character past the quartets is not base64. */
	if (clean.length % 4U == 1U) {
		error = EINVAL;
		goto release_clean;
	}

	/* Six bits a character, a byte out for each eight. */
	bits = 0;
	count = 0;
	for (index = 0; index < clean.length; index++) {
		found = NULL;
		if (clean.data[index] != 0)
			found = strchr(alphabet, clean.data[index]);
		if (found == NULL) {
			error = EINVAL;
			goto release_clean;
		}

		/* The six bits join the ones waiting; a whole byte goes out. */
		bits = (bits << 6) | (uint32_t)(found - alphabet);
		count += 6;
		if (count >= 8) {
			count -= 8;
			error = wb_buffer_append_byte(out, (unsigned char)(bits >> count));
			if (error != 0)
				goto release_clean;
		}
	}

release_clean:
	/* The cleaned copy goes. */
	wb_buffer_release(&clean);
	if (error != 0)
		return error;

	/* Succeeded: the bytes are decoded. */
	return 0;
}

/* Parses a MIME type and writes its serialization; EINVAL when it is not a MIME type. */
static int
data_mime(
	const char *text,
	size_t length,
	struct wb_buffer *out)
{
	struct data_mime_parser parser;
	struct wb_buffer value;
	size_t start;
	size_t end;
	size_t type_end;
	size_t subtype;
	size_t subtype_end;
	int space;
	int tokens;
	int error;

	/* Leading and trailing HTTP whitespace goes. */
	start = 0;
	end = length;
	data_trim(text, &start, &end, DATA_HTTP_SPACE);

	/* The type: tokens up to the slash. */
	subtype = start;
	while (subtype < end && text[subtype] != '/')
		subtype++;
	if (subtype == start || subtype >= end)
		return EINVAL;
	tokens = data_all(text + start, subtype - start, data_is_token);
	if (!tokens)
		return EINVAL;

	/* The subtype: tokens up to the first semicolon, without trailing whitespace. */
	type_end = subtype;
	subtype++;
	parser.position = subtype;
	while (parser.position < end && text[parser.position] != ';')
		parser.position++;
	subtype_end = parser.position;

	/* Only trailing whitespace may be removed from the subtype. */
	while (subtype_end > subtype) {
		space = data_is_space((unsigned char)text[subtype_end - 1U], DATA_HTTP_SPACE);
		if (!space)
			break;
		subtype_end--;
	}

	/* Requires a nonempty token subtype after trailing whitespace removal. */
	if (subtype_end == subtype)
		return EINVAL;
	tokens = data_all(text + subtype, subtype_end - subtype, data_is_token);
	if (!tokens)
		return EINVAL;

	/* The type and subtype in lower case. */
	error = data_append_lower(out, text + start, type_end - start);
	if (error != 0)
		return error;

	/* Separates the serialized type from the validated subtype. */
	error = wb_buffer_append_byte(out, '/');
	if (error != 0)
		return error;

	/* Serializes the validated subtype in lowercase. */
	error = data_append_lower(out, text + subtype, subtype_end - subtype);
	if (error != 0)
		return error;

	/* Each parameter after a semicolon. */
	parser.text = text;
	parser.end = end;
	parser.out = out;
	parser.parameters = out->length;
	wb_buffer_init(&value);
	while (parser.position < end) {
		error = data_parameter(&parser, &value);
		if (error != 0) {
			wb_buffer_release(&value);
			return error;
		}
	}

	/* Releases the reusable parameter buffer after the complete declaration. */
	wb_buffer_release(&value);

	/* Succeeded: the MIME type is serialized. */
	return 0;
}

/* Parses one parameter from its semicolon (a name, =, and a value or a quoted string) and adds it when it is valid. */
static int
data_parameter(
	struct data_mime_parser *parser,
	struct wb_buffer *value)
{
	const char *text;
	size_t name_start;
	size_t name_end;
	size_t value_start;
	size_t value_end;
	int space;
	int error;

	/* Past the semicolon and whitespace, the name up to ; or =. */
	text = parser->text;
	parser->position++;
	while (parser->position < parser->end) {
		space = data_is_space((unsigned char)text[parser->position], DATA_HTTP_SPACE);
		if (!space)
			break;
		parser->position++;
	}

	/* The name. */
	name_start = parser->position;
	while (parser->position < parser->end &&
	       text[parser->position] != ';' &&
	       text[parser->position] != '=')
		parser->position++;
	name_end = parser->position;

	/* A name without = is no parameter; nothing after = ends the parameters. */
	if (parser->position < parser->end && text[parser->position] == ';')
		return 0;
	if (parser->position < parser->end)
		parser->position++;
	if (parser->position >= parser->end)
		return 0;

	/* The value: a quoted string (anything after it up to ; is dropped), or up to ; without trailing whitespace. */
	wb_buffer_clear(value);
	if (text[parser->position] == '"') {
		error = data_quoted(text, parser->end, &parser->position, value);
		if (error != 0)
			return error;

		/* Ignores trailing material after the quoted value. */
		while (parser->position < parser->end && text[parser->position] != ';')
			parser->position++;
	} else {
		value_start = parser->position;
		while (parser->position < parser->end && text[parser->position] != ';')
			parser->position++;
		value_end = parser->position;
		while (value_end > value_start) {
			space = data_is_space((unsigned char)text[value_end - 1U], DATA_HTTP_SPACE);
			if (!space)
				break;
			value_end--;
		}

		/* The value without its trailing whitespace (its leading whitespace stays). */
		error = wb_buffer_append(value, text + value_start, value_end - value_start);
		if (error != 0)
			return error;

		/* Empty unquoted values contribute no parameter. */
		if (value->length == 0)
			return 0;
	}

	/* The parameter, when it is valid. */
	error = data_add_parameter(parser, name_start, name_end, value);
	if (error != 0)
		return error;

	/* Succeeded: the parameter is read. */
	return 0;
}

/* Adds a parameter to the serialization when its name is tokens, its value quoted-string tokens, and the name is new. */
static int
data_add_parameter(
	struct data_mime_parser *parser,
	size_t name_start,
	size_t name_end,
	const struct wb_buffer *value)
{
	const char *name;
	size_t length;
	size_t index;
	int valid;
	int present;
	int quotes;
	int error;

	/* The name must be tokens, and not a name already there. */
	name = parser->text + name_start;
	length = name_end - name_start;
	if (length == 0)
		return 0;
	valid = data_all(name, length, data_is_token);
	if (!valid)
		return 0;
	present = data_has_parameter(parser->out, parser->parameters, name, length);
	if (present)
		return 0;

	/* The value must be quoted-string tokens. */
	valid = data_all((const char *)value->data, value->length, data_is_quoted_token);
	if (!valid)
		return 0;

	/* A value that is empty or not all tokens is quoted. */
	quotes = 0;
	valid = data_all((const char *)value->data, value->length, data_is_token);
	if (value->length == 0 || !valid)
		quotes = 1;

	/* Separates this parameter from the preceding serialized field. */
	error = wb_buffer_append_byte(parser->out, ';');
	if (error != 0)
		return error;

	/* Canonicalizes the validated parameter name. */
	error = data_append_lower(parser->out, name, length);
	if (error != 0)
		return error;

	/* Separates the canonical name from its value. */
	error = wb_buffer_append_byte(parser->out, '=');
	if (error != 0)
		return error;

	/* Quotes values that contain non-token characters or are empty. */
	if (quotes) {
		error = wb_buffer_append_byte(parser->out, '"');
		if (error != 0)
			return error;
	}

	/* Preserves each value byte, escaping quote/backslash only inside quotes. */
	for (index = 0; index < value->length; index++) {
		if (quotes &&
		    (value->data[index] == '"' ||
		     value->data[index] == '\\')) {
			error = wb_buffer_append_byte(parser->out, '\\');
			if (error != 0)
				return error;
		}

		/* Writes the actual byte after any escape prefix succeeds. */
		error = wb_buffer_append_byte(parser->out, value->data[index]);
		if (error != 0)
			return error;
	}

	/* Closes only a value whose opening quote was written. */
	if (quotes) {
		error = wb_buffer_append_byte(parser->out, '"');
		if (error != 0)
			return error;
	}

	/* Succeeded: the parameter is added. */
	return 0;
}

/* Collects an HTTP quoted string's value from its opening quote, a backslash escaping the next character. */
static int
data_quoted(
	const char *text,
	size_t end,
	size_t *position,
	struct wb_buffer *value)
{
	int error;

	/* Past the opening quote, up to the closing one. */
	(*position)++;
	while (*position < end) {
		/* The closing quote ends it. */
		if (text[*position] == '"') {
			(*position)++;
			break;
		}

		/* A backslash takes the next character as it is (at the end, it is itself). */
		if (text[*position] == '\\') {
			(*position)++;
			if (*position >= end) {
				error = wb_buffer_append_byte(value, '\\');
				if (error != 0)
					return error;

				/* The final unpaired backslash is part of the collected value. */
				break;
			}
		}

		/* The character. */
		error = wb_buffer_append_byte(value, (unsigned char)text[*position]);
		if (error != 0)
			return error;

		/* Advances only after this value byte is copied. */
		(*position)++;
	}

	/* Succeeded: the value is collected. */
	return 0;
}

/* Tells whether the serialized parameters (from start) already have a name (compared in lower case). */
static int
data_has_parameter(
	const struct wb_buffer *serialized,
	size_t start,
	const char *name,
	size_t length)
{
	const char *text;
	size_t index;
	size_t at;
	int c;
	int same;
	int quoted;

	/* Each ";name=" in the serialization. */
	text = (const char *)serialized->data;
	quoted = 0;
	for (at = start; at < serialized->length; at++) {
		/* Ignores delimiters within values and consumes escaped quoted bytes together. */
		if (quoted) {
			if (text[at] == '\\' && at + 1U < serialized->length) {
				at++;
				continue;
			}

			/* An unescaped quote ends the serialized value. */
			if (text[at] == '"')
				quoted = 0;
			continue;
		}

		/* A serialized quoted value begins only outside another quoted value. */
		if (text[at] == '"') {
			quoted = 1;
			continue;
		}

		/* Only a real field delimiter can begin an existing parameter name. */
		if (text[at] != ';')
			continue;
		if (length >= serialized->length - at - 1U)
			continue;
		if (text[at + 1U + length] != '=')
			continue;

		/* The name, compared folded. */
		same = 1;
		for (index = 0; same && index < length; index++) {
			c = (unsigned char)name[index];
			if (c >= 'A' && c <= 'Z')
				c += 0x20;
			if (text[at + 1U + index] != c)
				same = 0;
		}

		/* A match. */
		if (same)
			return 1;
	}

	/* Not there. */
	return 0;
}
