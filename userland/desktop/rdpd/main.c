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
#include <netinet/tcp.h>
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

static void on_sig(int sig);
static int cb_send_bytes(void *user, const uint8_t *data, size_t len);
static void cb_on_auth_request(void *user, const char *domain, const char *user_name, const char *password);
static void cb_on_connected(void *user, uint32_t width, uint32_t height);
static void cb_on_resize(void *user, uint32_t width, uint32_t height);
static void cb_on_input_key(void *user, uint16_t scancode, uint16_t flags);
static void cb_on_input_mouse(void *user, uint16_t flags, uint16_t x, uint16_t y);
static void cb_on_disconnect(void *user, uint32_t error_info);
static int read_exact_fd(int fd, uint8_t *buf, size_t len);
static void handle_client(int client_sock, SSL_CTX *ssl_ctx);

/* Records shutdown signal receipt for the accept/client loops. */
static void
on_sig(
	int sig)
{
	(void)sig;
	rdpd_stop = 1;
}

/* Sends outgoing RDP/TLS bytes to the connected client socket. */
static int
cb_send_bytes(
	void *user,
	const uint8_t *data,
	size_t len)
{
	struct rdpd_client_ctx *ctx;
	size_t off;
	ssize_t n;
	ctx = (struct rdpd_client_ctx *)user;
	off = 0U;
	while (off < len) {
		if (ctx->tls_active && ctx->ssl != NULL)
			n = SSL_write(ctx->ssl, data + off, (int)(len - off));
		else
			n = send(ctx->sock, data + off, len - off, 0);
		if (n <= 0) {
			if (ctx->tls_active && ctx->ssl != NULL) {
				fprintf(stderr, "RDPD: SSL_write n=%zd ssl_err=%d errno=%d\n",
					n, SSL_get_error(ctx->ssl, (int)n), errno);
			} else {
				fprintf(stderr, "RDPD: send n=%zd errno=%d\n", n, errno);
			}
			return -1;
		}
		off += (size_t)n;
	}

	/* Succeeded. */
	return 0;
}

/* Looks up an NTLM NT hash from /etc/rdpd/ntlm.users (user:password or user:ntlm:<hex32>). */
static int
hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

static int
cb_on_ntlm_lookup(
	void *user,
	const char *domain,
	const char *user_name,
	uint8_t out_nt_hash[16])
{
	FILE *fp;
	char line[512];
	size_t ulen;

	(void)user;
	(void)domain;
	if (user_name == NULL || user_name[0] == '\0')
		return -1;
	ulen = strlen(user_name);
	fp = fopen("/etc/rdpd/ntlm.users", "r");
	if (fp == NULL)
		return -1;
	while (fgets(line, sizeof(line), fp) != NULL) {
		size_t len = strlen(line);
		char *sep;
		while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r'))
			line[--len] = '\0';
		if (line[0] == '\0' || line[0] == '#')
			continue;
		sep = strchr(line, ':');
		if (sep == NULL)
			continue;
		if ((size_t)(sep - line) != ulen ||
		    strncmp(line, user_name, ulen) != 0)
			continue;
		sep++;
		if (strncmp(sep, "ntlm:", 5U) == 0 && strlen(sep + 5U) == 32U) {
			size_t i;
			int ok = 1;
			for (i = 0U; i < 16U; i++) {
				int hi = hex_nibble(sep[5U + i * 2U]);
				int lo = hex_nibble(sep[5U + i * 2U + 1U]);
				if (hi < 0 || lo < 0) {
					ok = 0;
					break;
				}
				out_nt_hash[i] = (uint8_t)((hi << 4) | lo);
			}
			fclose(fp);
			return ok ? 0 : -1;
		}
		rdp_ntlm_nt_hash(sep, out_nt_hash);
		memset(line, 0, sizeof(line));
		fclose(fp);
		return 0;
	}
	fclose(fp);
	return -1;
}

/* Authenticates client credentials via passkey and accepts or rejects. */
static void
cb_on_auth_request(
	void *user,
	const char *domain,
	const char *user_name,
	const char *password)
{
	struct rdpd_client_ctx *ctx;
	int rc;
	(void)domain;
	ctx = (struct rdpd_client_ctx *)user;
	rc = rdpd_auth_user(user_name, password, &ctx->uid, &ctx->gid,
			    ctx->home, sizeof(ctx->home),
			    ctx->shell, sizeof(ctx->shell));
	if (rc == 0) {
		(void)snprintf(ctx->user, sizeof(ctx->user), "%s", user_name);
		ctx->authed = 1;
		(void)rdp_server_accept_auth(ctx->srv);
	} else {
		(void)rdp_server_reject_auth(ctx->srv, 0U);
		ctx->running = 0;
	}
}

/* Opens virtual HID and starts the headless Wayland desktop session. */
static void
cb_on_connected(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct rdpd_client_ctx *ctx;
	int rc;
	ctx = (struct rdpd_client_ctx *)user;
	ctx->width = 1280U;
	if (width != 0U)
		ctx->width = width;
	ctx->height = 720U;
	if (height != 0U)
		ctx->height = height;
	fprintf(stderr, "RDPD: connected %ux%u authed=%d\n",
		ctx->width, ctx->height, ctx->authed);
	if (!ctx->authed)
		return;
	rc = rdpd_hid_open(&ctx->hid, ctx->width, ctx->height);
	if (rc != 0)
		return;
	rc = rdpd_session_start(&ctx->sess, ctx->uid, ctx->gid, ctx->user,
				ctx->home, ctx->shell,
				ctx->width, ctx->height, ctx->hid.event_dev);
	if (rc == 0)
		ctx->session_started = 1;
}

/* Propagates client RDPEDISP resize requests to HID and Wayland. */
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

/* Forwards client keyboard scancodes into /dev/input/bridge. */
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

/* Forwards client mouse events into /dev/input/bridge. */
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

/* Stops the client loop upon RDP disconnect. */
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

/* Reads exactly len bytes from a stream descriptor. */
static int
read_exact_fd(
	int fd,
	uint8_t *buf,
	size_t len)
{
	size_t off;
	ssize_t n;
	off = 0U;
	while (off < len) {
		n = read(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		/* Checks for EOF before the full message was read. */
		if (n == 0)
			return -1;
		off += (size_t)n;
	}

	/* Succeeded. */
	return 0;
}

/* Runs one client RDP session in a dedicated child process. */
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
	struct pollfd pfds[2];
	nfds_t nfds;
	int rc;
	int need_tls;
	int set_ok;
	int acc_ok;
	int n;
	uint8_t hdr[KWL_RDP_FRAME_HDR_SIZE];
	uint32_t flags;
	uint64_t pts_us;
	uint32_t size;
	uint8_t *nb;
	int conn;
	uint32_t unacked;
	struct rdp_avc_rect rect;
	struct rdp_avc_quant quant;
	struct rdp_avc_stream avc;
	memset(&ctx, 0, sizeof(ctx));
	ctx.sock = client_sock;
	{
		int nodelay = 1;
		(void)setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY,
				 &nodelay, sizeof(nodelay));
	}
	ctx.hid.fd = -1;
	ctx.sess.rdp_fd = -1;
	ctx.running = 1;
	ctx.width = 1280U;
	ctx.height = 720U;
	memset(&cb, 0, sizeof(cb));
	cb.send_bytes = cb_send_bytes;
	cb.on_auth_request = cb_on_auth_request;
	cb.on_ntlm_lookup = cb_on_ntlm_lookup;
	cb.on_connected = cb_on_connected;
	cb.on_resize = cb_on_resize;
	cb.on_input_key = cb_on_input_key;
	cb.on_input_mouse = cb_on_input_mouse;
	cb.on_disconnect = cb_on_disconnect;
	memset(&cfg, 0, sizeof(cfg));
	cfg.width = ctx.width;
	cfg.height = ctx.height;
	(void)SSL_CTX_get0_certificate_ASN1(ssl_ctx, &cfg.tls_cert_der,
					    &cfg.tls_cert_der_len);
	ctx.srv = rdp_server_create(&cb, &ctx, &cfg);
	if (ctx.srv == NULL) {
		close(client_sock);
		return;
	}

	/* Allocates initial H.264 Access Unit staging buffer. */
	au_cap = 262144U;
	au_buf = (uint8_t *)malloc(au_cap);
	if (au_buf == NULL) {
		rdp_server_destroy(ctx.srv);
		close(client_sock);
		return;
	}

	/* Multiplexes network RDP traffic and headless Wayland video frames. */
	while (ctx.running && !rdpd_stop) {
		need_tls = rdp_server_needs_tls(ctx.srv);
		if (need_tls != 0 && !ctx.tls_active) {
			ctx.ssl = SSL_new(ssl_ctx);
			if (ctx.ssl == NULL)
				break;
			set_ok = SSL_set_fd(ctx.ssl, ctx.sock);
			acc_ok = SSL_accept(ctx.ssl);
			if (set_ok != 1 || acc_ok != 1) {
				fprintf(stderr, "RDPD: SSL_accept failed set=%d acc=%d\n",
					set_ok, acc_ok);
				break;
			}
			ctx.tls_active = 1;
			rc = rdp_server_tls_established(ctx.srv);
			if (rc != 0)
				break;
		}

		/* Prepares poll descriptors for socket and optional compositor pipe. */
		pfds[0].fd = ctx.sock;
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;
		nfds = 1U;
		conn = rdp_server_is_gfx_ready(ctx.srv);
		unacked = rdp_server_unacked_frames(ctx.srv);
		if (ctx.session_started && ctx.sess.rdp_fd >= 0 &&
		    conn != 0 && unacked < 6U) {
			pfds[1].fd = ctx.sess.rdp_fd;
			pfds[1].events = POLLIN;
			pfds[1].revents = 0;
			nfds = 2U;
		}

		/* Polls both descriptors with a short timeout for signal checks. */
		rc = poll(pfds, nfds, 100);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		/* Reads and feeds incoming RDP/TLS bytes (draining buffered TLS records). */
		if ((pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			for (;;) {
				if (ctx.tls_active)
					n = SSL_read(ctx.ssl, io_buf, sizeof(io_buf));
				else
					n = (int)recv(ctx.sock, io_buf, sizeof(io_buf), 0);
				if (n <= 0) {
					if (ctx.tls_active) {
						int ssl_err = SSL_get_error(ctx.ssl, n);
						if (ssl_err == SSL_ERROR_WANT_READ ||
						    ssl_err == SSL_ERROR_WANT_WRITE)
							break;
						fprintf(stderr, "RDPD: SSL_read n=%d err=%d\n",
							n, ssl_err);
					} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
						break;
					} else {
						fprintf(stderr, "RDPD: recv n=%d errno=%d\n",
							n, errno);
					}
					ctx.running = 0;
					break;
				}
				rc = rdp_server_feed(ctx.srv, io_buf, (size_t)n);
				if (rc != 0) {
					fprintf(stderr, "RDPD: rdp_server_feed rc=%d\n", rc);
					ctx.running = 0;
					break;
				}
				if (!ctx.tls_active || SSL_pending(ctx.ssl) <= 0)
					break;
			}
			if (!ctx.running)
				break;
		}

		/* Reads encoded H.264 AU from headless Wayland and sends over RDPEGFX. */
		if (nfds == 2U && (pfds[1].revents & POLLIN) != 0) {
			rc = read_exact_fd(ctx.sess.rdp_fd, hdr, sizeof(hdr));
			if (rc != 0) {
				fprintf(stderr, "RDPD: rdp_fd EOF/err rc=%d\n", rc);
				break;
			}
			kwl_rdp_unpack_frame_hdr(hdr, &flags, &pts_us, &size);
			if (size == 0U || size > 4194304U)
				break;
			if (size > au_cap) {
				nb = (uint8_t *)realloc(au_buf, size);
				if (nb == NULL)
					break;
				au_buf = nb;
				au_cap = size;
			}

			/* Reads the H.264 Access Unit payload and forwards it to RDPEGFX. */
			rc = read_exact_fd(ctx.sess.rdp_fd, au_buf, size);
			if (rc != 0)
				break;
			conn = rdp_server_is_connected(ctx.srv);
			if (conn != 0) {
				uint32_t fid = 0U;
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
				rc = rdp_server_send_avc_frame(
					ctx.srv, RDP_GFX_CODEC_AVC420, &avc,
					(uint32_t)(pts_us / 1000U), &fid);
				if (rc != 0) {
					fprintf(stderr, "RDPD: send_avc fid=%u size=%u rc=%d\n",
						fid, size, rc);
					break;
				}
			}
		}
	}

	/* Releases session, HID, RDP server, TLS, and socket state. */
	free(au_buf);
	rdpd_session_stop(&ctx.sess);
	rdpd_hid_close(&ctx.hid);
	rdp_server_destroy(ctx.srv);
	if (ctx.ssl != NULL) {
		(void)SSL_shutdown(ctx.ssl);
		SSL_free(ctx.ssl);
	}

	/* Closes the client socket. */
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
	int m1;
	int m2;
	int m3;
	int c_ok;
	int k_ok;
	int g_ok;
	int b_ok;
	int l_ok;
	int client_fd;
	pid_t pid;
	pid_t wp;
	port = RDPD_DEFAULT_PORT;
	cert_path = RDPD_CERT_PATH;
	key_path = RDPD_KEY_PATH;
	for (i = 1; i < argc; i++) {
		m1 = strcmp(argv[i], "-p");
		m2 = strcmp(argv[i], "-c");
		m3 = strcmp(argv[i], "-k");
		if (m1 == 0 && i + 1 < argc) {
			port = (uint16_t)atoi(argv[++i]);
		} else if (m2 == 0 && i + 1 < argc) {
			cert_path = argv[++i];
		} else if (m3 == 0 && i + 1 < argc) {
			key_path = argv[++i];
		}
	}

	/* Installs signal handlers and initializes server TLS context. */
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);
	ssl_ctx = SSL_CTX_new(TLS_server_method());
	if (ssl_ctx == NULL)
		return 1;
	c_ok = SSL_CTX_use_certificate_file(ssl_ctx, cert_path, SSL_FILETYPE_PEM);
	k_ok = SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM);
	if (c_ok != 1 || k_ok != 1) {
		g_ok = SSL_CTX_generate_self_signed(ssl_ctx, "zedbsd-rdpd");
		if (g_ok != 1) {
			SSL_CTX_free(ssl_ctx);
			return 1;
		}
	}

	/* Binds and listens on the configured TCP port. */
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		SSL_CTX_free(ssl_ctx);
		return 1;
	}

	/* Enables address reuse and binds the listening socket. */
	opt = 1;
	(void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	b_ok = bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
	l_ok = listen(listen_fd, 8);
	if (b_ok != 0 || l_ok != 0) {
		close(listen_fd);
		SSL_CTX_free(ssl_ctx);
		return 1;
	}

	/* Accepts incoming RDP client connections and forks per-session workers. */
	while (!rdpd_stop) {
		for (;;) {
			wp = waitpid(-1, NULL, WNOHANG);
			if (wp <= 0)
				break;
		}

		/* Accepts the next client connection. */
		client_fd = accept(listen_fd, NULL, NULL);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		/* Forks a worker child for the accepted client. */
		pid = fork();
		if (pid == 0) {
			close(listen_fd);
			handle_client(client_fd, ssl_ctx);
			SSL_CTX_free(ssl_ctx);
			_exit(0);
		}

		/* Parent closes its copy of the client socket. */
		close(client_fd);
	}

	/* Cleans up listening socket and TLS context on shutdown. */
	close(listen_fd);
	SSL_CTX_free(ssl_ctx);
	return 0;
}
