/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit and client-server loopback tests for userland/desktop/librdp.
 */

#include "userland/desktop/librdp/rdp-internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * In-memory transport endpoint connecting a client and server in loopback.
 */
struct loopback_peer {
	struct rdp_buf tx;
	int connected;
	uint32_t width;
	uint32_t height;
	int tls_needed;
	int disconnected;
	uint32_t disconnect_err;
	int auth_called;
	char auth_domain[128];
	char auth_user[128];
	char auth_pass[128];
	int reject_auth;
	struct rdp_server *srv_handle;
	uint32_t frame_count;
	uint32_t last_frame_id;
	uint16_t last_codec_id;
	uint8_t last_avc444_mode;
	size_t last_luma_len;
	size_t last_chroma_len;
	uint8_t last_luma_head[8];
	uint32_t ack_count;
	uint32_t last_ack_id;
	int resize_called;
	uint32_t resize_w;
	uint32_t resize_h;
	uint32_t key_count;
	uint16_t last_scancode;
	uint16_t last_key_flags;
	uint32_t unicode_count;
	uint16_t last_codepoint;
	uint32_t mouse_count;
	uint16_t last_mouse_flags;
	uint16_t last_mouse_x;
	uint16_t last_mouse_y;
	uint32_t sync_count;
	uint32_t last_sync_flags;
};

static int peer_send_bytes(void *user, const uint8_t *data, size_t len);
static void peer_on_tls(void *user);
static void peer_on_connected(void *user, uint32_t w, uint32_t h);
static void peer_on_avc_frame(void *user, uint32_t frame_id, uint16_t codec_id, const struct rdp_avc_stream *avc);
static void peer_on_frame_ack(void *user, uint32_t frame_id, uint32_t qdepth, uint32_t total);
static void peer_on_resize(void *user, uint32_t w, uint32_t h);
static void peer_on_input_key(void *user, uint16_t scancode, uint16_t flags);
static void peer_on_input_unicode(void *user, uint16_t codepoint, uint16_t flags);
static void peer_on_input_mouse(void *user, uint16_t flags, uint16_t x, uint16_t y);
static void peer_on_input_sync(void *user, uint32_t toggle_flags);
static void peer_on_auth(void *user, const char *dom, const char *usr, const char *pwd);
static void peer_on_disconnect(void *user, uint32_t err);
static void pump_loopback(struct rdp_client *c, struct loopback_peer *cp, struct rdp_server *s, struct loopback_peer *sp, int byte_by_byte);
static void test_wire_codecs(void);
static void test_loopback_session(int byte_by_byte);
static void test_auth_rejection(void);

/*
 * Runs all librdp unit and loopback tests.
 */
int
main(
	void)
{
	/* Tests individual wire codecs first. */
	test_wire_codecs();

	/* Tests full client-server roundtrip in bulk and byte-by-byte modes. */
	test_loopback_session(0);
	test_loopback_session(1);

	/* Tests server authentication rejection path. */
	test_auth_rejection();
	printf("librdp-host-test: all tests passed\n");

	/* Succeeded: all tests passed. */
	return 0;
}

/* Appends outgoing bytes to the loopback peer's tx buffer. */
static int
peer_send_bytes(
	void *user,
	const uint8_t *data,
	size_t len)
{
	struct loopback_peer *p;
	int err;

	/* Appends outgoing bytes into the peer's transmit buffer. */
	p = (struct loopback_peer *)user;
	err = rdp_buf_append(&p->tx, data, len);
	if (err != RDP_OK)
		return -1;
	return 0;
}

/* Records TLS requirement notification. */
static void
peer_on_tls(
	void *user)
{
	struct loopback_peer *p;
	p = (struct loopback_peer *)user;
	p->tls_needed = 1;
}

/* Records connection establishment. */
static void
peer_on_connected(
	void *user,
	uint32_t w,
	uint32_t h)
{
	struct loopback_peer *p;
	p = (struct loopback_peer *)user;
	p->connected = 1;
	p->width = w;
	p->height = h;
}

/* Records received AVC420/AVC444 frame metadata. */
static void
peer_on_avc_frame(
	void *user,
	uint32_t frame_id,
	uint16_t codec_id,
	const struct rdp_avc_stream *avc)
{
	struct loopback_peer *p;
	size_t n;
	p = (struct loopback_peer *)user;
	p->frame_count++;
	p->last_frame_id = frame_id;
	p->last_codec_id = codec_id;
	p->last_avc444_mode = avc->avc444_mode;
	p->last_luma_len = avc->luma.data_len;
	p->last_chroma_len = avc->chroma.data_len;
	n = avc->luma.data_len;
	if (n > 8U)
		n = 8U;
	if (n > 0U && avc->luma.data != NULL)
		memcpy(p->last_luma_head, avc->luma.data, n);
}

/* Records received frame acknowledgement. */
static void
peer_on_frame_ack(
	void *user,
	uint32_t frame_id,
	uint32_t qdepth,
	uint32_t total)
{
	struct loopback_peer *p;

	UNUSED_PARAMETER(qdepth);
	UNUSED_PARAMETER(total);
	p = (struct loopback_peer *)user;
	p->ack_count++;
	p->last_ack_id = frame_id;
}

/* Records desktop resize event. */
static void
peer_on_resize(
	void *user,
	uint32_t w,
	uint32_t h)
{
	struct loopback_peer *p;
	p = (struct loopback_peer *)user;
	p->resize_called = 1;
	p->resize_w = w;
	p->resize_h = h;
}

/* Records Fast-Path keyboard scancode event. */
static void
peer_on_input_key(
	void *user,
	uint16_t scancode,
	uint16_t flags)
{
	struct loopback_peer *p;
	p = (struct loopback_peer *)user;
	p->key_count++;
	p->last_scancode = scancode;
	p->last_key_flags = flags;
}

/* Records Fast-Path Unicode event. */
static void
peer_on_input_unicode(
	void *user,
	uint16_t codepoint,
	uint16_t flags)
{
	struct loopback_peer *p;

	UNUSED_PARAMETER(flags);
	p = (struct loopback_peer *)user;
	p->unicode_count++;
	p->last_codepoint = codepoint;
}

/* Records Fast-Path mouse event. */
static void
peer_on_input_mouse(
	void *user,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	struct loopback_peer *p;
	p = (struct loopback_peer *)user;
	p->mouse_count++;
	p->last_mouse_flags = flags;
	p->last_mouse_x = x;
	p->last_mouse_y = y;
}

/* Records Fast-Path sync event. */
static void
peer_on_input_sync(
	void *user,
	uint32_t toggle_flags)
{
	struct loopback_peer *p;
	p = (struct loopback_peer *)user;
	p->sync_count++;
	p->last_sync_flags = toggle_flags;
}

/* Records authentication request and accepts or rejects it. */
static void
peer_on_auth(
	void *user,
	const char *dom,
	const char *usr,
	const char *pwd)
{
	struct loopback_peer *p;
	p = (struct loopback_peer *)user;
	p->auth_called = 1;
	snprintf(p->auth_domain, sizeof(p->auth_domain), "%s", dom);
	snprintf(p->auth_user, sizeof(p->auth_user), "%s", usr);
	snprintf(p->auth_pass, sizeof(p->auth_pass), "%s", pwd);
	if (p->reject_auth != 0)
		assert(rdp_server_reject_auth(p->srv_handle, 0x00000005U) == RDP_OK);
	else
		assert(rdp_server_accept_auth(p->srv_handle) == RDP_OK);
}

/* Records disconnect notification. */
static void
peer_on_disconnect(
	void *user,
	uint32_t err)
{
	struct loopback_peer *p;
	p = (struct loopback_peer *)user;
	p->disconnected = 1;
	p->disconnect_err = err;
}

/* Pumps queued bytes between client and server until both sides are idle. */
static void
pump_loopback(
	struct rdp_client *c,
	struct loopback_peer *cp,
	struct rdp_server *s,
	struct loopback_peer *sp,
	int byte_by_byte)
{
	int progress;
	struct rdp_buf tmp;
	size_t i;
	rdp_buf_init(&tmp);
	do {
		progress = 0;
		if (cp->tx.len > 0U) {
			rdp_buf_clear(&tmp);
			assert(rdp_buf_append(&tmp, cp->tx.data, cp->tx.len) == RDP_OK);
			rdp_buf_clear(&cp->tx);
			if (byte_by_byte != 0) {
				for (i = 0U; i < tmp.len; i++)
					assert(rdp_server_feed(s, &tmp.data[i], 1U) == RDP_OK);
			} else {
				assert(rdp_server_feed(s, tmp.data, tmp.len) == RDP_OK);
			}

			/* Marks forward progress for this pump pass. */
			progress = 1;
		}

		/* Drains server-to-client bytes. */
		if (sp->tx.len > 0U) {
			rdp_buf_clear(&tmp);
			assert(rdp_buf_append(&tmp, sp->tx.data, sp->tx.len) == RDP_OK);
			rdp_buf_clear(&sp->tx);
			if (byte_by_byte != 0) {
				for (i = 0U; i < tmp.len; i++)
					assert(rdp_client_feed(c, &tmp.data[i], 1U) == RDP_OK);
			} else {
				assert(rdp_client_feed(c, tmp.data, tmp.len) == RDP_OK);
			}

			/* Marks forward progress for this pump pass. */
			progress = 1;
		}
	} while (progress != 0);
	rdp_buf_free(&tmp);
}

/* Tests individual wire codecs for roundtrip accuracy. */
static void
test_wire_codecs(
	void)
{
	struct rdp_buf buf;
	struct rdp_packet pkt;
	struct rdp_x224_neg neg;
	const uint8_t *dt_pay;
	size_t dt_len;
	size_t consumed;
	struct rdp_mcs_connect_info mcs_ci;
	struct rdp_sec_info sec_in;
	struct rdp_client_config cfg;
	uint32_t gfx_ver;
	uint32_t disp_w;
	uint32_t disp_h;
	rdp_buf_init(&buf);

	/* Tests X.224 Connection Request roundtrip. */
	assert(rdp_x224_encode_cr(&buf, RDP_NEG_PROTO_SSL) == RDP_OK);
	assert(rdp_stream_next_packet(&buf, &pkt, &consumed) == RDP_OK);
	assert(pkt.is_fastpath == 0);
	assert(rdp_x224_decode(pkt.payload, pkt.payload_len, &neg,
			       &dt_pay, &dt_len) == RDP_OK);
	assert(neg.tpdu_code == 0xE0U);
	assert(neg.protocols == RDP_NEG_PROTO_SSL);

	/* Tests MCS Connect-Initial roundtrip. */
	rdp_buf_clear(&buf);
	assert(rdp_mcs_encode_connect_initial(&buf, 1920U, 1080U, 1) == RDP_OK);
	assert(rdp_mcs_decode_connect_initial(buf.data, buf.len, &mcs_ci) == RDP_OK);
	assert(mcs_ci.width == 1920U);
	assert(mcs_ci.height == 1080U);
	assert(mcs_ci.has_drdynvc == 1);

	/* Tests TS_INFO_PACKET UTF-16LE credential roundtrip. */
	rdp_buf_clear(&buf);
	memset(&cfg, 0, sizeof(cfg));
	cfg.domain = "WORKGROUP";
	cfg.user_name = "alice";
	cfg.password = "s3cr3t";
	assert(rdp_sec_encode_info_pkt(&buf, &cfg) == RDP_OK);
	assert(rdp_sec_decode_info_pkt(buf.data, buf.len, &sec_in) == RDP_OK);
	assert(strcmp(sec_in.domain, "WORKGROUP") == 0);
	assert(strcmp(sec_in.user_name, "alice") == 0);
	assert(strcmp(sec_in.password, "s3cr3t") == 0);

	/* Tests RDPEGFX capability advertise/confirm roundtrip. */
	rdp_buf_clear(&buf);
	assert(rdp_gfx_encode_caps_advertise(&buf) == RDP_OK);
	assert(rdp_gfx_decode_caps_advertise(buf.data + 8U, buf.len - 8U,
					     &gfx_ver) == RDP_OK);
	assert(gfx_ver == RDPGFX_CAPVERSION_106);

	/* Tests RDPEDISP monitor layout roundtrip. */
	rdp_buf_clear(&buf);
	assert(rdp_disp_encode_monitor_layout(&buf, 2560U, 1440U) == RDP_OK);
	assert(rdp_disp_decode_monitor_layout(buf.data, buf.len, &disp_w,
					      &disp_h) == RDP_OK);
	assert(disp_w == 2560U);
	assert(disp_h == 1440U);
	rdp_buf_free(&buf);
}

/* Tests full client-server session lifecycle, AVC420/444 frames, resize, and input. */
static void
test_loopback_session(
	int byte_by_byte)
{
	struct loopback_peer cp;
	struct loopback_peer sp;
	struct rdp_callbacks ccb;
	struct rdp_callbacks scb;
	struct rdp_client *c;
	struct rdp_server *s;
	struct rdp_client_config ccfg;
	struct rdp_server_config scfg;
	uint8_t *big_luma;
	uint8_t chroma_nal[64];
	struct rdp_avc_rect rect;
	struct rdp_avc_quant quant;
	struct rdp_avc_stream avc;
	uint32_t fid;
	size_t i;
	memset(&cp, 0, sizeof(cp));
	memset(&sp, 0, sizeof(sp));
	rdp_buf_init(&cp.tx);
	rdp_buf_init(&sp.tx);
	memset(&ccb, 0, sizeof(ccb));
	ccb.send_bytes = peer_send_bytes;
	ccb.on_tls_required = peer_on_tls;
	ccb.on_connected = peer_on_connected;
	ccb.on_avc_frame = peer_on_avc_frame;
	ccb.on_resize = peer_on_resize;
	ccb.on_disconnect = peer_on_disconnect;
	memset(&scb, 0, sizeof(scb));
	scb.send_bytes = peer_send_bytes;
	scb.on_tls_required = peer_on_tls;
	scb.on_connected = peer_on_connected;
	scb.on_frame_ack = peer_on_frame_ack;
	scb.on_resize = peer_on_resize;
	scb.on_input_key = peer_on_input_key;
	scb.on_input_unicode = peer_on_input_unicode;
	scb.on_input_mouse = peer_on_input_mouse;
	scb.on_input_sync = peer_on_input_sync;
	scb.on_auth_request = peer_on_auth;
	scb.on_disconnect = peer_on_disconnect;
	scfg.width = 1280U;
	scfg.height = 720U;
	s = rdp_server_create(&scb, &sp, &scfg);
	assert(s != NULL);
	sp.srv_handle = s;
	c = rdp_client_create(&ccb, &cp);
	assert(c != NULL);
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.width = 1280U;
	ccfg.height = 720U;
	ccfg.domain = "ZED";
	ccfg.user_name = "awe";
	ccfg.password = "pass123";
	ccfg.client_name = "hosttest";
	assert(rdp_client_connect(c, &ccfg) == RDP_OK);

	/* Pumps X.224 CR/CC negotiation to TLS transition. */
	pump_loopback(c, &cp, s, &sp, byte_by_byte);
	assert(cp.tls_needed == 1);
	assert(sp.tls_needed == 1);
	assert(rdp_client_needs_tls(c) == 1);
	assert(rdp_server_needs_tls(s) == 1);

	/* Simulates TLS handshake completion and pumps MCS/Security/DRDYNVC/GFX. */
	assert(rdp_server_tls_established(s) == RDP_OK);
	assert(rdp_client_tls_established(c) == RDP_OK);
	pump_loopback(c, &cp, s, &sp, byte_by_byte);

	assert(sp.auth_called == 1);
	assert(strcmp(sp.auth_domain, "ZED") == 0);
	assert(strcmp(sp.auth_user, "awe") == 0);
	assert(strcmp(sp.auth_pass, "pass123") == 0);
	assert(cp.connected == 1);
	assert(sp.connected == 1);
	assert(rdp_client_is_connected(c) == 1);
	assert(rdp_server_is_connected(s) == 1);

	/* Sends a large (>1600 B) AVC420 frame to verify VC chunking & ACK. */
	big_luma = (uint8_t *)malloc(4096U);
	assert(big_luma != NULL);
	for (i = 0U; i < 4096U; i++)
		big_luma[i] = (uint8_t)(i & 0xFFU);
	rect.left = 0U;
	rect.top = 0U;
	rect.right = 1280U;
	rect.bottom = 720U;
	quant.qp = 24U;
	quant.quality = 90U;
	memset(&avc, 0, sizeof(avc));
	avc.luma.rect_count = 1U;
	avc.luma.rects = &rect;
	avc.luma.quants = &quant;
	avc.luma.data = big_luma;
	avc.luma.data_len = 4096U;

	assert(rdp_server_send_avc_frame(s, RDP_GFX_CODEC_AVC420, &avc,
					 100U, &fid) == RDP_OK);
	assert(rdp_server_unacked_frames(s) == 1U);
	pump_loopback(c, &cp, s, &sp, byte_by_byte);

	assert(cp.frame_count == 1U);
	assert(cp.last_frame_id == fid);
	assert(cp.last_codec_id == RDP_GFX_CODEC_AVC420);
	assert(cp.last_luma_len == 4096U);
	assert(cp.last_luma_head[0] == 0U && cp.last_luma_head[7] == 7U);

	assert(rdp_client_send_frame_ack(c, fid, 0U) == RDP_OK);
	pump_loopback(c, &cp, s, &sp, byte_by_byte);
	assert(sp.ack_count == 1U);
	assert(sp.last_ack_id == fid);
	assert(rdp_server_unacked_frames(s) == 0U);

	/* Sends an AVC444 (luma + chroma) frame and verifies both layers. */
	memset(chroma_nal, 0x5AU, sizeof(chroma_nal));
	avc.avc444_mode = RDP_AVC444_MODE_LUMA_CHROMA;
	avc.chroma.rect_count = 1U;
	avc.chroma.rects = &rect;
	avc.chroma.quants = &quant;
	avc.chroma.data = chroma_nal;
	avc.chroma.data_len = sizeof(chroma_nal);
	assert(rdp_server_send_avc_frame(s, RDP_GFX_CODEC_AVC444, &avc,
					 116U, &fid) == RDP_OK);
	pump_loopback(c, &cp, s, &sp, byte_by_byte);
	assert(cp.frame_count == 2U);
	assert(cp.last_codec_id == RDP_GFX_CODEC_AVC444);
	assert(cp.last_avc444_mode == RDP_AVC444_MODE_LUMA_CHROMA);
	assert(cp.last_luma_len == 4096U);
	assert(cp.last_chroma_len == sizeof(chroma_nal));

	/* Tests client resize request -> server resize -> client notification. */
	assert(rdp_client_request_resize(c, 1920U, 1080U) == RDP_OK);
	pump_loopback(c, &cp, s, &sp, byte_by_byte);
	assert(sp.resize_called == 1);
	assert(sp.resize_w == 1920U && sp.resize_h == 1080U);

	assert(rdp_server_resize(s, 1920U, 1080U) == RDP_OK);
	pump_loopback(c, &cp, s, &sp, byte_by_byte);
	assert(cp.resize_called == 1);
	assert(cp.resize_w == 1920U && cp.resize_h == 1080U);

	/* Tests Fast-Path keyboard, Unicode, mouse, and sync input PDUs. */
	assert(rdp_client_send_key(c, 0x1EU, RDP_KBD_FLAG_EXTENDED) == RDP_OK);
	assert(rdp_client_send_unicode(c, 0x3042U, 0U) == RDP_OK);
	assert(rdp_client_send_mouse(c, RDP_PTR_FLAG_MOVE | RDP_PTR_FLAG_BUTTON1 |
				     RDP_PTR_FLAG_DOWN, 640U, 360U) == RDP_OK);
	assert(rdp_client_send_sync(c, RDP_SYNC_CAPS_LOCK | RDP_SYNC_NUM_LOCK) == RDP_OK);
	pump_loopback(c, &cp, s, &sp, byte_by_byte);

	assert(sp.key_count == 1U && sp.last_scancode == 0x1EU);
	assert((sp.last_key_flags & RDP_KBD_FLAG_EXTENDED) != 0U);
	assert(sp.unicode_count == 1U && sp.last_codepoint == 0x3042U);
	assert(sp.mouse_count == 1U && sp.last_mouse_x == 640U &&
	       sp.last_mouse_y == 360U);
	assert(sp.sync_count == 1U &&
	       sp.last_sync_flags == (RDP_SYNC_CAPS_LOCK | RDP_SYNC_NUM_LOCK));
	free(big_luma);
	rdp_client_destroy(c);
	rdp_server_destroy(s);
	rdp_buf_free(&cp.tx);
	rdp_buf_free(&sp.tx);
}

/* Tests server auth rejection propagating SetErrorInfoPDU to client. */
static void
test_auth_rejection(
	void)
{
	struct loopback_peer cp;
	struct loopback_peer sp;
	struct rdp_callbacks ccb;
	struct rdp_callbacks scb;
	struct rdp_client *c;
	struct rdp_server *s;
	struct rdp_client_config ccfg;
	memset(&cp, 0, sizeof(cp));
	memset(&sp, 0, sizeof(sp));
	sp.reject_auth = 1;
	rdp_buf_init(&cp.tx);
	rdp_buf_init(&sp.tx);
	memset(&ccb, 0, sizeof(ccb));
	ccb.send_bytes = peer_send_bytes;
	ccb.on_disconnect = peer_on_disconnect;
	memset(&scb, 0, sizeof(scb));
	scb.send_bytes = peer_send_bytes;
	scb.on_auth_request = peer_on_auth;
	scb.on_disconnect = peer_on_disconnect;
	s = rdp_server_create(&scb, &sp, NULL);
	assert(s != NULL);
	sp.srv_handle = s;
	c = rdp_client_create(&ccb, &cp);
	assert(c != NULL);
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.user_name = "baduser";
	ccfg.password = "wrong";
	assert(rdp_client_connect(c, &ccfg) == RDP_OK);
	pump_loopback(c, &cp, s, &sp, 0);

	assert(rdp_server_tls_established(s) == RDP_OK);
	assert(rdp_client_tls_established(c) == RDP_OK);
	pump_loopback(c, &cp, s, &sp, 0);

	assert(sp.disconnected == 1);
	assert(cp.disconnected == 1);
	assert(cp.disconnect_err == 0x00000005U);
	rdp_client_destroy(c);
	rdp_server_destroy(s);
	rdp_buf_free(&cp.tx);
	rdp_buf_free(&sp.tx);
}
