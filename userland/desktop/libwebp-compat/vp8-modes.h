/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The macroblock header of a key frame (RFC 6386 sections 10, 11 and
 * 19.3): the segment, the skip flag and the prediction modes, read from
 * the first partition.
 */

#ifndef LIBWEBP_COMPAT_VP8_MODES_H
#define LIBWEBP_COMPAT_VP8_MODES_H

#include "common.h"
#include "bool.h"
#include "vp8-header.h"

/*
 * One macroblock's header.  segment 0..3 (0 when the map is not updated);
 * skip mb_skip_coeff (0 when the header has no skip flag); ymode
 * WEBPC_VP8_DC_PRED..B_PRED; bmodes the 16 subblock modes in raster order
 * (for a macroblock that is not B_PRED, the mode its ymode implies, 11.3);
 * uvmode WEBPC_VP8_DC_PRED..TM_PRED.
 */
struct webpc_vp8_modes {
	unsigned segment;
	int skip;
	unsigned ymode;
	uint8_t bmodes[16];
	unsigned uvmode;
};

/*
 * Reads one macroblock's header from the first partition (RFC 6386 19.3
 * macroblock_header for a key frame, 10, 11.1 to 11.5):
 *
 *   - segment_id with mb_segment_tree and the header's tree_probs when
 *     segmentation's update_map is set;
 *   - mb_skip_coeff with skip_prob when skip_enabled;
 *   - the luma mode with kf_ymode_tree and kf_ymode_prob;
 *   - for B_PRED, the 16 subblock modes in raster order, each with
 *     bmode_tree and kf_bmode_prob[A][L], A the mode of the subblock
 *     above and L of the one on the left; otherwise every subblock mode is
 *     the one the luma mode implies (DC_PRED B_DC_PRED, V_PRED B_VE_PRED,
 *     H_PRED B_HE_PRED, TM_PRED B_TM_PRED), for the contexts of the
 *     macroblocks that follow (11.3);
 *   - the chroma mode with uv_mode_tree and kf_uv_mode_prob.
 *
 * above holds the 4 subblock modes of the bottom row of the macroblock
 * above (B_DC_PRED above the frame), left the 4 of the right column of
 * the one on the left (B_DC_PRED left of the frame); both are replaced by
 * this macroblock's.  Returns nothing; reading past the partition's end
 * shows in webpc_bool_ended, which the caller checks.
 */
void webpc_vp8_read_modes(struct webpc_bool *first, const struct webpc_vp8_header *header, uint8_t above[4],
	uint8_t left[4], struct webpc_vp8_modes *modes);

#endif
