/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The gestures of a touch surface, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_GESTURE_H
#define KEILAND_GESTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The gestures of a touch surface: what the fingers of one wl_touch surface
 * mean, so that a tap, a long press and a drag are told apart the same way in
 * every program.
 *
 * The program passes the surface's wl_touch events on (in surface-local
 * pixels, with the event's time and the time it read the event, both in
 * microseconds) and reads the gestures with kl_gesture_next, which
 * also keeps the time (a long press is found by the clock, so the program
 * calls it at least every frame while a finger is down).
 *
 * - TAP: a finger lifted within 8 px of where it touched, before 500 ms.
 *   DOUBLE_TAP follows a TAP within 300 ms and 16 px of the last TAP.
 * - LONG_PRESS: a finger held within 8 px for 500 ms (a context menu, or
 *   the start of a selection); the lift then taps nothing.
 * - DRAG_BEGIN: the fingers moved 8 px (after_long_press says whether a
 *   long press came first); while dragging, kl_gesture_drag_offset
 *   gives the fingers' centroid's movement since, resampled for a frame.
 *   DRAG_END: the last finger lifted, with its velocity (for a fling).
 * - Two fingers: the drag follows their centroid (a finger added or
 *   lifted does not make it jump), and kl_gesture_pinch gives the
 *   change of their distance since the second touched.
 * - CANCEL: kl_gesture_cancel was called (wl_touch.cancel): whatever
 *   was going on ends without its lift.
 */
struct kl_gesture;

#define KL_GESTURE_TAP		1U
#define KL_GESTURE_DOUBLE_TAP	2U
#define KL_GESTURE_LONG_PRESS	3U
#define KL_GESTURE_DRAG_BEGIN	4U
#define KL_GESTURE_DRAG_END	5U
#define KL_GESTURE_CANCEL		6U

/*
 * One gesture: its kind (KL_GESTURE_*), where (the touch's point for a
 * tap or a long press, the centroid where a drag began), the velocity of a
 * DRAG_END (px/s), the fingers down, and for a DRAG_BEGIN whether a long
 * press came first.
 */
struct kl_gesture_event {
	unsigned kind;
	double x;
	double y;
	double vx;
	double vy;
	unsigned fingers;
	int after_long_press;
};

/*
 * Creates the gestures of one surface, with a touch motion device of its
 * own (the seat's touch screen as the program sees it).
 *
 * Returns NULL when memory is short.
 */
struct kl_gesture *kl_gesture_create(void);

/* Destroys the gestures of a surface. */
void kl_gesture_destroy(struct kl_gesture *gesture);

/*
 * A finger touches (wl_touch.down): its id, the event's time and the time
 * the program read it, and its place.  Refuses a sixth finger, and an id
 * already down, with EBUSY and EEXIST.
 */
int kl_gesture_down(struct kl_gesture *gesture, int32_t id, uint64_t time_us, uint64_t arrival_us, double x,
    double y);

/* A finger moves (wl_touch.motion).  Refuses an id that is not down with ENOENT. */
int kl_gesture_motion(struct kl_gesture *gesture, int32_t id, uint64_t time_us, uint64_t arrival_us, double x,
    double y);

/* A finger lifts (wl_touch.up).  Refuses an id that is not down with ENOENT. */
int kl_gesture_up(struct kl_gesture *gesture, int32_t id, uint64_t time_us);

/* The compositor took the fingers (wl_touch.cancel). */
void kl_gesture_cancel(struct kl_gesture *gesture);

/*
 * Takes the next gesture, after judging the time now (a long press).
 * Returns 1 with a gesture in *event, 0 when there is none.
 */
int kl_gesture_next(struct kl_gesture *gesture, uint64_t now_us, struct kl_gesture_event *event);

/*
 * Gives how far the dragging fingers' centroid has moved since the drag
 * began, resampled for a frame drawn at now_us.  Returns ENOENT when no
 * drag is going on.
 */
int kl_gesture_drag_offset(struct kl_gesture *gesture, uint64_t now_us, double *dx, double *dy);

/*
 * Gives the ratio of two fingers' distance to their distance when the
 * second touched, and their centroid, for a frame drawn at now_us.
 * Returns ENOENT unless two fingers are down.
 */
int kl_gesture_pinch(struct kl_gesture *gesture, uint64_t now_us, double *scale, double *x, double *y);

#ifdef __cplusplus
}
#endif

#endif
