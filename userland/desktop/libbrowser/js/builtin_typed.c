/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ArrayBuffer, the typed arrays and DataView (ws074-p230).
 *
 * An ArrayBuffer object holds a byte buffer cell (a fixed size, freed with
 * the cell).  A typed array is an ordinary object with native property
 * operations for its indices and a view cell (the buffer, an offset, a
 * length and the element type) as its internal value.  The methods that
 * are the same for every element type live on %TypedArray%.prototype; the
 * ones that only walk elements are written in the page's own language over
 * the generic Array methods (typed_prelude).
 */

#include "js/builtin.h"
#include "base/os-system.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The element types, in the order of the per-type intrinsics. */
enum typed_type {
	TYPED_INT8,
	TYPED_UINT8,
	TYPED_UINT8_CLAMPED,
	TYPED_INT16,
	TYPED_UINT16,
	TYPED_INT32,
	TYPED_UINT32,
	TYPED_FLOAT32,
	TYPED_FLOAT64,
	TYPED_BIGINT64,
	TYPED_BIGUINT64,
	TYPED_TYPES
};

/* The names of the element types. */
static const char *const typed_names[TYPED_TYPES] = {
	"Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array",
	"Uint32Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array"};

/* The bytes of an element of each type. */
static const unsigned typed_sizes[TYPED_TYPES] = {1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8};

/* The most bytes a buffer holds. */
#define TYPED_BYTES_MAX	(1UL << 30)

/* A byte buffer: the bytes follow in their own allocation. */
struct typed_buffer {
	struct vm_cell cell;
	size_t length;
	unsigned char *data;
};

/* A typed array's view of a buffer. */
struct typed_view {
	struct vm_cell cell;
	vm_value buffer_object;
	struct typed_buffer *buffer;
	uint32_t offset;
	uint32_t length;
	uint32_t type;
	uint32_t reserved;
	struct vm_heap *heap;
};

static void typed_view_trace(struct vm_heap *heap, struct vm_cell *cell);
static void typed_buffer_finalize(struct vm_heap *heap, struct vm_cell *cell);

static const struct vm_cell_type typed_buffer_type = {"buffer", NULL, typed_buffer_finalize};
static const struct vm_cell_type typed_view_type = {"typed-view", typed_view_trace, NULL};

static int typed_get_own(struct vm_object *object, vm_value key, struct vm_property *property);
static int typed_own_keys(struct vm_heap *heap, struct vm_object *object, struct wb_vector *keys);
static int typed_define(struct vm_realm *realm, struct vm_object *object, vm_value key, const struct vm_descriptor *descriptor, int *handled, int *done);
static int typed_delete(struct vm_heap *heap, struct vm_object *object, vm_value key, int *handled, int *deleted);

static const struct vm_native_operations typed_operations = {
	typed_get_own, typed_own_keys, typed_define, typed_delete, NULL};

/* The page-language half of %TypedArray%.prototype (the walks over elements and the static makers). */
static const char typed_prelude[] =
	"(function (P, T, A) {\n"
	"  var AP = Array.prototype;\n"
	"  function def(o, name, f) { Object.defineProperty(o, name, { value: f, writable: true, configurable: true, enumerable: false }); }\n"
	"  function viaArray(name) { var f = AP[name]; def(P, name, function () { return f.apply(this, arguments); }); }\n"
	"  ['forEach', 'indexOf', 'lastIndexOf', 'includes', 'join', 'reverse', 'every', 'some', 'find', 'findIndex', 'findLast',\n"
	"   'findLastIndex', 'reduce', 'reduceRight', 'fill', 'copyWithin', 'at', 'toLocaleString'].forEach(viaArray);\n"
	"  function make(self, n) { return new self.constructor(n); }\n"
	"  def(P, 'map', function map(f, thisArg) {\n"
	"    var n = this.length, r = make(this, n), i;\n"
	"    for (i = 0; i < n; i++) r[i] = f.call(thisArg, this[i], i, this);\n"
	"    return r;\n"
	"  });\n"
	"  def(P, 'filter', function filter(f, thisArg) {\n"
	"    var kept = [], i;\n"
	"    for (i = 0; i < this.length; i++) if (f.call(thisArg, this[i], i, this)) kept.push(this[i]);\n"
	"    var r = make(this, kept.length);\n"
	"    for (i = 0; i < kept.length; i++) r[i] = kept[i];\n"
	"    return r;\n"
	"  });\n"
	"  def(P, 'sort', function sort(cmp) {\n"
	"    var a = AP.slice.call(this), i;\n"
	"    a.sort(cmp || function (x, y) { return x < y ? -1 : x > y ? 1 : 0; });\n"
	"    for (i = 0; i < a.length; i++) this[i] = a[i];\n"
	"    return this;\n"
	"  });\n"
	"  def(P, 'toSorted', function toSorted(cmp) { return this.slice().sort(cmp); });\n"
	"  def(P, 'toReversed', function toReversed() { return this.slice().reverse(); });\n"
	"  def(P, 'with', function (index, value) { var r = this.slice(); r[index < 0 ? this.length + index : index] = value; return r; });\n"
	"  def(P, 'toString', AP.toString);\n"
	"  def(P, 'keys', function keys() { return AP.keys.call(this); });\n"
	"  def(P, 'entries', function entries() { return AP.entries.call(this); });\n"
	"  var values = function values() { return AP.values.call(this); };\n"
	"  def(P, 'values', values);\n"
	"  def(P, Symbol.iterator, values);\n"
	"  def(T, 'of', function of() { var r = new this(arguments.length), i; for (i = 0; i < arguments.length; i++) r[i] = arguments[i]; return r; });\n"
	"  def(T, 'from', function from(source, mapFn, thisArg) {\n"
	"    var list = Array.from(source), r = new this(list.length), i;\n"
	"    for (i = 0; i < list.length; i++) r[i] = mapFn ? mapFn.call(thisArg, list[i], i) : list[i];\n"
	"    return r;\n"
	"  });\n"
	"})";

/* WeakRef and FinalizationRegistry: the target is held strongly (nothing is collected early, so a deref never fails). */
static const char typed_weak_prelude[] =
	"(function (g) {\n"
	"  function def(o, name, f) { Object.defineProperty(o, name, { value: f, writable: true, configurable: true, enumerable: false }); }\n"
	"  function WeakRef(target) {\n"
	"    if (!new.target) throw new TypeError(\"Constructor WeakRef requires 'new'\");\n"
	"    if ((typeof target !== 'object' && typeof target !== 'function') || target === null) throw new TypeError('WeakRef: invalid target');\n"
	"    Object.defineProperty(this, '__target', { value: target });\n"
	"  }\n"
	"  def(WeakRef.prototype, 'deref', function deref() { return this.__target; });\n"
	"  Object.defineProperty(WeakRef.prototype, Symbol.toStringTag, { value: 'WeakRef', configurable: true });\n"
	"  function FinalizationRegistry(cleanup) {\n"
	"    if (!new.target) throw new TypeError(\"Constructor FinalizationRegistry requires 'new'\");\n"
	"    if (typeof cleanup !== 'function') throw new TypeError('FinalizationRegistry: cleanup must be callable');\n"
	"  }\n"
	"  def(FinalizationRegistry.prototype, 'register', function register() {});\n"
	"  def(FinalizationRegistry.prototype, 'unregister', function unregister() { return false; });\n"
	"  Object.defineProperty(FinalizationRegistry.prototype, Symbol.toStringTag, { value: 'FinalizationRegistry', configurable: true });\n"
	"  def(g, 'WeakRef', WeakRef);\n"
	"  def(g, 'FinalizationRegistry', FinalizationRegistry);\n"
	"})";

static int typed_numeric_index(const struct typed_view *view, vm_value key, int *canonical, int *valid, uint32_t *index);
static int typed_convert(struct vm_realm *realm, int type, vm_value value, double *number, uint64_t *big);
static int typed_make_view(struct vm_realm *realm, int type, struct vm_object *prototype, vm_value buffer_object, uint32_t offset, uint32_t length, int data_view, vm_value *result);

/* Marks a view's buffer object. */
static void
typed_view_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	vm_heap_mark_value(heap, ((struct typed_view *)cell)->buffer_object);
}

/* Frees a buffer's bytes. */
static void
typed_buffer_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	UNUSED_PARAMETER(heap);
	free(((struct typed_buffer *)cell)->data);
}

/* Reads an element of a type from bytes. */
static int
typed_load(
	struct vm_heap *heap,
	const unsigned char *bytes,
	int type,
	vm_value *value)
{
	int8_t i8;
	int16_t i16;
	uint16_t u16;
	int32_t i32;
	uint32_t u32;
	uint64_t u64;
	float f32;
	double f64;

	switch (type) {
	case TYPED_INT8:
		i8 = (int8_t)bytes[0];
		*value = vm_value_int32(i8);
		return 0;
	case TYPED_UINT8:
	case TYPED_UINT8_CLAMPED:
		*value = vm_value_int32(bytes[0]);
		return 0;
	case TYPED_INT16:
		memcpy(&i16, bytes, 2);
		*value = vm_value_int32(i16);
		return 0;
	case TYPED_UINT16:
		memcpy(&u16, bytes, 2);
		*value = vm_value_int32(u16);
		return 0;
	case TYPED_INT32:
		memcpy(&i32, bytes, 4);
		*value = vm_value_int32(i32);
		return 0;
	case TYPED_UINT32:
		memcpy(&u32, bytes, 4);
		*value = vm_value_number((double)u32);
		return 0;
	case TYPED_FLOAT32:
		memcpy(&f32, bytes, 4);
		*value = vm_value_number((double)f32);
		return 0;
	case TYPED_FLOAT64:
		memcpy(&f64, bytes, 8);
		*value = vm_value_number(f64);
		return 0;
	case TYPED_BIGINT64:
		memcpy(&u64, bytes, 8);
		return vm_bigint_from_int64(heap, (int64_t)u64, value);
	default:
		memcpy(&u64, bytes, 8);
		return vm_bigint_from_uint64(heap, u64, value);
	}
}

/* Wraps a number to the low bits of an integer type (ToInt32's wrap). */
static uint32_t
typed_wrap(
	double number)
{
	double wrapped;

	if (number != number || isinf(number))
		return 0U;
	wrapped = fmod(trunc(number), 4294967296.0);
	if (wrapped < 0.0)
		wrapped += 4294967296.0;
	return (uint32_t)wrapped;
}

/* Converts a value to what an element of a type stores (numbers, or BigInts for the 64-bit types). */
static int
typed_convert(
	struct vm_realm *realm,
	int type,
	vm_value value,
	double *number,
	uint64_t *big)
{
	struct vm_bigint *bigint;
	vm_value converted;
	int status;

	/* Content type selects the language conversion, independently of element width or signedness. */
	*number = 0.0;
	*big = 0;
	if (type == TYPED_BIGINT64 || type == TYPED_BIGUINT64) {
		status = vm_to_bigint(realm, value, &converted);
		if (status != 0)
			return status;
		bigint = vm_value_as_bigint(converted);
		*big = vm_bigint_to_uint64(bigint);
		return 0;
	}

	/* Number content rejects BigInt and Symbol while honoring object conversion in the owning realm. */
	status = vm_to_number(realm, value, number);
	if (status != 0)
		return status;

	/* Succeeded: native storage receives scalar Number or modulo-64-bit BigInt content. */
	return 0;
}

/* Writes a converted element into bytes. */
static void
typed_store(
	int type,
	unsigned char *bytes,
	double number,
	uint64_t big)
{
	uint32_t wrapped;
	uint16_t u16;
	float f32;

	switch (type) {
	case TYPED_INT8:
	case TYPED_UINT8:
		bytes[0] = (unsigned char)typed_wrap(number);
		break;
	case TYPED_UINT8_CLAMPED:
		if (number != number || number <= 0.0)
			bytes[0] = 0;
		else if (number >= 255.0)
			bytes[0] = 255;
		else
			bytes[0] = (unsigned char)nearbyint(number);
		break;
	case TYPED_INT16:
	case TYPED_UINT16:
		u16 = (uint16_t)typed_wrap(number);
		memcpy(bytes, &u16, 2);
		break;
	case TYPED_INT32:
	case TYPED_UINT32:
		wrapped = typed_wrap(number);
		memcpy(bytes, &wrapped, 4);
		break;
	case TYPED_FLOAT32:
		f32 = (float)number;
		memcpy(bytes, &f32, 4);
		break;
	case TYPED_FLOAT64:
		memcpy(bytes, &number, 8);
		break;
	default:
		memcpy(bytes, &big, 8);
		break;
	}
}

/* Reports the view an object holds when it is a typed array. */
static struct typed_view *
typed_view_of(
	vm_value value)
{
	struct vm_object *object;

	if (!vm_value_is_object(value))
		return NULL;
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_TYPED_ARRAY)
		return NULL;
	return (struct typed_view *)vm_value_as_cell(object->internal);
}

/* Reports the buffer an object holds when it is an ArrayBuffer. */
static struct typed_buffer *
typed_buffer_of(
	vm_value value)
{
	struct vm_object *object;

	if (!vm_value_is_object(value))
		return NULL;
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_ARRAY_BUFFER)
		return NULL;
	return (struct typed_buffer *)vm_value_as_cell(object->internal);
}

/* Reports the bytes of an ArrayBuffer, a typed array or a DataView (for the host's own users). */
int
js_typed_bytes(
	vm_value value,
	unsigned char **bytes,
	size_t *length)
{
	struct typed_view *view;
	struct typed_buffer *buffer;
	struct vm_object *object;

	view = typed_view_of(value);
	if (view != NULL) {
		*bytes = view->buffer->data + view->offset;
		*length = (size_t)view->length * typed_sizes[view->type];
		return 1;
	}
	buffer = typed_buffer_of(value);
	if (buffer != NULL) {
		*bytes = buffer->data;
		*length = buffer->length;
		return 1;
	}
	if (vm_value_is_object(value)) {
		object = (struct vm_object *)vm_value_as_cell(value);
		if (object->kind == VM_KIND_DATA_VIEW) {
			view = (struct typed_view *)vm_value_as_cell(object->internal);
			*bytes = view->buffer->data + view->offset;
			*length = view->length;
			return 1;
		}
	}
	return 0;
}

/* The native property operations. */

static int
typed_index(
	const struct typed_view *view,
	vm_value key,
	uint32_t *index)
{
	if (!vm_value_is_int32(key))
		return 0;
	if (vm_value_as_int32(key) < 0)
		return 0;
	*index = (uint32_t)vm_value_as_int32(key);
	return *index < view->length;
}

static int
typed_get_own(
	struct vm_object *object,
	vm_value key,
	struct vm_property *property)
{
	struct typed_view *view;
	uint32_t index;
	int error;

	view = (struct typed_view *)vm_value_as_cell(object->internal);
	if (!typed_index(view, key, &index))
		return 0;
	error = typed_load(view->heap, view->buffer->data + view->offset + (size_t)index * typed_sizes[view->type], (int)view->type, &property->temporary);
	if (error != 0)
		return -error;
	property->holder = object;
	property->value = &property->temporary;
	property->attributes = VM_PROPERTY_DEFAULT;
	return 1;
}

static int
typed_own_keys(
	struct vm_heap *heap,
	struct vm_object *object,
	struct wb_vector *keys)
{
	struct typed_view *view;
	vm_value key;
	uint32_t index;
	int error;

	UNUSED_PARAMETER(heap);
	view = (struct typed_view *)vm_value_as_cell(object->internal);
	for (index = 0; index < view->length; index++) {
		key = vm_value_int32((int32_t)index);
		error = wb_vector_push(keys, &key);
		if (error != 0)
			return error;
	}
	return 0;
}

/* Applies immutable integer-index policy to normalized primitive descriptors without running script. */
static int
typed_define(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	const struct vm_descriptor *descriptor,
	int *handled,
	int *done)
{
	struct typed_view *view;
	struct vm_bigint *bigint;
	uint32_t index;
	uint64_t big;
	double number;
	int canonical;
	int valid;
	int is_number;
	int status;

	UNUSED_PARAMETER(realm);

	/* Realm-owned callers perform conversion before entering this immutable heap policy. */
	*handled = 0;
	*done = 0;
	view = (struct typed_view *)vm_value_as_cell(object->internal);
	status = typed_numeric_index(view, key, &canonical, &valid, &index);
	if (status != 0)
		return status;
	if (!canonical)
		return 0;
	*handled = 1;
	if (!valid)
		return 0;

	/* Integer-index descriptors keep their required configurable, enumerable and writable attributes. */
	if ((descriptor->has & VM_HAS_CONFIGURABLE) != 0U && (descriptor->attributes & VM_PROPERTY_CONFIGURABLE) == 0U)
		return 0;
	if ((descriptor->has & VM_HAS_ENUMERABLE) != 0U && (descriptor->attributes & VM_PROPERTY_ENUMERABLE) == 0U)
		return 0;
	if ((descriptor->has & (VM_HAS_GET | VM_HAS_SET)) != 0U)
		return 0;
	if ((descriptor->has & VM_HAS_WRITABLE) != 0U && (descriptor->attributes & VM_PROPERTY_WRITABLE) == 0U)
		return 0;
	if ((descriptor->has & VM_HAS_VALUE) == 0U) {
		*done = 1;
		return 0;
	}

	/* Native storage accepts a normalized primitive of the view's content type and never invokes ToPrimitive. */
	number = 0.0;
	big = 0;
	if (view->type == TYPED_BIGINT64 || view->type == TYPED_BIGUINT64) {
		bigint = vm_value_as_bigint(descriptor->value);
		if (bigint == NULL)
			return EINVAL;
		big = vm_bigint_to_uint64(bigint);
	} else {
		is_number = vm_value_is_number(descriptor->value);
		if (!is_number)
			return EINVAL;
		number = vm_value_as_number(descriptor->value);
	}

	/* Existing fixed-view bounds and byte representation belong to the native storage implementation. */
	typed_store((int)view->type, view->buffer->data + view->offset + (size_t)index * typed_sizes[view->type], number, big);
	*done = 1;

	/* Succeeded: one already converted value was stored without executing script. */
	return 0;
}

static int
typed_delete(
	struct vm_heap *heap,
	struct vm_object *object,
	vm_value key,
	int *handled,
	int *deleted)
{
	struct typed_view *view;
	uint32_t index;

	UNUSED_PARAMETER(heap);
	view = (struct typed_view *)vm_value_as_cell(object->internal);
	*handled = 0;
	if (!typed_index(view, key, &index))
		return 0;
	*handled = 1;
	*deleted = 0;
	return 0;
}

/* Making buffers and views. */

static int
typed_make_buffer(
	struct vm_realm *realm,
	size_t length,
	struct vm_object *prototype,
	vm_value *result)
{
	struct typed_buffer *buffer;
	struct vm_object *object;

	if (length > TYPED_BYTES_MAX)
		return vm_throw_range_error(realm, "Array buffer allocation failed");
	buffer = vm_heap_alloc(realm->heap, &typed_buffer_type, sizeof(*buffer));
	if (buffer == NULL)
		return ENOMEM;
	buffer->length = length;
	buffer->data = calloc(length == 0 ? 1U : length, 1U);
	if (buffer->data == NULL)
		return ENOMEM;
	if (prototype == NULL)
		prototype = realm->intrinsics[VM_INTRINSIC_ARRAY_BUFFER_PROTOTYPE];
	object = vm_object_create(realm->heap, prototype);
	if (object == NULL)
		return ENOMEM;
	object->kind = VM_KIND_ARRAY_BUFFER;
	object->internal = vm_value_cell(buffer);
	*result = vm_value_cell(object);
	return 0;
}

/* Makes a typed array (or, with data_view, a DataView) over a buffer object. */
static int
typed_make_view(
	struct vm_realm *realm,
	int type,
	struct vm_object *prototype,
	vm_value buffer_object,
	uint32_t offset,
	uint32_t length,
	int data_view,
	vm_value *result)
{
	struct typed_view *view;
	struct vm_object *object;
	struct vm_cell *roots[4];
	unsigned index;
	unsigned registered;
	int status;

	/* A direct native caller need not retain the managed owner, prototype or fresh backing buffer. */
	*result = VM_VALUE_UNDEFINED;
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = NULL;
	if (prototype != NULL)
		roots[1] = &prototype->cell;
	roots[2] = vm_value_as_cell(buffer_object);
	roots[3] = NULL;
	registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* The new view remains unpublished until an actual object takes ownership of it. */
	view = vm_heap_alloc(realm->heap, &typed_view_type, sizeof(*view));
	if (view == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Keep the view and its backing object live while allocating the final wrapper. */
	roots[3] = &view->cell;
	view->buffer_object = buffer_object;
	view->buffer = typed_buffer_of(buffer_object);
	view->offset = offset;
	view->length = length;
	view->type = (uint32_t)type;
	view->heap = realm->heap;
	object = vm_object_create(realm->heap, prototype);
	if (object == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Native storage metadata is immutable and independent of user-modifiable prototype properties. */
	object->internal = vm_value_cell(view);
	if (data_view) {
		object->kind = VM_KIND_DATA_VIEW;
	} else {
		object->kind = VM_KIND_TYPED_ARRAY;
		object->native_operations = &typed_operations;
		if (type == TYPED_BIGINT64 || type == TYPED_BIGUINT64)
			object->flags |= VM_OBJECT_TYPED_BIGINT;
	}

	/* The wrapper now owns its view and buffer graph, with no remaining fallible operation before publication. */
	*result = vm_value_cell(object);
	status = 0;

cleanup:
	/* No factory root may escape its actual native frame. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve failed construction without publishing a partial view. */
	if (status != 0)
		return status;

	/* Succeeded: the completed typed object owns every collectible storage component. */
	return 0;
}

/* Makes a fresh typed array of a length with its own buffer. */
static int
typed_make_array(
	struct vm_realm *realm,
	int type,
	struct vm_object *prototype,
	size_t length,
	vm_value *result)
{
	vm_value buffer;
	int status;

	if (length > TYPED_BYTES_MAX / typed_sizes[type])
		return vm_throw_range_error(realm, "Invalid typed array length");
	status = typed_make_buffer(realm, length * typed_sizes[type], NULL, &buffer);
	if (status != 0)
		return status;
	return typed_make_view(realm, type, prototype, buffer, 0, (uint32_t)length, 0, result);
}

/* Reads an index argument (ToIndex): an integer from 0 up. */
static int
typed_to_index(
	struct vm_realm *realm,
	vm_value value,
	double *index,
	int *thrown)
{
	int status;

	*thrown = 0;
	status = js_builtin_integer(realm, value, index);
	if (status != 0)
		return status;
	if (*index < 0.0 || *index > (double)TYPED_BYTES_MAX) {
		*thrown = 1;
		return vm_throw_range_error(realm, "Invalid length");
	}
	return 0;
}

/* Finds the typed array a method's this value is, or throws. */
static int
typed_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct typed_view **view)
{
	*view = typed_view_of(this_value);
	if (*view != NULL)
		return 0;
	return vm_throw_type_error(realm, "this is not a typed array.");
}

/* A relative index argument clamped into [0, length]. */
static int
typed_relative(
	struct vm_realm *realm,
	vm_value value,
	double length,
	double fallback,
	double *result)
{
	double number;
	int status;

	if (value == VM_VALUE_UNDEFINED) {
		*result = fallback;
		return 0;
	}
	status = js_builtin_integer(realm, value, &number);
	if (status != 0)
		return status;
	if (number < 0.0)
		number += length;
	if (number < 0.0)
		number = 0.0;
	if (number > length)
		number = length;
	*result = number;
	return 0;
}

/* The common constructor of the typed arrays: new Type(length | typed array | buffer, offset, length | array-like | iterable). */
static int
typed_construct(
	struct vm_realm *realm,
	int type,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *prototype;
	struct typed_view *source;
	struct typed_buffer *buffer;
	vm_value first;
	vm_value element;
	vm_value method;
	vm_value list;
	vm_value constructor_array;
	vm_value from;
	vm_value created;
	vm_value arguments[1];
	double number;
	double offset;
	double length;
	uint32_t index;
	uint32_t size;
	uint32_t total;
	int thrown;
	int status;

	/* The prototype from new.target (or the type's own). */
	status = vm_construct_prototype(realm, realm->new_target, realm->intrinsics[VM_INTRINSIC_TYPED_PROTOTYPE_0 + type], &prototype);
	if (status != 0)
		return status;
	size = typed_sizes[type];
	first = js_argument(args, count, 0);

	/* No argument or a number: a zeroed array. */
	if (!vm_value_is_object(first)) {
		status = typed_to_index(realm, first == VM_VALUE_UNDEFINED ? vm_value_int32(0) : first, &number, &thrown);
		if (status != 0)
			return status;
		return typed_make_array(realm, type, prototype, (size_t)number, result);
	}

	/* A buffer: a view of it. */
	buffer = typed_buffer_of(first);
	if (buffer != NULL) {
		status = typed_to_index(realm, js_argument(args, count, 1), &offset, &thrown);
		if (status != 0)
			return status;
		if (fmod(offset, (double)size) != 0.0)
			return vm_throw_range_error(realm, "start offset of the typed array should be a multiple of the element size");
		if (offset > (double)buffer->length)
			return vm_throw_range_error(realm, "Start offset is outside the bounds of the buffer");
		if (js_argument(args, count, 2) == VM_VALUE_UNDEFINED) {
			if (fmod((double)buffer->length - offset, (double)size) != 0.0)
				return vm_throw_range_error(realm, "byte length of the typed array should be a multiple of the element size");
			length = ((double)buffer->length - offset) / (double)size;
		} else {
			status = typed_to_index(realm, js_argument(args, count, 2), &length, &thrown);
			if (status != 0)
				return status;
			if (offset + length * (double)size > (double)buffer->length)
				return vm_throw_range_error(realm, "Invalid typed array length");
		}
		return typed_make_view(realm, type, prototype, first, (uint32_t)offset, (uint32_t)length, 0, result);
	}

	/* Another typed array: a converted copy. */
	source = typed_view_of(first);
	if (source != NULL) {
		status = typed_make_array(realm, type, prototype, source->length, &created);
		if (status != 0)
			return status;
		for (index = 0; index < source->length; index++) {
			status = typed_load(realm->heap, source->buffer->data + source->offset + (size_t)index * typed_sizes[source->type], (int)source->type, &element);
			if (status != 0)
				return status;
			status = vm_set(realm, created, vm_value_int32((int32_t)index), element, 1);
			if (status != 0)
				return status;
		}
		*result = created;
		return 0;
	}

	/* An iterable that is not an array: Array.from first. */
	list = first;
	status = vm_get_method(realm, first, vm_symbol_key(realm, VM_SYMBOL_ITERATOR), &method);
	if (status != 0)
		return status;
	if (method != VM_VALUE_UNDEFINED && !(((struct vm_object *)vm_value_as_cell(first))->flags & VM_OBJECT_ARRAY)) {
		status = vm_get(realm, vm_value_cell(realm->global), vm_key_from_ascii(realm->heap, "Array"), &constructor_array);
		if (status != 0)
			return status;
		status = vm_get(realm, constructor_array, vm_key_from_ascii(realm->heap, "from"), &from);
		if (status != 0)
			return status;
		arguments[0] = first;
		status = vm_call(realm, from, constructor_array, arguments, 1, &list);
		if (status != 0)
			return status;
	}

	/* An array-like: its elements converted. */
	status = js_builtin_length(realm, list, &total);
	if (status != 0)
		return status;
	status = typed_make_array(realm, type, prototype, total, &created);
	if (status != 0)
		return status;
	for (index = 0; index < total; index++) {
		status = vm_get(realm, list, vm_value_int32((int32_t)index), &element);
		if (status != 0)
			return status;
		status = vm_set(realm, created, vm_value_int32((int32_t)index), element, 1);
		if (status != 0)
			return status;
	}
	*result = created;
	return 0;
}

/* One constructor function per element type. */
#define TYPED_CONSTRUCTOR(n) \
	static int typed_call_##n(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result) \
	{ \
		UNUSED_PARAMETER(this_value); \
		UNUSED_PARAMETER(args); \
		UNUSED_PARAMETER(count); \
		UNUSED_PARAMETER(result); \
		return vm_throw_type_error(realm, "Constructor " #n " requires 'new'"); \
	} \
	static int typed_new_##n(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result) \
	{ \
		UNUSED_PARAMETER(this_value); \
		return typed_construct(realm, n, args, count, result); \
	}

TYPED_CONSTRUCTOR(0)
TYPED_CONSTRUCTOR(1)
TYPED_CONSTRUCTOR(2)
TYPED_CONSTRUCTOR(3)
TYPED_CONSTRUCTOR(4)
TYPED_CONSTRUCTOR(5)
TYPED_CONSTRUCTOR(6)
TYPED_CONSTRUCTOR(7)
TYPED_CONSTRUCTOR(8)
TYPED_CONSTRUCTOR(9)
TYPED_CONSTRUCTOR(10)

static const vm_native typed_calls[TYPED_TYPES] = {
	typed_call_0, typed_call_1, typed_call_2, typed_call_3, typed_call_4, typed_call_5,
	typed_call_6, typed_call_7, typed_call_8, typed_call_9, typed_call_10};
static const vm_native typed_news[TYPED_TYPES] = {
	typed_new_0, typed_new_1, typed_new_2, typed_new_3, typed_new_4, typed_new_5,
	typed_new_6, typed_new_7, typed_new_8, typed_new_9, typed_new_10};

/* The abstract %TypedArray%: neither callable nor constructible. */
static int
typed_abstract(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(result);
	return vm_throw_type_error(realm, "Abstract class TypedArray not directly constructable");
}

/* The typed array's accessors. */
static int
typed_buffer_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct typed_view *view;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = typed_this(realm, this_value, &view);
	if (status != 0)
		return status;
	*result = view->buffer_object;
	return 0;
}

static int
typed_length_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct typed_view *view;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = typed_this(realm, this_value, &view);
	if (status != 0)
		return status;
	*result = vm_value_number((double)view->length);
	return 0;
}

static int
typed_byte_length_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct typed_view *view;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = typed_this(realm, this_value, &view);
	if (status != 0)
		return status;
	*result = vm_value_number((double)view->length * typed_sizes[view->type]);
	return 0;
}

static int
typed_byte_offset_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct typed_view *view;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = typed_this(realm, this_value, &view);
	if (status != 0)
		return status;
	*result = vm_value_number((double)view->offset);
	return 0;
}

/* The prototype of a view's type. */
static struct vm_object *
typed_prototype_of(
	struct vm_realm *realm,
	const struct typed_view *view)
{
	return realm->intrinsics[VM_INTRINSIC_TYPED_PROTOTYPE_0 + view->type];
}

/* %TypedArray%.prototype.subarray(begin, end): a view of the same bytes. */
static int
typed_subarray(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct typed_view *view;
	double begin;
	double end;
	int status;

	status = typed_this(realm, this_value, &view);
	if (status != 0)
		return status;
	status = typed_relative(realm, js_argument(args, count, 0), (double)view->length, 0.0, &begin);
	if (status != 0)
		return status;
	status = typed_relative(realm, js_argument(args, count, 1), (double)view->length, (double)view->length, &end);
	if (status != 0)
		return status;
	if (end < begin)
		end = begin;
	return typed_make_view(realm, (int)view->type, typed_prototype_of(realm, view), view->buffer_object,
		view->offset + (uint32_t)begin * typed_sizes[view->type], (uint32_t)(end - begin), 0, result);
}

/* %TypedArray%.prototype.slice(begin, end): a copy. */
static int
typed_slice(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct typed_view *view;
	struct typed_view *copy;
	double begin;
	double end;
	int status;

	status = typed_this(realm, this_value, &view);
	if (status != 0)
		return status;
	status = typed_relative(realm, js_argument(args, count, 0), (double)view->length, 0.0, &begin);
	if (status != 0)
		return status;
	status = typed_relative(realm, js_argument(args, count, 1), (double)view->length, (double)view->length, &end);
	if (status != 0)
		return status;
	if (end < begin)
		end = begin;
	status = typed_make_array(realm, (int)view->type, typed_prototype_of(realm, view), (size_t)(end - begin), result);
	if (status != 0)
		return status;
	copy = typed_view_of(*result);
	memcpy(copy->buffer->data, view->buffer->data + view->offset + (size_t)begin * typed_sizes[view->type], (size_t)(end - begin) * typed_sizes[view->type]);
	return 0;
}

/* %TypedArray%.prototype.set(source, offset): copies an array-like or a typed array in. */
static int
typed_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct typed_view *view;
	struct typed_view *source;
	vm_value element;
	vm_value list;
	double offset;
	uint32_t total;
	uint32_t index;
	int thrown;
	int status;

	status = typed_this(realm, this_value, &view);
	if (status != 0)
		return status;
	*result = VM_VALUE_UNDEFINED;
	status = typed_to_index(realm, js_argument(args, count, 1), &offset, &thrown);
	if (status != 0)
		return status;
	list = js_argument(args, count, 0);
	source = typed_view_of(list);
	if (source != NULL) {
		total = source->length;
	} else {
		status = vm_to_object(realm, list, &list);
		if (status != 0)
			return status;
		status = js_builtin_length(realm, list, &total);
		if (status != 0)
			return status;
	}
	if (offset + (double)total > (double)view->length)
		return vm_throw_range_error(realm, "offset is out of bounds");
	for (index = 0; index < total; index++) {
		if (source != NULL) {
			status = typed_load(realm->heap, source->buffer->data + source->offset + (size_t)index * typed_sizes[source->type], (int)source->type, &element);
		} else {
			status = vm_get(realm, list, vm_value_int32((int32_t)index), &element);
		}
		if (status != 0)
			return status;
		status = vm_set(realm, this_value, vm_value_int32((int32_t)((uint32_t)offset + index)), element, 1);
		if (status != 0)
			return status;
	}
	return 0;
}

/* ArrayBuffer. */

static int
buffer_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(result);
	return vm_throw_type_error(realm, "Constructor ArrayBuffer requires 'new'");
}

static int
buffer_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct vm_object *prototype;
	double length;
	int thrown;
	int status;

	UNUSED_PARAMETER(this_value);
	status = vm_construct_prototype(realm, realm->new_target, realm->intrinsics[VM_INTRINSIC_ARRAY_BUFFER_PROTOTYPE], &prototype);
	if (status != 0)
		return status;
	status = typed_to_index(realm, js_argument(args, count, 0), &length, &thrown);
	if (status != 0)
		return status;
	return typed_make_buffer(realm, (size_t)length, prototype, result);
}

static int
buffer_byte_length(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct typed_buffer *buffer;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	buffer = typed_buffer_of(this_value);
	if (buffer == NULL)
		return vm_throw_type_error(realm, "Receiver is not an ArrayBuffer");
	*result = vm_value_number((double)buffer->length);
	return 0;
}

static int
buffer_slice(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct typed_buffer *buffer;
	struct typed_buffer *copy;
	double begin;
	double end;
	int status;

	buffer = typed_buffer_of(this_value);
	if (buffer == NULL)
		return vm_throw_type_error(realm, "Receiver is not an ArrayBuffer");
	status = typed_relative(realm, js_argument(args, count, 0), (double)buffer->length, 0.0, &begin);
	if (status != 0)
		return status;
	status = typed_relative(realm, js_argument(args, count, 1), (double)buffer->length, (double)buffer->length, &end);
	if (status != 0)
		return status;
	if (end < begin)
		end = begin;
	status = typed_make_buffer(realm, (size_t)(end - begin), NULL, result);
	if (status != 0)
		return status;
	copy = typed_buffer_of(*result);
	memcpy(copy->data, buffer->data + (size_t)begin, (size_t)(end - begin));
	return 0;
}

static int
buffer_is_view(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	unsigned char *bytes;
	size_t length;

	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(this_value);
	*result = VM_VALUE_FALSE;
	if (typed_buffer_of(js_argument(args, count, 0)) == NULL && js_typed_bytes(js_argument(args, count, 0), &bytes, &length))
		*result = VM_VALUE_TRUE;
	return 0;
}

/* DataView. */

static int
view_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(result);
	return vm_throw_type_error(realm, "Constructor DataView requires 'new'");
}

static int
view_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	struct vm_object *prototype;
	struct typed_buffer *buffer;
	double offset;
	double length;
	int thrown;
	int status;

	UNUSED_PARAMETER(this_value);
	status = vm_construct_prototype(realm, realm->new_target, realm->intrinsics[VM_INTRINSIC_DATA_VIEW_PROTOTYPE], &prototype);
	if (status != 0)
		return status;
	buffer = typed_buffer_of(js_argument(args, count, 0));
	if (buffer == NULL)
		return vm_throw_type_error(realm, "First argument to DataView constructor must be an ArrayBuffer");
	status = typed_to_index(realm, js_argument(args, count, 1), &offset, &thrown);
	if (status != 0)
		return status;
	if (offset > (double)buffer->length)
		return vm_throw_range_error(realm, "Start offset is outside the bounds of the buffer");
	length = (double)buffer->length - offset;
	if (js_argument(args, count, 2) != VM_VALUE_UNDEFINED) {
		status = typed_to_index(realm, js_argument(args, count, 2), &length, &thrown);
		if (status != 0)
			return status;
		if (offset + length > (double)buffer->length)
			return vm_throw_range_error(realm, "Invalid DataView length");
	}
	return typed_make_view(realm, 0, prototype, js_argument(args, count, 0), (uint32_t)offset, (uint32_t)length, 1, result);
}

/* Finds a DataView's view. */
static int
view_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct typed_view **view)
{
	struct vm_object *object;

	if (vm_value_is_object(this_value)) {
		object = (struct vm_object *)vm_value_as_cell(this_value);
		if (object->kind == VM_KIND_DATA_VIEW) {
			*view = (struct typed_view *)vm_value_as_cell(object->internal);
			return 0;
		}
	}
	return vm_throw_type_error(realm, "Receiver is not a DataView");
}

static int
view_accessor(struct vm_realm *realm, vm_value this_value, int which, vm_value *result)
{
	struct typed_view *view;
	int status;

	status = view_this(realm, this_value, &view);
	if (status != 0)
		return status;
	if (which == 0)
		*result = view->buffer_object;
	else if (which == 1)
		*result = vm_value_number((double)view->length);
	else
		*result = vm_value_number((double)view->offset);
	return 0;
}

static int
view_buffer(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	return view_accessor(realm, this_value, 0, result);
}

static int
view_byte_length(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	return view_accessor(realm, this_value, 1, result);
}

static int
view_byte_offset(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	return view_accessor(realm, this_value, 2, result);
}

/* Reads or writes one element of a DataView: (byteOffset, littleEndian) or (byteOffset, value, littleEndian). */
static int
view_access(
	struct vm_realm *realm,
	vm_value this_value,
	int type,
	int write,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct typed_view *view;
	unsigned char scratch[8];
	unsigned char flipped[8];
	double offset;
	double number;
	uint64_t big;
	unsigned size;
	unsigned i;
	int little;
	int thrown;
	int status;

	status = view_this(realm, this_value, &view);
	if (status != 0)
		return status;
	size = typed_sizes[type];
	status = typed_to_index(realm, js_argument(args, count, 0), &offset, &thrown);
	if (status != 0)
		return status;
	if (offset + (double)size > (double)view->length)
		return vm_throw_range_error(realm, "Offset is outside the bounds of the DataView");
	little = vm_to_boolean(js_argument(args, count, write ? 2U : 1U));
	if (write) {
		status = typed_convert(realm, type, js_argument(args, count, 1), &number, &big);
		if (status != 0)
			return status;
		typed_store(type, scratch, number, big);
		for (i = 0; i < size; i++)
			flipped[i] = little ? scratch[i] : scratch[size - 1U - i];
		memcpy(view->buffer->data + view->offset + (size_t)offset, flipped, size);
		*result = VM_VALUE_UNDEFINED;
		return 0;
	}
	memcpy(scratch, view->buffer->data + view->offset + (size_t)offset, size);
	for (i = 0; i < size; i++)
		flipped[i] = little ? scratch[i] : scratch[size - 1U - i];
	return typed_load(realm->heap, flipped, type, result);
}

/* One getter and one setter per element type of DataView (there is no Uint8Clamped one). */
#define VIEW_METHODS(n, name) \
	static int view_get_##name(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result) \
	{ \
		return view_access(realm, this_value, n, 0, args, count, result); \
	} \
	static int view_set_##name(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result) \
	{ \
		return view_access(realm, this_value, n, 1, args, count, result); \
	}

VIEW_METHODS(TYPED_INT8, int8)
VIEW_METHODS(TYPED_UINT8, uint8)
VIEW_METHODS(TYPED_INT16, int16)
VIEW_METHODS(TYPED_UINT16, uint16)
VIEW_METHODS(TYPED_INT32, int32)
VIEW_METHODS(TYPED_UINT32, uint32)
VIEW_METHODS(TYPED_FLOAT32, float32)
VIEW_METHODS(TYPED_FLOAT64, float64)
VIEW_METHODS(TYPED_BIGINT64, bigint64)
VIEW_METHODS(TYPED_BIGUINT64, biguint64)

/* The random bytes crypto.getRandomValues fills (from the system's source). */
int
js_typed_random_fill(
	vm_value value)
{
	unsigned char *bytes;
	size_t length;
	int error;

	if (!js_typed_bytes(value, &bytes, &length))
		return EINVAL;
	error = wb_os_random(bytes, length);
	if (error != 0)
		return EIO;
	return 0;
}

/* Installs ArrayBuffer, the typed arrays and DataView. */
int
js_builtin_install_typed(
	struct vm_realm *realm)
{
	struct vm_function *constructor;
	struct vm_function *base;
	struct vm_function *array_buffer;
	struct vm_function *data_view;
	struct vm_object *prototype;
	struct vm_object *base_prototype;
	struct vm_string *text;
	vm_value source;
	vm_value function;
	vm_value arguments[3];
	vm_value ignored;
	int type;
	int status;
	int error;

	/* ArrayBuffer. */
	prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (prototype == NULL)
		return ENOMEM;
	realm->intrinsics[VM_INTRINSIC_ARRAY_BUFFER_PROTOTYPE] = prototype;
	error = js_builtin_constructor(realm, "ArrayBuffer", 1, buffer_call, buffer_construct, prototype, &array_buffer);
	if (error != 0)
		return error;
	error = js_builtin_accessor(realm, prototype, "byteLength", buffer_byte_length, NULL);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "slice", 2, buffer_slice);
	if (error == 0)
		error = js_builtin_method(realm, &array_buffer->object, "isView", 1, buffer_is_view);
	if (error == 0)
		error = js_builtin_tag(realm, prototype, "ArrayBuffer");
	if (error != 0)
		return error;

	/* %TypedArray% and its prototype. */
	base_prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (base_prototype == NULL)
		return ENOMEM;
	realm->intrinsics[VM_INTRINSIC_TYPED_ARRAY_PROTOTYPE] = base_prototype;
	error = js_builtin_constructor(realm, "TypedArray", 0, typed_abstract, typed_abstract, base_prototype, &base);
	if (error != 0)
		return error;
	error = js_builtin_accessor(realm, base_prototype, "buffer", typed_buffer_get, NULL);
	if (error == 0)
		error = js_builtin_accessor(realm, base_prototype, "byteLength", typed_byte_length_get, NULL);
	if (error == 0)
		error = js_builtin_accessor(realm, base_prototype, "byteOffset", typed_byte_offset_get, NULL);
	if (error == 0)
		error = js_builtin_accessor(realm, base_prototype, "length", typed_length_get, NULL);
	if (error == 0)
		error = js_builtin_method(realm, base_prototype, "subarray", 2, typed_subarray);
	if (error == 0)
		error = js_builtin_method(realm, base_prototype, "slice", 2, typed_slice);
	if (error == 0)
		error = js_builtin_method(realm, base_prototype, "set", 1, typed_set);
	if (error != 0)
		return error;

	/* The concrete types: each a constructor whose prototype chain leads to %TypedArray%. */
	for (type = 0; type < TYPED_TYPES; type++) {
		prototype = vm_object_create(realm->heap, base_prototype);
		if (prototype == NULL)
			return ENOMEM;
		realm->intrinsics[VM_INTRINSIC_TYPED_PROTOTYPE_0 + type] = prototype;
		error = js_builtin_constructor(realm, typed_names[type], 3, typed_calls[type], typed_news[type], prototype, &constructor);
		if (error != 0)
			return error;
		constructor->object.prototype = &base->object;
		error = js_builtin_value(realm, &constructor->object, "BYTES_PER_ELEMENT", vm_value_int32((int32_t)typed_sizes[type]), 0);
		if (error == 0)
			error = js_builtin_value(realm, prototype, "BYTES_PER_ELEMENT", vm_value_int32((int32_t)typed_sizes[type]), 0);
		if (error == 0)
			error = js_builtin_tag(realm, prototype, typed_names[type]);
		if (error != 0)
			return error;
	}

	/* DataView. */
	prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (prototype == NULL)
		return ENOMEM;
	realm->intrinsics[VM_INTRINSIC_DATA_VIEW_PROTOTYPE] = prototype;
	error = js_builtin_constructor(realm, "DataView", 1, view_call, view_construct, prototype, &data_view);
	if (error != 0)
		return error;
	error = js_builtin_accessor(realm, prototype, "buffer", view_buffer, NULL);
	if (error == 0)
		error = js_builtin_accessor(realm, prototype, "byteLength", view_byte_length, NULL);
	if (error == 0)
		error = js_builtin_accessor(realm, prototype, "byteOffset", view_byte_offset, NULL);
	if (error == 0)
		error = js_builtin_tag(realm, prototype, "DataView");
	if (error == 0)
		error = js_builtin_method(realm, prototype, "getInt8", 1, view_get_int8);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "setInt8", 2, view_set_int8);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "getUint8", 1, view_get_uint8);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "setUint8", 2, view_set_uint8);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "getInt16", 1, view_get_int16);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "setInt16", 2, view_set_int16);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "getUint16", 1, view_get_uint16);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "setUint16", 2, view_set_uint16);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "getInt32", 1, view_get_int32);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "setInt32", 2, view_set_int32);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "getUint32", 1, view_get_uint32);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "setUint32", 2, view_set_uint32);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "getFloat32", 1, view_get_float32);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "setFloat32", 2, view_set_float32);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "getFloat64", 1, view_get_float64);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "setFloat64", 2, view_set_float64);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "getBigInt64", 1, view_get_bigint64);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "setBigInt64", 2, view_set_bigint64);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "getBigUint64", 1, view_get_biguint64);
	if (error == 0)
		error = js_builtin_method(realm, prototype, "setBigUint64", 2, view_set_biguint64);
	if (error != 0)
		return error;

	/* The page-language half, run once over %TypedArray%.prototype. */
	source = VM_VALUE_UNDEFINED;
	text = vm_string_from_utf8(realm->heap, typed_prelude, sizeof(typed_prelude) - 1U);
	if (text == NULL)
		return ENOMEM;
	source = vm_value_cell(text);
	status = js_builtin_evaluate(realm, (const struct vm_string *)vm_value_as_cell(source), 0, &function);
	if (status != 0)
		return status;
	arguments[0] = vm_value_cell(base_prototype);
	arguments[1] = vm_value_cell(base);
	arguments[2] = VM_VALUE_UNDEFINED;
	status = vm_call(realm, function, VM_VALUE_UNDEFINED, arguments, 2, &ignored);
	if (status != 0)
		return status;

	/* WeakRef and FinalizationRegistry on the global object. */
	text = vm_string_from_utf8(realm->heap, typed_weak_prelude, sizeof(typed_weak_prelude) - 1U);
	if (text == NULL)
		return ENOMEM;
	status = js_builtin_evaluate(realm, text, 0, &function);
	if (status != 0)
		return status;
	arguments[0] = vm_value_cell(realm->global);
	status = vm_call(realm, function, VM_VALUE_UNDEFINED, arguments, 1, &ignored);
	if (status != 0)
		return status;

	/* Succeeded: the buffers and views are installed. */
	return 0;
}

/* Classifies canonical numeric keys and valid fixed-view indices without script or VM allocation. */
static int
typed_numeric_index(
	const struct typed_view *view,
	vm_value key,
	int *canonical,
	int *valid,
	uint32_t *index)
{
	double number;
	double whole;
	int negative_zero;
	int status;

	/* Noncanonical names continue into ordinary property storage. */
	*valid = 0;
	*index = 0;
	status = vm_canonical_numeric_index(key, &number, canonical);
	if (status != 0)
		return status;
	if (!*canonical)
		return 0;
	if (number != number)
		return 0;

	/* Negative zero is an invalid integer index even though its numeric value equals zero. */
	negative_zero = 0;
	if (number == 0.0)
		negative_zero = signbit(number);
	if (negative_zero)
		return 0;
	if (number < 0.0 || number >= (double)view->length)
		return 0;

	/* Bounds exclude infinities before integral conversion. */
	whole = floor(number);
	if (whole != number)
		return 0;
	*index = (uint32_t)number;
	*valid = 1;

	/* Succeeded: the key is either ordinary, invalid numeric, or a valid fixed-view index. */
	return 0;
}
