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
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct rdpd_client_ctx {
	int sock;
	SSL *ssl;
	int tls_active;
	int running;
	int authed;
	int is_extend;
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
static void cb_on_input_unicode(void *user, uint16_t codepoint, uint16_t flags);
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

/* Looks up an NTLM NT hash from RDPD_NTLM_USERS_PATH (user:password or user:ntlm:<hex32>). */
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
	if (strstr(user_name, "+extend") != NULL) {
		struct rdpd_client_ctx *ctx = (struct rdpd_client_ctx *)user;
		if (rdpd_extend_lookup(user_name, out_nt_hash) != 0) {
			fprintf(stderr, "RDPD: extend lookup failed user=%s\n",
				user_name);
			return -1;
		}
		ctx->is_extend = 1;
		ctx->authed = 1;
		(void)snprintf(ctx->user, sizeof(ctx->user), "%s", user_name);
		fprintf(stderr, "RDPD: extend lookup ok user=%s\n", user_name);
		return 0;
	}
	ulen = strlen(user_name);
	fp = fopen(RDPD_NTLM_USERS_PATH, "r");
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
	if (user_name != NULL && strstr(user_name, "+extend") != NULL) {
		uint8_t expected_hash[16];
		uint8_t got_hash[16];
		uint8_t diff = 0U;
		size_t i;

		if (password != NULL &&
		    rdpd_extend_lookup(user_name, expected_hash) == 0) {
			rdp_ntlm_nt_hash(password, got_hash);
			for (i = 0U; i < 16U; i++)
				diff |= (uint8_t)(expected_hash[i] ^ got_hash[i]);
			memset(expected_hash, 0, sizeof(expected_hash));
			memset(got_hash, 0, sizeof(got_hash));
			if (diff == 0U) {
				(void)snprintf(ctx->user, sizeof(ctx->user),
					       "%s", user_name);
				ctx->is_extend = 1;
				ctx->authed = 1;
				fprintf(stderr, "RDPD: extend auth ok user=%s\n",
					user_name);
				(void)rdp_server_accept_auth(ctx->srv);
				return;
			}
		}
		fprintf(stderr, "RDPD: extend auth failed user=%s\n", user_name);
		(void)rdp_server_reject_auth(ctx->srv, 0U);
		ctx->running = 0;
		return;
	}
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
	if (ctx->is_extend) {
		int attach_rc;
		kwl_rdp_clamp_size(&ctx->width, &ctx->height);
		attach_rc = rdpd_extend_attach(ctx->user, ctx->width,
					       ctx->height, &ctx->sess.rdp_fd);
		fprintf(stderr, "RDPD: extend attach user=%s %ux%u rc=%d fd=%d\n",
			ctx->user, ctx->width, ctx->height, attach_rc,
			ctx->sess.rdp_fd);
		if (attach_rc == 0) {
			ctx->sess.width = ctx->width;
			ctx->sess.height = ctx->height;
			ctx->session_started = 1;
		} else {
			(void)rdp_server_disconnect(ctx->srv, 0U);
			ctx->running = 0;
		}
		return;
	}
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
	if (!ctx->is_extend)
		rdpd_hid_set_size(&ctx->hid, width, height);
	rdpd_session_resize(&ctx->sess, width, height);
	(void)rdp_server_resize(ctx->srv, width, height);
}

/* Forwards client keyboard scancodes into /dev/input/bridge or extended rdp_fd. */
static void
cb_on_input_key(
	void *user,
	uint16_t scancode,
	uint16_t flags)
{
	struct rdpd_client_ctx *ctx;
	ctx = (struct rdpd_client_ctx *)user;
	if (ctx->is_extend) {
		uint8_t msg[KWL_RDP_CTRL_HDR_SIZE];
		if (ctx->sess.rdp_fd >= 0) {
			kwl_rdp_pack_ctrl(msg, KWL_RDP_MSG_KEY,
					  (uint32_t)scancode, (uint32_t)flags);
			(void)write(ctx->sess.rdp_fd, msg, sizeof(msg));
		}
		return;
	}
	rdpd_hid_key(&ctx->hid, scancode, flags);
}

/* Translates client Fast-Path Unicode events into PS/2 scancode key presses/releases. */
static void
cb_on_input_unicode(
	void *user,
	uint16_t codepoint,
	uint16_t flags)
{
	uint16_t scancode;
	int need_shift;
	int release;

	if (rdpd_unicode_to_ps2(codepoint, &scancode, &need_shift) != 0)
		return;
	release = (flags & RDP_KBD_FLAG_RELEASE) != 0U;
	if (!release) {
		if (need_shift)
			cb_on_input_key(user, 0x2AU, 0U);
		cb_on_input_key(user, scancode, 0U);
	} else {
		cb_on_input_key(user, scancode, RDP_KBD_FLAG_RELEASE);
		if (need_shift)
			cb_on_input_key(user, 0x2AU, RDP_KBD_FLAG_RELEASE);
	}
}

/* Forwards client mouse events into /dev/input/bridge or extended rdp_fd. */
static void
cb_on_input_mouse(
	void *user,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	struct rdpd_client_ctx *ctx;
	ctx = (struct rdpd_client_ctx *)user;
	if (ctx->is_extend) {
		uint8_t msg[KWL_RDP_CTRL_HDR_SIZE];
		if (ctx->sess.rdp_fd >= 0) {
			uint32_t packed_xy = ((uint32_t)x << 16) | (uint32_t)y;
			kwl_rdp_pack_ctrl(msg, KWL_RDP_MSG_POINTER,
					  (uint32_t)flags, packed_xy);
			(void)write(ctx->sess.rdp_fd, msg, sizeof(msg));
		}
		return;
	}
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
	ctx.sess.sessiond_fd = -1;
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
	cb.on_input_unicode = cb_on_input_unicode;
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
		if (nfds == 2U &&
		    (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
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

/* Ensures the parent directory of path exists (mode 0700). */
static void
ensure_parent_dir(
	const char *path,
	mode_t mode)
{
	char dir[256];
	const char *slash;
	size_t len;

	if (path == NULL)
		return;
	slash = strrchr(path, '/');
	if (slash == NULL || slash == path)
		return;
	len = (size_t)(slash - path);
	if (len >= sizeof(dir))
		return;
	memcpy(dir, path, len);
	dir[len] = '\0';
	(void)mkdir(RDPD_STATE_PARENT, 0755);
	(void)mkdir(dir, mode);
}

/* Writes data[0..len) to path with exact mode; unlinks on write failure. */
static int
write_file_exact(
	const char *path,
	const void *data,
	size_t len,
	mode_t mode)
{
	int fd;
	size_t off;
	ssize_t n;
	const uint8_t *p;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return -1;
	(void)fchmod(fd, mode);
	p = (const uint8_t *)data;
	off = 0U;
	while (off < len) {
		n = write(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			(void)unlink(path);
			return -1;
		}
		off += (size_t)n;
	}
	if (close(fd) != 0) {
		(void)unlink(path);
		return -1;
	}
	return 0;
}

/* Persists the generated self-signed certificate and private key as PEM. */
static void
save_tls_pem(
	SSL_CTX *ssl_ctx,
	const char *cert_path,
	const char *key_path)
{
	char *cert_pem;
	size_t cert_len;
	char *key_pem;
	size_t key_len;
	volatile char *vp;
	size_t i;

	cert_pem = NULL;
	cert_len = 0U;
	key_pem = NULL;
	key_len = 0U;
	if (SSL_CTX_export_PEM(ssl_ctx, &cert_pem, &cert_len,
			       &key_pem, &key_len) != 1) {
		fprintf(stderr, "RDPD: failed to export generated TLS PEM\n");
		return;
	}

	ensure_parent_dir(cert_path, 0700);
	ensure_parent_dir(key_path, 0700);
	if (write_file_exact(cert_path, cert_pem, cert_len, 0644) != 0) {
		fprintf(stderr, "RDPD: failed to save %s: errno=%d\n",
			cert_path, errno);
	}
	if (write_file_exact(key_path, key_pem, key_len, 0600) != 0) {
		fprintf(stderr, "RDPD: failed to save %s: errno=%d\n",
			key_path, errno);
	}

	vp = (volatile char *)key_pem;
	for (i = 0U; i < key_len; i++)
		vp[i] = '\0';
	free(cert_pem);
	free(key_pem);
}

static void save_tls_pem(SSL_CTX *ssl_ctx, const char *cert_path, const char *key_path);

/* Validates a username for ntlm.users (non-empty, no colon or control chars). */
static int
valid_ntlm_username(
	const char *username)
{
	size_t i;

	if (username == NULL || username[0] == '\0')
		return 0;
	for (i = 0U; username[i] != '\0'; i++) {
		unsigned char c = (unsigned char)username[i];
		if (c == ':' || c < 0x20U || c == 0x7fU || i >= 63U)
			return 0;
	}
	return 1;
}

/* Checks whether username is present in RDPD_NTLM_USERS_PATH. */
static int
cmd_has_user(
	const char *username)
{
	FILE *fp;
	char line[512];
	size_t ulen;

	if (!valid_ntlm_username(username))
		return 1;
	ulen = strlen(username);
	fp = fopen(RDPD_NTLM_USERS_PATH, "r");
	if (fp == NULL)
		return 1;
	while (fgets(line, sizeof(line), fp) != NULL) {
		size_t len = strlen(line);
		while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r'))
			line[--len] = '\0';
		if (line[0] == '\0' || line[0] == '#')
			continue;
		if (len > ulen && line[ulen] == ':' && strncmp(line, username, ulen) == 0) {
			memset(line, 0, sizeof(line));
			fclose(fp);
			return 0;
		}
	}
	memset(line, 0, sizeof(line));
	fclose(fp);
	return 1;
}

/* Reads password from stdin, hashes it, and atomically updates RDPD_NTLM_USERS_PATH. */
static int
cmd_add_user(
	const char *username)
{
	char password[256];
	uint8_t nt_hash[16];
	char hex[33];
	char tmp_path[256];
	char line[512];
	char entry[128];
	FILE *in_fp;
	FILE *out_fp;
	int out_fd;
	size_t ulen;
	size_t plen;
	size_t i;
	int elen;
	int write_ok;
	volatile char *vp;
	static const char hex_digits[] = "0123456789abcdef";

	if (!valid_ntlm_username(username))
		return 1;
	if (fgets(password, (int)sizeof(password), stdin) == NULL)
		return 1;
	plen = strlen(password);
	while (plen > 0U && (password[plen - 1U] == '\n' || password[plen - 1U] == '\r'))
		password[--plen] = '\0';

	rdp_ntlm_nt_hash(password, nt_hash);
	vp = (volatile char *)password;
	for (i = 0U; i < sizeof(password); i++)
		vp[i] = '\0';

	for (i = 0U; i < 16U; i++) {
		hex[i * 2U] = hex_digits[(nt_hash[i] >> 4) & 0x0fU];
		hex[i * 2U + 1U] = hex_digits[nt_hash[i] & 0x0fU];
	}
	hex[32] = '\0';
	memset(nt_hash, 0, sizeof(nt_hash));

	ensure_parent_dir(RDPD_NTLM_USERS_PATH, 0700);
	(void)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", RDPD_NTLM_USERS_PATH);
	out_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out_fd < 0) {
		memset(hex, 0, sizeof(hex));
		return 1;
	}
	(void)fchmod(out_fd, 0600);
	out_fp = fdopen(out_fd, "w");
	if (out_fp == NULL) {
		close(out_fd);
		(void)unlink(tmp_path);
		memset(hex, 0, sizeof(hex));
		return 1;
	}

	write_ok = 1;
	ulen = strlen(username);
	in_fp = fopen(RDPD_NTLM_USERS_PATH, "r");
	if (in_fp != NULL) {
		while (fgets(line, sizeof(line), in_fp) != NULL) {
			size_t len = strlen(line);
			if (len > ulen && line[ulen] == ':' && strncmp(line, username, ulen) == 0)
				continue;
			if (fputs(line, out_fp) == EOF) {
				write_ok = 0;
				break;
			}
		}
		memset(line, 0, sizeof(line));
		fclose(in_fp);
	}

	elen = snprintf(entry, sizeof(entry), "%s:ntlm:%s\n", username, hex);
	memset(hex, 0, sizeof(hex));
	if (elen <= 0 || (size_t)elen >= sizeof(entry) || fputs(entry, out_fp) == EOF)
		write_ok = 0;
	memset(entry, 0, sizeof(entry));

	if (fflush(out_fp) != 0)
		write_ok = 0;
	if (fclose(out_fp) != 0)
		write_ok = 0;
	if (!write_ok || rename(tmp_path, RDPD_NTLM_USERS_PATH) != 0) {
		(void)unlink(tmp_path);
		return 1;
	}
	return 0;
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

	if (argc >= 3 && strcmp(argv[1], "--has-user") == 0)
		return cmd_has_user(argv[2]);
	if (argc >= 3 && strcmp(argv[1], "--add-user") == 0)
		return cmd_add_user(argv[2]);

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
		save_tls_pem(ssl_ctx, cert_path, key_path);
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
