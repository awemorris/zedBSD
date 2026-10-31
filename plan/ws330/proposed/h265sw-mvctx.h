/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inputs the motion vector derivations of the software H.265
 * decoder share (merge mode, h265sw-merge.h; motion vector prediction,
 * h265sw-amvp.h): where the current prediction block lies in its coding
 * block, and what the slice says about its reference pictures.
 * Structures only.
 */

#ifndef LIBMEDIA_H265SW_MVCTX_H
#define LIBMEDIA_H265SW_MVCTX_H

#include "h265sw.h"
#include "h265sw-pic.h"
#include "h265sw-tmvp.h"

/*
 * One prediction block: the coding block ( x_cb, y_cb ) of n_cbs samples,
 * the prediction block ( x_pb, y_pb ) of w by h samples, part_idx
 * partIdx (0..3) and part_mode PartMode (H265SW_PART_*).
 */
struct h265sw_pu {
	int x_cb;
	int y_cb;
	unsigned n_cbs;
	int x_pb;
	int y_pb;
	unsigned w;
	unsigned h;
	unsigned part_idx;
	unsigned part_mode;
};

/*
 * What one slice gives the derivations: pic the picture context (the
 * neighbours' motion and availability); tmvp the temporal candidate's
 * context (col NULL when slice_temporal_mvp_enabled_flag is 0);
 * slice_type; num_ref[X] num_ref_idx_lX_active_minus1 + 1;
 * ref_poc[X][i] and ref_long[X][i] the order count and long-term marking
 * of RefPicListX[ i ]; par_mrg_level Log2ParMrgLevel (2..6);
 * max_merge_cand MaxNumMergeCand (1..5).
 */
struct h265sw_mvctx {
	const struct h265sw_pic *pic;
	struct h265sw_tmvp_ctx tmvp;
	unsigned slice_type;
	unsigned num_ref[2];
	int32_t ref_poc[2][H265SW_MAX_REFS];
	uint8_t ref_long[2][H265SW_MAX_REFS];
	unsigned par_mrg_level;
	unsigned max_merge_cand;
};

#endif
