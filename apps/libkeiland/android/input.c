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

#include <android/input.h>

#include <string.h>

/* The distance of one notch of a mouse's wheel, in pixels (as the Windows backend's). */
#define INPUT_NOTCH_PIXELS	60.0

/* The buttons of the mouse held as the last event told (only M reads and writes it). */
static int32_t input_buttons;

static struct kl_window *input_window(struct kl_app *app);
static void input_fill(struct kl_app *app, struct kl_window_event *input, unsigned kind, const AInputEvent *event);
static void input_place(const struct kl_app *app, const AInputEvent *event, size_t index, struct kl_window_event *input);
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
	if (tool == AMOTION_EVENT_TOOL_TYPE_FINGER || tool == AMOTION_EVENT_TOOL_TYPE_UNKNOWN)
		(void)input_touch(app, window, event);
	else
		(void)input_pointer(app, window, event);

	/* Succeeded. */
	return 1;
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

	/* Queued. */
	return android_window_queue(window, &input);
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

/* Gives a pointer's place in the logical window's pixels. */
static void
input_place(
	const struct kl_app *app,
	const AInputEvent *event,
	size_t index,
	struct kl_window_event *input)
{
	/* The event's place is the native window's; the logical window starts at the insets. */
	input->x = (double)AMotionEvent_getX(event, index) - (double)app->insets[ANDROID_INSET_LEFT];
	input->y = (double)AMotionEvent_getY(event, index) - (double)app->insets[ANDROID_INSET_TOP];
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

	/* What happened, and to which finger. */
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
		input_fill(app, &input, kind, event);
		input.id = AMotionEvent_getPointerId(event, index);
		input_place(app, event, index, &input);
		(void)android_window_queue(window, &input);
		return 1;
	}

	/* Every finger moved. */
	if (action == AMOTION_EVENT_ACTION_MOVE) {
		for (index = 0U; index < count; index++) {
			input_fill(app, &input, KL_WINDOW_TOUCH_MOTION, event);
			input.id = AMotionEvent_getPointerId(event, index);
			input_place(app, event, index, &input);
			(void)android_window_queue(window, &input);
		}
		return 1;
	}

	/* The system took the touch away: every finger is cancelled. */
	if (action == AMOTION_EVENT_ACTION_CANCEL) {
		for (index = 0U; index < count; index++) {
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

	/* The wheel. */
	if (action == AMOTION_EVENT_ACTION_SCROLL) {
		input_fill(app, &input, KL_WINDOW_AXIS, event);
		input_place(app, event, 0U, &input);
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
		input_fill(app, &input, KL_WINDOW_MOTION, event);
		input_place(app, event, 0U, &input);
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
		input_fill(app, &input, KL_WINDOW_BUTTON, event);
		input_place(app, event, 0U, &input);
		input.code = buttons[index].code;
		input.pressed = ((now & buttons[index].mask) != 0);
		input.modifiers = android_key_modifiers(AMotionEvent_getMetaState(event));
		(void)android_window_queue(window, &input);
	}

	/* Remembered for the next event. */
	input_buttons = now;
}
