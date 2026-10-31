/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CAVLC entropy decoding of the software H.264 decoder: the residual
 * blocks (residual_block_cavlc of 7.3.5.3.2 with the parsing of 9.2:
 * coeff_token by Table 9-5, the levels of 9.2.2, total_zeros by Tables
 * 9-7, 9-8 and 9-9, run_before by Table 9-10, and their combination of
 * 9.2.4), the context nC (9.2.1), and the mapping of coded_block_pattern
 * (9.1.2, Table 9-4).
 *
 * The code tables are data of the specification and live in
 * h264sw-cavlc.c, each block naming its table.
 */

#ifndef LIBMEDIA_H264SW_CAVLC_H
#define LIBMEDIA_H264SW_CAVLC_H

#include "h264sw.h"
#include "vcodec-bitreader.h"

struct h264sw_slice_ctx;

/* nC for the chroma DC block of 4:2:0 (9.2.1: ChromaArrayType 1). */
#define H264SW_NC_CHROMA_DC (-1)

/*
 * Reads one residual block (residual_block_cavlc, 7.3.5.3.2, and 9.2).
 *
 * nc is nC (0 and above from h264sw_cavlc_nc, or H264SW_NC_CHROMA_DC);
 * start and end are startIdx and endIdx and max_coeff maxNumCoeff (16
 * for a 4x4 block and Intra16x16DCLevel, 15 for Intra16x16ACLevel and
 * the chroma AC blocks, 4 for the 4:2:0 chroma DC).  levels receives
 * coeffLevel[ 0..max_coeff - 1 ]: the levels in the block's scan order,
 * 0 where no level was sent.  *total_coeff receives TotalCoeff(
 * coeff_token ).
 *
 * coeff_token: the column of Table 9-5 nC selects (0 <= nC < 2, 2 <= nC
 * < 4, 4 <= nC < 8, 8 <= nC as the 6-bit fixed-length code, nC -1).  The
 * levels: trailing ones' signs, then level_prefix (9.2.2.1) and
 * level_suffix with suffixLength (starting at 1 when TotalCoeff > 10 and
 * TrailingOnes < 3), levelSuffixSize, levelCode and its adjustments
 * (9.2.2), the first non-trailing level's +2 when TrailingOnes < 3.
 * total_zeros when TotalCoeff < endIdx - startIdx + 1 (tzVlcIndex =
 * TotalCoeff; Table 9-7 or 9-8 for blocks of 15 or 16 coefficients, Table
 * 9-9 a for the chroma DC of 4:2:0), run_before by Table 9-10 while
 * zerosLeft > 0; then the levels are placed by 9.2.4.
 *
 * Errors (EINVAL, levels then unspecified): a code not in its table,
 * TotalCoeff above end - start + 1, total_zeros or a run that puts a
 * level before start, a level_prefix above 25, a level outside -2^15 ..
 * 2^15 - 1, or a read past the end of the RBSP.  A level_prefix of 16 to
 * 25 is accepted: A.2.1 to A.2.3 limit it to 15 only for the Baseline,
 * Main and Extended profiles, and no 8-bit level needs a prefix above
 * 25 (levelSuffixSize stays below 32 bits).
 */
int h264sw_cavlc_residual_block(struct vcodec_bitreader *bits, int nc, unsigned start, unsigned end, unsigned max_coeff, int32_t *levels, unsigned *total_coeff);

/*
 * Derives nC for a block of the current macroblock (9.2.1).
 *
 * block is 0..15 for a luma 4x4 block (luma4x4BlkIdx; 0 also for
 * Intra16x16DCLevel, which uses blkA and blkB of block 0), 16..19 for a
 * Cb AC block and 20..23 for a Cr AC block (chroma4x4BlkIdx + 16 or +
 * 20).  The neighbours A and B come from h264sw_mb_neighbour_4x4 or
 * h264sw_mb_neighbour_chroma; nN is the total_coeff entry of the
 * neighbouring block (struct h264sw_mb keeps 0 for skipped macroblocks
 * and for blocks with no coded residual, and 16 for I_PCM, as 9.2.1
 * says); the blocks of the current macroblock already read have their
 * entries set by the caller.  nC is ( nA + nB + 1 ) >> 1 when both are
 * available, the available one when only one is, 0 when neither is.
 * The rule of 9.2.1 for constrained_intra_pred_flag applies only with
 * data partitioning, which this decoder refuses.
 */
int h264sw_cavlc_nc(const struct h264sw_slice_ctx *ctx, unsigned block);

/*
 * Maps the codeNum of coded_block_pattern me(v) to its value (9.1.2, Table 9-4).
 *
 * code_num is the ue(v) read; intra is 1 for the Intra_4x4 and Intra_8x8
 * macroblock prediction modes, 0 for Inter (the columns for
 * ChromaArrayType 1 or 2).  Returns 0 and *cbp (CodedBlockPatternLuma in
 * bits 0..3, CodedBlockPatternChroma in bits 4..5), or EINVAL for a
 * code_num above 47.
 */
int h264sw_cavlc_cbp(uint32_t code_num, int intra, unsigned *cbp);

#endif
