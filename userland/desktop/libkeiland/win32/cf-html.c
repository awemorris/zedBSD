/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HTML Format clipboard format of Windows.
 *
 * The encoder writes the data the header file describes.  The decoder reads
 * the header lines of data it cannot trust, keeps every index below the
 * length it was given, and accepts a pair of offsets only when the pair
 * lies inside the data.
 */

#include "userland/desktop/libkeiland/win32/cf-html.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The largest total length that ten decimal digits can express. */
#define CF_HTML_MAX_TOTAL		9999999999ULL

/* The line that opens the data. */
#define CF_HTML_VERSION_LINE		"Version:0.9\r\n"

/* The names of the header lines, each with its colon. */
#define CF_HTML_VERSION_NAME		"Version:"
#define CF_HTML_START_HTML_NAME		"StartHTML:"
#define CF_HTML_END_HTML_NAME		"EndHTML:"
#define CF_HTML_START_FRAGMENT_NAME	"StartFragment:"
#define CF_HTML_END_FRAGMENT_NAME	"EndFragment:"
#define CF_HTML_SOURCE_URL_NAME		"SourceURL:"

/* The end of a header line the encoder writes. */
#define CF_HTML_EOL			"\r\n"

/* The HTML that comes before the fragment, ending with the start comment. */
#define CF_HTML_PREFIX			"<html><body>\r\n<!--StartFragment-->"

/* The comment that marks where the fragment ends. */
#define CF_HTML_END_COMMENT		"<!--EndFragment-->"

/* The HTML that comes after the end comment. */
#define CF_HTML_SUFFIX			"\r\n</body>\r\n</html>"

/*
 * The header lines the decoder tells apart.
 */
enum cf_html_field {
	CF_HTML_FIELD_OTHER,
	CF_HTML_FIELD_VERSION,
	CF_HTML_FIELD_START_HTML,
	CF_HTML_FIELD_END_HTML,
	CF_HTML_FIELD_START_FRAGMENT,
	CF_HTML_FIELD_END_FRAGMENT,
	CF_HTML_FIELD_SOURCE_URL
};

/*
 * One offset line of a header that was read.
 *
 * An offset that was not seen leaves present at 0.  A seen offset is either
 * the text "-1", which sets none, or a number in value that is already
 * known not to pass the length of the data.
 */
struct cf_html_offset {
	int present;
	int none;
	size_t value;
};

/*
 * What a header said, as far as the decoder needs it.
 *
 * The smallest offset is the smallest number any of the four offset lines
 * gave (the length of the data when none did): the header cannot reach past
 * the place where the HTML starts.  The URL is a range of the data;
 * url_present tells an empty URL from no URL.
 */
struct cf_html_header {
	int version_present;
	struct cf_html_offset start_html;
	struct cf_html_offset end_html;
	struct cf_html_offset start_fragment;
	struct cf_html_offset end_fragment;
	int url_present;
	size_t url_start;
	size_t url_length;
	size_t smallest_offset;
};

static char *put_text(char *cursor, const char *text, size_t count);
static char *put_offset(char *cursor, const char *name, size_t value);
static void clear_encoder_output(char *buffer, size_t size, size_t *out_length);
static int has_line_break(const char *text);
static int parse_offset(const char *data, size_t start, size_t count, size_t length, struct cf_html_offset *offset);
static int name_is_valid(const char *data, size_t start, size_t count);
static int name_equals(const char *data, size_t start, size_t count, const char *name);
static enum cf_html_field field_of(const char *data, size_t start, size_t count);
static size_t line_end_of(const char *data, size_t start, size_t length);
static size_t next_line_of(const char *data, size_t line_end, size_t length);
static int read_offset_line(const char *data, size_t start, size_t count, size_t length, struct cf_html_header *header, struct cf_html_offset *offset);
static int read_line(const char *data, size_t length, size_t line_start, size_t colon, size_t line_end, struct cf_html_header *header);
static int read_header(const char *data, size_t length, size_t limit, struct cf_html_header *header);
static int pair_is_valid(const struct cf_html_offset *start, const struct cf_html_offset *end, size_t length);
static int select_range(const struct cf_html_header *header, size_t length, size_t *start, size_t *end);

/*
 * Wraps a fragment of HTML in the CF_HTML data.
 *
 * Every position is worked out first, in the order the parts are written,
 * so the buffer is touched only once the whole data is known to fit.
 */
int
keiui_win32_cf_html_encode(
	const char *fragment,
	size_t length,
	const char *source_url,
	char *buffer,
	size_t size,
	size_t *out_length)
{
	size_t url_length;
	size_t url_line_length;
	size_t header_length;
	size_t fixed_length;
	size_t start_html;
	size_t start_fragment;
	size_t end_fragment;
	size_t end_html;
	int broken;
	char *cursor;

	/* Refuses arguments the data cannot be delivered through. */
	if (buffer == NULL || out_length == NULL)
		return EINVAL;

	/* Leaves a clean result behind for every refusal that follows. */
	clear_encoder_output(buffer, size, out_length);

	/* Refuses a fragment that has a length but no bytes. */
	if (fragment == NULL && length > 0U)
		return EINVAL;

	/* Measures the URL, refusing one that would end its header line early or start another line. */
	url_length = 0U;
	url_line_length = 0U;
	if (source_url != NULL) {
		broken = has_line_break(source_url);
		if (broken)
			return EINVAL;

		url_length = strlen(source_url);
		url_line_length = (sizeof(CF_HTML_SOURCE_URL_NAME) - 1U) + url_length + (sizeof(CF_HTML_EOL) - 1U);
	}

	/* Adds up the header: the version line, the four offset lines, and the URL line when there is one. */
	header_length = (sizeof(CF_HTML_VERSION_LINE) - 1U) +
	    (sizeof(CF_HTML_START_HTML_NAME) - 1U) + KEIUI_WIN32_CF_HTML_DIGITS + (sizeof(CF_HTML_EOL) - 1U) +
	    (sizeof(CF_HTML_END_HTML_NAME) - 1U) + KEIUI_WIN32_CF_HTML_DIGITS + (sizeof(CF_HTML_EOL) - 1U) +
	    (sizeof(CF_HTML_START_FRAGMENT_NAME) - 1U) + KEIUI_WIN32_CF_HTML_DIGITS + (sizeof(CF_HTML_EOL) - 1U) +
	    (sizeof(CF_HTML_END_FRAGMENT_NAME) - 1U) + KEIUI_WIN32_CF_HTML_DIGITS + (sizeof(CF_HTML_EOL) - 1U);

	/* Adds up what is written whatever the fragment is, to check the total against the limit before it is made. */
	fixed_length = (sizeof(CF_HTML_PREFIX) - 1U) +
	    (sizeof(CF_HTML_END_COMMENT) - 1U) +
	    (sizeof(CF_HTML_SUFFIX) - 1U);

	/* Refuses a total that does not fit in a size or in a ten digit offset. */
	if (url_line_length > SIZE_MAX - header_length - fixed_length)
		return EINVAL;
	if (length > SIZE_MAX - header_length - fixed_length - url_line_length)
		return EINVAL;
	if ((unsigned long long)header_length + fixed_length + url_line_length + length > CF_HTML_MAX_TOTAL)
		return EINVAL;

	/* Places each part: the HTML follows the header, and the fragment follows the start comment. */
	start_html = header_length + url_line_length;
	start_fragment = start_html + (sizeof(CF_HTML_PREFIX) - 1U);
	end_fragment = start_fragment + length;
	end_html = end_fragment + (sizeof(CF_HTML_END_COMMENT) - 1U) + (sizeof(CF_HTML_SUFFIX) - 1U);

	/* Tells the caller how long the data is, and refuses a buffer without room for it and its NUL. */
	*out_length = end_html;
	if (size <= end_html)
		return ERANGE;

	/* Writes the header: the version, the four offsets, and the URL line. */
	cursor = buffer;
	cursor = put_text(cursor, CF_HTML_VERSION_LINE, sizeof(CF_HTML_VERSION_LINE) - 1U);
	cursor = put_offset(cursor, CF_HTML_START_HTML_NAME, start_html);
	cursor = put_offset(cursor, CF_HTML_END_HTML_NAME, end_html);
	cursor = put_offset(cursor, CF_HTML_START_FRAGMENT_NAME, start_fragment);
	cursor = put_offset(cursor, CF_HTML_END_FRAGMENT_NAME, end_fragment);
	if (source_url != NULL) {
		cursor = put_text(cursor, CF_HTML_SOURCE_URL_NAME, sizeof(CF_HTML_SOURCE_URL_NAME) - 1U);
		cursor = put_text(cursor, source_url, url_length);
		cursor = put_text(cursor, CF_HTML_EOL, sizeof(CF_HTML_EOL) - 1U);
	}

	/* Writes the HTML: the opening, the fragment, and the closing. */
	cursor = put_text(cursor, CF_HTML_PREFIX, sizeof(CF_HTML_PREFIX) - 1U);
	cursor = put_text(cursor, fragment, length);
	cursor = put_text(cursor, CF_HTML_END_COMMENT, sizeof(CF_HTML_END_COMMENT) - 1U);
	cursor = put_text(cursor, CF_HTML_SUFFIX, sizeof(CF_HTML_SUFFIX) - 1U);

	/* Ends the data with the NUL the clipboard's block holds. */
	*cursor = '\0';

	/* Succeeded: the buffer holds the data and its NUL. */
	return 0;
}

/*
 * Finds the fragment and the source URL in CF_HTML data.
 *
 * The header is read first, line by line, then the offsets it gave are
 * checked as pairs and the range of the fragment is chosen.  Nothing is
 * handed back unless every step succeeded.
 */
int
keiui_win32_cf_html_decode(
	const char *data,
	size_t length,
	const char **fragment,
	size_t *fragment_length,
	const char **source_url,
	size_t *source_url_length)
{
	struct cf_html_header header;
	struct cf_html_header whole;
	size_t start;
	size_t end;
	int error;

	/* Refuses to decode when there is nowhere to put the answer. */
	if (fragment == NULL || fragment_length == NULL)
		return EINVAL;

	/* Leaves the outputs empty, as a failure promises. */
	*fragment = NULL;
	*fragment_length = 0U;
	if (source_url != NULL)
		*source_url = NULL;
	if (source_url_length != NULL)
		*source_url_length = 0U;

	/* Refuses bytes that are promised but not there. */
	if (data == NULL)
		return EINVAL;

	/*
	 * Reads every header line once to find the smallest offset, which is
	 * where the header has to end, then reads again only the lines that
	 * end before it.
	 */
	error = read_header(data, length, length, &whole);
	if (error != 0)
		return error;
	error = read_header(data, length, whole.smallest_offset, &header);
	if (error != 0)
		return error;

	/* Refuses data that does not say what its version is. */
	if (!header.version_present)
		return EINVAL;

	/* Chooses the fragment's range, or the HTML's, out of the offsets. */
	error = select_range(&header, length, &start, &end);
	if (error != 0)
		return error;

	/* Hands back the range of the fragment. */
	*fragment = data + start;
	*fragment_length = end - start;

	/* Hands back the URL when the header had one and the caller wants it. */
	if (header.url_present) {
		if (source_url != NULL)
			*source_url = data + header.url_start;
		if (source_url_length != NULL)
			*source_url_length = header.url_length;
	}

	/* Succeeded: the outputs point into the data. */
	return 0;
}

/*
 * Copies bytes to the cursor and returns the position after them.
 */
static char *
put_text(
	char *cursor,
	const char *text,
	size_t count)
{
	/* Copies nothing for an empty fragment, which may have no bytes to copy from. */
	if (count > 0U)
		memcpy(cursor, text, count);

	/* Reports where the next part goes. */
	return cursor + count;
}

/*
 * Writes a header line, a name, then an offset as ten digits with leading
 * zeros, then a line end, and returns the position after it.
 */
static char *
put_offset(
	char *cursor,
	const char *name,
	size_t value)
{
	unsigned digit_index;
	size_t rest;

	/* Writes the name of the line, which ends with its colon. */
	cursor = put_text(cursor, name, strlen(name));

	/* Writes the digits from the right, so the number is padded with zeros on the left. */
	rest = value;
	for (digit_index = KEIUI_WIN32_CF_HTML_DIGITS;
	     digit_index > 0U;
	     digit_index--) {
		cursor[digit_index - 1U] = (char)('0' + (int)(rest % 10U));
		rest /= 10U;
	}
	cursor += KEIUI_WIN32_CF_HTML_DIGITS;

	/* Ends the line. */
	cursor = put_text(cursor, CF_HTML_EOL, sizeof(CF_HTML_EOL) - 1U);

	/* Reports where the next line goes. */
	return cursor;
}

/*
 * Makes the encoder's outputs describe no data, for a call that fails.
 */
static void
clear_encoder_output(
	char *buffer,
	size_t size,
	size_t *out_length)
{
	/* Leaves an empty string where the caller looks for the data. */
	if (size > 0U)
		buffer[0] = '\0';

	/* Reports that no data was made. */
	*out_length = 0U;
}

/*
 * Tells whether a text holds a CR or an LF.
 */
static int
has_line_break(
	const char *text)
{
	size_t index;

	/* Looks at each byte up to the end of the text. */
	for (index = 0U;
	     text[index] != '\0';
	     index++) {
		if (text[index] == '\r')
			return 1;
		if (text[index] == '\n')
			return 1;
	}

	/* Found no line break. */
	return 0;
}

/*
 * Reads an offset value out of count bytes of the data starting at start.
 *
 * The value is the text "-1" or decimal digits, at least one, and nothing
 * else.  A number that passes the length of the data is refused as soon as
 * it does, so a long run of digits cannot overflow.
 */
static int
parse_offset(
	const char *data,
	size_t start,
	size_t count,
	size_t length,
	struct cf_html_offset *offset)
{
	size_t index;
	size_t value;
	unsigned digit;

	/* Starts from an offset that has been seen and has no value yet. */
	offset->present = 1;
	offset->none = 0;
	offset->value = 0U;

	/* Refuses an empty value. */
	if (count == 0U)
		return EINVAL;

	/* Accepts the text "-1", which says there is no such position. */
	if (count == 2U && data[start] == '-' && data[start + 1U] == '1') {
		offset->none = 1;

		/* Succeeded: the offset is "-1". */
		return 0;
	}

	/* Reads the digits, refusing anything else and any number above the length. */
	value = 0U;
	for (index = 0U;
	     index < count;
	     index++) {
		if (data[start + index] < '0' || data[start + index] > '9')
			return EINVAL;

		digit = (unsigned)(data[start + index] - '0');
		if (value > (SIZE_MAX - digit) / 10U)
			return EINVAL;

		value = value * 10U + digit;
		if (value > length)
			return EINVAL;
	}
	offset->value = value;

	/* Succeeded: the offset is a number inside the data. */
	return 0;
}

/*
 * Tells whether count bytes of the data starting at start can be the name
 * of a header line: ASCII letters, digits, hyphens and underscores, at
 * least one.
 */
static int
name_is_valid(
	const char *data,
	size_t start,
	size_t count)
{
	size_t index;
	char letter;

	/* Refuses an empty name. */
	if (count == 0U)
		return 0;

	/* Refuses any other byte, so HTML with a colon in it is not taken for a header line. */
	for (index = 0U;
	     index < count;
	     index++) {
		letter = data[start + index];
		if (letter >= 'a' && letter <= 'z')
			continue;
		if (letter >= 'A' && letter <= 'Z')
			continue;
		if (letter >= '0' && letter <= '9')
			continue;
		if (letter == '-' || letter == '_')
			continue;

		return 0;
	}

	/* The name is made of the characters a name has. */
	return 1;
}

/*
 * Tells whether count bytes of the data starting at start are exactly a
 * name, compared case-sensitively.  The name given ends with its colon,
 * which is left out of the comparison.
 */
static int
name_equals(
	const char *data,
	size_t start,
	size_t count,
	const char *name)
{
	size_t name_length;
	int difference;

	/* Compares the lengths first, so the bytes compared are all inside the line. */
	name_length = strlen(name) - 1U;
	if (count != name_length)
		return 0;

	/* Compares the bytes. */
	difference = memcmp(data + start, name, name_length);
	if (difference != 0)
		return 0;

	/* The line has this name. */
	return 1;
}

/*
 * Tells which header line a name is.
 */
static enum cf_html_field
field_of(
	const char *data,
	size_t start,
	size_t count)
{
	int matches;

	/* Tries the version line. */
	matches = name_equals(data, start, count, CF_HTML_VERSION_NAME);
	if (matches)
		return CF_HTML_FIELD_VERSION;

	/* Tries the offsets of the HTML as a whole. */
	matches = name_equals(data, start, count, CF_HTML_START_HTML_NAME);
	if (matches)
		return CF_HTML_FIELD_START_HTML;
	matches = name_equals(data, start, count, CF_HTML_END_HTML_NAME);
	if (matches)
		return CF_HTML_FIELD_END_HTML;

	/* Tries the offsets of the fragment. */
	matches = name_equals(data, start, count, CF_HTML_START_FRAGMENT_NAME);
	if (matches)
		return CF_HTML_FIELD_START_FRAGMENT;
	matches = name_equals(data, start, count, CF_HTML_END_FRAGMENT_NAME);
	if (matches)
		return CF_HTML_FIELD_END_FRAGMENT;

	/* Tries the page's address. */
	matches = name_equals(data, start, count, CF_HTML_SOURCE_URL_NAME);
	if (matches)
		return CF_HTML_FIELD_SOURCE_URL;

	/* Any other name is skipped by the caller, the selection offsets among them. */
	return CF_HTML_FIELD_OTHER;
}

/*
 * Finds where a line that starts at start ends: at a CR, an LF or the end
 * of the data.
 */
static size_t
line_end_of(
	const char *data,
	size_t start,
	size_t length)
{
	size_t index;

	/* Walks to the first line break, or the end. */
	index = start;
	while (index < length && data[index] != '\r' && data[index] != '\n')
		index++;

	/* Reports the end of the line, which is not part of it. */
	return index;
}

/*
 * Finds where the next line starts after a line that ends at line_end: past
 * a CR LF pair as one break, or past a lone CR or LF.
 */
static size_t
next_line_of(
	const char *data,
	size_t line_end,
	size_t length)
{
	/* Stays at the end when the line had no break. */
	if (line_end >= length)
		return length;

	/* Takes CR LF as a single break. */
	if (data[line_end] == '\r' && line_end + 1U < length && data[line_end + 1U] == '\n')
		return line_end + 2U;

	/* Takes a lone CR or LF as a break. */
	return line_end + 1U;
}

/*
 * Reads the value of an offset line, once, and lowers the smallest offset
 * to it when it is a number.  A name that was already read keeps its first
 * value.
 */
static int
read_offset_line(
	const char *data,
	size_t start,
	size_t count,
	size_t length,
	struct cf_html_header *header,
	struct cf_html_offset *offset)
{
	struct cf_html_offset read;
	int error;

	/* Ignores a second line of the same name. */
	if (offset->present)
		return 0;

	/* Reads the value, refusing a malformed one. */
	error = parse_offset(data, start, count, length, &read);
	if (error != 0)
		return error;

	/* Keeps the offset, and notes it when it is the smallest number so far. */
	*offset = read;
	if (!read.none && read.value < header->smallest_offset)
		header->smallest_offset = read.value;

	/* Succeeded: the offset line was read. */
	return 0;
}

/*
 * Reads one header line, [line_start, line_end) of the data, whose name
 * ends at the colon, and records what it says.
 */
static int
read_line(
	const char *data,
	size_t length,
	size_t line_start,
	size_t colon,
	size_t line_end,
	struct cf_html_header *header)
{
	enum cf_html_field field;
	size_t value_start;
	size_t value_length;
	int error;

	/* Finds the name and the value on either side of the colon. */
	field = field_of(data, line_start, colon - line_start);
	value_start = colon + 1U;
	value_length = line_end - value_start;

	/* Records the line by its name. */
	error = 0;
	switch (field) {
	case CF_HTML_FIELD_VERSION:
		/* Any value counts: only the presence of the line is required. */
		header->version_present = 1;
		break;
	case CF_HTML_FIELD_START_HTML:
		error = read_offset_line(data, value_start, value_length, length, header, &header->start_html);
		break;
	case CF_HTML_FIELD_END_HTML:
		error = read_offset_line(data, value_start, value_length, length, header, &header->end_html);
		break;
	case CF_HTML_FIELD_START_FRAGMENT:
		error = read_offset_line(data, value_start, value_length, length, header, &header->start_fragment);
		break;
	case CF_HTML_FIELD_END_FRAGMENT:
		error = read_offset_line(data, value_start, value_length, length, header, &header->end_fragment);
		break;
	case CF_HTML_FIELD_SOURCE_URL:
		/* The first URL line wins, as with the offsets. */
		if (!header->url_present) {
			header->url_present = 1;
			header->url_start = value_start;
			header->url_length = value_length;
		}
		break;
	case CF_HTML_FIELD_OTHER:
	default:
		/* Skips a name the decoder does not use. */
		break;
	}

	/* Reports a malformed offset. */
	if (error != 0)
		return error;

	/* Succeeded: the line was recorded or skipped. */
	return 0;
}

/*
 * Reads the header lines from the start of the data, as far as limit.
 *
 * The header goes on while lines have the form name, colon, value; the
 * first line of another form is the HTML.  A line that does not end before
 * the limit is cut by it and is not read, since the limit is where the HTML
 * is said to start; so is a last line with no line break after it.
 */
static int
read_header(
	const char *data,
	size_t length,
	size_t limit,
	struct cf_html_header *header)
{
	size_t position;
	size_t line_end;
	size_t colon;
	int valid;
	int error;

	/* Starts with nothing read and no offset seen. */
	memset(header, 0, sizeof(*header));
	header->smallest_offset = length;

	/* Reads each line that starts inside the header. */
	position = 0U;
	while (position < limit) {
		line_end = line_end_of(data, position, length);

		/* Stops at a line that the limit cuts. */
		if (line_end >= limit)
			break;

		/* Finds the colon, to see whether the line has the form name, colon, value. */
		colon = position;
		while (colon < line_end && data[colon] != ':')
			colon++;
		if (colon >= line_end)
			break;

		/* Ends the header at a line whose name is not one: it is HTML that has a colon in it. */
		valid = name_is_valid(data, position, colon - position);
		if (!valid)
			break;

		/* Records what the line says. */
		error = read_line(data, length, position, colon, line_end, header);
		if (error != 0)
			return error;

		position = next_line_of(data, line_end, length);
	}

	/* Succeeded: the header was read. */
	return 0;
}

/*
 * Tells whether a start and an end that were both read make a range of the
 * data: the start lies on a byte of the data, and the end does not come
 * before it.  Neither is "-1"; the end is already known not to pass the
 * length.
 */
static int
pair_is_valid(
	const struct cf_html_offset *start,
	const struct cf_html_offset *end,
	size_t length)
{
	/* Refuses an offset that says there is no position. */
	if (start->none || end->none)
		return 0;

	/* Refuses a start with no byte at it, which leaves no data after the header. */
	if (start->value >= length)
		return 0;

	/* Refuses an end before the start. */
	if (end->value < start->value)
		return 0;

	/* The pair is a range inside the data. */
	return 1;
}

/*
 * Chooses the range the fragment occupies: from StartFragment to
 * EndFragment when both were given, otherwise from StartHTML to EndHTML.
 * An offset that was given and cannot be used is refused, not skipped.
 */
static int
select_range(
	const struct cf_html_header *header,
	size_t length,
	size_t *start,
	size_t *end)
{
	int html_start_none;
	int html_end_none;
	int fragment_pair;
	int html_pair;
	int valid;

	/* Notes whether each end of the HTML says there is no context. */
	html_start_none = 0;
	if (header->start_html.present && header->start_html.none)
		html_start_none = 1;
	html_end_none = 0;
	if (header->end_html.present && header->end_html.none)
		html_end_none = 1;

	/* Refuses a context that is absent at its end and present at its start. */
	if (html_end_none && !html_start_none)
		return EINVAL;

	/* Refuses a context that is absent at its start and present at its end. */
	if (html_start_none && header->end_html.present && !html_end_none)
		return EINVAL;

	/* Refuses "-1" for the fragment, which always has a position. */
	if (header->start_fragment.present && header->start_fragment.none)
		return EINVAL;
	if (header->end_fragment.present && header->end_fragment.none)
		return EINVAL;

	/* Checks the pair of the fragment, when both ends were given. */
	fragment_pair = 0;
	if (header->start_fragment.present && header->end_fragment.present) {
		valid = pair_is_valid(&header->start_fragment, &header->end_fragment, length);
		if (!valid)
			return EINVAL;

		fragment_pair = 1;
	}

	/* Checks the pair of the HTML, when both ends were given and name a context. */
	html_pair = 0;
	if (header->start_html.present && header->end_html.present && !html_start_none) {
		valid = pair_is_valid(&header->start_html, &header->end_html, length);
		if (!valid)
			return EINVAL;

		html_pair = 1;
	}

	/* Prefers the fragment, as it is what was selected. */
	if (fragment_pair) {
		*start = header->start_fragment.value;
		*end = header->end_fragment.value;

		/* Succeeded: the fragment's own range. */
		return 0;
	}

	/* Falls back to the HTML as a whole. */
	if (html_pair) {
		*start = header->start_html.value;
		*end = header->end_html.value;

		/* Succeeded: the range of the HTML. */
		return 0;
	}

	/* Refuses data with no usable pair of offsets. */
	return EINVAL;
}
