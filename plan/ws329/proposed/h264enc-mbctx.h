/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The state the software H.264 encoder decides and codes one macroblock
 * with (struct only, no functions): the speed preset's settings, the
 * macroblock context, and the decision handed from the mode decision
 * (h264enc-mode.h, h264enc-mode-b.h, h264enc-intra.h) to the coding of
 * the macroblock (h264enc-mb.h).
 *
 * Ownership: the encoder's slice state owns one struct h264enc_mbctx per
 * slice being coded (so slices can later be coded by separate threads);
 * everything it points to is the encoder's and lives at least for the
 * slice.  The slice context it points to is the decoder's own structure,
 * filled as the decoder fills it for the same slice.
 */

#ifndef LIBMEDIA_H264ENC_MBCTX_H
#define LIBMEDIA_H264ENC_MBCTX_H

#include "h264enc-cavlc.h"
#include "h264enc-cost.h"
#include "h264enc-me.h"
#include "h264enc-quant.h"
#include "h264enc-refplanes.h"
#include "h264enc-source.h"
#include "h264sw.h"
#include "h264sw-context.h"

/*
 * The settings of a speed preset (fixed values in h264enc-mode.h's
 * h264enc_presets): the default number of P references; the motion
 * search settings (lambda and the limits are filled per picture);
 * partitions: 1 when 16x8, 8x16 and 8x8 are tried; partition_all_refs: 1
 * when every reference is searched for each partition (else only the
 * best 16x16 reference); intra4x4_always: 1 when Intra_4x4 (and
 * Intra_8x8) is tried for every macroblock of P and B slices (else only
 * when the best inter cost is above the Intra_16x16 cost); intra8x8: 1
 * when Intra_8x8 is tried in High; transform_choice: 1 when inter
 * macroblocks of High choose between the 4x4 and 8x8 transforms (else
 * always 4x4; the PPS's transform_8x8_mode_flag is intra8x8 ||
 * transform_choice); early_skip: 1 when a macroblock whose skip
 * prediction leaves no level after quantization is taken as skipped
 * before any search.
 */
struct h264enc_preset {
	unsigned refs;
	struct h264enc_me_params me;
	int partitions;
	int partition_all_refs;
	int intra4x4_always;
	int intra8x8;
	int transform_choice;
	int early_skip;
};

/*
 * One macroblock's decision.
 *
 * kind: H264SW_MB_* (I_4X4, I_8X8, I_16X16, P_SKIP, P (16x16, 16x8, 8x16
 * as mb_type says), P_8X8, B_SKIP, B_DIRECT, B, B_8X8; never I_NXN,
 * P_8X8REF0 or I_PCM, which only the coding chooses).  mb_type: the value
 * of 7.4.5 for the slice type (for the intra kinds the value the coding
 * finishes: I_16X16's depends on the coded block pattern), unused for
 * the skip kinds.  sub_mb_type[4]: for the 8x8 kinds (P: 0 P_L0_8x8 only;
 * B: 0 direct, 1 L0, 2 L1, 3 Bi, all 8x8).  ref_idx[list][part] and
 * mv[list][part] (quarter samples) of each macroblock partition or 8x8
 * sub-macroblock in mbPartIdx order (-1 and 0 for an unused list; for
 * the direct kinds and direct sub-macroblocks they are not used: the
 * coding derives them).  intra_mode[16]: Intra4x4PredMode of each
 * luma4x4BlkIdx (I_4X4) or Intra8x8PredMode in [0..3] (I_8X8);
 * intra16x16_mode; chroma_mode (intra kinds).  levels: for I_4X4 and
 * I_8X8 the luma levels the decision already computed while it
 * reconstructed block by block (luma4x4[blk][16] or luma8x8[blk][64] in
 * scan order), which the coding uses as they are.  transform: 0 4x4, 1
 * 8x8, 2 let the coding choose (inter kinds of High only).  cost: the
 * decision's cost.
 */
struct h264enc_mb_decision {
	unsigned kind;
	unsigned mb_type;
	unsigned sub_mb_type[4];
	int ref_idx[2][4];
	int16_t mv[2][4][2];
	uint8_t intra_mode[16];
	unsigned intra16x16_mode;
	unsigned chroma_mode;
	int32_t luma4x4[16][16];
	int32_t luma8x8[4][64];
	unsigned transform;
	uint32_t cost;
};

/* The transform choices of struct h264enc_mb_decision. */
#define H264ENC_TRANSFORM_4X4 0U
#define H264ENC_TRANSFORM_8X8 1U
#define H264ENC_TRANSFORM_CHOOSE 2U

/*
 * The context of the macroblock being decided and coded.
 *
 * slice: the decoder's slice context of the slice being coded (picture,
 * header, slice_num, lists, weights, direct, mb_addr, n, decoded_4x4, qp,
 * prev_qp_delta_nonzero, prev_mb_addr, syntax); its picture's frame is
 * the frame being reconstructed.  source: the source picture.  ref[list]
 * [i]: the planes of RefPicListX[ i ]'s frame (NULL where the entry is
 * missing).  quant, lambda, cavlc: the encoder's tables.  preset: the
 * settings, with me.lambda and the limits set for this picture.  qp: the
 * macroblock's QP (the picture's).  transform_8x8_mode: the PPS's
 * transform_8x8_mode_flag (Intra_8x8 and the 8x8 transform may be used;
 * 1 only in High with a preset that uses either).  cabac: the slice codes
 * with CABAC.  level_limit: the largest |level| the entropy coding can
 * send (H264ENC_CAVLC_LEVEL_LIMIT for CAVLC outside High, 32767
 * otherwise).  pcm_nonzero: 1 in the Constrained Baseline and Main
 * profiles, whose I_PCM samples must not be 0 (A.2.1, A.2.2).  scratch: room for the predictions and
 * residuals a decision needs (at least H264ENC_SCRATCH bytes).
 */
#define H264ENC_SCRATCH 4096U

struct h264enc_mbctx {
	struct h264sw_slice_ctx *slice;
	const struct h264enc_source *source;
	const struct h264enc_refplanes *ref[2][H264SW_MAX_LIST];
	const struct h264enc_quant *quant;
	const struct h264enc_lambda *lambda;
	const struct h264enc_cavlc *cavlc;
	struct h264enc_preset preset;
	int qp;
	int transform_8x8_mode;
	int cabac;
	int32_t level_limit;
	int pcm_nonzero;
	uint8_t scratch[H264ENC_SCRATCH];
};

#endif
