/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The StdVideo H.264 parameter sets in the form the VPU's set writer takes
 * (vpu-codec.h), as libvulkan sends them and the PowerVR executor reads
 * them (vk-video.c's video_read_sps and video_read_pps): the host checks'
 * stand-in for the wire (ws322-p007b).
 */

#include <stdint.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "drivers/vpu/vpu-codec.h"

void convert_sps(const StdVideoH264SequenceParameterSet *from, struct vpu_h264_sps *to);
void convert_pps(const StdVideoH264PictureParameterSet *from, struct vpu_h264_pps *to);

/* StdVideoH264ScalingLists into the writer's form. */
static void
convert_scaling(const StdVideoH264ScalingLists *from, struct vpu_h264_scaling *to)
{
	to->present_mask = from->scaling_list_present_mask;
	to->default_mask = from->use_default_scaling_matrix_mask;
	memcpy(to->list4, from->ScalingList4x4, sizeof(to->list4));
	memcpy(to->list8, from->ScalingList8x8, sizeof(to->list8));
}

/* The SPS as libvulkan sends it and vk-video.c reads it. */
void
convert_sps(const StdVideoH264SequenceParameterSet *from, struct vpu_h264_sps *to)
{
	uint32_t index;

	memset(to, 0, sizeof(*to));
	to->flags = (uint32_t)from->flags.constraint_set0_flag | ((uint32_t)from->flags.constraint_set1_flag << 1) |
		((uint32_t)from->flags.constraint_set2_flag << 2) | ((uint32_t)from->flags.constraint_set3_flag << 3) |
		((uint32_t)from->flags.constraint_set4_flag << 4) | ((uint32_t)from->flags.constraint_set5_flag << 5) |
		((uint32_t)from->flags.direct_8x8_inference_flag << 6) | ((uint32_t)from->flags.mb_adaptive_frame_field_flag << 7) |
		((uint32_t)from->flags.frame_mbs_only_flag << 8) | ((uint32_t)from->flags.delta_pic_order_always_zero_flag << 9) |
		((uint32_t)from->flags.separate_colour_plane_flag << 10) |
		((uint32_t)from->flags.gaps_in_frame_num_value_allowed_flag << 11) |
		((uint32_t)from->flags.qpprime_y_zero_transform_bypass_flag << 12) |
		((uint32_t)from->flags.frame_cropping_flag << 13) | ((uint32_t)from->flags.seq_scaling_matrix_present_flag << 14);
	to->profile_idc = from->profile_idc;
	to->level = from->level_idc;
	to->chroma_format_idc = from->chroma_format_idc;
	to->id = from->seq_parameter_set_id;
	to->bit_depth_luma_minus8 = from->bit_depth_luma_minus8;
	to->bit_depth_chroma_minus8 = from->bit_depth_chroma_minus8;
	to->log2_max_frame_num_minus4 = from->log2_max_frame_num_minus4;
	to->pic_order_cnt_type = from->pic_order_cnt_type;
	to->offset_for_non_ref_pic = from->offset_for_non_ref_pic;
	to->offset_for_top_to_bottom_field = from->offset_for_top_to_bottom_field;
	to->log2_max_pic_order_cnt_lsb_minus4 = from->log2_max_pic_order_cnt_lsb_minus4;
	to->max_num_ref_frames = from->max_num_ref_frames;
	to->pic_width_in_mbs_minus1 = from->pic_width_in_mbs_minus1;
	to->pic_height_in_map_units_minus1 = from->pic_height_in_map_units_minus1;
	to->crop[0] = from->frame_crop_left_offset;
	to->crop[1] = from->frame_crop_right_offset;
	to->crop[2] = from->frame_crop_top_offset;
	to->crop[3] = from->frame_crop_bottom_offset;
	to->offset_count = from->num_ref_frames_in_pic_order_cnt_cycle;
	for (index = 0; index < to->offset_count && from->pOffsetForRefFrame != NULL; index++)
		to->offsets[index] = from->pOffsetForRefFrame[index];
	if (from->pScalingLists != NULL) {
		to->has_scaling = 1;
		convert_scaling(from->pScalingLists, &to->scaling);
	}
}

/* The PPS as libvulkan sends it. */
void
convert_pps(const StdVideoH264PictureParameterSet *from, struct vpu_h264_pps *to)
{
	memset(to, 0, sizeof(*to));
	to->flags = (uint32_t)from->flags.transform_8x8_mode_flag | ((uint32_t)from->flags.redundant_pic_cnt_present_flag << 1) |
		((uint32_t)from->flags.constrained_intra_pred_flag << 2) |
		((uint32_t)from->flags.deblocking_filter_control_present_flag << 3) |
		((uint32_t)from->flags.weighted_pred_flag << 4) |
		((uint32_t)from->flags.bottom_field_pic_order_in_frame_present_flag << 5) |
		((uint32_t)from->flags.entropy_coding_mode_flag << 6) | ((uint32_t)from->flags.pic_scaling_matrix_present_flag << 7);
	to->sps_id = from->seq_parameter_set_id;
	to->pps_id = from->pic_parameter_set_id;
	to->num_ref_idx_l0_default_active_minus1 = from->num_ref_idx_l0_default_active_minus1;
	to->num_ref_idx_l1_default_active_minus1 = from->num_ref_idx_l1_default_active_minus1;
	to->weighted_bipred_idc = from->weighted_bipred_idc;
	to->pic_init_qp_minus26 = from->pic_init_qp_minus26;
	to->pic_init_qs_minus26 = from->pic_init_qs_minus26;
	to->chroma_qp_index_offset = from->chroma_qp_index_offset;
	to->second_chroma_qp_index_offset = from->second_chroma_qp_index_offset;
	if (from->pScalingLists != NULL) {
		to->has_scaling = 1;
		convert_scaling(from->pScalingLists, &to->scaling);
	}
}

