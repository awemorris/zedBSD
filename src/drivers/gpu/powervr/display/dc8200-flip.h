/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bookkeeping of a display controller that scans a program's buffer
 * directly: which buffers the controller reads or may still read, when a
 * committed plane counts as taken, and a ring of what happened, in order,
 * for a test to check afterwards.
 *
 * Only values go in and out -- the buffers are named by the hold each one
 * carries -- so the rules are the same in the kernel and in a host test.
 * The caller serializes every call and frees a buffer only when a call
 * hands it back.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_DC8200_FLIP_H
#define KERN_DRIVERS_GPU_POWERVR_DC8200_FLIP_H

#include <stdint.h>

/*
 * How many vertical blanks after the commit's sequence a committed plane
 * counts as taken.
 *
 * The commit lands inside some frame; the blank ending that frame may
 * already be under way when the commit is written, so the plane is taken
 * at the latest at the blank after it.  This rests on the interrupt
 * coming at or after the point the controller takes its shadowed
 * registers, which the interrupt's scan-line record lets a test check.
 */
#define POWERVR_FLIP_LATCH_BLANKS	2U

/* How many events the ring keeps; the oldest are overwritten. */
#define POWERVR_FLIP_RING		64U

/*
 * What the controller does with one buffer, as the flip counts it.
 *
 * Holds counts the places the controller reads the buffer from or may
 * still read it from: the front, the pending commit and an unsettled
 * commit, so at most three.  Destroyed is set when the buffer's owner let
 * it go while it was held; the buffer is freed when the last hold goes.
 * Embedded in the display's resource, which lives until then.
 */
struct powervr_flip_hold {
	uint32_t holds;
	int destroyed;
};

/* The kinds of event the ring records. */
enum powervr_flip_event {
	POWERVR_FLIP_COMMIT = 1,
	POWERVR_FLIP_TAKEN,
	POWERVR_FLIP_DROP,
	POWERVR_FLIP_LOST,
	POWERVR_FLIP_DESTROY
};

/*
 * One event in the ring: what happened, to which buffer, and the
 * vertical-blank sequence it happened at.  Buffer is the hold's address,
 * zero for the loader's framebuffer.  Base is the commit's sequence the
 * event belongs to.
 */
struct powervr_flip_record {
	uint32_t event;
	uint32_t holds;
	uintptr_t buffer;
	uint64_t sequence;
	uint64_t base;
};

/*
 * The flip state of one plane.
 *
 * Front is the buffer the controller scans, NULL for the loader's
 * framebuffer.  Pending is the buffer of a commit written but not yet
 * taken; committing says whether there is one, since NULL names the
 * framebuffer.  Unsettled is a buffer committed before the output was
 * found lost, which the controller may still take; it is let go at the
 * next taken commit.  Lost is set from a lost output until a commit is
 * taken again.
 *
 * Mismatches counts events against the order the rules promise (a commit
 * counted taken before its blanks, a hold dropped below zero); a test
 * expects zero.
 */
struct powervr_flip {
	struct powervr_flip_hold *front;
	struct powervr_flip_hold *pending;
	struct powervr_flip_hold *unsettled;
	int committing;
	int lost;
	uint64_t base;
	uint32_t mismatches;
	uint32_t next;
	uint64_t recorded;
	struct powervr_flip_record ring[POWERVR_FLIP_RING];
};

/* Starts the flip state with the loader's framebuffer on the plane. */
void powervr_flip_init(struct powervr_flip *flip);

/*
 * Records a commit of a buffer (NULL for the loader's framebuffer) at the
 * sequence read right after it was written, and holds the buffer.
 */
void powervr_flip_commit(struct powervr_flip *flip, struct powervr_flip_hold *buffer, uint64_t base);

/*
 * Reports whether a commit written at base counts as taken once the
 * sequence has reached sequence.
 */
int powervr_flip_due(uint64_t base, uint64_t sequence);

/*
 * Makes the pending commit the front once it is taken, letting go of the
 * former front and of an unsettled buffer.
 *
 * Buffers whose last hold went and whose owner already let them go are
 * stored through freed, at most two, and their number is returned; the
 * caller frees them.
 */
unsigned powervr_flip_taken(struct powervr_flip *flip, uint64_t sequence, struct powervr_flip_hold **freed);

/*
 * Marks the output lost: the pending commit was not seen taken in time.
 * Its buffer stays held as unsettled, since the controller may still read
 * it.  The caller then commits the front again.
 */
void powervr_flip_lose(struct powervr_flip *flip, uint64_t sequence);

/*
 * Lets go of a buffer its owner destroys: reports whether it can be freed
 * now (nothing holds it) or is left to the last hold's drop.
 */
int powervr_flip_destroy(struct powervr_flip *flip, struct powervr_flip_hold *buffer, uint64_t sequence);

#endif
