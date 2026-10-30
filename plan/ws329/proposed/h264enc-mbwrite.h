/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The writing of slice_data() (7.3.4) and macroblock_layer() (7.3.5 with
 * mb_pred 7.3.5.1, sub_mb_pred 7.3.5.2 and residual 7.3.5.3) in the
 * software H.264 encoder, with CAVLC (h264enc-cavlc.h and the
 * descriptors of vcodec-bitwriter.h) or CABAC (h264enc-cabac-se.h,
 * h264enc-cabac-res.h).
 *
 * The writer reads the macroblock's syntax (ctx->syntax) and its entry,
 * both as h264enc_mb_code left them, and writes exactly what
 * h264sw_macroblock_parse reads back into the same syntax and entry.
 * The order and the presence conditions of every element are those of
 * 7.3.5 for frames without MBAFF.
 */

#ifndef LIBMEDIA_H264ENC_MBWRITE_H
#define LIBMEDIA_H264ENC_MBWRITE_H

#include "h264enc-cabac-res.h"
#include "h264enc-cabac-se.h"
#include "h264enc-cavlc.h"
#include "h264sw.h"
#include "vcodec-bitwriter.h"

struct h264sw_slice_ctx;

/*
 * One slice's writing state.
 *
 * writer: the slice NAL unit's RBSP, from the start of slice_data()
 * (the header is already in it).  cabac_mode: entropy_coding_mode_flag;
 * cabac: the CABAC state (started by h264enc_cabac_start); cavlc: the
 * code index.  skip_run: the skipped macroblocks not yet sent (CAVLC P
 * and B slices).  max_prefix: the largest CAVLC level_prefix (15 or 25).
 */
struct h264enc_slice_writer {
	struct vcodec_bitwriter writer;
	int cabac_mode;
	struct h264enc_cabac cabac;
	const struct h264enc_cavlc *cavlc;
	uint32_t skip_run;
	unsigned max_prefix;
};

/*
 * Writes the current macroblock of ctx (ctx->mb_addr) into the slice.
 *
 * skipped is 1 for P_Skip and B_Skip.  CAVLC: a skipped macroblock adds
 * to skip_run; another first writes mb_skip_run ue( skip_run ) (P and B
 * slices only) and sets it to 0, then macroblock_layer().  CABAC: in P and
 * B slices mb_skip_flag, then macroblock_layer() when not skipped, then
 * end_of_slice_flag( last ).  last is 1 for the slice's last macroblock:
 * CAVLC then writes a pending mb_skip_run (when above 0); neither writes
 * the trailing bits (h264enc_mbwrite_finish does).
 *
 * macroblock_layer(): mb_type (ue, or CABAC); I_PCM: the alignment and
 * the samples (CAVLC: pcm_alignment_zero_bit and 384 bytes; CABAC:
 * h264enc_cabac_pcm); transform_size_8x8_flag for I_NxN when the PPS's
 * transform_8x8_mode_flag is 1; mb_pred or sub_mb_pred (intra modes and
 * chroma mode; ref_idx te(v) with range num_ref_idx_active - 1 when that
 * is above 0, or CABAC; mvd se(v), or CABAC); coded_block_pattern me(v)
 * (h264enc_cavlc_cbp) or CABAC except for Intra_16x16;
 * transform_size_8x8_flag for inter kinds where 7.3.5 reads it; mb_qp_delta
 * when cbp is above 0 or the kind is Intra_16x16; then residual() for
 * startIdx 0 and endIdx 15: Intra16x16DCLevel, the luma blocks (8x8
 * CAVLC as four interleaved 4x4 lists: list k gets level 4 * i + k of the
 * 8x8 block, 7.3.5.3.2), the chroma DC of Cb and Cr when
 * CodedBlockPatternChroma is above 0, the chroma AC when it is 2.  CAVLC
 * nC comes from h264sw_cavlc_nc (the entry's total_coeff already holds
 * the current macroblock's counts).
 *
 * Afterwards ctx->prev_mb_addr becomes ctx->mb_addr and
 * ctx->prev_qp_delta_nonzero whether a non-zero mb_qp_delta was sent (0
 * for skipped and I_PCM macroblocks and when it was absent), as the
 * decoder leaves them.
 *
 * Returns 0; ERANGE when a CAVLC level needs a level_prefix above
 * max_prefix (the caller clamps and codes the macroblock again); EINVAL
 * for a syntax the slice cannot carry; or the writer's error.  On an
 * error the slice must be rewound to a mark taken before the macroblock
 * (h264enc_mbwrite_mark, h264enc_mbwrite_rewind).
 */
int h264enc_mbwrite(struct h264enc_slice_writer *slice, struct h264sw_slice_ctx *ctx, int skipped, int last);

/*
 * A saved writing position: the writer's mark, and for CABAC the engine
 * and the context variables (a macroblock may be written again, as I_PCM,
 * when it passed the bit limit).  skip_run and the slice context's
 * prev_mb_addr and prev_qp_delta_nonzero are saved too.
 */
struct h264enc_mbwrite_mark {
	struct vcodec_bitmark bits;
	struct vcodec_cabac_enc engine;
	uint8_t context[H264SW_CABAC_CONTEXTS];
	uint32_t skip_run;
	int32_t prev_mb_addr;
	int prev_qp_delta_nonzero;
};

/* Saves the position before a macroblock. */
void h264enc_mbwrite_mark(const struct h264enc_slice_writer *slice, const struct h264sw_slice_ctx *ctx, struct h264enc_mbwrite_mark *mark);

/* Goes back to a saved position (what was written after it is dropped). */
void h264enc_mbwrite_rewind(struct h264enc_slice_writer *slice, struct h264sw_slice_ctx *ctx, const struct h264enc_mbwrite_mark *mark);

/*
 * The bits of the macroblock written since mark, as A.3.1 j) counts
 * them: the writer's bits for CAVLC (less the mb_skip_run before the
 * macroblock), vcodec_cabac_enc_bits' growth for CABAC (an upper bound).
 */
uint64_t h264enc_mbwrite_bits(const struct h264enc_slice_writer *slice, const struct h264enc_mbwrite_mark *mark);

/*
 * Ends the slice data: for CAVLC rbsp_slice_trailing_bits (the trailing
 * bits); for CABAC the alignment zero bits after the flush of the last
 * end_of_slice_flag (the stop bit was the flush's last bit).  No
 * cabac_zero_word here (they follow the escaped NAL unit,
 * vcodec_nal_append).  Returns the writer's error.
 */
int h264enc_mbwrite_finish(struct h264enc_slice_writer *slice);

#endif
