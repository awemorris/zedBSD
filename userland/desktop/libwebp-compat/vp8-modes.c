/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The macroblock header of a lossy key frame: segment, skip flag and
 * prediction modes.
 */

#include "vp8-modes.h"

#include <errno.h>

/*
 * Reads one macroblock's header from the first partition (RFC 6386 19.3
 * macroblock_header for a key frame, 10, 11.1 to 11.5): - segment_id with
 * mb_segment_tree and the header's tree_probs when segmentation's update_map
 * is set; - mb_skip_coeff with skip_prob when skip_enabled; - the luma mode
 * with kf_ymode_tree and kf_ymode_prob; - for B_PRED, the 16 subblock modes
 * in raster order, each with bmode_tree and kf_bmode_prob[A][L], A the mode
 * of the subblock above and L of the one on the left; otherwise every
 * subblock mode is the one the luma mode implies (DC_PRED B_DC_PRED, V_PRED
 * B_VE_PRED, H_PRED B_HE_PRED, TM_PRED B_TM_PRED), for the contexts of the
 * macroblocks that follow (11.3); - the chroma mode with uv_mode_tree and
 * kf_uv_mode_prob.
 */
void
webpc_vp8_read_modes(
	struct webpc_bool *first,
	const struct webpc_vp8_header *header,
	uint8_t above[4],
	uint8_t left[4],
	struct webpc_vp8_modes *modes)
{
	UNUSED_PARAMETER(first);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(above);
	UNUSED_PARAMETER(left);
	UNUSED_PARAMETER(modes);

	/* Not implemented yet. */
}
