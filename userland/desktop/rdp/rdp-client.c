/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Native zedBSD RDP session thread: TCP connect via libkeiland, TLS via
 * libssl-compat, librdp protocol state machine, and H.264 decoding via
 * libmedia's media_decoder (vkvideo or h264sw fallback).
 */

#include "rdp-client.h"

#include "userland/desktop/libmedia/media-decoder.h"
#include "userland/desktop/librdp/rdp.h"

#include <compat/ssl/ssl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED_PARAMETER(x)	((void)(x))

/* Maximum number of queued UI input events waiting for the RDP thread. */
#define RDP_SESSION_INPUTS	128U

/* Queued input kinds. */
#define RDP_IN_KEY		1U
#define RDP_IN_UNICODE		2U
#define RDP_IN_MOUSE		3U

/*
 * One queued user input event for the RDP worker thread.
 */
struct rdp_in_event {
	unsigned kind;
	uint16_t code;
	uint16_t flags;
	uint16_t x;
	uint16_t y;
};

/*
 * Shared state between the main libkeiland UI thread and the RDP thread.
 */
struct rdp_app_session {
	struct kl_app *app;
	struct kl_window *window;
	struct rdp_app_target target;
	struct kl_mutex lock;
	struct kl_thread *thread;
	struct kl_socket_waker *waker;
	struct kl_socket *sock;
	SSL_CTX *ssl_ctx;
	SSL *ssl;
	int tls_active;
	struct rdp_client *client;
	struct media_decoder *decoder;
	struct media_scaler *scaler;
	uint32_t width;
	uint32_t height;
	uint32_t *fb;
	uint32_t fb_w;
	uint32_t fb_h;
	uint32_t *scratch;
	size_t scratch_cap;
	int stop;
	int disconnected;
	struct rdp_in_event inputs[RDP_SESSION_INPUTS];
	unsigned input_count;
	uint32_t wanted_w;
	uint32_t wanted_h;
	int wanted_resize;
};

static void rdp_thread_entry(void *arg);
static int session_connect_tcp(struct rdp_app_session *s);
static int session_send_bytes(void *user, const uint8_t *data, size_t len);
static void session_on_tls(void *user);
static void session_on_connected(void *user, uint32_t width, uint32_t height);
static void session_on_avc_frame(void *user, uint32_t frame_id, uint16_t codec_id, const struct rdp_avc_stream *avc);
static void session_on_resize(void *user, uint32_t width, uint32_t height);
static void session_on_disconnect(void *user, uint32_t error_info);
static void session_drain_inputs(struct rdp_app_session *s);
static void queue_input(struct rdp_app_session *s, unsigned kind, uint16_t code, uint16_t flags, uint16_t x, uint16_t y);

/*
 * Starts the RDP client session thread for target.
 */
int
rdp_app_session_start(
	struct kl_app *app,
	struct kl_window *window,
	const struct rdp_app_target *target,
	struct rdp_app_session **out)
{
	struct rdp_app_session *s;
	struct kl_thread_options opts;
	int error;
	if (app == NULL || window == NULL || target == NULL || out == NULL)
		return EINVAL;
	*out = NULL;
	s = (struct rdp_app_session *)calloc(1U, sizeof(*s));
	if (s == NULL)
		return ENOMEM;
	error = kl_mutex_init(&s->lock);
	if (error != 0) {
		free(s);
		return error;
	}
	error = kl_socket_waker_open(&s->waker);
	if (error != 0) {
		kl_mutex_destroy(&s->lock);
		free(s);
		return error;
	}
	s->app = app;
	s->window = window;
	s->target = *target;
	s->width = target->width > 0U ? target->width : 1280U;
	s->height = target->height > 0U ? target->height : 720U;
	memset(&opts, 0, sizeof(opts));
	opts.run = rdp_thread_entry;
	opts.argument = s;
	opts.name = "rdp-client";
	opts.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&opts, &s->thread);
	if (error != 0) {
		kl_socket_waker_close(s->waker);
		kl_mutex_destroy(&s->lock);
		free(s);
		return error;
	}
	*out = s;

	/* Succeeded: RDP session thread running. */
	return 0;
}

/*
 * Stops the RDP session thread, waits for exit, and frees all resources.
 */
void
rdp_app_session_stop(
	struct rdp_app_session *session)
{
	if (session == NULL)
		return;
	kl_mutex_lock(&session->lock);
	session->stop = 1;
	kl_mutex_unlock(&session->lock);
	kl_socket_waker_ring(session->waker);
	if (session->thread != NULL)
		(void)kl_thread_join(session->thread);
	if (session->scaler != NULL)
		media_scaler_free(session->scaler);
	if (session->decoder != NULL)
		media_decoder_close(session->decoder);
	if (session->ssl != NULL)
		SSL_free(session->ssl);
	if (session->ssl_ctx != NULL)
		SSL_CTX_free(session->ssl_ctx);
	if (session->sock != NULL)
		kl_socket_close(session->sock);
	kl_socket_waker_close(session->waker);
	kl_mutex_destroy(&session->lock);
	free(session->fb);
	free(session->scratch);
	free(session);
}

/*
 * Queues a Fast-Path keyboard scancode event for transmission.
 */
void
rdp_app_session_send_key(
	struct rdp_app_session *session,
	uint16_t scancode,
	uint16_t flags)
{
	if (session != NULL)
		queue_input(session, RDP_IN_KEY, scancode, flags, 0U, 0U);
}

/*
 * Queues a Fast-Path Unicode keyboard event for transmission.
 */
void
rdp_app_session_send_unicode(
	struct rdp_app_session *session,
	uint16_t codepoint,
	uint16_t flags)
{
	if (session != NULL)
		queue_input(session, RDP_IN_UNICODE, codepoint, flags, 0U, 0U);
}

/*
 * Queues a Fast-Path mouse pointer event for transmission.
 */
void
rdp_app_session_send_mouse(
	struct rdp_app_session *session,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	if (session != NULL)
		queue_input(session, RDP_IN_MOUSE, 0U, flags, x, y);
}

/*
 * Queues an MS-RDPEDISP resize request when the local window changes size.
 */
void
rdp_app_session_request_resize(
	struct rdp_app_session *session,
	uint32_t width,
	uint32_t height)
{
	if (session == NULL || width == 0U || height == 0U)
		return;
	kl_mutex_lock(&session->lock);
	session->wanted_w = (width + 1U) & ~1U;
	session->wanted_h = (height + 1U) & ~1U;
	session->wanted_resize = 1;
	kl_mutex_unlock(&session->lock);
	kl_socket_waker_ring(session->waker);
}

/*
 * Copies the latest decoded 0xAARRGGBB frame into pixels (width x height).
 */
int
rdp_app_session_copy_frame(
	struct rdp_app_session *session,
	uint32_t *pixels,
	uint32_t width,
	uint32_t height)
{
	uint32_t y;
	uint32_t copy_w;
	uint32_t copy_h;
	int copied;
	if (session == NULL || pixels == NULL)
		return 0;
	copied = 0;
	kl_mutex_lock(&session->lock);
	if (session->fb != NULL && session->fb_w > 0U && session->fb_h > 0U) {
		copy_w = session->fb_w < width ? session->fb_w : width;
		copy_h = session->fb_h < height ? session->fb_h : height;
		for (y = 0U; y < copy_h; y++) {
			memcpy(pixels + (size_t)y * width,
			       session->fb + (size_t)y * session->fb_w,
			       (size_t)copy_w * sizeof(*pixels));
		}
		copied = 1;
	}

	kl_mutex_unlock(&session->lock);
	return copied;
}

/* Worker thread entry point running TCP, TLS, librdp, and libmedia decode. */
static void
rdp_thread_entry(
	void *arg)
{
	struct rdp_app_session *s;
	struct media_track track;
	struct rdp_callbacks cb;
	struct rdp_client_config cfg;
	struct kl_socket_wait waits[2];
	size_t ready;
	uint8_t rxbuf[16384];
	size_t got;
	int ssl_rc;
	int stopping;
	int error;
	s = (struct rdp_app_session *)arg;

	/* Opens libmedia H.264 decoder (vkvideo or h264sw fallback). */
	memset(&track, 0, sizeof(track));
	track.kind = MEDIA_TRACK_VIDEO;
	track.codec = MEDIA_CODEC_H264;
	track.width = s->width;
	track.height = s->height;
	if (media_decoder_open(&track, &s->decoder) != 0)
		goto done;
	if (session_connect_tcp(s) != 0)
		goto done;
	memset(&cb, 0, sizeof(cb));
	cb.send_bytes = session_send_bytes;
	cb.on_tls_required = session_on_tls;
	cb.on_connected = session_on_connected;
	cb.on_avc_frame = session_on_avc_frame;
	cb.on_resize = session_on_resize;
	cb.on_disconnect = session_on_disconnect;
	s->client = rdp_client_create(&cb, s);
	if (s->client == NULL)
		goto done;
	memset(&cfg, 0, sizeof(cfg));
	cfg.width = s->width;
	cfg.height = s->height;
	cfg.domain = s->target.domain;
	cfg.user_name = s->target.user;
	cfg.password = s->target.password;
	cfg.client_name = "zedbsd-rdp";
	if (rdp_client_connect(s->client, &cfg) != RDP_OK)
		goto done;
	for (;;) {
		kl_mutex_lock(&s->lock);
		stopping = s->stop;
		kl_mutex_unlock(&s->lock);
		if (stopping != 0 || s->disconnected != 0)
			break;
		session_drain_inputs(s);
		memset(waits, 0, sizeof(waits));
		waits[0].socket = s->sock;
		waits[0].events = KL_SOCKET_READABLE;
		waits[1].waker = s->waker;
		waits[1].events = KL_SOCKET_READABLE;
		ready = 0U;
		error = kl_socket_poll(waits, 2U, 200, &ready);
		if (error != 0 && error != ETIMEDOUT)
			break;
		if (waits[0].ready == 0U)
			continue;
		for (;;) {
			if (s->tls_active != 0) {
				ssl_rc = SSL_read(s->ssl, rxbuf,
						  (int)sizeof(rxbuf));
				if (ssl_rc <= 0) {
					int err;
					err = SSL_get_error(s->ssl, ssl_rc);
					if (err == SSL_ERROR_WANT_READ ||
					    err == SSL_ERROR_WANT_WRITE)
						break;
					s->disconnected = 1;
					break;
				}
				got = (size_t)ssl_rc;
			} else {
				error = kl_socket_receive(s->sock, rxbuf,
							  sizeof(rxbuf), &got);
				if (error == EAGAIN || error == EWOULDBLOCK)
					break;
				if (error != 0 || got == 0U) {
					s->disconnected = 1;
					break;
				}
			}
			if (rdp_client_feed(s->client, rxbuf, got) != RDP_OK) {
				s->disconnected = 1;
				break;
			}
		}
	}

done:
	if (s->client != NULL) {
		rdp_client_disconnect(s->client);
		rdp_client_destroy(s->client);
		s->client = NULL;
	}
	(void)kl_app_post(s->app, RDP_APP_POST_END, NULL);
}

/* Resolves and connects a non-blocking TCP socket to the RDP server. */
static int
session_connect_tcp(
	struct rdp_app_session *s)
{
	struct kl_address addrs[8];
	size_t count;
	size_t i;
	struct kl_socket *sock;
	struct kl_socket_wait wait;
	size_t ready;
	int error;
	count = 0U;
	if (kl_socket_resolve(s->target.host, s->target.port, KL_ADDRESS_ANY,
			      KL_SOCKET_STREAM, addrs, 8U, &count) != 0 ||
	    count == 0U) {
		return EHOSTUNREACH;
	}
	for (i = 0U; i < count; i++) {
		error = kl_socket_open(addrs[i].family, KL_SOCKET_STREAM,
				       KL_SOCKET_NONBLOCKING, &sock);
		if (error != 0)
			continue;
		error = kl_socket_connect(sock, &addrs[i]);
		if (error == EINPROGRESS || error == EWOULDBLOCK) {
			memset(&wait, 0, sizeof(wait));
			wait.socket = sock;
			wait.events = KL_SOCKET_WRITABLE;
			ready = 0U;
			error = kl_socket_poll(&wait, 1U, 10000, &ready);
			if (error == 0)
				error = kl_socket_connect_result(sock);
		}
		if (error == 0) {
			(void)kl_socket_set_option(sock, KL_SOCKET_NO_DELAY, 1);
			s->sock = sock;
			return 0;
		}
		kl_socket_close(sock);
	}
	return ECONNREFUSED;
}

/* Sends raw or TLS-encrypted bytes on the session's TCP socket. */
static int
session_send_bytes(
	void *user,
	const uint8_t *data,
	size_t len)
{
	struct rdp_app_session *s;
	size_t off;
	size_t sent;
	int rc;
	struct kl_socket_wait wait;
	size_t ready;
	int error;
	s = (struct rdp_app_session *)user;
	off = 0U;
	while (off < len) {
		if (s->tls_active != 0) {
			rc = SSL_write(s->ssl, data + off, (int)(len - off));
			if (rc > 0) {
				off += (size_t)rc;
				continue;
			}
			rc = SSL_get_error(s->ssl, rc);
			if (rc != SSL_ERROR_WANT_WRITE &&
			    rc != SSL_ERROR_WANT_READ)
				return -1;
		} else {
			error = kl_socket_send(s->sock, data + off, len - off,
					       &sent);
			if (error == 0) {
				off += sent;
				continue;
			}
			if (error != EAGAIN && error != EWOULDBLOCK)
				return -1;
		}
		memset(&wait, 0, sizeof(wait));
		wait.socket = s->sock;
		wait.events = KL_SOCKET_WRITABLE;
		ready = 0U;
		if (kl_socket_poll(&wait, 1U, 5000, &ready) != 0)
			return -1;
	}
	return 0;
}

/* Performs TLS client handshake using libssl-compat. */
static void
session_on_tls(
	void *user)
{
	struct rdp_app_session *s;
	int rc;
	int err;
	struct kl_socket_wait wait;
	size_t ready;
	s = (struct rdp_app_session *)user;
	s->ssl_ctx = SSL_CTX_new(TLS_client_method());
	if (s->ssl_ctx == NULL) {
		s->disconnected = 1;
		return;
	}
	SSL_CTX_set_verify(s->ssl_ctx, SSL_VERIFY_NONE, NULL);
	s->ssl = SSL_new(s->ssl_ctx);
	if (s->ssl == NULL) {
		s->disconnected = 1;
		return;
	}
	(void)SSL_set_fd(s->ssl, (int)kl_socket_native(s->sock));
	(void)SSL_set_tlsext_host_name(s->ssl, s->target.host);
	for (;;) {
		rc = SSL_connect(s->ssl);
		if (rc == 1)
			break;
		err = SSL_get_error(s->ssl, rc);
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
			s->disconnected = 1;
			return;
		}
		memset(&wait, 0, sizeof(wait));
		wait.socket = s->sock;
		wait.events = (err == SSL_ERROR_WANT_WRITE) ?
			      KL_SOCKET_WRITABLE : KL_SOCKET_READABLE;
		ready = 0U;
		if (kl_socket_poll(&wait, 1U, 10000, &ready) != 0) {
			s->disconnected = 1;
			return;
		}
	}
	s->tls_active = 1;
	if (rdp_client_tls_established(s->client) != RDP_OK)
		s->disconnected = 1;
}

/* Records initial remote desktop dimensions on connect. */
static void
session_on_connected(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct rdp_app_session *s;
	s = (struct rdp_app_session *)user;
	s->width = width;
	s->height = height;
}

/* Decodes an incoming H.264 AVC frame via libmedia and notifies UI. */
static void
session_on_avc_frame(
	void *user,
	uint32_t frame_id,
	uint16_t codec_id,
	const struct rdp_avc_stream *avc)
{
	struct rdp_app_session *s;
	struct media_packet pkt;
	struct media_frame *frame;
	struct media_frame *last;
	int64_t pts;
	size_t need;
	uint32_t *nb;
	uint32_t *dst;
	int rc;

	UNUSED_PARAMETER(codec_id);
	s = (struct rdp_app_session *)user;
	if (s->decoder == NULL || avc->luma.data == NULL ||
	    avc->luma.data_len == 0U)
		return;
	memset(&pkt, 0, sizeof(pkt));
	pkt.data = avc->luma.data;
	pkt.size = avc->luma.data_len;
	pkt.keyframe = 1;
	(void)media_decoder_send(s->decoder, &pkt);
	last = NULL;
	pts = 0;
	for (;;) {
		rc = media_decoder_receive(s->decoder, &pts);
		if (rc <= 0)
			break;
		frame = media_decoder_picture(s->decoder);
		if (frame != NULL) {
			if (last != NULL)
				media_frame_free(&last);
			last = frame;
		}
	}
	if (last == NULL) {
		static const uint8_t aud_nal[6] = {
			0x00U, 0x00U, 0x00U, 0x01U, 0x09U, 0xf0U
		};
		struct media_packet aud_pkt;

		memset(&aud_pkt, 0, sizeof(aud_pkt));
		aud_pkt.data = aud_nal;
		aud_pkt.size = sizeof(aud_nal);
		aud_pkt.keyframe = 0;
		if (media_decoder_send(s->decoder, &aud_pkt) == 0) {
			for (;;) {
				rc = media_decoder_receive(s->decoder, &pts);
				if (rc <= 0)
					break;
				frame = media_decoder_picture(s->decoder);
				if (frame != NULL) {
					if (last != NULL)
						media_frame_free(&last);
					last = frame;
				}
			}
		}
	}
	if (last != NULL) {
		need = (size_t)s->width * (size_t)s->height;
		if (need > s->scratch_cap) {
			nb = (uint32_t *)realloc(
				s->scratch, need * sizeof(*s->scratch));
			if (nb != NULL) {
				s->scratch = nb;
				s->scratch_cap = need;
			}
		}
		if (s->scratch != NULL &&
		    media_frame_scale(last, &s->scaler, s->scratch,
				      (size_t)s->width * 4U,
				      (int)s->width,
				      (int)s->height) == 0) {
			kl_mutex_lock(&s->lock);
			if (s->fb == NULL || s->fb_w != s->width ||
			    s->fb_h != s->height) {
				dst = (uint32_t *)realloc(
					s->fb, need * sizeof(*s->fb));
				if (dst != NULL) {
					s->fb = dst;
					s->fb_w = s->width;
					s->fb_h = s->height;
				}
			}
			if (s->fb != NULL && s->fb_w == s->width &&
			    s->fb_h == s->height) {
				memcpy(s->fb, s->scratch,
				       need * sizeof(*s->fb));
			}

			kl_mutex_unlock(&s->lock);
			(void)kl_app_post(s->app, RDP_APP_POST_FRAME,
					  NULL);
		}
		media_frame_free(&last);
	}
	(void)rdp_client_send_frame_ack(s->client, frame_id, 0U);
}

/* Updates target dimensions when the server confirms resize. */
static void
session_on_resize(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct rdp_app_session *s;
	s = (struct rdp_app_session *)user;
	s->width = width;
	s->height = height;
}

/* Marks the session disconnected. */
static void
session_on_disconnect(
	void *user,
	uint32_t error_info)
{
	struct rdp_app_session *s;

	UNUSED_PARAMETER(error_info);
	s = (struct rdp_app_session *)user;
	s->disconnected = 1;
}

/* Drains queued input and resize events to librdp. */
static void
session_drain_inputs(
	struct rdp_app_session *s)
{
	struct rdp_in_event batch[RDP_SESSION_INPUTS];
	unsigned count;
	unsigned i;
	uint32_t rw;
	uint32_t rh;
	int do_resize;
	if (rdp_client_is_connected(s->client) == 0)
		return;
	kl_mutex_lock(&s->lock);
	count = s->input_count;
	if (count > 0U) {
		memcpy(batch, s->inputs, count * sizeof(batch[0]));
		s->input_count = 0U;
	}
	do_resize = s->wanted_resize;
	rw = s->wanted_w;
	rh = s->wanted_h;
	s->wanted_resize = 0;
	kl_mutex_unlock(&s->lock);
	for (i = 0U; i < count; i++) {
		if (batch[i].kind == RDP_IN_KEY) {
			(void)rdp_client_send_key(s->client, batch[i].code,
						  batch[i].flags);
		} else if (batch[i].kind == RDP_IN_UNICODE) {
			(void)rdp_client_send_unicode(s->client, batch[i].code,
						      batch[i].flags);
		} else if (batch[i].kind == RDP_IN_MOUSE) {
			(void)rdp_client_send_mouse(s->client, batch[i].flags,
						    batch[i].x, batch[i].y);
		}
	}
	if (do_resize != 0)
		(void)rdp_client_request_resize(s->client, rw, rh);
}

/* Appends one input event to the bounded queue and wakes the RDP thread. */
static void
queue_input(
	struct rdp_app_session *s,
	unsigned kind,
	uint16_t code,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	struct rdp_in_event *ev;
	kl_mutex_lock(&s->lock);
	if (s->input_count < RDP_SESSION_INPUTS) {
		ev = &s->inputs[s->input_count++];
		ev->kind = kind;
		ev->code = code;
		ev->flags = flags;
		ev->x = x;
		ev->y = y;
	}

	kl_mutex_unlock(&s->lock);
	kl_socket_waker_ring(s->waker);
}
