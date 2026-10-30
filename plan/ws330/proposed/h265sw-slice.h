/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice segment header of the software H.265 decoder (7.3.6.1,
 * 7.4.7.1) with the reference picture list modification (7.3.6.2,
 * 7.4.7.2) and the weighted prediction table (7.3.6.3, 7.4.7.3).
 *
 * A dependent slice segment carries only the first fields; the rest are
 * those of the preceding independent slice segment of the picture
 * (7.4.7.1), which the caller passes in.
 */

#ifndef LIBMEDIA_H265SW_SLICE_H
#define LIBMEDIA_H265SW_SLICE_H

#include "h265sw.h"
#include "h265sw-nal.h"
#include "h265sw-ps.h"
#include "h265sw-rps.h"
#include "vcodec-bitreader.h"

/*
 * The explicit weights of one slice (7.4.7.3), derived: luma_log2_denom
 * luma_log2_weight_denom (0..7), chroma_log2_denom ChromaLog2WeightDenom
 * (0..7); for list X and entry i: luma_weight[X][i] LumaWeightLX ( 2^denom
 * + delta when luma_weight_lX_flag, else 2^denom), luma_offset[X][i]
 * luma_offset_lX (-128..127, 0 when absent), chroma_weight[X][i][j]
 * ChromaWeightLX and chroma_offset[X][i][j] ChromaOffsetLX (7-58: from
 * delta_chroma_offset_lX, clipped to -128..127).  For 8 bits the offsets
 * need no shift (WpOffsetBdShiftY and C are 0).
 */
struct h265sw_weights {
	unsigned luma_log2_denom;
	unsigned chroma_log2_denom;
	int16_t luma_weight[2][H265SW_MAX_REFS];
	int16_t luma_offset[2][H265SW_MAX_REFS];
	int16_t chroma_weight[2][H265SW_MAX_REFS][2];
	int16_t chroma_offset[2][H265SW_MAX_REFS][2];
};

/*
 * One slice segment header, with its derived values.
 *
 * From the NAL unit: nal_type, temporal_id.
 *
 * Every segment: first_in_pic (first_slice_segment_in_pic_flag),
 * no_output_of_prior_pics (IRAP only), pps_id, dependent
 * (dependent_slice_segment_flag), address (slice_segment_address, a CTB
 * address in raster scan, 0..PicSizeInCtbsY - 1), slice_addr_rs
 * (SliceAddrRs: address, or that of the preceding independent segment
 * for a dependent one), num_entry_points with offset_len (entry points
 * are read and not kept: the decoder reads substreams one after another),
 * data_offset (the byte of the RBSP where slice_segment_data() starts,
 * after byte_alignment()).
 *
 * Independent segments (copied into a dependent one): slice_type;
 * pic_output (pic_output_flag, 1 when absent); poc_lsb
 * (slice_pic_order_cnt_lsb, 0 for IDR); st_rps the short-term set in
 * use (the SPS's set short_term_ref_pic_set_idx, or the one in the
 * header) with st_rps_bits (the bits st_ref_pic_set() took in the header,
 * for nothing but the record); num_long_term (num_long_term_sps +
 * num_long_term_pics, at most H265SW_MAX_RPS - NumDeltaPocs) with, for
 * each i, lt_poc_lsb (PocLsbLt), lt_used (UsedByCurrPicLt), lt_msb_present
 * (delta_poc_msb_present_flag) and lt_msb_cycle (DeltaPocMsbCycleLt,
 * 7-52, accumulated in uint64_t: each delta_poc_msb_cycle_lt is checked
 * against 0..2^( 32 - log2_max_poc_lsb ) (7.4.7.1) and the sum must stay
 * within that bound too, else EINVAL); temporal_mvp (slice_temporal_mvp_enabled_flag);
 * sao_luma, sao_chroma; num_ref_idx[2] (num_ref_idx_lX_active_minus1 +
 * 1, 1..15, 0 for lists the slice type does not use); list_mod[X] and
 * list_entry[X][i] (ref_pic_list_modification_flag_lX, list_entry_lX);
 * mvd_l1_zero; cabac_init; collocated_from_l0 (1 when absent);
 * collocated_ref_idx; weights (when the PPS asks for them); max_merge_cand
 * (MaxNumMergeCand 1..5); qp (SliceQpY, 7-54, 0..51);
 * cb_qp_offset and cr_qp_offset (slice_cb_qp_offset, -12..12, with the
 * PPS's offset also -12..12); deblocking_disabled, beta_offset_div2 and
 * tc_offset_div2 (the slice's, or the PPS's when not overridden);
 * loop_filter_across_slices (the PPS's when absent).
 *
 * Derived: num_pic_total_curr (NumPicTotalCurr, 7-57: the used entries
 * of st_rps and of the long-term entries; A.4.1 e) limits it to 8 below
 * level 8.5, but the decoder takes up to H265SW_MAX_RPS, which its
 * arrays hold).
 */
struct h265sw_slice_header {
	unsigned nal_type;
	unsigned temporal_id;

	int first_in_pic;
	int no_output_of_prior_pics;
	unsigned pps_id;
	int dependent;
	uint32_t address;
	uint32_t slice_addr_rs;
	unsigned num_entry_points;
	unsigned offset_len;
	size_t data_offset;

	unsigned slice_type;
	int pic_output;
	uint32_t poc_lsb;
	struct h265sw_st_rps st_rps;
	unsigned st_rps_bits;
	unsigned num_long_term;
	uint32_t lt_poc_lsb[H265SW_MAX_RPS];
	uint8_t lt_used[H265SW_MAX_RPS];
	uint8_t lt_msb_present[H265SW_MAX_RPS];
	uint32_t lt_msb_cycle[H265SW_MAX_RPS];
	int temporal_mvp;
	int sao_luma;
	int sao_chroma;
	unsigned num_ref_idx[2];
	int list_mod[2];
	uint8_t list_entry[2][H265SW_MAX_REFS];
	int mvd_l1_zero;
	int cabac_init;
	int collocated_from_l0;
	unsigned collocated_ref_idx;
	struct h265sw_weights weights;
	unsigned max_merge_cand;
	int qp;
	int cb_qp_offset;
	int cr_qp_offset;
	int deblocking_disabled;
	int beta_offset_div2;
	int tc_offset_div2;
	int loop_filter_across_slices;

	unsigned num_pic_total_curr;
};

/*
 * Reads the first fields of a slice segment header, up to and including
 * slice_pic_parameter_set_id, so the caller can find the PPS and SPS
 * (and decide whether the segment starts a picture) before the rest.
 *
 * Writes first_in_pic, no_output_of_prior_pics and pps_id of out (and
 * nal_type and temporal_id from nal).  Returns 0; EINVAL for a read
 * error or a pps_id above 63.
 */
int h265sw_slice_read_start(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_nal *nal);

/*
 * Reads the rest of a slice segment header after h265sw_slice_read_start
 * (7.3.6.1 to 7.3.6.3), with the active sps and pps.
 *
 * independent is the header of the preceding independent slice segment
 * of the same picture (NULL when there is none); a dependent segment
 * (dependent_slice_segment_flag 1, only allowed when the PPS has
 * dependent_slices) copies every field of it that it does not carry, and
 * is EINVAL without it.  The weights' presence condition on
 * PicOrderCnt( RefPicListX[ i ] ) != PicOrderCnt( CurrPic ) is always
 * true for a single-layer stream without screen content tools (a
 * reference never has the current picture's order count), so every flag
 * is read.
 *
 * Checks each value against 7.4.7.1 to 7.4.7.3 with the SPS and PPS
 * (slice_segment_address below PicSizeInCtbsY, slice_type 0..2 and I for
 * an IRAP picture with nuh_layer_id 0, short_term_ref_pic_set_idx below
 * num_short_term_ref_pic_sets, lt_idx_sps, num_long_term counts,
 * collocated_ref_idx below the list's num_ref_idx, five_minus_max_num_
 * merge_cand 0..4, SliceQpY 0..51, the QP and filter offsets,
 * offset_len_minus1 0..31, num_entry_point_offsets at most
 * PicHeightInCtbsY * tile columns or the tile count as 7.4.7.1 bounds it,
 * slice_segment_header_extension_length 0..256) and ends with
 * byte_alignment() (vcodec_bitreader_byte_alignment); data_offset is then the
 * reader's position / 8.
 *
 * Returns 0; EINVAL for any violation or read error; ENOTSUP for nothing
 * (the SPS and PPS were checked).  On error out is partly written.
 */
int h265sw_slice_read_rest(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_sps *sps, const struct h265sw_pps *pps, const struct h265sw_slice_header *independent);

#endif
