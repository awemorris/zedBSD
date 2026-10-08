/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch of a view of editable text (ws090-p003, plan/ws090/design.md
 * section 6.1; the user's decision of 2026-09-29: in a text view one
 * finger selects and two fingers scroll).
 *
 * This file keeps the selection the fingers make: a tap puts the caret, a
 * double tap selects a word, one finger's drag selects from where it
 * touched, and a drag that starts on a handle moves that end.  Which
 * finger's gesture is which is kl_ui's (ui.c); the positions are the
 * view's (its three answers).  All points here are in the view's content
 * coordinates except the context menu's place, which is the window's.
 *
 * KL_VERSION 74 (ws190-p002, plan/ws190/phase001/phase.md section 2.1):
 * a selection the fingers made shows the bar of editing buttons (bar) when
 * the fingers are off it: after a double tap, and after a drag that left a
 * selection; a drag hides it while it lasts, a tap, the keys' selection and
 * a long press take it away.
 */

#include "internal.h"

#include <math.h>
#include <string.h>

static int touch_near_handle(const struct kl_text_touch *touch, size_t position, double x, double y);
static void touch_grip(struct kl_text_touch *touch, double x, double y);
static double touch_edge_speed(double place, double size);
static void touch_set_bar(struct kl_text_touch *touch, int bar);

/*
 * Makes a text view's touch, with the caret at position 0.
 */
void
kl_text_touch_init(
	struct kl_text_touch *touch,
	const struct kl_text_view *view,
	void *data)
{
	/* Nothing selected, no finger. */
	memset(touch, 0, sizeof(*touch));
	touch->view = view;
	touch->data = data;
}

/*
 * Takes the selection the view has now (a key or the pointer changed it):
 * the handles go, since the selection is no longer the fingers'.
 */
void
kl_text_touch_set_selection(
	struct kl_text_touch *touch,
	size_t anchor,
	size_t caret)
{
	/* The view's selection, without handles. */
	touch->anchor = anchor;
	touch->caret = caret;
	touch->handles = 0;
	touch->handle = KL_TEXT_HANDLE_NONE;
	touch_set_bar(touch, 0);
}

/*
 * A tap at a point: the caret there, or (twice) the word there selected
 * with its handles.
 */
void
kl_text_touch_tap(
	struct kl_text_touch *touch,
	double x,
	double y,
	int twice)
{
	size_t position;
	size_t start;
	size_t end;

	/* The position under the finger. */
	position = touch->view->position_at(touch->data, x, y);
	touch->selecting = 0;
	touch->handle = KL_TEXT_HANDLE_NONE;

	/* Once: the caret there, nothing selected, no bar. */
	if (!twice) {
		touch->anchor = position;
		touch->caret = position;
		touch->handles = 0;
		touch->changes |= KL_TEXT_TOUCH_SELECTION;
		touch_set_bar(touch, 0);
		return;
	}

	/* Twice: the word there, its ends shown with handles. */
	start = position;
	end = position;
	touch->view->word_at(touch->data, position, &start, &end);
	touch->anchor = start;
	touch->caret = end;
	touch->handles = 0;
	if (end != start)
		touch->handles = 1;
	touch->changes |= KL_TEXT_TOUCH_SELECTION;

	/* The bar shows, even at a caret alone (Paste, Select All). */
	touch_set_bar(touch, 1);
}

/*
 * A long press: the view is asked for its context menu at the finger (a
 * place of the window).
 */
void
kl_text_touch_long_press(
	struct kl_text_touch *touch,
	double window_x,
	double window_y)
{
	/* The request and its place; the menu takes the bar's place. */
	touch->menu_x = window_x;
	touch->menu_y = window_y;
	touch->changes |= KL_TEXT_TOUCH_MENU;
	touch_set_bar(touch, 0);
}

/*
 * One finger's drag begins at a point: on a handle it moves that end,
 * anywhere else it selects from the point.
 */
void
kl_text_touch_drag_begin(
	struct kl_text_touch *touch,
	double x,
	double y)
{
	size_t other;
	int near;

	/* The finger, with no grip on a handle yet. */
	touch->finger_x = x;
	touch->finger_y = y;
	touch->selecting = 1;
	touch->handle = KL_TEXT_HANDLE_NONE;
	touch->grip_x = 0.0;
	touch->grip_y = 0.0;

	/* The bar hides while a finger drags. */
	touch_set_bar(touch, 0);

	/* A handle under the finger: the caret is always the end that moves, so the anchor is the other one. */
	if (touch->handles) {
		near = touch_near_handle(touch, touch->caret, x, y);
		if (near) {
			touch->handle = KL_TEXT_HANDLE_CARET;
			touch_grip(touch, x, y);
			return;
		}

		/* The anchor's handle: the ends change places. */
		near = touch_near_handle(touch, touch->anchor, x, y);
		if (near) {
			other = touch->caret;
			touch->caret = touch->anchor;
			touch->anchor = other;
			touch->handle = KL_TEXT_HANDLE_ANCHOR;
			touch_grip(touch, x, y);
			return;
		}
	}

	/* Anywhere else: a new selection from the point. */
	touch->anchor = touch->view->position_at(touch->data, x, y);
	touch->caret = touch->anchor;
	touch->handles = 0;
	touch->changes |= KL_TEXT_TOUCH_SELECTION;
}

/*
 * The selecting finger is at a point: the caret follows it.
 */
void
kl_text_touch_drag(
	struct kl_text_touch *touch,
	double x,
	double y)
{
	size_t position;

	/* Only while a finger selects. */
	if (!touch->selecting)
		return;

	/* The caret where the finger is (less its grip on a handle). */
	touch->finger_x = x;
	touch->finger_y = y;
	position = touch->view->position_at(touch->data, x - touch->grip_x, y - touch->grip_y);
	if (position == touch->caret)
		return;
	touch->caret = position;
	touch->changes |= KL_TEXT_TOUCH_SELECTION;
}

/*
 * The selecting finger lifts: a selection it made keeps its handles.
 */
void
kl_text_touch_drag_end(
	struct kl_text_touch *touch)
{
	/* Only a finger that selected. */
	if (!touch->selecting)
		return;

	/* The drag is over; a selection (not a caret) shows its handles and the bar. */
	touch->selecting = 0;
	touch->handle = KL_TEXT_HANDLE_NONE;
	touch->handles = 0;
	if (touch->anchor != touch->caret)
		touch->handles = 1;
	touch_set_bar(touch, touch->handles);
}

/*
 * Puts a selection as the fingers' own (KL_VERSION 74, Select All of the
 * bar): with its handles when it is not empty, and the bar shown.
 */
void
kl_text_touch_select(
	struct kl_text_touch *touch,
	size_t anchor,
	size_t caret)
{
	/* The selection, no finger on it. */
	touch->anchor = anchor;
	touch->caret = caret;
	touch->selecting = 0;
	touch->handle = KL_TEXT_HANDLE_NONE;
	touch->handles = 0;
	if (anchor != caret)
		touch->handles = 1;
	touch->changes |= KL_TEXT_TOUCH_SELECTION;

	/* The bar over it. */
	touch_set_bar(touch, 1);
}

/*
 * Hides the bar and keeps the selection and its handles (KL_VERSION 74,
 * after Copy).
 */
void
kl_text_touch_hide_bar(
	struct kl_text_touch *touch)
{
	/* No bar. */
	touch_set_bar(touch, 0);
}

/*
 * Shows the bar when it is hidden and hides it when it shows (KL_VERSION
 * 74, a tap inside the selection).
 */
void
kl_text_touch_toggle_bar(
	struct kl_text_touch *touch)
{
	/* The other state. */
	if (touch->bar) {
		touch_set_bar(touch, 0);
		return;
	}

	/* Shown. */
	touch_set_bar(touch, 1);
}

/*
 * Begins a finger's drag of a named end's handle (ws190-p002: kl_ui knows
 * which knob the finger touched, so the reach is not looked at again).  The
 * caret is the end that moves, so the anchor's handle swaps the ends.
 */
void
keiui_text_touch_hold(
	struct kl_text_touch *touch,
	int end,
	double x,
	double y)
{
	size_t other;

	/* The finger, the bar hidden while it drags. */
	touch->finger_x = x;
	touch->finger_y = y;
	touch->selecting = 1;
	touch->grip_x = 0.0;
	touch->grip_y = 0.0;
	touch_set_bar(touch, 0);

	/* The anchor's handle: the ends change places. */
	touch->handle = KL_TEXT_HANDLE_CARET;
	if (end == KL_TEXT_HANDLE_ANCHOR) {
		other = touch->caret;
		touch->caret = touch->anchor;
		touch->anchor = other;
		touch->handle = KL_TEXT_HANDLE_ANCHOR;
	}

	/* The grip on the caret's handle. */
	touch_grip(touch, x, y);
}

/*
 * Reports how fast the content should scroll by itself while a selecting
 * finger is near the viewport's edge (pixels a second on each axis, 0 away
 * from the edges).  Returns 1 when it should scroll.
 */
int
kl_text_touch_edge(
	const struct kl_text_touch *touch,
	const struct kl_scroll *scroll,
	double *vx,
	double *vy)
{
	/* Nothing while no finger selects. */
	*vx = 0.0;
	*vy = 0.0;
	if (!touch->selecting)
		return 0;

	/* Each axis the scroll moves along, by how near the finger is to its edges in the viewport. */
	if ((scroll->axes & KL_SCROLL_X) != 0U)
		*vx = touch_edge_speed(touch->finger_x - scroll->x, scroll->viewport_width);
	if ((scroll->axes & KL_SCROLL_Y) != 0U)
		*vy = touch_edge_speed(touch->finger_y - scroll->y, scroll->viewport_height);

	/* Away from the edges. */
	if (*vx == 0.0 && *vy == 0.0)
		return 0;

	/* Succeeded: the content should scroll. */
	return 1;
}

/*
 * Reports what the fingers changed since the last call
 * (KL_TEXT_TOUCH_* bits), and forgets it.
 */
unsigned
kl_text_touch_take(
	struct kl_text_touch *touch)
{
	unsigned changes;

	/* The changes, then none. */
	changes = touch->changes;
	touch->changes = 0;

	/* Reports them. */
	return changes;
}

/*
 * Draws the handles of a selection the fingers made: a small round knob
 * under each end, on a stem as tall as the caret.  origin is where the
 * content's 0, 0 is in the window (the viewport's corner less the scroll).
 */
void
kl_text_touch_draw_handles(
	const struct kl_text_touch *touch,
	struct kl_canvas *canvas,
	double origin_x,
	double origin_y,
	const struct kl_theme *theme)
{
	struct kl_rect rect;
	size_t ends[2];
	float x;
	float top;
	float bottom;
	int index;

	/* No handles, nothing to draw. */
	if (!touch->handles || touch->anchor == touch->caret)
		return;

	/* Each end's stem and knob. */
	ends[0] = touch->anchor;
	ends[1] = touch->caret;
	for (index = 0; index < 2; index++) {
		touch->view->caret_rect(touch->data, ends[index], &rect);
		x = (float)(origin_x + (double)rect.x);
		top = (float)(origin_y + (double)rect.y);
		bottom = top + (float)rect.height;
		kl_canvas_line(canvas, x, top, x, bottom, 2.0f, theme->accent);
		kl_canvas_circle(canvas, x, bottom + (float)KL_TEXT_HANDLE / 2.0f, (float)KL_TEXT_HANDLE / 2.0f, theme->accent);
	}
}

/* Keeps how far a finger on the caret's handle is from the middle of the caret. */
static void
touch_grip(
	struct kl_text_touch *touch,
	double x,
	double y)
{
	struct kl_rect rect;

	/* The caret's middle, and the finger's distance from it. */
	touch->view->caret_rect(touch->data, touch->caret, &rect);
	touch->grip_x = x - (double)rect.x;
	touch->grip_y = y - ((double)rect.y + (double)rect.height / 2.0);
}

/* Tells whether a point is within a finger's reach of an end's handle. */
static int
touch_near_handle(
	const struct kl_text_touch *touch,
	size_t position,
	double x,
	double y)
{
	struct kl_rect rect;
	double centre_x;
	double centre_y;
	double distance;

	/* The knob's centre under the end's caret. */
	touch->view->caret_rect(touch->data, position, &rect);
	centre_x = (double)rect.x;
	centre_y = (double)(rect.y + rect.height) + (double)KL_TEXT_HANDLE / 2.0;

	/* Within the reach. */
	distance = hypot(x - centre_x, y - centre_y);
	if (distance > (double)KL_TEXT_HANDLE_REACH / 2.0)
		return 0;

	/* Succeeded: the finger is on the handle. */
	return 1;
}

/*
 * Reports the speed of the self-scroll for a finger at a place of a
 * viewport of a size (negative toward the start), in proportion to how deep
 * it is into the edge band, and at full speed past the edge.
 */
static double
touch_edge_speed(
	double place,
	double size)
{
	double depth;

	/* Near the start: toward the start. */
	if (place < (double)KL_TEXT_EDGE) {
		depth = ((double)KL_TEXT_EDGE - place) / (double)KL_TEXT_EDGE;
		if (depth > 1.0)
			depth = 1.0;
		return -KL_TEXT_EDGE_SPEED * depth;
	}

	/* Near the end: toward the end. */
	if (place > size - (double)KL_TEXT_EDGE) {
		depth = (place - (size - (double)KL_TEXT_EDGE)) / (double)KL_TEXT_EDGE;
		if (depth > 1.0)
			depth = 1.0;
		return KL_TEXT_EDGE_SPEED * depth;
	}

	/* Away from both. */
	return 0.0;
}

/* Shows or hides the bar, noting a change for kl_text_touch_take. */
static void
touch_set_bar(
	struct kl_text_touch *touch,
	int bar)
{
	/* No change. */
	if (touch->bar == bar)
		return;

	/* The new state, told to the view. */
	touch->bar = bar;
	touch->changes |= KL_TEXT_TOUCH_BAR;
}
