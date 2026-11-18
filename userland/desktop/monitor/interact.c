/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's input (design.md sections 3.9 and 3.10): the
 * fingers, the pointer and the keys.
 *
 *   tap / click             a plate comes forward as a card; outside the
 *                           card, it goes back (unless pinned)
 *   long press / right click   pins the card (it stays), or unpins it
 *   swipe (one finger)      across a plate: the time range, longer to the
 *                           left, shorter to the right
 *   pinch                   apart: the plate under the fingers as a card
 *                           (detail); together: no card (overview)
 *   two-finger tap          detail or overview, in turn
 *   drag on the core        turns it a little; it comes back when let go
 *   Tab, Shift+Tab          the keyboard's plate, the next or the one before
 *   Enter, Space            the keyboard's plate as a card, or the card back
 *   Esc                     the overview (the card back even when pinned)
 *   P                       pins or unpins the card
 *   Left, Right, Shift+wheel   the time range
 *
 * Taps, long presses and drags are libkeiland's gestures (keiland.h); the
 * two-finger tap is told here, since the gestures give no tap for two
 * fingers.  Every action is logged (ZMON CARD, VIEW, RANGE, FOCUS, CORE)
 * for the tests.
 */

#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The evdev codes of the keys. */
#define KEY_ESC			1U
#define KEY_TAB			15U
#define KEY_P			25U
#define KEY_ENTER		28U
#define KEY_SPACE		57U
#define KEY_LEFT		105U
#define KEY_RIGHT		106U

/* How long a card takes to come out or go back, in milliseconds. */
#define CARD_MS			360.0f

/* The card's size when it is out, as shares of the window's width and height (design.md section 3.9). */
#define CARD_WIDTH		0.72f
#define CARD_HEIGHT		0.64f

/* A swipe: this fast (px/s) or this far (px), and at least twice as much sideways as up or down. */
#define SWIPE_SPEED		400.0f
#define SWIPE_DISTANCE		120.0f

/* A long press with the pointer, and how far a press may move to stay a click, in milliseconds and pixels. */
#define PRESS_LONG_MS		500U
#define PRESS_SLOP		8.0f

/* A two-finger tap: the second finger down within 250 ms of the first, and all up within 300 ms of it, in microseconds. */
#define TWO_DOWN_US		250000U
#define TWO_TAP_US		300000U

/* A pinch acts past these ratios of the fingers' distance. */
#define PINCH_OUT		1.25
#define PINCH_IN		0.8

/* How far the core turns, at most, in radians, and per pixel dragged. */
#define CORE_TURN_MAX		0.44f
#define CORE_TURN_PER_PIXEL	0.004f

/* How near the pointer the camera's tilt, and how slow its speed, count as settled (the pointer's -1..1 units, and a second). */
#define TILT_SETTLED		0.002f

/* The plates' names in the log, in the order of enum sm_plate. */
static const char *const plate_names[SM_PLATES] = {
	"cpu", "gpu", "memory", "network", "disk", "cores", "state", "graphics", "flow", "strata", "lanes", "latency", "events"
};

/* The keyboard's order through the plates: the summary row, the middle row, the bottom row (the latency's plate is the disks'). */
static const enum sm_plate keyboard_order[] = {
	SM_PLATE_CPU, SM_PLATE_GPU, SM_PLATE_MEMORY, SM_PLATE_NETWORK, SM_PLATE_DISK, SM_PLATE_CORES, SM_PLATE_STATE,
	SM_PLATE_GRAPHICS, SM_PLATE_FLOW, SM_PLATE_STRATA, SM_PLATE_LANES, SM_PLATE_EVENTS
};

static void card_open(struct sm_app *app, int plate, const char *how);
static void card_close(struct sm_app *app, const char *how);
static void card_pin(struct sm_app *app);
static int card_out(const struct sm_app *app);
static void act_tap(struct sm_app *app, float x, float y, const char *how);
static void act_long_press(struct sm_app *app, float x, float y);
static void act_swipe(struct sm_app *app, float dx);
static void view_detail(struct sm_app *app, int plate, const char *how);
static void view_overview(struct sm_app *app, const char *how);
static int hit_plate(const struct sm_app *app, float x, float y);
static int in_box(const struct sm_box *box, float x, float y);
static int on_core(const struct sm_app *app, float x, float y);
static float clamp_turn(float dx);
static int finger_slot(const struct sm_touch *touch, int32_t id);
static void finger_down(struct sm_app *app, const struct kl_window_event *event);
static void finger_up(struct sm_app *app, const struct kl_window_event *event, int slot);
static void touch_event(struct sm_app *app, const struct kl_window_event *event);
static void pointer_event(struct sm_app *app, const struct kl_window_event *event);
static void button_event(struct sm_app *app, const struct kl_window_event *event);
static void key_event(struct sm_app *app, const struct kl_window_event *event);
static void key_tab(struct sm_app *app, int backwards);
static void gesture_events(struct sm_app *app, uint64_t now_us);
static void drag_end(struct sm_app *app, const struct kl_gesture_event *gesture);
static void pinch_tick(struct sm_app *app, uint64_t now_us);
static int card_tick(struct sm_app *app, uint64_t now_ms);
static int tilt_settling(const struct sm_app *app);

/*
 * Makes the gestures of the window's fingers; returns 0, or -1 when
 * memory is short.
 */
int
sm_interact_open(
	struct sm_app *app)
{
	/* No card and no keyboard plate yet. */
	app->focus.plate = -1;
	app->focus.keyboard = -1;

	/* The gestures of the window's fingers. */
	app->touch.gesture = kl_gesture_create();
	if (app->touch.gesture == NULL)
		return -1;

	/* Succeeded: the input can be taken. */
	return 0;
}

/*
 * Releases the gestures.
 */
void
sm_interact_close(
	struct sm_app *app)
{
	/* The gestures, when they were made. */
	if (app->touch.gesture != NULL)
		kl_gesture_destroy(app->touch.gesture);
	app->touch.gesture = NULL;
}

/*
 * Takes one input of the window: a finger, the pointer or a key.
 */
void
sm_interact_event(
	struct sm_app *app,
	const struct kl_window_event *event)
{
	/* Each kind of input to its own reader. */
	switch (event->kind) {
	case KL_WINDOW_TOUCH_DOWN:
	case KL_WINDOW_TOUCH_MOTION:
	case KL_WINDOW_TOUCH_UP:
	case KL_WINDOW_TOUCH_CANCEL:
		touch_event(app, event);
		break;
	case KL_WINDOW_MOTION:
	case KL_WINDOW_LEAVE:
	case KL_WINDOW_AXIS:
		pointer_event(app, event);
		break;
	case KL_WINDOW_BUTTON:
		button_event(app, event);
		break;
	case KL_WINDOW_KEY:
		key_event(app, event);
		break;
	default:
		break;
	}
}

/*
 * Keeps the input's time: the fingers' gestures (a long press comes by
 * the clock), a pinch, the core's drag, the pointer's long press, and the
 * card's coming and going.
 *
 * Returns 1 while the input needs the clock and frames: something moves,
 * or a finger or the pointer's button is down (a long press is found by
 * the clock, also when the monitor's own clock is stopped for a test).
 */
int
sm_interact_tick(
	struct sm_app *app,
	uint64_t now_ms)
{
	struct sm_touch *touch;
	uint64_t now_us;
	double dx;
	double dy;
	int error;
	int active;
	int moving;
	int settling;
	int held;

	/* The gestures due, then a pinch of two fingers. */
	touch = &app->touch;
	now_us = kl_clock_us();
	gesture_events(app, now_us);
	pinch_tick(app, now_us);
	active = 0;

	/* A finger dragging the core turns it. */
	if (touch->dragging && touch->drag_core) {
		error = kl_gesture_drag_offset(touch->gesture, now_us, &dx, &dy);
		if (error == 0)
			touch->core_turn = clamp_turn((float)dx);
		active = 1;
	}

	/* The pointer held still on a plate is a long press, once (by the real clock). */
	if (touch->pressed &&
	    !touch->press_moved &&
	    !touch->press_long) {
		if (now_us / 1000U >= touch->press_ms + PRESS_LONG_MS) {
			touch->press_long = 1;
			act_long_press(app, touch->press_x, touch->press_y);
		}
	}

	/* Whether anything holds the core. */
	held = 0;
	if (touch->dragging && touch->drag_core)
		held = 1;
	else if (touch->pressed && touch->press_core)
		held = 1;

	/* The core comes back when nothing holds it. */
	if (!held && touch->core_turn != 0.0f) {
		touch->core_turn *= 0.85f;
		if (touch->core_turn > -0.002f && touch->core_turn < 0.002f)
			touch->core_turn = 0.0f;
		active = 1;
	}

	/* The card coming out or going back. */
	moving = card_tick(app, now_ms);
	if (moving)
		active = 1;

	/*
	 * The camera still turning towards the pointer: it is the hover's
	 * answer, so it keeps the input's frame rate rather than the
	 * animation's two frames a second.
	 */
	settling = tilt_settling(app);
	if (settling)
		active = 1;

	/* A finger or the button down keeps the clock going. */
	if (touch->fingers != 0U)
		active = 1;
	else if (touch->pressed)
		active = 1;

	/* Succeeded: whether the input needs frames. */
	return active;
}

/*
 * Names a plate for the log.
 */
const char *
sm_plate_name(
	enum sm_plate plate)
{
	/* The table's name. */
	return plate_names[plate];
}

/*
 * Gives the card's box at a stage of its coming out (eased, 0 to 1): from
 * its plate's place to the middle of the window, 72% of its width and 64%
 * of its height (design.md section 3.9).  No card gives an empty box.
 */
void
sm_card_box(
	const struct sm_app *app,
	float eased,
	struct sm_box *box)
{
	const struct sm_box *plate;
	struct sm_box target;

	/* No card: nothing. */
	memset(box, 0, sizeof(*box));
	if (app->focus.plate < 0)
		return;

	/* The card's place when it is out. */
	target.width = app->layout.width * CARD_WIDTH;
	target.height = app->layout.height * CARD_HEIGHT;
	target.x = (app->layout.width - target.width) * 0.5f;
	target.y = (app->layout.height - target.height) * 0.5f;

	/* Between the plate and that place. */
	plate = &app->view.plates[app->focus.plate];
	box->x = plate->x + (target.x - plate->x) * eased;
	box->y = plate->y + (target.y - plate->y) * eased;
	box->width = plate->width + (target.width - plate->width) * eased;
	box->height = plate->height + (target.height - plate->height) * eased;
}

/* Brings a plate forward as a card (or keeps the one that is out), logging how. */
static void
card_open(
	struct sm_app *app,
	int plate,
	const char *how)
{
	/* Another plate's card starts from its own plate, unpinned. */
	if (app->focus.plate != plate) {
		app->focus.progress = 0.0f;
		app->focus.pinned = 0;
	}

	/* The card comes out; the tests read the line. */
	app->focus.plate = plate;
	app->focus.opening = 1;
	app->dirty = 1;
	printf("ZMON CARD open plate=%s via=%s\n", plate_names[plate], how);
}

/* Sends the card back, unpinned, logging how. */
static void
card_close(
	struct sm_app *app,
	const char *how)
{
	int out;

	/* Nothing is out. */
	out = card_out(app);
	if (!out)
		return;

	/* The card goes back; the tests read the line. */
	app->focus.opening = 0;
	app->focus.pinned = 0;
	app->dirty = 1;
	printf("ZMON CARD close plate=%s via=%s\n", plate_names[app->focus.plate], how);
}

/* Pins the card that is out, or unpins it. */
static void
card_pin(
	struct sm_app *app)
{
	int out;

	/* Only a card that is out. */
	out = card_out(app);
	if (!out)
		return;

	/* A pinned card stays when the fingers or the pointer tap outside it. */
	if (app->focus.pinned)
		app->focus.pinned = 0;
	else
		app->focus.pinned = 1;
	app->dirty = 1;
	printf("ZMON CARD pin=%d plate=%s\n", app->focus.pinned, plate_names[app->focus.plate]);
}

/* Whether a card is out or coming out (not going back). */
static int
card_out(
	const struct sm_app *app)
{
	/* No plate. */
	if (app->focus.plate < 0)
		return 0;

	/* Going back. */
	if (!app->focus.opening)
		return 0;

	/* Succeeded: a card is out. */
	return 1;
}

/* A tap or a click at a point: a plate forward, or the card back when outside it. */
static void
act_tap(
	struct sm_app *app,
	float x,
	float y,
	const char *how)
{
	struct sm_box box;
	int out;
	int inside;
	int plate;

	/* With a card out: a tap on it keeps it, one outside sends it back unless pinned. */
	out = card_out(app);
	if (out) {
		sm_card_box(app, 1.0f, &box);
		inside = in_box(&box, x, y);
		if (inside)
			return;
		if (app->focus.pinned)
			return;
		card_close(app, how);
		return;
	}

	/* Otherwise the plate there comes forward. */
	plate = hit_plate(app, x, y);
	if (plate >= 0)
		card_open(app, plate, how);
}

/* A long press: pins the card that is out, or brings the plate there forward pinned. */
static void
act_long_press(
	struct sm_app *app,
	float x,
	float y)
{
	int out;
	int plate;

	/* A card out: pinned or unpinned. */
	out = card_out(app);
	if (out) {
		card_pin(app);
		return;
	}

	/* Otherwise the plate there, forward and pinned. */
	plate = hit_plate(app, x, y);
	if (plate < 0)
		return;
	card_open(app, plate, "long-press");
	card_pin(app);
}

/* A sideways swipe: a longer range to the left, a shorter one to the right. */
static void
act_swipe(
	struct sm_app *app,
	float dx)
{
	/* Leftwards: a longer range, when there is one. */
	if (dx < 0.0f) {
		if (app->range + 1U < SM_RANGES)
			sm_set_range(app, app->range + 1U);
		return;
	}

	/* Rightwards: a shorter range, when there is one. */
	if (app->range > 0U)
		sm_set_range(app, app->range - 1U);
}

/* The detail: a plate as a card (the state's when none is given). */
static void
view_detail(
	struct sm_app *app,
	int plate,
	const char *how)
{
	/* The state's plate stands for the whole when no plate is under the fingers. */
	if (plate < 0)
		plate = SM_PLATE_STATE;

	/* The detail's line, then the card. */
	printf("ZMON VIEW detail plate=%s via=%s\n", plate_names[plate], how);
	card_open(app, plate, how);
}

/* The overview: no card, pinned or not. */
static void
view_overview(
	struct sm_app *app,
	const char *how)
{
	/* The overview's line, then the card back (a pin does not hold it). */
	printf("ZMON VIEW overview via=%s\n", how);
	app->focus.pinned = 0;
	card_close(app, how);
}

/* The plate at a point (the latency's in front of the disks'), or -1. */
static int
hit_plate(
	const struct sm_app *app,
	float x,
	float y)
{
	int plate;
	int inside;

	/* From the front-most down: the later plates are drawn over the earlier. */
	for (plate = SM_PLATES - 1; plate >= 0; plate--) {
		inside = in_box(&app->view.plates[plate], x, y);
		if (inside)
			return plate;
	}

	/* No plate there. */
	return -1;
}

/* Whether a point is in a box. */
static int
in_box(
	const struct sm_box *box,
	float x,
	float y)
{
	/* Left of it or right of it. */
	if (x < box->x)
		return 0;
	if (x >= box->x + box->width)
		return 0;

	/* Above it or below it. */
	if (y < box->y)
		return 0;
	if (y >= box->y + box->height)
		return 0;

	/* Succeeded: inside. */
	return 1;
}

/* Whether a point is on the state's core (the middle of its plate, between its title and its words). */
static int
on_core(
	const struct sm_app *app,
	float x,
	float y)
{
	const struct sm_box *box;

	/* Beside the core. */
	box = &app->view.plates[SM_PLATE_STATE];
	if (x < box->x + box->width * 0.2f)
		return 0;
	if (x > box->x + box->width * 0.8f)
		return 0;

	/* Above it (the title) or below it (the words). */
	if (y < box->y + box->height * 0.15f)
		return 0;
	if (y > box->y + box->height * 0.75f)
		return 0;

	/* Succeeded: on the core. */
	return 1;
}

/* The core's turn for a drag of dx pixels, within its bounds. */
static float
clamp_turn(
	float dx)
{
	float turn;

	/* A little a pixel, no further than the bound either way. */
	turn = dx * CORE_TURN_PER_PIXEL;
	if (turn > CORE_TURN_MAX)
		return CORE_TURN_MAX;
	if (turn < -CORE_TURN_MAX)
		return -CORE_TURN_MAX;

	/* Succeeded: the turn. */
	return turn;
}

/* A finger's slot by its id, or -1 (a slot in use holds its id plus one, so zero is free). */
static int
finger_slot(
	const struct sm_touch *touch,
	int32_t id)
{
	unsigned slot;

	/* The slots in use. */
	for (slot = 0; slot < SM_FINGERS; slot++) {
		if (touch->ids[slot] == id + 1)
			return (int)slot;
	}

	/* The finger is not followed. */
	return -1;
}

/* A finger touches: its slot and place, a new contact's records, and the second finger's time and centroid. */
static void
finger_down(
	struct sm_app *app,
	const struct kl_window_event *event)
{
	struct sm_touch *touch;
	unsigned slot;

	/* The first free slot; a sixth finger is not followed. */
	touch = &app->touch;
	for (slot = 0; slot < SM_FINGERS; slot++) {
		if (touch->ids[slot] == 0)
			break;
	}

	/* No slot left. */
	if (slot == SM_FINGERS)
		return;

	/*
	 * The first finger of a contact starts its records again: the places of
	 * the last contact's fingers would otherwise count as movement.
	 */
	if (touch->fingers == 0U) {
		memset(touch->down_x, 0, sizeof(touch->down_x));
		memset(touch->down_y, 0, sizeof(touch->down_y));
		memset(touch->last_x, 0, sizeof(touch->last_x));
		memset(touch->last_y, 0, sizeof(touch->last_y));
		touch->most = 0;
		touch->moved = 0;
		touch->pinched = 0;
		touch->first_us = event->time_us;
	}

	/* The finger's slot and where it touched. */
	touch->ids[slot] = event->id + 1;
	touch->down_x[slot] = (float)event->x;
	touch->down_y[slot] = (float)event->y;
	touch->last_x[slot] = (float)event->x;
	touch->last_y[slot] = (float)event->y;

	/* The fingers down now and the most in this contact. */
	touch->fingers++;
	if (touch->fingers > touch->most)
		touch->most = touch->fingers;

	/* Only the second finger is timed and placed. */
	if (touch->fingers != 2U)
		return;

	/* When it came, and the two fingers' middle (where a two-finger tap acts). */
	touch->second_us = event->time_us;
	touch->two_x = 0.0f;
	touch->two_y = 0.0f;
	for (slot = 0; slot < SM_FINGERS; slot++) {
		if (touch->ids[slot] == 0)
			continue;
		touch->two_x += touch->down_x[slot] * 0.5f;
		touch->two_y += touch->down_y[slot] * 0.5f;
	}
}

/*
 * A finger lifts: its slot free, a lone finger's movement kept for the
 * swipe, and the last of two still fingers that touched together and
 * lifted soon is a two-finger tap (design.md section 3.10).
 */
static void
finger_up(
	struct sm_app *app,
	const struct kl_window_event *event,
	int slot)
{
	struct sm_touch *touch;
	int out;
	int plate;

	/* The slot is free; a lone finger's movement is the swipe's. */
	touch = &app->touch;
	touch->ids[slot] = 0;
	if (touch->fingers > 0U)
		touch->fingers--;
	if (touch->most == 1U) {
		touch->lift_dx = touch->last_x[slot] - touch->down_x[slot];
		touch->lift_dy = touch->last_y[slot] - touch->down_y[slot];
	}

	/* Fingers still down, or more or fewer than two in this contact. */
	if (touch->fingers != 0U)
		return;
	if (touch->most != 2U)
		return;

	/* Moved, pinched, or not together: no tap. */
	if (touch->moved)
		return;
	if (touch->pinched)
		return;
	if (touch->second_us > touch->first_us + TWO_DOWN_US)
		return;
	if (event->time_us > touch->second_us + TWO_TAP_US)
		return;

	/* A two-finger tap: the detail of the plate under the fingers, or the overview when a card is out. */
	out = card_out(app);
	if (out) {
		view_overview(app, "two-finger-tap");
		return;
	}

	/* The detail of the plate under the fingers' middle. */
	plate = hit_plate(app, touch->two_x, touch->two_y);
	view_detail(app, plate, "two-finger-tap");
}

/* A finger's event: to the gestures, and into the records of the two-finger tap and the swipe. */
static void
touch_event(
	struct sm_app *app,
	const struct kl_window_event *event)
{
	struct sm_touch *touch;
	float dx;
	float dy;
	int slot;

	/* Each kind of finger event. */
	touch = &app->touch;
	slot = finger_slot(touch, event->id);
	switch (event->kind) {
	case KL_WINDOW_TOUCH_DOWN:
		/* A new finger. */
		(void)kl_gesture_down(touch->gesture, event->id, event->time_us, event->arrival_us, event->x, event->y);
		if (slot < 0)
			finger_down(app, event);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		/* A finger moving: past the slop it is no tap. */
		(void)kl_gesture_motion(touch->gesture, event->id, event->time_us, event->arrival_us, event->x, event->y);
		if (slot < 0)
			break;
		touch->last_x[slot] = (float)event->x;
		touch->last_y[slot] = (float)event->y;
		dx = (float)event->x - touch->down_x[slot];
		dy = (float)event->y - touch->down_y[slot];
		if (dx * dx + dy * dy > PRESS_SLOP * PRESS_SLOP)
			touch->moved = 1;
		break;
	case KL_WINDOW_TOUCH_UP:
		/* A finger lifting. */
		(void)kl_gesture_up(touch->gesture, event->id, event->time_us);
		if (slot >= 0)
			finger_up(app, event, slot);
		break;
	default:
		/* The compositor took the fingers: nothing they did acts. */
		kl_gesture_cancel(touch->gesture);
		memset(touch->ids, 0, sizeof(touch->ids));
		touch->fingers = 0;
		touch->dragging = 0;
		touch->drag_core = 0;
		break;
	}

	/* The gestures want the clock soon. */
	app->dirty = 1;
}

/* The pointer's place (the parallax, and a turn of the core while pressed on it) and the wheel. */
static void
pointer_event(
	struct sm_app *app,
	const struct kl_window_event *event)
{
	struct sm_touch *touch;
	float dx;
	float dy;

	/* The pointer leaving: the camera straight again. */
	touch = &app->touch;
	if (event->kind == KL_WINDOW_LEAVE) {
		app->motion.pointer_x = 0.0f;
		app->motion.pointer_y = 0.0f;
		return;
	}

	/* Shift and the wheel: the range (the wheel down is a swipe to the left, a longer range). */
	if (event->kind == KL_WINDOW_AXIS) {
		if ((event->modifiers & KL_MOD_SHIFT) == 0U)
			return;
		if (event->dy != 0.0)
			act_swipe(app, (float)-event->dy);
		return;
	}

	/* The pointer's place, from -1 to 1 across the window. */
	if (app->layout.width > 0.0f && app->layout.height > 0.0f) {
		app->motion.pointer_x = (float)event->x / app->layout.width * 2.0f - 1.0f;
		app->motion.pointer_y = (float)event->y / app->layout.height * 2.0f - 1.0f;
	}

	/* A press moving: past the slop it is no click, and on the core it turns it. */
	if (touch->pressed) {
		dx = (float)event->x - touch->press_x;
		dy = (float)event->y - touch->press_y;
		if (dx * dx + dy * dy > PRESS_SLOP * PRESS_SLOP)
			touch->press_moved = 1;
		if (touch->press_core)
			touch->core_turn = clamp_turn(dx);
	}

	/* The camera follows. */
	app->dirty = 1;
}

/* The pointer's buttons: a click, a long press, a drag on the core, and the right button's pin. */
static void
button_event(
	struct sm_app *app,
	const struct kl_window_event *event)
{
	struct sm_touch *touch;

	/* The right button pins (design.md section 3.10: the pointer's long press). */
	touch = &app->touch;
	if (event->code == KL_BUTTON_RIGHT) {
		if (event->pressed)
			act_long_press(app, (float)event->x, (float)event->y);
		return;
	}

	/* Only the left button otherwise. */
	if (event->code != KL_BUTTON_LEFT)
		return;

	/* Pressed: a press begins, timed by the real clock (on the core, a turn). */
	if (event->pressed) {
		touch->pressed = 1;
		touch->press_moved = 0;
		touch->press_long = 0;
		touch->press_x = (float)event->x;
		touch->press_y = (float)event->y;
		touch->press_ms = kl_clock_us() / 1000U;
		touch->press_core = on_core(app, (float)event->x, (float)event->y);
		app->dirty = 1;
		return;
	}

	/* A release without a press. */
	if (!touch->pressed)
		return;

	/* A click unless it moved or was a long press. */
	if (!touch->press_moved && !touch->press_long)
		act_tap(app, (float)event->x, (float)event->y, "click");

	/* A turn of the core ends; it comes back by itself. */
	if (touch->press_core && touch->press_moved)
		printf("ZMON CORE turn=%.2f via=pointer\n", touch->core_turn);
	touch->pressed = 0;
	touch->press_core = 0;
	app->dirty = 1;
}

/* A key: the keyboard's plate, its card, the overview, the pin, the range. */
static void
key_event(
	struct sm_app *app,
	const struct kl_window_event *event)
{
	int out;
	int backwards;

	/* Only presses (a held key's repeats too). */
	if (!event->pressed)
		return;

	/* Each key. */
	switch (event->code) {
	case KEY_TAB:
		/* The next plate, or with Shift the one before. */
		backwards = 0;
		if ((event->modifiers & KL_MOD_SHIFT) != 0U)
			backwards = 1;
		key_tab(app, backwards);
		break;
	case KEY_ENTER:
	case KEY_SPACE:
		/* The card back, or the keyboard's plate as a card. */
		out = card_out(app);
		if (out)
			card_close(app, "key");
		else if (app->focus.keyboard >= 0)
			card_open(app, app->focus.keyboard, "key");
		break;
	case KEY_ESC:
		/* The overview, even over a pinned card. */
		view_overview(app, "key");
		break;
	case KEY_P:
		/* The card pinned or unpinned. */
		card_pin(app);
		break;
	case KEY_LEFT:
		/* A shorter range. */
		if (app->range > 0U)
			sm_set_range(app, app->range - 1U);
		break;
	case KEY_RIGHT:
		/* A longer range. */
		if (app->range + 1U < SM_RANGES)
			sm_set_range(app, app->range + 1U);
		break;
	default:
		break;
	}
}

/* Moves the keyboard's plate to the next in its order, or the one before. */
static void
key_tab(
	struct sm_app *app,
	int backwards)
{
	unsigned count;
	unsigned index;

	/* Where the keyboard's plate is in the order (none: the end). */
	count = sizeof(keyboard_order) / sizeof(keyboard_order[0]);
	for (index = 0; index < count; index++) {
		if ((int)keyboard_order[index] == app->focus.keyboard)
			break;
	}

	/* The next, from none the first; or the one before, from none the last. */
	if (!backwards) {
		index++;
		if (index >= count)
			index = 0;
	} else {
		if (index == 0U || index >= count)
			index = count;
		index--;
	}

	/* The plate drawn with the keyboard's edge; the tests read the line. */
	app->focus.keyboard = (int)keyboard_order[index];
	app->dirty = 1;
	printf("ZMON FOCUS plate=%s\n", plate_names[app->focus.keyboard]);
}

/* Takes the gestures due: taps, long presses, the drags' starts and ends. */
static void
gesture_events(
	struct sm_app *app,
	uint64_t now_us)
{
	struct kl_gesture_event gesture;
	struct sm_touch *touch;
	int found;
	int plate;
	int core;

	/* Each gesture due. */
	touch = &app->touch;
	for (;;) {
		found = kl_gesture_next(touch->gesture, now_us, &gesture);
		if (!found)
			break;

		/* Each kind. */
		switch (gesture.kind) {
		case KL_GESTURE_TAP:
			act_tap(app, (float)gesture.x, (float)gesture.y, "tap");
			break;
		case KL_GESTURE_LONG_PRESS:
			act_long_press(app, (float)gesture.x, (float)gesture.y);
			break;
		case KL_GESTURE_DRAG_BEGIN:
			/* One finger on the core turns it; one finger on another plate may be a swipe. */
			touch->dragging = 1;
			touch->drag_core = 0;
			touch->swipe_ok = 0;
			if (gesture.fingers != 1U)
				break;
			core = on_core(app, (float)gesture.x, (float)gesture.y);
			plate = hit_plate(app, (float)gesture.x, (float)gesture.y);
			if (core)
				touch->drag_core = 1;
			else if (plate >= 0)
				touch->swipe_ok = 1;
			break;
		case KL_GESTURE_DRAG_END:
			drag_end(app, &gesture);
			break;
		default:
			/* A cancel: no drag goes on. */
			touch->dragging = 0;
			touch->drag_core = 0;
			touch->swipe_ok = 0;
			break;
		}
	}
}

/*
 * A drag ends: a turn of the core ends, or a lone finger's drag that began
 * on a plate is a swipe when it went fast or far and mostly sideways
 * (design.md section 3.10; a pinch's drag is never a swipe).
 */
static void
drag_end(
	struct sm_app *app,
	const struct kl_gesture_event *gesture)
{
	struct sm_touch *touch;
	float distance;
	float across;
	float speed;
	float speed_across;
	int swipe;

	/* A turn of the core ends; it comes back by itself. */
	touch = &app->touch;
	if (touch->drag_core)
		printf("ZMON CORE turn=%.2f via=touch\n", touch->core_turn);

	/* How far and how fast the finger went, sideways and up or down. */
	distance = fabsf(touch->lift_dx);
	across = fabsf(touch->lift_dy);
	speed = fabsf((float)gesture->vx);
	speed_across = fabsf((float)gesture->vy);

	/* A swipe: one finger all along, from a plate, far or fast, at least twice as much sideways. */
	swipe = 0;
	if (touch->most != 1U) {
		/* Two or more fingers: a pinch's or a pan's drag. */
		swipe = 0;
	} else if (!touch->swipe_ok) {
		/* Begun on the core or outside the plates. */
		swipe = 0;
	} else if (distance >= SWIPE_DISTANCE && distance > 2.0f * across) {
		/* Far and sideways. */
		swipe = 1;
	} else if (speed >= SWIPE_SPEED && speed > 2.0f * speed_across) {
		/* A short flick, fast and sideways. */
		swipe = 2;
	}

	/* The range, the way the finger went (its distance, or its speed when it flicked a short way). */
	if (swipe == 1)
		act_swipe(app, touch->lift_dx);
	else if (swipe == 2)
		act_swipe(app, (float)gesture->vx);

	/* No drag goes on. */
	touch->dragging = 0;
	touch->drag_core = 0;
	touch->swipe_ok = 0;
}

/* Two fingers: a pinch acts once in a contact, when their distance passes a ratio of where it began. */
static void
pinch_tick(
	struct sm_app *app,
	uint64_t now_us)
{
	struct sm_touch *touch;
	double scale;
	double x;
	double y;
	int error;
	int plate;

	/* Only two fingers, before the pinch acted. */
	touch = &app->touch;
	if (touch->fingers != 2U)
		return;
	if (touch->pinched)
		return;

	/* Their distance against the start. */
	error = kl_gesture_pinch(touch->gesture, now_us, &scale, &x, &y);
	if (error != 0)
		return;

	/* Apart: the detail of the plate under them; together: the overview. */
	if (scale >= PINCH_OUT) {
		touch->pinched = 1;
		plate = hit_plate(app, (float)x, (float)y);
		view_detail(app, plate, "pinch");
	} else if (scale <= PINCH_IN) {
		touch->pinched = 1;
		view_overview(app, "pinch");
	}
}

/*
 * Moves the card out or back over CARD_MS (at once with the clock
 * stopped); returns 1 while it moves.
 */
static int
card_tick(
	struct sm_app *app,
	uint64_t now_ms)
{
	struct sm_focus *focus;
	float seconds;
	float step;

	/* The time since the last tick (all of it with the clock stopped). */
	focus = &app->focus;
	seconds = 0.0f;
	if (focus->last_ms != 0U && now_ms > focus->last_ms)
		seconds = (float)(now_ms - focus->last_ms) / 1000.0f;
	focus->last_ms = now_ms;
	if (app->fixed_clock)
		seconds = 1.0f;
	step = seconds * 1000.0f / CARD_MS;

	/* Coming out. */
	if (focus->opening && focus->progress < 1.0f) {
		focus->progress = fminf(1.0f, focus->progress + step);
		return 1;
	}

	/* Going back; the plate is no card once it is home. */
	if (!focus->opening && focus->progress > 0.0f) {
		focus->progress = fmaxf(0.0f, focus->progress - step);
		if (focus->progress <= 0.0f)
			focus->plate = -1;
		return 1;
	}

	/* Succeeded: the card rests. */
	return 0;
}

/*
 * Tells whether the camera's tilt is still on its way to the pointer;
 * returns 1 while it moves.  A stopped clock puts it there at once, and a
 * hidden window draws nothing to move it.
 */
static int
tilt_settling(
	const struct sm_app *app)
{
	const struct sm_motion *motion;
	float distance_x;
	float distance_y;
	float speed_x;
	float speed_y;

	/* A stopped clock or a hidden window: nothing to draw for it. */
	if (app->fixed_clock)
		return 0;
	if (!app->visible)
		return 0;

	/* How far the tilt is from the pointer, and how fast it moves. */
	motion = &app->motion;
	distance_x = fabsf(motion->pointer_x - motion->tilt_x);
	distance_y = fabsf(motion->pointer_y - motion->tilt_y);
	speed_x = fabsf(motion->tilt_speed_x);
	speed_y = fabsf(motion->tilt_speed_y);

	/* Away from the pointer on either axis: still moving. */
	if (distance_x > TILT_SETTLED)
		return 1;
	if (distance_y > TILT_SETTLED)
		return 1;

	/* Still swinging through the pointer's place: still moving. */
	if (speed_x > TILT_SETTLED)
		return 1;
	if (speed_y > TILT_SETTLED)
		return 1;

	/* Succeeded: the camera rests on the pointer. */
	return 0;
}
