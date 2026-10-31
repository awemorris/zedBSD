/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reconstruction of one parsed CTU in the software H.265 decoder,
 * coding unit after coding unit in decoding order (8.4, 8.5, 8.6): the
 * quantization parameters (8.6.1), intra prediction transform unit by
 * transform unit (8.4.4.1), the inter prediction of each prediction unit
 * (h265sw-inter.h), the residuals (h265sw-transform.h)
 * added and clipped (8.6.7, 8-325), PCM samples (8.4.1, 8-12 to 8-14: shifted by
 * BitDepth - PcmBitDepth), and the transquant bypass.
 *
 * It also leaves in the picture context what the in-loop filters and
 * later blocks read: QpY, the "coded" flag of each luma transform block,
 * the transform and prediction block edges, and the motion.
 */

#ifndef LIBMEDIA_H265SW_RECON_H
#define LIBMEDIA_H265SW_RECON_H

#include "h265sw.h"
#include "h265sw-ctu.h"
#include "h265sw-inter.h"
#include "h265sw-mvctx.h"
#include "h265sw-reflist.h"
#include "h265sw-scaling.h"
#include "h265sw-transform.h"
#include "vcodec-frame.h"

/*
 * The QP prediction state carried from quantization group to group
 * (8.6.1 step 1): prev_qp_y is qPY_PREV for the next group (QpY of the
 * last CU of the previous group); the caller resets it to SliceQpY at
 * the first group of a slice, of a tile, and of a CTB row when
 * entropy_coding_sync_enabled_flag is 1 (h265sw_recon_qp_reset).
 * group_qp_pred is qPY_PRED of the current group (computed at its first
 * CU, used by its other CUs).
 */
struct h265sw_qp_state {
	int prev_qp_y;
	int group_qp_pred;
};

/*
 * What the reconstruction of a slice segment's CTUs uses: pic, frame (the
 * current picture's samples), sps, pps, header, segment (index in
 * pic->segments), inter (the inter prediction's context for the slice:
 * its frame is the same frame), scaling (the picture's quantization
 * matrices, NULL when scaling_list_enabled_flag is 0), qp (the state
 * above).
 */
struct h265sw_recon_ctx {
	struct h265sw_pic *pic;
	struct vcodec_frame *frame;
	const struct h265sw_sps *sps;
	const struct h265sw_pps *pps;
	const struct h265sw_slice_header *header;
	unsigned segment;
	const struct h265sw_inter_ctx *inter;
	const struct h265sw_scaling_factors *scaling;
	struct h265sw_qp_state *qp;
};

/* Sets qPY_PREV to slice_qp (the first quantization group of a slice, tile or wavefront row). */
void h265sw_recon_qp_reset(struct h265sw_qp_state *state, int slice_qp);

/*
 * Reconstructs coding unit cu_index (below ctu->num_cu) of a parsed CTU
 * into ctx->frame, as h265sw_recon_ctu does for each CU.  The coding
 * units of a CTU must be reconstructed in decoding order, each once
 * (the QP state and the intra reference samples depend on the CUs
 * before); a caller may interleave other work between them (the encoder
 * reconstructs each CU as it decides it).  Same returns as
 * h265sw_recon_ctu.
 */
int h265sw_recon_cu(const struct h265sw_recon_ctx *ctx, const struct h265sw_ctu *ctu, unsigned cu_index);

/*
 * Reconstructs every coding unit of a parsed CTU into ctx->frame
 * (h265sw_recon_cu for each in order).
 *
 * Per CU: at a quantization group's first CU, qPY_PRED from qPY_PREV and
 * the left and upper groups inside the same CTB (8.6.1 steps 2 to 4,
 * h265sw_pic_available and h265sw_pic_zscan for ctbAddrA and B), then
 * h265sw_transform_qp with the CU's CuQpDeltaVal; QpY of the CU's blocks
 * written; qPY_PREV becomes the QpY of the group's last CU.
 *
 * Intra: for each transform unit in order, the luma block's reference
 * samples gathered from the frame with their availability (6.4.1 per
 * 4x4 block, constrained_intra_pred_flag excluding non-intra blocks,
 * 8.4.4.2.2 substitution), filter and predict (h265sw-intra.h) with the
 * prediction block's mode, then the residual added; the chroma blocks
 * likewise with IntraPredModeC (no filtering).  Inter: for each
 * prediction unit h265sw_inter_predict (motion and samples), then the
 * residual of each transform unit added.  Skip: merge, no residual.  PCM: the samples <<
 * ( BitDepth - PcmBitDepth ).  Bypass: residual = levels.  Each
 * residual addition clips to 0..255 (8.6.7).
 *
 * Deblocking inputs written: the edges of every transform block and
 * prediction block (h265sw_pic_mark_edges; the CU's own edges are both),
 * "coded" of each luma transform block with a non-zero level.
 *
 * Returns 0; EINVAL when a decoded reference index names no picture or a
 * merge or AMVP input is out of range (the caller conceals the rest of
 * the slice segment).  Every sample write stays inside the frame: blocks
 * beyond the picture's edge do not occur (coding_quadtree splits them)
 * and are skipped if a broken parse produced one.
 */
int h265sw_recon_ctu(const struct h265sw_recon_ctx *ctx, const struct h265sw_ctu *ctu);

#endif
