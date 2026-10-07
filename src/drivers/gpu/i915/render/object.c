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
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>

static int i915_object_insert_unlocked(struct i915_render_session *session, enum i915_vk_object_kind kind, i915_vk_handle handle, void *object);
static void *i915_object_lookup_unlocked(struct i915_render_session *session, enum i915_vk_object_kind kind, i915_vk_handle handle);
static void i915_object_remove_unlocked(struct i915_render_session *session, enum i915_vk_object_kind kind, i915_vk_handle handle);
static void *i915_object_take_unlocked(struct i915_render_session *session, enum i915_vk_object_kind kind, int (*match)(void *object, void *argument), void *argument);
static void i915_object_forget_unlocked(struct i915_render_session *session);

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
	/*
	 * Serializes every use of the entries: the sessions of the table's
	 * device run their command streams at once on different CPUs, and an
	 * insert that grows the array or fills a slot races a removal that
	 * moves the last entry, losing an object (BUG-117).
	 */
	struct mutex lock;

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
	int error;

	/* The caller receives nothing on failure. */
	*out = NULL;

	/* Allocates the table. */
	table = kern_calloc(1U, sizeof(*table));
	if (table == NULL)
		return ENOMEM;

	/* The lock its sessions take around each use. */
	error = mutex_init(&table->lock, LOCK_RANK_DEVICE, "i915 vk objects");
	if (error != 0) {
		kern_free(table);
		return error;
	}

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
 *
 * The caller holds the table's lock.
 */
static int
i915_object_insert_unlocked(
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
 *
 * The caller holds the table's lock.
 */
static void *
i915_object_lookup_unlocked(
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
 *
 * The caller holds the table's lock.
 */
static void
i915_object_remove_unlocked(
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
 * Takes one object of a kind out of the session's entries and returns it,
 * or NULL when none is left.
 *
 * `match`, when not NULL, picks the objects that may be taken: it is given
 * the object and `argument` and answers nonzero for one to take.  The caller
 * owns the taken object from here on.
 *
 * The caller holds the table's lock.
 */
static void *
i915_object_take_unlocked(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	int (*match)(void *object, void *argument),
	void *argument)
{
	struct i915_object_table *table;
	void *object;
	unsigned index;
	int picked;

	/* Scans the entries for one of the session and the kind that the condition picks. */
	table = session->vk->objects;
	for (index = 0U; index < table->count; index++) {
		if (table->entries[index].owner != session)
			continue;
		if (table->entries[index].kind != kind)
			continue;
		if (match != NULL) {
			picked = match(table->entries[index].object, argument);
			if (picked == 0)
				continue;
		}

		/* The last entry takes the freed slot. */
		object = table->entries[index].object;
		table->entries[index] = table->entries[table->count - 1U];
		table->count--;
		return object;
	}

	/* The session has no such object left. */
	return NULL;
}

/*
 * Drops every identity a closing session recorded.
 *
 * The objects themselves were released before (drv_i915_gfx_objects_release);
 * what is left are tokens, which own nothing.  A later session opened at the
 * same address never finds them.  The caller holds the table's lock.
 */
static void
i915_object_forget_unlocked(
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
	int error;

	/* Records the entry. */
	table = session->vk->objects;
	mutex_lock(&table->lock);

	error = i915_object_insert_unlocked(session, kind, handle, object);

	mutex_unlock(&table->lock);

	/* Reports whether the identity resolves to the object now. */
	return error;
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
	void *object;

	/* Finds the entry. */
	table = session->vk->objects;
	mutex_lock(&table->lock);

	object = i915_object_lookup_unlocked(session, kind, handle);

	mutex_unlock(&table->lock);

	/* The object, or NULL. */
	return object;
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

	/* Drops the entry. */
	table = session->vk->objects;
	mutex_lock(&table->lock);

	i915_object_remove_unlocked(session, kind, handle);

	mutex_unlock(&table->lock);
}

/*
 * Takes one object of a kind out of the session's entries and returns it,
 * or NULL when none is left.
 *
 * `match`, when not NULL, picks the objects that may be taken: it is given
 * the object and `argument` and answers nonzero for one to take (under the
 * table's lock: it only looks at the object).  The caller owns the taken
 * object from here on.
 */
void *
drv_i915_object_take(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	int (*match)(void *object, void *argument),
	void *argument)
{
	struct i915_object_table *table;
	void *object;

	/* Takes the entry out. */
	table = session->vk->objects;
	mutex_lock(&table->lock);

	object = i915_object_take_unlocked(session, kind, match, argument);

	mutex_unlock(&table->lock);

	/* The object, or NULL. */
	return object;
}

/*
 * Visits every object of a kind the session recorded.
 *
 * `visit` is given each object and `argument` under the table's lock: it
 * may change the object (BUG-244: a buffer or an image let go of an
 * allocation being freed), but not the table, and takes no lock of its own.
 */
void
drv_i915_object_each(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	void (*visit)(void *object, void *argument),
	void *argument)
{
	struct i915_object_table *table;
	unsigned index;

	/* Each entry of the session and the kind, under the lock. */
	table = session->vk->objects;
	mutex_lock(&table->lock);

	for (index = 0U; index < table->count; index++) {
		if (table->entries[index].owner != session)
			continue;
		if (table->entries[index].kind != kind)
			continue;
		visit(table->entries[index].object, argument);
	}

	mutex_unlock(&table->lock);
}

/*
 * Drops every identity a closing session recorded.
 *
 * The objects themselves were released before (drv_i915_gfx_objects_release);
 * what is left are tokens, which own nothing.
 */
void
drv_i915_object_forget(
	struct i915_render_session *session)
{
	struct i915_object_table *table;

	/* Drops the session's entries. */
	table = session->vk->objects;
	mutex_lock(&table->lock);

	i915_object_forget_unlocked(session);

	mutex_unlock(&table->lock);
}
