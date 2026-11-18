/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice segment header of H.265 (7.3.6, 7.4.7).
 */

#include "h265sw-slice.h"

#include <errno.h>
#include <string.h>

/* The largest slice_pic_parameter_set_id (7.4.7.1: 0..63). */
#define SLICE_MAX_PPS_ID 63U

/* The largest num_ref_idx_lX_active_minus1 (7.4.7.1: 0..14). */
#define SLICE_MAX_REF_IDX_MINUS1 14U

/* The most slice_segment_header_extension_data_byte a header carries (7.4.7.1: 0..256). */
#define SLICE_MAX_EXTENSION 256U

/* The largest offset_len_minus1 (7.4.7.1: 0..31). */
#define SLICE_MAX_OFFSET_LEN_MINUS1 31U

/* The largest five_minus_max_num_merge_cand, which leaves MaxNumMergeCand 1 (7-53). */
#define SLICE_MAX_FIVE_MINUS_MERGE 4U

/* The largest luma_log2_weight_denom and ChromaLog2WeightDenom (7.4.7.3: 0..7). */
#define SLICE_MAX_WEIGHT_DENOM 7

/* The range of delta_luma_weight_lX, delta_chroma_weight_lX and luma_offset_lX (7.4.7.3 with WpOffsetHalfRangeY 128: -128..127). */
#define SLICE_WEIGHT_MIN (-128)
#define SLICE_WEIGHT_MAX 127

/* WpOffsetHalfRangeC when high_precision_offsets_enabled_flag is 0 (7-58). */
#define SLICE_CHROMA_HALF_RANGE 128

/* The range of delta_chroma_offset_lX (7.4.7.3: -4 * 128 .. 4 * 128 - 1). */
#define SLICE_CHROMA_DELTA_OFFSET_MIN (-512)
#define SLICE_CHROMA_DELTA_OFFSET_MAX 511

/* The range of the chroma QP offsets and of the filter offsets of a slice, and the top of SliceQpY (7.4.7.1). */
#define SLICE_QP_OFFSET_LIMIT 12
#define SLICE_FILTER_OFFSET_LIMIT 6
#define SLICE_QP_MAX 51

/* Bit 6 of h265sw_sps.range_extension: high_precision_offsets_enabled_flag (7.3.2.2.2, the seventh flag). */
#define SLICE_HIGH_PRECISION_OFFSETS_BIT 6U

/* The most long-term entries and short-term entries together that the arrays of h265sw_slice_header hold. */
#define SLICE_MAX_LONG_TERM (H265SW_MAX_RPS - 1U)

/* Static function declarations. */
static unsigned slice_ceil_log2(uint32_t value);
static int64_t slice_floor_div(int64_t numerator, int64_t denominator);
static int slice_check_arguments(const struct h265sw_slice_header *out, const struct vcodec_bitreader *bits, const struct h265sw_sps *sps, const struct h265sw_pps *pps);
static int slice_copy_independent(struct h265sw_slice_header *out, const struct h265sw_slice_header *independent);
static int slice_read_independent(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_sps *sps, const struct h265sw_pps *pps);
static int slice_read_picture_fields(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_sps *sps, const struct h265sw_pps *pps);
static int slice_read_reference_set(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_sps *sps);
static int slice_read_long_term(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_sps *sps);
static void slice_count_pictures(struct h265sw_slice_header *out);
static int slice_read_inter(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_sps *sps, const struct h265sw_pps *pps);
static int slice_read_list_modification(struct h265sw_slice_header *out, struct vcodec_bitreader *bits);
static int slice_read_collocated(struct h265sw_slice_header *out, struct vcodec_bitreader *bits);
static int slice_read_weights(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_sps *sps);
static int slice_read_weights_list(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, unsigned chroma_array_type, unsigned list);
static int slice_read_quantization(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_sps *sps, const struct h265sw_pps *pps);
static int slice_read_filters(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_pps *pps);
static int slice_read_entry_points(struct h265sw_slice_header *out, struct vcodec_bitreader *bits, const struct h265sw_sps *sps, const struct h265sw_pps *pps);
static int slice_read_extension(struct vcodec_bitreader *bits, const struct h265sw_pps *pps);
static int slice_read_alignment(struct vcodec_bitreader *bits);

/*
 * Reads the first fields of a slice segment header, up to and including
 * slice_pic_parameter_set_id, so the caller can find the PPS and SPS (and
 * decide whether the segment starts a picture) before the rest.
 *
 * The no_output_of_prior_pics_flag is present for the IRAP types only; every
 * other field of the structure is cleared.
 */
int
h265sw_slice_read_start(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_nal *nal)
{
	uint32_t pps_id;
	int decodable;
	int irap;

	/* Refuses arguments no valid caller passes. */
	if (out == NULL || bits == NULL || nal == NULL)
		return EINVAL;

	/* Only a decodable VCL type has a slice segment header this parse reads. */
	decodable = h265sw_nal_is_decodable(nal->type);
	if (decodable == 0)
		return EINVAL;

	/* Starts from nothing read and takes the NAL unit's own fields. */
	memset(out, 0, sizeof(*out));
	out->nal_type = nal->type;
	out->temporal_id = nal->temporal_id;

	/* Reads the flag that starts a picture. */
	out->first_in_pic = (int)vcodec_bitreader_u1(bits);

	/* Reads the flag of an IRAP picture that decides what happens to the pictures before it. */
	irap = h265sw_nal_is_irap(nal->type);
	if (irap != 0)
		out->no_output_of_prior_pics = (int)vcodec_bitreader_u1(bits);

	/* Reads the id of the PPS in use. */
	pps_id = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (pps_id > SLICE_MAX_PPS_ID)
		return EINVAL;
	out->pps_id = (unsigned)pps_id;

	/* Succeeded: the segment's first fields are read. */
	return 0;
}

/*
 * Reads the rest of a slice segment header after h265sw_slice_read_start
 * (7.3.6.1 to 7.3.6.3).
 *
 * A dependent segment takes every independent field from the header of the
 * segment before it, so only the fields every segment has are read for it.
 */
int
h265sw_slice_read_rest(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps,
	const struct h265sw_slice_header *independent)
{
	uint32_t dependent;
	uint32_t address;
	int error;

	/* Refuses arguments no valid caller passes, and a pair of sets that is not the one named. */
	error = slice_check_arguments(out, bits, sps, pps);
	if (error != 0)
		return error;

	/* Reads the dependent flag and the address of a segment that is not the first of its picture. */
	dependent = 0U;
	address = 0U;
	if (out->first_in_pic == 0) {
		if (pps->dependent_slices != 0)
			dependent = vcodec_bitreader_u1(bits);
		address = vcodec_bitreader_u(bits, slice_ceil_log2((uint32_t)sps->pic_size_ctbs));
		if (bits->error != 0)
			return EINVAL;

		/* A later segment starts inside the picture and not where the first one does (7.4.7.1). */
		if (address >= sps->pic_size_ctbs)
			return EINVAL;
		if (address == 0U)
			return EINVAL;
	}
	out->dependent = (int)dependent;
	out->address = address;
	out->slice_addr_rs = address;

	/* Takes the independent fields from the preceding segment, or reads them. */
	if (dependent != 0U) {
		error = slice_copy_independent(out, independent);
	} else {
		error = slice_read_independent(out, bits, sps, pps);
	}
	if (error != 0)
		return error;

	/* Reads the entry points of tiles and wavefronts, which every segment may carry. */
	error = slice_read_entry_points(out, bits, sps, pps);
	if (error != 0)
		return error;

	/* Skips the header extension bytes, whose value a decoder ignores. */
	error = slice_read_extension(bits, pps);
	if (error != 0)
		return error;

	/* Reads the bits that end the header and start the slice segment data at a byte. */
	error = slice_read_alignment(bits);
	if (error != 0)
		return error;

	/* Records where the slice segment data starts. */
	out->data_offset = bits->position / 8U;

	/* Succeeded: the whole header is read. */
	return 0;
}

/* Returns Ceil( Log2( value ) ) (5.7): 0 for 0 and 1. */
static unsigned
slice_ceil_log2(
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

/* Divides rounding toward negative infinity, for a positive denominator (the Floor( x / m ) of h265sw.h). */
static int64_t
slice_floor_div(
	int64_t numerator,
	int64_t denominator)
{
	int64_t quotient;

	/* C division rounds toward zero, which is one too high for a negative inexact quotient. */
	quotient = numerator / denominator;
	if (numerator % denominator != 0 && numerator < 0)
		quotient -= 1;

	/* Succeeded: the floor. */
	return quotient;
}

/* Refuses a caller's mistake: a missing argument, a pair of sets that does not belong together, or sets outside the bounds of the arrays. */
static int
slice_check_arguments(
	const struct h265sw_slice_header *out,
	const struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps)
{
	/* Every pointer is needed. */
	if (out == NULL || bits == NULL || sps == NULL || pps == NULL)
		return EINVAL;

	/* The PPS is the one the segment names and the SPS is the one the PPS names. */
	if (out->pps_id != pps->id)
		return EINVAL;
	if (pps->sps_id != sps->id)
		return EINVAL;

	/* Sets the parse stored are inside these bounds; a hand-made one may not be. */
	if (sps->pic_size_ctbs == 0U)
		return EINVAL;
	if (sps->log2_max_poc_lsb < 4U || sps->log2_max_poc_lsb > 16U)
		return EINVAL;
	if (sps->max_sub_layers_minus1 >= H265SW_MAX_SUB_LAYERS)
		return EINVAL;
	if (sps->num_st_rps > H265SW_MAX_ST_RPS - 1U)
		return EINVAL;
	if (sps->num_lt_sps > H265SW_MAX_LT_SPS)
		return EINVAL;

	/* Succeeded: the arguments are usable. */
	return 0;
}

/*
 * Takes every independent field of a dependent segment from the
 * independent segment of its picture, keeping the fields of this segment's
 * own (7.4.7.1: the fields not present are inferred from the slice header).
 */
static int
slice_copy_independent(
	struct h265sw_slice_header *out,
	const struct h265sw_slice_header *independent)
{
	unsigned nal_type;
	unsigned temporal_id;
	int first_in_pic;
	int no_output_of_prior_pics;
	unsigned pps_id;
	uint32_t address;

	/* A dependent segment has nothing to depend on without it. */
	if (independent == NULL)
		return EINVAL;

	/* The segments of one picture use one PPS (7.4.7.1). */
	if (independent->pps_id != out->pps_id)
		return EINVAL;

	/* Remembers the fields of this segment that the copy would overwrite. */
	nal_type = out->nal_type;
	temporal_id = out->temporal_id;
	first_in_pic = out->first_in_pic;
	no_output_of_prior_pics = out->no_output_of_prior_pics;
	pps_id = out->pps_id;
	address = out->address;

	/* Takes the whole independent header, then puts this segment's own fields back. */
	*out = *independent;
	out->nal_type = nal_type;
	out->temporal_id = temporal_id;
	out->first_in_pic = first_in_pic;
	out->no_output_of_prior_pics = no_output_of_prior_pics;
	out->pps_id = pps_id;
	out->dependent = 1;
	out->address = address;

	/* The slice's SliceAddrRs is that of the independent segment it continues (7.4.7.1). */
	out->slice_addr_rs = independent->slice_addr_rs;

	/* Entry points and the data start belong to this segment and are read after the copy. */
	out->num_entry_points = 0U;
	out->offset_len = 0U;
	out->data_offset = 0U;

	/* Succeeded: the independent fields are inherited. */
	return 0;
}

/* Reads the fields only an independent segment carries (7.3.6.1 up to the entry points). */
static int
slice_read_independent(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps)
{
	unsigned chroma_array_type;
	int idr;
	int error;

	/* Values of an absent element: the picture is output and the collocated picture is in list 0. */
	out->pic_output = 1;
	out->collocated_from_l0 = 1;

	/* Reads the type, the output flag and the colour plane. */
	error = slice_read_picture_fields(out, bits, sps, pps);
	if (error != 0)
		return error;

	/* Reads the order count and the reference picture set of a picture that is not IDR (an IDR picture has an empty set). */
	idr = h265sw_nal_is_idr(out->nal_type);
	if (idr == 0) {
		error = slice_read_reference_set(out, bits, sps);
		if (error != 0)
			return error;
	}

	/* Counts the pictures the current picture may reference (7-57). */
	slice_count_pictures(out);

	/* Reads the SAO flags; the chroma flag needs a chroma array. */
	chroma_array_type = sps->chroma_format_idc;
	if (sps->separate_colour_plane != 0)
		chroma_array_type = 0U;
	if (sps->sao != 0) {
		out->sao_luma = (int)vcodec_bitreader_u1(bits);
		if (chroma_array_type != 0U)
			out->sao_chroma = (int)vcodec_bitreader_u1(bits);
	}
	if (bits->error != 0)
		return EINVAL;

	/* Reads the reference lists, motion and weights of a P or B slice. */
	if (out->slice_type != H265SW_SLICE_I) {
		error = slice_read_inter(out, bits, sps, pps);
		if (error != 0)
			return error;
	}

	/* Reads the quantization parameter and the chroma offsets. */
	error = slice_read_quantization(out, bits, sps, pps);
	if (error != 0)
		return error;

	/* Reads the deblocking and the loop filter controls. */
	error = slice_read_filters(out, bits, pps);
	if (error != 0)
		return error;

	/* Succeeded: the independent fields are read. */
	return 0;
}

/* Reads the reserved flags, the slice type, the output flag and the colour plane id. */
static int
slice_read_picture_fields(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps)
{
	uint32_t slice_type;
	uint32_t colour_plane_id;
	uint32_t flag;
	unsigned index;
	int irap;

	/* Reads the reserved flags, which a decoder ignores but keeps the bits of. */
	for (index = 0U; index < pps->num_extra_slice_header_bits && index < 8U; index++) {
		flag = vcodec_bitreader_u1(bits);
		out->reserved_flags |= flag << index;
	}

	/* Reads the slice type. */
	slice_type = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (slice_type > H265SW_SLICE_I)
		return EINVAL;
	out->slice_type = (unsigned)slice_type;

	/* An IRAP picture has intra slices only (7.4.7.1). */
	irap = h265sw_nal_is_irap(out->nal_type);
	if (irap != 0 && slice_type != H265SW_SLICE_I)
		return EINVAL;

	/* Reads the output flag, which is 1 when absent. */
	if (pps->output_flag_present != 0)
		out->pic_output = (int)vcodec_bitreader_u1(bits);

	/* Reads the colour plane the slice codes when the three planes are coded apart. */
	if (sps->separate_colour_plane != 0) {
		colour_plane_id = vcodec_bitreader_u(bits, 2U);
		if (bits->error != 0)
			return EINVAL;
		if (colour_plane_id > 2U)
			return EINVAL;
		out->colour_plane_id = (unsigned)colour_plane_id;
	}
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the picture fields are read. */
	return 0;
}

/* Reads the order count lsb, the short-term set, the long-term entries and the temporal MVP flag of a picture that is not IDR. */
static int
slice_read_reference_set(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps)
{
	uint32_t index;
	unsigned max_pictures;
	int error;

	/* Reads the least significant bits of the picture order count. */
	out->poc_lsb = vcodec_bitreader_u(bits, sps->log2_max_poc_lsb);

	/* Reads whether the short-term set comes from the SPS. */
	out->st_rps_sps = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Reads the set the header carries, predicted from the SPS's sets when it says so. */
	if (out->st_rps_sps == 0) {
		max_pictures = sps->max_dec_pic_buffering_minus1[sps->max_sub_layers_minus1];
		error = h265sw_rps_read(bits, sps->num_st_rps, sps->num_st_rps, sps->st_rps, max_pictures, &out->st_rps, &out->st_syntax);
		if (error != 0)
			return error;
	} else {
		/* Reads which of the SPS's sets is used; with one set there is nothing to read. */
		index = 0U;
		if (sps->num_st_rps > 1U)
			index = vcodec_bitreader_u(bits, slice_ceil_log2(sps->num_st_rps));
		if (bits->error != 0)
			return EINVAL;

		/* Refuses an index past the SPS's sets, which includes any index when the SPS has none. */
		if (index >= sps->num_st_rps)
			return EINVAL;
		out->st_rps_idx = (unsigned)index;
		out->st_rps = sps->st_rps[index];
	}

	/* Reads the long-term entries when the sequence has them. */
	if (sps->long_term_present != 0) {
		error = slice_read_long_term(out, bits, sps);
		if (error != 0)
			return error;
	}

	/* Reads whether temporal motion vector prediction is used. */
	if (sps->temporal_mvp != 0)
		out->temporal_mvp = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the reference picture set is read. */
	return 0;
}

/* Reads the long-term entries (7.3.6.1) and accumulates DeltaPocMsbCycleLt (7-52). */
static int
slice_read_long_term(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps)
{
	uint32_t from_sps;
	uint32_t signalled;
	uint32_t lt_idx;
	uint32_t delta_cycle;
	uint64_t total;
	uint64_t limit;
	uint64_t cycle_limit;
	uint64_t cycle;
	unsigned delta_pocs;
	unsigned lsb_bits;
	unsigned index;

	/* Reads how many entries come from the SPS and how many the header carries. */
	from_sps = 0U;
	if (sps->num_lt_sps > 0U)
		from_sps = vcodec_bitreader_ue(bits);
	signalled = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;

	/* The SPS entries are among the SPS's candidates (7.4.7.1). */
	if (from_sps > sps->num_lt_sps)
		return EINVAL;

	/* Together with the short-term entries they fit the arrays; the sum is made wide so no value wraps it. */
	delta_pocs = out->st_rps.num_negative + out->st_rps.num_positive;
	if (delta_pocs > SLICE_MAX_LONG_TERM)
		return EINVAL;
	total = (uint64_t)from_sps + (uint64_t)signalled;
	limit = (uint64_t)SLICE_MAX_LONG_TERM - (uint64_t)delta_pocs;
	if (total > limit)
		return EINVAL;
	out->num_long_term_sps = (unsigned)from_sps;
	out->num_long_term_pics = (unsigned)signalled;
	out->num_long_term = (unsigned)total;

	/* DeltaPocMsbCycleLt, and each cycle count, stays within 2^( 32 - log2_max_pic_order_cnt_lsb ) (7.4.7.1). */
	lsb_bits = sps->log2_max_poc_lsb;
	cycle_limit = (uint64_t)1U << (32U - lsb_bits);

	/* Reads each entry: its picture, whether its most significant part is given, and that part. */
	for (index = 0U; index < out->num_long_term; index++) {
		if (index < out->num_long_term_sps) {
			/* A candidate of the SPS: the index says which, and the SPS gives its lsb and use. */
			lt_idx = 0U;
			if (sps->num_lt_sps > 1U)
				lt_idx = vcodec_bitreader_u(bits, slice_ceil_log2(sps->num_lt_sps));
			if (bits->error != 0)
				return EINVAL;
			if (lt_idx >= sps->num_lt_sps)
				return EINVAL;
			out->lt_idx_sps[index] = (uint8_t)lt_idx;
			out->lt_poc_lsb[index] = sps->lt_poc_lsb[lt_idx];
			out->lt_used[index] = sps->lt_used[lt_idx];
		} else {
			/* An entry of the header: it gives its lsb and use itself. */
			out->lt_poc_lsb[index] = vcodec_bitreader_u(bits, lsb_bits);
			out->lt_used[index] = (uint8_t)vcodec_bitreader_u1(bits);
		}

		/* Reads whether the entry names its picture by the full order count, and the cycle count then. */
		out->lt_msb_present[index] = (uint8_t)vcodec_bitreader_u1(bits);
		delta_cycle = 0U;
		if (out->lt_msb_present[index] != 0U)
			delta_cycle = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if ((uint64_t)delta_cycle > cycle_limit)
			return EINVAL;

		/* Adds the cycle count to that of the entry before it, which restarts for the first entry of each group (7-52). */
		cycle = (uint64_t)delta_cycle;
		if (index != 0U && index != out->num_long_term_sps)
			cycle += (uint64_t)out->lt_msb_cycle[index - 1U];
		if (cycle > cycle_limit)
			return EINVAL;
		out->lt_msb_cycle[index] = (uint32_t)cycle;
	}

	/* Succeeded: the long-term entries are read. */
	return 0;
}

/* Derives NumPicTotalCurr from the used entries of the short-term and long-term sets (7-57). */
static void
slice_count_pictures(
	struct h265sw_slice_header *out)
{
	unsigned count;
	unsigned index;

	/* Counts the short-term entries the picture uses, before and after it. */
	count = 0U;
	for (index = 0U; index < out->st_rps.num_negative + out->st_rps.num_positive && index < H265SW_MAX_RPS; index++) {
		if (out->st_rps.used[index] != 0U)
			count++;
	}

	/* Counts the long-term entries the picture uses. */
	for (index = 0U; index < out->num_long_term && index < H265SW_MAX_RPS; index++) {
		if (out->lt_used[index] != 0U)
			count++;
	}

	/* Records the total. */
	out->num_pic_total_curr = count;
}

/* Reads the part of a P or B slice header that concerns reference pictures, motion and weights (7.3.6.1). */
static int
slice_read_inter(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps)
{
	uint32_t override_flag;
	uint32_t count_minus1;
	uint32_t five_minus_merge;
	int is_b;
	int weighted;
	int error;

	/* The lists start at the PPS's defaults, which the parse bounds by the arrays. */
	is_b = 0;
	if (out->slice_type == H265SW_SLICE_B)
		is_b = 1;
	if (pps->num_ref_idx_default[0] < 1U || pps->num_ref_idx_default[0] > H265SW_MAX_REFS)
		return EINVAL;
	if (pps->num_ref_idx_default[1] < 1U || pps->num_ref_idx_default[1] > H265SW_MAX_REFS)
		return EINVAL;
	out->num_ref_idx[0] = pps->num_ref_idx_default[0];
	if (is_b != 0)
		out->num_ref_idx[1] = pps->num_ref_idx_default[1];

	/* Reads whether the slice overrides the defaults, and the list sizes it gives. */
	override_flag = vcodec_bitreader_u1(bits);
	if (override_flag != 0U) {
		count_minus1 = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (count_minus1 > SLICE_MAX_REF_IDX_MINUS1)
			return EINVAL;
		out->num_ref_idx[0] = (unsigned)count_minus1 + 1U;
		if (is_b != 0) {
			count_minus1 = vcodec_bitreader_ue(bits);
			if (bits->error != 0)
				return EINVAL;
			if (count_minus1 > SLICE_MAX_REF_IDX_MINUS1)
				return EINVAL;
			out->num_ref_idx[1] = (unsigned)count_minus1 + 1U;
		}
	}
	if (bits->error != 0)
		return EINVAL;

	/* Reads the list modification when the PPS allows it and the picture has more than one reference. */
	if (pps->lists_modification_present != 0 && out->num_pic_total_curr > 1U) {
		error = slice_read_list_modification(out, bits);
		if (error != 0)
			return error;
	}

	/* Reads the flag of B slices that zeroes the second list's motion vector differences. */
	if (is_b != 0)
		out->mvd_l1_zero = (int)vcodec_bitreader_u1(bits);

	/* Reads which CABAC initialization table the slice uses, when the PPS lets it choose. */
	if (pps->cabac_init_present != 0)
		out->cabac_init = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Reads the collocated picture when temporal motion vector prediction is on. */
	if (out->temporal_mvp != 0) {
		error = slice_read_collocated(out, bits);
		if (error != 0)
			return error;
	}

	/* Reads the weighted prediction table when the PPS asks for it for this slice type. */
	weighted = 0;
	if (out->slice_type == H265SW_SLICE_P && pps->weighted_pred != 0)
		weighted = 1;
	if (out->slice_type == H265SW_SLICE_B && pps->weighted_bipred != 0)
		weighted = 1;
	if (weighted != 0) {
		error = slice_read_weights(out, bits, sps);
		if (error != 0)
			return error;
	}

	/* Reads the number of merge candidates, as five minus it. */
	five_minus_merge = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (five_minus_merge > SLICE_MAX_FIVE_MINUS_MERGE)
		return EINVAL;
	out->max_merge_cand = 5U - (unsigned)five_minus_merge;

	/* Succeeded: the inter fields are read. */
	return 0;
}

/* Reads ref_pic_lists_modification() (7.3.6.2): the entry of each list position in Ceil( Log2( NumPicTotalCurr ) ) bits. */
static int
slice_read_list_modification(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits)
{
	uint32_t entry;
	unsigned entry_bits;
	unsigned lists;
	unsigned list;
	unsigned index;

	/* A P slice has one list, a B slice two. */
	lists = 1U;
	if (out->slice_type == H265SW_SLICE_B)
		lists = 2U;
	entry_bits = slice_ceil_log2(out->num_pic_total_curr);

	/* Reads, for each list, whether it is given explicitly and then its entries. */
	for (list = 0U; list < lists; list++) {
		out->list_mod[list] = (int)vcodec_bitreader_u1(bits);
		if (bits->error != 0)
			return EINVAL;
		if (out->list_mod[list] == 0)
			continue;

		/* Reads one entry per list position; each names one of the pictures the picture may reference. */
		for (index = 0U; index < out->num_ref_idx[list]; index++) {
			entry = vcodec_bitreader_u(bits, entry_bits);
			if (bits->error != 0)
				return EINVAL;
			if (entry >= out->num_pic_total_curr)
				return EINVAL;
			out->list_entry[list][index] = (uint8_t)entry;
		}
	}

	/* Succeeded: the modification is read. */
	return 0;
}

/* Reads collocated_from_l0_flag and collocated_ref_idx (7.3.6.1) and checks the index against its list. */
static int
slice_read_collocated(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits)
{
	uint32_t ref_idx;
	unsigned list;

	/* A B slice says which list the collocated picture is in; a P slice always uses list 0. */
	if (out->slice_type == H265SW_SLICE_B)
		out->collocated_from_l0 = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	list = 0U;
	if (out->collocated_from_l0 == 0)
		list = 1U;

	/* The index is read only when its list has more than one picture; otherwise it is 0. */
	if (out->num_ref_idx[list] > 1U) {
		ref_idx = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (ref_idx >= out->num_ref_idx[list])
			return EINVAL;
		out->collocated_ref_idx = (unsigned)ref_idx;
	}

	/* Succeeded: the collocated picture is read. */
	return 0;
}

/* Reads pred_weight_table() (7.3.6.3) and derives the weights and offsets (7.4.7.3). */
static int
slice_read_weights(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps)
{
	uint32_t luma_denom;
	int32_t delta_denom;
	int64_t chroma_denom;
	unsigned chroma_array_type;
	int error;

	/* The offsets of the high precision range need limits this parse does not implement. */
	if (((sps->range_extension >> SLICE_HIGH_PRECISION_OFFSETS_BIT) & 1U) != 0U)
		return ENOTSUP;

	/* Reads the denominator of the luma weights. */
	luma_denom = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (luma_denom > (uint32_t)SLICE_MAX_WEIGHT_DENOM)
		return EINVAL;

	/* Reads the chroma denominator as a difference from it, when there is a chroma array. */
	chroma_array_type = sps->chroma_format_idc;
	if (sps->separate_colour_plane != 0)
		chroma_array_type = 0U;
	delta_denom = 0;
	if (chroma_array_type != 0U)
		delta_denom = vcodec_bitreader_se(bits);
	if (bits->error != 0)
		return EINVAL;
	chroma_denom = (int64_t)luma_denom + (int64_t)delta_denom;
	if (chroma_denom < 0 || chroma_denom > SLICE_MAX_WEIGHT_DENOM)
		return EINVAL;
	out->weights.luma_log2_denom = (unsigned)luma_denom;
	out->weights.chroma_log2_denom = (unsigned)chroma_denom;

	/* Reads the weights of list 0, and of list 1 for a B slice. */
	error = slice_read_weights_list(out, bits, chroma_array_type, 0U);
	if (error != 0)
		return error;
	if (out->slice_type == H265SW_SLICE_B) {
		error = slice_read_weights_list(out, bits, chroma_array_type, 1U);
		if (error != 0)
			return error;
	}

	/* Succeeded: the weights are read. */
	return 0;
}

/*
 * Reads the weights of one list: every luma flag, every chroma flag, then
 * the values of each entry whose flags are set.  The presence condition on
 * the order counts of the references is true for every entry (h265sw-slice.h).
 */
static int
slice_read_weights_list(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	unsigned chroma_array_type,
	unsigned list)
{
	uint32_t luma_flags[H265SW_MAX_REFS];
	uint32_t chroma_flags[H265SW_MAX_REFS];
	int32_t delta_luma_weight;
	int32_t luma_offset;
	int32_t delta_chroma_weight;
	int32_t delta_chroma_offset;
	int64_t luma_unit;
	int64_t chroma_unit;
	int64_t chroma_weight;
	int64_t offset;
	unsigned count;
	unsigned index;
	unsigned component;

	/* The weight an entry without a flag has: one in the fixed point of its denominator. */
	luma_unit = (int64_t)1 << out->weights.luma_log2_denom;
	chroma_unit = (int64_t)1 << out->weights.chroma_log2_denom;
	count = out->num_ref_idx[list];

	/* Reads the luma flag of each entry, then the chroma flag of each entry. */
	memset(luma_flags, 0, sizeof(luma_flags));
	memset(chroma_flags, 0, sizeof(chroma_flags));
	for (index = 0U; index < count; index++)
		luma_flags[index] = vcodec_bitreader_u1(bits);
	if (chroma_array_type != 0U) {
		for (index = 0U; index < count; index++)
			chroma_flags[index] = vcodec_bitreader_u1(bits);
	}
	if (bits->error != 0)
		return EINVAL;

	/* Reads the values of each entry and derives its weights and offsets. */
	for (index = 0U; index < count; index++) {
		/* Luma: a weight and an offset when the flag is set, else the unit weight and no offset. */
		out->weights.luma_weight[list][index] = (int16_t)luma_unit;
		out->weights.luma_offset[list][index] = 0;
		if (luma_flags[index] != 0U) {
			delta_luma_weight = vcodec_bitreader_se(bits);
			luma_offset = vcodec_bitreader_se(bits);
			if (bits->error != 0)
				return EINVAL;
			if (delta_luma_weight < SLICE_WEIGHT_MIN || delta_luma_weight > SLICE_WEIGHT_MAX)
				return EINVAL;
			if (luma_offset < SLICE_WEIGHT_MIN || luma_offset > SLICE_WEIGHT_MAX)
				return EINVAL;
			out->weights.luma_weight[list][index] = (int16_t)(luma_unit + (int64_t)delta_luma_weight);
			out->weights.luma_offset[list][index] = (int16_t)luma_offset;
		}

		/* Chroma has no values without a chroma array; with one it is the unit weight and no offset unless the flag is set. */
		if (chroma_array_type == 0U)
			continue;
		for (component = 0U; component < 2U; component++) {
			out->weights.chroma_weight[list][index][component] = (int16_t)chroma_unit;
			out->weights.chroma_offset[list][index][component] = 0;
		}
		if (chroma_flags[index] == 0U)
			continue;

		/* Reads the weight and the offset of each chroma component and derives the offset with 7-58. */
		for (component = 0U; component < 2U; component++) {
			delta_chroma_weight = vcodec_bitreader_se(bits);
			delta_chroma_offset = vcodec_bitreader_se(bits);
			if (bits->error != 0)
				return EINVAL;
			if (delta_chroma_weight < SLICE_WEIGHT_MIN || delta_chroma_weight > SLICE_WEIGHT_MAX)
				return EINVAL;
			if (delta_chroma_offset < SLICE_CHROMA_DELTA_OFFSET_MIN || delta_chroma_offset > SLICE_CHROMA_DELTA_OFFSET_MAX)
				return EINVAL;

			/* The weight, then the offset that cancels the weight's effect on the mid grey, clipped to the range (7-58). */
			chroma_weight = chroma_unit + (int64_t)delta_chroma_weight;
			offset = (int64_t)SLICE_CHROMA_HALF_RANGE + (int64_t)delta_chroma_offset - slice_floor_div((int64_t)SLICE_CHROMA_HALF_RANGE * chroma_weight, chroma_unit);
			if (offset < -(int64_t)SLICE_CHROMA_HALF_RANGE)
				offset = -(int64_t)SLICE_CHROMA_HALF_RANGE;
			if (offset > (int64_t)SLICE_CHROMA_HALF_RANGE - 1)
				offset = (int64_t)SLICE_CHROMA_HALF_RANGE - 1;
			out->weights.chroma_weight[list][index][component] = (int16_t)chroma_weight;
			out->weights.chroma_offset[list][index][component] = (int16_t)offset;
		}
	}

	/* Succeeded: the list's weights are read. */
	return 0;
}

/* Reads slice_qp_delta and the chroma QP offsets and derives SliceQpY (7-54). */
static int
slice_read_quantization(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps)
{
	int32_t qp_delta;
	int32_t cb_offset;
	int32_t cr_offset;
	int64_t qp;
	int64_t qp_min;

	/* Reads the difference from the PPS's initial QP. */
	qp_delta = vcodec_bitreader_se(bits);
	if (bits->error != 0)
		return EINVAL;

	/* SliceQpY lies in -QpBdOffsetY..51, with QpBdOffsetY 6 per bit of depth above 8. */
	qp = (int64_t)pps->init_qp + (int64_t)qp_delta;
	qp_min = -(int64_t)6 * ((int64_t)sps->bit_depth_luma - 8);
	if (qp < qp_min || qp > SLICE_QP_MAX)
		return EINVAL;
	out->qp = (int)qp;

	/* Reads the chroma QP offsets when the PPS lets slices have them. */
	if (pps->slice_chroma_qp_offsets_present != 0) {
		cb_offset = vcodec_bitreader_se(bits);
		cr_offset = vcodec_bitreader_se(bits);
		if (bits->error != 0)
			return EINVAL;

		/* Each offset, and its sum with the PPS's, lies in -12..12 (7.4.7.1). */
		if (cb_offset < -SLICE_QP_OFFSET_LIMIT || cb_offset > SLICE_QP_OFFSET_LIMIT)
			return EINVAL;
		if (cr_offset < -SLICE_QP_OFFSET_LIMIT || cr_offset > SLICE_QP_OFFSET_LIMIT)
			return EINVAL;
		if ((int64_t)pps->cb_qp_offset + (int64_t)cb_offset < -SLICE_QP_OFFSET_LIMIT)
			return EINVAL;
		if ((int64_t)pps->cb_qp_offset + (int64_t)cb_offset > SLICE_QP_OFFSET_LIMIT)
			return EINVAL;
		if ((int64_t)pps->cr_qp_offset + (int64_t)cr_offset < -SLICE_QP_OFFSET_LIMIT)
			return EINVAL;
		if ((int64_t)pps->cr_qp_offset + (int64_t)cr_offset > SLICE_QP_OFFSET_LIMIT)
			return EINVAL;
		out->cb_qp_offset = (int)cb_offset;
		out->cr_qp_offset = (int)cr_offset;
	}

	/* Succeeded: the quantization fields are read. */
	return 0;
}

/* Reads the deblocking override and the loop filter across slices flag (7.3.6.1). */
static int
slice_read_filters(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_pps *pps)
{
	int32_t beta_offset;
	int32_t tc_offset;

	/* Without an override the slice uses the PPS's deblocking parameters. */
	out->deblocking_disabled = pps->deblocking_disabled;
	out->beta_offset_div2 = pps->beta_offset_div2;
	out->tc_offset_div2 = pps->tc_offset_div2;

	/* Reads whether the slice overrides them. */
	if (pps->deblocking_override_enabled != 0)
		out->deblocking_override = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Reads the slice's own deblocking switch and, when it is on, the offsets. */
	if (out->deblocking_override != 0) {
		out->deblocking_disabled = (int)vcodec_bitreader_u1(bits);
		if (out->deblocking_disabled == 0) {
			beta_offset = vcodec_bitreader_se(bits);
			tc_offset = vcodec_bitreader_se(bits);
			if (bits->error != 0)
				return EINVAL;
			if (beta_offset < -SLICE_FILTER_OFFSET_LIMIT || beta_offset > SLICE_FILTER_OFFSET_LIMIT)
				return EINVAL;
			if (tc_offset < -SLICE_FILTER_OFFSET_LIMIT || tc_offset > SLICE_FILTER_OFFSET_LIMIT)
				return EINVAL;
			out->beta_offset_div2 = (int)beta_offset;
			out->tc_offset_div2 = (int)tc_offset;
		}
	}
	if (bits->error != 0)
		return EINVAL;

	/* The slice's flag is the PPS's unless some in-loop filter runs in the slice, in which case it is read. */
	out->loop_filter_across_slices = pps->loop_filter_across_slices;
	if (pps->loop_filter_across_slices != 0) {
		if (out->sao_luma != 0 || out->sao_chroma != 0 || out->deblocking_disabled == 0)
			out->loop_filter_across_slices = (int)vcodec_bitreader_u1(bits);
	}
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the filter controls are read. */
	return 0;
}

/* Reads num_entry_point_offsets and the offsets, which are checked against the bound of 7.4.7.1 and not kept. */
static int
slice_read_entry_points(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps)
{
	uint32_t count;
	uint32_t offset_len_minus1;
	int64_t limit;
	unsigned index;

	/* Tiles and wavefronts are the only things that make entry points. */
	if (pps->tiles == 0 && pps->entropy_sync == 0)
		return 0;

	/* Reads how many entry points the segment has. */
	count = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;

	/* The bound depends on whether tiles, wavefronts or both split the segment (7.4.7.1). */
	if (pps->tiles != 0 && pps->entropy_sync != 0) {
		limit = (int64_t)pps->tile_columns * (int64_t)sps->pic_height_ctbs - 1;
	} else if (pps->tiles != 0) {
		limit = (int64_t)pps->tile_columns * (int64_t)pps->tile_rows - 1;
	} else {
		limit = (int64_t)sps->pic_height_ctbs - 1;
	}
	if ((int64_t)count > limit)
		return EINVAL;
	out->num_entry_points = (unsigned)count;
	if (count == 0U)
		return 0;

	/* Reads the length of each offset, then the offsets, whose values the header parse does not need. */
	offset_len_minus1 = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (offset_len_minus1 > SLICE_MAX_OFFSET_LEN_MINUS1)
		return EINVAL;
	out->offset_len = (unsigned)offset_len_minus1 + 1U;
	for (index = 0U; index < out->num_entry_points && bits->error == 0; index++)
		(void)vcodec_bitreader_u(bits, out->offset_len);
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the entry points are read. */
	return 0;
}

/* Reads the length of the header extension and skips its bytes (7.3.6.1). */
static int
slice_read_extension(
	struct vcodec_bitreader *bits,
	const struct h265sw_pps *pps)
{
	uint32_t length;

	/* The extension is there only when the PPS says so. */
	if (pps->slice_header_extension_present == 0)
		return 0;

	/* Reads the length in bytes. */
	length = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (length > SLICE_MAX_EXTENSION)
		return EINVAL;

	/* Skips the bytes; running off the end of the RBSP is a broken header. */
	vcodec_bitreader_skip(bits, (size_t)length * 8U);
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the extension is skipped. */
	return 0;
}

/* Reads byte_alignment() (7.3.2.12): a one bit, then zero bits up to the next byte boundary. */
static int
slice_read_alignment(
	struct vcodec_bitreader *bits)
{
	uint32_t bit;
	int aligned;

	/* Reads alignment_bit_equal_to_one. */
	bit = vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (bit != 1U)
		return EINVAL;

	/* Reads the alignment_bit_equal_to_zero bits, at most seven. */
	aligned = vcodec_bitreader_aligned(bits);
	while (aligned == 0) {
		bit = vcodec_bitreader_u1(bits);
		if (bits->error != 0)
			return EINVAL;
		if (bit != 0U)
			return EINVAL;
		aligned = vcodec_bitreader_aligned(bits);
	}

	/* Succeeded: the reader is at the first byte of the slice segment data. */
	return 0;
}
