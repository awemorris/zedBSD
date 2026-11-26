/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Native zedBSD RDP client declarations: CLI parsing, input mapping, and
 * background protocol/media-decoder session management.
 */

#ifndef ZEDBSD_RDP_CLIENT_H
#define ZEDBSD_RDP_CLIENT_H

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* Application post code sent from the RDP worker thread to the main window. */
#define RDP_APP_POST_FRAME	0x72647066U
#define RDP_APP_POST_END	0x72647065U

/*
 * Connection target and initial geometry parsed from command-line arguments.
 */
struct rdp_app_target {
	char host[256];
	uint16_t port;
	char user[128];
	char domain[128];
	char password[128];
	uint32_t width;
	uint32_t height;
};

struct rdp_app_session;

int rdp_parse_args(int argc, char **argv, struct rdp_app_target *target);
int rdp_evdev_to_scancode(uint32_t evdev_code, uint16_t *scancode,
			  uint16_t *flags);

int rdp_app_session_start(struct kl_app *app, struct kl_window *window,
			  const struct rdp_app_target *target,
			  struct rdp_app_session **out);
void rdp_app_session_stop(struct rdp_app_session *session);
void rdp_app_session_send_key(struct rdp_app_session *session,
			      uint16_t scancode, uint16_t flags);
void rdp_app_session_send_unicode(struct rdp_app_session *session,
				  uint16_t codepoint, uint16_t flags);
void rdp_app_session_send_mouse(struct rdp_app_session *session,
				uint16_t flags, uint16_t x, uint16_t y);
void rdp_app_session_request_resize(struct rdp_app_session *session,
				    uint32_t width, uint32_t height);
int rdp_app_session_copy_frame(struct rdp_app_session *session,
			       uint32_t *pixels, uint32_t width,
			       uint32_t height);
int rdp_app_session_frame_size(struct rdp_app_session *session,
			       uint32_t *width, uint32_t *height);

#endif
