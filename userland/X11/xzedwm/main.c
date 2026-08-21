/* xzedwm - minimal reparenting window manager. SPDX-License-Identifier: Zlib */
#include <X11/Xlib.h>

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

struct frame {
	Window client;
	Window frame;
	GC gc;
	int x;
	int y;
	unsigned width;
	unsigned height;
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

static void
load_background(Display *display, Window root)
{
	FILE *file;
	char line[512];
	char path[400] = { 0 };

	file = fopen("/etc/Xzed/xzedwm.conf", "r");
	if (file == NULL)
		return;
	while (fgets(line, sizeof(line), file) != NULL) {
		char *p = line;
		size_t length;

		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, "background=", 11) != 0)
			continue;
		strncpy(path, p + 11, sizeof(path) - 1);
		length = strlen(path);
		while (length != 0 &&
		    (path[length - 1] == '\n' || path[length - 1] == '\r' ||
		    path[length - 1] == ' ' || path[length - 1] == '\t'))
			path[--length] = '\0';
		break;
	}
	fclose(file);
	if (path[0] != '\0' && !load_xpm(display, root, path))
		fprintf(stderr, "xzedwm: cannot load background %s\n", path);
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
	XSetForeground(display, frame->gc, 0x182838);
	XFillRectangle(display, frame->frame, frame->gc, 0, 0,
	    frame->width + 4, 20);
	XSetForeground(display, frame->gc, 0xa0b8c8);
	XFillRectangle(display, frame->frame, frame->gc, 0, 0,
	    frame->width + 4, 2);
	XFillRectangle(display, frame->frame, frame->gc, 0, 0, 2,
	    frame->height + 22);
	XFillRectangle(display, frame->frame, frame->gc, frame->width + 2, 0, 2,
	    frame->height + 22);
	XFillRectangle(display, frame->frame, frame->gc, 0, frame->height + 20,
	    frame->width + 4, 2);
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

	if (frame_count == MAX_FRAMES ||
	    !XGetGeometry(display, client, &root_return, &x, &y, &width, &height,
	    &border, &depth))
		return;
	frame = &frames[frame_count++];
	memset(frame, 0, sizeof(*frame));
	frame->client = client;
	frame->x = x;
	frame->y = y;
	frame->width = width;
	frame->height = height;
	frame->frame = XCreateSimpleWindow(display, root, x, y, width + 4,
	    height + 22, 0, 0, 0x304858);
	frame->gc = XCreateGC(display, frame->frame, 0, NULL);
	XSelectInput(display, frame->frame,
	    ExposureMask | ButtonPressMask | ButtonReleaseMask |
	    PointerMotionMask | StructureNotifyMask);
	XReparentWindow(display, client, frame->frame, 2, 20);
	XMapWindow(display, client);
	XMapWindow(display, frame->frame);
	decorate(display, frame);
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
				drag = frame;
				drag_x = event.xbutton.x_root - frame->x;
				drag_y = event.xbutton.y_root - frame->y;
			}
		} else if (event.type == MotionNotify && drag != NULL) {
			drag->x = event.xmotion.x_root - drag_x;
			drag->y = event.xmotion.y_root - drag_y;
			XMoveResizeWindow(display, drag->frame, drag->x, drag->y,
			    drag->width + 4, drag->height + 22);
		} else if (event.type == ButtonRelease) {
			drag = NULL;
		}
	}

	XCloseDisplay(display);
	return 0;
}
