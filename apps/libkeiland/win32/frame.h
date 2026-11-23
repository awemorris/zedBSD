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
 * the three caption buttons are, and lays out the tabs a window's title
 * bar shows.  Pure: no Windows header (the HT* values are given here), no
 * state; the tests run on the host.
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

/* The title bar's lengths at 96 dpi, which the room of its controls and tabs is made of. */
#define KEIUI_WIN32_TITLE_CONTROL	30U	/* a control, and the menu's button (a square) */
#define KEIUI_WIN32_TITLE_GAP		4U	/* between two controls, and after the menu's button */
#define KEIUI_WIN32_TITLE_SIDE		10U	/* from the window's left edge, and before the caption buttons */
#define KEIUI_WIN32_TITLE_DRAG_LEAST	96U	/* the bare title bar left after the tabs to drag the window by */

/*
 * The parts of a title bar its controls and tabs are laid out in (physical
 * pixels): the menu's button (empty without a menu), where the controls
 * or the tabs start (left), where the controls end (right: before the
 * margin before the caption buttons), and where the tab strip ends
 * (strip_right: right less the bare title bar left to drag the window by,
 * never left of left).
 */
struct keiui_win32_title_room {
	struct keiui_win32_rect menu;
	int32_t left;
	int32_t right;
	int32_t strip_right;
};

/*
 * Lays out the parts of a title bar of a frame (its width, dpi and state)
 * whose bar is height pixels tall: the menu's button, a
 * KEIUI_WIN32_TITLE_CONTROL square in the middle of the height at
 * KEIUI_WIN32_TITLE_SIDE from the left edge when has_menu is not 0, then
 * the controls' room from after it (and KEIUI_WIN32_TITLE_GAP) to
 * KEIUI_WIN32_TITLE_SIDE before the minimize button, and the tab strip's
 * end KEIUI_WIN32_TITLE_DRAG_LEAST before that (all scaled).  frame or
 * room NULL: nothing is written.
 */
void keiui_win32_title_room(const struct keiui_win32_frame *frame, uint32_t height, int has_menu, struct keiui_win32_title_room *room);

/* The tabs' lengths at 96 dpi. */
#define KEIUI_WIN32_TAB_HEIGHT		26U	/* a tab, and the strip's buttons (squares): the new tab's, the arrows, the list's */
#define KEIUI_WIN32_TAB_MOST		200U	/* the widest a tab grows */
#define KEIUI_WIN32_TAB_LEAST		96U	/* the narrowest a tab shrinks; past it the strip scrolls */
#define KEIUI_WIN32_TAB_GAP		2U	/* between two tabs, and between the strip's buttons */
#define KEIUI_WIN32_TAB_CLOSE		18U	/* a tab's close button (a square) */
#define KEIUI_WIN32_TAB_CLOSE_INSET	5U	/* from the close button to the tab's right edge */

/* What a point of the tab strip is on (keiui_win32_tabs_at). */
#define KEIUI_WIN32_TAB_NONE		0xFFFFFFFFU	/* no tab, no button */
#define KEIUI_WIN32_TAB_PART_BODY	0U		/* a tab (to choose it) */
#define KEIUI_WIN32_TAB_PART_CLOSE	1U		/* a tab's close button */
#define KEIUI_WIN32_TAB_PART_NEW	2U		/* the new tab's button */
#define KEIUI_WIN32_TAB_PART_BACK	3U		/* the arrow to the tabs before (while it can move) */
#define KEIUI_WIN32_TAB_PART_FORWARD	4U		/* the arrow to the tabs after (while it can move) */
#define KEIUI_WIN32_TAB_PART_LIST	5U		/* the list of the tabs out of sight */

/*
 * How a strip shows its tabs (keiui_win32_tabs_layout_scroll):
 *  ALL:    every tab, side by side (narrowed down to the least when need be);
 *          one tab that does not fit even at the least is left out;
 *  SCROLL: two or more tabs that do not fit at the least: as many as fit
 *          between an arrow at each end, the new tab's button and the
 *          list's button after the arrow on the right;
 *  LIST:   no room even for that: no tab, the new tab's button (when it is
 *          asked for and fits) and the list's button (when it fits).
 */
#define KEIUI_WIN32_STRIP_ALL		0U
#define KEIUI_WIN32_STRIP_SCROLL	1U
#define KEIUI_WIN32_STRIP_LIST		2U

/*
 * The room the tabs are laid out in: the title bar's part from left to
 * right (physical pixels, right not included) and its height, the dpi,
 * how many tabs there are, which is active (count or more: none), and
 * whether the new tab's button follows them.  A scrolling strip starts at
 * the tab first (moved to keep within the tabs), and is moved first to
 * show the active tab when reveal is not 0.
 */
struct keiui_win32_tab_strip {
	int32_t left;
	int32_t right;
	uint32_t caption;
	unsigned dpi;
	unsigned count;
	unsigned active;
	int new_button;
	unsigned first;
	int reveal;
};

/*
 * How a strip was laid out besides its tabs' places: its way
 * (KEIUI_WIN32_STRIP_*), the first tab shown and how many are shown (in
 * a row from first; 0 when none), the arrows of a scrolling strip (empty
 * otherwise) and whether each can move the strip (0: drawn faint, no
 * part), and the list's button (empty unless the strip scrolls or shows
 * the list).
 */
struct keiui_win32_tab_scroll {
	unsigned mode;
	unsigned first;
	unsigned shown;
	struct keiui_win32_rect back;
	struct keiui_win32_rect forward;
	int back_live;
	int forward_live;
	struct keiui_win32_rect list;
};

/*
 * Where one tab goes: the tab, its close button (empty when it has none),
 * and whether it is shown (0: it did not fit; both rectangles empty).
 */
struct keiui_win32_tab_place {
	struct keiui_win32_rect tab;
	struct keiui_win32_rect close;
	int shown;
};

/*
 * Lays out the tabs of a title bar, scrolling the strip when they do not
 * fit.
 *
 * All lengths are at 96 dpi scaled to the strip's dpi, and the strip's
 * parts never reach past its right.  The tabs, the button and the arrows
 * are KEIUI_WIN32_TAB_HEIGHT tall, in the middle of the caption's height;
 * the buttons and the arrows are squares of that side, KEIUI_WIN32_TAB_GAP
 * apart.
 *
 * KEIUI_WIN32_STRIP_ALL: the tabs share the room less the new tab's button
 * (and a gap, when asked for): each is the room's share less a gap, at
 * most KEIUI_WIN32_TAB_MOST, and the button follows the last tab.  A
 * single tab that does not fit at KEIUI_WIN32_TAB_LEAST is not shown; the
 * button then starts the strip when it fits.
 *
 * KEIUI_WIN32_STRIP_SCROLL, for two or more tabs whose least width is too
 * much for the room: the arrow back at the strip's left, the list's button
 * ending at its right, the new tab's button (when asked for) and the arrow
 * forward before it; between the arrows, as many tabs as fit at the least
 * width, sharing that room equally (at most the most).  first is moved to
 * show the active tab when reveal is not 0 and it is out of sight (by the
 * least move), then kept so that no room is left after the last tab.  The
 * arrow back can move while first is above 0, the one forward while tabs
 * follow the last shown.
 *
 * KEIUI_WIN32_STRIP_LIST, when not even one tab fits between the arrows:
 * the new tab's button (when asked for) and the list's button after it,
 * or the list's button alone when both do not fit, or nothing.
 *
 * A tab whose closable[index] is not 0 has a close button of
 * KEIUI_WIN32_TAB_CLOSE at its right, KEIUI_WIN32_TAB_CLOSE_INSET from its
 * edge, in the middle of its height.  A tab not shown has empty places.
 * strip or new_place NULL, or places or closable NULL with tabs: nothing is
 * written and strip's left is returned (0 without strip).  new_place is
 * empty without the button; scroll may be NULL.
 * Returns where the strip ends (the right edge of the last part shown;
 * left when nothing is shown).
 */
int32_t keiui_win32_tabs_layout_scroll(const struct keiui_win32_tab_strip *strip, const int *closable, struct keiui_win32_tab_place *places, struct keiui_win32_rect *new_place, struct keiui_win32_tab_scroll *scroll);

/*
 * Lays out the tabs of a title bar as keiui_win32_tabs_layout_scroll does,
 * always showing the active tab (reveal taken as 1), without telling how.
 */
int32_t keiui_win32_tabs_layout(const struct keiui_win32_tab_strip *strip, const int *closable, struct keiui_win32_tab_place *places, struct keiui_win32_rect *new_place);

/*
 * Tells what a point is on among the tabs laid out: a tab's close button
 * (part KEIUI_WIN32_TAB_PART_CLOSE), a tab (KEIUI_WIN32_TAB_PART_BODY), or
 * the new tab's button (KEIUI_WIN32_TAB_PART_NEW, and the index count).
 * Returns the tab's index, count for the button, or KEIUI_WIN32_TAB_NONE
 * (part not written).  NULL places with count above 0, or NULL part:
 * KEIUI_WIN32_TAB_NONE.
 */
unsigned keiui_win32_tabs_at(const struct keiui_win32_tab_place *places, unsigned count, const struct keiui_win32_rect *new_place, int32_t x, int32_t y, unsigned *part);

/*
 * Tells what a point is on among the tabs laid out, as keiui_win32_tabs_at
 * does, and also a live arrow (KEIUI_WIN32_TAB_PART_BACK or _FORWARD) or
 * the list's button (KEIUI_WIN32_TAB_PART_LIST), for which count is
 * returned.  An arrow that cannot move the strip is no part.  scroll NULL:
 * as keiui_win32_tabs_at.
 */
unsigned keiui_win32_tabs_at_scroll(const struct keiui_win32_tab_place *places, unsigned count, const struct keiui_win32_rect *new_place, const struct keiui_win32_tab_scroll *scroll, int32_t x, int32_t y, unsigned *part);

/* The wheel's notch (WHEEL_DELTA), which moves a scrolling strip by one tab. */
#define KEIUI_WIN32_WHEEL_NOTCH		120

/* Whose a wheel's turn is (keiui_win32_tab_wheel). */
#define KEIUI_WIN32_WHEEL_CONTENT	0U	/* the content's: not over the title bar */
#define KEIUI_WIN32_WHEEL_TITLE		1U	/* the title bar's: never the content's */

/*
 * Decides a wheel's turn (WM_MOUSEWHEEL, or WM_MOUSEHWHEEL when horizontal
 * is not 0, its signed delta) at a point of a frame: on_frame not 0 when
 * the window under the point is the frame itself (not its content nor
 * another window), y the point's row in the frame's client and bar the
 * title bar's height in pixels, on_strip not 0 when the point is on a
 * part of the tab strip (a tab, an arrow, the new tab's or the list's
 * button), scrolling not 0 when the strip scrolls.
 *
 * A point not on the frame or not above bar is the content's: pending is
 * cleared.  Otherwise the turn is the title bar's, and it moves the strip
 * only on a scrolling strip's part: the turn toward the tabs after (down,
 * or right) or before (up, or left) is added to pending (cleared first
 * when it turned the other way), and each KEIUI_WIN32_WHEEL_NOTCH of it
 * is one tab, written to steps (after: positive) and taken from pending.
 * Elsewhere on the title bar pending is cleared.  steps is 0 when the
 * strip does not move.  pending or steps NULL: the content's, nothing
 * written.
 */
unsigned keiui_win32_tab_wheel(int on_frame, int32_t y, uint32_t bar, int on_strip, int scrolling, int horizontal, int delta, int *pending, int *steps);

#endif
