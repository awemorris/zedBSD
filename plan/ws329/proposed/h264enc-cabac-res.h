/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC writing of residual blocks in the software H.264 encoder
 * (residual_block_cabac, 7.3.5.3.3): coded_block_flag, the significance
 * map (significant_coeff_flag and last_significant_coeff_flag), and the
 * levels (coeff_abs_level_minus1 as UEG0 with uCoff 14, and
 * coeff_sign_flag), with the contexts of h264sw-cabac-ctx.h; and an
 * estimate of the bits a block would take, from the context states,
 * without coding it.
 */

#ifndef LIBMEDIA_H264ENC_CABAC_RES_H
#define LIBMEDIA_H264ENC_CABAC_RES_H

#include "h264enc-cabac-se.h"
#include "h264sw.h"

struct h264sw_slice_ctx;

/*
 * Codes one residual block.
 *
 * cat (H264SW_CAT_*), blk, component, start, end and max_coeff are as
 * h264sw_cabac_residual_block of h264sw-cabac-se.h; levels holds
 * coeffLevel[ 0..max_coeff - 1 ] in scan order (0 outside start..end).
 * coded_block_flag is 1 exactly when a level is non-zero; it is coded
 * for every cat but 5 with h264sw_ctx_coded_block_flag (the current
 * entry's coded_flags must already hold this block's flag and those of
 * the blocks before it, as h264enc-mb.h fills them), and for cat 5 the
 * block must have a non-zero level (EINVAL otherwise: an 8x8 block
 * without levels has its cbp bit clear and is not coded).  Then, for
 * numCoeff = end + 1 positions from start: significant_coeff_flag of each
 * position before the last non-zero one, last_significant_coeff_flag
 * after each significant one (not coded at position numCoeff - 1, where
 * it is inferred), with h264sw_ctx_significance; then in reverse scan
 * order each level: the prefix of |level| - 1 (TU of at most 14 bins,
 * bin 0 and the others with h264sw_ctx_abs_level from the counts of
 * levels coded so far equal to 1 and above 1), for |level| - 1 >= 14 the
 * zeroth order Exp-Golomb suffix of |level| - 15 in bypass bins, and the
 * sign in a bypass bin.
 *
 * Returns 0, or EINVAL for bad arguments or a level outside
 * -2^15..2^15 - 1.
 */
int h264enc_cabac_residual_block(struct h264enc_cabac *cabac, const struct h264sw_slice_ctx *ctx, unsigned cat, unsigned blk, unsigned component, const int32_t *levels, unsigned start, unsigned end, unsigned max_coeff);

/*
 * Estimates the bits of the same block in 1/256 bit from the current
 * context states (vcodec_cabac_cost for each context-coded bin, 256 for
 * each bypass bin), as if each bin were coded with the state the context
 * has now (the state changes within the block are ignored).  Changes
 * nothing.  0 for bad arguments.
 */
uint32_t h264enc_cabac_residual_cost(const struct h264enc_cabac *cabac, const struct h264sw_slice_ctx *ctx, unsigned cat, unsigned blk, unsigned component, const int32_t *levels, unsigned start, unsigned end, unsigned max_coeff);

#endif
