/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The built-in objects of a realm (plan/ws074/design.md §12.3): the
 * installer that puts them on the global object, and the helpers the
 * groups share to make functions, constructors and properties.
 *
 * The Error objects come first, so a failure while the others are made is
 * already an Error; then Object, Function, Boolean, Number, Math and the
 * global functions.  Array, String and JSON arrive in ws074-p046.
 */

#include "js/builtin.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Installs the built-in objects on a realm's global object.
 */
int
js_install_builtins(
	struct vm_realm *realm)
{
	int error;

	/* Installs Error constructors before later failures need native exception objects. */
	error = js_builtin_install_error(realm);
	if (error != 0)
		return error;

	/* Installs the Object prototype foundation used by ordinary builtin objects. */
	error = js_builtin_install_object(realm);
	if (error != 0)
		return error;

	/* Installs callable Function objects after their ordinary object foundation. */
	error = js_builtin_install_function(realm);
	if (error != 0)
		return error;

	/* Installs Symbol keys before subsequent collections use symbolic protocols. */
	error = js_builtin_install_symbol(realm);
	if (error != 0)
		return error;

	/* Installs Array construction and indexed storage operations. */
	error = js_builtin_install_array(realm);
	if (error != 0)
		return error;

	/* Installs String wrappers and native UTF16 operations. */
	error = js_builtin_install_string(realm);
	if (error != 0)
		return error;

	/* Installs regular expression objects after their String inputs are available. */
	error = js_builtin_install_regexp(realm);
	if (error != 0)
		return error;

	/* Installs Date objects and their established native time operations. */
	error = js_builtin_install_date(realm);
	if (error != 0)
		return error;

	/* Installs structured JSON conversion after its object and string dependencies. */
	error = js_builtin_install_json(realm);
	if (error != 0)
		return error;

	/* Installs Boolean wrappers and their primitive conversions. */
	error = js_builtin_install_boolean(realm);
	if (error != 0)
		return error;

	/* Installs Number wrappers and numeric conversion methods. */
	error = js_builtin_install_number(realm);
	if (error != 0)
		return error;

	/* Installs BigInt beside Number. */
	error = js_builtin_install_bigint(realm);
	if (error != 0)
		return error;

	/* Installs ArrayBuffer, the typed arrays and DataView (their prelude needs Array, Object and Symbol). */
	error = js_builtin_install_typed(realm);
	if (error != 0)
		return error;

	/* Installs the shared Math operations after numeric objects are ready. */
	error = js_builtin_install_math(realm);
	if (error != 0)
		return error;

	/* Installs ordinary global conversion and evaluation functions. */
	error = js_builtin_install_global(realm);
	if (error != 0)
		return error;

	/* Installs URI encoding and decoding on the completed global object. */
	error = js_builtin_install_uri(realm);
	if (error != 0)
		return error;

	/* Installs the iterator prototype foundation for iterable builtin protocols. */
	error = js_builtin_install_iterator(realm);
	if (error != 0)
		return error;

	/* Installs Promise objects and their existing microtask protocol. */
	error = js_builtin_install_promise(realm);
	if (error != 0)
		return error;

	/* Installs generator objects and their suspended execution protocol. */
	error = js_builtin_install_generator(realm);
	if (error != 0)
		return error;

	/* Installs keyed collections after iterator and Symbol foundations. */
	error = js_builtin_install_collection(realm);
	if (error != 0)
		return error;

	/* Installs native Proxy after its callable, Array and property-protocol foundations are complete. */
	error = js_builtin_install_proxy(realm);
	if (error != 0)
		return error;

	/* Installs Reflect, whose functions call the same internal methods as Proxy's traps. */
	error = js_builtin_install_reflect(realm);
	if (error != 0)
		return error;

	/* Publishes builtin type tags only after all referenced objects exist. */
	error = js_builtin_install_tags(realm);
	if (error != 0)
		return error;

	/* Succeeded: the realm has its built-ins. */
	return 0;
}

/*
 * Creates a realm's native function with its name and arity.
 *
 * An optional constructor entry supplies the operation run by new.
 */
int
js_builtin_function(
	struct vm_realm *realm,
	const char *name,
	unsigned length,
	vm_native native,
	vm_native construct,
	struct vm_function **function)
{
	struct vm_function *made;

	/* The function. */
	made = vm_function_create_native(realm, name, length, native);
	if (made == NULL)
		return ENOMEM;

	/* Gives the allocated native function its established constructor entry point. */
	made->construct = construct;

	/* Publishes only the fully initialized native function. */
	*function = made;

	/* Succeeded: the caller receives its owned function cell. */
	return 0;
}

/*
 * Defines a non-enumerable native method on an ordinary object.
 *
 * The property remains writable and configurable.
 */
int
js_builtin_method(
	struct vm_realm *realm,
	struct vm_object *object,
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
	error = js_builtin_value(realm, object, name, vm_value_cell(function), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* Succeeded: the method is defined. */
	return 0;
}

/*
 * Defines a property of a built-in object by an ASCII name.
 */
int
js_builtin_value(
	struct vm_realm *realm,
	struct vm_object *object,
	const char *name,
	vm_value value,
	uint32_t attributes)
{
	vm_value key;
	int error;

	/* The name's key. */
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* The property. */
	error = vm_object_define(realm->heap, object, key, value, attributes);
	if (error != 0)
		return error;

	/* Succeeded: the property is defined. */
	return 0;
}

/*
 * Defines a configurable native accessor on an ordinary object.
 *
 * Its non-enumerable descriptor has a getter and an optional setter.
 */
int
js_builtin_accessor(
	struct vm_realm *realm,
	struct vm_object *object,
	const char *name,
	vm_native getter,
	vm_native setter)
{
	struct vm_function *getter_function;
	struct vm_function *setter_function;
	struct vm_accessor *accessor;
	char accessor_name[80];
	vm_value setter_value;
	int printed;
	int error;

	/* The getter, named "get NAME". */
	printed = snprintf(accessor_name, sizeof(accessor_name), "get %s", name);
	if (printed < 0)
		return EIO;

	/* Creates the getter only after its existing bounded name format succeeds. */
	error = js_builtin_function(realm, accessor_name, 0, getter, NULL, &getter_function);
	if (error != 0)
		return error;

	/* The setter, named "set NAME", when there is one. */
	setter_value = VM_VALUE_UNDEFINED;
	if (setter != NULL) {
		/* Formats the optional setter's existing bounded native name. */
		printed = snprintf(accessor_name, sizeof(accessor_name), "set %s", name);
		if (printed < 0)
			return EIO;

		/* Creates the setter after its native name is available. */
		error = js_builtin_function(realm, accessor_name, 1, setter, NULL, &setter_function);
		if (error != 0)
			return error;

		/* The completed setter value is retained by the following accessor factory. */
		setter_value = vm_value_cell(setter_function);
	}

	/* The pair as the property. */
	accessor = vm_accessor_create(realm->heap, vm_value_cell(getter_function), setter_value);
	if (accessor == NULL)
		return ENOMEM;

	/* Publishes the completed getter/setter pair under its actual property name. */
	error = js_builtin_value(realm, object, name, vm_value_cell(accessor), VM_PROPERTY_ACCESSOR | VM_PROPERTY_CONFIGURABLE);
	if (error != 0)
		return error;

	/* Succeeded: the accessor is defined. */
	return 0;
}

/*
 * Installs a built-in constructor and its reciprocal prototype properties.
 *
 * The constructor's prototype property cannot change; the constructor
 * is published on both its prototype and the global object.
 */
int
js_builtin_constructor(
	struct vm_realm *realm,
	const char *name,
	unsigned length,
	vm_native native,
	vm_native construct,
	struct vm_object *prototype,
	struct vm_function **function)
{
	struct vm_function *made;
	int error;

	/* The function. */
	error = js_builtin_function(realm, name, length, native, construct, &made);
	if (error != 0)
		return error;

	/* Its prototype, and the prototype's constructor. */
	error = js_builtin_value(realm, &made->object, "prototype", vm_value_cell(prototype), 0);
	if (error != 0)
		return error;

	/* Gives the prototype its reciprocal non-enumerable constructor property. */
	error = js_builtin_value(realm, prototype, "constructor", vm_value_cell(made), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* The global. */
	error = js_builtin_value(realm, realm->global, name, vm_value_cell(made), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* Publishes the constructor after its prototype and global properties are installed. */
	*function = made;

	/* Succeeded: the caller receives the installed constructor. */
	return 0;
}

/*
 * Reports the native function the realm is running (its callee).
 */
struct vm_function *
js_builtin_callee(
	const struct vm_realm *realm)
{
	struct vm_function *callee;

	/* The caller set it before the call. */
	callee = (struct vm_function *)vm_value_as_cell(realm->callee);

	/* Succeeded: the caller receives the native function for the current call. */
	return callee;
}

/*
 * Makes a string value from UTF-8 text.
 */
int
js_builtin_string(
	struct vm_realm *realm,
	const char *text,
	vm_value *value)
{
	struct vm_string *string;

	/* The string. */
	string = vm_string_from_utf8(realm->heap, text, strlen(text));
	if (string == NULL)
		return ENOMEM;

	/* Publishes the completed native string as its ordinary boxed value. */
	*value = vm_value_cell(string);

	/* Succeeded: the caller receives the converted UTF8 string. */
	return 0;
}

/*
 * Makes an array of values while retaining every input and the unpublished Array.
 */
int
js_builtin_array(
	struct vm_realm *realm,
	const vm_value *values,
	uint32_t count,
	vm_value *array)
{
	struct vm_object *made;
	struct vm_cell **roots;
	size_t root_count;
	size_t root_index;
	size_t registered;
	uint32_t index;
	int error;
	int is_cell;

	/* Reject missing elements or root-array size overflow before any allocation. */
	if (count != 0 && values == NULL)
		return EINVAL;

	/* Reserves one representable root slot beyond all caller input cells. */
	root_count = (size_t)count;
	if (root_count == SIZE_MAX)
		return EOVERFLOW;

	/* Includes the unpublished Array without overflowing its C allocation size. */
	root_count++;
	if (root_count > SIZE_MAX / sizeof(*roots))
		return EOVERFLOW;

	/* Every input cell survives Array creation before its element can be installed. */
	roots = calloc(root_count, sizeof(*roots));
	if (roots == NULL)
		return ENOMEM;

	/* Records every cell input before the Array factory can trigger collection. */
	for (index = 0; index < count; index++) {
		/* Non-cell values need no collector root in their corresponding slot. */
		is_cell = vm_value_is_cell(values[index]);
		if (is_cell)
			roots[index] = vm_value_as_cell(values[index]);
	}

	/* Reserve one last slot for the Array before any VM allocation. */
	registered = 0;
	error = 0;
	for (root_index = 0; root_index < root_count; root_index++) {
		/* Registers each stable C slot before any VM allocation starts. */
		error = vm_heap_add_root(realm->heap, &roots[root_index]);
		if (error != 0)
			goto cleanup;

		/* Only successfully registered slots carry a later removal obligation. */
		registered++;
	}

	/* The factory's own roots cover construction; this caller then retains its result. */
	made = vm_array_create(realm->heap, realm->array_prototype);
	if (made == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Retains the newly allocated Array through every property installation. */
	roots[count] = &made->cell;

	/* Assigns every element before the Array can escape to its caller. */
	for (index = 0; index < count; index++) {
		/* Each original input value enters its corresponding native indexed property. */
		error = vm_object_define(realm->heap, made, vm_value_int32((int32_t)index), values[index], VM_PROPERTY_DEFAULT);
		if (error != 0)
			goto cleanup;
	}

	/* Publishes only the fully populated Array. */
	*array = vm_value_cell(made);

cleanup:
	/* Every registered temporary slot is released, including failed property creation. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* The C-side slot table no longer carries any collector roots. */
	free(roots);

	/* Propagate the original allocation or element-definition failure. */
	if (error != 0)
		return error;

	/* Succeeded: only the returned Array owns its input values. */
	return 0;
}

/*
 * Converts a value to its integer-or-infinity representation.
 *
 * NaN becomes zero and the remaining numbers truncate towards zero.
 */
int
js_builtin_integer(
	struct vm_realm *realm,
	vm_value value,
	double *integer)
{
	double number;
	int status;

	/* The number. */
	status = vm_to_number(realm, value, &number);
	if (status != 0)
		return status;

	/* NaN is zero; infinities stay; the rest lose their fraction (and -0 its sign). */
	if (number != number) {
		*integer = 0.0;
		return 0;
	}

	/* The whole part (a -0 is 0). */
	*integer = trunc(number);

	/* Both zero signs use the existing positive-zero integer convention. */
	if (*integer == 0.0)
		*integer = 0.0;

	/* Succeeded: the integer. */
	return 0;
}

/*
 * Reads an array-like object's accepted native list length.
 *
 * The converted length property is capped at the engine's Array bound.
 */
int
js_builtin_length(
	struct vm_realm *realm,
	vm_value object,
	uint32_t *length)
{
	vm_value key;
	vm_value value;
	double number;
	int status;

	/* The length property. */
	*length = 0;
	key = vm_key_from_ascii(realm->heap, "length");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* Reads length only after its native property key is available. */
	status = vm_get(realm, object, key, &value);
	if (status != 0)
		return status;

	/* As an integer from 0 up (the engine's lists stop at 2^31). */
	status = js_builtin_integer(realm, value, &number);
	if (status != 0)
		return status;

	/* Nonpositive array-like lengths have the existing zero result. */
	if (number <= 0.0)
		return 0;

	/* The engine's established list bound remains an explicit range refusal. */
	if (number > 2147483647.0) {
		status = vm_throw_range_error(realm, "Invalid array length");
		if (status != 0)
			return status;

		/* Succeeded: the bound refusal follows the existing native throw contract. */
		return 0;
	}

	/* Publishes the completed representable array-like length. */
	*length = (uint32_t)number;

	/* Succeeded: the caller receives the accepted native list length. */
	return 0;
}
