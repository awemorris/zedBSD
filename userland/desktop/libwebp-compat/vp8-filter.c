/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The loop filter of the lossy format.
 */

#include "vp8-filter.h"

#include <errno.h>

/*
 * Works out a key-frame macroblock's controls (20.6): the level starts as
 * the header's, is replaced by (absolute) or added to (delta) the segment's
 * filter_level when segmentation is enabled, and is clamped to 0..63; when
 * deltas_enabled, ref_delta[0] (intra frame) is added, and mode_delta[0] too
 * for a B_PRED macroblock (b_pred 1), and it is clamped to 0..63 again.
 */
void
webpc_vp8_filter_params(
	const struct webpc_vp8_header *header,
	unsigned segment,
	int b_pred,
	struct webpc_vp8_filter_params *params)
{
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(segment);
	UNUSED_PARAMETER(b_pred);
	UNUSED_PARAMETER(params);

	/* Not implemented yet. */
}

/*
 * Filters count segments straddling one edge with the simple filter (15.2,
 * 20.6 filter_v_edge_simple and filter_h_edge_simple): pixel q0 of the first
 * segment is at edge (the first pixel after the edge), p0 at edge - across,
 * and so on; the next segment is along further.
 */
void
webpc_vp8_filter_simple_edge(
	uint8_t *edge,
	ptrdiff_t across,
	ptrdiff_t along,
	unsigned count,
	int limit)
{
	UNUSED_PARAMETER(edge);
	UNUSED_PARAMETER(across);
	UNUSED_PARAMETER(along);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(limit);

	/* Not implemented yet. */
}

/*
 * Filters count segments straddling a macroblock edge with the normal filter
 * (15.3, 20.6 filter_mb_v_edge and filter_mb_h_edge with their edge limit
 * level + 2): pixels as for the simple filter, p3..q3.
 */
void
webpc_vp8_filter_mb_edge(
	uint8_t *edge,
	ptrdiff_t across,
	ptrdiff_t along,
	unsigned count,
	int limit,
	int interior,
	int hev)
{
	UNUSED_PARAMETER(edge);
	UNUSED_PARAMETER(across);
	UNUSED_PARAMETER(along);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(limit);
	UNUSED_PARAMETER(interior);
	UNUSED_PARAMETER(hev);

	/* Not implemented yet. */
}

/*
 * Filters count segments straddling a subblock edge with the normal filter
 * (15.3, 20.6 filter_subblock_v_edge and filter_subblock_h_edge): the tests
 * as webpc_vp8_filter_mb_edge; then the common adjustment, using the outer
 * taps only with high edge variance, and without it also p1 and q1 change by
 * (a + 1) >> 1 of the adjustment.
 */
void
webpc_vp8_filter_sub_edge(
	uint8_t *edge,
	ptrdiff_t across,
	ptrdiff_t along,
	unsigned count,
	int limit,
	int interior,
	int hev)
{
	UNUSED_PARAMETER(edge);
	UNUSED_PARAMETER(across);
	UNUSED_PARAMETER(along);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(limit);
	UNUSED_PARAMETER(interior);
	UNUSED_PARAMETER(hev);

	/* Not implemented yet. */
}

/*
 * Filters one macroblock of a frame whose every macroblock is reconstructed
 * (15.1's four steps, in order): y, u and v point at the macroblock's
 * top-left pixel in each plane (y_stride, uv_stride).
 */
void
webpc_vp8_filter_macroblock(
	uint8_t *y,
	uint8_t *u,
	uint8_t *v,
	ptrdiff_t y_stride,
	ptrdiff_t uv_stride,
	unsigned mb_x,
	unsigned mb_y,
	const struct webpc_vp8_filter_params *params,
	int simple,
	int filter_inner)
{
	UNUSED_PARAMETER(y);
	UNUSED_PARAMETER(u);
	UNUSED_PARAMETER(v);
	UNUSED_PARAMETER(y_stride);
	UNUSED_PARAMETER(uv_stride);
	UNUSED_PARAMETER(mb_x);
	UNUSED_PARAMETER(mb_y);
	UNUSED_PARAMETER(params);
	UNUSED_PARAMETER(simple);
	UNUSED_PARAMETER(filter_inner);

	/* Not implemented yet. */
}
