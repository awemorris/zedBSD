/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SHA-2 hash functions TLS and X.509 use: SHA-256 (the compression
 * function on 32-bit words), SHA-384 and SHA-512 (the one on 64-bit
 * words).  SHA-224 and SHA-512/t are not needed and not provided.
 *
 * A hash absorbs secrets (HMAC keys, key-schedule secrets), so its state
 * is wiped by the _final functions; the compression functions are
 * constant time by construction (no data-dependent branch or index), and
 * an implementation keeps them so.  Lengths are public.
 *
 * Messages are limited to 2^61 - 1 bytes for every function here (the
 * byte count is kept in a uint64_t and the bit length of SHA-224/256 is
 * a 64-bit field); a longer total is a caller's mistake and is not
 * checked.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_SHA2_H
#define LIBCRYPTO_COMPAT_CRYC_SHA2_H

#include "cryc.h"

/*
 * The section numbers in the comments of this file are those of FIPS
 * 180-4 (August 2015).
 */

/* The digest and block sizes in bytes (FIPS 180-4 1, Figure 1). */
#define CRYC_SHA256_SIZE	32U
#define CRYC_SHA384_SIZE	48U
#define CRYC_SHA512_SIZE	64U
#define CRYC_SHA256_BLOCK	64U
#define CRYC_SHA512_BLOCK	128U

/*
 * A SHA-256 computation in progress.
 *
 * state is H(i) (6.2.2, 6.3); block holds used bytes of a block not yet
 * compressed (0 to 63); total counts every byte absorbed.  The structure
 * owns no memory; the caller allocates it.  It may be copied by
 * assignment to take a snapshot (a transcript hash).
 */
struct cryc_sha256 {
	uint32_t state[8];
	uint8_t block[CRYC_SHA256_BLOCK];
	size_t used;
	uint64_t total;
};

/*
 * A SHA-384 or SHA-512 computation in progress.
 *
 * state is H(i) (6.4.2, 6.5); block holds used bytes not yet compressed
 * (0 to 127); total counts every byte absorbed (the 128-bit length field
 * of 5.1.2 is total * 8 with its high 64 bits taken from total >> 61).
 * out_size is 48 for SHA-384 and 64 for SHA-512.  Copyable like
 * struct cryc_sha256.
 */
struct cryc_sha512 {
	uint64_t state[8];
	uint8_t block[CRYC_SHA512_BLOCK];
	size_t used;
	uint64_t total;
	size_t out_size;
};

/*
 * Starts a SHA-256 computation (the initial hash value of 5.3.3).
 *
 * hash is not NULL.  Any previous content of *hash is overwritten.
 */
void cryc_sha256_init(struct cryc_sha256 *hash);

/*
 * Absorbs size bytes (6.2.2 for each complete block, the rest kept).
 *
 * data may be NULL only when size is 0.  May be called any number of
 * times between _init and _final.
 */
void cryc_sha256_update(struct cryc_sha256 *hash, const uint8_t *data, size_t size);

/*
 * Pads (5.1.1), compresses the last blocks, writes the 32-byte digest
 * to out, and wipes *hash.
 *
 * out has room for 32 bytes.  After _final the
 * structure must be started again with an _init function before reuse.
 */
void cryc_sha256_final(struct cryc_sha256 *hash, uint8_t *out);

/* Starts a SHA-512 computation (5.3.5). */
void cryc_sha512_init(struct cryc_sha512 *hash);

/* Starts a SHA-384 computation (5.3.4), in the same structure. */
void cryc_sha384_init(struct cryc_sha512 *hash);

/* Absorbs size bytes (6.4.2 for each complete block); data may be NULL only when size is 0. */
void cryc_sha512_update(struct cryc_sha512 *hash, const uint8_t *data, size_t size);

/*
 * Pads (5.1.2), compresses the last blocks, writes hash->out_size bytes
 * (48 or 64) to out, and wipes *hash.
 */
void cryc_sha512_final(struct cryc_sha512 *hash, uint8_t *out);

#endif
