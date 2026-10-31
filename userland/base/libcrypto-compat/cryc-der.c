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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(der);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
}

/*
 * Returns 1 when nothing is left to read, else 0.
 */
int
cryc_der_empty(
	const struct cryc_der *der)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(der);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Reads the next element (identifier, length, contents: 8.1.1) whatever
 * its tag, and advances past it.
 */
int
cryc_der_next(
	struct cryc_der *der,
	struct cryc_der_element *element)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(der);
	UNUSED_PARAMETER(element);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(der);
	UNUSED_PARAMETER(tag);
	UNUSED_PARAMETER(element);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(der);
	UNUSED_PARAMETER(tag);
	UNUSED_PARAMETER(element);
	UNUSED_PARAMETER(present);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(element);
	UNUSED_PARAMETER(inner);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Checks a DER INTEGER (8.3) and gives its magnitude: the big-endian bytes
 * without the leading 0x00 that makes a positive value whose top bit is
 * set (8.3.2).
 */
int
cryc_der_integer(
	const struct cryc_der_element *element,
	const uint8_t **magnitude,
	size_t *size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(element);
	UNUSED_PARAMETER(magnitude);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(element);
	UNUSED_PARAMETER(max);
	UNUSED_PARAMETER(value);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Reads a BOOLEAN (8.2, 11.1).
 */
int
cryc_der_boolean(
	const struct cryc_der_element *element,
	int *value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(element);
	UNUSED_PARAMETER(value);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(element);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(unused);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(element);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(element);
	UNUSED_PARAMETER(seconds);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
