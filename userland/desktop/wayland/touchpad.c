/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch pad layer (ws159-p004): a touch pad's fingers made into a
 * pointer.
 *
 * The kernel reports a touch pad as a multitouch device of protocol B with
 * BTN_LEFT for the pad pressed (a click pad), and makes no gesture of the
 * fingers (plan/ws159/phase001 D1).  This file makes them, as the
 * 2026-10-04 user's touch pad rules (BUG-166) say:
 *
 *   - one finger moving moves the pointer, faster the faster it moves;
 *   - a short touch that hardly moves is a tap: one finger the left
 *     button, two the right one, three the middle one;
 *   - a finger that touches again within TAP_DRAG_MS of a tap and moves
 *     drags with the left button held until it lifts, the press made where
 *     the tap was; one that lifts again at once makes the second click of
 *     a double click;
 *   - the pad pressed is the left button (the right one with two fingers on
 *     the pad), and moving while it is pressed drags; the finger's first
 *     millimetre after the press is not motion, so the press itself does
 *     not move the pointer;
 *   - two fingers moving together scroll, by wheel notches, in the natural
 *     direction (the content follows the fingers) unless that is turned
 *     off.
 *
 * The gestures (ws142-p003; the 2026-10-04 user requests and the decisions
 * D1, D3 and D10 of plan/ws142/phase001):
 *
 *   - two fingers of which one (or both) touches within EDGE_UM of the
 *     bottom edge and that move up, of the left edge and move right, or of
 *     the right edge and move left, are a gesture (BOTTOM2, LEFT2, RIGHT2)
 *     rather than a scroll (one finger in the band is enough, ws181-p008):
 *     the first DECIDE_UM of their mean travel decides, and is held back
 *     meanwhile (a touch that turns out to be a scroll scrolls by it then);
 *   - the band is judged where each finger landed, measured from the
 *     axes' least and largest places alike, and fingers that land up to
 *     STAGGER_MS apart are one landing even if the first one has moved
 *     meanwhile (BUG-254: fingers seldom land at once);
 *   - two fingers of which one touches within EDGE_UM of the top edge and
 *     that move down are TOP2 (ws142-p009; App Home, ws181-p008);
 *   - the end of a touch of two fingers that scrolled (all of them lifted,
 *     or one of them) is told as SWIPE2's end, after its scrolling, so
 *     that the shell can make one swipe one step (ws142-p009);
 *   - three fingers that move up (DECIDE3_UM, mostly up) are UP3; three
 *     fingers moving otherwise do nothing (they move no pointer);
 *   - a tap of three fingers is TAP3 (the switcher), not the middle button,
 *     which pressing the pad with three fingers still gives.
 *
 * A gesture follows the fingers: it begins, reports their travel along its
 * way (inward from its edge, or up) with every report, and ends when they
 * lift (with their speed) or is given up when a finger more comes.  The
 * pad's size (kwl_touchpad_set_size) is needed for the edges; without it
 * only UP3 and TAP3 are made.
 *
 * A tap's click, its press and its release, is given when the finger lifts
 * (only then is it a tap), so the click completes at once (ws183-p002, the
 * 2026-10-08 UAT: the release used to wait TAP_DRAG_MS for a drag that
 * seldom comes).  A touch within TAP_DRAG_MS after it holds no button and
 * moves no pointer until it is a drag (it moved DRAG_START_UM: the press
 * comes then, before the motion held back) or a second tap (the second
 * click).  The file knows nothing of the seat; input.c applies the
 * actions.  The evdev codes are written here as numbers: they are the same
 * on every operating system.
 */

#include "touchpad.h"
#include "pointer-accel.h"

#include <stddef.h>
#include <string.h>

/* The evdev event types and codes a touch pad's reports use. */
#define TOUCHPAD_EV_KEY			0x01U
#define TOUCHPAD_EV_ABS			0x03U
#define TOUCHPAD_ABS_MT_SLOT		0x2fU
#define TOUCHPAD_ABS_MT_POSITION_X	0x35U
#define TOUCHPAD_ABS_MT_POSITION_Y	0x36U
#define TOUCHPAD_ABS_MT_TRACKING_ID	0x39U

/* A tap: at most this long and this far (micrometres), and the time a drag may start after it. */
#define TAP_MS				180U
#define TAP_TRAVEL_UM			3000
#define TAP_DRAG_MS			300U

/* The travel that makes a touch after a tap a drag, and the press's quiet travel (micrometres). */
#define DRAG_START_UM			1000
#define PRESS_QUIET_UM			1000

/* The finger travel of one wheel notch when two fingers scroll (micrometres). */
#define SCROLL_NOTCH_UM			KWL_TOUCHPAD_NOTCH_UM

/* The edges' band, and the travel that decides a two-finger and a three-finger gesture (micrometres). */
#define EDGE_UM				6000
#define DECIDE_UM			4000
#define DECIDE3_UM			8000

/* Two fingers that land at most this far apart in time landed together: the edges are judged where they landed (BUG-254). */
#define STAGGER_MS			150U

/* The edges a finger of a two-finger touch started in. */
#define EDGE_BOTTOM			0x1U
#define EDGE_LEFT			0x2U
#define EDGE_RIGHT			0x4U
#define EDGE_TOP			0x8U

/* Whether a touch of two or three fingers is decided: not yet, as no gesture (a scroll, or nothing), or as a gesture. */
#define DECIDED_NOT			0U
#define DECIDED_OTHER			1U
#define DECIDED_GESTURE			2U
#define DECIDED_SPENT			3U

/*
 * A gesture's speed is its travel over about the last SPEED_WINDOW_MS,
 * never divided by less than SPEED_SPAN_MS: reports read in a burst (the
 * compositor late, the reports bunched) then give no false flick (T1-126).
 */
#define SPEED_WINDOW_MS			100U
#define SPEED_SPAN_MS			50U

/*
 * The pointer's gain in pixels per millimetre, in 1/256: the level's slow
 * gain up to SPEED_SLOW millimetres per second, rising to its fast gain at
 * SPEED_FAST and above (ws089-p024).  The medium level is the curve of
 * ws159-p004; none keeps one gain at every speed.
 */
#define SPEED_SLOW			20
#define SPEED_FAST			150
#define ACCELERATION_LEVELS		4

/* Each level's slow and fast gains (none, mild, medium, strong), in 1/256 pixels per millimetre. */
static const int64_t gain_slow[ACCELERATION_LEVELS] = { 8 * 256, 6 * 256, 5 * 256, 4 * 256 };
static const int64_t gain_fast[ACCELERATION_LEVELS] = { 8 * 256, 11 * 256, 15 * 256, 20 * 256 };

/* The time between reports the speed is measured over, at least and at most (milliseconds). */
#define FRAME_MS_LEAST			1U
#define FRAME_MS_MOST			50U

static unsigned active_fingers(const struct kwl_touchpad *pad);
static void take_button(struct kwl_touchpad *pad, unsigned fingers, struct kwl_touchpad_actions *actions);
static void touch_begin(struct kwl_touchpad *pad, uint64_t now_ms, unsigned fingers);
static void take_motion(struct kwl_touchpad *pad, uint64_t now_ms, unsigned fingers, struct kwl_touchpad_actions *actions);
static void touch_end(struct kwl_touchpad *pad, uint64_t now_ms, struct kwl_touchpad_actions *actions);
static void pointer_motion(struct kwl_touchpad *pad, uint64_t now_ms, int64_t dx_um, int64_t dy_um, struct kwl_touchpad_actions *actions);
static void scroll(struct kwl_touchpad *pad, int64_t dx_um, int64_t dy_um, struct kwl_touchpad_actions *actions);
static void tap_finish(struct kwl_touchpad *pad);
static void tap_drag_begin(struct kwl_touchpad *pad, struct kwl_touchpad_actions *actions);
static void swipe_finish(struct kwl_touchpad *pad, struct kwl_touchpad_actions *actions);
static void fingers_changed(struct kwl_touchpad *pad, uint64_t now_ms, unsigned fingers, struct kwl_touchpad_actions *actions);
static uint32_t edges_of_fingers(const struct kwl_touchpad *pad);
static int fingers_landed_together(const struct kwl_touchpad *pad);
static void gesture_motion(struct kwl_touchpad *pad, uint64_t now_ms, unsigned fingers, int64_t dx_um, int64_t dy_um, struct kwl_touchpad_actions *actions);
static void gesture_begin(struct kwl_touchpad *pad, uint64_t now_ms, uint32_t gesture, unsigned fingers, struct kwl_touchpad_actions *actions);
static void gesture_end(struct kwl_touchpad *pad, uint32_t phase, uint64_t now_ms, struct kwl_touchpad_actions *actions);
static void gesture_sample(struct kwl_touchpad *pad, uint64_t now_ms);
static int64_t gesture_speed_at(const struct kwl_touchpad *pad, uint64_t now_ms);
static int64_t gesture_along(uint32_t gesture, int64_t dx_um, int64_t dy_um);
static void push_gesture(struct kwl_touchpad_actions *actions, uint32_t gesture, uint32_t phase, int64_t travel_um, int64_t speed);
static void push_button(struct kwl_touchpad_actions *actions, uint32_t button, uint32_t pressed);
static void push_motion(struct kwl_touchpad_actions *actions, int32_t dx, int32_t dy);
static void push_scroll(struct kwl_touchpad_actions *actions, int32_t vertical, int32_t horizontal, int32_t vertical_units, int32_t horizontal_units);
static int64_t magnitude(int64_t value);

/*
 * Starts a touch pad with no finger on it, of a resolution in units per
 * millimetre (a resolution below one is taken as one).
 */
void
kwl_touchpad_init(
	struct kwl_touchpad *pad,
	int32_t resolution_x,
	int32_t resolution_y)
{
	unsigned index;

	/* Nothing is known yet. */
	memset(pad, 0, sizeof(*pad));

	/* No slot holds a finger. */
	for (index = 0; index < KWL_TOUCHPAD_SLOTS; index++)
		pad->fingers[index].tracking = -1;

	/* The units, at least one per millimetre. */
	pad->resolution_x = resolution_x;
	if (pad->resolution_x < 1)
		pad->resolution_x = 1;
	pad->resolution_y = resolution_y;
	if (pad->resolution_y < 1)
		pad->resolution_y = 1;

	/* Scrolling follows the fingers unless the user turns it off; the medium curve. */
	pad->natural_scroll = 1;
	pad->acceleration = KWL_ACCEL_MEDIUM;
	pad->tap = KWL_TOUCHPAD_TAP_NONE;
}

/*
 * Gives the pad's size, its largest place across and down in its units
 * (the axes' maxima, the least places being zero), which the edges'
 * gestures need.
 */
void
kwl_touchpad_set_size(
	struct kwl_touchpad *pad,
	int32_t x_max,
	int32_t y_max)
{
	/* Axes that start at zero. */
	kwl_touchpad_set_range(pad, 0, x_max, 0, y_max);
}

/*
 * Gives the pad's least and largest places across and down in its units
 * (the axes' minima and maxima), which the edges' gestures are measured
 * from (BUG-254: each edge from its own end of the axis).
 */
void
kwl_touchpad_set_range(
	struct kwl_touchpad *pad,
	int32_t x_min,
	int32_t x_max,
	int32_t y_min,
	int32_t y_max)
{
	/* A range of nothing is not known: no edge then. */
	pad->x_min = 0;
	pad->y_min = 0;
	pad->x_max = 0;
	pad->y_max = 0;
	if (x_max > x_min && y_max > y_min) {
		pad->x_min = x_min;
		pad->y_min = y_min;
		pad->x_max = x_max;
		pad->y_max = y_max;
	}
}

/*
 * Takes the user's feel (ws089-p024): the acceleration's level (out of
 * range: the nearest) and whether the scrolling follows the fingers.
 */
void
kwl_touchpad_set_feel(
	struct kwl_touchpad *pad,
	int32_t acceleration,
	int32_t natural)
{
	/* The level, kept in the table. */
	if (acceleration < 0)
		acceleration = 0;
	if (acceleration >= (int32_t)ACCELERATION_LEVELS)
		acceleration = (int32_t)ACCELERATION_LEVELS - 1;
	pad->acceleration = acceleration;

	/* The scrolling's direction. */
	pad->natural_scroll = natural != 0;
}

/*
 * Takes one event of a report: a slot chosen, a finger's tracking
 * identifier or place, or the pad's button.
 */
void
kwl_touchpad_event(
	struct kwl_touchpad *pad,
	uint16_t type,
	uint16_t code,
	int32_t value)
{
	struct kwl_touchpad_finger *finger;

	/* The pad pressed or let go. */
	if (type == TOUCHPAD_EV_KEY && code == KWL_TOUCHPAD_BUTTON_LEFT) {
		pad->button_down = 0;
		if (value != 0)
			pad->button_down = 1;
		pad->button_changed = 1;
		return;
	}

	/* Every other event of a finger is an absolute one. */
	if (type != TOUCHPAD_EV_ABS)
		return;

	/* The slot the next events address. */
	if (code == TOUCHPAD_ABS_MT_SLOT) {
		pad->slot = value;
		return;
	}

	/* The finger of the slot addressed; a slot past the table is set aside. */
	if (pad->slot < 0 || (uint32_t)pad->slot >= KWL_TOUCHPAD_SLOTS)
		return;
	finger = &pad->fingers[pad->slot];

	/* A finger comes (a new identifier) or goes (-1). */
	if (code == TOUCHPAD_ABS_MT_TRACKING_ID) {
		finger->tracking = value;
		finger->fresh = 0;
		if (value >= 0)
			finger->fresh = 1;
		return;
	}

	/* Its place across; a new finger starts where it is. */
	if (code == TOUCHPAD_ABS_MT_POSITION_X) {
		finger->x = value;
		if (finger->fresh)
			finger->last_x = value;
		return;
	}

	/* Its place down. */
	if (code == TOUCHPAD_ABS_MT_POSITION_Y) {
		finger->y = value;
		if (finger->fresh)
			finger->last_y = value;
	}
}

/*
 * Ends a report: the button, the touch's beginning, the motion or the
 * scrolling, and the touch's end (a tap) become actions.
 */
void
kwl_touchpad_frame(
	struct kwl_touchpad *pad,
	uint64_t now_ms,
	struct kwl_touchpad_actions *actions)
{
	unsigned fingers;
	unsigned index;

	/* Nothing to do yet; the fingers now on the pad. */
	actions->count = 0;
	fingers = active_fingers(pad);

	/* A finger that came in this report landed here and now. */
	for (index = 0; index < KWL_TOUCHPAD_SLOTS; index++) {
		if (pad->fingers[index].tracking >= 0 && pad->fingers[index].fresh) {
			pad->fingers[index].start_x = pad->fingers[index].x;
			pad->fingers[index].start_y = pad->fingers[index].y;
			pad->fingers[index].start_ms = now_ms;
		}
	}

	/* The pad pressed or let go. */
	if (pad->button_changed)
		take_button(pad, fingers, actions);

	/* A touch begins when the first finger comes. */
	if (pad->fingers_before == 0U && fingers != 0U)
		touch_begin(pad, now_ms, fingers);

	/* The most fingers the touch had. */
	if (fingers > pad->touch_fingers)
		pad->touch_fingers = fingers;

	/* A finger came or went during the touch: a gesture under way ends or is given up, a new count starts deciding. */
	if (fingers != 0U &&
	    fingers != pad->fingers_before &&
	    pad->fingers_before != 0U)
		fingers_changed(pad, now_ms, fingers, actions);

	/* The fingers' motion moves the pointer or scrolls. */
	if (fingers != 0U)
		take_motion(pad, now_ms, fingers, actions);

	/* A touch ends when the last finger goes: perhaps a tap. */
	if (pad->fingers_before != 0U && fingers == 0U)
		touch_end(pad, now_ms, actions);

	/* The places at this report's end are where the next report's motion is measured from. */
	for (index = 0; index < KWL_TOUCHPAD_SLOTS; index++) {
		pad->fingers[index].last_x = pad->fingers[index].x;
		pad->fingers[index].last_y = pad->fingers[index].y;
		pad->fingers[index].fresh = 0;
	}

	/* Succeeded: the next report starts from these fingers. */
	pad->fingers_before = fingers;
	pad->last_frame_ms = now_ms;
}

/*
 * Lets time pass: a tap whose drag did not come in time stops waiting for
 * it (its click was given at its lift, so no button moves).
 */
void
kwl_touchpad_tick(
	struct kwl_touchpad *pad,
	uint64_t now_ms,
	struct kwl_touchpad_actions *actions)
{
	/* Nothing to do yet. */
	actions->count = 0;

	/* A tap waiting for a drag that did not come waits no more. */
	if (pad->tap == KWL_TOUCHPAD_TAP_PENDING && now_ms >= pad->tap_deadline_ms)
		tap_finish(pad);
}

/*
 * Lets every button the pad holds go and gives up a gesture under way (the
 * device is going away).
 */
void
kwl_touchpad_release_all(
	struct kwl_touchpad *pad,
	struct kwl_touchpad_actions *actions)
{
	/* Nothing to do yet. */
	actions->count = 0;

	/* A tap drag's left button; a tap's click is complete and holds none. */
	if (pad->tap == KWL_TOUCHPAD_TAP_DRAG)
		push_button(actions, KWL_TOUCHPAD_BUTTON_LEFT, 0U);
	pad->tap = KWL_TOUCHPAD_TAP_NONE;
	pad->tap_held_x = 0;
	pad->tap_held_y = 0;

	/* The pad's own press. */
	if (pad->button_sent != 0U)
		push_button(actions, pad->button_sent, 0U);
	pad->button_sent = 0;

	/* A gesture under way is given up. */
	if (pad->gesture != KWL_TOUCHPAD_GESTURE_NONE) {
		gesture_end(pad, KWL_TOUCHPAD_PHASE_CANCEL, pad->last_frame_ms, actions);
		pad->decided = DECIDED_SPENT;
	}

	/* A touch that scrolled has gone with the device. */
	swipe_finish(pad, actions);
}

/* Counts the fingers on the pad. */
static unsigned
active_fingers(
	const struct kwl_touchpad *pad)
{
	unsigned count;
	unsigned index;

	/* Every slot with a tracking identifier holds a finger. */
	count = 0;
	for (index = 0; index < KWL_TOUCHPAD_SLOTS; index++) {
		if (pad->fingers[index].tracking >= 0)
			count++;
	}

	/* Succeeded: the number of fingers. */
	return count;
}

/*
 * Takes the pad pressed (the left button, the right one with two fingers
 * on the pad, the middle one with three) or let go (the button the press
 * was given as).
 */
static void
take_button(
	struct kwl_touchpad *pad,
	unsigned fingers,
	struct kwl_touchpad_actions *actions)
{
	uint32_t button;

	/* The change is taken. */
	pad->button_changed = 0;

	/* A press: a tap waiting for its drag waits no more. */
	if (pad->button_down && pad->button_sent == 0U) {
		tap_finish(pad);

		/* A touch after a tap that presses the pad is a press, not the tap's drag or second click; its held motion goes. */
		if (pad->tap == KWL_TOUCHPAD_TAP_SECOND) {
			pad->tap = KWL_TOUCHPAD_TAP_NONE;
			pad->tap_held_x = 0;
			pad->tap_held_y = 0;
		}

		/* A gesture under way is given up, and the touch makes no other. */
		if (pad->gesture != KWL_TOUCHPAD_GESTURE_NONE) {
			gesture_end(pad, KWL_TOUCHPAD_PHASE_CANCEL, pad->last_frame_ms, actions);
			pad->decided = DECIDED_SPENT;
		}

		/* The button of the fingers on the pad. */
		button = KWL_TOUCHPAD_BUTTON_LEFT;
		if (fingers == 2U) {
			button = KWL_TOUCHPAD_BUTTON_RIGHT;
		} else if (fingers >= 3U) {
			button = KWL_TOUCHPAD_BUTTON_MIDDLE;
		}

		/* Presses it; this touch is no tap, and the press's own jolt is no motion. */
		push_button(actions, button, 1U);
		pad->button_sent = button;
		pad->touch_clicked = 1;
		pad->press_quiet = 1;
		pad->touch_travel_um = 0;
		return;
	}

	/* A release lets the pressed button go. */
	if (!pad->button_down && pad->button_sent != 0U) {
		push_button(actions, pad->button_sent, 0U);
		pad->button_sent = 0;
		pad->press_quiet = 0;
	}
}

/*
 * Begins a touch: its time and fingers; a touch within a tap's drag time
 * may become a drag or the second tap of a double click.
 */
static void
touch_begin(
	struct kwl_touchpad *pad,
	uint64_t now_ms,
	unsigned fingers)
{
	/* A tap whose time ran out waits no more before this touch counts. */
	if (pad->tap == KWL_TOUCHPAD_TAP_PENDING && now_ms >= pad->tap_deadline_ms)
		tap_finish(pad);

	/* One finger within the time may be the tap's drag or second tap: its motion is held back until it shows which. */
	if (pad->tap == KWL_TOUCHPAD_TAP_PENDING) {
		if (fingers == 1U) {
			pad->tap = KWL_TOUCHPAD_TAP_SECOND;
			pad->tap_held_x = 0;
			pad->tap_held_y = 0;
		} else {
			tap_finish(pad);
		}
	}

	/* The touch: when it began, its fingers, no travel yet, and whether the pad is pressed. */
	pad->touch_start_ms = now_ms;
	pad->touch_fingers = fingers;
	pad->touch_travel_um = 0;
	pad->touch_clicked = 0;
	if (pad->button_sent != 0U)
		pad->touch_clicked = 1;
	pad->scroll_travel_x_um = 0;
	pad->scroll_travel_y_um = 0;
	pad->scroll_units_x = 0;
	pad->scroll_units_y = 0;
	pad->scrolled = 0U;

	/* No gesture yet; two fingers that came at once start deciding, with the edges they touched. */
	pad->gesture = KWL_TOUCHPAD_GESTURE_NONE;
	pad->decided = DECIDED_NOT;
	pad->edges = 0;
	pad->gesture_dx_um = 0;
	pad->gesture_dy_um = 0;
	if (fingers == 2U)
		pad->edges = edges_of_fingers(pad);
}

/*
 * Takes the fingers' motion since the last report: with one finger, the
 * pad pressed or a tap drag, the finger that moved most moves the pointer;
 * with two fingers and nothing pressed, they scroll.
 */
static void
take_motion(
	struct kwl_touchpad *pad,
	uint64_t now_ms,
	unsigned fingers,
	struct kwl_touchpad_actions *actions)
{
	const struct kwl_touchpad_finger *finger;
	int64_t dx_um;
	int64_t dy_um;
	int64_t best_dx_um;
	int64_t best_dy_um;
	int64_t sum_dx_um;
	int64_t sum_dy_um;
	int64_t best;
	int64_t size;
	unsigned moving;
	unsigned index;

	/* Measures each finger that was on the pad at the last report too. */
	best_dx_um = 0;
	best_dy_um = 0;
	best = 0;
	sum_dx_um = 0;
	sum_dy_um = 0;
	moving = 0;
	for (index = 0; index < KWL_TOUCHPAD_SLOTS; index++) {
		/* A slot without a finger, or with one that came in this report, has no motion. */
		finger = &pad->fingers[index];
		if (finger->tracking < 0 || finger->fresh)
			continue;

		/* Its motion in micrometres. */
		dx_um = (int64_t)(finger->x - finger->last_x) * 1000 / pad->resolution_x;
		dy_um = (int64_t)(finger->y - finger->last_y) * 1000 / pad->resolution_y;
		sum_dx_um += dx_um;
		sum_dy_um += dy_um;
		moving++;

		/* The finger that moved most. */
		size = magnitude(dx_um) + magnitude(dy_um);
		if (size > best) {
			best = size;
			best_dx_um = dx_um;
			best_dy_um = dy_um;
		}
	}

	/* No finger to measure (every one came in this report). */
	if (moving == 0U)
		return;

	/* The touch's travel, which decides a tap and a drag. */
	pad->touch_travel_um += best;

	/* The fingers left after a gesture move nothing. */
	if (pad->decided == DECIDED_SPENT)
		return;

	/* A touch after a tap that moves far enough is a drag: the press, where the tap was, then the motion held back. */
	if (pad->tap == KWL_TOUCHPAD_TAP_SECOND && pad->touch_travel_um >= DRAG_START_UM)
		tap_drag_begin(pad, actions);

	/* Two or three fingers with nothing pressed: a gesture, a scroll (two), or nothing (three). */
	if (fingers >= 2U &&
	    pad->button_sent == 0U &&
	    pad->tap == KWL_TOUCHPAD_TAP_NONE) {
		gesture_motion(pad, now_ms, fingers, sum_dx_um / (int64_t)moving, sum_dy_um / (int64_t)moving, actions);
		return;
	}

	/* The press's first millimetre is not motion. */
	if (pad->press_quiet) {
		if (pad->touch_travel_um < PRESS_QUIET_UM)
			return;
		pad->press_quiet = 0;
	}

	/* Otherwise the finger that moved most moves the pointer. */
	pointer_motion(pad, now_ms, best_dx_um, best_dy_um, actions);
}

/*
 * Ends a touch: a tap drag lets its button go; a second quick tap makes
 * the second click of a double click; a tap of one finger clicks the left
 * button and waits the drag time for a drag; a tap of two fingers clicks
 * the right button, of three it is TAP3.
 */
static void
touch_end(
	struct kwl_touchpad *pad,
	uint64_t now_ms,
	struct kwl_touchpad_actions *actions)
{
	uint64_t duration;
	unsigned tapped;

	/* Whether the touch was a tap: short, hardly moving, the pad not pressed. */
	duration = now_ms - pad->touch_start_ms;
	tapped = 0;
	if (duration <= TAP_MS && pad->touch_travel_um < TAP_TRAVEL_UM && !pad->touch_clicked)
		tapped = 1;

	/* The scrolling's remainder goes with the touch, and a touch that scrolled says it has lifted. */
	pad->scroll_travel_x_um = 0;
	pad->scroll_travel_y_um = 0;
	pad->scroll_units_x = 0;
	pad->scroll_units_y = 0;
	swipe_finish(pad, actions);

	/* A gesture ends with its fingers. */
	if (pad->gesture != KWL_TOUCHPAD_GESTURE_NONE) {
		gesture_end(pad, KWL_TOUCHPAD_PHASE_END, now_ms, actions);
		return;
	}

	/* A tap drag ends with its finger. */
	if (pad->tap == KWL_TOUCHPAD_TAP_DRAG) {
		push_button(actions, KWL_TOUCHPAD_BUTTON_LEFT, 0U);
		pad->tap = KWL_TOUCHPAD_TAP_NONE;
		return;
	}

	/*
	 * A touch after a tap that never became a drag: the first click was
	 * complete already, and the motion held back goes (the pointer stays
	 * where the clicks were).
	 */
	if (pad->tap == KWL_TOUCHPAD_TAP_SECOND) {
		pad->tap = KWL_TOUCHPAD_TAP_NONE;
		pad->tap_held_x = 0;
		pad->tap_held_y = 0;

		/* A quick second touch is the second click of a double click; a long still one adds nothing. */
		if (tapped) {
			push_button(actions, KWL_TOUCHPAD_BUTTON_LEFT, 1U);
			push_button(actions, KWL_TOUCHPAD_BUTTON_LEFT, 0U);
		}

		/* The touch after the tap is done. */
		return;
	}

	/* A touch that was no tap ends here. */
	if (!tapped)
		return;

	/* A tap of one finger: the left click at once, and a drag or a second tap may follow within the drag time. */
	if (pad->touch_fingers == 1U) {
		push_button(actions, KWL_TOUCHPAD_BUTTON_LEFT, 1U);
		push_button(actions, KWL_TOUCHPAD_BUTTON_LEFT, 0U);
		pad->tap = KWL_TOUCHPAD_TAP_PENDING;
		pad->tap_deadline_ms = now_ms + TAP_DRAG_MS;
		return;
	}

	/* A tap of two fingers clicks the right button; of three or more it is the switcher's gesture (D1). */
	if (pad->touch_fingers == 2U) {
		push_button(actions, KWL_TOUCHPAD_BUTTON_RIGHT, 1U);
		push_button(actions, KWL_TOUCHPAD_BUTTON_RIGHT, 0U);
	} else {
		push_gesture(actions, KWL_TOUCHPAD_GESTURE_TAP3, KWL_TOUCHPAD_PHASE_END, 0, 0);
	}
}

/*
 * Moves the pointer by a finger's motion: micrometres to pixels at a gain
 * that grows with the finger's speed, the fractions kept for the next
 * report.
 */
static void
pointer_motion(
	struct kwl_touchpad *pad,
	uint64_t now_ms,
	int64_t dx_um,
	int64_t dy_um,
	struct kwl_touchpad_actions *actions)
{
	uint64_t elapsed;
	int64_t speed;
	int64_t gain;
	int64_t slow;
	int64_t fast;
	int64_t x;
	int64_t y;

	/* The time since the last report, within reason. */
	elapsed = now_ms - pad->last_frame_ms;
	if (elapsed < FRAME_MS_LEAST)
		elapsed = FRAME_MS_LEAST;
	if (elapsed > FRAME_MS_MOST)
		elapsed = FRAME_MS_MOST;

	/* The finger's speed in millimetres per second (micrometres per millisecond). */
	speed = (magnitude(dx_um) + magnitude(dy_um)) / (int64_t)elapsed;

	/* The gain of the user's level: slow, fast, or between them in proportion. */
	slow = gain_slow[pad->acceleration];
	fast = gain_fast[pad->acceleration];
	gain = slow;
	if (speed >= SPEED_FAST) {
		gain = fast;
	} else if (speed > SPEED_SLOW) {
		gain = slow + (fast - slow) * (speed - SPEED_SLOW) / (SPEED_FAST - SPEED_SLOW);
	}

	/* The motion in 1/256 pixels, with what the last reports left over. */
	x = dx_um * gain / 1000 + pad->motion_remainder_x;
	y = dy_um * gain / 1000 + pad->motion_remainder_y;

	/* Whole pixels move the pointer; the fractions wait. */
	pad->motion_remainder_x = x % 256;
	pad->motion_remainder_y = y % 256;
	x /= 256;
	y /= 256;

	/* A touch after a tap holds its motion back until it is a drag, so the drag's press is where the tap was. */
	if (pad->tap == KWL_TOUCHPAD_TAP_SECOND) {
		pad->tap_held_x += x;
		pad->tap_held_y += y;
		return;
	}

	/* The motion, when there is any. */
	if (x != 0 || y != 0)
		push_motion(actions, (int32_t)x, (int32_t)y);
}

/*
 * Scrolls by two fingers' motion: one notch for every SCROLL_NOTCH_UM, and
 * the same travel in the wheel's units, KWL_TOUCHPAD_NOTCH_UNITS a notch
 * (BUG-218: a client hears the fingers at once and smoothly, not a notch
 * of 2.5 mm later); the content follows the fingers when the scrolling is
 * natural.  The notches stay for the desktop's own uses of the wheel (App
 * Home's pages, the volume, the tabs).
 */
static void
scroll(
	struct kwl_touchpad *pad,
	int64_t dx_um,
	int64_t dy_um,
	struct kwl_touchpad_actions *actions)
{
	int64_t vertical;
	int64_t horizontal;
	int64_t vertical_units;
	int64_t horizontal_units;

	/* The fingers' travel, with what earlier reports left over; the touch has scrolled. */
	pad->scroll_travel_x_um += dx_um;
	pad->scroll_travel_y_um += dy_um;
	pad->scroll_units_x += dx_um * KWL_TOUCHPAD_NOTCH_UNITS;
	pad->scroll_units_y += dy_um * KWL_TOUCHPAD_NOTCH_UNITS;
	pad->scrolled = 1U;

	/* Whole notches; the rest waits. */
	vertical = pad->scroll_travel_y_um / SCROLL_NOTCH_UM;
	horizontal = pad->scroll_travel_x_um / SCROLL_NOTCH_UM;
	pad->scroll_travel_y_um -= vertical * SCROLL_NOTCH_UM;
	pad->scroll_travel_x_um -= horizontal * SCROLL_NOTCH_UM;

	/* Whole units; the rest waits too. */
	vertical_units = pad->scroll_units_y / SCROLL_NOTCH_UM;
	horizontal_units = pad->scroll_units_x / SCROLL_NOTCH_UM;
	pad->scroll_units_y -= vertical_units * SCROLL_NOTCH_UM;
	pad->scroll_units_x -= horizontal_units * SCROLL_NOTCH_UM;

	/* Natural scrolling moves the content with the fingers: the other way from the fingers' direction. */
	if (pad->natural_scroll) {
		vertical = -vertical;
		horizontal = -horizontal;
		vertical_units = -vertical_units;
		horizontal_units = -horizontal_units;
	}

	/* The scrolling, when there is any. */
	if (vertical_units != 0 || horizontal_units != 0)
		push_scroll(actions, (int32_t)vertical, (int32_t)horizontal, (int32_t)vertical_units, (int32_t)horizontal_units);
}

/*
 * A finger came or went while the pad is touched: a gesture under way
 * ends (fingers lifting one after the other) or is given up (a finger
 * more), and the rest of the touch does nothing; otherwise the new count
 * starts deciding afresh (fingers seldom land in the same report).  Two
 * fingers keep the edges they landed in while the touch has hardly moved,
 * or when they landed within STAGGER_MS of each other however far the
 * first one moved meanwhile (BUG-254).
 */
static void
fingers_changed(
	struct kwl_touchpad *pad,
	uint64_t now_ms,
	unsigned fingers,
	struct kwl_touchpad_actions *actions)
{
	int together;

	/* A gesture under way. */
	if (pad->gesture != KWL_TOUCHPAD_GESTURE_NONE) {
		if (fingers > pad->gesture_fingers) {
			gesture_end(pad, KWL_TOUCHPAD_PHASE_CANCEL, now_ms, actions);
		} else {
			gesture_end(pad, KWL_TOUCHPAD_PHASE_END, now_ms, actions);
		}

		/* The rest of the touch neither scrolls nor makes another gesture. */
		pad->decided = DECIDED_SPENT;
		return;
	}

	/* A spent touch stays so. */
	if (pad->decided == DECIDED_SPENT)
		return;

	/* A touch that scrolled says it has lifted (a finger of the two went). */
	swipe_finish(pad, actions);

	/* A new count decides afresh. */
	pad->decided = DECIDED_NOT;
	pad->edges = 0;
	pad->gesture_dx_um = 0;
	pad->gesture_dy_um = 0;

	/* Only two fingers have edges to keep. */
	if (fingers != 2U)
		return;

	/* Two fingers that landed together, or a touch that has hardly moved, keep their edges. */
	together = fingers_landed_together(pad);
	if (together || pad->touch_travel_um < TAP_TRAVEL_UM)
		pad->edges = edges_of_fingers(pad);
}

/*
 * Tells the edges the fingers on the pad landed in (EDGE_* bits; none
 * while the pad's size is not known).  One finger of the two in an edge's
 * band is enough (ws181-p008, the 2026-10-07 UAT: fingers seldom land side
 * by side along an edge).  Each band is measured inward from its own end
 * of the axis, the least place for the left and the top edges and the
 * largest for the right and the bottom ones, and a finger is judged where
 * it landed, not where it has moved since (BUG-254).
 */
static uint32_t
edges_of_fingers(
	const struct kwl_touchpad *pad)
{
	const struct kwl_touchpad_finger *finger;
	int32_t band_x;
	int32_t band_y;
	uint32_t edges;
	unsigned index;

	/* Without the size there is no edge. */
	if (pad->x_max <= 0 || pad->y_max <= 0)
		return 0;

	/* The band in units, and no edge until a finger is inside its band. */
	band_x = (int32_t)((int64_t)EDGE_UM * pad->resolution_x / 1000);
	band_y = (int32_t)((int64_t)EDGE_UM * pad->resolution_y / 1000);
	edges = 0;
	for (index = 0; index < KWL_TOUCHPAD_SLOTS; index++) {
		/* Each finger. */
		finger = &pad->fingers[index];
		if (finger->tracking < 0)
			continue;

		/* An edge it landed near is the touch's. */
		if (finger->start_y >= pad->y_max - band_y)
			edges |= EDGE_BOTTOM;
		if (finger->start_x <= pad->x_min + band_x)
			edges |= EDGE_LEFT;
		if (finger->start_x >= pad->x_max - band_x)
			edges |= EDGE_RIGHT;
		if (finger->start_y <= pad->y_min + band_y)
			edges |= EDGE_TOP;
	}

	/* Succeeded: the edges. */
	return edges;
}

/*
 * Tells whether the fingers on the pad landed together: the last of them
 * at most STAGGER_MS after the first (BUG-254).
 */
static int
fingers_landed_together(
	const struct kwl_touchpad *pad)
{
	const struct kwl_touchpad_finger *finger;
	uint64_t earliest;
	uint64_t latest;
	unsigned found;
	unsigned index;

	/* The first and the last landing among the fingers on the pad. */
	earliest = 0;
	latest = 0;
	found = 0;
	for (index = 0; index < KWL_TOUCHPAD_SLOTS; index++) {
		/* Each finger. */
		finger = &pad->fingers[index];
		if (finger->tracking < 0)
			continue;

		/* The first finger seen sets both ends; the others widen them. */
		if (!found) {
			earliest = finger->start_ms;
			latest = finger->start_ms;
			found = 1;
		} else if (finger->start_ms < earliest) {
			earliest = finger->start_ms;
		} else if (finger->start_ms > latest) {
			latest = finger->start_ms;
		}
	}

	/* No finger landed at all. */
	if (!found)
		return 0;

	/* Landings further apart than STAGGER_MS are two touches' worth. */
	if (latest - earliest > STAGGER_MS)
		return 0;

	/* Succeeded: the fingers landed together. */
	return 1;
}

/*
 * Takes two or three fingers' mean motion with nothing pressed: a gesture
 * under way follows it; an undecided touch gathers it until it decides;
 * a touch decided as no gesture scrolls (two fingers) or does nothing.
 */
static void
gesture_motion(
	struct kwl_touchpad *pad,
	uint64_t now_ms,
	unsigned fingers,
	int64_t dx_um,
	int64_t dy_um,
	struct kwl_touchpad_actions *actions)
{
	int64_t delta;
	int64_t across;
	int64_t down;
	uint32_t gesture;

	/* A gesture under way: its travel and speed. */
	if (pad->gesture != KWL_TOUCHPAD_GESTURE_NONE) {
		delta = gesture_along(pad->gesture, dx_um, dy_um);
		pad->gesture_travel_um += delta;
		gesture_sample(pad, now_ms);
		pad->gesture_speed = gesture_speed_at(pad, now_ms);
		push_gesture(actions, pad->gesture, KWL_TOUCHPAD_PHASE_UPDATE, pad->gesture_travel_um, pad->gesture_speed);
		return;
	}

	/* A spent touch does nothing. */
	if (pad->decided == DECIDED_SPENT)
		return;

	/* Decided as no gesture: two fingers scroll, three do nothing. */
	if (pad->decided == DECIDED_OTHER) {
		if (fingers == 2U)
			scroll(pad, dx_um, dy_um, actions);
		return;
	}

	/* Two fingers that touch no edge scroll at once. */
	if (fingers == 2U && pad->edges == 0U) {
		pad->decided = DECIDED_OTHER;
		scroll(pad, dx_um, dy_um, actions);
		return;
	}

	/* Undecided: the travel gathers until it decides. */
	pad->gesture_dx_um += dx_um;
	pad->gesture_dy_um += dy_um;
	across = magnitude(pad->gesture_dx_um);
	down = magnitude(pad->gesture_dy_um);
	if (fingers == 2U && across + down < DECIDE_UM)
		return;
	if (fingers >= 3U && across + down < DECIDE3_UM)
		return;

	/* Two fingers: inward from an edge they touch, mostly along it. */
	gesture = KWL_TOUCHPAD_GESTURE_NONE;
	if (fingers == 2U) {
		if ((pad->edges & EDGE_BOTTOM) != 0U &&
		    pad->gesture_dy_um < 0 &&
		    down >= 2 * across)
			gesture = KWL_TOUCHPAD_GESTURE_BOTTOM2;
		if ((pad->edges & EDGE_LEFT) != 0U &&
		    pad->gesture_dx_um > 0 &&
		    across >= 2 * down)
			gesture = KWL_TOUCHPAD_GESTURE_LEFT2;
		if ((pad->edges & EDGE_RIGHT) != 0U &&
		    pad->gesture_dx_um < 0 &&
		    across >= 2 * down)
			gesture = KWL_TOUCHPAD_GESTURE_RIGHT2;
		if ((pad->edges & EDGE_TOP) != 0U &&
		    pad->gesture_dy_um > 0 &&
		    down >= 2 * across)
			gesture = KWL_TOUCHPAD_GESTURE_TOP2;
	}

	/* Three fingers: up, mostly up. */
	if (fingers >= 3U &&
	    pad->gesture_dy_um < 0 &&
	    down >= 2 * across)
		gesture = KWL_TOUCHPAD_GESTURE_UP3;

	/* No gesture: two fingers scroll by what was held back. */
	if (gesture == KWL_TOUCHPAD_GESTURE_NONE) {
		pad->decided = DECIDED_OTHER;
		if (fingers == 2U)
			scroll(pad, pad->gesture_dx_um, pad->gesture_dy_um, actions);
		return;
	}

	/* The gesture begins with the travel so far. */
	gesture_begin(pad, now_ms, gesture, fingers, actions);
}

/* Begins a gesture with the fingers' travel so far. */
static void
gesture_begin(
	struct kwl_touchpad *pad,
	uint64_t now_ms,
	uint32_t gesture,
	unsigned fingers,
	struct kwl_touchpad_actions *actions)
{
	/* Under way, from the travel that decided it; this touch is no tap. */
	pad->gesture = gesture;
	pad->gesture_fingers = fingers;
	pad->decided = DECIDED_GESTURE;
	pad->gesture_travel_um = gesture_along(gesture, pad->gesture_dx_um, pad->gesture_dy_um);
	pad->gesture_speed = 0;
	pad->gesture_sample_next = 0;
	pad->gesture_sample_count = 0;
	gesture_sample(pad, now_ms);
	pad->touch_travel_um += TAP_TRAVEL_UM;
	push_gesture(actions, gesture, KWL_TOUCHPAD_PHASE_BEGIN, pad->gesture_travel_um, 0);
}

/* Ends a gesture (its fingers lifted) or gives it up (a finger more), with its speed at that time. */
static void
gesture_end(
	struct kwl_touchpad *pad,
	uint32_t phase,
	uint64_t now_ms,
	struct kwl_touchpad_actions *actions)
{
	/* The end, with the travel and the speed (fingers that stopped before they lifted are slow). */
	pad->gesture_speed = gesture_speed_at(pad, now_ms);
	push_gesture(actions, pad->gesture, phase, pad->gesture_travel_um, pad->gesture_speed);
	pad->gesture = KWL_TOUCHPAD_GESTURE_NONE;
	pad->gesture_travel_um = 0;
	pad->gesture_speed = 0;
}

/* Keeps the gesture's travel at a report, the oldest kept one giving way. */
static void
gesture_sample(
	struct kwl_touchpad *pad,
	uint64_t now_ms)
{
	/* In the ring. */
	pad->gesture_sample_ms[pad->gesture_sample_next] = now_ms;
	pad->gesture_sample_um[pad->gesture_sample_next] = pad->gesture_travel_um;
	pad->gesture_sample_next = (pad->gesture_sample_next + 1U) % KWL_TOUCHPAD_SAMPLES;
	if (pad->gesture_sample_count < KWL_TOUCHPAD_SAMPLES)
		pad->gesture_sample_count++;
}

/*
 * Measures a gesture's speed at a time (micrometres a second): its travel
 * since the newest kept report at least SPEED_WINDOW_MS old (or the oldest
 * kept), over that time but never less than SPEED_SPAN_MS.
 */
static int64_t
gesture_speed_at(
	const struct kwl_touchpad *pad,
	uint64_t now_ms)
{
	uint64_t base_ms;
	uint64_t span;
	int64_t base_um;
	unsigned found;
	unsigned age;
	unsigned slot;

	/* No report kept: no speed. */
	if (pad->gesture_sample_count == 0U)
		return 0;

	/* From the newest back, the first old enough; else the oldest. */
	base_ms = 0;
	base_um = 0;
	found = 0;
	for (age = 1; age <= pad->gesture_sample_count && !found; age++) {
		slot = (pad->gesture_sample_next + KWL_TOUCHPAD_SAMPLES - age) % KWL_TOUCHPAD_SAMPLES;
		base_ms = pad->gesture_sample_ms[slot];
		base_um = pad->gesture_sample_um[slot];
		if (now_ms >= base_ms && now_ms - base_ms >= SPEED_WINDOW_MS)
			found = 1;
	}

	/* The travel since over the time since, at least SPEED_SPAN_MS. */
	span = SPEED_SPAN_MS;
	if (now_ms >= base_ms && now_ms - base_ms > span)
		span = now_ms - base_ms;

	/* Succeeded: the speed. */
	return (pad->gesture_travel_um - base_um) * 1000 / (int64_t)span;
}

/* Gives a motion along a gesture's way: up for BOTTOM2 and UP3, right for LEFT2, left for RIGHT2, down for TOP2. */
static int64_t
gesture_along(
	uint32_t gesture,
	int64_t dx_um,
	int64_t dy_um)
{
	/* Each gesture's way. */
	switch (gesture) {
	case KWL_TOUCHPAD_GESTURE_LEFT2:
		return dx_um;
	case KWL_TOUCHPAD_GESTURE_RIGHT2:
		return -dx_um;
	case KWL_TOUCHPAD_GESTURE_TOP2:
		return dy_um;
	default:
		return -dy_um;
	}
}

/* Ends a tap's wait for its drag (its click is complete already, so no button moves). */
static void
tap_finish(
	struct kwl_touchpad *pad)
{
	/* Only a tap waiting for its drag is ended here. */
	if (pad->tap != KWL_TOUCHPAD_TAP_PENDING)
		return;

	/* No drag may follow the tap any more. */
	pad->tap = KWL_TOUCHPAD_TAP_NONE;
}

/*
 * Begins a tap drag: the left button pressed where the tap was, then the
 * motion the touch made before it was a drag.
 */
static void
tap_drag_begin(
	struct kwl_touchpad *pad,
	struct kwl_touchpad_actions *actions)
{
	/* The press, held until the finger lifts (touch_end). */
	push_button(actions, KWL_TOUCHPAD_BUTTON_LEFT, 1U);
	pad->tap = KWL_TOUCHPAD_TAP_DRAG;

	/* The motion held back, now the drag's. */
	if (pad->tap_held_x != 0 || pad->tap_held_y != 0)
		push_motion(actions, (int32_t)pad->tap_held_x, (int32_t)pad->tap_held_y);
	pad->tap_held_x = 0;
	pad->tap_held_y = 0;
}

/* Adds a button's press or release to the actions (a full list keeps the first ones). */
static void
push_button(
	struct kwl_touchpad_actions *actions,
	uint32_t button,
	uint32_t pressed)
{
	struct kwl_touchpad_action *action;

	/* A full list takes no more. */
	if (actions->count >= KWL_TOUCHPAD_ACTIONS)
		return;

	/* The button. */
	action = &actions->actions[actions->count];
	memset(action, 0, sizeof(*action));
	action->kind = KWL_TOUCHPAD_BUTTON;
	action->button = button;
	action->pressed = pressed;
	actions->count++;
}

/* Adds a motion to the actions. */
static void
push_motion(
	struct kwl_touchpad_actions *actions,
	int32_t dx,
	int32_t dy)
{
	struct kwl_touchpad_action *action;

	/* A full list takes no more. */
	if (actions->count >= KWL_TOUCHPAD_ACTIONS)
		return;

	/* The motion. */
	action = &actions->actions[actions->count];
	memset(action, 0, sizeof(*action));
	action->kind = KWL_TOUCHPAD_MOTION;
	action->dx = dx;
	action->dy = dy;
	actions->count++;
}

/* Adds scrolling to the actions. */
static void
push_scroll(
	struct kwl_touchpad_actions *actions,
	int32_t vertical,
	int32_t horizontal,
	int32_t vertical_units,
	int32_t horizontal_units)
{
	struct kwl_touchpad_action *action;

	/* A full list takes no more. */
	if (actions->count >= KWL_TOUCHPAD_ACTIONS)
		return;

	/* The scrolling. */
	action = &actions->actions[actions->count];
	memset(action, 0, sizeof(*action));
	action->kind = KWL_TOUCHPAD_SCROLL;
	action->vertical = vertical;
	action->horizontal = horizontal;
	action->vertical_units = vertical_units;
	action->horizontal_units = horizontal_units;
	actions->count++;
}

/* Tells, once, that a touch that scrolled has lifted: SWIPE2's end, after its scrolling. */
static void
swipe_finish(
	struct kwl_touchpad *pad,
	struct kwl_touchpad_actions *actions)
{
	/* A touch that never scrolled says nothing. */
	if (!pad->scrolled)
		return;

	/* The swipe ends; the next scroll of this touch (fingers landing again) is another. */
	pad->scrolled = 0U;
	push_gesture(actions, KWL_TOUCHPAD_GESTURE_SWIPE2, KWL_TOUCHPAD_PHASE_END, 0, 0);
}

/* Adds a gesture's phase to the actions. */
static void
push_gesture(
	struct kwl_touchpad_actions *actions,
	uint32_t gesture,
	uint32_t phase,
	int64_t travel_um,
	int64_t speed)
{
	struct kwl_touchpad_action *action;

	/* A full list takes no more. */
	if (actions->count >= KWL_TOUCHPAD_ACTIONS)
		return;

	/* The gesture. */
	action = &actions->actions[actions->count];
	memset(action, 0, sizeof(*action));
	action->kind = KWL_TOUCHPAD_GESTURE;
	action->gesture = gesture;
	action->phase = phase;
	action->travel_um = (int32_t)travel_um;
	action->speed = (int32_t)speed;
	actions->count++;
}

/* Gives a value without its sign. */
static int64_t
magnitude(
	int64_t value)
{
	/* A negative value turned round. */
	if (value < 0)
		return -value;

	/* Succeeded: the value is not negative. */
	return value;
}
