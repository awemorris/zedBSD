/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Sequential prediction is separate from queue admission and backend I/O. */
#include <kern/readahead.h>
#include <kern/page.h>
#include <errno.h>
#include <string.h>

/*
 * Invalidates outstanding stream work without ever reusing an exhausted generation.
 */
void
readahead_reset(
	struct readahead_state *state)
{
	/* Keeps a missing optional owner harmless. */
	if (state == NULL)
		return;
	state->valid = 0;
	state->sequential = 0;
	state->window = 0;
	state->useful = 0;
	state->next = 0;
	state->issued_end = 0;

	/* Saturation permanently disables this description instead of admitting ABA. */
	if (state->generation == UINT64_MAX) {
		state->exhausted = 1;
		return;
	}
	state->generation++;
}

/*
 * Plans one bounded missing window after consecutive successful demand reads.
 */
int
readahead_observe(
	struct readahead_state *state,
	uint64_t offset,
	size_t length,
	uint64_t eof,
	uint64_t useful,
	int pressure,
	struct readahead_request *request)
{
	uint64_t end;
	uint64_t first;
	uint64_t limit;
	uint64_t eof_rounded;
	uint64_t available;

	/* Rejects impossible demand ranges without overflowing endpoint arithmetic. */
	if (state == NULL || request == NULL)
		return EINVAL;
	memset(request, 0, sizeof(*request));
	if (eof > INT64_MAX || offset > eof || (uint64_t)length > eof - offset) {
		readahead_reset(state);
		return EINVAL;
	}
	if (pressure || length == 0) {
		readahead_reset(state);
		return 0;
	}
	if (state->exhausted)
		return 0;
	end = offset + (uint64_t)length;

	/* Starts a new stream and cancels prior work on random or reordered access. */
	if (state->valid && offset != state->next)
		readahead_reset(state);
	if (state->exhausted)
		return 0;
	if (!state->valid) {
		if (state->generation == 0)
			state->generation = 1;
		state->valid = 1;
		state->next = end;
		state->issued_end = end;
		state->sequential = 1;
		state->window = READAHEAD_MIN_WINDOW;
		return 0;
	}

	/* Grows only after useful prefetched bytes justify a larger lookahead window. */
	state->next = end;
	if (state->sequential < 2)
		state->sequential++;
	if (useful > READAHEAD_MIN_WINDOW - state->useful)
		useful = READAHEAD_MIN_WINDOW - state->useful;
	state->useful += useful;
	if (state->useful == READAHEAD_MIN_WINDOW)
		state->window = READAHEAD_MAX_WINDOW;

	/* Bounds issued bytes relative to demand progress, even for one-byte reads. */
	first = (end + ZEDBSD_PAGE_SIZE - 1U) & ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
	if (first >= eof)
		return 0;
	limit = first + state->window;
	eof_rounded = (eof + ZEDBSD_PAGE_SIZE - 1U) & ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
	if (limit > eof_rounded)
		limit = eof_rounded;
	if (first < state->issued_end)
		first = state->issued_end;
	if (first >= limit || first >= eof)
		return 0;
	available = limit - first;

	/* Refills in half-window runs instead of submitting another page on every read. */
	if (available < READAHEAD_MIN_WINDOW / 2U && limit != eof_rounded)
		return 0;
	if (available > READAHEAD_REQUEST_MAX)
		available = READAHEAD_REQUEST_MAX;

	/* Transfers a candidate generation to the separately bounded queue owner. */
	request->generation = state->generation;
	request->offset = first;
	request->length = (size_t)available;
	state->issued_end = first + available;
	return 0;
}

/*
 * Rejects work invalidated by a seek, pressure, close, or stream discontinuity.
 */
int
readahead_current(
	const struct readahead_state *state,
	const struct readahead_request *request)
{
	/* The queue must separately revalidate current VM identity and EOF. */
	if (state == NULL || request == NULL || !state->valid || state->exhausted ||
	    request->length == 0 || request->generation != state->generation)
		return 0;
	return 1;
}
