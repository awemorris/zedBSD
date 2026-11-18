/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Properties of any value as the language reaches them: getting, setting
 * (with strict mode's errors), deleting, the in and instanceof operators,
 * an object literal's definitions, the global variables of scripts, and
 * the enumeration of for-in.
 *
 * The primitives other than strings get their prototypes (Number.prototype
 * and the rest) with the built-ins; until then their
 * properties read undefined.
 */

#include "vm/internal.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Native fallback chains and script getters share a finite property-operation budget per realm. */
#define ACCESS_RECURSION_MAX 256U

/* The four distinct key lists preserve evaluation order before any Proxy own-key invariant is checked. */
enum access_own_key_list {
	ACCESS_OWN_TRAP_KEYS,
	ACCESS_OWN_TARGET_KEYS,
	ACCESS_OWN_FIXED_KEYS,
	ACCESS_OWN_CONFIGURABLE_KEYS,
	ACCESS_OWN_LIST_COUNT
};

/*
 * One for-in iterator owns the current prototype level, its unprocessed own
 * keys and the names already seen.  Native vectors remain traced between
 * interpreter steps and are released with this private cell.
 */
struct access_for_in {
	struct vm_cell cell;
	vm_value object;
	struct wb_vector keys;
	struct wb_vector visited;
	size_t index;
	int loaded;
};

/* One native Get frame keeps its realm and input values alive without conservative stack scanning. */
struct access_read_roots {
	struct vm_realm *realm;
	vm_value base;
	vm_value key;
	vm_value receiver;
};

/* One native Set frame owns the original write value as well as all receiver-aware read inputs. */
struct access_write_roots {
	struct access_read_roots read;
	vm_value value;
};

/* One active descriptor operation owns its realm, source, flagged payloads and unpublished output object. */
struct access_descriptor_roots {
	struct vm_realm *realm;
	vm_value source;
	const struct vm_descriptor *descriptor;
	vm_value made;
};

static void access_for_in_trace(struct vm_heap *heap, struct vm_cell *cell);
static void access_for_in_finalize(struct vm_heap *heap, struct vm_cell *cell);
static int access_string_property(struct vm_realm *realm, struct vm_string *string, vm_value key, vm_value *result, int *found);
static int access_refuse(struct vm_realm *realm, int strict, const char *message);
static struct vm_object *access_primitive_prototype(struct vm_realm *realm, vm_value value);
static int access_base_error(struct vm_realm *realm, vm_value base, vm_value key, const char *verb, const char *gerund);
static int access_set_primitive(struct vm_realm *realm, vm_value base, vm_value key, vm_value value, int strict);
static int access_wrap_string(struct vm_realm *realm, struct vm_object *wrapper, struct vm_string *string);
static int access_define_length(struct vm_realm *realm, struct vm_object *array, const struct vm_descriptor *current, const struct vm_descriptor *descriptor, int *done);
static void access_get_length_attributes(struct vm_object *array, vm_value length_key, uint32_t *attributes);

/* The cell type of for-in states: they hold their object and keys. */
static const struct vm_cell_type access_for_in_type = {
	"for-in", access_for_in_trace, access_for_in_finalize
};

static int access_key_listed(const struct wb_vector *keys, size_t count, vm_value key);
static int access_is_symbol(vm_value value);
static int access_instanceof(struct vm_realm *realm, vm_value value, vm_value constructor, vm_value *result);
static void access_keys_trace(struct vm_heap *heap, void *context);
static int access_property_key_list(struct vm_realm *realm, vm_value source, struct wb_vector *keys);
static int access_proxy_own_keys(struct vm_realm *realm, struct vm_proxy *proxy, struct wb_vector *keys);
static int access_set_prototype(struct vm_object *object, struct vm_object *prototype);
static int access_proxy_get_prototype(struct vm_realm *realm, struct vm_proxy *proxy, vm_value *prototype);
static int access_proxy_set_prototype(struct vm_realm *realm, struct vm_proxy *proxy, vm_value prototype, int *done);
static int access_proxy_delete(struct vm_realm *realm, struct vm_proxy *proxy, vm_value key, int *deleted);
static int access_extensibility(struct vm_realm *realm, struct vm_object *object, int prevent, int *result);
static int access_proxy_extensibility(struct vm_realm *realm, struct vm_proxy *proxy, int prevent, int *result);
static int access_set_typed(struct vm_realm *realm, struct vm_object *object, vm_value key, vm_value value, vm_value receiver, int *done);
static int access_define_typed(struct vm_realm *realm, struct vm_object *object, vm_value key, const struct vm_descriptor *descriptor, int *handled, int *done);
static void access_write_trace(struct vm_heap *heap, void *context);
static int access_set_receiver(struct vm_realm *realm, struct vm_object *object, vm_value key, vm_value value, vm_value receiver, int *done);
static int access_proxy_set(struct vm_realm *realm, struct vm_proxy *proxy, vm_value key, vm_value value, vm_value receiver, int *done);
static int access_proxy_define(struct vm_realm *realm, struct vm_proxy *proxy, vm_value key, const struct vm_descriptor *descriptor, int *done);
static int access_proxy_get_own(struct vm_realm *realm, struct vm_proxy *proxy, vm_value key, struct vm_descriptor *descriptor, int *found);
static int access_descriptor_compatible(int extensible, int found, const struct vm_descriptor *current, const struct vm_descriptor *descriptor);
static int access_descriptor_allowed(const struct vm_object *object, int found, const struct vm_descriptor *current, const struct vm_descriptor *descriptor);
static void access_descriptor_trace(struct vm_heap *heap, void *context);
static int access_descriptor_field(struct vm_realm *realm, vm_value object, const char *name, uint32_t flag, struct vm_descriptor *descriptor, vm_value *value);
static int access_descriptor_put(struct vm_realm *realm, struct vm_object *object, const char *name, vm_value value);
static void access_read_trace(struct vm_heap *heap, void *context);
static int access_get_receiver(struct vm_realm *realm, vm_value base, vm_value key, vm_value receiver, vm_value *result);
static int access_has_property(struct vm_realm *realm, struct vm_object *object, vm_value key, int *found);
static int access_proxy_has(struct vm_realm *realm, struct vm_proxy *proxy, vm_value key, int *found);
static int access_proxy_get(struct vm_realm *realm, struct vm_proxy *proxy, vm_value key, vm_value receiver, vm_value *result);

/*
 * Appends actual own property keys through realm-aware Proxy handling with complete native ownership.
 */
int
vm_own_property_keys(
	struct vm_realm *realm,
	struct vm_object *object,
	struct wb_vector *keys)
{
	struct access_read_roots roots;
	size_t beginning;
	int status;

	/* Caller prefixes and managed input ownership both survive every trap, native callback and error allocation. */
	beginning = keys->count;
	roots.realm = realm;
	roots.base = vm_value_cell(object);
	roots.key = VM_VALUE_UNDEFINED;
	roots.receiver = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;
	status = vm_heap_add_tracer(realm->heap, access_keys_trace, keys);
	if (status != 0) {
		vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
		return status;
	}

	/* Bound fallback recursion before any target operation while both input and prefix are rooted. */
	if (realm->property_depth >= ACCESS_RECURSION_MAX) {
		status = vm_throw_range_error(realm, "Maximum property recursion exceeded");
		vm_heap_remove_tracer(realm->heap, access_keys_trace, keys);
		vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
		return status;
	}

	/* Ordinary objects preserve their existing native policy and key order. */
	realm->property_depth++;
	if (object->kind == VM_KIND_PROXY)
		status = access_proxy_own_keys(realm, (struct vm_proxy *)object, keys);
	else
		status = vm_object_own_keys(realm->heap, object, keys);

	/* Retire the native depth and discard only this operation's partial append on failure. */
	realm->property_depth--;
	if (status != 0)
		keys->count = beginning;
	vm_heap_remove_tracer(realm->heap, access_keys_trace, keys);
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: only the completed operation's keys were appended to the preserved prefix. */
	return 0;
}

/*
 * Gets an object's actual prototype through ordinary storage or native Proxy internal methods.
 */
int
vm_get_prototype(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value *prototype)
{
	struct access_read_roots roots;
	int status;

	/* The managed owner and original input must survive all trap and depth-error allocations. */
	*prototype = VM_VALUE_NULL;
	roots.realm = realm;
	roots.base = vm_value_cell(object);
	roots.key = VM_VALUE_UNDEFINED;
	roots.receiver = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;
	if (realm->property_depth >= ACCESS_RECURSION_MAX) {
		status = vm_throw_range_error(realm, "Maximum property recursion exceeded");
		vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
		return status;
	}

	/* Ordinary prototype storage never invokes a native property callback or user script. */
	realm->property_depth++;
	if (object->kind == VM_KIND_PROXY) {
		status = access_proxy_get_prototype(realm, (struct vm_proxy *)object, prototype);
	} else {
		status = 0;
		if (object->prototype != NULL)
			*prototype = vm_value_cell(object->prototype);
	}

	/* Retire both native recursion depth and collector ownership on every completion path. */
	realm->property_depth--;
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: the returned object or null belongs to the completed prototype operation. */
	return 0;
}

/*
 * Sets an object's actual prototype while preserving original input ownership and Boolean refusal.
 */
int
vm_set_prototype(
	struct vm_realm *realm,
	struct vm_object *object,
	struct vm_object *prototype,
	int *done)
{
	struct access_read_roots roots;
	int status;

	/* A requested prototype may otherwise be owned only by the embedding call, even during method lookup. */
	*done = 0;
	roots.realm = realm;
	roots.base = vm_value_cell(object);
	roots.key = VM_VALUE_UNDEFINED;
	roots.receiver = VM_VALUE_NULL;
	if (prototype != NULL)
		roots.receiver = vm_value_cell(prototype);
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;
	if (realm->property_depth >= ACCESS_RECURSION_MAX) {
		status = vm_throw_range_error(realm, "Maximum property recursion exceeded");
		vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
		return status;
	}

	/* Proxy handling is realm-aware, while ordinary prototype policy remains a pure storage operation. */
	realm->property_depth++;
	if (object->kind == VM_KIND_PROXY) {
		status = access_proxy_set_prototype(realm, (struct vm_proxy *)object, roots.receiver, done);
	} else {
		status = 0;
		*done = access_set_prototype(object, prototype);
	}

	/* An abrupt trap cannot leak requested-prototype ownership or native recursion depth. */
	realm->property_depth--;
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: done reports actual acceptance without applying a caller's throwing policy. */
	return 0;
}

/*
 * Reports actual extensibility through ordinary policy or native Proxy internal methods.
 */
int
vm_is_extensible(
	struct vm_realm *realm,
	struct vm_object *object,
	int *extensible)
{
	int status;

	/* The shared realm frame owns both input lifetime and the finite native operation depth. */
	status = access_extensibility(realm, object, 0, extensible);
	if (status != 0)
		return status;

	/* Succeeded: the completed operation supplied the target's actual extensibility. */
	return 0;
}

/*
 * Prevents extensions through realm-aware Proxy handling and the existing pure native policy.
 */
int
vm_prevent_extensions(
	struct vm_realm *realm,
	struct vm_object *object,
	int *done)
{
	int status;

	/* Refusal and abrupt completion remain separate from the operation's Boolean success. */
	status = access_extensibility(realm, object, 1, done);
	if (status != 0)
		return status;

	/* Succeeded: done reports whether the target accepted preventing extensions. */
	return 0;
}

/*
 * Determines array identity through immutable Proxy targets without invoking any script operation.
 */
int
vm_is_array(
	struct vm_realm *realm,
	vm_value value,
	int *is_array)
{
	struct access_read_roots roots;
	struct vm_object *object;
	struct vm_proxy *proxy;
	int is_object;
	int status;

	/* Primitives never coerce, and ordinary array identity requires no allocation. */
	*is_array = 0;
	is_object = vm_value_is_object(value);
	if (!is_object)
		return 0;
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_PROXY) {
		if ((object->flags & VM_OBJECT_ARRAY) != 0U)
			*is_array = 1;
		return 0;
	}

	/* A revoked-target error may collect both its managed realm and otherwise unowned original Proxy. */
	roots.realm = realm;
	roots.base = value;
	roots.key = VM_VALUE_UNDEFINED;
	roots.receiver = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Targets are existing objects fixed at allocation, so the chain cannot form a cycle. */
	while (object->kind == VM_KIND_PROXY) {
		proxy = (struct vm_proxy *)object;
		if (proxy->target == VM_VALUE_NULL) {
			status = vm_throw_type_error(realm, "Cannot determine array identity of a revoked Proxy");
			break;
		}

		/* Neither handler properties nor prototype access participates in IsArray. */
		object = (struct vm_object *)vm_value_as_cell(proxy->target);
	}

	/* Only a successfully reached target contributes its immutable array identity. */
	if (status == 0) {
		if ((object->flags & VM_OBJECT_ARRAY) != 0U)
			*is_array = 1;
	}

	/* Retire the native ownership frame before propagating an abrupt outcome. */
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: the original Proxy frame no longer owns any collector edge. */
	return 0;
}

/*
 * Gets a property of any value: an object's through its chain (calling an
 * accessor's getter), a string's length and characters; reading from
 * undefined or null throws.
 */
int
vm_get(
	struct vm_realm *realm,
	vm_value base,
	vm_value key,
	vm_value *result)
{
	int status;

	/* Ordinary property access uses its base as the initial receiver. */
	status = vm_get_receiver(realm, base, key, base, result);
	if (status != 0)
		return status;

	/* Succeeded: the ordinary receiver convention is preserved. */
	return 0;
}

/*
 * Gets a property while retaining the receiver through prototypes and Proxy fallback.
 * Explicit roots support direct native callers and getter-driven collection.
 */
int
vm_get_receiver(
	struct vm_realm *realm,
	vm_value base,
	vm_value key,
	vm_value receiver,
	vm_value *result)
{
	struct access_read_roots roots;
	int status;

	/* Count native property recursion independently of interpreter call frames. */
	*result = VM_VALUE_UNDEFINED;
	if (realm->property_depth >= ACCESS_RECURSION_MAX) {
		status = vm_throw_range_error(realm, "Maximum property recursion exceeded");
		return status;
	}

	/* This native frame owns one property-operation depth until it returns. */
	realm->property_depth++;

	/* One native tracer retains the complete input graph before a getter can collect. */
	roots.realm = realm;
	roots.base = base;
	roots.key = key;
	roots.receiver = receiver;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0) {
		/* No tracer was acquired, but this attempted property operation still owns one depth count. */
		realm->property_depth--;
		return status;
	}

	/* The original receiver remains rooted through every getter and nested fallback. */
	status = access_get_receiver(realm, base, key, receiver, result);
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);

	/* Retire the operation count on both normal and abrupt completion. */
	realm->property_depth--;
	if (status != 0)
		return status;

	/* Succeeded: the result belongs to the completed property access. */
	return 0;
}

/*
 * Reads a method of a value (GetMethod): undefined for undefined or null,
 * a TypeError for anything else that cannot be called.
 */
int
vm_get_method(
	struct vm_realm *realm,
	vm_value value,
	vm_value key,
	vm_value *method)
{
	int callable;
	int status;

	/* The property. */
	*method = VM_VALUE_UNDEFINED;
	status = vm_get(realm, value, key, method);
	if (status != 0)
		return status;

	/* Undefined and null say there is no method. */
	if (*method == VM_VALUE_UNDEFINED || *method == VM_VALUE_NULL) {
		*method = VM_VALUE_UNDEFINED;
		return 0;
	}

	/* Anything else must be callable. */
	callable = vm_value_is_callable(*method);
	if (!callable) {
		*method = VM_VALUE_UNDEFINED;
		status = vm_throw_type_error(realm, "method is not a function");
		return status;
	}

	/* Succeeded: the method. */
	return 0;
}

/*
 * Puts a property of any value as sloppy code does (a refused assignment
 * is ignored).
 */
int
vm_put(
	struct vm_realm *realm,
	vm_value base,
	vm_value key,
	vm_value value)
{
	int status;

	/* The assignment without strict mode's errors. */
	status = vm_set(realm, base, key, value, 0);
	if (status != 0)
		return status;

	/* Succeeded: the value is put (or refused in silence). */
	return 0;
}

/*
 * Puts a property of any value by assignment: an object's through an
 * accessor's setter or into a data property; undefined and null throw.  A
 * refused assignment (a read-only property, an accessor without a setter,
 * an object that takes no new property, a primitive) is ignored, or a
 * TypeError in strict code.
 */
int
vm_set(
	struct vm_realm *realm,
	vm_value base,
	vm_value key,
	vm_value value,
	int strict)
{
	struct access_write_roots roots;
	struct vm_object *object;
	int is_object;
	int done;
	int status;

	/* undefined and null have no properties. */
	if (base == VM_VALUE_UNDEFINED || base == VM_VALUE_NULL) {
		status = access_base_error(realm, base, key, "set", "setting");
		return status;
	}

	/* A primitive takes no property, but its prototype's setter is called with it. */
	is_object = vm_value_is_object(base);
	if (!is_object) {
		status = access_set_primitive(realm, base, key, value, strict);
		return status;
	}

	/* Every object uses its actual receiver-aware operation, including exotic prototypes of ordinary receivers. */
	object = (struct vm_object *)vm_value_as_cell(base);
	roots.read.realm = realm;
	roots.read.base = base;
	roots.read.key = key;
	roots.read.receiver = base;
	roots.value = value;
	status = vm_heap_add_tracer(realm->heap, access_write_trace, &roots);
	if (status != 0)
		return status;
	status = vm_set_receiver(realm, object, key, value, base, &done);
	if (status != 0)
		goto cleanup;
	if (!done) {
		/* Keep native input ownership until any strict refusal exception has finished allocating. */
		if (object->kind == VM_KIND_PROXY)
			status = access_refuse(realm, strict, "Proxy set trap returned false");
		else
			status = access_refuse(realm, strict, "Cannot assign to read only property");
	}

cleanup:
	/* Accepted writes, sloppy refusals and abrupt operations all retire the outer assignment owner. */
	vm_heap_remove_tracer(realm->heap, access_write_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: the actual operation accepted the write, or sloppy code ignored its refusal. */
	return 0;
}

/*
 * Sets an object's property while preserving the original receiver and Boolean refusal.
 * Ordinary, array, Proxy and fixed-buffer TypedArray dispatch share explicit native input ownership.
 */
int
vm_set_receiver(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	vm_value value,
	vm_value receiver,
	int *done)
{
	struct access_write_roots roots;
	int status;

	/* Retain every input before either an exotic trap or ordinary descriptor callback can collect. */
	*done = 0;
	roots.read.realm = realm;
	roots.read.base = vm_value_cell(object);
	roots.read.key = key;
	roots.read.receiver = receiver;
	roots.value = value;
	status = vm_heap_add_tracer(realm->heap, access_write_trace, &roots);
	if (status != 0)
		return status;
	if (realm->property_depth >= ACCESS_RECURSION_MAX) {
		status = vm_throw_range_error(realm, "Maximum property recursion exceeded");
		vm_heap_remove_tracer(realm->heap, access_write_trace, &roots);
		return status;
	}

	/* One native operation depth covers both Proxy and ordinary dispatch. */
	realm->property_depth++;
	if (object->kind == VM_KIND_PROXY) {
		status = access_proxy_set(realm, (struct vm_proxy *)object, key, value, receiver, done);
	} else if (object->kind == VM_KIND_TYPED_ARRAY) {
		status = access_set_typed(realm, object, key, value, receiver, done);
	} else {
		status = access_set_receiver(realm, object, key, value, receiver, done);
	}

	/* Neither a thrown trap nor a refused ordinary definition may leak depth or input ownership. */
	realm->property_depth--;
	vm_heap_remove_tracer(realm->heap, access_write_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: done says whether the receiver accepted the property assignment. */
	return 0;
}

/*
 * Deletes an own property through realm-aware Proxy handling or the existing pure native policy.
 */
int
vm_delete_property(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	int *deleted)
{
	struct access_read_roots roots;
	int status;

	/* The input and managed owner must survive even a recursion-limit error allocation. */
	*deleted = 0;
	roots.realm = realm;
	roots.base = vm_value_cell(object);
	roots.key = key;
	roots.receiver = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;
	if (realm->property_depth >= ACCESS_RECURSION_MAX) {
		status = vm_throw_range_error(realm, "Maximum property recursion exceeded");
		vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
		return status;
	}

	/* The ordinary path retains its exact native policy and deletion semantics. */
	realm->property_depth++;
	if (object->kind == VM_KIND_PROXY)
		status = access_proxy_delete(realm, (struct vm_proxy *)object, key, deleted);
	else
		status = vm_object_delete(realm->heap, object, key, deleted);
	realm->property_depth--;
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: refusal remains a Boolean for the caller's strict-mode policy to interpret. */
	return 0;
}

/*
 * Deletes a property of any value (the delete operator) and stores whether
 * it is gone; a property that stays is a TypeError in strict code.
 */
int
vm_delete(
	struct vm_realm *realm,
	vm_value base,
	vm_value key,
	int strict,
	vm_value *result)
{
	struct vm_object *object;
	struct vm_string *string;
	vm_value ignored;
	int is_string;
	int is_object;
	int found;
	int deleted;
	int status;

	/* undefined and null have no properties. */
	*result = VM_VALUE_TRUE;
	if (base == VM_VALUE_UNDEFINED || base == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot convert undefined or null to object");
		return status;
	}

	/* A string's length and characters stay; any other property of a primitive is not there. */
	is_object = vm_value_is_object(base);
	if (!is_object) {
		is_string = vm_value_is_string(base);
		found = 0;
		if (is_string) {
			string = (struct vm_string *)vm_value_as_cell(base);
			status = access_string_property(realm, string, key, &ignored, &found);
			if (status != 0)
				return status;
		}

		/* A string keeps its own properties. */
		if (found) {
			*result = VM_VALUE_FALSE;
			status = access_refuse(realm, strict, "Cannot delete property of a string");
			return status;
		}

		/* Any other property of a primitive is not there. */
		return 0;
	}

	/* An object's own property, unless it is not configurable. */
	object = (struct vm_object *)vm_value_as_cell(base);
	status = vm_delete_property(realm, object, key, &deleted);
	if (status != 0)
		return status;
	if (!deleted) {
		*result = VM_VALUE_FALSE;
		status = access_refuse(realm, strict, "Cannot delete property");
		return status;
	}

	/* Succeeded: the property is gone. */
	return 0;
}

/*
 * Tells whether an object has a property on its chain (the in operator);
 * the right side must be an object.
 */
int
vm_in(
	struct vm_realm *realm,
	vm_value key,
	vm_value object,
	vm_value *result)
{
	struct access_read_roots roots;
	vm_value property_key;
	int is_object;
	int found;
	int status;

	/* Keep both language operands and their owner alive before property-key conversion can run script. */
	*result = VM_VALUE_FALSE;
	roots.realm = realm;
	roots.base = object;
	roots.key = key;
	roots.receiver = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* The right operand must be an object before the left operand is converted. */
	is_object = vm_value_is_object(object);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Cannot use 'in' operator to search for a key in a primitive");
		goto cleanup;
	}

	/* A fresh symbol returned by conversion also belongs to the active native frame. */
	status = vm_to_key(realm, key, &property_key);
	if (status != 0)
		goto cleanup;
	roots.receiver = property_key;
	status = vm_has_property(realm, (struct vm_object *)vm_value_as_cell(object), property_key, &found);
	if (status != 0)
		goto cleanup;
	*result = vm_value_boolean(found);

cleanup:
	/* No tracer may outlive the operand slots it observes. */
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: the actual object's HasProperty result supplies the language Boolean. */
	return 0;
}

/*
 * Searches an object's property chain through realm-aware HasProperty operations.
 * Script-free heap property hooks retain their existing contract.
 */
int
vm_has_property(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	int *found)
{
	struct access_read_roots roots;
	int status;

	/* Bound native fallback recursion independently from interpreter call frames. */
	*found = 0;
	if (realm->property_depth >= ACCESS_RECURSION_MAX) {
		status = vm_throw_range_error(realm, "Maximum property recursion exceeded");
		return status;
	}

	/* This HasProperty frame owns one recursion count until its normal or abrupt return. */
	realm->property_depth++;

	/* Property presence may invoke a Proxy trap that collects its own input graph. */
	roots.realm = realm;
	roots.base = vm_value_cell(object);
	roots.key = key;
	roots.receiver = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0) {
		/* This attempted operation still owns its depth even when tracer registration failed. */
		realm->property_depth--;
		return status;
	}

	/* Run the actual ordinary or Proxy presence operation while its complete input graph is rooted. */
	status = access_has_property(realm, object, key, found);
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);

	/* Retire property-operation depth on both normal and abrupt completion. */
	realm->property_depth--;
	if (status != 0)
		return status;

	/* Succeeded: found reports actual property presence. */
	return 0;
}

/*
 * Tells whether a value is an instance of a constructor (the instanceof
 * operator): whether the constructor's prototype is on the value's chain.
 */
int
vm_instanceof(
	struct vm_realm *realm,
	vm_value value,
	vm_value constructor,
	vm_value *result)
{
	struct access_read_roots roots;
	int status;

	/* Direct native calls retain both operands and the managed realm before any custom method lookup. */
	*result = VM_VALUE_FALSE;
	roots.realm = realm;
	roots.base = constructor;
	roots.key = VM_VALUE_UNDEFINED;
	roots.receiver = value;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Every constructor getter and custom hasInstance callback remains within this native ownership boundary. */
	status = access_instanceof(realm, value, constructor, result);
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: the caller receives the complete instance classification. */
	return 0;
}

/*
 * Tells whether a value is an instance of a constructor by its prototype
 * chain (OrdinaryHasInstance, Function.prototype[Symbol.hasInstance]): a
 * value that cannot be called has no instances.
 */
int
vm_ordinary_has_instance(
	struct vm_realm *realm,
	vm_value constructor,
	vm_value value,
	vm_value *result)
{
	struct access_read_roots roots;
	struct vm_cell *constructor_cell;
	struct vm_function *function;
	struct vm_cell *prototype_root;
	vm_value key;
	vm_value prototype;
	vm_value link;
	int callable;
	int is_object;
	int status;

	/* A non-callable has no instances; bound callables can delegate even for primitive values. */
	*result = VM_VALUE_FALSE;
	callable = vm_value_is_callable(constructor);
	if (!callable)
		return 0;

	/* A managed caller, constructor and original value survive key allocation and constructor getters. */
	roots.realm = realm;
	roots.base = constructor;
	roots.key = VM_VALUE_UNDEFINED;
	roots.receiver = value;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;
	prototype_root = NULL;
	status = vm_heap_add_root(realm->heap, &prototype_root);
	if (status != 0) {
		vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
		return status;
	}

	/* BoundTargetFunction delegation precedes the primitive-value refusal and may use a custom hasInstance. */
	constructor_cell = vm_value_as_cell(constructor);
	if (constructor_cell->type == &vm_function_type) {
		function = (struct vm_function *)constructor_cell;
		if (function->bound_target != VM_VALUE_UNDEFINED) {
			status = vm_instanceof(realm, value, function->bound_target, result);
			goto cleanup;
		}
	}

	/* Only a non-bound callable with an object operand needs a prototype-chain walk. */
	is_object = vm_value_is_object(value);
	if (!is_object) {
		status = 0;
		goto cleanup;
	}

	/* The constructor's prototype is retrieved before any value-side prototype method is called. */
	key = vm_key_from_ascii(realm->heap, "prototype");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Preserve the property key through a getter that may allocate or collect. */
	roots.key = key;
	status = vm_get(realm, constructor, key, &prototype);
	if (status != 0)
		goto cleanup;
	is_object = vm_value_is_object(prototype);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Function has non-object prototype in instanceof check");
		goto cleanup;
	}

	/* The target prototype and the latest returned link remain independently owned through later traps. */
	prototype_root = vm_value_as_cell(prototype);
	while (roots.receiver != VM_VALUE_NULL) {
		status = vm_get_prototype(realm, (struct vm_object *)vm_value_as_cell(roots.receiver), &link);
		if (status != 0)
			goto cleanup;
		roots.receiver = link;
		if (link == prototype) {
			*result = VM_VALUE_TRUE;
			break;
		}
	}

	/* Reaching null without matching the prototype is a successful false result. */
	status = 0;

cleanup:
	/* Neither abrupt chain traversal nor invalid constructor output may leak native ownership. */
	vm_heap_remove_root(realm->heap, &prototype_root);
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: the actual internal prototype methods supplied the instance result. */
	return 0;
}

/*
 * Defines a data property of an object literal (not an assignment: a
 * setter on the chain is not called).
 */
int
vm_define_data(
	struct vm_realm *realm,
	vm_value object,
	vm_value key,
	vm_value value)
{
	struct vm_object *target;
	int status;

	/* The property, writable, enumerable and configurable. */
	target = (struct vm_object *)vm_value_as_cell(object);
	status = vm_object_define(realm->heap, target, key, value, VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* Succeeded: the property is defined. */
	return 0;
}

/*
 * Defines the getter (or, with setter, the setter) of an object literal's
 * accessor property, keeping the other half when the property is an
 * accessor already.
 */
int
vm_define_accessor(
	struct vm_realm *realm,
	vm_value object,
	vm_value key,
	vm_value function,
	int setter)
{
	struct vm_property property;
	struct vm_accessor *accessor;
	struct vm_object *target;
	struct vm_cell *accessor_root;
	vm_value getter_function;
	vm_value setter_function;
	int found;
	int status;

	/* The other half, from an own accessor already there. */
	target = (struct vm_object *)vm_value_as_cell(object);
	getter_function = VM_VALUE_UNDEFINED;
	setter_function = VM_VALUE_UNDEFINED;
	found = vm_object_get_own(target, key, &property);
	if (found < 0)
		return -found;

	/* Present descriptors retain their ordinary accessor and attribute rules. */
	if (found && (property.attributes & VM_PROPERTY_ACCESSOR) != 0U) {
		accessor = (struct vm_accessor *)vm_value_as_cell(*property.value);
		getter_function = accessor->getter;
		setter_function = accessor->setter;
	}

	/* This half. */
	if (setter) {
		setter_function = function;
	} else {
		getter_function = function;
	}

	/* The pair, as an enumerable and configurable accessor property. */
	accessor = vm_accessor_create(realm->heap, getter_function, setter_function);
	if (accessor == NULL)
		return ENOMEM;

	/* The new pair is not owned by the literal until definition publishes it. */
	accessor_root = &accessor->cell;
	status = vm_heap_add_root(realm->heap, &accessor_root);
	if (status != 0)
		return status;
	status = vm_object_define(realm->heap, target, key, vm_value_cell(accessor),
	    VM_PROPERTY_ACCESSOR | VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);
	vm_heap_remove_root(realm->heap, &accessor_root);
	if (status != 0)
		return status;

	/* Succeeded: the accessor is defined. */
	return 0;
}

/*
 * Reads a global variable; a missing one is a ReferenceError, or
 * undefined for typeof.
 */
int
vm_get_global(
	struct vm_realm *realm,
	vm_value key,
	int for_typeof,
	vm_value *result)
{
	struct vm_property property;
	int found;
	int status;

	/* A script's top-level let or const comes first; before its declaration runs it cannot be read (not even by typeof). */
	*result = VM_VALUE_UNDEFINED;
	found = vm_object_get_own_ordinary(realm->lexicals, key, &property);
	if (found && *property.value == VM_VALUE_EMPTY) {
		status = vm_throw_uninitialized(realm, key);
		return status;
	}

	/* An initialized one is read from the record. */
	if (found) {
		*result = *property.value;
		return 0;
	}

	/* A name the global object does not have. */
	found = vm_object_find(realm->global, key, &property);
	if (found < 0)
		return -found;

	/* A missing descriptor follows the absence path after errors have been excluded. */
	if (!found) {
		if (for_typeof)
			return 0;
		status = vm_throw_not_defined(realm, key);
		return status;
	}

	/* The property's value (through a getter). */
	status = vm_get(realm, vm_value_cell(realm->global), key, result);
	if (status != 0)
		return status;

	/* Succeeded: the variable's value. */
	return 0;
}

/*
 * Assigns a global variable: sloppy code makes a missing one; strict code
 * may not (a ReferenceError).
 */
int
vm_put_global(
	struct vm_realm *realm,
	vm_value key,
	vm_value value,
	int strict)
{
	struct vm_property property;
	int found;
	int status;

	/* A script's top-level let or const: not before its declaration runs, and never a const. */
	found = vm_object_get_own_ordinary(realm->lexicals, key, &property);
	if (found && *property.value == VM_VALUE_EMPTY) {
		status = vm_throw_uninitialized(realm, key);
		return status;
	}

	/* A const keeps its value. */
	if (found && (property.attributes & VM_PROPERTY_WRITABLE) == 0U) {
		status = vm_throw_type_error(realm, "Assignment to constant variable.");
		return status;
	}

	/* A let takes the new one. */
	if (found) {
		*property.value = value;
		return 0;
	}

	/* Strict code assigns only a variable that exists. */
	if (strict) {
		found = vm_object_find(realm->global, key, &property);
		if (found < 0)
			return -found;

		/* A missing descriptor follows the absence path after errors have been excluded. */
		if (!found) {
			status = vm_throw_not_defined(realm, key);
			return status;
		}
	}

	/* The assignment to the global object. */
	status = vm_set(realm, vm_value_cell(realm->global), key, value, strict);
	if (status != 0)
		return status;

	/* Succeeded: the variable has the value. */
	return 0;
}

/*
 * Declares a var of a script: a global property that cannot be deleted,
 * undefined unless the global object has the name already.
 */
int
vm_define_global_var(
	struct vm_realm *realm,
	vm_value key)
{
	struct vm_property property;
	int found;
	int status;

	/* A name the global object has keeps its value. */
	found = vm_object_get_own(realm->global, key, &property);
	if (found < 0)
		return -found;

	/* Present descriptors retain their ordinary accessor and attribute rules. */
	if (found)
		return 0;

	/* A new variable: writable and enumerable, not configurable. */
	status = vm_object_define(realm->heap, realm->global, key, VM_VALUE_UNDEFINED,
	    VM_PROPERTY_WRITABLE | VM_PROPERTY_ENUMERABLE);
	if (status == EPERM) {
		status = vm_throw_type_error(realm, "Cannot declare a global variable");
		return status;
	}

	/* Any other failure of the definition. */
	if (status != 0)
		return status;

	/* Succeeded: the variable exists. */
	return 0;
}

/*
 * Declares a function of a script: the global property takes the
 * function; a property there already must be configurable, or a writable
 * and enumerable data property.
 */
int
vm_define_global_function(
	struct vm_realm *realm,
	vm_value key,
	vm_value function)
{
	struct vm_property property;
	uint32_t attributes;
	int found;
	int replaceable;
	int status;

	/* A property there already decides whether the declaration may replace it. */
	attributes = VM_PROPERTY_WRITABLE | VM_PROPERTY_ENUMERABLE;
	found = vm_object_get_own(realm->global, key, &property);
	if (found < 0)
		return -found;

	/* Present descriptors retain their ordinary accessor and attribute rules. */
	if (found && (property.attributes & VM_PROPERTY_CONFIGURABLE) == 0U) {
		replaceable = 0;
		if ((property.attributes & VM_PROPERTY_ACCESSOR) == 0U &&
		    (property.attributes & VM_PROPERTY_WRITABLE) != 0U &&
		    (property.attributes & VM_PROPERTY_ENUMERABLE) != 0U)
			replaceable = 1;
		if (!replaceable) {
			status = vm_throw_type_error(realm, "Cannot redefine a global function");
			return status;
		}

		/* A variable of the script takes the function as its value. */
		attributes = property.attributes;
	}

	/* The property. */
	status = vm_object_define(realm->heap, realm->global, key, function, attributes);
	if (status == EPERM) {
		status = vm_throw_type_error(realm, "Cannot declare a global function");
		return status;
	}

	/* Any other failure of the definition. */
	if (status != 0)
		return status;

	/* Succeeded: the function is declared. */
	return 0;
}

/*
 * Deletes a global variable (delete of a bare name in sloppy code) and
 * stores whether it is gone.
 */
int
vm_delete_global(
	struct vm_realm *realm,
	vm_value key,
	vm_value *result)
{
	struct vm_property property;
	int found;
	int status;

	/* A script's top-level let or const cannot be deleted. */
	found = vm_object_get_own_ordinary(realm->lexicals, key, &property);
	if (found) {
		*result = VM_VALUE_FALSE;
		return 0;
	}

	/* The global object's property (a var of a script is not configurable and stays). */
	status = vm_delete(realm, vm_value_cell(realm->global), key, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: whether it is gone. */
	return 0;
}

/*
 * Declares a script's top-level let or const in the realm's record of
 * them, holding the empty value until its declaration runs.  A name the
 * record has already (from this or an earlier script) is a SyntaxError.
 */
int
vm_define_global_lexical(
	struct vm_realm *realm,
	vm_value key,
	int is_const)
{
	struct vm_property property;
	uint32_t attributes;
	int found;
	int status;

	/* A name declared already. */
	found = vm_object_get_own_ordinary(realm->lexicals, key, &property);
	if (found) {
		status = vm_throw_redeclared(realm, key);
		return status;
	}

	/* A const is a property that cannot be written; a let one that can. */
	attributes = VM_PROPERTY_ENUMERABLE;
	if (!is_const)
		attributes |= VM_PROPERTY_WRITABLE;
	status = vm_object_define(realm->heap, realm->lexicals, key, VM_VALUE_EMPTY, attributes);
	if (status != 0)
		return status;

	/* Succeeded: the name is declared, not yet initialized. */
	return 0;
}

/*
 * Runs the declaration of a script's top-level let or const: the binding
 * takes its first value (a const too).
 */
int
vm_init_global_lexical(
	struct vm_realm *realm,
	vm_value key,
	vm_value value)
{
	struct vm_property property;
	int found;

	/* The record has the name, which the script's prologue declared. */
	found = vm_object_get_own_ordinary(realm->lexicals, key, &property);
	if (!found)
		return EINVAL;

	/* Succeeded: the binding has its value. */
	*property.value = value;
	return 0;
}

/*
 * Converts a value to an object (ToObject): an object is itself; a
 * boolean, a number, a string or a symbol gets a wrapper object from its
 * prototype; undefined and null throw.
 */
int
vm_to_object(
	struct vm_realm *realm,
	vm_value value,
	vm_value *object)
{
	struct vm_object *wrapper;
	struct vm_object *prototype;
	struct vm_cell *wrapper_root;
	int is_object;
	int is_string;
	int is_boolean;
	int is_number;
	int status;

	/* An object is itself. */
	is_object = vm_value_is_object(value);
	if (is_object) {
		*object = value;
		return 0;
	}

	/* undefined and null have no object. */
	if (value == VM_VALUE_UNDEFINED || value == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot convert undefined or null to object");
		return status;
	}

	/* The wrapper, from the primitive's prototype (Object.prototype before the built-ins). */
	prototype = access_primitive_prototype(realm, value);
	if (prototype == NULL)
		prototype = realm->object_prototype;
	wrapper = vm_object_create(realm->heap, prototype);
	if (wrapper == NULL)
		return ENOMEM;

	/* A primitive wrapper is detached while its indexed characters are built. */
	wrapper_root = &wrapper->cell;
	status = vm_heap_add_root(realm->heap, &wrapper_root);
	if (status != 0)
		return status;
	wrapper->internal = value;

	/* Its kind by the primitive's type; a string's characters and length are its own properties. */
	is_string = vm_value_is_string(value);
	is_boolean = vm_value_is_boolean(value);
	is_number = vm_value_is_number(value);
	if (is_boolean) {
		wrapper->kind = VM_KIND_BOOLEAN;
	} else if (is_number) {
		wrapper->kind = VM_KIND_NUMBER;
	} else if (is_string) {
		wrapper->kind = VM_KIND_STRING;
		status = access_wrap_string(realm, wrapper, (struct vm_string *)vm_value_as_cell(value));
		if (status != 0) {
			vm_heap_remove_root(realm->heap, &wrapper_root);
			return status;
		}
	} else if (vm_value_as_bigint(value) != NULL) {
		wrapper->kind = VM_KIND_BIGINT;
	} else {
		wrapper->kind = VM_KIND_SYMBOL;
	}

	/* Succeeded: the wrapper. */
	*object = vm_value_cell(wrapper);
	vm_heap_remove_root(realm->heap, &wrapper_root);
	return 0;
}

/*
 * Reads an object's own property as a descriptor; reports whether it has
 * one.
 */
int
vm_get_own_descriptor(
	struct vm_object *object,
	vm_value key,
	struct vm_descriptor *descriptor)
{
	struct vm_property property;
	struct vm_accessor *accessor;
	int found;

	/* The own property. */
	memset(descriptor, 0, sizeof(*descriptor));
	found = vm_object_get_own(object, key, &property);
	if (found < 0)
		return found;

	/* A missing descriptor follows the absence path after errors have been excluded. */
	if (!found)
		return 0;

	/* An accessor's getter and setter, or a data property's value and writability. */
	descriptor->attributes = property.attributes & (VM_PROPERTY_WRITABLE | VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);
	descriptor->has = VM_HAS_ENUMERABLE | VM_HAS_CONFIGURABLE;
	if ((property.attributes & VM_PROPERTY_ACCESSOR) != 0U) {
		accessor = (struct vm_accessor *)vm_value_as_cell(*property.value);
		descriptor->getter = accessor->getter;
		descriptor->setter = accessor->setter;
		descriptor->has |= VM_HAS_GET | VM_HAS_SET;
		descriptor->attributes &= ~VM_PROPERTY_WRITABLE;
	} else {
		descriptor->value = *property.value;
		descriptor->has |= VM_HAS_VALUE | VM_HAS_WRITABLE;
	}

	/* It has the property. */
	return 1;
}

/*
 * Reads an own property through the realm-aware language operation.
 * Script-free ordinary hooks remain separate from native Proxy traps.
 */
int
vm_get_own_property(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	struct vm_descriptor *descriptor,
	int *found)
{
	struct access_read_roots roots;
	int present;
	int status;

	/* No descriptor is published before a successful complete internal operation. */
	*found = 0;
	memset(descriptor, 0, sizeof(*descriptor));
	if (realm->property_depth >= ACCESS_RECURSION_MAX) {
		status = vm_throw_range_error(realm, "Maximum property recursion exceeded");
		return status;
	}

	/* Retain the owner and key before any trap, native hook or nested operation can collect. */
	realm->property_depth++;
	roots.realm = realm;
	roots.base = vm_value_cell(object);
	roots.key = key;
	roots.receiver = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0) {
		realm->property_depth--;
		return status;
	}

	/* Proxy lookup can run script; ordinary descriptors keep the original heap-only protocol. */
	if (object->kind == VM_KIND_PROXY) {
		status = access_proxy_get_own(realm, (struct vm_proxy *)object, key, descriptor, found);
	} else {
		present = vm_get_own_descriptor(object, key, descriptor);
		status = 0;
		if (present < 0) {
			status = -present;
		} else {
			*found = present;
		}
	}

	/* Balance native depth and frame ownership on normal and abrupt returns. */
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
	realm->property_depth--;
	if (status != 0)
		return status;

	/* Succeeded: the descriptor and its independent presence result belong to the caller. */
	return 0;
}

/*
 * Defines an object's own property from a descriptor as the language
 * validates it (ValidateAndApplyPropertyDescriptor, and an array's length
 * and indices); done says whether it was allowed.
 */
int
vm_define_own_property(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	const struct vm_descriptor *descriptor,
	int *done)
{
	struct vm_descriptor current;
	struct vm_accessor *accessor;
	struct vm_cell *accessor_root;
	vm_value length_key;
	vm_value getter;
	vm_value setter;
	vm_value value;
	uint32_t attributes;
	uint32_t index;
	int found;
	int allowed;
	int is_index;
	int is_accessor;
	int is_array;
	int status;
	int handled;

	/* Realm-aware Proxy definition owns its traps and leaves heap-only native policy unchanged. */
	if (object->kind == VM_KIND_PROXY) {
		status = access_proxy_define(realm, (struct vm_proxy *)object, key, descriptor, done);
		if (status != 0)
			return status;
		return 0;
	}

	/* TypedArray value conversion belongs to this realm layer before pure native policy. */
	if (object->kind == VM_KIND_TYPED_ARRAY) {
		status = access_define_typed(realm, object, key, descriptor, &handled, done);
		if (status != 0)
			return status;
		if (handled)
			return 0;
	}

	/* Native policy may reject or handle a descriptor before ordinary validation. */
	*done = 0;
	if (object->native_operations != NULL && object->native_operations->define != NULL) {
		handled = 0;
		status = object->native_operations->define(realm, object, key, descriptor, &handled, done);
		if (status != 0)
			return status;
		if (handled)
			return 0;
	}

	/* The property there now. */
	*done = 0;
	found = vm_get_own_descriptor(object, key, &current);
	if (found < 0)
		return -found;

	/* Array descriptor handling resolves the separate length metadata key. */
	length_key = vm_key_from_ascii(realm->heap, "length");
	if (length_key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* An array's length has rules of its own. */
	is_array = 0;
	if ((object->flags & VM_OBJECT_ARRAY) != 0U)
		is_array = 1;
	if (is_array && key == length_key) {
		status = access_define_length(realm, object, &current, descriptor, done);
		if (status != 0)
			return status;
		return 0;
	}

	/* An array takes no index past a length that cannot change. */
	is_index = vm_value_is_array_index(key, &index);
	if (is_array &&
	    is_index &&
	    !found &&
	    index >= object->length) {
		access_get_length_attributes(object, length_key, &attributes);
		if ((attributes & VM_PROPERTY_WRITABLE) == 0U)
			return 0;
	}

	/* Whether the change is allowed from what is there. */
	allowed = access_descriptor_allowed(object, found, &current, descriptor);
	if (!allowed)
		return 0;

	/* The kind after the change: the descriptor's, or (for a generic one) what is there. */
	is_accessor = 0;
	if ((descriptor->has & (VM_HAS_GET | VM_HAS_SET)) != 0U)
		is_accessor = 1;
	if ((descriptor->has & (VM_HAS_VALUE | VM_HAS_WRITABLE | VM_HAS_GET | VM_HAS_SET)) == 0U &&
	    found &&
	    (current.has & VM_HAS_GET) != 0U)
		is_accessor = 1;

	/* The enumerability and configurability: given, or kept, or false. */
	attributes = 0;
	if (found)
		attributes = current.attributes & (VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);
	if ((descriptor->has & VM_HAS_ENUMERABLE) != 0U)
		attributes = (attributes & ~VM_PROPERTY_ENUMERABLE) | (descriptor->attributes & VM_PROPERTY_ENUMERABLE);
	if ((descriptor->has & VM_HAS_CONFIGURABLE) != 0U)
		attributes = (attributes & ~VM_PROPERTY_CONFIGURABLE) | (descriptor->attributes & VM_PROPERTY_CONFIGURABLE);

	/* An accessor: the getter and setter given, or those it had. */
	if (is_accessor) {
		getter = VM_VALUE_UNDEFINED;
		setter = VM_VALUE_UNDEFINED;
		if (found && (current.has & VM_HAS_GET) != 0U) {
			getter = current.getter;
			setter = current.setter;
		}

		/* The ones given. */
		if ((descriptor->has & VM_HAS_GET) != 0U)
			getter = descriptor->getter;
		if ((descriptor->has & VM_HAS_SET) != 0U)
			setter = descriptor->setter;
		accessor = vm_accessor_create(realm->heap, getter, setter);
		if (accessor == NULL)
			return ENOMEM;

		/* The descriptor owns the pair only after the property is published. */
		accessor_root = &accessor->cell;
		status = vm_heap_add_root(realm->heap, &accessor_root);
		if (status != 0)
			return status;
		status = vm_object_define(realm->heap, object, key, vm_value_cell(accessor), attributes | VM_PROPERTY_ACCESSOR);
		vm_heap_remove_root(realm->heap, &accessor_root);
		if (status != 0)
			return status;
		*done = 1;
		return 0;
	}

	/* A data property: the value and writability given, or those it had. */
	value = VM_VALUE_UNDEFINED;
	if (found && (current.has & VM_HAS_VALUE) != 0U) {
		value = current.value;
		attributes |= current.attributes & VM_PROPERTY_WRITABLE;
	}

	/* The ones given. */
	if ((descriptor->has & VM_HAS_VALUE) != 0U)
		value = descriptor->value;
	if ((descriptor->has & VM_HAS_WRITABLE) != 0U)
		attributes = (attributes & ~VM_PROPERTY_WRITABLE) | (descriptor->attributes & VM_PROPERTY_WRITABLE);
	status = vm_object_define(realm->heap, object, key, value, attributes);
	if (status != 0)
		return status;

	/* Succeeded: the property is defined. */
	*done = 1;
	return 0;
}

/*
 * Tells whether two values are the same value (SameValue: NaN is itself,
 * +0 and -0 differ).
 */
int
vm_same_value(
	vm_value left,
	vm_value right)
{
	double left_number;
	double right_number;
	int left_is_number;
	int right_is_number;
	int left_negative;
	int right_negative;
	int equal;

	/* Numbers: NaN equals NaN, and the signs of zeros count. */
	left_is_number = vm_value_is_number(left);
	right_is_number = vm_value_is_number(right);
	if (left_is_number && right_is_number) {
		left_number = vm_value_as_number(left);
		right_number = vm_value_as_number(right);
		if (left_number != left_number && right_number != right_number)
			return 1;
		if (left_number == 0.0 && right_number == 0.0) {
			left_negative = signbit(left_number) != 0;
			right_negative = signbit(right_number) != 0;
			if (left_negative == right_negative)
				return 1;
			return 0;
		}

		/* Other numbers by value. */
		if (left_number == right_number)
			return 1;
		return 0;
	}

	/* Anything else as ===. */
	equal = vm_strict_equals(left, right);
	if (equal)
		return 1;

	/* Different values. */
	return 0;
}

/*
 * Starts a lazy for-in loop over the boxed value and its prototype chain.
 * Undefined and null produce an empty iterator.  Each next call checks
 * actual descriptors before yielding an enumerable string name once.
 */
int
vm_for_in_start(
	struct vm_realm *realm,
	vm_value value,
	vm_value *iterator)
{
	struct access_for_in *state;
	struct vm_cell *roots[3];
	vm_value object;
	unsigned index;
	unsigned registered;
	int is_cell;
	int status;

	/* Direct native inputs and the managed caller survive boxing and iterator allocation. */
	memset(roots, 0, sizeof(roots));
	if (realm->managed)
		roots[0] = &realm->cell;
	is_cell = vm_value_is_cell(value);
	if (is_cell)
		roots[1] = vm_value_as_cell(value);
	registered = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns the slot only after root registration succeeds. */
		registered++;
	}

	/* Nullish loop heads are empty; every other primitive includes its boxed prototype chain. */
	object = VM_VALUE_NULL;
	if (value != VM_VALUE_UNDEFINED && value != VM_VALUE_NULL) {
		status = vm_to_object(realm, value, &object);
		if (status != 0)
			goto cleanup;
		roots[1] = vm_value_as_cell(object);
	}

	/* Publish only a fully initialized private cell; property methods run when next first visits a level. */
	state = vm_heap_alloc(realm->heap, &access_for_in_type, sizeof(*state));
	if (state == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Both empty vectors are safe to trace and finalize before any user trap is invoked. */
	state->object = object;
	wb_vector_init(&state->keys, sizeof(vm_value));
	wb_vector_init(&state->visited, sizeof(vm_value));
	state->index = 0;
	state->loaded = 0;
	roots[2] = &state->cell;
	*iterator = vm_value_cell(state);

	/* Every acquired registration retires on success and all allocation failures. */
cleanup:
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Report why boxing or state allocation could not finish. */
	if (status != 0)
		return status;

	/* Succeeded: the caller owns a lazy for-in iterator. */
	return 0;
}

/*
 * Moves a for-in loop to its next key (as a string), skipping the keys
 * deleted since it started; done when there is none left.
 */
int
vm_for_in_next(
	struct vm_realm *realm,
	vm_value iterator,
	vm_value *key,
	int *done)
{
	struct access_for_in *state;
	struct vm_descriptor descriptor;
	struct vm_string *string;
	struct vm_cell *roots[2];
	vm_value candidate;
	vm_value canonical;
	vm_value prototype;
	unsigned index;
	unsigned registered;
	int is_cell;
	int is_symbol;
	int listed;
	int found;
	int status;

	/* Only the private iterator kind belongs in the bytecode's iterator register. */
	*done = 1;
	*key = VM_VALUE_UNDEFINED;
	is_cell = vm_value_is_cell(iterator);
	if (!is_cell)
		return EINVAL;
	state = (struct access_for_in *)vm_value_as_cell(iterator);
	if (state->cell.type != &access_for_in_type)
		return EINVAL;

	/* A bare native caller relies on this operation to retain its realm and all iterator-owned state. */
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &state->cell;
	registered = 0;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup may retire this root only after acquisition succeeds. */
		registered++;
	}

	/* Finish one own-key list before obtaining the next actual prototype. */
	while (state->object != VM_VALUE_NULL) {
		if (!state->loaded) {
			/* A fresh level collects keys once, while the state tracer retains every appended entry. */
			wb_vector_clear(&state->keys);
			state->index = 0;
			status = vm_own_property_keys(realm, (struct vm_object *)vm_value_as_cell(state->object), &state->keys);
			if (status != 0)
				goto cleanup;

			/* Subsequent next calls resume this snapshot rather than invoking OwnPropertyKeys again. */
			state->loaded = 1;
		}

		/* A processed present name shadows prototypes even when its descriptor is not enumerable. */
		while (state->index < state->keys.count) {
			candidate = *(vm_value *)wb_vector_at(&state->keys, state->index);
			state->index++;
			is_symbol = access_is_symbol(candidate);
			if (is_symbol)
				continue;

			/* Canonical property keys compare by value even when a trap returned a fresh non-atom string. */
			status = vm_to_key(realm, candidate, &canonical);
			if (status != 0)
				goto cleanup;
			listed = access_key_listed(&state->visited, state->visited.count, canonical);
			if (listed)
				continue;

			/* Absence does not mark a name visited, so a prototype may still supply that name. */
			status = vm_get_own_property(realm, (struct vm_object *)vm_value_as_cell(state->object), canonical, &descriptor, &found);
			if (status != 0)
				goto cleanup;
			if (!found)
				continue;

			/* Native vector growth does not collect; only descriptor attributes remain relevant afterward. */
			status = wb_vector_push(&state->visited, &canonical);
			if (status != 0)
				goto cleanup;
			if ((descriptor.attributes & VM_PROPERTY_ENUMERABLE) == 0U)
				continue;

			/* The yielded name is a string, while its canonical key remains owned by the visited list. */
			status = vm_to_string(realm, canonical, &string);
			if (status != 0)
				goto cleanup;
			*key = vm_value_cell(string);
			*done = 0;
			goto cleanup;
		}

		/* A fresh prototype result transfers directly to the traced current-level slot before another callback. */
		status = vm_get_prototype(realm, (struct vm_object *)vm_value_as_cell(state->object), &prototype);
		if (status != 0)
			goto cleanup;
		state->object = prototype;

		/* The completed level no longer owns remaining keys; the next iteration must load its successor. */
		wb_vector_clear(&state->keys);
		state->index = 0;
		state->loaded = 0;
	}

	/* Reaching null produces the done result without consulting another property method. */
	status = 0;

	/* Return values need no allocation after the realm and iterator registrations are retired. */
cleanup:
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve original property-method exceptions and native errno after cleanup. */
	if (status != 0)
		return status;

	/* Succeeded: key/done describe this one iterator step. */
	return 0;
}

/*
 * Converts a language object into a partial property descriptor in observable field order.
 * Native roots retain transient payloads before later getters or traps can collect.
 */
int
vm_to_property_descriptor(
	struct vm_realm *realm,
	vm_value value,
	struct vm_descriptor *descriptor)
{
	struct access_descriptor_roots roots;
	vm_value field;
	int callable;
	int is_object;
	int truth;
	int status;

	/* Initialize every output payload before its conversion tracer becomes visible. */
	memset(descriptor, 0, sizeof(*descriptor));
	descriptor->value = VM_VALUE_UNDEFINED;
	descriptor->getter = VM_VALUE_UNDEFINED;
	descriptor->setter = VM_VALUE_UNDEFINED;
	roots.realm = realm;
	roots.source = value;
	roots.descriptor = descriptor;
	roots.made = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_descriptor_trace, &roots);
	if (status != 0)
		return status;

	/* Descriptor sources must be objects, including native Proxy values. */
	is_object = vm_value_is_object(value);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Property description must be an object");
		goto cleanup;
	}

	/* Convert enumerable before observing any later descriptor field. */
	field = VM_VALUE_FALSE;
	status = access_descriptor_field(realm, value, "enumerable", VM_HAS_ENUMERABLE, descriptor, &field);
	if (status != 0)
		goto cleanup;
	truth = vm_to_boolean(field);
	if (truth)
		descriptor->attributes |= VM_PROPERTY_ENUMERABLE;

	/* Configurable follows enumerable and precedes the data value. */
	field = VM_VALUE_FALSE;
	status = access_descriptor_field(realm, value, "configurable", VM_HAS_CONFIGURABLE, descriptor, &field);
	if (status != 0)
		goto cleanup;
	truth = vm_to_boolean(field);
	if (truth)
		descriptor->attributes |= VM_PROPERTY_CONFIGURABLE;

	/* The descriptor tracer retains a fresh value through every later getter. */
	status = access_descriptor_field(realm, value, "value", VM_HAS_VALUE, descriptor, &descriptor->value);
	if (status != 0)
		goto cleanup;

	/* Writable is converted before any accessor fields are inspected. */
	field = VM_VALUE_FALSE;
	status = access_descriptor_field(realm, value, "writable", VM_HAS_WRITABLE, descriptor, &field);
	if (status != 0)
		goto cleanup;
	truth = vm_to_boolean(field);
	if (truth)
		descriptor->attributes |= VM_PROPERTY_WRITABLE;

	/* A bad getter fails immediately, before HasProperty or Get can inspect the setter. */
	status = access_descriptor_field(realm, value, "get", VM_HAS_GET, descriptor, &descriptor->getter);
	if (status != 0)
		goto cleanup;
	callable = vm_value_is_callable(descriptor->getter);
	if ((descriptor->has & VM_HAS_GET) != 0U &&
	    !callable &&
	    descriptor->getter != VM_VALUE_UNDEFINED) {
		status = vm_throw_type_error(realm, "Getter must be a function");
		goto cleanup;
	}

	/* Validate the setter immediately after its own read. */
	status = access_descriptor_field(realm, value, "set", VM_HAS_SET, descriptor, &descriptor->setter);
	if (status != 0)
		goto cleanup;
	callable = vm_value_is_callable(descriptor->setter);
	if ((descriptor->has & VM_HAS_SET) != 0U &&
	    !callable &&
	    descriptor->setter != VM_VALUE_UNDEFINED) {
		status = vm_throw_type_error(realm, "Setter must be a function");
		goto cleanup;
	}

	/* A descriptor is an accessor or a data property, not both. */
	if ((descriptor->has & (VM_HAS_GET | VM_HAS_SET)) != 0U && (descriptor->has & (VM_HAS_VALUE | VM_HAS_WRITABLE)) != 0U) {
		status = vm_throw_type_error(realm, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
		goto cleanup;
	}

cleanup:
	/* The caller takes the completed descriptor after its getter sequence. */
	vm_heap_remove_tracer(realm->heap, access_descriptor_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: the descriptor. */
	return 0;
}

/*
 * Makes an ordinary object containing exactly the present property-descriptor fields.
 * A missing descriptor is represented by undefined rather than an allocated object.
 */
int
vm_from_property_descriptor(
	struct vm_realm *realm,
	const struct vm_descriptor *descriptor,
	vm_value *result)
{
	struct access_descriptor_roots roots;
	struct vm_object *made;
	int truth;
	int status;

	/* Undefined has no fields to expose or managed inputs to retain. */
	*result = VM_VALUE_UNDEFINED;
	if (descriptor == NULL)
		return 0;

	/* Only fields marked present are traced, so absent C payload slots need not be initialized. */
	roots.realm = realm;
	roots.source = VM_VALUE_UNDEFINED;
	roots.descriptor = descriptor;
	roots.made = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_descriptor_trace, &roots);
	if (status != 0)
		return status;

	/* The operation's realm supplies the new object's ordinary prototype. */
	made = vm_object_create(realm->heap, realm->object_prototype);
	if (made == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The newborn must remain explicitly owned during every output-field allocation. */
	roots.made = vm_value_cell(made);

	/* A present value retains its identity rather than being cloned. */
	if ((descriptor->has & VM_HAS_VALUE) != 0U) {
		status = access_descriptor_put(realm, made, "value", descriptor->value);
		if (status != 0)
			goto cleanup;
	}

	/* Writable is omitted unless the input descriptor actually supplied that field. */
	if ((descriptor->has & VM_HAS_WRITABLE) != 0U) {
		truth = 0;
		if ((descriptor->attributes & VM_PROPERTY_WRITABLE) != 0U)
			truth = 1;
		status = access_descriptor_put(realm, made, "writable", vm_value_boolean(truth));
		if (status != 0)
			goto cleanup;
	}

	/* Accessor slots remain ordinary data fields on the representation object. */
	if ((descriptor->has & VM_HAS_GET) != 0U) {
		status = access_descriptor_put(realm, made, "get", descriptor->getter);
		if (status != 0)
			goto cleanup;
	}

	/* Setter presence is independent of the getter field. */
	if ((descriptor->has & VM_HAS_SET) != 0U) {
		status = access_descriptor_put(realm, made, "set", descriptor->setter);
		if (status != 0)
			goto cleanup;
	}

	/* Enumerable presence is independent of its Boolean value. */
	if ((descriptor->has & VM_HAS_ENUMERABLE) != 0U) {
		truth = 0;
		if ((descriptor->attributes & VM_PROPERTY_ENUMERABLE) != 0U)
			truth = 1;
		status = access_descriptor_put(realm, made, "enumerable", vm_value_boolean(truth));
		if (status != 0)
			goto cleanup;
	}

	/* Configurable is likewise emitted only when present, in the standard final position. */
	if ((descriptor->has & VM_HAS_CONFIGURABLE) != 0U) {
		truth = 0;
		if ((descriptor->attributes & VM_PROPERTY_CONFIGURABLE) != 0U)
			truth = 1;
		status = access_descriptor_put(realm, made, "configurable", vm_value_boolean(truth));
		if (status != 0)
			goto cleanup;
	}

	/* Publish the complete representation after all of its ordinary fields succeed. */
	*result = roots.made;
	status = 0;

cleanup:
	/* Neither the descriptor pointer nor the newborn object may escape this tracer's C frame. */
	vm_heap_remove_tracer(realm->heap, access_descriptor_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: the exact partial descriptor is represented by ordinary data properties. */
	return 0;
}

/*
 * Completes a valid partial descriptor with explicit defaults for its property kind.
 * The operation runs no script and performs no allocation.
 */
void
vm_complete_property_descriptor(
	struct vm_descriptor *descriptor)
{
	int accessor;

	/* Presence of either accessor slot chooses accessor completion; generic descriptors become data descriptors. */
	accessor = 0;
	if ((descriptor->has & (VM_HAS_GET | VM_HAS_SET)) != 0U)
		accessor = 1;
	if (accessor) {
		/* Missing accessor slots become explicit undefined payloads. */
		if ((descriptor->has & VM_HAS_GET) == 0U) {
			descriptor->getter = VM_VALUE_UNDEFINED;
			descriptor->has |= VM_HAS_GET;
		}

		/* Completion makes an omitted setter an explicit undefined slot. */
		if ((descriptor->has & VM_HAS_SET) == 0U) {
			descriptor->setter = VM_VALUE_UNDEFINED;
			descriptor->has |= VM_HAS_SET;
		}
	} else {
		/* Data completion supplies an undefined value and a false writable flag when omitted. */
		if ((descriptor->has & VM_HAS_VALUE) == 0U) {
			descriptor->value = VM_VALUE_UNDEFINED;
			descriptor->has |= VM_HAS_VALUE;
		}

		/* A missing writable field becomes present with its false default. */
		if ((descriptor->has & VM_HAS_WRITABLE) == 0U) {
			descriptor->attributes &= ~VM_PROPERTY_WRITABLE;
			descriptor->has |= VM_HAS_WRITABLE;
		}
	}

	/* Omitted enumerable and configurable fields become present false attributes. */
	if ((descriptor->has & VM_HAS_ENUMERABLE) == 0U) {
		descriptor->attributes &= ~VM_PROPERTY_ENUMERABLE;
		descriptor->has |= VM_HAS_ENUMERABLE;
	}

	/* Configurable likewise becomes explicitly false when absent. */
	if ((descriptor->has & VM_HAS_CONFIGURABLE) == 0U) {
		descriptor->attributes &= ~VM_PROPERTY_CONFIGURABLE;
		descriptor->has |= VM_HAS_CONFIGURABLE;
	}

	/* Succeeded: the descriptor has every field required by its property kind. */
	return;
}

/* Marks what a for-in state holds: its object and its keys. */
static void
access_for_in_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct access_for_in *state;

	/* Current prototypes and initialized native key entries remain owned between interpreter steps. */
	state = (struct access_for_in *)cell;
	vm_heap_mark_value(heap, state->object);
	access_keys_trace(heap, &state->keys);
	access_keys_trace(heap, &state->visited);
}

/* Frees a dead for-in state's keys. */
static void
access_for_in_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct access_for_in *state;

	UNUSED_PARAMETER(heap);

	/* Neither native vector outlives the private iterator cell which owns its storage. */
	state = (struct access_for_in *)cell;
	wb_vector_release(&state->keys);
	wb_vector_release(&state->visited);
}

/* Reads a string's own property: its length, or a character by index; found says whether it has the key. */
static int
access_string_property(
	struct vm_realm *realm,
	struct vm_string *string,
	vm_value key,
	vm_value *result,
	int *found)
{
	struct vm_string *character;
	vm_value length_key;
	uint32_t index;
	uint16_t unit;
	int is_index;

	/* The length. */
	*found = 0;
	*result = VM_VALUE_UNDEFINED;
	length_key = vm_key_from_ascii(realm->heap, "length");
	if (length_key == VM_VALUE_EMPTY)
		return ENOMEM;
	if (key == length_key) {
		*result = vm_value_int32((int32_t)string->length);
		*found = 1;
		return 0;
	}

	/* A character is a one-unit string. */
	is_index = vm_value_is_array_index(key, &index);
	if (is_index && index < string->length) {
		unit = vm_string_at(string, index);
		character = vm_string_from_units(realm->heap, &unit, 1);
		if (character == NULL)
			return ENOMEM;
		*result = vm_value_cell(character);
		*found = 1;
	}

	/* Succeeded: the property, or nothing. */
	return 0;
}

/* Refuses an assignment or a deletion: nothing in sloppy code, a TypeError in strict code. */
static int
access_refuse(
	struct vm_realm *realm,
	int strict,
	const char *message)
{
	int status;

	/* Sloppy code ignores it. */
	if (!strict)
		return 0;

	/* Strict code throws. */
	status = vm_throw_type_error(realm, message);
	return status;
}

/* Evaluate instanceof while the public wrapper owns both operands across all property methods. */
static int
access_instanceof(
	struct vm_realm *realm,
	vm_value value,
	vm_value constructor,
	vm_value *result)
{
	vm_value method;
	vm_value answer;
	vm_value builtin;
	int ordinary;
	int truth;
	int callable;
	int is_object;
	int status;

	/* An object's Symbol.hasInstance answers for it, unless it is Function.prototype's own. */
	*result = VM_VALUE_FALSE;
	is_object = vm_value_is_object(constructor);
	if (is_object) {
		status = vm_get_method(realm, constructor, vm_symbol_key(realm, VM_SYMBOL_HAS_INSTANCE), &method);
		if (status != 0)
			return status;
		ordinary = 0;
		if (realm->intrinsics[VM_INTRINSIC_HAS_INSTANCE] != NULL) {
			builtin = vm_value_cell(realm->intrinsics[VM_INTRINSIC_HAS_INSTANCE]);
			if (method == builtin)
				ordinary = 1;
		}

		/* A method of its own is called, its answer made a boolean. */
		if (method != VM_VALUE_UNDEFINED &&
		    !ordinary) {
			status = vm_call(realm, method, constructor, &value, 1, &answer);
			if (status != 0)
				return status;
			truth = vm_to_boolean(answer);
			*result = vm_value_boolean(truth);
			return 0;
		}
	}

	/* Otherwise only a callable right side answers instanceof. */
	callable = vm_value_is_callable(constructor);
	if (!callable) {
		status = vm_throw_type_error(realm, "Right-hand side of 'instanceof' is not callable");
		return status;
	}

	/* The prototype chain answers. */
	status = vm_ordinary_has_instance(realm, constructor, value, result);
	if (status != 0)
		return status;

	/* Succeeded: the answer is stored. */
	return 0;
}

/* Tells whether a key is among the first count keys of a list. */
static int
access_key_listed(
	const struct wb_vector *keys,
	size_t count,
	vm_value key)
{
	vm_value listed;
	size_t index;

	/* Keys are indices, atoms or symbols, which compare by value. */
	for (index = 0; index < count; index++) {
		listed = *(vm_value *)wb_vector_at(keys, index);
		if (listed == key)
			return 1;
	}

	/* Not listed. */
	return 0;
}

/* Tells whether a key is a symbol. */
static int
access_is_symbol(
	vm_value value)
{
	struct vm_cell *cell;
	int is_cell;

	/* Only cells are symbols. */
	is_cell = vm_value_is_cell(value);
	if (!is_cell)
		return 0;

	/* A symbol cell. */
	cell = vm_value_as_cell(value);
	if (cell->type == &vm_symbol_type)
		return 1;

	/* Another cell. */
	return 0;
}

/* Finds the prototype a primitive's properties come from (NULL for undefined and null, and before the built-ins). */
static struct vm_object *
access_primitive_prototype(
	struct vm_realm *realm,
	vm_value value)
{
	int is_boolean;
	int is_number;
	int is_string;
	int is_symbol;

	/* A boolean. */
	is_boolean = vm_value_is_boolean(value);
	if (is_boolean)
		return realm->intrinsics[VM_INTRINSIC_BOOLEAN_PROTOTYPE];

	/* A number. */
	is_number = vm_value_is_number(value);
	if (is_number)
		return realm->intrinsics[VM_INTRINSIC_NUMBER_PROTOTYPE];

	/* A string. */
	is_string = vm_value_is_string(value);
	if (is_string)
		return realm->intrinsics[VM_INTRINSIC_STRING_PROTOTYPE];

	/* A BigInt. */
	if (vm_value_as_bigint(value) != NULL)
		return realm->intrinsics[VM_INTRINSIC_BIGINT_PROTOTYPE];

	/* A symbol. */
	is_symbol = access_is_symbol(value);
	if (is_symbol)
		return realm->intrinsics[VM_INTRINSIC_SYMBOL_PROTOTYPE];

	/* undefined, null. */
	return NULL;
}

/* Assigns a property of a primitive: a setter on its prototype's chain is called with it; anything else is refused. */
static int
access_set_primitive(
	struct vm_realm *realm,
	vm_value base,
	vm_value key,
	vm_value value,
	int strict)
{
	struct vm_property property;
	struct vm_accessor *accessor;
	struct vm_object *prototype;
	vm_value ignored;
	int found;
	int status;

	/* A setter on the prototype's chain. */
	prototype = access_primitive_prototype(realm, base);
	found = 0;
	if (prototype != NULL) {
		found = vm_object_find(prototype, key, &property);
		if (found < 0)
			return -found;
	}

	/* Present descriptors retain their ordinary accessor and attribute rules. */
	if (found && (property.attributes & VM_PROPERTY_ACCESSOR) != 0U) {
		accessor = (struct vm_accessor *)vm_value_as_cell(*property.value);
		if (accessor->setter != VM_VALUE_UNDEFINED) {
			status = vm_call(realm, accessor->setter, base, &value, 1, &ignored);
			return status;
		}
	}

	/* Anything else: the primitive takes no property. */
	status = access_refuse(realm, strict, "Cannot create property on primitive value");
	return status;
}

/* Gives a String wrapper its characters (enumerable, read-only) and its length (read-only). */
static int
access_wrap_string(
	struct vm_realm *realm,
	struct vm_object *wrapper,
	struct vm_string *string)
{
	struct vm_string *character;
	vm_value key;
	uint32_t index;
	uint16_t unit;
	int status;

	/* Each character as an index property. */
	for (index = 0; index < string->length; index++) {
		unit = vm_string_at(string, index);
		character = vm_string_from_units(realm->heap, &unit, 1);
		if (character == NULL)
			return ENOMEM;
		status = vm_object_define(realm->heap, wrapper, vm_value_int32((int32_t)index), vm_value_cell(character),
		    VM_PROPERTY_ENUMERABLE);
		if (status != 0)
			return status;
	}

	/* The length. */
	key = vm_key_from_ascii(realm->heap, "length");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_object_define(realm->heap, wrapper, key, vm_value_int32((int32_t)string->length), 0);
	if (status != 0)
		return status;

	/* Succeeded: the wrapper has the string's properties. */
	return 0;
}

/* Tells whether a descriptor may be applied over what an object has (ValidateAndApplyPropertyDescriptor's refusals). */
static int
access_descriptor_allowed(
	const struct vm_object *object,
	int found,
	const struct vm_descriptor *current,
	const struct vm_descriptor *descriptor)
{
	int extensible;
	int allowed;

	/* Ordinary validation reads extensibility at the original immediate call site. */
	extensible = 1;
	if ((object->flags & VM_OBJECT_NOT_EXTENSIBLE) != 0U)
		extensible = 0;
	allowed = access_descriptor_compatible(extensible, found, current, descriptor);

	/* Succeeded: the pure compatibility rule supplies the unchanged ordinary decision. */
	return allowed;
}

/* Tests compatibility against a complete target descriptor and an explicit extensibility snapshot. */
static int
access_descriptor_compatible(
	int extensible,
	int found,
	const struct vm_descriptor *current,
	const struct vm_descriptor *descriptor)
{
	int is_accessor;
	int was_accessor;
	int generic;
	int same;

	/* A new property needs an object that takes new ones. */
	if (!found) {
		if (!extensible)
			return 0;
		return 1;
	}

	/* A configurable property may become anything. */
	if ((current->attributes & VM_PROPERTY_CONFIGURABLE) != 0U)
		return 1;

	/* A property that is not configurable stays so and keeps its enumerability. */
	if ((descriptor->has & VM_HAS_CONFIGURABLE) != 0U && (descriptor->attributes & VM_PROPERTY_CONFIGURABLE) != 0U)
		return 0;
	if ((descriptor->has & VM_HAS_ENUMERABLE) != 0U &&
	    (descriptor->attributes & VM_PROPERTY_ENUMERABLE) != (current->attributes & VM_PROPERTY_ENUMERABLE))
		return 0;

	/* It keeps its kind. */
	is_accessor = 0;
	if ((descriptor->has & (VM_HAS_GET | VM_HAS_SET)) != 0U)
		is_accessor = 1;
	generic = 0;
	if ((descriptor->has & (VM_HAS_VALUE | VM_HAS_WRITABLE | VM_HAS_GET | VM_HAS_SET)) == 0U)
		generic = 1;
	was_accessor = 0;
	if ((current->has & VM_HAS_GET) != 0U)
		was_accessor = 1;
	if (!generic && is_accessor != was_accessor)
		return 0;

	/* An accessor keeps its getter and setter. */
	if (was_accessor) {
		if ((descriptor->has & VM_HAS_GET) != 0U) {
			same = vm_same_value(descriptor->getter, current->getter);
			if (!same)
				return 0;
		}

		/* The setter too. */
		if ((descriptor->has & VM_HAS_SET) != 0U) {
			same = vm_same_value(descriptor->setter, current->setter);
			if (!same)
				return 0;
		}

		/* It keeps them. */
		return 1;
	}

	/* A read-only data property stays read-only with its value. */
	if ((current->attributes & VM_PROPERTY_WRITABLE) == 0U) {
		if ((descriptor->has & VM_HAS_WRITABLE) != 0U && (descriptor->attributes & VM_PROPERTY_WRITABLE) != 0U)
			return 0;
		if ((descriptor->has & VM_HAS_VALUE) != 0U) {
			same = vm_same_value(descriptor->value, current->value);
			if (!same)
				return 0;
		}
	}

	/* Allowed. */
	return 1;
}

/* Defines an array's length (ArraySetLength): a valid length, the elements past it removed, and its writability. */
static int
access_define_length(
	struct vm_realm *realm,
	struct vm_object *array,
	const struct vm_descriptor *current,
	const struct vm_descriptor *descriptor,
	int *done)
{
	struct vm_descriptor rest;
	struct vm_descriptor latest;
	vm_value length_key;
	double number;
	uint32_t length;
	int refused;
	int found;
	int allowed;
	int status;

	/* The new length, when given: a whole number in uint32's range. */
	*done = 0;
	length_key = vm_key_from_ascii(realm->heap, "length");
	if (length_key == VM_VALUE_EMPTY)
		return ENOMEM;
	rest = *descriptor;
	length = array->length;
	if ((descriptor->has & VM_HAS_VALUE) != 0U) {
		status = vm_to_uint32(realm, descriptor->value, &length);
		if (status != 0)
			return status;
		status = vm_to_number(realm, descriptor->value, &number);
		if (status != 0)
			return status;
		if ((double)length != number) {
			status = vm_throw_range_error(realm, "Invalid array length");
			return status;
		}

		/* The value as the number it is. */
		rest.value = vm_value_number((double)length);
	}

	/* Conversion may change the array or make its length read-only; inspect the actual post-conversion descriptor. */
	found = vm_get_own_descriptor(array, length_key, &latest);
	if (found < 0)
		return -found;
	if (!found)
		return EINVAL;
	current = &latest;

	/* The change must be allowed as for any property, and a read-only length cannot change. */
	allowed = access_descriptor_allowed(array, 1, current, &rest);
	if (!allowed)
		return 0;
	if ((current->attributes & VM_PROPERTY_WRITABLE) == 0U && length != array->length)
		return 0;

	/* A partial shrink preserves higher deletions and reports Boolean refusal after applying requested writability. */
	refused = 0;
	if (length != array->length) {
		status = vm_array_set_length(realm->heap, array, length);
		if (status == EPERM)
			refused = 1;
		else if (status != 0)
			return status;
	}

	/* A length made read-only stays so. */
	if ((descriptor->has & VM_HAS_WRITABLE) != 0U && (descriptor->attributes & VM_PROPERTY_WRITABLE) == 0U) {
		status = vm_object_define(realm->heap, array, length_key, vm_value_number((double)array->length), 0);
		if (status != 0)
			return status;
	}

	/* A fixed element prevented the complete shrink even though requested read-only metadata was applied. */
	if (refused)
		return 0;

	/* Succeeded: the length is defined. */
	*done = 1;
	return 0;
}

/* Reports the attributes of an array's length property. */
static void
access_get_length_attributes(
	struct vm_object *array,
	vm_value length_key,
	uint32_t *attributes)
{
	struct vm_property property;
	int found;

	/* The property (an array always has it). */
	*attributes = VM_PROPERTY_WRITABLE;
	found = vm_object_get_own_ordinary(array, length_key, &property);
	if (found)
		*attributes = property.attributes;
}

/* Throws the TypeError of a property read or write on undefined or null, naming the property as engines do. */
static int
access_base_error(
	struct vm_realm *realm,
	vm_value base,
	vm_value key,
	const char *verb,
	const char *gerund)
{
	struct wb_buffer name;
	char text[300];
	const char *what;
	int status;

	what = base == VM_VALUE_NULL ? "null" : "undefined";
	wb_buffer_init(&name);
	if (vm_value_is_string(key)) {
		(void)vm_string_to_utf8((const struct vm_string *)vm_value_as_cell(key), &name);
	} else if (vm_value_is_int32(key)) {
		(void)wb_buffer_printf(&name, "%d", (int)vm_value_as_int32(key));
	}
	if (name.length != 0)
		snprintf(text, sizeof(text), "Cannot %s properties of %s (%s '%.200s')", verb, what, gerund, wb_buffer_string(&name));
	else
		snprintf(text, sizeof(text), "Cannot %s properties of %s", verb, what);
	wb_buffer_release(&name);
	status = vm_throw_type_error(realm, text);
	return status;
}

/* Looks up a value using the receiver established by the original access operation. */
static int
access_get_receiver(
	struct vm_realm *realm,
	vm_value base,
	vm_value key,
	vm_value receiver,
	vm_value *result)
{
	struct vm_property property;
	struct vm_accessor *accessor;
	struct vm_object *holder;
	int is_object;
	int is_string;
	int found;
	int status;

	/* undefined and null have no properties. */
	*result = VM_VALUE_UNDEFINED;
	if (base == VM_VALUE_UNDEFINED || base == VM_VALUE_NULL) {
		status = access_base_error(realm, base, key, "read", "reading");
		return status;
	}

	/* A string's own length and characters. */
	is_string = vm_value_is_string(base);
	if (is_string) {
		status = access_string_property(realm, (struct vm_string *)vm_value_as_cell(base), key, result, &found);
		if (status != 0)
			return status;
		if (found)
			return 0;
	}

	/* An object's chain, or a primitive's prototype's (the realm has none before its built-ins). */
	is_object = vm_value_is_object(base);
	holder = NULL;
	if (is_object) {
		holder = (struct vm_object *)vm_value_as_cell(base);
	} else {
		holder = access_primitive_prototype(realm, base);
	}

	/* Nothing to look in. */
	if (holder == NULL)
		return 0;

	/* Walk ordinary descriptors until a realm-aware Proxy needs its actual Get operation. */
	found = 0;
	while (holder != NULL) {
		if (holder->kind == VM_KIND_PROXY) {
			status = access_proxy_get(realm, (struct vm_proxy *)holder, key, receiver, result);
			if (status != 0)
				return status;

			/* Succeeded: a Proxy owns the rest of the property lookup. */
			return 0;
		}

		/* Native ordinary descriptors cannot execute user script. */
		found = vm_object_get_own(holder, key, &property);
		if (found < 0)
			return -found;
		if (found)
			break;
		holder = holder->prototype;
	}

	/* A missing descriptor follows the absence path after errors have been excluded. */
	if (!found)
		return 0;

	/* A data property's value. */
	if ((property.attributes & VM_PROPERTY_ACCESSOR) == 0U) {
		*result = *property.value;
		return 0;
	}

	/* An accessor's getter, called with the original receiver as this (no getter reads undefined). */
	accessor = (struct vm_accessor *)vm_value_as_cell(*property.value);
	if (accessor->getter == VM_VALUE_UNDEFINED)
		return 0;
	status = vm_call(realm, accessor->getter, receiver, NULL, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the getter's result. */
	return 0;
}

/* Executes Proxy [[Get]] while preserving original protocol values through trap-driven revocation and collection. */
static int
access_proxy_get(
	struct vm_realm *realm,
	struct vm_proxy *proxy,
	vm_value key,
	vm_value receiver,
	vm_value *result)
{
	struct vm_cell *roots[8];
	struct vm_descriptor descriptor;
	struct vm_string *string;
	vm_value target;
	vm_value handler;
	vm_value method;
	vm_value trap_key;
	vm_value args[3];
	vm_value produced;
	vm_value method_key;
	unsigned index;
	unsigned registered;
	int is_cell;
	int is_symbol;
	int found;
	int equal;
	int status;

	/* Snapshot target and handler before any script can revoke the Proxy. */
	target = proxy->target;
	handler = proxy->handler;
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &proxy->object.cell;
	roots[2] = NULL;
	is_cell = vm_value_is_cell(target);
	if (is_cell)
		roots[2] = vm_value_as_cell(target);
	roots[3] = NULL;
	is_cell = vm_value_is_cell(handler);
	if (is_cell)
		roots[3] = vm_value_as_cell(handler);
	roots[4] = NULL;
	is_cell = vm_value_is_cell(receiver);
	if (is_cell)
		roots[4] = vm_value_as_cell(receiver);
	roots[5] = NULL;
	is_cell = vm_value_is_cell(key);
	if (is_cell)
		roots[5] = vm_value_as_cell(key);
	roots[6] = NULL;
	roots[7] = NULL;
	registered = 0;
	for (index = 0; index < 8U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* A revoked Proxy fails before consulting the handler. */
	if (target == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot get a property of a revoked Proxy");
		goto cleanup;
	}

	/* GetMethod accepts null as absence and rejects other noncallable traps. */
	method_key = vm_key_from_ascii(realm->heap, "get");
	if (method_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Resolve the current get trap through ordinary method lookup. */
	status = vm_get_method(realm, handler, method_key, &method);
	if (status != 0)
		goto cleanup;
	if (method == VM_VALUE_UNDEFINED) {
		status = vm_get_receiver(realm, target, key, receiver, result);
		goto cleanup;
	}

	/* A trap getter may have returned a function retained only by this native frame. */
	roots[6] = vm_value_as_cell(method);
	trap_key = key;
	is_symbol = access_is_symbol(key);
	if (!is_symbol) {
		status = vm_to_string(realm, key, &string);
		if (status != 0)
			goto cleanup;
		trap_key = vm_value_cell(string);
		roots[5] = &string->cell;
	}

	/* A property trap receives the actual target, a string or symbol key, and the original receiver. */
	args[0] = target;
	args[1] = trap_key;
	args[2] = receiver;
	status = vm_call(realm, method, handler, args, 3, &produced);
	if (status != 0)
		goto cleanup;
	is_cell = vm_value_is_cell(produced);
	if (is_cell)
		roots[7] = vm_value_as_cell(produced);

	/* Invariants use the original target even if the trap revoked its Proxy. */
	status = vm_get_own_property(realm, (struct vm_object *)vm_value_as_cell(target), key, &descriptor, &found);
	if (status != 0)
		goto cleanup;

	/* Nonconfigurable own descriptors constrain the value a trap may report. */
	if (found && (descriptor.attributes & VM_PROPERTY_CONFIGURABLE) == 0U) {
		if ((descriptor.has & VM_HAS_VALUE) != 0U && (descriptor.attributes & VM_PROPERTY_WRITABLE) == 0U) {
			equal = vm_same_value(produced, descriptor.value);
			if (!equal) {
				status = vm_throw_type_error(realm, "Proxy get trap changed a fixed data property");
				goto cleanup;
			}
		}

		/* A fixed accessor without a getter can expose only undefined. */
		if ((descriptor.has & VM_HAS_GET) != 0U &&
		    descriptor.getter == VM_VALUE_UNDEFINED &&
		    produced != VM_VALUE_UNDEFINED) {
			status = vm_throw_type_error(realm, "Proxy get trap supplied a value for a fixed getterless property");
			goto cleanup;
		}
	}

	/* Publish the trap result only after every fixed-property invariant succeeds. */
	*result = produced;
	status = 0;

cleanup:
	/* Keep no registrations referring to expired C stack slots. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve an abrupt outcome after releasing this frame's ownership. */
	if (status != 0)
		return status;

	/* Succeeded: the original receiver's read produced the validated value. */
	return 0;
}

/* Marks one active native property frame while a getter or Proxy trap runs the collector. */
static void
access_read_trace(
	struct vm_heap *heap,
	void *context)
{
	struct access_read_roots *roots;

	/* Managed realms need the same explicit ownership as the input and receiver values. */
	roots = context;
	if (roots->realm->managed)
		vm_heap_mark(heap, &roots->realm->cell);
	vm_heap_mark_value(heap, roots->base);
	vm_heap_mark_value(heap, roots->key);
	vm_heap_mark_value(heap, roots->receiver);
}

/* Walks ordinary own descriptors until a Proxy owns the rest of the HasProperty operation. */
static int
access_has_property(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	int *found)
{
	struct vm_property property;
	int present;
	int status;

	/* Presence never invokes an ordinary accessor's getter. */
	*found = 0;
	while (object != NULL) {
		if (object->kind == VM_KIND_PROXY) {
			status = access_proxy_has(realm, (struct vm_proxy *)object, key, found);
			if (status != 0)
				return status;

			/* Succeeded: the Proxy supplies the remaining chain's answer. */
			return 0;
		}

		/* Ordinary native hooks report descriptor presence without running script. */
		present = vm_object_get_own(object, key, &property);
		if (present < 0)
			return -present;
		if (present) {
			*found = 1;
			return 0;
		}

		/* A missing own descriptor continues on the ordinary prototype chain. */
		object = object->prototype;
	}

	/* Succeeded: no object on the chain supplies this property. */
	return 0;
}

/* Executes Proxy [[HasProperty]] without allowing traps to hide fixed or nonextensible own properties. */
static int
access_proxy_has(
	struct vm_realm *realm,
	struct vm_proxy *proxy,
	vm_value key,
	int *found)
{
	struct vm_cell *roots[6];
	struct vm_descriptor descriptor;
	struct vm_object *object;
	struct vm_string *string;
	vm_value target;
	vm_value handler;
	vm_value method;
	vm_value method_key;
	vm_value trap_key;
	vm_value args[2];
	vm_value produced;
	unsigned index;
	unsigned registered;
	int is_cell;
	int is_symbol;
	int present;
	int extensible;
	int truth;
	int status;

	/* Revocation during method lookup cannot retire the operation's original protocol participants. */
	*found = 0;
	target = proxy->target;
	handler = proxy->handler;
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &proxy->object.cell;
	roots[2] = NULL;
	is_cell = vm_value_is_cell(target);
	if (is_cell)
		roots[2] = vm_value_as_cell(target);
	roots[3] = NULL;
	is_cell = vm_value_is_cell(handler);
	if (is_cell)
		roots[3] = vm_value_as_cell(handler);
	roots[4] = NULL;
	is_cell = vm_value_is_cell(key);
	if (is_cell)
		roots[4] = vm_value_as_cell(key);
	roots[5] = NULL;
	registered = 0;
	for (index = 0; index < 6U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* A revoked Proxy must fail before any handler access. */
	if (target == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot search a revoked Proxy");
		goto cleanup;
	}

	/* Fetch the current has trap using the ordinary GetMethod rules. */
	method_key = vm_key_from_ascii(realm->heap, "has");
	if (method_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Read the current has method before choosing trap or target fallback. */
	status = vm_get_method(realm, handler, method_key, &method);
	if (status != 0)
		goto cleanup;
	if (method == VM_VALUE_UNDEFINED) {
		status = vm_has_property(realm, (struct vm_object *)vm_value_as_cell(target), key, found);
		goto cleanup;
	}

	/* The trap may be a fresh function returned only to this native frame. */
	roots[5] = vm_value_as_cell(method);
	trap_key = key;
	is_symbol = access_is_symbol(key);
	if (!is_symbol) {
		status = vm_to_string(realm, key, &string);
		if (status != 0)
			goto cleanup;
		trap_key = vm_value_cell(string);
		roots[4] = &string->cell;
	}

	/* Has receives exactly the actual target and a string-or-symbol property key. */
	args[0] = target;
	args[1] = trap_key;
	status = vm_call(realm, method, handler, args, 2, &produced);
	if (status != 0)
		goto cleanup;
	truth = vm_to_boolean(produced);
	if (truth) {
		*found = 1;
		status = 0;
		goto cleanup;
	}

	/* A false result inspects the original target after the trap's changes. */
	object = (struct vm_object *)vm_value_as_cell(target);
	status = vm_get_own_property(realm, object, key, &descriptor, &present);
	if (status != 0)
		goto cleanup;

	/* Only an existing own property constrains a false has result. */
	if (present) {
		if ((descriptor.attributes & VM_PROPERTY_CONFIGURABLE) == 0U) {
			status = vm_throw_type_error(realm, "Proxy has trap hid a fixed own property");
			goto cleanup;
		}

		/* A nonextensible target also keeps its configurable own properties visible. */
		status = vm_is_extensible(realm, object, &extensible);
		if (status != 0)
			goto cleanup;
		if (!extensible) {
			status = vm_throw_type_error(realm, "Proxy has trap hid a nonextensible target's own property");
			goto cleanup;
		}
	}

	/* The missing or legally hidden property produces an ordinary false result. */
	status = 0;

cleanup:
	/* Unregister only slots acquired before an allocation or trap failure. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Propagate the original failure after releasing this native frame's roots. */
	if (status != 0)
		return status;

	/* Succeeded: the reported presence respects the target's actual invariants. */
	return 0;
}

/* Reads one present descriptor field through the real HasProperty and Get operations. */
static int
access_descriptor_field(
	struct vm_realm *realm,
	vm_value object,
	const char *name,
	uint32_t flag,
	struct vm_descriptor *descriptor,
	vm_value *value)
{
	vm_value key;
	vm_value has;
	int status;

	/* Whether it has the field. */
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_in(realm, key, object, &has);
	if (status != 0)
		return status;
	if (has != VM_VALUE_TRUE)
		return 0;

	/* The field's value. */
	status = vm_get(realm, object, key, value);
	if (status != 0)
		return status;

	/* Succeeded: the descriptor has it. */
	descriptor->has |= flag;
	return 0;
}

/* Defines one ordinary data field while the surrounding descriptor frame owns both object and value. */
static int
access_descriptor_put(
	struct vm_realm *realm,
	struct vm_object *object,
	const char *name,
	vm_value value)
{
	vm_value key;
	int status;

	/* Representation field names are ordinary interned string keys. */
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_object_define(realm->heap, object, key, value, VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* Succeeded: the output field is writable, enumerable and configurable. */
	return 0;
}

/* Marks only descriptor payloads whose presence flags make their C slots meaningful. */
static void
access_descriptor_trace(
	struct vm_heap *heap,
	void *context)
{
	struct access_descriptor_roots *roots;
	const struct vm_descriptor *descriptor;

	/* The managed owner, input object and unpublished result belong to this conversion frame. */
	roots = context;
	if (roots->realm->managed)
		vm_heap_mark(heap, &roots->realm->cell);
	vm_heap_mark_value(heap, roots->source);
	vm_heap_mark_value(heap, roots->made);

	/* Undefined descriptors have no payload storage to inspect. */
	descriptor = roots->descriptor;
	if (descriptor == NULL)
		return;
	if ((descriptor->has & VM_HAS_VALUE) != 0U)
		vm_heap_mark_value(heap, descriptor->value);
	if ((descriptor->has & VM_HAS_GET) != 0U)
		vm_heap_mark_value(heap, descriptor->getter);
	if ((descriptor->has & VM_HAS_SET) != 0U)
		vm_heap_mark_value(heap, descriptor->setter);

	/* Succeeded: absent descriptor slots were never read as values. */
	return;
}

/* Executes Proxy own-descriptor lookup with snapshots taken before descriptor conversion can mutate the target. */
static int
access_proxy_get_own(
	struct vm_realm *realm,
	struct vm_proxy *proxy,
	vm_value key,
	struct vm_descriptor *descriptor,
	int *found)
{
	struct vm_cell *roots[8];
	struct access_descriptor_roots current_roots;
	struct access_descriptor_roots result_roots;
	struct vm_descriptor current;
	struct vm_object *target_object;
	struct vm_string *string;
	vm_value target;
	vm_value handler;
	vm_value method;
	vm_value method_key;
	vm_value trap_key;
	vm_value produced;
	vm_value args[2];
	unsigned index;
	unsigned registered;
	int current_registered;
	int result_registered;
	int is_cell;
	int is_object;
	int is_symbol;
	int present;
	int extensible;
	int allowed;
	int status;

	/* Absent target descriptors have no payload slots for the snapshot tracer to inspect. */
	memset(&current, 0, sizeof(current));

	/* Preserve the original target and handler even if a getter or trap revokes the record. */
	target = proxy->target;
	handler = proxy->handler;
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &proxy->object.cell;
	roots[2] = NULL;
	is_cell = vm_value_is_cell(target);
	if (is_cell)
		roots[2] = vm_value_as_cell(target);
	roots[3] = NULL;
	is_cell = vm_value_is_cell(handler);
	if (is_cell)
		roots[3] = vm_value_as_cell(handler);
	roots[4] = NULL;
	is_cell = vm_value_is_cell(key);
	if (is_cell)
		roots[4] = vm_value_as_cell(key);
	roots[5] = NULL;
	roots[6] = NULL;
	roots[7] = NULL;
	registered = 0;
	current_registered = 0;
	result_registered = 0;
	for (index = 0; index < 8U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* A revoked record cannot consult any handler property. */
	if (target == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot get a descriptor of a revoked Proxy");
		goto cleanup;
	}

	/* Resolve the dynamic descriptor trap through ordinary GetMethod semantics. */
	method_key = vm_key_from_ascii(realm->heap, "getOwnPropertyDescriptor");
	if (method_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* A missing or null method delegates to the original target's own operation. */
	status = vm_get_method(realm, handler, method_key, &method);
	if (status != 0)
		goto cleanup;
	if (method == VM_VALUE_UNDEFINED) {
		status = vm_get_own_property(realm, (struct vm_object *)vm_value_as_cell(target), key, descriptor, found);
		goto cleanup;
	}

	/* Retain a fresh callable trap and turn internal integer indices into script-visible strings. */
	roots[5] = vm_value_as_cell(method);
	trap_key = key;
	is_symbol = access_is_symbol(key);
	if (!is_symbol) {
		status = vm_to_string(realm, key, &string);
		if (status != 0)
			goto cleanup;
		trap_key = vm_value_cell(string);
		roots[6] = &string->cell;
	}

	/* The trap receives only the original target and normalized property key with handler as this. */
	args[0] = target;
	args[1] = trap_key;
	status = vm_call(realm, method, handler, args, 2, &produced);
	if (status != 0)
		goto cleanup;
	is_cell = vm_value_is_cell(produced);
	if (is_cell)
		roots[7] = vm_value_as_cell(produced);
	is_object = vm_value_is_object(produced);
	if (!is_object && produced != VM_VALUE_UNDEFINED) {
		status = vm_throw_type_error(realm, "Proxy descriptor trap must return an object or undefined");
		goto cleanup;
	}

	/* Target observation precedes all getters on the returned descriptor object. */
	target_object = (struct vm_object *)vm_value_as_cell(target);
	status = vm_get_own_property(realm, target_object, key, &current, &present);
	if (status != 0)
		goto cleanup;
	/* Target extent callbacks may revoke or mutate its graph, so retain all flagged snapshot payloads first. */
	current_roots.realm = realm;
	current_roots.source = VM_VALUE_UNDEFINED;
	current_roots.descriptor = &current;
	current_roots.made = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_descriptor_trace, &current_roots);
	if (status != 0)
		goto cleanup;
	current_registered = 1;
	/* Undefined output skips extent observation when the target property is absent or already fixed. */
	if (produced == VM_VALUE_UNDEFINED) {
		if (!present) {
			status = 0;
			goto cleanup;
		}

		/* A fixed own property cannot be hidden, regardless of the target's extensibility. */
		if ((current.attributes & VM_PROPERTY_CONFIGURABLE) == 0U) {
			status = vm_throw_type_error(realm, "Proxy descriptor trap cannot hide this own property");
			goto cleanup;
		}

		/* Only a configurable present property requires actual target extensibility on the absence path. */
		status = vm_is_extensible(realm, target_object, &extensible);
		if (status != 0)
			goto cleanup;
		if (!extensible) {
			status = vm_throw_type_error(realm, "Proxy descriptor trap cannot hide this own property");
			goto cleanup;
		}

		/* The observed configurable property may be hidden on an extensible target. */
		status = 0;
		goto cleanup;
	}

	/* Nonundefined output observes target extensibility before any returned descriptor getter executes. */
	status = vm_is_extensible(realm, target_object, &extensible);
	if (status != 0)
		goto cleanup;

	/* The original trap output and every converted field survive descriptor getters independently. */
	result_roots.realm = realm;
	result_roots.source = produced;
	result_roots.descriptor = descriptor;
	result_roots.made = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_descriptor_trace, &result_roots);
	if (status != 0)
		goto cleanup;
	result_registered = 1;
	status = vm_to_property_descriptor(realm, produced, descriptor);
	if (status != 0)
		goto cleanup;
	vm_complete_property_descriptor(descriptor);

	/* Compatibility uses the pre-conversion extensibility and target descriptor snapshots. */
	allowed = access_descriptor_compatible(extensible, present, &current, descriptor);
	if (!allowed) {
		status = vm_throw_type_error(realm, "Proxy descriptor trap returned an incompatible descriptor");
		goto cleanup;
	}

	/* Nonconfigurable claims must already be true of the target's observed own property. */
	if ((descriptor->attributes & VM_PROPERTY_CONFIGURABLE) == 0U) {
		if (!present || (current.attributes & VM_PROPERTY_CONFIGURABLE) != 0U) {
			status = vm_throw_type_error(realm, "Proxy descriptor trap invented a nonconfigurable property");
			goto cleanup;
		}

		/* A still-writable fixed property cannot be reported as newly read-only. */
		if ((descriptor->has & VM_HAS_WRITABLE) != 0U && (descriptor->attributes & VM_PROPERTY_WRITABLE) == 0U) {
			if ((current.attributes & VM_PROPERTY_WRITABLE) != 0U) {
				status = vm_throw_type_error(realm, "Proxy descriptor trap invented a fixed read-only property");
				goto cleanup;
			}
		}
	}

	/* Publish presence only after conversion, completion and every invariant have passed. */
	*found = 1;
	status = 0;

cleanup:
	/* Retire snapshot and output tracers before the stack descriptor storage leaves this frame. */
	if (result_registered)
		vm_heap_remove_tracer(realm->heap, access_descriptor_trace, &result_roots);
	if (current_registered)
		vm_heap_remove_tracer(realm->heap, access_descriptor_trace, &current_roots);
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve errors without publishing a partially checked descriptor. */
	if (status != 0)
		return status;

	/* Succeeded: absence or the fully checked descriptor is available to the caller. */
	return 0;
}

/* Executes Proxy definition using the original partial descriptor even when the trap mutates its representation. */
static int
access_proxy_define(
	struct vm_realm *realm,
	struct vm_proxy *proxy,
	vm_value key,
	const struct vm_descriptor *descriptor,
	int *done)
{
	struct vm_cell *roots[8];
	struct access_descriptor_roots input_roots;
	struct access_descriptor_roots current_roots;
	struct vm_descriptor current;
	struct vm_object *target_object;
	struct vm_string *string;
	vm_value target;
	vm_value handler;
	vm_value method;
	vm_value method_key;
	vm_value trap_key;
	vm_value representation;
	vm_value produced;
	vm_value args[3];
	unsigned index;
	unsigned registered;
	int input_registered;
	int current_registered;
	int depth_owned;
	int is_cell;
	int is_symbol;
	int truth;
	int present;
	int extensible;
	int setting_fixed;
	int allowed;
	int status;

	/* No absent target descriptor exposes uninitialized payloads to its eventual snapshot tracer. */
	memset(&current, 0, sizeof(current));

	/* Snapshot protocol participants and clear the Boolean output before any observable method read. */
	*done = 0;
	target = proxy->target;
	handler = proxy->handler;
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &proxy->object.cell;
	roots[2] = NULL;
	is_cell = vm_value_is_cell(target);
	if (is_cell)
		roots[2] = vm_value_as_cell(target);
	roots[3] = NULL;
	is_cell = vm_value_is_cell(handler);
	if (is_cell)
		roots[3] = vm_value_as_cell(handler);
	roots[4] = NULL;
	is_cell = vm_value_is_cell(key);
	if (is_cell)
		roots[4] = vm_value_as_cell(key);
	roots[5] = NULL;
	roots[6] = NULL;
	roots[7] = NULL;
	registered = 0;
	input_registered = 0;
	current_registered = 0;
	depth_owned = 0;
	for (index = 0; index < 8U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Retain flagged original input values independently of the separately mutable trap argument object. */
	input_roots.realm = realm;
	input_roots.source = VM_VALUE_UNDEFINED;
	input_roots.descriptor = descriptor;
	input_roots.made = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_descriptor_trace, &input_roots);
	if (status != 0)
		goto cleanup;
	input_registered = 1;
	if (realm->property_depth >= ACCESS_RECURSION_MAX) {
		status = vm_throw_range_error(realm, "Maximum property recursion exceeded");
		goto cleanup;
	}

	/* This frame owns one property depth until its normal or abrupt completion. */
	realm->property_depth++;
	depth_owned = 1;
	if (target == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot define a property of a revoked Proxy");
		goto cleanup;
	}

	/* GetMethod observes the current define trap and treats null as absence. */
	method_key = vm_key_from_ascii(realm->heap, "defineProperty");
	if (method_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Missing traps delegate the unchanged partial descriptor to the original target. */
	status = vm_get_method(realm, handler, method_key, &method);
	if (status != 0)
		goto cleanup;
	if (method == VM_VALUE_UNDEFINED) {
		status = vm_define_own_property(realm, (struct vm_object *)vm_value_as_cell(target), key, descriptor, done);
		goto cleanup;
	}

	/* Retain a fresh method and normalize internal indices without altering symbol identity. */
	roots[5] = vm_value_as_cell(method);
	trap_key = key;
	is_symbol = access_is_symbol(key);
	if (!is_symbol) {
		status = vm_to_string(realm, key, &string);
		if (status != 0)
			goto cleanup;
		trap_key = vm_value_cell(string);
		roots[6] = &string->cell;
	}

	/* A trap sees exactly the present descriptor fields as ordinary writable data properties. */
	status = vm_from_property_descriptor(realm, descriptor, &representation);
	if (status != 0)
		goto cleanup;
	roots[7] = vm_value_as_cell(representation);
	args[0] = target;
	args[1] = trap_key;
	args[2] = representation;
	status = vm_call(realm, method, handler, args, 3, &produced);
	if (status != 0)
		goto cleanup;
	truth = vm_to_boolean(produced);
	if (!truth) {
		status = 0;
		goto cleanup;
	}

	/* Successful traps are constrained by the original target's state after the trap has run. */
	target_object = (struct vm_object *)vm_value_as_cell(target);
	status = vm_get_own_property(realm, target_object, key, &current, &present);
	if (status != 0)
		goto cleanup;

	/* Extent traps may sever the target's last payload owner while compatibility still needs the original descriptor. */
	current_roots.realm = realm;
	current_roots.source = VM_VALUE_UNDEFINED;
	current_roots.descriptor = &current;
	current_roots.made = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_descriptor_trace, &current_roots);
	if (status != 0)
		goto cleanup;
	current_registered = 1;
	status = vm_is_extensible(realm, target_object, &extensible);
	if (status != 0)
		goto cleanup;
	setting_fixed = 0;
	if ((descriptor->has & VM_HAS_CONFIGURABLE) != 0U && (descriptor->attributes & VM_PROPERTY_CONFIGURABLE) == 0U)
		setting_fixed = 1;
	if (!present) {
		if (!extensible || setting_fixed) {
			status = vm_throw_type_error(realm, "Proxy define trap cannot invent this property");
			goto cleanup;
		}
	} else {
		/* Existing properties use the original input fields rather than the trap's mutable object. */
		allowed = access_descriptor_compatible(extensible, present, &current, descriptor);
		if (!allowed) {
			status = vm_throw_type_error(realm, "Proxy define trap accepted an incompatible descriptor");
			goto cleanup;
		}

		/* An explicit fixed claim must already hold on the target after the trap. */
		if (setting_fixed && (current.attributes & VM_PROPERTY_CONFIGURABLE) != 0U) {
			status = vm_throw_type_error(realm, "Proxy define trap invented a nonconfigurable property");
			goto cleanup;
		}

		/* A nonconfigurable data property must already be read-only before a false writable claim succeeds. */
		if ((current.has & VM_HAS_VALUE) != 0U &&
		    (current.attributes & VM_PROPERTY_CONFIGURABLE) == 0U &&
		    (current.attributes & VM_PROPERTY_WRITABLE) != 0U) {
			if ((descriptor->has & VM_HAS_WRITABLE) != 0U && (descriptor->attributes & VM_PROPERTY_WRITABLE) == 0U) {
				status = vm_throw_type_error(realm, "Proxy define trap invented a fixed read-only property");
				goto cleanup;
			}
		}
	}

	/* Report success only after every post-trap invariant agrees with the unchanged input descriptor. */
	*done = 1;
	status = 0;

cleanup:
	/* The original descriptor pointer and depth ownership end with this native frame. */
	if (depth_owned)
		realm->property_depth--;
	if (current_registered)
		vm_heap_remove_tracer(realm->heap, access_descriptor_trace, &current_roots);
	if (input_registered)
		vm_heap_remove_tracer(realm->heap, access_descriptor_trace, &input_roots);
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve errors without changing refusal into successful definition. */
	if (status != 0)
		return status;

	/* Succeeded: done distinguishes a valid truthy trap result from ordinary refusal. */
	return 0;
}

/* Applies ordinary receiver-aware Set without invoking a getter or losing the target descriptor snapshot. */
static int
access_set_receiver(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	vm_value value,
	vm_value receiver,
	int *done)
{
	struct access_descriptor_roots roots;
	struct vm_descriptor current;
	struct vm_descriptor existing;
	struct vm_descriptor change;
	struct vm_object *receiver_object;
	vm_value ignored;
	int present;
	int is_object;
	int status;

	/* A native descriptor may expose a transient accessor that no ordinary heap property owns. */
	memset(&current, 0, sizeof(current));
	roots.realm = realm;
	roots.source = VM_VALUE_UNDEFINED;
	roots.descriptor = &current;
	roots.made = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_descriptor_trace, &roots);
	if (status != 0)
		return status;
	status = vm_get_own_property(realm, object, key, &current, &present);
	if (status != 0)
		goto cleanup;
	if (!present) {
		if (object->prototype != NULL) {
			status = vm_set_receiver(realm, object->prototype, key, value, receiver, done);
			goto cleanup;
		}

		/* At the chain's end, an absent property acts as a writable data property. */
		current.has = VM_HAS_VALUE | VM_HAS_WRITABLE;
		current.attributes = VM_PROPERTY_WRITABLE;
		current.value = VM_VALUE_UNDEFINED;
	}

	/* Data descriptors first decide writability, then whether the actual receiver can have properties. */
	if ((current.has & VM_HAS_VALUE) != 0U) {
		if ((current.attributes & VM_PROPERTY_WRITABLE) == 0U) {
			status = 0;
			goto cleanup;
		}

		/* A primitive receiver cannot accept an ordinary data property. */
		is_object = vm_value_is_object(receiver);
		if (!is_object) {
			status = 0;
			goto cleanup;
		}

		/* Receiver reflection is a real internal operation and can run its own Proxy trap. */
		receiver_object = (struct vm_object *)vm_value_as_cell(receiver);
		status = vm_get_own_property(realm, receiver_object, key, &existing, &present);
		if (status != 0)
			goto cleanup;
		if (present) {
			if ((existing.has & VM_HAS_GET) != 0U || (existing.attributes & VM_PROPERTY_WRITABLE) == 0U) {
				status = 0;
				goto cleanup;
			}
		}

		/* Existing writable data changes only its value; new receiver properties receive all default attributes. */
		memset(&change, 0, sizeof(change));
		change.has = VM_HAS_VALUE;
		change.value = value;
		if (!present) {
			change.has |= VM_HAS_WRITABLE | VM_HAS_ENUMERABLE | VM_HAS_CONFIGURABLE;
			change.attributes = VM_PROPERTY_DEFAULT;
		}

		/* Array receivers and Proxy receivers apply their actual DefineOwnProperty rules. */
		status = vm_define_own_property(realm, receiver_object, key, &change, done);
		goto cleanup;
	}

	/* Getter-only accessors refuse assignment without observing the receiver's descriptor. */
	if (current.setter == VM_VALUE_UNDEFINED) {
		status = 0;
		goto cleanup;
	}

	/* A setter receives exactly one original value and the original receiver; its return value is ignored. */
	status = vm_call(realm, current.setter, receiver, &value, 1, &ignored);
	if (status != 0)
		goto cleanup;
	*done = 1;

cleanup:
	/* Retire the snapshot tracer before its descriptor storage leaves this native frame. */
	vm_heap_remove_tracer(realm->heap, access_descriptor_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: done preserves accepted assignment versus Boolean refusal. */
	return 0;
}

/* Marks the original write value together with the realm, base, key and receiver of its native frame. */
static void
access_write_trace(
	struct vm_heap *heap,
	void *context)
{
	struct access_write_roots *roots;

	/* Reuse the read-frame protocol for the shared input graph. */
	roots = context;
	access_read_trace(heap, &roots->read);
	vm_heap_mark_value(heap, roots->value);

	/* Succeeded: a sole write value remains live even after the script trap has returned. */
	return;
}

/* Executes Proxy Set while keeping original target, handler, value and receiver through revocation and collection. */
static int
access_proxy_set(
	struct vm_realm *realm,
	struct vm_proxy *proxy,
	vm_value key,
	vm_value value,
	vm_value receiver,
	int *done)
{
	struct vm_cell *roots[6];
	struct vm_descriptor current;
	struct vm_string *string;
	vm_value target;
	vm_value handler;
	vm_value method;
	vm_value method_key;
	vm_value trap_key;
	vm_value produced;
	vm_value args[4];
	unsigned index;
	unsigned registered;
	int is_cell;
	int is_symbol;
	int truth;
	int present;
	int same;
	int status;

	/* The surrounding write frame retains key, value and receiver while this frame snapshots protocol participants. */
	target = proxy->target;
	handler = proxy->handler;
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = &proxy->object.cell;
	roots[2] = NULL;
	is_cell = vm_value_is_cell(target);
	if (is_cell)
		roots[2] = vm_value_as_cell(target);
	roots[3] = NULL;
	is_cell = vm_value_is_cell(handler);
	if (is_cell)
		roots[3] = vm_value_as_cell(handler);
	roots[4] = NULL;
	roots[5] = NULL;
	registered = 0;
	for (index = 0; index < 6U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Revocation fails before consulting the handler. */
	if (target == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot set a property of a revoked Proxy");
		goto cleanup;
	}

	/* GetMethod retrieves the current set trap and rejects noncallable values other than null or undefined. */
	method_key = vm_key_from_ascii(realm->heap, "set");
	if (method_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Fallback delegates with the original receiver rather than substituting the target. */
	status = vm_get_method(realm, handler, method_key, &method);
	if (status != 0)
		goto cleanup;
	if (method == VM_VALUE_UNDEFINED) {
		status = vm_set_receiver(realm, (struct vm_object *)vm_value_as_cell(target), key, value, receiver, done);
		goto cleanup;
	}

	/* Retain a fresh trap and preserve symbol identity while exposing internal integer keys as strings. */
	roots[4] = vm_value_as_cell(method);
	trap_key = key;
	is_symbol = access_is_symbol(key);
	if (!is_symbol) {
		status = vm_to_string(realm, key, &string);
		if (status != 0)
			goto cleanup;
		trap_key = vm_value_cell(string);
		roots[5] = &string->cell;
	}

	/* The handler receives the target, normalized key, original value and original receiver as four arguments. */
	args[0] = target;
	args[1] = trap_key;
	args[2] = value;
	args[3] = receiver;
	status = vm_call(realm, method, handler, args, 4, &produced);
	if (status != 0)
		goto cleanup;
	truth = vm_to_boolean(produced);
	if (!truth) {
		status = 0;
		goto cleanup;
	}

	/* Truthy traps must respect the original target's own descriptor as observed after the trap. */
	status = vm_get_own_property(realm, (struct vm_object *)vm_value_as_cell(target), key, &current, &present);
	if (status != 0)
		goto cleanup;
	if (present && (current.attributes & VM_PROPERTY_CONFIGURABLE) == 0U) {
		if ((current.has & VM_HAS_VALUE) != 0U && (current.attributes & VM_PROPERTY_WRITABLE) == 0U) {
			same = vm_same_value(value, current.value);
			if (!same) {
				status = vm_throw_type_error(realm, "Proxy set trap changed a fixed data property");
				goto cleanup;
			}
		}

		/* A fixed accessor without a setter cannot claim successful assignment. */
		if ((current.has & VM_HAS_SET) != 0U && current.setter == VM_VALUE_UNDEFINED) {
			status = vm_throw_type_error(realm, "Proxy set trap accepted a fixed setter-less property");
			goto cleanup;
		}
	}

	/* A truthy result passing the fixed-property invariants accepts the assignment. */
	*done = 1;
	status = 0;

cleanup:
	/* The surrounding write frame remains active until every original protocol snapshot has been retired. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve abrupt completion instead of reporting Boolean success. */
	if (status != 0)
		return status;

	/* Succeeded: done distinguishes accepted assignment from a false trap result. */
	return 0;
}

/* Applies fixed-buffer integer-index Set while distinguishing conversion order for identical and different receivers. */
static int
access_set_typed(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	vm_value value,
	vm_value receiver,
	int *done)
{
	struct vm_cell *converted_root;
	struct vm_property property;
	struct vm_descriptor change;
	vm_value converted;
	double number;
	int canonical;
	int same;
	int present;
	int is_cell;
	int status;

	/* The outer write frame owns original inputs; this slot owns a fresh converted BigInt before native validity reads. */
	converted_root = NULL;
	status = vm_heap_add_root(realm->heap, &converted_root);
	if (status != 0)
		return status;
	status = vm_canonical_numeric_index(key, &number, &canonical);
	if (status != 0)
		goto cleanup;
	if (!canonical) {
		status = access_set_receiver(realm, object, key, value, receiver, done);
		goto cleanup;
	}

	/* A real TypedArray exposes pure integer-index validity through its own native policy. */
	if (object->native_operations == NULL || object->native_operations->get_own == NULL) {
		status = EINVAL;
		goto cleanup;
	}

	/* Identical receivers convert first, even when the numeric index is invalid or out of range. */
	same = vm_same_value(vm_value_cell(object), receiver);
	if (same) {
		converted = VM_VALUE_UNDEFINED;
		if ((object->flags & VM_OBJECT_TYPED_BIGINT) != 0U) {
			status = vm_to_bigint(realm, value, &converted);
			if (status != 0)
				goto cleanup;
		} else {
			status = vm_to_number(realm, value, &number);
			if (status != 0)
				goto cleanup;
			converted = vm_value_number(number);
		}

		/* Preserve the sole converted value through subsequent native collection and metadata observation. */
		is_cell = vm_value_is_cell(converted);
		if (is_cell)
			converted_root = vm_value_as_cell(converted);
		present = object->native_operations->get_own(object, key, &property);
		if (present < 0) {
			status = -present;
			goto cleanup;
		}

		/* An invalid integer index ignores the already converted value and still reports true. */
		if (!present) {
			*done = 1;
			status = 0;
			goto cleanup;
		}

		/* Native Define stores only the normalized primitive; no user conversion can occur inside the callback. */
		memset(&change, 0, sizeof(change));
		change.has = VM_HAS_VALUE;
		change.value = converted;
		status = vm_define_own_property(realm, object, key, &change, done);
		goto cleanup;
	}

	/* A distinct receiver checks integer-index validity before any value conversion. */
	present = object->native_operations->get_own(object, key, &property);
	if (present < 0) {
		status = -present;
		goto cleanup;
	}

	/* Invalid numeric keys accept assignment without consulting or creating a property on the receiver. */
	if (!present) {
		*done = 1;
		status = 0;
		goto cleanup;
	}

	/* Valid indices apply ordinary receiver reflection and definition to the original unconverted value. */
	status = access_set_receiver(realm, object, key, value, receiver, done);

cleanup:
	/* The temporary conversion owner retires before the surrounding write frame. */
	vm_heap_remove_root(realm->heap, &converted_root);
	if (status != 0)
		return status;

	/* Succeeded: done distinguishes ordinary refusal from accepted integer-index assignment. */
	return 0;
}

/* Converts fixed-buffer TypedArray descriptors in the realm before calling pure native storage policy. */
static int
access_define_typed(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	const struct vm_descriptor *descriptor,
	int *handled,
	int *done)
{
	struct access_descriptor_roots input_roots;
	struct access_descriptor_roots converted_roots;
	struct vm_descriptor converted;
	struct vm_property property;
	struct vm_cell *key_root;
	double number;
	int input_registered;
	int converted_registered;
	int canonical;
	int present;
	int native_handled;
	int is_cell;
	int status;

	/* A direct native caller can supply a sole target, key or flagged payload with no embedding roots. */
	*handled = 0;
	*done = 0;
	input_registered = 0;
	converted_registered = 0;
	key_root = NULL;
	is_cell = vm_value_is_cell(key);
	if (is_cell)
		key_root = vm_value_as_cell(key);
	status = vm_heap_add_root(realm->heap, &key_root);
	if (status != 0)
		return status;
	input_roots.realm = realm;
	input_roots.source = vm_value_cell(object);
	input_roots.descriptor = descriptor;
	input_roots.made = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_descriptor_trace, &input_roots);
	if (status != 0)
		goto cleanup;
	input_registered = 1;
	status = vm_canonical_numeric_index(key, &number, &canonical);
	if (status != 0)
		goto cleanup;
	if (!canonical) {
		status = 0;
		goto cleanup;
	}

	/* Canonical numeric keys belong exclusively to integer-index policy, even when missing. */
	*handled = 1;
	if (object->native_operations == NULL ||
	    object->native_operations->get_own == NULL ||
	    object->native_operations->define == NULL) {
		status = EINVAL;
		goto cleanup;
	}

	/* Index validity is observed before flags or user value conversion. */
	present = object->native_operations->get_own(object, key, &property);
	if (present < 0) {
		status = -present;
		goto cleanup;
	}

	/* An initially invalid index refuses without observing the value's primitive conversion. */
	if (!present) {
		status = 0;
		goto cleanup;
	}

	/* Integer-index properties cannot become nonconfigurable or nonenumerable. */
	if ((descriptor->has & VM_HAS_CONFIGURABLE) != 0U && (descriptor->attributes & VM_PROPERTY_CONFIGURABLE) == 0U) {
		status = 0;
		goto cleanup;
	}

	/* Enumerability has the same mandatory true attribute. */
	if ((descriptor->has & VM_HAS_ENUMERABLE) != 0U && (descriptor->attributes & VM_PROPERTY_ENUMERABLE) == 0U) {
		status = 0;
		goto cleanup;
	}

	/* Accessor descriptors are never integer-index data properties. */
	if ((descriptor->has & (VM_HAS_GET | VM_HAS_SET)) != 0U) {
		status = 0;
		goto cleanup;
	}

	/* A writable:false claim is rejected before any value conversion. */
	if ((descriptor->has & VM_HAS_WRITABLE) != 0U && (descriptor->attributes & VM_PROPERTY_WRITABLE) == 0U) {
		status = 0;
		goto cleanup;
	}

	/* An attribute-only descriptor needs no element conversion or native write. */
	if ((descriptor->has & VM_HAS_VALUE) == 0U) {
		*done = 1;
		status = 0;
		goto cleanup;
	}

	/* Copy only present metadata and initialize every payload before exposing the converted descriptor tracer. */
	memset(&converted, 0, sizeof(converted));
	converted.has = descriptor->has;
	converted.value = VM_VALUE_UNDEFINED;
	converted.getter = VM_VALUE_UNDEFINED;
	converted.setter = VM_VALUE_UNDEFINED;
	if ((descriptor->has & VM_HAS_WRITABLE) != 0U)
		converted.attributes |= descriptor->attributes & VM_PROPERTY_WRITABLE;
	if ((descriptor->has & VM_HAS_ENUMERABLE) != 0U)
		converted.attributes |= descriptor->attributes & VM_PROPERTY_ENUMERABLE;
	if ((descriptor->has & VM_HAS_CONFIGURABLE) != 0U)
		converted.attributes |= descriptor->attributes & VM_PROPERTY_CONFIGURABLE;
	converted_roots.realm = realm;
	converted_roots.source = VM_VALUE_UNDEFINED;
	converted_roots.descriptor = &converted;
	converted_roots.made = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_descriptor_trace, &converted_roots);
	if (status != 0)
		goto cleanup;
	converted_registered = 1;
	if ((object->flags & VM_OBJECT_TYPED_BIGINT) != 0U) {
		status = vm_to_bigint(realm, descriptor->value, &converted.value);
		if (status != 0)
			goto cleanup;
	} else {
		status = vm_to_number(realm, descriptor->value, &number);
		if (status != 0)
			goto cleanup;
		converted.value = vm_value_number(number);
	}

	/* Heap policy receives NULL realm and normalized primitives, so it cannot execute user conversion. */
	native_handled = 0;
	status = object->native_operations->define(NULL, object, key, &converted, &native_handled, done);
	if (status != 0)
		goto cleanup;
	if (!native_handled) {
		status = EINVAL;
		goto cleanup;
	}

	/* A conversion-time invalidation makes SetElement a no-op after the original valid Define was accepted. */
	*done = 1;
	status = 0;

cleanup:
	/* Retire converted and original descriptor owners before their stack storage leaves this operation. */
	if (converted_registered)
		vm_heap_remove_tracer(realm->heap, access_descriptor_trace, &converted_roots);
	if (input_registered)
		vm_heap_remove_tracer(realm->heap, access_descriptor_trace, &input_roots);
	vm_heap_remove_root(realm->heap, &key_root);
	if (status != 0)
		return status;

	/* Succeeded: handled distinguishes integer-index policy from ordinary fallback and done preserves refusal. */
	return 0;
}

/* Owns a realm and input across extensibility traps and script-free native policy collections. */
static int
access_extensibility(
	struct vm_realm *realm,
	struct vm_object *object,
	int prevent,
	int *result)
{
	struct access_read_roots roots;
	int status;

	/* Even a recursion-limit error may allocate while this frame is the only owner. */
	*result = 0;
	roots.realm = realm;
	roots.base = vm_value_cell(object);
	roots.key = VM_VALUE_UNDEFINED;
	roots.receiver = VM_VALUE_UNDEFINED;
	status = vm_heap_add_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;
	if (realm->property_depth >= ACCESS_RECURSION_MAX) {
		status = vm_throw_range_error(realm, "Maximum property recursion exceeded");
		vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
		return status;
	}

	/* Every dispatch owns exactly one depth count until its normal or abrupt completion. */
	realm->property_depth++;
	if (object->kind == VM_KIND_PROXY) {
		status = access_proxy_extensibility(realm, (struct vm_proxy *)object, prevent, result);
	} else if (prevent) {
		status = vm_object_prevent_extensions(object, result);
	} else {
		status = 0;
		*result = 1;
		if ((object->flags & VM_OBJECT_NOT_EXTENSIBLE) != 0U)
			*result = 0;
	}

	/* Native callbacks never execute script; their collections still require this complete input frame. */
	realm->property_depth--;
	vm_heap_remove_tracer(realm->heap, access_read_trace, &roots);
	if (status != 0)
		return status;

	/* Succeeded: both operation depth and collector ownership have returned to their caller's state. */
	return 0;
}

/* Runs the two one-target extensibility traps with original participants and post-trap invariants. */
static int
access_proxy_extensibility(
	struct vm_realm *realm,
	struct vm_proxy *proxy,
	int prevent,
	int *result)
{
	struct vm_cell *roots[3];
	vm_value target;
	vm_value handler;
	vm_value method;
	vm_value method_key;
	vm_value produced;
	const char *name;
	unsigned registered;
	unsigned index;
	int is_cell;
	int truth;
	int target_extensible;
	int status;

	/* Reentrant revocation cannot discard either original protocol participant. */
	target = proxy->target;
	handler = proxy->handler;
	memset(roots, 0, sizeof(roots));
	is_cell = vm_value_is_cell(target);
	if (is_cell)
		roots[0] = vm_value_as_cell(target);
	is_cell = vm_value_is_cell(handler);
	if (is_cell)
		roots[1] = vm_value_as_cell(handler);
	registered = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Reject revocation before observing the current handler method. */
	if (target == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot inspect extensibility of a revoked Proxy");
		goto cleanup;
	}

	/* Both operations use GetMethod, including null fallback and noncallable rejection. */
	name = "isExtensible";
	if (prevent)
		name = "preventExtensions";
	method_key = vm_key_from_ascii(realm->heap, name);
	if (method_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* GetMethod may execute a getter that revokes the original record or collects its graph. */
	status = vm_get_method(realm, handler, method_key, &method);
	if (status != 0)
		goto cleanup;
	if (method == VM_VALUE_UNDEFINED) {
		if (prevent)
			status = vm_prevent_extensions(realm, (struct vm_object *)vm_value_as_cell(target), result);
		else
			status = vm_is_extensible(realm, (struct vm_object *)vm_value_as_cell(target), result);
		goto cleanup;
	}

	/* The trap can be fresh and otherwise owned only by this native call frame. */
	roots[2] = vm_value_as_cell(method);
	status = vm_call(realm, method, handler, &target, 1, &produced);
	if (status != 0)
		goto cleanup;
	truth = vm_to_boolean(produced);
	if (prevent && !truth) {
		/* A refused prevention never queries or changes its target after the trap. */
		status = 0;
		goto cleanup;
	}

	/* Extensibility is observed after the trap's changes, using the target's actual internal method. */
	status = vm_is_extensible(realm, (struct vm_object *)vm_value_as_cell(target), &target_extensible);
	if (status != 0)
		goto cleanup;
	if (prevent) {
		if (target_extensible) {
			status = vm_throw_type_error(realm, "Proxy preventExtensions trap left its target extensible");
			goto cleanup;
		}
	} else if (truth != target_extensible) {
		status = vm_throw_type_error(realm, "Proxy isExtensible trap disagreed with its target");
		goto cleanup;
	}

	/* Only a completed invariant check publishes the trap's Boolean result. */
	*result = truth;
	status = 0;

cleanup:
	/* Release only acquired root registrations on every fallback, trap and error path. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve the original abrupt outcome after retiring snapshots and fresh method ownership. */
	if (status != 0)
		return status;

	/* Succeeded: the result belongs to the completed extensibility operation. */
	return 0;
}

/* Executes a deletion trap without allowing it to conceal fixed or nonextensible target properties. */
static int
access_proxy_delete(
	struct vm_realm *realm,
	struct vm_proxy *proxy,
	vm_value key,
	int *deleted)
{
	struct vm_cell *roots[5];
	struct vm_descriptor current;
	struct vm_object *target_object;
	struct vm_string *string;
	vm_value target;
	vm_value handler;
	vm_value method;
	vm_value method_key;
	vm_value trap_key;
	vm_value args[2];
	vm_value produced;
	unsigned registered;
	unsigned index;
	int is_cell;
	int is_symbol;
	int truth;
	int present;
	int extensible;
	int status;

	/* The outer operation owns realm and original input; these snapshots survive reentrant revocation. */
	target = proxy->target;
	handler = proxy->handler;
	memset(roots, 0, sizeof(roots));
	is_cell = vm_value_is_cell(target);
	if (is_cell)
		roots[0] = vm_value_as_cell(target);
	is_cell = vm_value_is_cell(handler);
	if (is_cell)
		roots[1] = vm_value_as_cell(handler);
	is_cell = vm_value_is_cell(key);
	if (is_cell)
		roots[2] = vm_value_as_cell(key);
	registered = 0;
	for (index = 0; index < 5U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Revocation fails before any handler lookup or target observation. */
	if (target == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot delete a property of a revoked Proxy");
		goto cleanup;
	}

	/* GetMethod preserves getter exceptions, null fallback and noncallable rejection. */
	method_key = vm_key_from_ascii(realm->heap, "deleteProperty");
	if (method_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* A missing current method forwards to the original target's actual internal deletion. */
	status = vm_get_method(realm, handler, method_key, &method);
	if (status != 0)
		goto cleanup;
	if (method == VM_VALUE_UNDEFINED) {
		status = vm_delete_property(realm, (struct vm_object *)vm_value_as_cell(target), key, deleted);
		goto cleanup;
	}

	/* Retain a fresh method before converting the internal numeric representation of a property key. */
	roots[3] = vm_value_as_cell(method);
	trap_key = key;
	is_symbol = access_is_symbol(key);
	if (!is_symbol) {
		status = vm_to_string(realm, key, &string);
		if (status != 0)
			goto cleanup;
		trap_key = vm_value_cell(string);
		roots[4] = &string->cell;
	}

	/* The handler receives exactly its original target and string-or-symbol key. */
	args[0] = target;
	args[1] = trap_key;
	status = vm_call(realm, method, handler, args, 2, &produced);
	if (status != 0)
		goto cleanup;
	truth = vm_to_boolean(produced);
	if (!truth) {
		/* Refusal never observes the target after the trap. */
		status = 0;
		goto cleanup;
	}

	/* A trap may actually delete or reconfigure its target before this post-trap observation. */
	target_object = (struct vm_object *)vm_value_as_cell(target);
	status = vm_get_own_property(realm, target_object, key, &current, &present);
	if (status != 0)
		goto cleanup;
	if (!present) {
		*deleted = 1;
		status = 0;
		goto cleanup;
	}

	/* Only the descriptor's scalar attributes are needed after this point, never any unrooted payload. */
	if ((current.attributes & VM_PROPERTY_CONFIGURABLE) == 0U) {
		status = vm_throw_type_error(realm, "Proxy deleteProperty trap concealed a fixed own property");
		goto cleanup;
	}

	/* Existing configurable properties of a nonextensible target cannot be concealed either. */
	status = vm_is_extensible(realm, target_object, &extensible);
	if (status != 0)
		goto cleanup;
	if (!extensible) {
		status = vm_throw_type_error(realm, "Proxy deleteProperty trap concealed a nonextensible target's own property");
		goto cleanup;
	}

	/* A configurable own property on an extensible target may legally remain despite a true trap result. */
	*deleted = 1;
	status = 0;

cleanup:
	/* Retire only acquired snapshots and transient key/method roots on all completion paths. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve the original error after releasing the helper's ownership. */
	if (status != 0)
		return status;

	/* Succeeded: deleted reports the actual internal operation's Boolean. */
	return 0;
}

/* Sets an object's prototype when allowed (OrdinarySetPrototypeOf): not for a non-extensible object, and never into a cycle. */
static int
access_set_prototype(
	struct vm_object *object,
	struct vm_object *prototype)
{
	struct vm_object *walk;

	/* The same prototype is no change. */
	if (object->prototype == prototype)
		return 1;

	/* A non-extensible object keeps its prototype. */
	if ((object->flags & VM_OBJECT_NOT_EXTENSIBLE) != 0U)
		return 0;

	/* The object must not be on the new prototype's chain. */
	for (walk = prototype; walk != NULL; walk = walk->prototype) {
		if (walk == object)
			return 0;

		/* Ordinary cycle detection stops before any nonordinary GetPrototypeOf implementation. */
		if (walk->kind == VM_KIND_PROXY)
			break;
	}

	/* Succeeded: the ordinary object now uses its accepted prototype. */
	object->prototype = prototype;
	return 1;
}

/* Runs Proxy GetPrototypeOf while retaining fresh results through post-trap target observations. */
static int
access_proxy_get_prototype(
	struct vm_realm *realm,
	struct vm_proxy *proxy,
	vm_value *prototype)
{
	struct vm_cell *roots[4];
	vm_value target;
	vm_value handler;
	vm_value method;
	vm_value method_key;
	vm_value produced;
	vm_value actual;
	unsigned registered;
	unsigned index;
	int is_cell;
	int is_object;
	int extensible;
	int same;
	int status;

	/* Original participants remain snapshots even if method lookup revokes their Proxy. */
	target = proxy->target;
	handler = proxy->handler;
	memset(roots, 0, sizeof(roots));
	is_cell = vm_value_is_cell(target);
	if (is_cell)
		roots[0] = vm_value_as_cell(target);
	is_cell = vm_value_is_cell(handler);
	if (is_cell)
		roots[1] = vm_value_as_cell(handler);
	registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Reject revoked input before reading its handler. */
	if (target == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot get the prototype of a revoked Proxy");
		goto cleanup;
	}

	/* Lookup follows the normal GetMethod rules, including null fallback and noncallable rejection. */
	method_key = vm_key_from_ascii(realm->heap, "getPrototypeOf");
	if (method_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* An absent trap forwards to the original target's actual internal method. */
	status = vm_get_method(realm, handler, method_key, &method);
	if (status != 0)
		goto cleanup;
	if (method == VM_VALUE_UNDEFINED) {
		status = vm_get_prototype(realm, (struct vm_object *)vm_value_as_cell(target), prototype);
		goto cleanup;
	}

	/* Both fresh methods and their sole returned object need native frame ownership. */
	roots[2] = vm_value_as_cell(method);
	status = vm_call(realm, method, handler, &target, 1, &produced);
	if (status != 0)
		goto cleanup;
	is_object = vm_value_is_object(produced);
	if (!is_object && produced != VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Proxy getPrototypeOf trap must return an object or null");
		goto cleanup;
	}

	/* A target's own extensibility operation may execute script, so retain the sole returned object. */
	if (is_object)
		roots[3] = vm_value_as_cell(produced);
	status = vm_is_extensible(realm, (struct vm_object *)vm_value_as_cell(target), &extensible);
	if (status != 0)
		goto cleanup;
	if (!extensible) {
		status = vm_get_prototype(realm, (struct vm_object *)vm_value_as_cell(target), &actual);
		if (status != 0)
			goto cleanup;
		same = vm_same_value(produced, actual);
		if (!same) {
			status = vm_throw_type_error(realm, "Proxy getPrototypeOf trap disagreed with its nonextensible target");
			goto cleanup;
		}
	}

	/* An extensible target permits any object or null; fixed targets require the identity already checked. */
	*prototype = produced;
	status = 0;

cleanup:
	/* Release only snapshots and results whose root registrations were acquired. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve a failed trap or invariant after retiring native ownership. */
	if (status != 0)
		return status;

	/* Succeeded: the caller now owns the returned prototype value. */
	return 0;
}

/* Runs Proxy SetPrototypeOf with the requested prototype rooted by its outer native operation frame. */
static int
access_proxy_set_prototype(
	struct vm_realm *realm,
	struct vm_proxy *proxy,
	vm_value prototype,
	int *done)
{
	struct vm_cell *roots[3];
	vm_value target;
	vm_value handler;
	vm_value method;
	vm_value method_key;
	vm_value args[2];
	vm_value produced;
	vm_value actual;
	unsigned registered;
	unsigned index;
	int is_cell;
	int truth;
	int extensible;
	int same;
	int status;

	/* Reentrant lookup or trap revocation cannot replace the original target or handler. */
	target = proxy->target;
	handler = proxy->handler;
	memset(roots, 0, sizeof(roots));
	is_cell = vm_value_is_cell(target);
	if (is_cell)
		roots[0] = vm_value_as_cell(target);
	is_cell = vm_value_is_cell(handler);
	if (is_cell)
		roots[1] = vm_value_as_cell(handler);
	registered = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* A revoked input fails before handler lookup. */
	if (target == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot set the prototype of a revoked Proxy");
		goto cleanup;
	}

	/* GetMethod observes the current handler and handles null fallback. */
	method_key = vm_key_from_ascii(realm->heap, "setPrototypeOf");
	if (method_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Missing traps delegate the actual transition to the original target. */
	status = vm_get_method(realm, handler, method_key, &method);
	if (status != 0)
		goto cleanup;
	if (method == VM_VALUE_UNDEFINED) {
		if (prototype == VM_VALUE_NULL)
			status = vm_set_prototype(realm, (struct vm_object *)vm_value_as_cell(target), NULL, done);
		else
			status = vm_set_prototype(realm, (struct vm_object *)vm_value_as_cell(target), (struct vm_object *)vm_value_as_cell(prototype), done);
		goto cleanup;
	}

	/* The trap receives exactly the original target and requested object-or-null prototype. */
	roots[2] = vm_value_as_cell(method);
	args[0] = target;
	args[1] = prototype;
	status = vm_call(realm, method, handler, args, 2, &produced);
	if (status != 0)
		goto cleanup;
	truth = vm_to_boolean(produced);
	if (!truth) {
		/* A refused transition never queries its target after the trap. */
		status = 0;
		goto cleanup;
	}

	/* Only nonextensible targets constrain a true trap result to the actual target prototype. */
	status = vm_is_extensible(realm, (struct vm_object *)vm_value_as_cell(target), &extensible);
	if (status != 0)
		goto cleanup;
	if (!extensible) {
		status = vm_get_prototype(realm, (struct vm_object *)vm_value_as_cell(target), &actual);
		if (status != 0)
			goto cleanup;
		same = vm_same_value(prototype, actual);
		if (!same) {
			status = vm_throw_type_error(realm, "Proxy setPrototypeOf trap disagreed with its nonextensible target");
			goto cleanup;
		}
	}

	/* A true result has passed the actual target's post-trap constraints. */
	*done = 1;
	status = 0;

cleanup:
	/* Retire every acquired snapshot and fresh method root regardless of outcome. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve failures after releasing all helper ownership. */
	if (status != 0)
		return status;

	/* Succeeded: done reports the accepted or refused transition. */
	return 0;
}

/* Marks every meaningful key in a native vector, including existing output prefixes. */
static void
access_keys_trace(
	struct vm_heap *heap,
	void *context)
{
	struct wb_vector *keys;
	size_t index;

	/* Unused capacity holds no live values and must never become a collector edge. */
	keys = context;
	for (index = 0; index < keys->count; index++)
		vm_heap_mark_value(heap, ((vm_value *)keys->items)[index]);
}

/* Creates a property-key list from an array-like object without coercing element values into keys. */
static int
access_property_key_list(
	struct vm_realm *realm,
	vm_value source,
	struct wb_vector *keys)
{
	struct vm_cell *current_root;
	struct vm_string *string;
	vm_value length_key;
	vm_value index_key;
	vm_value value;
	vm_value key;
	double number;
	uint64_t length;
	uint64_t index;
	int is_cell;
	int is_object;
	int is_string;
	int is_symbol;
	int status;

	/* The caller retains source and the accumulated vector; this slot owns each fresh intermediate value. */
	current_root = NULL;
	status = vm_heap_add_root(realm->heap, &current_root);
	if (status != 0)
		return status;
	is_object = vm_value_is_object(source);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Proxy ownKeys trap must return an object");
		goto cleanup;
	}

	/* LengthOfArrayLike gets length once before applying ToLength's number conversion. */
	length_key = vm_key_from_ascii(realm->heap, "length");
	if (length_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* A fresh length object must survive its own valueOf callback and any collection it performs. */
	status = vm_get(realm, source, length_key, &value);
	if (status != 0)
		goto cleanup;
	is_cell = vm_value_is_cell(value);
	if (is_cell)
		current_root = vm_value_as_cell(value);
	status = vm_to_number(realm, value, &number);
	if (status != 0)
		goto cleanup;
	current_root = NULL;

	/* NaN and nonpositive lengths produce an empty list, including negative zero and negative infinity. */
	length = 0;
	if (number == number && number > 0.0) {
		if (number >= 9007199254740991.0)
			length = (uint64_t)9007199254740991.0;
		else
			length = (uint64_t)floor(number);
	}

	/* Read each actual index in order; arbitrary large lengths remain lazy so the first abrupt getter still wins. */
	for (index = 0; index < length; index++) {
		if (index <= 2147483647U) {
			index_key = vm_value_int32((int32_t)index);
		} else {
			status = vm_to_string(realm, vm_value_number((double)index), &string);
			if (status != 0)
				goto cleanup;
			current_root = &string->cell;
			status = vm_key_from_string(realm->heap, string, &index_key);
			if (status != 0)
				goto cleanup;
			current_root = NULL;
		}

		/* Property-key lists permit only String or Symbol elements, with no object or numeric element coercion. */
		status = vm_get(realm, source, index_key, &value);
		if (status != 0)
			goto cleanup;
		current_root = NULL;
		is_cell = vm_value_is_cell(value);
		if (is_cell)
			current_root = vm_value_as_cell(value);
		is_string = vm_value_is_string(value);
		is_symbol = access_is_symbol(value);
		if (!is_string && !is_symbol) {
			status = vm_throw_type_error(realm, "Proxy ownKeys list elements must be strings or symbols");
			goto cleanup;
		}

		/* Only primitive strings are canonicalized into the VM's index-or-atom representation. */
		key = value;
		if (is_string) {
			status = vm_key_from_string(realm->heap, (struct vm_string *)vm_value_as_cell(value), &key);
			if (status != 0)
				goto cleanup;
		}

		/* The accumulated vector becomes the owner before the next getter may collect or delete earlier values. */
		status = wb_vector_push(keys, &key);
		if (status != 0)
			goto cleanup;
		current_root = NULL;
	}

	/* Every element was evaluated before the caller checks duplicate entries. */
	status = 0;

cleanup:
	/* Neither a length object nor an element may remain retained after this list conversion frame ends. */
	vm_heap_remove_root(realm->heap, &current_root);
	if (status != 0)
		return status;

	/* Succeeded: keys preserves the original array-like order as canonical internal property keys. */
	return 0;
}

/* Executes Proxy OwnPropertyKeys with complete list evaluation and post-trap target key-set invariants. */
static int
access_proxy_own_keys(
	struct vm_realm *realm,
	struct vm_proxy *proxy,
	struct wb_vector *keys)
{
	struct vm_cell *roots[4];
	struct wb_vector lists[ACCESS_OWN_LIST_COUNT];
	struct vm_descriptor descriptor;
	struct vm_object *target_object;
	vm_value target;
	vm_value handler;
	vm_value method;
	vm_value method_key;
	vm_value produced;
	vm_value key;
	size_t item;
	unsigned index;
	unsigned registered;
	unsigned traced;
	int is_cell;
	int listed;
	int extensible;
	int found;
	int status;

	/* Original participants survive method lookup and trap revocation independently of the outer Proxy record. */
	target = proxy->target;
	handler = proxy->handler;
	memset(roots, 0, sizeof(roots));
	is_cell = vm_value_is_cell(target);
	if (is_cell)
		roots[0] = vm_value_as_cell(target);
	is_cell = vm_value_is_cell(handler);
	if (is_cell)
		roots[1] = vm_value_as_cell(handler);
	registered = 0;
	traced = 0;
	for (index = 0; index < ACCESS_OWN_LIST_COUNT; index++)
		wb_vector_init(&lists[index], sizeof(vm_value));

	/* Every acquired root registration is retired even when a later registration fails. */
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Accumulated keys need native vector ownership across later element getters and target policy collections. */
	for (index = 0; index < ACCESS_OWN_LIST_COUNT; index++) {
		status = vm_heap_add_tracer(realm->heap, access_keys_trace, &lists[index]);
		if (status != 0)
			goto cleanup;
		traced++;
	}

	/* Revocation fails before the current handler method is observed. */
	if (target == VM_VALUE_NULL) {
		status = vm_throw_type_error(realm, "Cannot list the own keys of a revoked Proxy");
		goto cleanup;
	}

	/* GetMethod provides the usual dynamic getter, null fallback and noncallable checks. */
	method_key = vm_key_from_ascii(realm->heap, "ownKeys");
	if (method_key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Missing methods delegate to the original target's actual internal key operation. */
	status = vm_get_method(realm, handler, method_key, &method);
	if (status != 0)
		goto cleanup;
	if (method == VM_VALUE_UNDEFINED) {
		status = vm_own_property_keys(realm, (struct vm_object *)vm_value_as_cell(target), keys);
		goto cleanup;
	}

	/* Retain both a fresh trap and its otherwise unowned array-like result. */
	roots[2] = vm_value_as_cell(method);
	status = vm_call(realm, method, handler, &target, 1, &produced);
	if (status != 0)
		goto cleanup;
	is_cell = vm_value_is_cell(produced);
	if (is_cell)
		roots[3] = vm_value_as_cell(produced);
	status = access_property_key_list(realm, produced, &lists[ACCESS_OWN_TRAP_KEYS]);
	if (status != 0)
		goto cleanup;

	/* Duplicate rejection occurs only after the complete array-like list has been evaluated. */
	for (item = 0; item < lists[ACCESS_OWN_TRAP_KEYS].count; item++) {
		key = ((vm_value *)lists[ACCESS_OWN_TRAP_KEYS].items)[item];
		listed = access_key_listed(&lists[ACCESS_OWN_TRAP_KEYS], item, key);
		if (listed) {
			status = vm_throw_type_error(realm, "Proxy ownKeys trap returned duplicate keys");
			goto cleanup;
		}
	}

	/* Extensibility is snapshotted before the target key list, after all trap-result getters. */
	target_object = (struct vm_object *)vm_value_as_cell(target);
	status = vm_is_extensible(realm, target_object, &extensible);
	if (status != 0)
		goto cleanup;
	status = vm_own_property_keys(realm, target_object, &lists[ACCESS_OWN_TARGET_KEYS]);
	if (status != 0)
		goto cleanup;

	/* Classify every target descriptor before checking any missing key, preserving abrupt-operation order. */
	for (item = 0; item < lists[ACCESS_OWN_TARGET_KEYS].count; item++) {
		key = ((vm_value *)lists[ACCESS_OWN_TARGET_KEYS].items)[item];
		status = vm_get_own_property(realm, target_object, key, &descriptor, &found);
		if (status != 0)
			goto cleanup;
		if (found && (descriptor.attributes & VM_PROPERTY_CONFIGURABLE) == 0U)
			status = wb_vector_push(&lists[ACCESS_OWN_FIXED_KEYS], &key);
		else
			status = wb_vector_push(&lists[ACCESS_OWN_CONFIGURABLE_KEYS], &key);
		if (status != 0)
			goto cleanup;
	}

	/* Fixed keys are mandatory regardless of target extensibility. */
	for (item = 0; item < lists[ACCESS_OWN_FIXED_KEYS].count; item++) {
		key = ((vm_value *)lists[ACCESS_OWN_FIXED_KEYS].items)[item];
		listed = access_key_listed(&lists[ACCESS_OWN_TRAP_KEYS], lists[ACCESS_OWN_TRAP_KEYS].count, key);
		if (!listed) {
			status = vm_throw_type_error(realm, "Proxy ownKeys trap omitted a fixed target key");
			goto cleanup;
		}
	}

	/* A nonextensible target also requires every configurable key and forbids every extra key. */
	if (!extensible) {
		for (item = 0; item < lists[ACCESS_OWN_CONFIGURABLE_KEYS].count; item++) {
			key = ((vm_value *)lists[ACCESS_OWN_CONFIGURABLE_KEYS].items)[item];
			listed = access_key_listed(&lists[ACCESS_OWN_TRAP_KEYS], lists[ACCESS_OWN_TRAP_KEYS].count, key);
			if (!listed) {
				status = vm_throw_type_error(realm, "Proxy ownKeys trap omitted a nonextensible target key");
				goto cleanup;
			}
		}

		/* Both lists are unique and every target key was matched, so equal size excludes all extras. */
		if (lists[ACCESS_OWN_TRAP_KEYS].count != lists[ACCESS_OWN_TARGET_KEYS].count) {
			status = vm_throw_type_error(realm, "Proxy ownKeys trap added a key to a nonextensible target");
			goto cleanup;
		}
	}

	/* Preserve trap order when appending; no ordinary numeric-key sorting is applied to a Proxy list. */
	for (item = 0; item < lists[ACCESS_OWN_TRAP_KEYS].count; item++) {
		key = ((vm_value *)lists[ACCESS_OWN_TRAP_KEYS].items)[item];
		status = wb_vector_push(keys, &key);
		if (status != 0)
			goto cleanup;
	}

	/* Complete list and invariant checks supplied the successfully appended keys. */
	status = 0;

cleanup:
	/* Stop tracing vectors before releasing their C storage; unused capacity never retains cells. */
	while (traced != 0U) {
		traced--;
		vm_heap_remove_tracer(realm->heap, access_keys_trace, &lists[traced]);
	}

	/* Every vector was initialized before any fallible registration or script operation. */
	for (index = 0; index < ACCESS_OWN_LIST_COUNT; index++)
		wb_vector_release(&lists[index]);

	/* Original participants and transient trap objects retire after all list storage. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Propagate original errors after retiring snapshots, temporary lists and trap results. */
	if (status != 0)
		return status;

	/* Succeeded: the outer frame now owns the complete appended key sequence. */
	return 0;
}
