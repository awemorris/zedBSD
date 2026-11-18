/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Objects: properties found through shapes,
 * elements kept densely, prototypes, arrays and their length, and the
 * order of an object's own keys.
 *
 * A property key is a value: an atom (a string key), a symbol, or an int32
 * index (the keys of array indices below 2^31; larger indices are atoms
 * for now).  An index property with the default attributes lives in the
 * elements, unless the elements would be too sparse; every other property
 * lives in a slot the shape names.  An array's length is its first named
 * property, kept equal to the length field.
 *
 * The functions that only need data properties are here; calling an
 * accessor's getter or setter needs the interpreter, so a
 * get reports an accessor's cell and a set refuses to write through one.
 */

#include "vm/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The fewest slots or elements an object's arrays are made for. */
#define OBJECT_ARRAY_MIN	4U

/* How far past the elements in use a new index may go and still be kept densely. */
#define OBJECT_SPARSE_GAP	1024U

/* The largest index kept as an int32 key. */
#define OBJECT_INDEX_MAX	0x7FFFFFFFU

static void object_proxy_trace(struct vm_heap *heap, struct vm_cell *cell);
static void object_symbol_trace(struct vm_heap *heap, struct vm_cell *cell);
static void object_accessor_trace(struct vm_heap *heap, struct vm_cell *cell);

/* The cell type of plain objects: they hold their shape, prototype, slots and elements. */
const struct vm_cell_type vm_object_type = {
	"object", vm_object_trace, vm_object_finalize
};

/* The cell type of arrays: plain objects whose length follows their elements. */
const struct vm_cell_type vm_array_type = {
	"array", vm_object_trace, vm_object_finalize
};

/* The cell type of symbols: they hold their description. */
const struct vm_cell_type vm_symbol_type = {
	"symbol", object_symbol_trace, NULL
};

/* The cell type of accessor pairs: they hold their getter and setter. */
const struct vm_cell_type vm_accessor_type = {
	"accessor", object_accessor_trace, NULL
};

/* Proxy payloads share ordinary object storage and retain two additional managed edges. */
static const struct vm_cell_type object_proxy_type = {
	"proxy", object_proxy_trace, vm_object_finalize
};

static int object_grow_slots(struct vm_object *object, uint32_t count);
static int object_grow_elements(struct vm_object *object, uint32_t index);
static int object_store_named(struct vm_heap *heap, struct vm_object *object, vm_value key, vm_value value, uint32_t attributes);
static int object_reshape(struct vm_heap *heap, struct vm_object *object, vm_value skip, vm_value change, uint32_t attributes);
static int object_is_length(struct vm_object *object, vm_value key);
static void object_sync_length(struct vm_heap *heap, struct vm_object *object);
static int object_key_compare(const void *left, const void *right);
static int object_array_index(vm_value key, uint32_t *index);
static int object_array_key_compare(const void *left, const void *right);
static void object_array_keys_trace(struct vm_heap *heap, void *context);

/*
 * Marks the cell a value refers to, if it refers to one.
 */
void
vm_heap_mark_value(
	struct vm_heap *heap,
	vm_value value)
{
	int is_cell;

	/* Numbers and constants refer to nothing. */
	is_cell = vm_value_is_cell(value);
	if (!is_cell)
		return;

	/* The cell. */
	vm_heap_mark(heap, vm_value_as_cell(value));
}

/*
 * Tells whether a value is an object (a plain object, an array or a
 * function), as opposed to a primitive.
 */
int
vm_value_is_object(
	vm_value value)
{
	struct vm_cell *cell;
	int is_cell;

	/* Numbers and constants are primitives. */
	is_cell = vm_value_is_cell(value);
	if (!is_cell)
		return 0;

	/* The cell types that are objects. */
	cell = vm_value_as_cell(value);
	if (cell->type == &vm_object_type)
		return 1;
	if (cell->type == &vm_array_type)
		return 1;
	if (cell->type == &vm_function_type)
		return 1;
	if (cell->type == &vm_promise_type)
		return 1;
	if (cell->type == &object_proxy_type)
		return 1;

	/* Strings and symbols are primitives. */
	return 0;
}

/*
 * Tells whether a value is a string.
 */
int
vm_value_is_string(
	vm_value value)
{
	struct vm_cell *cell;
	int is_cell;

	/* Only cells are strings. */
	is_cell = vm_value_is_cell(value);
	if (!is_cell)
		return 0;

	/* A string cell. */
	cell = vm_value_as_cell(value);
	if (cell->type == &vm_string_type)
		return 1;

	/* Another cell. */
	return 0;
}

/*
 * Tells whether a key is an array index (an int32 key that is not
 * negative) and reports the index.
 */
int
vm_value_is_array_index(
	vm_value key,
	uint32_t *index)
{
	int32_t number;
	int is_int32;

	/* Only int32 keys are indices. */
	is_int32 = vm_value_is_int32(key);
	if (!is_int32)
		return 0;

	/* A negative int32 is no index. */
	number = vm_value_as_int32(key);
	if (number < 0)
		return 0;

	/* Reports the index. */
	*index = (uint32_t)number;
	return 1;
}

/*
 * Makes the property key of a string: an int32 when the string is an array
 * index in canonical form ("0", "17", never "017") below 2^31, its atom
 * otherwise.
 */
int
vm_key_from_string(
	struct vm_heap *heap,
	struct vm_string *string,
	vm_value *key)
{
	struct vm_string *atom;
	uint64_t number;
	uint32_t index;
	uint16_t unit;
	uint16_t first;
	int canonical;

	/* Digits only, without a leading zero (but "0" itself), below 2^31. */
	canonical = 0;
	if (string->length != 0 && string->length <= 10U)
		canonical = 1;
	number = 0;
	for (index = 0; canonical && index < string->length; index++) {
		unit = vm_string_at(string, index);
		if (unit < '0' || unit > '9')
			canonical = 0;
		number = number * 10U + (uint64_t)(unit - '0');
	}

	/* A leading zero makes a string that is not the index's canonical form. */
	first = 0;
	if (string->length != 0)
		first = vm_string_at(string, 0);
	if (canonical && string->length > 1U && first == '0')
		canonical = 0;

	/* An index past int32 stays a string. */
	if (canonical && number > OBJECT_INDEX_MAX)
		canonical = 0;

	/* An index is its int32. */
	if (canonical) {
		*key = vm_value_int32((int32_t)number);
		return 0;
	}

	/* Any other string is its atom. */
	atom = vm_atom(heap, string);
	if (atom == NULL)
		return ENOMEM;

	/* Succeeded: the key is the atom. */
	*key = vm_value_cell(atom);
	return 0;
}

/*
 * Makes the property key of an ASCII name that is not an index (the
 * engine's own names); VM_VALUE_EMPTY when out of memory.
 */
vm_value
vm_key_from_ascii(
	struct vm_heap *heap,
	const char *ascii)
{
	struct vm_string *atom;

	/* The name's atom. */
	atom = vm_atom_from_ascii(heap, ascii);
	if (atom == NULL)
		return VM_VALUE_EMPTY;

	/* Reports its value. */
	return vm_value_cell(atom);
}

/*
 * Makes a plain object with a prototype (NULL for none); NULL when out of
 * memory. Both the input prototype and unpublished cell survive collection.
 */
struct vm_object *
vm_object_create(
	struct vm_heap *heap,
	struct vm_object *prototype)
{
	struct vm_object *object;
	struct vm_cell *roots[2];
	unsigned registered;
	unsigned index;
	int error;

	/* The prototype remains available even if allocating the new cell collects. */
	roots[0] = NULL;
	if (prototype != NULL)
		roots[0] = &prototype->cell;
	roots[1] = NULL;
	registered = 0;
	error = 0;
	for (index = 0; index < 2U; index++) {
		error = vm_heap_add_root(heap, &roots[index]);
		if (error != 0)
			break;
		registered++;
	}

	/* Protect the newborn before shape initialization can allocate a cold root shape. */
	object = NULL;
	if (error == 0) {
		object = vm_heap_alloc(heap, &vm_object_type, sizeof(*object));
		if (object != NULL) {
			roots[1] = &object->cell;
			error = vm_object_init(heap, object, prototype);
		}
	}

	/* No factory-owned roots survive a failed or complete construction. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(heap, &roots[registered]);
	}

	/* Allocation or shape failure leaves no published object. */
	if (object == NULL || error != 0)
		return NULL;

	/* Succeeded: the caller receives the complete object. */
	return object;
}

/*
 * Makes an empty array with a prototype; NULL when out of memory.
 * Its unpublished cell survives the fallible length-property construction.
 */
struct vm_object *
vm_array_create(
	struct vm_heap *heap,
	struct vm_object *prototype)
{
	struct vm_object *array;
	struct vm_cell *roots[2];
	vm_value key;
	unsigned registered;
	unsigned index;
	int error;

	/* An incoming prototype can be the only caller-visible graph edge. */
	roots[0] = NULL;
	if (prototype != NULL)
		roots[0] = &prototype->cell;
	roots[1] = NULL;
	registered = 0;
	error = 0;
	for (index = 0; index < 2U; index++) {
		error = vm_heap_add_root(heap, &roots[index]);
		if (error != 0)
			break;
		registered++;
	}

	/* The newborn Array is retained through shape, key and named-length allocation. */
	array = NULL;
	if (error == 0) {
		do {
			array = vm_heap_alloc(heap, &vm_array_type, sizeof(*array));
			if (array == NULL)
				break;
			roots[1] = &array->cell;

			/* A cold root shape may itself require a collector allocation. */
			error = vm_object_init(heap, array, prototype);
			if (error != 0)
				break;
			array->flags |= VM_OBJECT_ARRAY;
			array->kind = VM_KIND_ARRAY;

			/* Heap-owned atoms keep the key, while this root keeps the Array. */
			key = vm_key_from_ascii(heap, "length");
			if (key == VM_VALUE_EMPTY) {
				error = ENOMEM;
				break;
			}

			/* Store the zero length only after the key has a heap-owned atom. */
			error = object_store_named(heap, array, key, vm_value_int32(0), VM_PROPERTY_WRITABLE);
		} while (0);
	}

	/* Even a partially initialized Array becomes reclaimable after failure. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(heap, &roots[registered]);
	}

	/* Report the allocation or length-property failure through the existing NULL convention. */
	if (array == NULL || error != 0)
		return NULL;

	/* Succeeded: callers receive an initialized Array with zero length. */
	return array;
}

/*
 * Creates private managed Proxy records with immutable internal call/construct slots.
 * Public constructor installation is independent of this native factory.
 */
int
vm_proxy_create(
	struct vm_realm *realm,
	vm_value target,
	vm_value handler,
	vm_value *result)
{
	struct vm_proxy *proxy;
	struct vm_cell *roots[4];
	vm_value values[2];
	unsigned index;
	unsigned registered;
	int is_cell;
	int is_object;
	int callable;
	int constructor;
	int status;

	/* Protect incoming ownership before either validation errors or allocation can collect. */
	*result = VM_VALUE_UNDEFINED;
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	values[0] = target;
	values[1] = handler;
	for (index = 0; index < 2U; index++) {
		roots[index + 1U] = NULL;
		is_cell = vm_value_is_cell(values[index]);
		if (is_cell)
			roots[index + 1U] = vm_value_as_cell(values[index]);
	}

	/* Reserve the unpublished-record root slot before acquiring registrations. */
	roots[3] = NULL;
	registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Both Proxy protocol participants must be objects. */
	is_object = vm_value_is_object(target);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Proxy target must be an object");
		goto cleanup;
	}

	/* Validate the handler independently before allocating the record. */
	is_object = vm_value_is_object(handler);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Proxy handler must be an object");
		goto cleanup;
	}

	/* Capture immutable internal call and construct slots before allocation can collect. */
	callable = vm_value_is_callable(target);
	constructor = vm_value_is_constructor(target);

	/* Retain the newborn before ordinary shape initialization can allocate. */
	proxy = vm_heap_alloc(realm->heap, &object_proxy_type, sizeof(*proxy));
	if (proxy == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Initialize every traced payload field before shape initialization can collect. */
	roots[3] = &proxy->object.cell;
	proxy->target = target;
	proxy->handler = handler;
	proxy->callable = callable;
	proxy->constructor = constructor;
	proxy->object.kind = VM_KIND_PROXY;
	proxy->object.internal = VM_VALUE_UNDEFINED;
	status = vm_object_init(realm->heap, &proxy->object, NULL);
	if (status != 0)
		goto cleanup;

	/* Only a complete native record becomes visible to the caller. */
	*result = vm_value_cell(proxy);

cleanup:
	/* Release only registrations actually acquired before a possible failure. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve an abrupt outcome after releasing this frame's ownership. */
	if (status != 0)
		return status;

	/* Succeeded: the caller owns the managed read primitive. */
	return 0;
}

/*
 * Revokes a private managed Proxy and releases both protocol ownership edges.
 */
int
vm_proxy_revoke(
	struct vm_realm *realm,
	vm_value value)
{
	struct vm_object *object;
	struct vm_proxy *proxy;
	int is_object;
	int status;

	/* Script values cannot forge a native Proxy record through their prototype. */
	is_object = vm_value_is_object(value);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Proxy revocation requires a native Proxy");
		return status;
	}

	/* Inspect the native kind only after establishing object identity. */
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_PROXY) {
		status = vm_throw_type_error(realm, "Proxy revocation requires a native Proxy");
		return status;
	}

	/* Clearing both slots is idempotent and prevents later Get operations from entering the handler. */
	proxy = (struct vm_proxy *)object;
	proxy->target = VM_VALUE_NULL;
	proxy->handler = VM_VALUE_NULL;

	/* Succeeded: no future read can use the retired protocol participants. */
	return 0;
}

/*
 * Gives an object made by another file (a function, later a DOM node) its
 * empty shape and its prototype.
 */
int
vm_object_init(
	struct vm_heap *heap,
	struct vm_object *object,
	struct vm_object *prototype)
{
	struct vm_shape *root;

	/* The root shape: no properties. */
	root = vm_shape_root(heap);
	if (root == NULL)
		return ENOMEM;

	/* The object's layout and prototype; the cell came zeroed, so there are no slots or elements. */
	object->shape = root;
	object->prototype = prototype;
	object->native_operations = NULL;

	/* Succeeded: the object is ready for properties. */
	return 0;
}

/*
 * Makes a symbol with a description (a string value or undefined).
 */
struct vm_symbol *
vm_symbol_create(
	struct vm_heap *heap,
	vm_value description)
{
	struct vm_symbol *symbol;
	struct vm_cell *root;
	int is_cell;
	int error;

	/* The cell and its description. */
	root = NULL;
	is_cell = vm_value_is_cell(description);
	if (is_cell)
		root = vm_value_as_cell(description);
	error = vm_heap_add_root(heap, &root);
	if (error != 0)
		return NULL;
	symbol = vm_heap_alloc(heap, &vm_symbol_type, sizeof(*symbol));
	if (symbol == NULL) {
		vm_heap_remove_root(heap, &root);
		return NULL;
	}

	/* The new symbol owns its description before temporary ownership ends. */
	symbol->description = description;
	symbol->private_name = 0;
	vm_heap_remove_root(heap, &root);

	/* Succeeded: the symbol. */
	return symbol;
}

/*
 * Makes an accessor pair (the getter and setter are functions or
 * undefined).
 */
struct vm_accessor *
vm_accessor_create(
	struct vm_heap *heap,
	vm_value getter,
	vm_value setter)
{
	struct vm_accessor *accessor;
	struct vm_cell *roots[2];
	unsigned registered;
	unsigned index;
	int is_cell;
	int error;

	/* The cell and its functions. */
	roots[0] = NULL;
	is_cell = vm_value_is_cell(getter);
	if (is_cell)
		roots[0] = vm_value_as_cell(getter);
	roots[1] = NULL;
	is_cell = vm_value_is_cell(setter);
	if (is_cell)
		roots[1] = vm_value_as_cell(setter);
	registered = 0;
	for (index = 0; index < 2U; index++) {
		error = vm_heap_add_root(heap, &roots[index]);
		if (error != 0)
			break;
		registered++;
	}

	/* A partially registered pair cannot enter the collecting allocator. */
	accessor = NULL;
	if (registered == 2U)
		accessor = vm_heap_alloc(heap, &vm_accessor_type, sizeof(*accessor));
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(heap, &roots[registered]);
	}

	/* A failed root or cell allocation publishes no partial pair. */
	if (accessor == NULL)
		return NULL;
	accessor->getter = getter;
	accessor->setter = setter;

	/* Succeeded: the pair. */
	return accessor;
}

/*
 * Finds an own native or stored property, preserving fallible lookup outcomes.
 */
int
vm_object_get_own(
	struct vm_object *object,
	vm_value key,
	struct vm_property *property)
{
	int found;

	/* Native policy may expose a virtual value without creating a stored property. */
	if (object->native_operations != NULL && object->native_operations->get_own != NULL) {
		found = object->native_operations->get_own(object, key, property);
		if (found != 0)
			return found;
	}

	/* Missing virtual properties continue into the ordinary own storage. */
	found = vm_object_get_own_ordinary(object, key, property);
	if (found == 0)
		return 0;

	/* Succeeded: the own stored property is available. */
	return found;
}

/*
 * Finds an object's own property of a key; zero when it has none.
 */
int
vm_object_get_own_ordinary(
	struct vm_object *object,
	vm_value key,
	struct vm_property *property)
{
	uint32_t index;
	uint32_t slot;
	uint32_t attributes;
	int is_index;
	int found;

	/* An index with a value in the elements. */
	is_index = vm_value_is_array_index(key, &index);
	if (is_index &&
	    index < object->element_capacity &&
	    object->elements[index] != VM_VALUE_EMPTY) {
		property->holder = object;
		property->value = &object->elements[index];
		property->attributes = VM_PROPERTY_DEFAULT;
		return 1;
	}

	/* Otherwise a slot the shape names. */
	found = vm_shape_find(object->shape, key, &slot, &attributes);
	if (!found)
		return 0;

	/* The property in its slot. */
	property->holder = object;
	property->value = &object->slots[slot];
	property->attributes = attributes;
	return 1;
}

/*
 * Finds a property of a key on an object or its prototypes; zero when
 * none has it.
 */
int
vm_object_find(
	struct vm_object *object,
	vm_value key,
	struct vm_property *property)
{
	int found;

	/* Each object of the chain, from the object itself. */
	while (object != NULL) {
		found = vm_object_get_own(object, key, property);
		if (found < 0)
			return found;
		if (found)
			return 1;

		/* Its prototype next. */
		object = object->prototype;
	}

	/* No object of the chain has the key. */
	return 0;
}

/*
 * Defines a stored ordinary property with a value and attributes.
 * Native script policy is dispatched before this storage primitive; an accessor
 * property stores its vm_accessor cell.
 */
int
vm_object_define(
	struct vm_heap *heap,
	struct vm_object *object,
	vm_value key,
	vm_value value,
	uint32_t attributes)
{
	uint32_t index;
	uint32_t slot;
	uint32_t old_attributes;
	double number;
	uint32_t length;
	int is_index;
	int existing;
	int is_length;
	int is_number;
	int dense;
	int named;
	int error;

	/* Script access has already normalized length; the C boundary accepts any valid uint32 number. */
	is_length = object_is_length(object, key);
	if (is_length) {
		/* Requires a numeric value rather than silently applying user coercion in the VM heap layer. */
		is_number = vm_value_is_number(value);
		if (!is_number)
			return EINVAL;

		/* Positive range checks reject NaN as well as infinities before the C unsigned cast. */
		number = vm_value_as_number(value);
		if (!(number >= 0.0 && number <= 4294967295.0))
			return EINVAL;

		/* Only exact integer metadata is a valid array length. */
		length = (uint32_t)number;
		if ((double)length != number)
			return EINVAL;

		/* Locates the existing length slot before updating its value and descriptor attributes. */
		named = vm_shape_find(object->shape, key, &slot, &old_attributes);
		if (!named)
			return EINVAL;

		/* Updates metadata and deletes existing supported indices after a contraction. */
		error = vm_array_set_length(heap, object, length);
		if (error != 0)
			return error;

		/* Descriptor writability must survive even when length is outside signed int32. */
		if (old_attributes != attributes) {
			error = object_reshape(heap, object, VM_VALUE_EMPTY, key, attributes);
			if (error != 0)
				return error;
		}

		/* Succeeded: the array's length value and requested attributes agree. */
		return 0;
	}

	/* Dense storage follows actual capacity, never a possibly huge logical length. */
	is_index = vm_value_is_array_index(key, &index);
	named = vm_shape_find(object->shape, key, &slot, &old_attributes);
	dense = 0;
	if (is_index &&
	    !named &&
	    attributes == VM_PROPERTY_DEFAULT &&
	    (uint64_t)index <= (uint64_t)object->element_capacity + OBJECT_SPARSE_GAP)
		dense = 1;
	if (dense) {
		/* A new property of an object that takes none is refused. */
		if ((object->flags & VM_OBJECT_NOT_EXTENSIBLE) != 0U &&
		    (index >= object->element_capacity || object->elements[index] == VM_VALUE_EMPTY))
			return EPERM;

		/* Room for the index. */
		error = object_grow_elements(object, index);
		if (error != 0)
			return error;

		/* Stores the value; the length reaches past the index. */
		object->elements[index] = value;
		if (index >= object->length)
			object->length = index + 1U;
		object_sync_length(heap, object);
		return 0;
	}

	/* An index in the elements that takes other attributes leaves them for a slot (an element that exists is not a new property). */
	existing = 0;
	if (is_index && index < object->element_capacity) {
		existing = object->elements[index] != VM_VALUE_EMPTY;
		object->elements[index] = VM_VALUE_EMPTY;
	}

	/* A property the shape has keeps its slot, with new attributes when they differ. */
	if (named) {
		if (old_attributes != attributes) {
			error = object_reshape(heap, object, VM_VALUE_EMPTY, key, attributes);
			if (error != 0)
				return error;
		}

		/* The value in its slot. */
		object->slots[slot] = value;
		return 0;
	}

	/* A new property of an object that takes none is refused (Object.freeze changes the attributes of what is there). */
	if ((object->flags & VM_OBJECT_NOT_EXTENSIBLE) != 0U && !existing)
		return EPERM;

	/* A new named property. */
	error = object_store_named(heap, object, key, value, attributes);
	if (error != 0)
		return error;

	/*
	 * An array's length reaches past an index kept in a slot.  A plain
	 * object's sparse numeric slot must not move its dense-elements watermark.
	 */
	if (is_index &&
	    (object->flags & VM_OBJECT_ARRAY) != 0U &&
	    index >= object->length) {
		object->length = index + 1U;
		object_sync_length(heap, object);
	}

	/* Succeeded: the property is defined. */
	return 0;
}

/*
 * Reads a property through the prototype chain: a data property's value,
 * an accessor's vm_accessor cell (its getter is called by the caller), or
 * undefined when no object has it.
 */
int
vm_object_get(
	struct vm_object *object,
	vm_value key,
	vm_value *value)
{
	struct vm_property property;
	int found;

	/* The property, wherever on the chain. */
	found = vm_object_find(object, key, &property);
	if (found < 0)
		return -found;
	if (!found) {
		*value = VM_VALUE_UNDEFINED;
		return 0;
	}

	/* Succeeded: its value. */
	*value = *property.value;
	return 0;
}

/*
 * Writes a property as assignment does (the ordinary [[Set]] for data
 * properties): an own writable property takes the value, and otherwise a
 * new own property is made unless a non-writable property or an accessor
 * on the chain forbids it.  *done says whether the value was written.
 */
int
vm_object_set(
	struct vm_heap *heap,
	struct vm_object *object,
	vm_value key,
	vm_value value,
	int *done)
{
	struct vm_property property;
	struct vm_descriptor descriptor;
	int found;
	int error;
	int handled;

	/* Nothing is written until the rules allow it. */
	*done = 0;
	if (object->native_operations != NULL && object->native_operations->define != NULL) {
		/* Native assignment may reject unsupported virtual keys before ordinary storage is created. */
		memset(&descriptor, 0, sizeof(descriptor));
		descriptor.has = VM_HAS_VALUE | VM_HAS_WRITABLE | VM_HAS_ENUMERABLE | VM_HAS_CONFIGURABLE;
		descriptor.value = value;
		descriptor.attributes = VM_PROPERTY_DEFAULT;
		handled = 0;
		error = object->native_operations->define(NULL, object, key, &descriptor, &handled, done);
		if (error != 0)
			return error;
		if (handled)
			return 0;
	}

	/* Stored and virtual descriptors still apply ordinary readonly and accessor restrictions. */
	found = vm_object_find(object, key, &property);
	if (found < 0)
		return -found;

	/* An accessor's setter is called by the caller; a non-writable property refuses. */
	if (found && (property.attributes & VM_PROPERTY_ACCESSOR) != 0U)
		return 0;
	if (found && (property.attributes & VM_PROPERTY_WRITABLE) == 0U)
		return 0;

	/* An own data property takes the value in place (an array's length by its rules). */
	if (found && property.holder == object) {
		error = vm_object_define(heap, object, key, value, property.attributes);
		if (error != 0)
			return error;
		*done = 1;
		return 0;
	}

	/* Otherwise a new own property; an object that takes none refuses. */
	if ((object->flags & VM_OBJECT_NOT_EXTENSIBLE) != 0U)
		return 0;
	error = vm_object_define(heap, object, key, value, VM_PROPERTY_DEFAULT);
	if (error != 0)
		return error;

	/* Succeeded: the value is written. */
	*done = 1;
	return 0;
}

/*
 * Deletes an own property; *deleted is 0 when it is not configurable (the
 * object keeps it) and 1 otherwise (including when there was none).
 */
int
vm_object_delete(
	struct vm_heap *heap,
	struct vm_object *object,
	vm_value key,
	int *deleted)
{
	uint32_t index;
	uint32_t slot;
	uint32_t attributes;
	int is_index;
	int named;
	int error;
	int handled;

	/* Native deletion policy may refuse a live virtual property or handle its removal. */
	if (object->native_operations != NULL && object->native_operations->delete != NULL) {
		handled = 0;
		error = object->native_operations->delete(heap, object, key, &handled, deleted);
		if (error != 0)
			return error;
		if (handled)
			return 0;
	}

	/* A missing property is deleted already. */
	*deleted = 1;
	is_index = vm_value_is_array_index(key, &index);
	if (is_index &&
	    index < object->element_capacity &&
	    object->elements[index] != VM_VALUE_EMPTY) {
		/* An element becomes a hole (an array keeps its length). */
		object->elements[index] = VM_VALUE_EMPTY;
		return 0;
	}

	/* A named property. */
	named = vm_shape_find(object->shape, key, &slot, &attributes);
	if (!named)
		return 0;

	/* One that is not configurable stays. */
	if ((attributes & VM_PROPERTY_CONFIGURABLE) == 0U) {
		*deleted = 0;
		return 0;
	}

	/* The shape is made again without it. */
	error = object_reshape(heap, object, key, VM_VALUE_EMPTY, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the property is gone. */
	return 0;
}

/*
 * Sets an array's length: a shorter length deletes the index properties
 * from the new length on.
 */
int
vm_array_set_length(
	struct vm_heap *heap,
	struct vm_object *array,
	uint32_t length)
{
	struct wb_vector keys;
	struct vm_cell *root;
	vm_value key;
	uint32_t index;
	size_t item;
	size_t kept;
	int is_index;
	int deleted;
	int traced;
	int error;

	/* Retain the array and every copied key across native enumeration and deletion callbacks. */
	root = &array->cell;
	wb_vector_init(&keys, sizeof(vm_value));
	traced = 0;
	error = vm_heap_add_root(heap, &root);
	if (error != 0)
		return error;
	error = vm_heap_add_tracer(heap, object_array_keys_trace, &keys);
	if (error != 0)
		goto cleanup;
	traced = 1;

	/* Growth changes no element descriptor and never scans a possibly huge sparse logical length. */
	if (length >= array->length) {
		array->length = length;
		object_sync_length(heap, array);
		goto cleanup;
	}

	/* Snapshot present own keys before publishing the shorter metadata; absent indices need no deletion. */
	error = vm_object_own_keys(heap, array, &keys);
	if (error != 0)
		goto cleanup;
	kept = 0;
	for (item = 0; item < keys.count; item++) {
		key = ((vm_value *)keys.items)[item];
		is_index = object_array_index(key, &index);
		if (is_index && index >= length) {
			((vm_value *)keys.items)[kept] = key;
			kept++;
		}
	}

	/* Numeric ordering includes atom-backed indices above signed int32 and native-policy prefixes. */
	keys.count = kept;
	if (keys.count > 1U)
		qsort(keys.items, keys.count, sizeof(vm_value), object_array_key_compare);
	array->length = length;
	object_sync_length(heap, array);

	/* Delete higher indices first so the first refusal preserves every lower descriptor and earlier higher deletion. */
	item = keys.count;
	while (item != 0U) {
		item--;
		key = ((vm_value *)keys.items)[item];
		error = vm_object_delete(heap, array, key, &deleted);
		if (error != 0)
			goto cleanup;
		if (!deleted) {
			/* The failed index bounds the actual length after the already completed higher deletions. */
			is_index = object_array_index(key, &index);
			array->length = index + 1U;
			object_sync_length(heap, array);
			error = EPERM;
			goto cleanup;
		}
	}

	/* Every present property at or above the requested bound was deleted. */
	error = 0;

cleanup:
	/* Snapshot ownership ends on growth, complete shrink, partial refusal and native allocation errors alike. */
	if (traced)
		vm_heap_remove_tracer(heap, object_array_keys_trace, &keys);
	wb_vector_release(&keys);
	vm_heap_remove_root(heap, &root);
	if (error != 0)
		return error;

	/* Succeeded: metadata and the remaining own elements agree with the requested length. */
	return 0;
}

/*
 * Lists an object's own keys in the order of ES2015's OrdinaryOwnPropertyKeys:
 * the indices ascending, then the strings and then the symbols in the order
 * they were added.
 */
int
vm_object_own_keys(
	struct vm_heap *heap,
	struct vm_object *object,
	struct wb_vector *keys)
{
	struct vm_cell *cell;
	vm_value *named_keys;
	uint32_t *slots;
	uint32_t *attributes;
	uint32_t count;
	uint32_t index;
	uint32_t first;
	size_t beginning;
	size_t read;
	size_t write;
	size_t prior;
	int duplicate;
	vm_value key;
	int pass;
	int is_index;
	int is_symbol;
	int error;

	/* Native keys form a policy-ordered prefix before ordinary own storage keys. */
	beginning = keys->count;
	if (object->native_operations != NULL && object->native_operations->own_keys != NULL) {
		error = object->native_operations->own_keys(heap, object, keys);
		if (error != 0)
			return error;
	}

	/* The shape's keys in the order they were added. */
	count = vm_shape_count(object->shape);
	named_keys = calloc((size_t)count + 1U, sizeof(*named_keys));
	if (named_keys == NULL)
		return ENOMEM;

	/* Slot metadata is allocated independently so a failure releases only completed owners. */
	slots = calloc((size_t)count + 1U, sizeof(*slots));
	if (slots == NULL) {
		free(named_keys);
		return ENOMEM;
	}

	/* Attribute metadata completes the temporary shape enumeration before it is read. */
	attributes = calloc((size_t)count + 1U, sizeof(*attributes));
	if (attributes == NULL) {
		free(named_keys);
		free(slots);
		return ENOMEM;
	}

	/* Fills them from the shape. */
	vm_shape_keys(object->shape, named_keys, slots, attributes);

	/* The indices: the elements' first. */
	error = 0;
	first = (uint32_t)keys->count;
	for (index = 0; error == 0 && index < object->element_capacity; index++) {
		if (object->elements[index] == VM_VALUE_EMPTY)
			continue;

		/* The element's index as its key. */
		key = vm_value_int32((int32_t)index);
		error = wb_vector_push(keys, &key);
	}

	/* Then the ones kept in slots. */
	for (index = 0; error == 0 && index < count; index++) {
		is_index = vm_value_is_int32(named_keys[index]);
		if (!is_index)
			continue;
		error = wb_vector_push(keys, &named_keys[index]);
	}

	/* All the indices sorted together. */
	if (error == 0 && keys->count - first > 1U)
		qsort((vm_value *)keys->items + first, keys->count - first, sizeof(vm_value), object_key_compare);

	/* Then the strings, then the symbols, each in the order added. */
	for (pass = 0; error == 0 && pass < 2; pass++) {
		for (index = 0; error == 0 && index < count; index++) {
			/* The indices are listed already. */
			is_index = vm_value_is_int32(named_keys[index]);
			if (is_index)
				continue;

			/* Strings in the first pass, symbols in the second; a private name never. */
			cell = vm_value_as_cell(named_keys[index]);
			is_symbol = 0;
			if (cell->type == &vm_symbol_type)
				is_symbol = 1;
			if (is_symbol != pass)
				continue;
			if (is_symbol && ((struct vm_symbol *)cell)->private_name)
				continue;
			error = wb_vector_push(keys, &named_keys[index]);
		}
	}

	/* The shape's lists are no longer needed. */
	free(named_keys);
	free(slots);
	free(attributes);
	if (error != 0)
		return error;

	/* Ordinary objects keep their existing enumeration cost and ordering. */
	if (object->native_operations != NULL && object->native_operations->own_keys != NULL) {
		/* Native and ordinary names may overlap; retain the first occurrence of each key. */
		write = beginning;
		for (read = beginning; read < keys->count; read++) {
			key = ((vm_value *)keys->items)[read];

			/* A preceding occurrence wins over duplicate native or stored property keys. */
			duplicate = 0;
			for (prior = beginning; prior < write; prior++) {
				if (((vm_value *)keys->items)[prior] == key) {
					duplicate = 1;
					break;
				}
			}

			/* Only unique keys advance the compacted prefix and ordinary tail. */
			if (!duplicate) {
				((vm_value *)keys->items)[write] = key;
				write++;
			}
		}

		/* The caller sees the compacted sequence, leaving any earlier append prefix intact. */
		keys->count = write;
	}

	/* Succeeded: the keys are listed. */
	return 0;
}

/*
 * Prevents extensions only when native policy permits the state transition.
 */
int
vm_object_prevent_extensions(
	struct vm_object *object,
	int *done)
{
	int allowed;
	int error;

	/* Refusal leaves flags and every descriptor unchanged. */
	*done = 0;
	allowed = 1;
	if (object->native_operations != NULL && object->native_operations->prevent_extensions != NULL) {
		error = object->native_operations->prevent_extensions(object, &allowed);
		if (error != 0)
			return error;
		if (!allowed)
			return 0;
	}

	/* Ordinary objects retain their established nonextensible flag semantics. */
	object->flags |= VM_OBJECT_NOT_EXTENSIBLE;
	*done = 1;

	/* Succeeded: this object refuses every subsequent new own property. */
	return 0;
}

/*
 * Marks what an object refers to: its shape, its prototype, the values in
 * its slots and in its elements.
 */
void
vm_object_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_object *object;
	uint32_t count;
	uint32_t index;

	/* The layout and the prototype. */
	object = (struct vm_object *)cell;
	if (object->shape != NULL)
		vm_heap_mark(heap, (struct vm_cell *)object->shape);
	if (object->prototype != NULL)
		vm_heap_mark(heap, &object->prototype->cell);

	/* The slots in use. */
	count = 0;
	if (object->shape != NULL)
		count = vm_shape_count(object->shape);
	for (index = 0; index < count && index < object->slot_capacity; index++)
		vm_heap_mark_value(heap, object->slots[index]);

	/* The elements. */
	for (index = 0; index < object->element_capacity; index++)
		vm_heap_mark_value(heap, object->elements[index]);

	/* The value a wrapper holds. */
	vm_heap_mark_value(heap, object->internal);
}

/*
 * Frees a dead object's slots and elements.
 */
void
vm_object_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_object *object;

	UNUSED_PARAMETER(heap);

	/* The two arrays the object owns. */
	object = (struct vm_object *)cell;
	free(object->slots);
	free(object->elements);
	object->slots = NULL;
	object->elements = NULL;
}

/* Marks a symbol's description. */
static void
object_symbol_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	/* The description, a string or undefined. */
	vm_heap_mark_value(heap, ((struct vm_symbol *)cell)->description);
}

/* Marks an accessor pair's getter and setter. */
static void
object_accessor_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_accessor *accessor;

	/* The two functions, where set. */
	accessor = (struct vm_accessor *)cell;
	vm_heap_mark_value(heap, accessor->getter);
	vm_heap_mark_value(heap, accessor->setter);
}

/* Makes room for a number of slots, keeping the values in them. */
static int
object_grow_slots(
	struct vm_object *object,
	uint32_t count)
{
	vm_value *slots;
	uint32_t capacity;
	uint32_t index;

	/* Room enough already. */
	if (count <= object->slot_capacity)
		return 0;

	/* Twice the room, at least the count. */
	capacity = object->slot_capacity * 2U;
	if (capacity < OBJECT_ARRAY_MIN)
		capacity = OBJECT_ARRAY_MIN;
	if (capacity < count)
		capacity = count;
	slots = realloc(object->slots, capacity * sizeof(*slots));
	if (slots == NULL)
		return ENOMEM;

	/* The new slots hold nothing yet. */
	for (index = object->slot_capacity; index < capacity; index++)
		slots[index] = VM_VALUE_EMPTY;
	object->slots = slots;
	object->slot_capacity = capacity;

	/* Succeeded: the slots fit. */
	return 0;
}

/* Makes room in the elements for an index, the new ones holes. */
static int
object_grow_elements(
	struct vm_object *object,
	uint32_t index)
{
	vm_value *elements;
	uint32_t capacity;
	uint32_t item;

	/* Room enough already. */
	if (index < object->element_capacity)
		return 0;

	/* Twice the room, at least past the index. */
	capacity = object->element_capacity * 2U;
	if (capacity < OBJECT_ARRAY_MIN)
		capacity = OBJECT_ARRAY_MIN;
	if (capacity <= index)
		capacity = index + 1U;
	elements = realloc(object->elements, capacity * sizeof(*elements));
	if (elements == NULL)
		return ENOMEM;

	/* The new elements are holes. */
	for (item = object->element_capacity; item < capacity; item++)
		elements[item] = VM_VALUE_EMPTY;
	object->elements = elements;
	object->element_capacity = capacity;

	/* Succeeded: the index fits. */
	return 0;
}

/* Adds a new named property: the shape one property longer and its slot. */
static int
object_store_named(
	struct vm_heap *heap,
	struct vm_object *object,
	vm_value key,
	vm_value value,
	uint32_t attributes)
{
	struct vm_shape *shape;
	struct vm_cell *roots[3];
	uint32_t slot;
	unsigned registered;
	unsigned index;
	int is_cell;
	int error;

	/* Retain the owner, new key and value while a shape allocation can collect. */
	roots[0] = &object->cell;
	roots[1] = NULL;
	is_cell = vm_value_is_cell(key);
	if (is_cell)
		roots[1] = vm_value_as_cell(key);
	roots[2] = NULL;
	is_cell = vm_value_is_cell(value);
	if (is_cell)
		roots[2] = vm_value_as_cell(value);
	registered = 0;
	for (index = 0; index < 3U; index++) {
		error = vm_heap_add_root(heap, &roots[index]);
		if (error != 0)
			goto cleanup;
		registered++;
	}

	/* The shape with the property is traced through the owner's root shape. */
	shape = vm_shape_add(heap, object->shape, key, attributes);
	if (shape == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* The slot, whose number is the count of properties before it. */
	slot = vm_shape_count(shape) - 1U;
	error = object_grow_slots(object, slot + 1U);
	if (error != 0)
		goto cleanup;

	/* The value, then the shape that names it. */
	object->slots[slot] = value;
	object->shape = shape;

cleanup:
	/* Published storage owns the property after temporary roots end. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(heap, &roots[registered]);
	}

	/* Report either complete publication or the allocation failure. */
	return error;
}

/*
 * Makes an object's shape again from the root: without the key skip, and
 * with the key change given new attributes (VM_VALUE_EMPTY for neither),
 * moving the values to their new slots.
 */
static int
object_reshape(
	struct vm_heap *heap,
	struct vm_object *object,
	vm_value skip,
	vm_value change,
	uint32_t attributes)
{
	struct vm_shape *shape;
	struct vm_cell *root;
	vm_value *keys;
	vm_value *slots;
	uint32_t *old_slots;
	uint32_t *old_attributes;
	uint32_t count;
	uint32_t kept;
	uint32_t index;
	uint32_t wanted;
	int error;

	/* The old layout, and the values in the order they were added. */
	count = vm_shape_count(object->shape);
	keys = calloc(count + 1U, sizeof(*keys));
	old_slots = calloc(count + 1U, sizeof(*old_slots));
	old_attributes = calloc(count + 1U, sizeof(*old_attributes));
	slots = calloc(count + 1U, sizeof(*slots));
	if (keys == NULL ||
	    old_slots == NULL ||
	    old_attributes == NULL ||
	    slots == NULL) {
		free(keys);
		free(old_slots);
		free(old_attributes);
		free(slots);
		return ENOMEM;
	}

	/* Fills them from the shape. */
	vm_shape_keys(object->shape, keys, old_slots, old_attributes);
	root = &object->cell;
	error = vm_heap_add_root(heap, &root);
	if (error != 0) {
		free(keys);
		free(old_slots);
		free(old_attributes);
		free(slots);
		return error;
	}

	/* The new path from the root, each value moving to its new slot. */
	shape = vm_shape_root(heap);
	if (shape == NULL)
		error = ENOMEM;
	kept = 0;
	for (index = 0; error == 0 && index < count; index++) {
		/* The key deleted is left out. */
		if (keys[index] == skip)
			continue;

		/* The key changed takes its new attributes. */
		wanted = old_attributes[index];
		if (keys[index] == change)
			wanted = attributes;
		shape = vm_shape_add(heap, shape, keys[index], wanted);
		if (shape == NULL) {
			error = ENOMEM;
			break;
		}

		/* The value keeps its place in the order. */
		slots[kept] = object->slots[old_slots[index]];
		kept++;
	}

	/* The lists of the old layout are no longer needed. */
	free(keys);
	free(old_slots);
	free(old_attributes);
	if (error != 0) {
		free(slots);
		vm_heap_remove_root(heap, &root);
		return error;
	}

	/* The new slots and shape replace the old ones. */
	free(object->slots);
	object->slots = slots;
	object->slot_capacity = count + 1U;
	for (index = kept; index < object->slot_capacity; index++)
		object->slots[index] = VM_VALUE_EMPTY;
	object->shape = shape;
	vm_heap_remove_root(heap, &root);

	/* Succeeded: the object has its new layout. */
	return 0;
}

/* Tells whether a key is an array's length. */
static int
object_is_length(
	struct vm_object *object,
	vm_value key)
{
	struct vm_cell *cell;
	int is_cell;
	int matches;

	/* Only arrays have the special length. */
	if ((object->flags & VM_OBJECT_ARRAY) == 0U)
		return 0;

	/* A key that is not a string is not "length". */
	is_cell = vm_value_is_cell(key);
	if (!is_cell)
		return 0;
	cell = vm_value_as_cell(key);
	if (cell->type != &vm_string_type)
		return 0;

	/* Compare the existing key without allocating a new atom or hiding allocation failure. */
	matches = vm_string_equal_ascii((struct vm_string *)cell, "length");
	if (!matches)
		return 0;

	/* The key is the array's length. */
	return 1;
}

/* Copies an array's length field into its length property (slot 0). */
static void
object_sync_length(
	struct vm_heap *heap,
	struct vm_object *object)
{
	UNUSED_PARAMETER(heap);

	/* Only arrays have the property. */
	if ((object->flags & VM_OBJECT_ARRAY) == 0U)
		return;

	/* The length as a number (an int32 while it fits). */
	object->slots[0] = vm_value_number((double)object->length);
}

/* Orders two int32 keys by their index, for qsort. */
static int
object_key_compare(
	const void *left,
	const void *right)
{
	int32_t first;
	int32_t second;

	/* The two indices. */
	first = vm_value_as_int32(*(const vm_value *)left);
	second = vm_value_as_int32(*(const vm_value *)right);

	/* The smaller first. */
	if (first < second)
		return -1;
	if (first > second)
		return 1;

	/* Equal keys. */
	return 0;
}

/* Marks ordinary object storage and the two Proxy protocol ownership edges. */
static void
object_proxy_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_proxy *proxy;

	/* Revocation clears both protocol fields, allowing their former graphs to be reclaimed. */
	proxy = (struct vm_proxy *)cell;
	vm_object_trace(heap, cell);
	vm_heap_mark_value(heap, proxy->target);
	vm_heap_mark_value(heap, proxy->handler);
}

/* Orders actual array-index keys numerically without signed-int32 truncation or coercion. */
static int
object_array_key_compare(
	const void *left,
	const void *right)
{
	uint32_t first;
	uint32_t second;
	int is_index;

	/* Enumeration filtering guarantees both keys are indices; normalization is pure and cannot run script. */
	is_index = object_array_index(*(const vm_value *)left, &first);
	if (!is_index)
		return 0;
	is_index = object_array_index(*(const vm_value *)right, &second);
	if (!is_index)
		return 0;

	/* Smaller numeric indices sort first, including the atom-backed upper half of uint32. */
	if (first < second)
		return -1;
	if (first > second)
		return 1;

	/* Succeeded: equal numeric indices compare equally. */
	return 0;
}

/* Keeps the native array-length key snapshot alive while deletion callbacks may collect. */
static void
object_array_keys_trace(
	struct vm_heap *heap,
	void *context)
{
	struct wb_vector *keys;
	size_t item;

	/* Vector storage is outside the managed heap and contains only initialized live entries. */
	keys = context;
	for (item = 0; item < keys->count; item++)
		vm_heap_mark_value(heap, ((vm_value *)keys->items)[item]);
}

/* Recognizes actual uint32 array-index spellings without widening the ordinary storage key representation. */
static int
object_array_index(
	vm_value key,
	uint32_t *index)
{
	struct vm_string *string;
	uint64_t number;
	uint32_t position;
	uint16_t unit;
	int is_index;
	int is_string;

	/* Existing small numeric keys already carry their exact nonnegative index. */
	is_index = vm_value_is_array_index(key, index);
	if (is_index)
		return 1;
	is_string = vm_value_is_string(key);
	if (!is_string)
		return 0;

	/* Canonical decimal array indices have at most ten digits and no redundant leading zero. */
	string = (struct vm_string *)vm_value_as_cell(key);
	if (string->length == 0U || string->length > 10U)
		return 0;
	unit = vm_string_at(string, 0);
	if (unit == '0' && string->length != 1U)
		return 0;

	/* Exact integer accumulation excludes nondecimal syntax and the reserved uint32 maximum. */
	number = 0;
	for (position = 0; position < string->length; position++) {
		unit = vm_string_at(string, position);
		if (unit < '0' || unit > '9')
			return 0;
		number = number * 10U + (uint64_t)(unit - '0');
	}

	/* The string 4294967295 is an ordinary property name rather than an array index. */
	if (number >= (uint64_t)4294967295U)
		return 0;
	*index = (uint32_t)number;

	/* Succeeded: the existing atom names one exact array index, including indices above signed int32. */
	return 1;
}
