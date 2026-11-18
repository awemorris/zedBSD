/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Array: the constructor, Array.isArray, Array.of, Array.from and
 * Array.prototype's methods, written as the generic algorithms of the
 * language (they work on any object with a length) over Get, Set,
 * HasProperty and DeleteProperty.
 */

#include "js/builtin.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * One built-in method: its name, its length and what it does.  The table
 * is constant for the life of the program.
 */
struct array_entry {
	const char *name;
	unsigned length;
	vm_native native;
};

/* What a search method looks for. */
#define ARRAY_FIND_VALUE	0
#define ARRAY_FIND_INDEX	1

/* What a callback loop does with the callback's answers. */
#define ARRAY_EACH		0
#define ARRAY_EVERY		1
#define ARRAY_SOME		2
#define ARRAY_MAP		3
#define ARRAY_FILTER		4

static int array_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_is_array(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_of(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_from(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_at(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_concat(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_copy_within(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_every(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_fill(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_filter(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_find(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_find_index(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_find_last(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_find_last_index(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_flat(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_flat_map(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_for_each(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_includes(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_index_of(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_join(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_last_index_of(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_map(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_pop(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_push(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_reduce(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_reduce_right(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_reverse(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_shift(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_slice(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_some(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_sort(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_splice(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_to_locale_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_to_reversed(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_to_sorted(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_to_spliced(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_to_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_unshift(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_with(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int array_this(struct vm_realm *realm, vm_value this_value, vm_value *object, uint32_t *length);
static int array_has(struct vm_realm *realm, vm_value object, uint32_t index, int *found);
static int array_get(struct vm_realm *realm, vm_value object, uint32_t index, vm_value *value);
static int array_set(struct vm_realm *realm, vm_value object, uint32_t index, vm_value value);
static int array_remove(struct vm_realm *realm, vm_value object, uint32_t index);
static int array_set_length(struct vm_realm *realm, vm_value object, double length);
static int array_new(struct vm_realm *realm, vm_value *array);
static int array_append(struct vm_realm *realm, vm_value array, vm_value value);
static int array_callback(struct vm_realm *realm, const vm_value *args, unsigned count, vm_value *callback);
static int array_each(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, int mode, vm_value *result);
static int array_search(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, int backwards, int what, vm_value *result);
static int array_relative(struct vm_realm *realm, vm_value value, uint32_t length, uint32_t fallback, uint32_t *index);
static int array_compare(struct vm_realm *realm, vm_value comparator, vm_value left, vm_value right, int *order);
static int array_merge_sort(struct vm_realm *realm, vm_value comparator, vm_value *items, vm_value *scratch, uint32_t count);
static int array_sorted_items(struct vm_realm *realm, vm_value object, uint32_t length, vm_value comparator, int skip_holes, vm_value *list, uint32_t *count);
static int array_join_with(struct vm_realm *realm, vm_value object, uint32_t length, struct vm_string *separator, int locale, vm_value *result);

/*
 * The functions of the Array constructor.
 */
static const struct array_entry array_statics[] = {
	{ "from", 1, array_from },
	{ "isArray", 1, array_is_array },
	{ "of", 0, array_of },
	{ NULL, 0, NULL }
};

/*
 * The methods of Array.prototype.
 */
static const struct array_entry array_methods[] = {
	{ "at", 1, array_at },
	{ "concat", 1, array_concat },
	{ "copyWithin", 2, array_copy_within },
	{ "every", 1, array_every },
	{ "fill", 1, array_fill },
	{ "filter", 1, array_filter },
	{ "find", 1, array_find },
	{ "findIndex", 1, array_find_index },
	{ "findLast", 1, array_find_last },
	{ "findLastIndex", 1, array_find_last_index },
	{ "flat", 0, array_flat },
	{ "flatMap", 1, array_flat_map },
	{ "forEach", 1, array_for_each },
	{ "includes", 1, array_includes },
	{ "indexOf", 1, array_index_of },
	{ "join", 1, array_join },
	{ "lastIndexOf", 1, array_last_index_of },
	{ "map", 1, array_map },
	{ "pop", 0, array_pop },
	{ "push", 1, array_push },
	{ "reduce", 1, array_reduce },
	{ "reduceRight", 1, array_reduce_right },
	{ "reverse", 0, array_reverse },
	{ "shift", 0, array_shift },
	{ "slice", 2, array_slice },
	{ "some", 1, array_some },
	{ "sort", 1, array_sort },
	{ "splice", 2, array_splice },
	{ "toLocaleString", 0, array_to_locale_string },
	{ "toReversed", 0, array_to_reversed },
	{ "toSorted", 1, array_to_sorted },
	{ "toSpliced", 2, array_to_spliced },
	{ "toString", 0, array_to_string },
	{ "unshift", 1, array_unshift },
	{ "with", 2, array_with },
	{ NULL, 0, NULL }
};

static int array_flatten(struct vm_realm *realm, vm_value target, vm_value source, uint32_t source_length, double depth, vm_value mapper, vm_value this_arg);
static int array_is_concat_spreadable(struct vm_realm *realm, vm_value value, int *spreadable);

/*
 * Installs Array.
 */
int
js_builtin_install_array(
	struct vm_realm *realm)
{
	const struct array_entry *entry;
	struct vm_function *constructor;
	int error;

	/* The constructor over the realm's Array.prototype (called or constructed alike). */
	error = js_builtin_constructor(realm, "Array", 1, array_call, array_call, realm->array_prototype, &constructor);
	if (error != 0)
		return error;

	/* Its functions, then the prototype's methods. */
	for (entry = array_statics; entry->name != NULL; entry++) {
		error = js_builtin_method(realm, &constructor->object, entry->name, entry->length, entry->native);
		if (error != 0)
			return error;
	}

	/* The prototype's methods. */
	for (entry = array_methods; entry->name != NULL; entry++) {
		error = js_builtin_method(realm, realm->array_prototype, entry->name, entry->length, entry->native);
		if (error != 0)
			return error;
	}

	/* Succeeded: Array is installed. */
	return 0;
}

/* Array(...items) or Array(length), with or without new. */
static int
array_call(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *prototype;
	struct vm_object *made;
	struct vm_cell *prototype_root;
	struct vm_cell *made_root;
	vm_value new_target;
	double number;
	uint32_t length;
	unsigned index;
	int is_number;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The array, from new.target's prototype. */
	new_target = realm->new_target;
	prototype = realm->array_prototype;
	if (new_target != VM_VALUE_UNDEFINED) {
		status = vm_construct_prototype(realm, new_target, realm->array_prototype, &prototype);
		if (status != 0)
			return status;
	}

	/* A getter may have supplied a fresh prototype for this construction. */
	prototype_root = &prototype->cell;
	status = vm_heap_add_root(realm->heap, &prototype_root);
	if (status != 0)
		return status;

	/* The array. */
	made = vm_array_create(realm->heap, prototype);
	vm_heap_remove_root(realm->heap, &prototype_root);
	if (made == NULL)
		return ENOMEM;
	*result = vm_value_cell(made);
	made_root = &made->cell;
	status = vm_heap_add_root(realm->heap, &made_root);
	if (status != 0)
		return status;

	/* One number is a length. */
	is_number = 0;
	if (count == 1U)
		is_number = vm_value_is_number(args[0]);

	/* Converts a numeric length through the existing safe unsigned conversion. */
	if (is_number) {
		number = vm_value_as_number(args[0]);
		status = vm_to_uint32(realm, args[0], &length);
		if (status != 0)
			goto cleanup;

		/* Fractional, non-finite and out-of-range numbers cannot be array lengths. */
		if ((double)length != number) {
			status = vm_throw_range_error(realm, "Invalid array length");
			goto cleanup;
		}

		/* The length. */
		status = vm_array_set_length(realm->heap, made, length);
		if (status != 0)
			goto cleanup;

		/* Succeeded: only the length metadata grew; no holes were allocated. */
		goto cleanup;
	}

	/* Anything else is the elements. */
	for (index = 0; index < count; index++) {
		status = vm_object_define(realm->heap, made, vm_value_int32((int32_t)index), args[index], VM_PROPERTY_DEFAULT);
		if (status != 0)
			goto cleanup;
	}

	/* The constructed array is now handed to the caller. */
cleanup:
	vm_heap_remove_root(realm->heap, &made_root);
	return status;
}

/* Array.isArray(value). */
static int
array_is_array(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int is_array;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Native Proxy targets preserve array identity and revoked records throw. */
	status = vm_is_array(realm, js_argument(args, count, 0), &is_array);
	if (status != 0)
		return status;

	/* Succeeded: publish only the completed abstract operation's Boolean. */
	*result = vm_value_boolean(is_array);
	return 0;
}

/* Array.of(...items). */
static int
array_of(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The items as an array. */
	status = js_builtin_array(realm, args, count, result);
	if (status != 0)
		return status;
	return 0;
}

/* Array.from(arrayLike, mapper, thisArg): the elements of an array-like object (iterables come later). */
static int
array_from(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *result_root;
	struct vm_cell *source_root;
	vm_value source;
	vm_value mapper;
	vm_value value;
	vm_value call_args[2];
	uint32_t length;
	uint32_t index;
	int callable;
	int iterable;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The mapper, when given, must be a function. */
	mapper = js_argument(args, count, 1);
	callable = vm_value_is_callable(mapper);
	if (mapper != VM_VALUE_UNDEFINED && !callable) {
		status = vm_throw_type_error(realm, "Array.from: the mapper is not a function");
		return status;
	}

	/* The source as an object, and its length. */
	status = vm_to_object(realm, js_argument(args, count, 0), &source);
	if (status != 0)
		return status;
	source_root = vm_value_as_cell(source);
	status = vm_heap_add_root(realm->heap, &source_root);
	if (status != 0)
		return status;
	status = vm_get_method(realm, source, vm_symbol_key(realm, VM_SYMBOL_ITERATOR), &value);
	if (status != 0)
		goto release_source;
	iterable = value != VM_VALUE_UNDEFINED;
	length = 0;
	if (!iterable) {
		status = js_builtin_length(realm, source, &length);
		if (status != 0)
			goto release_source;
	}
	status = array_new(realm, result);
	if (status != 0)
		goto release_source;
	result_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &result_root);
	if (status != 0)
		goto release_source;

	/* An iterable's values come from its iterator, then the mapper runs over them. */
	if (iterable) {
		status = vm_array_spread(realm, *result, source);
		if (status != 0)
			goto cleanup;
		length = ((struct vm_object *)vm_value_as_cell(*result))->length;
		if (mapper != VM_VALUE_UNDEFINED) {
			for (index = 0; index < length; index++) {
				status = array_get(realm, *result, index, &value);
				if (status != 0)
					goto cleanup;
				call_args[0] = value;
				call_args[1] = vm_value_int32((int32_t)index);
				status = vm_call(realm, mapper, js_argument(args, count, 2), call_args, 2, &value);
				if (status != 0)
					goto cleanup;
				status = vm_object_define(realm->heap, (struct vm_object *)vm_value_as_cell(*result), vm_value_int32((int32_t)index), value, VM_PROPERTY_DEFAULT);
				if (status != 0)
					goto cleanup;
			}
		}
		length = 0;
	}

	/* Each element, mapped when asked. */
	for (index = 0; index < length; index++) {
		status = array_get(realm, source, index, &value);
		if (status == 0 && mapper != VM_VALUE_UNDEFINED) {
			call_args[0] = value;
			call_args[1] = vm_value_int32((int32_t)index);
			status = vm_call(realm, mapper, js_argument(args, count, 2), call_args, 2, &value);
		}

		/* The value at the end. */
		if (status == 0)
			status = array_append(realm, *result, value);
		if (status != 0)
			goto cleanup;
	}

	/* The completed array is returned to the caller. */
cleanup:
	vm_heap_remove_root(realm->heap, &result_root);
release_source:
	vm_heap_remove_root(realm->heap, &source_root);
	if (status != 0)
		return status;

	/* Succeeded: the array. */
	return 0;
}

/* Array.prototype.at(index). */
static int
array_at(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value object;
	uint32_t length;
	double relative;
	double index;
	int status;

	/* The object, its length and the index (from the end when negative). */
	status = array_this(realm, this_value, &object, &length);
	if (status != 0)
		return status;
	status = js_builtin_integer(realm, js_argument(args, count, 0), &relative);
	if (status != 0)
		return status;
	index = relative;
	if (relative < 0.0)
		index = (double)length + relative;

	/* Outside the array is undefined. */
	*result = VM_VALUE_UNDEFINED;
	if (index < 0.0 || index >= (double)length)
		return 0;

	/* Succeeded: the element. */
	status = array_get(realm, object, (uint32_t)index, result);
	return status;
}

/* Array.prototype.concat(...items): this and the items, arrays spread. */
static int
array_concat(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *roots[4];
	vm_value object;
	vm_value item;
	vm_value value;
	uint32_t length;
	uint32_t index;
	uint32_t next;
	unsigned argument;
	unsigned registered;
	unsigned root_index;
	int found;
	int spreadable;
	int is_cell;
	int status;

	/* The converted source, result, current element and current item survive all protocol callbacks. */
	status = vm_to_object(realm, this_value, &object);
	if (status != 0)
		return status;
	memset(roots, 0, sizeof(roots));
	roots[0] = vm_value_as_cell(object);
	registered = 0;
	for (root_index = 0; root_index < 4U; root_index++) {
		status = vm_heap_add_root(realm->heap, &roots[root_index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Keep the plain newly created destination alive before any item can run getters. */
	status = array_new(realm, result);
	if (status != 0)
		goto cleanup;
	roots[1] = vm_value_as_cell(*result);

	/* Each source item first observes its actual spreadability, then either contributes itself or its present elements. */
	next = 0;
	for (argument = 0; argument <= count; argument++) {
		item = object;
		if (argument > 0)
			item = args[argument - 1U];
		roots[3] = NULL;
		is_cell = vm_value_is_cell(item);
		if (is_cell)
			roots[3] = vm_value_as_cell(item);
		status = array_is_concat_spreadable(realm, item, &spreadable);
		if (status != 0)
			goto cleanup;
		if (!spreadable) {
			/* A result element is an own default data property and cannot invoke an inherited setter. */
			status = vm_object_define(realm->heap, (struct vm_object *)roots[1], vm_value_int32((int32_t)next), item, VM_PROPERTY_DEFAULT);
			if (status != 0)
				goto cleanup;
			next++;
			continue;
		}

		/* Snapshot length before testing each index; absent properties retain holes in the result. */
		status = js_builtin_length(realm, item, &length);
		if (status != 0)
			goto cleanup;
		for (index = 0; index < length; index++) {
			status = array_has(realm, item, index, &found);
			if (status != 0)
				goto cleanup;
			if (!found)
				continue;

			/* A newly returned element needs ownership before destination property creation may collect. */
			status = array_get(realm, item, index, &value);
			if (status != 0)
				goto cleanup;
			roots[2] = NULL;
			is_cell = vm_value_is_cell(value);
			if (is_cell)
				roots[2] = vm_value_as_cell(value);
			status = vm_object_define(realm->heap, (struct vm_object *)roots[1], vm_value_int32((int32_t)(next + index)), value, VM_PROPERTY_DEFAULT);
			if (status != 0)
				goto cleanup;
		}

		/* Logical position includes skipped holes, even when no own destination element was created. */
		next += length;
	}

	/* Publish the final logical length, including any trailing holes. */
	status = array_set_length(realm, *result, (double)next);

cleanup:
	/* All partially registered roots and abrupt source operations retire through the same ownership exit. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Propagate the source's original abrupt completion or allocation error. */
	if (status != 0)
		return status;

	/* Succeeded: the plain result contains own data elements with source holes preserved. */
	return 0;
}

/* Array.prototype.copyWithin(target, start, end). */
static int
array_copy_within(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value object;
	vm_value value;
	uint32_t length;
	uint32_t target;
	uint32_t start;
	uint32_t end;
	uint32_t remaining;
	uint32_t from;
	uint32_t to;
	int backwards;
	int found;
	int status;

	/* The object, its length and the three positions. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_relative(realm, js_argument(args, count, 0), length, 0, &target);
	if (status == 0)
		status = array_relative(realm, js_argument(args, count, 1), length, 0, &start);
	if (status == 0)
		status = array_relative(realm, js_argument(args, count, 2), length, length, &end);
	if (status != 0)
		return status;

	/* How many, and in which direction so an overlap copies correctly. */
	remaining = 0;
	if (end > start)
		remaining = end - start;
	if (remaining > length - target)
		remaining = length - target;
	backwards = start < target && target < start + remaining;
	from = start;
	to = target;
	if (backwards) {
		from = start + remaining - 1U;
		to = target + remaining - 1U;
	}

	/* Each element (a hole deletes). */
	while (remaining > 0) {
		status = array_has(realm, object, from, &found);
		if (status == 0 && found) {
			status = array_get(realm, object, from, &value);
			if (status == 0)
				status = array_set(realm, object, to, value);
		} else if (status == 0) {
			status = array_remove(realm, object, to);
		}

		/* A failure ends the copy. */
		if (status != 0)
			return status;
		if (backwards) {
			from--;
			to--;
		} else {
			from++;
			to++;
		}

		/* One fewer to go. */
		remaining--;
	}

	/* Succeeded: the object. */
	*result = object;
	return 0;
}

/* Array.prototype.every(callback, thisArg). */
static int
array_every(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The loop. */
	status = array_each(realm, this_value, args, count, ARRAY_EVERY, result);
	return status;
}

/* Array.prototype.fill(value, start, end). */
static int
array_fill(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value object;
	uint32_t length;
	uint32_t start;
	uint32_t end;
	uint32_t index;
	int status;

	/* The object, its length and the range. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_relative(realm, js_argument(args, count, 1), length, 0, &start);
	if (status == 0)
		status = array_relative(realm, js_argument(args, count, 2), length, length, &end);
	if (status != 0)
		return status;

	/* Each element of the range. */
	for (index = start; index < end; index++) {
		status = array_set(realm, object, index, js_argument(args, count, 0));
		if (status != 0)
			return status;
	}

	/* Succeeded: the object. */
	*result = object;
	return 0;
}

/* Array.prototype.filter(callback, thisArg). */
static int
array_filter(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The loop. */
	status = array_each(realm, this_value, args, count, ARRAY_FILTER, result);
	return status;
}

/* Array.prototype.find(predicate, thisArg). */
static int
array_find(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Forwards, the value. */
	status = array_search(realm, this_value, args, count, 0, ARRAY_FIND_VALUE, result);
	return status;
}

/* Array.prototype.findIndex(predicate, thisArg). */
static int
array_find_index(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Forwards, the index. */
	status = array_search(realm, this_value, args, count, 0, ARRAY_FIND_INDEX, result);
	return status;
}

/* Array.prototype.findLast(predicate, thisArg). */
static int
array_find_last(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Backwards, the value. */
	status = array_search(realm, this_value, args, count, 1, ARRAY_FIND_VALUE, result);
	return status;
}

/* Array.prototype.findLastIndex(predicate, thisArg). */
static int
array_find_last_index(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Backwards, the index. */
	status = array_search(realm, this_value, args, count, 1, ARRAY_FIND_INDEX, result);
	return status;
}

/* Array.prototype.flat(depth). */
static int
array_flat(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *roots[2];
	vm_value object;
	vm_value depth_value;
	double depth;
	uint32_t length;
	unsigned registered;
	unsigned index;
	int status;

	/* Box the receiver and read its length before depth conversion or mapper validation. */
	status = array_this(realm, this_value, &object, &length);
	if (status != 0)
		return status;

	/* A newly boxed source and unpublished result remain independently rooted through every later callback. */
	roots[0] = vm_value_as_cell(object);
	roots[1] = NULL;
	registered = 0;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Depth conversion may run arbitrary script and collect the temporary boxed source. */
	depth = 1.0;
	depth_value = js_argument(args, count, 0);
	if (depth_value != VM_VALUE_UNDEFINED) {
		status = js_builtin_integer(realm, depth_value, &depth);
		if (status != 0)
			goto cleanup;
	}

	/* Create the plain destination only after source conversions finish. */
	status = array_new(realm, result);
	if (status != 0)
		goto cleanup;
	roots[1] = vm_value_as_cell(*result);
	status = array_flatten(realm, *result, object, length, depth, VM_VALUE_UNDEFINED, VM_VALUE_UNDEFINED);

cleanup:
	/* Neither conversion failure nor a recursive source exception may leak native source or result ownership. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve the original abrupt completion from conversion, mapper or nested array inspection. */
	if (status != 0)
		return status;

	/* Succeeded: the plain destination contains the requested flattened values. */
	return 0;
}

/* Array.prototype.flatMap(mapper, thisArg). */
static int
array_flat_map(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *roots[2];
	vm_value object;
	vm_value mapper;
	uint32_t length;
	unsigned registered;
	unsigned index;
	int status;

	/* Box the receiver and read its length before depth conversion or mapper validation. */
	status = array_this(realm, this_value, &object, &length);
	if (status != 0)
		return status;

	/* A newly boxed source and unpublished result remain independently rooted through every later callback. */
	roots[0] = vm_value_as_cell(object);
	roots[1] = NULL;
	registered = 0;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Mapper validation follows source length retrieval, while the converted source remains owned. */
	status = array_callback(realm, args, count, &mapper);
	if (status != 0)
		goto cleanup;

	/* Create the plain destination only after source conversions finish. */
	status = array_new(realm, result);
	if (status != 0)
		goto cleanup;
	roots[1] = vm_value_as_cell(*result);
	status = array_flatten(realm, *result, object, length, 1.0, mapper, js_argument(args, count, 1));

cleanup:
	/* Neither conversion failure nor a recursive source exception may leak native source or result ownership. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve the original abrupt completion from conversion, mapper or nested array inspection. */
	if (status != 0)
		return status;

	/* Succeeded: the plain destination contains the requested flattened values. */
	return 0;
}

/* Array.prototype.forEach(callback, thisArg). */
static int
array_for_each(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The loop. */
	status = array_each(realm, this_value, args, count, ARRAY_EACH, result);
	return status;
}

/* Array.prototype.includes(value, fromIndex): SameValueZero, holes read as undefined. */
static int
array_includes(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value object;
	vm_value value;
	vm_value wanted;
	uint32_t length;
	uint32_t index;
	int same;
	int numbers;
	int status;

	/* The object, its length and where to start. */
	*result = VM_VALUE_FALSE;
	status = array_this(realm, this_value, &object, &length);
	if (status != 0 || length == 0)
		return status;
	status = array_relative(realm, js_argument(args, count, 1), length, 0, &index);
	if (status != 0)
		return status;

	/* Each element from there. */
	wanted = js_argument(args, count, 0);
	for (; index < length; index++) {
		status = array_get(realm, object, index, &value);
		if (status != 0)
			return status;
		same = vm_same_value(value, wanted);
		numbers = vm_value_is_number(value) && vm_value_is_number(wanted);
		if (!same && numbers)
			same = vm_value_as_number(value) == vm_value_as_number(wanted);
		if (same) {
			*result = VM_VALUE_TRUE;
			return 0;
		}
	}

	/* Succeeded: not there. */
	return 0;
}

/* Array.prototype.indexOf(value, fromIndex): strict equality, holes skipped. */
static int
array_index_of(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value object;
	vm_value value;
	uint32_t length;
	uint32_t index;
	int found;
	int equal;
	int status;

	/* The object, its length and where to start. */
	*result = vm_value_int32(-1);
	status = array_this(realm, this_value, &object, &length);
	if (status != 0 || length == 0)
		return status;
	status = array_relative(realm, js_argument(args, count, 1), length, 0, &index);
	if (status != 0)
		return status;

	/* Each present element from there. */
	for (; index < length; index++) {
		status = array_has(realm, object, index, &found);
		if (status == 0 && found)
			status = array_get(realm, object, index, &value);
		if (status != 0)
			return status;
		if (!found)
			continue;
		equal = vm_strict_equals(value, js_argument(args, count, 0));
		if (equal) {
			*result = vm_value_int32((int32_t)index);
			return 0;
		}
	}

	/* Succeeded: not there. */
	return 0;
}

/* Array.prototype.join(separator). */
static int
array_join(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *separator;
	vm_value object;
	vm_value separator_value;
	uint32_t length;
	int status;

	/* The object, its length and the separator ("," by default). */
	status = array_this(realm, this_value, &object, &length);
	if (status != 0)
		return status;
	separator_value = js_argument(args, count, 0);
	if (separator_value == VM_VALUE_UNDEFINED) {
		separator = vm_string_from_utf8(realm->heap, ",", 1);
		if (separator == NULL)
			return ENOMEM;
	} else {
		status = vm_to_string(realm, args[0], &separator);
		if (status != 0)
			return status;
	}

	/* The joined text. */
	status = array_join_with(realm, object, length, separator, 0, result);
	return status;
}

/* Array.prototype.lastIndexOf(value, fromIndex). */
static int
array_last_index_of(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value object;
	vm_value value;
	uint32_t length;
	double from;
	double index;
	int found;
	int equal;
	int status;

	/* The object, its length and where to start (the end by default, from the end when negative). */
	*result = vm_value_int32(-1);
	status = array_this(realm, this_value, &object, &length);
	if (status != 0 || length == 0)
		return status;
	from = (double)length - 1.0;
	if (count > 1U) {
		status = js_builtin_integer(realm, args[1], &from);
		if (status != 0)
			return status;
	}

	/* From the end when negative, at most the last index. */
	index = from;
	if (from < 0.0)
		index = (double)length + from;
	if (index > (double)length - 1.0)
		index = (double)length - 1.0;

	/* Each present element backwards. */
	for (; index >= 0.0; index -= 1.0) {
		status = array_has(realm, object, (uint32_t)index, &found);
		if (status == 0 && found)
			status = array_get(realm, object, (uint32_t)index, &value);
		if (status != 0)
			return status;
		if (!found)
			continue;
		equal = vm_strict_equals(value, js_argument(args, count, 0));
		if (equal) {
			*result = vm_value_number(index);
			return 0;
		}
	}

	/* Succeeded: not there. */
	return 0;
}

/* Array.prototype.map(callback, thisArg). */
static int
array_map(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The loop. */
	status = array_each(realm, this_value, args, count, ARRAY_MAP, result);
	return status;
}

/* Array.prototype.pop(). */
static int
array_pop(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *result_root;
	vm_value object;
	uint32_t length;
	int is_cell;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The object and its length; an empty one keeps length 0. */
	*result = VM_VALUE_UNDEFINED;
	status = array_this(realm, this_value, &object, &length);
	if (status != 0)
		return status;
	if (length == 0) {
		status = array_set_length(realm, object, 0.0);
		return status;
	}

	/* The last element, removed. */
	status = array_get(realm, object, length - 1U, result);
	if (status != 0)
		return status;
	result_root = NULL;
	is_cell = vm_value_is_cell(*result);
	if (is_cell)
		result_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &result_root);
	if (status != 0)
		return status;
	status = array_remove(realm, object, length - 1U);
	if (status == 0)
		status = array_set_length(realm, object, (double)length - 1.0);
	vm_heap_remove_root(realm->heap, &result_root);
	return status;
}

/* Array.prototype.push(...items). */
static int
array_push(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value object;
	uint32_t length;
	unsigned index;
	int status;

	/* The object and its length. */
	status = array_this(realm, this_value, &object, &length);
	if (status != 0)
		return status;
	if ((double)length + (double)count > 9007199254740991.0) {
		status = vm_throw_type_error(realm, "Pushing past the largest length");
		return status;
	}

	/* Each item at the end. */
	for (index = 0; index < count; index++) {
		status = array_set(realm, object, length + index, args[index]);
		if (status != 0)
			return status;
	}

	/* Succeeded: the new length. */
	status = array_set_length(realm, object, (double)length + (double)count);
	*result = vm_value_number((double)length + (double)count);
	return status;
}

/* Array.prototype.reduce(callback, initial). */
static int
array_reduce(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *result_root;
	vm_value object;
	vm_value callback;
	vm_value value;
	vm_value call_args[4];
	uint32_t length;
	uint32_t index;
	int found;
	int started;
	int is_cell;
	int status;

	/* The object, its length and the callback. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_callback(realm, args, count, &callback);
	if (status != 0)
		return status;

	/* The accumulator: the initial value, or the first present element. */
	started = count > 1U;
	if (started)
		*result = args[1];
	result_root = NULL;
	is_cell = 0;
	if (started)
		is_cell = vm_value_is_cell(*result);
	if (is_cell)
		result_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &result_root);
	if (status != 0)
		return status;
	for (index = 0; index < length; index++) {
		status = array_has(realm, object, index, &found);
		if (status == 0 && found)
			status = array_get(realm, object, index, &value);
		if (status != 0)
			goto cleanup;
		if (!found)
			continue;
		if (!started) {
			*result = value;
			result_root = NULL;
			is_cell = vm_value_is_cell(*result);
			if (is_cell)
				result_root = vm_value_as_cell(*result);
			started = 1;
			continue;
		}

		/* The callback on the accumulator and the element. */
		call_args[0] = *result;
		call_args[1] = value;
		call_args[2] = vm_value_int32((int32_t)index);
		call_args[3] = object;
		status = vm_call(realm, callback, VM_VALUE_UNDEFINED, call_args, 4, result);
		if (status != 0)
			goto cleanup;
		result_root = NULL;
		is_cell = vm_value_is_cell(*result);
		if (is_cell)
			result_root = vm_value_as_cell(*result);
	}

	/* No element and no initial value. */
	if (!started) {
		status = vm_throw_type_error(realm, "Reduce of empty array with no initial value");
		goto cleanup;
	}

	/* The accumulator is handed to the caller. */
cleanup:
	vm_heap_remove_root(realm->heap, &result_root);
	return status;
}

/* Array.prototype.reduceRight(callback, initial). */
static int
array_reduce_right(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *result_root;
	vm_value object;
	vm_value callback;
	vm_value value;
	vm_value call_args[4];
	uint32_t length;
	uint32_t index;
	int found;
	int started;
	int is_cell;
	int status;

	/* The object, its length and the callback. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_callback(realm, args, count, &callback);
	if (status != 0)
		return status;

	/* The accumulator from the end. */
	started = count > 1U;
	if (started)
		*result = args[1];
	result_root = NULL;
	is_cell = 0;
	if (started)
		is_cell = vm_value_is_cell(*result);
	if (is_cell)
		result_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &result_root);
	if (status != 0)
		return status;
	for (index = length; index > 0; index--) {
		status = array_has(realm, object, index - 1U, &found);
		if (status == 0 && found)
			status = array_get(realm, object, index - 1U, &value);
		if (status != 0)
			goto cleanup;
		if (!found)
			continue;
		if (!started) {
			*result = value;
			result_root = NULL;
			is_cell = vm_value_is_cell(*result);
			if (is_cell)
				result_root = vm_value_as_cell(*result);
			started = 1;
			continue;
		}

		/* The callback on the accumulator and the element. */
		call_args[0] = *result;
		call_args[1] = value;
		call_args[2] = vm_value_int32((int32_t)(index - 1U));
		call_args[3] = object;
		status = vm_call(realm, callback, VM_VALUE_UNDEFINED, call_args, 4, result);
		if (status != 0)
			goto cleanup;
		result_root = NULL;
		is_cell = vm_value_is_cell(*result);
		if (is_cell)
			result_root = vm_value_as_cell(*result);
	}

	/* No element and no initial value. */
	if (!started) {
		status = vm_throw_type_error(realm, "Reduce of empty array with no initial value");
		goto cleanup;
	}

	/* The accumulator is handed to the caller. */
cleanup:
	vm_heap_remove_root(realm->heap, &result_root);
	return status;
}

/* Array.prototype.reverse(). */
static int
array_reverse(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *lower_root;
	struct vm_cell *upper_root;
	vm_value object;
	vm_value lower_value;
	vm_value upper_value;
	uint32_t length;
	uint32_t lower;
	uint32_t upper;
	int lower_found;
	int upper_found;
	int is_cell;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The object and its length. */
	status = array_this(realm, this_value, &object, &length);
	if (status != 0)
		return status;
	lower_root = NULL;
	upper_root = NULL;
	status = vm_heap_add_root(realm->heap, &lower_root);
	if (status != 0)
		return status;
	status = vm_heap_add_root(realm->heap, &upper_root);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &lower_root);
		return status;
	}

	/* Each pair from both ends (holes move too). */
	for (lower = 0; length > 0 && lower < length / 2U; lower++) {
		lower_root = NULL;
		upper_root = NULL;
		upper = length - 1U - lower;
		status = array_has(realm, object, lower, &lower_found);
		if (status == 0 && lower_found) {
			status = array_get(realm, object, lower, &lower_value);
			if (status == 0) {
				is_cell = vm_value_is_cell(lower_value);
				if (is_cell)
					lower_root = vm_value_as_cell(lower_value);
			}
		}

		/* The upper lookup may invoke user code before the lower value is written. */
		if (status == 0)
			status = array_has(realm, object, upper, &upper_found);
		if (status == 0 && upper_found) {
			status = array_get(realm, object, upper, &upper_value);
			if (status == 0) {
				is_cell = vm_value_is_cell(upper_value);
				if (is_cell)
					upper_root = vm_value_as_cell(upper_value);
			}
		}

		/* Both fetched values remain live through the swap. */
		if (status == 0 && upper_found)
			status = array_set(realm, object, lower, upper_value);
		if (status == 0 && !upper_found && lower_found)
			status = array_remove(realm, object, lower);
		if (status == 0 && lower_found)
			status = array_set(realm, object, upper, lower_value);
		if (status == 0 && !lower_found && upper_found)
			status = array_remove(realm, object, upper);
		if (status != 0)
			goto cleanup;
	}

	/* The swapped values have been published back into the receiver. */
cleanup:
	vm_heap_remove_root(realm->heap, &upper_root);
	vm_heap_remove_root(realm->heap, &lower_root);
	if (status != 0)
		return status;

	/* Succeeded: the object. */
	*result = object;
	return 0;
}

/* Array.prototype.shift(). */
static int
array_shift(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *result_root;
	vm_value object;
	vm_value value;
	uint32_t length;
	uint32_t index;
	int found;
	int is_cell;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The object and its length; an empty one keeps length 0. */
	*result = VM_VALUE_UNDEFINED;
	status = array_this(realm, this_value, &object, &length);
	if (status != 0)
		return status;
	if (length == 0) {
		status = array_set_length(realm, object, 0.0);
		return status;
	}

	/* The first element, and every other one down by one. */
	status = array_get(realm, object, 0, result);
	if (status != 0)
		return status;
	result_root = NULL;
	is_cell = vm_value_is_cell(*result);
	if (is_cell)
		result_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &result_root);
	if (status != 0)
		return status;
	for (index = 1; status == 0 && index < length; index++) {
		status = array_has(realm, object, index, &found);
		if (status == 0 && found) {
			status = array_get(realm, object, index, &value);
			if (status == 0)
				status = array_set(realm, object, index - 1U, value);
		} else if (status == 0) {
			status = array_remove(realm, object, index - 1U);
		}
	}

	/* The last one goes, and the length shrinks. */
	if (status == 0)
		status = array_remove(realm, object, length - 1U);
	if (status == 0)
		status = array_set_length(realm, object, (double)length - 1.0);
	vm_heap_remove_root(realm->heap, &result_root);
	return status;
}

/* Array.prototype.slice(start, end). */
static int
array_slice(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *result_root;
	vm_value object;
	vm_value value;
	uint32_t length;
	uint32_t start;
	uint32_t end;
	uint32_t index;
	int found;
	int status;

	/* The object, its length and the range. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_relative(realm, js_argument(args, count, 0), length, 0, &start);
	if (status == 0)
		status = array_relative(realm, js_argument(args, count, 1), length, length, &end);
	if (status == 0)
		status = array_new(realm, result);
	if (status != 0)
		return status;
	result_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &result_root);
	if (status != 0)
		return status;

	/* Each element of the range (holes kept). */
	for (index = start; index < end; index++) {
		status = array_has(realm, object, index, &found);
		if (status == 0 && found) {
			status = array_get(realm, object, index, &value);
			if (status == 0)
				status = array_set(realm, *result, index - start, value);
		}

		/* A failure ends the reversal. */
		if (status != 0)
			goto cleanup;
	}

	/* Succeeded: the new array with its length. */
	if (end < start)
		end = start;
	status = array_set_length(realm, *result, (double)(end - start));

	/* The new array is handed to the caller after every getter finishes. */
cleanup:
	vm_heap_remove_root(realm->heap, &result_root);
	return status;
}

/* Array.prototype.some(callback, thisArg). */
static int
array_some(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The loop. */
	status = array_each(realm, this_value, args, count, ARRAY_SOME, result);
	return status;
}

/* Array.prototype.sort(comparator): stable, undefined last, holes removed to the end. */
static int
array_sort(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *list;
	struct vm_cell *list_root;
	vm_value comparator;
	vm_value object;
	vm_value list_value;
	uint32_t length;
	uint32_t items;
	uint32_t index;
	int callable;
	int status;

	/* The comparator must be a function or undefined (checked first). */
	comparator = js_argument(args, count, 0);
	callable = vm_value_is_callable(comparator);
	if (comparator != VM_VALUE_UNDEFINED && !callable) {
		status = vm_throw_type_error(realm, "The comparison function must be either a function or undefined");
		return status;
	}

	/* The object, its length, and its sorted present elements. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_sorted_items(realm, object, length, comparator, 1, &list_value, &items);
	if (status != 0)
		return status;

	/* The sorted elements back, then the holes at the end. */
	list = (struct vm_object *)vm_value_as_cell(list_value);
	list_root = &list->cell;
	status = vm_heap_add_root(realm->heap, &list_root);
	if (status != 0)
		return status;
	for (index = 0; index < items; index++) {
		status = array_set(realm, object, index, list->elements[index]);
		if (status != 0)
			goto cleanup;
	}

	/* Then the holes. */
	for (index = items; index < length; index++) {
		status = array_remove(realm, object, index);
		if (status != 0)
			goto cleanup;
	}

	/* The temporary list no longer needs to survive setters. */
cleanup:
	vm_heap_remove_root(realm->heap, &list_root);
	if (status != 0)
		return status;

	/* Succeeded: the object. */
	*result = object;
	return 0;
}

/* Array.prototype.splice(start, deleteCount, ...items). */
static int
array_splice(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *result_root;
	vm_value object;
	vm_value value;
	double delete_number;
	uint32_t length;
	uint32_t start;
	uint32_t deleted;
	uint32_t inserted;
	uint32_t index;
	int found;
	int status;

	/* The object, its length, the start and how many go. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_relative(realm, js_argument(args, count, 0), length, 0, &start);
	if (status != 0)
		return status;
	inserted = 0;
	if (count > 2U)
		inserted = count - 2U;
	deleted = 0;
	if (count == 1U)
		deleted = length - start;
	if (count > 1U) {
		status = js_builtin_integer(realm, args[1], &delete_number);
		if (status != 0)
			return status;
		if (delete_number < 0.0)
			delete_number = 0.0;
		if (delete_number > (double)(length - start))
			delete_number = (double)(length - start);
		deleted = (uint32_t)delete_number;
	}

	/* The deleted elements, as the result. */
	status = array_new(realm, result);
	if (status != 0)
		return status;
	result_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &result_root);
	if (status != 0)
		return status;
	for (index = 0; status == 0 && index < deleted; index++) {
		status = array_has(realm, object, start + index, &found);
		if (status == 0 && found) {
			status = array_get(realm, object, start + index, &value);
			if (status == 0)
				status = array_set(realm, *result, index, value);
		}
	}

	/* The result's length. */
	if (status == 0)
		status = array_set_length(realm, *result, (double)deleted);
	if (status != 0)
		goto cleanup;

	/* The elements after them moved to make room or close the gap. */
	if (inserted < deleted) {
		for (index = start; status == 0 && index < length - deleted; index++) {
			status = array_has(realm, object, index + deleted, &found);
			if (status == 0 && found) {
				status = array_get(realm, object, index + deleted, &value);
				if (status == 0)
					status = array_set(realm, object, index + inserted, value);
			} else if (status == 0) {
				status = array_remove(realm, object, index + inserted);
			}
		}

		/* The ones left past the new end go. */
		for (index = length; status == 0 && index > length - deleted + inserted; index--)
			status = array_remove(realm, object, index - 1U);
	} else if (inserted > deleted) {
		for (index = length - deleted; status == 0 && index > start; index--) {
			status = array_has(realm, object, index + deleted - 1U, &found);
			if (status == 0 && found) {
				status = array_get(realm, object, index + deleted - 1U, &value);
				if (status == 0)
					status = array_set(realm, object, index + inserted - 1U, value);
			} else if (status == 0) {
				status = array_remove(realm, object, index + inserted - 1U);
			}
		}
	}

	/* The new items, then the new length. */
	for (index = 0; status == 0 && index < inserted; index++)
		status = array_set(realm, object, start + index, args[index + 2U]);
	if (status == 0)
		status = array_set_length(realm, object, (double)length - (double)deleted + (double)inserted);

	/* The removed elements survive all receiver mutations until return. */
cleanup:
	vm_heap_remove_root(realm->heap, &result_root);
	return status;
}

/* Array.prototype.toLocaleString(): each element's toLocaleString, joined by commas. */
static int
array_to_locale_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *separator;
	vm_value object;
	uint32_t length;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The object, its length, and the joined locale strings. */
	status = array_this(realm, this_value, &object, &length);
	if (status != 0)
		return status;
	separator = vm_string_from_utf8(realm->heap, ",", 1);
	if (separator == NULL)
		return ENOMEM;
	status = array_join_with(realm, object, length, separator, 1, result);
	return status;
}

/* Array.prototype.toReversed(): a reversed copy (holes read as undefined). */
static int
array_to_reversed(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *result_root;
	vm_value object;
	vm_value value;
	uint32_t length;
	uint32_t index;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The object, its length, and the copy. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_new(realm, result);
	if (status != 0)
		return status;
	result_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &result_root);
	if (status != 0)
		return status;
	for (index = 0; status == 0 && index < length; index++) {
		status = array_get(realm, object, length - 1U - index, &value);
		if (status == 0)
			status = array_set(realm, *result, index, value);
	}

	/* The reversed copy is now handed to the caller. */
	vm_heap_remove_root(realm->heap, &result_root);
	return status;
}

/* Array.prototype.toSorted(comparator): a sorted copy (holes read as undefined). */
static int
array_to_sorted(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value comparator;
	vm_value object;
	uint32_t length;
	uint32_t items;
	int callable;
	int status;

	/* The comparator must be a function or undefined. */
	comparator = js_argument(args, count, 0);
	callable = vm_value_is_callable(comparator);
	if (comparator != VM_VALUE_UNDEFINED && !callable) {
		status = vm_throw_type_error(realm, "The comparison function must be either a function or undefined");
		return status;
	}

	/* The sorted elements are the copy. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_sorted_items(realm, object, length, comparator, 0, result, &items);
	return status;
}

/* Array.prototype.toSpliced(start, skipCount, ...items): a spliced copy. */
static int
array_to_spliced(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *result_root;
	vm_value object;
	vm_value value;
	double skip_number;
	uint32_t length;
	uint32_t start;
	uint32_t skipped;
	uint32_t index;
	uint32_t next;
	int status;

	/* The object, its length, the start and how many are skipped. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_relative(realm, js_argument(args, count, 0), length, 0, &start);
	if (status != 0)
		return status;
	skipped = 0;
	if (count == 1U)
		skipped = length - start;
	if (count > 1U) {
		status = js_builtin_integer(realm, args[1], &skip_number);
		if (status != 0)
			return status;
		if (skip_number < 0.0)
			skip_number = 0.0;
		if (skip_number > (double)(length - start))
			skip_number = (double)(length - start);
		skipped = (uint32_t)skip_number;
	}

	/* Before the start, the new items, after the skipped ones. */
	status = array_new(realm, result);
	if (status != 0)
		return status;
	result_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &result_root);
	if (status != 0)
		return status;
	next = 0;
	for (index = 0; status == 0 && index < start; index++) {
		status = array_get(realm, object, index, &value);
		if (status == 0)
			status = array_set(realm, *result, next, value);
		next++;
	}

	/* The new items. */
	for (index = 2; status == 0 && index < count; index++) {
		status = array_set(realm, *result, next, args[index]);
		next++;
	}

	/* The rest after the skipped ones. */
	for (index = start + skipped; status == 0 && index < length; index++) {
		status = array_get(realm, object, index, &value);
		if (status == 0)
			status = array_set(realm, *result, next, value);
		next++;
	}

	/* The copy is handed to the caller after every getter finishes. */
	vm_heap_remove_root(realm->heap, &result_root);
	return status;
}

/* Array.prototype.toString(): this.join(), or Object.prototype.toString when there is no join. */
static int
array_to_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *object_root;
	vm_value object;
	vm_value key;
	vm_value join;
	vm_value to_string;
	int callable;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The object and its join. */
	status = vm_to_object(realm, this_value, &object);
	if (status != 0)
		return status;
	object_root = vm_value_as_cell(object);
	status = vm_heap_add_root(realm->heap, &object_root);
	if (status != 0)
		return status;
	key = vm_key_from_ascii(realm->heap, "join");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Read the receiver's join override. */
	status = vm_get(realm, object, key, &join);
	if (status != 0)
		goto cleanup;

	/* A callable join is called; otherwise Object.prototype.toString. */
	callable = vm_value_is_callable(join);
	if (callable) {
		status = vm_call(realm, join, object, NULL, 0, result);
		goto cleanup;
	}

	/* Object.prototype.toString, called on the object. */
	key = vm_key_from_ascii(realm->heap, "toString");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Fall back to the base object's string method. */
	status = vm_object_get(realm->object_prototype, key, &to_string);
	if (status != 0)
		goto cleanup;
	status = vm_call(realm, to_string, object, NULL, 0, result);
	/* The receiver survives either dynamic method call. */
cleanup:
	vm_heap_remove_root(realm->heap, &object_root);
	return status;
}

/* Array.prototype.unshift(...items). */
static int
array_unshift(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value object;
	vm_value value;
	uint32_t length;
	uint32_t index;
	int found;
	int status;

	/* The object and its length. */
	status = array_this(realm, this_value, &object, &length);
	if (status != 0)
		return status;

	/* The elements up by the count, from the top, then the items at the start. */
	for (index = length; count > 0 && status == 0 && index > 0; index--) {
		status = array_has(realm, object, index - 1U, &found);
		if (status == 0 && found) {
			status = array_get(realm, object, index - 1U, &value);
			if (status == 0)
				status = array_set(realm, object, index - 1U + count, value);
		} else if (status == 0) {
			status = array_remove(realm, object, index - 1U + count);
		}
	}

	/* The items at the start. */
	for (index = 0; status == 0 && index < count; index++)
		status = array_set(realm, object, index, args[index]);

	/* Succeeded: the new length. */
	if (status == 0)
		status = array_set_length(realm, object, (double)length + (double)count);
	*result = vm_value_number((double)length + (double)count);
	return status;
}

/* Array.prototype.with(index, value): a copy with one element replaced. */
static int
array_with(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *result_root;
	vm_value object;
	vm_value value;
	double relative;
	double target;
	uint32_t length;
	uint32_t index;
	int status;

	/* The object, its length and the index (from the end when negative), which must be inside. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = js_builtin_integer(realm, js_argument(args, count, 0), &relative);
	if (status != 0)
		return status;
	target = relative;
	if (relative < 0.0)
		target = (double)length + relative;
	if (target < 0.0 || target >= (double)length) {
		status = vm_throw_range_error(realm, "Invalid index");
		return status;
	}

	/* The copy. */
	status = array_new(realm, result);
	if (status != 0)
		return status;
	result_root = vm_value_as_cell(*result);
	status = vm_heap_add_root(realm->heap, &result_root);
	if (status != 0)
		return status;
	for (index = 0; status == 0 && index < length; index++) {
		value = js_argument(args, count, 1);
		if ((double)index != target)
			status = array_get(realm, object, index, &value);
		if (status == 0)
			status = array_set(realm, *result, index, value);
	}

	/* The copy is handed to the caller after every getter finishes. */
	vm_heap_remove_root(realm->heap, &result_root);
	return status;
}

/* Finds this as an object and its length. */
static int
array_this(
	struct vm_realm *realm,
	vm_value this_value,
	vm_value *object,
	uint32_t *length)
{
	struct vm_cell *object_root;
	int status;

	/* The object, then its length. */
	*length = 0;
	status = vm_to_object(realm, this_value, object);
	if (status != 0)
		return status;
	object_root = vm_value_as_cell(*object);
	status = vm_heap_add_root(realm->heap, &object_root);
	if (status != 0)
		return status;
	status = js_builtin_length(realm, *object, length);
	vm_heap_remove_root(realm->heap, &object_root);
	return status;
}

/* Tells whether an object has an index (HasProperty). */
static int
array_has(
	struct vm_realm *realm,
	vm_value object,
	uint32_t index,
	int *found)
{
	struct vm_cell *object_root;
	vm_value has;
	int status;

	/* The in operator's search. */
	object_root = vm_value_as_cell(object);
	status = vm_heap_add_root(realm->heap, &object_root);
	if (status != 0)
		return status;
	status = vm_in(realm, vm_value_int32((int32_t)index), object, &has);
	vm_heap_remove_root(realm->heap, &object_root);
	if (status != 0)
		return status;
	*found = has == VM_VALUE_TRUE;
	return 0;
}

/* Gets an element. */
static int
array_get(
	struct vm_realm *realm,
	vm_value object,
	uint32_t index,
	vm_value *value)
{
	struct vm_cell *object_root;
	int status;

	/* An element of a walk that may be long: a task asked to end stops it. */
	status = vm_heap_count_step(realm->heap);
	if (status != 0)
		return status;

	/* Through the chain. */
	object_root = vm_value_as_cell(object);
	status = vm_heap_add_root(realm->heap, &object_root);
	if (status != 0)
		return status;
	status = vm_get(realm, object, vm_value_int32((int32_t)index), value);
	vm_heap_remove_root(realm->heap, &object_root);
	return status;
}

/* Sets an element (a refusal throws). */
static int
array_set(
	struct vm_realm *realm,
	vm_value object,
	uint32_t index,
	vm_value value)
{
	struct vm_cell *object_root;
	struct vm_cell *value_root;
	int is_cell;
	int status;

	/* An element of a walk that may be long: a task asked to end stops it. */
	status = vm_heap_count_step(realm->heap);
	if (status != 0)
		return status;

	/* Strictly. */
	object_root = vm_value_as_cell(object);
	value_root = NULL;
	is_cell = vm_value_is_cell(value);
	if (is_cell)
		value_root = vm_value_as_cell(value);
	status = vm_heap_add_root(realm->heap, &object_root);
	if (status != 0)
		return status;
	status = vm_heap_add_root(realm->heap, &value_root);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &object_root);
		return status;
	}

	/* Both receiver and assigned value survive setter code. */
	status = vm_set(realm, object, vm_value_int32((int32_t)index), value, 1);
	vm_heap_remove_root(realm->heap, &value_root);
	vm_heap_remove_root(realm->heap, &object_root);
	return status;
}

/* Deletes an element (a refusal throws). */
static int
array_remove(
	struct vm_realm *realm,
	vm_value object,
	uint32_t index)
{
	struct vm_cell *object_root;
	vm_value deleted;
	int status;

	/* Strictly. */
	object_root = vm_value_as_cell(object);
	status = vm_heap_add_root(realm->heap, &object_root);
	if (status != 0)
		return status;
	status = vm_delete(realm, object, vm_value_int32((int32_t)index), 1, &deleted);
	vm_heap_remove_root(realm->heap, &object_root);
	return status;
}

/* Sets the length property (a refusal throws). */
static int
array_set_length(
	struct vm_realm *realm,
	vm_value object,
	double length)
{
	struct vm_cell *object_root;
	vm_value key;
	int status;

	/* Strictly. */
	object_root = vm_value_as_cell(object);
	status = vm_heap_add_root(realm->heap, &object_root);
	if (status != 0)
		return status;
	key = vm_key_from_ascii(realm->heap, "length");
	if (key == VM_VALUE_EMPTY) {
		vm_heap_remove_root(realm->heap, &object_root);
		return ENOMEM;
	}

	/* The receiver remains live through its length setter. */
	status = vm_set(realm, object, key, vm_value_number(length), 1);
	vm_heap_remove_root(realm->heap, &object_root);
	return status;
}

/* Makes an empty array. */
static int
array_new(
	struct vm_realm *realm,
	vm_value *array)
{
	struct vm_object *made;

	/* A plain array from Array.prototype. */
	made = vm_array_create(realm->heap, realm->array_prototype);
	if (made == NULL)
		return ENOMEM;
	*array = vm_value_cell(made);
	return 0;
}

/* Appends a value to an array made here (a definition, not an assignment). */
static int
array_append(
	struct vm_realm *realm,
	vm_value array,
	vm_value value)
{
	struct vm_object *object;
	struct vm_cell *array_root;
	struct vm_cell *value_root;
	int is_cell;
	int status;

	/* At the end. */
	object = (struct vm_object *)vm_value_as_cell(array);
	array_root = &object->cell;
	value_root = NULL;
	is_cell = vm_value_is_cell(value);
	if (is_cell)
		value_root = vm_value_as_cell(value);
	status = vm_heap_add_root(realm->heap, &array_root);
	if (status != 0)
		return status;
	status = vm_heap_add_root(realm->heap, &value_root);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &array_root);
		return status;
	}

	/* The new element and destination survive definition. */
	status = vm_object_define(realm->heap, object, vm_value_int32((int32_t)object->length), value, VM_PROPERTY_DEFAULT);
	vm_heap_remove_root(realm->heap, &value_root);
	vm_heap_remove_root(realm->heap, &array_root);
	return status;
}

/* Requires the first argument to be a function. */
static int
array_callback(
	struct vm_realm *realm,
	const vm_value *args,
	unsigned count,
	vm_value *callback)
{
	int callable;
	int status;

	/* A function, or a TypeError. */
	*callback = js_argument(args, count, 0);
	callable = vm_value_is_callable(*callback);
	if (!callable) {
		status = vm_throw_type_error(realm, "The callback is not a function");
		return status;
	}

	/* The callback. */
	return 0;
}

/* Runs a callback over each present element: forEach, every, some, map and filter. */
static int
array_each(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	int mode,
	vm_value *result)
{
	struct vm_cell *result_root;
	struct vm_cell *value_root;
	vm_value object;
	vm_value callback;
	vm_value value;
	vm_value answer;
	vm_value call_args[3];
	uint32_t length;
	uint32_t index;
	uint32_t kept;
	int found;
	int truth;
	int rooted;
	int is_cell;
	int status;

	/* The object, its length and the callback. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_callback(realm, args, count, &callback);
	if (status != 0)
		return status;

	/* The answer by the mode's default. */
	rooted = 0;
	*result = VM_VALUE_UNDEFINED;
	if (mode == ARRAY_EVERY)
		*result = VM_VALUE_TRUE;
	if (mode == ARRAY_SOME)
		*result = VM_VALUE_FALSE;
	if (mode == ARRAY_MAP || mode == ARRAY_FILTER) {
		status = array_new(realm, result);
		if (status != 0)
			return status;
		result_root = vm_value_as_cell(*result);
		status = vm_heap_add_root(realm->heap, &result_root);
		if (status != 0)
			return status;
		rooted = 1;
	}

	/* map's result is as long. */
	if (mode == ARRAY_MAP) {
		status = array_set_length(realm, *result, (double)length);
		if (status != 0)
			goto cleanup;
	}

	/* A fetched value survives a callback that removes it from the receiver. */
	value_root = NULL;
	status = vm_heap_add_root(realm->heap, &value_root);
	if (status != 0)
		goto cleanup;

	/* Each present element. */
	kept = 0;
	for (index = 0; index < length; index++) {
		value_root = NULL;
		status = array_has(realm, object, index, &found);
		if (status == 0 && found)
			status = array_get(realm, object, index, &value);
		if (status != 0)
			goto release_value;
		if (!found)
			continue;
		is_cell = vm_value_is_cell(value);
		if (is_cell)
			value_root = vm_value_as_cell(value);
		call_args[0] = value;
		call_args[1] = vm_value_int32((int32_t)index);
		call_args[2] = object;
		status = vm_call(realm, callback, js_argument(args, count, 1), call_args, 3, &answer);
		if (status != 0)
			goto release_value;

		/* What the mode does with the answer. */
		truth = vm_to_boolean(answer);
		if (mode == ARRAY_EVERY && !truth) {
			*result = VM_VALUE_FALSE;
			goto release_value;
		}

		/* some stops at the first true. */
		if (mode == ARRAY_SOME && truth) {
			*result = VM_VALUE_TRUE;
			goto release_value;
		}

		/* map keeps each answer at its index. */
		if (mode == ARRAY_MAP) {
			value_root = NULL;
			is_cell = vm_value_is_cell(answer);
			if (is_cell)
				value_root = vm_value_as_cell(answer);
			status = vm_object_define(realm->heap, (struct vm_object *)vm_value_as_cell(*result), vm_value_int32((int32_t)index),
			    answer, VM_PROPERTY_DEFAULT);
			if (status != 0)
				goto release_value;
		}

		/* filter keeps the accepted values in order. */
		if (mode == ARRAY_FILTER && truth) {
			status = vm_object_define(realm->heap, (struct vm_object *)vm_value_as_cell(*result), vm_value_int32((int32_t)kept),
			    value, VM_PROPERTY_DEFAULT);
			if (status != 0)
				goto release_value;
			kept++;
		}
	}

	/* The result is now owned by the caller. */
release_value:
	vm_heap_remove_root(realm->heap, &value_root);
cleanup:
	if (rooted)
		vm_heap_remove_root(realm->heap, &result_root);
	return status;
}

/* Finds the first (or last) element a predicate accepts: its value or its index (holes read as undefined). */
static int
array_search(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	int backwards,
	int what,
	vm_value *result)
{
	struct vm_cell *value_root;
	vm_value object;
	vm_value predicate;
	vm_value value;
	vm_value answer;
	vm_value call_args[3];
	uint32_t length;
	uint32_t step;
	uint32_t index;
	int truth;
	int is_cell;
	int status;

	/* The object, its length and the predicate. */
	status = array_this(realm, this_value, &object, &length);
	if (status == 0)
		status = array_callback(realm, args, count, &predicate);
	if (status != 0)
		return status;
	value_root = NULL;
	status = vm_heap_add_root(realm->heap, &value_root);
	if (status != 0)
		return status;

	/* Each element in the direction asked. */
	*result = VM_VALUE_UNDEFINED;
	if (what == ARRAY_FIND_INDEX)
		*result = vm_value_int32(-1);
	for (step = 0; step < length; step++) {
		value_root = NULL;
		index = step;
		if (backwards)
			index = length - 1U - step;
		status = array_get(realm, object, index, &value);
		if (status != 0)
			goto cleanup;
		is_cell = vm_value_is_cell(value);
		if (is_cell)
			value_root = vm_value_as_cell(value);
		call_args[0] = value;
		call_args[1] = vm_value_int32((int32_t)index);
		call_args[2] = object;
		status = vm_call(realm, predicate, js_argument(args, count, 1), call_args, 3, &answer);
		if (status != 0)
			goto cleanup;
		truth = vm_to_boolean(answer);
		if (!truth)
			continue;
		*result = value;
		if (what == ARRAY_FIND_INDEX)
			*result = vm_value_int32((int32_t)index);
		goto cleanup;
	}

	/* Succeeded: none accepted. */
cleanup:
	vm_heap_remove_root(realm->heap, &value_root);
	return status;
}

/* Converts a relative position (negative from the end) into an index from 0 to the length. */
static int
array_relative(
	struct vm_realm *realm,
	vm_value value,
	uint32_t length,
	uint32_t fallback,
	uint32_t *index)
{
	double relative;
	int status;

	/* undefined is the fallback. */
	*index = fallback;
	if (value == VM_VALUE_UNDEFINED)
		return 0;
	status = js_builtin_integer(realm, value, &relative);
	if (status != 0)
		return status;

	/* From the end when negative, clamped to the array. */
	if (relative < 0.0)
		relative += (double)length;
	if (relative < 0.0)
		relative = 0.0;
	if (relative > (double)length)
		relative = (double)length;
	*index = (uint32_t)relative;
	return 0;
}

/* Appends a source's elements to a target, arrays flattened to a depth (each mapped first when a mapper is given). */
static int
array_flatten(
	struct vm_realm *realm,
	vm_value target,
	vm_value source,
	uint32_t source_length,
	double depth,
	vm_value mapper,
	vm_value this_arg)
{
	struct vm_cell *value_root;
	vm_value value;
	vm_value call_args[3];
	uint32_t index;
	uint32_t length;
	int found;
	int is_array;
	int is_cell;
	int status;

	/* Each present element. */
	value_root = NULL;
	status = vm_heap_add_root(realm->heap, &value_root);
	if (status != 0)
		return status;
	for (index = 0; index < source_length; index++) {
		status = array_has(realm, source, index, &found);
		if (status == 0 && found)
			status = array_get(realm, source, index, &value);
		if (status != 0)
			goto cleanup;
		if (!found)
			continue;
		value_root = NULL;
		is_cell = vm_value_is_cell(value);
		if (is_cell)
			value_root = vm_value_as_cell(value);
		if (mapper != VM_VALUE_UNDEFINED) {
			call_args[0] = value;
			call_args[1] = vm_value_int32((int32_t)index);
			call_args[2] = source;
			status = vm_call(realm, mapper, this_arg, call_args, 3, &value);
			if (status != 0)
				goto cleanup;
			value_root = NULL;
			is_cell = vm_value_is_cell(value);
			if (is_cell)
				value_root = vm_value_as_cell(value);
		}

		/* An array within the depth is flattened; anything else appended. */
		is_array = 0;
		if (depth > 0.0) {
			status = vm_is_array(realm, value, &is_array);
			if (status != 0)
				goto cleanup;
		}

		/* Zero or negative depth appends even a revoked Proxy without testing its array identity. */
		if (is_array) {
			status = js_builtin_length(realm, value, &length);
			if (status == 0)
				status = array_flatten(realm, target, value, length, depth - 1.0, VM_VALUE_UNDEFINED, VM_VALUE_UNDEFINED);
		} else {
			status = array_append(realm, target, value);
		}

		/* A failure ends the flattening. */
		if (status != 0)
			goto cleanup;
	}

	/* The mapped value no longer spans another getter or recursive call. */
cleanup:
	vm_heap_remove_root(realm->heap, &value_root);
	return status;
}

/* Tells whether a value is an array object. */
static int
array_is_concat_spreadable(
	struct vm_realm *realm,
	vm_value value,
	int *spreadable)
{
	vm_value override;
	int is_object;
	int status;

	/* Primitives contribute one item and cannot supply a spreadability override. */
	*spreadable = 0;
	is_object = vm_value_is_object(value);
	if (!is_object)
		return 0;

	/* A present override is converted only to Boolean, without coercing any returned object. */
	status = vm_get(realm, value, vm_symbol_key(realm, VM_SYMBOL_IS_CONCAT_SPREADABLE), &override);
	if (status != 0)
		return status;
	if (override != VM_VALUE_UNDEFINED) {
		*spreadable = vm_to_boolean(override);
		return 0;
	}

	/* Without an override, actual array identity unwraps nested Proxies and preserves revoked-record errors. */
	status = vm_is_array(realm, value, spreadable);
	if (status != 0)
		return status;

	/* Succeeded: the explicit override or actual array identity supplied spreadability. */
	return 0;
}

/* Compares two elements for sort: undefined last, then the comparator's answer or the strings' order. */
static int
array_compare(
	struct vm_realm *realm,
	vm_value comparator,
	vm_value left,
	vm_value right,
	int *order)
{
	struct vm_string *left_string;
	struct vm_string *right_string;
	struct vm_cell *left_root;
	vm_value call_args[2];
	vm_value answer;
	double number;
	int status;

	/* undefined sorts after everything. */
	*order = 0;
	if (left == VM_VALUE_UNDEFINED && right == VM_VALUE_UNDEFINED)
		return 0;
	if (left == VM_VALUE_UNDEFINED) {
		*order = 1;
		return 0;
	}

	/* The same for the right. */
	if (right == VM_VALUE_UNDEFINED) {
		*order = -1;
		return 0;
	}

	/* The comparator's number (NaN is 0). */
	if (comparator != VM_VALUE_UNDEFINED) {
		call_args[0] = left;
		call_args[1] = right;
		status = vm_call(realm, comparator, VM_VALUE_UNDEFINED, call_args, 2, &answer);
		if (status == 0)
			status = vm_to_number(realm, answer, &number);
		if (status != 0)
			return status;
		if (number < 0.0)
			*order = -1;
		if (number > 0.0)
			*order = 1;
		return 0;
	}

	/* Otherwise the strings by code units. */
	status = vm_to_string(realm, left, &left_string);
	if (status != 0)
		return status;
	left_root = &left_string->cell;
	status = vm_heap_add_root(realm->heap, &left_root);
	if (status != 0)
		return status;
	status = vm_to_string(realm, right, &right_string);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &left_root);
		return status;
	}

	/* Both strings remain live while their code units are compared. */
	*order = vm_string_compare(left_string, right_string);
	vm_heap_remove_root(realm->heap, &left_root);
	return 0;
}

/* Sorts values stably by merging runs (the scratch holds as many). */
static int
array_merge_sort(
	struct vm_realm *realm,
	vm_value comparator,
	vm_value *items,
	vm_value *scratch,
	uint32_t count)
{
	uint32_t width;
	uint32_t start;
	uint32_t middle;
	uint32_t end;
	uint32_t left;
	uint32_t right;
	uint32_t out;
	int order;
	int status;

	/* Runs of width 1, 2, 4 and so on, merged pairwise. */
	for (width = 1; width < count; width *= 2U) {
		for (start = 0; start < count; start += 2U * width) {
			middle = start + width;
			if (middle > count)
				middle = count;
			end = start + 2U * width;
			if (end > count)
				end = count;
			left = start;
			right = middle;
			out = start;
			while (left < middle && right < end) {
				status = array_compare(realm, comparator, items[left], items[right], &order);
				if (status != 0)
					return status;

				/* A task asked to end stops the sort (a comparator in script stops at its own entry). */
				status = vm_heap_count_step(realm->heap);
				if (status != 0)
					return status;

				/* The smaller goes out first; equal ones keep their order. */
				if (order <= 0) {
					scratch[out] = items[left];
					left++;
				} else {
					scratch[out] = items[right];
					right++;
				}

				/* The next place. */
				out++;
			}
			while (left < middle) {
				scratch[out] = items[left];
				left++;
				out++;
			}
			while (right < end) {
				scratch[out] = items[right];
				right++;
				out++;
			}
		}

		/* The merged runs back in the items. */
		memcpy(items, scratch, (size_t)count * sizeof(vm_value));
	}

	/* Succeeded: the values are sorted. */
	return 0;
}

/*
 * Reads an object's elements into a new array (the present ones, or all
 * of them with holes as undefined) and sorts it; the arrays the sort
 * works in are heap objects, so the collector sees every value.
 */
static int
array_sorted_items(
	struct vm_realm *realm,
	vm_value object,
	uint32_t length,
	vm_value comparator,
	int skip_holes,
	vm_value *list,
	uint32_t *count)
{
	struct vm_object *items;
	struct vm_object *scratch;
	struct vm_cell *items_root;
	struct vm_cell *scratch_root;
	vm_value value;
	vm_value scratch_value;
	uint32_t index;
	int found;
	int status;

	/* The list of values. */
	*count = 0;
	status = array_new(realm, list);
	if (status != 0)
		return status;
	items = (struct vm_object *)vm_value_as_cell(*list);
	items_root = &items->cell;
	status = vm_heap_add_root(realm->heap, &items_root);
	if (status != 0)
		return status;
	for (index = 0; status == 0 && index < length; index++) {
		found = 1;
		if (skip_holes)
			status = array_has(realm, object, index, &found);
		if (status == 0 && found) {
			status = array_get(realm, object, index, &value);
			if (status == 0)
				status = array_append(realm, *list, value);
		}
	}

	/* A failure to read. */
	if (status != 0)
		goto release_items;
	*count = items->length;

	/* A scratch list as long, then the sort (the lists are dense: their elements are one array each). */
	status = array_new(realm, &scratch_value);
	if (status != 0)
		goto release_items;
	scratch = (struct vm_object *)vm_value_as_cell(scratch_value);
	scratch_root = &scratch->cell;
	status = vm_heap_add_root(realm->heap, &scratch_root);
	if (status != 0)
		goto release_items;
	if (*count > 0)
		status = vm_array_set_length(realm->heap, scratch, 0);
	for (index = 0; status == 0 && index < *count; index++)
		status = vm_object_define(realm->heap, scratch, vm_value_int32((int32_t)index), VM_VALUE_UNDEFINED, VM_PROPERTY_DEFAULT);
	if (status == 0 && *count > 1U)
		status = array_merge_sort(realm, comparator, items->elements, scratch->elements, *count);
	vm_heap_remove_root(realm->heap, &scratch_root);
release_items:
	vm_heap_remove_root(realm->heap, &items_root);
	return status;
}

/* Joins the elements' strings (undefined and null are empty) with a separator; with locale, each element's toLocaleString. */
static int
array_join_with(
	struct vm_realm *realm,
	vm_value object,
	uint32_t length,
	struct vm_string *separator,
	int locale,
	vm_value *result)
{
	struct wb_units text;
	struct vm_string *part;
	struct vm_string *joined;
	struct vm_cell *separator_root;
	struct vm_cell *value_root;
	vm_value value;
	vm_value key;
	vm_value method;
	uint32_t index;
	int is_cell;
	int status;

	/* The separator and current value survive reentrant string conversion. */
	separator_root = &separator->cell;
	status = vm_heap_add_root(realm->heap, &separator_root);
	if (status != 0)
		return status;
	value_root = NULL;
	status = vm_heap_add_root(realm->heap, &value_root);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &separator_root);
		return status;
	}

	/* Each element's string after the separator. */
	wb_units_init(&text);
	status = 0;
	for (index = 0; status == 0 && index < length; index++) {
		value_root = NULL;
		if (index > 0)
			status = vm_string_append_units(separator, &text);
		if (status == 0)
			status = array_get(realm, object, index, &value);
		if (status != 0 || value == VM_VALUE_UNDEFINED || value == VM_VALUE_NULL)
			continue;
		is_cell = vm_value_is_cell(value);
		if (is_cell)
			value_root = vm_value_as_cell(value);
		if (locale) {
			key = vm_key_from_ascii(realm->heap, "toLocaleString");
			if (key == VM_VALUE_EMPTY)
				status = ENOMEM;
			if (status == 0)
				status = vm_get(realm, value, key, &method);
			if (status == 0)
				status = vm_call(realm, method, value, NULL, 0, &value);
			if (status == 0) {
				value_root = NULL;
				is_cell = vm_value_is_cell(value);
				if (is_cell)
					value_root = vm_value_as_cell(value);
			}
		}

		/* Its string. */
		if (status == 0)
			status = vm_to_string(realm, value, &part);
		if (status == 0)
			status = vm_string_append_units(part, &text);
	}

	/* A failure leaves nothing. */
	if (status != 0) {
		wb_units_release(&text);
		vm_heap_remove_root(realm->heap, &value_root);
		vm_heap_remove_root(realm->heap, &separator_root);
		return status;
	}

	/* The text as a string. */
	joined = vm_string_from_units(realm->heap, text.data, text.length);
	wb_units_release(&text);
	vm_heap_remove_root(realm->heap, &value_root);
	vm_heap_remove_root(realm->heap, &separator_root);
	if (joined == NULL)
		return ENOMEM;
	*result = vm_value_cell(joined);
	return 0;
}
