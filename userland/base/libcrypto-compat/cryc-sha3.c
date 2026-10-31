/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Keccak-p[1600, 24] and the SHA-3 and SHAKE functions (FIPS 202).
 */

#include "cryc-sha3.h"

/*
 * Applies Keccak-p[1600, 24] (3.3, Algorithm 7) to 25 lanes in place.
 */
void
cryc_keccak_permute(
	uint64_t *lanes)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(lanes);
}

/*
 * Starts SHA3-256 (6.1).
 */
void
cryc_sha3_256_init(
	struct cryc_sha3 *sponge)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(sponge);
}

/*
 * Starts SHA3-512 (6.1).
 */
void
cryc_sha3_512_init(
	struct cryc_sha3 *sponge)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(sponge);
}

/*
 * Starts SHAKE128 (6.2).
 */
void
cryc_shake128_init(
	struct cryc_sha3 *sponge)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(sponge);
}

/*
 * Starts SHAKE256 (6.2).
 */
void
cryc_shake256_init(
	struct cryc_sha3 *sponge)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(sponge);
}

/*
 * Absorbs size bytes.
 */
void
cryc_sha3_absorb(
	struct cryc_sha3 *sponge,
	const uint8_t *data,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(sponge);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
}

/*
 * SHA3-256 and SHA3-512: pads, permutes and writes the digest (32 or 64
 * bytes, the sponge's c / 2 bits) to out, then wipes the sponge.
 */
void
cryc_sha3_final(
	struct cryc_sha3 *sponge,
	uint8_t *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(sponge);
	UNUSED_PARAMETER(out);
}

/*
 * SHAKE128 and SHAKE256: pads on the first call, then writes the next size
 * bytes of output to out (any number of calls, any sizes; the output is
 * one stream).
 */
void
cryc_shake_squeeze(
	struct cryc_sha3 *sponge,
	uint8_t *out,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(sponge);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(size);
}

/*
 * Wipes the sponge.
 */
void
cryc_sha3_clear(
	struct cryc_sha3 *sponge)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(sponge);
}
