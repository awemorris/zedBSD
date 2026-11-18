/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The buffer cache's record of a line's unjournaled blocks (ws073-p053,
 * BUG-163): one bit per block of the line, the line's first block lowest,
 * set by an ordinary write (outside any journal) and cleared by a
 * journal's pinned write over the block, so that the bits name exactly the
 * blocks whose newest data no journal carries.  A clean write-back of the
 * whole line clears them all.  A line has at most 32 blocks (a 4 KiB page
 * of 512-byte blocks has 8).
 *
 * Pure arithmetic, kept apart so that the host can test it
 * (plan/ws073/tests/host/unjournaled-test.c).
 */

#ifndef KERN_BUF_UNJOURNALED_H
#define KERN_BUF_UNJOURNALED_H

#include <stdint.h>

/* The most blocks one line's record names. */
#define BUF_UNJOURNALED_MAX	32U

/*
 * The bits of count blocks from the line's block offset; a range past the
 * record's width names every block from offset to the end of the record.
 */
static inline uint32_t
buf_unjournaled_bits(
	uint64_t offset,
	uint64_t count)
{
	uint64_t end;
	uint64_t bits;

	/* Nothing past the record. */
	if (offset >= BUF_UNJOURNALED_MAX || count == 0)
		return 0;

	/* The blocks up to the end of the range or of the record. */
	end = offset + count;
	if (end > BUF_UNJOURNALED_MAX)
		end = BUF_UNJOURNALED_MAX;
	bits = ((UINT64_C(1) << end) - 1U) & ~((UINT64_C(1) << offset) - 1U);

	/* The range's bits. */
	return (uint32_t)bits;
}

/*
 * The record after a write of count blocks from offset: an ordinary write
 * adds them, a journal's pinned write (pinned non-zero) takes them out.
 */
static inline uint32_t
buf_unjournaled_after_write(
	uint32_t record,
	uint64_t offset,
	uint64_t count,
	int pinned)
{
	uint32_t bits;

	/* The blocks the write covers. */
	bits = buf_unjournaled_bits(offset, count);

	/* A pinned write's blocks are the journal's now. */
	if (pinned)
		return record & ~bits;

	/* An ordinary write's blocks are unjournaled. */
	return record | bits;
}

/*
 * Finds the next run of unjournaled blocks at or after *start: sets *start
 * to its first block and returns its length, or returns 0 when none is
 * left.
 */
static inline uint32_t
buf_unjournaled_next_run(
	uint32_t record,
	uint32_t *start)
{
	uint32_t first;
	uint32_t end;
	uint32_t bit;

	/* Skips the blocks that are not unjournaled. */
	for (first = *start; first < BUF_UNJOURNALED_MAX; first++) {
		/* Stops at an unjournaled block. */
		bit = (uint32_t)1U << first;
		if ((record & bit) != 0U)
			break;
	}

	/* None is left. */
	if (first == BUF_UNJOURNALED_MAX)
		return 0;

	/* The run goes on while the blocks are unjournaled. */
	for (end = first; end < BUF_UNJOURNALED_MAX; end++) {
		/* Stops at a block that is not. */
		bit = (uint32_t)1U << end;
		if ((record & bit) == 0U)
			break;
	}

	/* The run's first block. */
	*start = first;

	/* Succeeded: the run's length. */
	return end - first;
}

#endif
