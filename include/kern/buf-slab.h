/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The buffer cache's index of header slabs that have a free slot.
 *
 * Buffer headers live in page-sized slabs of at most 64 slots, each slab
 * with a mask of its free slots.  Taking a header used to walk every slab
 * from the newest under the cache lock with interrupts off, so a cache of a
 * million lines spent a millisecond a header once eviction had scattered
 * the free slots.  Here only the slabs with
 * a free slot are linked, so taking and giving back a header is a constant
 * number of steps however many slabs exist.
 *
 * Pure bookkeeping on the slabs' records, with no lock and no memory of its
 * own, kept apart so that the host can test it.  The caller holds the cache lock
 * around every call.
 */

#ifndef KERN_BUF_SLAB_H
#define KERN_BUF_SLAB_H

#include <stddef.h>
#include <stdint.h>

/* The most slots one slab's free mask can name. */
#define BUF_SLAB_SLOTS_MAX	64U

/*
 * How many slabs with no header in use the index keeps instead of handing
 * them back, so that a cache that frees and takes one header in turn does
 * not free and allocate a page each time.
 */
#define BUF_SLAB_EMPTY_KEPT	1U

/*
 * One slab's free slots and its place among the slabs that have one.
 *
 * It is the first member of the buffer cache's slab, which owns the page;
 * the index only links it while a slot is free.
 */
struct buf_slab_record {
	struct buf_slab_record *partial_previous;
	struct buf_slab_record *partial_next;
	uint64_t free_mask;
	unsigned capacity;
	unsigned used;
	unsigned on_partial;
};

/*
 * The slabs that have a free slot, and how many of them have no slot in
 * use.
 *
 * A full slab is on no list: the headers in it lead back to it when they
 * are given back.  Zero-filled, it is an empty index.
 */
struct buf_slab_index {
	struct buf_slab_record *partial;
	unsigned empty_count;
};

/* Links a slab at the head of the slabs that have a free slot. */
static inline void
buf_slab_partial_link(
	struct buf_slab_index *index,
	struct buf_slab_record *record)
{
	record->partial_previous = NULL;
	record->partial_next = index->partial;

	/* The old head now follows this slab. */
	if (index->partial != NULL)
		index->partial->partial_previous = record;

	index->partial = record;
	record->on_partial = 1;
}

/* Unlinks a slab from the slabs that have a free slot. */
static inline void
buf_slab_partial_unlink(
	struct buf_slab_index *index,
	struct buf_slab_record *record)
{
	/* The slab before it, or the head, now leads to the slab after it. */
	if (record->partial_previous != NULL) {
		record->partial_previous->partial_next = record->partial_next;
	} else {
		index->partial = record->partial_next;
	}

	/* The slab after it now follows the slab before it. */
	if (record->partial_next != NULL)
		record->partial_next->partial_previous = record->partial_previous;

	record->partial_previous = NULL;
	record->partial_next = NULL;
	record->on_partial = 0;
}

/*
 * Prepares a new slab of capacity slots, every slot free, and adds it to
 * the index.  Reports 0, or -1 for a capacity the free mask cannot name.
 */
static inline int
buf_slab_insert(
	struct buf_slab_index *index,
	struct buf_slab_record *record,
	unsigned capacity)
{
	/* A slab needs a slot, and no more than the mask has bits. */
	if (capacity == 0 || capacity > BUF_SLAB_SLOTS_MAX)
		return -1;

	/* Every slot of the new slab is free. */
	record->capacity = capacity;
	record->used = 0;
	if (capacity == BUF_SLAB_SLOTS_MAX) {
		record->free_mask = UINT64_MAX;
	} else {
		record->free_mask = (UINT64_C(1) << capacity) - 1U;
	}

	/* The new slab is one with no slot in use, ready to be taken from. */
	buf_slab_partial_link(index, record);
	index->empty_count++;

	/* Succeeded: the slab is in the index. */
	return 0;
}

/*
 * Takes the lowest free slot of the first slab that has one.  Reports 0
 * with the slab and the slot, or -1 when no slab has a free slot (the
 * caller adds one).
 */
static inline int
buf_slab_take(
	struct buf_slab_index *index,
	struct buf_slab_record **record_out,
	unsigned *slot_out)
{
	struct buf_slab_record *record;
	unsigned slot;

	/* No slab with a free slot: the caller grows the slabs. */
	record = index->partial;
	if (record == NULL)
		return -1;

	/* Finds the lowest free slot; a slab on the list has one, at most 64 bits on. */
	slot = 0;
	while ((record->free_mask & (UINT64_C(1) << slot)) == 0)
		slot++;

	/* A slab that had no slot in use is no longer an empty one. */
	if (record->used == 0)
		index->empty_count--;

	/*
	 * The slot leaves the free mask and counts as used; a slab whose last
	 * free slot this was leaves the list until a header is given back.
	 */
	record->free_mask &= ~(UINT64_C(1) << slot);
	record->used++;
	if (record->free_mask == 0)
		buf_slab_partial_unlink(index, record);

	*record_out = record;
	*slot_out = slot;

	/* Succeeded: the slot is the caller's. */
	return 0;
}

/*
 * Gives a slot back to its slab.  Reports 1 when the slab is left with no
 * slot in use and the index has kept enough empty slabs: the slab has then
 * left the index and the caller frees its page.  Reports 0 otherwise, and
 * -1 for a slot the slab does not have or that is already free.
 */
static inline int
buf_slab_put(
	struct buf_slab_index *index,
	struct buf_slab_record *record,
	unsigned slot)
{
	uint64_t bit;

	/* Refuses a slot outside the slab. */
	if (slot >= record->capacity)
		return -1;

	/* Refuses a slot that is free already. */
	bit = UINT64_C(1) << slot;
	if ((record->free_mask & bit) != 0)
		return -1;

	/*
	 * The slot is free again; a slab that was full has a free slot now and
	 * rejoins the list.
	 */
	record->free_mask |= bit;
	record->used--;
	if (!record->on_partial)
		buf_slab_partial_link(index, record);

	/* A slab that still has a slot in use stays. */
	if (record->used != 0)
		return 0;

	/* The first empty slabs are kept for the next headers. */
	if (index->empty_count < BUF_SLAB_EMPTY_KEPT) {
		index->empty_count++;
		return 0;
	}

	/* Succeeded: the slab has left the index and its page is the caller's to free. */
	buf_slab_partial_unlink(index, record);
	return 1;
}

#endif
