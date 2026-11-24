/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An RDP remote desktop tab for mikuterm implementing terminal_view_ops.
 */

#ifndef MIKUTERM_RDP_TAB_H
#define MIKUTERM_RDP_TAB_H

#include "userland/desktop/terminal/connection.h"

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* Maximum number of queued user input events waiting for the RDP thread. */
#define RDP_TAB_INPUTS		128U

/* Post code used to wake the UI thread when the RDP worker updates pixels. */
#define RDP_TAB_POST		0x72647074U

/* Queued input event kinds sent from UI thread to RDP worker thread. */
#define RDP_TAB_IN_KEY		1U
#define RDP_TAB_IN_UNICODE	2U
#define RDP_TAB_IN_MOUSE	3U
#define RDP_TAB_IN_SYNC		4U

/*
 * One queued user input event for the RDP worker thread.
 */
struct rdp_tab_input {
	unsigned kind;
	uint16_t code;
	uint16_t flags;
	uint16_t x;
	uint16_t y;
};

/*
 * One RDP session tab shared between the terminal UI thread and the
 * RDP network/decoder thread.
 */
struct rdp_tab {
	struct terminal_connection connection;
	struct kl_mutex lock;
	unsigned references;
	struct kl_app *app;
	uint32_t post_id;
	int posted;
	struct kl_socket_waker *waker;
	char host[256];
	char title[256];
	char status_text[256];
	uint32_t *pixels;
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t view_width;
	uint32_t view_height;
	int has_frame;
	int news;
	int closed;
	int ended;
	int refused;
	int resize_enabled;
	uint32_t wanted_width;
	uint32_t wanted_height;
	int wanted_resize;
	struct rdp_tab_input inputs[RDP_TAB_INPUTS];
	unsigned input_count;
	double wheel_y;
};

int rdp_tab_create(const char *host, uint32_t width, uint32_t height,
		   int resize_enabled, struct kl_app *app, uint32_t post_id,
		   struct rdp_tab **out);
void rdp_tab_attach_waker(struct rdp_tab *tab, struct kl_socket_waker *waker);
void rdp_tab_release(struct rdp_tab *tab);
void rdp_tab_set_status(struct rdp_tab *tab, const char *status);
int rdp_tab_put_frame(struct rdp_tab *tab, const uint32_t *pixels,
		      uint32_t width, uint32_t height);
void rdp_tab_end(struct rdp_tab *tab, const char *reason);
int rdp_tab_take_input(struct rdp_tab *tab, struct rdp_tab_input *out);
int rdp_tab_take_resize(struct rdp_tab *tab, uint32_t *width, uint32_t *height);
int rdp_tab_is_closing(struct rdp_tab *tab);
int rdp_tab_evdev_to_scancode(uint32_t evdev_code, uint16_t *scancode,
			      uint16_t *flags);

#endif
