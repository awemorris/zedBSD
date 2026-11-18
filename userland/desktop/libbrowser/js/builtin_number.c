/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Boolean and Number: the constructors (a conversion when called, a
 * wrapper object with new), their prototypes (themselves wrappers of false
 * and 0), and Number's constants, predicates and formatting methods
 * (toString in any radix, toFixed, toExponential, toPrecision, all exact:
 * vm/number.c).
 */

#include "js/builtin.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/* The largest integer a double holds exactly (2^53 - 1). */
#define NUMBER_MAX_SAFE		9007199254740991.0

static int boolean_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int boolean_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int boolean_to_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int boolean_value_of(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int boolean_this(struct vm_realm *realm, vm_value this_value, vm_value *value);
static int number_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_is_finite(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_is_integer(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_is_nan(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_is_safe_integer(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_to_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_to_locale_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_value_of(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_to_fixed(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_to_exponential(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_to_precision(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int number_this(struct vm_realm *realm, vm_value this_value, double *number);
static int number_text_result(struct vm_realm *realm, struct wb_buffer *text, vm_value *result);
static int number_constant(struct vm_realm *realm, struct vm_object *object, const char *name, double value);

/*
 * Installs Boolean.
 */
int
js_builtin_install_boolean(
	struct vm_realm *realm)
{
	struct vm_function *constructor;
	struct vm_object *prototype;
	int error;

	/* Boolean.prototype: a Boolean object of false. */
	prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (prototype == NULL)
		return ENOMEM;
	prototype->kind = VM_KIND_BOOLEAN;
	prototype->internal = VM_VALUE_FALSE;
	realm->intrinsics[VM_INTRINSIC_BOOLEAN_PROTOTYPE] = prototype;

	/* The constructor and the methods. */
	error = js_builtin_constructor(realm, "Boolean", 1, boolean_call, boolean_construct, prototype, &constructor);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "toString", 0, boolean_to_string);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "valueOf", 0, boolean_value_of);
	if (error != 0)
		return error;

	/* Succeeded: Boolean is installed. */
	return 0;
}

/*
 * Installs Number.
 */
int
js_builtin_install_number(
	struct vm_realm *realm)
{
	struct vm_function *constructor;
	struct vm_object *prototype;
	int error;

	/* Number.prototype: a Number object of 0. */
	prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (prototype == NULL)
		return ENOMEM;
	prototype->kind = VM_KIND_NUMBER;
	prototype->internal = vm_value_int32(0);
	realm->intrinsics[VM_INTRINSIC_NUMBER_PROTOTYPE] = prototype;

	/* The constructor. */
	error = js_builtin_constructor(realm, "Number", 1, number_call, number_construct, prototype, &constructor);
	if (error != 0)
		return error;

	/* The constants (read-only, fixed). */
	error = number_constant(realm, &constructor->object, "EPSILON", 2.220446049250313e-16);
	if (error == 0)
		error = number_constant(realm, &constructor->object, "MAX_SAFE_INTEGER", NUMBER_MAX_SAFE);
	if (error == 0)
		error = number_constant(realm, &constructor->object, "MIN_SAFE_INTEGER", -NUMBER_MAX_SAFE);
	if (error == 0)
		error = number_constant(realm, &constructor->object, "MAX_VALUE", 1.7976931348623157e308);
	if (error == 0)
		error = number_constant(realm, &constructor->object, "MIN_VALUE", 5e-324);
	if (error == 0)
		error = number_constant(realm, &constructor->object, "NaN", NAN);
	if (error == 0)
		error = number_constant(realm, &constructor->object, "NEGATIVE_INFINITY", -INFINITY);
	if (error == 0)
		error = number_constant(realm, &constructor->object, "POSITIVE_INFINITY", INFINITY);
	if (error != 0)
		return error;

	/* The predicates (parseFloat and parseInt are the global ones, added with them). */
	error = js_builtin_method(realm, &constructor->object, "isFinite", 1, number_is_finite);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "isInteger", 1, number_is_integer);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "isNaN", 1, number_is_nan);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "isSafeInteger", 1, number_is_safe_integer);
	if (error != 0)
		return error;

	/* The prototype's methods. */
	error = js_builtin_method(realm, prototype, "toExponential", 1, number_to_exponential);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "toFixed", 1, number_to_fixed);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "toLocaleString", 0, number_to_locale_string);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "toPrecision", 1, number_to_precision);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "toString", 1, number_to_string);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "valueOf", 0, number_value_of);
	if (error != 0)
		return error;

	/* Succeeded: Number is installed. */
	return 0;
}

/* Boolean(value) called: ToBoolean. */
static int
boolean_call(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(this_value);

	/* Succeeded: the truth value. */
	*result = vm_value_boolean(vm_to_boolean(js_argument(args, count, 0)));
	return 0;
}

/* new Boolean(value): a Boolean object from new.target's prototype. */
static int
boolean_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *prototype;
	struct vm_object *made;
	vm_value truth;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The value, then the object. */
	truth = vm_value_boolean(vm_to_boolean(js_argument(args, count, 0)));
	status = vm_construct_prototype(realm, realm->new_target, realm->intrinsics[VM_INTRINSIC_BOOLEAN_PROTOTYPE], &prototype);
	if (status != 0)
		return status;
	made = vm_object_create(realm->heap, prototype);
	if (made == NULL)
		return ENOMEM;
	made->kind = VM_KIND_BOOLEAN;
	made->internal = truth;

	/* Succeeded: the object. */
	*result = vm_value_cell(made);
	return 0;
}

/* Boolean.prototype.toString(). */
static int
boolean_to_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	const char *text;
	vm_value value;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The boolean, as "true" or "false". */
	status = boolean_this(realm, this_value, &value);
	if (status != 0)
		return status;
	text = "false";
	if (value == VM_VALUE_TRUE)
		text = "true";
	status = js_builtin_string(realm, text, result);
	if (status != 0)
		return status;

	/* Succeeded: the text. */
	return 0;
}

/* Boolean.prototype.valueOf(). */
static int
boolean_value_of(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The boolean. */
	status = boolean_this(realm, this_value, result);
	if (status != 0)
		return status;

	/* Succeeded: the value. */
	return 0;
}

/* Finds the boolean of this: a boolean, or a Boolean object's (thisBooleanValue). */
static int
boolean_this(
	struct vm_realm *realm,
	vm_value this_value,
	vm_value *value)
{
	struct vm_object *object;
	int is_boolean;
	int is_object;
	int status;

	/* A boolean. */
	is_boolean = vm_value_is_boolean(this_value);
	if (is_boolean) {
		*value = this_value;
		return 0;
	}

	/* A Boolean object's value. */
	is_object = vm_value_is_object(this_value);
	if (is_object) {
		object = (struct vm_object *)vm_value_as_cell(this_value);
		if (object->kind == VM_KIND_BOOLEAN) {
			*value = object->internal;
			return 0;
		}
	}

	/* Anything else. */
	status = vm_throw_type_error(realm, "Boolean.prototype method called on a value that is not a Boolean");
	return status;
}

/* Number(value) called: ToNumber (0 without a value). */
static int
number_call(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_bigint *big;
	vm_value numeric;
	double number;
	int status;

	UNUSED_PARAMETER(this_value);

	/* No value is 0. */
	*result = vm_value_int32(0);
	if (count == 0)
		return 0;

	/* The numeric: a BigInt becomes the nearest number. */
	status = vm_to_numeric(realm, args[0], &numeric);
	if (status != 0)
		return status;
	big = vm_value_as_bigint(numeric);
	if (big != NULL) {
		*result = vm_value_number(vm_bigint_to_double(big));
		return 0;
	}
	number = vm_value_as_number(numeric);

	/* Succeeded: the number. */
	*result = vm_value_number(number);
	return 0;
}

/* new Number(value): a Number object from new.target's prototype. */
static int
number_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *prototype;
	struct vm_object *made;
	vm_value number;
	int status;

	/* The number, then the object. */
	status = number_call(realm, this_value, args, count, &number);
	if (status != 0)
		return status;
	status = vm_construct_prototype(realm, realm->new_target, realm->intrinsics[VM_INTRINSIC_NUMBER_PROTOTYPE], &prototype);
	if (status != 0)
		return status;
	made = vm_object_create(realm->heap, prototype);
	if (made == NULL)
		return ENOMEM;
	made->kind = VM_KIND_NUMBER;
	made->internal = number;

	/* Succeeded: the object. */
	*result = vm_value_cell(made);
	return 0;
}

/* Number.isFinite(value): a finite number (no conversion). */
static int
number_is_finite(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value value;
	double number;
	int is_number;

	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(this_value);

	/* Only a number. */
	value = js_argument(args, count, 0);
	*result = VM_VALUE_FALSE;
	is_number = vm_value_is_number(value);
	if (!is_number)
		return 0;

	/* Succeeded: whether it is finite. */
	number = vm_value_as_number(value);
	*result = vm_value_boolean(number == number && !isinf(number));
	return 0;
}

/* Number.isInteger(value): a finite number with no fraction. */
static int
number_is_integer(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value value;
	double number;
	int is_number;
	int infinite;

	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(this_value);

	/* Only a finite number. */
	value = js_argument(args, count, 0);
	*result = VM_VALUE_FALSE;
	is_number = vm_value_is_number(value);
	if (!is_number)
		return 0;
	number = vm_value_as_number(value);
	infinite = isinf(number);
	if (number != number || infinite)
		return 0;

	/* Succeeded: whether it has no fraction. */
	*result = vm_value_boolean(trunc(number) == number);
	return 0;
}

/* Number.isNaN(value): NaN itself (no conversion). */
static int
number_is_nan(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value value;
	double number;
	int is_number;

	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(this_value);

	/* Only a number. */
	value = js_argument(args, count, 0);
	*result = VM_VALUE_FALSE;
	is_number = vm_value_is_number(value);
	if (!is_number)
		return 0;

	/* Succeeded: whether it is NaN. */
	number = vm_value_as_number(value);
	*result = vm_value_boolean(number != number);
	return 0;
}

/* Number.isSafeInteger(value): an integer of at most 2^53 - 1 in magnitude. */
static int
number_is_safe_integer(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value integer;
	double number;
	int status;

	/* An integer first. */
	status = number_is_integer(realm, this_value, args, count, &integer);
	if (status != 0)
		return status;
	*result = VM_VALUE_FALSE;
	if (integer != VM_VALUE_TRUE)
		return 0;

	/* Succeeded: whether it is safe. */
	number = vm_value_as_number(args[0]);
	*result = vm_value_boolean(fabs(number) <= NUMBER_MAX_SAFE);
	return 0;
}

/* Number.prototype.toString(radix). */
static int
number_to_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_buffer text;
	vm_value radix_value;
	double number;
	double radix;
	int status;

	/* The number, and the radix (10 by default, from 2 to 36). */
	status = number_this(realm, this_value, &number);
	if (status != 0)
		return status;
	radix = 10.0;
	radix_value = js_argument(args, count, 0);
	if (radix_value != VM_VALUE_UNDEFINED) {
		status = js_builtin_integer(realm, radix_value, &radix);
		if (status != 0)
			return status;
	}

	/* The radix must be 2 to 36. */
	if (radix < 2.0 || radix > 36.0) {
		status = vm_throw_range_error(realm, "toString() radix must be between 2 and 36");
		return status;
	}

	/* The text. */
	wb_buffer_init(&text);
	status = vm_number_to_text(number, (int)radix, &text);
	if (status == 0)
		status = number_text_result(realm, &text, result);
	wb_buffer_release(&text);
	if (status != 0)
		return status;

	/* Succeeded: the text. */
	return 0;
}

/* Number.prototype.toLocaleString(): the radix-10 text (locales come with Intl). */
static int
number_to_locale_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* As toString(). */
	status = number_to_string(realm, this_value, NULL, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the text. */
	return 0;
}

/* Number.prototype.valueOf(). */
static int
number_value_of(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double number;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The number. */
	status = number_this(realm, this_value, &number);
	if (status != 0)
		return status;

	/* Succeeded: the value. */
	*result = vm_value_number(number);
	return 0;
}

/* Number.prototype.toFixed(digits): 0 to 100 digits after the point; 1e21 and above as toString. */
static int
number_to_fixed(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_buffer text;
	double number;
	double digits;
	double magnitude;
	int status;

	/* The number, then the digits (which must be 0 to 100 even for a number that ignores them). */
	status = number_this(realm, this_value, &number);
	if (status != 0)
		return status;
	status = js_builtin_integer(realm, js_argument(args, count, 0), &digits);
	if (status != 0)
		return status;
	if (digits < 0.0 || digits > 100.0) {
		status = vm_throw_range_error(realm, "toFixed() digits argument must be between 0 and 100");
		return status;
	}

	/* NaN, and numbers of 1e21 and more, as toString. */
	wb_buffer_init(&text);
	magnitude = fabs(number);
	if (number != number || magnitude >= 1e21) {
		status = vm_number_to_text(number, 10, &text);
	} else {
		status = vm_number_to_fixed(number, (int)digits, &text);
	}

	/* The text as a string. */
	if (status == 0)
		status = number_text_result(realm, &text, result);
	wb_buffer_release(&text);
	if (status != 0)
		return status;

	/* Succeeded: the text. */
	return 0;
}

/* Number.prototype.toExponential(digits). */
static int
number_to_exponential(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_buffer text;
	vm_value digits_value;
	double number;
	double digits;
	int infinite;
	int status;

	/* The number, then the digits (converted before a non-finite number is written). */
	status = number_this(realm, this_value, &number);
	if (status != 0)
		return status;
	digits_value = js_argument(args, count, 0);
	status = js_builtin_integer(realm, digits_value, &digits);
	if (status != 0)
		return status;

	/* A non-finite number as toString. */
	wb_buffer_init(&text);
	infinite = isinf(number);
	if (number != number || infinite) {
		status = vm_number_to_text(number, 10, &text);
	} else if (digits < 0.0 || digits > 100.0) {
		wb_buffer_release(&text);
		status = vm_throw_range_error(realm, "toExponential() argument must be between 0 and 100");
		return status;
	} else if (digits_value == VM_VALUE_UNDEFINED) {
		status = vm_number_to_exponential(number, -1, &text);
	} else {
		status = vm_number_to_exponential(number, (int)digits, &text);
	}

	/* The text as a string. */
	if (status == 0)
		status = number_text_result(realm, &text, result);
	wb_buffer_release(&text);
	if (status != 0)
		return status;

	/* Succeeded: the text. */
	return 0;
}

/* Number.prototype.toPrecision(precision). */
static int
number_to_precision(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_buffer text;
	vm_value precision_value;
	double number;
	double precision;
	int infinite;
	int status;

	/* The number; without a precision, toString. */
	status = number_this(realm, this_value, &number);
	if (status != 0)
		return status;
	precision_value = js_argument(args, count, 0);
	wb_buffer_init(&text);
	if (precision_value == VM_VALUE_UNDEFINED) {
		status = vm_number_to_text(number, 10, &text);
		if (status == 0)
			status = number_text_result(realm, &text, result);
		wb_buffer_release(&text);
		return status;
	}

	/* The precision (converted before a non-finite number is written), 1 to 100. */
	status = js_builtin_integer(realm, precision_value, &precision);
	if (status != 0) {
		wb_buffer_release(&text);
		return status;
	}

	/* A non-finite number as toString; the precision must be 1 to 100. */
	infinite = isinf(number);
	if (number != number || infinite) {
		status = vm_number_to_text(number, 10, &text);
	} else if (precision < 1.0 || precision > 100.0) {
		wb_buffer_release(&text);
		status = vm_throw_range_error(realm, "toPrecision() argument must be between 1 and 100");
		return status;
	} else {
		status = vm_number_to_precision(number, (int)precision, &text);
	}

	/* The text as a string. */
	if (status == 0)
		status = number_text_result(realm, &text, result);
	wb_buffer_release(&text);
	if (status != 0)
		return status;

	/* Succeeded: the text. */
	return 0;
}

/* Finds the number of this: a number, or a Number object's (thisNumberValue). */
static int
number_this(
	struct vm_realm *realm,
	vm_value this_value,
	double *number)
{
	struct vm_object *object;
	int is_number;
	int is_object;
	int status;

	/* A number. */
	is_number = vm_value_is_number(this_value);
	if (is_number) {
		*number = vm_value_as_number(this_value);
		return 0;
	}

	/* A Number object's value. */
	is_object = vm_value_is_object(this_value);
	if (is_object) {
		object = (struct vm_object *)vm_value_as_cell(this_value);
		if (object->kind == VM_KIND_NUMBER) {
			*number = vm_value_as_number(object->internal);
			return 0;
		}
	}

	/* Anything else. */
	status = vm_throw_type_error(realm, "Number.prototype method called on a value that is not a Number");
	return status;
}

/* Makes the string of a formatted number's text. */
static int
number_text_result(
	struct vm_realm *realm,
	struct wb_buffer *text,
	vm_value *result)
{
	int status;

	/* The text as a string. */
	status = js_builtin_string(realm, wb_buffer_string(text), result);
	if (status != 0)
		return status;

	/* Succeeded: the string. */
	return 0;
}

/* Defines a read-only, fixed number constant. */
static int
number_constant(
	struct vm_realm *realm,
	struct vm_object *object,
	const char *name,
	double value)
{
	int error;

	/* Neither writable, enumerable nor configurable. */
	error = js_builtin_value(realm, object, name, vm_value_number(value), 0);
	if (error != 0)
		return error;

	/* Succeeded: the constant is defined. */
	return 0;
}
