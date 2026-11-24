/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * terminal_view_ops implementation for mikuterm's RDP remote desktop tab.
 */

#include "rdp-tab.h"

#include "userland/desktop/librdp/rdp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED_PARAMETER(x)	((void)(x))

/* Background colour painted around or before the remote desktop surface. */
#define RDP_TAB_BG		0xFF181C20U

/* Evdev pointer button codes matching libkeiland. */
#define EVDEV_BTN_LEFT		0x110U
#define EVDEV_BTN_RIGHT		0x111U
#define EVDEV_BTN_MIDDLE	0x112U

/* Distance per wheel notch in libkeiland pointer axis units. */
#define RDP_WHEEL_NOTCH		60.0

static size_t rdp_conn_read(struct terminal_connection *conn, unsigned char *buf, size_t size);
static int rdp_conn_write(struct terminal_connection *conn, const unsigned char *bytes, size_t size, size_t *written);
static int rdp_conn_resize(struct terminal_connection *conn, unsigned cols, unsigned rows, unsigned w, unsigned h);
static int rdp_conn_mode(const struct terminal_connection *conn, unsigned *mode);
static unsigned rdp_conn_event(struct terminal_connection *conn, const struct kl_app_event *event);
static unsigned rdp_conn_pending(const struct terminal_connection *conn);
static void rdp_conn_close(struct terminal_connection *conn);
static int rdp_view_draw(struct terminal_connection *conn, const struct terminal_view_state *state, uint32_t *pixels, uint32_t width, uint32_t height);
static void rdp_view_title(struct terminal_connection *conn, char *title, size_t size);
static void rdp_view_input(struct terminal_connection *conn, const struct terminal_picture_event *event);
static unsigned rdp_view_drop(struct terminal_connection *conn, struct terminal_window *window, const struct terminal_drop_event *event);
static void rdp_view_shown(struct terminal_connection *conn, struct terminal_window *window, int visible);
static int rdp_view_confirm_close(struct terminal_connection *conn, struct terminal_window *window);
static int rdp_view_can_copy(const struct terminal_connection *conn);
static size_t rdp_view_copy(struct terminal_connection *conn, char *text, size_t size);
static int rdp_view_can_paste(const struct terminal_connection *conn);
static void rdp_view_paste(struct terminal_connection *conn, const char *text, size_t length);
static void rdp_view_refuse(struct terminal_connection *conn);
static void tab_wake_ui(struct rdp_tab *tab);
static void tab_wake_worker(struct rdp_tab *tab);
static void tab_queue_input(struct rdp_tab *tab, unsigned kind, uint16_t code, uint16_t flags, uint16_t x, uint16_t y);
static uint32_t decode_utf8_one(const char *s, size_t len, size_t *step);

/* View operations table for RDP tabs. */
static const struct terminal_view_ops rdp_view_ops = {
	rdp_view_draw,
	rdp_view_title,
	rdp_view_input,
	rdp_view_drop,
	rdp_view_shown,
	rdp_view_confirm_close,
	rdp_view_can_copy,
	rdp_view_copy,
	rdp_view_can_paste,
	rdp_view_paste,
	rdp_view_refuse
};

/* Terminal connection operations table for RDP tabs. */
static const struct terminal_connection_ops rdp_conn_ops = {
	rdp_conn_read,
	rdp_conn_write,
	rdp_conn_resize,
	rdp_conn_mode,
	rdp_conn_event,
	rdp_conn_pending,
	rdp_conn_close,
	NULL,
	NULL,
	&rdp_view_ops
};

/*
 * Allocates and initializes an RDP view tab for host.
 */
int
rdp_tab_create(
	const char *host,
	uint32_t width,
	uint32_t height,
	int resize_enabled,
	struct kl_app *app,
	uint32_t post_id,
	struct rdp_tab **out)
{
	struct rdp_tab *tab;
	int error;
	if (host == NULL || out == NULL)
		return EINVAL;
	*out = NULL;
	tab = (struct rdp_tab *)calloc(1U, sizeof(*tab));
	if (tab == NULL)
		return ENOMEM;
	error = kl_mutex_init(&tab->lock);
	if (error != 0) {
		free(tab);
		return error;
	}
	tab->connection.ops = &rdp_conn_ops;
	tab->references = 1U;
	tab->app = app;
	tab->post_id = post_id;
	tab->resize_enabled = resize_enabled;
	tab->fb_width = width > 0U ? width : 1280U;
	tab->fb_height = height > 0U ? height : 720U;
	snprintf(tab->host, sizeof(tab->host), "%s", host);
	snprintf(tab->title, sizeof(tab->title), "RDP: %.240s", host);
	snprintf(tab->status_text, sizeof(tab->status_text),
		 "Connecting to %.220s...", host);
	*out = tab;

	/* Succeeded: RDP tab created. */
	return 0;
}

/*
 * Attaches the worker thread's socket waker and increments reference count.
 */
void
rdp_tab_attach_waker(
	struct rdp_tab *tab,
	struct kl_socket_waker *waker)
{
	kl_mutex_lock(&tab->lock);
	tab->waker = waker;
	tab->references++;
	kl_mutex_unlock(&tab->lock);
}

/*
 * Releases one reference to tab and frees it when the last holder exits.
 */
void
rdp_tab_release(
	struct rdp_tab *tab)
{
	unsigned refs;
	if (tab == NULL)
		return;
	kl_mutex_lock(&tab->lock);
	refs = --tab->references;
	kl_mutex_unlock(&tab->lock);
	if (refs == 0U) {
		kl_mutex_destroy(&tab->lock);
		free(tab->pixels);
		free(tab);
	}
}

/*
 * Updates the tab status text and wakes the UI thread.
 */
void
rdp_tab_set_status(
	struct rdp_tab *tab,
	const char *status)
{
	kl_mutex_lock(&tab->lock);
	snprintf(tab->status_text, sizeof(tab->status_text), "%s",
		 status != NULL ? status : "");
	snprintf(tab->title, sizeof(tab->title), "RDP: %.240s", tab->host);
	tab->news = 1;
	kl_mutex_unlock(&tab->lock);
	tab_wake_ui(tab);
}

/*
 * Copies a decoded 0xAARRGGBB frame into the tab's front buffer and wakes UI.
 */
int
rdp_tab_put_frame(
	struct rdp_tab *tab,
	const uint32_t *pixels,
	uint32_t width,
	uint32_t height)
{
	size_t count;
	uint32_t *buf;
	if (tab == NULL || pixels == NULL || width == 0U || height == 0U)
		return EINVAL;
	count = (size_t)width * (size_t)height;
	kl_mutex_lock(&tab->lock);
	if (tab->pixels == NULL || tab->fb_width != width ||
	    tab->fb_height != height) {
		buf = (uint32_t *)realloc(tab->pixels,
					  count * sizeof(*tab->pixels));
		if (buf == NULL) {
			kl_mutex_unlock(&tab->lock);
			return ENOMEM;
		}
		tab->pixels = buf;
		tab->fb_width = width;
		tab->fb_height = height;
	}
	memcpy(tab->pixels, pixels, count * sizeof(*tab->pixels));
	tab->has_frame = 1;
	tab->news = 1;
	kl_mutex_unlock(&tab->lock);
	tab_wake_ui(tab);

	/* Succeeded: decoded frame published to the UI tab. */
	return 0;
}

/*
 * Marks the RDP session as ended and wakes the UI thread.
 */
void
rdp_tab_end(
	struct rdp_tab *tab,
	const char *reason)
{
	kl_mutex_lock(&tab->lock);
	if (reason != NULL && reason[0] != '\0') {
		snprintf(tab->status_text, sizeof(tab->status_text), "%s",
			 reason);
	}
	tab->ended = 1;
	tab->news = 1;
	kl_mutex_unlock(&tab->lock);
	tab_wake_ui(tab);
}

/*
 * Pops one queued input event for the worker thread, returning 1 if present.
 */
int
rdp_tab_take_input(
	struct rdp_tab *tab,
	struct rdp_tab_input *out)
{
	int got;
	kl_mutex_lock(&tab->lock);
	got = 0;
	if (tab->input_count > 0U) {
		*out = tab->inputs[0];
		tab->input_count--;
		if (tab->input_count > 0U) {
			memmove(&tab->inputs[0], &tab->inputs[1],
				tab->input_count * sizeof(tab->inputs[0]));
		}
		got = 1;
	}

	kl_mutex_unlock(&tab->lock);
	return got;
}

/*
 * Pops a pending resize request for the worker thread, returning 1 if present.
 */
int
rdp_tab_take_resize(
	struct rdp_tab *tab,
	uint32_t *width,
	uint32_t *height)
{
	int got;
	kl_mutex_lock(&tab->lock);
	got = tab->wanted_resize;
	if (got != 0) {
		*width = tab->wanted_width;
		*height = tab->wanted_height;
		tab->wanted_resize = 0;
	}

	kl_mutex_unlock(&tab->lock);
	return got;
}

/*
 * Returns nonzero when the UI tab has closed or been refused.
 */
int
rdp_tab_is_closing(
	struct rdp_tab *tab)
{
	int closing;
	kl_mutex_lock(&tab->lock);
	closing = tab->closed || tab->refused;
	kl_mutex_unlock(&tab->lock);
	return closing;
}

/*
 * Maps an evdev keycode to a PS/2 Set 1 scancode and RDP key flags.
 */
int
rdp_tab_evdev_to_scancode(
	uint32_t evdev_code,
	uint16_t *scancode,
	uint16_t *flags)
{
	static const struct {
		uint32_t evdev;
		uint16_t scan;
		uint16_t ext;
	} ext_map[] = {
		{ 96U, 0x1CU, RDP_KBD_FLAG_EXTENDED },
		{ 97U, 0x1DU, RDP_KBD_FLAG_EXTENDED },
		{ 98U, 0x35U, RDP_KBD_FLAG_EXTENDED },
		{ 100U, 0x38U, RDP_KBD_FLAG_EXTENDED },
		{ 102U, 0x47U, RDP_KBD_FLAG_EXTENDED },
		{ 103U, 0x48U, RDP_KBD_FLAG_EXTENDED },
		{ 104U, 0x49U, RDP_KBD_FLAG_EXTENDED },
		{ 105U, 0x4BU, RDP_KBD_FLAG_EXTENDED },
		{ 106U, 0x4DU, RDP_KBD_FLAG_EXTENDED },
		{ 107U, 0x4FU, RDP_KBD_FLAG_EXTENDED },
		{ 108U, 0x50U, RDP_KBD_FLAG_EXTENDED },
		{ 109U, 0x51U, RDP_KBD_FLAG_EXTENDED },
		{ 110U, 0x52U, RDP_KBD_FLAG_EXTENDED },
		{ 111U, 0x53U, RDP_KBD_FLAG_EXTENDED },
		{ 125U, 0x5BU, RDP_KBD_FLAG_EXTENDED },
		{ 126U, 0x5CU, RDP_KBD_FLAG_EXTENDED },
		{ 127U, 0x5DU, RDP_KBD_FLAG_EXTENDED }
	};
	size_t i;
	*flags = 0U;
	if (evdev_code >= 1U && evdev_code <= 88U) {
		*scancode = (uint16_t)evdev_code;
		return 1;
	}
	for (i = 0U; i < sizeof(ext_map) / sizeof(ext_map[0]); i++) {
		if (ext_map[i].evdev == evdev_code) {
			*scancode = ext_map[i].scan;
			*flags = ext_map[i].ext;
			return 1;
		}
	}
	return 0;
}

/* Consumes pending output notification (view tabs draw directly). */
static size_t
rdp_conn_read(
	struct terminal_connection *conn,
	unsigned char *buf,
	size_t size)
{
	struct rdp_tab *tab;

	UNUSED_PARAMETER(buf);
	UNUSED_PARAMETER(size);
	tab = (struct rdp_tab *)conn;
	kl_mutex_lock(&tab->lock);
	tab->news = 0;
	kl_mutex_unlock(&tab->lock);
	return 0U;
}

/* Rejects byte-stream terminal writes on a view tab. */
static int
rdp_conn_write(
	struct terminal_connection *conn,
	const unsigned char *bytes,
	size_t size,
	size_t *written)
{
	UNUSED_PARAMETER(conn);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(size);
	*written = 0U;
	return 0;
}

/* Queues an MS-RDPEDISP resize request when the tab size changes. */
static int
rdp_conn_resize(
	struct terminal_connection *conn,
	unsigned cols,
	unsigned rows,
	unsigned w,
	unsigned h)
{
	struct rdp_tab *tab;

	UNUSED_PARAMETER(cols);
	UNUSED_PARAMETER(rows);
	tab = (struct rdp_tab *)conn;
	if (w == 0U || h == 0U)
		return 0;
	kl_mutex_lock(&tab->lock);
	if (tab->resize_enabled != 0 &&
	    (w != tab->fb_width || h != tab->fb_height)) {
		tab->wanted_width = (w + 1U) & ~1U;
		tab->wanted_height = (h + 1U) & ~1U;
		tab->wanted_resize = 1;
	}

	kl_mutex_unlock(&tab->lock);
	tab_wake_worker(tab);
	return 0;
}

/* Reports non-line-mode input for a view tab. */
static int
rdp_conn_mode(
	const struct terminal_connection *conn,
	unsigned *mode)
{
	UNUSED_PARAMETER(conn);
	*mode = 0U;
	return 0;
}

/* Checks whether a wakeup post belongs to this RDP tab. */
static unsigned
rdp_conn_event(
	struct terminal_connection *conn,
	const struct kl_app_event *event)
{
	struct rdp_tab *tab;
	unsigned ready;
	tab = (struct rdp_tab *)conn;
	if (event->kind != KL_APP_POST || event->code != RDP_TAB_POST)
		return 0U;
	if ((uintptr_t)event->data != (uintptr_t)tab->post_id)
		return 0U;
	kl_mutex_lock(&tab->lock);
	tab->posted = 0;
	ready = 0U;
	if (tab->news != 0)
		ready |= TERMINAL_READY_OUTPUT;
	if (tab->ended != 0)
		ready |= TERMINAL_READY_EXIT;
	tab->news = 0;
	kl_mutex_unlock(&tab->lock);
	return ready;
}

/* Reports pending output or exit status without waiting for a post. */
static unsigned
rdp_conn_pending(
	const struct terminal_connection *conn)
{
	struct rdp_tab *tab;
	unsigned ready;
	tab = (struct rdp_tab *)conn;
	kl_mutex_lock(&tab->lock);
	ready = 0U;
	if (tab->news != 0)
		ready |= TERMINAL_READY_OUTPUT;
	if (tab->ended != 0)
		ready |= TERMINAL_READY_EXIT;

	kl_mutex_unlock(&tab->lock);
	return ready;
}

/* Closes the UI side of the RDP tab and signals the worker thread. */
static void
rdp_conn_close(
	struct terminal_connection *conn)
{
	struct rdp_tab *tab;
	tab = (struct rdp_tab *)conn;
	kl_mutex_lock(&tab->lock);
	tab->closed = 1;
	kl_mutex_unlock(&tab->lock);
	tab_wake_worker(tab);
	rdp_tab_release(tab);
}

/* Draws the latest decoded RDP frame into pixels (0xAARRGGBB). */
static int
rdp_view_draw(
	struct terminal_connection *conn,
	const struct terminal_view_state *state,
	uint32_t *pixels,
	uint32_t width,
	uint32_t height)
{
	struct rdp_tab *tab;
	uint32_t y;
	uint32_t copy_w;
	uint32_t copy_h;
	size_t total;
	size_t i;

	UNUSED_PARAMETER(state);
	tab = (struct rdp_tab *)conn;
	total = (size_t)width * (size_t)height;
	for (i = 0U; i < total; i++)
		pixels[i] = RDP_TAB_BG;
	kl_mutex_lock(&tab->lock);
	if (tab->has_frame != 0 && tab->pixels != NULL) {
		copy_w = tab->fb_width < width ? tab->fb_width : width;
		copy_h = tab->fb_height < height ? tab->fb_height : height;
		for (y = 0U; y < copy_h; y++) {
			memcpy(pixels + (size_t)y * width,
			       tab->pixels + (size_t)y * tab->fb_width,
			       (size_t)copy_w * sizeof(*pixels));
		}
	}

	kl_mutex_unlock(&tab->lock);
	return 0;
}

/* Writes the RDP tab's current title. */
static void
rdp_view_title(
	struct terminal_connection *conn,
	char *title,
	size_t size)
{
	struct rdp_tab *tab;
	tab = (struct rdp_tab *)conn;
	kl_mutex_lock(&tab->lock);
	snprintf(title, size, "%s", tab->title);
	kl_mutex_unlock(&tab->lock);
}

/* Converts a terminal_picture_event into queued RDP Fast-Path inputs. */
static void
rdp_view_input(
	struct terminal_connection *conn,
	const struct terminal_picture_event *event)
{
	struct rdp_tab *tab;
	uint16_t scan;
	uint16_t flags;
	uint16_t btn_flag;
	uint16_t px;
	uint16_t py;
	size_t pos;
	size_t len;
	size_t step;
	uint32_t cp;
	int notches;
	int i;
	tab = (struct rdp_tab *)conn;
	if (event->kind == TERMINAL_PICTURE_KEY) {
		if (rdp_tab_evdev_to_scancode(event->code, &scan, &flags) != 0) {
			if (event->pressed == 0)
				flags |= RDP_KBD_FLAG_RELEASE;
			tab_queue_input(tab, RDP_TAB_IN_KEY, scan, flags, 0U, 0U);
		}
		return;
	}
	if (event->kind == TERMINAL_PICTURE_TEXT) {
		len = strlen(event->text);
		pos = 0U;
		while (pos < len) {
			cp = decode_utf8_one(event->text + pos, len - pos, &step);
			pos += step;
			if (cp > 0U && cp <= 0xFFFFU) {
				tab_queue_input(tab, RDP_TAB_IN_UNICODE,
						(uint16_t)cp, 0U, 0U, 0U);
				tab_queue_input(tab, RDP_TAB_IN_UNICODE,
						(uint16_t)cp,
						RDP_KBD_FLAG_RELEASE, 0U, 0U);
			}
		}
		return;
	}
	px = event->x > 0 ? (uint16_t)event->x : 0U;
	py = event->y > 0 ? (uint16_t)event->y : 0U;
	if (event->kind == TERMINAL_PICTURE_MOTION) {
		tab_queue_input(tab, RDP_TAB_IN_MOUSE, 0U, RDP_PTR_FLAG_MOVE,
				px, py);
	} else if (event->kind == TERMINAL_PICTURE_BUTTON) {
		btn_flag = 0U;
		if (event->code == EVDEV_BTN_LEFT)
			btn_flag = RDP_PTR_FLAG_BUTTON1;
		else if (event->code == EVDEV_BTN_RIGHT)
			btn_flag = RDP_PTR_FLAG_BUTTON2;
		else if (event->code == EVDEV_BTN_MIDDLE)
			btn_flag = RDP_PTR_FLAG_BUTTON3;
		if (btn_flag != 0U) {
			if (event->pressed != 0)
				btn_flag |= RDP_PTR_FLAG_DOWN;
			tab_queue_input(tab, RDP_TAB_IN_MOUSE, 0U, btn_flag,
					px, py);
		}
	} else if (event->kind == TERMINAL_PICTURE_AXIS) {
		tab->wheel_y += event->dy;
		notches = (int)(tab->wheel_y / RDP_WHEEL_NOTCH);
		tab->wheel_y -= (double)notches * RDP_WHEEL_NOTCH;
		for (i = 0; i < (notches < 0 ? -notches : notches); i++) {
			flags = RDP_PTR_FLAG_WHEEL;
			if (notches > 0)
				flags |= (uint16_t)(120U & 0x01FFU);
			else
				flags |= (uint16_t)(RDP_PTR_FLAG_WHEEL_NEGATIVE |
						    ((-120) & 0x00FFU));
			tab_queue_input(tab, RDP_TAB_IN_MOUSE, 0U, flags, px, py);
		}
	}
}

/* Rejects file drops onto the RDP view. */
static unsigned
rdp_view_drop(
	struct terminal_connection *conn,
	struct terminal_window *window,
	const struct terminal_drop_event *event)
{
	UNUSED_PARAMETER(conn);
	UNUSED_PARAMETER(window);
	UNUSED_PARAMETER(event);
	return 0U;
}

/* Handles tab visibility changes. */
static void
rdp_view_shown(
	struct terminal_connection *conn,
	struct terminal_window *window,
	int visible)
{
	UNUSED_PARAMETER(conn);
	UNUSED_PARAMETER(window);
	UNUSED_PARAMETER(visible);
}

/* Allows closing the RDP tab immediately. */
static int
rdp_view_confirm_close(
	struct terminal_connection *conn,
	struct terminal_window *window)
{
	UNUSED_PARAMETER(conn);
	UNUSED_PARAMETER(window);
	return 1;
}

/* Reports no local text selection copy support yet. */
static int
rdp_view_can_copy(
	const struct terminal_connection *conn)
{
	UNUSED_PARAMETER(conn);
	return 0;
}

/* Copies no text. */
static size_t
rdp_view_copy(
	struct terminal_connection *conn,
	char *text,
	size_t size)
{
	UNUSED_PARAMETER(conn);
	UNUSED_PARAMETER(text);
	UNUSED_PARAMETER(size);
	return 0U;
}

/* Supports pasting UTF-8 text as RDP Unicode keystrokes. */
static int
rdp_view_can_paste(
	const struct terminal_connection *conn)
{
	UNUSED_PARAMETER(conn);
	return 1;
}

/* Sends pasted UTF-8 text as Fast-Path Unicode events. */
static void
rdp_view_paste(
	struct terminal_connection *conn,
	const char *text,
	size_t length)
{
	struct rdp_tab *tab;
	size_t pos;
	size_t step;
	uint32_t cp;
	tab = (struct rdp_tab *)conn;
	pos = 0U;
	while (pos < length) {
		cp = decode_utf8_one(text + pos, length - pos, &step);
		pos += step;
		if (cp > 0U && cp <= 0xFFFFU) {
			tab_queue_input(tab, RDP_TAB_IN_UNICODE,
					(uint16_t)cp, 0U, 0U, 0U);
			tab_queue_input(tab, RDP_TAB_IN_UNICODE,
					(uint16_t)cp, RDP_KBD_FLAG_RELEASE,
					0U, 0U);
		}
	}
}

/* Marks the tab as refused when the terminal has no room for a new tab. */
static void
rdp_view_refuse(
	struct terminal_connection *conn)
{
	struct rdp_tab *tab;
	tab = (struct rdp_tab *)conn;
	kl_mutex_lock(&tab->lock);
	tab->refused = 1;
	kl_mutex_unlock(&tab->lock);
	tab_wake_worker(tab);
}

/* Posts a wakeup event to the UI thread if one is not already pending. */
static void
tab_wake_ui(
	struct rdp_tab *tab)
{
	int should_post;
	kl_mutex_lock(&tab->lock);
	should_post = (tab->app != NULL && tab->posted == 0);
	if (should_post != 0)
		tab->posted = 1;

	kl_mutex_unlock(&tab->lock);
	if (should_post != 0)
		(void)kl_app_post(tab->app, RDP_TAB_POST,
				  (void *)(uintptr_t)tab->post_id);
}

/* Wakes the RDP worker thread's poll loop. */
static void
tab_wake_worker(
	struct rdp_tab *tab)
{
	struct kl_socket_waker *waker;
	kl_mutex_lock(&tab->lock);
	waker = tab->waker;
	kl_mutex_unlock(&tab->lock);
	if (waker != NULL)
		kl_socket_waker_ring(waker);
}

/* Appends one input event to the tab's bounded queue and wakes the worker. */
static void
tab_queue_input(
	struct rdp_tab *tab,
	unsigned kind,
	uint16_t code,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	struct rdp_tab_input *in;
	kl_mutex_lock(&tab->lock);
	if (tab->input_count < RDP_TAB_INPUTS) {
		in = &tab->inputs[tab->input_count++];
		in->kind = kind;
		in->code = code;
		in->flags = flags;
		in->x = x;
		in->y = y;
	}

	kl_mutex_unlock(&tab->lock);
	tab_wake_worker(tab);
}

/* Decodes one UTF-8 codepoint from s[0..len). */
static uint32_t
decode_utf8_one(
	const char *s,
	size_t len,
	size_t *step)
{
	uint8_t b0;
	if (len == 0U) {
		*step = 0U;
		return 0U;
	}
	b0 = (uint8_t)s[0];
	if (b0 < 0x80U) {
		*step = 1U;
		return b0;
	}
	if ((b0 & 0xE0U) == 0xC0U && len >= 2U) {
		*step = 2U;
		return (((uint32_t)(b0 & 0x1FU)) << 6) |
		       ((uint32_t)(s[1] & 0x3FU));
	}
	if ((b0 & 0xF0U) == 0xE0U && len >= 3U) {
		*step = 3U;
		return (((uint32_t)(b0 & 0x0FU)) << 12) |
		       (((uint32_t)(s[1] & 0x3FU)) << 6) |
		       ((uint32_t)(s[2] & 0x3FU));
	}
	if ((b0 & 0xF8U) == 0xF0U && len >= 4U) {
		*step = 4U;
		return 0xFFFDU;
	}
	*step = 1U;
	return 0U;
}
