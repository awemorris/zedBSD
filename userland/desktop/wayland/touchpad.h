/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch pad layer (touchpad.c, ws159-p004): turns a touch pad's
 * multitouch reports into a pointer's motion, buttons and scrolling.
 *
 * It knows nothing of the seat: the caller hands it each event of a report
 * and the report's end, and applies the actions it gives back (input.c).
 * So the host tests run it alone.
 */

#ifndef KWL_TOUCHPAD_H
#define KWL_TOUCHPAD_H

#include <stdint.h>

/* The fingers a touch pad's reports address (its slots), and the actions one call may give. */
#define KWL_TOUCHPAD_SLOTS	10U
#define KWL_TOUCHPAD_ACTIONS	16U

/* The fingers' travel of one wheel notch when two fingers scroll (micrometres; the shell turns notches back into travel). */
#define KWL_TOUCHPAD_NOTCH_UM	2500

/* The wheel's units in one notch (the seat's WHEEL_STEP): a scroll's finer measure divides a notch into these. */
#define KWL_TOUCHPAD_NOTCH_UNITS	15

/* The evdev codes of the left, right and middle buttons (the same on every OS). */
#define KWL_TOUCHPAD_BUTTON_LEFT	0x110U
#define KWL_TOUCHPAD_BUTTON_RIGHT	0x111U
#define KWL_TOUCHPAD_BUTTON_MIDDLE	0x112U

/* What an action asks of the seat. */
enum kwl_touchpad_action_kind {
	KWL_TOUCHPAD_MOTION,
	KWL_TOUCHPAD_BUTTON,
	KWL_TOUCHPAD_SCROLL,
	KWL_TOUCHPAD_GESTURE
};

/*
 * The gestures (ws142-p003, plan/ws142/phase001 D): two fingers from the
 * bottom edge up, three fingers up anywhere (both open Wiseview), two
 * fingers from the left edge to the right and from the right edge to the
 * left (the desktop on that side), and a tap of three fingers (the
 * application switcher, D1).  Two fingers from the top edge down (TOP2,
 * ws142-p009: App Home, ws181-p008; a fullscreen window is docked,
 * BUG-228).  An edge's gesture needs one of the two fingers in its band
 * (ws181-p008).  SWIPE2 is no gesture of its own: its end says that a touch of
 * two fingers that scrolled has lifted, so that one swipe can be one step
 * of Wiseview or the switcher (ws142-p009, BUG-215 and BUG-216).
 */
enum kwl_touchpad_gesture {
	KWL_TOUCHPAD_GESTURE_NONE,
	KWL_TOUCHPAD_GESTURE_BOTTOM2,
	KWL_TOUCHPAD_GESTURE_UP3,
	KWL_TOUCHPAD_GESTURE_LEFT2,
	KWL_TOUCHPAD_GESTURE_RIGHT2,
	KWL_TOUCHPAD_GESTURE_TAP3,
	KWL_TOUCHPAD_GESTURE_TOP2,
	KWL_TOUCHPAD_GESTURE_SWIPE2
};

/* A gesture's phases: it begins, follows the fingers, ends with them lifted, or is given up (a finger more). */
enum kwl_touchpad_phase {
	KWL_TOUCHPAD_PHASE_BEGIN,
	KWL_TOUCHPAD_PHASE_UPDATE,
	KWL_TOUCHPAD_PHASE_END,
	KWL_TOUCHPAD_PHASE_CANCEL
};

/*
 * One action: a relative motion in output pixels (dx, dy), a button's
 * press or release (button, pressed), or scrolling in wheel notches
 * (vertical positive down, horizontal positive right, as the seat takes
 * them).  It lives in the caller's list.
 */
struct kwl_touchpad_action {
	enum kwl_touchpad_action_kind kind;
	int32_t dx;
	int32_t dy;
	uint32_t button;
	uint32_t pressed;
	int32_t vertical;
	int32_t horizontal;
	/*
	 * A scroll's finer measure (BUG-218): the wheel's units, KWL_TOUCHPAD_NOTCH_UNITS a notch, so
	 * that a client hears the fingers from their first fraction of a millimetre, smoothly.
	 */
	int32_t vertical_units;
	int32_t horizontal_units;
	/* A gesture: which, its phase, the fingers' travel along its way (micrometres, inward from the edge or up) and their speed (micrometres a second). */
	uint32_t gesture;
	uint32_t phase;
	int32_t travel_um;
	int32_t speed;
};

/* The actions of one call, in order; the caller owns the storage. */
struct kwl_touchpad_actions {
	unsigned count;
	struct kwl_touchpad_action actions[KWL_TOUCHPAD_ACTIONS];
};

/*
 * One finger as the reports left it: its tracking identifier (-1 for no
 * finger), its place, its place at the last report's end, whether it came
 * in this report, and where and when it landed (the edges' gestures judge
 * a touch by where its fingers landed, BUG-254).
 */
struct kwl_touchpad_finger {
	int32_t tracking;
	int32_t x;
	int32_t y;
	int32_t last_x;
	int32_t last_y;
	uint32_t fresh;
	int32_t start_x;
	int32_t start_y;
	uint64_t start_ms;
};

/*
 * The states of the tap: none; a tap's click given at its lift, the time a
 * drag may start not yet over (no button held); a finger down within that
 * time, not yet a drag or a second tap (no button held, its motion held
 * back); and a tap drag, whose button is held until the finger lifts
 * (ws183-p002).
 */
enum kwl_touchpad_tap {
	KWL_TOUCHPAD_TAP_NONE,
	KWL_TOUCHPAD_TAP_PENDING,
	KWL_TOUCHPAD_TAP_SECOND,
	KWL_TOUCHPAD_TAP_DRAG
};

/*
 * One touch pad's state: its fingers and the slot the reports address, its
 * resolution in units per millimetre, the physical button as the reports
 * say and the button that press was given as, the tap, the touch now on
 * the pad (when it began, the most fingers, how far it went), and the
 * remainders of the motion and the scrolling that did not make a whole
 * pixel or notch; the user's feel (ws089-p024): the acceleration's level
 * (KWL_ACCEL_* of pointer-accel.h, which chooses the gain's curve) and
 * whether the scrolling follows the fingers.
 *
 * It lives in its input device from attach (kwl_touchpad_init) to detach.
 */
/* The latest reports of a gesture whose travel and times its speed is measured over. */
#define KWL_TOUCHPAD_SAMPLES	16U

struct kwl_touchpad {
	struct kwl_touchpad_finger fingers[KWL_TOUCHPAD_SLOTS];
	int32_t slot;
	int32_t resolution_x;
	int32_t resolution_y;
	uint32_t button_down;
	uint32_t button_changed;
	uint32_t button_sent;
	uint32_t press_quiet;
	enum kwl_touchpad_tap tap;
	uint64_t tap_deadline_ms;
	/* The pointer's motion (pixels) of a touch after a tap, held back until it is a drag (ws183-p002). */
	int64_t tap_held_x;
	int64_t tap_held_y;
	uint64_t touch_start_ms;
	uint32_t touch_fingers;
	uint32_t touch_clicked;
	int64_t touch_travel_um;
	uint32_t fingers_before;
	int64_t motion_remainder_x;
	int64_t motion_remainder_y;
	int64_t scroll_travel_x_um;
	int64_t scroll_travel_y_um;
	/* The travel not yet told as the wheel's units, in micrometres times KWL_TOUCHPAD_NOTCH_UNITS (exact). */
	int64_t scroll_units_x;
	int64_t scroll_units_y;
	/* Whether the touch now on the pad scrolled (its end is told as SWIPE2's, ws142-p009). */
	uint32_t scrolled;
	uint64_t last_frame_ms;
	int32_t natural_scroll;
	int32_t acceleration;
	/*
	 * The gestures (ws142-p003): the pad's least and largest places in
	 * units (the largest 0 while they are not known: no edge then), the edges a finger of a two-finger touch
	 * started in (EDGE_* bits), whether the touch is decided (a scroll, or
	 * none, or a gesture), the gesture under way and its fingers, the
	 * fingers' mean travel since the decision began (micrometres), the
	 * travel along its way, and its speed; the travel at its latest
	 * reports with their times, which the speed is measured over.
	 */
	int32_t x_min;
	int32_t y_min;
	int32_t x_max;
	int32_t y_max;
	uint32_t edges;
	uint32_t decided;
	uint32_t gesture;
	uint32_t gesture_fingers;
	int64_t gesture_dx_um;
	int64_t gesture_dy_um;
	int64_t gesture_travel_um;
	int64_t gesture_speed;
	uint64_t gesture_sample_ms[KWL_TOUCHPAD_SAMPLES];
	int64_t gesture_sample_um[KWL_TOUCHPAD_SAMPLES];
	unsigned gesture_sample_next;
	unsigned gesture_sample_count;
};

void kwl_touchpad_init(struct kwl_touchpad *pad, int32_t resolution_x, int32_t resolution_y);
void kwl_touchpad_set_size(struct kwl_touchpad *pad, int32_t x_max, int32_t y_max);
void kwl_touchpad_set_range(struct kwl_touchpad *pad, int32_t x_min, int32_t x_max, int32_t y_min, int32_t y_max);
void kwl_touchpad_set_feel(struct kwl_touchpad *pad, int32_t acceleration, int32_t natural);
void kwl_touchpad_event(struct kwl_touchpad *pad, uint16_t type, uint16_t code, int32_t value);
void kwl_touchpad_frame(struct kwl_touchpad *pad, uint64_t now_ms, struct kwl_touchpad_actions *actions);
void kwl_touchpad_tick(struct kwl_touchpad *pad, uint64_t now_ms, struct kwl_touchpad_actions *actions);
void kwl_touchpad_release_all(struct kwl_touchpad *pad, struct kwl_touchpad_actions *actions);

#endif
