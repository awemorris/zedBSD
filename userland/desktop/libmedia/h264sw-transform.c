/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transform coefficient decoding of the software H.264 decoder (8.5).
 */

#include "h264sw-transform.h"
#include "h264sw-tables.h"

#include <errno.h>
#include <stddef.h>

/*
 * Computes the scaling functions from a picture's scaling lists (8.5.9).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-transform.h.
 */
void
h264sw_levelscale_init(
	struct h264sw_levelscale *levelscale,
	const struct h264sw_scaling *scaling)
{
	UNUSED_PARAMETER(levelscale);
	UNUSED_PARAMETER(scaling);

	/* Does nothing yet. */
	return;
}

/*
 * Places a 4x4 block's levels from scan order to raster order (8.5.6, zig-zag).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-transform.h.
 */
void
h264sw_unscan4x4(
	const int32_t *levels,
	int32_t *block)
{
	UNUSED_PARAMETER(levels);
	UNUSED_PARAMETER(block);

	/* Does nothing yet. */
	return;
}

/*
 * Places an 8x8 block's 64 levels from scan order to raster order (8.5.7, 8x8 zig-zag).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-transform.h.
 */
void
h264sw_unscan8x8(
	const int32_t *levels,
	int32_t *block)
{
	UNUSED_PARAMETER(levels);
	UNUSED_PARAMETER(block);

	/* Does nothing yet. */
	return;
}

/*
 * Scales a 4x4 block of levels (8.5.12.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-transform.h.
 */
void
h264sw_scale4x4(
	int32_t *block,
	const int32_t *scale,
	int qp,
	int dc_done)
{
	UNUSED_PARAMETER(block);
	UNUSED_PARAMETER(scale);
	UNUSED_PARAMETER(qp);
	UNUSED_PARAMETER(dc_done);

	/* Does nothing yet. */
	return;
}

/*
 * Transforms a scaled 4x4 block into residual samples (8.5.12.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-transform.h.
 */
void
h264sw_idct4x4(
	int32_t *block)
{
	UNUSED_PARAMETER(block);

	/* Does nothing yet. */
	return;
}

/*
 * Scales an 8x8 block of levels (8.5.13.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-transform.h.
 */
void
h264sw_scale8x8(
	int32_t *block,
	const int32_t *scale,
	int qp)
{
	UNUSED_PARAMETER(block);
	UNUSED_PARAMETER(scale);
	UNUSED_PARAMETER(qp);

	/* Does nothing yet. */
	return;
}

/*
 * Transforms a scaled 8x8 block into residual samples (8.5.13.2: rows, then columns, then rij = ( mij + 32 ) >> 6).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-transform.h.
 */
void
h264sw_idct8x8(
	int32_t *block)
{
	UNUSED_PARAMETER(block);

	/* Does nothing yet. */
	return;
}

/*
 * Transforms and scales the 16 DC levels of an Intra_16x16 macroblock (8.5.10).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-transform.h.
 */
void
h264sw_luma_dc(
	int32_t *dc,
	int32_t scale00,
	int qp)
{
	UNUSED_PARAMETER(dc);
	UNUSED_PARAMETER(scale00);
	UNUSED_PARAMETER(qp);

	/* Does nothing yet. */
	return;
}

/*
 * Transforms and scales the four DC levels of a 4:2:0 chroma component (8.5.11).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-transform.h.
 */
void
h264sw_chroma_dc(
	int32_t *dc,
	int32_t scale00,
	int qp)
{
	UNUSED_PARAMETER(dc);
	UNUSED_PARAMETER(scale00);
	UNUSED_PARAMETER(qp);

	/* Does nothing yet. */
	return;
}

/*
 * Adds a residual block to its prediction and clips (8.5.14).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-transform.h.
 */
void
h264sw_add_residual(
	const uint8_t *pred,
	size_t pred_stride,
	const int32_t *residual,
	unsigned size,
	uint8_t *out,
	size_t out_stride)
{
	UNUSED_PARAMETER(pred);
	UNUSED_PARAMETER(pred_stride);
	UNUSED_PARAMETER(residual);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(out_stride);

	/* Does nothing yet. */
	return;
}
