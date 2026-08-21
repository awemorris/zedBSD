/*
 * xzedshell - lightweight Xzed desktop taskbar
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
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

static void
x_request(struct desktop_shell *shell)
{
	if (++shell->request_budget >= 5) {
		XSync(shell->display, False);
		shell->request_budget = 0;
	}
}

static void
x_finish(struct desktop_shell *shell)
{
	if (shell->request_budget != 0) {
		XSync(shell->display, False);
		shell->request_budget = 0;
	}
}

static int
read_entire_file(const char *path, char **result, size_t *result_size)
{
	struct stat st;
	char *data;
	size_t used = 0;
	int descriptor;

	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return 0;
	if (fstat(descriptor, &st) != 0 || st.st_size <= 0 ||
	    st.st_size > 65536) {
		close(descriptor);
		return 0;
	}
	data = malloc((size_t)st.st_size + 1U);
	if (data == NULL) {
		close(descriptor);
		return 0;
	}
	while (used < (size_t)st.st_size) {
		ssize_t count = read(descriptor, data + used,
		    (size_t)st.st_size - used);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			free(data);
			close(descriptor);
			return 0;
		}
		used += (size_t)count;
	}
	close(descriptor);
	data[used] = '\0';
	*result = data;
	*result_size = used;
	return 1;
}

static char *
next_quoted(char **cursor, char *end)
{
	char *start = *cursor;
	char *finish;

	while (start < end && *start != '"')
		start++;
	if (start == end)
		return NULL;
	finish = ++start;
	while (finish < end && *finish != '"')
		finish++;
	if (finish == end)
		return NULL;
	*finish = '\0';
	*cursor = finish + 1;
	return start;
}

static int
xpm_header(char *text, unsigned values[4])
{
	unsigned index;
	char *end;

	for (index = 0; index < 4; index++) {
		unsigned long value;
		while (*text == ' ' || *text == '\t')
			text++;
		value = strtoul(text, &end, 10);
		if (end == text)
			return 0;
		values[index] = (unsigned)value;
		text = end;
	}
	return 1;
}

static int
load_icon(struct icon *icon, const char *path)
{
	char keys[ICON_MAX_COLORS];
	char *file = NULL;
	char *cursor;
	char *end;
	char *line;
	size_t size;
	unsigned header[4];
	unsigned index;
	unsigned row;

	memset(icon, 0, sizeof(*icon));
	strncpy(icon->path, path, sizeof(icon->path) - 1U);
	if (!read_entire_file(path, &file, &size))
		return 0;
	cursor = file;
	end = file + size;
	line = next_quoted(&cursor, end);
	if (line == NULL || !xpm_header(line, header) ||
	    header[0] == 0 || header[0] > ICON_MAX_SIZE ||
	    header[1] == 0 || header[1] > ICON_MAX_SIZE ||
	    header[2] == 0 || header[2] > ICON_MAX_COLORS || header[3] != 1)
		goto invalid;
	icon->width = header[0];
	icon->height = header[1];
	icon->color_count = header[2];
	for (index = 0; index < icon->color_count; index++) {
		char *color;
		line = next_quoted(&cursor, end);
		if (line == NULL || (color = strstr(line + 1, " c ")) == NULL)
			goto invalid;
		keys[index] = line[0];
		color += 3;
		while (*color == ' ' || *color == '\t')
			color++;
		if (strcmp(color, "None") == 0)
			icon->colors[index] = UINT32_MAX;
		else if (*color == '#') {
			char *number_end;
			unsigned long value = strtoul(color + 1, &number_end, 16);
			if (number_end != color + 7 || *number_end != '\0')
				goto invalid;
			icon->colors[index] = (uint32_t)value;
		} else
			goto invalid;
	}
	for (row = 0; row < icon->height; row++) {
		unsigned column;
		line = next_quoted(&cursor, end);
		if (line == NULL || strlen(line) < icon->width)
			goto invalid;
		for (column = 0; column < icon->width; column++) {
			uint8_t palette = ICON_TRANSPARENT;
			for (index = 0; index < icon->color_count; index++)
				if (line[column] == keys[index]) {
					palette = icon->colors[index] == UINT32_MAX ?
					    ICON_TRANSPARENT : (uint8_t)index;
					break;
				}
			icon->pixels[row * ICON_MAX_SIZE + column] = palette;
		}
	}
	icon->valid = 1;
	free(file);
	return 1;
invalid:
	free(file);
	return 0;
}

static struct icon *
cached_icon(struct desktop_shell *shell, const char *path)
{
	unsigned index;

	for (index = 0; index < shell->icon_count; index++)
		if (strcmp(shell->icons[index].path, path) == 0)
			return shell->icons[index].valid ? &shell->icons[index] : NULL;
	if (shell->icon_count == ICON_CACHE_SIZE)
		return NULL;
	index = shell->icon_count++;
	return load_icon(&shell->icons[index], path) ? &shell->icons[index] : NULL;
}

static void
clock_text(char *buffer, size_t capacity)
{
	time_t now = time(NULL);
	int64_t seconds;
	unsigned hour, minute;

	if (now < 0) {
		strncpy(buffer, "--:--", capacity);
		buffer[capacity - 1U] = '\0';
		return;
	}
	seconds = now % 86400;
	if (seconds < 0)
		seconds += 86400;
	hour = (unsigned)(seconds / 3600);
	minute = (unsigned)((seconds % 3600) / 60);
	snprintf(buffer, capacity, "%02u:%02u", hour, minute);
}

static int
window_icon_path(struct desktop_shell *shell, Window window, char **path)
{
	Window root_return;
	Window parent_return;
	Window *children = NULL;
	unsigned count = 0;
	unsigned index;

	if (XzedGetIconPath(shell->display, window, path))
		return 1;
	if (!XQueryTree(shell->display, window, &root_return, &parent_return,
	    &children, &count))
		return 0;
	for (index = 0; index < count; index++)
		if (XzedGetIconPath(shell->display, children[index], path)) {
			XFree(children);
			return 1;
		}
	XFree(children);
	return 0;
}

static int
refresh_tasks(struct desktop_shell *shell)
{
	Window root_return;
	Window parent_return;
	Window *children = NULL;
	unsigned count = 0;
	unsigned index;
	struct task next[MAX_TASKS];
	unsigned next_count = 0;
	int changed;

	memset(next, 0, sizeof(next));
	if (!XQueryTree(shell->display, shell->root, &root_return,
	    &parent_return, &children, &count))
		return 0;
	for (index = 0; index < count && next_count < MAX_TASKS;
	    index++) {
		struct task *task;
		char *name = NULL;
		char *path = NULL;

		if (children[index] == shell->window ||
		    !XFetchName(shell->display, children[index], &name))
			continue;
		if (name[0] == '\0' || strcmp(name, "_XZED_SHELL") == 0) {
			XFree(name);
			continue;
		}
		task = &next[next_count++];
		strncpy(task->name, name, sizeof(task->name) - 1U);
		if (window_icon_path(shell, children[index], &path)) {
			task->icon = cached_icon(shell, path);
			XFree(path);
		}
		XFree(name);
	}
	XFree(children);
	changed = next_count != shell->task_count;
	if (!changed)
		for (index = 0; index < next_count; index++)
			if (strcmp(next[index].name, shell->tasks[index].name) != 0 ||
			    next[index].icon != shell->tasks[index].icon) {
				changed = 1;
				break;
			}
	if (changed) {
		memcpy(shell->tasks, next, sizeof(next));
		shell->task_count = next_count;
	}
	return changed;
}

static void
fill(struct desktop_shell *shell, unsigned long color, int x, int y,
    unsigned width, unsigned height)
{
	XSetForeground(shell->display, shell->gc, color);
	x_request(shell);
	XFillRectangle(shell->display, shell->window, shell->gc, x, y,
	    width, height);
	x_request(shell);
}

static void
draw_text(struct desktop_shell *shell, int x, int y, const char *text,
    size_t maximum)
{
	size_t length = strlen(text);
	if (length > maximum)
		length = maximum;
	XSetForeground(shell->display, shell->gc, BAR_TEXT);
	x_request(shell);
	XDrawString(shell->display, shell->window, shell->gc, x, y,
	    text, (int)length);
	x_request(shell);
}

static void
draw_icon(struct desktop_shell *shell, const struct icon *icon, int x, int y)
{
	unsigned color;
	if (icon == NULL || !icon->valid)
		return;
	for (color = 0; color < icon->color_count; color++) {
		XRectangle rectangles[ICON_MAX_SIZE * ICON_MAX_SIZE];
		int count = 0;
		unsigned row;
		if (icon->colors[color] == UINT32_MAX)
			continue;
		for (row = 0; row < icon->height; row++) {
			unsigned column = 0;
			while (column < icon->width) {
				unsigned first;
				while (column < icon->width &&
				    icon->pixels[row * ICON_MAX_SIZE + column] != color)
					column++;
				if (column == icon->width)
					break;
				first = column;
				while (column < icon->width &&
				    icon->pixels[row * ICON_MAX_SIZE + column] == color)
					column++;
				rectangles[count++] = (XRectangle){
				    (short)(x + (int)first), (short)(y + (int)row),
				    (unsigned short)(column - first), 1 };
			}
		}
		if (count != 0) {
			XSetForeground(shell->display, shell->gc, icon->colors[color]);
			x_request(shell);
			XFillRectangles(shell->display, shell->window, shell->gc,
			    rectangles, count);
			x_request(shell);
		}
	}
}

static void
draw_launcher(struct desktop_shell *shell)
{
	unsigned row;
	unsigned column;
	for (row = 0; row < 3; row++)
		for (column = 0; column < 3; column++)
			fill(shell, BAR_TEXT, 10 + (int)column * 5,
			    6 + (int)row * 5, 3, 3);
}

static void
draw_speaker(struct desktop_shell *shell, int x)
{
	fill(shell, BAR_MUTED, x, 10, 2, 4);
	fill(shell, BAR_MUTED, x + 2, 8, 2, 8);
	XSetForeground(shell->display, shell->gc, BAR_MUTED);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 6, 9,
	    x + 8, 11);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 8, 11,
	    x + 8, 13);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 8, 13,
	    x + 6, 15);
	x_request(shell);
}

static void
draw_monitor(struct desktop_shell *shell, int x)
{
	XSetForeground(shell->display, shell->gc, BAR_MUTED);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x, 6, x + 14, 6);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 14, 6,
	    x + 14, 15);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 14, 15,
	    x, 15);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x, 15, x, 6);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 7, 15,
	    x + 7, 18);
	x_request(shell);
	XDrawLine(shell->display, shell->window, shell->gc, x + 3, 18,
	    x + 11, 18);
	x_request(shell);
}

static void
draw_clock(struct desktop_shell *shell, const char *clock)
{
	unsigned status_x = shell->width > STATUS_WIDTH ?
	    shell->width - STATUS_WIDTH : shell->width;

	if (status_x >= shell->width)
		return;
	fill(shell, BAR_BACKGROUND, (int)status_x + 49, 2,
	    shell->width - status_x - 49U, TASKBAR_HEIGHT - 2U);
	draw_text(shell, (int)status_x + 60, 20, clock, 5);
}

static void
redraw(struct desktop_shell *shell)
{
	char clock[32];
	unsigned status_x = shell->width > STATUS_WIDTH ?
	    shell->width - STATUS_WIDTH : shell->width;
	unsigned x = LAUNCHER_WIDTH;
	unsigned index;

	clock_text(clock, sizeof(clock));
	fill(shell, BAR_BACKGROUND, 0, 0, shell->width, TASKBAR_HEIGHT);
	fill(shell, BAR_BORDER, 0, 0, shell->width, 2);
	draw_launcher(shell);
	fill(shell, BAR_BORDER, LAUNCHER_WIDTH - 1U, 2, 1,
	    TASKBAR_HEIGHT - 2U);
	for (index = 0; index < shell->task_count && x + 44U < status_x;
	    index++) {
		unsigned width = TASK_WIDTH;
		if (x + width > status_x)
			width = status_x - x;
		fill(shell, index == 0 ? BAR_PANEL_ACTIVE : BAR_PANEL,
		    (int)x, 2, width, TASKBAR_HEIGHT - 2U);
		fill(shell, BAR_BORDER, (int)(x + width - 1U), 2, 1,
		    TASKBAR_HEIGHT - 2U);
		draw_icon(shell, shell->tasks[index].icon, (int)x + 5, 4);
		draw_text(shell, (int)x + 28, 20, shell->tasks[index].name,
		    width > 36U ? (width - 36U) / 8U : 0);
		x += width;
	}
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

static void
update_clock(struct desktop_shell *shell)
{
	char clock[32];

	clock_text(clock, sizeof(clock));
	if (strcmp(clock, shell->clock) == 0)
		return;
	draw_clock(shell, clock);
	strncpy(shell->clock, clock, sizeof(shell->clock));
	shell->clock[sizeof(shell->clock) - 1U] = '\0';
	x_finish(shell);
}

static int
initialize(struct desktop_shell *shell)
{
	Window root_return;
	unsigned border;
	unsigned depth;
	int x;
	int y;

	memset(shell, 0, sizeof(*shell));
	shell->display = XOpenDisplay(NULL);
	if (shell->display == NULL)
		return -1;
	shell->root = DefaultRootWindow(shell->display);
	if (!XGetGeometry(shell->display, shell->root, &root_return, &x, &y,
	    &shell->width, &shell->height, &border, &depth))
		return -1;
	shell->window = XCreateSimpleWindow(shell->display, shell->root, 0,
	    (int)(shell->height - TASKBAR_HEIGHT), shell->width,
	    TASKBAR_HEIGHT, 0, 0, BAR_BACKGROUND);
	XStoreName(shell->display, shell->window, "_XZED_SHELL");
	shell->gc = XCreateGC(shell->display, shell->window, 0, NULL);
	shell->font = XLoadQueryFont(shell->display, "zed-unicode");
	if (shell->font == NULL)
		return -1;
	XSetFont(shell->display, shell->gc, shell->font->fid);
	XSelectInput(shell->display, shell->window, ExposureMask);
	XMapWindow(shell->display, shell->window);
	XSync(shell->display, False);
	return 0;
}

int
main(void)
{
	struct desktop_shell shell;
	struct pollfd descriptor;
	int running = 1;

	if (initialize(&shell) != 0) {
		fprintf(stderr, "xzedshell: initialization failed: %s\n",
		    strerror(errno));
		return 1;
	}
	(void)refresh_tasks(&shell);
	redraw(&shell);
	while (running) {
		int exposed = 0;
		int tasks_changed;

		descriptor = (struct pollfd){ ConnectionNumber(shell.display),
		    POLLIN, 0 };
		(void)poll(&descriptor, 1, 1000);
		while (XPending(shell.display)) {
			XEvent event;
			if (XNextEvent(shell.display, &event) < 0) {
				running = 0;
				break;
			}
			if (event.type == Expose)
				exposed = 1;
		}
		if (!running)
			break;
		tasks_changed = refresh_tasks(&shell);
		if (exposed || tasks_changed)
			redraw(&shell);
		else
			update_clock(&shell);
	}
	XDestroyWindow(shell.display, shell.window);
	XFreeGC(shell.display, shell.gc);
	XFreeFont(shell.display, shell.font);
	XCloseDisplay(shell.display);
	return 0;
}
