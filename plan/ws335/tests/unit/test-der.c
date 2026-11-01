/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the DER reader (cryc-der.h).
 *
 * The cases are built by hand from ITU-T X.690 (02/2021) clauses 8.1 to
 * 8.3, 8.6, 8.19, 10.1, 10.2, 11.1 and 11.2 and from RFC 5280 4.1.2.5:
 * every length form and its minimal-octets rule, truncation at every
 * point, the tags the reader refuses, INTEGER, BOOLEAN, BIT STRING and OID
 * contents (the small ones exhaustively), time conversion (hand-computed
 * epochs, an exhaustive day-by-day comparison with a calendar counter
 * written here, every field out of range, every character position), the
 * element views, and the whole reader on random and mutated input under
 * the sanitizers.
 *
 * Three corpora come from outside: the public keys and signatures of the
 * Wycheproof test files (whose flags say which signatures are valid DER,
 * and whose key files give the modulus to compare with), and the
 * certificates of the ca-certificates bundle, which must all walk as X.509
 * Certificates (RFC 5280 4.1) from the first byte to the last.  The
 * expected values never come from another implementation.
 */

#include "check.h"
#include "derwriter.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-der.h"

/* The value of a fill that shows a field was not written. */
#define FILL_BYTE	0xa5U

/* The deepest nesting the random walker follows. */
#define WALK_DEPTH	12U

/* The x509-limbo case whose certificates carry a negative serial number. */
#define NEGATIVE_SERIAL_CASE	"rfc5280::serial::negative"

/* The largest hex-encoded object read from a Wycheproof file, in bytes. */
#define OBJECT_MAX	8192U

/* The largest certificate of the x509-limbo cases is about 78 KB. */
#define LIMBO_MAX	131072U

/* Where the bundle is looked for when WS335_CACERT is not set. */
#define CACERT_DEFAULT	"/home/awe/zedBSD-claude1/build/distfiles/cacert-2026-08-13.pem"

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x2545f491U;

/* The elements the random walker has seen, and the views it found outside the input. */
static unsigned long walk_elements;
static unsigned long walk_bad;

/* Returns the next pseudo-random 32-bit value. */
static uint32_t
random32(void)
{
	/* Marsaglia's xorshift32 with the shifts 13, 17, 5. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Fills an element with a recognisable pattern, so that a missing write shows. */
static void
element_fill(
	struct cryc_der_element *element)
{
	memset(element, FILL_BYTE, sizeof(*element));
}

/* Returns 1 when the element is all zero (what a failed read leaves). */
static int
element_is_zero(
	const struct cryc_der_element *element)
{
	if (element->tag != 0U || element->contents != NULL || element->length != 0U)
		return 0;
	if (element->encoding != NULL || element->encoding_size != 0U)
		return 0;
	return 1;
}

/* Reads one element from bytes as a whole reader; returns the error and the reader's remaining size. */
static int
read_one(
	const unsigned char *bytes,
	size_t size,
	struct cryc_der_element *element,
	size_t *left)
{
	struct cryc_der der;
	int error;

	/* One read on a fresh reader. */
	cryc_der_init(&der, bytes, size);
	element_fill(element);
	error = cryc_der_next(&der, element);
	*left = der.size;
	return error;
}

/* Checks a hex-encoded element: the error, and for success the tag, the length and the remaining bytes. */
static void
check_hex_next(
	const char *hex,
	int expected_error,
	unsigned expected_tag,
	size_t expected_length,
	size_t expected_left)
{
	unsigned char bytes[64];
	struct cryc_der_element element;
	size_t size;
	size_t left;
	int error;

	/* Reads the one element. */
	size = hex_decode(hex, bytes, sizeof(bytes));
	CHECK(size != (size_t)-1);
	error = read_one(bytes, size, &element, &left);
	CHECK(error == expected_error);
	if (expected_error != 0) {
		/* A failed read leaves the reader and zeroes the element. */
		CHECK(left == size);
		CHECK(element_is_zero(&element));
		return;
	}
	CHECK(element.tag == expected_tag);
	CHECK(element.length == expected_length);
	CHECK(left == expected_left);
}

/* Reads every strict prefix of an encoding (an error) and the whole (a success). */
static void
check_truncations(
	const unsigned char *bytes,
	size_t size)
{
	struct cryc_der_element element;
	size_t prefix;
	size_t left;
	int bad;

	/* Every strict prefix of a valid element is an error that leaves the reader alone. */
	bad = 0;
	for (prefix = 0; prefix < size; prefix++) {
		if (read_one(bytes, prefix, &element, &left) != EINVAL)
			bad++;
		if (left != prefix || !element_is_zero(&element))
			bad++;
	}
	CHECK(bad == 0);

	/* The whole is read, consuming everything. */
	CHECK(read_one(bytes, size, &element, &left) == 0);
	CHECK(left == 0U);
}

/* Reads elements with every length form of 8.1.3 and the limits of 10.1. */
static void
test_lengths(void)
{
	static const size_t lengths[] = {
		0, 1, 2, 126, 127, 128, 129, 254, 255, 256, 257, 4095, 65535, 65536, 65537, 1000000, 16777214, 16777215
	};
	unsigned char *buffer;
	unsigned char *source;
	unsigned char small[512];
	unsigned char small_source[512];
	struct cryc_der_element element;
	struct cryc_der der;
	struct der_writer writer;
	size_t index;
	size_t length;
	size_t header;
	size_t left;
	size_t room;

	/* The most that is built: the header, the contents, three bytes after. */
	room = 16777215U + 5U + 3U;
	buffer = malloc(room);
	CHECK(buffer != NULL);
	if (buffer == NULL)
		return;
	source = malloc(16777215U);
	CHECK(source != NULL);
	if (source == NULL) {
		free(buffer);
		return;
	}
	memset(source, 0x5a, 16777215U);
	memset(buffer, 0, room);

	/* Each length: the minimal header (8.1.3.4 short below 128, 8.1.3.5 long otherwise), the views of 8.1.1. */
	for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
		length = lengths[index];
		if (length < 128U)
			header = 2;
		else if (length < 256U)
			header = 3;
		else if (length < 65536U)
			header = 4;
		else
			header = 5;
		der_init(&writer, buffer, room);
		der_put(&writer, 0x04U, source, length);
		CHECK(!writer.error);
		CHECK(writer.used == header + length);

		/* Three bytes of another value follow the element. */
		memset(buffer + header + length, 0x77, 3);
		cryc_der_init(&der, buffer, header + length + 3U);
		element_fill(&element);
		CHECK(cryc_der_next(&der, &element) == 0);
		CHECK(element.tag == CRYC_DER_OCTET_STRING);
		CHECK(element.length == length);
		CHECK(element.contents == buffer + header);
		CHECK(element.encoding == buffer);
		CHECK(element.encoding_size == header + length);
		CHECK(der.data == buffer + header + length);
		CHECK(der.size == 3U);
		CHECK(!cryc_der_empty(&der));
	}

	/* The longest accepted length is 0x00ffffff (done above); one more is refused although the bytes are there. */
	buffer[0] = 0x04;
	buffer[1] = 0x84;
	buffer[2] = 0x01;
	buffer[3] = 0x00;
	buffer[4] = 0x00;
	buffer[5] = 0x00;
	free(buffer);
	free(source);
	buffer = malloc(0x01000000UL + 6U);
	CHECK(buffer != NULL);
	if (buffer == NULL)
		return;
	memset(buffer, 0, 0x01000000UL + 6U);
	buffer[0] = 0x04;
	buffer[1] = 0x84;
	buffer[2] = 0x01;
	CHECK(read_one(buffer, 0x01000000UL + 6U, &element, &left) == EINVAL);
	CHECK(left == 0x01000000UL + 6U);
	CHECK(element_is_zero(&element));

	/* Five length octets, eight, and the reserved 0xff (8.1.3.5 c), with plenty of bytes behind. */
	buffer[1] = 0x85;
	CHECK(read_one(buffer, 64, &element, &left) == EINVAL);
	buffer[1] = 0x88;
	buffer[2] = 0xff;
	CHECK(read_one(buffer, 64, &element, &left) == EINVAL);
	buffer[1] = 0xff;
	CHECK(read_one(buffer, 64, &element, &left) == EINVAL);
	buffer[1] = 0xfe;
	CHECK(read_one(buffer, 64, &element, &left) == EINVAL);
	free(buffer);

	/* Truncation at every point, for a short, a long-by-one-octet and a long-by-two-octets length. */
	memset(small, 0x33, sizeof(small));
	memset(small_source, 0x33, sizeof(small_source));
	der_init(&writer, small, sizeof(small));
	der_put(&writer, 0x04U, small_source, 0);
	check_truncations(small, writer.used);
	der_init(&writer, small, sizeof(small));
	der_put(&writer, 0x04U, small_source, 127);
	check_truncations(small, writer.used);
	der_init(&writer, small, sizeof(small));
	der_put(&writer, 0x04U, small_source, 128);
	check_truncations(small, writer.used);
	der_init(&writer, small, sizeof(small));
	der_put(&writer, 0x04U, small_source, 300);
	check_truncations(small, writer.used);
}

/* The hand-written header cases: forms DER forbids and the edges of the ones it allows. */
static void
test_headers(void)
{
	/* The empty input and an identifier with no length. */
	check_hex_next("", EINVAL, 0, 0, 0);
	check_hex_next("04", EINVAL, 0, 0, 0);
	check_hex_next("30", EINVAL, 0, 0, 0);

	/* The short form, with bytes after the element. */
	check_hex_next("0400", 0, 0x04, 0, 0);
	check_hex_next("0400aabb", 0, 0x04, 0, 2);
	check_hex_next("0402aabbcc", 0, 0x04, 2, 1);

	/* The indefinite form (8.1.3.6) is refused, with or without an end-of-contents (10.1). */
	check_hex_next("3080", EINVAL, 0, 0, 0);
	check_hex_next("30800000", EINVAL, 0, 0, 0);
	check_hex_next("3080020101000000", EINVAL, 0, 0, 0);

	/* The long form for a length below 128 (10.1), at every octet count, with the bytes present. */
	check_hex_next("048100", EINVAL, 0, 0, 0);
	check_hex_next("04810100", EINVAL, 0, 0, 0);
	check_hex_next("04817f", EINVAL, 0, 0, 0);
	check_hex_next("0482000500", EINVAL, 0, 0, 0);
	check_hex_next("048300000500", EINVAL, 0, 0, 0);
	check_hex_next("04840000000500", EINVAL, 0, 0, 0);

	/* Leading zero octets in a length of 128 or more (10.1). */
	check_hex_next("04820080", EINVAL, 0, 0, 0);
	check_hex_next("0483000080", EINVAL, 0, 0, 0);
	check_hex_next("0483000100", EINVAL, 0, 0, 0);

	/* A length that exceeds what is left, by one and by a lot. */
	check_hex_next("040100", 0, 0x04, 1, 0);
	check_hex_next("0402aa", EINVAL, 0, 0, 0);
	check_hex_next("047f", EINVAL, 0, 0, 0);
	check_hex_next("04ffff", EINVAL, 0, 0, 0);
	check_hex_next("0484ffffffff", EINVAL, 0, 0, 0);
	check_hex_next("0484000000ff", EINVAL, 0, 0, 0);

	/* Long-form octets cut off by the end of the input. */
	check_hex_next("0481", EINVAL, 0, 0, 0);
	check_hex_next("048201", EINVAL, 0, 0, 0);
	check_hex_next("04830100", EINVAL, 0, 0, 0);

	/* Context, application and private tags, constructed and primitive, and the universal ones named. */
	check_hex_next("a000", 0, 0xa0, 0, 0);
	check_hex_next("a1020500", 0, 0xa1, 2, 0);
	check_hex_next("8001ff", 0, 0x80, 1, 0);
	check_hex_next("81020000", 0, 0x81, 2, 0);
	check_hex_next("3000", 0, 0x30, 0, 0);
	check_hex_next("3100", 0, 0x31, 0, 0);
	check_hex_next("0500", 0, 0x05, 0, 0);
	check_hex_next("0101ff", 0, 0x01, 1, 0);
	check_hex_next("4001aa", 0, 0x40, 1, 0);
	check_hex_next("c001aa", 0, 0xc0, 1, 0);
	check_hex_next("6001aa", 0, 0x60, 1, 0);
	check_hex_next("1e00", 0, 0x1e, 0, 0);

	/* Tag number 31 or more (the high-tag-number form, 8.1.2.4) in every class and form. */
	check_hex_next("1f0100", EINVAL, 0, 0, 0);
	check_hex_next("3f0100", EINVAL, 0, 0, 0);
	check_hex_next("5f0100", EINVAL, 0, 0, 0);
	check_hex_next("7f0100", EINVAL, 0, 0, 0);
	check_hex_next("9f0100", EINVAL, 0, 0, 0);
	check_hex_next("bf0100", EINVAL, 0, 0, 0);
	check_hex_next("df0100", EINVAL, 0, 0, 0);
	check_hex_next("ff0100", EINVAL, 0, 0, 0);
	check_hex_next("9f2100", EINVAL, 0, 0, 0);

	/* Constructed forms of the primitive universal types are refused (8.2.1, 8.3.1, 8.6.2, 8.19.1, 10.2). */
	check_hex_next("2100", EINVAL, 0, 0, 0);
	check_hex_next("2200", EINVAL, 0, 0, 0);
	check_hex_next("2300", EINVAL, 0, 0, 0);
	check_hex_next("2400", EINVAL, 0, 0, 0);
	check_hex_next("2500", EINVAL, 0, 0, 0);
	check_hex_next("2600", EINVAL, 0, 0, 0);
	check_hex_next("2c00", EINVAL, 0, 0, 0);
	check_hex_next("3300", EINVAL, 0, 0, 0);
	check_hex_next("3400", EINVAL, 0, 0, 0);
	check_hex_next("3600", EINVAL, 0, 0, 0);
	check_hex_next("3700", EINVAL, 0, 0, 0);
	check_hex_next("3800", EINVAL, 0, 0, 0);
	check_hex_next("3c00", EINVAL, 0, 0, 0);
	check_hex_next("3e00", EINVAL, 0, 0, 0);
	check_hex_next("2403040100", EINVAL, 0, 0, 0);

	/* A primitive SEQUENCE or SET is refused (8.9.1, 8.11.1). */
	check_hex_next("1000", EINVAL, 0, 0, 0);
	check_hex_next("1100", EINVAL, 0, 0, 0);
	check_hex_next("100100", EINVAL, 0, 0, 0);
}

/* Builds nested structures with the writer and walks them with expect, optional and enter. */
static void
test_walk(void)
{
	unsigned char outer[256];
	unsigned char inner[128];
	unsigned char version[8];
	unsigned char value[3] = { 0x01, 0x02, 0x03 };
	unsigned char two = 2;
	unsigned char five = 5;
	unsigned char truth = 0xff;
	struct der_writer outer_writer;
	struct der_writer inner_writer;
	struct der_writer version_writer;
	struct cryc_der der;
	struct cryc_der body;
	struct cryc_der explicit_body;
	struct cryc_der_element element;
	struct cryc_der_element sequence;
	struct cryc_der_element tagged;
	const uint8_t *magnitude;
	size_t size;
	size_t left;
	uint32_t number;
	int boolean;
	int present;

	/* SEQUENCE { [0] { INTEGER 2 }, INTEGER 5, OCTET STRING 010203, BOOLEAN TRUE } then NULL. */
	der_init(&version_writer, version, sizeof(version));
	der_put(&version_writer, 0x02U, &two, 1);
	der_init(&inner_writer, inner, sizeof(inner));
	der_wrap(&inner_writer, 0xa0U, &version_writer);
	der_put(&inner_writer, 0x02U, &five, 1);
	der_put(&inner_writer, 0x04U, value, 3);
	der_put(&inner_writer, 0x01U, &truth, 1);
	der_init(&outer_writer, outer, sizeof(outer));
	der_wrap(&outer_writer, 0x30U, &inner_writer);
	der_put(&outer_writer, 0x05U, NULL, 0);
	CHECK(!outer_writer.error);

	/* The top level holds the SEQUENCE then the NULL. */
	cryc_der_init(&der, outer, outer_writer.used);
	CHECK(cryc_der_expect(&der, CRYC_DER_SEQUENCE, &sequence) == 0);
	CHECK(sequence.length == inner_writer.used);
	CHECK(sequence.encoding_size == inner_writer.used + 2U);
	CHECK(sequence.encoding == outer);
	CHECK(!cryc_der_empty(&der));

	/* An expectation that fails reads nothing and zeroes the element. */
	element_fill(&element);
	left = der.size;
	CHECK(cryc_der_expect(&der, CRYC_DER_INTEGER, &element) == EINVAL);
	CHECK(der.size == left);
	CHECK(element_is_zero(&element));

	/* The NULL after it, then nothing. */
	CHECK(cryc_der_expect(&der, CRYC_DER_NULL, &element) == 0);
	CHECK(element.length == 0U);
	CHECK(cryc_der_empty(&der));
	element_fill(&element);
	CHECK(cryc_der_expect(&der, CRYC_DER_NULL, &element) == EINVAL);
	CHECK(element_is_zero(&element));
	element_fill(&element);
	CHECK(cryc_der_next(&der, &element) == EINVAL);
	CHECK(element_is_zero(&element));

	/* Enters the SEQUENCE and reads the optional explicit version. */
	CHECK(cryc_der_enter(&sequence, &body) == 0);
	CHECK(body.data == sequence.contents);
	CHECK(body.size == sequence.length);
	CHECK(cryc_der_optional(&body, CRYC_DER_CONTEXT | CRYC_DER_CONSTRUCTED, &tagged, &present) == 0);
	CHECK(present == 1);
	CHECK(tagged.tag == 0xa0U);
	CHECK(cryc_der_enter(&tagged, &explicit_body) == 0);
	CHECK(cryc_der_expect(&explicit_body, CRYC_DER_INTEGER, &element) == 0);
	number = 99;
	CHECK(cryc_der_small_integer(&element, 2, &number) == 0);
	CHECK(number == 2U);
	CHECK(cryc_der_empty(&explicit_body));

	/* An optional field of another tag is absent: nothing is read. */
	left = body.size;
	element_fill(&element);
	present = 7;
	CHECK(cryc_der_optional(&body, 0x81U, &element, &present) == 0);
	CHECK(present == 0);
	CHECK(body.size == left);
	CHECK(element_is_zero(&element));

	/* The serial number, the OCTET STRING, the BOOLEAN. */
	CHECK(cryc_der_expect(&body, CRYC_DER_INTEGER, &element) == 0);
	CHECK(cryc_der_integer(&element, &magnitude, &size) == 0);
	CHECK(size == 1U);
	CHECK(magnitude[0] == 0x05U);
	CHECK(cryc_der_expect(&body, CRYC_DER_OCTET_STRING, &element) == 0);
	CHECK(element.length == 3U);
	CHECK_BYTES(element.contents, value, 3);
	explicit_body.size = 5;
	CHECK(cryc_der_enter(&element, &explicit_body) == EINVAL);
	CHECK(explicit_body.size == 0U);
	CHECK(cryc_der_optional(&body, CRYC_DER_BOOLEAN, &element, &present) == 0);
	CHECK(present == 1);
	boolean = 0;
	CHECK(cryc_der_boolean(&element, &boolean) == 0);
	CHECK(boolean == 1);
	CHECK(cryc_der_empty(&body));

	/* An optional field when nothing is left. */
	present = 7;
	CHECK(cryc_der_optional(&body, CRYC_DER_BOOLEAN, &element, &present) == 0);
	CHECK(present == 0);

	/* The empty reader. */
	cryc_der_init(&der, NULL, 0);
	CHECK(cryc_der_empty(&der));
}

/* An optional field whose tag matches but whose encoding is bad is an error, not an absence. */
static void
test_optional_malformed(void)
{
	static const char *cases[] = {
		"a0", "a081", "a00500", "a0820001", "a080", "a001", "a1ff00", "81ff"
	};
	unsigned char bytes[16];
	struct cryc_der der;
	struct cryc_der_element element;
	size_t index;
	size_t size;
	int present;
	int error;
	unsigned tag;

	/* Each case starts with the tag that is asked for. */
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		size = hex_decode(cases[index], bytes, sizeof(bytes));
		CHECK(size != (size_t)-1);
		tag = bytes[0];
		cryc_der_init(&der, bytes, size);
		element_fill(&element);
		present = 7;
		error = cryc_der_optional(&der, tag, &element, &present);
		CHECK(error == EINVAL);
		CHECK(present == 0);
		CHECK(der.size == size);
		CHECK(element_is_zero(&element));
	}
}

/* Checks one INTEGER given as hex contents: the error, and the magnitude as hex. */
static void
check_integer(
	const char *hex,
	int expected_error,
	const char *expected_magnitude)
{
	unsigned char bytes[64];
	unsigned char want[64];
	struct cryc_der_element element;
	const uint8_t *magnitude;
	size_t size;
	size_t want_size;
	size_t length;
	int error;

	/* The element, as a reader would hand it over. */
	length = hex_decode(hex, bytes, sizeof(bytes));
	CHECK(length != (size_t)-1);
	memset(&element, 0, sizeof(element));
	element.tag = CRYC_DER_INTEGER;
	element.contents = bytes;
	element.length = length;
	magnitude = (const uint8_t *)bytes;
	size = 77;
	error = cryc_der_integer(&element, &magnitude, &size);
	CHECK(error == expected_error);
	if (expected_error != 0) {
		CHECK(magnitude == NULL);
		CHECK(size == 0U);
		return;
	}
	want_size = hex_decode(expected_magnitude, want, sizeof(want));
	CHECK(size == want_size);
	CHECK_BYTES(magnitude, want, want_size);
}

/* INTEGER contents (8.3): minimality, sign, leading zero for the top bit. */
static void
test_integer(void)
{
	unsigned char random_bytes[700];
	unsigned char magnitude_bytes[700];
	unsigned char output[1200];
	struct cryc_der_element element;
	struct cryc_der der;
	struct der_writer writer;
	const uint8_t *magnitude;
	size_t size;
	size_t length;
	size_t round;
	size_t skip;
	size_t index;
	int bad;

	/* Valid: zero, small values, the top bit set with its 0x00, longer values. */
	check_integer("00", 0, "00");
	check_integer("01", 0, "01");
	check_integer("7f", 0, "7f");
	check_integer("0080", 0, "80");
	check_integer("00ff", 0, "ff");
	check_integer("0100", 0, "0100");
	check_integer("7fff", 0, "7fff");
	check_integer("00ffff", 0, "ffff");
	check_integer("008000", 0, "8000");
	check_integer("00ff00ff", 0, "ff00ff");
	check_integer("0100000000", 0, "0100000000");
	check_integer("7f00000000000000", 0, "7f00000000000000");

	/* Empty (8.3.1: at least one octet). */
	check_integer("", EINVAL, "");

	/* Not minimal: the first nine bits all zero (8.3.2). */
	check_integer("0000", EINVAL, "");
	check_integer("0001", EINVAL, "");
	check_integer("007f", EINVAL, "");
	check_integer("000080", EINVAL, "");
	check_integer("00000080", EINVAL, "");
	check_integer("0001000000", EINVAL, "");

	/* Not minimal: the first nine bits all one (8.3.2), and negative values (8.3.3). */
	check_integer("ffff", EINVAL, "");
	check_integer("ff80", EINVAL, "");
	check_integer("ff7f", EINVAL, "");
	check_integer("ff00", EINVAL, "");
	check_integer("80", EINVAL, "");
	check_integer("ff", EINVAL, "");
	check_integer("8000", EINVAL, "");
	check_integer("fe", EINVAL, "");
	check_integer("8000000000", EINVAL, "");

	/* A long run of random values written by the test's writer round-trips (leading zeros stripped there). */
	bad = 0;
	for (round = 0; round < 400U; round++) {
		length = 1U + random32() % 600U;
		for (index = 0; index < length; index++)
			random_bytes[index] = (unsigned char)random32();
		if (round % 4U == 0U)
			memset(random_bytes, 0, length / 2U);
		skip = 0;
		while (skip + 1U < length && random_bytes[skip] == 0U)
			skip++;
		memcpy(magnitude_bytes, random_bytes + skip, length - skip);
		der_init(&writer, output, sizeof(output));
		der_unsigned(&writer, random_bytes, length);
		if (writer.error) {
			bad++;
			continue;
		}
		cryc_der_init(&der, output, writer.used);
		if (cryc_der_expect(&der, CRYC_DER_INTEGER, &element) != 0 || !cryc_der_empty(&der)) {
			bad++;
			continue;
		}
		if (cryc_der_integer(&element, &magnitude, &size) != 0) {
			bad++;
			continue;
		}
		if (size != length - skip || memcmp(magnitude, magnitude_bytes, size) != 0)
			bad++;
	}
	CHECK(bad == 0);
}

/* Checks one small INTEGER given as hex contents: the error and the value. */
static void
check_small(
	const char *hex,
	uint32_t max,
	int expected_error,
	uint32_t expected_value)
{
	unsigned char bytes[16];
	struct cryc_der_element element;
	size_t length;
	uint32_t value;
	int error;

	/* The element. */
	length = hex_decode(hex, bytes, sizeof(bytes));
	CHECK(length != (size_t)-1);
	memset(&element, 0, sizeof(element));
	element.tag = CRYC_DER_INTEGER;
	element.contents = bytes;
	element.length = length;
	value = 0x77777777U;
	error = cryc_der_small_integer(&element, max, &value);
	CHECK(error == expected_error);
	if (expected_error != 0)
		CHECK(value == 0U);
	else
		CHECK(value == expected_value);
}

/* Small INTEGERs: the range checks and every error of cryc_der_integer. */
static void
test_small_integer(void)
{
	/* Values and the maximum, at the edges. */
	check_small("00", 0xffffffffU, 0, 0);
	check_small("01", 0xffffffffU, 0, 1);
	check_small("7f", 0xffffffffU, 0, 127);
	check_small("0080", 0xffffffffU, 0, 128);
	check_small("00ff", 0xffffffffU, 0, 255);
	check_small("0100", 0xffffffffU, 0, 256);
	check_small("00ffff", 0xffffffffU, 0, 65535);
	check_small("7fffffff", 0xffffffffU, 0, 0x7fffffffU);
	check_small("0080000000", 0xffffffffU, 0, 0x80000000U);
	check_small("00ffffffff", 0xffffffffU, 0, 0xffffffffU);

	/* One byte too many for 32 bits is ERANGE, however the maximum is set. */
	check_small("0100000000", 0xffffffffU, ERANGE, 0);
	check_small("7fffffffff", 0xffffffffU, ERANGE, 0);
	check_small("00ffffffffff", 0xffffffffU, ERANGE, 0);
	check_small("0100000000000000000000", 0xffffffffU, ERANGE, 0);

	/* The maximum is inclusive. */
	check_small("02", 2, 0, 2);
	check_small("03", 2, ERANGE, 0);
	check_small("00", 0, 0, 0);
	check_small("01", 0, ERANGE, 0);
	check_small("0080", 127, ERANGE, 0);
	check_small("7f", 127, 0, 127);
	check_small("00ffffffff", 0xfffffffeU, ERANGE, 0);

	/* The errors of the INTEGER itself come first. */
	check_small("", 10, EINVAL, 0);
	check_small("80", 10, EINVAL, 0);
	check_small("ff", 10, EINVAL, 0);
	check_small("0000", 10, EINVAL, 0);
	check_small("007f", 10, EINVAL, 0);
	check_small("ffff", 10, EINVAL, 0);
}

/* Checks one BOOLEAN given as hex contents. */
static void
check_boolean(
	const char *hex,
	int expected_error,
	int expected_value)
{
	unsigned char bytes[8];
	struct cryc_der_element element;
	size_t length;
	int value;
	int error;

	/* The element. */
	length = hex_decode(hex, bytes, sizeof(bytes));
	CHECK(length != (size_t)-1);
	memset(&element, 0, sizeof(element));
	element.tag = CRYC_DER_BOOLEAN;
	element.contents = bytes;
	element.length = length;
	value = 55;
	error = cryc_der_boolean(&element, &value);
	CHECK(error == expected_error);
	if (expected_error != 0)
		CHECK(value == 0);
	else
		CHECK(value == expected_value);
}

/* BOOLEAN (8.2, 11.1): every value of the one octet, and the wrong lengths. */
static void
test_boolean(void)
{
	char hex[3];
	unsigned byte;

	/* FALSE is 00, TRUE is ff, nothing else (11.1). */
	check_boolean("00", 0, 0);
	check_boolean("ff", 0, 1);
	for (byte = 1; byte < 255U; byte++) {
		sprintf(hex, "%02x", byte);
		check_boolean(hex, EINVAL, 0);
	}

	/* Lengths other than one (8.2.1). */
	check_boolean("", EINVAL, 0);
	check_boolean("0000", EINVAL, 0);
	check_boolean("ffff", EINVAL, 0);
	check_boolean("00ff", EINVAL, 0);
}

/* Checks one BIT STRING given as hex contents: the error, the bytes and the unused count. */
static void
check_bit_string(
	const char *hex,
	int expected_error,
	const char *expected_bytes,
	unsigned expected_unused)
{
	unsigned char bytes[32];
	unsigned char want[32];
	struct cryc_der_element element;
	const uint8_t *got;
	size_t length;
	size_t got_size;
	size_t want_size;
	unsigned unused;
	int error;

	/* The element. */
	length = hex_decode(hex, bytes, sizeof(bytes));
	CHECK(length != (size_t)-1);
	memset(&element, 0, sizeof(element));
	element.tag = CRYC_DER_BIT_STRING;
	element.contents = bytes;
	element.length = length;
	got = bytes;
	got_size = 77;
	unused = 77;
	error = cryc_der_bit_string(&element, &got, &got_size, &unused);
	CHECK(error == expected_error);
	if (expected_error != 0) {
		CHECK(got == NULL);
		CHECK(got_size == 0U);
		CHECK(unused == 0U);
		return;
	}
	want_size = hex_decode(expected_bytes, want, sizeof(want));
	CHECK(got_size == want_size);
	CHECK(got == bytes + 1);
	CHECK_BYTES(got, want, want_size);
	CHECK(unused == expected_unused);
}

/* BIT STRING (8.6, 11.2): the unused-bit rules, exhaustively for one byte of data. */
static void
test_bit_string(void)
{
	unsigned char bytes[3];
	struct cryc_der_element element;
	const uint8_t *got;
	size_t got_size;
	unsigned unused_count;
	unsigned last;
	unsigned unused;
	unsigned modulus;
	int expected;
	int error;
	int bad;

	/* The empty string of bits, whole bytes, and unused bits that are zero. */
	check_bit_string("00", 0, "", 0);
	check_bit_string("0004", 0, "04", 0);
	check_bit_string("00ffff", 0, "ffff", 0);
	check_bit_string("03a8", 0, "a8", 3);
	check_bit_string("0780", 0, "80", 7);
	check_bit_string("01fe", 0, "fe", 1);
	check_bit_string("0400f0", 0, "00f0", 4);
	check_bit_string("0700", 0, "00", 7);

	/* An empty contents has no initial octet (8.6.2.2). */
	check_bit_string("", EINVAL, "", 0);

	/* More than seven unused bits (8.6.2.2). */
	check_bit_string("0800", EINVAL, "", 0);
	check_bit_string("09ff00", EINVAL, "", 0);
	check_bit_string("ff00", EINVAL, "", 0);
	check_bit_string("80ff", EINVAL, "", 0);

	/* Unused bits with no bytes (8.6.2.3). */
	check_bit_string("01", EINVAL, "", 0);
	check_bit_string("07", EINVAL, "", 0);

	/* Unused bits that are not zero (11.2.1). */
	check_bit_string("03a9", EINVAL, "", 0);
	check_bit_string("0781", EINVAL, "", 0);
	check_bit_string("01ff", EINVAL, "", 0);
	check_bit_string("0400f1", EINVAL, "", 0);

	/* Every unused count and last byte: accepted exactly when the last byte is a multiple of 2^unused (11.2.1). */
	bad = 0;
	memset(&element, 0, sizeof(element));
	element.tag = CRYC_DER_BIT_STRING;
	element.contents = bytes;
	element.length = 3;
	bytes[1] = 0xc3;
	for (unused = 0; unused < 8U; unused++) {
		for (last = 0; last < 256U; last++) {
			bytes[0] = (unsigned char)unused;
			bytes[2] = (unsigned char)last;
			modulus = 1U << unused;
			expected = last % modulus == 0U;
			unused_count = 99;
			got = NULL;
			got_size = 0;
			error = cryc_der_bit_string(&element, &got, &got_size, &unused_count);
			if (expected && (error != 0 || got != bytes + 1 || got_size != 2U || unused_count != unused))
				bad++;
			if (!expected && (error != EINVAL || got != NULL || got_size != 0U || unused_count != 0U))
				bad++;
		}
	}
	CHECK(bad == 0);
}

/* Returns the expected verdict for OID contents of the given bytes (X.690 8.19.2, written out from the rules). */
static int
oid_expected(
	const unsigned char *bytes,
	size_t size)
{
	size_t index;
	int starting;

	/* At least one octet. */
	if (size == 0U)
		return EINVAL;

	/* The last octet has bit 8 clear (8.19.2: bit 8 of each octet is 1 except the last of a subidentifier). */
	if (bytes[size - 1U] >= 0x80U)
		return EINVAL;

	/* No subidentifier starts with the octet 0x80 (8.19.2: the first octet shall not be 0x80). */
	starting = 1;
	for (index = 0; index < size; index++) {
		if (starting && bytes[index] == 0x80U)
			return EINVAL;
		starting = bytes[index] < 0x80U;
	}
	return 0;
}

/* OBJECT IDENTIFIER contents (8.19): the real OIDs, then every one- and two-octet contents, then random ones. */
static void
test_oid(void)
{
	static const char *good[] = {
		"2a864886f70d01010b", "550403", "2a8648ce3d0201", "2b6570", "2b6f0203", "00", "7f", "2a86808001"
	};
	static const char *bad_hex[] = {
		"", "80", "2a86", "2a8001", "8001", "2a80", "ff", "2a86f7", "2a808080"
	};
	unsigned char bytes[16];
	unsigned char state[16];
	struct cryc_der_element element;
	size_t index;
	size_t size;
	unsigned first;
	unsigned second;
	unsigned third;
	int mismatch;

	/* Well-formed OIDs: sha256WithRSAEncryption, commonName, id-ecPublicKey, id-Ed25519 and others. */
	for (index = 0; index < sizeof(good) / sizeof(good[0]); index++) {
		size = hex_decode(good[index], bytes, sizeof(bytes));
		memset(&element, 0, sizeof(element));
		element.tag = CRYC_DER_OID;
		element.contents = bytes;
		element.length = size;
		CHECK(cryc_der_oid(&element) == 0);
	}

	/* Malformed ones. */
	for (index = 0; index < sizeof(bad_hex) / sizeof(bad_hex[0]); index++) {
		size = hex_decode(bad_hex[index], bytes, sizeof(bytes));
		memset(&element, 0, sizeof(element));
		element.tag = CRYC_DER_OID;
		element.contents = bytes;
		element.length = size;
		CHECK(cryc_der_oid(&element) == EINVAL);
	}

	/* Every one-, two- and three-octet contents against the rules. */
	mismatch = 0;
	memset(&element, 0, sizeof(element));
	element.tag = CRYC_DER_OID;
	element.contents = state;
	for (first = 0; first < 256U; first++) {
		state[0] = (unsigned char)first;
		element.length = 1;
		if (cryc_der_oid(&element) != oid_expected(state, 1))
			mismatch++;
		for (second = 0; second < 256U; second++) {
			state[1] = (unsigned char)second;
			element.length = 2;
			if (cryc_der_oid(&element) != oid_expected(state, 2))
				mismatch++;
			for (third = 0; third < 256U; third += 1U) {
				state[2] = (unsigned char)third;
				element.length = 3;
				if (cryc_der_oid(&element) != oid_expected(state, 3))
					mismatch++;
			}
		}
	}
	CHECK(mismatch == 0);

	/* Random longer contents. */
	mismatch = 0;
	for (index = 0; index < 20000U; index++) {
		size = 1U + random32() % 15U;
		for (first = 0; first < size; first++)
			state[first] = (unsigned char)((random32() & 4U) != 0U ? random32() : (random32() & 0x80U ? 0x80U : 0x01U));
		element.length = size;
		if (cryc_der_oid(&element) != oid_expected(state, size))
			mismatch++;
	}
	CHECK(mismatch == 0);
}

/* Checks one time: the tag and text, the error, and the seconds. */
static void
check_time(
	unsigned tag,
	const char *text,
	int expected_error,
	int64_t expected_seconds)
{
	struct cryc_der_element element;
	int64_t seconds;
	int error;

	/* The element. */
	memset(&element, 0, sizeof(element));
	element.tag = tag;
	element.contents = (const uint8_t *)text;
	element.length = strlen(text);
	seconds = 12345;
	error = cryc_der_time(&element, &seconds);
	CHECK(error == expected_error);
	if (expected_error != 0)
		CHECK(seconds == 0);
	else if (!CHECK(seconds == expected_seconds))
		printf("  %s -> %lld, expected %lld\n", text, (long long)seconds, (long long)expected_seconds);
}

/* Hand-computed epochs (days between 1970-01-01 and the date, times 86400, plus the time of day). */
static void
test_time_values(void)
{
	/* The epoch itself, and the second before and after. */
	check_time(CRYC_DER_UTC_TIME, "700101000000Z", 0, 0);
	check_time(CRYC_DER_GENERALIZED_TIME, "19700101000000Z", 0, 0);
	check_time(CRYC_DER_GENERALIZED_TIME, "19691231235959Z", 0, -1);
	check_time(CRYC_DER_GENERALIZED_TIME, "19700101000001Z", 0, 1);

	/* The UTCTime pivot (RFC 5280 4.1.2.5): 50 is 1950, 49 is 2049, 99 is 1999, 00 is 2000. */
	check_time(CRYC_DER_UTC_TIME, "500101000000Z", 0, -631152000);
	check_time(CRYC_DER_UTC_TIME, "491231235959Z", 0, 2524607999LL);
	check_time(CRYC_DER_UTC_TIME, "991231235959Z", 0, 946684799);
	check_time(CRYC_DER_UTC_TIME, "000101000000Z", 0, 946684800);
	check_time(CRYC_DER_UTC_TIME, "000229000000Z", 0, 951782400);
	check_time(CRYC_DER_UTC_TIME, "000301000000Z", 0, 951868800);
	check_time(CRYC_DER_UTC_TIME, "380119031407Z", 0, 2147483647);

	/* GeneralizedTime at the ends of its range and at leap-year edges. */
	check_time(CRYC_DER_GENERALIZED_TIME, "00000101000000Z", 0, -62167219200LL);
	check_time(CRYC_DER_GENERALIZED_TIME, "00000229000000Z", 0, -62162121600LL);
	check_time(CRYC_DER_GENERALIZED_TIME, "00000301000000Z", 0, -62162035200LL);
	check_time(CRYC_DER_GENERALIZED_TIME, "18991231235959Z", 0, -2208988801LL);
	check_time(CRYC_DER_GENERALIZED_TIME, "19000101000000Z", 0, -2208988800LL);
	check_time(CRYC_DER_GENERALIZED_TIME, "19000301000000Z", 0, -2203891200LL);
	check_time(CRYC_DER_GENERALIZED_TIME, "20000229000000Z", 0, 951782400);
	check_time(CRYC_DER_GENERALIZED_TIME, "21000301000000Z", 0, 4107542400LL);
	check_time(CRYC_DER_GENERALIZED_TIME, "20380119031407Z", 0, 2147483647);
	check_time(CRYC_DER_GENERALIZED_TIME, "20380119031408Z", 0, 2147483648LL);
	check_time(CRYC_DER_GENERALIZED_TIME, "99991231235959Z", 0, 253402300799LL);
	check_time(CRYC_DER_GENERALIZED_TIME, "20000101123456Z", 0, 946730096);
}

/* Builds YYYYMMDDHHMMSSZ (or YYMMDDHHMMSSZ) text for a date counter and a time of day derived from the counter. */
static void
time_text(
	char *text,
	int wide,
	long year,
	int month,
	int day,
	long counter)
{
	int hour;
	int minute;
	int second;

	/* A time of day that varies from one day to the next. */
	hour = (int)(counter % 24L);
	minute = (int)((counter * 7L) % 60L);
	second = (int)((counter * 13L) % 60L);
	if (hour < 0)
		hour += 24;
	if (minute < 0)
		minute += 60;
	if (second < 0)
		second += 60;
	if (wide)
		sprintf(text, "%04ld%02d%02d%02d%02d%02dZ", year, month, day, hour, minute, second);
	else
		sprintf(text, "%02ld%02d%02d%02d%02d%02dZ", year % 100L, month, day, hour, minute, second);
}

/* Returns the days of a month by the Gregorian rule written out here. */
static int
month_days(
	long year,
	int month)
{
	static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int leap;

	/* A leap year: divisible by 4, except centuries that are not divisible by 400. */
	leap = 0;
	if (year % 4L == 0L && (year % 100L != 0L || year % 400L == 0L))
		leap = 1;
	if (month == 2 && leap)
		return 29;
	return days[month - 1];
}

/* Converts every day of 0000-01-01 .. 9999-12-31 by a calendar counter that walks away from the epoch. */
static void
test_time_every_day(void)
{
	char text[32];
	struct cryc_der_element element;
	int64_t seconds;
	int64_t expected;
	long counter;
	long year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
	unsigned long mismatches;
	unsigned long count;

	/* Forward from 1970-01-01 (counter 0) to 9999-12-31, both forms where the form can name the year. */
	mismatches = 0;
	count = 0;
	year = 1970;
	month = 1;
	day = 1;
	counter = 0;
	memset(&element, 0, sizeof(element));
	element.contents = (const uint8_t *)text;
	while (year <= 9999L) {
		time_text(text, 1, year, month, day, counter);
		hour = (int)(counter % 24L);
		minute = (int)((counter * 7L) % 60L);
		second = (int)((counter * 13L) % 60L);
		expected = (int64_t)counter * 86400 + hour * 3600 + minute * 60 + second;
		element.tag = CRYC_DER_GENERALIZED_TIME;
		element.length = 15;
		if (cryc_der_time(&element, &seconds) != 0 || seconds != expected)
			mismatches++;
		if (year >= 1970L && year <= 2049L) {
			time_text(text, 0, year, month, day, counter);
			element.tag = CRYC_DER_UTC_TIME;
			element.length = 13;
			if (cryc_der_time(&element, &seconds) != 0 || seconds != expected)
				mismatches++;
		}
		count++;

		/* The next day. */
		counter++;
		day++;
		if (day > month_days(year, month)) {
			day = 1;
			month++;
			if (month > 12) {
				month = 1;
				year++;
			}
		}
	}

	/* Backward from 1969-12-31 (counter -1) to 0000-01-01, and the UTCTime years 1950 to 1969. */
	year = 1969;
	month = 12;
	day = 31;
	counter = -1;
	while (year >= 0L) {
		time_text(text, 1, year, month, day, counter);
		hour = (int)(counter % 24L);
		minute = (int)((counter * 7L) % 60L);
		second = (int)((counter * 13L) % 60L);
		if (hour < 0)
			hour += 24;
		if (minute < 0)
			minute += 60;
		if (second < 0)
			second += 60;
		expected = (int64_t)counter * 86400 + hour * 3600 + minute * 60 + second;
		element.tag = CRYC_DER_GENERALIZED_TIME;
		element.length = 15;
		if (cryc_der_time(&element, &seconds) != 0 || seconds != expected)
			mismatches++;
		if (year >= 1950L) {
			time_text(text, 0, year, month, day, counter);
			element.tag = CRYC_DER_UTC_TIME;
			element.length = 13;
			if (cryc_der_time(&element, &seconds) != 0 || seconds != expected)
				mismatches++;
		}
		count++;

		/* The previous day. */
		counter--;
		day--;
		if (day < 1) {
			month--;
			if (month < 1) {
				month = 12;
				year--;
			}
			day = month_days(year, month);
		}
	}

	/* The days walked: 0000-01-01 to 9999-12-31 inclusive is 3652425 days; both ends were reached. */
	CHECK(count == 3652425UL);
	CHECK(year == -1L);
	CHECK(mismatches == 0UL);
}

/* Every field out of its range, every character position, the lengths and the forms RFC 5280 forbids. */
static void
test_time_errors(void)
{
	static const char *wide_bad[] = {
		"20250001000000Z", "20251301000000Z", "20250100000000Z", "20250132000000Z", "20250229000000Z",
		"19000229000000Z", "21000229000000Z", "20250431000000Z", "20250631000000Z", "20250931000000Z",
		"20251131000000Z", "20250230000000Z", "20250101240000Z", "20250101006000Z", "20250101000060Z",
		"20250101000061Z", "20250101250000Z", "20250101009900Z", "20250101000099Z", "20261231235960Z",
		"20250101000000z", "20250101000000", "2025010100000Z", "202501010000000Z", "20250101000000.0Z",
		"20250101000000.Z", "20250101000000+0000", "20250101000000-0500", "202501010000Z", "2025010100Z",
		"20250101000000ZZ", "2025-01-01T00:00Z", "Z", "", "20250101000000Y"
	};
	static const char *utc_bad[] = {
		"250001000000Z", "251301000000Z", "250100000000Z", "250132000000Z", "250229000000Z", "250431000000Z",
		"250101240000Z", "250101006000Z", "250101000060Z", "250101000000z", "250101000000", "25010100000Z",
		"2501010000000Z", "250101000000+0000", "250101000000-0500", "2501010000Z", "250101000000.0Z", "Z", "",
		"20250101000000Z"
	};
	char text[32];
	unsigned long index;
	size_t position;
	size_t length;
	int bad;
	int error;
	struct cryc_der_element element;
	int64_t seconds;
	const char *substitutes = "x +-:/.Z \t";
	size_t which;

	/* The lists of bad forms. */
	for (index = 0; index < sizeof(wide_bad) / sizeof(wide_bad[0]); index++)
		check_time(CRYC_DER_GENERALIZED_TIME, wide_bad[index], EINVAL, 0);
	for (index = 0; index < sizeof(utc_bad) / sizeof(utc_bad[0]); index++)
		check_time(CRYC_DER_UTC_TIME, utc_bad[index], EINVAL, 0);

	/* A time under the other tag's length, and under tags that are not times. */
	check_time(CRYC_DER_UTC_TIME, "20250101000000Z", EINVAL, 0);
	check_time(CRYC_DER_GENERALIZED_TIME, "250101000000Z", EINVAL, 0);
	check_time(CRYC_DER_INTEGER, "250101000000Z", EINVAL, 0);
	check_time(CRYC_DER_IA5_STRING, "250101000000Z", EINVAL, 0);
	check_time(CRYC_DER_OCTET_STRING, "20250101000000Z", EINVAL, 0);
	check_time(0, "250101000000Z", EINVAL, 0);
	check_time(0x37U, "250101000000Z", EINVAL, 0);

	/* A character that is not a digit in every digit position, of both forms. */
	bad = 0;
	memset(&element, 0, sizeof(element));
	element.contents = (const uint8_t *)text;
	for (which = 0; which < strlen(substitutes); which++) {
		for (position = 0; position < 14U; position++) {
			strcpy(text, "20250101000000Z");
			text[position] = substitutes[which];
			element.tag = CRYC_DER_GENERALIZED_TIME;
			element.length = 15;
			error = cryc_der_time(&element, &seconds);
			if (error != EINVAL)
				bad++;
		}
		for (position = 0; position < 12U; position++) {
			strcpy(text, "250101000000Z");
			text[position] = substitutes[which];
			element.tag = CRYC_DER_UTC_TIME;
			element.length = 13;
			error = cryc_der_time(&element, &seconds);
			if (error != EINVAL)
				bad++;
		}
	}
	CHECK(bad == 0);

	/* The characters next to the digits in ASCII (/ and :) and 0x80 and above in each position. */
	for (position = 0; position < 14U; position++) {
		strcpy(text, "20250101000000Z");
		text[position] = (char)0xb1;
		element.tag = CRYC_DER_GENERALIZED_TIME;
		element.length = 15;
		CHECK(cryc_der_time(&element, &seconds) == EINVAL);
	}

	/* Every length from 0 to 20 other than the right one is refused, whatever the text. */
	bad = 0;
	for (length = 0; length < 21U; length++) {
		memset(text, '1', sizeof(text));
		text[length] = 'Z';
		element.tag = CRYC_DER_UTC_TIME;
		element.length = length;
		if (length != 13U && cryc_der_time(&element, &seconds) != EINVAL)
			bad++;
		element.tag = CRYC_DER_GENERALIZED_TIME;
		if (length != 15U && cryc_der_time(&element, &seconds) != EINVAL)
			bad++;
	}
	CHECK(bad == 0);
}

/* Walks the elements of a structure recursively, checking that every view lies inside the input. */
static void
walk(
	struct cryc_der der,
	const uint8_t *base,
	size_t base_size,
	unsigned depth)
{
	struct cryc_der_element element;
	struct cryc_der inner;
	const uint8_t *magnitude;
	const uint8_t *bytes;
	const uint8_t *end;
	size_t size;
	unsigned unused;
	int64_t seconds;
	int boolean;
	uint32_t small;
	int moved;

	/* Reads until the reader fails or is empty; every success moves the reader forward. */
	end = base + base_size;
	for (;;) {
		moved = (int)der.size;
		if (cryc_der_next(&der, &element) != 0)
			break;
		walk_elements++;
		if ((int)der.size >= moved)
			walk_bad++;
		if (element.encoding < base || element.encoding + element.encoding_size > end)
			walk_bad++;
		if (element.contents < element.encoding || element.contents + element.length > element.encoding + element.encoding_size)
			walk_bad++;
		if (element.encoding_size < element.length + 2U)
			walk_bad++;

		/* The content readers answer anything, and what they hand back lies inside the contents. */
		if (cryc_der_integer(&element, &magnitude, &size) == 0) {
			if (magnitude < element.contents || magnitude + size > element.contents + element.length || size == 0U)
				walk_bad++;
		}
		(void)cryc_der_small_integer(&element, 100, &small);
		(void)cryc_der_boolean(&element, &boolean);
		if (cryc_der_bit_string(&element, &bytes, &size, &unused) == 0) {
			if (bytes < element.contents || bytes + size > element.contents + element.length || unused > 7U)
				walk_bad++;
		}
		(void)cryc_der_oid(&element);
		(void)cryc_der_time(&element, &seconds);

		/* Goes inside constructed elements. */
		if (cryc_der_enter(&element, &inner) == 0 && depth < WALK_DEPTH)
			walk(inner, base, base_size, depth + 1U);
	}
}

/* Runs the whole reader on random and mutated inputs; the sanitizers watch the reads. */
static void
test_random(
	const unsigned char *seed,
	size_t seed_size)
{
	static const unsigned char alphabet[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x30, 0x31, 0x80, 0x81, 0x82, 0x83, 0x84, 0xa0, 0xa3, 0xff, 0x7f, 0x17, 0x18
	};
	unsigned char bytes[96];
	unsigned char *copy;
	struct cryc_der der;
	size_t index;
	size_t round;
	size_t size;
	size_t flips;

	/* Random bytes drawn from a set that hits the interesting values, and from the whole range. */
	for (round = 0; round < 30000U; round++) {
		size = random32() % (sizeof(bytes) + 1U);
		for (index = 0; index < size; index++) {
			if (round % 3U == 0U)
				bytes[index] = (unsigned char)random32();
			else
				bytes[index] = alphabet[random32() % sizeof(alphabet)];
		}
		cryc_der_init(&der, bytes, size);
		walk(der, bytes, size, 0);
	}

	/* Well-formed structures built with the writer are walked fully (every element is seen). */
	CHECK(walk_bad == 0UL);

	/* A real certificate, mutated: bytes flipped, truncated, with an extra length octet. */
	copy = malloc(seed_size + 8U);
	CHECK(copy != NULL);
	if (copy == NULL)
		return;
	for (round = 0; round < 4000U; round++) {
		memcpy(copy, seed, seed_size);
		size = seed_size;
		flips = 1U + random32() % 3U;
		for (index = 0; index < flips; index++)
			copy[random32() % size] = (unsigned char)random32();
		if (round % 4U == 1U)
			size = random32() % (seed_size + 1U);
		if (round % 4U == 2U)
			copy[random32() % 12U] = alphabet[random32() % sizeof(alphabet)];
		cryc_der_init(&der, copy, size);
		walk(der, copy, size, 0);
	}
	free(copy);
	CHECK(walk_bad == 0UL);
	CHECK(walk_elements > 100000UL);
}

/* Reads a SEQUENCE { SEQUENCE { OID, ... }, ... } style item's first OID check: returns 0 when well formed. */
static int
read_algorithm(
	struct cryc_der *parent)
{
	struct cryc_der_element sequence;
	struct cryc_der_element oid;
	struct cryc_der body;
	struct cryc_der_element rest;
	int error;

	/* AlgorithmIdentifier ::= SEQUENCE { OID, parameters ANY OPTIONAL } (RFC 5280 4.1.1.2). */
	error = cryc_der_expect(parent, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return error;
	error = cryc_der_enter(&sequence, &body);
	if (error != 0)
		return error;
	error = cryc_der_expect(&body, CRYC_DER_OID, &oid);
	if (error != 0)
		return error;
	error = cryc_der_oid(&oid);
	if (error != 0)
		return error;

	/* Whatever parameters follow must themselves be well-formed elements. */
	while (!cryc_der_empty(&body)) {
		error = cryc_der_next(&body, &rest);
		if (error != 0)
			return error;
	}
	return 0;
}

/* Reads a Validity time: UTCTime or GeneralizedTime that converts. */
static int
read_validity_time(
	struct cryc_der *validity)
{
	struct cryc_der_element element;
	int64_t seconds;
	int error;

	/* Either tag, then the conversion. */
	error = cryc_der_next(validity, &element);
	if (error != 0)
		return error;
	error = cryc_der_time(&element, &seconds);
	return error;
}

/* Reads a SubjectPublicKeyInfo and gives its key bits. */
static int
read_spki(
	struct cryc_der *parent,
	const uint8_t **key,
	size_t *key_size)
{
	struct cryc_der_element sequence;
	struct cryc_der_element bits;
	struct cryc_der body;
	unsigned unused;
	int error;

	/* SEQUENCE { AlgorithmIdentifier, BIT STRING } (RFC 5280 4.1). */
	error = cryc_der_expect(parent, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return error;
	error = cryc_der_enter(&sequence, &body);
	if (error != 0)
		return error;
	error = read_algorithm(&body);
	if (error != 0)
		return error;
	error = cryc_der_expect(&body, CRYC_DER_BIT_STRING, &bits);
	if (error != 0)
		return error;
	error = cryc_der_bit_string(&bits, key, key_size, &unused);
	if (error != 0)
		return error;
	if (unused != 0U || !cryc_der_empty(&body))
		return EINVAL;
	return 0;
}

/* Walks one X.509 Certificate down to its parts; returns 0 when it parses strictly to the last byte. */
static int
walk_certificate(
	const unsigned char *bytes,
	size_t size)
{
	struct cryc_der top;
	struct cryc_der certificate;
	struct cryc_der tbs;
	struct cryc_der validity;
	struct cryc_der version;
	struct cryc_der extensions;
	struct cryc_der extension;
	struct cryc_der_element element;
	struct cryc_der_element sequence;
	struct cryc_der_element tagged;
	struct cryc_der_element signature;
	const uint8_t *magnitude;
	const uint8_t *key;
	size_t magnitude_size;
	size_t key_size;
	unsigned unused;
	uint32_t number;
	int present;
	int boolean;
	int error;

	/* Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue } (RFC 5280 4.1). */
	cryc_der_init(&top, bytes, size);
	error = cryc_der_expect(&top, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return error;
	if (!cryc_der_empty(&top))
		return EINVAL;
	error = cryc_der_enter(&sequence, &certificate);
	if (error != 0)
		return error;
	error = cryc_der_expect(&certificate, CRYC_DER_SEQUENCE, &element);
	if (error != 0)
		return error;
	error = cryc_der_enter(&element, &tbs);
	if (error != 0)
		return error;
	error = read_algorithm(&certificate);
	if (error != 0)
		return error;
	error = cryc_der_expect(&certificate, CRYC_DER_BIT_STRING, &signature);
	if (error != 0)
		return error;
	error = cryc_der_bit_string(&signature, &key, &key_size, &unused);
	if (error != 0)
		return error;
	if (!cryc_der_empty(&certificate))
		return EINVAL;

	/* The optional [0] EXPLICIT version, then the serial number. */
	error = cryc_der_optional(&tbs, CRYC_DER_CONTEXT | CRYC_DER_CONSTRUCTED, &tagged, &present);
	if (error != 0)
		return error;
	if (present) {
		error = cryc_der_enter(&tagged, &version);
		if (error != 0)
			return error;
		error = cryc_der_expect(&version, CRYC_DER_INTEGER, &element);
		if (error != 0)
			return error;
		error = cryc_der_small_integer(&element, 2, &number);
		if (error != 0)
			return error;
		if (!cryc_der_empty(&version))
			return EINVAL;
	}
	error = cryc_der_expect(&tbs, CRYC_DER_INTEGER, &element);
	if (error != 0)
		return error;
	error = cryc_der_integer(&element, &magnitude, &magnitude_size);
	if (error != 0)
		return error;

	/* The signature algorithm, the issuer, the validity (two times), the subject, the key. */
	error = read_algorithm(&tbs);
	if (error != 0)
		return error;
	error = cryc_der_expect(&tbs, CRYC_DER_SEQUENCE, &element);
	if (error != 0)
		return error;
	error = cryc_der_expect(&tbs, CRYC_DER_SEQUENCE, &element);
	if (error != 0)
		return error;
	error = cryc_der_enter(&element, &validity);
	if (error != 0)
		return error;
	error = read_validity_time(&validity);
	if (error != 0)
		return error;
	error = read_validity_time(&validity);
	if (error != 0)
		return error;
	if (!cryc_der_empty(&validity))
		return EINVAL;
	error = cryc_der_expect(&tbs, CRYC_DER_SEQUENCE, &element);
	if (error != 0)
		return error;
	error = read_spki(&tbs, &key, &key_size);
	if (error != 0)
		return error;

	/* The optional unique identifiers [1] and [2] (BIT STRINGs), then [3] EXPLICIT extensions. */
	error = cryc_der_optional(&tbs, CRYC_DER_CONTEXT | 1U, &tagged, &present);
	if (error != 0)
		return error;
	error = cryc_der_optional(&tbs, CRYC_DER_CONTEXT | 2U, &tagged, &present);
	if (error != 0)
		return error;
	error = cryc_der_optional(&tbs, CRYC_DER_CONTEXT | CRYC_DER_CONSTRUCTED | 3U, &tagged, &present);
	if (error != 0)
		return error;
	if (present) {
		error = cryc_der_enter(&tagged, &version);
		if (error != 0)
			return error;
		error = cryc_der_expect(&version, CRYC_DER_SEQUENCE, &element);
		if (error != 0)
			return error;
		error = cryc_der_enter(&element, &extensions);
		if (error != 0)
			return error;

		/* Extension ::= SEQUENCE { OID, critical BOOLEAN DEFAULT FALSE, OCTET STRING }. */
		while (!cryc_der_empty(&extensions)) {
			error = cryc_der_expect(&extensions, CRYC_DER_SEQUENCE, &element);
			if (error != 0)
				return error;
			error = cryc_der_enter(&element, &extension);
			if (error != 0)
				return error;
			error = cryc_der_expect(&extension, CRYC_DER_OID, &element);
			if (error != 0)
				return error;
			error = cryc_der_oid(&element);
			if (error != 0)
				return error;
			error = cryc_der_optional(&extension, CRYC_DER_BOOLEAN, &element, &present);
			if (error != 0)
				return error;
			if (present) {
				error = cryc_der_boolean(&element, &boolean);
				if (error != 0)
					return error;
			}
			error = cryc_der_expect(&extension, CRYC_DER_OCTET_STRING, &element);
			if (error != 0)
				return error;
			if (!cryc_der_empty(&extension))
				return EINVAL;
		}
	}
	if (!cryc_der_empty(&tbs))
		return EINVAL;

	/* Succeeded: the certificate parsed to its last byte. */
	return 0;
}

/* Reads the first bytes of a file (all of it) with "rb"; returns a malloc'd buffer or NULL. */
static char *
read_file(
	const char *path,
	size_t *size)
{
	FILE *stream;
	char *data;
	long length;

	/* Binary mode: Windows must not translate line ends. */
	stream = fopen(path, "rb");
	if (stream == NULL)
		return NULL;
	fseek(stream, 0, SEEK_END);
	length = ftell(stream);
	fseek(stream, 0, SEEK_SET);
	if (length < 0) {
		fclose(stream);
		return NULL;
	}
	data = malloc((size_t)length + 1U);
	if (data == NULL) {
		fclose(stream);
		return NULL;
	}
	*size = fread(data, 1, (size_t)length, stream);
	fclose(stream);
	data[*size] = '\0';
	return data;
}

/* Decodes a base64 line group into DER with the reader's sibling module: used only to get the bundle's DER. */
#include "userland/base/libcrypto-compat/cryc-pem.h"

/* Walks every certificate of the ca-certificates bundle as an X.509 Certificate. */
static void
test_bundle(
	unsigned char **first,
	size_t *first_size)
{
	const char *path;
	char *text;
	size_t size;
	size_t position;
	uint8_t *der;
	size_t der_size;
	unsigned long count;
	unsigned long failed;
	unsigned long expected;
	const char *scan;
	int error;

	/* The bundle must be readable: a missing file is a failure, not a pass. */
	path = getenv("WS335_CACERT");
	if (path == NULL)
		path = CACERT_DEFAULT;
	text = read_file(path, &size);
	CHECK(text != NULL);
	if (text == NULL)
		return;

	/* The number of certificates, counted independently of the PEM reader. */
	expected = 0;
	for (scan = strstr(text, "-----BEGIN CERTIFICATE-----"); scan != NULL;
	     scan = strstr(scan + 1, "-----BEGIN CERTIFICATE-----"))
		expected++;
	CHECK(expected > 100UL);

	/* Every block: decoded, then walked. */
	position = 0;
	count = 0;
	failed = 0;
	for (;;) {
		error = cryc_pem_next_certificate(text, size, &position, &der, &der_size);
		if (error == ENOENT)
			break;
		if (error != 0) {
			failed++;
			continue;
		}
		count++;
		if (walk_certificate(der, der_size) != 0)
			failed++;
		if (*first == NULL) {
			*first = der;
			*first_size = der_size;
		} else {
			free(der);
		}
	}
	CHECK(count == expected);
	CHECK(failed == 0UL);
	free(text);
}

/* Copies a vector's hex string into a buffer, returning the size or (size_t)-1. */
static size_t
decode_field(
	const struct vec_file *file,
	const char *key,
	unsigned char *buffer)
{
	return vec_hex(file, key, buffer, OBJECT_MAX);
}

/* Checks a Wycheproof signature: SEQUENCE { INTEGER r, INTEGER s } read strictly to the last byte. */
static int
read_ecdsa_signature(
	const unsigned char *bytes,
	size_t size)
{
	struct cryc_der top;
	struct cryc_der body;
	struct cryc_der_element sequence;
	struct cryc_der_element element;
	const uint8_t *magnitude;
	size_t magnitude_size;
	int error;

	/* The shape of FIPS 186-5 / X9.62 Ecdsa-Sig-Value. */
	cryc_der_init(&top, bytes, size);
	error = cryc_der_expect(&top, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return error;
	if (!cryc_der_empty(&top))
		return EINVAL;
	error = cryc_der_enter(&sequence, &body);
	if (error != 0)
		return error;
	error = cryc_der_expect(&body, CRYC_DER_INTEGER, &element);
	if (error != 0)
		return error;
	error = cryc_der_integer(&element, &magnitude, &magnitude_size);
	if (error != 0)
		return error;
	error = cryc_der_expect(&body, CRYC_DER_INTEGER, &element);
	if (error != 0)
		return error;
	error = cryc_der_integer(&element, &magnitude, &magnitude_size);
	if (error != 0)
		return error;
	if (!cryc_der_empty(&body))
		return EINVAL;
	return 0;
}

/* Returns 1 when the comma-separated flags contain the word. */
static int
has_flag(
	const char *flags,
	const char *word)
{
	size_t length;
	const char *found;

	/* The word as a whole item. */
	length = strlen(word);
	for (found = strstr(flags, word); found != NULL; found = strstr(found + 1, word)) {
		if ((found == flags || found[-1] == ',') && (found[length] == '\0' || found[length] == ','))
			return 1;
	}
	return 0;
}

/* The signatures of one Wycheproof ECDSA file: valid ones parse; ones flagged as broken encodings do not. */
static void
test_wycheproof_signatures(
	const char *name,
	unsigned long *parsed)
{
	static const char *encoding_flags[] = {
		"InvalidEncoding", "BerEncodedSignature", "InvalidTypesInSignature", "MissingZero"
	};
	struct vec_file file;
	unsigned char bytes[OBJECT_MAX];
	size_t size;
	const char *flags;
	const char *result;
	unsigned long valid_wrong;
	unsigned long broken_wrong;
	unsigned long valid_count;
	unsigned long broken_count;
	size_t index;
	int broken;
	int error;

	/* Reads every record. */
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return;
	}
	valid_wrong = 0;
	broken_wrong = 0;
	valid_count = 0;
	broken_count = 0;
	while (vec_next(&file)) {
		/* A PEM text in a group ends its record at the blank line inside it, so some records hold no test. */
		if (vec_get(&file, "sig") == NULL)
			continue;
		size = decode_field(&file, "sig", bytes);
		flags = vec_get(&file, "flags");
		result = vec_get(&file, "result");
		if (size == (size_t)-1 || flags == NULL || result == NULL) {
			valid_wrong++;
			continue;
		}
		error = read_ecdsa_signature(bytes, size);
		if (strcmp(result, "valid") == 0) {
			valid_count++;
			if (error != 0) {
				valid_wrong++;
				printf("  %s tcId %s: valid signature refused (%d)\n", name, vec_get(&file, "tcId"), error);
			}
		}
		broken = 0;
		for (index = 0; index < sizeof(encoding_flags) / sizeof(encoding_flags[0]); index++) {
			if (has_flag(flags, encoding_flags[index]))
				broken = 1;
		}
		if (broken) {
			broken_count++;
			if (error == 0)
				broken_wrong++;
		}
		*parsed += 1UL;
	}
	vec_close(&file);
	CHECK(valid_count > 0UL);
	CHECK(broken_count > 0UL);
	CHECK(valid_wrong == 0UL);
	CHECK(broken_wrong == 0UL);
}

/* Counts leading zero octets of a hex string (for comparing a modulus with a magnitude). */
static size_t
skip_zero_octets(
	const unsigned char *bytes,
	size_t size)
{
	size_t skip;

	/* Leading 0x00 octets, keeping one. */
	skip = 0;
	while (skip + 1U < size && bytes[skip] == 0U)
		skip++;
	return skip;
}

/* Compares an RSA modulus given as hex with the magnitude read from the key; returns 1 when they are the same number. */
static int
modulus_matches(
	const struct vec_file *file,
	const unsigned char *magnitude,
	size_t magnitude_size)
{
	unsigned char modulus[OBJECT_MAX];
	size_t modulus_size;
	size_t skip;

	/* The file writes the modulus with or without a leading 0x00; the number is the same. */
	modulus_size = decode_field(file, "group.publicKey.modulus", modulus);
	if (modulus_size == (size_t)-1)
		return 0;
	skip = skip_zero_octets(modulus, modulus_size);
	if (modulus_size - skip != magnitude_size)
		return 0;
	if (memcmp(modulus + skip, magnitude, magnitude_size) != 0)
		return 0;
	return 1;
}

/*
 * The public keys of one Wycheproof file: every publicKeyDer walks as a
 * SubjectPublicKeyInfo, and an RSA key's modulus is the file's modulus.
 * A group's PEM text contains a blank line, which ends the vector reader's
 * record there: the key is in one record and the modulus in the next.
 */
static void
test_wycheproof_keys(
	const char *name,
	int is_rsa,
	unsigned long *keys)
{
	struct vec_file file;
	unsigned char der[OBJECT_MAX];
	unsigned char previous[OBJECT_MAX];
	unsigned char pending[OBJECT_MAX];
	struct cryc_der top;
	struct cryc_der rsa;
	struct cryc_der_element element;
	struct cryc_der_element sequence;
	const uint8_t *key;
	const uint8_t *magnitude;
	size_t size;
	size_t previous_size;
	size_t pending_size;
	size_t key_size;
	size_t magnitude_size;
	unsigned long wrong;
	unsigned long seen;
	unsigned long compared;
	unsigned unused;
	int have_pending;
	int error;

	/* Reads every record; a group repeats its key in each of its tests, so a repeat is skipped. */
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return;
	}
	wrong = 0;
	seen = 0;
	compared = 0;
	previous_size = 0;
	pending_size = 0;
	have_pending = 0;
	while (vec_next(&file)) {
		/* A modulus waits in a later record than its key. */
		if (have_pending && vec_get(&file, "group.publicKey.modulus") != NULL) {
			compared++;
			if (!modulus_matches(&file, pending, pending_size))
				wrong++;
			have_pending = 0;
		}
		if (vec_get(&file, "group.publicKeyDer") == NULL)
			continue;
		size = decode_field(&file, "group.publicKeyDer", der);
		if (size == (size_t)-1) {
			wrong++;
			continue;
		}
		if (size == previous_size && memcmp(der, previous, size) == 0)
			continue;
		memcpy(previous, der, size);
		previous_size = size;
		seen++;

		/* SubjectPublicKeyInfo, to the last byte. */
		cryc_der_init(&top, der, size);
		error = cryc_der_expect(&top, CRYC_DER_SEQUENCE, &sequence);
		if (error != 0 || !cryc_der_empty(&top)) {
			wrong++;
			continue;
		}
		error = cryc_der_enter(&sequence, &top);
		if (error == 0)
			error = read_algorithm(&top);
		if (error == 0)
			error = cryc_der_expect(&top, CRYC_DER_BIT_STRING, &element);
		if (error == 0)
			error = cryc_der_bit_string(&element, &key, &key_size, &unused);
		if (error != 0 || !cryc_der_empty(&top)) {
			wrong++;
			continue;
		}
		if (!is_rsa)
			continue;

		/* RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } (RFC 8017 A.1.1). */
		cryc_der_init(&top, key, key_size);
		error = cryc_der_expect(&top, CRYC_DER_SEQUENCE, &sequence);
		if (error != 0 || !cryc_der_empty(&top)) {
			wrong++;
			continue;
		}
		error = cryc_der_enter(&sequence, &rsa);
		if (error == 0)
			error = cryc_der_expect(&rsa, CRYC_DER_INTEGER, &element);
		if (error == 0)
			error = cryc_der_integer(&element, &magnitude, &magnitude_size);
		if (error != 0) {
			wrong++;
			continue;
		}
		memcpy(pending, magnitude, magnitude_size);
		pending_size = magnitude_size;
		have_pending = 1;
		error = cryc_der_expect(&rsa, CRYC_DER_INTEGER, &element);
		if (error == 0)
			error = cryc_der_integer(&element, &magnitude, &magnitude_size);
		if (error != 0 || !cryc_der_empty(&rsa))
			wrong++;

		/* The modulus may be in this very record. */
		if (vec_get(&file, "group.publicKey.modulus") != NULL) {
			compared++;
			if (!modulus_matches(&file, pending, pending_size))
				wrong++;
			have_pending = 0;
		}
	}
	vec_close(&file);
	printf("  %s: %lu keys, %lu moduli compared, %lu wrong\n", name, seen, compared, wrong);
	CHECK(seen > 0UL);
	CHECK(wrong == 0UL);
	if (is_rsa)
		CHECK(compared == seen);
	*keys += seen;
}

/* The certificates of the x509-limbo cases: whatever the reader decides, the walk stays inside the input. */
static void
test_limbo(
	unsigned char **first,
	size_t *first_size)
{
	struct vec_file file;
	unsigned char *der;
	size_t size;
	size_t index;
	char key[32];
	unsigned long records;
	unsigned long certificates;
	unsigned long accepted;
	unsigned long negative;
	unsigned long unexplained;
	struct cryc_der top;

	/* The peer and the trusted certificates of every case. */
	der = malloc(LIMBO_MAX);
	CHECK(der != NULL);
	if (der == NULL)
		return;
	if (vec_open(&file, "limbo") != 0) {
		CHECK(0);
		free(der);
		return;
	}
	records = 0;
	certificates = 0;
	accepted = 0;
	negative = 0;
	unexplained = 0;
	while (vec_next(&file)) {
		records++;
		size = vec_hex(&file, "peer", der, LIMBO_MAX);
		if (size != (size_t)-1) {
			certificates++;
			if (walk_certificate(der, size) == 0)
				accepted++;
			else if (strcmp(vec_get(&file, "id"), NEGATIVE_SERIAL_CASE) == 0)
				negative++;
			else
				unexplained++;
			cryc_der_init(&top, der, size);
			walk(top, der, size, 0);
			if (*first == NULL) {
				*first = malloc(size);
				if (*first != NULL) {
					memcpy(*first, der, size);
					*first_size = size;
				}
			}
		}
		for (index = 0; index < 4U; index++) {
			sprintf(key, "trusted.%u", (unsigned)index);
			size = vec_hex(&file, key, der, LIMBO_MAX);
			if (size == (size_t)-1)
				continue;
			certificates++;
			if (walk_certificate(der, size) == 0)
				accepted++;
			else if (strcmp(vec_get(&file, "id"), NEGATIVE_SERIAL_CASE) == 0)
				negative++;
			else
				unexplained++;
		}
	}
	vec_close(&file);
	free(der);
	CHECK(records == 9792UL);
	CHECK(certificates >= records);

	/*
	 * Every certificate walks as X.509 except those of the one case whose
	 * serial number is negative, which cryc_der_integer refuses by its
	 * contract (a serial number is read as a non-negative magnitude).
	 */
	CHECK(unexplained == 0UL);
	CHECK(negative == 2UL);
	CHECK(accepted + negative == certificates);
	printf("limbo: %lu certificates, %lu walked, %lu refused for a negative serial\n", certificates, accepted, negative);
}

/* Runs the checks. */
int
main(void)
{
	unsigned char *seed;
	size_t seed_size;
	unsigned long parsed;
	unsigned long keys;

	/* The reader. */
	test_lengths();
	test_headers();
	test_walk();
	test_optional_malformed();
	test_integer();
	test_small_integer();
	test_boolean();
	test_bit_string();
	test_oid();
	test_time_values();
	test_time_errors();
	test_time_every_day();

	/* The corpora. */
	seed = NULL;
	seed_size = 0;
	test_bundle(&seed, &seed_size);
	test_limbo(&seed, &seed_size);
	parsed = 0;
	test_wycheproof_signatures("wycheproof-ecdsa_secp256r1_sha256_test", &parsed);
	test_wycheproof_signatures("wycheproof-ecdsa_secp256r1_sha512_test", &parsed);
	test_wycheproof_signatures("wycheproof-ecdsa_secp384r1_sha384_test", &parsed);
	test_wycheproof_signatures("wycheproof-ecdsa_secp384r1_sha512_test", &parsed);
	CHECK(parsed > 1000UL);
	keys = 0;
	test_wycheproof_keys("wycheproof-ecdsa_secp256r1_sha256_test", 0, &keys);
	test_wycheproof_keys("wycheproof-ecdsa_secp384r1_sha384_test", 0, &keys);
	test_wycheproof_keys("wycheproof-ed25519_test", 0, &keys);
	test_wycheproof_keys("wycheproof-rsa_signature_2048_sha256_test", 1, &keys);
	test_wycheproof_keys("wycheproof-rsa_signature_3072_sha256_test", 1, &keys);
	test_wycheproof_keys("wycheproof-rsa_signature_4096_sha512_test", 1, &keys);
	test_wycheproof_keys("wycheproof-rsa_pss_2048_sha256_mgf1_32_test", 1, &keys);
	test_wycheproof_keys("wycheproof-rsa_pss_3072_sha256_mgf1_32_test", 1, &keys);
	test_wycheproof_keys("wycheproof-rsa_pss_4096_sha512_mgf1_64_test", 1, &keys);
	CHECK(keys > 250UL);

	/* Random and mutated input, seeded with a real certificate. */
	CHECK(seed != NULL);
	if (seed != NULL) {
		test_random(seed, seed_size);
		free(seed);
	}
	return check_finish("test-der");
}
