/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice header of the software H.264 decoder (7.3.3) and the first
 * slice of a picture (7.4.1.2.4).
 */

#include "h264sw-slice.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The largest idr_pic_id (7.4.3). */
#define SLICE_MAX_IDR_PIC_ID 65535U

/* The largest redundant_pic_cnt (7.4.3). */
#define SLICE_MAX_REDUNDANT_PIC_CNT 127U

/* The largest num_ref_idx_lX_active_minus1 this decoder keeps; H264SW_MAX_LIST entries make a list. */
#define SLICE_MAX_REF_IDX_MINUS1 31U

/* The modification_of_pic_nums_idc that ends the modifications of a list (Table 7-7). */
#define SLICE_MOD_END 3U

/* The largest memory_management_control_operation (Table 7-9). */
#define SLICE_MAX_MMCO 6U

/* The largest luma_log2_weight_denom and chroma_log2_weight_denom (7.4.3.2). */
#define SLICE_MAX_WEIGHT_DENOM 7U

/* The range of a weight and of an offset of the prediction weight table (7.4.3.2). */
#define SLICE_MIN_WEIGHT (-128)
#define SLICE_MAX_WEIGHT 127

/* The largest cabac_init_idc (7.4.3). */
#define SLICE_MAX_CABAC_INIT_IDC 2U

/* The range of SliceQPY for 8-bit samples (7.4.3, equation 7-30). */
#define SLICE_MIN_QP 0
#define SLICE_MAX_QP 51

/* The largest disable_deblocking_filter_idc (7.4.3). */
#define SLICE_MAX_DEBLOCKING_IDC 2U

/* The range of slice_alpha_c0_offset_div2 and slice_beta_offset_div2 (7.4.3). */
#define SLICE_MIN_FILTER_DIV2 (-6)
#define SLICE_MAX_FILTER_DIV2 6

/* The disable_deblocking_filter_idc that leaves the offsets out of the header (7.3.3). */
#define SLICE_DEBLOCKING_OFF 1U

/* The highest slice_type (Table 7-6); 5..9 repeat 0..4. */
#define SLICE_MAX_TYPE 9U

/* slice_type values that say every slice of the picture has this type (Table 7-6). */
#define SLICE_TYPE_FIXED_FIRST 5U

/* The number of slice_type values per type, for slice_type % 5. */
#define SLICE_TYPE_COUNT 5U

/* The first and last nal_unit_type values of the data partitions (Table 7-1). */
#define SLICE_NAL_PARTITION_FIRST H264SW_NAL_PARTITION_A
#define SLICE_NAL_PARTITION_LAST H264SW_NAL_PARTITION_C

static int slice_read_identity(struct h264sw_bits *bits, const struct h264sw_nal *nal, const struct h264sw_ps_store *store, struct h264sw_slice_header *header, const struct h264sw_pps **pps, const struct h264sw_sps **sps);
static int slice_read_order(struct h264sw_bits *bits, const struct h264sw_pps *pps, const struct h264sw_sps *sps, struct h264sw_slice_header *header);
static int slice_read_ref_counts(struct h264sw_bits *bits, const struct h264sw_pps *pps, struct h264sw_slice_header *header);
static int slice_read_modifications(struct h264sw_bits *bits, const struct h264sw_sps *sps, struct h264sw_slice_header *header, unsigned list);
static int slice_read_weights(struct h264sw_bits *bits, struct h264sw_slice_header *header);
static int slice_read_weight_list(struct h264sw_bits *bits, struct h264sw_slice_header *header, unsigned list);
static int slice_read_weight_value(struct h264sw_bits *bits, int *value);
static int slice_read_marking(struct h264sw_bits *bits, const struct h264sw_sps *sps, struct h264sw_slice_header *header);
static int slice_read_operations(struct h264sw_bits *bits, const struct h264sw_sps *sps, struct h264sw_slice_header *header);
static int slice_read_quantizer_and_filter(struct h264sw_bits *bits, const struct h264sw_pps *pps, struct h264sw_slice_header *header);

/*
 * Reads a slice header from a slice RBSP (7.3.3).
 *
 * The header is cleared first and filled field by field in the order of
 * the syntax; the first failure ends the read and its error is returned.
 */
int
h264sw_slice_read(
	struct h264sw_bits *bits,
	const struct h264sw_nal *nal,
	const struct h264sw_ps_store *store,
	struct h264sw_slice_header *header)
{
	const struct h264sw_pps *pps;
	const struct h264sw_sps *sps;
	int needs_weights;
	int error;

	/* Without a reader, a NAL unit, a store and a place for the result nothing can be read. */
	if (bits == NULL ||
	    nal == NULL ||
	    store == NULL ||
	    header == NULL)
		return EINVAL;

	/* Starts from a header whose every field is 0, which is the value of an absent field. */
	memset(header, 0, sizeof(*header));
	pps = NULL;
	sps = NULL;

	/* Reads the slice type and the parameter set ids, and finds the parameter sets they name. */
	error = slice_read_identity(bits, nal, store, header, &pps, &sps);
	if (error != 0)
		return error;

	/* Reads frame_num and idr_pic_id. */
	header->frame_num = h264sw_bits_u(bits, sps->log2_max_frame_num);
	if (header->idr != 0) {
		header->idr_pic_id = h264sw_bits_ue(bits);
	}
	if (bits->error != 0)
		return EINVAL;

	/* An IDR picture restarts the frame numbering at 0 (7.4.3), and its identifier has a range. */
	if (header->idr != 0) {
		if (header->frame_num != 0U)
			return EINVAL;
		if (header->idr_pic_id > SLICE_MAX_IDR_PIC_ID)
			return EINVAL;
	}

	/* Reads the picture order count fields, redundant_pic_cnt and the B slice's direct mode flag. */
	error = slice_read_order(bits, pps, sps, header);
	if (error != 0)
		return error;

	/* Reads the number of reference pictures of each list. */
	error = slice_read_ref_counts(bits, pps, header);
	if (error != 0)
		return error;

	/* Reads the modifications of the list 0 of a P or B slice. */
	if (header->slice_type != H264SW_SLICE_I) {
		error = slice_read_modifications(bits, sps, header, 0U);
		if (error != 0)
			return error;
	}

	/* Reads the modifications of the list 1 of a B slice. */
	if (header->slice_type == H264SW_SLICE_B) {
		error = slice_read_modifications(bits, sps, header, 1U);
		if (error != 0)
			return error;
	}

	/* The PPS asks for explicit weights in P slices with weighted_pred_flag and in B slices with weighted_bipred_idc 1. */
	needs_weights = 0;
	if (header->slice_type == H264SW_SLICE_P &&
	    pps->weighted_pred != 0)
		needs_weights = 1;
	if (header->slice_type == H264SW_SLICE_B &&
	    pps->weighted_bipred_idc == 1U)
		needs_weights = 1;

	/* Reads the prediction weight table when the slice has one. */
	if (needs_weights != 0) {
		error = slice_read_weights(bits, header);
		if (error != 0)
			return error;
	}

	/* Reads the reference picture marking of a reference picture. */
	if (header->nal_ref_idc != 0U) {
		error = slice_read_marking(bits, sps, header);
		if (error != 0)
			return error;
	}

	/* Reads cabac_init_idc, which only the CABAC slices that predict from references send. */
	if (pps->cabac != 0 &&
	    header->slice_type != H264SW_SLICE_I) {
		header->cabac_init_idc = h264sw_bits_ue(bits);
		if (header->cabac_init_idc > SLICE_MAX_CABAC_INIT_IDC)
			return EINVAL;
	}

	/* Reads the quantizer and the deblocking filter controls. */
	error = slice_read_quantizer_and_filter(bits, pps, header);
	if (error != 0)
		return error;

	/* A read that ran past the end of the RBSP is a truncated header. */
	if (bits->error != 0)
		return EINVAL;

	/* Records where slice_data() starts for the macroblock layer. */
	header->data_position = bits->position;

	/* Succeeded: the header is complete and bits is at the slice data. */
	return 0;
}

/*
 * Reports whether a slice starts a new picture (7.4.1.2.4).
 *
 * Each comparison of the clause is a separate decision; the first that
 * differs says the slice starts a picture.
 */
int
h264sw_slice_new_picture(
	const struct h264sw_slice_header *previous,
	const struct h264sw_slice_header *current,
	const struct h264sw_sps *sps)
{
	unsigned poc_type;
	int compare_type_0;
	int compare_type_1;

	/* The first slice of a stream, or after a reset, always starts a picture. */
	if (previous == NULL)
		return 1;

	/* A missing current slice cannot be compared; a new picture is the safe answer. */
	if (current == NULL)
		return 1;

	/* A different frame_num, as it is written in the header, is a different picture. */
	if (previous->frame_num != current->frame_num)
		return 1;

	/* A different picture parameter set is a different picture. */
	if (previous->pps_id != current->pps_id)
		return 1;

	/* nal_ref_idc differing with one of the values 0 separates a reference picture from a non-reference one. */
	if (previous->nal_ref_idc != current->nal_ref_idc) {
		if (previous->nal_ref_idc == 0U)
			return 1;
		if (current->nal_ref_idc == 0U)
			return 1;
	}

	/* The order count type decides which order count fields the comparison uses. */
	compare_type_0 = 1;
	compare_type_1 = 1;
	if (sps != NULL) {
		poc_type = sps->poc_type;
		compare_type_0 = 0;
		compare_type_1 = 0;
		if (poc_type == 0U)
			compare_type_0 = 1;
		if (poc_type == 1U)
			compare_type_1 = 1;
	}

	/* With order count type 0 a different pic_order_cnt_lsb or delta_pic_order_cnt_bottom is a different picture. */
	if (compare_type_0 != 0) {
		if (previous->poc_lsb != current->poc_lsb)
			return 1;
		if (previous->delta_poc_bottom != current->delta_poc_bottom)
			return 1;
	}

	/* With order count type 1 a different delta_pic_order_cnt[ 0 ] or [ 1 ] is a different picture. */
	if (compare_type_1 != 0) {
		if (previous->delta_poc[0] != current->delta_poc[0])
			return 1;
		if (previous->delta_poc[1] != current->delta_poc[1])
			return 1;
	}

	/* An IDR picture never continues a non-IDR picture. */
	if (previous->idr != current->idr)
		return 1;

	/* Two IDR pictures in a row are told apart by idr_pic_id. */
	if (current->idr != 0) {
		if (previous->idr_pic_id != current->idr_pic_id)
			return 1;
	}

	/* Succeeded: every comparison matched, so the slice belongs to the same picture. */
	return 0;
}

/*
 * Reads first_mb_in_slice, slice_type and pic_parameter_set_id, and finds the parameter sets.
 *
 * Fills the NAL unit fields of the header, first_mb, the slice type and
 * the PPS id; hands back the PPS and the SPS it names.  Refuses what the
 * decoder does not take (data partitions, SP and SI slices) and what is
 * out of its range.
 */
static int
slice_read_identity(
	struct h264sw_bits *bits,
	const struct h264sw_nal *nal,
	const struct h264sw_ps_store *store,
	struct h264sw_slice_header *header,
	const struct h264sw_pps **pps,
	const struct h264sw_sps **sps)
{
	uint32_t first_mb;
	uint32_t slice_type;
	uint32_t pps_id;
	unsigned type;

	/* Data partitions are outside what the decoder takes; the caller filters them before it gets here. */
	if (nal->type >= SLICE_NAL_PARTITION_FIRST &&
	    nal->type <= SLICE_NAL_PARTITION_LAST)
		return ENOTSUP;

	/* Only a coded slice of a non-IDR or an IDR picture has this header. */
	if (nal->type != H264SW_NAL_SLICE &&
	    nal->type != H264SW_NAL_IDR)
		return EINVAL;

	/* Records the NAL unit fields; IdrPicFlag is nal_unit_type 5. */
	header->nal_type = nal->type;
	header->nal_ref_idc = nal->ref_idc;
	header->idr = 0;
	if (nal->type == H264SW_NAL_IDR)
		header->idr = 1;

	/* Reads the first macroblock, the slice type and the parameter set id. */
	first_mb = h264sw_bits_ue(bits);
	slice_type = h264sw_bits_ue(bits);
	pps_id = h264sw_bits_ue(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Refuses a slice type that Table 7-6 does not have. */
	if (slice_type > SLICE_MAX_TYPE)
		return EINVAL;
	type = (unsigned)(slice_type % SLICE_TYPE_COUNT);
	header->slice_type = type;
	header->slice_type_fixed = 0;
	if (slice_type >= SLICE_TYPE_FIXED_FIRST)
		header->slice_type_fixed = 1;

	/* An IDR picture has only I slices (or SI slices, which are refused below). */
	if (header->idr != 0) {
		if (type != H264SW_SLICE_I &&
		    type != H264SW_SLICE_SI)
			return EINVAL;
	}

	/* Finds the picture parameter set the slice names. */
	if (pps_id >= H264SW_PPS_IDS)
		return EINVAL;
	if (store->pps_present[pps_id] == 0)
		return EINVAL;
	*pps = &store->pps[pps_id];
	header->pps_id = (unsigned)pps_id;

	/* Finds the sequence parameter set the picture parameter set names. */
	if ((*pps)->sps_id >= H264SW_SPS_IDS)
		return EINVAL;
	if (store->sps_present[(*pps)->sps_id] == 0)
		return EINVAL;
	*sps = &store->sps[(*pps)->sps_id];

	/* SP and SI slices need decoding processes this decoder does not have. */
	if (type == H264SW_SLICE_SP ||
	    type == H264SW_SLICE_SI)
		return ENOTSUP;

	/* The first macroblock must be inside the picture (7.4.3). */
	if (first_mb >= (*sps)->size_mbs)
		return EINVAL;
	header->first_mb = (unsigned)first_mb;

	/* Succeeded: the slice is a P, B or I slice of known parameter sets. */
	return 0;
}

/*
 * Reads the picture order count fields, redundant_pic_cnt and direct_spatial_mv_pred_flag.
 *
 * The fields that the order count type or a PPS flag leaves out stay 0.
 */
static int
slice_read_order(
	struct h264sw_bits *bits,
	const struct h264sw_pps *pps,
	const struct h264sw_sps *sps,
	struct h264sw_slice_header *header)
{
	/* Order count type 0 sends the low bits of the count, and the bottom field's difference when the PPS says so. */
	if (sps->poc_type == 0U) {
		header->poc_lsb = h264sw_bits_u(bits, sps->log2_max_poc_lsb);
		if (pps->bottom_field_poc_present != 0)
			header->delta_poc_bottom = h264sw_bits_se(bits);
	}

	/* Order count type 1 sends the differences from the expected count unless they are always 0. */
	if (sps->poc_type == 1U &&
	    sps->delta_pic_order_always_zero == 0) {
		header->delta_poc[0] = h264sw_bits_se(bits);
		if (pps->bottom_field_poc_present != 0)
			header->delta_poc[1] = h264sw_bits_se(bits);
	}

	/* Reads redundant_pic_cnt when the PPS says slices send it. */
	if (pps->redundant_pic_cnt_present != 0) {
		header->redundant_pic_cnt = (unsigned)h264sw_bits_ue(bits);
		if (header->redundant_pic_cnt > SLICE_MAX_REDUNDANT_PIC_CNT)
			return EINVAL;
	}

	/* Reads the choice between the spatial and the temporal direct mode of a B slice. */
	if (header->slice_type == H264SW_SLICE_B)
		header->direct_spatial = (int)h264sw_bits_u1(bits);

	/* A read that ran past the end of the RBSP is a truncated header. */
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the order count fields are read. */
	return 0;
}

/*
 * Reads num_ref_idx_active_override_flag and the list lengths it overrides.
 *
 * An I slice has no reference lists; a P slice has list 0 only; a B slice
 * has both.  Without the override the lengths are the PPS defaults.
 */
static int
slice_read_ref_counts(
	struct h264sw_bits *bits,
	const struct h264sw_pps *pps,
	struct h264sw_slice_header *header)
{
	uint32_t override_flag;
	uint32_t count_minus1;

	/* An I slice does not predict from reference pictures. */
	if (header->slice_type == H264SW_SLICE_I)
		return 0;

	/* Starts from the lengths the PPS gives. */
	header->num_ref_idx_active[0] = pps->num_ref_idx_default[0];
	if (header->slice_type == H264SW_SLICE_B)
		header->num_ref_idx_active[1] = pps->num_ref_idx_default[1];

	/* Reads whether the slice sends lengths of its own. */
	override_flag = h264sw_bits_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Without the override the defaults stand. */
	if (override_flag == 0U)
		return 0;

	/* Reads the length of list 0 and checks it before using it. */
	count_minus1 = h264sw_bits_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (count_minus1 > SLICE_MAX_REF_IDX_MINUS1)
		return EINVAL;
	header->num_ref_idx_active[0] = (unsigned)count_minus1 + 1U;

	/* Reads the length of list 1, which only a B slice has. */
	if (header->slice_type == H264SW_SLICE_B) {
		count_minus1 = h264sw_bits_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (count_minus1 > SLICE_MAX_REF_IDX_MINUS1)
			return EINVAL;
		header->num_ref_idx_active[1] = (unsigned)count_minus1 + 1U;
	}

	/* Succeeded: the list lengths are known. */
	return 0;
}

/*
 * Reads ref_pic_list_modification_flag_lX and the modifications of one list (7.3.3.1).
 *
 * The ending idc 3 is not kept.  A list with more modifications than the
 * header keeps is refused.
 */
static int
slice_read_modifications(
	struct h264sw_bits *bits,
	const struct h264sw_sps *sps,
	struct h264sw_slice_header *header,
	unsigned list)
{
	uint32_t flag;
	uint32_t idc;
	uint32_t value;
	unsigned count;
	int done;

	/* Reads whether the list is modified at all. */
	flag = h264sw_bits_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (flag == 0U)
		return 0;

	/* Reads the modifications up to the ending idc, never beyond what the header keeps. */
	count = 0U;
	done = 0;
	while (done == 0) {
		idc = h264sw_bits_ue(bits);
		if (bits->error != 0)
			return EINVAL;

		/* The idc 3 ends the list. */
		if (idc == SLICE_MOD_END) {
			done = 1;
			continue;
		}

		/* Table 7-7 has no idc above 3. */
		if (idc > SLICE_MOD_END)
			return EINVAL;

		/* Reads the difference (idc 0 and 1) or the long-term picture number (idc 2). */
		value = h264sw_bits_ue(bits);
		if (bits->error != 0)
			return EINVAL;

		/* A difference is at most MaxPicNum, which is MaxFrameNum for frames (7.4.3.1). */
		if (idc != H264SW_MOD_LONG_TERM) {
			if (value >= sps->max_frame_num)
				return EINVAL;
		}

		/* Refuses a list with more modifications than the header keeps. */
		if (count >= H264SW_MAX_MODIFICATIONS)
			return EINVAL;

		/* Keeps the modification. */
		header->modification[list][count].idc = (unsigned)idc;
		header->modification[list][count].value = value;
		count++;
	}

	/* Keeps how many modifications the list has. */
	header->modification_count[list] = count;

	/* Succeeded: the list's modifications are read. */
	return 0;
}

/*
 * Reads pred_weight_table() (7.3.3.2) for the lists the slice type has.
 *
 * The chroma weights are always read: the decoder takes only 4:2:0.
 */
static int
slice_read_weights(
	struct h264sw_bits *bits,
	struct h264sw_slice_header *header)
{
	uint32_t denom;
	int error;

	/* Reads the denominators of the luma and the chroma weights. */
	denom = h264sw_bits_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (denom > SLICE_MAX_WEIGHT_DENOM)
		return EINVAL;
	header->luma_log2_denom = (unsigned)denom;
	denom = h264sw_bits_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (denom > SLICE_MAX_WEIGHT_DENOM)
		return EINVAL;
	header->chroma_log2_denom = (unsigned)denom;
	header->weighted = 1;

	/* Reads the weights of every reference index of list 0. */
	error = slice_read_weight_list(bits, header, 0U);
	if (error != 0)
		return error;

	/* Reads the weights of list 1, which only a B slice has. */
	if (header->slice_type == H264SW_SLICE_B) {
		error = slice_read_weight_list(bits, header, 1U);
		if (error != 0)
			return error;
	}

	/* Succeeded: the weight table is read. */
	return 0;
}

/*
 * Reads the weights and offsets of every reference index of one list.
 *
 * A flag of 0 leaves the inferred values: weight 2^denominator, offset 0
 * (7.4.3.2).
 */
static int
slice_read_weight_list(
	struct h264sw_bits *bits,
	struct h264sw_slice_header *header,
	unsigned list)
{
	struct h264sw_weight *weight;
	uint32_t flag;
	unsigned index;
	unsigned plane;
	int error;

	/* Reads one entry for each reference index of the list. */
	for (index = 0U;
	     index < header->num_ref_idx_active[list];
	     index++) {
		weight = &header->weight[list][index];

		/* Starts from the inferred values. */
		weight->luma_weight = 1 << header->luma_log2_denom;
		weight->luma_offset = 0;
		weight->chroma_weight[0] = 1 << header->chroma_log2_denom;
		weight->chroma_weight[1] = 1 << header->chroma_log2_denom;
		weight->chroma_offset[0] = 0;
		weight->chroma_offset[1] = 0;

		/* Reads the luma weight and offset when the flag says they are sent. */
		flag = h264sw_bits_u1(bits);
		if (bits->error != 0)
			return EINVAL;
		if (flag != 0U) {
			weight->luma_present = 1;
			error = slice_read_weight_value(bits, &weight->luma_weight);
			if (error != 0)
				return error;
			error = slice_read_weight_value(bits, &weight->luma_offset);
			if (error != 0)
				return error;
		}

		/* Reads the weight and offset of Cb and then of Cr when the flag says they are sent. */
		flag = h264sw_bits_u1(bits);
		if (bits->error != 0)
			return EINVAL;
		if (flag != 0U) {
			weight->chroma_present = 1;
			for (plane = 0U;
			     plane < 2U;
			     plane++) {
				error = slice_read_weight_value(bits, &weight->chroma_weight[plane]);
				if (error != 0)
					return error;
				error = slice_read_weight_value(bits, &weight->chroma_offset[plane]);
				if (error != 0)
					return error;
			}
		}
	}

	/* Succeeded: the list's weights are read. */
	return 0;
}

/* Reads one weight or offset, which lies in -128..127 (7.4.3.2). */
static int
slice_read_weight_value(
	struct h264sw_bits *bits,
	int *value)
{
	int32_t read;

	/* Reads the signed value and checks it before keeping it. */
	read = h264sw_bits_se(bits);
	if (bits->error != 0)
		return EINVAL;
	if (read < SLICE_MIN_WEIGHT)
		return EINVAL;
	if (read > SLICE_MAX_WEIGHT)
		return EINVAL;
	*value = (int)read;

	/* Succeeded: the value is in range. */
	return 0;
}

/*
 * Reads dec_ref_pic_marking() (7.3.3.3).
 *
 * An IDR picture sends two flags; any other picture sends the mode flag
 * and, in the adaptive mode, the operations.
 */
static int
slice_read_marking(
	struct h264sw_bits *bits,
	const struct h264sw_sps *sps,
	struct h264sw_slice_header *header)
{
	int error;

	/* An IDR picture sends whether prior pictures are output and whether it is a long-term picture. */
	if (header->idr != 0) {
		header->no_output_of_prior_pics = (int)h264sw_bits_u1(bits);
		header->long_term_reference = (int)h264sw_bits_u1(bits);
		if (bits->error != 0)
			return EINVAL;

		/* Succeeded: an IDR picture has no operations. */
		return 0;
	}

	/* Any other picture sends the marking mode: sliding window (0) or adaptive (1). */
	header->adaptive_marking = (int)h264sw_bits_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* The sliding window mode sends no operations. */
	if (header->adaptive_marking == 0)
		return 0;

	/* Reads the operations of the adaptive mode. */
	error = slice_read_operations(bits, sps, header);
	if (error != 0)
		return error;

	/* Succeeded: the marking is read. */
	return 0;
}

/*
 * Reads the memory management control operations up to the ending operation 0.
 *
 * The ending operation is not kept.  has_mmco5 says whether operation 5
 * is among the ones kept.
 */
static int
slice_read_operations(
	struct h264sw_bits *bits,
	const struct h264sw_sps *sps,
	struct h264sw_slice_header *header)
{
	struct h264sw_mmco *entry;
	uint32_t operation;
	uint32_t value;
	unsigned count;
	int done;

	/* Reads operations until the ending one, never beyond what the header keeps. */
	count = 0U;
	done = 0;
	while (done == 0) {
		operation = h264sw_bits_ue(bits);
		if (bits->error != 0)
			return EINVAL;

		/* The operation 0 ends the list. */
		if (operation == H264SW_MMCO_END) {
			done = 1;
			continue;
		}

		/* Table 7-9 has no operation above 6. */
		if (operation > SLICE_MAX_MMCO)
			return EINVAL;

		/* Refuses a picture with more operations than the header keeps. */
		if (count >= H264SW_MAX_MMCO)
			return EINVAL;

		/* Starts an entry whose fields the operation does not send are 0. */
		entry = &header->mmco[count];
		memset(entry, 0, sizeof(*entry));
		entry->operation = (unsigned)operation;

		/* Operations 1 and 3 name a short-term picture by the difference of picture numbers. */
		if (operation == H264SW_MMCO_SHORT_UNUSED ||
		    operation == H264SW_MMCO_SHORT_TO_LONG)
			entry->difference_of_pic_nums_minus1 = h264sw_bits_ue(bits);

		/* Operation 2 names a long-term picture by its long-term picture number. */
		if (operation == H264SW_MMCO_LONG_UNUSED)
			entry->long_term_pic_num = h264sw_bits_ue(bits);

		/* Operations 3 and 6 give the long-term frame index. */
		if (operation == H264SW_MMCO_SHORT_TO_LONG ||
		    operation == H264SW_MMCO_CURRENT_TO_LONG)
			entry->long_term_frame_idx = h264sw_bits_ue(bits);

		/* Operation 4 gives the largest long-term frame index plus 1, at most max_num_ref_frames (7.4.3.3). */
		if (operation == H264SW_MMCO_MAX_LONG_INDEX) {
			value = h264sw_bits_ue(bits);
			if (value > sps->max_num_ref_frames)
				return EINVAL;
			entry->max_long_term_frame_idx_plus1 = value;
		}

		/* Remembers operation 5, which resets frame_num and the order count after the picture. */
		if (operation == H264SW_MMCO_ALL_UNUSED)
			header->has_mmco5 = 1;

		/* Keeps the entry once its fields are read. */
		if (bits->error != 0)
			return EINVAL;
		count++;
	}

	/* Keeps how many operations the picture has. */
	header->mmco_count = count;

	/* Succeeded: the operations are read. */
	return 0;
}

/*
 * Reads slice_qp_delta and the deblocking filter controls, and derives SliceQPY and the filter offsets.
 *
 * Without deblocking_filter_control_present_flag the filter is on with
 * both offsets 0 (7.4.3).
 */
static int
slice_read_quantizer_and_filter(
	struct h264sw_bits *bits,
	const struct h264sw_pps *pps,
	struct h264sw_slice_header *header)
{
	int32_t qp_delta;
	int64_t qp;
	uint32_t idc;
	int32_t offset;

	/* Reads slice_qp_delta; SliceQPY is the PPS's initial value plus it, and lies in 0..51 (7-30). */
	qp_delta = h264sw_bits_se(bits);
	if (bits->error != 0)
		return EINVAL;
	qp = (int64_t)pps->pic_init_qp + (int64_t)qp_delta;
	if (qp < SLICE_MIN_QP)
		return EINVAL;
	if (qp > SLICE_MAX_QP)
		return EINVAL;
	header->qp = (int)qp;

	/* Without the deblocking controls the filter is on with offsets 0. */
	if (pps->deblocking_control_present == 0)
		return 0;

	/* Reads disable_deblocking_filter_idc. */
	idc = h264sw_bits_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (idc > SLICE_MAX_DEBLOCKING_IDC)
		return EINVAL;
	header->disable_deblocking = (unsigned)idc;

	/* The idc 1 leaves out the offsets. */
	if (idc == SLICE_DEBLOCKING_OFF)
		return 0;

	/* Reads slice_alpha_c0_offset_div2 and doubles it to FilterOffsetA (7-32). */
	offset = h264sw_bits_se(bits);
	if (bits->error != 0)
		return EINVAL;
	if (offset < SLICE_MIN_FILTER_DIV2)
		return EINVAL;
	if (offset > SLICE_MAX_FILTER_DIV2)
		return EINVAL;
	header->filter_offset_a = (int)offset * 2;

	/* Reads slice_beta_offset_div2 and doubles it to FilterOffsetB (7-33). */
	offset = h264sw_bits_se(bits);
	if (bits->error != 0)
		return EINVAL;
	if (offset < SLICE_MIN_FILTER_DIV2)
		return EINVAL;
	if (offset > SLICE_MAX_FILTER_DIV2)
		return EINVAL;
	header->filter_offset_b = (int)offset * 2;

	/* Succeeded: the quantizer and the filter controls are read. */
	return 0;
}
