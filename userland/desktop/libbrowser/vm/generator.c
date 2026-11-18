/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Generators and async functions (ws074-p086): calling one, resuming a
 * generator, and driving an async function from await to await.
 *
 * Both run their code in a run of the interpreter of their own (the frame
 * is the run's entry frame), which a suspend instruction ends: the frame's
 * registers go to the generator cell until the run is resumed.  A call of
 * a generator function runs the code's prologue (its parameters) up to the
 * suspend the compiler puts at the start of the body, and makes the
 * generator object; next, throw and return resume it.  A call of an async
 * function makes its promise and runs the code up to its first await; each
 * await waits on a promise (vm_promise_await) whose settlement resumes the
 * run in a microtask, and the end of the code settles the promise.
 */

#include "vm/bytecode.h"
#include "vm/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* A synchronous yield* owns the actual iterator and its cached next method independently of user properties. */
struct generator_delegate {
	struct vm_cell cell;
	vm_value iterator;
	vm_value next;
};

static void generator_delegate_trace(struct vm_heap *heap, struct vm_cell *cell);
static void generator_trace(struct vm_heap *heap, struct vm_cell *cell);
static void generator_finalize(struct vm_heap *heap, struct vm_cell *cell);

/* The cell type of a generator's run: it holds its function, its frame's values and its promise. */
const struct vm_cell_type vm_generator_type = {
	"generator", generator_trace, generator_finalize
};

/* The delegate record keeps protocol values alive across suspension and reentrant collection. */
static const struct vm_cell_type generator_delegate_type = {
	"generator delegate", generator_delegate_trace, NULL
};

static struct vm_generator *generator_create(struct vm_realm *realm, struct vm_function *function);
static int generator_object(struct vm_realm *realm, struct vm_function *function, struct vm_generator *generator, vm_value *result);
static int generator_settle(struct vm_realm *realm, struct vm_generator *generator, int status, vm_value value, int suspended);
static void generator_end(struct vm_generator *generator);

/*
 * Tells whether a function's code is a generator's or an async function's,
 * which a call does not run as an ordinary frame.
 */
int
vm_code_is_suspendable(
	const struct vm_function *function)
{
	/* A native function has no code. */
	if (function->code == NULL)
		return 0;

	/* A generator function. */
	if ((function->code->flags & VM_CODE_GENERATOR) != 0U)
		return 1;

	/* An async function. */
	if ((function->code->flags & VM_CODE_ASYNC) != 0U)
		return 1;

	/* An ordinary function. */
	return 0;
}

/*
 * Calls a generator function or an async function: a generator function's
 * parameters are bound and its generator object is the result; an async
 * function runs up to its first await and its promise is the result.
 * Returns 0, VM_THROWN (a generator's parameters threw) or an errno
 * value.
 */
int
vm_generator_call(
	struct vm_realm *realm,
	struct vm_function *function,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_generator *generator;
	vm_value value;
	int suspended;
	int status;

	/* The run's cell, running from now on. */
	*result = VM_VALUE_UNDEFINED;
	generator = generator_create(realm, function);
	if (generator == NULL)
		return ENOMEM;

	/* An async function: its promise, then its code up to the first await (whatever it throws rejects the promise). */
	if ((function->code->flags & VM_CODE_ASYNC) != 0U) {
		status = vm_promise_create(realm, NULL, &generator->promise);
		if (status != 0)
			return status;
		status = vm_interpret_start(realm, function, this_value, args, count, generator, &value, &suspended);
		status = generator_settle(realm, generator, status, value, suspended);
		if (status != 0)
			return status;

		/* Succeeded: the promise of the function's result. */
		*result = generator->promise;
		return 0;
	}

	/* A generator function: its parameters, up to the suspend at the start of its body. */
	status = vm_interpret_start(realm, function, this_value, args, count, generator, &value, &suspended);
	if (status != 0)
		return status;
	if (!suspended)
		return EINVAL;
	generator->state = VM_GENERATOR_START;

	/* The generator object. */
	status = generator_object(realm, function, generator, result);
	if (status != 0)
		return status;

	/* Succeeded: the generator, not started. */
	return 0;
}

/*
 * Resumes a generator object (its next, throw or return, by how): the
 * value it yields next (*done 0), or its return value (*done 1).  A
 * generator done, or not started and told to throw or return, does not
 * run.  direct_result identifies a yielded iterator result whose identity
 * the caller must preserve.  Returns 0, VM_THROWN or an errno value.
 */
int
vm_generator_resume(
	struct vm_realm *realm,
	vm_value generator_value,
	vm_value value,
	int how,
	vm_value *result,
	int *done,
	int *direct_result)
{
	struct vm_generator *generator;
	struct vm_object *object;
	vm_value yielded;
	int is_object;
	int suspended;
	int status;

	/* Only a generator object can be resumed. */
	*result = VM_VALUE_UNDEFINED;
	*done = 1;
	*direct_result = 0;
	is_object = vm_value_is_object(generator_value);
	if (!is_object) {
		status = vm_throw_type_error(realm, "next method called on incompatible receiver");
		return status;
	}

	/* An object of another kind is not one either. */
	object = (struct vm_object *)vm_value_as_cell(generator_value);
	if (object->kind != VM_KIND_GENERATOR) {
		status = vm_throw_type_error(realm, "next method called on incompatible receiver");
		return status;
	}

	/* A generator that is running cannot be resumed from inside itself. */
	generator = (struct vm_generator *)vm_value_as_cell(object->internal);
	if (generator->state == VM_GENERATOR_RUNNING) {
		status = vm_throw_type_error(realm, "Generator is already running");
		return status;
	}

	/* A generator not started that is told to throw or return is done without running. */
	if (generator->state == VM_GENERATOR_START && how != VM_RESUME_NEXT)
		generator_end(generator);

	/* A generator done: throw throws the value, return returns it, next has nothing more. */
	if (generator->state == VM_GENERATOR_DONE) {
		if (how == VM_RESUME_THROW) {
			status = vm_throw(realm, value);
			return status;
		}

		/* Return gives the value back as the end. */
		if (how == VM_RESUME_RETURN)
			*result = value;
		return 0;
	}

	/* The run goes on from where it suspended. */
	generator->state = VM_GENERATOR_RUNNING;
	status = vm_interpret_resume(realm, generator, value, how, &yielded, &suspended);
	if (status != 0) {
		generator_end(generator);
		return status;
	}

	/* It yielded: it waits for the next resumption. */
	if (suspended) {
		generator->state = VM_GENERATOR_SUSPENDED;
		*result = yielded;
		*done = 0;
		*direct_result = (int)generator->direct_result;
		return 0;
	}

	/* Succeeded: it returned, and is done. */
	generator_end(generator);
	*result = yielded;
	return 0;
}

/*
 * Goes on with an async function's run after the promise it awaited
 * settled (how is VM_RESUME_NEXT with the value, VM_RESUME_THROW with the
 * reason), up to its next await or its end.  Returns 0 or an errno value
 * (what the function throws rejects its promise).
 */
int
vm_generator_continue(
	struct vm_realm *realm,
	struct vm_generator *generator,
	vm_value value,
	int how)
{
	vm_value result;
	int suspended;
	int status;

	/* The run from the await. */
	generator->state = VM_GENERATOR_RUNNING;
	status = vm_interpret_resume(realm, generator, value, how, &result, &suspended);

	/* What it came to: another await, the end, or an exception. */
	status = generator_settle(realm, generator, status, result, suspended);
	if (status != 0)
		return status;

	/* Succeeded: the function waits again, or its promise is settled. */
	return 0;
}


/*
 * Captures the actual synchronous iterator and its next method for yield*.
 * The private record is managed so a suspended generator owns both edges.
 */
int
vm_delegate_start(
	struct vm_realm *realm,
	vm_value iterable,
	vm_value *result)
{
	struct generator_delegate *state;
	struct vm_cell *roots[4];
	vm_value method;
	vm_value made;
	vm_value key;
	unsigned index;
	unsigned registered;
	int is_cell;
	int is_object;
	int callable;
	int status;

	/* Retain the realm, operand, fetched method and unpublished record across user code. */
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = NULL;
	is_cell = vm_value_is_cell(iterable);
	if (is_cell)
		roots[1] = vm_value_as_cell(iterable);
	roots[2] = NULL;
	roots[3] = NULL;
	registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Only acquired registrations belong to cleanup. */
		registered++;
	}

	/* Use the well-known symbol directly; the mutable global Symbol binding is irrelevant. */
	key = vm_symbol_key(realm, VM_SYMBOL_ITERATOR);
	status = vm_get(realm, iterable, key, &method);
	if (status != 0)
		goto cleanup;
	callable = vm_value_is_callable(method);
	if (!callable) {
		status = vm_throw_type_error(realm, "yield* operand is not iterable");
		goto cleanup;
	}

	/* A getter may have created the method without storing it anywhere else. */
	roots[2] = vm_value_as_cell(method);
	state = vm_heap_alloc(realm->heap, &generator_delegate_type, sizeof(*state));
	if (state == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Initialize every traced edge before the iterator method can collect. */
	roots[3] = &state->cell;
	state->iterator = VM_VALUE_UNDEFINED;
	state->next = VM_VALUE_UNDEFINED;
	status = vm_call(realm, method, iterable, NULL, 0, &made);
	if (status != 0)
		goto cleanup;
	state->iterator = made;
	is_object = vm_value_is_object(made);
	if (!is_object) {
		status = vm_throw_type_error(realm, "yield* iterator is not an object");
		goto cleanup;
	}

	/* GetIterator caches next without checking callability until the first step. */
	key = vm_key_from_ascii(realm->heap, "next");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Cache the iterator method before exposing the record to compiled code. */
	status = vm_get(realm, made, key, &state->next);
	if (status != 0)
		goto cleanup;

	/* Publish the complete record only after both protocol reads succeeded. */
	*result = vm_value_cell(state);

cleanup:
	/* Release only registrations acquired before a possible failure. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Any failed native operation preserves its original error status. */
	if (status != 0)
		return status;

	/* Succeeded: the generator can retain the record in its registers. */
	return 0;
}

/*
 * Forwards one synchronous yield* resumption and classifies its completion.
 * Outcome -1 exposes the original iterator result; NEXT supplies the
 * expression value, and RETURN carries a return through surrounding finally.
 */
int
vm_delegate_step(
	struct vm_realm *realm,
	vm_value record,
	vm_value sent,
	int how,
	vm_value *result,
	int *outcome)
{
	struct generator_delegate *state;
	struct vm_cell *roots[5];
	vm_value method;
	vm_value made;
	vm_value key;
	vm_value done_value;
	unsigned index;
	unsigned registered;
	int is_cell;
	int is_object;
	int callable;
	int missing_throw;
	int done;
	int status;

	/* Retain all native-only values through getters, callbacks and forced collection. */
	state = (struct generator_delegate *)vm_value_as_cell(record);
	if (state->cell.type != &generator_delegate_type)
		return EINVAL;
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &state->cell;
	roots[2] = NULL;
	is_cell = vm_value_is_cell(sent);
	if (is_cell)
		roots[2] = vm_value_as_cell(sent);
	roots[3] = NULL;
	roots[4] = NULL;
	registered = 0;
	for (index = 0; index < 5U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup cannot unregister slots that registration never acquired. */
		registered++;
	}

	/* next is cached, while return and throw are fetched afresh on each abrupt resumption. */
	missing_throw = 0;
	method = state->next;
	if (how != VM_RESUME_NEXT) {
		if (how == VM_RESUME_THROW) {
			key = vm_key_from_ascii(realm->heap, "throw");
		} else if (how == VM_RESUME_RETURN) {
			key = vm_key_from_ascii(realm->heap, "return");
		} else {
			status = EINVAL;
			goto cleanup;
		}

		/* A missing property key is allocation failure, not an absent protocol method. */
		if (key == VM_VALUE_EMPTY) {
			status = ENOMEM;
			goto cleanup;
		}

		/* Observe the current abrupt-completion method rather than a cached property. */
		status = vm_get(realm, state->iterator, key, &method);
		if (status != 0)
			goto cleanup;

		/* A missing return preserves the caller's return value without running following statements. */
		if (method == VM_VALUE_UNDEFINED || method == VM_VALUE_NULL) {
			if (how == VM_RESUME_RETURN) {
				*result = sent;
				*outcome = VM_RESUME_RETURN;
				status = 0;
				goto cleanup;
			}

			/* Missing throw first closes normally, so a failing close supersedes the protocol TypeError. */
			missing_throw = 1;
			key = vm_key_from_ascii(realm->heap, "return");
			if (key == VM_VALUE_EMPTY) {
				status = ENOMEM;
				goto cleanup;
			}

			/* Closing uses the iterator's current return method and preserves its failure. */
			status = vm_get(realm, state->iterator, key, &method);
			if (status != 0)
				goto cleanup;
			if (method == VM_VALUE_UNDEFINED || method == VM_VALUE_NULL) {
				status = vm_throw_type_error(realm, "yield* iterator has no throw method");
				goto cleanup;
			}
		}
	}

	/* A fetched method may exist only in this frame while its call runs. */
	is_cell = vm_value_is_cell(method);
	if (is_cell)
		roots[3] = vm_value_as_cell(method);
	callable = vm_value_is_callable(method);
	if (!callable) {
		status = vm_throw_type_error(realm, "yield* iterator method is not callable");
		goto cleanup;
	}

	/* The initial next receives one undefined argument; closing after missing throw receives none. */
	if (missing_throw) {
		status = vm_call(realm, method, state->iterator, NULL, 0, &made);
	} else {
		status = vm_call(realm, method, state->iterator, &sent, 1, &made);
	}

	/* Any failed native operation preserves its original error status. */
	if (status != 0)
		goto cleanup;
	is_object = vm_value_is_object(made);
	if (!is_object) {
		status = vm_throw_type_error(realm, "yield* iterator result is not an object");
		goto cleanup;
	}

	/* IteratorClose checks the object alone; done and value are intentionally not observed. */
	if (missing_throw) {
		status = vm_throw_type_error(realm, "yield* iterator has no throw method");
		goto cleanup;
	}

	/* IteratorComplete may run a getter; the original result must remain alive. */
	roots[4] = vm_value_as_cell(made);
	key = vm_key_from_ascii(realm->heap, "done");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The result object remains rooted while a done getter executes. */
	status = vm_get(realm, made, key, &done_value);
	if (status != 0)
		goto cleanup;
	done = vm_to_boolean(done_value);
	if (!done) {
		*result = made;
		*outcome = -1;
		status = 0;
		goto cleanup;
	}

	/* A completed result alone reads value; throw-method completion becomes a normal expression value. */
	key = vm_key_from_ascii(realm->heap, "value");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Transfer the completed value only after its getter succeeds. */
	status = vm_get(realm, made, key, result);
	if (status != 0)
		goto cleanup;
	*outcome = VM_RESUME_NEXT;
	if (how == VM_RESUME_RETURN)
		*outcome = VM_RESUME_RETURN;

cleanup:
	/* No native-only roots outlive their stack slots. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Any failed native operation preserves its original error status. */
	if (status != 0)
		return status;

	/* Succeeded: the compiler can suspend or propagate the classified completion. */
	return 0;
}

/* Marks what a generator's run refers to: its function, its frame's values and its promise. */
static void
generator_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_generator *generator;
	uint32_t index;

	/* The function and the header's values. */
	generator = (struct vm_generator *)cell;
	if (generator->function != NULL)
		vm_heap_mark(heap, &generator->function->object.cell);
	vm_heap_mark_value(heap, generator->this_value);
	vm_heap_mark_value(heap, generator->new_target);
	vm_heap_mark_value(heap, generator->promise);

	/* The registers kept while it is suspended. */
	if (generator->registers == NULL)
		return;
	for (index = 0; index < generator->register_count; index++)
		vm_heap_mark_value(heap, generator->registers[index]);
}

/* Frees the registers a dead generator kept. */
static void
generator_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_generator *generator;

	UNUSED_PARAMETER(heap);

	/* The registers, when it still had them. */
	generator = (struct vm_generator *)cell;
	free(generator->registers);
	generator->registers = NULL;
}

/* Makes the cell of a run of a function, running; NULL when out of memory. */
static struct vm_generator *
generator_create(
	struct vm_realm *realm,
	struct vm_function *function)
{
	struct vm_generator *generator;

	/* The cell (zeroed: no registers yet). */
	generator = vm_heap_alloc(realm->heap, &vm_generator_type, sizeof(*generator));
	if (generator == NULL)
		return NULL;

	/* Its function; nothing else is known before the run first suspends. */
	generator->function = function;
	generator->this_value = VM_VALUE_UNDEFINED;
	generator->new_target = VM_VALUE_UNDEFINED;
	generator->argument_count = 0;
	generator->promise = VM_VALUE_UNDEFINED;
	generator->state = VM_GENERATOR_RUNNING;
	generator->direct_result = 0;

	/* Succeeded: the cell. */
	return generator;
}

/*
 * Makes a generator object for a run: its prototype is the function's
 * prototype property when that is an object, %GeneratorPrototype%
 * otherwise.
 */
static int
generator_object(
	struct vm_realm *realm,
	struct vm_function *function,
	struct vm_generator *generator,
	vm_value *result)
{
	struct vm_object *prototype;
	struct vm_object *object;
	vm_value key;
	vm_value value;
	int is_object;
	int status;

	/* The function's prototype property. */
	key = vm_key_from_ascii(realm->heap, "prototype");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_get(realm, vm_value_cell(function), key, &value);
	if (status != 0)
		return status;

	/* An object is the prototype; anything else gives the realm's (Object.prototype before the built-ins). */
	is_object = vm_value_is_object(value);
	if (is_object) {
		prototype = (struct vm_object *)vm_value_as_cell(value);
	} else if (realm->intrinsics[VM_INTRINSIC_GENERATOR_PROTOTYPE] != NULL) {
		prototype = realm->intrinsics[VM_INTRINSIC_GENERATOR_PROTOTYPE];
	} else {
		prototype = realm->object_prototype;
	}

	/* The object, holding the run. */
	object = vm_object_create(realm->heap, prototype);
	if (object == NULL)
		return ENOMEM;
	object->kind = VM_KIND_GENERATOR;
	object->internal = vm_value_cell(generator);

	/* Succeeded: the generator object. */
	*result = vm_value_cell(object);
	return 0;
}

/*
 * Takes an async function's run where a step of it left it: an await
 * waits on its promise, the end resolves the function's promise, an
 * exception rejects it.  An await whose value cannot become a promise
 * (reading its constructor threw) throws into the function at once.
 * Returns 0 or an errno value.
 */
static int
generator_settle(
	struct vm_realm *realm,
	struct vm_generator *generator,
	int status,
	vm_value value,
	int suspended)
{
	vm_value reason;

	/* Each step, until the function waits or ends. */
	for (;;) {
		/* An exception out of the function rejects its promise. */
		if (status == VM_THROWN) {
			reason = realm->exception;
			realm->exception = VM_VALUE_UNDEFINED;
			generator_end(generator);
			status = vm_promise_reject(realm, generator->promise, reason);
			if (status != 0)
				return status;
			return 0;
		}

		/* Any other failure ends everything. */
		if (status != 0) {
			generator_end(generator);
			return status;
		}

		/* The end resolves the promise with the value returned. */
		if (!suspended) {
			generator_end(generator);
			status = vm_promise_resolve(realm, generator->promise, value);
			if (status != 0)
				return status;
			return 0;
		}

		/* An await: the function waits for the value's promise. */
		generator->state = VM_GENERATOR_SUSPENDED;
		status = vm_promise_await(realm, value, generator);
		if (status != VM_THROWN)
			break;

		/* The value's promise could not be had: the exception goes into the function at the await. */
		reason = realm->exception;
		realm->exception = VM_VALUE_UNDEFINED;
		generator->state = VM_GENERATOR_RUNNING;
		status = vm_interpret_resume(realm, generator, reason, VM_RESUME_THROW, &value, &suspended);
	}

	/* An await that could not be set up. */
	if (status != 0)
		return status;

	/* Succeeded: the function waits for its promise. */
	return 0;
}

/* Ends a run for good: it is done and its registers are let go. */
static void
generator_end(
	struct vm_generator *generator)
{
	/* Done, which no resumption changes. */
	generator->state = VM_GENERATOR_DONE;

	/* The frame's registers are no longer needed. */
	free(generator->registers);
	generator->registers = NULL;
	generator->register_count = 0;
}

/* Marks both protocol edges while a delegating generator is suspended or executing native protocol work. */
static void
generator_delegate_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct generator_delegate *state;

	/* The actual iterator and the cached next method can be independently collectible. */
	state = (struct generator_delegate *)cell;
	vm_heap_mark_value(heap, state->iterator);
	vm_heap_mark_value(heap, state->next);
}
