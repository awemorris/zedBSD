/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Fast-Path input event encoding and decoding (MS-RDPBCGR).
 */

#include "rdp-internal.h"

/*
 * Encodes a single Fast-Path keyboard scancode event.
 */
int
rdp_input_encode_key(
	struct rdp_buf *out,
	uint16_t scancode,
	uint16_t flags)
{
	uint8_t fp_flags;
	uint8_t hdr_byte;
	int error;

	/* Maps public key flags into the 5-bit Fast-Path eventFlags nibble. */
	fp_flags = 0U;
	if ((flags & RDP_KBD_FLAG_RELEASE) != 0U)
		fp_flags |= 0x01U;
	if ((flags & RDP_KBD_FLAG_EXTENDED) != 0U)
		fp_flags |= 0x02U;
	if ((flags & RDP_KBD_FLAG_EXTENDED1) != 0U)
		fp_flags |= 0x04U;

	/* Emits a 4-byte Fast-Path input PDU carrying 1 scancode event. */
	error = rdp_buf_append_u8(out, (uint8_t)(1U << 2));
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u8(out, 4U);
	if (error != RDP_OK)
		return error;
	hdr_byte = (uint8_t)((fp_flags & 0x1FU) | (RDP_FP_INPUT_SCANCODE << 5));
	error = rdp_buf_append_u8(out, hdr_byte);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u8(out, (uint8_t)(scancode & 0xFFU));
	if (error != RDP_OK)
		return error;

	/* Succeeded: Fast-Path keyboard scancode event encoded. */
	return RDP_OK;
}

/*
 * Encodes a single Fast-Path Unicode keyboard event.
 */
int
rdp_input_encode_unicode(
	struct rdp_buf *out,
	uint16_t codepoint,
	uint16_t flags)
{
	uint8_t fp_flags;
	uint8_t hdr_byte;
	int error;

	/* Maps release flag into the Fast-Path Unicode event header. */
	fp_flags = 0U;
	if ((flags & RDP_KBD_FLAG_RELEASE) != 0U)
		fp_flags |= 0x01U;

	/* Emits a 5-byte Fast-Path input PDU carrying 1 Unicode event. */
	error = rdp_buf_append_u8(out, (uint8_t)(1U << 2));
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u8(out, 5U);
	if (error != RDP_OK)
		return error;
	hdr_byte = (uint8_t)((fp_flags & 0x1FU) | (RDP_FP_INPUT_UNICODE << 5));
	error = rdp_buf_append_u8(out, hdr_byte);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, codepoint);
	if (error != RDP_OK)
		return error;

	/* Succeeded: Fast-Path Unicode event encoded. */
	return RDP_OK;
}

/*
 * Encodes a single Fast-Path mouse pointer event.
 */
int
rdp_input_encode_mouse(
	struct rdp_buf *out,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	uint8_t hdr_byte;
	int error;

	/* Emits a 9-byte Fast-Path input PDU carrying 1 mouse event. */
	error = rdp_buf_append_u8(out, (uint8_t)(1U << 2));
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u8(out, 9U);
	if (error != RDP_OK)
		return error;
	hdr_byte = (uint8_t)(RDP_FP_INPUT_MOUSE << 5);
	error = rdp_buf_append_u8(out, hdr_byte);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, flags);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, x);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, y);
	if (error != RDP_OK)
		return error;

	/* Succeeded: Fast-Path mouse event encoded. */
	return RDP_OK;
}

/*
 * Encodes a Fast-Path keyboard toggle synchronization event.
 */
int
rdp_input_encode_sync(
	struct rdp_buf *out,
	uint32_t toggle_flags)
{
	uint8_t hdr_byte;
	int error;

	/* Emits a 3-byte Fast-Path input PDU carrying 1 sync event. */
	error = rdp_buf_append_u8(out, (uint8_t)(1U << 2));
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u8(out, 3U);
	if (error != RDP_OK)
		return error;
	hdr_byte = (uint8_t)(((uint8_t)(toggle_flags & 0x1FU)) |
			     (RDP_FP_INPUT_SYNC << 5));
	error = rdp_buf_append_u8(out, hdr_byte);
	if (error != RDP_OK)
		return error;

	/* Succeeded: Fast-Path sync event encoded. */
	return RDP_OK;
}

/*
 * Decodes Fast-Path input events and dispatches them to callbacks.
 */
int
rdp_input_decode_events(
	const uint8_t *data,
	size_t len,
	uint8_t event_count,
	const struct rdp_callbacks *cb,
	void *user)
{
	struct rdp_reader r;
	uint8_t i;
	uint8_t ev_hdr;
	uint8_t ev_code;
	uint8_t ev_flags;
	uint8_t scancode;
	uint16_t codepoint;
	uint16_t ptr_flags;
	uint16_t x;
	uint16_t y;
	int error;

	/* Initializes the cursor over the Fast-Path event stream. */
	rdp_reader_init(&r, data, len);

	/* Decodes each event in order and invokes matching callbacks. */
	for (i = 0; i < event_count; i++) {
		error = rdp_read_u8(&r, &ev_hdr);
		if (error != RDP_OK)
			return error;
		ev_flags = (uint8_t)(ev_hdr & 0x1FU);
		ev_code = (uint8_t)((ev_hdr >> 5) & 0x07U);
		if (ev_code == RDP_FP_INPUT_SCANCODE) {
			error = rdp_read_u8(&r, &scancode);
			if (error != RDP_OK)
				return error;
			if (cb->on_input_key != NULL)
				cb->on_input_key(user, scancode, ev_flags);
		} else if (ev_code == RDP_FP_INPUT_UNICODE) {
			error = rdp_read_u16_le(&r, &codepoint);
			if (error != RDP_OK)
				return error;
			if (cb->on_input_unicode != NULL)
				cb->on_input_unicode(user, codepoint, ev_flags);
		} else if (ev_code == RDP_FP_INPUT_MOUSE) {
			error = rdp_read_u16_le(&r, &ptr_flags);
			if (error != RDP_OK)
				return error;
			error = rdp_read_u16_le(&r, &x);
			if (error != RDP_OK)
				return error;
			error = rdp_read_u16_le(&r, &y);
			if (error != RDP_OK)
				return error;
			if (cb->on_input_mouse != NULL)
				cb->on_input_mouse(user, ptr_flags, x, y);
		} else if (ev_code == RDP_FP_INPUT_SYNC) {
			if (cb->on_input_sync != NULL)
				cb->on_input_sync(user, (uint32_t)ev_flags);
		} else {
			/* Rejects unknown Fast-Path input event codes. */
			return RDP_ERR_PROTO;
		}
	}

	/* Succeeded: all Fast-Path input events dispatched. */
	return RDP_OK;
}
