/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * I/O-independent RDP client protocol state machine.
 */

#include "rdp-internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * Per-connection RDP client state machine instance.
 */
struct rdp_client {
	struct rdp_callbacks cb;
	void *user;
	enum rdp_client_state state;
	struct rdp_client_config cfg;
	char domain_buf[128];
	char user_buf[128];
	char pass_buf[128];
	char cname_buf[128];
	struct rdp_buf rx;
	struct rdp_vc_reasm vc_reasm;
	struct rdp_dynvc_stream gfx_stream;
	struct rdp_dynvc_stream disp_stream;
	uint16_t user_id;
	uint16_t io_channel;
	uint16_t drdynvc_channel;
	int has_drdynvc;
	uint32_t share_id;
	uint32_t gfx_dynvc_id;
	uint32_t disp_dynvc_id;
	int has_gfx_dynvc;
	int has_disp_dynvc;
	uint32_t total_decoded;
	uint32_t cur_frame_id;
	uint16_t cur_codec_id;
	int has_pending_frame;
	struct rdp_avc_stream cur_avc;
	struct rdp_avc_rect luma_rects[RDP_MAX_AVC_RECTS];
	struct rdp_avc_quant luma_quants[RDP_MAX_AVC_RECTS];
	struct rdp_avc_rect chroma_rects[RDP_MAX_AVC_RECTS];
	struct rdp_avc_quant chroma_quants[RDP_MAX_AVC_RECTS];
	struct rdp_buf frame_copy;
	size_t chroma_off;
};

static int send_raw(struct rdp_client *c, const struct rdp_buf *buf);
static int send_mcs_dt(struct rdp_client *c, const struct rdp_buf *mcs);
static int send_io_data(struct rdp_client *c, const struct rdp_buf *sec_pdu);
static int send_dynvc_chunk_cb(void *ctx, const uint8_t *chunk, size_t len);
static int send_dynvc_msg(struct rdp_client *c, uint32_t chan_id, const struct rdp_buf *payload);
static int copy_str(char *dst, size_t dst_sz, const char *src);
static int handle_tpkt_packet(struct rdp_client *c, const uint8_t *tpdu, size_t len);
static int handle_mcs_pdu(struct rdp_client *c, const uint8_t *data, size_t len);
static int handle_io_pdu(struct rdp_client *c, const uint8_t *data, size_t len);
static int handle_dynvc_pdu(struct rdp_client *c, const uint8_t *data, size_t len);
static int handle_gfx_pdu(struct rdp_client *c, const uint8_t *data, size_t len);

/*
 * Allocates and initializes a new RDP client state machine.
 */
struct rdp_client *
rdp_client_create(
	const struct rdp_callbacks *cb,
	void *user)
{
	struct rdp_client *c;

	/* Rejects callers without a valid send_bytes transport callback. */
	if (cb == NULL || cb->send_bytes == NULL)
		return NULL;

	/* Allocates and zeroes the client context. */
	c = (struct rdp_client *)calloc(1U, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->cb = *cb;
	c->user = user;
	c->state = RDP_CLI_INIT;
	rdp_buf_init(&c->rx);
	rdp_buf_init(&c->vc_reasm.buf);
	rdp_buf_init(&c->gfx_stream.buf);
	rdp_buf_init(&c->disp_stream.buf);
	rdp_buf_init(&c->frame_copy);

	/* Succeeded: client state machine ready for rdp_client_connect(). */
	return c;
}

/*
 * Destroys an RDP client state machine and frees all buffers.
 */
void
rdp_client_destroy(
	struct rdp_client *client)
{
	/* Allows NULL pointers safely. */
	if (client == NULL)
		return;
	rdp_buf_free(&client->rx);
	rdp_vc_reasm_reset(&client->vc_reasm);
	rdp_dynvc_stream_reset(&client->gfx_stream);
	rdp_dynvc_stream_reset(&client->disp_stream);
	rdp_buf_free(&client->frame_copy);
	free(client);
}

/*
 * Begins an RDP client handshake by sending X.224 Connection Request.
 */
int
rdp_client_connect(
	struct rdp_client *client,
	const struct rdp_client_config *cfg)
{
	struct rdp_buf out;
	int error;

	/* Validates arguments and initial state. */
	if (client == NULL || cfg == NULL)
		return RDP_ERR_INVAL;
	if (client->state != RDP_CLI_INIT)
		return RDP_ERR_STATE;

	/* Stores connection configuration strings locally. */
	copy_str(client->domain_buf, sizeof(client->domain_buf), cfg->domain);
	copy_str(client->user_buf, sizeof(client->user_buf), cfg->user_name);
	copy_str(client->pass_buf, sizeof(client->pass_buf), cfg->password);
	copy_str(client->cname_buf, sizeof(client->cname_buf), cfg->client_name);
	client->cfg.width = cfg->width > 0U ? cfg->width : 1280U;
	client->cfg.height = cfg->height > 0U ? cfg->height : 720U;
	client->cfg.domain = client->domain_buf;
	client->cfg.user_name = client->user_buf;
	client->cfg.password = client->pass_buf;
	client->cfg.client_name = client->cname_buf;

	/* Encodes and sends X.224 Connection Request requesting PROTOCOL_SSL. */
	rdp_buf_init(&out);
	error = rdp_x224_encode_cr(&out, RDP_NEG_PROTO_SSL);
	if (error != RDP_OK) {
		rdp_buf_free(&out);
		return error;
	}
	client->state = RDP_CLI_WAIT_X224_CC;
	error = send_raw(client, &out);
	rdp_buf_free(&out);
	if (error != RDP_OK)
		return error;

	/* Succeeded: waiting for X.224 Connection Confirm. */
	return RDP_OK;
}

/*
 * Notifies the client state machine that the TLS handshake completed.
 */
int
rdp_client_tls_established(
	struct rdp_client *client)
{
	struct rdp_buf mcs;
	int error;

	/* Validates that the client is waiting for TLS completion. */
	if (client == NULL)
		return RDP_ERR_INVAL;
	if (client->state != RDP_CLI_WAIT_TLS)
		return RDP_ERR_STATE;

	/* Sends MCS Connect-Initial with client core, security, and drdynvc. */
	rdp_buf_init(&mcs);
	error = rdp_mcs_encode_connect_initial(&mcs, client->cfg.width,
					       client->cfg.height, 1);
	if (error != RDP_OK) {
		rdp_buf_free(&mcs);
		return error;
	}
	client->state = RDP_CLI_WAIT_MCS_CONNECT_RSP;
	error = send_mcs_dt(client, &mcs);
	rdp_buf_free(&mcs);
	if (error != RDP_OK)
		return error;

	/* Succeeded: MCS Connect-Initial transmitted over TLS. */
	return RDP_OK;
}

/*
 * Feeds incoming bytes from the transport into the client state machine.
 */
int
rdp_client_feed(
	struct rdp_client *client,
	const uint8_t *data,
	size_t len)
{
	struct rdp_packet pkt;
	size_t consumed;
	int error;

	/* Validates inputs. */
	if (client == NULL || (data == NULL && len > 0U))
		return RDP_ERR_INVAL;

	/* Appends incoming bytes to the stream reassembly buffer. */
	error = rdp_buf_append(&client->rx, data, len);
	if (error != RDP_OK)
		return error;

	/* Processes every complete TPKT packet currently in rx. */
	for (;;) {
		consumed = 0U;
		error = rdp_stream_next_packet(&client->rx, &pkt, &consumed);
		if (error == RDP_ERR_SHORT)
			break;
		if (error != RDP_OK)
			return error;
		if (pkt.is_fastpath != 0) {
			rdp_buf_consume(&client->rx, consumed);
			continue;
		}
		error = handle_tpkt_packet(client, pkt.payload, pkt.payload_len);
		rdp_buf_consume(&client->rx, consumed);
		if (error != RDP_OK)
			return error;
	}

	/* Succeeded: all available complete packets processed. */
	return RDP_OK;
}

/*
 * Returns nonzero when the client is waiting for TLS handshake completion.
 */
int
rdp_client_needs_tls(
	const struct rdp_client *client)
{
	if (client == NULL)
		return 0;
	if (client->state == RDP_CLI_WAIT_TLS)
		return 1;
	return 0;
}

/*
 * Returns nonzero when the client session is active and connected.
 */
int
rdp_client_is_connected(
	const struct rdp_client *client)
{
	if (client == NULL)
		return 0;
	if (client->state == RDP_CLI_CONNECTED)
		return 1;
	return 0;
}

/*
 * Sends an MCS DisconnectProviderUltimatum and transitions to disconnected.
 */
int
rdp_client_disconnect(
	struct rdp_client *client)
{
	struct rdp_buf mcs;
	int error;
	if (client == NULL)
		return RDP_ERR_INVAL;
	if (client->state == RDP_CLI_DISCONNECTED)
		return RDP_OK;

	/* Sends MCS Disconnect if TLS/MCS has started. */
	if (client->state >= RDP_CLI_WAIT_MCS_CONNECT_RSP) {
		rdp_buf_init(&mcs);
		error = rdp_mcs_encode_disconnect(&mcs, 3U);
		if (error == RDP_OK)
			send_mcs_dt(client, &mcs);
		rdp_buf_free(&mcs);
	}
	client->state = RDP_CLI_DISCONNECTED;
	if (client->cb.on_disconnect != NULL)
		client->cb.on_disconnect(client->user, 0U);

	/* Succeeded: client disconnected. */
	return RDP_OK;
}

/*
 * Sends a Fast-Path keyboard scancode event to the server.
 */
int
rdp_client_send_key(
	struct rdp_client *client,
	uint16_t scancode,
	uint16_t flags)
{
	struct rdp_buf out;
	int error;
	if (client == NULL)
		return RDP_ERR_INVAL;
	if (client->state != RDP_CLI_CONNECTED)
		return RDP_ERR_STATE;
	rdp_buf_init(&out);
	error = rdp_input_encode_key(&out, scancode, flags);
	if (error != RDP_OK) {
		rdp_buf_free(&out);
		return error;
	}
	error = send_raw(client, &out);
	rdp_buf_free(&out);
	if (error != RDP_OK)
		return error;

	/* Succeeded: keyboard event sent. */
	return RDP_OK;
}

/*
 * Sends a Fast-Path Unicode keyboard event to the server.
 */
int
rdp_client_send_unicode(
	struct rdp_client *client,
	uint16_t codepoint,
	uint16_t flags)
{
	struct rdp_buf out;
	int error;
	if (client == NULL)
		return RDP_ERR_INVAL;
	if (client->state != RDP_CLI_CONNECTED)
		return RDP_ERR_STATE;
	rdp_buf_init(&out);
	error = rdp_input_encode_unicode(&out, codepoint, flags);
	if (error != RDP_OK) {
		rdp_buf_free(&out);
		return error;
	}
	error = send_raw(client, &out);
	rdp_buf_free(&out);
	if (error != RDP_OK)
		return error;

	/* Succeeded: Unicode event sent. */
	return RDP_OK;
}

/*
 * Sends a Fast-Path mouse pointer event to the server.
 */
int
rdp_client_send_mouse(
	struct rdp_client *client,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	struct rdp_buf out;
	int error;
	if (client == NULL)
		return RDP_ERR_INVAL;
	if (client->state != RDP_CLI_CONNECTED)
		return RDP_ERR_STATE;
	rdp_buf_init(&out);
	error = rdp_input_encode_mouse(&out, flags, x, y);
	if (error != RDP_OK) {
		rdp_buf_free(&out);
		return error;
	}
	error = send_raw(client, &out);
	rdp_buf_free(&out);
	if (error != RDP_OK)
		return error;

	/* Succeeded: mouse event sent. */
	return RDP_OK;
}

/*
 * Sends a Fast-Path keyboard toggle synchronization event.
 */
int
rdp_client_send_sync(
	struct rdp_client *client,
	uint32_t toggle_flags)
{
	struct rdp_buf out;
	int error;
	if (client == NULL)
		return RDP_ERR_INVAL;
	if (client->state != RDP_CLI_CONNECTED)
		return RDP_ERR_STATE;
	rdp_buf_init(&out);
	error = rdp_input_encode_sync(&out, toggle_flags);
	if (error != RDP_OK) {
		rdp_buf_free(&out);
		return error;
	}
	error = send_raw(client, &out);
	rdp_buf_free(&out);
	if (error != RDP_OK)
		return error;

	/* Succeeded: sync event sent. */
	return RDP_OK;
}

/*
 * Sends an RDPGFX_FRAME_ACKNOWLEDGE_PDU for a decoded frame.
 */
int
rdp_client_send_frame_ack(
	struct rdp_client *client,
	uint32_t frame_id,
	uint32_t queue_depth)
{
	struct rdp_buf gfx;
	int error;
	if (client == NULL)
		return RDP_ERR_INVAL;
	if (client->state != RDP_CLI_CONNECTED || client->has_gfx_dynvc == 0)
		return RDP_ERR_STATE;
	client->total_decoded++;
	rdp_buf_init(&gfx);
	error = rdp_gfx_encode_frame_ack(&gfx, queue_depth, frame_id,
					 client->total_decoded);
	if (error != RDP_OK) {
		rdp_buf_free(&gfx);
		return error;
	}
	error = send_dynvc_msg(client, client->gfx_dynvc_id, &gfx);
	rdp_buf_free(&gfx);
	if (error != RDP_OK)
		return error;

	/* Succeeded: frame acknowledgement sent. */
	return RDP_OK;
}

/*
 * Sends a Display Control monitor layout request to resize the desktop.
 */
int
rdp_client_request_resize(
	struct rdp_client *client,
	uint32_t width,
	uint32_t height)
{
	struct rdp_buf disp;
	int error;
	if (client == NULL || width == 0U || height == 0U)
		return RDP_ERR_INVAL;
	if (client->state != RDP_CLI_CONNECTED || client->has_disp_dynvc == 0)
		return RDP_ERR_STATE;
	rdp_buf_init(&disp);
	error = rdp_disp_encode_monitor_layout(&disp, width, height);
	if (error != RDP_OK) {
		rdp_buf_free(&disp);
		return error;
	}
	error = send_dynvc_msg(client, client->disp_dynvc_id, &disp);
	rdp_buf_free(&disp);
	if (error != RDP_OK)
		return error;

	/* Succeeded: resize request transmitted over MS-RDPEDISP. */
	return RDP_OK;
}

/* Sends raw bytes through the caller's transport callback. */
static int
send_raw(
	struct rdp_client *c,
	const struct rdp_buf *buf)
{
	int rc;
	rc = c->cb.send_bytes(c->user, buf->data, buf->len);
	if (rc < 0)
		return RDP_ERR_IO;
	return RDP_OK;
}

/* Wraps an MCS PDU in X.224 DT + TPKT and sends it. */
static int
send_mcs_dt(
	struct rdp_client *c,
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
	error = send_raw(c, &out);
	rdp_buf_free(&out);
	return error;
}

/* Wraps a security or Share PDU in MCS SendDataRequest on io_channel. */
static int
send_io_data(
	struct rdp_client *c,
	const struct rdp_buf *sec_pdu)
{
	struct rdp_buf mcs;
	int error;
	rdp_buf_init(&mcs);
	error = rdp_mcs_encode_send_data_req(&mcs, c->user_id, c->io_channel,
					     sec_pdu->data, sec_pdu->len);
	if (error != RDP_OK) {
		rdp_buf_free(&mcs);
		return error;
	}
	error = send_mcs_dt(c, &mcs);
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
	struct rdp_client *c;
	struct rdp_buf mcs;
	int error;
	c = (struct rdp_client *)ctx;
	rdp_buf_init(&mcs);
	error = rdp_mcs_encode_send_data_req(&mcs, c->user_id,
					     c->drdynvc_channel, chunk, len);
	if (error != RDP_OK) {
		rdp_buf_free(&mcs);
		return error;
	}
	error = send_mcs_dt(c, &mcs);
	rdp_buf_free(&mcs);
	return error;
}

/* Wraps payload in DYNVC_DATA and sends it chunked over drdynvc. */
static int
send_dynvc_msg(
	struct rdp_client *c,
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
	error = rdp_vc_send_chunked(send_dynvc_chunk_cb, c, dyn.data, dyn.len);
	rdp_buf_free(&dyn);
	return error;
}

/* Copies an optional NUL-terminated string into a fixed buffer. */
static int
copy_str(
	char *dst,
	size_t dst_sz,
	const char *src)
{
	size_t n;
	if (src == NULL) {
		dst[0] = '\0';
		return RDP_OK;
	}
	n = strlen(src);
	if (n >= dst_sz)
		n = dst_sz - 1U;
	memcpy(dst, src, n);
	dst[n] = '\0';
	return RDP_OK;
}

/* Dispatches a decoded TPKT payload to X.224 or MCS handlers. */
static int
handle_tpkt_packet(
	struct rdp_client *c,
	const uint8_t *tpdu,
	size_t len)
{
	struct rdp_x224_neg neg;
	const uint8_t *dt_pay;
	size_t dt_len;
	int error;
	error = rdp_x224_decode(tpdu, len, &neg, &dt_pay, &dt_len);
	if (error != RDP_OK)
		return error;
	if (c->state == RDP_CLI_WAIT_X224_CC) {
		if (neg.tpdu_code != 0xD0U || neg.neg_type != 0x02U ||
		    (neg.protocols & RDP_NEG_PROTO_SSL) == 0U)
			return RDP_ERR_UNSUPPORTED;
		c->state = RDP_CLI_WAIT_TLS;
		if (c->cb.on_tls_required != NULL)
			c->cb.on_tls_required(c->user);
		return RDP_OK;
	}
	if (neg.tpdu_code != 0xF0U)
		return RDP_ERR_PROTO;
	return handle_mcs_pdu(c, dt_pay, dt_len);
}

/* Processes an incoming MCS PDU on the client. */
static int
handle_mcs_pdu(
	struct rdp_client *c,
	const uint8_t *data,
	size_t len)
{
	struct rdp_mcs_connect_info ci;
	struct rdp_mcs_pdu pdu;
	struct rdp_buf out;
	const uint8_t *vc_msg;
	size_t vc_len;
	int error;
	if (c->state == RDP_CLI_WAIT_MCS_CONNECT_RSP) {
		error = rdp_mcs_decode_connect_response(data, len, &ci);
		if (error != RDP_OK)
			return error;
		c->io_channel = ci.io_channel != 0U ? ci.io_channel : RDP_MCS_IO_CHANNEL;
		c->drdynvc_channel = ci.drdynvc_channel;
		c->has_drdynvc = ci.has_drdynvc;
		rdp_buf_init(&out);
		error = rdp_mcs_encode_erect_domain(&out);
		if (error == RDP_OK)
			error = send_mcs_dt(c, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_mcs_encode_attach_user_req(&out);
		if (error == RDP_OK) {
			c->state = RDP_CLI_WAIT_ATTACH_CONFIRM;
			error = send_mcs_dt(c, &out);
		}
		rdp_buf_free(&out);
		return error;
	}
	error = rdp_mcs_decode_pdu(data, len, &pdu);
	if (error != RDP_OK)
		return error;
	if (pdu.choice == 8U) {
		c->state = RDP_CLI_DISCONNECTED;
		if (c->cb.on_disconnect != NULL)
			c->cb.on_disconnect(c->user, 0U);
		return RDP_OK;
	}
	if (c->state == RDP_CLI_WAIT_ATTACH_CONFIRM && pdu.choice == 11U) {
		c->user_id = pdu.user_id;
		rdp_buf_init(&out);
		error = rdp_mcs_encode_channel_join_req(&out, c->user_id,
							c->io_channel);
		if (error == RDP_OK) {
			c->state = RDP_CLI_WAIT_JOIN_IO;
			error = send_mcs_dt(c, &out);
		}
		rdp_buf_free(&out);
		return error;
	}
	if (c->state == RDP_CLI_WAIT_JOIN_IO && pdu.choice == 15U) {
		rdp_buf_init(&out);
		error = rdp_mcs_encode_channel_join_req(&out, c->user_id,
							c->user_id);
		if (error == RDP_OK) {
			c->state = RDP_CLI_WAIT_JOIN_USER;
			error = send_mcs_dt(c, &out);
		}
		rdp_buf_free(&out);
		return error;
	}
	if (c->state == RDP_CLI_WAIT_JOIN_USER && pdu.choice == 15U) {
		rdp_buf_init(&out);
		if (c->has_drdynvc != 0 && c->drdynvc_channel != 0U) {
			error = rdp_mcs_encode_channel_join_req(&out, c->user_id,
								c->drdynvc_channel);
			if (error == RDP_OK) {
				c->state = RDP_CLI_WAIT_JOIN_DYNVC;
				error = send_mcs_dt(c, &out);
			}
		} else {
			error = rdp_sec_encode_info_pkt(&out, &c->cfg);
			if (error == RDP_OK) {
				c->state = RDP_CLI_WAIT_LICENSE;
				error = send_io_data(c, &out);
			}
		}
		rdp_buf_free(&out);
		return error;
	}
	if (c->state == RDP_CLI_WAIT_JOIN_DYNVC && pdu.choice == 15U) {
		rdp_buf_init(&out);
		error = rdp_sec_encode_info_pkt(&out, &c->cfg);
		if (error == RDP_OK) {
			c->state = RDP_CLI_WAIT_LICENSE;
			error = send_io_data(c, &out);
		}
		rdp_buf_free(&out);
		return error;
	}
	if (pdu.choice != 26U)
		return RDP_OK;
	if (pdu.channel_id == c->io_channel)
		return handle_io_pdu(c, pdu.payload, pdu.payload_len);
	if (c->has_drdynvc != 0 && pdu.channel_id == c->drdynvc_channel) {
		vc_msg = NULL;
		vc_len = 0U;
		error = rdp_vc_reasm_feed(&c->vc_reasm, pdu.payload,
					  pdu.payload_len, &vc_msg, &vc_len);
		if (error != RDP_OK)
			return error;
		if (vc_msg != NULL)
			return handle_dynvc_pdu(c, vc_msg, vc_len);
	}
	return RDP_OK;
}

/* Processes a Share Control/Data or licensing PDU on the I/O channel. */
static int
handle_io_pdu(
	struct rdp_client *c,
	const uint8_t *data,
	size_t len)
{
	struct rdp_active_pdu act;
	struct rdp_buf out;
	uint16_t pdu_type;
	uint8_t pdu_type2;
	uint32_t share_id;
	const uint8_t *body;
	size_t body_len;
	uint32_t err_info;
	struct rdp_reader r;
	int error;
	if (c->state == RDP_CLI_WAIT_LICENSE) {
		error = rdp_sec_decode_license_valid(data, len);
		if (error == RDP_OK) {
			c->state = RDP_CLI_WAIT_DEMAND_ACTIVE;
			return RDP_OK;
		}
	}
	if (c->state == RDP_CLI_WAIT_DEMAND_ACTIVE) {
		error = rdp_sec_decode_active_pdu(data, len,
						  RDP_PDUTYPE_DEMANDACTIVE,
						  &act);
		if (error != RDP_OK)
			return error;
		c->share_id = act.share_id;
		if (act.width > 0U && act.height > 0U) {
			c->cfg.width = act.width;
			c->cfg.height = act.height;
		}
		rdp_buf_init(&out);
		error = rdp_sec_encode_confirm_active(&out, c->share_id,
						      c->cfg.width,
						      c->cfg.height,
						      c->user_id);
		if (error == RDP_OK)
			error = send_io_data(c, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_sec_encode_share_data_sync(&out, c->share_id,
							       c->user_id,
							       RDP_MCS_IO_CHANNEL);
		if (error == RDP_OK)
			error = send_io_data(c, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_sec_encode_share_data_control(
				&out, c->share_id, c->user_id,
				RDP_CTRLACTION_COOPERATE, 0U, 0U);
		if (error == RDP_OK)
			error = send_io_data(c, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_sec_encode_share_data_control(
				&out, c->share_id, c->user_id,
				RDP_CTRLACTION_REQUEST_CONTROL, 0U, 0U);
		if (error == RDP_OK)
			error = send_io_data(c, &out);
		rdp_buf_clear(&out);
		if (error == RDP_OK)
			error = rdp_sec_encode_share_data_fontlist(
				&out, c->share_id, c->user_id);
		if (error == RDP_OK) {
			c->state = RDP_CLI_WAIT_FINALIZE;
			error = send_io_data(c, &out);
		}
		rdp_buf_free(&out);
		return error;
	}
	error = rdp_sec_decode_share_hdr(data, len, &pdu_type, &pdu_type2,
					 &share_id, &body, &body_len);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(share_id);
	if (pdu_type == RDP_PDUTYPE_DATAPDU && pdu_type2 == RDP_PDUTYPE2_FONTMAP) {
		if (c->state == RDP_CLI_WAIT_FINALIZE) {
			c->state = RDP_CLI_CONNECTED;
			if (c->cb.on_connected != NULL)
				c->cb.on_connected(c->user, c->cfg.width,
						   c->cfg.height);
		}
		return RDP_OK;
	}
	if (pdu_type == RDP_PDUTYPE_DATAPDU &&
	    pdu_type2 == RDP_PDUTYPE2_SET_ERROR_INFO) {
		err_info = 0U;
		rdp_reader_init(&r, body, body_len);
		rdp_read_u32_le(&r, &err_info);
		c->state = RDP_CLI_DISCONNECTED;
		if (c->cb.on_disconnect != NULL)
			c->cb.on_disconnect(c->user, err_info);
		return RDP_OK;
	}
	return RDP_OK;
}

/* Processes a reassembled DRDYNVC PDU on the client. */
static int
handle_dynvc_pdu(
	struct rdp_client *c,
	const uint8_t *data,
	size_t len)
{
	struct rdp_dynvc_msg msg;
	struct rdp_buf out;
	struct rdp_buf gfx;
	const uint8_t *sdata;
	size_t slen;
	int error;
	error = rdp_dynvc_decode(data, len, &msg);
	if (error != RDP_OK)
		return error;
	if (msg.cmd == 0x05U) {
		rdp_buf_init(&out);
		error = rdp_dynvc_encode_caps(&out, 2U);
		if (error == RDP_OK)
			error = rdp_vc_send_chunked(send_dynvc_chunk_cb, c,
						    out.data, out.len);
		rdp_buf_free(&out);
		return error;
	}
	if (msg.cmd == 0x01U) {
		if (strcmp(msg.channel_name, RDP_DYNVC_NAME_GFX) == 0) {
			c->gfx_dynvc_id = msg.channel_id;
			c->has_gfx_dynvc = 1;
		} else if (strcmp(msg.channel_name, RDP_DYNVC_NAME_DISP) == 0) {
			c->disp_dynvc_id = msg.channel_id;
			c->has_disp_dynvc = 1;
		}
		rdp_buf_init(&out);
		error = rdp_dynvc_encode_create_rsp(&out, msg.channel_id, 0U);
		if (error == RDP_OK)
			error = rdp_vc_send_chunked(send_dynvc_chunk_cb, c,
						    out.data, out.len);
		rdp_buf_free(&out);
		if (error != RDP_OK)
			return error;
		if (c->has_gfx_dynvc != 0 && msg.channel_id == c->gfx_dynvc_id) {
			rdp_buf_init(&gfx);
			error = rdp_gfx_encode_caps_advertise(&gfx);
			if (error == RDP_OK)
				error = send_dynvc_msg(c, c->gfx_dynvc_id, &gfx);
			rdp_buf_free(&gfx);
			return error;
		}
		return RDP_OK;
	}
	if (msg.cmd == 0x02U || msg.cmd == 0x03U) {
		if (c->has_gfx_dynvc != 0 && msg.channel_id == c->gfx_dynvc_id) {
			sdata = NULL;
			slen = 0U;
			error = rdp_dynvc_stream_feed(&c->gfx_stream, &msg,
						      &sdata, &slen);
			if (error != RDP_OK)
				return error;
			if (sdata != NULL)
				return handle_gfx_pdu(c, sdata, slen);
		}
	}
	return RDP_OK;
}

/* Processes an MS-RDPEGFX PDU received on the Graphics dynamic channel. */
static int
handle_gfx_pdu(
	struct rdp_client *c,
	const uint8_t *data,
	size_t len)
{
	uint16_t cmd_id;
	const uint8_t *body;
	size_t body_len;
	struct rdp_reader r;
	uint32_t ver;
	uint32_t w;
	uint32_t h;
	uint32_t ts;
	uint32_t fid;
	struct rdp_avc_stream avc;
	int error;
	error = rdp_gfx_decode_header(data, len, &cmd_id, &body, &body_len);
	if (error != RDP_OK)
		return error;
	if (cmd_id == RDPGFX_CMD_CAPS_CONFIRM) {
		return rdp_gfx_decode_caps_confirm(body, body_len, &ver);
	} else if (cmd_id == RDPGFX_CMD_RESET_GRAPHICS) {
		rdp_reader_init(&r, body, body_len);
		error = rdp_read_u32_le(&r, &w);
		if (error != RDP_OK)
			return error;
		error = rdp_read_u32_le(&r, &h);
		if (error != RDP_OK)
			return error;
		if (w != c->cfg.width || h != c->cfg.height) {
			c->cfg.width = w;
			c->cfg.height = h;
			if (c->cb.on_resize != NULL)
				c->cb.on_resize(c->user, w, h);
		}
		return RDP_OK;
	} else if (cmd_id == RDPGFX_CMD_START_FRAME) {
		rdp_reader_init(&r, body, body_len);
		error = rdp_read_u32_le(&r, &ts);
		if (error != RDP_OK)
			return error;

		UNUSED_PARAMETER(ts);
		error = rdp_read_u32_le(&r, &c->cur_frame_id);
		if (error != RDP_OK)
			return error;
		c->has_pending_frame = 0;
		return RDP_OK;
	} else if (cmd_id == RDPGFX_CMD_WIRE_TO_SURFACE_1) {
		error = rdp_gfx_decode_avc_pdu(
			body, body_len, &c->cur_codec_id, &avc,
			c->luma_rects, c->luma_quants,
			c->chroma_rects, c->chroma_quants, RDP_MAX_AVC_RECTS);
		if (error != RDP_OK)
			return error;
		rdp_buf_clear(&c->frame_copy);
		c->chroma_off = 0U;
		if (avc.luma.data != NULL && avc.luma.data_len > 0U) {
			error = rdp_buf_append(&c->frame_copy, avc.luma.data,
					       avc.luma.data_len);
			if (error != RDP_OK)
				return error;
		}
		c->chroma_off = c->frame_copy.len;
		if (avc.chroma.data != NULL && avc.chroma.data_len > 0U) {
			error = rdp_buf_append(&c->frame_copy, avc.chroma.data,
					       avc.chroma.data_len);
			if (error != RDP_OK)
				return error;
		}
		c->cur_avc = avc;
		if (avc.luma.data_len > 0U)
			c->cur_avc.luma.data = c->frame_copy.data;
		if (avc.chroma.data_len > 0U)
			c->cur_avc.chroma.data = c->frame_copy.data + c->chroma_off;
		c->has_pending_frame = 1;
		return RDP_OK;
	} else if (cmd_id == RDPGFX_CMD_END_FRAME) {
		rdp_reader_init(&r, body, body_len);
		error = rdp_read_u32_le(&r, &fid);
		if (error != RDP_OK)
			return error;
		if (c->has_pending_frame != 0 && c->cb.on_avc_frame != NULL) {
			c->cb.on_avc_frame(c->user, fid, c->cur_codec_id,
					   &c->cur_avc);
		}
		c->has_pending_frame = 0;
		return RDP_OK;
	}
	return RDP_OK;
}
