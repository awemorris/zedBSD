/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the GHCI driver that are arithmetic on words: the ring's
 * room and padding, the allocator of GPU addresses, the capability
 * record libvulkan reads and the table of job markers.
 *
 * Nothing declared here touches the kernel or the hardware, and no
 * function takes a lock: the caller serializes every call on one object.
 * A host test runs these functions exactly as the kernel does.
 */

#ifndef DRIVERS_GPU_GHCI_WORDS_H
#define DRIVERS_GPU_GHCI_WORDS_H

#include <stddef.h>
#include <stdint.h>

/* The job markers one session holds at once. */
#define GHCI_JOB_MARKERS		64U

/* The bytes of the capability record libvulkan reads (the native renderer's record). */
#define GHCI_CAPSET_BYTES		176U

/*
 * The states of a job marker.
 *
 * A FREE marker belongs to nobody.  RESERVED holds a completion that the
 * core has not published yet.  COMMITTED is bound to a ticket (a fence
 * sequence and its reset generation) and completes when the ticket is
 * answered.  FAILED completes with an error at the next retirement,
 * whatever the ticket says.
 */
enum ghci_job_state {
	GHCI_JOB_FREE = 0,
	GHCI_JOB_RESERVED,
	GHCI_JOB_COMMITTED,
	GHCI_JOB_FAILED
};

/*
 * One run of GPU addresses in use, kept in address order in the
 * allocator's table.
 */
struct ghci_range_extent {
	uint32_t address;
	uint32_t bytes;
};

/*
 * The allocator of a range [base, limit) of GPU addresses: first fit over
 * a table of the runs in use, sorted by address.
 *
 * The table is the caller's and lives as long as the allocator.  limit is
 * 64 bits wide so that a range may end at 2^32.  count is the number of
 * runs in use; capacity bounds it.
 */
struct ghci_range_map {
	uint64_t base;
	uint64_t limit;
	struct ghci_range_extent *extents;
	unsigned count;
	unsigned capacity;
};

/*
 * The answer a judge gives for the ticket of a committed marker: not yet,
 * passed, or ended by a reset or the loss of the device.
 */
enum ghci_job_answer {
	GHCI_JOB_WAITING = 0,
	GHCI_JOB_PASSED,
	GHCI_JOB_LOST
};

/*
 * One job marker of a session.
 *
 * completion is the core's opaque record, held from the reservation until
 * the marker is retired or withdrawn.  seq and generation are meaningful
 * once the marker is COMMITTED: the ticket whose answer completes it.
 */
struct ghci_job_marker {
	enum ghci_job_state state;
	void *completion;
	uint64_t seq;
	uint64_t generation;
};

/*
 * Answers the ticket of a committed marker; context is the retirer's.
 * The judge runs with the table's lock held and must not sleep.
 */
typedef enum ghci_job_answer (*ghci_job_judge)(void *context, uint64_t seq, uint64_t generation);

/*
 * The job markers of one session.
 *
 * reserved counts the RESERVED markers and committed the COMMITTED and
 * FAILED ones, so the room left is GHCI_JOB_MARKERS less both.  The
 * caller's lock protects the whole table.
 */
struct ghci_job_table {
	struct ghci_job_marker markers[GHCI_JOB_MARKERS];
	unsigned reserved;
	unsigned committed;
};

/*
 * Plans where the next need dwords go in the ring: pad receives how many
 * dwords of padding come first (0, or the rest of the ring up to its end).
 * Returns 0, EAGAIN while the ring has no room yet, or EINVAL for a request
 * no ring of this size can take.
 */
int ghci_ring_plan(uint32_t wptr, uint32_t rptr, uint32_t ring_dwords, uint32_t need, uint32_t *pad);

/* Starts an allocator over [base, limit) with the caller's table. */
void ghci_range_init(struct ghci_range_map *map, uint64_t base, uint64_t limit, struct ghci_range_extent *extents, unsigned capacity);

/*
 * Takes bytes of addresses aligned to alignment (a power of two), the
 * first run that fits.  Returns 0 with the address, ENOMEM when nothing
 * fits, ENOSPC when the table is full, or EINVAL for a bad request.
 */
int ghci_range_alloc(struct ghci_range_map *map, uint32_t bytes, uint32_t alignment, uint32_t *address);

/* Gives back the run that starts at address.  Returns 0 or ENOENT. */
int ghci_range_free(struct ghci_range_map *map, uint32_t address);

/* Reports the bytes of the range that are still free. */
uint64_t ghci_range_free_bytes(const struct ghci_range_map *map);

/* Reports whether [address, address + bytes) lies wholly inside one run in use: 1 when it does. */
int ghci_range_holds(const struct ghci_range_map *map, uint32_t address, uint32_t bytes);

/*
 * Fills the capability record of the native renderer protocol that
 * libvulkan reads into data (GHCI_CAPSET_BYTES bytes).
 */
void ghci_capset_fill(uint8_t *data);

/* Empties a session's table of job markers. */
void ghci_jobs_init(struct ghci_job_table *table);

/* Takes a free marker for completion.  Returns 0 with its index, or EAGAIN when every marker is in use. */
int ghci_jobs_reserve(struct ghci_job_table *table, void *completion, unsigned *index);

/*
 * Binds a reserved marker to a ticket.  A sequence of 0 says that no work
 * is outstanding: the marker is then freed at once, *passed is set and the
 * caller completes it.  Returns 0, or ESTALE when the marker does not hold
 * completion in the reserved state.
 */
int ghci_jobs_commit(struct ghci_job_table *table, unsigned index, void *completion, uint64_t seq, uint64_t generation, int *passed);

/*
 * Withdraws a reserved marker (fault 0: freed, nothing completes) or ends
 * it with an error (fault 1: FAILED, completed at the next retirement).
 * Returns 0, ESTALE for a marker that does not hold completion as a
 * cancel allows, or EINVAL for another fault value.
 */
int ghci_jobs_cancel(struct ghci_job_table *table, unsigned index, void *completion, unsigned fault);

/* Reports how many markers a reservation could take now. */
unsigned ghci_jobs_capacity(const struct ghci_job_table *table);

/* Reports the index of the marker a token points at.  Returns 0, or ESTALE for another pointer. */
int ghci_jobs_index(const struct ghci_job_table *table, const void *token, unsigned *index);

/*
 * Frees every marker that is done: a COMMITTED one whose ticket the judge
 * answers (passed: error 0, lost: EIO), and a FAILED one (EIO).  Each
 * freed marker's completion goes to done[] and its error to errors[], at
 * most capacity of them.  Returns the number written.
 */
unsigned ghci_jobs_retire(struct ghci_job_table *table, ghci_job_judge judge, void *context, void **done, int *errors, unsigned capacity);

/* Turns every COMMITTED marker into FAILED, for a device that lost its work. */
void ghci_jobs_fail(struct ghci_job_table *table);

#endif
