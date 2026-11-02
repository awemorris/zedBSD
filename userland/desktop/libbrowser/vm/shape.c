/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shapes (hidden classes): the tree of property layouts objects share.
 *
 * A shape is the shape before it plus one property: its key, its
 * attributes and the slot its value takes (the count of properties
 * before it).  Objects made the same way walk the same path from the
 * heap's root shape and share every shape on it; the transitions from a
 * shape to the shapes made from it are kept so the path is found again.
 * Keys are values: an atom, a symbol, or an int32 index.
 *
 * Finding a key walks the path back to the root, which is slow for an
 * object of many properties.  A shape of at least SHAPE_TABLE_MIN
 * properties is given a hash table of its path's keys the first time a
 * key is looked up in it.  The table is shared with the shapes before it
 * on the path: an entry whose slot is below a shape's count is that
 * shape's own property, and one at or past it is a later one.  A shape
 * made from the table's last shape takes the table on, one entry longer,
 * so an object that grows a property at a time keeps a single table.
 */

#include "vm/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The fewest transitions a shape's table is made for. */
#define SHAPE_TRANSITIONS_MIN	4U

/* The fewest properties a shape has before its keys are found through a hash table. */
#define SHAPE_TABLE_MIN		8U

/* The fewest entries of a key table (a power of two, at least twice the keys it holds). */
#define SHAPE_TABLE_ENTRIES_MIN	16U

/*
 * One entry of a key table: a key on the path, its slot (its place on the
 * path) and its attributes.  An entry whose key is VM_VALUE_EMPTY is free.
 */
struct vm_shape_entry {
	vm_value key;
	uint32_t slot;
	uint32_t attributes;
};

/*
 * A hash table of the keys on the path to its last shape, last.
 *
 * Every shape whose table it is lies on that path, so an entry whose slot
 * is below the shape's count is a property of the shape.  references
 * counts those shapes; the last one to go frees the table.  duplicates is
 * set when a key is on the path twice (each entry holds the last one), and
 * a shape before the last one then walks its path for a key whose entry
 * is past it.  mask is the entry count less one.
 */
struct vm_shape_table {
	struct vm_shape *last;
	uint32_t references;
	uint32_t used;
	uint32_t mask;
	uint32_t duplicates;
	struct vm_shape_entry *entries;
};

/*
 * One transition: the key and attributes added, and the shape they lead to.
 */
struct vm_shape_transition {
	vm_value key;
	uint32_t attributes;
	struct vm_shape *shape;
};

/*
 * A shape.
 *
 * parent is NULL for the root, whose key is VM_VALUE_EMPTY.  count is the
 * number of properties on the path to this shape (this one's slot plus
 * one).  The transitions are malloc'd and freed with the shape; the shape
 * holds the shapes they lead to, so a path lives as long as its root.
 * table is the key table the shape shares, or NULL until a lookup makes
 * one (and always NULL below SHAPE_TABLE_MIN properties).
 */
struct vm_shape {
	struct vm_cell cell;
	struct vm_shape *parent;
	vm_value key;
	uint32_t attributes;
	uint32_t slot;
	uint32_t count;
	uint32_t transition_count;
	uint32_t transition_capacity;
	uint32_t reserved;
	struct vm_shape_transition *transitions;
	struct vm_shape_table *table;
};

static int shape_walk(const struct vm_shape *shape, vm_value key, uint32_t *slot, uint32_t *attributes);
static uint32_t shape_hash(vm_value key);
static struct vm_shape_entry *shape_table_entry(const struct vm_shape_table *table, vm_value key);
static int shape_table_put(struct vm_shape_table *table, vm_value key, uint32_t slot, uint32_t attributes);
static struct vm_shape_table *shape_table_make(struct vm_shape *shape);
static void shape_table_extend(struct vm_shape *shape, struct vm_shape *added);
static void shape_trace(struct vm_heap *heap, struct vm_cell *cell);
static void shape_finalize(struct vm_heap *heap, struct vm_cell *cell);

/* The cell type of shapes: they hold their parent, their key and the shapes made from them. */
static const struct vm_cell_type shape_type = {
	"shape", shape_trace, shape_finalize
};

/*
 * Reports the heap's root shape (no properties), making it the first time.
 */
struct vm_shape *
vm_shape_root(
	struct vm_heap *heap)
{
	struct vm_shape *root;
	int error;

	/* The root made before. */
	if (heap->root_shape != NULL)
		return heap->root_shape;

	/* The root: no parent, no key, no properties. */
	root = vm_heap_alloc(heap, &shape_type, sizeof(*root));
	if (root == NULL)
		return NULL;
	root->key = VM_VALUE_EMPTY;

	/* The heap keeps it (and through it every shape) as a root. */
	heap->root_shape = root;
	error = vm_heap_add_root(heap, (struct vm_cell **)&heap->root_shape);
	if (error != 0) {
		heap->root_shape = NULL;
		return NULL;
	}

	/* Succeeded: the root shape. */
	return root;
}

/*
 * Reports the shape of a shape plus a property, following the transition
 * made before or making it; NULL when out of memory.
 */
struct vm_shape *
vm_shape_add(
	struct vm_heap *heap,
	struct vm_shape *shape,
	vm_value key,
	uint32_t attributes)
{
	struct vm_shape_transition *transitions;
	struct vm_shape *added;
	uint32_t capacity;
	uint32_t index;

	/* The transition made before, when there is one. */
	for (index = 0; index < shape->transition_count; index++) {
		if (shape->transitions[index].key != key)
			continue;
		if (shape->transitions[index].attributes != attributes)
			continue;
		return shape->transitions[index].shape;
	}

	/* The new shape (the parent is on this frame, so a collection keeps it). */
	added = vm_heap_alloc(heap, &shape_type, sizeof(*added));
	if (added == NULL)
		return NULL;
	added->parent = shape;
	added->key = key;
	added->attributes = attributes;
	added->slot = shape->count;
	added->count = shape->count + 1U;

	/* Room for the transition. */
	if (shape->transition_count == shape->transition_capacity) {
		capacity = shape->transition_capacity * 2U;
		if (capacity < SHAPE_TRANSITIONS_MIN)
			capacity = SHAPE_TRANSITIONS_MIN;
		transitions = realloc(shape->transitions, capacity * sizeof(*transitions));
		if (transitions == NULL)
			return NULL;
		shape->transitions = transitions;
		shape->transition_capacity = capacity;
	}

	/* Records the transition. */
	shape->transitions[shape->transition_count].key = key;
	shape->transitions[shape->transition_count].attributes = attributes;
	shape->transitions[shape->transition_count].shape = added;
	shape->transition_count++;

	/* The new shape takes on the key table its parent ends. */
	shape_table_extend(shape, added);

	/* Succeeded: the new shape. */
	return added;
}

/*
 * Finds a key's slot and attributes in a shape; zero when the shape does
 * not have the key.
 *
 * A shape of many properties is given a key table the first time; without
 * memory for one the path is walked.
 */
int
vm_shape_find(
	struct vm_shape *shape,
	vm_value key,
	uint32_t *slot,
	uint32_t *attributes)
{
	struct vm_shape_entry *entry;
	struct vm_shape_table *table;
	int found;

	/* A shape of few properties walks its path. */
	if (shape == NULL || shape->count < SHAPE_TABLE_MIN) {
		found = shape_walk(shape, key, slot, attributes);
		return found;
	}

	/* The shape's key table, made the first time. */
	table = shape->table;
	if (table == NULL) {
		table = shape_table_make(shape);
		if (table == NULL) {
			found = shape_walk(shape, key, slot, attributes);
			return found;
		}
		shape->table = table;
	}

	/* A key not on the table's path is not on the shape's either. */
	entry = shape_table_entry(table, key);
	if (entry->key != key)
		return 0;

	/* An entry within the shape's count is its own property. */
	if (entry->slot < shape->count) {
		*slot = entry->slot;
		*attributes = entry->attributes;
		return 1;
	}

	/* A later property: an earlier one of the same key may be the shape's when the path has the key twice. */
	if (table->duplicates) {
		found = shape_walk(shape, key, slot, attributes);
		return found;
	}

	/* Not among the shape's properties. */
	return 0;
}

/*
 * Reports how many properties a shape has.
 */
uint32_t
vm_shape_count(
	const struct vm_shape *shape)
{
	/* The count kept on the shape. */
	return shape->count;
}

/*
 * Writes a shape's keys, slots and attributes in the order they were
 * added (each array as long as the shape's count).
 */
int
vm_shape_keys(
	const struct vm_shape *shape,
	vm_value *keys,
	uint32_t *slots,
	uint32_t *attributes)
{
	uint32_t index;

	/* From the last property to the first, each at its own place. */
	index = shape->count;
	while (shape->parent != NULL) {
		index--;
		keys[index] = shape->key;
		slots[index] = shape->slot;
		attributes[index] = shape->attributes;
		shape = shape->parent;
	}

	/* Succeeded: the arrays are filled. */
	return 0;
}

/* Finds a key by walking a shape's path from its last property to its first; zero when it is not there. */
static int
shape_walk(
	const struct vm_shape *shape,
	vm_value key,
	uint32_t *slot,
	uint32_t *attributes)
{
	/* Walks from the last property added to the first. */
	while (shape != NULL && shape->parent != NULL) {
		/* The key's own shape. */
		if (shape->key == key) {
			*slot = shape->slot;
			*attributes = shape->attributes;
			return 1;
		}

		/* The property before it. */
		shape = shape->parent;
	}

	/* Not among the shape's properties. */
	return 0;
}

/* Mixes a key's bits into a hash (atoms and symbols differ in their cell addresses, indices in their values). */
static uint32_t
shape_hash(
	vm_value key)
{
	uint64_t mixed;

	/* A 64-bit finalizer spreads aligned addresses over the low bits. */
	mixed = key;
	mixed ^= mixed >> 33;
	mixed *= 0xff51afd7ed558ccdULL;
	mixed ^= mixed >> 33;
	mixed *= 0xc4ceb9fe1a85ec53ULL;
	mixed ^= mixed >> 33;

	/* The low bits pick the entry. */
	return (uint32_t)mixed;
}

/* Finds a key's entry in a key table, or the free entry where it would go. */
static struct vm_shape_entry *
shape_table_entry(
	const struct vm_shape_table *table,
	vm_value key)
{
	struct vm_shape_entry *entry;
	uint32_t index;

	/* Probes from the key's hash until the key or a free entry (the table is never full). */
	index = shape_hash(key) & table->mask;
	for (;;) {
		entry = &table->entries[index];
		if (entry->key == key || entry->key == VM_VALUE_EMPTY)
			return entry;
		index = (index + 1U) & table->mask;
	}
}

/*
 * Puts a key in a key table, growing the table to keep it at most half
 * full; a key already there takes the new slot and marks the table's path
 * as having the key twice.  ENOMEM leaves the table as it was.
 */
static int
shape_table_put(
	struct vm_shape_table *table,
	vm_value key,
	uint32_t slot,
	uint32_t attributes)
{
	struct vm_shape_entry *entries;
	struct vm_shape_entry *old_entries;
	struct vm_shape_entry *entry;
	uint32_t old_count;
	uint32_t count;
	uint32_t index;

	/* A key already on the path: the entry holds the last one. */
	entry = shape_table_entry(table, key);
	if (entry->key == key) {
		entry->slot = slot;
		entry->attributes = attributes;
		table->duplicates = 1;
		return 0;
	}

	/* Twice the entries when the new key would fill more than half. */
	old_count = table->mask + 1U;
	if ((table->used + 1U) * 2U > old_count) {
		if (old_count > UINT32_MAX / 2U / sizeof(*entries))
			return ENOMEM;
		count = old_count * 2U;
		entries = calloc(count, sizeof(*entries));
		if (entries == NULL)
			return ENOMEM;

		/* Moves each key to its place in the larger table. */
		old_entries = table->entries;
		table->entries = entries;
		table->mask = count - 1U;
		for (index = 0; index < old_count; index++) {
			if (old_entries[index].key == VM_VALUE_EMPTY)
				continue;
			entry = shape_table_entry(table, old_entries[index].key);
			*entry = old_entries[index];
		}
		free(old_entries);
		entry = shape_table_entry(table, key);
	}

	/* The new key. */
	entry->key = key;
	entry->slot = slot;
	entry->attributes = attributes;
	table->used++;

	/* Succeeded: the key is in the table. */
	return 0;
}

/* Makes the key table of a shape's path, the shape its last; NULL without memory. */
static struct vm_shape_table *
shape_table_make(
	struct vm_shape *shape)
{
	struct vm_shape_table *table;
	struct vm_shape_entry *entry;
	const struct vm_shape *walk;
	uint32_t count;

	/* At least twice as many entries as the path has keys. */
	count = SHAPE_TABLE_ENTRIES_MIN;
	while (count < shape->count * 2U) {
		if (count > UINT32_MAX / 4U / sizeof(*entry))
			return NULL;
		count *= 2U;
	}

	/* The table and its entries. */
	table = calloc(1, sizeof(*table));
	if (table == NULL)
		return NULL;
	table->entries = calloc(count, sizeof(*table->entries));
	if (table->entries == NULL) {
		free(table);
		return NULL;
	}
	table->mask = count - 1U;
	table->last = shape;
	table->references = 1;

	/* Each key from the last property back; the first met of a key is the one the path has (a later one marks it twice). */
	for (walk = shape; walk->parent != NULL; walk = walk->parent) {
		entry = shape_table_entry(table, walk->key);
		if (entry->key == walk->key) {
			table->duplicates = 1;
			continue;
		}
		entry->key = walk->key;
		entry->slot = walk->slot;
		entry->attributes = walk->attributes;
		table->used++;
	}

	/* Succeeded: the table ends at the shape. */
	return table;
}

/*
 * Gives a shape just made from another the other's key table when that
 * table ends at the other, with the new key; the table then ends at the
 * new shape.  Without memory the new shape makes its own when it is
 * looked up.
 */
static void
shape_table_extend(
	struct vm_shape *shape,
	struct vm_shape *added)
{
	struct vm_shape_table *table;
	int error;

	/* Only the table that ends at the parent can take the key on. */
	table = shape->table;
	if (table == NULL || table->last != shape)
		return;

	/* The new key, then the new shape shares the table. */
	error = shape_table_put(table, added->key, added->slot, added->attributes);
	if (error != 0)
		return;
	table->last = added;
	table->references++;
	added->table = table;
}

/* Marks a shape's parent, its key and the shapes its transitions lead to. */
static void
shape_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_shape *shape;
	uint32_t index;

	/* The path before it, and the key it adds. */
	shape = (struct vm_shape *)cell;
	if (shape->parent != NULL)
		vm_heap_mark(heap, &shape->parent->cell);
	vm_heap_mark_value(heap, shape->key);

	/* The shapes made from it. */
	for (index = 0; index < shape->transition_count; index++)
		vm_heap_mark(heap, &shape->transitions[index].shape->cell);
}

/* Frees a dead shape's transition table and its share of the key table. */
static void
shape_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_shape_table *table;
	struct vm_shape *shape;

	UNUSED_PARAMETER(heap);

	/* The table the shape owns. */
	shape = (struct vm_shape *)cell;
	free(shape->transitions);
	shape->transitions = NULL;

	/* The key table, freed by the last shape that shares it. */
	table = shape->table;
	shape->table = NULL;
	if (table == NULL)
		return;
	table->references--;
	if (table->references != 0U)
		return;
	free(table->entries);
	free(table);
}
