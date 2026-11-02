/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Fixed-width integers and Montgomery arithmetic.
 *
 * A number is limbs words of 32 bits, least significant first.  Every
 * loop below runs for the width, never for the value, and every mask is
 * built from arithmetic, so the time and the addresses touched depend
 * only on the widths.  A local buffer that holds a value which may be
 * secret is wiped before the function returns.
 *
 * A mask that selects or clears by a secret goes through bn_opaque first:
 * an optimizer that can see a mask is 0 or all ones (it is made from a
 * carry or a borrow here) may otherwise turn the bitwise selection into a
 * branch or into a choice of which buffer to load from (clang 19 at -O2
 * and -O3 did both; Valgrind's memcheck with the secret marked undefined
 * shows it).
 */

#include "cryc-bn.h"

/* Forward declarations. */
static uint32_t bn_add(uint32_t *out, const uint32_t *a, const uint32_t *b, size_t limbs);
static uint32_t bn_sub(uint32_t *out, const uint32_t *a, const uint32_t *b, size_t limbs);
static uint32_t bn_borrow(const uint32_t *a, const uint32_t *b, size_t limbs);
static uint32_t bn_mask_nonzero(uint32_t value);
static uint32_t bn_opaque(uint32_t value);

/*
 * Prepares the modulus given as size big-endian bytes (leading zero bytes
 * allowed).
 */
int
cryc_bn_mont_init(
	struct cryc_bn_mont *mont,
	const uint8_t *modulus,
	size_t size)
{
	size_t start;
	size_t significant;
	size_t bits;
	size_t limbs;
	size_t doublings;
	size_t index;
	uint32_t top_byte;
	uint32_t inverse;
	uint32_t m_low;
	int top_bits;
	int error;
	int step;

	/* Starts from a zeroed result, which is what every error leaves. */
	cryc_wipe(mont, sizeof(*mont));

	/* Finds the first non-zero byte: leading zero bytes carry no value. */
	start = 0;
	while (start < size && modulus[start] == 0)
		start++;

	/* Refuses a modulus of zero (every byte was zero, or size is 0). */
	if (start == size)
		return EINVAL;

	/* Refuses an even modulus, which has no inverse of 2^32. */
	if ((modulus[size - 1U] & 1U) == 0U)
		return EINVAL;

	/* Refuses the modulus 1, the only odd number below 3. */
	significant = size - start;
	if (significant == 1U && modulus[start] == 1U)
		return EINVAL;

	/* Refuses a modulus that is certainly wider than the largest allowed. */
	if (significant > CRYC_BN_MAX_BITS / 8U + 1U)
		return ENOTSUP;

	/* Counts the bits of the first byte, then of the whole modulus. */
	top_byte = modulus[start];
	top_bits = 0;
	while (top_byte != 0U) {
		top_bits++;
		top_byte >>= 1;
	}

	/* Totals the bits of the modulus. */
	bits = (significant - 1U) * 8U + (size_t)top_bits;

	/* Refuses a modulus wider than the largest allowed. */
	if (bits > CRYC_BN_MAX_BITS)
		return ENOTSUP;

	/* Reads the modulus into limbs, which cannot fail since it fits. */
	limbs = (bits + 31U) / 32U;
	error = cryc_bn_decode(mont->modulus, limbs, modulus + start, significant);

	/* Undoes the partial result if the read failed, which the size checks above rule out. */
	if (error != 0) {
		cryc_wipe(mont, sizeof(*mont));
		return EINVAL;
	}

	/* Records the size of the modulus. */
	mont->limbs = limbs;
	mont->bits = bits;

	/*
	 * Finds the inverse of the lowest limb modulo 2^32 by Newton's
	 * iteration: an odd number is its own inverse modulo 8 (three correct
	 * bits), and each step doubles the correct bits (6, 12, 24, 48), so
	 * four steps give the 32 that are needed.  The per-word factor is its
	 * negative.
	 */
	m_low = mont->modulus[0];
	inverse = m_low;
	for (step = 0; step < 4; step++)
		inverse = inverse * (2U - m_low * inverse);
	mont->m0inv = 0U - inverse;

	/*
	 * Finds R^2 mod m by doubling: 2^(bits - 1) is below m (m is odd,
	 * so not a power of two), and doubling it modulo m until the
	 * exponent is 64 * limbs gives 2^(64 * limbs) = R^2.  The modulus is
	 * public, so the loop may run for as long as it needs.
	 */
	mont->r2[(bits - 1U) / 32U] = (uint32_t)1U << ((bits - 1U) % 32U);
	doublings = 64U * limbs - (bits - 1U);
	for (index = 0; index < doublings; index++)
		cryc_bn_mod_add(mont->r2, mont->r2, mont->r2, mont);

	/* Succeeded: the modulus is ready for Montgomery arithmetic. */
	return 0;
}

/*
 * Reads size big-endian bytes into out (limbs limbs).
 */
int
cryc_bn_decode(
	uint32_t *out,
	size_t limbs,
	const uint8_t *in,
	size_t size)
{
	size_t position;
	size_t index;
	uint32_t excess;
	uint32_t value;
	uint32_t keep;
	int error;

	/* Clears the result, so that a value shorter than the width has zero high limbs. */
	for (index = 0; index < limbs; index++)
		out[index] = 0;

	/*
	 * Takes the bytes from the least significant end.  A byte inside the
	 * width goes into its limb; a byte beyond the width is gathered into
	 * excess, which stays zero when the value fits.  Which case a
	 * position is in depends only on the sizes.
	 */
	excess = 0;
	for (position = 0; position < size; position++) {
		value = in[size - 1U - position];

		/* Places a byte inside the width in its limb, and gathers one beyond it. */
		if (position / 4U < limbs)
			out[position / 4U] |= value << (8U * (position % 4U));
		else
			excess |= value;
	}

	/* Zeroes the result when the value did not fit, without branching on the bytes. */
	keep = cryc_ct_mask_zero32(excess);
	for (index = 0; index < limbs; index++)
		out[index] &= keep;
	error = (int)((uint32_t)EINVAL & ~keep);

	/* Reports whether the value fitted. */
	if (error != 0)
		return error;

	/* Succeeded: the value is in out. */
	return 0;
}

/*
 * Writes the low size * 8 bits of in (limbs limbs) big-endian into size
 * bytes at out (zero-padded on the left when size * 8 is more than the
 * width).
 */
void
cryc_bn_encode(
	const uint32_t *in,
	size_t limbs,
	uint8_t *out,
	size_t size)
{
	size_t position;
	uint32_t value;

	/*
	 * Writes the bytes from the least significant end.  A position past
	 * the width is zero; which case a position is in depends only on
	 * the sizes.
	 */
	for (position = 0; position < size; position++) {
		value = 0;

		/* Reads the byte from its limb when inside the width, and writes zero beyond it. */
		if (position / 4U < limbs)
			value = in[position / 4U] >> (8U * (position % 4U));
		out[size - 1U - position] = (uint8_t)(value & 0xffU);
	}
}

/*
 * Returns 0xffffffff when a < b and 0 otherwise (limbs limbs each), in
 * constant time.
 */
uint32_t
cryc_bn_lt(
	const uint32_t *a,
	const uint32_t *b,
	size_t limbs)
{
	uint32_t borrow;

	/* Finds whether a - b borrows out of the top limb, which is a < b. */
	borrow = bn_borrow(a, b, limbs);

	/* Reports the borrow as a mask. */
	return 0U - borrow;
}

/*
 * Returns 0xffffffff when a is zero and 0 otherwise, in constant time.
 */
uint32_t
cryc_bn_is_zero(
	const uint32_t *a,
	size_t limbs)
{
	size_t index;
	uint32_t any;

	/* Gathers every bit of every limb. */
	any = 0;
	for (index = 0; index < limbs; index++)
		any |= a[index];

	/* Reports whether nothing was set. */
	return cryc_ct_mask_zero32(any);
}

/*
 * Sets out to a where mask is 0xffffffff and to b where it is 0 (limbs
 * limbs; out may alias either), in constant time.
 */
void
cryc_bn_select(
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b,
	size_t limbs,
	uint32_t mask)
{
	size_t index;

	/* Takes each limb from a or from b by the mask, whose value the optimizer may not use. */
	mask = bn_opaque(mask);
	for (index = 0; index < limbs; index++)
		out[index] = (a[index] & mask) | (b[index] & ~mask);
}

/*
 * out = (a + b) mod m, for a and b less than m (mont->limbs limbs).
 */
void
cryc_bn_mod_add(
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b,
	const struct cryc_bn_mont *mont)
{
	uint32_t diff[CRYC_BN_MAX_LIMBS];
	size_t limbs;
	uint32_t carry;
	uint32_t borrow;
	uint32_t mask;

	/* Forms the sum a + b and keeps its carry out of the top limb. */
	limbs = mont->limbs;
	carry = bn_add(out, a, b, limbs);

	/* Forms the sum minus m, which is the answer when the sum reached m. */
	borrow = bn_sub(diff, out, mont->modulus, limbs);

	/* The sum reached m when it carried out, or when subtracting m did not borrow. */
	mask = bn_mask_nonzero(carry | (borrow ^ 1U));
	cryc_bn_select(out, diff, out, limbs, mask);

	/* Wipes the copy of a value that may be secret. */
	cryc_wipe(diff, limbs * sizeof(diff[0]));
}

/*
 * out = (a - b) mod m, for a and b less than m; aliasing and timing as
 * cryc_bn_mod_add.
 */
void
cryc_bn_mod_sub(
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b,
	const struct cryc_bn_mont *mont)
{
	uint32_t addend[CRYC_BN_MAX_LIMBS];
	size_t limbs;
	size_t index;
	uint32_t borrow;
	uint32_t mask;

	/* Forms a - b and keeps its borrow, which says the difference is negative. */
	limbs = mont->limbs;
	borrow = bn_sub(out, a, b, limbs);

	/* Takes m where the difference was negative and zero where it was not. */
	mask = bn_opaque(0U - borrow);
	for (index = 0; index < limbs; index++)
		addend[index] = mont->modulus[index] & mask;

	/* Adds m back to a negative difference, which wraps it into the range (the carry out is that wrap). */
	bn_add(out, out, addend, limbs);

	/* Wipes the copy of a value that may be secret (m, or zero). */
	cryc_wipe(addend, limbs * sizeof(addend[0]));
}

/*
 * out = a mod m for a less than 2m (one conditional subtraction, by mask):
 * reduces a hash taken as an integer of the order's bit length (FIPS 186-5
 * 6.4.2 step 3's e, derived from the leftmost bits of the hash) into the
 * range.
 */
void
cryc_bn_reduce_once(
	uint32_t *out,
	const uint32_t *a,
	const struct cryc_bn_mont *mont)
{
	uint32_t diff[CRYC_BN_MAX_LIMBS];
	size_t limbs;
	uint32_t borrow;
	uint32_t mask;

	/* Forms a - m, which is the answer when a is not below m. */
	limbs = mont->limbs;
	borrow = bn_sub(diff, a, mont->modulus, limbs);

	/* Takes the difference when subtracting m did not borrow. */
	mask = 0U - (borrow ^ 1U);
	cryc_bn_select(out, diff, a, limbs, mask);

	/* Wipes the copy of a value that may be secret. */
	cryc_wipe(diff, limbs * sizeof(diff[0]));
}

/*
 * out = a * b * R^-1 mod m (Montgomery multiplication), for a and b less
 * than m.
 */
void
cryc_bn_mont_mul(
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b,
	const struct cryc_bn_mont *mont)
{
	uint32_t total[CRYC_BN_MAX_LIMBS + 2];
	uint32_t diff[CRYC_BN_MAX_LIMBS];
	size_t limbs;
	size_t row;
	size_t column;
	uint64_t accumulator;
	uint32_t carry;
	uint32_t factor;
	uint32_t borrow;
	uint32_t mask;

	/* Starts the running total at zero; it has two words more than the modulus. */
	limbs = mont->limbs;
	for (column = 0; column < limbs + 2U; column++)
		total[column] = 0;

	/*
	 * Takes one word of a at a time (REDC in its word-by-word form):
	 * adds that word times b, then adds the multiple of m that clears the
	 * lowest word of the total, and drops that word.  After every row
	 * the total is below 2m.
	 */
	for (row = 0; row < limbs; row++) {
		/* Adds a[row] * b to the total. */
		carry = 0;
		for (column = 0; column < limbs; column++) {
			accumulator = (uint64_t)a[row] * b[column] + total[column] + carry;
			total[column] = (uint32_t)accumulator;
			carry = (uint32_t)(accumulator >> 32);
		}

		/* Takes the carry into the top two words of the total. */
		accumulator = (uint64_t)total[limbs] + carry;
		total[limbs] = (uint32_t)accumulator;
		total[limbs + 1U] = (uint32_t)(accumulator >> 32);

		/* Chooses the multiple of m whose lowest word cancels the total's. */
		factor = total[0] * mont->m0inv;

		/* Adds factor * m, whose lowest word cancels, and shifts the total down one word. */
		accumulator = (uint64_t)factor * mont->modulus[0] + total[0];
		carry = (uint32_t)(accumulator >> 32);
		for (column = 1; column < limbs; column++) {
			accumulator = (uint64_t)factor * mont->modulus[column] + total[column] + carry;
			total[column - 1U] = (uint32_t)accumulator;
			carry = (uint32_t)(accumulator >> 32);
		}

		/* Takes the last carry into the top words of the total. */
		accumulator = (uint64_t)total[limbs] + carry;
		total[limbs - 1U] = (uint32_t)accumulator;
		carry = (uint32_t)(accumulator >> 32);
		total[limbs] = total[limbs + 1U] + carry;
		total[limbs + 1U] = 0;
	}

	/* Forms the total minus m, which is the answer when the total reached m. */
	borrow = bn_sub(diff, total, mont->modulus, limbs);

	/* The total reached m when it has a word above the width, or when subtracting m did not borrow. */
	mask = bn_mask_nonzero(total[limbs] | (borrow ^ 1U));
	cryc_bn_select(out, diff, total, limbs, mask);

	/* Wipes the copies of values that may be secret. */
	cryc_wipe(total, (limbs + 2U) * sizeof(total[0]));
	cryc_wipe(diff, limbs * sizeof(diff[0]));
}

/*
 * out = a * R mod m (into Montgomery form), for a less than m; constant
 * time.
 */
void
cryc_bn_to_mont(
	uint32_t *out,
	const uint32_t *a,
	const struct cryc_bn_mont *mont)
{
	/* Multiplies by R^2, which Montgomery multiplication divides by R. */
	cryc_bn_mont_mul(out, a, mont->r2, mont);
}

/*
 * out = a * R^-1 mod m (out of Montgomery form), for a less than m;
 * constant time.
 */
void
cryc_bn_from_mont(
	uint32_t *out,
	const uint32_t *a,
	const struct cryc_bn_mont *mont)
{
	uint32_t one[CRYC_BN_MAX_LIMBS];
	size_t index;

	/* Builds the number 1 in the width of the modulus. */
	for (index = 0; index < mont->limbs; index++)
		one[index] = 0;
	one[0] = 1U;

	/* Multiplies by 1, which Montgomery multiplication divides by R. */
	cryc_bn_mont_mul(out, a, one, mont);
}

/*
 * out = R mod m (the Montgomery form of 1).
 */
void
cryc_bn_mont_one(
	uint32_t *out,
	const struct cryc_bn_mont *mont)
{
	uint32_t one[CRYC_BN_MAX_LIMBS];
	size_t index;

	/* Builds the number 1 in the width of the modulus. */
	for (index = 0; index < mont->limbs; index++)
		one[index] = 0;
	one[0] = 1U;

	/* Brings 1 into Montgomery form. */
	cryc_bn_to_mont(out, one, mont);
}

/*
 * out = base^exponent in Montgomery form, for base in Montgomery form and
 * less than m; exponent is size big-endian bytes (leading zeros allowed;
 * an exponent of 0 gives the Montgomery form of 1).
 */
void
cryc_bn_mont_exp(
	uint32_t *out,
	const uint32_t *base,
	const uint8_t *exponent,
	size_t size,
	const struct cryc_bn_mont *mont)
{
	uint32_t power[CRYC_BN_MAX_LIMBS];
	uint32_t factor[CRYC_BN_MAX_LIMBS];
	size_t limbs;
	size_t index;
	int bit;
	int started;

	/* Keeps a copy of the base, since out may be the base. */
	limbs = mont->limbs;
	for (index = 0; index < limbs; index++)
		factor[index] = base[index];

	/* Starts the power at 1 in Montgomery form. */
	cryc_bn_mont_one(power, mont);

	/*
	 * Runs square-and-multiply from the top bit of the exponent, which
	 * is public.  Squaring is skipped until the first set bit, where the
	 * power is still 1.
	 */
	started = 0;
	for (index = 0; index < size; index++) {
		for (bit = 7; bit >= 0; bit--) {
			/* Squares the power once the first set bit has been seen. */
			if (started != 0)
				cryc_bn_mont_mul(power, power, power, mont);

			/* Multiplies in the base for a set bit of the (public) exponent. */
			if (((exponent[index] >> bit) & 1U) != 0U) {
				cryc_bn_mont_mul(power, power, factor, mont);
				started = 1;
			}
		}
	}

	/* Publishes the power. */
	for (index = 0; index < limbs; index++)
		out[index] = power[index];

	/* Wipes the copies of values that may be secret. */
	cryc_wipe(power, limbs * sizeof(power[0]));
	cryc_wipe(factor, limbs * sizeof(factor[0]));
}

/*
 * Adds b to a (limbs limbs), writes the sum to out (which may be a or b),
 * and returns the carry out of the top limb (0 or 1).
 */
static uint32_t
bn_add(
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b,
	size_t limbs)
{
	size_t index;
	uint64_t sum;
	uint32_t carry;

	/* Adds limb by limb, carrying upward. */
	carry = 0;
	for (index = 0; index < limbs; index++) {
		sum = (uint64_t)a[index] + b[index] + carry;
		out[index] = (uint32_t)sum;
		carry = (uint32_t)(sum >> 32);
	}

	/* Reports the carry out of the top. */
	return carry;
}

/*
 * Subtracts b from a (limbs limbs), writes the difference to out (which
 * may be a or b), and returns the borrow out of the top limb (0 or 1).
 */
static uint32_t
bn_sub(
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b,
	size_t limbs)
{
	size_t index;
	uint64_t difference;
	uint32_t borrow;

	/*
	 * Subtracts limb by limb, borrowing from above.  The 64-bit
	 * difference wraps when negative, which sets bit 32.
	 */
	borrow = 0;
	for (index = 0; index < limbs; index++) {
		difference = (uint64_t)a[index] - b[index] - borrow;
		out[index] = (uint32_t)difference;
		borrow = (uint32_t)((difference >> 32) & 1U);
	}

	/* Reports the borrow out of the top. */
	return borrow;
}

/*
 * Returns the borrow out of the top limb of a - b (limbs limbs), 1 when
 * a < b, without storing the difference.
 */
static uint32_t
bn_borrow(
	const uint32_t *a,
	const uint32_t *b,
	size_t limbs)
{
	size_t index;
	uint64_t difference;
	uint32_t borrow;

	/* Runs the subtraction for its borrow alone. */
	borrow = 0;
	for (index = 0; index < limbs; index++) {
		difference = (uint64_t)a[index] - b[index] - borrow;
		borrow = (uint32_t)((difference >> 32) & 1U);
	}

	/* Reports the borrow out of the top. */
	return borrow;
}

/*
 * Returns 0xffffffff when value is not zero and 0 when it is, in constant
 * time.
 */
static uint32_t
bn_mask_nonzero(
	uint32_t value)
{
	uint32_t zero_mask;

	/* Inverts the mask of zero. */
	zero_mask = cryc_ct_mask_zero32(value);

	/* Reports the inverse. */
	return ~zero_mask;
}

/*
 * Returns value through a volatile object, so that the compiler cannot
 * know which of the values of a mask (0 or 0xffffffff) it holds.
 */
static uint32_t
bn_opaque(
	uint32_t value)
{
	volatile uint32_t hidden;

	/* Stored and loaded again. */
	hidden = value;
	return hidden;
}
