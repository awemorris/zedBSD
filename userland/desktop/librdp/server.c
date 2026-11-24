/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * I/O-independent RDP server protocol state machine.
 */

#include "rdp-internal.h"

#include <stdlib.h>
#include <string.h>

/* Share ID assigned by the server during DemandActive. */
#define RDP_SERVER_SHARE_ID		0x000103EAU

/* Dynamic channel IDs assigned by the server. */
#define RDP_DYNVC_ID_GFX		1U
#define RDP_DYNVC_ID_DISP		2U

/*
 * Per-connection RDP server state machine instance.
 */
struct rdp_server {
	struct rdp_callbacks cb;
	void *user;
	enum rdp_server_state state;
	uint32_t width;
	uint32_t height;
	struct rdp_buf rx;
	struct rdp_vc_reasm vc_reasm;
	struct rdp_dynvc_stream gfx_stream;
	struct rdp_dynvc_stream disp_stream;
	uint16_t user_id;
	int has_drdynvc;
	int io_joined;
	int user_joined;
	int dynvc_joined;
	int gfx_open;
	int gfx_ready;
	int disp_open;
	uint32_t next_frame_id;
	uint32_t unacked_frames;
};

static int send_raw(struct rdp_server *s, const struct rdp_buf *buf);
static int send_mcs_dt(struct rdp_server *s, const struct rdp_buf *mcs);
static int send_io_data(struct rdp_server *s, const struct rdp_buf *sec_pdu);
static int send_dynvc_chunk_cb(void *ctx, const uint8_t *chunk, size_t len);
static int send_dynvc_msg(struct rdp_server *s, uint32_t chan_id, const struct rdp_buf *payload);
static int handle_tpkt_packet(struct rdp_server *s, const uint8_t *tpdu, size_t len);
static int handle_mcs_pdu(struct rdp_server *s, const uint8_t *data, size_t len);
static int handle_io_pdu(struct rdp_server *s, const uint8_t *data, size_t len);
static int handle_dynvc_pdu(struct rdp_server *s, const uint8_t *data, size_t len);
static int handle_gfx_pdu(struct rdp_server *s, const uint8_t *data, size_t len);

/*
 * Allocates and initializes a new RDP server state machine.
 */
struct rdp_server *
rdp_server_create(
	const struct rdp_callbacks *cb,
	void *user,
	const struct rdp_server_config *cfg)
{
	struct rdp_server *s;

	/* Rejects callers without a valid send_bytes transport callback. */
	if (cb == NULL || cb->send_bytes == NULL)
		return NULL;

	/* Allocates and zeroes the server state machine. */
	s = (struct rdp_server *)calloc(1U, sizeof(*s));
	if (s == NULL)
		return NULL;
	s->cb = *cb;
	s->user = user;
	s->state = RDP_SRV_WAIT_X224_CR;
	s->width = (cfg != NULL && cfg->width > 0U) ? cfg->width : 1280U;
	s->height = (cfg != NULL && cfg->height > 0U) ? cfg->height : 720U;
	s->user_id = RDP_MCS_USER_BASE + 1U;
	s->next_frame_id = 1U;
	rdp_buf_init(&s->rx);
	rdp_buf_init(&s->vc_reasm.buf);
	rdp_buf_init(&s->gfx_stream.buf);
	rdp_buf_init(&s->disp_stream.buf);

	/* Succeeded: server waiting for X.224 Connection Request. */
	return s;
}

/*
 * Destroys an RDP server state machine and frees all buffers.
 */
void
rdp_server_destroy(
	struct rdp_server *server)
{
	/* Allows NULL pointers safely. */
	if (server == NULL)
		return;
	rdp_buf_free(&server->rx);
	rdp_vc_reasm_reset(&server->vc_reasm);
	rdp_dynvc_stream_reset(&server->gfx_stream);
	rdp_dynvc_stream_reset(&server->disp_stream);
	free(server);
}

/*
 * Notifies the server state machine that the TLS handshake completed.
 */
int
rdp_server_tls_established(
	struct rdp_server *server)
{
	if (server == NULL)
		return RDP_ERR_INVAL;
	if (server->state != RDP_SRV_WAIT_TLS)
		return RDP_ERR_STATE;
	server->state = RDP_SRV_WAIT_MCS_CONNECT_INIT;

	/* Succeeded: server ready for MCS Connect-Initial. */
	return RDP_OK;
}

/*
 * Feeds incoming bytes from the transport into the server state machine.
 */
int
rdp_server_feed(
	struct rdp_server *server,
	const uint8_t *data,
	size_t len)
{
	struct rdp_packet pkt;
	size_t consumed;
	int error;

	/* Validates inputs. */
	if (server == NULL || (data == NULL && len > 0U))
		return RDP_ERR_INVAL;

	/* Appends incoming bytes to the stream reassembly buffer. */
	error = rdp_buf_append(&server->rx, data, len);
	if (error != RDP_OK)
		return error;

	/* Processes every complete TPKT or Fast-Path packet in rx. */
	for (;;) {
		consumed = 0U;
		error = rdp_stream_next_packet(&server->rx, &pkt, &consumed);
		if (error == RDP_ERR_SHORT)
			break;
		if (error != RDP_OK)
			return error;
		if (pkt.is_fastpath != 0) {
			error = rdp_input_decode_events(
				pkt.payload, pkt.payload_len,
				pkt.event_count, &server->cb, server->user);
			rdp_buf_consume(&server->rx, consumed);
			if (error != RDP_OK)
				return error;

			continue;
		}
		error = handle_tpkt_packet(server, pkt.payload, pkt.payload_len);
		rdp_buf_consume(&server->rx, consumed);
		if (error != RDP_OK)
			return error;
	}

	/* Succeeded: all complete packets processed. */
	return RDP_OK;
}

/*
 * Returns nonzero when the server is waiting for TLS handshake completion.
 */
int
rdp_server_needs_tls(
	const struct rdp_server *server)
{
	if (server == NULL)
		return 0;
	if (server->state == RDP_SRV_WAIT_TLS)
		return 1;
	return 0;
}

/*
 * Returns nonzero when the server session is active and connected.
 */
int
rdp_server_is_connected(
	const struct rdp_server *server)
{
	if (server == NULL)
		return 0;
	if (server->state == RDP_SRV_CONNECTED)
		return 1;
	return 0;
}

/*
 * Approves client authentication and starts capability exchange.
 */
int
rdp_server_accept_auth(
	struct rdp_server *server)
{
	struct rdp_buf out;
	int error;
	if (server == NULL)
		return RDP_ERR_INVAL;
	if (server->state != RDP_SRV_WAIT_AUTH_DECISION)
		return RDP_ERR_STATE;

	/* Sends LICENSE_ERROR_ALERT (STATUS_VALID_CLIENT). */
	rdp_buf_init(&out);
	error = rdp_sec_encode_license_valid(&out);
	if (error == RDP_OK)
		error = send_io_data(server, &out);

	/* Sends DemandActivePDU with server capabilities. */
	rdp_buf_clear(&out);
	if (error == RDP_OK) {
		error = rdp_sec_encode_demand_active(
			&out, RDP_SERVER_SHARE_ID, server->width,
			server->height, RDP_MCS_IO_CHANNEL);
	}
	if (error == RDP_OK) {
		server->state = RDP_SRV_WAIT_CONFIRM_ACTIVE;
		error = send_io_data(server, &out);
	}
	rdp_buf_free(&out);
	if (error != RDP_OK)
		return error;

	/* Succeeded: DemandActivePDU sent to client. */
	return RDP_OK;
}

/*
 * Rejects client authentication with error_info and disconnects.
 */
int
rdp_server_reject_auth(
	struct rdp_server *server,
	uint32_t error_info)
{
	struct rdp_buf out;
	int error;
	if (server == NULL)
		return RDP_ERR_INVAL;
	if (server->state != RDP_SRV_WAIT_AUTH_DECISION)
		return RDP_ERR_STATE;
	rdp_buf_init(&out);
	error = rdp_sec_encode_share_data_error_info(&out, RDP_SERVER_SHARE_ID,
						     RDP_MCS_IO_CHANNEL,
						     error_info);
	if (error == RDP_OK)
		send_io_data(server, &out);
	rdp_buf_free(&out);
	server->state = RDP_SRV_DISCONNECTED;
	if (server->cb.on_disconnect != NULL)
		server->cb.on_disconnect(server->user, error_info);

	/* Succeeded: client rejected and disconnected. */
	return RDP_OK;
}

/*
 * Disconnects an active server session with an error_info reason code.
 */
int
rdp_server_disconnect(
	struct rdp_server *server,
	uint32_t error_info)
{
	struct rdp_buf out;
	int error;
	if (server == NULL)
		return RDP_ERR_INVAL;
	if (server->state == RDP_SRV_DISCONNECTED)
		return RDP_OK;
	if (server->state >= RDP_SRV_WAIT_CONFIRM_ACTIVE) {
		rdp_buf_init(&out);
		error = rdp_sec_encode_share_data_error_info(
			&out, RDP_SERVER_SHARE_ID, RDP_MCS_IO_CHANNEL,
			error_info);
		if (error == RDP_OK)
			send_io_data(server, &out);
		rdp_buf_free(&out);
	}
	server->state = RDP_SRV_DISCONNECTED;
	if (server->cb.on_disconnect != NULL)
		server->cb.on_disconnect(server->user, error_info);

	/* Succeeded: server disconnected. */
	return RDP_OK;
}

/*
 * Sends an H.264 AVC420 or AVC444 frame over MS-RDPEGFX.
 */
int
rdp_server_send_avc_frame(
	struct rdp_server *server,
	uint16_t codec_id,
	const struct rdp_avc_stream *avc,
	uint32_t timestamp_ms,
	uint32_t *out_frame_id)
{
	struct rdp_buf gfx;
	uint32_t fid;
	int error;
	if (server == NULL || avc == NULL)
		return RDP_ERR_INVAL;
	if (server->state != RDP_SRV_CONNECTED || server->gfx_ready == 0)
		return RDP_ERR_STATE;
	fid = server->next_frame_id++;
	rdp_buf_init(&gfx);

	/* Sends START_FRAME, WIRE_TO_SURFACE_1, and END_FRAME. */
	error = rdp_gfx_encode_start_frame(&gfx, timestamp_ms, fid);
	if (error == RDP_OK)
		error = send_dynvc_msg(server, RDP_DYNVC_ID_GFX, &gfx);
	rdp_buf_clear(&gfx);
	if (error == RDP_OK) {
		error = rdp_gfx_encode_wire_to_surface_avc(
			&gfx, 0U, codec_id, (uint16_t)server->width,
			(uint16_t)server->height, avc);
	}
	if (error == RDP_OK)
		error = send_dynvc_msg(server, RDP_DYNVC_ID_GFX, &gfx);
	rdp_buf_clear(&gfx);
	if (error == RDP_OK)
		error = rdp_gfx_encode_end_frame(&gfx, fid);
	if (error == RDP_OK)
		error = send_dynvc_msg(server, RDP_DYNVC_ID_GFX, &gfx);
	rdp_buf_free(&gfx);
	if (error != RDP_OK)
		return error;
	server->unacked_frames++;
	if (out_frame_id != NULL)
		*out_frame_id = fid;

	/* Succeeded: H.264 frame transmitted to client. */
	return RDP_OK;
}

/*
 * Updates the server graphics surface dimensions via MS-RDPEGFX.
 */
int
rdp_server_resize(
	struct rdp_server *server,
	uint32_t width,
	uint32_t height)
{
	struct rdp_buf gfx;
	int error;
	if (server == NULL || width == 0U || height == 0U)
		return RDP_ERR_INVAL;
	server->width = width;
	server->height = height;
	if (server->state != RDP_SRV_CONNECTED || server->gfx_ready == 0)
		return RDP_OK;
	rdp_buf_init(&gfx);
	error = rdp_gfx_encode_reset_graphics(&gfx, width, height);
	if (error == RDP_OK)
		error = send_dynvc_msg(server, RDP_DYNVC_ID_GFX, &gfx);
	rdp_buf_clear(&gfx);
	if (error == RDP_OK)
		error = rdp_gfx_encode_delete_surface(&gfx, 0U);
	if (error == RDP_OK)
		error = send_dynvc_msg(server, RDP_DYNVC_ID_GFX, &gfx);
	rdp_buf_clear(&gfx);
	if (error == RDP_OK) {
		error = rdp_gfx_encode_create_surface(&gfx, 0U,
						      (uint16_t)width,
						      (uint16_t)height);
	}
	if (error == RDP_OK)
		error = send_dynvc_msg(server, RDP_DYNVC_ID_GFX, &gfx);
	rdp_buf_clear(&gfx);
	if (error == RDP_OK)
		error = rdp_gfx_encode_map_surface(&gfx, 0U);
	if (error == RDP_OK)
		error = send_dynvc_msg(server, RDP_DYNVC_ID_GFX, &gfx);
	rdp_buf_free(&gfx);
	if (error != RDP_OK)
		return error;

	/* Succeeded: RDPEGFX output surface resized. */
	return RDP_OK;
}

/*
 * Returns the number of graphics frames sent but not yet acknowledged.
 */
uint32_t
rdp_server_unacked_frames(
	const struct rdp_server *server)
{
	if (server == NULL)
		return 0U;
	return server->unacked_frames;
}

/* Sends raw bytes through the caller's transport callback. */
static int
send_raw(
	struct rdp_server *s,
	const struct rdp_buf *buf)
{
	int rc;
	rc = s->cb.send_bytes(s->user, buf->data, buf->len);
	if (rc < 0)
		return RDP_ERR_IO;
	return RDP_OK;
}

/* Wraps an MCS PDU in X.224 DT + TPKT and sends it. */
static int
send_mcs_dt(
	struct rdp_server *s,
	const struct rdp_buf *mcs)
{
	struct rdp_buf out;
	int error;
	rdp_buf_init(&out);
	error = rdp_x224_encode_dt(&out, mcs->data, mcs->len);
	if (error != RDP_OK) {
		rdp_buf_free(&out);
		return error;
	}
	error = send_raw(s, &out);
	rdp_buf_free(&out);
	return error;
}

/* Wraps a Share/Security PDU in MCS SendDataIndication on io_channel. */
static int
send_io_data(
	struct rdp_server *s,
	const struct rdp_buf *sec_pdu)
{
	struct rdp_buf mcs;
	int error;
	rdp_buf_init(&mcs);
	error = rdp_mcs_encode_send_data_ind(&mcs, RDP_MCS_IO_CHANNEL,
					     RDP_MCS_IO_CHANNEL,
					     sec_pdu->data, sec_pdu->len);
	if (error != RDP_OK) {
		rdp_buf_free(&mcs);
		return error;
	}
	error = send_mcs_dt(s, &mcs);
	rdp_buf_free(&mcs);
	return error;
}

/* Chunk callback sending one static virtual channel fragment on drdynvc. */
static int
send_dynvc_chunk_cb(
	void *ctx,
	const uint8_t *chunk,
	size_t len)
{
	struct rdp_server *s;
	struct rdp_buf mcs;
	int error;
	s = (struct rdp_server *)ctx;
	rdp_buf_init(&mcs);
	error = rdp_mcs_encode_send_data_ind(&mcs, RDP_MCS_IO_CHANNEL,
					     RDP_MCS_DRDYNVC_CHANNEL, chunk,
					     len);
	if (error != RDP_OK) {
		rdp_buf_free(&mcs);
		return error;
	}
	error = send_mcs_dt(s, &mcs);
	rdp_buf_free(&mcs);
	return error;
}

/* Wraps payload in DYNVC_DATA and sends it chunked over drdynvc. */
static int
send_dynvc_msg(
	struct rdp_server *s,
	uint32_t chan_id,
	const struct rdp_buf *payload)
{
	struct rdp_buf dyn;
	int error;
	rdp_buf_init(&dyn);
	error = rdp_dynvc_encode_data(&dyn, chan_id, payload->data,
				      payload->len);
	if (error != RDP_OK) {
		rdp_buf_free(&dyn);
		return error;
	}
	error = rdp_vc_send_chunked(send_dynvc_chunk_cb, s, dyn.data, dyn.len);
	rdp_buf_free(&dyn);
	return error;
}

/* Dispatches a decoded TPKT payload to X.224 or MCS handlers. */
static int
handle_tpkt_packet(
	struct rdp_server *s,
	const uint8_t *tpdu,
	size_t len)
{
	struct rdp_x224_neg neg;
	const uint8_t *dt_pay;
	size_t dt_len;
	struct rdp_buf out;
	int error;
	error = rdp_x224_decode(tpdu, len, &neg, &dt_pay, &dt_len);
	if (error != RDP_OK)
		return error;
	if (s->state == RDP_SRV_WAIT_X224_CR) {
		if (neg.tpdu_code != 0xE0U ||
		    (neg.protocols & RDP_NEG_PROTO_SSL) == 0U)
			return RDP_ERR_UNSUPPORTED;
		rdp_buf_init(&out);
		error = rdp_x224_encode_cc(&out, RDP_NEG_PROTO_SSL);
		if (error == RDP_OK) {
			s->state = RDP_SRV_WAIT_TLS;
			error = send_raw(s, &out);
		}
		rdp_buf_free(&out);
		if (error == RDP_OK && s->cb.on_tls_required != NULL)
			s->cb.on_tls_required(s->user);
		return error;
	}
	if (neg.tpdu_code != 0xF0U)
		return RDP_ERR_PROTO;
	return handle_mcs_pdu(s, dt_pay, dt_len);
}

/* Processes an incoming MCS PDU on the server. */
static int
handle_mcs_pdu(
	struct rdp_server *s,
	const uint8_t *data,
	size_t len)
{
	struct rdp_mcs_connect_info ci;
	struct rdp_mcs_pdu pdu;
	struct rdp_buf out;
	const uint8_t *vc_msg;
	size_t vc_len;
	int error;
	if (s->state == RDP_SRV_WAIT_MCS_CONNECT_INIT) {
		error = rdp_mcs_decode_connect_initial(data, len, &ci);
		if (error != RDP_OK)
			return error;
		s->has_drdynvc = ci.has_drdynvc;
		if (ci.width > 0U && ci.height > 0U) {
			s->width = ci.width;
			s->height = ci.height;
		}
		rdp_buf_init(&out);
		error = rdp_mcs_encode_connect_response(
			&out, s->has_drdynvc ? RDP_MCS_DRDYNVC_CHANNEL : 0U);
		if (error == RDP_OK) {
			s->state = RDP_SRV_WAIT_ERECT_DOMAIN;
			error = send_mcs_dt(s, &out);
		}
		rdp_buf_free(&out);
		return error;
	}
	error = rdp_mcs_decode_pdu(data, len, &pdu);
	if (error != RDP_OK)
		return error;
	if (pdu.choice == 8U) {
		s->state = RDP_SRV_DISCONNECTED;
		if (s->cb.on_disconnect != NULL)
			s->cb.on_disconnect(s->user, 0U);
		return RDP_OK;
	}
	if (s->state == RDP_SRV_WAIT_ERECT_DOMAIN && pdu.choice == 1U) {
		s->state = RDP_SRV_WAIT_ATTACH_USER;
		return RDP_OK;
	}
	if (s->state == RDP_SRV_WAIT_ATTACH_USER && pdu.choice == 10U) {
		rdp_buf_init(&out);
		error = rdp_mcs_encode_attach_user_confirm(&out, s->user_id);
		if (error == RDP_OK) {
			s->state = RDP_SRV_WAIT_JOIN_CHANNELS;
			error = send_mcs_dt(s, &out);
		}
		rdp_buf_free(&out);
		return error;
	}
	if (s->state == RDP_SRV_WAIT_JOIN_CHANNELS && pdu.choice == 14U) {
		if (pdu.channel_id == RDP_MCS_IO_CHANNEL)
			s->io_joined = 1;
		else if (pdu.channel_id == s->user_id)
			s->user_joined = 1;
		else if (pdu.channel_id == RDP_MCS_DRDYNVC_CHANNEL)
			s->dynvc_joined = 1;
		rdp_buf_init(&out);
		error = rdp_mcs_encode_channel_join_confirm(&out, pdu.user_id,
							    pdu.channel_id);
		if (error == RDP_OK)
			error = send_mcs_dt(s, &out);
		rdp_buf_free(&out);
		if (s->io_joined != 0 && s->user_joined != 0 &&
		    (s->has_drdynvc == 0 || s->dynvc_joined != 0)) {
			s->state = RDP_SRV_WAIT_INFO_PKT;
		}
		return error;
	}
	if (pdu.choice != 25U)
		return RDP_OK;
	if (pdu.channel_id == RDP_MCS_IO_CHANNEL)
		return handle_io_pdu(s, pdu.payload, pdu.payload_len);
	if (s->has_drdynvc != 0 && pdu.channel_id == RDP_MCS_DRDYNVC_CHANNEL) {
		vc_msg = NULL;
		vc_len = 0U;
		error = rdp_vc_reasm_feed(&s->vc_reasm, pdu.payload,
					  pdu.payload_len, &vc_msg, &vc_len);
		if (error != RDP_OK)
			return error;
		if (vc_msg != NULL)
			return handle_dynvc_pdu(s, vc_msg, vc_len);
	}
	return RDP_OK;
}

/* Processes a TS_INFO_PACKET, ConfirmActivePDU, or Share Data PDU. */
static int
handle_io_pdu(
	struct rdp_server *s,
	const uint8_t *data,
	size_t len)
{
	struct rdp_sec_info info;
	struct rdp_active_pdu act;
	struct rdp_buf out;
	uint16_t pdu_type;
	uint8_t pdu_type2;
	uint32_t share_id;
	const uint8_t *body;
	size_t body_len;
	int error;
	if (s->state == RDP_SRV_WAIT_INFO_PKT) {
		error = rdp_sec_decode_info_pkt(data, len, &info);
		if (error != RDP_OK)
			return error;
		s->state = RDP_SRV_WAIT_AUTH_DECISION;
		if (s->cb.on_auth_request != NULL) {
			s->cb.on_auth_request(s->user, info.domain,
					      info.user_name, info.password);
			return RDP_OK;
		}
		return rdp_server_accept_auth(s);
	}
	if (s->state == RDP_SRV_WAIT_CONFIRM_ACTIVE) {
		error = rdp_sec_decode_active_pdu(data, len,
						  RDP_PDUTYPE_CONFIRMACTIVE,
						  &act);
		if (error != RDP_OK)
			return error;
		s->state = RDP_SRV_WAIT_FINALIZE;
		return RDP_OK;
	}
	error = rdp_sec_decode_share_hdr(data, len, &pdu_type, &pdu_type2,
					 &share_id, &body, &body_len);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(share_id);
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(body_len);
	if (s->state == RDP_SRV_WAIT_FINALIZE &&
	    pdu_type == RDP_PDUTYPE_DATAPDU &&
	    pdu_type2 == RDP_PDUTYPE2_FONTLIST) {
		rdp_buf_init(&out);
		error = rdp_sec_encode_share_data_sync(&out, RDP_SERVER_SHARE_ID,
						       RDP_MCS_IO_CHANNEL,
						       s->user_id);
		if (error == RDP_OK)
			error = send_io_data(s, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_sec_encode_share_data_control(
				&out, RDP_SERVER_SHARE_ID, RDP_MCS_IO_CHANNEL,
				RDP_CTRLACTION_COOPERATE, 0U, 0U);
		if (error == RDP_OK)
			error = send_io_data(s, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_sec_encode_share_data_control(
				&out, RDP_SERVER_SHARE_ID, RDP_MCS_IO_CHANNEL,
				RDP_CTRLACTION_GRANTED_CONTROL, s->user_id,
				0x03EAU);
		if (error == RDP_OK)
			error = send_io_data(s, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_sec_encode_share_data_fontmap(
				&out, RDP_SERVER_SHARE_ID, RDP_MCS_IO_CHANNEL);
		if (error == RDP_OK)
			error = send_io_data(s, &out);
		if (error == RDP_OK && s->has_drdynvc != 0) {
			rdp_buf_clear(&out);
			error = rdp_dynvc_encode_caps(&out, 2U);
			if (error == RDP_OK)
				error = rdp_vc_send_chunked(send_dynvc_chunk_cb,
							    s, out.data,
							    out.len);
		}
		rdp_buf_free(&out);
		if (error != RDP_OK)
			return error;
		s->state = RDP_SRV_CONNECTED;
		if (s->cb.on_connected != NULL)
			s->cb.on_connected(s->user, s->width, s->height);
		return RDP_OK;
	}
	return RDP_OK;
}

/* Processes a reassembled DRDYNVC PDU on the server. */
static int
handle_dynvc_pdu(
	struct rdp_server *s,
	const uint8_t *data,
	size_t len)
{
	struct rdp_dynvc_msg msg;
	struct rdp_buf out;
	const uint8_t *sdata;
	size_t slen;
	uint32_t rw;
	uint32_t rh;
	int error;
	error = rdp_dynvc_decode(data, len, &msg);
	if (error != RDP_OK)
		return error;
	if (msg.cmd == 0x05U) {
		rdp_buf_init(&out);
		error = rdp_dynvc_encode_create_req(&out, RDP_DYNVC_ID_GFX,
						    RDP_DYNVC_NAME_GFX);
		if (error == RDP_OK)
			error = rdp_vc_send_chunked(send_dynvc_chunk_cb, s,
						    out.data, out.len);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_dynvc_encode_create_req(
				&out, RDP_DYNVC_ID_DISP, RDP_DYNVC_NAME_DISP);
		if (error == RDP_OK)
			error = rdp_vc_send_chunked(send_dynvc_chunk_cb, s,
						    out.data, out.len);
		rdp_buf_free(&out);
		return error;
	}
	if (msg.cmd == 0x01U) {
		if (msg.creation_status != 0U)
			return RDP_OK;
		if (msg.channel_id == RDP_DYNVC_ID_GFX) {
			s->gfx_open = 1;
		} else if (msg.channel_id == RDP_DYNVC_ID_DISP) {
			s->disp_open = 1;
			rdp_buf_init(&out);
			error = rdp_disp_encode_caps(&out);
			if (error == RDP_OK)
				error = send_dynvc_msg(s, RDP_DYNVC_ID_DISP, &out);
			rdp_buf_free(&out);
			return error;
		}
		return RDP_OK;
	}
	if (msg.cmd == 0x02U || msg.cmd == 0x03U) {
		if (msg.channel_id == RDP_DYNVC_ID_GFX) {
			sdata = NULL;
			slen = 0U;
			error = rdp_dynvc_stream_feed(&s->gfx_stream, &msg,
						      &sdata, &slen);
			if (error != RDP_OK)
				return error;
			if (sdata != NULL)
				return handle_gfx_pdu(s, sdata, slen);
		} else if (msg.channel_id == RDP_DYNVC_ID_DISP) {
			sdata = NULL;
			slen = 0U;
			error = rdp_dynvc_stream_feed(&s->disp_stream, &msg,
						      &sdata, &slen);
			if (error != RDP_OK)
				return error;
			if (sdata != NULL) {
				error = rdp_disp_decode_monitor_layout(
					sdata, slen, &rw, &rh);
				if (error != RDP_OK)
					return error;
				if (s->cb.on_resize != NULL)
					s->cb.on_resize(s->user, rw, rh);
			}
		}
	}
	return RDP_OK;
}

/* Processes an MS-RDPEGFX PDU received from the client. */
static int
handle_gfx_pdu(
	struct rdp_server *s,
	const uint8_t *data,
	size_t len)
{
	uint16_t cmd_id;
	const uint8_t *body;
	size_t body_len;
	uint32_t ver;
	uint32_t qdepth;
	uint32_t fid;
	uint32_t total;
	struct rdp_buf out;
	int error;
	error = rdp_gfx_decode_header(data, len, &cmd_id, &body, &body_len);
	if (error != RDP_OK)
		return error;
	if (cmd_id == RDPGFX_CMD_CAPS_ADVERTISE) {
		error = rdp_gfx_decode_caps_advertise(body, body_len, &ver);
		if (error != RDP_OK)
			return error;
		rdp_buf_init(&out);
		error = rdp_gfx_encode_caps_confirm(&out, ver);
		if (error == RDP_OK)
			error = send_dynvc_msg(s, RDP_DYNVC_ID_GFX, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_gfx_encode_reset_graphics(&out, s->width,
							      s->height);
		if (error == RDP_OK)
			error = send_dynvc_msg(s, RDP_DYNVC_ID_GFX, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK) {
			error = rdp_gfx_encode_create_surface(
				&out, 0U, (uint16_t)s->width,
				(uint16_t)s->height);
		}
		if (error == RDP_OK)
			error = send_dynvc_msg(s, RDP_DYNVC_ID_GFX, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_gfx_encode_map_surface(&out, 0U);
		if (error == RDP_OK)
			error = send_dynvc_msg(s, RDP_DYNVC_ID_GFX, &out);
		rdp_buf_free(&out);
		if (error != RDP_OK)
			return error;
		s->gfx_ready = 1;
		return RDP_OK;
	}
	if (cmd_id == RDPGFX_CMD_FRAME_ACKNOWLEDGE) {
		error = rdp_gfx_decode_frame_ack(body, body_len, &qdepth, &fid,
						 &total);
		if (error != RDP_OK)
			return error;
		if (s->unacked_frames > 0U)
			s->unacked_frames--;
		if (s->cb.on_frame_ack != NULL)
			s->cb.on_frame_ack(s->user, fid, qdepth, total);
		return RDP_OK;
	}
	return RDP_OK;
}
