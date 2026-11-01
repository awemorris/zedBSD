/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * RSA signature verification (RFC 8017).
 */

#include "cryc-rsa.h"

#include <string.h>

/* The largest modulus in bytes (CRYC_BN_MAX_BITS bits). */
#define RSA_MAX_BYTES	(CRYC_BN_MAX_BITS / 8U)

/* The length in bytes of the DER bytes of a DigestInfo that come before the digest. */
#define RSA_PREFIX_SIZE	19U

/*
 * The DER encoding of a DigestInfo for each supported hash, up to the
 * digest itself: SEQUENCE, SEQUENCE { OBJECT IDENTIFIER, NULL }, OCTET
 * STRING header.  The digest follows.
 *
 * Values referenced: RFC 8017 (November 2016) 9.2, Notes, item 1: the DER
 * encodings T of the DigestInfo for SHA-256, SHA-384 and SHA-512 (each
 * prefix is the 19 bytes before "|| H").
 */
/* The DigestInfo prefix of SHA-256. */
static const uint8_t rsa_prefix_sha256[RSA_PREFIX_SIZE] = {
	0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
	0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};
/* The DigestInfo prefix of SHA-384. */
static const uint8_t rsa_prefix_sha384[RSA_PREFIX_SIZE] = {
	0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
	0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30
};
/* The DigestInfo prefix of SHA-512. */
static const uint8_t rsa_prefix_sha512[RSA_PREFIX_SIZE] = {
	0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
	0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40
};

/* Forward declarations. */
static int rsa_public_operation(const struct cryc_rsa_key *key, enum cryc_hash_id hash_id, const uint8_t *hash, size_t hash_size, const uint8_t *signature, size_t signature_size, uint8_t *encoded, size_t *modulus_size, size_t *modulus_bits);
static const uint8_t *rsa_digest_prefix(enum cryc_hash_id hash_id);
static void rsa_mgf1(enum cryc_hash_id hash_id, const uint8_t *seed, size_t seed_size, uint8_t *mask, size_t mask_size);

/*
 * Verifies an RSASSA-PKCS1-v1_5 signature (8.2.2) with hash hash_id over a
 * message whose hash is given.
 *
 * The expected encoded message is built from the hash and compared whole
 * with the one the signature gives; the decoded block is never parsed.
 */
int
cryc_rsa_pkcs1_verify(
	const struct cryc_rsa_key *key,
	enum cryc_hash_id hash_id,
	const uint8_t *hash,
	size_t hash_size,
	const uint8_t *signature,
	size_t signature_size)
{
	uint8_t encoded[RSA_MAX_BYTES];
	uint8_t expected[RSA_MAX_BYTES];
	const uint8_t *prefix;
	size_t modulus_size;
	size_t padding_size;
	size_t offset;
	int error;

	/* Steps 1 and 2: checks the arguments and applies the public key to the signature (EM = I2OSP (m, k)). */
	error = rsa_public_operation(key, hash_id, hash, hash_size, signature, signature_size, encoded, &modulus_size, NULL);
	if (error != 0)
		return error;

	/* The DigestInfo prefix of the hash (the hash is already known to be a supported one). */
	prefix = rsa_digest_prefix(hash_id);
	if (prefix == NULL)
		return ENOTSUP;

	/* 9.2 step 4: PS fills the block between the block type and the DigestInfo (at least 8 bytes, as k is at least 256). */
	padding_size = modulus_size - RSA_PREFIX_SIZE - hash_size - 3U;

	/* 9.2 step 5: EM' = 0x00 || 0x01 || PS || 0x00 || T. */
	expected[0] = 0x00U;
	expected[1] = 0x01U;
	memset(expected + 2U, 0xff, padding_size);
	offset = 2U + padding_size;
	expected[offset] = 0x00U;
	offset++;
	memcpy(expected + offset, prefix, RSA_PREFIX_SIZE);
	offset += RSA_PREFIX_SIZE;
	memcpy(expected + offset, hash, hash_size);

	/* 8.2.2 step 4: the signature is valid only when EM equals EM' byte for byte. */
	if (memcmp(encoded, expected, modulus_size) != 0)
		return EBADMSG;

	/* Succeeded: the signature verifies. */
	return 0;
}

/*
 * Verifies an RSASSA-PSS signature (8.1.2, EMSA-PSS-VERIFY of 9.1.2) with
 * hash hash_id, MGF1 (B.2.1) with the same hash, and a salt of salt_size
 * bytes.
 */
int
cryc_rsa_pss_verify(
	const struct cryc_rsa_key *key,
	enum cryc_hash_id hash_id,
	const uint8_t *hash,
	size_t hash_size,
	size_t salt_size,
	const uint8_t *signature,
	size_t signature_size)
{
	uint8_t encoded[RSA_MAX_BYTES];
	uint8_t database[RSA_MAX_BYTES];
	uint8_t check[CRYC_HASH_MAX_SIZE];
	uint8_t zeros[8];
	struct cryc_hash running;
	const uint8_t *message;
	const uint8_t *masked_database;
	const uint8_t *expected_hash;
	size_t modulus_size;
	size_t modulus_bits;
	size_t message_bits;
	size_t message_size;
	size_t database_size;
	size_t padding_size;
	size_t unused_bits;
	size_t index;
	uint32_t top_mask;
	uint32_t keep_mask;
	int error;

	/* Steps 1, 2a and 2b: checks the arguments and applies the public key (RSAVP1). */
	error = rsa_public_operation(key, hash_id, hash, hash_size, signature, signature_size, encoded, &modulus_size, &modulus_bits);
	if (error != 0)
		return error;

	/* Step 2c: emBits = modBits - 1 and emLen = ceil (emBits / 8), one less than k when modBits - 1 is a multiple of 8. */
	message_bits = modulus_bits - 1U;
	message_size = (message_bits + 7U) / 8U;

	/* Step 2c: when EM is a byte shorter than the k bytes of m, m must fit in it: its first byte is zero ("integer too large" otherwise). */
	message = encoded;
	if (message_size < modulus_size) {
		if (encoded[0] != 0x00U)
			return EBADMSG;
		message = encoded + 1;
	}

	/* 9.1.2 step 3: refuses an EM too short for the hash and the salt (salt_size is bounded first so the sum cannot wrap). */
	if (salt_size > message_size)
		return EBADMSG;
	if (message_size < hash_size + salt_size + 2U)
		return EBADMSG;

	/* 9.1.2 step 4: the last byte of EM is 0xbc. */
	if (message[message_size - 1U] != 0xbcU)
		return EBADMSG;

	/* 9.1.2 step 5: maskedDB is the leftmost emLen - hLen - 1 bytes and H the next hLen bytes. */
	database_size = message_size - hash_size - 1U;
	masked_database = message;
	expected_hash = message + database_size;

	/* 9.1.2 step 6: the leftmost 8emLen - emBits bits of maskedDB are zero. */
	unused_bits = 8U * message_size - message_bits;
	top_mask = (0xffU << (8U - unused_bits)) & 0xffU;
	keep_mask = 0xffU >> unused_bits;
	if (((uint32_t)masked_database[0] & top_mask) != 0U)
		return EBADMSG;

	/* 9.1.2 steps 7 and 8: dbMask = MGF1 (H, emLen - hLen - 1) and DB = maskedDB xor dbMask. */
	rsa_mgf1(hash_id, expected_hash, hash_size, database, database_size);
	for (index = 0; index < database_size; index++) {
		/* One byte of the mask removed from the masked data block. */
		database[index] = (uint8_t)(database[index] ^ masked_database[index]);
	}

	/* 9.1.2 step 9: clears the leftmost 8emLen - emBits bits of DB. */
	database[0] = (uint8_t)((uint32_t)database[0] & keep_mask);

	/* 9.1.2 step 10: the leftmost emLen - hLen - sLen - 2 bytes of DB are zero ... */
	padding_size = message_size - hash_size - salt_size - 2U;
	for (index = 0; index < padding_size; index++) {
		if (database[index] != 0x00U)
			return EBADMSG;
	}

	/* 9.1.2 step 10: ... and the byte at position emLen - hLen - sLen - 1 is 0x01. */
	if (database[padding_size] != 0x01U)
		return EBADMSG;

	/* 9.1.2 steps 11 to 13: the salt is the last sLen bytes of DB; H' = Hash (0x00 * 8 || mHash || salt). */
	memset(zeros, 0, sizeof(zeros));
	error = cryc_hash_init(&running, hash_id);
	if (error != 0)
		return error;
	cryc_hash_update(&running, zeros, sizeof(zeros));
	cryc_hash_update(&running, hash, hash_size);
	cryc_hash_update(&running, database + padding_size + 1U, salt_size);
	cryc_hash_final(&running, check);

	/* 9.1.2 step 14: consistent only when H equals H'. */
	if (memcmp(check, expected_hash, hash_size) != 0)
		return EBADMSG;

	/* Succeeded: the signature verifies. */
	return 0;
}

/*
 * Checks the arguments, then applies the public key to the signature:
 * writes EM = I2OSP (s^e mod n, k) to encoded (k bytes, k at most
 * RSA_MAX_BYTES) and reports k and, unless modulus_bits is NULL, the bit
 * length of n.
 *
 * Leading zero bytes of n and e are ignored (a caller that keeps a DER
 * sign byte is still understood).  The errors are checked in this order:
 * a missing argument (EINVAL); the key (n zero or even, or e not an odd
 * value of 3 to 2^32 - 1: EINVAL; n outside the bit range: ENOTSUP); the
 * hash (unsupported: ENOTSUP; wrong size: EINVAL); the signature (wrong
 * length, or s not less than n: EBADMSG).
 */
static int
rsa_public_operation(
	const struct cryc_rsa_key *key,
	enum cryc_hash_id hash_id,
	const uint8_t *hash,
	size_t hash_size,
	const uint8_t *signature,
	size_t signature_size,
	uint8_t *encoded,
	size_t *modulus_size,
	size_t *modulus_bits)
{
	struct cryc_bn_mont mont;
	uint32_t s_value[CRYC_BN_MAX_LIMBS];
	uint32_t s_mont[CRYC_BN_MAX_LIMBS];
	uint32_t m_mont[CRYC_BN_MAX_LIMBS];
	uint32_t m_value[CRYC_BN_MAX_LIMBS];
	const uint8_t *n_bytes;
	const uint8_t *e_bytes;
	const uint8_t *prefix;
	size_t n_size;
	size_t e_size;
	size_t expected_hash_size;
	size_t top_bits;
	size_t bits;
	size_t index;
	uint32_t top_byte;
	uint32_t e_value;
	uint32_t below;
	int error;

	/* Refuses missing arguments. */
	if (key == NULL)
		return EINVAL;
	if (key->n == NULL)
		return EINVAL;
	if (key->e == NULL)
		return EINVAL;
	if (hash == NULL)
		return EINVAL;
	if (signature == NULL)
		return EINVAL;

	/* Skips the leading zero bytes of n. */
	n_bytes = key->n;
	n_size = key->n_size;
	while (n_size > 0U && n_bytes[0] == 0x00U) {
		n_bytes++;
		n_size--;
	}

	/* Skips the leading zero bytes of e. */
	e_bytes = key->e;
	e_size = key->e_size;
	while (e_size > 0U && e_bytes[0] == 0x00U) {
		e_bytes++;
		e_size--;
	}

	/* The modulus must be a non-zero odd number. */
	if (n_size == 0U)
		return EINVAL;
	if ((n_bytes[n_size - 1U] & 1U) == 0U)
		return EINVAL;

	/* The exponent must be an odd number from 3 to 2^32 - 1 (at most 4 significant bytes). */
	if (e_size == 0U)
		return EINVAL;
	if (e_size > 4U)
		return EINVAL;
	e_value = 0;

	/* Reads the exponent's bytes big-endian (there are at most four). */
	for (index = 0; index < e_size; index++) {
		e_value = (e_value << 8) | (uint32_t)e_bytes[index];
	}
	if (e_value < 3U)
		return EINVAL;
	if ((e_value & 1U) == 0U)
		return EINVAL;

	/* The size of n in bits: the bits of the top byte and 8 for every byte below it (checked before the product, so it cannot wrap). */
	if (n_size > RSA_MAX_BYTES)
		return ENOTSUP;
	top_byte = n_bytes[0];
	top_bits = 0;

	/* Counts the bits of the top byte up to its highest set bit. */
	while (top_byte != 0U) {
		top_bits++;
		top_byte >>= 1;
	}
	bits = (n_size - 1U) * 8U + top_bits;
	if (bits < CRYC_RSA_MIN_BITS)
		return ENOTSUP;
	if (bits > CRYC_BN_MAX_BITS)
		return ENOTSUP;

	/* The hash must be one of the three supported, of its own length. */
	prefix = rsa_digest_prefix(hash_id);
	if (prefix == NULL)
		return ENOTSUP;
	expected_hash_size = cryc_hash_size(hash_id);
	if (hash_size != expected_hash_size)
		return EINVAL;

	/* 8.1.2 and 8.2.2 step 1: the signature is exactly k bytes. */
	if (signature_size != n_size)
		return EBADMSG;

	/* Prepares the modulus for Montgomery arithmetic. */
	error = cryc_bn_mont_init(&mont, n_bytes, n_size);
	if (error != 0)
		return error;

	/* Step 2a: s = OS2IP (S). */
	memset(s_value, 0, sizeof(s_value));
	memset(s_mont, 0, sizeof(s_mont));
	memset(m_mont, 0, sizeof(m_mont));
	memset(m_value, 0, sizeof(m_value));
	error = cryc_bn_decode(s_value, mont.limbs, signature, signature_size);
	if (error != 0)
		return EBADMSG;

	/* 5.2.2 step 1: s must be less than n ("signature representative out of range" otherwise). */
	below = cryc_bn_lt(s_value, mont.modulus, mont.limbs);
	if (below == 0U)
		return EBADMSG;

	/* 5.2.2 step 2: m = s^e mod n, computed in Montgomery form and brought back. */
	cryc_bn_to_mont(s_mont, s_value, &mont);
	cryc_bn_mont_exp(m_mont, s_mont, e_bytes, e_size, &mont);
	cryc_bn_from_mont(m_value, m_mont, &mont);

	/* Step 2c: EM = I2OSP (m, k). */
	cryc_bn_encode(m_value, mont.limbs, encoded, n_size);

	/* Reports k and, when asked, the bit length of n. */
	*modulus_size = n_size;
	if (modulus_bits != NULL)
		*modulus_bits = bits;

	/* Succeeded. */
	return 0;
}

/*
 * Returns the DigestInfo prefix of hash_id (RSA_PREFIX_SIZE bytes), or NULL
 * for a hash that is not supported here.
 */
static const uint8_t *
rsa_digest_prefix(
	enum cryc_hash_id hash_id)
{
	/* The three hashes of the signature algorithms of certificates and TLS. */
	switch (hash_id) {
	case CRYC_HASH_SHA256:
		return rsa_prefix_sha256;
	case CRYC_HASH_SHA384:
		return rsa_prefix_sha384;
	case CRYC_HASH_SHA512:
		return rsa_prefix_sha512;
	default:
		break;
	}

	/* Not supported. */
	return NULL;
}

/*
 * Writes the first mask_size bytes of MGF1 (B.2.1) over seed with hash
 * hash_id: T = Hash (seed || C) for the 4-byte counters C = 0, 1, ...
 *
 * mask_size is at most RSA_MAX_BYTES, so the counter stays far below 2^32.
 */
static void
rsa_mgf1(
	enum cryc_hash_id hash_id,
	const uint8_t *seed,
	size_t seed_size,
	uint8_t *mask,
	size_t mask_size)
{
	struct cryc_hash running;
	uint8_t counter_bytes[4];
	uint8_t block[CRYC_HASH_MAX_SIZE];
	size_t block_size;
	size_t offset;
	size_t take;
	uint32_t counter;
	int error;

	/* The digest length decides how many counters are needed. */
	block_size = cryc_hash_size(hash_id);
	if (block_size == 0U)
		return;

	/* Step 3: appends Hash (seed || C) for each counter until mask_size bytes are made. */
	offset = 0;
	counter = 0;
	while (offset < mask_size) {
		/* Step 3A: C = I2OSP (counter, 4). */
		cryc_store_be32(counter_bytes, counter);

		/* Step 3B: the hash of the seed and C. */
		error = cryc_hash_init(&running, hash_id);
		if (error != 0)
			return;
		cryc_hash_update(&running, seed, seed_size);
		cryc_hash_update(&running, counter_bytes, sizeof(counter_bytes));
		cryc_hash_final(&running, block);

		/* Step 4: takes only what is still needed of the last block. */
		take = mask_size - offset;
		if (take > block_size)
			take = block_size;
		memcpy(mask + offset, block, take);
		offset += take;
		counter++;
	}
}
