/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * RegExp (ws074-p027): the constructor, RegExp.prototype's exec, test,
 * toString, compile (Annex B) and the flag and source getters, and the
 * algorithms of its @@match, @@replace, @@search and @@split that
 * String.prototype's match, replace, replaceAll, search and split use
 * (there are no symbols yet: a RegExp object is what IsRegExp finds).
 *
 * A RegExp object is an ordinary object of kind VM_KIND_REGEXP whose
 * internal value is a state cell: the pattern and flags it was made with
 * and the compiled program (regexp.c), freed with the cell.  The
 * algorithms follow the specification's generic steps (Get of exec, flags
 * and lastIndex, the result's properties), so a script that replaces exec
 * or reads the results sees what the specification says.  The lists they
 * build are arrays in the heap, so that the collector sees what they hold.
 */

#include "js/builtin.h"
#include "js/regexp.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The largest integer ToLength gives. */
#define REGEXP_LENGTH_MAX	9007199254740991.0

/* The most parts split makes when no limit is given (2^32 - 1). */
#define REGEXP_SPLIT_MAX	4294967295.0

/*
 * The state of a RegExp object: its pattern and flags as given (the
 * source and flags it reports come from them), and its compiled program.
 */
struct regexp_state {
	struct vm_cell cell;
	struct vm_string *source;
	struct vm_string *flags;
	struct js_regexp_program *program;
};

/*
 * One flag getter: its name, the flag and the character the flags getter
 * spells it with (in the flags getter's order).
 */
struct regexp_flag {
	const char *name;
	unsigned flag;
	char letter;
	vm_native getter;
};

static void regexp_state_trace(struct vm_heap *heap, struct vm_cell *cell);
static void regexp_state_finalize(struct vm_heap *heap, struct vm_cell *cell);
static int regexp_symbol_method(struct vm_realm *realm, struct vm_object *prototype, int which, const char *name, unsigned length, vm_native native);
static int regexp_symbol_match_method(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_symbol_replace_method(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_symbol_search_method(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_symbol_split_method(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_symbol_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, struct vm_string **string);
static int regexp_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_exec(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_test(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_to_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_compile_method(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_flags(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_source(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_has_indices(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_global(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_ignore_case(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_multiline(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_dot_all(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_unicode(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_unicode_sets(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_sticky(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int regexp_get_flag(struct vm_realm *realm, vm_value this_value, unsigned index, vm_value *result);
static struct regexp_state *regexp_state_of(vm_value value);
static int regexp_make(struct vm_realm *realm, vm_value new_target, vm_value pattern, vm_value flags, vm_value *result);
static int regexp_initialize(struct vm_realm *realm, struct vm_object *object, vm_value pattern, vm_value flags);
static int regexp_builtin_exec(struct vm_realm *realm, vm_value regexp, struct vm_string *string, vm_value *result);
static int regexp_result(struct vm_realm *realm, const struct js_regexp_program *program, struct vm_string *string, const size_t *captures, vm_value *result);
static int regexp_exec_generic(struct vm_realm *realm, vm_value regexp, struct vm_string *string, vm_value *result);
static int regexp_flags_text(struct vm_realm *realm, vm_value regexp, unsigned *flags);
static int regexp_get(struct vm_realm *realm, vm_value object, const char *name, vm_value *value);
static int regexp_set(struct vm_realm *realm, vm_value object, const char *name, vm_value value);
static int regexp_to_length(struct vm_realm *realm, vm_value value, double *length);
static int regexp_get_string(struct vm_realm *realm, vm_value object, const char *name, struct vm_string **string);
static int regexp_get_match(struct vm_realm *realm, vm_value result, struct vm_string **string);
static void regexp_input(const struct vm_string *string, struct js_regexp_input *input);
static int regexp_poll(void *context);
static int regexp_slice(struct vm_realm *realm, const struct vm_string *string, size_t start, size_t end, vm_value *result);
static size_t regexp_advance(const struct vm_string *string, size_t index, int unicode);
static int regexp_append(struct wb_units *units, const struct vm_string *string, size_t start, size_t end);
static int regexp_push_value(struct vm_realm *realm, vm_value array, vm_value value);
static int regexp_units_value(struct vm_realm *realm, const struct wb_units *units, vm_value *result);
static int regexp_escape_source(struct vm_realm *realm, const struct vm_string *source, vm_value *result);

/* The type of the state cells. */
static const struct vm_cell_type regexp_state_type = {
	"regexp",
	regexp_state_trace,
	regexp_state_finalize
};

/* The flag getters, in the flags getter's order (d g i m s u v y). */
static const struct regexp_flag regexp_flags[] = {
	{ "hasIndices", JS_REGEXP_HAS_INDICES, 'd', regexp_get_has_indices },
	{ "global", JS_REGEXP_GLOBAL, 'g', regexp_get_global },
	{ "ignoreCase", JS_REGEXP_IGNORE_CASE, 'i', regexp_get_ignore_case },
	{ "multiline", JS_REGEXP_MULTILINE, 'm', regexp_get_multiline },
	{ "dotAll", JS_REGEXP_DOT_ALL, 's', regexp_get_dot_all },
	{ "unicode", JS_REGEXP_UNICODE, 'u', regexp_get_unicode },
	{ "unicodeSets", JS_REGEXP_UNICODE_SETS, 'v', regexp_get_unicode_sets },
	{ "sticky", JS_REGEXP_STICKY, 'y', regexp_get_sticky },
	{ NULL, 0, 0, NULL }
};

/* The index of each flag in the table. */
#define REGEXP_FLAG_HAS_INDICES		0U
#define REGEXP_FLAG_GLOBAL		1U
#define REGEXP_FLAG_IGNORE_CASE		2U
#define REGEXP_FLAG_MULTILINE		3U
#define REGEXP_FLAG_DOT_ALL		4U
#define REGEXP_FLAG_UNICODE		5U
#define REGEXP_FLAG_UNICODE_SETS	6U
#define REGEXP_FLAG_STICKY		7U

/*
 * Installs RegExp: the constructor, its prototype (an ordinary object)
 * with the methods and getters, and the realm's intrinsics that regular
 * expression literals are made from.
 */
int
js_builtin_install_regexp(
	struct vm_realm *realm)
{
	const struct regexp_flag *flag;
	struct vm_function *constructor;
	struct vm_object *prototype;
	int error;

	/* The prototype and the constructor. */
	prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (prototype == NULL)
		return ENOMEM;
	error = js_builtin_constructor(realm, "RegExp", 2, regexp_call, regexp_construct, prototype, &constructor);
	if (error != 0)
		return error;
	realm->intrinsics[VM_INTRINSIC_REGEXP_PROTOTYPE] = prototype;
	realm->intrinsics[VM_INTRINSIC_REGEXP] = &constructor->object;

	/* The methods. */
	error = js_builtin_method(realm, prototype, "exec", 1, regexp_exec);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "test", 1, regexp_test);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "toString", 0, regexp_to_string);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "compile", 2, regexp_compile_method);
	if (error != 0)
		return error;

	/* The getters: flags, source and each flag. */
	error = js_builtin_accessor(realm, prototype, "flags", regexp_get_flags, NULL);
	if (error == 0)
		error = js_builtin_accessor(realm, prototype, "source", regexp_get_source, NULL);
	for (flag = regexp_flags; error == 0 && flag->name != NULL; flag++)
		error = js_builtin_accessor(realm, prototype, flag->name, flag->getter, NULL);
	if (error != 0)
		return error;

	/* The methods String.prototype's match, replace, search and split call (ws074-p087). */
	error = regexp_symbol_method(realm, prototype, VM_SYMBOL_MATCH, "[Symbol.match]", 1, regexp_symbol_match_method);
	if (error == 0)
		error = regexp_symbol_method(realm, prototype, VM_SYMBOL_REPLACE, "[Symbol.replace]", 2, regexp_symbol_replace_method);
	if (error == 0)
		error = regexp_symbol_method(realm, prototype, VM_SYMBOL_SEARCH, "[Symbol.search]", 1, regexp_symbol_search_method);
	if (error == 0)
		error = regexp_symbol_method(realm, prototype, VM_SYMBOL_SPLIT, "[Symbol.split]", 2, regexp_symbol_split_method);
	if (error != 0)
		return error;

	/* Succeeded: RegExp is installed. */
	return 0;
}

/* Tells whether a value is a regular expression (IsRegExp: a RegExp object, as there is no Symbol.match yet). */
int
js_regexp_is(
	vm_value value)
{
	struct vm_object *object;
	int is_object;

	/* An object of the RegExp kind. */
	is_object = vm_value_is_object(value);
	if (!is_object)
		return 0;
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind == VM_KIND_REGEXP)
		return 1;

	/* Anything else. */
	return 0;
}

/* Makes a regular expression from a pattern and flags (RegExpCreate). */
int
js_regexp_create(
	struct vm_realm *realm,
	vm_value pattern,
	vm_value flags,
	vm_value *result)
{
	vm_value constructor;
	int error;

	/* With the realm's RegExp as new.target. */
	constructor = vm_value_cell(realm->intrinsics[VM_INTRINSIC_REGEXP]);
	error = regexp_make(realm, constructor, pattern, flags, result);
	if (error != 0)
		return error;

	/* Succeeded: the regular expression. */
	return 0;
}

/*
 * Matches a regular expression against a string (RegExp.prototype[@@match]):
 * without g the first match's result, with it an array of every match
 * (null for none).
 */
int
js_regexp_symbol_match(
	struct vm_realm *realm,
	vm_value regexp,
	struct vm_string *string,
	vm_value *result)
{
	struct vm_string *matched;
	vm_value found;
	vm_value list;
	double index;
	unsigned flags;
	uint32_t count;
	int error;

	/* Without g: the one match. */
	error = regexp_flags_text(realm, regexp, &flags);
	if (error != 0)
		return error;
	if ((flags & JS_REGEXP_GLOBAL) == 0) {
		error = regexp_exec_generic(realm, regexp, string, result);
		return error;
	}

	/* With it: every match from the start. */
	error = regexp_set(realm, regexp, "lastIndex", vm_value_int32(0));
	if (error != 0)
		return error;
	error = js_builtin_array(realm, NULL, 0, &list);
	if (error != 0)
		return error;
	count = 0;
	for (;;) {
		error = regexp_exec_generic(realm, regexp, string, &found);
		if (error != 0)
			return error;
		if (found == VM_VALUE_NULL)
			break;

		/* The matched text; an empty match moves lastIndex on by one character. */
		error = regexp_get_match(realm, found, &matched);
		if (error != 0)
			return error;
		error = regexp_push_value(realm, list, vm_value_cell(matched));
		if (error != 0)
			return error;
		count++;
		if (matched->length == 0) {
			error = regexp_get(realm, regexp, "lastIndex", &found);
			if (error == 0)
				error = regexp_to_length(realm, found, &index);
			if (error == 0) {
				index = (double)regexp_advance(string, (size_t)fmin(index, (double)string->length + 1.0),
				    (flags & (JS_REGEXP_UNICODE | JS_REGEXP_UNICODE_SETS)) != 0);
				error = regexp_set(realm, regexp, "lastIndex", vm_value_number(index));
			}

			/* A failure to move lastIndex. */
			if (error != 0)
				return error;
		}
	}

	/* No match is null. */
	if (count == 0) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Succeeded: the matches. */
	*result = list;
	return 0;
}

/*
 * Replaces a regular expression's matches in a string
 * (RegExp.prototype[@@replace]): the first, or with g every one, by a
 * function's result or a replacement string's substitution.
 */
int
js_regexp_symbol_replace(
	struct vm_realm *realm,
	vm_value regexp,
	struct vm_string *string,
	vm_value replace_value,
	vm_value *result)
{
	struct wb_units out;
	struct vm_string *replacement_text;
	struct vm_string *matched;
	struct vm_string *capture_string;
	struct vm_object *results_object;
	struct vm_object *arguments;
	vm_value results;
	vm_value found;
	vm_value value;
	vm_value named;
	vm_value arguments_value;
	vm_value replaced;
	double number;
	uint32_t result_count;
	uint32_t result_index;
	uint32_t length;
	uint32_t capture;
	uint32_t capture_count;
	size_t position;
	size_t next_source;
	unsigned flags;
	int functional;
	int error;

	/* The replacement: a function, or a string. */
	functional = vm_value_is_callable(replace_value);
	replacement_text = NULL;
	if (!functional) {
		error = vm_to_string(realm, replace_value, &replacement_text);
		if (error != 0)
			return error;
		replace_value = vm_value_cell(replacement_text);
	}

	/* With g every match from the start, otherwise the first. */
	error = regexp_flags_text(realm, regexp, &flags);
	if (error != 0)
		return error;
	if ((flags & JS_REGEXP_GLOBAL) != 0) {
		error = regexp_set(realm, regexp, "lastIndex", vm_value_int32(0));
		if (error != 0)
			return error;
	}

	/* The results, in an array the collector sees. */
	error = js_builtin_array(realm, NULL, 0, &results);
	if (error != 0)
		return error;
	for (;;) {
		error = regexp_exec_generic(realm, regexp, string, &found);
		if (error != 0)
			return error;
		if (found == VM_VALUE_NULL)
			break;
		error = regexp_push_value(realm, results, found);
		if (error != 0)
			return error;
		if ((flags & JS_REGEXP_GLOBAL) == 0)
			break;

		/* An empty match moves lastIndex on by one character. */
		error = regexp_get_match(realm, found, &matched);
		if (error != 0)
			return error;
		if (matched->length == 0) {
			error = regexp_get(realm, regexp, "lastIndex", &value);
			if (error == 0)
				error = regexp_to_length(realm, value, &number);
			if (error == 0) {
				number = (double)regexp_advance(string, (size_t)fmin(number, (double)string->length + 1.0),
				    (flags & (JS_REGEXP_UNICODE | JS_REGEXP_UNICODE_SETS)) != 0);
				error = regexp_set(realm, regexp, "lastIndex", vm_value_number(number));
			}

			/* A failure to move lastIndex. */
			if (error != 0)
				return error;
		}
	}

	/* Each result's replacement joins the text between the matches. */
	wb_units_init(&out);
	next_source = 0;
	results_object = (struct vm_object *)vm_value_as_cell(results);
	result_count = results_object->length;
	for (result_index = 0; result_index < result_count; result_index++) {
		found = results_object->elements[result_index];

		/* The match, where it is, and its captures (strings or undefined) after it in an array. */
		error = js_builtin_length(realm, found, &length);
		if (error == 0)
			error = regexp_get_match(realm, found, &matched);
		if (error == 0)
			error = regexp_get(realm, found, "index", &value);
		if (error == 0)
			error = js_builtin_integer(realm, value, &number);
		if (error != 0)
			break;
		position = (size_t)fmax(fmin(number, (double)string->length), 0.0);
		capture_count = 0;
		if (length > 0)
			capture_count = length - 1U;
		error = js_builtin_array(realm, NULL, 0, &arguments_value);
		if (error == 0)
			error = regexp_push_value(realm, arguments_value, vm_value_cell(matched));
		for (capture = 1; error == 0 && capture <= capture_count; capture++) {
			error = vm_get(realm, found, vm_value_int32((int32_t)capture), &value);
			if (error == 0 && value != VM_VALUE_UNDEFINED) {
				error = vm_to_string(realm, value, &capture_string);
				value = vm_value_cell(capture_string);
			}

			/* The capture joins the list. */
			if (error == 0)
				error = regexp_push_value(realm, arguments_value, value);
		}

		/* The named captures. */
		if (error == 0)
			error = regexp_get(realm, found, "groups", &named);
		if (error != 0)
			break;
		arguments = (struct vm_object *)vm_value_as_cell(arguments_value);

		/* A function is called with the match, the captures, the position, the string (and the groups). */
		if (functional) {
			error = regexp_push_value(realm, arguments_value, vm_value_number((double)position));
			if (error == 0)
				error = regexp_push_value(realm, arguments_value, vm_value_cell(string));
			if (error == 0 && named != VM_VALUE_UNDEFINED)
				error = regexp_push_value(realm, arguments_value, named);
			if (error == 0)
				error = vm_call(realm, replace_value, VM_VALUE_UNDEFINED, arguments->elements, arguments->length, &replaced);
			if (error == 0)
				error = vm_to_string(realm, replaced, &capture_string);
			if (error != 0)
				break;
			replaced = vm_value_cell(capture_string);
		} else {
			/* A string's substitution, with the groups as an object when there are any. */
			if (named != VM_VALUE_UNDEFINED) {
				error = vm_to_object(realm, named, &named);
				if (error != 0)
					break;
			}

			/* The substitution of the replacement string. */
			error = js_regexp_substitution(realm, matched, string, position, &arguments->elements[1], capture_count, named,
			    replacement_text, &replaced);
			if (error != 0)
				break;
		}

		/* A match after the last one's end is replaced; the text before it is kept. */
		if (position >= next_source) {
			error = regexp_append(&out, string, next_source, position);
			if (error == 0)
				error = regexp_append(&out, (struct vm_string *)vm_value_as_cell(replaced), 0, UINT32_MAX);
			if (error != 0)
				break;
			next_source = position + matched->length;
		}
	}

	/* The text after the last replaced match. */
	if (error == 0 && next_source < string->length)
		error = regexp_append(&out, string, next_source, string->length);
	if (error == 0)
		error = regexp_units_value(realm, &out, result);
	wb_units_release(&out);
	if (error != 0)
		return error;

	/* Succeeded: the replaced string. */
	return 0;
}

/*
 * Finds where a regular expression first matches a string
 * (RegExp.prototype[@@search]), from the start whatever its lastIndex
 * (which is put back): the index, or -1.
 */
int
js_regexp_symbol_search(
	struct vm_realm *realm,
	vm_value regexp,
	struct vm_string *string,
	vm_value *result)
{
	vm_value previous;
	vm_value current;
	vm_value found;
	int same;
	int error;

	/* lastIndex 0 for the search. */
	error = regexp_get(realm, regexp, "lastIndex", &previous);
	if (error != 0)
		return error;
	same = vm_same_value(previous, vm_value_int32(0));
	if (!same) {
		error = regexp_set(realm, regexp, "lastIndex", vm_value_int32(0));
		if (error != 0)
			return error;
	}

	/* The match. */
	error = regexp_exec_generic(realm, regexp, string, &found);
	if (error != 0)
		return error;

	/* lastIndex as it was. */
	error = regexp_get(realm, regexp, "lastIndex", &current);
	if (error != 0)
		return error;
	same = vm_same_value(current, previous);
	if (!same) {
		error = regexp_set(realm, regexp, "lastIndex", previous);
		if (error != 0)
			return error;
	}

	/* No match is -1. */
	if (found == VM_VALUE_NULL) {
		*result = vm_value_int32(-1);
		return 0;
	}

	/* Succeeded: the match's index. */
	error = regexp_get(realm, found, "index", result);
	if (error != 0)
		return error;
	return 0;
}

/*
 * Splits a string at a regular expression's matches
 * (RegExp.prototype[@@split]): a sticky copy of it is tried at each
 * position; an empty match at the last split does not split; the
 * captures join the parts; at most limit parts.
 */
int
js_regexp_symbol_split(
	struct vm_realm *realm,
	vm_value regexp,
	struct vm_string *string,
	vm_value limit,
	vm_value *result)
{
	struct vm_string *flags_string;
	struct wb_units sticky_flags;
	vm_value arguments[2];
	vm_value splitter;
	vm_value flags_value;
	vm_value list;
	vm_value found;
	vm_value value;
	vm_value part;
	double number;
	double most;
	uint32_t count;
	uint32_t length;
	uint32_t capture;
	uint32_t limit_integer;
	size_t size;
	size_t p;
	size_t q;
	size_t e;
	unsigned flags;
	int unicode;
	int has_sticky;
	int error;

	/* The splitter: the same pattern with its flags and y. */
	error = regexp_get(realm, regexp, "flags", &flags_value);
	if (error == 0)
		error = vm_to_string(realm, flags_value, &flags_string);
	if (error != 0)
		return error;
	wb_units_init(&sticky_flags);
	error = vm_string_append_units(flags_string, &sticky_flags);
	has_sticky = 0;
	flags = 0;
	for (count = 0; count < sticky_flags.length; count++) {
		if (sticky_flags.data[count] == 'y')
			has_sticky = 1;
		if (sticky_flags.data[count] == 'u' || sticky_flags.data[count] == 'v')
			flags |= JS_REGEXP_UNICODE;
	}

	/* u makes a surrogate pair one step. */
	unicode = (flags & JS_REGEXP_UNICODE) != 0;
	if (error == 0 && !has_sticky)
		error = wb_units_append_code_point(&sticky_flags, 'y');
	if (error == 0)
		error = regexp_units_value(realm, &sticky_flags, &flags_value);
	wb_units_release(&sticky_flags);
	if (error != 0)
		return error;
	arguments[0] = regexp;
	arguments[1] = flags_value;
	value = vm_value_cell(realm->intrinsics[VM_INTRINSIC_REGEXP]);
	error = vm_construct(realm, value, arguments, 2, value, &splitter);
	if (error != 0)
		return error;

	/* The limit (2^32 - 1 parts without one). */
	error = js_builtin_array(realm, NULL, 0, &list);
	if (error != 0)
		return error;
	most = REGEXP_SPLIT_MAX;
	if (limit != VM_VALUE_UNDEFINED) {
		error = vm_to_uint32(realm, limit, &limit_integer);
		if (error != 0)
			return error;
		most = (double)limit_integer;
	}

	/* A limit of 0 is no parts. */
	if (most == 0.0) {
		*result = list;
		return 0;
	}

	/* An empty string is one part unless the expression matches it. */
	size = string->length;
	if (size == 0) {
		error = regexp_exec_generic(realm, splitter, string, &found);
		if (error != 0)
			return error;
		if (found == VM_VALUE_NULL)
			error = regexp_push_value(realm, list, vm_value_cell(string));
		if (error != 0)
			return error;
		*result = list;
		return 0;
	}

	/* A match at each position q from the last split p. */
	count = 0;
	p = 0;
	q = 0;
	while (q < size) {
		error = regexp_set(realm, splitter, "lastIndex", vm_value_number((double)q));
		if (error == 0)
			error = regexp_exec_generic(realm, splitter, string, &found);
		if (error != 0)
			return error;
		if (found == VM_VALUE_NULL) {
			q = regexp_advance(string, q, unicode);
			continue;
		}

		/* Where the match ended; one that ends at the last split does not split. */
		error = regexp_get(realm, splitter, "lastIndex", &value);
		if (error == 0)
			error = regexp_to_length(realm, value, &number);
		if (error != 0)
			return error;
		e = (size_t)fmin(number, (double)size);
		if (e == p) {
			q = regexp_advance(string, q, unicode);
			continue;
		}

		/* The part before the match, then the captures. */
		error = regexp_slice(realm, string, p, q, &part);
		if (error == 0)
			error = regexp_push_value(realm, list, part);
		if (error != 0)
			return error;
		count++;
		if ((double)count == most) {
			*result = list;
			return 0;
		}

		/* The next part starts after the match. */
		p = e;
		error = js_builtin_length(realm, found, &length);
		if (error != 0)
			return error;
		for (capture = 1; capture < length; capture++) {
			error = vm_get(realm, found, vm_value_int32((int32_t)capture), &value);
			if (error == 0)
				error = regexp_push_value(realm, list, value);
			if (error != 0)
				return error;
			count++;
			if ((double)count == most) {
				*result = list;
				return 0;
			}
		}

		/* The search goes on from the match's end. */
		q = p;
	}

	/* The rest is the last part. */
	error = regexp_slice(realm, string, p, size, &part);
	if (error == 0)
		error = regexp_push_value(realm, list, part);
	if (error != 0)
		return error;

	/* Succeeded: the parts. */
	*result = list;
	return 0;
}

/*
 * Expands a replacement string's $ patterns for one match
 * (GetSubstitution): $$, $&, $`, $', $n and $nn (the captures, strings or
 * undefined), and $<name> (the named captures' object, or undefined).
 */
int
js_regexp_substitution(
	struct vm_realm *realm,
	struct vm_string *matched,
	struct vm_string *string,
	size_t position,
	const vm_value *captures,
	uint32_t capture_count,
	vm_value named,
	struct vm_string *replacement,
	vm_value *result)
{
	struct wb_units out;
	struct vm_string *text;
	struct vm_string *name;
	vm_value key;
	vm_value value;
	size_t index;
	size_t length;
	size_t tail;
	size_t close;
	uint32_t number;
	uint32_t two;
	size_t digits;
	uint16_t unit;
	uint16_t next;
	uint16_t second;
	uint16_t found;
	int error;

	/* Each unit or $ pattern of the replacement. */
	wb_units_init(&out);
	length = replacement->length;
	error = 0;
	for (index = 0; index < length && error == 0; index++) {
		unit = vm_string_at(replacement, index);
		next = 0;
		if (index + 1U < length)
			next = vm_string_at(replacement, index + 1U);
		if (unit != '$' || index + 1U >= length) {
			error = wb_units_append(&out, &unit, 1);
			continue;
		}

		/* What follows the $. */
		switch (next) {
		case '$':
			error = wb_units_append(&out, &unit, 1);
			index++;
			break;
		case '&':
			error = regexp_append(&out, matched, 0, matched->length);
			index++;
			break;
		case '`':
			error = regexp_append(&out, string, 0, position);
			index++;
			break;
		case '\'':
			tail = position + matched->length;
			if (tail < string->length)
				error = regexp_append(&out, string, tail, string->length);
			index++;
			break;
		case '<':
			/* $<name> with named captures and a >; literal otherwise. */
			close = index + 2U;
			while (close < length) {
				found = vm_string_at(replacement, close);
				if (found == '>')
					break;
				close++;
			}

			/* Without named captures or a >, the $ is literal. */
			if (named == VM_VALUE_UNDEFINED || close >= length) {
				error = wb_units_append(&out, &unit, 1);
				break;
			}

			/* The name between < and >. */
			error = regexp_slice(realm, replacement, index + 2U, close, &value);
			if (error == 0) {
				name = (struct vm_string *)vm_value_as_cell(value);
				error = vm_key_from_string(realm->heap, name, &key);
			}

			/* The capture of that name. */
			if (error == 0)
				error = vm_get(realm, named, key, &value);
			if (error == 0 && value != VM_VALUE_UNDEFINED) {
				error = vm_to_string(realm, value, &text);
				if (error == 0)
					error = regexp_append(&out, text, 0, text->length);
			}

			/* The pattern ends at the >. */
			index = close;
			break;
		default:
			/* $n or $nn: two digits unless they name no capture, then one; the $ is literal when neither does. */
			if (next < '0' || next > '9') {
				error = wb_units_append(&out, &unit, 1);
				break;
			}

			/* The first digit's capture. */
			number = (uint32_t)(next - '0');
			digits = 1;
			second = 0;
			if (index + 2U < length)
				second = vm_string_at(replacement, index + 2U);
			if (second >= '0' && second <= '9') {
				two = number * 10U + (uint32_t)(second - '0');
				if (two <= capture_count) {
					number = two;
					digits = 2;
				}
			}

			/* A capture that does not exist leaves the $ literal. */
			if (number < 1U || number > capture_count) {
				error = wb_units_append(&out, &unit, 1);
				break;
			}

			/* The digits are taken; the capture's text joins (nothing for undefined). */
			index += digits;
			value = captures[number - 1U];
			if (value != VM_VALUE_UNDEFINED) {
				text = (struct vm_string *)vm_value_as_cell(value);
				error = regexp_append(&out, text, 0, text->length);
			}

			break;
		}
	}

	/* The string. */
	if (error == 0)
		error = regexp_units_value(realm, &out, result);
	wb_units_release(&out);
	if (error != 0)
		return error;

	/* Succeeded: the substitution. */
	return 0;
}

/* Marks what a state cell refers to: its pattern and flags. */
static void
regexp_state_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct regexp_state *state;

	/* The two strings. */
	state = (struct regexp_state *)cell;
	if (state->source != NULL)
		vm_heap_mark(heap, &state->source->cell);
	if (state->flags != NULL)
		vm_heap_mark(heap, &state->flags->cell);
}

/* Frees a dead state cell's program. */
static void
regexp_state_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct regexp_state *state;

	/* The program is outside the heap. */
	(void)heap;
	state = (struct regexp_state *)cell;
	js_regexp_free(state->program);
	state->program = NULL;
}

/*
 * RegExp called as a function: a regular expression given without flags
 * whose constructor is RegExp is returned as it is; anything else makes a
 * new one.
 */
static int
regexp_call(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value callee;
	vm_value pattern;
	vm_value flags;
	vm_value constructor;
	int is_regexp;
	int error;

	/* The function itself is new.target. */
	(void)this_value;
	callee = vm_value_cell(js_builtin_callee(realm));
	pattern = js_argument(args, count, 0);
	flags = js_argument(args, count, 1);
	is_regexp = js_regexp_is(pattern);
	if (is_regexp && flags == VM_VALUE_UNDEFINED) {
		error = regexp_get(realm, pattern, "constructor", &constructor);
		if (error != 0)
			return error;
		if (constructor == callee) {
			*result = pattern;
			return 0;
		}
	}

	/* A new regular expression. */
	error = regexp_make(realm, callee, pattern, flags, result);
	if (error != 0)
		return error;

	/* Succeeded: the regular expression. */
	return 0;
}

/* new RegExp(pattern, flags). */
static int
regexp_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value new_target;
	int error;

	/* The object from new.target's prototype. */
	(void)this_value;
	new_target = realm->new_target;
	error = regexp_make(realm, new_target, js_argument(args, count, 0), js_argument(args, count, 1), result);
	if (error != 0)
		return error;

	/* Succeeded: the regular expression. */
	return 0;
}

/* RegExp.prototype.exec(string): the match's result array, or null. */
static int
regexp_exec(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct regexp_state *state;
	struct vm_string *string;
	int error;

	/* this must be a regular expression. */
	state = regexp_state_of(this_value);
	if (state == NULL) {
		error = vm_throw_type_error(realm, "RegExp.prototype.exec called on incompatible receiver");
		return error;
	}

	/* The string, then the match. */
	error = vm_to_string(realm, js_argument(args, count, 0), &string);
	if (error == 0)
		error = regexp_builtin_exec(realm, this_value, string, result);
	if (error != 0)
		return error;

	/* Succeeded: the result. */
	return 0;
}

/* RegExp.prototype.test(string): whether exec finds a match. */
static int
regexp_test(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	vm_value found;
	int is_object;
	int error;

	/* this must be an object. */
	is_object = vm_value_is_object(this_value);
	if (!is_object) {
		error = vm_throw_type_error(realm, "RegExp.prototype.test called on incompatible receiver");
		return error;
	}

	/* The match through exec. */
	error = vm_to_string(realm, js_argument(args, count, 0), &string);
	if (error == 0)
		error = regexp_exec_generic(realm, this_value, string, &found);
	if (error != 0)
		return error;

	/* Succeeded: whether there was one. */
	*result = vm_value_boolean(found != VM_VALUE_NULL);
	return 0;
}

/* RegExp.prototype.toString(): / source / flags. */
static int
regexp_to_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_units out;
	struct vm_string *source;
	struct vm_string *flags;
	uint16_t slash;
	int is_object;
	int error;

	/* this must be an object. */
	(void)args;
	(void)count;
	is_object = vm_value_is_object(this_value);
	if (!is_object) {
		error = vm_throw_type_error(realm, "RegExp.prototype.toString called on incompatible receiver");
		return error;
	}

	/* Its source and flags as strings. */
	error = regexp_get_string(realm, this_value, "source", &source);
	if (error == 0)
		error = regexp_get_string(realm, this_value, "flags", &flags);
	if (error != 0)
		return error;

	/* Between slashes. */
	wb_units_init(&out);
	slash = '/';
	error = wb_units_append(&out, &slash, 1);
	if (error == 0)
		error = regexp_append(&out, source, 0, source->length);
	if (error == 0)
		error = wb_units_append(&out, &slash, 1);
	if (error == 0)
		error = regexp_append(&out, flags, 0, flags->length);
	if (error == 0)
		error = regexp_units_value(realm, &out, result);
	wb_units_release(&out);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/* RegExp.prototype.compile(pattern, flags) (Annex B): this remade from a new pattern. */
static int
regexp_compile_method(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct regexp_state *state;
	struct regexp_state *other;
	vm_value pattern;
	vm_value flags;
	int error;

	/* this must be a regular expression. */
	state = regexp_state_of(this_value);
	if (state == NULL) {
		error = vm_throw_type_error(realm, "RegExp.prototype.compile called on incompatible receiver");
		return error;
	}

	/* A regular expression gives its pattern and flags (and no flags may be given with it). */
	pattern = js_argument(args, count, 0);
	flags = js_argument(args, count, 1);
	other = regexp_state_of(pattern);
	if (other != NULL) {
		if (flags != VM_VALUE_UNDEFINED) {
			error = vm_throw_type_error(realm, "Cannot supply flags when constructing one RegExp from another");
			return error;
		}

		/* The regular expression's own pattern and flags. */
		pattern = vm_value_cell(other->source);
		flags = vm_value_cell(other->flags);
	}

	/* The object remade. */
	error = regexp_initialize(realm, (struct vm_object *)vm_value_as_cell(this_value), pattern, flags);
	if (error != 0)
		return error;

	/* Succeeded: this. */
	*result = this_value;
	return 0;
}

/* get RegExp.prototype.flags: the letters of the flags that are on, in order. */
static int
regexp_get_flags(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	const struct regexp_flag *flag;
	struct vm_string *string;
	char letters[16];
	size_t length;
	vm_value value;
	int is_object;
	int on;
	int error;

	/* this must be an object. */
	(void)args;
	(void)count;
	is_object = vm_value_is_object(this_value);
	if (!is_object) {
		error = vm_throw_type_error(realm, "RegExp.prototype.flags getter called on non-object");
		return error;
	}

	/* Each flag's getter, in order. */
	length = 0;
	for (flag = regexp_flags; flag->name != NULL; flag++) {
		error = regexp_get(realm, this_value, flag->name, &value);
		if (error != 0)
			return error;
		on = vm_to_boolean(value);
		if (on) {
			letters[length] = flag->letter;
			length++;
		}
	}

	/* Succeeded: the letters. */
	string = vm_string_from_latin1(realm->heap, (const unsigned char *)letters, length);
	if (string == NULL)
		return ENOMEM;
	*result = vm_value_cell(string);
	return 0;
}

/* get RegExp.prototype.source: the pattern, escaped so that it reads back between slashes. */
static int
regexp_get_source(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct regexp_state *state;
	vm_value prototype;
	int error;

	/* RegExp.prototype's is the empty pattern; anything else but a regular expression is refused. */
	(void)args;
	(void)count;
	state = regexp_state_of(this_value);
	prototype = vm_value_cell(realm->intrinsics[VM_INTRINSIC_REGEXP_PROTOTYPE]);
	if (state == NULL) {
		if (this_value == prototype) {
			error = js_builtin_string(realm, "(?:)", result);
			return error;
		}

		/* Anything else is refused. */
		error = vm_throw_type_error(realm, "RegExp.prototype.source getter called on non-RegExp object");
		return error;
	}

	/* The escaped pattern. */
	error = regexp_escape_source(realm, state->source, result);
	if (error != 0)
		return error;

	/* Succeeded: the source. */
	return 0;
}

/* get RegExp.prototype.hasIndices. */
static int
regexp_get_has_indices(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The flag's value. */
	(void)args;
	(void)count;
	error = regexp_get_flag(realm, this_value, REGEXP_FLAG_HAS_INDICES, result);
	if (error != 0)
		return error;

	/* Succeeded: whether it is on. */
	return 0;
}

/* get RegExp.prototype.global. */
static int
regexp_get_global(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The flag's value. */
	(void)args;
	(void)count;
	error = regexp_get_flag(realm, this_value, REGEXP_FLAG_GLOBAL, result);
	if (error != 0)
		return error;

	/* Succeeded: whether it is on. */
	return 0;
}

/* get RegExp.prototype.ignoreCase. */
static int
regexp_get_ignore_case(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The flag's value. */
	(void)args;
	(void)count;
	error = regexp_get_flag(realm, this_value, REGEXP_FLAG_IGNORE_CASE, result);
	if (error != 0)
		return error;

	/* Succeeded: whether it is on. */
	return 0;
}

/* get RegExp.prototype.multiline. */
static int
regexp_get_multiline(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The flag's value. */
	(void)args;
	(void)count;
	error = regexp_get_flag(realm, this_value, REGEXP_FLAG_MULTILINE, result);
	if (error != 0)
		return error;

	/* Succeeded: whether it is on. */
	return 0;
}

/* get RegExp.prototype.dotAll. */
static int
regexp_get_dot_all(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The flag's value. */
	(void)args;
	(void)count;
	error = regexp_get_flag(realm, this_value, REGEXP_FLAG_DOT_ALL, result);
	if (error != 0)
		return error;

	/* Succeeded: whether it is on. */
	return 0;
}

/* get RegExp.prototype.unicode. */
static int
regexp_get_unicode(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The flag's value. */
	(void)args;
	(void)count;
	error = regexp_get_flag(realm, this_value, REGEXP_FLAG_UNICODE, result);
	if (error != 0)
		return error;

	/* Succeeded: whether it is on. */
	return 0;
}

/* get RegExp.prototype.unicodeSets. */
static int
regexp_get_unicode_sets(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The flag's value. */
	(void)args;
	(void)count;
	error = regexp_get_flag(realm, this_value, REGEXP_FLAG_UNICODE_SETS, result);
	if (error != 0)
		return error;

	/* Succeeded: whether it is on. */
	return 0;
}

/* get RegExp.prototype.sticky. */
static int
regexp_get_sticky(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The flag's value. */
	(void)args;
	(void)count;
	error = regexp_get_flag(realm, this_value, REGEXP_FLAG_STICKY, result);
	if (error != 0)
		return error;

	/* Succeeded: whether it is on. */
	return 0;
}

/*
 * Reads one flag of a regular expression for its getter: whether it is
 * on (undefined on RegExp.prototype itself; anything else is refused).
 */
static int
regexp_get_flag(
	struct vm_realm *realm,
	vm_value this_value,
	unsigned index,
	vm_value *result)
{
	const struct regexp_flag *flag;
	struct regexp_state *state;
	vm_value prototype;
	char message[96];
	unsigned flags;
	int error;

	/* A regular expression's flag; RegExp.prototype's is undefined; anything else is refused. */
	flag = &regexp_flags[index];
	state = regexp_state_of(this_value);
	prototype = vm_value_cell(realm->intrinsics[VM_INTRINSIC_REGEXP_PROTOTYPE]);
	if (state == NULL) {
		if (this_value == prototype) {
			*result = VM_VALUE_UNDEFINED;
			return 0;
		}

		/* Anything else is refused. */
		snprintf(message, sizeof(message), "RegExp.prototype.%s getter called on non-RegExp object", flag->name);
		error = vm_throw_type_error(realm, message);
		return error;
	}

	/* Succeeded: whether the flag is on. */
	flags = js_regexp_flags(state->program);
	*result = vm_value_boolean((flags & flag->flag) != 0);
	return 0;
}

/* Finds a regular expression's state; NULL for anything else. */
static struct regexp_state *
regexp_state_of(
	vm_value value)
{
	struct vm_object *object;
	int is_regexp;
	int initialized;

	/* An initialized RegExp object. */
	is_regexp = js_regexp_is(value);
	if (!is_regexp)
		return NULL;
	object = (struct vm_object *)vm_value_as_cell(value);
	initialized = vm_value_is_cell(object->internal);
	if (!initialized)
		return NULL;

	/* Succeeded: its state. */
	return (struct regexp_state *)vm_value_as_cell(object->internal);
}

/*
 * Makes a regular expression with new.target's prototype: the pattern (a
 * regular expression gives its own, and its flags unless flags are given)
 * compiled with the flags, lastIndex 0.
 */
static int
regexp_make(
	struct vm_realm *realm,
	vm_value new_target,
	vm_value pattern,
	vm_value flags,
	vm_value *result)
{
	struct regexp_state *other;
	struct vm_object *prototype;
	struct vm_object *object;
	vm_value key;
	int error;

	/* A regular expression's own pattern and flags. */
	other = regexp_state_of(pattern);
	if (other != NULL) {
		if (flags == VM_VALUE_UNDEFINED)
			flags = vm_value_cell(other->flags);
		pattern = vm_value_cell(other->source);
	}

	/* The object, with lastIndex (writable, not enumerable, not configurable). */
	error = vm_construct_prototype(realm, new_target, realm->intrinsics[VM_INTRINSIC_REGEXP_PROTOTYPE], &prototype);
	if (error != 0)
		return error;
	object = vm_object_create(realm->heap, prototype);
	if (object == NULL)
		return ENOMEM;
	object->kind = VM_KIND_REGEXP;
	object->internal = VM_VALUE_UNDEFINED;
	key = vm_key_from_ascii(realm->heap, "lastIndex");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	error = vm_object_define(realm->heap, object, key, vm_value_int32(0), VM_PROPERTY_WRITABLE);
	if (error != 0)
		return error;

	/* The pattern compiled. */
	error = regexp_initialize(realm, object, pattern, flags);
	if (error != 0)
		return error;

	/* Succeeded: the regular expression. */
	*result = vm_value_cell(object);
	return 0;
}

/*
 * Compiles a pattern and flags (undefined is empty) into an object's
 * state and sets its lastIndex to 0; a bad pattern or bad flags are a
 * SyntaxError.
 */
static int
regexp_initialize(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value pattern,
	vm_value flags)
{
	struct js_regexp_program *program;
	struct regexp_state *state;
	struct vm_string *source;
	struct vm_string *flags_string;
	struct wb_units pattern_units;
	struct wb_units flags_units;
	struct wb_buffer text;
	const char *message;
	const char *shown;
	char description[200];
	unsigned flag_bits;
	int error;

	/* The pattern and the flags as strings. */
	source = vm_string_from_latin1(realm->heap, NULL, 0);
	flags_string = source;
	if (source == NULL)
		return ENOMEM;
	if (pattern != VM_VALUE_UNDEFINED) {
		error = vm_to_string(realm, pattern, &source);
		if (error != 0)
			return error;
	}

	/* undefined flags are none. */
	if (flags != VM_VALUE_UNDEFINED) {
		error = vm_to_string(realm, flags, &flags_string);
		if (error != 0)
			return error;
	}

	/* The flags. */
	wb_units_init(&flags_units);
	error = vm_string_append_units(flags_string, &flags_units);
	if (error == 0)
		error = js_regexp_parse_flags(flags_units.data, flags_units.length, &flag_bits);
	wb_units_release(&flags_units);
	if (error == ENOMEM)
		return error;
	if (error != 0) {
		wb_buffer_init(&text);
		vm_string_to_utf8(flags_string, &text);
		shown = "";
		if (text.data != NULL)
			shown = (const char *)text.data;
		snprintf(description, sizeof(description), "Invalid flags supplied to RegExp constructor '%s'", shown);
		wb_buffer_release(&text);
		error = vm_throw_error(realm, VM_ERROR_SYNTAX, description);
		return error;
	}

	/* The pattern compiled. */
	wb_units_init(&pattern_units);
	error = vm_string_append_units(source, &pattern_units);
	if (error == 0)
		error = js_regexp_compile(pattern_units.data, pattern_units.length, flag_bits, &program, &message);
	wb_units_release(&pattern_units);
	if (error == ENOMEM)
		return error;
	if (error != 0) {
		wb_buffer_init(&text);
		vm_string_to_utf8(source, &text);
		shown = "";
		if (text.data != NULL)
			shown = (const char *)text.data;
		if (message == NULL)
			message = "syntax error";
		snprintf(description, sizeof(description), "Invalid regular expression: /%.120s/: %s", shown, message);
		wb_buffer_release(&text);
		error = vm_throw_error(realm, VM_ERROR_SYNTAX, description);
		return error;
	}

	/* The state holds the program (freed with the cell). */
	state = vm_heap_alloc(realm->heap, &regexp_state_type, sizeof(*state));
	if (state == NULL) {
		js_regexp_free(program);
		return ENOMEM;
	}

	/* The state keeps the pattern and flags as given. */
	state->source = source;
	state->flags = flags_string;
	state->program = program;
	object->internal = vm_value_cell(state);

	/* lastIndex starts at 0. */
	error = regexp_set(realm, vm_value_cell(object), "lastIndex", vm_value_int32(0));
	if (error != 0)
		return error;

	/* Succeeded: the object holds the compiled pattern. */
	return 0;
}

/*
 * Runs a regular expression's own matcher (RegExpBuiltinExec): from
 * lastIndex with g or y (which the match then moves, or resets when there
 * is none), from 0 otherwise; the result array, or null.
 */
static int
regexp_builtin_exec(
	struct vm_realm *realm,
	vm_value regexp,
	struct vm_string *string,
	vm_value *result)
{
	struct regexp_state *state;
	struct js_regexp_input input;
	size_t *captures;
	vm_value value;
	double last_index;
	unsigned flags;
	int moves;
	int matched;
	int error;

	/* lastIndex, used only with g or y. */
	state = regexp_state_of(regexp);
	error = regexp_get(realm, regexp, "lastIndex", &value);
	if (error == 0)
		error = regexp_to_length(realm, value, &last_index);
	if (error != 0)
		return error;
	flags = js_regexp_flags(state->program);
	moves = 0;
	if ((flags & (JS_REGEXP_GLOBAL | JS_REGEXP_STICKY)) != 0)
		moves = 1;
	if (!moves)
		last_index = 0.0;

	/* Past the end nothing matches. */
	if (last_index > (double)string->length) {
		if (moves) {
			error = regexp_set(realm, regexp, "lastIndex", vm_value_int32(0));
			if (error != 0)
				return error;
		}

		/* No match. */
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* The match. */
	captures = malloc((size_t)js_regexp_capture_count(state->program) * 2U * sizeof(*captures));
	if (captures == NULL)
		return ENOMEM;
	regexp_input(string, &input);
	input.poll = regexp_poll;
	input.poll_context = realm->heap;
	error = js_regexp_match(state->program, &input, (size_t)last_index, captures, &matched);
	if (error == E2BIG) {
		free(captures);
		error = vm_throw_range_error(realm, "Maximum call stack size exceeded in a regular expression");
		return error;
	}

	/* Out of memory, or the task was asked to end. */
	if (error != 0) {
		free(captures);
		return error;
	}

	/* No match: lastIndex back to 0 with g or y. */
	if (!matched) {
		free(captures);
		if (moves) {
			error = regexp_set(realm, regexp, "lastIndex", vm_value_int32(0));
			if (error != 0)
				return error;
		}

		/* No match. */
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* A match: lastIndex after it with g or y, and the result array. */
	error = 0;
	if (moves)
		error = regexp_set(realm, regexp, "lastIndex", vm_value_number((double)captures[1]));
	if (error == 0)
		error = regexp_result(realm, state->program, string, captures, result);
	free(captures);
	if (error != 0)
		return error;

	/* Succeeded: the result. */
	return 0;
}

/*
 * Makes a match's result array: the matched text and each capture (or
 * undefined), index, input, groups (an object without a prototype of the
 * named captures, or undefined), and under d the indices of each.
 */
static int
regexp_result(
	struct vm_realm *realm,
	const struct js_regexp_program *program,
	struct vm_string *string,
	const size_t *captures,
	vm_value *result)
{
	struct vm_object *array;
	struct vm_object *groups;
	struct vm_object *index_groups;
	struct vm_string *name_string;
	const uint16_t *name;
	vm_value pair[2];
	vm_value array_value;
	vm_value indices;
	vm_value value;
	vm_value key;
	vm_value group_value;
	vm_value index_groups_value;
	size_t name_length;
	uint32_t count;
	uint32_t capture;
	int named;
	int has_indices;
	int error;

	/* The array of the match and the captures. */
	error = js_builtin_array(realm, NULL, 0, &array_value);
	if (error != 0)
		return error;
	array = (struct vm_object *)vm_value_as_cell(array_value);
	count = js_regexp_capture_count(program);
	for (capture = 0; capture < count; capture++) {
		value = VM_VALUE_UNDEFINED;
		if (captures[capture * 2U] != JS_REGEXP_UNSET) {
			error = regexp_slice(realm, string, captures[capture * 2U], captures[capture * 2U + 1U], &value);
			if (error != 0)
				return error;
		}

		/* The capture joins the array. */
		error = regexp_push_value(realm, array_value, value);
		if (error != 0)
			return error;
	}

	/* index and input. */
	error = js_builtin_value(realm, array, "index", vm_value_number((double)captures[0]), VM_PROPERTY_DEFAULT);
	if (error == 0)
		error = js_builtin_value(realm, array, "input", vm_value_cell(string), VM_PROPERTY_DEFAULT);
	if (error != 0)
		return error;

	/* groups: the named captures' values by name. */
	named = js_regexp_has_names(program);
	group_value = VM_VALUE_UNDEFINED;
	groups = NULL;
	if (named) {
		groups = vm_object_create(realm->heap, NULL);
		if (groups == NULL)
			return ENOMEM;
		group_value = vm_value_cell(groups);
		for (capture = 1; capture < count; capture++) {
			named = js_regexp_capture_name(program, capture, &name, &name_length);
			if (!named)
				continue;
			name_string = vm_string_from_units(realm->heap, name, name_length);
			if (name_string == NULL)
				return ENOMEM;
			error = vm_key_from_string(realm->heap, name_string, &key);
			if (error == 0)
				error = vm_object_get(array, vm_value_int32((int32_t)capture), &value);
			if (error == 0)
				error = vm_object_define(realm->heap, groups, key, value, VM_PROPERTY_DEFAULT);
			if (error != 0)
				return error;
		}
	}

	/* The groups property (undefined without named captures). */
	error = js_builtin_value(realm, array, "groups", group_value, VM_PROPERTY_DEFAULT);
	if (error != 0)
		return error;

	/* Under d, indices: [start, end] of each capture (or undefined), with their groups. */
	has_indices = (js_regexp_flags(program) & JS_REGEXP_HAS_INDICES) != 0;
	if (has_indices) {
		error = js_builtin_array(realm, NULL, 0, &indices);
		if (error != 0)
			return error;
		index_groups_value = VM_VALUE_UNDEFINED;
		index_groups = NULL;
		if (groups != NULL) {
			index_groups = vm_object_create(realm->heap, NULL);
			if (index_groups == NULL)
				return ENOMEM;
			index_groups_value = vm_value_cell(index_groups);
		}

		/* Each capture's pair. */
		for (capture = 0; capture < count; capture++) {
			value = VM_VALUE_UNDEFINED;
			if (captures[capture * 2U] != JS_REGEXP_UNSET) {
				pair[0] = vm_value_number((double)captures[capture * 2U]);
				pair[1] = vm_value_number((double)captures[capture * 2U + 1U]);
				error = js_builtin_array(realm, pair, 2, &value);
				if (error != 0)
					return error;
			}

			/* The pair joins the indices. */
			error = regexp_push_value(realm, indices, value);
			if (error != 0)
				return error;
			named = 0;
			if (index_groups != NULL)
				named = js_regexp_capture_name(program, capture, &name, &name_length);
			if (named) {
				name_string = vm_string_from_units(realm->heap, name, name_length);
				if (name_string == NULL)
					return ENOMEM;
				error = vm_key_from_string(realm->heap, name_string, &key);
				if (error == 0)
					error = vm_object_define(realm->heap, index_groups, key, value, VM_PROPERTY_DEFAULT);
				if (error != 0)
					return error;
			}
		}

		/* The indices' groups, then the indices. */
		error = js_builtin_value(realm, (struct vm_object *)vm_value_as_cell(indices), "groups", index_groups_value,
		    VM_PROPERTY_DEFAULT);
		if (error == 0)
			error = js_builtin_value(realm, array, "indices", indices, VM_PROPERTY_DEFAULT);
		if (error != 0)
			return error;
	}

	/* Succeeded: the result. */
	*result = array_value;
	return 0;
}

/*
 * Runs a regular expression's exec (RegExpExec): its exec property when
 * that is callable (whose result must be an object or null), the built-in
 * matcher otherwise.
 */
static int
regexp_exec_generic(
	struct vm_realm *realm,
	vm_value regexp,
	struct vm_string *string,
	vm_value *result)
{
	struct regexp_state *state;
	vm_value exec;
	vm_value argument;
	int callable;
	int is_object;
	int error;

	/* The exec property. */
	error = regexp_get(realm, regexp, "exec", &exec);
	if (error != 0)
		return error;
	callable = vm_value_is_callable(exec);
	if (callable) {
		argument = vm_value_cell(string);
		error = vm_call(realm, exec, regexp, &argument, 1, result);
		if (error != 0)
			return error;
		is_object = vm_value_is_object(*result);
		if (!is_object && *result != VM_VALUE_NULL) {
			error = vm_throw_type_error(realm, "exec must return an object or null");
			return error;
		}

		/* Succeeded: the script's exec's result. */
		return 0;
	}

	/* Without one, only a regular expression can match. */
	state = regexp_state_of(regexp);
	if (state == NULL) {
		error = vm_throw_type_error(realm, "RegExp exec method called on incompatible receiver");
		return error;
	}

	/* The built-in matcher. */
	error = regexp_builtin_exec(realm, regexp, string, result);
	if (error != 0)
		return error;

	/* Succeeded: the result. */
	return 0;
}

/* Reads a regular expression's flags through its flags property. */
static int
regexp_flags_text(
	struct vm_realm *realm,
	vm_value regexp,
	unsigned *flags)
{
	struct vm_string *text;
	size_t index;
	uint16_t unit;
	int error;

	/* The flags property as a string. */
	error = regexp_get_string(realm, regexp, "flags", &text);
	if (error != 0)
		return error;

	/* The flags this code needs to know about. */
	*flags = 0;
	for (index = 0; index < text->length; index++) {
		unit = vm_string_at(text, index);
		if (unit == 'g')
			*flags |= JS_REGEXP_GLOBAL;
		else if (unit == 'u')
			*flags |= JS_REGEXP_UNICODE;
		else if (unit == 'v')
			*flags |= JS_REGEXP_UNICODE_SETS;
		else if (unit == 'y')
			*flags |= JS_REGEXP_STICKY;
	}

	/* Succeeded: the flags. */
	return 0;
}

/* Reads a property by an ASCII name. */
static int
regexp_get(
	struct vm_realm *realm,
	vm_value object,
	const char *name,
	vm_value *value)
{
	vm_value key;
	int error;

	/* The key, then the property. */
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	error = vm_get(realm, object, key, value);
	if (error != 0)
		return error;

	/* Succeeded: the value. */
	return 0;
}

/* Sets a property by an ASCII name, throwing when it cannot be set. */
static int
regexp_set(
	struct vm_realm *realm,
	vm_value object,
	const char *name,
	vm_value value)
{
	vm_value key;
	int error;

	/* The key, then the property (as strict code does). */
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	error = vm_set(realm, object, key, value, 1);
	if (error != 0)
		return error;

	/* Succeeded: the property is set. */
	return 0;
}

/* Converts a value to a length (ToLength: an integer from 0 to 2^53 - 1). */
static int
regexp_to_length(
	struct vm_realm *realm,
	vm_value value,
	double *length)
{
	double integer;
	int error;

	/* The integer, clamped. */
	error = js_builtin_integer(realm, value, &integer);
	if (error != 0)
		return error;
	if (integer < 0.0)
		integer = 0.0;
	if (integer > REGEXP_LENGTH_MAX)
		integer = REGEXP_LENGTH_MAX;

	/* Succeeded: the length. */
	*length = integer;
	return 0;
}

/* Reads a property by an ASCII name as a string. */
static int
regexp_get_string(
	struct vm_realm *realm,
	vm_value object,
	const char *name,
	struct vm_string **string)
{
	vm_value value;
	int error;

	/* The property, then ToString. */
	error = regexp_get(realm, object, name, &value);
	if (error == 0)
		error = vm_to_string(realm, value, string);
	if (error != 0)
		return error;

	/* Succeeded: the string. */
	return 0;
}

/* Reads a match result's matched text (its element 0) as a string. */
static int
regexp_get_match(
	struct vm_realm *realm,
	vm_value result,
	struct vm_string **string)
{
	vm_value value;
	int error;

	/* The element, then ToString. */
	error = vm_get(realm, result, vm_value_int32(0), &value);
	if (error == 0)
		error = vm_to_string(realm, value, string);
	if (error != 0)
		return error;

	/* Succeeded: the string. */
	return 0;
}

/* Describes a string's characters to the matcher. */
static void
regexp_input(
	const struct vm_string *string,
	struct js_regexp_input *input)
{
	/* UTF-16 or Latin-1, and no poll until the caller gives one. */
	input->units = NULL;
	input->latin1 = NULL;
	input->length = string->length;
	input->poll = NULL;
	input->poll_context = NULL;
	if ((string->flags & VM_STRING_WIDE) != 0) {
		input->units = vm_string_units(string);
	} else {
		input->latin1 = vm_string_latin1(string);
	}
}

/* Tells the matcher whether its match goes on: VM_TERMINATED when the heap's (context) task was asked to end. */
static int
regexp_poll(
	void *context)
{
	struct vm_heap *heap;
	int status;

	/* The heap's look. */
	heap = context;
	status = vm_heap_check_termination(heap);
	if (status != 0)
		return status;

	/* Succeeded: the match goes on. */
	return 0;
}

/* Makes the string of a range of a string's units. */
static int
regexp_slice(
	struct vm_realm *realm,
	const struct vm_string *string,
	size_t start,
	size_t end,
	vm_value *result)
{
	struct vm_string *made;

	/* The range in the string's own form. */
	if (end < start)
		end = start;
	if ((string->flags & VM_STRING_WIDE) != 0) {
		made = vm_string_from_units(realm->heap, vm_string_units(string) + start, end - start);
	} else {
		made = vm_string_from_latin1(realm->heap, vm_string_latin1(string) + start, end - start);
	}

	/* Out of memory. */
	if (made == NULL)
		return ENOMEM;

	/* Succeeded: the string. */
	*result = vm_value_cell(made);
	return 0;
}

/* Moves an index on by one character (AdvanceStringIndex): a surrogate pair is one under u. */
static size_t
regexp_advance(
	const struct vm_string *string,
	size_t index,
	int unicode)
{
	uint16_t lead;
	uint16_t trail;

	/* One unit without u, or at the end. */
	if (!unicode || index + 1U >= string->length)
		return index + 1U;

	/* A pair is two units. */
	lead = vm_string_at(string, index);
	trail = vm_string_at(string, index + 1U);
	if (lead >= 0xD800U && lead <= 0xDBFFU && trail >= 0xDC00U && trail <= 0xDFFFU)
		return index + 2U;

	/* Anything else is one. */
	return index + 1U;
}

/* Appends a range of a string's units (end clamped to its length). */
static int
regexp_append(
	struct wb_units *units,
	const struct vm_string *string,
	size_t start,
	size_t end)
{
	size_t index;
	uint16_t unit;
	int error;

	/* The range, clamped. */
	if (end > string->length)
		end = string->length;
	if (start >= end)
		return 0;

	/* UTF-16 at once; Latin-1 unit by unit. */
	if ((string->flags & VM_STRING_WIDE) != 0) {
		error = wb_units_append(units, vm_string_units(string) + start, end - start);
		return error;
	}

	/* Latin-1 units widen one by one. */
	error = wb_units_reserve(units, end - start);
	for (index = start; error == 0 && index < end; index++) {
		unit = vm_string_latin1(string)[index];
		error = wb_units_append(units, &unit, 1);
	}

	/* A failure to append. */
	if (error != 0)
		return error;

	/* Succeeded: the units are appended. */
	return 0;
}

/* Appends a value to an array at its end. */
static int
regexp_push_value(
	struct vm_realm *realm,
	vm_value array,
	vm_value value)
{
	struct vm_object *object;
	int error;

	/* The next index. */
	object = (struct vm_object *)vm_value_as_cell(array);
	error = vm_object_define(realm->heap, object, vm_value_int32((int32_t)object->length), value, VM_PROPERTY_DEFAULT);
	if (error != 0)
		return error;

	/* Succeeded: the value is at the end. */
	return 0;
}

/* Makes a string value of a list of units. */
static int
regexp_units_value(
	struct vm_realm *realm,
	const struct wb_units *units,
	vm_value *result)
{
	struct vm_string *string;

	/* The string (an empty list is the empty string). */
	string = vm_string_from_units(realm->heap, units->data, units->length);
	if (string == NULL)
		return ENOMEM;

	/* Succeeded: its value. */
	*result = vm_value_cell(string);
	return 0;
}

/*
 * Escapes a pattern for source (EscapeRegExpPattern): the empty pattern
 * is (?:), a / outside a class gets a \, line terminators are escapes.
 */
static int
regexp_escape_source(
	struct vm_realm *realm,
	const struct vm_string *source,
	vm_value *result)
{
	struct wb_units out;
	const char *escape;
	size_t index;
	uint16_t unit;
	uint16_t next;
	int in_class;
	int error;

	/* The empty pattern. */
	if (source->length == 0) {
		error = js_builtin_string(realm, "(?:)", result);
		return error;
	}

	/* Each unit, escaped where a slash or a line would end the literal. */
	wb_units_init(&out);
	in_class = 0;
	error = 0;
	for (index = 0; index < source->length && error == 0; index++) {
		unit = vm_string_at(source, index);
		escape = NULL;
		if (unit == '\\' && index + 1U < source->length) {
			/* An escape is copied whole (an escaped line terminator as its escape). */
			next = vm_string_at(source, index + 1U);
			index++;
			if (next == 0x0AU)
				escape = "\\n";
			else if (next == 0x0DU)
				escape = "\\r";
			else if (next == 0x2028U)
				escape = "\\u2028";
			else if (next == 0x2029U)
				escape = "\\u2029";
			if (escape == NULL) {
				error = wb_units_append(&out, &unit, 1);
				if (error == 0)
					error = wb_units_append(&out, &next, 1);
				continue;
			}
		} else if (unit == '/' && !in_class) {
			escape = "\\/";
		} else if (unit == 0x0AU) {
			escape = "\\n";
		} else if (unit == 0x0DU) {
			escape = "\\r";
		} else if (unit == 0x2028U) {
			escape = "\\u2028";
		} else if (unit == 0x2029U) {
			escape = "\\u2029";
		} else if (unit == '[') {
			in_class = 1;
		} else if (unit == ']') {
			in_class = 0;
		}

		/* The unit, or its escape. */
		if (escape == NULL) {
			error = wb_units_append(&out, &unit, 1);
			continue;
		}

		/* The escape's characters. */
		for (; *escape != '\0' && error == 0; escape++) {
			next = (uint16_t)*escape;
			error = wb_units_append(&out, &next, 1);
		}
	}

	/* The string. */
	if (error == 0)
		error = regexp_units_value(realm, &out, result);
	wb_units_release(&out);
	if (error != 0)
		return error;

	/* Succeeded: the source. */
	return 0;
}

/* Defines a method of RegExp.prototype keyed by a well-known symbol. */
static int
regexp_symbol_method(
	struct vm_realm *realm,
	struct vm_object *prototype,
	int which,
	const char *name,
	unsigned length,
	vm_native native)
{
	struct vm_function *function;
	int error;

	/* The function. */
	error = js_builtin_function(realm, name, length, native, NULL, &function);
	if (error != 0)
		return error;

	/* The property. */
	error = js_builtin_symbol_value(realm, prototype, which, vm_value_cell(function), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* Succeeded: the method is defined. */
	return 0;
}

/* Checks a symbol method's this (an object) and makes its first argument a string. */
static int
regexp_symbol_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	struct vm_string **string)
{
	int is_object;
	int status;

	/* this must be an object. */
	is_object = vm_value_is_object(this_value);
	if (!is_object) {
		status = vm_throw_type_error(realm, "RegExp.prototype method called on incompatible receiver");
		return status;
	}

	/* The string. */
	status = vm_to_string(realm, js_argument(args, count, 0), string);
	if (status != 0)
		return status;

	/* Succeeded: the string. */
	return 0;
}

/* RegExp.prototype[Symbol.match](string). */
static int
regexp_symbol_match_method(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	int status;

	/* The string, then the matches. */
	*result = VM_VALUE_UNDEFINED;
	status = regexp_symbol_string(realm, this_value, args, count, &string);
	if (status == 0)
		status = js_regexp_symbol_match(realm, this_value, string, result);
	if (status != 0)
		return status;

	/* Succeeded: the result. */
	return 0;
}

/* RegExp.prototype[Symbol.replace](string, replacement). */
static int
regexp_symbol_replace_method(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	int status;

	/* The string, then the replacement. */
	*result = VM_VALUE_UNDEFINED;
	status = regexp_symbol_string(realm, this_value, args, count, &string);
	if (status == 0)
		status = js_regexp_symbol_replace(realm, this_value, string, js_argument(args, count, 1), result);
	if (status != 0)
		return status;

	/* Succeeded: the result. */
	return 0;
}

/* RegExp.prototype[Symbol.search](string). */
static int
regexp_symbol_search_method(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	int status;

	/* The string, then the search. */
	*result = VM_VALUE_UNDEFINED;
	status = regexp_symbol_string(realm, this_value, args, count, &string);
	if (status == 0)
		status = js_regexp_symbol_search(realm, this_value, string, result);
	if (status != 0)
		return status;

	/* Succeeded: the index. */
	return 0;
}

/* RegExp.prototype[Symbol.split](string, limit). */
static int
regexp_symbol_split_method(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	int status;

	/* The string, then the parts. */
	*result = VM_VALUE_UNDEFINED;
	status = regexp_symbol_string(realm, this_value, args, count, &string);
	if (status == 0)
		status = js_regexp_symbol_split(realm, this_value, string, js_argument(args, count, 1), result);
	if (status != 0)
		return status;

	/* Succeeded: the parts. */
	return 0;
}
