/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The weighted sample prediction of the software H.264 decoder (8.4.2.3,
 * 8.4.3).
 */

#include "h264sw-weight.h"

#include <errno.h>
#include <stddef.h>

/*
 * Prepares the weights of a slice (8.4.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-weight.h.
 */
void
h264sw_weights_setup(
	struct h264sw_weights *weights,
	const struct h264sw_pps *pps,
	const struct h264sw_slice_header *header,
	const struct h264sw_reflist *lists,
	int32_t poc)
{
	UNUSED_PARAMETER(weights);
	UNUSED_PARAMETER(pps);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(lists);
	UNUSED_PARAMETER(poc);

	/* Does nothing yet. */
	return;
}

/*
 * Gives the factors for one partition's prediction (8.4.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-weight.h.
 */
void
h264sw_weights_of(
	const struct h264sw_weights *weights,
	int ref_idx0,
	int ref_idx1,
	unsigned component,
	struct h264sw_weight_factors *factors)
{
	UNUSED_PARAMETER(weights);
	UNUSED_PARAMETER(ref_idx0);
	UNUSED_PARAMETER(ref_idx1);
	UNUSED_PARAMETER(component);
	UNUSED_PARAMETER(factors);

	/* Does nothing yet. */
	return;
}

/*
 * Combines the predictions of a block into the final prediction (8.4.2.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-weight.h.
 */
void
h264sw_weights_apply(
	const struct h264sw_weight_factors *factors,
	const uint8_t *p0,
	const uint8_t *p1,
	size_t p_stride,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	UNUSED_PARAMETER(factors);
	UNUSED_PARAMETER(p0);
	UNUSED_PARAMETER(p1);
	UNUSED_PARAMETER(p_stride);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(out_stride);

	/* Does nothing yet. */
	return;
}
