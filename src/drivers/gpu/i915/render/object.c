/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan object table (see object.h).
 *
 * The table is a growable flat array scanned linearly; the object counts
 * of the applications it serves are small enough that nothing faster is
 * needed.  It belongs to the executor of one device and holds the objects
 * of all its sessions; each entry is keyed by the session that created it
 * too, because every process numbers its objects from the same start.
 */

#include "object.h"
#include <kern/kcrt.h>

#include <kern/kmem.h>

#include <uapi/errno.h>
#include <stddef.h>

/*
 * One live Vulkan object under its wire identity.
 *
 * It is a slot of the table's array and lives from the object's creation
 * to its destruction.
 */
struct i915_object_entry {
	/* The session that created the object; a lookup from another session does not find it. */
	struct i915_render_session *owner;

	/* The kind the object was created as; a lookup of another kind does not find it. */
	enum i915_vk_object_kind kind;

	/* The identity libvulkan gave the object. */
	i915_vk_handle handle;

	/* The object, owned by the part that created it. */
	void *object;
};

/*
 * The object table of one executor.
 *
 * It is created at attach and released at detach; only the index storage
 * belongs to it.
 */
struct i915_object_table {
	/* The entries; NULL until the first insert. */
	struct i915_object_entry *entries;

	/* How many entries are live and how many fit. */
	unsigned count;
	unsigned capacity;
};

/*
 * Creates the empty object table of an executor.
 */
int
drv_i915_object_table_create(
	struct i915_object_table **out)
{
	struct i915_object_table *table;

	/* The caller receives nothing on failure. */
	*out = NULL;

	/* Allocates the table. */
	table = kern_calloc(1U, sizeof(*table));
	if (table == NULL)
		return ENOMEM;

	/* An empty table owns no storage until the first insert. */
	table->entries = NULL;
	table->count = 0U;
	table->capacity = 0U;

	/* Succeeded: the executor can register objects. */
	*out = table;
	return 0;
}

/*
 * Releases the object table.
 *
 * The objects themselves are freed by the parts that created them.
 */
void
drv_i915_object_table_destroy(
	struct i915_object_table *table)
{
	/* A table never created is nothing to release. */
	if (table == NULL)
		return;

	/* Only the index storage belongs to the table. */
	if (table->entries != NULL)
		kern_free(table->entries);

	kern_free(table);
}

/*
 * Records a live object under its identity.
 *
 * An identity the session already recorded for the same kind is
 * overwritten in place.
 */
int
drv_i915_object_insert(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	i915_vk_handle handle,
	void *object)
{
	struct i915_object_table *table;
	struct i915_object_entry *grown;
	unsigned capacity;
	unsigned index;

	/* Overwrites the entry of a reused identity of the same session and kind. */
	table = session->vk->objects;
	for (index = 0U; index < table->count; index++) {
		if (table->entries[index].owner != session)
			continue;
		if (table->entries[index].kind != kind)
			continue;
		if (table->entries[index].handle != handle)
			continue;

		table->entries[index].object = object;
		return 0;
	}

	/* Grows a full index geometrically, so inserts amortize to constant time. */
	if (table->count == table->capacity) {
		/* The first array holds sixteen entries; each later one twice the last. */
		if (table->capacity == 0U) {
			capacity = 16U;
		} else {
			capacity = table->capacity * 2U;
		}

		/* Allocates the larger array. */
		grown = kern_calloc(capacity, sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;

		/* Moves the existing entries to the larger array before it replaces them. */
		if (table->entries != NULL) {
			kern_memcpy(grown, table->entries, (size_t)table->count * sizeof(*grown));
			kern_free(table->entries);
		}

		table->entries = grown;
		table->capacity = capacity;
	}

	/* Records the new entry in the next free slot. */
	table->entries[table->count].owner = session;
	table->entries[table->count].kind = kind;
	table->entries[table->count].handle = handle;
	table->entries[table->count].object = object;
	table->count++;

	/* Succeeded: the identity now resolves to this object. */
	return 0;
}

/*
 * Returns the object the session recorded under an identity of the given
 * kind, or NULL.
 */
void *
drv_i915_object_lookup(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	i915_vk_handle handle)
{
	struct i915_object_table *table;
	unsigned index;

	/* Scans the entries for the session, the kind and the identity. */
	table = session->vk->objects;
	for (index = 0U; index < table->count; index++) {
		if (table->entries[index].owner != session)
			continue;
		if (table->entries[index].kind != kind)
			continue;
		if (table->entries[index].handle != handle)
			continue;

		return table->entries[index].object;
	}

	/* No object of that session and kind has the identity. */
	return NULL;
}

/*
 * Drops an identity from the table.
 *
 * The last entry moves into the freed slot, so the order of the entries is
 * not kept.
 */
void
drv_i915_object_remove(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	i915_vk_handle handle)
{
	struct i915_object_table *table;
	unsigned index;

	/* Finds the entry and fills its slot with the last one. */
	table = session->vk->objects;
	for (index = 0U; index < table->count; index++) {
		if (table->entries[index].owner != session)
			continue;
		if (table->entries[index].kind != kind)
			continue;
		if (table->entries[index].handle != handle)
			continue;

		table->entries[index] = table->entries[table->count - 1U];
		table->count--;
		return;
	}
}

/*
 * Drops every identity a closing session recorded.
 *
 * The objects an application did not destroy stay with the parts that
 * created them; only their entries go, so a later session opened at the
 * same address never finds them.
 */
void
drv_i915_object_forget(
	struct i915_render_session *session)
{
	struct i915_object_table *table;
	unsigned index;

	/* Fills the slot of each of the session's entries with the last one, and looks at the slot again. */
	table = session->vk->objects;
	index = 0U;
	while (index < table->count) {
		if (table->entries[index].owner != session) {
			index++;
			continue;
		}

		/* The last entry takes the freed slot. */
		table->entries[index] = table->entries[table->count - 1U];
		table->count--;
	}
}
