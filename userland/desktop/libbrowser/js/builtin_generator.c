/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The objects of generators and async functions (ws074-p086):
 * %GeneratorFunction.prototype% (what a generator function inherits
 * from), %GeneratorPrototype% with next, return and throw (what its
 * generators inherit from, through the function's prototype object), and
 * %AsyncFunction.prototype%.
 *
 * Not in this pass: the GeneratorFunction and AsyncFunction constructors
 * (functions from source text).  The toStringTag properties come from
 * js_builtin_install_tags (ws074-p087).
 */

#include "js/builtin.h"

#include <errno.h>

static int generator_next(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int generator_return(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int generator_throw(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int generator_step(struct vm_realm *realm, vm_value generator, vm_value value, int how, vm_value *result);

/*
 * Installs the prototypes of generators and async functions: they are
 * reached from the functions and generators the scripts make, not from
 * the global object.
 */
int
js_builtin_install_generator(
	struct vm_realm *realm)
{
	struct vm_object *iterator_prototype;
	struct vm_object *generator_prototype;
	struct vm_object *function_prototype;
	struct vm_object *async_prototype;
	int error;

	/* %IteratorPrototype% (builtin_iterator.c), which every built-in iterator inherits from. */
	iterator_prototype = realm->intrinsics[VM_INTRINSIC_ITERATOR_PROTOTYPE];

	/* %GeneratorPrototype%, with the methods that resume a generator. */
	generator_prototype = vm_object_create(realm->heap, iterator_prototype);
	if (generator_prototype == NULL)
		return ENOMEM;
	realm->intrinsics[VM_INTRINSIC_GENERATOR_PROTOTYPE] = generator_prototype;
	error = js_builtin_method(realm, generator_prototype, "next", 1, generator_next);
	if (error == 0)
		error = js_builtin_method(realm, generator_prototype, "return", 1, generator_return);
	if (error == 0)
		error = js_builtin_method(realm, generator_prototype, "throw", 1, generator_throw);
	if (error != 0)
		return error;

	/* %GeneratorFunction.prototype%, a function's prototype, linked both ways to %GeneratorPrototype% (configurable only). */
	function_prototype = vm_object_create(realm->heap, realm->function_prototype);
	if (function_prototype == NULL)
		return ENOMEM;
	realm->intrinsics[VM_INTRINSIC_GENERATOR_FUNCTION_PROTOTYPE] = function_prototype;
	error = js_builtin_value(realm, function_prototype, "prototype", vm_value_cell(generator_prototype), VM_PROPERTY_CONFIGURABLE);
	if (error == 0)
		error = js_builtin_value(realm, generator_prototype, "constructor", vm_value_cell(function_prototype), VM_PROPERTY_CONFIGURABLE);
	if (error != 0)
		return error;

	/* %AsyncFunction.prototype%. */
	async_prototype = vm_object_create(realm->heap, realm->function_prototype);
	if (async_prototype == NULL)
		return ENOMEM;
	realm->intrinsics[VM_INTRINSIC_ASYNC_FUNCTION_PROTOTYPE] = async_prototype;

	/* Succeeded: the prototypes are installed. */
	return 0;
}

/* %GeneratorPrototype%.next(value): the generator runs to its next yield. */
static int
generator_next(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Resumed with the value. */
	status = generator_step(realm, this_value, js_argument(args, count, 0), VM_RESUME_NEXT, result);
	if (status != 0)
		return status;

	/* Succeeded: the iterator result. */
	return 0;
}

/* %GeneratorPrototype%.return(value): the generator returns the value from where it is (its finally blocks run). */
static int
generator_return(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Resumed with a return. */
	status = generator_step(realm, this_value, js_argument(args, count, 0), VM_RESUME_RETURN, result);
	if (status != 0)
		return status;

	/* Succeeded: the iterator result. */
	return 0;
}

/* %GeneratorPrototype%.throw(exception): the exception is thrown where the generator is. */
static int
generator_throw(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Resumed with a throw. */
	status = generator_step(realm, this_value, js_argument(args, count, 0), VM_RESUME_THROW, result);
	if (status != 0)
		return status;

	/* Succeeded: the iterator result. */
	return 0;
}

/* Resumes a generator and makes the iterator result { value, done } of what it came to. */
static int
generator_step(
	struct vm_realm *realm,
	vm_value generator,
	vm_value value,
	int how,
	vm_value *result)
{
	struct vm_object *object;
	vm_value produced;
	int done;
	int direct_result;
	int status;

	/* The generator's step. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_generator_resume(realm, generator, value, how, &produced, &done, &direct_result);
	if (status != 0)
		return status;

	/* Delegation exposes the original object without invoking its value getter. */
	if (direct_result) {
		*result = produced;
		return 0;
	}

	/* The iterator result. */
	object = vm_object_create(realm->heap, realm->object_prototype);
	if (object == NULL)
		return ENOMEM;
	status = js_builtin_value(realm, object, "value", produced, VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, object, "done", vm_value_boolean(done), VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* Succeeded: the result. */
	*result = vm_value_cell(object);
	return 0;
}
