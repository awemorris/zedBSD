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
 * At the end of a tick, the part of each top-level window's pixels (it
 * and its inferiors, draw.c) that changed is shown on its surface, which
 * is opened, resized, retitled and moved to follow the X window: through
 * a Vulkan swapchain on the surface while Vulkan can show it (from the
 * window's image on the GPU when it has one), else through the backend's
 * present (the pixels in memory brought up to date first).  The backend's pointer,
 * keyboard, focus, sizes and close requests come back as X events.  The
 * system draws the pointer; X cursors are not shown.
 *
 * A rooted server shows its root window the same way, as the one surface
 * (rooted.c decides while it is open), and leaves the stacking and the
 * focus of the windows to its clients.
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
static void rootless_size(const struct x11_window *window, int *width, int *height);
static void rootless_info(struct x11server *server, struct x11_window *window, const char *title, const char *class_name, struct x11_surface_info *info);
static void rootless_extras(struct x11server *server, struct x11_window *window, unsigned changed, struct x11_surface_info *info, struct x11_rect **shape, uint32_t **icon);
static void rootless_swapchain(struct x11server *server, struct x11_window *window);
static void rootless_cpu(struct x11server *server, struct x11_window *window, const char *why);
static int rootless_shaped_present(struct x11server *server, struct x11_window *window);
static int rootless_show(struct x11server *server, struct x11_window *window, int x, int y, int width, int height);

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
	struct x11_window *first;
	int box_x0;
	int box_y0;
	int box_x1;
	int box_y1;
	int left;
	int top;
	int right;
	int bottom;
	int busy;

	/*
	 * The windows shown: the root window's children, or a rooted server's
	 * root window alone (none while it has no client).
	 */
	first = server->root->first_child;
	if (server->mode == X11SERVER_ROOTED)
		first = x11_rooted_shown(server);

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
	for (window = first; window != NULL; window = window->above) {
		/* Its surface, following it; an unmapped window has none, and a minimized one (without pixels) shows nothing. */
		rootless_track(server, window);
		if (window->surface == NULL)
			continue;
		if (!window->mapped || window->image.pixels == NULL)
			continue;

		/* The part of the box inside it, or all of it when it must be drawn whole. */
		left = box_x0;
		top = box_y0;
		right = box_x1;
		bottom = box_y1;
		if (window->surface_fresh) {
			left = window->x;
			top = window->y;
			right = window->x + window->image.width;
			bottom = window->y + window->image.height;
		}

		/* Clipped to its pixels. */
		if (left < window->x)
			left = window->x;
		if (top < window->y)
			top = window->y;
		if (right > window->x + window->image.width)
			right = window->x + window->image.width;
		if (bottom > window->y + window->image.height)
			bottom = window->y + window->image.height;
		if (left >= right || top >= bottom)
			continue;

		/* The part shown, its place in the window as the damage. */
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
 * Closes the top-level windows the backend asked to close (at the tick
 * after the backend's events, not in the middle of them): the window
 * manager asks the client, or ends it.
 */
void
x11_rootless_closing(
	struct x11server *server)
{
	struct x11_window *window;
	unsigned index;

	/* Each window asked about, if it is still there; a rooted server's root window ends its clients. */
	for (index = 0U; index < server->closing_count; index++) {
		window = x11_window_find(server, server->closing[index]);
		if (window == NULL)
			continue;
		if (window == server->root) {
			x11_rooted_close(server);
			continue;
		}

		/* The window manager closes it as its client asked to be. */
		x11_wm_close(server, window);
	}

	/* None is left to close. */
	server->closing_count = 0U;
}

/*
 * Sends the pointer motion held back, if any: only the last of a burst of
 * moves reaches the clients.
 */
void
x11_rootless_flush_motion(
	struct x11server *server)
{
	/* Nothing is held. */
	if (!server->motion_pending)
		return;
	server->motion_pending = 0;

	/* Succeeded: the input hears the move. */
	x11_input_motion(server, server->motion_time, server->motion_x, server->motion_y);
}

/*
 * Closes a window's surface (and the swapchain on it): the window is
 * unmapped, destroyed or no longer a top-level window.
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

	/* A new surface is shown the way its window asks for then. */
	window->surface_present = 0;
}

/* Brings a top-level window's surface in step with it: opened when mapped, closed when not, its size, title and place. */
static void
rootless_track(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_surface_info info;
	struct x11_rect *shape;
	uint32_t *icon;
	const char *title;
	const char *class_name;
	char class_buffer[64];
	char title_buffer[X11_TITLE_MAX];
	unsigned changed;
	int width;
	int height;
	int differs;
	int failed;
	int kept;
	int shaped;

	/*
	 * An unmapped window has no surface, unless the window manager keeps
	 * it minimized (without pixels); an InputOnly one and a mapped one
	 * without pixels have none.
	 */
	kept = x11_wm_keeps_surface(window);
	if ((!window->mapped && !kept) || window->window_class == X11_INPUT_ONLY || (window->mapped && window->image.pixels == NULL)) {
		x11_rootless_forget(server, window);
		window->surface_reopen = 0;
		return;
	}

	/* The title: _NET_WM_NAME or WM_NAME, or X11 without one. */
	x11_wm_title(server, window, title_buffer, sizeof(title_buffer));
	title = title_buffer;
	if (title[0] == '\0')
		title = ROOTLESS_TITLE;

	/* The X client's class, when it named one. */
	class_name = NULL;
	failed = x11_window_class(server, window->id, class_buffer, sizeof(class_buffer));
	if (failed == 0)
		class_name = class_buffer;

	/*
	 * A shaped window on a backend that shows shapes only through its
	 * present loses its swapchain: the surface goes, and is opened again
	 * below at once and drawn whole this tick, so no frame is lost.
	 */
	shaped = rootless_shaped_present(server, window);
	if (shaped && window->swapchain != NULL) {
		x11_log(server, "X11SERVER PRESENT backend for the shaped window 0x%x", (unsigned)window->id);
		x11_rootless_forget(server, window);
		window->surface_reopen = 1;
	}

	/* A window without a surface gets one, while there are not too many; one opened again keeps its place. */
	if (window->surface == NULL) {
		if (server->surface_count >= X11_SURFACES_MAX)
			return;
		rootless_info(server, window, title, class_name, &info);
		if (window->surface_reopen)
			info.default_place = 0;
		rootless_extras(server, window, X11_SURFACE_CHANGED_SHAPE | X11_SURFACE_CHANGED_ICON, &info, &shape, &icon);
		window->surface = server->ops->open(server->backend, window->id, &info);
		free(shape);
		free(icon);
		if (window->surface == NULL)
			return;
		server->surface_count++;
		window->surface_reopen = 0;
		window->surface_width = (uint16_t)info.width;
		window->surface_height = (uint16_t)info.height;
		window->surface_x = window->x;
		window->surface_y = window->y;
		window->surface_fresh = 1;
		(void)snprintf(window->surface_title, sizeof(window->surface_title), "%s", title);

		/* The surface has all of the window manager's state. */
		window->wm_changed = 0U;
	}

	/* A shaped window's surface stays with the backend's present from now on, shape or not. */
	if (shaped)
		window->surface_present = 1;

	/*
	 * A swapchain on it once it has pixels, unless it or every window is
	 * shown through the backend (a surface Vulkan failed on goes, to be
	 * opened again).
	 */
	if (window->swapchain == NULL && window->image.pixels != NULL && !window->surface_present) {
		rootless_swapchain(server, window);
		if (window->surface == NULL)
			return;
	}

	/* What changed since the surface was last told: the window manager's part first. */
	rootless_size(window, &width, &height);
	changed = window->wm_changed;
	if (window->surface_width != width || window->surface_height != height)
		changed |= X11_SURFACE_CHANGED_SIZE;
	if (window->surface_x != window->x || window->surface_y != window->y)
		changed |= X11_SURFACE_CHANGED_POSITION;
	differs = strcmp(title, window->surface_title);
	if (differs != 0)
		changed |= X11_SURFACE_CHANGED_TITLE;

	/* A shape is cut to the window's size, so a shaped window's new size is a new shape. */
	if ((changed & X11_SURFACE_CHANGED_SIZE) != 0U && window->shape != NULL)
		changed |= X11_SURFACE_CHANGED_SHAPE;

	/* Nothing else to bring in step. */
	if (changed == 0U)
		return;

	/* The surface told; it counts the pointer's places from the window's corner on the root window. */
	rootless_info(server, window, title, class_name, &info);
	rootless_extras(server, window, changed, &info, &shape, &icon);
	server->ops->update(server->backend, window->surface, &info, changed);
	free(shape);
	free(icon);
	window->wm_changed = 0U;
	window->surface_x = window->x;
	window->surface_y = window->y;
	(void)snprintf(window->surface_title, sizeof(window->surface_title), "%s", title);
	if ((changed & X11_SURFACE_CHANGED_SIZE) == 0U)
		return;

	/* A resized swapchain; one that cannot be leaves the window to the backend's present. */
	if (window->swapchain != NULL) {
		failed = x11_vulkan_window_resize(window->swapchain, (unsigned)width, (unsigned)height);
		if (failed != 0)
			rootless_cpu(server, window, "cannot resize");
	}

	/* The size it has now, drawn whole. */
	window->surface_width = (uint16_t)width;
	window->surface_height = (uint16_t)height;
	window->surface_fresh = 1;
}

/*
 * Works out the size of a top-level window's surface: that of its pixels,
 * or, for a minimized window (which has none), the size they will have.
 */
static void
rootless_size(
	const struct x11_window *window,
	int *width,
	int *height)
{
	/* The pixels' size. */
	if (window->image.pixels != NULL) {
		*width = window->image.width;
		*height = window->image.height;
		return;
	}

	/* Succeeded: the window's size, as far as pixels may be. */
	*width = window->width;
	if (*width > X11_IMAGE_SIDE_MAX)
		*width = X11_IMAGE_SIDE_MAX;
	*height = window->height;
	if (*height > X11_IMAGE_SIDE_MAX)
		*height = X11_IMAGE_SIDE_MAX;
}

/* Fills what a top-level window's surface shows. */
static void
rootless_info(
	struct x11server *server,
	struct x11_window *window,
	const char *title,
	const char *class_name,
	struct x11_surface_info *info)
{
	int width;
	int height;

	/* Where its X window is, with its title and class. */
	memset(info, 0, sizeof(*info));
	info->x = window->x;
	info->y = window->y;
	rootless_size(window, &width, &height);
	info->width = (unsigned)width;
	info->height = (unsigned)height;
	info->title = title;
	info->class_name = class_name;

	/* A rooted server's root window is no window manager's. */
	if (window == server->root) {
		info->kind = X11_SURFACE_ROOT;
		return;
	}

	/* Succeeded: its kind, state, owner and limits, as its window manager has them. */
	x11_wm_info(server, window, info);
}

/*
 * Fills the parts of what a surface shows that are made only when they
 * changed: the window's shape and its icon (each malloc'd into *shape and
 * *icon, NULL when not made, freed by the caller once the backend has
 * them).  Without memory for one, the surface is not told of it.
 */
static void
rootless_extras(
	struct x11server *server,
	struct x11_window *window,
	unsigned changed,
	struct x11_surface_info *info,
	struct x11_rect **shape,
	uint32_t **icon)
{
	unsigned count;
	int error;

	/* Nothing made yet. */
	*shape = NULL;
	*icon = NULL;

	/* The shape (SHAPE's bounding region on the window's pixels). */
	if ((changed & X11_SURFACE_CHANGED_SHAPE) != 0U) {
		count = 0U;
		error = x11_shape_surface(window, shape, &count);
		if (error == 0 && *shape != NULL) {
			info->shaped = 1;
			info->shape = *shape;
			info->shape_count = count;
		}
	}

	/* Succeeded: the icon (_NET_WM_ICON), when it has one. */
	if ((changed & X11_SURFACE_CHANGED_ICON) != 0U) {
		error = x11_wm_icon(server, window, icon, &info->icon_width, &info->icon_height);
		if (error == 0)
			info->icon = *icon;
	}
}

/* Opens the swapchain of a new surface, unless windows are shown through the backend; one that cannot be opened turns Vulkan's windows off. */
static void
rootless_swapchain(
	struct x11server *server,
	struct x11_window *window)
{
	/* The backend's present shows every window. */
	if (server->cpu_present)
		return;

	/* The swapchain at the window's size. */
	window->swapchain = x11_vulkan_window_open(server, window->surface, (unsigned)window->image.width, (unsigned)window->image.height);
	if (window->swapchain != NULL)
		return;

	/* This window (from a new surface, the next tick) and later ones are shown through the backend. */
	rootless_cpu(server, window, "cannot show");
}

/*
 * Leaves a window, and every later one, to the backend's present: its
 * swapchain goes with its surface (a system window that was shown
 * through Vulkan may take no other pixels), which is opened again
 * without one and drawn whole.
 */
static void
rootless_cpu(
	struct x11server *server,
	struct x11_window *window,
	const char *why)
{
	/* The swapchain and the surface go; the next present opens the surface again, where it was. */
	x11_log(server, "X11SERVER PRESENT backend (Vulkan %s window 0x%x)", why, (unsigned)window->id);
	x11_rootless_forget(server, window);
	window->surface_reopen = 1;
	server->cpu_present = 1;
}

/*
 * Tells whether a window must be shown through the backend's present: it
 * has a bounding shape, and the backend shows shapes only there.
 */
static int
rootless_shaped_present(
	struct x11server *server,
	struct x11_window *window)
{
	int bounded;

	/* A backend whose swapchains show shapes as its present does. */
	if ((server->ops->flags & X11_BACKEND_SHAPED_PRESENT) == 0U)
		return 0;

	/* An unshaped window. */
	bounded = x11_shape_bounded(window);
	if (!bounded)
		return 0;

	/* Succeeded: shown through the present. */
	return 1;
}

/*
 * Shows a part of a top-level window's pixels (from its corner) on its
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
	struct x11_box box;
	int status;

	/* Through the swapchain: the whole image each frame, from the GPU when it is there. */
	if (window->swapchain != NULL) {
		if (window->image.gpu != NULL) {
			status = x11_vulkan_window_present_image(window->swapchain, &window->image);
		} else {
			status = x11_vulkan_window_present(window->swapchain, window->image.pixels, window->image.stride);
		}
		if (status >= 0)
			return status;

		/* Vulkan failed: this window and later ones are shown through the backend, whole, from a new surface. */
		rootless_cpu(server, window, "failed on");
		return 1;
	}

	/* The part's pixels in memory brought up to date with the GPU's. */
	box.x0 = x;
	box.y0 = y;
	box.x1 = x + width;
	box.y1 = y + height;
	x11_render_image_memory_box(&window->image, &box);

	/* Through the backend, the part as the damage. */
	part.x = x;
	part.y = y;
	part.width = (unsigned)width;
	part.height = (unsigned)height;
	status = server->ops->present(server->backend, window->surface, window->image.pixels, window->image.stride, &part, 1U);
	if (status != 0)
		return 1;

	/* Succeeded: the frame is the system's. */
	return 0;
}

/*
 * Takes a key from the backend: its keycode (its evdev code plus 8),
 * pressed or released.
 */
void
x11server_backend_key(
	struct x11server *server,
	uint8_t keycode,
	int pressed,
	uint32_t time)
{
	/* The move held back goes first, so the key is seen where the pointer is. */
	x11_rootless_flush_motion(server);

	/* Succeeded: the input handles it. */
	x11_input_key(server, keycode, pressed, time);
}

/*
 * Takes the keyboard's map from the backend (the user's layout): the
 * clients hear of it when it changed.
 */
void
x11server_backend_keymap(
	struct x11server *server,
	const struct x11_keymap *keymap)
{
	/* Succeeded: the input's map. */
	x11_input_keymap(server, keymap);
}

/*
 * Takes a character the backend's input method typed (a Unicode code
 * point): the X clients hear it as a key whose keysym is the character.
 */
void
x11server_backend_text(
	struct x11server *server,
	uint32_t character,
	uint32_t time)
{
	/* The move held back goes first, so the character is seen where the pointer is. */
	x11_rootless_flush_motion(server);

	/* Succeeded: the input types it. */
	x11_input_text(server, character, time);
}

/*
 * Takes the pointer from the backend: where it is, and a physical button
 * that changed.  A press focuses and raises the top-level window under
 * the pointer first (the rootless server is its window manager; a rooted
 * server's clients do that themselves).  Moves between buttons are held
 * back so only the last of a burst is sent.
 */
void
x11server_backend_pointer(
	struct x11server *server,
	const struct x11_pointer_frame *frame)
{
	struct x11_window *under;
	struct x11_window *top;

	/* A move alone is held back (the newest replaces an older one). */
	if (frame->button == 0U) {
		server->motion_pending = 1;
		server->motion_time = frame->time;
		server->motion_x = frame->x;
		server->motion_y = frame->y;
		return;
	}

	/* A button's change sends the move held back first, so the order holds. */
	x11_rootless_flush_motion(server);

	/* A press raises the top-level window under the pointer and gives it the focus. */
	if (frame->pressed && server->mode != X11SERVER_ROOTED) {
		under = x11_window_at(server, frame->x, frame->y);
		top = x11_window_top_level(under);
		if (top != server->root) {
			x11_window_raise(server, top);
			x11_input_focus_top_level(server, top, frame->time);
		}
	}

	/* Succeeded: the input hears the button. */
	x11_input_button(server, frame->button, frame->pressed, frame->time, frame->x, frame->y);
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

	/* Succeeded: the keyboard's focus. */
	if (keyboard)
		x11_input_focus_top_level(server, entered, server->now);
}

/*
 * Hears that the pointer or the keyboard left a surface for the system's
 * own windows: the pointer is outside the X windows, or no X window has
 * the focus when it was in that surface's window.
 */
void
x11server_backend_leave(
	struct x11server *server,
	uint32_t window,
	int keyboard)
{
	struct x11_window *left;

	/* The X window. */
	left = x11_window_find(server, window);
	if (left == NULL || left == server->root)
		return;

	/* The keyboard: the focus leaves the X windows when it was in this one. */
	if (keyboard) {
		x11_input_focus_lost(server, left, server->now);
		return;
	}

	/* Succeeded: the pointer is outside the X windows (after the move held back). */
	x11_rootless_flush_motion(server);
	x11_input_leave(server, server->now);
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
	int held;

	/* The X window, at a size it can have. */
	changed = x11_window_find(server, window);
	if (changed == NULL ||
	    changed == server->root ||
	    width <= 0 ||
	    height <= 0 ||
	    width > ROOTLESS_SIZE_MAX ||
	    height > ROOTLESS_SIZE_MAX)
		return;

	/* Within the limits its client put on its size (a system that does not keep them gave another). */
	x11_wm_constrain(server, changed, &width, &height);

	/* A place not known, or outside X's coordinates, stays as it is. */
	if (x < -ROOTLESS_SIZE_MAX || x > ROOTLESS_SIZE_MAX || y < -ROOTLESS_SIZE_MAX || y > ROOTLESS_SIZE_MAX) {
		x = X11_SURFACE_ANY;
		y = X11_SURFACE_ANY;
	}

	/* A client that draws in step with the window manager is asked first; a size given while it draws waits. */
	held = x11_wm_sync_configure(server, changed, x, y, width, height);
	if (held)
		return;

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
 * Hears the state the system or the user gave a surface
 * (X11_SURFACE_ICONIC, MAXIMIZED, FULLSCREEN): minimized, the window
 * manager unmaps the X window and makes it Iconic; brought back, it maps
 * it again; maximized or full-screen, it says so in _NET_WM_STATE.
 */
void
x11server_backend_state(
	struct x11server *server,
	uint32_t window,
	int state)
{
	struct x11_window *changed;

	/* The X window. */
	changed = x11_window_find(server, window);
	if (changed == NULL || changed == server->root)
		return;

	/* Succeeded: the window manager follows the state. */
	x11_wm_backend_state(server, changed, (unsigned)state);
}

/*
 * Hears that the system took the pointer's capture away (another program
 * was activated): an X client's active grab of the pointer ends.
 */
void
x11server_backend_capture_lost(
	struct x11server *server,
	uint32_t time)
{
	/* Succeeded: the grab ends. */
	x11_input_capture_lost(server, time);
}

/*
 * Hears the size of the system's frame around a surface (left, right,
 * top and bottom, in X's pixels): its window's _NET_FRAME_EXTENTS says it.
 */
void
x11server_backend_frame(
	struct x11server *server,
	uint32_t window,
	unsigned left,
	unsigned right,
	unsigned top,
	unsigned bottom)
{
	struct x11_window *framed;
	uint32_t extents[4];

	/* The X window. */
	framed = x11_window_find(server, window);
	if (framed == NULL || framed == server->root)
		return;

	/* Succeeded: the window manager says it. */
	extents[0] = left;
	extents[1] = right;
	extents[2] = top;
	extents[3] = bottom;
	x11_wm_frame(server, framed, extents);
}
