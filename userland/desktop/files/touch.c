/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of Files (ws081-p010): the fingers scroll the items and
 * the sidebar with libkeiland's gestures and scroller, and play the
 * pointer's buttons for taps, long presses and everything that is not
 * scrolled (touch.h).  The lines starting with "ZFILES TOUCH" (on standard
 * error, as Files' own) are what the tests read.
 */

#include "touch.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* What the first finger does: nothing yet, play the left button, or its gestures. */
#define TOUCH_MODE_NONE		0U
#define TOUCH_MODE_POINTER	1U
#define TOUCH_MODE_GESTURE	2U

/* How often the scroll is set while fingers are down or it glides, in milliseconds. */
#define TOUCH_TICK_MS		16

/* The oldest a wl_touch time may be and still be taken (older is another clock), in milliseconds. */
#define TOUCH_TIME_BEHIND	2000U

static uint64_t touch_time(const struct fm_touch_event *event);
static void touch_bounds(struct fm_touch *touch);
static void touch_target(struct fm_touch *touch, unsigned area);
static void touch_press(struct fm_touch *touch, uint64_t now);
static void touch_down(struct fm_touch *touch, const struct fm_touch_event *event);
static void touch_up(struct fm_touch *touch, const struct fm_touch_event *event);
static void touch_gestures(struct fm_touch *touch, uint64_t now);
static void touch_pointer(struct fm_touch *touch, unsigned kind, unsigned button, double x, double y, uint64_t now);
static void touch_pad(struct fm_touch *touch, const struct fm_touch_event *event, uint64_t time);
static void touch_pad_stop(struct fm_touch *touch, const struct fm_touch_event *event, uint64_t time);

/*
 * Makes the gestures and the scroller.
 *
 * Returns 0, or ENOMEM.
 */
int
fm_touch_open(
	struct fm_touch *touch)
{
	/* Nothing held yet. */
	memset(touch, 0, sizeof(*touch));

	/* The gestures of the window. */
	touch->gesture = kl_gesture_create();
	if (touch->gesture == NULL)
		return ENOMEM;

	/* The scroller of the area the fingers scroll. */
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
fm_touch_close(
	struct fm_touch *touch)
{
	/* Both, when they were made. */
	if (touch->scroller != NULL)
		kl_scroller_destroy(touch->scroller);
	if (touch->gesture != NULL)
		kl_gesture_destroy(touch->gesture);
	memset(touch, 0, sizeof(*touch));
}

/*
 * Takes a scrolled area's state for this round (FM_TOUCH_CONTENT or
 * _SIDEBAR).  For the area the fingers scroll, a scroll other than the one
 * they last set, or another thing shown there, is taken over: a glide stops
 * there, and a finger down drags on from it.
 */
void
fm_touch_layout(
	struct fm_touch *touch,
	unsigned area,
	const struct fm_touch_area *state)
{
	int elsewhere;

	/* Only the scrolled areas. */
	if (area != FM_TOUCH_CONTENT && area != FM_TOUCH_SIDEBAR)
		return;

	/* Whether the fingers' area changed without them. */
	elsewhere = 0;
	if (area == touch->target &&
	    (state->token != touch->areas[area].token ||
	     state->scroll != touch->scroll))
		elsewhere = 1;
	touch->areas[area] = *state;

	/* Only the fingers' area goes on. */
	if (area != touch->target || touch->scroller == NULL)
		return;

	/* Its bounds; a scroll set elsewhere is taken over. */
	touch_bounds(touch);
	touch->scroll = state->scroll;
	if (!elsewhere)
		return;

	/* The scroller holds that scroll; a glide stops, a finger down drags on from it. */
	kl_scroller_set_position(touch->scroller, 0.0, (double)state->scroll);
	touch->changed = 0;
	if (touch->moving && !touch->pressed) {
		touch->moving = 0;
		fprintf(stderr, "ZFILES TOUCH stop area=%u scroll=%d\n", area, state->scroll);
	}

	/* A finger down drags on from it, pressed again at the next tick's time. */
	if (touch->pressed)
		touch->repress = 1;
}

/*
 * Takes one touch input of the window: the first finger decides what the
 * touch does (the left button, or the gestures of a scrolled area).
 */
void
fm_touch_event(
	struct fm_touch *touch,
	const struct fm_touch_event *event)
{
	uint64_t time;

	/* Nothing without the gestures. */
	if (touch->gesture == NULL)
		return;

	/* The event's time. */
	time = touch_time(event);

	/* Follows the finger by the kind of touch. */
	switch (event->type) {
	case FM_TOUCH_DOWN:
		touch_down(touch, event);
		break;
	case FM_TOUCH_MOTION:
		/* The first finger's place; it moves the pointer when it plays the button or holds a long press's press. */
		if (event->id == touch->first_id) {
			touch->last_x = event->x;
			touch->last_y = event->y;
			if (touch->mode == TOUCH_MODE_POINTER || touch->holding)
				touch_pointer(touch, FM_TOUCH_POINTER_MOTION, FM_TOUCH_LEFT, event->x, event->y, event->arrival);
		}

		/* The gestures follow it. */
		if (touch->mode == TOUCH_MODE_GESTURE)
			(void)kl_gesture_motion(touch->gesture, event->id, time, event->arrival, event->x, event->y);
		break;
	case FM_TOUCH_UP:
		touch_up(touch, event);
		break;
	case FM_TOUCH_PAD:
		/* A touch pad's fingers scroll the area under the pointer (ws090-p019). */
		touch_pad(touch, event, time);
		break;
	case FM_TOUCH_PAD_STOP:
		/* They lift: the area flies on. */
		touch_pad_stop(touch, event, time);
		break;
	case FM_TOUCH_CANCEL:
		/* The compositor took the fingers (a drag and drop among them): a held button is let go (the main loop drops it during a drag and drop). */
		if (touch->mode == TOUCH_MODE_POINTER || touch->holding)
			touch_pointer(touch, FM_TOUCH_POINTER_RELEASE, FM_TOUCH_LEFT, touch->last_x, touch->last_y, event->arrival);
		if (touch->mode == TOUCH_MODE_GESTURE)
			kl_gesture_cancel(touch->gesture);
		touch->followed = 0;
		touch->holding = 0;
		touch->held = 0;
		touch->mode = TOUCH_MODE_NONE;
		fprintf(stderr, "ZFILES TOUCH cancel\n");
		break;
	default:
		break;
	}

	/* What the fingers mean so far. */
	touch_gestures(touch, event->arrival);
}

/*
 * Moves time on for the fingers: finds a long press, moves the scroller
 * with a drag, and sets the area's scroll where the scroller is at the
 * frame's time.
 *
 * Returns how many milliseconds until the scroll should be set again (-1
 * when it rests and no finger is down).
 */
int
fm_touch_tick(
	struct fm_touch *touch,
	uint64_t now)
{
	double x;
	double y;
	double dx;
	double dy;
	int scroll;
	int animating;
	int holding;
	int error;

	/* Nothing without the gestures. */
	if (touch->gesture == NULL)
		return -1;

	/* The gestures found by now (a long press among them). */
	touch_gestures(touch, now);

	/* A scroll taken over under a finger: the drag goes on from it. */
	if (touch->repress) {
		touch->repress = 0;
		if (touch->pressed)
			touch_press(touch, now);
	}

	/* Nothing to do while nothing is touched or moving. */
	if (!touch->pressed &&
	    !touch->moving &&
	    touch->mode == TOUCH_MODE_NONE)
		return -1;

	/* A drag moves the scroller with the fingers, resampled for the frame. */
	if (touch->dragging &&
	    touch->pressed) {
		error = kl_gesture_drag_offset(touch->gesture, now, &dx, &dy);
		if (error == 0)
			kl_scroller_drag(touch->scroller, dx - touch->base_x, dy - touch->base_y);
	}

	/* The area's scroll where the scroller is at the frame's time. */
	animating = kl_scroller_step(touch->scroller, now, &x, &y);
	if (touch->moving && touch->target != FM_TOUCH_OTHER) {
		scroll = (int)lround(y);
		if (scroll != touch->scroll) {
			touch->scroll = scroll;
			touch->changed = 1;
		}
	}

	/* The scroll rests once it stops with no finger on it, on the screen or on a touch pad. */
	holding = kl_scroller_axis_holding(touch->scroller);
	if (!animating &&
	    !touch->pressed &&
	    touch->mode == TOUCH_MODE_NONE &&
	    !holding &&
	    touch->moving) {
		touch->moving = 0;
		fprintf(stderr, "ZFILES TOUCH rest area=%u scroll=%d\n", touch->target, touch->scroll);
	}

	/* Fingers down (on the screen or a touch pad) or a glide want the next tick soon. */
	if (animating ||
	    holding ||
	    touch->mode != TOUCH_MODE_NONE)
		return TOUCH_TICK_MS;

	/* Nothing moves: no tick is due. */
	return -1;
}

/*
 * Takes the scroll the fingers set since it was last taken: the area and
 * its scroll.  Returns 1 with a new scroll, 0 when there is none.
 */
int
fm_touch_scroll(
	struct fm_touch *touch,
	unsigned *area,
	int *scroll)
{
	/* Nothing new. */
	if (!touch->changed)
		return 0;

	/* The scroll, taken. */
	*area = touch->target;
	*scroll = touch->scroll;
	touch->changed = 0;

	/* Succeeded: a new scroll. */
	return 1;
}

/*
 * Takes the oldest pointer event the fingers made.  Returns 1 with one, 0
 * when there is none.
 */
int
fm_touch_take_pointer(
	struct fm_touch *touch,
	struct fm_touch_pointer *pointer)
{
	unsigned index;

	/* None waits. */
	if (touch->pointer_count == 0U)
		return 0;

	/* The oldest, and the rest move up. */
	*pointer = touch->pointers[0];
	for (index = 1; index < touch->pointer_count; index++)
		touch->pointers[index - 1U] = touch->pointers[index];

	/* One fewer waits. */
	touch->pointer_count--;

	/* Succeeded: one taken. */
	return 1;
}

/*
 * Reports the monotonic clock in microseconds, the clock of the touch
 * events' arrival and of fm_touch_tick.
 */
uint64_t
fm_touch_clock(void)
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
	const struct fm_touch_event *event)
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

/* Gives the scroller the fingers' area's bounds and height (only when they changed). */
static void
touch_bounds(
	struct fm_touch *touch)
{
	const struct fm_touch_area *area;
	double largest;
	double height;

	/* The area's range and height (at least a pixel each way). */
	area = &touch->areas[touch->target];
	largest = (double)area->largest;
	if (largest < 0.0)
		largest = 0.0;
	height = (double)area->height;
	if (height < 1.0)
		height = 1.0;

	/* Unchanged bounds leave the scroller alone. */
	if (largest == touch->bounds_largest &&
	    height == touch->bounds_height)
		return;

	/* The new bounds; only down scrolls. */
	(void)kl_scroller_set_bounds(touch->scroller, 0.0, 0.0, 0.0, largest, 1.0, height);
	touch->bounds_largest = largest;
	touch->bounds_height = height;
}

/* Makes an area the one the scroller moves, from the scroll it has (a glide of another area stops). */
static void
touch_target(
	struct fm_touch *touch,
	unsigned area)
{
	/* The same area goes on as it is. */
	if (area == touch->target)
		return;

	/* The new area, its bounds and its scroll, at rest. */
	touch->target = area;
	touch->moving = 0;
	touch->changed = 0;
	touch->bounds_largest = -1.0;
	touch_bounds(touch);
	touch->scroll = touch->areas[area].scroll;
	kl_scroller_set_position(touch->scroller, 0.0, (double)touch->scroll);
}

/*
 * Presses the scroller where the area is: it takes the scroll over (from
 * where the area has it, when it did not own it), and a press on a glide
 * catches it.
 */
static void
touch_press(
	struct fm_touch *touch,
	uint64_t now)
{
	double dx;
	double dy;
	int caught;
	int error;

	/* The area's scroll, unless the scroller already owns it. */
	if (!touch->moving)
		kl_scroller_set_position(touch->scroller, 0.0, (double)touch->scroll);

	/* The press; the first press on a glide catches it. */
	caught = kl_scroller_press(touch->scroller, now);
	if (!touch->pressed) {
		touch->caught = caught;
		if (caught)
			fprintf(stderr, "ZFILES TOUCH caught area=%u scroll=%d\n", touch->target, touch->scroll);
	}

	/* The scroller owns the scroll from here. */
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

/*
 * A finger touches: the first plays the left button away from the scrolled
 * areas, or starts the gestures on one (pressing its scroller); the others
 * join the gestures.
 */
static void
touch_down(
	struct fm_touch *touch,
	const struct fm_touch_event *event)
{
	uint64_t time;
	int error;

	/* A finger while the first plays the button is left alone. */
	if (touch->mode == TOUCH_MODE_POINTER)
		return;

	/* The first finger decides what the touch does. */
	if (touch->mode == TOUCH_MODE_NONE) {
		touch->first_id = event->id;
		touch->serial = event->serial;
		touch->last_x = event->x;
		touch->last_y = event->y;
		touch->held = 0;
		touch->holding = 0;

		/* Away from the scrolled areas it is the left button: the pointer comes, and presses. */
		if (event->area != FM_TOUCH_CONTENT && event->area != FM_TOUCH_SIDEBAR) {
			touch->mode = TOUCH_MODE_POINTER;
			touch_pointer(touch, FM_TOUCH_POINTER_MOTION, FM_TOUCH_LEFT, event->x, event->y, event->arrival);
			touch_pointer(touch, FM_TOUCH_POINTER_PRESS, FM_TOUCH_LEFT, event->x, event->y, event->arrival);
			return;
		}

		/* On one it starts the gestures, and presses that area's scroller. */
		touch->mode = TOUCH_MODE_GESTURE;
		touch_target(touch, event->area);
		touch_press(touch, event->arrival);
	}

	/* The gestures follow it. */
	time = touch_time(event);
	error = kl_gesture_down(touch->gesture, event->id, time, event->arrival, event->x, event->y);
	if (error == 0)
		touch->followed++;
}

/*
 * A finger lifts: the button it played is released; a long press that did
 * not move is the right button's click; a held press is released.
 */
static void
touch_up(
	struct fm_touch *touch,
	const struct fm_touch_event *event)
{
	uint64_t time;
	int error;

	/* The finger that plays the button releases it where it last was. */
	if (touch->mode == TOUCH_MODE_POINTER) {
		if (event->id == touch->first_id) {
			touch_pointer(touch, FM_TOUCH_POINTER_RELEASE, FM_TOUCH_LEFT, touch->last_x, touch->last_y, event->arrival);
			touch->mode = TOUCH_MODE_NONE;
		}

		/* The other fingers do nothing. */
		return;
	}

	/* Nothing else without the gestures. */
	if (touch->mode != TOUCH_MODE_GESTURE)
		return;

	/* The gestures hear the lift. */
	time = touch_time(event);
	error = kl_gesture_up(touch->gesture, event->id, time);
	if (error == 0 && touch->followed > 0U)
		touch->followed--;

	/* The first finger's long press: a click of the right button, or the held press released. */
	if (event->id == touch->first_id) {
		if (touch->holding) {
			touch_pointer(touch, FM_TOUCH_POINTER_RELEASE, FM_TOUCH_LEFT, touch->last_x, touch->last_y, event->arrival);
		} else if (touch->held) {
			touch_pointer(touch, FM_TOUCH_POINTER_MOTION, FM_TOUCH_RIGHT, touch->press_x, touch->press_y, event->arrival);
			touch_pointer(touch, FM_TOUCH_POINTER_PRESS, FM_TOUCH_RIGHT, touch->press_x, touch->press_y, event->arrival);
			touch_pointer(touch, FM_TOUCH_POINTER_RELEASE, FM_TOUCH_RIGHT, touch->press_x, touch->press_y, event->arrival);
			fprintf(stderr, "ZFILES TOUCH context x=%.0f y=%.0f\n", touch->press_x, touch->press_y);
		}

		/* The long press is over. */
		touch->holding = 0;
		touch->held = 0;
	}

	/* The touch is over when the last finger lifts. */
	if (touch->followed == 0U)
		touch->mode = TOUCH_MODE_NONE;
}

/* Takes the gestures found so far and does what each means. */
static void
touch_gestures(
	struct fm_touch *touch,
	uint64_t now)
{
	struct kl_gesture_event gesture;
	int found;
	int flung;

	/* Each gesture in turn. */
	for (;;) {
		found = kl_gesture_next(touch->gesture, now, &gesture);
		if (!found)
			break;

		/* Does what the gesture means. */
		switch (gesture.kind) {
		case KL_GESTURE_TAP:
			/* A click of the left button there, unless the touch caught a glide (two quick ones make a double click). */
			if (!touch->caught) {
				touch_pointer(touch, FM_TOUCH_POINTER_MOTION, FM_TOUCH_LEFT, gesture.x, gesture.y, now);
				touch_pointer(touch, FM_TOUCH_POINTER_PRESS, FM_TOUCH_LEFT, gesture.x, gesture.y, now);
				touch_pointer(touch, FM_TOUCH_POINTER_RELEASE, FM_TOUCH_LEFT, gesture.x, gesture.y, now);
			}

			/* The tests' line. */
			fprintf(stderr, "ZFILES TOUCH tap x=%.0f y=%.0f caught=%d\n", gesture.x, gesture.y, touch->caught);
			break;
		case KL_GESTURE_LONG_PRESS:
			/* A long press waits for the lift (the context menu) or a move (a held press). */
			if (!touch->caught && touch->followed == 1U) {
				touch->held = 1;
				touch->press_x = gesture.x;
				touch->press_y = gesture.y;
				fprintf(stderr, "ZFILES TOUCH long-press x=%.0f y=%.0f\n", gesture.x, gesture.y);
			}

			/* Nothing else. */
			break;
		case KL_GESTURE_DRAG_BEGIN:
			/* After a long press the left button is held where it pressed and follows the finger; otherwise the drag scrolls. */
			if (touch->held) {
				touch->held = 0;
				touch->holding = 1;
				touch_pointer(touch, FM_TOUCH_POINTER_MOTION, FM_TOUCH_LEFT, touch->press_x, touch->press_y, now);
				touch_pointer(touch, FM_TOUCH_POINTER_PRESS, FM_TOUCH_LEFT, touch->press_x, touch->press_y, now);
				touch_pointer(touch, FM_TOUCH_POINTER_MOTION, FM_TOUCH_LEFT, touch->last_x, touch->last_y, now);
				fprintf(stderr, "ZFILES TOUCH hold x=%.0f y=%.0f\n", touch->press_x, touch->press_y);
			} else if (!touch->holding) {
				touch->dragging = 1;
				fprintf(stderr, "ZFILES TOUCH drag area=%u fingers=%u scroll=%d largest=%.0f\n", touch->target, touch->followed, touch->scroll, touch->bounds_largest);
			}

			/* Nothing else. */
			break;
		case KL_GESTURE_DRAG_END:
			/* The area glides on at the finger's velocity. */
			if (touch->dragging) {
				flung = kl_scroller_release(touch->scroller, now, gesture.vx, gesture.vy);
				fprintf(stderr, "ZFILES TOUCH release vy=%.0f scroll=%d\n", gesture.vy, touch->scroll);
				if (flung)
					fprintf(stderr, "ZFILES KINETIC fling source=touch vy=%.0f\n", gesture.vy);
				touch->pressed = 0;
			}

			/* The drag is over. */
			touch->dragging = 0;
			break;
		case KL_GESTURE_CANCEL:
			/* No glide. */
			if (touch->pressed)
				kl_scroller_cancel(touch->scroller, now);
			touch->pressed = 0;
			touch->dragging = 0;
			break;
		default:
			break;
		}
	}

	/* The last finger lifted without a drag: the scroller is let go still (a scroll past an end springs back). */
	if (touch->mode != TOUCH_MODE_GESTURE &&
	    touch->pressed) {
		kl_scroller_release(touch->scroller, now, 0.0, 0.0);
		touch->pressed = 0;
		touch->dragging = 0;
	}
}

/* Makes a pointer event at a place, for the main loop (a full queue drops it). */
static void
touch_pointer(
	struct fm_touch *touch,
	unsigned kind,
	unsigned button,
	double x,
	double y,
	uint64_t now)
{
	struct fm_touch_pointer *pointer;

	/* Room for it. */
	if (touch->pointer_count >= FM_TOUCH_POINTERS)
		return;

	/* The event, after the ones before it. */
	pointer = &touch->pointers[touch->pointer_count];
	touch->pointer_count++;
	pointer->kind = kind;
	pointer->button = button;
	pointer->x = (int32_t)floor(x);
	pointer->y = (int32_t)floor(y);
	pointer->time = (uint32_t)(now / 1000U);
	pointer->serial = touch->serial;
}

/*
 * A touch pad's two fingers move the area under the pointer by the
 * event's y (pixels, as a wheel scrolls) at the compositor's time
 * (ws090-p019): libkeiland's scroller takes the area at their first move
 * (from where it is, unless it already owns it; a glide is caught) and
 * follows them.  Fingers on the screen keep it from them.
 */
static void
touch_pad(
	struct fm_touch *touch,
	const struct fm_touch_event *event,
	uint64_t time)
{
	int holding;
	int caught;

	/* Fingers on the screen hold the scroll. */
	if (touch->mode != TOUCH_MODE_NONE || touch->scroller == NULL)
		return;

	/* The first move: the area under the pointer, from its scroll unless the scroller owns it already. */
	holding = kl_scroller_axis_holding(touch->scroller);
	if (!holding) {
		if (event->area != FM_TOUCH_CONTENT && event->area != FM_TOUCH_SIDEBAR)
			return;
		touch_target(touch, event->area);
		if (!touch->moving)
			kl_scroller_set_position(touch->scroller, 0.0, (double)touch->scroll);
	}

	/* The scroller follows the fingers and owns the scroll until it rests. */
	caught = kl_scroller_axis(touch->scroller, 0.0, (double)event->y, time, event->arrival);
	touch->moving = 1;
	if (caught)
		fprintf(stderr, "ZFILES TOUCH caught source=finger area=%u scroll=%d\n", touch->target, touch->scroll);
}

/* The touch pad's fingers lift: the area flies on at their velocity, as libkeiland's scroller throws it (ws090-p019). */
static void
touch_pad_stop(
	struct fm_touch *touch,
	const struct fm_touch_event *event,
	uint64_t time)
{
	double vx;
	double vy;
	int flung;

	/* The scroller throws the area the fingers held. */
	flung = kl_scroller_axis_stop(touch->scroller, time, event->arrival, &vx, &vy);
	if (flung)
		fprintf(stderr, "ZFILES KINETIC fling source=finger area=%u vy=%.0f\n", touch->target, vy);
}
