/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of Image Viewer (after PDF Viewer's ws081-p012): what
 * the fingers do to the view, with libkeiland's gestures and scroller (touch.h).
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

/* What a drag does: nothing yet, move the image, or swipe to the next or the previous image. */
#define TOUCH_DRAG_NONE		0
#define TOUCH_DRAG_SCROLL	1
#define TOUCH_DRAG_SWIPE	2

/* How far two fingers' distance must change before they zoom (a share of it). */
#define TOUCH_PINCH_START	0.05

/* How often the view is placed while fingers are down or the content glides, in milliseconds. */
#define TOUCH_TICK_MS		16

/* How much a double tap zooms in. */
#define TOUCH_DOUBLE_TAP_ZOOM	2.0

/* The share a swipe keeps of the finger's movement past the first and the last image (as the pointer's). */
#define TOUCH_SWIPE_RESIST	3.0

static int touch_for_pointer(const struct iv_app *app, const struct kl_window_event *event);
static void touch_pointer(struct iv_touch *touch, struct iv_app *app, const struct kl_window_event *event);
static void touch_press(struct iv_touch *touch, struct iv_app *app, uint64_t now);
static void touch_gestures(struct iv_touch *touch, struct iv_app *app, uint64_t now);
static void touch_drag_begin(struct iv_touch *touch, struct iv_app *app, uint64_t now);
static void touch_drag_end(struct iv_touch *touch, struct iv_app *app, uint64_t now, const struct kl_gesture_event *gesture);
static void touch_cancel(struct iv_touch *touch, struct iv_app *app, uint64_t now);
static void touch_double_tap(struct iv_touch *touch, struct iv_app *app, const struct kl_gesture_event *gesture);
static void touch_long_press(struct iv_touch *touch, struct iv_app *app, const struct kl_gesture_event *gesture);
static void touch_bounds(struct iv_touch *touch, struct iv_app *app);
static void touch_pinch(struct iv_touch *touch, struct iv_app *app, uint64_t now);
static void touch_pinch_end(struct iv_touch *touch, struct iv_app *app, uint64_t now);
static void touch_swipe(struct iv_touch *touch, struct iv_app *app, uint64_t now);

/*
 * Makes the gestures and the scroller.
 *
 * Returns 0, or ENOMEM.
 */
int
iv_touch_open(
	struct iv_touch *touch)
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
iv_touch_close(
	struct iv_touch *touch)
{
	/* Both, when they were made. */
	if (touch->scroller != NULL)
		kl_scroller_destroy(touch->scroller);
	if (touch->gesture != NULL)
		kl_gesture_destroy(touch->gesture);
	memset(touch, 0, sizeof(*touch));
}

/*
 * Takes one touch input of the window: a finger over the empty window
 * plays the pointer; the image's fingers go to the gestures, and the first
 * of them presses the scroller.
 */
void
iv_touch_event(
	struct iv_touch *touch,
	struct iv_app *app,
	const struct kl_window_event *event)
{
	uint64_t time;
	int error;
	int for_pointer;

	/* Nothing without the gestures. */
	if (touch->gesture == NULL)
		return;

	/* The finger that plays the pointer keeps it, and no other finger is taken meanwhile. */
	if (touch->pointer) {
		touch_pointer(touch, app, event);
		return;
	}

	/* A first finger over the empty window starts playing the pointer. */
	for_pointer = touch_for_pointer(app, event);
	if (event->kind == KL_WINDOW_TOUCH_DOWN &&
	    touch->fingers == 0U &&
	    for_pointer) {
		touch_pointer(touch, app, event);
		return;
	}

	/* The event's time. */
	time = event->time_us;

	/* Hands the finger to the gestures. */
	switch (event->kind) {
	case KL_WINDOW_TOUCH_DOWN:
		/* The first finger presses the scroller. */
		if (touch->fingers == 0U)
			touch_press(touch, app, event->arrival_us);

		/* The finger joins the gestures, and counts once they took it. */
		error = kl_gesture_down(touch->gesture, event->id, time, event->arrival_us, event->x, event->y);
		if (error == 0)
			touch->fingers++;
		break;
	case KL_WINDOW_TOUCH_MOTION:
		/* The finger's new place; the tick places the view from it. */
		(void)kl_gesture_motion(touch->gesture, event->id, time, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		/* The finger leaves the gestures, and the count once they let it go. */
		error = kl_gesture_up(touch->gesture, event->id, time);
		if (error == 0 &&
		    touch->fingers > 0U)
			touch->fingers--;
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		/* Every finger is gone at once. */
		kl_gesture_cancel(touch->gesture);
		touch->fingers = 0;
		break;
	}

	/* What the fingers mean so far. */
	touch_gestures(touch, app, event->arrival_us);
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
iv_touch_tick(
	struct iv_touch *touch,
	struct iv_app *app,
	uint64_t now)
{
	double x;
	double y;
	double dx;
	double dy;
	int animating;
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

	/* The image may have closed under the fingers: they have nothing to move. */
	if (!app->has_image) {
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

	/* Two fingers zoom; when they stop, a zoom below the fit springs back. */
	touch_pinch(touch, app, now);
	if (touch->pinching) {
		/* The zoom placed the view: the next tick is soon. */
		app->touching = 1;
		return TOUCH_TICK_MS;
	}

	/* A swipe moves the image across with the fingers. */
	if (touch->drag == TOUCH_DRAG_SWIPE) {
		touch_swipe(touch, app, now);

		/* The swipe placed the image: the next tick is soon. */
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
		app->dirty = 1;
	}

	/* What the view was set to, to see a move made elsewhere. */
	touch->written_x = app->scroll_x;
	touch->written_y = app->scroll_y;

	/* The content rests once it stops with no finger on it. */
	if (!animating &&
	    !touch->pressed &&
	    touch->fingers == 0U &&
	    touch->moving) {
		touch->moving = 0;
		iv_log("TOUCH rest x=%.1f y=%.1f", app->scroll_x, app->scroll_y);
		iv_app_clamp(app);
	}

	/* Fingers down or content that has not rested keep the viewer from decoding images ahead. */
	app->touching = 0;
	if (touch->moving ||
	    touch->fingers > 0U)
		app->touching = 1;

	/* Fingers down or gliding content want the next tick soon. */
	if (animating ||
	    touch->fingers > 0U)
		return TOUCH_TICK_MS;

	/* Nothing moves: no tick is due. */
	return -1;
}

/* Tells whether a finger touches where the pointer's button is the way in: the empty window. */
static int
touch_for_pointer(
	const struct iv_app *app,
	const struct kl_window_event *event)
{
	UNUSED_PARAMETER(event);

	/* The empty window takes every touch (its Open button). */
	if (!app->has_image)
		return 1;

	/* The image takes the rest. */
	return 0;
}

/*
 * Plays the pointer's left button with one finger: down presses where it
 * touches, motion moves the pointer, up releases; a cancel lets go without
 * a click.
 */
static void
touch_pointer(
	struct iv_touch *touch,
	struct iv_app *app,
	const struct kl_window_event *event)
{
	struct iv_event pointer;

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
	pointer.button = IV_BUTTON_LEFT;
	pointer.time = event->arrival_us / 1000U;

	/* Plays the pointer by the kind of touch. */
	switch (event->kind) {
	case KL_WINDOW_TOUCH_DOWN:
		/* The pointer comes, and presses. */
		touch->pointer = 1;
		touch->pointer_id = event->id;
		pointer.type = IV_EVENT_MOTION;
		iv_app_event(app, &pointer);

		/* The press, where the pointer came. */
		pointer.type = IV_EVENT_BUTTON;
		pointer.pressed = 1;
		iv_app_event(app, &pointer);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		/* The pointer follows the finger. */
		pointer.type = IV_EVENT_MOTION;
		iv_app_event(app, &pointer);
		break;
	case KL_WINDOW_TOUCH_UP:
		/* Released where the finger last was. */
		touch->pointer = 0;
		pointer.type = IV_EVENT_BUTTON;
		iv_app_event(app, &pointer);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		/* Let go without a release, so that nothing is clicked. */
		touch->pointer = 0;
		app->pressed = 0;
		app->dragging = 0;
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
	struct iv_touch *touch,
	struct iv_app *app,
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
			iv_log("TOUCH caught x=%.1f y=%.1f", app->scroll_x, app->scroll_y);
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
	struct iv_touch *touch,
	struct iv_app *app,
	uint64_t now)
{
	struct kl_gesture_event gesture;
	int found;

	/* Each gesture in turn. */
	for (;;) {
		found = kl_gesture_next(touch->gesture, now, &gesture);
		if (!found)
			break;

		/* Does what the gesture means. */
		switch (gesture.kind) {
		case KL_GESTURE_TAP:
			iv_log("TOUCH tap x=%.0f y=%.0f caught=%d", gesture.x, gesture.y, touch->caught);
			break;
		case KL_GESTURE_DOUBLE_TAP:
			touch_double_tap(touch, app, &gesture);
			break;
		case KL_GESTURE_LONG_PRESS:
			touch_long_press(touch, app, &gesture);
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
 * A drag begins: one finger moving more across than down over a fitted
 * image of a folder of several swipes it; anything else moves the image.
 */
static void
touch_drag_begin(
	struct iv_touch *touch,
	struct iv_app *app,
	uint64_t now)
{
	double dx;
	double dy;
	double across;
	double down;
	double content_width;
	int error;

	/* Nothing to drag without an image, or while one slides in. */
	touch->drag = TOUCH_DRAG_NONE;
	if (!app->has_image ||
	    app->sliding)
		return;

	/* Where the fingers went. */
	dx = 0.0;
	dy = 0.0;
	error = kl_gesture_drag_offset(touch->gesture, now, &dx, &dy);
	if (error != 0)
		return;

	/* A sideways swipe of a fitted image, with other images to go to. */
	content_width = iv_app_content_width(app);
	across = fabs(dx);
	down = fabs(dy);
	if (app->fit &&
	    app->folder.count > 1U &&
	    touch->fingers == 1U &&
	    across >= down &&
	    content_width <= (double)app->area_width) {
		touch->drag = TOUCH_DRAG_SWIPE;
		iv_log("TOUCH drag kind=swipe");
		return;
	}

	/* Otherwise the image moves. */
	touch->drag = TOUCH_DRAG_SCROLL;
	iv_log("TOUCH drag kind=scroll fingers=%u", touch->fingers);
}

/*
 * The last finger of a drag lifts with its velocity: the image glides on
 * (or settles), or the swipe brings the next image in or slides it back.
 */
static void
touch_drag_end(
	struct iv_touch *touch,
	struct iv_app *app,
	uint64_t now,
	const struct kl_gesture_event *gesture)
{
	/* A swipe changes the image by how far and how fast it went (pixels a millisecond). */
	if (touch->drag == TOUCH_DRAG_SWIPE) {
		touch_swipe(touch, app, now);
		iv_app_swipe_end(app, gesture->vx / 1000.0, 1);
		kl_scroller_release(touch->scroller, now, 0.0, 0.0);
	}

	/* A scroll glides on at the finger's velocity. */
	if (touch->drag == TOUCH_DRAG_SCROLL) {
		kl_scroller_release(touch->scroller, now, gesture->vx, gesture->vy);
		iv_log("TOUCH release vx=%.0f vy=%.0f x=%.1f y=%.1f", gesture->vx, gesture->vy, app->scroll_x, app->scroll_y);
	}

	/* The drag is over. */
	touch->drag = TOUCH_DRAG_NONE;
	touch->pressed = 0;
}

/* The compositor took the fingers: no fling, no swipe, no zoom left half done. */
static void
touch_cancel(
	struct iv_touch *touch,
	struct iv_app *app,
	uint64_t now)
{
	/* A swipe slides back. */
	if (touch->drag == TOUCH_DRAG_SWIPE)
		iv_app_swipe_end(app, 0.0, 0);

	/* Two fingers' zoom stops where it is. */
	if (touch->pinching)
		touch_pinch_end(touch, app, now);

	/* The scroller lets go (content past an end springs back). */
	if (touch->pressed)
		kl_scroller_cancel(touch->scroller, now);

	/* No finger drags any more. */
	touch->pressed = 0;
	touch->drag = TOUCH_DRAG_NONE;
	iv_log("TOUCH cancel");
}

/*
 * A double tap: fitted, zooms in about the tapped point; zoomed, goes back
 * to the fit.  A tap that caught gliding content zooms nothing.
 */
static void
touch_double_tap(
	struct iv_touch *touch,
	struct iv_app *app,
	const struct kl_gesture_event *gesture)
{
	/* Nothing to zoom without an image, or after a catch. */
	if (!app->has_image ||
	    touch->caught)
		return;

	/* The fit, or a closer look, about the tapped point. */
	iv_app_toggle_zoom(app, gesture->x, gesture->y);
	iv_log("TOUCH double-tap scale=%.3f", app->scale);
}

/* A long press: the context menu where the finger is. */
static void
touch_long_press(
	struct iv_touch *touch,
	struct iv_app *app,
	const struct kl_gesture_event *gesture)
{
	/* Logs every long press for the tests. */
	iv_log("TOUCH long-press x=%.0f y=%.0f", gesture->x, gesture->y);

	/* Only over an image, and not after a catch. */
	if (!app->has_image ||
	    touch->caught)
		return;

	/* The window opens it at the finger. */
	app->want_context = 1;
	app->context_x = (int)gesture->x;
	app->context_y = (int)gesture->y;
}

/* Gives the scroller the view's bounds and size as laid out now (only when they changed). */
static void
touch_bounds(
	struct iv_touch *touch,
	struct iv_app *app)
{
	double largest_x;
	double largest_y;
	double width;
	double height;

	/* How far the image can go across and down. */
	largest_x = iv_app_content_width(app) - (double)app->area_width;
	if (largest_x < 0.0)
		largest_x = 0.0;

	/* And down. */
	largest_y = iv_app_content_height(app) - (double)app->area_height;
	if (largest_y < 0.0)
		largest_y = 0.0;

	/* The area's size, at least a pixel. */
	width = (double)app->area_width;
	if (width < 1.0)
		width = 1.0;

	/* And its height. */
	height = (double)app->area_height;
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
	struct iv_touch *touch,
	struct iv_app *app,
	uint64_t now)
{
	double ratio;
	double change;
	double x;
	double y;
	double scale;
	int error;

	/* Only two fingers (or more) that do not swipe. */
	error = ENOENT;
	if (touch->fingers >= 2U &&
	    touch->drag != TOUCH_DRAG_SWIPE)
		error = kl_gesture_pinch(touch->gesture, now, &ratio, &x, &y);
	if (error != 0) {
		/* A zoom going on ends with the second finger. */
		if (touch->pinching)
			touch_pinch_end(touch, app, now);
		return;
	}

	/* The zoom starts once the distance changed enough, holding the place between the fingers. */
	if (!touch->pinching) {
		change = fabs(ratio - 1.0);
		if (change < TOUCH_PINCH_START)
			return;

		/* The zoom starts from the scale and the ratio now, holding the place between the fingers. */
		touch->pinching = 1;
		touch->ratio = ratio;
		touch->scale = app->scale;
		iv_app_place_at(app, x, y, &touch->place);
		app->zooming = 1;
		iv_log("TOUCH pinch start scale=%.3f", touch->scale);
	}

	/* The scale by the ratio since, with the place under the fingers. */
	scale = touch->scale * ratio / touch->ratio;
	iv_app_zoom_to(app, scale);
	iv_app_show_place(app, &touch->place, x, y);
	touch->written_x = app->scroll_x;
	touch->written_y = app->scroll_y;
}

/*
 * Two fingers' zoom ends: a zoom below the fit springs back to it, and a
 * finger still down drags on from where the image is.
 */
static void
touch_pinch_end(
	struct iv_touch *touch,
	struct iv_app *app,
	uint64_t now)
{
	/* The zoom is done; below the fit it springs back. */
	touch->pinching = 0;
	app->zooming = 0;
	app->dirty = 1;
	iv_log("TOUCH pinch end scale=%.3f", app->scale);
	iv_app_settle(app);

	/* The scroller takes the view where the zoom left it. */
	touch_bounds(touch, app);
	kl_scroller_set_position(touch->scroller, app->scroll_x, app->scroll_y);
	touch->written_x = app->scroll_x;
	touch->written_y = app->scroll_y;

	/* A finger still down presses the scroller again, to drag on from here. */
	if (touch->pressed &&
	    touch->fingers > 0U)
		touch_press(touch, app, now);
}

/* Moves the image with the fingers across, resisting past the first and the last image. */
static void
touch_swipe(
	struct iv_touch *touch,
	struct iv_app *app,
	uint64_t now)
{
	double dx;
	double dy;
	int error;

	/* Where the fingers are. */
	error = kl_gesture_drag_offset(touch->gesture, now, &dx, &dy);
	if (error != 0)
		return;

	/* The image follows across (the fingers' movement down is not used), a third as far past either end. */
	app->swipe = dx;
	if (app->folder.index == 0 &&
	    app->swipe > 0.0)
		app->swipe /= TOUCH_SWIPE_RESIST;
	if (app->folder.index + 1 >= app->folder.count &&
	    app->swipe < 0.0)
		app->swipe /= TOUCH_SWIPE_RESIST;
	app->dirty = 1;
}
