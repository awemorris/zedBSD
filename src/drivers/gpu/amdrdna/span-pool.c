/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pools of addresses handed out in aligned runs.
 *
 * The driver hands out three kinds of address from pools of this kind:
 * VRAM the CPU reaches through its window, VRAM only the GPU reaches, and
 * GPU addresses of the table that maps system memory.  A pool keeps its
 * free runs sorted by address; taking a run splits the first free run that
 * holds an aligned piece of the asked size, and giving one back merges it
 * with the free runs it touches.  The functions only compute, so the same
 * code runs in the kernel and in the host tests; the kernel serializes the
 * calls on one pool.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

static int insert_span(struct amdrdna_pool *pool, unsigned at, uint64_t start, uint64_t end);
static void remove_span(struct amdrdna_pool *pool, unsigned at);

/*
 * Starts a pool over [start, end) with nothing free in it.
 */
void
amdrdna_pool_init(
	struct amdrdna_pool *pool,
	uint64_t start,
	uint64_t end)
{
	/* Records the bounds; the owner gives the runs it may hand out. */
	pool->start = start;
	pool->end = end;
	pool->span_count = 0U;
	pool->free_bytes = 0U;
}

/*
 * Gives a run of addresses to the pool, merging it with the free runs it
 * touches.
 *
 * Returns 0, EINVAL for an empty run, a run outside the pool or one that
 * overlaps a free run, or ENOSPC when the pool cannot track another run.
 */
int
amdrdna_pool_give(
	struct amdrdna_pool *pool,
	uint64_t start,
	uint64_t bytes)
{
	struct amdrdna_span *before;
	struct amdrdna_span *after;
	uint64_t end;
	unsigned at;
	bool joins_before;
	bool joins_after;
	int error;

	/* An empty run, or one that leaves the pool, is refused. */
	if (bytes == 0U)
		return EINVAL;
	if (start < pool->start || start >= pool->end)
		return EINVAL;
	if (bytes > pool->end - start)
		return EINVAL;

	/* The run's end, which cannot wrap inside the pool. */
	end = start + bytes;

	/* Finds the first free run that starts at or after the given one. */
	for (at = 0U; at < pool->span_count; at++) {
		/* Stops at the first run that does not lie wholly below. */
		if (pool->spans[at].start >= start)
			break;
	}

	/* The run must not overlap the free run below it or the one above it. */
	before = NULL;
	after = NULL;
	if (at > 0U)
		before = &pool->spans[at - 1U];
	if (at < pool->span_count)
		after = &pool->spans[at];
	if (before != NULL && before->end > start)
		return EINVAL;
	if (after != NULL && after->start < end)
		return EINVAL;

	/* Works out which neighbours the run touches. */
	joins_before = false;
	joins_after = false;
	if (before != NULL && before->end == start)
		joins_before = true;
	if (after != NULL && after->start == end)
		joins_after = true;

	/* Merges the run into its neighbours, or adds it as a run of its own. */
	if (joins_before && joins_after) {
		/* The run closes the gap: the lower neighbour takes the upper one. */
		before->end = after->end;
		remove_span(pool, at);
	} else if (joins_before) {
		/* The lower neighbour grows upwards. */
		before->end = end;
	} else if (joins_after) {
		/* The upper neighbour grows downwards. */
		after->start = start;
	} else {
		/* The run stands alone. */
		error = insert_span(pool, at, start, end);
		if (error != 0)
			return error;
	}

	/* Counts the bytes now free. */
	pool->free_bytes += bytes;

	/* Succeeded: the run can be taken again. */
	return 0;
}

/*
 * Keeps a run of addresses out of the pool: whatever part of it is free is
 * no longer handed out.  Parts that are not free are left as they are.
 */
void
amdrdna_pool_hold(
	struct amdrdna_pool *pool,
	uint64_t start,
	uint64_t bytes)
{
	struct amdrdna_span *span;
	uint64_t end;
	uint64_t cut_start;
	uint64_t cut_end;
	unsigned at;
	int error;

	/* An empty run holds nothing. */
	if (bytes == 0U)
		return;

	/* The run's end; a run past the address space is clipped to its top. */
	end = start + bytes;
	if (end < start)
		end = UINT64_MAX;

	/* Cuts the run out of every free run it overlaps. */
	at = 0U;
	while (at < pool->span_count) {
		/* Skips a free run that does not overlap. */
		span = &pool->spans[at];
		if (span->end <= start || span->start >= end) {
			at++;
			continue;
		}

		/* The overlapping part, which is free no longer. */
		cut_start = span->start;
		if (start > cut_start)
			cut_start = start;
		cut_end = span->end;
		if (end < cut_end)
			cut_end = end;
		pool->free_bytes -= cut_end - cut_start;

		/* Keeps what lies on each side of the cut. */
		if (cut_start > span->start && cut_end < span->end) {
			/* The cut lies inside: the run splits in two, the upper part after it. */
			error = insert_span(pool, at + 1U, cut_end, span->end);
			if (error != 0) {
				/* No room for the upper part: it is lost to the pool rather than handed out. */
				pool->free_bytes -= span->end - cut_end;
			}

			/* The lower part keeps the run's place; the search goes on past the upper part. */
			pool->spans[at].end = cut_start;
			at += 2U;
		} else if (cut_start > span->start) {
			/* The cut takes the top of the run. */
			span->end = cut_start;
			at++;
		} else if (cut_end < span->end) {
			/* The cut takes the bottom of the run. */
			span->start = cut_end;
			at++;
		} else {
			/* The cut takes the whole run. */
			remove_span(pool, at);
		}
	}
}

/*
 * Takes a run of bytes, aligned to alignment (a power of two), from the
 * lowest free run that holds one.
 *
 * Returns 0 with the run's start, EINVAL for an empty run or an alignment
 * that is not a power of two, ENOMEM when no free run holds it, or ENOSPC
 * when splitting a run would need more runs than the pool tracks.
 */
int
amdrdna_pool_take(
	struct amdrdna_pool *pool,
	uint64_t bytes,
	uint64_t alignment,
	uint64_t *start)
{
	struct amdrdna_span *span;
	uint64_t aligned;
	uint64_t end;
	unsigned at;
	int error;

	/* An empty run, or an alignment that is not a power of two, is refused. */
	if (bytes == 0U)
		return EINVAL;
	if (alignment == 0U || (alignment & (alignment - 1U)) != 0U)
		return EINVAL;

	/* Finds the lowest free run that holds an aligned piece of the size. */
	for (at = 0U; at < pool->span_count; at++) {
		/* Aligns the run's start up; a start that wraps cannot hold the piece. */
		span = &pool->spans[at];
		aligned = (span->start + alignment - 1U) & ~(alignment - 1U);
		if (aligned < span->start)
			continue;

		/* Skips a run too short for the piece after its aligned start. */
		if (aligned >= span->end)
			continue;
		if (bytes > span->end - aligned)
			continue;

		/* The piece's end, which lies inside the run. */
		end = aligned + bytes;

		/* Takes the piece out of the run, keeping what lies on each side. */
		if (aligned > span->start && end < span->end) {
			/* The piece lies inside: the upper part becomes a run of its own. */
			error = insert_span(pool, at + 1U, end, span->end);
			if (error != 0)
				return error;

			/* The lower part keeps the run's place. */
			pool->spans[at].end = aligned;
		} else if (aligned > span->start) {
			/* The piece takes the top of the run. */
			span->end = aligned;
		} else if (end < span->end) {
			/* The piece takes the bottom of the run. */
			span->start = end;
		} else {
			/* The piece is the whole run. */
			remove_span(pool, at);
		}

		/* Counts the bytes taken and hands out the piece. */
		pool->free_bytes -= bytes;
		*start = aligned;

		/* Succeeded: the run is the caller's until it gives it back. */
		return 0;
	}

	/* No free run holds the piece. */
	return ENOMEM;
}

/* Inserts a free run at a position of the sorted list, moving the later runs up. */
static int
insert_span(
	struct amdrdna_pool *pool,
	unsigned at,
	uint64_t start,
	uint64_t end)
{
	unsigned index;

	/* The list holds a fixed number of runs. */
	if (pool->span_count == AMDRDNA_POOL_SPANS)
		return ENOSPC;

	/* Moves the runs from the position up by one, from the top down. */
	for (index = pool->span_count; index > at; index--)
		pool->spans[index] = pool->spans[index - 1U];

	/* Stores the new run. */
	pool->spans[at].start = start;
	pool->spans[at].end = end;
	pool->span_count++;

	/* Succeeded: the run is in the list. */
	return 0;
}

/* Removes the free run at a position of the sorted list, moving the later runs down. */
static void
remove_span(
	struct amdrdna_pool *pool,
	unsigned at)
{
	unsigned index;

	/* Moves the runs above the position down by one. */
	for (index = at; index + 1U < pool->span_count; index++)
		pool->spans[index] = pool->spans[index + 1U];

	/* The list is one run shorter. */
	pool->span_count--;
}
