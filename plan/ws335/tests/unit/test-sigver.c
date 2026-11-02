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

/* The number of slots of the cache of limbo results (a power of two, well above the distinct pairs). */
#define PAIR_SLOTS	(1UL << 17)

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

/* A parsed certificate of the bundle, with its bytes. */
struct bundle_entry {
	uint8_t *der;
	size_t der_size;
	struct cryc_x509 cert;
};

/* One certificate of a limbo case. */
struct limbo_entry {
	uint8_t *der;
	size_t der_size;
	struct cryc_x509 cert;
	int parsed;
};

/* One slot of the cache: the pair's key (0 for an empty slot) and the result of verifying it. */
struct pair_slot {
	uint64_t key;
	int result;
};

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x1d872b41U;

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

/* The fixtures taken from the vectors. */
static struct fixture fixture_pkcs1;
static struct fixture fixture_pkcs1_sha512;
static struct fixture fixture_pss;
static struct fixture fixture_p256;
static struct fixture fixture_p384;

/* The count of valid CAVP cases per curve (P-256, P-384) and hash (SHA-256, SHA-384, SHA-512). */
static unsigned long cavp_valid[2][3];

/* The counts of the CAVP RSA cases verified, valid, and refused for a key below 2048 bits. */
static unsigned long count_rsa_cavp;
static unsigned long count_rsa_cavp_valid;
static unsigned long count_rsa_cavp_small;

/* The results of the pairs verified so far (the corpus repeats the same certificates in many cases). */
static struct pair_slot *pair_cache;

/* The number of distinct pairs verified. */
static unsigned long count_limbo_distinct;

/* The count of limbo SUCCESS cases whose peer has an issuer of the same name in the case, and of those with a signature that verifies. */
static unsigned long limbo_success_with_issuer;
static unsigned long limbo_success_verified;
static unsigned long limbo_success_unsupported;

/* Returns the next pseudo-random 32-bit value. */
static uint32_t random32(void);

/* Fills out with size pseudo-random bytes. */
static void random_bytes(uint8_t *out, size_t size);

/* Verifies with the algorithm alg (and the salt length for PSS). */
static int verify(const struct cryc_pubkey *key, enum cryc_sig_alg alg, size_t salt_size, const uint8_t *message, size_t message_size, const uint8_t *signature, size_t signature_size);

/* Maps a hash's name as the vectors write it to an index 0, 1, 2, or -1. */
static int hash_index(const char *name);

/* Reads a DER length of the independent reader. */
static int ref_length(const uint8_t *data, size_t size, size_t *position, size_t *length);

/* Reads one INTEGER of an Ecdsa-Sig-Value of the independent reader. */
static int ref_integer(const uint8_t *data, size_t end, size_t *position, size_t size, uint8_t *out);

/* Reads a whole Ecdsa-Sig-Value of the independent reader. */
static int ref_decode(const uint8_t *data, size_t data_size, size_t size, uint8_t *r, uint8_t *s);

/* Builds SEQUENCE { INTEGER r, INTEGER s } from the magnitudes with derwriter. */
static size_t build_signature(const uint8_t *r, size_t r_size, const uint8_t *s, size_t s_size, uint8_t *out, size_t room);

/* Calls the decoder on a buffer with sentinel fills and checks it against the reader written here. */
static int decode_against_reference(const uint8_t *sig, size_t sig_size, size_t size);

/* Checks a signature made from chosen magnitudes. */
static void check_decode_value(const uint8_t *r, size_t r_size, const uint8_t *s, size_t s_size, size_t size);

/* Checks the decoder on built signatures of every width. */
static void check_decode_widths(void);

/* Checks the decoder on the single values around the sign octet and the zero. */
static void check_decode_singles(void);

/* Checks the decoder on the hand-made malformed encodings. */
static void check_decode_malformed(void);

/* Checks the decoder on random changes of a valid signature. */
static void check_decode_mutated(void);

/* Checks the decoder on pure random bytes. */
static void check_decode_noise(void);

/* Checks the decoder on built signatures of every width, and on the hand-made malformed ones. */
static void check_decode(void);

/* Points a copy's key at the copy's own buffers. */
static void fixture_relink(struct fixture *fixture);

/* Strips the leading zero bytes of hex text into out. */
static size_t hex_stripped(const char *text, uint8_t *out, size_t room);

/* Remembers a valid case as a fixture when the fixture is empty. */
static void fixture_keep(struct fixture *fixture, const struct cryc_pubkey *key, enum cryc_sig_alg alg, size_t salt_size, const uint8_t *message, size_t message_size, const uint8_t *signature, size_t signature_size);

/* Checks one Wycheproof ECDSA file through cryc_signature_verify. */
static void check_ecdsa_wycheproof(const char *name);

/* Copies a hex value of the record into out with leading zeros stripped. */
static size_t record_number(const struct vec_file *file, const char *key, uint8_t *out, size_t room);

/* Decodes a hex field as bytes without removing leading zeros. */
static size_t record_bytes(const struct vec_file *file, const char *key, uint8_t *out, size_t room);

/* Returns the last section key of the record that names a curve, or NULL. */
static const char *record_section(const struct vec_file *file);

/* Checks the CAVP ECDSA SigVer cases of P-256 and P-384. */
static void check_ecdsa_cavp(void);

/* Checks one Wycheproof RSA file. */
static void check_rsa_wycheproof(const char *name, int kind);

/* Returns the bit length of an RSA modulus given without the sign octet. */
static size_t modulus_bits(const struct cryc_rsa_key *key);

/* Checks one CAVP RSA SigVer file. */
static void check_rsa_cavp(const char *name, int kind);

/* Copies a fixture into work and relinks it. */
static void fixture_copy(struct fixture *work, const struct fixture *fixture);

/* The fixture as a verify call. */
static int fixture_verify(const struct fixture *fixture);

/* The changes common to every fixture: the message and the signature. */
static void check_changes_common(const struct fixture *fixture);

/* The changes to an RSA fixture. */
static void check_changes_rsa(const struct fixture *fixture, int is_pss, int hash);

/* The changes to an EC fixture. */
static void check_changes_ec(const struct fixture *fixture, int hash, const char *prime);

/* The argument errors of cryc_signature_verify, and the order they are checked in. */
static void check_arguments(void);

/* Reports whether an error is one the header names for verification. */
static int verify_error_known(int error);

/* Checks random keys, algorithms and signatures. */
static void check_random(void);

/* Reads a whole file; returns its bytes (NUL-terminated) and size, or NULL. */
static char *read_file(const char *path, size_t *size);

/* Returns the result the header gives for verifying a certificate's signature with a key. */
static int expected_for(const struct cryc_x509 *cert, const struct cryc_pubkey *key);

/* Verifies the signature of cert with key. */
static int verify_certificate(const struct cryc_x509 *cert, const struct cryc_pubkey *key);

/* Verifies the self-signature of every root of the bundle, then changes each. */
static void check_bundle(void);

/* Reads the certificate of key from the record into entries[*count]. */
static int limbo_load(const struct vec_file *file, const char *key, struct limbo_entry *entries, size_t *count);

/* Hashes size bytes (FNV-1a, 64 bits) from a start value. */
static uint64_t fnv64(uint64_t hash, const uint8_t *data, size_t size);

/* Verifies cert with the key of issuer, once for each distinct pair. */
static int verify_pair_cached(const struct cryc_x509 *cert, const struct cryc_x509 *issuer);

/* Checks the issuer relations of the x509-limbo certificates. */
static void check_limbo(void);

/*
 * Runs every check of signature verification.
 *
 * The exit status is the one check_finish reports.
 */
int
main(void)
{
	int status;

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

	/* The counts of every vector file. */
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

	/* Reports the number of checks and the exit status of the test. */
	status = check_finish("test-sigver");
	return status;
}

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
	int error;

	/* The parameters, then the call. */
	params.alg = alg;
	params.salt_size = salt_size;
	error = cryc_signature_verify(key, &params, message, message_size, signature, signature_size);

	/* Reports why the signature was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the signature verifies. */
	return 0;
}

/* Maps a hash's name as the vectors write it to an index 0, 1, 2 (SHA-256, SHA-384, SHA-512), or -1. */
static int
hash_index(
	const char *name)
{
	int compare;

	/* SHA-256, written with or without the hyphen. */
	compare = strcmp(name, "SHA-256");
	if (compare == 0)
		return 0;
	compare = strcmp(name, "SHA256");
	if (compare == 0)
		return 0;

	/* SHA-384, written with or without the hyphen. */
	compare = strcmp(name, "SHA-384");
	if (compare == 0)
		return 1;
	compare = strcmp(name, "SHA384");
	if (compare == 0)
		return 1;

	/* SHA-512, written with or without the hyphen. */
	compare = strcmp(name, "SHA-512");
	if (compare == 0)
		return 2;
	compare = strcmp(name, "SHA512");
	if (compare == 0)
		return 2;

	/* Any other name is not a hash of the module. */
	return -1;
}

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
	if (octets == 0U ||
	    octets > 4U ||
	    *position + 1U + octets > size)
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

	/* Succeeded: the length is read and the position is past it. */
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
	int error;

	/* The tag, which is INTEGER. */
	if (*position >= end || data[*position] != 0x02U)
		return -1;
	*position += 1U;

	/* The length, which must be at least one and must fit in what is left. */
	error = ref_length(data, end, position, &length);
	if (error != 0)
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

	/* A value of no significant byte is zero, which is refused, and one wider than the field does not fit. */
	if (significant == 0U)
		return -1;
	if (significant > size)
		return -1;

	/* Writes the value right-aligned in the field. */
	memset(out, 0, size);
	memcpy(out + (size - significant), data + start, significant);

	/* Succeeded: the INTEGER is read. */
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
	int error;

	/* The SEQUENCE covers the whole input. */
	position = 0;
	if (data_size < 2U || data[0] != 0x30U)
		return -1;
	position = 1;
	error = ref_length(data, data_size, &position, &length);
	if (error != 0)
		return -1;
	if (length != data_size - position)
		return -1;
	end = data_size;

	/* Two integers and nothing else. */
	error = ref_integer(data, end, &position, size, r);
	if (error != 0)
		return -1;
	error = ref_integer(data, end, &position, size, s);
	if (error != 0)
		return -1;
	if (position != end)
		return -1;

	/* Succeeded: the input is one well-formed Ecdsa-Sig-Value. */
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

	/* Reports the size of the encoding. */
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
	size_t edge_r;
	size_t edge_s;
	int error;
	int reference;

	/* Sentinels show whether anything was written. */
	memset(got_r, 0x5a, sizeof(got_r));
	memset(got_s, 0xa5, sizeof(got_s));
	error = cryc_ecdsa_signature_decode(sig, sig_size, size, got_r, got_s);

	/* The reader written here decides what the decoder must do; a field size it cannot hold is refused. */
	reference = -1;
	if (size >= 1U && size <= sizeof(ref_r))
		reference = ref_decode(sig, sig_size, size, ref_r, ref_s);
	count_decode_cases++;

	/* An accepted encoding gives the reader's values. */
	if (reference == 0) {
		CHECK(error == 0);
		if (error == 0) {
			CHECK_BYTES(got_r, ref_r, size);
			CHECK_BYTES(got_s, ref_s, size);
		}

		/* Reports the decoder's answer for an encoding the reader accepts. */
		return error;
	}

	/* The last byte of the field, which a refusal must leave as it was (the first byte when the field is wider than the buffer). */
	edge_r = 0;
	if (size < sizeof(got_r))
		edge_r = size;
	edge_s = 0;
	if (size < sizeof(got_s))
		edge_s = size;

	/* Refused with EBADMSG, and nothing written. */
	CHECK(error == EBADMSG);
	CHECK(got_r[0] == 0x5aU && got_r[edge_r] == 0x5aU);
	CHECK(got_s[0] == 0xa5U && got_s[edge_s] == 0xa5U);

	/* Reports the refusal. */
	if (error != 0)
		return error;

	/* Reports a decoder that accepted what the reader refused (the failed check above is already counted). */
	return 0;
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
	int accepted;
	int error;

	/* The encoding, and what the decoder must give: the magnitudes stripped of leading zeros, padded to size. */
	sig_size = build_signature(r, r_size, s, s_size, sig, sizeof(sig));
	CHECK(sig_size > 0U);
	if (sig_size == 0U)
		return;

	/* The leading zero bytes of each magnitude, which DER does not keep. */
	for (strip_r = 0; strip_r + 1U < r_size && r[strip_r] == 0U; strip_r++)
		continue;
	for (strip_s = 0; strip_s + 1U < s_size && s[strip_s] == 0U; strip_s++)
		continue;

	/* What the decoder says about the encoding. */
	error = decode_against_reference(sig, sig_size, size);

	/* The encoding is accepted when both magnitudes fit the field and neither is zero. */
	accepted = 0;
	if (r_size - strip_r <= size &&
	    s_size - strip_s <= size &&
	    !(r_size - strip_r == 1U && r[strip_r] == 0U) &&
	    !(s_size - strip_s == 1U && s[strip_s] == 0U))
		accepted = 1;

	/* An accepted encoding decodes to the magnitudes; any other is refused. */
	if (accepted) {
		CHECK(error == 0);

		/* The decoded values are the magnitudes right-aligned in the field. */
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

/* Checks the decoder on built signatures of every width. */
static void
check_decode_widths(void)
{
	static const size_t widths[8] = { 1U, 2U, 20U, 32U, 48U, 64U, 66U, 70U };
	uint8_t value_a[80];
	uint8_t value_b[80];
	size_t width;
	size_t shape;
	size_t which;

	/* Values of each width: all ones, the top bit set (the sign octet), the top bit clear, one, leading zeros in the magnitude. */
	for (width = 0; width < 8U; width++) {
		for (shape = 0; shape < 6U; shape++) {
			/* Shape 0 and 5: the first value all ones, the second one. */
			memset(value_a, 0xffU, widths[width]);
			memset(value_b, 0x00U, widths[width]);
			value_b[widths[width] - 1U] = 0x01U;

			/* The other shapes replace the values. */
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

				/* The last byte is odd, so that the magnitude is not all zero. */
				value_a[widths[width] - 1U] |= 0x01U;
				value_b[widths[width] - 1U] |= 0x01U;
			}

			/* Each field width against the width of the values (narrower, equal, wider). */
			for (which = 0; which < 8U; which++) {
				check_decode_value(value_a, widths[width], value_b, widths[width], widths[which]);
			}
		}
	}
}

/* Checks the decoder on the single values around the sign octet and the zero. */
static void
check_decode_singles(void)
{
	static const uint8_t singles[4] = { 0x00U, 0x01U, 0x7fU, 0x80U };
	uint8_t one;
	size_t index;

	/* Small magnitudes: the single values around the sign octet and the zero. */
	for (index = 0; index < 4U; index++) {
		one = singles[index];
		check_decode_value(&one, 1U, &one, 1U, 1U);
		check_decode_value(&one, 1U, &one, 1U, 32U);
		check_decode_value(&one, 1U, &singles[1], 1U, 32U);
		check_decode_value(&singles[1], 1U, &one, 1U, 32U);
	}
}

/* Checks the decoder on the hand-made malformed encodings, each against the reader and refused for the reason. */
static void
check_decode_malformed(void)
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
	uint8_t out_r[80];
	uint8_t out_s[80];
	int error;

	/* The hand-made malformed encodings, each against the reader and refused for the reason. */
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

/* Checks the decoder on random changes of a valid signature. */
static void
check_decode_mutated(void)
{
	uint8_t value_a[80];
	uint8_t value_b[80];
	uint8_t sig[400];
	uint8_t mutated[400];
	size_t mutated_size;
	size_t count;
	size_t sig_size;
	size_t index;
	size_t which;
	unsigned roll;
	unsigned iteration;

	/* A valid signature to change: r has a leading byte of one, s has its top bit set, so that s needs a sign octet. */
	value_a[0] = 0x01U;
	random_bytes(value_a + 1, 31U);
	random_bytes(value_b, 32U);
	value_b[0] |= 0x80U;
	sig_size = build_signature(value_a, 32U, value_b, 32U, sig, sizeof(sig));
	CHECK(sig_size > 0U);

	/* Random changes of a valid signature: the decoder and the reader agree on each. */
	for (iteration = 0; iteration < 40000U && sig_size > 0U; iteration++) {
		/* A copy of the signature to change. */
		memcpy(mutated, sig, sig_size);
		mutated_size = sig_size;

		/* One to three changes: a random byte, a flipped bit, a byte of zeros or ones, or a byte cut off the end. */
		count = 1U + random32() % 3U;
		while (count-- > 0U) {
			index = random32() % mutated_size;
			which = random32() % 4U;
			if (which == 0U) {
				mutated[index] = (uint8_t)random32();
			} else if (which == 1U) {
				mutated[index] ^= (uint8_t)(1U << (random32() % 8U));
			} else if (which == 2U) {
				roll = random32() % 3U;
				mutated[index] = 0xffU;
				if (roll == 0U)
					mutated[index] = 0x00U;
			} else if (mutated_size > 2U) {
				mutated_size -= random32() % 2U;
			}
		}

		/* The changed signature at a field size that fits and one that is wider. */
		(void)decode_against_reference(mutated, mutated_size, 32U);
		(void)decode_against_reference(mutated, mutated_size, 33U);
	}
}

/* Checks the decoder on pure random bytes of every length up to 80. */
static void
check_decode_noise(void)
{
	uint8_t noise[96];
	size_t noise_size;
	unsigned iteration;

	/* Pure random bytes of every length up to 80. */
	for (iteration = 0; iteration < 20000U; iteration++) {
		noise_size = random32() % 81U;
		random_bytes(noise, noise_size);

		/* Half of them start like a SEQUENCE of the right length. */
		if (noise_size > 2U && iteration % 2U == 0U) {
			noise[0] = 0x30U;
			noise[1] = (uint8_t)(noise_size - 2U);
		}

		/* The decoder against the reader. */
		(void)decode_against_reference(noise, noise_size, 32U);
	}
}

/* Checks the decoder on built signatures of every width, and on the hand-made malformed ones. */
static void
check_decode(void)
{
	/* Values of every width and shape. */
	check_decode_widths();

	/* The single values around the sign octet and the zero. */
	check_decode_singles();

	/* The hand-made malformed encodings. */
	check_decode_malformed();

	/* Random changes of a valid signature. */
	check_decode_mutated();

	/* Pure random bytes. */
	check_decode_noise();
}

/* ------------------------------------------------------------------ */
/* The vectors. */


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
	size_t text_size;

	/* Decodes (a value of an odd number of digits gets a leading zero digit), then removes the sign octet and other leading zeros. */
	text_size = strlen(text);
	if (text_size % 2U != 0U && text_size < sizeof(padded) - 1U) {
		padded[0] = '0';
		strcpy(padded + 1, text);
		text = padded;
	}

	/* The digits as bytes. */
	length = hex_decode(text, raw, sizeof(raw));
	if (length == (size_t)-1)
		return (size_t)-1;

	/* The leading zero bytes are dropped, and the rest must fit the room. */
	for (skip = 0; skip < length && raw[skip] == 0U; skip++)
		continue;
	if (length - skip > room)
		return (size_t)-1;
	memcpy(out, raw + skip, length - skip);

	/* Reports the size of the value without its leading zeros. */
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
	if (fixture->loaded ||
	    message_size == 0U ||
	    message_size > sizeof(fixture->message))
		return;
	if (signature_size > sizeof(fixture->signature))
		return;

	/* Copies the key, and the buffers it points to into the fixture. */
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

	/* Copies the message and the signature, and points the key at the fixture's own buffers. */
	memcpy(fixture->message, message, message_size);
	fixture->message_size = message_size;
	memcpy(fixture->signature, signature, signature_size);
	fixture->signature_size = signature_size;
	fixture_relink(fixture);

	/* Marks the fixture as taken. */
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
	const char *value;
	const char *sha;
	const char *curve;
	size_t message_size;
	size_t signature_size;
	size_t point_size;
	int index;
	int error;
	int other;
	int compare;
	int more;
	unsigned long valid;
	unsigned long invalid;

	/* Opens the file. */
	valid = 0;
	invalid = 0;
	point_size = 0;
	memset(&key, 0, sizeof(key));
	error = vec_open(&file, name);
	if (error != 0) {
		CHECK(0);
		return;
	}

	/* Reads every record: a key record sets the key, a case record is verified under it. */
	for (;;) {
		more = vec_next(&file);
		if (!more)
			break;

		/* A group's key is in a record of its own, before the group's cases. */
		value = vec_get(&file, "group.publicKey.uncompressed");
		if (value != NULL) {
			point_size = vec_hex(&file, "group.publicKey.uncompressed", point, sizeof(point));
			curve = vec_get(&file, "group.publicKey.curve");
			CHECK(point_size != (size_t)-1 && curve != NULL);
			memset(&key, 0, sizeof(key));
			key.type = CRYC_KEY_EC;

			/* The curve is P-256 when the file says secp256r1, and P-384 otherwise. */
			key.curve = CRYC_CURVE_P384;
			if (curve != NULL) {
				compare = strcmp(curve, "secp256r1");
				if (compare == 0)
					key.curve = CRYC_CURVE_P256;
			}

			/* The point of the record is the key's point. */
			key.point = point;
			key.point_size = point_size;
			continue;
		}

		/* The case: its verdict, hash, message and signature. */
		result = vec_get(&file, "result");
		sha = vec_get(&file, "group.sha");
		message_size = vec_hex(&file, "msg", message, sizeof(message));
		signature_size = vec_hex(&file, "sig", signature, sizeof(signature));
		CHECK(result != NULL && sha != NULL && message_size != (size_t)-1);
		if (result == NULL ||
		    sha == NULL ||
		    message_size == (size_t)-1 ||
		    key.type != CRYC_KEY_EC)
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

		/* Verifies the case; an empty message gives the same answer as no message. */
		count_ecdsa_wycheproof++;
		error = verify(&key, ecdsa_alg[index], 0U, message, message_size, signature, signature_size);
		if (message_size == 0U)
			CHECK(verify(&key, ecdsa_alg[index], 0U, NULL, 0U, signature, signature_size) == error);

		/* A valid case is accepted; an invalid one is refused as a bad signature. */
		compare = strcmp(result, "valid");
		if (compare == 0) {
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

	/* Closes the file and reports the counts. */
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
	size_t size;

	/* The field's text. */
	value = vec_get(file, key);
	if (value == NULL)
		return (size_t)-1;

	/* The number, stripped of its leading zeros. */
	size = hex_stripped(value, out, room);

	/* Reports the size of the number, or (size_t)-1 for a bad field. */
	return size;
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
	size_t size;

	/* Even texts as they are, odd texts with a zero digit in front. */
	value = vec_get(file, key);
	if (value == NULL)
		return (size_t)-1;
	length = strlen(value);
	if (length % 2U == 0U) {
		size = hex_decode(value, out, room);

		/* Reports the size of the bytes, or (size_t)-1 for bad text. */
		return size;
	}

	/* The odd text must fit the buffer with its zero digit. */
	if (length + 2U > sizeof(padded))
		return (size_t)-1;

	/* The odd text with a zero digit in front. */
	padded[0] = '0';
	memcpy(padded + 1, value, length + 1U);
	size = hex_decode(padded, out, room);

	/* Reports the size of the bytes, or (size_t)-1 for bad text. */
	return size;
}

/* Returns the last section key of the record that names a curve ("P-256,SHA-256"), or NULL. */
static const char *
record_section(
	const struct vec_file *file)
{
	size_t index;
	const char *key;
	int compare;

	/* From the last key backwards: the first section key that starts with a curve name. */
	for (index = file->count; index > 0U; index--) {
		key = file->keys[index - 1U];
		compare = strncmp(key, "section.", 8);
		if (compare == 0 && key[9] == '-')
			return key + 8;
	}

	/* No section key of the record names a curve. */
	return NULL;
}

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
	const char *value;
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
	int expected_error;
	int error;
	int compare;
	int more;
	unsigned long bad[2];

	/* The file. */
	bad[0] = 0;
	bad[1] = 0;
	error = vec_open(&file, "cavp-186-4ecdsatestvectors-SigVer");
	if (error != 0) {
		CHECK(0);
		return;
	}

	/* Reads every record; those of a curve and hash the module has are verified. */
	for (;;) {
		more = vec_next(&file);
		if (!more)
			break;

		/* A curve and a hash the module has. */
		section = record_section(&file);
		if (section == NULL)
			continue;
		compare = strncmp(section, "P-256,", 6);
		if (compare == 0) {
			curve_index = 0;
		} else {
			compare = strncmp(section, "P-384,", 6);
			if (compare != 0)
				continue;
			curve_index = 1;
		}

		/* The hash after the curve. */
		hash = hash_index(section + 6);
		if (hash < 0)
			continue;

		/* The size of a coordinate: 32 bytes for P-256 and 48 for P-384. */
		size = 48U;
		if (curve_index == 0)
			size = 32U;

		/* The fields. */
		message_size = vec_hex(&file, "Msg", message, sizeof(message));
		result = vec_get(&file, "Result");
		r_size = record_number(&file, "R", r, sizeof(r));
		s_size = record_number(&file, "S", s, sizeof(s));

		/* The coordinates of the public key, after the form byte. */
		x_size = (size_t)-1;
		value = vec_get(&file, "Qx");
		if (value != NULL)
			x_size = hex_decode(value, public_key + 1, size);
		y_size = (size_t)-1;
		value = vec_get(&file, "Qy");
		if (value != NULL)
			y_size = hex_decode(value, public_key + 1U + size, size);
		CHECK(message_size != (size_t)-1 && result != NULL);
		CHECK(r_size != (size_t)-1 && s_size != (size_t)-1);
		CHECK(x_size == size && y_size == size);
		if (message_size == (size_t)-1 ||
		    result == NULL ||
		    r_size == (size_t)-1 ||
		    s_size == (size_t)-1 ||
		    x_size != size ||
		    y_size != size)
			continue;

		/* The public key as an uncompressed point of the curve. */
		public_key[0] = 0x04U;
		memset(&key, 0, sizeof(key));
		key.type = CRYC_KEY_EC;
		key.curve = CRYC_CURVE_P384;
		if (curve_index == 0)
			key.curve = CRYC_CURVE_P256;
		key.point = public_key;
		key.point_size = 1U + 2U * size;

		/* r and s as they are (zero is still an INTEGER), built into DER by the writer. */
		if (r_size == 0U) {
			r[0] = 0;
			r_size = 1;
		}

		/* The same for s. */
		if (s_size == 0U) {
			s[0] = 0;
			s_size = 1;
		}

		/* The signature as DER. */
		sig_size = build_signature(r, r_size, s, s_size, sig, sizeof(sig));
		CHECK(sig_size > 0U);
		if (sig_size == 0U)
			continue;

		/* Verifies the case, and compares the answer with the file's verdict. */
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
			expected_error = EBADMSG;
			if (key_error != 0)
				expected_error = EINVAL;
			CHECK(error == expected_error);
			bad[curve_index]++;
		}
	}

	/* Closes the file and reports the counts. */
	vec_close(&file);
	printf("  CAVP SigVer: %lu valid, %lu / %lu bad (P-256 / P-384)\n", count_ecdsa_cavp_valid, bad[0], bad[1]);

	/* Every curve and hash has valid cases. */
	for (curve_index = 0; curve_index < 2; curve_index++) {
		for (hash = 0; hash < 3; hash++)
			CHECK(cavp_valid[curve_index][hash] >= 3U);
	}

	/* Both curves have bad cases. */
	CHECK(bad[0] >= 30U && bad[1] >= 30U);
}

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
	const char *flag_found;
	size_t message_size;
	size_t signature_size;
	size_t size;
	int index;
	int error;
	int compare;
	int more;
	unsigned long valid;
	unsigned long total;

	/* Opens the file. */
	valid = 0;
	total = 0;
	memset(&group, 0, sizeof(group));
	error = vec_open(&file, name);
	if (error != 0) {
		CHECK(0);
		return;
	}

	/* Reads every record: the group's fields are kept, and a record with a tcId is a case. */
	for (;;) {
		more = vec_next(&file);
		if (!more)
			break;

		/* The group's fields come before the case's, in this record or an earlier one. */
		value = vec_get(&file, "group.publicKey.modulus");
		if (value != NULL) {
			size = hex_stripped(value, group.n, sizeof(group.n));
			CHECK(size != (size_t)-1);
			group.n_size = size;
			if (size == (size_t)-1)
				group.n_size = 0U;
		}

		/* The public exponent of the group. */
		value = vec_get(&file, "group.publicKey.publicExponent");
		if (value != NULL) {
			size = hex_stripped(value, group.e, sizeof(group.e));
			CHECK(size != (size_t)-1);
			group.e_size = size;
			if (size == (size_t)-1)
				group.e_size = 0U;
		}

		/* The hash of the group. */
		value = vec_get(&file, "group.sha");
		if (value != NULL) {
			strncpy(group.sha, value, sizeof(group.sha) - 1U);
			group.sha[sizeof(group.sha) - 1U] = '\0';
		}

		/* The salt length of the group, for PSS. */
		value = vec_get(&file, "group.sLen");
		if (value != NULL) {
			group.salt_size = (size_t)strtoul(value, NULL, 10);
			group.have_salt_size = 1;
		}

		/* A record without a case number holds only group fields. */
		value = vec_get(&file, "tcId");
		if (value == NULL)
			continue;

		/* The case. */
		index = hash_index(group.sha);
		CHECK(index >= 0 && group.n_size > 0U && group.e_size > 0U);
		message_size = vec_hex(&file, "msg", message, sizeof(message));
		signature_size = vec_hex(&file, "sig", signature, sizeof(signature));
		result = vec_get(&file, "result");
		flags = vec_get(&file, "flags");
		CHECK(message_size != (size_t)-1 && signature_size != (size_t)-1 && result != NULL && flags != NULL);
		if (index < 0 ||
		    group.n_size == 0U ||
		    message_size == (size_t)-1 ||
		    signature_size == (size_t)-1 ||
		    result == NULL ||
		    flags == NULL)
			continue;
		if (kind == 1)
			CHECK(group.have_salt_size);

		/* The group's key. */
		memset(&key, 0, sizeof(key));
		key.type = CRYC_KEY_RSA;
		key.rsa.n = group.n;
		key.rsa.n_size = group.n_size;
		key.rsa.e = group.e;
		key.rsa.e_size = group.e_size;

		/* Verifies the case with the scheme of the file. */
		total++;
		count_rsa_wycheproof++;
		if (kind == 0)
			error = verify(&key, pkcs1_alg[index], 0U, message, message_size, signature, signature_size);
		else
			error = verify(&key, pss_alg[index], group.salt_size, message, message_size, signature, signature_size);

		/* valid: accepted; invalid: refused as a bad signature; acceptable: either, but a DigestInfo without NULL is refused. */
		compare = strcmp(result, "valid");
		if (compare == 0) {
			CHECK(error == 0);
			valid++;
			count_rsa_wycheproof_valid++;
			if (kind == 0 && index == 0)
				fixture_keep(&fixture_pkcs1, &key, pkcs1_alg[index], 0U, message, message_size, signature, signature_size);
			if (kind == 0 && index == 2 && group.n_size > 400U)
				fixture_keep(&fixture_pkcs1_sha512, &key, pkcs1_alg[index], 0U, message, message_size, signature, signature_size);
			if (kind == 1 && index == 0)
				fixture_keep(&fixture_pss, &key, pss_alg[index], group.salt_size, message, message_size, signature, signature_size);
		} else {
			compare = strcmp(result, "invalid");
			if (compare == 0) {
				CHECK(error == EBADMSG);
			} else {
				CHECK(error == 0 || error == EBADMSG);
				flag_found = strstr(flags, "MissingNull");
				if (flag_found != NULL)
					CHECK(error == EBADMSG);
			}
		}
	}

	/* Closes the file and reports the counts. */
	vec_close(&file);
	printf("  %s: %lu cases, %lu valid\n", name, total, valid);
	CHECK(total > 90U && valid > 5U);
}

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
	int more;
	unsigned long valid[3];
	unsigned long total;

	/* Opens the file. */
	n_size = 0;
	valid[0] = 0;
	valid[1] = 0;
	valid[2] = 0;
	total = 0;
	error = vec_open(&file, name);
	if (error != 0) {
		CHECK(0);
		return;
	}

	/* A record with an "n" sets the modulus; a record with a SHAAlg is a case. */
	for (;;) {
		more = vec_next(&file);
		if (!more)
			break;

		/* The modulus of the following cases. */
		value = vec_get(&file, "n");
		if (value != NULL) {
			n_size = hex_stripped(value, n, sizeof(n));
			CHECK(n_size != (size_t)-1);
			if (n_size == (size_t)-1)
				n_size = 0;
			continue;
		}

		/* A case names its hash; any other record is skipped. */
		value = vec_get(&file, "SHAAlg");
		if (value == NULL)
			continue;
		index = hash_index(value);
		if (index < 0)
			continue;
		CHECK(n_size > 0U);

		/* The fields of the case. */
		e_size = record_number(&file, "e", e, sizeof(e));
		message_size = record_bytes(&file, "Msg", message, sizeof(message));
		signature_size = record_bytes(&file, "S", signature, sizeof(signature));
		result = vec_get(&file, "Result");
		salt_size = 0;
		if (kind == 1) {
			salt_size = record_bytes(&file, "SaltVal", salt, sizeof(salt));
			CHECK(salt_size != (size_t)-1);
		}

		/* Every field must be there. */
		CHECK(e_size != (size_t)-1 && message_size != (size_t)-1 && signature_size != (size_t)-1 && result != NULL);
		if (n_size == 0U ||
		    e_size == (size_t)-1 ||
		    message_size == (size_t)-1 ||
		    signature_size == (size_t)-1 ||
		    result == NULL ||
		    salt_size == (size_t)-1)
			continue;

		/* The key of the case. */
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

		/* Verifies the case with the scheme of the file. */
		if (kind == 0)
			error = verify(&key, pkcs1_alg[index], 0U, message, message_size, signature, signature_size);
		else
			error = verify(&key, pss_alg[index], salt_size, message, message_size, signature, signature_size);
		total++;
		count_rsa_cavp++;

		/* A key below 2048 bits is refused; otherwise the file's verdict decides. */
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

	/* Closes the file and reports the counts. */
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
	int error;

	/* The key, the algorithm and salt, the message and the signature of the fixture. */
	error = verify(&fixture->key, fixture->alg, fixture->salt_size, fixture->message, fixture->message_size, fixture->signature, fixture->signature_size);

	/* Reports why the fixture does not verify. */
	if (error != 0)
		return error;

	/* Succeeded: the fixture verifies. */
	return 0;
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

	/* With every bit put back the message verifies again. */
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

	/* One byte more than the signature, which is zero. */
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

	/* With every bit put back the signature verifies again. */
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
		work.alg = pkcs1_alg[other];
		if (is_pss)
			work.alg = pss_alg[other];
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

	/* Takes the EC point out of the key again. */
	work.key.curve = CRYC_CURVE_NONE;
	work.key.point = NULL;
	work.key.point_size = 0U;
	work.alg = fixture->alg;

	/* The other scheme with each hash gives a bad signature (the encodings differ), with the right salt length for PSS. */
	for (other = 0; other < 3; other++) {
		work.alg = pss_alg[other];
		if (is_pss)
			work.alg = pkcs1_alg[other];
		work.salt_size = hash_bytes[other];
		CHECK(fixture_verify(&work) == EBADMSG);
	}

	/* Starts again from the fixture as it was. */
	fixture_copy(&work, fixture);

	/* A salt length other than the signature's, for PSS (the decoded length must be exactly the required one). */
	if (is_pss) {
		for (salt = 0; salt < 3U; salt++) {
			work.salt_size = fixture->salt_size + 1U + salt;
			CHECK(fixture_verify(&work) == EBADMSG);
		}

		/* Other salt lengths: one short, none, one byte, the modulus size and one more, and impossible ones. */
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

	/* The RSA key is taken out again. */
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

	/* The point has its whole size again. */
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

	/* The form byte is uncompressed again. */
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
	other_curve = CRYC_CURVE_P256;
	if (fixture->key.curve == CRYC_CURVE_P256)
		other_curve = CRYC_CURVE_P384;
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
		fixture = &fixture_p256;
		if (index == 0U)
			fixture = &fixture_pkcs1;
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
		params.alg = CRYC_SIG_ECDSA_SHA256;
		if (index == 0U)
			params.alg = CRYC_SIG_RSA_PKCS1_SHA256;
		else if (index == 1U)
			params.alg = CRYC_SIG_RSA_PSS_SHA384;
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
	if (error == 0)
		return 1;
	if (error == EBADMSG)
		return 1;
	if (error == EINVAL)
		return 1;
	if (error == ENOTSUP)
		return 1;

	/* Any other error is not one the header names. */
	return 0;
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
	const struct fixture *fixture;
	uint8_t r[48];
	uint8_t s[48];
	size_t n_size;
	size_t signature_size;
	unsigned iteration;
	int error;
	unsigned long accepted;

	/* RSA: random odd moduli of 256 to 1024 bytes, random exponents, random signatures. */
	accepted = 0;
	for (iteration = 0; iteration < 300U; iteration++) {
		/* A random modulus: with its top bit set in two iterations of three, and odd except in every seventh. */
		n_size = 200U + random32() % 825U;
		random_bytes(n, n_size);
		if (iteration % 3U != 0U)
			n[0] |= 0x80U;
		if (iteration % 7U != 0U)
			n[n_size - 1U] |= 0x01U;

		/* The key: the exponent is up to four bytes of the modulus. */
		memset(&key, 0, sizeof(key));
		key.type = CRYC_KEY_RSA;
		key.rsa.n = n;
		key.rsa.n_size = n_size;
		key.rsa.e = n + n_size - 4U;
		key.rsa.e_size = 1U + random32() % 4U;

		/* A random signature, of the modulus size in every other iteration. */
		signature_size = n_size;
		if (iteration % 2U != 0U)
			signature_size = random32() % 1100U;
		random_bytes(signature, signature_size);
		random_bytes(message, sizeof(message));

		/* A random RSA algorithm, and the answer must be one of the four results. */
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
		/* The key of a P-256 or a P-384 fixture in turn. */
		fixture = &fixture_p384;
		if (iteration % 2U == 0U)
			fixture = &fixture_p256;
		key = fixture->key;
		params.alg = ecdsa_alg[random32() % 3U];
		params.salt_size = 0U;

		/* Random bytes in one iteration of three, otherwise a well-formed SEQUENCE of two random integers. */
		if (iteration % 3U == 0U) {
			signature_size = random32() % 120U;
			random_bytes(signature, signature_size);
		} else {
			random_bytes(r, sizeof(r));
			random_bytes(s, sizeof(s));
			signature_size = build_signature(r, 32U + (iteration % 17U), s, 32U + (iteration % 13U), signature, sizeof(signature));
		}

		/* The answer must be one of the four results. */
		error = cryc_signature_verify(&key, &params, message, random32() % sizeof(message), signature, signature_size);
		CHECK(verify_error_known(error));
		if (error == 0)
			accepted++;
	}

	/* ECDSA with random points (nearly all off the curve), and random curve and form. */
	for (iteration = 0; iteration < 200U; iteration++) {
		/* The curve alternates; the form byte is 0x04 except in every fourth iteration. */
		memset(&key, 0, sizeof(key));
		key.type = CRYC_KEY_EC;
		key.curve = CRYC_CURVE_P384;
		if (iteration % 2U == 0U)
			key.curve = CRYC_CURVE_P256;
		random_bytes(point, sizeof(point));
		point[0] = 0x04U;
		if (iteration % 4U == 0U)
			point[0] = (uint8_t)random32();
		key.point = point;

		/* The size of the point is the right one except in every fifth iteration. */
		key.point_size = 97U;
		if (key.curve == CRYC_CURVE_P256)
			key.point_size = 65U;
		if (iteration % 5U == 0U)
			key.point_size = random32() % 100U;

		/* A random signature, which must be refused with one of the four results. */
		params.alg = ecdsa_alg[random32() % 3U];
		params.salt_size = 0U;
		signature_size = random32() % 120U;
		random_bytes(signature, signature_size);
		error = cryc_signature_verify(&key, &params, message, 10U, signature, signature_size);
		CHECK(verify_error_known(error));
		CHECK(error != 0);
	}

	/* Nothing random was accepted. */
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

	/* Measures the file, then rewinds it. */
	fseek(stream, 0, SEEK_END);
	length = ftell(stream);
	fseek(stream, 0, SEEK_SET);

	/* Allocates room for the data and a NUL. */
	data = malloc((size_t)length + 1U);
	if (data == NULL) {
		fclose(stream);
		return NULL;
	}

	/* Reads the whole file and ends the data with a NUL. */
	*size = fread(data, 1, (size_t)length, stream);
	fclose(stream);
	data[*size] = '\0';

	/* Reports the data, which the caller frees. */
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

	/* Reports the bit length of the modulus. */
	return bits;
}

/* Returns the result the header gives for verifying cert's signature with key under cert's own algorithm. */
static int
expected_for(
	const struct cryc_x509 *cert,
	const struct cryc_pubkey *key)
{
	size_t bits;
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
	if (key->type == CRYC_KEY_RSA) {
		bits = modulus_bits(&key->rsa);
		if (bits < 2048U)
			return ENOTSUP;
	}

	/* Succeeded: the pair is within what the header supports. */
	return 0;
}

/* Verifies the signature of cert with key. */
static int
verify_certificate(
	const struct cryc_x509 *cert,
	const struct cryc_pubkey *key)
{
	int error;

	/* The tbsCertificate is the message, the signatureValue the signature. */
	error = verify(key, cert->signature_alg.alg, cert->signature_alg.salt_size, cert->tbs, cert->tbs_size, cert->signature, cert->signature_size);

	/* Reports why the signature does not verify. */
	if (error != 0)
		return error;

	/* Succeeded: the certificate's signature verifies under the key. */
	return 0;
}

/* Verifies the self-signature of every root of the bundle, then changes each in one bit and under other keys. */
static void
check_bundle(void)
{
	static uint8_t work[LIMBO_MAX];
	struct bundle_entry *entries;
	const struct cryc_x509 *cert;
	const struct cryc_x509 *issuer;
	const char *path;
	char *text;
	uint8_t *der;
	size_t positions[3];
	size_t size;
	size_t position;
	size_t count;
	size_t capacity;
	size_t index;
	size_t other;
	size_t bit_at;
	size_t at;
	size_t der_size;
	size_t unsupported_alg;
	size_t unsupported_key;
	size_t rsa_pkcs1;
	size_t rsa_pss;
	size_t ecdsa;
	int error;
	int want;
	int compare;

	/* The bundle must be readable: a missing file is a failure, not a pass. */
	path = getenv("WS335_CACERT");
	if (path == NULL)
		path = CACERT_DEFAULT;
	text = read_file(path, &size);
	CHECK(text != NULL);
	if (text == NULL)
		return;

	/* Room for the certificates. */
	capacity = 400U;
	entries = calloc(capacity, sizeof(*entries));
	CHECK(entries != NULL);
	if (entries == NULL) {
		free(text);
		return;
	}

	/* Every certificate, parsed and kept. */
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

	/* The text is no longer needed once the certificates are parsed. */
	free(text);
	CHECK(count > 100U);

	/* The self-signature of each root. */
	unsupported_alg = 0;
	unsupported_key = 0;
	rsa_pkcs1 = 0;
	rsa_pss = 0;
	ecdsa = 0;
	for (index = 0; index < count; index++) {
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

	/* The counts of the self-signatures. */
	printf("  bundle: %lu certificates, %lu verified (%lu RSA PKCS#1, %lu RSA-PSS, %lu ECDSA), %lu ENOTSUP (%lu unsupported signature algorithm such as SHA-1, %lu unsupported key)\n", (unsigned long)count, count_bundle_ok, (unsigned long)rsa_pkcs1, (unsigned long)rsa_pss, (unsigned long)ecdsa, count_bundle_notsup, (unsigned long)unsupported_alg, (unsigned long)unsupported_key);
	CHECK(count_bundle_ok > 100UL);
	CHECK(rsa_pkcs1 > 20U && ecdsa > 20U);
	CHECK(count_bundle_ok + count_bundle_notsup == (unsigned long)count);

	/* One bit of the tbsCertificate changed (first, middle, last byte) or of the signature: a verifying root no longer verifies. */
	for (index = 0; index < count; index++) {
		/* Only a root whose own signature verifies is changed. */
		cert = &entries[index].cert;
		want = expected_for(cert, &cert->key);
		if (want != 0)
			continue;

		/* One bit of the tbsCertificate at its first, middle and last byte. */
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

		/* One bit of the signature at its first, middle and last byte. */
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
			other = (index + bit_at) % count;
			cert = &entries[index].cert;
			issuer = &entries[other].cert;
			want = expected_for(cert, &issuer->key);
			error = verify_certificate(cert, &issuer->key);
			if (want == 0)
				want = EBADMSG;

			/* The same EC key under both certificates verifies as the certificate does under its own key. */
			if (cert->key.curve == issuer->key.curve &&
			    cert->key.type == issuer->key.type &&
			    cert->key.type == CRYC_KEY_EC &&
			    issuer->key.point_size == cert->key.point_size) {
				compare = memcmp(issuer->key.point, cert->key.point, cert->key.point_size);
				if (compare == 0)
					want = 0;
			}

			/* The same RSA modulus under both certificates does too. */
			if (cert->key.type == CRYC_KEY_RSA &&
			    issuer->key.type == CRYC_KEY_RSA &&
			    issuer->key.rsa.n_size == cert->key.rsa.n_size) {
				compare = memcmp(issuer->key.rsa.n, cert->key.rsa.n, cert->key.rsa.n_size);
				if (compare == 0)
					want = 0;
			}

			/* The pair verifies as expected, or as the certificate does under its own key when the keys are the same. */
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
	int error;

	/* An absent key is the end of the list. */
	text = vec_get(file, key);
	if (text == NULL)
		return 0;

	/* The certificate's bytes. */
	length = strlen(text) / 2U;
	der = malloc(length + 1U);
	CHECK(der != NULL);
	if (der == NULL)
		return 0;
	size = hex_decode(text, der, length);
	CHECK(size == length);

	/* Keeps the bytes in the next entry. */
	entries[*count].der = der;
	entries[*count].der_size = size;
	if (size == (size_t)-1)
		entries[*count].der_size = 0U;

	/* The entry is parsed when the bytes decoded and the certificate parses. */
	entries[*count].parsed = 0;
	if (size != (size_t)-1) {
		error = cryc_x509_parse(&entries[*count].cert, der, size);
		if (error == 0)
			entries[*count].parsed = 1;
	}

	/* Counts the entry. */
	*count += 1U;

	/* Succeeded: there was a certificate, and it is kept. */
	return 1;
}

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

	/* Reports the hash. */
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

	/* A pair not seen before is verified and remembered. */
	error = verify_certificate(cert, &issuer->key);
	pair_cache[slot].key = key;
	pair_cache[slot].result = error;
	count_limbo_distinct++;

	/* Reports why the pair does not verify. */
	if (error != 0)
		return error;

	/* Succeeded: the pair verifies. */
	return 0;
}

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
	const struct cryc_x509 *cert;
	const struct cryc_x509 *issuer;
	const char *verdict;
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
	int compare;
	int loaded;
	int more;

	/* The records. */
	entries = calloc(130U, sizeof(*entries));
	pair_cache = calloc(PAIR_SLOTS, sizeof(*pair_cache));
	CHECK(entries != NULL && pair_cache != NULL);
	if (entries == NULL || pair_cache == NULL) {
		free(entries);
		free(pair_cache);
		return;
	}

	/* Opens the cases. */
	error = vec_open(&file, "limbo");
	if (error != 0) {
		CHECK(0);
		free(entries);
		free(pair_cache);
		return;
	}

	/* Reads every case of the file. */
	records = 0;
	for (;;) {
		more = vec_next(&file);
		if (!more)
			break;
		records++;
		verdict = vec_get(&file, "expected");
		compare = strcmp(verdict, "SUCCESS");
		expect_success = 0;
		if (compare == 0)
			expect_success = 1;

		/* The peer, the trusted and the untrusted certificates. */
		count = 0;
		(void)limbo_load(&file, "peer", entries, &count);
		for (index = 0; index < 4U; index++) {
			sprintf(key, "trusted.%u", (unsigned)index);
			loaded = limbo_load(&file, key, entries, &count);
			if (!loaded)
				break;
		}

		/* The untrusted certificates, as many as the room left allows. */
		for (index = 0; index < 120U && count < 125U; index++) {
			sprintf(key, "untrusted.%u", (unsigned)index);
			loaded = limbo_load(&file, key, entries, &count);
			if (!loaded)
				break;
		}

		/* Each certificate against the certificates that could have issued it: all of them in a SUCCESS case, the first four of one case in eight of the others. */
		index_end = count;
		if (!expect_success && count >= 4U)
			index_end = 4U;
		if (!expect_success && records % 8U != 0U)
			index_end = 0U;
		for (index = 0; index < index_end; index++) {
			candidates = 0;
			verified = 0;
			unsupported = 0;
			if (!entries[index].parsed)
				continue;
			for (other = 0; other < count; other++) {
				if (!entries[other].parsed)
					continue;
				cert = &entries[index].cert;
				issuer = &entries[other].cert;

				/* A candidate issuer has the certificate's issuer Name as its subject Name. */
				if (cert->issuer_size != issuer->subject_size)
					continue;
				compare = memcmp(cert->issuer, issuer->subject, cert->issuer_size);
				if (compare != 0)
					continue;

				/* Verifies the pair; the answer must be one of the four results. */
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

			/* In a SUCCESS case the peer's issuer is in the case: one candidate verifies, or the peer is outside the range. */
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

		/* Releases the certificates of this case. */
		for (index = 0; index < count; index++)
			free(entries[index].der);
	}

	/* Closes the file and releases the tables. */
	vec_close(&file);
	free(entries);
	free(pair_cache);
	pair_cache = NULL;

	/* The counts, and what the whole file must show. */
	printf("  limbo: %lu cases, %lu issuer pairs tried (%lu distinct), %lu verified; SUCCESS cases with an issuer in the case %lu (peer verified %lu, unsupported %lu)\n", records, count_limbo_pairs, count_limbo_distinct, count_limbo_ok, limbo_success_with_issuer, limbo_success_verified, limbo_success_unsupported);
	CHECK(records == 9792UL);
	CHECK(limbo_success_with_issuer > 900UL);
	CHECK(limbo_success_verified + limbo_success_unsupported == limbo_success_with_issuer);
}
