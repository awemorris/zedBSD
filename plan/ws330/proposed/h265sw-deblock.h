/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The deblocking filter of the software H.265 decoder (8.7.2): the
 * transform and prediction block edges on the 8x8 grid (8.7.2.2,
 * 8.7.2.3, recorded in the picture context by the reconstruction), the
 * boundary strength (8.7.2.4), the luma decisions and the strong and
 * normal filters (8.7.2.5.3, 8.7.2.5.4, 8.7.2.5.6, 8.7.2.5.7, Table
 * 8-12), and the chroma filter for edges of strength 2 on the chroma
 * 8x8 grid (8.7.2.5.5, 8.7.2.5.8).  All vertical edges of the picture
 * first, then all horizontal edges on the result (8.7.2.1).
 */

#ifndef LIBMEDIA_H265SW_DEBLOCK_H
#define LIBMEDIA_H265SW_DEBLOCK_H

#include "h265sw.h"
#include "h265sw-pic.h"
#include "vcodec-frame.h"

/*
 * Derives bS (8.7.2.4) of the edge between the 4x4 blocks p and q
 * (p left of or above q): 2 when either is intra; 1 when the edge is a
 * transform block edge and either luma transform block is "coded"; 1
 * when the two use different reference pictures or a different number of
 * motion vectors, or one vector pair differs by 4 or more in quarter
 * samples in either component (with the pairing rules of the clause for
 * two vectors and the same pictures); else 0.  Pictures are compared by
 * the vcodec_frame id of each block's references (pic->segments of each
 * block's CTB).  tu_edge says whether the edge is a transform block edge.
 */
unsigned h265sw_deblock_bs(const struct h265sw_pic *pic, unsigned xp, unsigned yp, unsigned xq, unsigned yq, int tu_edge);

/*
 * Filters one luma edge segment of 4 lines (8.7.2.5.3, 8.7.2.5.4,
 * 8.7.2.5.6, 8.7.2.5.7).
 *
 * q0 points at the first q sample of the first line; across is the
 * offset from a sample to the next one away from the edge on the q side
 * (1 for a vertical edge, the stride for a horizontal one; p samples are
 * at q0 - across, q0 - 2 * across ...); along the offset from one line to
 * the next.  bs is 1 or 2; qp_p and qp_q are QpY of the two blocks;
 * beta_offset_div2 and tc_offset_div2 those of the slice that contains
 * q0,0.  qPL = ( QpQ + QpP + 1 ) >> 1; Q for beta' Clip3( 0, 51, qPL +
 * ( slice_beta_offset_div2 << 1 ) ), for tC' Clip3( 0, 53, qPL + 2 * (
 * bS - 1 ) + ( slice_tc_offset_div2 << 1 ) ) (Table 8-12, scaled by
 * BitDepth); dE, dEp, dEq by the decision of 8.7.2.5.3; the strong or
 * normal filter.  no_p and no_q (pcm with pcm_loop_filter_disabled_flag,
 * or cu_transquant_bypass_flag, per side) keep that side's samples
 * (nDp or nDq 0).
 */
void h265sw_deblock_luma(uint8_t *q0, ptrdiff_t across, ptrdiff_t along, unsigned bs, int qp_p, int qp_q, int beta_offset_div2, int tc_offset_div2, int no_p, int no_q);

/*
 * Filters one chroma edge segment of `lines` lines (8.7.2.5.5,
 * 8.7.2.5.8) for bS 2: QpC from Table 8-10 of ( ( QpQ + QpP + 1 ) >> 1 )
 * + c_qp_pic_offset (pps_cb_qp_offset or pps_cr_qp_offset), tC' from
 * Table 8-12 with Q = Clip3( 0, 53, QpC + 2 + ( slice_tc_offset_div2 << 1
 * ) ), the one-sample filter of 8.7.2.5.8 (8-403 to 8-405) on p0 and q0; no_p and no_q as for
 * luma.
 */
void h265sw_deblock_chroma(uint8_t *q0, ptrdiff_t across, ptrdiff_t along, unsigned lines, int qp_p, int qp_q, int c_qp_pic_offset, int tc_offset_div2, int no_p, int no_q);

/*
 * Deblocks the whole picture in frame (8.7.2.1): for every edge segment
 * of 4 luma samples on the 8x8 grid that is a transform or prediction
 * block edge (pic->blk edge flags) and not excluded (the picture's
 * boundary; a tile boundary with loop_filter_across_tiles_enabled_flag
 * 0; an edge whose q side is in a slice with
 * slice_deblocking_filter_disabled_flag 1, or that is the upper or left
 * boundary of the q side's slice when that slice's
 * slice_loop_filter_across_slices_enabled_flag is 0), bS
 * (h265sw_deblock_bs) and, when it is above 0, the luma filter; the
 * chroma filter for bS 2 on the chroma 8x8 grid (luma 16x16).  Vertical
 * edges of the whole picture, then horizontal.  CTBs not decoded
 * (concealed) are treated as intra with filtering off on their side.
 */
void h265sw_deblock_picture(const struct h265sw_pic *pic, struct vcodec_frame *frame);

#endif
