/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scroller: what content a finger scrolls does, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_SCROLLER_H
#define KEILAND_SCROLLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The scroller: what content that a finger scrolls does after the finger lets
 * go, the same in every program.
 *
 * A scroller holds a position on up to two axes within bounds.  A drag
 * moves it with the finger; past a bound the content resists (rubber
 * band).  Let go fast enough, it glides on and slows down by
 * dv/dt = -v/tau - mu sign(v) (tau 0.45 s, mu 300 px/s^2), which stops at a
 * finite time; past a bound it springs back (critically damped, 16/s).  A
 * press while it glides catches it (stops it, and the press should not
 * activate what is under it); a fling within 400 ms of a catch, the same
 * way within 30 degrees, adds the caught speed.  The position is a pure
 * function of the time while it glides, so frames may come unevenly.
 *
 * Positions are logical pixels of content offset (a larger position shows
 * content further right or down); a finger moving down by d moves the
 * position up by d.  Times are CLOCK_MONOTONIC microseconds.
 */
struct kl_scroller;

/* The slowest fling (px/s): a release slower than this only settles. */
#define KL_SCROLLER_FLING_MIN	300.0

/*
 * Creates a scroller at position 0 with bounds 0..0 on both axes (it
 * scrolls nowhere until kl_scroller_set_bounds).
 *
 * Returns NULL when memory is short.
 */
struct kl_scroller *kl_scroller_create(void);

/* Destroys a scroller. */
void kl_scroller_destroy(struct kl_scroller *scroller);

/*
 * Sets the bounds of the position on each axis and the viewport's size (the
 * rubber band's scale).  An axis whose maximum equals its minimum does not
 * scroll.  A position left outside the new bounds springs back.  Refuses a
 * maximum below its minimum or a viewport not above zero with EINVAL.
 */
int kl_scroller_set_bounds(struct kl_scroller *scroller, double minimum_x, double maximum_x, double minimum_y,
    double maximum_y, double viewport_width, double viewport_height);

/* Moves the position at once (clamped to the bounds), stopping any motion. */
void kl_scroller_set_position(struct kl_scroller *scroller, double x, double y);

/*
 * A finger touches: starts a drag from the current position.  Returns 1
 * when it caught moving content (the touch should not tap), otherwise 0.
 */
int kl_scroller_press(struct kl_scroller *scroller, uint64_t now_us);

/*
 * The finger has moved by dx, dy since the press (the total, not a step;
 * kl_gesture_drag_offset gives it resampled for the frame).  The first
 * time it has moved 8 px, a drag within 22.5 degrees of one axis locks the
 * other (on a scroller that scrolls both ways).
 */
void kl_scroller_drag(struct kl_scroller *scroller, double dx, double dy);

/*
 * The finger lifts with a velocity (px/s, as the finger moved;
 * kl_motion_velocity or the gesture's DRAG_END gives it): a fling
 * when it is fast enough, otherwise the content settles.  Returns 1 for a
 * fling (KL_VERSION 41), 0 when it settles.
 */
int kl_scroller_release(struct kl_scroller *scroller, uint64_t now_us, double vx, double vy);

/*
 * A touch pad's two fingers (KL_AXIS_SOURCE_FINGER, KL_VERSION 41) move the
 * content by dx, dy: pixels as a wheel scrolls (down and right positive), at
 * the compositor's time event_us; now_us is the time the steps use.  The
 * first move since the fingers last lifted presses the scroller (returns 1
 * when it caught moving content); each drags it on and is kept for their
 * velocity.
 */
int kl_scroller_axis(struct kl_scroller *scroller, double dx, double dy, uint64_t event_us, uint64_t now_us);

/*
 * The touch pad's fingers lift (the axis stop at event_us): the content
 * flies on from now_us at their velocity (none when they rested before
 * lifting) with the same deceleration as a finger's fling, or settles.
 * Gives the velocity (px/s, a wheel's way; either may be NULL) and returns
 * 1 for a fling.
 */
int kl_scroller_axis_stop(struct kl_scroller *scroller, uint64_t event_us, uint64_t now_us, double *vx, double *vy);

/* Tells whether a touch pad's fingers hold the content (since kl_scroller_axis, until they lift). */
int kl_scroller_axis_holding(const struct kl_scroller *scroller);

/* The touch was taken away (wl_touch.cancel): no fling; content past a bound springs back. */
void kl_scroller_cancel(struct kl_scroller *scroller, uint64_t now_us);

/*
 * Gives the position to draw at a time (past a bound, as the rubber band
 * shows it).  Returns 1 while the content moves by itself (keep drawing
 * frames), 0 when it rests or follows a finger.
 */
int kl_scroller_step(struct kl_scroller *scroller, uint64_t now_us, double *x, double *y);

#ifdef __cplusplus
}
#endif

#endif
