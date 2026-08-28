/*
 * Xzed - small local X11 server for zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Wire constants and layouts follow the public X11 core protocol.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <zedbsd/graphics.h>

#include "userland/X11/xzed/input.h"
#include "userland/X11/xzed/pointer.h"

#define MAX_CLIENTS 8
#define MAX_WINDOWS 64
#define MAX_GCS 64
#define MAX_FONTS 32
#define MAX_PIXMAPS 16
#define INPUT_CAP (1024U * 1024U)
#define ROOT_XID 1U
#define COLORMAP_XID 2U
#define VISUAL_XID 3U
#define XZED_KEYCODE_UP 0xe0U
#define XZED_KEYCODE_DOWN 0xe1U
#define XZED_KEYCODE_LEFT 0xe2U
#define XZED_KEYCODE_RIGHT 0xe3U
#define XZED_KEYCODE_HOME 0xe4U
#define XZED_KEYCODE_END 0xe5U
#define XZED_KEYCODE_PAGE_UP 0xe6U
#define XZED_KEYCODE_PAGE_DOWN 0xe7U
#define XZED_KEYCODE_INSERT 0xe8U
#define XZED_KEYCODE_DELETE 0xe9U
#define XZED_ICON_PATH_ATOM 0x5a000001U
#define Above 0U
#define CURSOR_WIDTH 16
#define CURSOR_HEIGHT 16
#define CURSOR_HOT_X 3
#define CURSOR_HOT_Y 1
#define XC_BOTTOM_LEFT_CORNER 12U
#define XC_BOTTOM_RIGHT_CORNER 14U
#define XC_LEFT_PTR 68U
#define XC_SB_H_DOUBLE_ARROW 108U
#define XC_SB_V_DOUBLE_ARROW 116U

/*
 * Standard X11 left_ptr source and mask bitmaps.  A source bit is black; a
 * mask-only bit is white.  The bitmap's defined hot spot is (3, 1).
 */
static const uint16_t pointer_source[CURSOR_HEIGHT] = {
    0x0000, 0x0008, 0x0018, 0x0038, 0x0078, 0x00f8, 0x01f8, 0x03f8,
    0x07f8, 0x00f8, 0x00d8, 0x0188, 0x0180, 0x0300, 0x0300, 0x0000};
static const uint16_t pointer_mask[CURSOR_HEIGHT] = {
    0x000c, 0x001c, 0x003c, 0x007c, 0x00fc, 0x01fc, 0x03fc, 0x07fc,
    0x0ffc, 0x0ffc, 0x01fc, 0x03dc, 0x03cc, 0x0780, 0x0780, 0x0300};

/* Protocol resources are kept in fixed-size tables to avoid a general XID map.
 */

struct client {
	int fd;
	int order;
	int setup;
	uint16_t sequence;
	uint32_t base;
	uint8_t *input;
	size_t used;
	size_t capacity;
};

struct window {
	uint32_t id;
	uint32_t parent;
	uint32_t owner;
	uint32_t event_mask;
	uint32_t background;
	int16_t x;
	int16_t y;
	uint16_t width;
	uint16_t height;
	uint16_t border;
	int mapped;
	uint16_t cursor_shape;
	uint16_t input_left;
	uint16_t input_top;
	uint16_t input_right;
	uint16_t input_bottom;
	/* Retained contents let the server reveal a window without repainting
	 * it. */
	uint32_t *pixels;
	char name[64];
	char icon_path[160];
};

struct graphics_context {
	uint32_t id;
	uint32_t foreground;
	uint32_t font;
	int owner;
};

struct font_resource {
	uint32_t id;
	int owner;
};

struct pixmap {
	uint32_t id;
	int owner;
	uint16_t width;
	uint16_t height;
	uint32_t *pixels;
};

struct server {
	int listener;
	int graphics;
	struct xzed_input *input;
	struct graphics_mode mode;
	struct client clients[MAX_CLIENTS];
	struct window windows[MAX_WINDOWS];
	unsigned window_count;
	struct graphics_context gcs[MAX_GCS];
	unsigned gc_count;
	struct font_resource fonts[MAX_FONTS];
	unsigned font_count;
	struct pixmap pixmaps[MAX_PIXMAPS];
	unsigned pixmap_count;
	uint32_t *screen;
	uint8_t *transfer;
	/* Dirty coordinates form one half-open screen-space bounding box. */
	int dirty;
	int dirty_x0;
	int dirty_y0;
	int dirty_x1;
	int dirty_y1;
	int pointer_x;
	int pointer_y;
	uint32_t buttons;
	uint32_t key_state;
	uint32_t focus;
	int pointer_grab_owner;
	uint32_t pointer_grab_window;
	struct window *pending_motion_window;
	struct client *pending_motion_client;
	uint32_t pending_motion_time;
	int pending_motion_x;
	int pending_motion_y;
	uint16_t pending_motion_buttons;
	int pending_motion;
	int pointer_dirty;
	int pointer_old_x;
	int pointer_old_y;
};

static volatile int stopped;
static void mark_dirty(struct server *, int, int, int, int);

/* X11 requests use the byte order selected by each client during setup. */

static uint16_t
rd16(const uint8_t *p, int msb)
{
	return msb ? (uint16_t)((p[0] << 8) | p[1])
		   : (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
rd32(const uint8_t *p, int msb)
{
	return msb ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
			 ((uint32_t)p[2] << 8) | p[3]
		   : (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
			 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
wr16(uint8_t *p, uint16_t v, int msb)
{
	if (msb) {
		p[0] = (uint8_t)(v >> 8);
		p[1] = (uint8_t)v;
	} else {
		p[0] = (uint8_t)v;
		p[1] = (uint8_t)(v >> 8);
	}
}

static void
wr32(uint8_t *p, uint32_t v, int msb)
{
	if (msb) {
		p[0] = (uint8_t)(v >> 24);
		p[1] = (uint8_t)(v >> 16);
		p[2] = (uint8_t)(v >> 8);
		p[3] = (uint8_t)v;
	} else {
		p[0] = (uint8_t)v;
		p[1] = (uint8_t)(v >> 8);
		p[2] = (uint8_t)(v >> 16);
		p[3] = (uint8_t)(v >> 24);
	}
}

static int
write_all(int fd, const void *v, size_t n)
{
	const uint8_t *p = v;
	while (n) {
		ssize_t r = write(fd, p, n);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!r) {
			errno = EIO;
			return -1;
		}
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

static void
on_signal(int sig)
{
	(void)sig;
	stopped = 1;
}

/* Resource lookup and stacking order. */

static struct window *
find_window(struct server *s, uint32_t id)
{
	unsigned i;
	for (i = 0; i < s->window_count; i++)
		if (s->windows[i].id == id)
			return &s->windows[i];
	return NULL;
}

static struct window *
top_level_window(struct server *s, struct window *w)
{
	struct window *p;
	while (w && w->parent != ROOT_XID) {
		p = find_window(s, w->parent);
		if (!p || p == w)
			break;
		w = p;
	}
	return w;
}

static void
raise_window(struct server *s, struct window *w)
{
	struct window saved;
	size_t index;
	if (!w || w == &s->windows[0])
		return;
	index = (size_t)(w - s->windows);
	if (index >= s->window_count || index + 1U == s->window_count)
		return;
	mark_dirty(s, w->x, w->y, w->width, w->height);
	saved = *w;
	memmove(&s->windows[index], &s->windows[index + 1],
		(s->window_count - index - 1U) * sizeof(s->windows[0]));
	s->windows[s->window_count - 1U] = saved;
	mark_dirty(s, saved.x, saved.y, saved.width, saved.height);
}

static struct client *
owner_client(struct server *s, uint32_t owner)
{
	return owner < MAX_CLIENTS && s->clients[owner].fd >= 0
		   ? &s->clients[owner]
		   : NULL;
}

static struct graphics_context *
find_gc(struct server *s, uint32_t id)
{
	unsigned i;
	for (i = 0; i < s->gc_count; i++)
		if (s->gcs[i].id == id)
			return &s->gcs[i];
	return NULL;
}

static struct pixmap *
find_pixmap(struct server *s, uint32_t id)
{
	unsigned i;
	for (i = 0; i < s->pixmap_count; i++)
		if (s->pixmaps[i].id == id)
			return &s->pixmaps[i];
	return NULL;
}

static void
mark_dirty(struct server *s, int x, int y, int w, int h)
{
	/* Coalesce all changes until present() into one clipped screen region.
	 */
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > (int)s->mode.width)
		w = (int)s->mode.width - x;
	if (y + h > (int)s->mode.height)
		h = (int)s->mode.height - y;
	if (w <= 0 || h <= 0)
		return;
	if (!s->dirty) {
		s->dirty = 1;
		s->dirty_x0 = x;
		s->dirty_y0 = y;
		s->dirty_x1 = x + w;
		s->dirty_y1 = y + h;
		return;
	}
	if (x < s->dirty_x0)
		s->dirty_x0 = x;
	if (y < s->dirty_y0)
		s->dirty_y0 = y;
	if (x + w > s->dirty_x1)
		s->dirty_x1 = x + w;
	if (y + h > s->dirty_y1)
		s->dirty_y1 = y + h;
}

/*
 * Visual hit testing follows mapped child windows.  Input hit testing also
 * honors the private margins used by the window manager's resize handles.
 */

static struct window *
top_at_parent(struct server *s, uint32_t parent, int x, int y)
{
	unsigned i = s->window_count;
	while (i-- > 1) {
		struct window *w = &s->windows[i];
		if (w->mapped && w->parent == parent && x >= w->x &&
		    y >= w->y && x < w->x + w->width && y < w->y + w->height) {
			struct window *child = top_at_parent(s, w->id, x, y);
			return child ? child : w;
		}
	}
	return NULL;
}

static struct window *
top_at(struct server *s, int x, int y)
{
	struct window *w = top_at_parent(s, ROOT_XID, x, y);
	return w ? w : &s->windows[0];
}

static struct window *
input_at_parent(struct server *s, uint32_t parent, int x, int y)
{
	unsigned i = s->window_count;
	while (i-- > 1) {
		struct window *w = &s->windows[i];
		int inside;
		if (!w->mapped || w->parent != parent)
			continue;
		inside = x >= w->x - (int)w->input_left &&
			 y >= w->y - (int)w->input_top &&
			 x < w->x + w->width + (int)w->input_right &&
			 y < w->y + w->height + (int)w->input_bottom;
		if (inside) {
			if (x >= w->x && y >= w->y && x < w->x + w->width &&
			    y < w->y + w->height) {
				struct window *child =
				    input_at_parent(s, w->id, x, y);
				if (child)
					return child;
			}
			return w;
		}
	}
	return NULL;
}

static struct window *
input_at(struct server *s, int x, int y)
{
	struct window *w = input_at_parent(s, ROOT_XID, x, y);
	return w ? w : &s->windows[0];
}

static uint16_t
pointer_shape(struct server *s)
{
	struct window *w = s->pointer_grab_owner >= 0
			       ? find_window(s, s->pointer_grab_window)
			       : input_at(s, s->pointer_x, s->pointer_y);
	while (w && !w->cursor_shape && w->parent)
		w = find_window(s, w->parent);
	return w && w->cursor_shape ? w->cursor_shape : XC_LEFT_PTR;
}

/* Software cursor rendering and per-window backing stores. */

static int
resize_cursor_black(uint16_t shape, int x, int y)
{
	int dx = x - 7, dy = y - 7, adx = abs(dx), ady = abs(dy);
	if (shape == XC_SB_H_DOUBLE_ARROW)
		return (ady <= 1 && x >= 2 && x <= 13) ||
		       (x <= 6 && adx + ady <= 5) || (x >= 9 && adx + ady <= 6);
	if (shape == XC_SB_V_DOUBLE_ARROW)
		return (adx <= 1 && y >= 2 && y <= 13) ||
		       (y <= 6 && adx + ady <= 5) || (y >= 9 && adx + ady <= 6);
	if (shape == XC_BOTTOM_LEFT_CORNER)
		return (abs(x + y - 15) <= 1 && x >= 2 && x <= 13 && y >= 2 &&
			y <= 13) ||
		       (x == 2 && y >= 8 && y <= 13) ||
		       (y == 13 && x >= 2 && x <= 7) ||
		       (x == 13 && y >= 2 && y <= 7) ||
		       (y == 2 && x >= 8 && x <= 13);
	if (shape == XC_BOTTOM_RIGHT_CORNER)
		return (abs(x - y) <= 1 && x >= 2 && x <= 13 && y >= 2 &&
			y <= 13) ||
		       (x == 2 && y >= 2 && y <= 7) ||
		       (y == 2 && x >= 2 && x <= 7) ||
		       (x == 13 && y >= 8 && y <= 13) ||
		       (y == 13 && x >= 8 && x <= 13);
	return 0;
}

static int
cursor_pixel(uint16_t shape, int x, int y, uint32_t *color)
{
	int nx, ny;
	if (shape == XC_LEFT_PTR) {
		uint16_t bit = (uint16_t)(1U << (unsigned)x);
		if (!(pointer_mask[y] & bit))
			return 0;
		*color = (pointer_source[y] & bit) ? 0x000000 : 0xffffff;
		return 1;
	}
	if (resize_cursor_black(shape, x, y)) {
		*color = 0x000000;
		return 1;
	}
	for (ny = y - 1; ny <= y + 1; ny++)
		for (nx = x - 1; nx <= x + 1; nx++)
			if (nx >= 0 && ny >= 0 && nx < CURSOR_WIDTH &&
			    ny < CURSOR_HEIGHT &&
			    resize_cursor_black(shape, nx, ny)) {
				*color = 0xffffff;
				return 1;
			}
	return 0;
}

static uint32_t *
window_pixels_alloc(uint16_t width, uint16_t height, uint32_t color)
{
	size_t count = (size_t)width * height, i;
	uint32_t *p;
	if (!width || !height || (width && count / width != height) ||
	    count > SIZE_MAX / sizeof(*p))
		return NULL;
	p = malloc(count * sizeof(*p));
	if (!p)
		return NULL;
	for (i = 0; i < count; i++)
		p[i] = color;
	return p;
}

static int
window_pixels_resize(struct window *w, uint16_t width, uint16_t height)
{
	uint32_t *p;
	unsigned copy_width, copy_height, row;
	if (width == w->width && height == w->height)
		return 0;
	p = window_pixels_alloc(width, height, w->background);
	if (!p)
		return -1;
	copy_width = width < w->width ? width : w->width;
	copy_height = height < w->height ? height : w->height;
	for (row = 0; row < copy_height; row++)
		memcpy(p + (size_t)row * width,
		       w->pixels + (size_t)row * w->width,
		       (size_t)copy_width * sizeof(*p));
	free(w->pixels);
	w->pixels = p;
	return 0;
}

static int
window_pixels_resize_buffered(struct window *w, uint16_t width, uint16_t height,
			      int x_offset, int y_offset)
{
	uint32_t *p;
	int source_x = 0, source_y = 0, dest_x = x_offset, dest_y = y_offset;
	int copy_width = w->width, copy_height = w->height, row;
	if (width == w->width && height == w->height && x_offset == 0 &&
	    y_offset == 0)
		return 0;
	p = window_pixels_alloc(width, height, w->background);
	if (!p)
		return -1;
	if (dest_x < 0) {
		source_x = -dest_x;
		copy_width -= source_x;
		dest_x = 0;
	}
	if (dest_y < 0) {
		source_y = -dest_y;
		copy_height -= source_y;
		dest_y = 0;
	}
	if (dest_x + copy_width > (int)width)
		copy_width = (int)width - dest_x;
	if (dest_y + copy_height > (int)height)
		copy_height = (int)height - dest_y;
	if (copy_width > 0 && copy_height > 0)
		for (row = 0; row < copy_height; row++)
			memcpy(p + (size_t)(dest_y + row) * width +
				   (unsigned)dest_x,
			       w->pixels + (size_t)(source_y + row) * w->width +
				   (unsigned)source_x,
			       (size_t)copy_width * sizeof(*p));
	free(w->pixels);
	w->pixels = p;
	return 0;
}

/* Compositing and graphics-device presentation. */

static void
composite_region(struct server *s, int x, int y, int width, int height)
{
	/* Resolve each dirty pixel from the topmost retained window contents.
	 */
	int row, column;
	if (x < 0) {
		width += x;
		x = 0;
	}
	if (y < 0) {
		height += y;
		y = 0;
	}
	if (x + width > (int)s->mode.width)
		width = (int)s->mode.width - x;
	if (y + height > (int)s->mode.height)
		height = (int)s->mode.height - y;
	if (width <= 0 || height <= 0)
		return;
	for (row = y; row < y + height; row++)
		for (column = x; column < x + width; column++) {
			struct window *w = top_at(s, column, row);
			int local_x = column - w->x, local_y = row - w->y;
			s->screen[(size_t)row * s->mode.width + column] =
			    w->pixels[(size_t)local_y * w->width + local_x];
		}
}

static void
window_fill(struct server *s, struct window *w, int x, int y, int width,
	    int height, uint32_t color)
{
	int row, column;
	if (x < 0) {
		width += x;
		x = 0;
	}
	if (y < 0) {
		height += y;
		y = 0;
	}
	if (x + width > w->width)
		width = w->width - x;
	if (y + height > w->height)
		height = w->height - y;
	if (width <= 0 || height <= 0)
		return;
	for (row = y; row < y + height; row++)
		for (column = x; column < x + width; column++)
			w->pixels[(size_t)row * w->width + column] = color;
	mark_dirty(s, w->x + x, w->y + y, width, height);
}

static void
pixmap_fill(struct pixmap *p, int x, int y, int width, int height,
	    uint32_t color)
{
	int row, column;
	if (x < 0) {
		width += x;
		x = 0;
	}
	if (y < 0) {
		height += y;
		y = 0;
	}
	if (x + width > p->width)
		width = p->width - x;
	if (y + height > p->height)
		height = p->height - y;
	if (width <= 0 || height <= 0)
		return;
	for (row = y; row < y + height; row++)
		for (column = x; column < x + width; column++)
			p->pixels[(size_t)row * p->width + column] = color;
}

static void
present(struct server *s)
{
	struct graphics_blit b;
	struct graphics_rect r;
	struct graphics_flush f;
	uint16_t shape = pointer_shape(s);
	int hot_x = shape == XC_LEFT_PTR ? CURSOR_HOT_X : 7,
	    hot_y = shape == XC_LEFT_PTR ? CURSOR_HOT_Y : 7;
	int x, y, w, h;
	if (!s->dirty)
		return;
	x = s->dirty_x0;
	y = s->dirty_y0;
	w = s->dirty_x1 - x;
	h = s->dirty_y1 - y;
	s->dirty = 0;
	composite_region(s, x, y, w, h);
	/* The cursor is transient: overlay it only in the RGB24 transfer
	 * buffer. */
	for (int row = 0; row < h; row++)
		for (int column = 0; column < w; column++) {
			int ax = x + column, ay = y + row;
			int cx = ax - (s->pointer_x - hot_x);
			int cy = ay - (s->pointer_y - hot_y);
			uint32_t color =
			    s->screen[(size_t)ay * s->mode.width + ax];
			size_t off = ((size_t)row * w + column) * 3;
			if (cx >= 0 && cx < CURSOR_WIDTH && cy >= 0 &&
			    cy < CURSOR_HEIGHT)
				(void)cursor_pixel(shape, cx, cy, &color);
			s->transfer[off] = (uint8_t)(color >> 16);
			s->transfer[off + 1] = (uint8_t)(color >> 8);
			s->transfer[off + 2] = (uint8_t)color;
		}
	memset(&b, 0, sizeof(b));
	b.x = (uint32_t)x;
	b.y = (uint32_t)y;
	b.width = (uint32_t)w;
	b.height = (uint32_t)h;
	b.format = ZEDBSD_GRAPHICS_FORMAT_RGB24;
	b.stride = (uint32_t)w * 3;
	b.pixels = (uapi_ptr_t)(uintptr_t)s->transfer;
	(void)ioctl(s->graphics, ZEDBSD_GRAPHICS_BLIT, &b);
	r = (struct graphics_rect){(uint32_t)x, (uint32_t)y, (uint32_t)w,
				   (uint32_t)h};
	f = (struct graphics_flush){(uapi_ptr_t)(uintptr_t)&r, 1};
	(void)ioctl(s->graphics, ZEDBSD_GRAPHICS_FLUSH, &f);
}

static int
draw_text(struct server *s, struct window *w, struct graphics_context *g, int x,
	  int y, const uint8_t *text, size_t count, int wide)
{
	uint8_t bitmap[32];
	size_t i;
	uint32_t color = g ? g->foreground : 0xffffff;
	for (i = 0; i < count; i++) {
		struct graphics_glyph q;
		uint32_t cp =
		    wide ? ((uint32_t)text[i * 2] << 8) | text[i * 2 + 1]
			 : text[i];
		int gx, gy, top;
		memset(&q, 0, sizeof(q));
		q.codepoint = cp;
		q.bitmap = (uapi_ptr_t)(uintptr_t)bitmap;
		q.bitmap_capacity = sizeof(bitmap);
		if (ioctl(s->graphics, ZEDBSD_GRAPHICS_GET_GLYPH, &q))
			continue;
		top = y - (int)q.height;
		for (gy = 0; gy < (int)q.height; gy++)
			for (gx = 0; gx < (int)q.width; gx++)
				if (x + gx >= 0 && x + gx < w->width &&
				    top + gy >= 0 && top + gy < w->height &&
				    (bitmap[(size_t)gy * q.stride +
					    (unsigned)gx / 8] &
				     (0x80U >> ((unsigned)gx & 7))))
					w->pixels[(size_t)(top + gy) *
						      w->width +
						  (x + gx)] = color;
		mark_dirty(s, w->x + x, w->y + top, (int)q.width,
			   (int)q.height);
		x += (int)(q.advance ? q.advance : q.width);
	}
	return 0;
}

static void
repaint(struct server *s)
{
	mark_dirty(s, 0, 0, (int)s->mode.width, (int)s->mode.height);
}

/* X11 event and reply construction. */

static void
send_event(struct client *c, uint8_t type, uint32_t window, uint32_t detail,
	   uint32_t time, int16_t rx, int16_t ry, uint16_t state)
{
	uint8_t e[32];
	memset(e, 0, sizeof(e));
	e[0] = type;
	e[1] = (uint8_t)detail;
	wr16(e + 2, c->sequence, c->order);
	wr32(e + 4, time, c->order);
	wr32(e + 8, ROOT_XID, c->order);
	wr32(e + 12, window, c->order);
	wr32(e + 16, 0, c->order);
	wr16(e + 20, (uint16_t)rx, c->order);
	wr16(e + 22, (uint16_t)ry, c->order);
	wr16(e + 24, (uint16_t)rx, c->order);
	wr16(e + 26, (uint16_t)ry, c->order);
	wr16(e + 28, state, c->order);
	e[30] = 1;
	(void)write_all(c->fd, e, sizeof(e));
}

static void
send_motion_event(struct client *c, struct window *w, uint64_t time, int x,
		  int y, uint16_t buttons)
{
	if (c && w && (w->event_mask & (1U << 6)))
		send_event(c, 6, w->id, 0, (uint32_t)(time / 1000000), x, y,
			   buttons);
}

static void
expose_area(struct server *s, struct window *w, int x, int y, int width,
	    int height)
{
	struct client *c = owner_client(s, w->owner);
	uint8_t e[32];
	if (!c || !(w->event_mask & (1U << 15)))
		return;
	if (x < 0) {
		width += x;
		x = 0;
	}
	if (y < 0) {
		height += y;
		y = 0;
	}
	if (x + width > w->width)
		width = w->width - x;
	if (y + height > w->height)
		height = w->height - y;
	if (width <= 0 || height <= 0)
		return;
	memset(e, 0, sizeof(e));
	e[0] = 12;
	wr16(e + 2, c->sequence, c->order);
	wr32(e + 4, w->id, c->order);
	wr16(e + 8, (uint16_t)x, c->order);
	wr16(e + 10, (uint16_t)y, c->order);
	wr16(e + 12, (uint16_t)width, c->order);
	wr16(e + 14, (uint16_t)height, c->order);
	(void)write_all(c->fd, e, 32);
}

static void
configure_notify(struct server *s, struct window *w)
{
	struct client *c = owner_client(s, w->owner);
	uint8_t e[32];
	if (!c || !(w->event_mask & (1U << 17)))
		return;
	memset(e, 0, sizeof(e));
	e[0] = 22;
	wr16(e + 2, c->sequence, c->order);
	wr32(e + 4, w->id, c->order);
	wr32(e + 8, w->id, c->order);
	wr32(e + 12, 0, c->order);
	wr16(e + 16, (uint16_t)w->x, c->order);
	wr16(e + 18, (uint16_t)w->y, c->order);
	wr16(e + 20, w->width, c->order);
	wr16(e + 22, w->height, c->order);
	wr16(e + 24, w->border, c->order);
	(void)write_all(c->fd, e, sizeof(e));
}

static void
expose(struct server *s, struct window *w)
{
	expose_area(s, w, 0, 0, w->width, w->height);
	configure_notify(s, w);
}

static void
map_request(struct server *s, struct window *parent, struct window *w)
{
	struct client *c = owner_client(s, parent->owner);
	uint8_t e[32];
	if (!c)
		return;
	memset(e, 0, sizeof(e));
	e[0] = 20;
	wr16(e + 2, c->sequence, c->order);
	wr32(e + 4, parent->id, c->order);
	wr32(e + 8, w->id, c->order);
	(void)write_all(c->fd, e, sizeof(e));
}

static void
destroy_notify(struct client *c, uint32_t event, uint32_t window)
{
	uint8_t e[32];
	if (!c)
		return;
	memset(e, 0, sizeof(e));
	e[0] = 17;
	wr16(e + 2, c->sequence, c->order);
	wr32(e + 4, event, c->order);
	wr32(e + 8, window, c->order);
	(void)write_all(c->fd, e, sizeof(e));
}

static void
destroy_window(struct server *s, uint32_t id)
{
	struct window *w, *parent;
	size_t index;
	uint32_t child;
	if (id == ROOT_XID || (w = find_window(s, id)) == NULL)
		return;
	/* Children must disappear first so no resource retains a dead parent.
	 */
	for (;;) {
		unsigned i;
		child = 0;
		for (i = 1; i < s->window_count; i++)
			if (s->windows[i].parent == id) {
				child = s->windows[i].id;
				break;
			}
		if (!child)
			break;
		destroy_window(s, child);
	}
	w = find_window(s, id);
	if (!w)
		return;
	parent = find_window(s, w->parent);
	if ((w->event_mask & (1U << 17)) != 0)
		destroy_notify(owner_client(s, w->owner), w->id, w->id);
	if (parent && (parent->event_mask & (1U << 19)) != 0)
		destroy_notify(owner_client(s, parent->owner), parent->id,
			       w->id);
	mark_dirty(s, w->x, w->y, w->width, w->height);
	if (s->focus == w->id)
		s->focus = ROOT_XID;
	if (s->pointer_grab_window == w->id) {
		s->pointer_grab_owner = -1;
		s->pointer_grab_window = 0;
	}
	free(w->pixels);
	w->pixels = NULL;
	index = (size_t)(w - s->windows);
	if (index + 1U < s->window_count)
		memmove(&s->windows[index], &s->windows[index + 1],
			(s->window_count - index - 1U) * sizeof(s->windows[0]));
	s->window_count--;
	memset(&s->windows[s->window_count], 0, sizeof(s->windows[0]));
}

static struct window *
hit(struct server *s, int x, int y)
{
	return input_at(s, x, y);
}

static int
setup_reply(struct server *s, struct client *c)
{
	/* Describe the single 24-bit TrueColor screen exposed by Xzed. */
	static const char vendor[] = "zedBSD Xzed";
	uint8_t out[8 + 32 + 12 + 8 + 40 + 8 + 24];
	uint8_t *p = out + 8;
	uint16_t units;
	memset(out, 0, sizeof(out));
	out[0] = 1;
	wr16(out + 2, 11, c->order);
	wr16(out + 4, 0, c->order);
	units = (uint16_t)((sizeof(out) - 8) / 4);
	wr16(out + 6, units, c->order);
	wr32(p, 1, c->order);
	wr32(p + 4, c->base, c->order);
	wr32(p + 8, 0x003fffff, c->order);
	wr16(p + 16, sizeof(vendor) - 1, c->order);
	wr16(p + 18, 65535, c->order);
	p[20] = 1;
	p[21] = 1;
	p[22] = c->order ? 1 : 0;
	p[23] = 1;
	p[24] = 32;
	p[25] = 32;
	p[26] = 8;
	p[27] = 255;
	p += 32;
	memcpy(p, vendor, sizeof(vendor) - 1);
	p += 12;
	p[0] = 24;
	p[1] = 32;
	p[2] = 32;
	p += 8;
	wr32(p, ROOT_XID, c->order);
	wr32(p + 4, COLORMAP_XID, c->order);
	wr32(p + 8, 0xffffff, c->order);
	wr32(p + 12, 0, c->order);
	wr32(p + 16, 0x00ffffff, c->order);
	wr16(p + 20, (uint16_t)s->mode.width, c->order);
	wr16(p + 22, (uint16_t)s->mode.height, c->order);
	wr16(p + 24, (uint16_t)(s->mode.width * 254 / 96 / 10), c->order);
	wr16(p + 26, (uint16_t)(s->mode.height * 254 / 96 / 10), c->order);
	wr16(p + 28, 1, c->order);
	wr16(p + 30, 1, c->order);
	wr32(p + 32, VISUAL_XID, c->order);
	p[36] = 0;
	p[38] = 24;
	p[39] = 1;
	p += 40;
	p[0] = 24;
	wr16(p + 2, 1, c->order);
	p += 8;
	wr32(p, VISUAL_XID, c->order);
	p[4] = 4;
	p[5] = 8;
	wr16(p + 6, 256, c->order);
	wr32(p + 8, 0x00ff0000, c->order);
	wr32(p + 12, 0x0000ff00, c->order);
	wr32(p + 16, 0x000000ff, c->order);
	return write_all(c->fd, out, sizeof(out));
}

static void
error_reply(struct client *c, uint8_t code, uint32_t resource, uint8_t opcode)
{
	uint8_t e[32];
	memset(e, 0, 32);
	e[0] = 0;
	e[1] = code;
	wr16(e + 2, c->sequence, c->order);
	wr32(e + 4, resource, c->order);
	e[10] = opcode;
	(void)write_all(c->fd, e, 32);
}

static void
simple_reply(struct client *c, uint8_t *r, size_t n)
{
	r[0] = 1;
	wr16(r + 2, c->sequence, c->order);
	if (n >= 32)
		wr32(r + 4, (uint32_t)((n - 32) / 4), c->order);
	(void)write_all(c->fd, r, n);
}

/*
 * Dispatch the supported core requests plus Xzed's private opcodes.  A case
 * returns after handling a valid request; falling through the switch emits
 * BadWindow/BadValue using the request's resource field.
 */

static int
request(struct server *s, unsigned ci, const uint8_t *q, size_t n)
{
	struct client *c = &s->clients[ci];
	uint8_t op = q[0];
	uint32_t id;
	struct window *w;
	c->sequence++;
	switch (op) {
	case 1: /* CreateWindow */
		if (n < 32 || s->window_count == MAX_WINDOWS)
			break;
		id = rd32(q + 4, c->order);
		if (find_window(s, id)) {
			error_reply(c, 14, id, op);
			return 0;
		}
		w = &s->windows[s->window_count++];
		memset(w, 0, sizeof(*w));
		w->id = id;
		w->owner = ci;
		w->parent = rd32(q + 8, c->order);
		w->x = (int16_t)rd16(q + 12, c->order);
		w->y = (int16_t)rd16(q + 14, c->order);
		w->width = rd16(q + 16, c->order);
		w->height = rd16(q + 18, c->order);
		w->border = rd16(q + 20, c->order);
		w->background = 0x607080;
		{
			uint32_t mask = rd32(q + 28, c->order);
			size_t off = 32;
			unsigned bit;
			for (bit = 0; bit < 32 && off + 4 <= n; bit++)
				if (mask & (1U << bit)) {
					uint32_t v = rd32(q + off, c->order);
					if (bit == 1)
						w->background = v;
					if (bit == 11)
						w->event_mask = v;
					off += 4;
				}
		}
		w->pixels =
		    window_pixels_alloc(w->width, w->height, w->background);
		if (!w->pixels) {
			s->window_count--;
			memset(w, 0, sizeof(*w));
			error_reply(c, 11, id, op);
			return 0;
		}
		return 0;
	case 2: /* ChangeWindowAttributes */
		if (n < 12 ||
		    (w = find_window(s, rd32(q + 4, c->order))) == NULL)
			break;
		{
			uint32_t mask = rd32(q + 8, c->order);
			size_t off = 12;
			unsigned bit;
			for (bit = 0; bit < 32 && off + 4 <= n; bit++)
				if (mask & (1U << bit)) {
					uint32_t v = rd32(q + off, c->order);
					if (bit == 1)
						w->background = v;
					if (bit == 11) {
						w->event_mask = v;
						if (w == &s->windows[0])
							w->owner = ci;
					}
					off += 4;
				}
		}
		return 0;
	case 4: /* DestroyWindow */
		destroy_window(s, rd32(q + 4, c->order));
		return 0;
	case 7: /* ReparentWindow */
		if (n >= 16 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			struct window *p =
			    find_window(s, rd32(q + 8, c->order));
			if (p) {
				w->parent = p->id;
				w->x = (int16_t)(p->x + (int16_t)rd16(
							    q + 12, c->order));
				w->y = (int16_t)(p->y + (int16_t)rd16(
							    q + 14, c->order));
				return 0;
			}
		}
		break;
	case 8: /* MapWindow */
		if ((w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			struct window *p = find_window(s, w->parent);
			if (p && (p->event_mask & (1U << 20)) &&
			    p->owner != ci) {
				map_request(s, p, w);
				return 0;
			}
			w->mapped = 1;
			mark_dirty(s, w->x, w->y, w->width, w->height);
			expose(s, w);
			return 0;
		}
		break;
	case 10: /* UnmapWindow */
		if ((w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			int x = w->x, y = w->y, wi = w->width, he = w->height;
			w->mapped = 0;
			mark_dirty(s, x, y, wi, he);
			return 0;
		}
		break;
	case 12: /* ConfigureWindow */
		if (n >= 12 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			uint16_t mask = rd16(q + 8, c->order);
			size_t off = 12;
			unsigned bit;
			int oldx = w->x, oldy = w->y, oldw = w->width,
			    oldh = w->height, newx = w->x, newy = w->y,
			    raise = 0;
			uint16_t neww = w->width, newh = w->height,
				 newborder = w->border;
			for (bit = 0; bit < 7 && off + 4 <= n; bit++)
				if (mask & (1U << bit)) {
					uint32_t v = rd32(q + off, c->order);
					if (bit == 0)
						newx = (int16_t)v;
					else if (bit == 1)
						newy = (int16_t)v;
					else if (bit == 2)
						neww = (uint16_t)v;
					else if (bit == 3)
						newh = (uint16_t)v;
					else if (bit == 4)
						newborder = (uint16_t)v;
					else if (bit == 6 && v == Above)
						raise = 1;
					off += 4;
				}
			if ((neww != w->width || newh != w->height) &&
			    window_pixels_resize(w, neww, newh)) {
				error_reply(c, 11, w->id, op);
				return 0;
			}
			w->x = (int16_t)newx;
			w->y = (int16_t)newy;
			w->width = neww;
			w->height = newh;
			w->border = newborder;
			if (w->x != oldx || w->y != oldy) {
				unsigned j;
				for (j = 1; j < s->window_count; j++)
					if (s->windows[j].parent == w->id) {
						s->windows[j].x +=
						    (int16_t)(w->x - oldx);
						s->windows[j].y +=
						    (int16_t)(w->y - oldy);
					}
			}
			mark_dirty(s, oldx, oldy, oldw, oldh);
			mark_dirty(s, w->x, w->y, w->width, w->height);
			if (neww != (uint16_t)oldw || newh != (uint16_t)oldh)
				expose(s, w);
			if (raise)
				raise_window(s, top_level_window(s, w));
			return 0;
		}
		break;
	case 14: /* GetGeometry */
		if ((w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			uint8_t r[32];
			memset(r, 0, 32);
			r[1] = 24;
			wr32(r + 8, ROOT_XID, c->order);
			wr16(r + 12, (uint16_t)w->x, c->order);
			wr16(r + 14, (uint16_t)w->y, c->order);
			wr16(r + 16, w->width, c->order);
			wr16(r + 18, w->height, c->order);
			wr16(r + 20, w->border, c->order);
			simple_reply(c, r, 32);
			return 0;
		}
		break;
	case 15: /* QueryTree: return all direct children in stacking order. */
		if ((w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			uint8_t r[32 + MAX_WINDOWS * 4];
			unsigned i, count = 0;
			memset(r, 0, sizeof(r));
			wr32(r + 8, ROOT_XID, c->order);
			wr32(r + 12, w->parent, c->order);
			for (i = 1; i < s->window_count; i++)
				if (s->windows[i].id &&
				    s->windows[i].parent == w->id) {
					wr32(r + 32 + count * 4,
					     s->windows[i].id, c->order);
					count++;
				}
			wr16(r + 16, (uint16_t)count, c->order);
			simple_reply(c, r, 32 + count * 4);
			return 0;
		}
		break;
	case 18: /* ChangeProperty: retain desktop string properties. */
		if (n >= 24 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			uint32_t property = rd32(q + 8, c->order),
				 type = rd32(q + 12, c->order),
				 count = rd32(q + 20, c->order);
			char *target = NULL;
			size_t capacity = 0;
			if (property == 39) {
				target = w->name;
				capacity = sizeof(w->name);
			} else if (property == XZED_ICON_PATH_ATOM) {
				target = w->icon_path;
				capacity = sizeof(w->icon_path);
			}
			if (target && type == 31 && q[16] == 8 &&
			    count <= n - 24) {
				size_t copy =
				    count < capacity - 1 ? count : capacity - 1;
				memcpy(target, q + 24, copy);
				target[copy] = 0;
			}
			return 0;
		}
		break;
	case 20: /* GetProperty: WM_NAME and Xzed desktop strings. */
		if (n >= 24 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			uint8_t r[32 + 160];
			uint32_t property = rd32(q + 8, c->order);
			const char *value = NULL;
			size_t count = 0, padded = 0;
			memset(r, 0, sizeof(r));
			if (property == 39)
				value = w->name;
			else if (property == XZED_ICON_PATH_ATOM)
				value = w->icon_path;
			if (value && value[0] &&
			    (rd32(q + 12, c->order) == 0 ||
			     rd32(q + 12, c->order) == 31)) {
				count = strlen(value);
				padded = (count + 3U) & ~3U;
				r[1] = 8;
				wr32(r + 8, 31, c->order);
				wr32(r + 16, (uint32_t)count, c->order);
				memcpy(r + 32, value, count);
			}
			simple_reply(c, r, 32 + padded);
			return 0;
		}
		break;
	case 38: /* QueryPointer */
	{
		uint8_t r[32];
		struct window *h = hit(s, s->pointer_x, s->pointer_y);
		memset(r, 0, 32);
		r[1] = 1;
		wr32(r + 8, ROOT_XID, c->order);
		wr32(r + 12, h->id == ROOT_XID ? 0 : h->id, c->order);
		wr16(r + 16, (uint16_t)s->pointer_x, c->order);
		wr16(r + 18, (uint16_t)s->pointer_y, c->order);
		wr16(r + 20, (uint16_t)(s->pointer_x - h->x), c->order);
		wr16(r + 22, (uint16_t)(s->pointer_y - h->y), c->order);
		wr16(r + 24, (uint16_t)s->key_state, c->order);
		simple_reply(c, r, 32);
		return 0;
	}
	case 42:
		s->focus = rd32(q + 4, c->order);
		return 0;
	case 43: {
		uint8_t r[32];
		memset(r, 0, 32);
		r[1] = 0;
		wr32(r + 8, s->focus, c->order);
		simple_reply(c, r, 32);
		return 0;
	}
	case 45: /* OpenFont */
		if (n >= 12 && s->font_count < MAX_FONTS) {
			uint16_t ln = rd16(q + 8, c->order);
			if (12U + ln <= n) {
				s->fonts[s->font_count++] =
				    (struct font_resource){
					rd32(q + 4, c->order), (int)ci};
				return 0;
			}
		}
		break;
	case 46: /* CloseFont */
	{
		id = rd32(q + 4, c->order);
		unsigned i;
		for (i = 0; i < s->font_count; i++)
			if (s->fonts[i].id == id) {
				if (i + 1U < s->font_count)
					memmove(&s->fonts[i], &s->fonts[i + 1],
						(s->font_count - i - 1U) *
						    sizeof(s->fonts[0]));
				s->font_count--;
				memset(&s->fonts[s->font_count], 0,
				       sizeof(s->fonts[0]));
				return 0;
			}
	} break;
	case 47: /* QueryFont: the font is Unicode BMP, with 1- or 2-cell
		    glyphs. */
	{
		uint8_t r[60];
		memset(r, 0, sizeof(r));
		wr16(r + 8, 0, c->order);
		wr16(r + 10, 255, c->order);
		wr16(r + 12, 0, c->order);
		wr16(r + 14, 255, c->order);
		r[16] = 0;
		r[17] = 0;
		wr16(r + 18, 0, c->order);
		wr16(r + 20, 16, c->order);
		wr32(r + 56, 0, c->order);
		simple_reply(c, r, sizeof(r));
		return 0;
	}
	case 49: /* ListFonts */
	{
		static const char name[] = "zed-unicode";
		uint8_t r[44];
		memset(r, 0, sizeof(r));
		wr16(r + 8, 1, c->order);
		r[32] = (uint8_t)(sizeof(name) - 1);
		memcpy(r + 33, name, sizeof(name) - 1);
		simple_reply(c, r, sizeof(r));
		return 0;
	}
	case 53: /* CreatePixmap */
		if (n >= 16 && s->pixmap_count < MAX_PIXMAPS) {
			struct pixmap *p = &s->pixmaps[s->pixmap_count];
			size_t count;
			memset(p, 0, sizeof(*p));
			p->id = rd32(q + 4, c->order);
			p->owner = (int)ci;
			p->width = rd16(q + 12, c->order);
			p->height = rd16(q + 14, c->order);
			count = (size_t)p->width * p->height;
			if (!p->width || !p->height ||
			    (p->width && count / p->width != p->height) ||
			    (p->pixels = calloc(count, sizeof(*p->pixels))) ==
				NULL) {
				error_reply(c, 11, p->id, op);
				return 0;
			}
			s->pixmap_count++;
			return 0;
		}
		break;
	case 54: /* FreePixmap */
	{
		struct pixmap *p = find_pixmap(s, rd32(q + 4, c->order));
		if (p) {
			size_t pi = (size_t)(p - s->pixmaps);
			free(p->pixels);
			if (pi + 1U < s->pixmap_count)
				memmove(&s->pixmaps[pi], &s->pixmaps[pi + 1],
					(s->pixmap_count - pi - 1U) *
					    sizeof(s->pixmaps[0]));
			s->pixmap_count--;
			memset(&s->pixmaps[s->pixmap_count], 0,
			       sizeof(s->pixmaps[0]));
			return 0;
		}
	} break;
	case 55: /* CreateGC */
		if (n >= 16 && s->gc_count < MAX_GCS) {
			struct graphics_context *g = &s->gcs[s->gc_count++];
			uint32_t mask = rd32(q + 12, c->order);
			size_t off = 16;
			unsigned bit;
			memset(g, 0, sizeof(*g));
			g->id = rd32(q + 4, c->order);
			g->owner = (int)ci;
			g->foreground = 0xffffff;
			for (bit = 0; bit < 32 && off + 4 <= n; bit++)
				if (mask & (1U << bit)) {
					uint32_t v = rd32(q + off, c->order);
					if (bit == 2)
						g->foreground = v;
					if (bit == 14)
						g->font = v;
					off += 4;
				}
			return 0;
		}
		break;
	case 56: /* ChangeGC */
	{
		struct graphics_context *g = find_gc(s, rd32(q + 4, c->order));
		if (g && n >= 12) {
			uint32_t mask = rd32(q + 8, c->order);
			size_t off = 12;
			unsigned bit;
			for (bit = 0; bit < 32 && off + 4 <= n; bit++)
				if (mask & (1U << bit)) {
					uint32_t v = rd32(q + off, c->order);
					if (bit == 2)
						g->foreground = v;
					if (bit == 14)
						g->font = v;
					off += 4;
				}
			return 0;
		}
	} break;
	case 60: /* FreeGC */
	{
		id = rd32(q + 4, c->order);
		unsigned i;
		for (i = 0; i < s->gc_count; i++)
			if (s->gcs[i].id == id) {
				if (i + 1U < s->gc_count)
					memmove(&s->gcs[i], &s->gcs[i + 1],
						(s->gc_count - i - 1U) *
						    sizeof(s->gcs[0]));
				s->gc_count--;
				memset(&s->gcs[s->gc_count], 0,
				       sizeof(s->gcs[0]));
				return 0;
			}
	} break;
	case 62: /* CopyArea: the minimal server currently supports Pixmap to
		    Window. */
		if (n >= 28) {
			struct pixmap *p =
			    find_pixmap(s, rd32(q + 4, c->order));
			struct window *dest =
			    find_window(s, rd32(q + 8, c->order));
			int sx = (int16_t)rd16(q + 16, c->order),
			    sy = (int16_t)rd16(q + 18, c->order),
			    dx = (int16_t)rd16(q + 20, c->order),
			    dy = (int16_t)rd16(q + 22, c->order);
			int wi = rd16(q + 24, c->order),
			    he = rd16(q + 26, c->order), row, column;
			if (!p || !dest)
				break;
			for (row = 0; row < he; row++)
				for (column = 0; column < wi; column++) {
					int px = sx + column, py = sy + row,
					    wx = dx + column, wy = dy + row;
					if (px >= 0 && py >= 0 &&
					    px < p->width && py < p->height &&
					    wx >= 0 && wy >= 0 &&
					    wx < dest->width &&
					    wy < dest->height)
						dest->pixels[(size_t)wy *
								 dest->width +
							     wx] =
						    p->pixels[(size_t)py *
								  p->width +
							      px];
				}
			mark_dirty(s, dest->x + dx, dest->y + dy, wi, he);
			present(s);
			return 0;
		}
		break;
	case 65: /* PolyLine */
		if (n >= 16 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			struct graphics_context *g =
			    find_gc(s, rd32(q + 8, c->order));
			size_t off;
			int x = 0, y = 0;
			uint32_t color = g ? g->foreground : 0xffffff;
			for (off = 12; off + 4 <= n; off += 4) {
				int nx = (int16_t)rd16(q + off, c->order),
				    ny = (int16_t)rd16(q + off + 2, c->order);
				if (off != 12) {
					int x0 = x, y0 = y, dx = abs(nx - x0),
					    sx = x0 < nx ? 1 : -1,
					    dy = -abs(ny - y0),
					    sy = y0 < ny ? 1 : -1,
					    err = dx + dy;
					for (;;) {
						if (x0 >= 0 && y0 >= 0 &&
						    x0 < w->width &&
						    y0 < w->height)
							w->pixels[(size_t)y0 *
								      w->width +
								  x0] = color;
						if (x0 == nx && y0 == ny)
							break;
						{
							int e2 = 2 * err;
							if (e2 >= dy) {
								err += dy;
								x0 += sx;
							}
							if (e2 <= dx) {
								err += dx;
								y0 += sy;
							}
						}
					}
					mark_dirty(s, w->x + (x < nx ? x : nx),
						   w->y + (y < ny ? y : ny),
						   abs(nx - x) + 1,
						   abs(ny - y) + 1);
				}
				x = nx;
				y = ny;
			}
		}
		return 0;
	case 70: /* PolyFillRectangle */
		if (n >= 12) {
			struct pixmap *p;
			struct graphics_context *g =
			    find_gc(s, rd32(q + 8, c->order));
			size_t off;
			uint32_t color = g ? g->foreground : 0xffffff;
			w = find_window(s, rd32(q + 4, c->order));
			p = find_pixmap(s, rd32(q + 4, c->order));
			if (!w && !p)
				break;
			for (off = 12; off + 8 <= n; off += 8) {
				int16_t x = (int16_t)rd16(q + off, c->order),
					y = (int16_t)rd16(q + off + 2,
							  c->order);
				uint16_t wi = rd16(q + off + 4, c->order),
					 he = rd16(q + off + 6, c->order);
				if (w)
					window_fill(s, w, x, y, wi, he, color);
				else
					pixmap_fill(p, x, y, wi, he, color);
				if (w == &s->windows[0] && x == 0 && y == 0 &&
				    wi >= w->width && he >= w->height)
					s->windows[0].background = color;
			}
			return 0;
		}
		break;
	case 76:
	case 77: /* ImageText8 / ImageText16 */
		if (n >= 16 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			struct graphics_context *g =
			    find_gc(s, rd32(q + 8, c->order));
			size_t chars = q[1];
			if (16 + chars * (op == 77 ? 2U : 1U) <= n)
				(void)draw_text(s, w, g,
						(int16_t)rd16(q + 12, c->order),
						(int16_t)rd16(q + 14, c->order),
						q + 16, chars, op == 77);
			return 0;
		}
		break;
	case 101: /* GetKeyboardMapping */ {
		uint8_t r[32 + 4 * 248];
		unsigned i;
		memset(r, 0, sizeof(r));
		r[1] = 1;
		for (i = 0; i < q[5] && i < 248; i++) {
			uint32_t kc = (uint32_t)(q[4] + i),
				 ks = kc >= 8 ? kc - 8 : 0;
			switch (kc) {
			case XZED_KEYCODE_UP:
				ks = 0xff52;
				break;
			case XZED_KEYCODE_DOWN:
				ks = 0xff54;
				break;
			case XZED_KEYCODE_LEFT:
				ks = 0xff51;
				break;
			case XZED_KEYCODE_RIGHT:
				ks = 0xff53;
				break;
			case XZED_KEYCODE_HOME:
				ks = 0xff50;
				break;
			case XZED_KEYCODE_END:
				ks = 0xff57;
				break;
			case XZED_KEYCODE_PAGE_UP:
				ks = 0xff55;
				break;
			case XZED_KEYCODE_PAGE_DOWN:
				ks = 0xff56;
				break;
			case XZED_KEYCODE_INSERT:
				ks = 0xff63;
				break;
			case XZED_KEYCODE_DELETE:
				ks = 0xffff;
				break;
			}
			wr32(r + 32 + i * 4, ks, c->order);
		}
		simple_reply(c, r, 32 + (size_t)q[5] * 4);
		return 0;
	}
	case 117: {
		uint8_t r[32 + 4];
		memset(r, 0, sizeof(r));
		r[1] = 3;
		r[32] = 1;
		r[33] = 2;
		r[34] = 3;
		simple_reply(c, r, 36);
		return 0;
	}
	case 128: /* XzedPutImageRGB24: compact private RGB24 image upload. */
		if (n >= 16) {
			uint32_t drawable = rd32(q + 4, c->order);
			struct pixmap *p = find_pixmap(s, drawable);
			int x = (int16_t)rd16(q + 8, c->order),
			    y = (int16_t)rd16(q + 10, c->order);
			unsigned wi = rd16(q + 12, c->order),
				 he = rd16(q + 14, c->order);
			size_t count = (size_t)wi * he;
			unsigned row, column;
			w = find_window(s, drawable);
			if ((!w && !p) || !wi || !he ||
			    (wi && count / wi != he) || count > (n - 16U) / 3U)
				break;
			for (row = 0; row < he; row++)
				for (column = 0; column < wi; column++) {
					const uint8_t *rgb =
					    q + 16U +
					    ((size_t)row * wi + column) * 3U;
					int dx = x + (int)column,
					    dy = y + (int)row;
					uint32_t color =
					    ((uint32_t)rgb[0] << 16) |
					    ((uint32_t)rgb[1] << 8) | rgb[2];
					if (w) {
						if (dx >= 0 && dy >= 0 &&
						    dx < w->width &&
						    dy < w->height)
							w->pixels[(size_t)dy *
								      w->width +
								  (unsigned)
								      dx] =
							    color;
					} else if (dx >= 0 && dy >= 0 &&
						   dx < p->width &&
						   dy < p->height)
						p->pixels[(size_t)dy *
							      p->width +
							  (unsigned)dx] = color;
				}
			if (w)
				mark_dirty(s, w->x + x, w->y + y, (int)wi,
					   (int)he);
			return 0;
		}
		break;
	case 129: /* XzedSetCursorShape */
		if (n >= 12 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			uint16_t shape = (uint16_t)rd32(q + 8, c->order);
			if (shape != XC_LEFT_PTR &&
			    shape != XC_BOTTOM_LEFT_CORNER &&
			    shape != XC_BOTTOM_RIGHT_CORNER &&
			    shape != XC_SB_H_DOUBLE_ARROW &&
			    shape != XC_SB_V_DOUBLE_ARROW)
				break;
			w->cursor_shape = shape;
			mark_dirty(s, s->pointer_x - 16, s->pointer_y - 16, 32,
				   32);
			return 0;
		}
		break;
	case 130: /* XzedSetInputMargins */
		if (n >= 16 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			w->input_left = rd16(q + 8, c->order);
			w->input_top = rd16(q + 10, c->order);
			w->input_right = rd16(q + 12, c->order);
			w->input_bottom = rd16(q + 14, c->order);
			return 0;
		}
		break;
	case 131: /* XzedMoveResizeWindowBuffered: no child move or Expose. */
		if (n >= 24 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			int32_t newx = (int32_t)rd32(q + 8, c->order),
				newy = (int32_t)rd32(q + 12, c->order);
			uint32_t width = rd32(q + 16, c->order),
				 height = rd32(q + 20, c->order);
			int oldx = w->x, oldy = w->y, oldw = w->width,
			    oldh = w->height;
			if (newx < INT16_MIN || newx > INT16_MAX ||
			    newy < INT16_MIN || newy > INT16_MAX || !width ||
			    width > UINT16_MAX || !height ||
			    height > UINT16_MAX)
				break;
			if (window_pixels_resize_buffered(
				w, (uint16_t)width, (uint16_t)height,
				oldx - (int)newx, oldy - (int)newy)) {
				error_reply(c, 11, w->id, op);
				return 0;
			}
			w->x = (int16_t)newx;
			w->y = (int16_t)newy;
			w->width = (uint16_t)width;
			w->height = (uint16_t)height;
			mark_dirty(s, oldx, oldy, oldw, oldh);
			mark_dirty(s, w->x, w->y, w->width, w->height);
			return 0;
		}
		break;
	case 127:
		return 0;
	default:
		error_reply(c, 1, 0, op);
		return 0;
	}
	error_reply(c, 3, n >= 8 ? rd32(q + 4, c->order) : 0, op);
	return 0;
}

/* Client connection lifecycle and incremental protocol decoding. */

static void
close_client(struct server *s, unsigned i)
{
	unsigned j;
	if (s->clients[i].fd >= 0)
		close(s->clients[i].fd);
	s->clients[i].fd = -1;
	for (;;) {
		uint32_t id = 0;
		for (j = 1; j < s->window_count; j++)
			if (s->windows[j].owner == (uint32_t)i) {
				id = s->windows[j].id;
				break;
			}
		if (!id)
			break;
		destroy_window(s, id);
	}
	for (j = 0; j < s->pixmap_count;) {
		if (s->pixmaps[j].owner == (int)i) {
			free(s->pixmaps[j].pixels);
			if (j + 1U < s->pixmap_count)
				memmove(&s->pixmaps[j], &s->pixmaps[j + 1],
					(s->pixmap_count - j - 1U) *
					    sizeof(s->pixmaps[0]));
			s->pixmap_count--;
			memset(&s->pixmaps[s->pixmap_count], 0,
			       sizeof(s->pixmaps[0]));
		} else
			j++;
	}
	for (j = 0; j < s->gc_count;) {
		if (s->gcs[j].owner == (int)i) {
			if (j + 1U < s->gc_count)
				memmove(&s->gcs[j], &s->gcs[j + 1],
					(s->gc_count - j - 1U) *
					    sizeof(s->gcs[0]));
			s->gc_count--;
			memset(&s->gcs[s->gc_count], 0, sizeof(s->gcs[0]));
		} else
			j++;
	}
	for (j = 0; j < s->font_count;) {
		if (s->fonts[j].owner == (int)i) {
			if (j + 1U < s->font_count)
				memmove(&s->fonts[j], &s->fonts[j + 1],
					(s->font_count - j - 1U) *
					    sizeof(s->fonts[0]));
			s->font_count--;
			memset(&s->fonts[s->font_count], 0,
			       sizeof(s->fonts[0]));
		} else
			j++;
	}
	free(s->clients[i].input);
	memset(&s->clients[i], 0, sizeof(s->clients[i]));
	s->clients[i].fd = -1;
}

static void
read_client(struct server *s, unsigned i)
{
	struct client *c = &s->clients[i];
	uint8_t temp[4096];
	ssize_t nr;
	while ((nr = recv(c->fd, temp, sizeof(temp), MSG_DONTWAIT)) > 0) {
		if (c->used + (size_t)nr > INPUT_CAP) {
			close_client(s, i);
			return;
		}
		if (c->used + (size_t)nr > c->capacity) {
			size_t z = c->capacity ? c->capacity * 2 : 4096;
			while (z < c->used + (size_t)nr)
				z *= 2;
			c->input = realloc(c->input, z);
			if (!c->input) {
				close_client(s, i);
				return;
			}
			c->capacity = z;
		}
		memcpy(c->input + c->used, temp, (size_t)nr);
		c->used += (size_t)nr;
	}
	if (nr == 0) {
		close_client(s, i);
		return;
	}
	if (nr < 0 && errno != EAGAIN && errno != EINTR) {
		close_client(s, i);
		return;
	}
	/* One read may contain a partial request or several complete requests.
	 */
	for (;;) {
		size_t need;
		if (!c->setup) {
			uint16_t authn, authd;
			if (c->used < 12)
				return;
			if (c->input[0] != 'l' && c->input[0] != 'B') {
				close_client(s, i);
				return;
			}
			c->order = c->input[0] == 'B';
			authn = rd16(c->input + 6, c->order);
			authd = rd16(c->input + 8, c->order);
			need = 12 + ((authn + 3) & ~3U) + ((authd + 3) & ~3U);
			if (c->used < need)
				return;
			if (setup_reply(s, c)) {
				close_client(s, i);
				return;
			}
			c->setup = 1;
		} else {
			uint16_t units;
			if (c->used < 4)
				return;
			units = rd16(c->input + 2, c->order);
			if (!units) {
				close_client(s, i);
				return;
			}
			need = (size_t)units * 4;
			if (c->used < need)
				return;
			(void)request(s, i, c->input, need);
		}
		memmove(c->input, c->input + need, c->used - need);
		c->used -= need;
	}
}

/* Capability-discovered evdev records are normalized by input.c. */

static void
input_key(void *context, uint8_t keycode, int value, uint32_t time,
	uint16_t state)
{
	struct server *s = context;
	struct window *w = find_window(s, s->focus);
	struct client *c;
	if (!w)
		w = hit(s, s->pointer_x, s->pointer_y);
	c = owner_client(s, w->owner);
	s->key_state = state;
	if (!c || keycode == 0)
		return;
	if (value == 1)
		send_event(c, 2, w->id, keycode, time, s->pointer_x,
		    s->pointer_y, state);
	else if (value == 0)
		send_event(c, 3, w->id, keycode, time, s->pointer_x,
		    s->pointer_y, state);
	else if (value == 2) {
		send_event(c, 3, w->id, keycode, time, s->pointer_x,
		    s->pointer_y, state);
		send_event(c, 2, w->id, keycode, time, s->pointer_x,
		    s->pointer_y, state);
	}
}

static void
flush_pointer_motion(struct server *s)
{
	if (!s->pending_motion)
		return;
	send_motion_event(s->pending_motion_client, s->pending_motion_window,
	    (uint64_t)s->pending_motion_time * 1000000U, s->pending_motion_x,
	    s->pending_motion_y, s->pending_motion_buttons);
	s->pending_motion = 0;
}

static void
input_pointer(void *context, const struct xzed_input_pointer_frame *frame)
{
	struct server *s = context;
	struct window *w;
	struct client *c;
	int moved = frame->absolute || frame->relative_x != 0 ||
	    frame->relative_y != 0;
	size_t index;
	s->buttons = frame->buttons_before;
	if (frame->edge_count != 0)
		flush_pointer_motion(s);
	if (moved && !s->pointer_dirty) {
		s->pointer_dirty = 1;
		s->pointer_old_x = s->pointer_x;
		s->pointer_old_y = s->pointer_y;
	}
	if (frame->absolute) {
		s->pointer_x = frame->absolute_x;
		s->pointer_y = frame->absolute_y;
	}
	if (frame->relative_x != 0 || frame->relative_y != 0)
		xzed_pointer_move(&s->pointer_x, &s->pointer_y,
		    frame->relative_x, frame->relative_y, s->mode.width,
		    s->mode.height);
	w = s->pointer_grab_owner >= 0
		? find_window(s, s->pointer_grab_window)
		: hit(s, s->pointer_x, s->pointer_y);
	if (!w)
		w = hit(s, s->pointer_x, s->pointer_y);
	c = s->pointer_grab_owner >= 0
		? owner_client(s, (uint32_t)s->pointer_grab_owner)
		: owner_client(s, w->owner);
	if (moved && frame->edge_count != 0)
		send_motion_event(c, w, (uint64_t)frame->time * 1000000U,
		    s->pointer_x, s->pointer_y, s->buttons);
	else if (moved) {
		s->pending_motion_client = c;
		s->pending_motion_window = w;
		s->pending_motion_time = frame->time;
		s->pending_motion_x = s->pointer_x;
		s->pending_motion_y = s->pointer_y;
		s->pending_motion_buttons = s->buttons;
		s->pending_motion = 1;
	}
	for (index = 0; index < frame->edge_count; index++) {
		const struct xzed_input_button_edge *edge = &frame->edges[index];
		uint32_t window_id = w->id;
		if (c)
			send_event(c, edge->pressed ? 4 : 5, window_id,
			    edge->button, frame->time, s->pointer_x, s->pointer_y,
			    s->buttons);
		s->buttons = edge->buttons;
		if (edge->pressed) {
			struct window *top = top_level_window(s, w);
			/* The desktop panel remains clickable without taking the
			 * application's focus used by toggle-minimize semantics. */
			if (top == NULL || strcmp(top->name, "_XZED_SHELL") != 0) {
				s->focus = window_id;
				raise_window(s, top);
			}
			w = find_window(s, window_id);
			if (s->pointer_grab_owner < 0 && c) {
				s->pointer_grab_owner = w->owner;
				s->pointer_grab_window = w->id;
			}
		}
	}
	if (!s->buttons) {
		s->pointer_grab_owner = -1;
		s->pointer_grab_window = 0;
	}
}

static void
finish_pointer_input(struct server *s)
{
	flush_pointer_motion(s);
	if (!s->pointer_dirty)
		return;
	mark_dirty(s, s->pointer_old_x - CURSOR_WIDTH,
	    s->pointer_old_y - CURSOR_HEIGHT, CURSOR_WIDTH * 2,
	    CURSOR_HEIGHT * 2);
	present(s);
	mark_dirty(s, s->pointer_x - CURSOR_WIDTH,
	    s->pointer_y - CURSOR_HEIGHT, CURSOR_WIDTH * 2,
	    CURSOR_HEIGHT * 2);
	present(s);
	s->pointer_dirty = 0;
}

/* Graphics mode selection and server lifetime. */

static int
parse_size(const char *text, unsigned *width, unsigned *height)
{
	char *end;
	unsigned long w, h;

	w = strtoul(text, &end, 10);
	if (end == text || (*end != 'x' && *end != 'X') || w == 0 || w > 16384U)
		return 0;
	h = strtoul(end + 1, &end, 10);
	if (*end != '\0' || h == 0 || h > 16384U)
		return 0;
	*width = (unsigned)w;
	*height = (unsigned)h;
	return 1;
}

static int
choose_mode(int fd, unsigned preferred_width, unsigned preferred_height,
	    unsigned preferred_depth, struct graphics_mode *chosen)
{
	struct graphics_mode_info modes[16];
	struct graphics_mode_list list;
	int best = -1;
	unsigned pass, i;

	memset(&list, 0, sizeof(list));
	list.modes = (uapi_ptr_t)(uintptr_t)modes;
	list.capacity = sizeof(modes) / sizeof(modes[0]);
	if (ioctl(fd, ZEDBSD_GRAPHICS_GET_MODES, &list) != 0 || list.count == 0)
		return -1;
	if (list.count > list.capacity)
		list.count = list.capacity;
	/* Exact depth and a mode no larger than the request win.  If either is
	 * unavailable, relax depth first and size second. */
	for (pass = 0; pass < 4 && best < 0; pass++) {
		uint64_t best_area = 0;
		for (i = 0; i < list.count; i++) {
			uint64_t area =
			    (uint64_t)modes[i].width * modes[i].height;
			int depth_ok =
			    preferred_depth == 0 ||
			    modes[i].bits_per_pixel == preferred_depth;
			int size_ok = preferred_width == 0 ||
				      (modes[i].width <= preferred_width &&
				       modes[i].height <= preferred_height);
			if ((pass < 2 && !size_ok) ||
			    ((pass == 0 || pass == 2) && !depth_ok))
				continue;
			if (best < 0 ||
			    (size_ok ? area > best_area : area < best_area)) {
				best = (int)i;
				best_area = area;
			}
		}
	}
	if (best < 0) {
		errno = ENODEV;
		return -1;
	}
	memset(chosen, 0, sizeof(*chosen));
	chosen->preferred_width = modes[best].width;
	chosen->preferred_height = modes[best].height;
	chosen->preferred_bits_per_pixel = modes[best].bits_per_pixel;
	return 0;
}

static int
initialize(struct server *s, unsigned preferred_width,
	   unsigned preferred_height, unsigned preferred_depth)
{
	struct sockaddr_un a;
	struct graphics_caps caps;
	const struct xzed_input_handlers input_handlers = {input_key,
							   input_pointer};
	unsigned i;
	memset(s, 0, sizeof(*s));
	s->listener = s->graphics = -1;
	s->pointer_grab_owner = -1;
	for (i = 0; i < MAX_CLIENTS; i++)
		s->clients[i].fd = -1;
	s->graphics = open("/dev/graphics", O_RDWR | O_CLOEXEC);
	if (s->graphics < 0)
		return -1;
	if (ioctl(s->graphics, ZEDBSD_GRAPHICS_GET_CAPS, &caps))
		return -1;
	if (!(caps.capabilities & ZEDBSD_GRAPHICS_CAP_FLUSH) ||
	    !(caps.capabilities & ZEDBSD_GRAPHICS_CAP_GLYPH) ||
	    !(caps.capabilities & ZEDBSD_GRAPHICS_CAP_BLIT_RGB24)) {
		errno = ENOTSUP;
		return -1;
	}
	if (choose_mode(s->graphics, preferred_width, preferred_height,
			preferred_depth, &s->mode))
		return -1;
	if (ioctl(s->graphics, ZEDBSD_GRAPHICS_ENTER, &s->mode))
		return -1;
	(void)mkdir("/tmp/.X11-unix", 0777);
	(void)unlink("/tmp/.X11-unix/X0");
	s->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s->listener < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, "/tmp/.X11-unix/X0");
	if (bind(s->listener, (struct sockaddr *)&a, sizeof(a)) ||
	    listen(s->listener, 8))
		return -1;
	(void)fcntl(s->listener, F_SETFL,
		    fcntl(s->listener, F_GETFL) | O_NONBLOCK);
	s->windows[0] = (struct window){.id = ROOT_XID,
					.owner = MAX_CLIENTS,
					.background = 0x203040,
					.width = (uint16_t)s->mode.width,
					.height = (uint16_t)s->mode.height,
					.mapped = 1};
	s->window_count = 1;
	s->focus = ROOT_XID;
	s->pointer_x = (int)s->mode.width / 2;
	s->pointer_y = (int)s->mode.height / 2;
	if (xzed_input_open(&s->input, s->mode.width, s->mode.height,
	    &input_handlers, s) != 0)
		return -1;
	s->buttons = xzed_input_buttons(s->input);
	s->key_state = xzed_input_modifiers(s->input);
	s->windows[0].pixels =
	    window_pixels_alloc(s->windows[0].width, s->windows[0].height,
				s->windows[0].background);
	s->screen =
	    malloc((size_t)s->mode.width * s->mode.height * sizeof(*s->screen));
	s->transfer = malloc((size_t)s->mode.width * s->mode.height * 3U);
	if (!s->windows[0].pixels || !s->screen || !s->transfer) {
		errno = ENOMEM;
		return -1;
	}
	repaint(s);
	present(s);
	return 0;
}

static void
cleanup(struct server *s)
{
	unsigned i;
	for (i = 0; i < MAX_CLIENTS; i++)
		close_client(s, i);
	for (i = 0; i < s->window_count; i++)
		free(s->windows[i].pixels);
	for (i = 0; i < s->pixmap_count; i++)
		free(s->pixmaps[i].pixels);
	free(s->screen);
	free(s->transfer);
	if (s->listener >= 0)
		close(s->listener);
	(void)unlink("/tmp/.X11-unix/X0");
	xzed_input_close(s->input);
	if (s->graphics >= 0)
		close(s->graphics);
}

int
main(int argc, char **argv)
{
	struct server s;
	struct pollfd p[1 + XZED_INPUT_MAX_DEVICES + MAX_CLIENTS];
	unsigned i, count, input_base, input_count;
	int arg = 1;
	unsigned preferred_width = 0, preferred_height = 0,
		 preferred_depth = 24;
	extern char **environ;
	while (arg < argc) {
		if (strcmp(argv[arg], ":0") == 0) {
			arg++;
			continue;
		}
		if (strcmp(argv[arg], "--") == 0) {
			arg++;
			break;
		}
		if (strcmp(argv[arg], "--size") == 0 && arg + 1 < argc &&
		    parse_size(argv[arg + 1], &preferred_width,
			       &preferred_height)) {
			arg += 2;
			continue;
		}
		if (strcmp(argv[arg], "--depth") == 0 && arg + 1 < argc) {
			char *end;
			unsigned long d = strtoul(argv[arg + 1], &end, 10);
			if (*end == '\0' &&
			    (d == 4 || d == 8 || d == 24 || d == 32)) {
				preferred_depth = (unsigned)d;
				arg += 2;
				continue;
			}
		}
		fprintf(stderr, "usage: Xzed [:0] [--size WIDTHxHEIGHT] "
				"[--depth 4|8|24|32] "
				"[-- command [argument ...]]\n");
		return 2;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (initialize(&s, preferred_width, preferred_height,
		       preferred_depth)) {
		fprintf(stderr, "Xzed: %s\n", strerror(errno));
		cleanup(&s);
		return 1;
	}
	if (arg < argc) {
		pid_t pid = fork();
		if (pid < 0) {
			fprintf(stderr, "Xzed: fork: %s\n", strerror(errno));
			cleanup(&s);
			return 1;
		}
		if (pid == 0) {
			execve(argv[arg], &argv[arg], environ);
			fprintf(stderr, "Xzed: %s: %s\n", argv[arg],
				strerror(errno));
			_exit(127);
		}
	}
	while (!stopped) {
		int ready;
		count = 0;
		p[count++] = (struct pollfd){s.listener, POLLIN, 0};
		input_base = count;
		input_count = (unsigned)xzed_input_pollfds(s.input, p + count,
		    XZED_INPUT_MAX_DEVICES);
		count += input_count;
		for (i = 0; i < MAX_CLIENTS; i++)
			if (s.clients[i].fd >= 0)
				p[count++] =
				    (struct pollfd){s.clients[i].fd, POLLIN, 0};
		ready = poll(p, count, 20);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (p[0].revents & POLLIN) {
			int fd = accept(s.listener, NULL, NULL);
			if (fd >= 0) {
				int descriptor_flags = fcntl(fd, F_GETFD);
				int status_flags = fcntl(fd, F_GETFL);
				if (descriptor_flags < 0 || status_flags < 0 ||
				    fcntl(fd, F_SETFD,
					  descriptor_flags | FD_CLOEXEC) < 0 ||
				    fcntl(fd, F_SETFL,
					  status_flags | O_NONBLOCK) < 0) {
					close(fd);
				} else {
					for (i = 0; i < MAX_CLIENTS &&
						    s.clients[i].fd >= 0;
					     i++)
						;
					if (i == MAX_CLIENTS)
						close(fd);
					else {
						s.clients[i].fd = fd;
						s.clients[i].base = (i + 1U)
								    << 22;
					}
				}
			}
		}
		if (xzed_input_dispatch(s.input, p + input_base, input_count) != 0)
			stopped = 1;
		finish_pointer_input(&s);
		for (i = 0; i < MAX_CLIENTS; i++)
			if (s.clients[i].fd >= 0)
				read_client(&s, i);
		present(&s);
	}
	cleanup(&s);
	return 0;
}
