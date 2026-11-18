/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Atoms: the strings interned in a heap's atom table.
 *
 * Element and attribute names, property names and CSS identifiers are
 * atoms, so comparing two names compares two pointers.  Atoms live as
 * long as their heap: the collector marks the whole table.
 */

#include "vm/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The size of the table when the first atom arrives. */
#define ATOM_TABLE_MIN		256U

static int atom_table_grow(struct vm_heap *heap);
static size_t atom_slot_for_units(const struct vm_atom_table *table, uint32_t hash, const uint16_t *units, size_t length);
static int atom_matches_units(const struct vm_string *atom, const uint16_t *units, size_t length);
static int atom_insert(struct vm_heap *heap, struct vm_string *string);

/*
 * Finds or makes the atom with the characters of string.
 *
 * When no atom has them yet, string itself becomes the atom.
 */
struct vm_string *
vm_atom(
	struct vm_heap *heap,
	struct vm_string *string)
{
	struct vm_atom_table *table;
	struct vm_string *candidate;
	uint32_t hash;
	size_t slot;
	int same;
	int error;

	/* An atom is its own atom. */
	if ((string->flags & VM_STRING_ATOM) != 0)
		return string;

	/* Probes the table from the string's hash for a string with its characters. */
	table = &heap->atoms;
	hash = vm_string_hash(string);
	if (table->capacity != 0) {
		slot = hash & (table->capacity - 1U);
		while (table->slots[slot] != NULL) {
			candidate = table->slots[slot];
			if (candidate->hash == hash) {
				same = vm_string_equal(candidate, string);
				if (same)
					return candidate;
			}

			/* Moves to the next slot of the probe. */
			slot = (slot + 1U) & (table->capacity - 1U);
		}
	}

	/* The string becomes the atom for its characters. */
	error = atom_insert(heap, string);
	if (error != 0)
		return NULL;

	/* Succeeded: the string is interned. */
	return string;
}

/*
 * Finds or makes the atom for an ASCII name.
 */
struct vm_string *
vm_atom_from_ascii(
	struct vm_heap *heap,
	const char *ascii)
{
	struct vm_string *string;
	struct vm_string *atom;

	/* Makes a string of the name and interns it. */
	string = vm_string_from_latin1(heap, (const unsigned char *)ascii, strlen(ascii));
	if (string == NULL)
		return NULL;

	/* Interns it, or finds the atom that already has the name. */
	atom = vm_atom(heap, string);
	if (atom == NULL)
		return NULL;

	/* Succeeded: the atom holds the name. */
	return atom;
}

/*
 * Finds or makes the atom for length UTF-16 units.
 *
 * An existing atom is found without allocating, which is what a parser
 * wants for the names it sees again and again.
 */
struct vm_string *
vm_atom_from_units(
	struct vm_heap *heap,
	const uint16_t *units,
	size_t length)
{
	struct vm_string *string;
	struct vm_string *atom;

	/* Looks for the atom first. */
	atom = vm_atom_find_units(heap, units, length);
	if (atom != NULL)
		return atom;

	/* Makes a string of the units and interns it. */
	string = vm_string_from_units(heap, units, length);
	if (string == NULL)
		return NULL;

	/* Interns it. */
	atom = vm_atom(heap, string);
	if (atom == NULL)
		return NULL;

	/* Succeeded: the atom holds the units. */
	return atom;
}

/*
 * Finds the atom for length UTF-16 units, or NULL when there is none.
 */
struct vm_string *
vm_atom_find_units(
	struct vm_heap *heap,
	const uint16_t *units,
	size_t length)
{
	struct vm_atom_table *table;
	uint32_t hash;
	size_t slot;

	/* An empty table holds nothing. */
	table = &heap->atoms;
	if (table->capacity == 0)
		return NULL;

	/* Probes the table from the units' hash. */
	hash = wb_hash_units(units, length);
	slot = atom_slot_for_units(table, hash, units, length);

	/* Reports the atom found, or NULL for the empty slot probing stopped at. */
	return table->slots[slot];
}

/*
 * Marks every atom; the collector calls this at the start of a
 * collection.
 */
void
vm_atom_table_trace(
	struct vm_heap *heap)
{
	size_t index;

	/* Marks the string in every used slot. */
	for (index = 0; index < heap->atoms.capacity; index++) {
		/* Empty slots hold nothing. */
		if (heap->atoms.slots[index] == NULL)
			continue;

		/* The atom is kept. */
		vm_heap_mark(heap, &heap->atoms.slots[index]->cell);
	}
}

/*
 * Frees the atom table (the atoms themselves go with the heap's cells).
 */
void
vm_atom_table_release(
	struct vm_heap *heap)
{
	/* Frees the slots and forgets the atoms. */
	free(heap->atoms.slots);
	heap->atoms.slots = NULL;
	heap->atoms.capacity = 0;
	heap->atoms.count = 0;
}

/* Doubles the table and moves every atom into the new slots. */
static int
atom_table_grow(
	struct vm_heap *heap)
{
	struct vm_string **old_slots;
	struct vm_string **slots;
	size_t old_capacity;
	size_t capacity;
	size_t slot;
	size_t index;

	/* Allocates the larger, empty table. */
	old_slots = heap->atoms.slots;
	old_capacity = heap->atoms.capacity;
	capacity = old_capacity * 2U;
	if (capacity < ATOM_TABLE_MIN)
		capacity = ATOM_TABLE_MIN;
	slots = calloc(capacity, sizeof(*slots));
	if (slots == NULL)
		return ENOMEM;

	/* Moves each atom to the first free slot from its hash. */
	for (index = 0; index < old_capacity; index++) {
		/* Empty slots carry nothing over. */
		if (old_slots[index] == NULL)
			continue;

		/* Puts it in the first free slot from its hash. */
		slot = old_slots[index]->hash & (capacity - 1U);
		while (slots[slot] != NULL)
			slot = (slot + 1U) & (capacity - 1U);
		slots[slot] = old_slots[index];
	}

	/* Publishes the new table. */
	free(old_slots);
	heap->atoms.slots = slots;
	heap->atoms.capacity = capacity;

	/* Succeeded: the table has room. */
	return 0;
}

/* Probes for the slot of the atom with these units, or the empty slot where it would go. */
static size_t
atom_slot_for_units(
	const struct vm_atom_table *table,
	uint32_t hash,
	const uint16_t *units,
	size_t length)
{
	struct vm_string *candidate;
	size_t slot;
	int same;

	/* Walks the probe sequence until the atom or an empty slot. */
	slot = hash & (table->capacity - 1U);
	while (table->slots[slot] != NULL) {
		candidate = table->slots[slot];

		/* An atom with the same hash and characters ends the search. */
		if (candidate->hash == hash) {
			same = atom_matches_units(candidate, units, length);
			if (same)
				return slot;
		}

		/* Moves to the next slot of the probe. */
		slot = (slot + 1U) & (table->capacity - 1U);
	}

	/* The atom is not in the table; this is where it would go. */
	return slot;
}

/* Tells whether an atom holds exactly these units. */
static int
atom_matches_units(
	const struct vm_string *atom,
	const uint16_t *units,
	size_t length)
{
	uint16_t unit;
	size_t index;

	/* The lengths must agree. */
	if (atom->length != length)
		return 0;

	/* Compares unit by unit, whatever the atom's form. */
	for (index = 0; index < length; index++) {
		/* One differing unit settles it. */
		unit = vm_string_at(atom, index);
		if (unit != units[index])
			return 0;
	}

	/* Every unit matched. */
	return 1;
}

/* Adds a string that is not yet an atom to the table. */
static int
atom_insert(
	struct vm_heap *heap,
	struct vm_string *string)
{
	struct vm_atom_table *table;
	size_t slot;
	int error;

	/* Grows the table when adding would fill more than half of it. */
	table = &heap->atoms;
	if ((table->count + 1U) * 2U > table->capacity) {
		error = atom_table_grow(heap);
		if (error != 0)
			return error;
	}

	/* Puts the string in the first free slot from its hash. */
	slot = vm_string_hash(string) & (table->capacity - 1U);
	while (table->slots[slot] != NULL)
		slot = (slot + 1U) & (table->capacity - 1U);
	table->slots[slot] = string;
	table->count++;

	/* The string is now its heap's atom for its characters. */
	string->flags |= VM_STRING_ATOM;

	/* Succeeded: the atom is in the table. */
	return 0;
}
