/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit tests for h264sw-enc -> h264sw decode roundtrip, libssl-compat
 * TLS 1.3 server <-> client handshake, rdpd HID report conversion, and
 * wayland headless IPC framing.
 */

#include "userland/desktop/libmedia/media-decoder.h"
#include "userland/desktop/libmedia/media-encoder.h"
#include "userland/desktop/librdp/rdp.h"
#include "userland/desktop/rdpd/rdpd.h"
#include "userland/desktop/wayland/rdp-headless.h"

#include <compat/ssl/ssl.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_h264sw_enc_roundtrip(void);
static void *tls_server_thread(void *arg);
static void test_libssl_server_client(void);
static void test_rdpd_hid(void);
static void test_wayland_ipc(void);

struct tls_srv_arg {
	int fd;
	SSL_CTX *ctx;
	int ok;
};

/*
 * Runs p005 host unit tests.
 */
int
main(
	void)
{
	test_h264sw_enc_roundtrip();
	test_libssl_server_client();
	test_rdpd_hid();
	test_wayland_ipc();

	printf("ws393-p005-host-test: all tests passed\n");
	return 0;
}

/* Tests h264sw-enc encoding I420 frames and decoding them with h264sw. */
static void
test_h264sw_enc_roundtrip(
	void)
{
	struct media_encoder_config cfg;
	struct media_encoder *enc;
	struct media_encoder_picture pic;
	struct media_encoded pkt;
	struct media_track track;
	struct media_decoder *dec;
	struct media_packet mpkt;
	struct media_frame *frame;
	struct media_scaler *scaler;
	uint8_t y_plane[64 * 64];
	uint8_t u_plane[32 * 32];
	uint8_t v_plane[32 * 32];
	uint32_t rgba[64 * 64];
	int64_t pts;
	int f;

	memset(&cfg, 0, sizeof(cfg));
	cfg.codec = MEDIA_CODEC_H264;
	cfg.profile = MEDIA_ENCODER_PROFILE_H264_CONSTRAINED_BASELINE;
	cfg.width = 64U;
	cfg.height = 64U;
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

	memset(&track, 0, sizeof(track));
	track.kind = MEDIA_TRACK_VIDEO;
	track.codec = MEDIA_CODEC_H264;
	track.width = 64U;
	track.height = 64U;
	dec = NULL;
	assert(media_decoder_open(&track, &dec) == 0);
	assert(dec != NULL);

	scaler = NULL;
	for (f = 0; f < 3; f++) {
		memset(y_plane, (uint8_t)(80 + f * 30), sizeof(y_plane));
		memset(u_plane, 128U, sizeof(u_plane));
		memset(v_plane, 128U, sizeof(v_plane));

		memset(&pic, 0, sizeof(pic));
		pic.plane[0] = y_plane;
		pic.plane[1] = u_plane;
		pic.plane[2] = v_plane;
		pic.stride[0] = 64U;
		pic.stride[1] = 32U;
		pic.stride[2] = 32U;

		assert(media_encoder_send(enc, &pic, (int64_t)f * 33333, 0U) == 0);
		memset(&pkt, 0, sizeof(pkt));
		assert(media_encoder_receive(enc, 0U, &pkt) == 0);
		assert(pkt.data != NULL && pkt.size > 4U);
		if (f == 0)
			assert((pkt.flags & MEDIA_ENCODED_KEY) != 0U);

		memset(&mpkt, 0, sizeof(mpkt));
		mpkt.data = pkt.data;
		mpkt.size = pkt.size;
		mpkt.pts_us = (int64_t)f * 33333;
		mpkt.keyframe = (pkt.flags & MEDIA_ENCODED_KEY) != 0U;
		assert(media_decoder_send(dec, &mpkt) == 0);
		if (media_decoder_receive(dec, &pts) == 1) {
			frame = media_decoder_picture(dec);
			assert(frame != NULL);
			assert(media_frame_scale(frame, &scaler, rgba, 64U * 4U, 64, 64) == 0);
			media_frame_free(&frame);
		}
	}

	assert(media_decoder_send(dec, NULL) == 0);
	pts = 0;
	if (scaler == NULL) {
		int rc = media_decoder_receive(dec, &pts);
		if (rc != 1)
			fprintf(stderr, "media_decoder_receive rc=%d\n", rc);
		assert(rc == 1);
		frame = media_decoder_picture(dec);
		assert(frame != NULL);
		assert(media_frame_scale(frame, &scaler, rgba, 64U * 4U, 64, 64) == 0);
		media_frame_free(&frame);
	}

	media_scaler_free(scaler);
	media_decoder_close(dec);
	media_encoder_close(enc);
}

/* Server thread running SSL_accept and echoing one TLS application message. */
static void *
tls_server_thread(
	void *arg)
{
	struct tls_srv_arg *a;
	SSL *ssl;
	char buf[32];
	int n;

	a = (struct tls_srv_arg *)arg;
	ssl = SSL_new(a->ctx);
	assert(ssl != NULL);
	assert(SSL_set_fd(ssl, a->fd) == 1);
	int rc = SSL_accept(ssl);
	if (rc != 1)
		fprintf(stderr, "SSL_accept rc=%d err=%d errno=%d errcode=0x%lx\n", rc, SSL_get_error(ssl, rc), errno, (unsigned long)ERR_get_error());
	assert(rc == 1);

	memset(buf, 0, sizeof(buf));
	n = SSL_read(ssl, buf, sizeof(buf));
	assert(n == 5);
	assert(memcmp(buf, "hello", 5U) == 0);
	assert(SSL_write(ssl, "world", 5) == 5);

	(void)SSL_shutdown(ssl);
	SSL_free(ssl);
	a->ok = 1;
	return NULL;
}

/* Tests libssl-compat TLS_server_method <-> TLS_client_method over socketpair. */
static void
test_libssl_server_client(
	void)
{
	int sv[2];
	SSL_CTX *sctx;
	SSL_CTX *cctx;
	SSL *cssl;
	struct tls_srv_arg sarg;
	pthread_t thr;
	char buf[32];
	int n;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

	sctx = SSL_CTX_new(TLS_server_method());
	assert(sctx != NULL);
	assert(SSL_CTX_generate_self_signed(sctx, "zedbsd-rdp") == 1);

	sarg.fd = sv[0];
	sarg.ctx = sctx;
	sarg.ok = 0;
	assert(pthread_create(&thr, NULL, tls_server_thread, &sarg) == 0);

	cctx = SSL_CTX_new(TLS_client_method());
	assert(cctx != NULL);
	SSL_CTX_set_verify(cctx, SSL_VERIFY_NONE, NULL);
	cssl = SSL_new(cctx);
	assert(cssl != NULL);
	assert(SSL_set_fd(cssl, sv[1]) == 1);
	int rc = SSL_connect(cssl);
	if (rc != 1)
		fprintf(stderr, "SSL_connect rc=%d err=%d\n", rc, SSL_get_error(cssl, rc));
	assert(rc == 1);

	assert(SSL_write(cssl, "hello", 5) == 5);
	memset(buf, 0, sizeof(buf));
	n = SSL_read(cssl, buf, sizeof(buf));
	assert(n == 5);
	assert(memcmp(buf, "world", 5U) == 0);

	(void)SSL_shutdown(cssl);
	SSL_free(cssl);
	SSL_CTX_free(cctx);

	assert(pthread_join(thr, NULL) == 0);
	assert(sarg.ok == 1);

	assert(SSL_CTX_free != NULL);
	SSL_CTX_free(sctx);
	close(sv[0]);
	close(sv[1]);
}

/* Tests rdpd HID descriptor, PS/2->USB HID key mapping, and mouse scaling. */
static void
test_rdpd_hid(
	void)
{
	uint8_t desc[256];
	size_t dlen;
	struct rdpd_hid hid;
	uint8_t krep[9];
	uint8_t mrep[7];
	uint16_t sx;
	uint16_t sy;

	dlen = rdpd_hid_descriptor(desc, sizeof(desc));
	assert(dlen > 40U && dlen < sizeof(desc));
	assert(rdpd_hid_descriptor(desc, 10U) == 0U);

	assert(rdpd_ps2_to_hid_usage(0x1EU, 0U) == 0x04U);
	assert(rdpd_ps2_to_hid_usage(0x48U, RDP_KBD_FLAG_EXTENDED) == 0x52U);

	memset(&hid, 0, sizeof(hid));
	hid.fd = -1;
	hid.width = 1280U;
	hid.height = 720U;

	assert(rdpd_hid_format_key(&hid, 0x1DU, 0U, krep) == 0);
	assert(krep[0] == 0x01U && krep[1] == 0x01U);
	assert(rdpd_hid_format_key(&hid, 0x1EU, 0U, krep) == 0);
	assert(krep[0] == 0x01U && krep[1] == 0x01U && krep[3] == 0x04U);
	assert(rdpd_hid_format_key(&hid, 0x1EU, RDP_KBD_FLAG_RELEASE, krep) == 0);
	assert(krep[3] == 0x00U);
	assert(rdpd_hid_format_key(&hid, 0x1DU, RDP_KBD_FLAG_RELEASE, krep) == 0);
	assert(krep[1] == 0x00U);

	assert(rdpd_hid_format_mouse(&hid, RDP_PTR_FLAG_BUTTON1 | RDP_PTR_FLAG_DOWN,
				     1279U, 719U, mrep) == 0);
	sx = (uint16_t)mrep[2] | ((uint16_t)mrep[3] << 8);
	sy = (uint16_t)mrep[4] | ((uint16_t)mrep[5] << 8);
	assert(mrep[0] == 0x02U && mrep[1] == 0x01U);
	assert(sx == 32767U && sy == 32767U && mrep[6] == 0U);

	assert(rdpd_hid_format_mouse(&hid,
				     RDP_PTR_FLAG_BUTTON1 | RDP_PTR_FLAG_WHEEL | RDP_PTR_FLAG_WHEEL_NEGATIVE,
				     0U, 0U, mrep) == 0);
	sx = (uint16_t)mrep[2] | ((uint16_t)mrep[3] << 8);
	sy = (uint16_t)mrep[4] | ((uint16_t)mrep[5] << 8);
	assert(mrep[1] == 0x00U && sx == 0U && sy == 0U && (int8_t)mrep[6] == -1);
}

/* Tests wayland headless RDP IPC framing and dimension clamping. */
static void
test_wayland_ipc(
	void)
{
	uint8_t ctrl[KWL_RDP_CTRL_HDR_SIZE];
	uint8_t fhdr[KWL_RDP_FRAME_HDR_SIZE];
	uint32_t type;
	uint32_t a1;
	uint32_t a2;
	uint32_t flags;
	uint64_t pts;
	uint32_t sz;
	uint32_t w;
	uint32_t h;

	kwl_rdp_pack_ctrl(ctrl, KWL_RDP_MSG_RESIZE, 1920U, 1080U);
	kwl_rdp_unpack_ctrl(ctrl, &type, &a1, &a2);
	assert(type == KWL_RDP_MSG_RESIZE && a1 == 1920U && a2 == 1080U);

	kwl_rdp_pack_frame_hdr(fhdr, KWL_RDP_FRAME_KEY, 123456789ULL, 4096U);
	kwl_rdp_unpack_frame_hdr(fhdr, &flags, &pts, &sz);
	assert(flags == KWL_RDP_FRAME_KEY && pts == 123456789ULL && sz == 4096U);

	w = 101U;
	h = 9999U;
	kwl_rdp_clamp_size(&w, &h);
	assert(w == KWL_RDP_MIN_WIDTH && h == KWL_RDP_MAX_HEIGHT);
	w = 1281U;
	h = 721U;
	kwl_rdp_clamp_size(&w, &h);
	assert(w == 1280U && h == 720U);
}
