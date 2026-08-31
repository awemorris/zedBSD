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
#include <X11/Xzed.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
struct _XGC {
	uint32_t xid, foreground;
};
#define EVENT_QUEUE_SIZE 32U
struct _XDisplay {
	int fd;
	uint32_t base, next, root;
	unsigned long black, white;
	uint16_t sequence;
	uint8_t events[EVENT_QUEUE_SIZE][32];
	unsigned event_head, event_count;
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
	d->root = r32(b + 52);
	d->white = 0xffffff;

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
	w32(q + 24, 3);
	w32(q + 28, 3);
	w32(q + 32, (uint32_t)bc);
	w32(q + 36, (uint32_t)bg);

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
 * Implements the XzedPutImageRGB24 operation.
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
	unsigned row, column;
	size_t row_bytes;
	size_t payload, n, z;
	unsigned tile;
	uint8_t *q;
	int result;

	/* Checks the current descriptor. */
	if (!d || !pixels || !width || !height || width > 65535U ||
	    height > 65535U) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	row_bytes = (size_t)width * 3U;

	/* Handles the stride condition. */
	if (stride < row_bytes) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each element required by the operation. */
	for (row = 0; row < height; row++) {
		/* Process each element required by the operation. */
		for (column = 0; column < width;) {
			tile = width - column;

			/* Handles the tile condition. */
			if (tile > 640U)
				tile = 640U;
			payload = (size_t)tile * 3U;
			n = 16U + payload;
			z = (n + 3U) & ~3U;
			q = calloc(1, z);

			/* Handles the q condition. */
			if (!q)
				return -1;
			q[0] = 128;
			w32(q + 4, draw);
			w16(q + 8, (uint16_t)(x + (int)column));
			w16(q + 10, (uint16_t)(y + (int)row));
			w16(q + 12, (uint16_t)tile);
			w16(q + 14, 1);
			memcpy(q + 16U,
			       pixels + (size_t)row * stride +
				   (size_t)column * 3U,
			       payload);
			result = req(d, q, z);
			free(q);

			/* Checks the operation result. */
			if (result || XSync(d, False))
				return -1;
			column += tile;
		}
	}

	/* Reports successful completion. */
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

	/* Obtains the text req result. */
	function_result = text_req(d, 76, w, g, x, y, s, n, 0);

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

	/* Obtains the text req result. */
	function_result = text_req(d, 77, w, g, x, y, s, n, 1);

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
	uint8_t b[32];

	/* Checks the current descriptor. */
	if (d->event_count)
		return (int)d->event_count;
	f = fcntl(d->fd, F_GETFL);
	fcntl(d->fd, F_SETFL, f | O_NONBLOCK);
	z = (int)recv(d->fd, b, 32, MSG_DONTWAIT);
	fcntl(d->fd, F_SETFL, f);

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
 * Implements the XLookupKeysym operation.
 */
KeySym
XLookupKeysym(
	XKeyEvent *e,
	int index)
{
	KeyCode k;

	/* Handles the e condition. */
	if (!e || index != 0)
		return None;
	k = (KeyCode)e->keycode;

	/* Dispatch the selected operation case. */
	switch (k) {
	case 0xe0:
		/* Returns the computed result. */
		return XK_Up;
	case 0xe1:
		/* Returns the computed result. */
		return XK_Down;
	case 0xe2:
		/* Returns the computed result. */
		return XK_Left;
	case 0xe3:
		/* Returns the computed result. */
		return XK_Right;
	case 0xe4:
		/* Returns the computed result. */
		return XK_Home;
	case 0xe5:
		/* Returns the computed result. */
		return XK_End;
	case 0xe6:
		/* Returns the computed result. */
		return XK_Page_Up;
	case 0xe7:
		/* Returns the computed result. */
		return XK_Page_Down;
	case 0xe8:
		/* Returns the computed result. */
		return XK_Insert;
	case 0xe9:
		/* Returns the computed result. */
		return XK_Delete;
	default:
		/* Returns the computed result. */
		return k >= 8 ? (KeySym)(k - 8) : None;
	}
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

/* Supports the text req operation. */
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
	size_t bytes, z;
	uint8_t *q;

	bytes = (size_t)n * (wide ? 2U : 1U);
	z = (16 + bytes + 3) & ~3U;

	/* Checks the current item count. */
	if (n < 0 || n > 255)
		return -1;
	q = calloc(1, z);

	/* Handles the q condition. */
	if (!q)
		return -1;
	q[0] = op;
	q[1] = (uint8_t)n;
	w32(q + 4, w);
	w32(q + 8, g->xid);
	w16(q + 12, (uint16_t)x);
	w16(q + 14, (uint16_t)y);
	memcpy(q + 16, s, bytes);
	n = req(d, q, z);
	free(q);

	/* Returns the computed result. */
	return n;
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
