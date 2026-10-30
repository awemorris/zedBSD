/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The short-term reference picture sets of the software H.265 decoder:
 * st_ref_pic_set() (7.3.7) with its semantics (7.4.8), including the
 * prediction of one set from another (inter_ref_pic_set_prediction_flag,
 * 7-61 and 7-62).  An SPS holds up to 64 sets; a slice header may carry
 * one more, which may be predicted from the SPS's.
 */

#ifndef LIBMEDIA_H265SW_RPS_H
#define LIBMEDIA_H265SW_RPS_H

#include "h265sw.h"
#include "vcodec-bits.h"

/*
 * One short-term reference picture set after 7.4.8.
 *
 * num_negative and num_positive are NumNegativePics and NumPositivePics.
 * delta_poc[0..num_negative) are DeltaPocS0 (negative, in decreasing
 * order: the nearest picture first), delta_poc[num_negative ..
 * num_negative + num_positive) are DeltaPocS1 (positive, increasing);
 * used[] are UsedByCurrPicS0 and UsedByCurrPicS1 in the same places.
 * NumDeltaPocs is num_negative + num_positive, at most H265SW_MAX_RPS.
 * A delta is at most 16 * 2^15 in magnitude for a signalled set (7-67
 * to 7-70) and grows by at most 2^15 with each prediction from another
 * set (7-61, 7-62), so every delta of 65 sets fits in int32_t.
 */
struct h265sw_st_rps {
	unsigned num_negative;
	unsigned num_positive;
	int32_t delta_poc[H265SW_MAX_RPS];
	uint8_t used[H265SW_MAX_RPS];
};

/*
 * Reads st_ref_pic_set( index ) (7.3.7) and derives the set (7.4.8).
 *
 * index is stRpsIdx: 0..num_sets - 1 for a set of the SPS, num_sets for
 * the set of a slice header.  sets[0..num_sets) are the SPS's sets
 * already read (for index < num_sets only sets[0..index) are read).
 * max_pictures is sps_max_dec_pic_buffering_minus1 of the highest
 * sub-layer, the bound of NumNegativePics and of NumNegativePics +
 * NumPositivePics (7.4.8).
 *
 * With inter_ref_pic_set_prediction_flag 1: delta_idx_minus1 is read only
 * for index == num_sets (0..index - 1, else inferred 0); RefRpsIdx =
 * index - ( delta_idx_minus1 + 1 ) (7-59); delta_rps_sign and
 * abs_delta_rps_minus1 (0..2^15 - 1) give deltaRps (7-60); then
 * used_by_curr_pic_flag and use_delta_flag for j = 0..NumDeltaPocs[
 * RefRpsIdx ] (use_delta_flag inferred 1 when absent), and 7-61 and
 * 7-62 build the new set.  The derivation can produce one entry more
 * than the source set has, up to H265SW_MAX_RPS + 1: each count is
 * checked against max_pictures before an entry is stored, so no write
 * leaves the arrays.  Otherwise num_negative_pics, num_positive_pics,
 * delta_poc_s0_minus1, delta_poc_s1_minus1 (0..2^15 - 1) and the used
 * flags give the set (7-63 to 7-71).
 *
 * Returns 0 and *out; EINVAL for a value out of range, a derived set
 * whose sizes break the bounds above, or a read error.
 */
int h265sw_rps_read(struct vcodec_bits *bits, unsigned index, unsigned num_sets, const struct h265sw_st_rps *sets, unsigned max_pictures, struct h265sw_st_rps *out);

#endif
