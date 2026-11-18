/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Promises (ws074-p086): the promise object, its resolution and
 * settlement, its reactions, and the jobs that run them in the realm's
 * queue of microtasks.  The Promise constructor and its methods
 * (js/builtin_promise.c) and await (generator.c) are built on these.
 *
 * A reaction waits in its promise's list until the promise settles and is
 * then queued as a microtask (a cell of vm_promise_job_type in the realm's
 * queue, which vm_run_jobs hands to vm_promise_run_job).  A promise
 * rejected while nothing handles it is kept in the realm's list of
 * rejections, which the end of the checkpoint reports if nothing handled
 * it meanwhile.
 */

#include "vm/bytecode.h"
#include "vm/internal.h"

#include <errno.h>
#include <string.h>

/*
 * The shared state of a pair of resolving functions: the promise they
 * settle and whether one of them has been called (after which both do
 * nothing).  Each function holds the record as its data.
 */
struct promise_record {
	struct vm_cell cell;
	vm_value promise;
	int already_resolved;
	int reserved;
};

static void promise_trace(struct vm_heap *heap, struct vm_cell *cell);
static void promise_job_trace(struct vm_heap *heap, struct vm_cell *cell);
static void promise_record_trace(struct vm_heap *heap, struct vm_cell *cell);
static struct vm_promise_job *promise_job_create(struct vm_realm *realm, uint32_t kind);
static int promise_resolve_with(struct vm_realm *realm, vm_value promise, vm_value resolution);
static int promise_settle(struct vm_realm *realm, vm_value promise, uint32_t state, vm_value value);
static int promise_add_job(struct vm_realm *realm, struct vm_promise *promise, struct vm_promise_job *job);
static int promise_run_reaction(struct vm_realm *realm, struct vm_promise_job *job, vm_value argument);
static int promise_run_thenable(struct vm_realm *realm, struct vm_promise_job *job, vm_value argument);
static int promise_resolve_function(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int promise_reject_function(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* The cell type of promises: objects that also hold their result and their reactions. */
const struct vm_cell_type vm_promise_type = {
	"promise", promise_trace, vm_object_finalize
};

/* The cell type of a promise's jobs: they hold their handlers, the promise they settle and the run they resume. */
const struct vm_cell_type vm_promise_job_type = {
	"promise job", promise_job_trace, NULL
};

/* The cell type of the record a pair of resolving functions share. */
static const struct vm_cell_type promise_record_type = {
	"promise resolution", promise_record_trace, NULL
};

/*
 * Tells whether a value is a promise (an object made by vm_promise_create).
 */
int
vm_value_is_promise(
	vm_value value)
{
	struct vm_cell *cell;
	int is_cell;

	/* Only a cell can be one. */
	is_cell = vm_value_is_cell(value);
	if (!is_cell)
		return 0;

	/* A promise's cell has the promise type. */
	cell = vm_value_as_cell(value);
	if (cell->type == &vm_promise_type)
		return 1;

	/* Another cell. */
	return 0;
}

/*
 * Makes a pending promise with a prototype (NULL for Promise.prototype, or
 * Object.prototype before the built-ins).
 */
int
vm_promise_create(
	struct vm_realm *realm,
	struct vm_object *prototype,
	vm_value *promise)
{
	struct vm_promise *made;
	int error;

	/* The prototype the promise gets when none is given. */
	*promise = VM_VALUE_UNDEFINED;
	if (prototype == NULL)
		prototype = realm->intrinsics[VM_INTRINSIC_PROMISE_PROTOTYPE];
	if (prototype == NULL)
		prototype = realm->object_prototype;

	/* The cell, an object of the promise kind. */
	made = vm_heap_alloc(realm->heap, &vm_promise_type, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	error = vm_object_init(realm->heap, &made->object, prototype);
	if (error != 0)
		return error;
	made->object.kind = VM_KIND_PROMISE;

	/* Pending, with no reactions yet. */
	made->state = VM_PROMISE_PENDING;
	made->result = VM_VALUE_UNDEFINED;
	made->first = NULL;
	made->last = NULL;

	/* Succeeded: the promise. */
	*promise = vm_value_cell(made);
	return 0;
}

/*
 * Reports a promise's state (VM_PROMISE_*) and, once it is settled, its
 * value or reason.
 */
int
vm_promise_state(
	vm_value promise,
	vm_value *result)
{
	struct vm_promise *object;

	/* The state and the result. */
	object = (struct vm_promise *)vm_value_as_cell(promise);
	*result = object->result;

	/* Reports the state. */
	return (int)object->state;
}

/*
 * Makes the pair of resolving functions of a promise (what a Promise
 * executor receives): the first call of either settles or resolves the
 * promise, and every later call does nothing.
 */
int
vm_promise_resolving_functions(
	struct vm_realm *realm,
	vm_value promise,
	vm_value *resolve,
	vm_value *reject)
{
	struct promise_record *record;
	struct vm_function *function;

	/* The record the two share. */
	record = vm_heap_alloc(realm->heap, &promise_record_type, sizeof(*record));
	if (record == NULL)
		return ENOMEM;
	record->promise = promise;
	record->already_resolved = 0;

	/* The resolve function. */
	function = vm_function_create_native(realm, "", 1, promise_resolve_function);
	if (function == NULL)
		return ENOMEM;
	function->data = vm_value_cell(record);
	*resolve = vm_value_cell(function);

	/* The reject function. */
	function = vm_function_create_native(realm, "", 1, promise_reject_function);
	if (function == NULL)
		return ENOMEM;
	function->data = vm_value_cell(record);
	*reject = vm_value_cell(function);

	/* Succeeded: the pair. */
	return 0;
}

/*
 * Resolves a promise the engine made itself (the promise of an async
 * function, of then, of Promise.resolve): with a thenable it follows the
 * thenable, with anything else it is fulfilled.  Only the first
 * resolution or rejection counts.  Returns 0 or an errno value.
 */
int
vm_promise_resolve(
	struct vm_realm *realm,
	vm_value promise,
	vm_value resolution)
{
	struct vm_promise *object;
	int status;

	/* A promise whose resolution is decided keeps it. */
	object = (struct vm_promise *)vm_value_as_cell(promise);
	if (object->resolved)
		return 0;
	object->resolved = 1;

	/* The resolution. */
	status = promise_resolve_with(realm, promise, resolution);
	if (status != 0)
		return status;

	/* Succeeded: the promise is resolved. */
	return 0;
}

/*
 * Rejects a promise the engine made itself with a reason, unless its
 * resolution is already decided.  Returns 0 or an errno value.
 */
int
vm_promise_reject(
	struct vm_realm *realm,
	vm_value promise,
	vm_value reason)
{
	struct vm_promise *object;
	int status;

	/* A promise whose resolution is decided keeps it. */
	object = (struct vm_promise *)vm_value_as_cell(promise);
	if (object->resolved)
		return 0;
	object->resolved = 1;

	/* The rejection. */
	status = promise_settle(realm, promise, VM_PROMISE_REJECTED, reason);
	if (status != 0)
		return status;

	/* Succeeded: the promise is rejected. */
	return 0;
}

/*
 * Adds a reaction to a promise (PerformPromiseThen): when it settles,
 * on_fulfilled or on_rejected (either undefined for none) runs in a
 * microtask, and derived (undefined for none) is resolved with what the
 * handler returns or rejected with what it throws.  resolve and reject
 * are derived's resolving functions, or the empty value when the engine
 * made derived and settles it directly.  The promise counts as handled
 * from then on.
 */
int
vm_promise_then(
	struct vm_realm *realm,
	vm_value promise,
	vm_value on_fulfilled,
	vm_value on_rejected,
	vm_value derived,
	vm_value resolve,
	vm_value reject)
{
	struct vm_promise_job *job;
	int callable;
	int status;

	/* The reaction, with a handler only where one can be called. */
	job = promise_job_create(realm, VM_JOB_REACTION);
	if (job == NULL)
		return ENOMEM;
	callable = vm_value_is_callable(on_fulfilled);
	if (callable)
		job->on_fulfilled = on_fulfilled;
	callable = vm_value_is_callable(on_rejected);
	if (callable)
		job->on_rejected = on_rejected;
	job->derived = derived;
	job->resolve = resolve;
	job->reject = reject;

	/* It waits for the promise, or is queued at once when the promise is settled. */
	status = promise_add_job(realm, (struct vm_promise *)vm_value_as_cell(promise), job);
	if (status != 0)
		return status;

	/* Succeeded: the reaction is added. */
	return 0;
}

/*
 * Makes an async function's run wait for a value (await): the value
 * itself when it is a promise made by Promise, otherwise a new promise
 * resolved with it; when that promise settles, a microtask resumes the
 * run.  Returns 0, VM_THROWN (reading a promise's constructor threw) or an
 * errno value.
 */
int
vm_promise_await(
	struct vm_realm *realm,
	vm_value value,
	struct vm_generator *generator)
{
	struct vm_promise_job *job;
	vm_value promise;
	vm_value constructor;
	vm_value intrinsic;
	vm_value key;
	int is_promise;
	int status;

	/* A promise whose constructor is Promise is awaited as it is (PromiseResolve). */
	promise = VM_VALUE_UNDEFINED;
	is_promise = vm_value_is_promise(value);
	if (is_promise) {
		key = vm_key_from_ascii(realm->heap, "constructor");
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;
		status = vm_get(realm, value, key, &constructor);
		if (status != 0)
			return status;
		intrinsic = VM_VALUE_UNDEFINED;
		if (realm->intrinsics[VM_INTRINSIC_PROMISE] != NULL)
			intrinsic = vm_value_cell(realm->intrinsics[VM_INTRINSIC_PROMISE]);
		if (constructor == intrinsic)
			promise = value;
	}

	/* Anything else becomes a new promise resolved with it. */
	if (promise == VM_VALUE_UNDEFINED) {
		status = vm_promise_create(realm, NULL, &promise);
		if (status != 0)
			return status;
		status = vm_promise_resolve(realm, promise, value);
		if (status != 0)
			return status;
	}

	/* The job that resumes the run once the promise settles. */
	job = promise_job_create(realm, VM_JOB_AWAIT);
	if (job == NULL)
		return ENOMEM;
	job->generator = generator;
	status = promise_add_job(realm, (struct vm_promise *)vm_value_as_cell(promise), job);
	if (status != 0)
		return status;

	/* Succeeded: the run waits. */
	return 0;
}

/*
 * Runs a promise's job from the realm's queue of microtasks with its
 * argument (the settled value or reason, or the thenable).  Returns 0,
 * VM_THROWN (the checkpoint reports it) or an errno value.
 */
int
vm_promise_run_job(
	struct vm_realm *realm,
	vm_value job_value,
	vm_value argument)
{
	struct vm_promise_job *job;
	int how;
	int status;

	/* Each kind of job. */
	job = (struct vm_promise_job *)vm_value_as_cell(job_value);
	switch (job->kind) {
	case VM_JOB_AWAIT:
		/* The async function goes on with the value, or with the reason thrown at its await. */
		how = VM_RESUME_NEXT;
		if (job->rejected)
			how = VM_RESUME_THROW;
		status = vm_generator_continue(realm, job->generator, argument, how);
		break;
	case VM_JOB_THENABLE:
		status = promise_run_thenable(realm, job, argument);
		break;
	default:
		status = promise_run_reaction(realm, job, argument);
		break;
	}

	/* What the job did not survive. */
	if (status != 0)
		return status;

	/* Succeeded: the job ran. */
	return 0;
}

/*
 * Marks the promises of a realm's list of rejections (the realm's tracer
 * calls this).
 */
void
vm_promise_trace_rejections(
	struct vm_heap *heap,
	struct vm_realm *realm)
{
	vm_value *promise;
	size_t index;

	/* Each promise rejected while unhandled. */
	for (index = 0; index < realm->rejections.count; index++) {
		promise = wb_vector_at(&realm->rejections, index);
		vm_heap_mark_value(heap, *promise);
	}
}

/* Marks what a promise refers to: what every object refers to, its result and its reactions. */
static void
promise_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_promise *promise;

	/* The object's shape, prototype, slots and elements. */
	vm_object_trace(heap, cell);

	/* The value or the reason, and the first reaction (each marks the next). */
	promise = (struct vm_promise *)cell;
	vm_heap_mark_value(heap, promise->result);
	if (promise->first != NULL)
		vm_heap_mark(heap, &promise->first->cell);
}

/* Marks what a promise's job refers to: the next reaction, its handlers, the promise it settles and the run it resumes. */
static void
promise_job_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_promise_job *job;

	/* The next reaction in its promise's list. */
	job = (struct vm_promise_job *)cell;
	if (job->next != NULL)
		vm_heap_mark(heap, &job->next->cell);

	/* The handlers, the derived promise and its functions. */
	vm_heap_mark_value(heap, job->on_fulfilled);
	vm_heap_mark_value(heap, job->on_rejected);
	vm_heap_mark_value(heap, job->derived);
	vm_heap_mark_value(heap, job->resolve);
	vm_heap_mark_value(heap, job->reject);

	/* The async function's run. */
	if (job->generator != NULL)
		vm_heap_mark(heap, &job->generator->cell);
}

/* Marks the promise a pair of resolving functions settles. */
static void
promise_record_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct promise_record *record;

	/* The promise. */
	record = (struct promise_record *)cell;
	vm_heap_mark_value(heap, record->promise);
}

/* Makes a promise's job of a kind with nothing in it; NULL when out of memory. */
static struct vm_promise_job *
promise_job_create(
	struct vm_realm *realm,
	uint32_t kind)
{
	struct vm_promise_job *job;

	/* The cell (zeroed: no next, no run). */
	job = vm_heap_alloc(realm->heap, &vm_promise_job_type, sizeof(*job));
	if (job == NULL)
		return NULL;

	/* The kind; no handlers and no promise to settle yet. */
	job->kind = kind;
	job->on_fulfilled = VM_VALUE_UNDEFINED;
	job->on_rejected = VM_VALUE_UNDEFINED;
	job->derived = VM_VALUE_UNDEFINED;
	job->resolve = VM_VALUE_EMPTY;
	job->reject = VM_VALUE_EMPTY;

	/* Succeeded: the job. */
	return job;
}

/*
 * Resolves a promise with a value, whoever decided the resolution: the
 * promise itself is a TypeError, a thenable is followed (a job calls its
 * then), and anything else fulfills the promise.
 */
static int
promise_resolve_with(
	struct vm_realm *realm,
	vm_value promise,
	vm_value resolution)
{
	struct vm_promise_job *job;
	vm_value error_value;
	vm_value reason;
	vm_value then;
	vm_value key;
	int is_object;
	int callable;
	int status;

	/* A promise resolved with itself can never settle: it is rejected. */
	if (resolution == promise) {
		status = vm_error_create(realm, VM_ERROR_TYPE, "Chaining cycle detected for promise #<Promise>", &error_value);
		if (status != 0)
			return status;
		status = promise_settle(realm, promise, VM_PROMISE_REJECTED, error_value);
		return status;
	}

	/* A value that is not an object fulfills it. */
	is_object = vm_value_is_object(resolution);
	if (!is_object) {
		status = promise_settle(realm, promise, VM_PROMISE_FULFILLED, resolution);
		return status;
	}

	/* An object's then; reading it may throw, which rejects the promise. */
	key = vm_key_from_ascii(realm->heap, "then");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_get(realm, resolution, key, &then);
	if (status == VM_THROWN) {
		reason = realm->exception;
		realm->exception = VM_VALUE_UNDEFINED;
		status = promise_settle(realm, promise, VM_PROMISE_REJECTED, reason);
		return status;
	}

	/* What failed goes back to the caller. */
	if (status != 0)
		return status;

	/* An object without a then to call fulfills it. */
	callable = vm_value_is_callable(then);
	if (!callable) {
		status = promise_settle(realm, promise, VM_PROMISE_FULFILLED, resolution);
		return status;
	}

	/* A thenable: a job calls its then with the promise's resolving functions. */
	job = promise_job_create(realm, VM_JOB_THENABLE);
	if (job == NULL)
		return ENOMEM;
	job->derived = promise;
	job->on_fulfilled = then;
	status = vm_enqueue_job(realm, vm_value_cell(job), resolution);
	if (status != 0)
		return status;

	/* Succeeded: the promise follows the thenable. */
	return 0;
}

/*
 * Settles a pending promise (a settled one stays as it is): its state and
 * result, its reactions queued in order, and a rejection nothing handles
 * kept for the checkpoint's report.
 */
static int
promise_settle(
	struct vm_realm *realm,
	vm_value promise,
	uint32_t state,
	vm_value value)
{
	struct vm_promise *object;
	struct vm_promise_job *job;
	int status;

	/* A promise settles only once. */
	object = (struct vm_promise *)vm_value_as_cell(promise);
	if (object->state != VM_PROMISE_PENDING)
		return 0;

	/* Its state and its value or reason. */
	object->state = state;
	object->result = value;

	/* Each waiting reaction is queued in the order it came, with the value or the reason. */
	for (job = object->first; job != NULL; job = job->next) {
		if (state == VM_PROMISE_REJECTED)
			job->rejected = 1;
		status = vm_enqueue_job(realm, vm_value_cell(job), value);
		if (status != 0)
			return status;
	}

	/* The list is spent (the queue holds the jobs now). */
	object->first = NULL;
	object->last = NULL;

	/* A rejection nothing handles yet is kept for the checkpoint's report. */
	if (state == VM_PROMISE_REJECTED && !object->handled) {
		status = wb_vector_push(&realm->rejections, &promise);
		if (status != 0)
			return status;
	}

	/* Succeeded: the promise is settled. */
	return 0;
}

/*
 * Adds a job to a promise: at the end of the list while it is pending,
 * into the queue at once when it is settled.  The promise is handled from
 * then on.
 */
static int
promise_add_job(
	struct vm_realm *realm,
	struct vm_promise *promise,
	struct vm_promise_job *job)
{
	int status;

	/* Any reaction handles the promise (its rejection is not reported). */
	promise->handled = 1;

	/* A pending promise keeps the job until it settles. */
	if (promise->state == VM_PROMISE_PENDING) {
		if (promise->last == NULL) {
			promise->first = job;
		} else {
			promise->last->next = job;
		}

		/* The job is the list's last now. */
		promise->last = job;
		return 0;
	}

	/* A settled promise queues the job now, with its value or reason. */
	if (promise->state == VM_PROMISE_REJECTED)
		job->rejected = 1;
	status = vm_enqueue_job(realm, vm_value_cell(job), promise->result);
	if (status != 0)
		return status;

	/* Succeeded: the job is queued. */
	return 0;
}

/*
 * Runs a reaction (NewPromiseReactionJob): its handler for the way the
 * promise settled with the argument, or the argument itself without one,
 * and the derived promise resolved or rejected with the outcome.
 */
static int
promise_run_reaction(
	struct vm_realm *realm,
	struct vm_promise_job *job,
	vm_value argument)
{
	vm_value handler;
	vm_value value;
	vm_value ignored;
	int rejects;
	int status;

	/* The handler for the way the promise settled. */
	handler = job->on_fulfilled;
	if (job->rejected)
		handler = job->on_rejected;

	/* Without one, the value passes on the way it came. */
	if (handler == VM_VALUE_UNDEFINED) {
		value = argument;
		rejects = (int)job->rejected;
	} else {
		/* The handler's return value resolves, what it throws rejects. */
		status = vm_call(realm, handler, VM_VALUE_UNDEFINED, &argument, 1, &value);
		rejects = 0;
		if (status == VM_THROWN) {
			value = realm->exception;
			realm->exception = VM_VALUE_UNDEFINED;
			rejects = 1;
		} else if (status != 0) {
			return status;
		}
	}

	/* No derived promise: nothing more to do. */
	if (job->derived == VM_VALUE_UNDEFINED)
		return 0;

	/* A promise the engine made is settled directly. */
	if (job->resolve == VM_VALUE_EMPTY) {
		if (rejects) {
			status = vm_promise_reject(realm, job->derived, value);
		} else {
			status = vm_promise_resolve(realm, job->derived, value);
		}

		/* Reports how the settlement went. */
		return status;
	}

	/* Another's promise, through its resolving functions (what they throw is the job's). */
	if (rejects) {
		status = vm_call(realm, job->reject, VM_VALUE_UNDEFINED, &value, 1, &ignored);
	} else {
		status = vm_call(realm, job->resolve, VM_VALUE_UNDEFINED, &value, 1, &ignored);
	}

	/* What failed goes back to the caller. */
	if (status != 0)
		return status;

	/* Succeeded: the derived promise has the outcome. */
	return 0;
}

/*
 * Runs a thenable's job (NewPromiseResolveThenableJob): its then called
 * with a new pair of resolving functions of the promise; what then throws
 * rejects the promise unless it was resolved already.
 */
static int
promise_run_thenable(
	struct vm_realm *realm,
	struct vm_promise_job *job,
	vm_value argument)
{
	vm_value functions[2];
	vm_value reason;
	vm_value ignored;
	int status;

	/* The promise's resolving functions. */
	status = vm_promise_resolving_functions(realm, job->derived, &functions[0], &functions[1]);
	if (status != 0)
		return status;

	/* then, called on the thenable. */
	status = vm_call(realm, job->on_fulfilled, argument, functions, 2, &ignored);
	if (status == VM_THROWN) {
		reason = realm->exception;
		realm->exception = VM_VALUE_UNDEFINED;
		status = vm_call(realm, functions[1], VM_VALUE_UNDEFINED, &reason, 1, &ignored);
	}

	/* What failed goes back to the caller. */
	if (status != 0)
		return status;

	/* Succeeded: the thenable has the promise's functions. */
	return 0;
}

/* A promise's resolve function: the first call of the pair resolves the promise with its argument. */
static int
promise_resolve_function(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *function;
	struct promise_record *record;
	vm_value resolution;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The pair's record; a later call does nothing. */
	*result = VM_VALUE_UNDEFINED;
	function = (struct vm_function *)vm_value_as_cell(realm->callee);
	record = (struct promise_record *)vm_value_as_cell(function->data);
	if (record->already_resolved)
		return 0;
	record->already_resolved = 1;

	/* The resolution (undefined when none is given). */
	resolution = VM_VALUE_UNDEFINED;
	if (count > 0)
		resolution = args[0];
	status = promise_resolve_with(realm, record->promise, resolution);
	if (status != 0)
		return status;

	/* Succeeded: undefined. */
	return 0;
}

/* A promise's reject function: the first call of the pair rejects the promise with its argument. */
static int
promise_reject_function(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *function;
	struct promise_record *record;
	vm_value reason;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The pair's record; a later call does nothing. */
	*result = VM_VALUE_UNDEFINED;
	function = (struct vm_function *)vm_value_as_cell(realm->callee);
	record = (struct promise_record *)vm_value_as_cell(function->data);
	if (record->already_resolved)
		return 0;
	record->already_resolved = 1;

	/* The reason (undefined when none is given). */
	reason = VM_VALUE_UNDEFINED;
	if (count > 0)
		reason = args[0];
	status = promise_settle(realm, record->promise, VM_PROMISE_REJECTED, reason);
	if (status != 0)
		return status;

	/* Succeeded: undefined. */
	return 0;
}
