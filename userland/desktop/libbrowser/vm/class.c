/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Classes (ws074-p080): making a class's constructor and prototype from
 * its heritage, defining its methods and accessors, and reading a
 * property through super.
 *
 * A method's home object (the prototype, or the constructor for a static
 * one) is kept in its function's data: super.x reads x from the home
 * object's prototype with the method's this as the receiver.
 */

#include "vm/bytecode.h"
#include "vm/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int class_private_find(struct vm_realm *realm, vm_value object, vm_value key, struct vm_property *property, int *found);

/*
 * Initializes a class constructor and its instance prototype chain.
 *
 * The prototype inherits the parent's prototype, Object.prototype without
 * heritage, or null for extends null. Reciprocal constructor properties
 * link both objects; a derived constructor inherits the parent constructor.
 * The empty parent value denotes absent heritage.
 */
int
vm_class_setup(
	struct vm_realm *realm,
	vm_value constructor,
	vm_value parent,
	vm_value *prototype)
{
	struct vm_function *function;
	struct vm_object *made;
	struct vm_object *proto_parent;
	struct vm_object *constructor_parent;
	vm_value parent_prototype;
	vm_value key;
	int is_constructor;
	int is_object;
	int status;

	/* Without a heritage: Object.prototype and Function.prototype. */
	proto_parent = realm->object_prototype;
	constructor_parent = realm->function_prototype;

	/* extends null: a prototype with no prototype, and an ordinary constructor. */
	if (parent == VM_VALUE_NULL)
		proto_parent = NULL;

	/* extends a constructor: its prototype property (an object or null) and itself. */
	if (parent != VM_VALUE_EMPTY && parent != VM_VALUE_NULL) {
		is_constructor = vm_value_is_constructor(parent);
		if (!is_constructor) {
			status = vm_throw_type_error(realm, "Class extends value is not a constructor or null");
			if (status != 0)
				return status;

			/* Succeeded: the native class refusal follows its existing throw contract. */
			return 0;
		}

		/* Its prototype property, an object or null. */
		key = vm_key_from_ascii(realm->heap, "prototype");
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;

		/* Reads the actual parent's prototype before classifying its returned value. */
		status = vm_get(realm, parent, key, &parent_prototype);
		if (status != 0)
			return status;

		/* Only an object or null can enter the instance prototype chain. */
		is_object = vm_value_is_object(parent_prototype);
		if (!is_object && parent_prototype != VM_VALUE_NULL) {
			status = vm_throw_type_error(realm, "Class extends value does not have valid prototype property");
			if (status != 0)
				return status;

			/* Succeeded: the native class refusal follows its existing throw contract. */
			return 0;
		}

		/* The chains. */
		proto_parent = NULL;
		if (is_object)
			proto_parent = (struct vm_object *)vm_value_as_cell(parent_prototype);
		constructor_parent = (struct vm_object *)vm_value_as_cell(parent);
	}

	/* The prototype object, the constructor's home object too (for super in it). */
	made = vm_object_create(realm->heap, proto_parent);
	if (made == NULL)
		return ENOMEM;

	/* Retains the new home object through the constructor's native data edge. */
	function = (struct vm_function *)vm_value_as_cell(constructor);
	function->object.prototype = constructor_parent;
	function->data = vm_value_cell(made);

	/* constructor.prototype, fixed. */
	key = vm_key_from_ascii(realm->heap, "prototype");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* Defines the constructor's fixed prototype after its native key is available. */
	status = vm_object_define(realm->heap, &function->object, key, vm_value_cell(made), 0);
	if (status != 0)
		return status;

	/* prototype.constructor, not enumerable. */
	key = vm_key_from_ascii(realm->heap, "constructor");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* Gives the completed prototype its reciprocal non-enumerable constructor. */
	status = vm_object_define(realm->heap, made, key, constructor, VM_PROPERTY_WRITABLE | VM_PROPERTY_CONFIGURABLE);
	if (status != 0)
		return status;

	/* Publishes the completed instance prototype after both reciprocal properties exist. */
	*prototype = vm_value_cell(made);

	/* Succeeded: the class's native constructor and prototype chains are installed. */
	return 0;
}

/*
 * Defines a class method or accessor on its native home object.
 *
 * Kinds zero, one and two select a method, getter and setter respectively.
 * The non-enumerable function retains that object as its native home.
 */
int
vm_define_method(
	struct vm_realm *realm,
	vm_value home,
	vm_value key,
	vm_value function,
	uint32_t kind)
{
	struct vm_function *method;
	struct vm_object *object;
	struct vm_property property;
	struct vm_accessor *accessor;
	vm_value getter;
	vm_value setter;
	int found;
	int status;

	/* The function remembers where it lives (a bytecode method's data is its home object). */
	method = (struct vm_function *)vm_value_as_cell(function);
	if (method->code != NULL)
		method->data = home;

	/* A method: a writable, configurable data property. */
	object = (struct vm_object *)vm_value_as_cell(home);
	if (kind == 0U) {
		status = vm_object_define(realm->heap, object, key, function, VM_PROPERTY_WRITABLE | VM_PROPERTY_CONFIGURABLE);
		if (status != 0)
			return status;

		/* Succeeded: the native method has its class home and ordinary descriptor. */
		return 0;
	}

	/* An accessor's half joins the other half already there. */
	getter = VM_VALUE_UNDEFINED;
	setter = VM_VALUE_UNDEFINED;
	found = vm_object_get_own(object, key, &property);
	if (found < 0)
		return -found;

	/* Present descriptors retain their ordinary accessor and attribute rules. */
	if (found && (property.attributes & VM_PROPERTY_ACCESSOR) != 0U) {
		accessor = (struct vm_accessor *)vm_value_as_cell(*property.value);
		getter = accessor->getter;
		setter = accessor->setter;
	}

	/* The new half replaces its own. */
	if (kind == 1U) {
		getter = function;
	} else {
		setter = function;
	}

	/* The accessor property, configurable and not enumerable. */
	accessor = vm_accessor_create(realm->heap, getter, setter);
	if (accessor == NULL)
		return ENOMEM;

	/* Publishes the completed accessor pair under the class's ordinary method key. */
	status = vm_object_define(realm->heap, object, key, vm_value_cell(accessor), VM_PROPERTY_ACCESSOR | VM_PROPERTY_CONFIGURABLE);
	if (status != 0)
		return status;

	/* Succeeded: the accessor is defined. */
	return 0;
}

/*
 * Names a function after the key it is defined under (SetFunctionName):
 * a string key is the name, an index its numeral, a symbol its
 * description in brackets (or "" when it has none), a private name its
 * description as it is; prefix 1 puts "get " before it and 2 "set ".
 *
 * Only a bytecode function whose own name is still the one its code gave
 * is renamed, so a class's static name member stays.
 */
int
vm_set_function_name(
	struct vm_realm *realm,
	vm_value function,
	vm_value key,
	uint32_t prefix)
{
	const struct vm_cell_type *type;
	struct vm_property property;
	struct vm_symbol *symbol;
	struct vm_code *code;
	struct vm_string *name;
	struct vm_string *head;
	struct vm_cell *roots[2];
	struct vm_object *object;
	vm_value name_key;
	vm_value original;
	unsigned registered;
	int is_object;
	int is_cell;
	int is_int32;
	int found;
	int status;

	/* Only a bytecode function is named. */
	is_object = vm_value_is_object(function);
	if (!is_object)
		return 0;
	object = (struct vm_object *)vm_value_as_cell(function);
	if (object->kind != VM_KIND_FUNCTION)
		return 0;
	code = ((struct vm_function *)object)->code;
	if (code == NULL)
		return 0;

	/* Its own name must still be the one its code gave (a class's static name member stays). */
	name_key = vm_key_from_ascii(realm->heap, "name");
	if (name_key == VM_VALUE_EMPTY)
		return ENOMEM;
	found = vm_object_get_own(object, name_key, &property);
	if (found < 0)
		return -found;
	if (!found || (property.attributes & VM_PROPERTY_ACCESSOR) != 0U)
		return 0;
	original = VM_VALUE_EMPTY;
	if (code->name != NULL)
		original = vm_value_cell(code->name);
	if (*property.value != original)
		return 0;

	/* The key's kind: an index, a string, a symbol or a private name. */
	is_int32 = vm_value_is_int32(key);
	is_cell = vm_value_is_cell(key);
	type = NULL;
	if (is_cell)
		type = vm_value_as_cell(key)->type;
	symbol = NULL;
	if (type == &vm_symbol_type)
		symbol = (struct vm_symbol *)vm_value_as_cell(key);
	if (!is_int32 && type != &vm_string_type && symbol == NULL)
		return 0;

	/* The parts being joined stay alive while the next one is allocated. */
	roots[0] = NULL;
	roots[1] = NULL;
	registered = 0;
	while (registered < 2U) {
		status = vm_heap_add_root(realm->heap, &roots[registered]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* The base name: an index's numeral, a string, a private name's description, or a symbol's in brackets ("" without one). */
	name = NULL;
	status = 0;
	if (is_int32) {
		status = vm_to_string(realm, key, &name);
	} else if (symbol == NULL) {
		name = (struct vm_string *)vm_value_as_cell(key);
	} else if (symbol->description == VM_VALUE_UNDEFINED) {
		name = vm_string_from_utf8(realm->heap, "", 0);
	} else if (symbol->private_name) {
		name = (struct vm_string *)vm_value_as_cell(symbol->description);
	} else {
		head = vm_string_from_utf8(realm->heap, "[", 1);
		if (head != NULL) {
			roots[0] = &head->cell;
			head = vm_string_concat(realm->heap, head, (struct vm_string *)vm_value_as_cell(symbol->description));
		}
		if (head != NULL) {
			roots[0] = &head->cell;
			name = vm_string_from_utf8(realm->heap, "]", 1);
		}
		if (name != NULL)
			name = vm_string_concat(realm->heap, head, name);
	}
	if (status != 0)
		goto cleanup;
	if (name == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* An accessor's half: "get " or "set " before it. */
	if (prefix != 0U) {
		head = name;
		roots[0] = &head->cell;
		name = vm_string_from_utf8(realm->heap, prefix == 1U ? "get " : "set ", 4);
		if (name != NULL)
			name = vm_string_concat(realm->heap, name, head);
		if (name == NULL) {
			status = ENOMEM;
			goto cleanup;
		}
	}

	/* The name, configurable and neither writable nor enumerable. */
	roots[1] = &name->cell;
	status = vm_object_define(realm->heap, object, name_key, vm_value_cell(name), VM_PROPERTY_CONFIGURABLE);

cleanup:
	/* The parts are owned by the function's name, or no longer needed. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}
	if (status != 0)
		return status;

	/* Succeeded: the function has the key's name. */
	return 0;
}

/*
 * Reads a property through a class method's native home prototype.
 *
 * An inherited getter receives the current method's actual this value.
 */
int
vm_get_super(
	struct vm_realm *realm,
	vm_value home,
	vm_value key,
	vm_value this_value,
	vm_value *result)
{
	struct vm_object *object;
	struct vm_property property;
	struct vm_accessor *accessor;
	int is_object;
	int found;
	int status;

	/* A home object is needed (a method's), and its prototype may be null. */
	*result = VM_VALUE_UNDEFINED;
	is_object = vm_value_is_object(home);
	if (!is_object) {
		status = vm_throw_error(realm, VM_ERROR_SYNTAX, "'super' keyword unexpected here");
		if (status != 0)
			return status;

		/* Succeeded: the native class refusal follows its existing throw contract. */
		return 0;
	}

	/* The home object's prototype, where super looks. */
	object = ((struct vm_object *)vm_value_as_cell(home))->prototype;
	if (object == NULL) {
		status = vm_throw_type_error(realm, "Cannot read properties of null");
		if (status != 0)
			return status;

		/* Succeeded: the native class refusal follows its existing throw contract. */
		return 0;
	}

	/* The property on the prototype's chain. */
	found = vm_object_find(object, key, &property);
	if (found < 0)
		return -found;

	/* A missing descriptor follows the absence path after errors have been excluded. */
	if (!found)
		return 0;

	/* A data property's value. */
	if ((property.attributes & VM_PROPERTY_ACCESSOR) == 0U) {
		*result = *property.value;
		return 0;
	}

	/* A getter, called with the method's this (no getter reads undefined). */
	accessor = (struct vm_accessor *)vm_value_as_cell(*property.value);
	if (accessor->getter == VM_VALUE_UNDEFINED)
		return 0;

	/* Calls the inherited getter with the current method's actual this receiver. */
	status = vm_call(realm, accessor->getter, this_value, NULL, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the getter's value. */
	return 0;
}

/*
 * Throws a TypeError naming the class constructor invoked without new.
 */
int
vm_throw_class_call(
	struct vm_realm *realm,
	struct vm_function *function)
{
	struct wb_buffer name;
	char text[200];
	int printed;
	int status;

	/* The class's name as UTF-8 (a constructor's code is named after its class). */
	wb_buffer_init(&name);
	if (function->code->name != NULL) {
		status = vm_string_to_utf8(function->code->name, &name);
		if (status != 0) {
			wb_buffer_release(&name);
			return status;
		}
	}

	/* The message, as Chromium words it. */
	printed = snprintf(text, sizeof(text), "Class constructor %.120s cannot be invoked without 'new'", wb_buffer_string(&name));
	if (printed < 0) {
		wb_buffer_release(&name);
		return EIO;
	}

	/* Releases the converted name before constructing the already formatted native exception. */
	wb_buffer_release(&name);
	status = vm_throw_type_error(realm, text);
	if (status != 0)
		return status;

	/* Succeeded: the class-call refusal follows the existing native throw contract. */
	return 0;
}

/*
 * Reads an object's own private field or accessor.
 *
 * A private getter receives the actual object; an absent own brand throws
 * a TypeError because that object's class did not declare the member.
 */
int
vm_private_get(
	struct vm_realm *realm,
	vm_value object,
	vm_value key,
	vm_value *result)
{
	struct vm_property property;
	struct vm_accessor *accessor;
	int found;
	int status;

	/* The object's own member. */
	*result = VM_VALUE_UNDEFINED;
	status = class_private_find(realm, object, key, &property, &found);
	if (status != 0)
		return status;

	/* An absent private brand cannot supply a field or accessor result. */
	if (!found) {
		status = vm_throw_type_error(realm, "Cannot read private member from an object whose class did not declare it");
		if (status != 0)
			return status;

		/* Succeeded: the native class refusal follows its existing throw contract. */
		return 0;
	}

	/* A field's or a method's value. */
	if ((property.attributes & VM_PROPERTY_ACCESSOR) == 0U) {
		*result = *property.value;
		return 0;
	}

	/* An accessor without a getter cannot be read. */
	accessor = (struct vm_accessor *)vm_value_as_cell(*property.value);
	if (accessor->getter == VM_VALUE_UNDEFINED) {
		status = vm_throw_type_error(realm, "'#' accessor was defined without a getter");
		if (status != 0)
			return status;

		/* Succeeded: the native class refusal follows its existing throw contract. */
		return 0;
	}

	/* The getter's value. */
	status = vm_call(realm, accessor->getter, object, NULL, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the member's value. */
	return 0;
}

/*
 * Writes an object's own private field or invokes its private setter.
 *
 * A method, missing setter or absent own brand remains a TypeError.
 */
int
vm_private_set(
	struct vm_realm *realm,
	vm_value object,
	vm_value key,
	vm_value value)
{
	struct vm_property property;
	struct vm_accessor *accessor;
	vm_value ignored;
	int found;
	int status;

	/* The object's own member. */
	status = class_private_find(realm, object, key, &property, &found);
	if (status != 0)
		return status;

	/* An absent private brand cannot receive a field write or setter call. */
	if (!found) {
		status = vm_throw_type_error(realm, "Cannot write private member to an object whose class did not declare it");
		if (status != 0)
			return status;

		/* Succeeded: the native class refusal follows its existing throw contract. */
		return 0;
	}

	/* A field takes the value; a method cannot be assigned. */
	if ((property.attributes & VM_PROPERTY_ACCESSOR) == 0U) {
		if ((property.attributes & VM_PROPERTY_WRITABLE) == 0U) {
			status = vm_throw_type_error(realm, "Private method is not writable");
			if (status != 0)
				return status;

			/* Succeeded: the native class refusal follows its existing throw contract. */
			return 0;
		}

		/* A field takes the value. */
		*property.value = value;

		/* Succeeded: the ordinary writable private field holds the new value. */
		return 0;
	}

	/* An accessor without a setter cannot be written. */
	accessor = (struct vm_accessor *)vm_value_as_cell(*property.value);
	if (accessor->setter == VM_VALUE_UNDEFINED) {
		status = vm_throw_type_error(realm, "'#' accessor was defined without a setter");
		if (status != 0)
			return status;

		/* Succeeded: the native class refusal follows its existing throw contract. */
		return 0;
	}

	/* The setter. */
	status = vm_call(realm, accessor->setter, object, &value, 1, &ignored);
	if (status != 0)
		return status;

	/* Succeeded: the member is written. */
	return 0;
}

/*
 * Adds a private field from its class initializer to an unbranded object.
 */
int
vm_private_define(
	struct vm_realm *realm,
	vm_value object,
	vm_value key,
	vm_value value)
{
	struct vm_property property;
	int found;
	int status;

	/* An object that has it already. */
	status = class_private_find(realm, object, key, &property, &found);
	if (status != 0)
		return status;

	/* Repeated private field initialization on the same object remains illegal. */
	if (found) {
		status = vm_throw_type_error(realm, "Cannot initialize private field twice on the same object");
		if (status != 0)
			return status;

		/* Succeeded: the native class refusal follows its existing throw contract. */
		return 0;
	}

	/* A writable member, never enumerable. */
	status = vm_object_define(realm->heap, (struct vm_object *)vm_value_as_cell(object), key, value, VM_PROPERTY_WRITABLE);
	if (status != 0)
		return status;

	/* Succeeded: the field is added. */
	return 0;
}

/*
 * Copies a class's private method or accessor brand onto an instance.
 *
 * The source prototype's attributes are retained; an existing own brand
 * rejects repeated construction with a TypeError.
 */
int
vm_private_copy(
	struct vm_realm *realm,
	vm_value target,
	vm_value source,
	vm_value key)
{
	struct vm_property property;
	struct vm_object *holder;
	int found;
	int status;

	/* An instance that has it already (constructed twice). */
	status = class_private_find(realm, target, key, &property, &found);
	if (status != 0)
		return status;

	/* A constructed instance cannot acquire the same private method brand twice. */
	if (found) {
		status = vm_throw_type_error(realm, "Cannot initialize private methods twice on the same object");
		if (status != 0)
			return status;

		/* Succeeded: the native class refusal follows its existing throw contract. */
		return 0;
	}

	/* The prototype's member. */
	holder = (struct vm_object *)vm_value_as_cell(source);
	found = vm_object_get_own_ordinary(holder, key, &property);
	if (!found)
		return EINVAL;

	/* The same value (a method, or the accessor pair) and attributes on the instance. */
	status = vm_object_define(realm->heap, (struct vm_object *)vm_value_as_cell(target), key, *property.value, property.attributes);
	if (status != 0)
		return status;

	/* Succeeded: the instance has the member. */
	return 0;
}

/*
 * Tests an actual object's own private member brand for the in operator.
 */
int
vm_private_in(
	struct vm_realm *realm,
	vm_value key,
	vm_value object,
	vm_value *result)
{
	struct vm_property property;
	int is_object;
	int found;
	int status;

	/* Only an object can be asked. */
	is_object = vm_value_is_object(object);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Cannot use 'in' operator to search for a private field in a value that is not an object");
		if (status != 0)
			return status;

		/* Succeeded: the native class refusal follows its existing throw contract. */
		return 0;
	}

	/* Its own member. */
	found = vm_object_get_own_ordinary((struct vm_object *)vm_value_as_cell(object), key, &property);

	/* Publishes ordinary brand presence without searching any prototype. */
	*result = VM_VALUE_FALSE;
	if (found)
		*result = VM_VALUE_TRUE;

	/* Succeeded: whether it has it. */
	return 0;
}

/* Finds an object's own private member; a value that is not an object has none. */
static int
class_private_find(
	struct vm_realm *realm,
	vm_value object,
	vm_value key,
	struct vm_property *property,
	int *found)
{
	int is_object;

	UNUSED_PARAMETER(realm);

	/* A primitive has no private members. */
	*found = 0;
	is_object = vm_value_is_object(object);
	if (!is_object)
		return 0;

	/* Only the object's own (never its prototypes'). */
	*found = vm_object_get_own_ordinary((struct vm_object *)vm_value_as_cell(object), key, property);

	/* Succeeded: whether it has it. */
	return 0;
}
