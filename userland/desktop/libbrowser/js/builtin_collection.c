/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyed collections (ws074-p087): Map, Set, WeakMap and WeakSet.
 *
 * A collection is an object of its kind whose internal value is a cell of
 * its table: the entries in the order they were added (a removed one keeps
 * its place with the empty value as its key, so an iterator's index stays
 * right) and a hash index of chains over them.  Keys compare by
 * SameValueZero: a number is kept in one form (an int32 when it is one,
 * -0 as 0), a string by its characters, anything else by identity.  The
 * removed entries are dropped when the table grows, but only while no
 * iterator or forEach walks it (holds counts them; an iterator left before
 * its end keeps the table from being compacted, which costs only memory).
 *
 * Not in this pass: a WeakMap's and a WeakSet's weakness (they hold their
 * keys strongly: the collector has no ephemerons), Map.groupBy and the Set
 * methods of ES2025 (union and the others).
 */

#include "js/builtin.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The end of a chain of the hash index. */
#define COLLECTION_NONE		0xffffffffU

/* The first number of entries a table has room for. */
#define COLLECTION_FIRST	8U

/*
 * One entry of a table: its key (the empty value once removed), its value
 * (a Set's is the key), its hash and the next entry of its chain.
 */
struct collection_entry {
	vm_value key;
	vm_value value;
	uint32_t hash;
	uint32_t chain;
};

/*
 * The table of a collection: the entries (count used, removed ones
 * included, of capacity; malloc'd), the chains' heads (bucket_count, a
 * power of two; malloc'd), how many entries are live, and how many walks
 * hold it (while any does, the entries keep their places).
 */
struct collection_table {
	struct vm_cell cell;
	struct collection_entry *entries;
	uint32_t *buckets;
	uint32_t count;
	uint32_t capacity;
	uint32_t bucket_count;
	uint32_t size;
	uint32_t holds;
	uint32_t reserved;
};

static void collection_trace(struct vm_heap *heap, struct vm_cell *cell);
static void collection_finalize(struct vm_heap *heap, struct vm_cell *cell);
static int collection_install_one(struct vm_realm *realm, const char *name, uint32_t kind, struct vm_object **prototype);
static int collection_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_fill(struct vm_realm *realm, vm_value collection, uint32_t kind, vm_value iterable);
static int collection_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_add(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_has(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_delete(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_clear(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_size(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_for_each(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_keys(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_values(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_entries(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int collection_iterator(struct vm_realm *realm, vm_value this_value, uint32_t iterate, vm_value *result);
static int collection_this(struct vm_realm *realm, vm_value value, uint32_t kinds, struct collection_table **table);
static uint32_t collection_bits(const struct vm_realm *realm);
static int collection_define(struct vm_realm *realm, struct vm_object *object, const char *name, unsigned length, vm_native native, uint32_t bit, struct vm_function **function);
static int collection_weak_key(vm_value key);
static vm_value collection_normalize(vm_value key);
static uint32_t collection_hash(vm_value key);
static int collection_same(vm_value left, vm_value right);
static uint32_t collection_find(const struct collection_table *table, vm_value key, uint32_t hash);
static int collection_put(struct collection_table *table, vm_value key, vm_value value);
static int collection_remove(struct collection_table *table, vm_value key);
static int collection_grow(struct collection_table *table);
static int collection_rehash(struct collection_table *table, uint32_t bucket_count);

/* The bits of the kinds collection_this accepts. */
#define COLLECTION_MAP_BIT	0x1U
#define COLLECTION_SET_BIT	0x2U
#define COLLECTION_WEAK_MAP_BIT	0x4U
#define COLLECTION_WEAK_SET_BIT	0x8U

/* The cell type of a collection's table: it holds its keys and values. */
static const struct vm_cell_type collection_type = {
	"collection", collection_trace, collection_finalize
};

/* The methods of each collection's prototype (the kinds whose this they take are the bits). */
struct collection_method {
	const char *name;
	unsigned length;
	vm_native native;
	uint32_t kinds;
};

/* The table of the prototypes' methods. */
static const struct collection_method collection_methods[] = {
	{ "get", 1, collection_get, COLLECTION_MAP_BIT | COLLECTION_WEAK_MAP_BIT },
	{ "set", 2, collection_set, COLLECTION_MAP_BIT | COLLECTION_WEAK_MAP_BIT },
	{ "add", 1, collection_add, COLLECTION_SET_BIT | COLLECTION_WEAK_SET_BIT },
	{ "has", 1, collection_has, COLLECTION_MAP_BIT | COLLECTION_SET_BIT | COLLECTION_WEAK_MAP_BIT | COLLECTION_WEAK_SET_BIT },
	{ "delete", 1, collection_delete, COLLECTION_MAP_BIT | COLLECTION_SET_BIT | COLLECTION_WEAK_MAP_BIT | COLLECTION_WEAK_SET_BIT },
	{ "clear", 0, collection_clear, COLLECTION_MAP_BIT | COLLECTION_SET_BIT },
	{ "forEach", 1, collection_for_each, COLLECTION_MAP_BIT | COLLECTION_SET_BIT },
	{ "keys", 0, collection_keys, COLLECTION_MAP_BIT },
	{ "values", 0, collection_values, COLLECTION_MAP_BIT },
	{ "entries", 0, collection_entries, COLLECTION_SET_BIT },
	{ NULL, 0, NULL, 0 }
};

/* The names of the four constructors, by kind less VM_KIND_MAP. */
static const char *const collection_names[4] = {
	"Map", "Set", "WeakMap", "WeakSet"
};

/*
 * Installs Map, Set, WeakMap and WeakSet: each constructor with its
 * prototype.
 */
int
js_builtin_install_collection(
	struct vm_realm *realm)
{
	struct vm_object *prototype;
	uint32_t kind;
	int error;

	/* Each of the four. */
	for (kind = VM_KIND_MAP; kind <= VM_KIND_WEAK_SET; kind++) {
		error = collection_install_one(realm, collection_names[kind - VM_KIND_MAP], kind, &prototype);
		if (error != 0)
			return error;
	}

	/* Succeeded: the collections are installed. */
	return 0;
}

/*
 * Takes the next live entry of a Map or a Set from an index on: its key
 * and value, and the index after it.  Returns 1, or 0 when none is left
 * (and the walk no longer holds the table).
 */
int
js_collection_step(
	vm_value collection,
	uint32_t *index,
	vm_value *key,
	vm_value *value)
{
	struct collection_table *table;
	struct collection_entry *entry;
	struct vm_object *object;

	/* The table, from the entry at the index. */
	object = (struct vm_object *)vm_value_as_cell(collection);
	table = (struct collection_table *)vm_value_as_cell(object->internal);
	while (*index < table->count) {
		entry = &table->entries[*index];
		(*index)++;

		/* A removed entry is passed over. */
		if (entry->key == VM_VALUE_EMPTY)
			continue;

		/* The live entry. */
		*key = entry->key;
		*value = entry->value;
		return 1;
	}

	/* The end: the walk is over. */
	if (table->holds > 0)
		table->holds--;
	return 0;
}

/* Marks the keys and values of a table. */
static void
collection_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct collection_table *table;
	uint32_t index;

	/* Each entry's key and value (a removed one has neither). */
	table = (struct collection_table *)cell;
	for (index = 0; index < table->count; index++) {
		vm_heap_mark_value(heap, table->entries[index].key);
		vm_heap_mark_value(heap, table->entries[index].value);
	}
}

/* Frees what a dead table allocated. */
static void
collection_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct collection_table *table;

	UNUSED_PARAMETER(heap);

	/* The entries and the chains' heads. */
	table = (struct collection_table *)cell;
	free(table->entries);
	free(table->buckets);
	table->entries = NULL;
	table->buckets = NULL;
}

/* Installs one collection: its prototype with its methods, and its constructor on the global object. */
static int
collection_install_one(
	struct vm_realm *realm,
	const char *name,
	uint32_t kind,
	struct vm_object **prototype)
{
	const struct collection_method *method;
	struct vm_function *constructor;
	struct vm_function *function;
	struct vm_accessor *accessor;
	struct vm_object *made;
	uint32_t bit;
	int error;

	/* The prototype and the constructor (whose data is its kind). */
	made = vm_object_create(realm->heap, realm->object_prototype);
	if (made == NULL)
		return ENOMEM;
	error = js_builtin_constructor(realm, name, 0, collection_call, collection_construct, made, &constructor);
	if (error != 0)
		return error;
	constructor->data = vm_value_int32((int32_t)kind);
	realm->intrinsics[VM_INTRINSIC_MAP_PROTOTYPE + (kind - VM_KIND_MAP)] = made;

	/* The methods of this kind, each knowing the kind its this must be (its data). */
	bit = 1U << (kind - VM_KIND_MAP);
	for (method = collection_methods; method->name != NULL; method++) {
		if ((method->kinds & bit) == 0U)
			continue;
		error = collection_define(realm, made, method->name, method->length, method->native, bit, &function);
		if (error != 0)
			return error;
	}

	/* Map and Set: size, the iterator (entries for a Map, values for a Set) and species. */
	if (kind == VM_KIND_MAP || kind == VM_KIND_SET) {
		error = js_builtin_function(realm, "get size", 0, collection_size, NULL, &function);
		if (error != 0)
			return error;
		function->data = vm_value_int32((int32_t)bit);
		accessor = vm_accessor_create(realm->heap, vm_value_cell(function), VM_VALUE_UNDEFINED);
		if (accessor == NULL)
			return ENOMEM;
		error = js_builtin_value(realm, made, "size", vm_value_cell(accessor), VM_PROPERTY_ACCESSOR | VM_PROPERTY_CONFIGURABLE);
		if (error != 0)
			return error;
		if (kind == VM_KIND_MAP) {
			error = collection_define(realm, made, "entries", 0, collection_entries, bit, &function);
		} else {
			error = collection_define(realm, made, "values", 0, collection_values, bit, &function);
			if (error == 0)
				error = js_builtin_value(realm, made, "keys", vm_value_cell(function), JS_BUILTIN_METHOD);
		}

		/* The iterator also under Symbol.iterator, and the constructor's species. */
		if (error == 0)
			error = js_builtin_symbol_value(realm, made, VM_SYMBOL_ITERATOR, vm_value_cell(function), JS_BUILTIN_METHOD);
		if (error == 0)
			error = js_builtin_species(realm, &constructor->object);
		if (error != 0)
			return error;
	}

	/* The tag. */
	error = js_builtin_tag(realm, made, name);
	if (error != 0)
		return error;

	/* Succeeded: the collection is installed. */
	*prototype = made;
	return 0;
}

/* Map() and the others without new: a TypeError. */
static int
collection_call(
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

	/* A TypeError. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_throw_type_error(realm, "Constructor requires 'new'");
	return status;
}

/*
 * new Map(iterable) and the others: an empty collection from new.target's
 * prototype, filled from the iterable (entries for a Map, values for a
 * Set) through its own set or add.
 */
static int
collection_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_table *table;
	struct vm_object *prototype;
	struct vm_object *object;
	struct vm_function *constructor;
	vm_value new_target;
	uint32_t kind;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The kind (the constructor's data) and new.target, read before any script runs. */
	*result = VM_VALUE_UNDEFINED;
	constructor = js_builtin_callee(realm);
	kind = (uint32_t)vm_value_as_int32(constructor->data);
	new_target = realm->new_target;

	/* The object from new.target's prototype, of the kind. */
	status = vm_construct_prototype(realm, new_target, realm->intrinsics[VM_INTRINSIC_MAP_PROTOTYPE + (kind - VM_KIND_MAP)], &prototype);
	if (status != 0)
		return status;
	object = vm_object_create(realm->heap, prototype);
	if (object == NULL)
		return ENOMEM;
	object->kind = kind;

	/* Its empty table. */
	table = vm_heap_alloc(realm->heap, &collection_type, sizeof(*table));
	if (table == NULL)
		return ENOMEM;
	object->internal = vm_value_cell(table);

	/* The iterable's values or entries. */
	status = collection_fill(realm, vm_value_cell(object), kind, js_argument(args, count, 0));
	if (status != 0)
		return status;

	/* Succeeded: the collection. */
	*result = vm_value_cell(object);
	return 0;
}

/*
 * Fills a new collection from an iterable (none for undefined and null):
 * each value (a Map's is an entry object whose 0 and 1 are the key and the
 * value) through the collection's own set or add; an exception closes the
 * iteration.
 */
static int
collection_fill(
	struct vm_realm *realm,
	vm_value collection,
	uint32_t kind,
	vm_value iterable)
{
	vm_value adder;
	vm_value iterator;
	vm_value item;
	vm_value pair[2];
	vm_value ignored;
	const char *name;
	unsigned count;
	int callable;
	int is_object;
	int done;
	int status;

	/* Nothing to add. */
	if (iterable == VM_VALUE_UNDEFINED || iterable == VM_VALUE_NULL)
		return 0;

	/* The collection's own adder. */
	name = "set";
	if (kind == VM_KIND_SET || kind == VM_KIND_WEAK_SET)
		name = "add";
	status = vm_get(realm, collection, vm_key_from_ascii(realm->heap, name), &adder);
	if (status != 0)
		return status;
	callable = vm_value_is_callable(adder);
	if (!callable) {
		status = vm_throw_type_error(realm, "The collection's adder is not a function");
		return status;
	}

	/* Each value of the iterable. */
	status = vm_iter_start(realm, iterable, &iterator);
	if (status != 0)
		return status;
	for (;;) {
		status = vm_iter_next(realm, iterator, &item, &done);
		if (status != 0 || done)
			return status;

		/* A Map's item must be an object with the key and the value. */
		pair[0] = item;
		count = 1;
		if (kind == VM_KIND_MAP || kind == VM_KIND_WEAK_MAP) {
			is_object = vm_value_is_object(item);
			if (!is_object) {
				status = vm_throw_type_error(realm, "Iterator value is not an entry object");
			} else {
				status = vm_get(realm, item, vm_value_int32(0), &pair[0]);
				if (status == 0)
					status = vm_get(realm, item, vm_value_int32(1), &pair[1]);
			}

			/* A Map's adder takes the key and the value. */
			count = 2;
		}

		/* The addition; whatever throws closes the iteration first. */
		if (status == 0)
			status = vm_call(realm, adder, collection, pair, count, &ignored);
		if (status == VM_THROWN) {
			vm_iter_close(realm, iterator, 1);
			return status;
		}

		/* Any other failure ends the filling. */
		if (status != 0)
			return status;
	}
}

/* Map.prototype.get(key) and WeakMap's: the value of the key, undefined without one. */
static int
collection_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_table *table;
	vm_value key;
	uint32_t found;
	int status;

	/* The table. */
	*result = VM_VALUE_UNDEFINED;
	status = collection_this(realm, this_value, collection_bits(realm), &table);
	if (status != 0)
		return status;

	/* The key's entry. */
	key = collection_normalize(js_argument(args, count, 0));
	found = collection_find(table, key, collection_hash(key));
	if (found != COLLECTION_NONE)
		*result = table->entries[found].value;

	/* Succeeded: the value or undefined. */
	return 0;
}

/* Map.prototype.set(key, value) and WeakMap's: the entry added or changed; this is the result. */
static int
collection_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_table *table;
	struct vm_object *object;
	vm_value key;
	int weak_key;
	int status;

	/* The table. */
	*result = VM_VALUE_UNDEFINED;
	status = collection_this(realm, this_value, collection_bits(realm), &table);
	if (status != 0)
		return status;

	/* A WeakMap's key must be one that can be held weakly. */
	key = js_argument(args, count, 0);
	weak_key = collection_weak_key(key);
	object = (struct vm_object *)vm_value_as_cell(this_value);
	if (object->kind == VM_KIND_WEAK_MAP && !weak_key) {
		status = vm_throw_type_error(realm, "Invalid value used as weak map key");
		return status;
	}

	/* The entry. */
	status = collection_put(table, collection_normalize(key), js_argument(args, count, 1));
	if (status != 0)
		return status;

	/* Succeeded: this. */
	*result = this_value;
	return 0;
}

/* Set.prototype.add(value) and WeakSet's: the value added unless present; this is the result. */
static int
collection_add(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_table *table;
	struct vm_object *object;
	vm_value key;
	int weak_key;
	int status;

	/* The table. */
	*result = VM_VALUE_UNDEFINED;
	status = collection_this(realm, this_value, collection_bits(realm), &table);
	if (status != 0)
		return status;

	/* A WeakSet's value must be one that can be held weakly. */
	key = js_argument(args, count, 0);
	weak_key = collection_weak_key(key);
	object = (struct vm_object *)vm_value_as_cell(this_value);
	if (object->kind == VM_KIND_WEAK_SET && !weak_key) {
		status = vm_throw_type_error(realm, "Invalid value used in weak set");
		return status;
	}

	/* The entry, whose value is its key. */
	key = collection_normalize(key);
	status = collection_put(table, key, key);
	if (status != 0)
		return status;

	/* Succeeded: this. */
	*result = this_value;
	return 0;
}

/* has(key) of all four: whether the key is present. */
static int
collection_has(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_table *table;
	vm_value key;
	uint32_t found;
	int status;

	/* The table (of any of the four). */
	*result = VM_VALUE_FALSE;
	status = collection_this(realm, this_value, collection_bits(realm), &table);
	if (status != 0)
		return status;

	/* The key's entry. */
	key = collection_normalize(js_argument(args, count, 0));
	found = collection_find(table, key, collection_hash(key));
	if (found != COLLECTION_NONE)
		*result = VM_VALUE_TRUE;

	/* Succeeded: whether it is there. */
	return 0;
}

/* delete(key) of all four: the key's entry removed; whether there was one. */
static int
collection_delete(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_table *table;
	int removed;
	int status;

	/* The table (of any of the four). */
	*result = VM_VALUE_FALSE;
	status = collection_this(realm, this_value, collection_bits(realm), &table);
	if (status != 0)
		return status;

	/* The removal. */
	removed = collection_remove(table, collection_normalize(js_argument(args, count, 0)));
	*result = vm_value_boolean(removed);

	/* Succeeded: whether it was there. */
	return 0;
}

/* clear() of Map and Set: every entry removed (a walk goes on with what is added after). */
static int
collection_clear(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_table *table;
	uint32_t index;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The table. */
	*result = VM_VALUE_UNDEFINED;
	status = collection_this(realm, this_value, collection_bits(realm), &table);
	if (status != 0)
		return status;

	/* Each entry removed in place, and the chains emptied. */
	for (index = 0; index < table->count; index++) {
		table->entries[index].key = VM_VALUE_EMPTY;
		table->entries[index].value = VM_VALUE_UNDEFINED;
	}

	/* No chain has an entry any more. */
	for (index = 0; index < table->bucket_count; index++)
		table->buckets[index] = COLLECTION_NONE;
	table->size = 0;

	/* Without a walk, the entries' places are free again. */
	if (table->holds == 0)
		table->count = 0;

	/* Succeeded: undefined. */
	return 0;
}

/* get size of Map and Set: the number of entries. */
static int
collection_size(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_table *table;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A Map or a Set. */
	*result = VM_VALUE_UNDEFINED;
	status = collection_this(realm, this_value, collection_bits(realm), &table);
	if (status != 0)
		return status;

	/* Succeeded: the count. */
	*result = vm_value_int32((int32_t)table->size);
	return 0;
}

/* forEach(callback, thisArg) of Map and Set: callback(value, key, collection) for each entry in order. */
static int
collection_for_each(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_table *table;
	vm_value callback;
	vm_value arguments[3];
	vm_value ignored;
	uint32_t index;
	int callable;
	int found;
	int status;

	/* The table and the callback. */
	*result = VM_VALUE_UNDEFINED;
	status = collection_this(realm, this_value, collection_bits(realm), &table);
	if (status != 0)
		return status;
	callback = js_argument(args, count, 0);
	callable = vm_value_is_callable(callback);
	if (!callable) {
		status = vm_throw_type_error(realm, "forEach callback is not a function");
		return status;
	}

	/* Each entry, the walk holding the table (entries added meanwhile come too). */
	table->holds++;
	index = 0;
	for (;;) {
		found = js_collection_step(this_value, &index, &arguments[1], &arguments[0]);
		if (!found)
			break;
		arguments[2] = this_value;
		status = vm_call(realm, callback, js_argument(args, count, 1), arguments, 3, &ignored);
		if (status != 0) {
			table->holds--;
			return status;
		}
	}

	/* Succeeded: undefined (the end of the walk released the table). */
	return 0;
}

/* Map.prototype.keys(): an iterator of the keys. */
static int
collection_keys(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The iterator. */
	status = collection_iterator(realm, this_value, JS_ITERATE_KEYS, result);
	return status;
}

/* values() of Map and Set (also Set's keys and Symbol.iterator): an iterator of the values. */
static int
collection_values(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The iterator. */
	status = collection_iterator(realm, this_value, JS_ITERATE_VALUES, result);
	return status;
}

/* entries() of Map and Set (also Map's Symbol.iterator): an iterator of [key, value]. */
static int
collection_entries(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The iterator. */
	status = collection_iterator(realm, this_value, JS_ITERATE_ENTRIES, result);
	return status;
}

/* Makes an iterator of a Map or a Set, which holds the table until it ends. */
static int
collection_iterator(
	struct vm_realm *realm,
	vm_value this_value,
	uint32_t iterate,
	vm_value *result)
{
	struct collection_table *table;
	struct vm_object *object;
	uint32_t family;
	int status;

	/* A Map or a Set. */
	*result = VM_VALUE_UNDEFINED;
	status = collection_this(realm, this_value, collection_bits(realm), &table);
	if (status != 0)
		return status;

	/* The family by the kind. */
	object = (struct vm_object *)vm_value_as_cell(this_value);
	family = JS_ITERATOR_MAP;
	if (object->kind == VM_KIND_SET)
		family = JS_ITERATOR_SET;

	/* The iterator, which holds the table until it ends. */
	status = js_iterator_create(realm, family, this_value, iterate, result);
	if (status != 0)
		return status;
	table->holds++;

	/* Succeeded: the iterator. */
	return 0;
}

/* Finds the table of a collection whose kind is among the bits (a TypeError for anything else). */
static int
collection_this(
	struct vm_realm *realm,
	vm_value value,
	uint32_t kinds,
	struct collection_table **table)
{
	struct vm_object *object;
	int is_object;
	int status;

	/* A collection object of an accepted kind. */
	*table = NULL;
	is_object = vm_value_is_object(value);
	if (is_object) {
		object = (struct vm_object *)vm_value_as_cell(value);
		if (object->kind >= VM_KIND_MAP && object->kind <= VM_KIND_WEAK_SET && (kinds & (1U << (object->kind - VM_KIND_MAP))) != 0U) {
			*table = (struct collection_table *)vm_value_as_cell(object->internal);
			return 0;
		}
	}

	/* Anything else. */
	status = vm_throw_type_error(realm, "Method called on incompatible receiver");
	return status;
}

/* Reports the kinds the running method takes as its this (its data, the bit of its prototype's kind). */
static uint32_t
collection_bits(
	const struct vm_realm *realm)
{
	struct vm_function *function;

	/* The method's data. */
	function = (struct vm_function *)vm_value_as_cell(realm->callee);

	/* Reports the bits. */
	return (uint32_t)vm_value_as_int32(function->data);
}

/* Defines a collection's method whose data is the bit of the kind its this must be. */
static int
collection_define(
	struct vm_realm *realm,
	struct vm_object *object,
	const char *name,
	unsigned length,
	vm_native native,
	uint32_t bit,
	struct vm_function **function)
{
	int error;

	/* The function, knowing its kind. */
	error = js_builtin_function(realm, name, length, native, NULL, function);
	if (error != 0)
		return error;
	(*function)->data = vm_value_int32((int32_t)bit);

	/* The property. */
	error = js_builtin_value(realm, object, name, vm_value_cell(*function), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* Succeeded: the method is defined. */
	return 0;
}

/* Tells whether a value can be a weak collection's key: an object, or a symbol not in the registry. */
static int
collection_weak_key(
	vm_value key)
{
	struct vm_cell *cell;
	int is_object;
	int is_cell;

	/* An object. */
	is_object = vm_value_is_object(key);
	if (is_object)
		return 1;

	/* A symbol that Symbol.for did not make. */
	is_cell = vm_value_is_cell(key);
	if (!is_cell)
		return 0;
	cell = vm_value_as_cell(key);
	if (cell->type == &vm_symbol_type && !((struct vm_symbol *)cell)->registered)
		return 1;

	/* Anything else. */
	return 0;
}

/* Makes a key's one form: a number as an int32 when it is one, -0 as 0. */
static vm_value
collection_normalize(
	vm_value key)
{
	double number;
	int is_double;

	/* Only a double changes. */
	is_double = vm_value_is_double(key);
	if (!is_double)
		return key;

	/* -0 is 0; anything else in its own form. */
	number = vm_value_as_double(key);
	if (number == 0.0)
		return vm_value_int32(0);

	/* The number's form. */
	return vm_value_number(number);
}

/* Reports a normalized key's hash: a string's by its characters, anything else by its bits. */
static uint32_t
collection_hash(
	vm_value key)
{
	uint64_t bits;
	int is_string;

	/* A string by its characters. */
	is_string = vm_value_is_string(key);
	if (is_string)
		return vm_string_hash((struct vm_string *)vm_value_as_cell(key));

	/* A BigInt by its digits. */
	if (vm_value_as_bigint(key) != NULL)
		return vm_bigint_hash(vm_value_as_bigint(key));

	/* Anything else: its bits, mixed. */
	bits = key;
	bits ^= bits >> 33;
	bits *= 0xff51afd7ed558ccdULL;
	bits ^= bits >> 33;

	/* Reports the hash. */
	return (uint32_t)bits;
}

/* Tells whether two normalized keys are the same (SameValueZero). */
static int
collection_same(
	vm_value left,
	vm_value right)
{
	int left_string;
	int right_string;
	int equal;

	/* The same bits. */
	if (left == right)
		return 1;

	/* Two BigInts of the same value. */
	if (vm_value_as_bigint(left) != NULL && vm_value_as_bigint(right) != NULL)
		return vm_bigint_compare(vm_value_as_bigint(left), vm_value_as_bigint(right)) == 0;

	/* Two strings with the same characters. */
	left_string = vm_value_is_string(left);
	right_string = vm_value_is_string(right);
	if (!left_string || !right_string)
		return 0;
	equal = vm_string_equal((const struct vm_string *)vm_value_as_cell(left), (const struct vm_string *)vm_value_as_cell(right));

	/* Reports whether the characters match. */
	return equal;
}

/* Finds a key's live entry; COLLECTION_NONE when it is not present. */
static uint32_t
collection_find(
	const struct collection_table *table,
	vm_value key,
	uint32_t hash)
{
	uint32_t index;
	int same;

	/* An empty table has no chains. */
	if (table->bucket_count == 0)
		return COLLECTION_NONE;

	/* The key's chain. */
	for (index = table->buckets[hash & (table->bucket_count - 1U)]; index != COLLECTION_NONE; index = table->entries[index].chain) {
		if (table->entries[index].hash != hash)
			continue;
		same = collection_same(table->entries[index].key, key);
		if (same)
			return index;
	}

	/* Not there. */
	return COLLECTION_NONE;
}

/* Sets a key's value: its entry changed, or a new entry at the end. */
static int
collection_put(
	struct collection_table *table,
	vm_value key,
	vm_value value)
{
	struct collection_entry *entry;
	uint32_t hash;
	uint32_t found;
	uint32_t bucket;
	int status;

	/* A key present already has its value changed. */
	hash = collection_hash(key);
	found = collection_find(table, key, hash);
	if (found != COLLECTION_NONE) {
		table->entries[found].value = value;
		return 0;
	}

	/* Room for one more entry. */
	if (table->count == table->capacity) {
		status = collection_grow(table);
		if (status != 0)
			return status;
	}

	/* The entry at the end, at the head of its chain. */
	bucket = hash & (table->bucket_count - 1U);
	entry = &table->entries[table->count];
	entry->key = key;
	entry->value = value;
	entry->hash = hash;
	entry->chain = table->buckets[bucket];
	table->buckets[bucket] = table->count;
	table->count++;
	table->size++;

	/* Succeeded: the entry is added. */
	return 0;
}

/* Removes a key's entry (it keeps its place, emptied); whether there was one. */
static int
collection_remove(
	struct collection_table *table,
	vm_value key)
{
	uint32_t hash;
	uint32_t found;
	uint32_t *link;

	/* The key's entry. */
	hash = collection_hash(key);
	found = collection_find(table, key, hash);
	if (found == COLLECTION_NONE)
		return 0;

	/* Out of its chain. */
	link = &table->buckets[hash & (table->bucket_count - 1U)];
	while (*link != found)
		link = &table->entries[*link].chain;
	*link = table->entries[found].chain;

	/* Emptied in place. */
	table->entries[found].key = VM_VALUE_EMPTY;
	table->entries[found].value = VM_VALUE_UNDEFINED;
	table->size--;

	/* Succeeded: it was removed. */
	return 1;
}

/*
 * Makes room for more entries: the removed ones are dropped when no walk
 * holds the table and they are many, otherwise the entries double.
 */
static int
collection_grow(
	struct collection_table *table)
{
	struct collection_entry *entries;
	uint32_t capacity;
	uint32_t from;
	uint32_t to;
	int status;

	/* Many removed entries and no walk: they are dropped, the live ones moved up. */
	if (table->holds == 0 && table->size < table->count / 2U) {
		to = 0;
		for (from = 0; from < table->count; from++) {
			if (table->entries[from].key == VM_VALUE_EMPTY)
				continue;
			table->entries[to] = table->entries[from];
			to++;
		}

		/* The live entries are all there is now, with their chains rebuilt. */
		table->count = to;
		status = collection_rehash(table, table->bucket_count);
		if (status != 0)
			return status;
		return 0;
	}

	/* Otherwise twice the entries (the first time, a few). */
	capacity = COLLECTION_FIRST;
	if (table->capacity != 0)
		capacity = table->capacity * 2U;
	if (capacity < table->capacity)
		return ENOMEM;
	entries = realloc(table->entries, (size_t)capacity * sizeof(*entries));
	if (entries == NULL)
		return ENOMEM;
	table->entries = entries;
	table->capacity = capacity;

	/* And as many chains, rebuilt. */
	status = collection_rehash(table, capacity);
	if (status != 0)
		return status;

	/* Succeeded: there is room. */
	return 0;
}

/* Rebuilds the chains of a table with a number of heads (a power of two). */
static int
collection_rehash(
	struct collection_table *table,
	uint32_t bucket_count)
{
	uint32_t *buckets;
	uint32_t index;
	uint32_t bucket;

	/* The heads, all empty. */
	buckets = malloc((size_t)bucket_count * sizeof(*buckets));
	if (buckets == NULL)
		return ENOMEM;
	for (index = 0; index < bucket_count; index++)
		buckets[index] = COLLECTION_NONE;

	/* Each live entry at the head of its chain. */
	for (index = 0; index < table->count; index++) {
		if (table->entries[index].key == VM_VALUE_EMPTY)
			continue;
		bucket = table->entries[index].hash & (bucket_count - 1U);
		table->entries[index].chain = buckets[bucket];
		buckets[bucket] = index;
	}

	/* The new heads replace the old. */
	free(table->buckets);
	table->buckets = buckets;
	table->bucket_count = bucket_count;

	/* Succeeded: the chains are rebuilt. */
	return 0;
}
