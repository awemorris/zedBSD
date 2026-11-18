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

/* The subblocks along one side of a macroblock, and in all (raster order). */
#define MODES_SIDE		4U
#define MODES_SUBBLOCKS		16U

static unsigned modes_implied_bmode(unsigned ymode);
static unsigned modes_context(unsigned mode);
static void modes_read_bpred(struct webpc_bool *first, const uint8_t above[4], const uint8_t left[4], uint8_t bmodes[16]);

/*
 * Gives the subblock mode a 16x16 luma mode stands for when the modes of the
 * macroblocks that follow use it as their context.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 11.3, item 4 (the
 * mapping of the 16x16 luma modes to subblock modes).
 */
static unsigned
modes_implied_bmode(
	unsigned ymode)
{
	/* V_PRED has the vertical subblock mode. */
	if (ymode == WEBPC_VP8_V_PRED) {
		return WEBPC_VP8_B_VE_PRED;
	}

	/* H_PRED has the horizontal subblock mode. */
	if (ymode == WEBPC_VP8_H_PRED) {
		return WEBPC_VP8_B_HE_PRED;
	}

	/* TM_PRED has the true-motion subblock mode. */
	if (ymode == WEBPC_VP8_TM_PRED) {
		return WEBPC_VP8_B_TM_PRED;
	}

	/* DC_PRED, and the B_PRED that never reaches here, give B_DC_PRED. */
	return WEBPC_VP8_B_DC_PRED;
}

/*
 * Gives the context a neighbouring subblock mode selects: the mode itself,
 * or B_DC_PRED for a value no subblock mode has (a caller's bad array never
 * indexes outside the probability table).
 */
static unsigned
modes_context(
	unsigned mode)
{
	/* A value outside the ten subblock modes counts as the frame's edge. */
	if (mode >= WEBPC_VP8_BMODES) {
		return WEBPC_VP8_B_DC_PRED;
	}

	/* Succeeded: the mode itself is a valid context. */
	return mode;
}

/*
 * Reads the 16 subblock modes of a B_PRED macroblock in raster order, each
 * with the probabilities its above and left neighbours select.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 11.3 (the contexts
 * A and L of the j-th subblock; the neighbours of the edge subblocks are the
 * macroblocks above and to the left).
 */
static void
modes_read_bpred(
	struct webpc_bool *first,
	const uint8_t above[4],
	const uint8_t left[4],
	uint8_t bmodes[16])
{
	unsigned index;
	unsigned above_mode;
	unsigned left_mode;
	int mode;

	/* Reads each subblock after the ones above and to its left. */
	for (index = 0U;
	     index < MODES_SUBBLOCKS;
	     index++) {
		/* The mode above: the macroblock above for the top row, else this macroblock's own. */
		if (index < MODES_SIDE) {
			above_mode = modes_context(above[index]);
		} else {
			above_mode = bmodes[index - MODES_SIDE];
		}

		/* The mode to the left: the macroblock on the left for the left column, else this macroblock's own. */
		if ((index % MODES_SIDE) == 0U) {
			left_mode = modes_context(left[index / MODES_SIDE]);
		} else {
			left_mode = bmodes[index - 1U];
		}

		/* The subblock's mode, coded with the probabilities of the pair (above, left). */
		mode = webpc_bool_tree(first, webpc_vp8_bmode_tree, webpc_vp8_kf_bmode_prob[above_mode][left_mode], 0U);
		bmodes[index] = (uint8_t)mode;
	}
}

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
 *
 * Procedure referenced: RFC 6386 (November 2011) section 19.3
 * (macroblock_header) for the order of the fields, sections 10, 11.1, 11.2
 * and 11.4 for each field.
 */
void
webpc_vp8_read_modes(
	struct webpc_bool *first,
	const struct webpc_vp8_header *header,
	uint8_t above[4],
	uint8_t left[4],
	struct webpc_vp8_modes *modes)
{
	unsigned index;
	unsigned implied;
	int value;

	/* Starts from a macroblock with no segment and no skip. */
	modes->segment = 0U;
	modes->skip = 0;

	/* Reads the segment when the frame updates the map. */
	if (header->segmentation.update_map != 0) {
		value = webpc_bool_tree(first, webpc_vp8_segment_tree, header->segmentation.tree_probs, 0U);
		modes->segment = (unsigned)value;
	}

	/* Reads the skip flag when the frame has one. */
	if (header->skip_enabled != 0) {
		value = webpc_bool_read(first, header->skip_prob);
		modes->skip = value;
	}

	/* Reads the luma mode. */
	value = webpc_bool_tree(first, webpc_vp8_kf_ymode_tree, webpc_vp8_kf_ymode_prob, 0U);
	modes->ymode = (unsigned)value;

	/*
	 * Fills the 16 subblock modes: a B_PRED macroblock codes them, any
	 * other has the one its luma mode implies.
	 */
	if (modes->ymode == WEBPC_VP8_B_PRED) {
		modes_read_bpred(first, above, left, modes->bmodes);
	} else {
		implied = modes_implied_bmode(modes->ymode);
		for (index = 0U;
		     index < MODES_SUBBLOCKS;
		     index++) {
			modes->bmodes[index] = (uint8_t)implied;
		}
	}

	/* Reads the chroma mode. */
	value = webpc_bool_tree(first, webpc_vp8_uv_mode_tree, webpc_vp8_kf_uv_mode_prob, 0U);
	modes->uvmode = (unsigned)value;

	/* Hands this macroblock's bottom row and right column on as the next contexts. */
	for (index = 0U;
	     index < MODES_SIDE;
	     index++) {
		above[index] = modes->bmodes[(MODES_SUBBLOCKS - MODES_SIDE) + index];
		left[index] = modes->bmodes[(index * MODES_SIDE) + (MODES_SIDE - 1U)];
	}
}
