/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the tests' own H.265 syntax writer (h265write.h) against bit
 * strings arranged by hand from the syntax tables of ITU-T H.265 (V11)
 * (01/2026) clause 7.3 and Annex E, and its NAL unit and byte stream
 * output against bytes arranged by hand from 7.3.1, 7.4.2 and B.2, so the
 * other tests can trust the inputs it builds.
 *
 * The Exp-Golomb strings used (Table 9-2 of H.264, the same code as 9.2
 * of H.265): ue(0) 1, ue(1) 010, ue(2) 011, ue(3) 00100, ue(4) 00101,
 * ue(5) 00110, ue(6) 00111, ue(7) 0001000, ue(9) 0001010, ue(10)
 * 0001011, ue(14) 0001111, ue(15) 000010000, ue(64) 0000001000001; se(k)
 * is ue(2k - 1) for k above 0 and ue(-2k) otherwise (9.2.2).
 */

#include <stdlib.h>

#include "check.h"
#include "h265write.h"

/* The longest bit string a check spells out. */
#define EXPECT_BITS 4096U

/* A bit string being spelled out, of '0' and '1'; overflow set when a piece did not fit. */
struct expect {
	char text[EXPECT_BITS + 1U];
	size_t length;
	int overflow;
};

/* Static function declarations. */
static void expect_init(struct expect *expect);
static void expect_add(struct expect *expect, const char *piece);
static void expect_zeros(struct expect *expect, unsigned count);
static void expect_ones(struct expect *expect, unsigned count);
static void expect_align(struct expect *expect);
static void expect_ptl_main(struct expect *expect);
static int bits_equal(const struct bitwriter *writer, const struct expect *expect);
static void check_basics(void);
static void check_nal(void);
static void check_ptl(void);
static void check_st_rps(void);
static void check_scaling(void);
static void check_hrd(void);
static void check_vui(void);
static void check_vps(void);
static void check_sps(void);
static void check_pps(void);
static void check_slice_i(void);
static void check_slice_p(void);
static void check_slice_b(void);
static void check_slice_dependent(void);

/* The writer and its value structures, too large for the stack of a check. */
static struct bitwriter writer;
static struct expect expected;
static struct h265w_vps vps;
static struct h265w_sps sps;
static struct h265w_pps pps;
static struct h265w_slice slice;
static struct h265w_slice_context context;
static struct h265w_hrd hrd;
static struct h265w_vui vui;
static struct h265w_scaling scaling;

/* Empties a bit string. */
static void
expect_init(
	struct expect *expect)
{
	/* No bit yet. */
	memset(expect, 0, sizeof(*expect));
}

/* Appends a piece of '0' and '1'. */
static void
expect_add(
	struct expect *expect,
	const char *piece)
{
	size_t length;

	/* A piece that does not fit marks the string as broken. */
	length = strlen(piece);
	if (expect->length + length > EXPECT_BITS) {
		expect->overflow = 1;
		return;
	}
	memcpy(expect->text + expect->length, piece, length);
	expect->length += length;
	expect->text[expect->length] = '\0';
}

/* Appends count zero bits. */
static void
expect_zeros(
	struct expect *expect,
	unsigned count)
{
	unsigned index;

	/* One bit at a time. */
	for (index = 0U; index < count; index++)
		expect_add(expect, "0");
}

/* Appends count one bits. */
static void
expect_ones(
	struct expect *expect,
	unsigned count)
{
	unsigned index;

	/* One bit at a time. */
	for (index = 0U; index < count; index++)
		expect_add(expect, "1");
}

/* Appends rbsp_trailing_bits(): a one and zeros to the byte boundary. */
static void
expect_align(
	struct expect *expect)
{
	/* The stop bit, then the alignment zeros. */
	expect_add(expect, "1");
	while (expect->length % 8U != 0U && expect->overflow == 0)
		expect_add(expect, "0");
}

/*
 * Appends the profile_tier_level( 1, 0 ) of h265w_profile_main with
 * level 93: 2 + 1 + 5 + 32 + 4 + 43 + 1 + 8 = 96 bits.
 */
static void
expect_ptl_main(
	struct expect *expect)
{
	/* general_profile_space 0, general_tier_flag 0, general_profile_idc 1. */
	expect_add(expect, "00" "0" "00001");

	/* The compatibility flags: [ 1 ] and [ 2 ] set. */
	expect_add(expect, "0110");
	expect_zeros(expect, 28U);

	/* progressive 1, interlaced 0, non_packed 0, frame_only 1. */
	expect_add(expect, "1001");

	/* The 43 constraint bits and the inbld bit, all 0. */
	expect_zeros(expect, 44U);

	/* general_level_idc 93. */
	expect_add(expect, "01011101");
}

/* Reports whether the writer holds exactly the spelled bit string. */
static int
bits_equal(
	const struct bitwriter *bitwriter,
	const struct expect *expect)
{
	size_t index;
	unsigned bit;

	/* A broken string or a full writer never matches. */
	if (expect->overflow != 0 || bitwriter->overflow != 0)
		return 0;

	/* The lengths agree first, then every bit, most significant first. */
	if (expect->length != bitwriter->bits) {
		printf("length %lu, expected %lu\n", (unsigned long)bitwriter->bits, (unsigned long)expect->length);
		return 0;
	}
	for (index = 0U; index < expect->length; index++) {
		bit = (bitwriter->data[index / 8U] >> (7U - index % 8U)) & 1U;
		if (bit != (unsigned)(expect->text[index] - '0')) {
			printf("bit %lu differs\n", (unsigned long)index);
			return 0;
		}
	}

	/* Succeeded: the strings are the same. */
	return 1;
}

/* Checks Ceil( Log2( x ) ) and the 64-bit write. */
static void
check_basics(
	void)
{
	/* Ceil( Log2( x ) ) of 5.7 at its small values and at the top. */
	CHECK(h265w_ceil_log2(0U) == 0U);
	CHECK(h265w_ceil_log2(1U) == 0U);
	CHECK(h265w_ceil_log2(2U) == 1U);
	CHECK(h265w_ceil_log2(3U) == 2U);
	CHECK(h265w_ceil_log2(4U) == 2U);
	CHECK(h265w_ceil_log2(5U) == 3U);
	CHECK(h265w_ceil_log2(0x80000000U) == 31U);
	CHECK(h265w_ceil_log2(0x80000001U) == 32U);

	/* 43 bits with the top and the bottom set. */
	bitwriter_init(&writer);
	h265w_u64(&writer, ((uint64_t)1U << 42) | 1U, 43U);
	expect_init(&expected);
	expect_add(&expected, "1");
	expect_zeros(&expected, 41U);
	expect_add(&expected, "1");
	CHECK(bits_equal(&writer, &expected));
}

/* Checks the NAL unit header, the escaping and the byte stream (7.3.1, 7.4.2, B.2). */
static void
check_nal(
	void)
{
	static const uint8_t rbsp[] = { 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U };
	static const uint8_t escaped[] = { 0x02U, 0x01U, 0x00U, 0x00U, 0x03U, 0x01U, 0x00U, 0x00U, 0x03U, 0x00U, 0x03U };
	static const uint8_t rbsp2[] = { 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x04U };
	static const uint8_t escaped2[] = { 0x02U, 0x01U, 0x00U, 0x00U, 0x03U, 0x02U, 0x00U, 0x00U, 0x03U, 0x03U, 0x00U, 0x00U, 0x04U };
	static const uint8_t eos[] = { 0x00U, 0x00U, 0x00U, 0x01U, 0x48U, 0x01U, 0x00U, 0x00U, 0x01U, 0x40U, 0x01U, 0xAAU };
	uint8_t nal[32];
	uint8_t buffer[32];
	uint8_t one;
	size_t size;
	struct h265w_stream stream;

	/*
	 * TRAIL_R (1), layer 0, nuh_temporal_id_plus1 1: header 0x02 0x01; the
	 * payload 00 00 01 00 00 00 escaped before the 01 and the third 00,
	 * and a 03 after the final zero byte.
	 */
	size = h265w_nal(1U, 0U, 1U, rbsp, sizeof(rbsp), nal, sizeof(nal));
	CHECK(size == sizeof(escaped));
	CHECK(size == sizeof(escaped) && memcmp(nal, escaped, sizeof(escaped)) == 0);

	/* 00 00 02 and 00 00 03 escaped, 00 00 04 not. */
	size = h265w_nal(1U, 0U, 1U, rbsp2, sizeof(rbsp2), nal, sizeof(nal));
	CHECK(size == sizeof(escaped2));
	CHECK(size == sizeof(escaped2) && memcmp(nal, escaped2, sizeof(escaped2)) == 0);

	/* VPS (32), layer 63, nuh_temporal_id_plus1 7: 0 100000 1 / 11111 111. */
	size = h265w_nal(32U, 63U, 7U, NULL, 0U, nal, sizeof(nal));
	CHECK(size == 2U && nal[0] == 0x41U && nal[1] == 0xFFU);

	/* Too small an output: nothing. */
	CHECK(h265w_nal(1U, 0U, 1U, rbsp, sizeof(rbsp), nal, sizeof(escaped) - 1U) == 0U);
	CHECK(h265w_nal(1U, 0U, 1U, NULL, 0U, nal, 1U) == 0U);

	/* An end of sequence (36) after a four-byte start code, then a VPS byte 0xAA after a three-byte one. */
	h265w_stream_init(&stream, buffer, sizeof(buffer));
	h265w_stream_bytes(&stream, 36U, 0U, 1U, NULL, 0U);
	stream.start_code_bytes = 3U;
	one = 0xAAU;
	h265w_stream_bytes(&stream, 32U, 0U, 1U, &one, 1U);
	CHECK(stream.overflow == 0 && stream.size == sizeof(eos));
	CHECK(stream.size == sizeof(eos) && memcmp(buffer, eos, sizeof(eos)) == 0);

	/* A stream with no room for one more NAL unit says so and keeps its size. */
	h265w_stream_init(&stream, buffer, 5U);
	h265w_stream_bytes(&stream, 36U, 0U, 1U, NULL, 0U);
	CHECK(stream.overflow != 0 && stream.size == 0U);
}

/* Checks profile_tier_level() (7.3.3). */
static void
check_ptl(
	void)
{
	struct h265w_ptl ptl;

	/* Main, no sub-layers: 96 bits. */
	memset(&ptl, 0, sizeof(ptl));
	h265w_profile_main(&ptl.general);
	ptl.level_idc = 93U;
	bitwriter_init(&writer);
	h265w_ptl(&writer, &ptl, 0U);
	expect_init(&expected);
	expect_ptl_main(&expected);
	CHECK(expected.length == 96U);
	CHECK(bits_equal(&writer, &expected));

	/*
	 * Two sub-layers below the highest: sub-layer 0 has a profile, 1 a
	 * level (30); the reserved pairs of entries 2..7; then sub-layer 0's
	 * profile (space 1, tier 1, idc 2, only compatibility flag 31, flags
	 * 0 1 1 0, the 43 bits 1, the 44th 1) and sub-layer 1's level.
	 */
	ptl.profile_present[0] = 1U;
	ptl.level_present[1] = 1U;
	ptl.sub_level_idc[1] = 30U;
	ptl.sub_profile[0].profile_space = 1U;
	ptl.sub_profile[0].tier = 1U;
	ptl.sub_profile[0].profile_idc = 2U;
	ptl.sub_profile[0].compatibility = 0x80000000U;
	ptl.sub_profile[0].interlaced = 1U;
	ptl.sub_profile[0].non_packed = 1U;
	ptl.sub_profile[0].bits43 = 1U;
	ptl.sub_profile[0].bit44 = 1U;
	bitwriter_init(&writer);
	h265w_ptl(&writer, &ptl, 2U);
	expect_init(&expected);
	expect_ptl_main(&expected);
	expect_add(&expected, "10" "01");
	expect_zeros(&expected, 12U);
	expect_add(&expected, "01" "1" "00010");
	expect_zeros(&expected, 31U);
	expect_add(&expected, "1" "0110");
	expect_zeros(&expected, 42U);
	expect_add(&expected, "1" "1");
	expect_add(&expected, "00011110");
	CHECK(bits_equal(&writer, &expected));
}

/* Checks st_ref_pic_set() (7.3.7) in both forms and both places. */
static void
check_st_rps(
	void)
{
	struct h265w_st_rps rps;

	/*
	 * Explicit, index 0: two negative entries (minus1 0 and 2, used 1 and
	 * 0) and one positive (minus1 1, used 1).
	 */
	memset(&rps, 0, sizeof(rps));
	rps.num_negative_pics = 2U;
	rps.num_positive_pics = 1U;
	rps.delta_poc_s0_minus1[1] = 2U;
	rps.used_by_curr_pic_s0_flag[0] = 1U;
	rps.delta_poc_s1_minus1[0] = 1U;
	rps.used_by_curr_pic_s1_flag[0] = 1U;
	bitwriter_init(&writer);
	h265w_st_rps(&writer, &rps, 0U, 2U);
	expect_init(&expected);
	expect_add(&expected, "011" "010" "1" "1" "011" "0" "010" "1");
	CHECK(bits_equal(&writer, &expected));

	/* At index 0 the prediction flag is absent even when the field is set. */
	memset(&rps, 0, sizeof(rps));
	rps.inter_rps_pred = 1U;
	bitwriter_init(&writer);
	h265w_st_rps(&writer, &rps, 0U, 2U);
	expect_init(&expected);
	expect_add(&expected, "1" "1");
	CHECK(bits_equal(&writer, &expected));

	/*
	 * Predicted, index 1 of an SPS of two sets (no delta_idx_minus1):
	 * sign 1, abs_delta_rps_minus1 0, three pairs for NumDeltaPocs 2
	 * (used 1; used 0 use 1; used 0 use 0).
	 */
	memset(&rps, 0, sizeof(rps));
	rps.inter_rps_pred = 1U;
	rps.delta_idx_minus1 = 5U;
	rps.delta_rps_sign = 1U;
	rps.ref_num_delta_pocs = 2U;
	rps.used_by_curr_pic_flag[0] = 1U;
	rps.use_delta_flag[1] = 1U;
	bitwriter_init(&writer);
	h265w_st_rps(&writer, &rps, 1U, 2U);
	expect_init(&expected);
	expect_add(&expected, "1" "1" "1" "1" "01" "00");
	CHECK(bits_equal(&writer, &expected));

	/*
	 * Predicted, the slice header's (index == num_sets 2): delta_idx_minus1
	 * 1, sign 0, abs_delta_rps_minus1 3, one pair (used 0 use 1).
	 */
	memset(&rps, 0, sizeof(rps));
	rps.inter_rps_pred = 1U;
	rps.delta_idx_minus1 = 1U;
	rps.abs_delta_rps_minus1 = 3U;
	rps.use_delta_flag[0] = 1U;
	bitwriter_init(&writer);
	h265w_st_rps(&writer, &rps, 2U, 2U);
	expect_init(&expected);
	expect_add(&expected, "1" "010" "0" "00100" "01");
	CHECK(bits_equal(&writer, &expected));
}

/* Checks scaling_list_data() (7.3.4) and the delta maker. */
static void
check_scaling(
	void)
{
	static uint8_t lists[4][6][64];
	static uint8_t dc[2][6];
	unsigned size_id;
	unsigned matrix_id;
	unsigned index;
	unsigned coefficients;
	unsigned failures;
	int32_t next;

	/* Every list predicted with delta 0: 6 + 6 + 6 + 2 pairs of "0" "1". */
	memset(&scaling, 0, sizeof(scaling));
	bitwriter_init(&writer);
	h265w_scaling(&writer, &scaling);
	expect_init(&expected);
	for (index = 0U; index < 20U; index++)
		expect_add(&expected, "0" "1");
	CHECK(bits_equal(&writer, &expected));

	/*
	 * sizeId 0 matrixId 0 signalled (deltas 8 then fifteen 0), sizeId 2
	 * matrixId 1 signalled (DC minus8 -7, deltas 64 times 0), sizeId 3
	 * matrixId 3 predicted with delta 1.
	 */
	scaling.pred_mode[0][0] = 1U;
	scaling.delta_coef[0][0][0] = 8;
	scaling.pred_mode[2][1] = 1U;
	scaling.dc_coef_minus8[0][1] = -7;
	scaling.pred_matrix_id_delta[3][3] = 1U;
	bitwriter_init(&writer);
	h265w_scaling(&writer, &scaling);
	expect_init(&expected);
	expect_add(&expected, "1" "000010000");
	expect_ones(&expected, 15U);
	for (index = 0U; index < 5U + 6U + 1U; index++)
		expect_add(&expected, "0" "1");
	expect_add(&expected, "1" "0001111");
	expect_ones(&expected, 64U);
	for (index = 0U; index < 4U + 1U; index++)
		expect_add(&expected, "0" "1");
	expect_add(&expected, "0" "010");
	CHECK(bits_equal(&writer, &expected));

	/*
	 * The delta maker: lists of varied values 1..255 and DCs; replaying the
	 * recurrence of 7.3.4 on the deltas gives the lists back, and every
	 * delta is in -128..127.
	 */
	for (size_id = 0U; size_id < 4U; size_id++) {
		for (matrix_id = 0U; matrix_id < 6U; matrix_id++) {
			for (index = 0U; index < 64U; index++)
				lists[size_id][matrix_id][index] = (uint8_t)(1U + (size_id * 97U + matrix_id * 31U + index * 57U) % 255U);
		}
	}
	dc[0][0] = 1U;
	dc[0][1] = 255U;
	dc[1][0] = 128U;
	dc[1][3] = 7U;
	h265w_scaling_from_lists(&scaling, (const uint8_t (*)[6][64])lists, (const uint8_t (*)[6])dc);
	failures = 0U;
	for (size_id = 0U; size_id < 4U; size_id++) {
		for (matrix_id = 0U; matrix_id < 6U; matrix_id += (size_id == 3U) ? 3U : 1U) {
			coefficients = (size_id == 0U) ? 16U : 64U;
			next = 8;
			if (scaling.pred_mode[size_id][matrix_id] != 1U)
				failures++;
			if (size_id > 1U)
				next = scaling.dc_coef_minus8[size_id - 2U][matrix_id] + 8;
			if (size_id > 1U && next != (int32_t)dc[size_id - 2U][matrix_id])
				failures++;
			for (index = 0U; index < coefficients; index++) {
				if (scaling.delta_coef[size_id][matrix_id][index] < -128 || scaling.delta_coef[size_id][matrix_id][index] > 127)
					failures++;
				next = (next + scaling.delta_coef[size_id][matrix_id][index] + 256) % 256;
				if (next != (int32_t)lists[size_id][matrix_id][index])
					failures++;
			}
		}
	}
	CHECK(failures == 0U);

	/* A wrap: from 8 to 250 is -14, from 250 to 1 is 7. */
	CHECK(scaling.delta_coef[0][0][0] == (int32_t)lists[0][0][0] - 8);
	memset(lists, 0, sizeof(lists));
	lists[0][0][0] = 250U;
	lists[0][0][1] = 1U;
	h265w_scaling_from_lists(&scaling, (const uint8_t (*)[6][64])lists, (const uint8_t (*)[6])dc);
	CHECK(scaling.delta_coef[0][0][0] == -14);
	CHECK(scaling.delta_coef[0][0][1] == 7);
}

/* Checks hrd_parameters() and sub_layer_hrd_parameters() (E.2.2, E.2.3). */
static void
check_hrd(
	void)
{
	/*
	 * Common part with NAL parameters only, no sub-picture parameters,
	 * scales 3 and 5, lengths 23; one sub-layer: not general, within the
	 * CVS (elemental duration 2), cpb_cnt_minus1 1, two NAL entries
	 * (0, 1, CBR; 2, 0, not CBR).
	 */
	memset(&hrd, 0, sizeof(hrd));
	hrd.nal_present = 1U;
	hrd.bit_rate_scale = 3U;
	hrd.cpb_size_scale = 5U;
	hrd.initial_cpb_removal_delay_length_minus1 = 23U;
	hrd.au_cpb_removal_delay_length_minus1 = 23U;
	hrd.dpb_output_delay_length_minus1 = 23U;
	hrd.fixed_pic_rate_within_cvs[0] = 1U;
	hrd.elemental_duration_in_tc_minus1[0] = 2U;
	hrd.cpb_cnt_minus1[0] = 1U;
	hrd.nal[0].cpb_size_value_minus1[0] = 1U;
	hrd.nal[0].cbr_flag[0] = 1U;
	hrd.nal[0].bit_rate_value_minus1[1] = 2U;
	bitwriter_init(&writer);
	h265w_hrd(&writer, &hrd, 1, 0U);
	expect_init(&expected);
	expect_add(&expected, "1" "0" "0" "0011" "0101" "10111" "10111" "10111");
	expect_add(&expected, "0" "1" "011" "010");
	expect_add(&expected, "1" "010" "1" "011" "1" "0");
	CHECK(bits_equal(&writer, &expected));

	/*
	 * No common part (VCL only, with sub-picture parameters, given as the
	 * shared values); two sub-layers: general fixed rate (within inferred
	 * 1, elemental 0, cpb_cnt_minus1 0, a VCL entry of zeros with the DU
	 * values); then not fixed, low delay 1 (no cpb_cnt_minus1, one entry,
	 * CBR).
	 */
	memset(&hrd, 0, sizeof(hrd));
	hrd.vcl_present = 1U;
	hrd.sub_pic_present = 1U;
	hrd.fixed_pic_rate_general[0] = 1U;
	hrd.low_delay_hrd[1] = 1U;
	hrd.cpb_cnt_minus1[1] = 9U;
	hrd.vcl[1].cbr_flag[0] = 1U;
	bitwriter_init(&writer);
	h265w_hrd(&writer, &hrd, 0, 1U);
	expect_init(&expected);
	expect_add(&expected, "1" "1" "1" "1111" "0");
	expect_add(&expected, "0" "0" "1" "1111" "1");
	CHECK(bits_equal(&writer, &expected));
}

/* Checks vui_parameters() (E.2.1). */
static void
check_vui(
	void)
{
	/* Every part present but the overscan and the HRD. */
	memset(&vui, 0, sizeof(vui));
	vui.aspect_ratio_present = 1U;
	vui.aspect_ratio_idc = 255U;
	vui.sar_width = 4U;
	vui.sar_height = 3U;
	vui.signal_present = 1U;
	vui.video_format = 5U;
	vui.full_range = 1U;
	vui.colour_present = 1U;
	vui.colour_primaries = 1U;
	vui.transfer_characteristics = 2U;
	vui.matrix_coeffs = 9U;
	vui.chroma_loc_present = 1U;
	vui.chroma_loc_top = 2U;
	vui.chroma_loc_bottom = 5U;
	vui.field_seq = 1U;
	vui.default_display_window = 1U;
	vui.def_disp_win_offset[1] = 1U;
	vui.def_disp_win_offset[2] = 2U;
	vui.def_disp_win_offset[3] = 3U;
	vui.timing_present = 1U;
	vui.num_units_in_tick = 1001U;
	vui.time_scale = 60000U;
	vui.poc_proportional = 1U;
	vui.bitstream_restriction = 1U;
	vui.tiles_fixed_structure = 1U;
	vui.restricted_ref_pic_lists = 1U;
	vui.max_bytes_per_pic_denom = 2U;
	vui.max_bits_per_min_cu_denom = 1U;
	vui.log2_max_mv_length_horizontal = 15U;
	vui.log2_max_mv_length_vertical = 15U;
	bitwriter_init(&writer);
	h265w_vui(&writer, &vui, 0U);
	expect_init(&expected);
	expect_add(&expected, "1" "11111111" "0000000000000100" "0000000000000011");
	expect_add(&expected, "0");
	expect_add(&expected, "1" "101" "1" "1" "00000001" "00000010" "00001001");
	expect_add(&expected, "1" "011" "00110");
	expect_add(&expected, "0" "1" "0");
	expect_add(&expected, "1" "1" "010" "011" "00100");
	expect_add(&expected, "1");
	expect_zeros(&expected, 22U);
	expect_add(&expected, "1111101001");
	expect_zeros(&expected, 16U);
	expect_add(&expected, "1110101001100000");
	expect_add(&expected, "1" "1" "0");
	expect_add(&expected, "1" "1" "0" "1" "1" "011" "010" "000010000" "000010000");
	CHECK(bits_equal(&writer, &expected));

	/* Nothing present: the eleven flags at 0 (the HRD flag is inside the timing). */
	memset(&vui, 0, sizeof(vui));
	vui.hrd_present = 1U;
	bitwriter_init(&writer);
	h265w_vui(&writer, &vui, 0U);
	expect_init(&expected);
	expect_zeros(&expected, 10U);
	CHECK(bits_equal(&writer, &expected));
}

/* Checks video_parameter_set_rbsp() (7.3.2.1). */
static void
check_vps(
	void)
{
	/* The default VPS. */
	h265w_vps_default(&vps);
	bitwriter_init(&writer);
	h265w_vps(&writer, &vps);
	expect_init(&expected);
	expect_add(&expected, "0000" "1" "1" "000000" "000" "1");
	expect_ones(&expected, 16U);
	expect_ptl_main(&expected);
	expect_add(&expected, "1" "00101" "1" "1");
	expect_add(&expected, "000000" "1" "0" "0");
	expect_align(&expected);
	CHECK(bits_equal(&writer, &expected));

	/*
	 * Two layer sets of layer ids 0..1 (flags 1 1), timing (1, 2) with no
	 * POC proportionality and two HRDs: layer set 0 (common: neither kind,
	 * one sub-layer fixed), layer set 1 with cprms_present_flag 0; then an
	 * extension of three bits 101; sub-layer ordering of the highest only.
	 */
	vps.sub_layer_ordering_info_present = 0U;
	vps.max_layer_id = 1U;
	vps.num_layer_sets_minus1 = 1U;
	vps.layer_id_included = 1U;
	vps.timing_present = 1U;
	vps.num_units_in_tick = 1U;
	vps.time_scale = 2U;
	vps.num_hrd_parameters = 2U;
	vps.hrd_layer_set_idx[1] = 1U;
	vps.hrd[0].fixed_pic_rate_general[0] = 1U;
	vps.hrd[1].fixed_pic_rate_general[0] = 1U;
	vps.extension = 1U;
	vps.extension_data = 5U;
	vps.extension_data_bits = 3U;
	bitwriter_init(&writer);
	h265w_vps(&writer, &vps);
	expect_init(&expected);
	expect_add(&expected, "0000" "1" "1" "000000" "000" "1");
	expect_ones(&expected, 16U);
	expect_ptl_main(&expected);
	expect_add(&expected, "0" "00101" "1" "1");
	expect_add(&expected, "000001" "010" "1" "1");
	expect_add(&expected, "1");
	expect_zeros(&expected, 31U);
	expect_add(&expected, "1");
	expect_zeros(&expected, 30U);
	expect_add(&expected, "10" "0" "011");
	expect_add(&expected, "1" "0" "0" "1" "1" "1");
	expect_add(&expected, "010" "0" "1" "1" "1");
	expect_add(&expected, "1" "101");
	expect_align(&expected);
	CHECK(bits_equal(&writer, &expected));
}

/* Checks seq_parameter_set_rbsp() (7.3.2.2). */
static void
check_sps(
	void)
{
	/* The default SPS. */
	h265w_sps_default(&sps);
	bitwriter_init(&writer);
	h265w_sps(&writer, &sps);
	expect_init(&expected);
	expect_add(&expected, "0000" "000" "1");
	expect_ptl_main(&expected);
	expect_add(&expected, "1" "010" "0000001000001" "0000001000001" "0");
	expect_add(&expected, "1" "1" "00101");
	expect_add(&expected, "1" "00101" "1" "1");
	expect_add(&expected, "1" "010" "1" "011" "010" "010");
	expect_add(&expected, "0" "0" "0" "0");
	expect_add(&expected, "1" "0" "0" "0" "0" "0");
	expect_align(&expected);
	CHECK(bits_equal(&writer, &expected));

	/*
	 * 4:4:4 with separate planes, a conformance window (1, 0, 2, 0), no
	 * sub-layer ordering for the one sub-layer (written as the highest),
	 * scaling enabled without data, PCM (bit depths minus1 7 and 6, sizes
	 * 0 and 1, loop filter disabled), two sets (explicit empty; predicted
	 * from it with one pair), two long-term pictures of 8 bits (5 used,
	 * 200 not), the range extension flags 0x141 and extension_4bits 1 with
	 * the two data bits 10.
	 */
	sps.chroma_format_idc = 3U;
	sps.separate_colour_plane = 1U;
	sps.conformance_window = 1U;
	sps.conf_win_offset[0] = 1U;
	sps.conf_win_offset[2] = 2U;
	sps.sub_layer_ordering_info_present = 0U;
	sps.scaling_list_enabled = 1U;
	sps.pcm = 1U;
	sps.pcm_bit_depth_luma_minus1 = 7U;
	sps.pcm_bit_depth_chroma_minus1 = 6U;
	sps.log2_diff_max_min_pcm = 1U;
	sps.pcm_loop_filter_disabled = 1U;
	sps.num_st_rps = 2U;
	sps.st_rps[1].inter_rps_pred = 1U;
	sps.st_rps[1].used_by_curr_pic_flag[0] = 1U;
	sps.long_term_present = 1U;
	sps.num_lt_sps = 2U;
	sps.lt_poc_lsb[0] = 5U;
	sps.lt_used[0] = 1U;
	sps.lt_poc_lsb[1] = 200U;
	sps.extension_present = 1U;
	sps.range_extension_flag = 1U;
	sps.range_extension = 0x141U;
	sps.extension_4bits = 1U;
	sps.extension_data = 2U;
	sps.extension_data_bits = 2U;
	bitwriter_init(&writer);
	h265w_sps(&writer, &sps);
	expect_init(&expected);
	expect_add(&expected, "0000" "000" "1");
	expect_ptl_main(&expected);
	expect_add(&expected, "1" "00100" "1" "0000001000001" "0000001000001");
	expect_add(&expected, "1" "010" "1" "011" "1");
	expect_add(&expected, "1" "1" "00101");
	expect_add(&expected, "0" "00101" "1" "1");
	expect_add(&expected, "1" "010" "1" "011" "010" "010");
	expect_add(&expected, "1" "0");
	expect_add(&expected, "0" "0" "1" "0111" "0110" "1" "010" "1");
	expect_add(&expected, "011" "1" "1" "1" "0" "1" "1");
	expect_add(&expected, "1" "011" "00000101" "1" "11001000" "0");
	expect_add(&expected, "0" "0" "0");
	expect_add(&expected, "1" "1" "0" "0" "0" "0001");
	expect_add(&expected, "100000101");
	expect_add(&expected, "10");
	expect_align(&expected);
	CHECK(bits_equal(&writer, &expected));
}

/* Checks pic_parameter_set_rbsp() (7.3.2.3). */
static void
check_pps(
	void)
{
	/* The default PPS. */
	h265w_pps_default(&pps);
	bitwriter_init(&writer);
	h265w_pps(&writer, &pps);
	expect_init(&expected);
	expect_add(&expected, "1" "1" "0" "0" "000" "0" "0");
	expect_add(&expected, "1" "1" "1" "0" "0" "0" "1" "1");
	expect_add(&expected, "000000");
	expect_add(&expected, "1" "0");
	expect_add(&expected, "0" "0" "1" "0" "0");
	expect_align(&expected);
	CHECK(bits_equal(&writer, &expected));

	/*
	 * Tiles 3 x 2 not uniform (widths minus1 3 and 0, height minus1 5),
	 * the deblocking control (override on, offsets -2 and 3), and the
	 * range extension with a chroma QP offset list of two (1 -1, 0 0),
	 * depth 1, SAO scales 0 and 1.
	 */
	pps.tiles = 1U;
	pps.num_tile_columns_minus1 = 2U;
	pps.num_tile_rows_minus1 = 1U;
	pps.column_width_minus1[0] = 3U;
	pps.row_height_minus1[0] = 5U;
	pps.deblocking_control_present = 1U;
	pps.deblocking_override_enabled = 1U;
	pps.beta_offset_div2 = -2;
	pps.tc_offset_div2 = 3;
	pps.extension_present = 1U;
	pps.range_extension_flag = 1U;
	pps.chroma_qp_offset_list_enabled = 1U;
	pps.diff_cu_chroma_qp_offset_depth = 1U;
	pps.chroma_qp_offset_list_len_minus1 = 1U;
	pps.cb_qp_offset_list[0] = 1;
	pps.cr_qp_offset_list[0] = -1;
	pps.log2_sao_offset_scale_chroma = 1U;
	bitwriter_init(&writer);
	h265w_pps(&writer, &pps);
	expect_init(&expected);
	expect_add(&expected, "1" "1" "0" "0" "000" "0" "0");
	expect_add(&expected, "1" "1" "1" "0" "0" "0" "1" "1");
	expect_add(&expected, "000010");
	expect_add(&expected, "011" "010" "0" "00100" "1" "00110" "0");
	expect_add(&expected, "1" "1" "1" "0" "00101" "00110");
	expect_add(&expected, "0" "0" "1" "0");
	expect_add(&expected, "1" "1" "0" "0" "0" "0000");
	expect_add(&expected, "0" "1" "010" "010" "010" "011" "1" "1" "1" "010");
	expect_align(&expected);
	CHECK(bits_equal(&writer, &expected));
}

/* Checks the default I slice header of an IDR picture and a broken byte_alignment(). */
static void
check_slice_i(
	void)
{
	/* The context of the default SPS and PPS. */
	h265w_sps_default(&sps);
	h265w_pps_default(&pps);
	h265w_slice_context_from(&context, &sps, &pps, H265W_NAL_IDR_W_RADL);
	CHECK(context.pic_size_ctbs == 16U);
	CHECK(context.log2_max_poc_lsb == 8U);
	CHECK(context.chroma_array_type == 1U);
	CHECK(context.loop_filter_across_slices == 1U);

	/*
	 * first 1, no_output_of_prior_pics 0, pps 0, slice_type I, slice_qp_delta
	 * 0, slice_loop_filter_across_slices_enabled_flag 0 (the deblocking
	 * runs), then byte_alignment().
	 */
	h265w_slice_default(&slice);
	bitwriter_init(&writer);
	h265w_slice(&writer, &slice, &context);
	expect_init(&expected);
	expect_add(&expected, "1" "0" "1" "011" "1" "0");
	expect_add(&expected, "1" "0000000");
	CHECK(bits_equal(&writer, &expected));

	/* A broken alignment: a zero where the one goes, ones where the zeros go. */
	slice.alignment_one = 0U;
	slice.alignment_zero = 1U;
	bitwriter_init(&writer);
	h265w_slice(&writer, &slice, &context);
	expect_init(&expected);
	expect_add(&expected, "1" "0" "1" "011" "1" "0");
	expect_add(&expected, "0" "1111111");
	CHECK(bits_equal(&writer, &expected));
}

/* Checks a P slice header with most of the branches of 7.3.6.1 to 7.3.6.3. */
static void
check_slice_p(
	void)
{
	/* The context: every PPS and SPS control the P header reads. */
	memset(&context, 0, sizeof(context));
	context.nal_type = H265W_NAL_TRAIL_R;
	context.dependent_slices = 1U;
	context.num_extra_slice_header_bits = 2U;
	context.output_flag_present = 1U;
	context.log2_max_poc_lsb = 8U;
	context.num_st_rps = 3U;
	context.long_term_present = 1U;
	context.num_lt_sps = 2U;
	context.temporal_mvp = 1U;
	context.sao = 1U;
	context.chroma_array_type = 1U;
	context.lists_modification_present = 1U;
	context.cabac_init_present = 1U;
	context.weighted_pred = 1U;
	context.pic_size_ctbs = 5U;
	context.num_pic_total_curr = 3U;
	context.deblocking_override_enabled = 1U;
	context.loop_filter_across_slices = 1U;
	context.tiles = 1U;
	context.slice_header_extension_present = 1U;

	/* The header's values. */
	h265w_slice_default(&slice);
	slice.first_in_pic = 0U;
	slice.pps_id = 1U;
	slice.address = 4U;
	slice.reserved_flags = 2U;
	slice.slice_type = H265W_SLICE_P;
	slice.pic_output = 0U;
	slice.poc_lsb = 200U;
	slice.st_rps_sps = 1U;
	slice.st_rps_idx = 2U;
	slice.num_long_term_sps = 1U;
	slice.num_long_term_pics = 1U;
	slice.lt_idx_sps[0] = 1U;
	slice.delta_poc_msb_present[0] = 1U;
	slice.delta_poc_msb_cycle_lt[0] = 3U;
	slice.poc_lsb_lt[1] = 7U;
	slice.used_by_curr_pic_lt[1] = 1U;
	slice.temporal_mvp = 1U;
	slice.sao_luma = 1U;
	slice.num_ref_idx_override = 1U;
	slice.num_ref_idx_active_minus1[0] = 1U;
	slice.list_mod[0] = 1U;
	slice.list_entry[0][0] = 2U;
	slice.cabac_init = 1U;
	slice.collocated_ref_idx = 1U;
	slice.luma_log2_weight_denom = 6U;
	slice.delta_chroma_log2_weight_denom = -1;
	slice.luma_weight_flag[0][0] = 1U;
	slice.chroma_weight_flag[0][1] = 1U;
	slice.delta_luma_weight[0][0] = -3;
	slice.luma_offset[0][0] = 4;
	slice.delta_chroma_weight[0][1][0] = 1;
	slice.delta_chroma_offset[0][1][0] = -2;
	slice.delta_chroma_offset[0][1][1] = 5;
	slice.five_minus_max_num_merge_cand = 2U;
	slice.slice_qp_delta = -5;
	slice.deblocking_override = 1U;
	slice.beta_offset_div2 = 1;
	slice.tc_offset_div2 = -1;
	slice.loop_filter_across_slices = 1U;
	slice.num_entry_point_offsets = 2U;
	slice.offset_len_minus1 = 4U;
	slice.entry_point_offset_minus1 = 17U;
	slice.extension_length = 2U;
	slice.extension_data[0] = 0xA5U;
	slice.extension_data[1] = 0x0FU;
	bitwriter_init(&writer);
	h265w_slice(&writer, &slice, &context);

	/* The bits in syntax order. */
	expect_init(&expected);
	expect_add(&expected, "0" "010" "0" "100");
	expect_add(&expected, "01" "010" "0" "11001000" "1" "10");
	expect_add(&expected, "010" "010" "1" "1" "00100" "00000111" "1" "0");
	expect_add(&expected, "1" "1" "0");
	expect_add(&expected, "1" "010" "1" "10" "00" "1" "010");
	expect_add(&expected, "00111" "011" "1" "0" "0" "1" "00111" "0001000" "010" "00101" "1" "0001010");
	expect_add(&expected, "011" "0001011" "1" "0" "010" "011" "1");
	expect_add(&expected, "011" "00101" "10001" "10001");
	expect_add(&expected, "011" "10100101" "00001111");
	expect_align(&expected);
	CHECK(bits_equal(&writer, &expected));
}

/* Checks a B slice header: a predicted set in the header, the collocated list 1, weights without chroma. */
static void
check_slice_b(
	void)
{
	/* A monochrome sequence, one SPS set, bi-prediction weights, two default list 1 entries. */
	memset(&context, 0, sizeof(context));
	context.nal_type = H265W_NAL_TRAIL_R;
	context.log2_max_poc_lsb = 4U;
	context.num_st_rps = 1U;
	context.temporal_mvp = 1U;
	context.weighted_bipred = 1U;
	context.num_ref_idx_default_minus1[1] = 1U;
	context.pic_size_ctbs = 1U;

	/* The header's values. */
	h265w_slice_default(&slice);
	slice.slice_type = H265W_SLICE_B;
	slice.poc_lsb = 3U;
	slice.st_rps.inter_rps_pred = 1U;
	slice.st_rps.ref_num_delta_pocs = 1U;
	slice.st_rps.used_by_curr_pic_flag[0] = 1U;
	slice.st_rps.used_by_curr_pic_flag[1] = 1U;
	slice.temporal_mvp = 1U;
	slice.mvd_l1_zero = 1U;
	slice.collocated_from_l0 = 0U;
	slice.collocated_ref_idx = 1U;
	slice.luma_weight_flag[0][0] = 1U;
	slice.delta_luma_weight[0][0] = 1;
	slice.luma_weight_flag[1][1] = 1U;
	slice.delta_luma_weight[1][1] = -1;
	slice.luma_offset[1][1] = 2;
	bitwriter_init(&writer);
	h265w_slice(&writer, &slice, &context);

	/* The bits in syntax order. */
	expect_init(&expected);
	expect_add(&expected, "1" "1" "1" "0011" "0" "1" "1" "0" "1" "1" "1" "1");
	expect_add(&expected, "0" "1" "0" "010");
	expect_add(&expected, "1" "1" "010" "1" "0" "1" "011" "00100");
	expect_add(&expected, "1" "1");
	expect_align(&expected);
	CHECK(bits_equal(&writer, &expected));
}

/* Checks a dependent slice segment header, which carries only the first fields. */
static void
check_slice_dependent(
	void)
{
	/* Dependent segments allowed, one CTB (no address bits), wavefronts. */
	memset(&context, 0, sizeof(context));
	context.nal_type = H265W_NAL_TRAIL_R;
	context.dependent_slices = 1U;
	context.pic_size_ctbs = 1U;
	context.entropy_sync = 1U;
	context.sao = 1U;

	/* first 0, pps 0, dependent 1, no entry points, byte_alignment(). */
	h265w_slice_default(&slice);
	slice.first_in_pic = 0U;
	slice.dependent = 1U;
	bitwriter_init(&writer);
	h265w_slice(&writer, &slice, &context);
	expect_init(&expected);
	expect_add(&expected, "0" "1" "1" "1");
	expect_align(&expected);
	CHECK(bits_equal(&writer, &expected));
}

/* Runs the checks. */
int
main(
	void)
{
	int status;

	/* Each syntax structure in turn. */
	check_basics();
	check_nal();
	check_ptl();
	check_st_rps();
	check_scaling();
	check_hrd();
	check_vui();
	check_vps();
	check_sps();
	check_pps();
	check_slice_i();
	check_slice_p();
	check_slice_b();
	check_slice_dependent();

	/* The summary line and the exit status. */
	status = check_finish("h265write");
	return status;
}
