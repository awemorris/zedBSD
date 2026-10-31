/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hash chosen at run time, HMAC (RFC 2104) and HKDF (RFC 5869).
 */

#include "cryc-hash.h"

/*
 * Returns the digest size in bytes of id (32, 48, 64), or 0 for
 * CRYC_HASH_NONE or an unknown value.
 */
size_t
cryc_hash_size(
	enum cryc_hash_id id)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(id);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns the block size in bytes of id (64 or 128), or 0 for
 * CRYC_HASH_NONE or an unknown value.
 */
size_t
cryc_hash_block_size(
	enum cryc_hash_id id)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(id);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Starts a computation of id.
 */
int
cryc_hash_init(
	struct cryc_hash *hash,
	enum cryc_hash_id id)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(id);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Absorbs size bytes; data may be NULL only when size is 0.
 */
void
cryc_hash_update(
	struct cryc_hash *hash,
	const uint8_t *data,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
}

/*
 * Writes the digest (cryc_hash_size(hash->id) bytes) to out and wipes
 * *hash (its id becomes CRYC_HASH_NONE).
 */
void
cryc_hash_final(
	struct cryc_hash *hash,
	uint8_t *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(out);
}

/*
 * Hashes size bytes at data in one call and writes the digest to out.
 */
int
cryc_hash_digest(
	enum cryc_hash_id id,
	const uint8_t *data,
	size_t size,
	uint8_t *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(out);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Starts HMAC with key (RFC 2104 section 2): a key longer than the block
 * is first hashed; the key is padded with zeros to the block size.
 */
int
cryc_hmac_init(
	struct cryc_hmac *hmac,
	enum cryc_hash_id id,
	const uint8_t *key,
	size_t key_size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hmac);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(key_size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Absorbs size bytes of the message; data may be NULL only when size is 0.
 */
void
cryc_hmac_update(
	struct cryc_hmac *hmac,
	const uint8_t *data,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hmac);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
}

/*
 * Writes the MAC (the hash's digest size) to out and wipes *hmac.
 */
void
cryc_hmac_final(
	struct cryc_hmac *hmac,
	uint8_t *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hmac);
	UNUSED_PARAMETER(out);
}

/*
 * Wipes *hmac without producing a MAC (an abandoned computation).
 */
void
cryc_hmac_clear(
	struct cryc_hmac *hmac)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hmac);
}

/*
 * Computes HMAC(key, data) in one call into out (the digest size).
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(key_size);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(out);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * HKDF-Extract (RFC 5869 section 2.2): PRK = HMAC-Hash(salt, IKM).
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(salt);
	UNUSED_PARAMETER(salt_size);
	UNUSED_PARAMETER(ikm);
	UNUSED_PARAMETER(ikm_size);
	UNUSED_PARAMETER(prk);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * HKDF-Expand (RFC 5869 section 2.3): out_size bytes of OKM from PRK and
 * info.
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(prk);
	UNUSED_PARAMETER(prk_size);
	UNUSED_PARAMETER(info);
	UNUSED_PARAMETER(info_size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(out_size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
