/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The deblocking filter of the software H.264 decoder (8.7) for frames
 * without MBAFF: which edges of a macroblock are filtered, the boundary
 * strength of each (8.7.2.1), the thresholds (8.7.2.2 with Tables 8-16
 * and 8-17), and the filters of edges with bS below 4 (8.7.2.3) and
 * equal to 4 (8.7.2.4).
 *
 * The filter runs on a whole picture after all its macroblocks are
 * constructed (so slices in any order, ASO, need nothing special), in
 * increasing macroblock address, each macroblock reading the samples its
 * left and upper neighbours already filtered.  The tables of alpha',
 * beta' and tC0' are data of the specification in h264sw-deblock.c.
 */

#ifndef LIBMEDIA_H264SW_DEBLOCK_H
#define LIBMEDIA_H264SW_DEBLOCK_H

#include "h264sw.h"

struct h264sw_picture_ctx;

/*
 * Filters a decoded picture in place (8.7).
 *
 * For each macroblock address 0..size_mbs - 1: filterLeftMbEdgeFlag, filterTopMbEdgeFlag and
 * filterInternalEdgesFlag from its slice's disable_deblocking_filter_idc
 * (1: none; 2: not across slice boundaries, the neighbour in another
 * slice being "not available") and the picture edges.  Then the luma
 * vertical edges left to right (internal edges 4 and 12 only with
 * transform_size_8x8_flag 0), the luma horizontal edges top to bottom
 * (likewise), then for Cb and Cr the chroma vertical edges 0 and 4 and
 * the horizontal edges 0 and 4 (4:2:0: chroma edge 4 is filtered
 * whatever transform_size_8x8_flag is).  Each edge: bS of each 4-sample
 * segment (h264sw_deblock_bs; chroma uses the bS of the luma segment
 * it corresponds to), qPav = ( qPp + qPq + 1 ) >> 1 (8-453) with the
 * macroblocks' qp (for chroma QPC of each by Table 8-15 with the
 * component's offset, 8.7.2.2 for chromaEdgeFlag 1), FilterOffsetA and
 * FilterOffsetB of the slice holding q0 (the current macroblock), and
 * the filters below.
 *
 * The decoder's concealment gives each macroblock it fills in an entry
 * (kind I_16X16, slice the picture's last slice number, qp the last
 * macroblock's), so the filter treats it as any other intra macroblock.
 */
void h264sw_deblock_picture(struct h264sw_picture_ctx *picture);

/*
 * Derives the boundary strength of the four segments of one edge of a macroblock (8.7.2.1, frames).
 *
 * mb_addr is the macroblock holding q0; vertical is 1 for a vertical
 * edge; edge is 0..3, the edge at luma offset 4 * edge (0 is the
 * macroblock edge).  bs[k] receives bS of the samples k * 4 .. k * 4 + 3
 * along the edge: 4 when the edge is a macroblock edge and either p0 or
 * q0 is in an intra macroblock; 3 when either is intra (internal edge);
 * 2 when the 4x4 block (or 8x8 transform block) holding p0 or q0 has a
 * non-zero coefficient (nonzero); 1 when the two partitions use
 * different reference frames (compared by ref_id, as sets: the order of
 * the lists does not matter) or a different number of motion vectors,
 * or the same frames with motion vectors whose horizontal or vertical
 * components differ by 4 or more quarter samples (for two vectors to the
 * same frame twice, both pairings are tried as 8.7.2.1 says); 0
 * otherwise.  mixedModeEdgeFlag is 0 (frames).
 */
void h264sw_deblock_bs(const struct h264sw_picture_ctx *picture, unsigned mb_addr, int vertical, unsigned edge, uint8_t *bs);

/*
 * Filters one luma edge of 16 samples (8.7.2.2 to 8.7.2.4, chromaEdgeFlag 0).
 *
 * q0 points to the first q0 sample; across is the distance in bytes
 * from a sample to the next one across the edge (1 for a vertical edge,
 * the stride for a horizontal one), so p0 is q0[-across], p1
 * q0[-2 * across], q1 q0[across]; along is the distance to the next line
 * along the edge.  bs[0..3] are the strengths of the four segments of 4
 * lines; a segment with bs 0 is left alone.  qp_av is qPav, offset_a and
 * offset_b FilterOffsetA and FilterOffsetB: indexA = Clip3( 0, 51, qPav
 * + filterOffsetA ) (8-454), indexB likewise (8-455), alpha and beta
 * from Table 8-16 (8-456, 8-457: alpha' and beta' themselves for 8
 * bits), tC0 from Table 8-17 (8-461).  filterSamplesFlag (8-460: bS != 0
 * and |p0 - q0| < alpha and |p1 - p0| < beta and |q1 - q0| < beta) per
 * line; bS < 4 by 8.7.2.3 (tC from tC0, ap and aq), bS 4 by 8.7.2.4.
 */
void h264sw_deblock_luma_edge(uint8_t *q0, ptrdiff_t across, ptrdiff_t along, const uint8_t *bs, int qp_av, int offset_a, int offset_b);

/*
 * Filters one chroma edge of 8 samples of 4:2:0 (8.7.2.2 to 8.7.2.4, chromaEdgeFlag 1).
 *
 * As h264sw_deblock_luma_edge with 8 lines; bs[k] applies to lines
 * 2 * k and 2 * k + 1 (the luma segment k of the corresponding luma
 * edge); qp_av is the chroma qPav.  With chromaEdgeFlag 1 only p0 and q0
 * change: tC = tC0 + 1 for bS < 4, and the 3-tap filters of 8.7.2.4
 * for bS 4.
 */
void h264sw_deblock_chroma_edge(uint8_t *q0, ptrdiff_t across, ptrdiff_t along, const uint8_t *bs, int qp_av, int offset_a, int offset_b);

#endif
