/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bookkeeping of a display controller that scans a program's buffer
 * directly.
 *
 * A buffer is held from the commit that puts it on the plane until the
 * commit after it has been taken, so it is never freed while the
 * controller may read it.  A commit counts as taken two vertical blanks
 * after the sequence read right after it was written.  Every change is
 * recorded in a ring, and an event out of the promised order is counted,
 * so a test reads the order afterwards instead of watching the screen.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/powervr/display/dc8200-flip.h"

static void record(struct powervr_flip *flip, enum powervr_flip_event event, const struct powervr_flip_hold *buffer, uint64_t sequence);
static int drop(struct powervr_flip *flip, struct powervr_flip_hold *buffer, uint64_t sequence);

/*
 * Starts the flip state with the loader's framebuffer on the plane.
 */
void
powervr_flip_init(
	struct powervr_flip *flip)
{
	uint32_t index;

	/* Nothing is held: the controller scans the loader's framebuffer. */
	flip->front = NULL;
	flip->pending = NULL;
	flip->unsettled = NULL;
	flip->committing = 0;
	flip->lost = 0;
	flip->base = 0;
	flip->mismatches = 0;
	flip->next = 0;
	flip->recorded = 0;

	/* Empties the ring. */
	for (index = 0; index < POWERVR_FLIP_RING; index++) {
		flip->ring[index].event = 0;
		flip->ring[index].holds = 0;
		flip->ring[index].buffer = 0;
		flip->ring[index].sequence = 0;
		flip->ring[index].base = 0;
	}
}

/*
 * Records a commit of a buffer at the sequence read right after it was
 * written, and holds the buffer.
 */
void
powervr_flip_commit(
	struct powervr_flip *flip,
	struct powervr_flip_hold *buffer,
	uint64_t base)
{
	/* A second commit before the first is taken breaks the one-at-a-time rule. */
	if (flip->committing)
		flip->mismatches++;

	/*
	 * The hold keeps the buffer alive while the controller may take it;
	 * the framebuffer is the loader's and needs none.
	 */
	if (buffer != NULL)
		buffer->holds++;

	flip->pending = buffer;
	flip->committing = 1;
	flip->base = base;
	record(flip, POWERVR_FLIP_COMMIT, buffer, base);
}

/*
 * Reports whether a commit written at base counts as taken once the
 * sequence has reached sequence.
 */
int
powervr_flip_due(
	uint64_t base,
	uint64_t sequence)
{
	/* A sequence behind the base has not even reached the commit. */
	if (sequence < base)
		return 0;

	/* Taken from the second blank after the commit's sequence on. */
	if (sequence - base < POWERVR_FLIP_LATCH_BLANKS)
		return 0;

	return 1;
}

/*
 * Makes the pending commit the front once it is taken, letting go of the
 * former front and of an unsettled buffer.
 */
unsigned
powervr_flip_taken(
	struct powervr_flip *flip,
	uint64_t sequence,
	struct powervr_flip_hold **freed)
{
	struct powervr_flip_hold *former;
	struct powervr_flip_hold *unsettled;
	unsigned count;
	int due;

	/* Nothing to take without a commit. */
	count = 0;
	if (!flip->committing) {
		flip->mismatches++;
		return 0;
	}

	/* A commit counted taken before its blanks is out of the promised order. */
	due = powervr_flip_due(flip->base, sequence);
	if (!due)
		flip->mismatches++;

	/* The pending buffer is on the screen; the commit is over and the output is back. */
	former = flip->front;
	unsettled = flip->unsettled;
	flip->front = flip->pending;
	flip->pending = NULL;
	flip->unsettled = NULL;
	flip->committing = 0;
	flip->lost = 0;
	record(flip, POWERVR_FLIP_TAKEN, flip->front, sequence);

	/*
	 * Lets go of the former front only now, after the new one was taken:
	 * the controller read it until then.  A buffer committed twice in a
	 * row is held twice, so this drops only the older hold.
	 */
	if (former != NULL) {
		if (drop(flip, former, sequence)) {
			freed[count] = former;
			count++;
		}
	}

	/* Lets go of the buffer committed before the output was lost; it was not taken after this one. */
	if (unsettled != NULL) {
		if (drop(flip, unsettled, sequence)) {
			freed[count] = unsettled;
			count++;
		}
	}

	return count;
}

/*
 * Marks the output lost: the pending commit was not seen taken in time.
 */
void
powervr_flip_lose(
	struct powervr_flip *flip,
	uint64_t sequence)
{
	/* Nothing to lose without a commit. */
	if (!flip->committing) {
		flip->mismatches++;
		return;
	}

	/*
	 * The controller may still take the pending buffer later, so it keeps
	 * its hold as unsettled.  A commit lost while one is already unsettled
	 * is the front written back again; the front holds that buffer, so its
	 * extra hold is dropped and nothing is freed.  Anything else is out of
	 * order and its hold is kept, leaking rather than freeing early.
	 */
	if (flip->unsettled == NULL) {
		flip->unsettled = flip->pending;
	} else if (flip->pending == flip->front) {
		if (flip->pending != NULL)
			(void)drop(flip, flip->pending, sequence);
	} else {
		flip->mismatches++;
	}

	flip->pending = NULL;
	flip->committing = 0;
	flip->lost = 1;
	record(flip, POWERVR_FLIP_LOST, flip->unsettled, sequence);
}

/*
 * Lets go of a buffer its owner destroys: reports whether it can be freed
 * now or is left to the last hold's drop.
 */
int
powervr_flip_destroy(
	struct powervr_flip *flip,
	struct powervr_flip_hold *buffer,
	uint64_t sequence)
{
	/* Records the owner's letting go, with the holds it found. */
	record(flip, POWERVR_FLIP_DESTROY, buffer, sequence);

	/* A buffer nothing holds goes at once. */
	if (buffer->holds == 0)
		return 1;

	/* The last drop frees it. */
	buffer->destroyed = 1;
	return 0;
}

/* Drops one hold of a buffer and reports whether the buffer is to be freed now. */
static int
drop(
	struct powervr_flip *flip,
	struct powervr_flip_hold *buffer,
	uint64_t sequence)
{
	/* A drop below zero is out of order; nothing is freed for it. */
	if (buffer->holds == 0) {
		flip->mismatches++;
		return 0;
	}

	buffer->holds--;
	record(flip, POWERVR_FLIP_DROP, buffer, sequence);

	/* Freed when nothing holds it and its owner let it go. */
	if (buffer->holds != 0)
		return 0;
	if (!buffer->destroyed)
		return 0;

	return 1;
}

/* Appends one event to the ring, overwriting the oldest when it is full. */
static void
record(
	struct powervr_flip *flip,
	enum powervr_flip_event event,
	const struct powervr_flip_hold *buffer,
	uint64_t sequence)
{
	struct powervr_flip_record *entry;

	/* Fills the next slot and moves on. */
	entry = &flip->ring[flip->next];
	entry->event = (uint32_t)event;
	entry->holds = 0;
	if (buffer != NULL)
		entry->holds = buffer->holds;
	entry->buffer = (uintptr_t)buffer;
	entry->sequence = sequence;
	entry->base = flip->base;
	flip->next = (flip->next + 1U) % POWERVR_FLIP_RING;
	flip->recorded++;
}
