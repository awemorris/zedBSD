/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of Notes (ws081-p013): the fingers scroll and zoom the
 * page with libkeiland's gestures and scroll (kl_scroll, ws090-p015), the
 * toolbar takes their taps, and a palm is told from a finger by the pen
 * (touch.h).
 *
 * The page's place is a function of the zoom and the scroll: on an axis
 * where the zoomed page (with its margin) fits the room it is centred as
 * the whole page is, otherwise the scroll moves it.  At the zoom of 1 the
 * place is the whole page's (notes_view_layout).  While writing with a
 * finger is on (ws081-p015), the first finger on the page is not given to
 * the gestures: its events wait for the main loop as a line's.  The lines
 * starting with "NOTES TOUCH" are what the tests read.
 */

#include "touch.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* How far two fingers' distance must change before they zoom (a share of it). */
#define TOUCH_PINCH_START	0.05

/* How often the page is placed while fingers are down or it glides, in milliseconds. */
#define TOUCH_TICK_MS		16

/* The oldest a wl_touch time may be and still be taken (older is another clock), in milliseconds. */
#define TOUCH_TIME_BEHIND	2000U

/* How much a double tap zooms in. */
#define TOUCH_DOUBLE_TAP_ZOOM	2.0f

/* A zoom this close to 1 is the whole page. */
#define TOUCH_ZOOM_WHOLE	1.001f

static uint64_t touch_time(const struct notes_touch_event *event);
static struct notes_touch_finger *touch_finger(struct notes_touch *touch, int32_t id);
static int touch_palm(const struct notes_touch *touch, uint64_t now);
static void touch_down(struct notes_touch *touch, const struct notes_touch_event *event);
static void touch_press(struct notes_touch *touch, uint64_t now);
static void touch_gestures(struct notes_touch *touch, uint64_t now);
static void touch_cancel(struct notes_touch *touch, uint64_t now);
static void touch_double_tap(struct notes_touch *touch, const struct kl_gesture_event *gesture);
static void touch_pinch(struct notes_touch *touch, uint64_t now);
static void touch_pinch_end(struct notes_touch *touch, uint64_t now);
static float touch_zoom_max(const struct notes_touch *touch);
static void touch_extent(const struct notes_touch *touch, double *largest_x, double *largest_y);
static void touch_bounds(struct notes_touch *touch);
static void touch_zoom_about(struct notes_touch *touch, float zoom, float anchor_x, float anchor_y, float x, float y);
static void touch_place(struct notes_touch *touch);
static void touch_stop(struct notes_touch *touch, uint64_t now);
static void touch_write_push(struct notes_touch *touch, unsigned kind, uint64_t time);
static void touch_write_begin(struct notes_touch *touch, const struct notes_touch_event *event);
static void touch_write_motion(struct notes_touch *touch, const struct notes_touch_event *event);
static int touch_write_young(const struct notes_touch *touch, uint64_t now);
static void touch_write_handover(struct notes_touch *touch, uint64_t now);

/*
 * Makes the gestures and the scroll, at the whole page.
 *
 * Returns 0, or ENOMEM.
 */
int
notes_touch_open(
	struct notes_touch *touch)
{
	int error;

	/* Nothing held yet, the whole page. */
	memset(touch, 0, sizeof(*touch));
	touch->zoom = 1.0f;

	/* The gestures of the window. */
	touch->gesture = kl_gesture_create();
	if (touch->gesture == NULL)
		return ENOMEM;

	/* The scroll of the page, across and down. */
	error = kl_scroll_init(&touch->scroll, KL_SCROLL_X | KL_SCROLL_Y);
	if (error != 0) {
		kl_scroll_release(&touch->scroll);
		kl_gesture_destroy(touch->gesture);
		touch->gesture = NULL;
		return ENOMEM;
	}

	/* Succeeded: fingers can be taken. */
	return 0;
}

/*
 * Frees the gestures and the scroll.
 */
void
notes_touch_close(
	struct notes_touch *touch)
{
	/* Both, when they were made (a scroll never made holds nothing). */
	kl_scroll_release(&touch->scroll);
	if (touch->gesture != NULL)
		kl_gesture_destroy(touch->gesture);
	memset(touch, 0, sizeof(*touch));
}

/*
 * Takes the layout a frame is drawn at: the window's size, the toolbar's
 * band, the margin around the page, the page's size in points and the
 * scale at which the whole page fits.  The zoom stays (within what the new
 * page allows) and the page is placed again.
 */
void
notes_touch_layout(
	struct notes_touch *touch,
	uint32_t width,
	uint32_t height,
	float top,
	float margin,
	float page_width,
	float page_height,
	float fit_scale)
{
	double largest_x;
	double largest_y;
	float zoom_max;

	/* The layout. */
	touch->width = (float)width;
	touch->height = (float)height;
	touch->top = top;
	touch->margin = margin;
	touch->page_width = page_width;
	touch->page_height = page_height;
	touch->fit_scale = fit_scale;

	/* The zoom within what this page allows. */
	zoom_max = touch_zoom_max(touch);
	if (touch->zoom > zoom_max)
		touch->zoom = zoom_max;
	if (touch->zoom < 1.0f)
		touch->zoom = 1.0f;

	/* The scroll's ends; a page at rest stays within them. */
	touch_bounds(touch);
	if (!touch->moving) {
		touch_extent(touch, &largest_x, &largest_y);
		if (touch->scroll_x > largest_x)
			touch->scroll_x = largest_x;
		if (touch->scroll_x < 0.0)
			touch->scroll_x = 0.0;
		if (touch->scroll_y > largest_y)
			touch->scroll_y = largest_y;
		if (touch->scroll_y < 0.0)
			touch->scroll_y = 0.0;
		if (touch->gesture != NULL)
			kl_scroll_move_to(&touch->scroll, touch->scroll_x, touch->scroll_y, 0, notes_touch_clock());
	}

	/* Where the page is drawn. */
	touch_place(touch);
}

/*
 * Takes one touch input of the window: a new finger is taken for a palm
 * near the pen, or followed by the gestures (the first presses the
 * scroll, unless it touched the toolbar); a followed finger's motion and
 * lift go to the gestures.
 */
void
notes_touch_event(
	struct notes_touch *touch,
	const struct notes_touch_event *event)
{
	struct notes_touch_finger *finger;
	uint64_t time;
	int error;
	unsigned index;

	/* Nothing without the gestures. */
	if (touch->gesture == NULL)
		return;

	/* The event's time. */
	time = touch_time(event);

	/* Follows the finger by the kind of touch. */
	switch (event->type) {
	case NOTES_TOUCH_DOWN:
		touch_down(touch, event);
		break;
	case NOTES_TOUCH_MOTION:
		/* The writing finger draws on; a followed finger moves through the gestures. */
		if (touch->writing && event->id == touch->writer_id) {
			touch_write_motion(touch, event);
			break;
		}

		/* Any other finger the gestures follow. */
		finger = touch_finger(touch, event->id);
		if (finger != NULL && finger->followed)
			(void)kl_gesture_motion(touch->gesture, event->id, time, event->arrival, event->x, event->y);
		break;
	case NOTES_TOUCH_UP:
		/* The writing finger's lift ends its line where it last was. */
		if (touch->writing && event->id == touch->writer_id) {
			touch->writing = 0;
			touch_write_push(touch, NOTES_TOUCH_WRITE_END, time);
			printf("NOTES TOUCH write end path=%.0f\n", (double)touch->writer_path);
			fflush(stdout);
		}

		/* A followed finger lifts through the gestures; every finger leaves its slot. */
		finger = touch_finger(touch, event->id);
		if (finger == NULL)
			break;
		if (finger->followed) {
			error = kl_gesture_up(touch->gesture, event->id, time);
			if (error == 0 && touch->followed > 0U)
				touch->followed--;
		}

		/* Its slot is free again. */
		finger->used = 0;
		finger->followed = 0;
		break;
	case NOTES_TOUCH_CANCEL:
		/* The compositor took every finger: a line being written is taken back. */
		if (touch->writing) {
			touch->writing = 0;
			touch_write_push(touch, NOTES_TOUCH_WRITE_ABORT, touch->writer_time_us);
			printf("NOTES TOUCH write abort reason=cancel\n");
			fflush(stdout);
		}

		/* The gestures forget every finger. */
		kl_gesture_cancel(touch->gesture);
		for (index = 0; index < NOTES_TOUCH_FINGERS; index++) {
			touch->fingers[index].used = 0;
			touch->fingers[index].followed = 0;
		}

		/* None is followed. */
		touch->followed = 0;
		break;
	default:
		break;
	}

	/* What the fingers mean so far. */
	touch_gestures(touch, event->arrival);
}

/*
 * Tells whether the pen is near the window (over it, or touching).  When it
 * comes near, the fingers followed until then are cancelled (they were a
 * palm or will be put down for one), and a gliding page stops.
 */
void
notes_touch_pen(
	struct notes_touch *touch,
	int near,
	uint64_t now)
{
	unsigned index;

	/* The pen came near: the fingers down are left alone until they lift. */
	if (near && !touch->pen_near && touch->followed > 0U) {
		kl_gesture_cancel(touch->gesture);
		for (index = 0; index < NOTES_TOUCH_FINGERS; index++)
			touch->fingers[index].followed = 0;
		touch->followed = 0;
		printf("NOTES TOUCH palm cancel\n");
		fflush(stdout);
		touch_gestures(touch, now);
	}

	/* The pen came near a finger writing: the finger was a palm, and its line is taken back. */
	if (near &&
	    !touch->pen_near &&
	    touch->writing) {
		touch->writing = 0;
		touch_write_push(touch, NOTES_TOUCH_WRITE_ABORT, now);
		printf("NOTES TOUCH palm cancel writing\n");
		fflush(stdout);
	}

	/* A page gliding under the pen stops where it is (within its edges), so that the pen writes where it points. */
	if (near && !touch->pen_near)
		touch_stop(touch, now);

	/* When it left, for the time a palm is still expected. */
	if (!near && touch->pen_near)
		touch->pen_left_us = now;
	touch->pen_near = near;
}

/*
 * Moves time on for the fingers: finds a long press, zooms by two fingers,
 * moves the scroll with a drag, and places the page where the scroll is at
 * the frame's time.
 *
 * Returns how many milliseconds until the page should be placed again (-1
 * when it rests and no finger is followed).
 */
int
notes_touch_tick(
	struct notes_touch *touch,
	uint64_t now)
{
	double dx;
	double dy;
	int animating;
	int error;

	/* Nothing without the gestures. */
	if (touch->gesture == NULL)
		return -1;

	/* The gestures found by now (a long press among them). */
	touch_gestures(touch, now);

	/* Nothing to do while nothing is touched or moving. */
	if (!touch->pressed &&
	    !touch->moving &&
	    !touch->pinching &&
	    touch->followed == 0U)
		return -1;

	/* Two fingers zoom; when they stop, the page is drawn at its new scale. */
	touch_pinch(touch, now);
	if (touch->pinching) {
		/* The zoom placed the page: the next tick is soon. */
		return TOUCH_TICK_MS;
	}

	/* A drag moves the scroll with the fingers, resampled for the frame. */
	if (touch->dragging &&
	    touch->pressed) {
		error = kl_gesture_drag_offset(touch->gesture, now, &dx, &dy);
		if (error == 0)
			kl_scroll_drag(&touch->scroll, dx - touch->base_x, dy - touch->base_y);
	}

	/* The page where the scroll is at the frame's time. */
	animating = kl_scroll_step(&touch->scroll, now);
	if (touch->moving) {
		touch->scroll_x = touch->scroll.x;
		touch->scroll_y = touch->scroll.y;
		touch_place(touch);
	}

	/* The page rests once it stops with no finger on it. */
	if (!animating &&
	    !touch->pressed &&
	    touch->followed == 0U &&
	    touch->moving) {
		touch->moving = 0;
		printf("NOTES TOUCH rest x=%.1f y=%.1f zoom=%.3f\n", touch->scroll_x, touch->scroll_y, (double)touch->zoom);
		fflush(stdout);
	}

	/* Fingers followed or a gliding page want the next tick soon. */
	if (animating ||
	    touch->followed > 0U)
		return TOUCH_TICK_MS;

	/* Nothing moves: no tick is due. */
	return -1;
}

/*
 * Reports where the page is drawn: its top left in pixels, and pixels per
 * point.
 */
void
notes_touch_view(
	const struct notes_touch *touch,
	float *x,
	float *y,
	float *scale)
{
	/* The place as last computed. */
	*x = touch->view_x;
	*y = touch->view_y;
	*scale = touch->view_scale;
}

/*
 * Takes the oldest tap not yet taken: where it was, in surface pixels, and
 * whether it was on the toolbar.  Returns 1 with a tap, 0 when there is
 * none.
 */
int
notes_touch_take_tap(
	struct notes_touch *touch,
	float *x,
	float *y,
	int *toolbar)
{
	unsigned index;

	/* None waits. */
	if (touch->tap_count == 0U)
		return 0;

	/* The oldest, and the rest move up. */
	*x = touch->tap_x[0];
	*y = touch->tap_y[0];
	*toolbar = touch->tap_toolbar[0];
	for (index = 1; index < touch->tap_count; index++) {
		touch->tap_x[index - 1U] = touch->tap_x[index];
		touch->tap_y[index - 1U] = touch->tap_y[index];
		touch->tap_toolbar[index - 1U] = touch->tap_toolbar[index];
	}

	/* One fewer waits. */
	touch->tap_count--;

	/* Succeeded: one tap taken. */
	return 1;
}

/*
 * Turns writing with a finger on or off.  Turned off while a finger
 * writes, the line ends where the finger last was and the finger is left
 * alone until it lifts.
 */
void
notes_touch_write_mode(
	struct notes_touch *touch,
	int on,
	uint64_t now)
{
	/* A line under way is kept. */
	if (!on && touch->writing) {
		touch->writing = 0;
		touch_write_push(touch, NOTES_TOUCH_WRITE_END, now);
		printf("NOTES TOUCH write end path=%.0f\n", (double)touch->writer_path);
	}

	/* The mode, logged for the tests. */
	touch->write_mode = on;
	printf("NOTES TOUCH write-mode on=%d\n", on);
	fflush(stdout);
}

/*
 * Takes the oldest event of a writing finger not yet taken.  Returns 1
 * with one, 0 when there is none.
 */
int
notes_touch_take_write(
	struct notes_touch *touch,
	struct notes_touch_write *write)
{
	unsigned index;

	/* None waits. */
	if (touch->write_count == 0U)
		return 0;

	/* The oldest, and the rest move up. */
	*write = touch->writes[0];
	for (index = 1; index < touch->write_count; index++)
		touch->writes[index - 1U] = touch->writes[index];

	/* One fewer waits. */
	touch->write_count--;

	/* Succeeded: one taken. */
	return 1;
}

/*
 * Scrolls to the top of the page (another page is shown), keeping the zoom
 * and the scroll across.
 */
void
notes_touch_top(
	struct notes_touch *touch)
{
	/* The top, at rest (a glide or a flight stops). */
	touch->scroll_y = 0.0;
	touch->moving = 0;
	if (touch->gesture != NULL)
		kl_scroll_move_to(&touch->scroll, touch->scroll_x, touch->scroll_y, 0, notes_touch_clock());
	touch_place(touch);
}

/*
 * Reports the monotonic clock in microseconds, the clock of the touch
 * events' arrival and of notes_touch_tick.
 */
uint64_t
notes_touch_clock(void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Reports it in microseconds. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/*
 * Turns a wl_touch time (the compositor's milliseconds, the low 32 bits)
 * into microseconds of the full clock, by the time the event was read; a
 * time far behind or ahead of that is taken as the reading's time.
 */
static uint64_t
touch_time(
	const struct notes_touch_event *event)
{
	uint64_t arrival_ms;
	uint32_t behind;

	/* How far the event's time is behind its reading, modulo 2^32 milliseconds. */
	arrival_ms = event->arrival / 1000U;
	behind = (uint32_t)arrival_ms - event->time;

	/* Another clock, or a time ahead: the reading's time. */
	if (behind > TOUCH_TIME_BEHIND)
		return event->arrival;

	/* Reports the event's time. */
	return (arrival_ms - behind) * 1000U;
}

/* Finds a finger down by its id (NULL when it is not). */
static struct notes_touch_finger *
touch_finger(
	struct notes_touch *touch,
	int32_t id)
{
	unsigned index;

	/* Each slot in use. */
	for (index = 0; index < NOTES_TOUCH_FINGERS; index++) {
		if (touch->fingers[index].used && touch->fingers[index].id == id)
			return &touch->fingers[index];
	}

	/* Not down. */
	return NULL;
}

/* Tells whether a finger touching now is taken for a palm: the pen is near, or left a moment ago. */
static int
touch_palm(
	const struct notes_touch *touch,
	uint64_t now)
{
	/* The pen near. */
	if (touch->pen_near)
		return 1;

	/* The pen left a moment ago. */
	if (touch->pen_left_us != 0U &&
	    now < touch->pen_left_us + (uint64_t)NOTES_TOUCH_PALM_MS * 1000U)
		return 1;

	/* A finger. */
	return 0;
}

/* A finger touches: taken for a palm and left alone, or followed (the first presses the scroll). */
static void
touch_down(
	struct notes_touch *touch,
	const struct notes_touch_event *event)
{
	struct notes_touch_finger *finger;
	uint64_t time;
	unsigned index;
	int palm;
	int young;
	int error;

	/* A free slot; a finger past the last is left alone. */
	finger = NULL;
	for (index = 0; index < NOTES_TOUCH_FINGERS; index++) {
		if (!touch->fingers[index].used) {
			finger = &touch->fingers[index];
			break;
		}
	}

	/* No slot: the finger is left alone. */
	if (finger == NULL)
		return;

	/* The finger is down from now on. */
	finger->id = event->id;
	finger->used = 1;
	finger->followed = 0;

	/* A palm near the pen is left alone. */
	palm = touch_palm(touch, event->arrival);
	if (palm) {
		printf("NOTES TOUCH palm x=%.0f y=%.0f\n", (double)event->x, (double)event->y);
		fflush(stdout);
		return;
	}

	/* A second finger while one writes: a line only just begun is taken back and both scroll and zoom; otherwise it is left alone. */
	young = 0;
	if (touch->writing)
		young = touch_write_young(touch, event->arrival);
	if (touch->writing && !young) {
		printf("NOTES TOUCH write keep\n");
		fflush(stdout);
		return;
	}

	/* The line just begun is taken back, and its finger joins the gestures. */
	if (touch->writing)
		touch_write_handover(touch, event->arrival);

	/* While writing with a finger is on, the first finger on the page writes. */
	if (touch->write_mode &&
	    touch->followed == 0U &&
	    event->y >= touch->top) {
		touch_write_begin(touch, event);
		return;
	}

	/* The first finger followed: on the toolbar it only taps; on the page it presses the scroll. */
	if (touch->followed == 0U) {
		touch->toolbar = 0;
		if (event->y < touch->top)
			touch->toolbar = 1;
		if (!touch->toolbar)
			touch_press(touch, event->arrival);
	}

	/* The gestures follow it. */
	time = touch_time(event);
	error = kl_gesture_down(touch->gesture, event->id, time, event->arrival, event->x, event->y);
	if (error != 0)
		return;
	finger->followed = 1;
	touch->followed++;
}

/*
 * Presses the scroll where the page is: it takes the page over (from where
 * the page is, when it did not own it), and a press on a gliding page
 * catches it.
 */
static void
touch_press(
	struct notes_touch *touch,
	uint64_t now)
{
	double dx;
	double dy;
	int caught;
	int error;

	/* The page's ends, and its place unless the scroll already owns it. */
	touch_bounds(touch);
	if (!touch->moving)
		kl_scroll_move_to(&touch->scroll, touch->scroll_x, touch->scroll_y, 0, now);

	/* The press; the first press on a gliding page catches it. */
	caught = kl_scroll_press(&touch->scroll, now);
	if (!touch->pressed) {
		touch->caught = caught;
		if (caught) {
			printf("NOTES TOUCH caught x=%.1f y=%.1f\n", touch->scroll_x, touch->scroll_y);
			fflush(stdout);
		}
	}

	/* The scroll owns the page from here. */
	touch->pressed = 1;
	touch->moving = 1;

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
	struct notes_touch *touch,
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
			/*
			 * A tap waits for the main loop: on the toolbar it presses its
			 * button, on the page it ends an open text box (ws177-p012).
			 */
			if (touch->tap_count < NOTES_TOUCH_TAPS) {
				touch->tap_x[touch->tap_count] = (float)gesture.x;
				touch->tap_y[touch->tap_count] = (float)gesture.y;
				touch->tap_toolbar[touch->tap_count] = 0U;
				if (touch->toolbar && gesture.y < (double)touch->top)
					touch->tap_toolbar[touch->tap_count] = 1U;
				touch->tap_count++;
			}

			/* The tests' line. */
			printf("NOTES TOUCH tap x=%.0f y=%.0f toolbar=%d\n", gesture.x, gesture.y, touch->toolbar);
			fflush(stdout);
			break;
		case KL_GESTURE_DOUBLE_TAP:
			touch_double_tap(touch, &gesture);
			break;
		case KL_GESTURE_DRAG_BEGIN:
			/* A drag on the page scrolls it. */
			if (!touch->toolbar) {
				touch->dragging = 1;
				printf("NOTES TOUCH drag fingers=%u\n", touch->followed);
				fflush(stdout);
			}

			/* Nothing else. */
			break;
		case KL_GESTURE_DRAG_END:
			/* The page glides on at the finger's velocity. */
			if (touch->dragging) {
				(void)kl_scroll_fling(&touch->scroll, gesture.vx, gesture.vy, now);
				printf("NOTES TOUCH release vx=%.0f vy=%.0f x=%.1f y=%.1f\n", gesture.vx, gesture.vy, touch->scroll_x, touch->scroll_y);
				fflush(stdout);
				touch->pressed = 0;
			}

			/* The drag is over. */
			touch->dragging = 0;
			break;
		case KL_GESTURE_CANCEL:
			touch_cancel(touch, now);
			break;
		default:
			break;
		}
	}

	/* The last finger lifted without a drag: the scroll is let go still (a page past an edge springs back). */
	if (touch->followed == 0U &&
	    touch->pressed) {
		(void)kl_scroll_fling(&touch->scroll, 0.0, 0.0, now);
		touch->pressed = 0;
		touch->dragging = 0;
	}
}

/* The fingers were taken (by the compositor, or for a palm): no glide, no zoom left half done. */
static void
touch_cancel(
	struct notes_touch *touch,
	uint64_t now)
{
	/* Two fingers' zoom stops where it is. */
	if (touch->pinching)
		touch_pinch_end(touch, now);

	/* The scroll lets go (a page past an edge springs back). */
	if (touch->pressed)
		kl_scroll_cancel(&touch->scroll, now);
	touch->pressed = 0;
	touch->dragging = 0;
	touch->toolbar = 0;
	printf("NOTES TOUCH cancel\n");
	fflush(stdout);
}

/*
 * A double tap on the page: at the whole page, zooms in twice about the
 * tapped place; zoomed, goes back to the whole page.  A tap that caught a
 * gliding page zooms nothing.
 */
static void
touch_double_tap(
	struct notes_touch *touch,
	const struct kl_gesture_event *gesture)
{
	float anchor_x;
	float anchor_y;
	float x;
	float y;

	/* Nothing on the toolbar, after a catch, or while a finger writes (its taps are dots). */
	if (touch->toolbar ||
	    touch->caught ||
	    touch->write_mode)
		return;

	/* Zoomed: the whole page again. */
	x = (float)gesture->x;
	y = (float)gesture->y;
	anchor_x = (x - touch->view_x) / touch->view_scale;
	anchor_y = (y - touch->view_y) / touch->view_scale;
	if (touch->zoom > TOUCH_ZOOM_WHOLE) {
		touch_zoom_about(touch, 1.0f, anchor_x, anchor_y, x, y);
		printf("NOTES TOUCH double-tap zoom=%.3f\n", (double)touch->zoom);
		fflush(stdout);
		return;
	}

	/* At the whole page: twice, with the tapped place under the finger. */
	touch_zoom_about(touch, touch->zoom * TOUCH_DOUBLE_TAP_ZOOM, anchor_x, anchor_y, x, y);
	printf("NOTES TOUCH double-tap zoom=%.3f\n", (double)touch->zoom);
	fflush(stdout);
}

/*
 * Two fingers zoom: once their distance has changed enough, the zoom
 * follows its ratio and the page's point that was between them stays
 * between them (which also pans with them).  When they are no longer two,
 * the zoom ends.
 */
static void
touch_pinch(
	struct notes_touch *touch,
	uint64_t now)
{
	double ratio;
	double change;
	double x;
	double y;
	float zoom;
	int error;

	/* Only two fingers (or more) on the page. */
	error = ENOENT;
	if (touch->followed >= 2U &&
	    !touch->toolbar)
		error = kl_gesture_pinch(touch->gesture, now, &ratio, &x, &y);
	if (error != 0) {
		if (touch->pinching)
			touch_pinch_end(touch, now);
		return;
	}

	/* The zoom starts once the distance changed enough, holding the page's point between the fingers. */
	if (!touch->pinching) {
		change = fabs(ratio - 1.0);
		if (change < TOUCH_PINCH_START)
			return;
		touch->pinching = 1;
		touch->zooming = 1;
		touch->pinch_ratio = ratio;
		touch->pinch_zoom = touch->zoom;
		touch->anchor_x = ((float)x - touch->view_x) / touch->view_scale;
		touch->anchor_y = ((float)y - touch->view_y) / touch->view_scale;
		printf("NOTES TOUCH pinch start zoom=%.3f\n", (double)touch->zoom);
		fflush(stdout);
	}

	/* The zoom by the ratio since, with the point under the fingers. */
	zoom = touch->pinch_zoom * (float)(ratio / touch->pinch_ratio);
	touch_zoom_about(touch, zoom, touch->anchor_x, touch->anchor_y, (float)x, (float)y);
}

/*
 * Two fingers' zoom ends: the page is drawn at its new scale, and a finger
 * still down drags on from where the page is.
 */
static void
touch_pinch_end(
	struct notes_touch *touch,
	uint64_t now)
{
	/* The zoom is done. */
	touch->pinching = 0;
	touch->zooming = 0;
	printf("NOTES TOUCH pinch end zoom=%.3f\n", (double)touch->zoom);
	fflush(stdout);

	/* A finger still down drags on from here. */
	if (touch->pressed &&
	    touch->followed > 0U)
		touch_press(touch, now);
}

/* Reports the most the page may be zoomed: NOTES_TOUCH_ZOOM_MAX, or less where the page's picture would pass NOTES_TOUCH_PICTURE_MAX. */
static float
touch_zoom_max(
	const struct notes_touch *touch)
{
	float longest;
	float zoom;

	/* The page's longer side at the whole page's scale. */
	longest = touch->page_width;
	if (touch->page_height > longest)
		longest = touch->page_height;
	longest *= touch->fit_scale;

	/* The picture's limit, the zoom's own, and never below the whole page. */
	zoom = NOTES_TOUCH_ZOOM_MAX;
	if (longest > 0.0f && NOTES_TOUCH_PICTURE_MAX / longest < zoom)
		zoom = NOTES_TOUCH_PICTURE_MAX / longest;
	if (zoom < 1.0f)
		zoom = 1.0f;

	/* Reports the limit. */
	return zoom;
}

/* Reports how far the page can scroll on each axis at the zoom: 0 where it fits the room. */
static void
touch_extent(
	const struct notes_touch *touch,
	double *largest_x,
	double *largest_y)
{
	double scale;
	double room_height;

	/* The page with its margin, against the window below the toolbar. */
	scale = (double)touch->fit_scale * (double)touch->zoom;
	room_height = (double)touch->height - (double)touch->top;
	*largest_x = (double)touch->page_width * scale + 2.0 * (double)touch->margin - (double)touch->width;
	*largest_y = (double)touch->page_height * scale + 2.0 * (double)touch->margin - room_height;

	/* An axis that fits does not scroll. */
	if (*largest_x < 0.0)
		*largest_x = 0.0;
	if (*largest_y < 0.0)
		*largest_y = 0.0;
}

/* Gives the scroll the page's ends and the room's size for the rubber band (only when they changed). */
static void
touch_bounds(
	struct notes_touch *touch)
{
	double largest_x;
	double largest_y;
	double width;
	double height;

	/* Nothing before the scroll was made. */
	if (touch->gesture == NULL)
		return;

	/* The bounds, and the room (at least a pixel). */
	touch_extent(touch, &largest_x, &largest_y);
	width = (double)touch->width;
	if (width < 1.0)
		width = 1.0;
	height = (double)touch->height - (double)touch->top;
	if (height < 1.0)
		height = 1.0;

	/* Unchanged ends leave the scroll alone. */
	if (largest_x == touch->bounds_x &&
	    largest_y == touch->bounds_y &&
	    width == touch->bounds_width &&
	    height == touch->bounds_height)
		return;

	/* The new ends. */
	(void)kl_scroll_set_bounds(&touch->scroll, 0.0, largest_x, 0.0, largest_y, width, height);
	touch->bounds_x = largest_x;
	touch->bounds_y = largest_y;
	touch->bounds_width = width;
	touch->bounds_height = height;
}

/*
 * Zooms (within its bounds) with a page's point (anchor, in points) under a
 * place of the window, as far as the page's edges allow.
 */
static void
touch_zoom_about(
	struct notes_touch *touch,
	float zoom,
	float anchor_x,
	float anchor_y,
	float x,
	float y)
{
	double largest_x;
	double largest_y;
	double scale;
	float zoom_max;

	/* The zoom within its bounds. */
	zoom_max = touch_zoom_max(touch);
	if (zoom > zoom_max)
		zoom = zoom_max;
	if (zoom < 1.0f)
		zoom = 1.0f;
	touch->zoom = zoom;

	/* The scroll that puts the point under the place, within the page's edges. */
	touch_bounds(touch);
	touch_extent(touch, &largest_x, &largest_y);
	scale = (double)touch->fit_scale * (double)zoom;
	touch->scroll_x = (double)touch->margin - ((double)x - (double)anchor_x * scale);
	touch->scroll_y = (double)touch->top + (double)touch->margin - ((double)y - (double)anchor_y * scale);
	if (touch->scroll_x > largest_x)
		touch->scroll_x = largest_x;
	if (touch->scroll_x < 0.0)
		touch->scroll_x = 0.0;
	if (touch->scroll_y > largest_y)
		touch->scroll_y = largest_y;
	if (touch->scroll_y < 0.0)
		touch->scroll_y = 0.0;

	/* The scroll takes the page there at once (a finger down presses it again from there), and the page is placed. */
	kl_scroll_move_to(&touch->scroll, touch->scroll_x, touch->scroll_y, 0, notes_touch_clock());
	touch_place(touch);
}

/*
 * Places the page for the zoom and the scroll: centred on an axis where it
 * fits the room (on whole pixels, as the whole page is), moved by the
 * scroll where it does not.
 */
static void
touch_place(
	struct notes_touch *touch)
{
	double largest_x;
	double largest_y;
	double room_height;
	float scale;

	/* The scale (in float, as notes_view_layout's, so that the zoom of 1 is its place exactly), and how far the page can scroll. */
	scale = touch->fit_scale * touch->zoom;
	room_height = (double)touch->height - (double)touch->top;
	touch_extent(touch, &largest_x, &largest_y);

	/* Across. */
	touch->view_x = (float)floor(((double)touch->width - (double)(touch->page_width * scale)) / 2.0);
	if (largest_x > 0.0)
		touch->view_x = (float)((double)touch->margin - touch->scroll_x);

	/* Down. */
	touch->view_y = (float)floor((double)touch->top + (room_height - (double)(touch->page_height * scale)) / 2.0);
	if (largest_y > 0.0)
		touch->view_y = (float)((double)touch->top + (double)touch->margin - touch->scroll_y);

	/* The scale. */
	touch->view_scale = scale;
}

/* Stops a page gliding by itself where it is now (within its edges); a page held or at rest stays as it is. */
static void
touch_stop(
	struct notes_touch *touch,
	uint64_t now)
{
	/* Only a page that glides with no finger on it. */
	if (!touch->moving)
		return;
	if (touch->pressed)
		return;

	/* The scroll holds the page where it was last placed, within its edges. */
	kl_scroll_move_to(&touch->scroll, touch->scroll_x, touch->scroll_y, 0, now);
	touch->moving = 0;
	touch->scroll_x = touch->scroll.x;
	touch->scroll_y = touch->scroll.y;
	touch_place(touch);

	/* The tests' line. */
	printf("NOTES TOUCH stop x=%.1f y=%.1f\n", touch->scroll_x, touch->scroll_y);
	fflush(stdout);
}

/*
 * Queues an event of the writing finger at its last place and a time (in
 * microseconds).  A motion leaves the queue's last place for the end or
 * the take-back that follows it; a full queue drops the event.
 */
static void
touch_write_push(
	struct notes_touch *touch,
	unsigned kind,
	uint64_t time)
{
	struct notes_touch_write *write;
	unsigned limit;

	/* Room for it. */
	limit = NOTES_TOUCH_WRITES;
	if (kind == NOTES_TOUCH_WRITE_MOTION)
		limit--;
	if (touch->write_count >= limit)
		return;

	/* The event, after the ones before it. */
	write = &touch->writes[touch->write_count];
	touch->write_count++;
	write->kind = kind;
	write->x = touch->writer_x;
	write->y = touch->writer_y;
	write->time_ms = (uint32_t)(time / 1000U);
}

/* The first finger on the page, while writing with a finger is on, begins a line (a gliding page stops under it). */
static void
touch_write_begin(
	struct notes_touch *touch,
	const struct notes_touch_event *event)
{
	/* The page stays where the finger writes on it. */
	touch_stop(touch, event->arrival);

	/* The writing finger, where and when it touched. */
	touch->writing = 1;
	touch->writer_id = event->id;
	touch->writer_down_us = event->arrival;
	touch->writer_time_us = touch_time(event);
	touch->writer_x = event->x;
	touch->writer_y = event->y;
	touch->writer_path = 0.0f;
	touch->toolbar = 0;
	touch->caught = 0;

	/* The line begins there. */
	touch_write_push(touch, NOTES_TOUCH_WRITE_BEGIN, touch->writer_time_us);
	printf("NOTES TOUCH write begin x=%.0f y=%.0f\n", (double)event->x, (double)event->y);
	fflush(stdout);
}

/* The writing finger moves: every report is a point of the line, and the way it went is measured. */
static void
touch_write_motion(
	struct notes_touch *touch,
	const struct notes_touch_event *event)
{
	float dx;
	float dy;

	/* How far it moved since its last report. */
	dx = event->x - touch->writer_x;
	dy = event->y - touch->writer_y;
	touch->writer_path += sqrtf(dx * dx + dy * dy);

	/* Its new place and time, a point of the line. */
	touch->writer_x = event->x;
	touch->writer_y = event->y;
	touch->writer_time_us = touch_time(event);
	touch_write_push(touch, NOTES_TOUCH_WRITE_MOTION, touch->writer_time_us);
}

/*
 * Tells whether the line being written has only just begun: a second
 * finger now takes it back (two fingers put down one a moment after the
 * other scroll and zoom).
 */
static int
touch_write_young(
	const struct notes_touch *touch,
	uint64_t now)
{
	/* Touched a moment ago. */
	if (now < touch->writer_down_us + (uint64_t)NOTES_TOUCH_WRITE_GRACE_MS * 1000U)
		return 1;

	/* Barely moved. */
	if (touch->writer_path < NOTES_TOUCH_WRITE_SLOP)
		return 1;

	/* A line under way. */
	return 0;
}

/*
 * Takes back the line just begun and gives its finger to the gestures,
 * where it is now: with the second finger it scrolls and zooms.
 */
static void
touch_write_handover(
	struct notes_touch *touch,
	uint64_t now)
{
	struct notes_touch_finger *finger;
	int error;

	/* The line is taken back. */
	touch->writing = 0;
	touch_write_push(touch, NOTES_TOUCH_WRITE_ABORT, touch->writer_time_us);
	printf("NOTES TOUCH write abort reason=fingers\n");
	fflush(stdout);

	/* The finger presses the scroll, as the first finger on the page does. */
	touch->toolbar = 0;
	touch_press(touch, now);

	/* The gestures follow it from where it is. */
	finger = touch_finger(touch, touch->writer_id);
	error = kl_gesture_down(touch->gesture, touch->writer_id, touch->writer_time_us, now, touch->writer_x, touch->writer_y);
	if (error != 0)
		return;

	/* It is followed from now on (its slot is still held). */
	if (finger == NULL)
		return;
	finger->followed = 1;
	touch->followed++;
}
