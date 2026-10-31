/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hash chosen at run time, HMAC (RFC 2104) and HKDF (RFC 5869).
 */

#include <string.h>

#include "cryc-hash.h"

/*
 * The byte that is exclusive-ored into the padded key to make the inner
 * key (RFC 2104 section 2, ipad: 0x36 repeated).
 */
#define HMAC_INNER_PAD	0x36U

/*
 * The byte that is exclusive-ored into the padded key to make the outer
 * key (RFC 2104 section 2, opad: 0x5C repeated).
 */
#define HMAC_OUTER_PAD	0x5cU

/*
 * The most blocks HKDF-Expand may produce: the block counter is one octet
 * (RFC 5869 section 2.3, N <= 255).
 */
#define HKDF_MAX_BLOCKS	255U

/*
 * Returns the digest size in bytes of id (32, 48, 64), or 0 for
 * CRYC_HASH_NONE or an unknown value.
 */
size_t
cryc_hash_size(
	enum cryc_hash_id id)
{
	size_t size;

	/* Maps the hash to its digest size; anything else has none. */
	switch (id) {
	case CRYC_HASH_SHA256:
		size = CRYC_SHA256_SIZE;
		break;
	case CRYC_HASH_SHA384:
		size = CRYC_SHA384_SIZE;
		break;
	case CRYC_HASH_SHA512:
		size = CRYC_SHA512_SIZE;
		break;
	default:
		size = 0;
		break;
	}

	/* Reports the digest size, or 0 when there is no such hash. */
	return size;
}

/*
 * Returns the block size in bytes of id (64 or 128), or 0 for
 * CRYC_HASH_NONE or an unknown value.
 */
size_t
cryc_hash_block_size(
	enum cryc_hash_id id)
{
	size_t size;

	/* Maps the hash to its block size; anything else has none. */
	switch (id) {
	case CRYC_HASH_SHA256:
		size = CRYC_SHA256_BLOCK;
		break;
	case CRYC_HASH_SHA384:
	case CRYC_HASH_SHA512:
		size = CRYC_SHA512_BLOCK;
		break;
	default:
		size = 0;
		break;
	}

	/* Reports the block size, or 0 when there is no such hash. */
	return size;
}

/*
 * Starts a computation of id.
 *
 * Returns 0, or ENOTSUP for CRYC_HASH_NONE or an unknown id (hash->id is
 * then CRYC_HASH_NONE).
 */
int
cryc_hash_init(
	struct cryc_hash *hash,
	enum cryc_hash_id id)
{
	/*
	 * Starts from a cleared structure, so that a refused id leaves one
	 * that names no hash and holds no stale state.
	 */
	memset(hash, 0, sizeof(*hash));
	hash->id = CRYC_HASH_NONE;

	/* Starts the computation of the requested hash. */
	switch (id) {
	case CRYC_HASH_SHA256:
		cryc_sha256_init(&hash->state.sha256);
		hash->id = id;
		break;
	case CRYC_HASH_SHA384:
		cryc_sha384_init(&hash->state.sha512);
		hash->id = id;
		break;
	case CRYC_HASH_SHA512:
		cryc_sha512_init(&hash->state.sha512);
		hash->id = id;
		break;
	default:
		/* The hash is not one of those provided. */
		return ENOTSUP;
	}

	/* Succeeded: the structure holds a started computation. */
	return 0;
}

/*
 * Absorbs size bytes; data may be NULL only when size is 0.  hash was
 * started with cryc_hash_init.
 */
void
cryc_hash_update(
	struct cryc_hash *hash,
	const uint8_t *data,
	size_t size)
{
	/* Passes the bytes to the hash the structure was started with. */
	switch (hash->id) {
	case CRYC_HASH_SHA256:
		cryc_sha256_update(&hash->state.sha256, data, size);
		break;
	case CRYC_HASH_SHA384:
	case CRYC_HASH_SHA512:
		cryc_sha512_update(&hash->state.sha512, data, size);
		break;
	default:
		/* A structure that names no hash absorbs nothing. */
		break;
	}
}

/*
 * Writes the digest (cryc_hash_size(hash->id) bytes) to out and wipes
 * *hash (its id becomes CRYC_HASH_NONE).
 *
 * To read a digest and go on absorbing (a transcript), copy the structure
 * and finish the copy.
 */
void
cryc_hash_final(
	struct cryc_hash *hash,
	uint8_t *out)
{
	/* Finishes the hash the structure was started with. */
	switch (hash->id) {
	case CRYC_HASH_SHA256:
		cryc_sha256_final(&hash->state.sha256, out);
		break;
	case CRYC_HASH_SHA384:
	case CRYC_HASH_SHA512:
		cryc_sha512_final(&hash->state.sha512, out);
		break;
	default:
		/* A structure that names no hash has no digest to write. */
		break;
	}

	/* Wipes the whole structure, including the part of the union the hash did not use. */
	cryc_wipe(hash, sizeof(*hash));
	hash->id = CRYC_HASH_NONE;
}

/*
 * Hashes size bytes at data in one call and writes the digest to out.
 *
 * Returns 0, or ENOTSUP for an unknown id (out is not written).
 */
int
cryc_hash_digest(
	enum cryc_hash_id id,
	const uint8_t *data,
	size_t size,
	uint8_t *out)
{
	struct cryc_hash hash;
	int error;

	/* Starts the computation. */
	error = cryc_hash_init(&hash, id);
	if (error != 0)
		return error;

	/* Absorbs the whole message. */
	cryc_hash_update(&hash, data, size);

	/* Finishes the computation, which writes the digest and wipes the state. */
	cryc_hash_final(&hash, out);

	/* Succeeded: out holds the digest. */
	return 0;
}

/*
 * Starts HMAC with key (RFC 2104 section 2): a key longer than the block
 * is first hashed; the key is padded with zeros to the block size.
 *
 * key may be NULL only when key_size is 0 (an empty key is allowed).  The
 * key is not kept; the caller may wipe it after the call.  Returns 0 or
 * ENOTSUP for an unknown id.  Constant time in the key's bytes.
 */
int
cryc_hmac_init(
	struct cryc_hmac *hmac,
	enum cryc_hash_id id,
	const uint8_t *key,
	size_t key_size)
{
	uint8_t padded_key[CRYC_HASH_MAX_BLOCK];
	size_t block_size;
	size_t digest_size;
	size_t index;
	int error;

	/* Makes the structure name no hash until the key is absorbed. */
	memset(hmac, 0, sizeof(*hmac));

	/* Looks up the block size and digest size of the hash. */
	block_size = cryc_hash_block_size(id);
	digest_size = cryc_hash_size(id);
	if (block_size == 0 || digest_size == 0)
		return ENOTSUP;

	/* A missing key with a length is a caller's mistake that must not be read. */
	if (key == NULL && key_size != 0)
		return EINVAL;

	/*
	 * Builds the key block: a key longer than the block is replaced by
	 * its digest, and the key is then padded on the right with zero
	 * bytes up to the block size.  Only the length of the key decides
	 * which way it goes.
	 */
	memset(padded_key, 0, sizeof(padded_key));
	if (key_size > block_size) {
		error = cryc_hash_digest(id, key, key_size, padded_key);
		if (error != 0) {
			cryc_wipe(padded_key, sizeof(padded_key));
			return error;
		}
	} else if (key_size != 0) {
		memcpy(padded_key, key, key_size);
	}

	/* Makes the inner key: the key block exclusive-ored with ipad. */
	for (index = 0; index < block_size; index++)
		padded_key[index] = (uint8_t)(padded_key[index] ^ HMAC_INNER_PAD);

	/* Starts the inner hash with the inner key. */
	error = cryc_hash_init(&hmac->inner, id);
	if (error != 0) {
		cryc_wipe(padded_key, sizeof(padded_key));
		return error;
	}
	cryc_hash_update(&hmac->inner, padded_key, block_size);

	/*
	 * Makes the outer key from the inner key: exclusive-oring with both
	 * ipad and opad takes the ipad out and puts the opad in.
	 */
	for (index = 0; index < block_size; index++)
		padded_key[index] = (uint8_t)(padded_key[index] ^ (HMAC_INNER_PAD ^ HMAC_OUTER_PAD));

	/* Starts the outer hash with the outer key. */
	error = cryc_hash_init(&hmac->outer, id);
	if (error != 0) {
		cryc_wipe(padded_key, sizeof(padded_key));
		cryc_wipe(hmac, sizeof(*hmac));
		return error;
	}
	cryc_hash_update(&hmac->outer, padded_key, block_size);

	/* Wipes the key block, which is the key itself in another form. */
	cryc_wipe(padded_key, sizeof(padded_key));

	/* Succeeded: the structure is ready to absorb the message. */
	return 0;
}

/*
 * Absorbs size bytes of the message; data may be NULL only when size is
 * 0.
 */
void
cryc_hmac_update(
	struct cryc_hmac *hmac,
	const uint8_t *data,
	size_t size)
{
	/* The message goes through the inner hash only. */
	cryc_hash_update(&hmac->inner, data, size);
}

/*
 * Writes the MAC (the hash's digest size) to out and wipes *hmac.
 */
void
cryc_hmac_final(
	struct cryc_hmac *hmac,
	uint8_t *out)
{
	uint8_t inner_digest[CRYC_HASH_MAX_SIZE];
	size_t digest_size;

	/* Looks up the digest size of the hash the structure was started with. */
	digest_size = cryc_hash_size(hmac->inner.id);

	/* A structure that was never started has no MAC to give. */
	if (digest_size == 0) {
		cryc_wipe(hmac, sizeof(*hmac));
		return;
	}

	/* Finishes the inner hash, which covers the inner key and the message. */
	cryc_hash_final(&hmac->inner, inner_digest);

	/* Feeds the inner digest to the outer hash, which already holds the outer key. */
	cryc_hash_update(&hmac->outer, inner_digest, digest_size);

	/* Finishes the outer hash, which writes the MAC. */
	cryc_hash_final(&hmac->outer, out);

	/* Wipes the inner digest and the structure, which hold the key's influence. */
	cryc_wipe(inner_digest, sizeof(inner_digest));
	cryc_wipe(hmac, sizeof(*hmac));
}

/*
 * Wipes *hmac without producing a MAC (an abandoned computation).
 */
void
cryc_hmac_clear(
	struct cryc_hmac *hmac)
{
	/* Overwrites both hashes, which hold the key's influence. */
	cryc_wipe(hmac, sizeof(*hmac));
}

/*
 * Computes HMAC(key, data) in one call into out (the digest size).
 *
 * Returns 0 or ENOTSUP for an unknown id.
 */
int
cryc_hmac(
	enum cryc_hash_id id,
	const uint8_t *key,
	size_t key_size,
	const uint8_t *data,
	size_t size,
	uint8_t *out)
{
	struct cryc_hmac hmac;
	int error;

	/* Starts HMAC with the key. */
	error = cryc_hmac_init(&hmac, id, key, key_size);
	if (error != 0)
		return error;

	/* Absorbs the whole message. */
	cryc_hmac_update(&hmac, data, size);

	/* Finishes the computation, which writes the MAC and wipes the state. */
	cryc_hmac_final(&hmac, out);

	/* Succeeded: out holds the MAC. */
	return 0;
}

/*
 * HKDF-Extract (RFC 5869 section 2.2): PRK = HMAC-Hash(salt, IKM).
 *
 * A NULL salt or a salt_size of 0 means a string of HashLen zeros (2.2).
 * ikm may be NULL only when ikm_size is 0.  prk receives HashLen bytes
 * and may not overlap salt or ikm.  Returns 0 or ENOTSUP.
 */
int
cryc_hkdf_extract(
	enum cryc_hash_id id,
	const uint8_t *salt,
	size_t salt_size,
	const uint8_t *ikm,
	size_t ikm_size,
	uint8_t *prk)
{
	uint8_t zero_salt[CRYC_HASH_MAX_SIZE];
	size_t digest_size;
	int error;

	/* Looks up HashLen. */
	digest_size = cryc_hash_size(id);
	if (digest_size == 0)
		return ENOTSUP;

	/* A missing input with a length is a caller's mistake that must not be read. */
	if (ikm == NULL && ikm_size != 0)
		return EINVAL;

	/*
	 * An absent salt, or one of length zero, is a string of HashLen zero
	 * bytes (section 2.2).
	 */
	if (salt == NULL || salt_size == 0) {
		memset(zero_salt, 0, sizeof(zero_salt));
		error = cryc_hmac(id, zero_salt, digest_size, ikm, ikm_size, prk);
		if (error != 0)
			return error;

		/* Succeeded: prk holds the pseudorandom key made with the zero salt. */
		return 0;
	}

	/* Computes the pseudorandom key: the salt is the HMAC key and the input keying material is the message. */
	error = cryc_hmac(id, salt, salt_size, ikm, ikm_size, prk);
	if (error != 0)
		return error;

	/* Succeeded: prk holds the pseudorandom key. */
	return 0;
}

/*
 * HKDF-Expand (RFC 5869 section 2.3): out_size bytes of OKM from PRK and
 * info.
 *
 * prk_size is at least HashLen (RFC 5869 2.3; EINVAL otherwise).  info
 * may be NULL only when info_size is 0.  out_size is 0 to 255 * HashLen
 * (EINVAL above; nothing is written).  out may not overlap prk or info.
 * Returns 0, EINVAL or ENOTSUP; on an error out is not written.
 */
int
cryc_hkdf_expand(
	enum cryc_hash_id id,
	const uint8_t *prk,
	size_t prk_size,
	const uint8_t *info,
	size_t info_size,
	uint8_t *out,
	size_t out_size)
{
	struct cryc_hmac keyed;
	struct cryc_hmac running;
	uint8_t previous[CRYC_HASH_MAX_SIZE];
	size_t digest_size;
	size_t block_count;
	size_t block_number;
	size_t previous_size;
	size_t written;
	size_t chunk;
	uint8_t counter;
	int error;

	/* Looks up HashLen. */
	digest_size = cryc_hash_size(id);
	if (digest_size == 0)
		return ENOTSUP;

	/* PRK must be at least HashLen bytes (section 2.3). */
	if (prk_size < digest_size)
		return EINVAL;
	if (prk == NULL)
		return EINVAL;

	/* A missing info with a length is a caller's mistake that must not be read. */
	if (info == NULL && info_size != 0)
		return EINVAL;

	/* The output length is at most 255 * HashLen (section 2.3). */
	if (out_size > HKDF_MAX_BLOCKS * digest_size)
		return EINVAL;

	/* Asked for nothing: nothing is written. */
	if (out_size == 0)
		return 0;

	/* Works out how many HashLen blocks cover the output length, N = ceil(L / HashLen). */
	block_count = out_size / digest_size;
	if (out_size % digest_size != 0)
		block_count++;

	/* Keys HMAC with PRK once; each block starts from a copy of that state. */
	error = cryc_hmac_init(&keyed, id, prk, prk_size);
	if (error != 0)
		return error;

	/*
	 * Produces the blocks T(1) to T(N) in order.  Each is the HMAC of
	 * the previous block (empty for the first), info, and the one-octet
	 * counter; the output is T(1) | T(2) | ... cut to the requested
	 * length.
	 */
	written = 0;
	previous_size = 0;
	for (block_number = 1; block_number <= block_count; block_number++) {
		counter = (uint8_t)block_number;

		running = keyed;
		cryc_hmac_update(&running, previous, previous_size);
		cryc_hmac_update(&running, info, info_size);
		cryc_hmac_update(&running, &counter, 1);
		cryc_hmac_final(&running, previous);
		previous_size = digest_size;

		chunk = out_size - written;
		if (chunk > digest_size)
			chunk = digest_size;
		memcpy(out + written, previous, chunk);
		written += chunk;
	}

	/* Wipes the keyed state and the last block, which derive from the pseudorandom key. */
	cryc_hmac_clear(&keyed);
	cryc_wipe(previous, sizeof(previous));

	/* Succeeded: out holds the requested output keying material. */
	return 0;
}
