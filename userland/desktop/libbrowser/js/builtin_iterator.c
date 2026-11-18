/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The built-in iterators (ws074-p087): %IteratorPrototype% (whose
 * Symbol.iterator returns the iterator itself), the iterators of arrays,
 * strings, Maps and Sets with their prototypes, and the methods that make
 * them: Array.prototype.keys, values, entries and Symbol.iterator, and
 * String.prototype[Symbol.iterator].  arguments objects share
 * Array.prototype.values (the interpreter gives them it).
 *
 * An iterator is an object of kind VM_KIND_ITERATOR whose internal value is
 * a cell of its state: what it walks (undefined once it ended), where it
 * is, what it gives (keys, values or entries) and which family's next may
 * move it.  Not in this pass: the iterator helpers (Iterator.prototype.map
 * and the others, Iterator.from).
 */

#include "js/builtin.h"

#include <errno.h>

/*
 * The state of one built-in iterator: its family, what it gives, what it
 * walks and the next index there (an element's, a string's code unit, or
 * a collection's entry).
 */
struct iterator_state {
	struct vm_cell cell;
	vm_value source;
	uint32_t family;
	uint32_t kind;
	uint32_t index;
	uint32_t reserved;
};

static void iterator_state_trace(struct vm_heap *heap, struct vm_cell *cell);
static int iterator_return_this(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_array_next(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_string_next(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_map_next(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_set_next(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_array_keys(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_array_values(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_array_entries(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_string_iterator(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_array_method(struct vm_realm *realm, vm_value this_value, uint32_t kind, vm_value *result);
static int iterator_state_of(struct vm_realm *realm, vm_value value, uint32_t family, struct iterator_state **state);
static int iterator_family_prototype(struct vm_realm *realm, struct vm_object *parent, const char *tag, vm_native next, struct vm_object **prototype);
static int iterator_entry(struct vm_realm *realm, vm_value key, vm_value value, vm_value *entry);

/* The cell type of an iterator's state: it keeps what it walks. */
static const struct vm_cell_type iterator_state_type = {
	"iterator state", iterator_state_trace, NULL
};

/*
 * Installs %IteratorPrototype%, the iterators' prototypes, and the methods
 * of Array.prototype and String.prototype that make iterators.
 */
int
js_builtin_install_iterator(
	struct vm_realm *realm)
{
	struct vm_function *function;
	struct vm_object *iterator_prototype;
	struct vm_object *prototype;
	vm_value values;
	int error;

	/* %IteratorPrototype%, whose Symbol.iterator gives the iterator itself. */
	iterator_prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (iterator_prototype == NULL)
		return ENOMEM;
	realm->intrinsics[VM_INTRINSIC_ITERATOR_PROTOTYPE] = iterator_prototype;
	error = js_builtin_function(realm, "[Symbol.iterator]", 0, iterator_return_this, NULL, &function);
	if (error == 0)
		error = js_builtin_symbol_value(realm, iterator_prototype, VM_SYMBOL_ITERATOR, vm_value_cell(function), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* The four families' prototypes. */
	error = iterator_family_prototype(realm, iterator_prototype, "Array Iterator", iterator_array_next, &prototype);
	if (error != 0)
		return error;
	realm->intrinsics[VM_INTRINSIC_ARRAY_ITERATOR_PROTOTYPE] = prototype;
	error = iterator_family_prototype(realm, iterator_prototype, "String Iterator", iterator_string_next, &prototype);
	if (error != 0)
		return error;
	realm->intrinsics[VM_INTRINSIC_STRING_ITERATOR_PROTOTYPE] = prototype;
	error = iterator_family_prototype(realm, iterator_prototype, "Map Iterator", iterator_map_next, &prototype);
	if (error != 0)
		return error;
	realm->intrinsics[VM_INTRINSIC_MAP_ITERATOR_PROTOTYPE] = prototype;
	error = iterator_family_prototype(realm, iterator_prototype, "Set Iterator", iterator_set_next, &prototype);
	if (error != 0)
		return error;
	realm->intrinsics[VM_INTRINSIC_SET_ITERATOR_PROTOTYPE] = prototype;

	/* String.prototype[Symbol.iterator], which the engine's own iteration of a string recognizes. */
	error = js_builtin_function(realm, "[Symbol.iterator]", 0, iterator_string_iterator, NULL, &function);
	if (error != 0)
		return error;
	error = js_builtin_symbol_value(realm, realm->intrinsics[VM_INTRINSIC_STRING_PROTOTYPE], VM_SYMBOL_ITERATOR, vm_value_cell(function),
	    JS_BUILTIN_METHOD);
	if (error != 0)
		return error;
	realm->intrinsics[VM_INTRINSIC_STRING_ITERATOR] = &function->object;

	/* Array.prototype's keys, values (also its Symbol.iterator, which arguments objects share) and entries. */
	error = js_builtin_method(realm, realm->array_prototype, "keys", 0, iterator_array_keys);
	if (error == 0)
		error = js_builtin_method(realm, realm->array_prototype, "entries", 0, iterator_array_entries);
	if (error != 0)
		return error;
	error = js_builtin_function(realm, "values", 0, iterator_array_values, NULL, &function);
	if (error != 0)
		return error;
	values = vm_value_cell(function);
	error = js_builtin_value(realm, realm->array_prototype, "values", values, JS_BUILTIN_METHOD);
	if (error == 0)
		error = js_builtin_symbol_value(realm, realm->array_prototype, VM_SYMBOL_ITERATOR, values, JS_BUILTIN_METHOD);
	if (error != 0)
		return error;
	realm->intrinsics[VM_INTRINSIC_ARRAY_VALUES] = &function->object;

	/* Succeeded: the iterators are installed. */
	return 0;
}

/*
 * Makes a built-in iterator of a family over a source, giving keys,
 * values or entries (JS_ITERATE_*).
 */
int
js_iterator_create(
	struct vm_realm *realm,
	uint32_t family,
	vm_value source,
	uint32_t kind,
	vm_value *result)
{
	struct iterator_state *state;
	struct vm_object *prototype;
	struct vm_object *object;

	/* The family's prototype. */
	*result = VM_VALUE_UNDEFINED;
	prototype = realm->intrinsics[VM_INTRINSIC_ARRAY_ITERATOR_PROTOTYPE];
	if (family == JS_ITERATOR_STRING)
		prototype = realm->intrinsics[VM_INTRINSIC_STRING_ITERATOR_PROTOTYPE];
	if (family == JS_ITERATOR_MAP)
		prototype = realm->intrinsics[VM_INTRINSIC_MAP_ITERATOR_PROTOTYPE];
	if (family == JS_ITERATOR_SET)
		prototype = realm->intrinsics[VM_INTRINSIC_SET_ITERATOR_PROTOTYPE];

	/* The state, from the start. */
	state = vm_heap_alloc(realm->heap, &iterator_state_type, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	state->source = source;
	state->family = family;
	state->kind = kind;
	state->index = 0;

	/* The iterator object holding it. */
	object = vm_object_create(realm->heap, prototype);
	if (object == NULL)
		return ENOMEM;
	object->kind = VM_KIND_ITERATOR;
	object->internal = vm_value_cell(state);

	/* Succeeded: the iterator. */
	*result = vm_value_cell(object);
	return 0;
}

/*
 * Makes an iterator result object: { value, done }.
 */
int
js_iterator_result(
	struct vm_realm *realm,
	vm_value value,
	int done,
	vm_value *result)
{
	struct vm_object *object;
	int error;

	/* The object with its two properties. */
	object = vm_object_create(realm->heap, realm->object_prototype);
	if (object == NULL)
		return ENOMEM;
	error = js_builtin_value(realm, object, "value", value, VM_PROPERTY_DEFAULT);
	if (error == 0)
		error = js_builtin_value(realm, object, "done", vm_value_boolean(done), VM_PROPERTY_DEFAULT);
	if (error != 0)
		return error;

	/* Succeeded: the result. */
	*result = vm_value_cell(object);
	return 0;
}

/* Marks what an iterator walks. */
static void
iterator_state_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	/* The source. */
	vm_heap_mark_value(heap, ((struct iterator_state *)cell)->source);
}

/* %IteratorPrototype%[Symbol.iterator](): the iterator itself. */
static int
iterator_return_this(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Succeeded: this. */
	*result = this_value;
	return 0;
}

/* %ArrayIteratorPrototype%.next(): the next index, element or entry of an array-like object. */
static int
iterator_array_next(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct iterator_state *state;
	vm_value index;
	vm_value value;
	uint32_t length;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The state; an iterator that ended stays ended. */
	*result = VM_VALUE_UNDEFINED;
	status = iterator_state_of(realm, this_value, JS_ITERATOR_ARRAY, &state);
	if (status != 0)
		return status;
	if (state->source == VM_VALUE_UNDEFINED) {
		status = js_iterator_result(realm, VM_VALUE_UNDEFINED, 1, result);
		return status;
	}

	/* The length, read at each step; past it the iterator ends. */
	status = js_builtin_length(realm, state->source, &length);
	if (status != 0)
		return status;
	if (state->index >= length) {
		state->source = VM_VALUE_UNDEFINED;
		status = js_iterator_result(realm, VM_VALUE_UNDEFINED, 1, result);
		return status;
	}

	/* The index, the element, or both. */
	index = vm_value_int32((int32_t)state->index);
	state->index++;
	value = index;
	if (state->kind != JS_ITERATE_KEYS) {
		status = vm_get(realm, state->source, index, &value);
		if (status != 0)
			return status;
	}

	/* An entry pairs the index with the element. */
	if (state->kind == JS_ITERATE_ENTRIES) {
		status = iterator_entry(realm, index, value, &value);
		if (status != 0)
			return status;
	}

	/* Succeeded: the result. */
	status = js_iterator_result(realm, value, 0, result);
	return status;
}

/* %StringIteratorPrototype%.next(): the next code point of a string. */
static int
iterator_string_next(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct iterator_state *state;
	struct vm_string *string;
	uint16_t units[2];
	uint32_t length;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The state; an iterator at the end ends. */
	*result = VM_VALUE_UNDEFINED;
	status = iterator_state_of(realm, this_value, JS_ITERATOR_STRING, &state);
	if (status != 0)
		return status;
	string = NULL;
	if (state->source != VM_VALUE_UNDEFINED)
		string = (struct vm_string *)vm_value_as_cell(state->source);
	if (string == NULL || state->index >= string->length) {
		state->source = VM_VALUE_UNDEFINED;
		status = js_iterator_result(realm, VM_VALUE_UNDEFINED, 1, result);
		return status;
	}

	/* The first unit, and a trailing surrogate that pairs with it. */
	units[0] = vm_string_at(string, state->index);
	length = 1;
	if (units[0] >= 0xD800U && units[0] <= 0xDBFFU && state->index + 1U < string->length) {
		units[1] = vm_string_at(string, state->index + 1U);
		if (units[1] >= 0xDC00U && units[1] <= 0xDFFFU)
			length = 2;
	}

	/* The iterator moves past the code point. */
	state->index += length;

	/* The code point's string. */
	string = vm_string_from_units(realm->heap, units, length);
	if (string == NULL)
		return ENOMEM;

	/* Succeeded: the result. */
	status = js_iterator_result(realm, vm_value_cell(string), 0, result);
	return status;
}

/* %MapIteratorPrototype%.next(): the next key, value or entry of a Map. */
static int
iterator_map_next(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct iterator_state *state;
	vm_value key;
	vm_value value;
	int found;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The state and the next entry; none left ends the iterator. */
	*result = VM_VALUE_UNDEFINED;
	status = iterator_state_of(realm, this_value, JS_ITERATOR_MAP, &state);
	if (status != 0)
		return status;
	found = 0;
	if (state->source != VM_VALUE_UNDEFINED)
		found = js_collection_step(state->source, &state->index, &key, &value);
	if (!found) {
		state->source = VM_VALUE_UNDEFINED;
		status = js_iterator_result(realm, VM_VALUE_UNDEFINED, 1, result);
		return status;
	}

	/* The key, the value, or both. */
	if (state->kind == JS_ITERATE_KEYS)
		value = key;
	if (state->kind == JS_ITERATE_ENTRIES) {
		status = iterator_entry(realm, key, value, &value);
		if (status != 0)
			return status;
	}

	/* Succeeded: the result. */
	status = js_iterator_result(realm, value, 0, result);
	return status;
}

/* %SetIteratorPrototype%.next(): the next value (or [value, value]) of a Set. */
static int
iterator_set_next(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct iterator_state *state;
	vm_value key;
	vm_value value;
	int found;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The state and the next entry; none left ends the iterator. */
	*result = VM_VALUE_UNDEFINED;
	status = iterator_state_of(realm, this_value, JS_ITERATOR_SET, &state);
	if (status != 0)
		return status;
	found = 0;
	if (state->source != VM_VALUE_UNDEFINED)
		found = js_collection_step(state->source, &state->index, &key, &value);
	if (!found) {
		state->source = VM_VALUE_UNDEFINED;
		status = js_iterator_result(realm, VM_VALUE_UNDEFINED, 1, result);
		return status;
	}

	/* The value, or the entry of the value twice. */
	value = key;
	if (state->kind == JS_ITERATE_ENTRIES) {
		status = iterator_entry(realm, key, key, &value);
		if (status != 0)
			return status;
	}

	/* Succeeded: the result. */
	status = js_iterator_result(realm, value, 0, result);
	return status;
}

/* Array.prototype.keys(): an iterator of the indices. */
static int
iterator_array_keys(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The iterator. */
	status = iterator_array_method(realm, this_value, JS_ITERATE_KEYS, result);
	return status;
}

/* Array.prototype.values() and [Symbol.iterator](): an iterator of the elements. */
static int
iterator_array_values(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The iterator. */
	status = iterator_array_method(realm, this_value, JS_ITERATE_VALUES, result);
	return status;
}

/* Array.prototype.entries(): an iterator of [index, element]. */
static int
iterator_array_entries(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The iterator. */
	status = iterator_array_method(realm, this_value, JS_ITERATE_ENTRIES, result);
	return status;
}

/* String.prototype[Symbol.iterator](): an iterator of the code points of this as a string. */
static int
iterator_string_iterator(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* this must not be undefined or null; its string. */
	*result = VM_VALUE_UNDEFINED;
	if (this_value == VM_VALUE_UNDEFINED || this_value == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "String.prototype[Symbol.iterator] called on null or undefined");
		return status;
	}

	/* this as a string. */
	status = vm_to_string(realm, this_value, &string);
	if (status != 0)
		return status;

	/* The iterator. */
	status = js_iterator_create(realm, JS_ITERATOR_STRING, vm_value_cell(string), JS_ITERATE_VALUES, result);
	return status;
}

/* Makes an array iterator of this as an object. */
static int
iterator_array_method(
	struct vm_realm *realm,
	vm_value this_value,
	uint32_t kind,
	vm_value *result)
{
	vm_value object;
	int status;

	/* this as an object. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_to_object(realm, this_value, &object);
	if (status != 0)
		return status;

	/* The iterator. */
	status = js_iterator_create(realm, JS_ITERATOR_ARRAY, object, kind, result);
	return status;
}

/* Finds the state of an iterator of a family (a TypeError for any other value). */
static int
iterator_state_of(
	struct vm_realm *realm,
	vm_value value,
	uint32_t family,
	struct iterator_state **state)
{
	struct vm_object *object;
	int is_object;
	int status;

	/* An iterator object of the family. */
	*state = NULL;
	is_object = vm_value_is_object(value);
	if (is_object) {
		object = (struct vm_object *)vm_value_as_cell(value);
		if (object->kind == VM_KIND_ITERATOR) {
			*state = (struct iterator_state *)vm_value_as_cell(object->internal);
			if ((*state)->family == family)
				return 0;
		}
	}

	/* Anything else. */
	*state = NULL;
	status = vm_throw_type_error(realm, "next method called on incompatible receiver");
	return status;
}

/* Makes an iterator family's prototype: from the parent, with its next and its toStringTag. */
static int
iterator_family_prototype(
	struct vm_realm *realm,
	struct vm_object *parent,
	const char *tag,
	vm_native next,
	struct vm_object **prototype)
{
	struct vm_object *made;
	int error;

	/* The object. */
	made = vm_object_create(realm->heap, parent);
	if (made == NULL)
		return ENOMEM;

	/* next and the tag. */
	error = js_builtin_method(realm, made, "next", 0, next);
	if (error == 0)
		error = js_builtin_tag(realm, made, tag);
	if (error != 0)
		return error;

	/* Succeeded: the prototype. */
	*prototype = made;
	return 0;
}

/* Makes an entry array [key, value]. */
static int
iterator_entry(
	struct vm_realm *realm,
	vm_value key,
	vm_value value,
	vm_value *entry)
{
	vm_value pair[2];
	int status;

	/* The two, in an array. */
	pair[0] = key;
	pair[1] = value;
	status = js_builtin_array(realm, pair, 2, entry);
	if (status != 0)
		return status;

	/* Succeeded: the entry. */
	return 0;
}
