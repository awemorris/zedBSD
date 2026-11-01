/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reader of DER (ITU-T X.690).
 */

#include "cryc-der.h"

#include <string.h>

/* The mask of the tag number bits of an identifier octet (8.1.2.2). */
#define DER_TAG_NUMBER_MASK	0x1fU

/* The tag number bits that announce the high-tag-number form (8.1.2.4). */
#define DER_TAG_HIGH_FORM	0x1fU

/* The class bits of an identifier octet (8.1.2.2). */
#define DER_CLASS_MASK		0xc0U

/* The length octet that announces the indefinite form (8.1.3.6). */
#define DER_LENGTH_INDEFINITE	0x80U

/* The lengths below this value use the short form (8.1.3.4). */
#define DER_LENGTH_SHORT_LIMIT	0x80U

/* The most octets a length of at most CRYC_DER_MAX_LENGTH can need. */
#define DER_LENGTH_MAX_OCTETS	4U

/* The seconds in a day. */
#define DER_SECONDS_PER_DAY	86400

/*
 * The days from 0000-01-01 to 1970-01-01 in the proleptic Gregorian
 * calendar (year 0 is a leap year); see der_days_before_year.
 */
#define DER_DAYS_TO_EPOCH	719528

/* The text lengths of YYMMDDHHMMSSZ and YYYYMMDDHHMMSSZ (RFC 5280 4.1.2.5). */
#define DER_UTC_TIME_LENGTH		13U
#define DER_GENERALIZED_TIME_LENGTH	15U

/* Forward declarations. */
static int der_tag_is_valid(unsigned tag);
static int der_read_length(const uint8_t *data, size_t size, size_t *header_size, size_t *length);
static int der_is_leap_year(unsigned year);
static unsigned der_days_in_month(unsigned year, unsigned month);
static int64_t der_days_before_year(unsigned year);
static int der_read_digits(const uint8_t *text, size_t count, unsigned *value);

/*
 * Starts a reader over size bytes at data (data may be NULL only when size
 * is 0).
 */
void
cryc_der_init(
	struct cryc_der *der,
	const uint8_t *data,
	size_t size)
{
	/* The reader sees the caller's bytes. */
	der->data = data;
	der->size = size;
}

/*
 * Returns 1 when nothing is left to read, else 0.
 */
int
cryc_der_empty(
	const struct cryc_der *der)
{
	/* Nothing is left when no bytes remain. */
	if (der->size == 0)
		return 1;

	/* Bytes remain. */
	return 0;
}

/*
 * Reads the next element (identifier, length, contents: 8.1.1) whatever
 * its tag, and advances past it.
 *
 * An element whose identifier is a universal primitive type must be
 * primitive (8.2.1, 8.3.1, 8.6.2, 8.7.1, 8.8.1, 8.19.1, 10.2 for the
 * strings and times), and a SEQUENCE or SET must be constructed (8.9.1,
 * 8.11.1).  The constructed forms of the string types are refused as
 * DER requires.
 */
int
cryc_der_next(
	struct cryc_der *der,
	struct cryc_der_element *element)
{
	size_t header_size;
	size_t length;
	unsigned tag;
	int error;

	/* The caller sees nothing of a failed read. */
	memset(element, 0, sizeof(*element));

	/* Refuses an empty reader. */
	if (der->size == 0)
		return EINVAL;

	/* Refuses a tag this reader does not read. */
	tag = der->data[0];
	if (!der_tag_is_valid(tag))
		return EINVAL;

	/* Reads the length octets, which must be minimal and fit what is left. */
	error = der_read_length(der->data, der->size, &header_size, &length);
	if (error != 0)
		return error;

	/* Publishes the element as views into the caller's bytes. */
	element->tag = tag;
	element->contents = der->data + header_size;
	element->length = length;
	element->encoding = der->data;
	element->encoding_size = header_size + length;

	/* Moves the reader past the element. */
	der->data += header_size + length;
	der->size -= header_size + length;

	/* Succeeded: one element was read. */
	return 0;
}

/*
 * Reads the next element and requires its tag to be tag.
 */
int
cryc_der_expect(
	struct cryc_der *der,
	unsigned tag,
	struct cryc_der_element *element)
{
	struct cryc_der trial;
	int error;

	/* Reads on a copy so that a mismatch leaves the reader where it was. */
	trial = *der;
	error = cryc_der_next(&trial, element);
	if (error != 0)
		return error;

	/* Refuses an element of another type. */
	if (element->tag != tag) {
		memset(element, 0, sizeof(*element));
		return EINVAL;
	}

	/* Commits the read. */
	*der = trial;

	/* Succeeded: the element has the expected tag. */
	return 0;
}

/*
 * Reads the next element only when its tag is tag (an OPTIONAL or DEFAULT
 * field).
 */
int
cryc_der_optional(
	struct cryc_der *der,
	unsigned tag,
	struct cryc_der_element *element,
	int *present)
{
	int error;

	/* Nothing is read yet. */
	memset(element, 0, sizeof(*element));
	*present = 0;

	/* An exhausted reader has no such field. */
	if (der->size == 0)
		return 0;

	/* A different tag next means the field is absent and nothing is read. */
	if ((unsigned)der->data[0] != tag)
		return 0;

	/* Reads the field; a malformed one is an error, not an absence. */
	error = cryc_der_next(der, element);
	if (error != 0)
		return error;

	/* Reports the field as present. */
	*present = 1;

	/* Succeeded: the field was read. */
	return 0;
}

/*
 * Starts a reader over the contents of a constructed element (a SEQUENCE,
 * a SET, an explicit tag).
 */
int
cryc_der_enter(
	const struct cryc_der_element *element,
	struct cryc_der *inner)
{
	/* The inner reader is empty unless the element can be entered. */
	inner->data = NULL;
	inner->size = 0;

	/* Refuses a primitive element: it has no elements inside. */
	if ((element->tag & CRYC_DER_CONSTRUCTED) == 0)
		return EINVAL;

	/* The inner reader covers the contents. */
	inner->data = element->contents;
	inner->size = element->length;

	/* Succeeded: the inner reader is positioned at the first inner element. */
	return 0;
}

/*
 * Checks a DER INTEGER (8.3) and gives its magnitude: the big-endian bytes
 * without the leading 0x00 that makes a positive value whose top bit is
 * set (8.3.2).
 *
 * The tag of the element is not examined: an INTEGER under an implicit
 * tag has the same contents rules.
 */
int
cryc_der_integer(
	const struct cryc_der_element *element,
	const uint8_t **magnitude,
	size_t *size)
{
	const uint8_t *contents;
	size_t length;

	/* The caller sees nothing of a failed read. */
	*magnitude = NULL;
	*size = 0;

	/* Refuses an empty INTEGER (8.3.1: at least one octet). */
	contents = element->contents;
	length = element->length;
	if (length == 0)
		return EINVAL;

	/* Refuses a negative value: the top bit of the first octet is the sign (8.3.3). */
	if ((contents[0] & 0x80U) != 0)
		return EINVAL;

	/* Refuses a redundant leading 0x00 octet: the first nine bits would all be 0 (8.3.2). */
	if (length > 1 && contents[0] == 0 && (contents[1] & 0x80U) == 0)
		return EINVAL;

	/* Drops the 0x00 octet that only keeps a value with the top bit set positive. */
	if (length > 1 && contents[0] == 0) {
		contents++;
		length--;
	}

	/* Publishes the magnitude. */
	*magnitude = contents;
	*size = length;

	/* Succeeded: the INTEGER is a minimal non-negative value. */
	return 0;
}

/*
 * Reads a small non-negative INTEGER (a version, a pathLenConstraint, a
 * PSS saltLength) into *value.
 */
int
cryc_der_small_integer(
	const struct cryc_der_element *element,
	uint32_t max,
	uint32_t *value)
{
	const uint8_t *magnitude;
	size_t size;
	size_t index;
	uint32_t number;
	int error;

	/* The caller sees zero after a failed read. */
	*value = 0;

	/* Checks the INTEGER and finds its magnitude. */
	error = cryc_der_integer(element, &magnitude, &size);
	if (error != 0)
		return error;

	/* A magnitude of more than four octets exceeds every uint32_t. */
	if (size > 4)
		return ERANGE;

	/* Reads the magnitude as a big-endian number. */
	number = 0;
	for (index = 0; index < size; index++)
		number = (number << 8) | (uint32_t)magnitude[index];

	/* Refuses a value the caller does not accept. */
	if (number > max)
		return ERANGE;

	/* Publishes the value. */
	*value = number;

	/* Succeeded: the value is within max. */
	return 0;
}

/*
 * Reads a BOOLEAN (8.2, 11.1).
 */
int
cryc_der_boolean(
	const struct cryc_der_element *element,
	int *value)
{
	/* The caller sees false after a failed read. */
	*value = 0;

	/* Refuses a length other than one octet (8.2.1). */
	if (element->length != 1)
		return EINVAL;

	/* FALSE is 0x00 (8.2.2). */
	if (element->contents[0] == 0x00U)
		return 0;

	/* TRUE is 0xff in DER (11.1). */
	if (element->contents[0] == 0xffU) {
		*value = 1;
		return 0;
	}

	/* Refuses any other value: BER allows it, DER does not. */
	return EINVAL;
}

/*
 * Gives the bits of a BIT STRING (8.6, 11.2): bytes and the count of
 * unused bits in the last byte (0 to 7).
 */
int
cryc_der_bit_string(
	const struct cryc_der_element *element,
	const uint8_t **bytes,
	size_t *size,
	unsigned *unused)
{
	const uint8_t *contents;
	size_t length;
	unsigned unused_bits;
	unsigned mask;

	/* The caller sees nothing of a failed read. */
	*bytes = NULL;
	*size = 0;
	*unused = 0;

	/* Refuses an empty BIT STRING: the initial octet is always present (8.6.2.2). */
	contents = element->contents;
	length = element->length;
	if (length == 0)
		return EINVAL;

	/* Refuses an unused-bit count above 7 (8.6.2.2). */
	unused_bits = contents[0];
	if (unused_bits > 7U)
		return EINVAL;

	/* Refuses unused bits when there are no bytes to hold them (8.6.2.3). */
	if (length == 1 && unused_bits != 0)
		return EINVAL;

	/* Refuses unused bits that are not zero (11.2.1). */
	if (unused_bits != 0) {
		mask = (1U << unused_bits) - 1U;
		if (((unsigned)contents[length - 1] & mask) != 0)
			return EINVAL;
	}

	/* Publishes the bytes after the initial octet. */
	*bytes = contents + 1;
	*size = length - 1;
	*unused = unused_bits;

	/* Succeeded: the bits are well formed. */
	return 0;
}

/*
 * Checks that an OID's contents (8.19) are well formed: at least one byte,
 * each subidentifier in the fewest octets (no leading 0x80, 8.19.2), the
 * last byte with its top bit clear.
 */
int
cryc_der_oid(
	const struct cryc_der_element *element)
{
	size_t index;
	int starts_subidentifier;

	/* Refuses an empty OBJECT IDENTIFIER (8.19.1). */
	if (element->length == 0)
		return EINVAL;

	/* Refuses a last octet that continues into a subidentifier that is not there (8.19.2). */
	if ((element->contents[element->length - 1] & 0x80U) != 0)
		return EINVAL;

	/* Refuses a subidentifier that starts with a 0x80 octet: its first seven bits would be zero (8.19.2). */
	starts_subidentifier = 1;
	for (index = 0; index < element->length; index++) {
		if (starts_subidentifier && element->contents[index] == 0x80U)
			return EINVAL;
		starts_subidentifier = (element->contents[index] & 0x80U) == 0;
	}

	/* Succeeded: the contents are a well-formed OBJECT IDENTIFIER. */
	return 0;
}

/*
 * Converts a UTCTime or GeneralizedTime as RFC 5280 4.1.2.5 restricts them
 * (YYMMDDHHMMSSZ, with YY 50..99 meaning 19YY and 00..49 meaning 20YY;
 * YYYYMMDDHHMMSSZ; no fractions, always Z) into seconds since
 * 1970-01-01T00:00:00Z.
 */
int
cryc_der_time(
	const struct cryc_der_element *element,
	int64_t *seconds)
{
	const uint8_t *text;
	size_t digits;
	unsigned year;
	unsigned month;
	unsigned day;
	unsigned hour;
	unsigned minute;
	unsigned second;
	unsigned century_digits;
	unsigned index;
	int64_t days;
	int error;

	/* The caller sees zero after a failed read. */
	*seconds = 0;

	/* Chooses the form from the tag; the length must match the form. */
	text = element->contents;
	if (element->tag == CRYC_DER_UTC_TIME) {
		if (element->length != DER_UTC_TIME_LENGTH)
			return EINVAL;
		century_digits = 2;
	} else if (element->tag == CRYC_DER_GENERALIZED_TIME) {
		if (element->length != DER_GENERALIZED_TIME_LENGTH)
			return EINVAL;
		century_digits = 4;
	} else {
		/* Another type is not a time. */
		return EINVAL;
	}

	/* Refuses a time that is not given in Zulu (RFC 5280 4.1.2.5). */
	digits = element->length - 1;
	if (text[digits] != (uint8_t)'Z')
		return EINVAL;

	/* Reads the year, two digits (UTCTime) or four (GeneralizedTime). */
	error = der_read_digits(text, century_digits, &year);
	if (error != 0)
		return error;

	/* Gives a two-digit year its century: 50..99 is 19YY, 00..49 is 20YY. */
	if (century_digits == 2) {
		if (year >= 50U)
			year += 1900U;
		else
			year += 2000U;
	}

	/* Reads the five two-digit fields after the year. */
	index = century_digits;
	error = der_read_digits(text + index, 2, &month);
	if (error != 0)
		return error;
	error = der_read_digits(text + index + 2U, 2, &day);
	if (error != 0)
		return error;
	error = der_read_digits(text + index + 4U, 2, &hour);
	if (error != 0)
		return error;
	error = der_read_digits(text + index + 6U, 2, &minute);
	if (error != 0)
		return error;
	error = der_read_digits(text + index + 8U, 2, &second);
	if (error != 0)
		return error;

	/* Refuses a month that does not exist. */
	if (month < 1U || month > 12U)
		return EINVAL;

	/* Refuses a day that the month does not have (leap years included). */
	if (day < 1U || day > der_days_in_month(year, month))
		return EINVAL;

	/* Refuses an hour, minute or second out of range; a leap second (60) is refused. */
	if (hour > 23U || minute > 59U || second > 59U)
		return EINVAL;

	/* Counts the days from 1970-01-01: whole years, then the months before this one, then the day. */
	days = der_days_before_year(year) - DER_DAYS_TO_EPOCH;
	for (index = 1; index < month; index++)
		days += (int64_t)der_days_in_month(year, index);
	days += (int64_t)day - 1;

	/* Publishes the seconds since the epoch. */
	*seconds = days * DER_SECONDS_PER_DAY + (int64_t)hour * 3600 + (int64_t)minute * 60 + (int64_t)second;

	/* Succeeded: the time was converted. */
	return 0;
}

/*
 * Tells whether an identifier octet is one this reader accepts: tag number
 * below 31 (8.1.2.4), and the primitive or constructed form the universal
 * type requires.
 */
static int
der_tag_is_valid(
	unsigned tag)
{
	unsigned number;
	int constructed;

	/* Refuses the high-tag-number form: X.509 needs no tag number of 31 or more. */
	number = tag & DER_TAG_NUMBER_MASK;
	if (number == DER_TAG_HIGH_FORM)
		return 0;

	/* Tags outside the universal class carry no restriction here. */
	if ((tag & DER_CLASS_MASK) != 0)
		return 1;

	/* A SEQUENCE and a SET are always constructed (8.9.1, 8.11.1). */
	constructed = (tag & CRYC_DER_CONSTRUCTED) != 0;
	if (number == 16U || number == 17U) {
		if (!constructed)
			return 0;
		return 1;
	}

	/* Every other universal type is read in its primitive form (8.x, 10.2). */
	if (constructed)
		return 0;

	/* Accepted: a primitive universal type. */
	return 1;
}

/*
 * Reads the length octets that start at data[1] (8.1.3) and checks them
 * against DER and against the bytes left: size is the count of bytes from
 * the identifier octet.  *header_size receives the identifier and length
 * octets, *length the contents length.
 */
static int
der_read_length(
	const uint8_t *data,
	size_t size,
	size_t *header_size,
	size_t *length)
{
	size_t octets;
	size_t index;
	uint32_t value;
	unsigned first;

	/* Needs at least the first length octet. */
	if (size < 2)
		return EINVAL;

	/* Refuses the indefinite form (8.1.3.6): DER uses definite lengths only (10.1). */
	first = data[1];
	if (first == DER_LENGTH_INDEFINITE)
		return EINVAL;

	/* The short form carries the length itself (8.1.3.4). */
	if (first < DER_LENGTH_SHORT_LIMIT) {
		if ((size_t)first > size - 2U)
			return EINVAL;
		*header_size = 2;
		*length = first;
		return 0;
	}

	/* Refuses the reserved value 0xff (8.1.3.5 c) and a count that cannot fit the largest length. */
	octets = first & 0x7fU;
	if (first == 0xffU || octets > DER_LENGTH_MAX_OCTETS)
		return EINVAL;

	/* Refuses a length field cut short by the end of the input. */
	if (octets > size - 2U)
		return EINVAL;

	/* Refuses leading zero octets: the length must use the fewest octets (10.1). */
	if (data[2] == 0)
		return EINVAL;

	/* Reads the length as a big-endian number. */
	value = 0;
	for (index = 0; index < octets; index++)
		value = (value << 8) | (uint32_t)data[2U + index];

	/* Refuses a long form for a length the short form carries (10.1). */
	if (value < DER_LENGTH_SHORT_LIMIT)
		return EINVAL;

	/* Refuses a length above what this reader accepts, or above what is left. */
	if (value > CRYC_DER_MAX_LENGTH)
		return EINVAL;
	if ((size_t)value > size - 2U - octets)
		return EINVAL;

	/* Publishes the header size and the length. */
	*header_size = 2U + octets;
	*length = (size_t)value;

	/* Succeeded: the length is minimal and its contents are present. */
	return 0;
}

/*
 * Tells whether a year is a leap year of the Gregorian calendar.
 */
static int
der_is_leap_year(
	unsigned year)
{
	/* Every year divisible by 400 is a leap year. */
	if (year % 400U == 0)
		return 1;

	/* Other centuries are not. */
	if (year % 100U == 0)
		return 0;

	/* Otherwise every fourth year is. */
	if (year % 4U == 0)
		return 1;

	/* Not a leap year. */
	return 0;
}

/*
 * Gives the number of days of a month (1 to 12) of a year.
 */
static unsigned
der_days_in_month(
	unsigned year,
	unsigned month)
{
	/* The days of each month of a common year (January first). */
	static const unsigned char days[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};

	/* February gains a day in a leap year. */
	if (month == 2U && der_is_leap_year(year))
		return 29U;

	/* The other months have a fixed length. */
	return days[month - 1U];
}

/*
 * Gives the days from 0000-01-01 to the first day of year: 365 for each
 * year before it plus one for each leap year before it (years 0 to
 * year - 1; year 0 is a leap year).
 */
static int64_t
der_days_before_year(
	unsigned year)
{
	int64_t whole;
	int64_t leaps;

	/* Counts the leap years among 0 .. year - 1 by the Gregorian rule. */
	whole = (int64_t)year;
	leaps = (whole + 3) / 4 - (whole + 99) / 100 + (whole + 399) / 400;

	/* Reports the days. */
	return whole * 365 + leaps;
}

/*
 * Reads count ASCII digits at text into *value.  Refuses any other
 * character.
 */
static int
der_read_digits(
	const uint8_t *text,
	size_t count,
	unsigned *value)
{
	size_t index;
	unsigned number;

	/* Accumulates the decimal digits. */
	number = 0;
	for (index = 0; index < count; index++) {
		if (text[index] < (uint8_t)'0' || text[index] > (uint8_t)'9')
			return EINVAL;
		number = number * 10U + (unsigned)(text[index] - (uint8_t)'0');
	}

	/* Publishes the number. */
	*value = number;

	/* Succeeded: every character was a digit. */
	return 0;
}
