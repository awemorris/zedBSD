/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The job markers of a GHCI session.
 *
 * A marker is reserved with the core's completion before userspace
 * submits, committed to the ticket of the last submission of its queue,
 * and completed when that ticket is answered: passed, or ended by a reset
 * or the loss of the device.  The tickets are judged by the caller, who
 * knows the device's fence and its reset generations.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "ghci-words.h"

/*
 * Empties a session's table of job markers.
 */
void
ghci_jobs_init(
	struct ghci_job_table *table)
{
	unsigned index;

	/* Every marker is free and holds nothing. */
	for (index = 0U; index < GHCI_JOB_MARKERS; index++) {
		table->markers[index].state = GHCI_JOB_FREE;
		table->markers[index].completion = NULL;
		table->markers[index].seq = 0U;
		table->markers[index].generation = 0U;
	}

	/* No marker is reserved or waiting for a ticket. */
	table->reserved = 0U;
	table->committed = 0U;
}

/*
 * Takes a free marker for a completion.
 */
int
ghci_jobs_reserve(
	struct ghci_job_table *table,
	void *completion,
	unsigned *index)
{
	unsigned at;

	/* The first free marker becomes the reservation. */
	for (at = 0U; at < GHCI_JOB_MARKERS; at++) {
		if (table->markers[at].state == GHCI_JOB_FREE) {
			table->markers[at].state = GHCI_JOB_RESERVED;
			table->markers[at].completion = completion;
			table->reserved++;
			*index = at;
			return 0;
		}
	}

	/* Every marker is in use: the producer waits for capacity. */
	return EAGAIN;
}

/*
 * Binds a reserved marker to a ticket.
 */
int
ghci_jobs_commit(
	struct ghci_job_table *table,
	unsigned index,
	void *completion,
	uint64_t seq,
	uint64_t generation,
	int *passed)
{
	struct ghci_job_marker *marker;

	/* Refuses an index outside the table. */
	*passed = 0;
	if (index >= GHCI_JOB_MARKERS)
		return ESTALE;

	/* Refuses a marker that is not this completion's reservation. */
	marker = &table->markers[index];
	if (marker->state != GHCI_JOB_RESERVED)
		return ESTALE;
	if (marker->completion != completion)
		return ESTALE;

	/* The reservation ends either way. */
	table->reserved--;

	/* No work outstanding completes the marker at once: the caller does it. */
	if (seq == 0U) {
		marker->state = GHCI_JOB_FREE;
		marker->completion = NULL;
		*passed = 1;
		return 0;
	}

	/* Otherwise the marker waits for the ticket; it now counts as committed. */
	marker->state = GHCI_JOB_COMMITTED;
	marker->seq = seq;
	marker->generation = generation;
	table->committed++;

	/* Succeeded: the marker completes when the ticket is answered. */
	return 0;
}

/*
 * Withdraws a reserved marker or ends it with an error.
 */
int
ghci_jobs_cancel(
	struct ghci_job_table *table,
	unsigned index,
	void *completion,
	unsigned fault)
{
	struct ghci_job_marker *marker;

	/* Only a normal and a fault cancel exist. */
	if (fault > 1U)
		return EINVAL;

	/* Refuses an index outside the table. */
	if (index >= GHCI_JOB_MARKERS)
		return ESTALE;

	/* Refuses a marker that does not hold this completion. */
	marker = &table->markers[index];
	if (marker->state == GHCI_JOB_FREE)
		return ESTALE;
	if (marker->completion != completion)
		return ESTALE;

	/* A normal cancel withdraws a reservation only; nothing completes. */
	if (fault == 0U) {
		/* A committed marker is not withdrawn: it completes with its ticket. */
		if (marker->state != GHCI_JOB_RESERVED)
			return ESTALE;

		marker->state = GHCI_JOB_FREE;
		marker->completion = NULL;
		table->reserved--;
		return 0;
	}

	/* A fault cancel of a reservation keeps the completion, which ends with an error. */
	if (marker->state == GHCI_JOB_RESERVED) {
		marker->state = GHCI_JOB_FAILED;
		table->reserved--;
		table->committed++;
		return 0;
	}

	/* A fault cancel of a committed marker also ends it with an error. */
	if (marker->state == GHCI_JOB_COMMITTED)
		marker->state = GHCI_JOB_FAILED;

	/* Succeeded: the marker ends with an error at the next retirement. */
	return 0;
}

/*
 * Reports how many markers a reservation could take now.
 */
unsigned
ghci_jobs_capacity(
	const struct ghci_job_table *table)
{
	/* Reports the markers neither reserved nor waiting. */
	return GHCI_JOB_MARKERS - table->reserved - table->committed;
}

/*
 * Reports the index of the marker a token points at.
 */
int
ghci_jobs_index(
	const struct ghci_job_table *table,
	const void *token,
	unsigned *index)
{
	unsigned at;

	/* Compares the token with each marker's address before it is used. */
	for (at = 0U; at < GHCI_JOB_MARKERS; at++) {
		if (token == (const void *)&table->markers[at]) {
			*index = at;
			return 0;
		}
	}

	/* The token is not one of the session's markers. */
	return ESTALE;
}

/*
 * Frees every marker that is done and hands its completion back.
 */
unsigned
ghci_jobs_retire(
	struct ghci_job_table *table,
	ghci_job_judge judge,
	void *context,
	void **done,
	int *errors,
	unsigned capacity)
{
	struct ghci_job_marker *marker;
	enum ghci_job_answer answer;
	unsigned count;
	unsigned at;

	/* Looks at every marker that waits, while the caller has room for it. */
	count = 0U;
	for (at = 0U; at < GHCI_JOB_MARKERS; at++) {
		/* Stops once the caller's arrays are full; the rest wait for the next call. */
		if (count == capacity)
			break;

		/* A failed marker completes with an error. */
		marker = &table->markers[at];
		if (marker->state == GHCI_JOB_FAILED) {
			done[count] = marker->completion;
			errors[count] = EIO;
			count++;
			marker->state = GHCI_JOB_FREE;
			marker->completion = NULL;
			table->committed--;
			continue;
		}

		/* Markers that are free or still reserved wait for nothing here. */
		if (marker->state != GHCI_JOB_COMMITTED)
			continue;

		/* A committed marker completes once its ticket is answered: passed, or lost to a reset. */
		answer = judge(context, marker->seq, marker->generation);
		if (answer == GHCI_JOB_WAITING)
			continue;
		done[count] = marker->completion;
		errors[count] = 0;
		if (answer == GHCI_JOB_LOST)
			errors[count] = EIO;
		count++;
		marker->state = GHCI_JOB_FREE;
		marker->completion = NULL;
		table->committed--;
	}

	/* Reports the number of completions handed back. */
	return count;
}

/*
 * Turns every committed marker into a failed one.
 */
void
ghci_jobs_fail(
	struct ghci_job_table *table)
{
	unsigned at;

	/* The work the markers wait for will never complete. */
	for (at = 0U; at < GHCI_JOB_MARKERS; at++) {
		if (table->markers[at].state == GHCI_JOB_COMMITTED)
			table->markers[at].state = GHCI_JOB_FAILED;
	}
}
