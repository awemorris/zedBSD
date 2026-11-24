/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Native zedBSD RDP client window (userland/desktop/rdp).
 */

#include "rdp-client.h"

#include "userland/desktop/librdp/rdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default background colour before the first frame arrives. */
#define RDP_WIN_BG		0xFF181C20U

static void handle_window_event(struct rdp_app_session *session, struct kl_window *win, const struct kl_window_event *ev, uint32_t *win_w, uint32_t *win_h, uint32_t **canvas, size_t *canvas_cap, int *running);
static void present_canvas(struct kl_window *win, struct rdp_app_session *session, uint32_t *canvas, uint32_t w, uint32_t h);

/*
 * Entry point for zedBSD's native RDP client.
 */
int
main(
	int argc,
	char **argv)
{
	struct rdp_app_target target;
	struct kl_app_options aopts;
	struct kl_app *app;
	struct kl_window_options wopts;
	struct kl_window *win;
	struct rdp_app_session *session;
	struct kl_app_event ev;
	uint32_t win_w;
	uint32_t win_h;
	uint32_t *canvas;
	size_t canvas_cap;
	int running;
	char title[300];
	int rc;
	rc = rdp_parse_args(argc, argv, &target);
	if (rc != 0) {
		fprintf(stderr,
			"usage: rdp [--width=W] [--height=H] [--domain=D] "
			"[--password=P] [user@]host[:port]\n");
		return 1;
	}

	/* Opens the libkeiland application connection. */
	memset(&aopts, 0, sizeof(aopts));
	aopts.application = "rdp";
	app = kl_app_open(&aopts);
	if (app == NULL) {
		fprintf(stderr, "rdp: kl_app_open failed\n");
		return 1;
	}

	/* Opens the client display window. */
	snprintf(title, sizeof(title), "RDP: %.240s", target.host);
	memset(&wopts, 0, sizeof(wopts));
	wopts.application = "rdp";
	wopts.title = title;
	wopts.width = target.width;
	wopts.height = target.height;
	wopts.present = KL_PRESENT_SHM;
	win = kl_window_open(&wopts);
	if (win == NULL) {
		kl_app_close(app);
		fprintf(stderr, "rdp: kl_window_open failed\n");
		return 1;
	}

	/* Starts the background RDP network and decoder session. */
	rc = rdp_app_session_start(app, win, &target, &session);
	if (rc != 0) {
		kl_window_close(win);
		kl_app_close(app);
		fprintf(stderr, "rdp: failed to start session\n");
		return 1;
	}

	/* Allocates the presentation pixel canvas and runs the UI event loop. */
	win_w = target.width;
	win_h = target.height;
	canvas_cap = (size_t)win_w * (size_t)win_h;
	canvas = (uint32_t *)calloc(canvas_cap, sizeof(*canvas));
	if (canvas != NULL)
		present_canvas(win, session, canvas, win_w, win_h);
	running = 1;
	while (running != 0) {
		rc = kl_app_dispatch(app, -1);
		if (rc != 0)
			break;
		for (;;) {
			rc = kl_app_take(app, &ev);
			if (rc == 0)
				break;
			if (ev.kind == KL_APP_WINDOW && ev.window == win) {
				handle_window_event(session, win, &ev.input,
						    &win_w, &win_h, &canvas,
						    &canvas_cap, &running);
			} else if (ev.kind == KL_APP_POST &&
				   ev.code == RDP_APP_POST_FRAME) {
				if (canvas != NULL) {
					present_canvas(win, session, canvas,
						       win_w, win_h);
				}
			} else if (ev.kind == KL_APP_POST &&
				   ev.code == RDP_APP_POST_END) {
				running = 0;
			}
		}
	}

	/* Stops the RDP session and releases window resources. */
	rdp_app_session_stop(session);
	free(canvas);
	kl_window_close(win);
	kl_app_close(app);

	/* Succeeded: RDP client exited cleanly. */
	return 0;
}

/* Dispatches one libkeiland window event to the RDP session. */
static void
handle_window_event(
	struct rdp_app_session *session,
	struct kl_window *win,
	const struct kl_window_event *ev,
	uint32_t *win_w,
	uint32_t *win_h,
	uint32_t **canvas,
	size_t *canvas_cap,
	int *running)
{
	uint16_t scan;
	uint16_t flags;
	uint16_t btn_flag;
	uint16_t px;
	uint16_t py;
	uint32_t rw;
	uint32_t rh;
	size_t need;
	uint32_t *nb;
	int rsz_rc;
	int key_ok;
	if (ev->kind == KL_WINDOW_CLOSE) {
		*running = 0;
		return;
	}

	/* Handles window resize and requests RDPEDISP resize. */
	if (ev->kind == KL_WINDOW_RESIZE) {
		rw = *win_w;
		rh = *win_h;
		rsz_rc = kl_window_present_resize(win, &rw, &rh);
		if (rsz_rc == 0 && rw > 0U && rh > 0U) {
			*win_w = rw;
			*win_h = rh;
			need = (size_t)rw * (size_t)rh;
			if (need > *canvas_cap) {
				nb = (uint32_t *)realloc(
					*canvas, need * sizeof(**canvas));
				if (nb != NULL) {
					*canvas = nb;
					*canvas_cap = need;
				}
			}

			/* Notifies the server and redraws the resized canvas. */
			rdp_app_session_request_resize(session, rw, rh);
			if (*canvas != NULL)
				present_canvas(win, session, *canvas, rw, rh);
		}

		/* Resize handled. */
		return;
	}

	/* Handles keyboard press/release events. */
	if (ev->kind == KL_WINDOW_KEY) {
		key_ok = rdp_evdev_to_scancode(ev->code, &scan, &flags);
		if (key_ok != 0) {
			if (ev->pressed == 0)
				flags |= RDP_KBD_FLAG_RELEASE;
			rdp_app_session_send_key(session, scan, flags);
		}

		/* Key handled. */
		return;
	}

	/* Clamps pointer coordinates to non-negative values. */
	px = 0U;
	if (ev->x > 0.0)
		px = (uint16_t)ev->x;
	py = 0U;
	if (ev->y > 0.0)
		py = (uint16_t)ev->y;
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

		/* Sends the wheel event to the RDP server. */
		rdp_app_session_send_mouse(session, flags, px, py);
	}
}

/* Copies the latest decoded frame into canvas and presents it on win. */
static void
present_canvas(
	struct kl_window *win,
	struct rdp_app_session *session,
	uint32_t *canvas,
	uint32_t w,
	uint32_t h)
{
	size_t i;
	size_t total;
	total = (size_t)w * (size_t)h;
	for (i = 0U; i < total; i++)
		canvas[i] = RDP_WIN_BG;
	(void)rdp_app_session_copy_frame(session, canvas, w, h);
	(void)kl_window_present(win, canvas, (size_t)w * 4U);
}
