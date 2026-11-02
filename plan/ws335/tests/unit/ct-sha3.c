/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the SHA-3 functions: messages, seeds and the
 * permutation's state are marked secret, so memcheck reports any branch
 * or address that depends on their bytes (their lengths are public and
 * may be used freely).
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-sha3.h"

/* Fills size bytes at out with a fixed pattern. */
static void fill(uint8_t *out, size_t size, unsigned seed);

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned fold(const uint8_t *bytes, size_t size);

/* Fills size bytes at out with a fixed pattern. */
static void
fill(
	uint8_t *out,
	size_t size,
	unsigned seed)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(seed + index * 13U + (index >> 3));
}

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned
fold(
	const uint8_t *bytes,
	size_t size)
{
	unsigned value;
	size_t index;

	/* Exclusive-or of the bytes, rotated. */
	value = 0;
	for (index = 0; index < size; index++)
		value = ((value << 1) | (value >> 7)) ^ bytes[index];

	/* Reports the folded value. */
	return value & 0xffU;
}

/* Runs the checks. */
int
main(void)
{
	uint8_t data[400];
	uint8_t out[500];
	uint64_t lanes[25];
	struct cryc_sha3 sponge;
	unsigned total;
	unsigned lane;

	/* The sum of the folded outputs starts at zero. */
	total = 0;

	/* The permutation of a secret state. */
	for (lane = 0; lane < 25U; lane++)
		lanes[lane] = 0x0123456789abcdefULL * (lane + 1U);
	CT_SECRET(lanes, sizeof(lanes));
	cryc_keccak_permute(lanes);
	CT_PUBLIC(lanes, sizeof(lanes));
	total += lanes[0] & 0xffU;

	/* SHA3-256 of a secret message in parts across the block boundary. */
	fill(data, sizeof(data), 5U);
	CT_SECRET(data, sizeof(data));
	cryc_sha3_256_init(&sponge);
	cryc_sha3_absorb(&sponge, data, 100U);
	cryc_sha3_absorb(&sponge, data + 100, 200U);
	cryc_sha3_final(&sponge, out);
	CT_PUBLIC(out, 32U);
	total += fold(out, 32U);
	CT_PUBLIC(data, sizeof(data));

	/* SHA3-512 of a secret message. */
	fill(data, sizeof(data), 9U);
	CT_SECRET(data, sizeof(data));
	cryc_sha3_512_init(&sponge);
	cryc_sha3_absorb(&sponge, data, 71U);
	cryc_sha3_absorb(&sponge, data + 71, 73U);
	cryc_sha3_final(&sponge, out);
	CT_PUBLIC(out, 64U);
	total += fold(out, 64U);
	CT_PUBLIC(data, sizeof(data));

	/* SHAKE128 of a secret seed, squeezed in several parts across block boundaries. */
	fill(data, sizeof(data), 17U);
	CT_SECRET(data, sizeof(data));
	cryc_shake128_init(&sponge);
	cryc_sha3_absorb(&sponge, data, 34U);
	cryc_shake_squeeze(&sponge, out, 168U);
	cryc_shake_squeeze(&sponge, out + 168, 5U);
	cryc_shake_squeeze(&sponge, out + 173, 300U);
	cryc_sha3_clear(&sponge);
	CT_PUBLIC(out, 473U);
	total += fold(out, 473U);
	CT_PUBLIC(data, sizeof(data));

	/* SHAKE256 of a secret seed. */
	fill(data, sizeof(data), 31U);
	CT_SECRET(data, sizeof(data));
	cryc_shake256_init(&sponge);
	cryc_sha3_absorb(&sponge, data, 33U);
	cryc_sha3_absorb(&sponge, data + 33, 136U);
	cryc_shake_squeeze(&sponge, out, 64U);
	cryc_shake_squeeze(&sponge, out + 64, 200U);
	cryc_sha3_clear(&sponge);
	CT_PUBLIC(out, 264U);
	total += fold(out, 264U);
	CT_PUBLIC(data, sizeof(data));

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);

	/* The result. */
	return check_finish("ct-sha3");
}
