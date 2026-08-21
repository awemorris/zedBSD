/* xzedwm - minimal reparenting window manager. SPDX-License-Identifier: Zlib */
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

/* Flat, dark decoration used by the zedBSD desktop. */
#define FRAME_FACE 0x202833UL
#define FRAME_HIGHLIGHT 0x394450UL
#define FRAME_EDGE 0x090d11UL
#define FRAME_SYMBOL 0xd3d9dfUL

struct frame {
	Window client;
	Window frame;
	GC gc;
	int x;
	int y;
	unsigned width;
	unsigned height;
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

static unsigned
frame_width(const struct frame *frame)
{
	return frame->width + FRAME_BORDER * 2U;
}

static unsigned
frame_height(const struct frame *frame)
{
	return frame->height + TITLE_HEIGHT + FRAME_BORDER;
}

static char *
next_quoted(char **cursor, char *end)
{
	char *a = *cursor;
	char *b;

	while (a < end && *a != '"')
		a++;
	if (a == end)
		return NULL;
	b = ++a;
	while (b < end && *b != '"')
		b++;
	if (b == end)
		return NULL;
	*b = '\0';
	*cursor = b + 1;
	return a;
}

static int
xpm_header(char *s, unsigned *width, unsigned *height, unsigned *ncolors,
    unsigned *cpp)
{
	unsigned *values[4] = { width, height, ncolors, cpp };
	unsigned i;
	char *end;

	for (i = 0; i < 4; i++) {
		unsigned long value;

		while (*s == ' ' || *s == '\t')
			s++;
		value = strtoul(s, &end, 10);
		if (end == s)
			return 0;
		*values[i] = (unsigned)value;
		s = end;
	}
	return 1;
}

static int
read_entire_file(const char *path, char **result, size_t *result_size)
{
	struct stat st;
	char *data;
	size_t got = 0;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
	    (uint64_t)st.st_size > (uint64_t)SIZE_MAX - 1) {
		close(fd);
		return 0;
	}
	data = malloc((size_t)st.st_size + 1);
	if (data == NULL) {
		close(fd);
		return 0;
	}
	while (got < (size_t)st.st_size) {
		ssize_t nread = read(fd, data + got, (size_t)st.st_size - got);

		if (nread < 0 && errno == EINTR)
			continue;
		if (nread <= 0) {
			free(data);
			close(fd);
			return 0;
		}
		got += (size_t)nread;
	}
	close(fd);
	data[got] = '\0';
	*result = data;
	*result_size = got;
	return 1;
}

static int
load_xpm(Display *display, Window root, const char *path)
{
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

	if (!read_entire_file(path, &file, &file_size))
		goto out;
	cursor = file;
	end = file + file_size;
	quoted = next_quoted(&cursor, end);
	if (quoted == NULL ||
	    !xpm_header(quoted, &width, &height, &ncolors, &cpp) ||
	    width == 0 || height == 0 || ncolors == 0 ||
	    ncolors > MAX_XPM_COLORS || cpp != 1)
		goto out;

	for (i = 0; i < ncolors; i++) {
		char *hash;

		quoted = next_quoted(&cursor, end);
		if (quoted == NULL)
			goto out;
		colors[i].key = quoted[0];
		hash = strchr(quoted, '#');
		colors[i].pixel = hash != NULL ? strtoul(hash + 1, NULL, 16) : 0;
	}

	if ((size_t)width > SIZE_MAX / (size_t)height)
		goto out;
	pixel_count = (size_t)width * (size_t)height;
	pixels = malloc(pixel_count);
	rectangles = malloc(XPM_RECT_BATCH * sizeof(*rectangles));
	if (pixels == NULL || rectangles == NULL)
		goto out;
	for (y = 0; y < height; y++) {
		quoted = next_quoted(&cursor, end);
		if (quoted == NULL || strlen(quoted) < width)
			goto out;
		memcpy(pixels + (size_t)y * width, quoted, width);
	}

	if (!XGetGeometry(display, root, &root_return, &root_x, &root_y,
	    &root_width, &root_height, &border, &depth) ||
	    width != root_width || height != root_height)
		goto out;

	pixmap = XCreatePixmap(display, root, width, height, depth);
	if (pixmap == 0)
		goto out;
	gc = XCreateGC(display, pixmap, 0, NULL);
	if (gc == NULL)
		goto out;

	/*
	 * Build the complete background off screen.  Drawing these runs must not
	 * expose partially rendered color planes on /dev/graphics.
	 */
	for (i = 0; i < ncolors; i++) {
		int count = 0;

		XSetForeground(display, gc, colors[i].pixel);
		for (y = 0; y < height; y++) {
			unsigned x = 0;

			while (x < width) {
				unsigned start;

				if (pixels[(size_t)y * width + x] != colors[i].key) {
					x++;
					continue;
				}
				start = x;
				while (x < width &&
				    pixels[(size_t)y * width + x] == colors[i].key)
					x++;
				rectangles[count++] = (XRectangle) {
					(short)start, (short)y,
					(unsigned short)(x - start), 1
				};
				if (count == XPM_RECT_BATCH) {
					if (XFillRectangles(display, pixmap, gc,
					    rectangles, count) != 0)
						goto out;
					if (XSync(display, False) != 0)
						goto out;
					count = 0;
				}
			}
		}
		if (count != 0 && XFillRectangles(display, pixmap, gc,
		    rectangles, count) != 0)
			goto out;
		if (count != 0 && XSync(display, False) != 0)
			goto out;
	}

	/* One CopyArea is the only operation that changes the visible root. */
	XCopyArea(display, pixmap, root, gc, 0, 0, width, height, 0, 0);
	XFlush(display);

	if (background_gc != NULL)
		XFreeGC(display, background_gc);
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
	if (gc != NULL)
		XFreeGC(display, gc);
	if (pixmap != 0)
		XFreePixmap(display, pixmap);
	free(rectangles);
	free(pixels);
	free(file);
	return ok;
}

static int
ppm_token(char **cursor, char *end, char *token, size_t capacity)
{
	char *p = *cursor;
	size_t length = 0;

	for (;;) {
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' ||
		    *p == '\n'))
			p++;
		if (p >= end || *p != '#')
			break;
		while (p < end && *p != '\n')
			p++;
	}
	while (p < end && *p != ' ' && *p != '\t' && *p != '\r' &&
	    *p != '\n' && *p != '#') {
		if (length + 1U >= capacity)
			return 0;
		token[length++] = *p++;
	}
	if (length == 0)
		return 0;
	token[length] = '\0';
	*cursor = p;
	return 1;
}

static int
load_ppm(Display *display, Window root, const char *path)
{
	char *file = NULL;
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
	Pixmap pixmap = 0;
	GC gc = NULL;
	int ok = 0;

	if (!read_entire_file(path, &file, &file_size))
		goto out;
	cursor = file;
	end = file + file_size;
	if (!ppm_token(&cursor, end, token, sizeof(token)) ||
	    strcmp(token, "P6") != 0 ||
	    !ppm_token(&cursor, end, token, sizeof(token)))
		goto out;
	width = (unsigned)strtoul(token, NULL, 10);
	if (!ppm_token(&cursor, end, token, sizeof(token)))
		goto out;
	height = (unsigned)strtoul(token, NULL, 10);
	if (!ppm_token(&cursor, end, token, sizeof(token)) ||
	    strtoul(token, NULL, 10) != 255UL || width == 0 || height == 0 ||
	    width > 65535U || height > 65535U)
		goto out;
	if (cursor >= end || (*cursor != ' ' && *cursor != '\t' &&
	    *cursor != '\r' && *cursor != '\n'))
		goto out;
	if (*cursor++ == '\r' && cursor < end && *cursor == '\n')
		cursor++;
	if ((size_t)width > SIZE_MAX / (size_t)height ||
	    (size_t)width * height > SIZE_MAX / 3U)
		goto out;
	pixel_bytes = (size_t)width * height * 3U;
	if ((size_t)(end - cursor) < pixel_bytes)
		goto out;
	if (!XGetGeometry(display, root, &root_return, &root_x, &root_y,
	    &root_width, &root_height, &border, &depth) ||
	    width != root_width || height != root_height)
		goto out;
	pixmap = XCreatePixmap(display, root, width, height, depth);
	if (pixmap == 0)
		goto out;
	gc = XCreateGC(display, pixmap, 0, NULL);
	if (gc == NULL || XzedPutImageRGB24(display, pixmap, 0, 0, width,
	    height, (const unsigned char *)cursor, width * 3U) != 0 ||
	    XSync(display, False) != 0)
		goto out;
	XCopyArea(display, pixmap, root, gc, 0, 0, width, height, 0, 0);
	XSync(display, False);

	if (background_gc != NULL)
		XFreeGC(display, background_gc);
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
	if (gc != NULL)
		XFreeGC(display, gc);
	if (pixmap != 0)
		XFreePixmap(display, pixmap);
	free(file);
	return ok;
}

static void
load_background(Display *display, Window root)
{
	FILE *file;
	char line[512];
	char directory[320] = "/usr/share/xzedwm/background";
	char fallback[400] = { 0 };
	char path[400];
	unsigned width;
	unsigned height;
	unsigned border;
	unsigned depth;
	int x;
	int y;
	int path_length;
	Window root_return;

	file = fopen("/etc/Xzed/xzedwm.conf", "r");
	while (file != NULL && fgets(line, sizeof(line), file) != NULL) {
		char *p = line;
		char *destination;
		size_t capacity;
		size_t length;

		while (*p == ' ' || *p == '\t')
			p++;
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
		destination[capacity - 1U] = '\0';
		length = strlen(destination);
		while (length != 0 &&
		    (destination[length - 1] == '\n' ||
		    destination[length - 1] == '\r' ||
		    destination[length - 1] == ' ' ||
		    destination[length - 1] == '\t'))
			destination[--length] = '\0';
	}
	if (file != NULL)
		fclose(file);
	if (!XGetGeometry(display, root, &root_return, &x, &y, &width, &height,
	    &border, &depth))
		return;
	path_length = snprintf(path, sizeof(path), "%s/%ux%u.ppm", directory,
	    width, height);
	if (path_length < 0 || (size_t)path_length >= sizeof(path))
		return;
	if (load_ppm(display, root, path))
		return;
	if (fallback[0] != '\0' && load_xpm(display, root, fallback))
		return;
	fprintf(stderr, "xzedwm: cannot load %ux%u background from %s\n",
	    width, height, directory);
}

static void
restore_background(Display *display, Window root, int x, int y,
    unsigned width, unsigned height)
{
	if (background_pixmap == 0 || background_gc == NULL ||
	    x < 0 || y < 0 || (unsigned)x >= background_width ||
	    (unsigned)y >= background_height)
		return;
	if (width > background_width - (unsigned)x)
		width = background_width - (unsigned)x;
	if (height > background_height - (unsigned)y)
		height = background_height - (unsigned)y;
	XCopyArea(display, background_pixmap, root, background_gc,
	    x, y, width, height, x, y);
}

static struct frame *
by_frame(Window window)
{
	unsigned i;

	for (i = 0; i < frame_count; i++) {
		if (frames[i].frame == window)
			return &frames[i];
	}
	return NULL;
}

static void
decorate(Display *display, struct frame *frame)
{
	unsigned fw = frame_width(frame);
	unsigned fh = frame_height(frame);
	size_t title_length = strlen(frame->title);
	unsigned title_capacity = fw > 176U ? (fw - 176U) / 8U : 0;
	int minimize_x = (int)fw - 72;
	int maximize_x = (int)fw - 44;
	int close_x = (int)fw - 16;
	int title_x;
	XRectangle symbols[24];
	int symbol_count = 0;
	int offset;

	if (fw < 112U || fh < TITLE_HEIGHT + FRAME_BORDER)
		return;

	if (title_length > title_capacity)
		title_length = title_capacity;

	/* A black one-pixel outer edge encloses the title and client window. */
	XSetForeground(display, frame->gc, FRAME_EDGE);
	XFillRectangle(display, frame->frame, frame->gc, 0, 0, fw, fh);
	XSetForeground(display, frame->gc, FRAME_FACE);
	XFillRectangle(display, frame->frame, frame->gc, 1, 1, fw - 2U,
	    TITLE_HEIGHT - 2U);
	XSetForeground(display, frame->gc, FRAME_HIGHLIGHT);
	XFillRectangle(display, frame->frame, frame->gc, 2, 1, fw - 4U, 1);
	XSync(display, False);

	/* Flat monochrome controls: minimize, maximize, and close. */
	XSetForeground(display, frame->gc, FRAME_SYMBOL);
	symbols[symbol_count++] = (XRectangle) {
	    (short)(minimize_x - 4), 15, 9, 1
	};
	symbols[symbol_count++] = (XRectangle) {
	    (short)(maximize_x - 4), 9, 9, 1
	};
	symbols[symbol_count++] = (XRectangle) {
	    (short)(maximize_x - 4), 9, 1, 9
	};
	symbols[symbol_count++] = (XRectangle) {
	    (short)(maximize_x + 4), 9, 1, 9
	};
	symbols[symbol_count++] = (XRectangle) {
	    (short)(maximize_x - 4), 17, 9, 1
	};
	for (offset = -4; offset <= 4; offset++) {
		symbols[symbol_count++] = (XRectangle) {
		    (short)(close_x + offset), (short)(13 + offset), 1, 1
		};
		symbols[symbol_count++] = (XRectangle) {
		    (short)(close_x + offset), (short)(13 - offset), 1, 1
		};
	}
	XFillRectangles(display, frame->frame, frame->gc, symbols,
	    symbol_count);
	XSync(display, False);

	if (title_length != 0) {
		title_x = ((int)fw - (int)title_length * 8) / 2;
		XSetForeground(display, frame->gc, WhitePixel(display, 0));
		XDrawString(display, frame->frame, frame->gc, title_x, 20,
		    frame->title, (int)title_length);
	}
	XSync(display, False);
}

static void
manage(Display *display, Window root, Window client)
{
	Window root_return;
	int x;
	int y;
	unsigned width;
	unsigned height;
	unsigned border;
	unsigned depth;
	struct frame *frame;
	char *title = NULL;

	if (frame_count == MAX_FRAMES ||
	    !XGetGeometry(display, client, &root_return, &x, &y, &width, &height,
	    &border, &depth))
		return;
	(void)XFetchName(display, client, &title);
	if (title != NULL && strcmp(title, "_XZED_SHELL") == 0) {
		XMapWindow(display, client);
		XFree(title);
		return;
	}
	frame = &frames[frame_count++];
	memset(frame, 0, sizeof(*frame));
	frame->client = client;
	frame->x = x;
	frame->y = y;
	frame->width = width;
	frame->height = height;
	if (title != NULL && title[0] != '\0') {
		strncpy(frame->title, title, sizeof(frame->title) - 1U);
		frame->title[sizeof(frame->title) - 1U] = '\0';
	} else {
		strcpy(frame->title, "Xzed Application");
	}
	XFree(title);
	frame->frame = XCreateSimpleWindow(display, root, x, y,
	    frame_width(frame), frame_height(frame), 0, 0, FRAME_EDGE);
	XStoreName(display, frame->frame, frame->title);
	frame->gc = XCreateGC(display, frame->frame, 0, NULL);
	if (title_font != NULL)
		XSetFont(display, frame->gc, title_font->fid);
	XSelectInput(display, frame->frame,
	    ExposureMask | ButtonPressMask | ButtonReleaseMask |
	    PointerMotionMask | StructureNotifyMask);
	XReparentWindow(display, client, frame->frame, CLIENT_X, CLIENT_Y);
	XMapWindow(display, client);
	/* Drain the seven setup requests before mapping and decorating. */
	XSync(display, False);
	XMapWindow(display, frame->frame);
	XSync(display, False);
	decorate(display, frame);
	XSetInputFocus(display, client, RevertToParent, CurrentTime);
}

int
main(int argc, char **argv)
{
	Display *display = XOpenDisplay(NULL);
	Window root;
	XEvent event;
	struct frame *drag = NULL;
	int drag_x = 0;
	int drag_y = 0;
	extern char **environ;

	if (display == NULL) {
		fprintf(stderr, "xzedwm: cannot open display\n");
		return 1;
	}
	root = DefaultRootWindow(display);
	title_font = XLoadQueryFont(display, "zed-unicode");
	XSelectInput(display, root,
	    SubstructureRedirectMask | SubstructureNotifyMask | ExposureMask);
	load_background(display, root);
	if (argc > 1) {
		pid_t pid = fork();

		if (pid == 0) {
			execve(argv[1], argv + 1, environ);
			_exit(127);
		}
	}

	for (;;) {
		if (XNextEvent(display, &event) < 0)
			break;
		if (event.type == MapRequest) {
			manage(display, root, event.xmaprequest.window);
		} else if (event.type == Expose) {
			struct frame *frame = by_frame(event.xexpose.window);

			if (event.xexpose.window == root) {
				restore_background(display, root, event.xexpose.x,
				    event.xexpose.y, (unsigned)event.xexpose.width,
				    (unsigned)event.xexpose.height);
			} else if (frame != NULL) {
				decorate(display, frame);
			}
		} else if (event.type == ButtonPress) {
			struct frame *frame = by_frame(event.xbutton.window);

			if (frame != NULL && event.xbutton.keycode == 1) {
				XSetInputFocus(display, frame->client, RevertToParent,
				    CurrentTime);
				drag = frame;
				drag_x = event.xbutton.x_root - frame->x;
				drag_y = event.xbutton.y_root - frame->y;
			}
		} else if (event.type == MotionNotify && drag != NULL) {
			drag->x = event.xmotion.x_root - drag_x;
			drag->y = event.xmotion.y_root - drag_y;
			XMoveResizeWindow(display, drag->frame, drag->x, drag->y,
			    frame_width(drag), frame_height(drag));
		} else if (event.type == ButtonRelease) {
			drag = NULL;
		}
	}

	XCloseDisplay(display);
	return 0;
}
