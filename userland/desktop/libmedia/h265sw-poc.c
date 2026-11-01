/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The picture order count of H.265 and the order counts of a reference
 * picture set (8.3.1, 8.3.2).
 */

#include "h265sw-poc.h"

#include <errno.h>

/*
 * Resets the state (the start of a stream, or after a reset of the
 * decoding).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-poc.h.
 */
void
h265sw_poc_init(
	struct h265sw_poc_state *state)
{
	UNUSED_PARAMETER(state);

	/* Does nothing yet. */
	return;
}

/*
 * Derives PicOrderCntVal of a picture (8.3.1, 8-1 and 8-2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-poc.h.
 */
int
h265sw_poc_compute(
	const struct h265sw_poc_state *state,
	const struct h265sw_slice_header *header,
	int irap_no_rasl,
	unsigned log2_max_poc_lsb,
	int32_t *poc)
{
	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(irap_no_rasl);
	UNUSED_PARAMETER(log2_max_poc_lsb);
	UNUSED_PARAMETER(poc);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Records a decoded picture as prevTid0Pic when it qualifies.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-poc.h.
 */
void
h265sw_poc_finish(
	struct h265sw_poc_state *state,
	const struct h265sw_slice_header *header,
	int32_t poc)
{
	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(poc);

	/* Does nothing yet. */
	return;
}

/*
 * Derives the order counts of the reference picture set (8.3.2, 8-5).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-poc.h.
 */
int
h265sw_poc_rps(
	const struct h265sw_slice_header *header,
	int32_t poc,
	unsigned log2_max_poc_lsb,
	struct h265sw_rps_pocs *out)
{
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(poc);
	UNUSED_PARAMETER(log2_max_poc_lsb);
	UNUSED_PARAMETER(out);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
