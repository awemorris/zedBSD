/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pure state machine that turns touch contacts on an X11 tab into
 * pointer motion, button clicks, and wheel notches.
 */

#include "x11-touch.h"

#include <math.h>
#include <string.h>

/* Thresholds in screen pixels, microseconds, and relative pinch ratio. */
#define TOUCH_DRAG_PX		8
#define TOUCH_TWO_TAP_PX	16
#define TOUCH_WHEEL_PX		60
#define TOUCH_TWO_TAP_US	300000ULL
#define TOUCH_PINCH_THRESHOLD	0.08

static int find_slot(const struct x11_touch *touch, int32_t id);
static int find_free_slot(const struct x11_touch *touch);
static void reset_preserving_mode(struct x11_touch *touch);
static void push_action(struct x11_touch_action *out, unsigned room, unsigned *count, unsigned kind, unsigned button, int pressed, int32_t x, int32_t y, int notches, int32_t dx, int32_t dy, double scale_ratio);
static int moved_beyond(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int limit);
static int32_t average_x(const struct x11_touch *touch);
static int32_t average_y(const struct x11_touch *touch);
static double finger_distance(const struct x11_touch *touch);

/*
 * Consumes one touch event and writes up to room pointer actions into out.
 * Returns the number of actions written.
 */
unsigned
x11_touch_take(
	struct x11_touch *touch,
	unsigned kind,
	int32_t id,
	int32_t x,
	int32_t y,
	uint64_t time_us,
	struct x11_touch_action *out,
	unsigned room)
{
	unsigned count;
	int slot;
	int32_t avg_x;
	int32_t avg_y;
	int32_t dx;
	int32_t dy;
	int32_t delta_y;
	double dist;
	double delta;
	int moved;

	count = 0U;

	/* Cancelling releases any held left button and clears all gesture state. */
	if (kind == X11_TOUCH_CANCEL) {
		if (touch->pressed_left)
			push_action(out, room, &count, X11_TOUCH_BUTTON, 1U, 0, touch->last_x, touch->last_y, 0, 0, 0, 0.0);

		reset_preserving_mode(touch);
		return count;
	}

	/* A new contact either starts a single-finger touch or enters two-finger mode. */
	if (kind == X11_TOUCH_DOWN) {
		slot = find_free_slot(touch);
		if (slot < 0)
			return 0U;

		touch->active[slot] = 1;
		touch->id[slot] = id;
		touch->start_x[slot] = x;
		touch->start_y[slot] = y;
		touch->x[slot] = x;
		touch->y[slot] = y;

		/* Second finger cancels any pending or active left press and begins two-finger mode. */
		if (touch->active[0] && touch->active[1]) {
			if (touch->pressed_left) {
				push_action(out, room, &count, X11_TOUCH_BUTTON, 1U, 0, touch->last_x, touch->last_y, 0, 0, 0, 0.0);
				touch->pressed_left = 0;
			}

			touch->two_finger = 1;
			touch->two_done = 0;
			touch->moved_far = 0;
			touch->two_pinch_active = 0;
			touch->wheel_accum_y = 0;
			touch->two_start_time_us = time_us;
			touch->two_last_avg_x = average_x(touch);
			touch->two_last_avg_y = average_y(touch);
			touch->two_pinch_dist = finger_distance(touch);
			return count;
		}

		/* First finger moves the pointer without pressing yet. */
		if (!touch->two_finger) {
			touch->last_x = x;
			touch->last_y = y;
			push_action(out, room, &count, X11_TOUCH_MOTION, 0U, 0, x, y, 0, 0, 0, 0.0);
		}

		return count;
	}

	/* Remaining events apply only to an already-tracked contact. */
	slot = find_slot(touch, id);
	if (slot < 0)
		return 0U;

	if (kind == X11_TOUCH_MOVE) {
		touch->x[slot] = x;
		touch->y[slot] = y;

		/* Two-finger movement drives pan/pinch in ROOT mode or wheel in WINDOW mode. */
		if (touch->two_finger) {
			moved = moved_beyond(touch->start_x[slot], touch->start_y[slot], x, y, TOUCH_TWO_TAP_PX);
			if (moved)
				touch->moved_far = 1;

			if (touch->active[0] && touch->active[1]) {
				avg_x = average_x(touch);
				avg_y = average_y(touch);
				if (touch->mode == X11_TOUCH_MODE_ROOT) {
					dx = avg_x - touch->two_last_avg_x;
					dy = avg_y - touch->two_last_avg_y;
					touch->two_last_avg_x = avg_x;
					touch->two_last_avg_y = avg_y;
					if (dx != 0 || dy != 0) {
						push_action(out, room, &count, X11_TOUCH_PAN, 0U, 0, avg_x, avg_y, 0, dx, dy, 0.0);
						touch->two_done = 1;
					}

					dist = finger_distance(touch);
					if (touch->two_pinch_dist > 1.0) {
						delta = fabs(dist - touch->two_pinch_dist);
						if (!touch->two_pinch_active) {
							if (delta / touch->two_pinch_dist >= TOUCH_PINCH_THRESHOLD) {
								touch->two_pinch_active = 1;
								push_action(out, room, &count, X11_TOUCH_PINCH, 0U, 0, avg_x, avg_y, 0, 0, 0, dist / touch->two_pinch_dist);
								touch->two_pinch_dist = dist;
								touch->two_done = 1;
							}
						} else if (delta > 0.5) {
							push_action(out, room, &count, X11_TOUCH_PINCH, 0U, 0, avg_x, avg_y, 0, 0, 0, dist / touch->two_pinch_dist);
							touch->two_pinch_dist = dist;
							touch->two_done = 1;
						}
					}
				} else {
					delta_y = touch->two_last_avg_y - avg_y;
					touch->two_last_avg_y = avg_y;
					touch->wheel_accum_y += delta_y;

					/* Moving fingers upward (delta_y > 0) scrolls down (positive notches). */
					while (touch->wheel_accum_y >= TOUCH_WHEEL_PX) {
						push_action(out, room, &count, X11_TOUCH_WHEEL, 0U, 0, touch->last_x, touch->last_y, 1, 0, 0, 0.0);
						touch->wheel_accum_y -= TOUCH_WHEEL_PX;
						touch->two_done = 1;
					}

					while (touch->wheel_accum_y <= -TOUCH_WHEEL_PX) {
						push_action(out, room, &count, X11_TOUCH_WHEEL, 0U, 0, touch->last_x, touch->last_y, -1, 0, 0, 0.0);
						touch->wheel_accum_y += TOUCH_WHEEL_PX;
						touch->two_done = 1;
					}
				}
			}

			return count;
		}

		/* Single-finger movement presses left once it passes the drag threshold. */
		if (!touch->pressed_left) {
			moved = moved_beyond(touch->start_x[slot], touch->start_y[slot], x, y, TOUCH_DRAG_PX);
			if (moved) {
				push_action(out, room, &count, X11_TOUCH_BUTTON, 1U, 1, touch->start_x[slot], touch->start_y[slot], 0, 0, 0, 0.0);
				touch->pressed_left = 1;
			}
		}

		touch->last_x = x;
		touch->last_y = y;
		push_action(out, room, &count, X11_TOUCH_MOTION, 0U, 0, x, y, 0, 0, 0, 0.0);
		return count;
	}

	if (kind == X11_TOUCH_UP) {
		touch->x[slot] = x;
		touch->y[slot] = y;
		touch->active[slot] = 0;

		/* Lifting a finger in two-finger mode may complete a two-finger right tap. */
		if (touch->two_finger) {
			moved = moved_beyond(touch->start_x[slot], touch->start_y[slot], x, y, TOUCH_TWO_TAP_PX);
			if (moved)
				touch->moved_far = 1;

			if (!touch->two_done && !touch->moved_far && (time_us - touch->two_start_time_us) < TOUCH_TWO_TAP_US) {
				push_action(out, room, &count, X11_TOUCH_BUTTON, 3U, 1, touch->last_x, touch->last_y, 0, 0, 0, 0.0);
				push_action(out, room, &count, X11_TOUCH_BUTTON, 3U, 0, touch->last_x, touch->last_y, 0, 0, 0, 0.0);
				touch->two_done = 1;
			}

			if (!touch->active[0] && !touch->active[1])
				reset_preserving_mode(touch);

			return count;
		}

		/* Single-finger lift either clicks left (if never dragged) or releases the held left button. */
		touch->last_x = x;
		touch->last_y = y;
		if (!touch->pressed_left) {
			push_action(out, room, &count, X11_TOUCH_BUTTON, 1U, 1, x, y, 0, 0, 0, 0.0);
			push_action(out, room, &count, X11_TOUCH_BUTTON, 1U, 0, x, y, 0, 0, 0, 0.0);
		} else {
			push_action(out, room, &count, X11_TOUCH_BUTTON, 1U, 0, x, y, 0, 0, 0, 0.0);
		}

		reset_preserving_mode(touch);
		return count;
	}

	return 0U;
}

/* Returns the slot index holding id, or -1 if not present. */
static int
find_slot(
	const struct x11_touch *touch,
	int32_t id)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (touch->active[i] && touch->id[i] == id)
			return i;
	}

	return -1;
}

/* Returns the first inactive slot index, or -1 if both slots are occupied. */
static int
find_free_slot(
	const struct x11_touch *touch)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (!touch->active[i])
			return i;
	}

	return -1;
}

/* Resets touch gesture state while keeping the configured mode. */
static void
reset_preserving_mode(
	struct x11_touch *touch)
{
	unsigned mode;

	mode = touch->mode;
	memset(touch, 0, sizeof(*touch));
	touch->mode = mode;
}

/* Appends one action to out if room remains. */
static void
push_action(
	struct x11_touch_action *out,
	unsigned room,
	unsigned *count,
	unsigned kind,
	unsigned button,
	int pressed,
	int32_t x,
	int32_t y,
	int notches,
	int32_t dx,
	int32_t dy,
	double scale_ratio)
{
	struct x11_touch_action *action;

	if (*count >= room)
		return;

	action = &out[*count];
	action->kind = kind;
	action->button = button;
	action->pressed = pressed;
	action->x = x;
	action->y = y;
	action->notches = notches;
	action->dx = dx;
	action->dy = dy;
	action->scale_ratio = scale_ratio;
	*count += 1U;
}

/* Returns non-zero when (x1, y1) is strictly farther than limit pixels from (x0, y0). */
static int
moved_beyond(
	int32_t x0,
	int32_t y0,
	int32_t x1,
	int32_t y1,
	int limit)
{
	int64_t dx;
	int64_t dy;
	int64_t lim;

	dx = (int64_t)x1 - (int64_t)x0;
	dy = (int64_t)y1 - (int64_t)y0;
	lim = (int64_t)limit;
	return (dx * dx + dy * dy) > (lim * lim);
}

/* Returns the integer average of the two active contacts' X coordinates. */
static int32_t
average_x(
	const struct x11_touch *touch)
{
	return (int32_t)(((int64_t)touch->x[0] + (int64_t)touch->x[1]) / 2);
}

/* Returns the integer average of the two active contacts' Y coordinates. */
static int32_t
average_y(
	const struct x11_touch *touch)
{
	return (int32_t)(((int64_t)touch->y[0] + (int64_t)touch->y[1]) / 2);
}

/* Returns the Euclidean distance between the two active contacts. */
static double
finger_distance(
	const struct x11_touch *touch)
{
	double dx;
	double dy;
	double dist;

	/* Euclidean distance between the two contacts. */
	dx = (double)touch->x[1] - (double)touch->x[0];
	dy = (double)touch->y[1] - (double)touch->y[0];
	dist = sqrt(dx * dx + dy * dy);

	/* Succeeded. */
	return dist;
}
