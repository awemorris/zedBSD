/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sample adaptive offset of the software H.265 decoder (8.7.3): per
 * CTB and colour component, the band offset (bandTable from
 * sao_band_position, bandShift = BitDepth - 5) or the edge offset
 * (hPos and vPos of Table 8-13, edgeIdx by 8-411 and 8-412), applied to
 * the deblocked picture.  Every SAO decision reads the deblocked
 * samples, not samples SAO has already changed, so the picture is
 * filtered from a copy of the deblocked planes.
 */

#ifndef LIBMEDIA_H265SW_SAO_H
#define LIBMEDIA_H265SW_SAO_H

#include "h265sw.h"
#include "h265sw-pic.h"
#include "vcodec-frame.h"

/*
 * A copy of the deblocked planes the SAO reads: plane[cIdx] with
 * stride[cIdx], owned by the decoder and sized for the sequence (one
 * frame's worth).  Only the rows the SAO needs may be copied; the
 * contents are written by h265sw_sao_picture.
 */
struct h265sw_sao_scratch {
	uint8_t *plane[3];
	size_t stride[3];
};

/*
 * Applies SAO to one CTB of one component (8.7.3.2).
 *
 * src is the deblocked component (src_stride), dst the frame's component
 * (dst_stride), both of comp_width by comp_height samples; ( x0, y0 )
 * and ( w, h ) the CTB's samples of that component (clipped to the
 * picture); sao its parameters.  The per-sample rules read the picture
 * context: pic->blk for H265SW_BLK_NOFILTER at the luma location of each
 * sample (8-408: such samples are not changed), and pic->ctb for the
 * slice and tile of each neighbour:
 * a neighbour outside the picture, or across a slice boundary where the
 * slice of the later sample in z-scan order (h265sw_pic_zscan) has
 * slice_loop_filter_across_slices_enabled_flag 0, or across a tile
 * boundary with loop_filter_across_tiles_enabled_flag 0, gives edgeIdx 0
 * (no change).  Results are clipped to 0..255 (8-413).
 */
void h265sw_sao_ctb(const struct h265sw_pic *pic, unsigned c_idx, const struct h265sw_sao *sao, const uint8_t *src, size_t src_stride, uint8_t *dst, size_t dst_stride, unsigned comp_width, unsigned comp_height, unsigned x0, unsigned y0, unsigned w, unsigned h);

/*
 * Applies SAO to the whole picture (8.7.3.1): copies the deblocked planes
 * into scratch, then for each decoded CTB and each component whose slice
 * has slice_sao_luma_flag or slice_sao_chroma_flag and whose SaoTypeIdx
 * is not 0, h265sw_sao_ctb from scratch into frame.  Does nothing when
 * the SPS has sample_adaptive_offset_enabled_flag 0.
 */
void h265sw_sao_picture(const struct h265sw_pic *pic, struct vcodec_frame *frame, struct h265sw_sao_scratch *scratch);

#endif
