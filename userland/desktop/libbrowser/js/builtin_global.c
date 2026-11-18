/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The global functions: parseInt, parseFloat, isNaN, isFinite (and
 * Number.parseInt and Number.parseFloat, the same function objects), and
 * eval.
 *
 * eval evaluates its string as global code (what the language calls an
 * indirect eval: a direct eval, which sees the caller's variables, comes
 * later); its value is the completion of the last expression statement.
 * The Function constructor evaluates its source the same way.
 */

#include "js/builtin.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* The longest numeral parseInt and parseFloat read (the rest of a longer one only rounds). */
#define GLOBAL_NUMERAL_MAX	2000U

static int global_parse_int(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int global_parse_float(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int global_is_nan(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int global_is_finite(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int global_eval(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static uint32_t global_skip_space(const struct vm_string *string, uint32_t index);
static int global_digit(uint16_t unit, int radix);
static uint16_t global_unit(const struct vm_string *string, uint32_t index);
static uint32_t global_copy_digits(const struct vm_string *string, uint32_t *index, char *numeral, uint32_t *length);
static int global_define_both(struct vm_realm *realm, struct vm_object *number, const char *name, unsigned length, vm_native native);

/*
 * Installs the global functions.
 */
int
js_builtin_install_global(
	struct vm_realm *realm)
{
	struct vm_object *number;
	vm_value key;
	vm_value value;
	int error;

	/* Number, which shares parseInt and parseFloat. */
	key = vm_key_from_ascii(realm->heap, "Number");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	error = vm_object_get(realm->global, key, &value);
	if (error != 0)
		return error;
	number = (struct vm_object *)vm_value_as_cell(value);

	/* The functions. */
	error = global_define_both(realm, number, "parseInt", 2, global_parse_int);
	if (error == 0)
		error = global_define_both(realm, number, "parseFloat", 1, global_parse_float);
	if (error == 0)
		error = js_builtin_method(realm, realm->global, "isNaN", 1, global_is_nan);
	if (error == 0)
		error = js_builtin_method(realm, realm->global, "isFinite", 1, global_is_finite);
	if (error == 0)
		error = js_builtin_method(realm, realm->global, "eval", 1, global_eval);
	if (error != 0)
		return error;

	/* Succeeded: the global functions are installed. */
	return 0;
}

/*
 * Evaluates source text as global code (strict when asked or when it says
 * so) and stores its completion value; a syntax error is thrown as a
 * SyntaxError.
 */
int
js_builtin_evaluate(
	struct vm_realm *realm,
	const struct vm_string *source,
	int strict,
	vm_value *result)
{
	struct wb_units units;
	struct js_syntax_error error;
	char message[200];
	unsigned how;
	int status;

	/* The source's characters. */
	wb_units_init(&units);
	status = vm_string_append_units(source, &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* A developer's diagnosis: BROWSER_EVAL_DUMP names a file that receives each evaluated source. */
	if (getenv("BROWSER_EVAL_DUMP") != NULL) {
		FILE *dump;
		size_t at;

		dump = fopen(getenv("BROWSER_EVAL_DUMP"), "ab");
		if (dump != NULL) {
			for (at = 0; at < units.length; at++)
				fputc(units.data[at] < 0x7fU ? (int)units.data[at] : '?', dump);
			fputs("\n=====\n", dump);
			fclose(dump);
		}
	}

	/* The run, as a script of the realm. */
	how = 0;
	if (strict)
		how = JS_PARSE_STRICT;
	status = js_run_script(realm, units.data, units.length, how, result, &error);
	wb_units_release(&units);

	/* A syntax error is a SyntaxError; what is not supported yet an Error. */
	if (status == EINVAL && error.unsupported) {
		status = vm_throw_error(realm, VM_ERROR_PLAIN, error.message);
		return status;
	}

	/* A syntax error. */
	if (status == EINVAL) {
		snprintf(message, sizeof(message), "%s", error.message);
		status = vm_throw_error(realm, VM_ERROR_SYNTAX, message);
		return status;
	}

	/* Any other failure. */
	if (status != 0)
		return status;

	/* Succeeded: the completion value. */
	return 0;
}

/* parseInt(string, radix): the longest prefix of digits in the radix (0x for 16), after blanks and a sign. */
static int
global_parse_int(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	char digits[GLOBAL_NUMERAL_MAX + 1U];
	uint32_t index;
	uint32_t length;
	uint16_t unit;
	uint16_t next;
	int32_t radix;
	int negative;
	int strip_prefix;
	int digit;
	double number;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The string, and the radix. */
	status = vm_to_string(realm, js_argument(args, count, 0), &string);
	if (status != 0)
		return status;
	status = vm_to_int32(realm, js_argument(args, count, 1), &radix);
	if (status != 0)
		return status;

	/* Blanks, then a sign. */
	*result = vm_value_double(NAN);
	index = global_skip_space(string, 0);
	negative = 0;
	unit = global_unit(string, index);
	if (unit == '-' || unit == '+') {
		if (unit == '-')
			negative = 1;
		index++;
	}

	/* The radix: 2 to 36, or 0 for 10 (16 with a 0x prefix). */
	strip_prefix = 1;
	if (radix != 0) {
		if (radix < 2 || radix > 36)
			return 0;
		if (radix != 16)
			strip_prefix = 0;
	} else {
		radix = 10;
	}

	/* The prefix 0x or 0X. */
	unit = global_unit(string, index);
	next = global_unit(string, index + 1U);
	if (strip_prefix && unit == '0' && (next | 0x20U) == 'x') {
		index += 2U;
		radix = 16;
	}

	/* The digits in the radix. */
	length = 0;
	for (;;) {
		unit = global_unit(string, index);
		digit = global_digit(unit, radix);
		if (digit < 0)
			break;
		if (length < GLOBAL_NUMERAL_MAX) {
			digits[length] = (char)unit;
			length++;
		}

		/* The next character. */
		index++;
	}

	/* No digit: NaN. */
	if (length == 0)
		return 0;

	/* The number, read exactly and rounded once, with its sign. */
	number = vm_number_parse_radix(digits, length, radix);
	if (negative)
		number = -number;

	/* Succeeded: the number. */
	*result = vm_value_number(number);
	return 0;
}

/* parseFloat(string): the longest prefix that is a decimal numeral (or Infinity), after blanks. */
static int
global_parse_float(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	char numeral[GLOBAL_NUMERAL_MAX + 1U];
	static const char infinity[] = "Infinity";
	uint32_t index;
	uint32_t mark;
	uint32_t length;
	uint32_t digits;
	uint32_t exponent_digits;
	uint32_t letter;
	uint16_t unit;
	int negative;
	double number;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The string, blanks skipped, and a sign. */
	status = vm_to_string(realm, js_argument(args, count, 0), &string);
	if (status != 0)
		return status;
	*result = vm_value_double(NAN);
	index = global_skip_space(string, 0);
	negative = 0;
	unit = global_unit(string, index);
	if (unit == '-' || unit == '+') {
		if (unit == '-')
			negative = 1;
		index++;
	}

	/* Infinity, spelled out. */
	for (letter = 0; letter < 8U; letter++) {
		unit = global_unit(string, index + letter);
		if (unit != (uint16_t)infinity[letter])
			break;
	}

	/* Infinity with its sign. */
	if (letter == 8U) {
		number = INFINITY;
		if (negative)
			number = -INFINITY;
		*result = vm_value_double(number);
		return 0;
	}

	/* The digits and an optional point with more digits. */
	length = 0;
	digits = global_copy_digits(string, &index, numeral, &length);
	unit = global_unit(string, index);
	if (unit == '.' && length < GLOBAL_NUMERAL_MAX) {
		numeral[length] = '.';
		length++;
		index++;
		digits += global_copy_digits(string, &index, numeral, &length);
	}

	/* No digit: NaN. */
	if (digits == 0)
		return 0;

	/* An exponent only when it has digits. */
	unit = global_unit(string, index);
	if ((unit | 0x20U) == 'e' && length + 2U < GLOBAL_NUMERAL_MAX) {
		mark = length;
		numeral[length] = 'e';
		length++;
		index++;
		unit = global_unit(string, index);
		if (unit == '+' || unit == '-') {
			numeral[length] = (char)unit;
			length++;
			index++;
		}

		/* The exponent digits; none drops the e. */
		exponent_digits = global_copy_digits(string, &index, numeral, &length);
		if (exponent_digits == 0)
			length = mark;
	}

	/* The number, read exactly, with its sign. */
	number = vm_number_parse(numeral, length);
	if (negative)
		number = -number;

	/* Succeeded: the number. */
	*result = vm_value_number(number);
	return 0;
}

/* isNaN(value): whether ToNumber gives NaN. */
static int
global_is_nan(
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
	status = vm_to_number(realm, js_argument(args, count, 0), &number);
	if (status != 0)
		return status;

	/* Succeeded: whether it is NaN. */
	*result = vm_value_boolean(number != number);
	return 0;
}

/* isFinite(value): whether ToNumber gives a finite number. */
static int
global_is_finite(
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
	status = vm_to_number(realm, js_argument(args, count, 0), &number);
	if (status != 0)
		return status;

	/* Succeeded: whether it is finite. */
	*result = vm_value_boolean(number == number && !isinf(number));
	return 0;
}

/* eval(x): a string evaluated as global code; anything else is returned as it is. */
static int
global_eval(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value source;
	int is_string;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Anything but a string is its own value. */
	source = js_argument(args, count, 0);
	*result = source;
	is_string = vm_value_is_string(source);
	if (!is_string)
		return 0;

	/* The string as global code. */
	status = js_builtin_evaluate(realm, (const struct vm_string *)vm_value_as_cell(source), 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the completion value. */
	return 0;
}

/* Skips white space and line terminators from an index; reports the index after them. */
static uint32_t
global_skip_space(
	const struct vm_string *string,
	uint32_t index)
{
	uint16_t unit;
	int blank;

	/* While the character is a blank. */
	while (index < string->length) {
		unit = vm_string_at(string, index);
		blank = vm_is_space(unit);
		if (!blank)
			break;
		index++;
	}

	/* The first other character. */
	return index;
}

/* Reports a character's digit value in a radix, or -1 when it is not one. */
static int
global_digit(
	uint16_t unit,
	int radix)
{
	int digit;

	/* 0 to 9, then letters of either case. */
	digit = -1;
	if (unit >= '0' && unit <= '9')
		digit = unit - '0';
	if ((unit | 0x20U) >= 'a' && (unit | 0x20U) <= 'z')
		digit = (int)((unit | 0x20U) - 'a') + 10;

	/* Only below the radix. */
	if (digit >= radix)
		return -1;

	/* The value. */
	return digit;
}

/* Defines a function both as a global and on Number (the same function object). */
static int
global_define_both(
	struct vm_realm *realm,
	struct vm_object *number,
	const char *name,
	unsigned length,
	vm_native native)
{
	struct vm_function *function;
	int error;

	/* The function, then the two properties. */
	error = js_builtin_function(realm, name, length, native, NULL, &function);
	if (error == 0)
		error = js_builtin_value(realm, realm->global, name, vm_value_cell(function), JS_BUILTIN_METHOD);
	if (error == 0)
		error = js_builtin_value(realm, number, name, vm_value_cell(function), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* Succeeded: the function is defined. */
	return 0;
}

/* Reports a string's code unit at an index, or 0 past its end. */
static uint16_t
global_unit(
	const struct vm_string *string,
	uint32_t index)
{
	/* Past the end. */
	if (index >= string->length)
		return 0;

	/* The unit. */
	return vm_string_at(string, index);
}

/* Copies a run of decimal digits into a numeral (up to its size); reports how many there were. */
static uint32_t
global_copy_digits(
	const struct vm_string *string,
	uint32_t *index,
	char *numeral,
	uint32_t *length)
{
	uint32_t count;
	uint16_t unit;

	/* Each digit from the index. */
	count = 0;
	for (;;) {
		unit = global_unit(string, *index);
		if (unit < '0' || unit > '9' || *length >= GLOBAL_NUMERAL_MAX)
			break;
		numeral[*length] = (char)unit;
		(*length)++;
		(*index)++;
		count++;
	}

	/* The count. */
	return count;
}
