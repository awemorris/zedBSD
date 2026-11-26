/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Native zedBSD Display Sink (Target Mode) application (/bin/dispsink).
 * Advertises availability on UDP 33890, receives encrypted OFFER packets
 * from a Display Sink Host (such as Win32 mikuterm), and transitions into
 * an RDP/H.264 viewer session using librdp + libmedia.
 */

#include "apps/mikuterm-common/netdisp-proto.h"
#include "userland/desktop/rdp/rdp-client.h"
#include "userland/desktop/librdp/rdp.h"

#include <arpa/inet.h>
#include <compat/crypto/crypto.h>
#include <compat/crypto/evp.h>
#include <compat/crypto/rand.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DISPSINK_DEFAULT_W	1920U
#define DISPSINK_DEFAULT_H	1080U
#define DISPSINK_POST_OFFER	0x6473706fU
#define DISPSINK_CURSOR_HIDE_MS	1500ULL

#define DISPSINK_BG		KL_RGB(0x181c20U)
#define DISPSINK_CARD_BG	KL_RGB(0x222830U)
#define DISPSINK_CARD_BORDER	KL_RGB(0x36404eU)
#define DISPSINK_TEXT		KL_RGB(0xecf2f8U)
#define DISPSINK_MUTED		KL_RGB(0x9aa7b8U)
#define DISPSINK_ACCENT		KL_RGB(0x58a6ffU)

#define DISPSINK_FONT_PRIMARY	"/usr/share/fonts/keiland.ttf"
#define DISPSINK_FONT_FALLBACK	"/usr/share/fonts/keiland-fallback.ttf"

struct dispsink_adv_state {
	struct kl_app *app;
	struct kl_mutex lock;
	struct kl_thread *thread;
	int stop_pipe[2];
	int udp_sock;
	uint16_t reply_port;
	uint8_t device_id[NETDISP_DEVICE_ID_LEN];
	uint8_t sk[NETDISP_KEY_LEN];
	uint8_t pk[NETDISP_KEY_LEN];
	uint8_t nonce[NETDISP_NONCE_LEN];
	char fingerprint[10];
	char device_name[NETDISP_MAX_NAME_LEN + 1U];
	uint32_t width;
	uint32_t height;
	int have_offer;
	struct rdp_app_target offer_target;
};

static void
dispsink_log(
	const char *fmt,
	...)
{
	FILE *f;
	va_list ap;

	f = fopen("/tmp/dispsink.log", "a");
	if (f == NULL)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	fprintf(f, "\n");
	va_end(ap);
	fclose(f);
}

static void
load_device_id(
	uint8_t out_id[NETDISP_DEVICE_ID_LEN])
{
	int fd;
	ssize_t n;
	char buf[64];
	uint8_t hash[32];

	fd = open("/etc/machine-id", O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1U);
		close(fd);
		if (n > 0) {
			unsigned int dlen = 0U;
			buf[n] = '\0';
			if (EVP_Digest((const unsigned char *)buf, (size_t)n,
				       hash, &dlen, EVP_sha256(), NULL) == 1) {
				memcpy(out_id, hash, NETDISP_DEVICE_ID_LEN);
				return;
			}
		}
	}
	if (RAND_bytes(out_id, NETDISP_DEVICE_ID_LEN) != 1)
		memset(out_id, 0x42, NETDISP_DEVICE_ID_LEN);
}

static int
set_nonblock(
	int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return errno;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return errno;
	return 0;
}

static void
send_adv_packet(
	struct dispsink_adv_state *st)
{
	struct netdisp_adv adv;
	uint8_t pkt[NETDISP_ADV_MAX_LEN];
	size_t pkt_len;
	struct sockaddr_in bcast;
	uint8_t name_len;

	memset(&adv, 0, sizeof(adv));
	kl_mutex_lock(&st->lock);
	adv.flags = 0U;
	memcpy(adv.device_id, st->device_id, NETDISP_DEVICE_ID_LEN);
	memcpy(adv.nonce, st->nonce, NETDISP_NONCE_LEN);
	memcpy(adv.pk, st->pk, NETDISP_KEY_LEN);
	adv.reply_port = st->reply_port;
	adv.pref_width = (uint16_t)(st->width > 65535U ? 65535U : st->width);
	adv.pref_height = (uint16_t)(st->height > 65535U ? 65535U : st->height);
	adv.pref_scale120 = 120U;
	netdisp_sanitize_name(st->device_name, adv.device_name, &name_len);
	adv.name_len = name_len;
	kl_mutex_unlock(&st->lock);

	pkt_len = 0U;
	if (netdisp_adv_encode(&adv, pkt, sizeof(pkt), &pkt_len) != 0)
		return;
	memset(&bcast, 0, sizeof(bcast));
	bcast.sin_family = AF_INET;
	bcast.sin_port = htons((uint16_t)NETDISP_DISCOVERY_PORT);
	bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	(void)sendto(st->udp_sock, pkt, pkt_len, 0,
		     (const struct sockaddr *)&bcast, sizeof(bcast));
	{
		const char *adv_host = getenv("DISPSINK_ADV_HOST");
		if (adv_host != NULL && adv_host[0] != '\0') {
			struct sockaddr_in dst;
			memset(&dst, 0, sizeof(dst));
			dst.sin_family = AF_INET;
			dst.sin_port = htons((uint16_t)NETDISP_DISCOVERY_PORT);
			if (inet_pton(AF_INET, adv_host, &dst.sin_addr) == 1) {
				(void)sendto(st->udp_sock, pkt, pkt_len, 0,
					     (const struct sockaddr *)&dst, sizeof(dst));
			}
		}
	}
}

static void
adv_thread_main(
	void *arg)
{
	struct dispsink_adv_state *st;
	struct pollfd pfds[2];
	uint8_t rx[NETDISP_OFFER_MAX_LEN + 64U];
	struct sockaddr_in from;
	socklen_t from_len;
	ssize_t n;
	struct netdisp_offer_plain plain;
	char ip_str[64];

	st = (struct dispsink_adv_state *)arg;
	for (;;) {
		send_adv_packet(st);
		pfds[0].fd = st->stop_pipe[0];
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;
		pfds[1].fd = st->udp_sock;
		pfds[1].events = POLLIN;
		pfds[1].revents = 0;
		if (poll(pfds, 2, 1000) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if ((pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
			break;
		if ((pfds[1].revents & POLLIN) == 0)
			continue;
		for (;;) {
			from_len = sizeof(from);
			n = recvfrom(st->udp_sock, rx, sizeof(rx), 0,
				     (struct sockaddr *)&from, &from_len);
			if (n <= 0)
				break;
			memset(&plain, 0, sizeof(plain));
			{
				int dec_rc = netdisp_offer_decrypt(rx, (size_t)n, st->device_id,
								   st->sk, st->pk, st->nonce,
								   &plain);
				if (dec_rc != 0) {
					dispsink_log("adv: udp rx len=%zd decrypt_err=%d", n, dec_rc);
					continue;
				}
			}
			if (inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str)) == NULL)
				continue;
			dispsink_log("adv: OFFER ok from %s:%u user=%s", ip_str, (unsigned)plain.rdp_port, plain.username);
			kl_mutex_lock(&st->lock);
			if (!st->have_offer) {
				memset(&st->offer_target, 0, sizeof(st->offer_target));
				snprintf(st->offer_target.host,
					 sizeof(st->offer_target.host),
					 "%s", ip_str);
				st->offer_target.port = plain.rdp_port;
				snprintf(st->offer_target.user,
					 sizeof(st->offer_target.user),
					 "%s", plain.username);
				snprintf(st->offer_target.password,
					 sizeof(st->offer_target.password),
					 "%s", plain.password);
				st->offer_target.width = st->width;
				st->offer_target.height = st->height;
				st->have_offer = 1;
				kl_mutex_unlock(&st->lock);
				(void)kl_app_post(st->app, DISPSINK_POST_OFFER, NULL);
				OPENSSL_cleanse(&plain, sizeof(plain));
				return;
			}
			kl_mutex_unlock(&st->lock);
			OPENSSL_cleanse(&plain, sizeof(plain));
		}
	}
}

static int
adv_start(
	struct dispsink_adv_state *st)
{
	struct sockaddr_in addr;
	socklen_t addr_len;
	struct kl_thread_options topts;
	int bcast;
	int err;

	if (netdisp_generate_keypair(st->sk, st->pk, st->nonce) != 0)
		return EIO;
	netdisp_fingerprint_hex(st->pk, st->nonce, st->fingerprint);
	st->have_offer = 0;

	if (pipe(st->stop_pipe) != 0)
		return errno;
	(void)set_nonblock(st->stop_pipe[0]);
	(void)set_nonblock(st->stop_pipe[1]);

	st->udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (st->udp_sock < 0) {
		err = errno;
		close(st->stop_pipe[0]);
		close(st->stop_pipe[1]);
		st->stop_pipe[0] = -1;
		st->stop_pipe[1] = -1;
		return err;
	}
	bcast = 1;
	(void)setsockopt(st->udp_sock, SOL_SOCKET, SO_BROADCAST,
			 &bcast, sizeof(bcast));
	(void)set_nonblock(st->udp_sock);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(0);
	if (bind(st->udp_sock, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
		err = errno;
		close(st->udp_sock);
		close(st->stop_pipe[0]);
		close(st->stop_pipe[1]);
		st->udp_sock = -1;
		st->stop_pipe[0] = -1;
		st->stop_pipe[1] = -1;
		return err;
	}
	addr_len = sizeof(addr);
	if (getsockname(st->udp_sock, (struct sockaddr *)&addr, &addr_len) != 0) {
		err = errno;
		close(st->udp_sock);
		close(st->stop_pipe[0]);
		close(st->stop_pipe[1]);
		st->udp_sock = -1;
		st->stop_pipe[0] = -1;
		st->stop_pipe[1] = -1;
		return err;
	}
	st->reply_port = ntohs(addr.sin_port);

	memset(&topts, 0, sizeof(topts));
	topts.run = adv_thread_main;
	topts.argument = st;
	topts.name = "dispsink-adv";
	topts.priority = KL_THREAD_PRIORITY_NORMAL;
	err = kl_thread_start(&topts, &st->thread);
	if (err != 0) {
		close(st->udp_sock);
		close(st->stop_pipe[0]);
		close(st->stop_pipe[1]);
		st->udp_sock = -1;
		st->stop_pipe[0] = -1;
		st->stop_pipe[1] = -1;
		return err;
	}
	return 0;
}

static void
adv_stop(
	struct dispsink_adv_state *st)
{
	uint8_t byte;

	if (st->stop_pipe[1] >= 0) {
		byte = 1U;
		(void)write(st->stop_pipe[1], &byte, 1U);
	}
	if (st->thread != NULL) {
		(void)kl_thread_join(st->thread);
		st->thread = NULL;
	}
	if (st->udp_sock >= 0) {
		close(st->udp_sock);
		st->udp_sock = -1;
	}
	if (st->stop_pipe[0] >= 0) {
		close(st->stop_pipe[0]);
		st->stop_pipe[0] = -1;
	}
	if (st->stop_pipe[1] >= 0) {
		close(st->stop_pipe[1]);
		st->stop_pipe[1] = -1;
	}
}

static void
draw_waiting_screen(
	struct kl_window *win,
	struct kl_text *text,
	int have_text,
	const struct dispsink_adv_state *st,
	uint32_t *canvas,
	uint32_t w,
	uint32_t h)
{
	struct kl_canvas c;
	struct kl_rect full;
	float cw;
	float ch;
	float cx;
	float cy;
	char line1[160];
	char line2[160];
	char line3[160];

	if (canvas == NULL || w == 0U || h == 0U)
		return;
	if (kl_canvas_init(&c, canvas, (size_t)w, (int)w, (int)h) != 0)
		return;
	full.x = 0;
	full.y = 0;
	full.width = (int)w;
	full.height = (int)h;
	kl_canvas_fill(&c, &full, DISPSINK_BG);

	cw = w > 540U ? 480.0f : (float)w * 0.85f;
	ch = h > 280U ? 210.0f : (float)h * 0.75f;
	cx = ((float)w - cw) * 0.5f;
	cy = ((float)h - ch) * 0.5f;
	kl_canvas_round(&c, cx, cy, cw, ch, 12.0f, DISPSINK_CARD_BG);
	kl_canvas_round_border(&c, cx, cy, cw, ch, 12.0f, 1.0f, DISPSINK_CARD_BORDER);

	if (have_text) {
		int tx = (int)cx + 28;
		int ty = (int)cy + 46;
		int max_w = (int)cw - 56;
		const char *title = "DisplaySink (Display Mode)";

		(void)kl_text_draw_fit(text, &c, tx, ty, title, 22U, 1, max_w, DISPSINK_TEXT);
		snprintf(line1, sizeof(line1), "Device: %s (%ux%u)",
			 st->device_name, (unsigned)w, (unsigned)h);
		(void)kl_text_draw_fit(text, &c, tx, ty + 38, line1, 15U, 0, max_w, DISPSINK_MUTED);
		snprintf(line2, sizeof(line2), "Verification code: %s", st->fingerprint);
		(void)kl_text_draw_fit(text, &c, tx, ty + 74, line2, 18U, 1, max_w, DISPSINK_ACCENT);
		snprintf(line3, sizeof(line3), "Waiting for DisplaySink (PC mode) on UDP %u...",
			 (unsigned)NETDISP_DISCOVERY_PORT);
		(void)kl_text_draw_fit(text, &c, tx, ty + 114, line3, 14U, 0, max_w, DISPSINK_MUTED);
	}
	kl_canvas_release(&c);
	(void)kl_window_present(win, canvas, (size_t)w);
}

static uint64_t
dispsink_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0ULL;
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void
compute_letterbox(
	uint32_t win_w,
	uint32_t win_h,
	uint32_t src_w,
	uint32_t src_h,
	uint32_t *dx,
	uint32_t *dy,
	uint32_t *dw,
	uint32_t *dh)
{
	uint32_t w;
	uint32_t h;

	if (win_w == 0U || win_h == 0U || src_w == 0U || src_h == 0U) {
		*dx = 0U;
		*dy = 0U;
		*dw = win_w;
		*dh = win_h;
		return;
	}
	if ((uint64_t)win_w * (uint64_t)src_h <= (uint64_t)win_h * (uint64_t)src_w) {
		w = win_w;
		h = (uint32_t)(((uint64_t)win_w * (uint64_t)src_h) / (uint64_t)src_w);
	} else {
		h = win_h;
		w = (uint32_t)(((uint64_t)win_h * (uint64_t)src_w) / (uint64_t)src_h);
	}
	if (w == 0U)
		w = 1U;
	if (h == 0U)
		h = 1U;
	*dw = w;
	*dh = h;
	*dx = (win_w - w) / 2U;
	*dy = (win_h - h) / 2U;
}

static void
present_rdp_canvas(
	struct kl_window *win,
	struct rdp_app_session *session,
	uint32_t *canvas,
	uint32_t w,
	uint32_t h,
	uint32_t **src_buf,
	size_t *src_cap)
{
	size_t i;
	size_t total;
	uint32_t fw = 0U;
	uint32_t fh = 0U;
	uint32_t dx;
	uint32_t dy;
	uint32_t dw;
	uint32_t dh;
	uint32_t y;
	uint32_t x;

	total = (size_t)w * (size_t)h;
	for (i = 0U; i < total; i++)
		canvas[i] = 0xFF000000U;

	if (rdp_app_session_frame_size(session, &fw, &fh) != 0 && fw > 0U && fh > 0U) {
		size_t need = (size_t)fw * (size_t)fh;
		if (need > *src_cap) {
			uint32_t *nb = (uint32_t *)realloc(*src_buf, need * sizeof(uint32_t));
			if (nb != NULL) {
				*src_buf = nb;
				*src_cap = need;
			}
		}
		if (*src_buf != NULL && *src_cap >= need &&
		    rdp_app_session_copy_frame(session, *src_buf, fw, fh) != 0) {
			compute_letterbox(w, h, fw, fh, &dx, &dy, &dw, &dh);
			if (dw == fw && dh == fh) {
				for (y = 0U; y < dh; y++) {
					memcpy(canvas + (size_t)(dy + y) * w + dx,
					       *src_buf + (size_t)y * fw,
					       (size_t)dw * sizeof(uint32_t));
				}
			} else {
				for (y = 0U; y < dh; y++) {
					uint32_t sy = (uint32_t)(((uint64_t)y * fh) / dh);
					const uint32_t *srow = *src_buf + (size_t)sy * fw;
					uint32_t *drow = canvas + (size_t)(dy + y) * w + dx;
					for (x = 0U; x < dw; x++) {
						uint32_t sx = (uint32_t)(((uint64_t)x * fw) / dw);
						drow[x] = srow[sx];
					}
				}
			}
		}
	}
	(void)kl_window_present(win, canvas, (size_t)w);
}

static void
handle_rdp_window_event(
	struct rdp_app_session *session,
	const struct kl_window_event *ev,
	uint32_t win_w,
	uint32_t win_h)
{
	uint16_t scan;
	uint16_t flags;
	uint16_t btn_flag;
	uint16_t px;
	uint16_t py;
	uint32_t fw = win_w;
	uint32_t fh = win_h;
	uint32_t dx;
	uint32_t dy;
	uint32_t dw;
	uint32_t dh;
	double vx;
	double vy;

	if (ev->kind == KL_WINDOW_KEY) {
		if (rdp_evdev_to_scancode(ev->code, &scan, &flags) != 0) {
			if (ev->pressed == 0)
				flags |= RDP_KBD_FLAG_RELEASE;
			rdp_app_session_send_key(session, scan, flags);
		}
		return;
	}
	(void)rdp_app_session_frame_size(session, &fw, &fh);
	compute_letterbox(win_w, win_h, fw, fh, &dx, &dy, &dw, &dh);
	vx = ev->x - (double)dx;
	vy = ev->y - (double)dy;
	if (vx < 0.0)
		vx = 0.0;
	if (vy < 0.0)
		vy = 0.0;
	if (dw > 0U && dh > 0U) {
		vx = (vx * (double)fw) / (double)dw;
		vy = (vy * (double)fh) / (double)dh;
	}
	if (vx >= (double)fw)
		vx = (double)(fw > 0U ? fw - 1U : 0U);
	if (vy >= (double)fh)
		vy = (double)(fh > 0U ? fh - 1U : 0U);
	px = (uint16_t)vx;
	py = (uint16_t)vy;
	if (ev->kind == KL_WINDOW_MOTION) {
		rdp_app_session_send_mouse(session, RDP_PTR_FLAG_MOVE, px, py);
	} else if (ev->kind == KL_WINDOW_BUTTON) {
		btn_flag = 0U;
		if (ev->code == KL_BUTTON_LEFT)
			btn_flag = RDP_PTR_FLAG_BUTTON1;
		else if (ev->code == KL_BUTTON_RIGHT)
			btn_flag = RDP_PTR_FLAG_BUTTON2;
		else if (ev->code == KL_BUTTON_MIDDLE)
			btn_flag = RDP_PTR_FLAG_BUTTON3;
		if (btn_flag != 0U) {
			if (ev->pressed != 0)
				btn_flag |= RDP_PTR_FLAG_DOWN;
			rdp_app_session_send_mouse(session, btn_flag, px, py);
		}
	} else if (ev->kind == KL_WINDOW_AXIS) {
		flags = RDP_PTR_FLAG_WHEEL;
		if (ev->dy > 0.0) {
			flags |= (uint16_t)(120U & 0x01FFU);
		} else {
			flags |= (uint16_t)(RDP_PTR_FLAG_WHEEL_NEGATIVE |
					    ((-120) & 0x00FFU));
		}
		rdp_app_session_send_mouse(session, flags, px, py);
	}
}

static int
parse_args(
	int argc,
	char **argv,
	uint32_t *width,
	uint32_t *height,
	int *fullscreen,
	char *name,
	size_t name_cap)
{
	int i;
	long val;

	*width = DISPSINK_DEFAULT_W;
	*height = DISPSINK_DEFAULT_H;
	*fullscreen = 0;
	if (gethostname(name, name_cap) != 0 || name[0] == '\0')
		snprintf(name, name_cap, "zedBSD Display Sink");
	name[name_cap - 1U] = '\0';

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--width=", 8) == 0) {
			val = strtol(argv[i] + 8, NULL, 10);
			if (val < 200 || val > 8192)
				return EINVAL;
			*width = ((uint32_t)val + 1U) & ~1U;
		} else if (strncmp(argv[i], "--height=", 9) == 0) {
			val = strtol(argv[i] + 9, NULL, 10);
			if (val < 200 || val > 8192)
				return EINVAL;
			*height = ((uint32_t)val + 1U) & ~1U;
		} else if (strcmp(argv[i], "--fullscreen") == 0) {
			*fullscreen = 1;
		} else if (strncmp(argv[i], "--name=", 7) == 0) {
			snprintf(name, name_cap, "%s", argv[i] + 7);
		} else {
			return EINVAL;
		}
	}
	return 0;
}

int
main(
	int argc,
	char **argv)
{
	uint32_t win_w;
	uint32_t win_h;
	int fullscreen;
	struct dispsink_adv_state adv;
	struct kl_app_options aopts;
	struct kl_app *app;
	struct kl_window_options wopts;
	struct kl_window *win;
	struct kl_text text;
	int have_text;
	uint32_t *canvas;
	size_t canvas_cap;
	uint32_t *src_buf = NULL;
	size_t src_cap = 0U;
	struct rdp_app_session *session;
	struct kl_app_event ev;
	int running;
	int cursor_hidden = 0;
	uint64_t last_motion_ms = 0ULL;
	int rc;

	memset(&adv, 0, sizeof(adv));
	adv.udp_sock = -1;
	adv.stop_pipe[0] = -1;
	adv.stop_pipe[1] = -1;

	if (parse_args(argc, argv, &win_w, &win_h, &fullscreen,
		       adv.device_name, sizeof(adv.device_name)) != 0) {
		fprintf(stderr,
			"usage: dispsink [--width=W] [--height=H] [--fullscreen] [--name=NAME]\n");
		return 1;
	}
	adv.width = win_w;
	adv.height = win_h;
	load_device_id(adv.device_id);
	if (kl_mutex_init(&adv.lock) != 0) {
		fprintf(stderr, "dispsink: kl_mutex_init failed\n");
		return 1;
	}

	memset(&aopts, 0, sizeof(aopts));
	aopts.application = "dispsink";
	app = kl_app_open(&aopts);
	if (app == NULL) {
		fprintf(stderr, "dispsink: kl_app_open failed\n");
		kl_mutex_destroy(&adv.lock);
		return 1;
	}
	adv.app = app;

	memset(&wopts, 0, sizeof(wopts));
	wopts.application = "dispsink";
	wopts.title = "DisplaySink (Display Mode)";
	wopts.width = win_w;
	wopts.height = win_h;
	wopts.fullscreen = fullscreen;
	wopts.present = KL_PRESENT_SHM;
	win = kl_app_window_create(app, &wopts);
	if (win == NULL) {
		fprintf(stderr, "dispsink: kl_window_open failed\n");
		kl_app_close(app);
		kl_mutex_destroy(&adv.lock);
		return 1;
	}

	memset(&text, 0, sizeof(text));
	have_text = (kl_text_open(&text, DISPSINK_FONT_PRIMARY, DISPSINK_FONT_FALLBACK) == 0);

	canvas_cap = (size_t)win_w * (size_t)win_h;
	canvas = (uint32_t *)calloc(canvas_cap, sizeof(*canvas));
	if (canvas == NULL) {
		if (have_text)
			kl_text_close(&text);
		kl_window_close(win);
		kl_app_close(app);
		kl_mutex_destroy(&adv.lock);
		return 1;
	}

	if (adv_start(&adv) != 0) {
		fprintf(stderr, "dispsink: failed to start UDP discovery\n");
		free(canvas);
		if (have_text)
			kl_text_close(&text);
		kl_window_close(win);
		kl_app_close(app);
		kl_mutex_destroy(&adv.lock);
		return 1;
	}
	draw_waiting_screen(win, &text, have_text, &adv, canvas, win_w, win_h);

	session = NULL;
	running = 1;
	{
		int logged_first_frame = 0;
	while (running != 0) {
		int timeout_ms = (session != NULL && !cursor_hidden) ? 250 : -1;
		rc = kl_app_dispatch(app, timeout_ms);
		if (rc != 0)
			break;
		if (session != NULL && !cursor_hidden &&
		    dispsink_now_ms() - last_motion_ms >= DISPSINK_CURSOR_HIDE_MS) {
			kl_window_set_cursor_hidden(win, 1);
			cursor_hidden = 1;
		}
		for (;;) {
			rc = kl_app_take(app, &ev);
			if (rc == 0)
				break;
			if (ev.kind == KL_APP_WINDOW && ev.window == win) {
				if (ev.input.kind == KL_WINDOW_CLOSE) {
					running = 0;
					break;
				}
				if (ev.input.kind == KL_WINDOW_RESIZE) {
					uint32_t rw = win_w;
					uint32_t rh = win_h;
					if (kl_window_present_resize(win, &rw, &rh) == 0 &&
					    rw > 0U && rh > 0U) {
						size_t need = (size_t)rw * (size_t)rh;
						win_w = rw;
						win_h = rh;
						if (need > canvas_cap) {
							uint32_t *nb = (uint32_t *)realloc(canvas, need * sizeof(*canvas));
							if (nb != NULL) {
								canvas = nb;
								canvas_cap = need;
							}
						}
						kl_mutex_lock(&adv.lock);
						adv.width = (rw + 1U) & ~1U;
						adv.height = (rh + 1U) & ~1U;
						kl_mutex_unlock(&adv.lock);
						if (session != NULL) {
							rdp_app_session_request_resize(session, rw, rh);
							present_rdp_canvas(win, session, canvas, rw, rh, &src_buf, &src_cap);
						} else {
							draw_waiting_screen(win, &text, have_text, &adv, canvas, rw, rh);
						}
					}
					continue;
				}
				if (session != NULL) {
					if (ev.input.kind == KL_WINDOW_MOTION) {
						if (cursor_hidden) {
							kl_window_set_cursor_hidden(win, 0);
							cursor_hidden = 0;
						}
						last_motion_ms = dispsink_now_ms();
					}
					handle_rdp_window_event(session, &ev.input, win_w, win_h);
				}
			} else if (ev.kind == KL_APP_POST &&
				   ev.code == DISPSINK_POST_OFFER) {
				struct rdp_app_target target;
				int start_rc;

				if (session != NULL)
					continue;
				logged_first_frame = 0;
				kl_mutex_lock(&adv.lock);
				target = adv.offer_target;
				kl_mutex_unlock(&adv.lock);
				adv_stop(&adv);
				dispsink_log("ui: starting rdp_app_session_start %s:%u", target.host, (unsigned)target.port);
				start_rc = rdp_app_session_start(app, win, &target, &session);
				dispsink_log("ui: rdp_app_session_start rc=%d", start_rc);
				OPENSSL_cleanse(target.password, sizeof(target.password));
				if (start_rc != 0) {
					session = NULL;
					if (adv_start(&adv) == 0)
						draw_waiting_screen(win, &text, have_text, &adv, canvas, win_w, win_h);
					else
						running = 0;
				} else {
					kl_window_set_cursor_hidden(win, 1);
					cursor_hidden = 1;
				}
			} else if (ev.kind == KL_APP_POST &&
				   ev.code == RDP_APP_POST_FRAME) {
				if (session != NULL) {
					present_rdp_canvas(win, session, canvas, win_w, win_h, &src_buf, &src_cap);
					if (logged_first_frame < 3) {
						uint32_t cx = win_w / 2U;
						uint32_t cy = win_h / 2U;
						uint32_t px = canvas[(size_t)cy * win_w + cx];
						logged_first_frame++;
						dispsink_log("ui: frame %d presented %ux%u center=0x%08x",
						    logged_first_frame, win_w, win_h, px);
					}
				}
			} else if (ev.kind == KL_APP_POST &&
				   ev.code == RDP_APP_POST_END) {
				dispsink_log("ui: RDP_APP_POST_END");
				if (cursor_hidden) {
					kl_window_set_cursor_hidden(win, 0);
					cursor_hidden = 0;
				}
				if (session != NULL) {
					rdp_app_session_stop(session);
					session = NULL;
				}
				if (adv_start(&adv) == 0)
					draw_waiting_screen(win, &text, have_text, &adv, canvas, win_w, win_h);
				else
					running = 0;
			}
		}
	}
	}

	if (cursor_hidden)
		kl_window_set_cursor_hidden(win, 0);
	if (session != NULL)
		rdp_app_session_stop(session);
	adv_stop(&adv);
	free(src_buf);
	free(canvas);
	if (have_text)
		kl_text_close(&text);
	kl_window_close(win);
	kl_app_close(app);
	kl_mutex_destroy(&adv.lock);
	return 0;
}
