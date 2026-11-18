/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of a block device transfer that bypasses the cache
 * (O_DIRECT): which requests it accepts, and how a request is cut into
 * pieces that go straight to the caller's pages and pieces that go through
 * a staging buffer.  Pure, so that the host can test it.
 */

#ifndef KERN_BUF_DIRECT_H
#define KERN_BUF_DIRECT_H

#include <stddef.h>
#include <stdint.h>

/* The most one piece of a direct transfer moves: one request of the largest a driver takes. */
#define BUF_DIRECT_PIECE_BYTES	(4U * 1024U * 1024U)

/* The most one staged piece moves, through a staging buffer of the kernel's heap. */
#define BUF_DIRECT_STAGED_BYTES	(1024U * 1024U)

/*
 * Reports 1 when a direct transfer of length bytes at a byte offset of a
 * device of block_size bytes is not whole blocks (the caller refuses it
 * with EINVAL), and 0 when it is.
 */
static inline int
buf_direct_misaligned(
	int64_t offset,
	size_t length,
	uint32_t block_size)
{
	/* A device without blocks or a negative offset takes nothing. */
	if (block_size == 0U || offset < 0)
		return 1;

	/* Both ends must fall on block boundaries. */
	if ((uint64_t)offset % block_size != 0U || length % block_size != 0U)
		return 1;
	return 0;
}

/*
 * The bytes of the next piece that can go straight to the caller's pages: a
 * whole number of pages from a page boundary of the caller's buffer, at most
 * piece_max.  Zero means the piece goes through staging: the caller's byte
 * is inside a page, or less than a page remains.
 */
static inline size_t
buf_direct_page_piece(
	size_t user_page_offset,
	size_t remaining,
	size_t page_size,
	size_t piece_max)
{
	size_t bytes;

	/* A buffer that does not start a page, or a short rest, is staged. */
	if (page_size == 0U || user_page_offset != 0U || remaining < page_size)
		return 0;

	/* Takes the whole pages left, up to the bound of one piece. */
	bytes = remaining;
	if (bytes > piece_max)
		bytes = piece_max;
	return bytes - bytes % page_size;
}

/*
 * The bytes of the next piece that goes through staging: up to the next
 * page boundary of the caller's buffer when the rest could then go to the
 * pages, else as much as one piece holds; always whole blocks.
 */
static inline size_t
buf_direct_staged_piece(
	size_t user_page_offset,
	size_t remaining,
	size_t page_size,
	uint32_t block_size,
	size_t piece_max)
{
	size_t bytes;

	/* Reaches the caller's next page boundary when that is a block boundary too. */
	bytes = remaining;
	if (user_page_offset != 0U && page_size != 0U &&
	    (page_size - user_page_offset) % block_size == 0U &&
	    page_size - user_page_offset < bytes)
		bytes = page_size - user_page_offset;

	/* Takes at most one piece, in whole blocks. */
	if (bytes > piece_max)
		bytes = piece_max;
	if (block_size != 0U)
		bytes -= bytes % block_size;
	return bytes;
}

#endif
