/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zwm - minimal reparenting window manager.
 */

#include <X11/Xlib.h>
#include <X11/Xzed.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FRAMES 32
#define MAX_XPM_COLORS 64
/* Keep each PolyFillRectangle request within zedBSD's 2 KiB AF_UNIX packet. */
#define XPM_RECT_BATCH 128
#define FRAME_BORDER 2U
#define TITLE_HEIGHT 26U
#define CLIENT_X ((int)FRAME_BORDER)
#define CLIENT_Y ((int)TITLE_HEIGHT)
#define RESIZE_MARGIN 2
#define MIN_CLIENT_WIDTH 120U
#define MIN_CLIENT_HEIGHT 64U
#define EDGE_LEFT 1U
#define EDGE_RIGHT 2U
#define EDGE_TOP 4U
#define EDGE_BOTTOM 8U

/* Flat, dark decoration used by the zedBSD desktop. */
#define FRAME_FACE 0x222932UL
#define FRAME_SYMBOL 0xd3d9dfUL

struct frame {
	Window client;
	Window frame;
	GC gc;
	int x;
	int y;
	unsigned width;
	unsigned height;
	unsigned cursor_shape;
	char title[64];
};

struct xpm_color {
	char key;
	unsigned long pixel;
};

static struct frame frames[MAX_FRAMES];
static unsigned frame_count;
static Pixmap background_pixmap;
static GC background_gc;
static unsigned background_width;
static unsigned background_height;
static XFontStruct *title_font;

extern char **environ;

static void load_background(Display *display, Window root);
static int load_ppm(Display *display, Window root, const char *path);
static int read_entire_file(const char *path, char **result, size_t *result_size);
static int ppm_token(char **cursor, char *end, char *token, size_t capacity);
static int load_xpm(Display *display, Window root, const char *path);
static char *next_quoted(char **cursor, char *end);
static int xpm_header(char *s, unsigned *width, unsigned *height, unsigned *ncolors, unsigned *cpp);
static void manage(Display *display, Window root, Window client);
static struct frame *by_client(Window window);
static struct frame *by_frame(Window window);
static void activate(Display *display, struct frame *frame);
static unsigned frame_width(const struct frame *frame);
static unsigned frame_height(const struct frame *frame);
static void decorate(Display *display, struct frame *frame);
static void unmanage(Display *display, struct frame *frame);
static void restore_background(Display *display, Window root, int x, int y, unsigned width, unsigned height);
static void minimize(Display *display, Window root, struct frame *frame);
static unsigned resize_edges_at(const struct frame *frame, int x, int y);
static void set_resize_cursor(Display *display, struct frame *frame, int x, int y);
static unsigned cursor_for_edges(unsigned edges);

/*
 * Runs the zwm command.
 */
int
main(
	int argc,
	char **argv)
{
	struct frame *frame_local;
	struct frame *frame_local1;
	struct frame *frame_local2;
	struct frame *frame_local3;
	pid_t pid;
	int local_x;
	int local_y;
	unsigned fw;
	int dx;
	int dy;
	int x, y;
	int width, height;
	Display *display;
	Window root;
	XEvent event;
	struct frame *drag;
	unsigned drag_edges;
	int drag_start_x, drag_start_y;
	int drag_frame_x, drag_frame_y;
	unsigned drag_width, drag_height;

	display = XOpenDisplay(NULL);
	drag = NULL;
	drag_edges = 0;
	drag_start_x = 0;
	drag_start_y = 0;
	drag_frame_x = 0;
	drag_frame_y = 0;
	drag_width = 0;
	drag_height = 0;

	/* Handles the display availability. */
	if (display == NULL) {
		fprintf(stderr, "zwm: cannot open display\n");

		/* Reports operation failure. */
		return 1;
	}
	root = DefaultRootWindow(display);
	title_font = XLoadQueryFont(display, "zed-unicode");
	XSelectInput(display, root,
		     SubstructureRedirectMask | SubstructureNotifyMask |
			 ExposureMask);
	load_background(display, root);

	/* Validates the command-line arguments. */
	if (argc > 1) {
		pid = fork();

		/* Handles the pid condition. */
		if (pid == 0) {
			execve(argv[1], argv + 1, environ);
			_exit(127);
		}
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed XNextEvent operation. */
		if (XNextEvent(display, &event) < 0)
			break;

		/* Handles the event condition. */
		if (event.type == MapRequest) {
			manage(display, root, event.xmaprequest.window);
		} else if (event.type == DestroyNotify) {
			frame_local = by_client(event.xany.window);

			/* Handles the frame local availability. */
			if (frame_local != NULL) {
				drag = NULL;
				unmanage(display, frame_local);
			}
		} else if (event.type == Expose) {
			frame_local1 = by_frame(event.xexpose.window);

			/* Handles the event condition. */
			if (event.xexpose.window == root) {
				restore_background(
				    display, root, event.xexpose.x,
				    event.xexpose.y,
				    (unsigned)event.xexpose.width,
				    (unsigned)event.xexpose.height);
			} else if (frame_local1 != NULL) {
				decorate(display, frame_local1);
			}
		} else if (event.type == ButtonPress) {
			frame_local2 = by_frame(event.xbutton.window);

			/* Handles the frame local2 availability. */
			if (frame_local2 != NULL && event.xbutton.keycode == 1) {
				local_x = event.xbutton.x_root - frame_local2->x;
				local_y = event.xbutton.y_root - frame_local2->y;
				fw = frame_width(frame_local2);

				activate(display, frame_local2);

				/* Handles the local y condition. */
				if (local_y >= 2 &&
				    local_y < (int)TITLE_HEIGHT &&
				    local_x >= (int)fw - 84 &&
				    local_x < (int)fw - 60) {
					minimize(display, root, frame_local2);
					continue;
				}
				drag = frame_local2;
				drag_start_x = event.xbutton.x_root;
				drag_start_y = event.xbutton.y_root;
				drag_frame_x = frame_local2->x;
				drag_frame_y = frame_local2->y;
				drag_width = frame_local2->width;
				drag_height = frame_local2->height;
				drag_edges =
				    resize_edges_at(frame_local2, local_x, local_y);
			}
		} else if (event.type == MotionNotify && drag == NULL) {
			frame_local3 = by_frame(event.xmotion.window);

			/* Handles the frame local3 availability. */
			if (frame_local3 != NULL) {
				set_resize_cursor(
				    display, frame_local3,
				    event.xmotion.x_root - frame_local3->x,
				    event.xmotion.y_root - frame_local3->y);
			}
		} else if (event.type == MotionNotify) {
			dx = event.xmotion.x_root - drag_start_x;
			dy = event.xmotion.y_root - drag_start_y;
			x = drag_frame_x;
			y = drag_frame_y;
			width = (int)drag_width;
			height = (int)drag_height;

			/* Handles the drag edges condition. */
			if (drag_edges == 0) {
				x += dx;
				y += dy;
			} else {
				/* Handles the drag edges condition. */
				if (drag_edges & EDGE_LEFT) {
					x += dx;
					width -= dx;
				}

				/* Handles the drag edges condition. */
				if (drag_edges & EDGE_RIGHT)
					width += dx;

				/* Handles the drag edges condition. */
				if (drag_edges & EDGE_TOP) {
					y += dy;
					height -= dy;
				}

				/* Handles the drag edges condition. */
				if (drag_edges & EDGE_BOTTOM)
					height += dy;

				/* Handles the width condition. */
				if (width < (int)MIN_CLIENT_WIDTH) {
					/* Handles the drag edges condition. */
					if (drag_edges & EDGE_LEFT) {
						x -= (int)MIN_CLIENT_WIDTH -
						     width;
					}
					width = MIN_CLIENT_WIDTH;
				}

				/* Handles the height condition. */
				if (height < (int)MIN_CLIENT_HEIGHT) {
					/* Handles the drag edges condition. */
					if (drag_edges & EDGE_TOP) {
						y -= (int)MIN_CLIENT_HEIGHT -
						     height;
					}
					height = MIN_CLIENT_HEIGHT;
				}
			}

			/* Checks the current horizontal value. */
			if (x == drag->x && y == drag->y &&
			    width == (int)drag->width &&
			    height == (int)drag->height)
				continue;
			drag->x = x;
			drag->y = y;
			drag->width = (unsigned)width;
			drag->height = (unsigned)height;

			/* Handles the drag edges condition. */
			if (drag_edges == 0) {
				XMoveResizeWindow(display, drag->frame, drag->x,
						  drag->y, frame_width(drag),
						  frame_height(drag));
			} else {
				XzedMoveResizeWindowBuffered(
				    display, drag->frame, drag->x, drag->y,
				    frame_width(drag), frame_height(drag));
			}
		} else if (event.type == ButtonRelease) {
			/* Handles the drag availability. */
			if (drag != NULL) {
				/* Handles the drag edges condition. */
				if (drag_edges != 0) {
					XMoveResizeWindow(display, drag->client,
							  drag->x + CLIENT_X,
							  drag->y + CLIENT_Y,
							  drag->width,
							  drag->height);
				}
				decorate(display, drag);
				set_resize_cursor(
				    display, drag,
				    event.xbutton.x_root - drag->x,
				    event.xbutton.y_root - drag->y);
			}
			drag = NULL;
		}
	}

	XCloseDisplay(display);

	/* Reports successful completion. */
	return 0;
}

/* Supports the load background operation. */
static void
load_background(
	Display *display,
	Window root)
{
	char *p;
	char *destination;
	size_t capacity;
	size_t length;
	FILE *file;
	char line[512];
	char directory[320] = "/usr/share/zwm/backgrounds";
	char fallback[400] = {0};
	char path[400];
	unsigned width;
	unsigned height;
	unsigned border;
	unsigned depth;
	int x;
	int y;
	int path_length;
	Window root_return;

	/* Process input until it is exhausted. */
	file = fopen("/etc/Xzed/zwm.conf", "r");
	while (file != NULL && fgets(line, sizeof(line), file) != NULL) {
		p = line;

		/* Continue while the operation condition remains true. */
		while (*p == ' ' || *p == '\t')
			p++;

		/* Selects the matching prefix. */
		if (strncmp(p, "background_dir=", 15) == 0) {
			destination = directory;
			capacity = sizeof(directory);
			p += 15;
		} else if (strncmp(p, "background=", 11) == 0) {
			destination = fallback;
			capacity = sizeof(fallback);
			p += 11;
		} else {
			continue;
		}
		strncpy(destination, p, capacity - 1U);

		/* Process each remaining element. */
		destination[capacity - 1U] = '\0';
		length = strlen(destination);
		while (length != 0 && (destination[length - 1] == '\n' ||
				       destination[length - 1] == '\r' ||
				       destination[length - 1] == ' ' ||
				       destination[length - 1] == '\t'))
			destination[--length] = '\0';
	}

	/* Handles the file availability. */
	if (file != NULL)
		fclose(file);

	/* Handles a failed XGetGeometry operation. */
	if (!XGetGeometry(display, root, &root_return, &x, &y, &width, &height,
			  &border, &depth))

		/* Returns the computed result. */
		return;
	path_length = snprintf(path, sizeof(path), "%s/%ux%u.ppm", directory,
			       width, height);

	/* Handles the path length condition. */
	if (path_length < 0 || (size_t)path_length >= sizeof(path))
		return;

	/* Handles the load ppm condition. */
	if (load_ppm(display, root, path))
		return;

	/* Handles a failed load xpm operation. */
	if (fallback[0] != '\0' && load_xpm(display, root, fallback))
		return;
	fprintf(stderr, "zwm: cannot load %ux%u background from %s\n", width,
		height, directory);
}

/* Supports the load ppm operation. */
static int
load_ppm(
	Display *display,
	Window root,
	const char *path)
{
	char *file;
	char *cursor;
	char *end;
	char token[32];
	size_t file_size;
	size_t pixel_bytes;
	unsigned width;
	unsigned height;
	unsigned root_width;
	unsigned root_height;
	unsigned border;
	unsigned depth;
	int root_x;
	int root_y;
	Window root_return;

	Pixmap pixmap;
	GC gc;
	int ok;

	file = NULL;
	pixmap = 0;
	gc = NULL;
	ok = 0;

	/* Handles a failed read entire file operation. */
	if (!read_entire_file(path, &file, &file_size))
		goto out;
	cursor = file;
	end = file + file_size;

	/* Handles a failed ppm token operation. */
	if (!ppm_token(&cursor, end, token, sizeof(token)) ||
	    strcmp(token, "P6") != 0 ||
	    !ppm_token(&cursor, end, token, sizeof(token)))
		goto out;
	width = (unsigned)strtoul(token, NULL, 10);

	/* Handles a failed ppm token operation. */
	if (!ppm_token(&cursor, end, token, sizeof(token)))
		goto out;
	height = (unsigned)strtoul(token, NULL, 10);

	/* Handles a failed ppm token operation. */
	if (!ppm_token(&cursor, end, token, sizeof(token)) ||
	    strtoul(token, NULL, 10) != 255UL || width == 0 || height == 0 ||
	    width > 65535U || height > 65535U)
		goto out;

	/* Checks the current cursor position. */
	if (cursor >= end || (*cursor != ' ' && *cursor != '\t' &&
			      *cursor != '\r' && *cursor != '\n'))
		goto out;

	/* Checks the current cursor position. */
	if (*cursor++ == '\r' && cursor < end && *cursor == '\n')
		cursor++;

	/* Handles the width condition. */
	if ((size_t)width > SIZE_MAX / (size_t)height ||
	    (size_t)width * height > SIZE_MAX / 3U)
		goto out;
	pixel_bytes = (size_t)width * height * 3U;

	/* Checks the current endpoint. */
	if ((size_t)(end - cursor) < pixel_bytes)
		goto out;

	/* Handles a failed XGetGeometry operation. */
	if (!XGetGeometry(display, root, &root_return, &root_x, &root_y,
			  &root_width, &root_height, &border, &depth) ||
	    width != root_width || height != root_height)
		goto out;
	pixmap = XCreatePixmap(display, root, width, height, depth);

	/* Handles the pixmap condition. */
	if (pixmap == 0)
		goto out;
	gc = XCreateGC(display, pixmap, 0, NULL);

	/* Handles a failed XzedPutImageRGB24 operation. */
	if (gc == NULL ||
	    XzedPutImageRGB24(display, pixmap, 0, 0, width, height,
			      (const unsigned char *)cursor, width * 3U) != 0 ||
	    XSync(display, False) != 0)
		goto out;
	XCopyArea(display, pixmap, root, gc, 0, 0, width, height, 0, 0);
	XSync(display, False);

	/* Handles the background gc availability. */
	if (background_gc != NULL)
		XFreeGC(display, background_gc);

	/* Handles the background pixmap condition. */
	if (background_pixmap != 0)
		XFreePixmap(display, background_pixmap);
	background_pixmap = pixmap;
	background_gc = gc;
	background_width = width;
	background_height = height;
	pixmap = 0;
	gc = NULL;
	ok = 1;

out:

	/* Handles the gc availability. */
	if (gc != NULL)
		XFreeGC(display, gc);

	/* Handles the pixmap condition. */
	if (pixmap != 0)
		XFreePixmap(display, pixmap);
	free(file);

	/* Returns the computed result. */
	return ok;
}

/* Supports the read entire file operation. */
static int
read_entire_file(
	const char *path,
	char **result,
	size_t *result_size)
{
	ssize_t nread;
	struct stat st;
	char *data;
	size_t got;
	int fd;

	got = 0;

	fd = open(path, O_RDONLY);

	/* Checks the file descriptor. */
	if (fd < 0)
		return 0;

	/* Handles a failed fstat operation. */
	if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
	    (uint64_t)st.st_size > (uint64_t)SIZE_MAX - 1) {
		close(fd);

		/* Reports successful completion. */
		return 0;
	}
	data = malloc((size_t)st.st_size + 1);

	/* Handles the data availability. */
	if (data == NULL) {
		close(fd);

		/* Reports successful completion. */
		return 0;
	}
	while (got < (size_t)st.st_size) {
		nread = read(fd, data + got, (size_t)st.st_size - got);

		/* Handles the reported system error. */
		if (nread < 0 && errno == EINTR)
			continue;

		/* Handles the nread condition. */
		if (nread <= 0) {
			free(data);
			close(fd);

			/* Reports successful completion. */
			return 0;
		}
		got += (size_t)nread;
	}
	close(fd);
	data[got] = '\0';
	*result = data;
	*result_size = got;
	/* Reports operation failure. */
	return 1;
}

/* Supports the ppm token operation. */
static int
ppm_token(
	char **cursor,
	char *end,
	char *token,
	size_t capacity)
{
	char *p;
	size_t length;

	p = *cursor;
	length = 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Continue while the operation condition remains true. */
		while (p < end &&
		       (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
			p++;

		/* Checks the current pointer. */
		if (p >= end || *p != '#')
			break;

		/* Continue while the operation condition remains true. */
		while (p < end && *p != '\n')
			p++;
	}
	while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
	       *p != '#') {
		/* Checks the current data length. */
		if (length + 1U >= capacity)
			return 0;
		token[length++] = *p++;
	}

	/* Checks the current data length. */
	if (length == 0)
		return 0;
	token[length] = '\0';
	*cursor = p;
	/* Reports operation failure. */
	return 1;
}

/* Supports the load xpm operation. */
static int
load_xpm(
	Display *display,
	Window root,
	const char *path)
{
	char *hash;
	unsigned start;
	unsigned x;
	int count;
	struct xpm_color colors[MAX_XPM_COLORS];

	XRectangle *rectangles = NULL;
	char *file = NULL;
	char *cursor;
	char *end;
	char *quoted;
	char *pixels = NULL;
	size_t file_size;
	size_t pixel_count;
	unsigned width = 0;
	unsigned height = 0;
	unsigned ncolors = 0;
	unsigned cpp = 0;
	unsigned root_width = 0;
	unsigned root_height = 0;
	unsigned border = 0;
	unsigned depth = 0;
	unsigned i;
	unsigned y;
	int root_x = 0;
	int root_y = 0;
	Window root_return;
	Pixmap pixmap = 0;
	GC gc = 0;
	int ok = 0;

	/* Handles a failed read entire file operation. */
	if (!read_entire_file(path, &file, &file_size))
		goto out;
	cursor = file;
	end = file + file_size;
	quoted = next_quoted(&cursor, end);

	/* Handles a failed xpm header operation. */
	if (quoted == NULL ||
	    !xpm_header(quoted, &width, &height, &ncolors, &cpp) ||
	    width == 0 || height == 0 || ncolors == 0 ||
	    ncolors > MAX_XPM_COLORS || cpp != 1)
		goto out;

	/* Process each element required by the operation. */
	for (i = 0; i < ncolors; i++) {
		quoted = next_quoted(&cursor, end);

		/* Handles the quoted availability. */
		if (quoted == NULL)
			goto out;
		colors[i].key = quoted[0];
		hash = strchr(quoted, '#');
		colors[i].pixel =
		    hash != NULL ? strtoul(hash + 1, NULL, 16) : 0;
	}

	/* Handles the width condition. */
	if ((size_t)width > SIZE_MAX / (size_t)height)
		goto out;
	pixel_count = (size_t)width * (size_t)height;
	pixels = malloc(pixel_count);
	rectangles = malloc(XPM_RECT_BATCH * sizeof(*rectangles));

	/* Handles the pixels availability. */
	if (pixels == NULL || rectangles == NULL)
		goto out;

	/* Process each element required by the operation. */
	for (y = 0; y < height; y++) {
		quoted = next_quoted(&cursor, end);

		/* Handles a failed strlen operation. */
		if (quoted == NULL || strlen(quoted) < width)
			goto out;
		memcpy(pixels + (size_t)y * width, quoted, width);
	}

	/* Handles a failed XGetGeometry operation. */
	if (!XGetGeometry(display, root, &root_return, &root_x, &root_y,
			  &root_width, &root_height, &border, &depth) ||
	    width != root_width || height != root_height)
		goto out;

	pixmap = XCreatePixmap(display, root, width, height, depth);

	/* Handles the pixmap condition. */
	if (pixmap == 0)
		goto out;
	gc = XCreateGC(display, pixmap, 0, NULL);

	/* Handles the gc availability. */
	if (gc == NULL)
		goto out;

	/*
	 * Build the complete background off screen.  Drawing these runs must
	 * not expose partially rendered color planes on /dev/graphics.
	 */
	/* Process each element required by the operation. */
	for (i = 0; i < ncolors; i++) {
		count = 0;

		XSetForeground(display, gc, colors[i].pixel);

		/* Process each element required by the operation. */
		for (y = 0; y < height; y++) {
			x = 0;

			/* Continue while the operation condition remains true. */
			while (x < width) {
				/* Handles the pixels condition. */
				if (pixels[(size_t)y * width + x] !=
				    colors[i].key) {
					x++;
					continue;
				}

				/* Process each remaining element. */
				start = x;
				while (x < width &&
				       pixels[(size_t)y * width + x] ==
					   colors[i].key)
					x++;
				rectangles[count++] = (XRectangle){
				    (short)start, (short)y,
				    (unsigned short)(x - start), 1};

				/* Checks the remaining item count. */
				if (count == XPM_RECT_BATCH) {
					/* Handles a failed XFillRectangles operation. */
					if (XFillRectangles(display, pixmap, gc,
							    rectangles,
							    count) != 0)
						goto out;

					/* Handles a failed XSync operation. */
					if (XSync(display, False) != 0)
						goto out;
					count = 0;
				}
			}
		}

		/* Handles a failed XFillRectangles operation. */
		if (count != 0 && XFillRectangles(display, pixmap, gc,
						  rectangles, count) != 0)
			goto out;

		/* Handles a failed XSync operation. */
		if (count != 0 && XSync(display, False) != 0)
			goto out;
	}

	/* One CopyArea is the only operation that changes the visible root. */
	XCopyArea(display, pixmap, root, gc, 0, 0, width, height, 0, 0);
	XFlush(display);

	/* Handles the background gc availability. */
	if (background_gc != NULL)
		XFreeGC(display, background_gc);

	/* Handles the background pixmap condition. */
	if (background_pixmap != 0)
		XFreePixmap(display, background_pixmap);
	background_pixmap = pixmap;
	background_gc = gc;
	background_width = width;
	background_height = height;
	pixmap = 0;
	gc = NULL;
	ok = 1;

out:

	/* Handles the gc availability. */
	if (gc != NULL)
		XFreeGC(display, gc);

	/* Handles the pixmap condition. */
	if (pixmap != 0)
		XFreePixmap(display, pixmap);
	free(rectangles);
	free(pixels);
	free(file);

	/* Returns the computed result. */
	return ok;
}

/* Supports the next quoted operation. */
static char *
next_quoted(
	char **cursor,
	char *end)
{
	char *a;
	char *b;

	a = *cursor;

	/* Continue while the operation condition remains true. */
	while (a < end && *a != '"')
		a++;

	/* Handles the a condition. */
	if (a == end)
		return NULL;

	/* Continue while the operation condition remains true. */
	b = ++a;
	while (b < end && *b != '"')
		b++;

	/* Handles the b condition. */
	if (b == end)
		return NULL;
	*b = '\0';
	*cursor = b + 1;
	/* Returns the computed result. */
	return a;
}

/* Supports the xpm header operation. */
static int
xpm_header(
	char *s,
	unsigned *width,
	unsigned *height,
	unsigned *ncolors,
	unsigned *cpp)
{
	unsigned long value;
	unsigned *values[4] = {width, height, ncolors, cpp};
	unsigned i;
	char *end;

	/* Process each element required by the operation. */
	for (i = 0; i < 4; i++) {
		/* Continue while the operation condition remains true. */
		while (*s == ' ' || *s == '\t')
			s++;
		value = strtoul(s, &end, 10);

		/* Checks the current endpoint. */
		if (end == s)
			return 0;
		*values[i] = (unsigned)value;
		s = end;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the manage operation. */
static void
manage(
	Display *display,
	Window root,
	Window client)
{
	Window root_return;
	int x;
	int y;
	unsigned width;
	unsigned height;
	unsigned border;
	unsigned depth;
	struct frame *frame;
	char *title;
	int reparent_requested;

	title = NULL;
	reparent_requested = 0;

	frame = by_client(client);

	/* Handles the frame availability. */
	if (frame == NULL)
		frame = by_frame(client);

	/* Handles the frame availability. */
	if (frame != NULL) {
		activate(display, frame);

		/* Returns the computed result. */
		return;
	}

	/* Handles a failed XGetGeometry operation. */
	if (frame_count == MAX_FRAMES ||
	    !XGetGeometry(display, client, &root_return, &x, &y, &width,
			  &height, &border, &depth))

		/* Returns the computed result. */
		return;
	(void)XFetchName(display, client, &title);

	/* Handles the title availability. */
	if (title != NULL && strcmp(title, "_XZED_SHELL") == 0) {
		XMapWindow(display, client);
		XFree(title);

		/* Returns the computed result. */
		return;
	}
	frame = &frames[frame_count];
	memset(frame, 0, sizeof(*frame));
	frame->client = client;
	frame->x = x;
	frame->y = y;
	frame->width = width;
	frame->height = height;

	/* Handles the title availability. */
	if (title != NULL && title[0] != '\0') {
		strncpy(frame->title, title, sizeof(frame->title) - 1U);
		frame->title[sizeof(frame->title) - 1U] = '\0';
	} else {
		strcpy(frame->title, "Xzed Application");
	}
	XFree(title);
	frame->frame =
	    XCreateSimpleWindow(display, root, x, y, frame_width(frame),
				frame_height(frame), 0, 0, FRAME_FACE);

	/* Handles the frame condition. */
	if (frame->frame == 0)
		goto fail;
	frame->cursor_shape = XZED_CURSOR_LEFT_PTR;

	/* Handles a failed XzedSetCursorShape operation. */
	if (XzedSetCursorShape(display, frame->frame, XZED_CURSOR_LEFT_PTR) !=
		0 ||
	    XzedSetInputMargins(display, frame->frame, RESIZE_MARGIN,
				RESIZE_MARGIN, RESIZE_MARGIN,
				RESIZE_MARGIN) != 0)
		goto fail;

	/* Do not inherit a resize cursor after crossing into the client. */
	if (XzedSetCursorShape(display, client, XZED_CURSOR_LEFT_PTR) != 0 ||
	    !XStoreName(display, frame->frame, frame->title))
		goto fail;
	frame->gc = XCreateGC(display, frame->frame, 0, NULL);

	/* Handles the gc availability. */
	if (frame->gc == NULL)
		goto fail;

	/* Handles the title font availability. */
	if (title_font != NULL) {
		/* Handles a failed XSetFont operation. */
		if (XSetFont(display, frame->gc, title_font->fid) != 0)
			goto fail;
	}

	/* Handles a failed XSelectInput operation. */
	if (XSelectInput(display, frame->frame,
			 ExposureMask | ButtonPressMask | ButtonReleaseMask |
			     PointerMotionMask | StructureNotifyMask) != 0)
		goto fail;

	/* Handles a failed XReparentWindow operation. */
	if (XReparentWindow(display, client, frame->frame, CLIENT_X,
			    CLIENT_Y) != 0)
		goto fail;
	reparent_requested = 1;

	/* Drain the seven setup requests before mapping and decorating. */
	if (XSync(display, False) != 0 || XMapWindow(display, client) != 0 ||
	    XMapWindow(display, frame->frame) != 0 ||
	    XSync(display, False) != 0)
		goto fail;
	frame_count++;
	decorate(display, frame);
	activate(display, frame);

	/* Returns the computed result. */
	return;

fail:
	fprintf(stderr, "zwm: cannot manage window 0x%lx: %s\n",
		(unsigned long)client, strerror(errno));

	/* Handles the reparent requested condition. */
	if (reparent_requested)
		(void)XReparentWindow(display, client, root, x, y);

	/* Handles the gc availability. */
	if (frame->gc != NULL)
		(void)XFreeGC(display, frame->gc);

	/* Handles the frame condition. */
	if (frame->frame != 0) {
		(void)XUnmapWindow(display, frame->frame);
		(void)XDestroyWindow(display, frame->frame);
	}
	(void)XSync(display, False);
	memset(frame, 0, sizeof(*frame));
}

/* Supports the by client operation. */
static struct frame *
by_client(
	Window window)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < frame_count; i++) {
		/* Handles the frames condition. */
		if (frames[i].client == window)
			return &frames[i];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the by frame operation. */
static struct frame *
by_frame(
	Window window)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < frame_count; i++) {
		/* Handles the frames condition. */
		if (frames[i].frame == window)
			return &frames[i];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the activate operation. */
static void
activate(
	Display *display,
	struct frame *frame)
{
	XMapWindow(display, frame->client);
	XMapWindow(display, frame->frame);
	XRaiseWindow(display, frame->frame);
	XSetInputFocus(display, frame->client, RevertToParent, CurrentTime);
}

/* Supports the frame width operation. */
static unsigned
frame_width(
	const struct frame *frame)
{
	/* Returns the computed result. */
	return frame->width + FRAME_BORDER * 2U;
}

/* Supports the frame height operation. */
static unsigned
frame_height(
	const struct frame *frame)
{
	/* Returns the computed result. */
	return frame->height + TITLE_HEIGHT + FRAME_BORDER;
}

/* Supports the decorate operation. */
static void
decorate(
	Display *display,
	struct frame *frame)
{
	unsigned fw;
	unsigned fh;
	size_t title_length;
	unsigned title_capacity;
	int minimize_x;
	int maximize_x;
	int close_x;
	int title_x;

	XRectangle symbols[24];
	int symbol_count;
	int offset;

	fw = frame_width(frame);
	fh = frame_height(frame);
	title_length = strlen(frame->title);
	title_capacity = fw > 176U ? (fw - 176U) / 8U : 0;
	minimize_x = (int)fw - 72;
	maximize_x = (int)fw - 44;
	close_x = (int)fw - 16;
	symbol_count = 0;

	/* Handles the fw condition. */
	if (fw < 112U || fh < TITLE_HEIGHT + FRAME_BORDER)
		return;

	/* Handles the title length condition. */
	if (title_length > title_capacity)
		title_length = title_capacity;

	/*
	 * The title bar itself is the top edge: do not surround it with a
	 * separate frame.  The exposed two-pixel strips beside and below the
	 * client use the exact same face color as the title bar.
	 */
	XSetForeground(display, frame->gc, FRAME_FACE);
	XFillRectangle(display, frame->frame, frame->gc, 0, 0, fw, fh);

	/* Flat monochrome controls: minimize, maximize, and close. */
	XSetForeground(display, frame->gc, FRAME_SYMBOL);
	symbols[symbol_count++] =
	    (XRectangle){(short)(minimize_x - 4), 15, 9, 1};
	symbols[symbol_count++] =
	    (XRectangle){(short)(maximize_x - 4), 9, 9, 1};
	symbols[symbol_count++] =
	    (XRectangle){(short)(maximize_x - 4), 9, 1, 9};
	symbols[symbol_count++] =
	    (XRectangle){(short)(maximize_x + 4), 9, 1, 9};
	symbols[symbol_count++] =
	    (XRectangle){(short)(maximize_x - 4), 17, 9, 1};

	/* Process each element required by the operation. */
	for (offset = -4; offset <= 4; offset++) {
		symbols[symbol_count++] = (XRectangle){
		    (short)(close_x + offset), (short)(13 + offset), 1, 1};
		symbols[symbol_count++] = (XRectangle){
		    (short)(close_x + offset), (short)(13 - offset), 1, 1};
	}
	XFillRectangles(display, frame->frame, frame->gc, symbols,
			symbol_count);

	/* Handles the title length condition. */
	if (title_length != 0) {
		title_x = ((int)fw - (int)title_length * 8) / 2;
		XSetForeground(display, frame->gc, WhitePixel(display, 0));
		XDrawString(display, frame->frame, frame->gc, title_x, 20,
			    frame->title, (int)title_length);
	}
	XSync(display, False);
}

/* Supports the unmanage operation. */
static void
unmanage(
	Display *display,
	struct frame *frame)
{
	size_t index;

	/* Handles the frame availability. */
	if (frame == NULL)
		return;
	index = (size_t)(frame - frames);

	/* Checks the current index. */
	if (index >= frame_count)
		return;

	/* Handles the gc availability. */
	if (frame->gc != NULL)
		(void)XFreeGC(display, frame->gc);

	/* Handles the frame condition. */
	if (frame->frame != 0)
		(void)XDestroyWindow(display, frame->frame);

	/* Checks the current index. */
	if (index + 1U < frame_count) {
		memmove(&frames[index], &frames[index + 1U],
			(frame_count - index - 1U) * sizeof(frames[0]));
	}
	frame_count--;
	memset(&frames[frame_count], 0, sizeof(frames[0]));
	(void)XSync(display, False);
}

/* Supports the restore background operation. */
static void
restore_background(
	Display *display,
	Window root,
	int x,
	int y,
	unsigned width,
	unsigned height)
{
	/* Handles the background gc availability. */
	if (background_pixmap == 0 || background_gc == NULL || x < 0 || y < 0 ||
	    (unsigned)x >= background_width || (unsigned)y >= background_height)

		/* Returns the computed result. */
		return;

	/* Handles the width condition. */
	if (width > background_width - (unsigned)x)
		width = background_width - (unsigned)x;

	/* Handles the height condition. */
	if (height > background_height - (unsigned)y)
		height = background_height - (unsigned)y;
	XCopyArea(display, background_pixmap, root, background_gc, x, y, width,
		  height, x, y);
}

/* Supports the minimize operation. */
static void
minimize(
	Display *display,
	Window root,
	struct frame *frame)
{
	XUnmapWindow(display, frame->frame);
	XSetInputFocus(display, root, RevertToParent, CurrentTime);
}

/* Supports the resize edges at operation. */
static unsigned
resize_edges_at(
	const struct frame *frame,
	int x,
	int y)
{
	unsigned edges;
	unsigned fw;
	unsigned fh;
	int left;
	int right;
	int top;
	int bottom;

	edges = 0;
	fw = frame_width(frame);
	fh = frame_height(frame);
	left = x >= -RESIZE_MARGIN && x < RESIZE_MARGIN;
	right = x >= (int)fw - RESIZE_MARGIN && x < (int)fw + RESIZE_MARGIN;
	top = y >= -RESIZE_MARGIN && y < RESIZE_MARGIN;
	bottom = y >= (int)fh - RESIZE_MARGIN && y < (int)fh + RESIZE_MARGIN;

	/* Handles the left condition. */
	if (left && y >= -RESIZE_MARGIN && y < (int)fh + RESIZE_MARGIN)
		edges |= EDGE_LEFT;

	/* Handles the right condition. */
	if (right && y >= -RESIZE_MARGIN && y < (int)fh + RESIZE_MARGIN)
		edges |= EDGE_RIGHT;

	/* Handles the top condition. */
	if (top && x >= -RESIZE_MARGIN && x < (int)fw + RESIZE_MARGIN)
		edges |= EDGE_TOP;

	/* Handles the bottom condition. */
	if (bottom && x >= -RESIZE_MARGIN && x < (int)fw + RESIZE_MARGIN)
		edges |= EDGE_BOTTOM;

	/* Returns the computed result. */
	return edges;
}

/* Supports the set resize cursor operation. */
static void
set_resize_cursor(
	Display *display,
	struct frame *frame,
	int x,
	int y)
{
	unsigned shape;

	shape = cursor_for_edges(resize_edges_at(frame, x, y));

	/* Handles the shape condition. */
	if (shape != frame->cursor_shape) {
		XzedSetCursorShape(display, frame->frame, shape);
		frame->cursor_shape = shape;
	}
}

/* Supports the cursor for edges operation. */
static unsigned
cursor_for_edges(
	unsigned edges)
{
	/* Handles the edges condition. */
	if ((edges & (EDGE_LEFT | EDGE_BOTTOM)) == (EDGE_LEFT | EDGE_BOTTOM))
		return XZED_CURSOR_BOTTOM_LEFT;

	/* Handles the edges condition. */
	if ((edges & (EDGE_RIGHT | EDGE_BOTTOM)) == (EDGE_RIGHT | EDGE_BOTTOM))
		return XZED_CURSOR_BOTTOM_RIGHT;

	/* Handles the edges condition. */
	if ((edges & (EDGE_LEFT | EDGE_TOP)) == (EDGE_LEFT | EDGE_TOP))
		return XZED_CURSOR_BOTTOM_RIGHT;

	/* Handles the edges condition. */
	if ((edges & (EDGE_RIGHT | EDGE_TOP)) == (EDGE_RIGHT | EDGE_TOP))
		return XZED_CURSOR_BOTTOM_LEFT;

	/* Handles the edges condition. */
	if (edges & (EDGE_LEFT | EDGE_RIGHT))
		return XZED_CURSOR_HORIZONTAL;

	/* Handles the edges condition. */
	if (edges & (EDGE_TOP | EDGE_BOTTOM))
		return XZED_CURSOR_VERTICAL;

	/* Returns the computed result. */
	return XZED_CURSOR_LEFT_PTR;
}
