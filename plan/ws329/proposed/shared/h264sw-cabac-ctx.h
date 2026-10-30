/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC context selection of the H.264 macroblock layer, shared by the
 * software decoder (h264sw-cabac-se.c, which decodes bins with these
 * contexts) and the software encoder (h264enc-cabac-se.c and
 * h264enc-cabac-res.c, which encode bins with the same contexts): the
 * initialization of the context variables (9.3.1.1), the context index
 * increments that depend on the neighbouring macroblocks and blocks
 * (9.3.3.1.1), and the increments of the residual (9.3.3.1.3, Tables
 * 9-40 and 9-43).
 *
 * The functions read the state that both sides keep alike: the slice
 * context (its header, its neighbours ctx->n, prev_mb_addr and
 * prev_qp_delta_nonzero) and the struct h264sw_mb entries.  They read
 * only blocks and macroblocks that precede the current element in
 * decoding order, so an encoder that has filled the current macroblock's
 * entry completely (with the values the decoder's parser will write)
 * gets the contexts the decoder will use.
 *
 * Every function is pure: it changes nothing.  The values returned are
 * ctxIdx (ctxIdxOffset + ctxIdxInc) unless the comment says ctxIdxInc.
 */

#ifndef LIBMEDIA_H264SW_CABAC_CTX_H
#define LIBMEDIA_H264SW_CABAC_CTX_H

#include "h264sw.h"
#include "h264sw-cabac.h"

struct h264sw_slice_ctx;

/*
 * Initializes the context variables of a slice (9.3.1.1).
 *
 * state receives H264SW_CABAC_CONTEXTS bytes: pStateIdx in bits 0..5 and
 * valMPS in bit 6.  slice_type is H264SW_SLICE_I, _P or _B; cabac_init_idc
 * 0..2 (ignored for I); qp SliceQPY 0..51.  Each ctxIdx that Table 9-11
 * lists for the slice type, except 276, is initialized from (m, n) of
 * h264sw_cabac_init_mn by 9-5 with Clip3( 0, 51, SliceQPY ); the others
 * are 0.  This is the context part of h264sw_cabac_start, which calls it.
 * Returns 0, or EINVAL (state 0 everywhere) for a slice type other than
 * I, P, B or a cabac_init_idc above 2.
 */
int h264sw_cabac_contexts_init(uint8_t *state, unsigned slice_type, unsigned cabac_init_idc, int qp);

/* ctxIdx of mb_skip_flag (ctxIdxOffset 11 for P, 24 for B; ctxIdxInc 9.3.3.1.1.1 from A and B). */
unsigned h264sw_ctx_mb_skip(const struct h264sw_slice_ctx *ctx);

/*
 * ctxIdxInc of the first bin of mb_type (9.3.3.1.1.3) for the slice type:
 * for I slices (prefix of P and B intra types use their own offsets,
 * Table 9-39), condTermFlagN is 0 when N is not available or is I_NxN
 * (kind I_4X4 or I_8X8) or SI; for B slices, 0 when N is not available
 * or is B_Skip or B_Direct_16x16.  Returns condTermFlagA +
 * condTermFlagB.  0 for P slices (the P prefix's first bin has a fixed
 * context, Table 9-39).
 */
unsigned h264sw_ctx_mb_type_inc(const struct h264sw_slice_ctx *ctx, unsigned slice_type);

/* ctxIdx of transform_size_8x8_flag (399 + the neighbours' flags, 9.3.3.1.1.10). */
unsigned h264sw_ctx_transform_8x8(const struct h264sw_slice_ctx *ctx);

/* ctxIdx of bin 0 of intra_chroma_pred_mode (64 + 9.3.3.1.1.8); bins 1 and 2 use 64 + 3. */
unsigned h264sw_ctx_chroma_mode(const struct h264sw_slice_ctx *ctx);

/*
 * ctxIdx of bin 0 of ref_idx_lX for the partition whose upper-left luma
 * location is (x, y) in the current macroblock (54 + 9.3.3.1.1.6); bins
 * 1 and later use 54 + 4 and 54 + 5 (Table 9-39).
 */
unsigned h264sw_ctx_ref_idx(const struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y);

/*
 * ctxIdx of bin 0 of the prefix of mvd_lX component comp (0 horizontal:
 * offset 40, 1 vertical: 47) for the (sub-)partition at (x, y), from
 * absMvdComp of A plus B (9.3.3.1.1.7: below 3 gives 0, 3..32 gives 1,
 * above 32 gives 2).  Bins 1..8 of the prefix use offset + 3, + 4, + 5,
 * + 6, + 6, + 6, + 6, + 6 (Table 9-39).
 */
unsigned h264sw_ctx_mvd(const struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y, unsigned comp);

/*
 * ctxIdx of prefix bin b8 (luma8x8BlkIdx 0..3) of coded_block_pattern (73
 * + 9.3.3.1.1.4).  luma_bits holds the bins of this macroblock already
 * coded (bit b set for a coded 8x8 block b < b8), the neighbours' from
 * their cbp.
 */
unsigned h264sw_ctx_cbp_luma(const struct h264sw_slice_ctx *ctx, unsigned b8, unsigned luma_bits);

/* ctxIdx of suffix bin bin_idx (0 or 1) of coded_block_pattern (77 + 9.3.3.1.1.4 for chroma, the bin's own term). */
unsigned h264sw_ctx_cbp_chroma(const struct h264sw_slice_ctx *ctx, unsigned bin_idx);

/* ctxIdx of bin 0 of mb_qp_delta (60 + 9.3.3.1.1.5, from prev_mb_addr and prev_qp_delta_nonzero); bin 1 uses 60 + 2, later bins 60 + 3. */
unsigned h264sw_ctx_qp_delta(const struct h264sw_slice_ctx *ctx);

/*
 * ctxIdx of coded_block_flag of a block (Table 9-34 offsets 85 for
 * ctxBlockCat below 5, plus ctxIdxBlockCatOffset of Table 9-40, plus
 * 9.3.3.1.1.9).  cat, blk and component as h264sw_cabac_residual_block
 * of h264sw-cabac-se.h.  Not defined for cat 5 (no flag for 4:2:0).
 */
unsigned h264sw_ctx_coded_block_flag(const struct h264sw_slice_ctx *ctx, unsigned cat, unsigned blk, unsigned component);

/*
 * ctxIdx of significant_coeff_flag (last 0) or last_significant_coeff_flag
 * (last 1) at scan position idx of a block of category cat, frame coded
 * (Table 9-34 offsets 105, 166 for cats 0..4 and 402, 417 for cat 5;
 * Table 9-40; 9.3.3.1.3: ctxIdxInc idx for cats other than 3 and 5,
 * Min( idx / NumC8x8, 2 ) for cat 3 with NumC8x8 1, Table 9-43 for cat 5).
 */
unsigned h264sw_ctx_significance(unsigned cat, unsigned idx, int last);

/*
 * ctxIdx of bin bin_idx of the prefix of coeff_abs_level_minus1 of a
 * block of category cat (offset 227 for cats 0..4, 426 for cat 5; Table
 * 9-40; 9.3.3.1.3 with numDecodAbsLevelEq1 eq1 and numDecodAbsLevelGt1
 * gt1 counted over the levels of the block already coded, in reverse
 * scan order).
 */
unsigned h264sw_ctx_abs_level(unsigned cat, unsigned bin_idx, unsigned eq1, unsigned gt1);

#endif
