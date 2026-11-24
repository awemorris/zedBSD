/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Remote Desktop Protocol server daemon (rdpd) entry point and client loop.
 */

#include "rdpd.h"
#include "userland/desktop/librdp/rdp.h"
#include "userland/desktop/wayland/rdp-headless.h"

#include <arpa/inet.h>
#include <compat/ssl/ssl.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct rdpd_client_ctx {
	int sock;
	SSL *ssl;
	int tls_active;
	int running;
	int authed;
	int session_started;
	uint32_t width;
	uint32_t height;
	struct rdp_server *srv;
	struct rdpd_hid hid;
	struct rdpd_session sess;
	uid_t uid;
	gid_t gid;
	char user[64];
	char home[256];
	char shell[128];
};

static volatile sig_atomic_t rdpd_stop;

static void
on_sig(
	int sig)
{
	(void)sig;
	rdpd_stop = 1;
}

static int
cb_send_bytes(
	void *user,
	const uint8_t *data,
	size_t len)
{
	struct rdpd_client_ctx *ctx;
	size_t off;

	ctx = (struct rdpd_client_ctx *)user;
	off = 0U;
	while (off < len) {
		ssize_t n;

		if (ctx->tls_active && ctx->ssl != NULL)
			n = SSL_write(ctx->ssl, data + off, (int)(len - off));
		else
			n = send(ctx->sock, data + off, len - off, 0);

		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

static void
cb_on_auth_request(
	void *user,
	const char *domain,
	const char *user_name,
	const char *password)
{
	struct rdpd_client_ctx *ctx;

	(void)domain;
	ctx = (struct rdpd_client_ctx *)user;
	if (rdpd_auth_user(user_name, password, &ctx->uid, &ctx->gid,
			   ctx->home, sizeof(ctx->home),
			   ctx->shell, sizeof(ctx->shell)) == 0) {
		(void)snprintf(ctx->user, sizeof(ctx->user), "%s", user_name);
		ctx->authed = 1;
		(void)rdp_server_accept_auth(ctx->srv);
	} else {
		(void)rdp_server_reject_auth(ctx->srv, 0U);
		ctx->running = 0;
	}
}

static void
cb_on_connected(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct rdpd_client_ctx *ctx;

	ctx = (struct rdpd_client_ctx *)user;
	ctx->width = width != 0U ? width : 1280U;
	ctx->height = height != 0U ? height : 720U;
	if (!ctx->authed)
		return;
	if (rdpd_hid_open(&ctx->hid, ctx->width, ctx->height) != 0)
		return;
	if (rdpd_session_start(&ctx->sess, ctx->uid, ctx->gid, ctx->user,
			       ctx->home, ctx->shell,
			       ctx->width, ctx->height) == 0) {
		ctx->session_started = 1;
	}
}

static void
cb_on_resize(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct rdpd_client_ctx *ctx;

	ctx = (struct rdpd_client_ctx *)user;
	kwl_rdp_clamp_size(&width, &height);
	ctx->width = width;
	ctx->height = height;
	rdpd_hid_set_size(&ctx->hid, width, height);
	rdpd_session_resize(&ctx->sess, width, height);
	(void)rdp_server_resize(ctx->srv, width, height);
}

static void
cb_on_input_key(
	void *user,
	uint16_t scancode,
	uint16_t flags)
{
	struct rdpd_client_ctx *ctx;

	ctx = (struct rdpd_client_ctx *)user;
	rdpd_hid_key(&ctx->hid, scancode, flags);
}

static void
cb_on_input_mouse(
	void *user,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	struct rdpd_client_ctx *ctx;

	ctx = (struct rdpd_client_ctx *)user;
	rdpd_hid_mouse(&ctx->hid, flags, x, y);
}

static void
cb_on_disconnect(
	void *user,
	uint32_t error_info)
{
	struct rdpd_client_ctx *ctx;

	(void)error_info;
	ctx = (struct rdpd_client_ctx *)user;
	ctx->running = 0;
}

static int
read_exact_fd(
	int fd,
	uint8_t *buf,
	size_t len)
{
	size_t off;

	off = 0U;
	while (off < len) {
		ssize_t n;

		n = read(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

static void
handle_client(
	int client_sock,
	SSL_CTX *ssl_ctx)
{
	struct rdpd_client_ctx ctx;
	struct rdp_callbacks cb;
	struct rdp_server_config cfg;
	uint8_t io_buf[16384];
	uint8_t *au_buf;
	size_t au_cap;

	memset(&ctx, 0, sizeof(ctx));
	ctx.sock = client_sock;
	ctx.hid.fd = -1;
	ctx.sess.rdp_fd = -1;
	ctx.running = 1;
	ctx.width = 1280U;
	ctx.height = 720U;

	memset(&cb, 0, sizeof(cb));
	cb.send_bytes = cb_send_bytes;
	cb.on_auth_request = cb_on_auth_request;
	cb.on_connected = cb_on_connected;
	cb.on_resize = cb_on_resize;
	cb.on_input_key = cb_on_input_key;
	cb.on_input_mouse = cb_on_input_mouse;
	cb.on_disconnect = cb_on_disconnect;

	memset(&cfg, 0, sizeof(cfg));
	cfg.width = ctx.width;
	cfg.height = ctx.height;
	ctx.srv = rdp_server_create(&cb, &ctx, &cfg);
	if (ctx.srv == NULL) {
		close(client_sock);
		return;
	}

	au_cap = 262144U;
	au_buf = (uint8_t *)malloc(au_cap);
	if (au_buf == NULL) {
		rdp_server_destroy(ctx.srv);
		close(client_sock);
		return;
	}

	while (ctx.running && !rdpd_stop) {
		struct pollfd pfds[2];
		nfds_t nfds;
		int rc;

		if (rdp_server_needs_tls(ctx.srv) && !ctx.tls_active) {
			ctx.ssl = SSL_new(ssl_ctx);
			if (ctx.ssl == NULL ||
			    SSL_set_fd(ctx.ssl, ctx.sock) != 1 ||
			    SSL_accept(ctx.ssl) != 1) {
				break;
			}
			ctx.tls_active = 1;
			if (rdp_server_tls_established(ctx.srv) != 0)
				break;
		}

		pfds[0].fd = ctx.sock;
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;
		nfds = 1U;
		if (ctx.session_started && ctx.sess.rdp_fd >= 0) {
			pfds[1].fd = ctx.sess.rdp_fd;
			pfds[1].events = POLLIN;
			pfds[1].revents = 0;
			nfds = 2U;
		}

		rc = poll(pfds, nfds, 100);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if ((pfds[0].revents & POLLIN) != 0) {
			int n;

			if (ctx.tls_active)
				n = SSL_read(ctx.ssl, io_buf, sizeof(io_buf));
			else
				n = (int)recv(ctx.sock, io_buf, sizeof(io_buf), 0);
			if (n <= 0)
				break;
			if (rdp_server_feed(ctx.srv, io_buf, (size_t)n) != 0)
				break;
		}

		if (nfds == 2U && (pfds[1].revents & POLLIN) != 0) {
			uint8_t hdr[KWL_RDP_FRAME_HDR_SIZE];
			uint32_t flags;
			uint64_t pts_us;
			uint32_t size;

			if (read_exact_fd(ctx.sess.rdp_fd, hdr, sizeof(hdr)) != 0)
				break;
			kwl_rdp_unpack_frame_hdr(hdr, &flags, &pts_us, &size);
			if (size == 0U || size > 4194304U)
				break;
			if (size > au_cap) {
				uint8_t *nb = (uint8_t *)realloc(au_buf, size);
				if (nb == NULL)
					break;
				au_buf = nb;
				au_cap = size;
			}
			if (read_exact_fd(ctx.sess.rdp_fd, au_buf, size) != 0)
				break;

			if (rdp_server_is_connected(ctx.srv) &&
			    rdp_server_unacked_frames(ctx.srv) < 4U) {
				struct rdp_avc_rect rect;
				struct rdp_avc_quant quant;
				struct rdp_avc_stream avc;

				rect.left = 0U;
				rect.top = 0U;
				rect.right = ctx.width;
				rect.bottom = ctx.height;
				quant.qp = 26U;
				quant.quality = 100U;
				memset(&avc, 0, sizeof(avc));
				avc.luma.rect_count = 1U;
				avc.luma.rects = &rect;
				avc.luma.quants = &quant;
				avc.luma.data = au_buf;
				avc.luma.data_len = size;
				(void)rdp_server_send_avc_frame(
					ctx.srv, RDP_GFX_CODEC_AVC420, &avc,
					(uint32_t)(pts_us / 1000U), NULL);
			}
		}
	}

	free(au_buf);
	rdpd_session_stop(&ctx.sess);
	rdpd_hid_close(&ctx.hid);
	rdp_server_destroy(ctx.srv);
	if (ctx.ssl != NULL) {
		(void)SSL_shutdown(ctx.ssl);
		SSL_free(ctx.ssl);
	}
	close(client_sock);
}

int
main(
	int argc,
	char **argv)
{
	uint16_t port;
	const char *cert_path;
	const char *key_path;
	SSL_CTX *ssl_ctx;
	int listen_fd;
	int opt;
	struct sockaddr_in addr;
	int i;

	port = RDPD_DEFAULT_PORT;
	cert_path = RDPD_CERT_PATH;
	key_path = RDPD_KEY_PATH;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
			port = (uint16_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			cert_path = argv[++i];
		} else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
			key_path = argv[++i];
		}
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	ssl_ctx = SSL_CTX_new(TLS_server_method());
	if (ssl_ctx == NULL)
		return 1;

	if (SSL_CTX_use_certificate_file(ssl_ctx, cert_path, SSL_FILETYPE_PEM) != 1 ||
	    SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) != 1) {
		if (SSL_CTX_generate_self_signed(ssl_ctx, "zedbsd-rdpd") != 1) {
			SSL_CTX_free(ssl_ctx);
			return 1;
		}
	}

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		SSL_CTX_free(ssl_ctx);
		return 1;
	}

	opt = 1;
	(void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(listen_fd, 8) != 0) {
		close(listen_fd);
		SSL_CTX_free(ssl_ctx);
		return 1;
	}

	while (!rdpd_stop) {
		int client_fd;
		pid_t pid;

		while (waitpid(-1, NULL, WNOHANG) > 0)
			;

		client_fd = accept(listen_fd, NULL, NULL);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		pid = fork();
		if (pid == 0) {
			close(listen_fd);
			handle_client(client_fd, ssl_ctx);
			SSL_CTX_free(ssl_ctx);
			_exit(0);
		}
		close(client_fd);
	}

	close(listen_fd);
	SSL_CTX_free(ssl_ctx);
	return 0;
}
