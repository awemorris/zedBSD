/*
 * xzedshell - lightweight Xzed desktop taskbar
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <X11/Xlib.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TASKBAR_HEIGHT 30U
#define MAX_TASKS 16U
#define TASK_NAME 64U
#define TASK_WIDTH 120U
#define CLOCK_WIDTH 152U

#define BAR_FACE 0xaeb2c3UL
#define BAR_HIGHLIGHT 0xdcdee5UL
#define BAR_SHADOW 0x5d6069UL
#define BAR_TEXT 0x101018UL

struct desktop_shell {
	Display *display;
	Window root;
	Window window;
	GC gc;
	XFontStruct *font;
	unsigned width;
	unsigned height;
	char tasks[MAX_TASKS][TASK_NAME];
	unsigned task_count;
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

/* Gregorian UTC conversion; no locale or timezone database is required. */
static void
civil_from_days(int64_t days, int *year, unsigned *month, unsigned *day)
{
	int64_t era;
	unsigned day_of_era;
	unsigned year_of_era;
	int y;
	unsigned day_of_year;
	unsigned month_prime;

	days += 719468;
	era = (days >= 0 ? days : days - 146096) / 146097;
	day_of_era = (unsigned)(days - era * 146097);
	year_of_era = (day_of_era - day_of_era / 1460 +
	    day_of_era / 36524 - day_of_era / 146096) / 365;
	y = (int)year_of_era + (int)era * 400;
	day_of_year = day_of_era -
	    (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	month_prime = (5 * day_of_year + 2) / 153;
	*day = day_of_year - (153 * month_prime + 2) / 5 + 1;
	*month = month_prime + (month_prime < 10 ? 3 : (unsigned)-9);
	*year = y + (*month <= 2);
}

static void
clock_text(char *buffer, size_t capacity)
{
	time_t now = time(NULL);
	int64_t days;
	int64_t seconds;
	int year;
	unsigned month, day, hour, minute;

	if (now < 0) {
		strncpy(buffer, "---- -- -- --:--", capacity);
		buffer[capacity - 1U] = '\0';
		return;
	}
	days = now / 86400;
	seconds = now % 86400;
	if (seconds < 0) {
		seconds += 86400;
		days--;
	}
	civil_from_days(days, &year, &month, &day);
	hour = (unsigned)(seconds / 3600);
	minute = (unsigned)((seconds % 3600) / 60);
	snprintf(buffer, capacity, "%04d-%02u-%02u %02u:%02u",
	    year, month, day, hour, minute);
}

static void
refresh_tasks(struct desktop_shell *shell)
{
	Window root_return;
	Window parent_return;
	Window *children = NULL;
	unsigned count = 0;
	unsigned i;

	shell->task_count = 0;
	if (!XQueryTree(shell->display, shell->root, &root_return,
	    &parent_return, &children, &count))
		return;
	for (i = 0; i < count && shell->task_count < MAX_TASKS; i++) {
		char *name = NULL;
		if (children[i] == shell->window ||
		    !XFetchName(shell->display, children[i], &name))
			continue;
		if (name[0] != '\0' && strcmp(name, "_XZED_SHELL") != 0) {
			strncpy(shell->tasks[shell->task_count], name, TASK_NAME - 1U);
			shell->tasks[shell->task_count][TASK_NAME - 1U] = '\0';
			shell->task_count++;
		}
		XFree(name);
	}
	XFree(children);
}

static void
raised_box(struct desktop_shell *shell, unsigned x, unsigned y,
    unsigned width, unsigned height)
{
	XRectangle light[2];
	XRectangle dark[2];

	XSetForeground(shell->display, shell->gc, BAR_FACE);
	x_request(shell);
	XFillRectangle(shell->display, shell->window, shell->gc,
	    (int)x, (int)y, width, height);
	x_request(shell);
	light[0] = (XRectangle){ (short)x, (short)y, (unsigned short)width, 1 };
	light[1] = (XRectangle){ (short)x, (short)y, 1,
	    (unsigned short)height };
	XSetForeground(shell->display, shell->gc, BAR_HIGHLIGHT);
	x_request(shell);
	XFillRectangles(shell->display, shell->window, shell->gc, light, 2);
	x_request(shell);
	dark[0] = (XRectangle){ (short)x, (short)(y + height - 1U),
	    (unsigned short)width, 1 };
	dark[1] = (XRectangle){ (short)(x + width - 1U), (short)y, 1,
	    (unsigned short)height };
	XSetForeground(shell->display, shell->gc, BAR_SHADOW);
	x_request(shell);
	XFillRectangles(shell->display, shell->window, shell->gc, dark, 2);
	x_request(shell);
}

static void
draw_text(struct desktop_shell *shell, int x, const char *text,
    size_t maximum)
{
	size_t length = strlen(text);

	if (length > maximum)
		length = maximum;
	XSetForeground(shell->display, shell->gc, BAR_TEXT);
	x_request(shell);
	XDrawString(shell->display, shell->window, shell->gc, x, 23,
	    text, (int)length);
	x_request(shell);
}

static void
redraw(struct desktop_shell *shell)
{
	char clock[32];
	unsigned clock_x = shell->width > CLOCK_WIDTH + 4U ?
	    shell->width - CLOCK_WIDTH - 4U : 0;
	unsigned x = 4;
	unsigned i;

	clock_text(clock, sizeof(clock));
	XSetForeground(shell->display, shell->gc, BAR_FACE);
	x_request(shell);
	XFillRectangle(shell->display, shell->window, shell->gc, 0, 0,
	    shell->width, TASKBAR_HEIGHT);
	x_request(shell);
	raised_box(shell, 0, 0, shell->width, TASKBAR_HEIGHT);
	for (i = 0; i < shell->task_count && x + 40U < clock_x; i++) {
		unsigned width = TASK_WIDTH;
		if (x + width + 4U > clock_x)
			width = clock_x - x - 4U;
		raised_box(shell, x, 3, width, TASKBAR_HEIGHT - 6U);
		draw_text(shell, (int)x + 7, shell->tasks[i],
		    width > 14U ? (width - 14U) / 8U : 0);
		x += width + 3U;
	}
	if (clock_x != 0) {
		raised_box(shell, clock_x, 3, CLOCK_WIDTH,
		    TASKBAR_HEIGHT - 6U);
		draw_text(shell, (int)clock_x + 8, clock, 17);
	}
	x_finish(shell);
}

static int
initialize(struct desktop_shell *shell)
{
	Window root_return;
	unsigned border, depth;
	int x, y;

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
	    TASKBAR_HEIGHT, 0, 0, BAR_FACE);
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
	while (running) {
		descriptor = (struct pollfd){ ConnectionNumber(shell.display),
		    POLLIN, 0 };
		(void)poll(&descriptor, 1, 1000);
		while (XPending(shell.display)) {
			XEvent event;
			if (XNextEvent(shell.display, &event) < 0) {
				running = 0;
				break;
			}
		}
		if (!running)
			break;
		refresh_tasks(&shell);
		redraw(&shell);
	}
	XDestroyWindow(shell.display, shell.window);
	XFreeGC(shell.display, shell.gc);
	XFreeFont(shell.display, shell.font);
	XCloseDisplay(shell.display);
	return 0;
}
