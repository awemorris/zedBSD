/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice header of the software H.264 decoder (7.3.3, 7.4.3) with its
 * reference picture list modification (7.3.3.1), prediction weight table
 * (7.3.3.2) and decoded reference picture marking (7.3.3.3), and the test
 * for the first slice of a new picture (7.4.1.2.4).
 */

#ifndef LIBMEDIA_H264SW_SLICE_H
#define LIBMEDIA_H264SW_SLICE_H

#include "h264sw.h"
#include "vcodec-bitreader.h"
#include "h264sw-nal.h"
#include "h264sw-ps.h"

/* The slice types, as slice_type % 5 (Table 7-6). */
#define H264SW_SLICE_P 0U
#define H264SW_SLICE_B 1U
#define H264SW_SLICE_I 2U
#define H264SW_SLICE_SP 3U
#define H264SW_SLICE_SI 4U

/*
 * The most modification_of_pic_nums_idc operations kept for one list.
 *
 * The syntax does not bound them, but an operation beyond the
 * num_ref_idx_lX_active_minus1 + 1 positions of the list has no effect
 * other than an error; a slice with more than this is refused.
 */
#define H264SW_MAX_MODIFICATIONS 33U

/* The most memory_management_control_operation entries kept for one picture (more are refused). */
#define H264SW_MAX_MMCO 66U

/* The modification_of_pic_nums_idc values (Table 7-7). */
#define H264SW_MOD_SUBTRACT 0U
#define H264SW_MOD_ADD 1U
#define H264SW_MOD_LONG_TERM 2U

/* The memory_management_control_operation values (Table 7-9). */
#define H264SW_MMCO_END 0U
#define H264SW_MMCO_SHORT_UNUSED 1U
#define H264SW_MMCO_LONG_UNUSED 2U
#define H264SW_MMCO_SHORT_TO_LONG 3U
#define H264SW_MMCO_MAX_LONG_INDEX 4U
#define H264SW_MMCO_ALL_UNUSED 5U
#define H264SW_MMCO_CURRENT_TO_LONG 6U

/*
 * One modification of a reference picture list (7.3.3.1): idc is
 * modification_of_pic_nums_idc 0..2 and value is abs_diff_pic_num_minus1
 * (idc 0 and 1) or long_term_pic_num (idc 2).  The ending idc 3 is not
 * kept.
 */
struct h264sw_modification {
	unsigned idc;
	uint32_t value;
};

/*
 * One memory management control operation (7.3.3.3) with the fields its
 * operation sends; the others are 0.
 */
struct h264sw_mmco {
	unsigned operation;
	uint32_t difference_of_pic_nums_minus1;
	uint32_t long_term_pic_num;
	uint32_t long_term_frame_idx;
	uint32_t max_long_term_frame_idx_plus1;
};

/*
 * The weights and offsets of one reference index of one list (7.4.3.2),
 * already set to their inferred values when the flags are 0: weight
 * 2^luma_log2_weight_denom (or the chroma denominator) and offset 0.
 * [0] is Cb and [1] Cr.  The offsets are in sample units (for 8 bits
 * the offset as sent).
 */
struct h264sw_weight {
	int luma_weight;
	int luma_offset;
	int chroma_weight[2];
	int chroma_offset[2];
	int luma_present;
	int chroma_present;
};

/*
 * One slice header with the values derived from it.
 *
 * Owned by the decoder's slice state; one header lives while its slice
 * is decoded, and the first slice's header of a picture is kept until
 * the picture is marked (the marking fields are the same in every slice
 * of a picture, 7.4.3.3).
 */
struct h264sw_slice_header {
	/* The NAL unit: nal_unit_type, nal_ref_idc, and IdrPicFlag (type 5). */
	unsigned nal_type;
	unsigned nal_ref_idc;
	int idr;

	/* first_mb_in_slice (below PicSizeInMbs), slice_type % 5, and whether slice_type was 5..9 (every slice of the picture of this type). */
	unsigned first_mb;
	unsigned slice_type;
	int slice_type_fixed;

	/* pic_parameter_set_id, and frame_num (log2_max_frame_num bits). */
	unsigned pps_id;
	uint32_t frame_num;

	/* idr_pic_id (IDR only). */
	uint32_t idr_pic_id;

	/* The order count fields: pic_order_cnt_lsb and delta_pic_order_cnt_bottom (type 0), delta_pic_order_cnt[0] and [1] (type 1); 0 when absent. */
	uint32_t poc_lsb;
	int32_t delta_poc_bottom;
	int32_t delta_poc[2];

	/* redundant_pic_cnt (0 when absent; a slice with a value above 0 belongs to a redundant picture the decoder drops). */
	unsigned redundant_pic_cnt;

	/* direct_spatial_mv_pred_flag (B slices). */
	int direct_spatial;

	/* num_ref_idx_l0_active_minus1 + 1 and the same for list 1: from the PPS or the override, 1..32; list 1 is 0 for P slices, both 0 for I slices. */
	unsigned num_ref_idx_active[2];

	/* The list modifications (7.3.3.1): their count and entries for each list. */
	unsigned modification_count[2];
	struct h264sw_modification modification[2][H264SW_MAX_MODIFICATIONS];

	/*
	 * The prediction weight table (7.3.3.2), read when the PPS asks for
	 * explicit weights (weighted_pred_flag for P, weighted_bipred_idc 1
	 * for B): weighted is 1 then.  The denominators are
	 * luma_log2_weight_denom and chroma_log2_weight_denom (0..7);
	 * weight[list][ref_idx] for ref_idx below num_ref_idx_active[list].
	 */
	int weighted;
	unsigned luma_log2_denom;
	unsigned chroma_log2_denom;
	struct h264sw_weight weight[2][H264SW_MAX_LIST];

	/* The marking (7.3.3.3), for nal_ref_idc above 0: no_output_of_prior_pics_flag and long_term_reference_flag (IDR), adaptive_ref_pic_marking_mode_flag and the operations (non-IDR). */
	int no_output_of_prior_pics;
	int long_term_reference;
	int adaptive_marking;
	unsigned mmco_count;
	struct h264sw_mmco mmco[H264SW_MAX_MMCO];

	/* Whether one of the operations is 5 (all references unused), which resets frame_num and the order count after the picture (8.2.1). */
	int has_mmco5;

	/* cabac_init_idc (0..2, CABAC P and B slices). */
	unsigned cabac_init_idc;

	/* SliceQPY = 26 + pic_init_qp_minus26 + slice_qp_delta (7-30), checked to be 0..51. */
	int qp;

	/* disable_deblocking_filter_idc (0..2), and FilterOffsetA and FilterOffsetB (slice_alpha_c0_offset_div2 and slice_beta_offset_div2 times 2, -12..12). */
	unsigned disable_deblocking;
	int filter_offset_a;
	int filter_offset_b;

	/* The bit position in the RBSP where slice_data() starts, for the macroblock layer to continue from. */
	size_t data_position;
};

/*
 * Reads a slice header from a slice RBSP (7.3.3).
 *
 * bits is at the first bit of the RBSP of a NAL unit of type 1 or 5;
 * nal is that NAL unit.  Looks up the PPS and its SPS in store; reads
 * every field in the order of 7.3.3, the list modifications, the weight
 * table and the marking; derives the fields above.  On return bits is at
 * the start of slice_data() and data_position says where.
 *
 * The header does not check what needs the decoded picture buffer
 * (whether a modification names a picture that exists): the reference
 * list construction does.
 *
 * Returns 0; EINVAL when the PPS or its SPS is absent, a field is out of
 * its range (7.4.3: first_mb_in_slice not below PicSizeInMbs, slice_type
 * above 9, an IDR slice that is not I, frame_num not 0 in an IDR,
 * num_ref_idx_active above 32, a modification_of_pic_nums_idc above 3, a
 * weight outside -128..127, an operation above 6, more than the kept
 * modifications or operations, SliceQPY outside 0..51, a filter offset
 * outside -6..6 before doubling, cabac_init_idc above 2) or a read runs
 * past the end; ENOTSUP for SP and SI slices, field_pic_flag (cannot be
 * present: frame_mbs_only_flag is 1 in every SPS kept), slice groups, and
 * nal_unit_type 2..4 (data partitioning, which the caller filters first).
 */
int h264sw_slice_read(struct vcodec_bitreader *bits, const struct h264sw_nal *nal, const struct h264sw_ps_store *store, struct h264sw_slice_header *header);

/*
 * Reports whether a slice starts a new picture (7.4.1.2.4).
 *
 * previous is the header of the last slice of the current picture (NULL
 * when there is none: then the answer is 1); current is the slice just
 * read; sps is the SPS current's PPS names.  Returns 1 when any of the
 * comparisons of 7.4.1.2.4 that apply to frames differs: frame_num,
 * pic_parameter_set_id, nal_ref_idc with one of them 0, pic_order_cnt_type
 * 0 and pic_order_cnt_lsb or delta_pic_order_cnt_bottom, type 1 and
 * delta_pic_order_cnt[0] or [1], IdrPicFlag, both IDR and idr_pic_id.
 * Returns 0 when the slice belongs to the same picture.
 */
int h264sw_slice_new_picture(const struct h264sw_slice_header *previous, const struct h264sw_slice_header *current, const struct h264sw_sps *sps);

#endif
