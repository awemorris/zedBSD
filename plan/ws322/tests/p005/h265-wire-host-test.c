/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of libvulkan's H.265 decode wire (ws322-p005): libvulkan's own
 * video encoders (userland/desktop/libvulkan/video.c, included so its
 * static encoders can be called) write H.265 profiles, parameters creates
 * and updates (VPS, SPS and PPS with every pointer filled, and with every
 * pointer NULL), decodes with the H.265 picture and DPB slot records; a
 * reader written here from the wire rules (the comment at the top of
 * video.c) reads every field back and compares it with the source, then
 * requires the whole run to be consumed.  The structures are filled with
 * pseudo-random bytes, so every flag bit and every field width is used;
 * a flag word is checked against the raw bits of its bit-field structure,
 * not against a second packing routine.  The H.265 capabilities reply,
 * the parameter set keys and the profile check are checked too.
 *
 *   h265-wire-host-test
 */

#include "userland/desktop/libvulkan/video.c"

#include <stdio.h>
#include <stdlib.h>

/* A reader over the bytes one encoder wrote. */
struct test_reader {
	const uint8_t *bytes;
	size_t length;
	size_t at;
	int broken;
};

static int passes;
static int failures;
static uint32_t random_state = 0x2545f491U;

/* Counts one check. */
static void
check(
	int ok,
	const char *what)
{
	if (ok) {
		passes++;
		return;
	}
	failures++;
	printf("FAIL %s\n", what);
}

/* The context and object calls the encoders reach. */
struct vulkan_object *
vulkan_nondispatchable_object(
	uint64_t handle)
{
	return (struct vulkan_object *)(uintptr_t)handle;
}

uint64_t
vulkan_object_wire_id(
	const struct vulkan_object *object)
{
	return (uint64_t)(uintptr_t)object;
}

void
vulkan_context_error(
	struct vulkan_context *context,
	VkResult error)
{
	(void)context;
	(void)error;
}

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
	abort();
}

/* A small deterministic generator (xorshift32). */
static uint32_t
random_word(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Fills storage with pseudo-random bytes. */
static void
random_fill(
	void *storage,
	size_t bytes)
{
	uint8_t *out;
	size_t index;

	out = storage;
	for (index = 0; index < bytes; index++)
		out[index] = (uint8_t)random_word();
}

/* The raw 32 bits of a bit-field flags structure, the first declared flag in bit 0. */
static uint32_t
raw_word(
	const void *flags)
{
	uint32_t word;

	memcpy(&word, flags, sizeof(word));
	return word;
}

/* The low n bits. */
static uint32_t
low_bits(
	uint32_t word,
	unsigned n)
{
	if (n >= 32U)
		return word;
	return word & ((1U << n) - 1U);
}

static uint32_t
read_u32(
	struct test_reader *reader)
{
	uint32_t word;

	if (reader->broken || reader->length - reader->at < 4U) {
		reader->broken = 1;
		return 0;
	}
	word = (uint32_t)reader->bytes[reader->at] |
	    (uint32_t)reader->bytes[reader->at + 1U] << 8 |
	    (uint32_t)reader->bytes[reader->at + 2U] << 16 |
	    (uint32_t)reader->bytes[reader->at + 3U] << 24;
	reader->at += 4U;
	return word;
}

static uint64_t
read_u64(
	struct test_reader *reader)
{
	uint64_t low;
	uint64_t high;

	low = read_u32(reader);
	high = read_u32(reader);
	return low | high << 32;
}

/* Expects one 32-bit word. */
static void
expect_u32(
	struct test_reader *reader,
	uint32_t value,
	const char *what)
{
	uint32_t word;

	word = read_u32(reader);
	if (reader->broken) {
		check(0, what);
		return;
	}
	if (word != value)
		printf("  %s: wire 0x%x, source 0x%x\n", what, word, value);
	check(word == value, what);
}

/* Expects one 64-bit word. */
static void
expect_u64(
	struct test_reader *reader,
	uint64_t value,
	const char *what)
{
	uint64_t word;

	word = read_u64(reader);
	check(!reader->broken && word == value, what);
}

/* Expects a byte array: its count, then the bytes padded to four. */
static void
expect_bytes(
	struct test_reader *reader,
	const void *bytes,
	size_t count,
	const char *what)
{
	size_t padded;

	expect_u64(reader, count, what);
	padded = (count + 3U) & ~(size_t)3U;
	if (reader->broken || reader->length - reader->at < padded) {
		reader->broken = 1;
		check(0, what);
		return;
	}
	check(memcmp(reader->bytes + reader->at, bytes, count) == 0, what);
	reader->at += padded;
}

/* Expects an array of sixteen-bit numbers, each as a word. */
static void
expect_wide(
	struct test_reader *reader,
	const uint16_t *elements,
	uint32_t count,
	const char *what)
{
	uint32_t index;

	expect_u64(reader, count, what);
	for (index = 0; index < count; index++)
		expect_u32(reader, elements[index], what);
}

/* Expects an array of 32-bit numbers. */
static void
expect_words(
	struct test_reader *reader,
	const uint32_t *elements,
	uint32_t count,
	const char *what)
{
	uint32_t index;

	expect_u64(reader, count, what);
	for (index = 0; index < count; index++)
		expect_u32(reader, elements[index], what);
}

/* Expects a pointer's presence word; reports whether the pointer is present. */
static int
expect_pointer(
	struct test_reader *reader,
	const void *pointer,
	const char *what)
{
	expect_u64(reader, pointer != NULL ? 1U : 0U, what);
	return pointer != NULL;
}

/* Expects an absent chain link. */
static void
expect_chain_end(
	struct test_reader *reader,
	const char *what)
{
	expect_u64(reader, 0, what);
}

static void
read_ptl(
	struct test_reader *reader,
	const StdVideoH265ProfileTierLevel *ptl)
{
	expect_u32(reader, low_bits(raw_word(&ptl->flags), 5), "ptl flags");
	expect_u32(reader, (uint32_t)ptl->general_profile_idc, "ptl profile");
	expect_u32(reader, (uint32_t)ptl->general_level_idc, "ptl level");
}

static void
read_dpb(
	struct test_reader *reader,
	const StdVideoH265DecPicBufMgr *dpb)
{
	expect_words(reader, dpb->max_latency_increase_plus1, 7, "dpb latency");
	expect_bytes(reader, dpb->max_dec_pic_buffering_minus1, 7, "dpb buffering");
	expect_bytes(reader, dpb->max_num_reorder_pics, 7, "dpb reorder");
}

static void
read_sub_layers(
	struct test_reader *reader,
	const StdVideoH265SubLayerHrdParameters *layers,
	uint32_t count)
{
	uint32_t index;

	expect_u64(reader, count, "sub-layer count");
	for (index = 0; index < count; index++) {
		expect_words(reader, layers[index].bit_rate_value_minus1, 32, "sub-layer bit rate");
		expect_words(reader, layers[index].cpb_size_value_minus1, 32, "sub-layer cpb size");
		expect_words(reader, layers[index].cpb_size_du_value_minus1, 32, "sub-layer cpb du size");
		expect_words(reader, layers[index].bit_rate_du_value_minus1, 32, "sub-layer du bit rate");
		expect_u32(reader, layers[index].cbr_flag, "sub-layer cbr");
	}
}

static void
read_hrd(
	struct test_reader *reader,
	const StdVideoH265HrdParameters *hrd,
	uint32_t sub_layers)
{
	uint32_t raw;

	raw = raw_word(&hrd->flags);
	expect_u32(reader, low_bits(raw, 4), "hrd flags");
	expect_u32(reader, (raw >> 4) & 0xffU, "hrd fixed_pic_rate_general");
	expect_u32(reader, (raw >> 12) & 0xffU, "hrd fixed_pic_rate_within_cvs");
	expect_u32(reader, (raw >> 20) & 0xffU, "hrd low_delay");
	expect_u32(reader, hrd->tick_divisor_minus2, "hrd tick divisor");
	expect_u32(reader, hrd->du_cpb_removal_delay_increment_length_minus1, "hrd du removal length");
	expect_u32(reader, hrd->dpb_output_delay_du_length_minus1, "hrd du output length");
	expect_u32(reader, hrd->bit_rate_scale, "hrd bit rate scale");
	expect_u32(reader, hrd->cpb_size_scale, "hrd cpb scale");
	expect_u32(reader, hrd->cpb_size_du_scale, "hrd cpb du scale");
	expect_u32(reader, hrd->initial_cpb_removal_delay_length_minus1, "hrd initial removal length");
	expect_u32(reader, hrd->au_cpb_removal_delay_length_minus1, "hrd au removal length");
	expect_u32(reader, hrd->dpb_output_delay_length_minus1, "hrd output length");
	expect_bytes(reader, hrd->cpb_cnt_minus1, 7, "hrd cpb counts");
	expect_wide(reader, hrd->elemental_duration_in_tc_minus1, 7, "hrd durations");
	expect_wide(reader, hrd->reserved, 3, "hrd reserved");
	if (expect_pointer(reader, hrd->pSubLayerHrdParametersNal, "hrd nal"))
		read_sub_layers(reader, hrd->pSubLayerHrdParametersNal, sub_layers);
	if (expect_pointer(reader, hrd->pSubLayerHrdParametersVcl, "hrd vcl"))
		read_sub_layers(reader, hrd->pSubLayerHrdParametersVcl, sub_layers);
}

static void
read_scaling(
	struct test_reader *reader,
	const StdVideoH265ScalingLists *lists)
{
	expect_bytes(reader, lists->ScalingList4x4, 96, "scaling 4x4");
	expect_bytes(reader, lists->ScalingList8x8, 384, "scaling 8x8");
	expect_bytes(reader, lists->ScalingList16x16, 384, "scaling 16x16");
	expect_bytes(reader, lists->ScalingList32x32, 128, "scaling 32x32");
	expect_bytes(reader, lists->ScalingListDCCoef16x16, 6, "scaling dc 16x16");
	expect_bytes(reader, lists->ScalingListDCCoef32x32, 2, "scaling dc 32x32");
}

static void
read_palette(
	struct test_reader *reader,
	const StdVideoH265PredictorPaletteEntries *palette)
{
	expect_wide(reader, &palette->PredictorPaletteEntries[0][0], 384, "palette");
}

static void
read_vps(
	struct test_reader *reader,
	const StdVideoH265VideoParameterSet *vps)
{
	expect_u32(reader, low_bits(raw_word(&vps->flags), 4), "vps flags");
	expect_u32(reader, vps->vps_video_parameter_set_id, "vps id");
	expect_u32(reader, vps->vps_max_sub_layers_minus1, "vps sub-layers");
	expect_u32(reader, vps->reserved1, "vps reserved1");
	expect_u32(reader, vps->reserved2, "vps reserved2");
	expect_u32(reader, vps->vps_num_units_in_tick, "vps units");
	expect_u32(reader, vps->vps_time_scale, "vps scale");
	expect_u32(reader, vps->vps_num_ticks_poc_diff_one_minus1, "vps ticks");
	expect_u32(reader, vps->reserved3, "vps reserved3");
	if (expect_pointer(reader, vps->pDecPicBufMgr, "vps dpb"))
		read_dpb(reader, vps->pDecPicBufMgr);
	if (expect_pointer(reader, vps->pHrdParameters, "vps hrd"))
		read_hrd(reader, vps->pHrdParameters, (uint32_t)vps->vps_max_sub_layers_minus1 + 1U);
	if (expect_pointer(reader, vps->pProfileTierLevel, "vps ptl"))
		read_ptl(reader, vps->pProfileTierLevel);
}

static void
read_vui(
	struct test_reader *reader,
	const StdVideoH265SequenceParameterSetVui *vui,
	uint32_t sub_layers)
{
	expect_u32(reader, low_bits(raw_word(&vui->flags), 18), "vui flags");
	expect_u32(reader, (uint32_t)vui->aspect_ratio_idc, "vui aspect");
	expect_u32(reader, vui->sar_width, "vui sar width");
	expect_u32(reader, vui->sar_height, "vui sar height");
	expect_u32(reader, vui->video_format, "vui format");
	expect_u32(reader, vui->colour_primaries, "vui primaries");
	expect_u32(reader, vui->transfer_characteristics, "vui transfer");
	expect_u32(reader, vui->matrix_coeffs, "vui matrix");
	expect_u32(reader, vui->chroma_sample_loc_type_top_field, "vui chroma top");
	expect_u32(reader, vui->chroma_sample_loc_type_bottom_field, "vui chroma bottom");
	expect_u32(reader, vui->reserved1, "vui reserved1");
	expect_u32(reader, vui->reserved2, "vui reserved2");
	expect_u32(reader, vui->def_disp_win_left_offset, "vui window left");
	expect_u32(reader, vui->def_disp_win_right_offset, "vui window right");
	expect_u32(reader, vui->def_disp_win_top_offset, "vui window top");
	expect_u32(reader, vui->def_disp_win_bottom_offset, "vui window bottom");
	expect_u32(reader, vui->vui_num_units_in_tick, "vui units");
	expect_u32(reader, vui->vui_time_scale, "vui scale");
	expect_u32(reader, vui->vui_num_ticks_poc_diff_one_minus1, "vui ticks");
	expect_u32(reader, vui->min_spatial_segmentation_idc, "vui segmentation");
	expect_u32(reader, vui->reserved3, "vui reserved3");
	expect_u32(reader, vui->max_bytes_per_pic_denom, "vui bytes per pic");
	expect_u32(reader, vui->max_bits_per_min_cu_denom, "vui bits per cu");
	expect_u32(reader, vui->log2_max_mv_length_horizontal, "vui mv horizontal");
	expect_u32(reader, vui->log2_max_mv_length_vertical, "vui mv vertical");
	if (expect_pointer(reader, vui->pHrdParameters, "vui hrd"))
		read_hrd(reader, vui->pHrdParameters, sub_layers);
}

static void
read_short_term(
	struct test_reader *reader,
	const StdVideoH265ShortTermRefPicSet *set)
{
	expect_u32(reader, low_bits(raw_word(&set->flags), 2), "st flags");
	expect_u32(reader, set->delta_idx_minus1, "st delta idx");
	expect_u32(reader, set->use_delta_flag, "st use delta");
	expect_u32(reader, set->abs_delta_rps_minus1, "st abs delta");
	expect_u32(reader, set->used_by_curr_pic_flag, "st used");
	expect_u32(reader, set->used_by_curr_pic_s0_flag, "st used s0");
	expect_u32(reader, set->used_by_curr_pic_s1_flag, "st used s1");
	expect_u32(reader, set->reserved1, "st reserved1");
	expect_u32(reader, set->reserved2, "st reserved2");
	expect_u32(reader, set->reserved3, "st reserved3");
	expect_u32(reader, set->num_negative_pics, "st negative");
	expect_u32(reader, set->num_positive_pics, "st positive");
	expect_wide(reader, set->delta_poc_s0_minus1, 16, "st delta s0");
	expect_wide(reader, set->delta_poc_s1_minus1, 16, "st delta s1");
}

static void
read_sps(
	struct test_reader *reader,
	const StdVideoH265SequenceParameterSet *sps)
{
	uint32_t index;

	expect_u32(reader, low_bits(raw_word(&sps->flags), 30), "sps flags");
	expect_u32(reader, (uint32_t)sps->chroma_format_idc, "sps chroma");
	expect_u32(reader, sps->pic_width_in_luma_samples, "sps width");
	expect_u32(reader, sps->pic_height_in_luma_samples, "sps height");
	expect_u32(reader, sps->sps_video_parameter_set_id, "sps vps id");
	expect_u32(reader, sps->sps_max_sub_layers_minus1, "sps sub-layers");
	expect_u32(reader, sps->sps_seq_parameter_set_id, "sps id");
	expect_u32(reader, sps->bit_depth_luma_minus8, "sps luma depth");
	expect_u32(reader, sps->bit_depth_chroma_minus8, "sps chroma depth");
	expect_u32(reader, sps->log2_max_pic_order_cnt_lsb_minus4, "sps poc lsb");
	expect_u32(reader, sps->log2_min_luma_coding_block_size_minus3, "sps min cb");
	expect_u32(reader, sps->log2_diff_max_min_luma_coding_block_size, "sps diff cb");
	expect_u32(reader, sps->log2_min_luma_transform_block_size_minus2, "sps min tb");
	expect_u32(reader, sps->log2_diff_max_min_luma_transform_block_size, "sps diff tb");
	expect_u32(reader, sps->max_transform_hierarchy_depth_inter, "sps depth inter");
	expect_u32(reader, sps->max_transform_hierarchy_depth_intra, "sps depth intra");
	expect_u32(reader, sps->num_short_term_ref_pic_sets, "sps st count");
	expect_u32(reader, sps->num_long_term_ref_pics_sps, "sps lt count");
	expect_u32(reader, sps->pcm_sample_bit_depth_luma_minus1, "sps pcm luma");
	expect_u32(reader, sps->pcm_sample_bit_depth_chroma_minus1, "sps pcm chroma");
	expect_u32(reader, sps->log2_min_pcm_luma_coding_block_size_minus3, "sps pcm min");
	expect_u32(reader, sps->log2_diff_max_min_pcm_luma_coding_block_size, "sps pcm diff");
	expect_u32(reader, sps->reserved1, "sps reserved1");
	expect_u32(reader, sps->reserved2, "sps reserved2");
	expect_u32(reader, sps->palette_max_size, "sps palette size");
	expect_u32(reader, sps->delta_palette_max_predictor_size, "sps palette predictor");
	expect_u32(reader, sps->motion_vector_resolution_control_idc, "sps mv resolution");
	expect_u32(reader, sps->sps_num_palette_predictor_initializers_minus1, "sps palette initializers");
	expect_u32(reader, sps->conf_win_left_offset, "sps window left");
	expect_u32(reader, sps->conf_win_right_offset, "sps window right");
	expect_u32(reader, sps->conf_win_top_offset, "sps window top");
	expect_u32(reader, sps->conf_win_bottom_offset, "sps window bottom");
	if (expect_pointer(reader, sps->pProfileTierLevel, "sps ptl"))
		read_ptl(reader, sps->pProfileTierLevel);
	if (expect_pointer(reader, sps->pDecPicBufMgr, "sps dpb"))
		read_dpb(reader, sps->pDecPicBufMgr);
	if (expect_pointer(reader, sps->pScalingLists, "sps scaling"))
		read_scaling(reader, sps->pScalingLists);
	if (expect_pointer(reader, sps->pShortTermRefPicSet, "sps st")) {
		expect_u64(reader, sps->num_short_term_ref_pic_sets, "sps st array");
		for (index = 0; index < sps->num_short_term_ref_pic_sets; index++)
			read_short_term(reader, &sps->pShortTermRefPicSet[index]);
	}
	if (expect_pointer(reader, sps->pLongTermRefPicsSps, "sps lt")) {
		expect_u32(reader, sps->pLongTermRefPicsSps->used_by_curr_pic_lt_sps_flag, "lt used");
		expect_words(reader, sps->pLongTermRefPicsSps->lt_ref_pic_poc_lsb_sps, 32, "lt lsb");
	}
	if (expect_pointer(reader, sps->pSequenceParameterSetVui, "sps vui"))
		read_vui(reader, sps->pSequenceParameterSetVui, (uint32_t)sps->sps_max_sub_layers_minus1 + 1U);
	if (expect_pointer(reader, sps->pPredictorPaletteEntries, "sps palette"))
		read_palette(reader, sps->pPredictorPaletteEntries);
}

static void
read_pps(
	struct test_reader *reader,
	const StdVideoH265PictureParameterSet *pps)
{
	expect_u32(reader, low_bits(raw_word(&pps->flags), 31), "pps flags");
	expect_u32(reader, pps->pps_pic_parameter_set_id, "pps id");
	expect_u32(reader, pps->pps_seq_parameter_set_id, "pps sps id");
	expect_u32(reader, pps->sps_video_parameter_set_id, "pps vps id");
	expect_u32(reader, pps->num_extra_slice_header_bits, "pps extra bits");
	expect_u32(reader, pps->num_ref_idx_l0_default_active_minus1, "pps l0");
	expect_u32(reader, pps->num_ref_idx_l1_default_active_minus1, "pps l1");
	expect_u32(reader, (uint32_t)(int32_t)pps->init_qp_minus26, "pps init qp");
	expect_u32(reader, pps->diff_cu_qp_delta_depth, "pps qp depth");
	expect_u32(reader, (uint32_t)(int32_t)pps->pps_cb_qp_offset, "pps cb offset");
	expect_u32(reader, (uint32_t)(int32_t)pps->pps_cr_qp_offset, "pps cr offset");
	expect_u32(reader, (uint32_t)(int32_t)pps->pps_beta_offset_div2, "pps beta");
	expect_u32(reader, (uint32_t)(int32_t)pps->pps_tc_offset_div2, "pps tc");
	expect_u32(reader, pps->log2_parallel_merge_level_minus2, "pps merge level");
	expect_u32(reader, pps->log2_max_transform_skip_block_size_minus2, "pps skip size");
	expect_u32(reader, pps->diff_cu_chroma_qp_offset_depth, "pps chroma depth");
	expect_u32(reader, pps->chroma_qp_offset_list_len_minus1, "pps chroma list length");
	expect_bytes(reader, pps->cb_qp_offset_list, 6, "pps cb list");
	expect_bytes(reader, pps->cr_qp_offset_list, 6, "pps cr list");
	expect_u32(reader, pps->log2_sao_offset_scale_luma, "pps sao luma");
	expect_u32(reader, pps->log2_sao_offset_scale_chroma, "pps sao chroma");
	expect_u32(reader, (uint32_t)(int32_t)pps->pps_act_y_qp_offset_plus5, "pps act y");
	expect_u32(reader, (uint32_t)(int32_t)pps->pps_act_cb_qp_offset_plus5, "pps act cb");
	expect_u32(reader, (uint32_t)(int32_t)pps->pps_act_cr_qp_offset_plus3, "pps act cr");
	expect_u32(reader, pps->pps_num_palette_predictor_initializers, "pps palette initializers");
	expect_u32(reader, pps->luma_bit_depth_entry_minus8, "pps luma entry");
	expect_u32(reader, pps->chroma_bit_depth_entry_minus8, "pps chroma entry");
	expect_u32(reader, pps->num_tile_columns_minus1, "pps tile columns");
	expect_u32(reader, pps->num_tile_rows_minus1, "pps tile rows");
	expect_u32(reader, pps->reserved1, "pps reserved1");
	expect_u32(reader, pps->reserved2, "pps reserved2");
	expect_wide(reader, pps->column_width_minus1, 19, "pps column widths");
	expect_wide(reader, pps->row_height_minus1, 21, "pps row heights");
	expect_u32(reader, pps->reserved3, "pps reserved3");
	if (expect_pointer(reader, pps->pScalingLists, "pps scaling"))
		read_scaling(reader, pps->pScalingLists);
	if (expect_pointer(reader, pps->pPredictorPaletteEntries, "pps palette"))
		read_palette(reader, pps->pPredictorPaletteEntries);
}

static void
read_add(
	struct test_reader *reader,
	const VkVideoDecodeH265SessionParametersAddInfoKHR *add)
{
	uint32_t index;

	expect_u32(reader, VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR, "add type");
	expect_chain_end(reader, "add chain");
	expect_u32(reader, add->stdVPSCount, "add vps count");
	expect_u64(reader, add->stdVPSCount, "add vps array");
	for (index = 0; index < add->stdVPSCount; index++)
		read_vps(reader, &add->pStdVPSs[index]);
	expect_u32(reader, add->stdSPSCount, "add sps count");
	expect_u64(reader, add->stdSPSCount, "add sps array");
	for (index = 0; index < add->stdSPSCount; index++)
		read_sps(reader, &add->pStdSPSs[index]);
	expect_u32(reader, add->stdPPSCount, "add pps count");
	expect_u64(reader, add->stdPPSCount, "add pps array");
	for (index = 0; index < add->stdPPSCount; index++)
		read_pps(reader, &add->pStdPPSs[index]);
}

/* Takes a writer's bytes into a reader; the writer must have succeeded. */
static void
reader_take(
	struct test_reader *reader,
	struct vulkan_writer *writer,
	const char *what)
{
	check(writer->error == VK_SUCCESS, what);
	memset(reader, 0, sizeof(*reader));
	reader->bytes = writer->data;
	reader->length = writer->bytes;
}

/* Requires the reader to have consumed the run exactly. */
static void
reader_done(
	struct test_reader *reader,
	const char *what)
{
	check(!reader->broken && reader->at == reader->length, what);
}

/* The storage of one fully filled parameter set group. */
struct full_sets {
	StdVideoH265ProfileTierLevel ptl;
	StdVideoH265DecPicBufMgr dpb;
	StdVideoH265HrdParameters hrd;
	StdVideoH265SubLayerHrdParameters nal[3];
	StdVideoH265SubLayerHrdParameters vcl[3];
	StdVideoH265ScalingLists scaling;
	StdVideoH265ShortTermRefPicSet st[3];
	StdVideoH265LongTermRefPicsSps lt;
	StdVideoH265SequenceParameterSetVui vui;
	StdVideoH265PredictorPaletteEntries palette;
	StdVideoH265VideoParameterSet vps[2];
	StdVideoH265SequenceParameterSet sps[2];
	StdVideoH265PictureParameterSet pps[2];
};

/* Fills two of each set: the first with every pointer, the second with none. */
static void
full_sets_make(
	struct full_sets *sets)
{
	random_fill(sets, sizeof(*sets));

	sets->hrd.pSubLayerHrdParametersNal = sets->nal;
	sets->hrd.pSubLayerHrdParametersVcl = sets->vcl;
	sets->vui.pHrdParameters = &sets->hrd;

	sets->vps[0].vps_max_sub_layers_minus1 = 2U;
	sets->vps[0].pDecPicBufMgr = &sets->dpb;
	sets->vps[0].pHrdParameters = &sets->hrd;
	sets->vps[0].pProfileTierLevel = &sets->ptl;
	sets->vps[1].pDecPicBufMgr = NULL;
	sets->vps[1].pHrdParameters = NULL;
	sets->vps[1].pProfileTierLevel = NULL;

	sets->sps[0].sps_max_sub_layers_minus1 = 2U;
	sets->sps[0].num_short_term_ref_pic_sets = 3U;
	sets->sps[0].pProfileTierLevel = &sets->ptl;
	sets->sps[0].pDecPicBufMgr = &sets->dpb;
	sets->sps[0].pScalingLists = &sets->scaling;
	sets->sps[0].pShortTermRefPicSet = sets->st;
	sets->sps[0].pLongTermRefPicsSps = &sets->lt;
	sets->sps[0].pSequenceParameterSetVui = &sets->vui;
	sets->sps[0].pPredictorPaletteEntries = &sets->palette;
	sets->sps[1].pProfileTierLevel = NULL;
	sets->sps[1].pDecPicBufMgr = NULL;
	sets->sps[1].pScalingLists = NULL;
	sets->sps[1].pShortTermRefPicSet = NULL;
	sets->sps[1].pLongTermRefPicsSps = NULL;
	sets->sps[1].pSequenceParameterSetVui = NULL;
	sets->sps[1].pPredictorPaletteEntries = NULL;

	sets->pps[0].pScalingLists = &sets->scaling;
	sets->pps[0].pPredictorPaletteEntries = &sets->palette;
	sets->pps[1].pScalingLists = NULL;
	sets->pps[1].pPredictorPaletteEntries = NULL;
}

/* A parameters create with an H.265 create record and its add record. */
static void
test_parameters_create(void)
{
	struct full_sets *sets;
	VkVideoDecodeH265SessionParametersAddInfoKHR add;
	VkVideoDecodeH265SessionParametersCreateInfoKHR h265;
	VkBaseInStructure skipped;
	VkVideoSessionParametersCreateInfoKHR create;
	struct vulkan_writer writer;
	struct test_reader reader;
	int round;

	sets = malloc(sizeof(*sets));
	for (round = 0; round < 20; round++) {
		full_sets_make(sets);

		memset(&add, 0, sizeof(add));
		add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR;
		add.stdVPSCount = 2U;
		add.pStdVPSs = sets->vps;
		add.stdSPSCount = 2U;
		add.pStdSPSs = sets->sps;
		add.stdPPSCount = 2U;
		add.pStdPPSs = sets->pps;
		memset(&h265, 0, sizeof(h265));
		h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR;
		h265.maxStdVPSCount = 3U + (uint32_t)round;
		h265.maxStdSPSCount = 5U;
		h265.maxStdPPSCount = 9U;
		h265.pParametersAddInfo = &add;

		/* An unknown record before the H.265 one is skipped on the wire. */
		memset(&skipped, 0, sizeof(skipped));
		skipped.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		skipped.pNext = (const VkBaseInStructure *)&h265;
		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
		create.pNext = &skipped;
		create.flags = 0;
		create.videoSessionParametersTemplate = (VkVideoSessionParametersKHR)(uintptr_t)0x81U;
		create.videoSession = (VkVideoSessionKHR)(uintptr_t)0x70U;

		vulkan_writer_init(&writer);
		video_encode_struct(&writer, (const VkBaseInStructure *)&create);
		reader_take(&reader, &writer, "create encoded");

		expect_u32(&reader, VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR, "create type");
		expect_u64(&reader, 1, "create chain");
		expect_u32(&reader, VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR, "h265 create type");
		expect_chain_end(&reader, "h265 create chain");
		expect_u32(&reader, h265.maxStdVPSCount, "max vps");
		expect_u32(&reader, h265.maxStdSPSCount, "max sps");
		expect_u32(&reader, h265.maxStdPPSCount, "max pps");
		expect_u64(&reader, 1, "add pointer");
		read_add(&reader, &add);
		expect_u32(&reader, 0, "create flags");
		expect_u64(&reader, 0x81U, "create template");
		expect_u64(&reader, 0x70U, "create session");
		reader_done(&reader, "create consumed");
		vulkan_writer_finish(&writer);
	}
	free(sets);
}

/* An update carrying an H.265 add record. */
static void
test_parameters_update(void)
{
	struct full_sets *sets;
	VkVideoDecodeH265SessionParametersAddInfoKHR add;
	VkVideoSessionParametersUpdateInfoKHR update;
	struct vulkan_writer writer;
	struct test_reader reader;

	sets = malloc(sizeof(*sets));
	full_sets_make(sets);
	memset(&add, 0, sizeof(add));
	add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR;
	add.stdSPSCount = 1U;
	add.pStdSPSs = &sets->sps[0];
	add.stdPPSCount = 1U;
	add.pStdPPSs = &sets->pps[1];
	memset(&update, 0, sizeof(update));
	update.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR;
	update.pNext = &add;
	update.updateSequenceCount = 7U;

	vulkan_writer_init(&writer);
	video_encode_struct(&writer, (const VkBaseInStructure *)&update);
	reader_take(&reader, &writer, "update encoded");
	expect_u32(&reader, VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR, "update type");
	expect_u64(&reader, 1, "update chain");
	read_add(&reader, &add);
	expect_u32(&reader, 7U, "update sequence");
	reader_done(&reader, "update consumed");
	vulkan_writer_finish(&writer);
	free(sets);
}

static void
read_resource(
	struct test_reader *reader,
	const VkVideoPictureResourceInfoKHR *resource)
{
	expect_u32(reader, VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR, "resource type");
	expect_chain_end(reader, "resource chain");
	expect_u32(reader, (uint32_t)resource->codedOffset.x, "resource x");
	expect_u32(reader, (uint32_t)resource->codedOffset.y, "resource y");
	expect_u32(reader, resource->codedExtent.width, "resource width");
	expect_u32(reader, resource->codedExtent.height, "resource height");
	expect_u32(reader, resource->baseArrayLayer, "resource layer");
	expect_u64(reader, (uint64_t)(uintptr_t)resource->imageViewBinding, "resource view");
}

static void
read_slot(
	struct test_reader *reader,
	const VkVideoReferenceSlotInfoKHR *slot,
	const StdVideoDecodeH265ReferenceInfo *reference)
{
	expect_u32(reader, VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, "slot type");
	if (reference != NULL) {
		expect_u64(reader, 1, "slot chain");
		expect_u32(reader, VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR, "dpb type");
		expect_chain_end(reader, "dpb chain");
		expect_u64(reader, 1, "dpb reference pointer");
		expect_u32(reader, low_bits(raw_word(&reference->flags), 2), "reference flags");
		expect_u32(reader, (uint32_t)reference->PicOrderCntVal, "reference poc");
	} else {
		expect_chain_end(reader, "slot chain");
	}
	expect_u32(reader, (uint32_t)slot->slotIndex, "slot index");
	if (expect_pointer(reader, slot->pPictureResource, "slot resource"))
		read_resource(reader, slot->pPictureResource);
}

/* A decode with the H.265 picture record, a setup slot and two references. */
static void
test_decode(void)
{
	StdVideoDecodeH265PictureInfo std_picture;
	VkVideoDecodeH265PictureInfoKHR picture;
	StdVideoDecodeH265ReferenceInfo references[3];
	VkVideoDecodeH265DpbSlotInfoKHR dpb[3];
	VkVideoPictureResourceInfoKHR resources[3];
	VkVideoReferenceSlotInfoKHR slots[2];
	VkVideoReferenceSlotInfoKHR setup;
	VkVideoDecodeInfoKHR decode;
	struct vulkan_writer writer;
	struct test_reader reader;
	uint32_t offsets[4];
	uint32_t index;
	int round;

	for (round = 0; round < 50; round++) {
		random_fill(&std_picture, sizeof(std_picture));
		random_fill(references, sizeof(references));
		random_fill(offsets, sizeof(offsets));
		memset(&picture, 0, sizeof(picture));
		picture.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR;
		picture.pStdPictureInfo = &std_picture;
		picture.sliceSegmentCount = (uint32_t)round % 5U;
		picture.pSliceSegmentOffsets = offsets;
		for (index = 0; index < 3U; index++) {
			memset(&resources[index], 0, sizeof(resources[index]));
			resources[index].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
			resources[index].codedOffset.x = (int32_t)index;
			resources[index].codedExtent.width = 1920U;
			resources[index].codedExtent.height = 1080U;
			resources[index].imageViewBinding = (VkImageView)(uintptr_t)(0x50U + index);
			memset(&dpb[index], 0, sizeof(dpb[index]));
			dpb[index].sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
			dpb[index].pStdReferenceInfo = &references[index];
		}
		for (index = 0; index < 2U; index++) {
			memset(&slots[index], 0, sizeof(slots[index]));
			slots[index].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
			slots[index].pNext = &dpb[index + 1U];
			slots[index].slotIndex = (int32_t)index + 4;
			slots[index].pPictureResource = &resources[index + 1U];
		}
		memset(&setup, 0, sizeof(setup));
		setup.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		setup.pNext = &dpb[0];
		setup.slotIndex = 2;
		setup.pPictureResource = &resources[0];
		memset(&decode, 0, sizeof(decode));
		decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
		decode.pNext = &picture;
		decode.srcBuffer = (VkBuffer)(uintptr_t)0x40U;
		decode.srcBufferOffset = 512U;
		decode.srcBufferRange = 65536U + (uint64_t)round;
		decode.dstPictureResource = resources[0];
		decode.pSetupReferenceSlot = &setup;
		decode.referenceSlotCount = 2U;
		decode.pReferenceSlots = slots;

		vulkan_writer_init(&writer);
		video_encode_struct(&writer, (const VkBaseInStructure *)&decode);
		reader_take(&reader, &writer, "decode encoded");
		expect_u32(&reader, VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR, "decode type");
		expect_u64(&reader, 1, "decode chain");
		expect_u32(&reader, VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR, "picture type");
		expect_chain_end(&reader, "picture chain");
		expect_u64(&reader, 1, "std picture pointer");
		expect_u32(&reader, low_bits(raw_word(&std_picture.flags), 4), "picture flags");
		expect_u32(&reader, std_picture.sps_video_parameter_set_id, "picture vps");
		expect_u32(&reader, std_picture.pps_seq_parameter_set_id, "picture sps");
		expect_u32(&reader, std_picture.pps_pic_parameter_set_id, "picture pps");
		expect_u32(&reader, std_picture.NumDeltaPocsOfRefRpsIdx, "picture delta pocs");
		expect_u32(&reader, (uint32_t)std_picture.PicOrderCntVal, "picture poc");
		expect_u32(&reader, std_picture.NumBitsForSTRefPicSetInSlice, "picture st bits");
		expect_u32(&reader, std_picture.reserved, "picture reserved");
		expect_bytes(&reader, std_picture.RefPicSetStCurrBefore, 8, "picture before");
		expect_bytes(&reader, std_picture.RefPicSetStCurrAfter, 8, "picture after");
		expect_bytes(&reader, std_picture.RefPicSetLtCurr, 8, "picture long-term");
		expect_u32(&reader, picture.sliceSegmentCount, "slice count");
		expect_words(&reader, offsets, picture.sliceSegmentCount, "slice offsets");
		expect_u32(&reader, 0, "decode flags");
		expect_u64(&reader, 0x40U, "decode buffer");
		expect_u64(&reader, 512U, "decode offset");
		expect_u64(&reader, decode.srcBufferRange, "decode range");
		read_resource(&reader, &resources[0]);
		expect_u64(&reader, 1, "setup pointer");
		read_slot(&reader, &setup, &references[0]);
		expect_u32(&reader, 2U, "reference count");
		expect_u64(&reader, 2U, "reference array");
		read_slot(&reader, &slots[0], &references[1]);
		read_slot(&reader, &slots[1], &references[2]);
		reader_done(&reader, "decode consumed");
		vulkan_writer_finish(&writer);
	}
}

/* A profile with its H.265 record, and the capability output request and reply. */
static void
test_profile_and_capabilities(void)
{
	VkVideoDecodeH265ProfileInfoKHR h265;
	VkVideoProfileInfoKHR profile;
	VkVideoDecodeH265CapabilitiesKHR h265_caps;
	VkVideoDecodeCapabilitiesKHR decode_caps;
	VkVideoCapabilitiesKHR caps;
	struct vulkan_writer writer;
	struct test_reader reader;
	struct vulkan_reader reply;
	uint8_t *copy;

	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
	h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
	memset(&profile, 0, sizeof(profile));
	profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	profile.pNext = &h265;
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	vulkan_writer_init(&writer);
	video_encode_struct(&writer, (const VkBaseInStructure *)&profile);
	reader_take(&reader, &writer, "profile encoded");
	expect_u32(&reader, VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR, "profile type");
	expect_u64(&reader, 1, "profile chain");
	expect_u32(&reader, VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR, "h265 profile type");
	expect_chain_end(&reader, "h265 profile chain");
	expect_u32(&reader, STD_VIDEO_H265_PROFILE_IDC_MAIN, "h265 profile idc");
	expect_u32(&reader, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, "profile operation");
	expect_u32(&reader, VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR, "profile chroma");
	expect_u32(&reader, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR, "profile luma depth");
	expect_u32(&reader, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR, "profile chroma depth");
	reader_done(&reader, "profile consumed");
	vulkan_writer_finish(&writer);

	/* The output request names the three capability records in the caller's order. */
	memset(&h265_caps, 0, sizeof(h265_caps));
	h265_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;
	memset(&decode_caps, 0, sizeof(decode_caps));
	decode_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
	decode_caps.pNext = &h265_caps;
	memset(&caps, 0, sizeof(caps));
	caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
	caps.pNext = &decode_caps;
	vulkan_writer_init(&writer);
	video_encode_output_request(&writer, (VkBaseOutStructure *)&caps);
	reader_take(&reader, &writer, "request encoded");
	expect_u32(&reader, VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, "request type");
	expect_u64(&reader, 1, "request link");
	expect_u32(&reader, VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR, "request decode");
	expect_u64(&reader, 1, "request link 2");
	expect_u32(&reader, VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR, "request h265");
	expect_chain_end(&reader, "request end");
	reader_done(&reader, "request consumed");
	vulkan_writer_finish(&writer);

	/* A reply in the renderer's form fills the H.265 level. */
	vulkan_writer_init(&writer);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR);
	vulkan_write_u64(&writer, 0);
	vulkan_write_u32(&writer, STD_VIDEO_H265_LEVEL_IDC_5_1);
	vulkan_write_u32(&writer, VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR);
	vulkan_write_u32(&writer, 0);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u64(&writer, 16);
	vulkan_write_u32(&writer, 16);
	vulkan_write_u32(&writer, 16);
	vulkan_write_u32(&writer, 64);
	vulkan_write_u32(&writer, 64);
	vulkan_write_u32(&writer, 4096);
	vulkan_write_u32(&writer, 2176);
	vulkan_write_u32(&writer, 16);
	vulkan_write_u32(&writer, 15);
	{
		VkExtensionProperties header;

		memset(&header, 0, sizeof(header));
		strcpy(header.extensionName, VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME);
		header.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION;
		vulkan_encode_VkExtensionProperties(&writer, &header);
	}
	check(writer.error == VK_SUCCESS, "reply encoded");
	copy = malloc(writer.bytes);
	memcpy(copy, writer.data, writer.bytes);
	vulkan_reader_init(&reply, copy, writer.bytes);
	video_decode_output(&reply, (VkBaseOutStructure *)&caps);
	check(reply.error == VK_SUCCESS && reply.cursor == reply.bytes, "reply consumed");
	check(h265_caps.maxLevelIdc == STD_VIDEO_H265_LEVEL_IDC_5_1, "reply level");
	check(h265_caps.sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR && h265_caps.pNext == NULL, "reply header kept");
	check(decode_caps.flags == VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR, "reply decode flags");
	check(caps.maxCodedExtent.width == 4096U && caps.maxCodedExtent.height == 2176U, "reply extent");
	check(caps.maxDpbSlots == 16U && caps.maxActiveReferencePictures == 15U, "reply slots");
	check(strcmp(caps.stdHeaderVersion.extensionName, VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME) == 0, "reply header name");
	vulkan_reader_finish(&reply);
	vulkan_writer_finish(&writer);
}

/* The keys of H.265 sets: the ranges, an update's refusal of a present key, a creation's replacement. */
static void
test_keys(void)
{
	struct vulkan_video_keys keys;
	StdVideoH265VideoParameterSet vps[2];
	StdVideoH265SequenceParameterSet sps[2];
	StdVideoH265PictureParameterSet pps[2];
	VkVideoDecodeH265SessionParametersAddInfoKHR add;
	StdVideoH264SequenceParameterSet h264_sps;
	StdVideoH264PictureParameterSet h264_pps;
	VkVideoDecodeH264SessionParametersAddInfoKHR h264_add;
	VkResult status;

	memset(&keys, 0, sizeof(keys));
	memset(vps, 0, sizeof(vps));
	memset(sps, 0, sizeof(sps));
	memset(pps, 0, sizeof(pps));
	memset(&add, 0, sizeof(add));
	add.pStdVPSs = vps;
	add.pStdSPSs = sps;
	add.pStdPPSs = pps;

	/* The largest ids of each kind fit. */
	vps[0].vps_video_parameter_set_id = 15U;
	sps[0].sps_video_parameter_set_id = 15U;
	sps[0].sps_seq_parameter_set_id = 15U;
	pps[0].sps_video_parameter_set_id = 15U;
	pps[0].pps_seq_parameter_set_id = 15U;
	pps[0].pps_pic_parameter_set_id = 63U;
	add.stdVPSCount = 1U;
	add.stdSPSCount = 1U;
	add.stdPPSCount = 1U;
	status = video_h265_keys_add(&keys, &add, VK_FALSE);
	check(status == VK_SUCCESS, "keys: largest ids");
	check(keys.vps[0] == 0x8000U, "keys: vps bit");
	check(keys.sps[7] == 0x80000000U, "keys: sps bit");
	check(keys.pps[511] == 0x80000000U, "keys: pps bit");

	/* The same keys again are refused by an update and taken by a creation. */
	status = video_h265_keys_add(&keys, &add, VK_FALSE);
	check(status == VK_ERROR_INITIALIZATION_FAILED, "keys: update refuses a present key");
	status = video_h265_keys_add(&keys, &add, VK_TRUE);
	check(status == VK_SUCCESS, "keys: creation replaces");

	/* A different VPS id makes a different SPS and PPS key. */
	add.stdVPSCount = 0U;
	sps[0].sps_video_parameter_set_id = 14U;
	pps[0].sps_video_parameter_set_id = 14U;
	status = video_h265_keys_add(&keys, &add, VK_FALSE);
	check(status == VK_SUCCESS, "keys: the VPS id is part of the key");

	/* Each id one past its range is refused. */
	add.stdSPSCount = 0U;
	add.stdPPSCount = 0U;
	add.stdVPSCount = 1U;
	vps[0].vps_video_parameter_set_id = 16U;
	status = video_h265_keys_add(&keys, &add, VK_TRUE);
	check(status == VK_ERROR_INITIALIZATION_FAILED, "keys: vps id 16");
	add.stdVPSCount = 0U;
	add.stdSPSCount = 1U;
	sps[0].sps_video_parameter_set_id = 0U;
	sps[0].sps_seq_parameter_set_id = 16U;
	status = video_h265_keys_add(&keys, &add, VK_TRUE);
	check(status == VK_ERROR_INITIALIZATION_FAILED, "keys: sps id 16");
	sps[0].sps_seq_parameter_set_id = 0U;
	sps[0].sps_video_parameter_set_id = 16U;
	status = video_h265_keys_add(&keys, &add, VK_TRUE);
	check(status == VK_ERROR_INITIALIZATION_FAILED, "keys: sps vps id 16");
	add.stdSPSCount = 0U;
	add.stdPPSCount = 1U;
	pps[0].sps_video_parameter_set_id = 0U;
	pps[0].pps_seq_parameter_set_id = 0U;
	pps[0].pps_pic_parameter_set_id = 64U;
	status = video_h265_keys_add(&keys, &add, VK_TRUE);
	check(status == VK_ERROR_INITIALIZATION_FAILED, "keys: pps id 64");
	pps[0].pps_pic_parameter_set_id = 0U;
	pps[0].pps_seq_parameter_set_id = 16U;
	status = video_h265_keys_add(&keys, &add, VK_TRUE);
	check(status == VK_ERROR_INITIALIZATION_FAILED, "keys: pps sps id 16");
	pps[0].pps_seq_parameter_set_id = 0U;
	pps[0].sps_video_parameter_set_id = 16U;
	status = video_h265_keys_add(&keys, &add, VK_TRUE);
	check(status == VK_ERROR_INITIALIZATION_FAILED, "keys: pps vps id 16");

	/* Two sets with the same key in one update: the second is refused. */
	memset(&keys, 0, sizeof(keys));
	memset(pps, 0, sizeof(pps));
	pps[0].pps_pic_parameter_set_id = 5U;
	pps[1].pps_pic_parameter_set_id = 5U;
	add.stdPPSCount = 2U;
	status = video_h265_keys_add(&keys, &add, VK_FALSE);
	check(status == VK_ERROR_INITIALIZATION_FAILED, "keys: a repeated key in one update");

	/* H.264 keys keep their ranges in the shared key set. */
	memset(&keys, 0, sizeof(keys));
	memset(&h264_sps, 0, sizeof(h264_sps));
	memset(&h264_pps, 0, sizeof(h264_pps));
	memset(&h264_add, 0, sizeof(h264_add));
	h264_sps.seq_parameter_set_id = 31U;
	h264_pps.seq_parameter_set_id = 31U;
	h264_pps.pic_parameter_set_id = 255U;
	h264_add.stdSPSCount = 1U;
	h264_add.pStdSPSs = &h264_sps;
	h264_add.stdPPSCount = 1U;
	h264_add.pStdPPSs = &h264_pps;
	status = video_keys_add(&keys, &h264_add, VK_FALSE);
	check(status == VK_SUCCESS, "h264 keys: largest ids");
	check(keys.sps[0] == 0x80000000U && keys.pps[255] == 0x80000000U, "h264 keys: bits");
	status = video_keys_add(&keys, &h264_add, VK_FALSE);
	check(status == VK_ERROR_INITIALIZATION_FAILED, "h264 keys: update refuses");
	h264_sps.seq_parameter_set_id = 32U;
	status = video_keys_add(&keys, &h264_add, VK_TRUE);
	check(status == VK_ERROR_INITIALIZATION_FAILED, "h264 keys: sps id 32");
}

/* The profile check for each codec bit a device may have. */
static void
test_profile_check(void)
{
	struct VkPhysicalDevice_T physical;
	VkVideoDecodeH265ProfileInfoKHR h265;
	VkVideoDecodeH264ProfileInfoKHR h264;
	VkVideoProfileInfoKHR profile;
	VkResult status;

	memset(&physical, 0, sizeof(physical));
	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
	h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
	memset(&h264, 0, sizeof(h264));
	h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
	h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
	h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
	memset(&profile, 0, sizeof(profile));
	profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	profile.pNext = &h265;
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

	/* A device with H.264 only refuses H.265, and the other way round. */
	physical.supported_extensions = VULKAN_DEVICE_VIDEO_DECODE_H264;
	status = video_profile_check(&physical, &profile);
	check(status == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR, "profile: H.265 on an H.264 device");
	physical.supported_extensions = VULKAN_DEVICE_VIDEO_DECODE_H265;
	status = video_profile_check(&physical, &profile);
	check(status == VK_SUCCESS, "profile: H.265 Main");
	profile.pNext = &h264;
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	status = video_profile_check(&physical, &profile);
	check(status == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR, "profile: H.264 on an H.265 device");
	physical.supported_extensions = VULKAN_DEVICE_VIDEO_DECODE_H264 | VULKAN_DEVICE_VIDEO_DECODE_H265;
	status = video_profile_check(&physical, &profile);
	check(status == VK_SUCCESS, "profile: H.264 High on a device with both");

	/* H.265 refusals: a missing record, a profile other than Main, 10 bits, 4:2:2, an H.264 record. */
	profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	status = video_profile_check(&physical, &profile);
	check(status == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR, "profile: H.265 with an H.264 record");
	profile.pNext = NULL;
	status = video_profile_check(&physical, &profile);
	check(status == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR, "profile: H.265 without its record");
	profile.pNext = &h265;
	h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
	status = video_profile_check(&physical, &profile);
	check(status == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR, "profile: H.265 Main 10");
	h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
	status = video_profile_check(&physical, &profile);
	check(status == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR, "profile: H.265 10-bit");
	profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR;
	status = video_profile_check(&physical, &profile);
	check(status == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR, "profile: H.265 4:2:2");

	/* A codec operation the library does not carry. */
	profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	profile.videoCodecOperation = (VkVideoCodecOperationFlagBitsKHR)0x10000U;
	status = video_profile_check(&physical, &profile);
	check(status == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR, "profile: unknown operation");
}

int
main(void)
{
	test_parameters_create();
	test_parameters_update();
	test_decode();
	test_profile_and_capabilities();
	test_keys();
	test_profile_check();
	printf("h265-wire: %d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
