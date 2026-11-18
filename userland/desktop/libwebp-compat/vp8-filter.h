/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The loop filter of the lossy format (RFC 6386 section 15, and its
 * reference decoder's dixie_loopfilter.c, 20.6, which is correct where
 * the narrative differs): the control parameters of a macroblock, the
 * simple and the normal filters of one edge, and the four steps of one
 * macroblock.
 */

#ifndef LIBWEBP_COMPAT_VP8_FILTER_H
#define LIBWEBP_COMPAT_VP8_FILTER_H

#include "common.h"
#include "vp8-header.h"

/*
 * The controls of one macroblock (RFC 6386 15.4, 20.6
 * calculate_filter_parameters): level 0..63 (0: the macroblock is not
 * filtered at all), interior the interior limit, hev the high edge
 * variance threshold, mb_limit and sub_limit the edge limits of the
 * macroblock edges ((level + 2) * 2 + interior) and of the subblock edges
 * (level * 2 + interior).
 */
struct webpc_vp8_filter_params {
	int level;
	int interior;
	int hev;
	int mb_limit;
	int sub_limit;
};

/*
 * Works out a key-frame macroblock's controls (20.6): the level starts as
 * the header's, is replaced by (absolute) or added to (delta) the
 * segment's filter_level when segmentation is enabled, and is clamped to
 * 0..63 (a clamp libwebp does not make; cwebp's files, with absolute
 * segment values and no deltas, decode alike); when deltas_enabled, ref_delta[0] (intra frame) is added, and
 * mode_delta[0] too for a B_PRED macroblock (b_pred 1), and it is clamped
 * to 0..63 again.  interior is the level shifted right by 2 (sharpness
 * above 4) or 1 (sharpness 1..4), at most 9 - sharpness, and at least 1;
 * with sharpness 0 it is the level, at least 1.  hev is 2 for a level of
 * 40 or more, 1 for 15 or more, else 0 (key frame).  The edge limits
 * follow.
 */
void webpc_vp8_filter_params(const struct webpc_vp8_header *header, unsigned segment, int b_pred,
	struct webpc_vp8_filter_params *params);

/*
 * Filters count segments straddling one edge with the simple filter
 * (15.2, 20.6 filter_v_edge_simple and filter_h_edge_simple): pixel q0 of
 * the first segment is at edge (the first pixel after the edge), p0 at
 * edge - across, and so on; the next segment is along further.  A segment
 * is filtered when |p0 - q0| * 2 + (|p1 - q1| >> 1) <= limit; then a =
 * c(c(p1 - q1) + 3 * (q0 - p0)) on the signed (u2s) values, q0 -= c(a + 4)
 * >> 3 and p0 += c(a + 3) >> 3 (each shift a floor), clamped back (s2u).
 */
void webpc_vp8_filter_simple_edge(uint8_t *edge, ptrdiff_t across, ptrdiff_t along, unsigned count, int limit);

/*
 * Filters count segments straddling a macroblock edge with the normal
 * filter (15.3, 20.6 filter_mb_v_edge and filter_mb_h_edge): pixels as
 * for the simple filter, p3..q3.  limit is the combined edge limit itself
 * (the caller passes params->mb_limit, which is 20.6's 2 * E + I with E =
 * level + 2; it is not doubled here).  A segment is filtered when the
 * simple test passes with limit and every
 * |p3 - p2|, |p2 - p1|, |p1 - p0|, |q1 - q0|, |q2 - q1|, |q3 - q2| is at
 * most interior.  With high edge variance (|p1 - p0| > hev or |q1 - q0| >
 * hev) only p0 and q0 change, as in the common adjustment with the outer
 * taps; otherwise the macroblock filter's weights 27, 18 and 9 (with 63,
 * >> 7) change p2..q2.
 */
void webpc_vp8_filter_mb_edge(uint8_t *edge, ptrdiff_t across, ptrdiff_t along, unsigned count, int limit,
	int interior, int hev);

/*
 * Filters count segments straddling a subblock edge with the normal filter
 * (15.3, 20.6 filter_subblock_v_edge and filter_subblock_h_edge): limit is
 * params->sub_limit (2 * level + interior, not doubled here); the tests as
 * webpc_vp8_filter_mb_edge; then the common adjustment, using the outer
 * taps only with high edge variance, and without it also p1 and q1 change
 * by (f1 + 1) >> 1, where f1 = c(a + 4) >> 3 is the amount q0 moved.
 */
void webpc_vp8_filter_sub_edge(uint8_t *edge, ptrdiff_t across, ptrdiff_t along, unsigned count, int limit,
	int interior, int hev);

/*
 * Filters one macroblock of a frame whose every macroblock is
 * reconstructed (15.1's four steps, in order): y, u and v point at the
 * macroblock's top-left pixel in each plane (y_stride, uv_stride).
 * Nothing is done when params->level is 0.  Step 1 (the left macroblock
 * edge) only when mb_x > 0, step 3 (the top one) only when mb_y > 0; steps
 * 2 and 4 (the subblock edges, luma at 4, 8 and 12, chroma at 4) only
 * when filter_inner (B_PRED, or any coefficient coded, the residue's bit
 * 31).  simple selects the simple filter, which filters luma only
 * (15.2); the normal filter does all three planes.
 */
void webpc_vp8_filter_macroblock(uint8_t *y, uint8_t *u, uint8_t *v, ptrdiff_t y_stride, ptrdiff_t uv_stride,
	unsigned mb_x, unsigned mb_y, const struct webpc_vp8_filter_params *params, int simple, int filter_inner);

#endif
