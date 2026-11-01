/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the verification of a signature by a public key whatever the
 * algorithm (cryc-sigver.h): the DER decoding of an ECDSA signature and
 * cryc_signature_verify for RSA PKCS#1 v1.5, RSA-PSS and ECDSA.
 *
 * The decoder is compared with a second, independent strict DER reader
 * written here from ITU-T X.690 8.1, 8.3 and 10 and checked against
 * signatures built by derwriter.h (every width, the sign octet, values
 * of every length, long-form lengths for wide curves) and against
 * malformed encodings of every kind (truncated at every length, trailing
 * bytes, wrong tags, negative, non-minimal, zero, wide, overlong
 * lengths), and by random changes of valid encodings.
 *
 * The verification is driven by the published vectors: Wycheproof ECDSA
 * (P-256 and P-384), Wycheproof RSA PKCS#1 v1.5 and RSASSA-PSS, and the
 * CAVP ECDSA SigVer cases (P-256 and P-384 with every one of SHA-256,
 * SHA-384 and SHA-512, the combinations X.509 allows).  Then by what
 * happens to a valid signature when one thing changes (the algorithm, the
 * hash, the salt length, a bit of the message, of the signature, of the
 * key; the key's size, its exponent, its point), every error of the header
 * and the order the errors are checked in, and random input.  Real
 * certificates: the self-signature of every root of the ca-certificates
 * bundle (SHA-1 and unsupported keys are counted as ENOTSUP), those
 * signatures changed in one bit, and the issuer relations of the
 * x509-limbo certificates.
 *
 * Expected values come from the standards' and projects' vectors, the
 * bundle and limbo files, and the reader written here, not from another
 * implementation.
 */

#include "check.h"
#include "vecfile.h"
#include "derwriter.h"

#include "userland/base/libcrypto-compat/cryc-ecp.h"
#include "userland/base/libcrypto-compat/cryc-hash.h"
#include "userland/base/libcrypto-compat/cryc-pem.h"
#include "userland/base/libcrypto-compat/cryc-sigver.h"

/* Where the bundle is looked for when WS335_CACERT is not set. */
#define CACERT_DEFAULT	"/home/awe/zedBSD-claude1/build/distfiles/cacert-2026-08-13.pem"

/* The largest certificate of the x509-limbo cases is about 78 KB. */
#define LIMBO_MAX	(256U * 1024U)

/* The room for a message, a signature and a modulus of a vector. */
#define MESSAGE_MAX	8192U
#define SIGNATURE_MAX	1100U
#define MODULUS_MAX	1100U

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x1d872b41U;

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

/* Fills out with size pseudo-random bytes. */
static void
random_bytes(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte of each word. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random32() >> 11);
}

/* Forward declarations. */
static size_t modulus_bits(const struct cryc_rsa_key *key);

/* The counts printed at the end. */
static unsigned long count_ecdsa_wycheproof;
static unsigned long count_ecdsa_wycheproof_valid;
static unsigned long count_ecdsa_cavp;
static unsigned long count_ecdsa_cavp_valid;
static unsigned long count_rsa_wycheproof;
static unsigned long count_rsa_wycheproof_valid;
static unsigned long count_decode_cases;
static unsigned long count_bundle_ok;
static unsigned long count_bundle_notsup;
static unsigned long count_bundle_changed;
static unsigned long count_limbo_pairs;
static unsigned long count_limbo_ok;

/* Verifies with the algorithm alg (and the salt length for PSS). */
static int
verify(
	const struct cryc_pubkey *key,
	enum cryc_sig_alg alg,
	size_t salt_size,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *signature,
	size_t signature_size)
{
	struct cryc_sig_params params;

	/* The parameters, then the call. */
	params.alg = alg;
	params.salt_size = salt_size;
	return cryc_signature_verify(key, &params, message, message_size, signature, signature_size);
}

/* Maps a hash's name as the vectors write it to an index 0, 1, 2 (SHA-256, SHA-384, SHA-512), or -1. */
static int
hash_index(
	const char *name)
{
	/* The three hashes of the module. */
	if (strcmp(name, "SHA-256") == 0 || strcmp(name, "SHA256") == 0)
		return 0;
	if (strcmp(name, "SHA-384") == 0 || strcmp(name, "SHA384") == 0)
		return 1;
	if (strcmp(name, "SHA-512") == 0 || strcmp(name, "SHA512") == 0)
		return 2;
	return -1;
}

/* The algorithms by hash index. */
static const enum cryc_sig_alg pkcs1_alg[3] = { CRYC_SIG_RSA_PKCS1_SHA256, CRYC_SIG_RSA_PKCS1_SHA384, CRYC_SIG_RSA_PKCS1_SHA512 };
static const enum cryc_sig_alg pss_alg[3] = { CRYC_SIG_RSA_PSS_SHA256, CRYC_SIG_RSA_PSS_SHA384, CRYC_SIG_RSA_PSS_SHA512 };
static const enum cryc_sig_alg ecdsa_alg[3] = { CRYC_SIG_ECDSA_SHA256, CRYC_SIG_ECDSA_SHA384, CRYC_SIG_ECDSA_SHA512 };

/* The digest sizes by hash index. */
static const size_t hash_bytes[3] = { 32U, 48U, 64U };

/*
 * The prime p of each curve as bytes.
 *
 * Values referenced: NIST SP 800-186 (February 2023) 3.2.1.3 (P-256) and
 * 3.2.1.4 (P-384).
 */
static const char *const prime_hex[2] = {
	"ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
	"fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff"
};

/* ------------------------------------------------------------------ */
/* The independent reader of an Ecdsa-Sig-Value. */

/*
 * Reads a DER length at *position of data (size bytes): the short form
 * below 128, else the long form in the fewest octets (no leading zero
 * octet, and not a value below 128).  Returns 0 and sets *length and
 * advances *position, or -1.
 */
static int
ref_length(
	const uint8_t *data,
	size_t size,
	size_t *position,
	size_t *length)
{
	size_t octets;
	size_t value;
	size_t index;

	/* One octet at least. */
	if (*position >= size)
		return -1;
	if (data[*position] < 0x80U) {
		*length = data[*position];
		*position += 1U;
		return 0;
	}

	/* The long form: 0x80 alone (indefinite) and 0xff are not DER. */
	octets = (size_t)(data[*position] & 0x7fU);
	if (octets == 0U || octets > 4U || *position + 1U + octets > size)
		return -1;
	if (data[*position + 1U] == 0U)
		return -1;
	value = 0;
	for (index = 0; index < octets; index++)
		value = (value << 8) | data[*position + 1U + index];
	if (value < 128U)
		return -1;
	*position += 1U + octets;
	*length = value;
	return 0;
}

/*
 * Reads one INTEGER of an Ecdsa-Sig-Value at *position: tag 0x02, a DER
 * length, minimal and positive contents, at least 1, with no more than
 * size significant bytes; writes it right-aligned in out (size bytes).
 * Returns 0 or -1.
 */
static int
ref_integer(
	const uint8_t *data,
	size_t end,
	size_t *position,
	size_t size,
	uint8_t *out)
{
	size_t length;
	size_t start;
	size_t significant;

	/* The tag and the length. */
	if (*position >= end || data[*position] != 0x02U)
		return -1;
	*position += 1U;
	if (ref_length(data, end, position, &length) != 0)
		return -1;
	if (length == 0U || length > end - *position)
		return -1;
	start = *position;
	*position += length;

	/* Positive, and minimal: no first byte 0x00 before a byte below 0x80, no 0xff before one at 0x80. */
	if ((data[start] & 0x80U) != 0U)
		return -1;
	if (length > 1U && data[start] == 0U && (data[start + 1U] & 0x80U) == 0U)
		return -1;

	/* The significant bytes: not zero, and they fit. */
	significant = length;
	if (data[start] == 0U) {
		start++;
		significant--;
	}
	if (significant == 0U)
		return -1;
	if (significant > size)
		return -1;
	memset(out, 0, size);
	memcpy(out + (size - significant), data + start, significant);
	return 0;
}

/* Reads a whole Ecdsa-Sig-Value: returns 0 and r, s (size bytes each), or -1. */
static int
ref_decode(
	const uint8_t *data,
	size_t data_size,
	size_t size,
	uint8_t *r,
	uint8_t *s)
{
	size_t position;
	size_t length;
	size_t end;

	/* The SEQUENCE covers the whole input. */
	position = 0;
	if (data_size < 2U || data[0] != 0x30U)
		return -1;
	position = 1;
	if (ref_length(data, data_size, &position, &length) != 0)
		return -1;
	if (length != data_size - position)
		return -1;
	end = data_size;

	/* Two integers and nothing else. */
	if (ref_integer(data, end, &position, size, r) != 0)
		return -1;
	if (ref_integer(data, end, &position, size, s) != 0)
		return -1;
	if (position != end)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* The decoding of an ECDSA signature. */

/* Builds SEQUENCE { INTEGER r, INTEGER s } from the magnitudes with derwriter; returns the size, or 0 on a writer error. */
static size_t
build_signature(
	const uint8_t *r,
	size_t r_size,
	const uint8_t *s,
	size_t s_size,
	uint8_t *out,
	size_t room)
{
	uint8_t inner_bytes[400];
	struct der_writer inner;
	struct der_writer outer;

	/* The two integers, then the sequence around them. */
	der_init(&inner, inner_bytes, sizeof(inner_bytes));
	der_unsigned(&inner, r, r_size);
	der_unsigned(&inner, s, s_size);
	der_init(&outer, out, room);
	der_wrap(&outer, 0x30U, &inner);
	if (outer.error)
		return 0;
	return outer.used;
}

/* Calls the decoder on a buffer with sentinel fills and checks it against the reader written here. */
static int
decode_against_reference(
	const uint8_t *sig,
	size_t sig_size,
	size_t size)
{
	uint8_t got_r[80];
	uint8_t got_s[80];
	uint8_t ref_r[80];
	uint8_t ref_s[80];
	int error;
	int reference;

	/* Sentinels show whether anything was written. */
	memset(got_r, 0x5a, sizeof(got_r));
	memset(got_s, 0xa5, sizeof(got_s));
	error = cryc_ecdsa_signature_decode(sig, sig_size, size, got_r, got_s);
	reference = size >= 1U && size <= sizeof(ref_r) ? ref_decode(sig, sig_size, size, ref_r, ref_s) : -1;
	count_decode_cases++;
	if (reference == 0) {
		CHECK(error == 0);
		if (error == 0) {
			CHECK_BYTES(got_r, ref_r, size);
			CHECK_BYTES(got_s, ref_s, size);
		}
		return error;
	}

	/* Refused with EBADMSG, and nothing written. */
	CHECK(error == EBADMSG);
	CHECK(got_r[0] == 0x5aU && got_r[size < sizeof(got_r) ? size : 0] == 0x5aU);
	CHECK(got_s[0] == 0xa5U && got_s[size < sizeof(got_s) ? size : 0] == 0xa5U);
	return error;
}

/* Checks a signature made from chosen magnitudes: round trip, and every prefix and extension refused. */
static void
check_decode_value(
	const uint8_t *r,
	size_t r_size,
	const uint8_t *s,
	size_t s_size,
	size_t size)
{
	uint8_t sig[400];
	uint8_t padded[400];
	uint8_t out_r[80];
	uint8_t out_s[80];
	size_t sig_size;
	size_t length;
	size_t strip_r;
	size_t strip_s;
	size_t index;
	int error;

	/* The encoding, and what the decoder must give: the magnitudes stripped of leading zeros, padded to size. */
	sig_size = build_signature(r, r_size, s, s_size, sig, sizeof(sig));
	CHECK(sig_size > 0U);
	if (sig_size == 0U)
		return;
	for (strip_r = 0; strip_r + 1U < r_size && r[strip_r] == 0U; strip_r++)
		continue;
	for (strip_s = 0; strip_s + 1U < s_size && s[strip_s] == 0U; strip_s++)
		continue;
	error = decode_against_reference(sig, sig_size, size);
	if (r_size - strip_r <= size && s_size - strip_s <= size && !(r_size - strip_r == 1U && r[strip_r] == 0U) && !(s_size - strip_s == 1U && s[strip_s] == 0U)) {
		CHECK(error == 0);
		memset(out_r, 0x11, sizeof(out_r));
		memset(out_s, 0x22, sizeof(out_s));
		error = cryc_ecdsa_signature_decode(sig, sig_size, size, out_r, out_s);
		CHECK(error == 0);
		memset(padded, 0, sizeof(padded));
		memcpy(padded + (size - (r_size - strip_r)), r + strip_r, r_size - strip_r);
		CHECK_BYTES(out_r, padded, size);
		memset(padded, 0, sizeof(padded));
		memcpy(padded + (size - (s_size - strip_s)), s + strip_s, s_size - strip_s);
		CHECK_BYTES(out_s, padded, size);

		/* Every proper prefix is refused (truncated input), and so is the encoding with a byte after it. */
		for (length = 0; length < sig_size; length++)
			CHECK(decode_against_reference(sig, length, size) == EBADMSG);
		sig[sig_size] = 0x00U;
		CHECK(decode_against_reference(sig, sig_size + 1U, size) == EBADMSG);

		/* A single byte changed anywhere is accepted only when the reference accepts it. */
		for (index = 0; index < sig_size; index++) {
			sig[index] ^= 0x41U;
			(void)decode_against_reference(sig, sig_size, size);
			sig[index] ^= 0x41U;
		}
	} else {
		CHECK(error == EBADMSG);
	}
}

/* Checks the decoder on built signatures of every width, and on the hand-made malformed ones. */
static void
check_decode(void)
{
	static const size_t widths[8] = { 1U, 2U, 20U, 32U, 48U, 64U, 66U, 70U };
	uint8_t value_a[80];
	uint8_t value_b[80];
	uint8_t sig[400];
	uint8_t out_r[80];
	uint8_t out_s[80];
	size_t width;
	size_t sig_size;
	size_t index;
	size_t shape;
	size_t which;
	unsigned iteration;
	int error;

	/* Values of each width: all ones, the top bit set (the sign octet), the top bit clear, one, leading zeros in the magnitude. */
	for (width = 0; width < 8U; width++) {
		for (shape = 0; shape < 6U; shape++) {
			memset(value_a, 0xffU, widths[width]);
			memset(value_b, 0x00U, widths[width]);
			value_b[widths[width] - 1U] = 0x01U;
			if (shape == 1U) {
				memset(value_a, 0x00U, widths[width]);
				value_a[0] = 0x80U;
				memset(value_b, 0x00U, widths[width]);
				value_b[0] = 0x7fU;
			} else if (shape == 2U) {
				memset(value_a, 0x00U, widths[width]);
				value_a[0] = 0x01U;
				memset(value_b, 0xffU, widths[width]);
				value_b[0] = 0x7fU;
			} else if (shape == 3U) {
				random_bytes(value_a, widths[width]);
				random_bytes(value_b, widths[width]);
			} else if (shape == 4U) {
				random_bytes(value_a, widths[width]);
				random_bytes(value_b, widths[width]);
				value_a[0] = 0x00U;
				value_b[0] = 0x00U;
				if (widths[width] > 1U) {
					value_a[1] = 0x00U;
					value_b[1] = 0x00U;
				}
				value_a[widths[width] - 1U] |= 0x01U;
				value_b[widths[width] - 1U] |= 0x01U;
			}
			for (which = 0; which < 8U; which++) {
				/* Each field width against the width of the values (narrower, equal, wider). */
				check_decode_value(value_a, widths[width], value_b, widths[width], widths[which]);
			}
		}
	}

	/* Small magnitudes: the single values around the sign octet and the zero. */
	for (index = 0; index < 4U; index++) {
		static const uint8_t singles[4] = { 0x00U, 0x01U, 0x7fU, 0x80U };
		uint8_t one;

		one = singles[index];
		check_decode_value(&one, 1U, &one, 1U, 1U);
		check_decode_value(&one, 1U, &one, 1U, 32U);
		check_decode_value(&one, 1U, &singles[1], 1U, 32U);
		check_decode_value(&singles[1], 1U, &one, 1U, 32U);
	}

	/* The hand-made malformed encodings, each against the reader and refused for the reason. */
	{
		static const uint8_t no_sequence[] = { 0x31, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01 };
		static const uint8_t octet_ints[] = { 0x30, 0x06, 0x04, 0x01, 0x01, 0x04, 0x01, 0x01 };
		static const uint8_t one_int[] = { 0x30, 0x03, 0x02, 0x01, 0x01 };
		static const uint8_t three_ints[] = { 0x30, 0x09, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01 };
		static const uint8_t negative_r[] = { 0x30, 0x06, 0x02, 0x01, 0x80, 0x02, 0x01, 0x01 };
		static const uint8_t negative_s[] = { 0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0xff };
		static const uint8_t padded_r[] = { 0x30, 0x07, 0x02, 0x02, 0x00, 0x01, 0x02, 0x01, 0x01 };
		static const uint8_t padded_s[] = { 0x30, 0x07, 0x02, 0x01, 0x01, 0x02, 0x02, 0x00, 0x7f };
		static const uint8_t zero_r[] = { 0x30, 0x06, 0x02, 0x01, 0x00, 0x02, 0x01, 0x01 };
		static const uint8_t zero_s[] = { 0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00 };
		static const uint8_t zero_both[] = { 0x30, 0x06, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00 };
		static const uint8_t empty_int[] = { 0x30, 0x05, 0x02, 0x00, 0x02, 0x01, 0x01 };
		static const uint8_t long_form[] = { 0x30, 0x81, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01 };
		static const uint8_t long_int[] = { 0x30, 0x07, 0x02, 0x81, 0x01, 0x01, 0x02, 0x01, 0x01 };
		static const uint8_t indefinite[] = { 0x30, 0x80, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00 };
		static const uint8_t too_long[] = { 0x30, 0x07, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01 };
		static const uint8_t too_short[] = { 0x30, 0x05, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01 };
		static const uint8_t int_overrun[] = { 0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x05, 0x01 };
		static const uint8_t high_tag[] = { 0x1f, 0x10, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01 };
		static const uint8_t trailing[] = { 0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x00 };
		static const uint8_t wide_r[] = { 0x30, 0x08, 0x02, 0x03, 0x01, 0x00, 0x00, 0x02, 0x01, 0x01 };
		static const uint8_t ok_small[] = { 0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x7f };
		static const uint8_t ok_signed[] = { 0x30, 0x08, 0x02, 0x02, 0x00, 0x80, 0x02, 0x02, 0x00, 0xff };

		CHECK(decode_against_reference(no_sequence, sizeof(no_sequence), 32U) == EBADMSG);
		CHECK(decode_against_reference(octet_ints, sizeof(octet_ints), 32U) == EBADMSG);
		CHECK(decode_against_reference(one_int, sizeof(one_int), 32U) == EBADMSG);
		CHECK(decode_against_reference(three_ints, sizeof(three_ints), 32U) == EBADMSG);
		CHECK(decode_against_reference(negative_r, sizeof(negative_r), 32U) == EBADMSG);
		CHECK(decode_against_reference(negative_s, sizeof(negative_s), 32U) == EBADMSG);
		CHECK(decode_against_reference(padded_r, sizeof(padded_r), 32U) == EBADMSG);
		CHECK(decode_against_reference(padded_s, sizeof(padded_s), 32U) == EBADMSG);
		CHECK(decode_against_reference(zero_r, sizeof(zero_r), 32U) == EBADMSG);
		CHECK(decode_against_reference(zero_s, sizeof(zero_s), 32U) == EBADMSG);
		CHECK(decode_against_reference(zero_both, sizeof(zero_both), 32U) == EBADMSG);
		CHECK(decode_against_reference(empty_int, sizeof(empty_int), 32U) == EBADMSG);
		CHECK(decode_against_reference(long_form, sizeof(long_form), 32U) == EBADMSG);
		CHECK(decode_against_reference(long_int, sizeof(long_int), 32U) == EBADMSG);
		CHECK(decode_against_reference(indefinite, sizeof(indefinite), 32U) == EBADMSG);
		CHECK(decode_against_reference(too_long, sizeof(too_long), 32U) == EBADMSG);
		CHECK(decode_against_reference(too_short, sizeof(too_short), 32U) == EBADMSG);
		CHECK(decode_against_reference(int_overrun, sizeof(int_overrun), 32U) == EBADMSG);
		CHECK(decode_against_reference(high_tag, sizeof(high_tag), 32U) == EBADMSG);
		CHECK(decode_against_reference(trailing, sizeof(trailing), 32U) == EBADMSG);
		CHECK(decode_against_reference(wide_r, sizeof(wide_r), 2U) == EBADMSG);
		CHECK(decode_against_reference(wide_r, sizeof(wide_r), 3U) == 0);
		CHECK(decode_against_reference(ok_small, sizeof(ok_small), 32U) == 0);
		CHECK(decode_against_reference(ok_small, sizeof(ok_small), 1U) == 0);
		CHECK(decode_against_reference(ok_signed, sizeof(ok_signed), 1U) == 0);

		/* The exact values of the accepted ones. */
		error = cryc_ecdsa_signature_decode(ok_signed, sizeof(ok_signed), 4U, out_r, out_s);
		CHECK(error == 0);
		CHECK(out_r[0] == 0U && out_r[1] == 0U && out_r[2] == 0U && out_r[3] == 0x80U);
		CHECK(out_s[0] == 0U && out_s[1] == 0U && out_s[2] == 0U && out_s[3] == 0xffU);
		error = cryc_ecdsa_signature_decode(ok_small, sizeof(ok_small), 2U, out_r, out_s);
		CHECK(error == 0);
		CHECK(out_r[0] == 0U && out_r[1] == 1U && out_s[0] == 0U && out_s[1] == 0x7fU);

		/* An empty input with a NULL pointer, and a field of no bytes. */
		CHECK(cryc_ecdsa_signature_decode(NULL, 0U, 32U, out_r, out_s) == EBADMSG);
		CHECK(cryc_ecdsa_signature_decode(ok_small, sizeof(ok_small), 0U, out_r, out_s) == EBADMSG);
	}

	/* Random changes of a valid signature: the decoder and the reader agree on each. */
	value_a[0] = 0x01U;
	random_bytes(value_a + 1, 31U);
	random_bytes(value_b, 32U);
	value_b[0] |= 0x80U;
	sig_size = build_signature(value_a, 32U, value_b, 32U, sig, sizeof(sig));
	CHECK(sig_size > 0U);
	for (iteration = 0; iteration < 40000U && sig_size > 0U; iteration++) {
		uint8_t mutated[400];
		size_t mutated_size;
		size_t count;

		memcpy(mutated, sig, sig_size);
		mutated_size = sig_size;
		count = 1U + random32() % 3U;
		while (count-- > 0U) {
			index = random32() % mutated_size;
			which = random32() % 4U;
			if (which == 0U)
				mutated[index] = (uint8_t)random32();
			else if (which == 1U)
				mutated[index] ^= (uint8_t)(1U << (random32() % 8U));
			else if (which == 2U)
				mutated[index] = (uint8_t)(random32() % 3U == 0U ? 0x00U : 0xffU);
			else if (mutated_size > 2U)
				mutated_size -= random32() % 2U;
		}
		(void)decode_against_reference(mutated, mutated_size, 32U);
		(void)decode_against_reference(mutated, mutated_size, 33U);
	}

	/* Pure random bytes of every length up to 80. */
	for (iteration = 0; iteration < 20000U; iteration++) {
		uint8_t noise[96];
		size_t noise_size;

		noise_size = random32() % 81U;
		random_bytes(noise, noise_size);
		if (noise_size > 2U && iteration % 2U == 0U) {
			noise[0] = 0x30U;
			noise[1] = (uint8_t)(noise_size - 2U);
		}
		(void)decode_against_reference(noise, noise_size, 32U);
	}
}

/* ------------------------------------------------------------------ */
/* The vectors. */

/* A key and one valid signature, copied from a vector, to be changed one thing at a time. */
struct fixture {
	struct cryc_pubkey key;
	enum cryc_sig_alg alg;
	size_t salt_size;
	uint8_t n[MODULUS_MAX];
	uint8_t e[16];
	uint8_t point[128];
	uint8_t message[512];
	size_t message_size;
	uint8_t signature[SIGNATURE_MAX];
	size_t signature_size;
	int loaded;
};

/* The fixtures taken from the vectors. */
static struct fixture fixture_pkcs1;
static struct fixture fixture_pkcs1_sha512;
static struct fixture fixture_pss;
static struct fixture fixture_p256;
static struct fixture fixture_p384;

/* Points a copy's key at the copy's own buffers. */
static void
fixture_relink(
	struct fixture *fixture)
{
	/* The views follow the copy. */
	fixture->key.rsa.n = fixture->n;
	fixture->key.rsa.e = fixture->e;
	fixture->key.point = fixture->point;
}

/* Strips the leading zero bytes of hex text into out; returns the size, or (size_t)-1. */
static size_t
hex_stripped(
	const char *text,
	uint8_t *out,
	size_t room)
{
	static uint8_t raw[MODULUS_MAX + 8U];
	static char padded[2U * (MODULUS_MAX + 8U) + 2U];
	size_t length;
	size_t skip;

	/* Decodes (a value of an odd number of digits gets a leading zero digit), then removes the sign octet and other leading zeros. */
	if (strlen(text) % 2U != 0U && strlen(text) < sizeof(padded) - 1U) {
		padded[0] = '0';
		strcpy(padded + 1, text);
		text = padded;
	}
	length = hex_decode(text, raw, sizeof(raw));
	if (length == (size_t)-1)
		return (size_t)-1;
	for (skip = 0; skip < length && raw[skip] == 0U; skip++)
		continue;
	if (length - skip > room)
		return (size_t)-1;
	memcpy(out, raw + skip, length - skip);
	return length - skip;
}

/* Remembers a valid case as a fixture when the fixture is empty and the message is small but not empty. */
static void
fixture_keep(
	struct fixture *fixture,
	const struct cryc_pubkey *key,
	enum cryc_sig_alg alg,
	size_t salt_size,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *signature,
	size_t signature_size)
{
	/* The first valid case with a message of 1 to 512 bytes. */
	if (fixture->loaded || message_size == 0U || message_size > sizeof(fixture->message))
		return;
	if (signature_size > sizeof(fixture->signature))
		return;
	fixture->key = *key;
	fixture->alg = alg;
	fixture->salt_size = salt_size;
	if (key->type == CRYC_KEY_RSA) {
		if (key->rsa.n_size > sizeof(fixture->n) || key->rsa.e_size > sizeof(fixture->e))
			return;
		memcpy(fixture->n, key->rsa.n, key->rsa.n_size);
		memcpy(fixture->e, key->rsa.e, key->rsa.e_size);
	} else {
		if (key->point_size > sizeof(fixture->point))
			return;
		memcpy(fixture->point, key->point, key->point_size);
	}
	memcpy(fixture->message, message, message_size);
	fixture->message_size = message_size;
	memcpy(fixture->signature, signature, signature_size);
	fixture->signature_size = signature_size;
	fixture_relink(fixture);
	fixture->loaded = 1;
}

/* Checks one Wycheproof ECDSA file through cryc_signature_verify. */
static void
check_ecdsa_wycheproof(
	const char *name)
{
	static uint8_t message[MESSAGE_MAX];
	static uint8_t signature[SIGNATURE_MAX * 4U];
	struct vec_file file;
	struct cryc_pubkey key;
	uint8_t point[256];
	const char *result;
	const char *sha;
	const char *curve;
	size_t message_size;
	size_t signature_size;
	size_t point_size;
	int index;
	int error;
	int other;
	unsigned long valid;
	unsigned long invalid;

	/* Opens the file. */
	valid = 0;
	invalid = 0;
	point_size = 0;
	memset(&key, 0, sizeof(key));
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return;
	}
	while (vec_next(&file)) {
		/* A group's key is in a record of its own, before the group's cases. */
		if (vec_get(&file, "group.publicKey.uncompressed") != NULL) {
			point_size = vec_hex(&file, "group.publicKey.uncompressed", point, sizeof(point));
			curve = vec_get(&file, "group.publicKey.curve");
			CHECK(point_size != (size_t)-1 && curve != NULL);
			memset(&key, 0, sizeof(key));
			key.type = CRYC_KEY_EC;
			key.curve = curve != NULL && strcmp(curve, "secp256r1") == 0 ? CRYC_CURVE_P256 : CRYC_CURVE_P384;
			key.point = point;
			key.point_size = point_size;
			continue;
		}
		result = vec_get(&file, "result");
		sha = vec_get(&file, "group.sha");
		message_size = vec_hex(&file, "msg", message, sizeof(message));
		signature_size = vec_hex(&file, "sig", signature, sizeof(signature));
		CHECK(result != NULL && sha != NULL && message_size != (size_t)-1);
		if (result == NULL || sha == NULL || message_size == (size_t)-1 || key.type != CRYC_KEY_EC)
			continue;
		index = hash_index(sha);
		CHECK(index >= 0);
		if (index < 0)
			continue;

		/* A signature too long to hold here is invalid by the file's own verdict, and is refused as one. */
		if (signature_size == (size_t)-1) {
			CHECK(strcmp(result, "invalid") == 0);
			continue;
		}
		count_ecdsa_wycheproof++;
		error = verify(&key, ecdsa_alg[index], 0U, message, message_size, signature, signature_size);
		if (message_size == 0U)
			CHECK(verify(&key, ecdsa_alg[index], 0U, NULL, 0U, signature, signature_size) == error);
		if (strcmp(result, "valid") == 0) {
			CHECK(error == 0);
			valid++;
			count_ecdsa_wycheproof_valid++;
			if (message_size > 0U && message_size <= 512U) {
				if (key.curve == CRYC_CURVE_P256 && index == 0)
					fixture_keep(&fixture_p256, &key, ecdsa_alg[index], 0U, message, message_size, signature, signature_size);
				if (key.curve == CRYC_CURVE_P384 && index == 1)
					fixture_keep(&fixture_p384, &key, ecdsa_alg[index], 0U, message, message_size, signature, signature_size);
			}

			/* The same signature under each other hash is a different digest, so it does not verify. */
			for (other = 0; other < 3; other++) {
				if (other != index && (valid % 8U) == 0U)
					CHECK(verify(&key, ecdsa_alg[other], 0U, message, message_size, signature, signature_size) == EBADMSG);
			}
		} else {
			CHECK(strcmp(result, "invalid") == 0);
			CHECK(error == EBADMSG);
			invalid++;
		}
	}
	vec_close(&file);
	printf("  %s: %lu valid, %lu invalid\n", name, valid, invalid);
	CHECK(valid > 100U);
	CHECK(invalid > 50U);
}

/* Copies a hex value of the record into out and returns its size with leading zeros stripped, or (size_t)-1. */
static size_t
record_number(
	const struct vec_file *file,
	const char *key,
	uint8_t *out,
	size_t room)
{
	const char *value;

	/* The field's text, stripped. */
	value = vec_get(file, key);
	if (value == NULL)
		return (size_t)-1;
	return hex_stripped(value, out, room);
}

/* Decodes a hex field as bytes without removing leading zeros (an odd number of digits gets a leading zero digit, as the CAVP files write numbers); returns the size or (size_t)-1. */
static size_t
record_bytes(
	const struct vec_file *file,
	const char *key,
	uint8_t *out,
	size_t room)
{
	static char padded[2U * MESSAGE_MAX + 4U];
	const char *value;
	size_t length;

	/* Even texts as they are, odd texts with a zero digit in front. */
	value = vec_get(file, key);
	if (value == NULL)
		return (size_t)-1;
	length = strlen(value);
	if (length % 2U == 0U)
		return hex_decode(value, out, room);
	if (length + 2U > sizeof(padded))
		return (size_t)-1;
	padded[0] = '0';
	memcpy(padded + 1, value, length + 1U);
	return hex_decode(padded, out, room);
}

/* Returns the last section key of the record that names a curve ("P-256,SHA-256"), or NULL. */
static const char *
record_section(
	const struct vec_file *file)
{
	size_t index;
	const char *key;

	/* From the last key backwards: the first section key that starts with a curve name. */
	for (index = file->count; index > 0U; index--) {
		key = file->keys[index - 1U];
		if (strncmp(key, "section.", 8) == 0 && key[9] == '-')
			return key + 8;
	}
	return NULL;
}

/* The count of valid CAVP cases per curve (P-256, P-384) and hash (SHA-256, SHA-384, SHA-512). */
static unsigned long cavp_valid[2][3];

/* Checks the CAVP ECDSA SigVer cases of P-256 and P-384 with SHA-256, SHA-384 and SHA-512. */
static void
check_ecdsa_cavp(void)
{
	static uint8_t message[MESSAGE_MAX];
	struct vec_file file;
	struct cryc_pubkey key;
	struct cryc_ecp ec;
	struct cryc_ecp_point point;
	uint8_t public_key[1U + 2U * 48U];
	uint8_t r[80];
	uint8_t s[80];
	uint8_t sig[400];
	const char *section;
	const char *result;
	size_t message_size;
	size_t size;
	size_t sig_size;
	size_t r_size;
	size_t s_size;
	size_t x_size;
	size_t y_size;
	int curve_index;
	int hash;
	int key_error;
	int error;
	unsigned long bad[2];

	/* The file. */
	bad[0] = 0;
	bad[1] = 0;
	if (vec_open(&file, "cavp-186-4ecdsatestvectors-SigVer") != 0) {
		CHECK(0);
		return;
	}
	while (vec_next(&file)) {
		/* A curve and a hash the module has. */
		section = record_section(&file);
		if (section == NULL)
			continue;
		if (strncmp(section, "P-256,", 6) == 0)
			curve_index = 0;
		else if (strncmp(section, "P-384,", 6) == 0)
			curve_index = 1;
		else
			continue;
		hash = hash_index(section + 6);
		if (hash < 0)
			continue;
		size = curve_index == 0 ? 32U : 48U;

		/* The fields. */
		message_size = vec_hex(&file, "Msg", message, sizeof(message));
		result = vec_get(&file, "Result");
		r_size = record_number(&file, "R", r, sizeof(r));
		s_size = record_number(&file, "S", s, sizeof(s));
		x_size = vec_get(&file, "Qx") != NULL ? hex_decode(vec_get(&file, "Qx"), public_key + 1, size) : (size_t)-1;
		y_size = vec_get(&file, "Qy") != NULL ? hex_decode(vec_get(&file, "Qy"), public_key + 1U + size, size) : (size_t)-1;
		CHECK(message_size != (size_t)-1 && result != NULL);
		CHECK(r_size != (size_t)-1 && s_size != (size_t)-1);
		CHECK(x_size == size && y_size == size);
		if (message_size == (size_t)-1 || result == NULL || r_size == (size_t)-1 || s_size == (size_t)-1 || x_size != size || y_size != size)
			continue;
		public_key[0] = 0x04U;
		memset(&key, 0, sizeof(key));
		key.type = CRYC_KEY_EC;
		key.curve = curve_index == 0 ? CRYC_CURVE_P256 : CRYC_CURVE_P384;
		key.point = public_key;
		key.point_size = 1U + 2U * size;

		/* r and s as they are (zero is still an INTEGER), built into DER by the writer. */
		if (r_size == 0U) {
			r[0] = 0;
			r_size = 1;
		}
		if (s_size == 0U) {
			s[0] = 0;
			s_size = 1;
		}
		sig_size = build_signature(r, r_size, s, s_size, sig, sizeof(sig));
		CHECK(sig_size > 0U);
		if (sig_size == 0U)
			continue;
		count_ecdsa_cavp++;
		error = verify(&key, ecdsa_alg[hash], 0U, message, message_size, sig, sig_size);
		if (result[0] == 'P') {
			CHECK(error == 0);
			cavp_valid[curve_index][hash]++;
			count_ecdsa_cavp_valid++;
		} else {
			/* A bad signature gives EBADMSG, or the refusal of the key when the key is not a point of the curve. */
			CHECK(result[0] == 'F');
			memset(&ec, 0, sizeof(ec));
			CHECK(cryc_ecp_init(&ec, key.curve) == 0);
			key_error = cryc_ecp_decode(&ec, &point, public_key, 1U + 2U * size);
			CHECK(error == (key_error != 0 ? EINVAL : EBADMSG));
			bad[curve_index]++;
		}
	}
	vec_close(&file);
	printf("  CAVP SigVer: %lu valid, %lu / %lu bad (P-256 / P-384)\n", count_ecdsa_cavp_valid, bad[0], bad[1]);
	for (curve_index = 0; curve_index < 2; curve_index++) {
		for (hash = 0; hash < 3; hash++)
			CHECK(cavp_valid[curve_index][hash] >= 3U);
	}
	CHECK(bad[0] >= 30U && bad[1] >= 30U);
}

/* State of the current group of a Wycheproof RSA file. */
struct rsa_group {
	uint8_t n[MODULUS_MAX];
	size_t n_size;
	uint8_t e[16];
	size_t e_size;
	char sha[16];
	size_t salt_size;
	int have_salt_size;
};

/* Checks one Wycheproof RSA file: kind 0 is PKCS#1 v1.5, kind 1 is PSS. */
static void
check_rsa_wycheproof(
	const char *name,
	int kind)
{
	static uint8_t message[MESSAGE_MAX];
	static uint8_t signature[SIGNATURE_MAX * 4U];
	struct vec_file file;
	struct rsa_group group;
	struct cryc_pubkey key;
	const char *value;
	const char *result;
	const char *flags;
	size_t message_size;
	size_t signature_size;
	size_t size;
	int index;
	int error;
	unsigned long valid;
	unsigned long total;

	/* Opens the file. */
	valid = 0;
	total = 0;
	memset(&group, 0, sizeof(group));
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return;
	}
	while (vec_next(&file)) {
		/* The group's fields come before the case's, in this record or an earlier one. */
		value = vec_get(&file, "group.publicKey.modulus");
		if (value != NULL) {
			size = hex_stripped(value, group.n, sizeof(group.n));
			CHECK(size != (size_t)-1);
			group.n_size = size == (size_t)-1 ? 0U : size;
		}
		value = vec_get(&file, "group.publicKey.publicExponent");
		if (value != NULL) {
			size = hex_stripped(value, group.e, sizeof(group.e));
			CHECK(size != (size_t)-1);
			group.e_size = size == (size_t)-1 ? 0U : size;
		}
		value = vec_get(&file, "group.sha");
		if (value != NULL) {
			strncpy(group.sha, value, sizeof(group.sha) - 1U);
			group.sha[sizeof(group.sha) - 1U] = '\0';
		}
		value = vec_get(&file, "group.sLen");
		if (value != NULL) {
			group.salt_size = (size_t)strtoul(value, NULL, 10);
			group.have_salt_size = 1;
		}
		if (vec_get(&file, "tcId") == NULL)
			continue;

		/* The case. */
		index = hash_index(group.sha);
		CHECK(index >= 0 && group.n_size > 0U && group.e_size > 0U);
		message_size = vec_hex(&file, "msg", message, sizeof(message));
		signature_size = vec_hex(&file, "sig", signature, sizeof(signature));
		result = vec_get(&file, "result");
		flags = vec_get(&file, "flags");
		CHECK(message_size != (size_t)-1 && signature_size != (size_t)-1 && result != NULL && flags != NULL);
		if (index < 0 || group.n_size == 0U || message_size == (size_t)-1 || signature_size == (size_t)-1 || result == NULL || flags == NULL)
			continue;
		if (kind == 1)
			CHECK(group.have_salt_size);
		memset(&key, 0, sizeof(key));
		key.type = CRYC_KEY_RSA;
		key.rsa.n = group.n;
		key.rsa.n_size = group.n_size;
		key.rsa.e = group.e;
		key.rsa.e_size = group.e_size;
		total++;
		count_rsa_wycheproof++;
		if (kind == 0)
			error = verify(&key, pkcs1_alg[index], 0U, message, message_size, signature, signature_size);
		else
			error = verify(&key, pss_alg[index], group.salt_size, message, message_size, signature, signature_size);

		/* valid: accepted; invalid: refused as a bad signature; acceptable: either, but a DigestInfo without NULL is refused. */
		if (strcmp(result, "valid") == 0) {
			CHECK(error == 0);
			valid++;
			count_rsa_wycheproof_valid++;
			if (kind == 0 && index == 0)
				fixture_keep(&fixture_pkcs1, &key, pkcs1_alg[index], 0U, message, message_size, signature, signature_size);
			if (kind == 0 && index == 2 && group.n_size > 400U)
				fixture_keep(&fixture_pkcs1_sha512, &key, pkcs1_alg[index], 0U, message, message_size, signature, signature_size);
			if (kind == 1 && index == 0)
				fixture_keep(&fixture_pss, &key, pss_alg[index], group.salt_size, message, message_size, signature, signature_size);
		} else if (strcmp(result, "invalid") == 0) {
			CHECK(error == EBADMSG);
		} else {
			CHECK(error == 0 || error == EBADMSG);
			if (strstr(flags, "MissingNull") != NULL)
				CHECK(error == EBADMSG);
		}
	}
	vec_close(&file);
	printf("  %s: %lu cases, %lu valid\n", name, total, valid);
	CHECK(total > 90U && valid > 5U);
}

/* The counts of the CAVP RSA cases verified, valid, and refused for a key below 2048 bits. */
static unsigned long count_rsa_cavp;
static unsigned long count_rsa_cavp_valid;
static unsigned long count_rsa_cavp_small;

/* Checks one CAVP RSA SigVer file (kind 0 is PKCS#1 v1.5, kind 1 is PSS) with SHA-256, SHA-384 and SHA-512. */
static void
check_rsa_cavp(
	const char *name,
	int kind)
{
	static uint8_t message[MESSAGE_MAX];
	static uint8_t signature[SIGNATURE_MAX * 2U];
	static uint8_t salt[MESSAGE_MAX];
	static uint8_t n[MODULUS_MAX];
	static uint8_t e[MODULUS_MAX];
	struct vec_file file;
	struct cryc_pubkey key;
	const char *value;
	const char *result;
	size_t n_size;
	size_t e_size;
	size_t message_size;
	size_t signature_size;
	size_t salt_size;
	size_t bits;
	int index;
	int error;
	unsigned long valid[3];
	unsigned long total;

	/* Opens the file. */
	n_size = 0;
	valid[0] = 0;
	valid[1] = 0;
	valid[2] = 0;
	total = 0;
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return;
	}

	/* A record with an "n" sets the modulus; a record with a SHAAlg is a case. */
	while (vec_next(&file)) {
		value = vec_get(&file, "n");
		if (value != NULL) {
			n_size = hex_stripped(value, n, sizeof(n));
			CHECK(n_size != (size_t)-1);
			if (n_size == (size_t)-1)
				n_size = 0;
			continue;
		}
		value = vec_get(&file, "SHAAlg");
		if (value == NULL)
			continue;
		index = hash_index(value);
		if (index < 0)
			continue;
		CHECK(n_size > 0U);
		e_size = record_number(&file, "e", e, sizeof(e));
		message_size = record_bytes(&file, "Msg", message, sizeof(message));
		signature_size = record_bytes(&file, "S", signature, sizeof(signature));
		result = vec_get(&file, "Result");
		salt_size = 0;
		if (kind == 1) {
			salt_size = record_bytes(&file, "SaltVal", salt, sizeof(salt));
			CHECK(salt_size != (size_t)-1);
		}
		CHECK(e_size != (size_t)-1 && message_size != (size_t)-1 && signature_size != (size_t)-1 && result != NULL);
		if (n_size == 0U || e_size == (size_t)-1 || message_size == (size_t)-1 || signature_size == (size_t)-1 || result == NULL || salt_size == (size_t)-1)
			continue;
		memset(&key, 0, sizeof(key));
		key.type = CRYC_KEY_RSA;
		key.rsa.n = n;
		key.rsa.n_size = n_size;
		key.rsa.e = e;
		key.rsa.e_size = e_size;
		bits = modulus_bits(&key.rsa);

		/* The 3072-bit cases give a one-byte "SaltVal = 00" for a salt length of 0. */
		if (kind == 1 && bits == 3072U && salt_size == 1U && salt[0] == 0x00U)
			salt_size = 0;
		if (kind == 0)
			error = verify(&key, pkcs1_alg[index], 0U, message, message_size, signature, signature_size);
		else
			error = verify(&key, pss_alg[index], salt_size, message, message_size, signature, signature_size);
		total++;
		count_rsa_cavp++;
		if (bits < 2048U) {
			CHECK(error == ENOTSUP);
			count_rsa_cavp_small++;
		} else if (result[0] == 'P') {
			CHECK(error == 0);
			valid[index]++;
			count_rsa_cavp_valid++;
		} else {
			/* A bad signature, or an exponent the primitive refuses. */
			CHECK(error == EBADMSG || error == EINVAL);
		}
	}
	vec_close(&file);
	printf("  %s: %lu cases, valid %lu / %lu / %lu (SHA-256 / 384 / 512)\n", name, total, valid[0], valid[1], valid[2]);
	CHECK(valid[0] >= 4U && valid[1] >= 4U && valid[2] >= 4U);
}

/* ------------------------------------------------------------------ */
/* What happens to a valid signature when one thing changes. */

/* Copies a fixture into work and relinks it. */
static void
fixture_copy(
	struct fixture *work,
	const struct fixture *fixture)
{
	/* A byte copy, then the views. */
	memcpy(work, fixture, sizeof(*work));
	fixture_relink(work);
}

/* The fixture as a verify call. */
static int
fixture_verify(
	const struct fixture *fixture)
{
	/* The key, the algorithm and salt, the message and the signature of the fixture. */
	return verify(&fixture->key, fixture->alg, fixture->salt_size, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size);
}

/* The changes common to every fixture: the message and the signature. */
static void
check_changes_common(
	const struct fixture *fixture)
{
	static struct fixture work;
	size_t index;
	size_t bit;
	size_t length;

	/* The valid signature verifies. */
	CHECK(fixture->loaded);
	if (!fixture->loaded)
		return;
	CHECK(fixture_verify(fixture) == 0);

	/* One bit of the message changed, at the first, middle and last byte. */
	fixture_copy(&work, fixture);
	for (bit = 0; bit < 8U; bit++) {
		work.message[0] ^= (uint8_t)(1U << bit);
		CHECK(fixture_verify(&work) == EBADMSG);
		work.message[0] ^= (uint8_t)(1U << bit);
		work.message[work.message_size / 2U] ^= (uint8_t)(1U << bit);
		CHECK(fixture_verify(&work) == EBADMSG);
		work.message[work.message_size / 2U] ^= (uint8_t)(1U << bit);
		work.message[work.message_size - 1U] ^= (uint8_t)(1U << bit);
		CHECK(fixture_verify(&work) == EBADMSG);
		work.message[work.message_size - 1U] ^= (uint8_t)(1U << bit);
	}
	CHECK(fixture_verify(&work) == 0);

	/* A shorter, a longer and an empty message. */
	work.message_size = fixture->message_size - 1U;
	CHECK(fixture_verify(&work) == EBADMSG);
	work.message_size = fixture->message_size + 1U;
	CHECK(fixture_verify(&work) == EBADMSG);
	work.message_size = 0U;
	CHECK(fixture_verify(&work) == EBADMSG);
	work.message_size = fixture->message_size;
	CHECK(fixture_verify(&work) == 0);

	/* Every length of signature other than the right one: shorter (to none) and longer. */
	for (length = 0; length < fixture->signature_size; length++) {
		work.signature_size = length;
		CHECK(fixture_verify(&work) == EBADMSG);
	}
	work.signature_size = fixture->signature_size;
	work.signature[work.signature_size] = 0U;
	work.signature_size = fixture->signature_size + 1U;
	CHECK(fixture_verify(&work) == EBADMSG);
	work.signature_size = fixture->signature_size;
	CHECK(fixture_verify(&work) == 0);

	/* A NULL signature of size 0 is a bad signature, not a caller's mistake. */
	CHECK(verify(&fixture->key, fixture->alg, fixture->salt_size, fixture->message, fixture->message_size, NULL, 0U) == EBADMSG);

	/* One bit of the signature changed at every byte: nothing verifies (a malformed DER also gives EBADMSG). */
	for (index = 0; index < fixture->signature_size; index++) {
		work.signature[index] ^= (uint8_t)(1U << (index % 8U));
		CHECK(fixture_verify(&work) == EBADMSG);
		work.signature[index] ^= (uint8_t)(1U << (index % 8U));
	}
	CHECK(fixture_verify(&work) == 0);
}

/* The changes to an RSA fixture: the other algorithms, the key, the salt. */
static void
check_changes_rsa(
	const struct fixture *fixture,
	int is_pss,
	int hash)
{
	static struct fixture work;
	static uint8_t big_n[1100];
	static uint8_t big_signature[1100];
	size_t salt;
	int other;
	int error;

	/* The other hashes' algorithms of the same scheme are different digests. */
	fixture_copy(&work, fixture);
	for (other = 0; other < 3; other++) {
		if (other == hash)
			continue;
		work.alg = is_pss ? pss_alg[other] : pkcs1_alg[other];
		CHECK(fixture_verify(&work) == EBADMSG);
	}

	/* An ECDSA algorithm with an RSA key does not suit the key, even when the key also carries a usable EC point: its type decides. */
	work.key.curve = fixture_p256.key.curve;
	work.key.point = fixture_p256.point;
	work.key.point_size = fixture_p256.key.point_size;
	for (other = 0; other < 3; other++) {
		work.alg = ecdsa_alg[other];
		CHECK(fixture_verify(&work) == EINVAL);
	}
	work.key.curve = CRYC_CURVE_NONE;
	work.key.point = NULL;
	work.key.point_size = 0U;
	work.alg = fixture->alg;

	/* The other scheme with each hash gives a bad signature (the encodings differ), with the right salt length for PSS. */
	for (other = 0; other < 3; other++) {
		work.alg = is_pss ? pkcs1_alg[other] : pss_alg[other];
		work.salt_size = hash_bytes[other];
		CHECK(fixture_verify(&work) == EBADMSG);
	}
	fixture_copy(&work, fixture);

	/* A salt length other than the signature's, for PSS (the decoded length must be exactly the required one). */
	if (is_pss) {
		for (salt = 0; salt < 3U; salt++) {
			work.salt_size = fixture->salt_size + 1U + salt;
			CHECK(fixture_verify(&work) == EBADMSG);
		}
		work.salt_size = fixture->salt_size - 1U;
		CHECK(fixture_verify(&work) == EBADMSG);
		work.salt_size = 0U;
		CHECK(fixture_verify(&work) == EBADMSG);
		work.salt_size = 1U;
		CHECK(fixture_verify(&work) == EBADMSG);
		work.salt_size = fixture->key.rsa.n_size;
		CHECK(fixture_verify(&work) == EBADMSG);
		work.salt_size = fixture->key.rsa.n_size + 1U;
		CHECK(fixture_verify(&work) == EBADMSG);
		work.salt_size = (size_t)-1;
		CHECK(fixture_verify(&work) == EBADMSG);
		work.salt_size = (size_t)-1 - 33U;
		CHECK(fixture_verify(&work) == EBADMSG);
		work.salt_size = fixture->salt_size;
		CHECK(fixture_verify(&work) == 0);
	} else {
		/* PKCS#1 v1.5 has no salt: whatever salt_size says, it is not used. */
		work.salt_size = 123456U;
		CHECK(fixture_verify(&work) == 0);
		work.salt_size = fixture->salt_size;
	}

	/* The modulus changed by one bit in the middle (still odd): another key, a bad signature. */
	work.n[work.key.rsa.n_size / 2U] ^= 0x10U;
	CHECK(fixture_verify(&work) == EBADMSG);
	work.n[work.key.rsa.n_size / 2U] ^= 0x10U;

	/* The modulus made even: malformed. */
	work.n[work.key.rsa.n_size - 1U] ^= 0x01U;
	CHECK(fixture_verify(&work) == EINVAL);
	work.n[work.key.rsa.n_size - 1U] ^= 0x01U;
	CHECK(fixture_verify(&work) == 0);

	/* The exponent: 3 is allowed (a bad signature), 0, 1, 2, an even one, none and a five-byte one are malformed. */
	work.e[0] = 0x03U;
	work.key.rsa.e_size = 1U;
	CHECK(fixture_verify(&work) == EBADMSG);
	work.e[0] = 0x00U;
	CHECK(fixture_verify(&work) == EINVAL);
	work.e[0] = 0x01U;
	CHECK(fixture_verify(&work) == EINVAL);
	work.e[0] = 0x02U;
	CHECK(fixture_verify(&work) == EINVAL);
	work.e[0] = 0x04U;
	CHECK(fixture_verify(&work) == EINVAL);
	work.key.rsa.e_size = 0U;
	CHECK(fixture_verify(&work) == EINVAL);
	work.e[0] = 0x01U;
	work.e[1] = 0x00U;
	work.e[2] = 0x00U;
	work.e[3] = 0x00U;
	work.e[4] = 0x01U;
	work.key.rsa.e_size = 5U;
	CHECK(fixture_verify(&work) == EINVAL);
	fixture_copy(&work, fixture);
	CHECK(fixture_verify(&work) == 0);

	/* Missing parts of the key. */
	work.key.rsa.n = NULL;
	CHECK(fixture_verify(&work) == EINVAL);
	work.key.rsa.n = work.n;
	work.key.rsa.e = NULL;
	CHECK(fixture_verify(&work) == EINVAL);
	work.key.rsa.e = work.e;
	CHECK(fixture_verify(&work) == 0);

	/* The size limits: below 2048 bits, 2047 bits, 8193 bits are ENOTSUP; 8192 bits is in range (a bad signature). */
	memset(big_n, 0, sizeof(big_n));
	random_bytes(big_n, 128U);
	big_n[0] |= 0x80U;
	big_n[127] |= 0x01U;
	work.key.rsa.n = big_n;
	work.key.rsa.n_size = 128U;
	CHECK(fixture_verify(&work) == ENOTSUP);
	random_bytes(big_n, 256U);
	big_n[0] = (uint8_t)((big_n[0] & 0x3fU) | 0x40U);
	big_n[255] |= 0x01U;
	work.key.rsa.n_size = 256U;
	CHECK(fixture_verify(&work) == ENOTSUP);
	big_n[0] |= 0x80U;
	CHECK(fixture_verify(&work) != ENOTSUP);
	random_bytes(big_n, 1025U);
	big_n[0] = 0x01U;
	big_n[1024] |= 0x01U;
	work.key.rsa.n_size = 1025U;
	CHECK(fixture_verify(&work) == ENOTSUP);
	random_bytes(big_n, 1024U);
	big_n[0] |= 0x80U;
	big_n[1023] |= 0x01U;
	work.key.rsa.n_size = 1024U;
	random_bytes(big_signature, 1024U);
	big_signature[0] = 0x00U;
	work.signature_size = 1024U;
	memcpy(work.signature, big_signature, 1024U);
	error = fixture_verify(&work);
	CHECK(error == EBADMSG);
	fixture_copy(&work, fixture);
	CHECK(fixture_verify(&work) == 0);
}

/* The changes to an EC fixture: the other hashes, RSA algorithms, the key's point and curve. */
static void
check_changes_ec(
	const struct fixture *fixture,
	int hash,
	const char *prime)
{
	static struct fixture work;
	uint8_t p[48];
	size_t size;
	size_t total;
	size_t index;
	size_t length;
	unsigned form;
	int other;
	int error;
	enum cryc_curve_id other_curve;

	/* The point's coordinate size, and the prime as bytes. */
	total = fixture->key.point_size;
	size = (total - 1U) / 2U;
	CHECK(hex_decode(prime, p, sizeof(p)) == size);

	/* The other hashes are other digests: ECDSA does not tie the hash to the curve, but the signature was made over one. */
	fixture_copy(&work, fixture);
	for (other = 0; other < 3; other++) {
		if (other == hash)
			continue;
		work.alg = ecdsa_alg[other];
		CHECK(fixture_verify(&work) == EBADMSG);
	}

	/* An RSA algorithm with an EC key does not suit the key, whatever the signature is (even when the key also carries a usable RSA key: its type decides). */
	work.key.rsa = fixture_pkcs1.key.rsa;
	for (other = 0; other < 3; other++) {
		work.alg = pkcs1_alg[other];
		CHECK(fixture_verify(&work) == EINVAL);
		work.alg = pss_alg[other];
		CHECK(fixture_verify(&work) == EINVAL);
	}
	work.alg = fixture->alg;
	work.key.rsa.n = NULL;
	work.key.rsa.n_size = 0U;
	work.key.rsa.e = NULL;
	work.key.rsa.e_size = 0U;

	/* RSA fields of the key are ignored for an EC key. */
	work.key.rsa.n = (const uint8_t *)"x";
	work.key.rsa.n_size = 1U;
	CHECK(fixture_verify(&work) == 0);
	work.key.rsa.n = NULL;
	work.key.rsa.n_size = 0U;

	/* A point of the wrong size: none, one byte short, one byte long, only the form byte. */
	for (length = 0; length <= total + 1U; length++) {
		if (length == total)
			continue;
		work.key.point_size = length;
		CHECK(fixture_verify(&work) == EINVAL);
	}
	work.key.point_size = total;

	/* A NULL point of no bytes. */
	work.key.point = NULL;
	work.key.point_size = 0U;
	CHECK(fixture_verify(&work) == EINVAL);
	work.key.point = work.point;
	work.key.point_size = total;

	/* Another form byte than uncompressed: compressed, hybrid, infinity and others, at the right size. */
	for (form = 0; form < 256U; form++) {
		if (form == 0x04U)
			continue;
		work.point[0] = (uint8_t)form;
		CHECK(fixture_verify(&work) == EINVAL);
	}
	work.point[0] = 0x04U;

	/* A compressed point at the compressed size is malformed, not unsupported. */
	work.point[0] = 0x02U;
	work.key.point_size = 1U + size;
	CHECK(fixture_verify(&work) == EINVAL);
	work.point[0] = 0x03U;
	CHECK(fixture_verify(&work) == EINVAL);
	work.point[0] = 0x04U;
	work.key.point_size = total;

	/* A point off the curve: any bit of y changed, any bit of x changed. */
	for (index = 1U; index < total; index += 5U) {
		work.point[index] ^= 0x01U;
		CHECK(fixture_verify(&work) == EINVAL);
		work.point[index] ^= 0x01U;
	}

	/* A coordinate equal to p, or above it. */
	memcpy(work.point + 1, p, size);
	CHECK(fixture_verify(&work) == EINVAL);
	memcpy(work.point, fixture->point, total);
	memcpy(work.point + 1U + size, p, size);
	CHECK(fixture_verify(&work) == EINVAL);
	memset(work.point + 1U + size, 0xff, size);
	CHECK(fixture_verify(&work) == EINVAL);
	memcpy(work.point, fixture->point, total);

	/* The point at the other curve's size or under the other curve. */
	other_curve = fixture->key.curve == CRYC_CURVE_P256 ? CRYC_CURVE_P384 : CRYC_CURVE_P256;
	work.key.curve = other_curve;
	CHECK(fixture_verify(&work) == EINVAL);

	/* A curve that is none or not a curve. */
	work.key.curve = CRYC_CURVE_NONE;
	CHECK(fixture_verify(&work) == EINVAL);
	work.key.curve = (enum cryc_curve_id)3;
	CHECK(fixture_verify(&work) == EINVAL);
	work.key.curve = (enum cryc_curve_id)99;
	CHECK(fixture_verify(&work) == EINVAL);
	work.key.curve = (enum cryc_curve_id)-1;
	CHECK(fixture_verify(&work) == EINVAL);
	work.key.curve = fixture->key.curve;
	CHECK(fixture_verify(&work) == 0);

	/* The key is checked before the signature: a bad key with a malformed signature is EINVAL, a good key with one is EBADMSG. */
	work.point[1] ^= 0x01U;
	error = verify(&work.key, work.alg, 0U, work.message, work.message_size, (const uint8_t *)"garbage", 7U);
	CHECK(error == EINVAL);
	error = verify(&work.key, work.alg, 0U, work.message, work.message_size, NULL, 0U);
	CHECK(error == EINVAL);
	work.point[1] ^= 0x01U;
	error = verify(&work.key, work.alg, 0U, work.message, work.message_size, (const uint8_t *)"garbage", 7U);
	CHECK(error == EBADMSG);

	/* The unchanged fixture still verifies. */
	fixture_copy(&work, fixture);
	CHECK(fixture_verify(&work) == 0);
}

/* The argument errors of cryc_signature_verify, and the order they are checked in. */
static void
check_arguments(void)
{
	struct cryc_sig_params params;
	struct cryc_pubkey key;
	struct cryc_pubkey none;
	struct cryc_pubkey unsupported;
	static struct fixture work;
	const struct fixture *fixture;
	size_t index;
	int error;

	/* A valid RSA and a valid EC fixture to start from. */
	CHECK(fixture_pkcs1.loaded && fixture_p256.loaded);
	if (!fixture_pkcs1.loaded || !fixture_p256.loaded)
		return;

	/* Missing key or parameters. */
	params.alg = CRYC_SIG_RSA_PKCS1_SHA256;
	params.salt_size = 0U;
	fixture = &fixture_pkcs1;
	CHECK(cryc_signature_verify(NULL, &params, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == EINVAL);
	CHECK(cryc_signature_verify(&fixture->key, NULL, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == EINVAL);
	CHECK(cryc_signature_verify(NULL, NULL, NULL, 0U, NULL, 0U) == EINVAL);

	/* A NULL message with a size, a NULL signature with a size. */
	CHECK(cryc_signature_verify(&fixture->key, &params, NULL, 1U, fixture->signature, fixture->signature_size) == EINVAL);
	CHECK(cryc_signature_verify(&fixture->key, &params, fixture->message, fixture->message_size, NULL, 1U) == EINVAL);

	/* A NULL empty message is the empty message (a bad signature here). */
	CHECK(cryc_signature_verify(&fixture->key, &params, NULL, 0U, fixture->signature, fixture->signature_size) == EBADMSG);

	/* The algorithm that is not set, unsupported, or not an enumerator, with a good RSA key and with a good EC key. */
	for (index = 0; index < 2U; index++) {
		fixture = index == 0U ? &fixture_pkcs1 : &fixture_p256;
		params.alg = CRYC_SIG_NONE;
		CHECK(cryc_signature_verify(&fixture->key, &params, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == EINVAL);
		params.alg = CRYC_SIG_UNSUPPORTED;
		CHECK(cryc_signature_verify(&fixture->key, &params, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == ENOTSUP);
		params.alg = (enum cryc_sig_alg)11;
		CHECK(cryc_signature_verify(&fixture->key, &params, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == ENOTSUP);
		params.alg = (enum cryc_sig_alg)99;
		CHECK(cryc_signature_verify(&fixture->key, &params, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == ENOTSUP);
		params.alg = (enum cryc_sig_alg)-1;
		CHECK(cryc_signature_verify(&fixture->key, &params, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == ENOTSUP);
	}

	/* A key that is not set, unsupported, or not a type, with each kind of algorithm. */
	memset(&none, 0, sizeof(none));
	memset(&unsupported, 0, sizeof(unsupported));
	unsupported.type = CRYC_KEY_UNSUPPORTED;
	for (index = 0; index < 3U; index++) {
		params.alg = index == 0U ? CRYC_SIG_RSA_PKCS1_SHA256 : (index == 1U ? CRYC_SIG_RSA_PSS_SHA384 : CRYC_SIG_ECDSA_SHA256);
		CHECK(cryc_signature_verify(&none, &params, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == EINVAL);
		CHECK(cryc_signature_verify(&unsupported, &params, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == ENOTSUP);
		key = fixture_pkcs1.key;
		key.type = (enum cryc_key_type)4;
		CHECK(cryc_signature_verify(&key, &params, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == ENOTSUP);
		key.type = (enum cryc_key_type)100;
		CHECK(cryc_signature_verify(&key, &params, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size) == ENOTSUP);
	}

	/* An unset algorithm with an unsupported key, and an unsupported algorithm with an unset key: the first rule of the order. */
	params.alg = CRYC_SIG_NONE;
	CHECK(cryc_signature_verify(&unsupported, &params, NULL, 0U, NULL, 0U) == EINVAL);
	params.alg = CRYC_SIG_UNSUPPORTED;
	CHECK(cryc_signature_verify(&none, &params, NULL, 0U, NULL, 0U) == EINVAL);

	/* An unsupported algorithm or key with garbage: ENOTSUP before anything about the signature or the key's contents. */
	params.alg = CRYC_SIG_UNSUPPORTED;
	CHECK(cryc_signature_verify(&fixture_pkcs1.key, &params, NULL, 0U, (const uint8_t *)"x", 1U) == ENOTSUP);
	params.alg = CRYC_SIG_RSA_PSS_SHA512;
	CHECK(cryc_signature_verify(&unsupported, &params, NULL, 0U, (const uint8_t *)"x", 1U) == ENOTSUP);

	/* Each algorithm against the wrong kind of key: EINVAL, before the key's or the signature's contents are looked at. */
	fixture_copy(&work, &fixture_p256);
	work.point[1] ^= 0x01U;
	for (index = 0; index < 3U; index++) {
		params.alg = pkcs1_alg[index];
		CHECK(cryc_signature_verify(&work.key, &params, NULL, 0U, NULL, 0U) == EINVAL);
		params.alg = pss_alg[index];
		CHECK(cryc_signature_verify(&work.key, &params, NULL, 0U, NULL, 0U) == EINVAL);
		params.alg = ecdsa_alg[index];
		CHECK(cryc_signature_verify(&fixture_pkcs1.key, &params, NULL, 0U, NULL, 0U) == EINVAL);
	}

	/* The result of a successful call is 0 with every one of the nine algorithms' own fixtures (checked above); here a last sanity check. */
	error = fixture_verify(&fixture_p256);
	CHECK(error == 0);
}

/* ------------------------------------------------------------------ */
/* Random and hostile input. */

/* Whether an error is one the header names for verification. */
static int
verify_error_known(
	int error)
{
	/* The four results. */
	return error == 0 || error == EBADMSG || error == EINVAL || error == ENOTSUP;
}

/* Random keys, algorithms and signatures: no crash, and one of the four results; a random signature never verifies. */
static void
check_random(void)
{
	static uint8_t n[1100];
	static uint8_t signature[1100];
	static uint8_t message[300];
	static uint8_t point[128];
	struct cryc_pubkey key;
	struct cryc_sig_params params;
	size_t n_size;
	size_t signature_size;
	unsigned iteration;
	int error;
	unsigned long accepted;

	/* RSA: random odd moduli of 256 to 1024 bytes, random exponents, random signatures. */
	accepted = 0;
	for (iteration = 0; iteration < 300U; iteration++) {
		n_size = 200U + random32() % 825U;
		random_bytes(n, n_size);
		if (iteration % 3U != 0U)
			n[0] |= 0x80U;
		n[n_size - 1U] |= (uint8_t)(iteration % 7U == 0U ? 0U : 1U);
		memset(&key, 0, sizeof(key));
		key.type = CRYC_KEY_RSA;
		key.rsa.n = n;
		key.rsa.n_size = n_size;
		key.rsa.e = n + n_size - 4U;
		key.rsa.e_size = 1U + random32() % 4U;
		signature_size = iteration % 2U == 0U ? n_size : random32() % 1100U;
		random_bytes(signature, signature_size);
		random_bytes(message, sizeof(message));
		params.alg = (enum cryc_sig_alg)(CRYC_SIG_RSA_PKCS1_SHA256 + (int)(random32() % 6U));
		params.salt_size = random32() % 70U;
		error = cryc_signature_verify(&key, &params, message, random32() % sizeof(message), signature, signature_size);
		CHECK(verify_error_known(error));
		if (error == 0)
			accepted++;
	}

	/* ECDSA: a valid key with random signatures (random DER-like bytes, and a well-formed SEQUENCE of random integers). */
	CHECK(fixture_p256.loaded && fixture_p384.loaded);
	for (iteration = 0; iteration < 600U && fixture_p256.loaded && fixture_p384.loaded; iteration++) {
		const struct fixture *fixture;
		uint8_t r[48];
		uint8_t s[48];

		fixture = iteration % 2U == 0U ? &fixture_p256 : &fixture_p384;
		key = fixture->key;
		params.alg = ecdsa_alg[random32() % 3U];
		params.salt_size = 0U;
		if (iteration % 3U == 0U) {
			signature_size = random32() % 120U;
			random_bytes(signature, signature_size);
		} else {
			random_bytes(r, sizeof(r));
			random_bytes(s, sizeof(s));
			signature_size = build_signature(r, 32U + (iteration % 17U), s, 32U + (iteration % 13U), signature, sizeof(signature));
		}
		error = cryc_signature_verify(&key, &params, message, random32() % sizeof(message), signature, signature_size);
		CHECK(verify_error_known(error));
		if (error == 0)
			accepted++;
	}

	/* ECDSA with random points (nearly all off the curve), and random curve and form. */
	for (iteration = 0; iteration < 200U; iteration++) {
		memset(&key, 0, sizeof(key));
		key.type = CRYC_KEY_EC;
		key.curve = iteration % 2U == 0U ? CRYC_CURVE_P256 : CRYC_CURVE_P384;
		random_bytes(point, sizeof(point));
		point[0] = iteration % 4U == 0U ? (uint8_t)random32() : 0x04U;
		key.point = point;
		key.point_size = iteration % 5U == 0U ? random32() % 100U : (key.curve == CRYC_CURVE_P256 ? 65U : 97U);
		params.alg = ecdsa_alg[random32() % 3U];
		params.salt_size = 0U;
		signature_size = random32() % 120U;
		random_bytes(signature, signature_size);
		error = cryc_signature_verify(&key, &params, message, 10U, signature, signature_size);
		CHECK(verify_error_known(error));
		CHECK(error != 0);
	}
	CHECK(accepted == 0UL);
}

/* ------------------------------------------------------------------ */
/* Certificates. */

/* Reads a whole file; returns its bytes (NUL-terminated) and size, or NULL. */
static char *
read_file(
	const char *path,
	size_t *size)
{
	FILE *stream;
	long length;
	char *data;

	/* Binary mode (a Windows text mode would change line ends). */
	stream = fopen(path, "rb");
	if (stream == NULL)
		return NULL;
	fseek(stream, 0, SEEK_END);
	length = ftell(stream);
	fseek(stream, 0, SEEK_SET);
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

/* Returns the bit length of an RSA modulus given without the sign octet. */
static size_t
modulus_bits(
	const struct cryc_rsa_key *key)
{
	size_t bits;
	uint8_t top;

	/* The bytes, less the zero bits at the top of the first. */
	if (key->n_size == 0U)
		return 0U;
	bits = key->n_size * 8U;
	for (top = key->n[0]; (top & 0x80U) == 0U && bits > 0U; top = (uint8_t)(top << 1))
		bits--;
	return bits;
}

/* Returns the result the header gives for verifying cert's signature with key under cert's own algorithm. */
static int
expected_for(
	const struct cryc_x509 *cert,
	const struct cryc_pubkey *key)
{
	int rsa_alg;

	/* Unsupported algorithm or key first, then the kind of key, then RSA's size floor. */
	if (cert->signature_alg.alg == CRYC_SIG_UNSUPPORTED || cert->signature_alg.alg == CRYC_SIG_NONE)
		return ENOTSUP;
	if (key->type == CRYC_KEY_UNSUPPORTED)
		return ENOTSUP;
	rsa_alg = (int)cert->signature_alg.alg <= (int)CRYC_SIG_RSA_PSS_SHA512;
	if (rsa_alg && key->type != CRYC_KEY_RSA)
		return EINVAL;
	if (!rsa_alg && key->type != CRYC_KEY_EC)
		return EINVAL;
	if (key->type == CRYC_KEY_RSA && modulus_bits(&key->rsa) < 2048U)
		return ENOTSUP;
	return 0;
}

/* Verifies the signature of cert with key. */
static int
verify_certificate(
	const struct cryc_x509 *cert,
	const struct cryc_pubkey *key)
{
	/* The tbsCertificate is the message, the signatureValue the signature. */
	return verify(key, cert->signature_alg.alg, cert->signature_alg.salt_size, cert->tbs, cert->tbs_size, cert->signature, cert->signature_size);
}

/* A parsed certificate of the bundle, with its bytes. */
struct bundle_entry {
	uint8_t *der;
	size_t der_size;
	struct cryc_x509 cert;
};

/* Verifies the self-signature of every root of the bundle, then changes each in one bit and under other keys. */
static void
check_bundle(void)
{
	struct bundle_entry *entries;
	const char *path;
	char *text;
	size_t size;
	size_t position;
	size_t count;
	size_t capacity;
	size_t index;
	size_t other;
	size_t bit_at;
	static uint8_t work[LIMBO_MAX];
	uint8_t *der;
	size_t der_size;
	size_t unsupported_alg;
	size_t unsupported_key;
	size_t rsa_pkcs1;
	size_t rsa_pss;
	size_t ecdsa;
	int error;
	int want;

	/* The bundle must be readable: a missing file is a failure, not a pass. */
	path = getenv("WS335_CACERT");
	if (path == NULL)
		path = CACERT_DEFAULT;
	text = read_file(path, &size);
	CHECK(text != NULL);
	if (text == NULL)
		return;

	/* Every certificate, parsed and kept. */
	capacity = 400U;
	entries = calloc(capacity, sizeof(*entries));
	CHECK(entries != NULL);
	if (entries == NULL) {
		free(text);
		return;
	}
	position = 0;
	count = 0;
	for (;;) {
		error = cryc_pem_next_certificate(text, size, &position, &der, &der_size);
		if (error == ENOENT)
			break;
		CHECK(error == 0);
		if (error != 0 || count >= capacity)
			break;
		entries[count].der = der;
		entries[count].der_size = der_size;
		error = cryc_x509_parse(&entries[count].cert, der, der_size);
		CHECK(error == 0);
		if (error != 0)
			continue;
		count++;
	}
	free(text);
	CHECK(count > 100U);

	/* The self-signature of each root. */
	unsupported_alg = 0;
	unsupported_key = 0;
	rsa_pkcs1 = 0;
	rsa_pss = 0;
	ecdsa = 0;
	for (index = 0; index < count; index++) {
		const struct cryc_x509 *cert;

		cert = &entries[index].cert;
		want = expected_for(cert, &cert->key);
		error = verify_certificate(cert, &cert->key);
		CHECK(error == want);
		if (error != want)
			printf("bundle certificate %lu: alg %d key %d: got %d, expected %d\n", (unsigned long)index, (int)cert->signature_alg.alg, (int)cert->key.type, error, want);
		if (error == 0) {
			count_bundle_ok++;
			if ((int)cert->signature_alg.alg <= (int)CRYC_SIG_RSA_PKCS1_SHA512)
				rsa_pkcs1++;
			else if ((int)cert->signature_alg.alg <= (int)CRYC_SIG_RSA_PSS_SHA512)
				rsa_pss++;
			else
				ecdsa++;
		} else if (error == ENOTSUP) {
			count_bundle_notsup++;
			if (cert->signature_alg.alg == CRYC_SIG_UNSUPPORTED)
				unsupported_alg++;
			else
				unsupported_key++;
		}
	}
	printf("  bundle: %lu certificates, %lu verified (%lu RSA PKCS#1, %lu RSA-PSS, %lu ECDSA), %lu ENOTSUP (%lu unsupported signature algorithm such as SHA-1, %lu unsupported key)\n", (unsigned long)count, count_bundle_ok, (unsigned long)rsa_pkcs1, (unsigned long)rsa_pss, (unsigned long)ecdsa, count_bundle_notsup, (unsigned long)unsupported_alg, (unsigned long)unsupported_key);
	CHECK(count_bundle_ok > 100UL);
	CHECK(rsa_pkcs1 > 20U && ecdsa > 20U);
	CHECK(count_bundle_ok + count_bundle_notsup == (unsigned long)count);

	/* One bit of the tbsCertificate changed (first, middle, last byte) or of the signature: a verifying root no longer verifies. */
	for (index = 0; index < count; index++) {
		const struct cryc_x509 *cert;
		size_t positions[3];
		size_t at;

		cert = &entries[index].cert;
		if (expected_for(cert, &cert->key) != 0)
			continue;
		memcpy(work, cert->tbs, cert->tbs_size);
		positions[0] = 0U;
		positions[1] = cert->tbs_size / 2U;
		positions[2] = cert->tbs_size - 1U;
		for (at = 0; at < 3U; at++) {
			work[positions[at]] ^= (uint8_t)(1U << (index % 8U));
			error = verify(&cert->key, cert->signature_alg.alg, cert->signature_alg.salt_size, work, cert->tbs_size, cert->signature, cert->signature_size);
			CHECK(error == EBADMSG);
			if (error == EBADMSG)
				count_bundle_changed++;
			work[positions[at]] ^= (uint8_t)(1U << (index % 8U));
		}
		memcpy(work, cert->signature, cert->signature_size);
		positions[0] = 0U;
		positions[1] = cert->signature_size / 2U;
		positions[2] = cert->signature_size - 1U;
		for (at = 0; at < 3U; at++) {
			work[positions[at]] ^= (uint8_t)(1U << ((index + at) % 8U));
			error = verify(&cert->key, cert->signature_alg.alg, cert->signature_alg.salt_size, cert->tbs, cert->tbs_size, work, cert->signature_size);
			CHECK(error == EBADMSG);
			if (error == EBADMSG)
				count_bundle_changed++;
			work[positions[at]] ^= (uint8_t)(1U << ((index + at) % 8U));
		}
	}

	/* The signature of one root under the key of another: the header's result for that pair (a mismatched kind is EINVAL, a supported pair EBADMSG). */
	for (index = 0; index < count; index++) {
		for (bit_at = 1; bit_at <= 6U; bit_at += 5U) {
			const struct cryc_x509 *cert;
			const struct cryc_x509 *issuer;

			other = (index + bit_at) % count;
			cert = &entries[index].cert;
			issuer = &entries[other].cert;
			want = expected_for(cert, &issuer->key);
			error = verify_certificate(cert, &issuer->key);
			if (want == 0)
				want = EBADMSG;
			if (cert->key.curve == issuer->key.curve && cert->key.type == issuer->key.type && cert->key.type == CRYC_KEY_EC && issuer->key.point_size == cert->key.point_size && memcmp(issuer->key.point, cert->key.point, cert->key.point_size) == 0)
				want = 0;
			if (cert->key.type == CRYC_KEY_RSA && issuer->key.type == CRYC_KEY_RSA && issuer->key.rsa.n_size == cert->key.rsa.n_size && memcmp(issuer->key.rsa.n, cert->key.rsa.n, cert->key.rsa.n_size) == 0)
				want = 0;
			CHECK(error == want || (want == 0 && error == expected_for(cert, &cert->key)));
		}
	}

	/* The changes, each refused. */
	printf("  bundle: %lu changed signatures or certificates all refused\n", count_bundle_changed);
	CHECK(count_bundle_changed > 500UL);

	/* Frees what the loop above kept. */
	for (index = 0; index < count; index++)
		free(entries[index].der);
	free(entries);
}

/* One certificate of a limbo case. */
struct limbo_entry {
	uint8_t *der;
	size_t der_size;
	struct cryc_x509 cert;
	int parsed;
};

/* Reads the certificate of key from the record into entries[*count]; returns 1 when there was one. */
static int
limbo_load(
	const struct vec_file *file,
	const char *key,
	struct limbo_entry *entries,
	size_t *count)
{
	const char *text;
	size_t length;
	uint8_t *der;
	size_t size;

	/* An absent key is the end of the list. */
	text = vec_get(file, key);
	if (text == NULL)
		return 0;
	length = strlen(text) / 2U;
	der = malloc(length + 1U);
	CHECK(der != NULL);
	if (der == NULL)
		return 0;
	size = hex_decode(text, der, length);
	CHECK(size == length);
	entries[*count].der = der;
	entries[*count].der_size = size == (size_t)-1 ? 0U : size;
	entries[*count].parsed = size != (size_t)-1 && cryc_x509_parse(&entries[*count].cert, der, size) == 0;
	*count += 1U;
	return 1;
}

/* The number of slots of the cache of limbo results (a power of two, well above the distinct pairs). */
#define PAIR_SLOTS	(1UL << 17)

/* One slot of the cache: the pair's key (0 for an empty slot) and the result of verifying it. */
struct pair_slot {
	uint64_t key;
	int result;
};

/* The results of the pairs verified so far (the corpus repeats the same certificates in many cases). */
static struct pair_slot *pair_cache;

/* The number of distinct pairs verified. */
static unsigned long count_limbo_distinct;

/* Hashes size bytes (FNV-1a, 64 bits) from a start value. */
static uint64_t
fnv64(
	uint64_t hash,
	const uint8_t *data,
	size_t size)
{
	size_t index;

	/* Each byte. */
	for (index = 0; index < size; index++) {
		hash ^= data[index];
		hash *= 1099511628211ULL;
	}
	return hash;
}

/* Verifies cert with the key of issuer, once for each distinct pair of certificate and issuer key. */
static int
verify_pair_cached(
	const struct cryc_x509 *cert,
	const struct cryc_x509 *issuer)
{
	uint64_t key;
	size_t slot;
	int error;

	/* The pair's key: the whole certificate and the issuer's SubjectPublicKeyInfo. */
	key = fnv64(14695981039346656037ULL, cert->der, cert->der_size);
	key = fnv64(key ^ 0x9e3779b97f4a7c15ULL, issuer->spki, issuer->spki_size);
	if (key == 0U)
		key = 1U;

	/* Linear probing: a slot with the key is a repeat, an empty slot ends the search. */
	slot = (size_t)(key % PAIR_SLOTS);
	while (pair_cache[slot].key != 0U) {
		if (pair_cache[slot].key == key)
			return pair_cache[slot].result;
		slot = (slot + 1U) % PAIR_SLOTS;
	}
	error = verify_certificate(cert, &issuer->key);
	pair_cache[slot].key = key;
	pair_cache[slot].result = error;
	count_limbo_distinct++;
	return error;
}

/* The count of limbo SUCCESS cases whose peer has an issuer of the same name in the case, and of those with a signature that verifies. */
static unsigned long limbo_success_with_issuer;
static unsigned long limbo_success_verified;
static unsigned long limbo_success_unsupported;

/*
 * For each x509-limbo case (every SUCCESS case, and one case in eight of the
 * others, to bound the run): every certificate against every certificate
 * of the case whose subject Name is the same bytes as its issuer Name.
 * Whatever the case's verdict, the result is one of the four.  In a SUCCESS
 * case the peer's real issuer is in the case, so when there is a candidate
 * at all one of the candidates verifies the peer's signature (or the peer's
 * algorithm or key is outside the range).
 */
static void
check_limbo(void)
{
	struct vec_file file;
	struct limbo_entry *entries;
	char key[32];
	size_t count;
	size_t index;
	size_t other;
	size_t index_end;
	unsigned long records;
	int expect_success;
	int error;
	int candidates;
	int verified;
	int unsupported;

	/* The records. */
	entries = calloc(130U, sizeof(*entries));
	pair_cache = calloc(PAIR_SLOTS, sizeof(*pair_cache));
	CHECK(entries != NULL && pair_cache != NULL);
	if (entries == NULL || pair_cache == NULL) {
		free(entries);
		free(pair_cache);
		return;
	}
	if (vec_open(&file, "limbo") != 0) {
		CHECK(0);
		free(entries);
		free(pair_cache);
		return;
	}
	records = 0;
	while (vec_next(&file)) {
		records++;
		expect_success = strcmp(vec_get(&file, "expected"), "SUCCESS") == 0;

		/* The peer, the trusted and the untrusted certificates. */
		count = 0;
		(void)limbo_load(&file, "peer", entries, &count);
		for (index = 0; index < 4U; index++) {
			sprintf(key, "trusted.%u", (unsigned)index);
			if (!limbo_load(&file, key, entries, &count))
				break;
		}
		for (index = 0; index < 120U && count < 125U; index++) {
			sprintf(key, "untrusted.%u", (unsigned)index);
			if (!limbo_load(&file, key, entries, &count))
				break;
		}

		/* Each certificate against the certificates that could have issued it. */
		index_end = expect_success ? count : (count < 4U ? count : 4U);
		if (!expect_success && records % 8U != 0U)
			index_end = 0U;
		for (index = 0; index < index_end; index++) {
			candidates = 0;
			verified = 0;
			unsupported = 0;
			if (!entries[index].parsed)
				continue;
			for (other = 0; other < count; other++) {
				const struct cryc_x509 *cert;
				const struct cryc_x509 *issuer;

				if (!entries[other].parsed)
					continue;
				cert = &entries[index].cert;
				issuer = &entries[other].cert;
				if (cert->issuer_size != issuer->subject_size || memcmp(cert->issuer, issuer->subject, cert->issuer_size) != 0)
					continue;
				candidates++;
				count_limbo_pairs++;
				error = verify_pair_cached(cert, issuer);
				CHECK(verify_error_known(error));
				if (error == 0) {
					verified++;
					count_limbo_ok++;
				} else if (error == ENOTSUP) {
					unsupported++;
				}
			}
			if (expect_success && index == 0U && candidates > 0) {
				limbo_success_with_issuer++;
				if (verified > 0)
					limbo_success_verified++;
				else if (unsupported > 0)
					limbo_success_unsupported++;
				else
					printf("limbo %s: peer has %d candidate issuers, none verifies\n", vec_get(&file, "id"), candidates);
			}
		}
		for (index = 0; index < count; index++)
			free(entries[index].der);
	}
	vec_close(&file);
	free(entries);
	free(pair_cache);
	pair_cache = NULL;
	printf("  limbo: %lu cases, %lu issuer pairs tried (%lu distinct), %lu verified; SUCCESS cases with an issuer in the case %lu (peer verified %lu, unsupported %lu)\n", records, count_limbo_pairs, count_limbo_distinct, count_limbo_ok, limbo_success_with_issuer, limbo_success_verified, limbo_success_unsupported);
	CHECK(records == 9792UL);
	CHECK(limbo_success_with_issuer > 900UL);
	CHECK(limbo_success_verified + limbo_success_unsupported == limbo_success_with_issuer);
}

/* Runs every check. */
int
main(void)
{
	/* Progress is visible as it happens (the sections take a while). */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The decoder first. */
	check_decode();
	printf("  decoder: %lu cases\n", count_decode_cases);

	/* The vectors (they also supply the fixtures). */
	check_ecdsa_wycheproof("wycheproof-ecdsa_secp256r1_sha256_test");
	check_ecdsa_wycheproof("wycheproof-ecdsa_secp256r1_sha512_test");
	check_ecdsa_wycheproof("wycheproof-ecdsa_secp384r1_sha384_test");
	check_ecdsa_wycheproof("wycheproof-ecdsa_secp384r1_sha512_test");
	check_ecdsa_cavp();
	check_rsa_wycheproof("wycheproof-rsa_signature_2048_sha256_test", 0);
	check_rsa_wycheproof("wycheproof-rsa_signature_2048_sha384_test", 0);
	check_rsa_wycheproof("wycheproof-rsa_signature_2048_sha512_test", 0);
	check_rsa_wycheproof("wycheproof-rsa_signature_3072_sha256_test", 0);
	check_rsa_wycheproof("wycheproof-rsa_signature_4096_sha512_test", 0);
	check_rsa_wycheproof("wycheproof-rsa_pss_2048_sha256_mgf1_32_test", 1);
	check_rsa_wycheproof("wycheproof-rsa_pss_3072_sha256_mgf1_32_test", 1);
	check_rsa_wycheproof("wycheproof-rsa_pss_4096_sha512_mgf1_64_test", 1);
	check_rsa_cavp("cavp-186-3rsatestvectors-SigVer15_186-3", 0);
	check_rsa_cavp("cavp-186-3rsatestvectors-SigVerPSS_186-3", 1);
	printf("ecdsa wycheproof %lu (valid %lu), cavp %lu (valid %lu), rsa wycheproof %lu (valid %lu), rsa cavp %lu (valid %lu, %lu below 2048 bits), decode cases %lu\n", count_ecdsa_wycheproof, count_ecdsa_wycheproof_valid, count_ecdsa_cavp, count_ecdsa_cavp_valid, count_rsa_wycheproof, count_rsa_wycheproof_valid, count_rsa_cavp, count_rsa_cavp_valid, count_rsa_cavp_small, count_decode_cases);
	CHECK(count_ecdsa_wycheproof_valid > 700UL && count_rsa_wycheproof_valid > 250UL);

	/* A valid signature changed one thing at a time. */
	check_changes_common(&fixture_pkcs1);
	check_changes_common(&fixture_pkcs1_sha512);
	check_changes_common(&fixture_pss);
	check_changes_common(&fixture_p256);
	check_changes_common(&fixture_p384);
	check_changes_rsa(&fixture_pkcs1, 0, 0);
	check_changes_rsa(&fixture_pkcs1_sha512, 0, 2);
	check_changes_rsa(&fixture_pss, 1, 0);
	check_changes_ec(&fixture_p256, 0, prime_hex[0]);
	check_changes_ec(&fixture_p384, 1, prime_hex[1]);
	check_arguments();
	check_random();

	/* Certificates. */
	check_bundle();
	check_limbo();
	return check_finish("test-sigver");
}
