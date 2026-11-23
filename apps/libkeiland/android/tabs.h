/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The layout of the tabs in the title bar's strip on Android (tabs.c).
 *
 * Pure: no system header and no state, so that a host test can run it.
 * Coordinates are the strip's pixels, x to the right and y down from its
 * top-left corner.  Lengths given "at 96 dpi" are scaled with
 * android_scale (a window's scale is in 96ths: 96 is the base).
 */

#ifndef KEILAND_ANDROID_TABS_H
#define KEILAND_ANDROID_TABS_H

#include <stdint.h>

/* The strip's lengths at 96 dpi: the strip, a tab and the buttons (squares of a tab's height). */
#define ANDROID_STRIP_HEIGHT	48U
#define ANDROID_TAB_HEIGHT	40U
#define ANDROID_TAB_MOST	220U
#define ANDROID_TAB_LEAST	112U
#define ANDROID_TAB_GAP		4U
#define ANDROID_TAB_CLOSE	28U
#define ANDROID_TAB_CLOSE_INSET	6U
#define ANDROID_STRIP_SIDE	6U

/* What a point of the tab strip is on (android_tabs_at). */
#define ANDROID_TAB_NONE	0xFFFFFFFFU
#define ANDROID_TAB_PART_BODY	0U
#define ANDROID_TAB_PART_CLOSE	1U
#define ANDROID_TAB_PART_NEW	2U
#define ANDROID_TAB_PART_LIST	3U

/* How a strip shows its tabs: every tab, as many as fit (a finger's swipe moves the strip), or only the list's button. */
#define ANDROID_STRIP_ALL	0U
#define ANDROID_STRIP_SCROLL	1U
#define ANDROID_STRIP_LIST	2U

/* A rectangle of pixels: empty when width or height is 0. */
struct android_rect {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
};

/*
 * The room the tabs are laid out in: the strip's part from left to right
 * (right not included) and its height, the dpi, how many tabs there are,
 * which is active (count or more: none), and whether the new tab's button
 * follows them.  A scrolling strip starts at the tab first (moved to keep
 * within the tabs), and is moved first to show the active tab when reveal
 * is not 0.
 */
struct android_tab_strip {
	int32_t left;
	int32_t right;
	uint32_t height;
	unsigned dpi;
	unsigned count;
	unsigned active;
	int new_button;
	unsigned first;
	int reveal;
};

/*
 * How a strip was laid out besides its tabs' places: its way, the first tab
 * shown and how many are shown, and the list's button.  A scrolling strip has
 * no arrows: a finger's swipe moves it.
 */
struct android_tab_scroll {
	unsigned mode;
	unsigned first;
	unsigned shown;
	struct android_rect list;
};

/* Where one tab goes: the tab, its close button (empty when it has none), and whether it is shown. */
struct android_tab_place {
	struct android_rect tab;
	struct android_rect close;
	int shown;
};

/* Scales a length at 96 dpi to dpi (0 is the base), rounded to the nearest pixel. */
uint32_t android_scale(uint32_t value, unsigned dpi);

/*
 * Lays out the tabs, scrolling the strip when they do not fit: every tab
 * when the least widths fit, else the scrolling strip when one tab fits
 * before the buttons, else the list's button alone.  closable[index] says whether
 * a tab has a close button.  Returns where the strip ends (left when
 * nothing is shown).  places has count entries; new_place is empty without
 * the button; scroll may be NULL.
 */
int32_t android_tabs_layout(const struct android_tab_strip *strip, const int *closable, struct android_tab_place *places, struct android_rect *new_place, struct android_tab_scroll *scroll);

/*
 * Tells what a point is on among the tabs laid out: the tab's index (count
 * for the strip's buttons) and in part what of it, or ANDROID_TAB_NONE.  A
 * tab's close button reaches the tab's right end and its whole height.
 */
unsigned android_tabs_at(const struct android_tab_place *places, unsigned count, const struct android_rect *new_place, const struct android_tab_scroll *scroll, int32_t x, int32_t y, unsigned *part);

/* Answers 1 when the point is inside the rectangle (an empty one holds none). */
int android_rect_contains(const struct android_rect *rect, int32_t x, int32_t y);

#endif /* KEILAND_ANDROID_TABS_H */
