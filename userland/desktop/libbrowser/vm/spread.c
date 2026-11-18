/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Spreading and destructuring: iterating a value for an
 * array pattern or a spread, copying an object's own enumerable properties
 * for an object spread and an object pattern's rest, and calling with the
 * elements of an array as the arguments.
 *
 * Iteration follows the iterator protocol: the value's
 * Symbol.iterator method makes an iterator whose next is called for each
 * value, and a loop left early calls its return.  An array, an arguments
 * object and a string whose Symbol.iterator is still the built-in one are
 * iterated directly instead (their elements by index, the length read at
 * each step; a string by code point), which is what the built-in iterator
 * would do.
 */

#include "vm/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most arguments a call with a spread passes. */
#define SPREAD_ARGUMENTS_MAX	65535U

/*
 * The state of one iteration of a value: the value, the next index (an
 * element's, or a string's code unit), and whether it has ended.  An
 * iteration by the protocol (protocol set) keeps the iterator and its next
 * method instead.  It is a cell so that a register can hold it while the
 * code between steps runs.
 */
struct spread_iterator {
	struct vm_cell cell;
	vm_value source;
	vm_value iterator;
	vm_value next;
	uint32_t index;
	int done;
	int protocol;
	int reserved;
};

/* The native argument buffer and the count already returned by indexed getters. */
struct spread_call_args {
	vm_value *values;
	uint32_t filled;
};

static void spread_iterator_trace(struct vm_heap *heap, struct vm_cell *cell);
static void spread_call_args_trace(struct vm_heap *heap, void *context);
static int spread_length(struct vm_realm *realm, vm_value source, uint32_t *length);
static int spread_iterable(vm_value value);
static int spread_is_builtin(struct vm_realm *realm, vm_value value, vm_value method);
static int spread_protocol_next(struct vm_realm *realm, struct spread_iterator *state, vm_value *value, int *done);
static int spread_is_excluded(struct vm_realm *realm, vm_value excluded, vm_value key, int *found);

/* The cell type of an iteration's state: it keeps the value it iterates. */
static const struct vm_cell_type spread_iterator_type = {
	"iterator", spread_iterator_trace, NULL
};

static int spread_copy(struct vm_realm *realm, struct vm_object *target, vm_value source, vm_value excluded);
static void spread_trace_keys(struct vm_heap *heap, void *context);

/*
 * Gets the iterator of a for await loop (GetIterator with the async kind):
 * the value's Symbol.asyncIterator method's iterator, or else its
 * Symbol.iterator method's one with *sync set (the loop then awaits each
 * value itself, as CreateAsyncFromSyncIterator would).  A TypeError for a
 * value with neither method, or a method that makes no object.
 */
int
vm_async_iter_start(
	struct vm_realm *realm,
	vm_value value,
	vm_value *iterator,
	int *sync)
{
	struct vm_cell *roots[3];
	vm_value method;
	vm_value made;
	unsigned index;
	unsigned registered;
	int is_cell;
	int is_object;
	int status;

	/* Retain the collectible realm, the value and the method while the methods run. */
	*iterator = VM_VALUE_UNDEFINED;
	*sync = 0;
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = NULL;
	is_cell = vm_value_is_cell(value);
	if (is_cell)
		roots[1] = vm_value_as_cell(value);
	roots[2] = NULL;
	registered = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns this slot only after registration succeeds. */
		registered++;
	}

	/* The asynchronous method, else the synchronous one (undefined and null have neither). */
	method = VM_VALUE_UNDEFINED;
	if (value != VM_VALUE_UNDEFINED && value != VM_VALUE_NULL) {
		status = vm_get_method(realm, value, vm_symbol_key(realm, VM_SYMBOL_ASYNC_ITERATOR), &method);
		if (status != 0)
			goto cleanup;
		if (method == VM_VALUE_UNDEFINED) {
			status = vm_get_method(realm, value, vm_symbol_key(realm, VM_SYMBOL_ITERATOR), &method);
			if (status != 0)
				goto cleanup;
			*sync = 1;
		}
	}
	if (method == VM_VALUE_UNDEFINED) {
		status = vm_throw_type_error(realm, "value is not async iterable");
		goto cleanup;
	}
	is_cell = vm_value_is_cell(method);
	if (is_cell)
		roots[2] = vm_value_as_cell(method);

	/* The iterator, which must be an object. */
	status = vm_call(realm, method, value, NULL, 0, &made);
	if (status != 0)
		goto cleanup;
	is_object = vm_value_is_object(made);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Result of the Symbol.asyncIterator method is not an object");
		goto cleanup;
	}
	*iterator = made;

cleanup:
	/* Remove only acquired registrations before local slots expire. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}
	return status;
}

/*
 * Starts iterating a value (an array pattern's or a spread's): the state
 * in a new cell, or a TypeError for a value that cannot be iterated.
 */
int
vm_iter_start(
	struct vm_realm *realm,
	vm_value value,
	vm_value *iterator)
{
	struct spread_iterator *state;
	struct vm_cell *roots[4];
	vm_value method;
	vm_value made;
	vm_value key;
	unsigned index;
	unsigned registered;
	int builtin;
	int callable;
	int is_cell;
	int is_object;
	int status;

	/* Retain the collectible realm, direct input, method and unpublished state. */
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = NULL;
	is_cell = vm_value_is_cell(value);
	if (is_cell)
		roots[1] = vm_value_as_cell(value);
	roots[2] = NULL;
	roots[3] = NULL;
	registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns this slot only after registration succeeds. */
		registered++;
	}

	/* The value's Symbol.iterator method (undefined and null have none). */
	method = VM_VALUE_UNDEFINED;
	if (value != VM_VALUE_UNDEFINED && value != VM_VALUE_NULL) {
		status = vm_get(realm, value, vm_symbol_key(realm, VM_SYMBOL_ITERATOR), &method);
		if (status != 0)
			goto cleanup;
	}

	/* Keep a method returned by a getter alive during state allocation. */
	is_cell = vm_value_is_cell(method);
	if (is_cell)
		roots[2] = vm_value_as_cell(method);

	/* The state, from the first element. */
	state = vm_heap_alloc(realm->heap, &spread_iterator_type, sizeof(*state));
	if (state == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Protect the newborn before any user iterator method can run. */
	roots[3] = &state->cell;
	state->source = value;
	state->iterator = VM_VALUE_UNDEFINED;
	state->next = VM_VALUE_UNDEFINED;
	state->index = 0;
	state->done = 0;
	state->protocol = 0;

	/* A value iterated by the built-in iteration needs nothing more. */
	builtin = spread_is_builtin(realm, value, method);
	if (builtin) {
		*iterator = vm_value_cell(state);
		status = 0;
		goto cleanup;
	}

	/* Anything else needs a method to make its iterator. */
	callable = vm_value_is_callable(method);
	if (!callable) {
		status = vm_throw_type_error(realm, "value is not iterable");
		goto cleanup;
	}

	/* The iterator, which must be an object. */
	status = vm_call(realm, method, value, NULL, 0, &made);
	if (status != 0)
		goto cleanup;
	state->iterator = made;
	is_object = vm_value_is_object(made);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Result of the Symbol.iterator method is not an object");
		goto cleanup;
	}

	/* Its next method, read once. */
	key = vm_key_from_ascii(realm->heap, "next");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The state owns the returned iterator before reading its next method. */
	state->protocol = 1;
	status = vm_get(realm, made, key, &state->next);
	if (status != 0)
		goto cleanup;

	/* Succeeded: the iteration's state. */
	*iterator = vm_value_cell(state);

cleanup:
	/* Remove only acquired registrations before local slots expire. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A failed iterator construction never publishes its private state. */
	if (status != 0)
		return status;

	/* Succeeded: the caller owns the complete iterator state. */
	return 0;
}

/*
 * Takes the next value of an iteration; undefined once it has ended (an
 * array pattern's element past the end).  *done says whether it had.
 */
int
vm_iter_next(
	struct vm_realm *realm,
	vm_value iterator,
	vm_value *value,
	int *done)
{
	struct spread_iterator *state;
	struct vm_cell *roots[2];
	struct vm_string *string;
	uint16_t units[2];
	uint32_t length;
	unsigned index;
	unsigned registered;
	int is_string;
	int status;

	/* An ended iteration stays ended. */
	state = (struct spread_iterator *)vm_value_as_cell(iterator);
	*value = VM_VALUE_UNDEFINED;
	*done = 1;
	if (state->done)
		return 0;

	/* An iteration by the protocol calls the iterator's next. */
	if (state->protocol) {
		status = spread_protocol_next(realm, state, value, done);
		if (status != 0)
			return status;
		return 0;
	}

	/* Direct built-in iterators survive length getters and string allocation. */
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &state->cell;
	registered = 0;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Only successfully registered slots are released below. */
		registered++;
	}

	/* A string: the next code point (a surrogate pair together). */
	is_string = vm_value_is_string(state->source);
	if (is_string) {
		string = (struct vm_string *)vm_value_as_cell(state->source);
		if (state->index >= string->length) {
			state->done = 1;
			status = 0;
			goto cleanup;
		}

		/* The first unit, and a trailing surrogate that pairs with it. */
		units[0] = vm_string_at(string, state->index);
		length = 1;
		if (units[0] >= 0xD800U && units[0] <= 0xDBFFU && state->index + 1U < string->length) {
			units[1] = vm_string_at(string, state->index + 1U);
			if (units[1] >= 0xDC00U && units[1] <= 0xDFFFU)
				length = 2;
		}

		/* The code point's string. */
		string = vm_string_from_units(realm->heap, units, length);
		if (string == NULL) {
			status = ENOMEM;
			goto cleanup;
		}

		/* Advance past the complete code point after allocation succeeds. */
		state->index += length;
		*value = vm_value_cell(string);
		*done = 0;
		status = 0;
		goto cleanup;
	}

	/* An array or an arguments object: the element at the index, while it is below the length. */
	status = spread_length(realm, state->source, &length);
	if (status != 0)
		goto cleanup;
	if (state->index >= length) {
		state->done = 1;
		status = 0;
		goto cleanup;
	}

	/* The element at the index. */
	status = vm_get(realm, state->source, vm_value_int32((int32_t)state->index), value);
	if (status != 0)
		goto cleanup;

	/* Succeeded: the next value. */
	state->index++;
	*done = 0;
	status = 0;

cleanup:
	/* Release direct iterator ownership after the last callback or allocation. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Report failure only after the root registry is restored. */
	if (status != 0)
		return status;

	/* Succeeded: the caller observes a value or an exhausted iterator. */
	return 0;
}

/*
 * Closes an iteration left before its end (IteratorClose): an iterator of
 * the protocol has its return method called.  quiet is for a loop left by
 * an exception, which keeps its own: whatever return does is ignored.
 * Returns 0, VM_THROWN or an errno value.
 */
int
vm_iter_close(
	struct vm_realm *realm,
	vm_value iterator,
	int quiet)
{
	struct spread_iterator *state;
	struct vm_cell *roots[2];
	vm_value saved;
	vm_value method;
	vm_value result;
	vm_value key;
	unsigned index;
	unsigned registered;
	int is_object;
	int status;

	/* Retain the direct iterator and collectible realm while return runs. */
	state = (struct spread_iterator *)vm_value_as_cell(iterator);
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &state->cell;
	registered = 0;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns only the acquired registry slot. */
		registered++;
	}

	/* An iteration that ended, or a built-in one, has nothing to close. */
	if (state->done || !state->protocol) {
		state->done = 1;
		status = 0;
		goto cleanup;
	}

	/* Closed from now on, whatever return does. */
	state->done = 1;

	/* The iterator's return method, and its call when there is one. */
	saved = realm->exception;
	result = VM_VALUE_UNDEFINED;
	key = vm_key_from_ascii(realm->heap, "return");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Read and invoke the iterator return method with direct ownership held. */
	status = vm_get_method(realm, state->iterator, key, &method);
	if (status == 0 && method != VM_VALUE_UNDEFINED)
		status = vm_call(realm, method, state->iterator, NULL, 0, &result);

	/* Leaving by an exception: that exception stays, and what return did does not matter. */
	if (quiet && (status == 0 || status == VM_THROWN)) {
		realm->exception = saved;
		status = 0;
		goto cleanup;
	}

	/* A failure of return, or of finding it. */
	if (status != 0)
		goto cleanup;

	/* Without a method there is nothing to check. */
	if (method == VM_VALUE_UNDEFINED)
		goto cleanup;

	/* What return gave must be an object. */
	is_object = vm_value_is_object(result);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Iterator result is not an object");
		goto cleanup;
	}

	/* The return method completed with a valid object. */
	status = 0;

cleanup:
	/* Release the direct iterator after every exit, including a quiet close. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* The caller observes return's error except when quiet preserves its own. */
	if (status != 0)
		return status;

	/* Succeeded: the iterator is closed. */
	return 0;
}

/*
 * Makes an array of the remaining values in an iteration.
 * The callee retains its unpublished output across native getters and iterator callbacks.
 */
int
vm_iter_rest(
	struct vm_realm *realm,
	vm_value iterator,
	vm_value *array)
{
	struct vm_object *rest;
	struct vm_cell *roots[4];
	vm_value value;
	unsigned index;
	unsigned registered;
	int is_cell;
	int done;
	int status;

	/* Retain the collectible realm and direct iterator before the first VM allocation. */
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = NULL;
	is_cell = vm_value_is_cell(iterator);
	if (is_cell)
		roots[1] = vm_value_as_cell(iterator);
	roots[2] = NULL;
	roots[3] = NULL;
	registered = 0;

	/* Register private publication and element slots before callbacks can invoke collection. */
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns only slots whose registry allocation actually succeeded. */
		registered++;
	}

	/* Allocate the unpublished result while its caller inputs remain precisely owned. */
	rest = vm_array_create(realm->heap, realm->array_prototype);
	if (rest == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The callee keeps its complete private array alive until result publication. */
	roots[2] = &rest->cell;

	/* Append each real iterator value after its callback has successfully completed. */
	for (;;) {
		status = vm_iter_next(realm, iterator, &value, &done);
		if (status != 0)
			goto cleanup;

		/* The ordinary iterator's done flag ends this rest construction. */
		if (done)
			break;

		/* A temporary returned cell stays owned while its array property is allocated. */
		roots[3] = NULL;
		is_cell = vm_value_is_cell(value);
		if (is_cell)
			roots[3] = vm_value_as_cell(value);
		status = vm_object_define(realm->heap, rest, vm_value_int32((int32_t)rest->length), value, VM_PROPERTY_DEFAULT);
		if (status != 0)
			goto cleanup;
	}

	/* Publish only the complete result before releasing callee construction ownership. */
	*array = vm_value_cell(rest);

cleanup:
	/* Remove only acquired registry pointers before their local slots expire. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Propagate a failed iterator step or property publication. */
	if (status != 0)
		return status;

	/* Succeeded: the caller receives the complete precisely retained rest array. */
	return 0;
}

/*
 * Appends every value of an iterable to an array (a spread in an array
 * literal or in a call's arguments).
 */
int
vm_array_spread(
	struct vm_realm *realm,
	vm_value array,
	vm_value value)
{
	struct vm_object *target;
	struct vm_cell *roots[5];
	vm_value iterator;
	vm_value element;
	unsigned index;
	unsigned registered;
	int is_cell;
	int done;
	int status;

	/* Hold both direct inputs and later iterator/element across user callbacks. */
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = NULL;
	is_cell = vm_value_is_cell(array);
	if (is_cell)
		roots[1] = vm_value_as_cell(array);
	roots[2] = NULL;
	is_cell = vm_value_is_cell(value);
	if (is_cell)
		roots[2] = vm_value_as_cell(value);
	roots[3] = NULL;
	roots[4] = NULL;
	registered = 0;
	for (index = 0; index < 5U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Only successfully inserted slots belong to cleanup. */
		registered++;
	}

	/* The iteration of the value. */
	status = vm_iter_start(realm, value, &iterator);
	if (status != 0)
		goto cleanup;
	roots[3] = vm_value_as_cell(iterator);

	/* Each value at the end of the array. */
	target = (struct vm_object *)vm_value_as_cell(array);
	for (;;) {
		status = vm_iter_next(realm, iterator, &element, &done);
		if (status != 0)
			goto cleanup;
		if (done)
			break;
		roots[4] = NULL;
		is_cell = vm_value_is_cell(element);
		if (is_cell)
			roots[4] = vm_value_as_cell(element);
		status = vm_object_define(realm->heap, target, vm_value_int32((int32_t)target->length), element, VM_PROPERTY_DEFAULT);
		if (status != 0)
			goto cleanup;
	}

	/* The target now owns every copied element. */
	status = 0;

cleanup:
	/* Release precisely the roots acquired by this spread. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A failed iterator or property write cannot report complete spread. */
	if (status != 0)
		return status;

	/* Succeeded: the values are appended. */
	return 0;
}

/*
 * Copies the own enumerable properties of a value to an object (an object
 * literal's spread); undefined and null copy nothing.
 */
int
vm_copy_data_properties(
	struct vm_realm *realm,
	vm_value target,
	vm_value source)
{
	int status;

	/* Every property, none excluded. */
	status = spread_copy(realm, (struct vm_object *)vm_value_as_cell(target), source, VM_VALUE_UNDEFINED);
	if (status != 0)
		return status;

	/* Succeeded: the properties are copied. */
	return 0;
}

/*
 * Makes the object of an object pattern's rest: a new object with the own
 * enumerable properties of the source but those whose keys the array
 * excluded lists (the keys the pattern took).
 */
int
vm_object_rest(
	struct vm_realm *realm,
	vm_value source,
	vm_value excluded,
	vm_value *result)
{
	struct vm_object *rest;
	struct vm_cell *roots[4];
	unsigned index;
	unsigned registered;
	int is_cell;
	int status;

	/* A direct native caller need not retain the realm or input cells before the first allocation. */
	memset(roots, 0, sizeof(roots));
	if (realm->managed)
		roots[0] = &realm->cell;
	is_cell = vm_value_is_cell(source);
	if (is_cell)
		roots[1] = vm_value_as_cell(source);
	is_cell = vm_value_is_cell(excluded);
	if (is_cell)
		roots[2] = vm_value_as_cell(excluded);
	registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns this registry slot only after acquisition succeeds. */
		registered++;
	}

	/* The unpublished result acquires its registered slot before any source trap can run. */
	rest = vm_object_create(realm->heap, realm->object_prototype);
	if (rest == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Keep the unpublished result alive while exclusions and source accessors run. */
	roots[3] = &rest->cell;

	/* Exclusions and source properties follow the shared CopyDataProperties order. */
	status = spread_copy(realm, rest, source, excluded);
	if (status != 0)
		goto cleanup;
	*result = vm_value_cell(rest);

	/* Both partial registration and abrupt source operations retire exactly their acquired roots. */
cleanup:
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve abrupt property or root-registration errors after releasing native ownership. */
	if (status != 0)
		return status;

	/* Succeeded: the caller owns the completed object rest value. */
	return 0;
}

/*
 * Calls a function (or, with construct, constructs with it as new.target)
 * with the elements of an array as the arguments: a call with a spread.
 */
int
vm_call_array(
	struct vm_realm *realm,
	vm_value function,
	vm_value this_value,
	vm_value array,
	int construct,
	vm_value *result)
{
	struct vm_object *list;
	struct vm_cell *roots[4];
	struct spread_call_args held;
	vm_value *args;
	uint32_t count;
	uint32_t index;
	unsigned registered;
	int callable;
	int is_cell;
	int tracer_registered;
	int status;

	/* A value that is not a function cannot be called. */
	callable = vm_value_is_callable(function);
	if (!callable) {
		status = vm_throw_type_error(realm, "value is not a function");
		return status;
	}

	/* How many arguments; too many is the error a call too deep gives. */
	list = (struct vm_object *)vm_value_as_cell(array);
	count = list->length;
	if (count > SPREAD_ARGUMENTS_MAX) {
		status = vm_throw_range_error(realm, "Maximum call stack size exceeded");
		return status;
	}

	/* Allocate native argument storage for the fixed-length call. */
	args = calloc((size_t)count + 1U, sizeof(vm_value));
	if (args == NULL)
		return ENOMEM;

	/* Retain direct inputs before indexed getters or the call can collect. */
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = NULL;
	is_cell = vm_value_is_cell(function);
	if (is_cell)
		roots[1] = vm_value_as_cell(function);
	roots[2] = NULL;
	is_cell = vm_value_is_cell(this_value);
	if (is_cell)
		roots[2] = vm_value_as_cell(this_value);
	roots[3] = NULL;
	is_cell = vm_value_is_cell(array);
	if (is_cell)
		roots[3] = vm_value_as_cell(array);
	registered = 0;
	tracer_registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Only acquired root slots can be removed during cleanup. */
		registered++;
	}

	/* One tracer marks every argument already returned by an indexed getter. */
	held.values = args;
	held.filled = 0;
	status = vm_heap_add_tracer(realm->heap, spread_call_args_trace, &held);
	if (status != 0)
		goto cleanup;
	tracer_registered = 1;

	/* Publish each successful native argument before the next getter can collect. */
	for (index = 0; index < count; index++) {
		status = vm_get(realm, array, vm_value_int32((int32_t)index), &args[index]);
		if (status != 0)
			goto cleanup;
		held.filled = index + 1U;
	}

	/* The call, or the construction. */
	if (construct) {
		status = vm_construct(realm, function, args, count, function, result);
	} else {
		status = vm_call(realm, function, this_value, args, count, result);
	}

	/* Release the direct inputs and every argument on all outcomes. */
cleanup:
	/* The tracer must stop before its native argument buffer is freed. */
	if (tracer_registered)
		vm_heap_remove_tracer(realm->heap, spread_call_args_trace, &held);

	/* Release direct input roots after the last call or getter has returned. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* No collector callback retains the native array after cleanup. */
	free(args);

	/* A getter or invocation error prevents successful publication. */
	if (status != 0)
		return status;

	/* Succeeded: the call's result. */
	return 0;
}

/* Marks the value an iteration's state iterates. */
static void
spread_iterator_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct spread_iterator *state;

	/* The value, and the iterator and its next method. */
	state = (struct spread_iterator *)cell;
	vm_heap_mark_value(heap, state->source);
	vm_heap_mark_value(heap, state->iterator);
	vm_heap_mark_value(heap, state->next);
}

/* Marks argument values already returned by array indexed getters. */
static void
spread_call_args_trace(
	struct vm_heap *heap,
	void *context)
{
	struct spread_call_args *held;
	uint32_t index;

	/* Only initialized arguments in the native buffer are live. */
	held = context;
	for (index = 0; index < held->filled; index++)
		vm_heap_mark_value(heap, held->values[index]);
}

/* Reads the length of an array or an arguments object. */
static int
spread_length(
	struct vm_realm *realm,
	vm_value source,
	uint32_t *length)
{
	struct vm_object *object;
	vm_value key;
	vm_value value;
	double number;
	int status;

	/* An array's length is its own. */
	object = (struct vm_object *)vm_value_as_cell(source);
	if (object->kind == VM_KIND_ARRAY) {
		*length = object->length;
		return 0;
	}

	/* An arguments object's is its length property. */
	key = vm_key_from_ascii(realm->heap, "length");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_get(realm, source, key, &value);
	if (status != 0)
		return status;
	status = vm_to_number(realm, value, &number);
	if (status != 0)
		return status;

	/* A length that is not a whole number within range is cut to one. */
	*length = 0;
	if (number > 0.0 && number < 4294967295.0)
		*length = (uint32_t)number;

	/* Succeeded: the length. */
	return 0;
}

/* Tells whether a value has a built-in iteration: an array, an arguments object or a string. */
static int
spread_iterable(
	vm_value value)
{
	struct vm_object *object;
	int is_string;
	int is_object;

	/* A string. */
	is_string = vm_value_is_string(value);
	if (is_string)
		return 1;

	/* Only objects are left. */
	is_object = vm_value_is_object(value);
	if (!is_object)
		return 0;

	/* An array or an arguments object. */
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind == VM_KIND_ARRAY)
		return 1;
	if (object->kind == VM_KIND_ARGUMENTS)
		return 1;

	/* Nothing else in this pass. */
	return 0;
}

/*
 * Copies the own enumerable properties of a source to a target, but those
 * whose keys the array excluded lists (undefined for none); undefined and
 * null copy nothing, and a primitive copies its object's.
 */
static int
spread_copy(
	struct vm_realm *realm,
	struct vm_object *target,
	vm_value source,
	vm_value excluded)
{
	struct vm_descriptor descriptor;
	struct vm_object *object;
	struct vm_cell *roots[5];
	struct wb_vector keys;
	vm_value key;
	vm_value value;
	size_t index;
	unsigned slot;
	unsigned registered;
	int is_cell;
	int traced;
	int present;
	int found;
	int accepted;
	int status;

	/* Nullish sources have no keys and perform no allocation or target operation. */
	if (source == VM_VALUE_UNDEFINED || source == VM_VALUE_NULL)
		return 0;

	/* Empty vector storage and initialized slots make every partial acquisition share one cleanup. */
	wb_vector_init(&keys, sizeof(vm_value));
	memset(roots, 0, sizeof(roots));
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &target->cell;
	is_cell = vm_value_is_cell(source);
	if (is_cell)
		roots[2] = vm_value_as_cell(source);
	is_cell = vm_value_is_cell(excluded);
	if (is_cell)
		roots[3] = vm_value_as_cell(excluded);
	registered = 0;
	traced = 0;
	for (slot = 0; slot < 5U; slot++) {
		status = vm_heap_add_root(realm->heap, &roots[slot]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns this registry slot only after acquisition succeeds. */
		registered++;
	}

	/* The source's converted box remains live through native key collection and later getter calls. */
	status = vm_to_object(realm, source, &source);
	if (status != 0)
		goto cleanup;
	object = (struct vm_object *)vm_value_as_cell(source);
	roots[2] = &object->cell;

	/* Trace even unvisited fresh Symbols before the actual OwnPropertyKeys method returns its list. */
	status = vm_heap_add_tracer(realm->heap, spread_trace_keys, &keys);
	if (status != 0)
		goto cleanup;

	/* Cleanup must retire the acquired vector tracer before releasing its storage. */
	traced = 1;
	status = vm_own_property_keys(realm, object, &keys);
	if (status != 0)
		goto cleanup;

	/* Descriptor and value reads interleave per key; earlier getters may change later enumerability. */
	for (index = 0; index < keys.count; index++) {
		key = *(vm_value *)wb_vector_at(&keys, index);

		/* A rest key already consumed by the pattern causes no descriptor or value read. */
		status = spread_is_excluded(realm, excluded, key, &found);
		if (status != 0)
			goto cleanup;
		if (found)
			continue;

		/* Only an actual enumerable own property contributes a value at this step. */
		status = vm_get_own_property(realm, object, key, &descriptor, &present);
		if (status != 0)
			goto cleanup;
		if (!present)
			continue;
		if ((descriptor.attributes & VM_PROPERTY_ENUMERABLE) == 0U)
			continue;

		/* Retain a fresh getter result independently while target definition can call or collect. */
		status = vm_get(realm, source, key, &value);
		if (status != 0)
			goto cleanup;
		roots[4] = NULL;
		is_cell = vm_value_is_cell(value);
		if (is_cell)
			roots[4] = vm_value_as_cell(value);

		/* CreateDataProperty defines an own default data property without invoking inherited setters. */
		memset(&descriptor, 0, sizeof(descriptor));
		descriptor.has = VM_HAS_VALUE | VM_HAS_WRITABLE | VM_HAS_ENUMERABLE | VM_HAS_CONFIGURABLE;
		descriptor.attributes = VM_PROPERTY_DEFAULT;
		descriptor.value = value;
		status = vm_define_own_property(realm, target, key, &descriptor, &accepted);
		if (status != 0)
			goto cleanup;
		if (!accepted) {
			/* Fresh payload, source and key stay owned through refusal-error allocation. */
			status = vm_throw_type_error(realm, "Cannot create copied data property");
			goto cleanup;
		}
	}

	/* All selected properties were defined without publishing a partial success after an error. */
	status = 0;

	/* Keys and every acquired direct-call root are retired on normal and abrupt paths. */
cleanup:
	if (traced)
		vm_heap_remove_tracer(realm->heap, spread_trace_keys, &keys);
	wb_vector_release(&keys);
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve abrupt property or root-registration errors after releasing native ownership. */
	if (status != 0)
		return status;

	/* Succeeded: each selected own enumerable property has been copied. */
	return 0;
}

/* Keep every initialized native key entry live across later descriptor, exclusion and value callbacks. */
static void
spread_trace_keys(
	struct vm_heap *heap,
	void *context)
{
	struct wb_vector *keys;
	vm_value key;
	size_t index;

	/* The caller owns vector storage; only its published count contains valid values. */
	keys = context;
	for (index = 0; index < keys->count; index++) {
		key = *(vm_value *)wb_vector_at(keys, index);
		vm_heap_mark_value(heap, key);
	}
}

/* Tells whether the array of excluded keys (undefined for none) lists a key. */
static int
spread_is_excluded(
	struct vm_realm *realm,
	vm_value excluded,
	vm_value key,
	int *found)
{
	struct vm_object *list;
	struct vm_cell *listed_root;
	vm_value listed;
	uint32_t index;
	int is_cell;
	int status;

	/* Without a list nothing is excluded. */
	*found = 0;
	if (excluded == VM_VALUE_UNDEFINED)
		return 0;

	/* A getter result may exist only in native storage during ToPropertyKey. */
	listed_root = NULL;
	status = vm_heap_add_root(realm->heap, &listed_root);
	if (status != 0)
		return status;

	/* Each listed key, compared as a property key. */
	list = (struct vm_object *)vm_value_as_cell(excluded);
	for (index = 0; index < list->length; index++) {
		status = vm_get(realm, excluded, vm_value_int32((int32_t)index), &listed);
		if (status != 0)
			goto cleanup;
		listed_root = NULL;
		is_cell = vm_value_is_cell(listed);
		if (is_cell)
			listed_root = vm_value_as_cell(listed);
		status = vm_to_key(realm, listed, &listed);
		if (status != 0)
			goto cleanup;
		if (listed == key) {
			*found = 1;
			break;
		}
	}

	/* The temporary getter result is no longer needed on either path. */
	status = 0;

cleanup:
	vm_heap_remove_root(realm->heap, &listed_root);

	/* A getter or key conversion failure prevents a reliable exclusion answer. */
	if (status != 0)
		return status;

	/* Succeeded: found reports whether the key was listed. */
	return 0;
}

/*
 * Tells whether a value is iterated by the built-in iteration: an array,
 * an arguments object or a string whose Symbol.iterator is still the
 * built-in one (any of them before the built-ins exist).
 */
static int
spread_is_builtin(
	struct vm_realm *realm,
	vm_value value,
	vm_value method)
{
	struct vm_object *builtin;
	vm_value expected;
	int iterable;
	int is_string;

	/* Only the values with a built-in iteration. */
	iterable = spread_iterable(value);
	if (!iterable)
		return 0;

	/* A string: String.prototype's own iterator. */
	is_string = vm_value_is_string(value);
	if (is_string) {
		builtin = realm->intrinsics[VM_INTRINSIC_STRING_ITERATOR];
		if (builtin == NULL)
			return 1;
		expected = vm_value_cell(builtin);
		if (method == expected)
			return 1;
		return 0;
	}

	/* An array or an arguments object: Array.prototype.values. */
	builtin = realm->intrinsics[VM_INTRINSIC_ARRAY_VALUES];
	if (builtin == NULL)
		return 1;
	expected = vm_value_cell(builtin);
	if (method == expected)
		return 1;

	/* A user's iterator. */
	return 0;
}

/*
 * Takes the next value of an iteration by the protocol: the iterator's
 * next called, its result's done and value read.  Whatever throws ends the
 * iteration (a loop does not close an iterator whose next failed).
 */
static int
spread_protocol_next(
	struct vm_realm *realm,
	struct spread_iterator *state,
	vm_value *value,
	int *done)
{
	struct vm_cell *roots[3];
	vm_value result;
	vm_value flag;
	vm_value key;
	unsigned index;
	unsigned registered;
	int is_object;
	int status;

	/* Retain the callable state, its realm and the next result across getters. */
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &state->cell;
	roots[2] = NULL;
	registered = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Only acquired registrations belong to this call's cleanup. */
		registered++;
	}

	/* The call of next. */
	state->done = 1;
	status = vm_call(realm, state->next, state->iterator, NULL, 0, &result);
	if (status != 0)
		goto cleanup;
	is_object = vm_value_is_object(result);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Iterator result is not an object");
		goto cleanup;
	}

	/* Retain the returned object before reading user-defined done and value. */
	roots[2] = vm_value_as_cell(result);

	/* Its done. */
	key = vm_key_from_ascii(realm->heap, "done");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The done getter can run script and trigger collection. */
	status = vm_get(realm, result, key, &flag);
	if (status != 0)
		goto cleanup;
	*done = vm_to_boolean(flag);
	if (*done)
		goto cleanup;

	/* Its value. */
	key = vm_key_from_ascii(realm->heap, "value");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The value getter runs only for a nonterminal result. */
	status = vm_get(realm, result, key, value);
	if (status != 0)
		goto cleanup;

	/* Succeeded: the iteration goes on. */
	state->done = 0;

cleanup:
	/* Release the temporary result and state after their final callback. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A callback failure ends the iterator without publishing a new value. */
	if (status != 0)
		return status;

	/* Succeeded: the caller receives either a value or the done flag. */
	return 0;
}
