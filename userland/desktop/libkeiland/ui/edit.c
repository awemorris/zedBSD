/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The editing operations of the on-screen keyboard's buttons for a window
 * (libkeiui's version 8, ws102-p017, plan/ws102/design.md section 2.10), through
 * libkeiland's kl_edit.  The window says it carries out every
 * operation, with its state before each wait, and turns each operation
 * into the keys it stands for, queued as its own key inputs, so that an
 * application that takes those keys (Text Editor) needs nothing more.
 * select_begin and select_end are the library's own: while a selection is
 * being made, the keys that move the caret come with Shift.
 */

#include "window.h"

#include <keiland/keiland.h>

/* The evdev codes of the keys an operation stands for, and of those that move the caret. */
#define EDIT_KEY_A		30U
#define EDIT_KEY_C		46U
#define EDIT_KEY_V		47U
#define EDIT_KEY_X		45U
#define EDIT_KEY_Z		44U
#define EDIT_KEY_HOME		102U
#define EDIT_KEY_UP		103U
#define EDIT_KEY_PAGE_UP	104U
#define EDIT_KEY_LEFT		105U
#define EDIT_KEY_RIGHT		106U
#define EDIT_KEY_END		107U
#define EDIT_KEY_DOWN		108U
#define EDIT_KEY_PAGE_DOWN	109U

/* Every operation (the window carries each out, as keys or itself). */
#define EDIT_OPERATIONS		0xffU

/* The selection being made, in the state sent. */
#define EDIT_SELECTING		16U

static void edit_operation(void *data, uint32_t operation);
static void edit_keys(struct kl_window *window, uint32_t key, unsigned modifiers);

/*
 * Makes the window's edit object (nothing with a compositor without it).
 */
void
keiui_edit_start(
	struct kl_window *window)
{
	/* libkeiland's object; NULL when the compositor has no such protocol. */
	window->edit = kl_edit_create(window->display, window->toplevel, edit_operation, window);
}

/*
 * Sends the operations and the state as they are now (libkeiland sends
 * only a change).
 */
void
keiui_edit_update(
	struct kl_window *window)
{
	unsigned state;
	int paste;

	/* Nothing to tell without the object. */
	if (window->edit == NULL)
		return;

	/* The state the application told, or what the window can know. */
	if (window->edit_told) {
		state = window->edit_state;
	} else {
		state = KL_EDIT_HAS_SELECTION | KL_EDIT_CAN_UNDO | KL_EDIT_CAN_REDO;
		paste = kl_window_can_paste(window);
		if (paste)
			state |= KL_EDIT_CAN_PASTE;
	}

	/* A selection being made. */
	if (window->selecting)
		state |= EDIT_SELECTING;

	/* Succeeded: sent when it changed. */
	kl_edit_set_state(window->edit, EDIT_OPERATIONS, state);
}

/*
 * Adds Shift to a key that moves the caret while a selection is being
 * made (select_begin), so that it extends the selection.
 */
void
keiui_edit_key(
	struct kl_window *window,
	struct kl_window_event *event)
{
	/* Only while a selection is being made. */
	if (!window->selecting)
		return;

	/* The keys that move the caret. */
	switch (event->code) {
	case EDIT_KEY_HOME:
	case EDIT_KEY_UP:
	case EDIT_KEY_PAGE_UP:
	case EDIT_KEY_LEFT:
	case EDIT_KEY_RIGHT:
	case EDIT_KEY_END:
	case EDIT_KEY_DOWN:
	case EDIT_KEY_PAGE_DOWN:
		event->modifiers |= KL_MOD_SHIFT;
		break;
	default:
		break;
	}
}

/*
 * Destroys the window's edit object.
 */
void
keiui_edit_close(
	struct kl_window *window)
{
	/* The object (none: nothing), and it is forgotten. */
	kl_edit_destroy(window->edit);
	window->edit = NULL;
}

/*
 * Lets the compositor send the window the keys it chooses for the editing
 * operations (a terminal's own) instead of the operations: the window's
 * edit object goes.
 */
void
kl_window_edit_by_keys(
	struct kl_window *window)
{
	/* No edit object: the compositor falls back to the keys. */
	keiui_edit_close(window);
}

/*
 * Lets the application hear the editing operations first: callback returns
 * 1 when it carried the operation out itself (the default is skipped).
 */
void
kl_window_on_edit(
	struct kl_window *window,
	kl_window_edit_fn callback,
	void *data)
{
	/* The callback (NULL: none) and its data. */
	window->edit_callback = callback;
	window->edit_data = data;
}

/*
 * Tells the window's editing state (KL_EDIT_HAS_SELECTION ...), sent
 * before the next wait; from now on the state is the application's.
 */
void
kl_window_edit_state(
	struct kl_window *window,
	unsigned state)
{
	/* The application's state. */
	window->edit_told = 1;
	window->edit_state = state & (KL_EDIT_HAS_SELECTION | KL_EDIT_CAN_PASTE | KL_EDIT_CAN_UNDO | KL_EDIT_CAN_REDO);
}

/*
 * Tells whether a selection is being made (select_begin, until
 * select_end, a copy or a cut).
 */
int
kl_window_selecting(
	const struct kl_window *window)
{
	/* Succeeded: the mode. */
	return window->selecting;
}

/*
 * Carries out an operation the compositor asks for: the application's
 * callback first, then the keys it stands for, or the selection's mode.
 */
static void
edit_operation(
	void *data,
	uint32_t operation)
{
	struct kl_window *window;
	int handled;

	/* The application first, when it listens. */
	window = data;
	handled = 0;
	if (window->edit_callback != NULL)
		handled = window->edit_callback(window->edit_data, operation);
	if (handled)
		return;

	/* The keys each operation stands for; the selection's mode is the window's. */
	switch (operation) {
	case KL_EDIT_COPY:
		window->selecting = 0;
		edit_keys(window, EDIT_KEY_C, KL_MOD_CTRL);
		break;
	case KL_EDIT_CUT:
		window->selecting = 0;
		edit_keys(window, EDIT_KEY_X, KL_MOD_CTRL);
		break;
	case KL_EDIT_PASTE:
		edit_keys(window, EDIT_KEY_V, KL_MOD_CTRL);
		break;
	case KL_EDIT_UNDO:
		edit_keys(window, EDIT_KEY_Z, KL_MOD_CTRL);
		break;
	case KL_EDIT_REDO:
		edit_keys(window, EDIT_KEY_Z, KL_MOD_CTRL | KL_MOD_SHIFT);
		break;
	case KL_EDIT_SELECT_ALL:
		edit_keys(window, EDIT_KEY_A, KL_MOD_CTRL);
		break;
	case KL_EDIT_SELECT_BEGIN:
		window->selecting = 1;
		break;
	case KL_EDIT_SELECT_END:
		window->selecting = 0;
		break;
	default:
		break;
	}
}

/* Queues a key's press and release with modifiers, as the window's own key inputs. */
static void
edit_keys(
	struct kl_window *window,
	uint32_t key,
	unsigned modifiers)
{
	struct kl_window_event *event;
	int pressed;

	/* The press, then the release. */
	for (pressed = 1; pressed >= 0; pressed--) {
		event = keiui_window_push(window, KL_WINDOW_KEY);
		if (event == NULL)
			return;
		event->code = key;
		event->pressed = pressed;
		event->modifiers = modifiers;
	}
}
