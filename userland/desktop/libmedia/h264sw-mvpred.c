/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The motion vector prediction of the software H.264 decoder (8.4.1.1,
 * 8.4.1.3).
 */

#include "h264sw-mvpred.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>

/*
 * Predicts the motion vector of a partition (8.4.1.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mvpred.h.
 */
void
h264sw_mvpred(
	const struct h264sw_slice_ctx *ctx,
	unsigned list,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height,
	int ref_idx,
	int16_t *mvp)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(list);
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(ref_idx);
	UNUSED_PARAMETER(mvp);

	/* Does nothing yet. */
	return;
}

/*
 * Derives the motion vector of a P_Skip macroblock (8.4.1.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mvpred.h.
 */
void
h264sw_mvpred_pskip(
	const struct h264sw_slice_ctx *ctx,
	int16_t *mv)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(mv);

	/* Does nothing yet. */
	return;
}
