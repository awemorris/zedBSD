/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window mode of browser: a view (<browser/browser.h>) shown in a compositor
 * window, recording its drawing into the frames of the window's swapchain
 * on the shell's Vulkan device.  The view holds the page, its history, its
 * scroll, its timers, the network and the renderer; the shell holds the
 * window, the compositor's titlebar (back, forward, reload and the location,
 * whose URL can be edited) and the presenter (the swapchain), and
 * turns the window's input into the view's input (ws074-p056): the
 * pointer's moves, buttons, wheel and leaving, the keys with the DOM's
 * names (keys.c), and the keyboard's focus.  The view does what the input
 * means for the page (scrolling, links, the focus, the history's keys).
 * ws081-p006: the touch screen's fingers (touch.c) scroll the page with
 * inertia and stretch it past its ends (the view's placed scroll and
 * overscroll), and click with taps and long presses.
 * WS169 p005: a sign-in code that comes by mail is offered as a
 * notification, whose click types it into the page (mail.c).
 * The shell keeps its own shortcuts, which the page never sees: Ctrl+Q and
 * Ctrl+W close the window, and Ctrl+L edits the location; the titlebar's
 * controls step through the history and reload.
 *
 * The program writes lines to standard output that the guest tests read
 * (ZBROWSER READY, FRAME, LINK, NAVIGATE, TITLEBAR, CONSOLE, LOADING,
 * STOPPED, ERROR, TOUCH); they are its diagnostic interface.
 */

#include "shell/internal.h"
#include "../../picture/png-write.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest location typed into the URL's field that is opened (a longer one is cut). */
#define SHELL_LOCATION_MAX	4096U

/* The evdev codes of the keys of the shell's own shortcuts. */
#define SHELL_KEY_Q		16U
#define SHELL_KEY_W		17U
#define SHELL_KEY_L		38U

/* How far a press moves before the image under it is dragged out of the window, and the picture's longest side (ws189-p003). */
#define SHELL_DRAG_DISTANCE	8
#define SHELL_DRAG_SIDE		2048

/*
 * What the window mode holds while it runs: the view, the window with its
 * titlebar and presenter, whether the view must be drawn again, and
 * whether the window is up (the view's first page is loaded before it).
 *
 * The fingers (touch.c, ws081-p006): their gestures and scroller (without
 * them fingers do nothing), in how many milliseconds they want the next
 * round (-1: none), and the number of the page shown, which each page
 * committed moves on (a new page's scroll is taken over by the fingers).
 *
 * The sign-in codes of mail (mail.c, WS169 p005).
 *
 * A left press that may drag a picture out of the window (ws189-p003):
 * whether one is held, and where it went down.
 *
 * text_session is the view's text input's session when the window's text
 * input was last asked for (browser_view_text_session, ws177-p019).
 */
struct shell_state {
	struct browser_view *view;
	struct shell_window window;
	struct shell_titlebar titlebar;
	struct shell_present present;
	int dirty;
	int ready;
	struct shell_touch touch;
	int touch_due;
	unsigned long page_number;
	struct shell_mail mail;
	int drag_armed;
	int drag_press_x;
	int drag_press_y;
	uint64_t text_session;
};

static void shell_show_state(struct shell_state *state);
static void shell_input(struct shell_state *state, const struct shell_event *event);
static void shell_button(struct shell_state *state, const struct shell_event *event);
static int shell_drag_image(struct shell_state *state);
static void shell_key(struct shell_state *state, const struct shell_event *event);
static int shell_text(struct shell_state *state, const struct shell_event *event);
static void shell_text_input(struct shell_state *state);
static int shell_shortcut(struct shell_state *state, const struct shell_event *event);
static void shell_titlebar_input(struct shell_state *state, const struct shell_titlebar_event *event);
static void shell_go(struct shell_state *state, int steps);
static void shell_follow(struct shell_state *state, const char *target);
static void shell_follow_typed(struct shell_state *state, const char *typed);
static int shell_frame(struct shell_state *state);
static void shell_release(struct shell_state *state);
static int shell_resize_view(struct shell_state *state);
static void shell_redraw(void *context, struct browser_view *view);
static void shell_title(void *context, struct browser_view *view, const char *title);
static void shell_committed(void *context, struct browser_view *view);
static void shell_load(void *context, struct browser_view *view, enum browser_load_state state, const char *url, int error, const char *reason);
static enum browser_policy shell_link(void *context, struct browser_view *view, const char *href);
static void shell_console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static void shell_script_error(void *context, struct browser_view *view, int error);
static void shell_touch_round(struct shell_state *state);
static void shell_touch_pointer(struct shell_state *state, const struct shell_touch_pointer *made);

/*
 * Runs the browser in a compositor window until it is closed.
 *
 * Returns the program's exit status.
 */
int
shell_run(
	const struct shell_options *options)
{
	struct shell_state state;
	struct browser_callbacks callbacks;
	struct browser_view_options view_options;
	struct pollfd net_fds[SHELL_NET_FDS];
	struct shell_event event;
	struct shell_titlebar_event titlebar_event;
	size_t net_count;
	int changed;
	int timeout;
	int network;
	int mail_due;
	int status;
	int taken;
	int error;
	VkResult result;

	/* A window needs a page to show. */
	memset(&state, 0, sizeof(state));
	if (options->start == NULL) {
		fprintf(stderr, "browser: a page to open is needed (a file or a file: URL)\n");
		return 2;
	}

	/* The view, which tells the shell what happens to it. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.redraw = shell_redraw;
	callbacks.title = shell_title;
	callbacks.committed = shell_committed;
	callbacks.load = shell_load;
	callbacks.link = shell_link;
	callbacks.console = shell_console;
	callbacks.script_error = shell_script_error;
	callbacks.context = &state;
	memset(&view_options, 0, sizeof(view_options));
	view_options.version = BROWSER_API_VERSION;
	view_options.fonts = options->fonts;
	view_options.callbacks = &callbacks;
	view_options.stack_base = __builtin_frame_address(0);
	view_options.width = options->width;
	view_options.height = options->height;
	error = browser_view_create(&view_options, &state.view);
	if (error != 0) {
		fprintf(stderr, "browser: cannot start the network: %s\n", strerror(error));
		return 1;
	}

	/* The fingers; without memory for them they do nothing. */
	state.touch_due = -1;
	error = shell_touch_open(&state.touch);
	if (error != 0) {
		printf("ZBROWSER ERROR touch error=%d\n", error);
		fflush(stdout);
	}

	/* The start page, read before the window opens. */
	error = browser_view_load(state.view, options->start);
	if (error != 0) {
		shell_release(&state);
		return 1;
	}

	/* The window, with the page's title. */
	status = shell_window_open(&state.window, options->display, options->width, options->height,
	    browser_view_title(state.view));
	if (status != 0) {
		fprintf(stderr, "browser: cannot open a window: %s\n", strerror(errno));
		shell_release(&state);
		return 1;
	}

	/* The compositor's titlebar with the browser's controls (a compositor without it leaves the plain titlebar). */
	error = shell_titlebar_open(&state.titlebar, &state.window);
	if (error != 0) {
		printf("ZBROWSER ERROR titlebar error=%d\n", error);
		shell_titlebar_close(&state.titlebar);
	}

	/* The arrivals of mail with their sign-in codes (a compositor without them leaves the browser without codes). */
	shell_mail_open(&state.mail, state.window.app);

	/* The Vulkan presenter in the window. */
	result = shell_present_open(&state.present, &state.window, state.view);
	if (result != VK_SUCCESS) {
		fprintf(stderr, "browser: cannot draw in the window: %s failed (%d)\n", state.present.operation, (int)result);
		shell_release(&state);
		return 1;
	}

	/* The page at the window's size. */
	status = shell_resize_view(&state);
	if (status != 0) {
		shell_release(&state);
		return 1;
	}

	/* The line the tests wait for, and the titlebar's state. */
	printf("ZBROWSER READY width=%u height=%u document=%.0f\n", (unsigned)state.present.extent.width,
	    (unsigned)state.present.extent.height, browser_view_document_height(state.view));
	fflush(stdout);
	state.ready = 1;
	shell_show_state(&state);

	/* Draws, then waits for the compositor, until the window closes. */
	state.dirty = 1;
	while (!state.window.closed) {
		/* A new size replaces the swapchain and lays the page out again. */
		if (state.window.resized) {
			state.window.resized = 0;
			result = shell_present_resize(&state.present, state.view, state.window.width, state.window.height);
			if (result != VK_SUCCESS) {
				fprintf(stderr, "browser: cannot resize: %s failed (%d)\n", state.present.operation, (int)result);
				break;
			}

			/* The page at the new size. */
			status = shell_resize_view(&state);
			if (status != 0)
				break;
			state.dirty = 1;
		}

		/* Draws the page when it changed. */
		if (state.dirty) {
			status = shell_frame(&state);
			if (status != 0)
				break;
			state.dirty = 0;
		}

		/* Waits for the compositor, the view's descriptors, the view's next work or the end of a code's offer (a held key's repeat waits less, within the dispatch). */
		timeout = -1;
		network = browser_view_timeout(state.view);
		if (network >= 0 && (timeout < 0 || network < timeout))
			timeout = network;
		if (state.touch_due >= 0 && (timeout < 0 || state.touch_due < timeout))
			timeout = state.touch_due;
		mail_due = shell_mail_timeout(&state.mail, shell_clock());
		if (mail_due >= 0 && (timeout < 0 || mail_due < timeout))
			timeout = mail_due;
		net_count = browser_view_poll_fds(state.view, net_fds, SHELL_NET_FDS);
		status = shell_window_dispatch(&state.window, timeout, net_fds, net_count);
		if (status != 0) {
			fprintf(stderr, "browser: the connection to the compositor was lost\n");
			break;
		}

		/* Carries out the inputs that arrived. */
		for (;;) {
			taken = shell_window_take(&state.window, &event);
			if (!taken)
				break;
			shell_input(&state, &event);
		}

		/* And what was done with the titlebar. */
		for (;;) {
			taken = shell_titlebar_take(&state.titlebar, &titlebar_event);
			if (!taken)
				break;
			shell_titlebar_input(&state, &titlebar_event);
		}

		/* The fingers: the scroll they move and the clicks they make. */
		shell_touch_round(&state);

		/* A sign-in code that came by mail, offered or typed (the titlebar declared again shows the state again). */
		changed = shell_mail_round(&state.mail, state.view, &state.titlebar, shell_clock());
		if (changed)
			shell_show_state(&state);

		/* The view's work: the network, the page's timers, and the layout they changed. */
		browser_view_process(state.view, net_fds, net_count);
	}

	/* Closes everything. */
	shell_release(&state);

	/* Succeeded: the window was closed. */
	return 0;
}

/* Gives the view the swapchain's size; nonzero on failure. */
static int
shell_resize_view(
	struct shell_state *state)
{
	int error;

	/* The view at the swapchain's extent. */
	error = browser_view_resize(state->view, state->present.extent.width, state->present.extent.height);
	if (error != 0) {
		fprintf(stderr, "browser: cannot lay out the page: %s\n", strerror(error));
		return 1;
	}

	/* Succeeded: the page fits the window. */
	return 0;
}

/* Shows the page's title in the window and the history and location in the titlebar, and writes the NAVIGATE line. */
static void
shell_show_state(
	struct shell_state *state)
{
	const char *title;
	const char *url;
	int can_back;
	int can_forward;
	int error;

	/* The window's title: the page's, or its location. */
	title = browser_view_title(state->view);
	url = browser_view_url(state->view);
	shell_window_title(&state->window, title);

	/* The line the tests read. */
	printf("ZBROWSER NAVIGATE path=%s title=%s\n", url, title);
	fflush(stdout);

	/* The titlebar; a refusal is reported and the titlebar stays as it was. */
	can_back = browser_view_can_go(state->view, -1);
	can_forward = browser_view_can_go(state->view, 1);
	error = shell_titlebar_show(&state->titlebar, can_back, can_forward, url);
	if (error != 0) {
		printf("ZBROWSER ERROR titlebar-state error=%d\n", error);
		fflush(stdout);
	}
}

/* Gives one input of the window to the view: the pointer, the wheel, a key, or the keyboard's focus. */
static void
shell_input(
	struct shell_state *state,
	const struct shell_event *event)
{
	uint32_t modifiers;
	int distance_x;
	int distance_y;
	int dragged;
	int error;

	/* The modifiers held, as the view names them. */
	modifiers = shell_key_modifiers(event->modifiers);

	/* Each kind of input to its call. */
	error = 0;
	switch (event->type) {
	case SHELL_EVENT_BUTTON:
		shell_button(state, event);
		break;
	case SHELL_EVENT_MOTION:
		/* A left press on an image moved far enough drags the image out of the window (ws189-p003). */
		distance_x = abs(event->x - state->drag_press_x);
		distance_y = abs(event->y - state->drag_press_y);
		if (state->drag_armed &&
		    (distance_x > SHELL_DRAG_DISTANCE ||
		     distance_y > SHELL_DRAG_DISTANCE)) {
			state->drag_armed = 0;
			dragged = shell_drag_image(state);
			if (dragged)
				break;
		}

		/* The pointer's move, the page's. */
		error = browser_view_pointer_move(state->view, (float)event->x, (float)event->y, modifiers);
		break;
	case SHELL_EVENT_LEAVE:
		error = browser_view_pointer_leave(state->view);
		break;
	case SHELL_EVENT_SCROLL:
		error = browser_view_wheel(
			state->view,
			(float)event->x,
			(float)event->y,
			(float)event->scroll_x,
			(float)event->scroll,
			modifiers);
		break;
	case SHELL_EVENT_KEY:
		shell_key(state, event);
		break;
	case SHELL_EVENT_FOCUS:
		error = browser_view_focus(state->view, event->pressed);
		break;
	case SHELL_EVENT_TEXT_COMMIT:
	case SHELL_EVENT_TEXT_PREEDIT:
	case SHELL_EVENT_TEXT_DELETE:
		error = shell_text(state, event);
		break;
	default:
		break;
	}

	/* A failure of the page's scripts or its layout is reported and the window goes on. */
	if (error != 0) {
		printf("ZBROWSER ERROR input error=%s\n", strerror(error));
		fflush(stdout);
	}
}

/* Gives the view a pointer button pressed or let go (the view makes the click of a press and a release). */
static void
shell_button(
	struct shell_state *state,
	const struct shell_event *event)
{
	uint32_t modifiers;
	int button;
	int error;

	/* A left press may become a drag of the image under it; any release ends that. */
	state->drag_armed = 0;
	if (event->pressed && event->button == SHELL_BUTTON_LEFT) {
		state->drag_armed = 1;
		state->drag_press_x = event->x;
		state->drag_press_y = event->y;
	}

	/* The DOM's number of the button; a button it does not number is not given. */
	button = shell_key_button(event->button);
	if (button < 0)
		return;

	/* The button, at the pointer's place. */
	modifiers = shell_key_modifiers(event->modifiers);
	error = browser_view_pointer_button(state->view, (float)event->x, (float)event->y, button, event->pressed, modifiers);
	if (error != 0) {
		printf("ZBROWSER ERROR click error=%s\n", strerror(error));
		fflush(stdout);
	}
}

/* Gives the view a key pressed, repeated or let go, unless it is one of the shell's own shortcuts. */
static void
shell_key(
	struct shell_state *state,
	const struct shell_event *event)
{
	struct shell_key_names names;
	uint32_t modifiers;
	int taken;
	int error;

	/* The shell's own shortcuts never reach the page. */
	taken = shell_shortcut(state, event);
	if (taken)
		return;

	/* The key with the DOM's names. */
	shell_key_names(event->key, event->modifiers, &names);
	modifiers = shell_key_modifiers(event->modifiers);
	error = browser_view_key(state->view, names.key, names.code, names.text, event->pressed, event->repeat, modifiers);
	if (error != 0) {
		printf("ZBROWSER ERROR key error=%s\n", strerror(error));
		fflush(stdout);
	}
}

/* Gives the view an input method's text: committed, composed, or the bytes it deletes around the caret (ws090-p025). */
static int
shell_text(
	struct shell_state *state,
	const struct shell_event *event)
{
	int error;

	/* Each kind to its call. */
	switch (event->type) {
	case SHELL_EVENT_TEXT_COMMIT:
		error = browser_view_commit_text(state->view, event->text, 0U, 0U);
		break;
	case SHELL_EVENT_TEXT_PREEDIT:
		error = browser_view_compose(state->view, event->text, event->begin, event->end);
		break;
	default:
		error = browser_view_commit_text(state->view, "", event->before, event->after);
		break;
	}

	/* A failure of the page's scripts is the caller's to report. */
	if (error != 0)
		return error;

	/* Succeeded: the view took it. */
	return 0;
}

/*
 * Asks for the window's text input while the page's focused element takes
 * an input method's text, and tells where its caret is (after each frame,
 * where the caret was just drawn).
 */
static void
shell_text_input(
	struct shell_state *state)
{
	char text[KL_TEXT_SURROUNDING_MAX];
	float caret[4];
	uint64_t session;
	size_t cursor;
	unsigned hints;
	int purpose;
	int wanted;
	int known;

	/*
	 * Another field, another page, or a composing the page ended itself (a
	 * click put it into the value, ws177-p019): the text input is turned
	 * off first, so that the input method is deactivated and drops what it
	 * was composing, and on again below for the field that has the focus.
	 */
	session = browser_view_text_session(state->view);
	if (session != state->text_session) {
		kl_window_text_input(state->window.kui, 0);
		state->text_session = session;
	}

	/* On while the focus is in a field or a textarea, off otherwise. */
	wanted = browser_view_text_target(state->view, caret);
	kl_window_text_input(state->window.kui, wanted);
	if (!wanted)
		return;

	/* Where its caret is, for the candidates and the on-screen keyboard. */
	kl_window_text_cursor(state->window.kui, (int)caret[0], (int)caret[1], (int)caret[2], (int)caret[3]);

	/*
	 * Its text around the caret and what it is for (ws177-p019): the
	 * browser's purposes and hints are text-input-v3's numbers, as
	 * libkeiland's are.
	 */
	known = browser_view_text_context(state->view, text, sizeof(text), &cursor, &purpose, &hints);
	if (!known)
		return;
	kl_window_text_context(state->window.kui, text, cursor, cursor, hints, (unsigned)purpose);
}

/*
 * Carries out the shell's own shortcuts: Ctrl+Q and Ctrl+W close the
 * window, and Ctrl+L edits the location.  Returns whether the key was one
 * (its press and its release are both the shell's).
 */
static int
shell_shortcut(
	struct shell_state *state,
	const struct shell_event *event)
{
	int error;

	/* Only keys with Control are shortcuts. */
	if ((event->modifiers & SHELL_MOD_CTRL) == 0U)
		return 0;

	/* The keys that close the window, when pressed. */
	if (event->key == SHELL_KEY_Q || event->key == SHELL_KEY_W) {
		if (event->pressed)
			state->window.closed = 1;
		return 1;
	}

	/* The key that edits the location, when pressed. */
	if (event->key == SHELL_KEY_L) {
		if (event->pressed) {
			error = shell_titlebar_edit_location(&state->titlebar);
			if (error != 0)
				printf("ZBROWSER ERROR location-edit error=%d\n", error);
		}

		/* Its release is the shell's too. */
		return 1;
	}

	/* Any other key with Control is the page's. */
	return 0;
}

/* Carries out what was done with the titlebar: back, forward, reload, or the location clicked or edited. */
static void
shell_titlebar_input(
	struct shell_state *state,
	const struct shell_titlebar_event *event)
{
	int error;

	/* The location's editing ended: Enter opens the URL typed; Esc and leaving change nothing. */
	if (event->kind == SHELL_TITLEBAR_DONE) {
		if (event->id != SHELL_CONTROL_LOCATION)
			return;
		if (event->detail != KL_TEXT_SUBMITTED)
			return;
		shell_follow_typed(state, event->text);
		return;
	}

	/* A control chosen. */
	switch (event->id) {
	case SHELL_CONTROL_BACK:
		shell_go(state, -1);
		break;
	case SHELL_CONTROL_FORWARD:
		shell_go(state, 1);
		break;
	case SHELL_CONTROL_RELOAD:
		shell_go(state, 0);
		break;
	case SHELL_CONTROL_CODE:
		/* The sign-in code offered, typed into the page (WS169 p005); the titlebar shows the state again. */
		shell_mail_fill(&state->mail, state->view, &state->titlebar);
		shell_show_state(state);
		break;
	case SHELL_CONTROL_LOCATION:
		/* A part of the location turns it into the URL's field. */
		error = shell_titlebar_edit_location(&state->titlebar);
		if (error != 0)
			printf("ZBROWSER ERROR location-edit error=%d\n", error);
		break;
	default:
		break;
	}
}

/* Shows the page some steps back or forward, or the same page again (0, a reload). */
static void
shell_go(
	struct shell_state *state,
	int steps)
{
	/* A failure was reported by the load callback; the history stays where it was. */
	(void)browser_view_go(state->view, steps);
}

/* Opens a typed location, resolved against the page shown, as a new step. */
static void
shell_follow(
	struct shell_state *state,
	const char *target)
{
	int error;

	/* The view's navigation; a location that does not resolve is reported here. */
	error = browser_view_follow(state->view, target);
	if (error == EINVAL) {
		printf("ZBROWSER ERROR follow target=%s error=%s\n", target, strerror(error));
		fflush(stdout);
	}
}

/*
 * Opens what was typed into the URL's field (BUG-240): a path from the
 * root ("/usr/...") is a local file whatever page is shown, so it opens as
 * file://; anything else is resolved against the page as before.
 */
static void
shell_follow_typed(
	struct shell_state *state,
	const char *typed)
{
	static char location[sizeof("file://") + SHELL_LOCATION_MAX];

	/* Anything but an absolute path: as typed. */
	if (typed[0] != '/') {
		shell_follow(state, typed);
		return;
	}

	/* An absolute path: a local file, not a path of the page's site. */
	snprintf(location, sizeof(location), "file://%s", typed);
	printf("ZBROWSER location typed=path url=%s\n", location);
	shell_follow(state, location);
}

/* Draws the view into the window; a swapchain out of date is replaced and the frame drawn again. Nonzero on failure. */
static int
shell_frame(
	struct shell_state *state)
{
	VkResult result;
	int error;

	/* The page laid out as it is now, before a frame begins; one that cannot be laid out is reported and not drawn. */
	error = browser_view_prepare(state->view);
	if (error != 0) {
		printf("ZBROWSER ERROR layout error=%s\n", strerror(error));
		fflush(stdout);
		return 0;
	}

	/* The frame, which the view records into the window's image. */
	result = shell_present_frame(&state->present, state->view);

	/* A swapchain that no longer fits the window is replaced, and the frame drawn once more. */
	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		result = shell_present_resize(&state->present, state->view, state->window.width, state->window.height);
		if (result == VK_SUCCESS)
			result = shell_present_frame(&state->present, state->view);
	}

	/* Says what failed. */
	if (result != VK_SUCCESS) {
		fprintf(stderr, "browser: cannot draw a frame: %s failed (%d)\n", state->present.operation, (int)result);
		return 1;
	}

	/* The window's text input follows the frame: on while the page's focus takes an input method's text. */
	shell_text_input(state);

	/* The line the tests read. */
	printf("ZBROWSER FRAME scroll=%.0f width=%u height=%u\n", browser_view_scroll_y(state->view),
	    (unsigned)state->present.extent.width, (unsigned)state->present.extent.height);
	fflush(stdout);

	/* Succeeded: the frame is shown. */
	return 0;
}

/* Releases the presenter, the titlebar, the window and the view, in that order. */
static void
shell_release(
	struct shell_state *state)
{
	/* The presenter before the window whose surface it draws. */
	shell_present_close(&state->present, state->view);

	/* The titlebar before the window it belongs to. */
	shell_titlebar_close(&state->titlebar);

	/* The offer of a sign-in code, before the application's system goes with the window. */
	if (state->window.app != NULL)
		shell_mail_close(&state->mail, &state->titlebar);

	/* The window. */
	if (state->window.app != NULL)
		shell_window_close(&state->window);

	/* The view, with its page, its history and its network, and the fingers that moved it. */
	browser_view_destroy(state->view);
	state->view = NULL;
	shell_touch_close(&state->touch);
}

/* The view's callback: its content changed and is drawn in the next frame. */
static void
shell_redraw(
	void *context,
	struct browser_view *view)
{
	struct shell_state *state;

	/* The next frame. */
	UNUSED_PARAMETER(view);
	state = context;
	state->dirty = 1;
}

/* The view's callback: a script changed the title, which the window shows. */
static void
shell_title(
	void *context,
	struct browser_view *view,
	const char *title)
{
	struct shell_state *state;

	/* The window's title, once the window is up. */
	UNUSED_PARAMETER(view);
	state = context;
	if (state->ready)
		shell_window_title(&state->window, title);
}

/* The view's callback: a page became the one shown (the first page is shown once the window is up). */
static void
shell_committed(
	void *context,
	struct browser_view *view)
{
	struct shell_state *state;

	/* The title, the NAVIGATE line and the titlebar. */
	UNUSED_PARAMETER(view);
	state = context;
	if (state->ready)
		shell_show_state(state);

	/* Another page, whose scroll the fingers take over. */
	state->page_number++;
}

/* The view's callback: a load started (LOADING), was stopped (STOPPED) or failed (ERROR). */
static void
shell_load(
	void *context,
	struct browser_view *view,
	enum browser_load_state state,
	const char *url,
	int error,
	const char *reason)
{
	/* The line the tests read. */
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);
	if (state == BROWSER_LOAD_STARTED)
		printf("ZBROWSER LOADING url=%s\n", url);
	if (state == BROWSER_LOAD_STOPPED)
		printf("ZBROWSER STOPPED url=%s\n", url);
	if (state == BROWSER_LOAD_FAILED)
		printf("ZBROWSER ERROR load path=%s error=%s tls=%s\n", url, strerror(error), reason);
	fflush(stdout);
}

/* The view's callback: a click opens a link, which the shell allows (and writes the LINK line). */
static enum browser_policy
shell_link(
	void *context,
	struct browser_view *view,
	const char *href)
{
	/* The line the tests read. */
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);
	printf("ZBROWSER LINK href=%s\n", href);
	fflush(stdout);

	/* The view follows it. */
	return BROWSER_POLICY_ALLOW;
}

/* The view's callback: a page's console line, as a CONSOLE line of the diagnostic interface. */
static void
shell_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	/* The level's number and the text. */
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);
	printf("ZBROWSER CONSOLE level=%d %.*s\n", level, (int)length, text);
	fflush(stdout);
}

/* The view's callback: a timer's script failed. */
static void
shell_script_error(
	void *context,
	struct browser_view *view,
	int error)
{
	/* The line the tests read. */
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);
	printf("ZBROWSER ERROR script error=%s\n", strerror(error));
	fflush(stdout);
}

/*
 * Runs the fingers for one round (ws081-p006): gives them the page's scroll
 * and range, their events and the time, places the scroll and the
 * overscroll they set, and gives the view the clicks they made.  With no
 * finger down and nothing gliding there is nothing to do.
 */
static void
shell_touch_round(
	struct shell_state *state)
{
	struct shell_touch_pointer made;
	double largest_x;
	double largest_y;
	double scroll;
	double overscroll;
	unsigned index;
	int moved;
	int taken;
	int error;

	/* Nothing touches and nothing glides. */
	if (state->window.touch_count == 0U && state->touch_due < 0)
		return;

	/* The page as it is: its scroll, how far it scrolls, and the view's height. */
	error = browser_view_scroll_range(state->view, &largest_x, &largest_y);
	if (error != 0)
		largest_y = 0.0;
	shell_touch_layout(&state->touch, state->page_number, browser_view_scroll_y(state->view), largest_y,
	    (double)state->present.extent.height);

	/* The fingers' events. */
	for (index = 0; index < state->window.touch_count; index++)
		shell_touch_event(&state->touch, &state->window.touches[index]);

	/* The queue is empty again: the window fills it from the next read. */
	state->window.touch_count = 0U;

	/* Time moves on for them, and they say when they want the next round. */
	state->touch_due = shell_touch_tick(&state->touch, shell_touch_clock());

	/* The scroll and the stretch they set (the view asks to be drawn again when they change). */
	moved = shell_touch_scroll(&state->touch, &scroll, &overscroll);
	if (moved) {
		(void)browser_view_scroll_to(state->view, 0.0, scroll);
		(void)browser_view_set_overscroll(state->view, 0.0, overscroll);
	}

	/* The pointer's events they made, in order. */
	for (;;) {
		taken = shell_touch_take_pointer(&state->touch, &made);
		if (!taken)
			break;
		shell_touch_pointer(state, &made);
	}
}

/* Gives the view one pointer event the fingers made: the pointer moves there, or a button is pressed or let go. */
static void
shell_touch_pointer(
	struct shell_state *state,
	const struct shell_touch_pointer *made)
{
	uint32_t modifiers;
	int pressed;
	int error;

	/* The modifiers held on the keyboard. */
	modifiers = shell_key_modifiers(state->window.modifiers);

	/* A motion, or a button. */
	if (made->kind == SHELL_TOUCH_POINTER_MOTION) {
		error = browser_view_pointer_move(state->view, made->x, made->y, modifiers);
	} else {
		pressed = 0;
		if (made->kind == SHELL_TOUCH_POINTER_PRESS)
			pressed = 1;
		error = browser_view_pointer_button(state->view, made->x, made->y, made->button, pressed, modifiers);
	}

	/* A failure of the page's scripts or its layout is reported and the window goes on. */
	if (error != 0) {
		printf("ZBROWSER ERROR touch-click error=%s\n", strerror(error));
		fflush(stdout);
	}
}

/*
 * Drags the image under the press out of the window (ws189-p003): its
 * picture ("image/png", at most SHELL_DRAG_SIDE on its longer side) and the
 * URL of its source as text, as a copy, with the picture under the
 * pointer.  The drag starts first and its PNG is filled in after.  Returns
 * 1 when the drag started, 0 when there is no image there (the motion is
 * the page's then).
 */
static int
shell_drag_image(
	struct shell_state *state)
{
	struct browser_image image;
	struct kl_drag_data data[3];
	struct kl_drag_icon icon;
	unsigned char *png;
	uint32_t *fitted;
	size_t count;
	size_t size;
	int fitted_width;
	int fitted_height;
	int error;

	/* The image at the press. */
	error = browser_view_image_at(state->view, (float)state->drag_press_x, (float)state->drag_press_y, &image);
	if (error != 0)
		return 0;

	/* The picture, filled in later, and the source's URL as both text types when it has one. */
	data[0].type = "image/png";
	data[0].data = NULL;
	data[0].length = 0;
	count = 1;
	if (image.url[0] != '\0') {
		data[1].type = "text/plain;charset=utf-8";
		data[1].data = image.url;
		data[1].length = strlen(image.url);
		data[2].type = "text/plain";
		data[2].data = image.url;
		data[2].length = strlen(image.url);
		count = 3;
	}

	/* The drag from the press, a copy, the picture under the pointer at its middle. */
	icon.pixels = image.pixels;
	icon.width = image.width;
	icon.height = image.height;
	icon.hot_x = image.width / 2;
	icon.hot_y = image.height / 2;
	error = kl_window_start_drag_icon(state->window.kui, data, count, KL_DND_COPY, kl_window_press_serial(state->window.kui), &icon);
	if (error != 0) {
		printf("ZBROWSER DND drag failed errno=%d\n", error);
		fflush(stdout);
		browser_view_image_release(&image);
		return 0;
	}

	/* The PNG, shrunk, filled in. */
	png = NULL;
	size = 0;
	error = kl_picture_fit(image.pixels, image.width, image.height, (size_t)image.width, SHELL_DRAG_SIDE, &fitted, &fitted_width, &fitted_height);
	if (error == 0) {
		error = kl_picture_png(fitted, fitted_width, fitted_height, (size_t)fitted_width, &png, &size);
		free(fitted);
	}

	/* Given to the drag. */
	if (error == 0)
		error = kl_window_drag_fill(state->window.kui, "image/png", png, size);
	free(png);

	/* The log line the tests read. */
	printf("ZBROWSER DND drag image size=%dx%d bytes=%lu url=%s errno=%d\n", image.width, image.height, (unsigned long)size, image.url, error);
	fflush(stdout);
	browser_view_image_release(&image);

	/* Succeeded: the drag goes on. */
	return 1;
}
