/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * String: the constructor, String.fromCharCode, fromCodePoint and raw, and
 * String.prototype's methods; match, replace, replaceAll, search and split
 * hand a regular expression to RegExp's algorithms (builtin_regexp.c,
 * ws074-p027; matchAll waits for the iterators).  Strings are
 * worked on as UTF-16 code units; the case mappings come from the
 * generated Unicode tables (base/unicode.c), with the final sigma rule.
 * normalize returns the string unchanged for now (the normalization tables
 * come later).
 */

#include "js/builtin.h"
#include "base/unicode.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One built-in method: its name, its length and what it does.  The table
 * is constant for the life of the program.
 */
struct string_entry {
	const char *name;
	unsigned length;
	vm_native native;
};

/* The capital and small final sigma. */
#define STRING_SIGMA		0x03A3U
#define STRING_FINAL_SIGMA	0x03C2U
#define STRING_SMALL_SIGMA	0x03C3U

/* Which ends trim works on. */
#define STRING_TRIM_START	1
#define STRING_TRIM_END		2

static int string_delegate(struct vm_realm *realm, vm_value this_value, vm_value argument, int which, const vm_value *extra, unsigned extra_count, int *done, vm_value *result);
static int string_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_from_char_code(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_from_code_point(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_raw(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_at(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_char_at(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_char_code_at(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_code_point_at(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_concat(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_ends_with(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_includes(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_index_of(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_is_well_formed(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_last_index_of(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_locale_compare(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_normalize(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_pad_end(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_pad_start(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_repeat(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_slice(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_starts_with(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_substring(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_substr(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_to_lower_case(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_to_upper_case(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_to_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_to_well_formed(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_trim(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_trim_start(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_trim_end(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_this(struct vm_realm *realm, vm_value this_value, const char *method, struct vm_string **string);
static int string_this_value(struct vm_realm *realm, vm_value this_value, vm_value *value);
static int string_argument(struct vm_realm *realm, const vm_value *args, unsigned count, unsigned index, struct vm_string **string);
static int string_find(const struct vm_string *haystack, const struct vm_string *needle, uint32_t from, int backwards, uint32_t *found);
static int string_matches_at(const struct vm_string *haystack, const struct vm_string *needle, uint32_t position);
static int string_substring_value(struct vm_realm *realm, const struct vm_string *string, uint32_t start, uint32_t end, vm_value *result);
static int string_units_value(struct vm_realm *realm, struct wb_units *units, vm_value *result);
static int string_case(struct vm_realm *realm, vm_value this_value, int upper, vm_value *result);
static int string_final_sigma(const struct vm_string *string, uint32_t index);
static uint32_t string_code_point(const struct vm_string *string, uint32_t index, uint32_t *size);
static uint32_t string_code_point_before(const struct vm_string *string, uint32_t index, uint32_t *size);
static int string_pad(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, int at_start, vm_value *result);
static int string_trim_with(struct vm_realm *realm, vm_value this_value, int ends, vm_value *result);
static int string_match(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_replace(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_replace_all(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_search(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_split(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_replace_text(struct vm_realm *realm, struct vm_string *string, vm_value search_value, vm_value replace_value, int all, vm_value *result);
static int string_append_range(struct wb_units *units, const struct vm_string *string, uint32_t start, uint32_t end);
static int string_push(struct vm_realm *realm, vm_value array, vm_value value);
static int string_is_regexp(vm_value value);
static int string_position(struct vm_realm *realm, vm_value value, uint32_t length, uint32_t fallback, uint32_t *position);

/*
 * The methods of String.prototype.
 */
static const struct string_entry string_methods[] = {
	{ "at", 1, string_at },
	{ "charAt", 1, string_char_at },
	{ "charCodeAt", 1, string_char_code_at },
	{ "codePointAt", 1, string_code_point_at },
	{ "concat", 1, string_concat },
	{ "endsWith", 1, string_ends_with },
	{ "includes", 1, string_includes },
	{ "indexOf", 1, string_index_of },
	{ "isWellFormed", 0, string_is_well_formed },
	{ "lastIndexOf", 1, string_last_index_of },
	{ "localeCompare", 1, string_locale_compare },
	{ "match", 1, string_match },
	{ "normalize", 0, string_normalize },
	{ "padEnd", 1, string_pad_end },
	{ "padStart", 1, string_pad_start },
	{ "repeat", 1, string_repeat },
	{ "replace", 2, string_replace },
	{ "replaceAll", 2, string_replace_all },
	{ "search", 1, string_search },
	{ "slice", 2, string_slice },
	{ "split", 2, string_split },
	{ "startsWith", 1, string_starts_with },
	{ "substr", 2, string_substr },
	{ "substring", 2, string_substring },
	{ "toLocaleLowerCase", 0, string_to_lower_case },
	{ "toLocaleUpperCase", 0, string_to_upper_case },
	{ "toLowerCase", 0, string_to_lower_case },
	{ "toString", 0, string_to_string },
	{ "toUpperCase", 0, string_to_upper_case },
	{ "toWellFormed", 0, string_to_well_formed },
	{ "trim", 0, string_trim },
	{ "valueOf", 0, string_to_string },
	{ NULL, 0, NULL }
};

/*
 * Installs String.
 */
int
js_builtin_install_string(
	struct vm_realm *realm)
{
	const struct string_entry *entry;
	struct vm_function *constructor;
	struct vm_function *trim;
	struct vm_object *prototype;
	vm_value empty;
	int error;

	/* String.prototype: a String object of the empty string. */
	error = js_builtin_string(realm, "", &empty);
	if (error != 0)
		return error;
	prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (prototype == NULL)
		return ENOMEM;
	prototype->kind = VM_KIND_STRING;
	prototype->internal = empty;
	error = js_builtin_value(realm, prototype, "length", vm_value_int32(0), 0);
	if (error != 0)
		return error;
	realm->intrinsics[VM_INTRINSIC_STRING_PROTOTYPE] = prototype;

	/* The constructor and its functions. */
	error = js_builtin_constructor(realm, "String", 1, string_call, string_construct, prototype, &constructor);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "fromCharCode", 1, string_from_char_code);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "fromCodePoint", 1, string_from_code_point);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "raw", 1, string_raw);
	if (error != 0)
		return error;

	/* The prototype's methods, and trimStart and trimEnd with their Annex B names (the same functions). */
	for (entry = string_methods; entry->name != NULL; entry++) {
		error = js_builtin_method(realm, prototype, entry->name, entry->length, entry->native);
		if (error != 0)
			return error;
	}

	/* trimStart and trimLeft, trimEnd and trimRight. */
	error = js_builtin_function(realm, "trimStart", 0, string_trim_start, NULL, &trim);
	if (error == 0)
		error = js_builtin_value(realm, prototype, "trimStart", vm_value_cell(trim), JS_BUILTIN_METHOD);
	if (error == 0)
		error = js_builtin_value(realm, prototype, "trimLeft", vm_value_cell(trim), JS_BUILTIN_METHOD);
	if (error == 0)
		error = js_builtin_function(realm, "trimEnd", 0, string_trim_end, NULL, &trim);
	if (error == 0)
		error = js_builtin_value(realm, prototype, "trimEnd", vm_value_cell(trim), JS_BUILTIN_METHOD);
	if (error == 0)
		error = js_builtin_value(realm, prototype, "trimRight", vm_value_cell(trim), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* Succeeded: String is installed. */
	return 0;
}

/* String(value) called: the string ("" without a value; a symbol's description form). */
static int
string_call(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct vm_cell *cell;
	int is_cell;
	int status;

	UNUSED_PARAMETER(this_value);

	/* No value is the empty string. */
	if (count == 0) {
		status = js_builtin_string(realm, "", result);
		return status;
	}

	/* A symbol says what it is (String(symbol) is its descriptive string, ws074-p087). */
	is_cell = vm_value_is_cell(args[0]);
	if (is_cell) {
		cell = vm_value_as_cell(args[0]);
		if (cell->type == &vm_symbol_type) {
			status = js_symbol_descriptive_string(realm, (struct vm_symbol *)cell, result);
			return status;
		}
	}

	/* The string. */
	status = vm_to_string(realm, args[0], &string);
	if (status != 0)
		return status;
	*result = vm_value_cell(string);
	return 0;
}

/* new String(value): a String object from new.target's prototype. */
static int
string_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *prototype;
	struct vm_object *wrapper;
	vm_value string;
	vm_value wrapped;
	int status;

	/* The string, and its wrapper. */
	status = string_call(realm, this_value, args, count, &string);
	if (status == 0)
		status = vm_to_object(realm, string, &wrapped);
	if (status != 0)
		return status;

	/* The wrapper's prototype from new.target. */
	status = vm_construct_prototype(realm, realm->new_target, realm->intrinsics[VM_INTRINSIC_STRING_PROTOTYPE], &prototype);
	if (status != 0)
		return status;
	wrapper = (struct vm_object *)vm_value_as_cell(wrapped);
	wrapper->prototype = prototype;

	/* Succeeded: the object. */
	*result = wrapped;
	return 0;
}

/* String.fromCharCode(...codes): each as a uint16 code unit. */
static int
string_from_char_code(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_units units;
	uint32_t code;
	uint16_t unit;
	unsigned index;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Each code, modulo 2^16. */
	wb_units_init(&units);
	status = 0;
	for (index = 0; status == 0 && index < count; index++) {
		status = vm_to_uint32(realm, args[index], &code);
		unit = (uint16_t)code;
		if (status == 0)
			status = wb_units_append(&units, &unit, 1);
	}

	/* The string. */
	if (status == 0)
		status = string_units_value(realm, &units, result);
	wb_units_release(&units);
	return status;
}

/* String.fromCodePoint(...points): each a whole number from 0 to 0x10FFFF. */
static int
string_from_code_point(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_units units;
	double number;
	double whole;
	unsigned index;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Each code point, checked. */
	wb_units_init(&units);
	status = 0;
	for (index = 0; status == 0 && index < count; index++) {
		status = vm_to_number(realm, args[index], &number);
		if (status != 0)
			break;
		whole = trunc(number);
		if (number != whole || number < 0.0 || number > 1114111.0) {
			status = vm_throw_range_error(realm, "Invalid code point");
			break;
		}

		/* The code point's units. */
		status = wb_units_append_code_point(&units, (uint32_t)number);
	}

	/* The string. */
	if (status == 0)
		status = string_units_value(realm, &units, result);
	wb_units_release(&units);
	return status;
}

/* String.raw(template, ...substitutions): the raw strings with the substitutions between them. */
static int
string_raw(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_units units;
	struct vm_string *part;
	vm_value cooked;
	vm_value raw;
	vm_value key;
	vm_value value;
	uint32_t length;
	uint32_t index;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The template's raw strings, as an object with a length. */
	status = vm_to_object(realm, js_argument(args, count, 0), &cooked);
	if (status != 0)
		return status;
	key = vm_key_from_ascii(realm->heap, "raw");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_get(realm, cooked, key, &value);
	if (status == 0)
		status = vm_to_object(realm, value, &raw);
	if (status == 0)
		status = js_builtin_length(realm, raw, &length);
	if (status != 0)
		return status;

	/* Each raw string, a substitution after all but the last. */
	wb_units_init(&units);
	for (index = 0; status == 0 && index < length; index++) {
		status = vm_get(realm, raw, vm_value_int32((int32_t)index), &value);
		if (status == 0)
			status = vm_to_string(realm, value, &part);
		if (status == 0)
			status = vm_string_append_units(part, &units);
		if (status == 0 && index + 1U < length && index + 1U < count) {
			status = vm_to_string(realm, args[index + 1U], &part);
			if (status == 0)
				status = vm_string_append_units(part, &units);
		}
	}

	/* The string. */
	if (status == 0)
		status = string_units_value(realm, &units, result);
	wb_units_release(&units);
	return status;
}

/* String.prototype.at(index). */
static int
string_at(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	double relative;
	double index;
	int status;

	/* The string and the index (from the end when negative). */
	status = string_this(realm, this_value, "at", &string);
	if (status == 0)
		status = js_builtin_integer(realm, js_argument(args, count, 0), &relative);
	if (status != 0)
		return status;
	index = relative;
	if (relative < 0.0)
		index = (double)string->length + relative;

	/* Outside is undefined; inside the one unit. */
	*result = VM_VALUE_UNDEFINED;
	if (index < 0.0 || index >= (double)string->length)
		return 0;
	status = string_substring_value(realm, string, (uint32_t)index, (uint32_t)index + 1U, result);
	return status;
}

/* String.prototype.charAt(position). */
static int
string_char_at(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	double position;
	int status;

	/* The string and the position. */
	status = string_this(realm, this_value, "charAt", &string);
	if (status == 0)
		status = js_builtin_integer(realm, js_argument(args, count, 0), &position);
	if (status != 0)
		return status;

	/* Outside is the empty string. */
	if (position < 0.0 || position >= (double)string->length) {
		status = js_builtin_string(realm, "", result);
		return status;
	}

	/* The one unit. */
	status = string_substring_value(realm, string, (uint32_t)position, (uint32_t)position + 1U, result);
	return status;
}

/* String.prototype.charCodeAt(position). */
static int
string_char_code_at(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	double position;
	int status;

	/* The string and the position. */
	status = string_this(realm, this_value, "charCodeAt", &string);
	if (status == 0)
		status = js_builtin_integer(realm, js_argument(args, count, 0), &position);
	if (status != 0)
		return status;

	/* Outside is NaN. */
	*result = vm_value_double(NAN);
	if (position < 0.0 || position >= (double)string->length)
		return 0;
	*result = vm_value_int32((int32_t)vm_string_at(string, (uint32_t)position));
	return 0;
}

/* String.prototype.codePointAt(position). */
static int
string_code_point_at(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	double position;
	uint32_t size;
	int status;

	/* The string and the position. */
	status = string_this(realm, this_value, "codePointAt", &string);
	if (status == 0)
		status = js_builtin_integer(realm, js_argument(args, count, 0), &position);
	if (status != 0)
		return status;

	/* Outside is undefined; inside the code point starting there. */
	*result = VM_VALUE_UNDEFINED;
	if (position < 0.0 || position >= (double)string->length)
		return 0;
	*result = vm_value_int32((int32_t)string_code_point(string, (uint32_t)position, &size));
	return 0;
}

/* String.prototype.concat(...strings). */
static int
string_concat(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_units units;
	struct vm_string *string;
	struct vm_string *part;
	unsigned index;
	int status;

	/* The string, then each argument's. */
	status = string_this(realm, this_value, "concat", &string);
	if (status != 0)
		return status;
	wb_units_init(&units);
	status = vm_string_append_units(string, &units);
	for (index = 0; status == 0 && index < count; index++) {
		status = vm_to_string(realm, args[index], &part);
		if (status == 0)
			status = vm_string_append_units(part, &units);
	}

	/* The string. */
	if (status == 0)
		status = string_units_value(realm, &units, result);
	wb_units_release(&units);
	return status;
}

/* String.prototype.endsWith(search, endPosition). */
static int
string_ends_with(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct vm_string *search;
	uint32_t end;
	int matches;
	int regexp;
	int status;

	/* The string, the search (never a RegExp) and the end. */
	status = string_this(realm, this_value, "endsWith", &string);
	if (status != 0)
		return status;
	regexp = string_is_regexp(js_argument(args, count, 0));
	if (regexp) {
		status = vm_throw_type_error(realm, "First argument to String.prototype.endsWith must not be a regular expression");
		return status;
	}

	/* The search as a string, and the end. */
	status = string_argument(realm, args, count, 0, &search);
	if (status == 0)
		status = string_position(realm, js_argument(args, count, 1), string->length, string->length, &end);
	if (status != 0)
		return status;

	/* The search must fit before the end and match there. */
	*result = VM_VALUE_FALSE;
	if (search->length > end)
		return 0;
	matches = string_matches_at(string, search, end - search->length);
	*result = vm_value_boolean(matches);
	return 0;
}

/* String.prototype.includes(search, position). */
static int
string_includes(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct vm_string *search;
	uint32_t position;
	uint32_t found;
	int present;
	int regexp;
	int status;

	/* The string, the search (never a RegExp) and the start. */
	status = string_this(realm, this_value, "includes", &string);
	if (status != 0)
		return status;
	regexp = string_is_regexp(js_argument(args, count, 0));
	if (regexp) {
		status = vm_throw_type_error(realm, "First argument to String.prototype.includes must not be a regular expression");
		return status;
	}

	/* The search as a string, and the start. */
	status = string_argument(realm, args, count, 0, &search);
	if (status == 0)
		status = string_position(realm, js_argument(args, count, 1), string->length, 0, &position);
	if (status != 0)
		return status;

	/* Whether it is found from there. */
	present = string_find(string, search, position, 0, &found);
	*result = vm_value_boolean(present);
	return 0;
}

/* String.prototype.indexOf(search, position). */
static int
string_index_of(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct vm_string *search;
	uint32_t position;
	uint32_t found;
	int present;
	int status;

	/* The string, the search and the start. */
	status = string_this(realm, this_value, "indexOf", &string);
	if (status == 0)
		status = string_argument(realm, args, count, 0, &search);
	if (status == 0)
		status = string_position(realm, js_argument(args, count, 1), string->length, 0, &position);
	if (status != 0)
		return status;

	/* The first place from there, or -1. */
	present = string_find(string, search, position, 0, &found);
	*result = vm_value_int32(-1);
	if (present)
		*result = vm_value_int32((int32_t)found);
	return 0;
}

/* String.prototype.isWellFormed(): no lone surrogate. */
static int
string_is_well_formed(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	uint32_t index;
	uint32_t size;
	uint32_t point;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The string. */
	status = string_this(realm, this_value, "isWellFormed", &string);
	if (status != 0)
		return status;

	/* Each code point; a lone surrogate is a code point of its own in the surrogate range. */
	*result = VM_VALUE_TRUE;
	for (index = 0; index < string->length; index += size) {
		point = string_code_point(string, index, &size);
		if (point >= 0xD800U && point <= 0xDFFFU) {
			*result = VM_VALUE_FALSE;
			return 0;
		}
	}

	/* Every code point is whole. */
	return 0;
}

/* String.prototype.lastIndexOf(search, position). */
static int
string_last_index_of(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct vm_string *search;
	double number;
	uint32_t position;
	uint32_t found;
	int present;
	int status;

	/* The string, the search and the start (the end for NaN or undefined). */
	status = string_this(realm, this_value, "lastIndexOf", &string);
	if (status == 0)
		status = string_argument(realm, args, count, 0, &search);
	if (status == 0)
		status = vm_to_number(realm, js_argument(args, count, 1), &number);
	if (status != 0)
		return status;
	position = string->length;
	if (number == number) {
		number = trunc(number);
		if (number < 0.0)
			number = 0.0;
		if (number < (double)string->length)
			position = (uint32_t)number;
	}

	/* The last place at or before it, or -1. */
	present = string_find(string, search, position, 1, &found);
	*result = vm_value_int32(-1);
	if (present)
		*result = vm_value_int32((int32_t)found);
	return 0;
}

/* String.prototype.localeCompare(that): by code units (locales come with Intl). */
static int
string_locale_compare(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct vm_string *that;
	int order;
	int status;

	/* Both strings. */
	status = string_this(realm, this_value, "localeCompare", &string);
	if (status == 0)
		status = string_argument(realm, args, count, 0, &that);
	if (status != 0)
		return status;

	/* The order as -1, 0 or 1. */
	order = vm_string_compare(string, that);
	if (order < 0)
		order = -1;
	if (order > 0)
		order = 1;
	*result = vm_value_int32(order);
	return 0;
}

/* String.prototype.normalize(form): the form must be NFC, NFD, NFKC or NFKD; the string is returned as it is for now. */
static int
string_normalize(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct vm_string *form;
	vm_value form_value;
	int known;
	int status;

	/* The string and the form. */
	status = string_this(realm, this_value, "normalize", &string);
	if (status != 0)
		return status;
	*result = vm_value_cell(string);
	form_value = js_argument(args, count, 0);
	if (form_value == VM_VALUE_UNDEFINED)
		return 0;
	status = vm_to_string(realm, form_value, &form);
	if (status != 0)
		return status;
	known = vm_string_equal_ascii(form, "NFC");
	if (!known)
		known = vm_string_equal_ascii(form, "NFD");
	if (!known)
		known = vm_string_equal_ascii(form, "NFKC");
	if (!known)
		known = vm_string_equal_ascii(form, "NFKD");

	/* Any other form is refused. */
	if (!known) {
		status = vm_throw_range_error(realm, "The normalization form should be one of NFC, NFD, NFKC, NFKD");
		return status;
	}

	/* A known form. */
	return 0;
}

/* String.prototype.padEnd(maxLength, fillString). */
static int
string_pad_end(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* At the end. */
	status = string_pad(realm, this_value, args, count, 0, result);
	return status;
}

/* String.prototype.padStart(maxLength, fillString). */
static int
string_pad_start(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* At the start. */
	status = string_pad(realm, this_value, args, count, 1, result);
	return status;
}

/* String.prototype.repeat(count). */
static int
string_repeat(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_units units;
	struct vm_string *string;
	double times;
	double index;
	int status;

	/* The string and the count (0 and up, finite). */
	status = string_this(realm, this_value, "repeat", &string);
	if (status == 0)
		status = js_builtin_integer(realm, js_argument(args, count, 0), &times);
	if (status != 0)
		return status;
	if (times < 0.0 || times == INFINITY) {
		status = vm_throw_range_error(realm, "Invalid count value");
		return status;
	}

	/* Nothing to repeat. */
	if (string->length == 0 || times == 0.0) {
		status = js_builtin_string(realm, "", result);
		return status;
	}

	/* Too long a result. */
	if (times * (double)string->length > 268435456.0) {
		status = vm_throw_range_error(realm, "Invalid string length");
		return status;
	}

	/* The copies; a long run looks now and then whether its task was asked to end. */
	wb_units_init(&units);
	status = 0;
	for (index = 0.0; status == 0 && index < times; index += 1.0) {
		status = vm_string_append_units(string, &units);
		if (status == 0)
			status = vm_heap_count_step(realm->heap);
	}

	/* The string of the copies. */
	if (status == 0)
		status = string_units_value(realm, &units, result);
	wb_units_release(&units);
	return status;
}

/* String.prototype.slice(start, end). */
static int
string_slice(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	vm_value end_value;
	double start;
	double end;
	int status;

	/* The string and the range (negative from the end). */
	status = string_this(realm, this_value, "slice", &string);
	if (status == 0)
		status = js_builtin_integer(realm, js_argument(args, count, 0), &start);
	if (status != 0)
		return status;
	end = (double)string->length;
	end_value = js_argument(args, count, 1);
	if (end_value != VM_VALUE_UNDEFINED) {
		status = js_builtin_integer(realm, end_value, &end);
		if (status != 0)
			return status;
	}

	/* The range, clamped and ordered. */
	if (start < 0.0)
		start = fmax(0.0, (double)string->length + start);
	if (end < 0.0)
		end = fmax(0.0, (double)string->length + end);
	start = fmin(start, (double)string->length);
	end = fmin(end, (double)string->length);
	if (end < start)
		end = start;

	/* The part. */
	status = string_substring_value(realm, string, (uint32_t)start, (uint32_t)end, result);
	return status;
}

/* String.prototype.startsWith(search, position). */
static int
string_starts_with(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct vm_string *search;
	uint32_t position;
	int matches;
	int regexp;
	int status;

	/* The string, the search (never a RegExp) and the start. */
	status = string_this(realm, this_value, "startsWith", &string);
	if (status != 0)
		return status;
	regexp = string_is_regexp(js_argument(args, count, 0));
	if (regexp) {
		status = vm_throw_type_error(realm, "First argument to String.prototype.startsWith must not be a regular expression");
		return status;
	}

	/* The search as a string, and the start. */
	status = string_argument(realm, args, count, 0, &search);
	if (status == 0)
		status = string_position(realm, js_argument(args, count, 1), string->length, 0, &position);
	if (status != 0)
		return status;

	/* The search must fit after the start and match there. */
	*result = VM_VALUE_FALSE;
	if ((uint64_t)position + search->length > string->length)
		return 0;
	matches = string_matches_at(string, search, position);
	*result = vm_value_boolean(matches);
	return 0;
}

/* String.prototype.substring(start, end): the two clamped and ordered. */
static int
string_substring(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	uint32_t start;
	uint32_t end;
	uint32_t swap;
	int status;

	/* The string and the two positions. */
	status = string_this(realm, this_value, "substring", &string);
	if (status == 0)
		status = string_position(realm, js_argument(args, count, 0), string->length, 0, &start);
	if (status == 0)
		status = string_position(realm, js_argument(args, count, 1), string->length, string->length, &end);
	if (status != 0)
		return status;
	if (start > end) {
		swap = start;
		start = end;
		end = swap;
	}

	/* The part. */
	status = string_substring_value(realm, string, start, end, result);
	return status;
}

/* String.prototype.substr(start, length) (Annex B). */
static int
string_substr(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	vm_value length_value;
	double start;
	double length;
	double end;
	int status;

	/* The string, the start (negative from the end) and the length. */
	status = string_this(realm, this_value, "substr", &string);
	if (status == 0)
		status = js_builtin_integer(realm, js_argument(args, count, 0), &start);
	if (status != 0)
		return status;
	if (start < 0.0)
		start = fmax(0.0, (double)string->length + start);
	start = fmin(start, (double)string->length);
	length = (double)string->length;
	length_value = js_argument(args, count, 1);
	if (length_value != VM_VALUE_UNDEFINED) {
		status = js_builtin_integer(realm, length_value, &length);
		if (status != 0)
			return status;
	}

	/* The end. */
	end = fmin(start + fmax(length, 0.0), (double)string->length);

	/* The part. */
	status = string_substring_value(realm, string, (uint32_t)start, (uint32_t)end, result);
	return status;
}

/* String.prototype.toLowerCase(). */
static int
string_to_lower_case(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The lower-case mapping. */
	status = string_case(realm, this_value, 0, result);
	return status;
}

/* String.prototype.toUpperCase(). */
static int
string_to_upper_case(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The upper-case mapping. */
	status = string_case(realm, this_value, 1, result);
	return status;
}

/* String.prototype.toString() and valueOf(): the string of this (a string or a String object). */
static int
string_to_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* thisStringValue. */
	status = string_this_value(realm, this_value, result);
	return status;
}

/* String.prototype.toWellFormed(): lone surrogates replaced by U+FFFD. */
static int
string_to_well_formed(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_units units;
	struct vm_string *string;
	uint32_t index;
	uint32_t size;
	uint32_t point;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The string. */
	status = string_this(realm, this_value, "toWellFormed", &string);
	if (status != 0)
		return status;

	/* Each code point, a lone surrogate replaced. */
	wb_units_init(&units);
	for (index = 0; status == 0 && index < string->length; index += size) {
		point = string_code_point(string, index, &size);
		if (point >= 0xD800U && point <= 0xDFFFU)
			point = 0xFFFDU;
		status = wb_units_append_code_point(&units, point);
	}

	/* The string. */
	if (status == 0)
		status = string_units_value(realm, &units, result);
	wb_units_release(&units);
	return status;
}

/* String.prototype.trim(). */
static int
string_trim(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Both ends. */
	status = string_trim_with(realm, this_value, STRING_TRIM_START | STRING_TRIM_END, result);
	return status;
}

/* String.prototype.trimStart(). */
static int
string_trim_start(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The start. */
	status = string_trim_with(realm, this_value, STRING_TRIM_START, result);
	return status;
}

/* String.prototype.trimEnd(). */
static int
string_trim_end(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The end. */
	status = string_trim_with(realm, this_value, STRING_TRIM_END, result);
	return status;
}

/* Finds the string of this: undefined and null are refused, anything else converted. */
static int
string_this(
	struct vm_realm *realm,
	vm_value this_value,
	const char *method,
	struct vm_string **string)
{
	char message[120];
	int status;

	/* RequireObjectCoercible. */
	if (this_value == VM_VALUE_UNDEFINED || this_value == VM_VALUE_NULL) {
		snprintf(message, sizeof(message), "String.prototype.%s called on null or undefined", method);
		status = vm_throw_type_error(realm, message);
		return status;
	}

	/* ToString. */
	status = vm_to_string(realm, this_value, string);
	return status;
}

/* Finds the string value of this for toString and valueOf: a string or a String object's. */
static int
string_this_value(
	struct vm_realm *realm,
	vm_value this_value,
	vm_value *value)
{
	struct vm_object *object;
	int is_string;
	int is_object;
	int status;

	/* A string. */
	is_string = vm_value_is_string(this_value);
	if (is_string) {
		*value = this_value;
		return 0;
	}

	/* A String object's value. */
	is_object = vm_value_is_object(this_value);
	if (is_object) {
		object = (struct vm_object *)vm_value_as_cell(this_value);
		if (object->kind == VM_KIND_STRING) {
			*value = object->internal;
			return 0;
		}
	}

	/* Anything else. */
	status = vm_throw_type_error(realm, "String.prototype.toString requires that 'this' be a String");
	return status;
}

/* Converts an argument to a string (undefined is "undefined"). */
static int
string_argument(
	struct vm_realm *realm,
	const vm_value *args,
	unsigned count,
	unsigned index,
	struct vm_string **string)
{
	int status;

	/* ToString of the argument. */
	status = vm_to_string(realm, js_argument(args, count, index), string);
	return status;
}

/* Finds a string in another from a position (forwards, or backwards from it); reports whether and where. */
static int
string_find(
	const struct vm_string *haystack,
	const struct vm_string *needle,
	uint32_t from,
	int backwards,
	uint32_t *found)
{
	uint32_t position;
	int matches;

	/* A needle longer than the haystack is nowhere. */
	if (needle->length > haystack->length)
		return 0;

	/* Backwards from the start position (or the last place it fits). */
	if (backwards) {
		position = haystack->length - needle->length;
		if (from < position)
			position = from;
		for (;;) {
			matches = string_matches_at(haystack, needle, position);
			if (matches) {
				*found = position;
				return 1;
			}

			/* The start reached. */
			if (position == 0)
				return 0;
			position--;
		}
	}

	/* Forwards. */
	for (position = from; position + needle->length <= haystack->length; position++) {
		matches = string_matches_at(haystack, needle, position);
		if (matches) {
			*found = position;
			return 1;
		}
	}

	/* Not found. */
	return 0;
}

/* Tells whether a string's units at a position are another string's. */
static int
string_matches_at(
	const struct vm_string *haystack,
	const struct vm_string *needle,
	uint32_t position)
{
	uint32_t index;
	uint16_t unit;
	uint16_t other;

	/* Each unit. */
	for (index = 0; index < needle->length; index++) {
		unit = vm_string_at(haystack, position + index);
		other = vm_string_at(needle, index);
		if (unit != other)
			return 0;
	}

	/* All match. */
	return 1;
}

/* Makes a string of a range of another's units. */
static int
string_substring_value(
	struct vm_realm *realm,
	const struct vm_string *string,
	uint32_t start,
	uint32_t end,
	vm_value *result)
{
	struct wb_units units;
	uint32_t index;
	uint16_t unit;
	int status;

	/* The units of the range. */
	wb_units_init(&units);
	status = 0;
	for (index = start; status == 0 && index < end; index++) {
		unit = vm_string_at(string, index);
		status = wb_units_append(&units, &unit, 1);
	}

	/* The string. */
	if (status == 0)
		status = string_units_value(realm, &units, result);
	wb_units_release(&units);
	return status;
}

/* Makes a string value of a list of units. */
static int
string_units_value(
	struct vm_realm *realm,
	struct wb_units *units,
	vm_value *result)
{
	struct vm_string *string;

	/* The string (an empty list is the empty string). */
	string = vm_string_from_units(realm->heap, units->data, units->length);
	if (string == NULL)
		return ENOMEM;
	*result = vm_value_cell(string);
	return 0;
}

/* Maps this's string to upper or lower case (the full mappings, and the final sigma when lowering). */
static int
string_case(
	struct vm_realm *realm,
	vm_value this_value,
	int upper,
	vm_value *result)
{
	struct wb_units units;
	struct vm_string *string;
	uint32_t mapped[WB_CASE_MAX];
	const char *method;
	uint32_t index;
	uint32_t size;
	uint32_t point;
	unsigned mapped_count;
	unsigned item;
	int final;
	int status;

	/* The string. */
	method = "toLowerCase";
	if (upper)
		method = "toUpperCase";
	status = string_this(realm, this_value, method, &string);
	if (status != 0)
		return status;

	/* Each code point mapped. */
	wb_units_init(&units);
	for (index = 0; status == 0 && index < string->length; index += size) {
		point = string_code_point(string, index, &size);
		if (!upper && point == STRING_SIGMA) {
			final = string_final_sigma(string, index);
			point = STRING_SMALL_SIGMA;
			if (final)
				point = STRING_FINAL_SIGMA;
			status = wb_units_append_code_point(&units, point);
			continue;
		}

		/* Any other code point by its mapping. */
		mapped_count = wb_case_map(point, upper, mapped);
		for (item = 0; status == 0 && item < mapped_count; item++)
			status = wb_units_append_code_point(&units, mapped[item]);
	}

	/* The string. */
	if (status == 0)
		status = string_units_value(realm, &units, result);
	wb_units_release(&units);
	return status;
}

/* Tells whether a capital sigma is final: a cased letter before it and none after (case-ignorable ones skipped). */
static int
string_final_sigma(
	const struct vm_string *string,
	uint32_t index)
{
	uint32_t position;
	uint32_t size;
	uint32_t point;
	int before;
	int after;
	int ignorable;

	/* Before: skipping the case-ignorable, a cased letter. */
	before = 0;
	position = index;
	while (position > 0) {
		point = string_code_point_before(string, position, &size);
		position -= size;
		ignorable = wb_case_is_ignorable(point);
		if (ignorable)
			continue;
		before = wb_case_is_cased(point);
		break;
	}

	/* Only after a cased letter. */
	if (!before)
		return 0;

	/* After: skipping the case-ignorable, no cased letter. */
	after = 0;
	for (position = index + 1U; position < string->length; position += size) {
		point = string_code_point(string, position, &size);
		ignorable = wb_case_is_ignorable(point);
		if (ignorable)
			continue;
		after = wb_case_is_cased(point);
		break;
	}

	/* Final when nothing cased follows. */
	return !after;
}

/* Reads the code point at an index (a lone surrogate is itself); reports its size in units. */
static uint32_t
string_code_point(
	const struct vm_string *string,
	uint32_t index,
	uint32_t *size)
{
	uint16_t first;
	uint16_t second;

	/* One unit, unless a high surrogate is followed by a low one. */
	*size = 1;
	first = vm_string_at(string, index);
	if (first < 0xD800U || first > 0xDBFFU || index + 1U >= string->length)
		return first;
	second = vm_string_at(string, index + 1U);
	if (second < 0xDC00U || second > 0xDFFFU)
		return first;

	/* The pair. */
	*size = 2;
	return 0x10000U + (((uint32_t)first - 0xD800U) << 10) + ((uint32_t)second - 0xDC00U);
}

/* Reads the code point that ends before an index; reports its size in units. */
static uint32_t
string_code_point_before(
	const struct vm_string *string,
	uint32_t index,
	uint32_t *size)
{
	uint16_t low;
	uint16_t high;

	/* One unit, unless a low surrogate follows a high one. */
	*size = 1;
	low = vm_string_at(string, index - 1U);
	if (low < 0xDC00U || low > 0xDFFFU || index < 2U)
		return low;
	high = vm_string_at(string, index - 2U);
	if (high < 0xD800U || high > 0xDBFFU)
		return low;

	/* The pair. */
	*size = 2;
	return 0x10000U + (((uint32_t)high - 0xD800U) << 10) + ((uint32_t)low - 0xDC00U);
}

/* Pads this's string at the start or the end to a length with a fill string (a space by default). */
static int
string_pad(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	int at_start,
	vm_value *result)
{
	struct wb_units units;
	struct vm_string *string;
	struct vm_string *filler;
	const char *method;
	vm_value filler_value;
	double length;
	uint32_t fill_length;
	uint32_t index;
	uint16_t unit;
	int status;

	/* The string, the length and the filler. */
	method = "padEnd";
	if (at_start)
		method = "padStart";
	status = string_this(realm, this_value, method, &string);
	if (status == 0)
		status = js_builtin_integer(realm, js_argument(args, count, 0), &length);
	if (status != 0)
		return status;
	*result = vm_value_cell(string);
	if (length <= (double)string->length)
		return 0;
	filler_value = js_argument(args, count, 1);
	if (filler_value == VM_VALUE_UNDEFINED) {
		filler = vm_string_from_utf8(realm->heap, " ", 1);
		if (filler == NULL)
			return ENOMEM;
	} else {
		status = vm_to_string(realm, args[1], &filler);
		if (status != 0)
			return status;
	}

	/* An empty filler adds nothing. */
	if (filler->length == 0)
		return 0;
	if (length > 268435456.0) {
		status = vm_throw_range_error(realm, "Invalid string length");
		return status;
	}

	/* The fill, repeated and cut, on the side asked. */
	fill_length = (uint32_t)length - string->length;
	wb_units_init(&units);
	if (!at_start)
		status = vm_string_append_units(string, &units);
	for (index = 0; status == 0 && index < fill_length; index++) {
		unit = vm_string_at(filler, index % filler->length);
		status = wb_units_append(&units, &unit, 1);
	}

	/* The string after the fill at the start. */
	if (status == 0 && at_start)
		status = vm_string_append_units(string, &units);
	if (status == 0)
		status = string_units_value(realm, &units, result);
	wb_units_release(&units);
	return status;
}

/* Trims white space and line terminators from this's string at the ends asked. */
static int
string_trim_with(
	struct vm_realm *realm,
	vm_value this_value,
	int ends,
	vm_value *result)
{
	struct vm_string *string;
	uint32_t start;
	uint32_t end;
	int blank;
	int status;

	/* The string. */
	status = string_this(realm, this_value, "trim", &string);
	if (status != 0)
		return status;

	/* The blanks at each end asked. */
	start = 0;
	end = string->length;
	while ((ends & STRING_TRIM_START) != 0 && start < end) {
		blank = vm_is_space(vm_string_at(string, start));
		if (!blank)
			break;
		start++;
	}
	while ((ends & STRING_TRIM_END) != 0 && end > start) {
		blank = vm_is_space(vm_string_at(string, end - 1U));
		if (!blank)
			break;
		end--;
	}

	/* The part. */
	status = string_substring_value(realm, string, start, end, result);
	return status;
}

/* String.prototype.match(regexp): a regular expression's (or one made from the argument's) matches. */
static int
string_match(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	vm_value regexp;
	int is_regexp;
	int done;
	int status;

	/* An object's Symbol.match does the work (ws074-p087). */
	status = string_delegate(realm, this_value, js_argument(args, count, 0), VM_SYMBOL_MATCH, NULL, 0, &done, result);
	if (status != 0 || done)
		return status;

	/* The string, and the regular expression (one made from anything else). */
	status = string_this(realm, this_value, "match", &string);
	if (status != 0)
		return status;
	regexp = js_argument(args, count, 0);
	is_regexp = js_regexp_is(regexp);
	if (!is_regexp) {
		status = js_regexp_create(realm, regexp, VM_VALUE_UNDEFINED, &regexp);
		if (status != 0)
			return status;
	}

	/* The matches. */
	status = js_regexp_symbol_match(realm, regexp, string, result);
	if (status != 0)
		return status;

	/* Succeeded: the result. */
	return 0;
}

/* String.prototype.replace(search, replacement): the first match (of a string, or a regular expression's) replaced. */
static int
string_replace(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	vm_value replacement;
	int is_regexp;
	int done;
	int status;

	/* An object's Symbol.replace does the work (ws074-p087). */
	replacement = js_argument(args, count, 1);
	status = string_delegate(realm, this_value, js_argument(args, count, 0), VM_SYMBOL_REPLACE, &replacement, 1, &done, result);
	if (status != 0 || done)
		return status;

	/* A regular expression replaces by its own algorithm. */
	status = string_this(realm, this_value, "replace", &string);
	if (status != 0)
		return status;
	is_regexp = js_regexp_is(js_argument(args, count, 0));
	if (is_regexp) {
		status = js_regexp_symbol_replace(realm, js_argument(args, count, 0), string, js_argument(args, count, 1), result);
		return status;
	}

	/* A string's first occurrence. */
	status = string_replace_text(realm, string, js_argument(args, count, 0), js_argument(args, count, 1), 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the replaced string. */
	return 0;
}

/* String.prototype.replaceAll(search, replacement): every match replaced (a regular expression must be global). */
static int
string_replace_all(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct vm_string *flags_text;
	vm_value search;
	vm_value flags;
	vm_value key;
	uint32_t index;
	uint16_t letter;
	int global;
	int is_regexp;
	int status;

	/* A regular expression must have g, then replaces by its own algorithm. */
	status = string_this(realm, this_value, "replaceAll", &string);
	if (status != 0)
		return status;
	search = js_argument(args, count, 0);
	is_regexp = js_regexp_is(search);
	if (is_regexp) {
		key = vm_key_from_ascii(realm->heap, "flags");
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;
		status = vm_get(realm, search, key, &flags);
		if (status != 0)
			return status;
		if (flags == VM_VALUE_UNDEFINED || flags == VM_VALUE_NULL) {
			status = vm_throw_type_error(realm, "String.prototype.replaceAll called with a RegExp without flags");
			return status;
		}

		/* The flags must have g. */
		status = vm_to_string(realm, flags, &flags_text);
		if (status != 0)
			return status;
		global = 0;
		for (index = 0; index < flags_text->length; index++) {
			letter = vm_string_at(flags_text, index);
			if (letter == 'g')
				global = 1;
		}

		/* Without g replaceAll is refused. */
		if (!global) {
			status = vm_throw_type_error(realm, "replaceAll must be called with a global RegExp");
			return status;
		}

		/* The regular expression's algorithm. */
		status = js_regexp_symbol_replace(realm, search, string, js_argument(args, count, 1), result);
		return status;
	}

	/* Every occurrence of a string. */
	status = string_replace_text(realm, string, search, js_argument(args, count, 1), 1, result);
	if (status != 0)
		return status;

	/* Succeeded: the replaced string. */
	return 0;
}

/* String.prototype.search(regexp): where a regular expression (or one made from the argument) first matches, or -1. */
static int
string_search(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	vm_value regexp;
	int is_regexp;
	int done;
	int status;

	/* An object's Symbol.search does the work (ws074-p087). */
	status = string_delegate(realm, this_value, js_argument(args, count, 0), VM_SYMBOL_SEARCH, NULL, 0, &done, result);
	if (status != 0 || done)
		return status;

	/* The string, and the regular expression (one made from anything else). */
	status = string_this(realm, this_value, "search", &string);
	if (status != 0)
		return status;
	regexp = js_argument(args, count, 0);
	is_regexp = js_regexp_is(regexp);
	if (!is_regexp) {
		status = js_regexp_create(realm, regexp, VM_VALUE_UNDEFINED, &regexp);
		if (status != 0)
			return status;
	}

	/* The search. */
	status = js_regexp_symbol_search(realm, regexp, string, result);
	if (status != 0)
		return status;

	/* Succeeded: the index. */
	return 0;
}

/*
 * String.prototype.split(separator, limit): the parts between the
 * separator's matches (a regular expression's, or a string's; each unit
 * for the empty string), at most limit of them.
 */
static int
string_split(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct vm_string *separator;
	vm_value separator_value;
	vm_value limit_value;
	vm_value list;
	vm_value part;
	uint32_t limit;
	uint32_t parts;
	uint32_t start;
	uint32_t found;
	int is_regexp;
	int present;
	int done;
	int status;

	/* An object's Symbol.split does the work (ws074-p087). */
	separator_value = js_argument(args, count, 0);
	limit_value = js_argument(args, count, 1);
	status = string_delegate(realm, this_value, separator_value, VM_SYMBOL_SPLIT, &limit_value, 1, &done, result);
	if (status != 0 || done)
		return status;

	/* A regular expression splits by its own algorithm. */
	status = string_this(realm, this_value, "split", &string);
	if (status != 0)
		return status;
	is_regexp = js_regexp_is(separator_value);
	if (is_regexp) {
		status = js_regexp_symbol_split(realm, separator_value, string, limit_value, result);
		return status;
	}

	/* The limit (2^32 - 1 without one) and the separator as a string. */
	limit = UINT32_MAX;
	if (limit_value != VM_VALUE_UNDEFINED) {
		status = vm_to_uint32(realm, limit_value, &limit);
		if (status != 0)
			return status;
	}

	/* The separator as a string, and the list of parts. */
	status = vm_to_string(realm, separator_value, &separator);
	if (status == 0)
		status = js_builtin_array(realm, NULL, 0, &list);
	if (status != 0)
		return status;

	/* No parts for a limit of 0; the whole string without a separator. */
	*result = list;
	if (limit == 0)
		return 0;
	if (separator_value == VM_VALUE_UNDEFINED) {
		status = string_push(realm, list, vm_value_cell(string));
		return status;
	}

	/* The empty separator splits between the units. */
	if (separator->length == 0) {
		for (start = 0; start < string->length && start < limit; start++) {
			status = string_substring_value(realm, string, start, start + 1U, &part);
			if (status == 0)
				status = string_push(realm, list, part);
			if (status != 0)
				return status;
		}

		/* Succeeded: the units. */
		return 0;
	}

	/* An empty string is one part. */
	if (string->length == 0) {
		status = string_push(realm, list, vm_value_cell(string));
		return status;
	}

	/* The parts before each occurrence, then the rest. */
	parts = 0;
	start = 0;
	for (;;) {
		present = string_find(string, separator, start, 0, &found);
		if (!present)
			break;
		status = string_substring_value(realm, string, start, found, &part);
		if (status == 0)
			status = string_push(realm, list, part);
		if (status != 0)
			return status;
		parts++;
		if (parts == limit)
			return 0;
		start = found + separator->length;
	}

	/* The rest is the last part. */
	status = string_substring_value(realm, string, start, string->length, &part);
	if (status == 0)
		status = string_push(realm, list, part);
	if (status != 0)
		return status;

	/* Succeeded: the parts. */
	return 0;
}

/*
 * Replaces a string's first (or every) occurrence of a search string by a
 * function's result or a replacement string's substitution.
 */
static int
string_replace_text(
	struct vm_realm *realm,
	struct vm_string *string,
	vm_value search_value,
	vm_value replace_value,
	int all,
	vm_value *result)
{
	struct wb_vector positions;
	struct wb_units out;
	struct vm_string *search;
	struct vm_string *replacement;
	struct vm_string *text;
	vm_value arguments[3];
	vm_value replaced;
	uint32_t *position;
	uint32_t from;
	uint32_t found;
	uint32_t advance;
	uint32_t end;
	size_t index;
	int functional;
	int present;
	int status;

	/* The search string, and the replacement: a function or a string. */
	status = vm_to_string(realm, search_value, &search);
	if (status != 0)
		return status;
	functional = vm_value_is_callable(replace_value);
	replacement = NULL;
	if (!functional) {
		status = vm_to_string(realm, replace_value, &replacement);
		if (status != 0)
			return status;
	}

	/* Where the search occurs: the first time, or every time (an empty search at each unit and the end). */
	wb_vector_init(&positions, sizeof(uint32_t));
	advance = search->length;
	if (advance == 0)
		advance = 1;
	from = 0;
	status = 0;
	for (;;) {
		present = string_find(string, search, from, 0, &found);
		if (!present)
			break;
		status = wb_vector_push(&positions, &found);
		if (status != 0 || !all)
			break;
		from = found + advance;
		if (from > string->length)
			break;
	}

	/* The text between the occurrences and each one's replacement. */
	wb_units_init(&out);
	end = 0;
	for (index = 0; status == 0 && index < positions.count; index++) {
		position = wb_vector_at(&positions, index);
		if (functional) {
			arguments[0] = vm_value_cell(search);
			arguments[1] = vm_value_number((double)*position);
			arguments[2] = vm_value_cell(string);
			status = vm_call(realm, replace_value, VM_VALUE_UNDEFINED, arguments, 3, &replaced);
			if (status == 0)
				status = vm_to_string(realm, replaced, &text);
		} else {
			status = js_regexp_substitution(realm, search, string, *position, NULL, 0, VM_VALUE_UNDEFINED, replacement, &replaced);
			if (status == 0)
				text = (struct vm_string *)vm_value_as_cell(replaced);
		}

		/* The text before the occurrence, then its replacement. */
		if (status == 0)
			status = string_append_range(&out, string, end, *position);
		if (status == 0)
			status = vm_string_append_units(text, &out);
		end = *position + search->length;
	}

	/* The rest, then the string (the original itself when nothing occurred). */
	if (status == 0 && positions.count == 0) {
		*result = vm_value_cell(string);
	} else if (status == 0) {
		status = string_append_range(&out, string, end, string->length);
		if (status == 0)
			status = string_units_value(realm, &out, result);
	}

	/* The lists go. */
	wb_units_release(&out);
	wb_vector_release(&positions);
	if (status != 0)
		return status;

	/* Succeeded: the replaced string. */
	return 0;
}

/* Appends a range of a string's units. */
static int
string_append_range(
	struct wb_units *units,
	const struct vm_string *string,
	uint32_t start,
	uint32_t end)
{
	uint32_t index;
	uint16_t unit;
	int status;

	/* Each unit of the range. */
	status = 0;
	for (index = start; status == 0 && index < end && index < string->length; index++) {
		unit = vm_string_at(string, index);
		status = wb_units_append(units, &unit, 1);
	}

	/* A failure to append. */
	if (status != 0)
		return status;

	/* Succeeded: the range is appended. */
	return 0;
}

/* Appends a value to an array at its end. */
static int
string_push(
	struct vm_realm *realm,
	vm_value array,
	vm_value value)
{
	struct vm_object *object;
	int status;

	/* The next index. */
	object = (struct vm_object *)vm_value_as_cell(array);
	status = vm_object_define(realm->heap, object, vm_value_int32((int32_t)object->length), value, VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* Succeeded: the value is at the end. */
	return 0;
}

/* Tells whether a value is a regular expression (only RegExp objects for now; Symbol.match comes later). */
static int
string_is_regexp(
	vm_value value)
{
	struct vm_object *object;
	int is_object;

	/* A RegExp object. */
	is_object = vm_value_is_object(value);
	if (!is_object)
		return 0;
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind == VM_KIND_REGEXP)
		return 1;

	/* Anything else. */
	return 0;
}

/* Converts a position argument to a units index clamped to the string (undefined is the fallback). */
static int
string_position(
	struct vm_realm *realm,
	vm_value value,
	uint32_t length,
	uint32_t fallback,
	uint32_t *position)
{
	double number;
	int status;

	/* undefined is the fallback. */
	*position = fallback;
	if (value == VM_VALUE_UNDEFINED)
		return 0;
	status = js_builtin_integer(realm, value, &number);
	if (status != 0)
		return status;

	/* Clamped. */
	if (number < 0.0)
		number = 0.0;
	if (number > (double)length)
		number = (double)length;
	*position = (uint32_t)number;
	return 0;
}

/*
 * Lets an object argument of match, replace, search or split do the work
 * with its method keyed by a well-known symbol (the RegExp's, or a
 * user's): the method is called on it with this (not undefined or null)
 * and the extra arguments.  *done says it ran.
 */
static int
string_delegate(
	struct vm_realm *realm,
	vm_value this_value,
	vm_value argument,
	int which,
	const vm_value *extra,
	unsigned extra_count,
	int *done,
	vm_value *result)
{
	vm_value method;
	vm_value arguments[2];
	int is_object;
	int status;

	/* this must not be undefined or null. */
	*done = 0;
	if (this_value == VM_VALUE_UNDEFINED || this_value == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "String.prototype method called on null or undefined");
		return status;
	}

	/* Only an object's method counts. */
	is_object = vm_value_is_object(argument);
	if (!is_object)
		return 0;
	status = vm_get_method(realm, argument, vm_symbol_key(realm, which), &method);
	if (status != 0)
		return status;
	if (method == VM_VALUE_UNDEFINED)
		return 0;

	/* The call with this and the extra argument. */
	arguments[0] = this_value;
	arguments[1] = VM_VALUE_UNDEFINED;
	if (extra_count > 0)
		arguments[1] = extra[0];
	*done = 1;
	status = vm_call(realm, method, argument, arguments, 2, result);
	if (status != 0)
		return status;

	/* Succeeded: the method's result. */
	return 0;
}
