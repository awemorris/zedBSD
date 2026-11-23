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
#include <string.h>

/* The dpi at which the lengths of the header are written; dpi 0 means this. */
#define FRAME_BASE_DPI	96U

/* The number of caption buttons. */
#define FRAME_BUTTON_COUNT	3U

/* What the resize-band step answers when the point is on no band (not a WM_NCHITTEST value). */
#define FRAME_NO_EDGE	0xFFFFFFFFU

/*
 * The lengths of a tab strip's parts at its dpi, scaled once for a layout:
 * a tab's height and a button's side (the same), the gap, a tab's most and
 * least widths, a close button's side and inset, and the row's top.
 */
struct frame_strip_lengths {
	uint32_t height;
	uint32_t button;
	uint32_t gap;
	uint32_t most;
	uint32_t least;
	uint32_t close;
	uint32_t inset;
	int32_t y;
};

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
static void frame_strip_lengths(const struct keiui_win32_tab_strip *strip, struct frame_strip_lengths *lengths);
static int32_t frame_strip_all(const struct keiui_win32_tab_strip *strip, const int *closable, struct keiui_win32_tab_place *places, struct keiui_win32_rect *new_place, struct keiui_win32_tab_scroll *scroll, const struct frame_strip_lengths *lengths);
static int frame_strip_scroll(const struct keiui_win32_tab_strip *strip, const int *closable, struct keiui_win32_tab_place *places, struct keiui_win32_rect *new_place, struct keiui_win32_tab_scroll *scroll, const struct frame_strip_lengths *lengths, int32_t *end);
static int32_t frame_strip_list(const struct keiui_win32_tab_strip *strip, struct keiui_win32_rect *new_place, struct keiui_win32_tab_scroll *scroll, const struct frame_strip_lengths *lengths);
static void frame_tab_put(struct keiui_win32_tab_place *place, int64_t x, uint32_t width, int closable, const struct frame_strip_lengths *lengths);
static void frame_square(struct keiui_win32_rect *rect, int64_t x, const struct frame_strip_lengths *lengths);

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
 * Lays out the parts of a title bar its controls and tabs are laid out in.
 */
void
keiui_win32_title_room(
	const struct keiui_win32_frame *frame,
	uint32_t height,
	int has_menu,
	struct keiui_win32_title_room *room)
{
	struct keiui_win32_rect buttons[3];
	uint32_t size;
	uint32_t gap;
	int32_t margin;
	int32_t drag;

	/* Writes nothing without a frame and a place to write. */
	if (frame == NULL)
		return;
	if (room == NULL)
		return;

	/* The caption buttons, before which the room ends. */
	keiui_win32_frame_buttons(frame, buttons);

	/* The lengths at the window's dpi. */
	size = keiui_win32_scale(KEIUI_WIN32_TITLE_CONTROL, frame->dpi);
	gap = keiui_win32_scale(KEIUI_WIN32_TITLE_GAP, frame->dpi);
	margin = (int32_t)keiui_win32_scale(KEIUI_WIN32_TITLE_SIDE, frame->dpi);
	drag = (int32_t)keiui_win32_scale(KEIUI_WIN32_TITLE_DRAG_LEAST, frame->dpi);

	/* The room from the left margin to the margin before the minimize button. */
	frame_rect_empty(&room->menu);
	room->left = margin;
	room->right = frame_clamp_int32((int64_t)buttons[KEIUI_WIN32_BUTTON_MINIMIZE_INDEX].x - (int64_t)margin);

	/* The menu's button first at the left, in the middle of the bar's height, and the room after it. */
	if (has_menu) {
		room->menu.x = room->left;
		room->menu.y = (int32_t)(((int64_t)height - (int64_t)size) / 2);
		room->menu.width = size;
		room->menu.height = size;
		room->left += (int32_t)(size + gap);
	}

	/* The strip ends where the bare title bar to drag the window by starts, never left of its start. */
	room->strip_right = frame_clamp_int32((int64_t)room->right - (int64_t)drag);
	if (room->strip_right < room->left)
		room->strip_right = room->left;
}

/*
 * Lays out the tabs of a title bar, scrolling the strip when they do not
 * fit.
 *
 * The way is chosen in order: every tab when one tab or the least widths
 * of all fit, else the scrolling strip when one tab fits between its
 * arrows, else the list's button alone.
 */
int32_t
keiui_win32_tabs_layout_scroll(
	const struct keiui_win32_tab_strip *strip,
	const int *closable,
	struct keiui_win32_tab_place *places,
	struct keiui_win32_rect *new_place,
	struct keiui_win32_tab_scroll *scroll)
{
	struct keiui_win32_tab_scroll unasked;
	struct frame_strip_lengths lengths;
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
	if (strip->count != 0U &&
	    (places == NULL ||
	     closable == NULL))
		return strip->left;

	/* How the strip was laid out goes somewhere even when the caller does not ask. */
	if (scroll == NULL)
		scroll = &unasked;

	/* Starts from nothing shown: no tab, no button, no arrow. */
	memset(scroll, 0, sizeof(*scroll));
	frame_rect_empty(new_place);
	for (index = 0U; index < strip->count; index++) {
		frame_rect_empty(&places[index].tab);
		frame_rect_empty(&places[index].close);
		places[index].shown = 0;
	}

	/* The lengths at the strip's dpi. */
	frame_strip_lengths(strip, &lengths);

	/* The room the tabs share beside the new tab's button. */
	room = (int64_t)strip->right - (int64_t)strip->left;
	if (strip->new_button)
		room -= (int64_t)lengths.button + (int64_t)lengths.gap;
	if (room < 0)
		room = 0;

	/* No tab or one: the tabs side by side, which leave out a single tab that does not fit. */
	if (strip->count < 2U) {
		end = frame_strip_all(strip, closable, places, new_place, scroll, &lengths);
		return end;
	}

	/* Every tab at its share, when the share is not below the least. */
	share = room / (int64_t)strip->count - (int64_t)lengths.gap;
	if (share >= (int64_t)lengths.least) {
		end = frame_strip_all(strip, closable, places, new_place, scroll, &lengths);
		return end;
	}

	/* The strip scrolls when one tab fits between its arrows. */
	fits = frame_strip_scroll(strip, closable, places, new_place, scroll, &lengths, &end);
	if (fits)
		return end;

	/* Succeeded: only the list's button (and the new tab's) on a strip too narrow for a tab. */
	end = frame_strip_list(strip, new_place, scroll, &lengths);
	return end;
}

/*
 * Lays out the tabs of a title bar, always showing the active tab.
 */
int32_t
keiui_win32_tabs_layout(
	const struct keiui_win32_tab_strip *strip,
	const int *closable,
	struct keiui_win32_tab_place *places,
	struct keiui_win32_rect *new_place)
{
	struct keiui_win32_tab_strip revealing;
	int32_t end;

	/* Writes nothing without a strip. */
	if (strip == NULL)
		return 0;

	/* The same strip that brings its active tab into sight. */
	revealing = *strip;
	revealing.reveal = 1;

	/* Succeeded: the tabs laid out, the strip's end. */
	end = keiui_win32_tabs_layout_scroll(&revealing, closable, places, new_place, NULL);
	return end;
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

	/* Succeeded: the tabs and the new tab's button, without arrows. */
	index = keiui_win32_tabs_at_scroll(places, count, new_place, NULL, x, y, part);
	return index;
}

/*
 * Tells what a point is on among the tabs laid out, the arrows and the
 * list's button included.
 */
unsigned
keiui_win32_tabs_at_scroll(
	const struct keiui_win32_tab_place *places,
	unsigned count,
	const struct keiui_win32_rect *new_place,
	const struct keiui_win32_tab_scroll *scroll,
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

	/* No arrow nor list without the strip's scroll. */
	if (scroll == NULL)
		return KEIUI_WIN32_TAB_NONE;

	/* The arrow back, while it can move the strip. */
	if (scroll->back_live) {
		inside = frame_rect_contains(&scroll->back, x, y);
		if (inside) {
			*part = KEIUI_WIN32_TAB_PART_BACK;
			return count;
		}
	}

	/* The arrow forward, while it can move the strip. */
	if (scroll->forward_live) {
		inside = frame_rect_contains(&scroll->forward, x, y);
		if (inside) {
			*part = KEIUI_WIN32_TAB_PART_FORWARD;
			return count;
		}
	}

	/* The list's button. */
	inside = frame_rect_contains(&scroll->list, x, y);
	if (inside) {
		*part = KEIUI_WIN32_TAB_PART_LIST;
		return count;
	}

	/* Nothing of the strip. */
	return KEIUI_WIN32_TAB_NONE;
}

/*
 * Decides a wheel's turn at a point of a frame: the content's, or the
 * title bar's and how many tabs it moves the strip.
 */
unsigned
keiui_win32_tab_wheel(
	int on_frame,
	int32_t y,
	uint32_t bar,
	int on_strip,
	int scrolling,
	int horizontal,
	int delta,
	int *pending,
	int *steps)
{
	int toward;

	/* Nowhere to write: the content's, as before the strip had a wheel. */
	if (pending == NULL || steps == NULL)
		return KEIUI_WIN32_WHEEL_CONTENT;
	*steps = 0;

	/* A point over the content, or over another window, is the content's. */
	if (!on_frame) {
		*pending = 0;
		return KEIUI_WIN32_WHEEL_CONTENT;
	}

	/* A point of the frame below the title bar (or above it) is the content's too. */
	if (y < 0 || (int64_t)y >= (int64_t)bar) {
		*pending = 0;
		return KEIUI_WIN32_WHEEL_CONTENT;
	}

	/* Elsewhere on the title bar than a scrolling strip's part, the turn is lost. */
	if (!on_strip || !scrolling) {
		*pending = 0;
		return KEIUI_WIN32_WHEEL_TITLE;
	}

	/*
	 * The turn toward the tabs after: the vertical wheel's delta is
	 * positive turned away from the user (up, the tabs before), the
	 * horizontal one's to the right (the tabs after).
	 */
	toward = delta;
	if (!horizontal)
		toward = -delta;

	/* A turn the other way forgets what the last turns left. */
	if (*pending > 0 && toward < 0)
		*pending = 0;
	if (*pending < 0 && toward > 0)
		*pending = 0;

	/* Adds the turn, then takes each whole notch as a tab after or before. */
	*pending += toward;
	while (*pending >= KEIUI_WIN32_WHEEL_NOTCH) {
		*pending -= KEIUI_WIN32_WHEEL_NOTCH;
		(*steps)++;
	}
	while (*pending <= -KEIUI_WIN32_WHEEL_NOTCH) {
		*pending += KEIUI_WIN32_WHEEL_NOTCH;
		(*steps)--;
	}

	/* Succeeded: the title bar's turn, and the tabs it moves the strip by. */
	return KEIUI_WIN32_WHEEL_TITLE;
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

/*
 * Scales the lengths of a strip's parts to its dpi, and places their row
 * in the middle of the caption's height.
 */
static void
frame_strip_lengths(
	const struct keiui_win32_tab_strip *strip,
	struct frame_strip_lengths *lengths)
{
	/* Each length at the strip's dpi; a button is a square of a tab's height. */
	lengths->height = keiui_win32_scale(KEIUI_WIN32_TAB_HEIGHT, strip->dpi);
	lengths->button = lengths->height;
	lengths->gap = keiui_win32_scale(KEIUI_WIN32_TAB_GAP, strip->dpi);
	lengths->most = keiui_win32_scale(KEIUI_WIN32_TAB_MOST, strip->dpi);
	lengths->least = keiui_win32_scale(KEIUI_WIN32_TAB_LEAST, strip->dpi);
	lengths->close = keiui_win32_scale(KEIUI_WIN32_TAB_CLOSE, strip->dpi);
	lengths->inset = keiui_win32_scale(KEIUI_WIN32_TAB_CLOSE_INSET, strip->dpi);

	/* The row's top, in the middle of the caption's height. */
	lengths->y = (int32_t)(((int64_t)strip->caption - (int64_t)lengths->height) / 2);
}

/* Lays out every tab side by side at one width (KEIUI_WIN32_STRIP_ALL); returns where the strip ends. */
static int32_t
frame_strip_all(
	const struct keiui_win32_tab_strip *strip,
	const int *closable,
	struct keiui_win32_tab_place *places,
	struct keiui_win32_rect *new_place,
	struct keiui_win32_tab_scroll *scroll,
	const struct frame_strip_lengths *lengths)
{
	uint32_t width;
	int64_t room;
	int64_t share;
	int64_t x;
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
		frame_tab_put(&places[index], x, width, closable[index], lengths);
		x += (int64_t)width + (int64_t)lengths->gap;
	}

	/* How the strip shows its tabs: all of them from the first, or none. */
	scroll->mode = KEIUI_WIN32_STRIP_ALL;
	scroll->first = 0U;
	scroll->shown = shown;

	/* The new tab's button after the tabs, when asked for and when it fits before the strip's right. */
	if (strip->new_button &&
	    x + (int64_t)lengths->button <= (int64_t)strip->right) {
		frame_square(new_place, x, lengths);
		return frame_clamp_int32(x + (int64_t)lengths->button);
	}

	/* Without the button: nothing shown ends at the left. */
	if (shown == 0U)
		return strip->left;

	/* Succeeded: the strip ends at the last tab's right edge. */
	return frame_clamp_int32(x - (int64_t)lengths->gap);
}

/*
 * Lays out a scrolling strip (KEIUI_WIN32_STRIP_SCROLL) and writes where
 * it ends.  Returns 0, with nothing written, when not even one tab fits
 * between the arrows.
 */
static int
frame_strip_scroll(
	const struct keiui_win32_tab_strip *strip,
	const int *closable,
	struct keiui_win32_tab_place *places,
	struct keiui_win32_rect *new_place,
	struct keiui_win32_tab_scroll *scroll,
	const struct frame_strip_lengths *lengths,
	int32_t *end)
{
	int64_t list_x;
	int64_t new_x;
	int64_t forward_x;
	int64_t tabs_left;
	int64_t span;
	int64_t width;
	int64_t x;
	unsigned shown;
	unsigned first;
	unsigned index;

	/* From the right: the list's button at the strip's end, the new tab's button and the arrow forward before it. */
	list_x = (int64_t)strip->right - (int64_t)lengths->button;
	new_x = list_x;
	if (strip->new_button)
		new_x = list_x - (int64_t)lengths->gap - (int64_t)lengths->button;
	forward_x = new_x - (int64_t)lengths->gap - (int64_t)lengths->button;

	/* The tabs' room between the arrow back at the strip's left and the arrow forward. */
	tabs_left = (int64_t)strip->left + (int64_t)lengths->button + (int64_t)lengths->gap;
	span = forward_x - (int64_t)lengths->gap - tabs_left;
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

	/* The tabs in sight, side by side between the arrows. */
	x = tabs_left;
	for (index = first; index < first + shown; index++) {
		frame_tab_put(&places[index], x, (uint32_t)width, closable[index], lengths);
		x += width + (int64_t)lengths->gap;
	}

	/* The arrows at both ends, each live while tabs lie out of sight that way. */
	frame_square(&scroll->back, strip->left, lengths);
	frame_square(&scroll->forward, forward_x, lengths);
	scroll->back_live = 0;
	if (first > 0U)
		scroll->back_live = 1;
	scroll->forward_live = 0;
	if (first + shown < strip->count)
		scroll->forward_live = 1;

	/* The new tab's button, when asked for, and the list's button at the end. */
	if (strip->new_button)
		frame_square(new_place, new_x, lengths);
	frame_square(&scroll->list, list_x, lengths);

	/* How the strip shows its tabs. */
	scroll->mode = KEIUI_WIN32_STRIP_SCROLL;
	scroll->first = first;
	scroll->shown = shown;

	/* Succeeded: the strip ends at the list's button, its right. */
	*end = strip->right;
	return 1;
}

/* Lays out a strip too narrow for a tab (KEIUI_WIN32_STRIP_LIST); returns where it ends. */
static int32_t
frame_strip_list(
	const struct keiui_win32_tab_strip *strip,
	struct keiui_win32_rect *new_place,
	struct keiui_win32_tab_scroll *scroll,
	const struct frame_strip_lengths *lengths)
{
	int64_t both;
	int64_t x;

	/* No tab is shown; the list holds them all. */
	scroll->mode = KEIUI_WIN32_STRIP_LIST;
	scroll->first = 0U;
	scroll->shown = 0U;

	/* The new tab's button first, when asked for and when the list's button fits after it. */
	x = strip->left;
	both = 2 * (int64_t)lengths->button + (int64_t)lengths->gap;
	if (strip->new_button && x + both <= (int64_t)strip->right) {
		frame_square(new_place, x, lengths);
		x += (int64_t)lengths->button + (int64_t)lengths->gap;
	}

	/* Nothing at all when not even the list's button fits. */
	if (x + (int64_t)lengths->button > (int64_t)strip->right)
		return strip->left;

	/* Succeeded: the list's button, where the strip ends. */
	frame_square(&scroll->list, x, lengths);
	return frame_clamp_int32(x + (int64_t)lengths->button);
}

/* Places one tab at x of a width, with its close button when it is closable. */
static void
frame_tab_put(
	struct keiui_win32_tab_place *place,
	int64_t x,
	uint32_t width,
	int closable,
	const struct frame_strip_lengths *lengths)
{
	/* The tab, in the strip's row. */
	place->shown = 1;
	place->tab.x = frame_clamp_int32(x);
	place->tab.y = lengths->y;
	place->tab.width = width;
	place->tab.height = lengths->height;

	/* Its close button, at its right in the middle of its height, when there is room for it. */
	if (closable && width > lengths->close + lengths->inset) {
		place->close.x = frame_clamp_int32(x + (int64_t)width - (int64_t)lengths->inset - (int64_t)lengths->close);
		place->close.y = lengths->y + (int32_t)((lengths->height - lengths->close) / 2U);
		place->close.width = lengths->close;
		place->close.height = lengths->close;
	}
}

/* Places a button of the strip (a square of a tab's height) at x in the strip's row. */
static void
frame_square(
	struct keiui_win32_rect *rect,
	int64_t x,
	const struct frame_strip_lengths *lengths)
{
	/* The square. */
	rect->x = frame_clamp_int32(x);
	rect->y = lengths->y;
	rect->width = lengths->button;
	rect->height = lengths->button;
}
