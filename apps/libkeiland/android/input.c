/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the system's input queue brings: keys, fingers, a mouse and a pen.
 *
 * Every event is read on M, finished (told to the system as handled or not)
 * and turned into an input of the window on top.  A finger is a touch input
 * (KL_WINDOW_TOUCH_*, with its pointer id); a mouse or a pen is a pointer
 * (motion, buttons, the wheel).  The places are in the pixels of the
 * logical window, that is, less the insets.
 *
 * A key the input method takes (AInputQueue_preDispatchEvent) is not ours:
 * its text arrives through the bridge (ime.c).
 */

#include "apps/libkeiland/android/android.h"

#include "apps/libkeiland/android/bridge-jni.h"

#include <android/input.h>

#include <math.h>
#include <string.h>

/* A touch that is short and stays in its place is a tap, which shows the keyboard: the time in milliseconds, the distance in pixels. */
#define INPUT_TAP_MS		300
#define INPUT_TAP_PIXELS	32.0

/* The distance of one notch of a mouse's wheel, in pixels (as the Windows backend's). */
#define INPUT_NOTCH_PIXELS	60.0

/* The key whose press chose a menu item, until it is released (its release and repeats are not typed either). */
static int32_t input_shortcut_down;

/* The buttons of the mouse held as the last event told (only M reads and writes it). */
static int32_t input_buttons;

/* Whether the last touch event was taken by the title bar's strip (a tap on the strip does not show the keyboard). */
static int input_last_strip;

/* Where and when the touch that is going on began (the first finger). */
static float input_down_x;
static float input_down_y;
static int64_t input_down_time;

static struct kl_window *input_window(struct kl_app *app);
static int input_shortcut(struct kl_window *window, int32_t keycode, int32_t meta, int pressed, int repeated);
static void input_tap(const AInputEvent *event);
static void input_fill(struct kl_app *app, struct kl_window_event *input, unsigned kind, const AInputEvent *event);
static void input_place(const struct kl_window *window, const AInputEvent *event, size_t index, struct kl_window_event *input);
static int input_strip(struct kl_window *window, const AInputEvent *event, size_t index, int32_t finger, int phase);
static int input_touch(struct kl_app *app, struct kl_window *window, const AInputEvent *event);
static int input_pointer(struct kl_app *app, struct kl_window *window, const AInputEvent *event);
static void input_buttons_change(struct kl_app *app, struct kl_window *window, const AInputEvent *event);

/*
 * Reads the queue until it is empty, finishing each event: handled when the
 * program took it, not handled when it is the system's (a volume key).
 */
void
android_input_drain(
	struct kl_app *app,
	AInputQueue *queue)
{
	AInputEvent *event;
	int handled;
	int type;

	/* Each event the queue has. */
	while (AInputQueue_getEvent(queue, &event) >= 0) {
		/* The input method took it first. */
		if (AInputQueue_preDispatchEvent(queue, event) != 0)
			continue;

		/* Ours or not. */
		handled = 0;
		type = AInputEvent_getType(event);
		if (type == AINPUT_EVENT_TYPE_KEY)
			handled = android_input_key(app, event);
		else if (type == AINPUT_EVENT_TYPE_MOTION)
			handled = android_input_motion(app, event);
		AInputQueue_finishEvent(queue, event, handled);
	}
}

/*
 * Turns a key event into a KL_WINDOW_KEY of the window on top.  Returns 1
 * when the program took it, 0 for a key that is the system's.
 */
int
android_input_key(
	struct kl_app *app,
	const AInputEvent *event)
{
	struct kl_window *window;
	struct kl_window_event input;
	int32_t action;
	uint32_t code;

	/* A window to give it to. */
	window = input_window(app);
	if (window == NULL)
		return 0;

	/* Pressed or released; a key held down comes as presses with a repeat count. */
	action = AKeyEvent_getAction(event);
	if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP && action != AKEY_EVENT_ACTION_MULTIPLE)
		return 0;

	/* The key; one the table does not know (volume, power) is the system's. */
	code = android_key_code(AKeyEvent_getKeyCode(event), AKeyEvent_getScanCode(event));
	if (code == 0U)
		return 0;

	/* A key with Ctrl, Alt or Super that is a menu item's shortcut chooses the item (on its press), and is not typed. */
	if (input_shortcut(window, AKeyEvent_getKeyCode(event), AKeyEvent_getMetaState(event), action != AKEY_EVENT_ACTION_UP,
	    AKeyEvent_getRepeatCount(event) > 0))
		return 1;

	/* The input. */
	input_fill(app, &input, KL_WINDOW_KEY, event);
	input.code = code;
	input.pressed = (action != AKEY_EVENT_ACTION_UP);
	input.repeated = (AKeyEvent_getRepeatCount(event) > 0);
	input.modifiers = android_key_modifiers(AKeyEvent_getMetaState(event));
	(void)android_window_queue(window, &input);

	/* Succeeded: taken. */
	return 1;
}

/*
 * Turns a motion event into inputs of the window on top: touches for
 * fingers, a pointer's motion, buttons and wheel for a mouse or a pen.
 * Returns 1.
 */
int
android_input_motion(
	struct kl_app *app,
	const AInputEvent *event)
{
	struct kl_window *window;
	int32_t tool;

	/* A window to give it to; the event is finished as handled either way. */
	window = input_window(app);
	if (window == NULL)
		return 1;

	/* A finger, or a pointing device. */
	tool = AMotionEvent_getToolType(event, 0);
	if (tool == AMOTION_EVENT_TOOL_TYPE_FINGER || tool == AMOTION_EVENT_TOOL_TYPE_UNKNOWN) {
		(void)input_touch(app, window, event);
		if (!input_last_strip)
			input_tap(event);
	} else
		(void)input_pointer(app, window, event);

	/* Succeeded. */
	return 1;
}

/* Shows the keyboard when the finger that touched lifts in its place soon (a tap): the user may have put it away. */
static void
input_tap(
	const AInputEvent *event)
{
	int32_t action;
	float dx;
	float dy;
	int64_t time;

	/* The first finger's down, and its up. */
	action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
	if (action == AMOTION_EVENT_ACTION_DOWN) {
		input_down_x = AMotionEvent_getX(event, 0);
		input_down_y = AMotionEvent_getY(event, 0);
		input_down_time = AMotionEvent_getEventTime(event);
		return;
	}
	if (action != AMOTION_EVENT_ACTION_UP)
		return;

	/* Soon, and in the same place. */
	time = AMotionEvent_getEventTime(event) - input_down_time;
	dx = AMotionEvent_getX(event, 0) - input_down_x;
	dy = AMotionEvent_getY(event, 0) - input_down_y;
	if (time > (int64_t)INPUT_TAP_MS * 1000000 || sqrt((double)(dx * dx + dy * dy)) > INPUT_TAP_PIXELS)
		return;
	(void)keiland_java_show_keyboard(1);
}

/*
 * Turns a key of the key bar (told through the bridge) into a KL_WINDOW_KEY.
 * Returns 0, or ENOMEM.
 */
int
android_input_bridge_key(
	struct kl_app *app,
	const struct keiland_bridge_event *event)
{
	struct kl_window *window;
	struct kl_window_event input;
	uint32_t code;

	/* A window and a key. */
	window = input_window(app);
	if (window == NULL)
		return 0;
	code = android_key_code(event->a, 0);
	if (code == 0U)
		return 0;

	/* The input. */
	memset(&input, 0, sizeof(input));
	input.kind = KL_WINDOW_KEY;
	input.code = code;
	input.pressed = (event->c != 0);
	input.modifiers = android_key_modifiers(event->b);

	/* A shortcut of the menu is not typed. */
	if (input_shortcut(window, event->a, event->b, event->c != 0, 0))
		return 0;

	/* Queued. */
	return android_window_queue(window, &input);
}

/* Tells whether a key's press, repeat or release belongs to a menu item's shortcut (the press chooses the item).  Returns 1 if it does. */
static int
input_shortcut(
	struct kl_window *window,
	int32_t keycode,
	int32_t meta,
	int pressed,
	int repeated)
{
	/* The release of a key that chose an item, and its repeats. */
	if (input_shortcut_down == keycode && input_shortcut_down != 0) {
		if (!pressed)
			input_shortcut_down = 0;
		return 1;
	}

	/* A new press that is a shortcut. */
	if (pressed && !repeated && android_titlebar_shortcut(window, keycode, meta)) {
		input_shortcut_down = keycode;
		return 1;
	}
	return 0;
}

/* Returns the window the input goes to, or NULL. */
static struct kl_window *
input_window(
	struct kl_app *app)
{
	/* The window on top. */
	return android_window_top(app);
}

/* Starts an input of a kind, timed by the event (its time is in nanoseconds of the monotonic clock). */
static void
input_fill(
	struct kl_app *app,
	struct kl_window_event *input,
	unsigned kind,
	const AInputEvent *event)
{
	int64_t time;

	/* Nothing set but the kind and the time. */
	(void)app;
	memset(input, 0, sizeof(*input));
	input->kind = kind;
	time = AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY ? AKeyEvent_getEventTime(event) :
	    AMotionEvent_getEventTime(event);
	if (time > 0)
		input->time_us = (uint64_t)time / 1000U;
}

/* Gives a pointer's place in the program's window's pixels (below the strip). */
static void
input_place(
	const struct kl_window *window,
	const AInputEvent *event,
	size_t index,
	struct kl_window_event *input)
{
	/* The event's place is the native window's; the logical window starts at the insets, and the program's below the strip. */
	input->x = (double)AMotionEvent_getX(event, index) - (double)window->app->insets[ANDROID_INSET_LEFT];
	input->y = (double)AMotionEvent_getY(event, index) - (double)window->app->insets[ANDROID_INSET_TOP] - (double)android_titlebar_height(window);
}

/* Gives the strip a touch at a pointer's place (the window's pixels, the strip at the top); returns 1 when the strip took it. */
static int
input_strip(
	struct kl_window *window,
	const AInputEvent *event,
	size_t index,
	int32_t finger,
	int phase)
{
	double x;
	double y;

	/* The place in the window's pixels. */
	x = (double)AMotionEvent_getX(event, index) - (double)window->app->insets[ANDROID_INSET_LEFT];
	y = (double)AMotionEvent_getY(event, index) - (double)window->app->insets[ANDROID_INSET_TOP];
	return android_titlebar_hit(window, x, y, finger, phase);
}

/* Turns a finger event into touches: one finger down or up, or all fingers moved or cancelled. */
static int
input_touch(
	struct kl_app *app,
	struct kl_window *window,
	const AInputEvent *event)
{
	struct kl_window_event input;
	int32_t action;
	size_t index;
	size_t count;
	unsigned kind;

	/* What happened, and to which finger; the strip may take it. */
	input_last_strip = 0;
	action = AMotionEvent_getAction(event);
	index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
	count = AMotionEvent_getPointerCount(event);
	action &= AMOTION_EVENT_ACTION_MASK;

	/* One finger touched or left. */
	if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN ||
	    action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP) {
		kind = KL_WINDOW_TOUCH_DOWN;
		if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP)
			kind = KL_WINDOW_TOUCH_UP;
		if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_UP)
			index = 0U;
		if (index >= count)
			return 0;

		/* A finger that touches the strip, and the rest of its touch, is the strip's. */
		input_last_strip = input_strip(window, event, index, AMotionEvent_getPointerId(event, index), kind == KL_WINDOW_TOUCH_DOWN ? 1 : 0);
		if (input_last_strip)
			return 1;
		input_fill(app, &input, kind, event);
		input.id = AMotionEvent_getPointerId(event, index);
		input_place(window, event, index, &input);
		(void)android_window_queue(window, &input);
		return 1;
	}

	/* Every finger moved. */
	if (action == AMOTION_EVENT_ACTION_MOVE) {
		for (index = 0U; index < count; index++) {
			if (input_strip(window, event, index, AMotionEvent_getPointerId(event, index), 2)) {
				input_last_strip = 1;
				continue;
			}
			input_fill(app, &input, KL_WINDOW_TOUCH_MOTION, event);
			input.id = AMotionEvent_getPointerId(event, index);
			input_place(window, event, index, &input);
			(void)android_window_queue(window, &input);
		}
		return 1;
	}

	/* The system took the touch away: every finger is cancelled. */
	if (action == AMOTION_EVENT_ACTION_CANCEL) {
		for (index = 0U; index < count; index++) {
			if (input_strip(window, event, index, AMotionEvent_getPointerId(event, index), 3)) {
				input_last_strip = 1;
				continue;
			}
			input_fill(app, &input, KL_WINDOW_TOUCH_CANCEL, event);
			input.id = AMotionEvent_getPointerId(event, index);
			(void)android_window_queue(window, &input);
		}
		return 1;
	}

	/* Anything else (an outside touch) is not ours. */
	return 0;
}

/* Turns a mouse's or a pen's event into the pointer's inputs. */
static int
input_pointer(
	struct kl_app *app,
	struct kl_window *window,
	const AInputEvent *event)
{
	struct kl_window_event input;
	int32_t action;
	double notch;

	/* What happened. */
	action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;

	/* The pointer left the window. */
	if (action == AMOTION_EVENT_ACTION_HOVER_EXIT) {
		input_fill(app, &input, KL_WINDOW_LEAVE, event);
		(void)android_window_queue(window, &input);
		return 1;
	}

	/* The wheel; over the strip it is not the program's. */
	if (action == AMOTION_EVENT_ACTION_SCROLL) {
		if ((double)AMotionEvent_getY(event, 0) - (double)app->insets[ANDROID_INSET_TOP] < (double)android_titlebar_height(window))
			return 1;
		input_fill(app, &input, KL_WINDOW_AXIS, event);
		input_place(window, event, 0U, &input);
		input.axis_source = KL_AXIS_SOURCE_WHEEL;

		/* A turn away from the user scrolls up, which is a negative distance; right is positive. */
		notch = (double)AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0);
		input.dy = -notch * INPUT_NOTCH_PIXELS;
		notch = (double)AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, 0);
		input.dx = notch * INPUT_NOTCH_PIXELS;
		(void)android_window_queue(window, &input);
		return 1;
	}

	/* The place first, then the buttons that changed. */
	if (action == AMOTION_EVENT_ACTION_HOVER_MOVE || action == AMOTION_EVENT_ACTION_MOVE ||
	    action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_UP ||
	    action == AMOTION_EVENT_ACTION_BUTTON_PRESS || action == AMOTION_EVENT_ACTION_BUTTON_RELEASE ||
	    action == AMOTION_EVENT_ACTION_HOVER_ENTER) {
		/* The strip holds the mouse while its button is down on it: the motion is the strip's. */
		if (input_strip(window, event, 0U, ANDROID_FINGER_MOUSE, 2))
			return 1;
		input_fill(app, &input, KL_WINDOW_MOTION, event);
		input_place(window, event, 0U, &input);
		input.modifiers = android_key_modifiers(AMotionEvent_getMetaState(event));
		(void)android_window_queue(window, &input);
		input_buttons_change(app, window, event);
		return 1;
	}

	/* Anything else is not ours. */
	return 0;
}

/* Tells the pointer's buttons that were pressed or released since the last event. */
static void
input_buttons_change(
	struct kl_app *app,
	struct kl_window *window,
	const AInputEvent *event)
{
	static const struct {
		int32_t mask;
		uint32_t code;
	} buttons[] = {
		{ AMOTION_EVENT_BUTTON_PRIMARY, KL_BUTTON_LEFT },
		{ AMOTION_EVENT_BUTTON_SECONDARY, KL_BUTTON_RIGHT },
		{ AMOTION_EVENT_BUTTON_TERTIARY, KL_BUTTON_MIDDLE }
	};
	struct kl_window_event input;
	int32_t now;
	size_t index;
	int32_t action;

	/* The buttons held now; a press with no button state (a plain touch of a pen) is the primary one. */
	now = AMotionEvent_getButtonState(event);
	action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
	if (action == AMOTION_EVENT_ACTION_DOWN && now == 0)
		now = AMOTION_EVENT_BUTTON_PRIMARY;
	if (action == AMOTION_EVENT_ACTION_UP)
		now = 0;

	/* Each button that changed. */
	for (index = 0U; index < sizeof(buttons) / sizeof(buttons[0]); index++) {
		if ((now & buttons[index].mask) == (input_buttons & buttons[index].mask))
			continue;

		/* The primary button pressed on the strip, and its release, are the strip's. */
		if (buttons[index].code == KL_BUTTON_LEFT &&
		    input_strip(window, event, 0U, ANDROID_FINGER_MOUSE, (now & buttons[index].mask) != 0 ? 1 : 0))
			continue;
		input_fill(app, &input, KL_WINDOW_BUTTON, event);
		input_place(window, event, 0U, &input);
		input.code = buttons[index].code;
		input.pressed = ((now & buttons[index].mask) != 0);
		input.modifiers = android_key_modifiers(AMotionEvent_getMetaState(event));
		(void)android_window_queue(window, &input);
	}

	/* Remembered for the next event. */
	input_buttons = now;
}
