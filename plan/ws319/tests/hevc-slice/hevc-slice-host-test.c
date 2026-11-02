/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the i915 executor's H.265 slice segment header reader
 * and HCP command builder (src/drivers/gpu/i915/vk/vk-video-h265.h,
 * vk-video-hcp.h) against libmedia's conformance-tested H.265 parse.
 *
 * Check mode (default): reads one Annex B bitstream (a file, or the standard
 * input with "-") through h265-stream, h265-dpb (16 slots) and h265-std as
 * vkvideo-probe does, and for every decoded picture
 *   - makes the sequence and picture parameter set, the picture information
 *     and the slots as the executor receives them from the StdVideo
 *     structures (the flags packed as libvulkan packs them),
 *   - calls drv_i915_video_h265_check,
 *   - checks drv_i915_video_h265_matrices against libmedia's lists put
 *     through the up-right diagonal scan (6.5.3), or flat lists,
 *   - reads every slice segment's header with drv_i915_video_h265_slice_read
 *     (as the probe lays the segments in the decoder's buffer: a start code
 *     prefix and the NAL unit; again without the prefix and with a longer
 *     prefix and trailing bytes) and compares every field with the header
 *     libmedia reads from the same segment,
 *   - builds the picture's HCP commands with drv_i915_video_hcp_build as the
 *     executor resolves them and walks the batch: the commands' count and
 *     each slice state's place, type, QP, header length and BSD object.
 *   hevc-slice-host-test [--name NAME] [--corpus-out FILE] <bitstream>|-
 *
 * Fuzz mode: loads the corpus files the check mode wrote (real segments with
 * their sets) and mutates them: byte flips, truncations, garbage after a valid
 * NAL unit header, emulation prevention patterns, and values of the sets
 * within and outside their ranges.  Nothing is expected but that no call
 * crashes, no sanitizer reports, every call returns and what a call returns
 * satisfies the reader's contract.
 *   hevc-slice-host-test --fuzz N --seed S [--unchecked] <corpus>...
 *
 * Output: one line per stream ("stream NAME: ..."), MISMATCH lines, the
 * fuzz counts, and the last line "hevc-slice: PASS" or "hevc-slice: FAIL".
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "userland/desktop/libmedia/h265-dpb.h"
#include "userland/desktop/libmedia/h265-std.h"
#include "userland/desktop/libmedia/h265-stream.h"
#include "userland/desktop/libmedia/vcodec-bitreader.h"

#include "drivers/gpu/i915/vk/vk-video-h265.h"
#include "drivers/gpu/i915/vk/vk-video-hcp.h"
#include "drivers/gpu/i915/intel/genxml-video.h"

/* The slots of the DPB the test keeps (the executor's table has I915_VIDEO_H265_SLOTS, the last left absent). */
#define TEST_SLOTS 16U

/* The largest bitstream read. */
#define TEST_MAX_INPUT (256U * 1024U * 1024U)

/* The dwords of the batch given to drv_i915_video_hcp_build: the executor's. */
#define TEST_PAIRS (I915_VIDEO_HCP_MAX_DWORDS + 2U)

/* The bytes of a segment kept in the corpus (the header is at the start). */
#define CORPUS_BYTES 2048U

/* The segments one stream gives the corpus at most. */
#define CORPUS_PER_STREAM 40U

/* The bytes of a mutated segment at most (a corpus segment and the random bytes added). */
#define FUZZ_BYTES (CORPUS_BYTES + 1024U)

/* The return codes the fuzz counts: 0, then these, then any other. */
#define CODE_ZERO 0U
#define CODE_EINVAL 1U
#define CODE_ENOTSUP 2U
#define CODE_ENOSPC 3U
#define CODE_OTHER 4U
#define CODES 5U

/* The mismatches printed for one stream at most (all are counted). */
#define PRINT_LIMIT 40U

/* Where a mismatch is: the stream, the picture, the segment and the variant being compared. */
struct where {
	const char *name;
	unsigned long picture;
	unsigned segment;
	const char *variant;
};

/* What a stream's check found. */
struct totals {
	unsigned long pictures;
	unsigned long pictures_tested;
	unsigned long segments;
	unsigned long segments_variants;
	unsigned long mismatches;
	unsigned long printed;
	unsigned long check_refused;
	unsigned long check_invalid;
	unsigned long std_refused;
	unsigned long dpb_skipped;
	unsigned long program_runs;
	unsigned long program_failures;
	unsigned long program_too_many;
	unsigned long missing_pictures;
	unsigned long read_refused;
	unsigned long type_i;
	unsigned long type_p;
	unsigned long type_b;
	unsigned long dependent;
	unsigned long weighted;
	unsigned long entry_points;
	unsigned long modified;
	unsigned long long_term;
	unsigned long tiles;
	unsigned long wavefront;
	unsigned long scaling;
	unsigned long corpus;
	unsigned long hole_variants;
	unsigned long hole_rejected;
	unsigned long hole_same;
	unsigned long hole_differs;
	unsigned long matrices;
	unsigned long too_many_references;
};

/* One corpus entry: a real segment with the sets and slots it was read with. */
struct corpus_entry {
	struct i915_video_h265_sps sps;
	struct i915_video_h265_pps pps;
	struct i915_video_h265_picture picture;
	struct i915_video_h265_slot slots[I915_VIDEO_H265_SLOTS];
	struct i915_video_h265_slice independent;
	struct i915_video_h265_slice first;
	uint32_t has_independent;
	uint32_t has_first;
	uint32_t length;
	uint8_t *bytes;
};

/* The picture's expected reference list entry: its slot, order count and whether the count is a low part only. */
struct temp_entry {
	int slot;
	int32_t poc;
	int lsb_only;
};

static struct where where;
static struct totals totals;

/* The reason the last check or read gave (NULL when it took the input). */
static const char *last_reason;

static void mismatch(const char *field, long expected, long got);
static void check_value(const char *field, long expected, long got);
static unsigned pack_sps_flags(const StdVideoH265SpsFlags *flags);
static unsigned pack_pps_flags(const StdVideoH265PpsFlags *flags);
static unsigned pack_picture_flags(const StdVideoDecodeH265PictureInfoFlags *flags);
static void convert_scaling(const StdVideoH265ScalingLists *std, struct i915_video_h265_scaling *out);
static void convert_sps(const StdVideoH265SequenceParameterSet *std, struct i915_video_h265_sps *out);
static void convert_pps(const StdVideoH265PictureParameterSet *std, struct i915_video_h265_pps *out);
static void convert_picture(const StdVideoDecodeH265PictureInfo *std, struct i915_video_h265_picture *out);
static void convert_slots(const struct h265_dpb_plan *plan, struct i915_video_h265_slot *slots);
static int reference_read(const struct h265_stream *stream, const uint8_t *nal, size_t size, const struct h265sw_slice_header *independent, struct h265sw_slice_header *out, const uint8_t **rbsp);
static size_t epb_count(const uint8_t *nal, size_t upto);
static unsigned expected_list(const struct h265_picture *picture, const struct h265_dpb_plan *plan, unsigned list, unsigned count, int modified, const uint8_t *entries, struct temp_entry *out, unsigned log2_max_poc_lsb);
static void compare_slice(const struct i915_video_h265_slice *slice, const struct h265sw_slice_header *header, const struct h265_picture *picture, const struct h265_dpb_plan *plan, const struct i915_video_h265_slot *slots, const struct i915_video_h265_pps *pps, unsigned log2_max_poc_lsb);
static void compare_weights(const struct i915_video_h265_slice *slice, const struct h265sw_slice_header *header);
static void compare_lists(const struct i915_video_h265_slice *slice, const struct h265sw_slice_header *header, const struct h265_picture *picture, const struct h265_dpb_plan *plan, const struct i915_video_h265_slot *slots, unsigned log2_max_poc_lsb);
static void normalize(const struct i915_video_h265_slice *slice, size_t prefix, struct i915_video_h265_slice *out);
static void test_picture(struct h265_stream *stream, const struct h265_picture *picture, const struct h265_dpb_plan *plan, FILE *corpus);
static int run_program(const struct i915_video_h265_sps *sps, const struct i915_video_h265_pps *pps, const struct i915_video_h265_picture *picture, const struct i915_video_h265_slot *slots, const struct i915_video_h265_slice *slices, uint32_t count, uint32_t capacity, int walk, uint64_t *hash);
static int check_code(const struct i915_video_h265_sps *sps, const struct i915_video_h265_pps *pps);
static int read_code(const uint8_t *bytes, size_t length, const struct i915_video_h265_sps *sps, const struct i915_video_h265_pps *pps, const struct i915_video_h265_picture *picture, const struct i915_video_h265_slot *slots, const struct i915_video_h265_slice *independent, struct i915_video_h265_slice *slice);
static uint32_t tile_scan(const struct i915_video_h265_geometry *geometry, uint32_t address);
static void check_matrices(const struct i915_video_h265_sps *sps, const struct i915_video_h265_pps *pps, const StdVideoH265SequenceParameterSet *std_sps, const StdVideoH265PictureParameterSet *std_pps);
static void corpus_write(FILE *file, const struct i915_video_h265_sps *sps, const struct i915_video_h265_pps *pps, const struct i915_video_h265_picture *picture, const struct i915_video_h265_slot *slots, const struct i915_video_h265_slice *independent, const struct i915_video_h265_slice *first, const uint8_t *bytes, size_t length);
static int read_input(const char *path, uint8_t **data, size_t *size);
static int check_stream(const char *name, const char *path, const char *corpus_path);
static int fuzz_run(unsigned long cases, uint64_t seed, int unchecked, int count, char **paths);
static unsigned code_of(int error);

/* Reports one mismatch (counted, and printed up to the limit). */
static void
mismatch(
	const char *field,
	long expected,
	long got)
{
	totals.mismatches++;
	if (totals.printed >= PRINT_LIMIT)
		return;
	totals.printed++;
	printf("MISMATCH %s picture %lu segment %u (%s) %s: expected %ld got %ld%s%s\n",
	       where.name, where.picture, where.segment, where.variant, field, expected, got,
	       last_reason != NULL && strstr(field, "return") != NULL ? " reason: " : "",
	       last_reason != NULL && strstr(field, "return") != NULL ? last_reason : "");
}

/* Compares one value. */
static void
check_value(
	const char *field,
	long expected,
	long got)
{
	if (expected != got)
		mismatch(field, expected, got);
}

/* Packs the sequence parameter set flags, the first declared flag in bit 0 (libvulkan video_h265_sps_flags). */
static unsigned
pack_sps_flags(
	const StdVideoH265SpsFlags *flags)
{
	unsigned word;

	word = 0U;
	word |= (unsigned)flags->sps_temporal_id_nesting_flag << 0;
	word |= (unsigned)flags->separate_colour_plane_flag << 1;
	word |= (unsigned)flags->conformance_window_flag << 2;
	word |= (unsigned)flags->sps_sub_layer_ordering_info_present_flag << 3;
	word |= (unsigned)flags->scaling_list_enabled_flag << 4;
	word |= (unsigned)flags->sps_scaling_list_data_present_flag << 5;
	word |= (unsigned)flags->amp_enabled_flag << 6;
	word |= (unsigned)flags->sample_adaptive_offset_enabled_flag << 7;
	word |= (unsigned)flags->pcm_enabled_flag << 8;
	word |= (unsigned)flags->pcm_loop_filter_disabled_flag << 9;
	word |= (unsigned)flags->long_term_ref_pics_present_flag << 10;
	word |= (unsigned)flags->sps_temporal_mvp_enabled_flag << 11;
	word |= (unsigned)flags->strong_intra_smoothing_enabled_flag << 12;
	word |= (unsigned)flags->vui_parameters_present_flag << 13;
	word |= (unsigned)flags->sps_extension_present_flag << 14;
	word |= (unsigned)flags->sps_range_extension_flag << 15;
	word |= (unsigned)flags->transform_skip_rotation_enabled_flag << 16;
	word |= (unsigned)flags->transform_skip_context_enabled_flag << 17;
	word |= (unsigned)flags->implicit_rdpcm_enabled_flag << 18;
	word |= (unsigned)flags->explicit_rdpcm_enabled_flag << 19;
	word |= (unsigned)flags->extended_precision_processing_flag << 20;
	word |= (unsigned)flags->intra_smoothing_disabled_flag << 21;
	word |= (unsigned)flags->high_precision_offsets_enabled_flag << 22;
	word |= (unsigned)flags->persistent_rice_adaptation_enabled_flag << 23;
	word |= (unsigned)flags->cabac_bypass_alignment_enabled_flag << 24;
	word |= (unsigned)flags->sps_scc_extension_flag << 25;
	word |= (unsigned)flags->sps_curr_pic_ref_enabled_flag << 26;
	word |= (unsigned)flags->palette_mode_enabled_flag << 27;
	word |= (unsigned)flags->sps_palette_predictor_initializers_present_flag << 28;
	word |= (unsigned)flags->intra_boundary_filtering_disabled_flag << 29;

	return word;
}

/* Packs the picture parameter set flags, the first declared flag in bit 0 (libvulkan video_h265_pps_flags). */
static unsigned
pack_pps_flags(
	const StdVideoH265PpsFlags *flags)
{
	unsigned word;

	word = 0U;
	word |= (unsigned)flags->dependent_slice_segments_enabled_flag << 0;
	word |= (unsigned)flags->output_flag_present_flag << 1;
	word |= (unsigned)flags->sign_data_hiding_enabled_flag << 2;
	word |= (unsigned)flags->cabac_init_present_flag << 3;
	word |= (unsigned)flags->constrained_intra_pred_flag << 4;
	word |= (unsigned)flags->transform_skip_enabled_flag << 5;
	word |= (unsigned)flags->cu_qp_delta_enabled_flag << 6;
	word |= (unsigned)flags->pps_slice_chroma_qp_offsets_present_flag << 7;
	word |= (unsigned)flags->weighted_pred_flag << 8;
	word |= (unsigned)flags->weighted_bipred_flag << 9;
	word |= (unsigned)flags->transquant_bypass_enabled_flag << 10;
	word |= (unsigned)flags->tiles_enabled_flag << 11;
	word |= (unsigned)flags->entropy_coding_sync_enabled_flag << 12;
	word |= (unsigned)flags->uniform_spacing_flag << 13;
	word |= (unsigned)flags->loop_filter_across_tiles_enabled_flag << 14;
	word |= (unsigned)flags->pps_loop_filter_across_slices_enabled_flag << 15;
	word |= (unsigned)flags->deblocking_filter_control_present_flag << 16;
	word |= (unsigned)flags->deblocking_filter_override_enabled_flag << 17;
	word |= (unsigned)flags->pps_deblocking_filter_disabled_flag << 18;
	word |= (unsigned)flags->pps_scaling_list_data_present_flag << 19;
	word |= (unsigned)flags->lists_modification_present_flag << 20;
	word |= (unsigned)flags->slice_segment_header_extension_present_flag << 21;
	word |= (unsigned)flags->pps_extension_present_flag << 22;
	word |= (unsigned)flags->cross_component_prediction_enabled_flag << 23;
	word |= (unsigned)flags->chroma_qp_offset_list_enabled_flag << 24;
	word |= (unsigned)flags->pps_curr_pic_ref_enabled_flag << 25;
	word |= (unsigned)flags->residual_adaptive_colour_transform_enabled_flag << 26;
	word |= (unsigned)flags->pps_slice_act_qp_offsets_present_flag << 27;
	word |= (unsigned)flags->pps_palette_predictor_initializers_present_flag << 28;
	word |= (unsigned)flags->monochrome_palette_flag << 29;
	word |= (unsigned)flags->pps_range_extension_flag << 30;

	return word;
}

/* Packs the decoded picture's flags, the first declared flag in bit 0 (libvulkan video_h265_picture_flags). */
static unsigned
pack_picture_flags(
	const StdVideoDecodeH265PictureInfoFlags *flags)
{
	unsigned word;

	word = 0U;
	word |= (unsigned)flags->IrapPicFlag << 0;
	word |= (unsigned)flags->IdrPicFlag << 1;
	word |= (unsigned)flags->IsReference << 2;
	word |= (unsigned)flags->short_term_ref_pic_set_sps_flag << 3;

	return word;
}

/* Copies the scaling lists as coded (the lists h265-std made are the parse's, in coded order). */
static void
convert_scaling(
	const StdVideoH265ScalingLists *std,
	struct i915_video_h265_scaling *out)
{
	memset(out, 0, sizeof(*out));
	memcpy(out->list4, std->ScalingList4x4, sizeof(out->list4));
	memcpy(out->list8, std->ScalingList8x8, sizeof(out->list8));
	memcpy(out->list16, std->ScalingList16x16, sizeof(out->list16));
	memcpy(out->list32, std->ScalingList32x32, sizeof(out->list32));
	memcpy(out->dc16, std->ScalingListDCCoef16x16, sizeof(out->dc16));
	memcpy(out->dc32, std->ScalingListDCCoef32x32, sizeof(out->dc32));
}

/* Makes the kernel's sequence parameter set from the StdVideo one. */
static void
convert_sps(
	const StdVideoH265SequenceParameterSet *std,
	struct i915_video_h265_sps *out)
{
	memset(out, 0, sizeof(*out));
	out->flags = pack_sps_flags(&std->flags);
	out->vps_id = std->sps_video_parameter_set_id;
	out->id = std->sps_seq_parameter_set_id;
	out->chroma_format_idc = (uint32_t)std->chroma_format_idc;
	out->width = std->pic_width_in_luma_samples;
	out->height = std->pic_height_in_luma_samples;
	out->bit_depth_luma_minus8 = std->bit_depth_luma_minus8;
	out->bit_depth_chroma_minus8 = std->bit_depth_chroma_minus8;
	out->log2_max_poc_lsb_minus4 = std->log2_max_pic_order_cnt_lsb_minus4;
	out->log2_min_cb_minus3 = std->log2_min_luma_coding_block_size_minus3;
	out->log2_diff_cb = std->log2_diff_max_min_luma_coding_block_size;
	out->log2_min_tb_minus2 = std->log2_min_luma_transform_block_size_minus2;
	out->log2_diff_tb = std->log2_diff_max_min_luma_transform_block_size;
	out->depth_inter = std->max_transform_hierarchy_depth_inter;
	out->depth_intra = std->max_transform_hierarchy_depth_intra;
	out->short_term_sets = std->num_short_term_ref_pic_sets;
	out->long_term_sps = std->num_long_term_ref_pics_sps;
	out->pcm_depth_luma_minus1 = std->pcm_sample_bit_depth_luma_minus1;
	out->pcm_depth_chroma_minus1 = std->pcm_sample_bit_depth_chroma_minus1;
	out->log2_min_pcm_minus3 = std->log2_min_pcm_luma_coding_block_size_minus3;
	out->log2_diff_pcm = std->log2_diff_max_min_pcm_luma_coding_block_size;
	if (std->pScalingLists != NULL) {
		out->has_scaling = true;
		convert_scaling(std->pScalingLists, &out->scaling);
	}
}

/* Makes the kernel's picture parameter set from the StdVideo one. */
static void
convert_pps(
	const StdVideoH265PictureParameterSet *std,
	struct i915_video_h265_pps *out)
{
	unsigned i;

	memset(out, 0, sizeof(*out));
	out->flags = pack_pps_flags(&std->flags);
	out->id = std->pps_pic_parameter_set_id;
	out->sps_id = std->pps_seq_parameter_set_id;
	out->vps_id = std->sps_video_parameter_set_id;
	out->extra_bits = std->num_extra_slice_header_bits;
	out->l0_minus1 = std->num_ref_idx_l0_default_active_minus1;
	out->l1_minus1 = std->num_ref_idx_l1_default_active_minus1;
	out->cu_qp_delta_depth = std->diff_cu_qp_delta_depth;
	out->log2_merge_minus2 = std->log2_parallel_merge_level_minus2;
	out->tile_columns_minus1 = std->num_tile_columns_minus1;
	out->tile_rows_minus1 = std->num_tile_rows_minus1;
	out->init_qp_minus26 = std->init_qp_minus26;
	out->cb_qp_offset = std->pps_cb_qp_offset;
	out->cr_qp_offset = std->pps_cr_qp_offset;
	out->beta_div2 = std->pps_beta_offset_div2;
	out->tc_div2 = std->pps_tc_offset_div2;
	for (i = 0U; i < I915_VIDEO_H265_COLUMN_WIDTHS; i++)
		out->column_width_minus1[i] = std->column_width_minus1[i];
	for (i = 0U; i < I915_VIDEO_H265_ROW_HEIGHTS; i++)
		out->row_height_minus1[i] = std->row_height_minus1[i];
	if (std->pScalingLists != NULL) {
		out->has_scaling = true;
		convert_scaling(std->pScalingLists, &out->scaling);
	}
}

/* Makes the kernel's picture information from the StdVideo one. */
static void
convert_picture(
	const StdVideoDecodeH265PictureInfo *std,
	struct i915_video_h265_picture *out)
{
	memset(out, 0, sizeof(*out));
	out->flags = pack_picture_flags(&std->flags);
	out->vps_id = std->sps_video_parameter_set_id;
	out->sps_id = std->pps_seq_parameter_set_id;
	out->pps_id = std->pps_pic_parameter_set_id;
	out->delta_pocs = std->NumDeltaPocsOfRefRpsIdx;
	out->short_term_bits = std->NumBitsForSTRefPicSetInSlice;
	out->poc = std->PicOrderCntVal;
	memcpy(out->before, std->RefPicSetStCurrBefore, sizeof(out->before));
	memcpy(out->after, std->RefPicSetStCurrAfter, sizeof(out->after));
	memcpy(out->long_term, std->RefPicSetLtCurr, sizeof(out->long_term));
}

/* Makes the slots: each slot an active reference of the plan holds (its order count and whether it is long-term). */
static void
convert_slots(
	const struct h265_dpb_plan *plan,
	struct i915_video_h265_slot *slots)
{
	unsigned i;
	int slot;

	memset(slots, 0, I915_VIDEO_H265_SLOTS * sizeof(*slots));
	for (i = 0U; i < plan->reference_count && i < H265_DPB_PICTURES; i++) {
		slot = plan->reference_slot[i];
		if (slot < 0 || (unsigned)slot >= TEST_SLOTS)
			continue;
		slots[slot].present = true;
		slots[slot].long_term = plan->reference_long[i] != 0U;
		slots[slot].poc = plan->reference_poc[i];
	}
}

/*
 * Reads a slice segment header with libmedia from the NAL unit's bytes (after
 * the start code), with the sets the stream holds.  *rbsp is the RBSP copy
 * (valid until the next call).
 */
static int
reference_read(
	const struct h265_stream *stream,
	const uint8_t *nal,
	size_t size,
	const struct h265sw_slice_header *independent,
	struct h265sw_slice_header *out,
	const uint8_t **rbsp)
{
	static uint8_t buffer[H265_STREAM_SLICE_BYTES];
	struct h265sw_nal info;
	struct vcodec_bitreader bits;
	const struct h265sw_pps *pps;
	const struct h265sw_sps *sps;
	int error;

	error = h265sw_nal_parse_prefix(nal, size, H265_STREAM_SLICE_BYTES, buffer, sizeof(buffer), &info);
	if (error != 0)
		return error;
	vcodec_bitreader_init(&bits, info.rbsp, info.rbsp_size);
	error = h265sw_slice_read_start(out, &bits, &info);
	if (error != 0)
		return error;
	if (out->pps_id >= H265SW_PPS_IDS || !stream->store.pps_present[out->pps_id])
		return EINVAL;
	pps = &stream->store.pps[out->pps_id];
	sps = &stream->store.sps[pps->sps_id];
	*rbsp = info.rbsp;
	return h265sw_slice_read_rest(out, &bits, sps, pps, independent);
}

/* Counts the emulation prevention bytes of a NAL unit's bytes from its payload (offset 2) up to offset upto. */
static size_t
epb_count(
	const uint8_t *nal,
	size_t upto)
{
	size_t count;
	size_t i;
	unsigned zeros;

	count = 0U;
	zeros = 0U;
	for (i = 2U; i < upto; i++) {
		if (zeros >= 2U && nal[i] == 3U) {
			count++;
			zeros = 0U;
			continue;
		}
		if (nal[i] == 0U) {
			zeros++;
		} else {
			zeros = 0U;
		}
	}

	return count;
}

/*
 * Builds the expected RefPicList of a list from the plan's slots and the
 * picture's order counts (8.3.4): the temporary list (the sets of the list's
 * order, repeated), then the entries by list_entry or the first ones.  Returns
 * the entries made (0 when the sets are empty).
 */
static unsigned
expected_list(
	const struct h265_picture *picture,
	const struct h265_dpb_plan *plan,
	unsigned list,
	unsigned count,
	int modified,
	const uint8_t *entries,
	struct temp_entry *out,
	unsigned log2_max_poc_lsb)
{
	struct temp_entry temp[96];
	const struct h265sw_rps_pocs *rps;
	unsigned total;
	unsigned length;
	unsigned used;
	unsigned i;
	unsigned entry;
	unsigned set;
	unsigned n;
	const int *slots;
	const int32_t *pocs;

	rps = &picture->rps;
	total = plan->num_st_before + plan->num_st_after + plan->num_lt_curr;
	if (total == 0U || total > 48U)
		return 0U;
	length = count;
	if (total > length)
		length = total;
	used = 0U;
	while (used < length) {
		for (set = 0U; set < 3U && used < length; set++) {
			if (set == 2U) {
				n = plan->num_lt_curr;
				slots = plan->lt_curr;
				pocs = rps->lt_curr;
			} else if ((set == 0U) == (list == 0U)) {
				n = plan->num_st_before;
				slots = plan->st_before;
				pocs = rps->st_before;
			} else {
				n = plan->num_st_after;
				slots = plan->st_after;
				pocs = rps->st_after;
			}
			for (i = 0U; i < n && used < length; i++) {
				temp[used].slot = slots[i];
				temp[used].poc = pocs[i];
				temp[used].lsb_only = set == 2U && rps->lt_curr_msb[i] == 0U;
				used++;
			}
		}
	}
	(void)log2_max_poc_lsb;
	for (i = 0U; i < count; i++) {
		entry = i;
		if (modified)
			entry = entries[i];
		if (entry >= length) {
			out[i].slot = -2;
			out[i].poc = 0;
			out[i].lsb_only = 0;
			continue;
		}
		out[i] = temp[entry];
	}

	return count;
}

/* Compares the weights of an explicit-weights slice with libmedia's. */
static void
compare_weights(
	const struct i915_video_h265_slice *slice,
	const struct h265sw_slice_header *header)
{
	const struct h265sw_weights *w;
	char field[64];
	unsigned list;
	unsigned lists;
	unsigned i;
	unsigned c;

	w = &header->weights;
	check_value("weights.luma_log2_denom", (long)w->luma_log2_denom, (long)slice->weights.luma_log2_denom);
	check_value("weights.chroma_log2_denom", (long)w->chroma_log2_denom, (long)slice->weights.chroma_log2_denom);
	lists = header->slice_type == H265SW_SLICE_B ? 2U : 1U;
	for (list = 0U; list < lists; list++) {
		for (i = 0U; i < header->num_ref_idx[list] && i < H265SW_MAX_REFS; i++) {
			sprintf(field, "weights.luma_weight[%u][%u]", list, i);
			check_value(field, (long)w->luma_weight[list][i], (long)slice->weights.luma_weight[list][i]);
			sprintf(field, "weights.luma_offset[%u][%u]", list, i);
			check_value(field, (long)w->luma_offset[list][i], (long)slice->weights.luma_offset[list][i]);
			/* An entry whose weights differ from the defaults was sent; one not sent has the defaults. */
			if (w->luma_weight[list][i] != (1 << w->luma_log2_denom) || w->luma_offset[list][i] != 0) {
				sprintf(field, "weights.luma_sent[%u][%u]", list, i);
				check_value(field, 1L, (long)slice->weights.luma_sent[list][i]);
			}
			if (w->chroma_weight[list][i][0] != (1 << w->chroma_log2_denom) || w->chroma_offset[list][i][0] != 0 ||
			    w->chroma_weight[list][i][1] != (1 << w->chroma_log2_denom) || w->chroma_offset[list][i][1] != 0) {
				sprintf(field, "weights.chroma_sent[%u][%u]", list, i);
				check_value(field, 1L, (long)slice->weights.chroma_sent[list][i]);
			}
			for (c = 0U; c < 2U; c++) {
				sprintf(field, "weights.chroma_weight[%u][%u][%u]", list, i, c);
				check_value(field, (long)w->chroma_weight[list][i][c], (long)slice->weights.chroma_weight[list][i][c]);
				sprintf(field, "weights.chroma_offset[%u][%u][%u]", list, i, c);
				check_value(field, (long)w->chroma_offset[list][i][c], (long)slice->weights.chroma_offset[list][i][c]);
			}
		}
	}
}

/* Compares the reference lists as slots, and by order count through the slots. */
static void
compare_lists(
	const struct i915_video_h265_slice *slice,
	const struct h265sw_slice_header *header,
	const struct h265_picture *picture,
	const struct h265_dpb_plan *plan,
	const struct i915_video_h265_slot *slots,
	unsigned log2_max_poc_lsb)
{
	struct temp_entry expected[I915_VIDEO_H265_SLOTS];
	char field[64];
	unsigned list;
	unsigned lists;
	unsigned i;
	unsigned made;
	unsigned slot;
	int32_t poc;
	int32_t mask;

	lists = header->slice_type == H265SW_SLICE_B ? 2U : 1U;
	check_value("num_pic_total_curr", (long)(plan->num_st_before + plan->num_st_after + plan->num_lt_curr), (long)header->num_pic_total_curr);
	for (list = 0U; list < lists; list++) {
		made = expected_list(picture, plan, list, header->num_ref_idx[list], header->list_mod[list], header->list_entry[list], expected, log2_max_poc_lsb);
		if (made != header->num_ref_idx[list]) {
			mismatch("list built from the sets", (long)header->num_ref_idx[list], (long)made);
			continue;
		}
		for (i = 0U; i < made; i++) {
			slot = slice->list[list][i];
			sprintf(field, "list[%u][%u] slot", list, i);
			check_value(field, (long)expected[i].slot, (long)slot);
			if (slot >= TEST_SLOTS)
				continue;
			poc = slots[slot].poc;
			sprintf(field, "list[%u][%u] poc", list, i);
			if (expected[i].lsb_only) {
				mask = (int32_t)((1U << log2_max_poc_lsb) - 1U);
				check_value(field, (long)expected[i].poc, (long)(poc & mask));
			} else {
				check_value(field, (long)expected[i].poc, (long)poc);
			}
		}
	}
}

/* Compares every field of the parser's segment with libmedia's header of it. */
static void
compare_slice(
	const struct i915_video_h265_slice *slice,
	const struct h265sw_slice_header *header,
	const struct h265_picture *picture,
	const struct h265_dpb_plan *plan,
	const struct i915_video_h265_slot *slots,
	const struct i915_video_h265_pps *pps,
	unsigned log2_max_poc_lsb)
{
	int inter;
	int b_slice;
	int weighted;

	inter = header->slice_type != H265SW_SLICE_I;
	b_slice = header->slice_type == H265SW_SLICE_B;
	check_value("nal_type", (long)header->nal_type, (long)slice->nal_type);
	check_value("first_in_picture", header->first_in_pic != 0, slice->first_in_picture);
	check_value("dependent", header->dependent != 0, slice->dependent);
	check_value("address", (long)header->address, (long)slice->address);
	check_value("slice_address", (long)header->slice_addr_rs, (long)slice->slice_address);
	check_value("slice_type", (long)header->slice_type, (long)slice->slice_type);
	check_value("sao_luma", header->sao_luma != 0, slice->sao_luma);
	check_value("sao_chroma", header->sao_chroma != 0, slice->sao_chroma);
	check_value("temporal_mvp", header->temporal_mvp != 0, slice->temporal_mvp);
	check_value("references[0]", (long)header->num_ref_idx[0], (long)slice->references[0]);
	check_value("references[1]", (long)header->num_ref_idx[1], (long)slice->references[1]);
	check_value("mvd_l1_zero", header->mvd_l1_zero != 0, slice->mvd_l1_zero);
	check_value("cabac_init", header->cabac_init != 0, slice->cabac_init);
	check_value("collocated_from_l0", header->collocated_from_l0 != 0, slice->collocated_from_l0);
	check_value("collocated_ref_idx", (long)header->collocated_ref_idx, (long)slice->collocated_ref_idx);
	check_value("max_merge_candidates", (long)(inter ? header->max_merge_cand : 5U), (long)slice->max_merge_candidates);
	check_value("qp", (long)header->qp, (long)slice->qp);
	check_value("cb_qp_offset", (long)header->cb_qp_offset, (long)slice->cb_qp_offset);
	check_value("cr_qp_offset", (long)header->cr_qp_offset, (long)slice->cr_qp_offset);
	check_value("deblocking_disabled", header->deblocking_disabled != 0, slice->deblocking_disabled);
	if (header->deblocking_disabled == 0) {
		check_value("beta_div2", (long)header->beta_offset_div2, (long)slice->beta_div2);
		check_value("tc_div2", (long)header->tc_offset_div2, (long)slice->tc_div2);
	}
	check_value("loop_filter_across_slices", header->loop_filter_across_slices != 0, slice->loop_filter_across_slices);
	check_value("entry_points", (long)header->num_entry_points, (long)slice->entry_points);

	/* The weights, when the picture asks for them for this type. */
	weighted = (!b_slice && inter && (pps->flags & I915_VIDEO_H265_PPS_WEIGHTED) != 0U) ||
	    (b_slice && (pps->flags & I915_VIDEO_H265_PPS_BIPRED) != 0U);
	check_value("weighted", weighted != 0, slice->weighted);
	if (weighted && slice->weighted)
		compare_weights(slice, header);

	/* The lists. */
	if (inter)
		compare_lists(slice, header, picture, plan, slots, log2_max_poc_lsb);
}

/* Copies a segment with the position-dependent values taken out (the data's and the NAL unit's offsets by the prefix). */
static void
normalize(
	const struct i915_video_h265_slice *slice,
	size_t prefix,
	struct i915_video_h265_slice *out)
{
	*out = *slice;
	out->data_offset -= prefix;
	out->nal_offset -= prefix;
}

/* Checks a pair with drv_i915_video_h265_check: 0, ENOTSUP for a stream outside the decoder, EINVAL for values outside H.265. */
static int
check_code(
	const struct i915_video_h265_sps *sps,
	const struct i915_video_h265_pps *pps)
{
	last_reason = drv_i915_video_h265_check(sps, pps);
	if (last_reason == NULL)
		return 0;
	if (strstr(last_reason, "chroma format") != NULL || strstr(last_reason, "bit depth") != NULL ||
	    strstr(last_reason, "range or screen content") != NULL || strstr(last_reason, "size outside") != NULL ||
	    strstr(last_reason, "tiles past") != NULL)
		return ENOTSUP;

	return EINVAL;
}

/* Reads a segment with drv_i915_video_h265_slice_read: 0, or EINVAL with the reason in last_reason. */
static int
read_code(
	const uint8_t *bytes,
	size_t length,
	const struct i915_video_h265_sps *sps,
	const struct i915_video_h265_pps *pps,
	const struct i915_video_h265_picture *picture,
	const struct i915_video_h265_slot *slots,
	const struct i915_video_h265_slice *independent,
	struct i915_video_h265_slice *slice)
{
	last_reason = drv_i915_video_h265_slice_read(bytes, length, sps, pps, picture, slots, independent, slice);

	return last_reason != NULL ? EINVAL : 0;
}

/* Puts a coded list of side 4 or 8 in raster order (6.5.3, written here apart from the reader's). */
static void
test_raster(
	unsigned side,
	const uint8_t *coded,
	uint8_t *out)
{
	unsigned i;
	int x;
	int y;
	int stop;

	i = 0U;
	x = 0;
	y = 0;
	stop = 0;
	while (!stop) {
		while (y >= 0) {
			if (x < (int)side && y < (int)side) {
				out[(unsigned)y * side + (unsigned)x] = coded[i];
				i++;
			}
			y--;
			x++;
		}
		y = x;
		x = 0;
		if (i >= side * side)
			stop = 1;
	}
}

/* Checks the reader's scaling matrices against libmedia's lists in use (the picture's, else the sequence's), or flat. */
static void
check_matrices(
	const struct i915_video_h265_sps *sps,
	const struct i915_video_h265_pps *pps,
	const StdVideoH265SequenceParameterSet *std_sps,
	const StdVideoH265PictureParameterSet *std_pps)
{
	struct i915_video_h265_matrices got;
	const StdVideoH265ScalingLists *lists;
	uint8_t want[64];
	char field[64];
	unsigned m;
	unsigned k;

	drv_i915_video_h265_matrices(sps, pps, &got);
	totals.matrices++;
	if (!std_sps->flags.scaling_list_enabled_flag) {
		for (m = 0U; m < 6U; m++) {
			for (k = 0U; k < 64U; k++) {
				if ((k < 16U && got.list4[m][k] != 16U) || got.list8[m][k] != 16U || got.list16[m][k] != 16U ||
				    (m < 2U && got.list32[m][k] != 16U))
					mismatch("flat matrix entry", 16, (long)got.list8[m][k]);
			}
			check_value("flat dc16", 16, (long)got.dc16[m]);
		}
		return;
	}
	lists = std_sps->pScalingLists;
	if (std_pps->flags.pps_scaling_list_data_present_flag && std_pps->pScalingLists != NULL)
		lists = std_pps->pScalingLists;
	if (lists == NULL) {
		mismatch("scaling lists in use", 1, 0);
		return;
	}
	for (m = 0U; m < 6U; m++) {
		test_raster(4U, lists->ScalingList4x4[m], want);
		for (k = 0U; k < 16U; k++) {
			sprintf(field, "matrix 4x4[%u][%u]", m, k);
			check_value(field, (long)want[k], (long)got.list4[m][k]);
		}
		test_raster(8U, lists->ScalingList8x8[m], want);
		for (k = 0U; k < 64U; k++) {
			sprintf(field, "matrix 8x8[%u][%u]", m, k);
			check_value(field, (long)want[k], (long)got.list8[m][k]);
		}
		test_raster(8U, lists->ScalingList16x16[m], want);
		for (k = 0U; k < 64U; k++) {
			sprintf(field, "matrix 16x16[%u][%u]", m, k);
			check_value(field, (long)want[k], (long)got.list16[m][k]);
		}
		sprintf(field, "dc16[%u]", m);
		check_value(field, (long)lists->ScalingListDCCoef16x16[m], (long)got.dc16[m]);
	}
	for (m = 0U; m < 2U; m++) {
		test_raster(8U, lists->ScalingList32x32[m], want);
		for (k = 0U; k < 64U; k++) {
			sprintf(field, "matrix 32x32[%u][%u]", m, k);
			check_value(field, (long)want[k], (long)got.list32[m][k]);
		}
		sprintf(field, "dc32[%u]", m);
		check_value(field, (long)lists->ScalingListDCCoef32x32[m], (long)got.dc32[m]);
	}
}

/* The tile scan address of a CTB's raster scan address (6.5.1, CtbAddrRsToTs). */
static uint32_t
tile_scan(
	const struct i915_video_h265_geometry *geometry,
	uint32_t address)
{
	uint32_t x;
	uint32_t y;
	uint32_t column;
	uint32_t row;

	x = address % geometry->width_ctbs;
	y = address / geometry->width_ctbs;
	column = 0U;
	while (column + 1U < geometry->columns && x >= geometry->column_bounds[column + 1U])
		column++;
	row = 0U;
	while (row + 1U < geometry->rows && y >= geometry->row_bounds[row + 1U])
		row++;

	return geometry->row_bounds[row] * geometry->width_ctbs +
	    geometry->column_bounds[column] * (geometry->row_bounds[row + 1U] - geometry->row_bounds[row]) +
	    (y - geometry->row_bounds[row]) * (geometry->column_bounds[column + 1U] - geometry->column_bounds[column]) +
	    (x - geometry->column_bounds[column]);
}

/* The length of a command in the batch from its header. */
static unsigned
command_length(
	uint32_t header)
{
	/* MI commands (type 0): MI_FLUSH_DW (opcode 38, length 5:0) and MI_FORCE_WAKEUP (length 7:0). */
	if ((header >> 29) == 0U && ((header >> 23) & 0x3fU) == 38U)
		return (header & 0x3fU) + 2U;
	if ((header >> 29) == 0U)
		return (header & 0xffU) + 2U;

	/* MFX_WAIT is one dword. */
	if ((header >> 16) == (GEN12_VIDEO_MFX_WAIT >> 16))
		return 1U;

	/* The video commands: DWord Length (11:0) plus two. */
	return (header & 0xfffU) + 2U;
}

/*
 * Resolves a picture as the executor does (the references' places from the
 * three current sets, made-up addresses, each segment at 4096 bytes apart
 * after a three-byte start code) and builds its HCP commands into a batch
 * of capacity dwords.  Returns 0, EINVAL for what the executor refuses
 * before the build (no segment, more than 256, a first segment that does
 * not start the picture, addresses that do not increase in tile scan), E2BIG for more
 * than eight current references, ENOSPC when the batch overflowed.  With walk, the
 * batch is walked and checked against the segments.
 */
static int
run_program(
	const struct i915_video_h265_sps *sps,
	const struct i915_video_h265_pps *pps,
	const struct i915_video_h265_picture *picture,
	const struct i915_video_h265_slot *slots,
	const struct i915_video_h265_slice *slices,
	uint32_t count,
	uint32_t capacity,
	int walk,
	uint64_t *hash)
{
	static uint32_t dwords[TEST_PAIRS];
	static struct i915_video_hcp_decode decode;
	static uint32_t starts[I915_VIDEO_HCP_MAX_SLICES];
	static uint32_t ends[I915_VIDEO_HCP_MAX_SLICES];
	struct i915_video_h265_geometry geometry;
	struct i915_gfx_batch batch;
	const uint8_t *sets[3];
	uint32_t i;
	uint32_t set;
	uint32_t at;
	uint32_t length;
	uint32_t opcode;
	uint32_t slice_states;
	uint32_t bsd_objects;
	uint32_t ref_states;
	uint32_t weight_states;
	uint32_t qm_states;
	uint32_t pic_states;
	uint32_t tile_states;
	uint32_t want_refs;
	uint32_t want_weights;
	uint8_t chosen;
	const struct i915_video_h265_slice *slice;

	*hash = 0U;
	last_reason = "no segment, or more than 256";
	if (count == 0U || count > I915_VIDEO_HCP_MAX_SLICES)
		return EINVAL;
	last_reason = "the first segment does not start the picture";
	if (!slices[0].first_in_picture)
		return EINVAL;
	last_reason = "a later segment starts the picture or does not follow the last in tile scan";
	drv_i915_video_h265_geometry(sps, pps, &geometry);
	for (i = 1U; i < count; i++) {
		if (slices[i].first_in_picture ||
		    tile_scan(&geometry, slices[i].address) <= tile_scan(&geometry, slices[i - 1U].address))
			return EINVAL;
	}
	last_reason = NULL;

	/* The resolved decode. */
	memset(&decode, 0, sizeof(decode));
	decode.sps = sps;
	decode.pps = pps;
	decode.poc = picture->poc;
	drv_i915_video_h265_matrices(sps, pps, &decode.matrices);
	decode.destination = 0x100000000ULL;
	decode.pitch = 2048U;
	decode.chroma_rows = 1088U;
	for (i = 0U; i < I915_VIDEO_HCP_LINE_BUFFERS; i++)
		decode.line_buffers[i] = 0x200000000ULL + 0x100000ULL * i;
	decode.motion_write = 0x300000000ULL;
	for (i = 0U; i < I915_VIDEO_H265_SLOTS; i++) {
		decode.place[i] = I915_VIDEO_H265_NO_SLOT;
		decode.slots[i] = slots[i];
	}
	sets[0] = picture->before;
	sets[1] = picture->after;
	sets[2] = picture->long_term;
	for (set = 0U; set < 3U; set++) {
		for (i = 0U; i < I915_VIDEO_H265_SET_ENTRIES; i++) {
			chosen = sets[set][i];
			if (chosen >= I915_VIDEO_H265_SLOTS || !slots[chosen].present || decode.place[chosen] != I915_VIDEO_H265_NO_SLOT)
				continue;
			if (decode.reference_count >= I915_VIDEO_HCP_REFERENCES)
				return E2BIG;
			decode.place[chosen] = (uint8_t)decode.reference_count;
			decode.references[decode.reference_count].picture = 0x400000000ULL + 0x1000000ULL * chosen;
			decode.references[decode.reference_count].motion = 0x500000000ULL + 0x1000000ULL * chosen;
			decode.reference_count++;
		}
	}
	decode.bitstream_base = 0x600000000ULL;
	decode.bitstream_end = 0x700000000ULL;
	decode.skew = 64U;
	for (i = 0U; i < count; i++) {
		starts[i] = 4096U * i + 3U;
		ends[i] = 4096U * i + 3000U;
	}
	decode.slice_count = count;
	decode.slices = slices;
	decode.slice_starts = starts;
	decode.slice_ends = ends;
	decode.mocs = 3U;

	/* The build. */
	memset(&batch, 0, sizeof(batch));
	batch.cmds = dwords;
	batch.capacity = capacity < TEST_PAIRS ? capacity : TEST_PAIRS;
	drv_i915_video_hcp_build(&batch, &decode);
	*hash = 1469598103934665603ULL;
	for (i = 0U; i < batch.count && i < batch.capacity; i++)
		*hash = (*hash ^ dwords[i]) * 1099511628211ULL;
	*hash = (*hash ^ batch.count) * 1099511628211ULL;
	if (batch.count > I915_VIDEO_HCP_MAX_DWORDS)
		mismatch("batch longer than I915_VIDEO_HCP_MAX_DWORDS", (long)I915_VIDEO_HCP_MAX_DWORDS, (long)batch.count);
	if (batch.overflow)
		return ENOSPC;
	if (!walk)
		return 0;

	/* The walk: every command, and each slice state and BSD object against its segment. */
	slice_states = 0U;
	bsd_objects = 0U;
	ref_states = 0U;
	weight_states = 0U;
	qm_states = 0U;
	pic_states = 0U;
	tile_states = 0U;
	want_refs = 0U;
	want_weights = 0U;
	for (i = 0U; i < count; i++) {
		if (slices[i].slice_type != I915_VIDEO_H265_SLICE_I) {
			want_refs += slices[i].slice_type == I915_VIDEO_H265_SLICE_B ? 2U : 1U;
			if (slices[i].weighted)
				want_weights += slices[i].slice_type == I915_VIDEO_H265_SLICE_B ? 2U : 1U;
		}
	}
	for (at = 0U; at < batch.count; at += length) {
		length = command_length(dwords[at]);
		if (at + length > batch.count) {
			mismatch("command past the batch's end", (long)batch.count, (long)(at + length));
			return EINVAL;
		}
		opcode = dwords[at] >> 16;
		if (opcode == GEN12_VIDEO_HCP_SLICE_STATE) {
			check_value("slice state length", GEN12_VIDEO_HCP_SLICE_STATE_DWORDS, (long)length);
			if (slice_states < count) {
				slice = &slices[slice_states];
				check_value("slice state x", (long)(slice->address % geometry.width_ctbs), (long)(dwords[at + 1U] & 0x3ffU));
				check_value("slice state y", (long)(slice->address / geometry.width_ctbs), (long)((dwords[at + 1U] >> 16) & 0x3ffU));
				check_value("slice state type", (long)slice->slice_type, (long)(dwords[at + 3U] & 3U));
				check_value("slice state qp", (long)slice->qp, (long)((dwords[at + 3U] >> 6) & 0x3fU));
				check_value("slice state last", slice_states + 1U == count, (long)((dwords[at + 3U] >> 2) & 1U));
				check_value("slice state dependent", slice->dependent, (long)((dwords[at + 3U] >> 4) & 1U));
				check_value("slice state header length", (long)slice->header_bytes, (long)(dwords[at + 5U] & 0xffffU));
				check_value("slice state slice x", (long)(slice->slice_address % geometry.width_ctbs), (long)(dwords[at + 11U] & 0x3ffU));
			}
			slice_states++;
		} else if (opcode == GEN12_VIDEO_HCP_BSD_OBJECT) {
			if (bsd_objects < count) {
				check_value("BSD length", (long)(ends[bsd_objects] - starts[bsd_objects]), (long)dwords[at + 1U]);
				check_value("BSD start", (long)(decode.skew + starts[bsd_objects]), (long)dwords[at + 2U]);
			}
			bsd_objects++;
		} else if (opcode == GEN12_VIDEO_HCP_REF_IDX_STATE) {
			ref_states++;
		} else if (opcode == GEN12_VIDEO_HCP_WEIGHTOFFSET_STATE) {
			weight_states++;
		} else if (opcode == GEN12_VIDEO_HCP_QM_STATE) {
			qm_states++;
		} else if (opcode == GEN12_VIDEO_HCP_PIC_STATE) {
			pic_states++;
			check_value("picture state length", GEN12_VIDEO_HCP_PIC_STATE_DWORDS, (long)length);
			check_value("picture width in minimum blocks", (long)((sps->width >> (sps->log2_min_cb_minus3 + 3U)) - 1U), (long)(dwords[at + 1U] & 0x7ffU));
		} else if (opcode == GEN12_VIDEO_HCP_TILE_STATE) {
			tile_states++;
		} else if (opcode == GEN12_VIDEO_HCP_PIPE_BUF_ADDR_STATE) {
			check_value("pipe buffer state length", GEN12_VIDEO_HCP_PIPE_BUF_ADDR_STATE_DWORDS, (long)length);
		}
	}
	check_value("slice states", (long)count, (long)slice_states);
	check_value("BSD objects", (long)count, (long)bsd_objects);
	check_value("reference list states", (long)want_refs, (long)ref_states);
	check_value("weight states", (long)want_weights, (long)weight_states);
	check_value("quantizer matrix states", 20L, (long)qm_states);
	check_value("picture states", 1L, (long)pic_states);
	check_value("tile states", (pps->flags & I915_VIDEO_H265_PPS_TILES) != 0U, (long)tile_states);

	return 0;
}

/* Writes one corpus entry. */
static void
corpus_write(
	FILE *file,
	const struct i915_video_h265_sps *sps,
	const struct i915_video_h265_pps *pps,
	const struct i915_video_h265_picture *picture,
	const struct i915_video_h265_slot *slots,
	const struct i915_video_h265_slice *independent,
	const struct i915_video_h265_slice *first,
	const uint8_t *bytes,
	size_t length)
{
	struct corpus_entry entry;

	memset(&entry, 0, sizeof(entry));
	entry.sps = *sps;
	entry.pps = *pps;
	entry.picture = *picture;
	memcpy(entry.slots, slots, sizeof(entry.slots));
	if (independent != NULL) {
		entry.independent = *independent;
		entry.has_independent = 1U;
	}
	if (first != NULL) {
		entry.first = *first;
		entry.has_first = 1U;
	}
	if (length > CORPUS_BYTES)
		length = CORPUS_BYTES;
	entry.length = (uint32_t)length;
	entry.bytes = NULL;
	fwrite(&entry, sizeof(entry), 1U, file);
	fwrite(bytes, 1U, length, file);
}

/* Tests one picture: the sets and slots, the check, every segment and the program. */
static void
test_picture(
	struct h265_stream *stream,
	const struct h265_picture *picture,
	const struct h265_dpb_plan *plan,
	FILE *corpus)
{
	static struct h265_std_sps std_sps;
	static struct h265_std_pps std_pps;
	static struct i915_video_h265_sps sps;
	static struct i915_video_h265_pps pps;
	static struct i915_video_h265_picture bcm_picture;
	static struct i915_video_h265_slot slots[I915_VIDEO_H265_SLOTS];
	static struct i915_video_h265_slice slices[H265SW_MAX_SLICE_SEGMENTS];
	static uint8_t buffer[16U + H265_STREAM_SLICE_BYTES];
	static uint8_t variant_buffer[16U + H265_STREAM_SLICE_BYTES];
	struct h265sw_slice_header header;
	struct h265sw_slice_header independent_header;
	struct i915_video_h265_slice normal;
	struct i915_video_h265_slice other;
	struct i915_video_h265_slice other_normal;
	struct i915_video_h265_slice *slice;
	struct i915_video_h265_slice *independent;
	struct i915_video_h265_slice *first;
	struct i915_video_h265_picture hole_picture;
	struct i915_video_h265_slice hole_slice;
	StdVideoDecodeH265PictureInfo std_picture;
	const struct h265sw_sps *libmedia_sps;
	const struct h265sw_pps *libmedia_pps;
	const uint8_t *rbsp;
	const uint8_t *nal;
	size_t nal_size;
	size_t length;
	size_t extra;
	size_t offset_in_nal;
	uint64_t hash1;
	uint64_t hash2;
	uint64_t hash3;
	unsigned i;
	unsigned count;
	int error;
	int have_independent;
	int ok;
	int wavefront;
	int tiles;
	static const uint8_t trailer[] = { 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x72, 0x00, 0x00, 0x03, 0x00 };

	totals.pictures++;
	where.picture = (unsigned long)picture->index;
	where.segment = 0U;
	where.variant = "picture";

	/* The sets and the picture in the forms the kernel gets. */
	libmedia_pps = &stream->store.pps[picture->pps_id];
	libmedia_sps = &stream->store.sps[picture->sps_id];
	if (h265_std_picture(picture, plan, &std_picture) != 0 || h265_std_sps(libmedia_sps, &std_sps) != 0 ||
	    h265_std_pps(libmedia_pps, libmedia_sps, &std_pps) != 0) {
		totals.std_refused++;
		return;
	}
	convert_sps(&std_sps.sps, &sps);
	convert_pps(&std_pps.pps, &pps);
	convert_picture(&std_picture, &bcm_picture);
	convert_slots(plan, slots);
	if (plan->missing != 0U)
		totals.missing_pictures++;

	/* The pair the decoder takes. */
	error = check_code(&sps, &pps);
	if (error == ENOTSUP) {
		totals.check_refused++;
		return;
	}
	if (error != 0) {
		totals.check_invalid++;
		mismatch("drv_i915_video_h265_check return", 0, (long)error);
		return;
	}
	totals.pictures_tested++;
	where.variant = "matrices";
	check_matrices(&sps, &pps, &std_sps.sps, &std_pps.pps);
	wavefront = (pps.flags & I915_VIDEO_H265_PPS_ENTROPY_SYNC) != 0U;
	tiles = (pps.flags & I915_VIDEO_H265_PPS_TILES) != 0U;

	/* Each segment. */
	have_independent = 0;
	memset(&independent_header, 0, sizeof(independent_header));
	independent = NULL;
	first = NULL;
	ok = 1;
	count = picture->slice_count;
	if (count > H265SW_MAX_SLICE_SEGMENTS)
		count = H265SW_MAX_SLICE_SEGMENTS;
	for (i = 0U; i < count; i++) {
		where.segment = i;
		nal = stream->data + picture->slice_offset[i];
		nal_size = picture->slice_size[i];
		totals.segments++;

		/* libmedia's header of the segment. */
		error = reference_read(stream, nal, nal_size, have_independent ? &independent_header : NULL, &header, &rbsp);
		if (error != 0) {
			where.variant = "libmedia";
			totals.read_refused++;
			mismatch("libmedia read of a segment of a returned picture", 0, (long)error);
			ok = 0;
			continue;
		}
		if (i == 0U) {
			where.variant = "libmedia";
			check_value("picture.header.slice_type", (long)picture->header.slice_type, (long)header.slice_type);
			check_value("picture.header.data_offset", (long)picture->header.data_offset, (long)header.data_offset);
		}

		/* The probe's layout: a three-byte start code prefix and the NAL unit. */
		if (nal_size > H265_STREAM_SLICE_BYTES) {
			ok = 0;
			continue;
		}
		buffer[0] = 0U;
		buffer[1] = 0U;
		buffer[2] = 1U;
		memcpy(buffer + 3U, nal, nal_size);
		length = 3U + nal_size;
		slice = &slices[i];
		where.variant = "start code 3";
		error = read_code(buffer, length, &sps, &pps, &bcm_picture, slots, independent, slice);
		if (error != 0) {
			mismatch("drv_i915_video_h265_slice_read return", 0, (long)error);
			ok = 0;
			if (!header.dependent) {
				have_independent = 1;
				independent_header = header;
			}
			continue;
		}
		totals.segments_variants++;

		/* The fields. */
		compare_slice(slice, &header, picture, plan, slots, &pps, libmedia_sps->log2_max_poc_lsb);
		check_value("nal_offset", 3L, (long)slice->nal_offset);
		check_value("header_bytes (the NAL unit header and the RBSP to the data)", (long)(2U + header.data_offset), (long)slice->header_bytes);
		offset_in_nal = slice->data_offset - 3U;
		check_value("data_offset (raw minus emulation prevention bytes, from the RBSP)",
			    (long)header.data_offset, (long)(offset_in_nal - 2U - epb_count(nal, offset_in_nal)));
		if (header.data_offset < (size_t)(nal_size) && offset_in_nal < nal_size)
			check_value("first data byte", (long)rbsp[header.data_offset], (long)nal[offset_in_nal]);

		/* The segment's kind. */
		if (header.slice_type == H265SW_SLICE_I)
			totals.type_i++;
		else if (header.slice_type == H265SW_SLICE_P)
			totals.type_p++;
		else
			totals.type_b++;
		if (header.dependent)
			totals.dependent++;
		if (slice->weighted)
			totals.weighted++;
		if (header.num_entry_points != 0U)
			totals.entry_points++;
		if (header.list_mod[0] != 0 || header.list_mod[1] != 0)
			totals.modified++;
		if (header.num_long_term != 0U)
			totals.long_term++;

		/* The same segment as a raw NAL unit, and with a longer prefix and bytes after it. */
		normalize(slice, 3U, &normal);
		/* Without its start code the executor's segment is refused (every segment the executor reads starts with one). */
		where.variant = "no start code";
		error = read_code(buffer + 3U, nal_size, &sps, &pps, &bcm_picture, slots, independent, &other);
		if (error == 0)
			mismatch("drv_i915_video_h265_slice_read of a segment without a start code: return", (long)EINVAL, 0L);
		else
			totals.segments_variants++;
		extra = sizeof(trailer);
		variant_buffer[0] = 0U;
		variant_buffer[1] = 0U;
		variant_buffer[2] = 0U;
		variant_buffer[3] = 1U;
		memcpy(variant_buffer + 4U, nal, nal_size);
		memcpy(variant_buffer + 4U + nal_size, trailer, extra);
		where.variant = "start code 4 and trailing bytes";
		error = read_code(variant_buffer, 4U + nal_size + extra, &sps, &pps, &bcm_picture, slots, independent, &other);
		if (error != 0) {
			mismatch("drv_i915_video_h265_slice_read return", 0, (long)error);
		} else {
			totals.segments_variants++;
			normalize(&other, 4U, &other_normal);
			if (memcmp(&normal, &other_normal, sizeof(normal)) != 0)
				mismatch("slice differs from the start code variant", 0, 1);
		}

		/*
		 * A picture with a missing reference has no-slot entries in its sets (libvulkan sends 0xff where the DPB
		 * found nothing).  The first current entry made missing: the segment's header, whose fields do not depend
		 * on the lists, should read the same or be refused (informational: counted, not a mismatch).
		 */
		if (i == 0U && header.slice_type != H265SW_SLICE_I &&
		    plan->num_st_before + plan->num_st_after + plan->num_lt_curr >= 2U) {
			hole_picture = bcm_picture;
			if (hole_picture.before[0] != I915_VIDEO_H265_NO_SLOT) {
				hole_picture.before[0] = I915_VIDEO_H265_NO_SLOT;
			} else if (hole_picture.after[0] != I915_VIDEO_H265_NO_SLOT) {
				hole_picture.after[0] = I915_VIDEO_H265_NO_SLOT;
			} else {
				hole_picture.long_term[0] = I915_VIDEO_H265_NO_SLOT;
			}
			totals.hole_variants++;
			error = read_code(buffer, length, &sps, &pps, &hole_picture, slots, independent, &hole_slice);
			if (error != 0) {
				totals.hole_rejected++;
			} else if (hole_slice.data_offset == slice->data_offset && hole_slice.qp == slice->qp &&
				   hole_slice.max_merge_candidates == slice->max_merge_candidates &&
				   hole_slice.cb_qp_offset == slice->cb_qp_offset && hole_slice.cr_qp_offset == slice->cr_qp_offset &&
				   hole_slice.entry_points == slice->entry_points &&
				   hole_slice.collocated_ref_idx == slice->collocated_ref_idx) {
				totals.hole_same++;
			} else {
				totals.hole_differs++;
				if (totals.hole_differs <= 2U) {
					printf("NOTE %s picture %lu: with the first current entry of the sets 0xff, the header reads qp %d (expected %d), "
					       "data_offset %lu (expected %lu), max_merge %u (expected %u), sets before %u after %u long-term %u\n",
					       where.name, where.picture, hole_slice.qp, slice->qp, (unsigned long)hole_slice.data_offset,
					       (unsigned long)slice->data_offset, hole_slice.max_merge_candidates, slice->max_merge_candidates,
					       plan->num_st_before, plan->num_st_after, plan->num_lt_curr);
				}
			}
		}

		/* This segment is the next ones' independent segment when it is one. */
		if (!header.dependent) {
			have_independent = 1;
			independent_header = header;
			independent = slice;
		}
		if (i == 0U)
			first = slice;

		/* The corpus: the first segments of a picture sampled, and the other kinds. */
		if (corpus != NULL && totals.corpus < CORPUS_PER_STREAM &&
		    ((picture->index % 7U == 0U && i < 2U) || header.dependent ||
		     (header.num_entry_points != 0U && picture->index % 3U == 0U) ||
		     (slice->weighted && picture->index % 2U == 0U) || header.num_long_term != 0U ||
		     header.list_mod[0] != 0 || header.list_mod[1] != 0)) {
			corpus_write(corpus, &sps, &pps, &bcm_picture, slots, header.dependent ? independent : NULL, first,
				     buffer, length);
			totals.corpus++;
		}
	}
	if (!ok)
		return;

	/* The HCP commands of all the segments, as the executor builds them. */
	where.segment = 0U;
	where.variant = "program";
	totals.program_runs++;
	if (count > I915_VIDEO_HCP_MAX_SLICES) {
		error = run_program(&sps, &pps, &bcm_picture, slots, slices, count, TEST_PAIRS, 1, &hash1);
		totals.program_too_many++;
		if (error != EINVAL) {
			totals.program_failures++;
			mismatch("program with more segments than the executor takes: return", (long)EINVAL, (long)error);
		}
		return;
	}
	error = run_program(&sps, &pps, &bcm_picture, slots, slices, count, TEST_PAIRS, 1, &hash1);
	if (error == E2BIG) {
		/* More than eight current references: the executor skips the picture (counted, not a failure). */
		totals.too_many_references++;
		return;
	}
	if (error != 0) {
		totals.program_failures++;
		mismatch("program return", 0, (long)error);
		return;
	}
	if (tiles)
		totals.tiles++;
	if (wavefront)
		totals.wavefront++;
	if ((sps.flags & I915_VIDEO_H265_SPS_SCALING) != 0U)
		totals.scaling++;

	/* The same picture again into a smaller batch (an overflow), and again in full: the same commands. */
	if (picture->index % 2U == 0U) {
		error = run_program(&sps, &pps, &bcm_picture, slots, slices, count, 3U, 0, &hash2);
		if (error != ENOSPC) {
			totals.program_failures++;
			mismatch("program into a three-dword batch: return", (long)ENOSPC, (long)error);
		}
		error = run_program(&sps, &pps, &bcm_picture, slots, slices, count, TEST_PAIRS, 0, &hash3);
		if (error != 0 || hash3 != hash1) {
			totals.program_failures++;
			mismatch("program run again: commands", (long)hash1, (long)hash3);
		}
	}
}

/* Reads a whole file ("-" for the standard input) into a new buffer. */
static int
read_input(
	const char *path,
	uint8_t **data,
	size_t *size)
{
	FILE *file;
	uint8_t *buffer;
	size_t length;
	size_t got;

	file = stdin;
	if (strcmp(path, "-") != 0)
		file = fopen(path, "rb");
	if (file == NULL)
		return errno;
	buffer = malloc(TEST_MAX_INPUT);
	if (buffer == NULL)
		return ENOMEM;
	length = 0U;
	do {
		got = fread(buffer + length, 1U, TEST_MAX_INPUT - length, file);
		length += got;
	} while (got != 0U && length < TEST_MAX_INPUT);
	if (file != stdin)
		fclose(file);
	*data = buffer;
	*size = length;

	return 0;
}

/* Checks one stream; returns 0 for PASS (including a stream the decoder refuses), 1 for FAIL. */
static int
check_stream(
	const char *name,
	const char *path,
	const char *corpus_path)
{
	struct h265_stream *stream;
	struct h265_picture *picture;
	struct h265_dpb dpb;
	struct h265_dpb_plan plan;
	struct h265_dpb_output output[H265_DPB_MAX_OUTPUT];
	const struct h265sw_sps *sps;
	FILE *corpus;
	uint8_t *data;
	size_t size;
	unsigned count;
	unsigned i;
	int error;
	int refused;
	int failed;

	memset(&totals, 0, sizeof(totals));
	where.name = name;
	where.picture = 0UL;
	where.segment = 0U;
	where.variant = "stream";
	data = NULL;
	size = 0U;
	error = read_input(path, &data, &size);
	if (error != 0) {
		printf("stream %s: FAIL (cannot read the input: %d)\n", name, error);
		return 1;
	}
	stream = calloc(1U, sizeof(*stream));
	picture = calloc(1U, sizeof(*picture));
	if (stream == NULL || picture == NULL) {
		printf("stream %s: FAIL (out of memory)\n", name);
		return 1;
	}
	corpus = NULL;
	if (corpus_path != NULL)
		corpus = fopen(corpus_path, "ab");
	h265_stream_init(stream);
	h265_stream_input(stream, data, size);
	h265_dpb_init(&dpb, TEST_SLOTS);

	for (;;) {
		error = h265_stream_next(stream, picture);
		if (error == ENOENT)
			break;
		if (error != 0)
			continue;
		sps = &stream->store.sps[picture->sps_id];
		error = h265_dpb_begin(&dpb, sps, &picture->rps, picture->irap != 0 && picture->no_rasl_output != 0,
				       picture->no_output_of_prior_pics, &plan);
		if (error != 0) {
			totals.dpb_skipped++;
			if (error == EINVAL) {
				for (i = 0U; i < plan.output_count; i++)
					h265_dpb_release(&dpb, plan.output[i].slot);
			}
			continue;
		}
		for (i = 0U; i < plan.output_count; i++)
			h265_dpb_release(&dpb, plan.output[i].slot);
		error = h265_dpb_take_slot(&dpb, &plan);
		if (error != 0) {
			totals.dpb_skipped++;
			h265_dpb_abort(&dpb);
			continue;
		}
		test_picture(stream, picture, &plan, corpus);
		error = h265_dpb_end(&dpb, &plan, picture->poc, picture->pic_output, picture->index, output, &count);
		if (error != 0) {
			h265_dpb_abort(&dpb);
			continue;
		}
		for (i = 0U; i < count; i++)
			h265_dpb_release(&dpb, output[i].slot);
	}
	if (h265_dpb_flush(&dpb, output, &count) == 0) {
		for (i = 0U; i < count; i++)
			h265_dpb_release(&dpb, output[i].slot);
	}
	if (corpus != NULL)
		fclose(corpus);
	h265_stream_destroy(stream);
	free(picture);
	free(stream);
	free(data);

	/* A stream refused whole (every picture the decoder refuses) is not a failure. */
	refused = totals.check_refused > 0U && totals.pictures_tested == 0U && totals.check_invalid == 0U;
	failed = totals.mismatches != 0U || totals.program_failures != 0U || totals.check_invalid != 0U ||
	    (totals.pictures == 0U);
	printf("stream %s: %s pictures %lu tested %lu segments %lu variants %lu mismatches %lu check_refused %lu check_invalid %lu "
	       "std_refused %lu dpb_skipped %lu program_runs %lu program_failures %lu missing_pictures %lu\n",
	       name, refused ? "REFUSED" : (failed ? "FAIL" : "PASS"), totals.pictures, totals.pictures_tested, totals.segments,
	       totals.segments_variants, totals.mismatches, totals.check_refused, totals.check_invalid, totals.std_refused,
	       totals.dpb_skipped, totals.program_runs, totals.program_failures, totals.missing_pictures);
	if (totals.pictures_tested != 0U) {
		printf("  coverage: I %lu P %lu B %lu dependent %lu weighted %lu entry_points %lu list_modification %lu long_term %lu "
		       "tiles %lu wavefront %lu scaling %lu matrices %lu too_many_segments %lu too_many_references %lu corpus %lu\n",
		       totals.type_i, totals.type_p, totals.type_b, totals.dependent, totals.weighted, totals.entry_points,
		       totals.modified, totals.long_term, totals.tiles, totals.wavefront, totals.scaling, totals.matrices,
		       totals.program_too_many, totals.too_many_references, totals.corpus);
	}
	if (totals.hole_variants != 0U) {
		printf("  missing-reference variant of the first segment: %lu, refused %lu, header read the same %lu, header read differently %lu\n",
		       totals.hole_variants, totals.hole_rejected, totals.hole_same, totals.hole_differs);
	}
	fflush(stdout);
	if (refused)
		return 2;

	return failed ? 1 : 0;
}

/* Reports the counter slot of a return code. */
static unsigned
code_of(
	int error)
{
	if (error == 0)
		return CODE_ZERO;
	if (error == EINVAL)
		return CODE_EINVAL;
	if (error == ENOTSUP)
		return CODE_ENOTSUP;
	if (error == ENOSPC)
		return CODE_ENOSPC;

	return CODE_OTHER;
}

/* The fuzz's random numbers: xorshift64*, from the seed. */
static uint64_t random_state;

static uint64_t
random_next(void)
{
	random_state ^= random_state >> 12;
	random_state ^= random_state << 25;
	random_state ^= random_state >> 27;

	return random_state * 2685821657736338717ULL;
}

/* A random number below n (n above 0). */
static uint32_t
random_below(
	uint32_t n)
{
	return (uint32_t)((random_next() >> 16) % n);
}

/* A random value for a 32-bit field of a set: near the current value, small, extreme or random. */
static uint32_t
random_value(
	uint32_t current)
{
	switch (random_below(8U)) {
	case 0:
		return random_below(8U);
	case 1:
		return random_below(70U);
	case 2:
		return current + 1U;
	case 3:
		return current - 1U;
	case 4:
		return 0xffffffffU;
	case 5:
		return 0x80000000U;
	case 6:
		return current ^ (1U << random_below(32U));
	default:
		return (uint32_t)random_next();
	}
}

/* Changes the words of a block of 32-bit fields (a set's), a few at random. */
static void
mutate_words(
	uint32_t *words,
	size_t count,
	int flags_first)
{
	unsigned n;
	unsigned i;
	size_t at;

	n = 1U + random_below(5U);
	for (i = 0U; i < n; i++) {
		at = random_below((uint32_t)count);
		if (at == 0U && flags_first) {
			words[0] ^= 1U << random_below(31U);
		} else {
			words[at] = random_value(words[at]);
		}
	}
}

/* Changes a few things of the sets and slots of a corpus entry: the values within and outside their ranges. */
static void
mutate_sets(
	struct corpus_entry *entry)
{
	unsigned n;
	unsigned i;
	unsigned which;
	unsigned at;

	n = 1U + random_below(4U);
	for (i = 0U; i < n; i++) {
		which = random_below(5U);
		if (which == 0U) {
			mutate_words(&entry->sps.flags, offsetof(struct i915_video_h265_sps, has_scaling) / sizeof(uint32_t), 1);
		} else if (which == 1U) {
			mutate_words(&entry->pps.flags, offsetof(struct i915_video_h265_pps, has_scaling) / sizeof(uint32_t), 1);
		} else if (which == 2U) {
			if (random_below(3U) == 0U) {
				entry->picture.flags ^= 1U << random_below(4U);
			} else if (random_below(2U) == 0U) {
				at = random_below(7U);
				((uint32_t *)&entry->picture.flags)[at] = random_value(((uint32_t *)&entry->picture.flags)[at]);
			} else {
				at = random_below(3U * I915_VIDEO_H265_SET_ENTRIES);
				switch (random_below(4U)) {
				case 0:
					((uint8_t *)entry->picture.before)[at] = I915_VIDEO_H265_NO_SLOT;
					break;
				case 1:
					((uint8_t *)entry->picture.before)[at] = (uint8_t)random_below(I915_VIDEO_H265_SLOTS);
					break;
				case 2:
					((uint8_t *)entry->picture.before)[at] = (uint8_t)random_below(256U);
					break;
				default:
					((uint8_t *)entry->picture.before)[at] = (uint8_t)(I915_VIDEO_H265_SLOTS + random_below(4U));
					break;
				}
			}
		} else if (which == 3U) {
			at = random_below(I915_VIDEO_H265_SLOTS);
			switch (random_below(3U)) {
			case 0:
				entry->slots[at].present = !entry->slots[at].present;
				break;
			case 1:
				entry->slots[at].long_term = !entry->slots[at].long_term;
				break;
			default:
				entry->slots[at].poc = (int32_t)random_value((uint32_t)entry->slots[at].poc);
				break;
			}
		} else {
			if (random_below(2U) == 0U) {
				entry->sps.has_scaling = !entry->sps.has_scaling;
			} else {
				entry->pps.has_scaling = !entry->pps.has_scaling;
			}
		}
	}
}

/* Changes the bytes of a segment; length is the bytes in the buffer (capacity FUZZ_BYTES). Returns the new length. */
static size_t
mutate_bytes(
	uint8_t *bytes,
	size_t length)
{
	static const uint8_t specials[] = { 0x00, 0x01, 0x03, 0x80, 0xff, 0x40, 0x7f };
	unsigned mode;
	unsigned n;
	unsigned i;
	size_t at;
	size_t span;

	if (length == 0U)
		return 0U;
	mode = random_below(6U);
	if (mode == 0U || mode == 1U) {
		/* Bit flips and byte values, most of them in the header. */
		n = 1U + random_below(6U);
		for (i = 0U; i < n; i++) {
			span = length;
			if (random_below(10U) < 7U && span > 48U)
				span = 48U;
			at = random_below((uint32_t)span);
			if (mode == 0U) {
				bytes[at] ^= (uint8_t)(1U << random_below(8U));
			} else if (random_below(2U) == 0U) {
				bytes[at] = specials[random_below(sizeof(specials))];
			} else {
				bytes[at] = (uint8_t)random_below(256U);
			}
		}
		return length;
	}
	if (mode == 2U) {
		/* A truncation. */
		return random_below((uint32_t)length + 1U);
	}
	if (mode == 3U) {
		/* A valid NAL unit header (after a start code if there is one), then random bytes of random length. */
		at = 0U;
		if (length >= 3U && bytes[0] == 0U && bytes[1] == 0U && bytes[2] == 1U)
			at = 3U;
		if (length < at + 2U)
			return length;
		span = at + 2U + random_below(300U);
		for (i = (unsigned)(at + 2U); i < span && i < FUZZ_BYTES; i++) {
			bytes[i] = random_below(3U) == 0U ? (uint8_t)random_below(256U) : specials[random_below(sizeof(specials))];
		}
		return span < FUZZ_BYTES ? span : FUZZ_BYTES;
	}
	if (mode == 4U) {
		/* An emulation prevention pattern in the header, which shifts what follows. */
		at = random_below((uint32_t)(length < 64U ? length : 64U));
		if (at + 3U < length) {
			bytes[at] = 0U;
			bytes[at + 1U] = 0U;
			bytes[at + 2U] = random_below(2U) == 0U ? 3U : (uint8_t)random_below(4U);
		}
		return length;
	}

	/* Random bytes in place of the header past the NAL unit header, all of them. */
	for (i = 5U; i < length && i < 64U; i++)
		bytes[i] = (uint8_t)random_below(256U);

	return length;
}

/* The fuzz counts. */
struct fuzz_counts {
	unsigned long cases;
	unsigned long check[CODES];
	unsigned long read[CODES];
	unsigned long program[CODES];
	unsigned long program_runs;
	unsigned long violations;
	unsigned long set_mutations;
	unsigned long byte_mutations;
	unsigned long unchecked;
};

/* Reports a violation of the parser's contract by something a call returned. */
static void
violation(
	struct fuzz_counts *counts,
	unsigned long case_number,
	const char *what)
{
	counts->violations++;
	if (counts->violations <= PRINT_LIMIT)
		printf("VIOLATION case %lu: %s\n", case_number, what);
}

/* Checks what a successful slice_read returned against the parser's contract. */
static void
check_contract(
	struct fuzz_counts *counts,
	unsigned long case_number,
	const struct corpus_entry *entry,
	const struct i915_video_h265_slice *slice,
	size_t length)
{
	uint32_t log2_ctb;
	uint32_t ctbs;
	uint32_t list;
	uint32_t i;

	if (slice->data_offset >= length || slice->nal_offset >= slice->data_offset)
		violation(counts, case_number, "nal_offset or data_offset outside the bytes");
	if (slice->header_bytes < 3U || slice->header_bytes > slice->data_offset - slice->nal_offset)
		violation(counts, case_number, "header_bytes outside the NAL unit before its data");
	if (slice->slice_type > I915_VIDEO_H265_SLICE_I)
		violation(counts, case_number, "slice_type above 2");
	if (slice->references[0] > I915_VIDEO_H265_LIST || slice->references[1] > I915_VIDEO_H265_LIST)
		violation(counts, case_number, "references above 15");
	if (slice->qp < 0 || slice->qp > 51)
		violation(counts, case_number, "qp outside 0..51");
	if (slice->cb_qp_offset < -12 || slice->cb_qp_offset > 12 || slice->cr_qp_offset < -12 || slice->cr_qp_offset > 12)
		violation(counts, case_number, "chroma qp offset outside -12..12");
	if (slice->beta_div2 < -6 || slice->beta_div2 > 6 || slice->tc_div2 < -6 || slice->tc_div2 > 6)
		violation(counts, case_number, "deblocking offset outside -6..6");
	if (slice->slice_type != I915_VIDEO_H265_SLICE_I && (slice->max_merge_candidates < 1U || slice->max_merge_candidates > 5U))
		violation(counts, case_number, "max_merge_candidates outside 1..5");
	for (list = 0U; list < 2U; list++) {
		for (i = 0U; i < slice->references[list]; i++) {
			/* A dependent segment's lists are the independent one's, made with the slots before the mutation. */
			if (slice->list[list][i] >= I915_VIDEO_H265_SLOTS ||
			    (!slice->dependent && !entry->slots[slice->list[list][i]].present))
				violation(counts, case_number, "a list entry is not a present slot");
		}
	}
	log2_ctb = entry->sps.log2_min_cb_minus3 + 3U + entry->sps.log2_diff_cb;
	if (log2_ctb >= 4U && log2_ctb <= 6U) {
		ctbs = ((entry->sps.width + (1U << log2_ctb) - 1U) >> log2_ctb) *
		    ((entry->sps.height + (1U << log2_ctb) - 1U) >> log2_ctb);
		if (slice->address >= ctbs)
			violation(counts, case_number, "address at or beyond the picture's CTBs");
	}
	if (slice->slice_type != I915_VIDEO_H265_SLICE_I) {
		if (slice->collocated_ref_idx >= slice->references[slice->collocated_from_l0 ? 0U : 1U] &&
		    slice->temporal_mvp)
			violation(counts, case_number, "collocated_ref_idx beyond the list");
	}
}

/* Runs the fuzz over the loaded corpus. */
static int
fuzz_run(
	unsigned long cases,
	uint64_t seed,
	int unchecked,
	int count,
	char **paths)
{
	struct corpus_entry *corpus;
	struct corpus_entry entry;
	struct corpus_entry *base;
	struct i915_video_h265_slice slice;
	struct i915_video_h265_slice list_slices[64];
	static struct i915_video_h265_slice many[I915_VIDEO_HCP_MAX_SLICES + 8U];
	struct fuzz_counts counts;
	static uint8_t bytes[FUZZ_BYTES];
	size_t length;
	size_t loaded;
	size_t capacity;
	FILE *file;
	uint8_t *keep;
	unsigned long n;
	unsigned long case_number;
	uint64_t hash;
	uint32_t ctbs;
	uint32_t log2_ctb;
	uint32_t segments;
	uint32_t k;
	uint32_t previous;
	uint32_t capacities[6];
	int path;
	int set_mutation;
	int error;
	int checked;
	int use_independent;
	int dependent_none;
	struct i915_video_h265_slice *slices_in;
	unsigned long reads;
	unsigned limit_entries;

	capacities[0] = 0U;
	capacities[1] = 1U;
	capacities[2] = 7U;
	capacities[3] = 100U;
	capacities[4] = 4096U;
	capacities[5] = TEST_PAIRS;

	/* The corpus. */
	corpus = NULL;
	loaded = 0U;
	capacity = 0U;
	for (path = 0; path < count; path++) {
		file = fopen(paths[path], "rb");
		if (file == NULL) {
			printf("fuzz: cannot open %s\n", paths[path]);
			return 1;
		}
		for (;;) {
			if (fread(&entry, sizeof(entry), 1U, file) != 1U)
				break;
			if (entry.length > CORPUS_BYTES)
				break;
			keep = malloc(entry.length != 0U ? entry.length : 1U);
			if (keep == NULL)
				return 1;
			if (fread(keep, 1U, entry.length, file) != entry.length)
				break;
			if (loaded == capacity) {
				capacity = capacity == 0U ? 1024U : capacity * 2U;
				corpus = realloc(corpus, capacity * sizeof(*corpus));
				if (corpus == NULL)
					return 1;
			}
			corpus[loaded] = entry;
			corpus[loaded].bytes = keep;
			loaded++;
		}
		fclose(file);
	}
	if (loaded == 0U) {
		printf("fuzz: no corpus entries\n");
		return 1;
	}
	printf("fuzz: %lu corpus segments, %lu cases, seed %llu%s\n", (unsigned long)loaded, cases, (unsigned long long)seed,
	       unchecked ? ", sets not checked" : "");

	/* The most segments of a picture: 256 are taken, 257 and more are refused whole. */
	limit_entries = 0U;
	for (n = 0UL; n < (unsigned long)loaded && limit_entries < 24U; n++) {
		entry = corpus[n];
		if (entry.has_first == 0U || (entry.pps.flags & I915_VIDEO_H265_PPS_TILES) != 0U ||
		    check_code(&entry.sps, &entry.pps) != 0)
			continue;
		log2_ctb = entry.sps.log2_min_cb_minus3 + 3U + entry.sps.log2_diff_cb;
		ctbs = ((entry.sps.width + (1U << log2_ctb) - 1U) >> log2_ctb) * ((entry.sps.height + (1U << log2_ctb) - 1U) >> log2_ctb);
		if (ctbs < 300U)
			continue;
		for (k = 0U; k < I915_VIDEO_HCP_MAX_SLICES + 8U; k++) {
			many[k] = entry.first;
			many[k].address = k;
			many[k].first_in_picture = k == 0U;
			many[k].dependent = false;
		}
		where.name = "fuzz";
		where.variant = "segment count limit";
		error = run_program(&entry.sps, &entry.pps, &entry.picture, entry.slots, many, I915_VIDEO_HCP_MAX_SLICES, TEST_PAIRS, 0, &hash);
		if (error != 0)
			mismatch("program with 256 segments: return", 0, (long)error);
		error = run_program(&entry.sps, &entry.pps, &entry.picture, entry.slots, many, I915_VIDEO_HCP_MAX_SLICES + 1U, TEST_PAIRS, 0, &hash);
		if (error != EINVAL)
			mismatch("program with 257 segments: return", (long)EINVAL, (long)error);
		error = run_program(&entry.sps, &entry.pps, &entry.picture, entry.slots, many, I915_VIDEO_HCP_MAX_SLICES + 8U, TEST_PAIRS, 0, &hash);
		if (error != EINVAL)
			mismatch("program with 264 segments: return", (long)EINVAL, (long)error);
		error = run_program(&entry.sps, &entry.pps, &entry.picture, entry.slots, many, 0U, TEST_PAIRS, 0, &hash);
		if (error != EINVAL)
			mismatch("program with no segment: return", (long)EINVAL, (long)error);
		limit_entries++;
	}
	printf("fuzz: segment count limit checked with %u pictures\n", limit_entries);

	memset(&counts, 0, sizeof(counts));
	random_state = seed * 0x9e3779b97f4a7c15ULL + 0x1234567ULL;
	if (random_state == 0U)
		random_state = 1U;
	slices_in = list_slices;
	reads = 0U;
	for (case_number = 0UL; case_number < cases; case_number++) {
		counts.cases++;
		base = &corpus[random_below((uint32_t)loaded)];
		entry = *base;
		length = entry.length;
		memcpy(bytes, base->bytes, length);

		/* What changes: the bytes, the sets, both or the independent segment. */
		k = random_below(10U);
		set_mutation = k >= 4U;
		if (k < 6U) {
			length = mutate_bytes(bytes, length);
			counts.byte_mutations++;
		}
		if (set_mutation) {
			mutate_sets(&entry);
			counts.set_mutations++;
		}
		dependent_none = random_below(8U) == 0U;

		/* The check of the sets, before the parser as the executor does. */
		checked = 1;
		if (set_mutation) {
			error = check_code(&entry.sps, &entry.pps);
			counts.check[code_of(error)]++;
			if (error != 0 && !unchecked)
				checked = 0;
			if (error != 0 && unchecked)
				counts.unchecked++;
		}
		if (!checked)
			continue;

		/* The segment. */
		use_independent = entry.has_independent != 0U && !dependent_none;
		memset(&slice, 0xa5, sizeof(slice));
		error = read_code(bytes, length, &entry.sps, &entry.pps, &entry.picture, entry.slots,
						use_independent ? &entry.independent : NULL, &slice);
		counts.read[code_of(error)]++;
		reads++;
		if (error != 0 && error != EINVAL && error != ENOTSUP)
			violation(&counts, case_number, "slice_read returned a code other than 0, EINVAL, ENOTSUP");
		if (error != 0)
			continue;
		if (!unchecked)
			check_contract(&counts, case_number, &entry, &slice, length);

		/* The program over the segment alone (when it starts a picture) or after the picture's first, or a longer run. */
		log2_ctb = entry.sps.log2_min_cb_minus3 + 3U + entry.sps.log2_diff_cb;
		if (log2_ctb < 4U || log2_ctb > 6U)
			continue;
		ctbs = ((entry.sps.width + (1U << log2_ctb) - 1U) >> log2_ctb) * ((entry.sps.height + (1U << log2_ctb) - 1U) >> log2_ctb);
		if (ctbs == 0U)
			continue;
		segments = 0U;
		k = random_below(4U);
		if (k == 0U) {
			/* The segment alone. */
			list_slices[segments++] = slice;
		} else if (k == 1U && entry.has_first != 0U) {
			/* The picture's first segment, then this one. */
			list_slices[segments++] = entry.first;
			list_slices[segments++] = slice;
		} else if (k == 2U && entry.has_first != 0U) {
			/* The picture's first segment, then this one twice (an address that does not advance). */
			list_slices[segments++] = entry.first;
			list_slices[segments++] = slice;
			list_slices[segments++] = slice;
		} else {
			/* A run of segments at increasing random addresses, as the parser would return them. */
			list_slices[segments] = entry.has_first != 0U ? entry.first : slice;
			list_slices[segments].address = 0U;
			list_slices[segments].first_in_picture = true;
			list_slices[segments].dependent = false;
			segments++;
			n = 1U + random_below(63U);
			previous = 0U;
			while (n != 0UL && previous + 1U < ctbs) {
				previous += 1U + random_below((ctbs - previous - 1U) / (unsigned)n + 1U);
				if (previous >= ctbs)
					break;
				list_slices[segments] = slice;
				list_slices[segments].address = previous;
				list_slices[segments].first_in_picture = false;
				list_slices[segments].dependent = random_below(3U) == 0U;
				segments++;
				n--;
			}
		}
		if (list_slices[0].dependent || !list_slices[0].first_in_picture || list_slices[0].address != 0U) {
			/* Not a picture's start: the program must refuse it (or take it as it is, never crash). */
		}
		where.name = "fuzz";
		error = run_program(&entry.sps, &entry.pps, &entry.picture, entry.slots, slices_in, segments,
				    capacities[random_below(6U)], 0, &hash);
		counts.program[code_of(error)]++;
		counts.program_runs++;
		if (error != 0 && error != EINVAL && error != ENOSPC && error != E2BIG)
			violation(&counts, case_number, "program returned a code other than 0, EINVAL, ENOSPC, E2BIG");
	}

	printf("fuzz cases %lu: byte mutations %lu, set mutations %lu, reads %lu, program runs %lu\n", counts.cases,
	       counts.byte_mutations, counts.set_mutations, reads, counts.program_runs);
	printf("fuzz check returns: 0 %lu EINVAL %lu ENOTSUP %lu ENOSPC %lu other %lu\n", counts.check[CODE_ZERO],
	       counts.check[CODE_EINVAL], counts.check[CODE_ENOTSUP], counts.check[CODE_ENOSPC], counts.check[CODE_OTHER]);
	printf("fuzz slice_read returns: 0 %lu EINVAL %lu ENOTSUP %lu ENOSPC %lu other %lu\n", counts.read[CODE_ZERO],
	       counts.read[CODE_EINVAL], counts.read[CODE_ENOTSUP], counts.read[CODE_ENOSPC], counts.read[CODE_OTHER]);
	printf("fuzz program returns: 0 %lu EINVAL %lu ENOTSUP %lu ENOSPC %lu other %lu\n", counts.program[CODE_ZERO],
	       counts.program[CODE_EINVAL], counts.program[CODE_ENOTSUP], counts.program[CODE_ENOSPC], counts.program[CODE_OTHER]);
	printf("fuzz violations %lu, mismatches %lu, checks refused with sets not checked %lu\n", counts.violations,
	       totals.mismatches, counts.unchecked);
	for (n = 0UL; n < loaded; n++)
		free(corpus[n].bytes);
	free(corpus);

	return counts.violations != 0UL || totals.mismatches != 0UL;
}

int
main(
	int argc,
	char **argv)
{
	const char *name;
	const char *corpus_path;
	const char *input;
	unsigned long cases;
	uint64_t seed;
	int fuzz;
	int unchecked;
	int i;
	int status;

	name = "stream";
	corpus_path = NULL;
	input = NULL;
	cases = 0UL;
	seed = 1U;
	fuzz = 0;
	unchecked = 0;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
			name = argv[++i];
		} else if (strcmp(argv[i], "--corpus-out") == 0 && i + 1 < argc) {
			corpus_path = argv[++i];
		} else if (strcmp(argv[i], "--fuzz") == 0 && i + 1 < argc) {
			cases = strtoul(argv[++i], NULL, 10);
			fuzz = 1;
		} else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
			seed = strtoull(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--unchecked") == 0) {
			unchecked = 1;
		} else if (fuzz) {
			break;
		} else {
			input = argv[i];
		}
	}
	if (fuzz) {
		memset(&totals, 0, sizeof(totals));
		where.name = "fuzz";
		where.variant = "fuzz";
		status = fuzz_run(cases, seed, unchecked, argc - i, argv + i);
		printf("hevc-slice: %s\n", status == 0 ? "PASS" : "FAIL");
		return status;
	}
	if (input == NULL) {
		fprintf(stderr, "usage: hevc-slice-host-test [--name NAME] [--corpus-out FILE] <bitstream>|-\n"
			"       hevc-slice-host-test --fuzz N --seed S [--unchecked] <corpus>...\n");
		return 2;
	}
	status = check_stream(name, input, corpus_path);
	printf("hevc-slice: %s\n", status == 1 ? "FAIL" : "PASS");

	return status == 1 ? 1 : 0;
}
