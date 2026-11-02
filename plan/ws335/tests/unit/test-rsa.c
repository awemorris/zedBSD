/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks RSA signature verification, RSASSA-PKCS1-v1_5 and RSASSA-PSS
 * (cryc-rsa.h).
 *
 * The published vectors are read from the record files: the CAVP SigVer
 * files of FIPS 186-3 (PKCS1 v1.5 and PSS; SHA-256, SHA-384 and SHA-512;
 * 2048 and 3072 bits, and the 1024-bit cases which must be refused) and the
 * Wycheproof RSA signature and RSA-PSS files.
 *
 * Further cases are made here with a signer written from RFC 8017 (EMSA-
 * PKCS1-v1_5-ENCODE with the DigestInfo built by the DER writer, EMSA-PSS-
 * ENCODE, MGF1, RSASP1 with the private exponent).  The signer reproduces
 * the NIST PSS signatures byte for byte, which checks it against the
 * standard's vectors.  Keys with the private exponent come from the CAVP
 * file and from moduli built here as products of Mersenne primes (the
 * verification arithmetic does not care that the primes are special; each
 * key's private exponent comes from phi by a small division, and a key that
 * were wrong would fail its own round trip).  The moduli are chosen to give
 * every length of modBits modulo 8, including the one where EM is one byte
 * shorter than the modulus, and the 8192-bit limit.
 *
 * Forged signatures are made by signing a deliberately wrong encoded
 * message (every field of the padding changed, a hash that is not the
 * message's, a different DigestInfo, a PSS block with each field changed)
 * and by changing the signature (s + n, truncated, lengthened).  Argument
 * errors, key limits, salt lengths at and beyond the limits, and random
 * input are checked too.
 *
 * Expected values come from the standards' vectors and from the signer
 * here, not from another implementation.
 */

#include "check.h"
#include "vecfile.h"
#include "derwriter.h"

#include "userland/base/libcrypto-compat/cryc-bn.h"
#include "userland/base/libcrypto-compat/cryc-hash.h"
#include "userland/base/libcrypto-compat/cryc-rsa.h"

/* The largest modulus in bytes. */
#define K_MAX	1024U

/* The limbs of the test's own big integers (enough for the product of the primes of an 8192-bit key). */
#define BIG_LIMBS	300U

/* The largest hash in bytes. */
#define HASH_MAX	64U

/* A natural number of the test's own, for building keys: limbs little endian, used limbs without leading zeros. */
struct big {
	uint32_t limb[BIG_LIMBS];
	size_t used;
};

/* A key with its private exponent: all big-endian bytes without leading zeros. */
struct tkey {
	uint8_t n[K_MAX];
	size_t n_size;
	uint8_t e[8];
	size_t e_size;
	uint8_t d[K_MAX];
	size_t d_size;
	size_t bits;
};

/* The moduli built from Mersenne primes: the exponents of the primes 2^a - 1 and the bit length of the product. */
struct key_spec {
	size_t count;
	unsigned exponents[6];
	size_t bits;
};

/* The state of a Wycheproof file read record by record: the group's key and parameters, which come split over two records. */
struct group_state {
	char modulus[2300];
	char exponent[64];
	char sha[16];
	char mgf_sha[16];
	unsigned long salt_size;
	int have_salt_size;
};

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x2545f491U;

/* The three hashes, in the order the tests loop over them. */
static const enum cryc_hash_id hash_ids[3] = {
	CRYC_HASH_SHA256, CRYC_HASH_SHA384, CRYC_HASH_SHA512
};

/*
 * The moduli: the product of the primes 2^a - 1 has exactly the sum of the
 * a as its bit length.  The bit lengths are 2057, 2074, 2067, 2292, 2077,
 * 2054, 2095 and 2120 (modBits modulo 8 of 1, 2, 3, 4, 5, 6, 7 and 0) and
 * 8192.
 *
 * Values referenced: the Mersenne exponents 31, 61, 89, 107, 127, 521, 607,
 * 1279, 2203, 3217 and 4423 give prime numbers 2^a - 1 (OEIS A000043).
 */
static const struct key_spec key_specs[9] = {
	{ 5U, { 61U, 89U, 107U, 521U, 1279U, 0U }, 2057U },
	{ 4U, { 61U, 127U, 607U, 1279U, 0U, 0U }, 2074U },
	{ 5U, { 31U, 61U, 89U, 607U, 1279U, 0U }, 2067U },
	{ 2U, { 89U, 2203U, 0U, 0U, 0U, 0U }, 2292U },
	{ 5U, { 61U, 89U, 127U, 521U, 1279U, 0U }, 2077U },
	{ 4U, { 61U, 107U, 607U, 1279U, 0U, 0U }, 2054U },
	{ 5U, { 61U, 107U, 127U, 521U, 1279U, 0U }, 2095U },
	{ 4U, { 107U, 127U, 607U, 1279U, 0U, 0U }, 2120U },
	{ 4U, { 31U, 521U, 3217U, 4423U, 0U, 0U }, 8192U }
};

/* The CAVP key of 2048 bits with its private exponent, and one of 3072 bits (loaded from the PSS file). */
static struct tkey cavp_key_2048;
static struct tkey cavp_key_3072;
static int have_cavp_2048;
static int have_cavp_3072;

/* The keys made from the Mersenne primes. */
static struct tkey mersenne_keys[9];

/* Counts of what the vector files held, shown at the end. */
static unsigned long count_cavp_pkcs1;
static unsigned long count_cavp_pkcs1_valid;
static unsigned long count_cavp_pss;
static unsigned long count_cavp_pss_valid;
static unsigned long count_cavp_small;
static unsigned long count_cavp_reproduced;
static unsigned long count_wycheproof;
static unsigned long count_wycheproof_valid;
static unsigned long count_wycheproof_acceptable;

/* Returns the next pseudo-random 32-bit value. */
static uint32_t random32(void);

/* Fills size bytes with pseudo-random bytes. */
static void random_bytes(uint8_t *out, size_t size);

/* The digest size of a hash id, written again here from FIPS 180-4. */
static size_t hash_length(enum cryc_hash_id id);

/* Hashes data with the library's hash (checked by its own tests). */
static void digest(enum cryc_hash_id id, const uint8_t *data, size_t size, uint8_t *out);

/* Drops the zero limbs at the top. */
static void big_trim(struct big *x);

/* Sets x to 2^bits - amount, for amount 1 or 2 and bits of at least 33 or exactly 31. */
static void big_power_of_two_minus(struct big *x, unsigned bits, uint32_t amount);

/* Sets out to x * y (out must not be x or y). */
static void big_multiply(struct big *out, const struct big *x, const struct big *y);

/* Returns x mod m for a 32-bit m. */
static uint32_t big_mod_small(const struct big *x, uint32_t m);

/* Sets x to x * multiplier + addend. */
static void big_multiply_small_add(struct big *x, uint32_t multiplier, uint32_t addend);

/* Sets x to x / divisor (floor) and returns the remainder. */
static uint32_t big_divide_small(struct big *x, uint32_t divisor);

/* Writes x big-endian without leading zero bytes into out (room bytes) and returns the size (0 when it does not fit). */
static size_t big_to_bytes(const struct big *x, uint8_t *out, size_t room);

/* Decodes hex text that may have an odd number of digits (the CAVP files drop a leading zero digit) as a number: an odd text gets a zero digit on the left. */
static size_t hex_number(const char *text, uint8_t *out, size_t room);

/* The key as the library takes it. */
static void public_key(const struct tkey *key, struct cryc_rsa_key *out);

/* Returns the bit length of a big-endian number without leading zero bytes. */
static size_t bit_length(const uint8_t *bytes, size_t size);

/* Builds the key of a spec with e = 65537: n is the product of the primes, phi the product of one less, d = (1 + t * phi) / e for the t below e that makes the numerator divisible (e * d = 1 mod phi). */
static void make_key(struct tkey *key, const struct key_spec *spec);

/* Loads a key from hex text of n, e and d (leading zeros removed); returns 0, or -1 when a value is too big. */
static int load_key(struct tkey *key, const char *n_hex, const char *e_hex, const char *d_hex);

/* RSASP1 on arbitrary bytes: sig = m^d mod n as n_size bytes, for m of m_size bytes read as an integer. */
static int raw_sign(const struct tkey *key, const uint8_t *m, size_t m_size, uint8_t *sig);

/* The DER encoding T of the DigestInfo of hash (id) built with the DER writer: SEQUENCE { SEQUENCE { OID, NULL }, OCTET STRING hash }. */
static size_t digest_info(enum cryc_hash_id oid_id, const uint8_t *hash, size_t hash_size, int with_null, uint8_t *out, size_t room);

/* Builds EM = 0x00 || 0x01 || PS || 0x00 || T of k bytes (RFC 8017 9.2); returns k, or 0 when T does not fit. */
static size_t pkcs1_encode(const uint8_t *t, size_t t_size, size_t k, uint8_t *em);

/* Fills mask with MGF1 (RFC 8017 B.2.1) over seed with hash id. */
static void mgf1(enum cryc_hash_id id, const uint8_t *seed, size_t seed_size, uint8_t *mask, size_t mask_size);

/* EMSA-PSS-ENCODE (RFC 8017 9.1.1) of the hash mhash with the given salt for em_bits: writes em_len = ceil (em_bits / 8) bytes to em and returns em_len, or 0 when the block is too short. */
static size_t pss_encode(enum cryc_hash_id id, const uint8_t *mhash, const uint8_t *salt, size_t salt_size, size_t em_bits, uint8_t *em);

/* Signs the hash with PKCS1 v1.5 (the DigestInfo of id); returns 0, or -1 when it does not fit. */
static int sign_pkcs1(const struct tkey *key, enum cryc_hash_id id, const uint8_t *hash, uint8_t *sig);

/* Signs the hash with PSS (MGF1 with the same hash); returns 0, or -1 when the salt does not fit. */
static int sign_pss(const struct tkey *key, enum cryc_hash_id id, const uint8_t *hash, const uint8_t *salt, size_t salt_size, uint8_t *sig);

/* Verifies with PKCS1 v1.5 against a key of the tests. */
static int verify_pkcs1(const struct tkey *key, enum cryc_hash_id id, const uint8_t *hash, const uint8_t *sig);

/* Verifies with PSS against a key of the tests. */
static int verify_pss(const struct tkey *key, enum cryc_hash_id id, const uint8_t *hash, size_t salt_size, const uint8_t *sig);

/* Checks that the DigestInfo built here has the lengths and prefix bytes of RFC 8017 9.2 note 1. */
static void check_digest_info(void);

/* Round trips of both schemes for one key and one hash with several salt sizes, and the changes that must fail. */
static void round_trip_key(const struct tkey *key, enum cryc_hash_id id, int do_pkcs1, size_t salt_count);

/* Forged PKCS1 v1.5 signatures: a wrong EM signed with the private key, each case refused. */
static void forge_pkcs1(const struct tkey *key, enum cryc_hash_id id);

/* Forged PSS signatures: a wrong EM signed with the private key, each case refused. */
static void forge_pss(const struct tkey *key, enum cryc_hash_id id);

/* The signature as sent changed, and truncated and lengthened signatures, for one valid signature. */
static void check_signature_forms(const struct tkey *key);

/* A valid signature whose first byte is zero, sent without it or with another zero in front: neither is a signature of k bytes. */
static void check_leading_zero_signature(const struct tkey *key);

/* Argument errors and the limits of the key, with one good key and signature. */
static void check_arguments(const struct tkey *key);

/* Salt sizes near the limits with a valid signature: no overflow, always refused. */
static void check_salt_limits(const struct tkey *key);

/* Random keys, hashes, salts and signatures: only the documented errors, and no read outside the buffers (ASan). */
static void check_random_input(void);

/* Returns the hash id named by a vector's text ("SHA256", "SHA-256", "SHA384", ...), or CRYC_HASH_NONE. */
static enum cryc_hash_id hash_from_name(const char *name);

/* Decodes the hex value of key in the current record as a number (see hex_number); (size_t)-1 when absent or malformed. */
static size_t field_number(const struct vec_file *file, const char *key, uint8_t *out, size_t room);

/* Returns 1 when the exponent bytes (leading zeros ignored) are an odd value from 3 to 2^32 - 1. */
static int exponent_is_usable(const struct tkey *key);

/* The CAVP SigVer file of PKCS1 v1.5: the modulus is in the record before its cases. */
static void check_cavp_pkcs1(void);

/* The CAVP SigVer file of PSS: the key and the cases; valid cases are also reproduced with the signer here. */
static void check_cavp_pss(void);

/* Copies a field into a state buffer when the record has it. */
static void state_copy(const struct vec_file *file, const char *key, char *target, size_t room);

/* One Wycheproof file: kind 0 is PKCS1 v1.5, kind 1 is PSS. */
static void check_wycheproof(const char *name, int kind);

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

/* Fills size bytes with pseudo-random bytes. */
static void
random_bytes(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte from each draw's high bits. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random32() >> 24);
}

/* The digest size of a hash id, written again here from FIPS 180-4. */
static size_t
hash_length(
	enum cryc_hash_id id)
{
	/* SHA-256, SHA-384, SHA-512. */
	if (id == CRYC_HASH_SHA256)
		return 32U;
	if (id == CRYC_HASH_SHA384)
		return 48U;
	return 64U;
}

/* Hashes data with the library's hash (checked by its own tests). */
static void
digest(
	enum cryc_hash_id id,
	const uint8_t *data,
	size_t size,
	uint8_t *out)
{
	int error;

	/* One call. */
	error = cryc_hash_digest(id, data, size, out);
	CHECK(error == 0);
}

/* Drops the zero limbs at the top. */
static void
big_trim(
	struct big *x)
{
	/* Down to the highest non-zero limb. */
	while (x->used > 0U && x->limb[x->used - 1U] == 0U)
		x->used--;
}

/* Sets x to 2^bits - amount, for amount 1 or 2 and bits of at least 33 or exactly 31. */
static void
big_power_of_two_minus(
	struct big *x,
	unsigned bits,
	uint32_t amount)
{
	unsigned words;
	unsigned rest;
	unsigned index;

	/* 2^bits - 1 is all ones: whole limbs and a partial top limb. */
	memset(x, 0, sizeof(*x));
	words = bits / 32U;
	rest = bits % 32U;
	for (index = 0; index < words; index++)
		x->limb[index] = 0xffffffffU;
	if (rest != 0U)
		x->limb[words] = (1U << rest) - 1U;

	/* The partial top limb, when there is one, counts as used. */
	x->used = words;
	if (rest != 0U)
		x->used++;

	/* 2^bits - 2: the lowest limb is odd, so one is taken from it without a borrow. */
	if (amount == 2U)
		x->limb[0] -= 1U;
	big_trim(x);
}

/* Sets out to x * y (out must not be x or y). */
static void
big_multiply(
	struct big *out,
	const struct big *x,
	const struct big *y)
{
	size_t i;
	size_t j;
	uint64_t carry;
	uint64_t total;

	/* Schoolbook multiplication, a row of y for each limb of x. */
	memset(out, 0, sizeof(*out));
	for (i = 0; i < x->used; i++) {
		carry = 0;
		for (j = 0; j < y->used; j++) {
			total = (uint64_t)x->limb[i] * y->limb[j] + out->limb[i + j] + carry;
			out->limb[i + j] = (uint32_t)(total & 0xffffffffU);
			carry = total >> 32;
		}

		/* The carry out of the row is the limb above it. */
		out->limb[i + y->used] = (uint32_t)carry;
	}

	/* The product has at most as many limbs as both factors together. */
	out->used = x->used + y->used;
	big_trim(out);
}

/* Returns x mod m for a 32-bit m. */
static uint32_t
big_mod_small(
	const struct big *x,
	uint32_t m)
{
	size_t index;
	uint64_t rest;

	/* Long division from the top limb. */
	rest = 0;
	for (index = x->used; index > 0U; index--)
		rest = ((rest << 32) | x->limb[index - 1U]) % m;

	/* The remainder fits in 32 bits because m does. */
	return (uint32_t)rest;
}

/* Sets x to x * multiplier + addend. */
static void
big_multiply_small_add(
	struct big *x,
	uint32_t multiplier,
	uint32_t addend)
{
	size_t index;
	uint64_t carry;
	uint64_t total;

	/* One row, the carry running up. */
	carry = addend;
	for (index = 0; index < x->used; index++) {
		total = (uint64_t)x->limb[index] * multiplier + carry;
		x->limb[index] = (uint32_t)(total & 0xffffffffU);
		carry = total >> 32;
	}

	/* A carry out of the top limb makes the number one limb longer. */
	if (carry != 0U) {
		x->limb[x->used] = (uint32_t)carry;
		x->used++;
	}
}

/* Sets x to x / divisor (floor) and returns the remainder. */
static uint32_t
big_divide_small(
	struct big *x,
	uint32_t divisor)
{
	size_t index;
	uint64_t rest;
	uint64_t current;

	/* Long division from the top limb. */
	rest = 0;
	for (index = x->used; index > 0U; index--) {
		current = (rest << 32) | x->limb[index - 1U];
		x->limb[index - 1U] = (uint32_t)(current / divisor);
		rest = current % divisor;
	}

	/* The quotient loses the zero limbs the division left at the top. */
	big_trim(x);

	/* The remainder is the answer; the quotient stays in x. */
	return (uint32_t)rest;
}

/* Writes x big-endian without leading zero bytes into out (room bytes) and returns the size (0 when it does not fit). */
static size_t
big_to_bytes(
	const struct big *x,
	uint8_t *out,
	size_t room)
{
	size_t total;
	size_t index;
	size_t position;
	size_t skip;
	size_t size;

	/* Every byte of every limb, most significant first. */
	total = x->used * 4U;
	CHECK(total <= room + 3U);
	if (total > room + 3U)
		return 0;
	skip = 0;
	size = 0;
	for (index = 0; index < total; index++) {
		position = total - 1U - index;
		if (size == 0U && ((x->limb[position / 4U] >> (8U * (position % 4U))) & 0xffU) == 0U) {
			skip++;
			continue;
		}

		/* The output must have room for this byte. */
		CHECK(size < room);
		if (size >= room)
			return 0;
		out[size] = (uint8_t)((x->limb[position / 4U] >> (8U * (position % 4U))) & 0xffU);
		size++;
	}

	/* The count of skipped zero bytes is kept for symmetry and is not needed after the loop. */
	(void)skip;

	/* The number of bytes written. */
	return size;
}

/*
 * Decodes hex text that may have an odd number of digits (the CAVP files
 * drop a leading zero digit) as a number: an odd text gets a zero digit on
 * the left.  Returns the bytes written, or (size_t)-1.
 */
static size_t
hex_number(
	const char *text,
	uint8_t *out,
	size_t room)
{
	static char padded[8400];
	size_t length;
	size_t decoded;

	/* Even texts as they are. */
	length = strlen(text);
	if (length % 2U == 0U) {
		decoded = hex_decode(text, out, room);
		return decoded;
	}

	/* Odd texts with a zero digit in front. */
	if (length + 2U > sizeof(padded))
		return (size_t)-1;
	padded[0] = '0';
	memcpy(padded + 1, text, length + 1U);
	decoded = hex_decode(padded, out, room);

	/* The bytes decoded from the padded text. */
	return decoded;
}

/* The key as the library takes it. */
static void
public_key(
	const struct tkey *key,
	struct cryc_rsa_key *out)
{
	/* Borrows the bytes. */
	out->n = key->n;
	out->n_size = key->n_size;
	out->e = key->e;
	out->e_size = key->e_size;
}

/* Returns the bit length of a big-endian number without leading zero bytes. */
static size_t
bit_length(
	const uint8_t *bytes,
	size_t size)
{
	size_t bits;
	unsigned top;

	/* Eight for every byte below the top and the bits of the top byte. */
	if (size == 0U)
		return 0;
	bits = (size - 1U) * 8U;
	for (top = bytes[0]; top != 0U; top >>= 1)
		bits++;
	return bits;
}

/*
 * Builds the key of a spec with e = 65537: n is the product of the primes,
 * phi the product of one less, d = (1 + t * phi) / e for the t below e
 * that makes the numerator divisible (e * d = 1 mod phi).
 */
static void
make_key(
	struct tkey *key,
	const struct key_spec *spec)
{
	static struct big n_value;
	static struct big phi_value;
	static struct big factor;
	static struct big product;
	uint32_t e_value;
	uint32_t phi_mod_e;
	uint32_t inverse;
	uint32_t power;
	uint32_t remainder;
	uint32_t multiplier;
	size_t index;
	unsigned step;

	/* n and phi from the primes. */
	memset(key, 0, sizeof(*key));
	memset(&n_value, 0, sizeof(n_value));
	memset(&phi_value, 0, sizeof(phi_value));
	n_value.limb[0] = 1U;
	n_value.used = 1;
	phi_value.limb[0] = 1U;
	phi_value.used = 1;
	for (index = 0; index < spec->count; index++) {
		big_power_of_two_minus(&factor, spec->exponents[index], 1U);
		big_multiply(&product, &n_value, &factor);
		n_value = product;
		big_power_of_two_minus(&factor, spec->exponents[index], 2U);
		big_multiply(&product, &phi_value, &factor);
		phi_value = product;
	}

	/* The inverse of phi modulo the prime e by Fermat's little theorem. */
	e_value = 65537U;
	phi_mod_e = big_mod_small(&phi_value, e_value);
	CHECK(phi_mod_e != 0U);
	inverse = 1U;
	power = phi_mod_e;
	for (step = 0; step < 32U; step++) {
		if (((e_value - 2U) >> step) & 1U)
			inverse = (uint32_t)(((uint64_t)inverse * power) % e_value);
		power = (uint32_t)(((uint64_t)power * power) % e_value);
	}

	/* The multiplier t for which 1 + t * phi is divisible by e. */
	multiplier = e_value - inverse;

	/* d = (1 + multiplier * phi) / e, exact. */
	big_multiply_small_add(&phi_value, multiplier, 1U);
	remainder = big_divide_small(&phi_value, e_value);
	CHECK(remainder == 0U);

	/* The bytes. */
	key->n_size = big_to_bytes(&n_value, key->n, sizeof(key->n));
	key->d_size = big_to_bytes(&phi_value, key->d, sizeof(key->d));
	key->e[0] = 0x01U;
	key->e[1] = 0x00U;
	key->e[2] = 0x01U;
	key->e_size = 3;
	key->bits = bit_length(key->n, key->n_size);
	CHECK(key->bits == spec->bits);
}

/* Loads a key from hex text of n, e and d (leading zeros removed); returns 0, or -1 when a value is too big. */
static int
load_key(
	struct tkey *key,
	const char *n_hex,
	const char *e_hex,
	const char *d_hex)
{
	uint8_t buffer[K_MAX + 8U];
	size_t size;
	size_t skip;

	/* n. */
	memset(key, 0, sizeof(*key));
	size = hex_number(n_hex, buffer, sizeof(buffer));
	if (size == (size_t)-1 || size == 0U)
		return -1;
	for (skip = 0; skip < size && buffer[skip] == 0U; skip++)
		continue;
	if (size - skip > K_MAX)
		return -1;
	key->n_size = size - skip;
	memcpy(key->n, buffer + skip, key->n_size);
	key->bits = bit_length(key->n, key->n_size);

	/* e, at most 8 bytes. */
	size = hex_number(e_hex, buffer, sizeof(buffer));
	if (size == (size_t)-1)
		return -1;
	for (skip = 0; skip < size && buffer[skip] == 0U; skip++)
		continue;
	if (size - skip > sizeof(key->e))
		return -1;
	key->e_size = size - skip;
	memcpy(key->e, buffer + skip, key->e_size);

	/* d, when there is one. */
	if (d_hex == NULL)
		return 0;
	size = hex_number(d_hex, buffer, sizeof(buffer));
	if (size == (size_t)-1)
		return -1;
	for (skip = 0; skip < size && buffer[skip] == 0U; skip++)
		continue;
	if (size - skip > K_MAX)
		return -1;
	key->d_size = size - skip;
	memcpy(key->d, buffer + skip, key->d_size);
	return 0;
}

/*
 * RSASP1 on arbitrary bytes: sig = m^d mod n as n_size bytes, for m of
 * m_size bytes read as an integer.  Returns 0, or -1 when m is not less
 * than n (nothing is written).
 */
static int
raw_sign(
	const struct tkey *key,
	const uint8_t *m,
	size_t m_size,
	uint8_t *sig)
{
	struct cryc_bn_mont mont;
	uint32_t value[CRYC_BN_MAX_LIMBS];
	uint32_t in_mont[CRYC_BN_MAX_LIMBS];
	uint32_t out_mont[CRYC_BN_MAX_LIMBS];
	uint32_t out_value[CRYC_BN_MAX_LIMBS];
	uint32_t below;
	int error;

	/* The modulus. */
	error = cryc_bn_mont_init(&mont, key->n, key->n_size);
	CHECK(error == 0);
	if (error != 0)
		return -1;

	/* m, which must be in range. */
	memset(value, 0, sizeof(value));
	memset(in_mont, 0, sizeof(in_mont));
	memset(out_mont, 0, sizeof(out_mont));
	memset(out_value, 0, sizeof(out_value));
	error = cryc_bn_decode(value, mont.limbs, m, m_size);
	if (error != 0)
		return -1;
	below = cryc_bn_lt(value, mont.modulus, mont.limbs);
	if (below == 0U)
		return -1;

	/* m^d mod n. */
	cryc_bn_to_mont(in_mont, value, &mont);
	cryc_bn_mont_exp(out_mont, in_mont, key->d, key->d_size, &mont);
	cryc_bn_from_mont(out_value, out_mont, &mont);
	cryc_bn_encode(out_value, mont.limbs, sig, key->n_size);
	return 0;
}

/*
 * The DER encoding T of the DigestInfo of hash (id) built with the DER
 * writer: SEQUENCE { SEQUENCE { OID, NULL }, OCTET STRING hash }.
 * With_null 0 leaves out the NULL parameters; oid_id names the hash whose
 * OID is written (so a different one can be used on purpose).
 */
static size_t
digest_info(
	enum cryc_hash_id oid_id,
	const uint8_t *hash,
	size_t hash_size,
	int with_null,
	uint8_t *out,
	size_t room)
{
	struct der_writer algorithm_body;
	struct der_writer algorithm;
	struct der_writer body;
	struct der_writer whole;
	unsigned char algorithm_body_bytes[64];
	unsigned char algorithm_bytes[64];
	unsigned char body_bytes[128];
	const char *oid;

	/* The OIDs of the SHA-2 family: 2.16.840.1.101.3.4.2.1, .2 and .3 (FIPS 180-4 and RFC 8017 B.1). */
	oid = "2.16.840.1.101.3.4.2.1";
	if (oid_id == CRYC_HASH_SHA384)
		oid = "2.16.840.1.101.3.4.2.2";
	if (oid_id == CRYC_HASH_SHA512)
		oid = "2.16.840.1.101.3.4.2.3";

	/* AlgorithmIdentifier ::= SEQUENCE { algorithm OID, parameters NULL }. */
	der_init(&algorithm_body, algorithm_body_bytes, sizeof(algorithm_body_bytes));
	der_oid(&algorithm_body, oid);
	if (with_null)
		der_put(&algorithm_body, 0x05U, NULL, 0);
	der_init(&algorithm, algorithm_bytes, sizeof(algorithm_bytes));
	der_wrap(&algorithm, 0x30U, &algorithm_body);

	/* DigestInfo ::= SEQUENCE { digestAlgorithm, digest OCTET STRING }. */
	der_init(&body, body_bytes, sizeof(body_bytes));
	der_raw(&body, algorithm.data, algorithm.used);
	der_put(&body, 0x04U, hash, hash_size);
	der_init(&whole, out, room);
	der_wrap(&whole, 0x30U, &body);
	CHECK(whole.error == 0);
	return whole.used;
}

/* Builds EM = 0x00 || 0x01 || PS || 0x00 || T of k bytes (RFC 8017 9.2); returns k, or 0 when T does not fit. */
static size_t
pkcs1_encode(
	const uint8_t *t,
	size_t t_size,
	size_t k,
	uint8_t *em)
{
	size_t padding;

	/* Needs at least 8 bytes of PS. */
	if (k < t_size + 11U)
		return 0;
	padding = k - t_size - 3U;
	em[0] = 0x00U;
	em[1] = 0x01U;
	memset(em + 2U, 0xff, padding);
	em[2U + padding] = 0x00U;
	memcpy(em + 3U + padding, t, t_size);
	return k;
}

/* Fills mask with MGF1 (RFC 8017 B.2.1) over seed with hash id. */
static void
mgf1(
	enum cryc_hash_id id,
	const uint8_t *seed,
	size_t seed_size,
	uint8_t *mask,
	size_t mask_size)
{
	uint8_t input[HASH_MAX + 4U];
	uint8_t block[HASH_MAX];
	size_t length;
	size_t offset;
	size_t take;
	uint32_t counter;

	/* Hash (seed || C) for the counters C = 0, 1, ... */
	length = hash_length(id);
	memcpy(input, seed, seed_size);
	offset = 0;
	for (counter = 0; offset < mask_size; counter++) {
		input[seed_size] = (uint8_t)(counter >> 24);
		input[seed_size + 1U] = (uint8_t)(counter >> 16);
		input[seed_size + 2U] = (uint8_t)(counter >> 8);
		input[seed_size + 3U] = (uint8_t)counter;
		digest(id, input, seed_size + 4U, block);
		take = mask_size - offset;
		if (take > length)
			take = length;
		memcpy(mask + offset, block, take);
		offset += take;
	}
}

/*
 * EMSA-PSS-ENCODE (RFC 8017 9.1.1) of the hash mhash with the given salt
 * for em_bits: writes em_len = ceil (em_bits / 8) bytes to em and returns
 * em_len, or 0 when the block is too short.
 */
static size_t
pss_encode(
	enum cryc_hash_id id,
	const uint8_t *mhash,
	const uint8_t *salt,
	size_t salt_size,
	size_t em_bits,
	uint8_t *em)
{
	uint8_t input[8U + HASH_MAX + K_MAX];
	uint8_t h[HASH_MAX];
	uint8_t mask[K_MAX];
	size_t h_len;
	size_t em_len;
	size_t db_len;
	size_t padding;
	size_t index;

	/* Step 3. */
	h_len = hash_length(id);
	em_len = (em_bits + 7U) / 8U;
	if (em_len < h_len + salt_size + 2U)
		return 0;

	/* Steps 5 and 6: H = Hash (0x00 * 8 || mHash || salt). */
	memset(input, 0, 8U);
	memcpy(input + 8U, mhash, h_len);
	memcpy(input + 8U + h_len, salt, salt_size);
	digest(id, input, 8U + h_len + salt_size, h);

	/* Steps 7 and 8: DB = PS || 0x01 || salt, with PS zero bytes. */
	db_len = em_len - h_len - 1U;
	padding = em_len - salt_size - h_len - 2U;
	memset(em, 0, padding);
	em[padding] = 0x01U;
	memcpy(em + padding + 1U, salt, salt_size);

	/* Steps 9 and 10: maskedDB = DB xor MGF (H, emLen - hLen - 1). */
	mgf1(id, h, h_len, mask, db_len);
	for (index = 0; index < db_len; index++)
		em[index] = (uint8_t)(em[index] ^ mask[index]);

	/* Step 11: clears the leftmost 8 emLen - emBits bits. */
	em[0] = (uint8_t)(em[0] & (0xffU >> (8U * em_len - em_bits)));

	/* Step 12: EM = maskedDB || H || 0xbc. */
	memcpy(em + db_len, h, h_len);
	em[em_len - 1U] = 0xbcU;
	return em_len;
}

/* Signs the hash with PKCS1 v1.5 (the DigestInfo of id); returns 0, or -1 when it does not fit. */
static int
sign_pkcs1(
	const struct tkey *key,
	enum cryc_hash_id id,
	const uint8_t *hash,
	uint8_t *sig)
{
	uint8_t t[128];
	uint8_t em[K_MAX];
	size_t t_size;
	size_t em_size;
	int error;

	/* EM, then the private operation. */
	t_size = digest_info(id, hash, hash_length(id), 1, t, sizeof(t));
	em_size = pkcs1_encode(t, t_size, key->n_size, em);
	if (em_size == 0U)
		return -1;

	/* Signs EM with the private exponent. */
	error = raw_sign(key, em, em_size, sig);
	if (error != 0)
		return error;

	/* Succeeded: sig holds the signature. */
	return 0;
}

/* Signs the hash with PSS (MGF1 with the same hash); returns 0, or -1 when the salt does not fit. */
static int
sign_pss(
	const struct tkey *key,
	enum cryc_hash_id id,
	const uint8_t *hash,
	const uint8_t *salt,
	size_t salt_size,
	uint8_t *sig)
{
	uint8_t em[K_MAX];
	size_t em_size;
	int error;

	/* EM for emBits = modBits - 1, then the private operation on its emLen bytes. */
	em_size = pss_encode(id, hash, salt, salt_size, key->bits - 1U, em);
	if (em_size == 0U)
		return -1;

	/* Signs EM with the private exponent. */
	error = raw_sign(key, em, em_size, sig);
	if (error != 0)
		return error;

	/* Succeeded: sig holds the signature. */
	return 0;
}

/* Verifies with PKCS1 v1.5 against a key of the tests. */
static int
verify_pkcs1(
	const struct tkey *key,
	enum cryc_hash_id id,
	const uint8_t *hash,
	const uint8_t *sig)
{
	struct cryc_rsa_key rsa;
	int error;

	/* The key as the library takes it, the sizes from the key. */
	public_key(key, &rsa);

	/* The library's verdict on the signature. */
	error = cryc_rsa_pkcs1_verify(&rsa, id, hash, hash_length(id), sig, key->n_size);
	if (error != 0)
		return error;

	/* Succeeded: the signature is valid. */
	return 0;
}

/* Verifies with PSS against a key of the tests. */
static int
verify_pss(
	const struct tkey *key,
	enum cryc_hash_id id,
	const uint8_t *hash,
	size_t salt_size,
	const uint8_t *sig)
{
	struct cryc_rsa_key rsa;
	int error;

	/* The key as the library takes it, the sizes from the key. */
	public_key(key, &rsa);

	/* The library's verdict on the signature. */
	error = cryc_rsa_pss_verify(&rsa, id, hash, hash_length(id), salt_size, sig, key->n_size);
	if (error != 0)
		return error;

	/* Succeeded: the signature is valid. */
	return 0;
}

/* Checks that the DigestInfo built here has the lengths and prefix bytes of RFC 8017 9.2 note 1. */
static void
check_digest_info(void)
{
	static const uint8_t expected_start[3][4] = {
		{ 0x30U, 0x31U, 0x30U, 0x0dU },
		{ 0x30U, 0x41U, 0x30U, 0x0dU },
		{ 0x30U, 0x51U, 0x30U, 0x0dU }
	};
	static const uint8_t expected_oid_tail[3][6] = {
		{ 0x02U, 0x01U, 0x05U, 0x00U, 0x04U, 0x20U },
		{ 0x02U, 0x02U, 0x05U, 0x00U, 0x04U, 0x30U },
		{ 0x02U, 0x03U, 0x05U, 0x00U, 0x04U, 0x40U }
	};
	uint8_t hash[HASH_MAX];
	uint8_t t[128];
	size_t t_size;
	size_t index;
	size_t length;

	/* Each hash: 19 prefix bytes then the hash. */
	memset(hash, 0xa5, sizeof(hash));
	for (index = 0; index < 3U; index++) {
		length = hash_length(hash_ids[index]);
		t_size = digest_info(hash_ids[index], hash, length, 1, t, sizeof(t));
		CHECK(t_size == 19U + length);
		CHECK_BYTES(t, expected_start[index], 4);
		CHECK_BYTES(t + 13, expected_oid_tail[index], 6);
		CHECK(memcmp(t + 19, hash, length) == 0);

		/* Without the NULL parameters it is two bytes shorter. */
		t_size = digest_info(hash_ids[index], hash, length, 0, t, sizeof(t));
		CHECK(t_size == 17U + length);
	}
}

/* Round trips of both schemes for one key and one hash with several salt sizes, and the changes that must fail. */
static void
round_trip_key(
	const struct tkey *key,
	enum cryc_hash_id id,
	int do_pkcs1,
	size_t salt_count)
{
	uint8_t sig[K_MAX];
	uint8_t other[K_MAX];
	uint8_t hash[HASH_MAX];
	uint8_t wrong[HASH_MAX];
	uint8_t salt[K_MAX];
	uint8_t message[100];
	size_t h_len;
	size_t em_len;
	size_t max_salt;
	size_t sizes[4];
	size_t index;
	size_t position;
	size_t k;
	int error;
	uint32_t carry;
	size_t byte;
	uint32_t sum;
	uint8_t flip;

	/* A pseudo-random message and its hash. */
	h_len = hash_length(id);
	k = key->n_size;
	random_bytes(message, sizeof(message));
	digest(id, message, sizeof(message), hash);
	memcpy(wrong, hash, h_len);
	wrong[h_len / 2U] ^= 0x10U;
	random_bytes(salt, sizeof(salt));

	/* PKCS1 v1.5: a good signature, the wrong hash, a bit changed in the signature, the other scheme, another hash id. */
	if (do_pkcs1) {
		error = sign_pkcs1(key, id, hash, sig);
		CHECK(error == 0);
		error = verify_pkcs1(key, id, hash, sig);
		CHECK(error == 0);
		error = verify_pkcs1(key, id, wrong, sig);
		CHECK(error == EBADMSG);
		for (index = 0; index < 8U; index++) {
			/* The first byte, the last byte, then bytes at random. */
			if (index == 0U)
				position = 0U;
			else if (index == 1U)
				position = k - 1U;
			else
				position = (size_t)(random32() % k);

			/* One bit of the signature changed is refused, and restored it is accepted again. */
			flip = (uint8_t)(1U << (random32() % 8U));
			sig[position] ^= flip;
			error = verify_pkcs1(key, id, hash, sig);
			CHECK(error == EBADMSG);
			sig[position] ^= flip;
			error = verify_pkcs1(key, id, hash, sig);
			CHECK(error == 0);
		}

		/* The PKCS1 v1.5 signature is no PSS signature. */
		error = verify_pss(key, id, hash, h_len, sig);
		CHECK(error == EBADMSG);

		/* s + n is not less than n: refused although it is the same residue (when it still fits in k bytes). */
		memcpy(other, sig, k);
		carry = 0;
		for (byte = k; byte > 0U; byte--) {
			sum = (uint32_t)other[byte - 1U] + (uint32_t)key->n[byte - 1U] + carry;
			other[byte - 1U] = (uint8_t)(sum & 0xffU);
			carry = sum >> 8;
		}

		/* Only a sum that still fits in k bytes is a candidate. */
		if (carry == 0U) {
			error = verify_pkcs1(key, id, hash, other);
			CHECK(error == EBADMSG);
		}
	}

	/* PSS: the salt sizes (the hash length, the largest that fits, 0, 1: the first salt_count of them); each verifies with exactly its size and with no other. */
	if (salt_count > 0U) {
		em_len = (key->bits - 1U + 7U) / 8U;
		max_salt = em_len - h_len - 2U;
		sizes[0] = h_len;
		sizes[1] = max_salt;
		sizes[2] = 0;
		sizes[3] = 1;
		for (index = 0; index < salt_count && index < 4U; index++) {
			error = sign_pss(key, id, hash, salt, sizes[index], sig);
			CHECK(error == 0);
			error = verify_pss(key, id, hash, sizes[index], sig);
			CHECK(error == 0);
			error = verify_pss(key, id, hash, sizes[index] + 1U, sig);
			CHECK(error == EBADMSG);
			if (sizes[index] > 0U) {
				error = verify_pss(key, id, hash, sizes[index] - 1U, sig);
				CHECK(error == EBADMSG);
			}

			/* A signature of another hash and one of the other scheme are refused. */
			error = verify_pss(key, id, wrong, sizes[index], sig);
			CHECK(error == EBADMSG);
			error = verify_pkcs1(key, id, hash, sig);
			CHECK(error == EBADMSG);

			/* The largest salt that fits leaves no padding; one byte more cannot be signed at all. */
			if (sizes[index] == max_salt) {
				error = sign_pss(key, id, hash, salt, max_salt + 1U, other);
				CHECK(error == -1);
			}
		}
	}
}

/* Forged PKCS1 v1.5 signatures: a wrong EM signed with the private key, each case refused. */
static void
forge_pkcs1(
	const struct tkey *key,
	enum cryc_hash_id id)
{
	uint8_t sig[K_MAX];
	uint8_t hash[HASH_MAX];
	uint8_t t[128];
	uint8_t em[K_MAX];
	uint8_t bad_t[160];
	size_t h_len;
	size_t t_size;
	size_t k;
	size_t position;
	size_t count;
	size_t em_size;
	size_t padding;
	enum cryc_hash_id other_id;
	int error;
	int signed_ok;
	int include;

	/* The correct EM first: it verifies. */
	h_len = hash_length(id);
	k = key->n_size;
	random_bytes(hash, h_len);
	t_size = digest_info(id, hash, h_len, 1, t, sizeof(t));
	em_size = pkcs1_encode(t, t_size, k, em);
	CHECK(em_size == k);
	error = raw_sign(key, em, em_size, sig);
	CHECK(error == 0);
	error = verify_pkcs1(key, id, hash, sig);
	CHECK(error == 0);

	/* One bit changed at each position of the structure: the start, the last PS byte, the separator, the DigestInfo, the hash. */
	count = 0;
	for (position = 0; position < k; position++) {
		include = position < 4U || (position + t_size + 3U >= k && position + h_len <= k) || position + 2U >= k || position == k - h_len / 2U;
		if (!include)
			continue;
		em_size = pkcs1_encode(t, t_size, k, em);
		em[position] ^= 0x01U;
		signed_ok = raw_sign(key, em, em_size, sig);
		if (signed_ok != 0)
			continue;
		error = verify_pkcs1(key, id, hash, sig);
		CHECK(error == EBADMSG);
		count++;
	}

	/* Enough positions gave a signable block that was refused. */
	CHECK(count >= 28U);

	/* The block type 0x01 as 0x00, 0x02 and 0xff, and the first byte not zero. */
	em_size = pkcs1_encode(t, t_size, k, em);
	em[1] = 0x00U;
	error = raw_sign(key, em, em_size, sig);
	CHECK(error == 0);
	error = verify_pkcs1(key, id, hash, sig);
	CHECK(error == EBADMSG);
	em_size = pkcs1_encode(t, t_size, k, em);
	em[1] = 0x02U;
	error = raw_sign(key, em, em_size, sig);
	CHECK(error == 0);
	error = verify_pkcs1(key, id, hash, sig);
	CHECK(error == EBADMSG);

	/* The DigestInfo without the NULL parameters (a form some signers produce): refused, PS two bytes longer. */
	t_size = digest_info(id, hash, h_len, 0, bad_t, sizeof(bad_t));
	em_size = pkcs1_encode(bad_t, t_size, k, em);
	error = raw_sign(key, em, em_size, sig);
	CHECK(error == 0);
	error = verify_pkcs1(key, id, hash, sig);
	CHECK(error == EBADMSG);

	/* A DigestInfo naming another hash's OID over a hash of this one's length. */
	other_id = CRYC_HASH_SHA256;
	if (id == CRYC_HASH_SHA256)
		other_id = CRYC_HASH_SHA512;
	t_size = digest_info(other_id, hash, h_len, 1, bad_t, sizeof(bad_t));
	em_size = pkcs1_encode(bad_t, t_size, k, em);
	error = raw_sign(key, em, em_size, sig);
	CHECK(error == 0);
	error = verify_pkcs1(key, id, hash, sig);
	CHECK(error == EBADMSG);

	/* Garbage after the DigestInfo with one byte less of PS (the classic lax-parsing forgery shape). */
	t_size = digest_info(id, hash, h_len, 1, t, sizeof(t));
	padding = k - t_size - 3U;
	em[0] = 0x00U;
	em[1] = 0x01U;
	memset(em + 2U, 0xff, padding - 1U);
	em[2U + padding - 1U] = 0x00U;
	memcpy(em + 2U + padding, t, t_size);
	em[k - 1U] = 0x00U;
	error = raw_sign(key, em, k, sig);
	CHECK(error == 0);
	error = verify_pkcs1(key, id, hash, sig);
	CHECK(error == EBADMSG);

	/* The DigestInfo with a long-form length for the outer SEQUENCE (not DER), PS one byte shorter. */
	em[0] = 0x00U;
	em[1] = 0x01U;
	memset(em + 2U, 0xff, padding - 1U);
	em[2U + padding - 1U] = 0x00U;
	em[2U + padding] = 0x30U;
	em[3U + padding] = 0x81U;
	em[4U + padding] = (uint8_t)(t[1]);
	memcpy(em + 5U + padding, t + 2, t_size - 2U);
	error = raw_sign(key, em, k, sig);
	CHECK(error == 0);
	error = verify_pkcs1(key, id, hash, sig);
	CHECK(error == EBADMSG);

	/* The valid signature still verifies, and verifies for no other hash id. */
	error = sign_pkcs1(key, id, hash, sig);
	CHECK(error == 0);
	error = verify_pkcs1(key, id, hash, sig);
	CHECK(error == 0);
}

/* Forged PSS signatures: a wrong EM signed with the private key, each case refused. */
static void
forge_pss(
	const struct tkey *key,
	enum cryc_hash_id id)
{
	uint8_t sig[K_MAX];
	uint8_t hash[HASH_MAX];
	uint8_t wrong[HASH_MAX];
	uint8_t salt[HASH_MAX];
	uint8_t em[K_MAX];
	size_t h_len;
	size_t em_len;
	size_t em_bits;
	size_t database_len;
	size_t padding;
	size_t positions[12];
	size_t index;
	size_t count;
	size_t attempt;
	int error;
	int signed_ok;
	unsigned pattern;

	/* A valid block first. */
	h_len = hash_length(id);
	random_bytes(hash, h_len);
	random_bytes(salt, h_len);
	em_bits = key->bits - 1U;
	em_len = pss_encode(id, hash, salt, h_len, em_bits, em);
	CHECK(em_len != 0U);
	signed_ok = raw_sign(key, em, em_len, sig);
	CHECK(signed_ok == 0);
	error = verify_pss(key, id, hash, h_len, sig);
	CHECK(error == 0);

	/* Each field of the block changed: the first bytes, the padding, the 0x01, the salt, H, the trailer. */
	database_len = em_len - h_len - 1U;
	padding = em_len - h_len - h_len - 2U;
	positions[0] = 0;
	positions[1] = 1;
	positions[2] = padding - 1U;
	positions[3] = padding;
	positions[4] = padding + 1U;
	positions[5] = padding + h_len;
	positions[6] = database_len - 1U;
	positions[7] = database_len;
	positions[8] = database_len + h_len / 2U;
	positions[9] = em_len - 2U;
	positions[10] = em_len - 1U;
	positions[11] = em_len / 2U;
	count = 0;
	for (index = 0; index < 12U; index++) {
		for (pattern = 0; pattern < 2U; pattern++) {
			if (pattern == 1U && index != 0U && index != 1U && index != 10U)
				continue;
			em_len = pss_encode(id, hash, salt, h_len, em_bits, em);

			/* The first pattern changes the lowest bit of the byte, the second the highest. */
			if (pattern == 0U)
				em[positions[index]] ^= 0x01U;
			else
				em[positions[index]] ^= 0x80U;

			/* Signs the changed block and expects the signature to be refused. */
			signed_ok = raw_sign(key, em, em_len, sig);
			if (signed_ok != 0)
				continue;
			error = verify_pss(key, id, hash, h_len, sig);
			CHECK(error == EBADMSG);
			count++;
		}
	}

	/* Enough fields gave a signable block that was refused. */
	CHECK(count >= 12U);

	/* H computed over another message's hash (a signature of a different message). */
	memcpy(wrong, hash, h_len);
	wrong[0] ^= 0x01U;
	em_len = pss_encode(id, wrong, salt, h_len, em_bits, em);
	signed_ok = raw_sign(key, em, em_len, sig);
	CHECK(signed_ok == 0);
	error = verify_pss(key, id, hash, h_len, sig);
	CHECK(error == EBADMSG);

	/* A block whose DB is all zeros (no 0x01 marker): refused whatever the salt size, including sizes at which the padding length would wrap. */
	em_len = (em_bits + 7U) / 8U;
	database_len = em_len - h_len - 1U;
	mgf1(id, hash, h_len, em, database_len);
	em[0] = (uint8_t)(em[0] & (0xffU >> (8U * em_len - em_bits)));
	memcpy(em + database_len, hash, h_len);
	em[em_len - 1U] = 0xbcU;
	signed_ok = raw_sign(key, em, em_len, sig);
	CHECK(signed_ok == 0);
	if (signed_ok == 0) {
		positions[0] = 0;
		positions[1] = 1;
		positions[2] = h_len;
		positions[3] = em_len - h_len - 2U;
		positions[4] = em_len - h_len - 1U;
		positions[5] = em_len - h_len;
		positions[6] = em_len - h_len + 1U;
		positions[7] = em_len - 1U;
		positions[8] = em_len;
		for (index = 0; index < 9U; index++) {
			error = verify_pss(key, id, hash, positions[index], sig);
			CHECK(error == EBADMSG);
		}
	}

	/* The unused top bits of the block (when modBits - 1 is not a multiple of 8) set: refused although the rest is right. */
	if (em_len * 8U != em_bits) {
		for (attempt = 0; attempt < 8U; attempt++) {
			random_bytes(salt, h_len);
			em_len = pss_encode(id, hash, salt, h_len, em_bits, em);
			em[0] |= (uint8_t)(0x80U);
			signed_ok = raw_sign(key, em, em_len, sig);
			if (signed_ok != 0)
				continue;
			error = verify_pss(key, id, hash, h_len, sig);
			CHECK(error == EBADMSG);
		}
	}

	/* EM one byte shorter than the modulus: m with a non-zero first byte is "integer too large", refused although the lower bytes are a good block. */
	if (em_len + 1U == key->n_size) {
		count = 0;
		for (attempt = 0; attempt < 20U && count < 3U; attempt++) {
			random_bytes(salt, h_len);
			em_len = pss_encode(id, hash, salt, h_len, em_bits, em);
			memmove(em + 1, em, em_len);
			em[0] = 0x01U;
			signed_ok = raw_sign(key, em, em_len + 1U, sig);
			if (signed_ok != 0)
				continue;
			error = verify_pss(key, id, hash, h_len, sig);
			CHECK(error == EBADMSG);
			count++;
		}

		/* Three signable blocks were found, and all were refused. */
		CHECK(count == 3U);
	}
}

/* The signature as sent changed, and truncated and lengthened signatures, for one valid signature. */
static void
check_signature_forms(
	const struct tkey *key)
{
	uint8_t sig[K_MAX + 8U];
	uint8_t hash[HASH_MAX];
	struct cryc_rsa_key rsa;
	size_t length;
	size_t index;
	int error;

	/* A valid signature of both kinds, SHA-256. */
	random_bytes(hash, 32U);
	error = sign_pkcs1(key, CRYC_HASH_SHA256, hash, sig);
	CHECK(error == 0);
	public_key(key, &rsa);

	/* Every length from 0 to k + 8: only k is a signature (the others are refused by length, never read past the buffer). */
	for (length = 0; length <= key->n_size + 8U; length++) {
		memset(sig + key->n_size, 0, 8U);
		error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 32U, sig, length);
		if (length == key->n_size)
			CHECK(error == 0);
		else
			CHECK(error == EBADMSG);
	}

	/* s = 0, s = 1, s = n - 1, s = n, s = n + 1 and all ones (the last two are not less than n). */
	memset(sig, 0, key->n_size);
	error = verify_pkcs1(key, CRYC_HASH_SHA256, hash, sig);
	CHECK(error == EBADMSG);
	sig[key->n_size - 1U] = 0x01U;
	error = verify_pkcs1(key, CRYC_HASH_SHA256, hash, sig);
	CHECK(error == EBADMSG);
	memcpy(sig, key->n, key->n_size);
	sig[key->n_size - 1U] -= 1U;
	error = verify_pkcs1(key, CRYC_HASH_SHA256, hash, sig);
	CHECK(error == EBADMSG);
	sig[key->n_size - 1U] += 1U;
	error = verify_pkcs1(key, CRYC_HASH_SHA256, hash, sig);
	CHECK(error == EBADMSG);
	error = verify_pss(key, CRYC_HASH_SHA256, hash, 32U, sig);
	CHECK(error == EBADMSG);
	sig[key->n_size - 1U] += 2U;
	error = verify_pkcs1(key, CRYC_HASH_SHA256, hash, sig);
	CHECK(error == EBADMSG);
	memset(sig, 0xff, key->n_size);
	error = verify_pkcs1(key, CRYC_HASH_SHA256, hash, sig);
	CHECK(error == EBADMSG);
	error = verify_pss(key, CRYC_HASH_SHA256, hash, 32U, sig);
	CHECK(error == EBADMSG);

	/* Random signatures of the right length. */
	for (index = 0; index < 20U; index++) {
		random_bytes(sig, key->n_size);
		error = verify_pkcs1(key, CRYC_HASH_SHA256, hash, sig);
		CHECK(error == EBADMSG);
		error = verify_pss(key, CRYC_HASH_SHA256, hash, 32U, sig);
		CHECK(error == EBADMSG);
	}
}

/* A valid signature whose first byte is zero, sent without it or with another zero in front: neither is a signature of k bytes. */
static void
check_leading_zero_signature(
	const struct tkey *key)
{
	uint8_t sig[K_MAX + 8U];
	uint8_t hash[HASH_MAX];
	struct cryc_rsa_key rsa;
	size_t attempt;
	size_t k;
	int error;
	int found;

	/* The modulus of this key starts with 0x01, so about half of the signatures start with 0x00. */
	k = key->n_size;
	CHECK(key->n[0] == 0x01U);
	found = 0;
	for (attempt = 0; attempt < 30U && !found; attempt++) {
		random_bytes(hash, 32U);
		error = sign_pkcs1(key, CRYC_HASH_SHA256, hash, sig);
		CHECK(error == 0);
		if (sig[0] == 0x00U)
			found = 1;
	}

	/* A signature with a zero first byte was found. */
	CHECK(found);
	if (!found)
		return;

	/* The full signature verifies; the same number in k - 1 bytes and in k + 1 bytes does not. */
	public_key(key, &rsa);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 32U, sig, k);
	CHECK(error == 0);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 32U, sig + 1, k - 1U);
	CHECK(error == EBADMSG);
	memmove(sig + 1, sig, k);
	sig[0] = 0x00U;
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 32U, sig, k + 1U);
	CHECK(error == EBADMSG);
}

/* Argument errors and the limits of the key, with one good key and signature. */
static void
check_arguments(
	const struct tkey *key)
{
	static const size_t even_bytes[3] = { 1U, 128U, 1025U };
	static const size_t bit_counts[8] = { 1U, 1024U, 2040U, 2047U, 2048U, 8191U, 8192U, 8193U };
	uint8_t sig[K_MAX + 8U];
	uint8_t hash[HASH_MAX];
	uint8_t modulus[K_MAX + 4U];
	uint8_t exponent[8];
	struct cryc_rsa_key rsa;
	struct cryc_rsa_key bad;
	size_t k;
	size_t size;
	size_t index;
	size_t bits;
	size_t bytes;
	size_t top;
	int error;

	/* A valid signature of both kinds. */
	k = key->n_size;
	random_bytes(hash, 32U);
	error = sign_pkcs1(key, CRYC_HASH_SHA256, hash, sig);
	CHECK(error == 0);
	public_key(key, &rsa);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 32U, sig, k);
	CHECK(error == 0);

	/* Missing arguments. */
	error = cryc_rsa_pkcs1_verify(NULL, CRYC_HASH_SHA256, hash, 32U, sig, k);
	CHECK(error == EINVAL);
	bad = rsa;
	bad.n = NULL;
	error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, k);
	CHECK(error == EINVAL);
	bad = rsa;
	bad.e = NULL;
	error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, k);
	CHECK(error == EINVAL);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, NULL, 32U, sig, k);
	CHECK(error == EINVAL);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 32U, NULL, k);
	CHECK(error == EINVAL);
	error = cryc_rsa_pss_verify(NULL, CRYC_HASH_SHA256, hash, 32U, 32U, sig, k);
	CHECK(error == EINVAL);
	error = cryc_rsa_pss_verify(&rsa, CRYC_HASH_SHA256, NULL, 32U, 32U, sig, k);
	CHECK(error == EINVAL);
	error = cryc_rsa_pss_verify(&rsa, CRYC_HASH_SHA256, hash, 32U, 32U, NULL, k);
	CHECK(error == EINVAL);

	/* The hash: none, unknown ids, a wrong size for the id (0, one off, another hash's). */
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_NONE, hash, 32U, sig, k);
	CHECK(error == ENOTSUP);
	error = cryc_rsa_pkcs1_verify(&rsa, (enum cryc_hash_id)4, hash, 32U, sig, k);
	CHECK(error == ENOTSUP);
	error = cryc_rsa_pkcs1_verify(&rsa, (enum cryc_hash_id)-1, hash, 32U, sig, k);
	CHECK(error == ENOTSUP);
	error = cryc_rsa_pss_verify(&rsa, CRYC_HASH_NONE, hash, 32U, 32U, sig, k);
	CHECK(error == ENOTSUP);
	error = cryc_rsa_pss_verify(&rsa, (enum cryc_hash_id)7, hash, 32U, 32U, sig, k);
	CHECK(error == ENOTSUP);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 0U, sig, k);
	CHECK(error == EINVAL);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 31U, sig, k);
	CHECK(error == EINVAL);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 33U, sig, k);
	CHECK(error == EINVAL);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 48U, sig, k);
	CHECK(error == EINVAL);
	error = cryc_rsa_pss_verify(&rsa, CRYC_HASH_SHA384, hash, 32U, 48U, sig, k);
	CHECK(error == EINVAL);

	/* The order of the checks: a bad key before a bad hash before a bad signature length. */
	bad = rsa;
	bad.e_size = 0;
	error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_NONE, hash, 5U, sig, 3U);
	CHECK(error == EINVAL);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_NONE, hash, 5U, sig, 3U);
	CHECK(error == ENOTSUP);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 5U, sig, 3U);
	CHECK(error == EINVAL);
	error = cryc_rsa_pkcs1_verify(&rsa, CRYC_HASH_SHA256, hash, 32U, sig, 3U);
	CHECK(error == EBADMSG);

	/* The exponent: 0, 1, 2 and even values, 3 and the largest 32-bit odd value (accepted, but not this key's), five bytes, none. */
	for (index = 0; index < 10U; index++) {
		memset(exponent, 0, sizeof(exponent));
		bad = rsa;
		bad.e = exponent;
		bad.e_size = 1;

		/* Sets the exponent bytes and length of this case. */
		switch (index) {
		case 0: exponent[0] = 0x00U; break;
		case 1: exponent[0] = 0x01U; break;
		case 2: exponent[0] = 0x02U; break;
		case 3: exponent[0] = 0x04U; break;
		case 4: exponent[0] = 0x03U; break;
		case 5: exponent[0] = 0xffU; exponent[1] = 0xffU; exponent[2] = 0xffU; exponent[3] = 0xffU; bad.e_size = 4; break;
		case 6: exponent[0] = 0x01U; bad.e_size = 5; break;
		case 7: exponent[0] = 0x01U; exponent[4] = 0x03U; bad.e_size = 5; break;
		case 8: exponent[0] = 0x01U; exponent[2] = 0x01U; exponent[4] = 0x01U; bad.e_size = 5; break;
		default: bad.e_size = 0; break;
		}

		/* Only the odd 32-bit exponents above 1 are usable, and this key's signature does not fit them. */
		error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, k);
		if (index == 4U || index == 5U)
			CHECK(error == EBADMSG);
		else
			CHECK(error == EINVAL);
		error = cryc_rsa_pss_verify(&bad, CRYC_HASH_SHA256, hash, 32U, 32U, sig, k);
		if (index == 4U || index == 5U)
			CHECK(error == EBADMSG);
		else
			CHECK(error == EINVAL);
	}

	/* The exponent and the modulus with leading zero bytes mean the same: the good signature still verifies. */
	memset(exponent, 0, sizeof(exponent));
	memcpy(exponent + 3, key->e, key->e_size);
	bad = rsa;
	bad.e = exponent;
	bad.e_size = 3U + key->e_size;
	error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, k);
	CHECK(error == 0);
	modulus[0] = 0x00U;
	modulus[1] = 0x00U;
	memcpy(modulus + 2, key->n, k);
	bad = rsa;
	bad.n = modulus;
	bad.n_size = k + 2U;
	error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, k);
	CHECK(error == 0);

	/* The modulus: none, zero, even, one, odd below 2048 bits, exactly 2048 bits, 2047 bits, 8192 bits, 8193 bits. */
	bad = rsa;
	bad.n_size = 0;
	error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, k);
	CHECK(error == EINVAL);
	memset(modulus, 0, sizeof(modulus));
	bad.n = modulus;
	bad.n_size = 300;
	error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, k);
	CHECK(error == EINVAL);
	memcpy(modulus, key->n, k);
	modulus[k - 1U] &= 0xfeU;
	bad.n_size = k;
	error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, k);
	CHECK(error == EINVAL);
	error = cryc_rsa_pss_verify(&bad, CRYC_HASH_SHA256, hash, 32U, 32U, sig, k);
	CHECK(error == EINVAL);

	/* An even modulus is refused as EINVAL before its length is judged (too short and too long ones too). */
	for (index = 0; index < 3U; index++) {
		memset(modulus, 0xff, sizeof(modulus));
		modulus[even_bytes[index] - 1U] = 0xfeU;
		bad = rsa;
		bad.n = modulus;
		bad.n_size = even_bytes[index];
		error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, even_bytes[index]);
		CHECK(error == EINVAL);
		error = cryc_rsa_pss_verify(&bad, CRYC_HASH_SHA256, hash, 32U, 32U, sig, even_bytes[index]);
		CHECK(error == EINVAL);
	}

	/* Odd moduli of chosen lengths with all bits set below the top: the signature does not matter, the key is judged first. */
	for (index = 0; index < 8U; index++) {
		bits = bit_counts[index];
		bytes = (bits + 7U) / 8U;
		top = bits - (bytes - 1U) * 8U;
		memset(modulus, 0xff, sizeof(modulus));
		modulus[0] = (uint8_t)(0xffU >> (8U - top));
		bad = rsa;
		bad.n = modulus;
		bad.n_size = bytes;
		memset(sig, 0x11, sizeof(sig));
		error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, bytes);
		if (bits < 2048U || bits > 8192U)
			CHECK(error == ENOTSUP);
		else
			CHECK(error == EBADMSG);
		error = cryc_rsa_pss_verify(&bad, CRYC_HASH_SHA512, hash, 64U - 32U, 20U, sig, bytes);
		if (bits < 2048U || bits > 8192U)
			CHECK(error == ENOTSUP);
		else
			CHECK(error == EINVAL);
		error = cryc_rsa_pss_verify(&bad, CRYC_HASH_SHA512, hash, 64U, 20U, sig, bytes);
		if (bits < 2048U || bits > 8192U)
			CHECK(error == ENOTSUP);
		else
			CHECK(error == EBADMSG);
	}

	/* A huge modulus size (the largest the bytes could hold) is refused without reading it all. */
	size = 1025U;
	memset(modulus, 0xff, sizeof(modulus));
	bad = rsa;
	bad.n = modulus;
	bad.n_size = size;
	error = cryc_rsa_pkcs1_verify(&bad, CRYC_HASH_SHA256, hash, 32U, sig, size);
	CHECK(error == ENOTSUP);
}

/* Salt sizes near the limits with a valid signature: no overflow, always refused. */
static void
check_salt_limits(
	const struct tkey *key)
{
	uint8_t sig[K_MAX];
	uint8_t hash[HASH_MAX];
	uint8_t salt[HASH_MAX];
	size_t sizes[8];
	size_t index;
	int error;

	/* A good signature with a 32-byte salt. */
	random_bytes(hash, 32U);
	random_bytes(salt, 32U);
	error = sign_pss(key, CRYC_HASH_SHA256, hash, salt, 32U, sig);
	CHECK(error == 0);
	error = verify_pss(key, CRYC_HASH_SHA256, hash, 32U, sig);
	CHECK(error == 0);

	/* Sizes that would wrap a sum, and sizes just above the block. */
	sizes[0] = (size_t)-1;
	sizes[1] = (size_t)-1 - 1U;
	sizes[2] = (size_t)-1 - 33U;
	sizes[3] = (size_t)-1 - 34U;
	sizes[4] = (size_t)-1 - 35U;
	sizes[5] = key->n_size;
	sizes[6] = key->n_size + 1U;
	sizes[7] = (size_t)1 << (sizeof(size_t) * 8U - 1U);
	for (index = 0; index < 8U; index++) {
		error = verify_pss(key, CRYC_HASH_SHA256, hash, sizes[index], sig);
		CHECK(error == EBADMSG);
	}
}

/* Random keys, hashes, salts and signatures: only the documented errors, and no read outside the buffers (ASan). */
static void
check_random_input(void)
{
	uint8_t modulus[K_MAX];
	uint8_t exponent[8];
	uint8_t sig[K_MAX + 4U];
	uint8_t hash[HASH_MAX];
	struct cryc_rsa_key rsa;
	size_t n_size;
	size_t e_size;
	size_t index;
	size_t id;
	size_t hash_size;
	int error;
	int accepted;

	/* Exactly sized buffers for the hostile cases, so ASan sees any overrun. */
	accepted = 0;
	for (index = 0; index < 400U; index++) {
		n_size = 250U + (size_t)(random32() % 20U);
		e_size = 1U + (size_t)(random32() % 6U);
		random_bytes(modulus, n_size);
		modulus[0] |= 0x80U;
		modulus[n_size - 1U] |= 0x01U;
		random_bytes(exponent, e_size);
		if (index % 3U == 0U)
			exponent[e_size - 1U] |= 0x01U;
		random_bytes(sig, n_size);
		sig[0] = 0x00U;
		random_bytes(hash, HASH_MAX);
		rsa.n = modulus;
		rsa.n_size = n_size;
		rsa.e = exponent;
		rsa.e_size = e_size;
		id = (size_t)(random32() % 6U);

		/* The size of the hash the id names, and 64 for the ids that name none. */
		hash_size = 64U;
		if (id == 1U)
			hash_size = 32U;
		else if (id == 2U)
			hash_size = 48U;

		/* Verifies the random signature under the random key, both schemes. */
		error = cryc_rsa_pkcs1_verify(&rsa, (enum cryc_hash_id)id, hash, hash_size, sig, n_size);
		CHECK(error == EBADMSG || error == EINVAL || error == ENOTSUP);
		if (error == EBADMSG)
			accepted++;
		error = cryc_rsa_pss_verify(&rsa, (enum cryc_hash_id)id, hash, hash_size, random32() % 100U, sig, n_size);
		CHECK(error == EBADMSG || error == EINVAL || error == ENOTSUP);
	}

	/* Random keys that pass the checks of the library must give some bad-signature refusals. */
	CHECK(accepted > 20);
}

/* Returns the hash id named by a vector's text ("SHA256", "SHA-256", "SHA384", ...), or CRYC_HASH_NONE. */
static enum cryc_hash_id
hash_from_name(
	const char *name)
{
	int comparison;

	/* Both spellings of SHA-256. */
	comparison = strcmp(name, "SHA256");
	if (comparison == 0)
		return CRYC_HASH_SHA256;
	comparison = strcmp(name, "SHA-256");
	if (comparison == 0)
		return CRYC_HASH_SHA256;

	/* Both spellings of SHA-384. */
	comparison = strcmp(name, "SHA384");
	if (comparison == 0)
		return CRYC_HASH_SHA384;
	comparison = strcmp(name, "SHA-384");
	if (comparison == 0)
		return CRYC_HASH_SHA384;

	/* Both spellings of SHA-512. */
	comparison = strcmp(name, "SHA512");
	if (comparison == 0)
		return CRYC_HASH_SHA512;
	comparison = strcmp(name, "SHA-512");
	if (comparison == 0)
		return CRYC_HASH_SHA512;

	/* The name is no hash of the three. */
	return CRYC_HASH_NONE;
}

/* Decodes the hex value of key in the current record as a number (see hex_number); (size_t)-1 when absent or malformed. */
static size_t
field_number(
	const struct vec_file *file,
	const char *key,
	uint8_t *out,
	size_t room)
{
	const char *value;
	size_t decoded;

	/* The field, when there is one. */
	value = vec_get(file, key);
	if (value == NULL)
		return (size_t)-1;

	/* The bytes of the field's number. */
	decoded = hex_number(value, out, room);
	return decoded;
}

/* Returns 1 when the exponent bytes (leading zeros ignored) are an odd value from 3 to 2^32 - 1. */
static int
exponent_is_usable(
	const struct tkey *key)
{
	uint32_t value;
	size_t index;

	/* At most four bytes (the loader keeps up to eight). */
	if (key->e_size == 0U || key->e_size > 4U)
		return 0;
	value = 0;
	for (index = 0; index < key->e_size; index++)
		value = (value << 8) | key->e[index];
	if (value < 3U || (value & 1U) == 0U)
		return 0;
	return 1;
}

/* The CAVP SigVer file of PKCS1 v1.5: the modulus is in the record before its cases. */
static void
check_cavp_pkcs1(void)
{
	struct vec_file file;
	struct tkey key;
	static uint8_t msg[2048];
	static uint8_t sig[2048];
	uint8_t hash[HASH_MAX];
	enum cryc_hash_id id;
	const char *value;
	const char *result;
	size_t msg_size;
	size_t sig_size;
	size_t length;
	struct cryc_rsa_key rsa;
	char n_text[4096];
	int error;
	int have_n;
	int more;
	int usable;

	/* Opens the file. */
	error = vec_open(&file, "cavp-186-3rsatestvectors-SigVer15_186-3");
	if (error != 0) {
		CHECK(0);
		return;
	}

	/* No modulus is known before the first "n" record. */
	have_n = 0;
	n_text[0] = '\0';

	/* Records: an "n" record sets the modulus; the others are cases. */
	for (;;) {
		/* Reads the next record, and stops at the end of the file. */
		more = vec_next(&file);
		if (!more)
			break;

		/* An "n" record holds the modulus of the cases after it. */
		value = vec_get(&file, "n");
		if (value != NULL) {
			CHECK(strlen(value) < sizeof(n_text));
			snprintf(n_text, sizeof(n_text), "%s", value);
			have_n = 1;
			continue;
		}

		/* A case names its hash; a record without one is skipped. */
		value = vec_get(&file, "SHAAlg");
		if (value == NULL)
			continue;
		id = hash_from_name(value);
		if (id == CRYC_HASH_NONE)
			continue;
		CHECK(have_n);
		error = load_key(&key, n_text, vec_get(&file, "e"), NULL);
		CHECK(error == 0);
		msg_size = field_number(&file, "Msg", msg, sizeof(msg));
		sig_size = field_number(&file, "S", sig, sizeof(sig));
		CHECK(msg_size != (size_t)-1 && sig_size != (size_t)-1);
		result = vec_get(&file, "Result");
		CHECK(result != NULL);
		if (error != 0 || result == NULL || msg_size == (size_t)-1 || sig_size == (size_t)-1)
			continue;
		length = hash_length(id);
		digest(id, msg, msg_size, hash);
		public_key(&key, &rsa);
		error = cryc_rsa_pkcs1_verify(&rsa, id, hash, length, sig, sig_size);
		count_cavp_pkcs1++;

		/* Whether the exponent is one the library accepts. */
		usable = exponent_is_usable(&key);

		/* The verdict depends on the modulus size, the signature and the exponent. */
		if (key.bits < 2048U) {
			/* Below the floor: refused whatever the signature. */
			CHECK(error == ENOTSUP);
			count_cavp_small++;
		} else if (result[0] == 'P') {
			CHECK(error == 0);
			count_cavp_pkcs1_valid++;
		} else if (!usable) {
			CHECK(error == EINVAL);
		} else {
			CHECK(error == EBADMSG);
		}
	}

	/* Closes the file. */
	vec_close(&file);
}

/* The CAVP SigVer file of PSS: the key and the cases; valid cases are also reproduced with the signer here. */
static void
check_cavp_pss(void)
{
	struct vec_file file;
	struct tkey key;
	static uint8_t msg[2048];
	static uint8_t sig[2048];
	static uint8_t salt[2048];
	static uint8_t made[2048];
	uint8_t hash[HASH_MAX];
	enum cryc_hash_id id;
	const char *value;
	const char *result;
	size_t msg_size;
	size_t sig_size;
	size_t salt_size;
	size_t length;
	struct cryc_rsa_key rsa;
	char n_text[4096];
	int error;
	int have_n;
	int more;
	int usable;
	size_t slot;
	unsigned reproduced[6];

	/* Opens the file. */
	error = vec_open(&file, "cavp-186-3rsatestvectors-SigVerPSS_186-3");
	if (error != 0) {
		CHECK(0);
		return;
	}

	/* No modulus is known before the first "n" record, and no case is reproduced yet. */
	have_n = 0;
	n_text[0] = '\0';
	memset(reproduced, 0, sizeof(reproduced));

	/* Records: an "n" record sets the modulus; the others are cases. */
	for (;;) {
		/* Reads the next record, and stops at the end of the file. */
		more = vec_next(&file);
		if (!more)
			break;

		/* An "n" record holds the modulus of the cases after it. */
		value = vec_get(&file, "n");
		if (value != NULL) {
			CHECK(strlen(value) < sizeof(n_text));
			snprintf(n_text, sizeof(n_text), "%s", value);
			have_n = 1;
			continue;
		}

		/* A case names its hash; a record without one is skipped. */
		value = vec_get(&file, "SHAAlg");
		if (value == NULL)
			continue;
		id = hash_from_name(value);
		if (id == CRYC_HASH_NONE)
			continue;
		CHECK(have_n);
		error = load_key(&key, n_text, vec_get(&file, "e"), vec_get(&file, "d"));
		CHECK(error == 0);
		msg_size = field_number(&file, "Msg", msg, sizeof(msg));
		sig_size = field_number(&file, "S", sig, sizeof(sig));
		salt_size = field_number(&file, "SaltVal", salt, sizeof(salt));
		CHECK(msg_size != (size_t)-1 && sig_size != (size_t)-1 && salt_size != (size_t)-1);
		result = vec_get(&file, "Result");
		CHECK(result != NULL);
		if (error != 0 || result == NULL || msg_size == (size_t)-1 || sig_size == (size_t)-1 || salt_size == (size_t)-1)
			continue;
		length = hash_length(id);
		digest(id, msg, msg_size, hash);
		public_key(&key, &rsa);

		/*
		 * The 3072-bit cases of SHA-1, SHA-224, SHA-256 and SHA-512 give a
		 * one-byte "SaltVal = 00": a salt length of 0 (the valid cases verify, and
		 * are reproduced, with an empty salt and with no other length).
		 */
		if (key.bits == 3072U && salt_size == 1U && salt[0] == 0x00U)
			salt_size = 0;
		error = cryc_rsa_pss_verify(&rsa, id, hash, length, salt_size, sig, sig_size);
		count_cavp_pss++;

		/* Whether the exponent is one the library accepts. */
		usable = exponent_is_usable(&key);

		/* A key below the floor is refused whatever the signature. */
		if (key.bits < 2048U) {
			CHECK(error == ENOTSUP);
			count_cavp_small++;
			continue;
		}

		/* A valid case is accepted; an invalid one is refused for the reason its key gives. */
		if (result[0] == 'P') {
			CHECK(error == 0);
			count_cavp_pss_valid++;
		} else if (!usable) {
			CHECK(error == EINVAL);
		} else {
			CHECK(error == EBADMSG);
		}

		/* The slot that counts the reproduced cases of this hash and modulus size. */
		slot = (size_t)(id - 1);
		if (key.bits == 3072U)
			slot += 3U;

		/* A valid case: the key's private exponent and the case's salt must give the same signature with the signer here (a few per modulus size and hash). */
		if (result[0] == 'P' &&
		    key.bits >= 2048U &&
		    key.d_size > 0U &&
		    reproduced[slot] < 4U) {
			reproduced[slot]++;
			error = sign_pss(&key, id, hash, salt, salt_size, made);
			CHECK(error == 0);
			CHECK(sig_size == key.n_size);
			if (sig_size == key.n_size)
				CHECK_BYTES(made, sig, sig_size);
			count_cavp_reproduced++;

			/* The 2048-bit key for the later round trips. */
			if (key.bits == 2048U && !have_cavp_2048) {
				cavp_key_2048 = key;
				have_cavp_2048 = 1;
			}

			/* The 3072-bit key for the later round trips. */
			if (key.bits == 3072U && !have_cavp_3072) {
				cavp_key_3072 = key;
				have_cavp_3072 = 1;
			}
		}
	}

	/* Closes the file. */
	vec_close(&file);
}

/* Copies a field into a state buffer when the record has it. */
static void
state_copy(
	const struct vec_file *file,
	const char *key,
	char *target,
	size_t room)
{
	const char *value;

	/* Only when present. */
	value = vec_get(file, key);
	if (value == NULL)
		return;
	CHECK(strlen(value) < room);
	snprintf(target, room, "%s", value);
}

/* One Wycheproof file: kind 0 is PKCS1 v1.5, kind 1 is PSS. */
static void
check_wycheproof(
	const char *name,
	int kind)
{
	struct vec_file file;
	struct group_state state;
	struct tkey key;
	static uint8_t msg[8192];
	static uint8_t sig[8192];
	uint8_t hash[HASH_MAX];
	enum cryc_hash_id id;
	struct cryc_rsa_key rsa;
	const char *value;
	const char *result;
	const char *flags;
	const char *missing_null;
	size_t msg_size;
	size_t sig_size;
	size_t length;
	unsigned long salt_size;
	int error;
	int more;
	int valid_comparison;
	int invalid_comparison;

	/* Opens the file. */
	error = vec_open(&file, name);
	if (error != 0) {
		CHECK(0);
		return;
	}

	/* No group has been seen before the first record. */
	memset(&state, 0, sizeof(state));

	/* Records: group fields come before the case fields, in this record or the one before. */
	for (;;) {
		/* Reads the next record, and stops at the end of the file. */
		more = vec_next(&file);
		if (!more)
			break;

		/* Remembers the group fields this record carries. */
		state_copy(&file, "group.publicKey.modulus", state.modulus, sizeof(state.modulus));
		state_copy(&file, "group.publicKey.publicExponent", state.exponent, sizeof(state.exponent));
		state_copy(&file, "group.sha", state.sha, sizeof(state.sha));
		state_copy(&file, "group.mgfSha", state.mgf_sha, sizeof(state.mgf_sha));
		value = vec_get(&file, "group.sLen");
		if (value != NULL) {
			state.salt_size = strtoul(value, NULL, 10);
			state.have_salt_size = 1;
		}

		/* A record without a test id carries group fields only. */
		value = vec_get(&file, "tcId");
		if (value == NULL)
			continue;

		/* The case. */
		CHECK(state.modulus[0] != '\0' && state.exponent[0] != '\0');
		id = hash_from_name(state.sha);
		CHECK(id != CRYC_HASH_NONE);
		if (id == CRYC_HASH_NONE || state.modulus[0] == '\0')
			continue;
		error = load_key(&key, state.modulus, state.exponent, NULL);
		CHECK(error == 0);
		msg_size = vec_hex(&file, "msg", msg, sizeof(msg));
		sig_size = vec_hex(&file, "sig", sig, sizeof(sig));
		CHECK(msg_size != (size_t)-1 && sig_size != (size_t)-1);
		result = vec_get(&file, "result");
		flags = vec_get(&file, "flags");
		CHECK(result != NULL && flags != NULL);
		if (error != 0 || result == NULL || flags == NULL || msg_size == (size_t)-1 || sig_size == (size_t)-1)
			continue;
		length = hash_length(id);
		digest(id, msg, msg_size, hash);
		public_key(&key, &rsa);
		count_wycheproof++;
		if (kind == 0) {
			error = cryc_rsa_pkcs1_verify(&rsa, id, hash, length, sig, sig_size);
		} else {
			CHECK(state.have_salt_size);
			CHECK(strcmp(state.sha, state.mgf_sha) == 0);
			salt_size = state.salt_size;
			error = cryc_rsa_pss_verify(&rsa, id, hash, length, (size_t)salt_size, sig, sig_size);
		}

		/* Classifies the expected result of the case. */
		valid_comparison = strcmp(result, "valid");
		invalid_comparison = strcmp(result, "invalid");

		/* valid: accepted; invalid: refused as a bad signature; acceptable: either, but a DigestInfo without NULL is refused here. */
		if (valid_comparison == 0) {
			CHECK(error == 0);
			count_wycheproof_valid++;
		} else if (invalid_comparison == 0) {
			CHECK(error == EBADMSG);
		} else {
			CHECK(error == 0 || error == EBADMSG);
			missing_null = strstr(flags, "MissingNull");
			if (missing_null != NULL)
				CHECK(error == EBADMSG);
			count_wycheproof_acceptable++;
		}
	}

	/* Closes the file. */
	vec_close(&file);
}

/* Runs every check. */
int
main(void)
{
	size_t index;
	size_t which;
	size_t salt_count;
	int status;

	/* The standards' vectors first (they also find the CAVP keys with private exponents). */
	check_digest_info();
	check_cavp_pkcs1();
	check_cavp_pss();
	check_wycheproof("wycheproof-rsa_signature_2048_sha256_test", 0);
	check_wycheproof("wycheproof-rsa_signature_2048_sha384_test", 0);
	check_wycheproof("wycheproof-rsa_signature_2048_sha512_test", 0);
	check_wycheproof("wycheproof-rsa_signature_3072_sha256_test", 0);
	check_wycheproof("wycheproof-rsa_signature_4096_sha512_test", 0);
	check_wycheproof("wycheproof-rsa_pss_2048_sha256_mgf1_32_test", 1);
	check_wycheproof("wycheproof-rsa_pss_3072_sha256_mgf1_32_test", 1);
	check_wycheproof("wycheproof-rsa_pss_4096_sha512_mgf1_64_test", 1);
	printf("cavp pkcs1 %lu (valid %lu), cavp pss %lu (valid %lu), below 2048 bits %lu, signatures reproduced %lu\n",
	    count_cavp_pkcs1, count_cavp_pkcs1_valid, count_cavp_pss, count_cavp_pss_valid, count_cavp_small,
	    count_cavp_reproduced);
	printf("wycheproof %lu (valid %lu, acceptable %lu)\n", count_wycheproof, count_wycheproof_valid,
	    count_wycheproof_acceptable);
	CHECK(count_cavp_pkcs1_valid >= 18U && count_cavp_pss_valid >= 18U && count_cavp_reproduced >= 18U);
	CHECK(count_wycheproof > 1600U && count_wycheproof_valid > 150U);
	CHECK(have_cavp_2048 && have_cavp_3072);

	/* The keys made here. */
	for (index = 0; index < 9U; index++)
		make_key(&mersenne_keys[index], &key_specs[index]);

	/* Round trips and forgeries on the CAVP keys: all hashes, both schemes. */
	if (have_cavp_2048) {
		round_trip_key(&cavp_key_2048, CRYC_HASH_SHA256, 1, 4);
		round_trip_key(&cavp_key_2048, CRYC_HASH_SHA384, 1, 1);
		round_trip_key(&cavp_key_2048, CRYC_HASH_SHA512, 1, 2);
		forge_pkcs1(&cavp_key_2048, CRYC_HASH_SHA256);
		forge_pss(&cavp_key_2048, CRYC_HASH_SHA256);
		check_signature_forms(&cavp_key_2048);
		check_arguments(&cavp_key_2048);
		check_salt_limits(&cavp_key_2048);
	}

	/* The 3072-bit CAVP key gets one round trip. */
	if (have_cavp_3072)
		round_trip_key(&cavp_key_3072, CRYC_HASH_SHA384, 1, 1);

	/* The built keys: every modBits modulo 8, and the limit. */
	for (which = 0; which < 9U; which++) {
		if (which == 8U) {
			round_trip_key(&mersenne_keys[which], CRYC_HASH_SHA512, 1, 1);
			continue;
		}

		/* The first key checks two salt sizes, the others one. */
		salt_count = 1U;
		if (which == 0U)
			salt_count = 2U;
		round_trip_key(&mersenne_keys[which], hash_ids[which % 3U], 1, salt_count);
		forge_pss(&mersenne_keys[which], hash_ids[(which + 1U) % 3U]);
		if (which == 0U || which == 7U)
			forge_pkcs1(&mersenne_keys[which], hash_ids[(which + 2U) % 3U]);
	}

	/* The signature forms, the salt limits and the leading zero case on the built keys. */
	check_signature_forms(&mersenne_keys[0]);
	check_leading_zero_signature(&mersenne_keys[0]);
	check_salt_limits(&mersenne_keys[0]);
	check_salt_limits(&mersenne_keys[3]);

	/* Random input. */
	check_random_input();

	/* Reports the outcome of the checks as the exit status. */
	status = check_finish("test-rsa");
	return status;
}
