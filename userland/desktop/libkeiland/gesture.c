/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The gestures of a touch surface (WS081 p005, plan/ws081/design.md
 * section 5.6): tap, double tap, long press, drag with one or more fingers,
 * pinch and cancel, told apart the same way in every program.
 *
 * A touch starts a press.  A press that moves 8 px (the fingers' centroid)
 * becomes a drag; one held for 500 ms without moving is a long press; one
 * that lifts first is a tap.  A second finger turns a press into one that
 * can only drag.  The drag follows the centroid of the fingers down: when
 * a finger joins or lifts, the drag's offset keeps its value and follows
 * the new centroid from there, so the content does not jump.  Every finger
 * feeds a touch motion (motion.c), which resamples the centroid for the
 * frame being drawn and gives the last finger's velocity at the lift.
 */

#include <keiland/keiland.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The fingers a surface follows at once. */
#define GESTURE_FINGERS		5U

/* The gestures waiting to be taken. */
#define GESTURE_QUEUE		16U

/* A press moves this far (px) before it drags. */
#define GESTURE_SLOP		8.0

/* A press held this long (us) without moving is a long press. */
#define GESTURE_LONG_PRESS	500000U

/* A second tap this soon (us) and this near (px) to the last one is a double tap. */
#define GESTURE_DOUBLE_WITHIN	300000U
#define GESTURE_DOUBLE_NEAR	16.0

/* What the fingers of a surface are doing. */
enum gesture_state {
	GESTURE_IDLE,
	GESTURE_PRESSED,
	GESTURE_MULTI_PRESSED,
	GESTURE_LONG,
	GESTURE_DRAG
};

/*
 * One finger down: its wl_touch id, its last reported place, and the
 * motion its reports go into (made the first time the place is used, kept
 * for later fingers).
 */
struct gesture_finger {
	int used;
	int32_t id;
	double x;
	double y;
	struct kl_motion *motion;
};

/*
 * The gestures of one surface: the motion device its fingers teach, the
 * fingers, what they are doing, the press (its centroid and time), the
 * drag (the offset its earlier finger sets gave, and the centroid the
 * present set is measured from), the pinch's starting distance, the last
 * tap (for a double tap), whether a long press came before the drag, and
 * the gestures waiting to be taken (a ring).
 */
struct kl_gesture {
	struct kl_motion_device *device;
	struct gesture_finger fingers[GESTURE_FINGERS];
	unsigned count;
	enum gesture_state state;
	double press_x;
	double press_y;
	uint64_t press_us;
	double base_x;
	double base_y;
	double anchor_x;
	double anchor_y;
	double pinch_distance;
	int after_long_press;
	int last_tap;
	uint64_t last_tap_us;
	double last_tap_x;
	double last_tap_y;
	struct kl_gesture_event queue[GESTURE_QUEUE];
	unsigned queue_head;
	unsigned queue_count;
};

static struct gesture_finger *finger_of(struct kl_gesture *gesture, int32_t id);
static void raw_centroid(const struct kl_gesture *gesture, double *x, double *y);
static void resampled_centroid(struct kl_gesture *gesture, uint64_t now_us, double *x, double *y);
static void reanchor(struct kl_gesture *gesture, uint64_t time_us, double old_x, double old_y);
static void pinch_start(struct kl_gesture *gesture);
static void emit(struct kl_gesture *gesture, unsigned kind, double x, double y, double vx, double vy);
static void tap(struct kl_gesture *gesture, uint64_t time_us);

/*
 * Creates the gestures of one surface.
 */
struct kl_gesture *
kl_gesture_create(void)
{
	struct kl_gesture *gesture;

	/* Everything at zero: no finger, idle. */
	gesture = calloc(1, sizeof(*gesture));
	if (gesture == NULL)
		return NULL;

	/* The motion device the fingers teach. */
	gesture->device = kl_motion_device_create();
	if (gesture->device == NULL) {
		free(gesture);
		return NULL;
	}

	/* Succeeded: the new gestures. */
	return gesture;
}

/*
 * Destroys the gestures of a surface.
 */
void
kl_gesture_destroy(
	struct kl_gesture *gesture)
{
	unsigned index;

	/* Nothing to destroy. */
	if (gesture == NULL)
		return;

	/* The fingers' motions, the device, the gestures. */
	for (index = 0; index < GESTURE_FINGERS; index++)
		kl_motion_destroy(gesture->fingers[index].motion);
	kl_motion_device_destroy(gesture->device);
	free(gesture);
}

/*
 * A finger touches.
 */
int
kl_gesture_down(
	struct kl_gesture *gesture,
	int32_t id,
	uint64_t time_us,
	uint64_t arrival_us,
	double x,
	double y)
{
	struct gesture_finger *finger;
	double old_x;
	double old_y;
	unsigned index;

	/* Refuses a missing surface and a finger already down. */
	if (gesture == NULL)
		return EINVAL;
	finger = finger_of(gesture, id);
	if (finger != NULL)
		return EEXIST;

	/* A free place for the finger. */
	for (index = 0; index < GESTURE_FINGERS; index++) {
		if (!gesture->fingers[index].used) {
			finger = &gesture->fingers[index];
			break;
		}
	}

	/* Refuses a finger past the ones followed. */
	if (finger == NULL)
		return EBUSY;

	/* A drag measures its fingers' centroid before this one joins. */
	old_x = 0.0;
	old_y = 0.0;
	if (gesture->state == GESTURE_DRAG)
		resampled_centroid(gesture, time_us, &old_x, &old_y);

	/* The finger's motion starts with this report. */
	if (finger->motion == NULL)
		finger->motion = kl_motion_create(gesture->device);
	if (finger->motion == NULL)
		return ENOMEM;
	kl_motion_begin(finger->motion);
	(void)kl_motion_add(finger->motion, time_us, arrival_us, x, y);
	finger->used = 1;
	finger->id = id;
	finger->x = x;
	finger->y = y;
	gesture->count++;

	/* The first finger starts a press: where and when. */
	if (gesture->count == 1U) {
		gesture->state = GESTURE_PRESSED;
		gesture->press_x = x;
		gesture->press_y = y;
		gesture->press_us = time_us;
		gesture->after_long_press = 0;
		return 0;
	}

	/* A second finger: a press can only drag now, measured from the new centroid. */
	if (gesture->state == GESTURE_PRESSED || gesture->state == GESTURE_LONG) {
		gesture->state = GESTURE_MULTI_PRESSED;
		raw_centroid(gesture, &gesture->press_x, &gesture->press_y);
	} else if (gesture->state == GESTURE_DRAG) {
		reanchor(gesture, time_us, old_x, old_y);
	}

	/* Two fingers start a pinch. */
	if (gesture->count == 2U)
		pinch_start(gesture);

	/* Succeeded: the finger is followed. */
	return 0;
}

/*
 * A finger moves.
 */
int
kl_gesture_motion(
	struct kl_gesture *gesture,
	int32_t id,
	uint64_t time_us,
	uint64_t arrival_us,
	double x,
	double y)
{
	struct gesture_finger *finger;
	double centroid_x;
	double centroid_y;
	double moved;
	int error;

	/* Refuses a missing surface and a finger that is not down. */
	if (gesture == NULL)
		return EINVAL;
	finger = finger_of(gesture, id);
	if (finger == NULL)
		return ENOENT;

	/* The report goes into the finger's motion; a report out of order starts its stroke again. */
	error = kl_motion_add(finger->motion, time_us, arrival_us, x, y);
	if (error != 0) {
		kl_motion_begin(finger->motion);
		(void)kl_motion_add(finger->motion, time_us, arrival_us, x, y);
	}

	/* The finger's last place. */
	finger->x = x;
	finger->y = y;

	/* Only a press can become a drag. */
	if (gesture->state != GESTURE_PRESSED &&
	    gesture->state != GESTURE_MULTI_PRESSED &&
	    gesture->state != GESTURE_LONG)
		return 0;

	/* A press whose centroid moved past the slop drags, measured from where it pressed. */
	raw_centroid(gesture, &centroid_x, &centroid_y);
	moved = hypot(centroid_x - gesture->press_x, centroid_y - gesture->press_y);
	if (moved < GESTURE_SLOP)
		return 0;
	if (gesture->state == GESTURE_LONG)
		gesture->after_long_press = 1;
	gesture->state = GESTURE_DRAG;
	gesture->base_x = 0.0;
	gesture->base_y = 0.0;
	gesture->anchor_x = gesture->press_x;
	gesture->anchor_y = gesture->press_y;
	emit(gesture, KL_GESTURE_DRAG_BEGIN, gesture->press_x, gesture->press_y, 0.0, 0.0);

	/* Succeeded: the drag began. */
	return 0;
}

/*
 * A finger lifts.
 */
int
kl_gesture_up(
	struct kl_gesture *gesture,
	int32_t id,
	uint64_t time_us)
{
	struct gesture_finger *finger;
	double old_x;
	double old_y;
	double vx;
	double vy;

	/* Refuses a missing surface and a finger that is not down. */
	if (gesture == NULL)
		return EINVAL;
	finger = finger_of(gesture, id);
	if (finger == NULL)
		return ENOENT;

	/* A drag measures its fingers' centroid before this one lifts. */
	old_x = 0.0;
	old_y = 0.0;
	if (gesture->state == GESTURE_DRAG)
		resampled_centroid(gesture, time_us, &old_x, &old_y);

	/* The finger's velocity at the lift; its stroke teaches the device. */
	vx = 0.0;
	vy = 0.0;
	(void)kl_motion_velocity(finger->motion, time_us, &vx, &vy);
	kl_motion_end(finger->motion);
	finger->used = 0;
	gesture->count--;

	/* A single finger that lifted within the slop before the long press taps. */
	if (gesture->state == GESTURE_PRESSED) {
		tap(gesture, time_us);
		gesture->state = GESTURE_IDLE;
		return 0;
	}

	/* A drag ends with the last finger, with its velocity; before that it follows the fingers left. */
	if (gesture->state == GESTURE_DRAG) {
		if (gesture->count == 0U) {
			emit(gesture, KL_GESTURE_DRAG_END, finger->x, finger->y, vx, vy);
			gesture->state = GESTURE_IDLE;
		} else {
			reanchor(gesture, time_us, old_x, old_y);
			if (gesture->count == 2U)
				pinch_start(gesture);
		}

		/* The drag goes on, or ended. */
		return 0;
	}

	/* A long press, or fingers that pressed without moving, end with the last finger. */
	if (gesture->count == 0U)
		gesture->state = GESTURE_IDLE;

	/* Succeeded: the finger is no longer followed. */
	return 0;
}

/*
 * The compositor took the fingers.
 */
void
kl_gesture_cancel(
	struct kl_gesture *gesture)
{
	unsigned index;

	/* Nothing to cancel. */
	if (gesture == NULL)
		return;

	/* Every finger is forgotten, without teaching the device (the strokes were cut short). */
	for (index = 0; index < GESTURE_FINGERS; index++) {
		if (!gesture->fingers[index].used)
			continue;
		kl_motion_begin(gesture->fingers[index].motion);
		gesture->fingers[index].used = 0;
	}

	/* No finger is down. */
	gesture->count = 0;

	/* Whatever was going on ends. */
	gesture->state = GESTURE_IDLE;
	emit(gesture, KL_GESTURE_CANCEL, 0.0, 0.0, 0.0, 0.0);
}

/*
 * Takes the next gesture, after judging the time now.
 */
int
kl_gesture_next(
	struct kl_gesture *gesture,
	uint64_t now_us,
	struct kl_gesture_event *event)
{
	/* Refuses a missing surface or result. */
	if (gesture == NULL)
		return 0;
	if (event == NULL)
		return 0;

	/* A press held long enough is a long press. */
	if (gesture->state == GESTURE_PRESSED &&
	    now_us >= gesture->press_us &&
	    now_us - gesture->press_us >= GESTURE_LONG_PRESS) {
		gesture->state = GESTURE_LONG;
		emit(gesture, KL_GESTURE_LONG_PRESS, gesture->press_x, gesture->press_y, 0.0, 0.0);
	}

	/* Nothing waiting. */
	if (gesture->queue_count == 0U)
		return 0;

	/* Succeeded: the oldest gesture waiting. */
	*event = gesture->queue[gesture->queue_head];
	gesture->queue_head = (gesture->queue_head + 1U) % GESTURE_QUEUE;
	gesture->queue_count--;
	return 1;
}

/*
 * Gives how far the dragging fingers' centroid has moved since the drag
 * began, for a frame drawn at now_us.
 */
int
kl_gesture_drag_offset(
	struct kl_gesture *gesture,
	uint64_t now_us,
	double *dx,
	double *dy)
{
	double x;
	double y;

	/* Refuses a missing surface or result, and a surface that does not drag. */
	if (gesture == NULL)
		return EINVAL;
	if (dx == NULL)
		return EINVAL;
	if (dy == NULL)
		return EINVAL;
	if (gesture->state != GESTURE_DRAG)
		return ENOENT;

	/* The earlier finger sets' offset and the present centroid's movement since its anchor. */
	resampled_centroid(gesture, now_us, &x, &y);
	*dx = gesture->base_x + x - gesture->anchor_x;
	*dy = gesture->base_y + y - gesture->anchor_y;

	/* Succeeded: the drag's offset. */
	return 0;
}

/*
 * Gives two fingers' distance against their starting distance, and their
 * centroid, for a frame drawn at now_us.
 */
int
kl_gesture_pinch(
	struct kl_gesture *gesture,
	uint64_t now_us,
	double *scale,
	double *x,
	double *y)
{
	struct gesture_finger *pair[2];
	double points[2][2];
	double distance;
	unsigned found;
	unsigned index;
	int error;

	/* Refuses a missing surface or result, and anything but two fingers. */
	if (gesture == NULL)
		return EINVAL;
	if (scale == NULL)
		return EINVAL;
	if (x == NULL)
		return EINVAL;
	if (y == NULL)
		return EINVAL;
	if (gesture->count != 2U || gesture->pinch_distance <= 0.0)
		return ENOENT;

	/* The two fingers where their motions put them for the frame. */
	found = 0;
	for (index = 0; index < GESTURE_FINGERS && found < 2U; index++) {
		if (!gesture->fingers[index].used)
			continue;
		pair[found] = &gesture->fingers[index];
		error = kl_motion_point(pair[found]->motion, now_us, KL_MOTION_EXTRAPOLATION_CONTENT,
					     &points[found][0], &points[found][1]);
		if (error != 0) {
			points[found][0] = pair[found]->x;
			points[found][1] = pair[found]->y;
		}

		/* The next finger. */
		found++;
	}

	/* Their distance against the start, and their middle. */
	distance = hypot(points[1][0] - points[0][0], points[1][1] - points[0][1]);
	*scale = distance / gesture->pinch_distance;
	*x = (points[0][0] + points[1][0]) / 2.0;
	*y = (points[0][1] + points[1][1]) / 2.0;

	/* Succeeded: the pinch. */
	return 0;
}

/* Finds the finger down with a wl_touch id; NULL when none is. */
static struct gesture_finger *
finger_of(
	struct kl_gesture *gesture,
	int32_t id)
{
	unsigned index;

	/* Looks among the fingers down. */
	for (index = 0; index < GESTURE_FINGERS; index++) {
		if (gesture->fingers[index].used && gesture->fingers[index].id == id)
			return &gesture->fingers[index];
	}

	/* No finger has it. */
	return NULL;
}

/* Gives the centroid of the fingers' last reported places. */
static void
raw_centroid(
	const struct kl_gesture *gesture,
	double *x,
	double *y)
{
	unsigned index;
	unsigned count;

	/* The mean of the fingers down (the origin when there are none). */
	*x = 0.0;
	*y = 0.0;
	count = 0;
	for (index = 0; index < GESTURE_FINGERS; index++) {
		if (!gesture->fingers[index].used)
			continue;
		*x += gesture->fingers[index].x;
		*y += gesture->fingers[index].y;
		count++;
	}

	/* Succeeded: the mean. */
	if (count != 0U) {
		*x /= (double)count;
		*y /= (double)count;
	}
}

/* Gives the centroid of the fingers where their motions put them for a frame. */
static void
resampled_centroid(
	struct kl_gesture *gesture,
	uint64_t now_us,
	double *x,
	double *y)
{
	struct gesture_finger *finger;
	double point_x;
	double point_y;
	unsigned index;
	unsigned count;
	int error;

	/* The mean of each finger's resampled point (its last report without one). */
	*x = 0.0;
	*y = 0.0;
	count = 0;
	for (index = 0; index < GESTURE_FINGERS; index++) {
		finger = &gesture->fingers[index];
		if (!finger->used)
			continue;
		error = kl_motion_point(finger->motion, now_us, KL_MOTION_EXTRAPOLATION_CONTENT, &point_x, &point_y);
		if (error != 0) {
			point_x = finger->x;
			point_y = finger->y;
		}

		/* Adds it to the sum. */
		*x += point_x;
		*y += point_y;
		count++;
	}

	/* Succeeded: the mean. */
	if (count != 0U) {
		*x /= (double)count;
		*y /= (double)count;
	}
}

/*
 * Keeps a drag's offset when its fingers change: the offset up to the
 * change (the old fingers' centroid, old_x and old_y, against the anchor)
 * goes into the base, and the new fingers' centroid becomes the anchor.
 * Both centroids are resampled for the change's time, as the drag's offset
 * is for a frame, so the offset does not jump.
 */
static void
reanchor(
	struct kl_gesture *gesture,
	uint64_t time_us,
	double old_x,
	double old_y)
{
	double x;
	double y;

	/* The drag's offset up to the change stays. */
	gesture->base_x += old_x - gesture->anchor_x;
	gesture->base_y += old_y - gesture->anchor_y;

	/* The fingers now down are measured from their centroid now. */
	resampled_centroid(gesture, time_us, &x, &y);
	gesture->anchor_x = x;
	gesture->anchor_y = y;
}

/* Starts a pinch from the distance of the two fingers down. */
static void
pinch_start(
	struct kl_gesture *gesture)
{
	double points[2][2];
	unsigned found;
	unsigned index;

	/* The two fingers' places. */
	found = 0;
	for (index = 0; index < GESTURE_FINGERS && found < 2U; index++) {
		if (!gesture->fingers[index].used)
			continue;
		points[found][0] = gesture->fingers[index].x;
		points[found][1] = gesture->fingers[index].y;
		found++;
	}

	/* Their distance is the pinch's 1. */
	gesture->pinch_distance = 0.0;
	if (found == 2U)
		gesture->pinch_distance = hypot(points[1][0] - points[0][0], points[1][1] - points[0][1]);
}

/* Queues a gesture (the oldest is dropped when the queue is full). */
static void
emit(
	struct kl_gesture *gesture,
	unsigned kind,
	double x,
	double y,
	double vx,
	double vy)
{
	struct kl_gesture_event *event;
	unsigned slot;

	/* A full queue loses its oldest gesture. */
	if (gesture->queue_count == GESTURE_QUEUE) {
		gesture->queue_head = (gesture->queue_head + 1U) % GESTURE_QUEUE;
		gesture->queue_count--;
	}

	/* The gesture after the ones waiting. */
	slot = (gesture->queue_head + gesture->queue_count) % GESTURE_QUEUE;
	event = &gesture->queue[slot];
	event->kind = kind;
	event->x = x;
	event->y = y;
	event->vx = vx;
	event->vy = vy;
	event->fingers = gesture->count;
	event->after_long_press = gesture->after_long_press;
	gesture->queue_count++;
}

/* Queues a tap at the press, and a double tap when it follows the last tap closely. */
static void
tap(
	struct kl_gesture *gesture,
	uint64_t time_us)
{
	double distance;
	int twice;

	/* The tap. */
	emit(gesture, KL_GESTURE_TAP, gesture->press_x, gesture->press_y, 0.0, 0.0);

	/* A double tap: soon after and near the last tap (which is then used up). */
	twice = 0;
	distance = hypot(gesture->press_x - gesture->last_tap_x, gesture->press_y - gesture->last_tap_y);
	if (gesture->last_tap &&
	    time_us >= gesture->last_tap_us &&
	    time_us - gesture->last_tap_us <= GESTURE_DOUBLE_WITHIN &&
	    distance <= GESTURE_DOUBLE_NEAR)
		twice = 1;
	if (twice) {
		emit(gesture, KL_GESTURE_DOUBLE_TAP, gesture->press_x, gesture->press_y, 0.0, 0.0);
		gesture->last_tap = 0;
		return;
	}

	/* Succeeded: this tap may start a double tap. */
	gesture->last_tap = 1;
	gesture->last_tap_us = time_us;
	gesture->last_tap_x = gesture->press_x;
	gesture->last_tap_y = gesture->press_y;
}
