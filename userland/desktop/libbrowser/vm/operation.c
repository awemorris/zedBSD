/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * JavaScript's operations on values that the interpreter's instructions
 * use: the conversions (ToBoolean, ToPrimitive, ToNumber, ToInt32,
 * ToString, ToPropertyKey), the equalities, addition and the other
 * numeric operators, the relations and typeof, and the errors the engine
 * throws.
 *
 * An object becomes a primitive through its valueOf and toString methods
 * (Symbol.toPrimitive arrives with the symbols of ws074-p028).  Numbers
 * and their text convert exactly (number.c).  The errors are Error objects
 * of the realm once its built-ins are installed, strings ("TypeError:
 * ...") before.
 */

#include "vm/internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest numeral a string is read as (a longer string reads as NaN). */
#define OPERATION_NUMERAL_MAX	2000U

/*
 * The types of the language, as the equality and typeof tell them apart.
 */
enum operation_type {
	OPERATION_UNDEFINED,
	OPERATION_NULL,
	OPERATION_BOOLEAN,
	OPERATION_NUMBER,
	OPERATION_STRING,
	OPERATION_SYMBOL,
	OPERATION_BIGINT,
	OPERATION_OBJECT
};

/* One native ToBigInt frame owns its realm, original source and fresh primitive until conversion completes. */
struct operation_bigint_roots {
	struct vm_realm *realm;
	vm_value source;
	vm_value primitive;
};

static void operation_bigint_trace(struct vm_heap *heap, void *context);
static int operation_type(vm_value value);
static int operation_bigint_string(struct vm_realm *realm, const struct vm_bigint *big, struct vm_string **string);
static int operation_string_bigint(struct vm_realm *realm, vm_value string, vm_value *big);
static int operation_mix_error(struct vm_realm *realm);
static int operation_compare_bigint(struct vm_realm *realm, vm_value left, vm_value right, int *order);
static int operation_exotic_primitive(struct vm_realm *realm, vm_value value, vm_value exotic, int hint, vm_value *result);
static int operation_call_method(struct vm_realm *realm, vm_value object, const char *name, int *done, vm_value *result);
static int operation_number_string(struct vm_realm *realm, double number, struct vm_string **string);
static double operation_parse_number(const struct vm_string *string);
static double operation_parse_radix(const char *text, unsigned radix);
static int operation_is_decimal(const char *text);
static double operation_int32_of(double number);
static int operation_compare(struct vm_realm *realm, vm_value left, vm_value right, int *order);
static int operation_throw_text(struct vm_realm *realm, const char *text);
static double operation_power(double base, double exponent);
static int32_t operation_shift_signed(int32_t number, uint32_t count);
static int operation_name_message(vm_value key, const char *format, char *text, size_t size);

/*
 * Reads a primitive string as a number without invoking script or allocating VM cells.
 * The existing StringToNumber parser owns whitespace, radix and decimal semantics.
 */
double
vm_string_to_number(
	const struct vm_string *string)
{
	double number;

	/* Reuse the established language parser without a synthetic realm or object conversion. */
	number = operation_parse_number(string);

	/* Succeeded: the parsed number, including NaN for invalid numeric text. */
	return number;
}

/*
 * Classifies a valid property key by CanonicalNumericIndexString without script or VM allocation.
 * C buffer allocation preserves the existing exact shortest-number formatter.
 */
int
vm_canonical_numeric_index(
	vm_value key,
	double *number,
	int *canonical)
{
	struct wb_buffer text;
	const struct vm_string *string;
	uint32_t index;
	uint16_t unit;
	int is_int32;
	int is_string;
	int special;
	int status;

	/* Internal integer keys already denote their canonical numeric index. */
	*canonical = 0;
	*number = 0.0;
	is_int32 = vm_value_is_int32(key);
	if (is_int32) {
		*number = (double)vm_value_as_int32(key);
		*canonical = 1;
		return 0;
	}

	/* Symbols have no numeric index, and callers supply already converted property keys. */
	is_string = vm_value_is_string(key);
	if (!is_string)
		return 0;
	string = (const struct vm_string *)vm_value_as_cell(key);
	special = vm_string_equal_ascii(string, "-0");
	if (special) {
		*number = -0.0;
		*canonical = 1;
		return 0;
	}

	/* Canonical spelling must equal the exact language formatter's round-trip output. */
	*number = vm_string_to_number(string);
	wb_buffer_init(&text);
	status = vm_number_to_text(*number, 10, &text);
	if (status != 0)
		goto cleanup;
	if (text.length != string->length)
		goto cleanup;
	for (index = 0; index < string->length; index++) {
		unit = vm_string_at(string, index);
		if (unit != (uint16_t)text.data[index])
			goto cleanup;
	}

	/* Only identical source spelling denotes a canonical numeric index. */
	*canonical = 1;

cleanup:
	/* The formatter's C storage never retains or allocates a VM cell. */
	wb_buffer_release(&text);
	if (status != 0)
		return status;

	/* Succeeded: canonical distinguishes an actual numeric index from an ordinary property name. */
	return 0;
}

/*
 * Converts a language value to BigInt with ToPrimitive's number hint and ToBigInt's type rules.
 * Native roots retain the owner and fresh primitive across parsing allocation or script collection.
 */
int
vm_to_bigint(
	struct vm_realm *realm,
	vm_value value,
	vm_value *result)
{
	struct operation_bigint_roots roots;
	struct wb_buffer text;
	struct vm_bigint *big;
	struct vm_string *string;
	uint32_t start;
	uint32_t end;
	uint32_t index;
	uint16_t unit;
	int is_string;
	int blank;
	int boolean;
	int status;

	/* Publish no partial conversion while its source or transient primitive may still run script. */
	*result = VM_VALUE_UNDEFINED;
	roots.realm = realm;
	roots.source = value;
	roots.primitive = VM_VALUE_UNDEFINED;
	wb_buffer_init(&text);
	status = vm_heap_add_tracer(realm->heap, operation_bigint_trace, &roots);
	if (status != 0)
		return status;
	status = vm_to_primitive(realm, value, VM_HINT_NUMBER, &roots.primitive);
	if (status != 0)
		goto cleanup;
	big = vm_value_as_bigint(roots.primitive);
	if (big != NULL) {
		*result = roots.primitive;
		status = 0;
		goto cleanup;
	}

	/* Booleans become exactly zero or one without accepting Number primitives. */
	if (roots.primitive == VM_VALUE_TRUE || roots.primitive == VM_VALUE_FALSE) {
		boolean = 0;
		if (roots.primitive == VM_VALUE_TRUE)
			boolean = 1;
		status = vm_bigint_from_int64(realm->heap, boolean, result);
		goto cleanup;
	}

	/* Every remaining accepted primitive is a string; Number, Symbol, null and undefined throw. */
	is_string = vm_value_is_string(roots.primitive);
	if (!is_string) {
		status = vm_throw_type_error(realm, "Cannot convert the value to a BigInt");
		goto cleanup;
	}

	/* Trim the language's complete Unicode whitespace and line-terminator set before parsing ASCII numeric grammar. */
	string = (struct vm_string *)vm_value_as_cell(roots.primitive);
	start = 0;
	end = string->length;
	while (start < end) {
		unit = vm_string_at(string, start);
		blank = vm_is_space(unit);
		if (!blank)
			break;
		start++;
	}

	/* Trailing language whitespace is excluded independently of leading whitespace. */
	while (end > start) {
		unit = vm_string_at(string, end - 1U);
		blank = vm_is_space(unit);
		if (!blank)
			break;
		end--;
	}

	/* StringToBigInt's accepted digits, signs and prefixes are ASCII; other remaining code units are invalid. */
	for (index = start; index < end; index++) {
		unit = vm_string_at(string, index);
		if (unit > 0x7FU) {
			status = vm_throw_error(realm, VM_ERROR_SYNTAX, "Cannot convert the string to a BigInt");
			goto cleanup;
		}

		/* Copy one grammar byte into noncollectible parsing storage. */
		status = wb_buffer_append_byte(&text, (unsigned char)unit);
		if (status != 0)
			goto cleanup;
	}

	/* The established BigInt parser handles empty text, radix prefixes, signed decimal and exact magnitude. */
	status = vm_bigint_from_text(realm->heap, wb_buffer_string(&text), text.length, result);
	if (status == EINVAL)
		status = vm_throw_error(realm, VM_ERROR_SYNTAX, "Cannot convert the string to a BigInt");

cleanup:
	/* Neither the transient primitive nor its native parsing buffer escapes this frame's conversion lifetime. */
	wb_buffer_release(&text);
	vm_heap_remove_tracer(realm->heap, operation_bigint_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: the result is the converted BigInt with its original or exact parsed value. */
	return 0;
}

/*
 * Converts a value to a truth value (ToBoolean).
 */
int
vm_to_boolean(
	vm_value value)
{
	struct vm_string *string;
	struct vm_bigint *big;
	double number;
	int is_number;
	int is_string;

	/* The false constants. */
	if (value == VM_VALUE_FALSE || value == VM_VALUE_NULL || value == VM_VALUE_UNDEFINED)
		return 0;
	if (value == VM_VALUE_TRUE)
		return 1;

	/* A number is false when zero or NaN. */
	is_number = vm_value_is_number(value);
	if (is_number) {
		number = vm_value_as_number(value);
		if (number == 0.0 || number != number)
			return 0;
		return 1;
	}

	/* A string is false when empty. */
	is_string = vm_value_is_string(value);
	if (is_string) {
		string = (struct vm_string *)vm_value_as_cell(value);
		if (string->length == 0)
			return 0;
		return 1;
	}

	/* A BigInt is false when zero. */
	big = vm_value_as_bigint(value);
	if (big != NULL)
		return !vm_bigint_is_zero(big);

	/* Objects and symbols are true. */
	return 1;
}

/*
 * Converts a value to a primitive (ToPrimitive): an object through its
 * valueOf and toString methods, in the order the hint says.
 */
int
vm_to_primitive(
	struct vm_realm *realm,
	vm_value value,
	int hint,
	vm_value *result)
{
	vm_value exotic;
	int is_object;
	int status;

	/* A primitive is itself. */
	*result = value;
	is_object = vm_value_is_object(value);
	if (!is_object)
		return 0;

	/* The object's Symbol.toPrimitive decides when it has one (ws074-p087; Date.prototype has one). */
	status = vm_get_method(realm, value, vm_symbol_key(realm, VM_SYMBOL_TO_PRIMITIVE), &exotic);
	if (status != 0)
		return status;
	if (exotic != VM_VALUE_UNDEFINED) {
		status = operation_exotic_primitive(realm, value, exotic, hint, result);
		if (status != 0)
			return status;
		return 0;
	}

	/* Otherwise valueOf and toString. */
	status = vm_ordinary_to_primitive(realm, value, hint, result);
	if (status != 0)
		return status;

	/* Succeeded: the primitive. */
	return 0;
}

/*
 * Converts an object to a primitive through its valueOf and toString
 * methods (OrdinaryToPrimitive): toString first for the string hint,
 * valueOf first otherwise.
 */
int
vm_ordinary_to_primitive(
	struct vm_realm *realm,
	vm_value value,
	int hint,
	vm_value *result)
{
	const char *first;
	const char *second;
	int done;
	int status;

	/* A string hint tries toString first; the others valueOf. */
	first = "valueOf";
	second = "toString";
	if (hint == VM_HINT_STRING) {
		first = "toString";
		second = "valueOf";
	}

	/* The first method, when it is callable and gives a primitive. */
	status = operation_call_method(realm, value, first, &done, result);
	if (status != 0)
		return status;
	if (done)
		return 0;

	/* Then the second. */
	status = operation_call_method(realm, value, second, &done, result);
	if (status != 0)
		return status;
	if (done)
		return 0;

	/* Neither gave a primitive. */
	status = vm_throw_type_error(realm, "Cannot convert object to primitive value");
	return status;
}

/*
 * Converts a value to a number (ToNumber).
 */
int
vm_to_number(
	struct vm_realm *realm,
	vm_value value,
	double *number)
{
	struct vm_cell *cell;
	vm_value primitive;
	int is_number;
	int is_cell;
	int is_object;
	int status;

	/* A number is itself. */
	is_number = vm_value_is_number(value);
	if (is_number) {
		*number = vm_value_as_number(value);
		return 0;
	}

	/* true is one. */
	if (value == VM_VALUE_TRUE) {
		*number = 1.0;
		return 0;
	}

	/* false and null are zero. */
	if (value == VM_VALUE_FALSE || value == VM_VALUE_NULL) {
		*number = 0.0;
		return 0;
	}

	/* undefined (and any other constant) is NaN. */
	is_cell = vm_value_is_cell(value);
	if (!is_cell) {
		*number = NAN;
		return 0;
	}

	/* An object through its primitive. */
	is_object = vm_value_is_object(value);
	if (is_object) {
		status = vm_to_primitive(realm, value, VM_HINT_NUMBER, &primitive);
		if (status != 0)
			return status;
		status = vm_to_number(realm, primitive, number);
		if (status != 0)
			return status;
		return 0;
	}

	/* A string is read as a numeral. */
	cell = vm_value_as_cell(value);
	if (cell->type == &vm_string_type) {
		*number = operation_parse_number((const struct vm_string *)cell);
		return 0;
	}

	/* A BigInt cannot become a number implicitly. */
	if (cell->type == &vm_bigint_type) {
		status = vm_throw_type_error(realm, "Cannot convert a BigInt value to a number");
		return status;
	}

	/* A symbol cannot become a number. */
	status = vm_throw_type_error(realm, "Cannot convert a Symbol value to a number");
	return status;
}

/*
 * Converts a value to a numeric (ToNumeric): a number, or a BigInt as it is.
 */
int
vm_to_numeric(
	struct vm_realm *realm,
	vm_value value,
	vm_value *numeric)
{
	vm_value primitive;
	double number;
	int status;

	/* A number is itself. */
	if (vm_value_is_number(value)) {
		*numeric = value;
		return 0;
	}

	/* An object through its primitive; a BigInt stays. */
	status = vm_to_primitive(realm, value, VM_HINT_NUMBER, &primitive);
	if (status != 0)
		return status;
	if (vm_value_as_bigint(primitive) != NULL) {
		*numeric = primitive;
		return 0;
	}

	/* Anything else as a number. */
	status = vm_to_number(realm, primitive, &number);
	if (status != 0)
		return status;
	*numeric = vm_value_number(number);
	return 0;
}

/*
 * Converts a value to a signed 32-bit integer (ToInt32).
 */
int
vm_to_int32(
	struct vm_realm *realm,
	vm_value value,
	int32_t *number)
{
	double real;
	double wrapped;
	int is_int32;
	int status;

	/* An int32 is itself. */
	is_int32 = vm_value_is_int32(value);
	if (is_int32) {
		*number = vm_value_as_int32(value);
		return 0;
	}

	/* Anything else as a number, wrapped modulo 2^32. */
	status = vm_to_number(realm, value, &real);
	if (status != 0)
		return status;
	wrapped = operation_int32_of(real);

	/* Succeeded: the number in int32's range. */
	*number = (int32_t)wrapped;
	return 0;
}

/*
 * Converts a value to an unsigned 32-bit integer (ToUint32).
 */
int
vm_to_uint32(
	struct vm_realm *realm,
	vm_value value,
	uint32_t *number)
{
	int32_t signed_number;
	int status;

	/* The same bits as ToInt32. */
	status = vm_to_int32(realm, value, &signed_number);
	if (status != 0)
		return status;

	/* Succeeded: read as unsigned. */
	*number = (uint32_t)signed_number;
	return 0;
}

/*
 * Converts a value to a string (ToString).
 */
int
vm_to_string(
	struct vm_realm *realm,
	vm_value value,
	struct vm_string **string)
{
	struct vm_cell *cell;
	const char *text;
	vm_value primitive;
	int is_number;
	int is_object;
	int status;

	/* A number's numeral. */
	is_number = vm_value_is_number(value);
	if (is_number) {
		status = operation_number_string(realm, vm_value_as_number(value), string);
		if (status != 0)
			return status;
		return 0;
	}

	/* An object through its primitive. */
	is_object = vm_value_is_object(value);
	if (is_object) {
		status = vm_to_primitive(realm, value, VM_HINT_STRING, &primitive);
		if (status != 0)
			return status;
		status = vm_to_string(realm, primitive, string);
		if (status != 0)
			return status;
		return 0;
	}

	/* The constants' names. */
	text = NULL;
	if (value == VM_VALUE_TRUE)
		text = "true";
	if (value == VM_VALUE_FALSE)
		text = "false";
	if (value == VM_VALUE_NULL)
		text = "null";
	if (value == VM_VALUE_UNDEFINED || value == VM_VALUE_EMPTY)
		text = "undefined";

	/* A string is itself. */
	if (text == NULL) {
		cell = vm_value_as_cell(value);
		if (cell->type == &vm_string_type) {
			*string = (struct vm_string *)cell;
			return 0;
		}

		/* A BigInt's digits. */
		if (cell->type == &vm_bigint_type) {
			status = operation_bigint_string(realm, (const struct vm_bigint *)cell, string);
			return status;
		}

		/* A symbol cannot become a string implicitly. */
		status = vm_throw_type_error(realm, "Cannot convert a Symbol value to a string");
		return status;
	}

	/* The name as a string. */
	*string = vm_string_from_utf8(realm->heap, text, strlen(text));
	if (*string == NULL)
		return ENOMEM;

	/* Succeeded: the string. */
	return 0;
}

/*
 * Converts a value to a property key (ToPropertyKey): an index, an atom
 * or a symbol.
 */
int
vm_to_key(
	struct vm_realm *realm,
	vm_value value,
	vm_value *key)
{
	struct vm_string *string;
	struct vm_cell *cell;
	vm_value primitive;
	double number;
	int32_t whole;
	int is_number;
	int is_cell;
	int status;

	/* A number that is an index below 2^31 is that index. */
	is_number = vm_value_is_number(value);
	if (is_number) {
		number = vm_value_as_number(value);
		if (number >= 0.0 && number <= 2147483647.0) {
			whole = (int32_t)number;
			if ((double)whole == number && !(whole == 0 && 1.0 / number < 0.0)) {
				*key = vm_value_int32(whole);
				return 0;
			}
		}
	}

	/* An object through its primitive (with the string hint). */
	status = vm_to_primitive(realm, value, VM_HINT_STRING, &primitive);
	if (status != 0)
		return status;

	/* A symbol is itself. */
	is_cell = vm_value_is_cell(primitive);
	if (is_cell) {
		cell = vm_value_as_cell(primitive);
		if (cell->type == &vm_symbol_type) {
			*key = primitive;
			return 0;
		}
	}

	/* Anything else through its string. */
	status = vm_to_string(realm, primitive, &string);
	if (status != 0)
		return status;
	status = vm_key_from_string(realm->heap, string, key);
	if (status != 0)
		return status;

	/* Succeeded: the key. */
	return 0;
}

/*
 * Tells whether two values are strictly equal (===).
 */
int
vm_strict_equals(
	vm_value left,
	vm_value right)
{
	struct vm_string *left_string;
	struct vm_string *right_string;
	double left_value;
	double right_value;
	int left_number;
	int right_number;
	int left_is_string;
	int right_is_string;
	struct vm_bigint *left_big;
	struct vm_bigint *right_big;
	int same;

	/* Numbers compare by value (NaN is unequal to itself, +0 equals -0). */
	left_number = vm_value_is_number(left);
	right_number = vm_value_is_number(right);
	if (left_number && right_number) {
		left_value = vm_value_as_number(left);
		right_value = vm_value_as_number(right);
		if (left_value == right_value)
			return 1;
		return 0;
	}

	/* Strings compare by their characters. */
	left_is_string = vm_value_is_string(left);
	right_is_string = vm_value_is_string(right);
	if (left_is_string && right_is_string) {
		left_string = (struct vm_string *)vm_value_as_cell(left);
		right_string = (struct vm_string *)vm_value_as_cell(right);
		same = vm_string_equal(left_string, right_string);
		if (same)
			return 1;
		return 0;
	}

	/* BigInts compare by value. */
	left_big = vm_value_as_bigint(left);
	right_big = vm_value_as_bigint(right);
	if (left_big != NULL && right_big != NULL)
		return vm_bigint_compare(left_big, right_big) == 0;

	/* Everything else is the same value or not. */
	if (left == right)
		return 1;

	/* Different values. */
	return 0;
}

/*
 * Tells whether two values are loosely equal (==): the same type compares
 * strictly, null equals undefined, and otherwise the two are converted
 * towards numbers (an object through its primitive).
 */
int
vm_loose_equals(
	struct vm_realm *realm,
	vm_value left,
	vm_value right,
	int *equal)
{
	vm_value converted;
	double number;
	int left_type;
	int right_type;
	int order;
	int status;

	/* Each step converts one side and compares again, until the types agree or cannot. */
	*equal = 0;
	for (;;) {
		left_type = operation_type(left);
		right_type = operation_type(right);

		/* The same type compares strictly. */
		if (left_type == right_type) {
			*equal = vm_strict_equals(left, right);
			return 0;
		}

		/* null and undefined equal each other and nothing else. */
		if (left_type <= OPERATION_NULL && right_type <= OPERATION_NULL) {
			*equal = 1;
			return 0;
		}

		/* Either one alone equals nothing but the other. */
		if (left_type <= OPERATION_NULL || right_type <= OPERATION_NULL)
			return 0;

		/* A boolean becomes its number (either side). */
		if (left_type == OPERATION_BOOLEAN) {
			left = vm_value_int32(left == VM_VALUE_TRUE);
			continue;
		}

		/* The same on the right. */
		if (right_type == OPERATION_BOOLEAN) {
			right = vm_value_int32(right == VM_VALUE_TRUE);
			continue;
		}

		/* A string beside a number becomes a number. */
		if (left_type == OPERATION_STRING && right_type == OPERATION_NUMBER) {
			status = vm_to_number(realm, left, &number);
			if (status != 0)
				return status;
			left = vm_value_number(number);
			continue;
		}

		/* The same on the right. */
		if (left_type == OPERATION_NUMBER && right_type == OPERATION_STRING) {
			status = vm_to_number(realm, right, &number);
			if (status != 0)
				return status;
			right = vm_value_number(number);
			continue;
		}

		/* A BigInt beside a number compares by value. */
		if (left_type == OPERATION_BIGINT && right_type == OPERATION_NUMBER) {
			status = vm_bigint_compare_double(realm->heap, (const struct vm_bigint *)vm_value_as_cell(left), vm_value_as_number(right), &order);
			if (status != 0)
				return status;
			*equal = order == 0;
			return 0;
		}
		if (left_type == OPERATION_NUMBER && right_type == OPERATION_BIGINT) {
			status = vm_bigint_compare_double(realm->heap, (const struct vm_bigint *)vm_value_as_cell(right), vm_value_as_number(left), &order);
			if (status != 0)
				return status;
			*equal = order == 0;
			return 0;
		}

		/* A string beside a BigInt becomes a BigInt (unreadable: unequal). */
		if (left_type == OPERATION_STRING && right_type == OPERATION_BIGINT) {
			status = operation_string_bigint(realm, left, &converted);
			if (status != 0)
				return status;
			if (converted == VM_VALUE_UNDEFINED)
				return 0;
			left = converted;
			continue;
		}
		if (left_type == OPERATION_BIGINT && right_type == OPERATION_STRING) {
			status = operation_string_bigint(realm, right, &converted);
			if (status != 0)
				return status;
			if (converted == VM_VALUE_UNDEFINED)
				return 0;
			right = converted;
			continue;
		}

		/* An object beside a primitive becomes its primitive. */
		if (left_type == OPERATION_OBJECT) {
			status = vm_to_primitive(realm, left, VM_HINT_DEFAULT, &converted);
			if (status != 0)
				return status;
			left = converted;
			continue;
		}

		/* The same on the right. */
		if (right_type == OPERATION_OBJECT) {
			status = vm_to_primitive(realm, right, VM_HINT_DEFAULT, &converted);
			if (status != 0)
				return status;
			right = converted;
			continue;
		}

		/* A symbol beside a number or a string is equal to neither. */
		return 0;
	}
}

/*
 * Adds two values (+): after both become primitives, strings join when
 * either is a string, numbers add otherwise.
 */
int
vm_add(
	struct vm_realm *realm,
	vm_value left,
	vm_value right,
	vm_value *result)
{
	struct vm_string *left_string;
	struct vm_string *right_string;
	struct vm_string *joined;
	vm_value left_primitive;
	vm_value right_primitive;
	struct vm_bigint *left_big;
	struct vm_bigint *right_big;
	double left_number;
	double right_number;
	int64_t sum;
	int left_int32;
	int right_int32;
	int is_string;
	int status;

	/* Two int32s add without leaving the fast path. */
	left_int32 = vm_value_is_int32(left);
	right_int32 = vm_value_is_int32(right);
	if (left_int32 && right_int32) {
		sum = (int64_t)vm_value_as_int32(left) + (int64_t)vm_value_as_int32(right);
		*result = vm_value_number((double)sum);
		return 0;
	}

	/* Both sides as primitives, the left first. */
	status = vm_to_primitive(realm, left, VM_HINT_DEFAULT, &left_primitive);
	if (status != 0)
		return status;
	status = vm_to_primitive(realm, right, VM_HINT_DEFAULT, &right_primitive);
	if (status != 0)
		return status;

	/* A string on either side joins the two as strings. */
	is_string = vm_value_is_string(left_primitive);
	if (!is_string)
		is_string = vm_value_is_string(right_primitive);
	if (is_string) {
		status = vm_to_string(realm, left_primitive, &left_string);
		if (status != 0)
			return status;
		status = vm_to_string(realm, right_primitive, &right_string);
		if (status != 0)
			return status;
		joined = vm_string_concat(realm->heap, left_string, right_string);
		if (joined == NULL)
			return ENOMEM;
		*result = vm_value_cell(joined);
		return 0;
	}

	/* BigInts add to BigInts only. */
	left_big = vm_value_as_bigint(left_primitive);
	right_big = vm_value_as_bigint(right_primitive);
	if (left_big != NULL || right_big != NULL) {
		if (left_big == NULL || right_big == NULL) {
			status = operation_mix_error(realm);
			return status;
		}
		status = vm_bigint_binary(realm, VM_NUMERIC_ADD, left_big, right_big, result);
		return status;
	}

	/* Otherwise the numbers. */
	status = vm_to_number(realm, left_primitive, &left_number);
	if (status != 0)
		return status;
	status = vm_to_number(realm, right_primitive, &right_number);
	if (status != 0)
		return status;

	/* Succeeded: the sum. */
	*result = vm_value_number(left_number + right_number);
	return 0;
}

/*
 * Applies a numeric operator other than + to two values: both become
 * numbers (the left first); the bitwise operators work on their int32 (a
 * shift's count on its low five bits, >>> on the uint32).
 */
int
vm_numeric(
	struct vm_realm *realm,
	int operator,
	vm_value left,
	vm_value right,
	vm_value *result)
{
	struct vm_bigint *left_big;
	struct vm_bigint *right_big;
	double left_number;
	double right_number;
	double answer;
	int32_t left_int;
	int32_t right_int;
	uint32_t count;
	int status;

	/* Both sides as numerics, the left first; BigInts work with BigInts only. */
	status = vm_to_numeric(realm, left, &left);
	if (status != 0)
		return status;
	status = vm_to_numeric(realm, right, &right);
	if (status != 0)
		return status;
	left_big = vm_value_as_bigint(left);
	right_big = vm_value_as_bigint(right);
	if (left_big != NULL || right_big != NULL) {
		if (left_big == NULL || right_big == NULL) {
			status = operation_mix_error(realm);
			return status;
		}
		status = vm_bigint_binary(realm, operator, left_big, right_big, result);
		return status;
	}
	left_number = vm_value_as_number(left);
	right_number = vm_value_as_number(right);

	/* Their int32 values, for the bitwise operators. */
	left_int = (int32_t)operation_int32_of(left_number);
	right_int = (int32_t)operation_int32_of(right_number);
	count = (uint32_t)right_int & 31U;

	/* The operator. */
	answer = NAN;
	switch (operator) {
	case VM_NUMERIC_SUB:
		answer = left_number - right_number;
		break;
	case VM_NUMERIC_MUL:
		answer = left_number * right_number;
		break;
	case VM_NUMERIC_DIV:
		answer = left_number / right_number;
		break;
	case VM_NUMERIC_MOD:
		answer = fmod(left_number, right_number);
		break;
	case VM_NUMERIC_EXP:
		answer = operation_power(left_number, right_number);
		break;
	case VM_NUMERIC_AND:
		answer = (double)(left_int & right_int);
		break;
	case VM_NUMERIC_OR:
		answer = (double)(left_int | right_int);
		break;
	case VM_NUMERIC_XOR:
		answer = (double)(left_int ^ right_int);
		break;
	case VM_NUMERIC_SHL:
		answer = (double)(int32_t)((uint32_t)left_int << count);
		break;
	case VM_NUMERIC_SAR:
		answer = (double)operation_shift_signed(left_int, count);
		break;
	case VM_NUMERIC_SHR:
		answer = (double)((uint32_t)left_int >> count);
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the answer as a value. */
	*result = vm_value_number(answer);
	return 0;
}

/*
 * Compares two values (<).
 */
int
vm_less(
	struct vm_realm *realm,
	vm_value left,
	vm_value right,
	vm_value *result)
{
	int status;

	/* The less-than relation. */
	status = vm_relation(realm, VM_RELATION_LESS, left, right, result);
	if (status != 0)
		return status;

	/* Succeeded: the comparison. */
	return 0;
}

/*
 * Applies a relation (<, <=, >, >=): both sides become primitives (the
 * left first), then two strings compare by code units and anything else as
 * numbers, where NaN makes every relation false.
 */
int
vm_relation(
	struct vm_realm *realm,
	int relation,
	vm_value left,
	vm_value right,
	vm_value *result)
{
	vm_value left_primitive;
	vm_value right_primitive;
	int order;
	int status;

	/* Both sides as primitives with the number hint, the left first. */
	status = vm_to_primitive(realm, left, VM_HINT_NUMBER, &left_primitive);
	if (status != 0)
		return status;
	status = vm_to_primitive(realm, right, VM_HINT_NUMBER, &right_primitive);
	if (status != 0)
		return status;

	/* Their order: below zero, zero, above zero, or unordered (a NaN). */
	status = operation_compare(realm, left_primitive, right_primitive, &order);
	if (status != 0)
		return status;

	/* The relation asked for; an unordered pair satisfies none. */
	*result = VM_VALUE_FALSE;
	if (order == 2)
		return 0;
	switch (relation) {
	case VM_RELATION_LESS:
		*result = vm_value_boolean(order < 0);
		break;
	case VM_RELATION_LESS_EQUAL:
		*result = vm_value_boolean(order <= 0);
		break;
	case VM_RELATION_GREATER:
		*result = vm_value_boolean(order > 0);
		break;
	case VM_RELATION_GREATER_EQUAL:
		*result = vm_value_boolean(order >= 0);
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the relation's truth. */
	return 0;
}

/*
 * Names the type of a value as typeof does.
 */
int
vm_typeof(
	struct vm_realm *realm,
	vm_value value,
	vm_value *result)
{
	const char *name;
	vm_value key;
	int type;
	int callable;

	/* The type's name ("object" for null, "function" for anything callable). */
	type = operation_type(value);
	name = "object";
	if (type == OPERATION_UNDEFINED)
		name = "undefined";
	if (type == OPERATION_BOOLEAN)
		name = "boolean";
	if (type == OPERATION_NUMBER)
		name = "number";
	if (type == OPERATION_STRING)
		name = "string";
	if (type == OPERATION_SYMBOL)
		name = "symbol";
	if (type == OPERATION_BIGINT)
		name = "bigint";
	callable = vm_value_is_callable(value);
	if (callable)
		name = "function";

	/* The name as an atom (the same few strings every time). */
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* Succeeded: the name. */
	*result = key;
	return 0;
}

/*
 * Throws an error of a kind with a message: an Error object of the realm
 * (a string "Name: message" while the realm has no Error objects yet).
 */
int
vm_throw_error(
	struct vm_realm *realm,
	int kind,
	const char *message)
{
	static const char *const names[] = {
		"Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "EvalError", "URIError"
	};
	struct vm_object *prototype;
	vm_value error_value;
	char text[256];
	int status;

	/* Without the realm's Error objects, a string. */
	prototype = realm->intrinsics[VM_INTRINSIC_ERROR_PROTOTYPE + kind];
	if (prototype == NULL) {
		snprintf(text, sizeof(text), "%s: %s", names[kind], message);
		status = operation_throw_text(realm, text);
		return status;
	}

	/* The error object, thrown (without memory, undefined is thrown). */
	status = vm_error_create(realm, kind, message, &error_value);
	if (status != 0) {
		status = vm_throw(realm, VM_VALUE_UNDEFINED);
		return status;
	}

	/* The error becomes the realm's exception. */
	status = vm_throw(realm, error_value);

	/* Reports the throw. */
	return status;
}

/*
 * Makes an Error object of a kind with a message (its message property;
 * none for an empty message).
 */
int
vm_error_create(
	struct vm_realm *realm,
	int kind,
	const char *message,
	vm_value *error_value)
{
	struct vm_object *error_object;
	struct vm_string *text;
	vm_value key;
	int status;

	/* The object, from the kind's prototype. */
	error_object = vm_object_create(realm->heap, realm->intrinsics[VM_INTRINSIC_ERROR_PROTOTYPE + kind]);
	if (error_object == NULL)
		return ENOMEM;
	error_object->kind = VM_KIND_ERROR;

	/* The message: writable and configurable, not enumerable. */
	if (message[0] != '\0') {
		text = vm_string_from_utf8(realm->heap, message, strlen(message));
		if (text == NULL)
			return ENOMEM;
		key = vm_key_from_ascii(realm->heap, "message");
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;
		status = vm_object_define(realm->heap, error_object, key, vm_value_cell(text),
		    VM_PROPERTY_WRITABLE | VM_PROPERTY_CONFIGURABLE);
		if (status != 0)
			return status;
	}

	/* Succeeded: the error. */
	*error_value = vm_value_cell(error_object);
	return 0;
}

/*
 * Throws a TypeError with a message.
 */
int
vm_throw_type_error(
	struct vm_realm *realm,
	const char *message)
{
	int status;

	/* The error of that kind. */
	status = vm_throw_error(realm, VM_ERROR_TYPE, message);

	/* Reports the throw. */
	return status;
}

/*
 * Throws a RangeError with a message.
 */
int
vm_throw_range_error(
	struct vm_realm *realm,
	const char *message)
{
	int status;

	/* The error of that kind. */
	status = vm_throw_error(realm, VM_ERROR_RANGE, message);

	/* Reports the throw. */
	return status;
}

/*
 * Throws a ReferenceError with a message.
 */
int
vm_throw_reference_error(
	struct vm_realm *realm,
	const char *message)
{
	int status;

	/* The error of that kind. */
	status = vm_throw_error(realm, VM_ERROR_REFERENCE, message);

	/* Reports the throw. */
	return status;
}

/*
 * Throws the ReferenceError of a name that has no binding.
 */
int
vm_throw_not_defined(
	struct vm_realm *realm,
	vm_value key)
{
	char text[200];
	int status;

	/* The message with the name. */
	status = operation_name_message(key, "%.120s is not defined", text, sizeof(text));
	if (status != 0)
		return status;

	/* Reports the throw. */
	status = vm_throw_reference_error(realm, text);
	return status;
}

/*
 * Throws the ReferenceError of a let or const binding (a name, the key)
 * read or written before its declaration ran.
 */
int
vm_throw_uninitialized(
	struct vm_realm *realm,
	vm_value key)
{
	char text[200];
	int status;

	/* The message with the name, as Chromium writes it. */
	status = operation_name_message(key, "Cannot access '%.120s' before initialization", text, sizeof(text));
	if (status != 0)
		return status;

	/* Reports the throw. */
	status = vm_throw_reference_error(realm, text);
	return status;
}

/*
 * Throws the SyntaxError of a let or const declaring a name a script has
 * declared already (a name, the key).
 */
int
vm_throw_redeclared(
	struct vm_realm *realm,
	vm_value key)
{
	char text[200];
	int status;

	/* The message with the name, as Chromium writes it. */
	status = operation_name_message(key, "Identifier '%.120s' has already been declared", text, sizeof(text));
	if (status != 0)
		return status;

	/* Reports the throw. */
	status = vm_throw_error(realm, VM_ERROR_SYNTAX, text);
	return status;
}

/*
 * Tells whether a code unit is white space or a line terminator
 * (StringToNumber's and parseInt's blanks).
 */
int
vm_is_space(
	uint16_t unit)
{
	/* The ASCII blanks and line terminators. */
	if (unit == 0x09U || unit == 0x0AU || unit == 0x0BU || unit == 0x0CU || unit == 0x0DU || unit == 0x20U)
		return 1;

	/* NBSP, the BOM, LS and PS. */
	if (unit == 0xA0U || unit == 0xFEFFU || unit == 0x2028U || unit == 0x2029U)
		return 1;

	/* The other Unicode space separators (Zs). */
	if (unit == 0x1680U || (unit >= 0x2000U && unit <= 0x200AU) || unit == 0x202FU || unit == 0x205FU || unit == 0x3000U)
		return 1;

	/* Anything else. */
	return 0;
}

/* Reports the language type of a value. */
static int
operation_type(
	vm_value value)
{
	struct vm_cell *cell;
	int is_number;

	/* The constants. */
	if (value == VM_VALUE_UNDEFINED || value == VM_VALUE_EMPTY)
		return OPERATION_UNDEFINED;
	if (value == VM_VALUE_NULL)
		return OPERATION_NULL;
	if (value == VM_VALUE_TRUE || value == VM_VALUE_FALSE)
		return OPERATION_BOOLEAN;

	/* The numbers. */
	is_number = vm_value_is_number(value);
	if (is_number)
		return OPERATION_NUMBER;

	/* The cells by their type. */
	cell = vm_value_as_cell(value);
	if (cell->type == &vm_string_type)
		return OPERATION_STRING;
	if (cell->type == &vm_symbol_type)
		return OPERATION_SYMBOL;
	if (cell->type == &vm_bigint_type)
		return OPERATION_BIGINT;

	/* Everything else is an object. */
	return OPERATION_OBJECT;
}

/* Calls an object's method by name for ToPrimitive: done when it was callable and gave a primitive. */
static int
operation_call_method(
	struct vm_realm *realm,
	vm_value object,
	const char *name,
	int *done,
	vm_value *result)
{
	vm_value key;
	vm_value method;
	vm_value answer;
	int callable;
	int is_object;
	int status;

	/* The method, if the object has a callable one. */
	*done = 0;
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_get(realm, object, key, &method);
	if (status != 0)
		return status;
	callable = vm_value_is_callable(method);
	if (!callable)
		return 0;

	/* Its answer, with the object as this. */
	status = vm_call(realm, method, object, NULL, 0, &answer);
	if (status != 0)
		return status;

	/* An object answer does not count. */
	is_object = vm_value_is_object(answer);
	if (is_object)
		return 0;

	/* Succeeded: the primitive. */
	*result = answer;
	*done = 1;
	return 0;
}

/* Compares two primitives: order is -1, 0 or 1, or 2 when a NaN leaves them unordered. */
static int
operation_compare(
	struct vm_realm *realm,
	vm_value left,
	vm_value right,
	int *order)
{
	struct vm_bigint *left_big;
	struct vm_bigint *right_big;
	double left_number;
	double right_number;
	int left_is_string;
	int right_is_string;
	int status;

	/* Two strings compare by their code units. */
	left_is_string = vm_value_is_string(left);
	right_is_string = vm_value_is_string(right);
	if (left_is_string && right_is_string) {
		*order = vm_string_compare((struct vm_string *)vm_value_as_cell(left), (struct vm_string *)vm_value_as_cell(right));
		if (*order < 0)
			*order = -1;
		if (*order > 0)
			*order = 1;
		return 0;
	}

	/* A BigInt compares with a BigInt, a number or a string exactly. */
	left_big = vm_value_as_bigint(left);
	right_big = vm_value_as_bigint(right);
	if (left_big != NULL || right_big != NULL) {
		status = operation_compare_bigint(realm, left, right, order);
		return status;
	}

	/* Anything else as numbers, the left first. */
	status = vm_to_number(realm, left, &left_number);
	if (status != 0)
		return status;
	status = vm_to_number(realm, right, &right_number);
	if (status != 0)
		return status;

	/* NaN on either side leaves the two unordered. */
	*order = 2;
	if (left_number != left_number || right_number != right_number)
		return 0;

	/* Succeeded: the numbers' order. */
	*order = 0;
	if (left_number < right_number)
		*order = -1;
	if (left_number > right_number)
		*order = 1;
	return 0;
}

/* Wraps a number to int32's range as ToInt32 does (NaN and the infinities are zero). */
static double
operation_int32_of(
	double number)
{
	double truncated;
	double wrapped;
	int infinite;

	/* NaN and the infinities are zero. */
	infinite = isinf(number);
	if (number != number || infinite)
		return 0.0;

	/* The whole part, modulo 2^32, then into the signed range. */
	truncated = trunc(number);
	wrapped = fmod(truncated, 4294967296.0);
	if (wrapped < 0.0)
		wrapped += 4294967296.0;
	if (wrapped >= 2147483648.0)
		wrapped -= 4294967296.0;

	/* The number in int32's range. */
	return wrapped;
}

/* Makes a number's string (Number::toString in radix 10). */
static int
operation_number_string(
	struct vm_realm *realm,
	double number,
	struct vm_string **string)
{
	struct wb_buffer text;
	int error;

	/* The digits. */
	wb_buffer_init(&text);
	error = vm_number_to_text(number, 10, &text);
	if (error != 0) {
		wb_buffer_release(&text);
		return error;
	}

	/* As a string. */
	*string = vm_string_from_utf8(realm->heap, wb_buffer_string(&text), text.length);
	wb_buffer_release(&text);
	if (*string == NULL)
		return ENOMEM;

	/* Succeeded: the numeral. */
	return 0;
}

/*
 * Reads a string as a numeral (StringToNumber): blanks around it; empty is
 * 0; Infinity with a sign; 0x, 0o and 0b integers; a decimal numeral with
 * an optional sign, fraction and exponent.  Anything else is NaN.
 */
static double
operation_parse_number(
	const struct vm_string *string)
{
	char text[OPERATION_NUMERAL_MAX + 1U];
	uint32_t start;
	uint32_t end;
	uint32_t index;
	uint16_t unit;
	uint16_t prefix;
	size_t length;
	int blank;
	int decimal;
	int differs;
	double number;

	/* The blanks at both ends are not part of the numeral. */
	start = 0;
	end = string->length;
	while (start < end) {
		unit = vm_string_at(string, start);
		blank = vm_is_space(unit);
		if (!blank)
			break;
		start++;
	}
	while (end > start) {
		unit = vm_string_at(string, end - 1U);
		blank = vm_is_space(unit);
		if (!blank)
			break;
		end--;
	}

	/* Nothing but blanks is zero. */
	if (start == end)
		return 0.0;

	/* A numeral is ASCII and not too long. */
	if (end - start > OPERATION_NUMERAL_MAX)
		return NAN;
	length = 0;
	for (index = start; index < end; index++) {
		unit = vm_string_at(string, index);
		if (unit >= 0x80U)
			return NAN;
		text[length] = (char)unit;
		length++;
	}

	/* The numeral ends there. */
	text[length] = '\0';

	/* The infinities. */
	differs = strcmp(text, "Infinity");
	if (differs != 0)
		differs = strcmp(text, "+Infinity");
	if (differs == 0)
		return INFINITY;
	differs = strcmp(text, "-Infinity");
	if (differs == 0)
		return -INFINITY;

	/* A prefixed integer (no sign is allowed before the prefix). */
	prefix = 0;
	if (length > 2U && text[0] == '0')
		prefix = (uint16_t)(text[1] | 0x20);
	if (prefix == 'x')
		return operation_parse_radix(text + 2, 16);
	if (prefix == 'o')
		return operation_parse_radix(text + 2, 8);
	if (prefix == 'b')
		return operation_parse_radix(text + 2, 2);

	/* A decimal numeral, checked before it is read. */
	decimal = operation_is_decimal(text);
	if (!decimal)
		return NAN;

	/* The number read exactly, with its sign. */
	if (text[0] == '-') {
		number = vm_number_parse(text + 1, length - 1U);
		return -number;
	}

	/* A plus sign. */
	if (text[0] == '+') {
		number = vm_number_parse(text + 1, length - 1U);
		return number;
	}

	/* No sign. */
	number = vm_number_parse(text, length);
	return number;
}

/* Reads the digits of a prefixed integer in a radix; NaN when there are none or one is not a digit. */
static double
operation_parse_radix(
	const char *text,
	unsigned radix)
{
	const char *digit_text;
	unsigned digit;
	char character;
	double number;

	/* At least one digit. */
	if (*text == '\0')
		return NAN;

	/* Each character must be a digit of the radix. */
	for (digit_text = text; *digit_text != '\0'; digit_text++) {
		character = *digit_text;
		if (character >= '0' && character <= '9') {
			digit = (unsigned)(character - '0');
		} else if ((character | 0x20) >= 'a' && (character | 0x20) <= 'z') {
			digit = (unsigned)((character | 0x20) - 'a') + 10U;
		} else {
			return NAN;
		}

		/* A digit outside the radix makes no numeral. */
		if (digit >= radix)
			return NAN;
	}

	/* The number, read exactly and rounded once. */
	number = vm_number_parse_radix(text, strlen(text), (int)radix);
	return number;
}

/* Tells whether a text is a decimal numeral: a sign, digits with an optional point, and an optional exponent. */
static int
operation_is_decimal(
	const char *text)
{
	size_t digits;
	size_t exponent_digits;

	/* An optional sign. */
	if (*text == '+' || *text == '-')
		text++;

	/* The digits around an optional point; at least one digit in all. */
	digits = 0;
	while (*text >= '0' && *text <= '9') {
		digits++;
		text++;
	}

	/* A point, and the fraction's digits. */
	if (*text == '.') {
		text++;
		while (*text >= '0' && *text <= '9') {
			digits++;
			text++;
		}
	}

	/* At least one digit in all. */
	if (digits == 0)
		return 0;

	/* An optional exponent with at least one digit. */
	if (*text == 'e' || *text == 'E') {
		text++;
		if (*text == '+' || *text == '-')
			text++;
		exponent_digits = 0;
		while (*text >= '0' && *text <= '9') {
			exponent_digits++;
			text++;
		}

		/* The exponent needs a digit. */
		if (exponent_digits == 0)
			return 0;
	}

	/* Nothing may follow. */
	if (*text != '\0')
		return 0;

	/* A decimal numeral. */
	return 1;
}

/* Raises a number to a power as ** does (C's pow says 1 for 1 ** NaN and for (+-1) ** Infinity; the language says NaN). */
static double
operation_power(
	double base,
	double exponent)
{
	int infinite;

	/* A NaN exponent. */
	if (exponent != exponent)
		return NAN;

	/* One (either sign) to an infinite power. */
	infinite = isinf(exponent);
	if (infinite && (base == 1.0 || base == -1.0))
		return NAN;

	/* Everything else as C computes it. */
	return pow(base, exponent);
}

/* Shifts an int32 right keeping its sign (>>), spelled out since C leaves the right shift of a negative number to the compiler. */
static int32_t
operation_shift_signed(
	int32_t number,
	uint32_t count)
{
	/* A negative number: the complement shifted, then complemented back, fills with ones. */
	if (number < 0)
		return (int32_t)~(~(uint32_t)number >> count);

	/* A positive one fills with zeros. */
	return (int32_t)((uint32_t)number >> count);
}

/* Throws a text as a string value. */
static int
operation_throw_text(
	struct vm_realm *realm,
	const char *text)
{
	struct vm_string *string;
	int status;

	/* The text as a string; without memory, the exception is undefined. */
	string = vm_string_from_utf8(realm->heap, text, strlen(text));
	if (string == NULL) {
		status = vm_throw(realm, VM_VALUE_UNDEFINED);
		return status;
	}

	/* Throws it. */
	status = vm_throw(realm, vm_value_cell(string));

	/* Reports the throw. */
	return status;
}

/* Writes a message that names a variable (the key, a string) into text with a format that has one %s. */
static int
operation_name_message(
	vm_value key,
	const char *format,
	char *text,
	size_t size)
{
	struct wb_buffer name;
	struct vm_string *string;
	int is_string;
	int status;

	/* The name as UTF-8 (a key here is a name, never a symbol). */
	wb_buffer_init(&name);
	is_string = vm_value_is_string(key);
	if (is_string) {
		string = (struct vm_string *)vm_value_as_cell(key);
		status = vm_string_to_utf8(string, &name);
		if (status != 0) {
			wb_buffer_release(&name);
			return status;
		}
	}

	/* The message, the name cut short when it is long. */
	snprintf(text, size, format, wb_buffer_string(&name));
	wb_buffer_release(&name);

	/* Succeeded: the message is written. */
	return 0;
}

/*
 * Calls an object's Symbol.toPrimitive with the hint's name ("default",
 * "number" or "string"); what it returns must be a primitive.
 */
static int
operation_exotic_primitive(
	struct vm_realm *realm,
	vm_value value,
	vm_value exotic,
	int hint,
	vm_value *result)
{
	struct vm_string *name;
	const char *text;
	vm_value argument;
	int is_object;
	int status;

	/* The hint's name. */
	text = "default";
	if (hint == VM_HINT_NUMBER)
		text = "number";
	if (hint == VM_HINT_STRING)
		text = "string";
	name = vm_string_from_utf8(realm->heap, text, strlen(text));
	if (name == NULL)
		return ENOMEM;
	argument = vm_value_cell(name);

	/* The call. */
	status = vm_call(realm, exotic, value, &argument, 1, result);
	if (status != 0)
		return status;

	/* An object is not a primitive. */
	is_object = vm_value_is_object(*result);
	if (is_object) {
		status = vm_throw_type_error(realm, "Cannot convert object to primitive value");
		return status;
	}

	/* Succeeded: the primitive. */
	return 0;
}

/* Makes a BigInt's decimal string. */
static int
operation_bigint_string(
	struct vm_realm *realm,
	const struct vm_bigint *big,
	struct vm_string **string)
{
	struct wb_buffer text;
	int error;

	wb_buffer_init(&text);
	error = vm_bigint_to_text(big, 10, &text);
	if (error != 0) {
		wb_buffer_release(&text);
		return error;
	}
	*string = vm_string_from_utf8(realm->heap, wb_buffer_string(&text), text.length);
	wb_buffer_release(&text);
	if (*string == NULL)
		return ENOMEM;
	return 0;
}

/* Reads a string as a BigInt (StringToBigInt): undefined when it is no BigInt's numeral. */
static int
operation_string_bigint(
	struct vm_realm *realm,
	vm_value string,
	vm_value *big)
{
	struct wb_buffer text;
	int error;

	*big = VM_VALUE_UNDEFINED;
	wb_buffer_init(&text);
	error = vm_string_to_utf8((const struct vm_string *)vm_value_as_cell(string), &text);
	if (error != 0) {
		wb_buffer_release(&text);
		return error;
	}
	error = vm_bigint_from_text(realm->heap, wb_buffer_string(&text), text.length, big);
	wb_buffer_release(&text);
	if (error == EINVAL) {
		*big = VM_VALUE_UNDEFINED;
		return 0;
	}
	return error;
}

/* Throws the TypeError of mixing a BigInt with another type. */
static int
operation_mix_error(
	struct vm_realm *realm)
{
	return vm_throw_type_error(realm, "Cannot mix BigInt and other types, use explicit conversions");
}

/* Orders two primitives when either is a BigInt: -1, 0, 1, or 2 when unordered. */
static int
operation_compare_bigint(
	struct vm_realm *realm,
	vm_value left,
	vm_value right,
	int *order)
{
	vm_value converted;
	double number;
	int swap;
	int status;
	struct vm_bigint *big;
	vm_value other;

	/* A string beside a BigInt is read as a BigInt; unreadable leaves them unordered. */
	*order = 2;
	if (vm_value_is_string(left)) {
		status = operation_string_bigint(realm, left, &converted);
		if (status != 0)
			return status;
		if (converted == VM_VALUE_UNDEFINED)
			return 0;
		left = converted;
	}
	if (vm_value_is_string(right)) {
		status = operation_string_bigint(realm, right, &converted);
		if (status != 0)
			return status;
		if (converted == VM_VALUE_UNDEFINED)
			return 0;
		right = converted;
	}

	/* Two BigInts. */
	if (vm_value_as_bigint(left) != NULL && vm_value_as_bigint(right) != NULL) {
		*order = vm_bigint_compare(vm_value_as_bigint(left), vm_value_as_bigint(right));
		return 0;
	}

	/* A BigInt and a number (the other side as a number). */
	swap = vm_value_as_bigint(left) == NULL;
	big = vm_value_as_bigint(swap ? right : left);
	other = swap ? left : right;
	status = vm_to_number(realm, other, &number);
	if (status != 0)
		return status;
	status = vm_bigint_compare_double(realm->heap, big, number, order);
	if (status != 0)
		return status;
	if (swap && *order != 2)
		*order = -*order;
	return 0;
}

/* Retains the managed owner and both input stages of a live ToBigInt conversion. */
static void
operation_bigint_trace(
	struct vm_heap *heap,
	void *context)
{
	struct operation_bigint_roots *roots;

	/* A conversion can be the only native owner of its realm or freshly returned primitive. */
	roots = context;
	if (roots->realm->managed)
		vm_heap_mark(heap, &roots->realm->cell);
	vm_heap_mark_value(heap, roots->source);
	vm_heap_mark_value(heap, roots->primitive);

	/* Succeeded: later script getters and parsing allocation can collect safely. */
	return;
}
