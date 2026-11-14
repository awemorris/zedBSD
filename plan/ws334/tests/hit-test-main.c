/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A small program for a check under Wine that asks a running program's
 * frame what part of it points are on, as Windows asks it before the
 * pointer's every move:
 *   hit-test [--double] <x> <y> [<x> <y> ...]
 * The frame is the widest visible top-level window of libkeiland's frame
 * class.  Each point is in the frame's client (its whole window), x from
 * the left edge, or from the right edge when negative (-1 the last
 * column).  For each it sends WM_NCHITTEST with the point on the screen
 * and prints "hit-test: <x> <y> <answer>" (2 HTCAPTION, 1 HTCLIENT, 8 9 20
 * the caption buttons, 10 to 17 the edges); first the line
 * "hit-test: frame <width> <height>".  With --double, each point is also
 * double-clicked as Windows tells the frame of a double click there
 * (WM_NCLBUTTONDBLCLK with the answer, for a window whose X window Wine
 * does not move, as when it is maximized without a window manager), and
 * the frame's size after it is printed: "hit-test: frame <width>
 * <height>".  The exit status is 0 when the frame was found and every
 * point asked.
 */

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libkeiland's frame class (win32/window.c). */
#define HIT_FRAME_CLASS	L"KeilandWindow"

/* How long the frame may take to answer, in milliseconds. */
#define HIT_ANSWER_MS	5000U

/* The widest frame found so far, for EnumWindows. */
struct hit_search {
	HWND frame;
	LONG width;
};

static BOOL CALLBACK hit_each(HWND window, LPARAM data);

/*
 * Finds the frame and asks it about each point.
 */
int
main(
	int argc,
	char **argv)
{
	struct hit_search search;
	DWORD_PTR answer;
	LRESULT sent;
	RECT client;
	POINT point;
	LONG width;
	LONG height;
	long x;
	long y;
	int index;
	int first;
	int twice;
	BOOL found;
	int different;

	/* The option, and pairs of coordinates. */
	first = 1;
	twice = 0;
	different = 1;
	if (argc > 1)
		different = strcmp(argv[1], "--double");
	if (different == 0) {
		first = 2;
		twice = 1;
	}

	/* Pairs of coordinates are required. */
	if (argc - first < 2 || (argc - first) % 2 != 0) {
		fprintf(stderr, "usage: hit-test [--double] <x> <y> [<x> <y> ...]\n");
		return 2;
	}

	/* The frame. */
	search.frame = NULL;
	search.width = 0;
	(void)EnumWindows(hit_each, (LPARAM)&search);
	if (search.frame == NULL) {
		printf("hit-test: no frame\n");
		return 1;
	}

	/* The frame's client rectangle. */
	found = GetClientRect(search.frame, &client);
	if (!found) {
		printf("hit-test: no client rectangle (%lu)\n", (unsigned long)GetLastError());
		return 1;
	}

	/* The frame's size. */
	width = client.right - client.left;
	height = client.bottom - client.top;
	printf("hit-test: frame %ld %ld\n", (long)width, (long)height);

	/* Each point, on the screen, and the frame's answer. */
	for (index = first; index + 1 < argc; index += 2) {
		x = strtol(argv[index], NULL, 10);
		y = strtol(argv[index + 1], NULL, 10);
		if (x < 0)
			x += (long)width;
		point.x = (LONG)x;
		point.y = (LONG)y;
		(void)ClientToScreen(search.frame, &point);
		answer = 0;
		sent = SendMessageTimeoutW(search.frame, WM_NCHITTEST, 0, MAKELPARAM((WORD)(SHORT)point.x, (WORD)(SHORT)point.y),
		    SMTO_ABORTIFHUNG, HIT_ANSWER_MS, &answer);
		if (sent == 0) {
			printf("hit-test: %ld %ld no answer (%lu)\n", x, y, (unsigned long)GetLastError());
			return 1;
		}

		/* The click's answer. */
		printf("hit-test: %ld %ld %ld\n", x, y, (long)answer);

		/* The double click there, and the frame's size after it. */
		if (!twice)
			continue;
		sent = SendMessageTimeoutW(search.frame, WM_NCLBUTTONDBLCLK, (WPARAM)answer,
		    MAKELPARAM((WORD)(SHORT)point.x, (WORD)(SHORT)point.y), SMTO_ABORTIFHUNG, HIT_ANSWER_MS, NULL);
		if (sent == 0) {
			printf("hit-test: %ld %ld the double click had no answer (%lu)\n", x, y, (unsigned long)GetLastError());
			return 1;
		}

		/* The frame's client rectangle after the double click. */
		found = GetClientRect(search.frame, &client);
		if (!found) {
			printf("hit-test: no client rectangle (%lu)\n", (unsigned long)GetLastError());
			return 1;
		}

		/* The frame's size after the double click. */
		width = client.right - client.left;
		height = client.bottom - client.top;
		printf("hit-test: frame %ld %ld\n", (long)width, (long)height);
	}

	/* Succeeded: every point asked. */
	return 0;
}

/*
 * Keeps the widest visible window of the frame class.
 */
static BOOL CALLBACK
hit_each(
	HWND window,
	LPARAM data)
{
	struct hit_search *search;
	WCHAR name[64];
	RECT bounds;
	LONG width;
	int length;
	BOOL got;
	int different;
	BOOL visible;

	/* A visible window of the class. */
	search = (struct hit_search *)data;
	visible = IsWindowVisible(window);
	if (!visible)
		return TRUE;
	length = GetClassNameW(window, name, (int)(sizeof(name) / sizeof(name[0])));
	different = 0;
	if (length > 0)
		different = lstrcmpW(name, HIT_FRAME_CLASS);
	if (length <= 0 || different != 0)
		return TRUE;

	/* The widest so far. */
	got = GetWindowRect(window, &bounds);
	if (!got)
		return TRUE;
	width = bounds.right - bounds.left;
	if (width > search->width) {
		search->width = width;
		search->frame = window;
	}

	/* On to the next window. */
	return TRUE;
}
