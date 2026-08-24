/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/bc/number.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BC_BASE 1000000000U

void
bc_number_init(struct bc_number *number)
{
	memset(number, 0, sizeof(*number));
}

void
bc_number_free(struct bc_number *number)
{
	free(number->digit);
	bc_number_init(number);
}

static void
number_normalize(struct bc_number *number)
{
	while (number->length != 0 && number->digit[number->length - 1] == 0)
		number->length--;
	if (number->length == 0)
		number->sign = 0;
}

static int
number_reserve(struct bc_number *number, size_t needed)
{
	size_t capacity;
	uint32_t *digit;

	if (needed <= number->capacity)
		return 0;
	if (needed > SIZE_MAX / sizeof(*digit)) {
		errno = EOVERFLOW;
		return -1;
	}
	capacity = number->capacity == 0 ? 4 : number->capacity;
	while (capacity < needed) {
		if (capacity > SIZE_MAX / 2) {
			capacity = needed;
			break;
		}
		capacity *= 2;
	}
	digit = realloc(number->digit, capacity * sizeof(*digit));
	if (digit == NULL)
		return -1;
	number->digit = digit;
	number->capacity = capacity;
	return 0;
}

static void
number_move(struct bc_number *destination, struct bc_number *source)
{
	bc_number_free(destination);
	*destination = *source;
	bc_number_init(source);
}

int
bc_number_from_decimal(struct bc_number *number, const char *text,
		       size_t length)
{
	struct bc_number result;
	size_t offset = 0;
	int sign = 1;

	bc_number_init(&result);
	if (length != 0 && (*text == '+' || *text == '-')) {
		sign = *text == '-' ? -1 : 1;
		text++;
		length--;
	}
	if (length == 0) {
		errno = EINVAL;
		return -1;
	}
	while (offset < length && text[offset] == '0')
		offset++;
	if (offset == length) {
		number_move(number, &result);
		return 0;
	}
	for (; offset < length; offset++) {
		uint64_t carry;
		size_t index;

		if (text[offset] < '0' || text[offset] > '9') {
			bc_number_free(&result);
			errno = EINVAL;
			return -1;
		}
		carry = (unsigned)(text[offset] - '0');
		for (index = 0; index < result.length; index++) {
			uint64_t value =
			    (uint64_t)result.digit[index] * 10U + carry;
			result.digit[index] = (uint32_t)(value % BC_BASE);
			carry = value / BC_BASE;
		}
		if (carry != 0) {
			if (number_reserve(&result, result.length + 1) != 0) {
				bc_number_free(&result);
				return -1;
			}
			result.digit[result.length++] = (uint32_t)carry;
		}
	}
	result.sign = sign;
	number_move(number, &result);
	return 0;
}

int
bc_number_copy(struct bc_number *destination, const struct bc_number *source)
{
	struct bc_number result;

	bc_number_init(&result);
	if (source->length != 0) {
		if (number_reserve(&result, source->length) != 0)
			return -1;
		memcpy(result.digit, source->digit,
		       source->length * sizeof(*source->digit));
		result.length = source->length;
		result.sign = source->sign;
	}
	number_move(destination, &result);
	return 0;
}

static int
number_compare_absolute(const struct bc_number *left,
			const struct bc_number *right)
{
	size_t index;

	if (left->length != right->length)
		return left->length < right->length ? -1 : 1;
	index = left->length;
	while (index != 0) {
		index--;
		if (left->digit[index] != right->digit[index])
			return left->digit[index] < right->digit[index] ? -1
									: 1;
	}
	return 0;
}

static int
number_add_absolute(struct bc_number *result, const struct bc_number *left,
		    const struct bc_number *right)
{
	size_t length =
	    left->length > right->length ? left->length : right->length;
	size_t index;
	uint64_t carry = 0;

	if (number_reserve(result, length + 1) != 0)
		return -1;
	for (index = 0; index < length; index++) {
		uint64_t value = carry;

		if (index < left->length)
			value += left->digit[index];
		if (index < right->length)
			value += right->digit[index];
		result->digit[index] = (uint32_t)(value % BC_BASE);
		carry = value / BC_BASE;
	}
	result->length = length;
	if (carry != 0)
		result->digit[result->length++] = (uint32_t)carry;
	return 0;
}

static void
number_subtract_absolute(struct bc_number *result, const struct bc_number *left,
			 const struct bc_number *right)
{
	size_t index;
	uint64_t borrow = 0;

	for (index = 0; index < left->length; index++) {
		uint64_t value = left->digit[index];
		uint64_t subtract = borrow;

		if (index < right->length)
			subtract += right->digit[index];
		if (value < subtract) {
			result->digit[index] =
			    (uint32_t)(value + BC_BASE - subtract);
			borrow = 1;
		} else {
			result->digit[index] = (uint32_t)(value - subtract);
			borrow = 0;
		}
	}
	result->length = left->length;
	number_normalize(result);
}

int
bc_number_add(struct bc_number *destination, const struct bc_number *left,
	      const struct bc_number *right)
{
	struct bc_number result;
	int comparison;

	bc_number_init(&result);
	if (left->sign == 0)
		return bc_number_copy(destination, right);
	if (right->sign == 0)
		return bc_number_copy(destination, left);
	if (left->sign == right->sign) {
		if (number_add_absolute(&result, left, right) != 0)
			return -1;
		result.sign = left->sign;
	} else {
		comparison = number_compare_absolute(left, right);
		if (comparison == 0) {
			number_move(destination, &result);
			return 0;
		}
		if (number_reserve(&result, comparison > 0
						? left->length
						: right->length) != 0)
			return -1;
		if (comparison > 0) {
			number_subtract_absolute(&result, left, right);
			result.sign = left->sign;
		} else {
			number_subtract_absolute(&result, right, left);
			result.sign = right->sign;
		}
	}
	number_move(destination, &result);
	return 0;
}

int
bc_number_negate(struct bc_number *destination, const struct bc_number *source)
{
	if (bc_number_copy(destination, source) != 0)
		return -1;
	destination->sign = -destination->sign;
	return 0;
}

int
bc_number_subtract(struct bc_number *destination, const struct bc_number *left,
		   const struct bc_number *right)
{
	struct bc_number negative;
	int result;

	bc_number_init(&negative);
	if (bc_number_negate(&negative, right) != 0)
		return -1;
	result = bc_number_add(destination, left, &negative);
	bc_number_free(&negative);
	return result;
}

int
bc_number_multiply(struct bc_number *destination, const struct bc_number *left,
		   const struct bc_number *right)
{
	struct bc_number result;
	size_t i;

	bc_number_init(&result);
	if (left->sign == 0 || right->sign == 0) {
		number_move(destination, &result);
		return 0;
	}
	if (left->length > SIZE_MAX - right->length ||
	    number_reserve(&result, left->length + right->length) != 0)
		return -1;
	memset(result.digit, 0,
	       (left->length + right->length) * sizeof(*result.digit));
	result.length = left->length + right->length;
	for (i = 0; i < left->length; i++) {
		uint64_t carry = 0;
		size_t j;

		for (j = 0; j < right->length; j++) {
			uint64_t value =
			    result.digit[i + j] + carry +
			    (uint64_t)left->digit[i] * right->digit[j];

			result.digit[i + j] = (uint32_t)(value % BC_BASE);
			carry = value / BC_BASE;
		}
		result.digit[i + right->length] = (uint32_t)carry;
	}
	result.sign = left->sign * right->sign;
	number_normalize(&result);
	number_move(destination, &result);
	return 0;
}

static int
number_multiply_small(struct bc_number *destination,
		      const struct bc_number *source, uint32_t factor)
{
	struct bc_number result;
	uint64_t carry = 0;
	size_t index;

	bc_number_init(&result);
	if (factor == 0 || source->sign == 0) {
		number_move(destination, &result);
		return 0;
	}
	if (number_reserve(&result, source->length + 1) != 0)
		return -1;
	for (index = 0; index < source->length; index++) {
		uint64_t value =
		    (uint64_t)source->digit[index] * factor + carry;

		result.digit[index] = (uint32_t)(value % BC_BASE);
		carry = value / BC_BASE;
	}
	result.length = source->length;
	if (carry != 0)
		result.digit[result.length++] = (uint32_t)carry;
	result.sign = 1;
	number_move(destination, &result);
	return 0;
}

static int
number_shift_base(struct bc_number *number, uint32_t low)
{
	if (number->length == 0 && low == 0)
		return 0;
	if (number_reserve(number, number->length + 1) != 0)
		return -1;
	memmove(number->digit + 1, number->digit,
		number->length * sizeof(*number->digit));
	number->digit[0] = low;
	number->length++;
	number->sign = 1;
	return 0;
}

static int
number_divide_absolute(struct bc_number *quotient, struct bc_number *remainder,
		       const struct bc_number *dividend,
		       const struct bc_number *divisor)
{
	struct bc_number q;
	struct bc_number r;
	size_t index;

	bc_number_init(&q);
	bc_number_init(&r);
	if (number_reserve(&q, dividend->length) != 0)
		goto fail;
	memset(q.digit, 0, dividend->length * sizeof(*q.digit));
	q.length = dividend->length;
	index = dividend->length;
	while (index != 0) {
		uint32_t low = 0;
		uint32_t high = BC_BASE - 1;
		uint32_t selected = 0;

		index--;
		if (number_shift_base(&r, dividend->digit[index]) != 0)
			goto fail;
		while (low <= high) {
			uint32_t middle = low + (high - low) / 2;
			struct bc_number product;
			int comparison;

			bc_number_init(&product);
			if (number_multiply_small(&product, divisor, middle) !=
			    0) {
				bc_number_free(&product);
				goto fail;
			}
			comparison = number_compare_absolute(&product, &r);
			bc_number_free(&product);
			if (comparison <= 0) {
				selected = middle;
				if (middle == BC_BASE - 1)
					break;
				low = middle + 1;
			} else {
				if (middle == 0)
					break;
				high = middle - 1;
			}
		}
		q.digit[index] = selected;
		if (selected != 0) {
			struct bc_number product;

			bc_number_init(&product);
			if (number_multiply_small(&product, divisor,
						  selected) != 0) {
				bc_number_free(&product);
				goto fail;
			}
			number_subtract_absolute(&r, &r, &product);
			bc_number_free(&product);
		}
	}
	q.sign = 1;
	r.sign = r.length == 0 ? 0 : 1;
	number_normalize(&q);
	number_move(quotient, &q);
	number_move(remainder, &r);
	return 0;

fail:
	bc_number_free(&q);
	bc_number_free(&r);
	return -1;
}

int
bc_number_divide(struct bc_number *quotient, struct bc_number *remainder,
		 const struct bc_number *dividend,
		 const struct bc_number *divisor)
{
	struct bc_number q;
	struct bc_number r;

	if (divisor->sign == 0) {
		errno = EDOM;
		return -1;
	}
	bc_number_init(&q);
	bc_number_init(&r);
	if (dividend->sign != 0 &&
	    number_divide_absolute(&q, &r, dividend, divisor) != 0)
		return -1;
	if (q.length != 0)
		q.sign = dividend->sign * divisor->sign;
	if (r.length != 0)
		r.sign = dividend->sign;
	number_move(quotient, &q);
	number_move(remainder, &r);
	return 0;
}

int
bc_number_is_zero(const struct bc_number *number)
{
	return number->sign == 0;
}

int
bc_number_to_ull(const struct bc_number *number, unsigned long long *value)
{
	unsigned long long result = 0;
	size_t index = number->length;

	if (number->sign < 0)
		return -1;
	while (index != 0) {
		index--;
		if (result > (ULLONG_MAX - number->digit[index]) / BC_BASE)
			return -1;
		result = result * BC_BASE + number->digit[index];
	}
	*value = result;
	return 0;
}

int
bc_number_power(struct bc_number *destination, const struct bc_number *base,
		const struct bc_number *exponent)
{
	struct bc_number result;
	struct bc_number factor;
	unsigned long long power;

	if (bc_number_to_ull(exponent, &power) != 0 || power > 100000U) {
		errno = EOVERFLOW;
		return -1;
	}
	bc_number_init(&result);
	bc_number_init(&factor);
	if (bc_number_from_decimal(&result, "1", 1) != 0 ||
	    bc_number_copy(&factor, base) != 0)
		goto fail;
	while (power != 0) {
		if ((power & 1U) != 0 &&
		    bc_number_multiply(&result, &result, &factor) != 0)
			goto fail;
		power >>= 1;
		if (power != 0 &&
		    bc_number_multiply(&factor, &factor, &factor) != 0)
			goto fail;
	}
	number_move(destination, &result);
	bc_number_free(&factor);
	return 0;

fail:
	bc_number_free(&result);
	bc_number_free(&factor);
	return -1;
}

char *
bc_number_to_string(const struct bc_number *number)
{
	size_t maximum;
	size_t offset = 0;
	size_t index;
	char *text;
	int count;

	if (number->length > (SIZE_MAX - 3) / 9) {
		errno = EOVERFLOW;
		return NULL;
	}
	maximum = number->length * 9 + 3;
	text = malloc(maximum);
	if (text == NULL)
		return NULL;
	if (number->sign == 0) {
		strcpy(text, "0");
		return text;
	}
	if (number->sign < 0)
		text[offset++] = '-';
	index = number->length - 1;
	count = snprintf(text + offset, maximum - offset, "%u",
			 number->digit[index]);
	if (count < 0 || (size_t)count >= maximum - offset)
		goto fail;
	offset += (size_t)count;
	while (index != 0) {
		index--;
		count = snprintf(text + offset, maximum - offset, "%09u",
				 number->digit[index]);
		if (count != 9 || (size_t)count >= maximum - offset)
			goto fail;
		offset += (size_t)count;
	}
	return text;

fail:
	free(text);
	errno = EOVERFLOW;
	return NULL;
}
