/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Wayland window of Notes: a window of libkeiland's application
 * (WS131 p018, kl_app; ws090-p011 before), whose surface Notes draws on
 * with its own Vulkan (KL_PRESENT_NONE), and the queue of input events the
 * main loop draws from.
 *
 * The pointer's left button draws: its press, the motions while it is
 * held, and its release become NOTES_INPUT_DOWN, _MOTION and _UP events
 * from NOTES_SOURCE_POINTER with a fixed pressure, at the compositor's
 * time.  Every motion is kept, not only the last of a frame, because each
 * one is a sample of the stroke.  A pen tablet's tools come as the
 * window's KL_WINDOW_TABLET_* inputs (libkeiland's tablet protocol, taken
 * with kl_window_accept_tablet) and feed the same queue (tablet.c) with
 * the pen's own pressure and tilt; a pen without the tablet protocol
 * arrives as the pointer.  Keys are queued as pressed (Notes turns the
 * window's repeat off).  The menus' choices come as KL_WINDOW_ACTION
 * inputs (menu.c).  ws081-p013: the touch screen's events queue for
 * touch.c.  The compositor decorates the window (its title bar, ws114-p008).
 *
 * ws175-p008: while the text box is open (notes_window_box), the pointer's
 * inputs, the keys -- held keys repeating -- and an input method's text
 * also queue as they came for the box (box.c), and the keys reach only it.
 */

#include "app.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The evdev code of the left button. */
#define WINDOW_BUTTON_LEFT	0x110U

/* The evdev codes of S and W, the keys of Ctrl+S and Ctrl+W (ws177-p012). */
#define WINDOW_KEY_S		31U
#define WINDOW_KEY_W		17U

static void window_take(struct notes_window *window);
static void window_event(struct notes_window *window, const struct kl_window_event *event);
static void window_button(struct notes_window *window, const struct kl_window_event *event);
static void window_key(struct notes_window *window, const struct kl_window_event *event);
static void window_pointer_event(struct notes_window *window, unsigned kind, const struct kl_window_event *event);
static void window_touch_push(struct notes_window *window, unsigned type, const struct kl_window_event *event);
static void window_box_push(struct notes_window *window, const struct kl_window_event *event);
static int window_box_shortcut(const struct kl_window_event *event);
static int window_box_touch(struct notes_window *window, const struct kl_window_event *event);
static uint32_t window_modifiers(unsigned modifiers);

/*
 * Connects to the compositor and makes a toplevel window of a size,
 * mapped fullscreen when asked.  Returns 0 once the first configure is
 * acknowledged, or -1 with errno set.
 */
int
notes_window_open(
	struct notes_window *window,
	uint32_t width,
	uint32_t height,
	int fullscreen)
{
	struct kl_window_options options;
	struct kl_app_options app_options;
	int error;

	/* Nothing held yet. */
	memset(window, 0, sizeof(*window));

	/* The application: the connection, and the identity the gesture finds Notes by. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "notes";
	window->app = kl_app_open(&app_options);
	if (window->app == NULL)
		return -1;

	/* Its window: the title, the size and the full screen asked for. */
	memset(&options, 0, sizeof(options));
	options.title = "Notes";
	options.width = width;
	options.height = height;
	options.present = KL_PRESENT_NONE;
	options.fullscreen = fullscreen;
	window->kui = kl_app_window_create(window->app, &options);
	if (window->kui == NULL)
		return -1;

	/* The connection and the toplevel the file chooser opens on, and the size and the full screen given. */
	window->display = kl_app_display(window->app);
	window->toplevel = kl_window_toplevel(window->kui);
	kl_window_size(window->kui, &window->width, &window->height);
	window->fullscreen = kl_window_fullscreen(window->kui);

	/* Each key once (a held key does not repeat), and the pen with its pressure and tilt where the compositor has the tablet protocol. */
	(void)kl_window_set_repeat(window->kui, 0);
	error = kl_window_accept_tablet(window->kui);
	if (error == 0) {
		printf("NOTES TABLET seat\n");
		fflush(stdout);
	}

	/* What the window's making left queued (its size is already known). */
	window_take(window);
	window->resized = 0;

	/* Succeeded: the window can be drawn into. */
	return 0;
}

/*
 * Waits for the compositor's events, at most a timeout in milliseconds
 * (-1: for ever), runs them and takes the window's input.
 *
 * Returns 0, or -1 when the connection is broken.
 */
int
notes_window_dispatch(
	struct notes_window *window,
	int timeout)
{
	int status;

	/* Nothing to wait for when events are already queued for the main loop. */
	if (window->input_count != 0U ||
	    window->key_count != 0U ||
	    window->action_count != 0U ||
	    window->box_count != 0U)
		timeout = 0;

	/* The compositor's events. */
	status = kl_app_dispatch(window->app, timeout);
	if (status != 0)
		return -1;

	/* The input they queued. */
	window_take(window);

	/* Succeeded: the events so far have run. */
	return 0;
}

/*
 * Destroys the window's objects and disconnects.
 */
void
notes_window_close(
	struct notes_window *window)
{
	/* The menus, before the window they are shown on. */
	if (window->kui != NULL)
		notes_menu_close(window);

	/* The window, then the application and its connection. */
	if (window->kui != NULL)
		kl_window_close(window->kui);
	if (window->app != NULL)
		kl_app_close(window->app);
	memset(window, 0, sizeof(*window));
}

/*
 * Queues an input event for the main loop.
 *
 * The pointer's events come through here, and so do a pen tablet's.  A
 * full queue drops a motion but keeps room for the contact's end, so that
 * a stroke is always finished.
 */
void
notes_window_input(
	struct notes_window *window,
	const struct notes_input *input)
{
	/* The last slot is kept for an end of contact. */
	if (window->input_count + 1U >= NOTES_INPUTS && input->kind != NOTES_INPUT_UP)
		return;
	if (window->input_count >= NOTES_INPUTS)
		return;

	/* Succeeded: queued after the ones before it. */
	window->inputs[window->input_count] = *input;
	window->input_count++;
}

/*
 * Sets the title the compositor shows.
 */
void
notes_window_set_title(
	struct notes_window *window,
	const char *title)
{
	/* The toplevel's title. */
	kl_window_set_title(window->kui, title);
}

/*
 * Asks the compositor to make the window fullscreen, or to end it; the
 * configure that follows says what it did.
 */
void
notes_window_set_fullscreen(
	struct notes_window *window,
	int fullscreen)
{
	/* On the default output, or back to a window. */
	kl_window_set_fullscreen(window->kui, fullscreen);
}

/*
 * Opens the window's input to the text box, or closes it (ws175-p008):
 * while it is open a held key repeats and the box's inputs queue for it.
 */
void
notes_window_box(
	struct notes_window *window,
	int open)
{
	/* Already so. */
	if (window->box_open == open)
		return;

	/* The keys repeat for the box's typing only; the box's queue starts empty, with no finger of its own. */
	window->box_open = open;
	window->box_count = 0;
	window->box_touching = 0;
	(void)kl_window_set_repeat(window->kui, open);

	/* A closed box takes no input method's text. */
	if (!open)
		kl_window_text_input(window->kui, 0);
}

/* Tells the window where the text box is, for the fingers that go down on it (ws177-p012). */
void
notes_window_box_rect(
	struct notes_window *window,
	const struct kl_rect *rect)
{
	/* The rectangle in the window. */
	window->box_rect = *rect;
}

/*
 * Returns a monotonic time in milliseconds (0 when the clock cannot be read).
 */
uint64_t
notes_clock(void)
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

/* Takes every input the application queued for the window, and its full screen as the compositor left it. */
static void
window_take(
	struct notes_window *window)
{
	struct kl_app_event event;
	int taken;

	/* Each input, oldest first (the desktop's appearance is main.c's own watch). */
	for (;;) {
		taken = kl_app_take(window->app, &event);
		if (taken == 0)
			break;
		if (event.kind == KL_APP_WINDOW && event.window == window->kui)
			window_event(window, &event.input);
	}

	/* Whether the compositor made the window fullscreen. */
	window->fullscreen = kl_window_fullscreen(window->kui);
}

/* Turns one input of the window into Notes'. */
static void
window_event(
	struct notes_window *window,
	const struct kl_window_event *event)
{
	int shortcut;
	int owned;

	/* Every input carries the modifiers held. */
	window->modifiers = window_modifiers(event->modifiers);

	/*
	 * The text box's inputs while it is open: the keys and the text are
	 * only its own (ws175-p008), but Ctrl+S and Ctrl+W, which save and
	 * close (they arrive only when no System Menu runs them, ws177-p012).
	 */
	if (window->box_open) {
		shortcut = window_box_shortcut(event);
		switch (event->kind) {
		case KL_WINDOW_KEY:
			if (shortcut)
				break;
			window_box_push(window, event);
			return;
		case KL_WINDOW_TEXT_COMMIT:
		case KL_WINDOW_TEXT_PREEDIT:
		case KL_WINDOW_TEXT_DELETE:
			window_box_push(window, event);
			return;
		case KL_WINDOW_MOTION:
		case KL_WINDOW_BUTTON:
			/* Not over the toolbar, whose buttons (the box's font, size, colour) leave the box the keyboard. */
			if (event->y >= (double)NOTES_TOOLBAR_HEIGHT)
				window_box_push(window, event);
			break;
		case KL_WINDOW_LEAVE:
			window_box_push(window, event);
			break;
		case KL_WINDOW_TOUCH_DOWN:
		case KL_WINDOW_TOUCH_MOTION:
		case KL_WINDOW_TOUCH_UP:
		case KL_WINDOW_TOUCH_CANCEL:
			/* A finger on the box is the box's alone (ws177-p012). */
			owned = window_box_touch(window, event);
			if (owned)
				return;
			break;
		default:
			break;
		}
	}

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		/* The pointer's place; while the button is held, a sample of the contact. */
		window->pointer_x = (float)event->x;
		window->pointer_y = (float)event->y;
		if (window->pointer_down)
			window_pointer_event(window, NOTES_INPUT_MOTION, event);
		break;
	case KL_WINDOW_BUTTON:
		window_button(window, event);
		break;
	case KL_WINDOW_KEY:
		window_key(window, event);
		break;
	case KL_WINDOW_TOUCH_DOWN:
		window_touch_push(window, NOTES_TOUCH_DOWN, event);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		window_touch_push(window, NOTES_TOUCH_MOTION, event);
		break;
	case KL_WINDOW_TOUCH_UP:
		window_touch_push(window, NOTES_TOUCH_UP, event);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		window_touch_push(window, NOTES_TOUCH_CANCEL, event);
		break;
	case KL_WINDOW_TABLET_DOWN:
	case KL_WINDOW_TABLET_MOTION:
	case KL_WINDOW_TABLET_UP:
	case KL_WINDOW_TABLET_HOVER:
	case KL_WINDOW_TABLET_LEAVE:
		/* A pen tablet's tool (tablet.c). */
		notes_tablet_input(window, event);
		break;
	case KL_WINDOW_ACTION:
		/* A menu's item, for the main loop (menu.c). */
		notes_menu_chosen(window, event);
		break;
	case KL_WINDOW_RESIZE:
		/* The size the compositor gave, drawn at from the next frame. */
		kl_window_size(window->kui, &window->width, &window->height);
		window->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		/* The main loop ends Notes. */
		window->closed = 1;
		break;
	default:
		break;
	}
}

/* The left button starts and ends a contact. */
static void
window_button(
	struct notes_window *window,
	const struct kl_window_event *event)
{
	/* Only the left button draws. */
	if (event->code != WINDOW_BUTTON_LEFT)
		return;

	/* A press starts the contact, a release ends it. */
	if (event->pressed) {
		window->pointer_down = 1;
		window_pointer_event(window, NOTES_INPUT_DOWN, event);
	} else if (window->pointer_down) {
		window->pointer_down = 0;
		window_pointer_event(window, NOTES_INPUT_UP, event);
	}
}

/* Queues a pressed key with the modifiers held; releases and a held key's repeats do nothing. */
static void
window_key(
	struct notes_window *window,
	const struct kl_window_event *event)
{
	/* Only first presses, while the queue has room. */
	if (!event->pressed || event->repeated)
		return;
	if (window->key_count >= NOTES_KEYS)
		return;

	/* Succeeded: queued. */
	window->keys[window->key_count].key = event->code;
	window->keys[window->key_count].modifiers = window->modifiers;
	window->key_count++;
}

/* Queues a pointer event at the pointer's place, with the pointer's fixed pressure and no tilt, at the compositor's time. */
static void
window_pointer_event(
	struct notes_window *window,
	unsigned kind,
	const struct kl_window_event *event)
{
	struct notes_input input;

	/* The event. */
	memset(&input, 0, sizeof(input));
	input.kind = kind;
	input.source = NOTES_SOURCE_POINTER;
	input.x = window->pointer_x;
	input.y = window->pointer_y;
	input.pressure = NOTES_POINTER_PRESSURE;
	input.time_ms = (uint32_t)(event->time_us / 1000U);

	/* Queues it like any other source's. */
	notes_window_input(window, &input);
}

/* Queues a touch input for touch.c; a full queue drops it. */
static void
window_touch_push(
	struct notes_window *window,
	unsigned type,
	const struct kl_window_event *event)
{
	struct notes_touch_event *kept;

	/* A full queue drops the input (the fingers are far ahead of the program). */
	if (window->touch_count >= NOTES_TOUCH_EVENTS)
		return;

	/* The input, after the ones before it: its time as the compositor's milliseconds, and when it was read. */
	kept = &window->touches[window->touch_count];
	window->touch_count++;
	memset(kept, 0, sizeof(*kept));
	kept->type = type;
	kept->id = event->id;
	kept->x = (float)event->x;
	kept->y = (float)event->y;
	kept->time = (uint32_t)(event->time_us / 1000U);
	kept->arrival = event->arrival_us;
}

/*
 * Tells whether a key is one of Notes' shortcuts that pass the open text
 * box: Ctrl+S (with Shift too) and Ctrl+W, pressed or released.
 */
static int
window_box_shortcut(
	const struct kl_window_event *event)
{
	/* Only keys with Control and without Alt or Super. */
	if (event->kind != KL_WINDOW_KEY)
		return 0;
	if ((event->modifiers & KL_MOD_CTRL) == 0U)
		return 0;
	if ((event->modifiers & (KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return 0;

	/* S saves (Shift: as), W closes. */
	if (event->code == WINDOW_KEY_S)
		return 1;
	if (event->code == WINDOW_KEY_W)
		return 1;

	/* Any other key is the box's. */
	return 0;
}

/*
 * Gives a finger's input to the text box when the finger went down on it
 * (the finger's later inputs follow it there); a cancel goes to the box
 * and to the page both.  Returns 1 when the input is the box's alone.
 */
static int
window_box_touch(
	struct notes_window *window,
	const struct kl_window_event *event)
{
	int inside;

	/* A finger down on the box's rectangle is the box's from now on. */
	if (event->kind == KL_WINDOW_TOUCH_DOWN) {
		inside = 0;
		if (event->x >= (double)window->box_rect.x &&
		    event->y >= (double)window->box_rect.y &&
		    event->x < (double)(window->box_rect.x + window->box_rect.width) &&
		    event->y < (double)(window->box_rect.y + window->box_rect.height))
			inside = 1;
		if (!inside || window->box_touching)
			return 0;
		window->box_touching = 1;
		window->box_touch_id = event->id;
		window_box_push(window, event);
		return 1;
	}

	/* A cancel ends the box's finger, and the page's fingers too. */
	if (event->kind == KL_WINDOW_TOUCH_CANCEL) {
		if (window->box_touching)
			window_box_push(window, event);
		window->box_touching = 0;
		return 0;
	}

	/* Another finger's input is the page's. */
	if (!window->box_touching || event->id != window->box_touch_id)
		return 0;

	/* The box's finger: lifted, it is no longer followed. */
	window_box_push(window, event);
	if (event->kind == KL_WINDOW_TOUCH_UP)
		window->box_touching = 0;

	/* Succeeded: the box's alone. */
	return 1;
}

/* Queues an input for the text box; a full queue drops it. */
static void
window_box_push(
	struct notes_window *window,
	const struct kl_window_event *event)
{
	/* The box is far behind: the input goes. */
	if (window->box_count >= NOTES_BOX_EVENTS)
		return;

	/* Succeeded: after the ones before it. */
	window->box_events[window->box_count] = *event;
	window->box_count++;
}

/* Turns libkeiland's modifier bits into wl_keyboard's, which the shortcuts read. */
static uint32_t
window_modifiers(
	unsigned modifiers)
{
	uint32_t bits;

	/* Shift, Control and Alt. */
	bits = 0U;
	if ((modifiers & KL_MOD_SHIFT) != 0U)
		bits |= NOTES_MODIFIER_SHIFT;
	if ((modifiers & KL_MOD_CTRL) != 0U)
		bits |= NOTES_MODIFIER_CONTROL;
	if ((modifiers & KL_MOD_ALT) != 0U)
		bits |= NOTES_MODIFIER_ALT;

	/* Reports them. */
	return bits;
}
