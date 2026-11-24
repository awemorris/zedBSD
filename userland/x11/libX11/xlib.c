/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Minimal Xlib-compatible client library for Xzed.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "userland/x11/libX11/Xzed.h"
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
struct _XGC {
	uint32_t xid, foreground;
};
#define EVENT_QUEUE_SIZE 32U

/* The largest request, in bytes (its length field counts 65535 words). */
#define XZED_REQUEST_BYTES (65535U * 4U)

/* The keyboard's requests, and MappingNotify. */
#define KEYMAP_GET_KEYBOARD_MAPPING	101U
#define KEYMAP_GET_MODIFIER_MAPPING	119U
#define KEYMAP_MAPPING_NOTIFY		34

/* The core protocol's modifiers: Shift, Lock, and the first of Mod1 to Mod5. */
#define KEYMAP_MODIFIERS		8U
#define KEYMAP_LOCK			1U

/* The keysyms that name a modifier's meaning, and the keypad's keysyms. */
#define KEYMAP_MODE_SWITCH		0xff7eU
#define KEYMAP_NUM_LOCK			0xff7fU
#define KEYMAP_CAPS_LOCK		0xffe5U
#define KEYMAP_SHIFT_LOCK		0xffe6U
#define KEYMAP_KEYPAD_FIRST		0xff80U
#define KEYMAP_KEYPAD_LAST		0xffbdU

/* What the Lock modifier does: nothing, Caps Lock (letters), Shift Lock (every key). */
#define KEYMAP_LOCK_NONE		0
#define KEYMAP_LOCK_CAPS		1
#define KEYMAP_LOCK_SHIFT		2

/*
 * A connection: its socket, the ids it chooses, the root window, the
 * sequence number of its last request and the events read ahead of a
 * reply.  The keyboard's map, read when the connection opens and again
 * after a MappingNotify: the keycodes the server has, the keysyms of each
 * (keysyms_per_keycode a keycode; NULL when the server gave none), and
 * the modifier bits that are Mode_switch and Num_Lock and what Lock does.
 */
struct _XDisplay {
	int fd;
	uint32_t base, next, root;
	unsigned long black, white;
	uint16_t sequence;
	uint8_t events[EVENT_QUEUE_SIZE][32];
	unsigned event_head, event_count;
	uint8_t min_keycode;
	uint8_t max_keycode;
	unsigned keysyms_per_keycode;
	uint32_t *keysyms;
	unsigned mode_switch_mask;
	unsigned num_lock_mask;
	int lock_meaning;
	int keymap_stale;
};

static void w16(uint8_t *p, uint16_t v);
static int wr(int f, const void *v, size_t n);
static int rd(int f, void *v, size_t n);
static uint16_t r16(const uint8_t *p);
static uint32_t r32(const uint8_t *p);
static void w32(uint8_t *p, uint32_t v);
static int req(Display *d, uint8_t *q, size_t n);
static int reply(Display *d, uint8_t *b);
static int queue_event(Display *d, const uint8_t *b);
static int winreq(Display *d, uint8_t op, Window w);
static int store_string(Display *d, Window w, Atom property, const char *n);
static int fetch_string(Display *d, Window w, Atom property, char **value);
static int text_req(Display *d, uint8_t op, Drawable w, GC g, int x, int y, const void *s, int n, int wide);
static void event(Display *d, const uint8_t *b, XEvent *e);
static void keymap_load(Display *d);
static int keymap_read_keysyms(Display *d);
static int keymap_read_modifiers(Display *d);
static uint32_t keymap_keysym(const Display *d, unsigned keycode, unsigned column);
static void keymap_case(uint32_t keysym, uint32_t *lower, uint32_t *upper);
static int keymap_discard(Display *d, size_t bytes);

/*
 * Implements the XOpenDisplay operation.
 */
Display *
XOpenDisplay(
	const char *n)
{
	struct sockaddr_un a;
	uint8_t q[12] = {0}, h[8], b[4096];
	uint16_t z;
	size_t screen;
	Display *d;

	(void)n;
	d = calloc(1, sizeof(*d));

	/* Checks the current descriptor. */
	if (!d)
		return 0;
	d->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	/* Checks the current descriptor. */
	if (d->fd < 0)
		goto bad;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, "/tmp/.X11-unix/X0");

	/* Handles a failed connect operation. */
	if (connect(d->fd, (struct sockaddr *)&a, sizeof(a)))
		goto bad;
	q[0] = 'l';
	w16(q + 2, 11);

	/* Handles a failed wr operation. */
	if (wr(d->fd, q, 12) || rd(d->fd, h, 8) || h[0] != 1)
		goto bad;
	z = r16(h + 6);

	/* Handles a failed rd operation. */
	if (z * 4U > sizeof(b) || rd(d->fd, b, z * 4U))
		goto bad;
	d->base = r32(b + 4);
	d->next = 1;

	/*
	 * The first screen follows the fixed part, the vendor (padded to a
	 * word) and the pixmap formats (8 bytes each); its first word is the
	 * root window.
	 */
	screen = 32U + (((size_t)r16(b + 16) + 3U) & ~(size_t)3U) + 8U * (size_t)b[21];
	if (screen + 4U > z * 4U)
		goto bad;
	d->root = r32(b + screen);
	d->white = 0xffffff;

	/* The keycodes the server has, and its keyboard's map. */
	d->min_keycode = b[26];
	d->max_keycode = b[27];
	keymap_load(d);

	/* Returns the computed result. */
	return d;
bad:

	/* Checks the current descriptor. */
	if (d->fd >= 0)
		close(d->fd);
	free(d);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the XCloseDisplay operation.
 */
int
XCloseDisplay(
	Display *d)
{
	/* Checks the current descriptor. */
	if (!d)
		return 0;
	close(d->fd);
	free(d->keysyms);
	free(d);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the XDefaultScreen operation.
 */
int
XDefaultScreen(
	Display *d)
{
	(void)d;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the XConnectionNumber operation.
 */
int
XConnectionNumber(
	Display *d)
{
	/* Returns the computed result. */
	return d ? d->fd : -1;
}

/*
 * Implements the XRootWindow operation.
 */
Window
XRootWindow(
	Display *d,
	int s)
{
	(void)s;

	/* Returns the computed result. */
	return d->root;
}

/*
 * Implements the XBlackPixel operation.
 */
unsigned long
XBlackPixel(
	Display *d,
	int s)
{
	(void)d;
	(void)s;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the XWhitePixel operation.
 */
unsigned long
XWhitePixel(
	Display *d,
	int s)
{
	(void)d;
	(void)s;

	/* Returns the computed result. */
	return 0xffffff;
}

/*
 * Implements the XLoadFont operation.
 */
Font
XLoadFont(
	Display *d,
	const char *n)
{
	size_t l, z;
	uint8_t *q;

	Font f;

	l = strlen(n);
	z = (12 + l + 3) & ~3U;
	q = calloc(1, z);

	/* Handles the q condition. */
	if (!q)
		return 0;
	f = d->base | d->next++;
	q[0] = 45;
	w32(q + 4, f);
	w16(q + 8, (uint16_t)l);
	memcpy(q + 12, n, l);

	/* Handles the req condition. */
	if (req(d, q, z)) {
		free(q);

		/* Reports successful completion. */
		return 0;
	}
	free(q);

	/* Returns the computed result. */
	return f;
}

/*
 * Implements the XLoadQueryFont operation.
 */
XFontStruct *
XLoadQueryFont(
	Display *d,
	const char *n)
{
	uint8_t x[4];
	uint8_t q[8] = {0}, h[32];
	uint32_t extra;
	XFontStruct *f;

	f = calloc(1, sizeof(*f));

	/* Checks the current file state. */
	if (!f)
		return 0;
	f->fid = XLoadFont(d, n);

	/* Checks the current file state. */
	if (!f->fid) {
		free(f);

		/* Reports successful completion. */
		return 0;
	}
	q[0] = 47;
	w32(q + 4, f->fid);

	/* Handles a failed req operation. */
	if (req(d, q, 8) || reply(d, h) || h[0] != 1) {
		free(f);

		/* Reports successful completion. */
		return 0;
	}

	/* Continue while the operation condition remains true. */
	extra = r32(h + 4);
	while (extra--) {
		/* Handles a failed rd operation. */
		if (rd(d->fd, x, 4)) {
			free(f);

			/* Reports successful completion. */
			return 0;
		}
	}
	f->ascent = 16;
	f->descent = 0;
	f->min_bounds.width = 8;
	f->max_bounds.width = 16;
	f->all_chars_exist = True;
	f->max_char_or_byte2 = 255;
	f->max_byte1 = 255;

	/* Returns the computed result. */
	return f;
}

/*
 * Implements the XFreeFont operation.
 */
int
XFreeFont(
	Display *d,
	XFontStruct *f)
{
	uint8_t q[8] = {0};

	/* Checks the current file state. */
	if (!f)
		return 0;
	q[0] = 46;
	w32(q + 4, f->fid);
	req(d, q, 8);
	free(f);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the XListFonts operation.
 */
char **
XListFonts(
	Display *d,
	const char *p,
	int max,
	int *count)
{
	uint8_t x[4];
	size_t bytes;
	uint8_t *b;
	size_t l, z;
	uint8_t *q, h[32];
	uint32_t extra;
	char **v;

	l = strlen(p);
	z = (8 + l + 3) & ~3U;
	q = calloc(1, z);

	/* Checks the remaining item count. */
	if (count)
		*count = 0;
	/* Handles the q condition. */
	if (!q)
		return 0;
	q[0] = 49;
	w16(q + 4, (uint16_t)max);
	w16(q + 6, (uint16_t)l);
	memcpy(q + 8, p, l);

	/* Handles a failed req operation. */
	if (req(d, q, z) || reply(d, h) || h[0] != 1) {
		free(q);

		/* Reports successful completion. */
		return 0;
	}
	free(q);
	extra = r32(h + 4);

	/* Handles a failed r16 operation. */
	if (r16(h + 8) == 0) {
		/* Continue while the operation condition remains true. */
		while (extra--) {
			/* Handles a failed rd operation. */
			if (rd(d->fd, x, 4))
				break;
		}

		/* Reports successful completion. */
		return 0;
	}
	v = calloc(2, sizeof(*v));

	/* Handles the v condition. */
	if (!v)
		return 0;

	bytes = (size_t)extra * 4;
	b = malloc(bytes);

	/* Handles a failed rd operation. */
	if (!b || rd(d->fd, b, bytes)) {
		free(b);
		free(v);

		/* Reports successful completion. */
		return 0;
	}
	v[0] = malloc((size_t)b[0] + 1);

	/* Handles the v condition. */
	if (!v[0]) {
		free(b);
		free(v);

		/* Reports successful completion. */
		return 0;
	}
	memcpy(v[0], b + 1, b[0]);
	v[0][b[0]] = 0;
	free(b);

	/* Checks the remaining item count. */
	if (count)
		*count = 1;
	/* Returns the computed result. */
	return v;
}

/*
 * Implements the XFreeFontNames operation.
 */
int
XFreeFontNames(
	char **v)
{
	unsigned i;

	/* Handles the v condition. */
	if (!v)
		return 0;

	/* Process each element required by the operation. */
	for (i = 0; v[i]; i++)
		free(v[i]);
	free(v);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the XCreateSimpleWindow operation.
 */
Window
XCreateSimpleWindow(
	Display *d,
	Window p,
	int x,
	int y,
	unsigned int wi,
	unsigned int he,
	unsigned int bw,
	unsigned long bc,
	unsigned long bg)
{
	Window function_result;
	uint8_t q[40] = {0};
	uint32_t id;

	id = d->base | d->next++;
	q[0] = 1;
	q[1] = 24;
	w32(q + 4, id);
	w32(q + 8, p);
	w16(q + 12, (uint16_t)x);
	w16(q + 14, (uint16_t)y);
	w16(q + 16, (uint16_t)wi);
	w16(q + 18, (uint16_t)he);
	w16(q + 20, (uint16_t)bw);
	w16(q + 22, 1);
	w32(q + 24, 0);
	w32(q + 28, (1U << 1) | (1U << 3));
	w32(q + 32, (uint32_t)bg);
	w32(q + 36, (uint32_t)bc);

	/* Computes the function result. */
	function_result = req(d, q, sizeof(q)) ? 0 : id;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XCreatePixmap operation.
 */
Pixmap
XCreatePixmap(
	Display *d,
	Drawable draw,
	unsigned int width,
	unsigned int height,
	unsigned int depth)
{
	Pixmap function_result;
	uint8_t q[16] = {0};
	Pixmap id = d->base | d->next++;
	q[0] = 53;
	q[1] = (uint8_t)depth;
	w32(q + 4, id);
	w32(q + 8, draw);
	w16(q + 12, (uint16_t)width);
	w16(q + 14, (uint16_t)height);

	/* Computes the function result. */
	function_result = req(d, q, sizeof(q)) ? 0 : id;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XFreePixmap operation.
 */
int
XFreePixmap(
	Display *d,
	Pixmap p)
{
	int function_result;
	uint8_t q[8] = {0};

	q[0] = 54;
	w32(q + 4, p);

	/* Obtains the req result. */
	function_result = req(d, q, sizeof(q));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XCopyArea operation.
 */
int
XCopyArea(
	Display *d,
	Drawable src,
	Drawable dst,
	GC gc,
	int sx,
	int sy,
	unsigned int width,
	unsigned int height,
	int dx,
	int dy)
{
	int function_result;
	uint8_t q[28] = {0};

	q[0] = 62;
	w32(q + 4, src);
	w32(q + 8, dst);
	w32(q + 12, gc->xid);
	w16(q + 16, (uint16_t)sx);
	w16(q + 18, (uint16_t)sy);
	w16(q + 20, (uint16_t)dx);
	w16(q + 22, (uint16_t)dy);
	w16(q + 24, (uint16_t)width);
	w16(q + 26, (uint16_t)height);

	/* Obtains the req result. */
	function_result = req(d, q, sizeof(q));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XSelectInput operation.
 */
int
XSelectInput(
	Display *d,
	Window w,
	long m)
{
	int function_result;
	uint8_t q[16] = {0};

	q[0] = 2;
	w32(q + 4, w);
	w32(q + 8, 1U << 11);
	w32(q + 12, (uint32_t)m);

	/* Obtains the req result. */
	function_result = req(d, q, 16);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XMapWindow operation.
 */
int
XMapWindow(
	Display *d,
	Window w)
{
	int function_result;

	/* Obtains the winreq result. */
	function_result = winreq(d, 8, w);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XUnmapWindow operation.
 */
int
XUnmapWindow(
	Display *d,
	Window w)
{
	int function_result;

	/* Obtains the winreq result. */
	function_result = winreq(d, 10, w);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XDestroyWindow operation.
 */
int
XDestroyWindow(
	Display *d,
	Window w)
{
	int function_result;

	/* Obtains the winreq result. */
	function_result = winreq(d, 4, w);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XReparentWindow operation.
 */
int
XReparentWindow(
	Display *d,
	Window w,
	Window p,
	int x,
	int y)
{
	int function_result;
	uint8_t q[16] = {0};

	q[0] = 7;
	w32(q + 4, w);
	w32(q + 8, p);
	w16(q + 12, (uint16_t)x);
	w16(q + 14, (uint16_t)y);

	/* Obtains the req result. */
	function_result = req(d, q, 16);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XMoveResizeWindow operation.
 */
int
XMoveResizeWindow(
	Display *d,
	Window w,
	int x,
	int y,
	unsigned int wi,
	unsigned int he)
{
	int function_result;
	uint8_t q[28] = {0};

	q[0] = 12;
	w32(q + 4, w);
	w16(q + 8, 15);
	w32(q + 12, (uint32_t)x);
	w32(q + 16, (uint32_t)y);
	w32(q + 20, wi);
	w32(q + 24, he);

	/* Obtains the req result. */
	function_result = req(d, q, 28);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XRaiseWindow operation.
 */
int
XRaiseWindow(
	Display *d,
	Window w)
{
	int function_result;
	uint8_t q[16] = {0};

	q[0] = 12;
	w32(q + 4, w);
	w16(q + 8, CWStackMode);
	w32(q + 12, Above);

	/* Obtains the req result. */
	function_result = req(d, q, sizeof(q));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XSetInputFocus operation.
 */
int
XSetInputFocus(
	Display *d,
	Window w,
	int revert,
	Time time)
{
	int function_result;
	uint8_t q[12] = {0};

	q[0] = 42;
	q[1] = (uint8_t)revert;
	w32(q + 4, w);
	w32(q + 8, time);

	/* Obtains the req result. */
	function_result = req(d, q, sizeof(q));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XGetInputFocus operation.
 */
int
XGetInputFocus(
	Display *d,
	Window *w,
	int *revert)
{
	uint8_t q[4] = {0}, r[32];

	q[0] = 43;

	/* Handles a failed req operation. */
	if (req(d, q, sizeof(q)) || reply(d, r) || r[0] != 1)
		return 0;

	/* Handles the w condition. */
	if (w)
		*w = r32(r + 8);
	/* Handles the revert condition. */
	if (revert)
		*revert = r[1];
	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the XGetGeometry operation.
 */
int
XGetGeometry(
	Display *d,
	Drawable w,
	Window *root,
	int *x,
	int *y,
	unsigned int *wi,
	unsigned int *he,
	unsigned int *b,
	unsigned int *depth)
{
	uint8_t q[8] = {0}, r[32];

	q[0] = 14;
	w32(q + 4, w);

	/* Handles a failed req operation. */
	if (req(d, q, 8) || reply(d, r) || r[0] != 1)
		return 0;

	/* Handles the root condition. */
	if (root)
		*root = r32(r + 8);
	/* Checks the current horizontal value. */
	if (x)
		*x = (int16_t)r16(r + 12);
	/* Handles the y condition. */
	if (y)
		*y = (int16_t)r16(r + 14);
	/* Handles the wi condition. */
	if (wi)
		*wi = r16(r + 16);
	/* Handles the he condition. */
	if (he)
		*he = r16(r + 18);
	/* Handles the b condition. */
	if (b)
		*b = r16(r + 20);
	/* Handles the depth condition. */
	if (depth)
		*depth = r[1];
	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the XQueryTree operation.
 */
int
XQueryTree(
	Display *d,
	Window w,
	Window *root,
	Window *parent,
	Window **children,
	unsigned int *count)
{
	uint8_t x_local[4];
	uint8_t x_local1[4];
	uint8_t x_local2[4];
	uint8_t x_local3[4];
	uint8_t x_local4[4];
	uint8_t q[8] = {0}, r[32];
	uint32_t extra;
	uint16_t n;
	Window *v;
	unsigned i;

	v = 0;

	/* Handles the children condition. */
	if (!children || !count) {
		errno = EINVAL;

		/* Reports successful completion. */
		return 0;
	}
	*children = 0;
	*count = 0;
	q[0] = 15;
	w32(q + 4, w);

	/* Handles a failed req operation. */
	if (req(d, q, sizeof(q)) || reply(d, r) || r[0] != 1)
		return 0;
	extra = r32(r + 4);
	n = r16(r + 16);

	/* Handles the extra condition. */
	if (extra < (uint32_t)n) {
		/* Continue while the operation condition remains true. */
		while (extra--) {
			/* Handles a failed rd operation. */
			if (rd(d->fd, x_local, 4))
				break;
		}

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the current item count. */
	if (n) {
		v = malloc((size_t)n * sizeof(*v));

		/* Handles the v condition. */
		if (!v) {
			/* Continue while the operation condition remains true. */
			while (extra--) {
				/* Handles a failed rd operation. */
				if (rd(d->fd, x_local1, 4))
					break;
			}

			/* Reports successful completion. */
			return 0;
		}

		/* Process each element required by the operation. */
		for (i = 0; i < n; i++) {
			/* Handles a failed rd operation. */
			if (rd(d->fd, x_local2, 4)) {
				free(v);

				/* Reports successful completion. */
				return 0;
			}
			v[i] = r32(x_local2);
		}

		/* Process each element required by the operation. */
		for (i = n; i < extra; i++) {
			/* Handles a failed rd operation. */
			if (rd(d->fd, x_local3, 4)) {
				free(v);

				/* Reports successful completion. */
				return 0;
			}
		}
	} else {
		/* Continue while the operation condition remains true. */
		while (extra--) {
			/* Handles a failed rd operation. */
			if (rd(d->fd, x_local4, 4))
				return 0;
		}
	}

	/* Handles the root condition. */
	if (root)
		*root = r32(r + 8);
	/* Handles the parent condition. */
	if (parent)
		*parent = r32(r + 12);
	*children = v;
	*count = n;
	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the XStoreName operation.
 */
int
XStoreName(
	Display *d,
	Window w,
	const char *n)
{
	int function_result;

	/* Obtains the store string result. */
	function_result = store_string(d, w, XA_WM_NAME, n);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Sets a window's WM_CLASS: the instance's name and the class, each ended
 * by a NUL, as one 8-bit STRING (ws035-p092).  Returns 0 when it could not
 * be sent.
 */
int
XSetClassHint(
	Display *d,
	Window w,
	XClassHint *hint)
{
	unsigned char data[256];
	const char *name;
	const char *class_name;
	size_t name_length;
	size_t class_length;
	int result;

	/* Both strings, an empty one for a NULL. */
	name = hint->res_name;
	if (name == NULL)
		name = "";
	class_name = hint->res_class;
	if (class_name == NULL)
		class_name = "";
	name_length = strlen(name);
	class_length = strlen(class_name);
	if (name_length + class_length + 2U > sizeof(data))
		return 0;

	/* The instance, a NUL, the class, a NUL. */
	memcpy(data, name, name_length + 1U);
	memcpy(data + name_length + 1U, class_name, class_length + 1U);

	/* Obtains the change property result. */
	result = XChangeProperty(d, w, XA_WM_CLASS, XA_STRING, 8, PropModeReplace, data, (int)(name_length + class_length + 2U));

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the XFetchName operation.
 */
int
XFetchName(
	Display *d,
	Window w,
	char **name)
{
	int function_result;

	/* Obtains the fetch string result. */
	function_result = fetch_string(d, w, XA_WM_NAME, name);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XzedSetIconPath operation.
 */
int
XzedSetIconPath(
	Display *d,
	Window w,
	const char *path)
{
	int function_result;

	/* Obtains the store string result. */
	function_result = store_string(d, w, XZED_ICON_PATH_ATOM, path);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XzedGetIconPath operation.
 */
int
XzedGetIconPath(
	Display *d,
	Window w,
	char **path)
{
	int function_result;

	/* Obtains the fetch string result. */
	function_result = fetch_string(d, w, XZED_ICON_PATH_ATOM, path);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Puts RGB24 rows (top down, stride bytes apart) into a drawable at x, y:
 * as many whole rows per request as a request holds (a row wider than
 * that goes in tiles), one round trip per request.
 */
int
XzedPutImageRGB24(
	Display *d,
	Drawable draw,
	int x,
	int y,
	unsigned width,
	unsigned height,
	const unsigned char *pixels,
	unsigned stride)
{
	uint8_t *q;
	size_t row_bytes;
	size_t payload;
	size_t size;
	unsigned rows;
	unsigned band;
	unsigned row;
	unsigned column;
	unsigned tile;
	unsigned span;
	unsigned line;
	int result;

	/* A display, pixels and a size a request can name. */
	if (!d || !pixels || !width || !height || width > 65535U || height > 65535U) {
		errno = EINVAL;
		return -1;
	}

	/* Rows no shorter than the width. */
	row_bytes = (size_t)width * 3U;
	if (stride < row_bytes) {
		errno = EINVAL;
		return -1;
	}

	/* Whole rows per request (a request is at most 65535 words), else tiles of one row. */
	rows = (unsigned)((XZED_REQUEST_BYTES - 16U) / row_bytes);
	tile = width;
	if (rows == 0U) {
		rows = 1U;
		tile = 640U;
	}

	/* The request, large enough for one band. */
	q = malloc(XZED_REQUEST_BYTES);
	if (!q)
		return -1;

	/* Each band of rows, each tile of it. */
	for (row = 0U; row < height; row += band) {
		band = height - row;
		if (band > rows)
			band = rows;
		for (column = 0U; column < width; column += span) {
			/* The tile's width, its request's size. */
			span = tile;
			if (span > width - column)
				span = width - column;
			payload = (size_t)span * 3U * band;
			size = (16U + payload + 3U) & ~(size_t)3U;
			memset(q, 0, size);

			/* The header: the drawable, where, how large. */
			q[0] = 128;
			w32(q + 4, draw);
			w16(q + 8, (uint16_t)(x + (int)column));
			w16(q + 10, (uint16_t)(y + (int)row));
			w16(q + 12, (uint16_t)span);
			w16(q + 14, (uint16_t)band);

			/* The rows. */
			for (line = 0U; line < band; line++)
				memcpy(q + 16U + (size_t)line * span * 3U, pixels + (size_t)(row + line) * stride + (size_t)column * 3U, (size_t)span * 3U);

			/* Sent, and waited for: Xzed holds at most 1 MiB of a client's requests, so one request is in flight at a time. */
			result = req(d, q, size);
			if (!result)
				result = XSync(d, False);
			if (result) {
				free(q);
				return -1;
			}
		}
	}

	/* Succeeded: every band is in the drawable. */
	free(q);
	return 0;
}

/*
 * Asks the server whether it has an extension, and its major opcode and
 * first event and error.
 */
Bool
XQueryExtension(
	Display *d,
	const char *name,
	int *major_opcode,
	int *first_event,
	int *first_error)
{
	uint8_t q[8U + 256U];
	uint8_t r[32];
	size_t length;
	size_t size;
	int result;

	/* The request: the name's length and the name, padded. */
	length = strlen(name);
	if (!d || length > 255U)
		return False;
	size = (8U + length + 3U) & ~(size_t)3U;
	memset(q, 0, sizeof(q));
	q[0] = 98;
	w16(q + 4, (uint16_t)length);
	memcpy(q + 8, name, length);

	/* The reply. */
	result = req(d, q, size);
	if (!result)
		result = reply(d, r);
	if (result)
		return False;

	/* Present or not, with its numbers. */
	if (major_opcode)
		*major_opcode = r[9];
	if (first_event)
		*first_event = r[10];
	if (first_error)
		*first_error = r[11];
	if (!r[8])
		return False;
	return True;
}

/*
 * Sends a request of an extension (its length field filled in) and, when
 * reply32 is given, waits for its reply: the first 32 bytes there, and
 * the bytes after them in a new buffer (*extra, freed by the caller).
 * Returns 0, or -1 when the connection failed or the server refused it.
 */
int
XzedExtensionRequest(
	Display *d,
	void *request,
	size_t length,
	unsigned char *reply32,
	unsigned char **extra,
	size_t *extra_length)
{
	uint8_t *bytes;
	size_t words;
	int result;

	/* Whole words. */
	if (!d || !request || length < 4U || (length & 3U) || length > XZED_REQUEST_BYTES) {
		errno = EINVAL;
		return -1;
	}

	/* Sent; a request without a reply is done. */
	result = req(d, request, length);
	if (result)
		return -1;
	if (!reply32)
		return 0;

	/* The reply's first 32 bytes. */
	result = reply(d, reply32);
	if (result)
		return -1;

	/* The rest of it, when there is some. */
	words = r32(reply32 + 4);
	if (extra)
		*extra = NULL;
	if (extra_length)
		*extra_length = words * 4U;
	if (!words)
		return 0;
	bytes = malloc(words * 4U + 1U);
	if (!bytes)
		return -1;
	result = rd(d->fd, bytes, words * 4U);
	if (result) {
		free(bytes);
		return -1;
	}

	/* The bytes go to the caller, or are dropped. */
	bytes[words * 4U] = 0;
	if (extra) {
		*extra = bytes;
	} else {
		free(bytes);
	}

	/* Succeeded: the reply. */
	return 0;
}

/*
 * Implements the XzedSetCursorShape operation.
 */
int
XzedSetCursorShape(
	Display *d,
	Window w,
	unsigned shape)
{
	int function_result;
	uint8_t q[12] = {0};

	/* Checks the current descriptor. */
	if (!d) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	q[0] = 129;
	w32(q + 4, w);
	w32(q + 8, shape);

	/* Obtains the req result. */
	function_result = req(d, q, sizeof(q));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XzedSetInputMargins operation.
 */
int
XzedSetInputMargins(
	Display *d,
	Window w,
	unsigned left,
	unsigned top,
	unsigned right,
	unsigned bottom)
{
	int function_result;
	uint8_t q[16] = {0};

	/* Checks the current descriptor. */
	if (!d || left > 65535U || top > 65535U || right > 65535U ||
	    bottom > 65535U) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	q[0] = 130;
	w32(q + 4, w);
	w16(q + 8, (uint16_t)left);
	w16(q + 10, (uint16_t)top);
	w16(q + 12, (uint16_t)right);
	w16(q + 14, (uint16_t)bottom);

	/* Obtains the req result. */
	function_result = req(d, q, sizeof(q));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XzedMoveResizeWindowBuffered operation.
 */
int
XzedMoveResizeWindowBuffered(
	Display *d,
	Window w,
	int x,
	int y,
	unsigned width,
	unsigned height)
{
	int function_result;
	uint8_t q[24] = {0};

	/* Checks the current descriptor. */
	if (!d || !width || !height || width > 65535U || height > 65535U) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	q[0] = 131;
	w32(q + 4, w);
	w32(q + 8, (uint32_t)x);
	w32(q + 12, (uint32_t)y);
	w32(q + 16, width);
	w32(q + 20, height);

	/* Obtains the req result. */
	function_result = req(d, q, sizeof(q));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XFree operation.
 */
int
XFree(
	void *p)
{
	free(p);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the XCreateGC operation.
 */
GC
XCreateGC(
	Display *d,
	Drawable w,
	unsigned long m,
	void *v)
{
	uint8_t q[16] = {0};
	GC g;

	g = calloc(1, sizeof(*g));
	(void)m;
	(void)v;

	/* Handles the g condition. */
	if (!g)
		return 0;
	g->xid = d->base | d->next++;
	g->foreground = 0xffffff;
	q[0] = 55;
	w32(q + 4, g->xid);
	w32(q + 8, w);

	/* Handles the req condition. */
	if (req(d, q, 16)) {
		free(g);

		/* Reports successful completion. */
		return 0;
	}

	/* Returns the computed result. */
	return g;
}

/*
 * Implements the XFreeGC operation.
 */
int
XFreeGC(
	Display *d,
	GC g)
{
	uint8_t q[8] = {0};

	/* Handles the g condition. */
	if (!g)
		return 0;
	q[0] = 60;
	w32(q + 4, g->xid);
	req(d, q, 8);
	free(g);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the XSetForeground operation.
 */
int
XSetForeground(
	Display *d,
	GC g,
	unsigned long c)
{
	int function_result;
	uint8_t q[16] = {0};

	g->foreground = (uint32_t)c;
	q[0] = 56;
	w32(q + 4, g->xid);
	w32(q + 8, 4);
	w32(q + 12, (uint32_t)c);

	/* Obtains the req result. */
	function_result = req(d, q, 16);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XSetFont operation.
 */
int
XSetFont(
	Display *d,
	GC g,
	Font f)
{
	int function_result;
	uint8_t q[16] = {0};

	q[0] = 56;
	w32(q + 4, g->xid);
	w32(q + 8, 1U << 14);
	w32(q + 12, f);

	/* Obtains the req result. */
	function_result = req(d, q, 16);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XFillRectangles operation.
 */
int
XFillRectangles(
	Display *d,
	Drawable w,
	GC g,
	XRectangle *r,
	int count)
{
	uint8_t *q;
	size_t n;
	int i, result;

	/* Checks the remaining item count. */
	if (count < 0 || (!r && count)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	n = 12U + (size_t)count * 8U;
	q = calloc(1, n);

	/* Handles the q condition. */
	if (!q) {
		errno = ENOMEM;

		/* Reports operation failure. */
		return -1;
	}
	q[0] = 70;
	w32(q + 4, w);
	w32(q + 8, g->xid);

	/* Process each remaining element. */
	for (i = 0; i < count; i++) {
		w16(q + 12 + i * 8, (uint16_t)r[i].x);
		w16(q + 14 + i * 8, (uint16_t)r[i].y);
		w16(q + 16 + i * 8, r[i].width);
		w16(q + 18 + i * 8, r[i].height);
	}
	result = req(d, q, n);
	free(q);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the XFillRectangle operation.
 */
int
XFillRectangle(
	Display *d,
	Drawable w,
	GC g,
	int x,
	int y,
	unsigned int wi,
	unsigned int he)
{
	XRectangle r = {(short)x, (short)y, (unsigned short)wi,
			(unsigned short)he};
	int function_result;

	/* Obtains the XFillRectangles result. */
	function_result = XFillRectangles(d, w, g, &r, 1);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XDrawLine operation.
 */
int
XDrawLine(
	Display *d,
	Drawable w,
	GC g,
	int x,
	int y,
	int x2,
	int y2)
{
	int function_result;
	uint8_t q[20] = {0};

	q[0] = 65;
	w32(q + 4, w);
	w32(q + 8, g->xid);
	w16(q + 12, (uint16_t)x);
	w16(q + 14, (uint16_t)y);
	w16(q + 16, (uint16_t)x2);
	w16(q + 18, (uint16_t)y2);

	/* Obtains the req result. */
	function_result = req(d, q, 20);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XDrawString operation.
 */
int
XDrawString(
	Display *d,
	Drawable w,
	GC g,
	int x,
	int y,
	const char *s,
	int n)
{
	int function_result;

	/* Sends the string as PolyText8 so glyphs are drawn without filling a background box. */
	function_result = text_req(d, 74, w, g, x, y, s, n, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XDrawString16 operation.
 */
int
XDrawString16(
	Display *d,
	Drawable w,
	GC g,
	int x,
	int y,
	const XChar2b *s,
	int n)
{
	int function_result;

	/* Sends the 16-bit string as PolyText16 so glyphs are drawn without filling a background box. */
	function_result = text_req(d, 75, w, g, x, y, s, n, 1);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the XNextEvent operation.
 */
int
XNextEvent(
	Display *d,
	XEvent *e)
{
	uint8_t b[32];

	/* Checks the current descriptor. */
	if (d->event_count) {
		memcpy(b, d->events[d->event_head], 32);
		d->event_head = (d->event_head + 1U) % EVENT_QUEUE_SIZE;
		d->event_count--;
	} else if (rd(d->fd, b, 32))

		/* Reports operation failure. */
		return -1;
	event(d, b, e);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the XPending operation.
 */
int
XPending(
	Display *d)
{
	int f, z;
	int error;
	uint8_t b[32];

	/* Checks the current descriptor. */
	if (d->event_count)
		return (int)d->event_count;

	/*
	 * Only a whole event is taken: the bytes are looked at first, and a
	 * part of one stays in the socket for the next read.  Taking a part and
	 * dropping it would put every later reply out of step, and the next
	 * one waited for would never come (WS069 p007, BUG-057).
	 */
	f = fcntl(d->fd, F_GETFL);
	fcntl(d->fd, F_SETFL, f | O_NONBLOCK);
	z = (int)recv(d->fd, b, 32, MSG_DONTWAIT | MSG_PEEK);
	error = errno;
	if (z == 32)
		z = (int)recv(d->fd, b, 32, MSG_DONTWAIT);
	fcntl(d->fd, F_SETFL, f);
	if (z > 0 && z < 32)
		return 0;

	/*
	 * A connection the server has closed (or broken) ends the client, as
	 * Xlib's default I/O error handler does (WS069: Xzed rootless ends the
	 * client of a window the compositor closes).
	 */
	if (z == 0 || (z < 0 && error != EAGAIN && error != EWOULDBLOCK && error != EINTR)) {
		fprintf(stderr, "X connection broken: the server closed it\n");
		exit(1);
	}

	/* Handles the z condition. */
	if (z == 32 && (b[0] & 0x7f) >= 2)
		(void)queue_event(d, b);

	/* Returns the computed result. */
	return (int)d->event_count;
}

/*
 * Implements the XFlush operation.
 */
int
XFlush(
	Display *d)
{
	(void)d;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the XSync operation.
 */
int
XSync(
	Display *d,
	Bool discard)
{
	uint8_t q[4] = {0}, b[32];

	q[0] = 43;

	/* Handles a failed req operation. */
	if (req(d, q, sizeof(q)) || reply(d, b) || b[0] != 1)
		return -1;

	/* Handles the discard condition. */
	if (discard) {
		d->event_head = 0;
		d->event_count = 0;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Returns the keysym of a key event.
 *
 * Index 0 chooses the keysym the event's state types, by the core
 * protocol's rules: the second group while Mode_switch's modifier is held
 * (the first when the key has none), the keypad's digits while Num_Lock's
 * is, the second keysym of the group with Shift (or Shift Lock), and an
 * upper-case letter with Caps Lock.  Another index is that column of the
 * key's keysyms as the server gave them.  (Xlib's own XLookupKeysym
 * returns column 0 for index 0; the tree's programs want the keysym the
 * key types, which XLookupString gives elsewhere.)  The keyboard's map is read
 * again after the server said it changed.
 */
KeySym
XLookupKeysym(
	XKeyEvent *e,
	int index)
{
	Display *d;
	uint32_t first;
	uint32_t second;
	uint32_t lower;
	uint32_t upper;
	unsigned keycode;
	unsigned group;
	int shift;
	int lock;

	/* The event's connection and key. */
	if (e == NULL || e->display == NULL || index < 0)
		return None;
	d = e->display;
	keycode = e->keycode;

	/* The map, read again when the server said it changed. */
	if (d->keymap_stale)
		keymap_load(d);
	if (d->keysyms == NULL || keycode < d->min_keycode || keycode > d->max_keycode)
		return None;

	/* Another column: as the server gave it. */
	if (index != 0)
		return (KeySym)keymap_keysym(d, keycode, (unsigned)index);

	/*
	 * One keysym a keycode: the server (xzed) gives each character its own
	 * keycode, with Shift and Caps Lock already applied, so the keysym is
	 * the character as it is.
	 */
	if (d->keysyms_per_keycode == 1U)
		return (KeySym)keymap_keysym(d, keycode, 0U);

	/* The group: the second while Mode_switch is held and the key has one. */
	group = 0U;
	if ((e->state & d->mode_switch_mask) != 0U) {
		first = keymap_keysym(d, keycode, 2U);
		second = keymap_keysym(d, keycode, 3U);
		if (first != 0U || second != 0U)
			group = 2U;
	}
	first = keymap_keysym(d, keycode, group);
	second = keymap_keysym(d, keycode, group + 1U);

	/* A group of one keysym is that keysym twice, or a letter's two cases. */
	if (second == 0U) {
		keymap_case(first, &lower, &upper);
		first = lower;
		second = upper;
	}

	/* Shift, and Lock as Caps Lock or Shift Lock. */
	shift = 0;
	if ((e->state & ShiftMask) != 0U)
		shift = 1;
	lock = 0;
	if ((e->state & LockMask) != 0U)
		lock = d->lock_meaning;

	/* The keypad with Num_Lock: the digit, unless Shift (or Shift Lock) asks for the function. */
	if ((e->state & d->num_lock_mask) != 0U && second >= KEYMAP_KEYPAD_FIRST && second <= KEYMAP_KEYPAD_LAST) {
		if (shift || lock == KEYMAP_LOCK_SHIFT)
			return (KeySym)first;
		return (KeySym)second;
	}

	/* Shift or Shift Lock: the second, a letter in upper case under Caps Lock. */
	if (shift || lock == KEYMAP_LOCK_SHIFT) {
		if (lock == KEYMAP_LOCK_CAPS) {
			keymap_case(second, &lower, &upper);
			return (KeySym)upper;
		}
		return (KeySym)second;
	}

	/* Caps Lock alone: the first, a letter in upper case. */
	if (lock == KEYMAP_LOCK_CAPS) {
		keymap_case(first, &lower, &upper);
		return (KeySym)upper;
	}

	/* Succeeded: the first. */
	return (KeySym)first;
}

/*
 * Finds a name's atom (InternAtom); a new name gets one unless only an
 * existing one is asked for.  Returns the atom, or None.
 */
Atom
XInternAtom(
	Display *d,
	const char *name,
	Bool only_if_exists)
{
	uint8_t *q;
	uint8_t r[32];
	size_t length;
	size_t size;
	int error;

	/* The name's length, and the request padded to words. */
	length = strlen(name);
	if (length > 0xffffU)
		return None;
	size = (8U + length + 3U) & ~(size_t)3U;
	q = calloc(1, size);
	if (q == NULL)
		return None;

	/* The request: whether only an existing one, the length, the name. */
	q[0] = 16;
	q[1] = 0;
	if (only_if_exists)
		q[1] = 1;
	w16(q + 4, (uint16_t)length);
	memcpy(q + 8, name, length);
	error = req(d, q, size);
	free(q);
	if (error != 0)
		return None;

	/* The reply's atom. */
	error = reply(d, r);
	if (error != 0)
		return None;

	/* Succeeded: the atom (None when there is none). */
	return r32(r + 8);
}

/*
 * Makes a window the owner of a selection (SetSelectionOwner; None gives
 * it up).  Returns 1, or 0 when the request could not be sent.
 */
int
XSetSelectionOwner(
	Display *d,
	Atom selection,
	Window owner,
	Time time)
{
	uint8_t q[16] = {0};
	int error;

	/* The owner, the selection and the time. */
	q[0] = 22;
	w32(q + 4, owner);
	w32(q + 8, selection);
	w32(q + 12, time);
	error = req(d, q, sizeof(q));
	if (error != 0)
		return 0;

	/* Succeeded: sent. */
	return 1;
}

/*
 * Returns a selection's owner window (GetSelectionOwner), or None.
 */
Window
XGetSelectionOwner(
	Display *d,
	Atom selection)
{
	uint8_t q[8] = {0};
	uint8_t r[32];
	int error;

	/* The selection. */
	q[0] = 23;
	w32(q + 4, selection);
	error = req(d, q, sizeof(q));
	if (error != 0)
		return None;

	/* The reply's owner. */
	error = reply(d, r);
	if (error != 0)
		return None;

	/* Succeeded: the owner. */
	return r32(r + 8);
}

/*
 * Asks for a selection as a target in a property of a window
 * (ConvertSelection); a SelectionNotify event says when it is there.
 * Returns 1, or 0 when the request could not be sent.
 */
int
XConvertSelection(
	Display *d,
	Atom selection,
	Atom target,
	Atom property,
	Window requestor,
	Time time)
{
	uint8_t q[24] = {0};
	int error;

	/* The requestor, the selection, the target, the property and the time. */
	q[0] = 24;
	w32(q + 4, requestor);
	w32(q + 8, selection);
	w32(q + 12, target);
	w32(q + 16, property);
	w32(q + 20, time);
	error = req(d, q, sizeof(q));
	if (error != 0)
		return 0;

	/* Succeeded: sent. */
	return 1;
}

/*
 * Sets a window's property (ChangeProperty): nelements units of format
 * bits, replacing, or added before or after what is there.  Returns 1, or
 * 0 when the request could not be sent.
 */
int
XChangeProperty(
	Display *d,
	Window w,
	Atom property,
	Atom type,
	int format,
	int mode,
	const unsigned char *data,
	int nelements)
{
	uint8_t *q;
	size_t bytes;
	size_t size;
	size_t index;
	uint16_t unit16;
	uint32_t unit32;
	int error;

	/* The data's bytes, and the request padded to words. */
	if (nelements < 0 || (format != 8 && format != 16 && format != 32))
		return 0;
	bytes = (size_t)nelements * (size_t)(format / 8);
	size = (24U + bytes + 3U) & ~(size_t)3U;
	if (size > XZED_REQUEST_BYTES)
		return 0;
	q = calloc(1, size);
	if (q == NULL)
		return 0;

	/* The request: the mode, the window, the property, its type and format, the units and the data. */
	q[0] = 18;
	q[1] = (uint8_t)mode;
	w32(q + 4, w);
	w32(q + 8, property);
	w32(q + 12, type);
	q[16] = (uint8_t)format;
	w32(q + 20, (uint32_t)nelements);

	/*
	 * The data: 16- and 32-bit units are in the client's order, which
	 * this library always names little-endian, whatever the CPU's order.
	 */
	if (format == 8) {
		memcpy(q + 24, data, bytes);
	} else if (format == 16) {
		for (index = 0; index < (size_t)nelements; index++) {
			memcpy(&unit16, data + index * 2U, sizeof(unit16));
			w16(q + 24 + index * 2U, unit16);
		}
	} else {
		for (index = 0; index < (size_t)nelements; index++) {
			memcpy(&unit32, data + index * 4U, sizeof(unit32));
			w32(q + 24 + index * 4U, unit32);
		}
	}
	error = req(d, q, size);
	free(q);
	if (error != 0)
		return 0;

	/* Succeeded: sent. */
	return 1;
}

/*
 * Reads a window's property (GetProperty): its type and format, the units
 * given from long_offset (at most long_length units of four bytes), what
 * is left after them, and the data (malloc'd, with a NUL after it; XFree
 * frees it).  Returns 0 (Success), or 1 when the request failed.
 */
int
XGetWindowProperty(
	Display *d,
	Window w,
	Atom property,
	long long_offset,
	long long_length,
	Bool remove,
	Atom req_type,
	Atom *actual_type,
	int *actual_format,
	unsigned long *nitems,
	unsigned long *bytes_after,
	unsigned char **prop)
{
	uint8_t q[24] = {0};
	uint8_t r[32];
	unsigned char *data;
	size_t extra;
	size_t bytes;
	size_t index;
	uint16_t unit16;
	uint32_t unit32;
	int error;

	/* Nothing yet. */
	*actual_type = None;
	*actual_format = 0;
	*nitems = 0;
	*bytes_after = 0;
	*prop = NULL;

	/* The request. */
	q[0] = 20;
	q[1] = 0;
	if (remove)
		q[1] = 1;
	w32(q + 4, w);
	w32(q + 8, property);
	w32(q + 12, req_type);
	w32(q + 16, (uint32_t)long_offset);
	w32(q + 20, (uint32_t)long_length);
	error = req(d, q, sizeof(q));
	if (error != 0)
		return 1;

	/* The reply's header. */
	error = reply(d, r);
	if (error != 0)
		return 1;
	*actual_format = r[1];
	*actual_type = r32(r + 8);
	*bytes_after = r32(r + 12);
	*nitems = r32(r + 16);

	/* The data after it (its words), with a NUL. */
	extra = (size_t)r32(r + 4) * 4U;
	data = malloc(extra + 1U);
	if (data == NULL)
		return 1;
	if (extra != 0U) {
		error = rd(d->fd, data, extra);
		if (error != 0) {
			free(data);
			return 1;
		}
	}

	/* Succeeded: the data, ended after the units given. */
	bytes = 0;
	if (*actual_format != 0)
		bytes = (size_t)*nitems * (size_t)(*actual_format / 8);
	if (bytes > extra)
		bytes = extra;

	/* 16- and 32-bit units from the little-endian wire to the CPU's order. */
	if (*actual_format == 16) {
		for (index = 0; index + 2U <= bytes; index += 2U) {
			unit16 = r16(data + index);
			memcpy(data + index, &unit16, sizeof(unit16));
		}
	} else if (*actual_format == 32) {
		for (index = 0; index + 4U <= bytes; index += 4U) {
			unit32 = r32(data + index);
			memcpy(data + index, &unit32, sizeof(unit32));
		}
	}
	data[bytes] = 0;
	*prop = data;
	return 0;
}

/*
 * Removes a window's property (DeleteProperty).  Returns 1, or 0 when the
 * request could not be sent.
 */
int
XDeleteProperty(
	Display *d,
	Window w,
	Atom property)
{
	uint8_t q[12] = {0};
	int error;

	/* The window and the property. */
	q[0] = 19;
	w32(q + 4, w);
	w32(q + 8, property);
	error = req(d, q, sizeof(q));
	if (error != 0)
		return 0;

	/* Succeeded: sent. */
	return 1;
}

/*
 * Sends an event to a window's client (SendEvent).  Only SelectionNotify
 * is encoded (a selection's owner answers a request with it).  Returns 1,
 * or 0 for another event or when the request could not be sent.
 */
int
XSendEvent(
	Display *d,
	Window w,
	Bool propagate,
	long event_mask,
	XEvent *e)
{
	uint8_t q[44] = {0};
	int error;

	/* Only SelectionNotify. */
	if (e->type != SelectionNotify)
		return 0;

	/* The request: the destination, the mask and the event. */
	q[0] = 25;
	q[1] = 0;
	if (propagate)
		q[1] = 1;
	w32(q + 4, w);
	w32(q + 8, (uint32_t)event_mask);
	q[12] = SelectionNotify;
	w32(q + 16, e->xselection.time);
	w32(q + 20, e->xselection.requestor);
	w32(q + 24, e->xselection.selection);
	w32(q + 28, e->xselection.target);
	w32(q + 32, e->xselection.property);
	error = req(d, q, sizeof(q));
	if (error != 0)
		return 0;

	/* Succeeded: sent. */
	return 1;
}

/* Supports the w16 operation. */
static void
w16(
	uint8_t *p,
	uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/* Supports the wr operation. */
static int
wr(
	int f,
	const void *v,
	size_t n)
{
	ssize_t z;
	const uint8_t *p;

	/* Continue while the operation condition remains true. */
	p = v;
	while (n) {
		z = send(f, p, n, 0);

		/* Handles the z condition. */
		if (z < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* A request cut short puts every later one out of step: said (WS069 p010, BUG-057). */
			fprintf(stderr, "Xlib: send failed: errno=%d with %lu bytes of the request left\n", errno, (unsigned long)n);
			return -1;
		}

		/* Handles the z condition. */
		if (!z) {
			errno = EPIPE;

			/* Reports operation failure. */
			return -1;
		}
		p += z;
		n -= (size_t)z;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the rd operation. */
static int
rd(
	int f,
	void *v,
	size_t n)
{
	ssize_t z;
	uint8_t *p;

	/* Continue while the operation condition remains true. */
	p = v;
	while (n) {
		z = recv(f, p, n, 0);

		/* Handles the z condition. */
		if (z < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the z condition. */
		if (!z) {
			errno = EPIPE;

			/* Reports operation failure. */
			return -1;
		}
		p += z;
		n -= (size_t)z;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the r16 operation. */
static uint16_t
r16(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint16_t)(p[0] | p[1] << 8);
}

/* Supports the r32 operation. */
static uint32_t
r32(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

/* Supports the w32 operation. */
static void
w32(
	uint8_t *p,
	uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Supports the req operation. */
static int
req(
	Display *d,
	uint8_t *q,
	size_t n)
{
	int function_result;

	w16(q + 2, (uint16_t)(n / 4));
	d->sequence++;

	/* Obtains the wr result. */
	function_result = wr(d->fd, q, n);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the reply operation. */
static int
reply(
	Display *d,
	uint8_t *b)
{
	uint16_t expected;
	int failed;

	/* Continue until the operation reaches a terminal state. */
	expected = d->sequence;
	failed = 0;
	for (;;) {
		/* Handles a failed rd operation. */
		if (rd(d->fd, b, 32))
			return -1;

		/* Handles the b condition. */
		if (b[0] == 0) {
			/* Handles a failed r16 operation. */
			if (r16(b + 2) == expected) {
				errno = EIO;

				/* Reports operation failure. */
				return -1;
			}
			failed = 1;
			continue;
		}

		/* Handles the b condition. */
		if (b[0] == 1) {
			/* Handles a failed r16 operation. */
			if (r16(b + 2) != expected) {
				errno = EIO;

				/* Reports operation failure. */
				return -1;
			}

			/* Handles an operation failure. */
			if (failed) {
				errno = EIO;

				/* Reports operation failure. */
				return -1;
			}

			/* Reports successful completion. */
			return 0;
		}

		/* Handles the queue event condition. */
		if (queue_event(d, b))
			return -1;
	}
}

/* Supports the queue event operation. */
static int
queue_event(
	Display *d,
	const uint8_t *b)
{
	unsigned slot;

	/* Checks the current descriptor. */
	if (d->event_count == EVENT_QUEUE_SIZE) {
		errno = ENOBUFS;

		/* Reports operation failure. */
		return -1;
	}
	slot = (d->event_head + d->event_count) % EVENT_QUEUE_SIZE;
	memcpy(d->events[slot], b, 32);
	d->event_count++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the winreq operation. */
static int
winreq(
	Display *d,
	uint8_t op,
	Window w)
{
	int function_result;
	uint8_t q[8] = {0};

	q[0] = op;
	w32(q + 4, w);

	/* Obtains the req result. */
	function_result = req(d, q, 8);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the store string operation. */
static int
store_string(
	Display *d,
	Window w,
	Atom property,
	const char *n)
{
	size_t l, z;
	uint8_t *q;
	int result;

	/* Checks the current item count. */
	if (!n) {
		errno = EINVAL;

		/* Reports successful completion. */
		return 0;
	}
	l = strlen(n);

	/* Handles the l condition. */
	if (l > UINT32_MAX) {
		errno = EOVERFLOW;

		/* Reports successful completion. */
		return 0;
	}
	z = (24U + l + 3U) & ~3U;
	q = calloc(1, z);

	/* Handles the q condition. */
	if (!q)
		return 0;
	q[0] = 18;
	q[1] = PropModeReplace;
	w32(q + 4, w);
	w32(q + 8, property);
	w32(q + 12, XA_STRING);
	q[16] = 8;
	w32(q + 20, (uint32_t)l);
	memcpy(q + 24, n, l);
	result = req(d, q, z);
	free(q);

	/* Returns the computed result. */
	return result == 0;
}

/* Supports the fetch string operation. */
static int
fetch_string(
	Display *d,
	Window w,
	Atom property,
	char **value)
{
	uint8_t discard_local;
	uint8_t discard_local1;
	uint8_t discard_local2;
	uint8_t q[24] = {0}, r[32];
	uint32_t extra, n, padded;
	char *s;

	/* Validates the current value. */
	if (!value) {
		errno = EINVAL;

		/* Reports successful completion. */
		return 0;
	}
	*value = 0;
	q[0] = 20;
	w32(q + 4, w);
	w32(q + 8, property);
	w32(q + 12, XA_STRING);
	w32(q + 20, 256);

	/* Handles a failed req operation. */
	if (req(d, q, sizeof(q)) || reply(d, r) || r[0] != 1)
		return 0;
	extra = r32(r + 4);
	n = r32(r + 16);
	padded = extra * 4U;

	/* Handles a failed r32 operation. */
	if (r[1] != 8 || r32(r + 8) != XA_STRING || n > padded) {
		/* Continue while the operation condition remains true. */
		while (padded--) {
			/* Handles a failed rd operation. */
			if (rd(d->fd, &discard_local, 1))
				break;
		}

		/* Reports successful completion. */
		return 0;
	}
	s = malloc((size_t)n + 1U);

	/* Checks the current string state. */
	if (!s) {
		/* Continue while the operation condition remains true. */
		while (padded--) {
			/* Handles a failed rd operation. */
			if (rd(d->fd, &discard_local1, 1))
				break;
		}

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed rd operation. */
	if (n && rd(d->fd, s, n)) {
		free(s);

		/* Reports successful completion. */
		return 0;
	}

	/* Continue while the operation condition remains true. */
	s[n] = 0;
	while (padded > n) {
		/* Handles a failed rd operation. */
		if (rd(d->fd, &discard_local2, 1)) {
			free(s);

			/* Reports successful completion. */
			return 0;
		}
		padded--;
	}
	*value = s;
	/* Reports operation failure. */
	return 1;
}

/* Encodes and sends a PolyText8 or PolyText16 request for a single string. */
static int
text_req(
	Display *d,
	uint8_t op,
	Drawable w,
	GC g,
	int x,
	int y,
	const void *s,
	int n,
	int wide)
{
	size_t char_size;
	size_t chunks;
	size_t item_bytes;
	size_t rem;
	size_t chunk;
	size_t chunk_bytes;
	size_t z;
	const uint8_t *src;
	uint8_t *dst;
	uint8_t *q;
	int status;

	/* Rejects negative counts and empty string pointers when characters are requested. */
	if (n < 0 || (n > 0 && s == NULL))
		return -1;

	/* Computes the total PolyText item payload, splitting at 254 chars (255 is font-shift). */
	char_size = wide ? 2U : 1U;
	chunks = n == 0 ? 0U : ((size_t)n + 253U) / 254U;
	item_bytes = chunks * 2U + (size_t)n * char_size;
	z = (16U + item_bytes + 3U) & ~(size_t)3U;
	if (z > 262140U)
		return -1;

	/* Allocates the zero-padded request buffer. */
	q = calloc(1, z);
	if (!q)
		return -1;

	/* Writes the fixed PolyText header. */
	q[0] = op;
	q[1] = 0;
	w32(q + 4, w);
	w32(q + 8, g->xid);
	w16(q + 12, (uint16_t)x);
	w16(q + 14, (uint16_t)y);

	/* Packs the string into 254-character TEXTITEM chunks with zero delta. */
	src = (const uint8_t *)s;
	dst = q + 16;
	rem = (size_t)n;
	while (rem > 0U) {
		chunk = rem > 254U ? 254U : rem;
		chunk_bytes = chunk * char_size;
		dst[0] = (uint8_t)chunk;
		dst[1] = 0;
		memcpy(dst + 2, src, chunk_bytes);
		dst += 2U + chunk_bytes;
		src += chunk_bytes;
		rem -= chunk;
	}

	/* Sends the request and releases the temporary buffer. */
	status = req(d, q, z);
	free(q);

	/* Returns the write status. */
	return status;
}

/* Supports the event operation. */
static void
event(
	Display *d,
	const uint8_t *b,
	XEvent *e)
{
	memset(e, 0, sizeof(*e));
	e->type = b[0] & 0x7f;
	e->xany.display = d;
	e->xany.window = r32(b + 12);
	e->xany.serial = r16(b + 2);
	e->xany.send_event = (b[0] & 0x80) != 0;

	/* The keyboard's map changed: it is read again before the next key is looked up. */
	if (e->type == KEYMAP_MAPPING_NOTIFY)
		d->keymap_stale = 1;

	/* The selections' events. */
	if (e->type == SelectionClear) {
		e->xselectionclear.time = r32(b + 4);
		e->xselectionclear.window = r32(b + 8);
		e->xselectionclear.selection = r32(b + 12);
		return;
	}

	/* A request for a selection this client owns. */
	if (e->type == SelectionRequest) {
		e->xselectionrequest.time = r32(b + 4);
		e->xselectionrequest.owner = r32(b + 8);
		e->xselectionrequest.requestor = r32(b + 12);
		e->xselectionrequest.selection = r32(b + 16);
		e->xselectionrequest.target = r32(b + 20);
		e->xselectionrequest.property = r32(b + 24);
		return;
	}

	/* A conversion this client asked for is done. */
	if (e->type == SelectionNotify) {
		e->xselection.time = r32(b + 4);
		e->xselection.requestor = r32(b + 8);
		e->xselection.selection = r32(b + 12);
		e->xselection.target = r32(b + 16);
		e->xselection.property = r32(b + 20);
		return;
	}

	/* Handles the e condition. */
	if (e->type == Expose) {
		e->xexpose.window = r32(b + 4);
		e->xexpose.x = r16(b + 8);
		e->xexpose.y = r16(b + 10);
		e->xexpose.width = r16(b + 12);
		e->xexpose.height = r16(b + 14);
		e->xexpose.count = r16(b + 16);
	} else if (e->type == MapRequest) {
		e->xmaprequest.parent = r32(b + 4);
		e->xmaprequest.window = r32(b + 8);
	} else if (e->type == DestroyNotify) {
		e->xany.window = r32(b + 8);
	} else if (e->type == ConfigureNotify) {
		e->xconfigure.event = r32(b + 4);
		e->xconfigure.window = r32(b + 8);
		e->xconfigure.above = r32(b + 12);
		e->xconfigure.x = (int16_t)r16(b + 16);
		e->xconfigure.y = (int16_t)r16(b + 18);
		e->xconfigure.width = r16(b + 20);
		e->xconfigure.height = r16(b + 22);
		e->xconfigure.border_width = r16(b + 24);
		e->xconfigure.override_redirect = b[26];
	} else {
		e->xkey.keycode = b[1];
		e->xkey.time = r32(b + 4);
		e->xkey.root = r32(b + 8);
		e->xkey.window = r32(b + 12);
		e->xkey.subwindow = r32(b + 16);
		e->xkey.x_root = (int16_t)r16(b + 20);
		e->xkey.y_root = (int16_t)r16(b + 22);
		e->xkey.x = (int16_t)r16(b + 24);
		e->xkey.y = (int16_t)r16(b + 26);
		e->xkey.state = r16(b + 28);
		e->xkey.same_screen = b[30];
	}
}

/*
 * Reads the server's keyboard map: the keysyms of every keycode, and the
 * modifiers that are Mode_switch and Num_Lock and what Lock does.  A
 * server that answers neither leaves no map (no key has a keysym); one
 * that answers no modifier map leaves no modifier (xzed's).
 */
static void
keymap_load(
	Display *d)
{
	int error;

	/* Read once more only when asked again. */
	d->keymap_stale = 0;
	free(d->keysyms);
	d->keysyms = NULL;
	d->keysyms_per_keycode = 0U;
	d->mode_switch_mask = 0U;
	d->num_lock_mask = 0U;
	d->lock_meaning = KEYMAP_LOCK_NONE;

	/* The keysyms. */
	if (d->min_keycode == 0U || d->max_keycode < d->min_keycode)
		return;
	error = keymap_read_keysyms(d);
	if (error != 0)
		return;

	/* The modifiers (none when the server does not answer). */
	(void)keymap_read_modifiers(d);
}

/* Reads the keysyms of every keycode (GetKeyboardMapping).  Returns 0, or -1. */
static int
keymap_read_keysyms(
	Display *d)
{
	uint8_t q[8] = {0};
	uint8_t r[32];
	uint32_t *keysyms;
	uint8_t *bytes;
	size_t length;
	size_t count;
	size_t index;
	unsigned per;
	int failed;

	/* Every keycode the server has. */
	count = (size_t)d->max_keycode - d->min_keycode + 1U;
	q[0] = KEYMAP_GET_KEYBOARD_MAPPING;
	q[4] = d->min_keycode;
	q[5] = (uint8_t)count;
	failed = req(d, q, sizeof(q));
	if (failed)
		return -1;
	failed = reply(d, r);
	if (failed)
		return -1;

	/* The keysyms each keycode has, and their bytes. */
	per = r[1];
	length = (size_t)r32(r + 4) * 4U;
	if (per == 0U || length != count * per * 4U) {
		(void)keymap_discard(d, length);
		return -1;
	}
	bytes = malloc(length);
	if (bytes == NULL) {
		(void)keymap_discard(d, length);
		return -1;
	}
	failed = rd(d->fd, bytes, length);
	if (failed) {
		free(bytes);
		return -1;
	}

	/* The keysyms, kept in the host's order. */
	keysyms = malloc(count * per * sizeof(*keysyms));
	if (keysyms == NULL) {
		free(bytes);
		return -1;
	}
	for (index = 0U; index < count * per; index++)
		keysyms[index] = r32(bytes + index * 4U);
	free(bytes);

	/* Succeeded: the map. */
	d->keysyms = keysyms;
	d->keysyms_per_keycode = per;
	return 0;
}

/*
 * Reads the modifiers' keycodes (GetModifierMapping) and finds which
 * modifiers carry Mode_switch and Num_Lock, and whether Lock is Caps Lock
 * or Shift Lock.  Returns 0, or -1.
 */
static int
keymap_read_modifiers(
	Display *d)
{
	uint8_t q[4] = {0};
	uint8_t r[32];
	uint8_t *keycodes;
	size_t length;
	unsigned per;
	unsigned modifier;
	unsigned slot;
	unsigned column;
	unsigned keycode;
	uint32_t keysym;
	int failed;

	/* The request (a server without it answers with an error). */
	q[0] = KEYMAP_GET_MODIFIER_MAPPING;
	failed = req(d, q, sizeof(q));
	if (failed)
		return -1;
	failed = reply(d, r);
	if (failed)
		return -1;

	/* The keycodes, per of each modifier. */
	per = r[1];
	length = (size_t)r32(r + 4) * 4U;
	if (length != (size_t)per * KEYMAP_MODIFIERS) {
		(void)keymap_discard(d, length);
		return -1;
	}
	if (length == 0U)
		return 0;
	keycodes = malloc(length);
	if (keycodes == NULL) {
		(void)keymap_discard(d, length);
		return -1;
	}
	failed = rd(d->fd, keycodes, length);
	if (failed) {
		free(keycodes);
		return -1;
	}

	/* Each modifier's keys, by the keysyms they carry. */
	for (modifier = 0U; modifier < KEYMAP_MODIFIERS; modifier++) {
		for (slot = 0U; slot < per; slot++) {
			keycode = keycodes[modifier * per + slot];
			if (keycode == 0U)
				continue;
			for (column = 0U; column < d->keysyms_per_keycode; column++) {
				keysym = keymap_keysym(d, keycode, column);
				if (keysym == KEYMAP_MODE_SWITCH)
					d->mode_switch_mask |= 1U << modifier;
				if (keysym == KEYMAP_NUM_LOCK)
					d->num_lock_mask |= 1U << modifier;
				if (modifier == KEYMAP_LOCK && keysym == KEYMAP_CAPS_LOCK)
					d->lock_meaning = KEYMAP_LOCK_CAPS;
				if (modifier == KEYMAP_LOCK && keysym == KEYMAP_SHIFT_LOCK && d->lock_meaning == KEYMAP_LOCK_NONE)
					d->lock_meaning = KEYMAP_LOCK_SHIFT;
			}
		}
	}
	free(keycodes);

	/* Succeeded: the modifiers' meanings. */
	return 0;
}

/* Returns a keycode's keysym in a column (NoSymbol past those it has). */
static uint32_t
keymap_keysym(
	const Display *d,
	unsigned keycode,
	unsigned column)
{
	/* A keycode or column the map does not have. */
	if (d->keysyms == NULL || keycode < d->min_keycode || keycode > d->max_keycode)
		return 0U;
	if (column >= d->keysyms_per_keycode)
		return 0U;

	/* Succeeded: the keysym. */
	return d->keysyms[(size_t)(keycode - d->min_keycode) * d->keysyms_per_keycode + column];
}

/*
 * Gives a keysym's lower- and upper-case forms: a Latin-1 letter's two
 * cases, any other keysym itself twice.
 */
static void
keymap_case(
	uint32_t keysym,
	uint32_t *lower,
	uint32_t *upper)
{
	/* Itself, unless a letter. */
	*lower = keysym;
	*upper = keysym;

	/* The ASCII letters. */
	if (keysym >= 'a' && keysym <= 'z') {
		*upper = keysym - 0x20U;
		return;
	}
	if (keysym >= 'A' && keysym <= 'Z') {
		*lower = keysym + 0x20U;
		return;
	}

	/* Succeeded: the Latin-1 letters (but the multiplication and division signs). */
	if (keysym >= 0xe0U && keysym <= 0xfeU && keysym != 0xf7U)
		*upper = keysym - 0x20U;
	else if (keysym >= 0xc0U && keysym <= 0xdeU && keysym != 0xd7U)
		*lower = keysym + 0x20U;
}

/* Reads and drops the rest of a reply.  Returns 0, or -1. */
static int
keymap_discard(
	Display *d,
	size_t bytes)
{
	uint8_t chunk[256];
	size_t part;
	int failed;

	/* In chunks. */
	while (bytes > 0U) {
		part = bytes;
		if (part > sizeof(chunk))
			part = sizeof(chunk);
		failed = rd(d->fd, chunk, part);
		if (failed)
			return -1;
		bytes -= part;
	}

	/* Succeeded: dropped. */
	return 0;
}
