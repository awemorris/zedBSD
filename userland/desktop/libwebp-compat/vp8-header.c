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

/*
 * Reads the uncompressed data chunk of a key frame (RFC 6386 9.1, 19.1) from
 * data (size bytes, the frame from its tag): the tag (key frame bit 0 must
 * be 0, version at most 3, show_frame 1, first partition size), the start
 * code 9d 01 2a, width and height (low 14 bits, not 0) with their scale
 * bits.
 */
int
webpc_vp8_read_tag(
	const uint8_t *data,
	size_t size,
	struct webpc_vp8_header *header)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(header);

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Reads the rest of the key frame's header (RFC 6386 19.2, in its order,
 * with 9.2 to 9.11): starts first on the first partition (data +
 * WEBPC_VP8_TAG_SIZE, header->first_size bytes, which must fit in size, else
 * ENOBUFS), reads color_space, clamping_type, the segmentation, the filter
 * controls, log2_nbr_of_dct_partitions, the quantizer indices,
 * refresh_entropy_probs (read and dropped), the token probability updates
 * from the defaults, and the skip flag and probability.
 */
int
webpc_vp8_read_header(
	const uint8_t *data,
	size_t size,
	struct webpc_vp8_header *header,
	struct webpc_bool *first)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(first);

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Builds one segment's dequantization factors from its quantizer index q
 * (any value, not clamped by itself) and the five deltas of header (RFC 6386
 * 14.1 and 20.4 dequant_init): y1 DC from q + dq_y1_dc, y1 AC from q, y2
 * from q + dq_y2_dc and q + dq_y2_ac, uv from q + dq_uv_dc and q + dq_uv_ac;
 * each index is clamped to 0..127 and looked up (dc_q for the DC factors,
 * ac_q for the AC ones); then y2[0] is doubled, y2[1] is multiplied by 155
 * and divided by 100 and raised to 8 when below, and uv[0] is lowered to 132
 * when above.
 */
void
webpc_vp8_dequant_build(
	int32_t q,
	const struct webpc_vp8_header *header,
	struct webpc_vp8_dequant *out)
{
	UNUSED_PARAMETER(q);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(out);

	/* Not implemented yet. */
}
