/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests of h265sw-slice: the slice segment header (7.3.6.1 to 7.3.6.3,
 * 7.4.7.1 to 7.4.7.3) and byte_alignment() (7.3.2.12).
 *
 * Inputs are written by the syntax writer of h265write.h from values the
 * test chooses, and the SPS and PPS the header is read with come from
 * h265sw_ps_read_sps and h265sw_ps_read_pps over the writer's own SPS and
 * PPS.  Expected values are the values written, the inferences of the
 * semantics for the elements that were not written, and hand-worked
 * derivations (the comments give the working).  The random round trip
 * builds the expected header from the written values with its own
 * statement of the presence conditions, which is the syntax table again,
 * and compares every field.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "h265write.h"
#include "userland/desktop/libmedia/h265sw-slice.h"

/* The cut that keeps the whole RBSP. */
#define NO_CUT ((size_t)-1)

/* The state of the pseudo-random generator, fixed so a failure repeats. */
static uint32_t random_state = 0x1F2E3D4CU;

/* The parameter sets the header is read with, and the writer's structures they are made from. */
static struct h265sw_ps_store store;
static struct h265w_sps wsps;
static struct h265w_pps wpps;
static const struct h265sw_sps *sps;
static const struct h265sw_pps *pps;

/* The slice segment header being written and the context its syntax reads. */
static struct h265w_slice wslice;
static struct h265w_slice_context wctx;

/* The writers: one for parameter sets, one for the slice segment header. */
static struct bitwriter pwriter;
static struct bitwriter swriter;

/* The header the reader produced, and one kept as the preceding independent segment. */
static struct h265sw_slice_header hdr;
static struct h265sw_slice_header kept;

/* The header the random scenario must read as, and the kept independent one's. */
static struct h265sw_slice_header expect;
static struct h265sw_slice_header kept_expect;

/* The length of the last written header in bits and bytes. */
static size_t header_bits;
static size_t header_bytes;

/* Static function declarations. */
static uint32_t random_next(void);
static uint32_t rnd(uint32_t count);
static unsigned test_ceil_log2(uint32_t value);
static unsigned ue_bits(uint32_t value);
static int64_t test_floor_div(int64_t numerator, int64_t denominator);
static void fixture_default(void);
static int apply(void);
static int parse_bytes(unsigned nal_type, unsigned temporal_id, const uint8_t *data, size_t size, const struct h265sw_slice_header *independent);
static int parse(unsigned nal_type, unsigned temporal_id, size_t extra, size_t cut, const struct h265sw_slice_header *independent);
static void header_set(unsigned count_negative, unsigned count_positive);
static void set_explicit(struct h265sw_st_rps *out, const struct h265w_st_rps *written);
static void check_zero_fields(const struct h265sw_slice_header *header);
static void compare_headers(const struct h265sw_slice_header *want, const struct h265sw_slice_header *got);
static void check_invariants(const struct h265sw_slice_header *header, size_t size);
static void check_start(void);
static void check_baseline(void);
static void check_nal_types(void);
static void check_slice_types(void);
static void check_address(void);
static void check_dependent(void);
static void check_reserved_flags(void);
static void check_output_and_plane(void);
static void check_sps_sets(void);
static void check_header_sets(void);
static void check_long_term(void);
static void check_long_term_limits(void);
static void check_tools(void);
static void check_inter(void);
static void check_list_modification(void);
static void check_collocated(void);
static void check_weights(void);
static void check_weights_ranges(void);
static void check_merge_and_qp(void);
static void check_chroma_offsets(void);
static void check_deblocking(void);
static void check_entry_points(void);
static void check_extension(void);
static void check_alignment(void);
static void check_arguments(void);
static int gen_params(void);
static void gen_independent(unsigned nal_type, unsigned temporal_id);
static void gen_segment(unsigned nal_type, unsigned temporal_id, int first, int dependent, uint32_t address, const struct h265sw_slice_header *independent_expect);
static void check_random_roundtrip(void);
static void check_garbage(void);

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

/* Returns a pseudo-random number below count (count above 0). */
static uint32_t
rnd(
	uint32_t count)
{
	/* Succeeded: the number. */
	return random_next() % count;
}

/* Returns Ceil( Log2( value ) ): the fewest bits that hold the values 0..value - 1. */
static unsigned
test_ceil_log2(
	uint32_t value)
{
	unsigned bits;

	/* The fewest bits whose range reaches value. */
	bits = 0U;
	while (bits < 32U && ((uint64_t)1U << bits) < (uint64_t)value)
		bits++;

	/* Succeeded: the bit length. */
	return bits;
}

/* Returns the length of ue(v) of value: 2 * floor( log2( value + 1 ) ) + 1. */
static unsigned
ue_bits(
	uint32_t value)
{
	uint64_t code;
	unsigned length;

	/* The prefix is as long as the suffix, and the suffix is the bits of value + 1 below its top bit. */
	code = (uint64_t)value + 1U;
	length = 0U;
	while ((code >> (length + 1U)) != 0U)
		length++;

	/* Succeeded: the code's length. */
	return 2U * length + 1U;
}

/* Divides rounding toward negative infinity (denominator above 0). */
static int64_t
test_floor_div(
	int64_t numerator,
	int64_t denominator)
{
	int64_t quotient;

	/* Rounds toward zero, then steps down for an inexact negative quotient. */
	quotient = numerator / denominator;
	if (numerator % denominator != 0 && numerator < 0)
		quotient -= 1;

	/* Succeeded: the floor. */
	return quotient;
}

/* Resets the writer's SPS, PPS and slice to the valid defaults of h265write.h (64x64, CTB 16, 8 order count bits, one I slice). */
static void
fixture_default(void)
{
	/* The defaults, with room for up to nine reference pictures in a set. */
	h265w_sps_default(&wsps);
	wsps.max_dec_pic_buffering_minus1[0] = 10U;
	h265w_pps_default(&wpps);
	h265w_slice_default(&wslice);
	memset(&wctx, 0, sizeof(wctx));
}

/* Writes the SPS and PPS of the fixture and reads them into the store; fills the writer's context from them. */
static int
apply(void)
{
	struct vcodec_bitreader bits;
	int error;

	/* Starts with an empty store. */
	h265sw_ps_init(&store);

	/* Reads the SPS. */
	bitwriter_init(&pwriter);
	h265w_sps(&pwriter, &wsps);
	vcodec_bitreader_init(&bits, pwriter.data, bitwriter_bytes(&pwriter));
	error = h265sw_ps_read_sps(&store, &bits);
	if (error != 0)
		return error;

	/* Reads the PPS. */
	bitwriter_init(&pwriter);
	h265w_pps(&pwriter, &wpps);
	vcodec_bitreader_init(&bits, pwriter.data, bitwriter_bytes(&pwriter));
	error = h265sw_ps_read_pps(&store, &bits);
	if (error != 0)
		return error;

	/* Names the stored sets and fills the writer's context from the writer's structures. */
	sps = &store.sps[wsps.sps_id];
	pps = &store.pps[wpps.pps_id];
	h265w_slice_context_from(&wctx, &wsps, &wpps, 19U);

	/* Succeeded: the sets are ready. */
	return 0;
}

/*
 * Reads a header from bytes: h265sw_slice_read_start, then
 * h265sw_slice_read_rest with the fixture's SPS and PPS.  hdr starts as
 * a pattern that is not zero, to show the reader clears what it does not
 * write.
 */
static int
parse_bytes(
	unsigned nal_type,
	unsigned temporal_id,
	const uint8_t *data,
	size_t size,
	const struct h265sw_slice_header *independent)
{
	struct vcodec_bitreader bits;
	struct h265sw_nal nal;
	int error;

	/* Describes the NAL unit around the RBSP. */
	memset(&nal, 0, sizeof(nal));
	nal.type = nal_type;
	nal.layer_id = 0U;
	nal.temporal_id = temporal_id;
	nal.rbsp = data;
	nal.rbsp_size = size;
	nal.complete = 1;

	/* Reads the first fields into a structure that holds a pattern. */
	memset(&hdr, 0xCC, sizeof(hdr));
	vcodec_bitreader_init(&bits, data, size);
	error = h265sw_slice_read_start(&hdr, &bits, &nal);
	if (error != 0)
		return error;

	/* Reads the rest. */
	error = h265sw_slice_read_rest(&hdr, &bits, sps, pps, independent);

	/* Succeeded: the reader's answer. */
	return error;
}

/*
 * Writes the fixture's slice header with the context and reads it.  extra
 * bytes of slice data (0x5A) follow the header; cut keeps only that many
 * bytes of the RBSP.
 */
static int
parse(
	unsigned nal_type,
	unsigned temporal_id,
	size_t extra,
	size_t cut,
	const struct h265sw_slice_header *independent)
{
	size_t size;
	size_t index;

	/* Writes the header, then the data after it. */
	bitwriter_init(&swriter);
	wctx.nal_type = nal_type;
	h265w_slice(&swriter, &wslice, &wctx);
	header_bits = swriter.bits;
	header_bytes = bitwriter_bytes(&swriter);
	for (index = 0U; index < extra; index++)
		swriter.data[header_bytes + index] = 0x5AU;
	size = header_bytes + extra;
	if (cut < size)
		size = cut;

	/* Succeeded: the reader's answer. */
	return parse_bytes(nal_type, temporal_id, swriter.data, size, independent);
}

/* Sets the slice's own short-term set: count_negative entries one behind each other then count_positive ahead, all used. */
static void
header_set(
	unsigned count_negative,
	unsigned count_positive)
{
	unsigned index;

	/* The set as signalled: deltas of 1 each, every entry used. */
	wslice.st_rps_sps = 0U;
	memset(&wslice.st_rps, 0, sizeof(wslice.st_rps));
	wslice.st_rps.num_negative_pics = count_negative;
	wslice.st_rps.num_positive_pics = count_positive;
	for (index = 0U; index < count_negative && index < H265W_MAX_RPS; index++) {
		wslice.st_rps.delta_poc_s0_minus1[index] = 0U;
		wslice.st_rps.used_by_curr_pic_s0_flag[index] = 1U;
	}
	for (index = 0U; index < count_positive && index < H265W_MAX_RPS; index++) {
		wslice.st_rps.delta_poc_s1_minus1[index] = 0U;
		wslice.st_rps.used_by_curr_pic_s1_flag[index] = 1U;
	}
}

/* Derives the set an explicit st_ref_pic_set() signals (7-63 to 7-71): negative deltas step back, positive ones step forward. */
static void
set_explicit(
	struct h265sw_st_rps *out,
	const struct h265w_st_rps *written)
{
	int32_t delta;
	unsigned index;

	/* The counts, then each entry. */
	memset(out, 0, sizeof(*out));
	out->num_negative = written->num_negative_pics;
	out->num_positive = written->num_positive_pics;
	delta = 0;
	for (index = 0U; index < written->num_negative_pics; index++) {
		delta -= (int32_t)written->delta_poc_s0_minus1[index] + 1;
		out->delta_poc[index] = delta;
		out->used[index] = (uint8_t)written->used_by_curr_pic_s0_flag[index];
	}
	delta = 0;
	for (index = 0U; index < written->num_positive_pics; index++) {
		delta += (int32_t)written->delta_poc_s1_minus1[index] + 1;
		out->delta_poc[written->num_negative_pics + index] = delta;
		out->used[written->num_negative_pics + index] = (uint8_t)written->used_by_curr_pic_s1_flag[index];
	}
}

/* Checks that every field a header does not set when it is an I slice of an IDR picture is 0 or its inferred value. */
static void
check_zero_fields(
	const struct h265sw_slice_header *header)
{
	static const struct h265sw_st_rps_syntax zero_syntax;
	static const struct h265sw_weights zero_weights;
	unsigned index;

	/* The reference picture set and the long-term entries are empty. */
	CHECK(header->poc_lsb == 0U);
	CHECK(header->st_rps_sps == 0);
	CHECK(header->st_rps_idx == 0U);
	CHECK(header->st_rps.num_negative == 0U);
	CHECK(header->st_rps.num_positive == 0U);
	CHECK(memcmp(&header->st_syntax, &zero_syntax, sizeof(zero_syntax)) == 0);
	CHECK(header->num_long_term_sps == 0U);
	CHECK(header->num_long_term_pics == 0U);
	CHECK(header->num_long_term == 0U);
	for (index = 0U; index < H265SW_MAX_RPS; index++) {
		CHECK(header->st_rps.delta_poc[index] == 0);
		CHECK(header->st_rps.used[index] == 0U);
		CHECK(header->lt_idx_sps[index] == 0U);
		CHECK(header->lt_poc_lsb[index] == 0U);
		CHECK(header->lt_used[index] == 0U);
		CHECK(header->lt_msb_present[index] == 0U);
		CHECK(header->lt_msb_cycle[index] == 0U);
	}

	/* The inter part is absent. */
	CHECK(header->num_ref_idx[0] == 0U);
	CHECK(header->num_ref_idx[1] == 0U);
	CHECK(header->list_mod[0] == 0);
	CHECK(header->list_mod[1] == 0);
	CHECK(header->mvd_l1_zero == 0);
	CHECK(header->cabac_init == 0);
	CHECK(header->collocated_from_l0 == 1);
	CHECK(header->collocated_ref_idx == 0U);
	CHECK(memcmp(&header->weights, &zero_weights, sizeof(zero_weights)) == 0);
	CHECK(header->max_merge_cand == 0U);
	CHECK(header->num_pic_total_curr == 0U);
}

/* Compares every field of two headers. */
static void
compare_headers(
	const struct h265sw_slice_header *want,
	const struct h265sw_slice_header *got)
{
	unsigned index;

	/* The fields of the NAL unit and of every segment. */
	CHECK(got->nal_type == want->nal_type);
	CHECK(got->temporal_id == want->temporal_id);
	CHECK(got->first_in_pic == want->first_in_pic);
	CHECK(got->no_output_of_prior_pics == want->no_output_of_prior_pics);
	CHECK(got->pps_id == want->pps_id);
	CHECK(got->dependent == want->dependent);
	CHECK(got->address == want->address);
	CHECK(got->slice_addr_rs == want->slice_addr_rs);
	CHECK(got->num_entry_points == want->num_entry_points);
	CHECK(got->offset_len == want->offset_len);
	CHECK(got->data_offset == want->data_offset);

	/* The independent fields before the reference picture set. */
	CHECK(got->reserved_flags == want->reserved_flags);
	CHECK(got->slice_type == want->slice_type);
	CHECK(got->pic_output == want->pic_output);
	CHECK(got->colour_plane_id == want->colour_plane_id);
	CHECK(got->poc_lsb == want->poc_lsb);
	CHECK(got->st_rps_sps == want->st_rps_sps);
	CHECK(got->st_rps_idx == want->st_rps_idx);

	/* The short-term set, derived and as signalled. */
	CHECK(got->st_rps.num_negative == want->st_rps.num_negative);
	CHECK(got->st_rps.num_positive == want->st_rps.num_positive);
	for (index = 0U; index < H265SW_MAX_RPS; index++) {
		CHECK(got->st_rps.delta_poc[index] == want->st_rps.delta_poc[index]);
		CHECK(got->st_rps.used[index] == want->st_rps.used[index]);
	}
	CHECK(got->st_syntax.inter_rps_pred == want->st_syntax.inter_rps_pred);
	CHECK(got->st_syntax.delta_idx_minus1 == want->st_syntax.delta_idx_minus1);
	CHECK(got->st_syntax.delta_rps_sign == want->st_syntax.delta_rps_sign);
	CHECK(got->st_syntax.abs_delta_rps_minus1 == want->st_syntax.abs_delta_rps_minus1);
	CHECK(got->st_syntax.ref_num_delta_pocs == want->st_syntax.ref_num_delta_pocs);
	CHECK(got->st_syntax.used_by_curr_pic_flags == want->st_syntax.used_by_curr_pic_flags);
	CHECK(got->st_syntax.use_delta_flags == want->st_syntax.use_delta_flags);
	CHECK(got->st_syntax.num_negative_pics == want->st_syntax.num_negative_pics);
	CHECK(got->st_syntax.num_positive_pics == want->st_syntax.num_positive_pics);
	CHECK(memcmp(got->st_syntax.delta_poc_s0_minus1, want->st_syntax.delta_poc_s0_minus1, sizeof(got->st_syntax.delta_poc_s0_minus1)) == 0);
	CHECK(memcmp(got->st_syntax.delta_poc_s1_minus1, want->st_syntax.delta_poc_s1_minus1, sizeof(got->st_syntax.delta_poc_s1_minus1)) == 0);
	CHECK(got->st_syntax.used_by_curr_pic_s0_flags == want->st_syntax.used_by_curr_pic_s0_flags);
	CHECK(got->st_syntax.used_by_curr_pic_s1_flags == want->st_syntax.used_by_curr_pic_s1_flags);
	CHECK(got->st_syntax.bits == want->st_syntax.bits);

	/* The long-term entries. */
	CHECK(got->num_long_term_sps == want->num_long_term_sps);
	CHECK(got->num_long_term_pics == want->num_long_term_pics);
	CHECK(got->num_long_term == want->num_long_term);
	CHECK(memcmp(got->lt_idx_sps, want->lt_idx_sps, sizeof(got->lt_idx_sps)) == 0);
	CHECK(memcmp(got->lt_poc_lsb, want->lt_poc_lsb, sizeof(got->lt_poc_lsb)) == 0);
	CHECK(memcmp(got->lt_used, want->lt_used, sizeof(got->lt_used)) == 0);
	CHECK(memcmp(got->lt_msb_present, want->lt_msb_present, sizeof(got->lt_msb_present)) == 0);
	CHECK(memcmp(got->lt_msb_cycle, want->lt_msb_cycle, sizeof(got->lt_msb_cycle)) == 0);

	/* The tools and the inter part. */
	CHECK(got->temporal_mvp == want->temporal_mvp);
	CHECK(got->sao_luma == want->sao_luma);
	CHECK(got->sao_chroma == want->sao_chroma);
	CHECK(got->num_ref_idx[0] == want->num_ref_idx[0]);
	CHECK(got->num_ref_idx[1] == want->num_ref_idx[1]);
	CHECK(got->list_mod[0] == want->list_mod[0]);
	CHECK(got->list_mod[1] == want->list_mod[1]);
	CHECK(memcmp(got->list_entry, want->list_entry, sizeof(got->list_entry)) == 0);
	CHECK(got->mvd_l1_zero == want->mvd_l1_zero);
	CHECK(got->cabac_init == want->cabac_init);
	CHECK(got->collocated_from_l0 == want->collocated_from_l0);
	CHECK(got->collocated_ref_idx == want->collocated_ref_idx);
	CHECK(memcmp(&got->weights, &want->weights, sizeof(got->weights)) == 0);
	CHECK(got->max_merge_cand == want->max_merge_cand);

	/* Quantization and filters. */
	CHECK(got->qp == want->qp);
	CHECK(got->cb_qp_offset == want->cb_qp_offset);
	CHECK(got->cr_qp_offset == want->cr_qp_offset);
	CHECK(got->deblocking_override == want->deblocking_override);
	CHECK(got->deblocking_disabled == want->deblocking_disabled);
	CHECK(got->beta_offset_div2 == want->beta_offset_div2);
	CHECK(got->tc_offset_div2 == want->tc_offset_div2);
	CHECK(got->loop_filter_across_slices == want->loop_filter_across_slices);

	/* The derived count. */
	CHECK(got->num_pic_total_curr == want->num_pic_total_curr);
}

/* Checks the invariants any header the reader accepts has, whatever the bits were (size is the RBSP's length in bytes). */
static void
check_invariants(
	const struct h265sw_slice_header *header,
	size_t size)
{
	unsigned list;
	unsigned index;
	unsigned used;
	unsigned total;

	/* The segment's place. */
	CHECK(header->pps_id <= 63U);
	CHECK(header->data_offset <= size);
	CHECK(header->address < sps->pic_size_ctbs);
	CHECK(header->num_entry_points <= 21119U);

	/* The type and the lists. */
	CHECK(header->slice_type <= H265SW_SLICE_I);
	CHECK(header->num_ref_idx[0] <= H265SW_MAX_REFS);
	CHECK(header->num_ref_idx[1] <= H265SW_MAX_REFS);
	if (header->slice_type == H265SW_SLICE_I) {
		CHECK(header->num_ref_idx[0] == 0U);
		CHECK(header->num_ref_idx[1] == 0U);
	}
	if (header->slice_type == H265SW_SLICE_P)
		CHECK(header->num_ref_idx[1] == 0U);
	if (header->slice_type != H265SW_SLICE_I) {
		CHECK(header->num_ref_idx[0] >= 1U);
		CHECK(header->max_merge_cand >= 1U && header->max_merge_cand <= 5U);
		list = (header->collocated_from_l0 != 0) ? 0U : 1U;
		CHECK(header->collocated_ref_idx < header->num_ref_idx[list]);
	}

	/* The reference picture set stays inside the arrays. */
	CHECK(header->st_rps.num_negative + header->st_rps.num_positive <= H265SW_MAX_RPS - 1U);
	CHECK(header->num_long_term == header->num_long_term_sps + header->num_long_term_pics);
	CHECK(header->st_rps.num_negative + header->st_rps.num_positive + header->num_long_term <= H265SW_MAX_RPS - 1U);
	total = 0U;
	for (index = 0U; index < header->st_rps.num_negative + header->st_rps.num_positive; index++) {
		if (header->st_rps.used[index] != 0U)
			total++;
	}
	for (index = 0U; index < header->num_long_term; index++) {
		if (header->lt_used[index] != 0U)
			total++;
	}
	CHECK(header->num_pic_total_curr == total);

	/* A list entry names a picture of the picture's set. */
	for (list = 0U; list < 2U; list++) {
		if (header->list_mod[list] == 0)
			continue;
		for (index = 0U; index < header->num_ref_idx[list]; index++)
			CHECK(header->list_entry[list][index] < header->num_pic_total_curr);
	}

	/* The QP and the offsets are in range. */
	CHECK(header->qp >= -6 * ((int)sps->bit_depth_luma - 8) && header->qp <= 51);
	CHECK(header->cb_qp_offset >= -12 && header->cb_qp_offset <= 12);
	CHECK(header->cr_qp_offset >= -12 && header->cr_qp_offset <= 12);
	CHECK(header->beta_offset_div2 >= -6 && header->beta_offset_div2 <= 6);
	CHECK(header->tc_offset_div2 >= -6 && header->tc_offset_div2 <= 6);

	/* The weights are in range. */
	used = header->weights.luma_log2_denom;
	CHECK(used <= 7U);
	CHECK(header->weights.chroma_log2_denom <= 7U);
	for (list = 0U; list < 2U; list++) {
		for (index = 0U; index < H265SW_MAX_REFS; index++) {
			CHECK(header->weights.luma_weight[list][index] >= -127 && header->weights.luma_weight[list][index] <= 255);
			CHECK(header->weights.luma_offset[list][index] >= -128 && header->weights.luma_offset[list][index] <= 127);
			CHECK(header->weights.chroma_weight[list][index][0] >= -127 && header->weights.chroma_weight[list][index][0] <= 255);
			CHECK(header->weights.chroma_weight[list][index][1] >= -127 && header->weights.chroma_weight[list][index][1] <= 255);
			CHECK(header->weights.chroma_offset[list][index][0] >= -128 && header->weights.chroma_offset[list][index][0] <= 127);
			CHECK(header->weights.chroma_offset[list][index][1] >= -128 && header->weights.chroma_offset[list][index][1] <= 127);
		}
	}
}

/* h265sw_slice_read_start: every NAL type, the flags, the PPS id range, and the errors. */
static void
check_start(void)
{
	static const struct h265sw_slice_header zero_header;
	struct h265sw_slice_header header;
	struct vcodec_bitreader bits;
	struct h265sw_nal nal;
	struct bitwriter writer;
	unsigned type;
	unsigned first;
	unsigned prior;
	unsigned id;
	int error;
	int decodable;
	int irap;

	/* Every nal_unit_type: the decodable ones read, the reserved and non-VCL ones are refused. */
	for (type = 0U; type < 64U; type++) {
		for (first = 0U; first < 2U; first++) {
			for (prior = 0U; prior < 2U; prior++) {
				bitwriter_init(&writer);
				bitwriter_u(&writer, first, 1U);
				if (type >= 16U && type <= 23U)
					bitwriter_u(&writer, prior, 1U);
				bitwriter_ue(&writer, 5U);
				bitwriter_u(&writer, 0x7FU, 7U);

				memset(&nal, 0, sizeof(nal));
				nal.type = type;
				nal.temporal_id = 3U;
				memset(&header, 0xCC, sizeof(header));
				vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
				error = h265sw_slice_read_start(&header, &bits, &nal);

				decodable = (type <= 9U) || (type >= 16U && type <= 21U);
				if (decodable == 0) {
					CHECK(error == EINVAL);
					continue;
				}
				irap = (type >= 16U);
				CHECK(error == 0);
				CHECK(header.nal_type == type);
				CHECK(header.temporal_id == 3U);
				CHECK(header.first_in_pic == (int)first);
				CHECK(header.no_output_of_prior_pics == ((irap != 0) ? (int)prior : 0));
				CHECK(header.pps_id == 5U);

				/* Everything else is cleared, padding included. */
				header.nal_type = 0U;
				header.temporal_id = 0U;
				header.first_in_pic = 0;
				header.no_output_of_prior_pics = 0;
				header.pps_id = 0U;
				CHECK(memcmp(&header, &zero_header, sizeof(header)) == 0);
			}
		}
	}

	/* The PPS id range: 63 is the last. */
	for (id = 0U; id <= 70U; id++) {
		bitwriter_init(&writer);
		bitwriter_u(&writer, 1U, 1U);
		bitwriter_ue(&writer, id);
		memset(&nal, 0, sizeof(nal));
		nal.type = H265SW_NAL_TRAIL_R;
		vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
		error = h265sw_slice_read_start(&header, &bits, &nal);
		if (id <= 63U) {
			CHECK(error == 0);
			CHECK(header.pps_id == id);
		} else {
			CHECK(error == EINVAL);
		}
	}

	/* A huge ue(v) and one with too many leading zeros are refused. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 1U, 1U);
	bitwriter_ue(&writer, 0xFFFFFFFEU);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(h265sw_slice_read_start(&header, &bits, &nal) == EINVAL);
	bitwriter_init(&writer);
	bitwriter_u(&writer, 1U, 1U);
	bitwriter_u(&writer, 0U, 32U);
	bitwriter_u(&writer, 0U, 8U);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(h265sw_slice_read_start(&header, &bits, &nal) == EINVAL);

	/* Every cut of the first fields: an IRAP header is flag, flag, ue(0) = 3 bits, so cut at 0 bytes. */
	vcodec_bitreader_init(&bits, writer.data, 0U);
	CHECK(h265sw_slice_read_start(&header, &bits, &nal) == EINVAL);
	bitwriter_init(&writer);
	bitwriter_u(&writer, 1U, 1U);
	bitwriter_u(&writer, 0U, 1U);
	nal.type = H265SW_NAL_CRA;
	vcodec_bitreader_init(&bits, writer.data, 1U);
	CHECK(h265sw_slice_read_start(&header, &bits, &nal) == EINVAL);

	/* A missing argument. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 0xFFU, 8U);
	nal.type = H265SW_NAL_TRAIL_R;
	vcodec_bitreader_init(&bits, writer.data, 1U);
	CHECK(h265sw_slice_read_start(NULL, &bits, &nal) == EINVAL);
	CHECK(h265sw_slice_read_start(&header, NULL, &nal) == EINVAL);
	CHECK(h265sw_slice_read_start(&header, &bits, NULL) == EINVAL);
}

/* An IDR picture's I slice with the defaults: every field a value the syntax gives or infers. */
static void
check_baseline(void)
{
	int error;

	/* The first segment of an IDR picture, followed by four bytes of slice data. */
	fixture_default();
	CHECK(apply() == 0);
	wslice.slice_qp_delta = 0;
	wslice.loop_filter_across_slices = 1U;
	error = parse(H265SW_NAL_IDR_W_RADL, 0U, 4U, NO_CUT, NULL);
	CHECK(error == 0);
	CHECK(hdr.nal_type == H265SW_NAL_IDR_W_RADL);
	CHECK(hdr.temporal_id == 0U);
	CHECK(hdr.first_in_pic == 1);
	CHECK(hdr.no_output_of_prior_pics == 0);
	CHECK(hdr.pps_id == 0U);
	CHECK(hdr.dependent == 0);
	CHECK(hdr.address == 0U);
	CHECK(hdr.slice_addr_rs == 0U);
	CHECK(hdr.num_entry_points == 0U);
	CHECK(hdr.offset_len == 0U);
	CHECK(hdr.data_offset == header_bytes);
	CHECK(hdr.reserved_flags == 0U);
	CHECK(hdr.slice_type == H265SW_SLICE_I);
	CHECK(hdr.pic_output == 1);
	CHECK(hdr.colour_plane_id == 0U);
	CHECK(hdr.temporal_mvp == 0);
	CHECK(hdr.sao_luma == 0);
	CHECK(hdr.sao_chroma == 0);
	CHECK(hdr.qp == 26);
	CHECK(hdr.cb_qp_offset == 0);
	CHECK(hdr.cr_qp_offset == 0);
	CHECK(hdr.deblocking_override == 0);
	CHECK(hdr.deblocking_disabled == 0);
	CHECK(hdr.beta_offset_div2 == 0);
	CHECK(hdr.tc_offset_div2 == 0);
	CHECK(hdr.loop_filter_across_slices == 1);
	check_zero_fields(&hdr);

	/* The loop filter flag is the written one, since a filter runs. */
	wslice.loop_filter_across_slices = 0U;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.loop_filter_across_slices == 0);
	check_zero_fields(&hdr);
}

/* Every decodable NAL type through the whole header: IRAP and non-IRAP, IDR and not. */
static void
check_nal_types(void)
{
	static const unsigned types[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 16U, 17U, 18U, 19U, 20U, 21U};
	unsigned index;
	unsigned type;
	unsigned temporal_id;
	unsigned prior;
	int irap;
	int idr;
	int error;

	/* Each type with an I slice, the order count and an empty set (an IDR picture has neither). */
	for (index = 0U; index < sizeof(types) / sizeof(types[0]); index++) {
		for (prior = 0U; prior < 2U; prior++) {
			type = types[index];
			irap = (type >= 16U);
			idr = (type == 19U || type == 20U);
			temporal_id = (irap != 0) ? 0U : 1U;

			fixture_default();
			CHECK(apply() == 0);
			wslice.no_output_of_prior_pics = prior;
			wslice.poc_lsb = 0xA7U;
			header_set(0U, 0U);

			error = parse(type, temporal_id, 4U, NO_CUT, NULL);
			CHECK(error == 0);
			CHECK(hdr.nal_type == type);
			CHECK(hdr.temporal_id == temporal_id);
			CHECK(hdr.first_in_pic == 1);
			CHECK(hdr.no_output_of_prior_pics == ((irap != 0) ? (int)prior : 0));
			CHECK(hdr.pps_id == 0U);
			CHECK(hdr.slice_type == H265SW_SLICE_I);
			CHECK(hdr.poc_lsb == ((idr != 0) ? 0U : 0xA7U));
			CHECK(hdr.data_offset == header_bytes);
			CHECK(hdr.qp == 26);
			CHECK(hdr.num_pic_total_curr == 0U);
		}
	}
}

/* The slice type: B, P and I for non-IRAP types, I only for IRAP types, 3 and above refused. */
static void
check_slice_types(void)
{
	static const unsigned types[] = {0U, 1U, 8U, 9U, 16U, 17U, 18U, 19U, 20U, 21U};
	unsigned index;
	unsigned type;
	unsigned slice_type;
	int error;

	/* Each NAL type with slice types 0..2. */
	for (index = 0U; index < sizeof(types) / sizeof(types[0]); index++) {
		for (slice_type = 0U; slice_type <= 2U; slice_type++) {
			type = types[index];
			fixture_default();
			CHECK(apply() == 0);
			header_set(0U, 0U);
			wslice.slice_type = slice_type;
			error = parse(type, 0U, 0U, NO_CUT, NULL);
			if (type >= 16U && slice_type != H265SW_SLICE_I) {
				CHECK(error == EINVAL);
			} else {
				CHECK(error == 0);
				CHECK(hdr.slice_type == slice_type);
			}
		}
	}

	/* Slice types 3 to 7 and a huge one are refused. */
	for (slice_type = 3U; slice_type <= 7U; slice_type++) {
		fixture_default();
		CHECK(apply() == 0);
		header_set(0U, 0U);
		wslice.slice_type = slice_type;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
		CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == EINVAL);
	}
	wslice.slice_type = 0xFFFFFFFEU;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
}

/* slice_segment_address: its bit length Ceil( Log2( PicSizeInCtbsY ) ) for several picture sizes, and its range. */
static void
check_address(void)
{
	static const unsigned sizes[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U, 65U, 255U, 256U, 257U, 1023U, 1024U, 1025U};
	uint32_t candidates[8];
	unsigned index;
	unsigned count;
	unsigned bits;
	unsigned entry;
	uint32_t address;
	uint32_t limit;
	int error;

	/* Pictures one CTB tall and N CTBs wide. */
	for (index = 0U; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		count = sizes[index];
		fixture_default();
		wsps.width = 16U * count;
		wsps.height = 16U;
		wpps.num_extra_slice_header_bits = 6U;
		CHECK(apply() == 0);
		CHECK(sps->pic_size_ctbs == count);
		bits = test_ceil_log2(count);
		limit = (bits >= 31U) ? 0xFFFFFFFFU : (((uint32_t)1U << bits) - 1U);

		/* Addresses around the ends of the range and of the bit length. */
		candidates[0] = 0U;
		candidates[1] = 1U;
		candidates[2] = count / 2U;
		candidates[3] = count - 1U;
		candidates[4] = count;
		candidates[5] = count + 1U;
		candidates[6] = limit;
		candidates[7] = limit - 1U;

		/* A header with six reserved flags after the address shows where the address ended. */
		for (entry = 0U; entry < 8U; entry++) {
			address = candidates[entry];
			if (address > limit)
				continue;
			fixture_default();
			wsps.width = 16U * count;
			wsps.height = 16U;
			wpps.num_extra_slice_header_bits = 6U;
			CHECK(apply() == 0);
			wslice.first_in_pic = 0U;
			wslice.address = address;
			wslice.reserved_flags = 0x2AU;
			wslice.dependent = 1U;

			error = parse(H265SW_NAL_IDR_W_RADL, 0U, 3U, NO_CUT, NULL);
			if (address >= 1U && address <= count - 1U) {
				CHECK(error == 0);
				CHECK(hdr.address == address);
				CHECK(hdr.slice_addr_rs == address);
				CHECK(hdr.first_in_pic == 0);
				CHECK(hdr.dependent == 0);
				CHECK(hdr.reserved_flags == 0x2AU);
				CHECK(hdr.data_offset == header_bytes);
			} else {
				CHECK(error == EINVAL);
			}
		}
	}

	/* A wider picture: 2D sizes whose CTB count is not a multiple of the width. */
	fixture_default();
	wsps.width = 16U * 7U;
	wsps.height = 16U * 5U;
	CHECK(apply() == 0);
	CHECK(sps->pic_size_ctbs == 35U);
	wslice.first_in_pic = 0U;
	for (address = 0U; address < 64U; address++) {
		wslice.address = address;
		error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
		if (address >= 1U && address <= 34U) {
			CHECK(error == 0);
			CHECK(hdr.address == address);
		} else {
			CHECK(error == EINVAL);
		}
	}
}

/* dependent_slice_segment_flag, the copy of the independent fields and SliceAddrRs. */
static void
check_dependent(void)
{
	struct h265sw_slice_header other;
	struct h265sw_slice_header chain;
	int error;

	/* Without dependent_slice_segments_enabled_flag the flag is not read, whatever the writer's field holds. */
	fixture_default();
	wpps.dependent_slices = 0U;
	wsps.width = 64U;
	CHECK(apply() == 0);
	wslice.first_in_pic = 0U;
	wslice.address = 3U;
	wslice.dependent = 1U;
	wslice.slice_type = H265SW_SLICE_P;
	wslice.five_minus_max_num_merge_cand = 1U;
	header_set(1U, 0U);
	error = parse(H265SW_NAL_TRAIL_R, 0U, 2U, NO_CUT, NULL);
	CHECK(error == 0);
	CHECK(hdr.dependent == 0);
	CHECK(hdr.address == 3U);
	CHECK(hdr.slice_addr_rs == 3U);
	CHECK(hdr.slice_type == H265SW_SLICE_P);
	CHECK(hdr.max_merge_cand == 4U);

	/* With it, an independent segment is read in full and kept. */
	fixture_default();
	wpps.dependent_slices = 1U;
	wpps.slice_chroma_qp_offsets_present = 1U;
	wpps.cabac_init_present = 1U;
	wsps.sao = 1U;
	wsps.temporal_mvp = 1U;
	wsps.long_term_present = 1U;
	wsps.num_lt_sps = 1U;
	wsps.lt_poc_lsb[0] = 9U;
	wsps.lt_used[0] = 1U;
	CHECK(apply() == 0);
	wslice.first_in_pic = 0U;
	wslice.dependent = 0U;
	wslice.address = 3U;
	wslice.slice_type = H265SW_SLICE_B;
	wslice.poc_lsb = 77U;
	header_set(2U, 1U);
	wslice.num_long_term_sps = 1U;
	wslice.num_long_term_pics = 1U;
	wslice.poc_lsb_lt[1] = 100U;
	wslice.used_by_curr_pic_lt[1] = 1U;
	wslice.delta_poc_msb_present[0] = 1U;
	wslice.delta_poc_msb_cycle_lt[0] = 4U;
	wslice.temporal_mvp = 1U;
	wslice.sao_luma = 1U;
	wslice.sao_chroma = 1U;
	wslice.num_ref_idx_override = 1U;
	wslice.num_ref_idx_active_minus1[0] = 2U;
	wslice.num_ref_idx_active_minus1[1] = 1U;
	wslice.mvd_l1_zero = 1U;
	wslice.cabac_init = 1U;
	wslice.collocated_from_l0 = 0U;
	wslice.collocated_ref_idx = 1U;
	wslice.five_minus_max_num_merge_cand = 2U;
	wslice.slice_qp_delta = -5;
	wslice.cb_qp_offset = 3;
	wslice.cr_qp_offset = -4;
	wslice.loop_filter_across_slices = 1U;
	wctx.num_pic_total_curr = 5U;
	error = parse(H265SW_NAL_TRAIL_R, 1U, 0U, NO_CUT, NULL);
	CHECK(error == 0);
	CHECK(hdr.dependent == 0);
	CHECK(hdr.slice_addr_rs == 3U);
	CHECK(hdr.num_pic_total_curr == 5U);
	kept = hdr;

	/* A dependent segment copies every one of those and keeps its own address and place. */
	memset(&wslice, 0x6B, sizeof(wslice));
	wslice.first_in_pic = 0U;
	wslice.no_output_of_prior_pics = 0U;
	wslice.pps_id = 0U;
	wslice.dependent = 1U;
	wslice.address = 9U;
	wslice.extension_length = 0U;
	wslice.alignment_one = 1U;
	wslice.alignment_zero = 0U;
	error = parse(H265SW_NAL_TRAIL_R, 2U, 3U, NO_CUT, &kept);
	CHECK(error == 0);
	CHECK(hdr.dependent == 1);
	CHECK(hdr.address == 9U);
	CHECK(hdr.slice_addr_rs == 3U);
	CHECK(hdr.first_in_pic == 0);
	CHECK(hdr.temporal_id == 2U);
	CHECK(hdr.nal_type == H265SW_NAL_TRAIL_R);
	CHECK(hdr.data_offset == header_bytes);
	CHECK(hdr.num_entry_points == 0U);
	CHECK(hdr.offset_len == 0U);
	chain = hdr;
	hdr.dependent = kept.dependent;
	hdr.address = kept.address;
	hdr.temporal_id = kept.temporal_id;
	hdr.data_offset = kept.data_offset;
	CHECK(memcmp(&hdr.st_rps, &kept.st_rps, sizeof(hdr.st_rps)) == 0);
	CHECK(memcmp(&hdr.st_syntax, &kept.st_syntax, sizeof(hdr.st_syntax)) == 0);
	CHECK(memcmp(&hdr.weights, &kept.weights, sizeof(hdr.weights)) == 0);
	compare_headers(&kept, &hdr);

	/* A second dependent segment takes the independent fields from the dependent one (the chain keeps SliceAddrRs). */
	wslice.address = 12U;
	error = parse(H265SW_NAL_TRAIL_R, 2U, 0U, NO_CUT, &chain);
	CHECK(error == 0);
	CHECK(hdr.address == 12U);
	CHECK(hdr.slice_addr_rs == 3U);
	CHECK(hdr.slice_type == H265SW_SLICE_B);
	CHECK(hdr.poc_lsb == 77U);

	/* Without the independent header it is refused, and with one of another PPS. */
	CHECK(parse(H265SW_NAL_TRAIL_R, 2U, 0U, NO_CUT, NULL) == EINVAL);
	other = kept;
	other.pps_id = 1U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 2U, 0U, NO_CUT, &other) == EINVAL);

	/* A segment that is not dependent reads its own fields even when an independent header is passed. */
	fixture_default();
	wpps.dependent_slices = 1U;
	CHECK(apply() == 0);
	wslice.first_in_pic = 0U;
	wslice.dependent = 0U;
	wslice.address = 2U;
	wslice.slice_type = H265SW_SLICE_I;
	wslice.slice_qp_delta = 7;
	header_set(0U, 0U);
	error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, &kept);
	CHECK(error == 0);
	CHECK(hdr.dependent == 0);
	CHECK(hdr.slice_addr_rs == 2U);
	CHECK(hdr.slice_type == H265SW_SLICE_I);
	CHECK(hdr.qp == 33);
	CHECK(hdr.num_pic_total_curr == 0U);

	/* The first segment never has the flag, even with dependent slices enabled. */
	wslice.first_in_pic = 1U;
	wslice.dependent = 1U;
	wslice.address = 5U;
	error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
	CHECK(error == 0);
	CHECK(hdr.dependent == 0);
	CHECK(hdr.address == 0U);
	CHECK(hdr.slice_addr_rs == 0U);
}

/* The reserved flags of the slice header: 0 to 7 of them, every pattern. */
static void
check_reserved_flags(void)
{
	unsigned count;
	unsigned pattern;
	int error;

	/* num_extra_slice_header_bits 0..7 with every pattern of that many flags. */
	for (count = 0U; count <= 7U; count++) {
		for (pattern = 0U; pattern < ((unsigned)1U << count); pattern++) {
			fixture_default();
			wpps.num_extra_slice_header_bits = count;
			wslice.slice_type = H265SW_SLICE_I;
			CHECK(apply() == 0);
			wslice.reserved_flags = pattern | (0xFFU << count);
			error = parse(H265SW_NAL_IDR_N_LP, 0U, 0U, NO_CUT, NULL);
			CHECK(error == 0);
			CHECK(hdr.reserved_flags == pattern);
			CHECK(hdr.slice_type == H265SW_SLICE_I);
			CHECK(hdr.data_offset == header_bytes);
		}
	}
}

/* pic_output_flag, colour_plane_id and what a missing chroma array removes. */
static void
check_output_and_plane(void)
{
	unsigned present;
	unsigned value;
	unsigned plane;
	int error;

	/* The output flag is read only when the PPS says so; absent it is 1. */
	for (present = 0U; present < 2U; present++) {
		for (value = 0U; value < 2U; value++) {
			fixture_default();
			wpps.output_flag_present = present;
			CHECK(apply() == 0);
			wslice.pic_output = value;
			error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
			CHECK(error == 0);
			CHECK(hdr.pic_output == ((present != 0U) ? (int)value : 1));
			CHECK(hdr.data_offset == header_bytes);
		}
	}

	/* A 4:4:4 sequence with separate colour planes has a two-bit plane id of 0..2, and no chroma array. */
	for (plane = 0U; plane <= 3U; plane++) {
		fixture_default();
		wsps.chroma_format_idc = 3U;
		wsps.separate_colour_plane = 1U;
		wsps.sao = 1U;
		CHECK(apply() == 0);
		wslice.colour_plane_id = plane;
		wslice.sao_luma = 1U;
		wslice.sao_chroma = 1U;
		error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
		if (plane <= 2U) {
			CHECK(error == 0);
			CHECK(hdr.colour_plane_id == plane);
			CHECK(hdr.sao_luma == 1);
			CHECK(hdr.sao_chroma == 0);
			CHECK(hdr.data_offset == header_bytes);
		} else {
			CHECK(error == EINVAL);
		}
	}

	/* A 4:4:4 sequence without separate planes has no plane id and has the chroma SAO flag. */
	fixture_default();
	wsps.chroma_format_idc = 3U;
	wsps.separate_colour_plane = 0U;
	wsps.sao = 1U;
	CHECK(apply() == 0);
	wslice.colour_plane_id = 3U;
	wslice.sao_luma = 1U;
	wslice.sao_chroma = 1U;
	error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
	CHECK(error == 0);
	CHECK(hdr.colour_plane_id == 0U);
	CHECK(hdr.sao_chroma == 1);

	/* A monochrome sequence has no plane id and no chroma SAO flag. */
	fixture_default();
	wsps.chroma_format_idc = 0U;
	wsps.sao = 1U;
	CHECK(apply() == 0);
	wslice.sao_luma = 1U;
	wslice.sao_chroma = 1U;
	error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
	CHECK(error == 0);
	CHECK(hdr.sao_luma == 1);
	CHECK(hdr.sao_chroma == 0);
	CHECK(hdr.data_offset == header_bytes);
}

/* The short-term set taken from the SPS: the index width for several set counts. */
static void
check_sps_sets(void)
{
	static const unsigned counts[] = {1U, 2U, 3U, 4U, 5U, 33U, 64U};
	struct h265sw_st_rps want;
	struct h265sw_st_rps_syntax zero_syntax;
	unsigned index;
	unsigned count;
	unsigned set;
	unsigned bits;
	unsigned used;
	unsigned entry;
	int error;

	/* Each count of SPS sets; set i has one negative entry of delta -( i % 5 + 1 ) used when i is odd and one positive entry. */
	for (index = 0U; index < sizeof(counts) / sizeof(counts[0]); index++) {
		count = counts[index];
		bits = test_ceil_log2(count);
		for (set = 0U; set < (1U << bits) && set < 64U; set++) {
			fixture_default();
			wsps.num_st_rps = count;
			for (entry = 0U; entry < count; entry++) {
				memset(&wsps.st_rps[entry], 0, sizeof(wsps.st_rps[entry]));
				wsps.st_rps[entry].num_negative_pics = 1U;
				wsps.st_rps[entry].num_positive_pics = 1U;
				wsps.st_rps[entry].delta_poc_s0_minus1[0] = entry % 5U;
				wsps.st_rps[entry].used_by_curr_pic_s0_flag[0] = entry & 1U;
				wsps.st_rps[entry].delta_poc_s1_minus1[0] = (entry + 1U) % 4U;
				wsps.st_rps[entry].used_by_curr_pic_s1_flag[0] = 1U;
			}
			wsps.temporal_mvp = 0U;
			CHECK(apply() == 0);

			wslice.st_rps_sps = 1U;
			wslice.st_rps_idx = set;
			wslice.poc_lsb = 0x33U;
			wslice.reserved_flags = 0U;
			used = 1U + (set & 1U);
			wctx.num_pic_total_curr = used;
			error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
			if (set < count) {
				CHECK(error == 0);
				CHECK(hdr.st_rps_sps == 1);
				CHECK(hdr.st_rps_idx == set);
				CHECK(hdr.poc_lsb == 0x33U);
				set_explicit(&want, &wsps.st_rps[set]);
				CHECK(memcmp(&hdr.st_rps, &want, sizeof(want)) == 0);
				memset(&zero_syntax, 0, sizeof(zero_syntax));
				CHECK(memcmp(&hdr.st_syntax, &zero_syntax, sizeof(zero_syntax)) == 0);
				CHECK(hdr.num_pic_total_curr == used);
				CHECK(hdr.data_offset == header_bytes);
			} else {
				CHECK(error == EINVAL);
			}
		}
	}

	/* The flag set with no SPS sets is refused. */
	fixture_default();
	wsps.num_st_rps = 0U;
	CHECK(apply() == 0);
	wslice.st_rps_sps = 1U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
}

/* The short-term set the header carries: explicit, predicted from the SPS's sets, its bit count, and the bound of the DPB size. */
static void
check_header_sets(void)
{
	struct h265sw_st_rps want;
	unsigned index;
	int error;

	/*
	 * Explicit set in a header of an SPS without sets: negative entries
	 * -1 (used) and -6 (not used), positive +3 (used).  Bits: ue(2) 3,
	 * ue(1) 3, entry 1: ue(0) 1 + flag 1, entry 2: ue(4) 5 + 1, positive:
	 * ue(2) 3 + 1 = 18; with SPS sets the prediction flag adds one.
	 */
	for (index = 0U; index < 2U; index++) {
		fixture_default();
		wsps.num_st_rps = index;
		if (index == 1U) {
			wsps.st_rps[0].num_negative_pics = 0U;
			wsps.st_rps[0].num_positive_pics = 0U;
		}
		CHECK(apply() == 0);
		wslice.st_rps_sps = 0U;
		memset(&wslice.st_rps, 0, sizeof(wslice.st_rps));
		wslice.st_rps.num_negative_pics = 2U;
		wslice.st_rps.delta_poc_s0_minus1[0] = 0U;
		wslice.st_rps.used_by_curr_pic_s0_flag[0] = 1U;
		wslice.st_rps.delta_poc_s0_minus1[1] = 4U;
		wslice.st_rps.used_by_curr_pic_s0_flag[1] = 0U;
		wslice.st_rps.num_positive_pics = 1U;
		wslice.st_rps.delta_poc_s1_minus1[0] = 2U;
		wslice.st_rps.used_by_curr_pic_s1_flag[0] = 1U;
		wctx.num_pic_total_curr = 2U;
		error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
		CHECK(error == 0);
		CHECK(hdr.st_rps_sps == 0);
		CHECK(hdr.st_rps_idx == 0U);
		CHECK(hdr.st_rps.num_negative == 2U);
		CHECK(hdr.st_rps.num_positive == 1U);
		CHECK(hdr.st_rps.delta_poc[0] == -1);
		CHECK(hdr.st_rps.delta_poc[1] == -6);
		CHECK(hdr.st_rps.delta_poc[2] == 3);
		CHECK(hdr.st_rps.used[0] == 1U);
		CHECK(hdr.st_rps.used[1] == 0U);
		CHECK(hdr.st_rps.used[2] == 1U);
		CHECK(hdr.st_syntax.inter_rps_pred == 0);
		CHECK(hdr.st_syntax.num_negative_pics == 2U);
		CHECK(hdr.st_syntax.num_positive_pics == 1U);
		CHECK(hdr.st_syntax.delta_poc_s0_minus1[0] == 0U);
		CHECK(hdr.st_syntax.delta_poc_s0_minus1[1] == 4U);
		CHECK(hdr.st_syntax.delta_poc_s1_minus1[0] == 2U);
		CHECK(hdr.st_syntax.used_by_curr_pic_s0_flags == 1U);
		CHECK(hdr.st_syntax.used_by_curr_pic_s1_flags == 1U);
		CHECK(hdr.st_syntax.bits == 18U + index);
		CHECK(hdr.num_pic_total_curr == 2U);
		CHECK(hdr.data_offset == header_bytes);
	}

	/*
	 * Predicted from the SPS's set 0 = { -1 used, -3 not used, +2 used }
	 * (SPS sets: 2; the header's set is index 2, delta_idx_minus1 1 names
	 * set 0), deltaRps -1, flags j0 used, j1 not used with use_delta 1, j2
	 * used, j3 (deltaRps itself) used.  7-61: the negative list takes
	 * deltaRps -1 (flag j3) first, then the negative entries moved by -1:
	 * -2 (flag j0), -4 (flag j1): used 1, 1, 0.  7-62: the positive entry
	 * +2 - 1 = +1 (flag j2), used 1.  Bits: flag 1, ue(1) 3, sign 1,
	 * ue(0) 1, j0 1, j1 2, j2 1, j3 1 = 11.
	 */
	fixture_default();
	wsps.num_st_rps = 2U;
	memset(&wsps.st_rps[0], 0, sizeof(wsps.st_rps[0]));
	wsps.st_rps[0].num_negative_pics = 2U;
	wsps.st_rps[0].delta_poc_s0_minus1[0] = 0U;
	wsps.st_rps[0].used_by_curr_pic_s0_flag[0] = 1U;
	wsps.st_rps[0].delta_poc_s0_minus1[1] = 1U;
	wsps.st_rps[0].used_by_curr_pic_s0_flag[1] = 0U;
	wsps.st_rps[0].num_positive_pics = 1U;
	wsps.st_rps[0].delta_poc_s1_minus1[0] = 1U;
	wsps.st_rps[0].used_by_curr_pic_s1_flag[0] = 1U;
	memset(&wsps.st_rps[1], 0, sizeof(wsps.st_rps[1]));
	wsps.st_rps[1].num_negative_pics = 1U;
	wsps.st_rps[1].delta_poc_s0_minus1[0] = 1U;
	wsps.st_rps[1].used_by_curr_pic_s0_flag[0] = 1U;
	CHECK(apply() == 0);
	wslice.st_rps_sps = 0U;
	memset(&wslice.st_rps, 0, sizeof(wslice.st_rps));
	wslice.st_rps.inter_rps_pred = 1U;
	wslice.st_rps.delta_idx_minus1 = 1U;
	wslice.st_rps.delta_rps_sign = 1U;
	wslice.st_rps.abs_delta_rps_minus1 = 0U;
	wslice.st_rps.ref_num_delta_pocs = 3U;
	wslice.st_rps.used_by_curr_pic_flag[0] = 1U;
	wslice.st_rps.used_by_curr_pic_flag[1] = 0U;
	wslice.st_rps.use_delta_flag[1] = 1U;
	wslice.st_rps.used_by_curr_pic_flag[2] = 1U;
	wslice.st_rps.used_by_curr_pic_flag[3] = 1U;
	wctx.num_pic_total_curr = 3U;
	error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
	CHECK(error == 0);
	CHECK(hdr.st_rps.num_negative == 3U);
	CHECK(hdr.st_rps.num_positive == 1U);
	CHECK(hdr.st_rps.delta_poc[0] == -1);
	CHECK(hdr.st_rps.delta_poc[1] == -2);
	CHECK(hdr.st_rps.delta_poc[2] == -4);
	CHECK(hdr.st_rps.delta_poc[3] == 1);
	CHECK(hdr.st_rps.used[0] == 1U);
	CHECK(hdr.st_rps.used[1] == 1U);
	CHECK(hdr.st_rps.used[2] == 0U);
	CHECK(hdr.st_rps.used[3] == 1U);
	CHECK(hdr.st_syntax.inter_rps_pred == 1);
	CHECK(hdr.st_syntax.delta_idx_minus1 == 1U);
	CHECK(hdr.st_syntax.delta_rps_sign == 1);
	CHECK(hdr.st_syntax.abs_delta_rps_minus1 == 0U);
	CHECK(hdr.st_syntax.ref_num_delta_pocs == 3U);
	CHECK(hdr.st_syntax.used_by_curr_pic_flags == 13U);
	CHECK(hdr.st_syntax.use_delta_flags == 15U);
	CHECK(hdr.st_syntax.bits == 11U);
	CHECK(hdr.num_pic_total_curr == 3U);

	/*
	 * delta_idx_minus1 0 names set 1 = { -2 used }; deltaRps +1; flags j0
	 * not used with use_delta 1, j1 used.  Negative: -2 + 1 = -1 (flag
	 * j0, not used); positive: deltaRps +1 (flag j1, used).
	 */
	wslice.st_rps.delta_idx_minus1 = 0U;
	wslice.st_rps.delta_rps_sign = 0U;
	wslice.st_rps.abs_delta_rps_minus1 = 0U;
	wslice.st_rps.ref_num_delta_pocs = 1U;
	memset(wslice.st_rps.used_by_curr_pic_flag, 0, sizeof(wslice.st_rps.used_by_curr_pic_flag));
	memset(wslice.st_rps.use_delta_flag, 0, sizeof(wslice.st_rps.use_delta_flag));
	wslice.st_rps.used_by_curr_pic_flag[0] = 0U;
	wslice.st_rps.use_delta_flag[0] = 1U;
	wslice.st_rps.used_by_curr_pic_flag[1] = 1U;
	wctx.num_pic_total_curr = 1U;
	error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
	CHECK(error == 0);
	CHECK(hdr.st_rps.num_negative == 1U);
	CHECK(hdr.st_rps.num_positive == 1U);
	CHECK(hdr.st_rps.delta_poc[0] == -1);
	CHECK(hdr.st_rps.delta_poc[1] == 1);
	CHECK(hdr.st_rps.used[0] == 0U);
	CHECK(hdr.st_rps.used[1] == 1U);
	CHECK(hdr.st_syntax.delta_idx_minus1 == 0U);
	CHECK(hdr.st_syntax.ref_num_delta_pocs == 1U);
	CHECK(hdr.st_syntax.used_by_curr_pic_flags == 2U);
	CHECK(hdr.st_syntax.use_delta_flags == 3U);
	CHECK(hdr.num_pic_total_curr == 1U);

	/* delta_idx_minus1 at the number of SPS sets (a source that does not exist) is refused. */
	wslice.st_rps.delta_idx_minus1 = 2U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);

	/* The bound on the entries is the DPB size of the highest sub-layer: three entries fit 2 + 1 and not 1 + 1. */
	for (index = 0U; index < 4U; index++) {
		fixture_default();
		wsps.max_dec_pic_buffering_minus1[0] = index + 1U;
		CHECK(apply() == 0);
		header_set(3U, 0U);
		wctx.num_pic_total_curr = 3U;
		error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
		if (index + 1U >= 3U) {
			CHECK(error == 0);
		} else {
			CHECK(error == EINVAL);
		}
	}

	/* Three sub-layers: the highest one's DPB size (1 here) bounds it, not the lowest one's (8). */
	fixture_default();
	wsps.max_sub_layers_minus1 = 2U;
	wsps.sub_layer_ordering_info_present = 1U;
	wsps.max_dec_pic_buffering_minus1[0] = 8U;
	wsps.max_dec_pic_buffering_minus1[1] = 8U;
	wsps.max_dec_pic_buffering_minus1[2] = 1U;
	CHECK(apply() == 0);
	header_set(2U, 0U);
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
	header_set(1U, 0U);
	wctx.num_pic_total_curr = 1U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.st_rps.num_negative == 1U);
	set_explicit(&want, &wslice.st_rps);
	CHECK(memcmp(&hdr.st_rps, &want, sizeof(want)) == 0);
}

/* The long-term entries: counts, the index width for several SPS counts, copies from the SPS, and 7-52. */
static void
check_long_term(void)
{
	static const unsigned counts[] = {0U, 1U, 2U, 3U, 5U, 32U};
	unsigned index;
	unsigned count;
	unsigned bits;
	unsigned candidate;
	unsigned entry;
	int error;

	/* SPS counts of long-term candidates: each index, whose bit width is Ceil( Log2( count ) ). */
	for (index = 0U; index < sizeof(counts) / sizeof(counts[0]); index++) {
		count = counts[index];
		bits = test_ceil_log2(count);
		for (candidate = 0U; candidate < (1U << bits) || candidate == 0U; candidate++) {
			fixture_default();
			wsps.long_term_present = 1U;
			wsps.num_lt_sps = count;
			for (entry = 0U; entry < count; entry++) {
				wsps.lt_poc_lsb[entry] = (entry * 7U + 3U) & 0xFFU;
				wsps.lt_used[entry] = (entry & 1U) ^ 1U;
			}
			CHECK(apply() == 0);
			header_set(1U, 0U);
			memset(wslice.lt_idx_sps, 0, sizeof(wslice.lt_idx_sps));
			wslice.num_long_term_sps = (count > 0U) ? 1U : 0U;
			wslice.num_long_term_pics = 1U;
			wslice.lt_idx_sps[0] = candidate;
			wslice.poc_lsb_lt[wslice.num_long_term_sps] = 0xC9U;
			wslice.used_by_curr_pic_lt[wslice.num_long_term_sps] = 1U;
			wslice.delta_poc_msb_present[0] = 1U;
			wslice.delta_poc_msb_cycle_lt[0] = 2U;
			wctx.num_pic_total_curr = 1U + 1U + ((count > 0U) ? (unsigned)wsps.lt_used[candidate < count ? candidate : 0U] : 0U);
			error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
			if (count > 0U && candidate >= count) {
				CHECK(error == EINVAL);
				continue;
			}
			CHECK(error == 0);
			CHECK(hdr.num_long_term_sps == wslice.num_long_term_sps);
			CHECK(hdr.num_long_term_pics == 1U);
			CHECK(hdr.num_long_term == wslice.num_long_term_sps + 1U);
			CHECK(hdr.data_offset == header_bytes);
			if (count > 0U) {
				CHECK(hdr.lt_idx_sps[0] == candidate);
				CHECK(hdr.lt_poc_lsb[0] == wsps.lt_poc_lsb[candidate]);
				CHECK(hdr.lt_used[0] == wsps.lt_used[candidate]);
				CHECK(hdr.lt_msb_present[0] == 1U);
				CHECK(hdr.lt_msb_cycle[0] == 2U);
				CHECK(hdr.lt_poc_lsb[1] == 0xC9U);
				CHECK(hdr.lt_used[1] == 1U);
				CHECK(hdr.lt_msb_present[1] == 0U);
				CHECK(hdr.lt_msb_cycle[1] == 0U);
				CHECK(hdr.num_pic_total_curr == 2U + (unsigned)wsps.lt_used[candidate]);
			} else {
				CHECK(hdr.lt_poc_lsb[0] == 0xC9U);
				CHECK(hdr.lt_used[0] == 1U);
				CHECK(hdr.lt_msb_present[0] == 1U);
				CHECK(hdr.lt_msb_cycle[0] == 2U);
				CHECK(hdr.lt_idx_sps[0] == 0U);
				CHECK(hdr.num_pic_total_curr == 2U);
			}
		}
	}

	/* The 32 candidates of an SPS each read back their own lsb and use. */
	fixture_default();
	wsps.long_term_present = 1U;
	wsps.num_lt_sps = 32U;
	for (entry = 0U; entry < 32U; entry++) {
		wsps.lt_poc_lsb[entry] = entry * 5U + 1U;
		wsps.lt_used[entry] = (entry >> 1) & 1U;
	}
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.num_long_term_sps = 5U;
	wslice.num_long_term_pics = 0U;
	wslice.lt_idx_sps[0] = 31U;
	wslice.lt_idx_sps[1] = 0U;
	wslice.lt_idx_sps[2] = 17U;
	wslice.lt_idx_sps[3] = 2U;
	wslice.lt_idx_sps[4] = 30U;
	wctx.num_pic_total_curr = 1U + 0U + 0U + 1U + 1U;
	error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
	CHECK(error == 0);
	CHECK(hdr.lt_poc_lsb[0] == 31U * 5U + 1U);
	CHECK(hdr.lt_poc_lsb[1] == 1U);
	CHECK(hdr.lt_poc_lsb[2] == 17U * 5U + 1U);
	CHECK(hdr.lt_poc_lsb[3] == 2U * 5U + 1U);
	CHECK(hdr.lt_poc_lsb[4] == 30U * 5U + 1U);
	CHECK(hdr.lt_used[0] == 1U);
	CHECK(hdr.lt_used[1] == 0U);
	CHECK(hdr.lt_used[2] == 0U);
	CHECK(hdr.lt_used[3] == 1U);
	CHECK(hdr.lt_used[4] == 1U);
	CHECK(hdr.num_pic_total_curr == 3U);

	/* More entries from the SPS than the SPS has is refused. */
	fixture_default();
	wsps.long_term_present = 1U;
	wsps.num_lt_sps = 2U;
	wsps.lt_poc_lsb[0] = 1U;
	wsps.lt_poc_lsb[1] = 2U;
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.num_long_term_sps = 3U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
	wslice.num_long_term_sps = 2U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);

	/* An IDR picture has no long-term entries even when the SPS has them, and a sequence without them reads none. */
	wslice.num_long_term_sps = 0U;
	wslice.num_long_term_pics = 4U;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.num_long_term == 0U);
	fixture_default();
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.num_long_term_pics = 4U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.num_long_term == 0U);
	CHECK(hdr.num_long_term_pics == 0U);

	/*
	 * 7-52 by hand.  SPS candidates {lsb 10 used, lsb 20 not used}; the
	 * header takes both (index 1, 0) and two of its own.  Cycles with
	 * flags present 1, 1, 1, 0 and deltas 3, 2, 4, -: entry 0 = 3, entry 1
	 * = 2 + 3 = 5, entry 2 (the first of the header's own) = 4, entry 3
	 * (delta absent = 0) = 0 + 4 = 4.
	 */
	fixture_default();
	wsps.long_term_present = 1U;
	wsps.num_lt_sps = 2U;
	wsps.lt_poc_lsb[0] = 10U;
	wsps.lt_used[0] = 1U;
	wsps.lt_poc_lsb[1] = 20U;
	wsps.lt_used[1] = 0U;
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.num_long_term_sps = 2U;
	wslice.num_long_term_pics = 2U;
	wslice.lt_idx_sps[0] = 1U;
	wslice.lt_idx_sps[1] = 0U;
	wslice.poc_lsb_lt[2] = 33U;
	wslice.used_by_curr_pic_lt[2] = 1U;
	wslice.poc_lsb_lt[3] = 44U;
	wslice.used_by_curr_pic_lt[3] = 0U;
	wslice.delta_poc_msb_present[0] = 1U;
	wslice.delta_poc_msb_cycle_lt[0] = 3U;
	wslice.delta_poc_msb_present[1] = 1U;
	wslice.delta_poc_msb_cycle_lt[1] = 2U;
	wslice.delta_poc_msb_present[2] = 1U;
	wslice.delta_poc_msb_cycle_lt[2] = 4U;
	wslice.delta_poc_msb_present[3] = 0U;
	wslice.delta_poc_msb_cycle_lt[3] = 99U;
	wctx.num_pic_total_curr = 2U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.lt_msb_cycle[0] == 3U);
	CHECK(hdr.lt_msb_cycle[1] == 5U);
	CHECK(hdr.lt_msb_cycle[2] == 4U);
	CHECK(hdr.lt_msb_cycle[3] == 4U);
	CHECK(hdr.lt_msb_present[0] == 1U);
	CHECK(hdr.lt_msb_present[1] == 1U);
	CHECK(hdr.lt_msb_present[2] == 1U);
	CHECK(hdr.lt_msb_present[3] == 0U);
	CHECK(hdr.lt_poc_lsb[0] == 20U);
	CHECK(hdr.lt_poc_lsb[1] == 10U);
	CHECK(hdr.lt_poc_lsb[2] == 33U);
	CHECK(hdr.lt_poc_lsb[3] == 44U);
	CHECK(hdr.lt_used[0] == 0U);
	CHECK(hdr.lt_used[1] == 1U);
	CHECK(hdr.lt_used[2] == 1U);
	CHECK(hdr.lt_used[3] == 0U);
	CHECK(hdr.num_pic_total_curr == 2U);

	/* The first header-own entry restarts the sum even when its delta is absent: flags 1, 1, 0, 1 and deltas 3, 2, -, 7 give 3, 5, 0, 7. */
	wslice.delta_poc_msb_present[2] = 0U;
	wslice.delta_poc_msb_present[3] = 1U;
	wslice.delta_poc_msb_cycle_lt[3] = 7U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.lt_msb_cycle[0] == 3U);
	CHECK(hdr.lt_msb_cycle[1] == 5U);
	CHECK(hdr.lt_msb_cycle[2] == 0U);
	CHECK(hdr.lt_msb_cycle[3] == 7U);

	/* Without SPS entries the first entry starts the sum: flags 1, 0, 1 and deltas 2, -, 5 give 2, 2, 7. */
	fixture_default();
	wsps.long_term_present = 1U;
	wsps.num_lt_sps = 0U;
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.num_long_term_sps = 0U;
	wslice.num_long_term_pics = 3U;
	wslice.delta_poc_msb_present[0] = 1U;
	wslice.delta_poc_msb_cycle_lt[0] = 2U;
	wslice.delta_poc_msb_present[1] = 0U;
	wslice.delta_poc_msb_present[2] = 1U;
	wslice.delta_poc_msb_cycle_lt[2] = 5U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.lt_msb_cycle[0] == 2U);
	CHECK(hdr.lt_msb_cycle[1] == 2U);
	CHECK(hdr.lt_msb_cycle[2] == 7U);
}

/* The long-term limits: the sum with the short-term entries and the DeltaPocMsbCycleLt bound for several order count lengths. */
static void
check_long_term_limits(void)
{
	static const unsigned log2s[] = {4U, 8U, 12U, 16U};
	uint64_t bound;
	unsigned index;
	unsigned entries;
	unsigned negative;
	unsigned from_sps;
	int error;

	/* num_long_term_sps + num_long_term_pics + NumDeltaPocs is at most 15. */
	for (negative = 0U; negative <= 4U; negative++) {
		for (entries = 0U; entries <= 16U; entries++) {
			fixture_default();
			wsps.long_term_present = 1U;
			wsps.num_lt_sps = 0U;
			wsps.max_dec_pic_buffering_minus1[0] = 15U;
			CHECK(apply() == 0);
			header_set(negative, 0U);
			wslice.num_long_term_pics = entries;
			error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
			if (entries + negative <= 15U) {
				CHECK(error == 0);
				CHECK(hdr.num_long_term == entries);
			} else {
				CHECK(error == EINVAL);
			}
		}
	}

	/* The same with some entries from the SPS. */
	for (from_sps = 0U; from_sps <= 3U; from_sps++) {
		fixture_default();
		wsps.long_term_present = 1U;
		wsps.num_lt_sps = 3U;
		wsps.max_dec_pic_buffering_minus1[0] = 15U;
		CHECK(apply() == 0);
		header_set(2U, 0U);
		wslice.num_long_term_sps = from_sps;
		wslice.num_long_term_pics = 13U - from_sps;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
		wslice.num_long_term_pics = 14U - from_sps;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
	}

	/* A huge count cannot wrap the sum. */
	fixture_default();
	wsps.long_term_present = 1U;
	wsps.num_lt_sps = 0U;
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.num_long_term_pics = 0xFFFFFFFEU;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
	wslice.num_long_term_pics = 0xFFFFFFF0U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
	wsps.num_lt_sps = 32U;
	CHECK(apply() == 0);
	wslice.num_long_term_sps = 0xFFFFFFFEU;
	wslice.num_long_term_pics = 3U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);

	/* The cycle count and the sum of cycle counts are at most 2^( 32 - log2_max_pic_order_cnt_lsb ). */
	for (index = 0U; index < sizeof(log2s) / sizeof(log2s[0]); index++) {
		bound = (uint64_t)1U << (32U - log2s[index]);
		fixture_default();
		wsps.log2_max_poc_lsb_minus4 = log2s[index] - 4U;
		wsps.long_term_present = 1U;
		wsps.num_lt_sps = 0U;
		CHECK(apply() == 0);
		header_set(0U, 0U);
		wslice.num_long_term_pics = 2U;
		wslice.delta_poc_msb_present[0] = 1U;
		wslice.delta_poc_msb_present[1] = 1U;
		wslice.poc_lsb_lt[0] = 3U;
		wslice.poc_lsb_lt[1] = 4U;

		/* A single cycle count. */
		wslice.delta_poc_msb_cycle_lt[0] = (uint32_t)bound;
		wslice.delta_poc_msb_cycle_lt[1] = 0U;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
		CHECK(hdr.lt_msb_cycle[0] == (uint32_t)bound);
		CHECK(hdr.lt_msb_cycle[1] == (uint32_t)bound);
		wslice.delta_poc_msb_cycle_lt[0] = (uint32_t)bound + 1U;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);

		/* The sum of two. */
		wslice.delta_poc_msb_cycle_lt[0] = (uint32_t)bound - 1U;
		wslice.delta_poc_msb_cycle_lt[1] = 1U;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
		CHECK(hdr.lt_msb_cycle[1] == (uint32_t)bound);
		wslice.delta_poc_msb_cycle_lt[1] = 2U;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
		wslice.delta_poc_msb_cycle_lt[0] = (uint32_t)bound;
		wslice.delta_poc_msb_cycle_lt[1] = 1U;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);

		/* An absent entry between does not change the sum. */
		wslice.delta_poc_msb_present[1] = 0U;
		wslice.delta_poc_msb_cycle_lt[0] = (uint32_t)bound;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
		CHECK(hdr.lt_msb_cycle[1] == (uint32_t)bound);

		/* Two groups do not add: one cycle count from the SPS's group and one from the header's own are each at the bound. */
		wsps.num_lt_sps = 1U;
		wsps.lt_poc_lsb[0] = 5U;
		CHECK(apply() == 0);
		wslice.num_long_term_sps = 1U;
		wslice.num_long_term_pics = 1U;
		wslice.delta_poc_msb_present[0] = 1U;
		wslice.delta_poc_msb_present[1] = 1U;
		wslice.delta_poc_msb_cycle_lt[0] = (uint32_t)bound;
		wslice.delta_poc_msb_cycle_lt[1] = (uint32_t)bound;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
		CHECK(hdr.lt_msb_cycle[0] == (uint32_t)bound);
		CHECK(hdr.lt_msb_cycle[1] == (uint32_t)bound);
		wslice.delta_poc_msb_cycle_lt[1] = (uint32_t)bound + 1U;
		CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
	}
}

/* The temporal MVP flag and the SAO flags with their conditions. */
static void
check_tools(void)
{
	unsigned sps_tmvp;
	unsigned sps_sao;
	unsigned chroma;
	unsigned flag;
	unsigned sao_luma;
	unsigned sao_chroma;
	unsigned idr;
	int error;

	/* Every combination of the SPS's switches and the slice's flags, for an IDR picture and a TRAIL picture. */
	for (sps_tmvp = 0U; sps_tmvp < 2U; sps_tmvp++) {
		for (sps_sao = 0U; sps_sao < 2U; sps_sao++) {
			for (chroma = 0U; chroma < 2U; chroma++) {
				for (flag = 0U; flag < 2U; flag++) {
					for (idr = 0U; idr < 2U; idr++) {
						fixture_default();
						wsps.temporal_mvp = sps_tmvp;
						wsps.sao = sps_sao;
						wsps.chroma_format_idc = (chroma != 0U) ? 1U : 0U;
						CHECK(apply() == 0);
						header_set(0U, 0U);
						wslice.temporal_mvp = flag;
						wslice.sao_luma = flag;
						wslice.sao_chroma = flag ^ 1U;
						error = parse((idr != 0U) ? H265SW_NAL_IDR_W_RADL : H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
						CHECK(error == 0);
						CHECK(hdr.temporal_mvp == (int)((sps_tmvp != 0U && idr == 0U) ? flag : 0U));
						sao_luma = (sps_sao != 0U) ? flag : 0U;
						sao_chroma = (sps_sao != 0U && chroma != 0U) ? (flag ^ 1U) : 0U;
						CHECK(hdr.sao_luma == (int)sao_luma);
						CHECK(hdr.sao_chroma == (int)sao_chroma);
						CHECK(hdr.data_offset == header_bytes);
					}
				}
			}
		}
	}
}

/* The inter part: the list sizes from the PPS, the override, the 15-entry limit, and what each slice type has. */
static void
check_inter(void)
{
	unsigned override_flag;
	unsigned count0;
	unsigned count1;
	unsigned slice_type;
	int error;

	/* Defaults: PPS says 3 and 2; override says 5 and 7 (list 1 for B only). */
	for (slice_type = 0U; slice_type <= 1U; slice_type++) {
		for (override_flag = 0U; override_flag < 2U; override_flag++) {
			fixture_default();
			wpps.num_ref_idx_l0_default_minus1 = 2U;
			wpps.num_ref_idx_l1_default_minus1 = 1U;
			CHECK(apply() == 0);
			header_set(1U, 0U);
			wctx.num_pic_total_curr = 1U;
			wslice.slice_type = slice_type;
			wslice.num_ref_idx_override = override_flag;
			wslice.num_ref_idx_active_minus1[0] = 4U;
			wslice.num_ref_idx_active_minus1[1] = 6U;
			wslice.five_minus_max_num_merge_cand = 3U;
			error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
			CHECK(error == 0);
			CHECK(hdr.num_ref_idx[0] == ((override_flag != 0U) ? 5U : 3U));
			if (slice_type == H265SW_SLICE_B) {
				CHECK(hdr.num_ref_idx[1] == ((override_flag != 0U) ? 7U : 2U));
			} else {
				CHECK(hdr.num_ref_idx[1] == 0U);
			}
			CHECK(hdr.max_merge_cand == 2U);
			CHECK(hdr.data_offset == header_bytes);
		}
	}

	/* The limit: num_ref_idx_lX_active_minus1 up to 14 (15 pictures); 15 is refused; the PPS default up to 14 too. */
	for (count0 = 13U; count0 <= 16U; count0++) {
		for (count1 = 13U; count1 <= 16U; count1++) {
			fixture_default();
			CHECK(apply() == 0);
			header_set(1U, 0U);
			wctx.num_pic_total_curr = 1U;
			wslice.slice_type = H265SW_SLICE_B;
			wslice.num_ref_idx_override = 1U;
			wslice.num_ref_idx_active_minus1[0] = count0;
			wslice.num_ref_idx_active_minus1[1] = count1;
			error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
			if (count0 <= 14U && count1 <= 14U) {
				CHECK(error == 0);
				CHECK(hdr.num_ref_idx[0] == count0 + 1U);
				CHECK(hdr.num_ref_idx[1] == count1 + 1U);
			} else {
				CHECK(error == EINVAL);
			}
		}
	}
	fixture_default();
	wpps.num_ref_idx_l0_default_minus1 = 14U;
	wpps.num_ref_idx_l1_default_minus1 = 14U;
	CHECK(apply() == 0);
	header_set(1U, 0U);
	wctx.num_pic_total_curr = 1U;
	wslice.slice_type = H265SW_SLICE_B;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.num_ref_idx[0] == 15U);
	CHECK(hdr.num_ref_idx[1] == 15U);
}

/* ref_pic_lists_modification(): the condition on NumPicTotalCurr, the entry width and the entry range. */
static void
check_list_modification(void)
{
	static const unsigned totals[] = {1U, 2U, 3U, 4U, 5U, 8U, 9U, 10U};
	unsigned index;
	unsigned total;
	unsigned bits;
	unsigned present;
	unsigned slice_type;
	unsigned entry;
	unsigned list;
	int error;

	/* NumPicTotalCurr values from used short-term entries; flags and entries of P and B slices. */
	for (index = 0U; index < sizeof(totals) / sizeof(totals[0]); index++) {
		for (present = 0U; present < 2U; present++) {
			for (slice_type = 0U; slice_type <= 1U; slice_type++) {
				total = totals[index];
				bits = test_ceil_log2(total);
				fixture_default();
				wpps.lists_modification_present = present;
				CHECK(apply() == 0);
				header_set(total, 0U);
				wctx.num_pic_total_curr = total;
				wslice.slice_type = slice_type;
				wslice.num_ref_idx_override = 1U;
				wslice.num_ref_idx_active_minus1[0] = 2U;
				wslice.num_ref_idx_active_minus1[1] = 3U;
				wslice.list_mod[0] = 1U;
				wslice.list_mod[1] = 1U;
				for (entry = 0U; entry < 4U; entry++) {
					wslice.list_entry[0][entry] = (total - 1U) - entry % total;
					wslice.list_entry[1][entry] = (entry * 3U + 1U) % total;
				}
				wslice.mvd_l1_zero = 1U;

				error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
				CHECK(error == 0);
				CHECK(hdr.num_pic_total_curr == total);
				CHECK(hdr.data_offset == header_bytes);
				if (present != 0U && total > 1U) {
					CHECK(hdr.list_mod[0] == 1);
					for (entry = 0U; entry < 3U; entry++)
						CHECK(hdr.list_entry[0][entry] == wslice.list_entry[0][entry]);
					CHECK(hdr.list_entry[0][3] == 0U);
					if (slice_type == H265SW_SLICE_B) {
						CHECK(hdr.list_mod[1] == 1);
						for (entry = 0U; entry < 4U; entry++)
							CHECK(hdr.list_entry[1][entry] == wslice.list_entry[1][entry]);
					} else {
						CHECK(hdr.list_mod[1] == 0);
					}
				} else {
					CHECK(hdr.list_mod[0] == 0);
					CHECK(hdr.list_mod[1] == 0);
					CHECK(hdr.list_entry[0][0] == 0U);
				}
				CHECK(hdr.mvd_l1_zero == ((slice_type == H265SW_SLICE_B) ? 1 : 0));

				/* An entry equal to NumPicTotalCurr is refused when the width can hold it. */
				if (present != 0U && total > 1U && total < (1U << bits)) {
					for (list = 0U; list < 2U; list++) {
						wslice.list_entry[list][1] = total;
						error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
						if (list == 0U || slice_type == H265SW_SLICE_B) {
							CHECK(error == EINVAL);
						} else {
							CHECK(error == 0);
						}
						wslice.list_entry[list][1] = 0U;
					}
				}
			}
		}
	}

	/* One list modification flag per list: the flag of list 1 is for B slices only; a flag of 0 reads no entries. */
	fixture_default();
	wpps.lists_modification_present = 1U;
	CHECK(apply() == 0);
	header_set(4U, 0U);
	wctx.num_pic_total_curr = 4U;
	wslice.slice_type = H265SW_SLICE_B;
	wslice.num_ref_idx_override = 1U;
	wslice.num_ref_idx_active_minus1[0] = 1U;
	wslice.num_ref_idx_active_minus1[1] = 1U;
	wslice.list_mod[0] = 0U;
	wslice.list_mod[1] = 1U;
	wslice.list_entry[0][0] = 3U;
	wslice.list_entry[1][0] = 2U;
	wslice.list_entry[1][1] = 1U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.list_mod[0] == 0);
	CHECK(hdr.list_mod[1] == 1);
	CHECK(hdr.list_entry[0][0] == 0U);
	CHECK(hdr.list_entry[1][0] == 2U);
	CHECK(hdr.list_entry[1][1] == 1U);
}

/* collocated_from_l0_flag and collocated_ref_idx: when each is read, the inferred values, and the range. */
static void
check_collocated(void)
{
	static const unsigned sizes[] = {1U, 2U, 4U};
	unsigned size0;
	unsigned size1;
	unsigned from_l0;
	unsigned slice_type;
	unsigned ref_idx;
	unsigned count;
	unsigned tmvp;
	unsigned cabac_present;
	int error;

	/* Every pair of list sizes, both lists, with the flag and a range of indexes. */
	for (slice_type = 0U; slice_type <= 1U; slice_type++) {
		for (size0 = 0U; size0 < 3U; size0++) {
			for (size1 = 0U; size1 < 3U; size1++) {
				for (from_l0 = 0U; from_l0 < 2U; from_l0++) {
					for (ref_idx = 0U; ref_idx < 5U; ref_idx++) {
						fixture_default();
						wsps.temporal_mvp = 1U;
						CHECK(apply() == 0);
						header_set(1U, 0U);
						wctx.num_pic_total_curr = 1U;
						wslice.slice_type = slice_type;
						wslice.temporal_mvp = 1U;
						wslice.num_ref_idx_override = 1U;
						wslice.num_ref_idx_active_minus1[0] = sizes[size0] - 1U;
						wslice.num_ref_idx_active_minus1[1] = sizes[size1] - 1U;
						wslice.collocated_from_l0 = from_l0;
						wslice.collocated_ref_idx = ref_idx;
						error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);

						/* A P slice always uses list 0 and has no flag; a B slice's flag chooses. */
						count = sizes[size0];
						if (slice_type == H265SW_SLICE_B && from_l0 == 0U)
							count = sizes[size1];
						if (count > 1U && ref_idx >= count) {
							CHECK(error == EINVAL);
							continue;
						}
						CHECK(error == 0);
						CHECK(hdr.temporal_mvp == 1);
						CHECK(hdr.collocated_from_l0 == ((slice_type == H265SW_SLICE_B) ? (int)from_l0 : 1));
						CHECK(hdr.collocated_ref_idx == ((count > 1U) ? ref_idx : 0U));
						CHECK(hdr.data_offset == header_bytes);
					}
				}
			}
		}
	}

	/* Without temporal MVP nothing is read, and the CABAC flag follows the PPS. */
	for (tmvp = 0U; tmvp < 2U; tmvp++) {
		for (cabac_present = 0U; cabac_present < 2U; cabac_present++) {
			fixture_default();
			wsps.temporal_mvp = tmvp;
			wpps.cabac_init_present = cabac_present;
			CHECK(apply() == 0);
			header_set(1U, 0U);
			wctx.num_pic_total_curr = 1U;
			wslice.slice_type = H265SW_SLICE_B;
			wslice.temporal_mvp = 0U;
			wslice.cabac_init = 1U;
			wslice.collocated_from_l0 = 0U;
			wslice.collocated_ref_idx = 3U;
			error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
			CHECK(error == 0);
			CHECK(hdr.temporal_mvp == 0);
			CHECK(hdr.collocated_from_l0 == 1);
			CHECK(hdr.collocated_ref_idx == 0U);
			CHECK(hdr.cabac_init == (int)cabac_present);
		}
	}
}

/* The weights: hand-worked values for P and B, with and without a chroma array, and when each table is present. */
static void
check_weights(void)
{
	int error;

	/*
	 * P slice, 2 references, 4:2:0.  luma_log2_weight_denom 7, delta
	 * chroma denom -2 (ChromaLog2WeightDenom 5).  Luma flags {1, 0}, chroma
	 * flags {0, 1}.  Entry 0 luma: delta -128, offset -128: weight 128 -
	 * 128 = 0.  Entry 1 chroma, Cb: delta 127, delta offset -512: weight
	 * 32 + 127 = 159, offset = 128 - 512 - ( ( 128 * 159 ) >> 5 = 636 ) =
	 * -1020, clipped to -128.  Cr: delta -128, delta offset 511: weight 32
	 * - 128 = -96, offset = 128 + 511 - ( ( 128 * -96 ) >> 5 = -384 ) =
	 * 1023, clipped to 127.  Without flags: luma weight 128 offset 0,
	 * chroma weight 32 offset 0.
	 */
	fixture_default();
	wpps.weighted_pred = 1U;
	CHECK(apply() == 0);
	header_set(2U, 0U);
	wctx.num_pic_total_curr = 2U;
	wslice.slice_type = H265SW_SLICE_P;
	wslice.num_ref_idx_override = 1U;
	wslice.num_ref_idx_active_minus1[0] = 1U;
	wslice.luma_log2_weight_denom = 7U;
	wslice.delta_chroma_log2_weight_denom = -2;
	wslice.luma_weight_flag[0][0] = 1U;
	wslice.luma_weight_flag[0][1] = 0U;
	wslice.chroma_weight_flag[0][0] = 0U;
	wslice.chroma_weight_flag[0][1] = 1U;
	wslice.delta_luma_weight[0][0] = -128;
	wslice.luma_offset[0][0] = -128;
	wslice.delta_chroma_weight[0][1][0] = 127;
	wslice.delta_chroma_offset[0][1][0] = -512;
	wslice.delta_chroma_weight[0][1][1] = -128;
	wslice.delta_chroma_offset[0][1][1] = 511;
	wslice.five_minus_max_num_merge_cand = 0U;
	error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
	CHECK(error == 0);
	CHECK(hdr.weights.luma_log2_denom == 7U);
	CHECK(hdr.weights.chroma_log2_denom == 5U);
	CHECK(hdr.weights.luma_weight[0][0] == 0);
	CHECK(hdr.weights.luma_offset[0][0] == -128);
	CHECK(hdr.weights.luma_weight[0][1] == 128);
	CHECK(hdr.weights.luma_offset[0][1] == 0);
	CHECK(hdr.weights.chroma_weight[0][0][0] == 32);
	CHECK(hdr.weights.chroma_weight[0][0][1] == 32);
	CHECK(hdr.weights.chroma_offset[0][0][0] == 0);
	CHECK(hdr.weights.chroma_offset[0][0][1] == 0);
	CHECK(hdr.weights.chroma_weight[0][1][0] == 159);
	CHECK(hdr.weights.chroma_offset[0][1][0] == -128);
	CHECK(hdr.weights.chroma_weight[0][1][1] == -96);
	CHECK(hdr.weights.chroma_offset[0][1][1] == 127);
	CHECK(hdr.weights.luma_weight[0][2] == 0);
	CHECK(hdr.weights.luma_weight[1][0] == 0);
	CHECK(hdr.weights.chroma_weight[1][0][0] == 0);
	CHECK(hdr.max_merge_cand == 5U);
	CHECK(hdr.data_offset == header_bytes);

	/*
	 * Moderate values: luma denom 3, delta chroma denom +1 (4).  Luma
	 * flag 1 with delta 4, offset 5: weight 12, offset 5.  Chroma Cb:
	 * delta -3, delta offset 10: weight 16 - 3 = 13, offset = 128 + 10 -
	 * ( ( 128 * 13 ) >> 4 = 104 ) = 34.  Cr: delta 5, delta offset -7:
	 * weight 21, offset = 128 - 7 - ( 128 * 21 >> 4 = 168 ) = -47.
	 */
	wslice.luma_log2_weight_denom = 3U;
	wslice.delta_chroma_log2_weight_denom = 1;
	wslice.luma_weight_flag[0][0] = 1U;
	wslice.luma_weight_flag[0][1] = 0U;
	wslice.chroma_weight_flag[0][0] = 1U;
	wslice.chroma_weight_flag[0][1] = 0U;
	wslice.delta_luma_weight[0][0] = 4;
	wslice.luma_offset[0][0] = 5;
	wslice.delta_chroma_weight[0][0][0] = -3;
	wslice.delta_chroma_offset[0][0][0] = 10;
	wslice.delta_chroma_weight[0][0][1] = 5;
	wslice.delta_chroma_offset[0][0][1] = -7;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.luma_log2_denom == 3U);
	CHECK(hdr.weights.chroma_log2_denom == 4U);
	CHECK(hdr.weights.luma_weight[0][0] == 12);
	CHECK(hdr.weights.luma_offset[0][0] == 5);
	CHECK(hdr.weights.luma_weight[0][1] == 8);
	CHECK(hdr.weights.chroma_weight[0][0][0] == 13);
	CHECK(hdr.weights.chroma_offset[0][0][0] == 34);
	CHECK(hdr.weights.chroma_weight[0][0][1] == 21);
	CHECK(hdr.weights.chroma_offset[0][0][1] == -47);
	CHECK(hdr.weights.chroma_weight[0][1][0] == 16);
	CHECK(hdr.weights.chroma_weight[0][1][1] == 16);
	CHECK(hdr.weights.chroma_offset[0][1][0] == 0);

	/*
	 * Negative weights with denominator 0: luma delta -128 gives weight
	 * 1 - 128 = -127; chroma delta -100 gives -99, offset = 128 + 0 - (
	 * ( 128 * -99 ) >> 0 = -12672 ) = 12800, clipped to 127.  With
	 * denominator 2: weight 4 - 124 = -120 (delta -124), offset = 128 + 0
	 * - ( 128 * -120 >> 2 = -3840 ) = 3968, clipped to 127; and a weight
	 * of -1 at denominator 7 (delta -129 is out of range, so use denom 0
	 * weight -1 from delta -2): offset = 128 + -300 + 128 = -44.
	 */
	wslice.luma_log2_weight_denom = 0U;
	wslice.delta_chroma_log2_weight_denom = 0;
	wslice.luma_weight_flag[0][0] = 1U;
	wslice.delta_luma_weight[0][0] = -128;
	wslice.luma_offset[0][0] = 100;
	wslice.chroma_weight_flag[0][0] = 1U;
	wslice.delta_chroma_weight[0][0][0] = -100;
	wslice.delta_chroma_offset[0][0][0] = 0;
	wslice.delta_chroma_weight[0][0][1] = -2;
	wslice.delta_chroma_offset[0][0][1] = -300;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.luma_weight[0][0] == -127);
	CHECK(hdr.weights.luma_offset[0][0] == 100);
	CHECK(hdr.weights.chroma_weight[0][0][0] == -99);
	CHECK(hdr.weights.chroma_offset[0][0][0] == 127);
	CHECK(hdr.weights.chroma_weight[0][0][1] == -1);
	CHECK(hdr.weights.chroma_offset[0][0][1] == -44);
	wslice.luma_log2_weight_denom = 2U;
	wslice.delta_chroma_log2_weight_denom = 0;
	wslice.delta_chroma_weight[0][0][0] = -124;
	wslice.delta_chroma_offset[0][0][0] = 0;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.chroma_weight[0][0][0] == -120);
	CHECK(hdr.weights.chroma_offset[0][0][0] == 127);

	/* A weight of 0 at denominator 7 with an offset inside the range: offset = 128 + delta offset, here 128 - 200 = -72. */
	wslice.luma_log2_weight_denom = 7U;
	wslice.delta_chroma_log2_weight_denom = 0;
	wslice.delta_chroma_weight[0][0][0] = -128;
	wslice.delta_chroma_offset[0][0][0] = -200;
	wslice.delta_chroma_weight[0][0][1] = 127;
	wslice.delta_chroma_offset[0][0][1] = 0;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.chroma_weight[0][0][0] == 0);
	CHECK(hdr.weights.chroma_offset[0][0][0] == -72);

	/* A weight of 255 at denominator 7: offset = 128 + delta offset - 255, here delta offset 0 gives -127 and -5 gives -128 after the clip. */
	CHECK(hdr.weights.chroma_weight[0][0][1] == 255);
	CHECK(hdr.weights.chroma_offset[0][0][1] == -127);
	wslice.delta_chroma_offset[0][0][1] = -5;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.chroma_offset[0][0][1] == -128);

	/* A B slice reads both lists with their own sizes; the second list's values land in list 1. */
	fixture_default();
	wpps.weighted_bipred = 1U;
	CHECK(apply() == 0);
	header_set(3U, 0U);
	wctx.num_pic_total_curr = 3U;
	wslice.slice_type = H265SW_SLICE_B;
	wslice.num_ref_idx_override = 1U;
	wslice.num_ref_idx_active_minus1[0] = 1U;
	wslice.num_ref_idx_active_minus1[1] = 2U;
	wslice.luma_log2_weight_denom = 4U;
	wslice.delta_chroma_log2_weight_denom = 0;
	wslice.luma_weight_flag[0][1] = 1U;
	wslice.delta_luma_weight[0][1] = 3;
	wslice.luma_offset[0][1] = -4;
	wslice.chroma_weight_flag[1][2] = 1U;
	wslice.delta_chroma_weight[1][2][0] = 6;
	wslice.delta_chroma_offset[1][2][0] = 8;
	wslice.delta_chroma_weight[1][2][1] = -6;
	wslice.delta_chroma_offset[1][2][1] = -8;
	wslice.luma_weight_flag[1][0] = 1U;
	wslice.delta_luma_weight[1][0] = -9;
	wslice.luma_offset[1][0] = 9;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.luma_weight[0][0] == 16);
	CHECK(hdr.weights.luma_weight[0][1] == 19);
	CHECK(hdr.weights.luma_offset[0][1] == -4);
	CHECK(hdr.weights.luma_weight[0][2] == 0);
	CHECK(hdr.weights.luma_weight[1][0] == 7);
	CHECK(hdr.weights.luma_offset[1][0] == 9);
	CHECK(hdr.weights.luma_weight[1][1] == 16);
	CHECK(hdr.weights.luma_weight[1][2] == 16);
	CHECK(hdr.weights.chroma_weight[1][2][0] == 22);
	CHECK(hdr.weights.chroma_offset[1][2][0] == 128 + 8 - 176);
	CHECK(hdr.weights.chroma_weight[1][2][1] == 10);
	CHECK(hdr.weights.chroma_offset[1][2][1] == 128 - 8 - 80);
	CHECK(hdr.weights.chroma_weight[0][0][0] == 16);

	/* A P slice does not read the table when only weighted_bipred_flag is set, and a B slice not when only weighted_pred_flag is set. */
	fixture_default();
	wpps.weighted_pred = 0U;
	wpps.weighted_bipred = 1U;
	CHECK(apply() == 0);
	header_set(1U, 0U);
	wctx.num_pic_total_curr = 1U;
	wslice.slice_type = H265SW_SLICE_P;
	wslice.luma_log2_weight_denom = 3U;
	wslice.luma_weight_flag[0][0] = 1U;
	wslice.five_minus_max_num_merge_cand = 2U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.luma_log2_denom == 0U);
	CHECK(hdr.weights.luma_weight[0][0] == 0);
	CHECK(hdr.max_merge_cand == 3U);
	wpps.weighted_pred = 1U;
	wpps.weighted_bipred = 0U;
	CHECK(apply() == 0);
	wslice.slice_type = H265SW_SLICE_B;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.luma_log2_denom == 0U);
	CHECK(hdr.max_merge_cand == 3U);

	/* No chroma array: no chroma denominator difference, no chroma flags, no chroma values; the chroma denominator is the luma one. */
	fixture_default();
	wsps.chroma_format_idc = 0U;
	wpps.weighted_pred = 1U;
	CHECK(apply() == 0);
	header_set(1U, 0U);
	wctx.num_pic_total_curr = 1U;
	wslice.slice_type = H265SW_SLICE_P;
	wslice.luma_log2_weight_denom = 5U;
	wslice.delta_chroma_log2_weight_denom = -3;
	wslice.luma_weight_flag[0][0] = 1U;
	wslice.chroma_weight_flag[0][0] = 1U;
	wslice.delta_luma_weight[0][0] = 7;
	wslice.luma_offset[0][0] = 3;
	wslice.delta_chroma_weight[0][0][0] = 99;
	wslice.five_minus_max_num_merge_cand = 1U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.luma_log2_denom == 5U);
	CHECK(hdr.weights.chroma_log2_denom == 5U);
	CHECK(hdr.weights.luma_weight[0][0] == 39);
	CHECK(hdr.weights.luma_offset[0][0] == 3);
	CHECK(hdr.weights.chroma_weight[0][0][0] == 0);
	CHECK(hdr.weights.chroma_offset[0][0][0] == 0);
	CHECK(hdr.max_merge_cand == 4U);
	CHECK(hdr.data_offset == header_bytes);

	/* Separate colour planes have no chroma array either. */
	fixture_default();
	wsps.chroma_format_idc = 3U;
	wsps.separate_colour_plane = 1U;
	wpps.weighted_pred = 1U;
	CHECK(apply() == 0);
	header_set(1U, 0U);
	wctx.num_pic_total_curr = 1U;
	wslice.slice_type = H265SW_SLICE_P;
	wslice.luma_log2_weight_denom = 2U;
	wslice.luma_weight_flag[0][0] = 0U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.luma_weight[0][0] == 4);
	CHECK(hdr.weights.chroma_weight[0][0][0] == 0);
	CHECK(hdr.data_offset == header_bytes);
}

/* The weights' ranges and the high precision case. */
static void
check_weights_ranges(void)
{
	unsigned luma_denom;
	int delta_denom;
	int delta;
	int clipped;
	int error;
	int expect_ok;

	/* The luma denominator is 0..7; the chroma denominator luma + delta is 0..7. */
	for (luma_denom = 0U; luma_denom <= 9U; luma_denom++) {
		for (delta_denom = -9; delta_denom <= 9; delta_denom++) {
			fixture_default();
			wpps.weighted_pred = 1U;
			CHECK(apply() == 0);
			header_set(1U, 0U);
			wctx.num_pic_total_curr = 1U;
			wslice.slice_type = H265SW_SLICE_P;
			wslice.luma_log2_weight_denom = luma_denom;
			wslice.delta_chroma_log2_weight_denom = delta_denom;
			error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
			expect_ok = (luma_denom <= 7U) && ((int)luma_denom + delta_denom >= 0) && ((int)luma_denom + delta_denom <= 7);
			if (expect_ok != 0) {
				CHECK(error == 0);
				CHECK(hdr.weights.chroma_log2_denom == (unsigned)((int)luma_denom + delta_denom));
			} else {
				CHECK(error == EINVAL);
			}
		}
	}

	/* A huge difference cannot wrap the sum. */
	fixture_default();
	wpps.weighted_pred = 1U;
	CHECK(apply() == 0);
	header_set(1U, 0U);
	wctx.num_pic_total_curr = 1U;
	wslice.slice_type = H265SW_SLICE_P;
	wslice.luma_log2_weight_denom = 3U;
	wslice.delta_chroma_log2_weight_denom = 0x7FFFFFFF;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
	wslice.delta_chroma_log2_weight_denom = -0x7FFFFFFF;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);

	/* The deltas of the weights are -128..127, the luma offset -128..127, the chroma offset delta -512..511. */
	for (delta = -130; delta <= 130; delta++) {
		fixture_default();
		wpps.weighted_pred = 1U;
		CHECK(apply() == 0);
		header_set(1U, 0U);
		wctx.num_pic_total_curr = 1U;
		wslice.slice_type = H265SW_SLICE_P;
		wslice.luma_log2_weight_denom = 6U;
		wslice.luma_weight_flag[0][0] = 1U;
		wslice.delta_luma_weight[0][0] = delta;
		wslice.luma_offset[0][0] = 0;
		error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
		if (delta >= -128 && delta <= 127) {
			CHECK(error == 0);
			CHECK(hdr.weights.luma_weight[0][0] == 64 + delta);
		} else {
			CHECK(error == EINVAL);
		}

		wslice.delta_luma_weight[0][0] = 0;
		wslice.luma_offset[0][0] = delta;
		error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
		if (delta >= -128 && delta <= 127) {
			CHECK(error == 0);
			CHECK(hdr.weights.luma_offset[0][0] == delta);
		} else {
			CHECK(error == EINVAL);
		}

		wslice.luma_offset[0][0] = 0;
		wslice.luma_weight_flag[0][0] = 0U;
		wslice.chroma_weight_flag[0][0] = 1U;
		wslice.delta_chroma_weight[0][0][0] = delta;
		wslice.delta_chroma_weight[0][0][1] = 0;
		error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
		if (delta >= -128 && delta <= 127) {
			CHECK(error == 0);
			CHECK(hdr.weights.chroma_weight[0][0][0] == 64 + delta);
		} else {
			CHECK(error == EINVAL);
		}
		wslice.delta_chroma_weight[0][0][0] = 0;
		wslice.delta_chroma_weight[0][0][1] = delta;
		error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
		if (delta >= -128 && delta <= 127) {
			CHECK(error == 0);
			CHECK(hdr.weights.chroma_weight[0][0][1] == 64 + delta);
		} else {
			CHECK(error == EINVAL);
		}
	}
	for (delta = -516; delta <= 516; delta++) {
		fixture_default();
		wpps.weighted_pred = 1U;
		CHECK(apply() == 0);
		header_set(1U, 0U);
		wctx.num_pic_total_curr = 1U;
		wslice.slice_type = H265SW_SLICE_P;
		wslice.luma_log2_weight_denom = 6U;
		wslice.chroma_weight_flag[0][0] = 1U;
		wslice.delta_chroma_offset[0][0][0] = delta;
		wslice.delta_chroma_offset[0][0][1] = 0;
		error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);

		/* The weight is the unit, so the offset is the delta offset, clipped to -128..127 (7-58). */
		clipped = delta;
		if (clipped < -128)
			clipped = -128;
		if (clipped > 127)
			clipped = 127;
		if (delta >= -512 && delta <= 511) {
			CHECK(error == 0);
			CHECK(hdr.weights.chroma_offset[0][0][0] == clipped);
		} else {
			CHECK(error == EINVAL);
		}
		wslice.delta_chroma_offset[0][0][0] = 0;
		wslice.delta_chroma_offset[0][0][1] = delta;
		error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
		if (delta >= -512 && delta <= 511) {
			CHECK(error == 0);
		} else {
			CHECK(error == EINVAL);
		}
	}

	/* The high precision offsets range is not taken: a header with weights is ENOTSUP, one without is read. */
	fixture_default();
	wsps.extension_present = 1U;
	wsps.range_extension_flag = 1U;
	wsps.range_extension = 1U << 6;
	wpps.weighted_pred = 1U;
	wpps.weighted_bipred = 1U;
	CHECK(apply() == 0);
	header_set(1U, 0U);
	wctx.num_pic_total_curr = 1U;
	wslice.slice_type = H265SW_SLICE_P;
	wslice.five_minus_max_num_merge_cand = 0U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == ENOTSUP);
	wslice.slice_type = H265SW_SLICE_B;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == ENOTSUP);
	wslice.slice_type = H265SW_SLICE_I;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	wpps.weighted_pred = 0U;
	wpps.weighted_bipred = 0U;
	CHECK(apply() == 0);
	wslice.slice_type = H265SW_SLICE_P;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);

	/* Any other range extension flag does not matter for the weights. */
	wsps.range_extension = 0x1BFU;
	wpps.weighted_pred = 1U;
	CHECK(apply() == 0);
	wslice.slice_type = H265SW_SLICE_P;
	wslice.luma_log2_weight_denom = 2U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.weights.luma_weight[0][0] == 4);
}

/* five_minus_max_num_merge_cand and SliceQpY. */
static void
check_merge_and_qp(void)
{
	static const unsigned depths[] = {0U, 2U, 4U, 8U};
	unsigned merge;
	unsigned index;
	int init_minus26;
	int delta;
	int qp;
	int low;
	int error;

	/* MaxNumMergeCand = 5 - the element, which is 0..4. */
	for (merge = 0U; merge <= 8U; merge++) {
		fixture_default();
		CHECK(apply() == 0);
		header_set(1U, 0U);
		wctx.num_pic_total_curr = 1U;
		wslice.slice_type = H265SW_SLICE_P;
		wslice.five_minus_max_num_merge_cand = merge;
		error = parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL);
		if (merge <= 4U) {
			CHECK(error == 0);
			CHECK(hdr.max_merge_cand == 5U - merge);
		} else {
			CHECK(error == EINVAL);
		}
	}
	fixture_default();
	CHECK(apply() == 0);
	header_set(1U, 0U);
	wctx.num_pic_total_curr = 1U;
	wslice.slice_type = H265SW_SLICE_P;
	wslice.five_minus_max_num_merge_cand = 0xFFFFFFFEU;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);

	/* An I slice has no merge candidates to read. */
	wslice.slice_type = H265SW_SLICE_I;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.max_merge_cand == 0U);

	/* SliceQpY = 26 + init_qp_minus26 + slice_qp_delta, in -QpBdOffsetY..51, for bit depths 8, 10, 12 and 16. */
	for (index = 0U; index < sizeof(depths) / sizeof(depths[0]); index++) {
		low = -6 * (int)depths[index];
		for (init_minus26 = -(26 + 6 * (int)depths[index]); init_minus26 <= 25; init_minus26 += 17) {
			fixture_default();
			wsps.bit_depth_luma_minus8 = depths[index];
			wsps.bit_depth_chroma_minus8 = depths[index];
			wpps.init_qp_minus26 = init_minus26;
			CHECK(apply() == 0);
			header_set(0U, 0U);
			for (delta = -80; delta <= 80; delta++) {
				wslice.slice_qp_delta = delta;
				qp = 26 + init_minus26 + delta;
				error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
				if (qp >= low && qp <= 51) {
					CHECK(error == 0);
					CHECK(hdr.qp == qp);
				} else {
					CHECK(error == EINVAL);
				}
			}
		}
	}

	/* A huge delta cannot wrap. */
	fixture_default();
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.slice_qp_delta = 0x7FFFFFFF;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == EINVAL);
	wslice.slice_qp_delta = -0x7FFFFFFF;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == EINVAL);
}

/* The chroma QP offsets of the slice and their sum with the PPS's. */
static void
check_chroma_offsets(void)
{
	static const int pps_values[] = {-12, -5, 0, 5, 12};
	unsigned index;
	int slice_value;
	int error;
	int ok_cb;
	int ok_cr;

	/* Each pair of PPS offsets and slice offsets: valid when each is within 12 and the sums are too. */
	for (index = 0U; index < sizeof(pps_values) / sizeof(pps_values[0]); index++) {
		for (slice_value = -26; slice_value <= 26; slice_value++) {
			fixture_default();
			wpps.slice_chroma_qp_offsets_present = 1U;
			wpps.cb_qp_offset = pps_values[index];
			wpps.cr_qp_offset = -pps_values[index];
			CHECK(apply() == 0);
			header_set(0U, 0U);
			wslice.cb_qp_offset = slice_value;
			wslice.cr_qp_offset = slice_value;
			error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
			ok_cb = (slice_value >= -12 && slice_value <= 12 && pps_values[index] + slice_value >= -12 && pps_values[index] + slice_value <= 12);
			ok_cr = (slice_value >= -12 && slice_value <= 12 && -pps_values[index] + slice_value >= -12 && -pps_values[index] + slice_value <= 12);
			if (ok_cb != 0 && ok_cr != 0) {
				CHECK(error == 0);
				CHECK(hdr.cb_qp_offset == slice_value);
				CHECK(hdr.cr_qp_offset == slice_value);
			} else {
				CHECK(error == EINVAL);
			}

			/* Each offset alone: the other 0. */
			wslice.cb_qp_offset = slice_value;
			wslice.cr_qp_offset = 0;
			error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
			CHECK((error == 0) == (ok_cb != 0 && (-pps_values[index] >= -12 && -pps_values[index] <= 12)));
			wslice.cb_qp_offset = 0;
			wslice.cr_qp_offset = slice_value;
			error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
			CHECK((error == 0) == (ok_cr != 0));
		}
	}

	/* Without the PPS flag no offsets are read, and a huge value cannot wrap. */
	fixture_default();
	wpps.slice_chroma_qp_offsets_present = 0U;
	wpps.cb_qp_offset = 4;
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.cb_qp_offset = 11;
	wslice.cr_qp_offset = -11;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.cb_qp_offset == 0);
	CHECK(hdr.cr_qp_offset == 0);
	wpps.slice_chroma_qp_offsets_present = 1U;
	CHECK(apply() == 0);
	wslice.cb_qp_offset = 0x7FFFFFFF;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == EINVAL);
	wslice.cb_qp_offset = -0x7FFFFFFF;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == EINVAL);
}

/* The deblocking override, its offsets, and slice_loop_filter_across_slices_enabled_flag with its reading condition. */
static void
check_deblocking(void)
{
	unsigned override_enabled;
	unsigned pps_disabled;
	unsigned override_flag;
	unsigned slice_disabled;
	unsigned lf_pps;
	unsigned sao_luma;
	unsigned sao_chroma;
	unsigned control;
	int offset;
	int error;
	int disabled;
	int read;

	/* Every combination of the PPS's switches and the slice's override. */
	for (control = 0U; control < 2U; control++) {
		for (override_enabled = 0U; override_enabled < 2U; override_enabled++) {
			for (pps_disabled = 0U; pps_disabled < 2U; pps_disabled++) {
				for (override_flag = 0U; override_flag < 2U; override_flag++) {
					for (slice_disabled = 0U; slice_disabled < 2U; slice_disabled++) {
						fixture_default();
						wpps.deblocking_control_present = control;
						wpps.deblocking_override_enabled = override_enabled;
						wpps.deblocking_disabled = pps_disabled;
						wpps.beta_offset_div2 = (pps_disabled != 0U) ? 0 : 3;
						wpps.tc_offset_div2 = (pps_disabled != 0U) ? 0 : -2;
						wpps.loop_filter_across_slices = 0U;
						CHECK(apply() == 0);
						header_set(0U, 0U);
						wslice.deblocking_override = override_flag;
						wslice.deblocking_disabled = slice_disabled;
						wslice.beta_offset_div2 = -6;
						wslice.tc_offset_div2 = 5;
						error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
						CHECK(error == 0);
						CHECK(hdr.data_offset == header_bytes);

						/* Without the control the PPS's values are the default 0; the override flag is read only when enabled. */
						if (control == 0U) {
							CHECK(hdr.deblocking_override == 0);
							CHECK(hdr.deblocking_disabled == 0);
							CHECK(hdr.beta_offset_div2 == 0);
							CHECK(hdr.tc_offset_div2 == 0);
							continue;
						}
						if (override_enabled == 0U || override_flag == 0U) {
							CHECK(hdr.deblocking_override == 0);
							CHECK(hdr.deblocking_disabled == (int)pps_disabled);
							CHECK(hdr.beta_offset_div2 == wpps.beta_offset_div2);
							CHECK(hdr.tc_offset_div2 == wpps.tc_offset_div2);
						} else {
							CHECK(hdr.deblocking_override == 1);
							CHECK(hdr.deblocking_disabled == (int)slice_disabled);
							if (slice_disabled == 0U) {
								CHECK(hdr.beta_offset_div2 == -6);
								CHECK(hdr.tc_offset_div2 == 5);
							} else {
								CHECK(hdr.beta_offset_div2 == wpps.beta_offset_div2);
								CHECK(hdr.tc_offset_div2 == wpps.tc_offset_div2);
							}
						}
					}
				}
			}
		}
	}

	/* The offsets: -6..6, each alone out of range. */
	for (offset = -9; offset <= 9; offset++) {
		fixture_default();
		wpps.deblocking_control_present = 1U;
		wpps.deblocking_override_enabled = 1U;
		CHECK(apply() == 0);
		header_set(0U, 0U);
		wslice.deblocking_override = 1U;
		wslice.deblocking_disabled = 0U;
		wslice.beta_offset_div2 = offset;
		wslice.tc_offset_div2 = 0;
		error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
		if (offset >= -6 && offset <= 6) {
			CHECK(error == 0);
			CHECK(hdr.beta_offset_div2 == offset);
		} else {
			CHECK(error == EINVAL);
		}
		wslice.beta_offset_div2 = 0;
		wslice.tc_offset_div2 = offset;
		error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
		if (offset >= -6 && offset <= 6) {
			CHECK(error == 0);
			CHECK(hdr.tc_offset_div2 == offset);
		} else {
			CHECK(error == EINVAL);
		}
	}
	fixture_default();
	wpps.deblocking_control_present = 1U;
	wpps.deblocking_override_enabled = 1U;
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.deblocking_override = 1U;
	wslice.beta_offset_div2 = 0x7FFFFFFF;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == EINVAL);

	/* The loop filter flag is read only with the PPS flag and an in-loop filter running; otherwise it is the PPS's. */
	for (lf_pps = 0U; lf_pps < 2U; lf_pps++) {
		for (sao_luma = 0U; sao_luma < 2U; sao_luma++) {
			for (sao_chroma = 0U; sao_chroma < 2U; sao_chroma++) {
				for (disabled = 0; disabled < 2; disabled++) {
					fixture_default();
					wsps.sao = 1U;
					wpps.deblocking_control_present = 1U;
					wpps.deblocking_disabled = (unsigned)disabled;
					wpps.loop_filter_across_slices = lf_pps;
					CHECK(apply() == 0);
					header_set(0U, 0U);
					wslice.sao_luma = sao_luma;
					wslice.sao_chroma = sao_chroma;
					wslice.loop_filter_across_slices = 1U - lf_pps;
					error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
					CHECK(error == 0);
					read = (lf_pps != 0U) && (sao_luma != 0U || sao_chroma != 0U || disabled == 0);
					if (read != 0) {
						CHECK(hdr.loop_filter_across_slices == (int)(1U - lf_pps));
					} else {
						CHECK(hdr.loop_filter_across_slices == (int)lf_pps);
					}
					CHECK(hdr.data_offset == header_bytes);
				}
			}
		}
	}

	/* A slice that overrides to disabled has the flag read only when SAO runs. */
	fixture_default();
	wsps.sao = 1U;
	wpps.deblocking_control_present = 1U;
	wpps.deblocking_override_enabled = 1U;
	wpps.loop_filter_across_slices = 1U;
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.deblocking_override = 1U;
	wslice.deblocking_disabled = 1U;
	wslice.sao_luma = 0U;
	wslice.sao_chroma = 0U;
	wslice.loop_filter_across_slices = 0U;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.loop_filter_across_slices == 1);
	wslice.sao_chroma = 1U;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.loop_filter_across_slices == 0);
	CHECK(hdr.deblocking_disabled == 1);
}

/* The entry points: the three bounds, the offset length, and what dependent segments read. */
static void
check_entry_points(void)
{
	unsigned mode;
	unsigned count;
	unsigned columns;
	unsigned rows;
	unsigned limit;
	unsigned height_ctbs;
	unsigned length;
	int error;

	/* WPP only, tiles only, both: the bound of 7.4.7.1 against a count one over. */
	for (mode = 1U; mode <= 3U; mode++) {
		fixture_default();
		wsps.width = 16U * 8U;
		wsps.height = 16U * 6U;
		wpps.entropy_sync = (mode & 1U) ? 1U : 0U;
		wpps.tiles = (mode & 2U) ? 1U : 0U;
		wpps.num_tile_columns_minus1 = 2U;
		wpps.num_tile_rows_minus1 = 1U;
		wpps.uniform_spacing = 1U;
		CHECK(apply() == 0);
		columns = 3U;
		rows = 2U;
		height_ctbs = 6U;

		/* 3 * 2 tiles give 5; 6 rows of CTBs give 5; the product of columns and CTB rows gives 17. */
		limit = 5U;
		if (mode == 1U)
			limit = height_ctbs - 1U;
		if (mode == 2U)
			limit = columns * rows - 1U;
		if (mode == 3U)
			limit = columns * height_ctbs - 1U;
		for (count = 0U; count <= limit + 3U; count++) {
			header_set(0U, 0U);
			wslice.num_entry_point_offsets = count;
			wslice.offset_len_minus1 = 11U;
			wslice.entry_point_offset_minus1 = 0xABCU;
			error = parse(H265SW_NAL_IDR_W_RADL, 0U, 2U, NO_CUT, NULL);
			if (count <= limit) {
				CHECK(error == 0);
				CHECK(hdr.num_entry_points == count);
				CHECK(hdr.offset_len == ((count > 0U) ? 12U : 0U));
				CHECK(hdr.data_offset == header_bytes);
			} else {
				CHECK(error == EINVAL);
			}
		}

		/* The offset length: 1..32 bits; 33 is refused. */
		for (length = 0U; length <= 34U; length++) {
			header_set(0U, 0U);
			wslice.num_entry_point_offsets = 2U;
			wslice.offset_len_minus1 = length;
			wslice.entry_point_offset_minus1 = 0xFFFFFFFFU >> (31U - (length & 31U));
			error = parse(H265SW_NAL_IDR_W_RADL, 0U, 1U, NO_CUT, NULL);
			if (length <= 31U) {
				CHECK(error == 0);
				CHECK(hdr.offset_len == length + 1U);
				CHECK(hdr.num_entry_points == 2U);
				CHECK(hdr.data_offset == header_bytes);
			} else {
				CHECK(error == EINVAL);
			}
		}
	}

	/* The largest picture the parse takes: 20 columns of 22 rows of tiles; 439 entry points with both, 439 + 1 refused. */
	fixture_default();
	wsps.width = 16U * 20U;
	wsps.height = 16U * 22U;
	wpps.entropy_sync = 1U;
	wpps.tiles = 1U;
	wpps.num_tile_columns_minus1 = 19U;
	wpps.num_tile_rows_minus1 = 21U;
	wpps.uniform_spacing = 1U;
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.num_entry_point_offsets = 439U;
	wslice.offset_len_minus1 = 7U;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.num_entry_points == 439U);
	wslice.num_entry_point_offsets = 440U;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == EINVAL);

	/* A huge count is refused before any offsets are read. */
	wslice.num_entry_point_offsets = 0xFFFFFFFEU;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == EINVAL);

	/* A PPS with neither reads none, whatever the writer holds. */
	fixture_default();
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.num_entry_point_offsets = 3U;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.num_entry_points == 0U);
	CHECK(hdr.offset_len == 0U);

	/* A dependent segment has its own entry points. */
	fixture_default();
	wpps.dependent_slices = 1U;
	wpps.entropy_sync = 1U;
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.num_entry_point_offsets = 1U;
	wslice.offset_len_minus1 = 3U;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
	kept = hdr;
	memset(&wslice, 0, sizeof(wslice));
	wslice.first_in_pic = 0U;
	wslice.dependent = 1U;
	wslice.address = 5U;
	wslice.num_entry_point_offsets = 2U;
	wslice.offset_len_minus1 = 15U;
	wslice.alignment_one = 1U;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, &kept) == 0);
	CHECK(hdr.dependent == 1);
	CHECK(hdr.num_entry_points == 2U);
	CHECK(hdr.offset_len == 16U);
	CHECK(hdr.data_offset == header_bytes);
}

/* The header extension: lengths 0, 1, 256, and 257 (refused), and the data offset after it. */
static void
check_extension(void)
{
	static const unsigned lengths[] = {0U, 1U, 2U, 7U, 255U, 256U, 257U, 300U, 100000U};
	unsigned index;
	unsigned length;
	unsigned entry;
	int error;

	/* With the PPS flag: the length and its bytes are skipped; without it the field is not there. */
	for (index = 0U; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
		length = lengths[index];
		fixture_default();
		wpps.slice_header_extension_present = 1U;
		CHECK(apply() == 0);
		header_set(0U, 0U);
		wslice.extension_length = length;
		for (entry = 0U; entry < H265W_MAX_EXTENSION; entry++)
			wslice.extension_data[entry] = (uint8_t)(entry * 13U + 1U);
		error = parse(H265SW_NAL_IDR_W_RADL, 0U, 6U, NO_CUT, NULL);
		if (length <= 256U) {
			CHECK(error == 0);
			CHECK(hdr.data_offset == header_bytes);
		} else {
			CHECK(error == EINVAL);
		}

		wpps.slice_header_extension_present = 0U;
		CHECK(apply() == 0);
		error = parse(H265SW_NAL_IDR_W_RADL, 0U, 6U, NO_CUT, NULL);
		CHECK(error == 0);
		CHECK(hdr.data_offset == header_bytes);
	}

	/* The extension comes after the entry points and before the alignment. */
	fixture_default();
	wpps.slice_header_extension_present = 1U;
	wpps.entropy_sync = 1U;
	CHECK(apply() == 0);
	header_set(0U, 0U);
	wslice.num_entry_point_offsets = 2U;
	wslice.offset_len_minus1 = 4U;
	wslice.entry_point_offset_minus1 = 0x15U;
	wslice.extension_length = 3U;
	wslice.extension_data[0] = 0xFFU;
	wslice.extension_data[1] = 0x00U;
	wslice.extension_data[2] = 0x81U;
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
	CHECK(hdr.num_entry_points == 2U);
	CHECK(hdr.offset_len == 5U);
	CHECK(hdr.data_offset == header_bytes);
}

/* byte_alignment(): a bad one bit and a bad padding bit, with every padding length, and the data offset. */
static void
check_alignment(void)
{
	static const uint8_t data_bytes[] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
	unsigned extra_bits;
	unsigned padding;
	unsigned seen;
	uint8_t valid[16];
	size_t valid_size;
	int error;

	/* num_extra_slice_header_bits 0..7 shifts the end of the header, so the padding runs through 0..7 bits. */
	seen = 0U;
	for (extra_bits = 0U; extra_bits <= 7U; extra_bits++) {
		fixture_default();
		wpps.num_extra_slice_header_bits = extra_bits;
		CHECK(apply() == 0);
		header_set(0U, 0U);
		CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
		CHECK(hdr.data_offset == header_bytes);
		valid_size = header_bytes;
		memcpy(valid, swriter.data, valid_size);

		/* The padding is the zero bits after the last one bit of the header. */
		padding = 0U;
		while (((valid[valid_size - 1U] >> padding) & 1U) == 0U && padding < 8U)
			padding++;
		CHECK(padding <= 7U);
		seen |= 1U << padding;

		/* The one bit missing is refused. */
		wslice.alignment_one = 0U;
		error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
		CHECK(error == EINVAL);
		wslice.alignment_one = 1U;

		/* A one in the padding is refused when there is padding. */
		wslice.alignment_zero = 1U;
		error = parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL);
		if (padding != 0U) {
			CHECK(error == EINVAL);
		} else {
			CHECK(error == 0);
		}
		wslice.alignment_zero = 0U;
	}

	/* Every padding length of 0..7 occurred. */
	CHECK(seen == 0xFFU);

	/* The data after the header is not read: the header's last byte is where the data begins. */
	fixture_default();
	CHECK(apply() == 0);
	header_set(0U, 0U);
	CHECK(parse(H265SW_NAL_IDR_W_RADL, 0U, 0U, NO_CUT, NULL) == 0);
	valid_size = header_bytes;
	memcpy(valid, swriter.data, valid_size);
	memcpy(valid + valid_size, data_bytes, sizeof(data_bytes));
	CHECK(parse_bytes(H265SW_NAL_IDR_W_RADL, 0U, valid, valid_size + sizeof(data_bytes), NULL) == 0);
	CHECK(hdr.data_offset == valid_size);
}

/* A missing argument, a PPS that is not the one named, and sets outside the parse's bounds. */
static void
check_arguments(void)
{
	struct h265sw_sps bad_sps;
	struct h265sw_pps bad_pps;
	struct vcodec_bitreader bits;
	struct h265sw_nal nal;
	struct h265sw_slice_header header;

	/* A reader and header ready for the read after the first fields. */
	fixture_default();
	CHECK(apply() == 0);
	header_set(0U, 0U);
	bitwriter_init(&swriter);
	wctx.nal_type = H265SW_NAL_IDR_W_RADL;
	h265w_slice(&swriter, &wslice, &wctx);
	memset(&nal, 0, sizeof(nal));
	nal.type = H265SW_NAL_IDR_W_RADL;

	/* Every pointer is needed. */
	vcodec_bitreader_init(&bits, swriter.data, bitwriter_bytes(&swriter));
	CHECK(h265sw_slice_read_start(&header, &bits, &nal) == 0);
	CHECK(h265sw_slice_read_rest(NULL, &bits, sps, pps, NULL) == EINVAL);
	CHECK(h265sw_slice_read_rest(&header, NULL, sps, pps, NULL) == EINVAL);
	CHECK(h265sw_slice_read_rest(&header, &bits, NULL, pps, NULL) == EINVAL);
	CHECK(h265sw_slice_read_rest(&header, &bits, sps, NULL, NULL) == EINVAL);
	CHECK(h265sw_slice_read_rest(&header, &bits, sps, pps, NULL) == 0);

	/* The PPS must be the one the header names. */
	vcodec_bitreader_init(&bits, swriter.data, bitwriter_bytes(&swriter));
	CHECK(h265sw_slice_read_start(&header, &bits, &nal) == 0);
	bad_pps = *pps;
	bad_pps.id = 1U;
	CHECK(h265sw_slice_read_rest(&header, &bits, sps, &bad_pps, NULL) == EINVAL);

	/* The SPS must be the one the PPS names. */
	bad_pps = *pps;
	bad_pps.sps_id = 3U;
	CHECK(h265sw_slice_read_rest(&header, &bits, sps, &bad_pps, NULL) == EINVAL);

	/* Sets outside the bounds the parse keeps are refused. */
	bad_sps = *sps;
	bad_sps.pic_size_ctbs = 0U;
	CHECK(h265sw_slice_read_rest(&header, &bits, &bad_sps, pps, NULL) == EINVAL);
	bad_sps = *sps;
	bad_sps.log2_max_poc_lsb = 3U;
	CHECK(h265sw_slice_read_rest(&header, &bits, &bad_sps, pps, NULL) == EINVAL);
	bad_sps.log2_max_poc_lsb = 17U;
	CHECK(h265sw_slice_read_rest(&header, &bits, &bad_sps, pps, NULL) == EINVAL);
	bad_sps = *sps;
	bad_sps.max_sub_layers_minus1 = 7U;
	CHECK(h265sw_slice_read_rest(&header, &bits, &bad_sps, pps, NULL) == EINVAL);
	bad_sps = *sps;
	bad_sps.num_st_rps = 65U;
	CHECK(h265sw_slice_read_rest(&header, &bits, &bad_sps, pps, NULL) == EINVAL);
	bad_sps = *sps;
	bad_sps.num_lt_sps = 33U;
	CHECK(h265sw_slice_read_rest(&header, &bits, &bad_sps, pps, NULL) == EINVAL);

	/* A PPS default list size outside 1..15 is refused for P and B slices only. */
	fixture_default();
	CHECK(apply() == 0);
	header_set(1U, 0U);
	wctx.num_pic_total_curr = 1U;
	wslice.slice_type = H265SW_SLICE_P;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	bad_pps = *pps;
	bad_pps.num_ref_idx_default[0] = 16U;
	pps = &bad_pps;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
	bad_pps.num_ref_idx_default[0] = 0U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
	bad_pps.num_ref_idx_default[0] = 1U;
	bad_pps.num_ref_idx_default[1] = 16U;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == EINVAL);
	wslice.slice_type = H265SW_SLICE_I;
	CHECK(parse(H265SW_NAL_TRAIL_R, 0U, 0U, NO_CUT, NULL) == 0);
	pps = &store.pps[0];
}

/* Chooses a random SPS and PPS for the round trip; returns non-zero when the pair is not accepted by the parameter set parse. */
static int
gen_params(void)
{
	unsigned entry;
	unsigned ctb_size;
	unsigned lsb_bits;
	unsigned width_ctbs;
	unsigned height_ctbs;
	unsigned negative;
	unsigned positive;
	unsigned item;

	/* The format: chroma format, separate planes, bit depths. */
	fixture_default();
	wsps.chroma_format_idc = rnd(4U);
	if (wsps.chroma_format_idc == 3U)
		wsps.separate_colour_plane = rnd(2U);
	wsps.bit_depth_luma_minus8 = 2U * rnd(2U);
	wsps.bit_depth_chroma_minus8 = 2U * rnd(2U);

	/* The size and the CTB size. */
	wsps.log2_diff_max_min_cb = 1U + rnd(2U);
	ctb_size = 1U << (wsps.log2_min_cb_minus3 + 3U + wsps.log2_diff_max_min_cb);
	wsps.width = 8U * (1U + rnd(40U));
	wsps.height = 8U * (1U + rnd(40U));
	width_ctbs = (wsps.width + ctb_size - 1U) / ctb_size;
	height_ctbs = (wsps.height + ctb_size - 1U) / ctb_size;

	/* The order count length, the tools, the DPB size. */
	wsps.log2_max_poc_lsb_minus4 = rnd(13U);
	lsb_bits = wsps.log2_max_poc_lsb_minus4 + 4U;
	wsps.sao = rnd(2U);
	wsps.temporal_mvp = rnd(2U);
	wsps.max_dec_pic_buffering_minus1[0] = 10U;

	/* Up to four explicit short-term sets of up to six entries. */
	wsps.num_st_rps = rnd(5U);
	for (entry = 0U; entry < wsps.num_st_rps; entry++) {
		memset(&wsps.st_rps[entry], 0, sizeof(wsps.st_rps[entry]));
		negative = rnd(4U);
		positive = rnd(3U);
		wsps.st_rps[entry].num_negative_pics = negative;
		wsps.st_rps[entry].num_positive_pics = positive;
		for (item = 0U; item < negative; item++) {
			wsps.st_rps[entry].delta_poc_s0_minus1[item] = rnd(20U);
			wsps.st_rps[entry].used_by_curr_pic_s0_flag[item] = rnd(2U);
		}
		for (item = 0U; item < positive; item++) {
			wsps.st_rps[entry].delta_poc_s1_minus1[item] = rnd(20U);
			wsps.st_rps[entry].used_by_curr_pic_s1_flag[item] = rnd(2U);
		}
	}

	/* Long-term candidates. */
	wsps.long_term_present = rnd(2U);
	if (wsps.long_term_present != 0U) {
		wsps.num_lt_sps = rnd(4U);
		for (entry = 0U; entry < wsps.num_lt_sps; entry++) {
			wsps.lt_poc_lsb[entry] = random_next() & (((uint32_t)1U << lsb_bits) - 1U);
			wsps.lt_used[entry] = rnd(2U);
		}
	}

	/* The PPS controls of the slice header. */
	wpps.dependent_slices = rnd(2U);
	wpps.output_flag_present = rnd(2U);
	wpps.num_extra_slice_header_bits = rnd(8U);
	wpps.cabac_init_present = rnd(2U);
	wpps.num_ref_idx_l0_default_minus1 = rnd(4U);
	wpps.num_ref_idx_l1_default_minus1 = rnd(4U);
	wpps.init_qp_minus26 = (int32_t)rnd(52U + 6U * wsps.bit_depth_luma_minus8) - (int32_t)(26U + 6U * wsps.bit_depth_luma_minus8);
	wpps.cb_qp_offset = (int32_t)rnd(25U) - 12;
	wpps.cr_qp_offset = (int32_t)rnd(25U) - 12;
	wpps.slice_chroma_qp_offsets_present = rnd(2U);
	wpps.weighted_pred = rnd(2U);
	wpps.weighted_bipred = rnd(2U);
	wpps.entropy_sync = rnd(2U);
	if (width_ctbs >= 2U && height_ctbs >= 2U && rnd(3U) == 0U) {
		wpps.tiles = 1U;
		wpps.num_tile_columns_minus1 = rnd(2U);
		wpps.num_tile_rows_minus1 = rnd(2U);
		if (wpps.num_tile_columns_minus1 == 0U && wpps.num_tile_rows_minus1 == 0U)
			wpps.num_tile_columns_minus1 = 1U;
		wpps.uniform_spacing = 1U;
		wpps.loop_filter_across_tiles = rnd(2U);
	}
	wpps.deblocking_control_present = rnd(2U);
	if (wpps.deblocking_control_present != 0U) {
		wpps.deblocking_override_enabled = rnd(2U);
		wpps.deblocking_disabled = rnd(2U);
		if (wpps.deblocking_disabled == 0U) {
			wpps.beta_offset_div2 = (int32_t)rnd(13U) - 6;
			wpps.tc_offset_div2 = (int32_t)rnd(13U) - 6;
		}
	}
	wpps.loop_filter_across_slices = rnd(2U);
	wpps.lists_modification_present = rnd(2U);
	wpps.slice_header_extension_present = rnd(2U);

	/* Succeeded when the parse takes the pair: its answer is the result. */
	return apply();
}

/* Fills wslice and expect with a random independent segment's fields, from the writer's context (the syntax table's conditions). */
static void
gen_independent(
	unsigned nal_type,
	unsigned temporal_id)
{
	struct h265w_st_rps *written;
	unsigned irap;
	unsigned idr;
	unsigned lsb_bits;
	unsigned negative;
	unsigned positive;
	unsigned entry;
	unsigned list;
	unsigned total;
	unsigned from_sps;
	unsigned signalled;
	unsigned counts[2];
	unsigned pic_total;
	unsigned bits_used;
	unsigned collocated_list;
	unsigned luma_denom;
	unsigned chroma_denom;
	int weighted;
	int qp;
	int qp_min;
	int deblocking_disabled;
	int component;
	int64_t chroma_weight;
	int64_t offset;

	/* Starts from garbage in every field the syntax may leave unwritten, and from zero in the expected header. */
	for (entry = 0U; entry < sizeof(wslice); entry++)
		((uint8_t *)&wslice)[entry] = (uint8_t)rnd(256U);
	memset(&expect, 0, sizeof(expect));
	expect.nal_type = nal_type;
	expect.temporal_id = temporal_id;
	expect.pic_output = 1;
	expect.collocated_from_l0 = 1;
	irap = (nal_type >= 16U) ? 1U : 0U;
	idr = (nal_type == 19U || nal_type == 20U) ? 1U : 0U;
	lsb_bits = wctx.log2_max_poc_lsb;
	wslice.alignment_one = 1U;
	wslice.alignment_zero = 0U;
	wslice.extension_length = 0U;

	/* The reserved flags, the slice type, the output flag and the colour plane. */
	wslice.reserved_flags = random_next();
	expect.reserved_flags = wslice.reserved_flags & (((unsigned)1U << wctx.num_extra_slice_header_bits) - 1U);
	wslice.slice_type = (irap != 0U) ? H265W_SLICE_I : rnd(3U);
	expect.slice_type = wslice.slice_type;
	wslice.pic_output = rnd(2U);
	if (wctx.output_flag_present != 0U)
		expect.pic_output = (int)wslice.pic_output;
	wslice.colour_plane_id = rnd(3U);
	if (wctx.separate_colour_plane != 0U)
		expect.colour_plane_id = wslice.colour_plane_id;

	/* The order count, the short-term set and the long-term entries of a picture that is not IDR. */
	if (idr == 0U) {
		wslice.poc_lsb = random_next() & (((uint32_t)1U << lsb_bits) - 1U);
		expect.poc_lsb = wslice.poc_lsb;
		wslice.st_rps_sps = (wctx.num_st_rps > 0U) ? rnd(2U) : 0U;
		expect.st_rps_sps = (int)wslice.st_rps_sps;
		if (wslice.st_rps_sps != 0U) {
			/* A set of the SPS: with one set the index is not read. */
			wslice.st_rps_idx = rnd(wctx.num_st_rps);
			expect.st_rps_idx = wslice.st_rps_idx;
			set_explicit(&expect.st_rps, &wsps.st_rps[wslice.st_rps_idx]);
		} else {
			/* A set of the header: explicit, with the bits st_ref_pic_set() takes. */
			written = &wslice.st_rps;
			memset(written, 0, sizeof(*written));
			negative = rnd(4U);
			positive = rnd(3U);
			written->num_negative_pics = negative;
			written->num_positive_pics = positive;
			for (entry = 0U; entry < negative; entry++) {
				written->delta_poc_s0_minus1[entry] = rnd(40U);
				written->used_by_curr_pic_s0_flag[entry] = rnd(2U);
			}
			for (entry = 0U; entry < positive; entry++) {
				written->delta_poc_s1_minus1[entry] = rnd(40U);
				written->used_by_curr_pic_s1_flag[entry] = rnd(2U);
			}
			set_explicit(&expect.st_rps, written);
			expect.st_syntax.num_negative_pics = negative;
			expect.st_syntax.num_positive_pics = positive;
			bits_used = (wctx.num_st_rps != 0U) ? 1U : 0U;
			bits_used += ue_bits(negative) + ue_bits(positive);
			for (entry = 0U; entry < negative; entry++) {
				expect.st_syntax.delta_poc_s0_minus1[entry] = (uint16_t)written->delta_poc_s0_minus1[entry];
				expect.st_syntax.used_by_curr_pic_s0_flags |= written->used_by_curr_pic_s0_flag[entry] << entry;
				bits_used += ue_bits(written->delta_poc_s0_minus1[entry]) + 1U;
			}
			for (entry = 0U; entry < positive; entry++) {
				expect.st_syntax.delta_poc_s1_minus1[entry] = (uint16_t)written->delta_poc_s1_minus1[entry];
				expect.st_syntax.used_by_curr_pic_s1_flags |= written->used_by_curr_pic_s1_flag[entry] << entry;
				bits_used += ue_bits(written->delta_poc_s1_minus1[entry]) + 1U;
			}
			expect.st_syntax.bits = bits_used;
		}

		/* The long-term entries: some from the SPS's candidates, some of the header's own. */
		if (wctx.long_term_present != 0U) {
			from_sps = (wctx.num_lt_sps > 0U) ? rnd(wctx.num_lt_sps + 1U) : 0U;
			signalled = rnd(4U);
			wslice.num_long_term_sps = from_sps;
			wslice.num_long_term_pics = signalled;
			expect.num_long_term_sps = from_sps;
			expect.num_long_term_pics = signalled;
			expect.num_long_term = from_sps + signalled;
			for (entry = 0U; entry < from_sps + signalled; entry++) {
				if (entry < from_sps) {
					wslice.lt_idx_sps[entry] = rnd(wctx.num_lt_sps);
					expect.lt_idx_sps[entry] = (uint8_t)((wctx.num_lt_sps > 1U) ? wslice.lt_idx_sps[entry] : 0U);
					if (wctx.num_lt_sps <= 1U)
						wslice.lt_idx_sps[entry] = 0U;
					expect.lt_poc_lsb[entry] = wsps.lt_poc_lsb[expect.lt_idx_sps[entry]];
					expect.lt_used[entry] = (uint8_t)wsps.lt_used[expect.lt_idx_sps[entry]];
				} else {
					wslice.poc_lsb_lt[entry] = random_next() & (((uint32_t)1U << lsb_bits) - 1U);
					wslice.used_by_curr_pic_lt[entry] = rnd(2U);
					expect.lt_poc_lsb[entry] = wslice.poc_lsb_lt[entry];
					expect.lt_used[entry] = (uint8_t)wslice.used_by_curr_pic_lt[entry];
				}
				wslice.delta_poc_msb_present[entry] = rnd(2U);
				wslice.delta_poc_msb_cycle_lt[entry] = rnd(30U);
				expect.lt_msb_present[entry] = (uint8_t)wslice.delta_poc_msb_present[entry];
				total = (wslice.delta_poc_msb_present[entry] != 0U) ? wslice.delta_poc_msb_cycle_lt[entry] : 0U;
				if (entry != 0U && entry != from_sps)
					total += expect.lt_msb_cycle[entry - 1U];
				expect.lt_msb_cycle[entry] = total;
			}
		} else {
			/* The writer would still count them; keep the counts at 0 as the syntax has no such fields. */
			wslice.num_long_term_sps = 0U;
			wslice.num_long_term_pics = 0U;
		}

		/* The temporal MVP flag. */
		wslice.temporal_mvp = rnd(2U);
		if (wctx.temporal_mvp != 0U)
			expect.temporal_mvp = (int)wslice.temporal_mvp;
	}

	/* NumPicTotalCurr from the used entries (7-57). */
	pic_total = 0U;
	for (entry = 0U; entry < expect.st_rps.num_negative + expect.st_rps.num_positive; entry++) {
		if (expect.st_rps.used[entry] != 0U)
			pic_total++;
	}
	for (entry = 0U; entry < expect.num_long_term; entry++) {
		if (expect.lt_used[entry] != 0U)
			pic_total++;
	}
	expect.num_pic_total_curr = pic_total;
	wctx.num_pic_total_curr = pic_total;

	/* SAO. */
	wslice.sao_luma = rnd(2U);
	wslice.sao_chroma = rnd(2U);
	if (wctx.sao != 0U) {
		expect.sao_luma = (int)wslice.sao_luma;
		if (wctx.chroma_array_type != 0U)
			expect.sao_chroma = (int)wslice.sao_chroma;
	}

	/* The inter part. */
	if (expect.slice_type != H265SW_SLICE_I) {
		counts[0] = wctx.num_ref_idx_default_minus1[0] + 1U;
		counts[1] = wctx.num_ref_idx_default_minus1[1] + 1U;
		wslice.num_ref_idx_override = rnd(2U);
		wslice.num_ref_idx_active_minus1[0] = rnd(6U);
		wslice.num_ref_idx_active_minus1[1] = rnd(6U);
		if (wslice.num_ref_idx_override != 0U) {
			counts[0] = wslice.num_ref_idx_active_minus1[0] + 1U;
			if (expect.slice_type == H265SW_SLICE_B)
				counts[1] = wslice.num_ref_idx_active_minus1[1] + 1U;
		}
		expect.num_ref_idx[0] = counts[0];
		if (expect.slice_type == H265SW_SLICE_B)
			expect.num_ref_idx[1] = counts[1];

		/* The list modification, entries below NumPicTotalCurr. */
		wslice.list_mod[0] = rnd(2U);
		wslice.list_mod[1] = rnd(2U);
		for (list = 0U; list < 2U; list++) {
			for (entry = 0U; entry < H265W_MAX_REFS; entry++)
				wslice.list_entry[list][entry] = (pic_total > 0U) ? rnd(pic_total) : 0U;
		}
		if (wctx.lists_modification_present != 0U && pic_total > 1U) {
			for (list = 0U; list < ((expect.slice_type == H265SW_SLICE_B) ? 2U : 1U); list++) {
				expect.list_mod[list] = (int)wslice.list_mod[list];
				if (wslice.list_mod[list] == 0U)
					continue;
				for (entry = 0U; entry < expect.num_ref_idx[list]; entry++)
					expect.list_entry[list][entry] = (uint8_t)wslice.list_entry[list][entry];
			}
		}

		/* The flags, then the collocated picture. */
		wslice.mvd_l1_zero = rnd(2U);
		wslice.cabac_init = rnd(2U);
		if (expect.slice_type == H265SW_SLICE_B)
			expect.mvd_l1_zero = (int)wslice.mvd_l1_zero;
		if (wctx.cabac_init_present != 0U)
			expect.cabac_init = (int)wslice.cabac_init;
		wslice.collocated_from_l0 = rnd(2U);
		if (expect.temporal_mvp != 0) {
			if (expect.slice_type == H265SW_SLICE_B)
				expect.collocated_from_l0 = (int)wslice.collocated_from_l0;
			collocated_list = (expect.collocated_from_l0 != 0) ? 0U : 1U;
			wslice.collocated_ref_idx = rnd(expect.num_ref_idx[collocated_list]);
			if (expect.num_ref_idx[collocated_list] > 1U)
				expect.collocated_ref_idx = wslice.collocated_ref_idx;
		}

		/* The weights, when the PPS asks for them for this slice type. */
		weighted = 0;
		if (expect.slice_type == H265SW_SLICE_P && wctx.weighted_pred != 0U)
			weighted = 1;
		if (expect.slice_type == H265SW_SLICE_B && wctx.weighted_bipred != 0U)
			weighted = 1;
		luma_denom = rnd(8U);
		chroma_denom = luma_denom;
		wslice.luma_log2_weight_denom = luma_denom;
		wslice.delta_chroma_log2_weight_denom = (int32_t)rnd(8U) - (int32_t)luma_denom;
		if (wctx.chroma_array_type != 0U)
			chroma_denom = (unsigned)((int)luma_denom + wslice.delta_chroma_log2_weight_denom);
		for (list = 0U; list < 2U; list++) {
			for (entry = 0U; entry < H265W_MAX_REFS; entry++) {
				wslice.luma_weight_flag[list][entry] = rnd(2U);
				wslice.chroma_weight_flag[list][entry] = rnd(2U);
				wslice.delta_luma_weight[list][entry] = (int32_t)rnd(256U) - 128;
				wslice.luma_offset[list][entry] = (int32_t)rnd(256U) - 128;
				for (component = 0; component < 2; component++) {
					wslice.delta_chroma_weight[list][entry][component] = (int32_t)rnd(256U) - 128;
					wslice.delta_chroma_offset[list][entry][component] = (int32_t)rnd(1024U) - 512;
				}
			}
		}
		if (weighted != 0) {
			expect.weights.luma_log2_denom = luma_denom;
			expect.weights.chroma_log2_denom = chroma_denom;
			for (list = 0U; list < ((expect.slice_type == H265SW_SLICE_B) ? 2U : 1U); list++) {
				for (entry = 0U; entry < expect.num_ref_idx[list]; entry++) {
					expect.weights.luma_weight[list][entry] = (int16_t)(1 << luma_denom);
					if (wslice.luma_weight_flag[list][entry] != 0U) {
						expect.weights.luma_weight[list][entry] = (int16_t)((1 << luma_denom) + wslice.delta_luma_weight[list][entry]);
						expect.weights.luma_offset[list][entry] = (int16_t)wslice.luma_offset[list][entry];
					}
					if (wctx.chroma_array_type == 0U)
						continue;
					for (component = 0; component < 2; component++) {
						expect.weights.chroma_weight[list][entry][component] = (int16_t)(1 << chroma_denom);
						if (wslice.chroma_weight_flag[list][entry] == 0U)
							continue;
						chroma_weight = ((int64_t)1 << chroma_denom) + wslice.delta_chroma_weight[list][entry][component];
						offset = 128 + wslice.delta_chroma_offset[list][entry][component] - test_floor_div(128 * chroma_weight, (int64_t)1 << chroma_denom);
						if (offset < -128)
							offset = -128;
						if (offset > 127)
							offset = 127;
						expect.weights.chroma_weight[list][entry][component] = (int16_t)chroma_weight;
						expect.weights.chroma_offset[list][entry][component] = (int16_t)offset;
					}
				}
			}
		}

		/* The merge candidates. */
		wslice.five_minus_max_num_merge_cand = rnd(5U);
		expect.max_merge_cand = 5U - wslice.five_minus_max_num_merge_cand;
	}

	/* The QP: chosen in range, the delta from the PPS's initial value. */
	qp_min = -6 * (int)wsps.bit_depth_luma_minus8;
	qp = qp_min + (int)rnd((unsigned)(51 - qp_min + 1));
	wslice.slice_qp_delta = qp - (26 + wpps.init_qp_minus26);
	expect.qp = qp;

	/* The chroma offsets: chosen so that each and the sum with the PPS's are within 12. */
	wslice.cb_qp_offset = (int32_t)rnd(25U) - 12;
	wslice.cr_qp_offset = (int32_t)rnd(25U) - 12;
	while (wslice.cb_qp_offset + wpps.cb_qp_offset < -12 || wslice.cb_qp_offset + wpps.cb_qp_offset > 12)
		wslice.cb_qp_offset = (int32_t)rnd(25U) - 12;
	while (wslice.cr_qp_offset + wpps.cr_qp_offset < -12 || wslice.cr_qp_offset + wpps.cr_qp_offset > 12)
		wslice.cr_qp_offset = (int32_t)rnd(25U) - 12;
	if (wctx.slice_chroma_qp_offsets_present != 0U) {
		expect.cb_qp_offset = (int)wslice.cb_qp_offset;
		expect.cr_qp_offset = (int)wslice.cr_qp_offset;
	}

	/* The deblocking override: the PPS's values unless the slice overrides them. */
	deblocking_disabled = (int)wctx.pps_deblocking_disabled;
	expect.deblocking_disabled = deblocking_disabled;
	expect.beta_offset_div2 = (wpps.deblocking_control_present != 0U) ? (int)wpps.beta_offset_div2 : 0;
	expect.tc_offset_div2 = (wpps.deblocking_control_present != 0U) ? (int)wpps.tc_offset_div2 : 0;
	wslice.deblocking_override = rnd(2U);
	wslice.deblocking_disabled = rnd(2U);
	wslice.beta_offset_div2 = (int32_t)rnd(13U) - 6;
	wslice.tc_offset_div2 = (int32_t)rnd(13U) - 6;
	if (wctx.deblocking_override_enabled != 0U && wslice.deblocking_override != 0U) {
		expect.deblocking_override = 1;
		expect.deblocking_disabled = (int)wslice.deblocking_disabled;
		if (wslice.deblocking_disabled == 0U) {
			expect.beta_offset_div2 = (int)wslice.beta_offset_div2;
			expect.tc_offset_div2 = (int)wslice.tc_offset_div2;
		}
	}

	/* The loop filter across slices flag: read when the PPS allows it and some filter runs. */
	wslice.loop_filter_across_slices = rnd(2U);
	expect.loop_filter_across_slices = (int)wctx.loop_filter_across_slices;
	if (wctx.loop_filter_across_slices != 0U && (expect.sao_luma != 0 || expect.sao_chroma != 0 || expect.deblocking_disabled == 0))
		expect.loop_filter_across_slices = (int)wslice.loop_filter_across_slices;
}

/* Fills the segment's own fields for the writer and for the expected header: the place, the entry points and the extension. */
static void
gen_segment(
	unsigned nal_type,
	unsigned temporal_id,
	int first,
	int dependent,
	uint32_t address,
	const struct h265sw_slice_header *independent_expect)
{
	unsigned limit;
	unsigned count;
	unsigned entry;
	unsigned height_ctbs;
	unsigned ctb_size;
	unsigned irap;

	/* An independent segment has its own fields; a dependent one takes them from the independent header. */
	if (dependent == 0) {
		gen_independent(nal_type, temporal_id);
	} else {
		for (entry = 0U; entry < sizeof(wslice); entry++)
			((uint8_t *)&wslice)[entry] = (uint8_t)rnd(256U);
		wslice.alignment_one = 1U;
		wslice.alignment_zero = 0U;
		expect = *independent_expect;
		expect.temporal_id = temporal_id;
	}
	irap = (nal_type >= 16U) ? 1U : 0U;

	/* The place of the segment. */
	wslice.first_in_pic = (uint32_t)first;
	wslice.pps_id = 0U;
	wslice.dependent = (uint32_t)dependent;
	wslice.address = address;
	wslice.no_output_of_prior_pics = rnd(2U);
	expect.first_in_pic = first;
	expect.no_output_of_prior_pics = (irap != 0U) ? (int)wslice.no_output_of_prior_pics : 0;
	expect.pps_id = 0U;
	expect.dependent = dependent;
	expect.address = address;
	if (dependent == 0)
		expect.slice_addr_rs = address;
	if (dependent != 0)
		expect.slice_addr_rs = independent_expect->slice_addr_rs;

	/* The entry points, within the bound of 7.4.7.1. */
	expect.num_entry_points = 0U;
	expect.offset_len = 0U;
	wslice.num_entry_point_offsets = rnd(5U);
	wslice.offset_len_minus1 = rnd(32U);
	wslice.entry_point_offset_minus1 = random_next();
	if (wctx.tiles != 0U || wctx.entropy_sync != 0U) {
		ctb_size = 1U << (wsps.log2_min_cb_minus3 + 3U + wsps.log2_diff_max_min_cb);
		height_ctbs = (wsps.height + ctb_size - 1U) / ctb_size;
		if (wctx.tiles != 0U && wctx.entropy_sync != 0U) {
			limit = (wpps.num_tile_columns_minus1 + 1U) * height_ctbs - 1U;
		} else if (wctx.tiles != 0U) {
			limit = (wpps.num_tile_columns_minus1 + 1U) * (wpps.num_tile_rows_minus1 + 1U) - 1U;
		} else {
			limit = height_ctbs - 1U;
		}
		count = wslice.num_entry_point_offsets;
		if (count > limit) {
			count = limit;
			wslice.num_entry_point_offsets = count;
		}
		expect.num_entry_points = count;
		if (count > 0U)
			expect.offset_len = wslice.offset_len_minus1 + 1U;
	}

	/* The header extension. */
	if (wctx.slice_header_extension_present != 0U)
		wslice.extension_length = rnd(6U);
	else
		wslice.extension_length = rnd(1000U);
}

/* Random headers from random parameter sets: each is written, read, compared in full, cut at every byte, and bit-flipped. */
static void
check_random_roundtrip(void)
{
	static const unsigned types[] = {0U, 1U, 6U, 7U, 8U, 9U, 16U, 17U, 18U, 19U, 20U, 21U};
	unsigned iteration;
	unsigned nal_type;
	unsigned temporal_id;
	unsigned extra;
	unsigned skipped;
	unsigned flips;
	unsigned bit;
	size_t cut;
	size_t size;
	uint32_t pic_size;
	uint32_t address;
	int first;
	int error;
	uint8_t copy[512];

	/* Many random scenarios. */
	skipped = 0U;
	for (iteration = 0U; iteration < 4000U; iteration++) {
		if (gen_params() != 0) {
			skipped++;
			continue;
		}
		nal_type = types[rnd(sizeof(types) / sizeof(types[0]))];
		temporal_id = (nal_type >= 16U) ? 0U : rnd(3U);
		pic_size = wctx.pic_size_ctbs;
		first = (pic_size == 1U || rnd(3U) == 0U) ? 1 : 0;
		address = (first != 0) ? 0U : 1U + rnd(pic_size - 1U);

		/* The independent (or first) segment: written, read, and every field compared. */
		gen_segment(nal_type, temporal_id, first, 0, address, NULL);
		extra = rnd(11U);
		error = parse(nal_type, temporal_id, extra, NO_CUT, NULL);
		CHECK(error == 0);
		if (error != 0)
			continue;
		expect.data_offset = header_bytes;
		compare_headers(&expect, &hdr);
		check_invariants(&hdr, header_bytes + extra);
		kept = hdr;
		kept_expect = expect;

		/* Every strict prefix of the RBSP fails. */
		size = header_bytes;
		if (iteration % 3U == 0U) {
			for (cut = 0U; cut < size; cut++) {
				error = parse_bytes(nal_type, temporal_id, swriter.data, cut, NULL);
				CHECK(error == EINVAL);
			}
		}

		/* A copy with flipped bits: no fault, and an accepted header keeps the invariants. */
		if (size <= sizeof(copy)) {
			for (flips = 0U; flips < 6U; flips++) {
				memcpy(copy, swriter.data, size);
				for (bit = 0U; bit < 1U + flips % 3U; bit++)
					copy[rnd((uint32_t)size)] ^= (uint8_t)(1U << rnd(8U));
				error = parse_bytes(nal_type, temporal_id, copy, size, NULL);
				if (error == 0)
					check_invariants(&hdr, size);
			}
		}

		/* A dependent segment of the same picture, when the PPS allows one and the picture has room. */
		if (wctx.dependent_slices != 0U && pic_size > 1U) {
			gen_segment(nal_type, temporal_id, 0, 1, 1U + rnd(pic_size - 1U), &kept_expect);
			extra = rnd(11U);
			error = parse(nal_type, temporal_id, extra, NO_CUT, &kept);
			CHECK(error == 0);
			if (error == 0) {
				expect.data_offset = header_bytes;
				compare_headers(&expect, &hdr);
				check_invariants(&hdr, header_bytes + extra);
			}
			if (iteration % 3U == 0U) {
				for (cut = 0U; cut < header_bytes; cut++) {
					error = parse_bytes(nal_type, temporal_id, swriter.data, cut, &kept);
					CHECK(error == EINVAL);
				}
			}

			/* The same bytes without the independent header are refused. */
			CHECK(parse_bytes(nal_type, temporal_id, swriter.data, header_bytes, NULL) == EINVAL);
		}
	}

	/* Almost all scenarios were accepted by the parameter set parse. */
	CHECK(skipped < 400U);
}

/* Random bytes after a valid start: no fault, and an accepted header keeps the invariants. */
static void
check_garbage(void)
{
	unsigned iteration;
	unsigned length;
	unsigned index;
	unsigned accepted;
	unsigned nal_type;
	uint8_t data[96];
	int error;

	/* A rich sequence, so random bits get deep into the syntax. */
	fixture_default();
	wsps.sao = 1U;
	wsps.temporal_mvp = 1U;
	wsps.long_term_present = 1U;
	wsps.num_lt_sps = 3U;
	wsps.lt_poc_lsb[0] = 1U;
	wsps.lt_poc_lsb[1] = 2U;
	wsps.lt_poc_lsb[2] = 3U;
	wsps.lt_used[0] = 1U;
	wsps.lt_used[2] = 1U;
	wsps.num_st_rps = 3U;
	wsps.st_rps[0].num_negative_pics = 2U;
	wsps.st_rps[0].used_by_curr_pic_s0_flag[0] = 1U;
	wsps.st_rps[0].used_by_curr_pic_s0_flag[1] = 1U;
	wsps.st_rps[1].num_negative_pics = 1U;
	wsps.st_rps[1].num_positive_pics = 1U;
	wsps.st_rps[1].used_by_curr_pic_s0_flag[0] = 1U;
	wsps.st_rps[1].used_by_curr_pic_s1_flag[0] = 1U;
	wsps.st_rps[2].num_positive_pics = 3U;
	wsps.st_rps[2].used_by_curr_pic_s1_flag[0] = 1U;
	wpps.dependent_slices = 1U;
	wpps.output_flag_present = 1U;
	wpps.num_extra_slice_header_bits = 2U;
	wpps.cabac_init_present = 1U;
	wpps.num_ref_idx_l0_default_minus1 = 1U;
	wpps.slice_chroma_qp_offsets_present = 1U;
	wpps.weighted_pred = 1U;
	wpps.weighted_bipred = 1U;
	wpps.lists_modification_present = 1U;
	wpps.deblocking_control_present = 1U;
	wpps.deblocking_override_enabled = 1U;
	wpps.entropy_sync = 1U;
	wpps.slice_header_extension_present = 1U;
	CHECK(apply() == 0);

	/* Random bytes whose first two bits make a first segment of PPS 0; the rest is random. */
	accepted = 0U;
	for (iteration = 0U; iteration < 60000U; iteration++) {
		length = 1U + rnd(sizeof(data));
		for (index = 0U; index < length; index++)
			data[index] = (uint8_t)rnd(256U);
		nal_type = (rnd(2U) != 0U) ? H265SW_NAL_TRAIL_R : H265SW_NAL_RASL_N;
		data[0] = (uint8_t)(0xC0U | (data[0] & 0x3FU));
		error = parse_bytes(nal_type, 0U, data, length, NULL);
		if (error == 0) {
			accepted++;
			check_invariants(&hdr, length);
		} else {
			CHECK(error == EINVAL || error == ENOTSUP);
		}
	}

	/* Some random bytes are valid headers; the test is not vacuous. */
	CHECK(accepted > 100U);
}

int
main(void)
{
	check_start();
	check_baseline();
	check_nal_types();
	check_slice_types();
	check_address();
	check_dependent();
	check_reserved_flags();
	check_output_and_plane();
	check_sps_sets();
	check_header_sets();
	check_long_term();
	check_long_term_limits();
	check_tools();
	check_inter();
	check_list_modification();
	check_collocated();
	check_weights();
	check_weights_ranges();
	check_merge_and_qp();
	check_chroma_offsets();
	check_deblocking();
	check_entry_points();
	check_extension();
	check_alignment();
	check_arguments();
	check_random_roundtrip();
	check_garbage();

	return check_finish("slice");
}
