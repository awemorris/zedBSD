/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The libvulkan side of the H.265 video wire round trip: libvulkan's own
 * video encoders (userland/desktop/libvulkan/video.c, included so its
 * static encoders can be called) write an H.265 parameters create with
 * every pointer filled, an H.265 decode and two H.265 profiles into byte
 * runs the executor's readers then read (h265-wire-decode.c).  The
 * context and object calls the encoders do not reach are stubs.
 */

#include "userland/desktop/libvulkan/video.c"

#include <stdlib.h>

#include "plan/ws324/tests/h265-wire-test.h"

static size_t encode_take(struct vulkan_writer *writer, uint8_t *out, size_t capacity);

/* A handle stands for its own identity on the wire. */
struct vulkan_object *
vulkan_nondispatchable_object(
	uint64_t handle)
{
	struct vulkan_object *object;

	/* Takes the handle's value as the object, so the wire id reads it back. */
	object = (struct vulkan_object *)(uintptr_t)handle;

	/* Reports the object that stands for the handle. */
	return object;
}

/* Reports the wire id of an object, which is the handle it stands for. */
uint64_t
vulkan_object_wire_id(
	const struct vulkan_object *object)
{
	uint64_t id;

	/* Takes the object's address back as the handle. */
	id = (uint64_t)(uintptr_t)object;

	/* Reports the wire id. */
	return id;
}

/* Drops the error the encoders report, which this test never reaches. */
void
vulkan_context_error(
	struct vulkan_context *context,
	VkResult error)
{
	(void)context;
	(void)error;
}

/* Stops the run if an encoder tries to talk to a real executor, which the test never wires up. */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reader)
{
	(void)context;
	(void)writer;
	(void)reply_capacity;
	(void)reader;

	/* Aborts: no call under test is meant to reach the executor. */
	abort();
}

/*
 * Encodes an H.265 parameters create: one VPS (buffering, HRD with NAL
 * sub-layers, PTL), one SPS with every pointer (PTL, buffering, scaling
 * lists, two short-term sets, long-term pictures, a VUI whose HRD has NAL
 * and VCL sub-layers, palette initializers) and two sub-layers, and one
 * PPS with scaling lists, explicit tiles and palette initializers.
 */
size_t
wire_encode_h265_parameters(
	uint8_t *out,
	size_t capacity)
{
	static StdVideoH265SubLayerHrdParameters sub_layers[2];
	static StdVideoH265HrdParameters hrd;
	static StdVideoH265DecPicBufMgr dpb;
	static StdVideoH265ProfileTierLevel ptl;
	static StdVideoH265VideoParameterSet vps;
	static StdVideoH265ScalingLists sps_scaling;
	static StdVideoH265ScalingLists pps_scaling;
	static StdVideoH265ShortTermRefPicSet short_term[2];
	static StdVideoH265LongTermRefPicsSps long_term;
	static StdVideoH265SequenceParameterSetVui vui;
	static StdVideoH265PredictorPaletteEntries palette;
	static StdVideoH265SequenceParameterSet sps;
	static StdVideoH265PictureParameterSet pps;
	VkVideoDecodeH265SessionParametersAddInfoKHR add;
	VkVideoDecodeH265SessionParametersCreateInfoKHR h265;
	VkVideoSessionParametersCreateInfoKHR create;
	struct vulkan_writer writer;
	uint32_t index;
	size_t length;

	/* The sub-layers' HRD values, distinct so a reader that slips would show. */
	memset(sub_layers, 0, sizeof(sub_layers));
	for (index = 0U; index < 32U; index++) {
		sub_layers[0].bit_rate_value_minus1[index] = index;
		sub_layers[1].cpb_size_value_minus1[index] = 100U + index;
	}
	sub_layers[1].cbr_flag = 0x5U;

	/* The HRD: NAL and VCL present, each with the two sub-layers. */
	memset(&hrd, 0, sizeof(hrd));
	hrd.flags.nal_hrd_parameters_present_flag = 1U;
	hrd.flags.vcl_hrd_parameters_present_flag = 1U;
	hrd.flags.fixed_pic_rate_general_flag = 0x3U;
	hrd.tick_divisor_minus2 = 7U;
	hrd.cpb_cnt_minus1[0] = 1U;
	hrd.elemental_duration_in_tc_minus1[1] = 9U;
	hrd.pSubLayerHrdParametersNal = sub_layers;
	hrd.pSubLayerHrdParametersVcl = sub_layers;

	/* The buffering limits and the profile, tier and level. */
	memset(&dpb, 0, sizeof(dpb));
	dpb.max_dec_pic_buffering_minus1[0] = 4U;
	dpb.max_num_reorder_pics[0] = 2U;
	dpb.max_latency_increase_plus1[1] = 3U;
	memset(&ptl, 0, sizeof(ptl));
	ptl.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
	ptl.general_level_idc = STD_VIDEO_H265_LEVEL_IDC_4_1;
	ptl.flags.general_progressive_source_flag = 1U;

	/* The video parameter set: id 3, two sub-layers. */
	memset(&vps, 0, sizeof(vps));
	vps.vps_video_parameter_set_id = 3U;
	vps.vps_max_sub_layers_minus1 = 1U;
	vps.vps_num_units_in_tick = 1001U;
	vps.vps_time_scale = 60000U;
	vps.pDecPicBufMgr = &dpb;
	vps.pHrdParameters = &hrd;
	vps.pProfileTierLevel = &ptl;

	/* The scaling lists, each entry its own value. */
	memset(&sps_scaling, 0, sizeof(sps_scaling));
	for (index = 0U; index < 16U; index++)
		sps_scaling.ScalingList4x4[2][index] = (uint8_t)(index + 1U);
	for (index = 0U; index < 64U; index++) {
		sps_scaling.ScalingList8x8[5][index] = (uint8_t)(index + 2U);
		sps_scaling.ScalingList16x16[1][index] = (uint8_t)(index + 3U);
		sps_scaling.ScalingList32x32[1][index] = (uint8_t)(index + 4U);
	}
	sps_scaling.ScalingListDCCoef16x16[4] = 21U;
	sps_scaling.ScalingListDCCoef32x32[1] = 22U;
	memset(&pps_scaling, 0, sizeof(pps_scaling));
	pps_scaling.ScalingList4x4[0][0] = 77U;

	/* Two short-term sets, the second predicted. */
	memset(short_term, 0, sizeof(short_term));
	short_term[0].num_negative_pics = 2U;
	short_term[0].delta_poc_s0_minus1[1] = 1U;
	short_term[1].flags.inter_ref_pic_set_prediction_flag = 1U;
	short_term[1].abs_delta_rps_minus1 = 5U;

	/* The long-term pictures, the VUI with the HRD, the palette. */
	memset(&long_term, 0, sizeof(long_term));
	long_term.used_by_curr_pic_lt_sps_flag = 0x3U;
	long_term.lt_ref_pic_poc_lsb_sps[31] = 99U;
	memset(&vui, 0, sizeof(vui));
	vui.flags.vui_hrd_parameters_present_flag = 1U;
	vui.sar_width = 4U;
	vui.log2_max_mv_length_vertical = 15U;
	vui.pHrdParameters = &hrd;
	memset(&palette, 0, sizeof(palette));
	palette.PredictorPaletteEntries[2][127] = 0x1234U;

	/* The sequence parameter set: 1920x1080, CTB 64, scaling lists, PCM, long-term pictures. */
	memset(&sps, 0, sizeof(sps));
	sps.flags.scaling_list_enabled_flag = 1U;
	sps.flags.sps_scaling_list_data_present_flag = 1U;
	sps.flags.sample_adaptive_offset_enabled_flag = 1U;
	sps.flags.pcm_enabled_flag = 1U;
	sps.flags.long_term_ref_pics_present_flag = 1U;
	sps.flags.sps_temporal_mvp_enabled_flag = 1U;
	sps.flags.strong_intra_smoothing_enabled_flag = 1U;
	sps.flags.vui_parameters_present_flag = 1U;
	sps.chroma_format_idc = STD_VIDEO_H265_CHROMA_FORMAT_IDC_420;
	sps.pic_width_in_luma_samples = 1920U;
	sps.pic_height_in_luma_samples = 1080U;
	sps.sps_video_parameter_set_id = 3U;
	sps.sps_max_sub_layers_minus1 = 1U;
	sps.sps_seq_parameter_set_id = 5U;
	sps.log2_max_pic_order_cnt_lsb_minus4 = 4U;
	sps.log2_min_luma_coding_block_size_minus3 = 0U;
	sps.log2_diff_max_min_luma_coding_block_size = 3U;
	sps.log2_min_luma_transform_block_size_minus2 = 0U;
	sps.log2_diff_max_min_luma_transform_block_size = 3U;
	sps.max_transform_hierarchy_depth_inter = 2U;
	sps.max_transform_hierarchy_depth_intra = 1U;
	sps.num_short_term_ref_pic_sets = 2U;
	sps.num_long_term_ref_pics_sps = 2U;
	sps.pcm_sample_bit_depth_luma_minus1 = 7U;
	sps.pcm_sample_bit_depth_chroma_minus1 = 6U;
	sps.log2_min_pcm_luma_coding_block_size_minus3 = 1U;
	sps.log2_diff_max_min_pcm_luma_coding_block_size = 1U;
	sps.conf_win_bottom_offset = 4U;
	sps.pProfileTierLevel = &ptl;
	sps.pDecPicBufMgr = &dpb;
	sps.pScalingLists = &sps_scaling;
	sps.pShortTermRefPicSet = short_term;
	sps.pLongTermRefPicsSps = &long_term;
	sps.pSequenceParameterSetVui = &vui;
	sps.pPredictorPaletteEntries = &palette;

	/* The picture parameter set: tiles of explicit sizes, its own scaling lists, signed offsets. */
	memset(&pps, 0, sizeof(pps));
	pps.flags.tiles_enabled_flag = 1U;
	pps.flags.cu_qp_delta_enabled_flag = 1U;
	pps.flags.pps_scaling_list_data_present_flag = 1U;
	pps.flags.deblocking_filter_control_present_flag = 1U;
	pps.flags.loop_filter_across_tiles_enabled_flag = 1U;
	pps.pps_pic_parameter_set_id = 33U;
	pps.pps_seq_parameter_set_id = 5U;
	pps.sps_video_parameter_set_id = 3U;
	pps.num_extra_slice_header_bits = 2U;
	pps.num_ref_idx_l0_default_active_minus1 = 3U;
	pps.num_ref_idx_l1_default_active_minus1 = 1U;
	pps.init_qp_minus26 = -4;
	pps.diff_cu_qp_delta_depth = 2U;
	pps.pps_cb_qp_offset = -3;
	pps.pps_cr_qp_offset = 5;
	pps.pps_beta_offset_div2 = -2;
	pps.pps_tc_offset_div2 = 1;
	pps.log2_parallel_merge_level_minus2 = 1U;
	pps.cb_qp_offset_list[5] = -7;
	pps.num_tile_columns_minus1 = 2U;
	pps.num_tile_rows_minus1 = 1U;
	pps.column_width_minus1[0] = 9U;
	pps.column_width_minus1[18] = 18U;
	pps.row_height_minus1[0] = 7U;
	pps.row_height_minus1[20] = 20U;
	pps.pScalingLists = &pps_scaling;
	pps.pPredictorPaletteEntries = &palette;

	/* The sets to add, the H.265 create, the create itself. */
	memset(&add, 0, sizeof(add));
	add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR;
	add.stdVPSCount = 1U;
	add.pStdVPSs = &vps;
	add.stdSPSCount = 1U;
	add.pStdSPSs = &sps;
	add.stdPPSCount = 1U;
	add.pStdPPSs = &pps;
	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR;
	h265.maxStdVPSCount = 2U;
	h265.maxStdSPSCount = 4U;
	h265.maxStdPPSCount = 8U;
	h265.pParametersAddInfo = &add;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
	create.pNext = &h265;
	create.videoSession = (VkVideoSessionKHR)(uintptr_t)0x70U;

	/* Writes the present flag and the encoded create. */
	vulkan_writer_init(&writer);
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)&create);

	/* Takes the bytes out of the writer. */
	length = encode_take(&writer, out, capacity);

	/* Reports the length of the encoded create. */
	return length;
}

/*
 * Encodes an H.265 decode: a reference picture with its sets, three slice
 * segments, a setup slot and two references (the second long term).
 */
size_t
wire_encode_h265_decode(
	uint8_t *out,
	size_t capacity)
{
	StdVideoDecodeH265PictureInfo std_picture;
	VkVideoDecodeH265PictureInfoKHR picture;
	StdVideoDecodeH265ReferenceInfo std_references[2];
	VkVideoDecodeH265DpbSlotInfoKHR dpb[2];
	VkVideoPictureResourceInfoKHR resources[3];
	VkVideoReferenceSlotInfoKHR slots[2];
	VkVideoReferenceSlotInfoKHR setup;
	VkVideoDecodeInfoKHR decode;
	struct vulkan_writer writer;
	uint32_t offsets[3] = { 0U, 700U, 1500U };
	uint32_t index;
	size_t length;

	/* The picture: a reference with its sets' ids, order count and three sets. */
	memset(&std_picture, 0, sizeof(std_picture));
	std_picture.flags.IsReference = 1U;
	std_picture.flags.short_term_ref_pic_set_sps_flag = 1U;
	std_picture.sps_video_parameter_set_id = 3U;
	std_picture.pps_seq_parameter_set_id = 5U;
	std_picture.pps_pic_parameter_set_id = 33U;
	std_picture.NumDeltaPocsOfRefRpsIdx = 2U;
	std_picture.PicOrderCntVal = -6;
	std_picture.NumBitsForSTRefPicSetInSlice = 11U;
	memset(std_picture.RefPicSetStCurrBefore, STD_VIDEO_H265_NO_REFERENCE_PICTURE, sizeof(std_picture.RefPicSetStCurrBefore));
	memset(std_picture.RefPicSetStCurrAfter, STD_VIDEO_H265_NO_REFERENCE_PICTURE, sizeof(std_picture.RefPicSetStCurrAfter));
	memset(std_picture.RefPicSetLtCurr, STD_VIDEO_H265_NO_REFERENCE_PICTURE, sizeof(std_picture.RefPicSetLtCurr));
	std_picture.RefPicSetStCurrBefore[0] = 1U;
	std_picture.RefPicSetLtCurr[0] = 2U;
	memset(&picture, 0, sizeof(picture));
	picture.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR;
	picture.pStdPictureInfo = &std_picture;
	picture.sliceSegmentCount = 3U;
	picture.pSliceSegmentOffsets = offsets;

	/* The three picture resources: the output and the two references. */
	for (index = 0U; index < 3U; index++) {
		memset(&resources[index], 0, sizeof(resources[index]));
		resources[index].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
		resources[index].codedExtent.width = 1920U;
		resources[index].codedExtent.height = 1088U;
		resources[index].imageViewBinding = (VkImageView)(uintptr_t)(0x50U + index);
	}

	/* The two reference slots with their H.265 information. */
	for (index = 0U; index < 2U; index++) {
		memset(&std_references[index], 0, sizeof(std_references[index]));
		std_references[index].PicOrderCntVal = -10 + (int32_t)index;
		memset(&dpb[index], 0, sizeof(dpb[index]));
		dpb[index].sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
		dpb[index].pStdReferenceInfo = &std_references[index];
		memset(&slots[index], 0, sizeof(slots[index]));
		slots[index].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		slots[index].pNext = &dpb[index];
		slots[index].slotIndex = (int32_t)index + 1;
		slots[index].pPictureResource = &resources[index + 1U];
	}
	std_references[1].flags.used_for_long_term_reference = 1U;

	/* The setup slot the picture becomes. */
	memset(&setup, 0, sizeof(setup));
	setup.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	setup.slotIndex = 7;
	setup.pPictureResource = &resources[0];

	/* The decode: the stream range, the output, the setup slot and the references. */
	memset(&decode, 0, sizeof(decode));
	decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
	decode.pNext = &picture;
	decode.srcBuffer = (VkBuffer)(uintptr_t)0x40U;
	decode.srcBufferOffset = 512U;
	decode.srcBufferRange = 2048U;
	decode.dstPictureResource = resources[0];
	decode.pSetupReferenceSlot = &setup;
	decode.referenceSlotCount = 2U;
	decode.pReferenceSlots = slots;

	/* Writes the present flag and the encoded decode. */
	vulkan_writer_init(&writer);
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)&decode);

	/* Takes the bytes out of the writer. */
	length = encode_take(&writer, out, capacity);

	/* Reports the length of the encoded decode. */
	return length;
}

/* Encodes a profile: H.265 decode, 8-bit 4:2:0, with the H.265 profile chained (idc as given). */
size_t
wire_encode_h265_profile(
	uint8_t *out,
	size_t capacity,
	uint32_t profile_idc)
{
	VkVideoDecodeH265ProfileInfoKHR h265;
	VkVideoProfileInfoKHR profile;
	struct vulkan_writer writer;
	size_t length;

	/* The H.265 profile record and the profile it is chained to. */
	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
	h265.stdProfileIdc = (StdVideoH265ProfileIdc)profile_idc;
	memset(&profile, 0, sizeof(profile));
	profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	profile.pNext = &h265;
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

	/* Writes the profile structure. */
	vulkan_writer_init(&writer);
	video_encode_struct(&writer, (const VkBaseInStructure *)&profile);

	/* Takes the bytes out of the writer. */
	length = encode_take(&writer, out, capacity);

	/* Reports the length of the encoded profile. */
	return length;
}

/* Copies a writer's bytes out (0 when it failed or does not fit) and frees it. */
static size_t
encode_take(
	struct vulkan_writer *writer,
	uint8_t *out,
	size_t capacity)
{
	size_t bytes;

	/* Takes the bytes only when the writer succeeded and they fit. */
	bytes = 0U;
	if (writer->error == VK_SUCCESS && writer->bytes <= capacity) {
		memcpy(out, writer->data, writer->bytes);
		bytes = writer->bytes;
	}

	/* Frees the writer's buffer. */
	vulkan_writer_finish(writer);

	/* Reports how many bytes were taken. */
	return bytes;
}
