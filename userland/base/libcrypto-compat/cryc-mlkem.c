/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * K-PKE and ML-KEM-768 (FIPS 203 5 to 7).
 *
 * The section and algorithm numbers in the comments of this file are those
 * of FIPS 203 (August 2024).  The matrix A is never stored: each entry is
 * sampled from rho at the moment the product that uses it is formed, which
 * is how Algorithms 13 and 14 use it (every entry once).  Secrets (the
 * seeds, the noise polynomials, the message, the derived keys) are wiped
 * from the local arrays before every return.
 */

#include <string.h>

#include "cryc-mlkem.h"

/* Hides a mask from the optimizer. */
static uint32_t mlkem_opaque(uint32_t value);

/* The module dimension k of ML-KEM-768 (Table 2). */
#define MLKEM_K		3U

/* The noise parameter eta of both eta1 and eta2 of ML-KEM-768 (Table 2). */
#define MLKEM_ETA	2U

/* The compression widths d_u and d_v of ML-KEM-768 (Table 2). */
#define MLKEM_DU	10U
#define MLKEM_DV	4U

/* The size in bytes of one ByteEncode12 polynomial (32 * 12). */
#define MLKEM_POLY_BYTES	384U

/* The size in bytes of the encryption key of K-PKE (384 k + 32). */
#define MLKEM_EK_PKE_BYTES	1184U

/* The size in bytes of the part of the ciphertext made of u (32 d_u k). */
#define MLKEM_C1_BYTES		960U

/* The offsets inside the decapsulation key dk = dkPKE || ek || H(ek) || z (Algorithm 16 line 3). */
#define MLKEM_DK_EK_OFFSET	1152U
#define MLKEM_DK_HASH_OFFSET	2336U
#define MLKEM_DK_Z_OFFSET	2368U

static void mlkem_hash_h(uint8_t *out, const uint8_t *data, size_t size);
static void mlkem_hash_g(uint8_t *out, const uint8_t *first, size_t first_size, const uint8_t *second, size_t second_size);
static void mlkem_hash_j(uint8_t *out, const uint8_t *z, const uint8_t *ciphertext);
static void mlkem_noise(struct cryc_mlkem_poly *f, const uint8_t *seed, uint8_t nonce);
static void mlkem_poly_add(struct cryc_mlkem_poly *h, const struct cryc_mlkem_poly *f, const struct cryc_mlkem_poly *g);
static void mlkem_poly_sub(struct cryc_mlkem_poly *h, const struct cryc_mlkem_poly *f, const struct cryc_mlkem_poly *g);
static void mlkem_matrix_entry(struct cryc_mlkem_poly *f, const uint8_t *rho, unsigned byte_33, unsigned byte_34);
static void mlkem_kpke_encrypt(uint8_t *ciphertext, const uint8_t *ek, const uint8_t *m, const uint8_t *r);
static void mlkem_kpke_decrypt(uint8_t *m, const uint8_t *dk_pke, const uint8_t *ciphertext);

/*
 * ML-KEM.KeyGen_internal (Algorithm 16) from the seeds d and z (32 bytes
 * each): writes ek (1184 bytes) and dk (2400 bytes).
 *
 * The key generation of K-PKE (Algorithm 13) is written inline: ek is
 * the K-PKE encryption key and the first part of dk is its decryption
 * key.
 */
void
cryc_mlkem768_keygen_internal(
	uint8_t *ek,
	uint8_t *dk,
	const uint8_t *d,
	const uint8_t *z)
{
	struct cryc_mlkem_poly s[MLKEM_K];
	struct cryc_mlkem_poly e;
	struct cryc_mlkem_poly t;
	struct cryc_mlkem_poly entry;
	struct cryc_mlkem_poly product;
	uint8_t input[33];
	uint8_t expanded[64];
	unsigned i;
	unsigned j;
	uint8_t nonce;

	/* Expands d and the dimension byte k into rho (expanded[0..31]) and sigma (expanded[32..63]), Algorithm 13 line 1. */
	memcpy(input, d, 32U);
	input[32] = (uint8_t)MLKEM_K;
	mlkem_hash_g(expanded, input, sizeof(input), NULL, 0U);

	/* Samples the secret vector s from sigma and transforms it, lines 8 to 11 and 16. */
	nonce = 0;
	for (i = 0; i < MLKEM_K; i++) {
		mlkem_noise(&s[i], expanded + 32, nonce);
		nonce++;
		cryc_mlkem_ntt(&s[i]);
	}

	/*
	 * Forms each t[i] = sum over j of A[i][j] * s[j] + e[i], lines 12 to
	 * 15, 17 and 18, with the error polynomial sampled just before it is
	 * used (the nonces K .. 2K-1 follow those of s), and writes
	 * ByteEncode12(t[i]) into ek and ByteEncode12(s[i]) into dk, lines 19
	 * and 20.
	 */
	for (i = 0; i < MLKEM_K; i++) {
		mlkem_noise(&e, expanded + 32, nonce);
		nonce++;
		cryc_mlkem_ntt(&e);
		t = e;
		for (j = 0; j < MLKEM_K; j++) {
			mlkem_matrix_entry(&entry, expanded, j, i);
			cryc_mlkem_multiply_ntts(&product, &entry, &s[j]);
			mlkem_poly_add(&t, &t, &product);
		}
		cryc_mlkem_byte_encode(ek + MLKEM_POLY_BYTES * i, &t, 12U);
		cryc_mlkem_byte_encode(dk + MLKEM_POLY_BYTES * i, &s[i], 12U);
	}

	/* Appends the matrix seed rho to ek, line 19. */
	memcpy(ek + MLKEM_POLY_BYTES * MLKEM_K, expanded, 32U);

	/* Completes dk = dkPKE || ek || H(ek) || z, Algorithm 16 lines 2 and 3. */
	memcpy(dk + MLKEM_DK_EK_OFFSET, ek, MLKEM_EK_PKE_BYTES);
	mlkem_hash_h(dk + MLKEM_DK_HASH_OFFSET, ek, MLKEM_EK_PKE_BYTES);
	memcpy(dk + MLKEM_DK_Z_OFFSET, z, 32U);

	/* Wipes the secrets this function held. */
	cryc_wipe(s, sizeof(s));
	cryc_wipe(&e, sizeof(e));
	cryc_wipe(&t, sizeof(t));
	cryc_wipe(&product, sizeof(product));
	cryc_wipe(input, sizeof(input));
	cryc_wipe(expanded, sizeof(expanded));
}

/*
 * ML-KEM.KeyGen (Algorithm 19): d and z from random, then KeyGen_internal.
 *
 * Returns 0, or EIO when random failed (the outputs are wiped).
 */
int
cryc_mlkem768_keygen(
	uint8_t *ek,
	uint8_t *dk,
	cryc_random_fn random,
	void *random_context)
{
	uint8_t d[32];
	uint8_t z[32];
	int error;

	/* Draws the seed d, Algorithm 19 line 1. */
	error = random(random_context, d, sizeof(d));
	if (error != 0) {
		cryc_wipe(d, sizeof(d));
		cryc_wipe(ek, CRYC_MLKEM768_EK);
		cryc_wipe(dk, CRYC_MLKEM768_DK);
		return EIO;
	}

	/* Draws the rejection value z, line 2. */
	error = random(random_context, z, sizeof(z));
	if (error != 0) {
		cryc_wipe(d, sizeof(d));
		cryc_wipe(z, sizeof(z));
		cryc_wipe(ek, CRYC_MLKEM768_EK);
		cryc_wipe(dk, CRYC_MLKEM768_DK);
		return EIO;
	}

	/* Runs the deterministic key generation, line 6. */
	cryc_mlkem768_keygen_internal(ek, dk, d, z);

	/* Wipes the seeds. */
	cryc_wipe(d, sizeof(d));
	cryc_wipe(z, sizeof(z));

	/* Succeeded: ek and dk hold a new key pair. */
	return 0;
}

/*
 * Checks an encapsulation key (7.2 "modulus check": every 12-bit value
 * below q).
 *
 * ek is public, so the check may stop at the first value out of range.
 */
int
cryc_mlkem768_check_ek(
	const uint8_t *ek)
{
	struct cryc_mlkem_poly f;
	unsigned i;
	int clean;

	/* Decodes each of the k polynomials and refuses a value of q or more, which ByteDecode12 reports as not clean. */
	for (i = 0; i < MLKEM_K; i++) {
		clean = cryc_mlkem_byte_decode(&f, ek + MLKEM_POLY_BYTES * i, 12U);
		if (clean != 1)
			return EINVAL;
	}

	/* Succeeded: the encapsulation key passed the modulus check. */
	return 0;
}

/*
 * Checks a decapsulation key (7.3 "hash check": H(ek part) equals the
 * stored hash).
 *
 * The outcome is computed with a mask, so the time does not depend on dk
 * and no branch is taken on the comparison.
 */
int
cryc_mlkem768_check_dk(
	const uint8_t *dk)
{
	uint8_t hash[32];
	uint32_t mask;
	int equal;
	int error;

	/* Hashes the embedded encapsulation key, equation (7.2). */
	mlkem_hash_h(hash, dk + MLKEM_DK_EK_OFFSET, MLKEM_EK_PKE_BYTES);

	/* Compares with the stored hash. */
	equal = cryc_ct_equal(hash, dk + MLKEM_DK_HASH_OFFSET, sizeof(hash));
	mask = cryc_ct_mask_eq32((uint32_t)equal, 1U);

	/* EINVAL where the hashes differ, 0 where they are equal, without a branch. */
	error = (int)((uint32_t)EINVAL & ~mask);

	/* Wipes the recomputed hash. */
	cryc_wipe(hash, sizeof(hash));

	/* Reports the outcome of the hash check: 0 or EINVAL. */
	return error;
}

/*
 * ML-KEM.Encaps_internal (Algorithm 17) with the message m (32 bytes):
 * writes the shared key (32 bytes) and the ciphertext (1088 bytes).
 */
void
cryc_mlkem768_encaps_internal(
	uint8_t *shared,
	uint8_t *ciphertext,
	const uint8_t *ek,
	const uint8_t *m)
{
	uint8_t hash[32];
	uint8_t derived[64];

	/* Derives the shared key K (derived[0..31]) and the randomness r (derived[32..63]) from m || H(ek), line 1. */
	mlkem_hash_h(hash, ek, MLKEM_EK_PKE_BYTES);
	mlkem_hash_g(derived, m, 32U, hash, sizeof(hash));

	/* Encrypts m with the randomness r, line 2. */
	mlkem_kpke_encrypt(ciphertext, ek, m, derived + 32);

	/* Publishes the shared key. */
	memcpy(shared, derived, 32U);

	/* Wipes the derived values. */
	cryc_wipe(derived, sizeof(derived));
	cryc_wipe(hash, sizeof(hash));
}

/*
 * ML-KEM.Encaps (Algorithm 20): m from random, then Encaps_internal.
 *
 * Returns 0, EINVAL (ek fails the modulus check) or EIO (random failed).
 * The outputs are wiped on an error.
 */
int
cryc_mlkem768_encaps(
	uint8_t *shared,
	uint8_t *ciphertext,
	const uint8_t *ek,
	cryc_random_fn random,
	void *random_context)
{
	uint8_t m[32];
	int error;

	/* Refuses an encapsulation key that fails the modulus check (7.2). */
	error = cryc_mlkem768_check_ek(ek);
	if (error != 0) {
		cryc_wipe(shared, CRYC_MLKEM_SHARED);
		cryc_wipe(ciphertext, CRYC_MLKEM768_CT);
		return EINVAL;
	}

	/* Draws the message m, Algorithm 20 line 1. */
	error = random(random_context, m, sizeof(m));
	if (error != 0) {
		cryc_wipe(m, sizeof(m));
		cryc_wipe(shared, CRYC_MLKEM_SHARED);
		cryc_wipe(ciphertext, CRYC_MLKEM768_CT);
		return EIO;
	}

	/* Runs the deterministic encapsulation, line 5. */
	cryc_mlkem768_encaps_internal(shared, ciphertext, ek, m);

	/* Wipes the message. */
	cryc_wipe(m, sizeof(m));

	/* Succeeded: shared and ciphertext hold the new key and its encapsulation. */
	return 0;
}

/*
 * ML-KEM.Decaps (Algorithm 21 with Decaps_internal, Algorithm 18): writes
 * the shared key (32 bytes) for a 1088-byte ciphertext and a 2400-byte dk
 * that passed cryc_mlkem768_check_dk.
 *
 * The ciphertext is re-encrypted and compared with the one received; the
 * comparison and the choice between K' and the rejection key are made
 * with masks, so neither the time nor any branch depends on whether the
 * ciphertext was valid.
 */
void
cryc_mlkem768_decaps(
	uint8_t *shared,
	const uint8_t *ciphertext,
	const uint8_t *dk)
{
	uint8_t m_prime[32];
	uint8_t derived[64];
	uint8_t rejection[32];
	uint8_t again[CRYC_MLKEM768_CT];
	uint32_t mask;
	int equal;

	/* Decrypts the ciphertext with the decryption key of K-PKE, Algorithm 18 line 5. */
	mlkem_kpke_decrypt(m_prime, dk, ciphertext);

	/* Derives the candidate key K' (derived[0..31]) and the randomness r' (derived[32..63]) from m' || h, line 6. */
	mlkem_hash_g(derived, m_prime, sizeof(m_prime), dk + MLKEM_DK_HASH_OFFSET, 32U);

	/* Derives the rejection key from z || c, line 7. */
	mlkem_hash_j(rejection, dk + MLKEM_DK_Z_OFFSET, ciphertext);

	/* Re-encrypts m' under the embedded encapsulation key with the derived randomness, line 8. */
	mlkem_kpke_encrypt(again, dk + MLKEM_DK_EK_OFFSET, m_prime, derived + 32);

	/* Compares the received ciphertext with the re-encrypted one in constant time, line 9. */
	equal = cryc_ct_equal(ciphertext, again, sizeof(again));
	mask = cryc_ct_mask_eq32((uint32_t)equal, 1U);

	/* Selects K' when the ciphertexts match and the rejection key otherwise, lines 9 to 12. */
	cryc_ct_select_bytes(shared, derived, rejection, CRYC_MLKEM_SHARED, mask);

	/* Wipes everything derived from the secret. */
	cryc_wipe(m_prime, sizeof(m_prime));
	cryc_wipe(derived, sizeof(derived));
	cryc_wipe(rejection, sizeof(rejection));
	cryc_wipe(again, sizeof(again));
	cryc_wipe(&mask, sizeof(mask));
	cryc_wipe(&equal, sizeof(equal));
}

/* H (4.1): SHA3-256 of data, 32 bytes. */
static void
mlkem_hash_h(
	uint8_t *out,
	const uint8_t *data,
	size_t size)
{
	struct cryc_sha3 sponge;

	/* Hashes the input in one pass; the final call wipes the sponge. */
	cryc_sha3_256_init(&sponge);
	cryc_sha3_absorb(&sponge, data, size);
	cryc_sha3_final(&sponge, out);
}

/* G (4.1): SHA3-512 of first || second (second may be empty), 64 bytes. */
static void
mlkem_hash_g(
	uint8_t *out,
	const uint8_t *first,
	size_t first_size,
	const uint8_t *second,
	size_t second_size)
{
	struct cryc_sha3 sponge;

	/* Hashes the concatenation; the final call wipes the sponge. */
	cryc_sha3_512_init(&sponge);
	cryc_sha3_absorb(&sponge, first, first_size);
	cryc_sha3_absorb(&sponge, second, second_size);
	cryc_sha3_final(&sponge, out);
}

/* J (4.1): the first 32 bytes of SHAKE256 of z || c, with c of 1088 bytes. */
static void
mlkem_hash_j(
	uint8_t *out,
	const uint8_t *z,
	const uint8_t *ciphertext)
{
	struct cryc_sha3 sponge;

	/* Absorbs z and the ciphertext and squeezes 32 bytes. */
	cryc_shake256_init(&sponge);
	cryc_sha3_absorb(&sponge, z, 32U);
	cryc_sha3_absorb(&sponge, ciphertext, CRYC_MLKEM768_CT);
	cryc_shake_squeeze(&sponge, out, 32U);

	/* Wipes the sponge, which absorbed a secret. */
	cryc_sha3_clear(&sponge);
}

/*
 * Samples a noise polynomial: SamplePolyCBD_eta(PRF_eta(seed, nonce)),
 * where PRF_eta(s, b) is the first 64 * eta bytes of SHAKE256(s || b)
 * (4.1, equation (4.3)).
 */
static void
mlkem_noise(
	struct cryc_mlkem_poly *f,
	const uint8_t *seed,
	uint8_t nonce)
{
	struct cryc_sha3 sponge;
	uint8_t stream[64U * MLKEM_ETA];

	/* Absorbs the 32-byte seed and the one-byte counter and squeezes 64 eta bytes. */
	cryc_shake256_init(&sponge);
	cryc_sha3_absorb(&sponge, seed, 32U);
	cryc_sha3_absorb(&sponge, &nonce, 1U);
	cryc_shake_squeeze(&sponge, stream, sizeof(stream));

	/* Turns the bytes into coefficients in the centered binomial distribution. */
	cryc_mlkem_sample_cbd(f, stream, MLKEM_ETA);

	/* Wipes the sponge and the stream. */
	cryc_sha3_clear(&sponge);
	cryc_wipe(stream, sizeof(stream));
}

/*
 * h = f + g coefficient by coefficient modulo q, for coefficients already
 * in [0, q - 1]; h may alias f or g.  A masked subtraction replaces the
 * comparison.
 */
static void
mlkem_poly_add(
	struct cryc_mlkem_poly *h,
	const struct cryc_mlkem_poly *f,
	const struct cryc_mlkem_poly *g)
{
	unsigned i;
	uint32_t sum;
	uint32_t borrow;

	/* Adds, subtracts q, and adds q back where the subtraction wrapped. */
	for (i = 0; i < CRYC_MLKEM_N; i++) {
		sum = (uint32_t)(uint16_t)f->c[i] + (uint32_t)(uint16_t)g->c[i];
		sum -= (uint32_t)CRYC_MLKEM_Q;
		borrow = mlkem_opaque(0U - (sum >> 31));
		sum += borrow & (uint32_t)CRYC_MLKEM_Q;
		h->c[i] = (int16_t)sum;
	}
}

/*
 * h = f - g coefficient by coefficient modulo q, for coefficients already
 * in [0, q - 1]; h may alias f or g.
 */
static void
mlkem_poly_sub(
	struct cryc_mlkem_poly *h,
	const struct cryc_mlkem_poly *f,
	const struct cryc_mlkem_poly *g)
{
	unsigned i;
	uint32_t difference;
	uint32_t borrow;

	/* Forms f - g + q (in [1, 2q - 1]), subtracts q, and adds q back where that wrapped. */
	for (i = 0; i < CRYC_MLKEM_N; i++) {
		difference = (uint32_t)(uint16_t)f->c[i] + (uint32_t)CRYC_MLKEM_Q - (uint32_t)(uint16_t)g->c[i];
		difference -= (uint32_t)CRYC_MLKEM_Q;
		borrow = mlkem_opaque(0U - (difference >> 31));
		difference += borrow & (uint32_t)CRYC_MLKEM_Q;
		h->c[i] = (int16_t)difference;
	}
}

/*
 * Samples the matrix entry SampleNTT(rho || byte_33 || byte_34), where
 * byte_33 and byte_34 are the bytes 33 and 34 of the input of Algorithm 7
 * (A[i][j] uses j and then i).
 */
static void
mlkem_matrix_entry(
	struct cryc_mlkem_poly *f,
	const uint8_t *rho,
	unsigned byte_33,
	unsigned byte_34)
{
	uint8_t seed[34];

	/* Builds the 34-byte input and samples the polynomial. */
	memcpy(seed, rho, 32U);
	seed[32] = (uint8_t)byte_33;
	seed[33] = (uint8_t)byte_34;
	cryc_mlkem_sample_ntt(f, seed);
}

/*
 * K-PKE.Encrypt (Algorithm 14): writes the 1088-byte ciphertext of the
 * 32-byte message m under the key ek with the randomness r.
 *
 * ek is the 1184-byte K-PKE encryption key (the encapsulation key); its
 * modulus check is the caller's.  The vector y is kept as polynomials;
 * A^T y is formed one entry at a time.
 */
static void
mlkem_kpke_encrypt(
	uint8_t *ciphertext,
	const uint8_t *ek,
	const uint8_t *m,
	const uint8_t *r)
{
	struct cryc_mlkem_poly y[MLKEM_K];
	struct cryc_mlkem_poly noise;
	struct cryc_mlkem_poly sum;
	struct cryc_mlkem_poly entry;
	struct cryc_mlkem_poly product;
	struct cryc_mlkem_poly mu;
	const uint8_t *rho;
	unsigned i;
	unsigned j;
	uint8_t nonce;

	/* The matrix seed rho is the last 32 bytes of ek, line 3. */
	rho = ek + MLKEM_POLY_BYTES * MLKEM_K;

	/* Samples y from r (nonces 0 .. K-1) and transforms it, lines 9 to 12 and 18. */
	nonce = 0;
	for (i = 0; i < MLKEM_K; i++) {
		mlkem_noise(&y[i], r, nonce);
		nonce++;
		cryc_mlkem_ntt(&y[i]);
	}

	/*
	 * Forms u[i] = NTT^-1(sum over j of A[j][i] * y[j]) + e1[i], lines 13
	 * to 16 and 19, compresses it and writes c1, line 22.  The nonce of
	 * e1[i] is K + i.
	 */
	for (i = 0; i < MLKEM_K; i++) {
		memset(&sum, 0, sizeof(sum));
		for (j = 0; j < MLKEM_K; j++) {
			mlkem_matrix_entry(&entry, rho, i, j);
			cryc_mlkem_multiply_ntts(&product, &entry, &y[j]);
			mlkem_poly_add(&sum, &sum, &product);
		}
		cryc_mlkem_ntt_inverse(&sum);
		mlkem_noise(&noise, r, nonce);
		nonce++;
		mlkem_poly_add(&sum, &sum, &noise);
		cryc_mlkem_compress(&sum, MLKEM_DU);
		cryc_mlkem_byte_encode(ciphertext + 32U * MLKEM_DU * i, &sum, MLKEM_DU);
	}

	/* Samples e2 with the next nonce, line 17. */
	mlkem_noise(&noise, r, nonce);

	/* Decodes the message into mu = Decompress_1(ByteDecode_1(m)), line 20. */
	cryc_mlkem_byte_decode(&mu, m, 1U);
	cryc_mlkem_decompress(&mu, 1U);

	/* Forms t^T y in the NTT domain with t decoded from ek, lines 2 and 21. */
	memset(&sum, 0, sizeof(sum));
	for (i = 0; i < MLKEM_K; i++) {
		cryc_mlkem_byte_decode(&entry, ek + MLKEM_POLY_BYTES * i, 12U);
		cryc_mlkem_multiply_ntts(&product, &entry, &y[i]);
		mlkem_poly_add(&sum, &sum, &product);
	}

	/* v = NTT^-1(t^T y) + e2 + mu, line 21. */
	cryc_mlkem_ntt_inverse(&sum);
	mlkem_poly_add(&sum, &sum, &noise);
	mlkem_poly_add(&sum, &sum, &mu);

	/* Compresses v and writes c2, line 23. */
	cryc_mlkem_compress(&sum, MLKEM_DV);
	cryc_mlkem_byte_encode(ciphertext + MLKEM_C1_BYTES, &sum, MLKEM_DV);

	/* Wipes the secret polynomials. */
	cryc_wipe(y, sizeof(y));
	cryc_wipe(&noise, sizeof(noise));
	cryc_wipe(&sum, sizeof(sum));
	cryc_wipe(&product, sizeof(product));
	cryc_wipe(&mu, sizeof(mu));
}

/*
 * K-PKE.Decrypt (Algorithm 15): writes the 32-byte message of a
 * 1088-byte ciphertext under the 1152-byte decryption key dk_pke.
 */
static void
mlkem_kpke_decrypt(
	uint8_t *m,
	const uint8_t *dk_pke,
	const uint8_t *ciphertext)
{
	struct cryc_mlkem_poly u;
	struct cryc_mlkem_poly s;
	struct cryc_mlkem_poly product;
	struct cryc_mlkem_poly sum;
	struct cryc_mlkem_poly v;
	unsigned i;

	/*
	 * Forms s^T NTT(u') in the NTT domain, one term per coordinate: u'[i]
	 * is Decompress_du(ByteDecode_du(c1 part i)), and s[i] is
	 * ByteDecode12 of the key, lines 3 and 5.
	 */
	memset(&sum, 0, sizeof(sum));
	for (i = 0; i < MLKEM_K; i++) {
		cryc_mlkem_byte_decode(&u, ciphertext + 32U * MLKEM_DU * i, MLKEM_DU);
		cryc_mlkem_decompress(&u, MLKEM_DU);
		cryc_mlkem_ntt(&u);
		cryc_mlkem_byte_decode(&s, dk_pke + MLKEM_POLY_BYTES * i, 12U);
		cryc_mlkem_multiply_ntts(&product, &s, &u);
		mlkem_poly_add(&sum, &sum, &product);
	}
	cryc_mlkem_ntt_inverse(&sum);

	/* Decodes v' = Decompress_dv(ByteDecode_dv(c2)), line 4. */
	cryc_mlkem_byte_decode(&v, ciphertext + MLKEM_C1_BYTES, MLKEM_DV);
	cryc_mlkem_decompress(&v, MLKEM_DV);

	/* w = v' - NTT^-1(s^T NTT(u')), line 6. */
	mlkem_poly_sub(&v, &v, &sum);

	/* m = ByteEncode_1(Compress_1(w)), line 7. */
	cryc_mlkem_compress(&v, 1U);
	cryc_mlkem_byte_encode(m, &v, 1U);

	/* Wipes the secret polynomials. */
	cryc_wipe(&u, sizeof(u));
	cryc_wipe(&s, sizeof(s));
	cryc_wipe(&product, sizeof(product));
	cryc_wipe(&sum, sizeof(sum));
	cryc_wipe(&v, sizeof(v));
}

/*
 * Returns value through a volatile object, so that the compiler cannot
 * know which of the values of a mask (0 or 0xffffffff) it holds and turn
 * a selection by it into a branch or a choice of load.
 */
static uint32_t
mlkem_opaque(
	uint32_t value)
{
	volatile uint32_t hidden;

	/* Stored and loaded again. */
	hidden = value;
	return hidden;
}
