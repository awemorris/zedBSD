/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window of Settings (WS131 p019): a window of libkeiland's
 * application, whose input becomes se_event values in a queue the main
 * loop hands to the interface, and the first screen's mode (About and
 * Display show it).
 *
 * The application repeats a held key (its repeats come as presses).  A
 * finger is taken as the pointer: it moves the pointer where it lands,
 * presses the left button, and lets it go where it lifts (a tap is a
 * click; scrolling by finger comes later, ws089-p006).  The wheel scrolls
 * three pixels a unit (the compositor sends fifteen units a notch); a touch
 * pad's fingers say where they come from and when they lift (BUG-211).
 * The menus' choices join the queue as SE_EVENT_ACTION; the titlebar's
 * controls and fields go to its queue (titlebar.c).
 */

#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* How many pixels one unit of scrolling moves, and how many libkeiland gives a unit. */
#define WINDOW_SCROLL_SCALE	3.0
#define WINDOW_KL_SCROLL_SCALE	4.0

static void window_event(struct se_window *window, const struct kl_window_event *event);
static void window_axis(struct se_window *window, const struct kl_window_event *event);
static void window_text(struct se_window *window, const struct kl_window_event *event);
static void window_touch(struct se_window *window, const struct kl_window_event *event);
static void window_touch_place(struct se_window *window, double x, double y);
static void window_touch_button(struct se_window *window, uint32_t serial, int pressed);
static uint32_t window_modifiers(unsigned modifiers);

/*
 * Connects to the compositor and makes the window, configured.
 *
 * Returns 0, or -1 with errno set.
 */
int
se_window_open(
	struct se_window *window,
	const char *display,
	uint32_t width,
	uint32_t height,
	const char *title,
	const char *application)
{
	struct kl_window_options options;
	struct kl_app_options app_options;
	int error;

	/* Nothing held yet; no finger is the pointer and no descriptor is watched. */
	memset(window, 0, sizeof(*window));
	window->touch_id = -1;
	window->extra_fd = -1;

	/* The application: the connection, and the identity of its windows. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.display = display;
	app_options.application = application;
	window->app = kl_app_open(&app_options);
	if (window->app == NULL)
		return -1;

	/* Its window, drawn on the CPU and shown with Vulkan. */
	memset(&options, 0, sizeof(options));
	options.title = title;
	options.width = width;
	options.height = height;
	options.present = KL_PRESENT_VULKAN;
	window->kui = kl_app_window_create(window->app, &options);
	if (window->kui == NULL)
		return -1;

	/* The connection and the surface, the size, the state and the screen's mode the first configure left. */
	window->display = kl_app_display(window->app);
	window->surface = kl_window_surface(window->kui);
	kl_window_size(window->kui, &window->width, &window->height);
	window->maximized = kl_window_maximized(window->kui);
	error = kl_window_output_mode(window->kui, &window->output_width, &window->output_height, &window->output_refresh);
	if (error != 0)
		se_log("WINDOW output none");

	/* Succeeded: the window can be drawn into. */
	return 0;
}

/*
 * Waits up to a timeout (milliseconds, -1 for ever) for the compositor or
 * a later start of Settings, and takes the window's input.
 *
 * Returns 0, or -1 when the connection is broken.
 */
int
se_window_dispatch(
	struct se_window *window,
	int timeout)
{
	struct kl_app_event event;
	int status;
	int taken;

	/* A later start of Settings wakes the wait (the main loop reads the socket itself). */
	if (window->extra_fd >= 0 && !window->extra_watched) {
		status = kl_app_watch_fd(window->app, window->extra_fd, KL_APP_FD_READ);
		if (status == 0)
			window->extra_watched = 1;
	}

	/* Nothing is waited for while input is already queued. */
	if (window->event_count != 0U)
		timeout = 0;

	/* The compositor's events (a held key repeats within). */
	status = kl_app_dispatch(window->app, timeout);
	if (status != 0)
		return -1;

	/* The window's input, in its order. */
	for (;;) {
		taken = kl_app_take(window->app, &event);
		if (taken == 0)
			break;
		if (event.kind == KL_APP_WINDOW && event.window == window->kui)
			window_event(window, &event.input);
	}

	/* Succeeded: the events so far have run. */
	return 0;
}

/*
 * Takes the oldest queued input; zero when there is none.
 */
int
se_window_take(
	struct se_window *window,
	struct se_event *event)
{
	/* An empty queue. */
	if (window->event_count == 0U)
		return 0;

	/* The oldest input, and the queue moves on. */
	*event = window->events[window->event_first];
	window->event_first = (window->event_first + 1U) % SE_WINDOW_EVENTS;
	window->event_count--;

	/* Succeeded: one input taken. */
	return 1;
}

/*
 * Queues a menu's choice among the window's inputs, so that it is carried
 * out in the order it came with the keys around it.
 */
void
se_window_action(
	struct se_window *window,
	uint32_t action)
{
	struct se_event *event;

	/* The input; a full queue drops it. */
	event = se_window_push(window, SE_EVENT_ACTION);
	if (event == NULL)
		return;
	event->action = action;
}

/*
 * Asks the compositor to minimize the window.
 */
void
se_window_minimize(
	struct se_window *window)
{
	/* The request, sent with the next flush. */
	kl_window_minimize(window->kui);
	se_log("WINDOW minimize");
}

/*
 * Asks the compositor to maximize the window, or to bring a maximized one
 * back to its size (Window > Zoom).
 */
void
se_window_zoom(
	struct se_window *window)
{
	int maximized;

	/* A maximized window comes back; another one is maximized. */
	maximized = 1;
	if (window->maximized != 0)
		maximized = 0;
	kl_window_set_maximized(window->kui, maximized);

	/* The log line the tests wait for. */
	se_log("WINDOW zoom maximized=%d", maximized);
}

/*
 * Destroys the window, then the application and its connection.
 */
void
se_window_close(
	struct se_window *window)
{
	/* The window, then the application. */
	if (window->kui != NULL)
		kl_window_close(window->kui);
	if (window->app != NULL)
		kl_app_close(window->app);

	/* Nothing is held. */
	memset(window, 0, sizeof(*window));
	window->extra_fd = -1;
	window->touch_id = -1;
}

/*
 * Returns a monotonic time in milliseconds (0 when the clock cannot be read).
 */
uint64_t
se_clock(void)
{
	struct timespec now;
	int status;

	/* The monotonic clock. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0U;

	/* Reports it in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/*
 * Queues a new input of a kind at the pointer's place with the modifiers
 * held.  Returns the input to fill in, or NULL when the queue is full.
 */
struct se_event *
se_window_push(
	struct se_window *window,
	unsigned type)
{
	struct se_event *event;
	unsigned slot;

	/* A full queue drops the input (the user is far ahead of the program). */
	if (window->event_count == SE_WINDOW_EVENTS)
		return NULL;

	/* The slot after the last one queued. */
	slot = (window->event_first + window->event_count) % SE_WINDOW_EVENTS;
	window->event_count++;

	/* The input, with what every input carries. */
	event = &window->events[slot];
	memset(event, 0, sizeof(*event));
	event->type = type;
	event->x = window->pointer_x;
	event->y = window->pointer_y;
	event->modifiers = window->modifiers;
	event->time = se_clock();

	/* Reports the queued input for its details. */
	return event;
}

/* Turns one input of the window into the interface's, the menus' or the titlebar's. */
static void
window_event(
	struct se_window *window,
	const struct kl_window_event *event)
{
	struct se_event *input;
	int control;

	/* Every input carries the modifiers held. */
	window->modifiers = window_modifiers(event->modifiers);

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		/* The pointer's new place, as a motion. */
		window->pointer_x = (int)event->x;
		window->pointer_y = (int)event->y;
		(void)se_window_push(window, SE_EVENT_MOTION);
		break;
	case KL_WINDOW_LEAVE:
		/* The interface stops lighting what was under it. */
		(void)se_window_push(window, SE_EVENT_LEAVE);
		break;
	case KL_WINDOW_BUTTON:
		/* The button and its serial; a press's is kept for a context menu. */
		if (event->pressed)
			window->button_serial = event->serial;
		input = se_window_push(window, SE_EVENT_BUTTON);
		if (input == NULL)
			break;
		input->button = event->code;
		input->pressed = event->pressed;
		input->serial = event->serial;
		break;
	case KL_WINDOW_AXIS:
	case KL_WINDOW_AXIS_STOP:
		window_axis(window, event);
		break;
	case KL_WINDOW_KEY:
		/* The key (a held key's repeats are presses too). */
		input = se_window_push(window, SE_EVENT_KEY);
		if (input == NULL)
			break;
		input->key = event->code;
		input->pressed = event->pressed;
		break;
	case KL_WINDOW_TEXT_COMMIT:
	case KL_WINDOW_TEXT_PREEDIT:
	case KL_WINDOW_TEXT_DELETE:
		/* An input method's text, for the field that takes it (ws090-p022). */
		window_text(window, event);
		break;
	case KL_WINDOW_FOCUS:
		/* The page shown is drawn in the accent while the window has the focus, grey otherwise. */
		input = se_window_push(window, SE_EVENT_FOCUS);
		if (input != NULL)
			input->focused = event->pressed;
		break;
	case KL_WINDOW_TOUCH_DOWN:
	case KL_WINDOW_TOUCH_MOTION:
	case KL_WINDOW_TOUCH_UP:
	case KL_WINDOW_TOUCH_CANCEL:
		window_touch(window, event);
		break;
	case KL_WINDOW_RESIZE:
		/* The size and the state the compositor gave, drawn at from the next frame. */
		kl_window_size(window->kui, &window->width, &window->height);
		window->maximized = kl_window_maximized(window->kui);
		window->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		window->closed = 1;
		break;
	case KL_WINDOW_ACTION:
		/* A control's choice is the titlebar's; a menu's item is the interface's, in its place among the keys. */
		control = 0;
		if (event->code > SE_TITLEBAR_ACTION && event->code < SE_ACTION_PAGE_FIRST)
			control = 1;
		if (control) {
			if (window->titlebar != NULL)
				se_titlebar_input(window->titlebar, event);
			break;
		}
		se_log("MENU item=%d action=%u serial=%u", (int)event->id, event->code, event->serial);
		se_window_action(window, event->code);
		break;
	case KL_WINDOW_CONTROL_TEXT:
	case KL_WINDOW_CONTROL_DONE:
		/* A field's text, the titlebar's. */
		if (window->titlebar != NULL)
			se_titlebar_input(window->titlebar, event);
		break;
	default:
		break;
	}
}

/*
 * Turns the wheel's or a touch pad's scrolling into whole pixels of the
 * window's (the fraction kept for the next), and the fingers' lift into
 * SE_EVENT_AXIS_STOP.
 */
static void
window_axis(
	struct se_window *window,
	const struct kl_window_event *event)
{
	struct se_event *input;
	double distance;
	unsigned source;
	int whole;

	/* The fingers lifted: the fraction left goes with the scrolling. */
	if (event->kind == KL_WINDOW_AXIS_STOP) {
		window->axis_remainder = 0.0;
		input = se_window_push(window, SE_EVENT_AXIS_STOP);
		if (input == NULL)
			return;
		input->source = SE_SOURCE_FINGER;
		input->axis_ms = (uint32_t)(event->time_us / 1000U);
		return;
	}

	/* What it comes from. */
	source = SE_SOURCE_WHEEL;
	if (event->axis_source == KL_AXIS_SOURCE_FINGER)
		source = SE_SOURCE_FINGER;

	/* Only the vertical axis scrolls the window; its distance in the window's pixels, its fraction kept. */
	if (event->dy == 0.0)
		return;
	distance = event->dy * WINDOW_SCROLL_SCALE / WINDOW_KL_SCROLL_SCALE + window->axis_remainder;
	whole = (int)distance;
	window->axis_remainder = distance - (double)whole;
	if (whole == 0)
		return;

	/* The scroll, with what it came from. */
	input = se_window_push(window, SE_EVENT_AXIS);
	if (input == NULL)
		return;
	input->scroll = whole;
	input->source = source;
	input->axis_ms = (uint32_t)(event->time_us / 1000U);
}

/* Takes the first finger as the pointer: a press where it lands, motions while it moves, a release where it lifts. */
static void
window_touch(
	struct se_window *window,
	const struct kl_window_event *event)
{
	/* What the finger did. */
	switch (event->kind) {
	case KL_WINDOW_TOUCH_DOWN:
		/* Only one finger is the pointer at a time: it goes there and presses. */
		if (window->touch_id >= 0)
			return;
		window->touch_id = event->id;
		window->button_serial = event->serial;
		window_touch_place(window, event->x, event->y);
		window_touch_button(window, event->serial, 1);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		/* The pointer follows the finger taken. */
		if (event->id != window->touch_id)
			return;
		window_touch_place(window, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		/* The button is let go where the finger was. */
		if (event->id != window->touch_id)
			return;
		window->touch_id = -1;
		window_touch_button(window, event->serial, 0);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		/* The compositor took the fingers: the pointer leaves, so that the release clicks nothing. */
		if (window->touch_id < 0)
			return;
		window->touch_id = -1;
		window->pointer_x = -1;
		window->pointer_y = -1;
		window_touch_button(window, 0U, 0);
		break;
	default:
		break;
	}
}

/* Moves the pointer to a finger's place and queues the motion. */
static void
window_touch_place(
	struct se_window *window,
	double x,
	double y)
{
	struct se_event *input;

	/* The finger's place is the pointer's, a move the finger made. */
	window->pointer_x = (int)x;
	window->pointer_y = (int)y;
	input = se_window_push(window, SE_EVENT_MOTION);
	if (input != NULL)
		input->touch = 1;
}

/* Queues the left button pressed or let go by the finger taken as the pointer. */
static void
window_touch_button(
	struct se_window *window,
	uint32_t serial,
	int pressed)
{
	struct se_event *input;

	/* The button, as a click would give it, pressed by a finger. */
	input = se_window_push(window, SE_EVENT_BUTTON);
	if (input == NULL)
		return;
	input->button = SE_BUTTON_LEFT;
	input->pressed = pressed;
	input->serial = serial;
	input->touch = 1;
}

/* Turns libkeiland's modifier bits into Settings' (SE_MOD_*). */
static uint32_t
window_modifiers(
	unsigned modifiers)
{
	uint32_t bits;

	/* Shift, Control, Alt and Super. */
	bits = 0U;
	if ((modifiers & KL_MOD_SHIFT) != 0U)
		bits |= SE_MOD_SHIFT;
	if ((modifiers & KL_MOD_CTRL) != 0U)
		bits |= SE_MOD_CTRL;
	if ((modifiers & KL_MOD_ALT) != 0U)
		bits |= SE_MOD_ALT;
	if ((modifiers & KL_MOD_SUPER) != 0U)
		bits |= SE_MOD_SUPER;

	/* Reports them. */
	return bits;
}

/* Queues an input method's text, the bytes it deletes before the caret, or the text it composes (ws090-p022). */
static void
window_text(
	struct se_window *window,
	const struct kl_window_event *event)
{
	struct se_event *input;
	unsigned type;

	/* Its kind. */
	type = SE_EVENT_TEXT;
	if (event->kind == KL_WINDOW_TEXT_DELETE)
		type = SE_EVENT_TEXT_DELETE;
	else if (event->kind == KL_WINDOW_TEXT_PREEDIT)
		type = SE_EVENT_PREEDIT;

	/* Queued with the other input, in its place. */
	input = se_window_push(window, type);
	if (input == NULL)
		return;
	(void)snprintf(input->text, sizeof(input->text), "%s", event->text);
	input->before = event->before;
}
