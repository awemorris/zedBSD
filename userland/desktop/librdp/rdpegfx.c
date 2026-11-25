/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * MS-RDPEGFX graphics pipeline PDU codecs (H.264 AVC420 and AVC444).
 */

#include "rdp-internal.h"

#include <string.h>

static int encode_gfx_hdr(struct rdp_buf *out, uint16_t cmd_id, size_t body_len);
static int encode_avc420_layer(struct rdp_buf *out, uint16_t width, uint16_t height, const struct rdp_avc_layer *layer);
static int decode_avc420_layer(const uint8_t *data, size_t len, struct rdp_avc_layer *layer, struct rdp_avc_rect *rects, struct rdp_avc_quant *quants, uint32_t max_rects);

/*
 * Encodes an RDPGFX_CAPS_ADVERTISE_PDU offering versions 106, 104, and 81.
 */
int
rdp_gfx_encode_caps_advertise(
	struct rdp_buf *out)
{
	int error;

	/* Writes the 8-byte header (2 + 3 * 12 = 38 bytes body). */
	error = encode_gfx_hdr(out, RDPGFX_CMD_CAPS_ADVERTISE, 38U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 3U);
	if (error != RDP_OK)
		return error;

	/* Capset 1: RDPGFX_CAPVERSION_106 with THINCLIENT. */
	error = rdp_buf_append_u32_le(out, RDPGFX_CAPVERSION_106);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 4U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, RDPGFX_CAPS_FLAG_THINCLIENT);
	if (error != RDP_OK)
		return error;

	/* Capset 2: RDPGFX_CAPVERSION_104 with THINCLIENT. */
	error = rdp_buf_append_u32_le(out, RDPGFX_CAPVERSION_104);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 4U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, RDPGFX_CAPS_FLAG_THINCLIENT);
	if (error != RDP_OK)
		return error;

	/* Capset 3: RDPGFX_CAPVERSION_81 with THINCLIENT | AVC420_ENABLED. */
	error = rdp_buf_append_u32_le(out, RDPGFX_CAPVERSION_81);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 4U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out,
				     RDPGFX_CAPS_FLAG_THINCLIENT |
				     RDPGFX_CAPS_FLAG_AVC420_ENABLED);
}

/*
 * Decodes an RDPGFX_CAPS_ADVERTISE_PDU and picks the highest supported version.
 */
int
rdp_gfx_decode_caps_advertise(
	const uint8_t *data,
	size_t len,
	uint32_t *best_version,
	uint32_t *best_flags)
{
	struct rdp_reader r;
	uint16_t count;
	uint16_t i;
	uint32_t ver;
	uint32_t dlen;
	uint32_t cflags;
	uint32_t chosen;
	uint32_t chosen_flags;
	int error;

	/* Initializes the cursor over the advertise body. */
	rdp_reader_init(&r, data, len);
	chosen = 0U;
	chosen_flags = 0U;
	error = rdp_read_u16_le(&r, &count);
	if (error != RDP_OK)
		return error;

	/* Scans advertised capsets and selects 106, 104, or 81. */
	for (i = 0; i < count; i++) {
		error = rdp_read_u32_le(&r, &ver);
		if (error != RDP_OK)
			return error;
		error = rdp_read_u32_le(&r, &dlen);
		if (error != RDP_OK)
			return error;
		cflags = 0U;
		if (dlen >= 4U) {
			error = rdp_read_u32_le(&r, &cflags);
			if (error != RDP_OK)
				return error;
			if (dlen > 4U) {
				error = rdp_skip_bytes(&r, dlen - 4U);
				if (error != RDP_OK)
					return error;
			}
		} else if (dlen > 0U) {
			error = rdp_skip_bytes(&r, dlen);
			if (error != RDP_OK)
				return error;
		}
		if (ver == RDPGFX_CAPVERSION_106 ||
		    ver == RDPGFX_CAPVERSION_104 ||
		    ver == RDPGFX_CAPVERSION_81) {
			if (ver > chosen) {
				chosen = ver;
				chosen_flags = cflags;
			}
		}
	}
	if (chosen == 0U)
		return RDP_ERR_UNSUPPORTED;
	*best_version = chosen;
	if (best_flags != NULL)
		*best_flags = chosen_flags;

	/* Succeeded: compatible RDPEGFX capability version chosen. */
	return RDP_OK;
}

/*
 * Encodes an RDPGFX_CAPS_CONFIRM_PDU confirming version.
 */
int
rdp_gfx_encode_caps_confirm(
	struct rdp_buf *out,
	uint32_t version,
	uint32_t client_flags)
{
	uint32_t flags;
	int error;

	/* Only confirms capability flags that the client actually advertised. */
	flags = client_flags & RDPGFX_CAPS_FLAG_THINCLIENT;
	if (version == RDPGFX_CAPVERSION_81)
		flags |= (client_flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED);
	error = encode_gfx_hdr(out, RDPGFX_CMD_CAPS_CONFIRM, 12U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, version);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 4U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out, flags);
}

/*
 * Decodes an RDPGFX_CAPS_CONFIRM_PDU and extracts the confirmed version.
 */
int
rdp_gfx_decode_caps_confirm(
	const uint8_t *data,
	size_t len,
	uint32_t *version)
{
	struct rdp_reader r;
	uint32_t dlen;
	int error;

	/* Reads the confirmed version field from the capset. */
	rdp_reader_init(&r, data, len);
	error = rdp_read_u32_le(&r, version);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u32_le(&r, &dlen);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(dlen);

	/* Succeeded: RDPEGFX capabilities confirmed. */
	return RDP_OK;
}

/*
 * Encodes an RDPGFX_RESET_GRAPHICS_PDU (340 bytes total) for one monitor.
 */
int
rdp_gfx_encode_reset_graphics(
	struct rdp_buf *out,
	uint32_t width,
	uint32_t height)
{
	int error;

	/* Writes the 8-byte header and 332-byte body (total 340 bytes). */
	error = encode_gfx_hdr(out, RDPGFX_CMD_RESET_GRAPHICS, 332U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, width);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, height);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 1U);
	if (error != RDP_OK)
		return error;

	/* Monitor 0: primary monitor covering (0, 0) to (width - 1, height - 1). */
	error = rdp_buf_append_u32_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, width > 0U ? width - 1U : 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, height > 0U ? height - 1U : 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 1U);
	if (error != RDP_OK)
		return error;

	/* Pads the remainder of the fixed 340-byte RESET_GRAPHICS PDU. */
	return rdp_buf_append_zeros(out, 300U);
}

/*
 * Encodes an RDPGFX_CREATE_SURFACE_PDU.
 */
int
rdp_gfx_encode_create_surface(
	struct rdp_buf *out,
	uint16_t surface_id,
	uint16_t width,
	uint16_t height)
{
	int error;
	error = encode_gfx_hdr(out, RDPGFX_CMD_CREATE_SURFACE, 7U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, surface_id);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, width);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, height);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u8(out, RDPGFX_PIXEL_XRGB_8888);
}

/*
 * Encodes an RDPGFX_DELETE_SURFACE_PDU.
 */
int
rdp_gfx_encode_delete_surface(
	struct rdp_buf *out,
	uint16_t surface_id)
{
	int error;
	error = encode_gfx_hdr(out, RDPGFX_CMD_DELETE_SURFACE, 2U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u16_le(out, surface_id);
}

/*
 * Encodes an RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU at origin (0, 0).
 */
int
rdp_gfx_encode_map_surface(
	struct rdp_buf *out,
	uint16_t surface_id)
{
	int error;
	error = encode_gfx_hdr(out, RDPGFX_CMD_MAP_SURFACE_OUTPUT, 12U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, surface_id);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 0U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out, 0U);
}

/*
 * Encodes an RDPGFX_START_FRAME_PDU.
 */
int
rdp_gfx_encode_start_frame(
	struct rdp_buf *out,
	uint32_t timestamp_ms,
	uint32_t frame_id)
{
	int error;
	error = encode_gfx_hdr(out, RDPGFX_CMD_START_FRAME, 8U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, timestamp_ms);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out, frame_id);
}

/*
 * Encodes an RDPGFX_END_FRAME_PDU.
 */
int
rdp_gfx_encode_end_frame(
	struct rdp_buf *out,
	uint32_t frame_id)
{
	int error;
	error = encode_gfx_hdr(out, RDPGFX_CMD_END_FRAME, 4U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out, frame_id);
}

/*
 * Encodes an RDPGFX_WIRE_TO_SURFACE_1_PDU carrying AVC420 or AVC444 data.
 */
int
rdp_gfx_encode_wire_to_surface_avc(
	struct rdp_buf *out,
	uint16_t surface_id,
	uint16_t codec_id,
	uint16_t width,
	uint16_t height,
	const struct rdp_avc_stream *avc)
{
	struct rdp_buf bmp;
	struct rdp_buf s1;
	uint32_t lc;
	int error;

	/* Rejects unsupported codec identifiers. */
	if (codec_id != RDP_GFX_CODEC_AVC420 && codec_id != RDP_GFX_CODEC_AVC444)
		return RDP_ERR_INVAL;
	rdp_buf_init(&bmp);
	rdp_buf_init(&s1);

	/* Builds the AVC420 or AVC444 bitmapData payload. */
	if (codec_id == RDP_GFX_CODEC_AVC420) {
		error = encode_avc420_layer(&bmp, width, height, &avc->luma);
		if (error != RDP_OK)
			goto fail;
	} else {
		if (avc->avc444_mode == RDP_AVC444_MODE_CHROMA_ONLY) {
			error = encode_avc420_layer(&s1, width, height, &avc->chroma);
		} else {
			error = encode_avc420_layer(&s1, width, height, &avc->luma);
		}
		if (error != RDP_OK)
			goto fail;
		lc = (uint32_t)(avc->avc444_mode & 0x03U);
		if (avc->avc444_mode == RDP_AVC444_MODE_LUMA_CHROMA)
			lc |= ((uint32_t)s1.len << 2);
		error = rdp_buf_append_u32_le(&bmp, lc);
		if (error != RDP_OK)
			goto fail;
		error = rdp_buf_append(&bmp, s1.data, s1.len);
		if (error != RDP_OK)
			goto fail;
		if (avc->avc444_mode == RDP_AVC444_MODE_LUMA_CHROMA) {
			error = encode_avc420_layer(&bmp, width, height,
						    &avc->chroma);
			if (error != RDP_OK)
				goto fail;
		}
	}

	/* Writes the WIRE_TO_SURFACE_1 header (17 bytes + bitmapData). */
	error = encode_gfx_hdr(out, RDPGFX_CMD_WIRE_TO_SURFACE_1, 17U + bmp.len);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, surface_id);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, codec_id);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u8(out, RDPGFX_PIXEL_XRGB_8888);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, width);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, height);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u32_le(out, (uint32_t)bmp.len);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append(out, bmp.data, bmp.len);
	if (error != RDP_OK)
		goto fail;
	rdp_buf_free(&bmp);
	rdp_buf_free(&s1);

	/* Succeeded: WIRE_TO_SURFACE_1 AVC PDU encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&bmp);
	rdp_buf_free(&s1);
	return error;
}

/*
 * Encodes an RDPGFX_FRAME_ACKNOWLEDGE_PDU.
 */
int
rdp_gfx_encode_frame_ack(
	struct rdp_buf *out,
	uint32_t queue_depth,
	uint32_t frame_id,
	uint32_t total_decoded)
{
	int error;
	error = encode_gfx_hdr(out, RDPGFX_CMD_FRAME_ACKNOWLEDGE, 12U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, queue_depth);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, frame_id);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out, total_decoded);
}

/*
 * Decodes an 8-byte RDPGFX_HEADER and returns the command body slice.
 */
int
rdp_gfx_decode_header(
	const uint8_t *data,
	size_t len,
	uint16_t *cmd_id,
	const uint8_t **body,
	size_t *body_len)
{
	struct rdp_reader r;
	uint16_t flags;
	uint32_t pdu_len;
	int error;

	/* Reads cmdId, flags, and pduLength. */
	rdp_reader_init(&r, data, len);
	error = rdp_read_u16_le(&r, cmd_id);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &flags);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u32_le(&r, &pdu_len);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(flags);
	if (pdu_len < 8U || (size_t)pdu_len > len)
		return RDP_ERR_PROTO;
	*body = r.data + r.pos;
	*body_len = (size_t)(pdu_len - 8U);

	/* Succeeded: RDPEGFX header decoded. */
	return RDP_OK;
}

/*
 * Decodes an RDPGFX_WIRE_TO_SURFACE_1 body carrying AVC420 or AVC444.
 */
int
rdp_gfx_decode_avc_pdu(
	const uint8_t *body,
	size_t body_len,
	uint16_t *codec_id,
	struct rdp_avc_stream *out,
	struct rdp_avc_rect *luma_rects,
	struct rdp_avc_quant *luma_quants,
	struct rdp_avc_rect *chroma_rects,
	struct rdp_avc_quant *chroma_quants,
	uint32_t max_rects)
{
	struct rdp_reader r;
	uint16_t surf_id;
	uint8_t pix_fmt;
	uint32_t bmp_len;
	const uint8_t *bmp;
	uint32_t lc;
	uint32_t s1_len;
	int error;

	/* Clears output stream structure and initializes reader. */
	memset(out, 0, sizeof(*out));
	rdp_reader_init(&r, body, body_len);
	error = rdp_read_u16_le(&r, &surf_id);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, codec_id);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u8(&r, &pix_fmt);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(surf_id);
	UNUSED_PARAMETER(pix_fmt);

	/* Skips the 8-byte destRect and reads bitmapDataLength. */
	error = rdp_skip_bytes(&r, 8U);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u32_le(&r, &bmp_len);
	if (error != RDP_OK)
		return error;
	error = rdp_read_bytes(&r, &bmp, (size_t)bmp_len);
	if (error != RDP_OK)
		return error;

	/* Decodes AVC420 directly or splits AVC444 into luma and chroma layers. */
	if (*codec_id == RDP_GFX_CODEC_AVC420) {
		return decode_avc420_layer(bmp, bmp_len, &out->luma,
					   luma_rects, luma_quants, max_rects);
	}
	if (*codec_id != RDP_GFX_CODEC_AVC444)
		return RDP_ERR_UNSUPPORTED;
	rdp_reader_init(&r, bmp, bmp_len);
	error = rdp_read_u32_le(&r, &lc);
	if (error != RDP_OK)
		return error;
	out->avc444_mode = (uint8_t)(lc & 0x03U);
	if (out->avc444_mode == RDP_AVC444_MODE_LUMA_CHROMA) {
		s1_len = lc >> 2;
		if ((size_t)s1_len > rdp_reader_rem(&r))
			return RDP_ERR_PROTO;
		error = decode_avc420_layer(r.data + r.pos, s1_len,
					    &out->luma, luma_rects,
					    luma_quants, max_rects);
		if (error != RDP_OK)
			return error;
		error = rdp_skip_bytes(&r, s1_len);
		if (error != RDP_OK)
			return error;
		return decode_avc420_layer(r.data + r.pos, rdp_reader_rem(&r),
					   &out->chroma, chroma_rects,
					   chroma_quants, max_rects);
	} else if (out->avc444_mode == RDP_AVC444_MODE_LUMA_ONLY) {
		return decode_avc420_layer(r.data + r.pos, rdp_reader_rem(&r),
					   &out->luma, luma_rects,
					   luma_quants, max_rects);
	} else if (out->avc444_mode == RDP_AVC444_MODE_CHROMA_ONLY) {
		return decode_avc420_layer(r.data + r.pos, rdp_reader_rem(&r),
					   &out->chroma, chroma_rects,
					   chroma_quants, max_rects);
	}
	return RDP_ERR_PROTO;
}

/*
 * Decodes an RDPGFX_FRAME_ACKNOWLEDGE_PDU body.
 */
int
rdp_gfx_decode_frame_ack(
	const uint8_t *body,
	size_t body_len,
	uint32_t *queue_depth,
	uint32_t *frame_id,
	uint32_t *total_decoded)
{
	struct rdp_reader r;
	int error;
	rdp_reader_init(&r, body, body_len);
	error = rdp_read_u32_le(&r, queue_depth);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u32_le(&r, frame_id);
	if (error != RDP_OK)
		return error;
	return rdp_read_u32_le(&r, total_decoded);
}

/* Writes an 8-byte RDPGFX_HEADER. */
static int
encode_gfx_hdr(
	struct rdp_buf *out,
	uint16_t cmd_id,
	size_t body_len)
{
	int error;
	error = rdp_buf_append_u16_le(out, cmd_id);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out, (uint32_t)(8U + body_len));
}

/* Encodes an RDPGFX_AVC420_BITMAP_STREAM layer. */
static int
encode_avc420_layer(
	struct rdp_buf *out,
	uint16_t width,
	uint16_t height,
	const struct rdp_avc_layer *layer)
{
	uint32_t count;
	uint32_t i;
	struct rdp_avc_rect def_rect;
	struct rdp_avc_quant def_quant;
	const struct rdp_avc_rect *rects;
	const struct rdp_avc_quant *quants;
	int error;
	if (layer->data == NULL || layer->data_len == 0U)
		return RDP_ERR_INVAL;
	if (layer->rect_count == 0U || layer->rects == NULL) {
		def_rect.left = 0U;
		def_rect.top = 0U;
		def_rect.right = width;
		def_rect.bottom = height;
		def_quant.qp = 22U;
		def_quant.quality = 100U;
		count = 1U;
		rects = &def_rect;
		quants = &def_quant;
	} else {
		count = layer->rect_count;
		rects = layer->rects;
		quants = layer->quants;
	}
	error = rdp_buf_append_u32_le(out, count);
	if (error != RDP_OK)
		return error;
	for (i = 0U; i < count; i++) {
		error = rdp_buf_append_u16_le(out, rects[i].left);
		if (error != RDP_OK)
			return error;
		error = rdp_buf_append_u16_le(out, rects[i].top);
		if (error != RDP_OK)
			return error;
		error = rdp_buf_append_u16_le(out, rects[i].right);
		if (error != RDP_OK)
			return error;
		error = rdp_buf_append_u16_le(out, rects[i].bottom);
		if (error != RDP_OK)
			return error;
	}
	for (i = 0U; i < count; i++) {
		uint8_t qp;
		uint8_t qual;
		qp = quants != NULL ? (uint8_t)(quants[i].qp & 0x3FU) : 22U;
		qual = quants != NULL ? quants[i].quality : 100U;
		error = rdp_buf_append_u8(out, qp);
		if (error != RDP_OK)
			return error;
		error = rdp_buf_append_u8(out, qual);
		if (error != RDP_OK)
			return error;
	}
	return rdp_buf_append(out, layer->data, layer->data_len);
}

/* Decodes an RDPGFX_AVC420_BITMAP_STREAM layer into caller-supplied arrays. */
static int
decode_avc420_layer(
	const uint8_t *data,
	size_t len,
	struct rdp_avc_layer *layer,
	struct rdp_avc_rect *rects,
	struct rdp_avc_quant *quants,
	uint32_t max_rects)
{
	struct rdp_reader r;
	uint32_t count;
	uint32_t i;
	uint8_t qp_raw;
	int error;
	rdp_reader_init(&r, data, len);
	error = rdp_read_u32_le(&r, &count);
	if (error != RDP_OK)
		return error;
	if (count == 0U || count > max_rects)
		return RDP_ERR_PROTO;

	/* Verifies that regionRects and quantQualityVals fit before reading. */
	if ((size_t)count * 10U > rdp_reader_rem(&r))
		return RDP_ERR_PROTO;
	for (i = 0U; i < count; i++) {
		error = rdp_read_u16_le(&r, &rects[i].left);
		if (error != RDP_OK)
			return error;
		error = rdp_read_u16_le(&r, &rects[i].top);
		if (error != RDP_OK)
			return error;
		error = rdp_read_u16_le(&r, &rects[i].right);
		if (error != RDP_OK)
			return error;
		error = rdp_read_u16_le(&r, &rects[i].bottom);
		if (error != RDP_OK)
			return error;
	}
	for (i = 0U; i < count; i++) {
		error = rdp_read_u8(&r, &qp_raw);
		if (error != RDP_OK)
			return error;
		error = rdp_read_u8(&r, &quants[i].quality);
		if (error != RDP_OK)
			return error;
		quants[i].qp = (uint8_t)(qp_raw & 0x3FU);
	}
	layer->rect_count = count;
	layer->rects = rects;
	layer->quants = quants;
	layer->data = r.data + r.pos;
	layer->data_len = rdp_reader_rem(&r);
	return RDP_OK;
}
