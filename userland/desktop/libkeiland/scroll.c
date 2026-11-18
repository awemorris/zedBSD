/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scroller (WS081 p005, plan/ws081/design.md section 5): content a
 * finger drags, flings and pulls past its bounds.
 *
 * Each axis keeps its raw position: where the finger put it, past a bound
 * too.  What is drawn past a bound is the rubber band of the raw distance,
 * f(d) = L c d / (c |d| + L), which resists more the further it goes and
 * never reaches the viewport's size L.  A fling slows by
 * dv/dt = -v/tau - mu sign(v): v(t) = (v0 + mu tau) e^(-t/tau) - mu tau,
 * which stops at t = tau ln(1 + v0 / (mu tau)).  A diagonal fling gives
 * each axis its share of mu, so both axes stop together and the direction
 * holds.  An axis that passes a bound hands its velocity to a critically
 * damped spring on the raw distance, d(t) = (d0 + (v0 + w d0) t) e^(-w t),
 * and a spring that crosses the bound inwards hands its velocity back to a
 * fling.  The position is a closed form of the time within each phase.
 *
 * It is the one inertia of every program (ws090-p019, BUG-211): a touch
 * screen's finger drags and flings it (kl_scroller_press, _drag,
 * _release); a touch pad's two fingers drag it too, and their velocity,
 * worked out from their last moves at the compositor's times (the track
 * below, kl_axis_track), flings it when they lift (kl_scroller_axis,
 * _axis_stop).
 */

#include <keiland/keiland.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The fling's time constant (seconds) and constant friction (px/s^2). */
#define SCROLL_TAU			0.45
#define SCROLL_FRICTION			300.0

/* The fastest fling (px/s). */
#define SCROLL_SPEED_MAX		8000.0

/* The rubber band's stiffness, and the spring's rate (1/s). */
#define SCROLL_RUBBER			0.55
#define SCROLL_SPRING_RATE		16.0

/* A spring rests when it is this close (px) and this slow (px/s), or after this long (s). */
#define SCROLL_SPRING_CLOSE		0.25
#define SCROLL_SPRING_SLOW		5.0
#define SCROLL_SPRING_LONGEST		2.0

/* A drag decides its axis lock after this much movement (px), within this angle (degrees) of an axis. */
#define SCROLL_LOCK_DISTANCE		8.0
#define SCROLL_LOCK_ANGLE		22.5

/* A fling adds the speed it caught when it comes this soon (us) after the catch, within this angle (degrees). */
#define SCROLL_ACCELERATE_WITHIN	400000U
#define SCROLL_ACCELERATE_ANGLE		30.0

/* Moving content slower than this (px/s) is not caught (a press on it may tap). */
#define SCROLL_CATCH_SPEED		50.0

/* The degrees of a half turn. */
#define SCROLL_HALF_TURN		180.0

/* What one axis is doing. */
enum scroll_mode {
	SCROLL_REST,
	SCROLL_DRAG,
	SCROLL_FLING,
	SCROLL_SPRING
};

/*
 * One axis: its bounds and viewport, the raw position, what it is doing,
 * and the phase's start: its time, and for a fling the start position,
 * velocity, friction and stopping time, for a spring the bound and the
 * raw distance and velocity at the start.  spring_pending asks the next
 * step to start a spring at rest (the bounds shrank under it).
 * press_position is where a drag started; locked says the drag's lock
 * holds this axis still.
 */
struct scroll_axis {
	double minimum;
	double maximum;
	double viewport;
	double position;
	enum scroll_mode mode;
	uint64_t start_us;
	double start_position;
	double start_velocity;
	double friction;
	double stop_time;
	double bound;
	double overshoot;
	int spring_pending;
	double press_position;
	int locked;
};

/*
 * A scroller: its two axes, whether the drag has decided its lock, and the
 * velocity and time of the motion a press last caught (for a fling that
 * follows it).
 */
struct kl_scroller {
	struct scroll_axis axes[2];
	int lock_decided;
	int caught;
	double caught_vx;
	double caught_vy;
	uint64_t caught_us;

	/*
	 * A touch pad's fingers that hold the content (kl_scroller_axis): the
	 * moves since they took it (a wheel's way), and their track for the
	 * velocity when they lift.  A press, a release, a cancel or a new
	 * position lets go of them.
	 */
	int axis_holding;
	double axis_total_x;
	double axis_total_y;
	struct kl_axis_track axis_track;
};

static int axis_scrolls(const struct scroll_axis *axis);
static int axis_outside(const struct scroll_axis *axis, double position);
static double axis_nearest_bound(const struct scroll_axis *axis, double position);
static double axis_shown(const struct scroll_axis *axis);
static double rubber(double distance, double viewport);
static void axis_fling(struct scroll_axis *axis, uint64_t now_us, double velocity, double friction);
static void axis_spring(struct scroll_axis *axis, uint64_t now_us, double velocity);
static void axis_settle(struct scroll_axis *axis, uint64_t now_us, double velocity);
static int axis_advance(struct scroll_axis *axis, uint64_t now_us, double *velocity);
static void fling_at(const struct scroll_axis *axis, double t, double *position, double *velocity);
static void spring_at(const struct scroll_axis *axis, double t, double *distance, double *velocity);
static double seconds_since(uint64_t now_us, uint64_t start_us);
static double angle_between(double ax, double ay, double bx, double by);
static unsigned axis_track_slot(const struct kl_axis_track *track, unsigned age);

/*
 * Creates a scroller at 0 that scrolls nowhere yet.
 */
struct kl_scroller *
kl_scroller_create(void)
{
	struct kl_scroller *scroller;
	int index;

	/* Everything at zero: at rest at 0, bounds 0..0. */
	scroller = calloc(1, sizeof(*scroller));
	if (scroller == NULL)
		return NULL;

	/* A viewport of one pixel until the bounds are set, so the rubber band never divides by zero. */
	for (index = 0; index < 2; index++)
		scroller->axes[index].viewport = 1.0;

	/* Succeeded: the new scroller. */
	return scroller;
}

/*
 * Destroys a scroller.
 */
void
kl_scroller_destroy(
	struct kl_scroller *scroller)
{
	/* The scroller owns nothing else. */
	free(scroller);
}

/*
 * Sets the bounds and the viewport of both axes.
 */
int
kl_scroller_set_bounds(
	struct kl_scroller *scroller,
	double minimum_x,
	double maximum_x,
	double minimum_y,
	double maximum_y,
	double viewport_width,
	double viewport_height)
{
	struct scroll_axis *axis;
	int scrolls;
	int outside;
	int index;

	/* Refuses a missing scroller, bounds the wrong way round and an empty viewport. */
	if (scroller == NULL)
		return EINVAL;
	if (maximum_x < minimum_x)
		return EINVAL;
	if (maximum_y < minimum_y)
		return EINVAL;
	if (viewport_width <= 0.0)
		return EINVAL;
	if (viewport_height <= 0.0)
		return EINVAL;

	/* The new bounds and viewports. */
	scroller->axes[0].minimum = minimum_x;
	scroller->axes[0].maximum = maximum_x;
	scroller->axes[0].viewport = viewport_width;
	scroller->axes[1].minimum = minimum_y;
	scroller->axes[1].maximum = maximum_y;
	scroller->axes[1].viewport = viewport_height;

	/* An axis that no longer scrolls sits at its bound; one left outside at rest springs back. */
	for (index = 0; index < 2; index++) {
		axis = &scroller->axes[index];
		scrolls = axis_scrolls(axis);
		outside = axis_outside(axis, axis->position);
		if (!scrolls) {
			axis->position = axis->minimum;
			axis->mode = SCROLL_REST;
		} else if (axis->mode == SCROLL_REST && outside) {
			axis->spring_pending = 1;
		}
	}

	/* Succeeded: the bounds hold from now on. */
	return 0;
}

/*
 * Moves the position at once, within the bounds, stopping any motion.
 */
void
kl_scroller_set_position(
	struct kl_scroller *scroller,
	double x,
	double y)
{
	struct scroll_axis *axis;
	double wanted[2];
	int index;

	/* Nothing to move. */
	if (scroller == NULL)
		return;

	/* Fingers on a touch pad let go of it. */
	scroller->axis_holding = 0;

	/* Each axis at its place, clamped, at rest. */
	wanted[0] = x;
	wanted[1] = y;
	for (index = 0; index < 2; index++) {
		axis = &scroller->axes[index];
		axis->position = wanted[index];
		if (axis->position < axis->minimum)
			axis->position = axis->minimum;
		if (axis->position > axis->maximum)
			axis->position = axis->maximum;
		axis->mode = SCROLL_REST;
		axis->spring_pending = 0;
	}
}

/*
 * A finger touches: catches moving content and starts a drag.
 */
int
kl_scroller_press(
	struct kl_scroller *scroller,
	uint64_t now_us)
{
	struct scroll_axis *axis;
	double velocity[2];
	double speed;
	int index;

	/* Nothing to press. */
	if (scroller == NULL)
		return 0;

	/* Each axis where it is now, with how fast it moved. */
	for (index = 0; index < 2; index++) {
		axis = &scroller->axes[index];
		velocity[index] = 0.0;
		(void)axis_advance(axis, now_us, &velocity[index]);
	}

	/* Content moving fast enough is caught: remembered for a fling that follows. */
	speed = sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
	scroller->caught = 0;
	if (speed >= SCROLL_CATCH_SPEED) {
		scroller->caught = 1;
		scroller->caught_vx = velocity[0];
		scroller->caught_vy = velocity[1];
		scroller->caught_us = now_us;
	}

	/* Every axis follows the finger from here. */
	for (index = 0; index < 2; index++) {
		axis = &scroller->axes[index];
		axis->mode = SCROLL_DRAG;
		axis->spring_pending = 0;
		axis->press_position = axis->position;
		axis->locked = 0;
	}

	/* The drag decides its lock afresh; a touch pad's fingers holding it let go. */
	scroller->lock_decided = 0;
	scroller->axis_holding = 0;

	/* Succeeded: whether moving content was caught. */
	return scroller->caught;
}

/*
 * The finger has moved by dx, dy since the press.
 */
void
kl_scroller_drag(
	struct kl_scroller *scroller,
	double dx,
	double dy)
{
	struct scroll_axis *axis;
	double moved[2];
	double distance;
	double angle;
	int scrolls_x;
	int scrolls_y;
	int scrolls;
	int index;

	/* Nothing to drag. */
	if (scroller == NULL)
		return;

	/* The first 8 px decide the lock, on a scroller that scrolls both ways. */
	distance = sqrt(dx * dx + dy * dy);
	scrolls_x = axis_scrolls(&scroller->axes[0]);
	scrolls_y = axis_scrolls(&scroller->axes[1]);
	if (!scroller->lock_decided && distance >= SCROLL_LOCK_DISTANCE) {
		scroller->lock_decided = 1;
		angle = atan2(fabs(dy), fabs(dx)) * SCROLL_HALF_TURN / M_PI;
		if (scrolls_x && scrolls_y) {
			if (angle <= SCROLL_LOCK_ANGLE) {
				scroller->axes[1].locked = 1;
			} else if (angle >= 90.0 - SCROLL_LOCK_ANGLE) {
				scroller->axes[0].locked = 1;
			}
		}
	}

	/* Each dragged axis moves against the finger (the content follows it), past the bounds too. */
	moved[0] = dx;
	moved[1] = dy;
	for (index = 0; index < 2; index++) {
		axis = &scroller->axes[index];
		if (axis->mode != SCROLL_DRAG)
			continue;
		if (axis->locked)
			continue;
		scrolls = axis_scrolls(axis);
		if (!scrolls)
			continue;
		axis->position = axis->press_position - moved[index];
	}
}

/*
 * The finger lifts with a velocity: a fling when fast enough, otherwise the
 * content settles (springs back from past a bound).
 */
int
kl_scroller_release(
	struct kl_scroller *scroller,
	uint64_t now_us,
	double vx,
	double vy)
{
	struct scroll_axis *axis;
	double velocity[2];
	double caught_speed;
	double speed;
	double scale;
	double angle;
	int scrolls;
	int outside;
	int index;

	/* Nothing to release. */
	if (scroller == NULL)
		return 0;

	/* A touch pad's fingers holding it let go. */
	scroller->axis_holding = 0;

	/* The content's velocity is against the finger's; a locked or fixed axis has none. */
	velocity[0] = -vx;
	velocity[1] = -vy;
	for (index = 0; index < 2; index++) {
		axis = &scroller->axes[index];
		scrolls = axis_scrolls(axis);
		if (axis->locked || !scrolls)
			velocity[index] = 0.0;
	}

	/* Its speed. */
	speed = sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);

	/* Too slow to fling: every axis settles. */
	if (speed < KL_SCROLLER_FLING_MIN) {
		for (index = 0; index < 2; index++)
			axis_settle(&scroller->axes[index], now_us, velocity[index]);
		scroller->caught = 0;
		return 0;
	}

	/* A fling soon after a catch, the same way, adds the caught speed. */
	if (scroller->caught && now_us - scroller->caught_us <= SCROLL_ACCELERATE_WITHIN) {
		angle = angle_between(velocity[0], velocity[1], scroller->caught_vx, scroller->caught_vy);
		if (angle <= SCROLL_ACCELERATE_ANGLE) {
			caught_speed = sqrt(scroller->caught_vx * scroller->caught_vx + scroller->caught_vy * scroller->caught_vy);
			scale = (speed + caught_speed) / speed;
			velocity[0] *= scale;
			velocity[1] *= scale;
			speed += caught_speed;
		}
	}

	/* The catch is used up. */
	scroller->caught = 0;

	/* No faster than the fastest fling. */
	if (speed > SCROLL_SPEED_MAX) {
		scale = SCROLL_SPEED_MAX / speed;
		velocity[0] *= scale;
		velocity[1] *= scale;
		speed = SCROLL_SPEED_MAX;
	}

	/*
	 * Each axis flings with its share of the friction (both stop together
	 * and the direction holds), or springs back from past a bound.
	 */
	for (index = 0; index < 2; index++) {
		axis = &scroller->axes[index];
		outside = axis_outside(axis, axis->position);
		if (outside) {
			axis_spring(axis, now_us, velocity[index]);
		} else if (velocity[index] != 0.0) {
			axis_fling(axis, now_us, velocity[index], SCROLL_FRICTION * fabs(velocity[index]) / speed);
		} else {
			axis->mode = SCROLL_REST;
		}
	}

	/* Succeeded: a fling. */
	return 1;
}

/*
 * A touch pad's two fingers move the content by dx, dy (pixels, as a wheel
 * scrolls: down and right positive) at the compositor's time event_us:
 * their first move presses the scroller at now_us (the time its steps
 * use), catching moving content, and each move drags it on and is kept for
 * their velocity.  Returns 1 when the first move caught moving content.
 */
int
kl_scroller_axis(
	struct kl_scroller *scroller,
	double dx,
	double dy,
	uint64_t event_us,
	uint64_t now_us)
{
	int caught;

	/* Nothing to move. */
	if (scroller == NULL)
		return 0;

	/* The fingers' first move takes the content (stopping a flight) and starts their track. */
	caught = 0;
	if (!scroller->axis_holding) {
		caught = kl_scroller_press(scroller, now_us);
		scroller->axis_holding = 1;
		scroller->axis_total_x = 0.0;
		scroller->axis_total_y = 0.0;
		kl_axis_track_reset(&scroller->axis_track);
	}

	/* The move, kept for the velocity, and the content dragged the wheel's way (a finger's drag goes the other way). */
	scroller->axis_total_x += dx;
	scroller->axis_total_y += dy;
	kl_axis_track_add(&scroller->axis_track, dx, dy, event_us);
	kl_scroller_drag(scroller, -scroller->axis_total_x, -scroller->axis_total_y);

	/* Succeeded: whether moving content was caught. */
	return caught;
}

/*
 * A touch pad's fingers lift (the compositor's axis stop at event_us): the
 * content flies on from now_us at their velocity, as a finger's release
 * throws it (none when they rested before lifting), or settles.  Gives the
 * velocity (a wheel's way, px/s; may be NULL) and returns 1 for a fling, 0
 * when it settles or no fingers held it.
 */
int
kl_scroller_axis_stop(
	struct kl_scroller *scroller,
	uint64_t event_us,
	uint64_t now_us,
	double *vx,
	double *vy)
{
	double velocity_x;
	double velocity_y;
	int flung;

	/* None thrown yet. */
	if (vx != NULL)
		*vx = 0.0;
	if (vy != NULL)
		*vy = 0.0;

	/* Only content the fingers hold. */
	if (scroller == NULL || !scroller->axis_holding)
		return 0;

	/* Their velocity the wheel's way, thrown the finger's way. */
	kl_axis_track_velocity(&scroller->axis_track, event_us, &velocity_x, &velocity_y);
	if (vx != NULL)
		*vx = velocity_x;
	if (vy != NULL)
		*vy = velocity_y;
	flung = kl_scroller_release(scroller, now_us, -velocity_x, -velocity_y);
	if (flung == 0)
		return 0;

	/* Succeeded: the content flies. */
	return 1;
}

/*
 * Tells whether a touch pad's fingers hold the content (kl_scroller_axis
 * since their last stop).
 */
int
kl_scroller_axis_holding(
	const struct kl_scroller *scroller)
{
	/* No scroller, no fingers. */
	if (scroller == NULL)
		return 0;

	/* Succeeded: whether they hold it. */
	return scroller->axis_holding;
}

/*
 * Empties a track of a touch pad's scrolling.
 */
void
kl_axis_track_reset(
	struct kl_axis_track *track)
{
	/* No moves. */
	memset(track, 0, sizeof(*track));
}

/*
 * Adds one move of the fingers (pixels, as a wheel scrolls) at a time to
 * the track; the oldest goes when it is full.
 */
void
kl_axis_track_add(
	struct kl_axis_track *track,
	double dx,
	double dy,
	uint64_t now_us)
{
	/* The move in the next slot. */
	track->dx[track->next] = dx;
	track->dy[track->next] = dy;
	track->us[track->next] = now_us;
	track->next = (track->next + 1U) % KL_AXIS_TRACK_SAMPLES;

	/* One more, up to the slots. */
	if (track->count < KL_AXIS_TRACK_SAMPLES)
		track->count++;
}

/*
 * Works out the fingers' velocity (pixels a second, as a wheel scrolls) at
 * a time: the moves of the last KL_AXIS_TRACK_WINDOW_US after the oldest
 * of them, over the time since it.  Fingers that rested
 * KL_AXIS_TRACK_REST_US or longer, or a single move, give none.
 */
void
kl_axis_track_velocity(
	const struct kl_axis_track *track,
	uint64_t now_us,
	double *vx,
	double *vy)
{
	unsigned age;
	unsigned slot;
	unsigned newer;
	unsigned used;
	uint64_t newest_us;
	uint64_t oldest_us;
	double sum_x;
	double sum_y;
	double seconds;

	/* None until there is something to go by. */
	*vx = 0.0;
	*vy = 0.0;
	if (track->count < 2U)
		return;

	/* Fingers that rested before lifting throw nothing. */
	slot = axis_track_slot(track, 0U);
	newest_us = track->us[slot];
	if (now_us > newest_us && now_us - newest_us >= KL_AXIS_TRACK_REST_US)
		return;

	/* The moves within the window, newest first; the oldest of them starts the time and is not counted. */
	sum_x = 0.0;
	sum_y = 0.0;
	used = 0U;
	oldest_us = newest_us;
	for (age = 0U; age < track->count; age++) {
		slot = axis_track_slot(track, age);
		if (newest_us - track->us[slot] > KL_AXIS_TRACK_WINDOW_US)
			break;
		if (age != 0U) {
			newer = axis_track_slot(track, age - 1U);
			sum_x += track->dx[newer];
			sum_y += track->dy[newer];
			used++;
		}
		oldest_us = track->us[slot];
	}

	/* The distance after the oldest move counted, over the time since it. */
	if (used == 0U || newest_us <= oldest_us)
		return;
	seconds = (double)(newest_us - oldest_us) / 1000000.0;
	*vx = sum_x / seconds;
	*vy = sum_y / seconds;
}

/*
 * The touch was taken away: no fling; content past a bound springs back.
 */
void
kl_scroller_cancel(
	struct kl_scroller *scroller,
	uint64_t now_us)
{
	int index;

	/* Nothing to cancel. */
	if (scroller == NULL)
		return;

	/* A touch pad's fingers holding it let go, and every axis settles where the finger left it. */
	scroller->axis_holding = 0;
	for (index = 0; index < 2; index++)
		axis_settle(&scroller->axes[index], now_us, 0.0);
	scroller->caught = 0;
}

/*
 * Gives the position to draw at a time; reports whether the content moves
 * by itself.
 */
int
kl_scroller_step(
	struct kl_scroller *scroller,
	uint64_t now_us,
	double *x,
	double *y)
{
	double velocity;
	int moving;
	int animating;
	int index;

	/* Nothing to step. */
	if (scroller == NULL)
		return 0;

	/* Each axis on to the time. */
	animating = 0;
	for (index = 0; index < 2; index++) {
		moving = axis_advance(&scroller->axes[index], now_us, &velocity);
		if (moving)
			animating = 1;
	}

	/* The positions as they are shown (the rubber band past a bound). */
	if (x != NULL)
		*x = axis_shown(&scroller->axes[0]);
	if (y != NULL)
		*y = axis_shown(&scroller->axes[1]);

	/* Succeeded: whether to keep drawing frames. */
	return animating;
}

/* Tells whether an axis scrolls at all. */
static int
axis_scrolls(
	const struct scroll_axis *axis)
{
	/* An axis whose bounds meet does not. */
	if (axis->maximum > axis->minimum)
		return 1;
	return 0;
}

/* Tells whether a raw position lies past a bound of an axis that scrolls. */
static int
axis_outside(
	const struct scroll_axis *axis,
	double position)
{
	int scrolls;

	/* A fixed axis has nothing to be past. */
	scrolls = axis_scrolls(axis);
	if (!scrolls)
		return 0;

	/* Below the minimum or above the maximum. */
	if (position < axis->minimum)
		return 1;
	if (position > axis->maximum)
		return 1;
	return 0;
}

/* Gives the bound nearest a raw position past the bounds (the one it passed). */
static double
axis_nearest_bound(
	const struct scroll_axis *axis,
	double position)
{
	/* Above the maximum is nearest the maximum; anything else, the minimum. */
	if (position > axis->maximum)
		return axis->maximum;
	return axis->minimum;
}

/* Gives where an axis is drawn: its position, or past a bound the rubber band of the distance. */
static double
axis_shown(
	const struct scroll_axis *axis)
{
	double bound;
	int outside;

	/* Within the bounds the position is drawn as it is. */
	outside = axis_outside(axis, axis->position);
	if (!outside)
		return axis->position;

	/* Past a bound, the bound and the rubber band of how far past. */
	bound = axis_nearest_bound(axis, axis->position);
	return bound + rubber(axis->position - bound, axis->viewport);
}

/* Gives the rubber band of a raw distance past a bound: L c d / (c |d| + L). */
static double
rubber(
	double distance,
	double viewport)
{
	double shown;

	/* Grows with the distance, ever more slowly, never to the viewport's size. */
	shown = viewport * SCROLL_RUBBER * fabs(distance) / (SCROLL_RUBBER * fabs(distance) + viewport);

	/* The direction of the distance. */
	if (distance < 0.0)
		return -shown;
	return shown;
}

/* Starts a fling on an axis from its position, with a velocity and a friction. */
static void
axis_fling(
	struct scroll_axis *axis,
	uint64_t now_us,
	double velocity,
	double friction)
{
	/* A fling with no speed or no friction does not move. */
	if (velocity == 0.0 || friction <= 0.0) {
		axis->mode = SCROLL_REST;
		return;
	}

	/* Its start, and when it stops: tau ln(1 + |v0| / (mu tau)). */
	axis->mode = SCROLL_FLING;
	axis->start_us = now_us;
	axis->start_position = axis->position;
	axis->start_velocity = velocity;
	axis->friction = friction;
	axis->stop_time = SCROLL_TAU * log(1.0 + fabs(velocity) / (friction * SCROLL_TAU));
	axis->spring_pending = 0;
}

/* Starts a spring back to the bound an axis is past, with a velocity. */
static void
axis_spring(
	struct scroll_axis *axis,
	uint64_t now_us,
	double velocity)
{
	/* The bound it is past and how far. */
	axis->mode = SCROLL_SPRING;
	axis->start_us = now_us;
	axis->bound = axis_nearest_bound(axis, axis->position);
	axis->overshoot = axis->position - axis->bound;
	axis->start_velocity = velocity;
	axis->spring_pending = 0;
}

/* Lets an axis settle: past a bound it springs back with the velocity, otherwise it rests. */
static void
axis_settle(
	struct scroll_axis *axis,
	uint64_t now_us,
	double velocity)
{
	int outside;

	/* Past a bound it springs back. */
	outside = axis_outside(axis, axis->position);
	if (outside) {
		axis_spring(axis, now_us, velocity);
		return;
	}

	/* Otherwise it stays. */
	axis->mode = SCROLL_REST;
	axis->spring_pending = 0;
}

/*
 * Moves an axis on to a time: a fling that passes a bound hands on to a
 * spring, a spring that crosses the bound inwards to a fling, and each
 * comes to rest.  Gives the axis's velocity then and reports whether it
 * still moves by itself.
 */
static int
axis_advance(
	struct scroll_axis *axis,
	uint64_t now_us,
	double *velocity)
{
	double position;
	double distance;
	double t;
	int outside;
	int resting;

	/* Waiting to spring: the spring starts now. */
	*velocity = 0.0;
	if (axis->spring_pending) {
		axis_spring(axis, now_us, 0.0);
		return 1;
	}

	/* At rest or dragged: no velocity of its own. */
	if (axis->mode == SCROLL_REST || axis->mode == SCROLL_DRAG)
		return 0;
	t = seconds_since(now_us, axis->start_us);

	/* A fling: where the closed form puts it. */
	if (axis->mode == SCROLL_FLING) {
		fling_at(axis, t, &position, velocity);
		axis->position = position;

		/* Past a bound the spring takes over, with the fling's velocity. */
		outside = axis_outside(axis, position);
		if (outside) {
			axis_spring(axis, now_us, *velocity);
			return 1;
		}

		/* A fling that has stopped rests. */
		if (t >= axis->stop_time) {
			axis->mode = SCROLL_REST;
			*velocity = 0.0;
			return 0;
		}

		/* The fling goes on. */
		return 1;
	}

	/* A spring: the distance past the bound. */
	spring_at(axis, t, &distance, velocity);

	/* Crossed inwards: the bound, and a fling with the spring's velocity. */
	if (distance * axis->overshoot < 0.0) {
		axis->position = axis->bound;
		axis_fling(axis, now_us, *velocity, SCROLL_FRICTION);
		return 1;
	}

	/* Close and slow enough, or long enough: at the bound, at rest. */
	resting = 0;
	if (distance < SCROLL_SPRING_CLOSE && distance > -SCROLL_SPRING_CLOSE &&
	    *velocity < SCROLL_SPRING_SLOW && *velocity > -SCROLL_SPRING_SLOW)
		resting = 1;
	if (t >= SCROLL_SPRING_LONGEST)
		resting = 1;
	if (resting) {
		axis->position = axis->bound;
		axis->mode = SCROLL_REST;
		*velocity = 0.0;
		return 0;
	}

	/* Succeeded: the spring goes on. */
	axis->position = axis->bound + distance;
	return 1;
}

/* Gives a fling's position and velocity t seconds after it started (it stays where it stopped). */
static void
fling_at(
	const struct scroll_axis *axis,
	double t,
	double *position,
	double *velocity)
{
	double speed;
	double friction_tau;
	double decay;
	double sign;

	/* The fling is one-dimensional in its direction; after its stop it stays. */
	if (t > axis->stop_time)
		t = axis->stop_time;
	if (t < 0.0)
		t = 0.0;
	sign = 1.0;
	if (axis->start_velocity < 0.0)
		sign = -1.0;
	speed = fabs(axis->start_velocity);
	friction_tau = axis->friction * SCROLL_TAU;
	decay = exp(-t / SCROLL_TAU);

	/* v(t) = (v0 + mu tau) e^(-t/tau) - mu tau; x(t) = x0 + (v0 + mu tau) tau (1 - e^(-t/tau)) - mu tau t. */
	*velocity = sign * ((speed + friction_tau) * decay - friction_tau);
	*position = axis->start_position + sign * ((speed + friction_tau) * SCROLL_TAU * (1.0 - decay) - friction_tau * t);
}

/* Gives a spring's distance past its bound and its velocity t seconds after it started. */
static void
spring_at(
	const struct scroll_axis *axis,
	double t,
	double *distance,
	double *velocity)
{
	double rate;
	double decay;
	double lead;

	/* d(t) = (d0 + (v0 + w d0) t) e^(-w t); v(t) = (v0 - w (v0 + w d0) t) e^(-w t). */
	rate = SCROLL_SPRING_RATE;
	decay = exp(-rate * t);
	lead = axis->start_velocity + rate * axis->overshoot;
	*distance = (axis->overshoot + lead * t) * decay;
	*velocity = (axis->start_velocity - rate * lead * t) * decay;
}

/* Gives the seconds from a start to now (zero when now is earlier). */
static double
seconds_since(
	uint64_t now_us,
	uint64_t start_us)
{
	/* A time before the start counts as the start. */
	if (now_us <= start_us)
		return 0.0;

	/* Succeeded: the seconds between. */
	return (double)(now_us - start_us) / 1.0e6;
}

/* Gives the angle between two directions in degrees (180 when either is zero). */
static double
angle_between(
	double ax,
	double ay,
	double bx,
	double by)
{
	double lengths;
	double cosine;
	double angle;

	/* A direction of no length is as far from any as can be. */
	lengths = sqrt(ax * ax + ay * ay) * sqrt(bx * bx + by * by);
	if (lengths <= 0.0)
		return SCROLL_HALF_TURN;

	/* The cosine, kept within its range against rounding. */
	cosine = (ax * bx + ay * by) / lengths;
	if (cosine > 1.0)
		cosine = 1.0;
	if (cosine < -1.0)
		cosine = -1.0;

	/* Succeeded: the angle. */
	angle = acos(cosine) * SCROLL_HALF_TURN / M_PI;
	return angle;
}

/* Finds the slot of a track's move of an age (0 the newest). */
static unsigned
axis_track_slot(
	const struct kl_axis_track *track,
	unsigned age)
{
	unsigned slot;

	/* Back from the next free slot. */
	slot = (track->next + KL_AXIS_TRACK_SAMPLES - 1U - age) % KL_AXIS_TRACK_SAMPLES;

	/* Reports the slot. */
	return slot;
}
