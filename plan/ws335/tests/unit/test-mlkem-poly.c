/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the polynomial arithmetic, the encodings and the sampling of
 * ML-KEM (cryc-mlkem.h, FIPS 203 4).
 *
 * The expected values come from the standard and from code written here
 * from it, not from another implementation:
 *
 *   - NTT, NTT^-1 and MultiplyNTTs are compared with the definitions of
 *     4.3: the NTT of f is f modulo X^2 - zeta^(2 BitRev7(i) + 1) for
 *     each i (evaluated with powers of 17 computed here, so a wrong entry
 *     of the implementation's table cannot agree), and the product is
 *     compared with the schoolbook product in R_q.  The second array of
 *     Appendix A is also compared with the NTT of X^2, which is that
 *     array.
 *   - ByteEncode, ByteDecode and SamplePolyCBD are compared with
 *     Algorithms 3 to 6 and 8 written bit by bit; Compress and Decompress
 *     with equations (4.7) and (4.8) evaluated with integer division, for
 *     every coefficient and every d.  SampleNTT is compared with
 *     Algorithm 7 reading the XOF three bytes at a time.
 *   - The published vectors (Wycheproof ML-KEM-768: key generation from a
 *     seed, encapsulation from a message, the modulus-overflow cases) are
 *     checked through K-PKE.KeyGen and K-PKE.Encrypt (Algorithms 13 and 14)
 *     written here from the polynomial functions, so every function of
 *     this module takes part in matching the standard's outputs.
 *
 * Hand-worked examples are marked as such.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-mlkem.h"

/* The modulus, for the test's own arithmetic (which may divide). */
#define Q	3329U

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x2545f491U;

/* Returns the next pseudo-random 32-bit value. */
static uint32_t
random32(void)
{
	/* Marsaglia's xorshift32 with the shifts 13, 17, 5. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Fills size bytes with pseudo-random bytes. */
static void
random_bytes(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte from each draw's high bits. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random32() >> 24);
}

/* Returns base^exponent mod q by square and multiply. */
static uint32_t
power_mod(
	uint32_t base,
	uint32_t exponent)
{
	uint32_t result;

	/* Square and multiply from the low bit. */
	result = 1U;
	base %= Q;
	while (exponent != 0U) {
		if ((exponent & 1U) != 0U)
			result = result * base % Q;
		base = base * base % Q;
		exponent >>= 1;
	}
	return result;
}

/* Returns the 7-bit reversal of value (2.5, BitRev7). */
static unsigned
bit_reverse7(
	unsigned value)
{
	unsigned result;
	unsigned bit;

	/* The lowest bit becomes the highest. */
	result = 0U;
	for (bit = 0; bit < 7U; bit++)
		result |= ((value >> bit) & 1U) << (6U - bit);
	return result;
}

/* Makes a polynomial of random coefficients in [0, q - 1]. */
static void
random_poly(
	struct cryc_mlkem_poly *f)
{
	unsigned index;

	/* Each coefficient from a 32-bit draw. */
	for (index = 0; index < CRYC_MLKEM_N; index++)
		f->c[index] = (int16_t)(random32() % Q);
}

/* Makes a polynomial of random 16-bit patterns, including negative and large values. */
static void
random_wild_poly(
	struct cryc_mlkem_poly *f)
{
	unsigned index;

	/* Any 16 bits. */
	for (index = 0; index < CRYC_MLKEM_N; index++)
		f->c[index] = (int16_t)(uint16_t)(random32() >> 16);
}

/* Reads a polynomial's coefficients as the value each stands for modulo q. */
static void
to_residues(
	uint32_t *out,
	const struct cryc_mlkem_poly *f)
{
	unsigned index;

	/* The stored 16 bits as an unsigned value, then reduced. */
	for (index = 0; index < CRYC_MLKEM_N; index++)
		out[index] = (uint32_t)(uint16_t)f->c[index] % Q;
}

/* Checks that a polynomial equals expected residues, in range; reports the first difference. */
static void
check_poly(
	const struct cryc_mlkem_poly *f,
	const uint32_t *expected,
	const char *what)
{
	unsigned index;
	int same;

	/* Compares every coefficient and notes the first one that differs. */
	same = 1;
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		if ((uint32_t)(uint16_t)f->c[index] != expected[index]) {
			printf("  %s: coefficient %u is %d, expected %lu\n", what, index, (int)f->c[index],
			    (unsigned long)expected[index]);
			same = 0;
			break;
		}
	}
	CHECK(same);
}

/*
 * The NTT written from 4.3: for each i in [0, 127], the pair
 * (f_hat[2i], f_hat[2i + 1]) is f modulo X^2 - gamma with gamma =
 * 17^(2 BitRev7(i) + 1), where X^(2m) = gamma^m and X^(2m + 1) = gamma^m
 * X.
 */
static void
naive_ntt(
	uint32_t *out,
	const uint32_t *in)
{
	unsigned pair;
	unsigned m;
	uint32_t gamma;
	uint32_t power;
	uint32_t even;
	uint32_t odd;

	/* One quadratic modulus after another. */
	for (pair = 0; pair < 128U; pair++) {
		gamma = power_mod(17U, 2U * bit_reverse7(pair) + 1U);
		even = 0U;
		odd = 0U;
		power = 1U;
		for (m = 0; m < 128U; m++) {
			even = (even + in[2U * m] % Q * power) % Q;
			odd = (odd + in[2U * m + 1U] % Q * power) % Q;
			power = power * gamma % Q;
		}
		out[2U * pair] = even;
		out[2U * pair + 1U] = odd;
	}
}

/* The product in R_q = Z_q[X] / (X^256 + 1), by the schoolbook method (2.4.4). */
static void
naive_ring_multiply(
	uint32_t *out,
	const uint32_t *left,
	const uint32_t *right)
{
	unsigned a;
	unsigned b;
	unsigned position;
	uint32_t product;

	/* Clears the result. */
	for (position = 0; position < CRYC_MLKEM_N; position++)
		out[position] = 0U;

	/* Each term; X^256 = -1 turns the terms past the top into negative ones. */
	for (a = 0; a < CRYC_MLKEM_N; a++) {
		for (b = 0; b < CRYC_MLKEM_N; b++) {
			product = left[a] * right[b] % Q;
			position = a + b;
			if (position < CRYC_MLKEM_N)
				out[position] = (out[position] + product) % Q;
			else
				out[position - CRYC_MLKEM_N] = (out[position - CRYC_MLKEM_N] + Q - product) % Q;
		}
	}
}

/*
 * The 128 values of the second array of Appendix A, zeta^(2 BitRev7(i) + 1)
 * mod q for i = 0 to 127 (negative entries as printed), used to check the
 * NTT of X^2, whose even entries are exactly this array.
 *
 * Values referenced: FIPS 203 (August 2024) Appendix A, the array that
 * follows "the values zeta^(2 BitRev7(i) + 1) mod q need to be computed",
 * transcribed from the text of the PDF.
 */
static const int appendix_a_gamma[128] = {
	   17,   -17,  2761, -2761,   583,  -583,  2649, -2649,
	 1637, -1637,   723,  -723,  2288, -2288,  1100, -1100,
	 1409, -1409,  2662, -2662,  3281, -3281,   233,  -233,
	  756,  -756,  2156, -2156,  3015, -3015,  3050, -3050,
	 1703, -1703,  1651, -1651,  2789, -2789,  1789, -1789,
	 1847, -1847,   952,  -952,  1461, -1461,  2687, -2687,
	  939,  -939,  2308, -2308,  2437, -2437,  2388, -2388,
	  733,  -733,  2337, -2337,   268,  -268,   641,  -641,
	 1584, -1584,  2298, -2298,  2037, -2037,  3220, -3220,
	  375,  -375,  2549, -2549,  2090, -2090,  1645, -1645,
	 1063, -1063,   319,  -319,  2773, -2773,   757,  -757,
	 2099, -2099,   561,  -561,  2466, -2466,  2594, -2594,
	 2804, -2804,  1092, -1092,   403,  -403,  1026, -1026,
	 1143, -1143,  2150, -2150,  2775, -2775,   886,  -886,
	 1722, -1722,  1212, -1212,  1874, -1874,  1029, -1029,
	 2110, -2110,  2935, -2935,   885,  -885,  2154, -2154
};

/* Checks the NTT of single powers of X, the second array of Appendix A, and the first values of the first array through the NTT of X^2 and X^3. */
static void
test_ntt_basis(void)
{
	struct cryc_mlkem_poly f;
	uint32_t in[CRYC_MLKEM_N];
	uint32_t expected[CRYC_MLKEM_N];
	unsigned index;
	unsigned position;
	uint32_t want;
	int matches;

	/* The NTT of X^2: the even entries are the second array of Appendix A, the odd entries 0. */
	memset(&f, 0, sizeof(f));
	f.c[2] = 1;
	cryc_mlkem_ntt(&f);
	matches = 1;
	for (index = 0; index < 128U; index++) {
		want = (uint32_t)((appendix_a_gamma[index] + (int)Q) % (int)Q);
		if ((uint32_t)f.c[2U * index] != want || f.c[2U * index + 1U] != 0)
			matches = 0;
	}
	CHECK(matches);

	/* The NTT of X: odd entries 1, even entries 0 (no zeta enters). */
	memset(&f, 0, sizeof(f));
	f.c[1] = 1;
	cryc_mlkem_ntt(&f);
	matches = 1;
	for (index = 0; index < 128U; index++) {
		if (f.c[2U * index] != 0 || f.c[2U * index + 1U] != 1)
			matches = 0;
	}
	CHECK(matches);

	/* The NTT of 1: even entries 1, odd entries 0. */
	memset(&f, 0, sizeof(f));
	f.c[0] = 1;
	cryc_mlkem_ntt(&f);
	matches = 1;
	for (index = 0; index < 128U; index++) {
		if (f.c[2U * index] != 1 || f.c[2U * index + 1U] != 0)
			matches = 0;
	}
	CHECK(matches);

	/* Each delta polynomial against the definition (this covers the whole linear map). */
	for (position = 0; position < CRYC_MLKEM_N; position++) {
		memset(&f, 0, sizeof(f));
		memset(in, 0, sizeof(in));
		f.c[position] = 1;
		in[position] = 1U;
		naive_ntt(expected, in);
		cryc_mlkem_ntt(&f);
		check_poly(&f, expected, "ntt of a delta");
	}

	/* The maximum value q - 1 in each position (a different scaling of each column). */
	for (position = 0; position < CRYC_MLKEM_N; position += 5U) {
		memset(&f, 0, sizeof(f));
		memset(in, 0, sizeof(in));
		f.c[position] = (int16_t)(Q - 1U);
		in[position] = Q - 1U;
		naive_ntt(expected, in);
		cryc_mlkem_ntt(&f);
		check_poly(&f, expected, "ntt of a q-1 delta");
	}
}

/* Checks NTT against the definition on constants, random polynomials and out-of-range coefficients. */
static void
test_ntt_random(void)
{
	struct cryc_mlkem_poly f;
	uint32_t in[CRYC_MLKEM_N];
	uint32_t expected[CRYC_MLKEM_N];
	unsigned round;
	unsigned index;
	int16_t constant;

	/* Constant polynomials: 0, 1, q - 1 and 1664. */
	for (round = 0; round < 4U; round++) {
		constant = round == 0U ? (int16_t)0 : round == 1U ? (int16_t)1 : round == 2U ? (int16_t)(Q - 1U) : (int16_t)1664;
		for (index = 0; index < CRYC_MLKEM_N; index++) {
			f.c[index] = constant;
			in[index] = (uint32_t)constant;
		}
		naive_ntt(expected, in);
		cryc_mlkem_ntt(&f);
		check_poly(&f, expected, "ntt of a constant");
	}

	/* Random polynomials in range. */
	for (round = 0; round < 150U; round++) {
		random_poly(&f);
		to_residues(in, &f);
		naive_ntt(expected, in);
		cryc_mlkem_ntt(&f);
		check_poly(&f, expected, "ntt of a random polynomial");
	}

	/* Coefficients out of range (a caller's mistake) stand for their value mod q, with no undefined behaviour. */
	for (round = 0; round < 40U; round++) {
		random_wild_poly(&f);
		to_residues(in, &f);
		naive_ntt(expected, in);
		cryc_mlkem_ntt(&f);
		check_poly(&f, expected, "ntt of a wild polynomial");
	}
}

/* Checks NTT^-1 against the definition (through the forward definition) and as the inverse of NTT. */
static void
test_ntt_inverse(void)
{
	struct cryc_mlkem_poly f;
	struct cryc_mlkem_poly g;
	uint32_t in[CRYC_MLKEM_N];
	uint32_t forward[CRYC_MLKEM_N];
	uint32_t expected[CRYC_MLKEM_N];
	unsigned round;
	unsigned position;
	unsigned index;
	int in_range;

	/* The inverse of each delta in the NTT domain: its definition-NTT must give the delta back. */
	for (position = 0; position < CRYC_MLKEM_N; position++) {
		memset(&f, 0, sizeof(f));
		f.c[position] = 1;
		cryc_mlkem_ntt_inverse(&f);
		to_residues(in, &f);
		naive_ntt(forward, in);
		for (index = 0; index < CRYC_MLKEM_N; index++)
			expected[index] = index == position ? 1U : 0U;
		for (index = 0; index < CRYC_MLKEM_N; index++)
			g.c[index] = (int16_t)forward[index];
		check_poly(&g, expected, "naive ntt of the inverse of a delta");
	}

	/* The inverse of random NTT-domain values: NTT by the definition gives them back. */
	for (round = 0; round < 100U; round++) {
		random_poly(&f);
		to_residues(expected, &f);
		cryc_mlkem_ntt_inverse(&f);
		in_range = 1;
		for (index = 0; index < CRYC_MLKEM_N; index++) {
			if (f.c[index] < 0 || (uint32_t)f.c[index] >= Q)
				in_range = 0;
		}
		CHECK(in_range);
		to_residues(in, &f);
		naive_ntt(forward, in);
		for (index = 0; index < CRYC_MLKEM_N; index++)
			g.c[index] = (int16_t)forward[index];
		check_poly(&g, expected, "naive ntt of the inverse of random values");
	}

	/* NTT^-1(NTT(f)) = f and NTT(NTT^-1(f)) = f. */
	for (round = 0; round < 100U; round++) {
		random_poly(&f);
		to_residues(expected, &f);
		cryc_mlkem_ntt(&f);
		cryc_mlkem_ntt_inverse(&f);
		check_poly(&f, expected, "inverse of the forward transform");
		random_poly(&f);
		to_residues(expected, &f);
		cryc_mlkem_ntt_inverse(&f);
		cryc_mlkem_ntt(&f);
		check_poly(&f, expected, "forward transform of the inverse");
	}

	/* Out-of-range values stand for their value mod q. */
	for (round = 0; round < 30U; round++) {
		random_wild_poly(&f);
		to_residues(expected, &f);
		cryc_mlkem_ntt_inverse(&f);
		cryc_mlkem_ntt(&f);
		check_poly(&f, expected, "round trip of wild values");
	}
}

/* BaseCaseMultiply of Algorithm 12, written out with gamma from the powers of 17. */
static void
naive_multiply_ntts(
	uint32_t *out,
	const uint32_t *left,
	const uint32_t *right)
{
	unsigned pair;
	uint32_t gamma;
	uint32_t a0;
	uint32_t a1;
	uint32_t b0;
	uint32_t b1;

	/* One degree-one product modulo X^2 - gamma per pair. */
	for (pair = 0; pair < 128U; pair++) {
		gamma = power_mod(17U, 2U * bit_reverse7(pair) + 1U);
		a0 = left[2U * pair] % Q;
		a1 = left[2U * pair + 1U] % Q;
		b0 = right[2U * pair] % Q;
		b1 = right[2U * pair + 1U] % Q;
		out[2U * pair] = (a0 * b0 + a1 * b1 % Q * gamma) % Q;
		out[2U * pair + 1U] = (a0 * b1 + a1 * b0) % Q;
	}
}

/* Checks MultiplyNTTs against Algorithm 12 and, through the transforms, against the schoolbook product in R_q. */
static void
test_multiply(void)
{
	struct cryc_mlkem_poly f;
	struct cryc_mlkem_poly g;
	struct cryc_mlkem_poly h;
	struct cryc_mlkem_poly saved_f;
	struct cryc_mlkem_poly saved_g;
	uint32_t left[CRYC_MLKEM_N];
	uint32_t right[CRYC_MLKEM_N];
	uint32_t expected[CRYC_MLKEM_N];
	unsigned round;
	unsigned a;
	unsigned b;
	unsigned index;

	/* Direct comparison with the base-case formula, with in-range and wild inputs. */
	for (round = 0; round < 120U; round++) {
		if (round < 80U) {
			random_poly(&f);
			random_poly(&g);
		} else {
			random_wild_poly(&f);
			random_wild_poly(&g);
		}
		to_residues(left, &f);
		to_residues(right, &g);
		naive_multiply_ntts(expected, left, right);
		saved_f = f;
		saved_g = g;
		memset(&h, 0x55, sizeof(h));
		cryc_mlkem_multiply_ntts(&h, &f, &g);
		check_poly(&h, expected, "multiply_ntts");

		/* The inputs are not changed. */
		CHECK(memcmp(&f, &saved_f, sizeof(f)) == 0);
		CHECK(memcmp(&g, &saved_g, sizeof(g)) == 0);
	}

	/* The whole chain: NTT^-1(NTT(f) * NTT(g)) equals the schoolbook product. */
	for (round = 0; round < 40U; round++) {
		random_poly(&f);
		random_poly(&g);
		to_residues(left, &f);
		to_residues(right, &g);
		naive_ring_multiply(expected, left, right);
		cryc_mlkem_ntt(&f);
		cryc_mlkem_ntt(&g);
		cryc_mlkem_multiply_ntts(&h, &f, &g);
		cryc_mlkem_ntt_inverse(&h);
		check_poly(&h, expected, "ring product through the NTT");
	}

	/* Monomials: X^a * X^b = X^(a+b), or -X^(a+b-256) past the top (X^256 = -1). */
	for (round = 0; round < 60U; round++) {
		a = round == 0U ? 255U : round == 1U ? 255U : round == 2U ? 0U : random32() % CRYC_MLKEM_N;
		b = round == 0U ? 1U : round == 1U ? 255U : round == 2U ? 0U : random32() % CRYC_MLKEM_N;
		memset(&f, 0, sizeof(f));
		memset(&g, 0, sizeof(g));
		f.c[a] = 1;
		g.c[b] = 1;
		for (index = 0; index < CRYC_MLKEM_N; index++)
			expected[index] = 0U;
		if (a + b < CRYC_MLKEM_N)
			expected[a + b] = 1U;
		else
			expected[a + b - CRYC_MLKEM_N] = Q - 1U;
		cryc_mlkem_ntt(&f);
		cryc_mlkem_ntt(&g);
		cryc_mlkem_multiply_ntts(&h, &f, &g);
		cryc_mlkem_ntt_inverse(&h);
		check_poly(&h, expected, "monomial product");
	}

	/* Multiplying by the NTT of 1 is the identity, by 0 gives 0. */
	for (round = 0; round < 10U; round++) {
		random_poly(&f);
		to_residues(expected, &f);
		memset(&g, 0, sizeof(g));
		g.c[0] = 1;
		cryc_mlkem_ntt(&g);
		cryc_mlkem_ntt(&f);
		saved_f = f;
		cryc_mlkem_multiply_ntts(&h, &f, &g);
		cryc_mlkem_ntt_inverse(&h);
		check_poly(&h, expected, "multiplication by one");
		memset(&g, 0, sizeof(g));
		cryc_mlkem_multiply_ntts(&h, &saved_f, &g);
		for (index = 0; index < CRYC_MLKEM_N; index++)
			expected[index] = 0U;
		check_poly(&h, expected, "multiplication by zero");
	}

	/* Aliasing: the output may be either input or both. */
	for (round = 0; round < 20U; round++) {
		random_poly(&f);
		random_poly(&g);
		saved_f = f;
		saved_g = g;
		to_residues(left, &f);
		to_residues(right, &g);
		naive_multiply_ntts(expected, left, right);
		cryc_mlkem_multiply_ntts(&f, &f, &g);
		check_poly(&f, expected, "multiply with h = f");
		f = saved_f;
		g = saved_g;
		cryc_mlkem_multiply_ntts(&g, &f, &g);
		check_poly(&g, expected, "multiply with h = g");
		f = saved_f;
		to_residues(left, &f);
		naive_multiply_ntts(expected, left, left);
		cryc_mlkem_multiply_ntts(&f, &f, &f);
		check_poly(&f, expected, "multiply with h = f = g");
	}

	/* The product is commutative. */
	random_poly(&f);
	random_poly(&g);
	cryc_mlkem_multiply_ntts(&h, &f, &g);
	saved_f = h;
	cryc_mlkem_multiply_ntts(&h, &g, &f);
	CHECK(memcmp(&h, &saved_f, sizeof(h)) == 0);
}

/* Algorithm 5 written bit by bit with Algorithm 3: values (below 2^d, or q for d = 12) to 32 * d bytes. */
static void
naive_byte_encode(
	uint8_t *out,
	const uint32_t *values,
	unsigned d)
{
	static uint8_t bits[CRYC_MLKEM_N * 12U];
	unsigned index;
	unsigned bit;
	uint32_t value;

	/* The bit string: each value's bits, least significant first (lines 1 to 7). */
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		value = values[index];
		for (bit = 0; bit < d; bit++) {
			bits[index * d + bit] = (uint8_t)(value % 2U);
			value = (value - bits[index * d + bit]) / 2U;
		}
	}

	/* BitsToBytes: bit i of the string is bit i mod 8 of byte i / 8 (Algorithm 3). */
	memset(out, 0, 32U * d);
	for (index = 0; index < CRYC_MLKEM_N * d; index++)
		out[index / 8U] = (uint8_t)(out[index / 8U] + bits[index] * (1U << (index % 8U)));
}

/* Algorithm 6 written bit by bit with Algorithm 4: 32 * d bytes to 256 values mod m; returns 1 when no value is q or more. */
static int
naive_byte_decode(
	uint32_t *values,
	const uint8_t *in,
	unsigned d)
{
	static uint8_t bits[CRYC_MLKEM_N * 12U];
	unsigned index;
	unsigned bit;
	uint32_t value;
	uint32_t modulus;
	int clean;

	/* BytesToBits: bit i of the string is bit i mod 8 of byte i / 8 (Algorithm 4). */
	for (index = 0; index < 32U * d * 8U; index++)
		bits[index] = (uint8_t)((in[index / 8U] >> (index % 8U)) & 1U);

	/* The values: the sum of the d bits times powers of two, mod m (line 3). */
	modulus = d == 12U ? Q : (1U << d);
	clean = 1;
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		value = 0U;
		for (bit = 0; bit < d; bit++)
			value += (uint32_t)bits[index * d + bit] << bit;
		if (value >= modulus)
			clean = 0;
		values[index] = value % modulus;
	}
	return clean;
}

/* Compares a polynomial with expected values (a short form of check_poly for byte-level checks). */
static int
poly_equals(
	const struct cryc_mlkem_poly *f,
	const uint32_t *expected)
{
	unsigned index;

	/* Every coefficient. */
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		if ((uint32_t)(uint16_t)f->c[index] != expected[index])
			return 0;
	}
	return 1;
}

/* Checks ByteEncode on hand-worked examples. */
static void
test_encode_examples(void)
{
	struct cryc_mlkem_poly f;
	uint8_t out[384];
	uint8_t expected[384];

	/*
	 * Hand-worked, d = 1: the coefficients 1, 0, 1, 1, 0, 0, 0, 0 fill
	 * byte 0 least significant bit first (0b00001101 = 0x0d); the
	 * coefficient 8 is 1, so byte 1 is 0x01.
	 */
	memset(&f, 0, sizeof(f));
	f.c[0] = 1;
	f.c[2] = 1;
	f.c[3] = 1;
	f.c[8] = 1;
	memset(out, 0xaa, sizeof(out));
	cryc_mlkem_byte_encode(out, &f, 1U);
	memset(expected, 0, sizeof(expected));
	expected[0] = 0x0dU;
	expected[1] = 0x01U;
	CHECK_BYTES(out, expected, 32U);
	CHECK(out[32] == 0xaaU);

	/* Hand-worked, d = 4: the coefficients 0xA and 0x5 make the byte 0x5A (the first value is the low nibble). */
	memset(&f, 0, sizeof(f));
	f.c[0] = 0xa;
	f.c[1] = 0x5;
	cryc_mlkem_byte_encode(out, &f, 4U);
	memset(expected, 0, sizeof(expected));
	expected[0] = 0x5aU;
	CHECK_BYTES(out, expected, 128U);

	/*
	 * Hand-worked, d = 10: the coefficients 0x3ff, 0x001, 0x200 occupy
	 * bits 0 to 9, 10 to 19 and 20 to 29: byte 0 = 0xff, byte 1 = 0x03 |
	 * 0x04 (bit 10) = 0x07, byte 2 = 0x00, byte 3 = 0x20 (bit 29).
	 */
	memset(&f, 0, sizeof(f));
	f.c[0] = 0x3ff;
	f.c[1] = 0x001;
	f.c[2] = 0x200;
	cryc_mlkem_byte_encode(out, &f, 10U);
	memset(expected, 0, sizeof(expected));
	expected[0] = 0xffU;
	expected[1] = 0x07U;
	expected[3] = 0x20U;
	CHECK_BYTES(out, expected, 320U);

	/* Hand-worked, d = 11: the coefficient 0x7ff then 0 gives 0xff, 0x07. */
	memset(&f, 0, sizeof(f));
	f.c[0] = 0x7ff;
	cryc_mlkem_byte_encode(out, &f, 11U);
	memset(expected, 0, sizeof(expected));
	expected[0] = 0xffU;
	expected[1] = 0x07U;
	CHECK_BYTES(out, expected, 352U);

	/*
	 * Hand-worked, d = 12: the coefficients 0xABC, 0x123, 0x456 give the
	 * bytes BC 3A 12 56 04: byte 0 is the low byte of 0xABC, byte 1 is its
	 * high nibble A and the low nibble of 0x123 (3) shifted up, byte 2 is
	 * the rest of 0x123 (0x12), byte 3 the low byte of 0x456, byte 4 its
	 * high nibble 4.
	 */
	memset(&f, 0, sizeof(f));
	f.c[0] = 0xabc;
	f.c[1] = 0x123;
	f.c[2] = 0x456;
	cryc_mlkem_byte_encode(out, &f, 12U);
	memset(expected, 0, sizeof(expected));
	expected[0] = 0xbcU;
	expected[1] = 0x3aU;
	expected[2] = 0x12U;
	expected[3] = 0x56U;
	expected[4] = 0x04U;
	CHECK_BYTES(out, expected, 384U);

	/* Hand-worked: the last coefficient lands in the last two bytes: d = 12, c[255] = 0xcff gives 0xf0 then 0xcf. */
	memset(&f, 0, sizeof(f));
	f.c[255] = 0xcff;
	cryc_mlkem_byte_encode(out, &f, 12U);
	memset(expected, 0, sizeof(expected));
	expected[382] = 0xf0U;
	expected[383] = 0xcfU;
	CHECK_BYTES(out, expected, 384U);
}

/* Checks ByteDecode on hand-worked examples and the modulus check at its boundary. */
static void
test_decode_examples(void)
{
	struct cryc_mlkem_poly f;
	uint8_t in[384];
	uint32_t expected[CRYC_MLKEM_N];
	unsigned index;
	int clean;

	/* Hand-worked: the bytes BC 3A 12 56 04 are the coefficients 0xABC, 0x123, 0x456 (all below q). */
	memset(in, 0, sizeof(in));
	in[0] = 0xbcU;
	in[1] = 0x3aU;
	in[2] = 0x12U;
	in[3] = 0x56U;
	in[4] = 0x04U;
	clean = cryc_mlkem_byte_decode(&f, in, 12U);
	CHECK(clean == 1);
	CHECK(f.c[0] == 0xabc);
	CHECK(f.c[1] == 0x123);
	CHECK(f.c[2] == 0x456);
	CHECK(f.c[3] == 0);

	/* The largest value below q, 3328 = 0xd00: accepted as is. */
	memset(in, 0, sizeof(in));
	in[0] = 0x00U;
	in[1] = 0x0dU;
	clean = cryc_mlkem_byte_decode(&f, in, 12U);
	CHECK(clean == 1);
	CHECK(f.c[0] == 3328);

	/* q = 3329 = 0xd01: reduced to 0 and flagged. */
	in[0] = 0x01U;
	in[1] = 0x0dU;
	clean = cryc_mlkem_byte_decode(&f, in, 12U);
	CHECK(clean == 0);
	CHECK(f.c[0] == 0);
	CHECK(f.c[1] == 0);

	/* 4095 = 0xfff: reduced to 4095 - 3329 = 766 and flagged. */
	in[0] = 0xffU;
	in[1] = 0x0fU;
	clean = cryc_mlkem_byte_decode(&f, in, 12U);
	CHECK(clean == 0);
	CHECK(f.c[0] == 766);

	/* The overflow in the last coefficient (bytes 382 and 383) is flagged too. */
	memset(in, 0, sizeof(in));
	in[382] = 0x10U;
	in[383] = 0xd0U;
	clean = cryc_mlkem_byte_decode(&f, in, 12U);
	CHECK(clean == 0);
	CHECK(f.c[255] == 0);
	in[382] = 0xf0U;
	in[383] = 0xcfU;
	clean = cryc_mlkem_byte_decode(&f, in, 12U);
	CHECK(clean == 1);
	CHECK(f.c[255] == 3327);

	/* All bytes 0xff: every value is 4095, reduced to 766, and flagged. */
	memset(in, 0xff, sizeof(in));
	clean = cryc_mlkem_byte_decode(&f, in, 12U);
	CHECK(clean == 0);
	for (index = 0; index < CRYC_MLKEM_N; index++)
		expected[index] = 766U;
	CHECK(poly_equals(&f, expected));

	/* d = 1 of 0x0d, 0x01: the coefficients 1, 0, 1, 1, 0, 0, 0, 0, 1, 0, ... */
	memset(in, 0, sizeof(in));
	in[0] = 0x0dU;
	in[1] = 0x01U;
	clean = cryc_mlkem_byte_decode(&f, in, 1U);
	CHECK(clean == 1);
	for (index = 0; index < CRYC_MLKEM_N; index++)
		expected[index] = index == 0U || index == 2U || index == 3U || index == 8U ? 1U : 0U;
	CHECK(poly_equals(&f, expected));

	/* d = 11 with all bytes 0xff: every value 2047 (below q: the flag stays 1). */
	memset(in, 0xff, sizeof(in));
	clean = cryc_mlkem_byte_decode(&f, in, 11U);
	CHECK(clean == 1);
	CHECK(f.c[0] == 2047);
	CHECK(f.c[255] == 2047);
}

/* Checks ByteEncode and ByteDecode against the bit-by-bit algorithms for every d, with exact-size buffers. */
static void
test_encode_decode_random(void)
{
	struct cryc_mlkem_poly f;
	uint32_t values[CRYC_MLKEM_N];
	uint32_t decoded[CRYC_MLKEM_N];
	uint8_t *in;
	uint8_t *out;
	uint8_t expected[384];
	unsigned d;
	unsigned round;
	unsigned index;
	uint32_t limit;
	int clean;
	int naive_clean;

	/* Every bit width, several random inputs each. */
	for (d = 1U; d <= 12U; d++) {
		limit = d == 12U ? Q : (1U << d);
		for (round = 0; round < 20U; round++) {
			/* Encode random values; buffers have exactly 32 * d bytes so a longer write is caught. */
			for (index = 0; index < CRYC_MLKEM_N; index++) {
				values[index] = random32() % limit;
				f.c[index] = (int16_t)values[index];
			}
			out = (uint8_t *)malloc(32U * d);
			CHECK(out != NULL);
			cryc_mlkem_byte_encode(out, &f, d);
			naive_byte_encode(expected, values, d);
			CHECK_BYTES(out, expected, 32U * d);

			/* The round trip. */
			memset(&f, 0x33, sizeof(f));
			clean = cryc_mlkem_byte_decode(&f, out, d);
			CHECK(clean == 1);
			CHECK(poly_equals(&f, values));
			free(out);

			/* Decode random bytes: the values and the flag are those of the algorithm. */
			in = (uint8_t *)malloc(32U * d);
			CHECK(in != NULL);
			random_bytes(in, 32U * d);
			if (round == 0U)
				memset(in, 0xff, 32U * d);
			if (round == 1U)
				memset(in, 0x00, 32U * d);
			naive_clean = naive_byte_decode(decoded, in, d);
			clean = cryc_mlkem_byte_decode(&f, in, d);
			CHECK(clean == naive_clean);
			CHECK(poly_equals(&f, decoded));
			free(in);
		}
	}

	/* One value of q or more, at each of several positions, is flagged and reduced by q; the others are untouched. */
	for (index = 0; index < CRYC_MLKEM_N; index += 17U) {
		for (round = 0; round < CRYC_MLKEM_N; round++)
			values[round] = random32() % Q;
		values[index] = Q + random32() % (4096U - Q);
		for (round = 0; round < CRYC_MLKEM_N; round++)
			f.c[round] = (int16_t)values[round];
		out = (uint8_t *)malloc(384U);
		CHECK(out != NULL);
		cryc_mlkem_byte_encode(out, &f, 12U);
		clean = cryc_mlkem_byte_decode(&f, out, 12U);
		CHECK(clean == 0);
		values[index] -= Q;
		CHECK(poly_equals(&f, values));
		free(out);
	}

	/* Re-encoding a decoded 12-bit array gives the original exactly when it was clean (the check of 7.2). */
	for (round = 0; round < 50U; round++) {
		in = (uint8_t *)malloc(384U);
		out = (uint8_t *)malloc(384U);
		CHECK(in != NULL && out != NULL);
		random_bytes(in, 384U);
		clean = cryc_mlkem_byte_decode(&f, in, 12U);
		cryc_mlkem_byte_encode(out, &f, 12U);
		CHECK((memcmp(in, out, 384U) == 0) == (clean == 1));
		free(in);
		free(out);
	}
}

/* Checks the bit widths that are not defined: encode writes nothing, decode clears and returns 0. */
static void
test_encode_decode_bad_width(void)
{
	static const unsigned widths[] = { 0U, 13U, 16U, 32U, 100U, 0xffffffffU };
	struct cryc_mlkem_poly f;
	uint8_t out[400];
	uint8_t in[400];
	unsigned index;
	unsigned slot;
	int clean;
	int untouched;

	/* Each undefined width. */
	for (index = 0; index < sizeof(widths) / sizeof(widths[0]); index++) {
		random_poly(&f);
		memset(out, 0xa5, sizeof(out));
		cryc_mlkem_byte_encode(out, &f, widths[index]);
		untouched = 1;
		for (slot = 0; slot < sizeof(out); slot++) {
			if (out[slot] != 0xa5U)
				untouched = 0;
		}
		CHECK(untouched);

		memset(in, 0x5a, sizeof(in));
		random_poly(&f);
		clean = cryc_mlkem_byte_decode(&f, in, widths[index]);
		CHECK(clean == 0);
		for (slot = 0; slot < CRYC_MLKEM_N; slot++)
			CHECK(f.c[slot] == 0);
	}
}

/* Checks that encoding takes only the low d bits and decodes consistently with out-of-range coefficients. */
static void
test_encode_wild(void)
{
	struct cryc_mlkem_poly f;
	uint32_t values[CRYC_MLKEM_N];
	uint8_t out[384];
	uint8_t expected[384];
	unsigned d;
	unsigned round;
	unsigned index;

	/* Wild coefficients are cut to d bits. */
	for (d = 1U; d <= 12U; d++) {
		for (round = 0; round < 5U; round++) {
			random_wild_poly(&f);
			for (index = 0; index < CRYC_MLKEM_N; index++)
				values[index] = (uint32_t)(uint16_t)f.c[index] & ((1U << d) - 1U);
			cryc_mlkem_byte_encode(out, &f, d);
			naive_byte_encode(expected, values, d);
			CHECK_BYTES(out, expected, 32U * d);
		}
	}
}

/* Compress_d of one value by equation (4.7): floor(2^d x / q + 1/2) mod 2^d, by integer division. */
static uint32_t
reference_compress(
	uint32_t x,
	unsigned d)
{
	uint32_t numerator;

	/* floor((2^(d+1) x + q) / (2 q)) is the rounded quotient with halves up. */
	numerator = (x << (d + 1U)) + Q;
	return (numerator / (2U * Q)) % (1U << d);
}

/* Decompress_d of one value by equation (4.8): floor(q y / 2^d + 1/2), by integer division. */
static uint32_t
reference_decompress(
	uint32_t y,
	unsigned d)
{
	uint32_t numerator;

	/* floor((2 q y + 2^d) / 2^(d+1)) is the rounded quotient with halves up. */
	numerator = 2U * Q * y + (1U << d);
	return numerator / (2U << d);
}

/* Checks Compress for every coefficient value and every d against equation (4.7). */
static void
test_compress_all(void)
{
	struct cryc_mlkem_poly f;
	unsigned d;
	unsigned base;
	unsigned index;
	uint32_t value;
	int same;

	/* Every d from 1 to 11; the 3329 values go through in blocks of 256. */
	for (d = 1U; d <= 11U; d++) {
		same = 1;
		for (base = 0; base < Q; base += CRYC_MLKEM_N) {
			for (index = 0; index < CRYC_MLKEM_N; index++) {
				value = base + index;
				if (value >= Q)
					value = Q - 1U;
				f.c[index] = (int16_t)value;
			}
			cryc_mlkem_compress(&f, d);
			for (index = 0; index < CRYC_MLKEM_N; index++) {
				value = base + index;
				if (value >= Q)
					value = Q - 1U;
				if ((uint32_t)f.c[index] != reference_compress(value, d))
					same = 0;
			}
		}
		CHECK(same);
	}
}

/* Checks Decompress for every input value and every d against equation (4.8), and the round trips. */
static void
test_decompress_all(void)
{
	struct cryc_mlkem_poly f;
	struct cryc_mlkem_poly g;
	unsigned d;
	unsigned base;
	unsigned index;
	uint32_t value;
	uint32_t limit;
	uint32_t difference;
	int same;
	int trip;
	int close;

	/* Every d from 1 to 11. */
	for (d = 1U; d <= 11U; d++) {
		limit = 1U << d;
		same = 1;
		trip = 1;
		for (base = 0; base < limit; base += CRYC_MLKEM_N) {
			for (index = 0; index < CRYC_MLKEM_N; index++) {
				value = base + index;
				if (value >= limit)
					value = limit - 1U;
				f.c[index] = (int16_t)value;
			}
			cryc_mlkem_decompress(&f, d);
			g = f;
			for (index = 0; index < CRYC_MLKEM_N; index++) {
				value = base + index;
				if (value >= limit)
					value = limit - 1U;
				if ((uint32_t)f.c[index] != reference_decompress(value, d) || (uint32_t)f.c[index] >= Q)
					same = 0;
			}

			/* Compress inverts Decompress exactly (for d below 12 each y survives). */
			cryc_mlkem_compress(&g, d);
			for (index = 0; index < CRYC_MLKEM_N; index++) {
				value = base + index;
				if (value >= limit)
					value = limit - 1U;
				if ((uint32_t)g.c[index] != value)
					trip = 0;
			}
		}
		CHECK(same);
		CHECK(trip);
	}

	/* Decompress then Compress moves a value by at most round(q / 2^(d+1)) (equation (4.9), cyclically mod q). */
	for (d = 1U; d <= 11U; d++) {
		close = 1;
		for (base = 0; base < Q; base += CRYC_MLKEM_N) {
			for (index = 0; index < CRYC_MLKEM_N; index++) {
				value = base + index;
				if (value >= Q)
					value = Q - 1U;
				f.c[index] = (int16_t)value;
			}
			cryc_mlkem_compress(&f, d);
			cryc_mlkem_decompress(&f, d);
			for (index = 0; index < CRYC_MLKEM_N; index++) {
				value = base + index;
				if (value >= Q)
					value = Q - 1U;
				difference = (uint32_t)f.c[index] > value ? (uint32_t)f.c[index] - value : value - (uint32_t)f.c[index];
				if (difference > Q - difference)
					difference = Q - difference;
				if (difference > (Q + (1U << d)) / (2U << d))
					close = 0;
			}
		}
		CHECK(close);
	}
}

/* Checks Compress and Decompress on hand-worked values, out-of-range input and undefined widths. */
static void
test_compress_examples(void)
{
	struct cryc_mlkem_poly f;
	struct cryc_mlkem_poly saved;
	unsigned index;
	unsigned round;
	unsigned d;
	int same;

	/*
	 * Hand-worked Compress_1: 2x/q rounds to 1 for 832.25 < x < 2496.75, so
	 * 832 gives 0, 833 and 2496 give 1, 2497 gives 0.
	 */
	memset(&f, 0, sizeof(f));
	f.c[0] = 832;
	f.c[1] = 833;
	f.c[2] = 2496;
	f.c[3] = 2497;
	cryc_mlkem_compress(&f, 1U);
	CHECK(f.c[0] == 0);
	CHECK(f.c[1] == 1);
	CHECK(f.c[2] == 1);
	CHECK(f.c[3] == 0);

	/* Hand-worked: Compress_4(3328) = round(15.995) mod 16 = 16 mod 16 = 0; Compress_10(3328) = round(1023.7) mod 1024 = 0. */
	memset(&f, 0, sizeof(f));
	f.c[0] = 3328;
	cryc_mlkem_compress(&f, 4U);
	CHECK(f.c[0] == 0);
	f.c[0] = 3328;
	cryc_mlkem_compress(&f, 10U);
	CHECK(f.c[0] == 0);

	/* Hand-worked Decompress_1(1) = round(1664.5) = 1665 (halves up), Decompress_1(0) = 0. */
	memset(&f, 0, sizeof(f));
	f.c[0] = 1;
	cryc_mlkem_decompress(&f, 1U);
	CHECK(f.c[0] == 1665);
	CHECK(f.c[1] == 0);

	/* Hand-worked: Decompress_4(1) = round(208.06) = 208, Decompress_4(8) = round(1664.5) = 1665, Decompress_10(1) = round(3.25) = 3, Decompress_11(1) = round(1.63) = 2. */
	memset(&f, 0, sizeof(f));
	f.c[0] = 1;
	f.c[1] = 8;
	cryc_mlkem_decompress(&f, 4U);
	CHECK(f.c[0] == 208);
	CHECK(f.c[1] == 1665);
	memset(&f, 0, sizeof(f));
	f.c[0] = 1;
	cryc_mlkem_decompress(&f, 10U);
	CHECK(f.c[0] == 3);
	memset(&f, 0, sizeof(f));
	f.c[0] = 1;
	cryc_mlkem_decompress(&f, 11U);
	CHECK(f.c[0] == 2);

	/* A coefficient out of range stands for its value mod q in Compress (and for its low d bits in Decompress), without undefined behaviour. */
	random_wild_poly(&f);
	same = 1;
	cryc_mlkem_compress(&f, 10U);
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		if (f.c[index] < 0 || f.c[index] >= 1024)
			same = 0;
	}
	CHECK(same);
	memset(&f, 0, sizeof(f));
	f.c[0] = (int16_t)0xffff;
	cryc_mlkem_compress(&f, 4U);
	CHECK((uint32_t)f.c[0] == reference_compress(65535U % Q, 4U));
	memset(&f, 0, sizeof(f));
	f.c[0] = -1;
	cryc_mlkem_decompress(&f, 4U);
	CHECK((uint32_t)f.c[0] == reference_decompress(15U, 4U));

	/* Random 16-bit patterns for every width: Compress reduces mod q first, Decompress takes the low d bits. */
	for (round = 0; round < 12U; round++) {
		for (d = 1U; d <= 11U; d++) {
			random_wild_poly(&f);
			saved = f;
			cryc_mlkem_compress(&f, d);
			same = 1;
			for (index = 0; index < CRYC_MLKEM_N; index++) {
				if ((uint32_t)f.c[index] != reference_compress((uint32_t)(uint16_t)saved.c[index] % Q, d))
					same = 0;
			}
			CHECK(same);
			f = saved;
			cryc_mlkem_decompress(&f, d);
			same = 1;
			for (index = 0; index < CRYC_MLKEM_N; index++) {
				if ((uint32_t)f.c[index] != reference_decompress((uint32_t)(uint16_t)saved.c[index] & ((1U << d) - 1U), d))
					same = 0;
			}
			CHECK(same);
		}
	}

	/* The undefined widths leave the polynomial unchanged. */
	for (index = 0; index < 4U; index++) {
		static const unsigned widths[] = { 0U, 12U, 13U, 0xffffffffU };

		random_poly(&f);
		saved = f;
		cryc_mlkem_compress(&f, widths[index]);
		CHECK(memcmp(&f, &saved, sizeof(f)) == 0);
		cryc_mlkem_decompress(&f, widths[index]);
		CHECK(memcmp(&f, &saved, sizeof(f)) == 0);
	}
}

/* SamplePolyCBD of Algorithm 8 written bit by bit with Algorithm 4. */
static void
naive_cbd(
	uint32_t *out,
	const uint8_t *bytes,
	unsigned eta)
{
	static uint8_t bits[64U * 3U * 8U];
	unsigned index;
	unsigned j;
	int x;
	int y;
	int difference;

	/* BytesToBits (Algorithm 4). */
	for (index = 0; index < 64U * eta * 8U; index++)
		bits[index] = (uint8_t)((bytes[index / 8U] >> (index % 8U)) & 1U);

	/* x and y from 2 eta bits per coefficient, f[i] = x - y mod q (lines 2 to 6). */
	for (index = 0; index < CRYC_MLKEM_N; index++) {
		x = 0;
		y = 0;
		for (j = 0; j < eta; j++) {
			x += bits[2U * index * eta + j];
			y += bits[2U * index * eta + eta + j];
		}
		difference = x - y;
		out[index] = difference >= 0 ? (uint32_t)difference : Q - (uint32_t)(-difference);
	}
}

/* Checks SamplePolyCBD on hand-worked examples, all byte values and random inputs for eta 2 and 3. */
static void
test_sample_cbd(void)
{
	struct cryc_mlkem_poly f;
	uint32_t expected[CRYC_MLKEM_N];
	uint8_t *bytes;
	unsigned eta;
	unsigned round;
	unsigned value;
	unsigned index;
	unsigned low;
	unsigned high;
	uint32_t want_low;
	uint32_t want_high;
	static const unsigned bad_eta[] = { 0U, 1U, 4U, 5U, 0xffffffffU };

	/* Hand-worked eta = 2: four bits per coefficient, x from the first two, y from the last two. */
	bytes = (uint8_t *)malloc(128U);
	CHECK(bytes != NULL);
	memset(bytes, 0, 128U);
	bytes[0] = 0x03U;
	bytes[1] = 0x0cU;
	bytes[2] = 0x41U;
	bytes[3] = 0x25U;
	cryc_mlkem_sample_cbd(&f, bytes, 2U);
	CHECK(f.c[0] == 2);
	CHECK(f.c[1] == 0);
	CHECK(f.c[2] == 3327);
	CHECK(f.c[3] == 0);
	CHECK(f.c[4] == 1);
	CHECK(f.c[5] == 3328);
	CHECK(f.c[6] == 0);
	CHECK(f.c[7] == 1);
	CHECK(f.c[8] == 0);
	CHECK(f.c[255] == 0);

	/* Hand-worked: all zero bytes give 0, all 0xff bytes give 2 - 2 = 0 in every coefficient. */
	memset(bytes, 0, 128U);
	cryc_mlkem_sample_cbd(&f, bytes, 2U);
	for (index = 0; index < CRYC_MLKEM_N; index++)
		expected[index] = 0U;
	CHECK(poly_equals(&f, expected));
	memset(bytes, 0xff, 128U);
	cryc_mlkem_sample_cbd(&f, bytes, 2U);
	CHECK(poly_equals(&f, expected));

	/* The extremes: bytes 0x33 give x = 2, y = 0 in every coefficient (+2); bytes 0xcc give -2 = q - 2. */
	memset(bytes, 0x33, 128U);
	cryc_mlkem_sample_cbd(&f, bytes, 2U);
	for (index = 0; index < CRYC_MLKEM_N; index++)
		expected[index] = 2U;
	CHECK(poly_equals(&f, expected));
	memset(bytes, 0xcc, 128U);
	cryc_mlkem_sample_cbd(&f, bytes, 2U);
	for (index = 0; index < CRYC_MLKEM_N; index++)
		expected[index] = Q - 2U;
	CHECK(poly_equals(&f, expected));

	/* Every byte value at byte 0: the two coefficients from its two nibbles, computed by hand-written formulas. */
	for (value = 0; value < 256U; value++) {
		memset(bytes, 0, 128U);
		bytes[0] = (uint8_t)value;
		cryc_mlkem_sample_cbd(&f, bytes, 2U);
		low = value & 15U;
		high = value >> 4;
		want_low = (uint32_t)((int)((low & 1U) + ((low >> 1) & 1U)) - (int)(((low >> 2) & 1U) + ((low >> 3) & 1U)) + (int)Q) % Q;
		want_high = (uint32_t)((int)((high & 1U) + ((high >> 1) & 1U)) - (int)(((high >> 2) & 1U) + ((high >> 3) & 1U)) + (int)Q) % Q;
		CHECK((uint32_t)f.c[0] == want_low);
		CHECK((uint32_t)f.c[1] == want_high);
		CHECK(f.c[2] == 0);
	}
	free(bytes);

	/* Hand-worked eta = 3: six bits per coefficient; byte 0 = 0x07 gives x = 3, y = 0 (+3); byte 0 = 0x38 gives x = 0, y = 3 (-3). */
	bytes = (uint8_t *)malloc(192U);
	CHECK(bytes != NULL);
	memset(bytes, 0, 192U);
	bytes[0] = 0x07U;
	cryc_mlkem_sample_cbd(&f, bytes, 3U);
	CHECK(f.c[0] == 3);
	CHECK(f.c[1] == 0);
	memset(bytes, 0, 192U);
	bytes[0] = 0x38U;
	cryc_mlkem_sample_cbd(&f, bytes, 3U);
	CHECK(f.c[0] == (int16_t)(Q - 3U));
	CHECK(f.c[1] == 0);

	/* Coefficient 1 of eta = 3 starts at bit 6: bits 6 and 7 of byte 0 and bit 0 of byte 1 are its x bits (x = 3 here). */
	memset(bytes, 0, 192U);
	bytes[0] = 0xc0U;
	bytes[1] = 0x01U;
	cryc_mlkem_sample_cbd(&f, bytes, 3U);
	CHECK(f.c[0] == 0);
	CHECK(f.c[1] == 3);
	free(bytes);

	/* Random inputs for both eta against the bit-by-bit algorithm, with exact-size buffers. */
	for (eta = 2U; eta <= 3U; eta++) {
		for (round = 0; round < 100U; round++) {
			bytes = (uint8_t *)malloc(64U * eta);
			CHECK(bytes != NULL);
			random_bytes(bytes, 64U * eta);
			naive_cbd(expected, bytes, eta);
			cryc_mlkem_sample_cbd(&f, bytes, eta);
			CHECK(poly_equals(&f, expected));
			free(bytes);
		}
	}

	/* An eta that the standard does not define clears the polynomial. */
	for (index = 0; index < sizeof(bad_eta) / sizeof(bad_eta[0]); index++) {
		bytes = (uint8_t *)malloc(64U);
		CHECK(bytes != NULL);
		memset(bytes, 0xa7, 64U);
		random_poly(&f);
		cryc_mlkem_sample_cbd(&f, bytes, bad_eta[index]);
		for (round = 0; round < CRYC_MLKEM_N; round++)
			CHECK(f.c[round] == 0);
		free(bytes);
	}
}

/* SampleNTT of Algorithm 7 written from the standard, reading the XOF three bytes at a time. */
static void
naive_sample_ntt(
	uint32_t *out,
	const uint8_t *seed34)
{
	struct cryc_sha3 xof;
	uint8_t group[3];
	unsigned count;
	uint32_t d1;
	uint32_t d2;

	/* ctx <- XOF.Init(), XOF.Absorb(ctx, B) (lines 1 and 2). */
	cryc_shake128_init(&xof);
	cryc_sha3_absorb(&xof, seed34, 34U);

	/* The loop of lines 4 to 16. */
	count = 0U;
	while (count < CRYC_MLKEM_N) {
		cryc_shake_squeeze(&xof, group, 3U);
		d1 = (uint32_t)group[0] + 256U * ((uint32_t)group[1] % 16U);
		d2 = (uint32_t)group[1] / 16U + 16U * (uint32_t)group[2];
		if (d1 < Q) {
			out[count] = d1;
			count++;
		}
		if (d2 < Q && count < CRYC_MLKEM_N) {
			out[count] = d2;
			count++;
		}
	}
	cryc_sha3_clear(&xof);
}

/* Checks SampleNTT against Algorithm 7 on structured and random seeds. */
static void
test_sample_ntt(void)
{
	struct cryc_mlkem_poly f;
	struct cryc_mlkem_poly g;
	uint32_t expected[CRYC_MLKEM_N];
	uint8_t *seed;
	unsigned round;
	unsigned index;
	int in_range;

	/* Structured seeds: all zeros, all ones, and a counting pattern, each with the two index bytes varied. */
	for (round = 0; round < 200U; round++) {
		seed = (uint8_t *)malloc(34U);
		CHECK(seed != NULL);
		if (round < 50U) {
			memset(seed, round % 2U == 0U ? 0x00 : 0xff, 34U);
			seed[32] = (uint8_t)(round / 2U % 3U);
			seed[33] = (uint8_t)(round / 6U % 3U);
		} else if (round < 100U) {
			for (index = 0; index < 34U; index++)
				seed[index] = (uint8_t)(index + round);
		} else {
			random_bytes(seed, 34U);
		}
		naive_sample_ntt(expected, seed);
		memset(&f, 0x77, sizeof(f));
		cryc_mlkem_sample_ntt(&f, seed);
		CHECK(poly_equals(&f, expected));

		/* Every coefficient is below q. */
		in_range = 1;
		for (index = 0; index < CRYC_MLKEM_N; index++) {
			if (f.c[index] < 0 || (uint32_t)f.c[index] >= Q)
				in_range = 0;
		}
		CHECK(in_range);
		free(seed);
	}

	/* Different index bytes give different polynomials (rho || j || i: j and i are the last two bytes). */
	seed = (uint8_t *)malloc(34U);
	CHECK(seed != NULL);
	memset(seed, 0x42, 34U);
	seed[32] = 0;
	seed[33] = 1;
	cryc_mlkem_sample_ntt(&f, seed);
	seed[32] = 1;
	seed[33] = 0;
	cryc_mlkem_sample_ntt(&g, seed);
	CHECK(memcmp(&f, &g, sizeof(f)) != 0);
	free(seed);
}

/* The dimension of the module of ML-KEM-768 (Table 2: k = 3). */
#define K	3U

/* Sets out to H(a) with a of size bytes, H = SHA3-256 (4.1). */
static void
reference_h(
	uint8_t *out,
	const uint8_t *data,
	size_t size)
{
	struct cryc_sha3 sponge;

	/* One absorb, one final. */
	cryc_sha3_256_init(&sponge);
	cryc_sha3_absorb(&sponge, data, size);
	cryc_sha3_final(&sponge, out);
}

/* Sets out (64 bytes) to G(a || b), G = SHA3-512 (4.1). */
static void
reference_g(
	uint8_t *out,
	const uint8_t *first,
	size_t first_size,
	const uint8_t *second,
	size_t second_size)
{
	struct cryc_sha3 sponge;

	/* Two absorbs, one final. */
	cryc_sha3_512_init(&sponge);
	cryc_sha3_absorb(&sponge, first, first_size);
	cryc_sha3_absorb(&sponge, second, second_size);
	cryc_sha3_final(&sponge, out);
}

/* Sets out (128 bytes) to PRF_2(seed, nonce) = SHAKE256(seed || nonce, 128) (4.1, eta 2). */
static void
reference_prf(
	uint8_t *out,
	const uint8_t *seed,
	uint8_t nonce)
{
	struct cryc_sha3 sponge;
	uint8_t input[33];

	/* The seed and the one-byte counter. */
	memcpy(input, seed, 32U);
	input[32] = nonce;
	cryc_shake256_init(&sponge);
	cryc_sha3_absorb(&sponge, input, sizeof(input));
	cryc_shake_squeeze(&sponge, out, 128U);
	cryc_sha3_clear(&sponge);
}

/* Adds two polynomials coefficient by coefficient mod q (2.4.5), result into sum (which may be a). */
static void
poly_add(
	struct cryc_mlkem_poly *sum,
	const struct cryc_mlkem_poly *a,
	const struct cryc_mlkem_poly *b)
{
	unsigned index;

	/* Each coefficient. */
	for (index = 0; index < CRYC_MLKEM_N; index++)
		sum->c[index] = (int16_t)(((uint32_t)a->c[index] + (uint32_t)b->c[index]) % Q);
}

/* Fills the matrix A_hat[i][j] = SampleNTT(rho || j || i) (Algorithm 13 lines 3 to 7). */
static void
reference_matrix(
	struct cryc_mlkem_poly matrix[K][K],
	const uint8_t *rho)
{
	uint8_t seed[34];
	unsigned i;
	unsigned j;

	/* Each entry from its own seed. */
	memcpy(seed, rho, 32U);
	for (i = 0; i < K; i++) {
		for (j = 0; j < K; j++) {
			seed[32] = (uint8_t)j;
			seed[33] = (uint8_t)i;
			cryc_mlkem_sample_ntt(&matrix[i][j], seed);
		}
	}
}

/* K-PKE.KeyGen (Algorithm 13) built from the module's functions: writes ek (1184 bytes) and dk_pke (1152 bytes). */
static void
reference_keygen(
	uint8_t *ek,
	uint8_t *dk_pke,
	const uint8_t *d)
{
	struct cryc_mlkem_poly matrix[K][K];
	struct cryc_mlkem_poly s[K];
	struct cryc_mlkem_poly e[K];
	struct cryc_mlkem_poly t[K];
	struct cryc_mlkem_poly product;
	uint8_t input[33];
	uint8_t expanded[64];
	uint8_t stream[128];
	unsigned i;
	unsigned j;
	uint8_t nonce;

	/* (rho, sigma) = G(d || k), line 1: the byte k = 3 is appended. */
	memcpy(input, d, 32U);
	input[32] = (uint8_t)K;
	reference_g(expanded, input, 33U, NULL, 0U);
	reference_matrix(matrix, expanded);

	/* s and e from the centered binomial distribution, lines 8 to 15. */
	nonce = 0;
	for (i = 0; i < K; i++) {
		reference_prf(stream, expanded + 32, nonce);
		nonce++;
		cryc_mlkem_sample_cbd(&s[i], stream, 2U);
	}
	for (i = 0; i < K; i++) {
		reference_prf(stream, expanded + 32, nonce);
		nonce++;
		cryc_mlkem_sample_cbd(&e[i], stream, 2U);
	}

	/* The transforms, lines 16 and 17. */
	for (i = 0; i < K; i++) {
		cryc_mlkem_ntt(&s[i]);
		cryc_mlkem_ntt(&e[i]);
	}

	/* t_hat = A_hat * s_hat + e_hat, line 18. */
	for (i = 0; i < K; i++) {
		t[i] = e[i];
		for (j = 0; j < K; j++) {
			cryc_mlkem_multiply_ntts(&product, &matrix[i][j], &s[j]);
			poly_add(&t[i], &t[i], &product);
		}
	}

	/* ek = ByteEncode12(t_hat) || rho and dk = ByteEncode12(s_hat), lines 19 and 20. */
	for (i = 0; i < K; i++) {
		cryc_mlkem_byte_encode(ek + 384U * i, &t[i], 12U);
		cryc_mlkem_byte_encode(dk_pke + 384U * i, &s[i], 12U);
	}
	memcpy(ek + 384U * K, expanded, 32U);
}

/* K-PKE.Encrypt (Algorithm 14) built from the module's functions: writes c (1088 bytes); returns 0, or 1 when ek is not a valid key. */
static int
reference_encrypt(
	uint8_t *ciphertext,
	const uint8_t *ek,
	const uint8_t *m,
	const uint8_t *r)
{
	struct cryc_mlkem_poly matrix[K][K];
	struct cryc_mlkem_poly t[K];
	struct cryc_mlkem_poly y[K];
	struct cryc_mlkem_poly e1[K];
	struct cryc_mlkem_poly e2;
	struct cryc_mlkem_poly u;
	struct cryc_mlkem_poly v;
	struct cryc_mlkem_poly mu;
	struct cryc_mlkem_poly product;
	uint8_t stream[128];
	unsigned i;
	unsigned j;
	uint8_t nonce;
	int clean;

	/* t_hat from ek, and the matrix from rho, lines 2 to 8. */
	clean = 1;
	for (i = 0; i < K; i++) {
		if (cryc_mlkem_byte_decode(&t[i], ek + 384U * i, 12U) != 1)
			clean = 0;
	}
	reference_matrix(matrix, ek + 384U * K);

	/* y, e1 and e2 from r, lines 9 to 17. */
	nonce = 0;
	for (i = 0; i < K; i++) {
		reference_prf(stream, r, nonce);
		nonce++;
		cryc_mlkem_sample_cbd(&y[i], stream, 2U);
	}
	for (i = 0; i < K; i++) {
		reference_prf(stream, r, nonce);
		nonce++;
		cryc_mlkem_sample_cbd(&e1[i], stream, 2U);
	}
	reference_prf(stream, r, nonce);
	cryc_mlkem_sample_cbd(&e2, stream, 2U);

	/* y_hat, line 18. */
	for (i = 0; i < K; i++)
		cryc_mlkem_ntt(&y[i]);

	/* mu = Decompress_1(ByteDecode_1(m)), line 20. */
	cryc_mlkem_byte_decode(&mu, m, 1U);
	cryc_mlkem_decompress(&mu, 1U);

	/* u = NTT^-1(A_hat^T * y_hat) + e1, line 19, and c1 = ByteEncode_10(Compress_10(u)), line 22. */
	for (i = 0; i < K; i++) {
		memset(&u, 0, sizeof(u));
		for (j = 0; j < K; j++) {
			cryc_mlkem_multiply_ntts(&product, &matrix[j][i], &y[j]);
			poly_add(&u, &u, &product);
		}
		cryc_mlkem_ntt_inverse(&u);
		poly_add(&u, &u, &e1[i]);
		cryc_mlkem_compress(&u, 10U);
		cryc_mlkem_byte_encode(ciphertext + 320U * i, &u, 10U);
	}

	/* v = NTT^-1(t_hat^T * y_hat) + e2 + mu, line 21, and c2 = ByteEncode_4(Compress_4(v)), line 23. */
	memset(&v, 0, sizeof(v));
	for (i = 0; i < K; i++) {
		cryc_mlkem_multiply_ntts(&product, &t[i], &y[i]);
		poly_add(&v, &v, &product);
	}
	cryc_mlkem_ntt_inverse(&v);
	poly_add(&v, &v, &e2);
	poly_add(&v, &v, &mu);
	cryc_mlkem_compress(&v, 4U);
	cryc_mlkem_byte_encode(ciphertext + 320U * K, &v, 4U);

	return clean ? 0 : 1;
}

/* Checks key generation from the seeds of one record file and returns how many records had both a seed and an ek. */
static unsigned
check_keygen_file(
	const char *name,
	int check_dk)
{
	struct vec_file file;
	uint8_t seed[64];
	uint8_t ek[1184];
	uint8_t dk[2400];
	uint8_t made_ek[1184];
	uint8_t made_dk_pke[1152];
	uint8_t hash[32];
	size_t seed_size;
	size_t ek_size;
	size_t dk_size;
	const char *result;
	unsigned count;

	/* Opens the vectors. */
	count = 0U;
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return 0U;
	}

	/* Each record with a seed and an encapsulation key. */
	while (vec_next(&file)) {
		result = vec_get(&file, "result");
		seed_size = vec_hex(&file, "seed", seed, sizeof(seed));
		ek_size = vec_hex(&file, "ek", ek, sizeof(ek));
		if (result == NULL || strcmp(result, "valid") != 0 || seed_size != 64U || ek_size != 1184U)
			continue;
		count++;

		/* ek = ByteEncode12(t_hat) || rho from the seed d. */
		reference_keygen(made_ek, made_dk_pke, seed);
		CHECK_BYTES(made_ek, ek, 1184U);

		/* dk = dk_pke || ek || H(ek) || z, where the seed is d || z (Algorithm 16 line 3). */
		if (check_dk) {
			dk_size = vec_hex(&file, "dk", dk, sizeof(dk));
			CHECK(dk_size == 2400U);
			if (dk_size == 2400U) {
				CHECK_BYTES(made_dk_pke, dk, 1152U);
				CHECK_BYTES(ek, dk + 1152, 1184U);
				reference_h(hash, ek, 1184U);
				CHECK_BYTES(hash, dk + 2336, 32U);
				CHECK_BYTES(seed + 32, dk + 2368, 32U);
			}
		}
	}
	vec_close(&file);
	return count;
}

/* Checks the Wycheproof key generation vectors (100 seeds, FIPS 203 round 3 seeds and others) and the seed cases of the ML-KEM-768 test file. */
static void
test_vectors_keygen(void)
{
	unsigned count;

	/* The key generation file: every record is valid and complete. */
	count = check_keygen_file("wycheproof-mlkem_768_keygen_seed_test", 1);
	CHECK(count == 100U);

	/* The general file: the records that carry both a seed and an ek. */
	count = check_keygen_file("wycheproof-mlkem_768_test", 0);
	CHECK(count > 100U);
}

/* Checks the encapsulation vectors: valid ones through K-PKE.Encrypt and G, modulus-overflow ones through the decode check. */
static void
test_vectors_encaps(void)
{
	struct vec_file file;
	uint8_t m[32];
	uint8_t ek[1184];
	uint8_t c[1088];
	uint8_t key[32];
	uint8_t made_c[1088];
	uint8_t hash[32];
	uint8_t expanded[64];
	struct cryc_mlkem_poly poly;
	const char *result;
	const char *flags;
	size_t m_size;
	size_t ek_size;
	size_t c_size;
	size_t key_size;
	unsigned valid;
	unsigned overflow;
	unsigned other;
	unsigned i;
	int clean;
	int refused;

	/* Opens the vectors. */
	valid = 0U;
	overflow = 0U;
	other = 0U;
	if (vec_open(&file, "wycheproof-mlkem_768_encaps_test") != 0) {
		CHECK(0);
		return;
	}

	/* Each record. */
	while (vec_next(&file)) {
		result = vec_get(&file, "result");
		flags = vec_get(&file, "flags");
		m_size = vec_hex(&file, "m", m, sizeof(m));
		ek_size = vec_hex(&file, "ek", ek, sizeof(ek));
		if (result == NULL)
			continue;

		/* A valid case: (K, r) = G(m || H(ek)), c = K-PKE.Encrypt(ek, m, r) (Algorithm 17). */
		if (strcmp(result, "valid") == 0) {
			c_size = vec_hex(&file, "c", c, sizeof(c));
			key_size = vec_hex(&file, "K", key, sizeof(key));
			CHECK(m_size == 32U && ek_size == 1184U && c_size == 1088U && key_size == 32U);
			if (m_size != 32U || ek_size != 1184U || c_size != 1088U || key_size != 32U)
				continue;
			valid++;
			reference_h(hash, ek, 1184U);
			reference_g(expanded, m, 32U, hash, 32U);
			CHECK(reference_encrypt(made_c, ek, m, expanded + 32) == 0);
			CHECK_BYTES(made_c, c, 1088U);
			CHECK_BYTES(expanded, key, 32U);
			continue;
		}

		/* A modulus-overflow case: one of the 12-bit polynomials of ek has a value of q or more (7.2). */
		if (flags != NULL && strcmp(flags, "ModulusOverflow") == 0 && ek_size == 1184U) {
			overflow++;
			refused = 0;
			for (i = 0; i < K; i++) {
				clean = cryc_mlkem_byte_decode(&poly, ek + 384U * i, 12U);
				if (clean == 0)
					refused = 1;
			}
			CHECK(refused);
			continue;
		}
		other++;
	}
	vec_close(&file);

	/* The file holds 133 valid cases and 12 modulus-overflow ones. */
	CHECK(valid == 133U);
	CHECK(overflow == 12U);
	CHECK(other + valid + overflow > 0U);
}

/* Checks that every valid key of the vector files decodes clean and re-encodes to itself. */
static void
test_vectors_ek_roundtrip(void)
{
	struct vec_file file;
	uint8_t ek[1184];
	uint8_t again[384];
	struct cryc_mlkem_poly poly;
	size_t ek_size;
	const char *result;
	unsigned count;
	unsigned i;
	int clean;
	int same;

	/* Opens the vectors. */
	count = 0U;
	if (vec_open(&file, "wycheproof-mlkem_768_keygen_seed_test") != 0) {
		CHECK(0);
		return;
	}

	/* The ek of each key generation case: the three 12-bit polynomials decode clean and encode back. */
	while (vec_next(&file)) {
		result = vec_get(&file, "result");
		ek_size = vec_hex(&file, "ek", ek, sizeof(ek));
		if (result == NULL || strcmp(result, "valid") != 0 || ek_size != 1184U)
			continue;
		count++;
		same = 1;
		clean = 1;
		for (i = 0; i < K; i++) {
			if (cryc_mlkem_byte_decode(&poly, ek + 384U * i, 12U) != 1)
				clean = 0;
			cryc_mlkem_byte_encode(again, &poly, 12U);
			if (memcmp(again, ek + 384U * i, 384U) != 0)
				same = 0;
		}
		CHECK(clean);
		CHECK(same);
	}
	vec_close(&file);
	CHECK(count == 100U);
}

/* Runs the checks. */
int
main(void)
{
	/* The transforms and the product. */
	test_ntt_basis();
	test_ntt_random();
	test_ntt_inverse();
	test_multiply();

	/* The encodings. */
	test_encode_examples();
	test_decode_examples();
	test_encode_decode_random();
	test_encode_decode_bad_width();
	test_encode_wild();

	/* Compress and Decompress. */
	test_compress_all();
	test_decompress_all();
	test_compress_examples();

	/* The samplers. */
	test_sample_cbd();
	test_sample_ntt();

	/* The published vectors through K-PKE built from the module's functions. */
	test_vectors_keygen();
	test_vectors_encaps();
	test_vectors_ek_roundtrip();

	/* The result. */
	return check_finish("test-mlkem-poly");
}
