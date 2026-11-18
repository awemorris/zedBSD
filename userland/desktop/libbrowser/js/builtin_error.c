/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Error objects: Error and the native errors (TypeError, RangeError,
 * ReferenceError, SyntaxError, EvalError, URIError), their prototypes,
 * Error.prototype.toString, and %ThrowTypeError% (the accessor of strict
 * code's arguments.callee).
 *
 * Every error constructor runs the same native code; the function's data
 * says which kind it makes.  The engine's own errors (vm_throw_error) are
 * made from the same prototypes once they are installed.
 */

#include "js/builtin.h"

#include <errno.h>
#include <string.h>

/*
 * The names of the error kinds, in the order of enum vm_error_kind.  The
 * table is constant for the life of the program.
 */
static const char *const error_names[] = {
	"Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "EvalError", "URIError"
};

static int error_create(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int error_to_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int error_throw_type_error(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int error_install_kind(struct vm_realm *realm, int kind, struct vm_function *base, struct vm_function **constructor);
static int error_throw_type_error_make(struct vm_realm *realm);

/*
 * Installs Error, the native errors and %ThrowTypeError%.
 */
int
js_builtin_install_error(
	struct vm_realm *realm)
{
	struct vm_function *base;
	struct vm_function *constructor;
	struct vm_object *prototype;
	vm_value empty;
	int kind;
	int error;

	/* Error.prototype: its name, an empty message and toString. */
	prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (prototype == NULL)
		return ENOMEM;
	realm->intrinsics[VM_INTRINSIC_ERROR_PROTOTYPE] = prototype;
	error = js_builtin_string(realm, "", &empty);
	if (error == 0)
		error = js_builtin_value(realm, prototype, "message", empty, JS_BUILTIN_METHOD);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "toString", 0, error_to_string);
	if (error != 0)
		return error;

	/* Error itself (its data says the kind), and its prototype's name. */
	error = error_install_kind(realm, VM_ERROR_PLAIN, NULL, &base);
	if (error != 0)
		return error;

	/* The native errors, each a child of Error. */
	for (kind = VM_ERROR_TYPE; kind <= VM_ERROR_URI; kind++) {
		error = error_install_kind(realm, kind, base, &constructor);
		if (error != 0)
			return error;
	}

	/* %ThrowTypeError%. */
	error = error_throw_type_error_make(realm);
	if (error != 0)
		return error;

	/* Succeeded: the errors are installed. */
	return 0;
}

/* Makes one error kind's prototype (a child of Error.prototype for a native error) and constructor. */
static int
error_install_kind(
	struct vm_realm *realm,
	int kind,
	struct vm_function *base,
	struct vm_function **made)
{
	struct vm_function *constructor;
	struct vm_object *prototype;
	vm_value name;
	vm_value empty;
	int error;

	/* The prototype: Error.prototype itself, or a new child of it with an empty message. */
	prototype = realm->intrinsics[VM_INTRINSIC_ERROR_PROTOTYPE];
	if (kind != VM_ERROR_PLAIN) {
		prototype = vm_object_create(realm->heap, realm->intrinsics[VM_INTRINSIC_ERROR_PROTOTYPE]);
		if (prototype == NULL)
			return ENOMEM;
		realm->intrinsics[VM_INTRINSIC_ERROR_PROTOTYPE + kind] = prototype;
		error = js_builtin_string(realm, "", &empty);
		if (error == 0)
			error = js_builtin_value(realm, prototype, "message", empty, JS_BUILTIN_METHOD);
		if (error != 0)
			return error;
	}

	/* The prototype's name. */
	error = js_builtin_string(realm, error_names[kind], &name);
	if (error == 0)
		error = js_builtin_value(realm, prototype, "name", name, JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* The constructor (called or constructed alike), which knows its kind from its data. */
	error = js_builtin_constructor(realm, error_names[kind], 1, error_create, error_create, prototype, &constructor);
	if (error != 0)
		return error;
	constructor->data = vm_value_int32(kind);

	/* A native error's constructor inherits from Error. */
	if (base != NULL)
		constructor->object.prototype = &base->object;

	/* Succeeded: the kind is installed. */
	*made = constructor;
	return 0;
}

/* Makes an error of the constructor's kind (Error(message, options), with or without new). */
static int
error_create(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *constructor;
	struct vm_object *prototype;
	struct vm_object *made;
	struct vm_string *message;
	vm_value new_target;
	vm_value message_value;
	vm_value options;
	vm_value key;
	vm_value cause;
	vm_value has_cause;
	int kind;
	int is_object;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The constructor and new.target (the constructor itself when called without new). */
	constructor = js_builtin_callee(realm);
	kind = vm_value_as_int32(constructor->data);
	new_target = realm->new_target;
	if (new_target == VM_VALUE_UNDEFINED)
		new_target = vm_value_cell(constructor);

	/* The object, from new.target's prototype. */
	status = vm_construct_prototype(realm, new_target, realm->intrinsics[VM_INTRINSIC_ERROR_PROTOTYPE + kind], &prototype);
	if (status != 0)
		return status;
	made = vm_object_create(realm->heap, prototype);
	if (made == NULL)
		return ENOMEM;
	made->kind = VM_KIND_ERROR;

	/* The message, when given. */
	message_value = js_argument(args, count, 0);
	if (message_value != VM_VALUE_UNDEFINED) {
		status = vm_to_string(realm, args[0], &message);
		if (status != 0)
			return status;
		status = js_builtin_value(realm, made, "message", vm_value_cell(message), JS_BUILTIN_METHOD);
		if (status != 0)
			return status;
	}

	/* The cause, when the options have one. */
	options = js_argument(args, count, 1);
	is_object = vm_value_is_object(options);
	if (is_object) {
		key = vm_key_from_ascii(realm->heap, "cause");
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;
		status = vm_in(realm, key, options, &has_cause);
		if (status != 0)
			return status;
		if (has_cause == VM_VALUE_TRUE) {
			status = vm_get(realm, options, key, &cause);
			if (status != 0)
				return status;
			status = js_builtin_value(realm, made, "cause", cause, JS_BUILTIN_METHOD);
			if (status != 0)
				return status;
		}
	}

	/* Succeeded: the error. */
	*result = vm_value_cell(made);
	return 0;
}

/* Writes an error as "Name: message" (Error.prototype.toString). */
static int
error_to_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *name;
	struct vm_string *message;
	struct vm_string *separator;
	struct vm_string *joined;
	vm_value key;
	vm_value value;
	int is_object;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Only an object has a name and a message. */
	is_object = vm_value_is_object(this_value);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Error.prototype.toString called on a non-object");
		return status;
	}

	/* The name ("Error" when undefined). */
	key = vm_key_from_ascii(realm->heap, "name");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_get(realm, this_value, key, &value);
	if (status != 0)
		return status;
	if (value == VM_VALUE_UNDEFINED) {
		status = js_builtin_string(realm, "Error", &value);
		if (status != 0)
			return status;
	}

	/* As a string. */
	status = vm_to_string(realm, value, &name);
	if (status != 0)
		return status;

	/* The message ("" when undefined). */
	key = vm_key_from_ascii(realm->heap, "message");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_get(realm, this_value, key, &value);
	if (status != 0)
		return status;
	if (value == VM_VALUE_UNDEFINED) {
		status = js_builtin_string(realm, "", &value);
		if (status != 0)
			return status;
	}

	/* As a string. */
	status = vm_to_string(realm, value, &message);
	if (status != 0)
		return status;

	/* An empty name gives the message; an empty message the name. */
	if (name->length == 0) {
		*result = vm_value_cell(message);
		return 0;
	}

	/* An empty message gives the name. */
	if (message->length == 0) {
		*result = vm_value_cell(name);
		return 0;
	}

	/* Otherwise both with ": " between them. */
	separator = vm_string_from_utf8(realm->heap, ": ", 2);
	if (separator == NULL)
		return ENOMEM;
	joined = vm_string_concat(realm->heap, name, separator);
	if (joined == NULL)
		return ENOMEM;
	joined = vm_string_concat(realm->heap, joined, message);
	if (joined == NULL)
		return ENOMEM;

	/* Succeeded: the text. */
	*result = vm_value_cell(joined);
	return 0;
}

/* Throws the TypeError of %ThrowTypeError%. */
static int
error_throw_type_error(
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

	/* Always the error. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_throw_type_error(realm, "'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions");
	return status;
}

/* Makes %ThrowTypeError%: a function that throws, not extensible, with a fixed name and length. */
static int
error_throw_type_error_make(
	struct vm_realm *realm)
{
	struct vm_function *function;
	vm_value empty;
	int error;

	/* The function. */
	error = js_builtin_function(realm, "", 0, error_throw_type_error, NULL, &function);
	if (error != 0)
		return error;

	/* Its name and length cannot change. */
	error = js_builtin_string(realm, "", &empty);
	if (error == 0)
		error = js_builtin_value(realm, &function->object, "name", empty, 0);
	if (error == 0)
		error = js_builtin_value(realm, &function->object, "length", vm_value_int32(0), 0);
	if (error != 0)
		return error;

	/* It takes no new property. */
	function->object.flags |= VM_OBJECT_NOT_EXTENSIBLE;
	realm->intrinsics[VM_INTRINSIC_THROW_TYPE_ERROR] = &function->object;

	/* Succeeded: %ThrowTypeError% exists. */
	return 0;
}
