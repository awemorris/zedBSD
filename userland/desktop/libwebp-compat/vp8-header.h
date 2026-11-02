/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frame header of a lossy key frame (RFC 6386 sections 9 and 19.2):
 * the frame tag and size, the segmentation, the loop filter controls, the
 * token partitions, the quantizers and the dequantization factors, the
 * token probability updates and the skip probability.
 */

#ifndef LIBWEBP_COMPAT_VP8_HEADER_H
#define LIBWEBP_COMPAT_VP8_HEADER_H

#include "common.h"
#include "bool.h"
#include "vp8-tables.h"

/* The bytes of the frame tag and the key frame's start code and size (RFC 6386 9.1). */
#define WEBPC_VP8_TAG_SIZE		10U

/* The most token partitions (RFC 6386 9.5). */
#define WEBPC_VP8_PARTITIONS_MAX	8U

/* The segments (RFC 6386 9.3). */
#define WEBPC_VP8_SEGMENTS		4U

/*
 * The segmentation (RFC 6386 9.3, 19.2 update_segmentation).  enabled,
 * update_map and update_data are the flags as read (the latter two 0 when
 * not enabled); absolute is segment_feature_mode (1 absolute values, 0
 * deltas); quantizer and filter_level the per-segment values (0 when not
 * sent); tree_probs the segment id probabilities (255 when not sent).
 *
 * A key frame starts from the reference decoder's state (RFC 6386 20.4):
 * absolute 0 (deltas) and every value 0, so a frame with segmentation
 * enabled but update_data 0 decodes every segment with the frame's own
 * quantizer and filter level.  (libwebp starts from absolute 1 instead,
 * which gives quantizer index 0 there; no encoder writes such a frame.)
 */
struct webpc_vp8_segmentation {
	int enabled;
	int update_map;
	int update_data;
	int absolute;
	int32_t quantizer[WEBPC_VP8_SEGMENTS];
	int32_t filter_level[WEBPC_VP8_SEGMENTS];
	uint8_t tree_probs[3];
};

/*
 * The loop filter controls (RFC 6386 9.4, 19.2): simple is filter_type
 * (1 the simple filter, 0 the normal one), level loop_filter_level
 * (0..63), sharpness sharpness_level (0..7), deltas_enabled
 * loop_filter_adj_enable, ref_delta and mode_delta the four deltas each
 * (0 when never sent; the values a key frame sends replace them).
 */
struct webpc_vp8_filter_header {
	int simple;
	int level;
	int sharpness;
	int deltas_enabled;
	int32_t ref_delta[4];
	int32_t mode_delta[4];
};

/*
 * The dequantization factors of one segment (RFC 6386 14.1): index 0 for
 * the DC coefficient, 1 for the AC ones, of Y (y1), Y2 (y2) and chroma
 * (uv).
 */
struct webpc_vp8_dequant {
	int32_t y1[2];
	int32_t y2[2];
	int32_t uv[2];
};

/*
 * A key frame's header, filled by webpc_vp8_read_tag and
 * webpc_vp8_read_header in turn; it points into the caller's data
 * (partitions) and owns nothing.
 *
 *   width, height     The size (14 bits each; the scale bits are read
 *                     into horizontal_scale and vertical_scale and do not
 *                     change the decoding).
 *   version           The version (0..3).  Key frames decode alike in
 *                     every version (the filter type comes from the
 *                     header, 9.4; the reconstruction filter only
 *                     concerns inter prediction).
 *   first_size        The size of the first partition (19 bits).
 *   color_space, clamping_type
 *                     As read (9.2); neither changes the decoding (pixel
 *                     values are always clamped).
 *   segmentation, filter
 *                     As above.
 *   partition_count   1, 2, 4 or 8; partitions and partition_sizes the
 *                     token partitions (9.5).
 *   base_q, dq_*      y_ac_qi and the five deltas (9.6, 0 when not sent).
 *   dequant           The factors of each segment (all four filled; the
 *                     same when segmentation is off).
 *   coeff_probs       The token probabilities after the updates (9.9,
 *                     13.4).
 *   skip_enabled      mb_no_coeff_skip; skip_prob prob_skip_false (0 when
 *                     not enabled).
 */
struct webpc_vp8_header {
	unsigned width;
	unsigned height;
	unsigned horizontal_scale;
	unsigned vertical_scale;
	unsigned version;
	size_t first_size;
	int color_space;
	int clamping_type;
	struct webpc_vp8_segmentation segmentation;
	struct webpc_vp8_filter_header filter;
	unsigned partition_count;
	const uint8_t *partitions[WEBPC_VP8_PARTITIONS_MAX];
	size_t partition_sizes[WEBPC_VP8_PARTITIONS_MAX];
	int32_t base_q;
	int32_t dq_y1_dc;
	int32_t dq_y2_dc;
	int32_t dq_y2_ac;
	int32_t dq_uv_dc;
	int32_t dq_uv_ac;
	struct webpc_vp8_dequant dequant[WEBPC_VP8_SEGMENTS];
	uint8_t coeff_probs[WEBPC_VP8_COEFF_TYPES][WEBPC_VP8_COEFF_BANDS][WEBPC_VP8_COEFF_CONTEXTS][WEBPC_VP8_COEFF_NODES];
	int skip_enabled;
	unsigned skip_prob;
};

/*
 * Reads the uncompressed data chunk of a key frame (RFC 6386 9.1, 19.1)
 * from data (size bytes, the frame from its tag): the tag (key frame
 * bit 0 must be 0, version at most 3, show_frame 1, first partition
 * size), the start code 9d 01 2a, width and height (low 14 bits, not 0)
 * with their scale bits.  Clears header and fills those fields.  Returns
 * 0; ENOBUFS for fewer than WEBPC_VP8_TAG_SIZE bytes; EINVAL otherwise
 * (an interframe included).
 */
int webpc_vp8_read_tag(const uint8_t *data, size_t size, struct webpc_vp8_header *header);

/*
 * Reads the rest of the key frame's header (RFC 6386 19.2, in its order,
 * with 9.2 to 9.11): starts first on the first partition (data +
 * WEBPC_VP8_TAG_SIZE, header->first_size bytes, which must fit in size,
 * else ENOBUFS), reads color_space, clamping_type, the segmentation, the
 * filter controls, log2_nbr_of_dct_partitions, the quantizer indices,
 * refresh_entropy_probs (read and dropped), the token probability updates
 * from the defaults, and the skip flag and probability.  Leaves first
 * positioned at the first macroblock header.
 *
 * The token partitions (9.5) follow the first: (count - 1) sizes of 3
 * bytes little-endian (ENOBUFS when they do not fit), then the
 * partitions.  A size larger than what is left is cut to what is left
 * (as libwebp does); the last partition takes the rest and must not be
 * empty (ENOBUFS).
 *
 * The dequantization factors (14.1, 20.4 dequant_init) are built with
 * webpc_vp8_dequant_build for each segment, from base_q, or from the
 * segment's quantizer (absolute) or base_q plus it (delta).
 *
 * Returns 0; ENOBUFS as above or when first runs past its end
 * (webpc_bool_ended) by the end of the header.  On failure header is
 * unspecified.
 */
int webpc_vp8_read_header(const uint8_t *data, size_t size, struct webpc_vp8_header *header,
	struct webpc_bool *first);

/*
 * Builds one segment's dequantization factors from its quantizer index q
 * (any value, not clamped by itself) and the five deltas of header (RFC
 * 6386 14.1 and 20.4 dequant_init): y1 DC from q + dq_y1_dc, y1 AC from q,
 * y2 from q + dq_y2_dc and q + dq_y2_ac, uv from q + dq_uv_dc and q +
 * dq_uv_ac; each index is clamped to 0..127 and looked up (dc_q for the DC
 * factors, ac_q for the AC ones); then y2[0] is doubled, y2[1] is
 * multiplied by 155 and divided by 100 and raised to 8 when below, and
 * uv[0] is lowered to 132 when above.
 */
void webpc_vp8_dequant_build(int32_t q, const struct webpc_vp8_header *header, struct webpc_vp8_dequant *out);

#endif
