/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice data and the macroblock layer of the software H.264 decoder:
 * the loop over a slice's macroblocks (slice_data, 7.3.4, with
 * mb_skip_run for CAVLC and mb_skip_flag and end_of_slice_flag for
 * CABAC), and the syntax of one macroblock (macroblock_layer 7.3.5,
 * mb_pred 7.3.5.1, sub_mb_pred 7.3.5.2, residual 7.3.5.3 with
 * residual_luma 7.3.5.3.1) read with CAVLC (h264sw-cavlc.h and the
 * descriptors of h264sw-bits.h) or CABAC (h264sw-cabac-se.h).
 *
 * The parsing writes the macroblock's syntax into ctx->syntax and the
 * fields of the current struct h264sw_mb entry that the contexts of the
 * following elements and macroblocks read: slice, kind, cbp,
 * transform_8x8, qp, chroma_pred_mode, direct, total_coeff, coded_flags,
 * mvd, motion.intra, and motion.ref_idx of the partitions whose ref_idx
 * is sent or inferred (-1 for an unused list; the direct quadrants are
 * filled later by the reconstruction).  The reconstruction
 * (h264sw-recon.h) writes the rest.
 */

#ifndef LIBMEDIA_H264SW_MACROBLOCK_H
#define LIBMEDIA_H264SW_MACROBLOCK_H

#include "h264sw.h"

struct h264sw_slice_ctx;

/*
 * Decodes the macroblocks of a slice (7.3.4).
 *
 * ctx is prepared by the decoder: picture, header, slice_num, lists,
 * weights, direct, bits at the start of slice_data(), cabac_mode, and
 * for CABAC the engine started (h264sw_cabac_start); qp is SliceQPY.
 * Starting at first_mb_in_slice, for each macroblock: sets mb_addr;
 * refuses (EINVAL) an address whose entry already belongs to a slice
 * (overlapping or repeated slices); resets the entry with
 * h264sw_mb_reset( entry, slice_num ); clears the per-macroblock state
 * of the slice context (decoded_4x4 0, direct.spatial_ready 0); finds
 * its neighbours (h264sw_mb_neighbours); reads its skip indication;
 * parses it (h264sw_macroblock_parse) and reconstructs it
 * (h264sw_recon_mb); records prev_mb_addr; then moves to the next
 * address (CurrMbAddr + 1: there are no slice groups).  The loop ends
 * when CAVLC has no more RBSP data (more_rbsp_data(), after any
 * mb_skip_run) or CABAC decodes end_of_slice_flag 1.
 * picture->decoded_mbs counts each macroblock decoded (once: an address
 * is never decoded twice).
 *
 * Errors: a parse or reconstruction error ends the slice at that
 * macroblock and is returned (EINVAL, ENOTSUP); the macroblocks before
 * it stay decoded, the failing one's entry is reset with slice -1
 * (h264sw_mb_reset( entry, -1 )) so the decoder conceals it with the
 * rest.  A slice that runs past the last macroblock of the picture
 * returns EINVAL.  An mb_skip_run larger than the macroblocks left
 * returns EINVAL.
 */
int h264sw_slice_data(struct h264sw_slice_ctx *ctx);

/*
 * Parses one macroblock (7.3.5 and below) into ctx->syntax.
 *
 * skipped is 1 for a macroblock that mb_skip_run or mb_skip_flag says is
 * skipped: then only the inferred values are set (type P_Skip or
 * B_Skip, cbp 0, qp_delta 0, the entry's qp the predictor's).
 * Otherwise: mb_type (and for I_PCM the alignment and the samples, via
 * h264sw_cabac_pcm for CABAC or the reader for CAVLC), the
 * transform_size_8x8_flag where 7.3.5 reads it (for I_NxN after mb_type;
 * for the inter kinds after coded_block_pattern when the conditions of
 * 7.3.5 hold, including direct_8x8_inference_flag for B_Direct_16x16 and
 * B_8x8 with direct sub-macroblocks), mb_pred or sub_mb_pred, the
 * coded_block_pattern (except Intra_16x16), mb_qp_delta when there is
 * residual or the type is Intra_16x16, and the residual blocks.
 *
 * Value checks (EINVAL): mb_type and sub_mb_type in range; ref_idx not
 * above num_ref_idx_active - 1; intra_chroma_pred_mode at most 3;
 * mb_qp_delta in -26..25; each mvd component in -32768..32767 (7.4.5.1:
 * -8192 to 8191.75 luma samples; CAVLC's se(v) can say more, so it is
 * checked before it is stored in int16_t); rem_intra pred modes at most
 * 7; coded_block_pattern's codeNum at most 47; transform_size_8x8_flag
 * only where allowed.
 * QPY is updated by 7-37: ( QPY,PREV + mb_qp_delta + 52 ) % 52 for
 * 8 bits, and stored in the entry and in ctx->qp.  For I_PCM the
 * entry's qp is 0 (what 8.7.2.2 uses for qPp of an I_PCM macroblock) and
 * ctx->qp is left as it was (mb_qp_delta is absent, so QPY carries over).
 *
 * nC (CAVLC) and the CABAC contexts read the entries of the current
 * macroblock's blocks already parsed, so the total_coeff and
 * coded_flags of each block are written as soon as it is read.
 */
int h264sw_macroblock_parse(struct h264sw_slice_ctx *ctx, int skipped);

#endif
