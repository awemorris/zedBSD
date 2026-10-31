/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SHA-3 functions ML-KEM uses (FIPS 203 4.1): SHA3-256, SHA3-512,
 * SHAKE128 and SHAKE256, on the permutation Keccak-p[1600, 24].
 *
 * The sponge (4, Algorithm 8) is kept as a state that absorbs any number
 * of times and, for the SHAKEs, squeezes any number of times, as FIPS
 * 203's XOF (4.1, Algorithm 2's absorb and squeeze) requires.  Bit
 * strings are whole bytes; the padding bits of 6.1 and 6.2 (01 for SHA-3,
 * 1111 for SHAKE, then pad10*1, B.2) are applied as the bytes 0x06 and
 * 0x1f and a final 0x80.
 *
 * Constant time: the permutation is XORs, ANDs, NOTs and rotations of
 * 64-bit lanes (3.2), no table indexed by data; the round constants
 * (3.2.5, Algorithm 5) are data cited at their table in the .c file.
 * Inputs absorbed (ML-KEM seeds and keys) are secret; the state is wiped
 * by cryc_sha3_clear and by the one-shot functions.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_SHA3_H
#define LIBCRYPTO_COMPAT_CRYC_SHA3_H

#include "cryc.h"

/*
 * The section and algorithm numbers in the comments of this file are
 * those of FIPS 202 (August 2015).
 */

/* The rates in bytes (r / 8, 6.1 and 6.2: c = 2d for SHA-3, c = 256 and 512 for the SHAKEs). */
#define CRYC_SHA3_256_RATE	136U
#define CRYC_SHA3_512_RATE	72U
#define CRYC_SHAKE128_RATE	168U
#define CRYC_SHAKE256_RATE	136U

/*
 * A sponge in progress.
 *
 * lanes is the state A as 25 64-bit lanes, lane (x, y) at index x + 5y,
 * bytes little-endian within a lane (3.1.2, B.1).  rate is r / 8.
 * suffix is the domain byte (0x06 for SHA-3, 0x1f for SHAKE).  position
 * is the byte offset in the current block (absorbing) or of the next
 * byte to squeeze; squeezing is 1 once padding has been applied.
 * Copyable; secret when it has absorbed secrets.
 */
struct cryc_sha3 {
	uint64_t lanes[25];
	size_t rate;
	unsigned suffix;
	size_t position;
	int squeezing;
};

/* Applies Keccak-p[1600, 24] (3.3, Algorithm 7) to 25 lanes in place.  Exposed for the unit tests. */
void cryc_keccak_permute(uint64_t *lanes);

/* Starts SHA3-256 (6.1). */
void cryc_sha3_256_init(struct cryc_sha3 *sponge);

/* Starts SHA3-512 (6.1). */
void cryc_sha3_512_init(struct cryc_sha3 *sponge);

/* Starts SHAKE128 (6.2). */
void cryc_shake128_init(struct cryc_sha3 *sponge);

/* Starts SHAKE256 (6.2). */
void cryc_shake256_init(struct cryc_sha3 *sponge);

/*
 * Absorbs size bytes.  Only before the first squeeze or final (a call
 * after them is a caller's mistake and does nothing).  data may be NULL
 * only when size is 0.
 */
void cryc_sha3_absorb(struct cryc_sha3 *sponge, const uint8_t *data, size_t size);

/*
 * SHA3-256 and SHA3-512: pads, permutes and writes the digest (32 or 64
 * bytes, the sponge's c / 2 bits) to out, then wipes the sponge.
 */
void cryc_sha3_final(struct cryc_sha3 *sponge, uint8_t *out);

/*
 * SHAKE128 and SHAKE256: pads on the first call, then writes the next size
 * bytes of output to out (any number of calls, any sizes; the output is
 * one stream).  The sponge is not wiped (the caller calls
 * cryc_sha3_clear).
 */
void cryc_shake_squeeze(struct cryc_sha3 *sponge, uint8_t *out, size_t size);

/* Wipes the sponge. */
void cryc_sha3_clear(struct cryc_sha3 *sponge);

#endif
