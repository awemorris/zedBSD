/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland number component.
 */

#include "userland/base/bc/number.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BC_BASE 1000000000U

static void number_move(struct bc_number *destination, struct bc_number *source);
static int number_reserve(struct bc_number *number, size_t needed);
static int number_add_absolute(struct bc_number *result, const struct bc_number *left, const struct bc_number *right);
static int number_compare_absolute(const struct bc_number *left, const struct bc_number *right);
static void number_subtract_absolute(struct bc_number *result, const struct bc_number *left, const struct bc_number *right);
static void number_normalize(struct bc_number *number);
static int number_divide_absolute(struct bc_number *quotient, struct bc_number *remainder, const struct bc_number *dividend, const struct bc_number *divisor);
static int number_shift_base(struct bc_number *number, uint32_t low);
static int number_multiply_small(struct bc_number *destination, const struct bc_number *source, uint32_t factor);

/*
 * Implements the bc number init operation.
 */
void
bc_number_init(
	struct bc_number *number)
{
	memset(number, 0, sizeof(*number));
}

/*
 * Implements the bc number free operation.
 */
void
bc_number_free(
	struct bc_number *number)
{
	free(number->digit);
	bc_number_init(number);
}

/*
 * Implements the bc number from decimal operation.
 */
int
bc_number_from_decimal(
	struct bc_number *number,
	const char *text,
	size_t length)
{
	uint64_t value;
	uint64_t carry;
	size_t index;
	struct bc_number result;
	size_t offset;
	int sign;

	offset = 0;
	sign = 1;

	bc_number_init(&result);

	/* Checks the current data length. */
	if (length != 0 && (*text == '+' || *text == '-')) {
		sign = *text == '-' ? -1 : 1;
		text++;
		length--;
	}

	/* Checks the current data length. */
	if (length == 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	while (offset < length && text[offset] == '0')
		offset++;

	/* Checks the current offset. */
	if (offset == length) {
		number_move(number, &result);

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining element. */
	for (; offset < length; offset++) {
		/* Validates the current text. */
		if (text[offset] < '0' || text[offset] > '9') {
			bc_number_free(&result);
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}

		/* Process each remaining element. */
		carry = (unsigned)(text[offset] - '0');
		for (index = 0; index < result.length; index++) {
			value = (uint64_t)result.digit[index] * 10U + carry;
			result.digit[index] = (uint32_t)(value % BC_BASE);
			carry = value / BC_BASE;
		}

		/* Handles the carry condition. */
		if (carry != 0) {
			/* Handles a failed number reserve operation. */
			if (number_reserve(&result, result.length + 1) != 0) {
				bc_number_free(&result);

				/* Reports operation failure. */
				return -1;
			}
			result.digit[result.length++] = (uint32_t)carry;
		}
	}
	result.sign = sign;
	number_move(number, &result);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the bc number copy operation.
 */
int
bc_number_copy(
	struct bc_number *destination,
	const struct bc_number *source)
{
	struct bc_number result;

	bc_number_init(&result);

	/* Handles the source condition. */
	if (source->length != 0) {
		/* Handles a failed number reserve operation. */
		if (number_reserve(&result, source->length) != 0)
			return -1;
		memcpy(result.digit, source->digit,
		       source->length * sizeof(*source->digit));
		result.length = source->length;
		result.sign = source->sign;
	}
	number_move(destination, &result);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the bc number add operation.
 */
int
bc_number_add(
	struct bc_number *destination,
	const struct bc_number *left,
	const struct bc_number *right)
{
	int function_result;
	struct bc_number result;
	int comparison;

	bc_number_init(&result);

	/* Handles the left condition. */
	if (left->sign == 0) {
		/* Obtains the bc number copy result. */
		function_result = bc_number_copy(destination, right);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the right condition. */
	if (right->sign == 0) {
		/* Obtains the bc number copy result. */
		function_result = bc_number_copy(destination, left);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the left condition. */
	if (left->sign == right->sign) {
		/* Handles a failed number add absolute operation. */
		if (number_add_absolute(&result, left, right) != 0)
			return -1;
		result.sign = left->sign;
	} else {
		comparison = number_compare_absolute(left, right);

		/* Handles the comparison condition. */
		if (comparison == 0) {
			number_move(destination, &result);

			/* Reports successful completion. */
			return 0;
		}

		/* Handles a failed number reserve operation. */
		if (number_reserve(&result, comparison > 0
						? left->length
						: right->length) != 0)

			/* Reports operation failure. */
			return -1;

		/* Handles the comparison condition. */
		if (comparison > 0) {
			number_subtract_absolute(&result, left, right);
			result.sign = left->sign;
		} else {
			number_subtract_absolute(&result, right, left);
			result.sign = right->sign;
		}
	}
	number_move(destination, &result);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the bc number negate operation.
 */
int
bc_number_negate(
	struct bc_number *destination,
	const struct bc_number *source)
{
	/* Handles a failed bc number copy operation. */
	if (bc_number_copy(destination, source) != 0)
		return -1;
	destination->sign = -destination->sign;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the bc number subtract operation.
 */
int
bc_number_subtract(
	struct bc_number *destination,
	const struct bc_number *left,
	const struct bc_number *right)
{
	struct bc_number negative;
	int result;

	bc_number_init(&negative);

	/* Handles a failed bc number negate operation. */
	if (bc_number_negate(&negative, right) != 0)
		return -1;
	result = bc_number_add(destination, left, &negative);
	bc_number_free(&negative);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the bc number multiply operation.
 */
int
bc_number_multiply(
	struct bc_number *destination,
	const struct bc_number *left,
	const struct bc_number *right)
{
	uint64_t value;
	uint64_t carry;
	size_t j;
	struct bc_number result;
	size_t i;

	bc_number_init(&result);

	/* Handles the left condition. */
	if (left->sign == 0 || right->sign == 0) {
		number_move(destination, &result);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed number reserve operation. */
	if (left->length > SIZE_MAX - right->length ||
	    number_reserve(&result, left->length + right->length) != 0)

		/* Reports operation failure. */
		return -1;
	memset(result.digit, 0,
	       (left->length + right->length) * sizeof(*result.digit));

	/* Process each remaining element. */
	result.length = left->length + right->length;
	for (i = 0; i < left->length; i++) {
		carry = 0;

		/* Process each remaining element. */
		for (j = 0; j < right->length; j++) {
			value = result.digit[i + j] + carry +
		    (uint64_t)left->digit[i] * right->digit[j];

			result.digit[i + j] = (uint32_t)(value % BC_BASE);
			carry = value / BC_BASE;
		}
		result.digit[i + right->length] = (uint32_t)carry;
	}
	result.sign = left->sign * right->sign;
	number_normalize(&result);
	number_move(destination, &result);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the bc number divide operation.
 */
int
bc_number_divide(
	struct bc_number *quotient,
	struct bc_number *remainder,
	const struct bc_number *dividend,
	const struct bc_number *divisor)
{
	struct bc_number q;
	struct bc_number r;

	/* Handles the divisor condition. */
	if (divisor->sign == 0) {
		errno = EDOM;

		/* Reports operation failure. */
		return -1;
	}
	bc_number_init(&q);
	bc_number_init(&r);

	/* Handles a failed number divide absolute operation. */
	if (dividend->sign != 0 &&
	    number_divide_absolute(&q, &r, dividend, divisor) != 0)

		/* Reports operation failure. */
		return -1;

	/* Handles the q condition. */
	if (q.length != 0)
		q.sign = dividend->sign * divisor->sign;

	/* Handles the r condition. */
	if (r.length != 0)
		r.sign = dividend->sign;
	number_move(quotient, &q);
	number_move(remainder, &r);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the bc number is zero operation.
 */
int
bc_number_is_zero(
	const struct bc_number *number)
{
	/* Returns the computed result. */
	return number->sign == 0;
}

/*
 * Implements the bc number to ull operation.
 */
int
bc_number_to_ull(
	const struct bc_number *number,
	unsigned long long *value)
{
	unsigned long long result;
	size_t index;

	result = 0;
	index = number->length;

	/* Handles the number condition. */
	if (number->sign < 0)
		return -1;

	/* Process each remaining element. */
	while (index != 0) {
		index--;

		/* Checks the operation result. */
		if (result > (ULLONG_MAX - number->digit[index]) / BC_BASE)
			return -1;
		result = result * BC_BASE + number->digit[index];
	}
	*value = result;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the bc number power operation.
 */
int
bc_number_power(
	struct bc_number *destination,
	const struct bc_number *base,
	const struct bc_number *exponent)
{
	struct bc_number result;
	struct bc_number factor;
	unsigned long long power;

	/* Handles a failed bc number to ull operation. */
	if (bc_number_to_ull(exponent, &power) != 0 || power > 100000U) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	bc_number_init(&result);
	bc_number_init(&factor);

	/* Handles a failed bc number from decimal operation. */
	if (bc_number_from_decimal(&result, "1", 1) != 0 ||
	    bc_number_copy(&factor, base) != 0)
		goto fail;

	/* Continue while the operation condition remains true. */
	while (power != 0) {
		/* Handles a failed bc number multiply operation. */
		if ((power & 1U) != 0 &&
		    bc_number_multiply(&result, &result, &factor) != 0)
			goto fail;
		power >>= 1;

		/* Handles a failed bc number multiply operation. */
		if (power != 0 &&
		    bc_number_multiply(&factor, &factor, &factor) != 0)
			goto fail;
	}
	number_move(destination, &result);
	bc_number_free(&factor);

	/* Reports successful completion. */
	return 0;

fail:
	bc_number_free(&result);
	bc_number_free(&factor);

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the bc number to string operation.
 */
char *
bc_number_to_string(
	const struct bc_number *number)
{
	size_t maximum;
	size_t offset;
	size_t index;
	char *text;
	int count;

	offset = 0;

	/* Handles the number condition. */
	if (number->length > (SIZE_MAX - 3) / 9) {
		errno = EOVERFLOW;

		/* Reports that no result is available. */
		return NULL;
	}
	maximum = number->length * 9 + 3;
	text = malloc(maximum);

	/* Handles the text availability. */
	if (text == NULL)
		return NULL;

	/* Handles the number condition. */
	if (number->sign == 0) {
		strcpy(text, "0");

		/* Returns the computed result. */
		return text;
	}

	/* Handles the number condition. */
	if (number->sign < 0)
		text[offset++] = '-';
	index = number->length - 1;
	count = snprintf(text + offset, maximum - offset, "%u",
			 number->digit[index]);

	/* Checks the remaining item count. */
	if (count < 0 || (size_t)count >= maximum - offset)
		goto fail;
	offset += (size_t)count;

	/* Process each remaining element. */
	while (index != 0) {
		index--;
		count = snprintf(text + offset, maximum - offset, "%09u",
				 number->digit[index]);

		/* Checks the remaining item count. */
		if (count != 9 || (size_t)count >= maximum - offset)
			goto fail;
		offset += (size_t)count;
	}

	/* Returns the computed result. */
	return text;

fail:
	free(text);
	errno = EOVERFLOW;

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the number move operation. */
static void
number_move(
	struct bc_number *destination,
	struct bc_number *source)
{
	bc_number_free(destination);
	*destination = *source;
	bc_number_init(source);
}

/* Supports the number reserve operation. */
static int
number_reserve(
	struct bc_number *number,
	size_t needed)
{
	size_t capacity;
	uint32_t *digit;

	/* Handles the needed condition. */
	if (needed <= number->capacity)
		return 0;

	/* Handles the needed condition. */
	if (needed > SIZE_MAX / sizeof(*digit)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	/* Continue while the operation condition remains true. */
	capacity = number->capacity == 0 ? 4 : number->capacity;
	while (capacity < needed) {
		/* Handles the capacity condition. */
		if (capacity > SIZE_MAX / 2) {
			capacity = needed;
			break;
		}
		capacity *= 2;
	}
	digit = realloc(number->digit, capacity * sizeof(*digit));

	/* Handles the digit availability. */
	if (digit == NULL)
		return -1;
	number->digit = digit;
	number->capacity = capacity;

	/* Reports successful completion. */
	return 0;
}

/* Supports the number add absolute operation. */
static int
number_add_absolute(
	struct bc_number *result,
	const struct bc_number *left,
	const struct bc_number *right)
{
	uint64_t value;
	size_t length;
	size_t index;
	uint64_t carry;

	length = left->length > right->length ? left->length : right->length;
	carry = 0;

	/* Handles a failed number reserve operation. */
	if (number_reserve(result, length + 1) != 0)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < length; index++) {
		value = carry;

		/* Checks the current index. */
		if (index < left->length)
			value += left->digit[index];

		/* Checks the current index. */
		if (index < right->length)
			value += right->digit[index];
		result->digit[index] = (uint32_t)(value % BC_BASE);
		carry = value / BC_BASE;
	}
	result->length = length;

	/* Handles the carry condition. */
	if (carry != 0)
		result->digit[result->length++] = (uint32_t)carry;

	/* Reports successful completion. */
	return 0;
}

/* Supports the number compare absolute operation. */
static int
number_compare_absolute(
	const struct bc_number *left,
	const struct bc_number *right)
{
	size_t index;

	/* Handles the left condition. */
	if (left->length != right->length)
		return left->length < right->length ? -1 : 1;

	/* Process each remaining element. */
	index = left->length;
	while (index != 0) {
		index--;

		/* Handles the left condition. */
		if (left->digit[index] != right->digit[index]) {
			return left->digit[index] < right->digit[index] ? -1
									: 1;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the number subtract absolute operation. */
static void
number_subtract_absolute(
	struct bc_number *result,
	const struct bc_number *left,
	const struct bc_number *right)
{
	uint64_t value;
	uint64_t subtract;
	size_t index;
	uint64_t borrow;

	borrow = 0;

	/* Process each remaining element. */
	for (index = 0; index < left->length; index++) {
		value = left->digit[index];
		subtract = borrow;

		/* Checks the current index. */
		if (index < right->length)
			subtract += right->digit[index];

		/* Validates the current value. */
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

/* Supports the number normalize operation. */
static void
number_normalize(
	struct bc_number *number)
{
	/* Process each remaining element. */
	while (number->length != 0 && number->digit[number->length - 1] == 0)
		number->length--;

	/* Handles the number condition. */
	if (number->length == 0)
		number->sign = 0;
}

/* Supports the number divide absolute operation. */
static int
number_divide_absolute(
	struct bc_number *quotient,
	struct bc_number *remainder,
	const struct bc_number *dividend,
	const struct bc_number *divisor)
{
	struct bc_number product_local;
	struct bc_number product_local1;
	uint32_t middle;
	int comparison;
	uint32_t low;
	uint32_t high;
	uint32_t selected;
	struct bc_number q;
	struct bc_number r;
	size_t index;

	bc_number_init(&q);
	bc_number_init(&r);

	/* Handles a failed number reserve operation. */
	if (number_reserve(&q, dividend->length) != 0)
		goto fail;
	memset(q.digit, 0, dividend->length * sizeof(*q.digit));

	/* Process each remaining element. */
	q.length = dividend->length;
	index = dividend->length;
	while (index != 0) {
		low = 0;
		high = BC_BASE - 1;
		selected = 0;

		index--;

		/* Handles a failed number shift base operation. */
		if (number_shift_base(&r, dividend->digit[index]) != 0)
			goto fail;

		/* Continue while the operation condition remains true. */
		while (low <= high) {
			middle = low + (high - low) / 2;

			bc_number_init(&product_local);

			/* Handles a failed number multiply small operation. */
			if (number_multiply_small(&product_local, divisor, middle) !=
			    0) {
				bc_number_free(&product_local);
				goto fail;
			}
			comparison = number_compare_absolute(&product_local, &r);
			bc_number_free(&product_local);

			/* Handles the comparison condition. */
			if (comparison <= 0) {
				selected = middle;

				/* Handles the middle condition. */
				if (middle == BC_BASE - 1)
					break;
				low = middle + 1;
			} else {
				/* Handles the middle condition. */
				if (middle == 0)
					break;
				high = middle - 1;
			}
		}
		q.digit[index] = selected;

		/* Handles the selected condition. */
		if (selected != 0) {
			bc_number_init(&product_local1);

			/* Handles a failed number multiply small operation. */
			if (number_multiply_small(&product_local1, divisor,
						  selected) != 0) {
				bc_number_free(&product_local1);
				goto fail;
			}
			number_subtract_absolute(&r, &r, &product_local1);
			bc_number_free(&product_local1);
		}
	}
	q.sign = 1;
	r.sign = r.length == 0 ? 0 : 1;
	number_normalize(&q);
	number_move(quotient, &q);
	number_move(remainder, &r);

	/* Reports successful completion. */
	return 0;

fail:
	bc_number_free(&q);
	bc_number_free(&r);

	/* Reports operation failure. */
	return -1;
}

/* Supports the number shift base operation. */
static int
number_shift_base(
	struct bc_number *number,
	uint32_t low)
{
	/* Handles the number condition. */
	if (number->length == 0 && low == 0)
		return 0;

	/* Handles a failed number reserve operation. */
	if (number_reserve(number, number->length + 1) != 0)
		return -1;
	memmove(number->digit + 1, number->digit,
		number->length * sizeof(*number->digit));
	number->digit[0] = low;
	number->length++;
	number->sign = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the number multiply small operation. */
static int
number_multiply_small(
	struct bc_number *destination,
	const struct bc_number *source,
	uint32_t factor)
{
	uint64_t value;
	struct bc_number result;
	uint64_t carry;
	size_t index;

	carry = 0;

	bc_number_init(&result);

	/* Handles the factor condition. */
	if (factor == 0 || source->sign == 0) {
		number_move(destination, &result);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed number reserve operation. */
	if (number_reserve(&result, source->length + 1) != 0)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < source->length; index++) {
		value = (uint64_t)source->digit[index] * factor + carry;

		result.digit[index] = (uint32_t)(value % BC_BASE);
		carry = value / BC_BASE;
	}
	result.length = source->length;

	/* Handles the carry condition. */
	if (carry != 0)
		result.digit[result.length++] = (uint32_t)carry;
	result.sign = 1;
	number_move(destination, &result);

	/* Reports successful completion. */
	return 0;
}
