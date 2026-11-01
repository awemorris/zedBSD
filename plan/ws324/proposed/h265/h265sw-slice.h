/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice segment header of H.265 (7.3.6.1, 7.4.7.1) with the
 * reference picture list modification (7.3.6.2, 7.4.7.2) and the
 * weighted prediction table (7.3.6.3, 7.4.7.3).
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
 * (luma_log2_denom + delta_chroma_log2_weight_denom, 0..7); for list X and
 * entry i: luma_weight[X][i] LumaWeightLX ( 2^denom + delta_luma_weight_lX
 * when luma_weight_lX_flag, delta -128..127, else 2^denom),
 * luma_offset[X][i] luma_offset_lX (-128..127, 0 when absent: the range
 * of high_precision_offsets_enabled_flag 0, the only one this parse
 * takes for weights, 7.4.7.3: a slice with a pred_weight_table() under
 * an SPS whose high_precision_offsets_enabled_flag is 1 (range_extension
 * bit 6) is ENOTSUP), chroma_weight[X][i][j] ChromaWeightLX
 * ( 2^ChromaLog2WeightDenom + delta_chroma_weight_lX when
 * chroma_weight_lX_flag, delta -128..127, else 2^ChromaLog2WeightDenom)
 * and chroma_offset[X][i][j] ChromaOffsetLX (7-58: from
 * delta_chroma_offset_lX, -512..511, clipped to -128..127; the formula's
 * ( 128 * ChromaWeightLX ) >> ChromaLog2WeightDenom on a value that can
 * be negative is a floor division, h265sw.h).  Entries of lists and
 * indices the slice does not use are 0.
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
 * no_output_of_prior_pics (IRAP only, else 0), pps_id, dependent
 * (dependent_slice_segment_flag), address (slice_segment_address, a CTB
 * address in raster scan, 0..PicSizeInCtbsY - 1, Ceil( Log2(
 * PicSizeInCtbsY ) ) bits), slice_addr_rs (SliceAddrRs: address, or that
 * of the preceding independent segment for a dependent one),
 * num_entry_points with offset_len (offset_len_minus1 + 1; the offsets
 * are read and not kept), data_offset (the byte of the RBSP where
 * slice_segment_data() starts, after byte_alignment()).
 *
 * Independent segments (copied into a dependent one): reserved_flags
 * (slice_reserved_flag[ i ] as bit i); slice_type; pic_output
 * (pic_output_flag, 1 when absent); colour_plane_id (0 when absent);
 * poc_lsb (slice_pic_order_cnt_lsb, 0 for IDR); st_rps_sps
 * (short_term_ref_pic_set_sps_flag, 0 for IDR) with st_rps_idx
 * (short_term_ref_pic_set_idx, 0 when absent); st_rps the short-term set
 * in use (the SPS's set st_rps_idx, or the one in the header; empty for
 * IDR) and, for a set in the header, st_syntax as signalled (zero
 * otherwise) with st_syntax.bits the bits st_ref_pic_set() took
 * (NumBitsForSTRefPicSetInSlice) and st_syntax.ref_num_delta_pocs
 * NumDeltaPocs[ RefRpsIdx ] when it is predicted;
 * num_long_term_sps and num_long_term_pics (their sum num_long_term at
 * most H265SW_MAX_RPS - 1 - NumDeltaPocs) with, for each i,
 * lt_idx_sps[ i ] (i < num_long_term_sps, 0..num_lt_sps - 1, 0 when
 * absent), lt_poc_lsb (PocLsbLt), lt_used (UsedByCurrPicLt),
 * lt_msb_present (delta_poc_msb_present_flag) and lt_msb_cycle
 * (DeltaPocMsbCycleLt, 7-52, accumulated in uint64_t: each
 * delta_poc_msb_cycle_lt is checked against 0..2^( 32 -
 * log2_max_poc_lsb ) (7.4.7.1) and the sum must stay within that bound
 * too, else EINVAL); temporal_mvp (slice_temporal_mvp_enabled_flag);
 * sao_luma, sao_chroma; num_ref_idx[2] (num_ref_idx_lX_active_minus1 +
 * 1, 1..15, 0 for lists the slice type does not use); list_mod[X] and
 * list_entry[X][i] (ref_pic_list_modification_flag_lX, list_entry_lX,
 * Ceil( Log2( NumPicTotalCurr ) ) bits, below NumPicTotalCurr);
 * mvd_l1_zero; cabac_init; collocated_from_l0 (1 when absent);
 * collocated_ref_idx (below the list's num_ref_idx); weights (when the
 * PPS asks for them); max_merge_cand (MaxNumMergeCand 1..5); qp
 * (SliceQpY = 26 + init_qp_minus26 + slice_qp_delta, 7-54,
 * -QpBdOffsetY..51); cb_qp_offset and cr_qp_offset (slice_cb_qp_offset,
 * -12..12, with the PPS's offset also -12..12); deblocking_override
 * (deblocking_filter_override_flag); deblocking_disabled,
 * beta_offset_div2 and tc_offset_div2 (the slice's, -6..6, or the PPS's
 * when not overridden); loop_filter_across_slices (the PPS's when
 * absent).
 *
 * Derived: num_pic_total_curr (NumPicTotalCurr, 7-57: the used entries
 * of st_rps and of the long-term entries; A.4.1 limits it to 8 below
 * level 8.5, but the parse takes up to H265SW_MAX_RPS - 1, which its
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

	unsigned reserved_flags;
	unsigned slice_type;
	int pic_output;
	unsigned colour_plane_id;
	uint32_t poc_lsb;
	int st_rps_sps;
	unsigned st_rps_idx;
	struct h265sw_st_rps st_rps;
	struct h265sw_st_rps_syntax st_syntax;
	unsigned num_long_term_sps;
	unsigned num_long_term_pics;
	unsigned num_long_term;
	uint8_t lt_idx_sps[H265SW_MAX_RPS];
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
	int deblocking_override;
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
 * nal is the segment's NAL unit (a decodable VCL type; another type is
 * EINVAL); bits is at the first bit of its RBSP.  Clears *out, then
 * writes nal_type and temporal_id from nal, first_in_pic,
 * no_output_of_prior_pics and pps_id.  Returns 0; EINVAL for a read
 * error or a pps_id above 63.
 */
int h265sw_slice_read_start(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_nal *nal);

/*
 * Reads the rest of a slice segment header after h265sw_slice_read_start
 * (7.3.6.1 to 7.3.6.3), with the active sps and pps (pps->id is
 * out->pps_id and pps->sps_id is sps->id, else EINVAL: a caller's
 * mistake; the pair was checked with h265sw_ps_check_pair).
 *
 * independent is the header of the preceding independent slice segment
 * of the same picture (NULL when there is none); a dependent segment
 * (dependent_slice_segment_flag 1, only allowed when the PPS has
 * dependent_slices) copies every independent field of it (struct
 * h265sw_slice_header) and is EINVAL without it.  The weights' presence
 * condition on PicOrderCnt( RefPicListX[ i ] ) != PicOrderCnt( CurrPic )
 * is always true for a single-layer stream without screen content tools
 * (a reference never has the current picture's order count), so every
 * flag is read.  The screen content fields (use_integer_mv_flag, the ACT
 * offsets) and cu_chroma_qp_offset_enabled_flag are never present: the
 * PPS and SPS that would enable them were refused.
 *
 * Checks each value against 7.4.7.1 to 7.4.7.3 with the SPS and PPS
 * (slice_segment_address below PicSizeInCtbsY and above 0 for a segment
 * that is not the first, slice_type 0..2 and I for an IRAP picture,
 * short_term_ref_pic_set_idx below num_short_term_ref_pic_sets, the
 * slice header's set read with h265sw_rps_read( ..., num_st_rps,
 * num_st_rps, sps->st_rps, max_dec_pic_buffering_minus1 of the highest
 * sub-layer, ..., &out->st_syntax ), lt_idx_sps, num_long_term counts,
 * collocated_ref_idx below the list's num_ref_idx, five_minus_max_num_
 * merge_cand 0..4, SliceQpY, the QP and filter offsets, offset_len_minus1
 * 0..31, num_entry_point_offsets at most PicHeightInCtbsY - 1 with WPP
 * and no tiles, tile_columns * tile_rows - 1 with tiles and no WPP, and
 * tile_columns * PicHeightInCtbsY - 1 with both,
 * slice_segment_header_extension_length 0..256) and ends with
 * byte_alignment() (alignment_bit_equal_to_one, then zero bits to the
 * byte boundary, 7.3.2.12; read by this module); data_offset is then
 * the reader's position / 8.
 *
 * A read that runs off the end of the RBSP is EINVAL; a caller that
 * gave a short copy (struct h265sw_nal complete 0) retries with the whole
 * NAL unit before it takes that as a broken segment.
 *
 * Returns 0; EINVAL for any violation or read error; ENOTSUP for weights
 * under high_precision_offsets_enabled_flag 1 (see struct
 * h265sw_weights).  On error out is partly written.
 */
int h265sw_slice_read_rest(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_sps *sps, const struct h265sw_pps *pps, const struct h265sw_slice_header *independent);

#endif
