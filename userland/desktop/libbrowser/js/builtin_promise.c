/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Promise (ws074-p086): the constructor, Promise.prototype's then, catch
 * and finally, and Promise's resolve, reject, all, allSettled, any, race
 * and withResolvers, over the promises of vm/promise.c.
 *
 * A promise capability (the promise a constructor made with its
 * resolving functions) is a struct promise_capability; for Promise itself
 * then settles its promise directly (no functions), the other methods use
 * a real pair.  The combinators iterate what vm_iter_start can (arrays,
 * strings, arguments) until the iterator protocol arrives with Symbol.
 * Not in this pass: Symbol.species (a promise's constructor is its
 * species) and Symbol.toStringTag (Object.prototype.toString tells a
 * promise by its kind instead); AggregateError is an Error named so.
 */

#include "js/builtin.h"

#include <errno.h>
#include <string.h>

/* The combinators whose elements one record counts down. */
#define PROMISE_ALL		0U
#define PROMISE_ALL_SETTLED	1U
#define PROMISE_ANY		2U

/*
 * A promise capability: a promise and its resolving functions (the empty
 * value when the promise is Promise's own and is settled directly).
 */
struct promise_capability {
	vm_value promise;
	vm_value resolve;
	vm_value reject;
};

/*
 * What the element functions of one Promise.all, allSettled or any share:
 * the array of values (or of errors), the capability to settle, and how
 * many elements are still to come (one more while the iteration runs, so
 * the end cannot come before the last element is added).
 */
struct promise_combinator {
	struct vm_cell cell;
	vm_value values;
	vm_value promise;
	vm_value resolve;
	vm_value reject;
	uint32_t remaining;
	uint32_t kind;
};

/*
 * One element function's own state: its combinator, its index and whether
 * it was called (a second call does nothing).  rejects marks allSettled's
 * reject element.
 */
struct promise_element {
	struct vm_cell cell;
	struct promise_combinator *combinator;
	uint32_t index;
	uint32_t called;
	uint32_t rejects;
	uint32_t reserved;
};

static void promise_combinator_trace(struct vm_heap *heap, struct vm_cell *cell);
static void promise_element_trace(struct vm_heap *heap, struct vm_cell *cell);
static int promise_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_then(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_catch(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_finally(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_then_finally(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_catch_finally(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_finally_step(struct vm_realm *realm, int rejects, vm_value argument, vm_value *result);
static int promise_return_data(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_throw_data(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_static_resolve(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_static_reject(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_all(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_all_settled(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_any(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_race(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_with_resolvers(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_element(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_capability_executor(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_new_capability(struct vm_realm *realm, vm_value constructor, int functions, struct promise_capability *capability);
static int promise_settle_capability(struct vm_realm *realm, const struct promise_capability *capability, int rejects, vm_value value);
static int promise_species(struct vm_realm *realm, vm_value promise, vm_value *constructor);
static int promise_resolve_with(struct vm_realm *realm, vm_value constructor, vm_value value, vm_value *result);
static int promise_invoke_then(struct vm_realm *realm, vm_value promise, vm_value on_fulfilled, vm_value on_rejected, vm_value *result);
static int promise_combine(struct vm_realm *realm, vm_value constructor, vm_value iterable, uint32_t kind, vm_value *result);
static int promise_combine_values(struct vm_realm *realm, vm_value constructor, vm_value iterable, struct promise_capability *capability, uint32_t kind);
static int promise_element_function(struct vm_realm *realm, struct promise_combinator *combinator, uint32_t index, uint32_t rejects, vm_value *function);
static int promise_combinator_done(struct vm_realm *realm, struct promise_combinator *combinator);
static int promise_settled_record(struct vm_realm *realm, int rejected, vm_value value, vm_value *record);
static int promise_data_function(struct vm_realm *realm, vm_native native, unsigned length, vm_value data, vm_value *function);
static int promise_get(struct vm_realm *realm, vm_value object, const char *name, vm_value *value);

/* The cell type of a combinator's shared state. */
static const struct vm_cell_type promise_combinator_type = {
	"promise combinator", promise_combinator_trace, NULL
};

/* The cell type of an element function's state. */
static const struct vm_cell_type promise_element_type = {
	"promise element", promise_element_trace, NULL
};

/*
 * Installs Promise: the constructor with its functions, and its
 * prototype (an ordinary object) with then, catch and finally.
 */
int
js_builtin_install_promise(
	struct vm_realm *realm)
{
	struct vm_function *constructor;
	struct vm_object *prototype;
	int error;

	/* The prototype, and the constructor on the global object. */
	prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (prototype == NULL)
		return ENOMEM;
	realm->intrinsics[VM_INTRINSIC_PROMISE_PROTOTYPE] = prototype;
	error = js_builtin_constructor(realm, "Promise", 1, promise_call, promise_construct, prototype, &constructor);
	if (error != 0)
		return error;
	realm->intrinsics[VM_INTRINSIC_PROMISE] = &constructor->object;

	/* The prototype's methods. */
	error = js_builtin_method(realm, prototype, "then", 2, promise_then);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "catch", 1, promise_catch);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "finally", 1, promise_finally);
	if (error != 0)
		return error;

	/* The constructor's functions. */
	error = js_builtin_method(realm, &constructor->object, "resolve", 1, promise_static_resolve);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "reject", 1, promise_static_reject);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "all", 1, promise_all);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "allSettled", 1, promise_all_settled);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "any", 1, promise_any);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "race", 1, promise_race);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "withResolvers", 0, promise_with_resolvers);
	if (error != 0)
		return error;

	/* Succeeded: Promise is installed. */
	return 0;
}

/* Marks what a combinator's state holds: its values and its capability. */
static void
promise_combinator_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct promise_combinator *combinator;

	/* The values and the capability. */
	combinator = (struct promise_combinator *)cell;
	vm_heap_mark_value(heap, combinator->values);
	vm_heap_mark_value(heap, combinator->promise);
	vm_heap_mark_value(heap, combinator->resolve);
	vm_heap_mark_value(heap, combinator->reject);
}

/* Marks an element function's combinator. */
static void
promise_element_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct promise_element *element;

	/* The shared state. */
	element = (struct promise_element *)cell;
	if (element->combinator != NULL)
		vm_heap_mark(heap, &element->combinator->cell);
}

/* Promise(): only new may make a promise. */
static int
promise_call(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A TypeError, as a call of a class. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_throw_type_error(realm, "Promise constructor cannot be invoked without 'new'");
	return status;
}

/*
 * new Promise(executor): a pending promise from new.target's prototype,
 * whose resolving functions the executor is called with; what the
 * executor throws rejects it.
 */
static int
promise_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *prototype;
	vm_value functions[2];
	vm_value new_target;
	vm_value executor;
	vm_value promise;
	vm_value reason;
	vm_value ignored;
	int callable;
	int status;

	UNUSED_PARAMETER(this_value);

	/* new.target first (the calls below may run scripts); the executor must be a function. */
	*result = VM_VALUE_UNDEFINED;
	new_target = realm->new_target;
	executor = js_argument(args, count, 0);
	callable = vm_value_is_callable(executor);
	if (!callable) {
		status = vm_throw_type_error(realm, "Promise resolver is not a function");
		return status;
	}

	/* The promise from new.target's prototype. */
	status = vm_construct_prototype(realm, new_target, realm->intrinsics[VM_INTRINSIC_PROMISE_PROTOTYPE], &prototype);
	if (status != 0)
		return status;
	status = vm_promise_create(realm, prototype, &promise);
	if (status != 0)
		return status;

	/* Its resolving functions, given to the executor. */
	status = vm_promise_resolving_functions(realm, promise, &functions[0], &functions[1]);
	if (status != 0)
		return status;
	status = vm_call(realm, executor, VM_VALUE_UNDEFINED, functions, 2, &ignored);

	/* What the executor throws rejects the promise (unless it was resolved already). */
	if (status == VM_THROWN) {
		reason = realm->exception;
		realm->exception = VM_VALUE_UNDEFINED;
		status = vm_call(realm, functions[1], VM_VALUE_UNDEFINED, &reason, 1, &ignored);
	}

	/* What failed goes back to the caller. */
	if (status != 0)
		return status;

	/* Succeeded: the promise. */
	*result = promise;
	return 0;
}

/*
 * Promise.prototype.then(onFulfilled, onRejected): a new promise from the
 * promise's constructor, settled by the handler that runs when the
 * promise settles.
 */
static int
promise_then(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct promise_capability capability;
	vm_value constructor;
	int is_promise;
	int status;

	/* Only a promise has then. */
	*result = VM_VALUE_UNDEFINED;
	is_promise = vm_value_is_promise(this_value);
	if (!is_promise) {
		status = vm_throw_type_error(realm, "Method Promise.prototype.then called on incompatible receiver");
		return status;
	}

	/* The derived promise, from the promise's constructor. */
	status = promise_species(realm, this_value, &constructor);
	if (status != 0)
		return status;
	status = promise_new_capability(realm, constructor, 0, &capability);
	if (status != 0)
		return status;

	/* The reaction. */
	status = vm_promise_then(realm, this_value, js_argument(args, count, 0), js_argument(args, count, 1), capability.promise,
	    capability.resolve, capability.reject);
	if (status != 0)
		return status;

	/* Succeeded: the derived promise. */
	*result = capability.promise;
	return 0;
}

/* Promise.prototype.catch(onRejected): this.then(undefined, onRejected). */
static int
promise_catch(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* then, looked up on this (it may be any thenable). */
	status = promise_invoke_then(realm, this_value, VM_VALUE_UNDEFINED, js_argument(args, count, 0), result);
	if (status != 0)
		return status;

	/* Succeeded: what then returned. */
	return 0;
}

/*
 * Promise.prototype.finally(onFinally): this.then with two functions that
 * call onFinally and then pass the value or the reason on, after the
 * promise onFinally returns settles.
 */
static int
promise_finally(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value on_finally;
	vm_value constructor;
	vm_value then_finally;
	vm_value catch_finally;
	vm_value pair;
	vm_value parts[2];
	int is_object;
	int callable;
	int status;

	/* Only an object has then. */
	*result = VM_VALUE_UNDEFINED;
	is_object = vm_value_is_object(this_value);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Method Promise.prototype.finally called on incompatible receiver");
		return status;
	}

	/* The constructor the value's promise is made with. */
	status = promise_species(realm, this_value, &constructor);
	if (status != 0)
		return status;

	/* Without a function, onFinally is passed to then as it is. */
	on_finally = js_argument(args, count, 0);
	callable = vm_value_is_callable(on_finally);
	if (!callable) {
		status = promise_invoke_then(realm, this_value, on_finally, on_finally, result);
		return status;
	}

	/* The two functions share onFinally and the constructor. */
	parts[0] = on_finally;
	parts[1] = constructor;
	status = js_builtin_array(realm, parts, 2, &pair);
	if (status != 0)
		return status;
	status = promise_data_function(realm, promise_then_finally, 1, pair, &then_finally);
	if (status != 0)
		return status;
	status = promise_data_function(realm, promise_catch_finally, 1, pair, &catch_finally);
	if (status != 0)
		return status;

	/* then with the two. */
	status = promise_invoke_then(realm, this_value, then_finally, catch_finally, result);
	if (status != 0)
		return status;

	/* Succeeded: what then returned. */
	return 0;
}

/* The function finally gives then for a fulfillment: onFinally, then the value passes on. */
static int
promise_then_finally(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The value passes on once onFinally's result settles. */
	status = promise_finally_step(realm, 0, js_argument(args, count, 0), result);
	if (status != 0)
		return status;

	/* Succeeded: the promise of the value. */
	return 0;
}

/* The function finally gives then for a rejection: onFinally, then the reason is thrown again. */
static int
promise_catch_finally(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The reason is thrown again once onFinally's result settles. */
	status = promise_finally_step(realm, 1, js_argument(args, count, 0), result);
	if (status != 0)
		return status;

	/* Succeeded: the promise of the reason. */
	return 0;
}

/*
 * The step of finally's two functions: onFinally is called (it and the
 * constructor are the running function's data), and once the promise of
 * its result is fulfilled the value passes on or the reason is thrown
 * again (rejects).
 */
static int
promise_finally_step(
	struct vm_realm *realm,
	int rejects,
	vm_value argument,
	vm_value *result)
{
	vm_value on_finally;
	vm_value constructor;
	vm_value outcome;
	vm_value promise;
	vm_value pass_on;
	vm_value pair;
	int status;

	/* The shared onFinally and constructor. */
	*result = VM_VALUE_UNDEFINED;
	pair = js_builtin_callee(realm)->data;
	status = vm_get(realm, pair, vm_value_int32(0), &on_finally);
	if (status != 0)
		return status;
	status = vm_get(realm, pair, vm_value_int32(1), &constructor);
	if (status != 0)
		return status;

	/* onFinally, with no argument. */
	status = vm_call(realm, on_finally, VM_VALUE_UNDEFINED, NULL, 0, &outcome);
	if (status != 0)
		return status;

	/* Its result as a promise of the constructor. */
	status = promise_resolve_with(realm, constructor, outcome, &promise);
	if (status != 0)
		return status;

	/* A function that returns the value, or throws the reason. */
	if (rejects) {
		status = promise_data_function(realm, promise_throw_data, 0, argument, &pass_on);
	} else {
		status = promise_data_function(realm, promise_return_data, 0, argument, &pass_on);
	}

	/* What failed goes back to the caller. */
	if (status != 0)
		return status;

	/* promise.then(passOn). */
	status = promise_invoke_then(realm, promise, pass_on, VM_VALUE_UNDEFINED, result);
	if (status != 0)
		return status;

	/* Succeeded: the promise of the value passed on. */
	return 0;
}

/* A function that returns its data (finally's value thunk). */
static int
promise_return_data(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Succeeded: the data. */
	*result = js_builtin_callee(realm)->data;
	return 0;
}

/* A function that throws its data (finally's thrower). */
static int
promise_throw_data(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The data, thrown. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_throw(realm, js_builtin_callee(realm)->data);
	return status;
}

/* Promise.resolve(value): the value itself when it is a promise of this constructor, else a new promise resolved with it. */
static int
promise_static_resolve(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int is_object;
	int status;

	/* this must be an object (the constructor). */
	*result = VM_VALUE_UNDEFINED;
	is_object = vm_value_is_object(this_value);
	if (!is_object) {
		status = vm_throw_type_error(realm, "PromiseResolve called on non-object");
		return status;
	}

	/* PromiseResolve. */
	status = promise_resolve_with(realm, this_value, js_argument(args, count, 0), result);
	if (status != 0)
		return status;

	/* Succeeded: the promise. */
	return 0;
}

/* Promise.reject(reason): a new promise of this constructor rejected with the reason. */
static int
promise_static_reject(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct promise_capability capability;
	int status;

	/* A capability of the constructor, rejected. */
	*result = VM_VALUE_UNDEFINED;
	status = promise_new_capability(realm, this_value, 1, &capability);
	if (status != 0)
		return status;
	status = promise_settle_capability(realm, &capability, 1, js_argument(args, count, 0));
	if (status != 0)
		return status;

	/* Succeeded: the rejected promise. */
	*result = capability.promise;
	return 0;
}

/* Promise.all(iterable): the values of all the promises once all are fulfilled, the first reason otherwise. */
static int
promise_all(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The combinator of all. */
	status = promise_combine(realm, this_value, js_argument(args, count, 0), PROMISE_ALL, result);
	if (status != 0)
		return status;

	/* Succeeded: the promise. */
	return 0;
}

/* Promise.allSettled(iterable): a record of each promise's outcome once all are settled. */
static int
promise_all_settled(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The combinator of allSettled. */
	status = promise_combine(realm, this_value, js_argument(args, count, 0), PROMISE_ALL_SETTLED, result);
	if (status != 0)
		return status;

	/* Succeeded: the promise. */
	return 0;
}

/* Promise.any(iterable): the first value fulfilled, or an AggregateError of every reason. */
static int
promise_any(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The combinator of any. */
	status = promise_combine(realm, this_value, js_argument(args, count, 0), PROMISE_ANY, result);
	if (status != 0)
		return status;

	/* Succeeded: the promise. */
	return 0;
}

/* Promise.race(iterable): settled as the first of the promises to settle. */
static int
promise_race(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct promise_capability capability;
	vm_value promise_resolve;
	vm_value iterator;
	vm_value value;
	vm_value next_promise;
	vm_value ignored;
	vm_value reason;
	int callable;
	int done;
	int status;

	/* A capability of the constructor, with real functions to give each promise's then. */
	*result = VM_VALUE_UNDEFINED;
	status = promise_new_capability(realm, this_value, 1, &capability);
	if (status != 0)
		return status;

	/* The constructor's resolve, then each value of the iterable raced. */
	status = promise_get(realm, this_value, "resolve", &promise_resolve);
	if (status == 0) {
		callable = vm_value_is_callable(promise_resolve);
		if (!callable)
			status = vm_throw_type_error(realm, "Promise resolve is not a function");
	}

	/* The iteration, unless the lookup failed. */
	if (status == 0)
		status = vm_iter_start(realm, js_argument(args, count, 0), &iterator);
	while (status == 0) {
		status = vm_iter_next(realm, iterator, &value, &done);
		if (status != 0 || done)
			break;

		/* The value as a promise, whose settlement settles the race. */
		status = vm_call(realm, promise_resolve, this_value, &value, 1, &next_promise);
		if (status == 0)
			status = promise_invoke_then(realm, next_promise, capability.resolve, capability.reject, &ignored);
	}

	/* What the iteration threw rejects the promise. */
	if (status == VM_THROWN) {
		reason = realm->exception;
		realm->exception = VM_VALUE_UNDEFINED;
		status = promise_settle_capability(realm, &capability, 1, reason);
	}

	/* What failed goes back to the caller. */
	if (status != 0)
		return status;

	/* Succeeded: the promise of the race. */
	*result = capability.promise;
	return 0;
}

/* Promise.withResolvers(): { promise, resolve, reject } of a new promise of this constructor. */
static int
promise_with_resolvers(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct promise_capability capability;
	struct vm_object *object;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A capability with real functions. */
	*result = VM_VALUE_UNDEFINED;
	status = promise_new_capability(realm, this_value, 1, &capability);
	if (status != 0)
		return status;

	/* The object with the three. */
	object = vm_object_create(realm->heap, realm->object_prototype);
	if (object == NULL)
		return ENOMEM;
	status = js_builtin_value(realm, object, "promise", capability.promise, VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, object, "resolve", capability.resolve, VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, object, "reject", capability.reject, VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* Succeeded: the object. */
	*result = vm_value_cell(object);
	return 0;
}

/*
 * An element function of all, allSettled or any: the first call puts its
 * argument (or allSettled's record of it) at its index, and the last
 * element to come settles the combinator's promise.
 */
static int
promise_element(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct promise_combinator *combinator;
	struct promise_element *element;
	vm_value value;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The element's state; a second call does nothing. */
	*result = VM_VALUE_UNDEFINED;
	element = (struct promise_element *)vm_value_as_cell(js_builtin_callee(realm)->data);
	combinator = element->combinator;
	if (element->called)
		return 0;
	element->called = 1;

	/* The value at the element's index: the argument, or allSettled's record of it. */
	value = js_argument(args, count, 0);
	if (combinator->kind == PROMISE_ALL_SETTLED) {
		status = promise_settled_record(realm, (int)element->rejects, value, &value);
		if (status != 0)
			return status;
	}

	/* The value goes at the element's index. */
	status = vm_put(realm, combinator->values, vm_value_int32((int32_t)element->index), value);
	if (status != 0)
		return status;

	/* One fewer to come; the last settles the promise. */
	combinator->remaining--;
	if (combinator->remaining == 0) {
		status = promise_combinator_done(realm, combinator);
		if (status != 0)
			return status;
	}

	/* Succeeded: undefined. */
	return 0;
}

/*
 * The executor NewPromiseCapability gives a constructor: it keeps the
 * resolve and reject it is called with in its data (an array of two),
 * once.
 */
static int
promise_capability_executor(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value pair;
	vm_value resolve;
	vm_value reject;
	int status;

	UNUSED_PARAMETER(this_value);

	/* What the executor already has. */
	*result = VM_VALUE_UNDEFINED;
	pair = js_builtin_callee(realm)->data;
	status = vm_get(realm, pair, vm_value_int32(0), &resolve);
	if (status == 0)
		status = vm_get(realm, pair, vm_value_int32(1), &reject);
	if (status != 0)
		return status;

	/* A second call with functions it already has is a TypeError. */
	if (resolve != VM_VALUE_UNDEFINED || reject != VM_VALUE_UNDEFINED) {
		status = vm_throw_type_error(realm, "Promise executor has already been invoked with non-undefined arguments");
		return status;
	}

	/* The two, kept. */
	status = vm_put(realm, pair, vm_value_int32(0), js_argument(args, count, 0));
	if (status == 0)
		status = vm_put(realm, pair, vm_value_int32(1), js_argument(args, count, 1));
	if (status != 0)
		return status;

	/* Succeeded: undefined. */
	return 0;
}

/*
 * Makes a promise capability of a constructor (NewPromiseCapability):
 * for Promise itself a promise the engine settles directly (with a real
 * pair of resolving functions when functions asks for them), for another
 * constructor what new gives with the pair its executor received.
 */
static int
promise_new_capability(
	struct vm_realm *realm,
	vm_value constructor,
	int functions,
	struct promise_capability *capability)
{
	vm_value executor;
	vm_value pair;
	vm_value empty[2];
	vm_value intrinsic;
	int is_constructor;
	int callable;
	int status;

	/* Nothing yet. */
	capability->promise = VM_VALUE_UNDEFINED;
	capability->resolve = VM_VALUE_EMPTY;
	capability->reject = VM_VALUE_EMPTY;

	/* Promise itself: a promise of its own, with functions when asked. */
	intrinsic = VM_VALUE_UNDEFINED;
	if (realm->intrinsics[VM_INTRINSIC_PROMISE] != NULL)
		intrinsic = vm_value_cell(realm->intrinsics[VM_INTRINSIC_PROMISE]);
	if (constructor == intrinsic) {
		status = vm_promise_create(realm, NULL, &capability->promise);
		if (status != 0)
			return status;
		if (!functions)
			return 0;

		/* The pair of resolving functions. */
		status = vm_promise_resolving_functions(realm, capability->promise, &capability->resolve, &capability->reject);
		if (status != 0)
			return status;
		return 0;
	}

	/* Another constructor must be one. */
	is_constructor = vm_value_is_constructor(constructor);
	if (!is_constructor) {
		status = vm_throw_type_error(realm, "Promise capability constructor is not a constructor");
		return status;
	}

	/* The executor, keeping what it is called with. */
	empty[0] = VM_VALUE_UNDEFINED;
	empty[1] = VM_VALUE_UNDEFINED;
	status = js_builtin_array(realm, empty, 2, &pair);
	if (status != 0)
		return status;
	status = promise_data_function(realm, promise_capability_executor, 2, pair, &executor);
	if (status != 0)
		return status;

	/* new constructor(executor). */
	status = vm_construct(realm, constructor, &executor, 1, constructor, &capability->promise);
	if (status != 0)
		return status;

	/* The functions it gave, which must be callable. */
	status = vm_get(realm, pair, vm_value_int32(0), &capability->resolve);
	if (status == 0)
		status = vm_get(realm, pair, vm_value_int32(1), &capability->reject);
	if (status != 0)
		return status;
	callable = vm_value_is_callable(capability->resolve);
	if (callable)
		callable = vm_value_is_callable(capability->reject);
	if (!callable) {
		status = vm_throw_type_error(realm, "Promise resolve or reject function is not callable");
		return status;
	}

	/* Succeeded: the capability. */
	return 0;
}

/* Resolves or rejects a capability's promise with a value: directly, or through its functions. */
static int
promise_settle_capability(
	struct vm_realm *realm,
	const struct promise_capability *capability,
	int rejects,
	vm_value value)
{
	vm_value ignored;
	int status;

	/* Promise's own promise without functions. */
	if (capability->resolve == VM_VALUE_EMPTY) {
		if (rejects) {
			status = vm_promise_reject(realm, capability->promise, value);
		} else {
			status = vm_promise_resolve(realm, capability->promise, value);
		}

		/* What failed goes back to the caller. */
		if (status != 0)
			return status;
		return 0;
	}

	/* Through the functions. */
	if (rejects) {
		status = vm_call(realm, capability->reject, VM_VALUE_UNDEFINED, &value, 1, &ignored);
	} else {
		status = vm_call(realm, capability->resolve, VM_VALUE_UNDEFINED, &value, 1, &ignored);
	}

	/* What failed goes back to the caller. */
	if (status != 0)
		return status;

	/* Succeeded: the promise has the outcome. */
	return 0;
}

/*
 * Finds the constructor a promise's derived promises are made with
 * (SpeciesConstructor, with the constructor as its own species until
 * Symbol.species): its constructor property, Promise when that is
 * undefined.
 */
static int
promise_species(
	struct vm_realm *realm,
	vm_value promise,
	vm_value *constructor)
{
	vm_value species;
	int is_constructor;
	int is_object;
	int status;

	/* The constructor property. */
	status = promise_get(realm, promise, "constructor", constructor);
	if (status != 0)
		return status;

	/* Undefined gives Promise. */
	if (*constructor == VM_VALUE_UNDEFINED) {
		*constructor = vm_value_cell(realm->intrinsics[VM_INTRINSIC_PROMISE]);
		return 0;
	}

	/* Anything else must be an object. */
	is_object = vm_value_is_object(*constructor);
	if (!is_object) {
		status = vm_throw_type_error(realm, "The .constructor property is not an object");
		return status;
	}

	/* Its Symbol.species: undefined or null give Promise, anything else must construct (ws074-p087). */
	status = vm_get(realm, *constructor, vm_symbol_key(realm, VM_SYMBOL_SPECIES), &species);
	if (status != 0)
		return status;
	if (species == VM_VALUE_UNDEFINED || species == VM_VALUE_NULL) {
		*constructor = vm_value_cell(realm->intrinsics[VM_INTRINSIC_PROMISE]);
		return 0;
	}

	/* A species must be a constructor. */
	is_constructor = vm_value_is_constructor(species);
	if (!is_constructor) {
		status = vm_throw_type_error(realm, "object.constructor[Symbol.species] is not a constructor");
		return status;
	}

	/* Succeeded: the species. */
	*constructor = species;
	return 0;
}

/*
 * Makes a value a promise of a constructor (PromiseResolve): a promise
 * whose constructor is the constructor is itself, anything else a new
 * promise of the constructor resolved with it.
 */
static int
promise_resolve_with(
	struct vm_realm *realm,
	vm_value constructor,
	vm_value value,
	vm_value *result)
{
	struct promise_capability capability;
	vm_value value_constructor;
	int is_promise;
	int status;

	/* A promise of the same constructor. */
	*result = VM_VALUE_UNDEFINED;
	is_promise = vm_value_is_promise(value);
	if (is_promise) {
		status = promise_get(realm, value, "constructor", &value_constructor);
		if (status != 0)
			return status;
		if (value_constructor == constructor) {
			*result = value;
			return 0;
		}
	}

	/* A new promise resolved with the value. */
	status = promise_new_capability(realm, constructor, 0, &capability);
	if (status != 0)
		return status;
	status = promise_settle_capability(realm, &capability, 0, value);
	if (status != 0)
		return status;

	/* Succeeded: the new promise. */
	*result = capability.promise;
	return 0;
}

/* Calls a value's then method with two handlers (Invoke(promise, "then", ...)). */
static int
promise_invoke_then(
	struct vm_realm *realm,
	vm_value promise,
	vm_value on_fulfilled,
	vm_value on_rejected,
	vm_value *result)
{
	vm_value then;
	vm_value handlers[2];
	int status;

	/* The method. */
	*result = VM_VALUE_UNDEFINED;
	status = promise_get(realm, promise, "then", &then);
	if (status != 0)
		return status;

	/* The call (a TypeError when then is not a function). */
	handlers[0] = on_fulfilled;
	handlers[1] = on_rejected;
	status = vm_call(realm, then, promise, handlers, 2, result);
	if (status != 0)
		return status;

	/* Succeeded: what then returned. */
	return 0;
}

/*
 * Runs Promise.all, allSettled or any on a constructor and an iterable: a
 * capability of the constructor, settled by the element functions each
 * value's promise is given; what the iteration throws rejects it.
 */
static int
promise_combine(
	struct vm_realm *realm,
	vm_value constructor,
	vm_value iterable,
	uint32_t kind,
	vm_value *result)
{
	struct promise_capability capability;
	vm_value reason;
	int status;

	/* A capability of the constructor, with real functions. */
	*result = VM_VALUE_UNDEFINED;
	status = promise_new_capability(realm, constructor, 1, &capability);
	if (status != 0)
		return status;

	/* The values; what they throw rejects the promise. */
	status = promise_combine_values(realm, constructor, iterable, &capability, kind);
	if (status == VM_THROWN) {
		reason = realm->exception;
		realm->exception = VM_VALUE_UNDEFINED;
		status = promise_settle_capability(realm, &capability, 1, reason);
	}

	/* What failed goes back to the caller. */
	if (status != 0)
		return status;

	/* Succeeded: the promise of the combination. */
	*result = capability.promise;
	return 0;
}

/*
 * Gives each value of the iterable, as a promise of the constructor, an
 * element function of the combinator (and the capability's other
 * function), and ends the count once the iteration is over.
 */
static int
promise_combine_values(
	struct vm_realm *realm,
	vm_value constructor,
	vm_value iterable,
	struct promise_capability *capability,
	uint32_t kind)
{
	struct promise_combinator *combinator;
	struct vm_object *values;
	vm_value promise_resolve;
	vm_value iterator;
	vm_value value;
	vm_value next_promise;
	vm_value on_fulfilled;
	vm_value on_rejected;
	vm_value ignored;
	uint32_t index;
	int callable;
	int done;
	int status;

	/* The constructor's resolve. */
	status = promise_get(realm, constructor, "resolve", &promise_resolve);
	if (status != 0)
		return status;
	callable = vm_value_is_callable(promise_resolve);
	if (!callable) {
		status = vm_throw_type_error(realm, "Promise resolve is not a function");
		return status;
	}

	/* The iteration. */
	status = vm_iter_start(realm, iterable, &iterator);
	if (status != 0)
		return status;

	/* The shared state: the array, the capability and one count for the iteration itself. */
	values = vm_array_create(realm->heap, realm->array_prototype);
	if (values == NULL)
		return ENOMEM;
	combinator = vm_heap_alloc(realm->heap, &promise_combinator_type, sizeof(*combinator));
	if (combinator == NULL)
		return ENOMEM;
	combinator->values = vm_value_cell(values);
	combinator->promise = capability->promise;
	combinator->resolve = capability->resolve;
	combinator->reject = capability->reject;
	combinator->remaining = 1;
	combinator->kind = kind;

	/* Each value in turn. */
	for (index = 0;; index++) {
		status = vm_iter_next(realm, iterator, &value, &done);
		if (status != 0)
			return status;
		if (done)
			break;

		/* Its place in the array, and one more to come. */
		status = vm_put(realm, combinator->values, vm_value_int32((int32_t)index), VM_VALUE_UNDEFINED);
		if (status != 0)
			return status;
		combinator->remaining++;

		/* The value as a promise of the constructor. */
		status = vm_call(realm, promise_resolve, constructor, &value, 1, &next_promise);
		if (status != 0)
			return status;

		/* The handlers: all takes the value and passes a rejection on, any the reverse, allSettled records both. */
		on_fulfilled = capability->resolve;
		on_rejected = capability->reject;
		if (kind == PROMISE_ALL || kind == PROMISE_ALL_SETTLED) {
			status = promise_element_function(realm, combinator, index, 0, &on_fulfilled);
			if (status != 0)
				return status;
		}

		/* allSettled and any record a rejection too. */
		if (kind == PROMISE_ALL_SETTLED || kind == PROMISE_ANY) {
			status = promise_element_function(realm, combinator, index, 1, &on_rejected);
			if (status != 0)
				return status;
		}

		/* nextPromise.then(onFulfilled, onRejected). */
		status = promise_invoke_then(realm, next_promise, on_fulfilled, on_rejected, &ignored);
		if (status != 0)
			return status;
	}

	/* The iteration's own count; with nothing else to come the promise settles now. */
	combinator->remaining--;
	if (combinator->remaining == 0) {
		status = promise_combinator_done(realm, combinator);
		if (status != 0)
			return status;
	}

	/* Succeeded: every value has its handlers. */
	return 0;
}

/* Makes an element function of a combinator for an index (rejects for allSettled's and any's reject side). */
static int
promise_element_function(
	struct vm_realm *realm,
	struct promise_combinator *combinator,
	uint32_t index,
	uint32_t rejects,
	vm_value *function)
{
	struct promise_element *element;
	int status;

	/* The element's state. */
	element = vm_heap_alloc(realm->heap, &promise_element_type, sizeof(*element));
	if (element == NULL)
		return ENOMEM;
	element->combinator = combinator;
	element->index = index;
	element->rejects = rejects;

	/* The function holding it. */
	status = promise_data_function(realm, promise_element, 1, vm_value_cell(element), function);
	if (status != 0)
		return status;

	/* Succeeded: the element function. */
	return 0;
}

/*
 * Settles a combinator's promise once every element came: all and
 * allSettled resolve it with the array, any rejects it with an
 * AggregateError of the reasons.
 */
static int
promise_combinator_done(
	struct vm_realm *realm,
	struct promise_combinator *combinator)
{
	struct promise_capability capability;
	struct vm_object *error_object;
	vm_value error_value;
	vm_value name;
	int status;

	/* The capability the combinator keeps. */
	capability.promise = combinator->promise;
	capability.resolve = combinator->resolve;
	capability.reject = combinator->reject;

	/* all and allSettled: the array of values. */
	if (combinator->kind != PROMISE_ANY) {
		status = promise_settle_capability(realm, &capability, 0, combinator->values);
		if (status != 0)
			return status;
		return 0;
	}

	/* any: an AggregateError with the reasons as its errors. */
	status = vm_error_create(realm, VM_ERROR_PLAIN, "All promises were rejected", &error_value);
	if (status != 0)
		return status;
	error_object = (struct vm_object *)vm_value_as_cell(error_value);
	status = js_builtin_string(realm, "AggregateError", &name);
	if (status == 0)
		status = js_builtin_value(realm, error_object, "name", name, JS_BUILTIN_METHOD);
	if (status == 0)
		status = js_builtin_value(realm, error_object, "errors", combinator->values, JS_BUILTIN_METHOD);
	if (status != 0)
		return status;
	status = promise_settle_capability(realm, &capability, 1, error_value);
	if (status != 0)
		return status;

	/* Succeeded: the promise is rejected. */
	return 0;
}

/* Makes allSettled's record of an outcome: { status, value } or { status, reason }. */
static int
promise_settled_record(
	struct vm_realm *realm,
	int rejected,
	vm_value value,
	vm_value *record)
{
	struct vm_object *object;
	vm_value text;
	int status;

	/* The object. */
	object = vm_object_create(realm->heap, realm->object_prototype);
	if (object == NULL)
		return ENOMEM;

	/* Its status and the value or the reason. */
	if (rejected) {
		status = js_builtin_string(realm, "rejected", &text);
		if (status == 0)
			status = js_builtin_value(realm, object, "status", text, VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = js_builtin_value(realm, object, "reason", value, VM_PROPERTY_DEFAULT);
	} else {
		status = js_builtin_string(realm, "fulfilled", &text);
		if (status == 0)
			status = js_builtin_value(realm, object, "status", text, VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = js_builtin_value(realm, object, "value", value, VM_PROPERTY_DEFAULT);
	}

	/* What failed goes back to the caller. */
	if (status != 0)
		return status;

	/* Succeeded: the record. */
	*record = vm_value_cell(object);
	return 0;
}

/* Makes an anonymous native function that keeps data. */
static int
promise_data_function(
	struct vm_realm *realm,
	vm_native native,
	unsigned length,
	vm_value data,
	vm_value *function)
{
	struct vm_function *made;
	int status;

	/* The function, named "". */
	status = js_builtin_function(realm, "", length, native, NULL, &made);
	if (status != 0)
		return status;
	made->data = data;

	/* Succeeded: the function. */
	*function = vm_value_cell(made);
	return 0;
}

/* Reads a property of a value by an ASCII name. */
static int
promise_get(
	struct vm_realm *realm,
	vm_value object,
	const char *name,
	vm_value *value)
{
	vm_value key;
	int status;

	/* The key. */
	*value = VM_VALUE_UNDEFINED;
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* The property. */
	status = vm_get(realm, object, key, value);
	if (status != 0)
		return status;

	/* Succeeded: the value. */
	return 0;
}
