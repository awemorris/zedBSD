/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The weighted sample prediction of the software H.264 decoder: the
 * choice of default, explicit or implicit weights and their values
 * (8.4.3), and the combination of one or two predicted blocks into the
 * prediction (8.4.2.3: default 8.4.2.3.1, weighted 8.4.2.3.2).
 */

#ifndef LIBMEDIA_H264SW_WEIGHT_H
#define LIBMEDIA_H264SW_WEIGHT_H

#include "h264sw.h"
#include "h264sw-ps.h"
#include "h264sw-reflist.h"
#include "h264sw-slice.h"

/* The weighting of a slice: none (8.4.2.3.1), explicit (the slice's table), implicit (from the order counts). */
#define H264SW_WEIGHT_DEFAULT 0U
#define H264SW_WEIGHT_EXPLICIT 1U
#define H264SW_WEIGHT_IMPLICIT 2U

/*
 * The weights of a slice, prepared once when the slice starts.
 *
 * mode is H264SW_WEIGHT_* as the PPS and the slice type select
 * (weighted_pred_flag for P, weighted_bipred_idc for B).  header is the
 * slice header (its explicit table).  For implicit weights,
 * implicit_w0[i][j] is w0 for refIdxL0 i and refIdxL1 j (w1 is 64 - w0),
 * computed by 8.4.3 (8-280 to 8-283) for i below the list 0 count and j
 * below the list 1 count; the luma and chroma weights are equal and
 * logWD is 5 with offsets 0.
 */
struct h264sw_weights {
	unsigned mode;
	const struct h264sw_slice_header *header;
	int16_t implicit_w0[H264SW_MAX_LIST][H264SW_MAX_LIST];
};

/*
 * The factors of one prediction of one component (8.4.3 outputs): logWD,
 * w0, w1, o0, o1, and whether they apply (weighted 0: the default
 * prediction of 8.4.2.3.1).
 */
struct h264sw_weight_factors {
	int weighted;
	unsigned log_wd;
	int w0;
	int w1;
	int o0;
	int o1;
};

/*
 * Prepares the weights of a slice (8.4.3).
 *
 * pps and header give the mode; for implicit weights, lists and the
 * current picture's PicOrderCnt poc give each pair's w0: 32 when
 * DiffPicOrderCnt( pic1, pic0 ) is 0, when either is long-term, or when
 * DistScaleFactor >> 2 is outside -64..128; else 64 - ( DistScaleFactor
 * >> 2 ) with tb, td, tx and DistScaleFactor of 8-201, 8-202, 8-197 and
 * 8-198 (the same as temporal direct's, with pic0 and pic1 in place of
 * the co-located references).
 */
void h264sw_weights_setup(struct h264sw_weights *weights, const struct h264sw_pps *pps, const struct h264sw_slice_header *header, const struct h264sw_reflist *lists, int32_t poc);

/*
 * Gives the factors for one partition's prediction (8.4.3).
 *
 * ref_idx0 and ref_idx1 are refIdxL0 and refIdxL1 (-1 when the list is
 * not used; at least one is used).  component is 0 luma, 1 Cb, 2 Cr.
 * Implicit weights apply only when both lists are used; explicit
 * weights for either (8.4.3's implicitModeFlag and explicitModeFlag).
 */
void h264sw_weights_of(const struct h264sw_weights *weights, int ref_idx0, int ref_idx1, unsigned component, struct h264sw_weight_factors *factors);

/*
 * Combines the predictions of a block into the final prediction (8.4.2.3).
 *
 * p0 and p1 are the list 0 and list 1 predictions (width by height,
 * p_stride bytes per row), either NULL when its list is not used.  With
 * factors->weighted 0: one list is copied (8-271, 8-272), two are
 * averaged ( p0 + p1 + 1 ) >> 1 (8-273).  With weighted 1: one list
 * Clip1( ( ( p * w + 2^( logWD - 1 ) ) >> logWD ) + o ) for logWD >= 1,
 * Clip1( p * w + o ) otherwise (8-274, 8-275), two lists by 8-276.  out
 * may be p0.
 */
void h264sw_weights_apply(const struct h264sw_weight_factors *factors, const uint8_t *p0, const uint8_t *p1, size_t p_stride, unsigned width, unsigned height, uint8_t *out, size_t out_stride);

#endif
