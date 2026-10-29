/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the slice header reader of the software H.264 decoder: slice
 * headers built with the bit writer from the syntax of 7.3.3, 7.3.3.1,
 * 7.3.3.2 and 7.3.3.3 (parameter sets are read first by the parameter
 * set reader), every field and the derived values, the inferred weights
 * of 7.4.3.2, the errors for out-of-range and unsupported input, the
 * detection of the first slice of a picture (7.4.1.2.4), and the reader's
 * behaviour on truncated, damaged and random input.
 */

#include "check.h"
#include "bitwriter.h"

#include "userland/desktop/libmedia/h264sw-slice.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most entries a test writes for one list of modifications or weights, and operations of a picture. */
#define TEST_MODS 40U
#define TEST_MMCOS 70U

/* The size of the pictures the tests use: 4 by 3 macroblocks. */
#define TEST_WIDTH_MBS 4U
#define TEST_HEIGHT_MBS 3U
#define TEST_SIZE_MBS 12U

/* The largest codeNum a ue(v) of this decoder reads (9.1). */
#define TEST_BIG_UE 0xFFFFFFFEU

/*
 * What the parameter sets of a test say: the fields of the SPS and the PPS
 * that change which fields a slice header has.
 */
struct config {
	unsigned profile;
	unsigned log2_frame_minus4;
	unsigned poc_type;
	unsigned log2_poc_minus4;
	int always_zero;
	unsigned max_ref;
	int cabac;
	int bottom_poc;
	unsigned ref0_minus1;
	unsigned ref1_minus1;
	int weighted_pred;
	unsigned bipred;
	int32_t qp_minus26;
	int deblocking;
	int redundant;
};

/* One memory management control operation a test writes. */
struct test_mmco {
	uint32_t operation;
	uint32_t difference;
	uint32_t long_term_pic_num;
	uint32_t long_term_frame_idx;
	uint32_t max_plus1;
};

/* The fields of the slice header a test wants, in the order of the syntax. */
struct params {
	unsigned nal_type;
	unsigned ref_idc;
	uint32_t first_mb;
	uint32_t slice_type;
	uint32_t pps_id;
	uint32_t frame_num;
	uint32_t idr_pic_id;
	uint32_t poc_lsb;
	int32_t delta_bottom;
	int32_t delta0;
	int32_t delta1;
	uint32_t redundant;
	int direct;
	int override;
	uint32_t n0_minus1;
	uint32_t n1_minus1;
	int mod_flag[2];
	unsigned mod_count[2];
	uint32_t mod_idc[2][TEST_MODS];
	uint32_t mod_value[2][TEST_MODS];
	uint32_t luma_denom;
	uint32_t chroma_denom;
	int luma_flag[2][H264SW_MAX_LIST];
	int32_t luma_weight[2][H264SW_MAX_LIST];
	int32_t luma_offset[2][H264SW_MAX_LIST];
	int chroma_flag[2][H264SW_MAX_LIST];
	int32_t chroma_weight[2][H264SW_MAX_LIST][2];
	int32_t chroma_offset[2][H264SW_MAX_LIST][2];
	int no_output;
	int long_term;
	int adaptive;
	unsigned mmco_count;
	struct test_mmco mmco[TEST_MMCOS];
	uint32_t cabac_init;
	int32_t qp_delta;
	uint32_t deblock_idc;
	int32_t alpha;
	int32_t beta;
};

/* The parameter sets the tests read into. */
static struct h264sw_ps_store store;

/* The RBSP being built, and the bit position where its slice header ended. */
static struct bitwriter writer;
static size_t header_bits;

/* The configuration the last install used. */
static struct config current_config;

/* Gives the configuration of a plain stream: order count type 2, CAVLC, no optional PPS fields. */
static void
config_defaults(
	struct config *c)
{
	memset(c, 0, sizeof(*c));
	c->profile = 77U;
	c->poc_type = 2U;
	c->max_ref = 4U;
	c->qp_minus26 = 0;
}

/* Gives the parameters of an IDR I slice with the given reference indication. */
static void
params_idr(
	struct params *p)
{
	memset(p, 0, sizeof(*p));
	p->nal_type = H264SW_NAL_IDR;
	p->ref_idc = 3U;
	p->slice_type = 2U;
	p->frame_num = 0U;
}

/* Gives the parameters of a non-IDR slice of the type, a reference picture. */
static void
params_slice(
	struct params *p,
	uint32_t slice_type)
{
	memset(p, 0, sizeof(*p));
	p->nal_type = H264SW_NAL_SLICE;
	p->ref_idc = 2U;
	p->slice_type = slice_type;
	p->frame_num = 3U;
}

/* Writes the SPS of the configuration (7.3.2.1.1) with id 0, a picture of 4 by 3 macroblocks and no VUI. */
static void
write_sps(
	struct bitwriter *w,
	const struct config *c)
{
	bitwriter_init(w);
	bitwriter_u(w, c->profile, 8U);
	bitwriter_u(w, 0U, 8U);
	bitwriter_u(w, 30U, 8U);
	bitwriter_ue(w, 0U);

	/* The frame number length and the order count parameters. */
	bitwriter_ue(w, c->log2_frame_minus4);
	bitwriter_ue(w, c->poc_type);
	if (c->poc_type == 0U) {
		bitwriter_ue(w, c->log2_poc_minus4);
	}
	if (c->poc_type == 1U) {
		bitwriter_u(w, (unsigned)c->always_zero, 1U);
		bitwriter_se(w, -2);
		bitwriter_se(w, 1);
		bitwriter_ue(w, 1U);
		bitwriter_se(w, 2);
	}

	/* The reference frames, the picture size, frame coding only, and no cropping or VUI. */
	bitwriter_ue(w, c->max_ref);
	bitwriter_u(w, 0U, 1U);
	bitwriter_ue(w, TEST_WIDTH_MBS - 1U);
	bitwriter_ue(w, TEST_HEIGHT_MBS - 1U);
	bitwriter_u(w, 1U, 1U);
	bitwriter_u(w, 1U, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_trailing(w);
}

/* Writes the PPS of the configuration (7.3.2.2) with id 0 naming SPS 0. */
static void
write_pps(
	struct bitwriter *w,
	const struct config *c,
	uint32_t pps_id)
{
	bitwriter_init(w);
	bitwriter_ue(w, pps_id);
	bitwriter_ue(w, 0U);
	bitwriter_u(w, (unsigned)c->cabac, 1U);
	bitwriter_u(w, (unsigned)c->bottom_poc, 1U);
	bitwriter_ue(w, 0U);
	bitwriter_ue(w, c->ref0_minus1);
	bitwriter_ue(w, c->ref1_minus1);
	bitwriter_u(w, (unsigned)c->weighted_pred, 1U);
	bitwriter_u(w, c->bipred, 2U);
	bitwriter_se(w, c->qp_minus26);
	bitwriter_se(w, 0);
	bitwriter_se(w, 0);
	bitwriter_u(w, (unsigned)c->deblocking, 1U);
	bitwriter_u(w, 0U, 1U);
	bitwriter_u(w, (unsigned)c->redundant, 1U);
	bitwriter_trailing(w);
}

/* Reads the parameter sets of the configuration into the store, as PPS ids 0 and 1. */
static void
install(
	const struct config *c)
{
	struct h264sw_bits bits;
	struct bitwriter *w;
	int error;

	/* A fresh store, so a test never sees the sets of the one before. */
	h264sw_ps_init(&store);
	current_config = *c;
	w = (struct bitwriter *)malloc(sizeof(*w));
	CHECK(w != NULL);
	if (w == NULL)
		return;

	/* Reads the SPS. */
	write_sps(w, c);
	h264sw_bits_init(&bits, w->data, bitwriter_bytes(w));
	error = h264sw_ps_read_sps(&store, &bits);
	CHECK(error == 0);

	/* Reads the PPS under the ids 0 and 1, which are the same set. */
	write_pps(w, c, 0U);
	h264sw_bits_init(&bits, w->data, bitwriter_bytes(w));
	error = h264sw_ps_read_pps(&store, &bits);
	CHECK(error == 0);
	write_pps(w, c, 1U);
	h264sw_bits_init(&bits, w->data, bitwriter_bytes(w));
	error = h264sw_ps_read_pps(&store, &bits);
	CHECK(error == 0);
	free(w);
}

/* Gives the list length the slice has: the override, or the PPS default. */
static unsigned
active_count(
	const struct config *c,
	const struct params *p,
	unsigned list)
{
	uint32_t minus1;

	/* The override replaces the default; a huge value is cut to the arrays a test has. */
	if (list == 0U) {
		minus1 = c->ref0_minus1;
		if (p->override != 0)
			minus1 = p->n0_minus1;
	} else {
		minus1 = c->ref1_minus1;
		if (p->override != 0)
			minus1 = p->n1_minus1;
	}
	if (minus1 >= H264SW_MAX_LIST)
		return H264SW_MAX_LIST;
	return minus1 + 1U;
}

/* Writes the modifications of one list (7.3.3.1). */
static void
write_modifications(
	struct bitwriter *w,
	const struct params *p,
	unsigned list)
{
	unsigned index;

	/* The flag, then the operations and the ending idc 3. */
	bitwriter_u(w, (unsigned)p->mod_flag[list], 1U);
	if (p->mod_flag[list] == 0)
		return;
	for (index = 0U; index < p->mod_count[list]; index++) {
		bitwriter_ue(w, p->mod_idc[list][index]);
		bitwriter_ue(w, p->mod_value[list][index]);
	}
	bitwriter_ue(w, 3U);
}

/* Writes the weights of one list (7.3.3.2) for count reference indices. */
static void
write_weights(
	struct bitwriter *w,
	const struct params *p,
	unsigned list,
	unsigned count)
{
	unsigned index;
	unsigned plane;

	/* Each index has a luma flag and values, then a chroma flag and values. */
	for (index = 0U; index < count; index++) {
		bitwriter_u(w, (unsigned)p->luma_flag[list][index], 1U);
		if (p->luma_flag[list][index] != 0) {
			bitwriter_se(w, p->luma_weight[list][index]);
			bitwriter_se(w, p->luma_offset[list][index]);
		}
		bitwriter_u(w, (unsigned)p->chroma_flag[list][index], 1U);
		if (p->chroma_flag[list][index] != 0) {
			for (plane = 0U; plane < 2U; plane++) {
				bitwriter_se(w, p->chroma_weight[list][index][plane]);
				bitwriter_se(w, p->chroma_offset[list][index][plane]);
			}
		}
	}
}

/* Writes dec_ref_pic_marking( ) (7.3.3.3). */
static void
write_marking(
	struct bitwriter *w,
	const struct params *p)
{
	unsigned index;
	const struct test_mmco *m;

	/* An IDR picture sends two flags. */
	if (p->nal_type == H264SW_NAL_IDR) {
		bitwriter_u(w, (unsigned)p->no_output, 1U);
		bitwriter_u(w, (unsigned)p->long_term, 1U);
		return;
	}

	/* Another picture sends the mode, and in the adaptive mode the operations and the ending 0. */
	bitwriter_u(w, (unsigned)p->adaptive, 1U);
	if (p->adaptive == 0)
		return;
	for (index = 0U; index < p->mmco_count; index++) {
		m = &p->mmco[index];
		bitwriter_ue(w, m->operation);
		if (m->operation == 1U || m->operation == 3U)
			bitwriter_ue(w, m->difference);
		if (m->operation == 2U)
			bitwriter_ue(w, m->long_term_pic_num);
		if (m->operation == 3U || m->operation == 6U)
			bitwriter_ue(w, m->long_term_frame_idx);
		if (m->operation == 4U)
			bitwriter_ue(w, m->max_plus1);
	}
	bitwriter_ue(w, 0U);
}

/* Writes slice_header( ) (7.3.3) for the configuration and the parameters, in the order of the syntax. */
static void
write_header(
	struct bitwriter *w,
	const struct config *c,
	const struct params *p)
{
	uint32_t type;
	int idr;
	unsigned count;

	idr = (p->nal_type == H264SW_NAL_IDR);
	type = p->slice_type % 5U;

	/* The first macroblock, the type, the PPS and the frame number. */
	bitwriter_ue(w, p->first_mb);
	bitwriter_ue(w, p->slice_type);
	bitwriter_ue(w, p->pps_id);
	bitwriter_u(w, p->frame_num, c->log2_frame_minus4 + 4U);
	if (idr)
		bitwriter_ue(w, p->idr_pic_id);

	/* The order count fields of the two types that have them. */
	if (c->poc_type == 0U) {
		bitwriter_u(w, p->poc_lsb, c->log2_poc_minus4 + 4U);
		if (c->bottom_poc != 0)
			bitwriter_se(w, p->delta_bottom);
	}
	if (c->poc_type == 1U && c->always_zero == 0) {
		bitwriter_se(w, p->delta0);
		if (c->bottom_poc != 0)
			bitwriter_se(w, p->delta1);
	}
	if (c->redundant != 0)
		bitwriter_ue(w, p->redundant);
	if (type == 1U)
		bitwriter_u(w, (unsigned)p->direct, 1U);

	/* The reference list lengths. */
	if (type == 0U || type == 1U) {
		bitwriter_u(w, (unsigned)p->override, 1U);
		if (p->override != 0) {
			bitwriter_ue(w, p->n0_minus1);
			if (type == 1U)
				bitwriter_ue(w, p->n1_minus1);
		}
	}

	/* The list modifications, the weight table and the marking. */
	if (type != 2U && type != 4U)
		write_modifications(w, p, 0U);
	if (type == 1U)
		write_modifications(w, p, 1U);
	if ((c->weighted_pred != 0 && (type == 0U || type == 3U)) ||
	    (c->bipred == 1U && type == 1U)) {
		bitwriter_ue(w, p->luma_denom);
		bitwriter_ue(w, p->chroma_denom);
		count = active_count(c, p, 0U);
		write_weights(w, p, 0U, count);
		if (type == 1U) {
			count = active_count(c, p, 1U);
			write_weights(w, p, 1U, count);
		}
	}
	if (p->ref_idc != 0U)
		write_marking(w, p);

	/* The entropy coder's context choice, the quantizer, and the deblocking filter. */
	if (c->cabac != 0 && type != 2U && type != 4U)
		bitwriter_ue(w, p->cabac_init);
	bitwriter_se(w, p->qp_delta);
	if (c->deblocking != 0) {
		bitwriter_ue(w, p->deblock_idc);
		if (p->deblock_idc != 1U) {
			bitwriter_se(w, p->alpha);
			bitwriter_se(w, p->beta);
		}
	}
}

/* Builds the RBSP of a slice (the header, then three bits of slice data and the trailing bits) into the global writer. */
static void
build(
	const struct config *c,
	const struct params *p)
{
	bitwriter_init(&writer);
	write_header(&writer, c, p);
	header_bits = writer.bits;
	bitwriter_u(&writer, 5U, 3U);
	bitwriter_trailing(&writer);
}

/* Reads the global writer's RBSP as a NAL unit of the type, into the header. */
static int
read_rbsp(
	unsigned type,
	unsigned ref_idc,
	const uint8_t *data,
	size_t size,
	struct h264sw_slice_header *header)
{
	struct h264sw_bits bits;
	struct h264sw_nal nal;

	nal.type = type;
	nal.ref_idc = ref_idc;
	nal.rbsp = data;
	nal.rbsp_size = size;
	h264sw_bits_init(&bits, data, size);
	return h264sw_slice_read(&bits, &nal, &store, header);
}

/* Builds a slice and reads it, filling the header. */
static int
read_slice(
	const struct params *p,
	struct h264sw_slice_header *header)
{
	build(&current_config, p);
	return read_rbsp(p->nal_type, p->ref_idc, writer.data, bitwriter_bytes(&writer), header);
}

/* Reads a slice with the given parameters and expects an error. */
static void
expect_error(
	const struct params *p,
	int expected)
{
	struct h264sw_slice_header header;
	int error;

	error = read_slice(p, &header);
	CHECK(error == expected);
}

/* Reads a slice that must succeed, and checks that it stopped where the header ended. */
static void
expect_ok(
	const struct params *p,
	struct h264sw_slice_header *header)
{
	int error;

	error = read_slice(p, header);
	CHECK(error == 0);
	CHECK(header->data_position == header_bits);
}

/* The hand-coded bits of an IDR I slice: the bytes and the length are worked out from 7.3.3 and 9.1 below. */
static void
test_hand_coded(void)
{
	struct config c;
	struct h264sw_slice_header header;
	struct h264sw_bits bits;
	struct h264sw_nal nal;
	/*
	 * first_mb_in_slice 0: ue 1; slice_type 7: ue 0001000; pic_parameter_set_id 0: ue 1;
	 * frame_num: u(4) 0000; idr_pic_id 0: ue 1; no_output_of_prior_pics_flag 0;
	 * long_term_reference_flag 0; slice_qp_delta 0: se 1 (17 bits); then rbsp_stop_one_bit 1.
	 */
	static const uint8_t rbsp[3] = { 0x88U, 0x84U, 0xC0U };
	int error;

	/* Order count type 2, frame_num of 4 bits, PPS initial QP 26, no deblocking controls. */
	config_defaults(&c);
	install(&c);
	nal.type = H264SW_NAL_IDR;
	nal.ref_idc = 3U;
	nal.rbsp = rbsp;
	nal.rbsp_size = sizeof(rbsp);
	h264sw_bits_init(&bits, rbsp, sizeof(rbsp));
	error = h264sw_slice_read(&bits, &nal, &store, &header);
	CHECK(error == 0);
	CHECK(header.nal_type == 5U);
	CHECK(header.nal_ref_idc == 3U);
	CHECK(header.idr == 1);
	CHECK(header.first_mb == 0U);
	CHECK(header.slice_type == H264SW_SLICE_I);
	CHECK(header.slice_type_fixed == 1);
	CHECK(header.pps_id == 0U);
	CHECK(header.frame_num == 0U);
	CHECK(header.idr_pic_id == 0U);
	CHECK(header.no_output_of_prior_pics == 0);
	CHECK(header.long_term_reference == 0);
	CHECK(header.num_ref_idx_active[0] == 0U);
	CHECK(header.num_ref_idx_active[1] == 0U);
	CHECK(header.weighted == 0);
	CHECK(header.mmco_count == 0U);
	CHECK(header.has_mmco5 == 0);
	CHECK(header.qp == 26);
	CHECK(header.disable_deblocking == 0U);
	CHECK(header.filter_offset_a == 0);
	CHECK(header.filter_offset_b == 0);
	CHECK(header.data_position == 17U);
	CHECK(bits.position == 17U);
}

/* An IDR slice with its two marking flags, and a non-IDR I slice with order count type 0 and deblocking offsets. */
static void
test_intra(void)
{
	struct config c;
	struct params p;
	struct h264sw_slice_header header;

	/* The IDR flags and the identifier of the picture. */
	config_defaults(&c);
	c.log2_frame_minus4 = 4U;
	install(&c);
	params_idr(&p);
	p.first_mb = 11U;
	p.slice_type = 7U;
	p.idr_pic_id = 65535U;
	p.no_output = 1;
	p.long_term = 1;
	p.qp_delta = -3;
	expect_ok(&p, &header);
	CHECK(header.first_mb == 11U);
	CHECK(header.slice_type == H264SW_SLICE_I);
	CHECK(header.slice_type_fixed == 1);
	CHECK(header.idr_pic_id == 65535U);
	CHECK(header.no_output_of_prior_pics == 1);
	CHECK(header.long_term_reference == 1);
	CHECK(header.qp == 23);

	/* Order count type 0 with the bottom field difference; the PPS initial QP is 24; the filter offsets double. */
	config_defaults(&c);
	c.log2_frame_minus4 = 1U;
	c.poc_type = 0U;
	c.log2_poc_minus4 = 4U;
	c.bottom_poc = 1;
	c.deblocking = 1;
	c.qp_minus26 = -2;
	install(&c);
	params_slice(&p, 2U);
	p.frame_num = 9U;
	p.poc_lsb = 200U;
	p.delta_bottom = -3;
	p.qp_delta = 5;
	p.deblock_idc = 0U;
	p.alpha = -3;
	p.beta = 6;
	expect_ok(&p, &header);
	CHECK(header.idr == 0);
	CHECK(header.nal_type == 1U);
	CHECK(header.nal_ref_idc == 2U);
	CHECK(header.slice_type == H264SW_SLICE_I);
	CHECK(header.slice_type_fixed == 0);
	CHECK(header.frame_num == 9U);
	CHECK(header.poc_lsb == 200U);
	CHECK(header.delta_poc_bottom == -3);
	CHECK(header.delta_poc[0] == 0);
	CHECK(header.delta_poc[1] == 0);
	CHECK(header.adaptive_marking == 0);
	CHECK(header.mmco_count == 0U);
	CHECK(header.qp == 29);
	CHECK(header.disable_deblocking == 0U);
	CHECK(header.filter_offset_a == -6);
	CHECK(header.filter_offset_b == 12);
	CHECK(header.num_ref_idx_active[0] == 0U);
	CHECK(header.num_ref_idx_active[1] == 0U);

	/* The deblocking idc 1 leaves the offsets out, and idc 2 keeps them. */
	p.deblock_idc = 1U;
	expect_ok(&p, &header);
	CHECK(header.disable_deblocking == 1U);
	CHECK(header.filter_offset_a == 0);
	CHECK(header.filter_offset_b == 0);
	p.deblock_idc = 2U;
	expect_ok(&p, &header);
	CHECK(header.disable_deblocking == 2U);
	CHECK(header.filter_offset_a == -6);
	CHECK(header.filter_offset_b == 12);

	/* A non-reference picture has no marking, and the stream still reads to its end. */
	p.ref_idc = 0U;
	expect_ok(&p, &header);
	CHECK(header.nal_ref_idc == 0U);
	CHECK(header.adaptive_marking == 0);
}

/* A P slice that uses every optional part: the order count type 1, redundant_pic_cnt, the override, modifications, weights, marking, CABAC. */
static void
test_predicted(void)
{
	struct config c;
	struct params p;
	struct h264sw_slice_header header;
	unsigned index;

	config_defaults(&c);
	c.poc_type = 1U;
	c.bottom_poc = 1;
	c.redundant = 1;
	c.weighted_pred = 1;
	c.cabac = 1;
	c.deblocking = 1;
	c.ref0_minus1 = 0U;
	c.max_ref = 8U;
	install(&c);
	params_slice(&p, 0U);
	p.delta0 = -7;
	p.delta1 = 11;
	p.redundant = 3U;
	p.override = 1;
	p.n0_minus1 = 3U;
	p.mod_flag[0] = 1;
	p.mod_count[0] = 3U;
	p.mod_idc[0][0] = H264SW_MOD_SUBTRACT;
	p.mod_value[0][0] = 5U;
	p.mod_idc[0][1] = H264SW_MOD_ADD;
	p.mod_value[0][1] = 2U;
	p.mod_idc[0][2] = H264SW_MOD_LONG_TERM;
	p.mod_value[0][2] = 1U;
	p.luma_denom = 5U;
	p.chroma_denom = 3U;
	p.luma_flag[0][1] = 1;
	p.luma_weight[0][1] = 40;
	p.luma_offset[0][1] = -5;
	p.chroma_flag[0][2] = 1;
	p.chroma_weight[0][2][0] = 7;
	p.chroma_offset[0][2][0] = -128;
	p.chroma_weight[0][2][1] = 127;
	p.chroma_offset[0][2][1] = 127;
	p.luma_flag[0][3] = 1;
	p.luma_weight[0][3] = -128;
	p.luma_offset[0][3] = 127;
	p.chroma_flag[0][3] = 1;
	p.chroma_weight[0][3][0] = 1;
	p.chroma_offset[0][3][0] = 2;
	p.chroma_weight[0][3][1] = 3;
	p.chroma_offset[0][3][1] = 4;
	p.adaptive = 1;
	p.mmco_count = 6U;
	p.mmco[0].operation = H264SW_MMCO_SHORT_UNUSED;
	p.mmco[0].difference = 4U;
	p.mmco[1].operation = H264SW_MMCO_LONG_UNUSED;
	p.mmco[1].long_term_pic_num = 3U;
	p.mmco[2].operation = H264SW_MMCO_SHORT_TO_LONG;
	p.mmco[2].difference = 0U;
	p.mmco[2].long_term_frame_idx = 2U;
	p.mmco[3].operation = H264SW_MMCO_MAX_LONG_INDEX;
	p.mmco[3].max_plus1 = 3U;
	p.mmco[4].operation = H264SW_MMCO_CURRENT_TO_LONG;
	p.mmco[4].long_term_frame_idx = 1U;
	p.mmco[5].operation = H264SW_MMCO_ALL_UNUSED;
	p.cabac_init = 2U;
	p.qp_delta = -10;
	p.deblock_idc = 1U;
	expect_ok(&p, &header);

	/* The header fields in the order of the syntax. */
	CHECK(header.slice_type == H264SW_SLICE_P);
	CHECK(header.frame_num == 3U);
	CHECK(header.delta_poc[0] == -7);
	CHECK(header.delta_poc[1] == 11);
	CHECK(header.poc_lsb == 0U);
	CHECK(header.delta_poc_bottom == 0);
	CHECK(header.redundant_pic_cnt == 3U);
	CHECK(header.direct_spatial == 0);
	CHECK(header.num_ref_idx_active[0] == 4U);
	CHECK(header.num_ref_idx_active[1] == 0U);

	/* The list 0 modifications, in order, without the ending idc. */
	CHECK(header.modification_count[0] == 3U);
	CHECK(header.modification_count[1] == 0U);
	CHECK(header.modification[0][0].idc == 0U);
	CHECK(header.modification[0][0].value == 5U);
	CHECK(header.modification[0][1].idc == 1U);
	CHECK(header.modification[0][1].value == 2U);
	CHECK(header.modification[0][2].idc == 2U);
	CHECK(header.modification[0][2].value == 1U);

	/* The weights: 2^5 = 32 and 2^3 = 8 where the flag is 0 (7.4.3.2). */
	CHECK(header.weighted == 1);
	CHECK(header.luma_log2_denom == 5U);
	CHECK(header.chroma_log2_denom == 3U);
	CHECK(header.weight[0][0].luma_present == 0);
	CHECK(header.weight[0][0].chroma_present == 0);
	CHECK(header.weight[0][0].luma_weight == 32);
	CHECK(header.weight[0][0].luma_offset == 0);
	CHECK(header.weight[0][0].chroma_weight[0] == 8);
	CHECK(header.weight[0][0].chroma_weight[1] == 8);
	CHECK(header.weight[0][0].chroma_offset[0] == 0);
	CHECK(header.weight[0][0].chroma_offset[1] == 0);
	CHECK(header.weight[0][1].luma_present == 1);
	CHECK(header.weight[0][1].luma_weight == 40);
	CHECK(header.weight[0][1].luma_offset == -5);
	CHECK(header.weight[0][1].chroma_present == 0);
	CHECK(header.weight[0][1].chroma_weight[0] == 8);
	CHECK(header.weight[0][2].luma_present == 0);
	CHECK(header.weight[0][2].luma_weight == 32);
	CHECK(header.weight[0][2].chroma_present == 1);
	CHECK(header.weight[0][2].chroma_weight[0] == 7);
	CHECK(header.weight[0][2].chroma_offset[0] == -128);
	CHECK(header.weight[0][2].chroma_weight[1] == 127);
	CHECK(header.weight[0][2].chroma_offset[1] == 127);
	CHECK(header.weight[0][3].luma_present == 1);
	CHECK(header.weight[0][3].luma_weight == -128);
	CHECK(header.weight[0][3].luma_offset == 127);
	CHECK(header.weight[0][3].chroma_present == 1);
	CHECK(header.weight[0][3].chroma_weight[0] == 1);
	CHECK(header.weight[0][3].chroma_offset[0] == 2);
	CHECK(header.weight[0][3].chroma_weight[1] == 3);
	CHECK(header.weight[0][3].chroma_offset[1] == 4);

	/* The adaptive marking: six operations, each with only the fields its operation sends. */
	CHECK(header.adaptive_marking == 1);
	CHECK(header.mmco_count == 6U);
	CHECK(header.has_mmco5 == 1);
	CHECK(header.mmco[0].operation == 1U);
	CHECK(header.mmco[0].difference_of_pic_nums_minus1 == 4U);
	CHECK(header.mmco[0].long_term_pic_num == 0U);
	CHECK(header.mmco[0].long_term_frame_idx == 0U);
	CHECK(header.mmco[0].max_long_term_frame_idx_plus1 == 0U);
	CHECK(header.mmco[1].operation == 2U);
	CHECK(header.mmco[1].long_term_pic_num == 3U);
	CHECK(header.mmco[1].difference_of_pic_nums_minus1 == 0U);
	CHECK(header.mmco[2].operation == 3U);
	CHECK(header.mmco[2].difference_of_pic_nums_minus1 == 0U);
	CHECK(header.mmco[2].long_term_frame_idx == 2U);
	CHECK(header.mmco[3].operation == 4U);
	CHECK(header.mmco[3].max_long_term_frame_idx_plus1 == 3U);
	CHECK(header.mmco[4].operation == 6U);
	CHECK(header.mmco[4].long_term_frame_idx == 1U);
	CHECK(header.mmco[5].operation == 5U);
	CHECK(header.mmco[5].long_term_frame_idx == 0U);

	/* CABAC's context choice, the quantizer 26 - 10, and the deblocking idc 1 without offsets. */
	CHECK(header.cabac_init_idc == 2U);
	CHECK(header.qp == 16);
	CHECK(header.disable_deblocking == 1U);
	CHECK(header.filter_offset_a == 0);
	CHECK(header.filter_offset_b == 0);

	/* Without the PPS flags the same slice has none of those fields: no weights, no redundant count, no CABAC choice. */
	c.weighted_pred = 0;
	c.redundant = 0;
	c.cabac = 0;
	install(&c);
	expect_ok(&p, &header);
	CHECK(header.weighted == 0);
	CHECK(header.redundant_pic_cnt == 0U);
	CHECK(header.cabac_init_idc == 0U);
	CHECK(header.mmco_count == 6U);
	CHECK(header.qp == 16);

	/* The PPS default list length applies without the override. */
	c.ref0_minus1 = 2U;
	install(&c);
	p.override = 0;
	expect_ok(&p, &header);
	CHECK(header.num_ref_idx_active[0] == 3U);

	/* Cases: the list modification with no operation, and with the largest count the header keeps. */
	p.mod_flag[0] = 1;
	p.mod_count[0] = 0U;
	expect_ok(&p, &header);
	CHECK(header.modification_count[0] == 0U);
	p.mod_count[0] = H264SW_MAX_MODIFICATIONS;
	for (index = 0U; index < H264SW_MAX_MODIFICATIONS; index++) {
		p.mod_idc[0][index] = index % 3U;
		p.mod_value[0][index] = index % 15U;
	}
	expect_ok(&p, &header);
	CHECK(header.modification_count[0] == H264SW_MAX_MODIFICATIONS);
	CHECK(header.modification[0][H264SW_MAX_MODIFICATIONS - 1U].idc == (H264SW_MAX_MODIFICATIONS - 1U) % 3U);
	CHECK(header.modification[0][H264SW_MAX_MODIFICATIONS - 1U].value == (H264SW_MAX_MODIFICATIONS - 1U) % 15U);
}

/* A B slice: the direct mode, both lists, the weights of both lists and the order count type 0. */
static void
test_bidirectional(void)
{
	struct config c;
	struct params p;
	struct h264sw_slice_header header;

	config_defaults(&c);
	c.poc_type = 0U;
	c.log2_poc_minus4 = 2U;
	c.bipred = 1U;
	c.ref0_minus1 = 1U;
	c.ref1_minus1 = 2U;
	install(&c);

	/* Slice type 6 is a B slice that says every slice of the picture is one. */
	params_slice(&p, 6U);
	p.ref_idc = 0U;
	p.poc_lsb = 37U;
	p.direct = 1;
	p.mod_flag[1] = 1;
	p.mod_count[1] = 2U;
	p.mod_idc[1][0] = H264SW_MOD_ADD;
	p.mod_value[1][0] = 0U;
	p.mod_idc[1][1] = H264SW_MOD_SUBTRACT;
	p.mod_value[1][1] = 9U;
	p.luma_denom = 7U;
	p.chroma_denom = 0U;
	p.luma_flag[1][2] = 1;
	p.luma_weight[1][2] = -1;
	p.luma_offset[1][2] = 1;
	p.chroma_flag[0][1] = 1;
	p.chroma_weight[0][1][0] = -2;
	p.chroma_offset[0][1][0] = -3;
	p.chroma_weight[0][1][1] = 5;
	p.chroma_offset[0][1][1] = 6;
	p.qp_delta = 2;
	expect_ok(&p, &header);
	CHECK(header.slice_type == H264SW_SLICE_B);
	CHECK(header.slice_type_fixed == 1);
	CHECK(header.nal_ref_idc == 0U);
	CHECK(header.poc_lsb == 37U);
	CHECK(header.direct_spatial == 1);
	CHECK(header.num_ref_idx_active[0] == 2U);
	CHECK(header.num_ref_idx_active[1] == 3U);
	CHECK(header.modification_count[0] == 0U);
	CHECK(header.modification_count[1] == 2U);
	CHECK(header.modification[1][0].idc == 1U);
	CHECK(header.modification[1][0].value == 0U);
	CHECK(header.modification[1][1].idc == 0U);
	CHECK(header.modification[1][1].value == 9U);
	CHECK(header.weighted == 1);
	CHECK(header.luma_log2_denom == 7U);
	CHECK(header.chroma_log2_denom == 0U);
	CHECK(header.weight[0][0].luma_weight == 128);
	CHECK(header.weight[0][0].chroma_weight[0] == 1);
	CHECK(header.weight[0][1].chroma_weight[0] == -2);
	CHECK(header.weight[0][1].chroma_offset[0] == -3);
	CHECK(header.weight[0][1].chroma_weight[1] == 5);
	CHECK(header.weight[0][1].chroma_offset[1] == 6);
	CHECK(header.weight[1][0].luma_weight == 128);
	CHECK(header.weight[1][2].luma_present == 1);
	CHECK(header.weight[1][2].luma_weight == -1);
	CHECK(header.weight[1][2].luma_offset == 1);
	CHECK(header.weight[1][2].chroma_weight[1] == 1);
	CHECK(header.mmco_count == 0U);
	CHECK(header.qp == 28);

	/* The B slice's list lengths can be overridden separately. */
	p.override = 1;
	p.n0_minus1 = 4U;
	p.n1_minus1 = 0U;
	expect_ok(&p, &header);
	CHECK(header.num_ref_idx_active[0] == 5U);
	CHECK(header.num_ref_idx_active[1] == 1U);

	/* Implicit weights (idc 2) and no weights (idc 0) send no table; a P slice with the explicit B flag sends none either. */
	c.bipred = 2U;
	install(&c);
	p.override = 0;
	expect_ok(&p, &header);
	CHECK(header.weighted == 0);
	c.bipred = 0U;
	install(&c);
	expect_ok(&p, &header);
	CHECK(header.weighted == 0);
	c.bipred = 1U;
	install(&c);
	params_slice(&p, 0U);
	expect_ok(&p, &header);
	CHECK(header.weighted == 0);
	CHECK(header.num_ref_idx_active[0] == 2U);
	CHECK(header.num_ref_idx_active[1] == 0U);

	/* The largest list lengths of both lists: 32. */
	params_slice(&p, 1U);
	p.override = 1;
	p.n0_minus1 = 31U;
	p.n1_minus1 = 31U;
	p.luma_denom = 1U;
	p.chroma_denom = 2U;
	expect_ok(&p, &header);
	CHECK(header.num_ref_idx_active[0] == 32U);
	CHECK(header.num_ref_idx_active[1] == 32U);
	CHECK(header.weight[1][31].luma_weight == 2);
	CHECK(header.weight[1][31].chroma_weight[0] == 4);
}

/* Slice types 5 to 9 are the types 0 to 4 with the slice_type_fixed mark. */
static void
test_slice_types(void)
{
	struct config c;
	struct params p;
	struct h264sw_slice_header header;
	uint32_t type;

	config_defaults(&c);
	install(&c);

	/* P, B, I as 0, 1, 2 and 5, 6, 7. */
	for (type = 0U; type < 3U; type++) {
		params_slice(&p, type);
		expect_ok(&p, &header);
		CHECK(header.slice_type == type);
		CHECK(header.slice_type_fixed == 0);
		params_slice(&p, type + 5U);
		expect_ok(&p, &header);
		CHECK(header.slice_type == type);
		CHECK(header.slice_type_fixed == 1);
	}
}

/* Every kind of input the header refuses. */
static void
test_errors(void)
{
	struct config c;
	struct params p;
	struct h264sw_slice_header header;
	unsigned index;

	config_defaults(&c);
	c.poc_type = 0U;
	c.cabac = 1;
	c.deblocking = 1;
	c.redundant = 1;
	c.weighted_pred = 1;
	c.bipred = 1U;
	install(&c);

	/* A PPS that was never read, or an id beyond the table, is not found. */
	params_slice(&p, 2U);
	p.pps_id = 7U;
	expect_error(&p, EINVAL);
	p.pps_id = 256U;
	expect_error(&p, EINVAL);
	p.pps_id = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.pps_id = 1U;
	expect_ok(&p, &header);
	CHECK(header.pps_id == 1U);

	/* A PPS whose SPS is gone is not usable. */
	p.pps_id = 0U;
	store.sps_present[0] = 0;
	expect_error(&p, EINVAL);
	store.sps_present[0] = 1;
	expect_ok(&p, &header);

	/* The first macroblock must be below PicSizeInMbs = 12. */
	p.first_mb = TEST_SIZE_MBS - 1U;
	expect_ok(&p, &header);
	p.first_mb = TEST_SIZE_MBS;
	expect_error(&p, EINVAL);
	p.first_mb = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.first_mb = 0U;

	/* Table 7-6 ends at 9. */
	p.slice_type = 9U;
	expect_error(&p, ENOTSUP);
	p.slice_type = 10U;
	expect_error(&p, EINVAL);
	p.slice_type = TEST_BIG_UE;
	expect_error(&p, EINVAL);

	/* SP and SI slices need decoding processes outside the decoder. */
	p.slice_type = 3U;
	expect_error(&p, ENOTSUP);
	p.slice_type = 8U;
	expect_error(&p, ENOTSUP);
	p.slice_type = 4U;
	expect_error(&p, ENOTSUP);

	/* An IDR picture has I slices only; the frame number and the identifier have ranges. */
	params_idr(&p);
	p.slice_type = 0U;
	expect_error(&p, EINVAL);
	p.slice_type = 5U;
	expect_error(&p, EINVAL);
	p.slice_type = 1U;
	expect_error(&p, EINVAL);
	p.slice_type = 3U;
	expect_error(&p, EINVAL);
	p.slice_type = 2U;
	p.frame_num = 1U;
	expect_error(&p, EINVAL);
	p.frame_num = 0U;
	p.idr_pic_id = 65536U;
	expect_error(&p, EINVAL);
	p.idr_pic_id = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.idr_pic_id = 65535U;
	expect_ok(&p, &header);

	/* Data partitions are outside the decoder; other NAL unit types do not carry this header. */
	params_slice(&p, 2U);
	build(&current_config, &p);
	CHECK(read_rbsp(2U, 2U, writer.data, bitwriter_bytes(&writer), &header) == ENOTSUP);
	CHECK(read_rbsp(3U, 2U, writer.data, bitwriter_bytes(&writer), &header) == ENOTSUP);
	CHECK(read_rbsp(4U, 2U, writer.data, bitwriter_bytes(&writer), &header) == ENOTSUP);
	CHECK(read_rbsp(6U, 2U, writer.data, bitwriter_bytes(&writer), &header) == EINVAL);
	CHECK(read_rbsp(20U, 2U, writer.data, bitwriter_bytes(&writer), &header) == EINVAL);
	CHECK(read_rbsp(0U, 2U, writer.data, bitwriter_bytes(&writer), &header) == EINVAL);

	/* redundant_pic_cnt is at most 127. */
	params_slice(&p, 0U);
	p.redundant = 127U;
	expect_ok(&p, &header);
	p.redundant = 128U;
	expect_error(&p, EINVAL);
	p.redundant = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.redundant = 0U;

	/* List lengths of 1 to 32 only. */
	p.override = 1;
	p.n0_minus1 = 32U;
	expect_error(&p, EINVAL);
	p.n0_minus1 = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.n0_minus1 = 31U;
	expect_ok(&p, &header);
	params_slice(&p, 1U);
	p.override = 1;
	p.n0_minus1 = 1U;
	p.n1_minus1 = 32U;
	expect_error(&p, EINVAL);
	p.n1_minus1 = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.n1_minus1 = 0U;
	expect_ok(&p, &header);

	/* Modification idc above 3, too many modifications, a difference beyond MaxPicNum = 16. */
	params_slice(&p, 0U);
	p.mod_flag[0] = 1;
	p.mod_count[0] = 1U;
	p.mod_idc[0][0] = 4U;
	expect_error(&p, EINVAL);
	p.mod_idc[0][0] = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.mod_idc[0][0] = 0U;
	p.mod_value[0][0] = 16U;
	expect_error(&p, EINVAL);
	p.mod_idc[0][0] = 1U;
	expect_error(&p, EINVAL);
	p.mod_value[0][0] = 15U;
	expect_ok(&p, &header);
	p.mod_idc[0][0] = 2U;
	p.mod_value[0][0] = TEST_BIG_UE;
	expect_ok(&p, &header);
	CHECK(header.modification[0][0].value == TEST_BIG_UE);
	p.mod_count[0] = H264SW_MAX_MODIFICATIONS + 1U;
	for (index = 0U; index < TEST_MODS; index++) {
		p.mod_idc[0][index] = 2U;
		p.mod_value[0][index] = index;
	}
	expect_error(&p, EINVAL);
	p.mod_count[0] = TEST_MODS;
	expect_error(&p, EINVAL);
	p.mod_flag[0] = 0;

	/* Weights: the denominators 0 to 7 and the values -128 to 127. */
	p.override = 0;
	p.luma_denom = 8U;
	expect_error(&p, EINVAL);
	p.luma_denom = 7U;
	p.chroma_denom = 8U;
	expect_error(&p, EINVAL);
	p.chroma_denom = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.chroma_denom = 0U;
	expect_ok(&p, &header);
	p.luma_flag[0][0] = 1;
	p.luma_weight[0][0] = 128;
	expect_error(&p, EINVAL);
	p.luma_weight[0][0] = -129;
	expect_error(&p, EINVAL);
	p.luma_weight[0][0] = 127;
	p.luma_offset[0][0] = 128;
	expect_error(&p, EINVAL);
	p.luma_offset[0][0] = -129;
	expect_error(&p, EINVAL);
	p.luma_offset[0][0] = INT32_MAX;
	expect_error(&p, EINVAL);
	p.luma_offset[0][0] = -128;
	expect_ok(&p, &header);
	p.chroma_flag[0][0] = 1;
	p.chroma_weight[0][0][1] = 128;
	expect_error(&p, EINVAL);
	p.chroma_weight[0][0][1] = 0;
	p.chroma_offset[0][0][1] = -129;
	expect_error(&p, EINVAL);
	p.chroma_offset[0][0][1] = 0;
	expect_ok(&p, &header);
	p.luma_flag[0][0] = 0;
	p.chroma_flag[0][0] = 0;

	/* Marking: no operation above 6, no more than the header keeps, operation 4 within max_num_ref_frames = 4. */
	p.adaptive = 1;
	p.mmco_count = 1U;
	p.mmco[0].operation = 7U;
	expect_error(&p, EINVAL);
	p.mmco[0].operation = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.mmco[0].operation = 4U;
	p.mmco[0].max_plus1 = 5U;
	expect_error(&p, EINVAL);
	p.mmco[0].max_plus1 = 4U;
	expect_ok(&p, &header);
	p.mmco[0].max_plus1 = 0U;
	expect_ok(&p, &header);
	p.mmco_count = H264SW_MAX_MMCO;
	for (index = 0U; index < TEST_MMCOS; index++) {
		p.mmco[index].operation = 6U;
		p.mmco[index].long_term_frame_idx = index;
	}
	expect_ok(&p, &header);
	CHECK(header.mmco_count == H264SW_MAX_MMCO);
	CHECK(header.mmco[H264SW_MAX_MMCO - 1U].long_term_frame_idx == H264SW_MAX_MMCO - 1U);
	p.mmco_count = H264SW_MAX_MMCO + 1U;
	expect_error(&p, EINVAL);
	p.mmco_count = TEST_MMCOS;
	expect_error(&p, EINVAL);
	p.mmco_count = 1U;
	p.mmco[0].operation = 1U;
	p.mmco[0].difference = TEST_BIG_UE;
	expect_ok(&p, &header);
	CHECK(header.mmco[0].difference_of_pic_nums_minus1 == TEST_BIG_UE);
	p.adaptive = 0;

	/* cabac_init_idc 0 to 2. */
	p.cabac_init = 3U;
	expect_error(&p, EINVAL);
	p.cabac_init = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.cabac_init = 2U;
	expect_ok(&p, &header);

	/* SliceQPY = 26 + delta in 0..51. */
	p.qp_delta = 25;
	expect_ok(&p, &header);
	CHECK(header.qp == 51);
	p.qp_delta = 26;
	expect_error(&p, EINVAL);
	p.qp_delta = -26;
	expect_ok(&p, &header);
	CHECK(header.qp == 0);
	p.qp_delta = -27;
	expect_error(&p, EINVAL);
	p.qp_delta = INT32_MAX;
	expect_error(&p, EINVAL);
	p.qp_delta = -INT32_MAX;
	expect_error(&p, EINVAL);
	p.qp_delta = 0;

	/* The deblocking idc 0 to 2, and the offsets -6 to 6 before doubling. */
	p.deblock_idc = 3U;
	expect_error(&p, EINVAL);
	p.deblock_idc = TEST_BIG_UE;
	expect_error(&p, EINVAL);
	p.deblock_idc = 0U;
	p.alpha = 7;
	expect_error(&p, EINVAL);
	p.alpha = -7;
	expect_error(&p, EINVAL);
	p.alpha = INT32_MAX;
	expect_error(&p, EINVAL);
	p.alpha = 6;
	p.beta = 7;
	expect_error(&p, EINVAL);
	p.beta = -7;
	expect_error(&p, EINVAL);
	p.beta = -6;
	expect_ok(&p, &header);
	CHECK(header.filter_offset_a == 12);
	CHECK(header.filter_offset_b == -12);
	p.deblock_idc = 1U;
	p.alpha = 100;
	expect_ok(&p, &header);
	CHECK(header.filter_offset_a == 0);
}

/* Extreme order count differences are kept as written, without overflowing. */
static void
test_extremes(void)
{
	struct config c;
	struct params p;
	struct h264sw_slice_header header;

	config_defaults(&c);
	c.poc_type = 0U;
	c.log2_poc_minus4 = 12U;
	c.log2_frame_minus4 = 12U;
	c.bottom_poc = 1;
	install(&c);
	params_slice(&p, 2U);
	p.frame_num = 65535U;
	p.poc_lsb = 65535U;
	p.delta_bottom = INT32_MAX;
	expect_ok(&p, &header);
	CHECK(header.frame_num == 65535U);
	CHECK(header.poc_lsb == 65535U);
	CHECK(header.delta_poc_bottom == INT32_MAX);
	p.delta_bottom = -INT32_MAX;
	expect_ok(&p, &header);
	CHECK(header.delta_poc_bottom == -INT32_MAX);

	/* Order count type 1 with both differences at the extremes. */
	c.poc_type = 1U;
	c.always_zero = 0;
	install(&c);
	p.delta0 = INT32_MAX;
	p.delta1 = -INT32_MAX;
	expect_ok(&p, &header);
	CHECK(header.delta_poc[0] == INT32_MAX);
	CHECK(header.delta_poc[1] == -INT32_MAX);
	CHECK(header.poc_lsb == 0U);

	/* delta_pic_order_always_zero_flag leaves both out. */
	c.always_zero = 1;
	install(&c);
	expect_ok(&p, &header);
	CHECK(header.delta_poc[0] == 0);
	CHECK(header.delta_poc[1] == 0);
}

/* A missing argument is an error, not a crash. */
static void
test_arguments(void)
{
	struct config c;
	struct params p;
	struct h264sw_slice_header header;
	struct h264sw_bits bits;
	struct h264sw_nal nal;

	config_defaults(&c);
	install(&c);
	params_slice(&p, 2U);
	build(&current_config, &p);
	h264sw_bits_init(&bits, writer.data, bitwriter_bytes(&writer));
	nal.type = 1U;
	nal.ref_idc = 1U;
	nal.rbsp = writer.data;
	nal.rbsp_size = bitwriter_bytes(&writer);
	CHECK(h264sw_slice_read(NULL, &nal, &store, &header) == EINVAL);
	CHECK(h264sw_slice_read(&bits, NULL, &store, &header) == EINVAL);
	CHECK(h264sw_slice_read(&bits, &nal, NULL, &header) == EINVAL);
	CHECK(h264sw_slice_read(&bits, &nal, &store, NULL) == EINVAL);

	/* An empty RBSP cannot hold a header. */
	CHECK(read_rbsp(1U, 1U, writer.data, 0U, &header) == EINVAL);
}

/* Every prefix of a header with all optional parts is a truncated header: an error, never a crash, and never a success. */
static void
test_truncation(void)
{
	struct config c;
	struct params p;
	struct h264sw_slice_header header;
	uint8_t copy[BITWRITER_BYTES];
	size_t size;
	size_t length;
	size_t needed;
	int error;
	unsigned round;

	for (round = 0U; round < 2U; round++) {
		config_defaults(&c);
		c.poc_type = 0U;
		c.bottom_poc = 1;
		c.redundant = 1;
		c.weighted_pred = 1;
		c.bipred = 1U;
		c.cabac = 1;
		c.deblocking = 1;
		c.ref0_minus1 = 2U;
		c.ref1_minus1 = 1U;
		install(&c);
		params_slice(&p, round == 0U ? 0U : 1U);
		p.mod_flag[0] = 1;
		p.mod_count[0] = 2U;
		p.mod_idc[0][0] = 0U;
		p.mod_value[0][0] = 3U;
		p.mod_idc[0][1] = 2U;
		p.mod_value[0][1] = 1U;
		p.mod_flag[1] = 1;
		p.mod_count[1] = 1U;
		p.mod_idc[1][0] = 1U;
		p.mod_value[1][0] = 2U;
		p.luma_flag[0][1] = 1;
		p.luma_weight[0][1] = 3;
		p.chroma_flag[0][2] = 1;
		p.adaptive = 1;
		p.mmco_count = 2U;
		p.mmco[0].operation = 1U;
		p.mmco[1].operation = 3U;
		p.deblock_idc = 0U;
		p.alpha = 1;
		p.beta = 2;
		expect_ok(&p, &header);

		/* Keeps the whole RBSP and the bytes the header needs. */
		size = bitwriter_bytes(&writer);
		memcpy(copy, writer.data, size);
		needed = (header_bits + 7U) / 8U;

		/* Every shorter RBSP fails to read; a longer one up to the full size reads. */
		for (length = 0U; length < needed; length++) {
			error = read_rbsp(1U, 2U, copy, length, &header);
			CHECK(error == EINVAL);
		}
		for (length = needed; length <= size; length++) {
			error = read_rbsp(1U, 2U, copy, length, &header);
			CHECK(error == 0);
		}
	}
}

/* Checks the invariants every successful read keeps, whatever the bits were. */
static void
check_invariants(
	const struct h264sw_slice_header *h)
{
	unsigned list;
	unsigned index;

	CHECK(h->slice_type <= H264SW_SLICE_B || h->slice_type == H264SW_SLICE_I);
	CHECK(h->first_mb < TEST_SIZE_MBS);
	CHECK(h->qp >= 0 && h->qp <= 51);
	CHECK(h->num_ref_idx_active[0] <= H264SW_MAX_LIST);
	CHECK(h->num_ref_idx_active[1] <= H264SW_MAX_LIST);
	CHECK(h->disable_deblocking <= 2U);
	CHECK(h->filter_offset_a >= -12 && h->filter_offset_a <= 12);
	CHECK(h->filter_offset_b >= -12 && h->filter_offset_b <= 12);
	CHECK(h->cabac_init_idc <= 2U);
	CHECK(h->mmco_count <= H264SW_MAX_MMCO);
	for (list = 0U; list < 2U; list++) {
		CHECK(h->modification_count[list] <= H264SW_MAX_MODIFICATIONS);
		for (index = 0U; index < h->modification_count[list]; index++)
			CHECK(h->modification[list][index].idc <= 2U);
	}
	for (index = 0U; index < h->mmco_count; index++)
		CHECK(h->mmco[index].operation >= 1U && h->mmco[index].operation <= 6U);
	if (h->idr != 0) {
		CHECK(h->frame_num == 0U);
		CHECK(h->slice_type == H264SW_SLICE_I);
	}
}

/* Next value of a deterministic pseudo-random sequence. */
static uint32_t
next_random(
	uint32_t *state)
{
	*state = *state * 1664525U + 1013904223U;
	return *state >> 8;
}

/* Damaged and random input never crashes the reader and keeps its invariants when it reads. */
static void
test_fuzz(void)
{
	struct config c;
	struct params p;
	struct h264sw_slice_header header;
	uint8_t copy[BITWRITER_BYTES];
	uint8_t damaged[BITWRITER_BYTES];
	size_t size;
	size_t round;
	size_t flips;
	size_t at;
	uint32_t state;
	unsigned type;
	int error;

	/* A configuration with every optional field present, and a P slice using many of them. */
	config_defaults(&c);
	c.poc_type = 0U;
	c.bottom_poc = 1;
	c.redundant = 1;
	c.weighted_pred = 1;
	c.bipred = 1U;
	c.cabac = 1;
	c.deblocking = 1;
	c.ref0_minus1 = 3U;
	c.ref1_minus1 = 1U;
	install(&c);
	params_slice(&p, 1U);
	p.mod_flag[0] = 1;
	p.mod_count[0] = 2U;
	p.mod_idc[0][1] = 1U;
	p.luma_flag[0][1] = 1;
	p.chroma_flag[0][2] = 1;
	p.adaptive = 1;
	p.mmco_count = 3U;
	p.mmco[0].operation = 1U;
	p.mmco[1].operation = 3U;
	p.mmco[2].operation = 4U;
	p.mmco[2].max_plus1 = 1U;
	p.deblock_idc = 0U;
	expect_ok(&p, &header);
	size = bitwriter_bytes(&writer);
	memcpy(copy, writer.data, size);

	/* Flips a few bits of the valid RBSP at a time. */
	state = 12345U;
	for (round = 0U; round < 20000U; round++) {
		memcpy(damaged, copy, size);
		flips = 1U + (next_random(&state) % 4U);
		while (flips != 0U) {
			at = next_random(&state) % (size * 8U);
			damaged[at / 8U] ^= (uint8_t)(0x80U >> (at % 8U));
			flips--;
		}
		type = 1U;
		if ((next_random(&state) & 7U) == 0U)
			type = 5U;
		error = read_rbsp(type, 1U + (next_random(&state) % 3U), damaged, size, &header);
		CHECK(error == 0 || error == EINVAL || error == ENOTSUP);
		if (error == 0)
			check_invariants(&header);
	}

	/* Fills whole buffers with random bytes of random lengths. */
	for (round = 0U; round < 20000U; round++) {
		size = 1U + (next_random(&state) % 64U);
		for (at = 0U; at < size; at++)
			damaged[at] = (uint8_t)next_random(&state);
		type = 1U;
		if ((next_random(&state) & 7U) == 0U)
			type = 5U;
		error = read_rbsp(type, 1U + (next_random(&state) % 3U), damaged, size, &header);
		CHECK(error == 0 || error == EINVAL || error == ENOTSUP);
		if (error == 0)
			check_invariants(&header);
	}

	/* Fills buffers with all zero bits or all one bits: unbounded Exp-Golomb codes. */
	memset(damaged, 0, sizeof(damaged));
	error = read_rbsp(1U, 1U, damaged, 64U, &header);
	CHECK(error == EINVAL);
	memset(damaged, 0xFF, sizeof(damaged));
	error = read_rbsp(1U, 1U, damaged, 64U, &header);
	CHECK(error == 0 || error == EINVAL || error == ENOTSUP);
}

/* The test of 7.4.1.2.4: each comparison alone makes a new picture, and equal slices do not. */
static void
test_new_picture(void)
{
	struct config c;
	struct params p;
	struct h264sw_slice_header base;
	struct h264sw_slice_header other;
	struct h264sw_sps sps;

	/* A P slice of order count type 0 as the earlier slice. */
	config_defaults(&c);
	c.poc_type = 0U;
	c.bottom_poc = 1;
	install(&c);
	params_slice(&p, 0U);
	p.frame_num = 5U;
	p.poc_lsb = 20U;
	p.delta_bottom = 1;
	expect_ok(&p, &base);
	sps = store.sps[0];
	CHECK(sps.poc_type == 0U);

	/* No earlier slice: a new picture. */
	CHECK(h264sw_slice_new_picture(NULL, &base, &sps) == 1);

	/* The same header, and one that differs only in what the clause does not compare, is the same picture. */
	other = base;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 0);
	other.first_mb = 7U;
	other.slice_type = H264SW_SLICE_I;
	other.qp = 40;
	other.cabac_init_idc = 1U;
	other.redundant_pic_cnt = 1U;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 0);

	/* frame_num. */
	other = base;
	other.frame_num = 6U;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 1);

	/* pic_parameter_set_id. */
	other = base;
	other.pps_id = 1U;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 1);

	/* nal_ref_idc: differing with a zero on either side, but not 2 against 1. */
	other = base;
	other.nal_ref_idc = 1U;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 0);
	other.nal_ref_idc = 0U;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 1);
	CHECK(h264sw_slice_new_picture(&other, &base, &sps) == 1);
	CHECK(h264sw_slice_new_picture(&other, &other, &sps) == 0);

	/* With type 0: pic_order_cnt_lsb and delta_pic_order_cnt_bottom. */
	other = base;
	other.poc_lsb = 21U;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 1);
	other = base;
	other.delta_poc_bottom = 2;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 1);

	/* With type 0 the type 1 differences are not compared. */
	other = base;
	other.delta_poc[0] = 4;
	other.delta_poc[1] = 5;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 0);

	/* IdrPicFlag. */
	other = base;
	other.idr = 1;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 1);
	CHECK(h264sw_slice_new_picture(&other, &base, &sps) == 1);

	/* idr_pic_id, compared only when both are IDR slices. */
	other = base;
	other.idr_pic_id = 3U;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 0);
	base.idr = 1;
	other = base;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 0);
	other.idr_pic_id = 3U;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 1);
	base.idr = 0;

	/* With type 1: the two differences are compared, and the type 0 fields are not. */
	sps.poc_type = 1U;
	other = base;
	other.delta_poc[0] = 4;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 1);
	other = base;
	other.delta_poc[1] = 5;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 1);
	other = base;
	other.poc_lsb = 99U;
	other.delta_poc_bottom = 99;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 0);

	/* With type 2 no order count field is compared. */
	sps.poc_type = 2U;
	other = base;
	other.poc_lsb = 99U;
	other.delta_poc_bottom = 99;
	other.delta_poc[0] = 4;
	other.delta_poc[1] = 5;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 0);
	other.frame_num = 0U;
	CHECK(h264sw_slice_new_picture(&base, &other, &sps) == 1);
}

/* Runs every test and prints the summary line. */
int
main(void)
{
	test_hand_coded();
	test_intra();
	test_predicted();
	test_bidirectional();
	test_slice_types();
	test_errors();
	test_extremes();
	test_arguments();
	test_truncation();
	test_fuzz();
	test_new_picture();
	return check_finish("slice");
}
