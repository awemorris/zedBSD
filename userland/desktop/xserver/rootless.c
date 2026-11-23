/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Rootless: each mapped top-level X window is a surface of the backend, a
 * window of the system the server is shown on.
 *
 * At the end of a tick, the part of each top-level window that changed is
 * drawn with its children over it (its composite) and shown on its
 * surface, which is opened, resized, retitled and moved to follow the X
 * window: through a Vulkan swapchain on the surface while Vulkan can show
 * it, else through the backend's present.  The backend's pointer,
 * keyboard, focus, sizes and close requests come back as X events.  The
 * system draws the pointer; X cursors are not shown.
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
static void rootless_info(struct x11_window *window, const char *title, const char *class_name, struct x11_surface_info *info);
static int rootless_composite(struct x11server *server, struct x11_window *window, size_t bytes);
static void rootless_swapchain(struct x11server *server, struct x11_window *window);
static void rootless_cpu(struct x11server *server, struct x11_window *window, const char *why);
static int rootless_show(struct x11server *server, struct x11_window *window, int x, int y, int width, int height);
static struct x11_window *rootless_visible_at(struct x11_window *top, int x, int y);
static void rootless_compose(struct x11_window *top, int x, int y, int width, int height);

/*
 * Shows what has changed: each top-level window the changed box touches
 * (all of a new or resized one) is drawn and shown on its surface.  A
 * window whose last frame the system still holds keeps its change for a
 * later tick.
 */
void
x11_rootless_present(
	struct x11server *server)
{
	struct x11_window *window;
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
	for (window = server->root->first_child; window != NULL; window = window->above) {
		/* Its surface, following it; an unmapped window has none. */
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

		/* The part drawn and shown, its place in the window as the damage. */
		rootless_compose(window, left, top, right - left, bottom - top);
		busy = rootless_show(server, window, left - window->x, top - window->y, right - left, bottom - top);
		if (busy != 0) {
			x11_mark_dirty(server, left, top, right - left, bottom - top);
			continue;
		}

		/* The surface shows all of it now. */
		window->surface_fresh = 0;
	}
}

/*
 * Ends the clients whose top-level windows the backend asked to close
 * (at the tick after the backend's events, not in the middle of them).
 */
void
x11_rootless_closing(
	struct x11server *server)
{
	struct x11_window *window;
	struct x11_client *client;
	unsigned index;

	/* Each window asked about, if it is still there and a client's. */
	for (index = 0U; index < server->closing_count; index++) {
		window = x11_window_find(server, server->closing[index]);
		if (window == NULL)
			continue;
		client = x11_client_of(server, window->owner);
		if (client == NULL)
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
	int x;
	int y;

	/* Nothing is held. */
	if (!server->motion_pending)
		return;
	server->motion_pending = 0;

	/* Sent from where the pointer was then, with the buttons held then. */
	x = server->pointer_x;
	y = server->pointer_y;
	server->pointer_x = server->motion_x;
	server->pointer_y = server->motion_y;
	x11_event_motion(server, server->motion_time, (uint16_t)(server->key_state | ((unsigned)server->motion_buttons << 8)));
	server->pointer_x = x;
	server->pointer_y = y;
}

/*
 * Closes a window's surface (and the swapchain on it) and frees its
 * composite (the window is unmapped or destroyed).
 */
void
x11_rootless_forget(
	struct x11server *server,
	struct x11_window *window)
{
	/* The swapchain, before the surface it is on. */
	if (window->swapchain != NULL)
		x11_vulkan_window_close(window->swapchain);
	window->swapchain = NULL;

	/* The surface. */
	if (window->surface != NULL) {
		server->ops->close(server->backend, window->surface);
		server->surface_count--;
	}
	window->surface = NULL;

	/* The composite and its charge. */
	free(window->composite);
	window->composite = NULL;
	x11_memory_release(server, window->owner, window->composite_bytes);
	window->composite_bytes = 0U;
}

/* Brings a top-level window's surface in step with it: opened when mapped, closed when not, its size, title and place. */
static void
rootless_track(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_surface_info info;
	const char *title;
	const char *class_name;
	char class_buffer[64];
	unsigned changed;
	size_t bytes;
	int differs;
	int failed;

	/* An unmapped window, and an InputOnly one, has no surface. */
	if (!window->mapped || window->window_class == X11_INPUT_ONLY) {
		x11_rootless_forget(server, window);
		return;
	}

	/* The title: WM_NAME, or X11 without one. */
	title = window->name;
	if (title[0] == '\0')
		title = ROOTLESS_TITLE;

	/* The X client's class, when it named one. */
	class_name = NULL;
	failed = x11_window_class(server, window->id, class_buffer, sizeof(class_buffer));
	if (failed == 0)
		class_name = class_buffer;

	/* A window without a surface gets one (while there are not too many), and a composite of its size. */
	bytes = (size_t)window->width * (size_t)window->height * sizeof(uint32_t);
	if (window->surface == NULL) {
		if (server->surface_count >= X11_SURFACES_MAX)
			return;
		failed = rootless_composite(server, window, bytes);
		if (failed != 0)
			return;
		rootless_info(window, title, class_name, &info);
		window->surface = server->ops->open(server->backend, window->id, &info);
		if (window->surface == NULL)
			return;
		server->surface_count++;
		window->surface_width = window->width;
		window->surface_height = window->height;
		window->surface_x = window->x;
		window->surface_y = window->y;
		window->surface_fresh = 1;
		(void)snprintf(window->surface_title, sizeof(window->surface_title), "%s", title);

		/* A swapchain on it, unless windows are shown through the backend. */
		rootless_swapchain(server, window);
	}

	/* What changed since the surface was last told. */
	changed = 0U;
	if (window->surface_width != window->width || window->surface_height != window->height)
		changed |= X11_SURFACE_CHANGED_SIZE;
	if (window->surface_x != window->x || window->surface_y != window->y)
		changed |= X11_SURFACE_CHANGED_POSITION;
	differs = strcmp(title, window->surface_title);
	if (differs != 0)
		changed |= X11_SURFACE_CHANGED_TITLE;

	/* A resized window gets a composite of its new size. */
	if ((changed & X11_SURFACE_CHANGED_SIZE) != 0U) {
		failed = rootless_composite(server, window, bytes);
		if (failed != 0) {
			x11_rootless_forget(server, window);
			return;
		}
	}

	/* Nothing else to bring in step. */
	if (changed == 0U)
		return;

	/* The surface told; it counts the pointer's places from the window's corner on the root window. */
	rootless_info(window, title, class_name, &info);
	server->ops->update(server->backend, window->surface, &info, changed);
	window->surface_x = window->x;
	window->surface_y = window->y;
	(void)snprintf(window->surface_title, sizeof(window->surface_title), "%s", title);
	if ((changed & X11_SURFACE_CHANGED_SIZE) == 0U)
		return;

	/* A resized swapchain; one that cannot be leaves the window to the backend's present. */
	if (window->swapchain != NULL) {
		failed = x11_vulkan_window_resize(window->swapchain, window->width, window->height);
		if (failed != 0)
			rootless_cpu(server, window, "cannot resize");
	}

	/* The size it has now, drawn whole. */
	window->surface_width = window->width;
	window->surface_height = window->height;
	window->surface_fresh = 1;
}

/* Fills what a top-level window's surface shows. */
static void
rootless_info(
	struct x11_window *window,
	const char *title,
	const char *class_name,
	struct x11_surface_info *info)
{
	/* A decorated top-level window, where its X window is, with its title and class. */
	memset(info, 0, sizeof(*info));
	info->kind = X11_SURFACE_TOPLEVEL;
	info->x = window->x;
	info->y = window->y;
	info->width = window->width;
	info->height = window->height;
	info->title = title;
	info->class_name = class_name;
}

/*
 * Gives a top-level window a composite of a size, charged to its owner.
 * Returns 0, or nonzero when it cannot have one.
 */
static int
rootless_composite(
	struct x11server *server,
	struct x11_window *window,
	size_t bytes)
{
	uint32_t *composite;
	int error;

	/* The owner is charged for the new size instead of the old. */
	x11_memory_release(server, window->owner, window->composite_bytes);
	window->composite_bytes = 0U;
	error = x11_memory_charge(server, window->owner, bytes);
	if (error != 0)
		return -1;

	/* The memory. */
	composite = realloc(window->composite, bytes);
	if (composite == NULL) {
		x11_memory_release(server, window->owner, bytes);
		return -1;
	}

	/* Succeeded: the composite. */
	window->composite = composite;
	window->composite_bytes = bytes;
	return 0;
}

/* Opens the swapchain of a new surface, unless windows are shown through the backend; one that cannot be opened turns Vulkan off. */
static void
rootless_swapchain(
	struct x11server *server,
	struct x11_window *window)
{
	/* The backend's present shows every window. */
	if (server->cpu_present)
		return;

	/* The swapchain at the window's size. */
	window->swapchain = x11_vulkan_window_open(server, window->surface, window->width, window->height);
	if (window->swapchain != NULL)
		return;

	/* This window and later ones are shown through the backend. */
	server->cpu_present = 1;
	x11_log(server, "X11SERVER PRESENT backend (Vulkan cannot show windows)");
}

/* Leaves a window, and every later one, to the backend's present: its swapchain goes, and it is drawn whole. */
static void
rootless_cpu(
	struct x11server *server,
	struct x11_window *window,
	const char *why)
{
	/* The swapchain goes. */
	x11_log(server, "X11SERVER PRESENT backend (Vulkan %s window 0x%x)", why, (unsigned)window->id);
	x11_vulkan_window_close(window->swapchain);
	window->swapchain = NULL;
	server->cpu_present = 1;
	window->surface_fresh = 1;
}

/*
 * Shows a part of a top-level window's composite (from its corner) on its
 * surface.  Returns 0 when it was shown, or nonzero when the system still
 * holds the last frame or it could not be shown (the part waits for a
 * later tick).
 */
static int
rootless_show(
	struct x11server *server,
	struct x11_window *window,
	int x,
	int y,
	int width,
	int height)
{
	struct x11_rect part;
	int status;

	/* Through the swapchain: the whole image each frame. */
	if (window->swapchain != NULL) {
		status = x11_vulkan_window_present(window->swapchain, window->composite);
		if (status >= 0)
			return status;

		/* Vulkan failed: this window and later ones are shown through the backend, whole. */
		rootless_cpu(server, window, "failed on");
		x = 0;
		y = 0;
		width = (int)window->width;
		height = (int)window->height;
	}

	/* Through the backend, the part as the damage. */
	part.x = x;
	part.y = y;
	part.width = (unsigned)width;
	part.height = (unsigned)height;
	status = server->ops->present(server->backend, window->surface, window->composite, window->width, &part, 1U);
	if (status != 0)
		return 1;

	/* Succeeded: the frame is the system's. */
	return 0;
}

/*
 * Returns the deepest window drawn at a place on the root window within a
 * top-level window: the topmost mapped InputOutput child there, and its
 * own in turn (the top-level window when none is).
 */
static struct x11_window *
rootless_visible_at(
	struct x11_window *top,
	int x,
	int y)
{
	struct x11_window *window;
	struct x11_window *child;

	/* Down the tree, into the topmost drawn child under the place each time. */
	window = top;
	child = window->last_child;
	while (child != NULL) {
		if (child->mapped &&
		    child->window_class == X11_INPUT_OUTPUT &&
		    x >= child->x &&
		    y >= child->y &&
		    x < child->x + child->width &&
		    y < child->y + child->height) {
			window = child;
			child = window->last_child;
			continue;
		}
		child = child->below;
	}

	/* Succeeded: the window drawn there. */
	return window;
}

/* Draws a part of a top-level window (on the root window's coordinates) into its composite, its children over it. */
static void
rootless_compose(
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
	children = 0;
	for (window = top->first_child; window != NULL && !children; window = window->above) {
		if (window->mapped && window->window_class == X11_INPUT_OUTPUT)
			children = 1;
	}
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
			window = rootless_visible_at(top, column, row);
			if (window->pixels == NULL)
				window = top;
			top->composite[(size_t)(row - top->y) * top->width + (size_t)(column - top->x)] =
			    window->pixels[(size_t)(row - window->y) * window->width + (size_t)(column - window->x)];
		}
	}
}

/*
 * Takes a key from the backend: pressed or released, sent from the focus
 * window (or the window under the pointer inside it).
 */
void
x11server_backend_key(
	struct x11server *server,
	uint8_t keycode,
	int pressed,
	uint32_t time,
	uint16_t state)
{
	/* The modifiers held are the keyboard's state from now on. */
	server->key_state = state;
	if (keycode == 0U)
		return;

	/* The press or the release, with the buttons held. */
	x11_event_key(server, keycode, pressed, time, (uint16_t)(state | ((unsigned)server->buttons << 8)));
}

/*
 * Takes the pointer from the backend: where it is, and a button that
 * changed.  A press that reaches a client grabs the pointer for it until
 * every button is up; it focuses and raises the top-level window under
 * the pointer (the rootless server is its window manager).  Moves between
 * buttons are held back so only the last of a burst is sent.
 */
void
x11server_backend_pointer(
	struct x11server *server,
	const struct x11_pointer_frame *frame)
{
	struct x11_window *top;
	uint16_t state;

	/* A button's change sends the move held back first, so the order holds. */
	server->buttons = frame->buttons_before;
	if (frame->button != 0U)
		x11_rootless_flush_motion(server);

	/* Where the pointer is. */
	server->pointer_x = frame->x;
	server->pointer_y = frame->y;

	/* A move alone is held back (the newest replaces an older one). */
	if (frame->button == 0U) {
		server->motion_pending = 1;
		server->motion_time = frame->time;
		server->motion_x = server->pointer_x;
		server->motion_y = server->pointer_y;
		server->motion_buttons = server->buttons;
		return;
	}

	/* The button's press or release, with the buttons held before it. */
	state = (uint16_t)(server->key_state | ((unsigned)server->buttons << 8));
	x11_event_button(server, frame->button, frame->pressed, frame->time, state);
	server->buttons = frame->buttons_after;

	/* A press focuses and raises the top-level window under the pointer. */
	if (frame->pressed) {
		top = x11_window_top_level(x11_window_at(server, server->pointer_x, server->pointer_y));
		if (top != server->root) {
			x11_window_raise(server, top);
			server->focus = top->id;
		}
	}

	/* With every button up, the grab ends. */
	if (server->buttons == 0U) {
		server->grab_owner = X11_NO_CLIENT;
		server->grab_window = 0U;
	}
}

/*
 * Hears that the pointer or the keyboard came into a surface: its X
 * window is raised, and takes the focus for the keyboard.
 */
void
x11server_backend_enter(
	struct x11server *server,
	uint32_t window,
	int keyboard)
{
	struct x11_window *entered;

	/* The X window, raised so that the pointer's hits find it. */
	entered = x11_window_find(server, window);
	if (entered == NULL || entered == server->root)
		return;
	x11_window_raise(server, entered);

	/* The keyboard's focus. */
	if (keyboard)
		server->focus = window;
}

/*
 * Hears that the system moved a surface or gave it a size (the user moved
 * or resized it, or the system corrected what the core asked for): the X
 * window takes the place (X11_SURFACE_ANY: not known, the place stays) and
 * the size, its client is told, and a new size is drawn again by it.  The
 * surface is not told back what it reported.
 */
void
x11server_backend_configure(
	struct x11server *server,
	uint32_t window,
	int32_t x,
	int32_t y,
	int width,
	int height)
{
	struct x11_window *changed;

	/* The X window, at a size it can have. */
	changed = x11_window_find(server, window);
	if (changed == NULL ||
	    changed == server->root ||
	    width <= 0 ||
	    height <= 0 ||
	    width > ROOTLESS_SIZE_MAX ||
	    height > ROOTLESS_SIZE_MAX)
		return;

	/* A place not known, or outside X's coordinates, stays as it is. */
	if (x < -ROOTLESS_SIZE_MAX || x > ROOTLESS_SIZE_MAX || y < -ROOTLESS_SIZE_MAX || y > ROOTLESS_SIZE_MAX) {
		x = X11_SURFACE_ANY;
		y = X11_SURFACE_ANY;
	}

	/* Succeeded: the window takes them, and its clients are told. */
	x11_window_backend_configure(server, changed, (int)x, (int)y, (uint16_t)width, (uint16_t)height);
}

/*
 * Hears that the system asked a surface to close: its client ends at the
 * next tick.
 */
void
x11server_backend_close(
	struct x11server *server,
	uint32_t window)
{
	/* Remembered, when there is room. */
	if (server->closing_count < X11_CLOSING_MAX) {
		server->closing[server->closing_count] = window;
		server->closing_count++;
	}
}

/*
 * Hears that the system minimized a surface or brought it back.  The
 * server does not keep ICCCM's Iconic state yet, so nothing changes.
 */
void
x11server_backend_state(
	struct x11server *server,
	uint32_t window,
	int iconic)
{
	/* Not kept yet. */
	(void)server;
	(void)window;
	(void)iconic;
}
