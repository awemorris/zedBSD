/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests of h265sw-ps: profile_tier_level() (7.3.3), the video, sequence
 * and picture parameter sets (7.3.2.1 to 7.3.2.3, 7.4.3), the check of a
 * PPS against its SPS (7.4.3.3.1) and the store.
 *
 * The inputs are written by the syntax writer of h265write.h.  The
 * expected values are the written syntax elements, the derived values
 * worked from 7-10 to 7-19, 7.4.3.2.1 and Table 6-1 by hand (the hand
 * examples give the working), and the bit counts of 7.3.3 added up.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "h265write.h"
#include "userland/desktop/libmedia/h265sw-ps.h"

/* The state of the pseudo-random generator, fixed so a failure repeats. */
static uint32_t random_state = 0x6C078965U;

/* The writer, the store (and a saved copy of it), too large for the stack. */
static struct bitwriter writer;
static struct h265sw_ps_store store;
static struct h265sw_ps_store saved;

/* The syntax values of the set being tested. */
static struct h265w_vps vps_syntax;
static struct h265w_sps sps_syntax;
static struct h265w_pps pps_syntax;
static struct h265w_ptl ptl_syntax;

/* The scaling lists and DC values of the last sps_random() that signalled them. */
static uint8_t random_lists[4][6][64];
static uint8_t random_dc[2][6];

/* The sets of the check_pair tests. */
static struct h265sw_sps pair_sps;
static struct h265sw_pps pair_pps;

/* The bits written by the last write (including the trailing bits) and its bytes. */
static size_t last_bits;
static size_t last_bytes;

/* Static function declarations. */
static uint32_t random_next(void);
static int do_ptl(const struct h265w_ptl *syntax, unsigned max_sub, size_t cut, struct h265sw_ptl *out, size_t *position);
static int do_vps(const struct h265w_vps *syntax, size_t cut);
static int do_sps(const struct h265w_sps *syntax, size_t cut);
static int do_pps(const struct h265w_pps *syntax, size_t cut);
static int do_bytes(int kind, const uint8_t *data, size_t size);
static void store_save(void);
static int store_same_but_refused(void);
static void hrd_fill(struct h265w_hrd *hrd, unsigned seed, unsigned nal, unsigned vcl);
static void sps_random(struct h265w_sps *sps);
static void sps_check_syntax(const struct h265w_sps *syntax, const struct h265sw_sps *read);
static void rps_chain(struct h265w_sps *sps, unsigned count);
static void sps_blocks(struct h265w_sps *sps, uint32_t min_cb_minus3, uint32_t diff_cb);
static void check_rps_explicit(const struct h265w_st_rps *syntax, const struct h265sw_sps *read, unsigned index);
static void check_sps_scaling(const struct h265w_sps *syntax, const struct h265sw_sps *read);
static void check_ptl_bits(void);
static void check_ptl_fields(void);
static void check_vps_basic(void);
static void check_vps_ordering(void);
static void check_vps_layers(void);
static void check_vps_hrd(void);
static void check_vps_ranges(void);
static void check_sps_default(void);
static void check_sps_branches(void);
static void check_sps_derived(void);
static void check_sps_sizes(void);
static void check_sps_log2(void);
static void check_sps_pcm(void);
static void check_sps_sets(void);
static void check_sps_extensions(void);
static void check_sps_vui(void);
static void check_sps_supported(void);
static void check_sps_random(void);
static void check_pps_default(void);
static void check_pps_ranges(void);
static void check_pps_tiles(void);
static void check_pps_extensions(void);
static void check_pps_branches(void);
static void pair_reset(void);
static void pps_random(struct h265w_pps *pps);
static void check_invariants(int kind, unsigned id);
static void check_pair(void);
static void check_store(void);
static void check_truncated(void);
static void check_fuzz(void);

/* Returns the next pseudo-random number (xorshift32). */
static uint32_t
random_next(void)
{
	/* The three shifts of the generator. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	/* Succeeded: the next number. */
	return random_state;
}

/*
 * Writes a profile_tier_level() and reads it with the first cut bytes (all
 * when cut is large), keeping the bit position the reader ended at.
 */
static int
do_ptl(
	const struct h265w_ptl *syntax,
	unsigned max_sub,
	size_t cut,
	struct h265sw_ptl *out,
	size_t *position)
{
	struct vcodec_bitreader bits;
	size_t bytes;
	int error;

	/* Writes the structure, padded to a byte, plus enough zero bytes that a stop bit exists. */
	bitwriter_init(&writer);
	h265w_ptl(&writer, syntax, max_sub);
	last_bits = writer.bits;
	bitwriter_trailing(&writer);
	bytes = bitwriter_bytes(&writer);
	last_bytes = bytes;
	if (cut < bytes)
		bytes = cut;

	/* Reads it into a structure that holds a value the reader must overwrite. */
	memset(out, 0xCC, sizeof(*out));
	vcodec_bitreader_init(&bits, writer.data, bytes);
	error = h265sw_ps_read_ptl(out, &bits, max_sub);
	*position = bits.position;

	/* Succeeded: the reader's answer. */
	return error;
}

/* Writes a VPS and reads it with the first cut bytes (all when cut is large) into the store. */
static int
do_vps(
	const struct h265w_vps *syntax,
	size_t cut)
{
	struct vcodec_bitreader bits;
	size_t bytes;

	bitwriter_init(&writer);
	h265w_vps(&writer, syntax);
	last_bits = writer.bits;
	bytes = bitwriter_bytes(&writer);
	last_bytes = bytes;
	if (cut < bytes)
		bytes = cut;
	vcodec_bitreader_init(&bits, writer.data, bytes);

	/* Succeeded: the reader's answer. */
	return h265sw_ps_read_vps(&store, &bits);
}

/* Writes an SPS and reads it with the first cut bytes into the store. */
static int
do_sps(
	const struct h265w_sps *syntax,
	size_t cut)
{
	struct vcodec_bitreader bits;
	size_t bytes;

	bitwriter_init(&writer);
	h265w_sps(&writer, syntax);
	last_bits = writer.bits;
	bytes = bitwriter_bytes(&writer);
	last_bytes = bytes;
	if (cut < bytes)
		bytes = cut;
	vcodec_bitreader_init(&bits, writer.data, bytes);

	/* Succeeded: the reader's answer. */
	return h265sw_ps_read_sps(&store, &bits);
}

/* Writes a PPS and reads it with the first cut bytes into the store. */
static int
do_pps(
	const struct h265w_pps *syntax,
	size_t cut)
{
	struct vcodec_bitreader bits;
	size_t bytes;

	bitwriter_init(&writer);
	h265w_pps(&writer, syntax);
	last_bits = writer.bits;
	bytes = bitwriter_bytes(&writer);
	last_bytes = bytes;
	if (cut < bytes)
		bytes = cut;
	vcodec_bitreader_init(&bits, writer.data, bytes);

	/* Succeeded: the reader's answer. */
	return h265sw_ps_read_pps(&store, &bits);
}

/* Reads raw bytes as a VPS (kind 0), SPS (1) or PPS (2) into the store. */
static int
do_bytes(
	int kind,
	const uint8_t *data,
	size_t size)
{
	struct vcodec_bitreader bits;

	vcodec_bitreader_init(&bits, data, size);
	if (kind == 0)
		return h265sw_ps_read_vps(&store, &bits);
	if (kind == 1)
		return h265sw_ps_read_sps(&store, &bits);
	return h265sw_ps_read_pps(&store, &bits);
}

/* Saves the store, to compare it after a read that must not change it. */
static void
store_save(void)
{
	memcpy(&saved, &store, sizeof(store));
}

/* Compares the store with the saved copy, ignoring the refused flags (a refused read may set them). */
static int
store_same_but_refused(void)
{
	memcpy(saved.vps_refused, store.vps_refused, sizeof(store.vps_refused));
	memcpy(saved.sps_refused, store.sps_refused, sizeof(store.sps_refused));
	memcpy(saved.pps_refused, store.pps_refused, sizeof(store.pps_refused));
	return memcmp(&saved, &store, sizeof(store)) == 0;
}

/* Fills an HRD with valid values that depend on seed, for one sub-layer and the NAL and VCL parts chosen. */
static void
hrd_fill(
	struct h265w_hrd *hrd,
	unsigned seed,
	unsigned nal,
	unsigned vcl)
{
	unsigned index;

	/* One CPB in each part; the picture rate is not fixed, the delay is not low. */
	memset(hrd, 0, sizeof(*hrd));
	hrd->nal_present = nal;
	hrd->vcl_present = vcl;
	hrd->bit_rate_scale = seed % 16U;
	hrd->cpb_size_scale = (seed + 1U) % 16U;
	hrd->initial_cpb_removal_delay_length_minus1 = 10U + seed % 8U;
	hrd->au_cpb_removal_delay_length_minus1 = 11U;
	hrd->dpb_output_delay_length_minus1 = 12U;
	for (index = 0U; index < H265W_MAX_SUB_LAYERS; index++) {
		hrd->fixed_pic_rate_general[index] = 1U;
		hrd->elemental_duration_in_tc_minus1[index] = seed + index;
		hrd->cpb_cnt_minus1[index] = seed % 3U;
		hrd->nal[index].bit_rate_value_minus1[0] = 1000U + seed;
		hrd->nal[index].cpb_size_value_minus1[0] = 2000U + seed;
		hrd->vcl[index].bit_rate_value_minus1[0] = 3000U + seed;
		hrd->vcl[index].cpb_size_value_minus1[0] = 4000U + seed;
	}
}

/*
 * Fills sps with a random valid SPS: any chroma format, bit depths 8..16,
 * block sizes, a conformance window, sub-layer ordering, scaling lists,
 * PCM, explicit short-term sets, long-term pictures, a small VUI and the
 * range extension flags.  The profile is 4 (a VUI error is then an error).
 */
static void
sps_random(
	struct h265w_sps *sps)
{
	unsigned index;
	unsigned entry;
	unsigned min_cb;
	unsigned ctb;
	unsigned min_tb;
	unsigned max_tb_limit;
	unsigned sub_width;
	unsigned sub_height;
	unsigned count;

	h265w_sps_default(sps);
	sps->ptl.general.profile_idc = 4U;
	sps->vps_id = random_next() % 16U;
	sps->max_sub_layers_minus1 = random_next() % 7U;
	sps->temporal_id_nesting = random_next() % 2U;
	sps->sps_id = random_next() % 16U;
	sps->chroma_format_idc = random_next() % 4U;
	if (sps->chroma_format_idc == 3U)
		sps->separate_colour_plane = random_next() % 2U;
	sps->bit_depth_luma_minus8 = random_next() % 9U;
	sps->bit_depth_chroma_minus8 = random_next() % 9U;
	sps->log2_max_poc_lsb_minus4 = random_next() % 13U;

	/* Block sizes: MinCbLog2SizeY 3..6, CtbLog2SizeY 4..6, transform sizes within the limits. */
	min_cb = 3U + random_next() % 4U;
	ctb = min_cb;
	if (ctb < 4U)
		ctb = 4U;
	ctb += random_next() % (7U - ctb);
	sps->log2_min_cb_minus3 = min_cb - 3U;
	sps->log2_diff_max_min_cb = ctb - min_cb;
	min_tb = 2U + random_next() % (min_cb - 2U);
	max_tb_limit = ctb;
	if (max_tb_limit > 5U)
		max_tb_limit = 5U;
	sps->log2_min_tb_minus2 = min_tb - 2U;
	sps->log2_diff_max_min_tb = random_next() % (max_tb_limit - min_tb + 1U);
	sps->max_th_depth_inter = random_next() % (ctb - min_tb + 1U);
	sps->max_th_depth_intra = random_next() % (ctb - min_tb + 1U);

	/* The size: a multiple of the smallest coding block. */
	sps->width = (1U + random_next() % (6000U >> min_cb)) << min_cb;
	sps->height = (1U + random_next() % (4000U >> min_cb)) << min_cb;

	/* A conformance window that leaves at least one chroma unit. */
	sub_width = 1U;
	sub_height = 1U;
	if (sps->chroma_format_idc == 1U) {
		sub_width = 2U;
		sub_height = 2U;
	} else if (sps->chroma_format_idc == 2U) {
		sub_width = 2U;
	}
	sps->conformance_window = random_next() % 2U;
	if (sps->conformance_window != 0U) {
		sps->conf_win_offset[0] = random_next() % (sps->width / sub_width / 4U + 1U);
		sps->conf_win_offset[1] = random_next() % (sps->width / sub_width / 4U + 1U);
		sps->conf_win_offset[2] = random_next() % (sps->height / sub_height / 4U + 1U);
		sps->conf_win_offset[3] = random_next() % (sps->height / sub_height / 4U + 1U);
	}

	/* Sub-layer ordering. */
	sps->sub_layer_ordering_info_present = random_next() % 2U;
	for (index = 0U; index <= sps->max_sub_layers_minus1; index++) {
		sps->max_dec_pic_buffering_minus1[index] = 4U + random_next() % 12U;
		sps->max_num_reorder_pics[index] = random_next() % (sps->max_dec_pic_buffering_minus1[index] + 1U);
		sps->max_latency_increase_plus1[index] = random_next() % 100U;
	}

	/* Scaling lists: none, default or signalled. */
	sps->scaling_list_enabled = random_next() % 2U;
	if (sps->scaling_list_enabled != 0U) {
		sps->scaling_list_data_present = random_next() % 2U;
		if (sps->scaling_list_data_present != 0U) {
			memset(random_lists, 0, sizeof(random_lists));
			memset(random_dc, 0, sizeof(random_dc));
			for (index = 0U; index < 4U; index++) {
				for (entry = 0U; entry < 6U; entry += (index == 3U) ? 3U : 1U) {
					for (count = 0U; count < 64U; count++)
						random_lists[index][entry][count] = (uint8_t)(1U + random_next() % 255U);
					random_dc[0][entry] = (uint8_t)(1U + random_next() % 255U);
					random_dc[1][entry] = (uint8_t)(1U + random_next() % 255U);
				}
			}
			h265w_scaling_from_lists(&sps->scaling, (const uint8_t (*)[6][64])random_lists, (const uint8_t (*)[6])random_dc);
		}
	}
	sps->amp = random_next() % 2U;
	sps->sao = random_next() % 2U;

	/* PCM: depths within the bit depths, sizes within Min( MinCb, 5 )..Min( Ctb, 5 ). */
	sps->pcm = random_next() % 2U;
	if (sps->pcm != 0U) {
		unsigned low;
		unsigned high;
		unsigned pcm_min;

		sps->pcm_bit_depth_luma_minus1 = random_next() % (sps->bit_depth_luma_minus8 + 8U);
		sps->pcm_bit_depth_chroma_minus1 = random_next() % (sps->bit_depth_chroma_minus8 + 8U);
		low = (min_cb < 5U) ? min_cb : 5U;
		high = (ctb < 5U) ? ctb : 5U;
		pcm_min = low + random_next() % (high - low + 1U);
		sps->log2_min_pcm_minus3 = pcm_min - 3U;
		sps->log2_diff_max_min_pcm = random_next() % (high - pcm_min + 1U);
		sps->pcm_loop_filter_disabled = random_next() % 2U;
	}

	/* Explicit short-term sets, each within the buffering of the highest sub-layer. */
	sps->num_st_rps = random_next() % 11U;
	count = sps->max_dec_pic_buffering_minus1[sps->max_sub_layers_minus1];
	for (index = 0U; index < sps->num_st_rps; index++) {
		memset(&sps->st_rps[index], 0, sizeof(sps->st_rps[index]));
		sps->st_rps[index].num_negative_pics = random_next() % (count + 1U);
		sps->st_rps[index].num_positive_pics = random_next() % (count - sps->st_rps[index].num_negative_pics + 1U);
		for (entry = 0U; entry < 16U; entry++) {
			sps->st_rps[index].delta_poc_s0_minus1[entry] = random_next() % 32768U;
			sps->st_rps[index].delta_poc_s1_minus1[entry] = random_next() % 32768U;
			sps->st_rps[index].used_by_curr_pic_s0_flag[entry] = random_next() % 2U;
			sps->st_rps[index].used_by_curr_pic_s1_flag[entry] = random_next() % 2U;
		}
	}

	/* Long-term pictures. */
	sps->long_term_present = random_next() % 2U;
	if (sps->long_term_present != 0U) {
		sps->num_lt_sps = random_next() % 33U;
		for (index = 0U; index < sps->num_lt_sps; index++) {
			sps->lt_poc_lsb[index] = random_next() % (1U << (sps->log2_max_poc_lsb_minus4 + 4U));
			sps->lt_used[index] = random_next() % 2U;
		}
	}
	sps->temporal_mvp = random_next() % 2U;
	sps->strong_intra_smoothing = random_next() % 2U;

	/* A small VUI. */
	sps->vui_present = random_next() % 2U;
	if (sps->vui_present != 0U) {
		sps->vui.aspect_ratio_present = 1U;
		sps->vui.aspect_ratio_idc = 1U + random_next() % 16U;
		sps->vui.timing_present = random_next() % 2U;
		sps->vui.num_units_in_tick = 1U + random_next() % 1000U;
		sps->vui.time_scale = 1U + random_next() % 100000U;
		sps->vui.bitstream_restriction = random_next() % 2U;
		sps->vui.max_bytes_per_pic_denom = random_next() % 17U;
		sps->vui.max_bits_per_min_cu_denom = random_next() % 17U;
		sps->vui.log2_max_mv_length_horizontal = random_next() % 16U;
		sps->vui.log2_max_mv_length_vertical = random_next() % 16U;
		sps->vui.motion_vectors_over_pic_boundaries = random_next() % 2U;
	}

	/* The range extension flags. */
	sps->extension_present = random_next() % 2U;
	if (sps->extension_present != 0U) {
		sps->range_extension_flag = random_next() % 2U;
		sps->range_extension = random_next() % 512U;
	}
}

/* Checks the signalled fields of a read SPS against the syntax that was written (the hand-written derivations of 7.4.3.2.1). */
static void
sps_check_syntax(
	const struct h265w_sps *syntax,
	const struct h265sw_sps *read)
{
	unsigned index;
	unsigned last;
	unsigned expected_ext;

	CHECK(read->vps_id == syntax->vps_id);
	CHECK(read->max_sub_layers_minus1 == syntax->max_sub_layers_minus1);
	CHECK((read->temporal_id_nesting != 0) == (syntax->temporal_id_nesting != 0U));
	CHECK(read->ptl.profile_idc == syntax->ptl.general.profile_idc);
	CHECK(read->ptl.level_idc == syntax->ptl.level_idc);
	CHECK(read->id == syntax->sps_id);
	CHECK(read->chroma_format_idc == syntax->chroma_format_idc);
	CHECK((read->separate_colour_plane != 0) == (syntax->chroma_format_idc == 3U && syntax->separate_colour_plane != 0U));
	CHECK(read->width == syntax->width);
	CHECK(read->height == syntax->height);
	CHECK((read->conformance_window != 0) == (syntax->conformance_window != 0U));
	for (index = 0U; index < 4U; index++)
		CHECK(read->conf_win_offset[index] == ((syntax->conformance_window != 0U) ? syntax->conf_win_offset[index] : 0U));
	CHECK(read->bit_depth_luma == 8U + syntax->bit_depth_luma_minus8);
	CHECK(read->bit_depth_chroma == 8U + syntax->bit_depth_chroma_minus8);
	CHECK(read->log2_max_poc_lsb == 4U + syntax->log2_max_poc_lsb_minus4);
	CHECK((read->sub_layer_ordering_info_present != 0) == (syntax->sub_layer_ordering_info_present != 0U));

	/* The ordering values, the highest sub-layer's standing for the lower ones when they are not sent; nothing above. */
	last = syntax->max_sub_layers_minus1;
	for (index = 0U; index < H265SW_MAX_SUB_LAYERS; index++) {
		if (index > last) {
			CHECK(read->max_dec_pic_buffering_minus1[index] == 0U);
			CHECK(read->max_num_reorder_pics[index] == 0U);
			CHECK(read->max_latency_increase_plus1[index] == 0U);
		} else if (syntax->sub_layer_ordering_info_present != 0U || index == last) {
			CHECK(read->max_dec_pic_buffering_minus1[index] == syntax->max_dec_pic_buffering_minus1[index]);
			CHECK(read->max_num_reorder_pics[index] == syntax->max_num_reorder_pics[index]);
			CHECK(read->max_latency_increase_plus1[index] == syntax->max_latency_increase_plus1[index]);
		} else {
			CHECK(read->max_dec_pic_buffering_minus1[index] == syntax->max_dec_pic_buffering_minus1[last]);
			CHECK(read->max_num_reorder_pics[index] == syntax->max_num_reorder_pics[last]);
			CHECK(read->max_latency_increase_plus1[index] == syntax->max_latency_increase_plus1[last]);
		}
	}

	/* Block sizes (7-10, 7-11) and transform sizes. */
	CHECK(read->min_cb_log2 == syntax->log2_min_cb_minus3 + 3U);
	CHECK(read->ctb_log2 == syntax->log2_min_cb_minus3 + 3U + syntax->log2_diff_max_min_cb);
	CHECK(read->min_tb_log2 == syntax->log2_min_tb_minus2 + 2U);
	CHECK(read->max_tb_log2 == syntax->log2_min_tb_minus2 + 2U + syntax->log2_diff_max_min_tb);
	CHECK(read->max_th_depth_inter == syntax->max_th_depth_inter);
	CHECK(read->max_th_depth_intra == syntax->max_th_depth_intra);
	CHECK((read->scaling_list_enabled != 0) == (syntax->scaling_list_enabled != 0U));
	CHECK((read->scaling_list_data_present != 0) == (syntax->scaling_list_enabled != 0U && syntax->scaling_list_data_present != 0U));
	CHECK((read->amp != 0) == (syntax->amp != 0U));
	CHECK((read->sao != 0) == (syntax->sao != 0U));
	CHECK((read->pcm != 0) == (syntax->pcm != 0U));
	if (syntax->pcm != 0U) {
		CHECK(read->pcm_bit_depth_luma == syntax->pcm_bit_depth_luma_minus1 + 1U);
		CHECK(read->pcm_bit_depth_chroma == syntax->pcm_bit_depth_chroma_minus1 + 1U);
		CHECK(read->pcm_min_log2 == syntax->log2_min_pcm_minus3 + 3U);
		CHECK(read->pcm_max_log2 == syntax->log2_min_pcm_minus3 + 3U + syntax->log2_diff_max_min_pcm);
		CHECK((read->pcm_loop_filter_disabled != 0) == (syntax->pcm_loop_filter_disabled != 0U));
	} else {
		CHECK(read->pcm_bit_depth_luma == 0U && read->pcm_min_log2 == 0U && read->pcm_max_log2 == 0U);
	}
	CHECK(read->num_st_rps == syntax->num_st_rps);
	CHECK((read->long_term_present != 0) == (syntax->long_term_present != 0U));
	CHECK(read->num_lt_sps == ((syntax->long_term_present != 0U) ? syntax->num_lt_sps : 0U));
	if (syntax->long_term_present != 0U) {
		for (index = 0U; index < syntax->num_lt_sps; index++) {
			CHECK(read->lt_poc_lsb[index] == syntax->lt_poc_lsb[index]);
			CHECK(read->lt_used[index] == syntax->lt_used[index]);
		}
	}
	CHECK((read->temporal_mvp != 0) == (syntax->temporal_mvp != 0U));
	CHECK((read->strong_intra_smoothing != 0) == (syntax->strong_intra_smoothing != 0U));
	CHECK((read->vui_present != 0) == (syntax->vui_present != 0U));
	CHECK((read->extension_present != 0) == (syntax->extension_present != 0U));
	CHECK((read->range_extension_present != 0) == (syntax->extension_present != 0U && syntax->range_extension_flag != 0U));
	expected_ext = 0U;
	if (syntax->extension_present != 0U && syntax->range_extension_flag != 0U)
		expected_ext = syntax->range_extension & 0x1FFU;
	CHECK(read->range_extension == expected_ext);
}

/*
 * Fills sps with a chain of count short-term sets: set 0 is explicit,
 * and each later set is predicted from the one before it.  The sets cycle
 * with period two after the first (worked by hand in the comments of
 * check_sps_sets): A = S0 { -1 }; B (from A, deltaRps -1, flags used) =
 * S0 { -1, -2 }; C (from B, deltaRps +1) = S0 { -1 }, S1 { 1 }; and B
 * again from C (deltaRps -1).
 */
static void
rps_chain(
	struct h265w_sps *sps,
	unsigned count)
{
	unsigned index;
	unsigned reference;

	/* The first set, A. */
	sps->num_st_rps = count;
	memset(&sps->st_rps[0], 0, sizeof(sps->st_rps[0]));
	sps->st_rps[0].num_negative_pics = 1U;
	sps->st_rps[0].used_by_curr_pic_s0_flag[0] = 1U;

	/* B for odd indexes, C for even ones, each predicted from the set before. */
	for (index = 1U; index < count; index++) {
		memset(&sps->st_rps[index], 0, sizeof(sps->st_rps[index]));
		sps->st_rps[index].inter_rps_pred = 1U;
		reference = 0U;
		if (index == 1U) {
			/* From A: one entry, so two flag pairs. */
			reference = 1U;
		} else {
			/* From C or B: two entries, so three flag pairs. */
			reference = 2U;
		}
		sps->st_rps[index].ref_num_delta_pocs = reference;
		sps->st_rps[index].used_by_curr_pic_flag[0] = 1U;
		sps->st_rps[index].used_by_curr_pic_flag[1] = 1U;
		sps->st_rps[index].used_by_curr_pic_flag[2] = 1U;
		if ((index & 1U) != 0U) {
			sps->st_rps[index].delta_rps_sign = 1U;
		} else {
			sps->st_rps[index].delta_rps_sign = 0U;
		}
	}
}

/*
 * The bits profile_tier_level( 1, maxNumSubLayersMinus1 ) takes (7.3.3):
 * the general part is 2 + 1 + 5 + 32 + 4 + 43 + 1 = 88 bits and the level
 * is 8; each sub-layer adds a profile flag and a level flag; with
 * sub-layers there are 2 * ( 8 - maxNumSubLayersMinus1 ) reserved bits;
 * each present sub-layer profile is 88 bits and level 8.
 */
static void
check_ptl_bits(void)
{
	struct h265sw_ptl out;
	size_t position;
	size_t expected;
	size_t cut;
	unsigned max_sub;
	unsigned mask;
	unsigned index;
	unsigned profiles;
	unsigned levels;
	int error;

	for (max_sub = 0U; max_sub <= 6U; max_sub++) {
		/* Every combination of the profile and level flags of the sub-layers. */
		for (mask = 0U; mask < (1U << (2U * max_sub)); mask++) {
			memset(&ptl_syntax, 0, sizeof(ptl_syntax));
			ptl_syntax.general.profile_idc = 1U;
			ptl_syntax.general.bits43 = ((uint64_t)random_next() << 11) ^ random_next();
			ptl_syntax.general.bits43 &= (((uint64_t)1U << 43) - 1U);
			ptl_syntax.general.bit44 = random_next() & 1U;
			ptl_syntax.level_idc = 93U;
			profiles = 0U;
			levels = 0U;
			for (index = 0U; index < max_sub; index++) {
				ptl_syntax.profile_present[index] = (mask >> (2U * index)) & 1U;
				ptl_syntax.level_present[index] = (mask >> (2U * index + 1U)) & 1U;
				profiles += ptl_syntax.profile_present[index];
				levels += ptl_syntax.level_present[index];
				ptl_syntax.sub_profile[index].profile_idc = 1U + index;
				ptl_syntax.sub_profile[index].bits43 = random_next();
				ptl_syntax.sub_level_idc[index] = 30U + index;
			}
			for (index = 0U; index < 8U; index++)
				ptl_syntax.reserved_2bits[index] = random_next() % 4U;

			expected = 88U + 8U + 2U * max_sub + 88U * profiles + 8U * levels;
			if (max_sub > 0U)
				expected += 2U * (8U - max_sub);

			error = do_ptl(&ptl_syntax, max_sub, (size_t)-1, &out, &position);
			CHECK(error == 0);
			CHECK(position == expected);
			CHECK(last_bits == expected);
			CHECK(out.profile_idc == 1U);
			CHECK(out.level_idc == 93U);
		}
	}

	/* The longest structure cut at every byte: refused until the whole of it is there. */
	memset(&ptl_syntax, 0, sizeof(ptl_syntax));
	ptl_syntax.general.profile_idc = 2U;
	ptl_syntax.level_idc = 120U;
	for (index = 0U; index < 6U; index++) {
		ptl_syntax.profile_present[index] = 1U;
		ptl_syntax.level_present[index] = 1U;
	}
	error = do_ptl(&ptl_syntax, 6U, (size_t)-1, &out, &position);
	CHECK(error == 0);
	for (cut = 0U; cut < last_bytes; cut++) {
		error = do_ptl(&ptl_syntax, 6U, cut, &out, &position);
		if (cut * 8U < last_bits) {
			CHECK(error == EINVAL);
		} else {
			CHECK(error == 0);
		}
	}

	/* Seven sub-layers and missing arguments are refused. */
	error = do_ptl(&ptl_syntax, 7U, (size_t)-1, &out, &position);
	CHECK(error == EINVAL);
	{
		struct vcodec_bitreader bits;

		vcodec_bitreader_init(&bits, writer.data, 32U);
		CHECK(h265sw_ps_read_ptl(NULL, &bits, 0U) == EINVAL);
		CHECK(h265sw_ps_read_ptl(&out, NULL, 0U) == EINVAL);
	}
}

/* The general profile fields the structure keeps. */
static void
check_ptl_fields(void)
{
	struct h265sw_ptl out;
	size_t position;
	int error;

	/* Every field set to a distinct value; the constraint bits are all ones and do not leak into them. */
	memset(&ptl_syntax, 0, sizeof(ptl_syntax));
	ptl_syntax.general.profile_space = 3U;
	ptl_syntax.general.tier = 1U;
	ptl_syntax.general.profile_idc = 31U;
	ptl_syntax.general.compatibility = 0xA5A5A5A5U;
	ptl_syntax.general.progressive = 1U;
	ptl_syntax.general.interlaced = 0U;
	ptl_syntax.general.non_packed = 1U;
	ptl_syntax.general.frame_only = 0U;
	ptl_syntax.general.bits43 = ((uint64_t)1U << 43) - 1U;
	ptl_syntax.general.bit44 = 1U;
	ptl_syntax.level_idc = 186U;
	error = do_ptl(&ptl_syntax, 0U, (size_t)-1, &out, &position);
	CHECK(error == 0);
	CHECK(out.profile_space == 3U);
	CHECK(out.tier == 1U);
	CHECK(out.profile_idc == 31U);
	CHECK(out.compatibility == 0xA5A5A5A5U);
	CHECK(out.progressive == 1 && out.interlaced == 0 && out.non_packed == 1 && out.frame_only == 0);
	CHECK(out.level_idc == 186U);

	/* The flags the other way round and every compatibility bit alone. */
	ptl_syntax.general.progressive = 0U;
	ptl_syntax.general.interlaced = 1U;
	ptl_syntax.general.non_packed = 0U;
	ptl_syntax.general.frame_only = 1U;
	ptl_syntax.general.compatibility = 0x00000001U;
	ptl_syntax.general.profile_space = 0U;
	ptl_syntax.general.tier = 0U;
	ptl_syntax.level_idc = 0U;
	error = do_ptl(&ptl_syntax, 0U, (size_t)-1, &out, &position);
	CHECK(error == 0);
	CHECK(out.progressive == 0 && out.interlaced == 1 && out.non_packed == 0 && out.frame_only == 1);
	CHECK(out.compatibility == 1U && out.profile_space == 0U && out.tier == 0U && out.level_idc == 0U);
	ptl_syntax.general.compatibility = 0x80000000U;
	error = do_ptl(&ptl_syntax, 0U, (size_t)-1, &out, &position);
	CHECK(error == 0);
	CHECK(out.compatibility == 0x80000000U);
}

/* A default VPS and the fields it keeps; ids, flags and reserved bits. */
static void
check_vps_basic(void)
{
	unsigned id;
	unsigned reserved;
	int error;

	/* The default VPS: Main at level 93, one layer, one sub-layer. */
	h265sw_ps_init(&store);
	h265w_vps_default(&vps_syntax);
	error = do_vps(&vps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.vps_present[0] == 1 && store.vps_refused[0] == 0);
	CHECK(store.generation == 1U);
	CHECK(store.vps[0].id == 0U);
	CHECK(store.vps[0].base_layer_internal == 1 && store.vps[0].base_layer_available == 1);
	CHECK(store.vps[0].max_layers_minus1 == 0U && store.vps[0].max_sub_layers_minus1 == 0U);
	CHECK(store.vps[0].temporal_id_nesting == 1);
	CHECK(store.vps[0].ptl.profile_idc == 1U && store.vps[0].ptl.level_idc == 93U);
	CHECK(store.vps[0].sub_layer_ordering_info_present == 1);
	CHECK(store.vps[0].max_dec_pic_buffering_minus1[0] == 4U);
	CHECK(store.vps[0].max_num_reorder_pics[0] == 0U && store.vps[0].max_latency_increase_plus1[0] == 0U);
	CHECK(store.vps[0].max_layer_id == 0U && store.vps[0].num_layer_sets_minus1 == 0U);
	CHECK(store.vps[0].timing_present == 0 && store.vps[0].num_hrd_parameters == 0U && store.vps[0].extension == 0);
	CHECK(store.vps[0].hrd.nal_hrd_present == 0 && store.vps[0].hrd.vcl_hrd_present == 0);

	/* Each id 0..15 lands in its own slot. */
	h265sw_ps_init(&store);
	for (id = 0U; id < 16U; id++) {
		h265w_vps_default(&vps_syntax);
		vps_syntax.vps_id = id;
		error = do_vps(&vps_syntax, (size_t)-1);
		CHECK(error == 0);
		CHECK(store.vps_present[id] == 1 && store.vps[id].id == id);
	}
	CHECK(store.generation == 16U);

	/* The reserved 16 bits are read over whatever they hold, and the profile after them is found. */
	for (reserved = 0U; reserved < 4U; reserved++) {
		h265sw_ps_init(&store);
		h265w_vps_default(&vps_syntax);
		vps_syntax.reserved_0xffff_16bits = (reserved == 0U) ? 0U : (reserved == 1U) ? 0x1234U : (reserved == 2U) ? 0xFFFFU : 0x8001U;
		vps_syntax.ptl.general.profile_idc = 5U;
		vps_syntax.ptl.level_idc = 150U;
		error = do_vps(&vps_syntax, (size_t)-1);
		CHECK(error == 0);
		CHECK(store.vps[0].ptl.profile_idc == 5U && store.vps[0].ptl.level_idc == 150U);
	}

	/* The layer fields: 0, 62 and 63 layers (63 is read although reserved), the base layer flags, the largest layer id. */
	h265sw_ps_init(&store);
	h265w_vps_default(&vps_syntax);
	vps_syntax.max_layers_minus1 = 63U;
	vps_syntax.max_layer_id = 63U;
	vps_syntax.base_layer_internal = 0U;
	vps_syntax.base_layer_available = 0U;
	vps_syntax.temporal_id_nesting = 0U;
	error = do_vps(&vps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.vps[0].max_layers_minus1 == 63U && store.vps[0].max_layer_id == 63U);
	CHECK(store.vps[0].base_layer_internal == 0 && store.vps[0].base_layer_available == 0);
	CHECK(store.vps[0].temporal_id_nesting == 0);
	vps_syntax.max_layers_minus1 = 62U;
	error = do_vps(&vps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.vps[0].max_layers_minus1 == 62U);
}

/* The sub-layer ordering values: every sub-layer, the highest copied down, and the ranges. */
static void
check_vps_ordering(void)
{
	unsigned max_sub;
	unsigned present;
	unsigned index;
	int error;

	/* For 1..7 sub-layers with and without per-sub-layer values (7-1 copies the highest one down). */
	for (max_sub = 0U; max_sub <= 6U; max_sub++) {
		for (present = 0U; present <= 1U; present++) {
			h265sw_ps_init(&store);
			h265w_vps_default(&vps_syntax);
			vps_syntax.max_sub_layers_minus1 = max_sub;
			vps_syntax.sub_layer_ordering_info_present = present;
			for (index = 0U; index <= max_sub; index++) {
				vps_syntax.max_dec_pic_buffering_minus1[index] = 3U + index;
				vps_syntax.max_num_reorder_pics[index] = index;
				vps_syntax.max_latency_increase_plus1[index] = 10U + index;
			}
			error = do_vps(&vps_syntax, (size_t)-1);
			CHECK(error == 0);
			CHECK(store.vps[0].max_sub_layers_minus1 == max_sub);
			CHECK(store.vps[0].sub_layer_ordering_info_present == (int)present);
			for (index = 0U; index < H265SW_MAX_SUB_LAYERS; index++) {
				if (index > max_sub) {
					CHECK(store.vps[0].max_dec_pic_buffering_minus1[index] == 0U);
					CHECK(store.vps[0].max_num_reorder_pics[index] == 0U);
					CHECK(store.vps[0].max_latency_increase_plus1[index] == 0U);
				} else if (present != 0U || index == max_sub) {
					CHECK(store.vps[0].max_dec_pic_buffering_minus1[index] == 3U + index);
					CHECK(store.vps[0].max_num_reorder_pics[index] == index);
					CHECK(store.vps[0].max_latency_increase_plus1[index] == 10U + index);
				} else {
					CHECK(store.vps[0].max_dec_pic_buffering_minus1[index] == 3U + max_sub);
					CHECK(store.vps[0].max_num_reorder_pics[index] == max_sub);
					CHECK(store.vps[0].max_latency_increase_plus1[index] == 10U + max_sub);
				}
			}
		}
	}

	/* The ranges: the buffer up to 15 and no more, the reorder depth up to the buffer, the latency up to 2^32 - 2. */
	h265sw_ps_init(&store);
	h265w_vps_default(&vps_syntax);
	vps_syntax.max_dec_pic_buffering_minus1[0] = 15U;
	vps_syntax.max_num_reorder_pics[0] = 15U;
	vps_syntax.max_latency_increase_plus1[0] = 0xFFFFFFFEU;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == 0);
	CHECK(store.vps[0].max_latency_increase_plus1[0] == 0xFFFFFFFEU);
	vps_syntax.max_dec_pic_buffering_minus1[0] = 16U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);
	vps_syntax.max_dec_pic_buffering_minus1[0] = 0x80000000U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);
	vps_syntax.max_dec_pic_buffering_minus1[0] = 4U;
	vps_syntax.max_num_reorder_pics[0] = 5U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);
	vps_syntax.max_num_reorder_pics[0] = 4U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == 0);

	/* Seven sub-layers (vps_max_sub_layers_minus1 7) is refused. */
	vps_syntax.max_sub_layers_minus1 = 7U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);
}

/* The layer sets: their flags are read over, and the counts are bounded. */
static void
check_vps_layers(void)
{
	static const unsigned sets[] = {0U, 1U, 2U, 7U, 1023U};
	static const unsigned layer_ids[] = {0U, 1U, 31U, 63U};
	unsigned set_index;
	unsigned id_index;
	int error;

	/* Each count and layer id: the flags are read over and the timing after them is in place. */
	for (set_index = 0U; set_index < sizeof(sets) / sizeof(sets[0]); set_index++) {
		for (id_index = 0U; id_index < sizeof(layer_ids) / sizeof(layer_ids[0]); id_index++) {
			h265sw_ps_init(&store);
			h265w_vps_default(&vps_syntax);
			vps_syntax.num_layer_sets_minus1 = sets[set_index];
			vps_syntax.max_layer_id = layer_ids[id_index];
			vps_syntax.layer_id_included = set_index & 1U;
			vps_syntax.timing_present = 1U;
			vps_syntax.num_units_in_tick = 1001U;
			vps_syntax.time_scale = 60000U;
			error = do_vps(&vps_syntax, (size_t)-1);
			CHECK(error == 0);
			CHECK(store.vps[0].num_layer_sets_minus1 == sets[set_index]);
			CHECK(store.vps[0].max_layer_id == layer_ids[id_index]);
			CHECK(store.vps[0].num_units_in_tick == 1001U && store.vps[0].time_scale == 60000U);
		}
	}

	/* vps_num_layer_sets_minus1 1024 and larger is refused (the writer's flag loop bounds how large a count a test can write). */
	h265w_vps_default(&vps_syntax);
	vps_syntax.num_layer_sets_minus1 = 1024U;
	vps_syntax.max_layer_id = 63U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);
	vps_syntax.num_layer_sets_minus1 = 1100U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);
	vps_syntax.num_layer_sets_minus1 = 8000U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);
}

/* The HRD parameters: none, one and three, the common part sharing, and only the first kept. */
static void
check_vps_hrd(void)
{
	unsigned count;
	unsigned common_1;
	unsigned common_2;
	unsigned max_sub;
	unsigned index;
	int error;

	/* Timing without HRD parameters, with POC proportionality, and the values kept. */
	h265sw_ps_init(&store);
	h265w_vps_default(&vps_syntax);
	vps_syntax.timing_present = 1U;
	vps_syntax.num_units_in_tick = 1U;
	vps_syntax.time_scale = 0xFFFFFFFFU;
	vps_syntax.poc_proportional = 1U;
	vps_syntax.num_ticks_poc_diff_one_minus1 = 0xFFFFFFFEU;
	error = do_vps(&vps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.vps[0].timing_present == 1);
	CHECK(store.vps[0].num_units_in_tick == 1U && store.vps[0].time_scale == 0xFFFFFFFFU);
	CHECK(store.vps[0].poc_proportional_to_timing == 1);
	CHECK(store.vps[0].num_ticks_poc_diff_one_minus1 == 0xFFFFFFFEU);
	CHECK(store.vps[0].num_hrd_parameters == 0U);

	/* A tick or scale of 0 is refused. */
	vps_syntax.num_units_in_tick = 0U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);
	vps_syntax.num_units_in_tick = 1U;
	vps_syntax.time_scale = 0U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);

	/* 1 and 3 HRD parameters (the second and third with and without their own common part), at 1 and 3 sub-layers. */
	for (max_sub = 0U; max_sub <= 2U; max_sub += 2U) {
		for (count = 1U; count <= 3U; count += 2U) {
			for (common_1 = 0U; common_1 <= 1U; common_1++) {
				for (common_2 = 0U; common_2 <= 1U; common_2++) {
					h265sw_ps_init(&store);
					h265w_vps_default(&vps_syntax);
					vps_syntax.max_sub_layers_minus1 = max_sub;
					for (index = 0U; index <= max_sub; index++)
						vps_syntax.max_dec_pic_buffering_minus1[index] = 4U;
					vps_syntax.sub_layer_ordering_info_present = 1U;
					vps_syntax.num_layer_sets_minus1 = 2U;
					vps_syntax.timing_present = 1U;
					vps_syntax.num_units_in_tick = 1001U;
					vps_syntax.time_scale = 60000U;
					vps_syntax.num_hrd_parameters = count;

					/* The first has its own common part (NAL only); the second takes a VCL part too when it has its own; the third likewise. */
					hrd_fill(&vps_syntax.hrd[0], 0U, 1U, 0U);
					vps_syntax.hrd_layer_set_idx[0] = 0U;
					vps_syntax.cprms_present[1] = common_1;
					if (common_1 != 0U)
						hrd_fill(&vps_syntax.hrd[1], 5U, 0U, 1U);
					else
						hrd_fill(&vps_syntax.hrd[1], 5U, 1U, 0U);
					vps_syntax.hrd_layer_set_idx[1] = 1U;
					vps_syntax.cprms_present[2] = common_2;
					if (common_2 != 0U)
						hrd_fill(&vps_syntax.hrd[2], 9U, 1U, 1U);
					else if (common_1 != 0U)
						hrd_fill(&vps_syntax.hrd[2], 9U, 0U, 1U);
					else
						hrd_fill(&vps_syntax.hrd[2], 9U, 1U, 0U);
					vps_syntax.hrd_layer_set_idx[2] = 2U;

					error = do_vps(&vps_syntax, (size_t)-1);
					CHECK(error == 0);
					CHECK(store.vps[0].num_hrd_parameters == count);

					/* Only the first is kept. */
					CHECK(store.vps[0].hrd.nal_hrd_present == 1 && store.vps[0].hrd.vcl_hrd_present == 0);
					CHECK(store.vps[0].hrd.bit_rate_scale == 0U && store.vps[0].hrd.cpb_size_scale == 1U);
					CHECK(store.vps[0].hrd.initial_cpb_removal_delay_length_minus1 == 10U);
					CHECK(store.vps[0].hrd.cpb_cnt_minus1[0] == 0U);
					CHECK(store.vps[0].hrd.nal[0].bit_rate_value_minus1[0] == 1000U);
					CHECK(store.vps[0].hrd.nal[0].cpb_size_value_minus1[0] == 2000U);
					CHECK(store.vps[0].hrd.fixed_pic_rate_general[max_sub] == 1U);
					CHECK(store.vps[0].hrd.fixed_pic_rate_general[max_sub + 1U] == 0U);
				}
			}
		}
	}

	/* No HRD parameters leaves the kept HRD zero. */
	h265sw_ps_init(&store);
	h265w_vps_default(&vps_syntax);
	vps_syntax.timing_present = 1U;
	vps_syntax.num_units_in_tick = 1U;
	vps_syntax.time_scale = 1U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == 0);
	CHECK(store.vps[0].hrd.nal_hrd_present == 0 && store.vps[0].hrd.bit_rate_scale == 0U);
}

/* The layer set index of each HRD: range, duplicates, count, and the extension data. */
static void
check_vps_ranges(void)
{
	static const unsigned lengths[7] = {0U, 1U, 2U, 5U, 13U, 31U, 32U};
	unsigned base_internal;
	unsigned first;
	unsigned second;
	unsigned bits;
	unsigned index;
	int error;
	int expected;

	/* Three layer sets (indexes 0..2) and two HRD parameters with every pair of indexes (0..4). */
	for (base_internal = 0U; base_internal <= 1U; base_internal++) {
		for (first = 0U; first <= 4U; first++) {
			for (second = 0U; second <= 4U; second++) {
				h265sw_ps_init(&store);
				h265w_vps_default(&vps_syntax);
				vps_syntax.base_layer_internal = base_internal;
				vps_syntax.num_layer_sets_minus1 = 2U;
				vps_syntax.timing_present = 1U;
				vps_syntax.num_units_in_tick = 1U;
				vps_syntax.time_scale = 1U;
				vps_syntax.num_hrd_parameters = 2U;
				hrd_fill(&vps_syntax.hrd[0], 1U, 1U, 0U);
				hrd_fill(&vps_syntax.hrd[1], 2U, 1U, 0U);
				vps_syntax.cprms_present[1] = 1U;
				vps_syntax.hrd_layer_set_idx[0] = first;
				vps_syntax.hrd_layer_set_idx[1] = second;

				/* Valid when both are in lowest..2 and differ (the lowest is 1 when the base layer is external). */
				expected = 0;
				if (first < (1U - base_internal) || first > 2U || second < (1U - base_internal) || second > 2U || first == second)
					expected = EINVAL;
				error = do_vps(&vps_syntax, (size_t)-1);
				CHECK(error == expected);
			}
		}
	}

	/* More HRD parameters than layer sets (num_hrd_parameters 4 with 3 sets) is refused; 3 is taken. */
	h265sw_ps_init(&store);
	h265w_vps_default(&vps_syntax);
	vps_syntax.num_layer_sets_minus1 = 2U;
	vps_syntax.timing_present = 1U;
	vps_syntax.num_units_in_tick = 1U;
	vps_syntax.time_scale = 1U;
	vps_syntax.num_hrd_parameters = 4U;
	for (first = 0U; first < 4U; first++) {
		hrd_fill(&vps_syntax.hrd[first], first, 1U, 0U);
		vps_syntax.hrd_layer_set_idx[first] = first;
		vps_syntax.cprms_present[first] = 1U;
	}
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);
	vps_syntax.num_layer_sets_minus1 = 3U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == 0);
	CHECK(store.vps[0].num_hrd_parameters == 4U);

	/* The extension data is read over, of any length up to 32 bits. */
	for (index = 0U; index < 7U; index++) {
		bits = lengths[index];
		h265sw_ps_init(&store);
		h265w_vps_default(&vps_syntax);
		vps_syntax.extension = 1U;
		vps_syntax.extension_data_bits = bits;
		vps_syntax.extension_data = random_next();
		CHECK(do_vps(&vps_syntax, (size_t)-1) == 0);
		CHECK(store.vps[0].extension == 1);
	}
}

/* Checks the derived short-term set of an explicit st_ref_pic_set written with the syntax values (7-63 to 7-70 by hand). */
static void
check_rps_explicit(
	const struct h265w_st_rps *syntax,
	const struct h265sw_sps *read,
	unsigned index)
{
	unsigned entry;
	int32_t delta;

	CHECK(read->st_rps[index].num_negative == syntax->num_negative_pics);
	CHECK(read->st_rps[index].num_positive == syntax->num_positive_pics);
	CHECK(read->st_syntax[index].inter_rps_pred == 0);
	CHECK(read->st_syntax[index].num_negative_pics == syntax->num_negative_pics);
	delta = 0;
	for (entry = 0U; entry < syntax->num_negative_pics; entry++) {
		delta -= (int32_t)syntax->delta_poc_s0_minus1[entry] + 1;
		CHECK(read->st_rps[index].delta_poc[entry] == delta);
		CHECK(read->st_rps[index].used[entry] == syntax->used_by_curr_pic_s0_flag[entry]);
		CHECK(read->st_syntax[index].delta_poc_s0_minus1[entry] == syntax->delta_poc_s0_minus1[entry]);
	}
	delta = 0;
	for (entry = 0U; entry < syntax->num_positive_pics; entry++) {
		delta += (int32_t)syntax->delta_poc_s1_minus1[entry] + 1;
		CHECK(read->st_rps[index].delta_poc[syntax->num_negative_pics + entry] == delta);
		CHECK(read->st_rps[index].used[syntax->num_negative_pics + entry] == syntax->used_by_curr_pic_s1_flag[entry]);
		CHECK(read->st_syntax[index].delta_poc_s1_minus1[entry] == syntax->delta_poc_s1_minus1[entry]);
	}
}

/* Checks the scaling lists of a read SPS: zero, the defaults, or the lists the syntax coded. */
static void
check_sps_scaling(
	const struct h265w_sps *syntax,
	const struct h265sw_sps *read)
{
	struct h265sw_scaling_list expected;
	unsigned size_id;
	unsigned matrix_id;
	unsigned entries;

	/* Without scaling lists the structure is all zero. */
	memset(&expected, 0, sizeof(expected));
	if (syntax->scaling_list_enabled != 0U && syntax->scaling_list_data_present == 0U) {
		h265sw_scaling_default(&expected);
		CHECK(memcmp(&read->scaling, &expected, sizeof(expected)) == 0);
	} else if (syntax->scaling_list_enabled != 0U) {
		/* The coded lists are the ones written. */
		for (size_id = 0U; size_id < 4U; size_id++) {
			entries = (size_id == 0U) ? 16U : 64U;
			for (matrix_id = 0U; matrix_id < 6U; matrix_id += (size_id == 3U) ? 3U : 1U) {
				CHECK(memcmp(read->scaling.list[size_id][matrix_id], random_lists[size_id][matrix_id], entries) == 0);
				if (size_id > 1U)
					CHECK(read->scaling.dc[size_id - 2U][matrix_id] == random_dc[size_id - 2U][matrix_id]);
			}
		}
	} else {
		CHECK(memcmp(&read->scaling, &expected, sizeof(expected)) == 0);
	}
}

/* A default SPS and the fields and derived values it gives. */
static void
check_sps_default(void)
{
	int error;

	h265sw_ps_init(&store);
	h265w_sps_default(&sps_syntax);
	error = do_sps(&sps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.sps_present[0] == 1 && store.sps_refused[0] == 0);
	CHECK(store.generation == 1U && store.sps_generation[0] == 1U);
	sps_check_syntax(&sps_syntax, &store.sps[0]);

	/* The default is 64x64, CTB 16, minimum coding block 8: 4x4 CTBs and 8x8 minimum blocks (7-14 to 7-19). */
	CHECK(store.sps[0].width == 64U && store.sps[0].height == 64U);
	CHECK(store.sps[0].min_cb_log2 == 3U && store.sps[0].ctb_log2 == 4U);
	CHECK(store.sps[0].min_tb_log2 == 2U && store.sps[0].max_tb_log2 == 4U);
	CHECK(store.sps[0].pic_width_ctbs == 4U && store.sps[0].pic_height_ctbs == 4U && store.sps[0].pic_size_ctbs == 16U);
	CHECK(store.sps[0].pic_width_min_cbs == 8U && store.sps[0].pic_height_min_cbs == 8U);
	CHECK(store.sps[0].crop_x == 0U && store.sps[0].crop_y == 0U);
	CHECK(store.sps[0].crop_width == 64U && store.sps[0].crop_height == 64U);
	CHECK(store.sps[0].ptl.profile_idc == 1U && store.sps[0].ptl.level_idc == 93U);
	CHECK(store.sps[0].log2_max_poc_lsb == 8U);
	CHECK(store.sps[0].max_dec_pic_buffering_minus1[0] == 4U);
	CHECK(store.sps[0].num_st_rps == 0U && store.sps[0].num_lt_sps == 0U);
	CHECK(store.sps[0].vui_present == 0 && store.sps[0].extension_present == 0);
}

/* Sub-layer ordering of every shape, the scaling list cases, a small VUI and the range extension flags. */
static void
check_sps_branches(void)
{
	unsigned max_sub;
	unsigned present;
	unsigned index;
	unsigned scaling_case;
	int error;

	/* 1..7 sub-layers with and without per-sub-layer values; the profile part has sub-layer parts too. */
	for (max_sub = 0U; max_sub <= 6U; max_sub++) {
		for (present = 0U; present <= 1U; present++) {
			h265sw_ps_init(&store);
			h265w_sps_default(&sps_syntax);
			sps_syntax.max_sub_layers_minus1 = max_sub;
			sps_syntax.sub_layer_ordering_info_present = present;
			for (index = 0U; index < max_sub; index++) {
				sps_syntax.ptl.profile_present[index] = index & 1U;
				sps_syntax.ptl.level_present[index] = (index >> 1) & 1U;
			}
			for (index = 0U; index <= max_sub; index++) {
				sps_syntax.max_dec_pic_buffering_minus1[index] = 5U + index;
				sps_syntax.max_num_reorder_pics[index] = 1U + index;
				sps_syntax.max_latency_increase_plus1[index] = 20U + index;
			}
			error = do_sps(&sps_syntax, (size_t)-1);
			CHECK(error == 0);
			sps_check_syntax(&sps_syntax, &store.sps[0]);
			CHECK(store.sps[0].ptl.level_idc == 93U);
		}
	}

	/* Scaling lists: not enabled, enabled with the defaults, and signalled. */
	for (scaling_case = 0U; scaling_case < 3U; scaling_case++) {
		h265sw_ps_init(&store);
		sps_random(&sps_syntax);
		sps_syntax.scaling_list_enabled = (scaling_case == 0U) ? 0U : 1U;
		sps_syntax.scaling_list_data_present = (scaling_case == 2U) ? 1U : 0U;
		if (scaling_case == 2U) {
			static uint8_t lists[4][6][64];
			static uint8_t dc[2][6];
			unsigned size_id;
			unsigned matrix_id;
			unsigned entry;

			memset(lists, 0, sizeof(lists));
			memset(dc, 0, sizeof(dc));
			for (size_id = 0U; size_id < 4U; size_id++) {
				for (matrix_id = 0U; matrix_id < 6U; matrix_id += (size_id == 3U) ? 3U : 1U) {
					for (entry = 0U; entry < 64U; entry++)
						lists[size_id][matrix_id][entry] = (uint8_t)(1U + (size_id * 50U + matrix_id * 7U + entry) % 255U);
					dc[0][matrix_id] = (uint8_t)(16U + matrix_id);
					dc[1][matrix_id] = (uint8_t)(200U - matrix_id);
				}
			}
			memcpy(random_lists, lists, sizeof(lists));
			memcpy(random_dc, dc, sizeof(dc));
			h265w_scaling_from_lists(&sps_syntax.scaling, (const uint8_t (*)[6][64])lists, (const uint8_t (*)[6])dc);
		}
		error = do_sps(&sps_syntax, (size_t)-1);
		CHECK(error == 0);
		sps_check_syntax(&sps_syntax, &store.sps[sps_syntax.sps_id]);
		check_sps_scaling(&sps_syntax, &store.sps[sps_syntax.sps_id]);
	}
}

/* The derived sizes against hand-worked cases (7-14 to 7-19, Table 6-1, the window of 7.4.3.2.1). */
static void
check_sps_derived(void)
{
	int error;

	/*
	 * 1920x1080, 4:2:0, minimum coding block 8, CTB 64, bottom window offset 4:
	 * SubHeightC 2 gives a 1072-sample high output at y 0; CTBs
	 * Ceil( 1920 / 64 ) = 30 by Ceil( 1080 / 64 ) = 17 (16.875), 510 in all;
	 * minimum blocks 240 by 135.
	 */
	h265sw_ps_init(&store);
	h265w_sps_default(&sps_syntax);
	sps_syntax.width = 1920U;
	sps_syntax.height = 1080U;
	sps_syntax.log2_diff_max_min_cb = 3U;
	sps_syntax.log2_diff_max_min_tb = 3U;
	sps_syntax.conformance_window = 1U;
	sps_syntax.conf_win_offset[3] = 4U;
	error = do_sps(&sps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.sps[0].crop_x == 0U && store.sps[0].crop_y == 0U);
	CHECK(store.sps[0].crop_width == 1920U && store.sps[0].crop_height == 1072U);
	CHECK(store.sps[0].pic_width_ctbs == 30U && store.sps[0].pic_height_ctbs == 17U);
	CHECK(store.sps[0].pic_size_ctbs == 510U);
	CHECK(store.sps[0].pic_width_min_cbs == 240U && store.sps[0].pic_height_min_cbs == 135U);
	CHECK(store.sps[0].ctb_log2 == 6U && store.sps[0].max_tb_log2 == 5U);

	/* 4:2:0 with offsets 1, 1, 1, 1 in units of 2: x 2, y 2, 60x60. */
	h265w_sps_default(&sps_syntax);
	sps_syntax.conformance_window = 1U;
	sps_syntax.conf_win_offset[0] = 1U;
	sps_syntax.conf_win_offset[1] = 1U;
	sps_syntax.conf_win_offset[2] = 1U;
	sps_syntax.conf_win_offset[3] = 1U;
	error = do_sps(&sps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.sps[0].crop_x == 2U && store.sps[0].crop_y == 2U);
	CHECK(store.sps[0].crop_width == 60U && store.sps[0].crop_height == 60U);

	/* 4:2:2: SubWidthC 2, SubHeightC 1; offsets 1, 2, 3, 4: x 2, y 3, 64 - 2 * 3 = 58 wide, 64 - 7 = 57 high. */
	h265w_sps_default(&sps_syntax);
	sps_syntax.chroma_format_idc = 2U;
	sps_syntax.conformance_window = 1U;
	sps_syntax.conf_win_offset[0] = 1U;
	sps_syntax.conf_win_offset[1] = 2U;
	sps_syntax.conf_win_offset[2] = 3U;
	sps_syntax.conf_win_offset[3] = 4U;
	error = do_sps(&sps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.sps[0].crop_x == 2U && store.sps[0].crop_y == 3U);
	CHECK(store.sps[0].crop_width == 58U && store.sps[0].crop_height == 57U);

	/* 4:4:4 (with and without separate planes) and monochrome: SubWidthC and SubHeightC 1; offsets 5, 6, 7, 8: x 5, y 7, 53x49. */
	{
		static const unsigned formats[4][2] = {{3U, 0U}, {3U, 1U}, {0U, 0U}, {3U, 0U}};
		unsigned index;

		for (index = 0U; index < 4U; index++) {
			h265w_sps_default(&sps_syntax);
			sps_syntax.chroma_format_idc = formats[index][0];
			sps_syntax.separate_colour_plane = formats[index][1];
			sps_syntax.conformance_window = 1U;
			sps_syntax.conf_win_offset[0] = 5U;
			sps_syntax.conf_win_offset[1] = 6U;
			sps_syntax.conf_win_offset[2] = 7U;
			sps_syntax.conf_win_offset[3] = 8U;
			error = do_sps(&sps_syntax, (size_t)-1);
			CHECK(error == 0);
			CHECK(store.sps[0].crop_x == 5U && store.sps[0].crop_y == 7U);
			CHECK(store.sps[0].crop_width == 53U && store.sps[0].crop_height == 49U);
			CHECK(store.sps[0].chroma_format_idc == formats[index][0]);
			CHECK(store.sps[0].separate_colour_plane == (int)formats[index][1]);
		}
	}

	/* An 8x24 picture, CTB 16 (a partial CTB): 1 by Ceil( 24 / 16 ) = 2 CTBs, 1 by 3 minimum blocks. */
	h265w_sps_default(&sps_syntax);
	sps_syntax.width = 8U;
	sps_syntax.height = 24U;
	error = do_sps(&sps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.sps[0].pic_width_ctbs == 1U && store.sps[0].pic_height_ctbs == 2U && store.sps[0].pic_size_ctbs == 2U);
	CHECK(store.sps[0].pic_width_min_cbs == 1U && store.sps[0].pic_height_min_cbs == 3U);

	/* Minimum coding block 64 and CTB 64: 192x128 is 3 by 2 CTBs and 3 by 2 minimum blocks. */
	h265w_sps_default(&sps_syntax);
	sps_syntax.log2_min_cb_minus3 = 3U;
	sps_syntax.log2_diff_max_min_cb = 0U;
	sps_syntax.log2_min_tb_minus2 = 3U;
	sps_syntax.log2_diff_max_min_tb = 0U;
	sps_syntax.max_th_depth_inter = 1U;
	sps_syntax.max_th_depth_intra = 1U;
	sps_syntax.width = 192U;
	sps_syntax.height = 128U;
	error = do_sps(&sps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.sps[0].min_cb_log2 == 6U && store.sps[0].ctb_log2 == 6U && store.sps[0].min_tb_log2 == 5U && store.sps[0].max_tb_log2 == 5U);
	CHECK(store.sps[0].pic_width_ctbs == 3U && store.sps[0].pic_height_ctbs == 2U);
	CHECK(store.sps[0].pic_width_min_cbs == 3U && store.sps[0].pic_height_min_cbs == 2U);
}

/* The SPS valid block sizes with a given minimum coding block and CTB (transform sizes at their smallest, 64x64 picture). */
static void
sps_blocks(
	struct h265w_sps *sps,
	uint32_t min_cb_minus3,
	uint32_t diff_cb)
{
	h265w_sps_default(sps);
	sps->log2_min_cb_minus3 = min_cb_minus3;
	sps->log2_diff_max_min_cb = diff_cb;
	sps->log2_min_tb_minus2 = 0U;
	sps->log2_diff_max_min_tb = 0U;
	sps->max_th_depth_inter = 0U;
	sps->max_th_depth_intra = 0U;
}

/* The picture size and the window: multiples of the minimum block, the limits, and the products. */
static void
check_sps_sizes(void)
{
	struct size_case {
		uint32_t width;
		uint32_t height;
		uint32_t min_cb_minus3;
		int expected;
	};
	static const struct size_case cases[] = {
		{64U, 64U, 0U, 0},
		{0U, 64U, 0U, EINVAL},
		{64U, 0U, 0U, EINVAL},
		{65U, 64U, 0U, EINVAL},
		{64U, 65U, 0U, EINVAL},
		{72U, 64U, 1U, EINVAL},
		{80U, 64U, 1U, 0},
		{16888U, 8U, 0U, 0},
		{16896U, 8U, 0U, EINVAL},
		{8U, 16888U, 0U, 0},
		{8U, 16896U, 0U, EINVAL},
		{16889U, 8U, 0U, EINVAL},
		{8192U, 4352U, 0U, 0},
		{8192U, 4360U, 0U, EINVAL},
		{16888U, 2104U, 0U, 0},
		{16888U, 2112U, 0U, EINVAL},
		{128U, 128U, 3U, 0},
		{64U, 64U, 3U, 0},
		{96U, 64U, 3U, EINVAL},
		{0x80000000U, 8U, 0U, EINVAL},
		{8U, 0x80000000U, 0U, EINVAL},
		{0xFFFFFFFEU, 8U, 0U, EINVAL},
		{0xFFFFFFF8U, 0xFFFFFFF8U, 0U, EINVAL}
	};
	unsigned index;
	unsigned offset_index;
	unsigned width;
	unsigned height;
	int error;
	int expected;

	for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++) {
		sps_blocks(&sps_syntax, cases[index].min_cb_minus3, (cases[index].min_cb_minus3 == 0U) ? 1U : 0U);
		sps_syntax.width = cases[index].width;
		sps_syntax.height = cases[index].height;
		error = do_sps(&sps_syntax, (size_t)-1);
		CHECK(error == cases[index].expected);
	}

	/* The window leaves one chroma unit: 4:2:0, 64x64 allows 31 units across (62 < 64) and not 32. */
	for (offset_index = 0U; offset_index < 4U; offset_index++) {
		for (width = 0U; width <= 3U; width++) {
			h265w_sps_default(&sps_syntax);
			sps_syntax.conformance_window = 1U;
			sps_syntax.conf_win_offset[offset_index] = (width == 0U) ? 0U : (width == 1U) ? 31U : (width == 2U) ? 32U : 0x80000000U;
			error = do_sps(&sps_syntax, (size_t)-1);
			expected = (width <= 1U) ? 0 : EINVAL;
			CHECK(error == expected);
		}
	}

	/* The two sides together: 15 + 16 is 31 units, 16 + 16 is 32; a sum that would wrap 32 bits is refused. */
	for (height = 0U; height < 2U; height++) {
		h265w_sps_default(&sps_syntax);
		sps_syntax.conformance_window = 1U;
		sps_syntax.conf_win_offset[0 + 2U * height] = 15U;
		sps_syntax.conf_win_offset[1 + 2U * height] = 16U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
		sps_syntax.conf_win_offset[0 + 2U * height] = 16U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
		sps_syntax.conf_win_offset[0 + 2U * height] = 0x80000000U;
		sps_syntax.conf_win_offset[1 + 2U * height] = 0x80000000U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	}

	/* 4:4:4 takes whole samples: 63 across leaves one, 64 leaves none. */
	h265w_sps_default(&sps_syntax);
	sps_syntax.chroma_format_idc = 3U;
	sps_syntax.conformance_window = 1U;
	sps_syntax.conf_win_offset[0] = 63U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.sps[0].crop_x == 63U && store.sps[0].crop_width == 1U);
	sps_syntax.conf_win_offset[0] = 64U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
}

/* The log2 syntax elements: bounds, and ue(v) values that would wrap a sum. */
static void
check_sps_log2(void)
{
	static const uint32_t huge[] = {0x80000000U, 0xFFFFFFFDU, 0xFFFFFFFEU};
	unsigned min_cb_minus3;
	unsigned diff_cb;
	unsigned min_cb;
	unsigned ctb;
	unsigned min_tb_minus2;
	unsigned limit;
	unsigned index;
	unsigned other;
	unsigned values;
	int error;
	int expected;

	/* Every pair of the two coding block syntax elements 0..4: valid when CtbLog2SizeY is 4..6, else not supported. */
	for (min_cb_minus3 = 0U; min_cb_minus3 <= 4U; min_cb_minus3++) {
		for (diff_cb = 0U; diff_cb <= 4U; diff_cb++) {
			h265sw_ps_init(&store);
			sps_blocks(&sps_syntax, min_cb_minus3, diff_cb);
			sps_syntax.sps_id = 3U;
			sps_syntax.width = 64U;
			sps_syntax.height = 64U;
			ctb = min_cb_minus3 + 3U + diff_cb;
			expected = 0;
			if (min_cb_minus3 > 3U || diff_cb > 3U || ctb < 4U || ctb > 6U)
				expected = ENOTSUP;
			error = do_sps(&sps_syntax, (size_t)-1);
			CHECK(error == expected);
			if (expected == ENOTSUP) {
				CHECK(store.sps_refused[3] == 1 && store.sps_present[3] == 0);
				CHECK(store.generation == 0U);
			} else {
				CHECK(store.sps_refused[3] == 0 && store.sps_present[3] == 1);
				CHECK(store.sps[3].ctb_log2 == ctb && store.sps[3].min_cb_log2 == min_cb_minus3 + 3U);
			}
		}
	}

	/* Large ue(v) values, alone and in pairs that would wrap 32-bit sums (to 0..2): not supported, never accepted. */
	for (index = 0U; index < 3U; index++) {
		for (other = 0U; other < 7U; other++) {
			sps_blocks(&sps_syntax, huge[index], (other < 4U) ? other : huge[other - 4U]);
			CHECK(do_sps(&sps_syntax, (size_t)-1) == ENOTSUP);
			sps_blocks(&sps_syntax, (other < 4U) ? other : huge[other - 4U], huge[index]);
			CHECK(do_sps(&sps_syntax, (size_t)-1) == ENOTSUP);
		}
	}
	sps_blocks(&sps_syntax, 0xFFFFFFFDU, 4U);
	CHECK(do_sps(&sps_syntax, (size_t)-1) == ENOTSUP);

	/* The transform block sizes for each minimum coding block with CTB 64. */
	for (min_cb = 3U; min_cb <= 6U; min_cb++) {
		for (min_tb_minus2 = 0U; min_tb_minus2 <= 5U; min_tb_minus2++) {
			sps_blocks(&sps_syntax, min_cb - 3U, 6U - min_cb);
			sps_syntax.log2_min_tb_minus2 = min_tb_minus2;
			expected = (min_tb_minus2 + 2U < min_cb) ? 0 : EINVAL;
			error = do_sps(&sps_syntax, (size_t)-1);
			CHECK(error == expected);
		}
		sps_blocks(&sps_syntax, min_cb - 3U, 6U - min_cb);
		sps_syntax.log2_min_tb_minus2 = 0x80000000U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);

		/* The largest transform block: Min( CTB, 5 ) - MinTb above the smallest, one more is refused. */
		for (min_tb_minus2 = 0U; min_tb_minus2 + 2U < min_cb; min_tb_minus2++) {
			limit = 5U - (min_tb_minus2 + 2U);
			for (values = 0U; values <= limit + 1U; values++) {
				sps_blocks(&sps_syntax, min_cb - 3U, 6U - min_cb);
				sps_syntax.log2_min_tb_minus2 = min_tb_minus2;
				sps_syntax.log2_diff_max_min_tb = values;
				expected = (values <= limit) ? 0 : EINVAL;
				CHECK(do_sps(&sps_syntax, (size_t)-1) == expected);
			}
			sps_blocks(&sps_syntax, min_cb - 3U, 6U - min_cb);
			sps_syntax.log2_min_tb_minus2 = min_tb_minus2;
			sps_syntax.log2_diff_max_min_tb = 0x80000000U;
			CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);

			/* The hierarchy depths: up to CtbLog2SizeY - MinTb, inter and intra alike. */
			limit = 6U - (min_tb_minus2 + 2U);
			for (values = 0U; values <= limit + 1U; values++) {
				sps_blocks(&sps_syntax, min_cb - 3U, 6U - min_cb);
				sps_syntax.log2_min_tb_minus2 = min_tb_minus2;
				sps_syntax.max_th_depth_inter = values;
				expected = (values <= limit) ? 0 : EINVAL;
				CHECK(do_sps(&sps_syntax, (size_t)-1) == expected);
				sps_blocks(&sps_syntax, min_cb - 3U, 6U - min_cb);
				sps_syntax.log2_min_tb_minus2 = min_tb_minus2;
				sps_syntax.max_th_depth_intra = values;
				CHECK(do_sps(&sps_syntax, (size_t)-1) == expected);
			}
			sps_blocks(&sps_syntax, min_cb - 3U, 6U - min_cb);
			sps_syntax.log2_min_tb_minus2 = min_tb_minus2;
			sps_syntax.max_th_depth_inter = 0x80000000U;
			CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
		}
	}

	/* A CTB of 16 caps the transform size at 16 (Min( 4, 5 )): MinTb 2 allows a difference of 2, not 3. */
	sps_blocks(&sps_syntax, 0U, 1U);
	sps_syntax.log2_diff_max_min_tb = 2U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	sps_syntax.log2_diff_max_min_tb = 3U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);

	/* The order count length: log2_max_pic_order_cnt_lsb_minus4 0 and 12 are valid, 13 is not. */
	for (values = 0U; values <= 14U; values++) {
		h265w_sps_default(&sps_syntax);
		sps_syntax.log2_max_poc_lsb_minus4 = values;
		expected = (values <= 12U) ? 0 : EINVAL;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == expected);
		if (expected == 0)
			CHECK(store.sps[0].log2_max_poc_lsb == values + 4U);
	}
	h265w_sps_default(&sps_syntax);
	sps_syntax.log2_max_poc_lsb_minus4 = 0x80000000U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);

	/* The buffering: 15 is the most, a reorder depth is at most the buffer. */
	h265w_sps_default(&sps_syntax);
	sps_syntax.max_dec_pic_buffering_minus1[0] = 15U;
	sps_syntax.max_num_reorder_pics[0] = 15U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	sps_syntax.max_dec_pic_buffering_minus1[0] = 16U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	sps_syntax.max_dec_pic_buffering_minus1[0] = 0x80000000U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	sps_syntax.max_dec_pic_buffering_minus1[0] = 4U;
	sps_syntax.max_num_reorder_pics[0] = 5U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	sps_syntax.max_num_reorder_pics[0] = 4U;
	sps_syntax.max_latency_increase_plus1[0] = 0xFFFFFFFEU;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.sps[0].max_latency_increase_plus1[0] == 0xFFFFFFFEU);

	/* Formats, depths and ids at their edges. */
	h265w_sps_default(&sps_syntax);
	sps_syntax.chroma_format_idc = 4U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	sps_syntax.chroma_format_idc = 0x80000000U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	h265w_sps_default(&sps_syntax);
	sps_syntax.bit_depth_luma_minus8 = 8U;
	sps_syntax.bit_depth_chroma_minus8 = 8U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.sps[0].bit_depth_luma == 16U && store.sps[0].bit_depth_chroma == 16U);
	sps_syntax.bit_depth_luma_minus8 = 9U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	sps_syntax.bit_depth_luma_minus8 = 0U;
	sps_syntax.bit_depth_chroma_minus8 = 9U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	h265w_sps_default(&sps_syntax);
	sps_syntax.sps_id = 15U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.sps_present[15] == 1);
	sps_syntax.sps_id = 16U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	sps_syntax.sps_id = 0x80000000U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	h265w_sps_default(&sps_syntax);
	sps_syntax.max_sub_layers_minus1 = 7U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
}

/* PCM: the sample depths, and the block size range Min( MinCbLog2SizeY, 5 )..Min( CtbLog2SizeY, 5 ). */
static void
check_sps_pcm(void)
{
	unsigned min_cb;
	unsigned ctb;
	unsigned pcm_min_minus3;
	unsigned diff;
	unsigned low;
	unsigned high;
	unsigned pcm_min;
	unsigned format;
	int error;
	int expected;

	/* Each minimum coding block with CTB 64 (and CTB 16 for the smallest): every PCM minimum 3..6 and difference 0..3. */
	for (min_cb = 3U; min_cb <= 6U; min_cb++) {
		for (ctb = min_cb; ctb <= 6U; ctb++) {
			if (ctb < 4U)
				continue;
			for (pcm_min_minus3 = 0U; pcm_min_minus3 <= 3U; pcm_min_minus3++) {
				for (diff = 0U; diff <= 3U; diff++) {
					sps_blocks(&sps_syntax, min_cb - 3U, ctb - min_cb);
					sps_syntax.pcm = 1U;
					sps_syntax.pcm_bit_depth_luma_minus1 = 7U;
					sps_syntax.pcm_bit_depth_chroma_minus1 = 7U;
					sps_syntax.log2_min_pcm_minus3 = pcm_min_minus3;
					sps_syntax.log2_diff_max_min_pcm = diff;
					low = (min_cb < 5U) ? min_cb : 5U;
					high = (ctb < 5U) ? ctb : 5U;
					pcm_min = pcm_min_minus3 + 3U;
					expected = 0;
					if (pcm_min < low || pcm_min > high || pcm_min + diff > high)
						expected = EINVAL;
					error = do_sps(&sps_syntax, (size_t)-1);
					CHECK(error == expected);
					if (expected == 0) {
						CHECK(store.sps[0].pcm_min_log2 == pcm_min);
						CHECK(store.sps[0].pcm_max_log2 == pcm_min + diff);
					}
				}
			}
		}
	}

	/* ue(v) values that would wrap: refused. */
	sps_blocks(&sps_syntax, 0U, 3U);
	sps_syntax.pcm = 1U;
	sps_syntax.pcm_bit_depth_luma_minus1 = 7U;
	sps_syntax.pcm_bit_depth_chroma_minus1 = 7U;
	sps_syntax.log2_min_pcm_minus3 = 0x80000000U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	sps_syntax.log2_min_pcm_minus3 = 0xFFFFFFFDU;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	sps_syntax.log2_min_pcm_minus3 = 0U;
	sps_syntax.log2_diff_max_min_pcm = 0x80000000U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	sps_syntax.log2_diff_max_min_pcm = 0xFFFFFFFEU;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);

	/* Sample depths at most the bit depths: luma 8 takes 8 and not 9; luma 10 takes 10; 16 takes 16. */
	for (diff = 0U; diff <= 8U; diff += 2U) {
		h265w_sps_default(&sps_syntax);
		sps_syntax.bit_depth_luma_minus8 = diff;
		sps_syntax.pcm = 1U;
		sps_syntax.pcm_bit_depth_luma_minus1 = 7U + diff;
		sps_syntax.log2_min_pcm_minus3 = 0U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
		CHECK(store.sps[0].pcm_bit_depth_luma == 8U + diff);
		if (diff < 8U) {
			sps_syntax.pcm_bit_depth_luma_minus1 = 8U + diff;
			CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
		}
	}

	/* The chroma sample depth is checked for 4:2:0, 4:2:2 and 4:4:4, and ignored without chroma (monochrome and separate planes). */
	for (format = 0U; format < 5U; format++) {
		h265w_sps_default(&sps_syntax);
		sps_syntax.chroma_format_idc = (format == 0U) ? 0U : (format == 1U) ? 1U : (format == 2U) ? 2U : 3U;
		sps_syntax.separate_colour_plane = (format == 4U) ? 1U : 0U;
		sps_syntax.pcm = 1U;
		sps_syntax.pcm_bit_depth_luma_minus1 = 7U;
		sps_syntax.pcm_bit_depth_chroma_minus1 = 7U;
		sps_syntax.log2_min_pcm_minus3 = 0U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
		sps_syntax.pcm_bit_depth_chroma_minus1 = 8U;
		expected = (format == 0U || format == 4U) ? 0 : EINVAL;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == expected);
	}
}

/* The short-term set chain, the long-term pictures and where the sets read their bound from. */
static void
check_sps_sets(void)
{
	static const unsigned counts[] = {0U, 1U, 2U, 3U, 7U, 64U};
	unsigned count_index;
	unsigned count;
	unsigned index;
	unsigned lt_bits;
	unsigned lsb_value;
	int error;

	/* Chains of 0, 1, 2, 3, 7 and 64 sets: A, then B and C alternately (derived by hand in rps_chain's comment). */
	for (count_index = 0U; count_index < sizeof(counts) / sizeof(counts[0]); count_index++) {
		count = counts[count_index];
		h265sw_ps_init(&store);
		h265w_sps_default(&sps_syntax);
		rps_chain(&sps_syntax, count);
		error = do_sps(&sps_syntax, (size_t)-1);
		CHECK(error == 0);
		CHECK(store.sps[0].num_st_rps == count);
		for (index = 0U; index < count; index++) {
			if (index == 0U) {
				CHECK(store.sps[0].st_rps[0].num_negative == 1U && store.sps[0].st_rps[0].num_positive == 0U);
				CHECK(store.sps[0].st_rps[0].delta_poc[0] == -1 && store.sps[0].st_rps[0].used[0] == 1U);
				CHECK(store.sps[0].st_syntax[0].inter_rps_pred == 0);
			} else if ((index & 1U) != 0U) {
				/* B: S0 -1, -2, both used. */
				CHECK(store.sps[0].st_rps[index].num_negative == 2U && store.sps[0].st_rps[index].num_positive == 0U);
				CHECK(store.sps[0].st_rps[index].delta_poc[0] == -1 && store.sps[0].st_rps[index].delta_poc[1] == -2);
				CHECK(store.sps[0].st_rps[index].used[0] == 1U && store.sps[0].st_rps[index].used[1] == 1U);
				CHECK(store.sps[0].st_syntax[index].inter_rps_pred == 1);
				CHECK(store.sps[0].st_syntax[index].delta_idx_minus1 == 0U);
				CHECK(store.sps[0].st_syntax[index].delta_rps_sign == 1);
			} else {
				/* C: S0 -1, S1 +1. */
				CHECK(store.sps[0].st_rps[index].num_negative == 1U && store.sps[0].st_rps[index].num_positive == 1U);
				CHECK(store.sps[0].st_rps[index].delta_poc[0] == -1 && store.sps[0].st_rps[index].delta_poc[1] == 1);
				CHECK(store.sps[0].st_rps[index].used[0] == 1U && store.sps[0].st_rps[index].used[1] == 1U);
				CHECK(store.sps[0].st_syntax[index].delta_rps_sign == 0);
			}
		}
		/* Entries past the sets are 0. */
		if (count < 64U)
			CHECK(store.sps[0].st_rps[count].num_negative == 0U && store.sps[0].st_rps[count].delta_poc[0] == 0);
	}

	/* 65 sets is refused. */
	h265w_sps_default(&sps_syntax);
	rps_chain(&sps_syntax, 64U);
	sps_syntax.num_st_rps = 65U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);

	/* The bound of the sets is the buffering of the highest sub-layer: the chain needs two entries. */
	h265w_sps_default(&sps_syntax);
	rps_chain(&sps_syntax, 2U);
	sps_syntax.max_dec_pic_buffering_minus1[0] = 2U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	sps_syntax.max_dec_pic_buffering_minus1[0] = 1U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	rps_chain(&sps_syntax, 1U);
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	sps_syntax.max_dec_pic_buffering_minus1[0] = 0U;
	sps_syntax.max_num_reorder_pics[0] = 0U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);

	/* With two sub-layers the highest one's buffering is the bound: 1 below and 3 above is enough for B, 3 below and 1 above is not. */
	h265w_sps_default(&sps_syntax);
	sps_syntax.max_sub_layers_minus1 = 1U;
	sps_syntax.sub_layer_ordering_info_present = 1U;
	rps_chain(&sps_syntax, 2U);
	sps_syntax.max_dec_pic_buffering_minus1[0] = 1U;
	sps_syntax.max_dec_pic_buffering_minus1[1] = 3U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	sps_syntax.max_dec_pic_buffering_minus1[0] = 3U;
	sps_syntax.max_dec_pic_buffering_minus1[1] = 1U;
	sps_syntax.max_num_reorder_pics[0] = 0U;
	sps_syntax.max_num_reorder_pics[1] = 0U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);

	/* The long-term pictures: none, one, 32 and 33; the order count bits are log2_max_pic_order_cnt_lsb. */
	for (lt_bits = 4U; lt_bits <= 16U; lt_bits += 12U) {
		h265sw_ps_init(&store);
		h265w_sps_default(&sps_syntax);
		sps_syntax.log2_max_poc_lsb_minus4 = lt_bits - 4U;
		sps_syntax.long_term_present = 1U;
		sps_syntax.num_lt_sps = 32U;
		lsb_value = (1U << lt_bits) - 1U;
		for (index = 0U; index < 32U; index++) {
			sps_syntax.lt_poc_lsb[index] = (index & 1U) ? lsb_value : (index & 7U);
			sps_syntax.lt_used[index] = index & 1U;
		}
		error = do_sps(&sps_syntax, (size_t)-1);
		CHECK(error == 0);
		CHECK(store.sps[0].long_term_present == 1 && store.sps[0].num_lt_sps == 32U);
		for (index = 0U; index < 32U; index++) {
			CHECK(store.sps[0].lt_poc_lsb[index] == ((index & 1U) ? lsb_value : (index & 7U)));
			CHECK(store.sps[0].lt_used[index] == (index & 1U));
		}
		sps_syntax.num_lt_sps = 0U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
		CHECK(store.sps[0].long_term_present == 1 && store.sps[0].num_lt_sps == 0U);
		sps_syntax.num_lt_sps = 33U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
		sps_syntax.num_lt_sps = 0x80000000U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	}
	h265w_sps_default(&sps_syntax);
	sps_syntax.num_lt_sps = 5U;
	sps_syntax.long_term_present = 0U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.sps[0].long_term_present == 0 && store.sps[0].num_lt_sps == 0U);
}

/* The range extension flags, the extensions that are not read, and the extension data. */
static void
check_sps_extensions(void)
{
	static const unsigned lengths[] = {0U, 1U, 13U, 32U};
	unsigned bit;
	unsigned kind;
	unsigned index;
	int error;

	/* Each of the nine flags alone, all of them, and none: every bit lands in its place. */
	for (bit = 0U; bit <= 10U; bit++) {
		h265sw_ps_init(&store);
		h265w_sps_default(&sps_syntax);
		sps_syntax.extension_present = 1U;
		sps_syntax.range_extension_flag = 1U;
		sps_syntax.range_extension = (bit < 9U) ? (1U << bit) : (bit == 9U) ? 0x1FFU : 0U;
		error = do_sps(&sps_syntax, (size_t)-1);
		CHECK(error == 0);
		CHECK(store.sps[0].extension_present == 1 && store.sps[0].range_extension_present == 1);
		CHECK(store.sps[0].range_extension == sps_syntax.range_extension);
		error = h265sw_ps_sps_supported(&store.sps[0]);
		CHECK(error == ((sps_syntax.range_extension == 0U) ? 0 : ENOTSUP));
	}

	/* An extension block with no range extension keeps the flag clear. */
	h265w_sps_default(&sps_syntax);
	sps_syntax.extension_present = 1U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.sps[0].extension_present == 1 && store.sps[0].range_extension_present == 0 && store.sps[0].range_extension == 0U);

	/* The multilayer, 3D and screen content extensions are not read: not supported, the id is marked refused, the store keeps its content. */
	for (kind = 0U; kind < 7U; kind++) {
		h265sw_ps_init(&store);
		h265w_sps_default(&sps_syntax);
		sps_syntax.sps_id = 2U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
		store_save();

		sps_syntax.extension_present = 1U;
		sps_syntax.range_extension_flag = (kind >= 4U) ? 1U : 0U;
		sps_syntax.multilayer_extension_flag = (kind == 0U || kind == 3U || kind == 4U) ? 1U : 0U;
		sps_syntax.extension_3d_flag = (kind == 1U || kind == 3U || kind == 5U) ? 1U : 0U;
		sps_syntax.scc_extension_flag = (kind == 2U || kind == 3U || kind == 6U) ? 1U : 0U;
		sps_syntax.width = 128U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == ENOTSUP);
		CHECK(store.sps_refused[2] == 1);
		CHECK(store_same_but_refused() != 0);
		CHECK(store.sps[2].width == 64U);

		/* A good set of the id clears the mark. */
		sps_syntax.extension_present = 0U;
		CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
		CHECK(store.sps_refused[2] == 0 && store.sps[2].width == 128U);
	}

	/* The extension data is read over (sps_extension_4bits not 0), of any length, with or without the range extension. */
	for (index = 0U; index < 4U; index++) {
		h265sw_ps_init(&store);
		h265w_sps_default(&sps_syntax);
		sps_syntax.extension_present = 1U;
		sps_syntax.range_extension_flag = index & 1U;
		sps_syntax.range_extension = 0x155U;
		sps_syntax.extension_4bits = 1U + index;
		sps_syntax.extension_data_bits = lengths[index];
		sps_syntax.extension_data = random_next();
		CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
		CHECK(store.sps[0].range_extension_present == (int)(index & 1U));
	}
}

/* The VUI: a read one, and one that fails to read, per profile (A.3.2 to A.3.4). */
static void
check_sps_vui(void)
{
	struct h265sw_vui zero_vui;
	unsigned profile;
	unsigned form;
	int error;
	int expected;

	/* A VUI with an extended aspect ratio and a restriction block. */
	h265sw_ps_init(&store);
	h265w_sps_default(&sps_syntax);
	sps_syntax.vui_present = 1U;
	sps_syntax.vui.aspect_ratio_present = 1U;
	sps_syntax.vui.aspect_ratio_idc = 255U;
	sps_syntax.vui.sar_width = 4U;
	sps_syntax.vui.sar_height = 3U;
	sps_syntax.vui.bitstream_restriction = 1U;
	sps_syntax.vui.max_bytes_per_pic_denom = 7U;
	sps_syntax.vui.max_bits_per_min_cu_denom = 9U;
	sps_syntax.vui.log2_max_mv_length_horizontal = 11U;
	sps_syntax.vui.log2_max_mv_length_vertical = 12U;
	error = do_sps(&sps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 1);
	CHECK(store.sps[0].vui.aspect_ratio_present == 1 && store.sps[0].vui.aspect_ratio_idc == 255U);
	CHECK(store.sps[0].vui.sar_width == 4U && store.sps[0].vui.sar_height == 3U);
	CHECK(store.sps[0].vui.bitstream_restriction == 1);
	CHECK(store.sps[0].vui.max_bytes_per_pic_denom == 7U && store.sps[0].vui.max_bits_per_min_cu_denom == 9U);
	CHECK(store.sps[0].vui.log2_max_mv_length_horizontal == 11U && store.sps[0].vui.log2_max_mv_length_vertical == 12U);

	/* The sub-layer count reaches the VUI's HRD parameters: a VUI with HRD parameters of three sub-layers reads. */
	h265sw_ps_init(&store);
	h265w_sps_default(&sps_syntax);
	sps_syntax.max_sub_layers_minus1 = 2U;
	sps_syntax.sub_layer_ordering_info_present = 1U;
	sps_syntax.max_dec_pic_buffering_minus1[0] = 4U;
	sps_syntax.max_dec_pic_buffering_minus1[1] = 4U;
	sps_syntax.max_dec_pic_buffering_minus1[2] = 4U;
	sps_syntax.vui_present = 1U;
	sps_syntax.vui.timing_present = 1U;
	sps_syntax.vui.num_units_in_tick = 1001U;
	sps_syntax.vui.time_scale = 30000U;
	sps_syntax.vui.hrd_present = 1U;
	hrd_fill(&sps_syntax.vui.hrd, 3U, 1U, 1U);
	sps_syntax.ptl.general.profile_idc = 4U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.sps[0].vui.hrd_present == 1 && store.sps[0].vui.hrd.fixed_pic_rate_general[2] == 1U);
	CHECK(store.sps[0].vui.hrd.nal[2].bit_rate_value_minus1[0] == 1003U);

	/* A VUI that breaks the syntax (a tick of 0) with extension flags after it: Main, Main 10 and Main Still Picture lose the VUI, the others fail. */
	memset(&zero_vui, 0, sizeof(zero_vui));
	for (profile = 0U; profile <= 9U; profile++) {
		h265sw_ps_init(&store);
		h265w_sps_default(&sps_syntax);
		sps_syntax.ptl.general.profile_idc = profile;
		sps_syntax.vui_present = 1U;
		sps_syntax.vui.timing_present = 1U;
		sps_syntax.vui.num_units_in_tick = 0U;
		sps_syntax.vui.time_scale = 25U;
		sps_syntax.extension_present = 1U;
		sps_syntax.range_extension_flag = 1U;
		sps_syntax.range_extension = 0x1FFU;
		expected = (profile >= 1U && profile <= 3U) ? 0 : EINVAL;
		error = do_sps(&sps_syntax, (size_t)-1);
		CHECK(error == expected);
		if (expected == 0) {
			CHECK(store.sps_present[0] == 1);
			CHECK(store.sps[0].vui_present == 0);
			CHECK(memcmp(&store.sps[0].vui, &zero_vui, sizeof(zero_vui)) == 0);
			CHECK(store.sps[0].extension_present == 0 && store.sps[0].range_extension_present == 0 && store.sps[0].range_extension == 0U);
			CHECK(store.sps[0].ptl.profile_idc == profile);
			CHECK(store.sps[0].width == 64U && store.sps[0].log2_max_poc_lsb == 8U);
		} else {
			CHECK(store.sps_present[0] == 0);
		}
	}

	/* An SPS cut inside its VUI: dropped for profile 1, an error for profile 4 (the HRD parameters make the VUI long). */
	for (form = 0U; form < 2U; form++) {
		h265sw_ps_init(&store);
		h265w_sps_default(&sps_syntax);
		sps_syntax.ptl.general.profile_idc = (form == 0U) ? 1U : 4U;
		sps_syntax.vui_present = 1U;
		sps_syntax.vui.timing_present = 1U;
		sps_syntax.vui.num_units_in_tick = 1001U;
		sps_syntax.vui.time_scale = 30000U;
		sps_syntax.vui.hrd_present = 1U;
		hrd_fill(&sps_syntax.vui.hrd, 4U, 1U, 1U);
		CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
		CHECK(store.sps[0].vui_present == 1);
		h265sw_ps_init(&store);
		error = do_sps(&sps_syntax, last_bytes - 4U);
		CHECK(error == ((form == 0U) ? 0 : EINVAL));
		if (form == 0U)
			CHECK(store.sps[0].vui_present == 0 && store.sps_present[0] == 1);
	}
}

/* h265sw_ps_sps_supported on hand-built sequences. */
static void
check_sps_supported(void)
{
	struct h265sw_sps sps;

	/* 4:2:0 at 8 bits, no tools: taken. */
	memset(&sps, 0, sizeof(sps));
	sps.chroma_format_idc = 1U;
	sps.bit_depth_luma = 8U;
	sps.bit_depth_chroma = 8U;
	CHECK(h265sw_ps_sps_supported(&sps) == 0);

	/* Each way out of the range. */
	sps.chroma_format_idc = 0U;
	CHECK(h265sw_ps_sps_supported(&sps) == ENOTSUP);
	sps.chroma_format_idc = 2U;
	CHECK(h265sw_ps_sps_supported(&sps) == ENOTSUP);
	sps.chroma_format_idc = 3U;
	CHECK(h265sw_ps_sps_supported(&sps) == ENOTSUP);
	sps.chroma_format_idc = 1U;
	sps.bit_depth_luma = 10U;
	CHECK(h265sw_ps_sps_supported(&sps) == ENOTSUP);
	sps.bit_depth_luma = 8U;
	sps.bit_depth_chroma = 10U;
	CHECK(h265sw_ps_sps_supported(&sps) == ENOTSUP);
	sps.bit_depth_chroma = 8U;
	sps.range_extension = 0x100U;
	CHECK(h265sw_ps_sps_supported(&sps) == ENOTSUP);
	sps.range_extension = 0U;
	sps.vui_present = 1;
	sps.vui.field_seq = 1;
	CHECK(h265sw_ps_sps_supported(&sps) == ENOTSUP);
	sps.vui.field_seq = 0;
	CHECK(h265sw_ps_sps_supported(&sps) == 0);
	sps.vui_present = 0;
	sps.vui.field_seq = 1;
	CHECK(h265sw_ps_sps_supported(&sps) == 0);
	CHECK(h265sw_ps_sps_supported(NULL) == EINVAL);
}

/* Random valid SPSs: every signalled field, the derived sizes, the sets and the scaling lists. */
static void
check_sps_random(void)
{
	unsigned round;
	unsigned index;
	unsigned sub_width;
	unsigned sub_height;
	unsigned ctb_size;
	unsigned id;
	int error;

	h265sw_ps_init(&store);
	for (round = 0U; round < 3000U; round++) {
		sps_random(&sps_syntax);
		error = do_sps(&sps_syntax, (size_t)-1);
		CHECK(error == 0);
		if (error != 0)
			continue;
		id = sps_syntax.sps_id;
		sps_check_syntax(&sps_syntax, &store.sps[id]);
		check_sps_scaling(&sps_syntax, &store.sps[id]);
		for (index = 0U; index < sps_syntax.num_st_rps; index++)
			check_rps_explicit(&sps_syntax.st_rps[index], &store.sps[id], index);

		/* The derived sizes by the formulas of 7-10 to 7-19 and Table 6-1 written again from the syntax. */
		sub_width = 1U;
		sub_height = 1U;
		if (sps_syntax.chroma_format_idc == 1U) {
			sub_width = 2U;
			sub_height = 2U;
		} else if (sps_syntax.chroma_format_idc == 2U) {
			sub_width = 2U;
		}
		ctb_size = 1U << (sps_syntax.log2_min_cb_minus3 + 3U + sps_syntax.log2_diff_max_min_cb);
		CHECK(store.sps[id].crop_x == sub_width * ((sps_syntax.conformance_window != 0U) ? sps_syntax.conf_win_offset[0] : 0U));
		CHECK(store.sps[id].crop_y == sub_height * ((sps_syntax.conformance_window != 0U) ? sps_syntax.conf_win_offset[2] : 0U));
		CHECK(store.sps[id].crop_width + store.sps[id].crop_x + sub_width * ((sps_syntax.conformance_window != 0U) ? sps_syntax.conf_win_offset[1] : 0U) == sps_syntax.width);
		CHECK(store.sps[id].crop_height + store.sps[id].crop_y + sub_height * ((sps_syntax.conformance_window != 0U) ? sps_syntax.conf_win_offset[3] : 0U) == sps_syntax.height);
		CHECK(store.sps[id].crop_width >= 1U && store.sps[id].crop_height >= 1U);
		CHECK(store.sps[id].pic_width_ctbs * ctb_size >= sps_syntax.width);
		CHECK((store.sps[id].pic_width_ctbs - 1U) * ctb_size < sps_syntax.width);
		CHECK(store.sps[id].pic_height_ctbs * ctb_size >= sps_syntax.height);
		CHECK((store.sps[id].pic_height_ctbs - 1U) * ctb_size < sps_syntax.height);
		CHECK(store.sps[id].pic_size_ctbs == store.sps[id].pic_width_ctbs * store.sps[id].pic_height_ctbs);
		CHECK(store.sps[id].pic_width_min_cbs * (1U << store.sps[id].min_cb_log2) == sps_syntax.width);
		CHECK(store.sps[id].pic_height_min_cbs * (1U << store.sps[id].min_cb_log2) == sps_syntax.height);
	}
}

/* A default PPS and the fields it gives. */
static void
check_pps_default(void)
{
	int error;

	h265sw_ps_init(&store);
	h265w_pps_default(&pps_syntax);
	error = do_pps(&pps_syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(store.pps_present[0] == 1 && store.pps_refused[0] == 0);
	CHECK(store.generation == 1U);
	CHECK(store.pps[0].id == 0U && store.pps[0].sps_id == 0U);
	CHECK(store.pps[0].dependent_slices == 0 && store.pps[0].output_flag_present == 0);
	CHECK(store.pps[0].num_extra_slice_header_bits == 0U);
	CHECK(store.pps[0].num_ref_idx_default[0] == 1U && store.pps[0].num_ref_idx_default[1] == 1U);
	CHECK(store.pps[0].init_qp == 26);
	CHECK(store.pps[0].cu_qp_delta == 0 && store.pps[0].diff_cu_qp_delta_depth == 0U);
	CHECK(store.pps[0].cb_qp_offset == 0 && store.pps[0].cr_qp_offset == 0);
	CHECK(store.pps[0].tiles == 0 && store.pps[0].tile_columns == 1U && store.pps[0].tile_rows == 1U);
	CHECK(store.pps[0].uniform_spacing == 1 && store.pps[0].loop_filter_across_tiles == 1);
	CHECK(store.pps[0].loop_filter_across_slices == 1);
	CHECK(store.pps[0].deblocking_control_present == 0 && store.pps[0].beta_offset_div2 == 0 && store.pps[0].tc_offset_div2 == 0);
	CHECK(store.pps[0].log2_parallel_merge_level == 2U);
	CHECK(store.pps[0].scaling_list_present == 0 && store.pps[0].extension_present == 0);
}

/* The ranges of the values the PPS alone bounds. */
static void
check_pps_ranges(void)
{
	unsigned index;
	unsigned value;
	int error;
	int32_t qp;
	int32_t offsets[5];

	/* Ids. */
	h265sw_ps_init(&store);
	h265w_pps_default(&pps_syntax);
	pps_syntax.pps_id = 63U;
	pps_syntax.sps_id = 15U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
	CHECK(store.pps_present[63] == 1 && store.pps[63].sps_id == 15U);
	pps_syntax.pps_id = 64U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
	pps_syntax.pps_id = 0x80000000U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
	pps_syntax.pps_id = 0U;
	pps_syntax.sps_id = 16U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
	pps_syntax.sps_id = 0xFFFFFFFEU;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);

	/* The default reference list sizes: minus1 0..14. */
	for (value = 0U; value <= 16U; value++) {
		h265w_pps_default(&pps_syntax);
		pps_syntax.num_ref_idx_l0_default_minus1 = value;
		pps_syntax.num_ref_idx_l1_default_minus1 = 14U - (value > 14U ? 14U : value);
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((value <= 14U) ? 0 : EINVAL));
		if (error == 0)
			CHECK(store.pps[0].num_ref_idx_default[0] == value + 1U && store.pps[0].num_ref_idx_default[1] == 15U - value);
		h265w_pps_default(&pps_syntax);
		pps_syntax.num_ref_idx_l1_default_minus1 = value;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((value <= 14U) ? 0 : EINVAL));
	}
	h265w_pps_default(&pps_syntax);
	pps_syntax.num_ref_idx_l0_default_minus1 = 0x80000000U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);

	/* init_qp_minus26 -74..25 (init_qp -48..51), one beyond each end, and the largest se(v) values. */
	for (qp = -80; qp <= 30; qp++) {
		h265w_pps_default(&pps_syntax);
		pps_syntax.init_qp_minus26 = qp;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((qp >= -74 && qp <= 25) ? 0 : EINVAL));
		if (error == 0)
			CHECK(store.pps[0].init_qp == 26 + qp);
	}
	h265w_pps_default(&pps_syntax);
	pps_syntax.init_qp_minus26 = 0x7FFFFFFF;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
	pps_syntax.init_qp_minus26 = -0x7FFFFFFF;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);

	/* diff_cu_qp_delta_depth: 0..3 while read here (the SPS bound is check_pair's), only when the flag is on. */
	for (value = 0U; value <= 5U; value++) {
		h265w_pps_default(&pps_syntax);
		pps_syntax.cu_qp_delta = 1U;
		pps_syntax.diff_cu_qp_delta_depth = value;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((value <= 3U) ? 0 : EINVAL));
		if (error == 0)
			CHECK(store.pps[0].cu_qp_delta == 1 && store.pps[0].diff_cu_qp_delta_depth == value);
	}
	h265w_pps_default(&pps_syntax);
	pps_syntax.diff_cu_qp_delta_depth = 9U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
	CHECK(store.pps[0].diff_cu_qp_delta_depth == 0U);
	pps_syntax.cu_qp_delta = 1U;
	pps_syntax.diff_cu_qp_delta_depth = 0x80000000U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);

	/* The chroma QP offsets: -12..12. */
	for (qp = -14; qp <= 14; qp++) {
		h265w_pps_default(&pps_syntax);
		pps_syntax.cb_qp_offset = qp;
		pps_syntax.cr_qp_offset = -qp;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((qp >= -12 && qp <= 12) ? 0 : EINVAL));
		if (error == 0)
			CHECK(store.pps[0].cb_qp_offset == qp && store.pps[0].cr_qp_offset == -qp);
		h265w_pps_default(&pps_syntax);
		pps_syntax.cr_qp_offset = qp;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((qp >= -12 && qp <= 12) ? 0 : EINVAL));
	}

	/* The deblocking controls: offsets -6..6 when the filter is not disabled; absent (0) when it is. */
	for (qp = -8; qp <= 8; qp++) {
		h265w_pps_default(&pps_syntax);
		pps_syntax.deblocking_control_present = 1U;
		pps_syntax.deblocking_override_enabled = 1U;
		pps_syntax.beta_offset_div2 = qp;
		pps_syntax.tc_offset_div2 = -qp;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((qp >= -6 && qp <= 6) ? 0 : EINVAL));
		if (error == 0)
			CHECK(store.pps[0].beta_offset_div2 == qp && store.pps[0].tc_offset_div2 == -qp && store.pps[0].deblocking_override_enabled == 1);
		pps_syntax.beta_offset_div2 = 0;
		pps_syntax.tc_offset_div2 = qp;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((qp >= -6 && qp <= 6) ? 0 : EINVAL));
	}
	h265w_pps_default(&pps_syntax);
	pps_syntax.deblocking_control_present = 1U;
	pps_syntax.deblocking_disabled = 1U;
	pps_syntax.beta_offset_div2 = 5;
	pps_syntax.tc_offset_div2 = 5;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
	CHECK(store.pps[0].deblocking_disabled == 1 && store.pps[0].beta_offset_div2 == 0 && store.pps[0].tc_offset_div2 == 0);

	/* The parallel merge level: minus2 0..4 here (CtbLog2SizeY - 2 at most), 5 and larger refused without wrapping. */
	for (value = 0U; value <= 6U; value++) {
		h265w_pps_default(&pps_syntax);
		pps_syntax.log2_parallel_merge_level_minus2 = value;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((value <= 4U) ? 0 : EINVAL));
		if (error == 0)
			CHECK(store.pps[0].log2_parallel_merge_level == value + 2U);
	}
	h265w_pps_default(&pps_syntax);
	pps_syntax.log2_parallel_merge_level_minus2 = 0xFFFFFFFEU;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
	pps_syntax.log2_parallel_merge_level_minus2 = 0xFFFFFFFFU - 1U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);

	/* Plain flags and the extra header bits pass through. */
	for (index = 0U; index < 8U; index++) {
		h265w_pps_default(&pps_syntax);
		pps_syntax.num_extra_slice_header_bits = index;
		pps_syntax.dependent_slices = index & 1U;
		pps_syntax.output_flag_present = (index >> 1) & 1U;
		pps_syntax.sign_data_hiding = (index >> 2) & 1U;
		pps_syntax.cabac_init_present = (index + 1U) & 1U;
		pps_syntax.constrained_intra = (index >> 1) & 1U;
		pps_syntax.transform_skip = (index >> 2) & 1U;
		pps_syntax.slice_chroma_qp_offsets_present = index & 1U;
		pps_syntax.weighted_pred = (index >> 1) & 1U;
		pps_syntax.weighted_bipred = (index >> 2) & 1U;
		pps_syntax.transquant_bypass = index & 1U;
		pps_syntax.entropy_sync = (index >> 1) & 1U;
		pps_syntax.loop_filter_across_slices = (index >> 2) & 1U;
		pps_syntax.lists_modification_present = index & 1U;
		pps_syntax.slice_header_extension_present = (index >> 1) & 1U;
		CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
		CHECK(store.pps[0].num_extra_slice_header_bits == index);
		CHECK(store.pps[0].dependent_slices == (int)(index & 1U));
		CHECK(store.pps[0].output_flag_present == (int)((index >> 1) & 1U));
		CHECK(store.pps[0].sign_data_hiding == (int)((index >> 2) & 1U));
		CHECK(store.pps[0].cabac_init_present == (int)((index + 1U) & 1U));
		CHECK(store.pps[0].constrained_intra == (int)((index >> 1) & 1U));
		CHECK(store.pps[0].transform_skip == (int)((index >> 2) & 1U));
		CHECK(store.pps[0].slice_chroma_qp_offsets_present == (int)(index & 1U));
		CHECK(store.pps[0].weighted_pred == (int)((index >> 1) & 1U));
		CHECK(store.pps[0].weighted_bipred == (int)((index >> 2) & 1U));
		CHECK(store.pps[0].transquant_bypass == (int)(index & 1U));
		CHECK(store.pps[0].entropy_sync == (int)((index >> 1) & 1U));
		CHECK(store.pps[0].loop_filter_across_slices == (int)((index >> 2) & 1U));
		CHECK(store.pps[0].lists_modification_present == (int)(index & 1U));
		CHECK(store.pps[0].slice_header_extension_present == (int)((index >> 1) & 1U));
	}
	(void)offsets;
}

/* The tiles: counts up to 20 by 22, explicit sizes up to 1055, and the tile loop filter flag. */
static void
check_pps_tiles(void)
{
	static const unsigned counts[] = {1U, 2U, 3U, 19U, 20U, 21U, 22U, 23U, 1000U, 0x80000000U};
	unsigned column_index;
	unsigned row_index;
	unsigned columns;
	unsigned rows;
	unsigned entry;
	unsigned uniform;
	int error;
	int expected;

	/* Every pair of counts, uniform and explicit (sizes 1 + i), valid up to 20 by 22 and not both 1. */
	for (uniform = 0U; uniform <= 1U; uniform++) {
		for (column_index = 0U; column_index < sizeof(counts) / sizeof(counts[0]); column_index++) {
			for (row_index = 0U; row_index < sizeof(counts) / sizeof(counts[0]); row_index++) {
				columns = counts[column_index];
				rows = counts[row_index];
				h265sw_ps_init(&store);
				h265w_pps_default(&pps_syntax);
				pps_syntax.tiles = 1U;
				pps_syntax.num_tile_columns_minus1 = columns - 1U;
				pps_syntax.num_tile_rows_minus1 = rows - 1U;
				pps_syntax.uniform_spacing = uniform;
				for (entry = 0U; entry < 32U; entry++) {
					pps_syntax.column_width_minus1[entry] = entry;
					pps_syntax.row_height_minus1[entry] = entry + 2U;
				}
				pps_syntax.loop_filter_across_tiles = (columns + rows) & 1U;
				if (columns > 20U || rows > 22U)
					expected = ENOTSUP;
				else if (columns == 1U && rows == 1U)
					expected = EINVAL;
				else
					expected = 0;
				error = do_pps(&pps_syntax, (size_t)-1);
				CHECK(error == expected);
				if (expected == ENOTSUP) {
					CHECK(store.pps_refused[0] == 1 && store.pps_present[0] == 0);
				} else if (expected == 0) {
					CHECK(store.pps[0].tiles == 1);
					CHECK(store.pps[0].tile_columns == columns && store.pps[0].tile_rows == rows);
					CHECK(store.pps[0].uniform_spacing == (int)uniform);
					CHECK(store.pps[0].loop_filter_across_tiles == (int)((columns + rows) & 1U));
					for (entry = 0U; entry < H265SW_MAX_TILE_COLS; entry++) {
						if (uniform == 0U && entry + 1U < columns)
							CHECK(store.pps[0].column_width[entry] == entry + 1U);
						else
							CHECK(store.pps[0].column_width[entry] == 0U);
					}
					for (entry = 0U; entry < H265SW_MAX_TILE_ROWS; entry++) {
						if (uniform == 0U && entry + 1U < rows)
							CHECK(store.pps[0].row_height[entry] == entry + 3U);
						else
							CHECK(store.pps[0].row_height[entry] == 0U);
					}
				}
			}
		}
	}

	/* An explicit size of 1055 CTBs is taken and 1056 refused, at the first and the last position, for columns and rows. */
	for (entry = 0U; entry < 4U; entry++) {
		h265w_pps_default(&pps_syntax);
		pps_syntax.tiles = 1U;
		pps_syntax.num_tile_columns_minus1 = 19U;
		pps_syntax.num_tile_rows_minus1 = 21U;
		for (columns = 0U; columns < 32U; columns++) {
			pps_syntax.column_width_minus1[columns] = 0U;
			pps_syntax.row_height_minus1[columns] = 0U;
		}
		if (entry == 0U)
			pps_syntax.column_width_minus1[0] = 1054U;
		else if (entry == 1U)
			pps_syntax.column_width_minus1[18] = 1054U;
		else if (entry == 2U)
			pps_syntax.row_height_minus1[0] = 1054U;
		else
			pps_syntax.row_height_minus1[20] = 1054U;
		CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
		if (entry == 0U)
			CHECK(store.pps[0].column_width[0] == 1055U);
		if (entry == 3U)
			CHECK(store.pps[0].row_height[20] == 1055U);
		if (entry == 0U)
			pps_syntax.column_width_minus1[0] = 1055U;
		else if (entry == 1U)
			pps_syntax.column_width_minus1[18] = 1055U;
		else if (entry == 2U)
			pps_syntax.row_height_minus1[0] = 1055U;
		else
			pps_syntax.row_height_minus1[20] = 1055U;
		CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
		if (entry == 0U)
			pps_syntax.column_width_minus1[0] = 0x80000000U;
		else if (entry == 1U)
			pps_syntax.column_width_minus1[18] = 0xFFFFFFFEU;
		else if (entry == 2U)
			pps_syntax.row_height_minus1[0] = 0x80000000U;
		else
			pps_syntax.row_height_minus1[20] = 0xFFFFFFFEU;
		CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
	}
}

/* The range extension, and the extensions that are not read. */
static void
check_pps_extensions(void)
{
	static const unsigned lengths[] = {0U, 1U, 13U, 32U};
	unsigned value;
	unsigned kind;
	unsigned index;
	int error;

	/* A range extension with no tool on is read and flagged; with the transform skip size present or absent. */
	for (value = 0U; value <= 1U; value++) {
		h265sw_ps_init(&store);
		h265w_pps_default(&pps_syntax);
		pps_syntax.transform_skip = value;
		pps_syntax.extension_present = 1U;
		pps_syntax.range_extension_flag = 1U;
		pps_syntax.log2_max_transform_skip_block_size_minus2 = 0U;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == 0);
		CHECK(store.pps[0].extension_present == 1 && store.pps[0].range_extension_present == 1);
		CHECK(store.pps[0].transform_skip == (int)value);
	}

	/* The maximum transform skip size above 4x4 is outside the range (1..3), 4 and larger are not valid at all; absent (no transform skip) it is not read. */
	for (value = 0U; value <= 6U; value++) {
		h265w_pps_default(&pps_syntax);
		pps_syntax.transform_skip = 1U;
		pps_syntax.extension_present = 1U;
		pps_syntax.range_extension_flag = 1U;
		pps_syntax.log2_max_transform_skip_block_size_minus2 = value;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((value == 0U) ? 0 : (value <= 3U) ? ENOTSUP : EINVAL));
		pps_syntax.transform_skip = 0U;
		CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
	}
	h265w_pps_default(&pps_syntax);
	pps_syntax.transform_skip = 1U;
	pps_syntax.extension_present = 1U;
	pps_syntax.range_extension_flag = 1U;
	pps_syntax.log2_max_transform_skip_block_size_minus2 = 0x80000000U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);

	/* Cross-component prediction is outside the range. */
	h265sw_ps_init(&store);
	h265w_pps_default(&pps_syntax);
	pps_syntax.extension_present = 1U;
	pps_syntax.range_extension_flag = 1U;
	pps_syntax.cross_component_prediction = 1U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == ENOTSUP);
	CHECK(store.pps_refused[0] == 1 && store.pps_present[0] == 0);

	/* The chroma QP offset list is outside the range when enabled (valid contents), and its values are checked first. */
	for (value = 0U; value <= 7U; value++) {
		h265w_pps_default(&pps_syntax);
		pps_syntax.extension_present = 1U;
		pps_syntax.range_extension_flag = 1U;
		pps_syntax.chroma_qp_offset_list_enabled = 1U;
		pps_syntax.diff_cu_chroma_qp_offset_depth = 3U;
		pps_syntax.chroma_qp_offset_list_len_minus1 = value;
		for (index = 0U; index < 6U; index++) {
			pps_syntax.cb_qp_offset_list[index] = 12 - (int32_t)index;
			pps_syntax.cr_qp_offset_list[index] = -12 + (int32_t)index;
		}
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == ((value <= 5U) ? ENOTSUP : EINVAL));
	}
	h265w_pps_default(&pps_syntax);
	pps_syntax.extension_present = 1U;
	pps_syntax.range_extension_flag = 1U;
	pps_syntax.chroma_qp_offset_list_enabled = 1U;
	pps_syntax.diff_cu_chroma_qp_offset_depth = 4U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
	pps_syntax.diff_cu_chroma_qp_offset_depth = 0U;
	pps_syntax.chroma_qp_offset_list_len_minus1 = 5U;
	pps_syntax.cb_qp_offset_list[5] = 13;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
	pps_syntax.cb_qp_offset_list[5] = 0;
	pps_syntax.cr_qp_offset_list[5] = -13;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
	pps_syntax.cr_qp_offset_list[5] = 12;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == ENOTSUP);

	/* chroma_qp_offset_list_len_minus1 6 is not valid even when the stream holds seven pairs: a stream written by hand, since the writer holds six. */
	{
		struct bitwriter hand;
		struct vcodec_bitreader bits;
		unsigned pair;
		unsigned length;

		for (length = 5U; length <= 6U; length++) {
			bitwriter_init(&hand);
			bitwriter_ue(&hand, 0U);
			bitwriter_ue(&hand, 0U);
			bitwriter_u(&hand, 0U, 1U);
			bitwriter_u(&hand, 0U, 1U);
			bitwriter_u(&hand, 0U, 3U);
			bitwriter_u(&hand, 0U, 2U);
			bitwriter_ue(&hand, 0U);
			bitwriter_ue(&hand, 0U);
			bitwriter_se(&hand, 0);
			bitwriter_u(&hand, 0U, 3U);
			bitwriter_se(&hand, 0);
			bitwriter_se(&hand, 0);
			bitwriter_u(&hand, 0U, 6U);
			bitwriter_u(&hand, 0U, 4U);
			bitwriter_ue(&hand, 0U);
			bitwriter_u(&hand, 0U, 1U);
			bitwriter_u(&hand, 1U, 1U);
			bitwriter_u(&hand, 1U, 1U);
			bitwriter_u(&hand, 0U, 3U);
			bitwriter_u(&hand, 0U, 4U);
			bitwriter_u(&hand, 0U, 1U);
			bitwriter_u(&hand, 1U, 1U);
			bitwriter_ue(&hand, 0U);
			bitwriter_ue(&hand, length);
			for (pair = 0U; pair <= length; pair++) {
				bitwriter_se(&hand, 0);
				bitwriter_se(&hand, 0);
			}
			bitwriter_ue(&hand, 0U);
			bitwriter_ue(&hand, 0U);
			bitwriter_trailing(&hand);
			vcodec_bitreader_init(&bits, hand.data, bitwriter_bytes(&hand));
			error = h265sw_ps_read_pps(&store, &bits);
			CHECK(error == ((length <= 5U) ? ENOTSUP : EINVAL));
		}
	}

	/* The SAO offset scales: 0 is taken, 1..6 are outside the range, 7 and larger are not valid. */
	for (value = 0U; value <= 8U; value++) {
		for (kind = 0U; kind < 2U; kind++) {
			h265w_pps_default(&pps_syntax);
			pps_syntax.extension_present = 1U;
			pps_syntax.range_extension_flag = 1U;
			if (kind == 0U)
				pps_syntax.log2_sao_offset_scale_luma = value;
			else
				pps_syntax.log2_sao_offset_scale_chroma = value;
			error = do_pps(&pps_syntax, (size_t)-1);
			CHECK(error == ((value == 0U) ? 0 : (value <= 6U) ? ENOTSUP : EINVAL));
		}
	}

	/* The multilayer, 3D and screen content extensions (alone and with the range extension): outside the range, marked refused, the store unchanged. */
	for (kind = 0U; kind < 7U; kind++) {
		h265sw_ps_init(&store);
		h265w_pps_default(&pps_syntax);
		pps_syntax.pps_id = 5U;
		CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
		store_save();
		pps_syntax.extension_present = 1U;
		pps_syntax.range_extension_flag = (kind >= 4U) ? 1U : 0U;
		pps_syntax.multilayer_extension_flag = (kind == 0U || kind == 3U || kind == 4U) ? 1U : 0U;
		pps_syntax.extension_3d_flag = (kind == 1U || kind == 3U || kind == 5U) ? 1U : 0U;
		pps_syntax.scc_extension_flag = (kind == 2U || kind == 3U || kind == 6U) ? 1U : 0U;
		pps_syntax.init_qp_minus26 = 3;
		CHECK(do_pps(&pps_syntax, (size_t)-1) == ENOTSUP);
		CHECK(store.pps_refused[5] == 1);
		CHECK(store_same_but_refused() != 0);
		CHECK(store.pps[5].init_qp == 26);

		/* A good set of the id clears the mark. */
		pps_syntax.extension_present = 0U;
		CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
		CHECK(store.pps_refused[5] == 0 && store.pps[5].init_qp == 29);
	}

	/* The extension data is read over, of any length, with and without the range extension. */
	for (index = 0U; index < 4U; index++) {
		h265sw_ps_init(&store);
		h265w_pps_default(&pps_syntax);
		pps_syntax.extension_present = 1U;
		pps_syntax.range_extension_flag = index & 1U;
		pps_syntax.extension_4bits = 1U + index;
		pps_syntax.extension_data_bits = lengths[index];
		pps_syntax.extension_data = random_next();
		CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
		CHECK(store.pps[0].range_extension_present == (int)(index & 1U));
	}
}

/* Fills pps with a random valid PPS (any tile layout up to the limits, signalled scaling lists, deblocking controls, a range extension without tools). */
static void
pps_random(
	struct h265w_pps *pps)
{
	unsigned index;
	unsigned entry;
	unsigned count;

	h265w_pps_default(pps);
	pps->pps_id = random_next() % 64U;
	pps->sps_id = random_next() % 16U;
	pps->dependent_slices = random_next() % 2U;
	pps->output_flag_present = random_next() % 2U;
	pps->num_extra_slice_header_bits = random_next() % 8U;
	pps->sign_data_hiding = random_next() % 2U;
	pps->cabac_init_present = random_next() % 2U;
	pps->num_ref_idx_l0_default_minus1 = random_next() % 15U;
	pps->num_ref_idx_l1_default_minus1 = random_next() % 15U;
	pps->init_qp_minus26 = (int32_t)(random_next() % 100U) - 74;
	pps->constrained_intra = random_next() % 2U;
	pps->transform_skip = random_next() % 2U;
	pps->cu_qp_delta = random_next() % 2U;
	pps->diff_cu_qp_delta_depth = random_next() % 4U;
	pps->cb_qp_offset = (int32_t)(random_next() % 25U) - 12;
	pps->cr_qp_offset = (int32_t)(random_next() % 25U) - 12;
	pps->slice_chroma_qp_offsets_present = random_next() % 2U;
	pps->weighted_pred = random_next() % 2U;
	pps->weighted_bipred = random_next() % 2U;
	pps->transquant_bypass = random_next() % 2U;
	pps->entropy_sync = random_next() % 2U;
	pps->tiles = random_next() % 2U;
	if (pps->tiles != 0U) {
		do {
			pps->num_tile_columns_minus1 = random_next() % 20U;
			pps->num_tile_rows_minus1 = random_next() % 22U;
		} while (pps->num_tile_columns_minus1 == 0U && pps->num_tile_rows_minus1 == 0U);
		pps->uniform_spacing = random_next() % 2U;
		for (index = 0U; index < 32U; index++) {
			pps->column_width_minus1[index] = random_next() % 1055U;
			pps->row_height_minus1[index] = random_next() % 1055U;
		}
		pps->loop_filter_across_tiles = random_next() % 2U;
	}
	pps->loop_filter_across_slices = random_next() % 2U;
	pps->deblocking_control_present = random_next() % 2U;
	pps->deblocking_override_enabled = random_next() % 2U;
	pps->deblocking_disabled = random_next() % 2U;
	pps->beta_offset_div2 = (int32_t)(random_next() % 13U) - 6;
	pps->tc_offset_div2 = (int32_t)(random_next() % 13U) - 6;
	pps->scaling_list_present = random_next() % 2U;
	if (pps->scaling_list_present != 0U) {
		memset(random_lists, 0, sizeof(random_lists));
		memset(random_dc, 0, sizeof(random_dc));
		for (index = 0U; index < 4U; index++) {
			for (entry = 0U; entry < 6U; entry += (index == 3U) ? 3U : 1U) {
				for (count = 0U; count < 64U; count++)
					random_lists[index][entry][count] = (uint8_t)(1U + random_next() % 255U);
				random_dc[0][entry] = (uint8_t)(1U + random_next() % 255U);
				random_dc[1][entry] = (uint8_t)(1U + random_next() % 255U);
			}
		}
		h265w_scaling_from_lists(&pps->scaling, (const uint8_t (*)[6][64])random_lists, (const uint8_t (*)[6])random_dc);
	}
	pps->lists_modification_present = random_next() % 2U;
	pps->log2_parallel_merge_level_minus2 = random_next() % 5U;
	pps->slice_header_extension_present = random_next() % 2U;
	pps->extension_present = random_next() % 2U;
	if (pps->extension_present != 0U) {
		pps->range_extension_flag = random_next() % 2U;
		pps->extension_4bits = random_next() % 2U;
		pps->extension_data_bits = random_next() % 33U;
		pps->extension_data = random_next();
	}
}

/* Random valid PPSs: every signalled field and the derived tile arrays. */
static void
check_pps_branches(void)
{
	unsigned round;
	unsigned entry;
	unsigned id;
	unsigned size_id;
	unsigned matrix_id;
	int error;
	int expected;

	h265sw_ps_init(&store);
	for (round = 0U; round < 3000U; round++) {
		pps_random(&pps_syntax);
		expected = 0;
		if (pps_syntax.tiles != 0U && pps_syntax.num_tile_columns_minus1 == 0U && pps_syntax.num_tile_rows_minus1 == 0U)
			expected = EINVAL;
		error = do_pps(&pps_syntax, (size_t)-1);
		CHECK(error == expected);
		if (error != 0)
			continue;
		id = pps_syntax.pps_id;
		CHECK(store.pps[id].id == id && store.pps[id].sps_id == pps_syntax.sps_id);
		CHECK(store.pps[id].dependent_slices == (int)pps_syntax.dependent_slices);
		CHECK(store.pps[id].output_flag_present == (int)pps_syntax.output_flag_present);
		CHECK(store.pps[id].num_extra_slice_header_bits == pps_syntax.num_extra_slice_header_bits);
		CHECK(store.pps[id].sign_data_hiding == (int)pps_syntax.sign_data_hiding);
		CHECK(store.pps[id].cabac_init_present == (int)pps_syntax.cabac_init_present);
		CHECK(store.pps[id].num_ref_idx_default[0] == pps_syntax.num_ref_idx_l0_default_minus1 + 1U);
		CHECK(store.pps[id].num_ref_idx_default[1] == pps_syntax.num_ref_idx_l1_default_minus1 + 1U);
		CHECK(store.pps[id].init_qp == 26 + pps_syntax.init_qp_minus26);
		CHECK(store.pps[id].constrained_intra == (int)pps_syntax.constrained_intra);
		CHECK(store.pps[id].transform_skip == (int)pps_syntax.transform_skip);
		CHECK(store.pps[id].cu_qp_delta == (int)pps_syntax.cu_qp_delta);
		CHECK(store.pps[id].diff_cu_qp_delta_depth == ((pps_syntax.cu_qp_delta != 0U) ? pps_syntax.diff_cu_qp_delta_depth : 0U));
		CHECK(store.pps[id].cb_qp_offset == pps_syntax.cb_qp_offset && store.pps[id].cr_qp_offset == pps_syntax.cr_qp_offset);
		CHECK(store.pps[id].slice_chroma_qp_offsets_present == (int)pps_syntax.slice_chroma_qp_offsets_present);
		CHECK(store.pps[id].weighted_pred == (int)pps_syntax.weighted_pred && store.pps[id].weighted_bipred == (int)pps_syntax.weighted_bipred);
		CHECK(store.pps[id].transquant_bypass == (int)pps_syntax.transquant_bypass);
		CHECK(store.pps[id].tiles == (int)pps_syntax.tiles && store.pps[id].entropy_sync == (int)pps_syntax.entropy_sync);
		if (pps_syntax.tiles != 0U) {
			CHECK(store.pps[id].tile_columns == pps_syntax.num_tile_columns_minus1 + 1U);
			CHECK(store.pps[id].tile_rows == pps_syntax.num_tile_rows_minus1 + 1U);
			CHECK(store.pps[id].uniform_spacing == (int)pps_syntax.uniform_spacing);
			CHECK(store.pps[id].loop_filter_across_tiles == (int)pps_syntax.loop_filter_across_tiles);
			for (entry = 0U; entry < H265SW_MAX_TILE_COLS; entry++)
				CHECK(store.pps[id].column_width[entry] == ((pps_syntax.uniform_spacing == 0U && entry < pps_syntax.num_tile_columns_minus1) ? pps_syntax.column_width_minus1[entry] + 1U : 0U));
			for (entry = 0U; entry < H265SW_MAX_TILE_ROWS; entry++)
				CHECK(store.pps[id].row_height[entry] == ((pps_syntax.uniform_spacing == 0U && entry < pps_syntax.num_tile_rows_minus1) ? pps_syntax.row_height_minus1[entry] + 1U : 0U));
		} else {
			CHECK(store.pps[id].tile_columns == 1U && store.pps[id].tile_rows == 1U);
			CHECK(store.pps[id].uniform_spacing == 1 && store.pps[id].loop_filter_across_tiles == 1);
		}
		CHECK(store.pps[id].loop_filter_across_slices == (int)pps_syntax.loop_filter_across_slices);
		CHECK(store.pps[id].deblocking_control_present == (int)pps_syntax.deblocking_control_present);
		if (pps_syntax.deblocking_control_present != 0U) {
			CHECK(store.pps[id].deblocking_override_enabled == (int)pps_syntax.deblocking_override_enabled);
			CHECK(store.pps[id].deblocking_disabled == (int)pps_syntax.deblocking_disabled);
			if (pps_syntax.deblocking_disabled == 0U) {
				CHECK(store.pps[id].beta_offset_div2 == pps_syntax.beta_offset_div2);
				CHECK(store.pps[id].tc_offset_div2 == pps_syntax.tc_offset_div2);
			} else {
				CHECK(store.pps[id].beta_offset_div2 == 0 && store.pps[id].tc_offset_div2 == 0);
			}
		} else {
			CHECK(store.pps[id].deblocking_override_enabled == 0 && store.pps[id].deblocking_disabled == 0);
		}
		CHECK(store.pps[id].scaling_list_present == (int)pps_syntax.scaling_list_present);
		if (pps_syntax.scaling_list_present != 0U) {
			for (size_id = 0U; size_id < 4U; size_id++) {
				for (matrix_id = 0U; matrix_id < 6U; matrix_id += (size_id == 3U) ? 3U : 1U)
					CHECK(memcmp(store.pps[id].scaling.list[size_id][matrix_id], random_lists[size_id][matrix_id], (size_id == 0U) ? 16U : 64U) == 0);
			}
		}
		CHECK(store.pps[id].lists_modification_present == (int)pps_syntax.lists_modification_present);
		CHECK(store.pps[id].log2_parallel_merge_level == pps_syntax.log2_parallel_merge_level_minus2 + 2U);
		CHECK(store.pps[id].slice_header_extension_present == (int)pps_syntax.slice_header_extension_present);
		CHECK(store.pps[id].extension_present == (int)pps_syntax.extension_present);
		CHECK(store.pps[id].range_extension_present == (int)(pps_syntax.extension_present != 0U && pps_syntax.range_extension_flag != 0U));
	}
}

/* Sets the pair structures to a consistent SPS (8-bit, CTB 16, minimum block 8, 10 by 6 CTBs) and PPS. */
static void
pair_reset(void)
{
	memset(&pair_sps, 0, sizeof(pair_sps));
	memset(&pair_pps, 0, sizeof(pair_pps));
	pair_sps.id = 0U;
	pair_sps.bit_depth_luma = 8U;
	pair_sps.bit_depth_chroma = 8U;
	pair_sps.ctb_log2 = 4U;
	pair_sps.min_cb_log2 = 3U;
	pair_sps.pic_width_ctbs = 10U;
	pair_sps.pic_height_ctbs = 6U;
	pair_pps.sps_id = 0U;
	pair_pps.init_qp = 26;
	pair_pps.tile_columns = 1U;
	pair_pps.tile_rows = 1U;
	pair_pps.uniform_spacing = 1;
	pair_pps.log2_parallel_merge_level = 2U;
}

/* The conditions of h265sw_ps_check_pair: each at its boundary. */
static void
check_pair(void)
{
	unsigned depth;
	unsigned bit_depth;
	int qp;
	int error;
	int expected;

	pair_reset();
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == 0);
	CHECK(h265sw_ps_check_pair(NULL, &pair_pps) == EINVAL);
	CHECK(h265sw_ps_check_pair(&pair_sps, NULL) == EINVAL);

	/* The PPS must name the SPS. */
	pair_pps.sps_id = 1U;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == EINVAL);
	pair_sps.id = 1U;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == 0);

	/* A PPS scaling list needs scaling lists enabled in the SPS. */
	pair_reset();
	pair_pps.scaling_list_present = 1;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == EINVAL);
	pair_sps.scaling_list_enabled = 1;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == 0);
	pair_pps.scaling_list_present = 0;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == 0);

	/* init_qp: -QpBdOffsetY..51 (0 for 8 bits, -12 for 10, -48 for 16). */
	for (bit_depth = 8U; bit_depth <= 16U; bit_depth += 2U) {
		for (qp = -50; qp <= 53; qp++) {
			pair_reset();
			pair_sps.bit_depth_luma = bit_depth;
			pair_pps.init_qp = qp;
			expected = (qp >= -6 * (int)(bit_depth - 8U) && qp <= 51) ? 0 : EINVAL;
			error = h265sw_ps_check_pair(&pair_sps, &pair_pps);
			CHECK(error == expected);
		}
	}

	/* diff_cu_qp_delta_depth: at most CtbLog2SizeY - MinCbLog2SizeY (1 here, 3 with CTB 64). */
	for (depth = 0U; depth <= 4U; depth++) {
		pair_reset();
		pair_pps.diff_cu_qp_delta_depth = depth;
		CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == ((depth <= 1U) ? 0 : EINVAL));
		pair_sps.ctb_log2 = 6U;
		CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == ((depth <= 3U) ? 0 : EINVAL));
	}

	/* log2_parallel_merge_level: at most CtbLog2SizeY. */
	for (depth = 2U; depth <= 7U; depth++) {
		pair_reset();
		pair_pps.log2_parallel_merge_level = depth;
		CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == ((depth <= 4U) ? 0 : EINVAL));
		pair_sps.ctb_log2 = 6U;
		CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == ((depth <= 6U) ? 0 : EINVAL));
	}

	/* Tile counts: at most the CTBs across and down (10 and 6), uniform spacing. */
	for (depth = 1U; depth <= 12U; depth++) {
		pair_reset();
		pair_pps.tiles = 1;
		pair_pps.tile_columns = depth;
		pair_pps.tile_rows = 2U;
		CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == ((depth <= 10U) ? 0 : EINVAL));
		pair_pps.tile_columns = 2U;
		pair_pps.tile_rows = depth;
		CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == ((depth <= 6U) ? 0 : EINVAL));
	}

	/* Without tiles the counts are not looked at. */
	pair_reset();
	pair_pps.tiles = 0;
	pair_pps.tile_columns = 99U;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == 0);

	/* Explicit widths and heights: the sum leaves at least one CTB for the last column and row. */
	pair_reset();
	pair_pps.tiles = 1;
	pair_pps.uniform_spacing = 0;
	pair_pps.tile_columns = 3U;
	pair_pps.tile_rows = 3U;
	pair_pps.column_width[0] = 4U;
	pair_pps.column_width[1] = 5U;
	pair_pps.row_height[0] = 2U;
	pair_pps.row_height[1] = 3U;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == 0);
	pair_pps.column_width[1] = 6U;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == EINVAL);
	pair_pps.column_width[0] = 1U;
	pair_pps.column_width[1] = 1U;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == 0);
	pair_pps.row_height[1] = 4U;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == EINVAL);
	pair_pps.row_height[1] = 3U;
	CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == 0);

	/* A sum of the largest widths does not wrap. */
	{
		unsigned entry;

		pair_pps.tile_columns = H265SW_MAX_TILE_COLS;
		pair_pps.tile_rows = 2U;
		for (entry = 0U; entry < H265SW_MAX_TILE_COLS; entry++)
			pair_pps.column_width[entry] = 1055U;
		pair_sps.pic_width_ctbs = 1055U;
		CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == EINVAL);
		pair_pps.tile_columns = 1U;
		pair_pps.tile_rows = 1U;
		CHECK(h265sw_ps_check_pair(&pair_sps, &pair_pps) == 0);
	}
}

/* The store: re-sent sets keep the counters, changed ones step them, failures leave it alone, refusals are marked. */
static void
check_store(void)
{
	static struct h265sw_ps_store empty;
	unsigned generation;
	unsigned sps_generation_0;
	unsigned sps_generation_1;
	int error;

	/* Init clears everything, whatever the store held. */
	memset(&store, 0x5A, sizeof(store));
	h265sw_ps_init(&store);
	memset(&empty, 0, sizeof(empty));
	CHECK(memcmp(&store, &empty, sizeof(store)) == 0);

	/* A new SPS counts once; the same SPS sent again counts no more, in either counter. */
	h265w_sps_default(&sps_syntax);
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 1U && store.sps_generation[0] == 1U);
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 1U && store.sps_generation[0] == 1U);
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 1U && store.sps_generation[0] == 1U);

	/* A different content steps both by one. */
	sps_syntax.amp = 1U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 2U && store.sps_generation[0] == 2U);
	CHECK(store.sps[0].amp == 1);

	/* Another id has its own counter and steps the total. */
	sps_syntax.sps_id = 1U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 3U && store.sps_generation[0] == 2U && store.sps_generation[1] == 1U);
	sps_syntax.sps_id = 0U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 3U);

	/* The same holds for a PPS and a VPS: a total step only for a change. */
	h265w_pps_default(&pps_syntax);
	CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 4U);
	CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 4U);
	pps_syntax.init_qp_minus26 = 5;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 5U);
	h265w_vps_default(&vps_syntax);
	CHECK(do_vps(&vps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 6U);
	CHECK(do_vps(&vps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 6U);
	vps_syntax.max_dec_pic_buffering_minus1[0] = 6U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == 0);
	CHECK(store.generation == 7U);
	CHECK(store.sps_generation[0] == 2U && store.sps_generation[1] == 1U);

	/* Failed reads leave every byte of the store as it was: a broken SPS, PPS and VPS, and a cut one. */
	store_save();
	sps_syntax.log2_max_poc_lsb_minus4 = 13U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == EINVAL);
	CHECK(store_same_but_refused() != 0);
	CHECK(memcmp(&saved, &store, sizeof(store)) == 0);
	sps_syntax.log2_max_poc_lsb_minus4 = 4U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	store_save();
	CHECK(do_sps(&sps_syntax, last_bytes - 1U) == EINVAL);
	CHECK(memcmp(&saved, &store, sizeof(store)) == 0);
	pps_syntax.init_qp_minus26 = 100;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == EINVAL);
	CHECK(memcmp(&saved, &store, sizeof(store)) == 0);
	vps_syntax.max_dec_pic_buffering_minus1[0] = 20U;
	CHECK(do_vps(&vps_syntax, (size_t)-1) == EINVAL);
	CHECK(memcmp(&saved, &store, sizeof(store)) == 0);

	/* A refused SPS marks only the refusal; the earlier content and counters stay; a good one of the id clears the mark. */
	generation = store.generation;
	sps_generation_0 = store.sps_generation[0];
	sps_generation_1 = store.sps_generation[1];
	sps_syntax.log2_max_poc_lsb_minus4 = 4U;
	sps_syntax.log2_diff_max_min_cb = 5U;
	error = do_sps(&sps_syntax, (size_t)-1);
	CHECK(error == ENOTSUP);
	CHECK(store.sps_refused[0] == 1 && store.sps_present[0] == 1);
	CHECK(store.generation == generation && store.sps_generation[0] == sps_generation_0 && store.sps_generation[1] == sps_generation_1);
	CHECK(store_same_but_refused() != 0);
	sps_syntax.log2_diff_max_min_cb = 1U;
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	CHECK(store.sps_refused[0] == 0);
	CHECK(store.generation == generation && store.sps_generation[0] == sps_generation_0);

	/* A refused PPS marks only the refusal. */
	store_save();
	pps_syntax.init_qp_minus26 = 5;
	pps_syntax.tiles = 1U;
	pps_syntax.num_tile_columns_minus1 = 20U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == ENOTSUP);
	CHECK(store.pps_refused[0] == 1 && store.pps_present[0] == 1);
	CHECK(store_same_but_refused() != 0);
	pps_syntax.tiles = 0U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
	CHECK(store.pps_refused[0] == 0);

	/* A VPS is never refused. */
	CHECK(store.vps_refused[0] == 0 && store.vps_refused[5] == 0);
}

/*
 * Every cut of random valid sets of each kind: refused and the store
 * untouched; the whole one taken.  The sets carry no extension data (a cut
 * inside extension data is a shorter extension, which is valid).
 */
static void
check_truncated(void)
{
	unsigned round;
	unsigned kind;
	unsigned count;
	size_t cut;
	size_t bytes;
	int error;

	for (round = 0U; round < 40U; round++) {
		for (kind = 0U; kind < 3U; kind++) {
			h265sw_ps_init(&store);
			if (kind == 0U) {
				h265w_vps_default(&vps_syntax);
				vps_syntax.max_sub_layers_minus1 = random_next() % 7U;
				vps_syntax.sub_layer_ordering_info_present = random_next() % 2U;
				for (count = 0U; count <= vps_syntax.max_sub_layers_minus1; count++) {
					vps_syntax.max_dec_pic_buffering_minus1[count] = 2U + random_next() % 10U;
					vps_syntax.max_latency_increase_plus1[count] = random_next();
					if (vps_syntax.max_latency_increase_plus1[count] == 0xFFFFFFFFU)
						vps_syntax.max_latency_increase_plus1[count] = 0U;
				}
				vps_syntax.num_layer_sets_minus1 = random_next() % 4U;
				vps_syntax.max_layer_id = random_next() % 64U;
				vps_syntax.timing_present = random_next() % 2U;
				vps_syntax.num_units_in_tick = 1U + random_next() % 1000U;
				vps_syntax.time_scale = 1U + random_next() % 100000U;
				vps_syntax.poc_proportional = random_next() % 2U;
				vps_syntax.num_ticks_poc_diff_one_minus1 = random_next() % 100000U;
				vps_syntax.num_hrd_parameters = random_next() % (vps_syntax.num_layer_sets_minus1 + 2U);
				for (count = 0U; count < 4U; count++) {
					hrd_fill(&vps_syntax.hrd[count], random_next() % 20U, 1U, random_next() % 2U);
					vps_syntax.hrd_layer_set_idx[count] = count;
					vps_syntax.cprms_present[count] = 1U;
				}
				vps_syntax.extension = 0U;
				if (vps_syntax.num_hrd_parameters > 4U)
					vps_syntax.num_hrd_parameters = 4U;
				error = do_vps(&vps_syntax, (size_t)-1);
			} else if (kind == 1U) {
				sps_random(&sps_syntax);
				error = do_sps(&sps_syntax, (size_t)-1);
			} else {
				pps_random(&pps_syntax);
				if (pps_syntax.tiles != 0U && pps_syntax.num_tile_columns_minus1 == 0U && pps_syntax.num_tile_rows_minus1 == 0U)
					pps_syntax.num_tile_rows_minus1 = 1U;
				pps_syntax.extension_4bits = 0U;
				error = do_pps(&pps_syntax, (size_t)-1);
			}
			CHECK(error == 0);
			bytes = last_bytes;
			store_save();

			if (error != 0)
				continue;
			h265sw_ps_init(&store);
			store_save();
			for (cut = 0U; cut < bytes; cut++) {
				if (kind == 0U)
					error = do_vps(&vps_syntax, cut);
				else if (kind == 1U)
					error = do_sps(&sps_syntax, cut);
				else
					error = do_pps(&pps_syntax, cut);
				CHECK(error == EINVAL);
				CHECK(store_same_but_refused() != 0);
			}
		}
	}
}

/* The invariants of a stored SPS, PPS or VPS that every successful read keeps. */
static void
check_invariants(
	int kind,
	unsigned id)
{
	unsigned index;
	unsigned entry;
	unsigned max_pictures;
	unsigned columns;
	unsigned rows;

	if (kind == 0) {
		CHECK(store.vps[id].id == id);
		CHECK(store.vps[id].max_sub_layers_minus1 <= 6U);
		CHECK(store.vps[id].num_layer_sets_minus1 <= 1023U);
		CHECK(store.vps[id].num_hrd_parameters <= store.vps[id].num_layer_sets_minus1 + 1U);
		for (index = 0U; index <= store.vps[id].max_sub_layers_minus1; index++) {
			CHECK(store.vps[id].max_dec_pic_buffering_minus1[index] <= 15U);
			CHECK(store.vps[id].max_num_reorder_pics[index] <= store.vps[id].max_dec_pic_buffering_minus1[index]);
		}
		return;
	}
	if (kind == 1) {
		CHECK(store.sps[id].id == id);
		CHECK(store.sps[id].chroma_format_idc <= 3U);
		CHECK(store.sps[id].bit_depth_luma >= 8U && store.sps[id].bit_depth_luma <= 16U);
		CHECK(store.sps[id].bit_depth_chroma >= 8U && store.sps[id].bit_depth_chroma <= 16U);
		CHECK(store.sps[id].log2_max_poc_lsb >= 4U && store.sps[id].log2_max_poc_lsb <= 16U);
		CHECK(store.sps[id].min_cb_log2 >= 3U && store.sps[id].min_cb_log2 <= 6U);
		CHECK(store.sps[id].ctb_log2 >= 4U && store.sps[id].ctb_log2 <= 6U && store.sps[id].ctb_log2 >= store.sps[id].min_cb_log2);
		CHECK(store.sps[id].min_tb_log2 >= 2U && store.sps[id].min_tb_log2 < store.sps[id].min_cb_log2);
		CHECK(store.sps[id].max_tb_log2 >= store.sps[id].min_tb_log2 && store.sps[id].max_tb_log2 <= 5U && store.sps[id].max_tb_log2 <= store.sps[id].ctb_log2);
		CHECK(store.sps[id].width >= 1U && store.sps[id].width <= H265SW_MAX_SIDE);
		CHECK(store.sps[id].height >= 1U && store.sps[id].height <= H265SW_MAX_SIDE);
		CHECK((uint64_t)store.sps[id].width * store.sps[id].height <= H265SW_MAX_LUMA_PS);
		CHECK(store.sps[id].width % (1U << store.sps[id].min_cb_log2) == 0U);
		CHECK(store.sps[id].height % (1U << store.sps[id].min_cb_log2) == 0U);
		CHECK(store.sps[id].crop_width >= 1U && store.sps[id].crop_x + store.sps[id].crop_width <= store.sps[id].width);
		CHECK(store.sps[id].crop_height >= 1U && store.sps[id].crop_y + store.sps[id].crop_height <= store.sps[id].height);
		CHECK(store.sps[id].num_st_rps <= 64U && store.sps[id].num_lt_sps <= 32U);
		max_pictures = store.sps[id].max_dec_pic_buffering_minus1[store.sps[id].max_sub_layers_minus1];
		for (index = 0U; index < store.sps[id].num_st_rps; index++) {
			CHECK(store.sps[id].st_rps[index].num_negative + store.sps[id].st_rps[index].num_positive <= max_pictures);
			for (entry = 1U; entry < store.sps[id].st_rps[index].num_negative; entry++)
				CHECK(store.sps[id].st_rps[index].delta_poc[entry] < store.sps[id].st_rps[index].delta_poc[entry - 1U]);
		}
		if (store.sps[id].pcm != 0) {
			CHECK(store.sps[id].pcm_min_log2 <= store.sps[id].pcm_max_log2 && store.sps[id].pcm_max_log2 <= 5U);
			CHECK(store.sps[id].pcm_bit_depth_luma >= 1U && store.sps[id].pcm_bit_depth_luma <= store.sps[id].bit_depth_luma);
		}
		return;
	}
	CHECK(store.pps[id].id == id && store.pps[id].sps_id <= 15U);
	CHECK(store.pps[id].init_qp >= -48 && store.pps[id].init_qp <= 51);
	CHECK(store.pps[id].num_ref_idx_default[0] >= 1U && store.pps[id].num_ref_idx_default[0] <= 15U);
	CHECK(store.pps[id].num_ref_idx_default[1] >= 1U && store.pps[id].num_ref_idx_default[1] <= 15U);
	CHECK(store.pps[id].cb_qp_offset >= -12 && store.pps[id].cb_qp_offset <= 12);
	CHECK(store.pps[id].cr_qp_offset >= -12 && store.pps[id].cr_qp_offset <= 12);
	CHECK(store.pps[id].beta_offset_div2 >= -6 && store.pps[id].beta_offset_div2 <= 6);
	CHECK(store.pps[id].tc_offset_div2 >= -6 && store.pps[id].tc_offset_div2 <= 6);
	CHECK(store.pps[id].diff_cu_qp_delta_depth <= 3U);
	CHECK(store.pps[id].log2_parallel_merge_level >= 2U && store.pps[id].log2_parallel_merge_level <= 6U);
	columns = store.pps[id].tile_columns;
	rows = store.pps[id].tile_rows;
	CHECK(columns >= 1U && columns <= H265SW_MAX_TILE_COLS && rows >= 1U && rows <= H265SW_MAX_TILE_ROWS);
	for (entry = 0U; entry < H265SW_MAX_TILE_COLS; entry++)
		CHECK(store.pps[id].column_width[entry] <= 1055U);
	for (entry = 0U; entry < H265SW_MAX_TILE_ROWS; entry++)
		CHECK(store.pps[id].row_height[entry] <= 1055U);
	CHECK(store.pps[id].range_extension_present == 0 || store.pps[id].extension_present != 0);
}

/*
 * Corrupted copies of valid sets and random bytes: no crash, only the
 * allowed errors, a failure leaves the store as it was, and a success keeps
 * the invariants.
 */
static void
check_fuzz(void)
{
	static uint8_t good[3][8192];
	size_t good_size[3];
	uint8_t data[8192 + 16];
	size_t size;
	size_t index;
	unsigned round;
	unsigned kind;
	unsigned mutation;
	unsigned count;
	unsigned id;
	int changed;
	int error;
	int which;

	/* One valid RBSP of each kind to corrupt. */
	h265sw_ps_init(&store);
	h265w_vps_default(&vps_syntax);
	vps_syntax.timing_present = 1U;
	vps_syntax.num_units_in_tick = 1001U;
	vps_syntax.time_scale = 30000U;
	vps_syntax.num_layer_sets_minus1 = 2U;
	vps_syntax.num_hrd_parameters = 3U;
	for (count = 0U; count < 3U; count++) {
		hrd_fill(&vps_syntax.hrd[count], count, 1U, 1U);
		vps_syntax.hrd_layer_set_idx[count] = count;
		vps_syntax.cprms_present[count] = (count == 1U) ? 0U : 1U;
	}
	CHECK(do_vps(&vps_syntax, (size_t)-1) == 0);
	good_size[0] = last_bytes;
	memcpy(good[0], writer.data, last_bytes);
	sps_random(&sps_syntax);
	sps_syntax.vui_present = 1U;
	sps_syntax.vui.timing_present = 1U;
	sps_syntax.vui.num_units_in_tick = 1001U;
	sps_syntax.vui.time_scale = 30000U;
	sps_syntax.vui.hrd_present = 1U;
	hrd_fill(&sps_syntax.vui.hrd, 2U, 1U, 1U);
	CHECK(do_sps(&sps_syntax, (size_t)-1) == 0);
	good_size[1] = last_bytes;
	memcpy(good[1], writer.data, last_bytes);
	pps_random(&pps_syntax);
	pps_syntax.tiles = 1U;
	pps_syntax.num_tile_columns_minus1 = 4U;
	pps_syntax.num_tile_rows_minus1 = 3U;
	pps_syntax.deblocking_control_present = 1U;
	pps_syntax.scaling_list_present = 0U;
	CHECK(do_pps(&pps_syntax, (size_t)-1) == 0);
	good_size[2] = last_bytes;
	memcpy(good[2], writer.data, last_bytes);

	h265sw_ps_init(&store);
	store_save();
	for (round = 0U; round < 60000U; round++) {
		kind = round % 3U;
		which = (int)kind;

		/* A valid set, or random bytes, then a few changes. */
		mutation = random_next() % 8U;
		if (mutation == 0U) {
			size = random_next() % 64U;
			for (index = 0U; index < size; index++)
				data[index] = (uint8_t)random_next();
		} else {
			size = good_size[kind];
			memcpy(data, good[kind], size);
			count = 1U + random_next() % 3U;
			while (count > 0U) {
				index = random_next() % size;
				if ((random_next() & 1U) != 0U)
					data[index] ^= (uint8_t)(1U << (random_next() % 8U));
				else
					data[index] = (uint8_t)random_next();
				count--;
			}
			if (mutation == 1U)
				size = random_next() % (size + 1U);
		}

		error = do_bytes(which, data, size);
		CHECK(error == 0 || error == EINVAL || error == ENOTSUP);
		if (error != 0) {
			CHECK(store_same_but_refused() != 0);
		} else {
			/* A success: its id is the one the bytes name; check what it kept. */
			id = 0U;
			changed = 0;
			if (which == 0) {
				for (index = 0U; index < H265SW_VPS_IDS; index++) {
					if (memcmp(&store.vps[index], &saved.vps[index], sizeof(store.vps[0])) != 0) {
						id = (unsigned)index;
						changed = 1;
					}
				}
				if (changed != 0)
					check_invariants(0, id);
			} else if (which == 1) {
				for (index = 0U; index < H265SW_SPS_IDS; index++) {
					if (memcmp(&store.sps[index], &saved.sps[index], sizeof(store.sps[0])) != 0) {
						id = (unsigned)index;
						changed = 1;
					}
				}
				if (changed != 0)
					check_invariants(1, id);
			} else {
				for (index = 0U; index < H265SW_PPS_IDS; index++) {
					if (memcmp(&store.pps[index], &saved.pps[index], sizeof(store.pps[0])) != 0) {
						id = (unsigned)index;
						changed = 1;
					}
				}
				if (changed != 0)
					check_invariants(2, id);
			}
		}
		store_save();
	}
}

int
main(void)
{
	/* Runs every group of checks. */
	check_ptl_bits();
	check_ptl_fields();
	check_vps_basic();
	check_vps_ordering();
	check_vps_layers();
	check_vps_hrd();
	check_vps_ranges();
	check_sps_default();
	check_sps_branches();
	check_sps_derived();
	check_sps_sizes();
	check_sps_log2();
	check_sps_pcm();
	check_sps_sets();
	check_sps_extensions();
	check_sps_vui();
	check_sps_supported();
	check_sps_random();
	check_pps_default();
	check_pps_ranges();
	check_pps_tiles();
	check_pps_extensions();
	check_pps_branches();
	check_pair();
	check_store();
	check_truncated();
	check_fuzz();

	/* Reports the summary line. */
	return check_finish("ps");
}
