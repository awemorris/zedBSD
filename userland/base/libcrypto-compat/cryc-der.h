/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A reader of DER, the Distinguished Encoding Rules of ASN.1, for
 * certificates, public keys, algorithm identifiers and signatures.
 *
 * Strict DER only (clause 10 and 11 on top of the BER of clause 8):
 * definite lengths in the minimum number of octets (10.1, 8.1.3.5 note:
 * the short form for lengths below 128), no indefinite length (8.1.3.6
 * is refused), tags in the low-tag-number form for tag numbers below 31
 * and the high form only above (8.1.2.4), primitive encodings for the
 * string types this reader reads (10.2), BOOLEAN TRUE as 0xff (11.1),
 * minimal INTEGERs (8.3.2), BIT STRINGs with unused bits zero (11.2).
 * Anything else is EINVAL.
 *
 * Every element read is a view into the caller's buffer: nothing is
 * copied and nothing is allocated.  The input is untrusted; every length
 * is checked against what is left before it is used, and nesting is
 * bounded by the callers' structure, not by recursion here.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_DER_H
#define LIBCRYPTO_COMPAT_CRYC_DER_H

#include "cryc.h"

/*
 * The clause numbers in the comments of this file are those of ITU-T
 * X.690 (02/2021).
 */

/*
 * The tags (identifier octets, 8.1.2) of the universal types read, as the
 * full first identifier octet including the constructed bit.  Values
 * referenced: ITU-T X.680 (02/2021) 8.4 Table 1 universal class tag
 * assignments, with the class and constructed bits of X.690 8.1.2.
 */
#define CRYC_DER_BOOLEAN		0x01U
#define CRYC_DER_INTEGER		0x02U
#define CRYC_DER_BIT_STRING		0x03U
#define CRYC_DER_OCTET_STRING		0x04U
#define CRYC_DER_NULL			0x05U
#define CRYC_DER_OID			0x06U
#define CRYC_DER_UTF8_STRING		0x0cU
#define CRYC_DER_PRINTABLE_STRING	0x13U
#define CRYC_DER_T61_STRING		0x14U
#define CRYC_DER_IA5_STRING		0x16U
#define CRYC_DER_UTC_TIME		0x17U
#define CRYC_DER_GENERALIZED_TIME	0x18U
#define CRYC_DER_UNIVERSAL_STRING	0x1cU
#define CRYC_DER_BMP_STRING		0x1eU
#define CRYC_DER_SEQUENCE		0x30U
#define CRYC_DER_SET			0x31U

/* The class and constructed bits of an identifier octet (8.1.2.2, 8.1.2.5). */
#define CRYC_DER_CONTEXT		0x80U
#define CRYC_DER_CONSTRUCTED		0x20U

/* The largest length this reader accepts: a certificate chain fits far below 2^24. */
#define CRYC_DER_MAX_LENGTH		0x00ffffffUL

/*
 * A position in a DER encoding.
 *
 * data and size are the bytes still to read (the reader advances data and
 * shrinks size).  The reader owns nothing; the bytes outlive it.
 */
struct cryc_der {
	const uint8_t *data;
	size_t size;
};

/*
 * One element read: its tag (the first identifier octet; tags above 30
 * are refused, X.509 needs none), its contents (a view), and the whole
 * encoding including the identifier and length octets (for hashing a
 * tbsCertificate or comparing a Name).
 */
struct cryc_der_element {
	unsigned tag;
	const uint8_t *contents;
	size_t length;
	const uint8_t *encoding;
	size_t encoding_size;
};

/* Starts a reader over size bytes at data (data may be NULL only when size is 0). */
void cryc_der_init(struct cryc_der *der, const uint8_t *data, size_t size);

/* Returns 1 when nothing is left to read, else 0. */
int cryc_der_empty(const struct cryc_der *der);

/*
 * Reads the next element (identifier, length, contents: 8.1.1) whatever
 * its tag, and advances past it.
 *
 * Returns 0; EINVAL when the encoding is not strict DER (above), a tag
 * number is 31 or more, the length exceeds what is left or
 * CRYC_DER_MAX_LENGTH, or nothing is left.  On an error the reader does
 * not move and *element is zeroed.
 */
int cryc_der_next(struct cryc_der *der, struct cryc_der_element *element);

/*
 * Reads the next element and requires its tag to be tag.
 *
 * Returns 0, or EINVAL as cryc_der_next and when the tag differs (the
 * reader then does not move).
 */
int cryc_der_expect(struct cryc_der *der, unsigned tag, struct cryc_der_element *element);

/*
 * Reads the next element only when its tag is tag (an OPTIONAL or
 * DEFAULT field).
 *
 * Returns 0 with *present 1 and the element read; 0 with *present 0 when
 * nothing is left or the next tag differs (nothing read); EINVAL when the
 * next element with that tag is malformed.
 */
int cryc_der_optional(struct cryc_der *der, unsigned tag, struct cryc_der_element *element, int *present);

/*
 * Starts a reader over the contents of a constructed element (a
 * SEQUENCE, a SET, an explicit tag).
 *
 * Returns 0, or EINVAL when element's tag lacks the constructed bit.
 */
int cryc_der_enter(const struct cryc_der_element *element, struct cryc_der *inner);

/*
 * Checks a DER INTEGER (8.3) and gives its magnitude: the big-endian
 * bytes without the leading 0x00 that makes a positive value whose top
 * bit is set (8.3.2).
 *
 * Returns 0; EINVAL for an empty contents, a non-minimal encoding (8.3.2:
 * the first nine bits all 0 or all 1), or a negative value (top bit set:
 * no X.509 or signature INTEGER read here may be negative).  A value of
 * zero gives one byte 0x00.
 */
int cryc_der_integer(const struct cryc_der_element *element, const uint8_t **magnitude, size_t *size);

/*
 * Reads a small non-negative INTEGER (a version, a pathLenConstraint, a
 * PSS saltLength) into *value.
 *
 * Returns 0, EINVAL as cryc_der_integer, or ERANGE when the value exceeds
 * max.
 */
int cryc_der_small_integer(const struct cryc_der_element *element, uint32_t max, uint32_t *value);

/*
 * Reads a BOOLEAN (8.2, 11.1).  Returns 0 or EINVAL (length other than 1,
 * or a value other than 0x00 and 0xff).
 */
int cryc_der_boolean(const struct cryc_der_element *element, int *value);

/*
 * Gives the bits of a BIT STRING (8.6, 11.2): bytes and the count of
 * unused bits in the last byte (0 to 7).
 *
 * Returns 0; EINVAL for an empty contents, an unused-bit count above 7,
 * unused bits with no bytes, or unused bits not zero (11.2.1).
 */
int cryc_der_bit_string(const struct cryc_der_element *element, const uint8_t **bytes, size_t *size, unsigned *unused);

/*
 * Checks that an OID's contents (8.19) are well formed: at least one
 * byte, each subidentifier in the fewest octets (no leading 0x80, 8.19.2),
 * the last byte with its top bit clear.  Returns 0 or EINVAL.  OIDs are
 * compared by their contents bytes against the constants of the callers.
 */
int cryc_der_oid(const struct cryc_der_element *element);

/*
 * Converts a UTCTime or GeneralizedTime as RFC 5280 4.1.2.5 restricts
 * them (YYMMDDHHMMSSZ, with YY 50..99 meaning 19YY and 00..49 meaning
 * 20YY; YYYYMMDDHHMMSSZ; no fractions, always Z) into seconds since
 * 1970-01-01T00:00:00Z.
 *
 * Fields are checked (month 1..12, day within the month with leap years,
 * hour 0..23, minute and second 0..59; a leap second is refused).
 * GeneralizedTime may name years 0000 to 9999 (results before 1970 are
 * negative).  Returns 0 or EINVAL (another tag, another form, a field out
 * of range).
 */
int cryc_der_time(const struct cryc_der_element *element, int64_t *seconds);

#endif
