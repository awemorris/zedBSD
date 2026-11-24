/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Byte buffer utilities, TPKT/Fast-Path stream framing, and X.224 TPDU
 * encoding and decoding.
 */

#include "rdp-internal.h"

#include <stdlib.h>
#include <string.h>

/* TPKT header version and minimum length. */
#define TPKT_VERSION			3U
#define TPKT_HDR_LEN			4U

/* X.224 TPDU codes and negotiation sub-record types. */
#define X224_TPDU_CR			0xE0U
#define X224_TPDU_CC			0xD0U
#define X224_TPDU_DR			0x80U
#define X224_TPDU_DT			0xF0U
#define X224_NEG_TYPE_REQ		0x01U
#define X224_NEG_TYPE_RSP		0x02U
#define X224_NEG_TYPE_FAILURE		0x03U
#define X224_NEG_LEN			8U

/*
 * Initializes an empty dynamic byte buffer.
 */
void
rdp_buf_init(
	struct rdp_buf *buf)
{
	/* Clears all buffer fields so the first append allocates storage. */
	buf->data = NULL;
	buf->len = 0;
	buf->cap = 0;
}

/*
 * Releases storage held by a dynamic byte buffer.
 */
void
rdp_buf_free(
	struct rdp_buf *buf)
{
	/* Frees any allocated heap block and resets the buffer. */
	free(buf->data);
	buf->data = NULL;
	buf->len = 0;
	buf->cap = 0;
}

/*
 * Resets the used length of a buffer to zero while retaining capacity.
 */
void
rdp_buf_clear(
	struct rdp_buf *buf)
{
	/* Discards valid bytes without releasing the backing allocation. */
	buf->len = 0;
}

/*
 * Ensures that at least extra additional bytes can be appended to buf.
 */
int
rdp_buf_reserve(
	struct rdp_buf *buf,
	size_t extra)
{
	size_t needed;
	size_t new_cap;
	uint8_t *new_data;

	/* Rejects size requests that would wrap size_t or exceed the PDU cap. */
	if (extra > RDP_MAX_PDU_SIZE - buf->len)
		return RDP_ERR_NOMEM;

	/* Returns immediately when existing capacity already covers the request. */
	needed = buf->len + extra;
	if (needed <= buf->cap)
		return RDP_OK;

	/* Grows capacity exponentially starting from 256 bytes. */
	new_cap = buf->cap == 0 ? 256U : buf->cap;
	while (new_cap < needed) {
		if (new_cap > RDP_MAX_PDU_SIZE / 2U) {
			new_cap = RDP_MAX_PDU_SIZE;
			break;
		}

		new_cap *= 2U;
	}

	/* Reallocates the backing array to the new capacity. */
	new_data = (uint8_t *)realloc(buf->data, new_cap);
	if (new_data == NULL)
		return RDP_ERR_NOMEM;

	buf->data = new_data;
	buf->cap = new_cap;

	/* Succeeded: the buffer can now accept extra bytes. */
	return RDP_OK;
}

/*
 * Appends len bytes from data to the end of buf.
 */
int
rdp_buf_append(
	struct rdp_buf *buf,
	const void *data,
	size_t len)
{
	int error;

	/* Skips zero-length appends without touching storage. */
	if (len == 0)
		return RDP_OK;

	/* Reserves space for the incoming slice. */
	error = rdp_buf_reserve(buf, len);
	if (error != RDP_OK)
		return error;

	/* Copies the payload bytes and advances the valid length. */
	memcpy(buf->data + buf->len, data, len);
	buf->len += len;

	/* Succeeded: bytes are appended. */
	return RDP_OK;
}

/*
 * Appends a single unsigned byte to buf.
 */
int
rdp_buf_append_u8(
	struct rdp_buf *buf,
	uint8_t v)
{
	int error;

	/* Appends the single byte via the common buffer path. */
	error = rdp_buf_append(buf, &v, 1U);
	if (error != RDP_OK)
		return error;

	/* Succeeded: byte appended. */
	return RDP_OK;
}

/*
 * Appends a 16-bit little-endian integer to buf.
 */
int
rdp_buf_append_u16_le(
	struct rdp_buf *buf,
	uint16_t v)
{
	uint8_t raw[2];
	int error;

	/* Encodes the 16-bit value in little-endian byte order. */
	raw[0] = (uint8_t)(v & 0xFFU);
	raw[1] = (uint8_t)((v >> 8) & 0xFFU);

	/* Appends the encoded two-byte representation. */
	error = rdp_buf_append(buf, raw, sizeof(raw));
	if (error != RDP_OK)
		return error;

	/* Succeeded: 16-bit value appended. */
	return RDP_OK;
}

/*
 * Appends a 16-bit big-endian integer to buf.
 */
int
rdp_buf_append_u16_be(
	struct rdp_buf *buf,
	uint16_t v)
{
	uint8_t raw[2];
	int error;

	/* Encodes the 16-bit value in big-endian byte order. */
	raw[0] = (uint8_t)((v >> 8) & 0xFFU);
	raw[1] = (uint8_t)(v & 0xFFU);

	/* Appends the encoded two-byte representation. */
	error = rdp_buf_append(buf, raw, sizeof(raw));
	if (error != RDP_OK)
		return error;

	/* Succeeded: 16-bit big-endian value appended. */
	return RDP_OK;
}

/*
 * Appends a 32-bit little-endian integer to buf.
 */
int
rdp_buf_append_u32_le(
	struct rdp_buf *buf,
	uint32_t v)
{
	uint8_t raw[4];
	int error;

	/* Encodes the 32-bit value in little-endian byte order. */
	raw[0] = (uint8_t)(v & 0xFFU);
	raw[1] = (uint8_t)((v >> 8) & 0xFFU);
	raw[2] = (uint8_t)((v >> 16) & 0xFFU);
	raw[3] = (uint8_t)((v >> 24) & 0xFFU);

	/* Appends the encoded four-byte representation. */
	error = rdp_buf_append(buf, raw, sizeof(raw));
	if (error != RDP_OK)
		return error;

	/* Succeeded: 32-bit value appended. */
	return RDP_OK;
}

/*
 * Appends count zero bytes to buf.
 */
int
rdp_buf_append_zeros(
	struct rdp_buf *buf,
	size_t count)
{
	int error;

	/* Skips zero-byte padding requests. */
	if (count == 0)
		return RDP_OK;

	/* Reserves space for the zero run. */
	error = rdp_buf_reserve(buf, count);
	if (error != RDP_OK)
		return error;

	/* Clears the newly reserved tail and advances length. */
	memset(buf->data + buf->len, 0, count);
	buf->len += count;

	/* Succeeded: zero padding appended. */
	return RDP_OK;
}

/*
 * Removes count consumed bytes from the front of buf.
 */
void
rdp_buf_consume(
	struct rdp_buf *buf,
	size_t count)
{
	/* Clears the entire buffer when all bytes are consumed. */
	if (count >= buf->len) {
		buf->len = 0;
		return;
	}

	/* Shifts remaining unread bytes to the start of the buffer. */
	if (count > 0) {
		memmove(buf->data, buf->data + count, buf->len - count);
		buf->len -= count;
	}
}

/*
 * Initializes a read-only cursor over data of length len.
 */
void
rdp_reader_init(
	struct rdp_reader *r,
	const uint8_t *data,
	size_t len)
{
	/* Sets the cursor at byte zero. */
	r->data = data;
	r->len = len;
	r->pos = 0;
}

/*
 * Returns the number of unread bytes remaining in r.
 */
size_t
rdp_reader_rem(
	const struct rdp_reader *r)
{
	/* Reports remaining unread bytes. */
	return r->len - r->pos;
}

/*
 * Reads one unsigned byte from r.
 */
int
rdp_read_u8(
	struct rdp_reader *r,
	uint8_t *out)
{
	/* Rejects reads past the end of the packet slice. */
	if (r->len - r->pos < 1U)
		return RDP_ERR_PROTO;

	/* Extracts the byte and advances the cursor. */
	*out = r->data[r->pos];
	r->pos += 1U;

	/* Succeeded: byte read. */
	return RDP_OK;
}

/*
 * Reads a 16-bit little-endian integer from r.
 */
int
rdp_read_u16_le(
	struct rdp_reader *r,
	uint16_t *out)
{
	uint16_t val;

	/* Rejects reads when fewer than two bytes remain. */
	if (r->len - r->pos < 2U)
		return RDP_ERR_PROTO;

	/* Reconstructs the little-endian integer and advances the cursor. */
	val = (uint16_t)r->data[r->pos] |
	      ((uint16_t)r->data[r->pos + 1U] << 8);
	*out = val;
	r->pos += 2U;

	/* Succeeded: 16-bit value read. */
	return RDP_OK;
}

/*
 * Reads a 16-bit big-endian integer from r.
 */
int
rdp_read_u16_be(
	struct rdp_reader *r,
	uint16_t *out)
{
	uint16_t val;

	/* Rejects reads when fewer than two bytes remain. */
	if (r->len - r->pos < 2U)
		return RDP_ERR_PROTO;

	/* Reconstructs the big-endian integer and advances the cursor. */
	val = ((uint16_t)r->data[r->pos] << 8) |
	      (uint16_t)r->data[r->pos + 1U];
	*out = val;
	r->pos += 2U;

	/* Succeeded: 16-bit big-endian value read. */
	return RDP_OK;
}

/*
 * Reads a 32-bit little-endian integer from r.
 */
int
rdp_read_u32_le(
	struct rdp_reader *r,
	uint32_t *out)
{
	uint32_t val;

	/* Rejects reads when fewer than four bytes remain. */
	if (r->len - r->pos < 4U)
		return RDP_ERR_PROTO;

	/* Reconstructs the little-endian integer and advances the cursor. */
	val = (uint32_t)r->data[r->pos] |
	      ((uint32_t)r->data[r->pos + 1U] << 8) |
	      ((uint32_t)r->data[r->pos + 2U] << 16) |
	      ((uint32_t)r->data[r->pos + 3U] << 24);
	*out = val;
	r->pos += 4U;

	/* Succeeded: 32-bit value read. */
	return RDP_OK;
}

/*
 * Returns a pointer to len bytes in r and advances the cursor.
 */
int
rdp_read_bytes(
	struct rdp_reader *r,
	const uint8_t **out,
	size_t len)
{
	/* Rejects reads that exceed the remaining slice length. */
	if (r->len - r->pos < len)
		return RDP_ERR_PROTO;

	/* Points directly into the slice and advances past it. */
	*out = r->data + r->pos;
	r->pos += len;

	/* Succeeded: slice pointer returned. */
	return RDP_OK;
}

/*
 * Advances r by len bytes without returning data.
 */
int
rdp_skip_bytes(
	struct rdp_reader *r,
	size_t len)
{
	/* Rejects skips that exceed the remaining slice length. */
	if (r->len - r->pos < len)
		return RDP_ERR_PROTO;

	/* Advances the cursor past the skipped region. */
	r->pos += len;

	/* Succeeded: bytes skipped. */
	return RDP_OK;
}

/*
 * Extracts one complete TPKT or Fast-Path packet from rx.
 */
int
rdp_stream_next_packet(
	struct rdp_buf *rx,
	struct rdp_packet *pkt,
	size_t *consumed)
{
	uint8_t b0;
	uint8_t b1;
	size_t total_len;
	size_t hdr_len;
	uint8_t num_events;

	/* Waits for at least one byte to classify TPKT vs Fast-Path. */
	if (rx->len == 0)
		return RDP_ERR_SHORT;

	/* Checks whether the stream starts with TPKT version 3. */
	b0 = rx->data[0];
	if (b0 == TPKT_VERSION) {
		if (rx->len < TPKT_HDR_LEN)
			return RDP_ERR_SHORT;

		/* Rejects malformed TPKT lengths smaller than the header. */
		total_len = ((size_t)rx->data[2] << 8) | (size_t)rx->data[3];
		if (total_len < TPKT_HDR_LEN)
			return RDP_ERR_PROTO;

		/* Waits until the full TPKT frame has arrived. */
		if (rx->len < total_len)
			return RDP_ERR_SHORT;

		pkt->is_fastpath = 0;
		pkt->event_count = 0;
		pkt->payload = rx->data + TPKT_HDR_LEN;
		pkt->payload_len = total_len - TPKT_HDR_LEN;
		*consumed = total_len;

		/* Succeeded: complete TPKT frame extracted. */
		return RDP_OK;
	}

	/* Rejects non-Fast-Path action bits (only action 0 is supported). */
	if ((b0 & 0x03U) != 0x00U)
		return RDP_ERR_PROTO;

	/* Waits for the first Fast-Path length byte. */
	if (rx->len < 2U)
		return RDP_ERR_SHORT;

	/* Decodes the 1-byte or 2-byte PER length of the Fast-Path PDU. */
	b1 = rx->data[1];
	if ((b1 & 0x80U) == 0U) {
		total_len = (size_t)b1;
		hdr_len = 2U;
	} else {
		if (rx->len < 3U)
			return RDP_ERR_SHORT;

		total_len = (((size_t)(b1 & 0x7FU)) << 8) | (size_t)rx->data[2];
		hdr_len = 3U;
	}

	/* Rejects truncated Fast-Path lengths that cannot hold the header. */
	if (total_len < hdr_len)
		return RDP_ERR_PROTO;

	/* Waits until the full Fast-Path packet has arrived. */
	if (rx->len < total_len)
		return RDP_ERR_SHORT;

	/* Extracts numEvents from bits 2..5 or the trailing numEvents byte. */
	num_events = (uint8_t)((b0 >> 2) & 0x0FU);
	if (num_events == 0U) {
		if (total_len < hdr_len + 1U)
			return RDP_ERR_PROTO;

		num_events = rx->data[hdr_len];
		hdr_len += 1U;
	}

	pkt->is_fastpath = 1;
	pkt->event_count = num_events;
	pkt->payload = rx->data + hdr_len;
	pkt->payload_len = total_len - hdr_len;
	*consumed = total_len;

	/* Succeeded: complete Fast-Path packet extracted. */
	return RDP_OK;
}

/*
 * Wraps payload in a 4-byte TPKT header into out.
 */
int
rdp_tpkt_wrap(
	struct rdp_buf *out,
	const uint8_t *payload,
	size_t len)
{
	size_t total;
	int error;

	/* Rejects payloads that exceed the 16-bit TPKT length field. */
	if (len > 65535U - TPKT_HDR_LEN)
		return RDP_ERR_INVAL;

	/* Writes the 4-byte TPKT header. */
	total = TPKT_HDR_LEN + len;
	error = rdp_buf_append_u8(out, (uint8_t)TPKT_VERSION);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(out, 0x00U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, (uint16_t)total);
	if (error != RDP_OK)
		return error;

	/* Appends the TPKT payload bytes. */
	error = rdp_buf_append(out, payload, len);
	if (error != RDP_OK)
		return error;

	/* Succeeded: TPKT frame built. */
	return RDP_OK;
}

/*
 * Encodes an X.224 Connection Request with RDP Negotiation Request.
 */
int
rdp_x224_encode_cr(
	struct rdp_buf *out,
	uint32_t requested_protocols)
{
	struct rdp_buf tpdu;
	int error;

	/* Initializes a temporary buffer for the X.224 TPDU. */
	rdp_buf_init(&tpdu);

	/* Appends the fixed 7-byte CR-TPDU header (LI = 14) and RDP_NEG_REQ. */
	error = rdp_buf_append_u8(&tpdu, 14U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&tpdu, X224_TPDU_CR);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u16_be(&tpdu, 0U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u16_be(&tpdu, 0U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&tpdu, 0U);
	if (error != RDP_OK)
		goto fail;

	/* Appends the 8-byte RDP_NEG_REQ structure. */
	error = rdp_buf_append_u8(&tpdu, X224_NEG_TYPE_REQ);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&tpdu, 0U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u16_le(&tpdu, X224_NEG_LEN);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u32_le(&tpdu, requested_protocols);
	if (error != RDP_OK)
		goto fail;

	/* Wraps the X.224 CR-TPDU in TPKT. */
	error = rdp_tpkt_wrap(out, tpdu.data, tpdu.len);
	if (error != RDP_OK)
		goto fail;

	rdp_buf_free(&tpdu);

	/* Succeeded: X.224 Connection Request encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&tpdu);
	return error;
}

/*
 * Encodes an X.224 Connection Confirm with RDP Negotiation Response.
 */
int
rdp_x224_encode_cc(
	struct rdp_buf *out,
	uint32_t selected_protocol)
{
	struct rdp_buf tpdu;
	int error;

	/* Initializes a temporary buffer for the X.224 CC-TPDU. */
	rdp_buf_init(&tpdu);

	/* Appends the fixed CC-TPDU header (LI = 14) and RDP_NEG_RSP. */
	error = rdp_buf_append_u8(&tpdu, 14U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&tpdu, X224_TPDU_CC);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u16_be(&tpdu, 0U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u16_be(&tpdu, 0x1234U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&tpdu, 0U);
	if (error != RDP_OK)
		goto fail;

	/* Appends the 8-byte RDP_NEG_RSP structure. */
	error = rdp_buf_append_u8(&tpdu, X224_NEG_TYPE_RSP);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&tpdu, 0U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u16_le(&tpdu, X224_NEG_LEN);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u32_le(&tpdu, selected_protocol);
	if (error != RDP_OK)
		goto fail;

	/* Wraps the X.224 CC-TPDU in TPKT. */
	error = rdp_tpkt_wrap(out, tpdu.data, tpdu.len);
	if (error != RDP_OK)
		goto fail;

	rdp_buf_free(&tpdu);

	/* Succeeded: X.224 Connection Confirm encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&tpdu);
	return error;
}

/*
 * Wraps an MCS payload in an X.224 Data TPDU and TPKT frame.
 */
int
rdp_x224_encode_dt(
	struct rdp_buf *out,
	const uint8_t *mcs_data,
	size_t mcs_len)
{
	size_t total;
	int error;

	/* Rejects payloads that exceed TPKT 16-bit framing. */
	if (mcs_len > 65535U - TPKT_HDR_LEN - 3U)
		return RDP_ERR_INVAL;

	/* Writes the TPKT header covering X.224 DT (3 bytes) plus MCS data. */
	total = TPKT_HDR_LEN + 3U + mcs_len;
	error = rdp_buf_append_u8(out, (uint8_t)TPKT_VERSION);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(out, 0x00U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, (uint16_t)total);
	if (error != RDP_OK)
		return error;

	/* Writes the 3-byte X.224 DT header (LI=2, code=0xF0, EOT=0x80). */
	error = rdp_buf_append_u8(out, 0x02U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(out, X224_TPDU_DT);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(out, 0x80U);
	if (error != RDP_OK)
		return error;

	/* Appends the MCS payload bytes. */
	error = rdp_buf_append(out, mcs_data, mcs_len);
	if (error != RDP_OK)
		return error;

	/* Succeeded: X.224 Data TPDU encoded. */
	return RDP_OK;
}

/*
 * Decodes an X.224 TPDU inside a TPKT payload.
 */
int
rdp_x224_decode(
	const uint8_t *tpdu,
	size_t tpdu_len,
	struct rdp_x224_neg *neg,
	const uint8_t **dt_payload,
	size_t *dt_len)
{
	struct rdp_reader r;
	uint8_t li;
	uint8_t code;
	uint16_t dst_ref;
	uint16_t src_ref;
	uint8_t cls;
	uint8_t neg_type;
	uint8_t neg_flags;
	uint16_t neg_len;
	uint32_t protos;
	const uint8_t *var_data;
	size_t var_len;
	size_t i;
	int error;

	/* Initializes the reader and clears output fields. */
	rdp_reader_init(&r, tpdu, tpdu_len);
	memset(neg, 0, sizeof(*neg));
	*dt_payload = NULL;
	*dt_len = 0;

	/* Reads the X.224 length indicator and TPDU code. */
	error = rdp_read_u8(&r, &li);
	if (error != RDP_OK)
		return error;

	error = rdp_read_u8(&r, &code);
	if (error != RDP_OK)
		return error;

	neg->tpdu_code = code;

	/* Handles X.224 Data TPDU by returning the trailing MCS slice. */
	if (code == X224_TPDU_DT) {
		if (li < 2U || rdp_reader_rem(&r) < (size_t)(li - 1U))
			return RDP_ERR_PROTO;

		error = rdp_skip_bytes(&r, (size_t)(li - 1U));
		if (error != RDP_OK)
			return error;

		*dt_payload = r.data + r.pos;
		*dt_len = rdp_reader_rem(&r);

		/* Succeeded: X.224 DT payload located. */
		return RDP_OK;
	}

	/* Rejects non-CR/CC/DR TPDUs. */
	if (code != X224_TPDU_CR && code != X224_TPDU_CC && code != X224_TPDU_DR)
		return RDP_ERR_PROTO;

	/* Reads the fixed 6-byte CR/CC/DR header fields. */
	error = rdp_read_u16_be(&r, &dst_ref);
	if (error != RDP_OK)
		return error;

	error = rdp_read_u16_be(&r, &src_ref);
	if (error != RDP_OK)
		return error;

	error = rdp_read_u8(&r, &cls);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(dst_ref);
	UNUSED_PARAMETER(src_ref);
	UNUSED_PARAMETER(cls);

	/* Scans variable CR/CC bytes past any Cookie line for RDP_NEG_*. */
	var_len = rdp_reader_rem(&r);
	if (var_len == 0)
		return RDP_OK;

	error = rdp_read_bytes(&r, &var_data, var_len);
	if (error != RDP_OK)
		return error;

	/* Skips optional Cookie: ...\r\n prefix present in CR packets. */
	i = 0;
	if (code == X224_TPDU_CR && var_len >= 7U &&
	    memcmp(var_data, "Cookie:", 7U) == 0) {
		while (i + 1U < var_len) {
			if (var_data[i] == '\r' && var_data[i + 1U] == '\n') {
				i += 2U;
				break;
			}

			i++;
		}
	}

	/* Parses the 8-byte RDP_NEG_REQ/RSP/FAILURE tail if present. */
	if (var_len - i >= X224_NEG_LEN) {
		rdp_reader_init(&r, var_data + i, var_len - i);
		error = rdp_read_u8(&r, &neg_type);
		if (error != RDP_OK)
			return error;

		error = rdp_read_u8(&r, &neg_flags);
		if (error != RDP_OK)
			return error;

		error = rdp_read_u16_le(&r, &neg_len);
		if (error != RDP_OK)
			return error;

		error = rdp_read_u32_le(&r, &protos);
		if (error != RDP_OK)
			return error;

		if (neg_len != X224_NEG_LEN)
			return RDP_ERR_PROTO;

		neg->neg_type = neg_type;
		neg->neg_flags = neg_flags;
		neg->protocols = protos;
	}

	/* Succeeded: X.224 negotiation packet decoded. */
	return RDP_OK;
}
