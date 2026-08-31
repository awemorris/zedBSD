/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zshell - lightweight Xzed desktop taskbar
 */

#include <X11/Xlib.h>
#include <X11/Xzed.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TASKBAR_HEIGHT 24U
#define LAUNCHER_WIDTH 34U
#define STATUS_WIDTH 112U
#define TASK_WIDTH 160U
#define MAX_TASKS 16U
#define TASK_NAME 64U
#define ICON_CACHE_SIZE 8U
#define ICON_PATH_SIZE 160U
#define ICON_MAX_SIZE 32U
#define ICON_MAX_COLORS 16U
#define ICON_TRANSPARENT 0xffU

#define BAR_BACKGROUND 0x17212bUL
#define BAR_PANEL 0x1d2935UL
#define BAR_PANEL_ACTIVE 0x243341UL
#define BAR_BORDER 0x344554UL
#define BAR_TEXT 0xe4eaf0UL
#define BAR_MUTED 0xaeb8c2UL

/* Icons use the deliberately small one-character-per-pixel XPM subset. */
struct icon {
	char path[ICON_PATH_SIZE];
	unsigned width;
	unsigned height;
	unsigned color_count;
	uint32_t colors[ICON_MAX_COLORS];
	uint8_t pixels[ICON_MAX_SIZE * ICON_MAX_SIZE];
	int valid;
};

struct task {
	char name[TASK_NAME];
	struct icon *icon;
	Window window;
	Window client;
};

struct desktop_shell {
	Display *display;
	Window root;
	Window window;
	GC gc;
	XFontStruct *font;
	unsigned width;
	unsigned height;
	struct task tasks[MAX_TASKS];
	unsigned task_count;
	struct icon icons[ICON_CACHE_SIZE];
	unsigned icon_count;
	char clock[6];
	int request_budget;
};

static int initialize(struct desktop_shell *shell);
static int refresh_tasks(struct desktop_shell *shell);
static Window window_client(struct desktop_shell *shell, Window window);
static int window_icon_path(struct desktop_shell *shell, Window window, char **path);
static struct icon *cached_icon(struct desktop_shell *shell, const char *path);
static int load_icon(struct icon *icon, const char *path);
static int read_entire_file(const char *path, char **result, size_t *result_size);
static char *next_quoted(char **cursor, char *end);
static int xpm_header(char *text, unsigned values[4]);
static void redraw(struct desktop_shell *shell);
static void clock_text(char *buffer, size_t capacity);
static void fill(struct desktop_shell *shell, unsigned long color, int x, int y, unsigned width, unsigned height);
static void x_request(struct desktop_shell *shell);
static void draw_launcher(struct desktop_shell *shell);
static void draw_icon(struct desktop_shell *shell, const struct icon *icon, int x, int y);
static void draw_text(struct desktop_shell *shell, int x, int y, const char *text, size_t maximum);
static void draw_speaker(struct desktop_shell *shell, int x);
static void draw_monitor(struct desktop_shell *shell, int x);
static void draw_clock(struct desktop_shell *shell, const char *clock);
static void x_finish(struct desktop_shell *shell);
static void task_click(struct desktop_shell *shell, int x);
static void update_clock(struct desktop_shell *shell);

/*
 * Runs the zshell command.
 */
int
main(
	void)
{
	XEvent event;
	int exposed;
	int tasks_changed;
	struct desktop_shell shell;
	struct pollfd descriptor;
	int running;

	running = 1;

	/* Handles a failed initialize operation. */
	if (initialize(&shell) != 0) {
		fprintf(stderr, "zshell: initialization failed: %s\n",
			strerror(errno));

		/* Reports operation failure. */
		return 1;
	}
	(void)refresh_tasks(&shell);
	redraw(&shell);

	/* Continue while the operation condition remains true. */
	while (running) {

		exposed = 0;

		/*
 * The timeout also drives the clock and discovers root-window
		 * changes. */
		descriptor =
		    (struct pollfd){ConnectionNumber(shell.display), POLLIN, 0};
		(void)poll(&descriptor, 1, 1000);

		/* Continue while the operation condition remains true. */
		while (XPending(shell.display)) {
			/* Handles a failed XNextEvent operation. */
			if (XNextEvent(shell.display, &event) < 0) {
				running = 0;
				break;
			}

			/* Handles the event condition. */
			if (event.type == Expose)
				exposed = 1;
			else if (event.type == ButtonPress &&
				 event.xbutton.keycode == 1)
				task_click(&shell, event.xbutton.x_root);
		}

		/* Handles the running condition. */
		if (!running)
			break;
		tasks_changed = refresh_tasks(&shell);

		/* Handles the exposed condition. */
		if (exposed || tasks_changed)
			redraw(&shell);
		else
			update_clock(&shell);
	}
	XDestroyWindow(shell.display, shell.window);
	XFreeGC(shell.display, shell.gc);
	XFreeFont(shell.display, shell.font);
	XCloseDisplay(shell.display);

	/* Reports successful completion. */
	return 0;
}

/* X connection setup and event loop. */
static int
initialize(
	struct desktop_shell *shell)
{
	Window root_return;
	unsigned border;
	unsigned depth;
	int x;
	int y;

	memset(shell, 0, sizeof(*shell));
	shell->display = XOpenDisplay(NULL);

	/* Handles the display availability. */
	if (shell->display == NULL)
		return -1;
	shell->root = DefaultRootWindow(shell->display);

	/* Handles a failed XGetGeometry operation. */
	if (!XGetGeometry(shell->display, shell->root, &root_return, &x, &y,
			  &shell->width, &shell->height, &border, &depth))

		/* Reports operation failure. */
		return -1;
	shell->window = XCreateSimpleWindow(
	    shell->display, shell->root, 0,
	    (int)(shell->height - TASKBAR_HEIGHT), shell->width, TASKBAR_HEIGHT,
	    0, 0, BAR_BACKGROUND);
	XStoreName(shell->display, shell->window, "_XZED_SHELL");
	shell->gc = XCreateGC(shell->display, shell->window, 0, NULL);
	shell->font = XLoadQueryFont(shell->display, "zed-unicode");

	/* Handles the font availability. */
	if (shell->font == NULL)
		return -1;
	XSetFont(shell->display, shell->gc, shell->font->fid);
	XSelectInput(shell->display, shell->window,
		     ExposureMask | ButtonPressMask);
	XMapWindow(shell->display, shell->window);
	XSync(shell->display, False);

	/* Reports successful completion. */
	return 0;
}

/* Supports the refresh tasks operation. */
static int
refresh_tasks(
	struct desktop_shell *shell)
{
	struct task *task;
	char *name;
	char *path;
	Window root_return;
	Window parent_return;
	Window *children;
	unsigned count;
	unsigned index;
	struct task next[MAX_TASKS];
	unsigned next_count;
	int changed;

	children = NULL;
	count = 0;
	next_count = 0;

	memset(next, 0, sizeof(next));

	/* Handles a failed XQueryTree operation. */
	if (!XQueryTree(shell->display, shell->root, &root_return,
			&parent_return, &children, &count))

		/* Reports successful completion. */
		return 0;

	/* Process each linked entry. */
	for (index = 0; index < count && next_count < MAX_TASKS; index++) {

		name = NULL;
		path = NULL;

		/* Handles a failed XFetchName operation. */
		if (children[index] == shell->window ||
		    !XFetchName(shell->display, children[index], &name))
			continue;

		/* Validates the current name. */
		if (name[0] == '\0' || strcmp(name, "_XZED_SHELL") == 0) {
			XFree(name);
			continue;
		}
		task = &next[next_count++];
		strncpy(task->name, name, sizeof(task->name) - 1U);
		task->window = children[index];
		task->client = window_client(shell, children[index]);

		/* Handles the window icon path condition. */
		if (window_icon_path(shell, children[index], &path)) {
			task->icon = cached_icon(shell, path);
			XFree(path);
		}
		XFree(name);
	}
	XFree(children);

	/*
 * Preserve the existing array when the root stacking order is
	 * unchanged. */
	changed = next_count != shell->task_count;

	/* Handles the changed condition. */
	if (!changed)

		/* Process each linked entry. */
		for (index = 0; index < next_count; index++)

			/* Handles the next condition. */
			if (next[index].window != shell->tasks[index].window ||
			    next[index].client != shell->tasks[index].client ||
			    strcmp(next[index].name,
				   shell->tasks[index].name) != 0 ||
			    next[index].icon != shell->tasks[index].icon) {
				changed = 1;
				break;
			}

	/* Handles the changed condition. */
	if (changed) {
		memcpy(shell->tasks, next, sizeof(next));
		shell->task_count = next_count;
	}

	/* Returns the computed result. */
	return changed;
}

/* Supports the window client operation. */
static Window
window_client(
	struct desktop_shell *shell,
	Window window)
{
	Window root_return, parent_return, *children;
	unsigned count;
	Window client;

	children = NULL;
	count = 0;
	client = window;

	/* Handles a failed XQueryTree operation. */
	if (XQueryTree(shell->display, window, &root_return, &parent_return,
		       &children, &count) &&
	    count != 0)
		client = children[0];
	XFree(children);

	/* Returns the computed result. */
	return client;
}

/* Supports the window icon path operation. */
static int
window_icon_path(
	struct desktop_shell *shell,
	Window window,
	char **path)
{
	Window root_return;
	Window parent_return;
	Window *children;
	unsigned count;
	unsigned index;

	children = NULL;
	count = 0;

	/*
 * A window manager may attach the icon to its frame or to the client.
	 */
	if (XzedGetIconPath(shell->display, window, path))
		return 1;

	/* Handles a failed XQueryTree operation. */
	if (!XQueryTree(shell->display, window, &root_return, &parent_return,
			&children, &count))

		/* Reports successful completion. */
		return 0;

	/* Process each remaining element. */
	for (index = 0; index < count; index++)

		/* Handles a failed XzedGetIconPath operation. */
		if (XzedGetIconPath(shell->display, children[index], path)) {
			XFree(children);

			/* Reports operation failure. */
			return 1;
		}
	XFree(children);

	/* Reports successful completion. */
	return 0;
}

/* Supports the cached icon operation. */
static struct icon *
cached_icon(
	struct desktop_shell *shell,
	const char *path)
{
	struct icon *function_result;
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < shell->icon_count; index++)

		/* Selects the matching value. */
		if (strcmp(shell->icons[index].path, path) == 0)
			return shell->icons[index].valid ? &shell->icons[index]
							 : NULL;

	/* Handles the shell condition. */
	if (shell->icon_count == ICON_CACHE_SIZE)
		return NULL;
	index = shell->icon_count++;

	/* Computes the function result. */
	function_result = load_icon(&shell->icons[index], path) ? &shell->icons[index]
						     : NULL;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the load icon operation. */
static int
load_icon(
	struct icon *icon,
	const char *path)
{
	char *number_end;
	unsigned long value;
	char *color;
	uint8_t palette;
	unsigned column;
	char keys[ICON_MAX_COLORS];
	char *file;
	char *cursor;
	char *end;
	char *line;
	size_t size;
	unsigned header[4];
	unsigned index;
	unsigned row;

	file = NULL;

	memset(icon, 0, sizeof(*icon));
	strncpy(icon->path, path, sizeof(icon->path) - 1U);

	/* Handles a failed read entire file operation. */
	if (!read_entire_file(path, &file, &size))
		return 0;
	cursor = file;
	end = file + size;
	line = next_quoted(&cursor, end);

	/* Handles a failed xpm header operation. */
	if (line == NULL || !xpm_header(line, header) || header[0] == 0 ||
	    header[0] > ICON_MAX_SIZE || header[1] == 0 ||
	    header[1] > ICON_MAX_SIZE || header[2] == 0 ||
	    header[2] > ICON_MAX_COLORS || header[3] != 1)
		goto invalid;
	icon->width = header[0];
	icon->height = header[1];
	icon->color_count = header[2];

	/*
 * Xzed desktop icons require one-byte keys and #RRGGBB or None colors.
	 */
	/* Process each remaining element. */
	for (index = 0; index < icon->color_count; index++) {

		line = next_quoted(&cursor, end);

		/* Handles a failed strstr operation. */
		if (line == NULL || (color = strstr(line + 1, " c ")) == NULL)
			goto invalid;
		keys[index] = line[0];
		color += 3;

		/* Continue while the operation condition remains true. */
		while (*color == ' ' || *color == '\t')
			color++;

		/* Selects the matching value. */
		if (strcmp(color, "None") == 0)
			icon->colors[index] = UINT32_MAX;
		else if (*color == '#') {

						value = strtoul(color + 1, &number_end, 16);

			/* Handles the number end condition. */
			if (number_end != color + 7 || *number_end != '\0')
				goto invalid;
			icon->colors[index] = (uint32_t)value;
		} else
			goto invalid;
	}

	/* Process each element required by the operation. */
	for (row = 0; row < icon->height; row++) {

		line = next_quoted(&cursor, end);

		/* Handles a failed strlen operation. */
		if (line == NULL || strlen(line) < icon->width)
			goto invalid;

		/* Process each element required by the operation. */
		for (column = 0; column < icon->width; column++) {
			/* Process each remaining element. */
			palette = ICON_TRANSPARENT;
			for (index = 0; index < icon->color_count; index++)

				/* Handles the line condition. */
				if (line[column] == keys[index]) {
					palette =
					    icon->colors[index] == UINT32_MAX
						? ICON_TRANSPARENT
						: (uint8_t)index;
					break;
				}
			icon->pixels[row * ICON_MAX_SIZE + column] = palette;
		}
	}
	icon->valid = 1;
	free(file);

	/* Reports operation failure. */
	return 1;
invalid:
	free(file);

	/* Reports successful completion. */
	return 0;
}

/* Minimal XPM loading and icon cache. */
static int
read_entire_file(
	const char *path,
	char **result,
	size_t *result_size)
{
	ssize_t count;
	struct stat st;
	char *data;
	size_t used;
	int descriptor;

	used = 0;

	descriptor = open(path, O_RDONLY);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return 0;

	/* Handles a failed fstat operation. */
	if (fstat(descriptor, &st) != 0 || st.st_size <= 0 ||
	    st.st_size > 65536) {
		close(descriptor);

		/* Reports successful completion. */
		return 0;
	}
	data = malloc((size_t)st.st_size + 1U);

	/* Handles the data availability. */
	if (data == NULL) {
		close(descriptor);

		/* Reports successful completion. */
		return 0;
	}
	while (used < (size_t)st.st_size) {

		count = read(descriptor, data + used, (size_t)st.st_size - used);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0) {
			free(data);
			close(descriptor);

			/* Reports successful completion. */
			return 0;
		}
		used += (size_t)count;
	}
	close(descriptor);
	data[used] = '\0';
	*result = data;
	*result_size = used;
	/* Reports operation failure. */
	return 1;
}

/* Supports the next quoted operation. */
static char *
next_quoted(
	char **cursor,
	char *end)
{
	char *start;
	char *finish;

	start = *cursor;

	/* Continue while the operation condition remains true. */
	while (start < end && *start != '"')
		start++;

	/* Handles the start condition. */
	if (start == end)
		return NULL;

	/* Continue while the operation condition remains true. */
	finish = ++start;
	while (finish < end && *finish != '"')
		finish++;

	/* Handles the finish condition. */
	if (finish == end)
		return NULL;
	*finish = '\0';
	*cursor = finish + 1;
	/* Returns the computed result. */
	return start;
}

/* Supports the xpm header operation. */
static int
xpm_header(
	char *text,
	unsigned values[4])
{
	unsigned long value;
	unsigned index;
	char *end;

	/* Process each remaining element. */
	for (index = 0; index < 4; index++) {
		/* Continue while the operation condition remains true. */
		while (*text == ' ' || *text == '\t')
			text++;
		value = strtoul(text, &end, 10);

		/* Checks the current endpoint. */
		if (end == text)
			return 0;
		values[index] = (unsigned)value;
		text = end;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the redraw operation. */
static void
redraw(
	struct desktop_shell *shell)
{
	unsigned width;
	char clock[32];
	Window focus;
	int revert;
	unsigned status_x;
	unsigned x;
	unsigned index;

	focus = None;
	status_x = shell->width > STATUS_WIDTH
				? shell->width - STATUS_WIDTH
				: shell->width;
	x = LAUNCHER_WIDTH;

	/*
 * Build the complete taskbar in Xzed's retained window backing store.
	 */
	clock_text(clock, sizeof(clock));
	(void)XGetInputFocus(shell->display, &focus, &revert);
	fill(shell, BAR_BACKGROUND, 0, 0, shell->width, TASKBAR_HEIGHT);
	fill(shell, BAR_BORDER, 0, 0, shell->width, 2);
	draw_launcher(shell);
	fill(shell, BAR_BORDER, LAUNCHER_WIDTH - 1U, 2, 1, TASKBAR_HEIGHT - 2U);

	/* Process each remaining element. */
	for (index = 0; index < shell->task_count && x + 44U < status_x;
	     index++) {

		width = TASK_WIDTH;

		/* Checks the current horizontal value. */
		if (x + width > status_x)
			width = status_x - x;
		fill(shell,
		     focus == shell->tasks[index].client ||
			     focus == shell->tasks[index].window
			 ? BAR_PANEL_ACTIVE
			 : BAR_PANEL,
		     (int)x, 2, width, TASKBAR_HEIGHT - 2U);
		fill(shell, BAR_BORDER, (int)(x + width - 1U), 2, 1,
		     TASKBAR_HEIGHT - 2U);
		draw_icon(shell, shell->tasks[index].icon, (int)x + 5, 4);
		draw_text(shell, (int)x + 28, 20, shell->tasks[index].name,
			  width > 36U ? (width - 36U) / 8U : 0);
		x += width;
	}

	/* Handles the status x condition. */
	if (status_x < shell->width) {
		fill(shell, BAR_BORDER, (int)status_x, 2, 1,
		     TASKBAR_HEIGHT - 2U);
		draw_speaker(shell, (int)status_x + 8);
		draw_monitor(shell, (int)status_x + 28);
		fill(shell, BAR_BORDER, (int)status_x + 48, 2, 1,
		     TASKBAR_HEIGHT - 2U);
		draw_clock(shell, clock);
	}
	strncpy(shell->clock, clock, sizeof(shell->clock));
	shell->clock[sizeof(shell->clock) - 1U] = '\0';
	x_finish(shell);
}

/* Desktop task discovery. */
static void
clock_text(
	char *buffer,
	size_t capacity)
{
	time_t now;
	int64_t seconds;
	unsigned hour, minute;

	now = time(NULL);

	/* Handles the now condition. */
	if (now < 0) {
		strncpy(buffer, "--:--", capacity);
		buffer[capacity - 1U] = '\0';

		/* Returns the computed result. */
		return;
	}
	seconds = now % 86400;

	/* Handles the seconds condition. */
	if (seconds < 0)
		seconds += 86400;
	hour = (unsigned)(seconds / 3600);
	minute = (unsigned)((seconds % 3600) / 60);
	snprintf(buffer, capacity, "%02u:%02u", hour, minute);
}

/* Taskbar drawing. */
static void
fill(
	struct desktop_shell *shell,
	unsigned long color,
	int x,
	int y,
	unsigned width,
	unsigned height)
{
	XSetForeground(shell->display, shell->gc, color);
	x_request(shell);
	XFillRectangle(shell->display, shell->window, shell->gc, x, y, width,
		       height);
	x_request(shell);
}

/* X request batching. */
static void
x_request(
	struct desktop_shell *shell)
{
	/*
 * Keep a redraw as one server-side dirty batch.  Intermediate XSync
	 * round trips make Xzed present partially drawn taskbar contents. */
	shell->request_budget++;
}

/* Supports the draw launcher operation. */
static void
draw_launcher(
	struct desktop_shell *shell)
{
	unsigned row;
	unsigned column;

	/* Process each element required by the operation. */
	for (row = 0; row < 3; row++)

		/* Process each element required by the operation. */
		for (column = 0; column < 3; column++)
			fill(shell, BAR_TEXT, 10 + (int)column * 5,
			     6 + (int)row * 5, 3, 3);
}

/* Supports the draw icon operation. */
static void
draw_icon(
	struct desktop_shell *shell,
	const struct icon *icon,
	int x,
	int y)
{
	unsigned first;
	unsigned column;
	unsigned color;

	/* Handles the icon availability. */
	if (icon == NULL || !icon->valid)
		return;

	/*
 * Collapse adjacent pixels into horizontal runs to reduce X requests.
	 */
	/* Process each remaining element. */
	for (color = 0; color < icon->color_count; color++) {
		XRectangle rectangles[ICON_MAX_SIZE * ICON_MAX_SIZE];
		int count = 0;
		unsigned row;

		/* Handles the icon condition. */
		if (icon->colors[color] == UINT32_MAX)
			continue;

		/* Process each element required by the operation. */
		for (row = 0; row < icon->height; row++) {
			/* Continue while the operation condition remains true. */
			column = 0;
			while (column < icon->width) {
				/* Process each remaining element. */
				while (column < icon->width &&
				       icon->pixels[row * ICON_MAX_SIZE +
						    column] != color)
					column++;

				/* Handles the column condition. */
				if (column == icon->width)
					break;

				/* Process each remaining element. */
				first = column;
				while (column < icon->width &&
				       icon->pixels[row * ICON_MAX_SIZE +
						    column] == color)
					column++;
				rectangles[count++] = (XRectangle){
				    (short)(x + (int)first),
				    (short)(y + (int)row),
				    (unsigned short)(column - first), 1};
			}
		}

		/* Checks the remaining item count. */
		if (count != 0) {
			XSetForeground(shell->display, shell->gc,
				       icon->colors[color]);
			x_request(shell);
			XFillRectangles(shell->display, shell->window,
					shell->gc, rectangles, count);
			x_request(shell);
		}
	}
}

/* Supports the draw text operation. */
static void
draw_text(
	struct desktop_shell *shell,
	int x,
	int y,
	const char *text,
	size_t maximum)
{
	size_t length;

	length = strlen(text);

	/* Checks the current data length. */
	if (length > maximum)
		length = maximum;
	XSetForeground(shell->display, shell->gc, BAR_TEXT);
	x_request(shell);
	XDrawString(shell->display, shell->window, shell->gc, x, y, text,
		    (int)length);
	x_request(shell);
}

/* Supports the draw speaker operation. */
static void
draw_speaker(
	struct desktop_shell *shell,
	int x)
{
	fill(shell, BAR_MUTED, x, 10, 2, 4);
	fill(shell, BAR_MUTED, x + 2, 8, 2, 8);
	XSetForeground(shell->display, shell->gc, BAR_MUTED);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 6, 9, x + 8,
		  11);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 8, 11, x + 8,
		  13);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 8, 13, x + 6,
		  15);
	x_request(shell);
}

/* Supports the draw monitor operation. */
static void
draw_monitor(
	struct desktop_shell *shell,
	int x)
{
	XSetForeground(shell->display, shell->gc, BAR_MUTED);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x, 6, x + 14, 6);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 14, 6, x + 14,
		  15);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 14, 15, x, 15);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x, 15, x, 6);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 7, 15, x + 7,
		  18);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 3, 18, x + 11,
		  18);
	x_request(shell);
}

/* Supports the draw clock operation. */
static void
draw_clock(
	struct desktop_shell *shell,
	const char *clock)
{
	unsigned status_x;

	status_x = shell->width > STATUS_WIDTH
				? shell->width - STATUS_WIDTH
				: shell->width;

	/* Handles the status x condition. */
	if (status_x >= shell->width)
		return;
	fill(shell, BAR_BACKGROUND, (int)status_x + 49, 2,
	     shell->width - status_x - 49U, TASKBAR_HEIGHT - 2U);
	draw_text(shell, (int)status_x + 60, 20, clock, 5);
}

/* Supports the x finish operation. */
static void
x_finish(
	struct desktop_shell *shell)
{
	/* Handles the shell condition. */
	if (shell->request_budget != 0) {
		XSync(shell->display, False);
		shell->request_budget = 0;
	}
}

/* Supports the task click operation. */
static void
task_click(
	struct desktop_shell *shell,
	int x)
{
	unsigned status_x;
	unsigned index;
	Window focus;
	int revert;

	status_x = shell->width > STATUS_WIDTH
				? shell->width - STATUS_WIDTH
				: shell->width;
	focus = None;

	/* Checks the current horizontal value. */
	if (x < (int)LAUNCHER_WIDTH || (unsigned)x >= status_x)
		return;
	index = ((unsigned)x - LAUNCHER_WIDTH) / TASK_WIDTH;

	/* Checks the current index. */
	if (index >= shell->task_count)
		return;
	(void)XGetInputFocus(shell->display, &focus, &revert);

	/*
 * Clicking the active task hides it; clicking an inactive task restores
	 * it. */
	if (focus == shell->tasks[index].client ||
	    focus == shell->tasks[index].window) {
		XUnmapWindow(shell->display, shell->tasks[index].window);
		XSetInputFocus(shell->display, shell->root, RevertToParent,
			       CurrentTime);
	} else {
		XMapWindow(shell->display, shell->tasks[index].window);
		XRaiseWindow(shell->display, shell->tasks[index].window);
		XSetInputFocus(shell->display, shell->tasks[index].client,
			       RevertToParent, CurrentTime);
	}
	redraw(shell);
}

/* Supports the update clock operation. */
static void
update_clock(
	struct desktop_shell *shell)
{
	char clock[32];

	clock_text(clock, sizeof(clock));

	/* Selects the matching value. */
	if (strcmp(clock, shell->clock) == 0)
		return;
	draw_clock(shell, clock);
	strncpy(shell->clock, clock, sizeof(shell->clock));
	shell->clock[sizeof(shell->clock) - 1U] = '\0';
	x_finish(shell);
}
