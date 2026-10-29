/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC syntax elements of the software H.264 decoder (9.3.2, 9.3.3.1).
 */

#include "h264sw-cabac-se.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>

/*
 * Decodes mb_skip_flag (ctxIdxOffset 11 for P, 24 for B; ctxIdxInc by 9.3.3.1.1.1: condTermFlagN 0 when mbAddrN is not available or its mb_skip_flag is 1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
unsigned
h264sw_cabac_mb_skip(
	struct h264sw_slice_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Decodes mb_type (9.3.2.5, Tables 9-36 and 9-37; ctxIdxOffset 3 for I slices, 14 and 17 for P prefix and suffix, 27 and 32 for B; the first bin's ctxIdxInc by 9.3.3.1.1.3, the others by Table 9-39 and 9.3.3.1.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
int
h264sw_cabac_mb_type(
	struct h264sw_slice_ctx *ctx,
	unsigned slice_type,
	unsigned *mb_type)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(slice_type);
	UNUSED_PARAMETER(mb_type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Decodes sub_mb_type (Table 9-38; ctxIdxOffset 21 for P, 36 for B, ctxIdxInc by Table 9-39 and 9.3.3.1.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
int
h264sw_cabac_sub_mb_type(
	struct h264sw_slice_ctx *ctx,
	unsigned slice_type,
	unsigned *sub_mb_type)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(slice_type);
	UNUSED_PARAMETER(sub_mb_type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Decodes transform_size_8x8_flag (ctxIdxOffset 399, ctxIdxInc by 9.3.3.1.1.10 from the neighbours' flags).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
unsigned
h264sw_cabac_transform_8x8(
	struct h264sw_slice_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Decodes prev_intra4x4_pred_mode_flag or prev_intra8x8_pred_mode_flag (ctxIdx 68) and, when it is 0, rem_intra4x4_pred_mode or rem_intra8x8_pred_mode (three bins with ctxIdx 69, FL, least significant bin first).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
void
h264sw_cabac_intra_mode(
	struct h264sw_slice_ctx *ctx,
	unsigned *flag,
	unsigned *rem)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(flag);
	UNUSED_PARAMETER(rem);

	/* Does nothing yet. */
	return;
}

/*
 * Decodes intra_chroma_pred_mode (TU cMax 3, ctxIdxOffset 64, ctxIdxInc by 9.3.3.1.1.8 and Table 9-39).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
unsigned
h264sw_cabac_chroma_mode(
	struct h264sw_slice_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Decodes ref_idx_l0 or ref_idx_l1 (U binarization, ctxIdxOffset 54; the first bin's ctxIdxInc by 9.3.3.1.1.6, the others by Table 9-39).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
int
h264sw_cabac_ref_idx(
	struct h264sw_slice_ctx *ctx,
	unsigned list,
	unsigned x,
	unsigned y,
	unsigned *ref_idx)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(list);
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
	UNUSED_PARAMETER(ref_idx);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Decodes one component of mvd_l0 or mvd_l1 (UEG3 with signedValFlag 1 and uCoff 9, ctxIdxOffset 40 for the horizontal and 47 for the vertical component; the prefix's first bin's ctxIdxInc by 9.3.3.1.1.7 from absMvdCompA + absMvdCompB, the others by Table 9-39; suffix and sign bypass).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
int
h264sw_cabac_mvd(
	struct h264sw_slice_ctx *ctx,
	unsigned list,
	unsigned x,
	unsigned y,
	unsigned component,
	int *mvd)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(list);
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
	UNUSED_PARAMETER(component);
	UNUSED_PARAMETER(mvd);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Decodes coded_block_pattern (9.3.2.6: prefix of four FL bins, one per 8x8 luma block, ctxIdxOffset 73 with ctxIdxInc by 9.3.3.1.1.4 from the left and upper 8x8 blocks, which may be bins already decoded of this macroblock; suffix TU cMax 2 with ctxIdxOffset 77).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
unsigned
h264sw_cabac_cbp(
	struct h264sw_slice_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Decodes mb_qp_delta (9.3.2.7: the mapping of Table 9-3 over U bins, ctxIdxOffset 60; the first bin's ctxIdxInc by 9.3.3.1.1.5 from the previous macroblock in decoding order, ctx->prev_mb_addr and ctx->prev_qp_delta_nonzero).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
int
h264sw_cabac_qp_delta(
	struct h264sw_slice_ctx *ctx,
	int *qp_delta)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(qp_delta);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Decodes end_of_slice_flag (ctxIdx 276, DecodeTerminate).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
unsigned
h264sw_cabac_end_of_slice(
	struct h264sw_slice_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Decodes one residual block (residual_block_cabac, 7.3.5.3.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac-se.h.
 */
int
h264sw_cabac_residual_block(
	struct h264sw_slice_ctx *ctx,
	unsigned cat,
	unsigned blk,
	unsigned component,
	unsigned start,
	unsigned end,
	unsigned max_coeff,
	int32_t *levels,
	unsigned *coded)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(cat);
	UNUSED_PARAMETER(blk);
	UNUSED_PARAMETER(component);
	UNUSED_PARAMETER(start);
	UNUSED_PARAMETER(end);
	UNUSED_PARAMETER(max_coeff);
	UNUSED_PARAMETER(levels);
	UNUSED_PARAMETER(coded);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
