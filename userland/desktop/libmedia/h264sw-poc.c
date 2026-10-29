/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The picture order count of the software H.264 decoder (8.2.1).
 */

#include "h264sw-poc.h"

#include <errno.h>
#include <stddef.h>

/*
 * Clears the state as before the first picture of a stream.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-poc.h.
 */
void
h264sw_poc_init(
	struct h264sw_poc_state *state)
{
	UNUSED_PARAMETER(state);

	/* Does nothing yet. */
	return;
}

/*
 * Derives the order counts of a frame from its first slice's header (8.2.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-poc.h.
 */
int
h264sw_poc_compute(
	const struct h264sw_poc_state *state,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	struct h264sw_poc *poc)
{
	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(poc);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Carries the state past a decoded picture, and resets its counts when it had operation 5 (8.2.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-poc.h.
 */
void
h264sw_poc_finish(
	struct h264sw_poc_state *state,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	struct h264sw_poc *poc)
{
	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(poc);

	/* Does nothing yet. */
	return;
}
