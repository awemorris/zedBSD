/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The short-term reference picture sets of H.265: st_ref_pic_set()
 * (7.3.7) with its semantics (7.4.8), including the prediction of one set
 * from another (inter_ref_pic_set_prediction_flag, 7-61 and 7-62).  An
 * SPS holds up to 64 sets; a slice header may carry one more, which may
 * be predicted from the SPS's.
 *
 * Each set is kept twice: derived (the lists DeltaPocS0, DeltaPocS1 and
 * the used flags every decoding process reads) and as signalled (the
 * syntax elements, which the Vulkan Video path hands to a driver that may
 * rebuild the parameter sets or parse the slice headers itself).
 */

#ifndef LIBMEDIA_H265SW_RPS_H
#define LIBMEDIA_H265SW_RPS_H

#include "h265sw.h"
#include "vcodec-bitreader.h"

/*
 * One short-term reference picture set after 7.4.8.
 *
 * num_negative and num_positive are NumNegativePics and NumPositivePics.
 * delta_poc[0..num_negative) are DeltaPocS0 (negative, in decreasing
 * order: the nearest picture first), delta_poc[num_negative ..
 * num_negative + num_positive) are DeltaPocS1 (positive, increasing);
 * used[] are UsedByCurrPicS0 and UsedByCurrPicS1 in the same places.
 * NumDeltaPocs is num_negative + num_positive, at most H265SW_MAX_RPS - 1
 * (the max_pictures bound of h265sw_rps_read, at most 15).  A delta is at
 * most 16 * 2^15 in magnitude for a signalled set (7-67 to 7-70) and
 * grows by at most 2^15 with each prediction from another set (7-61,
 * 7-62), so every delta of 65 sets fits in int32_t.  Entries at or past
 * NumDeltaPocs are 0.
 */
struct h265sw_st_rps {
	unsigned num_negative;
	unsigned num_positive;
	int32_t delta_poc[H265SW_MAX_RPS];
	uint8_t used[H265SW_MAX_RPS];
};

/*
 * One st_ref_pic_set() as signalled (7.3.7), with the inferences of
 * 7.4.8.  Every field is 0 when its syntax element is absent unless said.
 *
 * inter_rps_pred is inter_ref_pic_set_prediction_flag.  With it 1:
 * delta_idx_minus1 (0 when absent, i.e. for a set of the SPS),
 * delta_rps_sign, abs_delta_rps_minus1 (0..2^15 - 1), ref_num_delta_pocs
 * NumDeltaPocs[ RefRpsIdx ] (the number of flag pairs read is that plus
 * one), used_by_curr_pic_flags with used_by_curr_pic_flag[ j ] as bit j,
 * use_delta_flags with use_delta_flag[ j ] as bit j (1 when absent, i.e.
 * when used_by_curr_pic_flag[ j ] is 1).  j is at most 15, so both masks
 * fit in 16 bits.
 *
 * With it 0: num_negative_pics and num_positive_pics,
 * delta_poc_s0_minus1[ i ] and delta_poc_s1_minus1[ i ] (0..2^15 - 1),
 * used_by_curr_pic_s0_flags and used_by_curr_pic_s1_flags with the flag
 * of entry i as bit i.  Entries past the counts are 0.
 *
 * bits is the number of bits st_ref_pic_set() took (for a set of a slice
 * header, NumBitsForSTRefPicSetInSlice of the Vulkan picture information).
 */
struct h265sw_st_rps_syntax {
	int inter_rps_pred;
	unsigned delta_idx_minus1;
	int delta_rps_sign;
	unsigned abs_delta_rps_minus1;
	unsigned ref_num_delta_pocs;
	uint32_t used_by_curr_pic_flags;
	uint32_t use_delta_flags;
	unsigned num_negative_pics;
	unsigned num_positive_pics;
	uint16_t delta_poc_s0_minus1[H265SW_MAX_RPS];
	uint16_t delta_poc_s1_minus1[H265SW_MAX_RPS];
	uint32_t used_by_curr_pic_s0_flags;
	uint32_t used_by_curr_pic_s1_flags;
	unsigned bits;
};

/*
 * Reads st_ref_pic_set( index ) (7.3.7) and derives the set (7.4.8).
 *
 * index is stRpsIdx: 0..num_sets - 1 for a set of the SPS, num_sets for
 * the set of a slice header (num_sets 0..64; index above num_sets is
 * EINVAL).  sets[0..num_sets) are the SPS's sets already derived (for
 * index < num_sets only sets[0..index) are read; sets is NULL only when
 * index is 0).  max_pictures is sps_max_dec_pic_buffering_minus1 of the
 * highest sub-layer (0..15), the bound of NumNegativePics and of
 * NumNegativePics + NumPositivePics (7.4.8).
 *
 * With inter_ref_pic_set_prediction_flag 1 (read only when index is not
 * 0): delta_idx_minus1 is read only for index == num_sets (0..index - 1,
 * else EINVAL); RefRpsIdx = index - ( delta_idx_minus1 + 1 ) (7-59);
 * delta_rps_sign and abs_delta_rps_minus1 (0..2^15 - 1) give deltaRps
 * (7-60); then used_by_curr_pic_flag and use_delta_flag for j =
 * 0..NumDeltaPocs[ RefRpsIdx ], and 7-61 and 7-62 build the new set.
 * The derivation can produce one entry more than the source set has: each
 * count is checked against max_pictures before an entry is stored, so no
 * write leaves the arrays.  Otherwise num_negative_pics (0..max_pictures),
 * num_positive_pics (0..max_pictures - num_negative_pics),
 * delta_poc_s0_minus1, delta_poc_s1_minus1 (0..2^15 - 1) and the used
 * flags give the set (7-63 to 7-71).
 *
 * syntax may be NULL; when it is not, it is written in full with the set
 * as signalled (struct h265sw_st_rps_syntax), bits included.
 *
 * Returns 0 and *out; EINVAL for a value out of range, a derived set
 * whose sizes break the bounds above, or a read error.  On error *out and
 * *syntax are left partly written.
 */
int h265sw_rps_read(struct vcodec_bitreader *bits, unsigned index, unsigned num_sets, const struct h265sw_st_rps *sets, unsigned max_pictures, struct h265sw_st_rps *out, struct h265sw_st_rps_syntax *syntax);

#endif
