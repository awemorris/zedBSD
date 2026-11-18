/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of the browser's window (ws081-p006): the fingers
 * scroll the page with libkeiland's gestures and scroller, stretch it past
 * its ends, and play the pointer's buttons for taps and long presses
 * (touch.h).  The lines starting with "ZBROWSER TOUCH" (on standard
 * output, as the shell's own) are what the tests read.
 */

#include "shell/touch.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* How often the scroll is set while fingers are down or the page glides, in milliseconds. */
#define TOUCH_TICK_MS		16

/* The oldest a wl_touch time may be and still be taken (older is another clock), in milliseconds. */
#define TOUCH_TIME_BEHIND	2000U

/* How far the page's scroll may differ from the fingers' before it counts as set elsewhere, in pixels. */
#define TOUCH_SCROLL_SLACK	0.5

/* How small a change of the scroll or the overscroll still counts, in pixels. */
#define TOUCH_CHANGE_MIN	0.01

static uint64_t touch_time(const struct shell_touch_event *event);
static void touch_pad(struct shell_touch *touch, const struct shell_touch_event *event, uint64_t time);
static void touch_pad_stop(struct shell_touch *touch, const struct shell_touch_event *event, uint64_t time);
static void touch_bounds(struct shell_touch *touch);
static void touch_press(struct shell_touch *touch, uint64_t now);
static void touch_place(struct shell_touch *touch, double y);
static void touch_gestures(struct shell_touch *touch, uint64_t now);
static void touch_pointer(struct shell_touch *touch, unsigned kind, int button, double x, double y);
static void touch_click(struct shell_touch *touch, int button, double x, double y);

/*
 * Makes the gestures and the scroller.
 *
 * Returns 0, or ENOMEM.
 */
int
shell_touch_open(
	struct shell_touch *touch)
{
	/* Nothing held yet; the scroller's bounds are set by the first layout. */
	memset(touch, 0, sizeof(*touch));
	touch->bounds_largest = -1.0;

	/* The gestures of the window. */
	touch->gesture = kl_gesture_create();
	if (touch->gesture == NULL)
		return ENOMEM;

	/* The scroller of the page. */
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
shell_touch_close(
	struct shell_touch *touch)
{
	/* Both, when they were made. */
	if (touch->scroller != NULL)
		kl_scroller_destroy(touch->scroller);
	if (touch->gesture != NULL)
		kl_gesture_destroy(touch->gesture);
	memset(touch, 0, sizeof(*touch));
}

/*
 * Takes the page as it is this round: the page's token (another page is
 * another number), its scroll, how far it scrolls and the view's height
 * (pixels).  A scroll other than the one the fingers last set, or another
 * page, is taken over: a glide and a stretch stop there, and a finger down
 * drags on from it.
 */
void
shell_touch_layout(
	struct shell_touch *touch,
	unsigned long token,
	double scroll,
	double largest,
	double height)
{
	double apart;
	int elsewhere;

	/* Whether the scroll changed without the fingers. */
	elsewhere = 0;
	apart = fabs(scroll - touch->scroll);
	if (token != touch->token) {
		elsewhere = 1;
	} else if (apart > TOUCH_SCROLL_SLACK) {
		elsewhere = 1;
	}

	/* The page's range and the view. */
	touch->token = token;
	touch->largest = largest;
	if (touch->largest < 0.0)
		touch->largest = 0.0;
	touch->height = height;
	if (touch->scroller == NULL)
		return;
	touch_bounds(touch);

	/* The same scroll goes on as the fingers have it. */
	if (!elsewhere)
		return;

	/* The scroller holds the new scroll, with no stretch (which the main loop takes back from the view). */
	touch->scroll = scroll;
	touch->changed = 0;
	if (touch->overscroll != 0.0) {
		touch->overscroll = 0.0;
		touch->changed = 1;
	}

	/* The scroller is there from now on. */
	kl_scroller_set_position(touch->scroller, 0.0, scroll);

	/* A glide stops. */
	if (touch->moving && !touch->pressed) {
		touch->moving = 0;
		printf("ZBROWSER TOUCH stop scroll=%.0f\n", scroll);
		fflush(stdout);
	}

	/* A finger down drags on from it, pressed again at the next tick's time. */
	if (touch->pressed)
		touch->repress = 1;
}

/*
 * Takes one touch input of the window: the gestures follow every finger,
 * and the first presses the scroller.
 */
void
shell_touch_event(
	struct shell_touch *touch,
	const struct shell_touch_event *event)
{
	uint64_t time;
	int error;

	/* Nothing without the gestures. */
	if (touch->gesture == NULL)
		return;

	/* The event's time. */
	time = touch_time(event);

	/* Follows the finger by the kind of touch. */
	switch (event->type) {
	case SHELL_TOUCH_DOWN:
		/* The first finger presses the scroller (catching a glide), and its place is a long press's. */
		if (touch->followed == 0U) {
			touch->held = 0;
			touch->press_x = event->x;
			touch->press_y = event->y;
			touch_press(touch, event->arrival);
		}

		/* The gestures follow it. */
		error = kl_gesture_down(touch->gesture, event->id, time, event->arrival, event->x, event->y);
		if (error == 0)
			touch->followed++;
		break;
	case SHELL_TOUCH_MOTION:
		(void)kl_gesture_motion(touch->gesture, event->id, time, event->arrival, event->x, event->y);
		break;
	case SHELL_TOUCH_UP:
		/* The gestures hear the lift. */
		error = kl_gesture_up(touch->gesture, event->id, time);
		if (error == 0 && touch->followed > 0U)
			touch->followed--;

		/* The last finger of a long press that did not move: a click of the secondary button where it pressed. */
		if (touch->followed == 0U && touch->held) {
			touch->held = 0;
			touch_click(touch, SHELL_TOUCH_SECONDARY, touch->press_x, touch->press_y);
			printf("ZBROWSER TOUCH context x=%.0f y=%.0f\n", (double)touch->press_x, (double)touch->press_y);
			fflush(stdout);
		}

		/* Nothing else. */
		break;
	case SHELL_TOUCH_PAD:
		/* A touch pad's fingers scroll the page (ws090-p019). */
		touch_pad(touch, event, time);
		break;
	case SHELL_TOUCH_PAD_STOP:
		/* They lift: the page flies on. */
		touch_pad_stop(touch, event, time);
		break;
	case SHELL_TOUCH_CANCEL:
		/* The compositor took the fingers: whatever they did ends without its lift. */
		kl_gesture_cancel(touch->gesture);
		touch->followed = 0;
		touch->held = 0;
		printf("ZBROWSER TOUCH cancel\n");
		fflush(stdout);
		break;
	default:
		break;
	}

	/* What the fingers mean so far. */
	touch_gestures(touch, event->arrival);
}

/*
 * Moves time on for the fingers: finds a long press, moves the scroller
 * with a drag, and places the page where the scroller is at the frame's
 * time.
 *
 * Returns how many milliseconds until the scroll should be set again (-1
 * when it rests and no finger is down).
 */
int
shell_touch_tick(
	struct shell_touch *touch,
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
	    touch->followed == 0U)
		return -1;

	/* A drag moves the scroller with the fingers, resampled for the frame. */
	if (touch->dragging &&
	    touch->pressed) {
		error = kl_gesture_drag_offset(touch->gesture, now, &dx, &dy);
		if (error == 0)
			kl_scroller_drag(touch->scroller, dx - touch->base_x, dy - touch->base_y);
	}

	/* The page where the scroller is at the frame's time. */
	animating = kl_scroller_step(touch->scroller, now, &x, &y);
	if (touch->moving)
		touch_place(touch, y);

	/* The scroll rests once it stops with no finger on it, on the screen or on a touch pad. */
	holding = kl_scroller_axis_holding(touch->scroller);
	if (!animating &&
	    !touch->pressed &&
	    touch->followed == 0U &&
	    !holding &&
	    touch->moving) {
		touch->moving = 0;
		printf("ZBROWSER TOUCH rest scroll=%.0f\n", touch->scroll);
		fflush(stdout);
	}

	/* Fingers down (on the screen or a touch pad) or a glide want the next tick soon. */
	if (animating ||
	    holding ||
	    touch->followed > 0U)
		return TOUCH_TICK_MS;

	/* Nothing moves: no tick is due. */
	return -1;
}

/*
 * Takes the scroll and the overscroll the fingers set since they were last
 * taken.  Returns 1 with new ones, 0 when there are none.
 */
int
shell_touch_scroll(
	struct shell_touch *touch,
	double *scroll,
	double *overscroll)
{
	/* Nothing new. */
	if (!touch->changed)
		return 0;

	/* Both, taken. */
	*scroll = touch->scroll;
	*overscroll = touch->overscroll;
	touch->changed = 0;

	/* Succeeded: a new scroll. */
	return 1;
}

/*
 * Takes the oldest pointer event the fingers made.  Returns 1 with one, 0
 * when there is none.
 */
int
shell_touch_take_pointer(
	struct shell_touch *touch,
	struct shell_touch_pointer *pointer)
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
 * events' arrival and of shell_touch_tick.
 */
uint64_t
shell_touch_clock(void)
{
	/* libkeiland's clock, in microseconds. */
	return kl_clock_us();
}

/*
 * Turns a wl_touch time (the compositor's milliseconds, the low 32 bits)
 * into microseconds of the full clock, by the time the event was read; a
 * time far behind or ahead of that is taken as the reading's time.
 */
static uint64_t
touch_time(
	const struct shell_touch_event *event)
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

/* Gives the scroller the page's range and the view's height (only when they changed). */
static void
touch_bounds(
	struct shell_touch *touch)
{
	double height;

	/* The view's height, at least a pixel. */
	height = touch->height;
	if (height < 1.0)
		height = 1.0;

	/* Unchanged bounds leave the scroller alone. */
	if (touch->largest == touch->bounds_largest &&
	    height == touch->bounds_height)
		return;

	/* The new bounds; only down scrolls. */
	(void)kl_scroller_set_bounds(touch->scroller, 0.0, 0.0, 0.0, touch->largest, 1.0, height);
	touch->bounds_largest = touch->largest;
	touch->bounds_height = height;
}

/*
 * Presses the scroller where the page is: it takes the scroll over (from
 * where the page is, when it did not own it), and a press on a glide
 * catches it.
 */
static void
touch_press(
	struct shell_touch *touch,
	uint64_t now)
{
	double dx;
	double dy;
	int caught;
	int error;

	/* The page's scroll, unless the scroller already owns it. */
	if (!touch->moving)
		kl_scroller_set_position(touch->scroller, 0.0, touch->scroll);

	/* The press; the first press on a glide catches it. */
	caught = kl_scroller_press(touch->scroller, now);
	if (!touch->pressed) {
		touch->caught = caught;
		if (caught) {
			printf("ZBROWSER TOUCH caught scroll=%.0f\n", touch->scroll);
			fflush(stdout);
		}
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
 * Places the page where the scroller is: the scroll within the document,
 * and the rest past an end as the overscroll (positive when the content is
 * pulled down past the top).
 */
static void
touch_place(
	struct shell_touch *touch,
	double y)
{
	double scroll;
	double overscroll;
	double moved;
	double stretched;

	/* The scroll within the document. */
	scroll = y;
	if (scroll < 0.0)
		scroll = 0.0;
	if (scroll > touch->largest)
		scroll = touch->largest;

	/* The stretch past an end. */
	overscroll = scroll - y;

	/* A change the main loop takes. */
	moved = fabs(scroll - touch->scroll);
	stretched = fabs(overscroll - touch->overscroll);
	if (moved >= TOUCH_CHANGE_MIN)
		touch->changed = 1;
	if (stretched >= TOUCH_CHANGE_MIN)
		touch->changed = 1;
	touch->scroll = scroll;
	touch->overscroll = overscroll;
}

/* Takes the gestures found so far and does what each means. */
static void
touch_gestures(
	struct shell_touch *touch,
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
			/* A click of the primary button there, unless the touch caught a glide. */
			if (!touch->caught)
				touch_click(touch, SHELL_TOUCH_PRIMARY, gesture.x, gesture.y);

			/* The tests' line. */
			printf("ZBROWSER TOUCH tap x=%.0f y=%.0f caught=%d\n", gesture.x, gesture.y, touch->caught);
			fflush(stdout);
			break;
		case KL_GESTURE_LONG_PRESS:
			/* One finger's long press waits for its lift (the secondary button's click). */
			if (!touch->caught && touch->followed == 1U) {
				touch->held = 1;
				touch->press_x = (float)gesture.x;
				touch->press_y = (float)gesture.y;
				printf("ZBROWSER TOUCH long-press x=%.0f y=%.0f\n", gesture.x, gesture.y);
				fflush(stdout);
			}

			/* Nothing else. */
			break;
		case KL_GESTURE_DRAG_BEGIN:
			/* The drag scrolls (a long press before it clicks nothing). */
			touch->held = 0;
			touch->dragging = 1;
			printf("ZBROWSER TOUCH drag fingers=%u scroll=%.0f largest=%.0f\n", touch->followed, touch->scroll, touch->largest);
			fflush(stdout);
			break;
		case KL_GESTURE_DRAG_END:
			/* The page glides on at the fingers' velocity. */
			if (touch->dragging) {
				flung = kl_scroller_release(touch->scroller, now, gesture.vx, gesture.vy);
				printf("ZBROWSER TOUCH release vy=%.0f scroll=%.0f\n", gesture.vy, touch->scroll);
				if (flung)
					printf("ZBROWSER KINETIC fling source=touch vy=%.0f\n", gesture.vy);
				fflush(stdout);
				touch->pressed = 0;
			}

			/* The drag is over. */
			touch->dragging = 0;
			break;
		case KL_GESTURE_CANCEL:
			/* No glide (a stretch springs back). */
			if (touch->pressed)
				kl_scroller_cancel(touch->scroller, now);
			touch->pressed = 0;
			touch->dragging = 0;
			break;
		default:
			break;
		}
	}

	/* The last finger lifted without a drag: the scroller is let go still (a stretch springs back). */
	if (touch->followed == 0U &&
	    touch->pressed) {
		kl_scroller_release(touch->scroller, now, 0.0, 0.0);
		touch->pressed = 0;
		touch->dragging = 0;
	}
}

/* Makes a pointer event at a place, for the main loop (a full queue drops it). */
static void
touch_pointer(
	struct shell_touch *touch,
	unsigned kind,
	int button,
	double x,
	double y)
{
	struct shell_touch_pointer *pointer;

	/* Room for it. */
	if (touch->pointer_count >= SHELL_TOUCH_POINTERS)
		return;

	/* The event, after the ones before it. */
	pointer = &touch->pointers[touch->pointer_count];
	touch->pointer_count++;
	pointer->kind = kind;
	pointer->button = button;
	pointer->x = (float)x;
	pointer->y = (float)y;
}

/* Makes a click of a button at a place: the pointer comes there, presses and releases. */
static void
touch_click(
	struct shell_touch *touch,
	int button,
	double x,
	double y)
{
	/* The three events, in order. */
	touch_pointer(touch, SHELL_TOUCH_POINTER_MOTION, button, x, y);
	touch_pointer(touch, SHELL_TOUCH_POINTER_PRESS, button, x, y);
	touch_pointer(touch, SHELL_TOUCH_POINTER_RELEASE, button, x, y);
}

/*
 * A touch pad's two fingers move the page by the event's y (pixels, as a
 * wheel scrolls) at the compositor's time (ws090-p019): libkeiland's
 * scroller takes the page at their first move (from where it is, unless
 * it already owns it; a glide is caught) and follows them.  Fingers on the
 * screen keep it from them.
 */
static void
touch_pad(
	struct shell_touch *touch,
	const struct shell_touch_event *event,
	uint64_t time)
{
	int holding;
	int caught;

	/* Fingers on the screen hold the page. */
	if (touch->followed > 0U || touch->scroller == NULL)
		return;

	/* The first move: the scroller from the page's scroll, unless it owns it already. */
	holding = kl_scroller_axis_holding(touch->scroller);
	if (!holding && !touch->moving)
		kl_scroller_set_position(touch->scroller, 0.0, touch->scroll);

	/* The scroller follows the fingers and owns the scroll until it rests. */
	caught = kl_scroller_axis(touch->scroller, 0.0, (double)event->y, time, event->arrival);
	touch->moving = 1;
	if (caught) {
		printf("ZBROWSER TOUCH caught source=finger scroll=%.0f\n", touch->scroll);
		fflush(stdout);
	}
}

/* The touch pad's fingers lift: the page flies on at their velocity, as libkeiland's scroller throws it (ws090-p019). */
static void
touch_pad_stop(
	struct shell_touch *touch,
	const struct shell_touch_event *event,
	uint64_t time)
{
	double vx;
	double vy;
	int flung;

	/* The scroller throws the page the fingers held. */
	flung = kl_scroller_axis_stop(touch->scroller, time, event->arrival, &vx, &vy);
	if (flung) {
		printf("ZBROWSER KINETIC fling source=finger vy=%.0f\n", vy);
		fflush(stdout);
	}
}
