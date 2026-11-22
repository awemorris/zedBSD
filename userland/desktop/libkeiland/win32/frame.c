/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The geometry of the Windows backend's window frame.
 */

#include "userland/desktop/libkeiland/win32/frame.h"

#include <stddef.h>

/* The dpi at which the lengths of the header are written; dpi 0 means this. */
#define FRAME_BASE_DPI	96U

/* The number of caption buttons. */
#define FRAME_BUTTON_COUNT	3U

/* What the resize-band step answers when the point is on no band (not a WM_NCHITTEST value). */
#define FRAME_NO_EDGE	0xFFFFFFFFU

/*
 * What the resize bands answer, by the vertical side (rows: top, middle,
 * bottom) and the horizontal side (columns: left, middle, right) the point
 * is on.  The middle of both is never looked up: a point on no band has
 * no side.
 */
static const unsigned frame_edge_table[3][3] = {
	{ KEIUI_WIN32_HIT_TOP_LEFT, KEIUI_WIN32_HIT_TOP, KEIUI_WIN32_HIT_TOP_RIGHT },
	{ KEIUI_WIN32_HIT_LEFT, FRAME_NO_EDGE, KEIUI_WIN32_HIT_RIGHT },
	{ KEIUI_WIN32_HIT_BOTTOM_LEFT, KEIUI_WIN32_HIT_BOTTOM, KEIUI_WIN32_HIT_BOTTOM_RIGHT }
};

static int32_t frame_clamp_int32(int64_t value);
static int frame_rect_contains(const struct keiui_win32_rect *rect, int32_t x, int32_t y);
static int frame_pick_side(int64_t coordinate, int64_t extent, int64_t low_reach, int64_t high_reach);
static unsigned frame_edge_hit(const struct keiui_win32_frame *frame, const struct keiui_win32_rect buttons[3], int32_t x, int32_t y);
static void frame_rect_empty(struct keiui_win32_rect *rect);

/*
 * Scales a length at 96 dpi to dpi.
 *
 * The product is exact in 64 bits and is rounded to the nearest whole
 * pixel, halves up; a result that does not fit 32 bits is UINT32_MAX.
 */
uint32_t
keiui_win32_scale(
	uint32_t value,
	unsigned dpi)
{
	uint64_t product;
	uint64_t scaled;
	uint64_t window_dpi;

	/* A window that does not know its dpi is taken at the base one. */
	window_dpi = dpi;
	if (window_dpi == 0U)
		window_dpi = FRAME_BASE_DPI;

	/* Multiplies in 64 bits, where 32 bit by 32 bit cannot overflow, and rounds half up. */
	product = (uint64_t)value * window_dpi;
	scaled = (product + FRAME_BASE_DPI / 2U) / FRAME_BASE_DPI;

	/* Saturates a result a 32 bit length cannot hold. */
	if (scaled > UINT32_MAX)
		return UINT32_MAX;

	/* Succeeded: the length in physical pixels. */
	return (uint32_t)scaled;
}

/*
 * Writes the rectangles of the three caption buttons.
 *
 * Without a title bar, or in full screen, all three are empty.
 */
void
keiui_win32_frame_buttons(
	const struct keiui_win32_frame *frame,
	struct keiui_win32_rect buttons[3])
{
	unsigned index;
	uint32_t button_width;
	uint32_t button_height;
	int64_t left;

	/* Writes nothing without both a frame and a place to write. */
	if (frame == NULL)
		return;
	if (buttons == NULL)
		return;

	/* Starts from three empty rectangles, which is the answer when there are no buttons. */
	for (index = 0; index < FRAME_BUTTON_COUNT; index++) {
		buttons[index].x = 0;
		buttons[index].y = 0;
		buttons[index].width = 0;
		buttons[index].height = 0;
	}

	/* Full screen draws no title bar. */
	if (frame->fullscreen != 0)
		return;

	/* A window without a title bar has no caption buttons. */
	if (frame->caption_height == 0U)
		return;

	/* Scales the size of one button to the window's dpi. */
	button_width = keiui_win32_scale(KEIUI_WIN32_BUTTON_WIDTH, frame->dpi);
	button_height = keiui_win32_scale(frame->caption_height, frame->dpi);

	/* Lays the buttons side by side ending at the right edge, minimize first and close last. */
	for (index = 0; index < FRAME_BUTTON_COUNT; index++) {
		left = (int64_t)frame->width - (int64_t)(FRAME_BUTTON_COUNT - index) * (int64_t)button_width;
		buttons[index].x = frame_clamp_int32(left);
		buttons[index].y = 0;
		buttons[index].width = button_width;
		buttons[index].height = button_height;
	}
}

/*
 * Answers what part of the window a point is on.
 *
 * The steps are tried in the order the header gives, and the first that
 * holds decides.
 */
unsigned
keiui_win32_frame_hit(
	const struct keiui_win32_frame *frame,
	int32_t x,
	int32_t y)
{
	struct keiui_win32_rect buttons[3];
	unsigned edge;
	unsigned index;
	int inside;
	int64_t caption;

	/* Without a frame there is nothing to hit. */
	if (frame == NULL)
		return KEIUI_WIN32_HIT_NOWHERE;

	/* A point outside the window is on no part of it. */
	if (x < 0 || (int64_t)x >= (int64_t)frame->width)
		return KEIUI_WIN32_HIT_NOWHERE;
	if (y < 0 || (int64_t)y >= (int64_t)frame->height)
		return KEIUI_WIN32_HIT_NOWHERE;

	/* Full screen has no frame: everything belongs to the client. */
	if (frame->fullscreen != 0)
		return KEIUI_WIN32_HIT_CLIENT;

	/* Places the caption buttons, which both the resize bands and the button step need. */
	keiui_win32_frame_buttons(frame, buttons);

	/* A maximized window has no resize bands; otherwise the bands come before every other part. */
	if (frame->maximized == 0) {
		edge = frame_edge_hit(frame, buttons, x, y);
		if (edge != FRAME_NO_EDGE)
			return edge;
	}

	/* The caption buttons, each answering with its own value. */
	inside = frame_rect_contains(&buttons[KEIUI_WIN32_BUTTON_CLOSE_INDEX], x, y);
	if (inside != 0)
		return KEIUI_WIN32_HIT_CLOSE;
	inside = frame_rect_contains(&buttons[KEIUI_WIN32_BUTTON_MAXIMIZE_INDEX], x, y);
	if (inside != 0)
		return KEIUI_WIN32_HIT_MAXIMIZE;
	inside = frame_rect_contains(&buttons[KEIUI_WIN32_BUTTON_MINIMIZE_INDEX], x, y);
	if (inside != 0)
		return KEIUI_WIN32_HIT_MINIMIZE;

	/* The application's own controls take the pointer even where they sit on the title bar. */
	if (frame->controls != NULL) {
		for (index = 0; index < frame->control_count; index++) {
			inside = frame_rect_contains(&frame->controls[index], x, y);
			if (inside != 0)
				return KEIUI_WIN32_HIT_CLIENT;
		}
	}

	/* The rest of the title bar drags the window. */
	caption = (int64_t)keiui_win32_scale(frame->caption_height, frame->dpi);
	if ((int64_t)y < caption)
		return KEIUI_WIN32_HIT_CAPTION;

	/* Everything else is the client area. */
	return KEIUI_WIN32_HIT_CLIENT;
}

/*
 * Lays out the tabs of a title bar.
 */
int32_t
keiui_win32_tabs_layout(
	const struct keiui_win32_tab_strip *strip,
	const int *closable,
	struct keiui_win32_tab_place *places,
	struct keiui_win32_rect *new_place)
{
	struct keiui_win32_tab_place *place;
	uint32_t height;
	uint32_t gap;
	uint32_t button;
	uint32_t most;
	uint32_t least;
	uint32_t close;
	uint32_t inset;
	uint32_t width;
	int64_t room;
	int64_t share;
	int64_t x;
	int32_t y;
	unsigned shown;
	unsigned first;
	unsigned index;

	/* Writes nothing without the strip and the places to write. */
	if (strip == NULL || new_place == NULL)
		return 0;
	if (strip->count != 0U && (places == NULL || closable == NULL))
		return strip->left;

	/* The lengths at the window's dpi. */
	height = keiui_win32_scale(KEIUI_WIN32_TAB_HEIGHT, strip->dpi);
	gap = keiui_win32_scale(KEIUI_WIN32_TAB_GAP, strip->dpi);
	most = keiui_win32_scale(KEIUI_WIN32_TAB_MOST, strip->dpi);
	least = keiui_win32_scale(KEIUI_WIN32_TAB_LEAST, strip->dpi);
	close = keiui_win32_scale(KEIUI_WIN32_TAB_CLOSE, strip->dpi);
	inset = keiui_win32_scale(KEIUI_WIN32_TAB_CLOSE_INSET, strip->dpi);
	y = (int32_t)(((int64_t)strip->caption - (int64_t)height) / 2);

	/* The room the tabs share: the strip less the new tab's button and its gap. */
	button = 0U;
	if (strip->new_button)
		button = height;
	room = (int64_t)strip->right - (int64_t)strip->left;
	if (strip->new_button)
		room -= (int64_t)button + (int64_t)gap;
	if (room < 0)
		room = 0;

	/* Each tab's width: the room's share less its gap, up to the most. */
	width = 0U;
	shown = 0U;
	if (strip->count != 0U) {
		share = room / (int64_t)strip->count - (int64_t)gap;
		if (share > (int64_t)most)
			share = (int64_t)most;
		width = (uint32_t)least;
		shown = strip->count;
		if (share >= (int64_t)least)
			width = (uint32_t)share;
	}

	/* Too many even at the least: as many as fit. */
	if (width == least && strip->count != 0U && (int64_t)strip->count * ((int64_t)least + (int64_t)gap) > room) {
		shown = (unsigned)(room / ((int64_t)least + (int64_t)gap));
		if (shown > strip->count)
			shown = strip->count;
	}

	/* The first tab shown: the first one, or the one that ends the shown run at the active tab. */
	first = 0U;
	if (strip->active < strip->count && shown != 0U && strip->active >= shown)
		first = strip->active - shown + 1U;

	/* Each tab, side by side from the strip's left. */
	x = strip->left;
	for (index = 0U; index < strip->count; index++) {
		/* A tab not among those shown has an empty place. */
		place = &places[index];
		frame_rect_empty(&place->tab);
		frame_rect_empty(&place->close);
		place->shown = 0;
		if (index < first || index >= first + shown)
			continue;

		/* The tab. */
		place->shown = 1;
		place->tab.x = frame_clamp_int32(x);
		place->tab.y = y;
		place->tab.width = width;
		place->tab.height = height;

		/* Its close button, at its right in the middle of its height. */
		if (closable[index] && width > close + inset) {
			place->close.x = frame_clamp_int32(x + (int64_t)width - (int64_t)inset - (int64_t)close);
			place->close.y = y + (int32_t)((height - close) / 2U);
			place->close.width = close;
			place->close.height = close;
		}

		/* The next tab after a gap. */
		x += (int64_t)width + (int64_t)gap;
	}

	/* The new tab's button after the tabs, or nothing. */
	frame_rect_empty(new_place);
	if (!strip->new_button) {
		if (shown == 0U)
			return strip->left;
		return frame_clamp_int32(x - (int64_t)gap);
	}

	/* Places the button after the last tab shown. */
	new_place->x = frame_clamp_int32(x);
	new_place->y = y;
	new_place->width = button;
	new_place->height = height;

	/* Succeeded: the strip ends at the button's right edge. */
	return frame_clamp_int32(x + (int64_t)button);
}

/*
 * Tells what a point is on among the tabs laid out.
 */
unsigned
keiui_win32_tabs_at(
	const struct keiui_win32_tab_place *places,
	unsigned count,
	const struct keiui_win32_rect *new_place,
	int32_t x,
	int32_t y,
	unsigned *part)
{
	unsigned index;
	int inside;

	/* Somewhere to answer, and the places. */
	if (part == NULL)
		return KEIUI_WIN32_TAB_NONE;
	if (count != 0U && places == NULL)
		return KEIUI_WIN32_TAB_NONE;

	/* Each tab shown: its close button first, then the tab. */
	for (index = 0U; index < count; index++) {
		if (!places[index].shown)
			continue;

		/* The close button, which is inside the tab. */
		inside = frame_rect_contains(&places[index].close, x, y);
		if (inside) {
			*part = KEIUI_WIN32_TAB_PART_CLOSE;
			return index;
		}

		/* The rest of the tab. */
		inside = frame_rect_contains(&places[index].tab, x, y);
		if (inside) {
			*part = KEIUI_WIN32_TAB_PART_BODY;
			return index;
		}
	}

	/* The new tab's button. */
	if (new_place != NULL) {
		inside = frame_rect_contains(new_place, x, y);
		if (inside) {
			*part = KEIUI_WIN32_TAB_PART_NEW;
			return count;
		}
	}

	/* Nothing of the strip. */
	return KEIUI_WIN32_TAB_NONE;
}

/*
 * Limits a 64 bit coordinate to the range of int32_t.
 */
static int32_t
frame_clamp_int32(
	int64_t value)
{
	/* A coordinate below the range is the lowest one. */
	if (value < (int64_t)INT32_MIN)
		return INT32_MIN;

	/* A coordinate above the range is the highest one. */
	if (value > (int64_t)INT32_MAX)
		return INT32_MAX;

	/* Succeeded: the coordinate already fits. */
	return (int32_t)value;
}

/*
 * Answers 1 when the point is inside the rectangle, 0 when it is not.
 *
 * The edges are compared in 64 bits, so a rectangle at the end of the
 * coordinate range does not wrap; an empty rectangle contains nothing.
 */
static int
frame_rect_contains(
	const struct keiui_win32_rect *rect,
	int32_t x,
	int32_t y)
{
	int64_t right;
	int64_t bottom;

	/* Finds the first coordinate past the rectangle in each direction. */
	right = (int64_t)rect->x + (int64_t)rect->width;
	bottom = (int64_t)rect->y + (int64_t)rect->height;

	/* A point left of or right of the rectangle is outside it. */
	if ((int64_t)x < (int64_t)rect->x)
		return 0;
	if ((int64_t)x >= right)
		return 0;

	/* A point above or below the rectangle is outside it. */
	if ((int64_t)y < (int64_t)rect->y)
		return 0;
	if ((int64_t)y >= bottom)
		return 0;

	/* Succeeded: the point is inside. */
	return 1;
}

/*
 * Tells which end of an axis a coordinate is near.
 *
 * Answers -1 for the low end (coordinate below low_reach), 1 for the high
 * end (within high_reach of extent) and 0 for neither.  When both hold,
 * as in a window smaller than the two reaches together, the nearer end
 * wins and the low end wins a tie.
 */
static int
frame_pick_side(
	int64_t coordinate,
	int64_t extent,
	int64_t low_reach,
	int64_t high_reach)
{
	int low;
	int high;

	/* Tests the coordinate against each end of the axis. */
	low = 0;
	if (coordinate < low_reach)
		low = 1;
	high = 0;
	if (coordinate + high_reach >= extent)
		high = 1;

	/* Near both ends: the nearer one decides, so the choice does not depend on the order of the tests. */
	if (low != 0 && high != 0) {
		if (coordinate * 2 <= extent - 1)
			return -1;
		return 1;
	}

	/* Near only the low end. */
	if (low != 0)
		return -1;

	/* Near only the high end. */
	if (high != 0)
		return 1;

	/* Succeeded: near neither end. */
	return 0;
}

/*
 * Answers the resize part a point is on, or FRAME_NO_EDGE.
 *
 * The bands lie inside the four edges.  The top band is thinner where it
 * crosses a caption button so that the button can be reached from its
 * top row.  A point on a band that is also close to the end of that
 * band's edge resizes the corner instead.
 */
static unsigned
frame_edge_hit(
	const struct keiui_win32_frame *frame,
	const struct keiui_win32_rect buttons[3],
	int32_t x,
	int32_t y)
{
	int64_t border;
	int64_t top_border;
	int64_t corner;
	int64_t point_x;
	int64_t point_y;
	int side_x;
	int side_y;
	unsigned index;

	/* Scales the lengths of the bands to the window's dpi. */
	border = (int64_t)keiui_win32_scale(KEIUI_WIN32_RESIZE_BORDER, frame->dpi);
	top_border = border;
	corner = (int64_t)keiui_win32_scale(KEIUI_WIN32_RESIZE_CORNER, frame->dpi);
	point_x = x;
	point_y = y;

	/* Thins the top band where the point's column belongs to a caption button. */
	for (index = 0; index < FRAME_BUTTON_COUNT; index++) {
		if (point_x < (int64_t)buttons[index].x)
			continue;
		if (point_x >= (int64_t)buttons[index].x + (int64_t)buttons[index].width)
			continue;
		top_border = (int64_t)keiui_win32_scale(KEIUI_WIN32_TOP_BORDER, frame->dpi);
	}

	/* Finds the sides whose band holds the point. */
	side_x = frame_pick_side(point_x, (int64_t)frame->width, border, border);
	side_y = frame_pick_side(point_y, (int64_t)frame->height, top_border, border);

	/* A point in no band is not on an edge. */
	if (side_x == 0 && side_y == 0)
		return FRAME_NO_EDGE;

	/* A point in a vertical band only becomes a corner when it is within the corner's reach of an end. */
	if (side_y == 0)
		side_y = frame_pick_side(point_y, (int64_t)frame->height, corner, corner);

	/* A point in a horizontal band only becomes a corner when it is within the corner's reach of an end. */
	if (side_x == 0)
		side_x = frame_pick_side(point_x, (int64_t)frame->width, corner, corner);

	/* Succeeded: the resize part for the sides found. */
	return frame_edge_table[side_y + 1][side_x + 1];
}

/* Makes a rectangle empty at the origin. */
static void
frame_rect_empty(
	struct keiui_win32_rect *rect)
{
	/* Nowhere, of no size. */
	rect->x = 0;
	rect->y = 0;
	rect->width = 0U;
	rect->height = 0U;
}
