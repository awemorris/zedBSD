/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Xzed - small local X11 server for zedBSD
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

/*
 * Protocol resources are kept in fixed-size tables to avoid a general XID map.
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
	/*
 * Retained contents let the server reveal a window without repainting
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

extern char **environ;

static int parse_size(const char *text, unsigned *width, unsigned *height);
static int initialize(struct server *s, unsigned preferred_width, unsigned preferred_height, unsigned preferred_depth);
static int choose_mode(int fd, unsigned preferred_width, unsigned preferred_height, unsigned preferred_depth, struct graphics_mode *chosen);
static uint32_t *window_pixels_alloc(uint16_t width, uint16_t height, uint32_t color);
static void repaint(struct server *s);
static void mark_dirty(struct server *s, int x, int y, int w, int h);
static void present(struct server *s);
static uint16_t pointer_shape(struct server *s);
static struct window *find_window(struct server *s, uint32_t id);
static struct window *input_at(struct server *s, int x, int y);
static struct window *input_at_parent(struct server *s, uint32_t parent, int x, int y);
static void composite_region(struct server *s, int x, int y, int width, int height);
static struct window *top_at(struct server *s, int x, int y);
static struct window *top_at_parent(struct server *s, uint32_t parent, int x, int y);
static int cursor_pixel(uint16_t shape, int x, int y, uint32_t *color);
static int resize_cursor_black(uint16_t shape, int x, int y);
static void cleanup(struct server *s);
static void close_client(struct server *s, unsigned i);
static void destroy_window(struct server *s, uint32_t id);
static void destroy_notify(struct client *c, uint32_t event, uint32_t window);
static void wr16(uint8_t *p, uint16_t v, int msb);
static void wr32(uint8_t *p, uint32_t v, int msb);
static int write_all(int fd, const void *v, size_t n);
static struct client *owner_client(struct server *s, uint32_t owner);
static void finish_pointer_input(struct server *s);
static void flush_pointer_motion(struct server *s);
static void send_motion_event(struct client *c, struct window *w, uint64_t time, int x, int y, uint16_t buttons);
static void send_event(struct client *c, uint8_t type, uint32_t window, uint32_t detail, uint32_t time, int16_t rx, int16_t ry, uint16_t state);
static void read_client(struct server *s, unsigned i);
static uint16_t rd16(const uint8_t *p, int msb);
static int setup_reply(struct server *s, struct client *c);
static int request(struct server *s, unsigned ci, const uint8_t *q, size_t n);
static uint32_t rd32(const uint8_t *p, int msb);
static void error_reply(struct client *c, uint8_t code, uint32_t resource, uint8_t opcode);
static void map_request(struct server *s, struct window *parent, struct window *w);
static void expose(struct server *s, struct window *w);
static void expose_area(struct server *s, struct window *w, int x, int y, int width, int height);
static void configure_notify(struct server *s, struct window *w);
static int window_pixels_resize(struct window *w, uint16_t width, uint16_t height);
static void raise_window(struct server *s, struct window *w);
static struct window *top_level_window(struct server *s, struct window *w);
static void simple_reply(struct client *c, uint8_t *r, size_t n);
static struct window *hit(struct server *s, int x, int y);
static struct pixmap *find_pixmap(struct server *s, uint32_t id);
static struct graphics_context *find_gc(struct server *s, uint32_t id);
static void window_fill(struct server *s, struct window *w, int x, int y, int width, int height, uint32_t color);
static void pixmap_fill(struct pixmap *p, int x, int y, int width, int height, uint32_t color);
static int draw_text(struct server *s, struct window *w, struct graphics_context *g, int x, int y, const uint8_t *text, size_t count, int wide);
static int window_pixels_resize_buffered(struct window *w, uint16_t width, uint16_t height, int x_offset, int y_offset);
static void on_signal(int sig);
static void input_key(void *context, uint8_t keycode, int value, uint32_t time, uint16_t state);
static void input_pointer(void *context, const struct xzed_input_pointer_frame *frame);

/*
 * Runs the xzed command.
 */
int
main(
	int argc,
	char **argv)
{
	char *end;
	unsigned long d;
	pid_t pid;
	int descriptor_flags;
	int status_flags;
	int fd;
	int ready;
	struct server s;
	struct pollfd p[1 + XZED_INPUT_MAX_DEVICES + MAX_CLIENTS];
	unsigned i, count, input_base, input_count;
	int arg;
	unsigned preferred_width, preferred_height, preferred_depth;

	/* Process each remaining command-line operand. */
	arg = 1;
	preferred_width = 0;
	preferred_height = 0;
	preferred_depth = 24;
	while (arg < argc) {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[arg], ":0") == 0) {
			arg++;
			continue;
		}

		/* Handles the selected command-line operation. */
		if (strcmp(argv[arg], "--") == 0) {
			arg++;
			break;
		}

		/* Handles the selected command-line operation. */
		if (strcmp(argv[arg], "--size") == 0 && arg + 1 < argc &&
		    parse_size(argv[arg + 1], &preferred_width,
			       &preferred_height)) {
			arg += 2;
			continue;
		}

		/* Handles the selected command-line operation. */
		if (strcmp(argv[arg], "--depth") == 0 && arg + 1 < argc) {
			d = strtoul(argv[arg + 1], &end, 10);

			/* Checks the current endpoint. */
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

		/* Reports operation failure. */
		return 2;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/* Handles the initialize condition. */
	if (initialize(&s, preferred_width, preferred_height,
		       preferred_depth)) {
		fprintf(stderr, "Xzed: %s\n", strerror(errno));
		cleanup(&s);

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (arg < argc) {
		pid = fork();

		/* Handles the pid condition. */
		if (pid < 0) {
			fprintf(stderr, "Xzed: fork: %s\n", strerror(errno));
			cleanup(&s);

			/* Reports operation failure. */
			return 1;
		}

		/* Handles the pid condition. */
		if (pid == 0) {
			execve(argv[arg], &argv[arg], environ);
			fprintf(stderr, "Xzed: %s: %s\n", argv[arg],
				strerror(errno));
			_exit(127);
		}
	}
	while (!stopped) {
		count = 0;
		p[count++] = (struct pollfd){s.listener, POLLIN, 0};
		input_base = count;
		input_count = (unsigned)xzed_input_pollfds(s.input, p + count,
		    XZED_INPUT_MAX_DEVICES);
		count += input_count;

		/* Process each element required by the operation. */
		for (i = 0; i < MAX_CLIENTS; i++) {
			/* Checks the current string state. */
			if (s.clients[i].fd >= 0) {
				p[count++] =
				    (struct pollfd){s.clients[i].fd, POLLIN, 0};
			}
		}
		ready = poll(p, count, 20);

		/* Handles the ready condition. */
		if (ready < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;
			break;
		}

		/* Checks the current pointer. */
		if (p[0].revents & POLLIN) {
			fd = accept(s.listener, NULL, NULL);

			/* Checks the file descriptor. */
			if (fd >= 0) {
				descriptor_flags = fcntl(fd, F_GETFD);
				status_flags = fcntl(fd, F_GETFL);

				/* Handles a failed fcntl operation. */
				if (descriptor_flags < 0 || status_flags < 0 ||
				    fcntl(fd, F_SETFD,
					  descriptor_flags | FD_CLOEXEC) < 0 ||
				    fcntl(fd, F_SETFL,
					  status_flags | O_NONBLOCK) < 0) {
					close(fd);
				} else {
					/* Process each element required by the operation. */
					for (i = 0; i < MAX_CLIENTS &&
						    s.clients[i].fd >= 0;
					     i++)
						;

					/* Checks the current index. */
					if (i == MAX_CLIENTS) {
						close(fd);
					} else {
						s.clients[i].fd = fd;
						s.clients[i].base = (i + 1U)
								    << 22;
					}
				}
			}
		}

		/* Handles a failed xzed input dispatch operation. */
		if (xzed_input_dispatch(s.input, p + input_base, input_count) != 0)
			stopped = 1;
		finish_pointer_input(&s);

		/* Process each element required by the operation. */
		for (i = 0; i < MAX_CLIENTS; i++) {
			/* Checks the current string state. */
			if (s.clients[i].fd >= 0)
				read_client(&s, i);
		}
		present(&s);
	}
	cleanup(&s);

	/* Reports successful completion. */
	return 0;
}

/* Graphics mode selection and server lifetime. */
static int
parse_size(
	const char *text,
	unsigned *width,
	unsigned *height)
{
	char *end;
	unsigned long w, h;

	w = strtoul(text, &end, 10);

	/* Checks the current endpoint. */
	if (end == text || (*end != 'x' && *end != 'X') || w == 0 || w > 16384U)
		return 0;
	h = strtoul(end + 1, &end, 10);

	/* Checks the current endpoint. */
	if (*end != '\0' || h == 0 || h > 16384U)
		return 0;
	*width = (unsigned)w;
	*height = (unsigned)h;
	/* Reports operation failure. */
	return 1;
}

/* Supports the initialize operation. */
static int
initialize(
	struct server *s,
	unsigned preferred_width,
	unsigned preferred_height,
	unsigned preferred_depth)
{
	struct sockaddr_un a;
	struct graphics_caps caps;
	const struct xzed_input_handlers input_handlers = {input_key,
							   input_pointer};
	unsigned i;

	memset(s, 0, sizeof(*s));

	/* Process each element required by the operation. */
	s->listener = s->graphics = -1;
	s->pointer_grab_owner = -1;
	for (i = 0; i < MAX_CLIENTS; i++)
		s->clients[i].fd = -1;
	s->graphics = open("/dev/graphics", O_RDWR | O_CLOEXEC);

	/* Checks the current string state. */
	if (s->graphics < 0)
		return -1;

	/* Handles a failed ioctl operation. */
	if (ioctl(s->graphics, ZEDBSD_GRAPHICS_GET_CAPS, &caps))
		return -1;

	/* Handles the caps condition. */
	if (!(caps.capabilities & ZEDBSD_GRAPHICS_CAP_FLUSH) ||
	    !(caps.capabilities & ZEDBSD_GRAPHICS_CAP_GLYPH) ||
	    !(caps.capabilities & ZEDBSD_GRAPHICS_CAP_BLIT_RGB24)) {
		errno = ENOTSUP;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed choose mode operation. */
	if (choose_mode(s->graphics, preferred_width, preferred_height,
			preferred_depth, &s->mode))

		/* Reports operation failure. */
		return -1;

	/* Handles a failed ioctl operation. */
	if (ioctl(s->graphics, ZEDBSD_GRAPHICS_ENTER, &s->mode))
		return -1;
	(void)mkdir("/tmp/.X11-unix", 0777);
	(void)unlink("/tmp/.X11-unix/X0");
	s->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	/* Checks the current string state. */
	if (s->listener < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, "/tmp/.X11-unix/X0");

	/* Handles a failed bind operation. */
	if (bind(s->listener, (struct sockaddr *)&a, sizeof(a)) ||
	    listen(s->listener, 8))

		/* Reports operation failure. */
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

	/* Handles a failed xzed input open operation. */
	if (xzed_input_open(&s->input, s->mode.width, s->mode.height,
	    &input_handlers, s) != 0)

		/* Reports operation failure. */
		return -1;
	s->buttons = xzed_input_buttons(s->input);
	s->key_state = xzed_input_modifiers(s->input);
	s->windows[0].pixels =
	    window_pixels_alloc(s->windows[0].width, s->windows[0].height,
				s->windows[0].background);
	s->screen =
	    malloc((size_t)s->mode.width * s->mode.height * sizeof(*s->screen));
	s->transfer = malloc((size_t)s->mode.width * s->mode.height * 3U);

	/* Checks the current string state. */
	if (!s->windows[0].pixels || !s->screen || !s->transfer) {
		errno = ENOMEM;

		/* Reports operation failure. */
		return -1;
	}
	repaint(s);
	present(s);

	/* Reports successful completion. */
	return 0;
}

/* Supports the choose mode operation. */
static int
choose_mode(
	int fd,
	unsigned preferred_width,
	unsigned preferred_height,
	unsigned preferred_depth,
	struct graphics_mode *chosen)
{
	uint64_t area;
	uint64_t best_area;
	struct graphics_mode_info modes[16];
	struct graphics_mode_list list;
	int best;
	int depth_ok;
	int size_ok;
	unsigned pass, i;

	best = -1;

	memset(&list, 0, sizeof(list));
	list.modes = (uapi_ptr_t)(uintptr_t)modes;
	list.capacity = sizeof(modes) / sizeof(modes[0]);

	/* Handles a failed ioctl operation. */
	if (ioctl(fd, ZEDBSD_GRAPHICS_GET_MODES, &list) != 0 || list.count == 0)
		return -1;

	/* Handles the list condition. */
	if (list.count > list.capacity)
		list.count = list.capacity;

	/*
 * Exact depth and a mode no larger than the request win.  If either is
	 * unavailable, relax depth first and size second. */
	/* Process each element required by the operation. */
	for (pass = 0; pass < 4 && best < 0; pass++) {
		/* Process each remaining element. */
		best_area = 0;
		for (i = 0; i < list.count; i++) {
			area = (uint64_t)modes[i].width * modes[i].height;
			depth_ok = preferred_depth == 0 ||
				   modes[i].bits_per_pixel == preferred_depth;
			size_ok = preferred_width == 0 ||
				  (modes[i].width <= preferred_width &&
				   modes[i].height <= preferred_height);

			/* Handles the pass condition. */
			if ((pass < 2 && !size_ok) ||
			    ((pass == 0 || pass == 2) && !depth_ok))
				continue;

			/* Handles the best condition. */
			if (best < 0 ||
			    (size_ok ? area > best_area : area < best_area)) {
				best = (int)i;
				best_area = area;
			}
		}
	}

	/* Handles the best condition. */
	if (best < 0) {
		errno = ENODEV;

		/* Reports operation failure. */
		return -1;
	}
	memset(chosen, 0, sizeof(*chosen));
	chosen->preferred_width = modes[best].width;
	chosen->preferred_height = modes[best].height;
	chosen->preferred_bits_per_pixel = modes[best].bits_per_pixel;

	/* Reports successful completion. */
	return 0;
}

/* Supports the window pixels alloc operation. */
static uint32_t *
window_pixels_alloc(
	uint16_t width,
	uint16_t height,
	uint32_t color)
{
	size_t count, i;
	uint32_t *p;

	count = (size_t)width * height;

	/* Handles the width condition. */
	if (!width || !height || (width && count / width != height) ||
	    count > SIZE_MAX / sizeof(*p))

		/* Reports that no result is available. */
		return NULL;
	p = malloc(count * sizeof(*p));

	/* Checks the current pointer. */
	if (!p)
		return NULL;

	/* Process each remaining element. */
	for (i = 0; i < count; i++)
		p[i] = color;

	/* Returns the computed result. */
	return p;
}

/* Supports the repaint operation. */
static void
repaint(
	struct server *s)
{
	mark_dirty(s, 0, 0, (int)s->mode.width, (int)s->mode.height);
}

/* Supports the mark dirty operation. */
static void
mark_dirty(
	struct server *s,
	int x,
	int y,
	int w,
	int h)
{
	/*
 * Coalesce all changes until present() into one clipped screen region.
	 */
	if (x < 0) {
		w += x;
		x = 0;
	}

	/* Handles the y condition. */
	if (y < 0) {
		h += y;
		y = 0;
	}

	/* Checks the current horizontal value. */
	if (x + w > (int)s->mode.width)
		w = (int)s->mode.width - x;

	/* Handles the y condition. */
	if (y + h > (int)s->mode.height)
		h = (int)s->mode.height - y;

	/* Handles the w condition. */
	if (w <= 0 || h <= 0)
		return;

	/* Checks the current string state. */
	if (!s->dirty) {
		s->dirty = 1;
		s->dirty_x0 = x;
		s->dirty_y0 = y;
		s->dirty_x1 = x + w;
		s->dirty_y1 = y + h;

		/* Returns the computed result. */
		return;
	}

	/* Checks the current horizontal value. */
	if (x < s->dirty_x0)
		s->dirty_x0 = x;

	/* Handles the y condition. */
	if (y < s->dirty_y0)
		s->dirty_y0 = y;

	/* Checks the current horizontal value. */
	if (x + w > s->dirty_x1)
		s->dirty_x1 = x + w;

	/* Handles the y condition. */
	if (y + h > s->dirty_y1)
		s->dirty_y1 = y + h;
}

/* Supports the present operation. */
static void
present(
	struct server *s)
{
	int ax, ay;
	int cx;
	int cy;
	uint32_t color;
	size_t off;
	int row_for;
	int column_for;
	struct graphics_blit b;
	struct graphics_rect r;
	struct graphics_flush f;
	uint16_t shape;
	int hot_x;
	int hot_y;
	int x, y, w, h;

	shape = pointer_shape(s);
	hot_x = shape == XC_LEFT_PTR ? CURSOR_HOT_X : 7;
	hot_y = shape == XC_LEFT_PTR ? CURSOR_HOT_Y : 7;

	/* Checks the current string state. */
	if (!s->dirty)
		return;
	x = s->dirty_x0;
	y = s->dirty_y0;
	w = s->dirty_x1 - x;
	h = s->dirty_y1 - y;
	s->dirty = 0;
	composite_region(s, x, y, w, h);

	/*
 * The cursor is transient: overlay it only in the RGB24 transfer
	 * buffer. */
	/* Process each element required by the operation. */
	for (row_for = 0; row_for < h; row_for++) {
		/* Process each element required by the operation. */
		for (column_for = 0; column_for < w; column_for++) {
			ax = x + column_for;
			ay = y + row_for;
			cx = ax - (s->pointer_x - hot_x);
			cy = ay - (s->pointer_y - hot_y);
			color = s->screen[(size_t)ay * s->mode.width + ax];
			off = ((size_t)row_for * w + column_for) * 3;

			/* Handles the cx condition. */
			if (cx >= 0 && cx < CURSOR_WIDTH && cy >= 0 &&
			    cy < CURSOR_HEIGHT)
				(void)cursor_pixel(shape, cx, cy, &color);
			s->transfer[off] = (uint8_t)(color >> 16);
			s->transfer[off + 1] = (uint8_t)(color >> 8);
			s->transfer[off + 2] = (uint8_t)color;
		}
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

/* Supports the pointer shape operation. */
static uint16_t
pointer_shape(
	struct server *s)
{
	struct window *w = s->pointer_grab_owner >= 0
			       ? find_window(s, s->pointer_grab_window)
			       : input_at(s, s->pointer_x, s->pointer_y);

	/* Continue while the operation condition remains true. */
	while (w && !w->cursor_shape && w->parent)
		w = find_window(s, w->parent);

	/* Returns the computed result. */
	return w && w->cursor_shape ? w->cursor_shape : XC_LEFT_PTR;
}

/* Resource lookup and stacking order. */
static struct window *
find_window(
	struct server *s,
	uint32_t id)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < s->window_count; i++) {
		/* Checks the current string state. */
		if (s->windows[i].id == id)
			return &s->windows[i];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the input at operation. */
static struct window *
input_at(
	struct server *s,
	int x,
	int y)
{
	struct window *w;

	w = input_at_parent(s, ROOT_XID, x, y);

	/* Returns the computed result. */
	return w ? w : &s->windows[0];
}

/* Supports the input at parent operation. */
static struct window *
input_at_parent(
	struct server *s,
	uint32_t parent,
	int x,
	int y)
{
	struct window *child;
	struct window *w;
	int inside;
	unsigned i;

	/* Continue while the operation condition remains true. */
	i = s->window_count;
	while (i-- > 1) {
		w = &s->windows[i];

		/* Handles the w condition. */
		if (!w->mapped || w->parent != parent)
			continue;
		inside = x >= w->x - (int)w->input_left &&
			 y >= w->y - (int)w->input_top &&
			 x < w->x + w->width + (int)w->input_right &&
			 y < w->y + w->height + (int)w->input_bottom;

		/* Handles the inside condition. */
		if (inside) {
			/* Checks the current horizontal value. */
			if (x >= w->x && y >= w->y && x < w->x + w->width &&
			    y < w->y + w->height) {
				child = input_at_parent(s, w->id, x, y);

				/* Checks the child process state. */
				if (child)
					return child;
			}

			/* Returns the computed result. */
			return w;
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Compositing and graphics-device presentation. */
static void
composite_region(
	struct server *s,
	int x,
	int y,
	int width,
	int height)
{
	struct window *w;
	int local_x, local_y;

	/*
 * Resolve each dirty pixel from the topmost retained window contents.
	 */
	int row, column;

	/* Checks the current horizontal value. */
	if (x < 0) {
		width += x;
		x = 0;
	}

	/* Handles the y condition. */
	if (y < 0) {
		height += y;
		y = 0;
	}

	/* Checks the current horizontal value. */
	if (x + width > (int)s->mode.width)
		width = (int)s->mode.width - x;

	/* Handles the y condition. */
	if (y + height > (int)s->mode.height)
		height = (int)s->mode.height - y;

	/* Handles the width condition. */
	if (width <= 0 || height <= 0)
		return;

	/* Process each element required by the operation. */
	for (row = y; row < y + height; row++) {
		/* Process each element required by the operation. */
		for (column = x; column < x + width; column++) {
			w = top_at(s, column, row);
			local_x = column - w->x;
			local_y = row - w->y;
			s->screen[(size_t)row * s->mode.width + column] =
			    w->pixels[(size_t)local_y * w->width + local_x];
		}
	}
}

/* Supports the top at operation. */
static struct window *
top_at(
	struct server *s,
	int x,
	int y)
{
	struct window *w;

	w = top_at_parent(s, ROOT_XID, x, y);

	/* Returns the computed result. */
	return w ? w : &s->windows[0];
}

/* Visual hit testing follows mapped child windows.  Input hit testing also honors the private margins used by the window manager's resize handles. */
static struct window *
top_at_parent(
	struct server *s,
	uint32_t parent,
	int x,
	int y)
{
	struct window *child;
	struct window *w;
	unsigned i;

	/* Continue while the operation condition remains true. */
	i = s->window_count;
	while (i-- > 1) {
		w = &s->windows[i];

		/* Handles the w condition. */
		if (w->mapped && w->parent == parent && x >= w->x &&
		    y >= w->y && x < w->x + w->width && y < w->y + w->height) {
			child = top_at_parent(s, w->id, x, y);

			/* Returns the computed result. */
			return child ? child : w;
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the cursor pixel operation. */
static int
cursor_pixel(
	uint16_t shape,
	int x,
	int y,
	uint32_t *color)
{
	uint16_t bit;
	int nx, ny;

	/* Handles the shape condition. */
	if (shape == XC_LEFT_PTR) {
		bit = (uint16_t)(1U << (unsigned)x);

		/* Handles the pointer mask condition. */
		if (!(pointer_mask[y] & bit))
			return 0;
		*color = (pointer_source[y] & bit) ? 0x000000 : 0xffffff;
		/* Reports operation failure. */
		return 1;
	}

	/* Handles the resize cursor black condition. */
	if (resize_cursor_black(shape, x, y)) {
		*color = 0x000000;
		/* Reports operation failure. */
		return 1;
	}

	/* Process each element required by the operation. */
	for (ny = y - 1; ny <= y + 1; ny++) {
		/* Process each element required by the operation. */
		for (nx = x - 1; nx <= x + 1; nx++) {
			/* Handles a failed resize cursor black operation. */
			if (nx >= 0 && ny >= 0 && nx < CURSOR_WIDTH &&
			    ny < CURSOR_HEIGHT &&
			    resize_cursor_black(shape, nx, ny)) {
				*color = 0xffffff;
				/* Reports operation failure. */
				return 1;
			}
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Software cursor rendering and per-window backing stores. */
static int
resize_cursor_black(
	uint16_t shape,
	int x,
	int y)
{
	int function_result;
	int dx, dy, adx, ady;

	dx = x - 7;
	dy = y - 7;
	adx = abs(dx);
	ady = abs(dy);

	/* Handles the shape condition. */
	if (shape == XC_SB_H_DOUBLE_ARROW) {
		return (ady <= 1 && x >= 2 && x <= 13) ||
		       (x <= 6 && adx + ady <= 5) || (x >= 9 && adx + ady <= 6);
	}

	/* Handles the shape condition. */
	if (shape == XC_SB_V_DOUBLE_ARROW) {
		return (adx <= 1 && y >= 2 && y <= 13) ||
		       (y <= 6 && adx + ady <= 5) || (y >= 9 && adx + ady <= 6);
	}

	/* Handles the shape condition. */
	if (shape == XC_BOTTOM_LEFT_CORNER) {
		/* Computes the function result. */
		function_result = (abs(x + y - 15) <= 1 && x >= 2 && x <= 13 && y >= 2 &&
			y <= 13) ||
		       (x == 2 && y >= 8 && y <= 13) ||
		       (y == 13 && x >= 2 && x <= 7) ||
		       (x == 13 && y >= 2 && y <= 7) ||
		       (y == 2 && x >= 8 && x <= 13);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the shape condition. */
	if (shape == XC_BOTTOM_RIGHT_CORNER) {
		/* Computes the function result. */
		function_result = (abs(x - y) <= 1 && x >= 2 && x <= 13 && y >= 2 &&
			y <= 13) ||
		       (x == 2 && y >= 2 && y <= 7) ||
		       (y == 2 && x >= 2 && x <= 7) ||
		       (x == 13 && y >= 8 && y <= 13) ||
		       (y == 13 && x >= 8 && x <= 13);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the cleanup operation. */
static void
cleanup(
	struct server *s)
{
	unsigned i;

	/* Process each element required by the operation. */
	for (i = 0; i < MAX_CLIENTS; i++)
		close_client(s, i);

	/* Process each remaining element. */
	for (i = 0; i < s->window_count; i++)
		free(s->windows[i].pixels);

	/* Process each remaining element. */
	for (i = 0; i < s->pixmap_count; i++)
		free(s->pixmaps[i].pixels);
	free(s->screen);
	free(s->transfer);

	/* Checks the current string state. */
	if (s->listener >= 0)
		close(s->listener);
	(void)unlink("/tmp/.X11-unix/X0");
	xzed_input_close(s->input);

	/* Checks the current string state. */
	if (s->graphics >= 0)
		close(s->graphics);
}

/* Client connection lifecycle and incremental protocol decoding. */
static void
close_client(
	struct server *s,
	unsigned i)
{
	uint32_t id;
	unsigned j;

	/* Checks the current string state. */
	if (s->clients[i].fd >= 0)
		close(s->clients[i].fd);

	/* Continue until the operation reaches a terminal state. */
	s->clients[i].fd = -1;
	for (;;) {
		/* Process each remaining element. */
		id = 0;
		for (j = 1; j < s->window_count; j++) {
			/* Checks the current string state. */
			if (s->windows[j].owner == (uint32_t)i) {
				id = s->windows[j].id;
				break;
			}
		}

		/* Handles the id condition. */
		if (!id)
			break;
		destroy_window(s, id);
	}

	/* Process each remaining element. */
	for (j = 0; j < s->pixmap_count;) {
		/* Checks the current string state. */
		if (s->pixmaps[j].owner == (int)i) {
			free(s->pixmaps[j].pixels);

			/* Handles the j condition. */
			if (j + 1U < s->pixmap_count) {
				memmove(&s->pixmaps[j], &s->pixmaps[j + 1],
					(s->pixmap_count - j - 1U) *
					    sizeof(s->pixmaps[0]));
			}
			s->pixmap_count--;
			memset(&s->pixmaps[s->pixmap_count], 0,
			       sizeof(s->pixmaps[0]));
		} else {
			j++;
		}
	}

	/* Process each remaining element. */
	for (j = 0; j < s->gc_count;) {
		/* Checks the current string state. */
		if (s->gcs[j].owner == (int)i) {
			/* Handles the j condition. */
			if (j + 1U < s->gc_count) {
				memmove(&s->gcs[j], &s->gcs[j + 1],
					(s->gc_count - j - 1U) *
					    sizeof(s->gcs[0]));
			}
			s->gc_count--;
			memset(&s->gcs[s->gc_count], 0, sizeof(s->gcs[0]));
		} else {
			j++;
		}
	}

	/* Process each remaining element. */
	for (j = 0; j < s->font_count;) {
		/* Checks the current string state. */
		if (s->fonts[j].owner == (int)i) {
			/* Handles the j condition. */
			if (j + 1U < s->font_count) {
				memmove(&s->fonts[j], &s->fonts[j + 1],
					(s->font_count - j - 1U) *
					    sizeof(s->fonts[0]));
			}
			s->font_count--;
			memset(&s->fonts[s->font_count], 0,
			       sizeof(s->fonts[0]));
		} else {
			j++;
		}
	}
	free(s->clients[i].input);
	memset(&s->clients[i], 0, sizeof(s->clients[i]));
	s->clients[i].fd = -1;
}

/* Supports the destroy window operation. */
static void
destroy_window(
	struct server *s,
	uint32_t id)
{
	unsigned i;
	struct window *w, *parent;
	size_t index;
	uint32_t child;

	/* Handles a failed find window operation. */
	if (id == ROOT_XID || (w = find_window(s, id)) == NULL)
		return;

	/*
 * Children must disappear first so no resource retains a dead parent.
	 */
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Process each remaining element. */
		child = 0;
		for (i = 1; i < s->window_count; i++) {
			/* Checks the current string state. */
			if (s->windows[i].parent == id) {
				child = s->windows[i].id;
				break;
			}
		}

		/* Checks the child process state. */
		if (!child)
			break;
		destroy_window(s, child);
	}
	w = find_window(s, id);

	/* Handles the w condition. */
	if (!w)
		return;
	parent = find_window(s, w->parent);

	/* Handles the w condition. */
	if ((w->event_mask & (1U << 17)) != 0)
		destroy_notify(owner_client(s, w->owner), w->id, w->id);

	/* Handles the parent condition. */
	if (parent && (parent->event_mask & (1U << 19)) != 0) {
		destroy_notify(owner_client(s, parent->owner), parent->id,
			       w->id);
	}
	mark_dirty(s, w->x, w->y, w->width, w->height);

	/* Checks the current string state. */
	if (s->focus == w->id)
		s->focus = ROOT_XID;

	/* Checks the current string state. */
	if (s->pointer_grab_window == w->id) {
		s->pointer_grab_owner = -1;
		s->pointer_grab_window = 0;
	}
	free(w->pixels);
	w->pixels = NULL;
	index = (size_t)(w - s->windows);

	/* Checks the current index. */
	if (index + 1U < s->window_count) {
		memmove(&s->windows[index], &s->windows[index + 1],
			(s->window_count - index - 1U) * sizeof(s->windows[0]));
	}
	s->window_count--;
	memset(&s->windows[s->window_count], 0, sizeof(s->windows[0]));
}

/* Supports the destroy notify operation. */
static void
destroy_notify(
	struct client *c,
	uint32_t event,
	uint32_t window)
{
	uint8_t e[32];

	/* Classifies the current input character. */
	if (!c)
		return;
	memset(e, 0, sizeof(e));
	e[0] = 17;
	wr16(e + 2, c->sequence, c->order);
	wr32(e + 4, event, c->order);
	wr32(e + 8, window, c->order);
	(void)write_all(c->fd, e, sizeof(e));
}

/* Supports the wr16 operation. */
static void
wr16(
	uint8_t *p,
	uint16_t v,
	int msb)
{
	/* Handles the msb condition. */
	if (msb) {
		p[0] = (uint8_t)(v >> 8);
		p[1] = (uint8_t)v;
	} else {
		p[0] = (uint8_t)v;
		p[1] = (uint8_t)(v >> 8);
	}
}

/* Supports the wr32 operation. */
static void
wr32(
	uint8_t *p,
	uint32_t v,
	int msb)
{
	/* Handles the msb condition. */
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

/* Supports the write all operation. */
static int
write_all(
	int fd,
	const void *v,
	size_t n)
{
	ssize_t r;
	const uint8_t *p;

	/* Continue while the operation condition remains true. */
	p = v;
	while (n) {
		r = write(fd, p, n);

		/* Handles the r condition. */
		if (r < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the r condition. */
		if (!r) {
			errno = EIO;

			/* Reports operation failure. */
			return -1;
		}
		p += r;
		n -= (size_t)r;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the owner client operation. */
static struct client *
owner_client(
	struct server *s,
	uint32_t owner)
{
	/* Returns the computed result. */
	return owner < MAX_CLIENTS && s->clients[owner].fd >= 0
		   ? &s->clients[owner]
		   : NULL;
}

/* Supports the finish pointer input operation. */
static void
finish_pointer_input(
	struct server *s)
{
	flush_pointer_motion(s);

	/* Checks the current string state. */
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

/* Supports the flush pointer motion operation. */
static void
flush_pointer_motion(
	struct server *s)
{
	/* Checks the current string state. */
	if (!s->pending_motion)
		return;
	send_motion_event(s->pending_motion_client, s->pending_motion_window,
	    (uint64_t)s->pending_motion_time * 1000000U, s->pending_motion_x,
	    s->pending_motion_y, s->pending_motion_buttons);
	s->pending_motion = 0;
}

/* Supports the send motion event operation. */
static void
send_motion_event(
	struct client *c,
	struct window *w,
	uint64_t time,
	int x,
	int y,
	uint16_t buttons)
{
	/* Classifies the current input character. */
	if (c && w && (w->event_mask & (1U << 6))) {
		send_event(c, 6, w->id, 0, (uint32_t)(time / 1000000), x, y,
			   buttons);
	}
}

/* X11 event and reply construction. */
static void
send_event(
	struct client *c,
	uint8_t type,
	uint32_t window,
	uint32_t detail,
	uint32_t time,
	int16_t rx,
	int16_t ry,
	uint16_t state)
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

/* Supports the read client operation. */
static void
read_client(
	struct server *s,
	unsigned i)
{
	size_t z;
	uint16_t authn, authd;
	uint16_t units;
	size_t need;
	struct client *c = &s->clients[i];
	uint8_t temp[4096];
	ssize_t nr;

	/* Process each remaining element. */
	while ((nr = recv(c->fd, temp, sizeof(temp), MSG_DONTWAIT)) > 0) {
		/* Classifies the current input character. */
		if (c->used + (size_t)nr > INPUT_CAP) {
			close_client(s, i);

			/* Returns the computed result. */
			return;
		}

		/* Classifies the current input character. */
		if (c->used + (size_t)nr > c->capacity) {
			/* Process each remaining element. */
			z = c->capacity ? c->capacity * 2 : 4096;
			while (z < c->used + (size_t)nr)
				z *= 2;
			c->input = realloc(c->input, z);

			/* Classifies the current input character. */
			if (!c->input) {
				close_client(s, i);

				/* Returns the computed result. */
				return;
			}
			c->capacity = z;
		}
		memcpy(c->input + c->used, temp, (size_t)nr);
		c->used += (size_t)nr;
	}

	/* Handles the nr condition. */
	if (nr == 0) {
		close_client(s, i);

		/* Returns the computed result. */
		return;
	}

	/* Handles the reported system error. */
	if (nr < 0 && errno != EAGAIN && errno != EINTR) {
		close_client(s, i);

		/* Returns the computed result. */
		return;
	}

	/*
 * One read may contain a partial request or several complete requests.
	 */
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Classifies the current input character. */
		if (!c->setup) {
			/* Classifies the current input character. */
			if (c->used < 12)
				return;

			/* Classifies the current input character. */
			if (c->input[0] != 'l' && c->input[0] != 'B') {
				close_client(s, i);

				/* Returns the computed result. */
				return;
			}
			c->order = c->input[0] == 'B';
			authn = rd16(c->input + 6, c->order);
			authd = rd16(c->input + 8, c->order);
			need = 12 + ((authn + 3) & ~3U) + ((authd + 3) & ~3U);

			/* Classifies the current input character. */
			if (c->used < need)
				return;

			/* Handles the setup reply condition. */
			if (setup_reply(s, c)) {
				close_client(s, i);

				/* Returns the computed result. */
				return;
			}
			c->setup = 1;
		} else {
			/* Classifies the current input character. */
			if (c->used < 4)
				return;
			units = rd16(c->input + 2, c->order);

			/* Handles the units condition. */
			if (!units) {
				close_client(s, i);

				/* Returns the computed result. */
				return;
			}
			need = (size_t)units * 4;

			/* Classifies the current input character. */
			if (c->used < need)
				return;
			(void)request(s, i, c->input, need);
		}
		memmove(c->input, c->input + need, c->used - need);
		c->used -= need;
	}
}

/* X11 requests use the byte order selected by each client during setup. */
static uint16_t
rd16(
	const uint8_t *p,
	int msb)
{
	/* Returns the computed result. */
	return msb ? (uint16_t)((p[0] << 8) | p[1])
		   : (uint16_t)(p[0] | (p[1] << 8));
}

/* Supports the setup reply operation. */
static int
setup_reply(
	struct server *s,
	struct client *c)
{
	int function_result;

	/* Describe the single 24-bit TrueColor screen exposed by Xzed. */
	static const char vendor[] = "zedBSD Xzed";
	uint8_t out[8 + 32 + 12 + 8 + 40 + 8 + 24];
	uint8_t *p;
	uint16_t units;

	p = out + 8;
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

	/* Obtains the write all result. */
	function_result = write_all(c->fd, out, sizeof(out));

	/* Returns the computed result. */
	return function_result;
}

/* Dispatch the supported core requests plus Xzed's private opcodes.  A case returns after handling a valid request; falling through the switch emits BadWindow/BadValue using the request's resource field. */
static int
request(
	struct server *s,
	unsigned ci,
	const uint8_t *q,
	size_t n)
{
	static const char name[] = "zed-unicode";
	uint32_t kc, ks;
	uint32_t mask_local;
	size_t off_local;
	unsigned bit_local;
	uint32_t v_local;
	uint32_t mask_local2;
	size_t off_local3;
	unsigned bit_local4;
	uint32_t v_local1;
	struct window *p_local;
	struct window *p_local5;
	int x_local, y_local, wi_local, he_local;
	uint16_t mask_local7;
	size_t off_local8;
	unsigned bit_local9;
	int oldx_local, oldy_local, oldw_local, oldh_local, newx_local, newy_local, raise_local;
	uint32_t v_local6;
	uint8_t r_local[32];
	uint8_t r_local10[32 + MAX_WINDOWS * 4];
	unsigned i_local, count_local;
	uint32_t property_local, type_local, count_local11;
	uint8_t r_local12[32 + 160];
	uint32_t property_local13;
	uint8_t r_local14[32];
	uint8_t r_local15[32];
	unsigned i_local16;
	uint8_t r_local17[60];
	uint8_t r_local18[44];
	struct pixmap *p_local19;
	size_t count_local20;
	struct pixmap *p_local21;
	struct graphics_context *g_local;
	uint32_t mask_local23;
	size_t off_local24;
	unsigned bit_local25;
	uint32_t v_local22;
	struct graphics_context *g_local30;
	uint32_t mask_local27;
	size_t off_local28;
	unsigned bit_local29;
	uint32_t v_local26;
	unsigned i_local31;
	struct pixmap *p_local32;
	struct graphics_context *g_local33;
	size_t off_local34;
	int x_local35, y_local36;
	uint32_t color_local;
	int x0_local, y0_local, dx_local, sx_local, dy_local, sy_local, err_local;
	struct pixmap *p_local41;
	struct graphics_context *g_local42;
	size_t off_local43;
	uint32_t color_local44;
	int16_t x_local37, y_local38;
	uint16_t wi_local39, he_local40;
	struct graphics_context *g_local45;
	uint8_t r_local46[32 + 4 * 248];
	unsigned i_local47;
	uint8_t r_local48[32 + 4];
	int dx_local49, dy_local50;
	uint32_t color_local51;
	int32_t newx_local52, newy_local53;
	unsigned j;
	uint16_t neww, newh, newborder;
	size_t copy;
	char *target;
	size_t capacity;
	const char *value;
	struct window *h;
	uint16_t ln;
	size_t pi;
	int px, py, wx, wy;
	struct window *dest;
	int e2;
	int nx, ny;
	size_t chars;
	const uint8_t *rgb;
	uint32_t drawable;
	uint16_t shape;
	uint32_t width, height;
	struct client *c = &s->clients[ci];
	uint8_t op = q[0];
	uint32_t id;
	struct window *w;
	size_t count, padded;
	int sx, sy, dx, dy;
	int wi, he, row, column;
	struct pixmap *p;
	int x, y;
	int oldx, oldy, oldw, oldh;

	c->sequence++;

	/* Dispatch the selected operation case. */
	switch (op) {
	case 1: /* CreateWindow */
		if (n < 32 || s->window_count == MAX_WINDOWS)
			break;
		id = rd32(q + 4, c->order);

		/* Handles the find window condition. */
		if (find_window(s, id)) {
			error_reply(c, 14, id, op);

			/* Reports successful completion. */
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

		/* Process each element required by the operation. */
		mask_local = rd32(q + 28, c->order);
		off_local = 32;
		for (bit_local = 0; bit_local < 32 && off_local + 4 <= n; bit_local++) {
			/* Handles the mask local condition. */
			if (mask_local & (1U << bit_local)) {
				v_local = rd32(q + off_local, c->order);

				/* Handles the bit local condition. */
				if (bit_local == 1)
					w->background = v_local;

				/* Handles the bit local condition. */
				if (bit_local == 11)
					w->event_mask = v_local;
				off_local += 4;
			}
		}
		w->pixels =
		    window_pixels_alloc(w->width, w->height, w->background);

		/* Handles the w condition. */
		if (!w->pixels) {
			s->window_count--;
			memset(w, 0, sizeof(*w));
			error_reply(c, 11, id, op);

			/* Reports successful completion. */
			return 0;
		}

		/* Reports successful completion. */
		return 0;
	case 2: /* ChangeWindowAttributes */
		if (n < 12 ||
		    (w = find_window(s, rd32(q + 4, c->order))) == NULL)
			break;

		/* Process each element required by the operation. */
		mask_local2 = rd32(q + 8, c->order);
		off_local3 = 12;
		for (bit_local4 = 0; bit_local4 < 32 && off_local3 + 4 <= n; bit_local4++) {
			/* Handles the mask local2 condition. */
			if (mask_local2 & (1U << bit_local4)) {
				v_local1 = rd32(q + off_local3, c->order);

				/* Handles the bit local4 condition. */
				if (bit_local4 == 1)
					w->background = v_local1;

				/* Handles the bit local4 condition. */
				if (bit_local4 == 11) {
					w->event_mask = v_local1;

					/* Handles the w condition. */
					if (w == &s->windows[0])
						w->owner = ci;
				}
				off_local3 += 4;
			}
		}

		/* Reports successful completion. */
		return 0;
	case 4: /* DestroyWindow */
		destroy_window(s, rd32(q + 4, c->order));

		/* Reports successful completion. */
		return 0;
	case 7: /* ReparentWindow */
		if (n >= 16 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			p_local = find_window(s, rd32(q + 8, c->order));

			/* Handles the p local condition. */
			if (p_local) {
				w->parent = p_local->id;
				w->x = (int16_t)(p_local->x + (int16_t)rd16(
							    q + 12, c->order));
				w->y = (int16_t)(p_local->y + (int16_t)rd16(
							    q + 14, c->order));

				/* Reports successful completion. */
				return 0;
			}
		}
		break;
	case 8: /* MapWindow */
		if ((w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			p_local5 = find_window(s, w->parent);

			/* Handles the p local5 condition. */
			if (p_local5 && (p_local5->event_mask & (1U << 20)) &&
			    p_local5->owner != ci) {
				map_request(s, p_local5, w);

				/* Reports successful completion. */
				return 0;
			}
			w->mapped = 1;
			mark_dirty(s, w->x, w->y, w->width, w->height);
			expose(s, w);

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 10: /* UnmapWindow */
		if ((w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			x_local = w->x;
			y_local = w->y;
			wi_local = w->width;
			he_local = w->height;
			w->mapped = 0;
			mark_dirty(s, x_local, y_local, wi_local, he_local);

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 12: /* ConfigureWindow */
		if (n >= 12 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			mask_local7 = rd16(q + 8, c->order);
			off_local8 = 12;

			/* Process each element required by the operation. */
			oldx_local = w->x;
			oldy_local = w->y;
			oldw_local = w->width;
			oldh_local = w->height;
			newx_local = w->x;
			newy_local = w->y;
			raise_local = 0;
			neww = w->width;
			newh = w->height;
			newborder = w->border;
			for (bit_local9 = 0; bit_local9 < 7 && off_local8 + 4 <= n; bit_local9++) {
				/* Handles the mask local7 condition. */
				if (mask_local7 & (1U << bit_local9)) {
					v_local6 = rd32(q + off_local8, c->order);

					/* Handles the bit local9 condition. */
					if (bit_local9 == 0)
						newx_local = (int16_t)v_local6;
					else if (bit_local9 == 1)
						newy_local = (int16_t)v_local6;
					else if (bit_local9 == 2)
						neww = (uint16_t)v_local6;
					else if (bit_local9 == 3)
						newh = (uint16_t)v_local6;
					else if (bit_local9 == 4)
						newborder = (uint16_t)v_local6;
					else if (bit_local9 == 6 && v_local6 == Above)
						raise_local = 1;
					off_local8 += 4;
				}
			}

			/* Handles a failed window pixels resize operation. */
			if ((neww != w->width || newh != w->height) &&
			    window_pixels_resize(w, neww, newh)) {
				error_reply(c, 11, w->id, op);

				/* Reports successful completion. */
				return 0;
			}
			w->x = (int16_t)newx_local;
			w->y = (int16_t)newy_local;
			w->width = neww;
			w->height = newh;
			w->border = newborder;

			/* Handles the w condition. */
			if (w->x != oldx_local || w->y != oldy_local) {
				/* Process each remaining element. */
				for (j = 1; j < s->window_count; j++) {
					/* Checks the current string state. */
					if (s->windows[j].parent == w->id) {
						s->windows[j].x +=
						    (int16_t)(w->x - oldx_local);
						s->windows[j].y +=
						    (int16_t)(w->y - oldy_local);
					}
				}
			}
			mark_dirty(s, oldx_local, oldy_local, oldw_local, oldh_local);
			mark_dirty(s, w->x, w->y, w->width, w->height);

			/* Handles the neww condition. */
			if (neww != (uint16_t)oldw_local || newh != (uint16_t)oldh_local)
				expose(s, w);

			/* Handles the raise local condition. */
			if (raise_local)
				raise_window(s, top_level_window(s, w));

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 14: /* GetGeometry */
		if ((w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			memset(r_local, 0, 32);
			r_local[1] = 24;
			wr32(r_local + 8, ROOT_XID, c->order);
			wr16(r_local + 12, (uint16_t)w->x, c->order);
			wr16(r_local + 14, (uint16_t)w->y, c->order);
			wr16(r_local + 16, w->width, c->order);
			wr16(r_local + 18, w->height, c->order);
			wr16(r_local + 20, w->border, c->order);
			simple_reply(c, r_local, 32);

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 15: /* QueryTree: return all direct children in stacking order. */
		if ((w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			count_local = 0;
			memset(r_local10, 0, sizeof(r_local10));
			wr32(r_local10 + 8, ROOT_XID, c->order);
			wr32(r_local10 + 12, w->parent, c->order);

			/* Process each remaining element. */
			for (i_local = 1; i_local < s->window_count; i_local++) {
				/* Checks the current string state. */
				if (s->windows[i_local].id &&
				    s->windows[i_local].parent == w->id) {
					wr32(r_local10 + 32 + count_local * 4,
					     s->windows[i_local].id, c->order);
					count_local++;
				}
			}
			wr16(r_local10 + 16, (uint16_t)count_local, c->order);
			simple_reply(c, r_local10, 32 + count_local * 4);

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 18: /* ChangeProperty: retain desktop string properties. */
		if (n >= 24 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			property_local = rd32(q + 8, c->order);
			type_local = rd32(q + 12, c->order);
			count_local11 = rd32(q + 20, c->order);
			target = NULL;
			capacity = 0;

			/* Handles the property local condition. */
			if (property_local == 39) {
				target = w->name;
				capacity = sizeof(w->name);
			} else if (property_local == XZED_ICON_PATH_ATOM) {
				target = w->icon_path;
				capacity = sizeof(w->icon_path);
			}

			/* Handles the target condition. */
			if (target && type_local == 31 && q[16] == 8 &&
			    count_local11 <= n - 24) {
				copy = count_local11 < capacity - 1 ? count_local11 : capacity - 1;
				memcpy(target, q + 24, copy);
				target[copy] = 0;
			}

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 20: /* GetProperty: WM_NAME and Xzed desktop strings. */
		if (n >= 24 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			property_local13 = rd32(q + 8, c->order);
			value = NULL;
			count = 0;
			padded = 0;
			memset(r_local12, 0, sizeof(r_local12));

			/* Handles the property local13 condition. */
			if (property_local13 == 39)
				value = w->name;
			else if (property_local13 == XZED_ICON_PATH_ATOM)
				value = w->icon_path;

			/* Handles a failed rd32 operation. */
			if (value && value[0] &&
			    (rd32(q + 12, c->order) == 0 ||
			     rd32(q + 12, c->order) == 31)) {
				count = strlen(value);
				padded = (count + 3U) & ~3U;
				r_local12[1] = 8;
				wr32(r_local12 + 8, 31, c->order);
				wr32(r_local12 + 16, (uint32_t)count, c->order);
				memcpy(r_local12 + 32, value, count);
			}
			simple_reply(c, r_local12, 32 + padded);

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 38: /* QueryPointer */

	h = hit(s, s->pointer_x, s->pointer_y);
	memset(r_local14, 0, 32);
	r_local14[1] = 1;
	wr32(r_local14 + 8, ROOT_XID, c->order);
	wr32(r_local14 + 12, h->id == ROOT_XID ? 0 : h->id, c->order);
	wr16(r_local14 + 16, (uint16_t)s->pointer_x, c->order);
	wr16(r_local14 + 18, (uint16_t)s->pointer_y, c->order);
	wr16(r_local14 + 20, (uint16_t)(s->pointer_x - h->x), c->order);
	wr16(r_local14 + 22, (uint16_t)(s->pointer_y - h->y), c->order);
	wr16(r_local14 + 24, (uint16_t)s->key_state, c->order);
	simple_reply(c, r_local14, 32);

	/* Reports successful completion. */
	return 0;
	case 42:
		s->focus = rd32(q + 4, c->order);

		/* Reports successful completion. */
		return 0;
	case 43:

	memset(r_local15, 0, 32);
	r_local15[1] = 0;
	wr32(r_local15 + 8, s->focus, c->order);
	simple_reply(c, r_local15, 32);

	/* Reports successful completion. */
	return 0;
	case 45: /* OpenFont */
		if (n >= 12 && s->font_count < MAX_FONTS) {
			ln = rd16(q + 8, c->order);

			/* Handles the ln condition. */
			if (12U + ln <= n) {
				s->fonts[s->font_count++] =
				    (struct font_resource){
					rd32(q + 4, c->order), (int)ci};

				/* Reports successful completion. */
				return 0;
			}
		}
		break;
	case 46: /* CloseFont */

	/* Process each remaining element. */
	id = rd32(q + 4, c->order);
	for (i_local16 = 0; i_local16 < s->font_count; i_local16++) {
		/* Checks the current string state. */
		if (s->fonts[i_local16].id == id) {
			/* Handles the i local16 condition. */
			if (i_local16 + 1U < s->font_count) {
				memmove(&s->fonts[i_local16], &s->fonts[i_local16 + 1],
					(s->font_count - i_local16 - 1U) *
					    sizeof(s->fonts[0]));
			}
			s->font_count--;
			memset(&s->fonts[s->font_count], 0,
			       sizeof(s->fonts[0]));

			/* Reports successful completion. */
			return 0;
		}
	}
	break;
	case 47: /*
 * QueryFont: the font is Unicode BMP, with 1- or 2-cell
		    glyphs. */

	memset(r_local17, 0, sizeof(r_local17));
	wr16(r_local17 + 8, 0, c->order);
	wr16(r_local17 + 10, 255, c->order);
	wr16(r_local17 + 12, 0, c->order);
	wr16(r_local17 + 14, 255, c->order);
	r_local17[16] = 0;
	r_local17[17] = 0;
	wr16(r_local17 + 18, 0, c->order);
	wr16(r_local17 + 20, 16, c->order);
	wr32(r_local17 + 56, 0, c->order);
	simple_reply(c, r_local17, sizeof(r_local17));

	/* Reports successful completion. */
	return 0;
	case 49: /* ListFonts */

	memset(r_local18, 0, sizeof(r_local18));
	wr16(r_local18 + 8, 1, c->order);
	r_local18[32] = (uint8_t)(sizeof(name) - 1);
	memcpy(r_local18 + 33, name, sizeof(name) - 1);
	simple_reply(c, r_local18, sizeof(r_local18));

	/* Reports successful completion. */
	return 0;
	case 53: /* CreatePixmap */
		if (n >= 16 && s->pixmap_count < MAX_PIXMAPS) {
			p_local19 = &s->pixmaps[s->pixmap_count];

			memset(p_local19, 0, sizeof(*p_local19));
			p_local19->id = rd32(q + 4, c->order);
			p_local19->owner = (int)ci;
			p_local19->width = rd16(q + 12, c->order);
			p_local19->height = rd16(q + 14, c->order);
			count_local20 = (size_t)p_local19->width * p_local19->height;

			/* Handles a failed calloc operation. */
			if (!p_local19->width || !p_local19->height ||
			    (p_local19->width && count_local20 / p_local19->width != p_local19->height) ||
			    (p_local19->pixels = calloc(count_local20, sizeof(*p_local19->pixels))) ==
				NULL) {
				error_reply(c, 11, p_local19->id, op);

				/* Reports successful completion. */
				return 0;
			}
			s->pixmap_count++;

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 54: /* FreePixmap */

	p_local21 = find_pixmap(s, rd32(q + 4, c->order));

	/* Handles the p local21 condition. */
	if (p_local21) {
		pi = (size_t)(p_local21 - s->pixmaps);
		free(p_local21->pixels);

		/* Handles the pi condition. */
		if (pi + 1U < s->pixmap_count) {
			memmove(&s->pixmaps[pi], &s->pixmaps[pi + 1],
				(s->pixmap_count - pi - 1U) *
				    sizeof(s->pixmaps[0]));
		}
		s->pixmap_count--;
		memset(&s->pixmaps[s->pixmap_count], 0,
		       sizeof(s->pixmaps[0]));

		/* Reports successful completion. */
		return 0;
	}
	break;
	case 55: /* CreateGC */
		if (n >= 16 && s->gc_count < MAX_GCS) {
			g_local = &s->gcs[s->gc_count++];
			mask_local23 = rd32(q + 12, c->order);
			off_local24 = 16;

			memset(g_local, 0, sizeof(*g_local));

			/* Process each element required by the operation. */
			g_local->id = rd32(q + 4, c->order);
			g_local->owner = (int)ci;
			g_local->foreground = 0xffffff;
			for (bit_local25 = 0; bit_local25 < 32 && off_local24 + 4 <= n; bit_local25++) {
				/* Handles the mask local23 condition. */
				if (mask_local23 & (1U << bit_local25)) {
					v_local22 = rd32(q + off_local24, c->order);

					/* Handles the bit local25 condition. */
					if (bit_local25 == 2)
						g_local->foreground = v_local22;

					/* Handles the bit local25 condition. */
					if (bit_local25 == 14)
						g_local->font = v_local22;
					off_local24 += 4;
				}
			}

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 56: /* ChangeGC */

	g_local30 = find_gc(s, rd32(q + 4, c->order));

	/* Handles the g local30 condition. */
	if (g_local30 && n >= 12) {
		mask_local27 = rd32(q + 8, c->order);
		off_local28 = 12;

		/* Process each element required by the operation. */
		for (bit_local29 = 0; bit_local29 < 32 && off_local28 + 4 <= n; bit_local29++) {
			/* Handles the mask local27 condition. */
			if (mask_local27 & (1U << bit_local29)) {
				v_local26 = rd32(q + off_local28, c->order);

				/* Handles the bit local29 condition. */
				if (bit_local29 == 2)
					g_local30->foreground = v_local26;

				/* Handles the bit local29 condition. */
				if (bit_local29 == 14)
					g_local30->font = v_local26;
				off_local28 += 4;
			}
		}

		/* Reports successful completion. */
		return 0;
	}
	break;
	case 60: /* FreeGC */

	/* Process each remaining element. */
	id = rd32(q + 4, c->order);
	for (i_local31 = 0; i_local31 < s->gc_count; i_local31++) {
		/* Checks the current string state. */
		if (s->gcs[i_local31].id == id) {
			/* Handles the i local31 condition. */
			if (i_local31 + 1U < s->gc_count) {
				memmove(&s->gcs[i_local31], &s->gcs[i_local31 + 1],
					(s->gc_count - i_local31 - 1U) *
					    sizeof(s->gcs[0]));
			}
			s->gc_count--;
			memset(&s->gcs[s->gc_count], 0,
			       sizeof(s->gcs[0]));

			/* Reports successful completion. */
			return 0;
		}
	}
	break;
	case 62: /*
 * CopyArea: the minimal server currently supports Pixmap to
		    Window. */
		if (n >= 28) {
			p_local32 = find_pixmap(s, rd32(q + 4, c->order));
			dest = find_window(s, rd32(q + 8, c->order));
			sx = (int16_t)rd16(q + 16, c->order);
			sy = (int16_t)rd16(q + 18, c->order);
			dx = (int16_t)rd16(q + 20, c->order);
			dy = (int16_t)rd16(q + 22, c->order);
			wi = rd16(q + 24, c->order);
			he = rd16(q + 26, c->order);

			/* Handles the p local32 condition. */
			if (!p_local32 || !dest)
				break;

			/* Process each element required by the operation. */
			for (row = 0; row < he; row++) {
				/* Process each element required by the operation. */
				for (column = 0; column < wi; column++) {
					px = sx + column;
					py = sy + row;
					wx = dx + column;
					wy = dy + row;

					/* Handles the px condition. */
					if (px >= 0 && py >= 0 &&
					    px < p_local32->width && py < p_local32->height &&
					    wx >= 0 && wy >= 0 &&
					    wx < dest->width &&
					    wy < dest->height) {
						dest->pixels[(size_t)wy *
								 dest->width +
							     wx] =
						    p_local32->pixels[(size_t)py *
								  p_local32->width +
							      px];
					}
				}
			}
			mark_dirty(s, dest->x + dx, dest->y + dy, wi, he);
			present(s);

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 65: /* PolyLine */
		if (n >= 16 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			g_local33 = find_gc(s, rd32(q + 8, c->order));

			/* Process each element required by the operation. */
			x_local35 = 0;
			y_local36 = 0;
			color_local = g_local33 ? g_local33->foreground : 0xffffff;
			for (off_local34 = 12; off_local34 + 4 <= n; off_local34 += 4) {
				nx = (int16_t)rd16(q + off_local34, c->order);
				ny = (int16_t)rd16(q + off_local34 + 2, c->order);

				/* Handles the off local34 condition. */
				if (off_local34 != 12) {
					/* Continue until the operation reaches a terminal state. */
					x0_local = x_local35;
					y0_local = y_local36;
					dx_local = abs(nx - x0_local);
					sx_local = x0_local < nx ? 1 : -1;
					dy_local = -abs(ny - y0_local);
					sy_local = y0_local < ny ? 1 : -1;
					err_local = dx_local + dy_local;
					for (;;) {
						/* Handles the x0 local condition. */
						if (x0_local >= 0 && y0_local >= 0 &&
						    x0_local < w->width &&
						    y0_local < w->height) {
							w->pixels[(size_t)y0_local *
								      w->width +
								  x0_local] = color_local;
						}

						/* Handles the x0 local condition. */
						if (x0_local == nx && y0_local == ny)
							break;

						e2 = 2 * err_local;

						/* Handles the e2 condition. */
						if (e2 >= dy_local) {
							err_local += dy_local;
							x0_local += sx_local;
						}

						/* Handles the e2 condition. */
						if (e2 <= dx_local) {
							err_local += dx_local;
							y0_local += sy_local;
						}
					}
					mark_dirty(s, w->x + (x_local35 < nx ? x_local35 : nx),
						   w->y + (y_local36 < ny ? y_local36 : ny),
						   abs(nx - x_local35) + 1,
						   abs(ny - y_local36) + 1);
				}
				x_local35 = nx;
				y_local36 = ny;
			}
		}

		/* Reports successful completion. */
		return 0;
	case 70: /* PolyFillRectangle */
		if (n >= 12) {
			g_local42 = find_gc(s, rd32(q + 8, c->order));

			color_local44 = g_local42 ? g_local42->foreground : 0xffffff;
			w = find_window(s, rd32(q + 4, c->order));
			p_local41 = find_pixmap(s, rd32(q + 4, c->order));

			/* Handles the w condition. */
			if (!w && !p_local41)
				break;

			/* Process each element required by the operation. */
			for (off_local43 = 12; off_local43 + 8 <= n; off_local43 += 8) {
				x_local37 = (int16_t)rd16(q + off_local43, c->order);
				y_local38 = (int16_t)rd16(q + off_local43 + 2,
							  c->order);
				wi_local39 = rd16(q + off_local43 + 4, c->order);
				he_local40 = rd16(q + off_local43 + 6, c->order);

				/* Handles the w condition. */
				if (w)
					window_fill(s, w, x_local37, y_local38, wi_local39, he_local40, color_local44);
				else
					pixmap_fill(p_local41, x_local37, y_local38, wi_local39, he_local40, color_local44);

				/* Handles the w condition. */
				if (w == &s->windows[0] && x_local37 == 0 && y_local38 == 0 &&
				    wi_local39 >= w->width && he_local40 >= w->height)
					s->windows[0].background = color_local44;
			}

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 76:
	case 77: /* ImageText8 / ImageText16 */
		if (n >= 16 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			g_local45 = find_gc(s, rd32(q + 8, c->order));
			chars = q[1];

			/* Handles the chars condition. */
			if (16 + chars * (op == 77 ? 2U : 1U) <= n) {
				(void)draw_text(s, w, g_local45,
						(int16_t)rd16(q + 12, c->order),
						(int16_t)rd16(q + 14, c->order),
						q + 16, chars, op == 77);
			}

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 101:
		/* GetKeyboardMapping */

	memset(r_local46, 0, sizeof(r_local46));

	/* Process each element required by the operation. */
	r_local46[1] = 1;
	for (i_local47 = 0; i_local47 < q[5] && i_local47 < 248; i_local47++) {
		kc = (uint32_t)(q[4] + i_local47);
		ks = kc >= 8 ? kc - 8 : 0;

		/* Dispatch the selected operation case. */
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
		wr32(r_local46 + 32 + i_local47 * 4, ks, c->order);
	}
	simple_reply(c, r_local46, 32 + (size_t)q[5] * 4);

	/* Reports successful completion. */
	return 0;
	case 117:

	memset(r_local48, 0, sizeof(r_local48));
	r_local48[1] = 3;
	r_local48[32] = 1;
	r_local48[33] = 2;
	r_local48[34] = 3;
	simple_reply(c, r_local48, 36);

	/* Reports successful completion. */
	return 0;
	case 128: /* XzedPutImageRGB24: compact private RGB24 image upload. */
		if (n >= 16) {
			drawable = rd32(q + 4, c->order);
			p = find_pixmap(s, drawable);
			x = (int16_t)rd16(q + 8, c->order);
			y = (int16_t)rd16(q + 10, c->order);
			wi = rd16(q + 12, c->order);
			he = rd16(q + 14, c->order);
			count = (size_t)wi * he;
			w = find_window(s, drawable);

			/* Handles the w condition. */
			if ((!w && !p) || !wi || !he ||
			    (wi && count / (size_t)wi != (size_t)he) ||
			    count > (n - 16U) / 3U)
				break;

			/* Process each element required by the operation. */
			for (row = 0; row < he; row++) {
				/* Process each element required by the operation. */
				for (column = 0; column < wi; column++) {
					rgb = q + 16U +
					    ((size_t)row * wi + column) * 3U;
					dx_local49 = x + (int)column;
					dy_local50 = y + (int)row;
					color_local51 = ((uint32_t)rgb[0] << 16) |
					    ((uint32_t)rgb[1] << 8) | rgb[2];

					/* Handles the w condition. */
					if (w) {
						/* Handles the dx local49 condition. */
						if (dx_local49 >= 0 && dy_local50 >= 0 &&
						    dx_local49 < w->width &&
						    dy_local50 < w->height) {
							w->pixels[(size_t)dy_local50 *
								      w->width +
								  (unsigned)
								      dx_local49] =
							    color_local51;
						}
					} else if (dx_local49 >= 0 && dy_local50 >= 0 &&
						   dx_local49 < p->width &&
						   dy_local50 < p->height) {
						p->pixels[(size_t)dy_local50 *
							      p->width +
							  (unsigned)dx_local49] = color_local51;
					}
				}
			}

			/* Handles the w condition. */
			if (w) {
				mark_dirty(s, w->x + x, w->y + y, (int)wi,
					   (int)he);
			}

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 129: /* XzedSetCursorShape */
		if (n >= 12 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			shape = (uint16_t)rd32(q + 8, c->order);

			/* Handles the shape condition. */
			if (shape != XC_LEFT_PTR &&
			    shape != XC_BOTTOM_LEFT_CORNER &&
			    shape != XC_BOTTOM_RIGHT_CORNER &&
			    shape != XC_SB_H_DOUBLE_ARROW &&
			    shape != XC_SB_V_DOUBLE_ARROW)
				break;
			w->cursor_shape = shape;
			mark_dirty(s, s->pointer_x - 16, s->pointer_y - 16, 32,
				   32);

			/* Reports successful completion. */
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

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 131: /* XzedMoveResizeWindowBuffered: no child move or Expose. */
		if (n >= 24 &&
		    (w = find_window(s, rd32(q + 4, c->order))) != NULL) {
			newx_local52 = (int32_t)rd32(q + 8, c->order);
			newy_local53 = (int32_t)rd32(q + 12, c->order);
			width = rd32(q + 16, c->order);
			height = rd32(q + 20, c->order);
			oldx = w->x;
			oldy = w->y;
			oldw = w->width;
			oldh = w->height;

			/* Handles the newx local52 condition. */
			if (newx_local52 < INT16_MIN || newx_local52 > INT16_MAX ||
			    newy_local53 < INT16_MIN || newy_local53 > INT16_MAX || !width ||
			    width > UINT16_MAX || !height ||
			    height > UINT16_MAX)
				break;

			/* Handles the window pixels resize buffered condition. */
			if (window_pixels_resize_buffered(
				w, (uint16_t)width, (uint16_t)height,
				oldx - (int)newx_local52, oldy - (int)newy_local53)) {
				error_reply(c, 11, w->id, op);

				/* Reports successful completion. */
				return 0;
			}
			w->x = (int16_t)newx_local52;
			w->y = (int16_t)newy_local53;
			w->width = (uint16_t)width;
			w->height = (uint16_t)height;
			mark_dirty(s, oldx, oldy, oldw, oldh);
			mark_dirty(s, w->x, w->y, w->width, w->height);

			/* Reports successful completion. */
			return 0;
		}
		break;
	case 127:
		/* Reports successful completion. */
		return 0;
	default:
		error_reply(c, 1, 0, op);

		/* Reports successful completion. */
		return 0;
	}
	error_reply(c, 3, n >= 8 ? rd32(q + 4, c->order) : 0, op);

	/* Reports successful completion. */
	return 0;
}

/* Supports the rd32 operation. */
static uint32_t
rd32(
	const uint8_t *p,
	int msb)
{
	/* Returns the computed result. */
	return msb ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
			 ((uint32_t)p[2] << 8) | p[3]
		   : (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
			 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Supports the error reply operation. */
static void
error_reply(
	struct client *c,
	uint8_t code,
	uint32_t resource,
	uint8_t opcode)
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

/* Supports the map request operation. */
static void
map_request(
	struct server *s,
	struct window *parent,
	struct window *w)
{
	struct client *c;
	uint8_t e[32];

	c = owner_client(s, parent->owner);

	/* Classifies the current input character. */
	if (!c)
		return;
	memset(e, 0, sizeof(e));
	e[0] = 20;
	wr16(e + 2, c->sequence, c->order);
	wr32(e + 4, parent->id, c->order);
	wr32(e + 8, w->id, c->order);
	(void)write_all(c->fd, e, sizeof(e));
}

/* Supports the expose operation. */
static void
expose(
	struct server *s,
	struct window *w)
{
	expose_area(s, w, 0, 0, w->width, w->height);
	configure_notify(s, w);
}

/* Supports the expose area operation. */
static void
expose_area(
	struct server *s,
	struct window *w,
	int x,
	int y,
	int width,
	int height)
{
	struct client *c;
	uint8_t e[32];

	c = owner_client(s, w->owner);

	/* Classifies the current input character. */
	if (!c || !(w->event_mask & (1U << 15)))
		return;

	/* Checks the current horizontal value. */
	if (x < 0) {
		width += x;
		x = 0;
	}

	/* Handles the y condition. */
	if (y < 0) {
		height += y;
		y = 0;
	}

	/* Checks the current horizontal value. */
	if (x + width > w->width)
		width = w->width - x;

	/* Handles the y condition. */
	if (y + height > w->height)
		height = w->height - y;

	/* Handles the width condition. */
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

/* Supports the configure notify operation. */
static void
configure_notify(
	struct server *s,
	struct window *w)
{
	struct client *c;
	uint8_t e[32];

	c = owner_client(s, w->owner);

	/* Classifies the current input character. */
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

/* Supports the window pixels resize operation. */
static int
window_pixels_resize(
	struct window *w,
	uint16_t width,
	uint16_t height)
{
	uint32_t *p;
	unsigned copy_width, copy_height, row;

	/* Handles the width condition. */
	if (width == w->width && height == w->height)
		return 0;
	p = window_pixels_alloc(width, height, w->background);

	/* Checks the current pointer. */
	if (!p)
		return -1;

	/* Process each element required by the operation. */
	copy_width = width < w->width ? width : w->width;
	copy_height = height < w->height ? height : w->height;
	for (row = 0; row < copy_height; row++) {
		memcpy(p + (size_t)row * width,
		       w->pixels + (size_t)row * w->width,
		       (size_t)copy_width * sizeof(*p));
	}
	free(w->pixels);
	w->pixels = p;

	/* Reports successful completion. */
	return 0;
}

/* Supports the raise window operation. */
static void
raise_window(
	struct server *s,
	struct window *w)
{
	struct window saved;
	size_t index;

	/* Handles the w condition. */
	if (!w || w == &s->windows[0])
		return;
	index = (size_t)(w - s->windows);

	/* Checks the current index. */
	if (index >= s->window_count || index + 1U == s->window_count)
		return;
	mark_dirty(s, w->x, w->y, w->width, w->height);
	saved = *w;
	memmove(&s->windows[index], &s->windows[index + 1],
		(s->window_count - index - 1U) * sizeof(s->windows[0]));
	s->windows[s->window_count - 1U] = saved;
	mark_dirty(s, saved.x, saved.y, saved.width, saved.height);
}

/* Supports the top level window operation. */
static struct window *
top_level_window(
	struct server *s,
	struct window *w)
{
	struct window *p;

	/* Continue while the operation condition remains true. */
	while (w && w->parent != ROOT_XID) {
		p = find_window(s, w->parent);

		/* Checks the current pointer. */
		if (!p || p == w)
			break;
		w = p;
	}

	/* Returns the computed result. */
	return w;
}

/* Supports the simple reply operation. */
static void
simple_reply(
	struct client *c,
	uint8_t *r,
	size_t n)
{
	r[0] = 1;
	wr16(r + 2, c->sequence, c->order);

	/* Checks the current item count. */
	if (n >= 32)
		wr32(r + 4, (uint32_t)((n - 32) / 4), c->order);
	(void)write_all(c->fd, r, n);
}

/* Supports the hit operation. */
static struct window *
hit(
	struct server *s,
	int x,
	int y)
{
	struct window *function_result;

	/* Obtains the input at result. */
	function_result = input_at(s, x, y);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the find pixmap operation. */
static struct pixmap *
find_pixmap(
	struct server *s,
	uint32_t id)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < s->pixmap_count; i++) {
		/* Checks the current string state. */
		if (s->pixmaps[i].id == id)
			return &s->pixmaps[i];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the find gc operation. */
static struct graphics_context *
find_gc(
	struct server *s,
	uint32_t id)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < s->gc_count; i++) {
		/* Checks the current string state. */
		if (s->gcs[i].id == id)
			return &s->gcs[i];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the window fill operation. */
static void
window_fill(
	struct server *s,
	struct window *w,
	int x,
	int y,
	int width,
	int height,
	uint32_t color)
{
	int row, column;

	/* Checks the current horizontal value. */
	if (x < 0) {
		width += x;
		x = 0;
	}

	/* Handles the y condition. */
	if (y < 0) {
		height += y;
		y = 0;
	}

	/* Checks the current horizontal value. */
	if (x + width > w->width)
		width = w->width - x;

	/* Handles the y condition. */
	if (y + height > w->height)
		height = w->height - y;

	/* Handles the width condition. */
	if (width <= 0 || height <= 0)
		return;

	/* Process each element required by the operation. */
	for (row = y; row < y + height; row++) {
		/* Process each element required by the operation. */
		for (column = x; column < x + width; column++)
			w->pixels[(size_t)row * w->width + column] = color;
	}
	mark_dirty(s, w->x + x, w->y + y, width, height);
}

/* Supports the pixmap fill operation. */
static void
pixmap_fill(
	struct pixmap *p,
	int x,
	int y,
	int width,
	int height,
	uint32_t color)
{
	int row, column;

	/* Checks the current horizontal value. */
	if (x < 0) {
		width += x;
		x = 0;
	}

	/* Handles the y condition. */
	if (y < 0) {
		height += y;
		y = 0;
	}

	/* Checks the current horizontal value. */
	if (x + width > p->width)
		width = p->width - x;

	/* Handles the y condition. */
	if (y + height > p->height)
		height = p->height - y;

	/* Handles the width condition. */
	if (width <= 0 || height <= 0)
		return;

	/* Process each element required by the operation. */
	for (row = y; row < y + height; row++) {
		/* Process each element required by the operation. */
		for (column = x; column < x + width; column++)
			p->pixels[(size_t)row * p->width + column] = color;
	}
}

/* Supports the draw text operation. */
static int
draw_text(
	struct server *s,
	struct window *w,
	struct graphics_context *g,
	int x,
	int y,
	const uint8_t *text,
	size_t count,
	int wide)
{
	struct graphics_glyph q;
	uint32_t cp;
	int gx, gy, top;
	uint8_t bitmap[32];
	size_t i;
	uint32_t color;

	/* Process each remaining element. */
	color = g ? g->foreground : 0xffffff;
	for (i = 0; i < count; i++) {
		cp = wide ? ((uint32_t)text[i * 2] << 8) | text[i * 2 + 1]
	 : text[i];

		memset(&q, 0, sizeof(q));
		q.codepoint = cp;
		q.bitmap = (uapi_ptr_t)(uintptr_t)bitmap;
		q.bitmap_capacity = sizeof(bitmap);

		/* Handles a failed ioctl operation. */
		if (ioctl(s->graphics, ZEDBSD_GRAPHICS_GET_GLYPH, &q))
			continue;

		/* Process each element required by the operation. */
		top = y - (int)q.height;
		for (gy = 0; gy < (int)q.height; gy++) {
			/* Process each element required by the operation. */
			for (gx = 0; gx < (int)q.width; gx++) {
				/* Checks the current horizontal value. */
				if (x + gx >= 0 && x + gx < w->width &&
				    top + gy >= 0 && top + gy < w->height &&
				    (bitmap[(size_t)gy * q.stride +
					    (unsigned)gx / 8] &
				     (0x80U >> ((unsigned)gx & 7)))) {
					w->pixels[(size_t)(top + gy) *
						      w->width +
						  (x + gx)] = color;
				}
			}
		}
		mark_dirty(s, w->x + x, w->y + top, (int)q.width,
			   (int)q.height);
		x += (int)(q.advance ? q.advance : q.width);
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the window pixels resize buffered operation. */
static int
window_pixels_resize_buffered(
	struct window *w,
	uint16_t width,
	uint16_t height,
	int x_offset,
	int y_offset)
{
	uint32_t *p;
	int source_x, source_y, dest_x, dest_y;
	int copy_width, copy_height, row;

	source_x = 0;
	source_y = 0;
	dest_x = x_offset;
	dest_y = y_offset;
	copy_width = w->width;
	copy_height = w->height;

	/* Handles the width condition. */
	if (width == w->width && height == w->height && x_offset == 0 &&
	    y_offset == 0)

		/* Reports successful completion. */
		return 0;
	p = window_pixels_alloc(width, height, w->background);

	/* Checks the current pointer. */
	if (!p)
		return -1;

	/* Handles the dest x condition. */
	if (dest_x < 0) {
		source_x = -dest_x;
		copy_width -= source_x;
		dest_x = 0;
	}

	/* Handles the dest y condition. */
	if (dest_y < 0) {
		source_y = -dest_y;
		copy_height -= source_y;
		dest_y = 0;
	}

	/* Handles the dest x condition. */
	if (dest_x + copy_width > (int)width)
		copy_width = (int)width - dest_x;

	/* Handles the dest y condition. */
	if (dest_y + copy_height > (int)height)
		copy_height = (int)height - dest_y;

	/* Handles the copy width condition. */
	if (copy_width > 0 && copy_height > 0) {
		/* Process each element required by the operation. */
		for (row = 0; row < copy_height; row++) {
			memcpy(p + (size_t)(dest_y + row) * width +
				   (unsigned)dest_x,
			       w->pixels + (size_t)(source_y + row) * w->width +
				   (unsigned)source_x,
			       (size_t)copy_width * sizeof(*p));
		}
	}
	free(w->pixels);
	w->pixels = p;

	/* Reports successful completion. */
	return 0;
}

/* Supports the on signal operation. */
static void
on_signal(
	int sig)
{
	(void)sig;
	stopped = 1;
}

/* Capability-discovered evdev records are normalized by input.c. */
static void
input_key(
	void *context,
	uint8_t keycode,
	int value,
	uint32_t time,
	uint16_t state)
{
	struct server *s;
	struct window *w;
	struct client *c;

	s = context;
	w = find_window(s, s->focus);

	/* Handles the w condition. */
	if (!w)
		w = hit(s, s->pointer_x, s->pointer_y);
	c = owner_client(s, w->owner);
	s->key_state = state;

	/* Classifies the current input character. */
	if (!c || keycode == 0)
		return;

	/* Validates the current value. */
	if (value == 1) {
		send_event(c, 2, w->id, keycode, time, s->pointer_x,
		    s->pointer_y, state);
	} else if (value == 0) {
		send_event(c, 3, w->id, keycode, time, s->pointer_x,
		    s->pointer_y, state);
	} else if (value == 2) {
		send_event(c, 3, w->id, keycode, time, s->pointer_x,
		    s->pointer_y, state);
		send_event(c, 2, w->id, keycode, time, s->pointer_x,
		    s->pointer_y, state);
	}
}

/* Supports the input pointer operation. */
static void
input_pointer(
	void *context,
	const struct xzed_input_pointer_frame *frame)
{
	struct window *top;
	const struct xzed_input_button_edge *edge;
	uint32_t window_id;
	struct server *s;
	struct window *w;
	struct client *c;
	int moved = frame->absolute || frame->relative_x != 0 ||
	    frame->relative_y != 0;
	size_t index;

	s = context;
	s->buttons = frame->buttons_before;

	/* Handles the frame condition. */
	if (frame->edge_count != 0)
		flush_pointer_motion(s);

	/* Handles the moved condition. */
	if (moved && !s->pointer_dirty) {
		s->pointer_dirty = 1;
		s->pointer_old_x = s->pointer_x;
		s->pointer_old_y = s->pointer_y;
	}

	/* Handles the frame condition. */
	if (frame->absolute) {
		s->pointer_x = frame->absolute_x;
		s->pointer_y = frame->absolute_y;
	}

	/* Handles the frame condition. */
	if (frame->relative_x != 0 || frame->relative_y != 0) {
		xzed_pointer_move(&s->pointer_x, &s->pointer_y,
		    frame->relative_x, frame->relative_y, s->mode.width,
		    s->mode.height);
	}
	w = s->pointer_grab_owner >= 0
		? find_window(s, s->pointer_grab_window)
		: hit(s, s->pointer_x, s->pointer_y);

	/* Handles the w condition. */
	if (!w)
		w = hit(s, s->pointer_x, s->pointer_y);
	c = s->pointer_grab_owner >= 0
		? owner_client(s, (uint32_t)s->pointer_grab_owner)
		: owner_client(s, w->owner);

	/* Handles the moved condition. */
	if (moved && frame->edge_count != 0) {
		send_motion_event(c, w, (uint64_t)frame->time * 1000000U,
		    s->pointer_x, s->pointer_y, s->buttons);
	} else if (moved) {
		s->pending_motion_client = c;
		s->pending_motion_window = w;
		s->pending_motion_time = frame->time;
		s->pending_motion_x = s->pointer_x;
		s->pending_motion_y = s->pointer_y;
		s->pending_motion_buttons = s->buttons;
		s->pending_motion = 1;
	}

	/* Process each remaining element. */
	for (index = 0; index < frame->edge_count; index++) {
		edge = &frame->edges[index];
		window_id = w->id;

		/* Classifies the current input character. */
		if (c) {
			send_event(c, edge->pressed ? 4 : 5, window_id,
			    edge->button, frame->time, s->pointer_x, s->pointer_y,
			    s->buttons);
		}
		s->buttons = edge->buttons;

		/* Handles the edge condition. */
		if (edge->pressed) {
			top = top_level_window(s, w);

			/*
 * The desktop panel remains clickable without taking the
			 * application's focus used by toggle-minimize semantics. */
			if (top == NULL || strcmp(top->name, "_XZED_SHELL") != 0) {
				s->focus = window_id;
				raise_window(s, top);
			}
			w = find_window(s, window_id);

			/* Checks the current string state. */
			if (s->pointer_grab_owner < 0 && c) {
				s->pointer_grab_owner = w->owner;
				s->pointer_grab_window = w->id;
			}
		}
	}

	/* Checks the current string state. */
	if (!s->buttons) {
		s->pointer_grab_owner = -1;
		s->pointer_grab_window = 0;
	}
}
