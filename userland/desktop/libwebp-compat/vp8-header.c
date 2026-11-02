/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frame header of a lossy key frame.
 */

#include "vp8-header.h"

#include <errno.h>
#include <string.h>

/* The start code that follows the frame tag of a key frame (RFC 6386 9.1). */
#define HEADER_START_CODE_0		0x9dU
#define HEADER_START_CODE_1		0x01U
#define HEADER_START_CODE_2		0x2aU

/* The highest version number a key frame may carry (RFC 6386 9.1). */
#define HEADER_VERSION_MAX		3U

/* The bytes of one token partition size in the partition table (RFC 6386 9.5). */
#define HEADER_PARTITION_ENTRY		3U

/* The quantizer index range that the lookup tables cover (RFC 6386 14.1). */
#define HEADER_QINDEX_MAX		127

/*
 * The range a quantizer index is first limited to before a delta of at most
 * 15 is added: any index above 142 or below -15 ends clamped at 127 or 0
 * whatever the delta is, so limiting it early changes no result and keeps
 * the sum from overflowing.
 */
#define HEADER_QINDEX_GUARD		256

/* Forward declarations of the static functions. */
static int32_t header_clamp_q(int32_t q);
static int32_t header_read_optional_signed(struct webpc_bool *first, unsigned bits);
static void header_read_segmentation(struct webpc_bool *first, struct webpc_vp8_segmentation *segmentation);
static void header_read_filter(struct webpc_bool *first, struct webpc_vp8_filter_header *filter);
static void header_read_quant(struct webpc_bool *first, struct webpc_vp8_header *header);
static void header_read_coeff_probs(struct webpc_bool *first, struct webpc_vp8_header *header);
static int header_read_partitions(struct webpc_vp8_header *header, const uint8_t *rest, size_t rest_size);
static void header_build_dequant(struct webpc_vp8_header *header);

/*
 * Reads the uncompressed data chunk of a key frame (RFC 6386 9.1, 19.1) from
 * data (size bytes, the frame from its tag): the tag (key frame bit 0 must
 * be 0, version at most 3, show_frame 1, first partition size), the start
 * code 9d 01 2a, width and height (low 14 bits, not 0) with their scale
 * bits.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 9.1 and 19.1 (the
 * frame tag and the key frame start code, width and height).
 */
int
webpc_vp8_read_tag(
	const uint8_t *data,
	size_t size,
	struct webpc_vp8_header *header)
{
	uint32_t tag;
	uint32_t width_field;
	uint32_t height_field;
	unsigned version;

	/* Starts from a header with nothing read. */
	memset(header, 0, sizeof(*header));

	/* A frame shorter than its tag, start code and size is cut short. */
	if (size < WEBPC_VP8_TAG_SIZE)
		return ENOBUFS;

	/* Reads the 3 byte tag: bit 0 is 0 for a key frame, then version, show_frame and the size. */
	tag = webpc_le24(data);

	/* Refuses an interframe: only key frames are decoded. */
	if ((tag & 1U) != 0U)
		return EINVAL;

	/* Refuses a version that the format does not define. */
	version = (unsigned)((tag >> 1) & 7U);
	if (version > HEADER_VERSION_MAX)
		return EINVAL;

	/* Refuses a frame that is not meant to be shown. */
	if (((tag >> 4) & 1U) == 0U)
		return EINVAL;

	/* Refuses a key frame without its start code. */
	if (data[3] != HEADER_START_CODE_0 ||
	    data[4] != HEADER_START_CODE_1 ||
	    data[5] != HEADER_START_CODE_2)
		return EINVAL;

	/* Reads the two 16 bit fields that hold 14 bits of size and 2 bits of scale each. */
	width_field = webpc_le16(data + 6);
	height_field = webpc_le16(data + 8);

	/* Keeps the fields of the tag and the size. */
	header->version = version;
	header->first_size = (size_t)(tag >> 5);
	header->width = (unsigned)(width_field & 0x3fffU);
	header->horizontal_scale = (unsigned)(width_field >> 14);
	header->height = (unsigned)(height_field & 0x3fffU);
	header->vertical_scale = (unsigned)(height_field >> 14);

	/* Refuses an image without area. */
	if (header->width == 0U)
		return EINVAL;
	if (header->height == 0U)
		return EINVAL;

	/* Succeeded: the tag and size are in header. */
	return 0;
}

/*
 * Reads the rest of the key frame's header (RFC 6386 19.2, in its order,
 * with 9.2 to 9.11): starts first on the first partition (data +
 * WEBPC_VP8_TAG_SIZE, header->first_size bytes, which must fit in size, else
 * ENOBUFS), reads color_space, clamping_type, the segmentation, the filter
 * controls, log2_nbr_of_dct_partitions, the quantizer indices,
 * refresh_entropy_probs (read and dropped), the token probability updates
 * from the defaults, and the skip flag and probability.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 9.2 to 9.6, 9.9,
 * 9.11 and 19.2 (the order of the key frame header), and section 20.4
 * (decode_frame, the order of the partitions and the dequantization).
 */
int
webpc_vp8_read_header(
	const uint8_t *data,
	size_t size,
	struct webpc_vp8_header *header,
	struct webpc_bool *first)
{
	size_t after_tag;
	size_t after_first;
	uint32_t literal;
	int error;

	/* The first partition must lie inside the data after the tag. */
	if (size < WEBPC_VP8_TAG_SIZE)
		return ENOBUFS;
	after_tag = size - WEBPC_VP8_TAG_SIZE;
	if (header->first_size > after_tag)
		return ENOBUFS;
	after_first = after_tag - header->first_size;

	/* Starts the boolean decoder at the first partition. */
	webpc_bool_init(first, data + WEBPC_VP8_TAG_SIZE, header->first_size);

	/* Reads color_space and clamping_type; neither changes the decoding. */
	literal = webpc_bool_literal(first, 1U);
	header->color_space = (int)literal;
	literal = webpc_bool_literal(first, 1U);
	header->clamping_type = (int)literal;

	/* Reads the segmentation, the loop filter controls and the partition count. */
	header_read_segmentation(first, &header->segmentation);
	header_read_filter(first, &header->filter);
	literal = webpc_bool_literal(first, 2U);
	header->partition_count = 1U << literal;

	/* Reads the quantizer indices. */
	header_read_quant(first, header);

	/* Reads refresh_entropy_probs and drops it: a still image has no next frame. */
	literal = webpc_bool_literal(first, 1U);
	UNUSED_PARAMETER(literal);

	/* Reads the token probability updates, starting from the defaults. */
	header_read_coeff_probs(first, header);

	/* Reads mb_no_coeff_skip and, when it is set, prob_skip_false. */
	literal = webpc_bool_literal(first, 1U);
	header->skip_enabled = (int)literal;
	header->skip_prob = 0U;
	if (header->skip_enabled != 0) {
		literal = webpc_bool_literal(first, 8U);
		header->skip_prob = (unsigned)literal;
	}

	/* A header that read past the first partition's end was cut short. */
	if (webpc_bool_ended(first) != 0)
		return ENOBUFS;

	/* Splits what follows the first partition into the token partitions. */
	error = header_read_partitions(header, data + WEBPC_VP8_TAG_SIZE + header->first_size, after_first);
	if (error != 0)
		return error;

	/* Builds the dequantization factors of each segment. */
	header_build_dequant(header);

	/* Succeeded: the header is complete and first stands at the first macroblock header. */
	return 0;
}

/*
 * Builds one segment's dequantization factors from its quantizer index q
 * (any value, not clamped by itself) and the five deltas of header (RFC
 * 6386 14.1 and 20.4 dequant_init): y1 DC from q + dq_y1_dc, y1 AC from q,
 * y2 from q + dq_y2_dc and q + dq_y2_ac, uv from q + dq_uv_dc and q +
 * dq_uv_ac; each index is clamped to 0..127 and looked up (dc_q for the DC
 * factors, ac_q for the AC ones); then y2[0] is doubled, y2[1] is
 * multiplied by 155 and divided by 100 and raised to 8 when below, and
 * uv[0] is lowered to 132 when above.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 14.1 (the
 * dequantization indices and the adjusted Y2 and chroma factors) and
 * section 20.4 (dequant_init).
 */
void
webpc_vp8_dequant_build(
	int32_t q,
	const struct webpc_vp8_header *header,
	struct webpc_vp8_dequant *out)
{
	int32_t y1_dc_index;
	int32_t y2_dc_index;
	int32_t y2_ac_index;
	int32_t uv_dc_index;
	int32_t uv_ac_index;
	int32_t y2_ac;

	/* Limits q early so that adding a delta cannot overflow; no result changes. */
	if (q > HEADER_QINDEX_GUARD)
		q = HEADER_QINDEX_GUARD;
	if (q < -HEADER_QINDEX_GUARD)
		q = -HEADER_QINDEX_GUARD;

	/* Clamps each index to the range of the lookup tables. */
	y1_dc_index = header_clamp_q(q + header->dq_y1_dc);
	y2_dc_index = header_clamp_q(q + header->dq_y2_dc);
	y2_ac_index = header_clamp_q(q + header->dq_y2_ac);
	uv_dc_index = header_clamp_q(q + header->dq_uv_dc);
	uv_ac_index = header_clamp_q(q + header->dq_uv_ac);

	/* Looks up the factors of the Y plane; its AC index is q itself. */
	out->y1[0] = (int32_t)webpc_vp8_dc_q[y1_dc_index];
	out->y1[1] = (int32_t)webpc_vp8_ac_q[header_clamp_q(q)];

	/* Looks up the Y2 factors: the DC one is doubled, the AC one scaled by 155/100 with a floor of 8. */
	out->y2[0] = (int32_t)webpc_vp8_dc_q[y2_dc_index] * 2;
	y2_ac = (int32_t)webpc_vp8_ac_q[y2_ac_index] * 155 / 100;
	if (y2_ac < 8)
		y2_ac = 8;
	out->y2[1] = y2_ac;

	/* Looks up the chroma factors: the DC one is capped at 132. */
	out->uv[0] = (int32_t)webpc_vp8_dc_q[uv_dc_index];
	if (out->uv[0] > 132)
		out->uv[0] = 132;
	out->uv[1] = (int32_t)webpc_vp8_ac_q[uv_ac_index];
}

/* Clamps a quantizer index to 0..127 (RFC 6386 14.1). */
static int32_t
header_clamp_q(
	int32_t q)
{
	/* Limits the index to the lookup tables' first and last entries. */
	if (q < 0)
		return 0;
	if (q > HEADER_QINDEX_MAX)
		return HEADER_QINDEX_MAX;

	/* The index is already inside the tables. */
	return q;
}

/*
 * Reads a value that may be absent: a flag bit, and when it is set a
 * magnitude of bits bits and a sign; an absent value is 0 (RFC 6386 9.3,
 * 9.4, 9.6).
 */
static int32_t
header_read_optional_signed(
	struct webpc_bool *first,
	unsigned bits)
{
	uint32_t flag;
	int32_t value;

	/* Reads the flag that says whether the value is sent. */
	flag = webpc_bool_literal(first, 1U);
	if (flag == 0U)
		return 0;

	/* Reads the magnitude and the sign. */
	value = webpc_bool_signed(first, bits);

	/* The value that was sent. */
	return value;
}

/*
 * Reads segmentation_enabled and update_segmentation() (RFC 6386 9.3,
 * 19.2); the values start from the reference decoder's key frame state.
 */
static void
header_read_segmentation(
	struct webpc_bool *first,
	struct webpc_vp8_segmentation *segmentation)
{
	uint32_t literal;
	unsigned index;

	/* A key frame starts from no segmentation, no values and unsent probabilities. */
	memset(segmentation, 0, sizeof(*segmentation));
	segmentation->tree_probs[0] = 255U;
	segmentation->tree_probs[1] = 255U;
	segmentation->tree_probs[2] = 255U;

	/* Reads segmentation_enabled; the rest is only sent when it is set. */
	literal = webpc_bool_literal(first, 1U);
	segmentation->enabled = (int)literal;
	if (segmentation->enabled == 0)
		return;

	/* Reads whether the segment map and the segment values are updated. */
	literal = webpc_bool_literal(first, 1U);
	segmentation->update_map = (int)literal;
	literal = webpc_bool_literal(first, 1U);
	segmentation->update_data = (int)literal;

	/* Reads the segment values: the mode, then four quantizers and four filter levels. */
	if (segmentation->update_data != 0) {
		literal = webpc_bool_literal(first, 1U);
		segmentation->absolute = (int)literal;

		/* Reads the quantizer of each segment: 7 bits and a sign. */
		for (index = 0U; index < WEBPC_VP8_SEGMENTS; index++)
			segmentation->quantizer[index] = header_read_optional_signed(first, 7U);

		/* Reads the filter level of each segment: 6 bits and a sign. */
		for (index = 0U; index < WEBPC_VP8_SEGMENTS; index++)
			segmentation->filter_level[index] = header_read_optional_signed(first, 6U);
	}

	/* Reads the probabilities of the segment id tree: a flag and 8 bits each, 255 when unsent. */
	if (segmentation->update_map != 0) {
		for (index = 0U; index < 3U; index++) {
			literal = webpc_bool_literal(first, 1U);
			if (literal != 0U) {
				literal = webpc_bool_literal(first, 8U);
				segmentation->tree_probs[index] = (uint8_t)literal;
			}
		}
	}
}

/*
 * Reads filter_type, loop_filter_level, sharpness_level and
 * mb_lf_adjustments() (RFC 6386 9.4, 19.2).
 */
static void
header_read_filter(
	struct webpc_bool *first,
	struct webpc_vp8_filter_header *filter)
{
	uint32_t literal;
	unsigned index;

	/* A key frame starts from no deltas. */
	memset(filter, 0, sizeof(*filter));

	/* Reads the filter type, level and sharpness. */
	literal = webpc_bool_literal(first, 1U);
	filter->simple = (int)literal;
	literal = webpc_bool_literal(first, 6U);
	filter->level = (int)literal;
	literal = webpc_bool_literal(first, 3U);
	filter->sharpness = (int)literal;

	/* Reads loop_filter_adj_enable; the deltas only follow when it is set. */
	literal = webpc_bool_literal(first, 1U);
	filter->deltas_enabled = (int)literal;
	if (filter->deltas_enabled == 0)
		return;

	/* Reads mode_ref_lf_delta_update; the values only follow when it is set. */
	literal = webpc_bool_literal(first, 1U);
	if (literal == 0U)
		return;

	/* Reads the four reference frame deltas, then the four mode deltas: 6 bits and a sign each. */
	for (index = 0U; index < 4U; index++)
		filter->ref_delta[index] = header_read_optional_signed(first, 6U);
	for (index = 0U; index < 4U; index++)
		filter->mode_delta[index] = header_read_optional_signed(first, 6U);
}

/* Reads quant_indices(): y_ac_qi and the five optional deltas (RFC 6386 9.6, 19.2). */
static void
header_read_quant(
	struct webpc_bool *first,
	struct webpc_vp8_header *header)
{
	uint32_t literal;

	/* Reads the base index, which is also the Y AC index. */
	literal = webpc_bool_literal(first, 7U);
	header->base_q = (int32_t)literal;

	/* Reads the deltas in the order of the format: 4 bits and a sign each, 0 when absent. */
	header->dq_y1_dc = header_read_optional_signed(first, 4U);
	header->dq_y2_dc = header_read_optional_signed(first, 4U);
	header->dq_y2_ac = header_read_optional_signed(first, 4U);
	header->dq_uv_dc = header_read_optional_signed(first, 4U);
	header->dq_uv_ac = header_read_optional_signed(first, 4U);
}

/*
 * Reads token_prob_update() starting from the default probabilities: for
 * each probability a bool with the probability of updating it, and when it
 * is 1 the new value in 8 bits (RFC 6386 13.4, 19.2).
 *
 * Procedure referenced: RFC 6386 (November 2011) section 13.4.
 */
static void
header_read_coeff_probs(
	struct webpc_bool *first,
	struct webpc_vp8_header *header)
{
	uint32_t literal;
	int update;
	unsigned type;
	unsigned band;
	unsigned context;
	unsigned node;

	/* Starts from the default probabilities. */
	memcpy(header->coeff_probs, webpc_vp8_coeff_default, sizeof(header->coeff_probs));

	/* Visits every probability in the order of the format. */
	for (type = 0U; type < WEBPC_VP8_COEFF_TYPES; type++) {
		for (band = 0U; band < WEBPC_VP8_COEFF_BANDS; band++) {
			for (context = 0U; context < WEBPC_VP8_COEFF_CONTEXTS; context++) {
				for (node = 0U; node < WEBPC_VP8_COEFF_NODES; node++) {
					/* Reads whether this probability is updated, with its own probability. */
					update = webpc_bool_read(first, webpc_vp8_coeff_update[type][band][context][node]);
					if (update != 0) {
						/* Takes the new probability. */
						literal = webpc_bool_literal(first, 8U);
						header->coeff_probs[type][band][context][node] = (uint8_t)literal;
					}
				}
			}
		}
	}
}

/*
 * Splits the data after the first partition into the token partitions: the
 * table of 3 byte little-endian sizes of all but the last, then the
 * partitions (RFC 6386 9.5).  A size that passes the end is cut to what is
 * left; the last partition takes the rest and must not be empty.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 9.5 and section
 * 20.4 (decode_and_init_token_partitions).
 */
static int
header_read_partitions(
	struct webpc_vp8_header *header,
	const uint8_t *rest,
	size_t rest_size)
{
	const uint8_t *cursor;
	size_t table_size;
	size_t remaining;
	size_t part_size;
	unsigned last;
	unsigned index;

	/* The table of sizes must fit in what follows the first partition. */
	last = header->partition_count - 1U;
	table_size = (size_t)HEADER_PARTITION_ENTRY * (size_t)last;
	if (rest_size < table_size)
		return ENOBUFS;

	/* The partitions start after the table; what is left of the data is theirs. */
	cursor = rest + table_size;
	remaining = rest_size - table_size;

	/* Cuts each partition but the last at its size, or at the end of the data. */
	for (index = 0U; index < last; index++) {
		part_size = (size_t)webpc_le24(rest + (size_t)HEADER_PARTITION_ENTRY * (size_t)index);
		if (part_size > remaining)
			part_size = remaining;
		header->partitions[index] = cursor;
		header->partition_sizes[index] = part_size;
		cursor += part_size;
		remaining -= part_size;
	}

	/* The last partition takes the rest and cannot be empty. */
	if (remaining == 0U)
		return ENOBUFS;
	header->partitions[last] = cursor;
	header->partition_sizes[last] = remaining;

	/* Succeeded: every partition is placed. */
	return 0;
}

/*
 * Builds the dequantization factors of the four segments: from base_q when
 * segmentation is off, otherwise from each segment's quantizer, as it is
 * (absolute) or added to base_q (delta); the index is not clamped here
 * (RFC 6386 20.4 dequant_init).
 */
static void
header_build_dequant(
	struct webpc_vp8_header *header)
{
	int32_t q;
	unsigned index;

	/* Builds the factors of each segment. */
	for (index = 0U; index < WEBPC_VP8_SEGMENTS; index++) {
		q = header->base_q;
		if (header->segmentation.enabled != 0) {
			if (header->segmentation.absolute != 0)
				q = header->segmentation.quantizer[index];
			else
				q = header->base_q + header->segmentation.quantizer[index];
		}
		webpc_vp8_dequant_build(q, header, &header->dequant[index]);
	}
}
