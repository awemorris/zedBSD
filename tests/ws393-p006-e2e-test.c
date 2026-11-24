/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * End-to-end TCP loopback integration test:
 *   client (librdp + libssl-compat TLS client + h264sw decoder)
 *   <->
 *   server (libssl-compat TLS server + librdp + h264sw-enc + rdpd HID)
 */

#include "userland/desktop/libmedia/media-decoder.h"
#include "userland/desktop/libmedia/media-encoder.h"
#include "userland/desktop/librdp/rdp.h"
#include "userland/desktop/rdpd/rdpd.h"

#include <compat/ssl/ssl.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct e2e_srv_state {
	int listen_fd;
	int client_fd;
	uint16_t port;
	SSL_CTX *ssl_ctx;
	SSL *ssl;
	int tls_active;
	int running;
	int authed;
	int connected;
	int sent_640;
	int got_ack;
	int got_key;
	int got_mouse;
	int got_resize;
	int sent_800;
	uint32_t width;
	uint32_t height;
	uint32_t next_ts_ms;
	struct rdp_server *srv;
	struct media_encoder *enc;
	struct rdpd_hid hid;
	uint8_t last_krep[9];
	uint8_t last_mrep[7];
};

struct e2e_cli_state {
	int sock;
	SSL_CTX *ssl_ctx;
	SSL *ssl;
	int tls_active;
	int running;
	int connected;
	int decoded_640;
	int decoded_800;
	int got_resize;
	uint32_t width;
	uint32_t height;
	uint32_t next_ts_ms;
	struct rdp_client *cli;
	struct media_decoder *dec;
};

static int
ssl_send_all(
	int sock,
	SSL *ssl,
	int tls_active,
	const uint8_t *data,
	size_t len)
{
	size_t off;

	off = 0U;
	while (off < len) {
		ssize_t n;

		if (tls_active && ssl != NULL)
			n = SSL_write(ssl, data + off, (int)(len - off));
		else
			n = send(sock, data + off, len - off, 0);
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

static struct media_encoder *
open_h264_encoder(
	uint32_t width,
	uint32_t height)
{
	struct media_encoder_config cfg;
	struct media_encoder *enc;

	memset(&cfg, 0, sizeof(cfg));
	cfg.codec = MEDIA_CODEC_H264;
	cfg.profile = MEDIA_ENCODER_PROFILE_H264_CONSTRAINED_BASELINE;
	cfg.width = width;
	cfg.height = height;
	cfg.rate_num = 30U;
	cfg.rate_den = 1U;
	cfg.input = MEDIA_ENCODER_INPUT_I420;
	cfg.allow = MEDIA_ENCODER_SOFTWARE;
	cfg.purpose = MEDIA_ENCODER_LOW_LATENCY;
	cfg.output = MEDIA_ENCODER_ANNEX_B;
	cfg.rate_control = MEDIA_ENCODER_RATE_CQP;
	cfg.qp = 26U;
	enc = NULL;
	assert(media_encoder_create(&cfg, &enc) == 0);
	assert(enc != NULL);
	return enc;
}

static int
send_encoded_frame(
	struct e2e_srv_state *s,
	uint8_t luma_val,
	uint32_t ts_ms)
{
	uint8_t *y_plane;
	uint8_t *u_plane;
	uint8_t *v_plane;
	struct media_encoder_picture pic;
	struct media_encoded pkt;
	struct rdp_avc_rect rect;
	struct rdp_avc_quant quant;
	struct rdp_avc_stream avc;
	size_t y_sz;
	size_t uv_sz;
	int rc;

	y_sz = (size_t)s->width * (size_t)s->height;
	uv_sz = (size_t)(s->width / 2U) * (size_t)(s->height / 2U);
	y_plane = (uint8_t *)malloc(y_sz);
	u_plane = (uint8_t *)malloc(uv_sz);
	v_plane = (uint8_t *)malloc(uv_sz);
	assert(y_plane != NULL && u_plane != NULL && v_plane != NULL);
	memset(y_plane, luma_val, y_sz);
	memset(u_plane, 128U, uv_sz);
	memset(v_plane, 128U, uv_sz);

	memset(&pic, 0, sizeof(pic));
	pic.plane[0] = y_plane;
	pic.plane[1] = u_plane;
	pic.plane[2] = v_plane;
	pic.stride[0] = s->width;
	pic.stride[1] = s->width / 2U;
	pic.stride[2] = s->width / 2U;
	assert(media_encoder_send(s->enc, &pic, (int64_t)ts_ms * 1000,
				  MEDIA_ENCODE_FORCE_KEY) == 0);

	memset(&pkt, 0, sizeof(pkt));
	assert(media_encoder_receive(s->enc, 0U, &pkt) == 0);
	assert(pkt.data != NULL && pkt.size > 4U);

	rect.left = 0U;
	rect.top = 0U;
	rect.right = s->width;
	rect.bottom = s->height;
	quant.qp = 26U;
	quant.quality = 100U;
	memset(&avc, 0, sizeof(avc));
	avc.luma.rect_count = 1U;
	avc.luma.rects = &rect;
	avc.luma.quants = &quant;
	avc.luma.data = pkt.data;
	avc.luma.data_len = pkt.size;
	rc = rdp_server_send_avc_frame(s->srv, RDP_GFX_CODEC_AVC420,
				       &avc, ts_ms, NULL);
	if (rc != 0)
		assert(rc == RDP_ERR_STATE);

	free(y_plane);
	free(u_plane);
	free(v_plane);
	return rc == 0 ? 0 : 1;
}

static int
srv_send_bytes(
	void *user,
	const uint8_t *data,
	size_t len)
{
	struct e2e_srv_state *s;

	s = (struct e2e_srv_state *)user;
	return ssl_send_all(s->client_fd, s->ssl, s->tls_active, data, len);
}

static void
srv_on_auth(
	void *user,
	const char *domain,
	const char *user_name,
	const char *password)
{
	struct e2e_srv_state *s;

	(void)domain;
	s = (struct e2e_srv_state *)user;
	assert(strcmp(user_name, "alice") == 0);
	assert(strcmp(password, "secret") == 0);
	s->authed = 1;
	assert(rdp_server_accept_auth(s->srv) == 0);
}

static void
srv_on_connected(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct e2e_srv_state *s;

	s = (struct e2e_srv_state *)user;
	s->width = width;
	s->height = height;
	s->hid.width = width;
	s->hid.height = height;
	s->enc = open_h264_encoder(width, height);
	s->connected = 1;
}

static void
srv_on_frame_ack(
	void *user,
	uint32_t frame_id,
	uint32_t queue_depth,
	uint32_t total_decoded)
{
	struct e2e_srv_state *s;

	(void)frame_id;
	(void)queue_depth;
	s = (struct e2e_srv_state *)user;
	if (total_decoded >= 1U)
		s->got_ack = 1;
}

static void
srv_on_input_key(
	void *user,
	uint16_t scancode,
	uint16_t flags)
{
	struct e2e_srv_state *s;

	s = (struct e2e_srv_state *)user;
	assert(rdpd_hid_format_key(&s->hid, scancode, flags, s->last_krep) == 0);
	s->got_key = 1;
}

static void
srv_on_input_mouse(
	void *user,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	struct e2e_srv_state *s;

	s = (struct e2e_srv_state *)user;
	assert(rdpd_hid_format_mouse(&s->hid, flags, x, y, s->last_mrep) == 0);
	s->got_mouse = 1;
}

static void
srv_on_resize(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct e2e_srv_state *s;

	s = (struct e2e_srv_state *)user;
	s->width = width;
	s->height = height;
	rdpd_hid_set_size(&s->hid, width, height);
	media_encoder_close(s->enc);
	s->enc = open_h264_encoder(width, height);
	assert(rdp_server_resize(s->srv, width, height) == 0);
	s->got_resize = 1;
}

static void
srv_on_disconnect(
	void *user,
	uint32_t error_info)
{
	struct e2e_srv_state *s;

	(void)error_info;
	s = (struct e2e_srv_state *)user;
	s->running = 0;
}

static void *
srv_thread_main(
	void *arg)
{
	struct e2e_srv_state *s;
	struct rdp_callbacks cb;
	struct rdp_server_config cfg;
	uint8_t buf[16384];

	s = (struct e2e_srv_state *)arg;
	s->client_fd = accept(s->listen_fd, NULL, NULL);
	assert(s->client_fd >= 0);

	memset(&cb, 0, sizeof(cb));
	cb.send_bytes = srv_send_bytes;
	cb.on_auth_request = srv_on_auth;
	cb.on_connected = srv_on_connected;
	cb.on_frame_ack = srv_on_frame_ack;
	cb.on_input_key = srv_on_input_key;
	cb.on_input_mouse = srv_on_input_mouse;
	cb.on_resize = srv_on_resize;
	cb.on_disconnect = srv_on_disconnect;

	memset(&cfg, 0, sizeof(cfg));
	cfg.width = 640U;
	cfg.height = 480U;
	s->srv = rdp_server_create(&cb, s, &cfg);
	assert(s->srv != NULL);
	s->running = 1;

	while (s->running) {
		struct pollfd pfd;
		int n;

		if (rdp_server_needs_tls(s->srv) && !s->tls_active) {
			s->ssl = SSL_new(s->ssl_ctx);
			assert(s->ssl != NULL);
			assert(SSL_set_fd(s->ssl, s->client_fd) == 1);
			assert(SSL_accept(s->ssl) == 1);
			s->tls_active = 1;
			assert(rdp_server_tls_established(s->srv) == 0);
		}

		if (s->connected && !s->sent_640 &&
		    rdp_server_is_connected(s->srv)) {
			s->next_ts_ms += 33U;
			if (send_encoded_frame(s, 96U, s->next_ts_ms) == 0)
				s->sent_640 = 1;
		}
		if (s->got_resize && !s->sent_800 &&
		    rdp_server_is_connected(s->srv)) {
			s->next_ts_ms += 33U;
			if (send_encoded_frame(s, 160U, s->next_ts_ms) == 0)
				s->sent_800 = 1;
		}

		pfd.fd = s->client_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 100) <= 0)
			continue;

		if (s->tls_active)
			n = SSL_read(s->ssl, buf, sizeof(buf));
		else
			n = (int)recv(s->client_fd, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		if (rdp_server_feed(s->srv, buf, (size_t)n) != 0)
			break;
	}

	if (s->enc != NULL)
		media_encoder_close(s->enc);
	rdp_server_destroy(s->srv);
	if (s->ssl != NULL) {
		(void)SSL_shutdown(s->ssl);
		SSL_free(s->ssl);
	}
	close(s->client_fd);
	return NULL;
}

static int
cli_send_bytes(
	void *user,
	const uint8_t *data,
	size_t len)
{
	struct e2e_cli_state *c;

	c = (struct e2e_cli_state *)user;
	return ssl_send_all(c->sock, c->ssl, c->tls_active, data, len);
}

static void
cli_on_connected(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct e2e_cli_state *c;

	c = (struct e2e_cli_state *)user;
	c->width = width;
	c->height = height;
	c->connected = 1;
}

static void
cli_on_avc_frame(
	void *user,
	uint32_t frame_id,
	uint16_t codec_id,
	const struct rdp_avc_stream *avc)
{
	struct e2e_cli_state *c;
	struct media_track track;
	struct media_packet mpkt;
	struct media_frame *frame;
	struct media_scaler *scaler;
	uint32_t *rgba;
	int64_t pts;

	(void)codec_id;
	c = (struct e2e_cli_state *)user;
	if (c->dec == NULL) {
		memset(&track, 0, sizeof(track));
		track.kind = MEDIA_TRACK_VIDEO;
		track.codec = MEDIA_CODEC_H264;
		track.width = c->width;
		track.height = c->height;
		assert(media_decoder_open(&track, &c->dec) == 0);
	}

	memset(&mpkt, 0, sizeof(mpkt));
	mpkt.data = (void *)avc->luma.data;
	mpkt.size = avc->luma.data_len;
	mpkt.pts_us = 0;
	mpkt.keyframe = 1;
	assert(media_decoder_send(c->dec, &mpkt) == 0);
	if (media_decoder_receive(c->dec, &pts) != 1) {
		assert(media_decoder_send(c->dec, NULL) == 0);
		assert(media_decoder_receive(c->dec, &pts) == 1);
	}
	frame = media_decoder_picture(c->dec);
	assert(frame != NULL);

	rgba = (uint32_t *)malloc((size_t)c->width * (size_t)c->height * 4U);
	assert(rgba != NULL);
	scaler = NULL;
	assert(media_frame_scale(frame, &scaler, rgba, c->width * 4U,
				 (int)c->width, (int)c->height) == 0);
	media_scaler_free(scaler);
	free(rgba);
	media_frame_free(&frame);

	assert(rdp_client_send_frame_ack(c->cli, frame_id, 0U) == 0);
	if (c->width == 640U && c->height == 480U)
		c->decoded_640 = 1;
	else if (c->width == 800U && c->height == 600U)
		c->decoded_800 = 1;
}

static void
cli_on_resize(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct e2e_cli_state *c;

	c = (struct e2e_cli_state *)user;
	c->width = width;
	c->height = height;
	c->got_resize = 1;
	if (c->dec != NULL) {
		media_decoder_close(c->dec);
		c->dec = NULL;
	}
}

static void
cli_on_disconnect(
	void *user,
	uint32_t error_info)
{
	struct e2e_cli_state *c;

	(void)error_info;
	c = (struct e2e_cli_state *)user;
	c->running = 0;
}

int
main(
	void)
{
	struct e2e_srv_state srv;
	struct e2e_cli_state cli;
	struct sockaddr_in addr;
	socklen_t alen;
	pthread_t srv_thr;
	struct rdp_callbacks cb;
	struct rdp_client_config ccfg;
	uint8_t buf[16384];
	int opt;
	int sent_input;
	int sent_resize;

	memset(&srv, 0, sizeof(srv));
	srv.hid.fd = -1;
	srv.ssl_ctx = SSL_CTX_new(TLS_server_method());
	assert(srv.ssl_ctx != NULL);
	assert(SSL_CTX_generate_self_signed(srv.ssl_ctx, "zedbsd-rdp-e2e") == 1);

	srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(srv.listen_fd >= 0);
	opt = 1;
	(void)setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	assert(bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(srv.listen_fd, 1) == 0);
	alen = sizeof(addr);
	assert(getsockname(srv.listen_fd, (struct sockaddr *)&addr, &alen) == 0);
	srv.port = ntohs(addr.sin_port);

	assert(pthread_create(&srv_thr, NULL, srv_thread_main, &srv) == 0);

	memset(&cli, 0, sizeof(cli));
	cli.sock = socket(AF_INET, SOCK_STREAM, 0);
	assert(cli.sock >= 0);
	assert(connect(cli.sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);

	cli.ssl_ctx = SSL_CTX_new(TLS_client_method());
	assert(cli.ssl_ctx != NULL);
	SSL_CTX_set_verify(cli.ssl_ctx, SSL_VERIFY_NONE, NULL);

	memset(&cb, 0, sizeof(cb));
	cb.send_bytes = cli_send_bytes;
	cb.on_connected = cli_on_connected;
	cb.on_avc_frame = cli_on_avc_frame;
	cb.on_resize = cli_on_resize;
	cb.on_disconnect = cli_on_disconnect;

	cli.cli = rdp_client_create(&cb, &cli);
	assert(cli.cli != NULL);
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.width = 640U;
	ccfg.height = 480U;
	ccfg.user_name = "alice";
	ccfg.password = "secret";
	ccfg.client_name = "e2e-client";
	assert(rdp_client_connect(cli.cli, &ccfg) == 0);

	cli.running = 1;
	sent_input = 0;
	sent_resize = 0;
	while (cli.running) {
		struct pollfd pfd;
		int n;

		if (rdp_client_needs_tls(cli.cli) && !cli.tls_active) {
			cli.ssl = SSL_new(cli.ssl_ctx);
			assert(cli.ssl != NULL);
			assert(SSL_set_fd(cli.ssl, cli.sock) == 1);
			assert(SSL_connect(cli.ssl) == 1);
			cli.tls_active = 1;
			assert(rdp_client_tls_established(cli.cli) == 0);
		}

		if (cli.decoded_640 && !sent_input) {
			assert(rdp_client_send_key(cli.cli, 0x1EU, 0U) == 0);
			assert(rdp_client_send_mouse(
				cli.cli,
				RDP_PTR_FLAG_BUTTON1 | RDP_PTR_FLAG_DOWN,
				320U, 240U) == 0);
			sent_input = 1;
		}
		if (sent_input && !sent_resize) {
			assert(rdp_client_request_resize(cli.cli, 800U, 600U) == 0);
			sent_resize = 1;
		}
		if (cli.decoded_800 && srv.got_ack && srv.got_key &&
		    srv.got_mouse && srv.got_resize) {
			(void)rdp_client_disconnect(cli.cli);
			break;
		}

		pfd.fd = cli.sock;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 100) <= 0)
			continue;

		if (cli.tls_active)
			n = SSL_read(cli.ssl, buf, sizeof(buf));
		else
			n = (int)recv(cli.sock, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		assert(rdp_client_feed(cli.cli, buf, (size_t)n) == 0);
	}

	if (cli.ssl != NULL) {
		(void)SSL_shutdown(cli.ssl);
		SSL_free(cli.ssl);
	}
	close(cli.sock);
	assert(pthread_join(srv_thr, NULL) == 0);

	assert(srv.authed == 1);
	assert(srv.connected == 1);
	assert(srv.got_ack == 1);
	assert(srv.got_key == 1);
	assert(srv.got_mouse == 1);
	assert(srv.last_krep[3] == 0x04U);
	assert(srv.last_mrep[1] == 0x01U);
	assert(srv.got_resize == 1);
	assert(cli.connected == 1);
	assert(cli.decoded_640 == 1);
	assert(cli.got_resize == 1);
	assert(cli.decoded_800 == 1);

	if (cli.dec != NULL)
		media_decoder_close(cli.dec);
	rdp_client_destroy(cli.cli);
	SSL_CTX_free(cli.ssl_ctx);
	SSL_CTX_free(srv.ssl_ctx);
	close(srv.listen_fd);

	printf("ws393-p006-e2e-test: all E2E TCP loopback tests passed\n");
	return 0;
}
