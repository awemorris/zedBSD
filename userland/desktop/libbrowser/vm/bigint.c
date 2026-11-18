/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BigInt (ws074-p230): an arbitrary-precision integer cell, its arithmetic,
 * bitwise operations (two's complement semantics), comparisons, conversions
 * and its text.
 *
 * A BigInt cell holds a sign and a magnitude of 32-bit limbs, least
 * significant first, with no leading zero limb (zero has no limbs and is
 * never negative).  The arithmetic works on malloc'd magnitudes and makes
 * one cell from the answer.
 */

#include "vm/vm.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The largest shift or exponent, in bits, that is computed. */
#define BIGINT_BITS_MAX	(1UL << 24)

/* A magnitude and its sign while an operation works on it. */
struct bn {
	uint32_t *d;
	size_t n;
	int neg;
};

/* The cell type of BigInts: they own nothing outside their cell. */
const struct vm_cell_type vm_bigint_type = {
	"bigint", NULL, NULL
};

/* Allocates the limbs of an operation (at least one limb is allocated). */
static int
bn_alloc(
	struct bn *x,
	size_t n)
{
	x->d = calloc(n == 0 ? 1 : n, sizeof(uint32_t));
	x->n = n;
	x->neg = 0;
	if (x->d == NULL)
		return ENOMEM;
	return 0;
}

/* Drops the leading zero limbs and the sign of a zero. */
static void
bn_trim(
	struct bn *x)
{
	while (x->n > 0 && x->d[x->n - 1] == 0U)
		x->n--;
	if (x->n == 0)
		x->neg = 0;
}

/* Reads a BigInt cell into an operation's number (the limbs are copied). */
static int
bn_load(
	struct bn *x,
	const struct vm_bigint *big)
{
	const uint32_t *limbs;
	int error;

	error = bn_alloc(x, big->length);
	if (error != 0)
		return error;
	limbs = (const uint32_t *)(big + 1);
	if (big->length != 0)
		memcpy(x->d, limbs, big->length * sizeof(uint32_t));
	x->neg = (int)big->negative;
	return 0;
}

/* Makes a cell from an operation's number and frees the number. */
static int
bn_finish(
	struct vm_heap *heap,
	struct bn *x,
	vm_value *result)
{
	struct vm_bigint *big;

	bn_trim(x);
	big = vm_heap_alloc(heap, &vm_bigint_type, sizeof(*big) + x->n * sizeof(uint32_t));
	if (big == NULL) {
		free(x->d);
		return ENOMEM;
	}
	big->length = (uint32_t)x->n;
	big->negative = (uint32_t)x->neg;
	if (x->n != 0)
		memcpy(big + 1, x->d, x->n * sizeof(uint32_t));
	free(x->d);
	*result = vm_value_cell(big);
	return 0;
}

/* Compares two magnitudes. */
static int
mag_cmp(
	const uint32_t *a,
	size_t an,
	const uint32_t *b,
	size_t bn)
{
	size_t i;

	if (an != bn)
		return an < bn ? -1 : 1;
	for (i = an; i > 0; i--) {
		if (a[i - 1] != b[i - 1])
			return a[i - 1] < b[i - 1] ? -1 : 1;
	}
	return 0;
}

/* Adds two magnitudes into out (max(an, bn) + 1 limbs). */
static void
mag_add(
	const uint32_t *a,
	size_t an,
	const uint32_t *b,
	size_t bn,
	uint32_t *out)
{
	uint64_t carry;
	uint64_t sum;
	size_t n;
	size_t i;

	n = an > bn ? an : bn;
	carry = 0;
	for (i = 0; i < n; i++) {
		sum = carry;
		if (i < an)
			sum += a[i];
		if (i < bn)
			sum += b[i];
		out[i] = (uint32_t)sum;
		carry = sum >> 32;
	}
	out[n] = (uint32_t)carry;
}

/* Subtracts b from a (a is not less than b) into out (an limbs). */
static void
mag_sub(
	const uint32_t *a,
	size_t an,
	const uint32_t *b,
	size_t bn,
	uint32_t *out)
{
	int64_t borrow;
	int64_t difference;
	size_t i;

	borrow = 0;
	for (i = 0; i < an; i++) {
		difference = (int64_t)a[i] - borrow;
		if (i < bn)
			difference -= (int64_t)b[i];
		borrow = 0;
		if (difference < 0) {
			difference += (int64_t)1 << 32;
			borrow = 1;
		}
		out[i] = (uint32_t)difference;
	}
}

/* Divides a magnitude in place by a small number; reports the remainder. */
static uint32_t
mag_divmod_small(
	uint32_t *a,
	size_t n,
	uint32_t divisor)
{
	uint64_t rest;
	uint64_t cur;
	size_t i;

	rest = 0;
	for (i = n; i > 0; i--) {
		cur = (rest << 32) | a[i - 1];
		a[i - 1] = (uint32_t)(cur / divisor);
		rest = cur % divisor;
	}
	return (uint32_t)rest;
}

/* Multiplies a magnitude in place by a small number and adds a small number (a has room for the carry). */
static void
mag_muladd_small(
	uint32_t *a,
	size_t *n,
	uint32_t factor,
	uint32_t addend)
{
	uint64_t carry;
	uint64_t cur;
	size_t i;

	carry = addend;
	for (i = 0; i < *n; i++) {
		cur = (uint64_t)a[i] * factor + carry;
		a[i] = (uint32_t)cur;
		carry = cur >> 32;
	}
	if (carry != 0)
		a[(*n)++] = (uint32_t)carry;
}

/* Counts the bits of a magnitude. */
static size_t
mag_bits(
	const uint32_t *a,
	size_t n)
{
	uint32_t top;
	size_t bits;

	if (n == 0)
		return 0;
	top = a[n - 1];
	bits = 0;
	while (top != 0U) {
		bits++;
		top >>= 1;
	}
	return (n - 1) * 32U + bits;
}

/* Tells whether a bit of a magnitude is set. */
static int
mag_bit(
	const uint32_t *a,
	size_t n,
	size_t bit)
{
	if (bit / 32U >= n)
		return 0;
	return (int)((a[bit / 32U] >> (bit % 32U)) & 1U);
}

/* Makes the magnitude of a * b. */
static int
bn_mul(
	const struct bn *a,
	const struct bn *b,
	struct bn *out)
{
	uint64_t carry;
	uint64_t cur;
	size_t i;
	size_t j;
	int error;

	error = bn_alloc(out, a->n + b->n);
	if (error != 0)
		return error;
	for (i = 0; i < a->n; i++) {
		carry = 0;
		for (j = 0; j < b->n; j++) {
			cur = (uint64_t)a->d[i] * b->d[j] + out->d[i + j] + carry;
			out->d[i + j] = (uint32_t)cur;
			carry = cur >> 32;
		}
		out->d[i + b->n] = (uint32_t)carry;
	}
	out->neg = a->neg != b->neg;
	bn_trim(out);
	return 0;
}

/* Makes the signed sum a + b (b negated when subtract). */
static int
bn_add(
	const struct bn *a,
	const struct bn *b,
	int subtract,
	struct bn *out)
{
	size_t n;
	int bneg;
	int order;
	int error;

	bneg = b->neg;
	if (subtract && b->n != 0)
		bneg = !bneg;
	n = (a->n > b->n ? a->n : b->n) + 1U;
	error = bn_alloc(out, n);
	if (error != 0)
		return error;
	if (a->neg == bneg) {
		mag_add(a->d, a->n, b->d, b->n, out->d);
		out->neg = a->neg;
	} else {
		order = mag_cmp(a->d, a->n, b->d, b->n);
		if (order >= 0) {
			mag_sub(a->d, a->n, b->d, b->n, out->d);
			out->neg = a->neg;
		} else {
			mag_sub(b->d, b->n, a->d, a->n, out->d);
			out->neg = bneg;
		}
	}
	bn_trim(out);
	return 0;
}

/* Divides magnitudes by shift and subtract: quotient and remainder (either may be NULL). */
static int
bn_divmod(
	const struct bn *a,
	const struct bn *b,
	struct bn *quotient,
	struct bn *remainder)
{
	struct bn q;
	struct bn r;
	size_t bits;
	size_t i;
	size_t j;
	uint32_t carry;
	uint32_t next;
	int error;

	error = bn_alloc(&q, a->n);
	if (error != 0)
		return error;
	error = bn_alloc(&r, b->n + 1U);
	if (error != 0) {
		free(q.d);
		return error;
	}
	r.n = 0;

	/* A one-limb divisor divides directly. */
	if (b->n == 1) {
		memcpy(q.d, a->d, a->n * sizeof(uint32_t));
		r.d[0] = mag_divmod_small(q.d, a->n, b->d[0]);
		r.n = 1;
	} else {
		bits = mag_bits(a->d, a->n);
		for (i = bits; i > 0; i--) {
			/* rest = rest * 2 + the next bit. */
			carry = (uint32_t)mag_bit(a->d, a->n, i - 1);
			for (j = 0; j < r.n; j++) {
				next = r.d[j] >> 31;
				r.d[j] = (r.d[j] << 1) | carry;
				carry = next;
			}
			if (carry != 0U)
				r.d[r.n++] = carry;
			if (mag_cmp(r.d, r.n, b->d, b->n) >= 0) {
				mag_sub(r.d, r.n, b->d, b->n, r.d);
				while (r.n > 0 && r.d[r.n - 1] == 0U)
					r.n--;
				q.d[(i - 1) / 32U] |= 1U << ((i - 1) % 32U);
			}
		}
	}
	bn_trim(&q);
	bn_trim(&r);
	if (quotient != NULL)
		*quotient = q;
	else
		free(q.d);
	if (remainder != NULL)
		*remainder = r;
	else
		free(r.d);
	return 0;
}

/* Shifts a magnitude left by bits into a fresh number. */
static int
bn_shift_left(
	const struct bn *a,
	size_t bits,
	struct bn *out)
{
	size_t words;
	size_t rest;
	size_t i;
	int error;

	words = bits / 32U;
	rest = bits % 32U;
	error = bn_alloc(out, a->n + words + 1U);
	if (error != 0)
		return error;
	for (i = 0; i < a->n; i++) {
		out->d[i + words] |= a->d[i] << rest;
		if (rest != 0)
			out->d[i + words + 1U] |= a->d[i] >> (32U - rest);
	}
	out->neg = a->neg;
	bn_trim(out);
	return 0;
}

/* Shifts a magnitude right by bits (truncating) into a fresh number; reports whether a set bit fell off. */
static int
bn_shift_right(
	const struct bn *a,
	size_t bits,
	struct bn *out,
	int *lost)
{
	size_t words;
	size_t rest;
	size_t i;
	int error;

	words = bits / 32U;
	rest = bits % 32U;
	*lost = 0;
	for (i = 0; i < a->n && i < words; i++) {
		if (a->d[i] != 0U)
			*lost = 1;
	}
	if (words < a->n && rest != 0 && (a->d[words] & ((1U << rest) - 1U)) != 0U)
		*lost = 1;
	error = bn_alloc(out, a->n > words ? a->n - words : 0);
	if (error != 0)
		return error;
	for (i = words; i < a->n; i++) {
		out->d[i - words] = a->d[i] >> rest;
		if (rest != 0 && i + 1U < a->n)
			out->d[i - words] |= a->d[i + 1U] << (32U - rest);
	}
	out->neg = a->neg;
	bn_trim(out);
	return 0;
}

/* Reads a number's limbs in two's complement over width limbs. */
static void
bn_twos(
	const struct bn *a,
	size_t width,
	uint32_t *out)
{
	uint64_t carry;
	size_t i;

	memset(out, 0, width * sizeof(uint32_t));
	memcpy(out, a->d, a->n * sizeof(uint32_t));
	if (!a->neg)
		return;
	carry = 1;
	for (i = 0; i < width; i++) {
		carry += (uint32_t)~out[i];
		out[i] = (uint32_t)carry;
		carry >>= 32;
	}
}

/* Makes a number from width limbs of two's complement. */
static int
bn_from_twos(
	uint32_t *limbs,
	size_t width,
	struct bn *out)
{
	uint64_t carry;
	size_t i;
	int error;

	error = bn_alloc(out, width);
	if (error != 0)
		return error;
	out->neg = (int)(limbs[width - 1U] >> 31);
	if (out->neg) {
		carry = 1;
		for (i = 0; i < width; i++) {
			carry += (uint32_t)~limbs[i];
			out->d[i] = (uint32_t)carry;
			carry >>= 32;
		}
	} else {
		memcpy(out->d, limbs, width * sizeof(uint32_t));
	}
	bn_trim(out);
	return 0;
}

/* Makes a BigInt from a signed 64-bit integer. */
static int
bn_from_int64(
	int64_t value,
	struct bn *out)
{
	uint64_t magnitude;
	int error;

	error = bn_alloc(out, 2);
	if (error != 0)
		return error;
	magnitude = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
	out->d[0] = (uint32_t)magnitude;
	out->d[1] = (uint32_t)(magnitude >> 32);
	out->neg = value < 0;
	bn_trim(out);
	return 0;
}

/* Makes base ^ exponent by repeated squaring. */
static int
bn_pow(
	const struct bn *a,
	uint32_t exponent,
	struct bn *out)
{
	struct bn base;
	struct bn result;
	struct bn tmp;
	int error;

	error = bn_alloc(&result, 1);
	if (error != 0)
		return error;
	result.d[0] = 1U;
	result.n = 1U;
	error = bn_alloc(&base, a->n);
	if (error != 0) {
		free(result.d);
		return error;
	}
	memcpy(base.d, a->d, a->n * sizeof(uint32_t));
	base.neg = a->neg;
	while (exponent != 0U) {
		if ((exponent & 1U) != 0U) {
			error = bn_mul(&result, &base, &tmp);
			if (error != 0)
				break;
			free(result.d);
			result = tmp;
		}
		exponent >>= 1;
		if (exponent != 0U) {
			error = bn_mul(&base, &base, &tmp);
			if (error != 0)
				break;
			free(base.d);
			base = tmp;
		}
	}
	free(base.d);
	if (error != 0) {
		free(result.d);
		return error;
	}
	*out = result;
	return 0;
}

/* Makes a BigInt from a whole number. */
int
vm_bigint_from_double(
	struct vm_heap *heap,
	double number,
	vm_value *result)
{
	struct bn x;
	struct bn y;
	double mantissa;
	uint64_t bits;
	int exponent;
	int lost;
	int error;

	/* The 53-bit mantissa and the power of two. */
	mantissa = frexp(fabs(number), &exponent);
	bits = (uint64_t)ldexp(mantissa, 53);
	exponent -= 53;
	error = bn_alloc(&x, 2);
	if (error != 0)
		return error;
	x.d[0] = (uint32_t)bits;
	x.d[1] = (uint32_t)(bits >> 32);
	bn_trim(&x);
	if (exponent >= 0)
		error = bn_shift_left(&x, (size_t)exponent, &y);
	else
		error = bn_shift_right(&x, (size_t)(-exponent), &y, &lost);
	free(x.d);
	if (error != 0)
		return error;
	y.neg = number < 0.0 && y.n != 0;
	return bn_finish(heap, &y, result);
}

/* Makes a BigInt from a signed integer. */
int
vm_bigint_from_int64(
	struct vm_heap *heap,
	int64_t value,
	vm_value *result)
{
	struct bn x;
	int error;

	error = bn_from_int64(value, &x);
	if (error != 0)
		return error;
	return bn_finish(heap, &x, result);
}

/* Makes a BigInt from a numeral's digits in a radix (an optional leading -); EINVAL for no digits or a bad digit. */
int
vm_bigint_from_digits(
	struct vm_heap *heap,
	const char *digits,
	size_t length,
	int radix,
	int negative,
	vm_value *result)
{
	struct bn x;
	size_t i;
	size_t per;
	int digit;
	int error;
	char c;

	if (length == 0)
		return EINVAL;
	per = length / 8U + 2U;
	error = bn_alloc(&x, per * (radix == 10 ? 1U : 1U) + 1U);
	if (error != 0)
		return error;
	x.n = 0;
	for (i = 0; i < length; i++) {
		c = digits[i];
		digit = -1;
		if (c >= '0' && c <= '9')
			digit = c - '0';
		else if (c >= 'a' && c <= 'z')
			digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z')
			digit = c - 'A' + 10;
		if (digit < 0 || digit >= radix) {
			free(x.d);
			return EINVAL;
		}
		mag_muladd_small(x.d, &x.n, (uint32_t)radix, (uint32_t)digit);
	}
	x.neg = negative;
	return bn_finish(heap, &x, result);
}

/* Reads a string as StringToBigInt does: trimmed, empty is 0, a 0x/0o/0b prefix or a signed decimal; EINVAL otherwise. */
int
vm_bigint_from_text(
	struct vm_heap *heap,
	const char *text,
	size_t length,
	vm_value *result)
{
	int radix;
	int negative;

	while (length > 0 && (text[0] == ' ' || (text[0] >= '\t' && text[0] <= '\r'))) {
		text++;
		length--;
	}
	while (length > 0 && (text[length - 1] == ' ' || (text[length - 1] >= '\t' && text[length - 1] <= '\r')))
		length--;
	if (length == 0)
		return vm_bigint_from_int64(heap, 0, result);
	radix = 10;
	negative = 0;
	if (length > 2 && text[0] == '0') {
		if (text[1] == 'x' || text[1] == 'X')
			radix = 16;
		if (text[1] == 'o' || text[1] == 'O')
			radix = 8;
		if (text[1] == 'b' || text[1] == 'B')
			radix = 2;
		if (radix != 10) {
			text += 2;
			length -= 2;
		}
	}
	if (radix == 10 && length > 1 && (text[0] == '+' || text[0] == '-')) {
		negative = text[0] == '-';
		text++;
		length--;
	}
	return vm_bigint_from_digits(heap, text, length, radix, negative, result);
}

/* Appends a BigInt's digits in a radix (2 to 36). */
int
vm_bigint_to_text(
	const struct vm_bigint *big,
	int radix,
	struct wb_buffer *out)
{
	static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	struct bn x;
	char *text;
	size_t size;
	size_t used;
	size_t i;
	uint32_t digit;
	int error;

	if (big->length == 0)
		return wb_buffer_append_byte(out, '0');
	error = bn_load(&x, big);
	if (error != 0)
		return error;
	size = big->length * 32U + 2U;
	text = malloc(size);
	if (text == NULL) {
		free(x.d);
		return ENOMEM;
	}
	used = 0;
	while (x.n > 0) {
		digit = mag_divmod_small(x.d, x.n, (uint32_t)radix);
		text[used++] = digits[digit];
		while (x.n > 0 && x.d[x.n - 1] == 0U)
			x.n--;
	}
	free(x.d);
	error = 0;
	if (big->negative)
		error = wb_buffer_append_byte(out, '-');
	for (i = used; error == 0 && i > 0; i--)
		error = wb_buffer_append_byte(out, (unsigned char)text[i - 1]);
	free(text);
	return error;
}

/* Reports a BigInt as the nearest double. */
double
vm_bigint_to_double(
	const struct vm_bigint *big)
{
	const uint32_t *limbs;
	uint64_t top;
	size_t bits;
	size_t shift;
	size_t i;
	int sticky;
	double answer;

	limbs = (const uint32_t *)(big + 1);
	bits = mag_bits(limbs, big->length);
	if (bits == 0)
		return 0.0;
	if (bits <= 64U) {
		top = limbs[0];
		if (big->length > 1U)
			top |= (uint64_t)limbs[1] << 32;
		answer = (double)top;
	} else {
		shift = bits - 64U;
		top = 0;
		for (i = 0; i < 64U; i++) {
			if (mag_bit(limbs, big->length, shift + i))
				top |= (uint64_t)1 << i;
		}
		sticky = 0;
		for (i = 0; i < shift && !sticky; i++) {
			if (mag_bit(limbs, big->length, i))
				sticky = 1;
		}
		if (sticky)
			top |= 1U;
		if (bits > 1100U)
			answer = HUGE_VAL;
		else
			answer = ldexp((double)top, (int)shift);
	}
	if (big->negative)
		answer = -answer;
	return answer;
}

/* Tells whether a BigInt is zero. */
int
vm_bigint_is_zero(
	const struct vm_bigint *big)
{
	return big->length == 0;
}

/* Compares two BigInts: -1, 0 or 1. */
int
vm_bigint_compare(
	const struct vm_bigint *a,
	const struct vm_bigint *b)
{
	int order;

	if (a->negative != b->negative)
		return a->negative ? -1 : 1;
	order = mag_cmp((const uint32_t *)(a + 1), a->length, (const uint32_t *)(b + 1), b->length);
	if (a->negative)
		order = -order;
	return order;
}

/* Compares a BigInt with a double: -1, 0, 1, or 2 when unordered (NaN). */
int
vm_bigint_compare_double(
	struct vm_heap *heap,
	const struct vm_bigint *a,
	double number,
	int *order)
{
	struct vm_bigint *whole;
	vm_value cell;
	double floored;
	int error;

	if (number != number) {
		*order = 2;
		return 0;
	}
	if (isinf(number)) {
		*order = number > 0 ? -1 : 1;
		return 0;
	}
	floored = floor(number);
	error = vm_bigint_from_double(heap, floored, &cell);
	if (error != 0)
		return error;
	whole = (struct vm_bigint *)vm_value_as_cell(cell);
	*order = vm_bigint_compare(a, whole);
	if (*order == 0 && number > floored)
		*order = -1;
	return 0;
}

/* The shift of a BigInt by a count that is a BigInt (a negative count shifts the other way). */
static int
bigint_shift(
	struct vm_realm *realm,
	const struct bn *a,
	const struct bn *count,
	int left,
	vm_value *result)
{
	struct bn out;
	size_t amount;
	int lost;
	int error;

	if (count->neg)
		left = !left;
	if (count->n > 1U || (count->n == 1U && count->d[0] > BIGINT_BITS_MAX)) {
		/* A huge count: left is out of range, right gives 0 or -1. */
		if (left && a->n != 0)
			return vm_throw_range_error(realm, "Maximum BigInt size exceeded");
		amount = (size_t)BIGINT_BITS_MAX * 64U;
	} else {
		amount = count->n == 0 ? 0 : count->d[0];
	}
	if (left) {
		error = bn_shift_left(a, amount, &out);
		if (error != 0)
			return error;
		return bn_finish(realm->heap, &out, result);
	}
	error = bn_shift_right(a, amount, &out, &lost);
	if (error != 0)
		return error;
	if (a->neg && lost) {
		/* Rounds toward negative infinity: one more in magnitude. */
		struct bn one;
		struct bn sum;

		error = bn_from_int64(1, &one);
		if (error != 0) {
			free(out.d);
			return error;
		}
		out.neg = 0;
		error = bn_add(&out, &one, 0, &sum);
		free(one.d);
		free(out.d);
		if (error != 0)
			return error;
		sum.neg = 1;
		return bn_finish(realm->heap, &sum, result);
	}
	return bn_finish(realm->heap, &out, result);
}

/* Applies a binary operator (enum vm_numeric_operator, or add) to two BigInts. */
int
vm_bigint_binary(
	struct vm_realm *realm,
	int operator,
	const struct vm_bigint *left,
	const struct vm_bigint *right,
	vm_value *result)
{
	struct bn a;
	struct bn b;
	struct bn out;
	size_t width;
	size_t i;
	size_t n;
	uint32_t *ta;
	uint32_t *tb;
	uint32_t *tc;
	int error;

	error = bn_load(&a, left);
	if (error != 0)
		return error;
	error = bn_load(&b, right);
	if (error != 0) {
		free(a.d);
		return error;
	}
	error = 0;
	switch (operator) {
	case VM_NUMERIC_ADD:
		error = bn_add(&a, &b, 0, &out);
		break;
	case VM_NUMERIC_SUB:
		error = bn_add(&a, &b, 1, &out);
		break;
	case VM_NUMERIC_MUL:
		error = bn_mul(&a, &b, &out);
		break;
	case VM_NUMERIC_DIV:
	case VM_NUMERIC_MOD:
		if (b.n == 0) {
			free(a.d);
			free(b.d);
			return vm_throw_range_error(realm, "Division by zero");
		}
		if (operator == VM_NUMERIC_DIV)
			error = bn_divmod(&a, &b, &out, NULL);
		else
			error = bn_divmod(&a, &b, NULL, &out);
		if (error == 0) {
			if (operator == VM_NUMERIC_DIV)
				out.neg = a.neg != b.neg && out.n != 0;
			else
				out.neg = a.neg && out.n != 0;
		}
		break;
	case VM_NUMERIC_EXP:
		if (b.neg) {
			free(a.d);
			free(b.d);
			return vm_throw_range_error(realm, "Exponent must be non-negative");
		}
		if (a.n == 0 || (a.n == 1U && a.d[0] == 1U)) {
			/* 0 and 1 and -1 stay small whatever the exponent. */
			error = bn_alloc(&out, 1);
			if (error != 0)
				break;
			if (b.n == 0) {
				out.d[0] = 1U;
			} else {
				out.d[0] = a.n == 0 ? 0U : 1U;
				out.neg = a.neg && (b.d[0] & 1U) != 0U;
			}
			bn_trim(&out);
			break;
		}
		if (b.n > 1U || b.d[0] > BIGINT_BITS_MAX / 8U) {
			free(a.d);
			free(b.d);
			return vm_throw_range_error(realm, "Maximum BigInt size exceeded");
		}
		error = bn_pow(&a, b.d[0], &out);
		break;
	case VM_NUMERIC_SHL:
		error = bigint_shift(realm, &a, &b, 1, result);
		free(a.d);
		free(b.d);
		return error;
	case VM_NUMERIC_SAR:
		error = bigint_shift(realm, &a, &b, 0, result);
		free(a.d);
		free(b.d);
		return error;
	case VM_NUMERIC_SHR:
		free(a.d);
		free(b.d);
		return vm_throw_type_error(realm, "BigInts have no unsigned right shift, use >> instead");
	case VM_NUMERIC_AND:
	case VM_NUMERIC_OR:
	case VM_NUMERIC_XOR:
		n = a.n > b.n ? a.n : b.n;
		width = n + 1U;
		ta = malloc(width * 3U * sizeof(uint32_t));
		if (ta == NULL) {
			error = ENOMEM;
			break;
		}
		tb = ta + width;
		tc = tb + width;
		bn_twos(&a, width, ta);
		bn_twos(&b, width, tb);
		for (i = 0; i < width; i++) {
			if (operator == VM_NUMERIC_AND)
				tc[i] = ta[i] & tb[i];
			else if (operator == VM_NUMERIC_OR)
				tc[i] = ta[i] | tb[i];
			else
				tc[i] = ta[i] ^ tb[i];
		}
		error = bn_from_twos(tc, width, &out);
		free(ta);
		break;
	default:
		error = EINVAL;
		break;
	}
	free(a.d);
	free(b.d);
	if (error != 0)
		return error;
	return bn_finish(realm->heap, &out, result);
}

/* Negates a BigInt. */
int
vm_bigint_negate(
	struct vm_heap *heap,
	const struct vm_bigint *big,
	vm_value *result)
{
	struct bn x;
	int error;

	error = bn_load(&x, big);
	if (error != 0)
		return error;
	if (x.n != 0)
		x.neg = !x.neg;
	return bn_finish(heap, &x, result);
}

/* Adds a small integer to a BigInt (for ++ and --, and ~). */
int
vm_bigint_add_small(
	struct vm_heap *heap,
	const struct vm_bigint *big,
	int64_t amount,
	vm_value *result)
{
	struct bn a;
	struct bn b;
	struct bn out;
	int error;

	error = bn_load(&a, big);
	if (error != 0)
		return error;
	error = bn_from_int64(amount, &b);
	if (error != 0) {
		free(a.d);
		return error;
	}
	error = bn_add(&a, &b, 0, &out);
	free(a.d);
	free(b.d);
	if (error != 0)
		return error;
	return bn_finish(heap, &out, result);
}

/* ~x is -x - 1. */
int
vm_bigint_not(
	struct vm_heap *heap,
	const struct vm_bigint *big,
	vm_value *result)
{
	struct bn x;
	struct bn one;
	struct bn out;
	int error;

	error = bn_load(&x, big);
	if (error != 0)
		return error;
	if (x.n != 0)
		x.neg = !x.neg;
	error = bn_from_int64(1, &one);
	if (error != 0) {
		free(x.d);
		return error;
	}
	error = bn_add(&x, &one, 1, &out);
	free(x.d);
	free(one.d);
	if (error != 0)
		return error;
	return bn_finish(heap, &out, result);
}

/* BigInt.asUintN / asIntN: the value modulo 2^bits, as unsigned or into the signed range. */
int
vm_bigint_as_n(
	struct vm_realm *realm,
	uint64_t bits,
	int is_signed,
	const struct vm_bigint *big,
	vm_value *result)
{
	struct bn a;
	struct bn out;
	uint32_t *t;
	size_t width;
	size_t keep;
	size_t i;
	int top;
	int error;

	if (bits == 0)
		return vm_bigint_from_int64(realm->heap, 0, result);
	if (bits > BIGINT_BITS_MAX)
		return vm_throw_range_error(realm, "Invalid value: not (convertible to) a safe integer");
	error = bn_load(&a, big);
	if (error != 0)
		return error;
	keep = ((size_t)bits + 31U) / 32U;
	width = (a.n > keep ? a.n : keep) + 1U;
	t = malloc(width * sizeof(uint32_t));
	if (t == NULL) {
		free(a.d);
		return ENOMEM;
	}
	bn_twos(&a, width, t);
	free(a.d);

	/* The low bits only. */
	for (i = keep; i < width; i++)
		t[i] = 0;
	if ((bits % 32U) != 0U)
		t[keep - 1U] &= (1U << (bits % 32U)) - 1U;
	top = (int)((t[(bits - 1U) / 32U] >> ((bits - 1U) % 32U)) & 1U);
	if (is_signed && top) {
		/* r - 2^bits: set every bit above and read as a negative two's complement number. */
		for (i = (size_t)bits; i < width * 32U; i++)
			t[i / 32U] |= 1U << (i % 32U);
	}
	error = bn_from_twos(t, width, &out);
	free(t);
	if (error != 0)
		return error;
	return bn_finish(realm->heap, &out, result);
}

/* Reports the cell of a value that is a BigInt, or NULL. */
struct vm_bigint *
vm_value_as_bigint(
	vm_value value)
{
	struct vm_cell *cell;
	int is_cell;

	is_cell = vm_value_is_cell(value);
	if (!is_cell)
		return NULL;
	cell = vm_value_as_cell(value);
	if (cell->type == &vm_bigint_type)
		return (struct vm_bigint *)cell;
	return NULL;
}

/* Reports a BigInt's hash. */
uint32_t
vm_bigint_hash(
	const struct vm_bigint *big)
{
	const uint32_t *limbs;
	uint32_t hash;
	size_t i;

	limbs = (const uint32_t *)(big + 1);
	hash = 2166136261U ^ big->negative;
	for (i = 0; i < big->length; i++)
		hash = (hash ^ limbs[i]) * 16777619U;
	return hash;
}

/* Reports a BigInt's low 64 bits (two's complement, as BigInt.asUintN(64) keeps them). */
uint64_t
vm_bigint_to_uint64(
	const struct vm_bigint *big)
{
	const uint32_t *limbs;
	uint64_t low;

	limbs = (const uint32_t *)(big + 1);
	low = 0;
	if (big->length > 0)
		low = limbs[0];
	if (big->length > 1U)
		low |= (uint64_t)limbs[1] << 32;
	if (big->negative)
		low = (uint64_t)0 - low;
	return low;
}

/* Makes a BigInt from an unsigned 64-bit integer. */
int
vm_bigint_from_uint64(
	struct vm_heap *heap,
	uint64_t value,
	vm_value *result)
{
	struct bn x;
	int error;

	error = bn_alloc(&x, 2);
	if (error != 0)
		return error;
	x.d[0] = (uint32_t)value;
	x.d[1] = (uint32_t)(value >> 32);
	return bn_finish(heap, &x, result);
}
