/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hosts of URLs (the WHATWG URL Standard's host parser): IPv6
 * addresses in brackets, opaque hosts of the schemes that are not
 * special, and domains, which become ASCII (the internationalized labels
 * as punycode) and may turn out to be IPv4 addresses.
 *
 * Domain to ASCII is a reduced UTS #46: ASCII is folded to lower case,
 * the full-width forms, the compatibility spaces, the mathematical letters
 * and the ideographic full stops are mapped, the characters UTS #46
 * ignores are dropped, noncharacters are refused, other characters are
 * folded to lower case by the Unicode case table, and a label with
 * non-ASCII characters is encoded with punycode (RFC 3492).  The rest of
 * UTS #46 (NFC, the full mapping table, the disallowed characters, the
 * bidi and joiner rules) comes later.
 */

#include "net/net.h"
#include "base/unicode.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The parameters of punycode (RFC 3492 §5). */
#define PUNY_BASE		36U
#define PUNY_TMIN		1U
#define PUNY_TMAX		26U
#define PUNY_SKEW		38U
#define PUNY_DAMP		700U
#define PUNY_INITIAL_BIAS	72U
#define PUNY_INITIAL_N		128U

/* The largest 32-bit value, which punycode's arithmetic must not pass. */
#define PUNY_MAX		0xffffffffU

/* The number of 16-bit pieces of an IPv6 address. */
#define HOST_IPV6_PIECES	8

/*
 * An IPv6 address while it is parsed: its text, where the parser stands,
 * the pieces so far, the next piece, and where the compressed zeros are
 * (-1 for none).
 */
struct host_ipv6_parser {
	const char *text;
	size_t length;
	size_t pointer;
	uint16_t pieces[HOST_IPV6_PIECES];
	int piece_index;
	int compress;
};

static int host_ipv6(const char *text, size_t length, struct wb_buffer *out);
static int host_ipv6_char(const struct host_ipv6_parser *parser);
static int host_ipv6_piece(struct host_ipv6_parser *parser);
static int host_ipv6_ipv4(struct host_ipv6_parser *parser);
static int host_ipv6_serialize(const uint16_t *pieces, struct wb_buffer *out);
static int host_hex(int c);
static int host_opaque(const char *text, size_t length, struct wb_buffer *out);
static int host_domain(const char *text, size_t length, struct wb_buffer *out);
static int host_code_points(const char *text, size_t length, uint32_t **points, size_t *count);
static int host_to_ascii(const uint32_t *points, size_t count, struct wb_buffer *out);
static int host_map(uint32_t point, uint32_t *mapped, size_t *count);
static int host_label(const uint32_t *points, size_t count, struct wb_buffer *out);
static int host_check_punycode(const char *label, size_t length);
static int host_ends_in_number(const char *text, size_t length);
static int host_ipv4_number(const char *text, size_t length, uint64_t *number);
static int host_ipv4(const char *text, size_t length, struct wb_buffer *out);
static uint32_t puny_adapt(uint32_t delta, uint32_t points, int first);
static uint32_t puny_threshold(uint32_t k, uint32_t bias);
static char puny_digit(uint32_t value);
static int puny_encode(const uint32_t *points, size_t count, struct wb_buffer *out);
static int puny_encode_delta(uint32_t delta, uint32_t bias, struct wb_buffer *out);
static int host_forbidden(int c, int domain);

/*
 * Parses a host (the text between the authority and the port or path)
 * and writes its serialization: an IPv6 address in brackets, an opaque
 * host (opaque set, for a scheme that is not special), an IPv4 address,
 * or an ASCII domain.  Returns EINVAL when the text is not a host.
 */
int
net_host_parse(
	const char *input,
	size_t length,
	int opaque,
	struct wb_buffer *out)
{
	int error;

	/* An IPv6 address in brackets. */
	if (length > 0 && input[0] == '[') {
		if (input[length - 1U] != ']')
			return EINVAL;
		error = host_ipv6(input + 1, length - 2U, out);
		return error;
	}

	/* The host of a scheme that is not special stays as it is, percent-encoded. */
	if (opaque) {
		error = host_opaque(input, length, out);
		return error;
	}

	/* Otherwise a domain, or an IPv4 address. */
	error = host_domain(input, length, out);
	if (error != 0)
		return error;

	/* Succeeded: the host is written. */
	return 0;
}

/* Parses an IPv6 address (without its brackets) and writes it in brackets. */
static int
host_ipv6(
	const char *text,
	size_t length,
	struct wb_buffer *out)
{
	struct host_ipv6_parser parser;
	uint16_t piece;
	int swaps;
	int index;
	int c;
	int error;

	/* The parser at the start. */
	memset(&parser, 0, sizeof(parser));
	parser.text = text;
	parser.length = length;
	parser.compress = -1;

	/* A leading :: compresses the first pieces. */
	c = host_ipv6_char(&parser);
	if (c == ':') {
		if (length < 2U || text[1] != ':')
			return EINVAL;
		parser.pointer = 2;
		parser.piece_index = 1;
		parser.compress = 1;
	}

	/* The pieces, each after the last. */
	while (parser.pointer < length) {
		error = host_ipv6_piece(&parser);
		if (error != 0)
			return error;
	}

	/* Without compression the address must be whole. */
	if (parser.compress == -1 && parser.piece_index != HOST_IPV6_PIECES)
		return EINVAL;

	/* The compressed zeros move the pieces after them to the end. */
	if (parser.compress != -1) {
		swaps = parser.piece_index - parser.compress;
		index = HOST_IPV6_PIECES - 1;
		while (index != 0 && swaps > 0) {
			piece = parser.pieces[parser.compress + swaps - 1];
			parser.pieces[parser.compress + swaps - 1] = parser.pieces[index];
			parser.pieces[index] = piece;
			index--;
			swaps--;
		}
	}

	/* The serialization. */
	error = host_ipv6_serialize(parser.pieces, out);
	if (error != 0)
		return error;

	/* Succeeded: the address is written. */
	return 0;
}

/* Reports the IPv6 parser's current character, or -1 at the end. */
static int
host_ipv6_char(
	const struct host_ipv6_parser *parser)
{
	/* Past the end. */
	if (parser->pointer >= parser->length)
		return -1;

	/* The character. */
	return (unsigned char)parser->text[parser->pointer];
}

/* Parses one piece of an IPv6 address (or the :: that compresses, or an embedded IPv4 address). */
static int
host_ipv6_piece(
	struct host_ipv6_parser *parser)
{
	int value;
	int digits;
	int digit;
	int c;
	int error;

	/* Eight pieces are all an address has. */
	if (parser->piece_index == HOST_IPV6_PIECES)
		return EINVAL;

	/* A second colon in a row is the compression, which comes once. */
	c = host_ipv6_char(parser);
	if (c == ':') {
		if (parser->compress != -1)
			return EINVAL;
		parser->pointer++;
		parser->piece_index++;
		parser->compress = parser->piece_index;
		return 0;
	}

	/* Up to four hexadecimal digits. */
	value = 0;
	for (digits = 0; digits < 4; digits++) {
		c = host_ipv6_char(parser);
		digit = host_hex(c);
		if (digit < 0)
			break;
		value = value * 16 + digit;
		parser->pointer++;
	}

	/* A dot after them starts an embedded IPv4 address in their place. */
	c = host_ipv6_char(parser);
	if (c == '.') {
		if (digits == 0)
			return EINVAL;
		parser->pointer -= (size_t)digits;
		error = host_ipv6_ipv4(parser);
		return error;
	}

	/* A colon (not at the end) or the end follows the piece. */
	if (c == ':') {
		parser->pointer++;
		if (parser->pointer >= parser->length)
			return EINVAL;
	} else if (c != -1) {
		return EINVAL;
	}

	/* Succeeded: the piece is stored. */
	parser->pieces[parser->piece_index] = (uint16_t)value;
	parser->piece_index++;
	return 0;
}

/* Parses the IPv4 address that ends an IPv6 address into its last two pieces. */
static int
host_ipv6_ipv4(
	struct host_ipv6_parser *parser)
{
	int numbers_seen;
	int piece;
	int c;

	/* The address takes two pieces. */
	if (parser->piece_index > HOST_IPV6_PIECES - 2)
		return EINVAL;

	/* Four decimal numbers separated by dots. */
	for (numbers_seen = 0; numbers_seen < 4; numbers_seen++) {
		/* A dot before every number but the first. */
		if (numbers_seen > 0) {
			c = host_ipv6_char(parser);
			if (c != '.')
				return EINVAL;
			parser->pointer++;
		}

		/* At least one digit, and no leading zero before another. */
		c = host_ipv6_char(parser);
		if (c < '0' || c > '9')
			return EINVAL;
		piece = -1;
		for (;;) {
			c = host_ipv6_char(parser);
			if (c < '0' || c > '9')
				break;
			if (piece == 0)
				return EINVAL;
			if (piece == -1) {
				piece = c - '0';
			} else {
				piece = piece * 10 + (c - '0');
			}

			/* No byte is larger than 255. */
			if (piece > 255)
				return EINVAL;
			parser->pointer++;
		}

		/* The byte goes into the piece; every two bytes make one. */
		parser->pieces[parser->piece_index] = (uint16_t)(parser->pieces[parser->piece_index] * 0x100 + piece);
		if (numbers_seen == 1 || numbers_seen == 3)
			parser->piece_index++;
	}

	/* Nothing may follow the address. */
	c = host_ipv6_char(parser);
	if (c != -1)
		return EINVAL;

	/* Succeeded: the address fills its two pieces. */
	return 0;
}

/* Writes an IPv6 address in brackets, its longest run of two or more zero pieces compressed. */
static int
host_ipv6_serialize(
	const uint16_t *pieces,
	struct wb_buffer *out)
{
	int best_start;
	int best_length;
	int run_start;
	int run_length;
	int ignore0;
	int index;
	int error;

	/* The first longest run of zeros, two pieces at least. */
	best_start = -1;
	best_length = 1;
	run_start = -1;
	run_length = 0;
	for (index = 0; index < HOST_IPV6_PIECES; index++) {
		if (pieces[index] != 0) {
			run_start = -1;
			run_length = 0;
			continue;
		}

		/* A zero piece starts or lengthens a run. */
		if (run_start == -1)
			run_start = index;
		run_length++;
		if (run_length > best_length) {
			best_start = run_start;
			best_length = run_length;
		}
	}

	/* The pieces in lower-case hexadecimal, :: in place of the run. */
	error = wb_buffer_append_byte(out, '[');
	ignore0 = 0;
	for (index = 0; error == 0 && index < HOST_IPV6_PIECES; index++) {
		/* The rest of the compressed run. */
		if (ignore0 && pieces[index] == 0)
			continue;
		ignore0 = 0;

		/* The run itself: "::" at the start, ":" after a piece's own colon. */
		if (index == best_start && index == 0) {
			error = wb_buffer_append_string(out, "::");
			ignore0 = 1;
			continue;
		} else if (index == best_start) {
			error = wb_buffer_append_byte(out, ':');
			ignore0 = 1;
			continue;
		}

		/* A piece and its colon. */
		error = wb_buffer_printf(out, "%x", (unsigned)pieces[index]);
		if (error == 0 && index != HOST_IPV6_PIECES - 1)
			error = wb_buffer_append_byte(out, ':');
	}

	/* The closing bracket. */
	if (error == 0)
		error = wb_buffer_append_byte(out, ']');
	if (error != 0)
		return error;

	/* Succeeded: the address is written. */
	return 0;
}

/* Reports a hexadecimal digit's value, or -1 for another character (or the end). */
static int
host_hex(
	int c)
{
	/* The three ranges of digits. */
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	/* Not a digit. */
	return -1;
}

/* Checks an opaque host and writes it with its controls and non-ASCII bytes percent-encoded. */
static int
host_opaque(
	const char *text,
	size_t length,
	struct wb_buffer *out)
{
	size_t index;
	int forbidden;
	int c;
	int error;

	/* No forbidden host code point (a % is allowed). */
	for (index = 0; index < length; index++) {
		c = (unsigned char)text[index];
		forbidden = host_forbidden(c, 0);
		if (forbidden)
			return EINVAL;
	}

	/* The C0 control percent-encode set. */
	error = 0;
	for (index = 0; error == 0 && index < length; index++) {
		c = (unsigned char)text[index];
		if (c < 0x20 || c > 0x7e) {
			error = wb_buffer_printf(out, "%%%02X", (unsigned)c);
		} else {
			error = wb_buffer_append_byte(out, (unsigned char)c);
		}
	}

	/* A buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the host is written. */
	return 0;
}

/* Parses a domain: percent-decoded, UTF-8 decoded, to ASCII, checked, and an IPv4 address when it ends in a number. */
static int
host_domain(
	const char *text,
	size_t length,
	struct wb_buffer *out)
{
	struct wb_buffer ascii;
	uint32_t *points;
	size_t count;
	size_t index;
	int forbidden;
	int number;
	int error;

	/* The code points, then the domain in ASCII. */
	wb_buffer_init(&ascii);
	error = host_code_points(text, length, &points, &count);
	if (error != 0)
		return error;
	error = host_to_ascii(points, count, &ascii);
	free(points);

	/* No forbidden domain code point in it. */
	for (index = 0; error == 0 && index < ascii.length; index++) {
		forbidden = host_forbidden(ascii.data[index], 1);
		if (forbidden)
			error = EINVAL;
	}

	/* A domain that ends in a number is an IPv4 address; any other is itself. */
	number = 0;
	if (error == 0)
		number = host_ends_in_number((const char *)ascii.data, ascii.length);
	if (error == 0 && number) {
		error = host_ipv4((const char *)ascii.data, ascii.length, out);
	} else if (error == 0) {
		error = wb_buffer_append(out, ascii.data, ascii.length);
	}

	/* The ASCII copy goes. */
	wb_buffer_release(&ascii);
	if (error != 0)
		return error;

	/* Succeeded: the host is written. */
	return 0;
}

/* Decodes a domain's escapes and then its UTF-8 into a new array of code points. */
static int
host_code_points(
	const char *text,
	size_t length,
	uint32_t **points,
	size_t *count)
{
	struct wb_buffer decoded;
	uint32_t point;
	size_t index;
	size_t used;
	int error;

	/* The bytes the escapes stand for. */
	*points = NULL;
	*count = 0;
	wb_buffer_init(&decoded);
	error = net_percent_decode(text, length, &decoded);
	if (error != 0) {
		wb_buffer_release(&decoded);
		return error;
	}

	/* An array large enough for one code point a byte. */
	*points = malloc((decoded.length + 1U) * sizeof(**points));
	if (*points == NULL) {
		wb_buffer_release(&decoded);
		return ENOMEM;
	}

	/* The code points (bytes that are not UTF-8 decode as the replacement character). */
	for (index = 0; index < decoded.length; index += used) {
		used = wb_utf8_decode(decoded.data + index, decoded.length - index, &point);
		if (used == 0)
			used = 1;
		(*points)[*count] = point;
		(*count)++;
	}

	/* Succeeded: the code points are there. */
	wb_buffer_release(&decoded);
	return 0;
}

/* Turns a domain's code points into ASCII: mapped, split at the dots, each label ASCII or punycode. */
static int
host_to_ascii(
	const uint32_t *points,
	size_t count,
	struct wb_buffer *out)
{
	uint32_t *mapped;
	size_t mapped_count;
	size_t start;
	size_t index;
	int error;

	/* The mapping of each code point (up to three each). */
	mapped = malloc((count * 3U + 1U) * sizeof(*mapped));
	if (mapped == NULL)
		return ENOMEM;
	mapped_count = 0;
	error = 0;
	for (index = 0; error == 0 && index < count; index++)
		error = host_map(points[index], mapped, &mapped_count);

	/* Each label between the dots. */
	start = 0;
	for (index = 0; error == 0 && index <= mapped_count; index++) {
		if (index < mapped_count && mapped[index] != '.')
			continue;
		error = host_label(mapped + start, index - start, out);
		if (error == 0 && index < mapped_count)
			error = wb_buffer_append_byte(out, '.');
		start = index + 1U;
	}

	/* The mapped copy goes; an empty domain is not one. */
	free(mapped);
	if (error != 0)
		return error;
	if (out->length == 0)
		return EINVAL;

	/* Succeeded: the domain is ASCII. */
	return 0;
}

/* Maps one code point of a domain into the array (none, one or up to three); EINVAL for one not allowed. */
static int
host_map(
	uint32_t point,
	uint32_t *mapped,
	size_t *count)
{
	uint32_t folded[3];
	size_t folds;
	size_t fold;

	/* The replacement character (bytes that were not UTF-8) and the noncharacters are not allowed. */
	if (point == WB_REPLACEMENT)
		return EINVAL;
	if ((point >= 0xfdd0U && point <= 0xfdefU) || (point & 0xfffeU) == 0xfffeU)
		return EINVAL;

	/* Ignored characters: the soft hyphen, the zero widths, the variation selectors. */
	if (point == 0x00adU || point == 0x034fU || (point >= 0x180bU && point <= 0x180dU) || point == 0x200bU ||
	    point == 0x2060U || (point >= 0xfe00U && point <= 0xfe0fU) || point == 0xfeffU)
		return 0;

	/* The compatibility spaces are spaces (which a domain then forbids). */
	if (point == 0x00a0U || (point >= 0x2000U && point <= 0x200aU) || point == 0x202fU || point == 0x205fU ||
	    point == 0x3000U)
		point = ' ';

	/* The full stops of other scripts are dots; the full-width forms their ASCII. */
	if (point == 0x3002U || point == 0xff0eU || point == 0xff61U)
		point = '.';
	if (point >= 0xff01U && point <= 0xff5eU)
		point -= 0xfee0U;

	/* The mathematical letters (capital, then small, in each style) and digits are their ASCII ones. */
	if (point >= 0x1d400U && point <= 0x1d6a3U) {
		point = (point - 0x1d400U) % 52U;
		if (point < 26U) {
			point += 'a';
		} else {
			point += 'a' - 26U;
		}
	} else if (point >= 0x1d7ceU && point <= 0x1d7ffU) {
		point = '0' + (point - 0x1d7ceU) % 10U;
	}

	/* ASCII in lower case. */
	if (point >= 'A' && point <= 'Z')
		point += 0x20U;
	if (point < 0x80U) {
		mapped[*count] = point;
		(*count)++;
		return 0;
	}

	/* Other characters by the Unicode lower-case mapping. */
	folds = wb_case_map(point, 0, folded);
	for (fold = 0; fold < folds && fold < 3U; fold++) {
		mapped[*count] = folded[fold];
		(*count)++;
	}

	/* Succeeded: the code point is mapped. */
	return 0;
}

/* Writes one label in ASCII: as it is, or "xn--" and its punycode; an xn-- label must be valid punycode. */
static int
host_label(
	const uint32_t *points,
	size_t count,
	struct wb_buffer *out)
{
	size_t start;
	size_t index;
	int ascii;
	int valid;
	int differs;
	int error;

	/* Whether the label is ASCII already. */
	ascii = 1;
	for (index = 0; index < count; index++) {
		if (points[index] >= 0x80U)
			ascii = 0;
	}

	/* A label with other characters is punycode. */
	if (!ascii) {
		error = wb_buffer_append_string(out, "xn--");
		if (error == 0)
			error = puny_encode(points, count, out);
		return error;
	}

	/* An ASCII label as it is. */
	start = out->length;
	error = 0;
	for (index = 0; error == 0 && index < count; index++)
		error = wb_buffer_append_byte(out, (unsigned char)points[index]);
	if (error != 0)
		return error;

	/* One that says it is punycode must be. */
	if (count >= 4U) {
		differs = memcmp(out->data + start, "xn--", 4);
		if (differs == 0) {
			valid = host_check_punycode((const char *)out->data + start + 4U, count - 4U);
			if (!valid)
				return EINVAL;
		}
	}

	/* Succeeded: the label is written. */
	return 0;
}

/* Tells whether a label's text after "xn--" decodes as punycode to a label with a non-ASCII character. */
static int
host_check_punycode(
	const char *label,
	size_t length)
{
	uint32_t n;
	uint32_t i;
	uint32_t bias;
	uint32_t out_count;
	uint32_t old_i;
	uint32_t w;
	uint32_t k;
	uint32_t digit;
	uint32_t t;
	size_t basic;
	size_t in;
	size_t index;
	int non_ascii;
	int value;

	/* "xn--" alone passes (UTS #46 as the URL Standard runs it lets it through). */
	if (length == 0)
		return 1;

	/* The basic code points are those before the last delimiter. */
	basic = 0;
	for (index = 0; index < length; index++) {
		if (label[index] == '-')
			basic = index;
	}

	/* The digits start after the delimiter, and there must be some. */
	out_count = (uint32_t)basic;
	in = 0;
	if (basic > 0)
		in = basic + 1U;
	if (basic > 0 && in >= length)
		return 0;

	/* The deltas that insert the other code points. */
	n = PUNY_INITIAL_N;
	i = 0;
	bias = PUNY_INITIAL_BIAS;
	non_ascii = 0;
	while (in < length) {
		/* One delta, as digits of variable length. */
		old_i = i;
		w = 1;
		for (k = PUNY_BASE;; k += PUNY_BASE) {
			if (in >= length)
				return 0;
			value = (unsigned char)label[in];
			in++;
			if (value >= '0' && value <= '9') {
				digit = (uint32_t)(value - '0' + 26);
			} else if ((value | 0x20) >= 'a' && (value | 0x20) <= 'z') {
				digit = (uint32_t)((value | 0x20) - 'a');
			} else {
				return 0;
			}

			/* The digit's weight, without overflow. */
			if (digit > (PUNY_MAX - i) / w)
				return 0;
			i += digit * w;
			t = puny_threshold(k, bias);
			if (digit < t)
				break;
			if (w > PUNY_MAX / (PUNY_BASE - t))
				return 0;
			w *= PUNY_BASE - t;
		}

		/* The code point the delta inserts, which must be a scalar value. */
		out_count++;
		bias = puny_adapt(i - old_i, out_count, old_i == 0);
		if (i / out_count > PUNY_MAX - n)
			return 0;
		n += i / out_count;
		i %= out_count;
		if (n > WB_CODE_POINT_MAX || (n >= 0xd800U && n <= 0xdfffU))
			return 0;
		if (n >= 0x80U)
			non_ascii = 1;
		i++;
	}

	/* A valid label that decodes to ASCII only is not an internationalized label. */
	return non_ascii;
}

/* Tells whether a domain ends in a number (its last label, a trailing dot aside, is digits or 0x and hexadecimal). */
static int
host_ends_in_number(
	const char *text,
	size_t length)
{
	uint64_t number;
	size_t end;
	size_t start;
	size_t index;
	int digits;
	int valid;

	/* The last label, without an empty one after a final dot. */
	end = length;
	if (end > 0 && text[end - 1U] == '.') {
		end--;
		if (end == 0)
			return 0;
	}

	/* Back to the dot before it. */
	start = end;
	while (start > 0 && text[start - 1U] != '.')
		start--;
	if (start == end)
		return 0;

	/* All digits. */
	digits = 1;
	for (index = start; index < end; index++) {
		if (text[index] < '0' || text[index] > '9')
			digits = 0;
	}

	/* That is a number. */
	if (digits)
		return 1;

	/* Or a number of the IPv4 parser (0x and hexadecimal). */
	valid = host_ipv4_number(text + start, end - start, &number);
	if (valid && end - start >= 2U && text[start] == '0' && (text[start + 1U] | 0x20) == 'x')
		return 1;
	return 0;
}

/* Parses one part of an IPv4 address: decimal, octal after 0, or hexadecimal after 0x; nonzero when it is one. */
static int
host_ipv4_number(
	const char *text,
	size_t length,
	uint64_t *number)
{
	size_t index;
	int radix;
	int digit;

	/* The radix from the prefix. */
	*number = 0;
	if (length == 0)
		return 0;
	radix = 10;
	if (length >= 2U && text[0] == '0' && (text[1] | 0x20) == 'x') {
		text += 2;
		length -= 2U;
		radix = 16;
	} else if (length >= 2U && text[0] == '0') {
		text += 1;
		length -= 1U;
		radix = 8;
	}

	/* The digits (none after 0x is zero), the value kept below 2^40. */
	for (index = 0; index < length; index++) {
		digit = host_hex((unsigned char)text[index]);
		if (digit < 0 || digit >= radix)
			return 0;
		if (*number < ((uint64_t)1 << 40))
			*number = *number * (uint64_t)radix + (uint64_t)digit;
	}

	/* A number. */
	return 1;
}

/* Parses an IPv4 address in its forms (one to four parts) and writes it dotted. */
static int
host_ipv4(
	const char *text,
	size_t length,
	struct wb_buffer *out)
{
	uint64_t numbers[4];
	uint64_t address;
	uint64_t limit;
	size_t count;
	size_t start;
	size_t index;
	int valid;
	int error;

	/* The parts between the dots (a final empty one is dropped). */
	if (length > 0 && text[length - 1U] == '.')
		length--;
	count = 0;
	start = 0;
	for (index = 0; index <= length; index++) {
		if (index < length && text[index] != '.')
			continue;
		if (count == 4)
			return EINVAL;
		valid = host_ipv4_number(text + start, index - start, &numbers[count]);
		if (!valid)
			return EINVAL;
		count++;
		start = index + 1U;
	}

	/* Each part but the last is a byte. */
	for (index = 0; index + 1U < count; index++) {
		if (numbers[index] > 255U)
			return EINVAL;
	}

	/* The last part must fit the bytes left for it. */
	limit = (uint64_t)1 << (8U * (5U - count));
	if (numbers[count - 1U] >= limit)
		return EINVAL;

	/* The address. */
	address = numbers[count - 1U];
	for (index = 0; index + 1U < count; index++)
		address += numbers[index] << (8U * (3U - index));

	/* Dotted decimal. */
	error = wb_buffer_printf(out, "%u.%u.%u.%u", (unsigned)((address >> 24) & 0xffU), (unsigned)((address >> 16) & 0xffU),
	    (unsigned)((address >> 8) & 0xffU), (unsigned)(address & 0xffU));
	if (error != 0)
		return error;

	/* Succeeded: the address is written. */
	return 0;
}

/* Adapts punycode's bias after a delta (RFC 3492 §6.1). */
static uint32_t
puny_adapt(
	uint32_t delta,
	uint32_t points,
	int first)
{
	uint32_t k;

	/* Scales the delta down. */
	if (first) {
		delta /= PUNY_DAMP;
	} else {
		delta /= 2U;
	}

	/* Adds its share of the points. */
	delta += delta / points;

	/* Divides it until it is small. */
	for (k = 0; delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2U; k += PUNY_BASE)
		delta /= PUNY_BASE - PUNY_TMIN;

	/* The new bias. */
	return k + (PUNY_BASE - PUNY_TMIN + 1U) * delta / (delta + PUNY_SKEW);
}

/* Reports the threshold of a digit position k under a bias (tmin, k - bias, or tmax). */
static uint32_t
puny_threshold(
	uint32_t k,
	uint32_t bias)
{
	/* Clamped to tmin and tmax. */
	if (k <= bias)
		return PUNY_TMIN;
	if (k >= bias + PUNY_TMAX)
		return PUNY_TMAX;

	/* In between. */
	return k - bias;
}

/* Reports the character of a punycode digit (a to z, then 0 to 9). */
static char
puny_digit(
	uint32_t value)
{
	/* The letters, then the digits. */
	if (value < 26U)
		return (char)('a' + value);
	return (char)('0' + value - 26U);
}

/* Encodes a label's code points as punycode (RFC 3492 §6.3). */
static int
puny_encode(
	const uint32_t *points,
	size_t count,
	struct wb_buffer *out)
{
	uint32_t n;
	uint32_t delta;
	uint32_t bias;
	uint32_t handled;
	uint32_t basic;
	uint32_t m;
	size_t index;
	int error;

	/* The basic code points first. */
	error = 0;
	basic = 0;
	for (index = 0; error == 0 && index < count; index++) {
		if (points[index] >= 0x80U)
			continue;
		error = wb_buffer_append_byte(out, (unsigned char)points[index]);
		basic++;
	}

	/* A delimiter after them when there were any. */
	if (error == 0 && basic > 0)
		error = wb_buffer_append_byte(out, '-');

	/* Each other code point in order of value, as deltas. */
	n = PUNY_INITIAL_N;
	delta = 0;
	bias = PUNY_INITIAL_BIAS;
	handled = basic;
	while (error == 0 && handled < count) {
		/* The next code point to insert. */
		m = PUNY_MAX;
		for (index = 0; index < count; index++) {
			if (points[index] >= n && points[index] < m)
				m = points[index];
		}

		/* The delta up to it, without overflow. */
		if ((m - n) > (PUNY_MAX - delta) / (handled + 1U))
			return EINVAL;
		delta += (m - n) * (handled + 1U);
		n = m;

		/* Each place it goes. */
		for (index = 0; error == 0 && index < count; index++) {
			if (points[index] < n)
				delta++;
			if (points[index] != n)
				continue;
			error = puny_encode_delta(delta, bias, out);
			bias = puny_adapt(delta, handled + 1U, handled == basic);
			delta = 0;
			handled++;
		}

		/* The next value. */
		delta++;
		n++;
	}

	/* A buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the label is encoded. */
	return 0;
}

/* Writes one delta as punycode's variable-length digits. */
static int
puny_encode_delta(
	uint32_t delta,
	uint32_t bias,
	struct wb_buffer *out)
{
	uint32_t q;
	uint32_t k;
	uint32_t t;
	char digit;
	int error;

	/* The digits below each threshold, then the last. */
	q = delta;
	for (k = PUNY_BASE;; k += PUNY_BASE) {
		t = puny_threshold(k, bias);
		if (q < t)
			break;
		digit = puny_digit(t + (q - t) % (PUNY_BASE - t));
		error = wb_buffer_append_byte(out, (unsigned char)digit);
		if (error != 0)
			return error;
		q = (q - t) / (PUNY_BASE - t);
	}

	/* The last digit. */
	digit = puny_digit(q);
	error = wb_buffer_append_byte(out, (unsigned char)digit);
	if (error != 0)
		return error;

	/* Succeeded: the delta is written. */
	return 0;
}

/* Tells whether a byte is a forbidden host code point (or, for a domain, a forbidden domain code point). */
static int
host_forbidden(
	int c,
	int domain)
{
	/* The forbidden host code points. */
	if (c == 0x00 || c == 0x09 || c == 0x0a || c == 0x0d || c == ' ' || c == '#' || c == '/' || c == ':' ||
	    c == '<' || c == '>' || c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' || c == '^' || c == '|')
		return 1;

	/* A domain also forbids the C0 controls, % and DELETE. */
	if (domain && (c <= 0x1f || c == '%' || c == 0x7f))
		return 1;
	return 0;
}
