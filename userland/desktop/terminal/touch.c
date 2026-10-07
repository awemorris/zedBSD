/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of the terminal (ws081-p011): the fingers scroll the
 * scrollback with libkeiland's gestures and scroll (kl_scroll, ws090-p015:
 * its own ends below 0 and the rubber band against the grid), and play the
 * pointer's left button for taps and a long press (touch.h).
 *
 * The view's position is kept in pixels back from the live screen; the
 * screen shows it as whole lines back (its view) and a pixel offset within
 * a line.  Past the live screen the offset is negative (the text moves up,
 * nothing below it), past the oldest line kept it passes a line's height.
 * The lines starting with "ZTERM TOUCH" are what the tests read.
 */

#include "touch.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* How often the view is placed while fingers are down or it glides, in milliseconds. */
#define TOUCH_TICK_MS		16

/* The oldest a wl_touch time may be and still be taken (older is another clock), in milliseconds. */
#define TOUCH_TIME_BEHIND	2000U

static uint64_t touch_time(const struct terminal_touch_event *event);
static double touch_position(const struct terminal_touch *touch, unsigned view, int offset);
static void touch_split(const struct terminal_touch *touch, double position, unsigned *view, int *offset);
static void touch_bounds(struct terminal_touch *touch);
static void touch_press(struct terminal_touch *touch, uint64_t now);
static void touch_gestures(struct terminal_touch *touch, uint64_t now);
static void touch_pointer(struct terminal_touch *touch, unsigned kind, double x, double y, uint64_t now);
static void touch_pad(struct terminal_touch *touch, double dy, uint64_t time, uint64_t now);
static void touch_pad_stop(struct terminal_touch *touch, uint64_t time, uint64_t now);

/*
 * Makes the gestures and the scroll.
 *
 * Returns 0, or ENOMEM.
 */
int
terminal_touch_open(
	struct terminal_touch *touch)
{
	int error;

	/* Nothing held yet. */
	memset(touch, 0, sizeof(*touch));

	/* The gestures of the window. */
	touch->gesture = kl_gesture_create();
	if (touch->gesture == NULL)
		return ENOMEM;

	/* The scroll of the view, down only. */
	error = kl_scroll_init(&touch->scroll, KL_SCROLL_Y);
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
terminal_touch_close(
	struct terminal_touch *touch)
{
	/* Both, when they were made (a scroll never made holds nothing). */
	kl_scroll_release(&touch->scroll);
	if (touch->gesture != NULL)
		kl_gesture_destroy(touch->gesture);
	memset(touch, 0, sizeof(*touch));
}

/*
 * Takes the screen's state for this round: which screen, a line's height,
 * the lines the scrollback keeps, the grid's height, and the view as the
 * screen has it.  A view other than the one the fingers last set (set
 * elsewhere) is taken over: a glide stops there, and a finger down drags on
 * from it.
 */
void
terminal_touch_layout(
	struct terminal_touch *touch,
	const void *screen,
	unsigned cell_height,
	unsigned history,
	unsigned grid_height,
	unsigned view,
	int offset)
{
	double position;
	int elsewhere;

	/* Nothing before the scroll was made. */
	if (touch->gesture == NULL)
		return;

	/* The layout, and the scroll's ends for it. */
	touch->cell_height = cell_height;
	if (touch->cell_height == 0U)
		touch->cell_height = 1U;
	touch->history = history;
	touch->grid_height = (double)grid_height;
	touch_bounds(touch);

	/* Another screen (another tab), or another view than the fingers set: taken over. */
	elsewhere = 0;
	if (screen != touch->screen)
		elsewhere = 1;
	if (view != touch->view || offset != touch->offset)
		elsewhere = 1;
	touch->screen = screen;
	touch->view = view;
	touch->offset = offset;
	if (!elsewhere)
		return;

	/* The scroll holds that view at once (a glide stops); a finger down drags on from it. */
	position = touch_position(touch, view, offset);
	kl_scroll_move_to(&touch->scroll, 0.0, -position, 0, terminal_touch_clock());
	touch->changed = 0;
	if (touch->moving && !touch->pressed) {
		touch->moving = 0;
		printf("ZTERM TOUCH stop view=%u offset=%d\n", view, offset);
		fflush(stdout);
	}

	/* A finger down drags on from it, pressed again at the next tick's time. */
	if (touch->pressed)
		touch->repress = 1;
}

/*
 * Takes one touch input of the window: the first finger presses the
 * scroll; a finger after a long press moves the pointer's selection.
 */
void
terminal_touch_event(
	struct terminal_touch *touch,
	const struct terminal_touch_event *event)
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
	case TERMINAL_TOUCH_DOWN:
		/* The first finger presses the scroll, and its serial goes with the pointer's presses. */
		if (touch->followed == 0U) {
			touch->first_id = event->id;
			touch->serial = event->serial;
			touch->selecting = 0;
			touch->last_x = event->x;
			touch->last_y = event->y;
			touch_press(touch, event->arrival);
		}

		/* The gestures follow it. */
		error = kl_gesture_down(touch->gesture, event->id, time, event->arrival, event->x, event->y);
		if (error == 0)
			touch->followed++;
		break;
	case TERMINAL_TOUCH_MOTION:
		/* After a long press the first finger moves the selection. */
		(void)kl_gesture_motion(touch->gesture, event->id, time, event->arrival, event->x, event->y);
		if (event->id == touch->first_id) {
			touch->last_x = event->x;
			touch->last_y = event->y;
			if (touch->selecting)
				touch_pointer(touch, TERMINAL_TOUCH_POINTER_MOTION, event->x, event->y, event->arrival);
		}

		/* Nothing else. */
		break;
	case TERMINAL_TOUCH_UP:
		/* The first finger's lift ends a selection. */
		error = kl_gesture_up(touch->gesture, event->id, time);
		if (error == 0 && touch->followed > 0U)
			touch->followed--;
		if (touch->selecting && event->id == touch->first_id) {
			touch_pointer(touch, TERMINAL_TOUCH_RELEASE, touch->last_x, touch->last_y, event->arrival);
			touch->selecting = 0;
		}

		/* Nothing else. */
		break;
	case TERMINAL_TOUCH_PAD:
		/* A touch pad's fingers scroll the view (ws090-p019). */
		touch_pad(touch, event->y, time, event->arrival);
		break;
	case TERMINAL_TOUCH_PAD_STOP:
		/* They lift: the view flies on. */
		touch_pad_stop(touch, time, event->arrival);
		break;
	case TERMINAL_TOUCH_CANCEL:
		/* The compositor took every finger: a selection ends where it is. */
		kl_gesture_cancel(touch->gesture);
		touch->followed = 0;
		if (touch->selecting) {
			touch_pointer(touch, TERMINAL_TOUCH_RELEASE, touch->last_x, touch->last_y, event->arrival);
			touch->selecting = 0;
		}

		/* Nothing else. */
		break;
	default:
		break;
	}

	/* What the fingers mean so far. */
	touch_gestures(touch, event->arrival);
}

/*
 * Moves time on for the fingers: finds a long press, moves the scroll with
 * a drag, and sets the view where the scroll is at the frame's time.
 *
 * Returns how many milliseconds until the view should be set again (-1
 * when it rests and no finger is down).
 */
int
terminal_touch_tick(
	struct terminal_touch *touch,
	uint64_t now)
{
	double dx;
	double dy;
	unsigned view;
	int offset;
	int animating;
	int holding;
	int error;

	/* Nothing without the gestures. */
	if (touch->gesture == NULL)
		return -1;

	/* The gestures found by now (a long press among them). */
	touch_gestures(touch, now);

	/* A view taken over under a finger: the drag goes on from it. */
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

	/* A drag moves the scroll with the fingers, resampled for the frame. */
	if (touch->dragging &&
	    touch->pressed) {
		error = kl_gesture_drag_offset(touch->gesture, now, &dx, &dy);
		if (error == 0)
			kl_scroll_drag(&touch->scroll, dx - touch->base_x, dy - touch->base_y);
	}

	/* The view where the scroll is at the frame's time. */
	animating = kl_scroll_step(&touch->scroll, now);
	if (touch->moving) {
		touch_split(touch, -touch->scroll.y, &view, &offset);
		if (view != touch->view || offset != touch->offset) {
			touch->view = view;
			touch->offset = offset;
			touch->changed = 1;
		}
	}

	/* The view rests once it stops with no finger on it, on the screen or on a touch pad. */
	holding = kl_scroll_axis_holding(&touch->scroll);
	if (!animating &&
	    !touch->pressed &&
	    touch->followed == 0U &&
	    !holding &&
	    touch->moving) {
		touch->moving = 0;
		printf("ZTERM TOUCH rest view=%u offset=%d\n", touch->view, touch->offset);
		fflush(stdout);
	}

	/* Fingers down (on the screen or a touch pad) or a gliding view want the next tick soon. */
	if (animating ||
	    holding ||
	    touch->followed > 0U)
		return TOUCH_TICK_MS;

	/* Nothing moves: no tick is due. */
	return -1;
}

/*
 * Takes the view the fingers set since it was last taken: lines back from
 * the live screen and the pixel offset within a line.  Returns 1 with a
 * new view, 0 when there is none.
 */
int
terminal_touch_view(
	struct terminal_touch *touch,
	unsigned *view,
	int *offset)
{
	/* Nothing new. */
	if (!touch->changed)
		return 0;

	/* The view, taken. */
	*view = touch->view;
	*offset = touch->offset;
	touch->changed = 0;

	/* Succeeded: a new view. */
	return 1;
}

/*
 * Takes the oldest pointer event the fingers made.  Returns 1 with one, 0
 * when there is none.
 */
int
terminal_touch_take_pointer(
	struct terminal_touch *touch,
	struct terminal_touch_pointer *pointer)
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
 * events' arrival and of terminal_touch_tick.
 */
uint64_t
terminal_touch_clock(void)
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
	const struct terminal_touch_event *event)
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

/* Reports a view's position in pixels back from the live screen. */
static double
touch_position(
	const struct terminal_touch *touch,
	unsigned view,
	int offset)
{
	/* Whole lines, and the offset within one. */
	return (double)view * (double)touch->cell_height + (double)offset;
}

/*
 * Splits a position (pixels back from the live screen) into whole lines
 * back, within what the scrollback keeps, and the pixel offset left over
 * (negative past the live screen, a line or more past the oldest line).
 */
static void
touch_split(
	const struct terminal_touch *touch,
	double position,
	unsigned *view,
	int *offset)
{
	double lines;

	/* Past the live screen: the live screen, moved up. */
	if (position < 0.0) {
		*view = 0;
		*offset = (int)lround(position);
		return;
	}

	/* Whole lines, no further back than the oldest kept. */
	lines = floor(position / (double)touch->cell_height);
	if (lines > (double)touch->history)
		lines = (double)touch->history;
	*view = (unsigned)lines;
	*offset = (int)lround(position - lines * (double)touch->cell_height);
}

/* Gives the scroll the scrollback's ends and the grid's height for the rubber band (only when they changed). */
static void
touch_bounds(
	struct terminal_touch *touch)
{
	double top;
	double height;

	/* From the oldest line kept (a negative position) to the live screen. */
	top = -(double)touch->history * (double)touch->cell_height;
	height = touch->grid_height;
	if (height < 1.0)
		height = 1.0;

	/* Unchanged ends leave the scroll alone. */
	if (top == touch->bounds_top &&
	    height == touch->bounds_height)
		return;

	/* The new ends; only down scrolls. */
	(void)kl_scroll_set_bounds(&touch->scroll, 0.0, 0.0, top, 0.0, 1.0, height);
	touch->bounds_top = top;
	touch->bounds_height = height;
}

/*
 * Presses the scroll where the view is: it takes the view over (from where
 * the screen has it, when it did not own it), and a press on a gliding
 * view catches it.
 */
static void
touch_press(
	struct terminal_touch *touch,
	uint64_t now)
{
	double position;
	double dx;
	double dy;
	int caught;
	int error;

	/* The view's place, unless the scroll already owns it. */
	if (!touch->moving) {
		position = touch_position(touch, touch->view, touch->offset);
		kl_scroll_move_to(&touch->scroll, 0.0, -position, 0, now);
	}

	/* The press; the first press on a gliding view catches it. */
	caught = kl_scroll_press(&touch->scroll, now);
	if (!touch->pressed) {
		touch->caught = caught;
		if (caught) {
			printf("ZTERM TOUCH caught view=%u offset=%d\n", touch->view, touch->offset);
			fflush(stdout);
		}
	}

	/* The scroll owns the view from here. */
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
	struct terminal_touch *touch,
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
			/* A click, unless the touch caught a gliding view (two quick ones make a double click). */
			if (!touch->caught) {
				touch_pointer(touch, TERMINAL_TOUCH_PRESS, gesture.x, gesture.y, now);
				touch_pointer(touch, TERMINAL_TOUCH_RELEASE, gesture.x, gesture.y, now);
			}

			/* The tests' line. */
			printf("ZTERM TOUCH tap x=%.0f y=%.0f caught=%d\n", gesture.x, gesture.y, touch->caught);
			fflush(stdout);
			break;
		case KL_GESTURE_LONG_PRESS:
			/* The button is held there (the main loop selects the word, or holds the selection to drag it). */
			if (!touch->caught && touch->followed == 1U) {
				touch->selecting = 1;
				touch_pointer(touch, TERMINAL_TOUCH_HOLD, gesture.x, gesture.y, now);
				printf("ZTERM TOUCH select x=%.0f y=%.0f\n", gesture.x, gesture.y);
				fflush(stdout);
			}

			/* Nothing else. */
			break;
		case KL_GESTURE_DRAG_BEGIN:
			/* A drag scrolls, unless it grows a selection. */
			if (!touch->selecting) {
				touch->dragging = 1;
				printf("ZTERM TOUCH drag fingers=%u\n", touch->followed);
				fflush(stdout);
			}

			/* Nothing else. */
			break;
		case KL_GESTURE_DRAG_END:
			/* The view glides on at the finger's velocity. */
			if (touch->dragging) {
				flung = kl_scroll_fling(&touch->scroll, gesture.vx, gesture.vy, now);
				printf("ZTERM TOUCH release vy=%.0f view=%u offset=%d\n", gesture.vy, touch->view, touch->offset);
				if (flung)
					printf("ZTERM KINETIC fling source=touch vy=%.0f\n", gesture.vy);
				fflush(stdout);
				touch->pressed = 0;
			}

			/* The drag is over. */
			touch->dragging = 0;
			break;
		case KL_GESTURE_CANCEL:
			/* No glide. */
			if (touch->pressed)
				kl_scroll_cancel(&touch->scroll, now);
			touch->pressed = 0;
			touch->dragging = 0;
			break;
		default:
			break;
		}
	}

	/* The last finger lifted without a drag: the scroll is let go still (a view past an end springs back). */
	if (touch->followed == 0U &&
	    touch->pressed) {
		(void)kl_scroll_fling(&touch->scroll, 0.0, 0.0, now);
		touch->pressed = 0;
		touch->dragging = 0;
	}
}

/* Makes a pointer event at a place, for the main loop (a full queue drops it). */
static void
touch_pointer(
	struct terminal_touch *touch,
	unsigned kind,
	double x,
	double y,
	uint64_t now)
{
	struct terminal_touch_pointer *pointer;

	/* Room for it. */
	if (touch->pointer_count >= TERMINAL_TOUCH_POINTERS)
		return;

	/* The event, after the ones before it. */
	pointer = &touch->pointers[touch->pointer_count];
	touch->pointer_count++;
	pointer->kind = kind;
	pointer->x = (int32_t)floor(x);
	pointer->y = (int32_t)floor(y);
	pointer->time = (uint32_t)(now / 1000U);
	pointer->serial = touch->serial;
}

/*
 * A touch pad's two fingers move the view by dy (pixels, as a wheel
 * scrolls: down toward the live screen) at the compositor's time
 * (ws090-p019): the scroll takes the view at their first move (from where
 * the screen has it, unless it already owns it; a glide is caught) and
 * follows them.  Fingers on the screen keep it from them.
 */
static void
touch_pad(
	struct terminal_touch *touch,
	double dy,
	uint64_t time,
	uint64_t now)
{
	double position;
	int holding;
	int caught;

	/* Fingers on the screen hold the view, and nothing moves before the scroll was made. */
	if (touch->followed > 0U || touch->gesture == NULL)
		return;

	/* The first move: the scroll from the view the screen shows, unless it owns it already. */
	holding = kl_scroll_axis_holding(&touch->scroll);
	if (!holding && !touch->moving) {
		position = touch_position(touch, touch->view, touch->offset);
		kl_scroll_move_to(&touch->scroll, 0.0, -position, 0, now);
	}

	/* The scroll follows the fingers and owns the view until it rests. */
	caught = kl_scroll_axis_at(&touch->scroll, 0.0, dy, KL_AXIS_SOURCE_FINGER, time, now);
	touch->moving = 1;
	if (caught) {
		printf("ZTERM TOUCH caught source=finger view=%u offset=%d\n", touch->view, touch->offset);
		fflush(stdout);
	}
}

/* The touch pad's fingers lift: the view flies on at their velocity, as the scroll throws it (ws090-p019). */
static void
touch_pad_stop(
	struct terminal_touch *touch,
	uint64_t time,
	uint64_t now)
{
	double vx;
	double vy;
	int flung;

	/* The scroll throws the view the fingers held. */
	flung = kl_scroll_axis_stop_at(&touch->scroll, time, now, &vx, &vy);
	if (flung) {
		printf("ZTERM KINETIC fling source=finger vy=%.0f\n", vy);
		fflush(stdout);
	}
}
