/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The buffer cache's sequential-stream prediction for raw block devices.
 *
 * One stream follows the reads of one open block device.  When a read
 * starts where the previous one ended twice in a row, the stream proposes
 * the blocks after it, up to a window ahead of the reader, so that the
 * cache can read them while the reader copies what it has.  Any other read
 * starts the stream again and changes its generation, which makes the
 * work proposed before stale.
 *
 * Pure bookkeeping with no lock and no memory of its own, kept apart so
 * that the host can test it.  The caller serializes every call on one
 * stream.
 */

#ifndef KERN_BUF_READAHEAD_H
#define KERN_BUF_READAHEAD_H

#include <stdint.h>

/*
 * One stream's prediction.
 *
 * next is the block the next sequential read starts at, issued the end of
 * the blocks already proposed, and sequential how many reads in a row
 * continued the stream (saturating at 2).  valid is zero for a stream that
 * has seen no read.
 */
struct buf_readahead_stream {
	uint64_t next;
	uint64_t issued;
	uint64_t generation;
	unsigned sequential;
	unsigned valid;
};

/* Forgets a stream's reads and makes everything proposed before stale. */
static inline void
buf_readahead_restart(
	struct buf_readahead_stream *stream)
{
	stream->valid = 0;
	stream->sequential = 0;
	stream->next = 0;
	stream->issued = 0;
	stream->generation++;
}

/*
 * Records one completed read of count blocks at block, on a device of
 * limit blocks, and proposes the blocks to read ahead.
 *
 * The proposal is [*start, *start + *length); a zero length proposes
 * nothing.  It reaches at most window blocks past the end of the read and
 * is at least unit blocks long unless it reaches the device's end.  The
 * caller records what it took with buf_readahead_taken().
 */
static inline void
buf_readahead_plan(
	struct buf_readahead_stream *stream,
	uint64_t block,
	uint64_t count,
	uint64_t limit,
	uint64_t window,
	uint64_t unit,
	uint64_t *start,
	uint64_t *length)
{
	uint64_t end;
	uint64_t want;

	*start = 0;
	*length = 0;

	/* An impossible read restarts the stream. */
	if (count == 0 ||
	    block > limit ||
	    count > limit - block) {
		buf_readahead_restart(stream);
		return;
	}
	end = block + count;

	/* A read elsewhere starts a new stream. */
	if (!stream->valid || block != stream->next) {
		stream->generation++;
		stream->valid = 1;
		stream->sequential = 1;
		stream->next = end;
		stream->issued = end;
		return;
	}

	/* A read that continues the stream. */
	if (stream->sequential < 2U)
		stream->sequential++;
	stream->next = end;
	if (stream->issued < end)
		stream->issued = end;

	/* Proposes the missing part of the window once the stream is sequential. */
	if (stream->sequential < 2U || window == 0)
		return;
	want = limit;
	if (window < limit - end)
		want = end + window;
	if (want <= stream->issued)
		return;
	if (want - stream->issued < unit && want != limit)
		return;
	*start = stream->issued;
	*length = want - stream->issued;
}

/* Records that the blocks before end have been handed to the readers ahead. */
static inline void
buf_readahead_taken(
	struct buf_readahead_stream *stream,
	uint64_t end)
{
	if (end > stream->issued)
		stream->issued = end;
}

/*
 * Forgets the blocks proposed but not read, so that the next sequential read
 * proposes them again instead of leaving a hole.
 */
static inline void
buf_readahead_rewind(
	struct buf_readahead_stream *stream)
{
	stream->issued = stream->next;
}

/*
 * Forgets the blocks proposed from block on, which were not taken after all,
 * so that the next sequential read proposes them again.
 */
static inline void
buf_readahead_rewind_to(
	struct buf_readahead_stream *stream,
	uint64_t block)
{
	if (block < stream->next)
		block = stream->next;
	if (block < stream->issued)
		stream->issued = block;
}

#endif
