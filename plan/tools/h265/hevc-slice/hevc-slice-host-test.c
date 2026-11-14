/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the kernel's H.265 slice segment header parser
 * (src/drivers/gpu/videocore/vk/vulkan-video-hevc.h) against libmedia's
 * conformance-tested H.265 parse.
 *
 * Check mode (default): reads one Annex B bitstream (a file, or the standard
 * input with "-") through h265-stream, h265-dpb (16 slots) and h265-std as
 * vkvideo-probe does, and for every decoded picture
 *   - makes the sequence and picture parameter set, the picture information
 *     and the sixteen slots as the kernel receives them from the StdVideo
 *     structures (the flags packed as libvulkan packs them),
 *   - calls bcm2712_hevc_check,
 *   - reads every slice segment's header with bcm2712_hevc_slice_read (as
 *     the probe lays the segments in the decoder's buffer: a start code
 *     prefix and the NAL unit; again without the prefix and with a longer
 *     prefix and trailing bytes) and compares every field with the header
 *     libmedia reads from the same segment,
 *   - calls bcm2712_hevc_program on all the segments.
 *   hevc-slice-host-test [--name NAME] [--corpus-out FILE] <bitstream>|-
 *
 * Fuzz mode: loads the corpus files the check mode wrote (real segments with
 * their sets) and mutates them: byte flips, truncations, garbage after a valid
 * NAL unit header, emulation prevention patterns, and values of the sets
 * within and outside their ranges.  Nothing is expected but that no call
 * crashes, no sanitizer reports, every call returns and what a call returns
 * satisfies the parser's contract.
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

#include "drivers/gpu/videocore/vk/vulkan-video-hevc.h"

/* The slots of the DPB: the decoder's sixteen reference entries. */
#define TEST_SLOTS 16U

/* The largest bitstream read. */
#define TEST_MAX_INPUT (256U * 1024U * 1024U)

/* The command pairs of the list given to bcm2712_hevc_program. */
#define TEST_PAIRS 262144U

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
};

/* One corpus entry: a real segment with the sets and slots it was read with. */
struct corpus_entry {
	struct bcm2712_hevc_sps sps;
	struct bcm2712_hevc_pps pps;
	struct bcm2712_hevc_picture picture;
	struct bcm2712_hevc_slot slots[TEST_SLOTS];
	struct bcm2712_hevc_slice independent;
	struct bcm2712_hevc_slice first;
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

/*
 * What the fuzz counted: how often each call returned each code, how often
 * the parser's contract was broken, and how many mutations of each kind ran.
 */
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

/*
 * Where the mismatch being reported happened.
 *
 * The check fills it in as it goes, so mismatch() can name the stream, the
 * picture, the segment and the variant without every caller passing them.
 * It lives as long as the process.
 */
static struct where where;

/*
 * What the stream being checked (or the fuzz) found.
 *
 * check_stream clears it at the start of each stream and the fuzz clears it
 * once; mismatch() counts into it, so a zero mismatches count at the end is
 * the pass of the whole run.
 */
static struct totals totals;

/*
 * The state of the fuzz's random number generator.
 *
 * It is set from the seed once per fuzz run and never zero, so the same seed
 * gives the same sequence of mutations.
 */
static uint64_t random_state;

static void mismatch(const char *field, long expected, long got);
static void check_value(const char *field, long expected, long got);
static unsigned pack_sps_flags(const StdVideoH265SpsFlags *flags);
static unsigned pack_pps_flags(const StdVideoH265PpsFlags *flags);
static unsigned pack_picture_flags(const StdVideoDecodeH265PictureInfoFlags *flags);
static void convert_scaling(const StdVideoH265ScalingLists *std, struct bcm2712_hevc_scaling *out);
static void convert_sps(const StdVideoH265SequenceParameterSet *std, struct bcm2712_hevc_sps *out);
static void convert_pps(const StdVideoH265PictureParameterSet *std, struct bcm2712_hevc_pps *out);
static void convert_picture(const StdVideoDecodeH265PictureInfo *std, struct bcm2712_hevc_picture *out);
static void convert_slots(const struct h265_dpb_plan *plan, struct bcm2712_hevc_slot *slots);
static int reference_read(const struct h265_stream *stream, const uint8_t *nal, size_t size, const struct h265sw_slice_header *independent, struct h265sw_slice_header *out, const uint8_t **rbsp);
static size_t epb_count(const uint8_t *nal, size_t upto);
static unsigned expected_list(const struct h265_picture *picture, const struct h265_dpb_plan *plan, unsigned list, unsigned count, int modified, const uint8_t *entries, struct temp_entry *out, unsigned log2_max_poc_lsb);
static void compare_weights(const struct bcm2712_hevc_slice *slice, const struct h265sw_slice_header *header);
static void compare_lists(const struct bcm2712_hevc_slice *slice, const struct h265sw_slice_header *header, const struct h265_picture *picture, const struct h265_dpb_plan *plan, const struct bcm2712_hevc_slot *slots, unsigned log2_max_poc_lsb);
static void compare_slice(const struct bcm2712_hevc_slice *slice, const struct h265sw_slice_header *header, const struct h265_picture *picture, const struct h265_dpb_plan *plan, const struct bcm2712_hevc_slot *slots, const struct bcm2712_hevc_pps *pps, unsigned log2_max_poc_lsb);
static void normalize(const struct bcm2712_hevc_slice *slice, size_t prefix, struct bcm2712_hevc_slice *out);
static int run_program(const struct bcm2712_hevc_sps *sps, const struct bcm2712_hevc_pps *pps, const struct bcm2712_hevc_picture *picture, const struct bcm2712_hevc_slot *slots, const struct bcm2712_hevc_slice *slices, uint32_t count, uint32_t capacity, struct bcm2712_hevc_phase2 *phase2, uint64_t *hash);
static void corpus_write(FILE *file, const struct bcm2712_hevc_sps *sps, const struct bcm2712_hevc_pps *pps, const struct bcm2712_hevc_picture *picture, const struct bcm2712_hevc_slot *slots, const struct bcm2712_hevc_slice *independent, const struct bcm2712_hevc_slice *first, const uint8_t *bytes, size_t length);
static void test_picture(struct h265_stream *stream, const struct h265_picture *picture, const struct h265_dpb_plan *plan, FILE *corpus);
static int read_input(const char *path, uint8_t **data, size_t *size);
static int check_stream(const char *name, const char *path, const char *corpus_path);
static unsigned code_of(int error);
static uint64_t random_next(void);
static uint32_t random_below(uint32_t n);
static uint32_t random_value(uint32_t current);
static void mutate_words(uint32_t *words, size_t count, int flags_first);
static void mutate_picture_info(struct corpus_entry *entry);
static void mutate_slot(struct corpus_entry *entry);
static void mutate_sets(struct corpus_entry *entry);
static size_t mutate_bytes(uint8_t *bytes, size_t length);
static void violation(struct fuzz_counts *counts, unsigned long case_number, const char *what);
static uint32_t ctb_log2_of(const struct bcm2712_hevc_sps *sps);
static uint32_t ctbs_of(const struct bcm2712_hevc_sps *sps, uint32_t log2_ctb);
static void check_contract(struct fuzz_counts *counts, unsigned long case_number, const struct corpus_entry *entry, const struct bcm2712_hevc_slice *slice, size_t length);
static int corpus_load(int count, char **paths, struct corpus_entry **corpus, size_t *loaded);
static void segment_limit_check(const struct corpus_entry *corpus, size_t loaded);
static uint32_t build_program_list(const struct corpus_entry *entry, const struct bcm2712_hevc_slice *slice, uint32_t ctbs, struct bcm2712_hevc_slice *list_slices);
static int fuzz_run(unsigned long cases, uint64_t seed, int unchecked, int count, char **paths);

/*
 * Runs the check of one bitstream, or the fuzz over corpus files.
 *
 * The options are described at the top of the file.  The exit status is 0 for
 * a pass, 1 for a failure and 2 for a usage error.
 */
int
main(
	int argc,
	char **argv)
{
	const char *name;
	const char *corpus_path;
	const char *input;
	const char *verdict;
	unsigned long cases;
	uint64_t seed;
	int fuzz;
	int unchecked;
	int i;
	int status;
	int has_value;
	int name_difference;
	int corpus_difference;
	int fuzz_difference;
	int seed_difference;
	int unchecked_difference;

	/* Starts from the defaults: a stream named "stream", no corpus, no fuzz. */
	name = "stream";
	corpus_path = NULL;
	input = NULL;
	cases = 0UL;
	seed = 1U;
	fuzz = 0;
	unchecked = 0;

	/* Reads the options; the first argument that is not one is the bitstream, or the first corpus file in fuzz mode. */
	for (i = 1; i < argc; i++) {
		/* Compares the argument with each option the test knows. */
		name_difference = strcmp(argv[i], "--name");
		corpus_difference = strcmp(argv[i], "--corpus-out");
		fuzz_difference = strcmp(argv[i], "--fuzz");
		seed_difference = strcmp(argv[i], "--seed");
		unchecked_difference = strcmp(argv[i], "--unchecked");

		/* Finds whether a value follows the argument, for the options that take one. */
		has_value = 0;
		if (i + 1 < argc)
			has_value = 1;

		/* Takes the option the argument names, or the argument itself as an input. */
		if (name_difference == 0 && has_value) {
			/* The name the stream's lines carry. */
			name = argv[++i];
		} else if (corpus_difference == 0 && has_value) {
			/* The file the check appends its corpus to. */
			corpus_path = argv[++i];
		} else if (fuzz_difference == 0 && has_value) {
			/* The number of fuzz cases, which also selects fuzz mode. */
			cases = strtoul(argv[++i], NULL, 10);
			fuzz = 1;
		} else if (seed_difference == 0 && has_value) {
			/* The seed of the fuzz's random numbers. */
			seed = strtoull(argv[++i], NULL, 10);
		} else if (unchecked_difference == 0) {
			/* Lets mutated sets the check refuses reach the parser. */
			unchecked = 1;
		} else if (fuzz) {
			/* The corpus files start here. */
			break;
		} else {
			/* The bitstream to check. */
			input = argv[i];
		}
	}

	/* Runs the fuzz over the corpus files left in the arguments. */
	if (fuzz) {
		/* Clears what the mismatch reports count into and names the fuzz as their place. */
		memset(&totals, 0, sizeof(totals));
		where.name = "fuzz";
		where.variant = "fuzz";

		/* Runs the cases and prints their counts. */
		status = fuzz_run(cases, seed, unchecked, argc - i, argv + i);

		/* Prints the verdict line the run script reads. */
		verdict = "PASS";
		if (status != 0)
			verdict = "FAIL";
		printf("hevc-slice: %s\n", verdict);

		/* Reports whether the fuzz found anything. */
		return status;
	}

	/* Refuses a run with no bitstream to check. */
	if (input == NULL) {
		fprintf(
			stderr,
			"usage: hevc-slice-host-test [--name NAME] [--corpus-out FILE] <bitstream>|-\n"
			"       hevc-slice-host-test --fuzz N --seed S [--unchecked] <corpus>...\n");

		/* The command line was wrong. */
		return 2;
	}

	/* Checks the bitstream and prints the verdict line the run script reads. */
	status = check_stream(name, input, corpus_path);
	verdict = "PASS";
	if (status == 1)
		verdict = "FAIL";
	printf("hevc-slice: %s\n", verdict);

	/* Reports a failed stream as the exit status. */
	if (status == 1)
		return 1;

	/* Succeeded: the stream passed or the decoder refused it as a whole. */
	return 0;
}

/* Reports one mismatch (counted, and printed up to the limit). */
static void
mismatch(
	const char *field,
	long expected,
	long got)
{
	/* Counts the mismatch whether or not it is printed. */
	totals.mismatches++;

	/* Stops printing at the limit, so one bad stream does not flood the output. */
	if (totals.printed >= PRINT_LIMIT)
		return;

	/* Prints the mismatch with the place it happened. */
	totals.printed++;
	printf(
		"MISMATCH %s picture %lu segment %u (%s) %s: expected %ld got %ld\n",
		where.name,
		where.picture,
		where.segment,
		where.variant,
		field,
		expected,
		got);
}

/* Compares one value. */
static void
check_value(
	const char *field,
	long expected,
	long got)
{
	/* Reports the value as a mismatch when the parser's differs from the reference's. */
	if (expected != got)
		mismatch(field, expected, got);
}

/* Packs the sequence parameter set flags, the first declared flag in bit 0 (libvulkan video_h265_sps_flags). */
static unsigned
pack_sps_flags(
	const StdVideoH265SpsFlags *flags)
{
	unsigned word;

	/* Packs the flags of the base specification, bits 0 to 14. */
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

	/* Packs the flags of the range extension, bits 15 to 24. */
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

	/* Packs the flags of the screen content extension, bits 25 to 29. */
	word |= (unsigned)flags->sps_scc_extension_flag << 25;
	word |= (unsigned)flags->sps_curr_pic_ref_enabled_flag << 26;
	word |= (unsigned)flags->palette_mode_enabled_flag << 27;
	word |= (unsigned)flags->sps_palette_predictor_initializers_present_flag << 28;
	word |= (unsigned)flags->intra_boundary_filtering_disabled_flag << 29;

	/* Reports the packed flags. */
	return word;
}

/* Packs the picture parameter set flags, the first declared flag in bit 0 (libvulkan video_h265_pps_flags). */
static unsigned
pack_pps_flags(
	const StdVideoH265PpsFlags *flags)
{
	unsigned word;

	/* Packs the flags of the base specification, bits 0 to 22. */
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

	/* Packs the flags of the range and screen content extensions, bits 23 to 30. */
	word |= (unsigned)flags->cross_component_prediction_enabled_flag << 23;
	word |= (unsigned)flags->chroma_qp_offset_list_enabled_flag << 24;
	word |= (unsigned)flags->pps_curr_pic_ref_enabled_flag << 25;
	word |= (unsigned)flags->residual_adaptive_colour_transform_enabled_flag << 26;
	word |= (unsigned)flags->pps_slice_act_qp_offsets_present_flag << 27;
	word |= (unsigned)flags->pps_palette_predictor_initializers_present_flag << 28;
	word |= (unsigned)flags->monochrome_palette_flag << 29;
	word |= (unsigned)flags->pps_range_extension_flag << 30;

	/* Reports the packed flags. */
	return word;
}

/* Packs the decoded picture's flags, the first declared flag in bit 0 (libvulkan video_h265_picture_flags). */
static unsigned
pack_picture_flags(
	const StdVideoDecodeH265PictureInfoFlags *flags)
{
	unsigned word;

	/* Packs the four flags the decoder reads about the picture. */
	word = 0U;
	word |= (unsigned)flags->IrapPicFlag << 0;
	word |= (unsigned)flags->IdrPicFlag << 1;
	word |= (unsigned)flags->IsReference << 2;
	word |= (unsigned)flags->short_term_ref_pic_set_sps_flag << 3;

	/* Reports the packed flags. */
	return word;
}

/* Copies the scaling lists as coded (the lists h265-std made are the parse's, in coded order). */
static void
convert_scaling(
	const StdVideoH265ScalingLists *std,
	struct bcm2712_hevc_scaling *out)
{
	/* Copies each size's lists and its DC coefficients into a cleared output. */
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
	struct bcm2712_hevc_sps *out)
{
	/* Clears the output and packs the flags. */
	memset(out, 0, sizeof(*out));
	out->flags = pack_sps_flags(&std->flags);

	/* Copies the identity of the set and the size of the picture. */
	out->vps_id = std->sps_video_parameter_set_id;
	out->id = std->sps_seq_parameter_set_id;
	out->chroma_format_idc = (uint32_t)std->chroma_format_idc;
	out->width = std->pic_width_in_luma_samples;
	out->height = std->pic_height_in_luma_samples;
	out->bit_depth_luma_minus8 = std->bit_depth_luma_minus8;
	out->bit_depth_chroma_minus8 = std->bit_depth_chroma_minus8;

	/* Copies the sizes of the coding and transform blocks. */
	out->log2_max_poc_lsb_minus4 = std->log2_max_pic_order_cnt_lsb_minus4;
	out->log2_min_cb_minus3 = std->log2_min_luma_coding_block_size_minus3;
	out->log2_diff_cb = std->log2_diff_max_min_luma_coding_block_size;
	out->log2_min_tb_minus2 = std->log2_min_luma_transform_block_size_minus2;
	out->log2_diff_tb = std->log2_diff_max_min_luma_transform_block_size;
	out->depth_inter = std->max_transform_hierarchy_depth_inter;
	out->depth_intra = std->max_transform_hierarchy_depth_intra;

	/* Copies the counts of the reference picture sets the slice headers may name. */
	out->short_term_sets = std->num_short_term_ref_pic_sets;
	out->long_term_sps = std->num_long_term_ref_pics_sps;

	/* Copies the sample sizes of the PCM blocks. */
	out->pcm_depth_luma_minus1 = std->pcm_sample_bit_depth_luma_minus1;
	out->pcm_depth_chroma_minus1 = std->pcm_sample_bit_depth_chroma_minus1;
	out->log2_min_pcm_minus3 = std->log2_min_pcm_luma_coding_block_size_minus3;
	out->log2_diff_pcm = std->log2_diff_max_min_pcm_luma_coding_block_size;

	/* Copies the scaling lists when the set carries them. */
	if (std->pScalingLists != NULL) {
		out->has_scaling = true;
		convert_scaling(std->pScalingLists, &out->scaling);
	}
}

/* Makes the kernel's picture parameter set from the StdVideo one. */
static void
convert_pps(
	const StdVideoH265PictureParameterSet *std,
	struct bcm2712_hevc_pps *out)
{
	unsigned i;

	/* Clears the output and packs the flags. */
	memset(out, 0, sizeof(*out));
	out->flags = pack_pps_flags(&std->flags);

	/* Copies the identity of the set and the defaults of the slice headers. */
	out->id = std->pps_pic_parameter_set_id;
	out->sps_id = std->pps_seq_parameter_set_id;
	out->vps_id = std->sps_video_parameter_set_id;
	out->extra_bits = std->num_extra_slice_header_bits;
	out->l0_minus1 = std->num_ref_idx_l0_default_active_minus1;
	out->l1_minus1 = std->num_ref_idx_l1_default_active_minus1;

	/* Copies the quantization and deblocking parameters and the tile counts. */
	out->cu_qp_delta_depth = std->diff_cu_qp_delta_depth;
	out->log2_merge_minus2 = std->log2_parallel_merge_level_minus2;
	out->tile_columns_minus1 = std->num_tile_columns_minus1;
	out->tile_rows_minus1 = std->num_tile_rows_minus1;
	out->init_qp_minus26 = std->init_qp_minus26;
	out->cb_qp_offset = std->pps_cb_qp_offset;
	out->cr_qp_offset = std->pps_cr_qp_offset;
	out->beta_div2 = std->pps_beta_offset_div2;
	out->tc_div2 = std->pps_tc_offset_div2;

	/* Copies the widths of the tile columns. */
	for (i = 0U; i < BCM2712_HEVC_COLUMN_WIDTHS; i++)
		out->column_width_minus1[i] = std->column_width_minus1[i];

	/* Copies the heights of the tile rows. */
	for (i = 0U; i < BCM2712_HEVC_ROW_HEIGHTS; i++)
		out->row_height_minus1[i] = std->row_height_minus1[i];

	/* Copies the scaling lists when the set carries them. */
	if (std->pScalingLists != NULL) {
		out->has_scaling = true;
		convert_scaling(std->pScalingLists, &out->scaling);
	}
}

/* Makes the kernel's picture information from the StdVideo one. */
static void
convert_picture(
	const StdVideoDecodeH265PictureInfo *std,
	struct bcm2712_hevc_picture *out)
{
	/* Clears the output and packs the flags. */
	memset(out, 0, sizeof(*out));
	out->flags = pack_picture_flags(&std->flags);

	/* Copies the sets the picture names, its order count and the bits of its short-term set. */
	out->vps_id = std->sps_video_parameter_set_id;
	out->sps_id = std->pps_seq_parameter_set_id;
	out->pps_id = std->pps_pic_parameter_set_id;
	out->delta_pocs = std->NumDeltaPocsOfRefRpsIdx;
	out->short_term_bits = std->NumBitsForSTRefPicSetInSlice;
	out->poc = std->PicOrderCntVal;

	/* Copies the three reference sets the slices build their lists from. */
	memcpy(out->before, std->RefPicSetStCurrBefore, sizeof(out->before));
	memcpy(out->after, std->RefPicSetStCurrAfter, sizeof(out->after));
	memcpy(out->long_term, std->RefPicSetLtCurr, sizeof(out->long_term));
}

/* Makes the slots: each slot an active reference of the plan holds (its order count and whether it is long-term). */
static void
convert_slots(
	const struct h265_dpb_plan *plan,
	struct bcm2712_hevc_slot *slots)
{
	unsigned i;
	int slot;

	/* Clears every slot, so a slot no reference holds is absent. */
	memset(slots, 0, TEST_SLOTS * sizeof(*slots));

	/* Fills the slots the plan's active references hold. */
	for (i = 0U; i < plan->reference_count && i < H265_DPB_PICTURES; i++) {
		/* Skips a reference whose slot the decoder does not have. */
		slot = plan->reference_slot[i];
		if (slot < 0 || (unsigned)slot >= TEST_SLOTS)
			continue;

		/* Records the reference's order count and whether it is long-term. */
		slots[slot].present = true;
		slots[slot].long_term = false;
		if (plan->reference_long[i] != 0U)
			slots[slot].long_term = true;
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

	/* Splits the NAL unit into its header and its RBSP. */
	error = h265sw_nal_parse_prefix(nal, size, H265_STREAM_SLICE_BYTES, buffer, sizeof(buffer), &info);
	if (error != 0)
		return error;

	/* Reads the start of the header, up to the picture parameter set it names. */
	vcodec_bitreader_init(&bits, info.rbsp, info.rbsp_size);
	error = h265sw_slice_read_start(out, &bits, &info);
	if (error != 0)
		return error;

	/* Refuses a slice that names a picture parameter set the stream did not carry. */
	if (out->pps_id >= H265SW_PPS_IDS || !stream->store.pps_present[out->pps_id])
		return EINVAL;

	/* Finds the sets the slice names and hands the RBSP back to the caller. */
	pps = &stream->store.pps[out->pps_id];
	sps = &stream->store.sps[pps->sps_id];
	*rbsp = info.rbsp;

	/* Reads the rest of the header with those sets. */
	error = h265sw_slice_read_rest(out, &bits, sps, pps, independent);
	if (error != 0)
		return error;

	/* Succeeded: the header is in *out. */
	return 0;
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

	/* Walks the payload, counting a three that follows two zero bytes. */
	count = 0U;
	zeros = 0U;
	for (i = 2U; i < upto; i++) {
		/* Counts the emulation prevention byte, which also ends the run of zero bytes. */
		if (zeros >= 2U && nal[i] == 3U) {
			count++;
			zeros = 0U;
			continue;
		}

		/* Extends the run of zero bytes, or ends it at any other byte. */
		if (nal[i] == 0U) {
			zeros++;
		} else {
			zeros = 0U;
		}
	}

	/* Reports the emulation prevention bytes found. */
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
	unsigned set_length;
	const int *set_slots;
	const int32_t *set_pocs;

	/* The width of the order count's low part is not needed to build the list: compare_lists uses it. */
	(void)log2_max_poc_lsb;

	/* Counts the pictures of the three sets the list is built from. */
	rps = &picture->rps;
	total = plan->num_st_before + plan->num_st_after + plan->num_lt_curr;

	/* Builds no list when the sets are empty or larger than the temporary list holds. */
	if (total == 0U || total > 48U)
		return 0U;

	/* Sizes the temporary list: the entries wanted, or all the sets when they are more. */
	length = count;
	if (total > length)
		length = total;

	/* Fills the temporary list with the sets in the list's order, repeated until it is full. */
	used = 0U;
	while (used < length) {
		/* Takes the three sets once, the one this list starts with first. */
		for (set = 0U; set < 3U && used < length; set++) {
			/* Selects the long-term set third, and the two short-term sets in the order of the list. */
			if (set == 2U) {
				set_length = plan->num_lt_curr;
				set_slots = plan->lt_curr;
				set_pocs = rps->lt_curr;
			} else if ((set == 0U) == (list == 0U)) {
				set_length = plan->num_st_before;
				set_slots = plan->st_before;
				set_pocs = rps->st_before;
			} else {
				set_length = plan->num_st_after;
				set_slots = plan->st_after;
				set_pocs = rps->st_after;
			}

			/* Appends the set's pictures with the slot, the order count and whether only its low part is known. */
			for (i = 0U; i < set_length && used < length; i++) {
				temp[used].slot = set_slots[i];
				temp[used].poc = set_pocs[i];
				temp[used].lsb_only = 0;
				if (set == 2U && rps->lt_curr_msb[i] == 0U)
					temp[used].lsb_only = 1;
				used++;
			}
		}
	}

	/* Picks the list's entries from the temporary list, by list_entry when the list is modified. */
	for (i = 0U; i < count; i++) {
		/* Takes the entry the list names, or the next one when the list is not modified. */
		entry = i;
		if (modified)
			entry = entries[i];

		/* Marks an entry beyond the temporary list as a slot nothing has. */
		if (entry >= length) {
			out[i].slot = -2;
			out[i].poc = 0;
			out[i].lsb_only = 0;
			continue;
		}

		/* Copies the temporary list's entry. */
		out[i] = temp[entry];
	}

	/* Reports the entries made. */
	return count;
}

/* Compares the weights of an explicit-weights slice with libmedia's. */
static void
compare_weights(
	const struct bcm2712_hevc_slice *slice,
	const struct h265sw_slice_header *header)
{
	const struct h265sw_weights *weights;
	char field[64];
	unsigned list;
	unsigned lists;
	unsigned i;
	unsigned component;

	/* Compares the denominators the weights share. */
	weights = &header->weights;
	check_value("weights.luma_log2_denom", (long)weights->luma_log2_denom, (long)slice->weights.luma_log2_denom);
	check_value("weights.chroma_log2_denom", (long)weights->chroma_log2_denom, (long)slice->weights.chroma_log2_denom);

	/* Counts the lists a slice of this type has: two for a B slice, else one. */
	lists = 1U;
	if (header->slice_type == H265SW_SLICE_B)
		lists = 2U;

	/* Compares the weights and offsets of every entry of each list. */
	for (list = 0U; list < lists; list++) {
		for (i = 0U; i < header->num_ref_idx[list] && i < H265SW_MAX_REFS; i++) {
			/* Compares the luma weight and offset of the entry. */
			sprintf(field, "weights.luma_weight[%u][%u]", list, i);
			check_value(field, (long)weights->luma_weight[list][i], (long)slice->weights.luma_weight[list][i]);
			sprintf(field, "weights.luma_offset[%u][%u]", list, i);
			check_value(field, (long)weights->luma_offset[list][i], (long)slice->weights.luma_offset[list][i]);

			/* Compares the weight and offset of each chroma component of the entry. */
			for (component = 0U; component < 2U; component++) {
				sprintf(field, "weights.chroma_weight[%u][%u][%u]", list, i, component);
				check_value(field, (long)weights->chroma_weight[list][i][component], (long)slice->weights.chroma_weight[list][i][component]);
				sprintf(field, "weights.chroma_offset[%u][%u][%u]", list, i, component);
				check_value(field, (long)weights->chroma_offset[list][i][component], (long)slice->weights.chroma_offset[list][i][component]);
			}
		}
	}
}

/* Compares the reference lists as slots, and by order count through the slots. */
static void
compare_lists(
	const struct bcm2712_hevc_slice *slice,
	const struct h265sw_slice_header *header,
	const struct h265_picture *picture,
	const struct h265_dpb_plan *plan,
	const struct bcm2712_hevc_slot *slots,
	unsigned log2_max_poc_lsb)
{
	struct temp_entry expected[BCM2712_HEVC_SLOTS];
	char field[64];
	unsigned list;
	unsigned lists;
	unsigned i;
	unsigned made;
	unsigned slot;
	int32_t poc;
	int32_t mask;

	/* Counts the lists a slice of this type has: two for a B slice, else one. */
	lists = 1U;
	if (header->slice_type == H265SW_SLICE_B)
		lists = 2U;

	/* Compares the number of pictures the slice's lists draw from. */
	check_value("num_pic_total_curr", (long)(plan->num_st_before + plan->num_st_after + plan->num_lt_curr), (long)header->num_pic_total_curr);

	/* Compares each list with the one built from the sets. */
	for (list = 0U; list < lists; list++) {
		/* Builds the expected list and refuses a list of the wrong length. */
		made = expected_list(picture, plan, list, header->num_ref_idx[list], header->list_mod[list], header->list_entry[list], expected, log2_max_poc_lsb);
		if (made != header->num_ref_idx[list]) {
			mismatch("list built from the sets", (long)header->num_ref_idx[list], (long)made);
			continue;
		}

		/* Compares the slot and the order count of every entry. */
		for (i = 0U; i < made; i++) {
			/* Compares the slot of the entry. */
			slot = slice->list[list][i];
			sprintf(field, "list[%u][%u] slot", list, i);
			check_value(field, (long)expected[i].slot, (long)slot);

			/* Leaves the order count of a slot the decoder does not have. */
			if (slot >= TEST_SLOTS)
				continue;

			/* Compares the order count the slot holds with the entry's. */
			poc = slots[slot].poc;
			sprintf(field, "list[%u][%u] poc", list, i);
			if (expected[i].lsb_only) {
				/* A long-term entry without its high part is known by the low part only. */
				mask = (int32_t)((1U << log2_max_poc_lsb) - 1U);
				check_value(field, (long)expected[i].poc, (long)(poc & mask));
			} else {
				/* The entry's whole order count. */
				check_value(field, (long)expected[i].poc, (long)poc);
			}
		}
	}
}

/* Compares every field of the parser's segment with libmedia's header of it. */
static void
compare_slice(
	const struct bcm2712_hevc_slice *slice,
	const struct h265sw_slice_header *header,
	const struct h265_picture *picture,
	const struct h265_dpb_plan *plan,
	const struct bcm2712_hevc_slot *slots,
	const struct bcm2712_hevc_pps *pps,
	unsigned log2_max_poc_lsb)
{
	unsigned expected_merge;
	int inter;
	int b_slice;
	int weighted;

	/* Tells the slice types apart: an inter slice is P or B. */
	inter = 0;
	if (header->slice_type != H265SW_SLICE_I)
		inter = 1;
	b_slice = 0;
	if (header->slice_type == H265SW_SLICE_B)
		b_slice = 1;

	/* Compares what names the segment and where it starts. */
	check_value("nal_type", (long)header->nal_type, (long)slice->nal_type);
	check_value("temporal_id_plus1", (long)header->temporal_id + 1L, (long)slice->temporal_id_plus1);
	check_value("first_in_picture", header->first_in_pic != 0, slice->first_in_picture);
	check_value("dependent", header->dependent != 0, slice->dependent);
	check_value("address", (long)header->address, (long)slice->address);
	check_value("slice_type", (long)header->slice_type, (long)slice->slice_type);

	/* Compares the tools the slice switches on and its reference counts. */
	check_value("sao_luma", header->sao_luma != 0, slice->sao_luma);
	check_value("sao_chroma", header->sao_chroma != 0, slice->sao_chroma);
	check_value("temporal_mvp", header->temporal_mvp != 0, slice->temporal_mvp);
	check_value("references[0]", (long)header->num_ref_idx[0], (long)slice->references[0]);
	check_value("references[1]", (long)header->num_ref_idx[1], (long)slice->references[1]);
	check_value("mvd_l1_zero", header->mvd_l1_zero != 0, slice->mvd_l1_zero);
	check_value("cabac_init", header->cabac_init != 0, slice->cabac_init);
	check_value("collocated_from_l0", header->collocated_from_l0 != 0, slice->collocated_from_l0);
	check_value("collocated_ref_idx", (long)header->collocated_ref_idx, (long)slice->collocated_ref_idx);

	/* Compares the merge candidates, which an intra slice does not have. */
	expected_merge = 0U;
	if (inter)
		expected_merge = header->max_merge_cand;
	check_value("max_merge_candidates", (long)expected_merge, (long)slice->max_merge_candidates);

	/* Compares the quantization parameters. */
	check_value("qp", (long)header->qp, (long)slice->qp);
	check_value("cb_qp_offset", (long)header->cb_qp_offset, (long)slice->cb_qp_offset);
	check_value("cr_qp_offset", (long)header->cr_qp_offset, (long)slice->cr_qp_offset);

	/* Compares the deblocking switch, and its offsets when the filter is on. */
	check_value("deblocking_disabled", header->deblocking_disabled != 0, slice->deblocking_disabled);
	if (header->deblocking_disabled == 0) {
		check_value("beta_div2", (long)header->beta_offset_div2, (long)slice->beta_div2);
		check_value("tc_div2", (long)header->tc_offset_div2, (long)slice->tc_div2);
	}

	/* Compares the filter across slices and the entry points. */
	check_value("loop_filter_across_slices", header->loop_filter_across_slices != 0, slice->loop_filter_across_slices);
	check_value("entry_points", (long)header->num_entry_points, (long)slice->entry_points);

	/* Decides whether the picture asks for weights for this slice type: P by weighted_pred, B by weighted_bipred. */
	weighted = 0;
	if (!b_slice && inter && (pps->flags & BCM2712_HEVC_PPS_WEIGHTED) != 0U) {
		weighted = 1;
	} else if (b_slice && (pps->flags & BCM2712_HEVC_PPS_BIPRED) != 0U) {
		weighted = 1;
	}

	/* Compares the weights, when the picture asks for them for this type. */
	check_value("weighted", weighted != 0, slice->weighted);
	if (weighted && slice->weighted)
		compare_weights(slice, header);

	/* Compares the lists of an inter slice. */
	if (inter)
		compare_lists(slice, header, picture, plan, slots, log2_max_poc_lsb);
}

/* Copies a segment with the position-dependent values taken out (the data's offset by the prefix, no bus address). */
static void
normalize(
	const struct bcm2712_hevc_slice *slice,
	size_t prefix,
	struct bcm2712_hevc_slice *out)
{
	/* Copies the segment and clears what depends on where the bytes were put. */
	*out = *slice;
	out->data_offset -= prefix;
	out->data_bytes = 0U;
	out->segment_physical = 0U;
}

/* Runs bcm2712_hevc_program with a command list of the capacity, and hashes the pairs it wrote. */
static int
run_program(
	const struct bcm2712_hevc_sps *sps,
	const struct bcm2712_hevc_pps *pps,
	const struct bcm2712_hevc_picture *picture,
	const struct bcm2712_hevc_slot *slots,
	const struct bcm2712_hevc_slice *slices,
	uint32_t count,
	uint32_t capacity,
	struct bcm2712_hevc_phase2 *phase2,
	uint64_t *hash)
{
	static uint32_t pairs[2U * TEST_PAIRS];
	struct bcm2712_hevc_commands commands;
	uint32_t i;
	int error;

	/* Gives the program an empty command list of the capacity and a cleared phase 2 record. */
	memset(&commands, 0, sizeof(commands));
	commands.pairs = pairs;
	commands.capacity = capacity;
	memset(phase2, 0, sizeof(*phase2));

	/* Programs the decoder from the sets, the slots and the segments. */
	error = bcm2712_hevc_program(sps, pps, picture, slots, slices, count, &commands, phase2);

	/* Hashes the pairs written and their count, to compare two runs. */
	*hash = 1469598103934665603ULL;
	for (i = 0U; i < 2U * commands.count && i < 2U * TEST_PAIRS; i++)
		*hash = (*hash ^ pairs[i]) * 1099511628211ULL;
	*hash = (*hash ^ commands.count) * 1099511628211ULL;

	/* Refuses a success whose command list is overflowed, empty or longer than its capacity. */
	if (error == 0 &&
	    (commands.overflow ||
	     commands.count == 0U ||
	     commands.count > capacity)) {
		mismatch("program commands.overflow/count", 0, (long)commands.count);
		return EINVAL;
	}

	/* Expects ENOSPC only with the overflow flag set. */
	if (error == ENOSPC && !commands.overflow)
		mismatch("program ENOSPC without overflow", 1, 0);

	/* Reports why the program failed. */
	if (error != 0)
		return error;

	/* Succeeded: the command list holds the program. */
	return 0;
}

/* Writes one corpus entry. */
static void
corpus_write(
	FILE *file,
	const struct bcm2712_hevc_sps *sps,
	const struct bcm2712_hevc_pps *pps,
	const struct bcm2712_hevc_picture *picture,
	const struct bcm2712_hevc_slot *slots,
	const struct bcm2712_hevc_slice *independent,
	const struct bcm2712_hevc_slice *first,
	const uint8_t *bytes,
	size_t length)
{
	struct corpus_entry entry;

	/* Copies the sets, the picture and the slots the segment was read with. */
	memset(&entry, 0, sizeof(entry));
	entry.sps = *sps;
	entry.pps = *pps;
	entry.picture = *picture;
	memcpy(entry.slots, slots, sizeof(entry.slots));

	/* Copies the independent segment the dependent one needs, when there is one. */
	if (independent != NULL) {
		entry.independent = *independent;
		entry.has_independent = 1U;
	}

	/* Copies the picture's first segment, when it is not the segment itself. */
	if (first != NULL) {
		entry.first = *first;
		entry.has_first = 1U;
	}

	/* Keeps at most the first bytes of the segment, where the header is. */
	if (length > CORPUS_BYTES)
		length = CORPUS_BYTES;
	entry.length = (uint32_t)length;
	entry.bytes = NULL;

	/* Writes the record and the segment's bytes. */
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
	static struct bcm2712_hevc_sps sps;
	static struct bcm2712_hevc_pps pps;
	static struct bcm2712_hevc_picture bcm_picture;
	static struct bcm2712_hevc_slot slots[TEST_SLOTS];
	static struct bcm2712_hevc_slice slices[H265SW_MAX_SLICE_SEGMENTS];
	static uint8_t buffer[16U + H265_STREAM_SLICE_BYTES];
	static uint8_t variant_buffer[16U + H265_STREAM_SLICE_BYTES];
	static const uint8_t trailer[] = { 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x72, 0x00, 0x00, 0x03, 0x00 };
	struct h265sw_slice_header header;
	struct h265sw_slice_header independent_header;
	struct h265sw_slice_header *previous_header;
	struct bcm2712_hevc_slice normal;
	struct bcm2712_hevc_slice other;
	struct bcm2712_hevc_slice other_normal;
	struct bcm2712_hevc_slice *slice;
	struct bcm2712_hevc_slice *independent;
	struct bcm2712_hevc_slice *corpus_independent;
	struct bcm2712_hevc_slice *first;
	struct bcm2712_hevc_phase2 phase2;
	struct bcm2712_hevc_picture hole_picture;
	struct bcm2712_hevc_slice hole_slice;
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
	uint64_t physical;
	uint32_t log2_ctb;
	uint32_t ctb_rows;
	unsigned i;
	unsigned count;
	unsigned expected_collocated;
	int error;
	int difference;
	int have_independent;
	int all_read;
	int sampled;
	int hole_unchanged;
	int wavefront;
	int tiles;

	/* Counts the picture and names it in the mismatch reports. */
	totals.pictures++;
	where.picture = (unsigned long)picture->index;
	where.segment = 0U;
	where.variant = "picture";

	/* Finds the sets the picture uses in libmedia's store. */
	libmedia_pps = &stream->store.pps[picture->pps_id];
	libmedia_sps = &stream->store.sps[picture->sps_id];

	/* Makes the StdVideo picture information; a picture libmedia cannot describe is not tested. */
	error = h265_std_picture(picture, plan, &std_picture);
	if (error != 0) {
		totals.std_refused++;
		return;
	}

	/* Makes the StdVideo sequence parameter set; one libmedia cannot describe is not tested. */
	error = h265_std_sps(libmedia_sps, &std_sps);
	if (error != 0) {
		totals.std_refused++;
		return;
	}

	/* Makes the StdVideo picture parameter set; one libmedia cannot describe is not tested. */
	error = h265_std_pps(libmedia_pps, libmedia_sps, &std_pps);
	if (error != 0) {
		totals.std_refused++;
		return;
	}

	/* Converts the StdVideo structures into the forms the kernel gets. */
	convert_sps(&std_sps.sps, &sps);
	convert_pps(&std_pps.pps, &pps);
	convert_picture(&std_picture, &bcm_picture);
	convert_slots(plan, slots);

	/* Counts a picture with a reference the DPB did not find. */
	if (plan->missing != 0U)
		totals.missing_pictures++;

	/* Asks whether the decoder takes the pair of sets; a refusal ends the test of this picture. */
	error = bcm2712_hevc_check(&sps, &pps);
	if (error == ENOTSUP) {
		totals.check_refused++;
		return;
	}

	/* Reports a pair the decoder calls invalid, which a conformant stream must not give. */
	if (error != 0) {
		totals.check_invalid++;
		mismatch("bcm2712_hevc_check", 0, (long)error);
		return;
	}

	/* Counts the picture as tested and notes the tools its picture parameter set switches on. */
	totals.pictures_tested++;
	wavefront = 0;
	if ((pps.flags & BCM2712_HEVC_PPS_ENTROPY_SYNC) != 0U)
		wavefront = 1;
	tiles = 0;
	if ((pps.flags & BCM2712_HEVC_PPS_TILES) != 0U)
		tiles = 1;

	/* Starts the walk over the picture's segments, at most as many as the parser's list holds. */
	have_independent = 0;
	memset(&independent_header, 0, sizeof(independent_header));
	independent = NULL;
	first = NULL;
	all_read = 1;
	physical = 0x40000000ULL;
	count = picture->slice_count;
	if (count > H265SW_MAX_SLICE_SEGMENTS)
		count = H265SW_MAX_SLICE_SEGMENTS;

	/* Compares each segment as read by the parser and by libmedia. */
	for (i = 0U; i < count; i++) {
		/* Names the segment in the mismatch reports. */
		where.segment = i;
		nal = stream->data + picture->slice_offset[i];
		nal_size = picture->slice_size[i];
		totals.segments++;

		/* Reads the segment's header with libmedia; a dependent segment takes the independent one's fields. */
		previous_header = NULL;
		if (have_independent)
			previous_header = &independent_header;
		error = reference_read(stream, nal, nal_size, previous_header, &header, &rbsp);
		if (error != 0) {
			where.variant = "libmedia";
			totals.read_refused++;
			mismatch("libmedia read of a segment of a returned picture", 0, (long)error);
			all_read = 0;
			continue;
		}

		/* Compares what the picture record says about its first segment. */
		if (i == 0U) {
			where.variant = "libmedia";
			check_value("picture.header.slice_type", (long)picture->header.slice_type, (long)header.slice_type);
			check_value("picture.header.data_offset", (long)picture->header.data_offset, (long)header.data_offset);
		}

		/* Skips a segment larger than the decoder's buffer holds. */
		if (nal_size > H265_STREAM_SLICE_BYTES) {
			all_read = 0;
			continue;
		}

		/* Lays the segment out as the probe does: a three-byte start code prefix and the NAL unit. */
		buffer[0] = 0U;
		buffer[1] = 0U;
		buffer[2] = 1U;
		memcpy(buffer + 3U, nal, nal_size);
		length = 3U + nal_size;
		slice = &slices[i];

		/* Reads the segment with the parser under test. */
		where.variant = "start code 3";
		error = bcm2712_hevc_slice_read(buffer, length, &sps, &pps, &bcm_picture, slots, independent, slice);
		if (error != 0) {
			mismatch("bcm2712_hevc_slice_read return", 0, (long)error);
			all_read = 0;

			/* Keeps libmedia's independent header for the dependent segments that follow. */
			if (!header.dependent) {
				have_independent = 1;
				independent_header = header;
			}

			continue;
		}

		/* Counts the segment's first read. */
		totals.segments_variants++;

		/* Compares every field, the data's size and its place in the bytes. */
		compare_slice(slice, &header, picture, plan, slots, &pps, libmedia_sps->log2_max_poc_lsb);
		check_value("data_bytes", (long)(length - slice->data_offset), (long)slice->data_bytes);
		offset_in_nal = slice->data_offset - 3U;
		check_value(
			"data_offset (raw minus emulation prevention bytes, from the RBSP)",
			(long)header.data_offset,
			(long)(offset_in_nal - 2U - epb_count(nal, offset_in_nal)));

		/* Compares the first data byte of the segment. */
		if (header.data_offset < (size_t)(nal_size) && offset_in_nal < nal_size)
			check_value("first data byte", (long)rbsp[header.data_offset], (long)nal[offset_in_nal]);

		/* Counts the segment's slice type. */
		if (header.slice_type == H265SW_SLICE_I)
			totals.type_i++;
		else if (header.slice_type == H265SW_SLICE_P)
			totals.type_p++;
		else
			totals.type_b++;

		/* Counts a dependent segment. */
		if (header.dependent)
			totals.dependent++;

		/* Counts a segment with explicit weights. */
		if (slice->weighted)
			totals.weighted++;

		/* Counts a segment with entry points. */
		if (header.num_entry_points != 0U)
			totals.entry_points++;

		/* Counts a segment whose lists are modified. */
		if (header.list_mod[0] != 0 || header.list_mod[1] != 0)
			totals.modified++;

		/* Counts a segment that names long-term pictures. */
		if (header.num_long_term != 0U)
			totals.long_term++;

		/* Reads the same segment as a raw NAL unit, without the start code. */
		normalize(slice, 3U, &normal);
		where.variant = "no start code";
		error = bcm2712_hevc_slice_read(buffer + 3U, nal_size, &sps, &pps, &bcm_picture, slots, independent, &other);
		if (error != 0) {
			mismatch("bcm2712_hevc_slice_read return", 0, (long)error);
		} else {
			totals.segments_variants++;
			check_value("data_bytes", (long)(nal_size - other.data_offset), (long)other.data_bytes);

			/* Expects the same segment as the start code variant gave, whatever the prefix. */
			normalize(&other, 0U, &other_normal);
			difference = memcmp(&normal, &other_normal, sizeof(normal));
			if (difference != 0)
				mismatch("slice differs from the start code variant", 0, 1);
		}

		/* Reads the same segment with a four-byte start code and bytes after it. */
		extra = sizeof(trailer);
		variant_buffer[0] = 0U;
		variant_buffer[1] = 0U;
		variant_buffer[2] = 0U;
		variant_buffer[3] = 1U;
		memcpy(variant_buffer + 4U, nal, nal_size);
		memcpy(variant_buffer + 4U + nal_size, trailer, extra);
		where.variant = "start code 4 and trailing bytes";
		error = bcm2712_hevc_slice_read(variant_buffer, 4U + nal_size + extra, &sps, &pps, &bcm_picture, slots, independent, &other);
		if (error != 0) {
			mismatch("bcm2712_hevc_slice_read return", 0, (long)error);
		} else {
			totals.segments_variants++;
			check_value("data_bytes", (long)(4U + nal_size + extra - other.data_offset), (long)other.data_bytes);

			/* Expects the same segment as the start code variant gave, whatever the prefix. */
			normalize(&other, 4U, &other_normal);
			difference = memcmp(&normal, &other_normal, sizeof(normal));
			if (difference != 0)
				mismatch("slice differs from the start code variant", 0, 1);
		}

		/*
		 * A picture with a missing reference has no-slot entries in its sets (libvulkan sends 0xff where the DPB
		 * found nothing).  The first current entry made missing: the segment's header, whose fields do not depend
		 * on the lists, should read the same or be refused (informational: counted, not a mismatch).
		 */
		if (i == 0U &&
		    header.slice_type != H265SW_SLICE_I &&
		    plan->num_st_before + plan->num_st_after + plan->num_lt_curr >= 2U) {
			/* Makes the first current entry of the sets a missing one. */
			hole_picture = bcm_picture;
			if (hole_picture.before[0] != BCM2712_HEVC_NO_SLOT) {
				hole_picture.before[0] = BCM2712_HEVC_NO_SLOT;
			} else if (hole_picture.after[0] != BCM2712_HEVC_NO_SLOT) {
				hole_picture.after[0] = BCM2712_HEVC_NO_SLOT;
			} else {
				hole_picture.long_term[0] = BCM2712_HEVC_NO_SLOT;
			}

			/* Reads the first segment against the picture with the hole. */
			totals.hole_variants++;
			error = bcm2712_hevc_slice_read(buffer, length, &sps, &pps, &hole_picture, slots, independent, &hole_slice);

			/* Decides whether the header read the same, differently, or was refused. */
			hole_unchanged = 0;
			if (error == 0 &&
			    hole_slice.data_offset == slice->data_offset &&
			    hole_slice.qp == slice->qp &&
			    hole_slice.max_merge_candidates == slice->max_merge_candidates &&
			    hole_slice.cb_qp_offset == slice->cb_qp_offset &&
			    hole_slice.cr_qp_offset == slice->cr_qp_offset &&
			    hole_slice.entry_points == slice->entry_points &&
			    hole_slice.collocated_ref_idx == slice->collocated_ref_idx) {
				hole_unchanged = 1;
			}

			/* Counts the outcome: refused, read the same, or read differently. */
			if (error != 0) {
				totals.hole_rejected++;
			} else if (hole_unchanged) {
				totals.hole_same++;
			} else {
				totals.hole_differs++;

				/* Prints the first two differences as notes. */
				if (totals.hole_differs <= 2U) {
					printf(
						"NOTE %s picture %lu: with the first current entry of the sets 0xff, the header reads qp %d (expected %d), "
						"data_offset %lu (expected %lu), max_merge %u (expected %u), sets before %u after %u long-term %u\n",
						where.name,
						where.picture,
						hole_slice.qp,
						slice->qp,
						(unsigned long)hole_slice.data_offset,
						(unsigned long)slice->data_offset,
						hole_slice.max_merge_candidates,
						slice->max_merge_candidates,
						plan->num_st_before,
						plan->num_st_after,
						plan->num_lt_curr);
				}
			}
		}

		/* Places the segment at the bus address the executor lays them out at: 64-byte aligned. */
		slice->segment_physical = physical;
		physical += (length + 63U) & ~(size_t)63U;

		/* Takes this segment as the next ones' independent segment when it is one. */
		if (!header.dependent) {
			have_independent = 1;
			independent_header = header;
			independent = slice;
		}

		/* Remembers the picture's first segment. */
		if (i == 0U)
			first = slice;

		/* Decides whether the segment goes into the corpus: the first segments of a picture sampled, and the other kinds. */
		sampled = 0;
		if (picture->index % 7U == 0U && i < 2U) {
			sampled = 1;
		} else if (header.dependent) {
			sampled = 1;
		} else if (header.num_entry_points != 0U && picture->index % 3U == 0U) {
			sampled = 1;
		} else if (slice->weighted && picture->index % 2U == 0U) {
			sampled = 1;
		} else if (header.num_long_term != 0U) {
			sampled = 1;
		} else if (header.list_mod[0] != 0 || header.list_mod[1] != 0) {
			sampled = 1;
		}

		/* Writes the segment with the sets it was read with, when the corpus wants it and has room. */
		if (corpus != NULL && totals.corpus < CORPUS_PER_STREAM && sampled) {
			corpus_independent = NULL;
			if (header.dependent)
				corpus_independent = independent;
			corpus_write(corpus, &sps, &pps, &bcm_picture, slots, corpus_independent, first, buffer, length);
			totals.corpus++;
		}
	}

	/* Leaves a picture whose segments were not all read; the program needs them all. */
	if (!all_read)
		return;

	/* Names the program in the mismatch reports and counts its run. */
	where.segment = 0U;
	where.variant = "program";
	totals.program_runs++;

	/* Expects a refusal when the picture has more segments than the program's list counts. */
	if (count > BCM2712_HEVC_SLICES) {
		error = run_program(&sps, &pps, &bcm_picture, slots, slices, count, TEST_PAIRS, &phase2, &hash1);
		totals.program_too_many++;

		/* Reports an answer other than the refusal. */
		if (error != EINVAL) {
			totals.program_failures++;
			mismatch("program with more segments than the list counts: return", (long)EINVAL, (long)error);
		}

		/* Ends the test of the picture: the refusal was all this program run was for. */
		return;
	}

	/* Runs the program over all the segments. */
	error = run_program(&sps, &pps, &bcm_picture, slots, slices, count, TEST_PAIRS, &phase2, &hash1);
	if (error != 0) {
		totals.program_failures++;
		mismatch("program return", 0, (long)error);
		return;
	}

	/* Compares the values of phase 2 with the sets: the frame size, the order count and the CTB rows. */
	log2_ctb = sps.log2_min_cb_minus3 + 3U + sps.log2_diff_cb;
	ctb_rows = (sps.height + ((1U << log2_ctb) - 1U)) >> log2_ctb;
	check_value("phase2.frame_size", (long)((sps.height << 16) | sps.width), (long)phase2.frame_size);
	check_value("phase2.current_poc", (long)(uint32_t)picture->poc, (long)phase2.current_poc);
	check_value("phase2.ctb_rows", (long)ctb_rows, (long)phase2.ctb_rows);
	check_value("phase2.write_motion", (sps.flags & BCM2712_HEVC_SPS_TEMPORAL_MVP) != 0U, phase2.write_motion);

	/* Finds the collocated slot the last slice with temporal prediction names. */
	expected_collocated = BCM2712_HEVC_NO_SLOT;
	for (i = 0U; i < count; i++) {
		/* Takes the slot from the list the slice's collocated picture is in: list 0, or list 1 of a B slice. */
		if (slices[i].temporal_mvp && slices[i].slice_type != BCM2712_HEVC_SLICE_I) {
			if (slices[i].collocated_from_l0 || slices[i].slice_type != BCM2712_HEVC_SLICE_B) {
				expected_collocated = slices[i].list[0][slices[i].collocated_ref_idx];
			} else {
				expected_collocated = slices[i].list[1][slices[i].collocated_ref_idx];
			}
		}
	}

	/* Compares the collocated slot the slices named. */
	check_value("phase2.collocated_slot", (long)expected_collocated, (long)phase2.collocated_slot);

	/* Counts a picture that uses tiles. */
	if (tiles)
		totals.tiles++;

	/* Counts a picture that uses wavefront parallel processing. */
	if (wavefront)
		totals.wavefront++;

	/* Counts a picture whose sequence uses scaling lists. */
	if ((sps.flags & BCM2712_HEVC_SPS_SCALING) != 0U)
		totals.scaling++;

	/* Runs the same picture again after a smaller list (ENOSPC or not), and after the previous call's state: the same commands. */
	if (picture->index % 2U == 0U) {
		/* Expects the three-pair list to overflow. */
		error = run_program(&sps, &pps, &bcm_picture, slots, slices, count, 3U, &phase2, &hash2);
		if (error != ENOSPC) {
			totals.program_failures++;
			mismatch("program with a three-pair list: return", (long)ENOSPC, (long)error);
		}

		/* Expects the full list again to give the commands of the first run. */
		error = run_program(&sps, &pps, &bcm_picture, slots, slices, count, TEST_PAIRS, &phase2, &hash3);
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
	int difference;

	/* Opens the input: the standard input for "-", else the file. */
	file = stdin;
	difference = strcmp(path, "-");
	if (difference != 0)
		file = fopen(path, "rb");

	/* Reports why the file could not be opened. */
	if (file == NULL)
		return errno;

	/* Allocates the buffer the whole input is read into. */
	buffer = malloc(TEST_MAX_INPUT);
	if (buffer == NULL)
		return ENOMEM;

	/* Reads until the end of the input or the limit. */
	length = 0U;
	do {
		got = fread(buffer + length, 1U, TEST_MAX_INPUT - length, file);
		length += got;
	} while (got != 0U && length < TEST_MAX_INPUT);

	/* Closes the file, but not the standard input. */
	if (file != stdin)
		fclose(file);

	/* Hands the bytes read to the caller. */
	*data = buffer;
	*size = length;

	/* Succeeded: the caller owns the buffer. */
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
	const char *verdict;
	FILE *corpus;
	uint8_t *data;
	size_t size;
	unsigned count;
	unsigned i;
	int error;
	int refused;
	int failed;
	int no_rasl_output;

	/* Clears the totals and names the stream in the mismatch reports. */
	memset(&totals, 0, sizeof(totals));
	where.name = name;
	where.picture = 0UL;
	where.segment = 0U;
	where.variant = "stream";

	/* Reads the whole bitstream. */
	data = NULL;
	size = 0U;
	error = read_input(path, &data, &size);
	if (error != 0) {
		printf("stream %s: FAIL (cannot read the input: %d)\n", name, error);
		return 1;
	}

	/* Allocates the stream reader. */
	stream = calloc(1U, sizeof(*stream));
	if (stream == NULL) {
		printf("stream %s: FAIL (out of memory)\n", name);
		return 1;
	}

	/* Allocates the picture record the reader fills. */
	picture = calloc(1U, sizeof(*picture));
	if (picture == NULL) {
		printf("stream %s: FAIL (out of memory)\n", name);
		return 1;
	}

	/* Opens the corpus the segments are appended to, when one is asked for. */
	corpus = NULL;
	if (corpus_path != NULL)
		corpus = fopen(corpus_path, "ab");

	/* Starts the reader over the bytes and the DPB with the decoder's sixteen slots. */
	h265_stream_init(stream);
	h265_stream_input(stream, data, size);
	h265_dpb_init(&dpb, TEST_SLOTS);

	/* Tests every picture the stream gives. */
	for (;;) {
		/* Takes the next picture; ENOENT is the end of the stream. */
		error = h265_stream_next(stream, picture);
		if (error == ENOENT)
			break;

		/* Skips a picture the reader could not make. */
		if (error != 0)
			continue;

		/* Plans the picture's place in the DPB. */
		sps = &stream->store.sps[picture->sps_id];
		no_rasl_output = 0;
		if (picture->irap != 0 && picture->no_rasl_output != 0)
			no_rasl_output = 1;
		error = h265_dpb_begin(
			&dpb,
			sps,
			&picture->rps,
			no_rasl_output,
			picture->no_output_of_prior_pics,
			&plan);
		if (error != 0) {
			totals.dpb_skipped++;

			/* Releases the slots of the pictures the plan sends out, when it still names them. */
			if (error == EINVAL) {
				for (i = 0U; i < plan.output_count; i++)
					h265_dpb_release(&dpb, plan.output[i].slot);
			}

			continue;
		}

		/* Releases the slots of the pictures the plan sends out. */
		for (i = 0U; i < plan.output_count; i++)
			h265_dpb_release(&dpb, plan.output[i].slot);

		/* Takes a free slot for the picture. */
		error = h265_dpb_take_slot(&dpb, &plan);
		if (error != 0) {
			totals.dpb_skipped++;
			h265_dpb_abort(&dpb);
			continue;
		}

		/* Tests the picture. */
		test_picture(stream, picture, &plan, corpus);

		/* Completes the picture in the DPB and takes the pictures it sends out. */
		error = h265_dpb_end(&dpb, &plan, picture->poc, picture->pic_output, picture->index, output, &count);
		if (error != 0) {
			h265_dpb_abort(&dpb);
			continue;
		}

		/* Releases the slots of the pictures sent out. */
		for (i = 0U; i < count; i++)
			h265_dpb_release(&dpb, output[i].slot);
	}

	/* Flushes the DPB at the end of the stream and releases the slots it sends out. */
	error = h265_dpb_flush(&dpb, output, &count);
	if (error == 0) {
		for (i = 0U; i < count; i++)
			h265_dpb_release(&dpb, output[i].slot);
	}

	/* Closes the corpus. */
	if (corpus != NULL)
		fclose(corpus);

	/* Frees the reader, the picture record and the bitstream. */
	h265_stream_destroy(stream);
	free(picture);
	free(stream);
	free(data);

	/* A stream refused whole (every picture the decoder refuses) is not a failure. */
	refused = 0;
	if (totals.check_refused > 0U &&
	    totals.pictures_tested == 0U &&
	    totals.check_invalid == 0U)
		refused = 1;

	/* Decides whether the stream failed: a mismatch, a failed program, an invalid pair, or no picture at all. */
	failed = 0;
	if (totals.mismatches != 0U) {
		failed = 1;
	} else if (totals.program_failures != 0U) {
		failed = 1;
	} else if (totals.check_invalid != 0U) {
		failed = 1;
	} else if (totals.pictures == 0U) {
		failed = 1;
	}

	/* Names the verdict of the stream; a refusal takes precedence. */
	verdict = "PASS";
	if (failed)
		verdict = "FAIL";
	if (refused)
		verdict = "REFUSED";

	/* Prints the stream's summary line. */
	printf(
		"stream %s: %s pictures %lu tested %lu segments %lu variants %lu mismatches %lu check_refused %lu check_invalid %lu "
		"std_refused %lu dpb_skipped %lu program_runs %lu program_failures %lu missing_pictures %lu\n",
		name,
		verdict,
		totals.pictures,
		totals.pictures_tested,
		totals.segments,
		totals.segments_variants,
		totals.mismatches,
		totals.check_refused,
		totals.check_invalid,
		totals.std_refused,
		totals.dpb_skipped,
		totals.program_runs,
		totals.program_failures,
		totals.missing_pictures);

	/* Prints what the stream covered, when any picture was tested. */
	if (totals.pictures_tested != 0U) {
		printf(
			"  coverage: I %lu P %lu B %lu dependent %lu weighted %lu entry_points %lu list_modification %lu long_term %lu "
			"tiles %lu wavefront %lu scaling %lu too_many_segments %lu corpus %lu\n",
			totals.type_i,
			totals.type_p,
			totals.type_b,
			totals.dependent,
			totals.weighted,
			totals.entry_points,
			totals.modified,
			totals.long_term,
			totals.tiles,
			totals.wavefront,
			totals.scaling,
			totals.program_too_many,
			totals.corpus);
	}

	/* Prints how the header read with a missing reference, when that was tried. */
	if (totals.hole_variants != 0U) {
		printf(
			"  missing-reference variant of the first segment: %lu, refused %lu, header read the same %lu, header read differently %lu\n",
			totals.hole_variants,
			totals.hole_rejected,
			totals.hole_same,
			totals.hole_differs);
	}

	/* Makes the lines visible before the next stream's run. */
	fflush(stdout);

	/* Reports a stream refused whole as the distinct status 2. */
	if (refused)
		return 2;

	/* Reports a failed stream. */
	if (failed)
		return 1;

	/* Succeeded: the stream passed. */
	return 0;
}

/* Reports the counter slot of a return code. */
static unsigned
code_of(
	int error)
{
	/* Reports the slot of each code the fuzz counts, and the slot of any other. */
	if (error == 0)
		return CODE_ZERO;
	if (error == EINVAL)
		return CODE_EINVAL;
	if (error == ENOTSUP)
		return CODE_ENOTSUP;
	if (error == ENOSPC)
		return CODE_ENOSPC;

	/* Any other code goes to the last slot. */
	return CODE_OTHER;
}

/* Steps the fuzz's random numbers: xorshift64*, from the seed. */
static uint64_t
random_next(void)
{
	/* Advances the state by the xorshift steps. */
	random_state ^= random_state >> 12;
	random_state ^= random_state << 25;
	random_state ^= random_state >> 27;

	/* Reports the state scrambled by the multiplier. */
	return random_state * 2685821657736338717ULL;
}

/* A random number below n (n above 0). */
static uint32_t
random_below(
	uint32_t n)
{
	uint64_t draw;

	/* Draws the next number of the sequence. */
	draw = random_next();

	/* Reports the draw reduced to below n, from its upper bits. */
	return (uint32_t)((draw >> 16) % n);
}

/* A random value for a 32-bit field of a set: near the current value, small, extreme or random. */
static uint32_t
random_value(
	uint32_t current)
{
	uint32_t value;
	uint32_t kind;
	uint32_t bit;

	/* Draws the kind of value to make. */
	kind = random_below(8U);

	/* Makes the value of that kind. */
	switch (kind) {
	case 0:
		value = random_below(8U);
		break;
	case 1:
		value = random_below(70U);
		break;
	case 2:
		value = current + 1U;
		break;
	case 3:
		value = current - 1U;
		break;
	case 4:
		value = 0xffffffffU;
		break;
	case 5:
		value = 0x80000000U;
		break;
	case 6:
		bit = random_below(32U);
		value = current ^ (1U << bit);
		break;
	default:
		value = (uint32_t)random_next();
		break;
	}

	/* Reports the value made. */
	return value;
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
	unsigned bit;
	size_t at;

	/* Draws how many words to change. */
	n = 1U + random_below(5U);

	/* Changes that many words. */
	for (i = 0U; i < n; i++) {
		/* Picks the word to change. */
		at = random_below((uint32_t)count);

		/* Flips one flag of the first word (never the top bit), or gives any other word a new value. */
		if (at == 0U && flags_first) {
			bit = random_below(31U);
			words[0] ^= 1U << bit;
		} else {
			words[at] = random_value(words[at]);
		}
	}
}

/* Changes the picture information of a corpus entry: its flags, its other words or an entry of the three sets. */
static void
mutate_picture_info(
	struct corpus_entry *entry)
{
	uint32_t *words;
	uint8_t *set_bytes;
	unsigned at;
	unsigned bit;
	unsigned choice;
	unsigned roll;

	/* Draws which kind of change to make: a flag flip, a word, or an entry of the sets. */
	roll = random_below(3U);
	if (roll == 0U) {
		/* Flips one of the four flags. */
		bit = random_below(4U);
		entry->picture.flags ^= 1U << bit;
	} else {
		/* Draws between a new word and an entry of the sets. */
		roll = random_below(2U);
		if (roll == 0U) {
			/* Gives one of the seven words after the flags a new value. */
			words = (uint32_t *)&entry->picture.flags;
			at = random_below(7U);
			words[at] = random_value(words[at]);
		} else {
			/* Changes one entry of the three sets, which are laid out one after another. */
			set_bytes = (uint8_t *)entry->picture.before;
			at = random_below(3U * BCM2712_HEVC_SET_ENTRIES);

			/* Makes the entry no slot, a slot, any byte or a slot beyond the decoder's. */
			choice = random_below(4U);
			switch (choice) {
			case 0:
				set_bytes[at] = BCM2712_HEVC_NO_SLOT;
				break;
			case 1:
				set_bytes[at] = (uint8_t)random_below(BCM2712_HEVC_SLOTS);
				break;
			case 2:
				set_bytes[at] = (uint8_t)random_below(256U);
				break;
			default:
				set_bytes[at] = (uint8_t)(BCM2712_HEVC_SLOTS + random_below(4U));
				break;
			}
		}
	}
}

/* Changes one slot of a corpus entry: whether it is present, whether it is long-term, or its order count. */
static void
mutate_slot(
	struct corpus_entry *entry)
{
	unsigned at;
	unsigned choice;

	/* Picks the slot and the change to make. */
	at = random_below(BCM2712_HEVC_SLOTS);
	choice = random_below(3U);

	/* Makes the change. */
	switch (choice) {
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
}

/* Changes a few things of the sets and slots of a corpus entry: the values within and outside their ranges. */
static void
mutate_sets(
	struct corpus_entry *entry)
{
	unsigned n;
	unsigned i;
	unsigned which;
	unsigned coin;

	/* Draws how many things to change. */
	n = 1U + random_below(4U);

	/* Changes that many things. */
	for (i = 0U; i < n; i++) {
		/* Draws what to change: a set, the picture information, a slot, or whether a set has scaling lists. */
		which = random_below(5U);
		if (which == 0U) {
			/* The words of the sequence parameter set before its scaling lists. */
			mutate_words(&entry->sps.flags, offsetof(struct bcm2712_hevc_sps, has_scaling) / sizeof(uint32_t), 1);
		} else if (which == 1U) {
			/* The words of the picture parameter set before its scaling lists. */
			mutate_words(&entry->pps.flags, offsetof(struct bcm2712_hevc_pps, has_scaling) / sizeof(uint32_t), 1);
		} else if (which == 2U) {
			/* The picture information. */
			mutate_picture_info(entry);
		} else if (which == 3U) {
			/* One slot. */
			mutate_slot(entry);
		} else {
			/* Whether the sequence or the picture parameter set carries scaling lists. */
			coin = random_below(2U);
			if (coin == 0U) {
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
	unsigned roll;
	unsigned special;
	uint32_t limit;
	size_t at;
	size_t span;
	size_t new_length;

	/* Leaves an empty segment as it is. */
	if (length == 0U)
		return 0U;

	/* Draws the kind of mutation. */
	mode = random_below(6U);
	new_length = length;
	if (mode == 0U || mode == 1U) {
		/* Flips bits (mode 0) or sets byte values (mode 1), most of them in the header. */
		n = 1U + random_below(6U);
		for (i = 0U; i < n; i++) {
			/* Draws the byte, in the first 48 bytes most of the time. */
			span = length;
			roll = random_below(10U);
			if (roll < 7U && span > 48U)
				span = 48U;
			at = random_below((uint32_t)span);

			/* Changes the byte: a bit flip, a special value or a random one. */
			if (mode == 0U) {
				bytes[at] ^= (uint8_t)(1U << random_below(8U));
			} else {
				roll = random_below(2U);
				if (roll == 0U) {
					special = random_below((uint32_t)sizeof(specials));
					bytes[at] = specials[special];
				} else {
					bytes[at] = (uint8_t)random_below(256U);
				}
			}
		}
	} else if (mode == 2U) {
		/* A truncation. */
		new_length = random_below((uint32_t)length + 1U);
	} else if (mode == 3U) {
		/* A valid NAL unit header (after a start code if there is one), then random bytes of random length. */
		at = 0U;
		if (length >= 3U && bytes[0] == 0U && bytes[1] == 0U && bytes[2] == 1U)
			at = 3U;

		/* Does nothing to a segment too short for a NAL unit header. */
		if (length >= at + 2U) {
			/* Fills the bytes after the header with special values or random ones. */
			span = at + 2U + random_below(300U);
			for (i = (unsigned)(at + 2U); i < span && i < FUZZ_BYTES; i++) {
				roll = random_below(3U);
				if (roll == 0U) {
					bytes[i] = (uint8_t)random_below(256U);
				} else {
					special = random_below((uint32_t)sizeof(specials));
					bytes[i] = specials[special];
				}
			}

			/* Takes the filled bytes as the segment, to the buffer's capacity. */
			new_length = span;
			if (new_length >= FUZZ_BYTES)
				new_length = FUZZ_BYTES;
		}
	} else if (mode == 4U) {
		/* An emulation prevention pattern in the header, which shifts what follows. */
		limit = (uint32_t)length;
		if (length >= 64U)
			limit = 64U;
		at = random_below(limit);
		if (at + 3U < length) {
			bytes[at] = 0U;
			bytes[at + 1U] = 0U;
			roll = random_below(2U);
			if (roll == 0U) {
				bytes[at + 2U] = 3U;
			} else {
				bytes[at + 2U] = (uint8_t)random_below(4U);
			}
		}
	} else {
		/* Random bytes in place of the header past the NAL unit header, all of them. */
		for (i = 5U; i < length && i < 64U; i++)
			bytes[i] = (uint8_t)random_below(256U);
	}

	/* Reports the length after the mutation. */
	return new_length;
}

/* Reports a violation of the parser's contract by something a call returned. */
static void
violation(
	struct fuzz_counts *counts,
	unsigned long case_number,
	const char *what)
{
	/* Counts the violation. */
	counts->violations++;

	/* Prints the violation up to the limit. */
	if (counts->violations <= PRINT_LIMIT)
		printf("VIOLATION case %lu: %s\n", case_number, what);
}

/* Reports the log2 of the coding tree block size of a sequence parameter set. */
static uint32_t
ctb_log2_of(
	const struct bcm2712_hevc_sps *sps)
{
	/* Reports the smallest coding block's log2 plus the difference up to the largest. */
	return sps->log2_min_cb_minus3 + 3U + sps->log2_diff_cb;
}

/* Reports the number of coding tree blocks of a picture of the sequence parameter set's size. */
static uint32_t
ctbs_of(
	const struct bcm2712_hevc_sps *sps,
	uint32_t log2_ctb)
{
	/* Reports the columns times the rows, each size rounded up to whole blocks. */
	return ((sps->width + (1U << log2_ctb) - 1U) >> log2_ctb) * ((sps->height + (1U << log2_ctb) - 1U) >> log2_ctb);
}

/* Checks what a successful slice_read returned against the parser's contract. */
static void
check_contract(
	struct fuzz_counts *counts,
	unsigned long case_number,
	const struct corpus_entry *entry,
	const struct bcm2712_hevc_slice *slice,
	size_t length)
{
	uint32_t log2_ctb;
	uint32_t ctbs;
	uint32_t list;
	uint32_t collocated_list;
	uint32_t i;

	/* Requires the data to lie inside the bytes and to run to their end. */
	if (slice->data_offset >= length || slice->data_bytes != length - slice->data_offset)
		violation(counts, case_number, "data_offset or data_bytes outside the bytes");

	/* Requires a slice type of the three the standard has. */
	if (slice->slice_type > BCM2712_HEVC_SLICE_I)
		violation(counts, case_number, "slice_type above 2");

	/* Requires each list to have at most the reference count the standard allows. */
	if (slice->references[0] > BCM2712_HEVC_REFERENCES || slice->references[1] > BCM2712_HEVC_REFERENCES)
		violation(counts, case_number, "references above 15");

	/* Requires the quantization parameter in its range. */
	if (slice->qp < 0 || slice->qp > 51)
		violation(counts, case_number, "qp outside 0..51");

	/* Requires the chroma quantization offsets in their range. */
	if (slice->cb_qp_offset < -12 ||
	    slice->cb_qp_offset > 12 ||
	    slice->cr_qp_offset < -12 ||
	    slice->cr_qp_offset > 12)
		violation(counts, case_number, "chroma qp offset outside -12..12");

	/* Requires the deblocking offsets in their range. */
	if (slice->beta_div2 < -6 ||
	    slice->beta_div2 > 6 ||
	    slice->tc_div2 < -6 ||
	    slice->tc_div2 > 6)
		violation(counts, case_number, "deblocking offset outside -6..6");

	/* Requires an inter slice to have one to five merge candidates. */
	if (slice->slice_type != BCM2712_HEVC_SLICE_I &&
	    (slice->max_merge_candidates < 1U ||
	     slice->max_merge_candidates > 5U))
		violation(counts, case_number, "max_merge_candidates outside 1..5");

	/* Requires every entry of the lists to name a slot that is present. */
	for (list = 0U; list < 2U; list++) {
		for (i = 0U; i < slice->references[list]; i++) {
			/* A dependent segment's lists are the independent one's, made with the slots before the mutation. */
			if (slice->list[list][i] >= BCM2712_HEVC_SLOTS ||
			    (!slice->dependent && !entry->slots[slice->list[list][i]].present))
				violation(counts, case_number, "a list entry is not a present slot");
		}
	}

	/* Requires the segment's address to lie inside the picture, for a block size the decoder supports. */
	log2_ctb = ctb_log2_of(&entry->sps);
	if (log2_ctb >= 4U && log2_ctb <= 6U) {
		ctbs = ctbs_of(&entry->sps, log2_ctb);
		if (slice->address >= ctbs)
			violation(counts, case_number, "address at or beyond the picture's CTBs");
	}

	/* Requires the collocated picture's index to lie inside its list. */
	if (slice->slice_type != BCM2712_HEVC_SLICE_I) {
		/* The collocated picture is in list 0 when collocated_from_l0 is set, else in list 1. */
		collocated_list = 1U;
		if (slice->collocated_from_l0)
			collocated_list = 0U;
		if (slice->collocated_ref_idx >= slice->references[collocated_list] &&
		    slice->temporal_mvp)
			violation(counts, case_number, "collocated_ref_idx beyond the list");
	}
}

/* Loads the corpus files into one array of entries; returns 0, or 1 when a file cannot be opened or nothing was loaded. */
static int
corpus_load(
	int count,
	char **paths,
	struct corpus_entry **corpus,
	size_t *loaded)
{
	struct corpus_entry entry;
	struct corpus_entry *grown;
	FILE *file;
	uint8_t *keep;
	size_t capacity;
	size_t got;
	size_t keep_size;
	int path;

	/* Starts with an empty array. */
	*corpus = NULL;
	*loaded = 0U;
	capacity = 0U;

	/* Reads the entries of every file. */
	for (path = 0; path < count; path++) {
		/* Opens the corpus file. */
		file = fopen(paths[path], "rb");
		if (file == NULL) {
			printf("fuzz: cannot open %s\n", paths[path]);
			return 1;
		}

		/* Reads the entries of the file, each followed by its bytes, until one is incomplete. */
		for (;;) {
			/* Reads the record of the entry. */
			got = fread(&entry, sizeof(entry), 1U, file);
			if (got != 1U)
				break;

			/* Stops at a record whose length no entry of this test has. */
			if (entry.length > CORPUS_BYTES)
				break;

			/* Allocates the entry's bytes; an empty entry still gets one byte. */
			keep_size = entry.length;
			if (keep_size == 0U)
				keep_size = 1U;
			keep = malloc(keep_size);
			if (keep == NULL)
				return 1;

			/* Reads the entry's bytes. */
			got = fread(keep, 1U, entry.length, file);
			if (got != entry.length)
				break;

			/* Doubles the array when it is full, starting at 1024 entries. */
			if (*loaded == capacity) {
				if (capacity == 0U) {
					capacity = 1024U;
				} else {
					capacity = capacity * 2U;
				}

				/* Grows the array to the new capacity. */
				grown = realloc(*corpus, capacity * sizeof(**corpus));
				if (grown == NULL)
					return 1;
				*corpus = grown;
			}

			/* Appends the entry with its bytes. */
			(*corpus)[*loaded] = entry;
			(*corpus)[*loaded].bytes = keep;
			(*loaded)++;
		}

		/* Closes the corpus file. */
		fclose(file);
	}

	/* Refuses a corpus with no entry. */
	if (*loaded == 0U) {
		printf("fuzz: no corpus entries\n");
		return 1;
	}

	/* Succeeded: the entries are in the array. */
	return 0;
}

/* Runs the program over a picture of 256, 257, 264 and no segments: only 256 are taken, 257 and more are refused whole. */
static void
segment_limit_check(
	const struct corpus_entry *corpus,
	size_t loaded)
{
	static struct bcm2712_hevc_slice many[BCM2712_HEVC_SLICES + 8U];
	struct corpus_entry entry;
	struct bcm2712_hevc_phase2 phase2;
	unsigned long corpus_index;
	unsigned limit_entries;
	uint64_t hash;
	uint32_t log2_ctb;
	uint32_t ctbs;
	uint32_t k;
	int error;

	/* Walks the corpus for pictures large enough to hold the segments, at most 24 of them. */
	limit_entries = 0U;
	for (corpus_index = 0UL; corpus_index < (unsigned long)loaded && limit_entries < 24U; corpus_index++) {
		entry = corpus[corpus_index];

		/* Skips an entry without the picture's first segment, whose fields the segments copy. */
		if (entry.has_first == 0U)
			continue;

		/* Skips an entry that uses tiles. */
		if ((entry.pps.flags & BCM2712_HEVC_PPS_TILES) != 0U)
			continue;

		/* Skips an entry whose pair of sets the decoder does not take. */
		error = bcm2712_hevc_check(&entry.sps, &entry.pps);
		if (error != 0)
			continue;

		/* Skips a picture with too few coding tree blocks to give 300 distinct addresses. */
		log2_ctb = ctb_log2_of(&entry.sps);
		ctbs = ctbs_of(&entry.sps, log2_ctb);
		if (ctbs < 300U)
			continue;

		/* Makes the segments: copies of the first one at the addresses 0, 1, 2 and so on. */
		for (k = 0U; k < BCM2712_HEVC_SLICES + 8U; k++) {
			many[k] = entry.first;
			many[k].address = k;
			many[k].first_in_picture = false;
			if (k == 0U)
				many[k].first_in_picture = true;
			many[k].dependent = false;
		}

		/* Names the run in the mismatch reports. */
		where.name = "fuzz";
		where.variant = "segment count limit";

		/* Expects the program to take 256 segments. */
		error = run_program(&entry.sps, &entry.pps, &entry.picture, entry.slots, many, BCM2712_HEVC_SLICES, TEST_PAIRS, &phase2, &hash);
		if (error != 0)
			mismatch("program with 256 segments: return", 0, (long)error);

		/* Expects the program to refuse 257 segments. */
		error = run_program(&entry.sps, &entry.pps, &entry.picture, entry.slots, many, BCM2712_HEVC_SLICES + 1U, TEST_PAIRS, &phase2, &hash);
		if (error != EINVAL)
			mismatch("program with 257 segments: return", (long)EINVAL, (long)error);

		/* Expects the program to refuse 264 segments. */
		error = run_program(&entry.sps, &entry.pps, &entry.picture, entry.slots, many, BCM2712_HEVC_SLICES + 8U, TEST_PAIRS, &phase2, &hash);
		if (error != EINVAL)
			mismatch("program with 264 segments: return", (long)EINVAL, (long)error);

		/* Expects the program to refuse a picture with no segment. */
		error = run_program(&entry.sps, &entry.pps, &entry.picture, entry.slots, many, 0U, TEST_PAIRS, &phase2, &hash);
		if (error != EINVAL)
			mismatch("program with no segment: return", (long)EINVAL, (long)error);

		/* Counts the picture. */
		limit_entries++;
	}

	/* Prints how many pictures the limit was checked with. */
	printf("fuzz: segment count limit checked with %u pictures\n", limit_entries);
}

/* Fills the list of segments the program is run over, a way the case chose; returns the number of segments. */
static uint32_t
build_program_list(
	const struct corpus_entry *entry,
	const struct bcm2712_hevc_slice *slice,
	uint32_t ctbs,
	struct bcm2712_hevc_slice *list_slices)
{
	unsigned long remaining;
	uint32_t segments;
	uint32_t previous;
	uint32_t choice;
	uint32_t step_limit;
	uint32_t step;
	uint32_t coin;

	/* Draws the shape of the list: the segment alone, after the picture's first, twice, or a run. */
	segments = 0U;
	choice = random_below(4U);
	if (choice == 0U) {
		/* The segment alone. */
		list_slices[segments++] = *slice;
	} else if (choice == 1U && entry->has_first != 0U) {
		/* The picture's first segment, then this one. */
		list_slices[segments++] = entry->first;
		list_slices[segments++] = *slice;
	} else if (choice == 2U && entry->has_first != 0U) {
		/* The picture's first segment, then this one twice (an address that does not advance). */
		list_slices[segments++] = entry->first;
		list_slices[segments++] = *slice;
		list_slices[segments++] = *slice;
	} else {
		/* A run of segments at increasing random addresses, as the parser would return them. */
		list_slices[segments] = *slice;
		if (entry->has_first != 0U)
			list_slices[segments] = entry->first;
		list_slices[segments].address = 0U;
		list_slices[segments].first_in_picture = true;
		list_slices[segments].dependent = false;
		segments++;

		/* Adds up to 63 segments, each at a random address after the one before. */
		remaining = 1U + random_below(63U);
		previous = 0U;
		while (remaining != 0UL && previous + 1U < ctbs) {
			/* Advances the address by a random step that leaves room for the segments still to come. */
			step_limit = (ctbs - previous - 1U) / (unsigned)remaining + 1U;
			step = random_below(step_limit);
			previous += 1U + step;

			/* Stops at an address beyond the picture. */
			if (previous >= ctbs)
				break;

			/* Adds the segment, dependent one time in three. */
			list_slices[segments] = *slice;
			list_slices[segments].address = previous;
			list_slices[segments].first_in_picture = false;
			coin = random_below(3U);
			list_slices[segments].dependent = false;
			if (coin == 0U)
				list_slices[segments].dependent = true;
			segments++;
			remaining--;
		}
	}

	/* Reports the number of segments in the list. */
	return segments;
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
	static uint8_t bytes[FUZZ_BYTES];
	struct corpus_entry *corpus;
	struct corpus_entry entry;
	struct corpus_entry *base;
	struct bcm2712_hevc_slice slice;
	struct bcm2712_hevc_slice list_slices[64];
	struct bcm2712_hevc_phase2 phase2;
	struct fuzz_counts counts;
	const struct bcm2712_hevc_slice *independent_segment;
	const char *note;
	size_t length;
	size_t loaded;
	unsigned long corpus_index;
	unsigned long case_number;
	unsigned long reads;
	uint64_t hash;
	uint32_t ctbs;
	uint32_t log2_ctb;
	uint32_t segments;
	uint32_t k;
	uint32_t draw;
	uint32_t program_capacity;
	unsigned code;
	uint32_t capacities[6];
	int set_mutation;
	int error;
	int checked;
	int use_independent;
	int dependent_none;

	/* Lists the capacities the program is run with: none, one pair, a few, and the full list. */
	capacities[0] = 0U;
	capacities[1] = 1U;
	capacities[2] = 7U;
	capacities[3] = 100U;
	capacities[4] = 4096U;
	capacities[5] = TEST_PAIRS;

	/* Loads the corpus files. */
	error = corpus_load(count, paths, &corpus, &loaded);
	if (error != 0)
		return 1;

	/* Prints what the fuzz runs over. */
	note = "";
	if (unchecked)
		note = ", sets not checked";
	printf(
		"fuzz: %lu corpus segments, %lu cases, seed %llu%s\n",
		(unsigned long)loaded,
		cases,
		(unsigned long long)seed,
		note);

	/* Checks the most segments of a picture: 256 are taken, 257 and more are refused whole. */
	segment_limit_check(corpus, loaded);

	/* Seeds the random numbers; a zero state would stay zero. */
	memset(&counts, 0, sizeof(counts));
	random_state = seed * 0x9e3779b97f4a7c15ULL + 0x1234567ULL;
	if (random_state == 0U)
		random_state = 1U;
	reads = 0U;

	/* Runs the cases. */
	for (case_number = 0UL; case_number < cases; case_number++) {
		/* Takes a corpus entry and its bytes as the case's starting point. */
		counts.cases++;
		draw = random_below((uint32_t)loaded);
		base = &corpus[draw];
		entry = *base;
		length = entry.length;
		memcpy(bytes, base->bytes, length);

		/* Draws what changes: the bytes, the sets, both or the independent segment. */
		k = random_below(10U);
		set_mutation = 0;
		if (k >= 4U)
			set_mutation = 1;

		/* Mutates the bytes in six cases of ten. */
		if (k < 6U) {
			length = mutate_bytes(bytes, length);
			counts.byte_mutations++;
		}

		/* Mutates the sets when the draw chose to. */
		if (set_mutation) {
			mutate_sets(&entry);
			counts.set_mutations++;
		}

		/* Draws whether the parser gets no independent segment, though the entry has one. */
		draw = random_below(8U);
		dependent_none = 0;
		if (draw == 0U)
			dependent_none = 1;

		/* Checks the mutated sets before the parser, as the executor does. */
		checked = 1;
		if (set_mutation) {
			error = bcm2712_hevc_check(&entry.sps, &entry.pps);
			code = code_of(error);
			counts.check[code]++;
			if (error != 0 && !unchecked)
				checked = 0;
			if (error != 0 && unchecked)
				counts.unchecked++;
		}

		/* Leaves a case whose sets the check refused. */
		if (!checked)
			continue;

		/* Chooses the independent segment the parser gets: the entry's, or none. */
		use_independent = 0;
		if (entry.has_independent != 0U && !dependent_none)
			use_independent = 1;
		independent_segment = NULL;
		if (use_independent)
			independent_segment = &entry.independent;

		/* Reads the segment with the parser over a poisoned output. */
		memset(&slice, 0xa5, sizeof(slice));
		error = bcm2712_hevc_slice_read(bytes, length, &entry.sps, &entry.pps, &entry.picture, entry.slots, independent_segment, &slice);
		code = code_of(error);
		counts.read[code]++;
		reads++;

		/* Reports a return code the parser's contract does not have. */
		if (error != 0 && error != EINVAL && error != ENOTSUP)
			violation(&counts, case_number, "slice_read returned a code other than 0, EINVAL, ENOTSUP");

		/* Leaves a case the parser refused. */
		if (error != 0)
			continue;

		/* Checks what the parser returned against its contract, when the sets were checked. */
		if (!unchecked)
			check_contract(&counts, case_number, &entry, &slice, length);

		/* Leaves a case whose block size the decoder does not support. */
		log2_ctb = ctb_log2_of(&entry.sps);
		if (log2_ctb < 4U || log2_ctb > 6U)
			continue;

		/* Leaves a picture without coding tree blocks. */
		ctbs = ctbs_of(&entry.sps, log2_ctb);
		if (ctbs == 0U)
			continue;

		/* Makes the list of segments for the program: the segment alone (when it starts a picture) or after the picture's first, or a longer run. */
		segments = build_program_list(&entry, &slice, ctbs, list_slices);

		/* Places the segments one after another at 64-byte steps from the first bus address. */
		list_slices[0].segment_physical = 0x40000000ULL;
		for (k = 1U; k < segments; k++)
			list_slices[k].segment_physical = 0x40000000ULL + 64U * k;

		/* Runs the program over the list with a capacity drawn from the list above, and counts its answer. */
		where.name = "fuzz";
		draw = random_below(6U);
		program_capacity = capacities[draw];
		error = run_program(&entry.sps, &entry.pps, &entry.picture, entry.slots, list_slices, segments, program_capacity, &phase2, &hash);
		code = code_of(error);
		counts.program[code]++;
		counts.program_runs++;

		/* Reports a return code the program's contract does not have. */
		if (error != 0 && error != EINVAL && error != ENOSPC)
			violation(&counts, case_number, "program returned a code other than 0, EINVAL, ENOSPC");
	}

	/* Prints the counts of the run. */
	printf(
		"fuzz cases %lu: byte mutations %lu, set mutations %lu, reads %lu, program runs %lu\n",
		counts.cases,
		counts.byte_mutations,
		counts.set_mutations,
		reads,
		counts.program_runs);
	printf(
		"fuzz check returns: 0 %lu EINVAL %lu ENOTSUP %lu ENOSPC %lu other %lu\n",
		counts.check[CODE_ZERO],
		counts.check[CODE_EINVAL],
		counts.check[CODE_ENOTSUP],
		counts.check[CODE_ENOSPC],
		counts.check[CODE_OTHER]);
	printf(
		"fuzz slice_read returns: 0 %lu EINVAL %lu ENOTSUP %lu ENOSPC %lu other %lu\n",
		counts.read[CODE_ZERO],
		counts.read[CODE_EINVAL],
		counts.read[CODE_ENOTSUP],
		counts.read[CODE_ENOSPC],
		counts.read[CODE_OTHER]);
	printf(
		"fuzz program returns: 0 %lu EINVAL %lu ENOTSUP %lu ENOSPC %lu other %lu\n",
		counts.program[CODE_ZERO],
		counts.program[CODE_EINVAL],
		counts.program[CODE_ENOTSUP],
		counts.program[CODE_ENOSPC],
		counts.program[CODE_OTHER]);
	printf(
		"fuzz violations %lu, mismatches %lu, checks refused with sets not checked %lu\n",
		counts.violations,
		totals.mismatches,
		counts.unchecked);

	/* Frees the corpus. */
	for (corpus_index = 0UL; corpus_index < (unsigned long)loaded; corpus_index++)
		free(corpus[corpus_index].bytes);
	free(corpus);

	/* Reports a contract violation as a failure. */
	if (counts.violations != 0UL)
		return 1;

	/* Reports a mismatch as a failure. */
	if (totals.mismatches != 0UL)
		return 1;

	/* Succeeded: the fuzz found nothing. */
	return 0;
}
