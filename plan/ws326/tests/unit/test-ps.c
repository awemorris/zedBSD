/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the parameter set reader of the software H.264 decoder: every
 * field of sequence and picture parameter sets built with the bit writer
 * from the syntax of 7.3.2.1.1, 7.3.2.1.1.1, 7.3.2.2 and E.1.1, the
 * derived values, the errors for out-of-range and unsupported input,
 * the fall-back rules of the scaling lists (Table 7-2), and the reader's
 * behaviour on truncated and damaged input.
 */

#include "check.h"
#include "bitwriter.h"

#include "userland/desktop/libmedia/h264sw-ps.h"
#include "userland/desktop/libmedia/h264sw-tables.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The kinds of scaling list a test asks the builder for. */
#define LIST_ABSENT 0U
#define LIST_FULL 1U
#define LIST_DEFAULT 2U
#define LIST_SHORT 3U

/* How many entries a LIST_SHORT list sends before it ends with nextScale 0. */
#define SHORT_ENTRIES 4U

/* The VUI fields a test wants in an SPS. */
struct vui_params {
	int present;
	int aspect_present;
	unsigned aspect_idc;
	unsigned sar_width;
	unsigned sar_height;
	int overscan_present;
	int signal_present;
	int full_range;
	int colour_present;
	unsigned matrix;
	int chroma_loc_present;
	unsigned chroma_loc_top;
	unsigned chroma_loc_bottom;
	int timing_present;
	uint32_t units;
	uint32_t scale;
	int nal_hrd;
	int vcl_hrd;
	unsigned cpb_cnt_minus1;
	int restriction_present;
	unsigned max_bytes_denom;
	unsigned max_bits_denom;
	unsigned log2_mv_horizontal;
	unsigned log2_mv_vertical;
	unsigned reorder;
	unsigned dec_buffering;
};

/* The SPS fields a test wants, in the order of the syntax. */
struct sps_params {
	unsigned profile;
	unsigned constraint_flags;
	unsigned level;
	unsigned id;
	unsigned chroma_format_idc;
	unsigned bit_depth_luma_minus8;
	unsigned bit_depth_chroma_minus8;
	int bypass;
	int matrix_present;
	unsigned list_kind[8];
	unsigned list_base[8];
	unsigned log2_frame_minus4;
	unsigned poc_type;
	unsigned log2_poc_minus4;
	int always_zero;
	int32_t offset_non_ref;
	int32_t offset_top_bottom;
	unsigned cycle;
	int32_t offsets[8];
	unsigned max_ref;
	int gaps;
	uint32_t width_minus1;
	uint32_t height_minus1;
	int frame_mbs_only;
	int direct_8x8;
	int crop_present;
	uint32_t crop_left;
	uint32_t crop_right;
	uint32_t crop_top;
	uint32_t crop_bottom;
	struct vui_params vui;
};

/* The PPS fields a test wants, in the order of the syntax. */
struct pps_params {
	unsigned id;
	unsigned sps_id;
	int cabac;
	int bottom_poc;
	unsigned slice_groups_minus1;
	unsigned ref0_minus1;
	unsigned ref1_minus1;
	int weighted_pred;
	unsigned bipred;
	int32_t qp_minus26;
	int32_t qs_minus26;
	int32_t chroma_offset;
	int deblocking;
	int constrained;
	int redundant;
	int extension;
	int transform_8x8;
	int matrix_present;
	unsigned list_kind[8];
	unsigned list_base[8];
	int32_t second_offset;
};

/* Two stores the tests read into: the one under test and a copy to prove a failed read changed nothing. */
static struct h264sw_ps_store store;
static struct h264sw_ps_store saved;

/* The RBSP being built. */
static struct bitwriter writer;

/* Gives the parameters of a plain Baseline stream: 720x576, level 3.0, order count type 2. */
static void
sps_defaults(
	struct sps_params *p)
{
	memset(p, 0, sizeof(*p));
	p->profile = 66U;
	p->level = 30U;
	p->chroma_format_idc = 1U;
	p->poc_type = 2U;
	p->max_ref = 1U;
	p->width_minus1 = 44U;
	p->height_minus1 = 35U;
	p->frame_mbs_only = 1;
	p->direct_8x8 = 1;
}

/* Gives the parameters of a High profile stream, which sends chroma_format_idc and the bit depths. */
static void
sps_high(
	struct sps_params *p)
{
	sps_defaults(p);
	p->profile = 100U;
	p->level = 40U;
}

/* The value entry j of a LIST_FULL or LIST_SHORT list has, for a base. */
static unsigned
list_value(
	unsigned kind,
	unsigned base,
	unsigned index)
{
	/* A short list repeats its last sent value to the end. */
	if (kind == LIST_SHORT &&
	    index >= SHORT_ENTRIES)
		index = SHORT_ENTRIES - 1U;

	/* The values climb by one and wrap every 20 entries, so they stay between base and base + 19. */
	return base + (index % 20U);
}

/* Writes one scaling_list( ) (7.3.2.1.1.1) of the kind and base, for size entries. */
static void
write_scaling_list(
	struct bitwriter *w,
	unsigned kind,
	unsigned base,
	unsigned size)
{
	unsigned index;
	unsigned last;
	unsigned value;
	unsigned sent;

	/* useDefaultScalingMatrixFlag is a first delta_scale that makes nextScale 0: -8 from lastScale 8. */
	if (kind == LIST_DEFAULT) {
		bitwriter_se(w, -8);
		return;
	}

	/* Sends the values as differences from the one before, starting from lastScale 8. */
	sent = size;
	if (kind == LIST_SHORT)
		sent = SHORT_ENTRIES;
	last = 8U;
	for (index = 0U; index < sent; index++) {
		value = list_value(kind, base, index);
		bitwriter_se(w, (int32_t)value - (int32_t)last);
		last = value;
	}

	/* A short list ends with a delta_scale that makes nextScale 0, so the rest repeats the last value. */
	if (kind == LIST_SHORT)
		bitwriter_se(w, -(int32_t)last);
}

/* Writes the list present flags and the lists of a scaling matrix for count lists. */
static void
write_scaling_matrix(
	struct bitwriter *w,
	const unsigned *kind,
	const unsigned *base,
	unsigned count)
{
	unsigned index;
	unsigned size;

	/* Each list has a present flag, then the list when the flag is 1. */
	for (index = 0U; index < count; index++) {
		if (kind[index] == LIST_ABSENT) {
			bitwriter_u(w, 0U, 1U);
			continue;
		}
		bitwriter_u(w, 1U, 1U);
		size = 16U;
		if (index >= 6U)
			size = 64U;
		write_scaling_list(w, kind[index], base[index], size);
	}
}

/* Writes hrd_parameters( ) (E.1.2) with count + 1 CPB specifications of arbitrary values. */
static void
write_hrd(
	struct bitwriter *w,
	unsigned cpb_cnt_minus1)
{
	unsigned index;

	bitwriter_ue(w, cpb_cnt_minus1);
	bitwriter_u(w, 3U, 4U);
	bitwriter_u(w, 5U, 4U);
	for (index = 0U; index <= cpb_cnt_minus1; index++) {
		bitwriter_ue(w, 1000U + index);
		bitwriter_ue(w, 5000U + index);
		bitwriter_u(w, index & 1U, 1U);
	}
	bitwriter_u(w, 23U, 5U);
	bitwriter_u(w, 23U, 5U);
	bitwriter_u(w, 23U, 5U);
	bitwriter_u(w, 24U, 5U);
}

/* Writes vui_parameters( ) (E.1.1). */
static void
write_vui(
	struct bitwriter *w,
	const struct vui_params *v)
{
	/* The aspect ratio. */
	bitwriter_u(w, (unsigned)v->aspect_present, 1U);
	if (v->aspect_present != 0) {
		bitwriter_u(w, v->aspect_idc, 8U);
		if (v->aspect_idc == 255U) {
			bitwriter_u(w, v->sar_width, 16U);
			bitwriter_u(w, v->sar_height, 16U);
		}
	}

	/* The overscan flags. */
	bitwriter_u(w, (unsigned)v->overscan_present, 1U);
	if (v->overscan_present != 0)
		bitwriter_u(w, 1U, 1U);

	/* The video signal: video_format 5 (unspecified), the range, and the colour description. */
	bitwriter_u(w, (unsigned)v->signal_present, 1U);
	if (v->signal_present != 0) {
		bitwriter_u(w, 5U, 3U);
		bitwriter_u(w, (unsigned)v->full_range, 1U);
		bitwriter_u(w, (unsigned)v->colour_present, 1U);
		if (v->colour_present != 0) {
			bitwriter_u(w, 1U, 8U);
			bitwriter_u(w, 1U, 8U);
			bitwriter_u(w, v->matrix, 8U);
		}
	}

	/* The chroma sample location types. */
	bitwriter_u(w, (unsigned)v->chroma_loc_present, 1U);
	if (v->chroma_loc_present != 0) {
		bitwriter_ue(w, v->chroma_loc_top);
		bitwriter_ue(w, v->chroma_loc_bottom);
	}

	/* The timing information with fixed_frame_rate_flag 1. */
	bitwriter_u(w, (unsigned)v->timing_present, 1U);
	if (v->timing_present != 0) {
		bitwriter_u(w, v->units, 32U);
		bitwriter_u(w, v->scale, 32U);
		bitwriter_u(w, 1U, 1U);
	}

	/* The HRD parameters of the NAL and the VCL, and low_delay_hrd_flag when either is present. */
	bitwriter_u(w, (unsigned)v->nal_hrd, 1U);
	if (v->nal_hrd != 0)
		write_hrd(w, v->cpb_cnt_minus1);
	bitwriter_u(w, (unsigned)v->vcl_hrd, 1U);
	if (v->vcl_hrd != 0)
		write_hrd(w, v->cpb_cnt_minus1);
	if (v->nal_hrd != 0 ||
	    v->vcl_hrd != 0)
		bitwriter_u(w, 0U, 1U);

	/* pic_struct_present_flag, then the bitstream restriction. */
	bitwriter_u(w, 1U, 1U);
	bitwriter_u(w, (unsigned)v->restriction_present, 1U);
	if (v->restriction_present != 0) {
		bitwriter_u(w, 1U, 1U);
		bitwriter_ue(w, v->max_bytes_denom);
		bitwriter_ue(w, v->max_bits_denom);
		bitwriter_ue(w, v->log2_mv_horizontal);
		bitwriter_ue(w, v->log2_mv_vertical);
		bitwriter_ue(w, v->reorder);
		bitwriter_ue(w, v->dec_buffering);
	}
}

/*
 * Writes a whole SPS RBSP (7.3.2.1.1) and returns the bit position at
 * which the VUI starts (the position of vui_parameters_present_flag).
 */
static size_t
write_sps(
	struct bitwriter *w,
	const struct sps_params *p)
{
	unsigned index;
	size_t vui_start;

	/* profile_idc, the six constraint flags (set0 first), the two reserved bits, level_idc and the id. */
	bitwriter_init(w);
	bitwriter_u(w, p->profile, 8U);
	for (index = 0U; index < 6U; index++)
		bitwriter_u(w, (p->constraint_flags >> index) & 1U, 1U);
	bitwriter_u(w, 0U, 2U);
	bitwriter_u(w, p->level, 8U);
	bitwriter_ue(w, p->id);

	/* The fields of the High profiles. */
	if (p->profile == 100U ||
	    p->profile == 110U ||
	    p->profile == 122U ||
	    p->profile == 244U) {
		bitwriter_ue(w, p->chroma_format_idc);
		if (p->chroma_format_idc == 3U)
			bitwriter_u(w, 0U, 1U);
		bitwriter_ue(w, p->bit_depth_luma_minus8);
		bitwriter_ue(w, p->bit_depth_chroma_minus8);
		bitwriter_u(w, (unsigned)p->bypass, 1U);
		bitwriter_u(w, (unsigned)p->matrix_present, 1U);
		if (p->matrix_present != 0) {
			if (p->chroma_format_idc != 3U) {
				write_scaling_matrix(w, p->list_kind, p->list_base, 8U);
			}
		}
	}

	/* The frame number length and the order count fields. */
	bitwriter_ue(w, p->log2_frame_minus4);
	bitwriter_ue(w, p->poc_type);
	if (p->poc_type == 0U) {
		bitwriter_ue(w, p->log2_poc_minus4);
	} else if (p->poc_type == 1U) {
		bitwriter_u(w, (unsigned)p->always_zero, 1U);
		bitwriter_se(w, p->offset_non_ref);
		bitwriter_se(w, p->offset_top_bottom);
		bitwriter_ue(w, p->cycle);
		for (index = 0U; index < p->cycle && index < 8U; index++)
			bitwriter_se(w, p->offsets[index]);
	}

	/* The reference frames, the size, the interlace flags and the direct inference flag. */
	bitwriter_ue(w, p->max_ref);
	bitwriter_u(w, (unsigned)p->gaps, 1U);
	bitwriter_ue(w, p->width_minus1);
	bitwriter_ue(w, p->height_minus1);
	bitwriter_u(w, (unsigned)p->frame_mbs_only, 1U);
	if (p->frame_mbs_only == 0)
		bitwriter_u(w, 0U, 1U);
	bitwriter_u(w, (unsigned)p->direct_8x8, 1U);

	/* The cropping window. */
	bitwriter_u(w, (unsigned)p->crop_present, 1U);
	if (p->crop_present != 0) {
		bitwriter_ue(w, p->crop_left);
		bitwriter_ue(w, p->crop_right);
		bitwriter_ue(w, p->crop_top);
		bitwriter_ue(w, p->crop_bottom);
	}

	/* The VUI, or its absence, and the trailing bits. */
	vui_start = w->bits;
	bitwriter_u(w, (unsigned)p->vui.present, 1U);
	if (p->vui.present != 0)
		write_vui(w, &p->vui);
	bitwriter_trailing(w);

	/* Succeeded: where the VUI starts. */
	return vui_start;
}

/* Gives the parameters of a PPS with the commonest values (CAVLC, no extension). */
static void
pps_defaults(
	struct pps_params *p)
{
	memset(p, 0, sizeof(*p));
	p->ref0_minus1 = 0U;
	p->qp_minus26 = 0;
}

/* Writes a whole PPS RBSP (7.3.2.2); a slice group count above 0 stops after num_slice_groups_minus1. */
static void
write_pps(
	struct bitwriter *w,
	const struct pps_params *p)
{
	unsigned count;

	/* The ids, the entropy coding mode, the bottom field flag and the slice group count. */
	bitwriter_init(w);
	bitwriter_ue(w, p->id);
	bitwriter_ue(w, p->sps_id);
	bitwriter_u(w, (unsigned)p->cabac, 1U);
	bitwriter_u(w, (unsigned)p->bottom_poc, 1U);
	bitwriter_ue(w, p->slice_groups_minus1);
	if (p->slice_groups_minus1 != 0U) {
		bitwriter_trailing(w);
		return;
	}

	/* The reference list lengths, the weighted prediction, the quantization parameters and the flags. */
	bitwriter_ue(w, p->ref0_minus1);
	bitwriter_ue(w, p->ref1_minus1);
	bitwriter_u(w, (unsigned)p->weighted_pred, 1U);
	bitwriter_u(w, p->bipred, 2U);
	bitwriter_se(w, p->qp_minus26);
	bitwriter_se(w, p->qs_minus26);
	bitwriter_se(w, p->chroma_offset);
	bitwriter_u(w, (unsigned)p->deblocking, 1U);
	bitwriter_u(w, (unsigned)p->constrained, 1U);
	bitwriter_u(w, (unsigned)p->redundant, 1U);

	/* The High profile extension, with the scaling lists of 6 or 8 entries. */
	if (p->extension != 0) {
		bitwriter_u(w, (unsigned)p->transform_8x8, 1U);
		bitwriter_u(w, (unsigned)p->matrix_present, 1U);
		if (p->matrix_present != 0) {
			count = 6U;
			if (p->transform_8x8 != 0)
				count = 8U;
			write_scaling_matrix(w, p->list_kind, p->list_base, count);
		}
		bitwriter_se(w, p->second_offset);
	}
	bitwriter_trailing(w);
}

/* Reads the RBSP in w as an SPS into the store. */
static int
read_sps_bytes(
	const uint8_t *data,
	size_t size)
{
	struct vcodec_bitreader bits;

	vcodec_bitreader_init(&bits, data, size);
	return h264sw_ps_read_sps(&store, &bits);
}

/* Reads the RBSP in w as a PPS into the store. */
static int
read_pps_bytes(
	const uint8_t *data,
	size_t size)
{
	struct vcodec_bitreader bits;

	vcodec_bitreader_init(&bits, data, size);
	return h264sw_ps_read_pps(&store, &bits);
}

/* Builds an SPS from the parameters and reads it; returns the reader's answer. */
static int
read_sps(
	const struct sps_params *p)
{
	size_t start;

	start = write_sps(&writer, p);
	CHECK(start != 0U);
	return read_sps_bytes(writer.data, bitwriter_bytes(&writer));
}

/* Builds a PPS from the parameters and reads it; returns the reader's answer. */
static int
read_pps(
	const struct pps_params *p)
{
	write_pps(&writer, p);
	return read_pps_bytes(writer.data, bitwriter_bytes(&writer));
}

/* Checks that a failed read left the whole store as it was before it. */
static void
check_store_unchanged(
	void)
{
	CHECK(memcmp(&store, &saved, sizeof(store)) == 0);
}

/* Takes a copy of the store before a read that is expected to fail. */
static void
save_store(
	void)
{
	memcpy(&saved, &store, sizeof(store));
}

/* Checks a Baseline SPS: every field and the derived values. */
static void
test_sps_baseline(
	void)
{
	struct sps_params p;
	const struct h264sw_sps *s;
	int error;

	/* Constraint_set1 and set3 (bits 1 and 3), level 3.0, 720x576 (45 x 36 macroblocks), type 2, no crop, no VUI. */
	h264sw_ps_init(&store);
	sps_defaults(&p);
	p.constraint_flags = 0x0AU;
	p.id = 3U;
	p.log2_frame_minus4 = 4U;
	p.max_ref = 2U;
	p.gaps = 1;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps_present[3] == 1);
	CHECK(store.sps_present[0] == 0);

	/* Every field. */
	s = &store.sps[3];
	CHECK(s->profile_idc == 66U);
	CHECK(s->constraint_flags == 0x0AU);
	CHECK(s->level_idc == 30U);
	CHECK(s->id == 3U);
	CHECK(s->scaling_matrix_present == 0);
	CHECK(s->log2_max_frame_num == 8U);
	CHECK(s->max_frame_num == 256U);
	CHECK(s->poc_type == 2U);
	CHECK(s->max_num_ref_frames == 2U);
	CHECK(s->gaps_allowed == 1);
	CHECK(s->width_mbs == 45U);
	CHECK(s->height_mbs == 36U);
	CHECK(s->size_mbs == 1620U);
	CHECK(s->direct_8x8_inference == 1);
	CHECK(s->crop_left == 0U && s->crop_right == 0U && s->crop_top == 0U && s->crop_bottom == 0U);
	CHECK(s->vui_present == 0);
	CHECK(s->sar_width == 0U && s->sar_height == 0U);
	CHECK(s->signal_present == 0 && s->colour_present == 0 && s->restriction_present == 0);
	CHECK(s->matrix_coefficients == 2U);

	/* Level 3.0 holds 8100 macroblocks: 8100 / 1620 = 5 frames. */
	CHECK(s->dpb_frames == 5U);
	CHECK(store.sps_generation[3] == 0U);
}

/* Checks the order count fields of the three types, and the largest frame number length. */
static void
test_sps_poc(
	void)
{
	struct sps_params p;
	const struct h264sw_sps *s;
	int error;

	/* Type 0 with a 16-bit least significant part, and the longest frame number (log2 16, MaxFrameNum 65536). */
	h264sw_ps_init(&store);
	sps_defaults(&p);
	p.poc_type = 0U;
	p.log2_poc_minus4 = 12U;
	p.log2_frame_minus4 = 12U;
	error = read_sps(&p);
	CHECK(error == 0);
	s = &store.sps[0];
	CHECK(s->poc_type == 0U);
	CHECK(s->log2_max_poc_lsb == 16U);
	CHECK(s->log2_max_frame_num == 16U);
	CHECK(s->max_frame_num == 65536U);

	/* Type 1 with a cycle of 3 reference frames: ExpectedDeltaPerPicOrderCntCycle is 2 - 3 + 5 = 4 (7-12). */
	sps_defaults(&p);
	p.poc_type = 1U;
	p.always_zero = 1;
	p.offset_non_ref = -7;
	p.offset_top_bottom = 3;
	p.cycle = 3U;
	p.offsets[0] = 2;
	p.offsets[1] = -3;
	p.offsets[2] = 5;
	error = read_sps(&p);
	CHECK(error == 0);
	s = &store.sps[0];
	CHECK(s->poc_type == 1U);
	CHECK(s->delta_pic_order_always_zero == 1);
	CHECK(s->offset_for_non_ref_pic == -7);
	CHECK(s->offset_for_top_to_bottom_field == 3);
	CHECK(s->poc_cycle_length == 3U);
	CHECK(s->offset_for_ref_frame[0] == 2);
	CHECK(s->offset_for_ref_frame[1] == -3);
	CHECK(s->offset_for_ref_frame[2] == 5);
	CHECK(s->expected_delta_per_poc_cycle == 4);

	/* Type 1 with no cycle at all: the expected delta is 0. */
	p.cycle = 0U;
	p.always_zero = 0;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].poc_cycle_length == 0U);
	CHECK(store.sps[0].expected_delta_per_poc_cycle == 0);
	CHECK(store.sps[0].delta_pic_order_always_zero == 0);

	/* Type 2. */
	sps_defaults(&p);
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].poc_type == 2U);
}

/* Checks the full 255-entry cycle of type 1, whose offsets sum beyond 32 bits. */
static void
test_sps_poc_cycle_255(
	void)
{
	struct bitwriter *w;
	struct sps_params p;
	unsigned index;
	size_t start;
	int error;

	/* Builds a type 1 SPS by hand with 255 offsets of 2147483647 (the largest value of se(v)). */
	h264sw_ps_init(&store);
	sps_defaults(&p);
	p.poc_type = 1U;
	p.cycle = 0U;
	w = &writer;
	start = write_sps(w, &p);
	CHECK(start != 0U);

	/* Rewrites the stream with the cycle: profile 66, level 30, id 0, then the fields. */
	bitwriter_init(w);
	bitwriter_u(w, 66U, 8U);
	bitwriter_u(w, 0U, 8U);
	bitwriter_u(w, 30U, 8U);
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_se(w, 0);
	bitwriter_se(w, 0);
	bitwriter_ue(w, 255U);
	for (index = 0U; index < 255U; index++)
		bitwriter_se(w, 2147483647);
	bitwriter_ue(w, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_ue(w, 10U);
	bitwriter_ue(w, 10U);
	bitwriter_u(w, 1U, 1U);
	bitwriter_u(w, 1U, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_trailing(w);
	error = read_sps_bytes(w->data, bitwriter_bytes(w));
	CHECK(error == 0);
	CHECK(store.sps[0].poc_cycle_length == 255U);
	CHECK(store.sps[0].expected_delta_per_poc_cycle == 255LL * 2147483647LL);
	CHECK(store.sps[0].offset_for_ref_frame[254] == 2147483647);

	/* A cycle of 256 is out of range. */
	save_store();
	bitwriter_init(w);
	bitwriter_u(w, 66U, 8U);
	bitwriter_u(w, 0U, 8U);
	bitwriter_u(w, 30U, 8U);
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_se(w, 0);
	bitwriter_se(w, 0);
	bitwriter_ue(w, 256U);
	for (index = 0U; index < 256U; index++)
		bitwriter_se(w, 1);
	bitwriter_trailing(w);
	error = read_sps_bytes(w->data, bitwriter_bytes(w));
	CHECK(error == EINVAL);
	check_store_unchanged();
}

/* Checks the cropping window, including the largest window that fits and the first that does not. */
static void
test_sps_crop(
	void)
{
	struct sps_params p;
	int error;

	/* 11 x 9 macroblocks: a 176 x 144 picture; crop units are 2 columns and 2 rows. */
	h264sw_ps_init(&store);
	sps_defaults(&p);
	p.width_minus1 = 10U;
	p.height_minus1 = 8U;
	p.crop_present = 1;
	p.crop_left = 1U;
	p.crop_right = 2U;
	p.crop_top = 3U;
	p.crop_bottom = 4U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].crop_left == 2U);
	CHECK(store.sps[0].crop_right == 4U);
	CHECK(store.sps[0].crop_top == 6U);
	CHECK(store.sps[0].crop_bottom == 8U);

	/* The common 1080p crop: 1920 x 1088 cropped by 8 rows at the bottom, 4 offset units. */
	sps_defaults(&p);
	p.width_minus1 = 119U;
	p.height_minus1 = 67U;
	p.crop_present = 1;
	p.crop_bottom = 4U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].crop_bottom == 8U);
	CHECK(store.sps[0].width_mbs == 120U);
	CHECK(store.sps[0].height_mbs == 68U);

	/* The largest window that keeps a column pair: left 43 + right 44 + 1 = 88 units of 2 columns in 176. */
	sps_defaults(&p);
	p.width_minus1 = 10U;
	p.height_minus1 = 8U;
	p.crop_present = 1;
	p.crop_left = 43U;
	p.crop_right = 44U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].crop_left == 86U);
	CHECK(store.sps[0].crop_right == 88U);

	/* One more unit empties the window: EINVAL, and the store keeps the earlier set. */
	save_store();
	p.crop_left = 44U;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* The same for rows: 144 rows are 72 units; top 36 + bottom 36 + 1 is 73. */
	sps_defaults(&p);
	p.width_minus1 = 10U;
	p.height_minus1 = 8U;
	p.crop_present = 1;
	p.crop_top = 36U;
	p.crop_bottom = 36U;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* A huge offset cannot overflow the check. */
	p.crop_top = 0U;
	p.crop_bottom = 0xFFFFFFFEU;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();
}

/* Checks the VUI: the aspect ratios, the signal, the HRD skip and the restriction. */
static void
test_sps_vui(
	void)
{
	struct sps_params p;
	const struct h264sw_sps *s;
	unsigned idc;
	static const unsigned ratio[17][2] = {
		{ 0U, 0U }, { 1U, 1U }, { 12U, 11U }, { 10U, 11U }, { 16U, 11U }, { 40U, 33U }, { 24U, 11U },
		{ 20U, 11U }, { 32U, 11U }, { 80U, 33U }, { 18U, 11U }, { 15U, 11U }, { 64U, 33U }, { 160U, 99U },
		{ 4U, 3U }, { 3U, 2U }, { 2U, 1U }
	};
	int error;

	/* Every aspect_ratio_idc of Table E-1. */
	h264sw_ps_init(&store);
	for (idc = 1U; idc <= 16U; idc++) {
		sps_defaults(&p);
		p.vui.present = 1;
		p.vui.aspect_present = 1;
		p.vui.aspect_idc = idc;
		error = read_sps(&p);
		CHECK(error == 0);
		CHECK(store.sps[0].vui_present == 1);
		CHECK(store.sps[0].sar_width == ratio[idc][0]);
		CHECK(store.sps[0].sar_height == ratio[idc][1]);
	}

	/* The extended ratio carries its own sizes. */
	sps_defaults(&p);
	p.vui.present = 1;
	p.vui.aspect_present = 1;
	p.vui.aspect_idc = 255U;
	p.vui.sar_width = 4000U;
	p.vui.sar_height = 3001U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].sar_width == 4000U);
	CHECK(store.sps[0].sar_height == 3001U);

	/* An extended ratio with a zero side is unspecified: 0 and 0. */
	p.vui.sar_width = 0U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 1);
	CHECK(store.sps[0].sar_width == 0U);
	CHECK(store.sps[0].sar_height == 0U);

	/* aspect_ratio_idc 0 is unspecified too. */
	p.vui.aspect_idc = 0U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].sar_width == 0U && store.sps[0].sar_height == 0U);

	/* The signal type, the colour description, the overscan flag and the chroma location. */
	sps_defaults(&p);
	p.vui.present = 1;
	p.vui.overscan_present = 1;
	p.vui.signal_present = 1;
	p.vui.full_range = 1;
	p.vui.colour_present = 1;
	p.vui.matrix = 6U;
	p.vui.chroma_loc_present = 1;
	p.vui.chroma_loc_top = 5U;
	p.vui.chroma_loc_bottom = 0U;
	error = read_sps(&p);
	CHECK(error == 0);
	s = &store.sps[0];
	CHECK(s->vui_present == 1);
	CHECK(s->signal_present == 1);
	CHECK(s->full_range == 1);
	CHECK(s->colour_present == 1);
	CHECK(s->matrix_coefficients == 6U);

	/* A signal type without a colour description keeps the unspecified matrix 2. */
	p.vui.colour_present = 0;
	p.vui.full_range = 0;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].signal_present == 1);
	CHECK(store.sps[0].full_range == 0);
	CHECK(store.sps[0].colour_present == 0);
	CHECK(store.sps[0].matrix_coefficients == 2U);

	/* Both HRDs with 3 CPBs, timing, and the restriction after them: the skip must land on the restriction. */
	sps_defaults(&p);
	p.max_ref = 2U;
	p.vui.present = 1;
	p.vui.timing_present = 1;
	p.vui.units = 1001U;
	p.vui.scale = 60000U;
	p.vui.nal_hrd = 1;
	p.vui.vcl_hrd = 1;
	p.vui.cpb_cnt_minus1 = 2U;
	p.vui.restriction_present = 1;
	p.vui.max_bytes_denom = 2U;
	p.vui.max_bits_denom = 1U;
	p.vui.log2_mv_horizontal = 15U;
	p.vui.log2_mv_vertical = 15U;
	p.vui.reorder = 2U;
	p.vui.dec_buffering = 4U;
	error = read_sps(&p);
	CHECK(error == 0);
	s = &store.sps[0];
	CHECK(s->vui_present == 1);
	CHECK(s->restriction_present == 1);
	CHECK(s->max_num_reorder_frames == 2U);
	CHECK(s->max_dec_frame_buffering == 4U);

	/* The restriction sets the buffer size: 4, though the level alone would give 5. */
	CHECK(s->dpb_frames == 4U);
}

/* Checks the VUI that is dropped and the SPS kept: reserved aspect ratios, bad ranges, truncation. */
static void
test_sps_vui_dropped(
	void)
{
	struct sps_params p;
	size_t vui_start;
	size_t cut;
	int error;

	/* aspect_ratio_idc 17 and 254 are reserved: the VUI goes, the SPS stays, with a valid SPS around it. */
	h264sw_ps_init(&store);
	sps_defaults(&p);
	p.vui.present = 1;
	p.vui.aspect_present = 1;
	p.vui.aspect_idc = 17U;
	p.vui.restriction_present = 1;
	p.vui.dec_buffering = 3U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps_present[0] == 1);
	CHECK(store.sps[0].vui_present == 0);
	CHECK(store.sps[0].sar_width == 0U && store.sps[0].sar_height == 0U);
	CHECK(store.sps[0].restriction_present == 0);
	CHECK(store.sps[0].dpb_frames == 5U);
	p.vui.aspect_idc = 254U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 0);

	/* A chroma location type of 6 is out of range: the VUI goes. */
	sps_defaults(&p);
	p.vui.present = 1;
	p.vui.signal_present = 1;
	p.vui.full_range = 1;
	p.vui.chroma_loc_present = 1;
	p.vui.chroma_loc_top = 6U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 0);
	CHECK(store.sps[0].signal_present == 0);
	CHECK(store.sps[0].full_range == 0);

	/* A timing tick of 0 is out of range. */
	sps_defaults(&p);
	p.vui.present = 1;
	p.vui.timing_present = 1;
	p.vui.units = 0U;
	p.vui.scale = 25U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 0);

	/* More CPB specifications than the 32 allowed: the VUI goes (cpb_cnt_minus1 is 32). */
	sps_defaults(&p);
	p.vui.present = 1;
	p.vui.nal_hrd = 1;
	p.vui.cpb_cnt_minus1 = 32U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 0);

	/* max_num_reorder_frames above max_dec_frame_buffering, and a buffer smaller than max_num_ref_frames, are out of range. */
	sps_defaults(&p);
	p.max_ref = 2U;
	p.vui.present = 1;
	p.vui.restriction_present = 1;
	p.vui.reorder = 3U;
	p.vui.dec_buffering = 2U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 0);
	p.vui.reorder = 0U;
	p.vui.dec_buffering = 1U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 0);

	/* A buffer size above 16 frames and a motion vector length above 15 bits are out of range. */
	p.vui.dec_buffering = 17U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 0);
	p.vui.dec_buffering = 2U;
	p.vui.log2_mv_horizontal = 16U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 0);
	p.vui.log2_mv_horizontal = 15U;
	p.vui.max_bytes_denom = 17U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 0);

	/* A VUI cut off inside its timing information: the SPS stays, with no VUI. */
	sps_defaults(&p);
	p.vui.present = 1;
	p.vui.timing_present = 1;
	p.vui.units = 1001U;
	p.vui.scale = 60000U;
	p.vui.restriction_present = 1;
	p.vui.dec_buffering = 2U;
	vui_start = write_sps(&writer, &p);
	cut = vui_start / 8U + 3U;
	CHECK(cut < bitwriter_bytes(&writer));
	h264sw_ps_init(&store);
	error = read_sps_bytes(writer.data, cut);
	CHECK(error == 0);
	CHECK(store.sps_present[0] == 1);
	CHECK(store.sps[0].vui_present == 0);
	CHECK(store.sps[0].restriction_present == 0);
	CHECK(store.sps[0].width_mbs == 45U);
	CHECK(store.sps[0].dpb_frames == 5U);

	/* The complete one reads with its VUI, to show the cut was what dropped it. */
	h264sw_ps_init(&store);
	error = read_sps_bytes(writer.data, bitwriter_bytes(&writer));
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 1);
	CHECK(store.sps[0].dpb_frames == 2U);
}

/* Checks the SPS values outside the ranges of the syntax: EINVAL, store untouched. */
static void
test_sps_invalid(
	void)
{
	struct sps_params p;
	static const uint8_t long_code[] = { 0x42U, 0x00U, 0x1EU, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U };
	int error;

	/* A good SPS first, so the store has something a failed read must not disturb. */
	h264sw_ps_init(&store);
	sps_defaults(&p);
	p.id = 1U;
	error = read_sps(&p);
	CHECK(error == 0);
	save_store();

	/* log2_max_frame_num_minus4 13. */
	sps_defaults(&p);
	p.id = 1U;
	p.log2_frame_minus4 = 13U;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* pic_order_cnt_type 3. */
	sps_defaults(&p);
	p.id = 1U;
	p.poc_type = 3U;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* log2_max_pic_order_cnt_lsb_minus4 13. */
	sps_defaults(&p);
	p.id = 1U;
	p.poc_type = 0U;
	p.log2_poc_minus4 = 13U;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* seq_parameter_set_id 32. */
	sps_defaults(&p);
	p.id = 32U;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* max_num_ref_frames 17. */
	sps_defaults(&p);
	p.id = 1U;
	p.max_ref = 17U;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* The largest code the reader takes (codeNum 2^32 - 2) is far above the range. */
	sps_defaults(&p);
	p.id = 1U;
	p.log2_frame_minus4 = 0xFFFFFFFEU;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* A code with 32 leading zero bits is longer than the reader takes: profile 66, level 30, then 32 zeros for the id. */
	error = read_sps_bytes(long_code, sizeof(long_code));
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* bit_depth_luma_minus8 7 is out of range, though 1 would only be unsupported. */
	sps_high(&p);
	p.id = 1U;
	p.bit_depth_luma_minus8 = 7U;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* chroma_format_idc 4. */
	sps_high(&p);
	p.id = 1U;
	p.chroma_format_idc = 4U;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* direct_8x8_inference_flag must be 1 with interlaced coding allowed: EINVAL rather than ENOTSUP. */
	sps_defaults(&p);
	p.id = 1U;
	p.frame_mbs_only = 0;
	p.direct_8x8 = 0;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* A valid-looking SPS that is also damaged is EINVAL, not ENOTSUP: an unsupported bit depth and a bad frame number length. */
	sps_high(&p);
	p.id = 1U;
	p.bit_depth_luma_minus8 = 2U;
	p.log2_frame_minus4 = 13U;
	error = read_sps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();
}

/* Checks the valid SPSs the decoder does not take: ENOTSUP, store untouched. */
static void
test_sps_unsupported(
	void)
{
	struct sps_params p;
	int error;
	unsigned profile;
	static const unsigned refused[] = { 44U, 83U, 86U, 118U, 128U, 138U, 139U, 134U, 135U, 0U, 255U };

	/* A good SPS first. */
	h264sw_ps_init(&store);
	sps_defaults(&p);
	p.id = 1U;
	error = read_sps(&p);
	CHECK(error == 0);
	save_store();

	/* Profiles outside the table, refused before their other fields are read. */
	for (profile = 0U; profile < sizeof(refused) / sizeof(refused[0]); profile++) {
		sps_defaults(&p);
		p.id = 1U;
		p.profile = refused[profile];
		error = read_sps(&p);
		CHECK(error == ENOTSUP);
		check_store_unchanged();
	}

	/* Extended with neither constraint_set0 nor constraint_set1. */
	sps_defaults(&p);
	p.id = 1U;
	p.profile = 88U;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();

	/* Extended with constraint_set0 is taken; with constraint_set1 too. */
	p.constraint_flags = 0x01U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[1].profile_idc == 88U);
	p.constraint_flags = 0x02U;
	error = read_sps(&p);
	CHECK(error == 0);
	save_store();

	/* High 10 with 8 bits and 4:2:0 is taken; with 10 bits it is not. */
	sps_high(&p);
	p.id = 2U;
	p.profile = 110U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[2].profile_idc == 110U);
	save_store();
	p.bit_depth_luma_minus8 = 2U;
	p.bit_depth_chroma_minus8 = 2U;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();

	/* High 4:2:2 and High 4:4:4 Predictive with 4:2:0 8 bits are taken. */
	sps_high(&p);
	p.id = 2U;
	p.profile = 122U;
	error = read_sps(&p);
	CHECK(error == 0);
	p.profile = 244U;
	error = read_sps(&p);
	CHECK(error == 0);
	save_store();

	/* chroma_format_idc 0, 2 and 3. */
	sps_high(&p);
	p.id = 2U;
	p.chroma_format_idc = 0U;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();
	p.chroma_format_idc = 2U;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();
	p.chroma_format_idc = 3U;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();

	/* A bit depth of 9 for the luma, or for the chroma only. */
	sps_high(&p);
	p.id = 2U;
	p.bit_depth_luma_minus8 = 1U;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();
	p.bit_depth_luma_minus8 = 0U;
	p.bit_depth_chroma_minus8 = 1U;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();

	/* qpprime_y_zero_transform_bypass_flag 1 (lossless). */
	sps_high(&p);
	p.id = 2U;
	p.bypass = 1;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();

	/* frame_mbs_only_flag 0 (interlaced). */
	sps_defaults(&p);
	p.id = 2U;
	p.frame_mbs_only = 0;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();

	/* Sizes: width 1055 macroblocks is the largest taken, 1056 is not; a height the same; 139264 total. */
	sps_defaults(&p);
	p.id = 2U;
	p.width_minus1 = 1054U;
	p.height_minus1 = 131U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[2].width_mbs == 1055U);
	CHECK(store.sps[2].size_mbs == 1055U * 132U);
	save_store();
	p.width_minus1 = 1055U;
	p.height_minus1 = 0U;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();
	p.width_minus1 = 0U;
	p.height_minus1 = 1055U;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();
	p.width_minus1 = 1054U;
	p.height_minus1 = 132U;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();

	/* The largest codes cannot overflow the size arithmetic. */
	p.width_minus1 = 0xFFFFFFFEU;
	p.height_minus1 = 0xFFFFFFFEU;
	error = read_sps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();
}

/* Checks truncated SPSs: every prefix of a complex one answers EINVAL or a decision, never a crash. */
static void
test_sps_truncated(
	void)
{
	struct sps_params p;
	size_t length;
	size_t size;
	int error;

	/* A High SPS with matrix, type 1 cycle, crop and a VUI with HRD and restriction. */
	h264sw_ps_init(&store);
	sps_high(&p);
	p.matrix_present = 1;
	p.list_kind[0] = LIST_FULL;
	p.list_base[0] = 10U;
	p.list_kind[3] = LIST_DEFAULT;
	p.list_kind[6] = LIST_SHORT;
	p.list_base[6] = 20U;
	p.poc_type = 1U;
	p.cycle = 2U;
	p.offsets[0] = 1;
	p.offsets[1] = 2;
	p.crop_present = 1;
	p.crop_bottom = 4U;
	p.width_minus1 = 119U;
	p.height_minus1 = 67U;
	p.max_ref = 4U;
	p.vui.present = 1;
	p.vui.aspect_present = 1;
	p.vui.aspect_idc = 255U;
	p.vui.sar_width = 1U;
	p.vui.sar_height = 1U;
	p.vui.nal_hrd = 1;
	p.vui.cpb_cnt_minus1 = 1U;
	p.vui.restriction_present = 1;
	p.vui.max_bytes_denom = 2U;
	p.vui.max_bits_denom = 1U;
	p.vui.log2_mv_horizontal = 15U;
	p.vui.log2_mv_vertical = 15U;
	p.vui.reorder = 1U;
	p.vui.dec_buffering = 4U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 1);
	size = bitwriter_bytes(&writer);

	/* Each shorter prefix either fails with EINVAL or, when only the trailing part is gone and the VUI dropped, is kept. */
	for (length = 0U; length < size; length++) {
		h264sw_ps_init(&store);
		error = read_sps_bytes(writer.data, length);
		CHECK(error == 0 || error == EINVAL);
		if (error == 0)
			CHECK(store.sps[0].vui_present == 0);
		if (error != 0)
			CHECK(store.sps_present[0] == 0);
	}
}

/* Checks the buffer size dpb_frames for the levels and the VUI. */
static void
test_sps_dpb(
	void)
{
	struct sps_params p;
	int error;

	/* Level 3.0, 720x576: 8100 / 1620 = 5. */
	h264sw_ps_init(&store);
	sps_defaults(&p);
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 5U);

	/* Level 4.0, 1920x1088 (8160 macroblocks): 32768 / 8160 = 4. */
	sps_high(&p);
	p.width_minus1 = 119U;
	p.height_minus1 = 67U;
	p.max_ref = 4U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 4U);

	/* Level 1b by level_idc 9: 396 / 99 = 4, where level 1.1 would give 900 / 99 = 9. */
	sps_defaults(&p);
	p.width_minus1 = 10U;
	p.height_minus1 = 8U;
	p.level = 9U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 4U);

	/* Level 1b by level_idc 11 with constraint_set3_flag in Baseline and in Main. */
	p.level = 11U;
	p.constraint_flags = 0x08U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 4U);
	p.profile = 77U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 4U);

	/* Level 1.1 without the flag: 900 / 99 = 9. */
	p.constraint_flags = 0U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 9U);

	/* In the High profile constraint_set3_flag does not make level 11 into 1b (it marks an intra profile). */
	sps_high(&p);
	p.width_minus1 = 10U;
	p.height_minus1 = 8U;
	p.level = 11U;
	p.constraint_flags = 0x08U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 9U);

	/* A level_idc that is not in Table A-1 allows 16. */
	sps_defaults(&p);
	p.width_minus1 = 10U;
	p.height_minus1 = 8U;
	p.level = 99U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 16U);

	/* A picture too big for the level gives 0 frames by the formula: raised to max_num_ref_frames, or to 1. */
	sps_high(&p);
	p.width_minus1 = 119U;
	p.height_minus1 = 67U;
	p.level = 10U;
	p.max_ref = 3U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 3U);
	p.max_ref = 0U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 1U);

	/* A tiny picture at a high level is capped at 16: 696320 / 1 is far above. */
	sps_high(&p);
	p.width_minus1 = 0U;
	p.height_minus1 = 0U;
	p.level = 62U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].dpb_frames == 16U);

	/* max_dec_frame_buffering 0 in the VUI gives 1, since a picture needs a buffer. */
	sps_defaults(&p);
	p.max_ref = 0U;
	p.vui.present = 1;
	p.vui.restriction_present = 1;
	p.vui.dec_buffering = 0U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].vui_present == 1);
	CHECK(store.sps[0].dpb_frames == 1U);
}

/* Checks the generation counter: unchanged for an identical set, moved on for a different one. */
static void
test_sps_generation(
	void)
{
	struct sps_params p;
	int error;

	/* The first set of an id does not move the generation. */
	h264sw_ps_init(&store);
	sps_defaults(&p);
	p.id = 5U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps_generation[5] == 0U);

	/* The same set again leaves it. */
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps_generation[5] == 0U);

	/* A different width replaces the set and moves the generation. */
	p.width_minus1 = 21U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps_generation[5] == 1U);
	CHECK(store.sps[5].width_mbs == 22U);

	/* Identical again: no move; a different level: one more. */
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps_generation[5] == 1U);
	p.level = 31U;
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps_generation[5] == 2U);

	/* Another id is independent. */
	CHECK(store.sps_generation[4] == 0U);
	CHECK(store.sps_present[4] == 0);

	/* An init empties everything. */
	h264sw_ps_init(&store);
	CHECK(store.sps_present[5] == 0);
	CHECK(store.sps_generation[5] == 0U);
}

/* Checks the scaling lists of an SPS: every kind, and rule A for the lists not sent. */
static void
test_sps_scaling(
	void)
{
	struct sps_params p;
	const struct h264sw_sps *s;
	unsigned index;
	int error;

	/* Baseline: no matrix, so the lists stay flat 16 in the set. */
	h264sw_ps_init(&store);
	sps_defaults(&p);
	error = read_sps(&p);
	CHECK(error == 0);
	CHECK(store.sps[0].scaling_matrix_present == 0);
	for (index = 0U; index < 16U; index++)
		CHECK(store.sps[0].scaling4x4[0][index] == 16U);
	for (index = 0U; index < 64U; index++)
		CHECK(store.sps[0].scaling8x8[1][index] == 16U);

	/*
	 * High with a matrix: list 0 sent, 1 and 2 absent (rule A: copy the
	 * one before), 3 absent (Default_4x4_Inter), 4 sent short, 5 absent
	 * (copy 4), 6 asks for the default, 7 absent (Default_8x8_Inter).
	 */
	sps_high(&p);
	p.matrix_present = 1;
	p.list_kind[0] = LIST_FULL;
	p.list_base[0] = 10U;
	p.list_kind[4] = LIST_SHORT;
	p.list_base[4] = 30U;
	p.list_kind[6] = LIST_DEFAULT;
	error = read_sps(&p);
	CHECK(error == 0);
	s = &store.sps[0];
	CHECK(s->scaling_matrix_present == 1);
	CHECK(s->scaling_state[0] == H264SW_LIST_EXPLICIT);
	CHECK(s->scaling_state[1] == H264SW_LIST_ABSENT);
	CHECK(s->scaling_state[2] == H264SW_LIST_ABSENT);
	CHECK(s->scaling_state[3] == H264SW_LIST_ABSENT);
	CHECK(s->scaling_state[4] == H264SW_LIST_EXPLICIT);
	CHECK(s->scaling_state[5] == H264SW_LIST_ABSENT);
	CHECK(s->scaling_state[6] == H264SW_LIST_DEFAULT);
	CHECK(s->scaling_state[7] == H264SW_LIST_ABSENT);
	for (index = 0U; index < 16U; index++) {
		CHECK(s->scaling4x4[0][index] == list_value(LIST_FULL, 10U, index));
		CHECK(s->scaling4x4[1][index] == list_value(LIST_FULL, 10U, index));
		CHECK(s->scaling4x4[2][index] == list_value(LIST_FULL, 10U, index));
		CHECK(s->scaling4x4[3][index] == h264sw_default4x4[1][index]);
		CHECK(s->scaling4x4[4][index] == list_value(LIST_SHORT, 30U, index));
		CHECK(s->scaling4x4[5][index] == list_value(LIST_SHORT, 30U, index));
	}
	for (index = 0U; index < 64U; index++) {
		CHECK(s->scaling8x8[0][index] == h264sw_default8x8[0][index]);
		CHECK(s->scaling8x8[1][index] == h264sw_default8x8[1][index]);
	}

	/* The short list's repeat of its last sent value, written out: base + 3 from entry 3 on. */
	CHECK(s->scaling4x4[4][0] == 30U);
	CHECK(s->scaling4x4[4][3] == 33U);
	CHECK(s->scaling4x4[4][4] == 33U);
	CHECK(s->scaling4x4[4][15] == 33U);

	/* With the whole matrix absent rule A gives the defaults for list 0 and 3, and copies for the rest. */
	sps_high(&p);
	p.matrix_present = 1;
	error = read_sps(&p);
	CHECK(error == 0);
	s = &store.sps[0];
	for (index = 0U; index < 16U; index++) {
		CHECK(s->scaling4x4[0][index] == h264sw_default4x4[0][index]);
		CHECK(s->scaling4x4[2][index] == h264sw_default4x4[0][index]);
		CHECK(s->scaling4x4[3][index] == h264sw_default4x4[1][index]);
		CHECK(s->scaling4x4[5][index] == h264sw_default4x4[1][index]);
	}

	/* A list of 8x8 entries sent in full: the values of the wrap pattern at 64 entries. */
	sps_high(&p);
	p.matrix_present = 1;
	p.list_kind[7] = LIST_FULL;
	p.list_base[7] = 90U;
	error = read_sps(&p);
	CHECK(error == 0);
	for (index = 0U; index < 64U; index++)
		CHECK(store.sps[0].scaling8x8[1][index] == list_value(LIST_FULL, 90U, index));
	CHECK(store.sps[0].scaling_state[7] == H264SW_LIST_EXPLICIT);
}

/* Checks a delta_scale outside -128..127 and the wrap of nextScale modulo 256. */
static void
test_sps_scaling_delta(
	void)
{
	struct bitwriter *w;
	size_t start;
	unsigned index;
	int error;
	struct sps_params p;

	/* Builds a High SPS by hand whose list 0 has a first delta of +127 and then a wrapping delta. */
	h264sw_ps_init(&store);
	sps_high(&p);
	w = &writer;
	start = write_sps(w, &p);
	CHECK(start != 0U);
	bitwriter_init(w);
	bitwriter_u(w, 100U, 8U);
	bitwriter_u(w, 0U, 8U);
	bitwriter_u(w, 40U, 8U);
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, 1U);
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, 0U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_u(w, 1U, 1U);

	/* List 0: lastScale 8, delta 127 gives 135, then delta -128 gives 7, then 14 more deltas of 0 give 7. */
	bitwriter_u(w, 1U, 1U);
	bitwriter_se(w, 127);
	bitwriter_se(w, -128);
	for (index = 2U; index < 16U; index++)
		bitwriter_se(w, 0);

	/* Lists 1 to 7 are absent. */
	for (index = 1U; index < 8U; index++)
		bitwriter_u(w, 0U, 1U);

	/* The rest of the SPS: type 2, one reference frame, 16x16, frame only, no crop, no VUI. */
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, 2U);
	bitwriter_ue(w, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, 0U);
	bitwriter_u(w, 1U, 1U);
	bitwriter_u(w, 1U, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_trailing(w);
	error = read_sps_bytes(w->data, bitwriter_bytes(w));
	CHECK(error == 0);
	CHECK(store.sps[0].scaling4x4[0][0] == 135U);
	CHECK(store.sps[0].scaling4x4[0][1] == 7U);
	CHECK(store.sps[0].scaling4x4[0][15] == 7U);

	/* A delta of +128 is out of range. */
	save_store();
	bitwriter_init(w);
	bitwriter_u(w, 100U, 8U);
	bitwriter_u(w, 0U, 8U);
	bitwriter_u(w, 40U, 8U);
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, 1U);
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, 0U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_u(w, 1U, 1U);
	bitwriter_u(w, 1U, 1U);
	bitwriter_se(w, 128);
	for (index = 1U; index < 16U; index++)
		bitwriter_se(w, 0);
	bitwriter_trailing(w);
	error = read_sps_bytes(w->data, bitwriter_bytes(w));
	CHECK(error == EINVAL);
	check_store_unchanged();
}

/* Checks the picture parameter set: every field, with and without the High profile extension. */
static void
test_pps(
	void)
{
	struct pps_params p;
	const struct h264sw_pps *s;
	int error;

	/* A basic CAVLC set: id 7 naming SPS 2, 4 and 2 default references, weighted prediction, qp 30. */
	h264sw_ps_init(&store);
	pps_defaults(&p);
	p.id = 7U;
	p.sps_id = 2U;
	p.cabac = 0;
	p.bottom_poc = 1;
	p.ref0_minus1 = 3U;
	p.ref1_minus1 = 1U;
	p.weighted_pred = 1;
	p.bipred = 2U;
	p.qp_minus26 = 4;
	p.qs_minus26 = -3;
	p.chroma_offset = -5;
	p.deblocking = 1;
	p.constrained = 1;
	p.redundant = 0;
	error = read_pps(&p);
	CHECK(error == 0);
	CHECK(store.pps_present[7] == 1);
	CHECK(store.pps_present[0] == 0);
	s = &store.pps[7];
	CHECK(s->id == 7U);
	CHECK(s->sps_id == 2U);
	CHECK(s->cabac == 0);
	CHECK(s->bottom_field_poc_present == 1);
	CHECK(s->num_ref_idx_default[0] == 4U);
	CHECK(s->num_ref_idx_default[1] == 2U);
	CHECK(s->weighted_pred == 1);
	CHECK(s->weighted_bipred_idc == 2U);
	CHECK(s->pic_init_qp == 30);
	CHECK(s->chroma_qp_offset == -5);
	CHECK(s->deblocking_control_present == 1);
	CHECK(s->constrained_intra_pred == 1);
	CHECK(s->redundant_pic_cnt_present == 0);

	/* Without the extension: no 8x8 transform, no matrix, the Cr offset equals the Cb one. */
	CHECK(s->transform_8x8_mode == 0);
	CHECK(s->scaling_matrix_present == 0);
	CHECK(s->second_chroma_qp_offset == -5);

	/* With the extension: transform_8x8_mode and a Cr offset of its own. */
	pps_defaults(&p);
	p.id = 255U;
	p.sps_id = 31U;
	p.cabac = 1;
	p.qp_minus26 = -26;
	p.chroma_offset = 12;
	p.redundant = 1;
	p.extension = 1;
	p.transform_8x8 = 1;
	p.second_offset = -12;
	error = read_pps(&p);
	CHECK(error == 0);
	s = &store.pps[255];
	CHECK(s->cabac == 1);
	CHECK(s->pic_init_qp == 0);
	CHECK(s->chroma_qp_offset == 12);
	CHECK(s->redundant_pic_cnt_present == 1);
	CHECK(s->transform_8x8_mode == 1);
	CHECK(s->scaling_matrix_present == 0);
	CHECK(s->second_chroma_qp_offset == -12);
	CHECK(s->sps_id == 31U);

	/* The extension with transform_8x8_mode 0, and the largest quantization parameter 51 and list lengths 32. */
	pps_defaults(&p);
	p.id = 1U;
	p.ref0_minus1 = 31U;
	p.ref1_minus1 = 31U;
	p.qp_minus26 = 25;
	p.extension = 1;
	p.second_offset = 4;
	error = read_pps(&p);
	CHECK(error == 0);
	s = &store.pps[1];
	CHECK(s->transform_8x8_mode == 0);
	CHECK(s->second_chroma_qp_offset == 4);
	CHECK(s->pic_init_qp == 51);
	CHECK(s->num_ref_idx_default[0] == 32U);
	CHECK(s->num_ref_idx_default[1] == 32U);

	/* A second read of the same id replaces the set. */
	p.qp_minus26 = -1;
	error = read_pps(&p);
	CHECK(error == 0);
	CHECK(store.pps[1].pic_init_qp == 25);
}

/* Checks the PPS errors: EINVAL for ranges and damage, ENOTSUP for slice groups, store untouched. */
static void
test_pps_errors(
	void)
{
	struct pps_params p;
	struct bitwriter *w;
	size_t length;
	size_t size;
	int error;

	/* A good set first. */
	h264sw_ps_init(&store);
	pps_defaults(&p);
	p.id = 9U;
	error = read_pps(&p);
	CHECK(error == 0);
	save_store();

	/* Slice groups: ENOTSUP for 1 extra group and for a huge count. */
	pps_defaults(&p);
	p.id = 9U;
	p.slice_groups_minus1 = 1U;
	error = read_pps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();
	p.slice_groups_minus1 = 7U;
	error = read_pps(&p);
	CHECK(error == ENOTSUP);
	check_store_unchanged();

	/* weighted_bipred_idc 3. */
	pps_defaults(&p);
	p.id = 9U;
	p.bipred = 3U;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* pic_init_qp_minus26 of -27 and 26. */
	pps_defaults(&p);
	p.id = 9U;
	p.qp_minus26 = -27;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();
	p.qp_minus26 = 26;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* pic_init_qs_minus26 of 26. */
	p.qp_minus26 = 0;
	p.qs_minus26 = 26;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* num_ref_idx_lX_default_active_minus1 of 32 for either list. */
	pps_defaults(&p);
	p.id = 9U;
	p.ref0_minus1 = 32U;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();
	p.ref0_minus1 = 0U;
	p.ref1_minus1 = 32U;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* chroma_qp_index_offset of 13 and second_chroma_qp_index_offset of -13. */
	pps_defaults(&p);
	p.id = 9U;
	p.chroma_offset = 13;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();
	p.chroma_offset = 0;
	p.extension = 1;
	p.second_offset = -13;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* Ids: PPS 256 and SPS 32. */
	pps_defaults(&p);
	p.id = 256U;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();
	p.id = 9U;
	p.sps_id = 32U;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* Missing trailing bits: a set whose last byte is not a stop bit followed by zeros. */
	pps_defaults(&p);
	p.id = 9U;
	write_pps(&writer, &p);
	w = &writer;
	w->data[bitwriter_bytes(w) - 1U] = 0x00U;
	error = read_pps_bytes(w->data, bitwriter_bytes(w));
	CHECK(error == EINVAL);
	check_store_unchanged();

	/* Truncated at every length. */
	write_pps(&writer, &p);
	size = bitwriter_bytes(&writer);
	for (length = 0U; length < size; length++) {
		error = read_pps_bytes(writer.data, length);
		CHECK(error == EINVAL);
		check_store_unchanged();
	}

	/* A matrix with more flags than the data holds, and a huge ue. */
	pps_defaults(&p);
	p.id = 9U;
	p.ref0_minus1 = 0xFFFFFFFEU;
	error = read_pps(&p);
	CHECK(error == EINVAL);
	check_store_unchanged();
}

/* Writes the SPS lists of the scaling tests: a High SPS that sends the given kinds. */
static void
read_sps_lists(
	int matrix_present,
	const unsigned *kind,
	const unsigned *base)
{
	struct sps_params p;
	unsigned index;
	int error;

	sps_high(&p);
	p.matrix_present = matrix_present;
	for (index = 0U; index < 8U; index++) {
		p.list_kind[index] = kind[index];
		p.list_base[index] = base[index];
	}
	error = read_sps(&p);
	CHECK(error == 0);
}

/* Reads a PPS with the given matrix and the 8x8 transform on; returns nothing, the store holds it. */
static void
read_pps_lists(
	int matrix_present,
	int transform_8x8,
	const unsigned *kind,
	const unsigned *base)
{
	struct pps_params p;
	unsigned index;
	int error;

	pps_defaults(&p);
	p.id = 0U;
	p.extension = 1;
	p.transform_8x8 = transform_8x8;
	p.matrix_present = matrix_present;
	for (index = 0U; index < 8U; index++) {
		p.list_kind[index] = kind[index];
		p.list_base[index] = base[index];
	}
	error = read_pps(&p);
	CHECK(error == 0);
}

/* Compares a resolved list with the expected values of a kind and base (or of a table). */
static int
list_equals_pattern(
	const uint8_t *list,
	unsigned size,
	unsigned kind,
	unsigned base)
{
	unsigned index;

	for (index = 0U; index < size; index++) {
		if (list[index] != list_value(kind, base, index))
			return 0;
	}
	return 1;
}

/* Compares a resolved list with a table list. */
static int
list_equals_table(
	const uint8_t *list,
	const uint8_t *table,
	unsigned size)
{
	return memcmp(list, table, size) == 0;
}

/* Checks the four cases of h264sw_ps_scaling and the fall-back rules A and B. */
static void
test_scaling_resolution(
	void)
{
	static unsigned none_kind[8];
	static unsigned none_base[8];
	unsigned kind[8];
	unsigned base[8];
	struct h264sw_scaling out;
	unsigned index;

	/* Case 1: neither set has a matrix: all 16. */
	h264sw_ps_init(&store);
	read_sps_lists(0, none_kind, none_base);
	read_pps_lists(0, 0, none_kind, none_base);
	memset(&out, 0, sizeof(out));
	h264sw_ps_scaling(&store.sps[0], &store.pps[0], &out);
	for (index = 0U; index < 96U; index++)
		CHECK(((const uint8_t *)out.list4x4)[index] == 16U);
	for (index = 0U; index < 128U; index++)
		CHECK(((const uint8_t *)out.list8x8)[index] == 16U);

	/* Case 2: only the SPS has a matrix: its lists, rule A already applied. */
	memset(kind, 0, sizeof(kind));
	kind[0] = LIST_FULL;
	base[0] = 10U;
	kind[3] = LIST_FULL;
	base[3] = 50U;
	kind[7] = LIST_DEFAULT;
	read_sps_lists(1, kind, base);
	read_pps_lists(0, 0, none_kind, none_base);
	memset(&out, 0, sizeof(out));
	h264sw_ps_scaling(&store.sps[0], &store.pps[0], &out);
	CHECK(list_equals_pattern(out.list4x4[0], 16U, LIST_FULL, 10U));
	CHECK(list_equals_pattern(out.list4x4[1], 16U, LIST_FULL, 10U));
	CHECK(list_equals_pattern(out.list4x4[2], 16U, LIST_FULL, 10U));
	CHECK(list_equals_pattern(out.list4x4[3], 16U, LIST_FULL, 50U));
	CHECK(list_equals_pattern(out.list4x4[5], 16U, LIST_FULL, 50U));
	CHECK(list_equals_table(out.list8x8[0], h264sw_default8x8[0], 64U));
	CHECK(list_equals_table(out.list8x8[1], h264sw_default8x8[1], 64U));

	/*
	 * Case 3: only the PPS has a matrix (rule A for its absent lists):
	 * list 0 absent gives Default_4x4_Intra, 1 sent, 2 absent copies 1,
	 * 3 asks for the default (Inter), 4 and 5 absent copy the list
	 * before, 6 sent, 7 absent gives Default_8x8_Inter.
	 */
	read_sps_lists(0, none_kind, none_base);
	memset(kind, 0, sizeof(kind));
	kind[1] = LIST_FULL;
	base[1] = 20U;
	kind[3] = LIST_DEFAULT;
	kind[6] = LIST_FULL;
	base[6] = 60U;
	read_pps_lists(1, 1, kind, base);
	memset(&out, 0, sizeof(out));
	h264sw_ps_scaling(&store.sps[0], &store.pps[0], &out);
	CHECK(list_equals_table(out.list4x4[0], h264sw_default4x4[0], 16U));
	CHECK(list_equals_pattern(out.list4x4[1], 16U, LIST_FULL, 20U));
	CHECK(list_equals_pattern(out.list4x4[2], 16U, LIST_FULL, 20U));
	CHECK(list_equals_table(out.list4x4[3], h264sw_default4x4[1], 16U));
	CHECK(list_equals_table(out.list4x4[4], h264sw_default4x4[1], 16U));
	CHECK(list_equals_table(out.list4x4[5], h264sw_default4x4[1], 16U));
	CHECK(list_equals_pattern(out.list8x8[0], 64U, LIST_FULL, 60U));
	CHECK(list_equals_table(out.list8x8[1], h264sw_default8x8[1], 64U));

	/*
	 * Case 4: both have a matrix (rule B for the PPS's absent lists).
	 * The SPS sends 0 (base 10), 1 (base 30), 3 (base 50), 4 (base 70),
	 * 6 (base 90) and 7 (base 100); the PPS sends only list 2 (base 120)
	 * and list 7 (base 110).  The PPS's lists 0, 3 and 6 fall back to the
	 * SPS's own 0, 3 and 6; its lists 1, 4 and 5 fall back to the list
	 * before them in the PPS (so 1 is the PPS's 0, not the SPS's 1, and 4
	 * and 5 are the PPS's 3); list 2 and list 7 are sent.
	 */
	memset(kind, 0, sizeof(kind));
	kind[0] = LIST_FULL;
	base[0] = 10U;
	kind[1] = LIST_FULL;
	base[1] = 30U;
	kind[3] = LIST_FULL;
	base[3] = 50U;
	kind[4] = LIST_FULL;
	base[4] = 70U;
	kind[6] = LIST_FULL;
	base[6] = 90U;
	kind[7] = LIST_FULL;
	base[7] = 100U;
	read_sps_lists(1, kind, base);
	memset(kind, 0, sizeof(kind));
	kind[2] = LIST_FULL;
	base[2] = 120U;
	kind[7] = LIST_FULL;
	base[7] = 110U;
	read_pps_lists(1, 1, kind, base);
	memset(&out, 0, sizeof(out));
	h264sw_ps_scaling(&store.sps[0], &store.pps[0], &out);
	CHECK(list_equals_pattern(out.list4x4[0], 16U, LIST_FULL, 10U));
	CHECK(list_equals_pattern(out.list4x4[1], 16U, LIST_FULL, 10U));
	CHECK(list_equals_pattern(out.list4x4[2], 16U, LIST_FULL, 120U));
	CHECK(list_equals_pattern(out.list4x4[3], 16U, LIST_FULL, 50U));
	CHECK(list_equals_pattern(out.list4x4[4], 16U, LIST_FULL, 50U));
	CHECK(list_equals_pattern(out.list4x4[5], 16U, LIST_FULL, 50U));
	CHECK(list_equals_pattern(out.list8x8[0], 64U, LIST_FULL, 90U));
	CHECK(list_equals_pattern(out.list8x8[1], 64U, LIST_FULL, 110U));

	/* Case 4 with the PPS asking for a default list: it is the default, not the SPS's. */
	memset(kind, 0, sizeof(kind));
	kind[0] = LIST_DEFAULT;
	kind[6] = LIST_DEFAULT;
	read_pps_lists(1, 1, kind, base);
	memset(&out, 0, sizeof(out));
	h264sw_ps_scaling(&store.sps[0], &store.pps[0], &out);
	CHECK(list_equals_table(out.list4x4[0], h264sw_default4x4[0], 16U));
	CHECK(list_equals_table(out.list4x4[1], h264sw_default4x4[0], 16U));
	CHECK(list_equals_table(out.list8x8[0], h264sw_default8x8[0], 64U));

	/* A PPS with transform_8x8_mode 0 sends only the six 4x4 lists; the 8x8 ones fall back (rule B: the SPS's). */
	memset(kind, 0, sizeof(kind));
	kind[0] = LIST_FULL;
	base[0] = 40U;
	read_pps_lists(1, 0, kind, base);
	memset(&out, 0, sizeof(out));
	h264sw_ps_scaling(&store.sps[0], &store.pps[0], &out);
	CHECK(list_equals_pattern(out.list4x4[0], 16U, LIST_FULL, 40U));
	CHECK(list_equals_pattern(out.list8x8[0], 64U, LIST_FULL, 90U));
	CHECK(store.pps[0].transform_8x8_mode == 0);
	CHECK(store.pps[0].scaling_state[6] == H264SW_LIST_ABSENT);
}

/* Reads damaged and random input at every byte of a complex SPS and PPS: no crash, only the documented errors. */
static void
test_damage(
	void)
{
	struct sps_params sp;
	struct pps_params pp;
	size_t size;
	size_t index;
	unsigned round;
	unsigned seed;
	int error;
	size_t position;
	static const uint8_t patterns[4] = { 0x00U, 0xFFU, 0x80U, 0x01U };
	static uint8_t copy[BITWRITER_BYTES];

	/* A High SPS with every optional part, as the base of the damage. */
	h264sw_ps_init(&store);
	sps_high(&sp);
	sp.matrix_present = 1;
	sp.list_kind[0] = LIST_FULL;
	sp.list_base[0] = 10U;
	sp.list_kind[6] = LIST_SHORT;
	sp.list_base[6] = 20U;
	sp.poc_type = 1U;
	sp.cycle = 3U;
	sp.offsets[0] = 1;
	sp.offsets[1] = 2;
	sp.offsets[2] = 3;
	sp.crop_present = 1;
	sp.crop_bottom = 4U;
	sp.width_minus1 = 119U;
	sp.height_minus1 = 67U;
	sp.max_ref = 4U;
	sp.vui.present = 1;
	sp.vui.aspect_present = 1;
	sp.vui.aspect_idc = 255U;
	sp.vui.sar_width = 1U;
	sp.vui.sar_height = 1U;
	sp.vui.nal_hrd = 1;
	sp.vui.vcl_hrd = 1;
	sp.vui.cpb_cnt_minus1 = 1U;
	sp.vui.timing_present = 1;
	sp.vui.units = 1U;
	sp.vui.scale = 50U;
	sp.vui.restriction_present = 1;
	sp.vui.reorder = 1U;
	sp.vui.dec_buffering = 4U;
	error = read_sps(&sp);
	CHECK(error == 0);
	size = bitwriter_bytes(&writer);
	memcpy(copy, writer.data, size);

	/* Every byte replaced by each of eight patterns. */
	for (index = 0U; index < size; index++) {
		for (round = 0U; round < 8U; round++) {
			memcpy(writer.data, copy, size);
			if (round < 4U) {
				/* The first four rounds put a fixed pattern in the byte. */
				writer.data[index] = patterns[round];
			} else {
				/* The others flip one bit of it. */
				writer.data[index] = (uint8_t)(copy[index] ^ (1U << round));
			}
			error = read_sps_bytes(writer.data, size);
			CHECK(error == 0 || error == EINVAL || error == ENOTSUP);
		}
	}

	/* Pseudo-random corruptions: a linear congruential sequence picks the bytes and values. */
	seed = 12345U;
	for (round = 0U; round < 20000U; round++) {
		memcpy(writer.data, copy, size);
		seed = seed * 1103515245U + 12345U;
		position = (seed >> 8) % size;
		seed = seed * 1103515245U + 12345U;
		writer.data[position] = (uint8_t)(seed >> 16);
		seed = seed * 1103515245U + 12345U;
		position = (seed >> 8) % size;
		seed = seed * 1103515245U + 12345U;
		writer.data[position] = (uint8_t)(seed >> 16);
		error = read_sps_bytes(writer.data, size);
		CHECK(error == 0 || error == EINVAL || error == ENOTSUP);
	}

	/* The same for a PPS with a matrix. */
	pps_defaults(&pp);
	pp.id = 3U;
	pp.extension = 1;
	pp.transform_8x8 = 1;
	pp.matrix_present = 1;
	pp.list_kind[0] = LIST_FULL;
	pp.list_base[0] = 10U;
	pp.list_kind[6] = LIST_DEFAULT;
	pp.second_offset = 2;
	error = read_pps(&pp);
	CHECK(error == 0);
	size = bitwriter_bytes(&writer);
	memcpy(copy, writer.data, size);
	for (round = 0U; round < 20000U; round++) {
		memcpy(writer.data, copy, size);
		seed = seed * 1103515245U + 12345U;
		position = (seed >> 8) % size;
		seed = seed * 1103515245U + 12345U;
		writer.data[position] = (uint8_t)(seed >> 16);
		error = read_pps_bytes(writer.data, size);
		CHECK(error == 0 || error == EINVAL || error == ENOTSUP);
	}
}

int
main(
	void)
{
	/* Runs every group of checks and prints the summary line. */
	test_sps_baseline();
	test_sps_poc();
	test_sps_poc_cycle_255();
	test_sps_crop();
	test_sps_vui();
	test_sps_vui_dropped();
	test_sps_invalid();
	test_sps_unsupported();
	test_sps_truncated();
	test_sps_dpb();
	test_sps_generation();
	test_sps_scaling();
	test_sps_scaling_delta();
	test_pps();
	test_pps_errors();
	test_scaling_resolution();
	test_damage();

	/* Succeeded when every check passed. */
	return check_finish("ps");
}
