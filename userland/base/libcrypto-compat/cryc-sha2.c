/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SHA-256, SHA-384 and SHA-512 (FIPS 180-4).
 */

#include "cryc-sha2.h"

/*
 * Starts a SHA-256 computation (the initial hash value of 5.3.3).
 */
void
cryc_sha256_init(
	struct cryc_sha256 *hash)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
}

/*
 * Absorbs size bytes (6.2.2 for each complete block, the rest kept).
 */
void
cryc_sha256_update(
	struct cryc_sha256 *hash,
	const uint8_t *data,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
}

/*
 * Pads (5.1.1), compresses the last blocks, writes the 32-byte digest to
 * out, and wipes *hash.
 */
void
cryc_sha256_final(
	struct cryc_sha256 *hash,
	uint8_t *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(out);
}

/*
 * Starts a SHA-512 computation (5.3.5).
 */
void
cryc_sha512_init(
	struct cryc_sha512 *hash)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
}

/*
 * Starts a SHA-384 computation (5.3.4), in the same structure.
 */
void
cryc_sha384_init(
	struct cryc_sha512 *hash)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
}

/*
 * Absorbs size bytes (6.4.2 for each complete block); data may be NULL
 * only when size is 0.
 */
void
cryc_sha512_update(
	struct cryc_sha512 *hash,
	const uint8_t *data,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
}

/*
 * Pads (5.1.2), compresses the last blocks, writes hash->out_size bytes
 * (48 or 64) to out, and wipes *hash.
 */
void
cryc_sha512_final(
	struct cryc_sha512 *hash,
	uint8_t *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(out);
}
