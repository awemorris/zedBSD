/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks K-PKE and ML-KEM-768 (cryc-mlkem.h, FIPS 203 5 to 7).
 *
 * The expected values come from the standard and from the published
 * vectors, not from another implementation:
 *
 *   - Wycheproof ML-KEM-768 (the key generation, encapsulation, general and
 *     semi-expanded decapsulation files): key generation from a seed
 *     (ek and dk byte for byte), encapsulation from a message (c and K),
 *     decapsulation of valid, bit-flipped and random ciphertexts (the
 *     latter two answer the implicit-rejection key), the modulus check of
 *     ek and the hash check of dk.  Cases whose only fault is a wrong
 *     length are not applicable to these functions (the callers check
 *     sizes, as the header says) and are only counted.
 *   - The implicit-rejection key is recomputed here from J(z || c) =
 *     SHAKE256(z || c, 32) (4.1) with the sponge of cryc-sha3.h, which has
 *     its own tests.
 *   - Round trips, the layout of dk (Algorithm 16 line 3), the sizes of
 *     Table 3, and the error and wiping behaviour of the functions that
 *     take a random source are checked with the standard's definitions.
 *
 * Buffers handed to the functions are exactly the size the header names,
 * allocated one by one, so AddressSanitizer reports a read or write outside
 * them.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-mlkem.h"

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x1badcafeU;

/*
 * A deterministic random source: it hands out the bytes of data in order
 * and fails (EIO) on the call number fail_call (counting from 1), or when
 * the data runs out.  calls counts the calls made.
 */
struct fixed_random {
	const uint8_t *data;
	size_t size;
	size_t position;
	unsigned calls;
	unsigned fail_call;
};

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
static void random_bytes(uint8_t *out, size_t size);

/* The cryc_random_fn of the tests: serves the bytes of a struct fixed_random. */
static int fixed_random_fn(void *context, uint8_t *out, size_t size);

/* Allocates an exact-size buffer or stops the test. */
static uint8_t * exact(size_t size);

/* Returns 1 when size bytes at buffer are all value. */
static int all_bytes(const uint8_t *buffer, size_t size, uint8_t value);

/* J(z || c) = SHAKE256(z || c, 32) (4.1), the implicit-rejection key, for a 1088-byte c. */
static void reference_j(uint8_t *out, const uint8_t *z, const uint8_t *ciphertext);

/* H(data) = SHA3-256 (4.1). */
static void reference_h(uint8_t *out, const uint8_t *data, size_t size);

/* Checks key generation, the key checks and the layout of dk for one seed d || z. */
static void check_key_from_seed(const uint8_t *seed, const uint8_t *ek_expected, const uint8_t *dk_expected);

/* Sets coefficient 2 * pair + which of the 12-bit polynomial at bytes (384 bytes) to value, leaving the others. */
static void set_coefficient(uint8_t *bytes, unsigned index, unsigned value);

/* Fills size bytes with pseudo-random bytes. */
static void
random_bytes(
	uint8_t *out,
	size_t size)
{
	size_t i;

	/* One byte per draw, from the high bits. */
	for (i = 0; i < size; i++)
		out[i] = (uint8_t)(random32() >> 11);
}

/* The cryc_random_fn of the tests: serves the bytes of a struct fixed_random. */
static int
fixed_random_fn(
	void *context,
	uint8_t *out,
	size_t size)
{
	struct fixed_random *source;

	/* Counts the call and fails where the test asked or when the data is short. */
	source = (struct fixed_random *)context;
	source->calls++;
	if (source->calls == source->fail_call || source->position + size > source->size) {
		memset(out, 0x5a, size);
		return EIO;
	}

	/* Serves the next bytes. */
	memcpy(out, source->data + source->position, size);
	source->position += size;
	return 0;
}

/* Allocates an exact-size buffer or stops the test. */
static uint8_t *
exact(
	size_t size)
{
	uint8_t *buffer;

	/* One allocation of exactly size bytes. */
	buffer = (uint8_t *)malloc(size);
	if (buffer == NULL) {
		printf("FAIL out of memory\n");
		exit(EXIT_FAILURE);
	}

	/* Succeeded: the caller owns the buffer. */
	return buffer;
}

/* Returns 1 when size bytes at buffer are all value. */
static int
all_bytes(
	const uint8_t *buffer,
	size_t size,
	uint8_t value)
{
	size_t i;

	/* Any other byte makes it 0. */
	for (i = 0; i < size; i++) {
		if (buffer[i] != value)
			return 0;
	}

	/* Succeeded: every byte equals the value. */
	return 1;
}

/* J(z || c) = SHAKE256(z || c, 32) (4.1), the implicit-rejection key, for a 1088-byte c. */
static void
reference_j(
	uint8_t *out,
	const uint8_t *z,
	const uint8_t *ciphertext)
{
	struct cryc_sha3 sponge;

	/* Absorbs z then the ciphertext and squeezes 32 bytes. */
	cryc_shake256_init(&sponge);
	cryc_sha3_absorb(&sponge, z, 32U);
	cryc_sha3_absorb(&sponge, ciphertext, CRYC_MLKEM768_CT);
	cryc_shake_squeeze(&sponge, out, 32U);
	cryc_sha3_clear(&sponge);
}

/* H(data) = SHA3-256 (4.1). */
static void
reference_h(
	uint8_t *out,
	const uint8_t *data,
	size_t size)
{
	struct cryc_sha3 sponge;

	/* One pass. */
	cryc_sha3_256_init(&sponge);
	cryc_sha3_absorb(&sponge, data, size);
	cryc_sha3_final(&sponge, out);
}

/* Checks the sizes of Table 3 and the constants of the header. */
static void
test_constants(void)
{
	/* Table 3, ML-KEM-768: ek 1184, dk 2400, c 1088, K 32 bytes; the seeds d and z are 32 bytes each. */
	CHECK(CRYC_MLKEM768_EK == 1184U);
	CHECK(CRYC_MLKEM768_DK == 2400U);
	CHECK(CRYC_MLKEM768_CT == 1088U);
	CHECK(CRYC_MLKEM_SHARED == 32U);
	CHECK(CRYC_MLKEM_SEED == 32U);

	/* The sizes follow from k = 3, du = 10, dv = 4: 384k + 32, 768k + 96, 32(du k + dv). */
	CHECK(CRYC_MLKEM768_EK == 384U * 3U + 32U);
	CHECK(CRYC_MLKEM768_DK == 768U * 3U + 96U);
	CHECK(CRYC_MLKEM768_CT == 32U * (10U * 3U + 4U));
}

/* Checks key generation, the key checks and the layout of dk for one seed d || z. */
static void
check_key_from_seed(
	const uint8_t *seed,
	const uint8_t *ek_expected,
	const uint8_t *dk_expected)
{
	uint8_t *ek;
	uint8_t *dk;
	uint8_t hash[32];

	/* Runs the deterministic key generation. */
	ek = exact(CRYC_MLKEM768_EK);
	dk = exact(CRYC_MLKEM768_DK);
	cryc_mlkem768_keygen_internal(ek, dk, seed, seed + 32);

	/* The published ek and dk, when the record has them. */
	if (ek_expected != NULL)
		CHECK_BYTES(ek, ek_expected, CRYC_MLKEM768_EK);
	if (dk_expected != NULL)
		CHECK_BYTES(dk, dk_expected, CRYC_MLKEM768_DK);

	/* dk = dkPKE || ek || H(ek) || z (Algorithm 16 line 3). */
	CHECK_BYTES(dk + 1152, ek, 1184U);
	reference_h(hash, ek, 1184U);
	CHECK_BYTES(dk + 2336, hash, 32U);
	CHECK_BYTES(dk + 2368, seed + 32, 32U);

	/* The generated keys pass their own checks. */
	CHECK(cryc_mlkem768_check_ek(ek) == 0);
	CHECK(cryc_mlkem768_check_dk(dk) == 0);

	/* Releases the buffers. */
	free(ek);
	free(dk);
}

/* Checks key generation against the Wycheproof seed vectors (100 cases). */
static void
test_vectors_keygen(void)
{
	struct vec_file file;
	uint8_t seed[64];
	uint8_t ek[1184];
	uint8_t dk[2400];
	size_t seed_size;
	size_t ek_size;
	size_t dk_size;
	const char *result;
	unsigned count;
	int opened;
	int more;
	int verdict;

	/* Opens the vectors. */
	count = 0U;
	opened = vec_open(&file, "wycheproof-mlkem_768_keygen_seed_test");
	if (opened != 0) {
		CHECK(0);
		return;
	}

	/* Each record: the seed d || z gives exactly ek and dk. */
	for (more = vec_next(&file);
	     more;
	     more = vec_next(&file)) {
		result = vec_get(&file, "result");
		seed_size = vec_hex(&file, "seed", seed, sizeof(seed));
		ek_size = vec_hex(&file, "ek", ek, sizeof(ek));
		dk_size = vec_hex(&file, "dk", dk, sizeof(dk));

		/* Only valid records are checked. */
		if (result == NULL)
			continue;
		verdict = strcmp(result, "valid");
		if (verdict != 0)
			continue;
		CHECK(seed_size == 64U && ek_size == 1184U && dk_size == 2400U);
		if (seed_size != 64U || ek_size != 1184U || dk_size != 2400U)
			continue;
		count++;
		check_key_from_seed(seed, ek, dk);
	}

	/* Releases the file. */
	vec_close(&file);

	/* All 100 records of the file were checked. */
	CHECK(count == 100U);
}

/*
 * Checks encapsulation against the Wycheproof vectors: valid cases give
 * the published c and K (through Encaps_internal and through Encaps with
 * a source that serves m), and an ek that fails the modulus check is
 * refused with EINVAL and the outputs wiped.  A case whose only fault is
 * the length of ek is counted, not run.
 */
static void
test_vectors_encaps(void)
{
	struct vec_file file;
	struct fixed_random source;
	uint8_t m[32];
	uint8_t ek[1184];
	uint8_t c[1088];
	uint8_t key[32];
	uint8_t *made_c;
	uint8_t *made_key;
	size_t m_size;
	size_t ek_size;
	size_t c_size;
	size_t key_size;
	const char *result;
	unsigned valid;
	unsigned refused;
	unsigned skipped;
	int error;
	int opened;
	int more;
	int verdict;

	/* Opens the vectors. */
	valid = 0U;
	refused = 0U;
	skipped = 0U;
	opened = vec_open(&file, "wycheproof-mlkem_768_encaps_test");
	if (opened != 0) {
		CHECK(0);
		return;
	}

	/* Allocates the buffers of the outputs at their exact sizes. */
	made_c = exact(CRYC_MLKEM768_CT);
	made_key = exact(CRYC_MLKEM_SHARED);

	/* Each record. */
	for (more = vec_next(&file);
	     more;
	     more = vec_next(&file)) {
		result = vec_get(&file, "result");
		m_size = vec_hex(&file, "m", m, sizeof(m));
		ek_size = vec_hex(&file, "ek", ek, sizeof(ek));
		if (result == NULL)
			continue;

		/* A valid case: Encaps_internal gives c and K, and Encaps does the same when its random source serves m. */
		verdict = strcmp(result, "valid");
		if (verdict == 0) {
			c_size = vec_hex(&file, "c", c, sizeof(c));
			key_size = vec_hex(&file, "K", key, sizeof(key));
			CHECK(m_size == 32U && ek_size == 1184U && c_size == 1088U && key_size == 32U);
			if (m_size != 32U || ek_size != 1184U || c_size != 1088U || key_size != 32U)
				continue;
			valid++;
			CHECK(cryc_mlkem768_check_ek(ek) == 0);
			cryc_mlkem768_encaps_internal(made_key, made_c, ek, m);
			CHECK_BYTES(made_c, c, 1088U);
			CHECK_BYTES(made_key, key, 32U);

			/* Encaps with a source that serves m gives the same c and K. */
			memset(&source, 0, sizeof(source));
			source.data = m;
			source.size = sizeof(m);
			memset(made_c, 0, CRYC_MLKEM768_CT);
			memset(made_key, 0, CRYC_MLKEM_SHARED);
			error = cryc_mlkem768_encaps(made_key, made_c, ek, fixed_random_fn, &source);
			CHECK(error == 0);
			CHECK(source.calls == 1U);
			CHECK_BYTES(made_c, c, 1088U);
			CHECK_BYTES(made_key, key, 32U);
			continue;
		}

		/* An invalid case with a key of the right length: the modulus check refuses it and Encaps wipes its outputs without drawing random bytes. */
		if (ek_size == 1184U) {
			refused++;
			CHECK(cryc_mlkem768_check_ek(ek) == EINVAL);
			memset(&source, 0, sizeof(source));
			source.data = m;
			source.size = sizeof(m);
			memset(made_c, 0xaa, CRYC_MLKEM768_CT);
			memset(made_key, 0xaa, CRYC_MLKEM_SHARED);
			error = cryc_mlkem768_encaps(made_key, made_c, ek, fixed_random_fn, &source);
			CHECK(error == EINVAL);
			CHECK(source.calls == 0U);
			CHECK(all_bytes(made_c, CRYC_MLKEM768_CT, 0U));
			CHECK(all_bytes(made_key, CRYC_MLKEM_SHARED, 0U));
			continue;
		}

		/* A wrong-length key: a caller's check. */
		skipped++;
	}

	/* Releases the file. */
	vec_close(&file);
	free(made_c);
	free(made_key);

	/* 133 valid cases, 112 invalid ones with a key of the right length (100 not reduced, 12 modulus overflow), 20 of the wrong length. */
	CHECK(valid == 133U);
	CHECK(refused == 112U);
	CHECK(skipped == 20U);
}

/*
 * Checks decapsulation against the Wycheproof general vectors: for every
 * seed with a ciphertext of the right length, the key from the seed
 * decapsulates the ciphertext to K (the valid, bit-flipped and random
 * ciphertexts alike: those with a fault answer the implicit-rejection key).
 */
static void
test_vectors_decaps(void)
{
	struct vec_file file;
	uint8_t seed[64];
	uint8_t ek[1184];
	uint8_t c[1088];
	uint8_t key[32];
	uint8_t rejection[32];
	uint8_t *dk;
	uint8_t *made_ek;
	uint8_t *made_key;
	size_t seed_size;
	size_t ek_size;
	size_t c_size;
	size_t key_size;
	const char *result;
	unsigned valid;
	unsigned skipped;
	unsigned rejected;
	int opened;
	int more;
	int verdict;
	int different;

	/* Opens the vectors. */
	valid = 0U;
	skipped = 0U;
	rejected = 0U;
	opened = vec_open(&file, "wycheproof-mlkem_768_test");
	if (opened != 0) {
		CHECK(0);
		return;
	}

	/* Allocates the buffers of the outputs at their exact sizes. */
	dk = exact(CRYC_MLKEM768_DK);
	made_ek = exact(CRYC_MLKEM768_EK);
	made_key = exact(CRYC_MLKEM_SHARED);

	/* Each record. */
	for (more = vec_next(&file);
	     more;
	     more = vec_next(&file)) {
		result = vec_get(&file, "result");
		seed_size = vec_hex(&file, "seed", seed, sizeof(seed));
		ek_size = vec_hex(&file, "ek", ek, sizeof(ek));
		c_size = vec_hex(&file, "c", c, sizeof(c));
		key_size = vec_hex(&file, "K", key, sizeof(key));
		if (result == NULL)
			continue;

		/* A case with a bad seed or ciphertext length is a caller's check. */
		verdict = strcmp(result, "valid");
		if (verdict != 0) {
			skipped++;
			continue;
		}

		/* The lengths of a valid record are the fixed ones. */
		CHECK(seed_size == 64U && ek_size == 1184U && c_size == 1088U && key_size == 32U);
		if (seed_size != 64U || ek_size != 1184U || c_size != 1088U || key_size != 32U)
			continue;
		valid++;

		/* The key from the seed is the published ek and decapsulates c to K. */
		cryc_mlkem768_keygen_internal(made_ek, dk, seed, seed + 32);
		CHECK_BYTES(made_ek, ek, 1184U);
		CHECK(cryc_mlkem768_check_dk(dk) == 0);
		cryc_mlkem768_decaps(made_key, c, dk);
		CHECK_BYTES(made_key, key, 32U);

		/* A case whose K is the implicit-rejection key agrees with J(z || c). */
		reference_j(rejection, seed + 32, c);
		different = memcmp(rejection, key, 32U);
		if (different == 0)
			rejected++;
	}

	/* Releases the file. */
	vec_close(&file);
	free(dk);
	free(made_ek);
	free(made_key);

	/* 161 valid cases; the 40 others have a seed or ciphertext of the wrong length; the random and bit-flipped ciphertexts (20 of the valid ones) are rejections. */
	CHECK(valid == 161U);
	CHECK(skipped == 40U);
	CHECK(rejected >= 20U);
}

/*
 * Checks the semi-expanded decapsulation vectors: a valid dk decapsulates
 * to K (including the two keys with a zeroed byte whose re-encryption
 * comparison must still be exact), and a dk whose hash check fails is
 * refused with EINVAL.  Cases whose only fault is a length are counted.
 */
static void
test_vectors_semi_expanded(void)
{
	struct vec_file file;
	uint8_t dk[2400];
	uint8_t c[1088];
	uint8_t key[32];
	uint8_t *made_key;
	size_t dk_size;
	size_t c_size;
	size_t key_size;
	const char *result;
	const char *flags;
	unsigned valid;
	unsigned bad_key;
	unsigned skipped;
	int opened;
	int more;
	int verdict;
	int flagged;

	/* Opens the vectors. */
	valid = 0U;
	bad_key = 0U;
	skipped = 0U;
	opened = vec_open(&file, "wycheproof-mlkem_768_semi_expanded_decaps_test");
	if (opened != 0) {
		CHECK(0);
		return;
	}

	/* Allocates the buffer of the output at its exact size. */
	made_key = exact(CRYC_MLKEM_SHARED);

	/* Each record. */
	for (more = vec_next(&file);
	     more;
	     more = vec_next(&file)) {
		result = vec_get(&file, "result");
		flags = vec_get(&file, "flags");
		dk_size = vec_hex(&file, "dk", dk, sizeof(dk));
		c_size = vec_hex(&file, "c", c, sizeof(c));
		key_size = vec_hex(&file, "K", key, sizeof(key));
		if (result == NULL)
			continue;

		/* A valid case. */
		verdict = strcmp(result, "valid");
		if (verdict == 0) {
			CHECK(dk_size == 2400U && c_size == 1088U && key_size == 32U);
			if (dk_size != 2400U || c_size != 1088U || key_size != 32U)
				continue;
			valid++;
			CHECK(cryc_mlkem768_check_dk(dk) == 0);
			cryc_mlkem768_decaps(made_key, c, dk);
			CHECK_BYTES(made_key, key, 32U);
			continue;
		}

		/* An invalid dk of the right length: the hash check refuses it. */
		flagged = 0;
		if (flags != NULL) {
			verdict = strcmp(flags, "InvalidDecapsulationKey");
			if (verdict == 0)
				flagged = 1;
		}

		/* The flagged key is counted and must be refused. */
		if (flagged) {
			bad_key++;
			CHECK(dk_size == 2400U);
			CHECK(cryc_mlkem768_check_dk(dk) == EINVAL);
			continue;
		}

		/* A wrong length of dk or c: a caller's check. */
		skipped++;
	}

	/* Releases the file. */
	vec_close(&file);
	free(made_key);

	/* Three valid cases, two corrupted keys, four wrong lengths. */
	CHECK(valid == 3U);
	CHECK(bad_key == 2U);
	CHECK(skipped == 4U);
}

/* Checks KeyGen with a random source: d then z are drawn, the result equals KeyGen_internal, and a failure wipes the outputs. */
static void
test_keygen_random(void)
{
	struct fixed_random source;
	uint8_t seed[64];
	uint8_t *ek;
	uint8_t *dk;
	uint8_t *ek_expected;
	uint8_t *dk_expected;
	unsigned fail;
	int error;

	/* Allocates the key buffers at their exact sizes, so an access past them is an error. */
	ek = exact(CRYC_MLKEM768_EK);
	dk = exact(CRYC_MLKEM768_DK);
	ek_expected = exact(CRYC_MLKEM768_EK);
	dk_expected = exact(CRYC_MLKEM768_DK);

	/* The random source serves d and then z; the key is KeyGen_internal(d, z). */
	random_bytes(seed, sizeof(seed));
	memset(&source, 0, sizeof(source));
	source.data = seed;
	source.size = sizeof(seed);
	error = cryc_mlkem768_keygen(ek, dk, fixed_random_fn, &source);
	CHECK(error == 0);
	CHECK(source.calls == 2U);
	CHECK(source.position == 64U);
	cryc_mlkem768_keygen_internal(ek_expected, dk_expected, seed, seed + 32);
	CHECK_BYTES(ek, ek_expected, CRYC_MLKEM768_EK);
	CHECK_BYTES(dk, dk_expected, CRYC_MLKEM768_DK);

	/* A failure of either draw gives EIO and wipes ek and dk. */
	for (fail = 1U; fail <= 2U; fail++) {
		memset(&source, 0, sizeof(source));
		source.data = seed;
		source.size = sizeof(seed);
		source.fail_call = fail;
		memset(ek, 0xaa, CRYC_MLKEM768_EK);
		memset(dk, 0xaa, CRYC_MLKEM768_DK);
		error = cryc_mlkem768_keygen(ek, dk, fixed_random_fn, &source);
		CHECK(error == EIO);
		CHECK(all_bytes(ek, CRYC_MLKEM768_EK, 0U));
		CHECK(all_bytes(dk, CRYC_MLKEM768_DK, 0U));
	}

	/* A source that runs dry after d fails the second draw. */
	memset(&source, 0, sizeof(source));
	source.data = seed;
	source.size = 32U;
	error = cryc_mlkem768_keygen(ek, dk, fixed_random_fn, &source);
	CHECK(error == EIO);
	CHECK(all_bytes(dk, CRYC_MLKEM768_DK, 0U));

	/* Releases the buffers. */
	free(ek);
	free(dk);
	free(ek_expected);
	free(dk_expected);
}

/* Checks that an encapsulation failing for lack of random bytes gives EIO and wipes the outputs. */
static void
test_encaps_random_failure(void)
{
	struct fixed_random source;
	uint8_t seed[64];
	uint8_t m[32];
	uint8_t *ek;
	uint8_t *dk;
	uint8_t *c;
	uint8_t *key;
	int error;

	/* Allocates the key buffers at their exact sizes, so an access past them is an error. */
	ek = exact(CRYC_MLKEM768_EK);
	dk = exact(CRYC_MLKEM768_DK);
	c = exact(CRYC_MLKEM768_CT);
	key = exact(CRYC_MLKEM_SHARED);

	/* A valid key, and a source that fails its only draw. */
	random_bytes(seed, sizeof(seed));
	cryc_mlkem768_keygen_internal(ek, dk, seed, seed + 32);
	memset(m, 0, sizeof(m));
	memset(&source, 0, sizeof(source));
	source.data = m;
	source.size = sizeof(m);
	source.fail_call = 1U;
	memset(c, 0xaa, CRYC_MLKEM768_CT);
	memset(key, 0xaa, CRYC_MLKEM_SHARED);
	error = cryc_mlkem768_encaps(key, c, ek, fixed_random_fn, &source);
	CHECK(error == EIO);
	CHECK(all_bytes(c, CRYC_MLKEM768_CT, 0U));
	CHECK(all_bytes(key, CRYC_MLKEM_SHARED, 0U));

	/* Releases the buffers. */
	free(ek);
	free(dk);
	free(c);
	free(key);
}

/* Sets coefficient 2 * pair + which of the 12-bit polynomial at bytes (384 bytes) to value, leaving the others. */
static void
set_coefficient(
	uint8_t *bytes,
	unsigned index,
	unsigned value)
{
	unsigned base;

	/* Two coefficients share three bytes: the first is byte 0 and the low nibble of byte 1, the second the high nibble of byte 1 and byte 2 (Algorithm 5 with d = 12). */
	base = (index / 2U) * 3U;
	if ((index & 1U) == 0U) {
		bytes[base] = (uint8_t)(value & 0xffU);
		bytes[base + 1U] = (uint8_t)((bytes[base + 1U] & 0xf0U) | ((value >> 8) & 0x0fU));
	} else {
		bytes[base + 1U] = (uint8_t)((bytes[base + 1U] & 0x0fU) | ((value & 0x0fU) << 4));
		bytes[base + 2U] = (uint8_t)(value >> 4);
	}
}

/* Checks the modulus check of ek at the boundary values of q in every polynomial and at both ends of each. */
static void
test_check_ek(void)
{
	uint8_t seed[64];
	uint8_t *ek;
	uint8_t *dk;
	unsigned poly;
	unsigned position;
	unsigned positions[4];
	unsigned p;

	/* Allocates the key buffers at their exact sizes, so an access past them is an error. */
	ek = exact(CRYC_MLKEM768_EK);
	dk = exact(CRYC_MLKEM768_DK);
	random_bytes(seed, sizeof(seed));
	cryc_mlkem768_keygen_internal(ek, dk, seed, seed + 32);
	CHECK(cryc_mlkem768_check_ek(ek) == 0);

	/* The first, second, next-to-last and last coefficients of each polynomial: q - 1 passes; q, q + 1 and 4095 fail. */
	positions[0] = 0U;
	positions[1] = 1U;
	positions[2] = 254U;
	positions[3] = 255U;
	for (poly = 0U; poly < 3U; poly++) {
		for (p = 0U; p < 4U; p++) {
			position = positions[p];
			set_coefficient(ek + 384U * poly, position, 3328U);
			CHECK(cryc_mlkem768_check_ek(ek) == 0);
			set_coefficient(ek + 384U * poly, position, 3329U);
			CHECK(cryc_mlkem768_check_ek(ek) == EINVAL);
			set_coefficient(ek + 384U * poly, position, 3330U);
			CHECK(cryc_mlkem768_check_ek(ek) == EINVAL);
			set_coefficient(ek + 384U * poly, position, 4095U);
			CHECK(cryc_mlkem768_check_ek(ek) == EINVAL);
			set_coefficient(ek + 384U * poly, position, 0U);
			CHECK(cryc_mlkem768_check_ek(ek) == 0);
			memcpy(ek, dk + 1152, CRYC_MLKEM768_EK);
		}
	}

	/* The seed rho at the end is not covered by the modulus check: any value passes. */
	memset(ek + 1152, 0xff, 32U);
	CHECK(cryc_mlkem768_check_ek(ek) == 0);

	/* Releases the buffers. */
	free(ek);
	free(dk);
}

/* Checks the hash check of dk: it fails exactly when a byte of the embedded ek or of the hash changes. */
static void
test_check_dk(void)
{
	uint8_t seed[64];
	uint8_t *ek;
	uint8_t *dk;
	unsigned i;
	int error;
	int expected;

	/* Allocates the key buffers at their exact sizes, so an access past them is an error. */
	ek = exact(CRYC_MLKEM768_EK);
	dk = exact(CRYC_MLKEM768_DK);
	random_bytes(seed, sizeof(seed));
	cryc_mlkem768_keygen_internal(ek, dk, seed, seed + 32);

	/* Each byte of dk flipped in turn: the check covers bytes 1152 to 2367 (ek and H(ek)) and not dkPKE or z. */
	for (i = 0U; i < CRYC_MLKEM768_DK; i++) {
		dk[i] ^= 0x01U;
		error = cryc_mlkem768_check_dk(dk);
		expected = 0;
		if (i >= 1152U && i < 2368U)
			expected = EINVAL;
		CHECK(error == expected);
		dk[i] ^= 0x01U;
	}

	/* The key is whole again after every byte was flipped back. */
	CHECK(cryc_mlkem768_check_dk(dk) == 0);

	/* Releases the buffers. */
	free(ek);
	free(dk);
}

/* Checks that a ciphertext that was changed gives the implicit-rejection key J(z || c'), never K and never an error. */
static void
test_implicit_rejection(void)
{
	uint8_t seed[64];
	uint8_t m[32];
	uint8_t *ek;
	uint8_t *dk;
	uint8_t *c;
	uint8_t *key;
	uint8_t *got;
	uint8_t expected[32];
	unsigned i;
	unsigned bit;
	unsigned rounds;

	/* Allocates the key buffers at their exact sizes, so an access past them is an error. */
	ek = exact(CRYC_MLKEM768_EK);
	dk = exact(CRYC_MLKEM768_DK);
	c = exact(CRYC_MLKEM768_CT);
	key = exact(CRYC_MLKEM_SHARED);
	got = exact(CRYC_MLKEM_SHARED);

	/* A key pair and a valid encapsulation. */
	random_bytes(seed, sizeof(seed));
	random_bytes(m, sizeof(m));
	cryc_mlkem768_keygen_internal(ek, dk, seed, seed + 32);
	cryc_mlkem768_encaps_internal(key, c, ek, m);
	cryc_mlkem768_decaps(got, c, dk);
	CHECK_BYTES(got, key, 32U);

	/* Every byte of c changed in turn (a different bit each time): the key is J(z || c'). */
	for (i = 0U; i < CRYC_MLKEM768_CT; i++) {
		c[i] ^= (uint8_t)(1U << (i & 7U));
		cryc_mlkem768_decaps(got, c, dk);
		reference_j(expected, seed + 32, c);
		CHECK_BYTES(got, expected, 32U);
		CHECK(memcmp(got, key, 32U) != 0);
		c[i] ^= (uint8_t)(1U << (i & 7U));
	}

	/* Every bit of the first, the last of c1 and the last byte flipped in turn. */
	for (bit = 0U; bit < 8U; bit++) {
		c[0] ^= (uint8_t)(1U << bit);
		cryc_mlkem768_decaps(got, c, dk);
		reference_j(expected, seed + 32, c);
		CHECK_BYTES(got, expected, 32U);
		c[0] ^= (uint8_t)(1U << bit);

		/* The same for the last byte of the ciphertext. */
		c[1087] ^= (uint8_t)(1U << bit);
		cryc_mlkem768_decaps(got, c, dk);
		reference_j(expected, seed + 32, c);
		CHECK_BYTES(got, expected, 32U);
		c[1087] ^= (uint8_t)(1U << bit);
	}

	/* The all-zero and all-0xff ciphertexts (the latter has values above q in c1 once decoded). */
	memset(c, 0x00, CRYC_MLKEM768_CT);
	cryc_mlkem768_decaps(got, c, dk);
	reference_j(expected, seed + 32, c);
	CHECK_BYTES(got, expected, 32U);
	memset(c, 0xff, CRYC_MLKEM768_CT);
	cryc_mlkem768_decaps(got, c, dk);
	reference_j(expected, seed + 32, c);
	CHECK_BYTES(got, expected, 32U);

	/* Random ciphertexts. */
	for (rounds = 0U; rounds < 30U; rounds++) {
		random_bytes(c, CRYC_MLKEM768_CT);
		cryc_mlkem768_decaps(got, c, dk);
		reference_j(expected, seed + 32, c);
		CHECK_BYTES(got, expected, 32U);
	}

	/* Releases the buffers. */
	free(ek);
	free(dk);
	free(c);
	free(key);
	free(got);
}

/* Checks how a corrupted dk behaves: z matters only for a bad ciphertext, a changed secret part makes the re-encryption differ. */
static void
test_corrupt_dk(void)
{
	uint8_t seed[64];
	uint8_t m[32];
	uint8_t z2[32];
	uint8_t *ek;
	uint8_t *dk;
	uint8_t *c;
	uint8_t *key;
	uint8_t *got;
	uint8_t expected[32];
	unsigned i;

	/* Allocates the key buffers at their exact sizes, so an access past them is an error. */
	ek = exact(CRYC_MLKEM768_EK);
	dk = exact(CRYC_MLKEM768_DK);
	c = exact(CRYC_MLKEM768_CT);
	key = exact(CRYC_MLKEM_SHARED);
	got = exact(CRYC_MLKEM_SHARED);
	random_bytes(seed, sizeof(seed));
	random_bytes(m, sizeof(m));
	cryc_mlkem768_keygen_internal(ek, dk, seed, seed + 32);
	cryc_mlkem768_encaps_internal(key, c, ek, m);

	/* A different z: the valid ciphertext still gives K, a bad one gives the key of the new z. */
	random_bytes(z2, sizeof(z2));
	memcpy(dk + 2368, z2, 32U);
	cryc_mlkem768_decaps(got, c, dk);
	CHECK_BYTES(got, key, 32U);
	c[7] ^= 0x10U;
	cryc_mlkem768_decaps(got, c, dk);
	reference_j(expected, z2, c);
	CHECK_BYTES(got, expected, 32U);
	c[7] ^= 0x10U;
	memcpy(dk + 2368, seed + 32, 32U);

	/* A big change in each of the three secret polynomials (the top byte of the first coefficients): the check still passes, the decryption no longer matches, the answer is the rejection key. */
	for (i = 0U; i < 3U; i++) {
		dk[384U * i + 1U] ^= 0xf0U;
		CHECK(cryc_mlkem768_check_dk(dk) == 0);
		cryc_mlkem768_decaps(got, c, dk);
		reference_j(expected, seed + 32, c);
		CHECK_BYTES(got, expected, 32U);
		dk[384U * i + 1U] ^= 0xf0U;
	}

	/* With every byte restored, the key decapsulates to the original. */
	cryc_mlkem768_decaps(got, c, dk);
	CHECK_BYTES(got, key, 32U);

	/* Releases the buffers. */
	free(ek);
	free(dk);
	free(c);
	free(key);
	free(got);
}

/* Checks round trips with random keys and messages through every entry point, and that the shared key depends on m and on the key. */
static void
test_round_trip(void)
{
	struct fixed_random source;
	uint8_t seed[64];
	uint8_t m[32];
	uint8_t other_m[32];
	uint8_t *ek;
	uint8_t *dk;
	uint8_t *c;
	uint8_t *c_again;
	uint8_t *key;
	uint8_t *key_again;
	uint8_t *got;
	unsigned round;
	int error;

	/* Allocates the key buffers at their exact sizes, so an access past them is an error. */
	ek = exact(CRYC_MLKEM768_EK);
	dk = exact(CRYC_MLKEM768_DK);
	c = exact(CRYC_MLKEM768_CT);
	c_again = exact(CRYC_MLKEM768_CT);
	key = exact(CRYC_MLKEM_SHARED);
	key_again = exact(CRYC_MLKEM_SHARED);
	got = exact(CRYC_MLKEM_SHARED);

	/* Many random key pairs and messages. */
	for (round = 0U; round < 40U; round++) {
		random_bytes(seed, sizeof(seed));
		random_bytes(m, sizeof(m));
		cryc_mlkem768_keygen_internal(ek, dk, seed, seed + 32);

		/* Encaps_internal then Decaps recovers the key. */
		cryc_mlkem768_encaps_internal(key, c, ek, m);
		cryc_mlkem768_decaps(got, c, dk);
		CHECK_BYTES(got, key, 32U);

		/* The same inputs give the same output (the functions are deterministic). */
		cryc_mlkem768_encaps_internal(key_again, c_again, ek, m);
		CHECK_BYTES(c_again, c, 1088U);
		CHECK_BYTES(key_again, key, 32U);

		/* Another message gives another ciphertext and key. */
		random_bytes(other_m, sizeof(other_m));
		cryc_mlkem768_encaps_internal(key_again, c_again, ek, other_m);
		CHECK(memcmp(c_again, c, 1088U) != 0);
		CHECK(memcmp(key_again, key, 32U) != 0);

		/* Encaps with a random source gives a pair that Decaps opens. */
		memset(&source, 0, sizeof(source));
		source.data = m;
		source.size = sizeof(m);
		error = cryc_mlkem768_encaps(key_again, c_again, ek, fixed_random_fn, &source);
		CHECK(error == 0);
		CHECK_BYTES(c_again, c, 1088U);
		cryc_mlkem768_decaps(got, c_again, dk);
		CHECK_BYTES(got, key_again, 32U);
	}

	/* Special messages: all zero and all 0xff. */
	memset(m, 0x00, sizeof(m));
	cryc_mlkem768_encaps_internal(key, c, ek, m);
	cryc_mlkem768_decaps(got, c, dk);
	CHECK_BYTES(got, key, 32U);
	memset(m, 0xff, sizeof(m));
	cryc_mlkem768_encaps_internal(key, c, ek, m);
	cryc_mlkem768_decaps(got, c, dk);
	CHECK_BYTES(got, key, 32U);

	/* A ciphertext made for one key decapsulated with another gives the rejection key of the second. */
	random_bytes(seed, sizeof(seed));
	cryc_mlkem768_keygen_internal(ek, dk, seed, seed + 32);
	cryc_mlkem768_decaps(got, c, dk);
	reference_j(key_again, seed + 32, c);
	CHECK_BYTES(got, key_again, 32U);

	/* Releases the buffers. */
	free(ek);
	free(dk);
	free(c);
	free(c_again);
	free(key);
	free(key_again);
	free(got);
}

/* Runs the checks. */
int
main(void)
{
	/* The constants and the vectors. */
	test_constants();
	test_vectors_keygen();
	test_vectors_encaps();
	test_vectors_decaps();
	test_vectors_semi_expanded();

	/* The random-source functions and the key checks. */
	test_keygen_random();
	test_encaps_random_failure();
	test_check_ek();
	test_check_dk();

	/* Decapsulation: implicit rejection, corrupted keys and round trips. */
	test_implicit_rejection();
	test_corrupt_dk();
	test_round_trip();

	/* The result. */
	return check_finish("test-mlkem");
}
