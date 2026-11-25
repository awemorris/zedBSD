/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * RDP client worker thread: TCP connect via libkeiland, TLS handshake via
 * libssl-compat, librdp state machine, and H.264 decoding via rdp_decoder_ops.
 */

#include "rdp-thread.h"
#include "mikuterm.h"

#include "userland/desktop/librdp/rdp.h"

#include <compat/ssl/ssl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#define UNUSED_PARAMETER(x)	((void)(x))

/*
 * Per-thread RDP session context.
 */
struct rdp_worker {
	struct rdp_tab *tab;
	struct rdp_host_entry target;
	const struct rdp_decoder_ops *decoder_ops;
	void *decoder_ctx;
	struct kl_socket *sock;
	struct kl_socket_waker *waker;
	SSL_CTX *ssl_ctx;
	SSL *ssl;
	int tls_active;
	struct rdp_client *client;
	uint32_t width;
	uint32_t height;
	uint32_t *frame_buf;
	size_t frame_cap;
	int connected;
	int disconnected;
};

static void rdp_worker_main(void *arg);
static int worker_connect_tcp(struct rdp_worker *w, unsigned timeout_sec);
static int worker_send_bytes(void *user, const uint8_t *data, size_t len);
static void worker_on_tls(void *user);
static void worker_on_connected(void *user, uint32_t width, uint32_t height);
static void worker_on_avc_frame(void *user, uint32_t frame_id, uint16_t codec_id, const struct rdp_avc_stream *avc);
static void worker_on_resize(void *user, uint32_t width, uint32_t height);
static void worker_on_disconnect(void *user, uint32_t error_info);
static void worker_drain_tab(struct rdp_worker *w);

/*
 * Starts a detached worker thread for tab connecting to target.
 */
int
rdp_thread_start(
	struct rdp_tab *tab,
	const struct rdp_host_entry *target,
	const struct rdp_decoder_ops *decoder_ops,
	unsigned connect_timeout)
{
	struct rdp_thread_args *args;
	struct kl_thread_options opts;
	struct kl_thread *thr;
	int error;
	if (tab == NULL || target == NULL || decoder_ops == NULL)
		return EINVAL;
	args = (struct rdp_thread_args *)calloc(1U, sizeof(*args));
	if (args == NULL)
		return ENOMEM;
	args->tab = tab;
	args->target = *target;
	args->decoder_ops = decoder_ops;
	args->connect_timeout = connect_timeout > 0U ? connect_timeout : 15U;
	memset(&opts, 0, sizeof(opts));
	opts.run = rdp_worker_main;
	opts.argument = args;
	opts.name = "rdp-client";
	opts.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&opts, &thr);
	if (error != 0) {
		free(args);
		return error;
	}
	(void)kl_thread_detach(thr);

	/* Succeeded: RDP worker thread started. */
	return 0;
}

/* Worker thread entry point driving TCP, TLS, librdp, and H.264 decode. */
static void
rdp_worker_main(
	void *arg)
{
	struct rdp_thread_args *args;
	struct rdp_worker w;
	struct rdp_callbacks cb;
	struct rdp_client_config cfg;
	struct kl_socket_wait waits[2];
	size_t ready;
	uint8_t rxbuf[16384];
	size_t got;
	int ssl_rc;
	int error;
	char msg[256];
	args = (struct rdp_thread_args *)arg;
	memset(&w, 0, sizeof(w));
	w.tab = args->tab;
	w.target = args->target;
	w.decoder_ops = args->decoder_ops;
	w.width = args->target.width > 0U ? args->target.width : 1280U;
	w.height = args->target.height > 0U ? args->target.height : 720U;
	if (kl_socket_waker_open(&w.waker) != 0) {
		rdp_tab_end(w.tab, "Failed to create RDP waker.");
		rdp_tab_release(w.tab);
		free(args);
		return;
	}
	rdp_tab_attach_waker(w.tab, w.waker);
	if (w.decoder_ops->create(&w.decoder_ctx) != 0) {
		rdp_tab_end(w.tab, "Failed to initialize H.264 decoder.");
		goto cleanup;
	}
	if (worker_connect_tcp(&w, args->connect_timeout) != 0) {
		snprintf(msg, sizeof(msg), "Could not connect to %s:%u.",
			 w.target.host_name, (unsigned)w.target.port);
		rdp_tab_end(w.tab, msg);
		goto cleanup;
	}
	{
		int nodelay = 1;
		(void)setsockopt((int)kl_socket_native(w.sock), IPPROTO_TCP,
				 TCP_NODELAY, (const char *)&nodelay,
				 sizeof(nodelay));
	}
	memset(&cb, 0, sizeof(cb));
	cb.send_bytes = worker_send_bytes;
	cb.on_tls_required = worker_on_tls;
	cb.on_connected = worker_on_connected;
	cb.on_avc_frame = worker_on_avc_frame;
	cb.on_resize = worker_on_resize;
	cb.on_disconnect = worker_on_disconnect;
	w.client = rdp_client_create(&cb, &w);
	if (w.client == NULL) {
		rdp_tab_end(w.tab, "Out of memory creating RDP client.");
		goto cleanup;
	}
	memset(&cfg, 0, sizeof(cfg));
	cfg.width = w.width;
	cfg.height = w.height;
	cfg.domain = w.target.domain;
	cfg.user_name = w.target.user;
	cfg.password = w.target.password;
	cfg.client_name = "mikuterm";
	if (rdp_client_connect(w.client, &cfg) != RDP_OK) {
		rdp_tab_end(w.tab, "RDP connection handshake failed.");
		goto cleanup;
	}

	/* Event loop polling the TCP socket and the UI tab waker. */
	while (w.disconnected == 0 && rdp_tab_is_closing(w.tab) == 0) {
		worker_drain_tab(&w);
		memset(waits, 0, sizeof(waits));
		waits[0].socket = w.sock;
		waits[0].events = KL_SOCKET_READABLE;
		waits[1].waker = w.waker;
		waits[1].events = KL_SOCKET_READABLE;
		ready = 0U;
		error = kl_socket_poll(waits, 2U, 200, &ready);
		if (error != 0 && error != ETIMEDOUT)
			break;
		if (waits[0].ready == 0U)
			continue;
		for (;;) {
			if (w.tls_active != 0) {
				ssl_rc = SSL_read(w.ssl, rxbuf,
						  (int)sizeof(rxbuf));
				if (ssl_rc <= 0) {
					int err;
					err = SSL_get_error(w.ssl, ssl_rc);
					if (err == SSL_ERROR_WANT_READ ||
					    err == SSL_ERROR_WANT_WRITE)
						break;
					w.disconnected = 1;
					break;
				}
				got = (size_t)ssl_rc;
			} else {
				error = kl_socket_receive(w.sock, rxbuf,
							  sizeof(rxbuf), &got);
				if (error == EAGAIN || error == EWOULDBLOCK)
					break;
				if (error != 0 || got == 0U) {
					w.disconnected = 1;
					break;
				}
			}
			if (rdp_client_feed(w.client, rxbuf, got) != RDP_OK) {
				w.disconnected = 1;
				break;
			}
		}
	}
	rdp_tab_end(w.tab, "RDP session closed.");

cleanup:
	if (w.client != NULL) {
		rdp_client_disconnect(w.client);
		rdp_client_destroy(w.client);
	}
	if (w.ssl != NULL)
		SSL_free(w.ssl);
	if (w.ssl_ctx != NULL)
		SSL_CTX_free(w.ssl_ctx);
	if (w.sock != NULL)
		kl_socket_close(w.sock);
	if (w.decoder_ctx != NULL)
		w.decoder_ops->destroy(w.decoder_ctx);
	free(w.frame_buf);
	kl_socket_waker_close(w.waker);
	rdp_tab_release(w.tab);
	free(args);
}

/* Resolves and connects a non-blocking TCP socket to the RDP server. */
static int
worker_connect_tcp(
	struct rdp_worker *w,
	unsigned timeout_sec)
{
	struct kl_address addrs[8];
	size_t count;
	size_t i;
	struct kl_socket *s;
	struct kl_socket_wait wait;
	size_t ready;
	int error;
	if (kl_socket_resolve(w->target.host_name, w->target.port,
			      KL_ADDRESS_ANY, KL_SOCKET_STREAM, addrs,
			      8U, &count) != 0 || count == 0U)
		return EHOSTUNREACH;
	for (i = 0U; i < count; i++) {
		error = kl_socket_open(addrs[i].family, KL_SOCKET_STREAM,
				       KL_SOCKET_NONBLOCKING, &s);
		if (error != 0)
			continue;
		error = kl_socket_connect(s, &addrs[i]);
		if (error == EINPROGRESS || error == EWOULDBLOCK) {
			memset(&wait, 0, sizeof(wait));
			wait.socket = s;
			wait.events = KL_SOCKET_WRITABLE;
			ready = 0U;
			error = kl_socket_poll(&wait, 1U,
					       (int)(timeout_sec * 1000U),
					       &ready);
			if (error == 0)
				error = kl_socket_connect_result(s);
		}
		if (error == 0) {
			(void)kl_socket_set_option(s, KL_SOCKET_NO_DELAY, 1);
			w->sock = s;
			return 0;
		}
		kl_socket_close(s);
	}
	return ECONNREFUSED;
}

/* Sends raw or TLS-encrypted bytes on the worker's TCP socket. */
static int
worker_send_bytes(
	void *user,
	const uint8_t *data,
	size_t len)
{
	struct rdp_worker *w;
	size_t off;
	size_t sent;
	int rc;
	struct kl_socket_wait wait;
	size_t ready;
	int error;
	w = (struct rdp_worker *)user;
	off = 0U;
	while (off < len) {
		if (w->tls_active != 0) {
			rc = SSL_write(w->ssl, data + off, (int)(len - off));
			if (rc > 0) {
				off += (size_t)rc;
				continue;
			}
			rc = SSL_get_error(w->ssl, rc);
			if (rc != SSL_ERROR_WANT_WRITE &&
			    rc != SSL_ERROR_WANT_READ)
				return -1;
		} else {
			error = kl_socket_send(w->sock, data + off, len - off,
					       &sent);
			if (error == 0) {
				off += sent;
				continue;
			}
			if (error != EAGAIN && error != EWOULDBLOCK)
				return -1;
		}
		memset(&wait, 0, sizeof(wait));
		wait.socket = w->sock;
		wait.events = KL_SOCKET_WRITABLE;
		ready = 0U;
		if (kl_socket_poll(&wait, 1U, 5000, &ready) != 0)
			return -1;
	}
	return 0;
}

/* Performs the TLS client handshake after X.224 Connection Confirm. */
static void
worker_on_tls(
	void *user)
{
	struct rdp_worker *w;
	int rc;
	int err;
	struct kl_socket_wait wait;
	size_t ready;
	w = (struct rdp_worker *)user;
	w->ssl_ctx = SSL_CTX_new(TLS_client_method());
	if (w->ssl_ctx == NULL) {
		w->disconnected = 1;
		return;
	}
	SSL_CTX_set_verify(w->ssl_ctx, SSL_VERIFY_NONE, NULL);
	w->ssl = SSL_new(w->ssl_ctx);
	if (w->ssl == NULL) {
		w->disconnected = 1;
		return;
	}
	(void)SSL_set_fd(w->ssl, (int)kl_socket_native(w->sock));
	(void)SSL_set_tlsext_host_name(w->ssl, w->target.host_name);
	for (;;) {
		rc = SSL_connect(w->ssl);
		if (rc == 1)
			break;
		err = SSL_get_error(w->ssl, rc);
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
			w->disconnected = 1;
			return;
		}
		memset(&wait, 0, sizeof(wait));
		wait.socket = w->sock;
		wait.events = (err == SSL_ERROR_WANT_WRITE) ?
			      KL_SOCKET_WRITABLE : KL_SOCKET_READABLE;
		ready = 0U;
		if (kl_socket_poll(&wait, 1U, 10000, &ready) != 0) {
			w->disconnected = 1;
			return;
		}
	}
	w->tls_active = 1;
	if (rdp_client_tls_established(w->client) != RDP_OK)
		w->disconnected = 1;
}

/* Updates status when the RDP session completes capability negotiation. */
static void
worker_on_connected(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct rdp_worker *w;
	w = (struct rdp_worker *)user;
	w->connected = 1;
	w->width = width;
	w->height = height;
	rdp_tab_set_status(w->tab, "Connected");
}

/* Decodes an incoming H.264 AVC frame and publishes it to the RDP tab. */
static void
worker_on_avc_frame(
	void *user,
	uint32_t frame_id,
	uint16_t codec_id,
	const struct rdp_avc_stream *avc)
{
	struct rdp_worker *w;
	size_t need;
	uint32_t dec_w;
	uint32_t dec_h;
	int error;

	UNUSED_PARAMETER(codec_id);
	w = (struct rdp_worker *)user;
	if (avc->luma.data == NULL || avc->luma.data_len == 0U)
		return;
	need = (size_t)w->width * (size_t)w->height;
	if (need > w->frame_cap) {
		uint32_t *nb;
		nb = (uint32_t *)realloc(w->frame_buf,
					 need * sizeof(*w->frame_buf));
		if (nb == NULL)
			return;
		w->frame_buf = nb;
		w->frame_cap = need;
	}
	dec_w = w->width;
	dec_h = w->height;
	error = w->decoder_ops->decode(w->decoder_ctx, avc->luma.data,
				       avc->luma.data_len, w->frame_buf,
				       w->width, w->height, &dec_w, &dec_h);
	if (error == 0)
		(void)rdp_tab_put_frame(w->tab, w->frame_buf, w->width,
					w->height);
	(void)rdp_client_send_frame_ack(w->client, frame_id, 0U);
}

/* Updates the worker's target surface dimensions on server resize. */
static void
worker_on_resize(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct rdp_worker *w;
	w = (struct rdp_worker *)user;
	w->width = width;
	w->height = height;
}

/* Marks the worker loop for exit on server disconnect. */
static void
worker_on_disconnect(
	void *user,
	uint32_t error_info)
{
	struct rdp_worker *w;

	w = (struct rdp_worker *)user;
	if (w->connected == 0 && error_info != 0U)
		mikuterm_rdp_password_forget(w->target.host_name, w->target.port, w->target.user);
	w->disconnected = 1;
}

/* Drains queued user inputs and resize requests from the UI tab. */
static void
worker_drain_tab(
	struct rdp_worker *w)
{
	struct rdp_tab_input in;
	uint32_t rw;
	uint32_t rh;
	if (rdp_client_is_connected(w->client) == 0)
		return;
	while (rdp_tab_take_input(w->tab, &in) != 0) {
		if (in.kind == RDP_TAB_IN_KEY)
			(void)rdp_client_send_key(w->client, in.code, in.flags);
		else if (in.kind == RDP_TAB_IN_UNICODE)
			(void)rdp_client_send_unicode(w->client, in.code,
						      in.flags);
		else if (in.kind == RDP_TAB_IN_MOUSE)
			(void)rdp_client_send_mouse(w->client, in.flags,
						    in.x, in.y);
		else if (in.kind == RDP_TAB_IN_SYNC)
			(void)rdp_client_send_sync(w->client, in.flags);
	}
	if (rdp_tab_take_resize(w->tab, &rw, &rh) != 0)
		(void)rdp_client_request_resize(w->client, rw, rh);
}
