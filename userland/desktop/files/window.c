/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window of files (WS131 p020): a window of libkeiland's application,
 * or the desktop's surface under every window (files --desktop,
 * ws094-p003), whose input becomes fm_event values in a queue the main
 * loop hands to the interface.
 *
 * The application repeats a held key (its repeats come as presses).  The
 * pointer's buttons carry their serial, which a context menu is opened
 * with.  The wheel scrolls three pixels a unit (the compositor sends fifteen
 * units a notch); a touch pad's fingers scroll as fingers do (ws090-p019):
 * their moves and their lift join the touch inputs, which queue for
 * touch.c with the touch screen's (ws081-p010).  The menus' choices join
 * the queue as FM_EVENT_ACTION; the titlebar's controls and fields go to
 * its queue (titlebar.c), the context menu's choices to the menus
 * (menu.c), and drag and drop's inputs become the drop events (dnd.c).
 */

#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* How many pixels one unit of scrolling moves, and how many libkeiland gives a unit. */
#define WINDOW_SCROLL_SCALE	3
#define WINDOW_KL_SCROLL_SCALE	4.0

static int window_open(struct fm_window *window, const char *display, const struct kl_window_options *options);
static void window_event(struct fm_window *window, const struct kl_window_event *event);
static void window_axis(struct fm_window *window, const struct kl_window_event *event);
static void window_text(struct fm_window *window, const struct kl_window_event *event);
static void window_drop(struct fm_window *window, const struct kl_window_event *event);
static void window_desktop_log(struct fm_window *window);
static void window_pad_push(struct fm_window *window, unsigned type, uint32_t time, double distance);
static void window_touch_push(struct fm_window *window, unsigned type, const struct kl_window_event *event);
static uint32_t window_modifiers(unsigned modifiers);

/*
 * Connects to the compositor and makes a toplevel window of a size, its
 * size until the compositor gives one.
 *
 * Returns 0 once the first configure is acknowledged, or -1 with errno set.
 */
int
fm_window_open(
	struct fm_window *window,
	const char *display,
	uint32_t width,
	uint32_t height,
	const char *title,
	const char *application)
{
	struct kl_window_options options;
	int status;

	/* A window, drawn on the CPU and shown with Vulkan. */
	memset(&options, 0, sizeof(options));
	options.title = title;
	options.application = application;
	options.width = width;
	options.height = height;
	options.present = KL_PRESENT_VULKAN;
	status = window_open(window, display, &options);
	return status;
}

/*
 * Connects to the compositor and makes the desktop's surface with the
 * token the compositor gave the program; its size is the compositor's.
 *
 * Returns 0 once the first configure is acknowledged, or -1 with errno set.
 */
int
fm_window_open_desktop(
	struct fm_window *window,
	const char *display,
	const char *token)
{
	struct kl_window_options options;
	int status;

	/* The desktop's surface, drawn on the CPU and shown with Vulkan. */
	memset(&options, 0, sizeof(options));
	options.application = "files";
	options.present = KL_PRESENT_VULKAN;
	options.role = KL_WINDOW_ROLE_DESKTOP;
	options.token = token;
	status = window_open(window, display, &options);
	if (status != 0)
		return status;

	/* The desktop has the keyboard when it is pressed; the log line the tests read. */
	window->desktop = 1;
	window->activated = 1;
	window_desktop_log(window);
	return 0;
}

/*
 * Waits up to a timeout (milliseconds, -1 for ever) for the compositor's
 * events and takes them; they queue input for fm_window_take.
 *
 * Returns 0, or -1 when the connection is broken.
 */
int
fm_window_dispatch(
	struct fm_window *window,
	int timeout)
{
	struct kl_app_event event;
	int status;
	int taken;

	/* Nothing is waited for while input is already queued. */
	if (window->event_count != 0U || window->touch_count != 0U)
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
fm_window_take(
	struct fm_window *window,
	struct fm_event *event)
{
	/* An empty queue. */
	if (window->event_count == 0U)
		return 0;

	/* The oldest input, and the queue moves on. */
	*event = window->events[window->event_first];
	window->event_first = (window->event_first + 1U) % FM_WINDOW_EVENTS;
	window->event_count--;

	/* Succeeded: one input taken. */
	return 1;
}

/*
 * Queues a menu's choice among the window's inputs, so that it is carried
 * out in the order it came with the keys around it (a shortcut and the
 * text typed after it).
 */
void
fm_window_action(
	struct fm_window *window,
	uint32_t action)
{
	struct fm_event *event;

	/* The input; a full queue drops it. */
	event = fm_window_push(window, FM_EVENT_ACTION);
	if (event == NULL)
		return;
	event->action = action;
}

/*
 * Asks the compositor to minimize the window.
 */
void
fm_window_minimize(
	struct fm_window *window)
{
	/* The request, sent with the next flush. */
	kl_window_minimize(window->kui);
	fm_log("WINDOW minimize");
}

/*
 * Asks the compositor to maximize the window, or to bring a maximized one
 * back to its size (Window > Zoom).
 */
void
fm_window_zoom(
	struct fm_window *window)
{
	int maximized;

	/* A maximized window comes back; another one is maximized. */
	maximized = 1;
	if (window->maximized != 0)
		maximized = 0;
	kl_window_set_maximized(window->kui, maximized);

	/* The log line the tests wait for. */
	fm_log("WINDOW zoom maximized=%d", maximized);
}

/*
 * Destroys the window, then the application and its connection.
 */
void
fm_window_close(
	struct fm_window *window)
{
	/* The window, then the application. */
	if (window->kui != NULL)
		kl_window_close(window->kui);
	if (window->app != NULL)
		kl_app_close(window->app);
	memset(window, 0, sizeof(*window));
}

/*
 * Returns a monotonic time in milliseconds (0 when the clock cannot be read).
 */
uint64_t
fm_clock(void)
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
struct fm_event *
fm_window_push(
	struct fm_window *window,
	unsigned type)
{
	struct fm_event *event;
	unsigned slot;

	/* A full queue drops the input (the user is far ahead of the program). */
	if (window->event_count == FM_WINDOW_EVENTS)
		return NULL;

	/* The slot after the last one queued. */
	slot = (window->event_first + window->event_count) % FM_WINDOW_EVENTS;
	window->event_count++;

	/* The input, with what every input carries. */
	event = &window->events[slot];
	memset(event, 0, sizeof(*event));
	event->type = type;
	event->x = window->pointer_x;
	event->y = window->pointer_y;
	event->modifiers = window->modifiers;
	event->time = fm_clock();

	/* Reports the queued input for its details. */
	return event;
}

/* Opens the application and its window or desktop surface, as the options say; 0 or -1 with errno set. */
static int
window_open(
	struct fm_window *window,
	const char *display,
	const struct kl_window_options *options)
{
	struct kl_app_options app_options;
	int error;

	/* Nothing held yet. */
	memset(window, 0, sizeof(*window));

	/* The application: the connection, and the identity of its windows. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.display = display;
	app_options.application = options->application;
	window->app = kl_app_open(&app_options);
	if (window->app == NULL)
		return -1;

	/* Its window, configured. */
	window->kui = kl_app_window_create(window->app, options);
	if (window->kui == NULL)
		return -1;

	/* The connection and the surface, the size and the state the first configure left. */
	window->display = kl_app_display(window->app);
	window->surface = kl_window_surface(window->kui);
	kl_window_size(window->kui, &window->width, &window->height);
	window->maximized = kl_window_maximized(window->kui);

	/* Drag and drop of file names with other programs, where the compositor has it (dnd.c). */
	error = kl_window_accept_drops(window->kui, KL_DROP_URIS);
	if (error != 0)
		fm_log("DND none errno=%d", error);

	/* Succeeded: the surface can be drawn into. */
	window->resized = 0;
	return 0;
}

/* Turns one input of the window into the interface's, the touch inputs', the menus' or the titlebar's. */
static void
window_event(
	struct fm_window *window,
	const struct kl_window_event *event)
{
	struct fm_event *input;
	int control;

	/* Every input carries the modifiers held. */
	window->modifiers = window_modifiers(event->modifiers);

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		/* The pointer's new place, as a motion. */
		window->pointer_x = (int)event->x;
		window->pointer_y = (int)event->y;
		(void)fm_window_push(window, FM_EVENT_MOTION);
		break;
	case KL_WINDOW_LEAVE:
		/* The interface stops lighting what was under it. */
		(void)fm_window_push(window, FM_EVENT_LEAVE);
		break;
	case KL_WINDOW_BUTTON:
		/* The button and its serial; a press's is kept for a context menu and a drag. */
		if (event->pressed)
			window->button_serial = event->serial;
		input = fm_window_push(window, FM_EVENT_BUTTON);
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
		input = fm_window_push(window, FM_EVENT_KEY);
		if (input == NULL)
			break;
		input->key = event->code;
		input->pressed = event->pressed;
		break;
	case KL_WINDOW_TEXT_COMMIT:
	case KL_WINDOW_TEXT_PREEDIT:
	case KL_WINDOW_TEXT_DELETE:
		/* An input method's text, for the name being changed (ws090-p022). */
		window_text(window, event);
		break;
	case KL_WINDOW_FOCUS:
		/* The selection is drawn in the accent while the window has the focus, grey otherwise. */
		window->activated = event->pressed;
		input = fm_window_push(window, FM_EVENT_FOCUS);
		if (input != NULL)
			input->focused = event->pressed;
		break;
	case KL_WINDOW_TOUCH_DOWN:
		window_touch_push(window, FM_TOUCH_DOWN, event);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		window_touch_push(window, FM_TOUCH_MOTION, event);
		break;
	case KL_WINDOW_TOUCH_UP:
		window_touch_push(window, FM_TOUCH_UP, event);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		window_touch_push(window, FM_TOUCH_CANCEL, event);
		break;
	case KL_WINDOW_RESIZE:
		/* The size and the state the compositor gave, drawn at from the next frame. */
		kl_window_size(window->kui, &window->width, &window->height);
		window->maximized = kl_window_maximized(window->kui);
		window->resized = 1;
		if (window->desktop)
			window_desktop_log(window);
		break;
	case KL_WINDOW_CLOSE:
		window->closed = 1;
		break;
	case KL_WINDOW_ACTION:
		/* A control's choice is the titlebar's; an item of the menus or of the context menu is the interface's, in its place among the keys. */
		control = 0;
		if (event->code > FM_TITLEBAR_ACTION && event->code < FM_CONTEXT_ACTION)
			control = 1;
		if (control) {
			if (window->titlebar != NULL)
				fm_titlebar_input(window->titlebar, event);
			break;
		}
		if (window->menu != NULL)
			fm_menu_chosen(window->menu, event);
		break;
	case KL_WINDOW_CONTROL_TEXT:
	case KL_WINDOW_CONTROL_DONE:
		/* A field's text, the titlebar's. */
		if (window->titlebar != NULL)
			fm_titlebar_input(window->titlebar, event);
		break;
	case KL_WINDOW_POPUP_DONE:
		/* The context menu closed (chosen from or not). */
		fm_log("CONTEXT-MENU done");
		if (window->menu != NULL)
			window->menu->context_done = 1;
		break;
	case KL_WINDOW_DROP_ENTER:
	case KL_WINDOW_DROP_MOTION:
	case KL_WINDOW_DROP_LEAVE:
	case KL_WINDOW_DROP:
	case KL_WINDOW_DROP_ACTION:
	case KL_WINDOW_DRAG_DONE:
	case KL_WINDOW_CONTROL_DROP:
		window_drop(window, event);
		break;
	default:
		break;
	}
}

/*
 * Turns the wheel's scrolling into the window's pixels, and a touch pad's
 * fingers' moves and lift into touch inputs (only the vertical axis
 * scrolls the window).
 */
static void
window_axis(
	struct fm_window *window,
	const struct kl_window_event *event)
{
	struct fm_event *input;
	uint32_t time;
	double units;

	/* The compositor's time, in milliseconds. */
	time = (uint32_t)(event->time_us / 1000U);

	/* The fingers lifted: the scroll flies on (ws090-p019). */
	if (event->kind == KL_WINDOW_AXIS_STOP) {
		window_pad_push(window, FM_TOUCH_PAD_STOP, time, 0.0);
		return;
	}

	/* The vertical distance, in the compositor's units. */
	if (event->dy == 0.0)
		return;
	units = event->dy / WINDOW_KL_SCROLL_SCALE;

	/* A touch pad's fingers scroll as fingers do, unrounded. */
	if (event->axis_source == KL_AXIS_SOURCE_FINGER) {
		window_pad_push(window, FM_TOUCH_PAD, time, units * WINDOW_SCROLL_SCALE);
		return;
	}

	/* A wheel's whole units, scaled to the window's pixels. */
	input = fm_window_push(window, FM_EVENT_AXIS);
	if (input == NULL)
		return;
	input->scroll = (int)units * WINDOW_SCROLL_SCALE;
}

/* Turns drag and drop's inputs into the drop events (dnd.c's side of ui-drag.c). */
static void
window_drop(
	struct fm_window *window,
	const struct kl_window_event *event)
{
	struct fm_event *input;

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_DROP_ENTER:
		/* A drag of file names came over the window (whether it is the window's own in pressed). */
		window->drop_over = 1;
		input = fm_window_push(window, FM_EVENT_DROP_ENTER);
		if (input == NULL)
			return;
		input->x = (int)event->x;
		input->y = (int)event->y;
		input->pressed = event->pressed;
		input->focused = 1;
		break;
	case KL_WINDOW_DROP_MOTION:
		/* Where it is. */
		input = fm_window_push(window, FM_EVENT_DROP_MOTION);
		if (input == NULL)
			return;
		input->x = (int)event->x;
		input->y = (int)event->y;
		break;
	case KL_WINDOW_DROP_LEAVE:
		window->drop_over = 0;
		(void)fm_window_push(window, FM_EVENT_DROP_LEAVE);
		break;
	case KL_WINDOW_DROP:
		/* Dropped: the main loop reads the names and finishes; a context menu now answers the drop. */
		window->drop_serial = event->serial;
		(void)fm_window_push(window, FM_EVENT_DROP);
		break;
	case KL_WINDOW_DROP_ACTION:
		/* The compositor's choice of action for the drag over the window. */
		input = fm_window_push(window, FM_EVENT_DROP_ACTION);
		if (input != NULL)
			input->action = event->code;
		break;
	case KL_WINDOW_CONTROL_DROP:
		/* The part of the titlebar's path the drag is over (control 0 for none). */
		input = fm_window_push(window, FM_EVENT_DROP_PART);
		if (input == NULL)
			return;
		input->action = (uint32_t)event->id;
		input->button = (uint32_t)event->begin;
		break;
	case KL_WINDOW_DRAG_DONE:
		/* The window's own drag ended, dropped somewhere or not. */
		window->dragging = 0;
		fm_log("DND end dropped=%u action=%d", event->code, (int)event->begin);
		input = fm_window_push(window, FM_EVENT_DRAG_DONE);
		if (input != NULL)
			input->pressed = (int)event->code;
		break;
	default:
		break;
	}
}

/* Logs the desktop surface's place and size, as the compositor configured it. */
static void
window_desktop_log(
	struct fm_window *window)
{
	int32_t x;
	int32_t y;
	int error;

	/* Its place. */
	x = 0;
	y = 0;
	error = kl_window_desktop_place(window->kui, &x, &y);
	if (error != 0)
		return;

	/* The log line the tests read. */
	fm_log("DESKTOP configure x=%d y=%d width=%u height=%u", (int)x, (int)y, window->width, window->height);
}

/*
 * Queues a touch pad's scrolling among the touch inputs (ws090-p019), in
 * their order: the move (as a wheel scrolls) or the fingers' lift, at the
 * compositor's time; a full queue drops it.
 */
static void
window_pad_push(
	struct fm_window *window,
	unsigned type,
	uint32_t time,
	double distance)
{
	struct fm_touch_event *event;

	/* A full queue drops the input. */
	if (window->touch_count >= FM_WINDOW_TOUCHES)
		return;

	/* The input, after the ones before it; the main loop finds the area under the pointer. */
	event = &window->touches[window->touch_count];
	window->touch_count++;
	memset(event, 0, sizeof(*event));
	event->type = type;
	event->y = (float)distance;
	event->time = time;
	event->area = FM_TOUCH_OTHER;
	event->arrival = fm_touch_clock();
}

/* Queues a finger's input with the time the window read it; a full queue drops it. */
static void
window_touch_push(
	struct fm_window *window,
	unsigned type,
	const struct kl_window_event *event)
{
	struct fm_touch_event *kept;

	/* A full queue drops the input (the fingers are far ahead of the program). */
	if (window->touch_count >= FM_WINDOW_TOUCHES)
		return;

	/* The input, after the ones before it (what is under a finger is the main loop's to find); a down's serial, a cancel's every finger. */
	kept = &window->touches[window->touch_count];
	window->touch_count++;
	memset(kept, 0, sizeof(*kept));
	kept->type = type;
	kept->id = event->id;
	if (type == FM_TOUCH_CANCEL)
		kept->id = -1;
	kept->x = (float)event->x;
	kept->y = (float)event->y;
	kept->time = (uint32_t)(event->time_us / 1000U);
	if (type == FM_TOUCH_DOWN)
		kept->serial = event->serial;
	kept->area = FM_TOUCH_OTHER;
	kept->arrival = fm_touch_clock();
}

/* Turns libkeiland's modifier bits into the file manager's (FM_MOD_*). */
static uint32_t
window_modifiers(
	unsigned modifiers)
{
	uint32_t bits;

	/* Shift, Control, Alt and Super. */
	bits = 0U;
	if ((modifiers & KL_MOD_SHIFT) != 0U)
		bits |= FM_MOD_SHIFT;
	if ((modifiers & KL_MOD_CTRL) != 0U)
		bits |= FM_MOD_CTRL;
	if ((modifiers & KL_MOD_ALT) != 0U)
		bits |= FM_MOD_ALT;
	if ((modifiers & KL_MOD_SUPER) != 0U)
		bits |= FM_MOD_SUPER;

	/* Reports them. */
	return bits;
}

/* Queues an input method's text, the bytes it deletes before the caret, or the text it composes (ws090-p022). */
static void
window_text(
	struct fm_window *window,
	const struct kl_window_event *event)
{
	struct fm_event *input;
	unsigned type;

	/* Its kind. */
	type = FM_EVENT_TEXT;
	if (event->kind == KL_WINDOW_TEXT_DELETE)
		type = FM_EVENT_TEXT_DELETE;
	else if (event->kind == KL_WINDOW_TEXT_PREEDIT)
		type = FM_EVENT_PREEDIT;

	/* Queued with the other input, in its place. */
	input = fm_window_push(window, type);
	if (input == NULL)
		return;
	(void)snprintf(input->text, sizeof(input->text), "%s", event->text);
	input->before = event->before;
}
