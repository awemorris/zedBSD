/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC writing of the macroblock-level syntax elements in the
 * software H.264 encoder: for each element its binarization (9.3.2) and
 * the context of each bin (Table 9-34 offsets, Table 9-39 for the bins
 * after the first, the neighbour-dependent increments of 9.3.3.1.1 from
 * h264sw-cabac-ctx.h), coded with the engine of vcodec-cabac-enc.h.  The
 * residual blocks are in h264enc-cabac-res.h.
 *
 * Every function codes one element of the current macroblock
 * (ctx->n.current).  The slice context and the struct h264sw_mb entries
 * must hold what the decoder will hold when it decodes the same element:
 * the encoder fills the current entry completely before it writes the
 * macroblock (h264enc-mb.h), and every context derivation reads only
 * blocks and elements that precede the current one in decoding order,
 * so the contexts are the decoder's.
 *
 * The bin strings of mb_type and sub_mb_type (Tables 9-36, 9-37, 9-38)
 * are the encoder's own tables (data of the specification, each block
 * with its "Values referenced:" comment); the decoder decodes them by its
 * own code, and the unit tests decode every value written.
 */

#ifndef LIBMEDIA_H264ENC_CABAC_SE_H
#define LIBMEDIA_H264ENC_CABAC_SE_H

#include "h264sw.h"
#include "h264sw-cabac.h"
#include "vcodec-bitwriter.h"
#include "vcodec-cabac-enc.h"

struct h264sw_slice_ctx;

/*
 * One slice's CABAC writing state: the engine and the context variables
 * (ctxIdx 0..459, one byte each as the decoder keeps them).  Owned by
 * the encoder's slice state.
 */
struct h264enc_cabac {
	struct vcodec_cabac_enc engine;
	uint8_t context[H264SW_CABAC_CONTEXTS];
};

/*
 * Starts CABAC for a slice (9.3.1): writes cabac_alignment_one_bit up to
 * the byte boundary, initializes the contexts with
 * h264sw_cabac_contexts_init( slice_type, cabac_init_idc, qp ) and starts
 * the engine on writer.  engine.bins is left as it is (the caller sets
 * it to 0 for the first slice of a picture).  Returns 0, EINVAL for bad
 * arguments, or the writer's error.
 */
int h264enc_cabac_start(struct h264enc_cabac *cabac, struct vcodec_bitwriter *writer, unsigned slice_type, unsigned cabac_init_idc, int qp);

/* Codes mb_skip_flag (P and B slices; context h264sw_ctx_mb_skip). */
void h264enc_cabac_mb_skip(struct h264enc_cabac *cabac, const struct h264sw_slice_ctx *ctx, unsigned skipped);

/*
 * Codes mb_type (9.3.2.5): for I slices the bin string of Table 9-36
 * (ctxIdxOffset 3; bin 0 with h264sw_ctx_mb_type_inc, bin 1 of the
 * I_NxN-or-not test with DecodeTerminate for I_PCM, the others by Table
 * 9-39); for P slices the prefix of Table 9-37 (offset 14) and, for an
 * intra type 5 + n, the prefix "1" and the I bin string of n as suffix
 * with offset 17; for B slices Table 9-37 (offset 27, bin 0 with
 * h264sw_ctx_mb_type_inc) and for 23 + n the B prefix "111101" and the
 * suffix with offset 32.  mb_type is numbered as the tables of 7.4.5.
 * The I_PCM bin is coded with vcodec_cabac_enc_terminate( 1 ), which
 * flushes: the caller goes on with h264enc_cabac_pcm.  Returns 0 or
 * EINVAL for a value outside its slice type's range.
 */
int h264enc_cabac_mb_type(struct h264enc_cabac *cabac, const struct h264sw_slice_ctx *ctx, unsigned slice_type, unsigned mb_type);

/* Codes sub_mb_type (Table 9-38; offset 21 for P, 36 for B; bins by Table 9-39).  Returns 0 or EINVAL. */
int h264enc_cabac_sub_mb_type(struct h264enc_cabac *cabac, unsigned slice_type, unsigned sub_mb_type);

/* Codes transform_size_8x8_flag (context h264sw_ctx_transform_8x8). */
void h264enc_cabac_transform_8x8(struct h264enc_cabac *cabac, const struct h264sw_slice_ctx *ctx, unsigned flag);

/* Codes prev_intra4x4/8x8_pred_mode_flag (ctxIdx 68) and, when flag is 0, rem (three bins ctxIdx 69, least significant first). */
void h264enc_cabac_intra_mode(struct h264enc_cabac *cabac, unsigned flag, unsigned rem);

/* Codes intra_chroma_pred_mode 0..3 (TU cMax 3; bin 0 context h264sw_ctx_chroma_mode, bins 1 and 2 ctxIdx 67). */
void h264enc_cabac_chroma_mode(struct h264enc_cabac *cabac, const struct h264sw_slice_ctx *ctx, unsigned mode);

/*
 * Codes ref_idx_lX of the partition at (x, y) (U binarization; bin 0
 * context h264sw_ctx_ref_idx, bin 1 ctxIdx 58, later bins 59).  ref_idx
 * must be below the slice's num_ref_idx_active of the list (EINVAL
 * otherwise; nothing coded).
 */
int h264enc_cabac_ref_idx(struct h264enc_cabac *cabac, const struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y, unsigned ref_idx);

/*
 * Codes one component of mvd_lX of the (sub-)partition at (x, y) (9.3.2.3
 * UEG3 with signedValFlag 1 and uCoff 9: a TU prefix of Min( |mvd|, 9 )
 * with contexts from h264sw_ctx_mvd and Table 9-39, then for |mvd| >= 9
 * the third order Exp-Golomb suffix of |mvd| - 9 in bypass bins, then the
 * sign in a bypass bin when mvd is not 0).  mvd is -32768..32767 (EINVAL
 * otherwise).
 */
int h264enc_cabac_mvd(struct h264enc_cabac *cabac, const struct h264sw_slice_ctx *ctx, unsigned list, unsigned x, unsigned y, unsigned component, int mvd);

/*
 * Codes coded_block_pattern (9.3.2.6): four prefix bins, one per 8x8
 * luma block in order, each with h264sw_ctx_cbp_luma given the bins
 * already coded, then the TU cMax 2 suffix of the chroma part with
 * h264sw_ctx_cbp_chroma.
 */
void h264enc_cabac_cbp(struct h264enc_cabac *cabac, const struct h264sw_slice_ctx *ctx, unsigned cbp);

/* Codes mb_qp_delta -26..25 (9.3.2.7: the value mapped by Table 9-3, U bins; contexts h264sw_ctx_qp_delta and Table 9-39).  Returns 0 or EINVAL. */
int h264enc_cabac_qp_delta(struct h264enc_cabac *cabac, const struct h264sw_slice_ctx *ctx, int qp_delta);

/*
 * Codes end_of_slice_flag (ctxIdx 276, EncodeTerminate).  For 1 the
 * engine flushes, writing the rbsp_stop_one_bit; the caller then writes
 * vcodec_bitwriter_align_zero (rbsp_alignment_zero_bit).
 */
void h264enc_cabac_end_of_slice(struct h264enc_cabac *cabac, unsigned last);

/*
 * Writes the samples of an I_PCM macroblock after its mb_type (whose
 * terminate bin flushed the engine): pcm_alignment_zero_bit up to the
 * byte boundary, the count (384) sample bytes, then restarts the engine
 * (9.3.1.2, InitEncoder 9.3.4.1); the contexts are kept and bins is
 * carried.  Returns 0 or the writer's error.
 */
int h264enc_cabac_pcm(struct h264enc_cabac *cabac, const uint8_t *samples, size_t count);

#endif
