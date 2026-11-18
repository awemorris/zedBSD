/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Rootless: each mapped top-level X window is a window of the desktop.
 *
 * At the end of a pass, the part of each top-level window that changed is
 * drawn with its children over it (its composite) and handed to its
 * desktop window, which is opened, resized, retitled and moved to follow
 * the X window.  The desktop's pointer, keyboard, focus, sizes and close
 * requests come back as X events.  The desktop draws the pointer; X
 * cursors are not shown.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The title of a window with no WM_NAME. */
#define ROOTLESS_TITLE		"X11"

/* The largest size the desktop may give a window. */
#define ROOTLESS_SIZE_MAX	16384

static void rootless_track(struct x11server *server, struct x11_window *window);
static int rootless_has_children(struct x11server *server, const struct x11_window *top);
static void rootless_compose(struct x11server *server, struct x11_window *top, int x, int y, int width, int height);
static void rootless_key(void *context, uint8_t keycode, int pressed, uint32_t time, uint16_t state);
static void rootless_pointer(void *context, const struct x11_pointer_frame *frame);
static void rootless_enter(void *context, uint32_t window, int keyboard);
static void rootless_configure(void *context, uint32_t window, int width, int height);
static void rootless_close(void *context, uint32_t window);

/*
 * What the desktop's connection tells the server.
 */
const struct x11_wayland_callbacks x11_rootless_callbacks = {
	rootless_key, rootless_pointer, rootless_enter, rootless_configure, rootless_close, x11_selection_wayland, x11_selection_send,
	x11_selection_primary, x11_selection_primary_send
};

/*
 * Shows what has changed: each top-level window the changed box touches
 * (all of a new or resized one) is drawn and handed to its desktop window.
 * A window whose buffers the desktop still holds keeps its change for a
 * later pass.
 */
void
x11_rootless_present(
	struct x11server *server)
{
	struct x11_window *window;
	unsigned slot;
	int box_x0;
	int box_y0;
	int box_x1;
	int box_y1;
	int left;
	int top;
	int right;
	int bottom;
	int busy;

	/* Nothing has changed. */
	if (!server->dirty)
		return;

	/* The box, taken; a window that cannot show its part puts it back. */
	box_x0 = server->dirty_x0;
	box_y0 = server->dirty_y0;
	box_x1 = server->dirty_x1;
	box_y1 = server->dirty_y1;
	server->dirty = 0;

	/* Each top-level window. */
	for (slot = 1U; slot < server->window_count; slot++) {
		window = &server->windows[slot];
		if (window->parent != X11_ROOT_XID)
			continue;

		/* Its desktop window, following it; an unmapped window has none. */
		rootless_track(server, window);
		if (window->surface == NULL)
			continue;

		/* The part of the box inside it, or all of it when it must be drawn whole. */
		left = box_x0;
		top = box_y0;
		right = box_x1;
		bottom = box_y1;
		if (window->surface_fresh) {
			left = window->x;
			top = window->y;
			right = window->x + window->width;
			bottom = window->y + window->height;
		}

		/* Clipped to the window. */
		if (left < window->x)
			left = window->x;
		if (top < window->y)
			top = window->y;
		if (right > window->x + window->width)
			right = window->x + window->width;
		if (bottom > window->y + window->height)
			bottom = window->y + window->height;
		if (left >= right || top >= bottom)
			continue;

		/* The part drawn and handed over, its place in the window as the damage. */
		rootless_compose(server, window, left, top, right - left, bottom - top);
		busy = x11_wayland_window_present(window->surface, window->composite, left - window->x, top - window->y, right - left, bottom - top);
		if (busy != 0) {
			x11_mark_dirty(server, left, top, right - left, bottom - top);
			continue;
		}

		/* The desktop window shows all of it now. */
		window->surface_fresh = 0;
	}
}

/*
 * Ends the clients whose top-level windows the desktop asked to close
 * (after the desktop's events, not in the middle of them).
 */
void
x11_rootless_closing(
	struct x11server *server)
{
	struct x11_window *window;
	unsigned index;

	/* Each window asked about, if it is still there and a client's. */
	for (index = 0U; index < server->closing_count; index++) {
		window = x11_window_find(server, server->closing[index]);
		if (window == NULL || window->owner >= X11_MAX_CLIENTS)
			continue;

		/* Its client goes, with all its windows. */
		x11_client_close(server, window->owner);
	}

	/* None is left to close. */
	server->closing_count = 0U;
}

/*
 * Sends the pointer motion held back, if any: only the last of a burst of
 * moves reaches the client.
 */
void
x11_rootless_flush_motion(
	struct x11server *server)
{
	struct x11_window *window;
	int x;
	int y;

	/* Nothing is held. */
	if (!server->motion_pending)
		return;
	server->motion_pending = 0;

	/* The window must still be there and want motion. */
	window = x11_window_find(server, server->motion_window);
	if (window == NULL || (window->event_mask & X11_MASK_POINTER_MOTION) == 0U)
		return;

	/* Sent from where the pointer was then. */
	x = server->pointer_x;
	y = server->pointer_y;
	server->pointer_x = server->motion_x;
	server->pointer_y = server->motion_y;
	x11_input_event(server, server->motion_client, X11_EVENT_MOTION_NOTIFY, window->id, 0U, server->motion_time, server->motion_buttons);
	server->pointer_x = x;
	server->pointer_y = y;
}

/*
 * Closes a window's desktop window and frees its composite (the window is
 * unmapped or destroyed).
 */
void
x11_rootless_forget(
	struct x11_window *window)
{
	/* The desktop window. */
	if (window->surface != NULL)
		x11_wayland_window_close(window->surface);
	window->surface = NULL;

	/* The composite. */
	free(window->composite);
	window->composite = NULL;
}

/* Brings a top-level window's desktop window in step with it: opened when mapped, closed when not, its size, title and place. */
static void
rootless_track(
	struct x11server *server,
	struct x11_window *window)
{
	const char *title;
	const char *app_id;
	char class_name[64];
	uint32_t *composite;
	size_t bytes;
	int differs;
	int failed;

	/* An unmapped window has no desktop window. */
	if (!window->mapped) {
		x11_rootless_forget(window);
		return;
	}

	/* The title: WM_NAME, or X11 without one. */
	title = window->name;
	if (title[0] == '\0')
		title = ROOTLESS_TITLE;

	/* A window without a desktop window gets one, and a composite of its size. */
	bytes = (size_t)window->width * (size_t)window->height * sizeof(uint32_t);
	if (window->surface == NULL) {
		composite = realloc(window->composite, bytes);
		if (composite == NULL)
			return;
		window->composite = composite;
		app_id = NULL;
		failed = x11_window_class(server, window->id, class_name, sizeof(class_name));
		if (failed == 0)
			app_id = class_name;
		window->surface = x11_wayland_window_open(server->wayland, window->id, title, app_id, window->width, window->height);
		if (window->surface == NULL)
			return;
		window->surface_width = window->width;
		window->surface_height = window->height;
		window->surface_fresh = 1;
		(void)snprintf(window->surface_title, sizeof(window->surface_title), "%s", title);
	}

	/* A resized window gets buffers and a composite of its new size, and is drawn whole. */
	if (window->surface_width != window->width || window->surface_height != window->height) {
		composite = realloc(window->composite, bytes);
		if (composite == NULL) {
			x11_rootless_forget(window);
			return;
		}

		/* The composite of the new size. */
		window->composite = composite;
		failed = x11_wayland_window_resize(window->surface, window->width, window->height);
		if (failed != 0) {
			x11_rootless_forget(window);
			return;
		}

		/* The size it has now, drawn whole. */
		window->surface_width = window->width;
		window->surface_height = window->height;
		window->surface_fresh = 1;
	}

	/* A renamed window gets its new title. */
	differs = strcmp(title, window->surface_title);
	if (differs != 0) {
		x11_wayland_window_title(window->surface, title);
		(void)snprintf(window->surface_title, sizeof(window->surface_title), "%s", title);
	}

	/* The pointer's places are counted from the window's corner on the root window. */
	x11_wayland_window_move(window->surface, window->x, window->y);
}

/* Reports whether a top-level window has a mapped child drawn over it. */
static int
rootless_has_children(
	struct x11server *server,
	const struct x11_window *top)
{
	unsigned slot;

	/* Any mapped window whose parent it is. */
	for (slot = 1U; slot < server->window_count; slot++) {
		if (server->windows[slot].mapped && server->windows[slot].parent == top->id)
			return 1;
	}

	/* None. */
	return 0;
}

/* Draws a part of a top-level window (on the root window's coordinates) into its composite, its children over it. */
static void
rootless_compose(
	struct x11server *server,
	struct x11_window *top,
	int x,
	int y,
	int width,
	int height)
{
	struct x11_window *window;
	int children;
	int row;
	int column;

	/* A window alone is copied row by row. */
	children = rootless_has_children(server, top);
	if (!children) {
		for (row = y; row < y + height; row++) {
			memcpy(top->composite + (size_t)(row - top->y) * top->width + (size_t)(x - top->x),
			       top->pixels + (size_t)(row - top->y) * top->width + (size_t)(x - top->x),
			       (size_t)width * sizeof(uint32_t));
		}

		/* The window is drawn. */
		return;
	}

	/* Otherwise each pixel comes from the topmost window there. */
	for (row = y; row < y + height; row++) {
		for (column = x; column < x + width; column++) {
			window = x11_window_child_at(server, top->id, column, row);
			if (window == NULL || window->pixels == NULL)
				window = top;
			top->composite[(size_t)(row - top->y) * top->width + (size_t)(column - top->x)] =
			    window->pixels[(size_t)(row - window->y) * window->width + (size_t)(column - window->x)];
		}
	}
}

/* A key from the desktop: pressed or released in the focus window (or the one under the pointer). */
static void
rootless_key(
	void *context,
	uint8_t keycode,
	int pressed,
	uint32_t time,
	uint16_t state)
{
	struct x11server *server;
	struct x11_window *window;
	uint8_t type;

	/* The modifiers held are the keyboard's state from now on. */
	server = context;
	server->key_state = state;
	if (keycode == 0U)
		return;

	/* The focus window, or the one under the pointer when the focus is gone. */
	window = x11_window_find(server, server->focus);
	if (window == NULL)
		window = x11_window_at(server, server->pointer_x, server->pointer_y);

	/* The press or the release. */
	type = X11_EVENT_KEY_RELEASE;
	if (pressed)
		type = X11_EVENT_KEY_PRESS;
	x11_input_event(server, window->owner, type, window->id, keycode, time, state);
}

/*
 * The pointer from the desktop: where it is, and a button that changed.
 * A press grabs the pointer for its window until every button is up,
 * focuses and raises it; moves between buttons are held back so only the
 * last of a burst is sent.
 */
static void
rootless_pointer(
	void *context,
	const struct x11_pointer_frame *frame)
{
	struct x11server *server;
	struct x11_window *window;
	struct x11_window *top;
	unsigned owner;
	uint8_t type;

	/* A button's change sends the move held back first, so the order holds. */
	server = context;
	server->buttons = frame->buttons_before;
	if (frame->button != 0U)
		x11_rootless_flush_motion(server);

	/* Where the pointer is. */
	server->pointer_x = frame->x;
	server->pointer_y = frame->y;

	/* The window it speaks to: the grab's, or the one under it. */
	window = NULL;
	if (server->grab_owner >= 0)
		window = x11_window_find(server, server->grab_window);
	if (window == NULL)
		window = x11_window_at(server, server->pointer_x, server->pointer_y);
	owner = window->owner;
	if (server->grab_owner >= 0)
		owner = (unsigned)server->grab_owner;

	/* A move alone is held back (the newest replaces an older one). */
	if (frame->button == 0U) {
		server->motion_pending = 1;
		server->motion_client = owner;
		server->motion_window = window->id;
		server->motion_time = frame->time;
		server->motion_x = server->pointer_x;
		server->motion_y = server->pointer_y;
		server->motion_buttons = server->buttons;
		return;
	}

	/* The button's press or release, with the buttons held before it. */
	type = X11_EVENT_BUTTON_RELEASE;
	if (frame->pressed)
		type = X11_EVENT_BUTTON_PRESS;
	x11_input_event(server, owner, type, window->id, frame->button, frame->time, server->buttons);
	server->buttons = frame->buttons_after;

	/* A press focuses and raises the window's top level, and grabs the pointer for it. */
	if (frame->pressed) {
		server->focus = window->id;
		top = x11_window_top_level(server, window);
		x11_window_raise(server, top);
		window = x11_window_find(server, server->focus);
		if (server->grab_owner < 0 &&
		    window != NULL &&
		    window->owner < X11_MAX_CLIENTS) {
			server->grab_owner = (int)window->owner;
			server->grab_window = window->id;
		}
	}

	/* With every button up, the grab ends. */
	if (server->buttons == 0U) {
		server->grab_owner = -1;
		server->grab_window = 0U;
	}
}

/* The pointer or the keyboard came into a desktop window: its X window is raised, and takes the focus for the keyboard. */
static void
rootless_enter(
	void *context,
	uint32_t window,
	int keyboard)
{
	struct x11server *server;
	struct x11_window *entered;

	/* The X window, raised so that the pointer's hits find it. */
	server = context;
	entered = x11_window_find(server, window);
	if (entered == NULL)
		return;
	x11_window_raise(server, entered);

	/* The keyboard's focus. */
	if (keyboard)
		server->focus = window;
}

/* The desktop gave a window a size: the X window is resized, told, and drawn again by its client. */
static void
rootless_configure(
	void *context,
	uint32_t window,
	int width,
	int height)
{
	struct x11server *server;
	struct x11_window *resized;
	int failed;

	/* The X window, at a size it can have. */
	server = context;
	resized = x11_window_find(server, window);
	if (resized == NULL ||
	    width <= 0 ||
	    height <= 0 ||
	    width > ROOTLESS_SIZE_MAX ||
	    height > ROOTLESS_SIZE_MAX)
		return;

	/* Pixels of the new size. */
	failed = x11_window_resize(resized, (uint16_t)width, (uint16_t)height);
	if (failed != 0)
		return;

	/* The size, told to the client, which draws the window again. */
	resized->width = (uint16_t)width;
	resized->height = (uint16_t)height;
	x11_expose(server, resized);
	x11_mark_dirty(server, resized->x, resized->y, resized->width, resized->height);
}

/* The desktop asked a window to close: its client ends after the desktop's events. */
static void
rootless_close(
	void *context,
	uint32_t window)
{
	struct x11server *server;

	/* Remembered, when there is room. */
	server = context;
	if (server->closing_count < X11_MAX_WINDOWS) {
		server->closing[server->closing_count] = window;
		server->closing_count++;
	}
}
