/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The intra prediction of the lossy format.
 */

#include "vp8-predict.h"

#include <errno.h>

/*
 * Predicts a block of size by size (16 for luma, 8 for chroma) pixels into
 * dst (stride bytes a row) with mode WEBPC_VP8_DC_PRED, V_PRED, H_PRED or
 * TM_PRED (RFC 6386 12.2, 12.3): DC_PRED the rounded mean of the above row
 * and the left column; of the one available alone (rounded mean of size
 * values); 128 with neither (shift = 3 + have_above + have_left for 16, one
 * less for 8, as 20.14 rounds); V_PRED every row the above row; H_PRED every
 * column the left one; TM_PRED left[r] + above[c] - corner, clamped to
 * 0..255.
 */
void
webpc_vp8_predict_block(
	unsigned mode,
	unsigned size,
	const struct webpc_vp8_edges *edges,
	uint8_t *dst,
	ptrdiff_t stride)
{
	UNUSED_PARAMETER(mode);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(edges);
	UNUSED_PARAMETER(dst);
	UNUSED_PARAMETER(stride);

	/* Not implemented yet. */
}

/*
 * Predicts one 4x4 luma subblock into dst (stride bytes a row) with mode
 * WEBPC_VP8_B_* (RFC 6386 12.3, its subblock code, and 20.14).
 */
void
webpc_vp8_predict_subblock(
	unsigned mode,
	const uint8_t *above,
	const uint8_t *left,
	uint8_t *dst,
	ptrdiff_t stride)
{
	UNUSED_PARAMETER(mode);
	UNUSED_PARAMETER(above);
	UNUSED_PARAMETER(left);
	UNUSED_PARAMETER(dst);
	UNUSED_PARAMETER(stride);

	/* Not implemented yet. */
}
