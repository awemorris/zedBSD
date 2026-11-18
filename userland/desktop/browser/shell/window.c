/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window of browser: a window of libkeiland's application (WS131
 * p025, kl_app; its own xdg-shell toplevel and seat before), whose surface
 * the drawing thread draws on with its own Vulkan (KL_PRESENT_NONE).  The
 * application waits for the compositor; the view's wake, called from the
 * view's threads, and the drawing thread's post an event to the
 * application (kl_app_post), which ends the wait.
 *
 * The application queues the window's input; this file turns it into the
 * shell_event values the main loop reads: the pointer's moves (the last of
 * a run of moves stands for them all), buttons, wheel and leaving, the keys
 * pressed, repeated and let go (a held key repeats within kl_app_dispatch,
 * after a release read with it, BUG-111), and the keyboard's focus coming
 * and going.  The desktop's appearance changing is noted for the main
 * loop (themed).  The fingers and a touch pad's scrolling are queued for
 * touch.c (ws081-p006, ws090-p019), and what is done with the titlebar's
 * controls goes to the titlebar (titlebar.c).
 */

#include "shell/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * How the wheel's distance is scaled: libkeiland gives 4 pixels for each
 * of the compositor's scroll units, the browser scrolls 3 (15 units a notch,
 * 45 pixels).
 */
#define WINDOW_SCROLL_UNIT	4.0
#define WINDOW_SCROLL_SCALE	3

static void window_event(struct shell_window *window, const struct kl_window_event *input);
static void window_button(struct shell_window *window, const struct kl_window_event *input);
static void window_axis(struct shell_window *window, const struct kl_window_event *input);
static void window_key(struct shell_window *window, const struct kl_window_event *input);
static void window_focus(struct shell_window *window, const struct kl_window_event *input);
static void window_text(struct shell_window *window, int type, const struct kl_window_event *input);
static struct shell_event *window_push(struct shell_window *window, int type);
static void window_open(struct shell_window *window, const struct kl_window_event *input);
static void window_push_motion(struct shell_window *window);
static void window_pad_push(struct shell_window *window, unsigned type, const struct kl_window_event *input);
static void window_touch_push(struct shell_window *window, unsigned type, const struct kl_window_event *input);
static uint32_t window_modifiers(unsigned modifiers);
static void window_language(struct shell_window *window, const char *language);

/*
 * Connects to the compositor: the application the window will be made on
 * (it knows the program's data directory too).
 *
 * Returns 0, or -1 with errno set.
 */
int
shell_window_connect(
	struct shell_window *window,
	const char *display)
{
	struct kl_app_options app_options;

	/* Nothing held yet. */
	memset(window, 0, sizeof(*window));

	/* The application: the connection to the compositor. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.display = display;
	app_options.application = SHELL_APPLICATION_ID;
	window->app = kl_app_open(&app_options);
	if (window->app == NULL)
		return -1;

	/* Succeeded: the application is open. */
	return 0;
}

/*
 * Makes the toplevel window of the connected application: a size with a
 * title, shown with a way (KL_PRESENT_NONE for the presenter's own Vulkan,
 * KL_PRESENT_SHM for the CPU presenter).
 *
 * Returns 0 once the first configure is acknowledged, or -1 with errno set.
 */
int
shell_window_open(
	struct shell_window *window,
	uint32_t width,
	uint32_t height,
	const char *title,
	unsigned present)
{
	struct kl_window_options options;
	struct kl_app_event event;
	int taken;

	/* Its window, the size asked for until the compositor gives one. */
	memset(&options, 0, sizeof(options));
	options.title = title;
	options.width = width;
	options.height = height;
	options.present = present;
	window->kui = kl_app_window_create(window->app, &options);
	if (window->kui == NULL)
		return -1;

	/* The window's size as the first configure left it. */
	kl_window_size(window->kui, &window->width, &window->height);

	/* What the window's making left queued (its size is already known). */
	for (;;) {
		taken = kl_app_take(window->app, &event);
		if (taken == 0)
			break;
		if (event.kind == KL_APP_WINDOW)
			window_event(window, &event.input);
		if (event.kind == KL_APP_THEME)
			window->themed = 1;
		if (event.kind == KL_APP_LANGUAGE)
			window_language(window, event.language);
	}

	/* Succeeded: the window can be drawn into. */
	window->resized = 0;
	return 0;
}

/*
 * Waits up to a timeout (milliseconds, -1 for ever; a held key's repeat
 * or input already queued waits less) for the compositor's events and the
 * view's wake, and takes what happened: the window's input for
 * shell_window_take and the titlebar's for the titlebar (a wake needs
 * nothing more: the main loop runs the view after each dispatch).
 *
 * Returns 0, or -1 when the connection is broken.
 */
int
shell_window_dispatch(
	struct shell_window *window,
	int timeout)
{
	struct kl_app_event event;
	int status;
	int taken;

	/* Nothing is waited for while input is already queued. */
	if (window->event_count != 0U || window->touch_count != 0U)
		timeout = 0;

	/* The compositor and the view's wake, together. */
	status = kl_app_dispatch(window->app, timeout);
	if (status != 0)
		return -1;

	/* What happened, in its order: the window's input (the view's wakes carry nothing). */
	for (;;) {
		taken = kl_app_take(window->app, &event);
		if (taken == 0)
			break;

		/* The window's input. */
		if (event.kind == KL_APP_WINDOW && event.window == window->kui)
			window_event(window, &event.input);

		/* The desktop's appearance changed: the main loop gives the view its scheme. */
		if (event.kind == KL_APP_THEME)
			window->themed = 1;

		/* The desktop's language changed: the main loop words the shell's question in it. */
		if (event.kind == KL_APP_LANGUAGE)
			window_language(window, event.language);

		/* The diagnostic clock's tick, timed as it is taken. */
		if (event.kind == KL_APP_POST &&
		    event.code == SHELL_POST_TICK &&
		    window->latency != NULL)
			shell_latency_take(window->latency);
	}

	/* Succeeded: the events so far have run. */
	return 0;
}

/*
 * Takes the oldest queued input; zero when there is none.
 */
int
shell_window_take(
	struct shell_window *window,
	struct shell_event *event)
{
	/* An empty queue. */
	if (window->event_count == 0U)
		return 0;

	/* The oldest input, and the queue moves on. */
	*event = window->events[window->event_first];
	window->event_first = (window->event_first + 1U) % SHELL_WINDOW_EVENTS;
	window->event_count--;

	/* Succeeded: one input taken. */
	return 1;
}

/*
 * Changes the title the compositor shows.
 */
void
shell_window_title(
	struct shell_window *window,
	const char *title)
{
	/* The request, sent with the next dispatch. */
	kl_window_set_title(window->kui, title);
}

/*
 * Closes the window and keeps the application, which a window of another
 * way (shell_window_open) can be made on.  What the window had queued is
 * gone with it.
 */
void
shell_window_drop(
	struct shell_window *window)
{
	/* The window. */
	if (window->kui != NULL)
		kl_window_close(window->kui);
	window->kui = NULL;

	/* What it queued, its size and the titlebar that heard it. */
	window->width = 0U;
	window->height = 0U;
	window->resized = 0;
	window->closed = 0;
	window->modifiers = 0U;
	window->titlebar = NULL;
	window->event_first = 0U;
	window->event_count = 0U;
	window->touch_count = 0U;
}

/*
 * Closes the window and disconnects.
 */
void
shell_window_close(
	struct shell_window *window)
{
	/* The window, then the application and its connection. */
	if (window->kui != NULL)
		kl_window_close(window->kui);
	if (window->app != NULL)
		kl_app_close(window->app);
	memset(window, 0, sizeof(*window));
}

/*
 * Returns a monotonic time in milliseconds.
 */
uint64_t
shell_clock(void)
{
	/* libkeiland's clock, in milliseconds. */
	return kl_clock_us() / 1000U;
}

/* Notes the desktop's language changed to one, for the main loop (the last change told is the language now). */
static void
window_language(
	struct shell_window *window,
	const char *language)
{
	/* The language's name and the mark. */
	(void)snprintf(window->language, sizeof(window->language), "%s", language);
	window->languaged = 1;
}

/* Turns one input of the window into the shell's. */
static void
window_event(
	struct shell_window *window,
	const struct kl_window_event *input)
{
	/* Every input carries the modifiers held. */
	window->modifiers = window_modifiers(input->modifiers);

	/* What it is. */
	switch (input->kind) {
	case KL_WINDOW_MOTION:
		/* The place the buttons are pressed at, and the move. */
		window->pointer_x = (int)input->x;
		window->pointer_y = (int)input->y;
		window_push_motion(window);
		break;
	case KL_WINDOW_LEAVE:
		(void)window_push(window, SHELL_EVENT_LEAVE);
		break;
	case KL_WINDOW_BUTTON:
		window_button(window, input);
		break;
	case KL_WINDOW_AXIS:
		window_axis(window, input);
		break;
	case KL_WINDOW_AXIS_STOP:
		/* The touch pad's fingers lift: the page flies on (ws090-p019). */
		window_pad_push(window, SHELL_TOUCH_PAD_STOP, input);
		break;
	case KL_WINDOW_KEY:
		window_key(window, input);
		break;
	case KL_WINDOW_FOCUS:
		window_focus(window, input);
		break;
	case KL_WINDOW_TEXT_COMMIT:
		window_text(window, SHELL_EVENT_TEXT_COMMIT, input);
		break;
	case KL_WINDOW_TEXT_PREEDIT:
		window_text(window, SHELL_EVENT_TEXT_PREEDIT, input);
		break;
	case KL_WINDOW_TEXT_DELETE:
		window_text(window, SHELL_EVENT_TEXT_DELETE, input);
		break;
	case KL_WINDOW_TOUCH_DOWN:
		window_touch_push(window, SHELL_TOUCH_DOWN, input);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		window_touch_push(window, SHELL_TOUCH_MOTION, input);
		break;
	case KL_WINDOW_TOUCH_UP:
		window_touch_push(window, SHELL_TOUCH_UP, input);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		window_touch_push(window, SHELL_TOUCH_CANCEL, input);
		break;
	case KL_WINDOW_RESIZE:
		/* The size the compositor gave, drawn at from the next frame. */
		kl_window_size(window->kui, &window->width, &window->height);
		window->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		/* The main loop ends the program. */
		window->closed = 1;
		break;
	case KL_WINDOW_POST:
		/* The URLs a later start of the program handed over, for the main loop (a full queue drops them). */
		if (input->code == SHELL_POST_OPEN)
			window_open(window, input);

		/* The drawing thread's configure, acknowledged at once (it waits to present the first frame of its size). */
		if (input->code == SHELL_POST_ACK && window->render != NULL)
			shell_render_ack(window->render);
		break;
	case KL_WINDOW_ACTION:
	case KL_WINDOW_CONTROL_DONE:
		/* A titlebar's control, for the main loop (titlebar.c). */
		if (window->titlebar != NULL)
			shell_titlebar_post(window->titlebar, input);
		break;
	default:
		break;
	}
}

/* A pointer button is pressed or let go, at the pointer's place. */
static void
window_button(
	struct shell_window *window,
	const struct kl_window_event *input)
{
	struct shell_event *event;

	/* The pointer's place. */
	window->pointer_x = (int)input->x;
	window->pointer_y = (int)input->y;

	/* The input; a full queue drops it. */
	event = window_push(window, SHELL_EVENT_BUTTON);
	if (event == NULL)
		return;

	/* The button and whether it went down. */
	event->button = input->code;
	event->pressed = 0;
	if (input->pressed)
		event->pressed = 1;
}

/* The wheel turns, or a touch pad's fingers scroll: scrolling in pixels, down or right. */
static void
window_axis(
	struct shell_window *window,
	const struct kl_window_event *input)
{
	struct shell_event *event;
	int units;

	/* A touch pad's fingers scroll the page as fingers do, with libkeiland's scroller (ws090-p019), unrounded. */
	if (input->axis_source == KL_AXIS_SOURCE_FINGER && input->dy != 0.0) {
		window_pad_push(window, SHELL_TOUCH_PAD, input);
		return;
	}

	/* The vertical distance as a wheel's, scaled to the window's pixels. */
	if (input->dy != 0.0) {
		event = window_push(window, SHELL_EVENT_SCROLL);
		if (event == NULL)
			return;
		units = (int)(input->dy / WINDOW_SCROLL_UNIT);
		event->scroll = units * WINDOW_SCROLL_SCALE;
	}

	/* And the horizontal one. */
	if (input->dx != 0.0) {
		event = window_push(window, SHELL_EVENT_SCROLL);
		if (event == NULL)
			return;
		units = (int)(input->dx / WINDOW_SCROLL_UNIT);
		event->scroll_x = units * WINDOW_SCROLL_SCALE;
	}
}

/* A key is pressed, repeated or let go. */
static void
window_key(
	struct shell_window *window,
	const struct kl_window_event *input)
{
	struct shell_event *event;

	/* The input; a full queue drops it. */
	event = window_push(window, SHELL_EVENT_KEY);
	if (event == NULL)
		return;

	/* The key, the character it types when the backend says (a press only), whether it went down, and whether it is a held key's repeat. */
	event->key = input->code;
	if (input->pressed) {
		memcpy(event->text, input->text, sizeof(event->text));
		event->text[sizeof(event->text) - 1U] = '\0';
	}
	event->pressed = 0;
	if (input->pressed)
		event->pressed = 1;
	event->repeat = 0;
	if (input->repeated)
		event->repeat = 1;
}

/* The keyboard's focus comes or goes; without it no modifier is held. */
static void
window_focus(
	struct shell_window *window,
	const struct kl_window_event *input)
{
	struct shell_event *event;

	/* Leaving forgets the modifiers. */
	if (!input->pressed)
		window->modifiers = 0U;

	/* The input; a full queue drops it. */
	event = window_push(window, SHELL_EVENT_FOCUS);
	if (event == NULL)
		return;

	/* Whether it came. */
	event->pressed = 0;
	if (input->pressed)
		event->pressed = 1;
}

/* Queues an input method's text, its composing or its deletion for the page (ws090-p025). */
static void
window_text(
	struct shell_window *window,
	int type,
	const struct kl_window_event *input)
{
	struct shell_event *event;

	/* The input; a full queue drops it. */
	event = window_push(window, type);
	if (event == NULL)
		return;

	/* The text with its cursor, and the bytes to delete. */
	memcpy(event->text, input->text, sizeof(event->text));
	event->text[sizeof(event->text) - 1U] = '\0';
	event->begin = input->begin;
	event->end = input->end;
	event->before = input->before;
	event->after = input->after;
}

/* Queues a new input of a kind with the modifiers held; NULL when the queue is full. */
static struct shell_event *
window_push(
	struct shell_window *window,
	int type)
{
	struct shell_event *event;
	unsigned slot;

	/* A full queue drops the input (the user is far ahead of the program). */
	if (window->event_count == SHELL_WINDOW_EVENTS)
		return NULL;

	/* The slot after the last one queued. */
	slot = (window->event_first + window->event_count) % SHELL_WINDOW_EVENTS;
	window->event_count++;

	/* The input, with what every input carries: the modifiers and the pointer's place. */
	event = &window->events[slot];
	memset(event, 0, sizeof(*event));
	event->type = type;
	event->modifiers = window->modifiers;
	event->x = window->pointer_x;
	event->y = window->pointer_y;

	/* Reports the queued input for its details. */
	return event;
}

/* Queues the URLs a later start handed over; a full queue drops them with their list. */
static void
window_open(
	struct shell_window *window,
	const struct kl_window_event *input)
{
	struct shell_event *event;

	/* The input; a full queue drops it. */
	event = window_push(window, SHELL_EVENT_OPEN);
	if (event == NULL) {
		free(input->data);
		return;
	}

	/* The list, now the event's. */
	event->data = input->data;
}

/*
 * Queues a move of the pointer to its place now; a move queued last is
 * moved instead (only the latest place of a run of moves matters).
 */
static void
window_push_motion(
	struct shell_window *window)
{
	struct shell_event *last;
	unsigned slot;

	/* The input queued last, when it is a move, takes the new place. */
	if (window->event_count != 0U) {
		slot = (window->event_first + window->event_count - 1U) % SHELL_WINDOW_EVENTS;
		last = &window->events[slot];
		if (last->type == SHELL_EVENT_MOTION) {
			last->x = window->pointer_x;
			last->y = window->pointer_y;
			last->modifiers = window->modifiers;
			return;
		}
	}

	/* Otherwise a new move; a full queue drops it. */
	(void)window_push(window, SHELL_EVENT_MOTION);
}

/*
 * Queues a touch pad's scrolling among the touch inputs (ws090-p019): a
 * move (as the wheel scrolls, unrounded) or the fingers' lift, at the
 * compositor's time; a full queue drops it.
 */
static void
window_pad_push(
	struct shell_window *window,
	unsigned type,
	const struct kl_window_event *input)
{
	struct shell_touch_event *event;

	/* A full queue drops the input. */
	if (window->touch_count >= SHELL_WINDOW_TOUCHES)
		return;

	/* The input, after the ones before it: its time and when it was read. */
	event = &window->touches[window->touch_count];
	window->touch_count++;
	memset(event, 0, sizeof(*event));
	event->type = type;
	event->time = (uint32_t)(input->time_us / 1000U);
	event->arrival = input->arrival_us;

	/* A move's distance at the browser's scale. */
	if (type == SHELL_TOUCH_PAD)
		event->y = (float)(input->dy / WINDOW_SCROLL_UNIT * (double)WINDOW_SCROLL_SCALE);
}

/* Queues a touch input with its time and when it was read; a full queue drops it. */
static void
window_touch_push(
	struct shell_window *window,
	unsigned type,
	const struct kl_window_event *input)
{
	struct shell_touch_event *event;

	/* A full queue drops the input (the fingers are far ahead of the program). */
	if (window->touch_count >= SHELL_WINDOW_TOUCHES)
		return;

	/* The input, after the ones before it: the compositor's time in milliseconds (the low 32 bits). */
	event = &window->touches[window->touch_count];
	window->touch_count++;
	memset(event, 0, sizeof(*event));
	event->type = type;
	event->id = input->id;
	event->x = (float)input->x;
	event->y = (float)input->y;
	event->time = (uint32_t)(input->time_us / 1000U);
	event->arrival = input->arrival_us;

	/* A cancel is every finger's, with no place or time. */
	if (type == SHELL_TOUCH_CANCEL) {
		event->id = -1;
		event->x = 0.0f;
		event->y = 0.0f;
		event->time = 0U;
	}
}

/* Turns libkeiland's modifier bits into the shell's. */
static uint32_t
window_modifiers(
	unsigned modifiers)
{
	uint32_t bits;

	/* Shift, Control, Alt and the logo key. */
	bits = 0U;
	if ((modifiers & KL_MOD_SHIFT) != 0U)
		bits |= SHELL_MOD_SHIFT;
	if ((modifiers & KL_MOD_CTRL) != 0U)
		bits |= SHELL_MOD_CTRL;
	if ((modifiers & KL_MOD_ALT) != 0U)
		bits |= SHELL_MOD_ALT;
	if ((modifiers & KL_MOD_SUPER) != 0U)
		bits |= SHELL_MOD_META;

	/* Reports them. */
	return bits;
}
