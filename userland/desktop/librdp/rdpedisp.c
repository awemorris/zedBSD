/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * MS-RDPEDISP Display Control dynamic channel PDU codecs.
 */

#include "rdp-internal.h"

/* Display Control PDU type identifiers. */
#define DISP_PDU_TYPE_MONITOR_LAYOUT	0x00000002U
#define DISP_PDU_TYPE_CAPS		0x00000005U

/*
 * Encodes a DISPLAYCONTROL_CAPS_PDU advertising single-monitor resize support.
 */
int
rdp_disp_encode_caps(
	struct rdp_buf *out)
{
	int error;

	/* Writes the 8-byte DISPLAYCONTROL_HEADER (total 20 bytes). */
	error = rdp_buf_append_u32_le(out, DISP_PDU_TYPE_CAPS);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 20U);
	if (error != RDP_OK)
		return error;

	/* Advertises 1 monitor up to 3840x2160 area factor. */
	error = rdp_buf_append_u32_le(out, 1U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 3840U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out, 2160U);
}

/*
 * Encodes a DISPLAYCONTROL_MONITOR_LAYOUT_PDU requesting width x height.
 */
int
rdp_disp_encode_monitor_layout(
	struct rdp_buf *out,
	uint32_t width,
	uint32_t height)
{
	int error;

	/* Writes the 8-byte header + 8-byte count + 40-byte layout = 56 bytes. */
	error = rdp_buf_append_u32_le(out, DISP_PDU_TYPE_MONITOR_LAYOUT);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 56U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 40U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 1U);
	if (error != RDP_OK)
		return error;

	/* Primary monitor at (0, 0) with 100% scale. */
	error = rdp_buf_append_u32_le(out, 1U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, width);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, height);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 100U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out, 100U);
}

/*
 * Decodes a DISPLAYCONTROL_MONITOR_LAYOUT_PDU and extracts primary dimensions.
 */
int
rdp_disp_decode_monitor_layout(
	const uint8_t *data,
	size_t len,
	uint32_t *width,
	uint32_t *height)
{
	struct rdp_reader r;
	uint32_t pdu_type;
	uint32_t pdu_len;
	uint32_t layout_sz;
	uint32_t num_mon;
	int error;

	/* Initializes the cursor and validates the Display Control header. */
	rdp_reader_init(&r, data, len);
	error = rdp_read_u32_le(&r, &pdu_type);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u32_le(&r, &pdu_len);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(pdu_len);
	if (pdu_type != DISP_PDU_TYPE_MONITOR_LAYOUT)
		return RDP_ERR_PROTO;
	error = rdp_read_u32_le(&r, &layout_sz);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u32_le(&r, &num_mon);
	if (error != RDP_OK)
		return error;
	if (layout_sz < 40U || num_mon == 0U)
		return RDP_ERR_PROTO;

	/* Skips Flags, Left, Top (12 bytes) to read Width and Height. */
	error = rdp_skip_bytes(&r, 12U);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u32_le(&r, width);
	if (error != RDP_OK)
		return error;
	return rdp_read_u32_le(&r, height);
}
