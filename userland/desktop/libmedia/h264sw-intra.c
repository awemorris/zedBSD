/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The intra prediction of the software H.264 decoder (8.3).
 */

#include "h264sw-intra.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>

/*
 * Derives Intra4x4PredMode of a block (8.3.1.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-intra.h.
 */
unsigned
h264sw_intra4x4_mode(
	const struct h264sw_slice_ctx *ctx,
	unsigned blk,
	unsigned prev_flag,
	unsigned rem)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(blk);
	UNUSED_PARAMETER(prev_flag);
	UNUSED_PARAMETER(rem);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Derives Intra8x8PredMode of a block (8.3.2.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-intra.h.
 */
unsigned
h264sw_intra8x8_mode(
	const struct h264sw_slice_ctx *ctx,
	unsigned blk,
	unsigned prev_flag,
	unsigned rem)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(blk);
	UNUSED_PARAMETER(prev_flag);
	UNUSED_PARAMETER(rem);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Gathers the neighbouring luma samples of a block of the current macroblock.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-intra.h.
 */
void
h264sw_intra_edge_luma(
	const struct h264sw_slice_ctx *ctx,
	unsigned size,
	unsigned blk,
	struct h264sw_intra_edge *edge)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(blk);
	UNUSED_PARAMETER(edge);

	/* Does nothing yet. */
	return;
}

/*
 * Gathers the neighbouring samples of the current macroblock's 8x8 block of chroma component plane (1 Cb, 2 Cr), for 8.3.4, with the same rules.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-intra.h.
 */
void
h264sw_intra_edge_chroma(
	const struct h264sw_slice_ctx *ctx,
	unsigned plane,
	struct h264sw_intra_edge *edge)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(plane);
	UNUSED_PARAMETER(edge);

	/* Does nothing yet. */
	return;
}

/*
 * Predicts a 4x4 luma block (8.3.1.2.1 to 8.3.1.2.9).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-intra.h.
 */
int
h264sw_intra_pred4x4(
	unsigned mode,
	const struct h264sw_intra_edge *edge,
	uint8_t *pred)
{
	UNUSED_PARAMETER(mode);
	UNUSED_PARAMETER(edge);
	UNUSED_PARAMETER(pred);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Predicts an 8x8 luma block (8.3.2.2): first the reference samples are filtered (8.3.2.2.1, which also substitutes the unavailable top-right samples), then the mode's equations (8.3.2.2.2 to 8.3.2.2.10).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-intra.h.
 */
int
h264sw_intra_pred8x8(
	unsigned mode,
	const struct h264sw_intra_edge *edge,
	uint8_t *pred)
{
	UNUSED_PARAMETER(mode);
	UNUSED_PARAMETER(edge);
	UNUSED_PARAMETER(pred);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Predicts a 16x16 luma block (8.3.3.1 to 8.3.3.4); pred receives 256 samples.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-intra.h.
 */
int
h264sw_intra_pred16x16(
	unsigned mode,
	const struct h264sw_intra_edge *edge,
	uint8_t *pred)
{
	UNUSED_PARAMETER(mode);
	UNUSED_PARAMETER(edge);
	UNUSED_PARAMETER(pred);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Predicts an 8x8 chroma block of 4:2:0 (8.3.4.1 to 8.3.4.4).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-intra.h.
 */
int
h264sw_intra_pred_chroma(
	unsigned mode,
	const struct h264sw_intra_edge *edge,
	uint8_t *pred)
{
	UNUSED_PARAMETER(mode);
	UNUSED_PARAMETER(edge);
	UNUSED_PARAMETER(pred);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
