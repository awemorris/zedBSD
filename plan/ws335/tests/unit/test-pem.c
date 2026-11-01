/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks base64 (RFC 4648 section 4) and the PEM certificate reader
 * (RFC 7468) of cryc-pem.h.
 *
 * Base64 is checked against the test vectors of RFC 4648 section 10, every
 * alphabet character and every other byte value, every canonical and
 * non-canonical final group (all 2-character and 3-character tails, so the
 * zero-bits rule of RFC 4648 3.5 is exercised exhaustively), whitespace at
 * every position, the output-room limits, round trips through an encoder
 * written here from RFC 4648 section 4, and random text compared with a
 * reference validity rule.  The PEM reader is checked on hand-built texts
 * for every case its header names (explanatory text, other labels, a BEGIN
 * line with no END line, a different END label, broken base64, an empty
 * body, text that only looks like a BEGIN line, positions), on the two
 * certificate examples of RFC 7468, on random mixes of PEM fragments, on
 * the public-key texts of the Wycheproof files (whose DER is given beside
 * them), on certificates re-encoded here (limbo), and on the whole
 * ca-certificates bundle, each block compared with the bundle's own text.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-der.h"
#include "userland/base/libcrypto-compat/cryc-pem.h"

/* The value of a fill that shows a byte was not written. */
#define SENTINEL_BYTE	0x5aU

/* Where the bundle is looked for when WS335_CACERT is not set. */
#define CACERT_DEFAULT	"/home/awe/zedBSD-claude1/build/distfiles/cacert-2026-08-13.pem"

/* The largest certificate of the x509-limbo cases is about 78 KB. */
#define LIMBO_MAX	131072U

/* The pieces of a PEM block (RFC 7468 section 2). */
#define BEGIN_LINE	"-----BEGIN CERTIFICATE-----"
#define END_LINE	"-----END CERTIFICATE-----"

/* "foobar" in base64 (RFC 4648 section 10). */
#define FOOBAR		"Zm9vYmFy"

/*
 * Values referenced: RFC 7468 (April 2015) section 5.1 Figure 6 and section
 * 5.2 Figure 7: the base64 lines of two certificate examples.
 */
#define FIG6_LINES \
	"MIICLDCCAdKgAwIBAgIBADAKBggqhkjOPQQDAjB9MQswCQYDVQQGEwJCRTEPMA0G\n" \
	"A1UEChMGR251VExTMSUwIwYDVQQLExxHbnVUTFMgY2VydGlmaWNhdGUgYXV0aG9y\n" \
	"aXR5MQ8wDQYDVQQIEwZMZXV2ZW4xJTAjBgNVBAMTHEdudVRMUyBjZXJ0aWZpY2F0\n" \
	"ZSBhdXRob3JpdHkwHhcNMTEwNTIzMjAzODIxWhcNMTIxMjIyMDc0MTUxWjB9MQsw\n" \
	"CQYDVQQGEwJCRTEPMA0GA1UEChMGR251VExTMSUwIwYDVQQLExxHbnVUTFMgY2Vy\n" \
	"dGlmaWNhdGUgYXV0aG9yaXR5MQ8wDQYDVQQIEwZMZXV2ZW4xJTAjBgNVBAMTHEdu\n" \
	"dVRMUyBjZXJ0aWZpY2F0ZSBhdXRob3JpdHkwWTATBgcqhkjOPQIBBggqhkjOPQMB\n" \
	"BwNCAARS2I0jiuNn14Y2sSALCX3IybqiIJUvxUpj+oNfzngvj/Niyv2394BWnW4X\n" \
	"uQ4RTEiywK87WRcWMGgJB5kX/t2no0MwQTAPBgNVHRMBAf8EBTADAQH/MA8GA1Ud\n" \
	"DwEB/wQFAwMHBgAwHQYDVR0OBBYEFPC0gf6YEr+1KLlkQAPLzB9mTigDMAoGCCqG\n" \
	"SM49BAMCA0gAMEUCIDGuwD1KPyG+hRf88MeyMQcqOFZD0TbVleF+UsAGQ4enAiEA\n" \
	"l4wOuDwKQa+upc8GftXE2C//4mKANBC6It01gUaTIpo=\n"

#define FIG7_LINES \
	"MIIBmTCCAUegAwIBAgIBKjAJBgUrDgMCHQUAMBMxETAPBgNVBAMTCEF0bGFudGlz\n" \
	"MB4XDTEyMDcwOTAzMTAzOFoXDTEzMDcwOTAzMTAzN1owEzERMA8GA1UEAxMIQXRs\n" \
	"YW50aXMwXDANBgkqhkiG9w0BAQEFAANLADBIAkEAu+BXo+miabDIHHx+yquqzqNh\n" \
	"Ryn/XtkJIIHVcYtHvIX+S1x5ErgMoHehycpoxbErZmVR4GCq1S2diNmRFZCRtQID\n" \
	"AQABo4GJMIGGMAwGA1UdEwEB/wQCMAAwIAYDVR0EAQH/BBYwFDAOMAwGCisGAQQB\n" \
	"gjcCARUDAgeAMB0GA1UdJQQWMBQGCCsGAQUFBwMCBggrBgEFBQcDAzA1BgNVHQEE\n" \
	"LjAsgBA0jOnSSuIHYmnVryHAdywMoRUwEzERMA8GA1UEAxMIQXRsYW50aXOCASow\n" \
	"CQYFKw4DAh0FAANBAKi6HRBaNEL5R0n56nvfclQNaXiDT174uf+lojzA4lhVInc0\n" \
	"ILwpnZ1izL4MlI9eCSHhVQBHEp2uQdXJB+d5Byg=\n"

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x1b873593U;

/*
 * Values referenced: RFC 4648 (October 2006) section 4 Table 1: the base64
 * alphabet, in the order of the values 0 to 63.
 */
static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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

/*
 * Encodes size bytes as base64 (RFC 4648 section 4: 3 bytes to 4
 * characters, "=" padding), breaking the text every wrap characters with
 * eol (wrap 0: no breaks), and ends it with eol when wrap is not 0.
 * Returns the length; out has room for the result and a NUL.
 */
static size_t
encode(
	const unsigned char *data,
	size_t size,
	char *out,
	size_t wrap,
	const char *eol)
{
	size_t used;
	size_t column;
	size_t index;
	unsigned long group;
	unsigned count;
	unsigned shift;
	char characters[4];
	unsigned part;

	/* Every group of up to three bytes. */
	used = 0;
	column = 0;
	for (index = 0; index < size; index += 3U) {
		count = (unsigned)(size - index < 3U ? size - index : 3U);
		group = (unsigned long)data[index] << 16;
		if (count > 1U)
			group |= (unsigned long)data[index + 1U] << 8;
		if (count > 2U)
			group |= (unsigned long)data[index + 2U];
		for (part = 0; part < 4U; part++) {
			shift = 18U - 6U * part;
			characters[part] = alphabet[(group >> shift) & 63UL];
		}
		if (count < 3U)
			characters[3] = '=';
		if (count < 2U)
			characters[2] = '=';
		for (part = 0; part < 4U; part++) {
			if (wrap != 0U && column == wrap) {
				memcpy(out + used, eol, strlen(eol));
				used += strlen(eol);
				column = 0;
			}
			out[used++] = characters[part];
			column++;
		}
	}
	if (wrap != 0U) {
		memcpy(out + used, eol, strlen(eol));
		used += strlen(eol);
	}
	out[used] = '\0';
	return used;
}

/* Returns a copy of text of exactly size bytes, with no NUL after them (so the sanitizer sees any overread). */
static char *
dup_exact(
	const char *text,
	size_t size)
{
	char *copy;

	/* One byte at least, so that an empty text has an address. */
	copy = malloc(size == 0U ? 1U : size);
	if (copy == NULL) {
		printf("FAIL out of memory\n");
		exit(EXIT_FAILURE);
	}
	if (size > 0U)
		memcpy(copy, text, size);
	return copy;
}

/* Decodes text (a C string) with room bytes of output; checks the error and, for success, the bytes. */
static void
check_decode(
	const char *text,
	size_t room,
	int expected_error,
	const char *expected_hex)
{
	unsigned char out[96];
	unsigned char want[96];
	char *copy;
	size_t written;
	size_t want_size;
	int error;

	/* The text is passed without a NUL after it. */
	CHECK(room <= 64U);
	memset(out, SENTINEL_BYTE, sizeof(out));
	copy = dup_exact(text, strlen(text));
	written = 77;
	error = cryc_base64_decode(copy, strlen(text), out, room, &written);
	free(copy);
	CHECK(error == expected_error);

	/* Nothing past the room is written. */
	if (room < sizeof(out)) {
		CHECK(out[room] == SENTINEL_BYTE);
		CHECK(out[sizeof(out) - 1U] == SENTINEL_BYTE);
	}
	if (expected_error != 0) {
		CHECK(written == 0U);
		return;
	}
	want_size = hex_decode(expected_hex, want, sizeof(want));
	CHECK(written == want_size);
	CHECK_BYTES(out, want, want_size);
}

/* The test vectors of RFC 4648 section 10, and other examples of a base64 text. */
static void
test_rfc4648_vectors(void)
{
	/* Values referenced: RFC 4648 section 10, the six vectors "f" to "foobar". */
	check_decode("", 64, 0, "");
	check_decode("Zg==", 64, 0, "66");
	check_decode("Zm8=", 64, 0, "666f");
	check_decode("Zm9v", 64, 0, "666f6f");
	check_decode("Zm9vYg==", 64, 0, "666f6f62");
	check_decode("Zm9vYmE=", 64, 0, "666f6f6261");
	check_decode("Zm9vYmFy", 64, 0, "666f6f626172");

	/* Hand-computed groups: "ABC", the extremes of the alphabet, and padded tails. */
	check_decode("QUJD", 64, 0, "414243");
	check_decode("QUJDRA==", 64, 0, "41424344");
	check_decode("////", 64, 0, "ffffff");
	check_decode("++++", 64, 0, "fbefbe");
	check_decode("AAAA", 64, 0, "000000");
	check_decode("AAA=", 64, 0, "0000");
	check_decode("AA==", 64, 0, "00");
	check_decode("/w==", 64, 0, "ff");
	check_decode("//8=", 64, 0, "ffff");
	check_decode("+/8=", 64, 0, "fbff");
}

/* Every alphabet character gives its value; every other byte value is refused. */
static void
test_alphabet(void)
{
	char text[16];
	unsigned char out[8];
	size_t written;
	unsigned value;
	unsigned character;
	unsigned position;
	int in_alphabet;
	int error;
	int bad;

	/* Each character first and last in a group: its value, in the right bits of the output. */
	bad = 0;
	for (value = 0; value < 64U; value++) {
		text[0] = alphabet[value];
		text[1] = 'A';
		text[2] = 'A';
		text[3] = 'A';
		error = cryc_base64_decode(text, 4, out, sizeof(out), &written);
		if (error != 0 || written != 3U || out[0] != (unsigned char)(value << 2) || out[1] != 0U || out[2] != 0U)
			bad++;
		text[0] = 'A';
		text[3] = alphabet[value];
		error = cryc_base64_decode(text, 4, out, sizeof(out), &written);
		if (error != 0 || written != 3U || out[0] != 0U || out[1] != 0U || out[2] != (unsigned char)value)
			bad++;
	}
	CHECK(bad == 0);

	/* Each byte value at each place of a long text: only the alphabet decodes (whitespace and "=" make it ragged). */
	bad = 0;
	for (position = 0; position < 8U; position++) {
		for (character = 0; character < 256U; character++) {
			memcpy(text, "QUJDQUJD", 8);
			text[position] = (char)character;
			in_alphabet = character != 0U && strchr(alphabet, (int)character) != NULL;
			error = cryc_base64_decode(text, 8, out, sizeof(out), &written);
			if (in_alphabet && (error != 0 || written != 6U))
				bad++;
			if (!in_alphabet && (error != EINVAL || written != 0U))
				bad++;
		}
	}
	CHECK(bad == 0);

	/* The URL alphabet (RFC 4648 section 5) is not accepted here. */
	check_decode("-_-_", 64, EINVAL, "");
	check_decode("Zm9-", 64, EINVAL, "");
	check_decode("Zm9_", 64, EINVAL, "");
}

/* Padding: where "=" may stand, what may follow it, and the zero bits it leaves. */
static void
test_padding(void)
{
	static const char *bad_texts[] = {
		"Zg=", "Zg", "Z", "Zm8", "Zm9", "Zm9vYg=", "Zm9vYg", "Zm9vY", "Z===", "====", "=", "==", "===", "=Zg=",
		"Zg==Zg==", "Zm8=Zm8=", "Zg=a", "Zm8=a", "Zg==a", "Zg===", "Zg====", "Zm8==", "Zm8===", "Zg==Zm9v",
		"Zm9vZg==Zg==", "Z=g=", "Zg=g", "=g==", "Zm=v"
	};
	size_t index;

	/* One "=" ends three characters, two end two; anything else is refused (RFC 7468 section 3 requires padding). */
	for (index = 0; index < sizeof(bad_texts) / sizeof(bad_texts[0]); index++)
		check_decode(bad_texts[index], 64, EINVAL, "");

	/* The bits left over by padding must be zero (RFC 4648 3.5): 'h' is 100001, 'g' is 100000. */
	check_decode("Zh==", 64, EINVAL, "");
	check_decode("Zg==", 64, 0, "66");
	check_decode("Zm9=", 64, EINVAL, "");
	check_decode("Zm+=", 64, EINVAL, "");
	check_decode("Zm8=", 64, 0, "666f");
	check_decode("Zm9vYh==", 64, EINVAL, "");
	check_decode("Zm9vYmF=", 64, EINVAL, "");
}

/* Every two-character and three-character tail against the zero-bits rule, computed from the alphabet. */
static void
test_tails(void)
{
	char text[8];
	unsigned char out[8];
	size_t written;
	unsigned first;
	unsigned second;
	unsigned third;
	int error;
	int bad;
	int expected;

	/* "xy==": one byte, valid when the low four bits of y's value are zero. */
	bad = 0;
	text[2] = '=';
	text[3] = '=';
	for (first = 0; first < 64U; first++) {
		for (second = 0; second < 64U; second++) {
			text[0] = alphabet[first];
			text[1] = alphabet[second];
			expected = (second & 15U) == 0U;
			error = cryc_base64_decode(text, 4, out, sizeof(out), &written);
			if (expected && (error != 0 || written != 1U || out[0] != (unsigned char)((first << 2) | (second >> 4))))
				bad++;
			if (!expected && (error != EINVAL || written != 0U))
				bad++;
		}
	}
	CHECK(bad == 0);

	/* "xyz=": two bytes, valid when the low two bits of z's value are zero. */
	bad = 0;
	text[3] = '=';
	for (first = 0; first < 64U; first++) {
		for (second = 0; second < 64U; second++) {
			for (third = 0; third < 64U; third++) {
				text[0] = alphabet[first];
				text[1] = alphabet[second];
				text[2] = alphabet[third];
				expected = (third & 3U) == 0U;
				error = cryc_base64_decode(text, 4, out, sizeof(out), &written);
				if (expected && error != 0)
					bad++;
				if (expected && (written != 2U || out[0] != (unsigned char)((first << 2) | (second >> 4)) ||
				    out[1] != (unsigned char)((second << 4) | (third >> 2))))
					bad++;
				if (!expected && (error != EINVAL || written != 0U))
					bad++;
			}
		}
	}
	CHECK(bad == 0);
}

/* Whitespace between characters is skipped (space, tab, CR, LF); other control characters are not. */
static void
test_whitespace(void)
{
	static const char skipped[] = " \t\r\n";
	static const char refused[] = "\f\v\x01\x1f\x85\xa0";
	static const char unpadded[] = "Zm9vYmFy";
	static const char padded[] = "Zm9vYg==";
	char text[32];
	unsigned char out[16];
	size_t written;
	size_t position;
	size_t index;
	int bad;
	int error;

	/* One whitespace character at every position of an unpadded and a padded text. */
	bad = 0;
	for (index = 0; index < strlen(skipped); index++) {
		for (position = 0; position <= 8U; position++) {
			memcpy(text, unpadded, position);
			text[position] = skipped[index];
			memcpy(text + position + 1U, &unpadded[position], 8U - position);
			error = cryc_base64_decode(text, 9, out, sizeof(out), &written);
			if (error != 0 || written != 6U || memcmp(out, "foobar", 6) != 0)
				bad++;
		}
		for (position = 0; position <= 8U; position++) {
			memcpy(text, padded, position);
			text[position] = skipped[index];
			memcpy(text + position + 1U, &padded[position], 8U - position);
			error = cryc_base64_decode(text, 9, out, sizeof(out), &written);
			if (error != 0 || written != 4U || memcmp(out, "foob", 4) != 0)
				bad++;
		}
	}
	CHECK(bad == 0);

	/* Only whitespace, whitespace around padding, and line ends of every kind. */
	check_decode(" \t\r\n \n", 64, 0, "");
	check_decode("Zg = =", 64, 0, "66");
	check_decode("Zg\n==\n", 64, 0, "66");
	check_decode("\r\nZm9v\r\nYmFy\r\n", 64, 0, "666f6f626172");
	check_decode("Zm9v\rYmFy", 64, 0, "666f6f626172");
	check_decode("Zm9v\n\n\n\nYmFy", 64, 0, "666f6f626172");
	check_decode("Zg==\n\n", 64, 0, "66");
	check_decode("Zg==\nZg==", 64, EINVAL, "");

	/* Other whitespace and control characters, and bytes of 0x80 and above, are refused. */
	bad = 0;
	for (index = 0; index < sizeof(refused) - 1U; index++) {
		memcpy(text, "Zm9v", 4);
		text[4] = refused[index];
		memcpy(text + 5, "YmFy", 4);
		error = cryc_base64_decode(text, 9, out, sizeof(out), &written);
		if (error != EINVAL || written != 0U)
			bad++;
	}
	CHECK(bad == 0);

	/* A NUL byte in the text is not whitespace. */
	memcpy(text, "Zm9v\0YmFy", 9);
	error = cryc_base64_decode(text, 9, out, sizeof(out), &written);
	CHECK(error == EINVAL);
}

/* The room: exact, one short, none; and that a malformed text is EINVAL whatever the room. */
static void
test_room(void)
{
	unsigned char data[64];
	unsigned char out[96];
	char text[160];
	size_t size;
	size_t length;
	size_t written;
	size_t index;
	int error;
	int bad;

	/* Every size from 0 to 40: the exact room works, one byte less is ENOBUFS and writes nothing past the room. */
	bad = 0;
	for (index = 0; index < sizeof(data); index++)
		data[index] = (unsigned char)(index * 37U + 11U);
	for (size = 0; size <= 40U; size++) {
		length = encode(data, size, text, 0, "");
		memset(out, SENTINEL_BYTE, sizeof(out));
		written = 99;
		error = cryc_base64_decode(text, length, out, size, &written);
		if (error != 0 || written != size || memcmp(out, data, size) != 0 || out[size] != SENTINEL_BYTE)
			bad++;
		if (size > 0U) {
			memset(out, SENTINEL_BYTE, sizeof(out));
			written = 99;
			error = cryc_base64_decode(text, length, out, size - 1U, &written);
			if (error != ENOBUFS || written != 0U)
				bad++;
			if (out[size - 1U] != SENTINEL_BYTE)
				bad++;
		}
	}
	CHECK(bad == 0);

	/* Room zero, with and without a buffer, for empty and non-empty text. */
	written = 99;
	CHECK(cryc_base64_decode("", 0, NULL, 0, &written) == 0);
	CHECK(written == 0U);
	written = 99;
	CHECK(cryc_base64_decode(NULL, 0, NULL, 0, &written) == 0);
	CHECK(written == 0U);
	written = 99;
	CHECK(cryc_base64_decode("Zg==", 4, NULL, 0, &written) == ENOBUFS);
	CHECK(written == 0U);

	/* A malformed text is reported as malformed, however little room there is. */
	written = 99;
	CHECK(cryc_base64_decode("Zm9v!", 5, out, 0, &written) == EINVAL);
	CHECK(written == 0U);
	written = 99;
	CHECK(cryc_base64_decode("Zm9vYmFy!", 9, out, 2, &written) == EINVAL);
	written = 99;
	CHECK(cryc_base64_decode("Zm9vYg=", 7, out, 1, &written) == EINVAL);
}

/* Returns 1 when the text, with whitespace removed, is canonical padded base64 (the reference rule). */
static int
reference_valid(
	const char *stripped,
	size_t length)
{
	size_t index;
	size_t padding;
	unsigned last;
	const char *found;

	/* A whole number of groups. */
	if (length % 4U != 0U)
		return 0;

	/* Only alphabet characters, then at most two "=" at the very end. */
	padding = 0;
	if (length >= 1U && stripped[length - 1U] == '=')
		padding = 1;
	if (length >= 2U && stripped[length - 1U] == '=' && stripped[length - 2U] == '=')
		padding = 2;
	for (index = 0; index < length - padding; index++) {
		if (stripped[index] == '\0' || strchr(alphabet, stripped[index]) == NULL)
			return 0;
	}

	/* The bits that padding leaves must be zero. */
	if (padding == 1U) {
		found = strchr(alphabet, stripped[length - 2U]);
		last = (unsigned)(found - alphabet);
		if ((last & 3U) != 0U)
			return 0;
	}
	if (padding == 2U) {
		found = strchr(alphabet, stripped[length - 3U]);
		last = (unsigned)(found - alphabet);
		if ((last & 15U) != 0U)
			return 0;
	}
	return 1;
}

/* Random data round-trips through the encoder with every wrapping; random text agrees with the reference rule. */
static void
test_roundtrip_and_random(void)
{
	static const size_t wraps[] = { 0, 4, 64, 76, 5, 1000 };
	static const char *eols[] = { "\n", "\r\n", "\r", " ", "\t\n" };
	static const char pool[] = "ABCDEFGH wxyz0123+/=== \n\r\t";
	unsigned char data[600];
	unsigned char out[600];
	char *text;
	char stripped[96];
	char sample[96];
	char again[96];
	size_t size;
	size_t length;
	size_t written;
	size_t index;
	size_t round;
	size_t count;
	int error;
	int bad;
	int expected;

	/* Data of random sizes (0 to 39 all hit) with every wrapping and line end. */
	text = malloc(sizeof(data) * 2U + 64U);
	CHECK(text != NULL);
	if (text == NULL)
		return;
	bad = 0;
	for (round = 0; round < 3000U; round++) {
		size = round < 40U ? round : random32() % (sizeof(data) + 1U);
		for (index = 0; index < size; index++)
			data[index] = (unsigned char)random32();
		length = encode(data, size, text, wraps[round % (sizeof(wraps) / sizeof(wraps[0]))], eols[round % 5U]);
		error = cryc_base64_decode(text, length, out, sizeof(out), &written);
		if (error != 0 || written != size || memcmp(out, data, size) != 0)
			bad++;
	}
	CHECK(bad == 0);
	free(text);

	/* Random text from a pool of alphabet characters, "=" and whitespace: the verdict is the reference rule's. */
	bad = 0;
	for (round = 0; round < 200000U; round++) {
		size = random32() % 17U;
		count = 0;
		for (index = 0; index < size; index++) {
			sample[index] = pool[random32() % (sizeof(pool) - 1U)];
			if (sample[index] != ' ' && sample[index] != '\n' && sample[index] != '\r' && sample[index] != '\t')
				stripped[count++] = sample[index];
		}
		stripped[count] = '\0';
		expected = reference_valid(stripped, count);
		error = cryc_base64_decode(sample, size, out, sizeof(out), &written);
		if (expected && (error != 0 || written > count / 4U * 3U))
			bad++;
		if (!expected && (error != EINVAL || written != 0U))
			bad++;

		/* A text accepted must re-encode to the same characters (the form is canonical). */
		if (error == 0) {
			length = encode(out, written, again, 0, "");
			if (length != count || memcmp(again, stripped, count) != 0)
				bad++;
		}
	}
	CHECK(bad == 0);
}

/* A megabyte of data round-trips (the text is long, with line ends every 64 characters). */
static void
test_large(void)
{
	unsigned char *data;
	unsigned char *out;
	char *text;
	size_t size;
	size_t length;
	size_t written;
	size_t index;
	int error;

	/* One mebibyte and one byte more than a multiple of three. */
	size = 1048576U + 1U;
	data = malloc(size);
	out = malloc(size);
	text = malloc(size * 2U);
	CHECK(data != NULL && out != NULL && text != NULL);
	if (data == NULL || out == NULL || text == NULL)
		return;
	for (index = 0; index < size; index++)
		data[index] = (unsigned char)random32();
	length = encode(data, size, text, 64, "\n");
	error = cryc_base64_decode(text, length, out, size, &written);
	CHECK(error == 0);
	CHECK(written == size);
	CHECK(memcmp(out, data, size) == 0);
	free(data);
	free(out);
	free(text);
}

/* Reads from text (a C string, copied with no NUL after it) starting at from; checks every output of one call. */
static void
check_next(
	const char *text,
	size_t from,
	int expected_error,
	size_t expected_position,
	const char *expected_der)
{
	char *copy;
	size_t position;
	size_t size;
	size_t der_size;
	uint8_t *der;
	int error;

	/* A pre-filled result shows what the call writes. */
	size = strlen(text);
	copy = dup_exact(text, size);
	position = from;
	der = (uint8_t *)copy;
	der_size = 99;
	error = cryc_pem_next_certificate(copy, size, &position, &der, &der_size);
	CHECK(error == expected_error);
	CHECK(position == expected_position);
	if (expected_error != 0) {
		CHECK(der == NULL);
		CHECK(der_size == 0U);
	} else {
		CHECK(der != NULL);
		CHECK(der_size == strlen(expected_der));
		if (der != NULL && der_size == strlen(expected_der))
			CHECK_BYTES(der, expected_der, der_size);
		free(der);
	}
	free(copy);
}

/* Checks that text holds no certificate: ENOENT with the position at the end. */
static void
check_none(
	const char *text)
{
	/* The search runs to the end of the text. */
	check_next(text, 0, ENOENT, strlen(text), NULL);
}

/* One block in its forms: line ends, trailing spaces, no final line end, text around it. */
static void
test_pem_forms(void)
{
	/* The plain form, everything consumed. */
	check_next(BEGIN_LINE "\n" FOOBAR "\n" END_LINE "\n", 0, 0, strlen(BEGIN_LINE "\n" FOOBAR "\n" END_LINE "\n"), "foobar");

	/* CR LF, a lone CR, and no line end after END. */
	check_next(BEGIN_LINE "\r\n" FOOBAR "\r\n" END_LINE "\r\n", 0, 0,
	    strlen(BEGIN_LINE "\r\n" FOOBAR "\r\n" END_LINE "\r\n"), "foobar");
	check_next(BEGIN_LINE "\r" FOOBAR "\r" END_LINE "\r", 0, 0, strlen(BEGIN_LINE "\r" FOOBAR "\r" END_LINE "\r"), "foobar");
	check_next(BEGIN_LINE "\n" FOOBAR "\n" END_LINE, 0, 0, strlen(BEGIN_LINE "\n" FOOBAR "\n" END_LINE), "foobar");

	/* Spaces and tabs after the BEGIN line and after the END line (lax: RFC 7468 section 3). */
	check_next(BEGIN_LINE " \t \n" FOOBAR "\n" END_LINE "  \t\n", 0, 0,
	    strlen(BEGIN_LINE " \t \n" FOOBAR "\n" END_LINE "  \t\n"), "foobar");

	/* The base64 broken over lines of any length, with blank lines and indentation inside. */
	check_next(BEGIN_LINE "\nZm9v\nYmFy\n" END_LINE "\n", 0, 0, strlen(BEGIN_LINE "\nZm9v\nYmFy\n" END_LINE "\n"), "foobar");
	check_next(BEGIN_LINE "\n\n  Zm9v  \n\n\tYmFy\n\n" END_LINE "\n", 0, 0,
	    strlen(BEGIN_LINE "\n\n  Zm9v  \n\n\tYmFy\n\n" END_LINE "\n"), "foobar");
	check_next(BEGIN_LINE "\nZ\nm\n9\nv\nY\nm\nF\ny\n" END_LINE "\n", 0, 0,
	    strlen(BEGIN_LINE "\nZ\nm\n9\nv\nY\nm\nF\ny\n" END_LINE "\n"), "foobar");

	/* END right after the base64, with no line end between them. */
	check_next(BEGIN_LINE "\n" FOOBAR END_LINE "\n", 0, 0, strlen(BEGIN_LINE "\n" FOOBAR END_LINE "\n"), "foobar");

	/* Padding at the end of the body. */
	check_next(BEGIN_LINE "\nZm9vYg==\n" END_LINE "\n", 0, 0, strlen(BEGIN_LINE "\nZm9vYg==\n" END_LINE "\n"), "foob");
	check_next(BEGIN_LINE "\nZm8=\n" END_LINE "\n", 0, 0, strlen(BEGIN_LINE "\nZm8=\n" END_LINE "\n"), "fo");

	/* Text before and after the block (explanatory text, RFC 7468 section 5.2), including text that mentions BEGIN. */
	check_next("Subject: CN=Atlantis\nIssuer: CN=Atlantis\nNot a -----BEGIN CERTIFICATE----- line\n"
	    BEGIN_LINE "\n" FOOBAR "\n" END_LINE "\nafter the block\n", 0, 0,
	    strlen("Subject: CN=Atlantis\nIssuer: CN=Atlantis\nNot a -----BEGIN CERTIFICATE----- line\n"
	    BEGIN_LINE "\n" FOOBAR "\n" END_LINE "\n"), "foobar");
}

/* Text that only looks like a BEGIN line is explanatory text. */
static void
test_pem_lookalikes(void)
{
	/* Not at the start of a line. */
	check_none("x" BEGIN_LINE "\n" FOOBAR "\n" END_LINE "\n");
	check_none(" " BEGIN_LINE "\n" FOOBAR "\n" END_LINE "\n");

	/* Text after the dashes, on the same line. */
	check_none(BEGIN_LINE " x\n" FOOBAR "\n" END_LINE "\n");
	check_none(BEGIN_LINE FOOBAR "\n" END_LINE "\n");

	/* Wrong dashes, wrong case, wrong label, a label with more words, a missing space. */
	check_none("----BEGIN CERTIFICATE-----\n" FOOBAR "\n" END_LINE "\n");
	check_none("-----BEGIN CERTIFICATE----\n" FOOBAR "\n" END_LINE "\n");
	check_none("-----BEGIN certificate-----\n" FOOBAR "\n-----END certificate-----\n");
	check_none("-----BEGINCERTIFICATE-----\n" FOOBAR "\n-----ENDCERTIFICATE-----\n");
	check_none("-----BEGIN CERTIFICATE REQUEST-----\n" FOOBAR "\n-----END CERTIFICATE REQUEST-----\n");
	check_none("-----BEGIN X509 CERTIFICATE-----\n" FOOBAR "\n-----END X509 CERTIFICATE-----\n");
	check_none("-----BEGIN TRUSTED CERTIFICATE-----\n" FOOBAR "\n-----END TRUSTED CERTIFICATE-----\n");
	check_none("-----BEGIN CERTIFICATE CHAIN-----\n" FOOBAR "\n-----END CERTIFICATE CHAIN-----\n");

	/* A lone END line, and text with neither. */
	check_none(END_LINE "\n");
	check_none("nothing to see\n");
	check_none("-----");
	check_none("");
}

/* A BEGIN line with no usable block after it: the error, and the position after the BEGIN line. */
static void
test_pem_broken(void)
{
	size_t after_begin;

	/* The position after the BEGIN line and its line end. */
	after_begin = strlen(BEGIN_LINE "\n");

	/* No END line at all, with and without a body. */
	check_next(BEGIN_LINE "\n" FOOBAR "\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE, 0, EINVAL, strlen(BEGIN_LINE), NULL);

	/* An END line of another label. */
	check_next(BEGIN_LINE "\n" FOOBAR "\n-----END CERTIFICATE REQUEST-----\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\n" FOOBAR "\n-----END PRIVATE KEY-----\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\n" FOOBAR "\n-----END certificate-----\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\n" FOOBAR "\n----END CERTIFICATE-----\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\n" FOOBAR "\n-----END CERTIFICATE----\n", 0, EINVAL, after_begin, NULL);

	/* Another BEGIN line before any END line. */
	check_next(BEGIN_LINE "\n" FOOBAR "\n" BEGIN_LINE "\n" FOOBAR "\n" END_LINE "\n", 0, EINVAL, after_begin, NULL);

	/* Base64 that does not decode: a bad character, bad padding, a ragged group, non-zero pad bits, text after padding. */
	check_next(BEGIN_LINE "\nZm9v!mFy\n" END_LINE "\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\nZm9vYmF\n" END_LINE "\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\nZm9vYg=\n" END_LINE "\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\nZm9vYh==\n" END_LINE "\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\nZg==Zg==\n" END_LINE "\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\nZm9v-Zm9v\n" END_LINE "\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\nZm9v\001YmFy\n" END_LINE "\n", 0, EINVAL, after_begin, NULL);

	/* No content: an empty body, and one of whitespace only. */
	check_next(BEGIN_LINE "\n" END_LINE "\n", 0, EINVAL, after_begin, NULL);
	check_next(BEGIN_LINE "\n \n\t\n\n" END_LINE "\n", 0, EINVAL, after_begin, NULL);
}

/* Several blocks, other labels between them, a broken one skipped by a caller that goes on. */
static void
test_pem_sequences(void)
{
	static const char first_text[] = "intro\n" BEGIN_LINE "\nZm9v\n" END_LINE "\n";
	static const char text[] =
	    "intro\n" BEGIN_LINE "\nZm9v\n" END_LINE "\n"
	    "-----BEGIN TRUSTED CERTIFICATE-----\nAAAA\n-----END TRUSTED CERTIFICATE-----\n"
	    "between\n"
	    "-----BEGIN PRIVATE KEY-----\nQUJD\n-----END PRIVATE KEY-----\n"
	    BEGIN_LINE "\nYmFy\n" END_LINE "\n"
	    "-----BEGIN X509 CRL-----\nQUJD\n-----END X509 CRL-----\n"
	    BEGIN_LINE "\nZm9vYg==\n" END_LINE "\n"
	    "end\n";
	static const char broken_text[] =
	    BEGIN_LINE "\nZm9v\n" END_LINE "\n"
	    BEGIN_LINE "\nZm9v!\n" END_LINE "\n"
	    BEGIN_LINE "\nYmFy\n" END_LINE "\n"
	    BEGIN_LINE "\nQUJD\n"
	    BEGIN_LINE "\nRA==\n" END_LINE "\n";
	char *copy;
	size_t size;
	size_t position;
	size_t previous;
	size_t der_size;
	uint8_t *der;
	int error;
	unsigned count;

	/* The first position is past the first block's END line. */
	check_next(first_text, 0, 0, strlen(first_text), "foo");

	/* Three certificates, then no more: ENOENT leaves the position at the end. */
	size = strlen(text);
	copy = dup_exact(text, size);
	position = 0;
	error = cryc_pem_next_certificate(copy, size, &position, &der, &der_size);
	CHECK(error == 0);
	CHECK(der_size == 3U && memcmp(der, "foo", 3) == 0);
	free(der);
	previous = position;
	error = cryc_pem_next_certificate(copy, size, &position, &der, &der_size);
	CHECK(error == 0);
	CHECK(position > previous);
	CHECK(der_size == 3U && memcmp(der, "bar", 3) == 0);
	free(der);
	previous = position;
	error = cryc_pem_next_certificate(copy, size, &position, &der, &der_size);
	CHECK(error == 0);
	CHECK(position > previous);
	CHECK(der_size == 4U && memcmp(der, "foob", 4) == 0);
	free(der);
	error = cryc_pem_next_certificate(copy, size, &position, &der, &der_size);
	CHECK(error == ENOENT);
	CHECK(position == size);
	CHECK(der == NULL);
	CHECK(der_size == 0U);

	/* A call at the end, and past the end, finds nothing and puts the position at the end. */
	position = size;
	CHECK(cryc_pem_next_certificate(copy, size, &position, &der, &der_size) == ENOENT);
	CHECK(position == size);
	position = size + 1000U;
	CHECK(cryc_pem_next_certificate(copy, size, &position, &der, &der_size) == ENOENT);
	CHECK(position == size);
	CHECK(der == NULL);
	free(copy);

	/* Empty text, with and without a pointer. */
	position = 0;
	CHECK(cryc_pem_next_certificate("", 0, &position, &der, &der_size) == ENOENT);
	CHECK(position == 0U);
	CHECK(der == NULL);
	position = 0;
	CHECK(cryc_pem_next_certificate(NULL, 0, &position, &der, &der_size) == ENOENT);

	/* A caller that goes on after an error finds the good blocks: foo, (broken), bar, (no END), RA==. */
	size = strlen(broken_text);
	copy = dup_exact(broken_text, size);
	position = 0;
	count = 0;
	for (;;) {
		previous = position;
		error = cryc_pem_next_certificate(copy, size, &position, &der, &der_size);
		if (error == ENOENT)
			break;
		CHECK(position > previous);
		if (error == 0) {
			CHECK(count < 3U);
			if (count == 0U)
				CHECK(der_size == 3U && memcmp(der, "foo", 3) == 0);
			if (count == 1U)
				CHECK(der_size == 3U && memcmp(der, "bar", 3) == 0);
			if (count == 2U)
				CHECK(der_size == 1U && memcmp(der, "D", 1) == 0);
			count++;
			free(der);
		} else {
			CHECK(error == EINVAL);
			CHECK(der == NULL);
		}
	}
	free(copy);
	CHECK(count == 3U);
}

/* A position in the middle of a block starts the search there. */
static void
test_pem_positions(void)
{
	static const char text[] = BEGIN_LINE "\nZm9v\n" END_LINE "\n" BEGIN_LINE "\nYmFy\n" END_LINE "\n";
	size_t after_begin;
	size_t second;

	/* Starting after the first BEGIN line the first block is gone: its END line is explanatory text. */
	after_begin = strlen(BEGIN_LINE "\n");
	second = strlen(BEGIN_LINE "\nZm9v\n" END_LINE "\n");
	check_next(text, after_begin, 0, strlen(text), "bar");

	/* Starting at the second block, and in the middle of its BEGIN line. */
	check_next(text, second, 0, strlen(text), "bar");
	check_next(text, second + 5U, ENOENT, strlen(text), NULL);

	/* Starting at the line end of the first BEGIN line: that BEGIN line is already behind. */
	check_next(text, strlen(BEGIN_LINE), 0, strlen(text), "bar");
}

/* The two certificate examples of RFC 7468 decode to well-formed DER of the length their first bytes give. */
static void
test_pem_examples(void)
{
	static const char *texts[] = {
		BEGIN_LINE "\n" FIG6_LINES END_LINE "\n",
		"Subject: CN=Atlantis\nIssuer: CN=Atlantis\nValidity: from 7/9/2012 3:10:38 AM UTC to 7/9/2013 3:10:37 AM UTC\n"
		    BEGIN_LINE "\n" FIG7_LINES END_LINE "\n"
	};
	static const size_t expected_size[] = { 560U, 413U };
	struct cryc_der reader;
	struct cryc_der_element element;
	char *copy;
	size_t size;
	size_t position;
	size_t der_size;
	size_t index;
	uint8_t *der;
	int error;

	/* Each example: the DER is one SEQUENCE whose length covers every byte (0x022c + 4 and 0x0199 + 4). */
	for (index = 0; index < 2U; index++) {
		size = strlen(texts[index]);
		copy = dup_exact(texts[index], size);
		position = 0;
		error = cryc_pem_next_certificate(copy, size, &position, &der, &der_size);
		CHECK(error == 0);
		CHECK(position == size);
		CHECK(der_size == expected_size[index]);
		if (error == 0) {
			cryc_der_init(&reader, der, der_size);
			CHECK(cryc_der_expect(&reader, CRYC_DER_SEQUENCE, &element) == 0);
			CHECK(cryc_der_empty(&reader));
			CHECK(element.encoding_size == der_size);
			free(der);
		}
		free(copy);
	}
}

/* Random mixes of PEM fragments: the reader always ends, the position always advances, and results obey their contract. */
static void
test_pem_random(void)
{
	static const char *fragments[] = {
		"-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----", "-----BEGIN PRIVATE KEY-----", "-----END PRIVATE KEY-----",
		"-----BEGIN ", "-----END ", "-----", "CERTIFICATE", "\n", "\r\n", "\r", "Zm9v", "Zg==", "YmFy", "!", " ", "\t", "-",
		"----", "BEGIN", "text", "QUJD\nRA==", "="
	};
	char text[512];
	size_t size;
	size_t choice;
	size_t round;
	size_t count;
	size_t position;
	size_t previous;
	size_t der_size;
	uint8_t *der;
	char *copy;
	int error;
	int bad;
	int steps;

	/* Each text is a handful of fragments and sometimes a NUL byte; every call is checked. */
	bad = 0;
	for (round = 0; round < 40000U; round++) {
		size = 0;
		count = random32() % 14U;
		while (count-- > 0U) {
			choice = random32() % (sizeof(fragments) / sizeof(fragments[0]) + 1U);
			if (choice == sizeof(fragments) / sizeof(fragments[0])) {
				text[size++] = '\0';
				continue;
			}
			memcpy(text + size, fragments[choice], strlen(fragments[choice]));
			size += strlen(fragments[choice]);
		}
		copy = dup_exact(text, size);
		position = 0;
		for (steps = 0; steps < 64; steps++) {
			previous = position;
			error = cryc_pem_next_certificate(copy, size, &position, &der, &der_size);
			if (position > size)
				bad++;
			if (error == 0) {
				if (der == NULL || der_size == 0U || position <= previous)
					bad++;
				free(der);
				continue;
			}
			if (der != NULL || der_size != 0U)
				bad++;
			if (error == ENOENT) {
				if (position != size)
					bad++;
				break;
			}
			if (error != EINVAL || position <= previous)
				bad++;
		}
		if (steps == 64)
			bad++;
		free(copy);
	}
	CHECK(bad == 0);
}

/* The PEM text and the DER of the public keys of the Wycheproof files agree (another label: not read as a certificate). */
static void
test_wycheproof_pem(
	const char *name,
	unsigned long *keys)
{
	struct vec_file file;
	unsigned char der[8192];
	unsigned char decoded[8192];
	char text[16384];
	const char *piece;
	size_t size;
	size_t used;
	size_t written;
	size_t position;
	size_t der_size;
	size_t first;
	size_t line;
	size_t body_end;
	uint8_t *certificate;
	unsigned long wrong;
	unsigned long seen;
	int error;

	/* The record with the key holds the PEM as a first line in a value and the rest as lines without " = ". */
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return;
	}
	wrong = 0;
	seen = 0;
	while (vec_next(&file)) {
		if (vec_get(&file, "group.publicKeyDer") == NULL || vec_get(&file, "group.publicKeyPem") == NULL)
			continue;
		size = vec_hex(&file, "group.publicKeyDer", der, sizeof(der));
		if (size == (size_t)-1) {
			wrong++;
			continue;
		}

		/* The lines of the PEM text, from the field to the END line. */
		for (first = 0; first < file.count; first++) {
			if (strcmp(file.keys[first], "group.publicKeyPem") == 0)
				break;
		}
		used = 0;
		for (line = first; line < file.count; line++) {
			piece = line == first ? file.values[line] : file.keys[line];
			if (used + strlen(piece) + 2U > sizeof(text))
				break;
			memcpy(text + used, piece, strlen(piece));
			used += strlen(piece);
			text[used++] = '\n';
			if (strncmp(piece, "-----END", 8) == 0)
				break;
		}
		seen++;

		/* A PUBLIC KEY block is not a certificate. */
		position = 0;
		error = cryc_pem_next_certificate(text, used, &position, &certificate, &der_size);
		if (error != ENOENT || position != used || certificate != NULL)
			wrong++;

		/* Its base64 (the lines between BEGIN and END) is the DER the file gives. */
		if (used < 27U || strncmp(text, "-----BEGIN PUBLIC KEY-----\n", 27) != 0) {
			wrong++;
			continue;
		}
		for (body_end = 27; body_end + 8U < used; body_end++) {
			if (strncmp(text + body_end, "-----END", 8) == 0)
				break;
		}
		error = cryc_base64_decode(text + 27, body_end - 27U, decoded, sizeof(decoded), &written);
		if (error != 0 || written != size || memcmp(decoded, der, size) != 0)
			wrong++;
	}
	vec_close(&file);
	printf("  %s: %lu PEM keys, %lu wrong\n", name, seen, wrong);
	CHECK(seen > 0UL);
	CHECK(wrong == 0UL);
	*keys += seen;
}

/* Reads a whole file with "rb"; returns a malloc'd buffer (NUL after the data) or NULL. */
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

/*
 * Reads every certificate of the ca-certificates bundle with the reader and
 * with a second method written here: the BEGIN and END lines found with
 * strstr, the lines between them joined, and the DER re-encoded with the
 * test's encoder must give the same characters.
 */
static void
test_bundle(void)
{
	const char *path;
	char *text;
	char *joined;
	char *encoded;
	const char *begin;
	const char *end;
	const char *line;
	size_t size;
	size_t position;
	size_t der_size;
	size_t joined_size;
	size_t length;
	unsigned long count;
	unsigned long wrong;
	unsigned long expected;
	uint8_t *der;
	struct cryc_der reader;
	struct cryc_der_element element;
	int error;

	/* The bundle must be readable: a missing file is a failure, not a pass. */
	path = getenv("WS335_CACERT");
	if (path == NULL)
		path = CACERT_DEFAULT;
	text = read_file(path, &size);
	CHECK(text != NULL);
	if (text == NULL)
		return;
	joined = malloc(size + 1U);
	encoded = malloc(size * 2U + 16U);
	CHECK(joined != NULL && encoded != NULL);
	if (joined == NULL || encoded == NULL)
		return;

	/* Every block, in order, against the bundle's own text. */
	position = 0;
	count = 0;
	wrong = 0;
	expected = 0;
	begin = strstr(text, BEGIN_LINE);
	while (begin != NULL) {
		expected++;
		end = strstr(begin, END_LINE);
		if (end == NULL) {
			wrong++;
			break;
		}

		/* The characters between the BEGIN line and the END line, without line ends. */
		joined_size = 0;
		for (line = begin + strlen(BEGIN_LINE); line < end; line++) {
			if (*line != '\n' && *line != '\r' && *line != ' ')
				joined[joined_size++] = *line;
		}
		joined[joined_size] = '\0';

		/* The reader's answer. */
		error = cryc_pem_next_certificate(text, size, &position, &der, &der_size);
		if (error != 0) {
			wrong++;
			begin = strstr(end, BEGIN_LINE);
			continue;
		}
		count++;
		length = encode(der, der_size, encoded, 0, "");
		if (length != joined_size || memcmp(encoded, joined, joined_size) != 0)
			wrong++;
		cryc_der_init(&reader, der, der_size);
		if (cryc_der_expect(&reader, CRYC_DER_SEQUENCE, &element) != 0 || !cryc_der_empty(&reader))
			wrong++;
		free(der);
		begin = strstr(end, BEGIN_LINE);
	}
	error = cryc_pem_next_certificate(text, size, &position, &der, &der_size);
	CHECK(error == ENOENT);
	CHECK(expected > 100UL);
	CHECK(count == expected);
	CHECK(wrong == 0UL);
	printf("  bundle: %lu certificates\n", count);
	free(joined);
	free(encoded);
	free(text);
}

/* The certificates of the x509-limbo cases, wrapped in PEM here with several line widths and ends, read back. */
static void
test_limbo_pem(void)
{
	static const size_t wraps[] = { 64, 76, 0, 16 };
	static const char *eols[] = { "\n", "\r\n", "\n", "\r" };
	struct vec_file file;
	unsigned char *der;
	char *body;
	char *text;
	size_t size;
	size_t position;
	size_t der_size;
	size_t used;
	uint8_t *read;
	unsigned long count;
	unsigned long wrong;
	int error;

	/* Room for the largest certificate of the cases (about 78 KB), its text and the lines around it. */
	der = malloc(LIMBO_MAX);
	body = malloc(LIMBO_MAX * 2U);
	text = malloc(LIMBO_MAX * 2U + 128U);
	CHECK(der != NULL && body != NULL && text != NULL);
	if (der == NULL || body == NULL || text == NULL)
		return;

	/* The peer certificate of every case, as "text", BEGIN, body, END. */
	if (vec_open(&file, "limbo") != 0) {
		CHECK(0);
		return;
	}
	count = 0;
	wrong = 0;
	while (vec_next(&file)) {
		size = vec_hex(&file, "peer", der, LIMBO_MAX);
		if (size == (size_t)-1) {
			wrong++;
			continue;
		}
		encode(der, size, body, wraps[count % 4U], eols[count % 4U]);
		used = 0;
		memcpy(text + used, "text\n", 5);
		used += 5U;
		memcpy(text + used, BEGIN_LINE, strlen(BEGIN_LINE));
		used += strlen(BEGIN_LINE);
		memcpy(text + used, eols[count % 4U], strlen(eols[count % 4U]));
		used += strlen(eols[count % 4U]);
		memcpy(text + used, body, strlen(body));
		used += strlen(body);
		if (wraps[count % 4U] == 0U) {
			memcpy(text + used, eols[count % 4U], strlen(eols[count % 4U]));
			used += strlen(eols[count % 4U]);
		}
		memcpy(text + used, END_LINE, strlen(END_LINE));
		used += strlen(END_LINE);
		position = 0;
		error = cryc_pem_next_certificate(text, used, &position, &read, &der_size);
		if (error != 0 || der_size != size || memcmp(read, der, size) != 0 || position != used)
			wrong++;
		if (error == 0)
			free(read);
		count++;
	}
	vec_close(&file);
	free(der);
	free(body);
	free(text);
	CHECK(count == 9792UL);
	CHECK(wrong == 0UL);
}

/* Runs the checks. */
int
main(void)
{
	unsigned long keys;

	/* Base64. */
	test_rfc4648_vectors();
	test_alphabet();
	test_padding();
	test_tails();
	test_whitespace();
	test_room();
	test_roundtrip_and_random();
	test_large();

	/* PEM. */
	test_pem_forms();
	test_pem_lookalikes();
	test_pem_broken();
	test_pem_sequences();
	test_pem_positions();
	test_pem_examples();
	test_pem_random();
	keys = 0;
	test_wycheproof_pem("wycheproof-ecdsa_secp256r1_sha256_test", &keys);
	test_wycheproof_pem("wycheproof-ed25519_test", &keys);
	test_wycheproof_pem("wycheproof-rsa_signature_2048_sha256_test", &keys);
	test_wycheproof_pem("wycheproof-rsa_pss_4096_sha512_mgf1_64_test", &keys);
	CHECK(keys > 100UL);
	test_limbo_pem();
	test_bundle();
	return check_finish("test-pem");
}
