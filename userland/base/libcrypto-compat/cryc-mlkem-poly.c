/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The polynomial arithmetic, encodings and sampling of ML-KEM (FIPS 203
 * 4).
 *
 * The section and algorithm numbers in the comments of this file are those
 * of FIPS 203 (August 2024).  Every coefficient is kept fully reduced in
 * [0, q - 1] between functions; every modular reduction is a Barrett
 * reduction with a fixed multiplier followed by a masked subtraction, so
 * no division or remainder is applied to a value derived from a secret.
 */

#include "cryc-mlkem.h"

/* The modulus as an unsigned value, for the unsigned arithmetic below. */
#define MLKEM_Q		3329U

/*
 * The multiplier of the Barrett reduction: floor(2^32 / q).  For an x of
 * 32 bits, (x * MLKEM_BARRETT) >> 32 is the quotient of x by q or one
 * less, so x minus that quotient times q is below 2 * q.
 */
#define MLKEM_BARRETT	1290167U

/*
 * The multiplier of the division of the numerator of Compress by q:
 * ceil(2^35 / q).  For every numerator n below 2^23 (the numerators of
 * Compress reach at most 3328 * 2^11 + 1664), (n * MLKEM_COMPRESS_DIV) >>
 * 35 is exactly floor(n / q).
 */
#define MLKEM_COMPRESS_DIV	10321340U

/* The inverse of 128 modulo q, 3303 (Algorithm 10, line 14). */
#define MLKEM_INVERSE_128	3303U

/* The most 3-byte groups SampleNTT reads from the XOF per block of output (168 / 3). */
#define MLKEM_SAMPLE_GROUPS	56U

/*
 * The most blocks of XOF output SampleNTT reads before it gives up.  The
 * limit is far above the 280 iterations Appendix B gives as the lowest
 * allowed bound (each block holds 56 iterations).
 */
#define MLKEM_SAMPLE_BLOCKS	64U

/*
 * The powers of the primitive 256th root of unity zeta = 17 that the NTT
 * uses: entry i is zeta^BitRev7(i) mod q.  It is fixed data indexed by
 * the step number of the transform, a public value; the table is never
 * written.  The factors of Algorithm 11, zeta^(2 BitRev7(i) + 1), are
 * the second array of Appendix A: entry 64 + i / 2 of this table for even
 * i and its negative for odd i.
 *
 * Values referenced: FIPS 203 (August 2024) Appendix A, the array of
 * zeta^BitRev7(i) mod q for i = 0 to 127, transcribed from the text of the
 * PDF and checked against the powers 17^BitRev7(i) mod q computed
 * independently.
 */
static const uint16_t mlkem_zeta[128] = {
	   1U, 1729U, 2580U, 3289U, 2642U,  630U, 1897U,  848U,
	1062U, 1919U,  193U,  797U, 2786U, 3260U,  569U, 1746U,
	 296U, 2447U, 1339U, 1476U, 3046U,   56U, 2240U, 1333U,
	1426U, 2094U,  535U, 2882U, 2393U, 2879U, 1974U,  821U,
	 289U,  331U, 3253U, 1756U, 1197U, 2304U, 2277U, 2055U,
	 650U, 1977U, 2513U,  632U, 2865U,   33U, 1320U, 1915U,
	2319U, 1435U,  807U,  452U, 1438U, 2868U, 1534U, 2402U,
	2647U, 2617U, 1481U,  648U, 2474U, 3110U, 1227U,  910U,
	  17U, 2761U,  583U, 2649U, 1637U,  723U, 2288U, 1100U,
	1409U, 2662U, 3281U,  233U,  756U, 2156U, 3015U, 3050U,
	1703U, 1651U, 2789U, 1789U, 1847U,  952U, 1461U, 2687U,
	 939U, 2308U, 2437U, 2388U,  733U, 2337U,  268U,  641U,
	1584U, 2298U, 2037U, 3220U,  375U, 2549U, 2090U, 1645U,
	1063U,  319U, 2773U,  757U, 2099U,  561U, 2466U, 2594U,
	2804U, 1092U,  403U, 1026U, 1143U, 2150U, 2775U,  886U,
	1722U, 1212U, 1874U, 1029U, 2110U, 2935U,  885U, 2154U
};

static uint32_t mlkem_reduce(uint32_t value);
static uint32_t mlkem_load(int16_t coefficient);
static void mlkem_zero(struct cryc_mlkem_poly *f);

/*
 * NTT(f) in place (Algorithm 9).
 *
 * Constant time: every loop bound and index below is a function of the
 * step number, and every reduction is a Barrett reduction.
 */
void
cryc_mlkem_ntt(
	struct cryc_mlkem_poly *f)
{
	unsigned length;
	unsigned start;
	unsigned position;
	unsigned step;
	uint32_t factor;
	uint32_t low;
	uint32_t high;
	uint32_t product;

	/* Puts every coefficient in range before the butterflies. */
	for (position = 0; position < CRYC_MLKEM_N; position++)
		f->c[position] = (int16_t)mlkem_load(f->c[position]);

	/*
	 * Runs the seven layers of Algorithm 9, from the longest butterfly
	 * span down to 2; the zeta of each block is the next table entry,
	 * counting from 1.
	 */
	step = 1U;
	for (length = 128U; length >= 2U; length >>= 1) {
		for (start = 0; start < CRYC_MLKEM_N; start += 2U * length) {
			factor = mlkem_zeta[step];
			step++;

			/* One butterfly: t = zeta * f[j + len]; f[j + len] = f[j] - t; f[j] = f[j] + t. */
			for (position = start; position < start + length; position++) {
				low = (uint32_t)f->c[position];
				high = (uint32_t)f->c[position + length];
				product = mlkem_reduce(factor * high);
				f->c[position + length] = (int16_t)mlkem_reduce(low + MLKEM_Q - product);
				f->c[position] = (int16_t)mlkem_reduce(low + product);
			}
		}
	}
}

/*
 * NTT^-1(f) in place (Algorithm 10).
 *
 * Constant time, like the forward transform.
 */
void
cryc_mlkem_ntt_inverse(
	struct cryc_mlkem_poly *f)
{
	unsigned length;
	unsigned start;
	unsigned position;
	unsigned step;
	uint32_t factor;
	uint32_t low;
	uint32_t high;

	/* Puts every coefficient in range before the butterflies. */
	for (position = 0; position < CRYC_MLKEM_N; position++)
		f->c[position] = (int16_t)mlkem_load(f->c[position]);

	/*
	 * Runs the seven layers of Algorithm 10, from span 2 up to 128; the
	 * zeta of each block is the next table entry, counting down from
	 * 127.
	 */
	step = 127U;
	for (length = 2U; length <= 128U; length <<= 1) {
		for (start = 0; start < CRYC_MLKEM_N; start += 2U * length) {
			factor = mlkem_zeta[step];
			step--;

			/* One butterfly: t = f[j]; f[j] = t + f[j + len]; f[j + len] = zeta * (f[j + len] - t). */
			for (position = start; position < start + length; position++) {
				low = (uint32_t)f->c[position];
				high = (uint32_t)f->c[position + length];
				f->c[position] = (int16_t)mlkem_reduce(low + high);
				f->c[position + length] = (int16_t)mlkem_reduce(factor * (high + MLKEM_Q - low));
			}
		}
	}

	/* Multiplies every coefficient by 128^-1 mod q (line 14). */
	for (position = 0; position < CRYC_MLKEM_N; position++) {
		low = (uint32_t)f->c[position];
		f->c[position] = (int16_t)mlkem_reduce(low * MLKEM_INVERSE_128);
	}
}

/*
 * h = f * g in T_q (MultiplyNTTs, Algorithm 11, with BaseCaseMultiply,
 * Algorithm 12).
 *
 * The four inputs of a pair are read before its two outputs are written,
 * so h may be f or g.
 */
void
cryc_mlkem_multiply_ntts(
	struct cryc_mlkem_poly *h,
	const struct cryc_mlkem_poly *f,
	const struct cryc_mlkem_poly *g)
{
	unsigned pair;
	uint32_t a0;
	uint32_t a1;
	uint32_t b0;
	uint32_t b1;
	uint32_t gamma;
	uint32_t cross;
	uint32_t c0;
	uint32_t c1;

	/* Multiplies each of the 128 degree-one residues modulo X^2 - gamma. */
	for (pair = 0; pair < 128U; pair++) {
		/* The four inputs of this pair. */
		a0 = mlkem_load(f->c[2U * pair]);
		a1 = mlkem_load(f->c[2U * pair + 1U]);
		b0 = mlkem_load(g->c[2U * pair]);
		b1 = mlkem_load(g->c[2U * pair + 1U]);

		/*
		 * gamma = zeta^(2 BitRev7(pair) + 1): table entry 64 + pair / 2,
		 * negated for an odd pair (Appendix A, second array).  The pair
		 * number is public.
		 */
		gamma = mlkem_zeta[64U + (pair >> 1)];
		if ((pair & 1U) != 0U)
			gamma = MLKEM_Q - gamma;

		/* BaseCaseMultiply: c0 = a0 b0 + a1 b1 gamma, c1 = a0 b1 + a1 b0 (Algorithm 12). */
		cross = mlkem_reduce(a1 * b1);
		cross = mlkem_reduce(cross * gamma);
		c0 = mlkem_reduce(a0 * b0 + cross);
		c1 = mlkem_reduce(a0 * b1 + a1 * b0);

		/* Stores the product pair. */
		h->c[2U * pair] = (int16_t)c0;
		h->c[2U * pair + 1U] = (int16_t)c1;
	}
}

/*
 * Writes ByteEncode_d(f) (Algorithm 5), 32 * d bytes, for d from 1 to 12.
 *
 * The coefficients are written consecutively, d bits each, least
 * significant bit first, and the bit string is cut into bytes least
 * significant bit first (Algorithm 3).  A d outside 1 to 12 writes
 * nothing.
 */
void
cryc_mlkem_byte_encode(
	uint8_t *out,
	const struct cryc_mlkem_poly *f,
	unsigned d)
{
	unsigned index;
	unsigned count;
	unsigned position;
	uint32_t accumulator;
	uint32_t mask;
	uint32_t value;

	/* Refuses a bit width Algorithm 5 does not define. */
	if (d < 1U || d > 12U)
		return;

	/* Packs each coefficient's low d bits into the byte stream. */
	mask = ((uint32_t)1U << d) - 1U;
	accumulator = 0U;
	count = 0U;
	position = 0U;
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		value = (uint32_t)(uint16_t)f->c[index] & mask;
		accumulator |= value << count;
		count += d;

		/* Writes every complete byte. */
		while (count >= 8U) {
			out[position] = (uint8_t)(accumulator & 0xffU);
			position++;
			accumulator >>= 8;
			count -= 8U;
		}
	}
}

/*
 * Reads ByteDecode_d (Algorithm 6) from 32 * d bytes.
 *
 * For d = 12 each 12-bit value is reduced modulo q with one masked
 * subtraction (a 12-bit value is below 2 * q); the return value is 1 when
 * no value needed it.  For d below 12 the values are below 2^d, so below q,
 * and the return value is 1.  A d outside 1 to 12 clears f and returns 0.
 */
int
cryc_mlkem_byte_decode(
	struct cryc_mlkem_poly *f,
	const uint8_t *in,
	unsigned d)
{
	unsigned index;
	unsigned count;
	unsigned position;
	uint32_t accumulator;
	uint32_t mask;
	uint32_t value;
	uint32_t below;
	uint32_t overflow;
	uint32_t clean;

	/* Refuses a bit width Algorithm 6 does not define. */
	if (d < 1U || d > 12U) {
		mlkem_zero(f);
		return 0;
	}

	/* Unpacks d bits per coefficient from the byte stream. */
	mask = ((uint32_t)1U << d) - 1U;
	accumulator = 0U;
	count = 0U;
	position = 0U;
	overflow = 0U;
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		/* Takes in whole bytes until d bits are available. */
		while (count < d) {
			accumulator |= (uint32_t)in[position] << count;
			position++;
			count += 8U;
		}

		/* Takes the low d bits as the next value. */
		value = accumulator & mask;
		accumulator >>= d;
		count -= d;

		/* Reduces a value of q or more and remembers it; for d below 12 this cannot happen. */
		below = cryc_ct_mask_lt32(value, MLKEM_Q);
		overflow |= ~below;
		value -= MLKEM_Q & ~below;
		f->c[index] = (int16_t)value;
	}

	/* Reports 1 when every value was already below q. */
	clean = cryc_ct_mask_zero32(overflow) & 1U;
	return (int)clean;
}

/*
 * Compress_d of every coefficient in place (4.2.1, equation (4.7)), d
 * below 12.
 *
 * Compress_d(x) = round(2^d x / q) mod 2^d with halves rounded up; no half
 * occurs because q is odd.  That is floor((2^d x + 1664) / q) mod 2^d,
 * with the division done by the fixed multiplier.  A d outside 1 to 11
 * leaves f unchanged.
 */
void
cryc_mlkem_compress(
	struct cryc_mlkem_poly *f,
	unsigned d)
{
	unsigned index;
	uint32_t value;
	uint32_t numerator;
	uint32_t quotient;
	uint32_t mask;

	/* Refuses a bit width equation (4.7) does not use. */
	if (d < 1U || d > 11U)
		return;

	/* Compresses each coefficient. */
	mask = ((uint32_t)1U << d) - 1U;
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		value = mlkem_load(f->c[index]);
		numerator = (value << d) + (MLKEM_Q >> 1);
		quotient = (uint32_t)(((uint64_t)numerator * (uint64_t)MLKEM_COMPRESS_DIV) >> 35);
		f->c[index] = (int16_t)(quotient & mask);
	}
}

/*
 * Decompress_d of every coefficient in place (4.2.1, equation (4.8)), d
 * below 12.
 *
 * Decompress_d(y) = round(q y / 2^d) with halves rounded up, that is (q y
 * + 2^(d - 1)) >> d for a y below 2^d (the input is cut to d bits).  A d
 * outside 1 to 11 leaves f unchanged.
 */
void
cryc_mlkem_decompress(
	struct cryc_mlkem_poly *f,
	unsigned d)
{
	unsigned index;
	uint32_t value;
	uint32_t mask;
	uint32_t rounding;

	/* Refuses a bit width equation (4.8) does not use. */
	if (d < 1U || d > 11U)
		return;

	/* Decompresses each coefficient. */
	mask = ((uint32_t)1U << d) - 1U;
	rounding = (uint32_t)1U << (d - 1U);
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		value = (uint32_t)(uint16_t)f->c[index] & mask;
		f->c[index] = (int16_t)((value * MLKEM_Q + rounding) >> d);
	}
}

/*
 * SampleNTT (Algorithm 7) from a 34-byte input rho || j || i, using
 * SHAKE128 as the XOF.
 *
 * The XOF output is read one block at a time (the output is one stream,
 * so reading 168 bytes at once gives the same 3-byte groups as reading 3
 * at a time).  Each group gives two candidates of 12 bits, kept when
 * below q.  The input is public, so the rejection test may branch.  If
 * 64 blocks do not give 256 coefficients (a probability far below 2^-261,
 * Appendix B), f is cleared.
 */
void
cryc_mlkem_sample_ntt(
	struct cryc_mlkem_poly *f,
	const uint8_t *seed34)
{
	struct cryc_sha3 sponge;
	uint8_t block[CRYC_SHAKE128_RATE];
	unsigned count;
	unsigned blocks;
	unsigned group;
	unsigned base;
	uint32_t first;
	uint32_t second;

	/* Starts the XOF and absorbs the 34-byte input. */
	cryc_shake128_init(&sponge);
	cryc_sha3_absorb(&sponge, seed34, 34U);

	/* Reads blocks and parses their 3-byte groups until 256 coefficients are filled. */
	count = 0U;
	for (blocks = 0; blocks < MLKEM_SAMPLE_BLOCKS && count < CRYC_MLKEM_N; blocks++) {
		cryc_shake_squeeze(&sponge, block, sizeof(block));

		/* Parses the groups of this block. */
		for (group = 0; group < MLKEM_SAMPLE_GROUPS && count < CRYC_MLKEM_N; group++) {
			base = group * 3U;

			/* d1 = C[0] + 256 (C[1] mod 16), d2 = C[1] / 16 + 16 C[2] (lines 6 and 7). */
			first = (uint32_t)block[base] + 256U * ((uint32_t)block[base + 1U] & 15U);
			second = ((uint32_t)block[base + 1U] >> 4) + 16U * (uint32_t)block[base + 2U];

			/* Keeps d1 when it is below q (lines 8 to 11). */
			if (first < MLKEM_Q) {
				f->c[count] = (int16_t)first;
				count++;
			}

			/* Keeps d2 when it is below q and there is room (lines 12 to 15). */
			if (second < MLKEM_Q && count < CRYC_MLKEM_N) {
				f->c[count] = (int16_t)second;
				count++;
			}
		}
	}

	/* Gives up cleanly when the bound was reached: the polynomial is cleared. */
	if (count < CRYC_MLKEM_N)
		mlkem_zero(f);

	/* The sponge and the block hold only public values; they are wiped anyway. */
	cryc_sha3_clear(&sponge);
	cryc_wipe(block, sizeof(block));
}

/*
 * SamplePolyCBD_eta (Algorithm 8) from 64 * eta bytes, eta 2 (ML-KEM-768's
 * eta1 and eta2).
 *
 * The coefficient i is x - y mod q where x and y are the sums of the eta
 * bits at 2 i eta and 2 i eta + eta of the bit string (each byte read
 * least significant bit first, Algorithm 4).  The bit positions are
 * functions of i and eta only.  Eta 3 (the other value of the standard) is
 * also supported; any other eta clears f.
 */
void
cryc_mlkem_sample_cbd(
	struct cryc_mlkem_poly *f,
	const uint8_t *bytes,
	unsigned eta)
{
	unsigned index;
	unsigned bit;
	unsigned position;
	uint32_t plus;
	uint32_t minus;
	uint32_t value;

	/* Refuses a parameter Algorithm 8 does not define. */
	if (eta != 2U && eta != 3U) {
		mlkem_zero(f);
		return;
	}

	/* Forms each coefficient from its 2 * eta bits. */
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		plus = 0U;
		minus = 0U;

		/* x is the sum of the first eta bits, y of the next eta (lines 3 and 4). */
		for (bit = 0; bit < eta; bit++) {
			position = 2U * index * eta + bit;
			plus += ((uint32_t)bytes[position >> 3] >> (position & 7U)) & 1U;
			position = 2U * index * eta + eta + bit;
			minus += ((uint32_t)bytes[position >> 3] >> (position & 7U)) & 1U;
		}

		/* x - y mod q, computed as x + q - y and reduced (line 5). */
		value = mlkem_reduce(plus + MLKEM_Q - minus);
		f->c[index] = (int16_t)value;
	}
}

/*
 * Reduces a 32-bit value modulo q into [0, q - 1], in constant time.
 *
 * The quotient estimate is one too small at most, so the remainder
 * estimate is below 2 * q and one masked subtraction finishes it.
 */
static uint32_t
mlkem_reduce(
	uint32_t value)
{
	uint64_t product;
	uint32_t quotient;
	uint32_t remainder;
	uint32_t below;

	/* Estimates the quotient with the fixed multiplier. */
	product = (uint64_t)value * (uint64_t)MLKEM_BARRETT;
	quotient = (uint32_t)(product >> 32);

	/* The remainder estimate, in [0, 2 * q). */
	remainder = value - quotient * MLKEM_Q;

	/* Subtracts q once more exactly when the estimate is q or more. */
	below = cryc_ct_mask_lt32(remainder, MLKEM_Q);
	remainder -= MLKEM_Q & ~below;

	/* Reports the remainder in [0, q - 1]. */
	return remainder;
}

/*
 * Reads a coefficient as a value reduced into [0, q - 1], whatever its
 * stored bits.
 *
 * A coefficient between functions is already in range; a caller's mistake
 * (a negative or large value) gives a defined, reduced result instead of
 * undefined behaviour.
 */
static uint32_t
mlkem_load(
	int16_t coefficient)
{
	uint32_t bits;
	uint32_t reduced;

	/* The stored 16 bits as an unsigned value. */
	bits = (uint32_t)(uint16_t)coefficient;

	/* Puts the value in range. */
	reduced = mlkem_reduce(bits);

	/* Reports the value in [0, q - 1]. */
	return reduced;
}

/* Sets every coefficient of a polynomial to 0. */
static void
mlkem_zero(
	struct cryc_mlkem_poly *f)
{
	unsigned index;

	/* Clears each coefficient. */
	for (index = 0; index < CRYC_MLKEM_N; index++)
		f->c[index] = 0;
}
