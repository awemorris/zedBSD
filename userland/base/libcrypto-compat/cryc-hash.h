/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A hash chosen at run time (the hash of a cipher suite, of a signature
 * algorithm, of an EVP_MD), and the constructions on a hash that TLS and
 * the public interface use: HMAC (RFC 2104) and HKDF (RFC 5869).
 *
 * HMAC keys and HKDF inputs are secrets: every structure here is wiped
 * by its _final function or by cryc_hmac_clear, and the functions are
 * constant time in the key and the data (lengths are public).
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_HASH_H
#define LIBCRYPTO_COMPAT_CRYC_HASH_H

#include "cryc-sha2.h"

/* The largest digest and block of any hash here (SHA-512's). */
#define CRYC_HASH_MAX_SIZE	CRYC_SHA512_SIZE
#define CRYC_HASH_MAX_BLOCK	CRYC_SHA512_BLOCK

/* The hashes.  0 is no hash, so a zeroed structure names none. */
enum cryc_hash_id {
	CRYC_HASH_NONE = 0,
	CRYC_HASH_SHA256 = 1,
	CRYC_HASH_SHA384 = 2,
	CRYC_HASH_SHA512 = 3
};

/*
 * A computation of the hash id, in progress.
 *
 * id selects the member of state in use.  Copyable by assignment (a
 * snapshot of a transcript).  Owns no memory.
 */
struct cryc_hash {
	enum cryc_hash_id id;
	union {
		struct cryc_sha256 sha256;
		struct cryc_sha512 sha512;
	} state;
};

/*
 * An HMAC computation in progress (RFC 2104 section 2).
 *
 * inner has absorbed (K xor ipad) and the data so far; outer has absorbed
 * (K xor opad).  Both hold the key's influence and are secret.
 */
struct cryc_hmac {
	struct cryc_hash inner;
	struct cryc_hash outer;
};

/* Returns the digest size in bytes of id (32, 48, 64), or 0 for CRYC_HASH_NONE or an unknown value. */
size_t cryc_hash_size(enum cryc_hash_id id);

/* Returns the block size in bytes of id (64 or 128), or 0 for CRYC_HASH_NONE or an unknown value. */
size_t cryc_hash_block_size(enum cryc_hash_id id);

/*
 * Starts a computation of id.
 *
 * Returns 0, or ENOTSUP for CRYC_HASH_NONE or an unknown id (hash->id is
 * then CRYC_HASH_NONE).
 */
int cryc_hash_init(struct cryc_hash *hash, enum cryc_hash_id id);

/* Absorbs size bytes; data may be NULL only when size is 0.  hash was started with cryc_hash_init. */
void cryc_hash_update(struct cryc_hash *hash, const uint8_t *data, size_t size);

/*
 * Writes the digest (cryc_hash_size(hash->id) bytes) to out and wipes
 * *hash (its id becomes CRYC_HASH_NONE).
 *
 * To read a digest and go on absorbing (a transcript), copy the structure
 * and finish the copy.
 */
void cryc_hash_final(struct cryc_hash *hash, uint8_t *out);

/*
 * Hashes size bytes at data in one call and writes the digest to out.
 *
 * Returns 0, or ENOTSUP for an unknown id (out is not written).
 */
int cryc_hash_digest(enum cryc_hash_id id, const uint8_t *data, size_t size, uint8_t *out);

/*
 * Starts HMAC with key (RFC 2104 section 2): a key longer than the block
 * is first hashed; the key is padded with zeros to the block size.
 *
 * key may be NULL only when key_size is 0 (an empty key is allowed).  The
 * key is not kept; the caller may wipe it after the call.  Returns 0 or
 * ENOTSUP for an unknown id.  Constant time in the key's bytes.
 */
int cryc_hmac_init(struct cryc_hmac *hmac, enum cryc_hash_id id, const uint8_t *key, size_t key_size);

/* Absorbs size bytes of the message; data may be NULL only when size is 0. */
void cryc_hmac_update(struct cryc_hmac *hmac, const uint8_t *data, size_t size);

/* Writes the MAC (the hash's digest size) to out and wipes *hmac. */
void cryc_hmac_final(struct cryc_hmac *hmac, uint8_t *out);

/* Wipes *hmac without producing a MAC (an abandoned computation). */
void cryc_hmac_clear(struct cryc_hmac *hmac);

/*
 * Computes HMAC(key, data) in one call into out (the digest size).
 *
 * Returns 0 or ENOTSUP for an unknown id.
 */
int cryc_hmac(enum cryc_hash_id id, const uint8_t *key, size_t key_size, const uint8_t *data, size_t size, uint8_t *out);

/*
 * HKDF-Extract (RFC 5869 section 2.2): PRK = HMAC-Hash(salt, IKM).
 *
 * A NULL salt or a salt_size of 0 means a string of HashLen zeros (2.2).
 * ikm may be NULL only when ikm_size is 0.  prk receives HashLen bytes
 * and may not overlap salt or ikm.  Returns 0 or ENOTSUP.
 */
int cryc_hkdf_extract(enum cryc_hash_id id, const uint8_t *salt, size_t salt_size, const uint8_t *ikm, size_t ikm_size, uint8_t *prk);

/*
 * HKDF-Expand (RFC 5869 section 2.3): out_size bytes of OKM from PRK and
 * info.
 *
 * prk_size is at least HashLen (RFC 5869 2.3; EINVAL otherwise).  info
 * may be NULL only when info_size is 0.  out_size is 0 to 255 * HashLen
 * (EINVAL above; nothing is written).  out may not overlap prk or info.
 * Returns 0, EINVAL or ENOTSUP; on an error out is not written.
 */
int cryc_hkdf_expand(enum cryc_hash_id id, const uint8_t *prk, size_t prk_size, const uint8_t *info, size_t info_size, uint8_t *out, size_t out_size);

#endif
