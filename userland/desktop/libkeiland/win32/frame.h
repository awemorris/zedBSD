/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The geometry of the Windows backend's window frame (frame.c).
 *
 * A Keiland window on Windows draws its own title bar, as Keiland's
 * compositor draws it on zedBSD: the window removes the system's
 * non-client area (WM_NCCALCSIZE gives the whole window to the client) and
 * answers WM_NCHITTEST itself, so Windows still moves, resizes, snaps and
 * shows the snap layouts over the maximize button (HTMAXBUTTON).  This
 * module decides, for a point, what part of the window it is on, and where
 * the three caption buttons are.  Pure: no Windows header (the HT* values
 * are given here), no state; the tests run on the host.
 *
 * Coordinates are the client area's physical pixels, x to the right and y
 * down from the window's top-left corner.  Lengths given "at 96 dpi" are
 * scaled with keiui_win32_scale.
 */

#ifndef KEIUI_WIN32_FRAME_H
#define KEIUI_WIN32_FRAME_H

#include <stdint.h>

/* The answers of WM_NCHITTEST (<winuser.h> values). */
#define KEIUI_WIN32_HIT_NOWHERE		0U	/* HTNOWHERE */
#define KEIUI_WIN32_HIT_CLIENT		1U	/* HTCLIENT */
#define KEIUI_WIN32_HIT_CAPTION		2U	/* HTCAPTION */
#define KEIUI_WIN32_HIT_MINIMIZE	8U	/* HTMINBUTTON */
#define KEIUI_WIN32_HIT_MAXIMIZE	9U	/* HTMAXBUTTON */
#define KEIUI_WIN32_HIT_LEFT		10U	/* HTLEFT */
#define KEIUI_WIN32_HIT_RIGHT		11U	/* HTRIGHT */
#define KEIUI_WIN32_HIT_TOP		12U	/* HTTOP */
#define KEIUI_WIN32_HIT_TOP_LEFT	13U	/* HTTOPLEFT */
#define KEIUI_WIN32_HIT_TOP_RIGHT	14U	/* HTTOPRIGHT */
#define KEIUI_WIN32_HIT_BOTTOM		15U	/* HTBOTTOM */
#define KEIUI_WIN32_HIT_BOTTOM_LEFT	16U	/* HTBOTTOMLEFT */
#define KEIUI_WIN32_HIT_BOTTOM_RIGHT	17U	/* HTBOTTOMRIGHT */
#define KEIUI_WIN32_HIT_CLOSE		20U	/* HTCLOSE */

/* The lengths of the frame at 96 dpi. */
#define KEIUI_WIN32_RESIZE_BORDER	8U	/* the band inside each edge that resizes */
#define KEIUI_WIN32_RESIZE_CORNER	16U	/* along an edge, how far from a corner the corner's diagonal resize reaches */
#define KEIUI_WIN32_TOP_BORDER		4U	/* the top edge's band where it crosses the caption buttons is thinner */
#define KEIUI_WIN32_BUTTON_WIDTH	46U	/* each caption button */

/* The caption buttons, in order from the left (keiui_win32_frame_buttons). */
#define KEIUI_WIN32_BUTTON_MINIMIZE_INDEX	0U
#define KEIUI_WIN32_BUTTON_MAXIMIZE_INDEX	1U
#define KEIUI_WIN32_BUTTON_CLOSE_INDEX		2U

/* A rectangle of physical pixels: x and y its top-left corner; empty when width or height is 0. */
struct keiui_win32_rect {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
};

/*
 * A window's frame as the hit test needs it.  The window fills it in from
 * its state before each WM_NCHITTEST; the module only reads it.
 */
struct keiui_win32_frame {
	uint32_t width;				/* the client area (the whole window) */
	uint32_t height;
	unsigned dpi;				/* the window's dpi (GetDpiForWindow), 96 = 100 %; 0 is taken as 96 */
	int maximized;				/* 1 when maximized: no resize bands, the buttons reach the top */
	int fullscreen;				/* 1 when full screen: no title bar at all (everything is client) */
	uint32_t caption_height;		/* the title bar's height at 96 dpi (0: no title bar) */
	const struct keiui_win32_rect *controls;	/* the application's own controls in the title bar (physical pixels) */
	unsigned control_count;
};

/*
 * Scales a length at 96 dpi to dpi: value * dpi / 96 rounded to the
 * nearest integer (halves up), computed in 64 bits; dpi 0 is 96.  A result
 * above UINT32_MAX is UINT32_MAX.
 */
uint32_t keiui_win32_scale(uint32_t value, unsigned dpi);

/*
 * Writes the rectangles of the three caption buttons (minimize, maximize,
 * close, at the KEIUI_WIN32_BUTTON_*_INDEX places of buttons).  They sit
 * side by side at the right edge, close the rightmost, each
 * KEIUI_WIN32_BUTTON_WIDTH wide (scaled) and the caption's height (scaled)
 * tall, from y 0.  A window narrower than the three is laid out the same
 * way (x may go negative).  Full screen or no title bar: three empty
 * rectangles.  frame or buttons NULL: nothing is written.
 */
void keiui_win32_frame_buttons(const struct keiui_win32_frame *frame, struct keiui_win32_rect buttons[3]);

/*
 * Answers WM_NCHITTEST for a point of the client area.
 *
 * In this order (the first that holds):
 *  1. frame NULL, or the point outside 0 <= x < width, 0 <= y < height:
 *     NOWHERE.
 *  2. full screen: CLIENT.
 *  3. not maximized, inside the resize band of an edge
 *     (KEIUI_WIN32_RESIZE_BORDER scaled; the top band is
 *     KEIUI_WIN32_TOP_BORDER scaled where it crosses a caption button):
 *     the corner (TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT) when also
 *     within KEIUI_WIN32_RESIZE_CORNER scaled of the crossing edge, else the
 *     edge (LEFT, RIGHT, TOP, BOTTOM).
 *  4. inside a caption button: MINIMIZE, MAXIMIZE or CLOSE.
 *  5. inside one of the controls: CLIENT.
 *  6. y below the caption's height (scaled): CAPTION.
 *  7. otherwise CLIENT.
 * A rectangle contains x when rect.x <= x < rect.x + width (computed in 64
 * bits), the same for y.
 */
unsigned keiui_win32_frame_hit(const struct keiui_win32_frame *frame, int32_t x, int32_t y);

#endif
