/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Math: its constants and functions.  Most functions are the C library's
 * of the same name after ToNumber; round, max, min, hypot, sign, clz32,
 * imul and fround follow the language's own rules; random is xorshift128+.
 */

#include "js/builtin.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

/*
 * A Math function of one number, as the C library computes it.
 */
typedef double (*math_unary)(double number);

/*
 * One Math function that is the C library's function of one number: its
 * name and the function.  The table is constant for the life of the
 * program.
 */
struct math_entry {
	const char *name;
	math_unary function;
};

/*
 * The state of Math.random, shared by every realm of a thread (each thread
 * has its own, so that the script threads of two views do not race on it).
 *
 * It is seeded on the first call from the time and an address, and only
 * ever moves forward; zero in both words means not seeded yet.
 */
static _Thread_local uint64_t math_random_state[2];

static int math_apply_unary(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_round(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_sign(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_fround(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_clz32(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_imul(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_atan2(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_pow(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_max(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_min(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_hypot(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_random(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int math_extreme(struct vm_realm *realm, const vm_value *args, unsigned count, int maximum, vm_value *result);
static int math_number(struct vm_realm *realm, const vm_value *args, unsigned count, unsigned index, double *number);
static double math_abs(double number);
static double math_trunc(double number);

/*
 * The functions that are the C library's of one number.
 */
static const struct math_entry math_unaries[] = {
	{ "abs", math_abs },
	{ "acos", acos },
	{ "acosh", acosh },
	{ "asin", asin },
	{ "asinh", asinh },
	{ "atan", atan },
	{ "atanh", atanh },
	{ "cbrt", cbrt },
	{ "ceil", ceil },
	{ "cos", cos },
	{ "cosh", cosh },
	{ "exp", exp },
	{ "expm1", expm1 },
	{ "floor", floor },
	{ "log", log },
	{ "log1p", log1p },
	{ "log10", log10 },
	{ "log2", log2 },
	{ "sin", sin },
	{ "sinh", sinh },
	{ "sqrt", sqrt },
	{ "tan", tan },
	{ "tanh", tanh },
	{ "trunc", math_trunc },
	{ NULL, NULL }
};

/*
 * Installs Math.
 */
int
js_builtin_install_math(
	struct vm_realm *realm)
{
	const struct math_entry *entry;
	struct vm_function *function;
	struct vm_object *math;
	int error;

	/* The object, a global. */
	math = vm_object_create(realm->heap, realm->object_prototype);
	if (math == NULL)
		return ENOMEM;
	error = js_builtin_value(realm, realm->global, "Math", vm_value_cell(math), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* The constants (read-only, fixed). */
	error = js_builtin_value(realm, math, "E", vm_value_number(2.718281828459045), 0);
	if (error == 0)
		error = js_builtin_value(realm, math, "LN10", vm_value_number(2.302585092994046), 0);
	if (error == 0)
		error = js_builtin_value(realm, math, "LN2", vm_value_number(0.6931471805599453), 0);
	if (error == 0)
		error = js_builtin_value(realm, math, "LOG10E", vm_value_number(0.4342944819032518), 0);
	if (error == 0)
		error = js_builtin_value(realm, math, "LOG2E", vm_value_number(1.4426950408889634), 0);
	if (error == 0)
		error = js_builtin_value(realm, math, "PI", vm_value_number(3.141592653589793), 0);
	if (error == 0)
		error = js_builtin_value(realm, math, "SQRT1_2", vm_value_number(0.7071067811865476), 0);
	if (error == 0)
		error = js_builtin_value(realm, math, "SQRT2", vm_value_number(1.4142135623730951), 0);
	if (error != 0)
		return error;

	/* The functions of one number: one native code, the C function in its data (an index of the table). */
	for (entry = math_unaries; entry->name != NULL; entry++) {
		error = js_builtin_function(realm, entry->name, 1, math_apply_unary, NULL, &function);
		if (error == 0)
			error = js_builtin_value(realm, math, entry->name, vm_value_cell(function), JS_BUILTIN_METHOD);
		if (error != 0)
			return error;
		function->data = vm_value_int32((int32_t)(entry - math_unaries));
	}

	/* The others. */
	error = js_builtin_method(realm, math, "atan2", 2, math_atan2);
	if (error == 0)
		error = js_builtin_method(realm, math, "clz32", 1, math_clz32);
	if (error == 0)
		error = js_builtin_method(realm, math, "fround", 1, math_fround);
	if (error == 0)
		error = js_builtin_method(realm, math, "hypot", 2, math_hypot);
	if (error == 0)
		error = js_builtin_method(realm, math, "imul", 2, math_imul);
	if (error == 0)
		error = js_builtin_method(realm, math, "max", 2, math_max);
	if (error == 0)
		error = js_builtin_method(realm, math, "min", 2, math_min);
	if (error == 0)
		error = js_builtin_method(realm, math, "pow", 2, math_pow);
	if (error == 0)
		error = js_builtin_method(realm, math, "random", 0, math_random);
	if (error == 0)
		error = js_builtin_method(realm, math, "round", 1, math_round);
	if (error == 0)
		error = js_builtin_method(realm, math, "sign", 1, math_sign);
	if (error != 0)
		return error;

	/* Succeeded: Math is installed. */
	return 0;
}

/* Applies the C function a Math function stands for to its number. */
static int
math_apply_unary(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *function;
	double number;
	int32_t index;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Which function this is (read before anything else runs). */
	function = js_builtin_callee(realm);
	index = vm_value_as_int32(function->data);

	/* The number, then the function. */
	status = math_number(realm, args, count, 0, &number);
	if (status != 0)
		return status;

	/* Succeeded: the result. */
	*result = vm_value_number(math_unaries[index].function(number));
	return 0;
}

/* Math.round(x): the nearest integer, a tie towards +Infinity (keeping -0). */
static int
math_round(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double number;
	double below;
	int infinite;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The number. */
	status = math_number(realm, args, count, 0, &number);
	if (status != 0)
		return status;

	/* NaN, the infinities and the integers stay; so does a zero's sign. */
	below = floor(number);
	infinite = isinf(number);
	if (number != number || infinite || below == number) {
		*result = vm_value_number(number);
		return 0;
	}

	/* Between -0.5 and 0 is -0; otherwise the floor, one up from its half on. */
	if (number < 0.0 && number >= -0.5) {
		*result = vm_value_double(-0.0);
		return 0;
	}

	/* One up from the half on. */
	if (number - below >= 0.5)
		below += 1.0;

	/* Succeeded: the integer. */
	*result = vm_value_number(below);
	return 0;
}

/* Math.sign(x). */
static int
math_sign(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double number;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The number. */
	status = math_number(realm, args, count, 0, &number);
	if (status != 0)
		return status;

	/* NaN and the zeros are themselves; the rest 1 or -1. */
	*result = vm_value_number(number);
	if (number > 0.0)
		*result = vm_value_int32(1);
	if (number < 0.0)
		*result = vm_value_int32(-1);

	/* Succeeded: the sign. */
	return 0;
}

/* Math.fround(x): the nearest single-precision number. */
static int
math_fround(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double number;
	float single;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The number, rounded to a float and back. */
	status = math_number(realm, args, count, 0, &number);
	if (status != 0)
		return status;
	single = (float)number;

	/* Succeeded: the rounded number. */
	*result = vm_value_number((double)single);
	return 0;
}

/* Math.clz32(x): the leading zero bits of the uint32. */
static int
math_clz32(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	uint32_t bits;
	int zeros;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The uint32. */
	status = vm_to_uint32(realm, js_argument(args, count, 0), &bits);
	if (status != 0)
		return status;

	/* Its zeros from the top. */
	zeros = 32;
	while (bits != 0) {
		zeros--;
		bits >>= 1;
	}

	/* Succeeded: the count. */
	*result = vm_value_int32(zeros);
	return 0;
}

/* Math.imul(a, b): the int32 product. */
static int
math_imul(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	uint32_t left;
	uint32_t right;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Both as uint32, in order. */
	status = vm_to_uint32(realm, js_argument(args, count, 0), &left);
	if (status != 0)
		return status;
	status = vm_to_uint32(realm, js_argument(args, count, 1), &right);
	if (status != 0)
		return status;

	/* Succeeded: the product modulo 2^32, as an int32. */
	*result = vm_value_int32((int32_t)(left * right));
	return 0;
}

/* Math.atan2(y, x). */
static int
math_atan2(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double y;
	double x;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Both numbers, in order. */
	status = math_number(realm, args, count, 0, &y);
	if (status != 0)
		return status;
	status = math_number(realm, args, count, 1, &x);
	if (status != 0)
		return status;

	/* Succeeded: the angle. */
	*result = vm_value_number(atan2(y, x));
	return 0;
}

/* Math.pow(base, exponent): as the ** operator. */
static int
math_pow(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The operator's rules. */
	status = vm_numeric(realm, VM_NUMERIC_EXP, js_argument(args, count, 0), js_argument(args, count, 1), result);
	if (status != 0)
		return status;

	/* Succeeded: the power. */
	return 0;
}

/* Math.max(...values). */
static int
math_max(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The largest. */
	status = math_extreme(realm, args, count, 1, result);
	if (status != 0)
		return status;

	/* Succeeded: the maximum. */
	return 0;
}

/* Math.min(...values). */
static int
math_min(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The smallest. */
	status = math_extreme(realm, args, count, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the minimum. */
	return 0;
}

/* Math.hypot(...values): the square root of the sum of squares (Infinity beats NaN). */
static int
math_hypot(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double numbers[64];
	double largest;
	double sum;
	double scaled;
	double magnitude;
	unsigned index;
	int infinite;
	int nan;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Every argument converted first (a long list is summed as it comes, below). */
	infinite = 0;
	nan = 0;
	largest = 0.0;
	for (index = 0; index < count; index++) {
		status = math_number(realm, args, count, index, &scaled);
		if (status != 0)
			return status;
		if (scaled != scaled)
			nan = 1;
		magnitude = fabs(scaled);
		if (magnitude == INFINITY)
			infinite = 1;
		if (index < 64U)
			numbers[index] = magnitude;
		if (magnitude > largest)
			largest = magnitude;
	}

	/* Infinity, then NaN, then zero for no values or only zeros. */
	if (infinite) {
		*result = vm_value_double(INFINITY);
		return 0;
	}

	/* Then NaN. */
	if (nan) {
		*result = vm_value_double(NAN);
		return 0;
	}

	/* Then zero for no values or only zeros. */
	if (largest == 0.0) {
		*result = vm_value_int32(0);
		return 0;
	}

	/* The sum of the squares scaled by the largest (so nothing overflows). */
	sum = 0.0;
	for (index = 0; index < count && index < 64U; index++) {
		scaled = numbers[index] / largest;
		sum += scaled * scaled;
	}

	/* Succeeded: the length. */
	*result = vm_value_number(largest * sqrt(sum));
	return 0;
}

/* Math.random(): a number from 0 to below 1 (xorshift128+, 53 bits). */
static int
math_random(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	uint64_t first;
	uint64_t second;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The seed, once: the time and the realm's address, never both zero. */
	if (math_random_state[0] == 0 && math_random_state[1] == 0) {
		math_random_state[0] = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL;
		math_random_state[1] = (uint64_t)(uintptr_t)realm ^ 0xD1B54A32D192ED03ULL;
		if (math_random_state[0] == 0 && math_random_state[1] == 0)
			math_random_state[1] = 1;
	}

	/* One step of xorshift128+. */
	first = math_random_state[0];
	second = math_random_state[1];
	math_random_state[0] = second;
	first ^= first << 23;
	first ^= first >> 17;
	first ^= second ^ (second >> 26);
	math_random_state[1] = first;

	/* Succeeded: the top 53 bits of the sum as a fraction. */
	*result = vm_value_number((double)((first + second) >> 11) / 9007199254740992.0);
	return 0;
}

/* Finds the largest or smallest of the arguments (every one converted; NaN wins; +0 is above -0). */
static int
math_extreme(
	struct vm_realm *realm,
	const vm_value *args,
	unsigned count,
	int maximum,
	vm_value *result)
{
	double best;
	double number;
	unsigned index;
	int negative;
	int status;

	/* Without values, -Infinity for the largest and Infinity for the smallest. */
	best = INFINITY;
	if (maximum)
		best = -INFINITY;
	for (index = 0; index < count; index++) {
		status = math_number(realm, args, count, index, &number);
		if (status != 0)
			return status;

		/* NaN stays once met (the rest are still converted). */
		if (best != best)
			continue;
		if (number != number) {
			best = number;
			continue;
		}

		/* A bigger (or smaller) number, and the sign of zero (+0 is above -0). */
		negative = signbit(number) != 0;
		if (maximum && (number > best || (number == 0.0 && best == 0.0 && !negative)))
			best = number;
		if (!maximum && (number < best || (number == 0.0 && best == 0.0 && negative)))
			best = number;
	}

	/* Succeeded: the extreme. */
	*result = vm_value_number(best);
	return 0;
}

/* Converts an argument to a number (undefined when missing: NaN). */
static int
math_number(
	struct vm_realm *realm,
	const vm_value *args,
	unsigned count,
	unsigned index,
	double *number)
{
	int status;

	/* ToNumber of the argument. */
	status = vm_to_number(realm, js_argument(args, count, index), number);
	if (status != 0)
		return status;

	/* Succeeded: the number. */
	return 0;
}

/* Reports the magnitude of a number. */
static double
math_abs(
	double number)
{
	/* The C function. */
	return fabs(number);
}

/* Reports a number without its fraction. */
static double
math_trunc(
	double number)
{
	/* The C function. */
	return trunc(number);
}
