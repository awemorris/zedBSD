/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of PDF Viewer (ws081-p012): what the fingers do to the
 * view, with libkeiland's gestures and scroller (touch.h).
 *
 * The scroller owns the view from a touch until the content rests: each
 * tick sets scroll_x and scroll_y from it (past an end, as the rubber band
 * shows it), and a move made elsewhere in the meantime (a key, the wheel,
 * an action, a resize) is taken over rather than undone.  The view is
 * placed at the frame's time, from the fingers' motion resampled for it.
 * The fingers come from libkeiland's window (ws090-p008), their times already
 * turned into microseconds of CLOCK_MONOTONIC.
 */

#include "touch.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/* What a drag does: nothing yet, scroll the view, or swipe the page mode's page. */
#define TOUCH_DRAG_NONE		0
#define TOUCH_DRAG_SCROLL	1
#define TOUCH_DRAG_SWIPE	2

/* How far two fingers' distance must change before they zoom (a share of it). */
#define TOUCH_PINCH_START	0.05

/* How often the view is placed while fingers are down or the content glides, in milliseconds. */
#define TOUCH_TICK_MS		16

/* How much a double tap zooms in. */
#define TOUCH_DOUBLE_TAP_ZOOM	2.0

/* How long the page indicator stays after the view moves, in milliseconds (as the view's). */
#define TOUCH_INDICATOR_MS	1400U

/* The share a swipe keeps of the finger's movement past the first and the last page (as the pointer's). */
#define TOUCH_SWIPE_RESIST	3.0

static int touch_for_pointer(const struct pv_app *app, const struct kl_window_event *event);
static void touch_pointer(struct pv_touch *touch, struct pv_app *app, const struct kl_window_event *event);
static void touch_press(struct pv_touch *touch, struct pv_app *app, uint64_t now);
static void touch_gestures(struct pv_touch *touch, struct pv_app *app, uint64_t now);
static void touch_drag_begin(struct pv_touch *touch, struct pv_app *app, uint64_t now);
static void touch_drag_end(struct pv_touch *touch, struct pv_app *app, uint64_t now, const struct kl_gesture_event *gesture);
static void touch_cancel(struct pv_touch *touch, struct pv_app *app, uint64_t now);
static void touch_double_tap(struct pv_touch *touch, struct pv_app *app, const struct kl_gesture_event *gesture);
static void touch_bounds(struct pv_touch *touch, struct pv_app *app);
static void touch_pinch(struct pv_touch *touch, struct pv_app *app, uint64_t now);
static void touch_pinch_end(struct pv_touch *touch, struct pv_app *app, uint64_t now);
static void touch_swipe(struct pv_touch *touch, struct pv_app *app, uint64_t now);
static int touch_handle(struct pv_touch *touch, struct pv_app *app, const struct kl_window_event *event, double x);

/*
 * Makes the gestures and the scroller.
 *
 * Returns 0, or ENOMEM.
 */
int
pv_touch_open(
	struct pv_touch *touch)
{
	/* Nothing held yet. */
	memset(touch, 0, sizeof(*touch));

	/* The gestures of the window. */
	touch->gesture = kl_gesture_create();
	if (touch->gesture == NULL)
		return ENOMEM;

	/* The scroller of the view. */
	touch->scroller = kl_scroller_create();
	if (touch->scroller == NULL) {
		kl_gesture_destroy(touch->gesture);
		touch->gesture = NULL;
		return ENOMEM;
	}

	/* Succeeded: fingers can be taken. */
	return 0;
}

/*
 * Frees the gestures and the scroller.
 */
void
pv_touch_close(
	struct pv_touch *touch)
{
	/* Both, when they were made. */
	if (touch->scroller != NULL)
		kl_scroller_destroy(touch->scroller);
	if (touch->gesture != NULL)
		kl_gesture_destroy(touch->gesture);
	memset(touch, 0, sizeof(*touch));
}

/*
 * Takes one touch input of the window: a finger over the sidebar or the
 * password card plays the pointer; the pages' fingers go to the gestures,
 * and the first of them presses the scroller.
 */
void
pv_touch_event(
	struct pv_touch *touch,
	struct pv_app *app,
	const struct kl_window_event *event)
{
	uint64_t time;
	double x;
	int sidebar;
	int error;
	int for_pointer;
	int held;

	/* Nothing without the gestures. */
	if (touch->gesture == NULL)
		return;

	/* The finger that plays the pointer keeps it, and no other finger is taken meanwhile. */
	if (touch->pointer) {
		touch_pointer(touch, app, event);
		return;
	}

	/* A first finger over the sidebar or the card starts playing the pointer. */
	for_pointer = touch_for_pointer(app, event);
	if (event->kind == KL_WINDOW_TOUCH_DOWN &&
	    touch->fingers == 0U &&
	    for_pointer) {
		touch_pointer(touch, app, event);
		return;
	}

	/* The event's time, and the place over the pages (right of the sidebar). */
	time = event->time_us;
	sidebar = pv_app_sidebar_width(app);
	x = event->x - (double)sidebar;

	/* A finger on a handle of the selection moves it, apart from the gestures (ws177-p042). */
	held = touch_handle(touch, app, event, x);
	if (held)
		return;

	/* Hands the finger to the gestures. */
	switch (event->kind) {
	case KL_WINDOW_TOUCH_DOWN:
		/* The first finger presses the scroller. */
		if (touch->fingers == 0U)
			touch_press(touch, app, event->arrival_us);
		error = kl_gesture_down(touch->gesture, event->id, time, event->arrival_us, x, event->y);
		if (error == 0)
			touch->fingers++;
		break;
	case KL_WINDOW_TOUCH_MOTION:
		(void)kl_gesture_motion(touch->gesture, event->id, time, event->arrival_us, x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		error = kl_gesture_up(touch->gesture, event->id, time);
		if (error == 0 &&
		    touch->fingers > 0U)
			touch->fingers--;
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		kl_gesture_cancel(touch->gesture);
		touch->fingers = 0;
		break;
	}

	/* What the fingers mean so far. */
	touch_gestures(touch, app, event->arrival_us);
}

/*
 * A touch pad's two fingers move the view by the event's dx, dy (pixels,
 * as a wheel scrolls) at the compositor's time (ws090-p019): libkeiland's
 * scroller takes the view at their first move (from where it is, unless it
 * already owns it; a glide is caught) and follows them, as a finger on the
 * screen does.  Fingers on the screen keep it from them.
 */
void
pv_touch_pad(
	struct pv_touch *touch,
	struct pv_app *app,
	const struct kl_window_event *event)
{
	int holding;
	int caught;

	/* Fingers on the screen, the pointer's finger, or no document. */
	if (touch->gesture == NULL ||
	    touch->fingers > 0U ||
	    touch->pointer ||
	    !app->has_document)
		return;

	/* The first move: the view's bounds, and its place unless the scroller owns it already. */
	holding = kl_scroller_axis_holding(touch->scroller);
	if (!holding) {
		touch_bounds(touch, app);
		if (!touch->moving)
			kl_scroller_set_position(touch->scroller, app->scroll_x, app->scroll_y);
		touch->written_x = app->scroll_x;
		touch->written_y = app->scroll_y;
	}

	/* The scroller follows the fingers and owns the view until it rests. */
	caught = kl_scroller_axis(touch->scroller, event->dx, event->dy, event->time_us, event->arrival_us);
	touch->moving = 1;
	app->touching = 1;
	if (caught)
		pv_log("TOUCH caught source=finger x=%.1f y=%.1f", app->scroll_x, app->scroll_y);
}

/*
 * The touch pad's fingers lift: the view flies on at their velocity, as
 * libkeiland's scroller throws it (ws090-p019).
 */
void
pv_touch_pad_stop(
	struct pv_touch *touch,
	const struct kl_window_event *event)
{
	double vx;
	double vy;
	int flung;

	/* The scroller throws the view the fingers held. */
	flung = kl_scroller_axis_stop(touch->scroller, event->time_us, event->arrival_us, &vx, &vy);
	if (flung)
		pv_log("KINETIC fling source=finger vx=%.0f vy=%.0f", vx, vy);
}

/*
 * Moves time on for the fingers: finds a long press, places the view at
 * the frame's time from the fingers (a drag, two fingers' zoom, a swipe) or
 * from the gliding content, and notices a move of the view made elsewhere.
 *
 * Returns how many milliseconds until the view should be placed again
 * (-1 when nothing moves and no finger is down).
 */
int
pv_touch_tick(
	struct pv_touch *touch,
	struct pv_app *app,
	uint64_t now)
{
	double x;
	double y;
	double dx;
	double dy;
	int animating;
	int holding;
	int error;

	/* Nothing without the gestures. */
	if (touch->gesture == NULL)
		return -1;

	/* The gestures found by now (a long press among them). */
	touch_gestures(touch, app, now);

	/* Nothing to do while nothing is touched or moving. */
	if (!touch->pressed &&
	    !touch->moving &&
	    touch->fingers == 0U) {
		app->touching = 0;
		return -1;
	}

	/* The document may have closed under the fingers: they have nothing to move. */
	if (!app->has_document) {
		touch->moving = 0;
		touch->drag = TOUCH_DRAG_NONE;
		if (touch->pinching)
			touch_pinch_end(touch, app, now);

		/* Fingers still down keep the ticks coming until they lift. */
		app->touching = 0;
		if (touch->fingers > 0U) {
			app->touching = 1;
			return TOUCH_TICK_MS;
		}

		/* No finger: no tick is due. */
		return -1;
	}

	/* The bounds of the view as laid out now. */
	touch_bounds(touch, app);

	/* A move made elsewhere (a key, the wheel, an action, a resize) is taken over. */
	if (touch->moving &&
	    (app->scroll_x != touch->written_x ||
	     app->scroll_y != touch->written_y)) {
		kl_scroller_set_position(touch->scroller, app->scroll_x, app->scroll_y);
		touch->written_x = app->scroll_x;
		touch->written_y = app->scroll_y;
		if (touch->pressed)
			touch_press(touch, app, now);
	}

	/* Two fingers zoom; when they stop, the pages are drawn again at the new scale. */
	touch_pinch(touch, app, now);
	if (touch->pinching) {
		/* The zoom placed the view: the next tick is soon. */
		app->touching = 1;
		return TOUCH_TICK_MS;
	}

	/* A swipe moves the page mode's page with the fingers. */
	if (touch->drag == TOUCH_DRAG_SWIPE) {
		touch_swipe(touch, app, now);

		/* The swipe placed the page: the next tick is soon. */
		app->touching = 1;
		return TOUCH_TICK_MS;
	}

	/* A drag moves the scroller with the fingers, resampled for the frame. */
	if (touch->drag == TOUCH_DRAG_SCROLL &&
	    touch->pressed) {
		error = kl_gesture_drag_offset(touch->gesture, now, &dx, &dy);
		if (error == 0)
			kl_scroller_drag(touch->scroller, dx - touch->base_x, dy - touch->base_y);
	}

	/* The view where the scroller is at the frame's time. */
	animating = kl_scroller_step(touch->scroller, now, &x, &y);
	if (touch->moving &&
	    (x != app->scroll_x ||
	     y != app->scroll_y)) {
		app->scroll_x = x;
		app->scroll_y = y;
		app->indicator_until = app->now + TOUCH_INDICATOR_MS;
		app->dirty = 1;
	}

	/* What the view was set to, to see a move made elsewhere. */
	touch->written_x = app->scroll_x;
	touch->written_y = app->scroll_y;

	/* The content rests once it stops with no finger on it, on the screen or on a touch pad. */
	holding = kl_scroller_axis_holding(touch->scroller);
	if (!animating &&
	    !touch->pressed &&
	    touch->fingers == 0U &&
	    !holding &&
	    touch->moving) {
		touch->moving = 0;
		pv_log("TOUCH rest x=%.1f y=%.1f", app->scroll_x, app->scroll_y);
		pv_app_clamp(app);
	}

	/* Fingers down or content that has not rested keep the viewer from drawing pages ahead. */
	app->touching = 0;
	if (touch->moving ||
	    touch->fingers > 0U)
		app->touching = 1;

	/* Fingers down (on the screen or a touch pad) or gliding content want the next tick soon. */
	if (animating ||
	    holding ||
	    touch->fingers > 0U)
		return TOUCH_TICK_MS;

	/* Nothing moves: no tick is due. */
	return -1;
}

/* Tells whether a finger touches where the pointer's button is the way in: the card, the sidebar. */
static int
touch_for_pointer(
	const struct pv_app *app,
	const struct kl_window_event *event)
{
	int sidebar;

	/* The password card takes every touch. */
	if (app->asking_password)
		return 1;

	/* So does the sidebar, where it is. */
	sidebar = pv_app_sidebar_width(app);
	if (sidebar > 0 && event->x < (double)sidebar)
		return 1;

	/* The pages take the rest. */
	return 0;
}

/*
 * Plays the pointer's left button with one finger: down presses where it
 * touches, motion moves the pointer, up releases; a cancel lets go without
 * a click.
 */
static void
touch_pointer(
	struct pv_touch *touch,
	struct pv_app *app,
	const struct kl_window_event *event)
{
	struct pv_event pointer;

	/* Only the finger that plays it, once it plays it. */
	if (touch->pointer &&
	    event->kind != KL_WINDOW_TOUCH_CANCEL &&
	    event->id != touch->pointer_id)
		return;

	/* The input, at the finger's place (up has none: where it last was). */
	memset(&pointer, 0, sizeof(pointer));
	pointer.x = touch->pointer_x;
	pointer.y = touch->pointer_y;
	if (event->kind == KL_WINDOW_TOUCH_DOWN ||
	    event->kind == KL_WINDOW_TOUCH_MOTION) {
		pointer.x = (int)floor(event->x);
		pointer.y = (int)floor(event->y);
		touch->pointer_x = pointer.x;
		touch->pointer_y = pointer.y;
	}

	/* The left button, at the time the finger was read. */
	pointer.button = PV_BUTTON_LEFT;
	pointer.time = event->arrival_us / 1000U;

	/* Plays the pointer by the kind of touch. */
	switch (event->kind) {
	case KL_WINDOW_TOUCH_DOWN:
		/* The pointer comes, and presses. */
		touch->pointer = 1;
		touch->pointer_id = event->id;
		pointer.type = PV_EVENT_MOTION;
		pv_app_event(app, &pointer);
		pointer.type = PV_EVENT_BUTTON;
		pointer.pressed = 1;
		pv_app_event(app, &pointer);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		pointer.type = PV_EVENT_MOTION;
		pv_app_event(app, &pointer);
		break;
	case KL_WINDOW_TOUCH_UP:
		/* Released where the finger last was. */
		touch->pointer = 0;
		pointer.type = PV_EVENT_BUTTON;
		pv_app_event(app, &pointer);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		/* Let go without a release, so that nothing is clicked. */
		touch->pointer = 0;
		app->pressed = 0;
		app->dragging = 0;
		app->thumbnail_pressed = 0;
		app->thumbnail_dragging = 0;
		break;
	}
}

/*
 * Presses the scroller where the view is: it takes the view over (when it
 * did not own it, from where the view is now), and a press on gliding
 * content catches it.
 */
static void
touch_press(
	struct pv_touch *touch,
	struct pv_app *app,
	uint64_t now)
{
	double dx;
	double dy;
	int error;
	int caught;

	/* The view's bounds, and its place unless the scroller already owns it. */
	touch_bounds(touch, app);
	if (!touch->moving)
		kl_scroller_set_position(touch->scroller, app->scroll_x, app->scroll_y);

	/* The press; a first press on gliding content catches it. */
	caught = kl_scroller_press(touch->scroller, now);
	if (!touch->pressed) {
		touch->caught = caught;
		if (caught)
			pv_log("TOUCH caught x=%.1f y=%.1f", app->scroll_x, app->scroll_y);
	}

	/* The scroller owns the view from here. */
	touch->pressed = 1;
	touch->moving = 1;
	touch->written_x = app->scroll_x;
	touch->written_y = app->scroll_y;

	/* A drag already going on is measured from here. */
	touch->base_x = 0.0;
	touch->base_y = 0.0;
	error = kl_gesture_drag_offset(touch->gesture, now, &dx, &dy);
	if (error == 0) {
		touch->base_x = dx;
		touch->base_y = dy;
	}
}

/* Takes the gestures found so far and does what each means. */
static void
touch_gestures(
	struct pv_touch *touch,
	struct pv_app *app,
	uint64_t now)
{
	struct kl_gesture_event gesture;
	int selected;
	int found;

	/* Each gesture in turn. */
	for (;;) {
		found = kl_gesture_next(touch->gesture, now, &gesture);
		if (!found)
			break;

		/* Does what the gesture means. */
		switch (gesture.kind) {
		case KL_GESTURE_TAP:
			/* A tap lets a selection go (ws177-p042). */
			pv_log("TOUCH tap x=%.0f y=%.0f caught=%d", gesture.x, gesture.y, touch->caught);
			pv_select_clear(app);
			break;
		case KL_GESTURE_DOUBLE_TAP:
			touch_double_tap(touch, app, &gesture);
			break;
		case KL_GESTURE_LONG_PRESS:
			/* A long press on a word selects it, with its handles (ws177-p042). */
			selected = pv_select_word_at(app, (int)floor(gesture.x), (int)floor(gesture.y));
			pv_log("TOUCH long-press x=%.0f y=%.0f selected=%d", gesture.x, gesture.y, selected);
			break;
		case KL_GESTURE_DRAG_BEGIN:
			touch_drag_begin(touch, app, now);
			break;
		case KL_GESTURE_DRAG_END:
			touch_drag_end(touch, app, now, &gesture);
			break;
		case KL_GESTURE_CANCEL:
			touch_cancel(touch, app, now);
			break;
		default:
			break;
		}
	}

	/* The last finger lifted without a drag: the scroller is let go still (content past an end springs back). */
	if (touch->fingers == 0U &&
	    touch->pressed) {
		kl_scroller_release(touch->scroller, now, 0.0, 0.0);
		touch->pressed = 0;
		touch->drag = TOUCH_DRAG_NONE;
	}
}

/*
 * A drag begins: in the page mode, one finger moving more across than
 * down over a page that fits across swipes it; anything else scrolls.
 */
static void
touch_drag_begin(
	struct pv_touch *touch,
	struct pv_app *app,
	uint64_t now)
{
	double dx;
	double dy;
	double across;
	double down;
	double content_width;
	int error;

	/* Nothing to drag without a document, or during a page turn. */
	touch->drag = TOUCH_DRAG_NONE;
	if (!app->has_document ||
	    app->turning)
		return;

	/* Where the fingers went. */
	dx = 0.0;
	dy = 0.0;
	error = kl_gesture_drag_offset(touch->gesture, now, &dx, &dy);
	if (error != 0)
		return;

	/* A sideways swipe of a page that fits across, in the page mode. */
	content_width = pv_app_content_width(app);
	across = fabs(dx);
	down = fabs(dy);
	if (app->mode == PV_MODE_PAGE &&
	    touch->fingers == 1U &&
	    across >= down &&
	    content_width <= (double)app->width) {
		touch->drag = TOUCH_DRAG_SWIPE;
		pv_log("TOUCH drag kind=swipe");
		return;
	}

	/* Otherwise the view scrolls. */
	touch->drag = TOUCH_DRAG_SCROLL;
	pv_log("TOUCH drag kind=scroll fingers=%u", touch->fingers);
}

/*
 * The last finger of a drag lifts with its velocity: the view glides on
 * (or settles), or the swipe turns the page or slides it back.
 */
static void
touch_drag_end(
	struct pv_touch *touch,
	struct pv_app *app,
	uint64_t now,
	const struct kl_gesture_event *gesture)
{
	int flung;

	/* A swipe turns by how far and how fast the page went (pixels a millisecond). */
	if (touch->drag == TOUCH_DRAG_SWIPE) {
		touch_swipe(touch, app, now);
		pv_app_swipe_end(app, gesture->vx / 1000.0, 1);
		kl_scroller_release(touch->scroller, now, 0.0, 0.0);
	}

	/* A scroll glides on at the finger's velocity. */
	if (touch->drag == TOUCH_DRAG_SCROLL) {
		flung = kl_scroller_release(touch->scroller, now, gesture->vx, gesture->vy);
		pv_log("TOUCH release vx=%.0f vy=%.0f x=%.1f y=%.1f", gesture->vx, gesture->vy, app->scroll_x, app->scroll_y);
		if (flung)
			pv_log("KINETIC fling source=touch vx=%.0f vy=%.0f", gesture->vx, gesture->vy);
	}

	/* The drag is over. */
	touch->drag = TOUCH_DRAG_NONE;
	touch->pressed = 0;
}

/* The compositor took the fingers: no fling, no turn, no zoom left half done. */
static void
touch_cancel(
	struct pv_touch *touch,
	struct pv_app *app,
	uint64_t now)
{
	/* A swipe slides back. */
	if (touch->drag == TOUCH_DRAG_SWIPE)
		pv_app_swipe_end(app, 0.0, 0);

	/* Two fingers' zoom stops where it is. */
	if (touch->pinching)
		touch_pinch_end(touch, app, now);

	/* The scroller lets go (content past an end springs back). */
	if (touch->pressed)
		kl_scroller_cancel(touch->scroller, now);
	touch->pressed = 0;
	touch->drag = TOUCH_DRAG_NONE;
	pv_log("TOUCH cancel");
}

/*
 * A double tap: at the mode's fit, zooms in to twice the scale about the
 * tapped point; zoomed, goes back to the fit.  A tap that caught gliding
 * content zooms nothing.
 */
static void
touch_double_tap(
	struct pv_touch *touch,
	struct pv_app *app,
	const struct kl_gesture_event *gesture)
{
	struct pv_place place;
	double scale;
	size_t page;

	/* Nothing to zoom without a document, or after a catch. */
	if (!app->has_document ||
	    touch->caught)
		return;

	/* Zoomed: the mode's fit again. */
	if (app->fit == PV_FIT_CUSTOM) {
		pv_app_action(app, PV_ACTION_ZOOM_RESET);
		pv_log("TOUCH double-tap zoom=fit");
		return;
	}

	/* At the fit: twice the scale, with the tapped place kept under the finger. */
	pv_app_place_at(app, gesture->x, gesture->y, &place);
	page = pv_app_current_page(app);
	scale = pv_app_scale(app, page);
	pv_app_zoom_to(app, scale * TOUCH_DOUBLE_TAP_ZOOM);
	pv_app_show_place(app, &place, gesture->x, gesture->y);
	pv_log("TOUCH double-tap zoom=%.3f", app->zoom);
}

/* Gives the scroller the view's bounds and size as laid out now (only when they changed). */
static void
touch_bounds(
	struct pv_touch *touch,
	struct pv_app *app)
{
	double largest_x;
	double largest_y;
	double width;
	double height;

	/* How far the view can go across and down. */
	largest_x = pv_app_content_width(app) - (double)app->width;
	if (largest_x < 0.0)
		largest_x = 0.0;
	largest_y = pv_app_content_height(app) - (double)app->height;
	if (largest_y < 0.0)
		largest_y = 0.0;

	/* The view's size, at least a pixel. */
	width = (double)app->width;
	if (width < 1.0)
		width = 1.0;
	height = (double)app->height;
	if (height < 1.0)
		height = 1.0;

	/* Unchanged bounds leave the scroller alone. */
	if (largest_x == touch->bounds_x &&
	    largest_y == touch->bounds_y &&
	    width == touch->bounds_width &&
	    height == touch->bounds_height)
		return;

	/* The new bounds. */
	(void)kl_scroller_set_bounds(touch->scroller, 0.0, largest_x, 0.0, largest_y, width, height);
	touch->bounds_x = largest_x;
	touch->bounds_y = largest_y;
	touch->bounds_width = width;
	touch->bounds_height = height;
}

/*
 * Two fingers zoom: once their distance has changed enough, the scale
 * follows its ratio and the place that was between them stays between
 * them (which also pans with them).  When they are no longer two, the zoom
 * ends.
 */
static void
touch_pinch(
	struct pv_touch *touch,
	struct pv_app *app,
	uint64_t now)
{
	double ratio;
	double change;
	double x;
	double y;
	double scale;
	size_t page;
	int error;

	/* Only two fingers (or more) that do not swipe. */
	error = ENOENT;
	if (touch->fingers >= 2U &&
	    touch->drag != TOUCH_DRAG_SWIPE)
		error = kl_gesture_pinch(touch->gesture, now, &ratio, &x, &y);
	if (error != 0) {
		if (touch->pinching)
			touch_pinch_end(touch, app, now);
		return;
	}

	/* The zoom starts once the distance changed enough, holding the place between the fingers. */
	if (!touch->pinching) {
		change = fabs(ratio - 1.0);
		if (change < TOUCH_PINCH_START)
			return;
		touch->pinching = 1;
		touch->ratio = ratio;
		page = pv_app_current_page(app);
		touch->scale = pv_app_scale(app, page);
		pv_app_place_at(app, x, y, &touch->place);
		app->zooming = 1;
		pv_log("TOUCH pinch start scale=%.3f", touch->scale);
	}

	/* The scale by the ratio since, with the place under the fingers. */
	scale = touch->scale * ratio / touch->ratio;
	pv_app_zoom_to(app, scale);
	pv_app_show_place(app, &touch->place, x, y);
	app->indicator_until = app->now + TOUCH_INDICATOR_MS;
	touch->written_x = app->scroll_x;
	touch->written_y = app->scroll_y;
}

/*
 * Two fingers' zoom ends: the pages are drawn again at the new scale, and
 * a finger still down drags on from where the view is.
 */
static void
touch_pinch_end(
	struct pv_touch *touch,
	struct pv_app *app,
	uint64_t now)
{
	/* The zoom is done; the frame draws the pages at their scale again. */
	touch->pinching = 0;
	app->zooming = 0;
	app->dirty = 1;
	pv_log("TOUCH pinch end scale=%.3f", app->zoom);

	/* The scroller takes the view where the zoom left it. */
	touch_bounds(touch, app);
	kl_scroller_set_position(touch->scroller, app->scroll_x, app->scroll_y);
	touch->written_x = app->scroll_x;
	touch->written_y = app->scroll_y;
	if (touch->pressed &&
	    touch->fingers > 0U)
		touch_press(touch, app, now);
}

/* Moves the page mode's page with the fingers across, resisting past the first and the last page. */
static void
touch_swipe(
	struct pv_touch *touch,
	struct pv_app *app,
	uint64_t now)
{
	double dx;
	double dy;
	int error;

	/* Where the fingers are. */
	error = kl_gesture_drag_offset(touch->gesture, now, &dx, &dy);
	if (error != 0)
		return;

	/* The page follows, a third as far past either end. */
	app->swipe = dx;
	if (app->page == 0 &&
	    app->swipe > 0.0)
		app->swipe /= TOUCH_SWIPE_RESIST;
	if (app->page + 1 >= app->document.count &&
	    app->swipe < 0.0)
		app->swipe /= TOUCH_SWIPE_RESIST;
	app->dirty = 1;
}

/*
 * Moves a handle of the selection with the finger that holds it
 * (ws177-p042): the first finger down on a handle takes it, its motion
 * moves the handle to the character under it, and its lift or a cancel
 * lets it go.  x is the finger's place over the pages.  Returns 1 when the
 * event was the handle's (the gestures do not see it).
 */
static int
touch_handle(
	struct pv_touch *touch,
	struct pv_app *app,
	const struct kl_window_event *event,
	double x)
{
	int which;

	/* A first finger down on a handle takes it. */
	if (touch->handle == 0) {
		if (event->kind != KL_WINDOW_TOUCH_DOWN || touch->fingers != 0U)
			return 0;
		which = pv_select_handle_at(app, (int)floor(x), (int)floor(event->y));
		if (which == 0)
			return 0;
		touch->handle = which;
		touch->handle_id = event->id;
		pv_log("TOUCH handle=%d", which);
		return 1;
	}

	/* Another finger while a handle is held is passed over. */
	if (event->kind != KL_WINDOW_TOUCH_CANCEL && event->id != touch->handle_id)
		return 1;

	/* Its motion moves the handle. */
	if (event->kind == KL_WINDOW_TOUCH_MOTION) {
		pv_select_handle_move(app, touch->handle, (int)floor(x), (int)floor(event->y));
		return 1;
	}

	/* Its lift (or a cancel) lets the handle go. */
	if (event->kind == KL_WINDOW_TOUCH_UP || event->kind == KL_WINDOW_TOUCH_CANCEL)
		touch->handle = 0;

	/* The handle's. */
	return 1;
}
