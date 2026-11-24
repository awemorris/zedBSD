/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The layout of the tabs in the title bar's strip on Android: where each tab,
 * its close button and the strip's buttons go.  Pure: no system header, no
 * state, so that a host test can run it.  The way of laying out is the
 * Windows backend's (win32/frame.c), with the lengths of a finger's size.
 */

#include "apps/libkeiland/android/tabs.h"

#include <stddef.h>
#include <string.h>

/* The dpi at which the lengths of the header are written; dpi 0 means this. */
#define TABS_BASE_DPI	96U

/*
 * The lengths of a tab strip's parts at its dpi, scaled once for a layout:
 * a tab's height and a button's side (the same), the gap, a tab's most and
 * least widths, a close button's side and inset, and the row's top.
 */
struct tabs_lengths {
	uint32_t height;
	uint32_t button;
	uint32_t gap;
	uint32_t most;
	uint32_t least;
	uint32_t close;
	uint32_t inset;
	int32_t y;
};

static int32_t tabs_clamp(int64_t value);
static void tabs_empty(struct android_rect *rect);
static void tabs_lengths_make(const struct android_tab_strip *strip, struct tabs_lengths *lengths);
static int32_t tabs_all(const struct android_tab_strip *strip, const int *closable, struct android_tab_place *places, struct android_rect *new_place, struct android_tab_scroll *scroll, const struct tabs_lengths *lengths);
static int tabs_scroll(const struct android_tab_strip *strip, const int *closable, struct android_tab_place *places, struct android_rect *new_place, struct android_tab_scroll *scroll, const struct tabs_lengths *lengths, int32_t *end);
static int32_t tabs_list(const struct android_tab_strip *strip, struct android_rect *new_place, struct android_tab_scroll *scroll, const struct tabs_lengths *lengths);
static void tabs_put(struct android_tab_place *place, int64_t x, uint32_t width, int closable, const struct tabs_lengths *lengths);
static void tabs_square(struct android_rect *rect, int64_t x, const struct tabs_lengths *lengths);

/*
 * Scales a length at 96 dpi to dpi: the product is exact in 64 bits and is
 * rounded to the nearest whole pixel, halves up.
 */
uint32_t
android_scale(
	uint32_t value,
	unsigned dpi)
{
	uint64_t scaled;
	uint64_t window_dpi;

	/* A window that does not know its dpi is taken at the base one. */
	window_dpi = dpi;
	if (window_dpi == 0U)
		window_dpi = TABS_BASE_DPI;

	/* Multiplies in 64 bits and rounds half up. */
	scaled = ((uint64_t)value * window_dpi + TABS_BASE_DPI / 2U) / TABS_BASE_DPI;

	/* Saturates a result a 32 bit length cannot hold. */
	if (scaled > UINT32_MAX)
		return UINT32_MAX;

	/* Succeeded: the length in pixels. */
	return (uint32_t)scaled;
}

/*
 * Lays out the tabs of the strip, scrolling it when they do not fit.  The
 * way is chosen in order: every tab when one tab or the least widths of all
 * fit, else the scrolling strip when one tab fits before the buttons, else
 * the list's button alone.
 */
int32_t
android_tabs_layout(
	const struct android_tab_strip *strip,
	const int *closable,
	struct android_tab_place *places,
	struct android_rect *new_place,
	struct android_tab_scroll *scroll)
{
	struct android_tab_scroll unasked;
	struct tabs_lengths lengths;
	int64_t room;
	int64_t share;
	int32_t end;
	unsigned index;
	int fits;

	/* Writes nothing without the strip and the places to write. */
	if (strip == NULL)
		return 0;
	if (new_place == NULL)
		return strip->left;
	if (strip->count != 0U && (places == NULL || closable == NULL))
		return strip->left;

	/* How the strip was laid out goes somewhere even when the caller does not ask. */
	if (scroll == NULL)
		scroll = &unasked;

	/* Starts from nothing shown: no tab, no button. */
	memset(scroll, 0, sizeof(*scroll));
	tabs_empty(new_place);
	for (index = 0U; index < strip->count; index++) {
		tabs_empty(&places[index].tab);
		tabs_empty(&places[index].close);
		places[index].shown = 0;
	}

	/* The lengths at the strip's dpi. */
	tabs_lengths_make(strip, &lengths);

	/* The room the tabs share beside the new tab's button. */
	room = (int64_t)strip->right - (int64_t)strip->left;
	if (strip->new_button)
		room -= (int64_t)lengths.button + (int64_t)lengths.gap;
	if (room < 0)
		room = 0;

	/* No tab or one: the tabs side by side, which leave out a single tab that does not fit. */
	if (strip->count < 2U) {
		end = tabs_all(strip, closable, places, new_place, scroll, &lengths);
		return end;
	}

	/* Every tab at its share, when the share is not below the least. */
	share = room / (int64_t)strip->count - (int64_t)lengths.gap;
	if (share >= (int64_t)lengths.least) {
		end = tabs_all(strip, closable, places, new_place, scroll, &lengths);
		return end;
	}

	/* The strip scrolls when one tab fits before the buttons. */
	fits = tabs_scroll(strip, closable, places, new_place, scroll, &lengths, &end);
	if (fits)
		return end;

	/* Succeeded: only the list's button (and the new tab's) on a strip too narrow for a tab. */
	end = tabs_list(strip, new_place, scroll, &lengths);
	return end;
}

/*
 * Tells what a point is on among the tabs laid out.  A close button's
 * touch area is the tab's right end over its whole height (a finger is
 * wider than the button).
 */
unsigned
android_tabs_at(
	const struct android_tab_place *places,
	unsigned count,
	const struct android_rect *new_place,
	const struct android_tab_scroll *scroll,
	int32_t x,
	int32_t y,
	unsigned *part)
{
	struct android_rect reach;
	unsigned index;
	int hit;

	/* Somewhere to answer, and the places. */
	if (part == NULL || (count != 0U && places == NULL))
		return ANDROID_TAB_NONE;

	/* Each tab shown: its close button's reach first, then the tab. */
	for (index = 0U; index < count; index++) {
		if (!places[index].shown)
			continue;
		if (places[index].close.width != 0U) {
			reach.x = places[index].close.x - (int32_t)(places[index].close.width / 4U);
			reach.y = places[index].tab.y;
			reach.width = (uint32_t)(places[index].tab.x + (int32_t)places[index].tab.width - reach.x);
			reach.height = places[index].tab.height;
			hit = android_rect_contains(&reach, x, y);
			if (hit) {
				*part = ANDROID_TAB_PART_CLOSE;
				return index;
			}
		}

		hit = android_rect_contains(&places[index].tab, x, y);
		if (hit) {
			*part = ANDROID_TAB_PART_BODY;
			return index;
		}
	}

	/* The new tab's button. */
	if (new_place != NULL) {
		hit = android_rect_contains(new_place, x, y);
		if (hit) {
			*part = ANDROID_TAB_PART_NEW;
			return count;
		}
	}

	/* No list without the strip's scroll. */
	if (scroll == NULL)
		return ANDROID_TAB_NONE;

	/* The list's button. */
	hit = android_rect_contains(&scroll->list, x, y);
	if (hit) {
		*part = ANDROID_TAB_PART_LIST;
		return count;
	}

	/* Nothing of the strip. */
	return ANDROID_TAB_NONE;
}

/* Answers 1 when the point is inside the rectangle, compared in 64 bits so that no end wraps. */
int
android_rect_contains(
	const struct android_rect *rect,
	int32_t x,
	int32_t y)
{
	/* An empty rectangle holds nothing; the edges are the first coordinate past the rectangle. */
	if (rect->width == 0U || rect->height == 0U)
		return 0;
	if ((int64_t)x < (int64_t)rect->x || (int64_t)x >= (int64_t)rect->x + (int64_t)rect->width)
		return 0;
	if ((int64_t)y < (int64_t)rect->y || (int64_t)y >= (int64_t)rect->y + (int64_t)rect->height)
		return 0;

	/* Inside. */
	return 1;
}

/* Limits a 64 bit coordinate to the range of int32_t. */
static int32_t
tabs_clamp(
	int64_t value)
{
	/* Out of the range is the nearest end of it. */
	if (value < (int64_t)INT32_MIN)
		return INT32_MIN;
	if (value > (int64_t)INT32_MAX)
		return INT32_MAX;

	/* Succeeded: the coordinate already fits. */
	return (int32_t)value;
}

/* Makes a rectangle empty at the origin. */
static void
tabs_empty(
	struct android_rect *rect)
{
	/* Nowhere, of no size. */
	rect->x = 0;
	rect->y = 0;
	rect->width = 0U;
	rect->height = 0U;
}

/* Scales the lengths of a strip's parts to its dpi, and places their row in the middle of the strip's height. */
static void
tabs_lengths_make(
	const struct android_tab_strip *strip,
	struct tabs_lengths *lengths)
{
	/* Each length at the strip's dpi; a button is a square of a tab's height. */
	lengths->height = android_scale(ANDROID_TAB_HEIGHT, strip->dpi);
	lengths->button = lengths->height;
	lengths->gap = android_scale(ANDROID_TAB_GAP, strip->dpi);
	lengths->most = android_scale(ANDROID_TAB_MOST, strip->dpi);
	lengths->least = android_scale(ANDROID_TAB_LEAST, strip->dpi);
	lengths->close = android_scale(ANDROID_TAB_CLOSE, strip->dpi);
	lengths->inset = android_scale(ANDROID_TAB_CLOSE_INSET, strip->dpi);

	/* The row's top, in the middle of the strip's height. */
	lengths->y = (int32_t)(((int64_t)strip->height - (int64_t)lengths->height) / 2);
}

/* Lays out every tab side by side at one width (ANDROID_STRIP_ALL); returns where the strip ends. */
static int32_t
tabs_all(
	const struct android_tab_strip *strip,
	const int *closable,
	struct android_tab_place *places,
	struct android_rect *new_place,
	struct android_tab_scroll *scroll,
	const struct tabs_lengths *lengths)
{
	uint32_t width;
	int64_t room;
	int64_t share;
	int64_t x;
	int32_t end;
	unsigned shown;
	unsigned index;

	/* The room the tabs share beside the new tab's button. */
	room = (int64_t)strip->right - (int64_t)strip->left;
	if (strip->new_button)
		room -= (int64_t)lengths->button + (int64_t)lengths->gap;
	if (room < 0)
		room = 0;

	/* Each tab's width: the room's share less its gap, up to the most; a share below the least shows none. */
	width = 0U;
	shown = 0U;
	if (strip->count != 0U) {
		share = room / (int64_t)strip->count - (int64_t)lengths->gap;
		if (share > (int64_t)lengths->most)
			share = (int64_t)lengths->most;
		if (share >= (int64_t)lengths->least) {
			width = (uint32_t)share;
			shown = strip->count;
		}
	}

	/* Each tab shown, side by side from the strip's left. */
	x = strip->left;
	for (index = 0U; index < shown; index++) {
		tabs_put(&places[index], x, width, closable[index], lengths);
		x += (int64_t)width + (int64_t)lengths->gap;
	}

	/* How the strip shows its tabs: all of them from the first, or none. */
	scroll->mode = ANDROID_STRIP_ALL;
	scroll->first = 0U;
	scroll->shown = shown;

	/* The new tab's button after the tabs, when asked for and when it fits before the strip's right. */
	if (strip->new_button && x + (int64_t)lengths->button <= (int64_t)strip->right) {
		tabs_square(new_place, x, lengths);
		end = tabs_clamp(x + (int64_t)lengths->button);
		return end;
	}

	/* Without the button: nothing shown ends at the left. */
	if (shown == 0U)
		return strip->left;

	/* Succeeded: the strip ends at the last tab's right edge. */
	end = tabs_clamp(x - (int64_t)lengths->gap);
	return end;
}

/* Lays out a scrolling strip (ANDROID_STRIP_SCROLL) and writes where it ends.  Returns 0, with nothing written, when not even one tab fits before the buttons. */
static int
tabs_scroll(
	const struct android_tab_strip *strip,
	const int *closable,
	struct android_tab_place *places,
	struct android_rect *new_place,
	struct android_tab_scroll *scroll,
	const struct tabs_lengths *lengths,
	int32_t *end)
{
	int64_t list_x;
	int64_t new_x;
	int64_t span;
	int64_t width;
	int64_t x;
	unsigned shown;
	unsigned first;
	unsigned index;

	/* From the right: the list's button at the strip's end, and the new tab's button before it. */
	list_x = (int64_t)strip->right - (int64_t)lengths->button;
	new_x = list_x;
	if (strip->new_button)
		new_x = list_x - (int64_t)lengths->gap - (int64_t)lengths->button;

	/* The tabs' room, from the strip's left to the buttons. */
	span = new_x - (int64_t)lengths->gap - (int64_t)strip->left;
	if (span < (int64_t)lengths->least)
		return 0;

	/* As many tabs as fit at the least, sharing the room equally (up to the most). */
	shown = (unsigned)((span + (int64_t)lengths->gap) / ((int64_t)lengths->least + (int64_t)lengths->gap));
	if (shown > strip->count)
		shown = strip->count;
	width = (span + (int64_t)lengths->gap) / (int64_t)shown - (int64_t)lengths->gap;
	if (width > (int64_t)lengths->most)
		width = (int64_t)lengths->most;

	/* The first tab shown: the one asked for, moved by the least to show the active tab when asked to. */
	first = strip->first;
	if (strip->reveal && strip->active < strip->count) {
		if (strip->active < first)
			first = strip->active;
		if (strip->active >= first + shown)
			first = strip->active + 1U - shown;
	}

	/* No room is left after the last tab. */
	if (first > strip->count - shown)
		first = strip->count - shown;

	/* The tabs in sight, side by side from the strip's left. */
	x = strip->left;
	for (index = first; index < first + shown; index++) {
		tabs_put(&places[index], x, (uint32_t)width, closable[index], lengths);
		x += width + (int64_t)lengths->gap;
	}

	/* The new tab's button, when asked for, and the list's button at the end. */
	if (strip->new_button)
		tabs_square(new_place, new_x, lengths);
	tabs_square(&scroll->list, list_x, lengths);

	/* How the strip shows its tabs. */
	scroll->mode = ANDROID_STRIP_SCROLL;
	scroll->first = first;
	scroll->shown = shown;

	/* Succeeded: the strip ends at the list's button, its right. */
	*end = strip->right;
	return 1;
}

/* Lays out a strip too narrow for a tab (ANDROID_STRIP_LIST); returns where it ends. */
static int32_t
tabs_list(
	const struct android_tab_strip *strip,
	struct android_rect *new_place,
	struct android_tab_scroll *scroll,
	const struct tabs_lengths *lengths)
{
	int64_t both;
	int64_t x;
	int32_t end;

	/* No tab is shown; the list holds them all. */
	scroll->mode = ANDROID_STRIP_LIST;
	scroll->first = 0U;
	scroll->shown = 0U;

	/* The new tab's button first, when asked for and when the list's button fits after it. */
	x = strip->left;
	both = 2 * (int64_t)lengths->button + (int64_t)lengths->gap;
	if (strip->new_button && x + both <= (int64_t)strip->right) {
		tabs_square(new_place, x, lengths);
		x += (int64_t)lengths->button + (int64_t)lengths->gap;
	}

	/* Nothing at all when not even the list's button fits. */
	if (x + (int64_t)lengths->button > (int64_t)strip->right)
		return strip->left;

	/* Succeeded: the list's button, where the strip ends. */
	tabs_square(&scroll->list, x, lengths);
	end = tabs_clamp(x + (int64_t)lengths->button);
	return end;
}

/* Places one tab at x of a width, with its close button when it is closable. */
static void
tabs_put(
	struct android_tab_place *place,
	int64_t x,
	uint32_t width,
	int closable,
	const struct tabs_lengths *lengths)
{
	/* The tab, in the strip's row. */
	place->shown = 1;
	place->tab.x = tabs_clamp(x);
	place->tab.y = lengths->y;
	place->tab.width = width;
	place->tab.height = lengths->height;

	/* Its close button, at its right in the middle of its height, when there is room for it. */
	if (closable && width > lengths->close + lengths->inset) {
		place->close.x = tabs_clamp(x + (int64_t)width - (int64_t)lengths->inset - (int64_t)lengths->close);
		place->close.y = lengths->y + (int32_t)((lengths->height - lengths->close) / 2U);
		place->close.width = lengths->close;
		place->close.height = lengths->close;
	}
}

/* Places a button of the strip (a square of a tab's height) at x in the strip's row. */
static void
tabs_square(
	struct android_rect *rect,
	int64_t x,
	const struct tabs_lengths *lengths)
{
	/* The square. */
	rect->x = tabs_clamp(x);
	rect->y = lengths->y;
	rect->width = lengths->button;
	rect->height = lengths->button;
}
