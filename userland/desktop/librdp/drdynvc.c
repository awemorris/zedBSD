/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Static virtual channel chunking/reassembly and MS-RDPEDYC (DRDYNVC)
 * dynamic virtual channel protocol codec.
 */

#include "rdp-internal.h"

#include <string.h>

/* DRDYNVC command codes (upper nibble of header byte). */
#define DYNVC_CMD_CREATE		0x01U
#define DYNVC_CMD_DATA_FIRST		0x02U
#define DYNVC_CMD_DATA			0x03U
#define DYNVC_CMD_CLOSE			0x04U
#define DYNVC_CMD_CAPS			0x05U

static int append_var_uint(struct rdp_buf *buf, uint32_t val, uint8_t *out_cbid);
static int read_var_uint(struct rdp_reader *r, uint8_t cbid, uint32_t *out);

/*
 * Feeds a static virtual channel PDU chunk and emits a complete message.
 */
int
rdp_vc_reasm_feed(
	struct rdp_vc_reasm *reasm,
	const uint8_t *data,
	size_t len,
	const uint8_t **out_msg,
	size_t *out_len)
{
	struct rdp_reader r;
	uint32_t total_len;
	uint32_t flags;
	const uint8_t *chunk;
	size_t chunk_len;
	int error;

	/* Clears outputs until a complete static virtual channel PDU arrives. */
	*out_msg = NULL;
	*out_len = 0U;

	/* Reads the 8-byte CHANNEL_PDU_HEADER. */
	rdp_reader_init(&r, data, len);
	error = rdp_read_u32_le(&r, &total_len);
	if (error != RDP_OK)
		return error;

	error = rdp_read_u32_le(&r, &flags);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(total_len);
	chunk_len = rdp_reader_rem(&r);
	error = rdp_read_bytes(&r, &chunk, chunk_len);
	if (error != RDP_OK)
		return error;

	/* Resets the reassembly buffer on CHANNEL_FLAG_FIRST. */
	if ((flags & RDP_CHANNEL_FLAG_FIRST) != 0U) {
		rdp_buf_clear(&reasm->buf);
		reasm->active = 1;
	} else if (reasm->active == 0) {
		return RDP_ERR_PROTO;
	}

	/* Appends the chunk payload and checks the bounded PDU limit. */
	error = rdp_buf_append(&reasm->buf, chunk, chunk_len);
	if (error != RDP_OK)
		return error;

	/* Delivers the assembled message when CHANNEL_FLAG_LAST is set. */
	if ((flags & RDP_CHANNEL_FLAG_LAST) != 0U) {
		reasm->active = 0;
		*out_msg = reasm->buf.data;
		*out_len = reasm->buf.len;
	}

	/* Succeeded: static virtual channel chunk processed. */
	return RDP_OK;
}

/*
 * Resets static virtual channel reassembly state.
 */
void
rdp_vc_reasm_reset(
	struct rdp_vc_reasm *reasm)
{
	/* Releases the reassembly buffer and marks the stream inactive. */
	rdp_buf_free(&reasm->buf);
	reasm->active = 0;
}

/*
 * Splits msg into static virtual channel chunks and sends each via send_mcs.
 */
int
rdp_vc_send_chunked(
	int (*send_mcs)(void *ctx, const uint8_t *chunk, size_t chunk_len),
	void *ctx,
	const uint8_t *msg,
	size_t msg_len)
{
	struct rdp_buf pkt;
	size_t offset;
	size_t piece;
	uint32_t flags;
	int error;

	/* Initializes a temporary buffer for one static VC chunk at a time. */
	rdp_buf_init(&pkt);
	offset = 0U;

	/* Emits at least one chunk even if msg_len is zero. */
	do {
		piece = msg_len - offset;
		if (piece > RDP_VC_CHUNK_SIZE)
			piece = RDP_VC_CHUNK_SIZE;

		flags = 0U;
		if (offset == 0U)
			flags |= RDP_CHANNEL_FLAG_FIRST;
		if (offset + piece == msg_len)
			flags |= RDP_CHANNEL_FLAG_LAST;

		rdp_buf_clear(&pkt);
		error = rdp_buf_append_u32_le(&pkt, (uint32_t)msg_len);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append_u32_le(&pkt, flags);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append(&pkt, msg + offset, piece);
		if (error != RDP_OK)
			goto fail;

		error = send_mcs(ctx, pkt.data, pkt.len);
		if (error != RDP_OK)
			goto fail;

		offset += piece;
	} while (offset < msg_len);

	rdp_buf_free(&pkt);

	/* Succeeded: static virtual channel message sent. */
	return RDP_OK;

fail:
	rdp_buf_free(&pkt);
	return error;
}

/*
 * Encodes a DYNVC_CAPS_VERSION PDU.
 */
int
rdp_dynvc_encode_caps(
	struct rdp_buf *out,
	uint16_t version)
{
	int error;

	/* Emits Cmd=5 (DYNVC_CAPS), pad, version, and priority charges. */
	error = rdp_buf_append_u8(out, (uint8_t)(DYNVC_CMD_CAPS << 4));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(out, 0U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(out, version);
	if (error != RDP_OK)
		return error;

	return rdp_buf_append_zeros(out, 8U);
}

/*
 * Encodes a DYNVC_CREATE_REQ PDU opening a named dynamic channel.
 */
int
rdp_dynvc_encode_create_req(
	struct rdp_buf *out,
	uint32_t channel_id,
	const char *name)
{
	struct rdp_buf tmp;
	uint8_t cbid;
	size_t nlen;
	int error;

	/* Encodes channel_id first to learn its cbId width. */
	rdp_buf_init(&tmp);
	error = append_var_uint(&tmp, channel_id, &cbid);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(out, (uint8_t)((DYNVC_CMD_CREATE << 4) | cbid));
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append(out, tmp.data, tmp.len);
	if (error != RDP_OK)
		goto fail;

	nlen = strlen(name) + 1U;
	error = rdp_buf_append(out, name, nlen);
	if (error != RDP_OK)
		goto fail;

	rdp_buf_free(&tmp);

	/* Succeeded: DYNVC_CREATE_REQ encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&tmp);
	return error;
}

/*
 * Encodes a DYNVC_CREATE_RSP PDU acknowledging a dynamic channel.
 */
int
rdp_dynvc_encode_create_rsp(
	struct rdp_buf *out,
	uint32_t channel_id,
	uint32_t status)
{
	struct rdp_buf tmp;
	uint8_t cbid;
	int error;

	/* Encodes channel_id first to learn its cbId width. */
	rdp_buf_init(&tmp);
	error = append_var_uint(&tmp, channel_id, &cbid);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(out, (uint8_t)((DYNVC_CMD_CREATE << 4) | cbid));
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append(out, tmp.data, tmp.len);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u32_le(out, status);
	if (error != RDP_OK)
		goto fail;

	rdp_buf_free(&tmp);

	/* Succeeded: DYNVC_CREATE_RSP encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&tmp);
	return error;
}

/*
 * Encodes a DYNVC_DATA PDU carrying payload on channel_id.
 */
int
rdp_dynvc_encode_data(
	struct rdp_buf *out,
	uint32_t channel_id,
	const uint8_t *payload,
	size_t len)
{
	struct rdp_buf tmp;
	uint8_t cbid;
	int error;

	/* Encodes channel_id first to learn its cbId width. */
	rdp_buf_init(&tmp);
	error = append_var_uint(&tmp, channel_id, &cbid);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(out, (uint8_t)((DYNVC_CMD_DATA << 4) | cbid));
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append(out, tmp.data, tmp.len);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append(out, payload, len);
	if (error != RDP_OK)
		goto fail;

	rdp_buf_free(&tmp);

	/* Succeeded: DYNVC_DATA encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&tmp);
	return error;
}

/*
 * Encodes a DYNVC_CLOSE_REQ PDU closing channel_id.
 */
int
rdp_dynvc_encode_close_req(
	struct rdp_buf *out,
	uint32_t channel_id)
{
	struct rdp_buf tmp;
	uint8_t cbid;
	int error;

	/* Encodes channel_id first to learn its cbId width. */
	rdp_buf_init(&tmp);
	error = append_var_uint(&tmp, channel_id, &cbid);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(out, (uint8_t)((DYNVC_CMD_CLOSE << 4) | cbid));
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append(out, tmp.data, tmp.len);
	if (error != RDP_OK)
		goto fail;

	rdp_buf_free(&tmp);

	/* Succeeded: DYNVC_CLOSE_REQ encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&tmp);
	return error;
}

/*
 * Decodes a DRDYNVC PDU into msg.
 */
int
rdp_dynvc_decode(
	const uint8_t *data,
	size_t len,
	struct rdp_dynvc_msg *msg)
{
	struct rdp_reader r;
	uint8_t hdr;
	uint8_t cbid;
	uint8_t sp;
	uint8_t pad;
	size_t rem;
	size_t copy_len;
	int error;

	/* Clears output and reads the 1-byte DRDYNVC header. */
	memset(msg, 0, sizeof(*msg));
	rdp_reader_init(&r, data, len);

	error = rdp_read_u8(&r, &hdr);
	if (error != RDP_OK)
		return error;

	msg->cmd = (uint8_t)((hdr >> 4) & 0x0FU);
	cbid = (uint8_t)(hdr & 0x03U);
	sp = (uint8_t)((hdr >> 2) & 0x03U);

	/* Parses command-specific fields. */
	if (msg->cmd == DYNVC_CMD_CAPS) {
		error = rdp_read_u8(&r, &pad);
		if (error != RDP_OK)
			return error;

		UNUSED_PARAMETER(pad);
		return rdp_read_u16_le(&r, &msg->version);
	}

	error = read_var_uint(&r, cbid, &msg->channel_id);
	if (error != RDP_OK)
		return error;

	if (msg->cmd == DYNVC_CMD_CREATE) {
		rem = rdp_reader_rem(&r);
		if (rem == 4U) {
			return rdp_read_u32_le(&r, &msg->creation_status);
		}

		copy_len = rem;
		if (copy_len >= sizeof(msg->channel_name))
			copy_len = sizeof(msg->channel_name) - 1U;

		memcpy(msg->channel_name, r.data + r.pos, copy_len);
		msg->channel_name[copy_len] = '\0';
		return RDP_OK;
	} else if (msg->cmd == DYNVC_CMD_DATA_FIRST) {
		error = read_var_uint(&r, sp, &msg->total_len);
		if (error != RDP_OK)
			return error;

		msg->data = r.data + r.pos;
		msg->data_len = rdp_reader_rem(&r);
		return RDP_OK;
	} else if (msg->cmd == DYNVC_CMD_DATA) {
		msg->data = r.data + r.pos;
		msg->data_len = rdp_reader_rem(&r);
		return RDP_OK;
	} else if (msg->cmd == DYNVC_CMD_CLOSE) {
		return RDP_OK;
	}

	return RDP_ERR_PROTO;
}

/*
 * Feeds a decoded DRDYNVC data message into a per-channel reassembly stream.
 */
int
rdp_dynvc_stream_feed(
	struct rdp_dynvc_stream *stream,
	const struct rdp_dynvc_msg *msg,
	const uint8_t **out_data,
	size_t *out_len)
{
	int error;

	/* Clears outputs until a complete dynamic channel message is ready. */
	*out_data = NULL;
	*out_len = 0U;

	/* Handles unfragmented single DYNVC_DATA messages directly. */
	if (msg->cmd == DYNVC_CMD_DATA && stream->active == 0) {
		*out_data = msg->data;
		*out_len = msg->data_len;
		return RDP_OK;
	}

	/* Starts a fragmented message on DYNVC_DATA_FIRST. */
	if (msg->cmd == DYNVC_CMD_DATA_FIRST) {
		if (msg->total_len > RDP_MAX_PDU_SIZE)
			return RDP_ERR_NOMEM;

		rdp_buf_clear(&stream->buf);
		stream->expected_len = msg->total_len;
		stream->active = 1;
	}

	/* Appends fragment bytes and checks completion against expected_len. */
	error = rdp_buf_append(&stream->buf, msg->data, msg->data_len);
	if (error != RDP_OK)
		return error;

	if (stream->buf.len >= stream->expected_len) {
		stream->active = 0;
		*out_data = stream->buf.data;
		*out_len = stream->expected_len;
	}

	/* Succeeded: DRDYNVC fragment processed. */
	return RDP_OK;
}

/*
 * Releases reassembly storage for a dynamic virtual channel stream.
 */
void
rdp_dynvc_stream_reset(
	struct rdp_dynvc_stream *stream)
{
	/* Frees the reassembly buffer and clears state. */
	rdp_buf_free(&stream->buf);
	stream->expected_len = 0U;
	stream->active = 0;
}

/* Encodes an unsigned 32-bit integer in 1, 2, or 4 bytes and reports cbId. */
static int
append_var_uint(
	struct rdp_buf *buf,
	uint32_t val,
	uint8_t *out_cbid)
{
	if (val <= 0xFFU) {
		*out_cbid = 0U;
		return rdp_buf_append_u8(buf, (uint8_t)val);
	}

	if (val <= 0xFFFFU) {
		*out_cbid = 1U;
		return rdp_buf_append_u16_le(buf, (uint16_t)val);
	}

	*out_cbid = 2U;
	return rdp_buf_append_u32_le(buf, val);
}

/* Reads a 1-, 2-, or 4-byte variable-width integer according to cbid. */
static int
read_var_uint(
	struct rdp_reader *r,
	uint8_t cbid,
	uint32_t *out)
{
	uint8_t v8;
	uint16_t v16;
	int error;

	if (cbid == 0U) {
		error = rdp_read_u8(r, &v8);
		if (error != RDP_OK)
			return error;

		*out = v8;
		return RDP_OK;
	}

	if (cbid == 1U) {
		error = rdp_read_u16_le(r, &v16);
		if (error != RDP_OK)
			return error;

		*out = v16;
		return RDP_OK;
	}

	if (cbid == 2U)
		return rdp_read_u32_le(r, out);

	return RDP_ERR_PROTO;
}
