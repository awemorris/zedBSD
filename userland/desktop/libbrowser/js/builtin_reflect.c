/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reflect (ECMA-262 §28.1): an ordinary namespace object whose functions
 * call the internal methods of an object directly, so a native Proxy's traps
 * run just as they do for the language's own operators.
 */

#include "js/builtin.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * One function of the namespace: its name, its length and what it does.
 * The table is constant for the life of the program.
 */
struct reflect_entry {
	const char *name;
	unsigned length;
	vm_native native;
};

/*
 * The values of an array-like argument list (CreateListFromArrayLike) in
 * native storage.  The collector marks the first filled values while the
 * tracer is registered; the list belongs to the native call that made it.
 */
struct reflect_list {
	vm_value *values;
	uint32_t filled;
	int traced;
};

static int reflect_apply(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_define_property(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_delete_property(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_get_own_property_descriptor(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_get_prototype_of(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_has(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_is_extensible(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_own_keys(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_prevent_extensions(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_set_prototype_of(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int reflect_target(struct vm_realm *realm, const vm_value *args, unsigned count, const char *message, struct vm_object **object);
static int reflect_target_key(struct vm_realm *realm, const vm_value *args, unsigned count, const char *message, struct vm_object **object, vm_value *key);
static int reflect_list_fill(struct vm_realm *realm, vm_value value, struct reflect_list *list);
static void reflect_list_release(struct vm_realm *realm, struct reflect_list *list);
static void reflect_list_trace(struct vm_heap *heap, void *context);
static void reflect_keys_trace(struct vm_heap *heap, void *context);
static void reflect_descriptor_trace(struct vm_heap *heap, void *context);

/*
 * The functions of the namespace, in the specification's order.
 */
static const struct reflect_entry reflect_functions[] = {
	{ "apply", 3, reflect_apply },
	{ "construct", 2, reflect_construct },
	{ "defineProperty", 3, reflect_define_property },
	{ "deleteProperty", 2, reflect_delete_property },
	{ "get", 2, reflect_get },
	{ "getOwnPropertyDescriptor", 2, reflect_get_own_property_descriptor },
	{ "getPrototypeOf", 1, reflect_get_prototype_of },
	{ "has", 2, reflect_has },
	{ "isExtensible", 1, reflect_is_extensible },
	{ "ownKeys", 1, reflect_own_keys },
	{ "preventExtensions", 1, reflect_prevent_extensions },
	{ "set", 3, reflect_set },
	{ "setPrototypeOf", 2, reflect_set_prototype_of },
	{ NULL, 0, NULL }
};

/*
 * Installs the Reflect namespace on a realm's global object.
 *
 * Its Symbol.toStringTag comes with the other namespaces' tags.
 */
int
js_builtin_install_reflect(
	struct vm_realm *realm)
{
	const struct reflect_entry *entry;
	struct vm_object *reflect;
	int error;

	/* The ordinary object, a global like Math. */
	reflect = vm_object_create(realm->heap, realm->object_prototype);
	if (reflect == NULL)
		return ENOMEM;
	error = js_builtin_value(realm, realm->global, "Reflect", vm_value_cell(reflect), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* Each function as a method of the namespace. */
	for (entry = reflect_functions; entry->name != NULL; entry++) {
		error = js_builtin_method(realm, reflect, entry->name, entry->length, entry->native);
		if (error != 0)
			return error;
	}

	/* Succeeded: the realm has Reflect. */
	return 0;
}

/* Reflect.apply(target, thisArgument, argumentsList). */
static int
reflect_apply(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct reflect_list list;
	vm_value target;
	int callable;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Only a function can be called, refused before the list is read. */
	target = js_argument(args, count, 0);
	callable = vm_value_is_callable(target);
	if (!callable) {
		status = vm_throw_type_error(realm, "Reflect.apply target is not a function");
		return status;
	}

	/* The arguments from the array-like list, then the call. */
	status = reflect_list_fill(realm, js_argument(args, count, 2), &list);
	if (status == 0)
		status = vm_call(realm, target, js_argument(args, count, 1), list.values, list.filled, result);
	reflect_list_release(realm, &list);
	if (status != 0)
		return status;

	/* Succeeded: the function's result. */
	return 0;
}

/* Reflect.construct(target, argumentsList[, newTarget]). */
static int
reflect_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct reflect_list list;
	vm_value target;
	vm_value new_target;
	int constructor;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The target must be a constructor. */
	target = js_argument(args, count, 0);
	constructor = vm_value_is_constructor(target);
	if (!constructor) {
		status = vm_throw_type_error(realm, "Reflect.construct target is not a constructor");
		return status;
	}

	/* An absent newTarget is the target; a present one, even undefined, must be a constructor. */
	new_target = target;
	if (count >= 3U)
		new_target = args[2];
	constructor = vm_value_is_constructor(new_target);
	if (!constructor) {
		status = vm_throw_type_error(realm, "Reflect.construct newTarget is not a constructor");
		return status;
	}

	/* The arguments from the array-like list, then the construction. */
	status = reflect_list_fill(realm, js_argument(args, count, 1), &list);
	if (status == 0)
		status = vm_construct(realm, target, list.values, list.filled, new_target, result);
	reflect_list_release(realm, &list);
	if (status != 0)
		return status;

	/* Succeeded: the new object. */
	return 0;
}

/* Reflect.defineProperty(target, propertyKey, attributes): whether the definition was allowed. */
static int
reflect_define_property(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_descriptor descriptor;
	struct vm_object *object;
	struct vm_cell *key_root;
	vm_value key;
	int is_cell;
	int done;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object and the key. */
	status = reflect_target_key(realm, args, count, "Reflect.defineProperty called on non-object", &object, &key);
	if (status != 0)
		return status;

	/* The converted key survives the descriptor's getters. */
	key_root = NULL;
	is_cell = vm_value_is_cell(key);
	if (is_cell)
		key_root = vm_value_as_cell(key);
	status = vm_heap_add_root(realm->heap, &key_root);
	if (status != 0)
		return status;
	status = vm_to_property_descriptor(realm, js_argument(args, count, 2), &descriptor);
	if (status != 0)
		goto cleanup;

	/* The definition with the descriptor's values kept alive; a refusal is false, not an error. */
	status = vm_heap_add_tracer(realm->heap, reflect_descriptor_trace, &descriptor);
	if (status != 0)
		goto cleanup;
	status = vm_define_own_property(realm, object, key, &descriptor, &done);
	vm_heap_remove_tracer(realm->heap, reflect_descriptor_trace, &descriptor);

cleanup:
	/* The key is no longer needed past the definition. */
	vm_heap_remove_root(realm->heap, &key_root);
	if (status != 0)
		return status;

	/* Succeeded: whether the property was defined. */
	*result = vm_value_boolean(done);
	return 0;
}

/* Reflect.deleteProperty(target, propertyKey): whether the property is gone. */
static int
reflect_delete_property(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *object;
	vm_value key;
	int deleted;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object and the key. */
	status = reflect_target_key(realm, args, count, "Reflect.deleteProperty called on non-object", &object, &key);
	if (status != 0)
		return status;

	/* The object's own [[Delete]]. */
	status = vm_delete_property(realm, object, key, &deleted);
	if (status != 0)
		return status;

	/* Succeeded: whether the deletion was allowed. */
	*result = vm_value_boolean(deleted);
	return 0;
}

/* Reflect.get(target, propertyKey[, receiver]). */
static int
reflect_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *object;
	vm_value receiver;
	vm_value key;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object and the key. */
	status = reflect_target_key(realm, args, count, "Reflect.get called on non-object", &object, &key);
	if (status != 0)
		return status;

	/* An absent receiver is the target itself. */
	receiver = args[0];
	if (count >= 3U)
		receiver = args[2];

	/* The object's own [[Get]] with that receiver. */
	status = vm_get_receiver(realm, vm_value_cell(object), key, receiver, result);
	if (status != 0)
		return status;

	/* Succeeded: the value. */
	return 0;
}

/* Reflect.getOwnPropertyDescriptor(target, propertyKey): the descriptor as an object, or undefined. */
static int
reflect_get_own_property_descriptor(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_descriptor descriptor;
	struct vm_object *object;
	vm_value key;
	int found;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object and the key. */
	status = reflect_target_key(realm, args, count, "Reflect.getOwnPropertyDescriptor called on non-object", &object, &key);
	if (status != 0)
		return status;

	/* The object's own [[GetOwnProperty]]. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_get_own_property(realm, object, key, &descriptor, &found);
	if (status != 0)
		return status;

	/* A missing property is undefined. */
	if (!found)
		return 0;

	/* The descriptor as an ordinary object. */
	status = vm_from_property_descriptor(realm, &descriptor, result);
	if (status != 0)
		return status;

	/* Succeeded: the descriptor. */
	return 0;
}

/* Reflect.getPrototypeOf(target). */
static int
reflect_get_prototype_of(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *object;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object, without conversion. */
	status = reflect_target(realm, args, count, "Reflect.getPrototypeOf called on non-object", &object);
	if (status != 0)
		return status;

	/* The object's own [[GetPrototypeOf]]. */
	status = vm_get_prototype(realm, object, result);
	if (status != 0)
		return status;

	/* Succeeded: the prototype, or null. */
	return 0;
}

/* Reflect.has(target, propertyKey). */
static int
reflect_has(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *object;
	vm_value key;
	int found;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object and the key. */
	status = reflect_target_key(realm, args, count, "Reflect.has called on non-object", &object, &key);
	if (status != 0)
		return status;

	/* The object's own [[HasProperty]], through its prototypes. */
	status = vm_has_property(realm, object, key, &found);
	if (status != 0)
		return status;

	/* Succeeded: whether the property exists. */
	*result = vm_value_boolean(found);
	return 0;
}

/* Reflect.isExtensible(target). */
static int
reflect_is_extensible(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *object;
	int extensible;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object, without conversion. */
	status = reflect_target(realm, args, count, "Reflect.isExtensible called on non-object", &object);
	if (status != 0)
		return status;

	/* The object's own [[IsExtensible]]. */
	status = vm_is_extensible(realm, object, &extensible);
	if (status != 0)
		return status;

	/* Succeeded: whether properties can be added. */
	*result = vm_value_boolean(extensible);
	return 0;
}

/* Reflect.ownKeys(target): every own key, strings and symbols, as an array. */
static int
reflect_own_keys(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct wb_vector keys;
	struct vm_string *name;
	struct vm_object *object;
	vm_value *slot;
	size_t item;
	int is_int32;
	int traced;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object, without conversion. */
	status = reflect_target(realm, args, count, "Reflect.ownKeys called on non-object", &object);
	if (status != 0)
		return status;

	/* The keys stay alive while the indices become strings and the array is made. */
	wb_vector_init(&keys, sizeof(vm_value));
	traced = 0;
	status = vm_heap_add_tracer(realm->heap, reflect_keys_trace, &keys);
	if (status != 0)
		goto cleanup;
	traced = 1;

	/* The object's own [[OwnPropertyKeys]], in its order. */
	status = vm_own_property_keys(realm, object, &keys);
	if (status != 0)
		goto cleanup;

	/* An index key is shown as its numeral, in place. */
	for (item = 0; item < keys.count; item++) {
		slot = wb_vector_at(&keys, item);
		is_int32 = vm_value_is_int32(*slot);
		if (!is_int32)
			continue;
		status = vm_to_string(realm, *slot, &name);
		if (status != 0)
			goto cleanup;
		*slot = vm_value_cell(name);
	}

	/* The array (CreateArrayFromList). */
	status = js_builtin_array(realm, keys.items, (uint32_t)keys.count, result);

cleanup:
	/* The native key list is released after its tracer stops. */
	if (traced)
		vm_heap_remove_tracer(realm->heap, reflect_keys_trace, &keys);
	wb_vector_release(&keys);
	if (status != 0)
		return status;

	/* Succeeded: the keys. */
	return 0;
}

/* Reflect.preventExtensions(target): whether the object is now not extensible. */
static int
reflect_prevent_extensions(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *object;
	int done;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object, without conversion. */
	status = reflect_target(realm, args, count, "Reflect.preventExtensions called on non-object", &object);
	if (status != 0)
		return status;

	/* The object's own [[PreventExtensions]]. */
	status = vm_prevent_extensions(realm, object, &done);
	if (status != 0)
		return status;

	/* Succeeded: whether the change was allowed. */
	*result = vm_value_boolean(done);
	return 0;
}

/* Reflect.set(target, propertyKey, V[, receiver]): whether the assignment was allowed. */
static int
reflect_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *object;
	vm_value receiver;
	vm_value key;
	int done;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object and the key. */
	status = reflect_target_key(realm, args, count, "Reflect.set called on non-object", &object, &key);
	if (status != 0)
		return status;

	/* An absent receiver is the target itself. */
	receiver = args[0];
	if (count >= 4U)
		receiver = args[3];

	/* The object's own [[Set]] with that receiver. */
	status = vm_set_receiver(realm, object, key, js_argument(args, count, 2), receiver, &done);
	if (status != 0)
		return status;

	/* Succeeded: whether the value was stored. */
	*result = vm_value_boolean(done);
	return 0;
}

/* Reflect.setPrototypeOf(target, proto): whether the change was allowed. */
static int
reflect_set_prototype_of(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *object;
	struct vm_object *prototype;
	vm_value proto;
	int is_object;
	int done;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The object, without conversion. */
	status = reflect_target(realm, args, count, "Reflect.setPrototypeOf called on non-object", &object);
	if (status != 0)
		return status;

	/* The prototype must be an object or null. */
	proto = js_argument(args, count, 1);
	is_object = vm_value_is_object(proto);
	if (!is_object && proto != VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Object prototype may only be an Object or null");
		return status;
	}

	/* The object's own [[SetPrototypeOf]]. */
	prototype = NULL;
	if (is_object)
		prototype = (struct vm_object *)vm_value_as_cell(proto);
	status = vm_set_prototype(realm, object, prototype, &done);
	if (status != 0)
		return status;

	/* Succeeded: whether the prototype was changed. */
	*result = vm_value_boolean(done);
	return 0;
}

/* Requires the first argument to be an object, throwing a TypeError with a message otherwise. */
static int
reflect_target(
	struct vm_realm *realm,
	const vm_value *args,
	unsigned count,
	const char *message,
	struct vm_object **object)
{
	vm_value target;
	int is_object;
	int status;

	/* A primitive is refused. */
	*object = NULL;
	target = js_argument(args, count, 0);
	is_object = vm_value_is_object(target);
	if (!is_object) {
		status = vm_throw_type_error(realm, message);
		return status;
	}

	/* Succeeded: the object. */
	*object = (struct vm_object *)vm_value_as_cell(target);
	return 0;
}

/* Requires the first argument to be an object, then converts the second to a property key. */
static int
reflect_target_key(
	struct vm_realm *realm,
	const vm_value *args,
	unsigned count,
	const char *message,
	struct vm_object **object,
	vm_value *key)
{
	int status;

	/* The object comes first, so a primitive target is refused before the key is converted. */
	status = reflect_target(realm, args, count, message, object);
	if (status != 0)
		return status;

	/* ToPropertyKey, which may call the key's toString or valueOf. */
	status = vm_to_key(realm, js_argument(args, count, 1), key);
	if (status != 0)
		return status;

	/* Succeeded: the object and the key. */
	return 0;
}

/*
 * Reads an array-like object's elements into a native list
 * (CreateListFromArrayLike).
 *
 * The list is always released with reflect_list_release, also on failure.
 */
static int
reflect_list_fill(
	struct vm_realm *realm,
	vm_value value,
	struct reflect_list *list)
{
	size_t allocation_count;
	uint32_t length;
	uint32_t index;
	int is_object;
	int status;

	/* An empty list until it is read. */
	list->values = NULL;
	list->filled = 0;
	list->traced = 0;

	/* Only an object has elements. */
	is_object = vm_value_is_object(value);
	if (!is_object) {
		status = vm_throw_type_error(realm, "CreateListFromArrayLike called on non-object");
		return status;
	}

	/* The list's length (LengthOfArrayLike). */
	status = js_builtin_length(realm, value, &length);
	if (status != 0)
		return status;
	allocation_count = (size_t)length + 1U;
	list->values = calloc(allocation_count, sizeof(vm_value));
	if (list->values == NULL)
		return ENOMEM;

	/* A returned getter value may have no other owner before the next getter. */
	status = vm_heap_add_tracer(realm->heap, reflect_list_trace, list);
	if (status != 0)
		return status;
	list->traced = 1;

	/* Each element in index order. */
	for (index = 0; index < length; index++) {
		status = vm_get(realm, value, vm_value_int32((int32_t)index), &list->values[index]);
		if (status != 0)
			return status;
		list->filled = index + 1U;
	}

	/* Succeeded: the list. */
	return 0;
}

/* Stops tracing a native argument list and frees it. */
static void
reflect_list_release(
	struct vm_realm *realm,
	struct reflect_list *list)
{
	/* The tracer stops before the storage it reads is freed. */
	if (list->traced)
		vm_heap_remove_tracer(realm->heap, reflect_list_trace, list);
	list->traced = 0;
	free(list->values);
	list->values = NULL;
	list->filled = 0;
}

/* Marks the values a native argument list has read so far. */
static void
reflect_list_trace(
	struct vm_heap *heap,
	void *context)
{
	struct reflect_list *list;
	uint32_t index;

	/* Unfilled slots hold no value for the collector. */
	list = context;
	for (index = 0; index < list->filled; index++)
		vm_heap_mark_value(heap, list->values[index]);
}

/* Marks the keys of a native key vector. */
static void
reflect_keys_trace(
	struct vm_heap *heap,
	void *context)
{
	struct wb_vector *keys;
	size_t index;

	/* Each key: an index, a string or a symbol. */
	keys = context;
	for (index = 0; index < keys->count; index++)
		vm_heap_mark_value(heap, *(vm_value *)wb_vector_at(keys, index));
}

/* Marks the present values of a temporary property descriptor. */
static void
reflect_descriptor_trace(
	struct vm_heap *heap,
	void *context)
{
	struct vm_descriptor *descriptor;

	/* Only the fields marked present hold language values. */
	descriptor = context;
	if ((descriptor->has & VM_HAS_VALUE) != 0U)
		vm_heap_mark_value(heap, descriptor->value);
	if ((descriptor->has & VM_HAS_GET) != 0U)
		vm_heap_mark_value(heap, descriptor->getter);
	if ((descriptor->has & VM_HAS_SET) != 0U)
		vm_heap_mark_value(heap, descriptor->setter);
}
