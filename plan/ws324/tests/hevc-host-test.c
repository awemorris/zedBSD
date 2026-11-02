/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the nvrtx H.265 decode's parts that touch no hardware: the
 * H.265 picture structure (each field at the byte and bit the public
 * structure puts it), the work buffers' sizes, the decode push buffer,
 * the scaling list and tile buffers, the description of a picture from
 * its sets, its references by surface and its initial lists, and the
 * bits of a slice segment header the decoder is told to skip.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/nvrtx-nvdec.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-video.h"

/*
 * Where one field of the H.265 picture structure lies: the word's byte,
 * the field's lowest bit and its width (a whole byte, half-word or word
 * has bit 0 and its width).
 *
 * Values referenced: NVIDIA open-gpu-doc 9fdf5c4062007929d9f4e6cbad9c9771fe61b880
 * classes/video/nvdec_drv.h (nvdec_hevc_pic_s, compiled on the host and
 * each field found by setting it alone).
 */
struct field {
	const char *name;
	uint32_t byte;
	uint32_t low;
	uint32_t width;
};

/*
 * A bit writer for the slice headers the skip is counted from: the bits
 * of the payload (before the emulation prevention), most significant
 * first.
 */
struct writer {
	uint8_t bytes[256];
	size_t bit;
};

static void check(int ok, const char *what);
static uint32_t word(const uint8_t *bytes, size_t at);
static uint32_t field_value(const uint8_t *bytes, const struct field *field);
static void test_pic(void);
static void test_sizes(void);
static void test_push(void);
static void test_scaling(void);
static void test_tiles(void);
static void test_describe(void);
static void test_references(void);
static void test_skip(void);
static void put(struct writer *writer, uint32_t value, uint32_t count);
static void put_golomb(struct writer *writer, uint32_t value);
static size_t slice(const struct writer *writer, uint32_t nal_type, bool start_code, uint8_t *out);
static void sets_default(struct nvrtx_video_h265_sps *sps, struct nvrtx_video_h265_pps *pps, struct nvrtx_video_h265_picture *picture);

/* The fields of the picture structure the driver writes, as the public structure lays them out. */
static const struct field fields[] = {
	{ "stream_len", 48U, 0U, 32U },
	{ "tileformat", 64U, 0U, 2U },
	{ "gob_height", 64U, 2U, 3U },
	{ "sw_start_code_e", 64U, 8U, 8U },
	{ "disp_output_mode", 64U, 16U, 8U },
	{ "framestride[0]", 68U, 0U, 32U },
	{ "framestride[1]", 72U, 0U, 32U },
	{ "colMvBuffersize", 76U, 0U, 32U },
	{ "HevcSaoBufferOffset", 80U, 0U, 32U },
	{ "HevcBsdCtrlOffset", 84U, 0U, 32U },
	{ "pic_width_in_luma_samples", 88U, 0U, 16U },
	{ "pic_height_in_luma_samples", 88U, 16U, 16U },
	{ "chroma_format_idc", 92U, 0U, 4U },
	{ "bit_depth_luma", 92U, 4U, 4U },
	{ "bit_depth_chroma", 92U, 8U, 4U },
	{ "log2_min_luma_coding_block_size", 92U, 12U, 4U },
	{ "log2_max_luma_coding_block_size", 92U, 16U, 4U },
	{ "log2_min_transform_block_size", 92U, 20U, 4U },
	{ "log2_max_transform_block_size", 92U, 24U, 4U },
	{ "max_transform_hierarchy_depth_inter", 96U, 0U, 3U },
	{ "max_transform_hierarchy_depth_intra", 96U, 3U, 3U },
	{ "scalingListEnable", 96U, 6U, 1U },
	{ "amp_enable_flag", 96U, 7U, 1U },
	{ "sample_adaptive_offset_enabled_flag", 96U, 8U, 1U },
	{ "pcm_enabled_flag", 96U, 9U, 1U },
	{ "pcm_sample_bit_depth_luma", 96U, 10U, 4U },
	{ "pcm_sample_bit_depth_chroma", 96U, 14U, 4U },
	{ "log2_min_pcm_luma_coding_block_size", 96U, 18U, 4U },
	{ "log2_max_pcm_luma_coding_block_size", 96U, 22U, 4U },
	{ "pcm_loop_filter_disabled_flag", 96U, 26U, 1U },
	{ "sps_temporal_mvp_enabled_flag", 96U, 27U, 1U },
	{ "strong_intra_smoothing_enabled_flag", 96U, 28U, 1U },
	{ "dependent_slice_segments_enabled_flag", 100U, 0U, 1U },
	{ "output_flag_present_flag", 100U, 1U, 1U },
	{ "num_extra_slice_header_bits", 100U, 2U, 3U },
	{ "sign_data_hiding_enabled_flag", 100U, 5U, 1U },
	{ "cabac_init_present_flag", 100U, 6U, 1U },
	{ "num_ref_idx_l0_default_active", 100U, 7U, 4U },
	{ "num_ref_idx_l1_default_active", 100U, 11U, 4U },
	{ "init_qp", 100U, 15U, 7U },
	{ "constrained_intra_pred_flag", 100U, 22U, 1U },
	{ "transform_skip_enabled_flag", 100U, 23U, 1U },
	{ "cu_qp_delta_enabled_flag", 100U, 24U, 1U },
	{ "diff_cu_qp_delta_depth", 100U, 25U, 2U },
	{ "pps_cb_qp_offset", 104U, 0U, 8U },
	{ "pps_cr_qp_offset", 104U, 8U, 8U },
	{ "pps_beta_offset", 104U, 16U, 8U },
	{ "pps_tc_offset", 104U, 24U, 8U },
	{ "pps_slice_chroma_qp_offsets_present_flag", 108U, 0U, 1U },
	{ "weighted_pred_flag", 108U, 1U, 1U },
	{ "weighted_bipred_flag", 108U, 2U, 1U },
	{ "transquant_bypass_enabled_flag", 108U, 3U, 1U },
	{ "tiles_enabled_flag", 108U, 4U, 1U },
	{ "entropy_coding_sync_enabled_flag", 108U, 5U, 1U },
	{ "num_tile_columns", 108U, 6U, 5U },
	{ "num_tile_rows", 108U, 11U, 5U },
	{ "loop_filter_across_tiles_enabled_flag", 108U, 16U, 1U },
	{ "loop_filter_across_slices_enabled_flag", 108U, 17U, 1U },
	{ "deblocking_filter_control_present_flag", 108U, 18U, 1U },
	{ "deblocking_filter_override_enabled_flag", 108U, 19U, 1U },
	{ "pps_deblocking_filter_disabled_flag", 108U, 20U, 1U },
	{ "lists_modification_present_flag", 108U, 21U, 1U },
	{ "log2_parallel_merge_level", 108U, 22U, 3U },
	{ "slice_segment_header_extension_present_flag", 108U, 25U, 1U },
	{ "num_ref_frames", 112U, 0U, 8U },
	{ "longtermflag", 112U, 16U, 16U },
	{ "IDR_picture_flag", 180U, 0U, 8U },
	{ "RAP_picture_flag", 180U, 8U, 8U },
	{ "curr_pic_idx", 180U, 16U, 8U },
	{ "pattern_id", 180U, 24U, 8U },
	{ "sw_hdr_skip_length", 184U, 0U, 16U },
	{ "separate_colour_plane_flag", 212U, 0U, 1U },
	{ "log2_max_pic_order_cnt_lsb_minus4", 212U, 1U, 4U },
	{ "num_short_term_ref_pic_sets", 212U, 5U, 7U },
	{ "num_long_term_ref_pics_sps", 212U, 12U, 6U },
	{ "num_delta_pocs_of_rps_idx", 212U, 19U, 8U },
	{ "long_term_ref_pics_present_flag", 212U, 27U, 1U },
	{ "num_bits_short_term_ref_pics_in_slice", 216U, 0U, 32U },
	{ "log2MaxTransformSkipSize", 224U, 0U, 4U },
	{ "HevcFltAboveOffset", 240U, 0U, 32U },
	{ "HevcSaoAboveOffset", 244U, 0U, 32U },
};

/* The failed checks so far. */
static int failures;

/* The checks run so far. */
static int checks;

/* Runs every test and reports the count. */
int
main(void)
{
	/* Runs every group of checks. */
	test_pic();
	test_sizes();
	test_push();
	test_scaling();
	test_tiles();
	test_describe();
	test_references();
	test_skip();

	/* Reports the count, then the result. */
	printf("hevc host test: %d checks, %d failed\n", checks, failures);
	if (failures != 0)
		return 1;
	return 0;
}

/* Counts one check and names it when it fails. */
static void
check(
	int ok,
	const char *what)
{
	/* Counts it. */
	checks++;
	if (ok)
		return;

	/* Names the failure. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* Reads a little-endian word. */
static uint32_t
word(
	const uint8_t *bytes,
	size_t at)
{
	/* The four bytes, the low one first. */
	return (uint32_t)bytes[at] | ((uint32_t)bytes[at + 1U] << 8) | ((uint32_t)bytes[at + 2U] << 16) | ((uint32_t)bytes[at + 3U] << 24);
}

/* Reads one field of the picture structure. */
static uint32_t
field_value(
	const uint8_t *bytes,
	const struct field *field)
{
	uint32_t value;

	/* The word, shifted and masked. */
	value = word(bytes, field->byte) >> field->low;
	if (field->width < 32U)
		value &= (1U << field->width) - 1U;
	return value;
}

/* Finds a field by name. */
static const struct field *
field_named(
	const char *name)
{
	size_t index;

	/* The table's entry of the name. */
	for (index = 0U; index < sizeof(fields) / sizeof(fields[0]); index++) {
		if (strcmp(fields[index].name, name) == 0)
			return &fields[index];
	}
	return NULL;
}

/* Checks one field's value. */
static void
check_field(
	const uint8_t *pic,
	const char *name,
	uint32_t expected)
{
	const struct field *field;
	char what[128];

	/* The field must be in the table and hold the value. */
	field = field_named(name);
	snprintf(what, sizeof(what), "pic: %s", name);
	check(field != NULL && field_value(pic, field) == expected, what);
}

/* The picture structure: each field where the public structure puts it, nothing else written. */
static void
test_pic(void)
{
	struct nvrtx_nvdec_hevc_picture picture;
	uint8_t pic[NVRTX_NVDEC_HEVC_PIC_BYTES + 16U];
	uint32_t index;
	int outside;

	/* A description with a distinct value in every field. */
	memset(&picture, 0, sizeof(picture));
	picture.width = 1920U;
	picture.height = 1080U;
	picture.chroma_format_idc = 1U;
	picture.bit_depth_luma = 8U;
	picture.bit_depth_chroma = 9U;
	picture.log2_min_cb = 3U;
	picture.log2_max_cb = 6U;
	picture.log2_min_tb = 2U;
	picture.log2_max_tb = 5U;
	picture.depth_inter = 4U;
	picture.depth_intra = 3U;
	picture.scaling_enabled = 1U;
	picture.amp = 1U;
	picture.sao = 1U;
	picture.pcm = 1U;
	picture.pcm_depth_luma = 8U;
	picture.pcm_depth_chroma = 7U;
	picture.log2_min_pcm = 3U;
	picture.log2_max_pcm = 5U;
	picture.pcm_loop_filter_disabled = 1U;
	picture.temporal_mvp = 1U;
	picture.strong_intra_smoothing = 1U;
	picture.dependent_slices = 1U;
	picture.output_flag_present = 1U;
	picture.extra_header_bits = 5U;
	picture.sign_data_hiding = 1U;
	picture.cabac_init_present = 1U;
	picture.ref_l0_default = 15U;
	picture.ref_l1_default = 9U;
	picture.init_qp = 100U;
	picture.constrained_intra = 1U;
	picture.transform_skip = 1U;
	picture.cu_qp_delta = 1U;
	picture.cu_qp_delta_depth = 3U;
	picture.cb_qp_offset = -12;
	picture.cr_qp_offset = 11;
	picture.beta_offset = -6;
	picture.tc_offset = 4;
	picture.slice_chroma_qp_offsets = 1U;
	picture.weighted_pred = 1U;
	picture.weighted_bipred = 1U;
	picture.transquant_bypass = 1U;
	picture.tiles = 1U;
	picture.entropy_sync = 1U;
	picture.tile_columns = 20U;
	picture.tile_rows = 22U;
	picture.filter_across_tiles = 1U;
	picture.filter_across_slices = 1U;
	picture.deblocking_control = 1U;
	picture.deblocking_override = 1U;
	picture.deblocking_disabled = 1U;
	picture.lists_modification = 1U;
	picture.log2_parallel_merge = 4U;
	picture.header_extension = 1U;
	picture.idr = 1U;
	picture.irap = 1U;
	picture.surface = 13U;
	picture.reference_count = 7U;
	picture.long_term_mask = 0xa005U;
	for (index = 0U; index < 16U; index++) {
		picture.list0[index] = (uint8_t)index;
		picture.list1[index] = (uint8_t)(15U - index);
		picture.order_difference[index] = (int32_t)index - 8;
	}
	picture.skip_bits = 0x1234U;
	picture.dxva = 1U;
	picture.separate_planes = 1U;
	picture.log2_max_poc_lsb_minus4 = 12U;
	picture.short_term_sets = 64U;
	picture.long_term_sps = 32U;
	picture.long_term_present = 1U;
	picture.delta_pocs = 200U;
	picture.short_term_bits = 0x89abU;
	picture.stream_bytes = 123457U;
	picture.pitch = 1984U;
	picture.coloc_units = 510U;
	picture.sao_units = 2040U;
	picture.bsd_units = 18360U;
	picture.filter_above_units = 18615U;
	picture.sao_above_units = 18920U;

	/* Writes the structure into a run with guard bytes after it. */
	memset(pic, 0xa5, sizeof(pic));
	nvrtx_nvdec_hevc_pic(pic, &picture);
	outside = 0;
	for (index = NVRTX_NVDEC_HEVC_PIC_BYTES; index < sizeof(pic); index++) {
		if (pic[index] != 0xa5U)
			outside = 1;
	}
	check(!outside, "pic: nothing written past the structure");

	/* Each field. */
	check_field(pic, "stream_len", 123457U);
	check_field(pic, "tileformat", 1U);
	check_field(pic, "gob_height", 0U);
	check_field(pic, "sw_start_code_e", 1U);
	check_field(pic, "disp_output_mode", 0U);
	check_field(pic, "framestride[0]", 1984U);
	check_field(pic, "framestride[1]", 1984U);
	check_field(pic, "colMvBuffersize", 510U);
	check_field(pic, "HevcSaoBufferOffset", 2040U);
	check_field(pic, "HevcBsdCtrlOffset", 18360U);
	check_field(pic, "pic_width_in_luma_samples", 1920U);
	check_field(pic, "pic_height_in_luma_samples", 1080U);
	check_field(pic, "chroma_format_idc", 1U);
	check_field(pic, "bit_depth_luma", 8U);
	check_field(pic, "bit_depth_chroma", 9U);
	check_field(pic, "log2_min_luma_coding_block_size", 3U);
	check_field(pic, "log2_max_luma_coding_block_size", 6U);
	check_field(pic, "log2_min_transform_block_size", 2U);
	check_field(pic, "log2_max_transform_block_size", 5U);
	check_field(pic, "max_transform_hierarchy_depth_inter", 4U);
	check_field(pic, "max_transform_hierarchy_depth_intra", 3U);
	check_field(pic, "scalingListEnable", 1U);
	check_field(pic, "amp_enable_flag", 1U);
	check_field(pic, "sample_adaptive_offset_enabled_flag", 1U);
	check_field(pic, "pcm_enabled_flag", 1U);
	check_field(pic, "pcm_sample_bit_depth_luma", 8U);
	check_field(pic, "pcm_sample_bit_depth_chroma", 7U);
	check_field(pic, "log2_min_pcm_luma_coding_block_size", 3U);
	check_field(pic, "log2_max_pcm_luma_coding_block_size", 5U);
	check_field(pic, "pcm_loop_filter_disabled_flag", 1U);
	check_field(pic, "sps_temporal_mvp_enabled_flag", 1U);
	check_field(pic, "strong_intra_smoothing_enabled_flag", 1U);
	check_field(pic, "dependent_slice_segments_enabled_flag", 1U);
	check_field(pic, "output_flag_present_flag", 1U);
	check_field(pic, "num_extra_slice_header_bits", 5U);
	check_field(pic, "sign_data_hiding_enabled_flag", 1U);
	check_field(pic, "cabac_init_present_flag", 1U);
	check_field(pic, "num_ref_idx_l0_default_active", 15U);
	check_field(pic, "num_ref_idx_l1_default_active", 9U);
	check_field(pic, "init_qp", 100U);
	check_field(pic, "constrained_intra_pred_flag", 1U);
	check_field(pic, "transform_skip_enabled_flag", 1U);
	check_field(pic, "cu_qp_delta_enabled_flag", 1U);
	check_field(pic, "diff_cu_qp_delta_depth", 3U);
	check_field(pic, "pps_cb_qp_offset", 0xf4U);
	check_field(pic, "pps_cr_qp_offset", 11U);
	check_field(pic, "pps_beta_offset", 0xfaU);
	check_field(pic, "pps_tc_offset", 4U);
	check_field(pic, "pps_slice_chroma_qp_offsets_present_flag", 1U);
	check_field(pic, "weighted_pred_flag", 1U);
	check_field(pic, "weighted_bipred_flag", 1U);
	check_field(pic, "transquant_bypass_enabled_flag", 1U);
	check_field(pic, "tiles_enabled_flag", 1U);
	check_field(pic, "entropy_coding_sync_enabled_flag", 1U);
	check_field(pic, "num_tile_columns", 20U);
	check_field(pic, "num_tile_rows", 22U);
	check_field(pic, "loop_filter_across_tiles_enabled_flag", 1U);
	check_field(pic, "loop_filter_across_slices_enabled_flag", 1U);
	check_field(pic, "deblocking_filter_control_present_flag", 1U);
	check_field(pic, "deblocking_filter_override_enabled_flag", 1U);
	check_field(pic, "pps_deblocking_filter_disabled_flag", 1U);
	check_field(pic, "lists_modification_present_flag", 1U);
	check_field(pic, "log2_parallel_merge_level", 4U);
	check_field(pic, "slice_segment_header_extension_present_flag", 1U);
	check_field(pic, "num_ref_frames", 7U);
	check_field(pic, "longtermflag", 0xa005U);
	check_field(pic, "IDR_picture_flag", 1U);
	check_field(pic, "RAP_picture_flag", 1U);
	check_field(pic, "curr_pic_idx", 13U);
	check_field(pic, "pattern_id", 2U);
	check_field(pic, "sw_hdr_skip_length", 0x1234U);
	check_field(pic, "separate_colour_plane_flag", 1U);
	check_field(pic, "log2_max_pic_order_cnt_lsb_minus4", 12U);
	check_field(pic, "num_short_term_ref_pic_sets", 64U);
	check_field(pic, "num_long_term_ref_pics_sps", 32U);
	check_field(pic, "num_delta_pocs_of_rps_idx", 200U);
	check_field(pic, "long_term_ref_pics_present_flag", 1U);
	check_field(pic, "num_bits_short_term_ref_pics_in_slice", 0x89abU);
	check_field(pic, "log2MaxTransformSkipSize", 2U);
	check(word(pic, 224) == 2U, "pic: no range extension but the transform skip size");
	check_field(pic, "HevcFltAboveOffset", 18615U);
	check_field(pic, "HevcSaoAboveOffset", 18920U);

	/* The lists and the order differences, a byte and a half-word each. */
	check(pic[116] == 0U && pic[116 + 15] == 15U && pic[132] == 15U && pic[132 + 15] == 0U, "pic: initreflistidxl0 and l1");
	check((pic[148] | (pic[149] << 8)) == 0xfff8U && (pic[148 + 30] | (pic[148 + 31] << 8)) == 7U, "pic: RefDiffPicOrderCnts");

	/* The encryption, the timeout (default 0), the multi-layer and slice level words stay zero. */
	check(word(pic, 0) == 0U && word(pic, 44) == 0U && word(pic, 52) == 0U && word(pic, 56) == 0U && word(pic, 60) == 0U, "pic: no encryption, default timeout");
	check(word(pic, 252) == 0U && word(pic, 256) == 0U && word(pic, 260) == 0U && word(pic, 264) == 0U, "pic: no multi-layer, no slice level decode");

	/* Without dxva the counts the decoder would parse with stay zero. */
	picture.dxva = 0U;
	nvrtx_nvdec_hevc_pic(pic, &picture);
	check(word(pic, 212) == 0U && word(pic, 216) == 0U, "pic: no DXVA counts without dxva");
	check_field(pic, "sw_hdr_skip_length", 0x1234U);
}

/* The work buffers' sizes for 1080p and 4K. */
static void
test_sizes(void)
{
	struct nvrtx_nvdec_hevc_buffers buffers;

	/* 1080p: 1920 by 1088 on CTBs, 17 surfaces, 122 columns of 16. */
	nvrtx_nvdec_hevc_buffer_sizes(1920U, 1080U, 17U, &buffers);
	check(buffers.coloc_picture == 130560U && buffers.coloc == 130560U * 17U, "sizes: 1080p co-located data");
	check(buffers.sao_at == 522240U && buffers.bsd_at == 4700160U, "sizes: 1080p SAO and BSD");
	check(buffers.filter_above_at == 4765440U && buffers.sao_above_at == 7376640U && buffers.filter == 9987840U, "sizes: 1080p above parts");

	/* 4K: 3840 by 2176 on CTBs, 242 columns of 16. */
	nvrtx_nvdec_hevc_buffer_sizes(3840U, 2160U, 17U, &buffers);
	check(buffers.coloc_picture == 522240U && buffers.sao_at == 1044480U && buffers.bsd_at == 9400320U, "sizes: 4K");
	check(buffers.filter_above_at == 9530880U && buffers.sao_above_at == 19975680U && buffers.filter == 30420480U, "sizes: 4K above parts");

	/* Every part on 256 bytes for an odd size. */
	nvrtx_nvdec_hevc_buffer_sizes(1000U, 70U, 17U, &buffers);
	check((buffers.coloc_picture & 255U) == 0U && (buffers.sao_at & 255U) == 0U && (buffers.bsd_at & 255U) == 0U, "sizes: odd size on 256 bytes");
	check((buffers.filter_above_at & 255U) == 0U && (buffers.sao_above_at & 255U) == 0U && (buffers.filter & 255U) == 0U, "sizes: odd size parts on 256 bytes");
}

/* The method header the push buffer has for one method of count words. */
static uint32_t
header(
	uint32_t method,
	uint32_t count)
{
	/* Incrementing, the count, subchannel 4, the word address. */
	return (1U << 29) | (count << 16) | (4U << 13) | (method >> 2);
}

/* The H.265 decode push buffer's words. */
static void
test_push(void)
{
	struct nvrtx_nvdec_frame frame;
	struct nvrtx_nvdec_hevc_frame hevc;
	uint8_t push[NVRTX_NVDEC_DECODE_PUSH_BYTES];
	uint32_t index;
	size_t length;
	int ok;

	/* A frame of distinct addresses. */
	memset(&frame, 0, sizeof(frame));
	frame.pic = 0x100000U;
	frame.bitstream = 0x200100U;
	frame.status = 0x300000U;
	frame.coloc = 0x400000U;
	frame.serial = 9U;
	for (index = 0U; index < NVRTX_NVDEC_SURFACES; index++) {
		frame.luma[index] = 0x1000000U + index * 0x10000U;
		frame.chroma[index] = 0x2000000U + index * 0x10000U;
	}
	hevc.scaling = 0x500000U;
	hevc.tiles = 0x600000U;
	hevc.filter = 0x700000U;
	memset(push, 0, sizeof(push));
	length = nvrtx_nvdec_push_hevc(push, &frame, &hevc);

	/* The length and the codec. */
	check(length == 224U && length <= NVRTX_NVDEC_DECODE_PUSH_BYTES, "push: 56 words");
	check(word(push, 0) == header(0x200U, 1U) && word(push, 4) == 7U, "push: application HEVC");
	check(word(push, 8) == header(0x400U, 1U) && word(push, 12) == (7U | (1U << 4) | (1U << 6)), "push: control HEVC, timer, concealment");
	check(word(push, 16) == header(0x40cU, 1U) && word(push, 20) == 9U, "push: picture index");

	/* The buffers. */
	check(word(push, 24) == header(0x404U, 1U) && word(push, 28) == 0x1000U, "push: picture structure");
	check(word(push, 32) == header(0x408U, 1U) && word(push, 36) == 0x2001U, "push: bitstream");
	check(word(push, 40) == header(0x424U, 1U) && word(push, 44) == 0x3000U, "push: status");
	check(word(push, 48) == header(0x580U, 1U) && word(push, 52) == 0x5000U, "push: scaling list");
	check(word(push, 56) == header(0x584U, 1U) && word(push, 60) == 0x6000U, "push: tile sizes");
	check(word(push, 64) == header(0x588U, 1U) && word(push, 68) == 0x7000U, "push: filter buffer");
	check(word(push, 72) == header(0x414U, 1U) && word(push, 76) == 0x4000U, "push: co-located data");

	/* The sixteen surfaces' planes, then the start. */
	check(word(push, 80) == header(0x430U, 16U) && word(push, 148) == header(0x474U, 16U), "push: sixteen luma and chroma planes");
	ok = 1;
	for (index = 0U; index < 16U; index++) {
		if (word(push, 84U + index * 4U) != (uint32_t)(frame.luma[index] >> 8))
			ok = 0;
		if (word(push, 152U + index * 4U) != (uint32_t)(frame.chroma[index] >> 8))
			ok = 0;
	}
	check(ok, "push: each plane's address");
	check(word(push, 216) == header(0x300U, 1U) && word(push, 220) == 0U, "push: execute");
}

/* The scaling list buffer's parts and the lists in raster order. */
static void
test_scaling(void)
{
	struct nvrtx_video_h265_sps sps;
	struct nvrtx_video_h265_pps pps;
	struct nvrtx_nvdec_hevc_scaling scaling;
	uint8_t buffer[NVRTX_NVDEC_HEVC_SCALING_BYTES];
	uint32_t index;

	/* The defaults: flat 4x4, Table 7-6 in raster order, DC 16. */
	memset(&sps, 0, sizeof(sps));
	memset(&pps, 0, sizeof(pps));
	sps.flags = NVRTX_VIDEO_H265_SPS_SCALING;
	nvrtx_video_h265_scaling(&sps, &pps, &scaling);
	check(scaling.list4[0][0] == 16U && scaling.list4[5][15] == 16U && scaling.dc16[3] == 16U && scaling.dc32[1] == 16U, "scaling: flat 4x4 and DC defaults");
	check(scaling.list8[0][4] == 17U && scaling.list8[0][5] == 18U && scaling.list8[0][6] == 21U && scaling.list8[0][7] == 24U, "scaling: intra default first row");
	check(scaling.list8[0][56] == 24U && scaling.list8[0][63] == 115U && scaling.list16[2][63] == 115U && scaling.list32[0][63] == 115U, "scaling: intra default corners");
	check(scaling.list8[3][6] == 20U && scaling.list8[5][63] == 91U && scaling.list16[3][63] == 91U && scaling.list32[1][63] == 91U, "scaling: inter defaults");

	/* The sequence's own lists, coded index i holding i: raster places by the diagonal scan. */
	sps.flags |= NVRTX_VIDEO_H265_SPS_SCALING_DATA;
	sps.has_scaling = true;
	for (index = 0U; index < 16U; index++)
		sps.scaling.list4[1][index] = (uint8_t)index;
	for (index = 0U; index < 64U; index++) {
		sps.scaling.list8[2][index] = (uint8_t)index;
		sps.scaling.list32[1][index] = (uint8_t)(100U + index);
	}
	sps.scaling.dc16[5] = 40U;
	sps.scaling.dc32[0] = 41U;
	nvrtx_video_h265_scaling(&sps, &pps, &scaling);
	check(scaling.list4[1][0] == 0U && scaling.list4[1][4] == 1U && scaling.list4[1][1] == 2U && scaling.list4[1][15] == 15U, "scaling: 4x4 diagonal to raster");
	check(scaling.list8[2][8] == 1U && scaling.list8[2][1] == 2U && scaling.list8[2][56] == 28U && scaling.list8[2][7] == 35U, "scaling: 8x8 diagonal to raster");
	check(scaling.list32[1][63] == 163U && scaling.dc16[5] == 40U && scaling.dc32[0] == 41U, "scaling: 32x32 and DC values");

	/* The picture's own lists win over the sequence's. */
	pps.flags = NVRTX_VIDEO_H265_PPS_SCALING_DATA;
	pps.has_scaling = true;
	pps.scaling.list4[1][0] = 99U;
	nvrtx_video_h265_scaling(&sps, &pps, &scaling);
	check(scaling.list4[1][0] == 99U && scaling.list8[2][8] == 0U, "scaling: the picture's lists win");

	/* The buffer's parts. */
	memset(&scaling, 0, sizeof(scaling));
	for (index = 0U; index < 6U; index++)
		scaling.dc16[index] = (uint8_t)(1U + index);
	scaling.dc32[0] = 7U;
	scaling.dc32[1] = 8U;
	scaling.list4[0][0] = 9U;
	scaling.list4[5][15] = 10U;
	scaling.list8[0][0] = 11U;
	scaling.list8[5][63] = 12U;
	scaling.list16[0][0] = 13U;
	scaling.list16[5][63] = 14U;
	scaling.list32[0][0] = 15U;
	scaling.list32[1][63] = 16U;
	scaling.list4[1][1] = 17U;
	scaling.list8[2][3] = 18U;
	scaling.list16[4][8] = 19U;
	scaling.list32[0][7] = 20U;
	memset(buffer, 0xee, sizeof(buffer));
	nvrtx_nvdec_hevc_scaling(buffer, &scaling);
	check(buffer[0] == 1U && buffer[5] == 6U && buffer[6] == 7U && buffer[7] == 8U && buffer[8] == 0U && buffer[15] == 0U, "scaling: DC values and the pad");
	check(buffer[16] == 9U && buffer[111] == 10U && buffer[112] == 11U && buffer[495] == 12U, "scaling: 4x4 and 8x8 parts");
	check(buffer[496] == 13U && buffer[879] == 14U && buffer[880] == 15U && buffer[1007] == 16U && buffer[1008] == 0U, "scaling: 16x16 and 32x32 parts");
	check(buffer[16 + 16 + 4] == 17U && buffer[112 + 128 + 24] == 18U && buffer[496 + 256 + 1] == 19U && buffer[880 + 56] == 20U, "scaling: each list column by column");
}

/* Reads a 16-bit little-endian value. */
static uint32_t
half(
	const uint8_t *bytes,
	size_t at)
{
	/* The two bytes. */
	return (uint32_t)bytes[at] | ((uint32_t)bytes[at + 1U] << 8);
}

/* The tile buffer and the tiles of a picture. */
static void
test_tiles(void)
{
	struct nvrtx_video_h265_sps sps;
	struct nvrtx_video_h265_pps pps;
	struct nvrtx_nvdec_hevc_tiles tiles;
	uint8_t buffer[NVRTX_NVDEC_HEVC_TILE_BYTES];

	/* 1920 by 1080 in CTBs of 64: 30 by 17. */
	memset(&sps, 0, sizeof(sps));
	memset(&pps, 0, sizeof(pps));
	sps.width = 1920U;
	sps.height = 1080U;
	sps.log2_diff_cb = 3U;

	/* Without tiles: one tile. */
	nvrtx_video_h265_tiles(&sps, &pps, &tiles);
	check(tiles.columns == 1U && tiles.rows == 1U && tiles.width[0] == 30U && tiles.height[0] == 17U, "tiles: one tile");
	memset(buffer, 0xee, sizeof(buffer));
	nvrtx_nvdec_hevc_tiles(buffer, &tiles, false);
	check(half(buffer, 0) == 30U && half(buffer, 2) == 17U && half(buffer, 4) == 0U && half(buffer, 0x700) == 0U, "tiles: one tile's buffer");

	/* Uniform: three columns of 10, rows of 8 and 9. */
	pps.flags = NVRTX_VIDEO_H265_PPS_TILES | NVRTX_VIDEO_H265_PPS_UNIFORM;
	pps.tile_columns_minus1 = 2U;
	pps.tile_rows_minus1 = 1U;
	nvrtx_video_h265_tiles(&sps, &pps, &tiles);
	check(tiles.width[0] == 10U && tiles.width[1] == 10U && tiles.width[2] == 10U && tiles.height[0] == 8U && tiles.height[1] == 9U, "tiles: uniform");
	nvrtx_nvdec_hevc_tiles(buffer, &tiles, true);
	check(half(buffer, 0) == 10U && half(buffer, 2) == 8U && half(buffer, 20) == 10U && half(buffer, 22) == 9U && half(buffer, 24) == 0U, "tiles: each tile's size");
	check(half(buffer, 0x700) == 40U && half(buffer, 0x702) == 80U && half(buffer, 0x704) == 120U, "tiles: column edges in 16 pixels");
	check(half(buffer, 0x706) == 32U && half(buffer, 0x708) == 68U && half(buffer, 0x70a) == 0U, "tiles: row edges in 16 pixels");

	/* Explicit: columns of 5, 10 and what is left (15); rows of 4 and 13. */
	pps.flags = NVRTX_VIDEO_H265_PPS_TILES;
	pps.column_width_minus1[0] = 4U;
	pps.column_width_minus1[1] = 9U;
	pps.row_height_minus1[0] = 3U;
	nvrtx_video_h265_tiles(&sps, &pps, &tiles);
	check(tiles.width[0] == 5U && tiles.width[1] == 10U && tiles.width[2] == 15U && tiles.height[0] == 4U && tiles.height[1] == 13U, "tiles: explicit");

	/* CTBs of 16: the edges are in CTBs. */
	sps.log2_diff_cb = 1U;
	pps.flags = 0U;
	nvrtx_video_h265_tiles(&sps, &pps, &tiles);
	check(tiles.width[0] == 120U && tiles.height[0] == 68U && tiles.log2_ctb == 4U, "tiles: CTBs of 16");
}

/* The sets of a 1080p Main stream that the description and the skip tests start from. */
static void
sets_default(
	struct nvrtx_video_h265_sps *sps,
	struct nvrtx_video_h265_pps *pps,
	struct nvrtx_video_h265_picture *picture)
{
	/* The sequence. */
	memset(sps, 0, sizeof(*sps));
	sps->chroma_format_idc = 1U;
	sps->width = 1920U;
	sps->height = 1080U;
	sps->log2_max_poc_lsb_minus4 = 4U;
	sps->log2_diff_cb = 3U;
	sps->log2_diff_tb = 3U;

	/* The picture set. */
	memset(pps, 0, sizeof(*pps));
	pps->id = 0U;

	/* The picture, without references. */
	memset(picture, 0, sizeof(*picture));
	memset(picture->before, 0xff, sizeof(picture->before));
	memset(picture->after, 0xff, sizeof(picture->after));
	memset(picture->long_term, 0xff, sizeof(picture->long_term));
}

/* The description from the sets. */
static void
test_describe(void)
{
	struct nvrtx_video_h265_sps sps;
	struct nvrtx_video_h265_pps pps;
	struct nvrtx_video_h265_picture picture;
	struct nvrtx_nvdec_hevc_picture description;

	/* A stream with PCM, tiles and signed offsets. */
	sets_default(&sps, &pps, &picture);
	sps.flags = NVRTX_VIDEO_H265_SPS_PCM | NVRTX_VIDEO_H265_SPS_SAO | NVRTX_VIDEO_H265_SPS_LONG_TERM | NVRTX_VIDEO_H265_SPS_SEPARATE_PLANES;
	sps.pcm_depth_luma_minus1 = 7U;
	sps.pcm_depth_chroma_minus1 = 7U;
	sps.log2_min_pcm_minus3 = 0U;
	sps.log2_diff_pcm = 2U;
	sps.short_term_sets = 5U;
	sps.long_term_sps = 3U;
	pps.flags = NVRTX_VIDEO_H265_PPS_TILES | NVRTX_VIDEO_H265_PPS_ACROSS_TILES | NVRTX_VIDEO_H265_PPS_ACROSS_SLICES;
	pps.tile_columns_minus1 = 3U;
	pps.tile_rows_minus1 = 2U;
	pps.l0_minus1 = 2U;
	pps.l1_minus1 = 0U;
	pps.init_qp_minus26 = -26;
	pps.beta_div2 = -3;
	pps.tc_div2 = 2;
	pps.cb_qp_offset = -1;
	pps.log2_merge_minus2 = 2U;
	picture.flags = NVRTX_VIDEO_H265_PICTURE_IRAP | NVRTX_VIDEO_H265_PICTURE_IDR;
	picture.delta_pocs = 3U;
	picture.short_term_bits = 17U;
	memset(&description, 0, sizeof(description));
	nvrtx_video_h265_describe(&sps, &pps, &picture, &description);

	/* The sizes and tools as the decoder counts them. */
	check(description.width == 1920U && description.height == 1080U && description.bit_depth_luma == 8U && description.bit_depth_chroma == 8U, "describe: size and depths");
	check(description.log2_min_cb == 3U && description.log2_max_cb == 6U && description.log2_min_tb == 2U && description.log2_max_tb == 5U, "describe: block sizes");
	check(description.pcm == 1U && description.pcm_depth_luma == 8U && description.log2_min_pcm == 3U && description.log2_max_pcm == 5U, "describe: PCM");
	check(description.sao == 1U && description.amp == 0U && description.scaling_enabled == 0U, "describe: tools");

	/* The picture parameter set. */
	check(description.ref_l0_default == 3U && description.ref_l1_default == 1U && description.init_qp == 0U, "describe: references and quantizer");
	check(description.beta_offset == -6 && description.tc_offset == 4 && description.cb_qp_offset == -1, "describe: offsets");
	check(description.tiles == 1U && description.tile_columns == 4U && description.tile_rows == 3U && description.filter_across_tiles == 1U, "describe: tiles");
	check(description.filter_across_slices == 1U && description.log2_parallel_merge == 4U, "describe: filters and merge level");

	/* The picture and the counts the decoder may parse with. */
	check(description.idr == 1U && description.irap == 1U, "describe: picture kind");
	check(description.separate_planes == 1U && description.short_term_sets == 5U && description.long_term_sps == 3U && description.long_term_present == 1U, "describe: header counts");
	check(description.delta_pocs == 3U && description.short_term_bits == 17U && description.log2_max_poc_lsb_minus4 == 4U, "describe: picture's counts");

	/* Without tiles, no tile counts. */
	pps.flags = 0U;
	memset(&description, 0, sizeof(description));
	nvrtx_video_h265_describe(&sps, &pps, &picture, &description);
	check(description.tiles == 0U && description.tile_columns == 0U && description.tile_rows == 0U && description.filter_across_tiles == 0U, "describe: no tiles");
}

/* The references by surface, the substitute and the initial lists. */
static void
test_references(void)
{
	struct nvrtx_video_h265_picture picture;
	struct nvrtx_video_h265_reference references[4];
	struct nvrtx_nvdec_hevc_picture description;
	uint32_t substitute;
	uint32_t index;
	int error;
	int ok;

	/* The picture at 8 into surface 0; references at slots 1 (4), 3 (12) and 5 (0, long term). */
	memset(&picture, 0, sizeof(picture));
	memset(picture.before, 0xff, sizeof(picture.before));
	memset(picture.after, 0xff, sizeof(picture.after));
	memset(picture.long_term, 0xff, sizeof(picture.long_term));
	picture.poc = 8;
	picture.before[0] = 1U;
	picture.after[0] = 3U;
	picture.long_term[0] = 5U;
	references[0].slot = 1U;
	references[0].flags = 0U;
	references[0].poc = 4;
	references[1].slot = 3U;
	references[1].flags = 0U;
	references[1].poc = 12;
	references[2].slot = 5U;
	references[2].flags = NVRTX_VIDEO_H265_REFERENCE_LONG_TERM;
	references[2].poc = 0;
	memset(&description, 0, sizeof(description));
	description.surface = 0U;
	error = nvrtx_video_h265_references(&picture, references, 3U, &description, &substitute);
	check(error == 0 && substitute == 1U, "references: the substitute is the first before");
	check(description.order_difference[1] == 4 && description.order_difference[3] == -4 && description.order_difference[5] == 8, "references: differences");
	check(description.order_difference[0] == 0 && description.order_difference[2] == 4 && description.order_difference[15] == 4, "references: own and substitute differences");
	check(description.long_term_mask == (1U << 10) && description.reference_count == 3U, "references: long-term bit and count");

	/* The initial lists. */
	ok = 1;
	for (index = 0U; index < 16U; index++) {
		if (description.list0[index] != (uint8_t[3]){ 1U, 3U, 5U }[index % 3U])
			ok = 0;
		if (description.list1[index] != (uint8_t[3]){ 3U, 1U, 5U }[index % 3U])
			ok = 0;
	}
	check(ok, "references: lists before-after-long and after-before-long repeated");

	/* A set naming a slot the decode does not reference. */
	picture.after[0] = 2U;
	error = nvrtx_video_h265_references(&picture, references, 3U, &description, &substitute);
	check(error == EINVAL, "references: a set naming no reference is refused");

	/* A far reference's difference is cut to the field's range. */
	picture.after[0] = 3U;
	references[1].poc = -1000;
	error = nvrtx_video_h265_references(&picture, references, 3U, &description, &substitute);
	check(error == 0 && description.order_difference[3] == 127, "references: difference cut to 127");

	/* No references: the picture itself is the substitute, the lists stay empty. */
	memset(picture.before, 0xff, sizeof(picture.before));
	memset(picture.after, 0xff, sizeof(picture.after));
	memset(picture.long_term, 0xff, sizeof(picture.long_term));
	description.surface = 4U;
	error = nvrtx_video_h265_references(&picture, references, 0U, &description, &substitute);
	check(error == 0 && substitute == 4U && description.reference_count == 0U && description.list0[0] == 0U && description.order_difference[9] == 0, "references: none");

	/* The free surface: the first no reference holds. */
	references[0].slot = 0U;
	references[1].slot = 1U;
	check(nvrtx_video_h265_free_surface(references, 2U) == 2U && nvrtx_video_h265_free_surface(references, 0U) == 0U, "references: free surface");
}

/* Appends count bits of a value, the most significant first. */
static void
put(
	struct writer *writer,
	uint32_t value,
	uint32_t count)
{
	uint32_t index;
	uint32_t bit;

	/* Each bit. */
	for (index = 0U; index < count; index++) {
		bit = (value >> (count - 1U - index)) & 1U;
		if (bit != 0U)
			writer->bytes[writer->bit / 8U] |= (uint8_t)(0x80U >> (writer->bit % 8U));
		writer->bit++;
	}
}

/* Appends an unsigned exp-Golomb code. */
static void
put_golomb(
	struct writer *writer,
	uint32_t value)
{
	uint32_t bits;
	uint32_t code;

	/* The code is value + 1 after as many zeros as it has bits less one. */
	code = value + 1U;
	bits = 0U;
	while ((code >> bits) > 1U)
		bits++;
	put(writer, 0U, bits);
	put(writer, code, bits + 1U);
}

/*
 * Makes a slice segment NAL unit of a payload: an optional start code
 * (00 00 00 01), the two header bytes of nal_type, the payload with its
 * emulation prevention bytes and a stop bit; reports its length.
 */
static size_t
slice(
	const struct writer *writer,
	uint32_t nal_type,
	bool start_code,
	uint8_t *out)
{
	struct writer payload;
	size_t bytes;
	size_t index;
	size_t at;
	uint32_t zeros;

	/* The payload, a stop bit and a byte of data after it. */
	payload = *writer;
	put(&payload, 1U, 1U);
	while ((payload.bit % 8U) != 0U)
		put(&payload, 0U, 1U);
	put(&payload, 0xa5U, 8U);
	bytes = payload.bit / 8U;

	/* The start code and the NAL unit header. */
	at = 0U;
	if (start_code) {
		out[at++] = 0U;
		out[at++] = 0U;
		out[at++] = 0U;
		out[at++] = 1U;
	}
	out[at++] = (uint8_t)(nal_type << 1);
	out[at++] = 1U;

	/* The payload, a 3 put after two zeros before a byte of 3 or less. */
	zeros = 0U;
	for (index = 0U; index < bytes; index++) {
		if (zeros >= 2U && payload.bytes[index] <= 3U) {
			out[at++] = 3U;
			zeros = 0U;
		}
		out[at++] = payload.bytes[index];
		if (payload.bytes[index] == 0U) {
			zeros++;
		} else {
			zeros = 0U;
		}
	}

	/* Succeeded: the unit's length. */
	return at;
}

/* The bits of a slice segment header the decoder is told to skip. */
static void
test_skip(void)
{
	struct nvrtx_video_h265_sps sps;
	struct nvrtx_video_h265_pps pps;
	struct nvrtx_video_h265_picture picture;
	struct writer writer;
	uint8_t unit[300];
	uint32_t bits;
	size_t length;
	int error;

	/* An IDR picture with the output flag: only that flag is skipped. */
	sets_default(&sps, &pps, &picture);
	pps.flags = NVRTX_VIDEO_H265_PPS_OUTPUT_FLAG;
	memset(&writer, 0, sizeof(writer));
	put(&writer, 1U, 1U);
	put(&writer, 0U, 1U);
	put_golomb(&writer, 0U);
	put_golomb(&writer, 2U);
	put(&writer, 1U, 1U);
	length = slice(&writer, 19U, true, unit);
	bits = 0U;
	error = nvrtx_video_h265_skip_bits(unit, length, &sps, &pps, &picture, &bits);
	check(error == 0 && bits == 1U, "skip: IDR, the output flag");

	/* A trailing picture with the sequence's set 2 of 3 and extra bits before the type: order bits, flag, index. */
	sets_default(&sps, &pps, &picture);
	sps.short_term_sets = 3U;
	pps.extra_bits = 2U;
	memset(&writer, 0, sizeof(writer));
	put(&writer, 1U, 1U);
	put_golomb(&writer, 0U);
	put(&writer, 3U, 2U);
	put_golomb(&writer, 1U);
	put(&writer, 0x5aU, 8U);
	put(&writer, 1U, 1U);
	put(&writer, 2U, 2U);
	put(&writer, 1U, 1U);
	length = slice(&writer, 1U, true, unit);
	error = nvrtx_video_h265_skip_bits(unit, length, &sps, &pps, &picture, &bits);
	check(error == 0 && bits == 11U, "skip: the sequence's set by index");

	/* The header's own set: passed over by its bit count. */
	sets_default(&sps, &pps, &picture);
	picture.short_term_bits = 7U;
	memset(&writer, 0, sizeof(writer));
	put(&writer, 1U, 1U);
	put_golomb(&writer, 0U);
	put_golomb(&writer, 0U);
	put(&writer, 0x12U, 8U);
	put(&writer, 0U, 1U);
	put(&writer, 0x55U, 7U);
	put(&writer, 1U, 1U);
	length = slice(&writer, 1U, false, unit);
	error = nvrtx_video_h265_skip_bits(unit, length, &sps, &pps, &picture, &bits);
	check(error == 0 && bits == 16U, "skip: the header's own set, without a start code");

	/* Long-term pictures: one from the sequence's three, one of its own. */
	sets_default(&sps, &pps, &picture);
	sps.flags = NVRTX_VIDEO_H265_SPS_LONG_TERM;
	sps.short_term_sets = 3U;
	sps.long_term_sps = 3U;
	memset(&writer, 0, sizeof(writer));
	put(&writer, 1U, 1U);
	put_golomb(&writer, 0U);
	put_golomb(&writer, 1U);
	put(&writer, 0x33U, 8U);
	put(&writer, 1U, 1U);
	put(&writer, 1U, 2U);
	put_golomb(&writer, 1U);
	put_golomb(&writer, 1U);
	put(&writer, 2U, 2U);
	put(&writer, 1U, 1U);
	put_golomb(&writer, 0U);
	put(&writer, 0x44U, 8U);
	put(&writer, 1U, 1U);
	put(&writer, 0U, 1U);
	put(&writer, 1U, 1U);
	length = slice(&writer, 1U, true, unit);
	error = nvrtx_video_h265_skip_bits(unit, length, &sps, &pps, &picture, &bits);
	check(error == 0 && bits == 31U, "skip: long-term pictures");

	/* Zeros in the skipped bits: the emulation prevention bytes are not counted. */
	sets_default(&sps, &pps, &picture);
	sps.log2_max_poc_lsb_minus4 = 12U;
	picture.short_term_bits = 16U;
	memset(&writer, 0, sizeof(writer));
	put(&writer, 1U, 1U);
	put_golomb(&writer, 0U);
	put_golomb(&writer, 0U);
	put(&writer, 0U, 16U);
	put(&writer, 0U, 1U);
	put(&writer, 0U, 16U);
	put(&writer, 1U, 1U);
	length = slice(&writer, 1U, true, unit);
	check(memchr(unit + 6, 3, length - 6U) != NULL, "skip: the unit has an emulation prevention byte");
	error = nvrtx_video_h265_skip_bits(unit, length, &sps, &pps, &picture, &bits);
	check(error == 0 && bits == 33U, "skip: emulation prevention taken out");

	/* Not the first segment of the picture, another picture set, a header cut short: refused. */
	sets_default(&sps, &pps, &picture);
	memset(&writer, 0, sizeof(writer));
	put(&writer, 0U, 1U);
	put_golomb(&writer, 0U);
	length = slice(&writer, 1U, true, unit);
	error = nvrtx_video_h265_skip_bits(unit, length, &sps, &pps, &picture, &bits);
	check(error == EINVAL, "skip: not the first segment");
	memset(&writer, 0, sizeof(writer));
	put(&writer, 1U, 1U);
	put_golomb(&writer, 4U);
	put_golomb(&writer, 1U);
	length = slice(&writer, 1U, true, unit);
	error = nvrtx_video_h265_skip_bits(unit, length, &sps, &pps, &picture, &bits);
	check(error == EINVAL, "skip: another picture set");
	error = nvrtx_video_h265_skip_bits(unit, 7U, &sps, &pps, &picture, &bits);
	check(error == EINVAL, "skip: cut short");
}
