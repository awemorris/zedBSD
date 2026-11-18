/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Keccak-p[1600, 24] and the SHA-3 and SHAKE functions (FIPS 202).
 *
 * The section and algorithm numbers in the comments of this file are
 * those of FIPS 202 (August 2015).  The state is 25 lanes of 64 bits; lane
 * (x, y) is at index x + 5y and a lane is the little-endian reading of
 * eight bytes of the state string (3.1.2, B.1), so bit z of the lane
 * is bit z of the integer and moving z up by n is a rotation to the left
 * by n.  Every step of a round is an exclusive or, an and, a complement
 * or a rotation of lanes, and every index and rotation count is a public
 * constant, so no branch and no address depends on the state.
 */

#include <string.h>

#include "cryc-sha3.h"

/* The number of lanes of the state, 5 by 5 (3.1). */
#define SHA3_LANES	25U

/* The number of rounds of Keccak-p[1600, 24] (3.3, Algorithm 7: nr = 24). */
#define SHA3_ROUNDS	24U

/* The width of a lane in bits (Table 1: w = 64 for b = 1600). */
#define SHA3_LANE_BITS	64U

/* The domain byte of the SHA-3 hash functions: the bits 01 then the first bit of the padding (6.1, B.2). */
#define SHA3_SUFFIX_HASH	0x06U

/* The domain byte of SHAKE128 and SHAKE256: the bits 1111 then the first bit of the padding (6.2, B.2). */
#define SHA3_SUFFIX_XOF		0x1fU

/* The last bit of the padding, the final 1 of pad10*1, as it lies in the last byte of a block (5.1, B.2). */
#define SHA3_PAD_LAST		0x80U

/*
 * The offsets of rho, in bits, indexed by x + 5y.
 *
 * The rotation of a lane is the offset reduced modulo 64.  The values are
 * fixed data indexed by the lane position, a public value; the table is
 * never written.
 *
 * Values referenced: FIPS 202 (August 2015) 3.2.2, Table 2 (the offsets
 * of rho), regrouped from the table's order of columns x = 3, 4, 0, 1, 2
 * and rows y = 2, 1, 0, 4, 3 into the order x = 0 to 4 within y = 0 to 4.
 */
static const unsigned rho_offset[SHA3_LANES] = {
	  0U,   1U, 190U,  28U,  91U,
	 36U, 300U,   6U,  55U, 276U,
	  3U,  10U, 171U, 153U, 231U,
	105U,  45U,  15U,  21U, 136U,
	210U,  66U, 253U, 120U,  78U
};

/*
 * The round constants of iota, one per round.
 *
 * They are fixed data indexed by the round number, a public value; the
 * table is never written.
 *
 * Values referenced: FIPS 202 (August 2015) 3.2.5, Algorithm 6 (the
 * round constant RC of round ir has bit 2^j - 1 equal to rc(j + 7ir) for
 * j = 0 to 6 and every other bit 0) with Algorithm 5 (rc), evaluated for
 * ir = 0 to 23, the rounds of Algorithm 7 for nr = 24.
 */
static const uint64_t iota_constant[SHA3_ROUNDS] = {
	CRYC_U64(0x00000000, 0x00000001), CRYC_U64(0x00000000, 0x00008082),
	CRYC_U64(0x80000000, 0x0000808a), CRYC_U64(0x80000000, 0x80008000),
	CRYC_U64(0x00000000, 0x0000808b), CRYC_U64(0x00000000, 0x80000001),
	CRYC_U64(0x80000000, 0x80008081), CRYC_U64(0x80000000, 0x00008009),
	CRYC_U64(0x00000000, 0x0000008a), CRYC_U64(0x00000000, 0x00000088),
	CRYC_U64(0x00000000, 0x80008009), CRYC_U64(0x00000000, 0x8000000a),
	CRYC_U64(0x00000000, 0x8000808b), CRYC_U64(0x80000000, 0x0000008b),
	CRYC_U64(0x80000000, 0x00008089), CRYC_U64(0x80000000, 0x00008003),
	CRYC_U64(0x80000000, 0x00008002), CRYC_U64(0x80000000, 0x00000080),
	CRYC_U64(0x00000000, 0x0000800a), CRYC_U64(0x80000000, 0x8000000a),
	CRYC_U64(0x80000000, 0x80008081), CRYC_U64(0x80000000, 0x00008080),
	CRYC_U64(0x00000000, 0x80000001), CRYC_U64(0x80000000, 0x80008008)
};

static uint64_t rotate_left(uint64_t value, unsigned count);
static uint64_t load_le64(const uint8_t *bytes);
static void sponge_start(struct cryc_sha3 *sponge, size_t rate, unsigned suffix);
static int sponge_is_usable(const struct cryc_sha3 *sponge);
static void sponge_pad(struct cryc_sha3 *sponge);
static void sponge_xor_byte(struct cryc_sha3 *sponge, size_t offset, uint8_t value);
static void sponge_xor_in(struct cryc_sha3 *sponge, const uint8_t *data, size_t count);
static void sponge_read_out(struct cryc_sha3 *sponge, uint8_t *out, size_t count);

/*
 * Applies Keccak-p[1600, 24] (3.3, Algorithm 7) to 25 lanes in place.
 *
 * lanes must hold 25 lanes.  The working copies of the state are wiped
 * before the function returns.
 */
void
cryc_keccak_permute(
	uint64_t *lanes)
{
	uint64_t parity[5];
	uint64_t spread[5];
	uint64_t moved[SHA3_LANES];
	unsigned round;
	unsigned x;
	unsigned y;
	unsigned source;

	/* A missing state is a caller's mistake and is left alone. */
	if (lanes == NULL)
		return;

	/* Runs the rounds ir = 0 to 23, each theta, rho, pi, chi, iota in turn. */
	for (round = 0; round < SHA3_ROUNDS; round++) {
		/* Theta step 1: the parity C[x] of each column of lanes (Algorithm 1). */
		for (x = 0; x < 5U; x++) {
			parity[x] = lanes[x] ^ lanes[x + 5U] ^ lanes[x + 10U] ^ lanes[x + 15U] ^ lanes[x + 20U];
		}

		/* Theta step 2: D[x] is C[x - 1] and C[x + 1] moved up one bit, so the rotation is by 1. */
		for (x = 0; x < 5U; x++) {
			spread[x] = parity[(x + 4U) % 5U] ^ rotate_left(parity[(x + 1U) % 5U], 1U);
		}

		/* Theta step 3: adds D[x] into every lane of column x. */
		for (y = 0; y < 5U; y++) {
			for (x = 0; x < 5U; x++) {
				lanes[x + 5U * y] ^= spread[x];
			}
		}

		/*
		 * Rho and pi together (Algorithms 2 and 3): the new lane (x, y)
		 * is the lane (x + 3y mod 5, x) rotated by its offset.
		 */
		for (y = 0; y < 5U; y++) {
			for (x = 0; x < 5U; x++) {
				source = (x + 3U * y) % 5U + 5U * x;
				moved[x + 5U * y] = rotate_left(lanes[source], rho_offset[source] % SHA3_LANE_BITS);
			}
		}

		/* Chi: each lane is combined with the next two lanes of its row (Algorithm 4). */
		for (y = 0; y < 5U; y++) {
			for (x = 0; x < 5U; x++) {
				lanes[x + 5U * y] = moved[x + 5U * y] ^
				    (~moved[(x + 1U) % 5U + 5U * y] & moved[(x + 2U) % 5U + 5U * y]);
			}
		}

		/* Iota: the round constant is added into lane (0, 0) (Algorithm 6). */
		lanes[0] ^= iota_constant[round];
	}

	/* Wipes the copies of the state that the rounds left on the stack. */
	cryc_wipe(parity, sizeof(parity));
	cryc_wipe(spread, sizeof(spread));
	cryc_wipe(moved, sizeof(moved));
}

/*
 * Starts SHA3-256 (6.1).
 */
void
cryc_sha3_256_init(
	struct cryc_sha3 *sponge)
{
	/* A missing sponge is a caller's mistake. */
	if (sponge == NULL)
		return;

	/* The capacity is 512 bits, so the rate is 136 bytes. */
	sponge_start(sponge, CRYC_SHA3_256_RATE, SHA3_SUFFIX_HASH);
}

/*
 * Starts SHA3-512 (6.1).
 */
void
cryc_sha3_512_init(
	struct cryc_sha3 *sponge)
{
	/* A missing sponge is a caller's mistake. */
	if (sponge == NULL)
		return;

	/* The capacity is 1024 bits, so the rate is 72 bytes. */
	sponge_start(sponge, CRYC_SHA3_512_RATE, SHA3_SUFFIX_HASH);
}

/*
 * Starts SHAKE128 (6.2).
 */
void
cryc_shake128_init(
	struct cryc_sha3 *sponge)
{
	/* A missing sponge is a caller's mistake. */
	if (sponge == NULL)
		return;

	/* The capacity is 256 bits, so the rate is 168 bytes. */
	sponge_start(sponge, CRYC_SHAKE128_RATE, SHA3_SUFFIX_XOF);
}

/*
 * Starts SHAKE256 (6.2).
 */
void
cryc_shake256_init(
	struct cryc_sha3 *sponge)
{
	/* A missing sponge is a caller's mistake. */
	if (sponge == NULL)
		return;

	/* The capacity is 512 bits, so the rate is 136 bytes. */
	sponge_start(sponge, CRYC_SHAKE256_RATE, SHA3_SUFFIX_XOF);
}

/*
 * Absorbs size bytes.
 *
 * Only before the first squeeze or final; a call after them, on a sponge
 * that was never started, or with missing data does nothing.  data may be
 * NULL only when size is 0.
 */
void
cryc_sha3_absorb(
	struct cryc_sha3 *sponge,
	const uint8_t *data,
	size_t size)
{
	size_t room;
	size_t chunk;
	int usable;

	/* Nothing to absorb. */
	if (sponge == NULL)
		return;
	if (size == 0)
		return;

	/* Missing data with a length is a caller's mistake that must not be read. */
	if (data == NULL)
		return;

	/* A sponge that was never started, was wiped, or has begun to give output takes no input. */
	usable = sponge_is_usable(sponge);
	if (usable == 0)
		return;
	if (sponge->squeezing != 0)
		return;

	/* Takes the input a block at a time, running the permutation each time a block is full (Algorithm 8 step 6). */
	while (size > 0) {
		/* A full block is mixed in before more is added. */
		if (sponge->position >= sponge->rate) {
			cryc_keccak_permute(sponge->lanes);
			sponge->position = 0;
		}

		/* As much as fits in the rest of the current block. */
		room = sponge->rate - sponge->position;
		chunk = size;
		if (chunk > room)
			chunk = room;

		/* Exclusive-ors the chunk into the state at the current position. */
		sponge_xor_in(sponge, data, chunk);

		/* Moves past the bytes taken. */
		data += chunk;
		size -= chunk;
	}

	/* A block that was just filled is mixed in at once, so a pending position is always inside a block. */
	if (sponge->position >= sponge->rate) {
		cryc_keccak_permute(sponge->lanes);
		sponge->position = 0;
	}
}

/*
 * SHA3-256 and SHA3-512: pads, permutes and writes the digest (32 or 64
 * bytes, the sponge's c / 2 bits) to out, then wipes the sponge.
 *
 * A sponge that is not an unfinished SHA-3 hash writes nothing and is
 * wiped all the same.
 */
void
cryc_sha3_final(
	struct cryc_sha3 *sponge,
	uint8_t *out)
{
	size_t digest_size;
	int usable;

	/* A missing sponge is a caller's mistake. */
	if (sponge == NULL)
		return;

	/* The digest is half the capacity: 32 bytes at rate 136, 64 bytes at rate 72, and none for anything else. */
	digest_size = 0;
	usable = sponge_is_usable(sponge);
	if (usable != 0 && sponge->squeezing == 0 && sponge->suffix == SHA3_SUFFIX_HASH) {
		if (sponge->rate == CRYC_SHA3_256_RATE)
			digest_size = 32U;
		else if (sponge->rate == CRYC_SHA3_512_RATE)
			digest_size = 64U;
	}

	/* Pads, permutes and writes the digest; it is shorter than the rate, so one block is enough (Algorithm 8 step 8). */
	if (digest_size != 0 && out != NULL) {
		sponge_pad(sponge);
		sponge_read_out(sponge, out, digest_size);
	}

	/* Wipes the state, which has absorbed the message. */
	cryc_sha3_clear(sponge);
}

/*
 * SHAKE128 and SHAKE256: pads on the first call, then writes the next size
 * bytes of output to out (any number of calls, any sizes; the output is
 * one stream).
 *
 * A sponge that was never started or was wiped gives zero bytes.
 */
void
cryc_shake_squeeze(
	struct cryc_sha3 *sponge,
	uint8_t *out,
	size_t size)
{
	size_t room;
	size_t chunk;
	int usable;

	/* A missing sponge is a caller's mistake. */
	if (sponge == NULL)
		return;

	/* A sponge that was never started or was wiped gives only zeros, never stale memory. */
	usable = sponge_is_usable(sponge);
	if (usable == 0) {
		if (out != NULL && size > 0)
			memset(out, 0, size);
		return;
	}

	/* The first call ends the input: pads and mixes in the last block (Algorithm 8 step 6). */
	if (sponge->squeezing == 0)
		sponge_pad(sponge);

	/* Missing room for the output is a caller's mistake that must not be written. */
	if (out == NULL)
		return;

	/* Gives the output a block at a time, running the permutation when a block is used up (Algorithm 8 step 10). */
	while (size > 0) {
		/* A block that was given out in full is replaced by the next one. */
		if (sponge->position >= sponge->rate) {
			cryc_keccak_permute(sponge->lanes);
			sponge->position = 0;
		}

		/* As much as is left in the current block. */
		room = sponge->rate - sponge->position;
		chunk = size;
		if (chunk > room)
			chunk = room;

		/* Copies the chunk out of the state at the current position. */
		sponge_read_out(sponge, out, chunk);

		/* Moves past the bytes given. */
		out += chunk;
		size -= chunk;
	}
}

/*
 * Wipes the sponge.
 *
 * A wiped sponge has rate 0, so every further call on it does nothing.
 */
void
cryc_sha3_clear(
	struct cryc_sha3 *sponge)
{
	/* A missing sponge is a caller's mistake. */
	if (sponge == NULL)
		return;

	/* Overwrites the state and the bookkeeping in a way the compiler keeps. */
	cryc_wipe(sponge, sizeof(*sponge));
}

/*
 * Rotates a lane to the left by count bits, 0 to 63.
 *
 * The right shift is by 64 - count reduced modulo 64, so count 0 never
 * shifts by the width of the type.
 */
static uint64_t
rotate_left(
	uint64_t value,
	unsigned count)
{
	uint64_t high;
	uint64_t low;

	/* The bits that move up and the bits that wrap around to the bottom. */
	high = value << count;
	low = value >> ((SHA3_LANE_BITS - count) % SHA3_LANE_BITS);

	/* Succeeded: the rotated lane (count 0 gives the value itself). */
	return high | low;
}

/*
 * Reads a 64-bit little-endian integer from bytes[0..7].
 */
static uint64_t
load_le64(
	const uint8_t *bytes)
{
	uint64_t value;
	unsigned index;

	/* Puts byte i at bit 8i, the first byte of the state string lowest (3.1.2, B.1). */
	value = 0;
	for (index = 0; index < 8U; index++) {
		value |= (uint64_t)bytes[index] << (8U * index);
	}

	/* Succeeded: the lane. */
	return value;
}

/*
 * Clears the state and sets the rate and the domain byte of a new sponge.
 */
static void
sponge_start(
	struct cryc_sha3 *sponge,
	size_t rate,
	unsigned suffix)
{
	unsigned index;

	/* Step 5 of Algorithm 8: the state is 0 to the power b. */
	for (index = 0; index < SHA3_LANES; index++) {
		sponge->lanes[index] = 0;
	}

	/* Nothing absorbed yet, and the input is still open. */
	sponge->rate = rate;
	sponge->suffix = suffix;
	sponge->position = 0;
	sponge->squeezing = 0;
}

/*
 * Reports whether a sponge was started and has a sound position.
 *
 * Returns 1 for a rate that one of the four functions uses with a
 * position not past the end of the block, and 0 for a sponge that was
 * never started, was wiped, or was damaged.
 */
static int
sponge_is_usable(
	const struct cryc_sha3 *sponge)
{
	/* The four rates of the functions (6.1, 6.2) are the only ones the sponge is started with. */
	if (sponge->rate != CRYC_SHA3_256_RATE &&
	    sponge->rate != CRYC_SHA3_512_RATE &&
	    sponge->rate != CRYC_SHAKE128_RATE)
		return 0;

	/* A position inside the block, or just past it when a block was used up and the next is not yet made. */
	if (sponge->position > sponge->rate)
		return 0;

	/* Succeeded: the sponge can be used. */
	return 1;
}

/*
 * Ends the input: adds the domain byte and the padding of pad10*1 and
 * mixes in the last block.
 *
 * The domain bits and the first padding bit make the one-byte suffix at the
 * current position; the last padding bit is the top bit of the last byte of
 * the block (5.1, 6.1, 6.2, B.2).  When both land in one byte they add
 * together.  After this the sponge gives output from position 0.
 */
static void
sponge_pad(
	struct cryc_sha3 *sponge)
{
	/* A block that is already full is mixed in first, so the suffix has a place in an open block. */
	if (sponge->position >= sponge->rate) {
		cryc_keccak_permute(sponge->lanes);
		sponge->position = 0;
	}

	/* The domain bits with the first bit of the padding, then the closing bit of the padding. */
	sponge_xor_byte(sponge, sponge->position, (uint8_t)sponge->suffix);
	sponge_xor_byte(sponge, sponge->rate - 1U, (uint8_t)SHA3_PAD_LAST);

	/* The block is complete: mixes it in. */
	cryc_keccak_permute(sponge->lanes);

	/* The input is closed, and the output starts at the beginning of the block. */
	sponge->position = 0;
	sponge->squeezing = 1;
}

/*
 * Exclusive-ors one byte into the state at a byte offset of the state
 * string.
 *
 * The offset must be inside the state.  Byte i of the string is bits 8(i
 * mod 8) to 8(i mod 8) + 7 of lane i / 8 (3.1.2, B.1).
 */
static void
sponge_xor_byte(
	struct cryc_sha3 *sponge,
	size_t offset,
	uint8_t value)
{
	size_t shift;

	/* The place of the byte within its lane. */
	shift = 8U * (offset % 8U);
	sponge->lanes[offset / 8U] ^= (uint64_t)value << shift;
}

/*
 * Exclusive-ors count bytes into the state at the current position.
 *
 * count must not run past the end of the block.  Whole aligned lanes are
 * taken at once; the bytes before and after one at a time.
 */
static void
sponge_xor_in(
	struct cryc_sha3 *sponge,
	const uint8_t *data,
	size_t count)
{
	size_t used;
	size_t shift;

	/* Moves along the data, using the position as the offset in the state string. */
	used = 0;
	while (used < count) {
		/* A whole aligned lane in one step. */
		if (sponge->position % 8U == 0 && count - used >= 8U) {
			sponge->lanes[sponge->position / 8U] ^= load_le64(data + used);
			sponge->position += 8U;
			used += 8U;
		} else {
			/* One byte, at the place within its lane that the position gives. */
			shift = 8U * (sponge->position % 8U);
			sponge->lanes[sponge->position / 8U] ^= (uint64_t)data[used] << shift;
			sponge->position += 1U;
			used += 1U;
		}
	}
}

/*
 * Copies count bytes of the state at the current position to out and
 * advances the position.
 *
 * count must not run past the end of the block.
 */
static void
sponge_read_out(
	struct cryc_sha3 *sponge,
	uint8_t *out,
	size_t count)
{
	size_t used;
	size_t shift;
	unsigned index;
	uint64_t lane;

	/* Moves along the output, using the position as the offset in the state string. */
	used = 0;
	while (used < count) {
		/* A whole aligned lane in one step. */
		if (sponge->position % 8U == 0 && count - used >= 8U) {
			lane = sponge->lanes[sponge->position / 8U];
			for (index = 0; index < 8U; index++) {
				out[used + index] = (uint8_t)(lane >> (8U * index));
			}

			/* Moves past the lane. */
			sponge->position += 8U;
			used += 8U;
		} else {
			/* One byte, from the place within its lane that the position gives. */
			shift = 8U * (sponge->position % 8U);
			out[used] = (uint8_t)(sponge->lanes[sponge->position / 8U] >> shift);
			sponge->position += 1U;
			used += 1U;
		}
	}
}
