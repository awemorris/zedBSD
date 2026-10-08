/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's text input (ws090-p013, libkeiui's version 6; WS102's D1): the
 * client's side of text-input-unstable-v3, through which the compositor sends
 * the text an input method composes and commits, and the kana of the
 * on-screen keyboard.  Only the public client header is used; the
 * compositor's and the input method's sides are not this library's.
 *
 * The application asks for text (kl_window_text_input) where it edits
 * text; the text input is enabled while it asks and the text input is on
 * the window's surface (enter), and each change of that, or of the caret's
 * rectangle, is committed.  What arrives before a done is kept and queued
 * at the done, in the protocol's order: the bytes to delete around the
 * caret, the text to commit, then the text being composed.  The widgets'
 * side (KL_VERSION 47, ws090-p022): a window's input given to its widgets
 * and the text input asked for while one of their fields has the keyboard.
 */

#include "window.h"
#include "internal.h"

#include <wayland/text-input-unstable-v3-client-protocol.h>

#include <string.h>

static void text_enter(void *data, struct zwp_text_input_v3 *input, struct wl_surface *surface);
static void text_leave(void *data, struct zwp_text_input_v3 *input, struct wl_surface *surface);
static void text_preedit(void *data, struct zwp_text_input_v3 *input, const char *text, int32_t begin, int32_t end);
static void text_commit(void *data, struct zwp_text_input_v3 *input, const char *text);
static void text_delete(void *data, struct zwp_text_input_v3 *input, uint32_t before, uint32_t after);
static void text_done(void *data, struct zwp_text_input_v3 *input, uint32_t serial);
static void text_state(struct kl_window *window);
static void text_copy(char *out, const char *text);

/* The text input's events. */
static const struct zwp_text_input_v3_listener text_listener = {
	text_enter,
	text_leave,
	text_preedit,
	text_commit,
	text_delete,
	text_done
};

/*
 * Binds the compositor's text input manager (from the registry).
 */
void
keiui_text_input_bind(
	struct kl_window *window,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Version 1 is the only one. */
	window->text_manager = wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1U);
}

/*
 * Gets the seat's text input, once the globals are bound.
 */
void
keiui_text_input_start(
	struct kl_window *window)
{
	int status;

	/* Without the manager or a seat there is no text input (keys still type). */
	if (window->text_manager == NULL || window->seat == NULL)
		return;

	/* The seat's text input, heard. */
	window->text_input = zwp_text_input_manager_v3_get_text_input(window->text_manager, window->seat);
	if (window->text_input == NULL)
		return;
	status = zwp_text_input_v3_add_listener(window->text_input, &text_listener, window);
	if (status != 0) {
		zwp_text_input_v3_destroy(window->text_input);
		window->text_input = NULL;
	}
}

/*
 * Destroys the text input and its manager.
 */
void
keiui_text_input_close(
	struct kl_window *window)
{
	/* The text input before its manager. */
	if (window->text_input != NULL)
		zwp_text_input_v3_destroy(window->text_input);
	window->text_input = NULL;
	if (window->text_manager != NULL)
		zwp_text_input_manager_v3_destroy(window->text_manager);
	window->text_manager = NULL;
}

/*
 * Asks for the text an input method or the on-screen keyboard sends (1),
 * or no longer (0): where the application edits text, and not elsewhere
 * (a dialog, a chooser).
 */
void
kl_window_text_input(
	struct kl_window *window,
	int enabled)
{
	/* The same as before: nothing to say. */
	enabled = enabled != 0;
	if (enabled == window->text_wanted)
		return;

	/* Enabled or disabled while the text input is on the window. */
	window->text_wanted = enabled;
	text_state(window);
}

/*
 * Tells where the caret is (surface pixels), so that a candidate list or a
 * keyboard stays out of its way; sent when it changes.
 */
void
kl_window_text_cursor(
	struct kl_window *window,
	int x,
	int y,
	int width,
	int height)
{
	/* The same as before: nothing to say. */
	if (window->text_cursor[0] == x && window->text_cursor[1] == y && window->text_cursor[2] == width && window->text_cursor[3] == height)
		return;
	window->text_cursor[0] = x;
	window->text_cursor[1] = y;
	window->text_cursor[2] = width;
	window->text_cursor[3] = height;

	/* Sent while enabled. */
	if (!window->text_enabled || window->text_input == NULL)
		return;
	zwp_text_input_v3_set_cursor_rectangle(window->text_input, x, y, width, height);
	zwp_text_input_v3_commit(window->text_input);
	window->text_commits++;
}

/* The text input came to a surface of the program: the window's is enabled when the application asks. */
static void
text_enter(
	void *data,
	struct zwp_text_input_v3 *input,
	struct wl_surface *surface)
{
	struct kl_window *window;

	/* Only the window's own surface (a chooser's window has its own). */
	(void)input;
	window = data;
	if (surface != window->surface)
		return;
	window->text_entered = 1;
	text_state(window);
}

/* The text input left a surface: the window's is disabled. */
static void
text_leave(
	void *data,
	struct zwp_text_input_v3 *input,
	struct wl_surface *surface)
{
	struct kl_window *window;

	/* Only the window's own surface. */
	(void)input;
	window = data;
	if (surface != window->surface)
		return;
	window->text_entered = 0;
	text_state(window);
}

/* The text being composed, kept for the done. */
static void
text_preedit(
	void *data,
	struct zwp_text_input_v3 *input,
	const char *text,
	int32_t begin,
	int32_t end)
{
	struct kl_window *window;

	/* Kept (none is an empty text). */
	(void)input;
	window = data;
	text_copy(window->text_preedit, text);
	window->text_preedit_begin = begin;
	window->text_preedit_end = end;
	window->text_preedit_set = 1;
}

/* The text to commit, kept for the done. */
static void
text_commit(
	void *data,
	struct zwp_text_input_v3 *input,
	const char *text)
{
	struct kl_window *window;

	/* Kept. */
	(void)input;
	window = data;
	text_copy(window->text_commit, text);
}

/* The bytes to delete around the caret, kept for the done. */
static void
text_delete(
	void *data,
	struct zwp_text_input_v3 *input,
	uint32_t before,
	uint32_t after)
{
	struct kl_window *window;

	/* Kept. */
	(void)input;
	window = data;
	window->text_before = before;
	window->text_after = after;
}

/* The events before it apply together: queued as the window's inputs, deleting, committing, then composing. */
static void
text_done(
	void *data,
	struct zwp_text_input_v3 *input,
	uint32_t serial)
{
	struct kl_window_event *event;
	struct kl_window *window;

	/* The serial is the compositor's count of the window's commits; a text for an older state is still taken. */
	(void)input;
	(void)serial;
	window = data;

	/* The bytes to delete around the caret. */
	if (window->text_before != 0U || window->text_after != 0U) {
		event = keiui_window_push(window, KL_WINDOW_TEXT_DELETE);
		if (event != NULL) {
			event->before = window->text_before;
			event->after = window->text_after;
		}
	}

	/* The text to commit. */
	if (window->text_commit[0] != '\0') {
		event = keiui_window_push(window, KL_WINDOW_TEXT_COMMIT);
		if (event != NULL)
			memcpy(event->text, window->text_commit, sizeof(event->text));
	}

	/* The text being composed, when it came or when one shown goes. */
	if (window->text_preedit_set && (window->text_preedit[0] != '\0' || window->text_preedit_shown)) {
		event = keiui_window_push(window, KL_WINDOW_TEXT_PREEDIT);
		if (event != NULL) {
			memcpy(event->text, window->text_preedit, sizeof(event->text));
			event->begin = window->text_preedit_begin;
			event->end = window->text_preedit_end;
		}

		/* Whether one shows now. */
		window->text_preedit_shown = window->text_preedit[0] != '\0';
	}

	/* Nothing is kept for the next done. */
	window->text_commit[0] = '\0';
	window->text_preedit[0] = '\0';
	window->text_preedit_set = 0;
	window->text_before = 0U;
	window->text_after = 0U;
}

/* Enables the text input while the application asks and it is on the window, disables it otherwise; each change is committed. */
static void
text_state(
	struct kl_window *window)
{
	int enabled;

	/* Nothing to enable. */
	if (window->text_input == NULL)
		return;

	/* Whether it should be, and whether that changes. */
	enabled = window->text_wanted && window->text_entered;
	if (enabled == window->text_enabled)
		return;
	window->text_enabled = enabled;

	/* Enabled: plain text, and where the caret is. */
	if (enabled) {
		zwp_text_input_v3_enable(window->text_input);
		zwp_text_input_v3_set_content_type(window->text_input, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
		zwp_text_input_v3_set_cursor_rectangle(window->text_input, window->text_cursor[0], window->text_cursor[1], window->text_cursor[2], window->text_cursor[3]);
	} else {
		zwp_text_input_v3_disable(window->text_input);
	}

	/* The new state, counted. */
	zwp_text_input_v3_commit(window->text_input);
	window->text_commits++;
}

/* Copies a text into a window's buffer, cut at a character's start when it is too long (NULL is empty). */
static void
text_copy(
	char *out,
	const char *text)
{
	size_t length;

	/* None is empty. */
	if (text == NULL) {
		out[0] = '\0';
		return;
	}

	/* What fits, back to a character's start. */
	length = strlen(text);
	if (length >= KL_WINDOW_TEXT_MAX) {
		length = KL_WINDOW_TEXT_MAX - 1U;
		while (length > 0U && ((unsigned char)text[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* The bytes, ended. */
	memcpy(out, text, length);
	out[length] = '\0';
}

/*
 * Gives one input of a window to the widgets: the pointer and the main
 * button, the wheel, the keys, the fingers, and the text an input method
 * sends.  Returns 1 when it was the widgets' input, 0 for another kind.
 */
int
kl_ui_window_input(
	struct kl_ui *ui,
	const struct kl_window_event *event)
{
	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		(void)kl_ui_pointer_motion(ui, event->x, event->y);
		break;
	case KL_WINDOW_LEAVE:
		(void)kl_ui_pointer_leave(ui);
		break;
	case KL_WINDOW_BUTTON:
		/* The main button only, where the pointer is. */
		(void)kl_ui_pointer_motion(ui, event->x, event->y);
		if (event->code == KL_BUTTON_LEFT)
			(void)kl_ui_pointer_button(ui, event->pressed, event->arrival_us);
		break;
	case KL_WINDOW_AXIS:
	case KL_WINDOW_AXIS_STOP:
		(void)kl_ui_axis(ui, event);
		break;
	case KL_WINDOW_KEY:
		(void)kl_ui_key(ui, event->code, event->pressed, event->modifiers);
		break;
	case KL_WINDOW_TOUCH_DOWN:
		(void)kl_ui_touch_down(ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		(void)kl_ui_touch_motion(ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		(void)kl_ui_touch_up(ui, event->id, event->time_us, event->arrival_us);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		(void)kl_ui_touch_cancel(ui, event->arrival_us);
		break;
	case KL_WINDOW_TEXT_COMMIT:
	case KL_WINDOW_TEXT_PREEDIT:
	case KL_WINDOW_TEXT_DELETE:
		(void)kl_ui_text(ui, event);
		break;
	default:
		/* Not the widgets' input. */
		return 0;
	}

	/* Succeeded: the widgets took it. */
	return 1;
}

/*
 * Asks for a window's text input while the focused widget of the frame
 * shown takes text, and tells where its caret is, after each frame.  It
 * also ties the input to the window, whose clipboard the text widgets'
 * Ctrl+C, Ctrl+X and Ctrl+V use (KL_VERSION 67, ws177-p013).
 */
void
kl_ui_window_text(
	struct kl_ui *ui,
	struct kl_window *window)
{
	struct kl_rect caret;
	int wanted;

	/* The window, for the clipboard. */
	keiui_ui_set_window(ui, window, kl_window_copy, kl_window_paste);

	/* On while a field has the keyboard, off otherwise. */
	wanted = kl_ui_text_wanted(ui, &caret);
	kl_window_text_input(window, wanted);
	if (!wanted)
		return;

	/* Where its caret is, for the candidates and the on-screen keyboard. */
	kl_window_text_cursor(window, caret.x, caret.y, caret.width, caret.height);
}
