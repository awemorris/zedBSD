/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window of the library (ws090-p004, Text Editor's window.c moved
 * here, itself PDF Viewer's): an xdg-shell toplevel of its own connection,
 * and its seat's pointer, keyboard and touch screen, whose input becomes
 * kl_window_event values in a queue the application takes.  The last
 * input's serial is kept for the clipboard and the primary selection
 * (clipboard.c, primary.c), whose managers are bound here, and for the
 * context menus.
 *
 * The compositor does not repeat keys, so a key held past the repeat delay is
 * pressed again on each interval -- only by kl_window_repeat, which the
 * application calls after a dispatch, so that a release read in the same
 * dispatch stops it first (BUG-111).
 */

#include "window.h"
#include "internal.h"

#include "userland/desktop/libwayland/content-type-v1-client-protocol.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The seat version the window understands (frames and discrete axes). */
#define WINDOW_SEAT_VERSION	5U

/* The repeat's delay and interval when the compositor gives none, in milliseconds. */
#define WINDOW_REPEAT_DELAY	400U
#define WINDOW_REPEAT_INTERVAL	40U

/* How many pixels one unit of scrolling moves (the compositor sends 15 units a wheel notch). */
#define WINDOW_SCROLL_SCALE	4.0

/* The oldest a compositor's input time may be and still be taken (older is another clock), in milliseconds. */
#define WINDOW_TOUCH_BEHIND	2000U

/* The modifier bits of wl_keyboard.modifiers as the compositor reports them. */
#define WINDOW_WAYLAND_SHIFT	0x01U
#define WINDOW_WAYLAND_CTRL	0x04U
#define WINDOW_WAYLAND_ALT	0x08U
#define WINDOW_WAYLAND_SUPER	0x40U

/* The evdev codes of the modifier keys, which never repeat. */
#define WINDOW_KEY_LEFTCTRL	29U
#define WINDOW_KEY_LEFTSHIFT	42U
#define WINDOW_KEY_RIGHTSHIFT	54U
#define WINDOW_KEY_LEFTALT	56U
#define WINDOW_KEY_CAPSLOCK	58U
#define WINDOW_KEY_RIGHTCTRL	97U
#define WINDOW_KEY_RIGHTALT	100U
#define WINDOW_KEY_LEFTMETA	125U
#define WINDOW_KEY_RIGHTMETA	126U

static void window_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void window_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void window_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void window_configure(void *data, struct xdg_surface *surface, uint32_t serial);
static void window_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void window_toplevel_close(void *data, struct xdg_toplevel *toplevel);
static void window_toplevel_bounds(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height);
static void window_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities);
static void window_seat_name(void *data, struct wl_seat *seat, const char *name);
static void window_desktop_configure(void *data, struct kl_desktop *desktop, uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height);
static void window_output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform);
static void window_output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh);
static void window_output_done(void *data, struct wl_output *output);
static void window_output_scale(void *data, struct wl_output *output, int32_t factor);
static void window_pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y);
static void window_pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface);
static void window_pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void window_pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
static void window_pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value);
static void window_pointer_frame(void *data, struct wl_pointer *pointer);
static void window_pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source);
static void window_pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis);
static void window_pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete);
static void window_keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size);
static void window_keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys);
static void window_keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface);
static void window_keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void window_keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
static void window_keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay);
static int window_modifier_key(uint32_t key);
static void window_touch_push(struct kl_window *window, unsigned kind, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y);
static void window_touch_down(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time, struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y);
static void window_touch_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time, int32_t id);
static void window_touch_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y);
static void window_touch_frame(void *data, struct wl_touch *touch);
static void window_touch_cancel(void *data, struct wl_touch *touch);
static int window_touch_foreign(struct kl_window *window, int32_t id, int forget);
static struct kl_window_event *window_push(struct kl_window *window, unsigned kind);
static int window_setup(struct kl_window *window, const struct kl_window_options *options);
static int window_setup_shared(struct kl_window *window, struct xdg_toplevel *parent, const struct kl_window_options *options, uint32_t min_width, uint32_t min_height);
static int window_setup_app(struct kl_window *window, struct kl_app *app, const struct kl_window_options *options);
static int window_surface(struct kl_window *window, const struct kl_window_options *options, const char *application);
static void window_search(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void window_woken(void *data, struct wl_callback *callback, uint32_t time);
static void window_inset(void *data, int32_t right, int32_t bottom, uint32_t reason);

/*
 * The globals a window on another's connection finds through a registry of
 * its own queue: their names and versions (0: not announced).
 */
struct window_found {
	uint32_t compositor;
	uint32_t compositor_version;
	uint32_t shm;
	uint32_t shell;
	uint32_t shell_version;
	uint32_t seat;
	uint32_t seat_version;
	uint32_t text_manager;
};

/* The registry's callbacks of that search. */
static const struct wl_registry_listener search_listener = {
	window_search, window_global_remove
};

/* The wake-up's sync. */
static const struct wl_callback_listener woken_listener = {
	window_woken
};

/* The registry's callbacks, for as long as the registry lives. */
static const struct wl_registry_listener registry_listener = {
	window_global, window_global_remove
};

/* The shell's liveness check. */
static const struct xdg_wm_base_listener shell_listener = {
	window_ping
};

/* The configure acknowledgement of the window's role. */
static const struct xdg_surface_listener surface_listener = {
	window_configure
};

/* The size the compositor gives the window, its request to close, and the largest size it may choose. */
static const struct xdg_toplevel_listener toplevel_listener = {
	window_toplevel_configure, window_toplevel_close, window_toplevel_bounds
};

/* The seat's devices and name. */
static const struct wl_seat_listener seat_listener = {
	window_seat_capabilities, window_seat_name
};

/* The desktop surface's configure (KL_VERSION 46). */
static const struct kl_desktop_listener desktop_listener = {
	window_desktop_configure
};

/* The first screen's description: its current mode is kept (KL_VERSION 45). */
static const struct wl_output_listener output_listener = {
	window_output_geometry, window_output_mode, window_output_done, window_output_scale, NULL, NULL
};

/* The touch screen's events of versions 1 to 5 (shape and orientation, of version 6, are never called). */
static const struct wl_touch_listener touch_listener = {
	window_touch_down, window_touch_up, window_touch_motion, window_touch_frame, window_touch_cancel, NULL, NULL
};

/* The pointer's events of versions 1 to 5 (the later members are never called). */
static const struct wl_pointer_listener pointer_listener = {
	window_pointer_enter, window_pointer_leave, window_pointer_motion, window_pointer_button,
	window_pointer_axis, window_pointer_frame, window_pointer_axis_source, window_pointer_axis_stop,
	window_pointer_axis_discrete, NULL, NULL
};

/* The keyboard's events of versions 1 to 5. */
static const struct wl_keyboard_listener keyboard_listener = {
	window_keyboard_keymap, window_keyboard_enter, window_keyboard_leave,
	window_keyboard_key, window_keyboard_modifiers, window_keyboard_repeat
};

/*
 * Connects to the compositor and makes a toplevel window, configured and
 * ready to be drawn into (the presenter made, except for
 * KL_PRESENT_NONE).
 *
 * Returns NULL with errno set: EINVAL (no options, an unknown way of
 * showing), a connection's error, EOPNOTSUPP (no compositor or shell, or
 * no shared memory for KL_PRESENT_SHM), EPROTO (no configure), ENOMEM,
 * EIO (Vulkan refused).
 */
struct kl_window *
kl_window_open(
	const struct kl_window_options *options)
{
	struct kl_window *window;
	int error;

	/* Only the three ways of showing. */
	if (options == NULL || options->present > KL_PRESENT_NONE) {
		errno = EINVAL;
		return NULL;
	}

	/* The record. */
	window = calloc(1, sizeof(*window));
	if (window == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* Its declarative parts' models. */
	keiui_declare_window_init(window);

	/* The connection, the globals, the surface and its first configure. */
	error = window_setup(window, options);
	if (error != 0) {
		kl_window_close(window);
		errno = error;
		return NULL;
	}

	/* Succeeded: the window can be drawn into. */
	return window;
}

/*
 * Destroys the window's objects and disconnects.
 */
void
kl_window_close(
	struct kl_window *window)
{
	/* No window, nothing to close. */
	if (window == NULL)
		return;

	/* Out of its application's windows and queue; its menus, controls and glass before the toplevel they name. */
	if (window->app != NULL)
		keiui_app_forget(window->app, window);
	keiui_declare_window_close(window);

	/* The keyboard's inset and the editing operations before the toplevel they name. */
	kl_keyboard_inset_destroy(window->inset);
	keiui_edit_close(window);

	/* The presenter before the surface it shows on. */
	if (window->present == KL_PRESENT_VULKAN)
		keiui_present_close(&window->vulkan);
	keiui_shm_close(window);

	/* The clipboard and the primary selection before the seat they belong to. */
	keiui_clipboard_close(window);
	keiui_primary_close(window);
	keiui_text_input_close(window);
	keiui_tablet_close(window);
	free(window->clipboard);
	free(window->primary_text);

	/* The devices and the seat. */
	if (window->touch != NULL)
		wl_touch_destroy(window->touch);
	if (window->pointer != NULL)
		wl_pointer_destroy(window->pointer);
	if (window->keyboard != NULL)
		wl_keyboard_destroy(window->keyboard);
	if (window->seat != NULL)
		wl_seat_destroy(window->seat);
	if (window->output != NULL)
		wl_output_destroy(window->output);

	/* The surface's content type before the surface, the manager with it. */
	if (window->content_type != NULL)
		wp_content_type_v1_destroy(window->content_type);
	if (window->content_manager != NULL)
		wp_content_type_manager_v1_destroy(window->content_manager);

	/* The roles before the surface, the surface before the globals that made it. */
	if (window->toplevel != NULL)
		xdg_toplevel_destroy(window->toplevel);
	if (window->desktop != NULL)
		kl_desktop_destroy(window->desktop);
	if (window->role != NULL)
		xdg_surface_destroy(window->role);
	if (window->surface != NULL)
		wl_surface_destroy(window->surface);
	if (window->shell != NULL)
		xdg_wm_base_destroy(window->shell);
	if (window->shm != NULL)
		wl_shm_destroy(window->shm);
	if (window->compositor != NULL)
		wl_compositor_destroy(window->compositor);
	if (window->registry != NULL)
		wl_registry_destroy(window->registry);

	/* The owner's wake-up. */
	if (window->notify_sync != NULL)
		wl_callback_destroy(window->notify_sync);

	/* The connection last (unless it is the application's), then the record. */
	if (window->display != NULL && !window->shared)
		wl_display_disconnect(window->display);
	if (window->display != NULL && window->shared)
		(void)wl_display_flush(window->display);
	free(window);
}

/*
 * Waits up to a timeout (milliseconds, -1 for ever) for the compositor's
 * events and runs them; they queue input for kl_window_take.
 *
 * Returns 0, or -1 when the connection is broken.
 */
int
kl_window_dispatch(
	struct kl_window *window,
	int timeout_ms)
{
	int status;

	/* The compositor alone. */
	status = kl_window_dispatch_fds(window, NULL, 0U, timeout_ms, NULL);
	if (status != 0)
		return -1;

	/* Succeeded: the events so far have run. */
	return 0;
}

/*
 * Waits up to a timeout (milliseconds, -1 for ever) for the compositor's
 * events or for other descriptors of the application (a terminal's
 * shells), and runs the compositor's events (libkeiui's version 11).
 *
 * ready[i] is set to 1 when fds[i] has bytes to read or its other end has
 * gone, 0 otherwise.  Returns 0, or -1 when the connection is broken.
 */
int
kl_window_dispatch_fds(
	struct kl_window *window,
	const int *fds,
	unsigned count,
	int timeout_ms,
	int *ready)
{
	struct pollfd descriptors[1U + KL_WINDOW_FDS_MAX];
	unsigned index;
	unsigned used;
	int status;

	/* No other descriptor is ready until the poll says so. */
	for (index = 0; index < count; index++)
		ready[index] = 0;

	/* The editing state as it is now goes out with what is flushed (edit.c). */
	keiui_edit_update(window);

	/* Runs what is queued until a read of new events can be reserved. */
	for (;;) {
		status = wl_display_dispatch_pending(window->display);
		if (status < 0)
			return -1;

		/* A reserved read means nothing is queued any more. */
		status = wl_display_prepare_read(window->display);
		if (status == 0)
			break;

		/* EAGAIN asks for another dispatch; anything else is a broken connection. */
		if (errno != EAGAIN)
			return -1;
	}

	/* Sends what the window asked for. */
	status = wl_display_flush(window->display);
	if (status < 0 && errno != EAGAIN) {
		wl_display_cancel_read(window->display);
		return -1;
	}

	/* Nothing is waited for while input is already queued. */
	if (window->event_count != 0U)
		timeout_ms = 0;

	/* The compositor's descriptor, then each other one (at most KL_WINDOW_FDS_MAX). */
	descriptors[0].fd = wl_display_get_fd(window->display);
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	used = 1U;
	for (index = 0; index < count && index < KL_WINDOW_FDS_MAX; index++) {
		descriptors[used].fd = fds[index];
		descriptors[used].events = POLLIN;
		descriptors[used].revents = 0;
		used++;
	}

	/* Waits for any of them. */
	status = poll(descriptors, (nfds_t)used, timeout_ms);

	/* Reads the compositor's events, or gives the reservation back. */
	if (status > 0 && (descriptors[0].revents & POLLIN) != 0) {
		status = wl_display_read_events(window->display);
		if (status < 0)
			return -1;
	} else {
		wl_display_cancel_read(window->display);
		if (status < 0 && errno != EINTR)
			return -1;

		/* A hung-up connection has no more events. */
		if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			return -1;
	}

	/* Each other descriptor that has bytes, or whose other end has gone. */
	for (index = 1U; index < used; index++) {
		/* A descriptor that is not ready stays not ready. */
		if ((descriptors[index].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
			ready[index - 1U] = 1;
	}

	/* Runs the events read. */
	status = wl_display_dispatch_pending(window->display);
	if (status < 0)
		return -1;

	/* Succeeded: the events so far have run. */
	return 0;
}

/*
 * Takes the oldest queued input.  Returns 1 with it in *event, 0 when
 * there is none.
 */
int
kl_window_take(
	struct kl_window *window,
	struct kl_window_event *event)
{
	/* An empty queue. */
	if (window->event_count == 0U)
		return 0;

	/* The oldest input, and the queue moves on. */
	*event = window->events[window->event_first];
	window->event_first = (window->event_first + 1U) % KEIUI_WINDOW_EVENTS;
	window->event_count--;

	/* Succeeded: one input taken. */
	return 1;
}

/*
 * Queues an input of the application's own (a code of its choosing, as a
 * KL_WINDOW_POST event) after the inputs queued so far: a choice heard
 * through another object during the dispatch keeps its place among the
 * window's keys.
 */
void
kl_window_post(
	struct kl_window *window,
	uint32_t code)
{
	struct kl_window_event *event;

	/* The input; a full queue drops it. */
	event = window_push(window, KL_WINDOW_POST);
	if (event == NULL)
		return;
	event->code = code;
}

/*
 * Presses the held key again when its repeat is due (the application
 * calls it after a dispatch), and reports in how many milliseconds the
 * next repeat is due (-1 when no key is held).
 */
int
kl_window_repeat(
	struct kl_window *window,
	uint64_t now_us)
{
	struct kl_window_event *event;
	uint64_t now;

	/* No key held. */
	if (window->repeat_key == 0U)
		return -1;

	/* Not yet time: the wait until it is. */
	now = now_us / 1000U;
	if (now < window->repeat_at)
		return (int)(window->repeat_at - now);

	/* The key once more. */
	event = window_push(window, KL_WINDOW_KEY);
	if (event != NULL) {
		event->code = window->repeat_key;
		event->pressed = 1;
		event->repeated = 1;
		keiui_edit_key(window, event);
	}

	/*
	 * The next repeat is one interval after this one was due, so a loop that
	 * woke a little late keeps the pace (BUG-172); one that fell a whole
	 * interval behind starts again from now instead of catching up in a burst.
	 */
	window->repeat_at += window->repeat_interval;
	if (window->repeat_at <= now)
		window->repeat_at = now + window->repeat_interval;

	/* Reports the wait until the next repeat. */
	return (int)(window->repeat_at - now);
}

/*
 * Reports in how many milliseconds the held key's repeat is due (0: now,
 * -1 when no key is held), without pressing it: the loop waits that long
 * and reads the compositor's events (a release among them) before the
 * repeat is pressed (BUG-111).
 */
int
kl_window_repeat_wait(
	const struct kl_window *window,
	uint64_t now_us)
{
	uint64_t now;

	/* No key held. */
	if (window->repeat_key == 0U)
		return -1;

	/* Due already. */
	now = now_us / 1000U;
	if (now >= window->repeat_at)
		return 0;

	/* Reports the wait until it is due. */
	return (int)(window->repeat_at - now);
}

/*
 * Sets the title the compositor shows.
 */
void
kl_window_set_title(
	struct kl_window *window,
	const char *title)
{
	/* The request, sent with the next flush (a desktop surface has no title). */
	if (window->toplevel != NULL)
		xdg_toplevel_set_title(window->toplevel, title);
}

/*
 * Reports the size the compositor gives the window (the one a frame is
 * drawn at after kl_window_present_resize).
 */
void
kl_window_size(
	const struct kl_window *window,
	uint32_t *width,
	uint32_t *height)
{
	/* The configured size. */
	*width = window->width;
	*height = window->height;
}

/*
 * Makes the presenter fit the window's size and reports the size a frame
 * is drawn at.  Returns 0, or EIO when Vulkan refused.
 */
int
kl_window_present_resize(
	struct kl_window *window,
	uint32_t *width,
	uint32_t *height)
{
	VkResult result;

	/* Vulkan: a swapchain of the size, whose extent may differ. */
	if (window->present == KL_PRESENT_VULKAN) {
		result = keiui_present_resize(&window->vulkan, window->width, window->height);
		if (result != VK_SUCCESS)
			return EIO;
		*width = window->vulkan.extent.width;
		*height = window->vulkan.extent.height;
		return 0;
	}

	/* Shared memory, or the application's own drawing: the window's size. */
	*width = window->width;
	*height = window->height;
	window->shm_width = window->width;
	window->shm_height = window->height;

	/* Succeeded: frames are drawn at that size. */
	return 0;
}

/*
 * Shows a frame (premultiplied 0xAARRGGBB words, stride words a row) of
 * the size kl_window_present_resize reported.  Returns 0, EAGAIN when it
 * could not be shown now (the swapchain is out of date: resize and draw
 * again; no shared-memory buffer is free: draw again later), EINVAL for
 * KL_PRESENT_NONE, or EIO.
 */
int
kl_window_present(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride)
{
	int error;

	/* The whole frame changed. */
	error = kl_window_present_part(window, pixels, stride, NULL);
	return error;
}

/*
 * Shows a frame of which only a part changed since the last one
 * (KL_VERSION 63, BUG-221): as kl_window_present, but only the part's
 * rows are copied and the compositor is told that part alone, so it
 * draws that much again.  The pixels outside the part must be the last
 * frame's.  A NULL part, a new swapchain or shared memory take the whole
 * frame.  Returns as kl_window_present.
 */
int
kl_window_present_part(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride,
	const struct kl_rect *part)
{
	VkResult result;
	int error;

	/* Shared memory. */
	if (window->present == KL_PRESENT_SHM) {
		error = keiui_shm_present(window, pixels, stride);
		if (error != 0)
			return error;
		return 0;
	}

	/* The application draws its own frames. */
	if (window->present != KL_PRESENT_VULKAN)
		return EINVAL;

	/* Vulkan: out of date asks for a resize and another frame. */
	result = keiui_present_frame(&window->vulkan, pixels, stride, part);
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		return EAGAIN;
	if (result != VK_SUCCESS)
		return EIO;

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Tells whether the frames are blended by their alpha (a see-through
 * swapchain, or shared memory), which the compositor's glass needs.
 */
int
kl_window_see_through(
	const struct kl_window *window)
{
	/* Shared memory is ARGB, blended by its alpha. */
	if (window->present == KL_PRESENT_SHM)
		return 1;

	/* Vulkan: when the swapchain is premultiplied. */
	if (window->present == KL_PRESENT_VULKAN && window->vulkan.premultiplied)
		return 1;

	/* Opaque. */
	return 0;
}

/*
 * Reports the window's connection (for libkeiland's menus, titlebar and glass).
 */
struct wl_display *
kl_window_display(
	const struct kl_window *window)
{
	/* The connection. */
	return window->display;
}

/*
 * Reports the window's surface.
 */
struct wl_surface *
kl_window_surface(
	const struct kl_window *window)
{
	/* The surface. */
	return window->surface;
}

/*
 * Reports the window's toplevel.
 */
struct xdg_toplevel *
kl_window_toplevel(
	const struct kl_window *window)
{
	/* The toplevel. */
	return window->toplevel;
}

/*
 * Asks the compositor to make the window fullscreen (on the output it is
 * on), or to take it out of the full screen (libkeiui's version 10).  The answer
 * is a configure: kl_window_fullscreen tells once it came.
 */
void
kl_window_set_fullscreen(
	struct kl_window *window,
	int fullscreen)
{
	/* The request, sent with the next flush (a desktop surface fills its screen already). */
	if (window->toplevel == NULL)
		return;
	if (fullscreen) {
		xdg_toplevel_set_fullscreen(window->toplevel, NULL);
	} else {
		xdg_toplevel_unset_fullscreen(window->toplevel);
	}
}

/*
 * Tells the compositor what the window shows (KL_CONTENT_*, KL_VERSION
 * 41), from its next frame.  Returns 0, EINVAL for another type, or
 * ENOTSUP when the compositor does not take content types.
 */
int
kl_window_set_content_type(
	struct kl_window *window,
	unsigned type)
{
	/* Refuses a type the protocol does not have. */
	if (window == NULL || type > KL_CONTENT_GAME)
		return EINVAL;

	/* Without the compositor's manager nothing is told. */
	if (window->content_manager == NULL)
		return ENOTSUP;

	/* The surface's object, made the first time. */
	if (window->content_type == NULL)
		window->content_type = wp_content_type_manager_v1_get_surface_content_type(window->content_manager, window->surface);
	if (window->content_type == NULL)
		return ENOMEM;

	/* The type, applied with the surface's next commit. */
	wp_content_type_v1_set_content_type(window->content_type, (uint32_t)type);

	/* Succeeded: told. */
	return 0;
}

/*
 * Turns a held key's repeat on or off for the window (KL_VERSION 44: Notes
 * takes each key once).  Returns 0, or EINVAL without a window.
 */
int
kl_window_set_repeat(
	struct kl_window *window,
	int enabled)
{
	/* A window. */
	if (window == NULL)
		return EINVAL;

	/* Off: the key held now stops too. */
	window->repeat_off = 0;
	if (!enabled) {
		window->repeat_off = 1;
		window->repeat_key = 0U;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reports whether the compositor's last configure maximized the window
 * (KL_VERSION 45).
 */
int
kl_window_maximized(
	const struct kl_window *window)
{
	/* The state as last configured. */
	return window->maximized;
}

/*
 * Asks the compositor to maximize the window, or to bring it back to its
 * size (KL_VERSION 45); the configure that follows says what it did.
 */
void
kl_window_set_maximized(
	struct kl_window *window,
	int maximized)
{
	/* Maximized, or back (a desktop surface is neither). */
	if (window->toplevel == NULL)
		return;
	if (maximized) {
		xdg_toplevel_set_maximized(window->toplevel);
	} else {
		xdg_toplevel_unset_maximized(window->toplevel);
	}
}

/*
 * Asks the compositor to minimize the window (KL_VERSION 45).
 */
void
kl_window_minimize(
	struct kl_window *window)
{
	/* The request, sent with the next flush (a desktop surface is never minimized). */
	if (window->toplevel != NULL)
		xdg_toplevel_set_minimized(window->toplevel);
}

/*
 * Reports where the desktop's surface is on the screen, as the compositor
 * last configured it (KL_VERSION 46).  Returns 0, or EINVAL for a window.
 */
int
kl_window_desktop_place(
	const struct kl_window *window,
	int32_t *x,
	int32_t *y)
{
	/* Only a desktop surface has a place. */
	if (window->desktop == NULL)
		return EINVAL;

	/* Succeeded: the place. */
	*x = window->desktop_x;
	*y = window->desktop_y;
	return 0;
}

/*
 * Reports the first screen's current mode: its size and its refresh in
 * millihertz (KL_VERSION 45).  Returns 0, or ENOENT while it is not known.
 */
int
kl_window_output_mode(
	const struct kl_window *window,
	int32_t *width,
	int32_t *height,
	int32_t *refresh)
{
	/* Not told yet. */
	if (window->output_width <= 0 || window->output_height <= 0)
		return ENOENT;

	/* Succeeded: the mode. */
	*width = window->output_width;
	*height = window->output_height;
	*refresh = window->output_refresh;
	return 0;
}

/*
 * Reports the name of the Vulkan device that shows a KL_PRESENT_VULKAN
 * window's frames (KL_VERSION 45; empty for another presenter).
 */
const char *
kl_window_device_name(
	const struct kl_window *window)
{
	/* The presenter's device. */
	return window->vulkan.device_name;
}

/*
 * Reports how long the last frame's copy, acquire, queueing and wait took,
 * in milliseconds (KL_VERSION 45, a KL_PRESENT_VULKAN window's).
 */
void
kl_window_present_times(
	const struct kl_window *window,
	struct kl_present_times *times)
{
	/* The presenter's times. */
	times->copy_ms = window->vulkan.copy_ms;
	times->acquire_ms = window->vulkan.acquire_ms;
	times->present_ms = window->vulkan.present_ms;
	times->wait_ms = window->vulkan.wait_ms;
}

/*
 * Reports whether the compositor's last configure made the window
 * fullscreen (libkeiui's version 10).
 */
int
kl_window_fullscreen(
	const struct kl_window *window)
{
	/* The state as last configured. */
	return window->fullscreen;
}

/*
 * Reports the serial of the window's last input.
 */
uint32_t
kl_window_serial(
	const struct kl_window *window)
{
	/* The serial. */
	return window->serial;
}

/*
 * Reports the serial of the last press of a button or a finger on the
 * window (a context menu opens for a press).
 */
uint32_t
kl_window_press_serial(
	const struct kl_window *window)
{
	/* The press's serial. */
	return window->press_serial;
}

/*
 * Takes the serial of an input the application heard through another
 * object (a System Menu's or a titlebar's choice), so that a selection set
 * in answer carries it.
 */
void
kl_window_set_serial(
	struct kl_window *window,
	uint32_t serial)
{
	/* The latest input's serial. */
	window->serial = serial;
}

/*
 * Reports the window's seat (for a context menu's popup).
 */
struct wl_seat *
kl_window_seat(
	const struct kl_window *window)
{
	/* The seat. */
	return window->seat;
}

/*
 * Reports the monotonic clock in microseconds (the clock of the library's
 * times).
 */
uint64_t
kl_clock_us(void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Reports it in microseconds. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/*
 * Reports the monotonic clock in milliseconds.
 */
/*
 * Makes a toplevel window on the application's connection (a file
 * chooser's): its own globals and seat, found through a queue of its own
 * and heard on the application's default queue, a parent and a smallest
 * size.  Its first configure comes with the application's next dispatch;
 * frames are shown through shared memory.
 *
 * Returns NULL with errno set: EINVAL, EOPNOTSUPP (no compositor, shell
 * or shared memory), ENOMEM.
 */
struct kl_window *
keiui_window_open_shared(
	struct wl_display *display,
	struct xdg_toplevel *parent,
	const struct kl_window_options *options,
	uint32_t min_width,
	uint32_t min_height)
{
	struct kl_window *window;
	int error;

	/* A connection, and frames through shared memory. */
	if (display == NULL || options == NULL || options->present != KL_PRESENT_SHM) {
		errno = EINVAL;
		return NULL;
	}

	/* The record, on the application's connection. */
	window = calloc(1, sizeof(*window));
	if (window == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* On the application's connection, with its declarative parts' models. */
	window->display = display;
	window->shared = 1;
	keiui_declare_window_init(window);

	/* The globals, the surface and its toplevel. */
	error = window_setup_shared(window, parent, options, min_width, min_height);
	if (error != 0) {
		kl_window_close(window);
		errno = error;
		return NULL;
	}

	/* Succeeded: the window waits for its first configure. */
	return window;
}

/*
 * Names who hears of the window's queued input (and of a buffer given back)
 * on another's connection, once the events that queued it are run.
 */
void
keiui_window_set_notify(
	struct kl_window *window,
	void (*notify)(void *data),
	void *data)
{
	/* The owner. */
	window->notify = notify;
	window->notify_data = data;
}

/*
 * Asks for the owner's wake-up after the events being run (one sync at a
 * time; nothing without an owner).
 */
void
keiui_window_wake(
	struct kl_window *window)
{
	int status;

	/* No owner, or a wake-up already asked for. */
	if (window->notify == NULL || window->notify_sync != NULL)
		return;

	/* A sync, whose answer comes after the events read with it. */
	window->notify_sync = wl_display_sync(window->display);
	if (window->notify_sync == NULL)
		return;
	status = wl_callback_add_listener(window->notify_sync, &woken_listener, window);
	if (status != 0) {
		wl_callback_destroy(window->notify_sync);
		window->notify_sync = NULL;
	}
}

/*
 * Makes a window of an application (kl_app_window_create): it binds what
 * it needs from the application's registry, without a search of its own,
 * and is configured when this returns.  Returns NULL with errno set as
 * kl_window_open does.
 */
struct kl_window *
keiui_window_open_app(
	struct kl_app *app,
	const struct kl_window_options *options)
{
	struct kl_window *window;
	int error;

	/* Only the three ways of showing. */
	if (options == NULL || options->present > KL_PRESENT_NONE) {
		errno = EINVAL;
		return NULL;
	}

	/* The record. */
	window = calloc(1, sizeof(*window));
	if (window == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* On the application's connection, with its declarative parts' models. */
	window->display = app->display;
	window->shared = 1;
	keiui_declare_window_init(window);

	/* One of the application's windows from now on (its close takes it out again). */
	window->app = app;
	window->app_next = app->windows;
	app->windows = window;

	/* The globals, the surface and its first configure. */
	error = window_setup_app(window, app, options);
	if (error != 0) {
		kl_window_close(window);
		errno = error;
		return NULL;
	}

	/* Succeeded: the window can be drawn into. */
	return window;
}

uint64_t
keiui_clock_ms(void)
{
	uint64_t now;

	/* The microseconds' clock. */
	now = kl_clock_us();

	/* Reports it in milliseconds. */
	return now / 1000U;
}

/* Connects, binds the globals, makes the surface and its toplevel, waits for the first configure and makes the presenter; 0 or an errno value. */
static int
window_setup(
	struct kl_window *window,
	const struct kl_window_options *options)
{
	VkResult result;
	int status;

	/* The size the window asks for until the compositor gives one, and the key repeat's defaults. */
	window->width = options->width;
	window->height = options->height;
	window->preferred_width = options->width;
	window->preferred_height = options->height;
	window->repeat_delay = WINDOW_REPEAT_DELAY;
	window->repeat_interval = WINDOW_REPEAT_INTERVAL;
	window->present = options->present;

	/* The connection. */
	window->display = wl_display_connect(options->display);
	if (window->display == NULL)
		return errno;

	/* The globals: the compositor, shared memory, the shell, the seat and the selections' managers. */
	window->registry = wl_display_get_registry(window->display);
	if (window->registry == NULL)
		return ENOMEM;
	status = wl_registry_add_listener(window->registry, &registry_listener, window);
	if (status != 0)
		return EINVAL;
	status = wl_display_roundtrip(window->display);
	if (status < 0)
		return EPROTO;

	/* The clipboard and the primary selection, through the seat (without them the window keeps its own copies). */
	keiui_clipboard_start(window);
	keiui_primary_start(window);
	keiui_text_input_start(window);

	/* A window needs a compositor and a shell, and shared memory to show frames through it. */
	if (window->compositor == NULL || window->shell == NULL)
		return EOPNOTSUPP;
	if (window->present == KL_PRESENT_SHM && window->shm == NULL)
		return EOPNOTSUPP;

	/* The surface, its toplevel, title and identity. */
	status = window_surface(window, options, options->application);
	if (status != 0)
		return status;

	/* The on-screen keyboard's inset, where the compositor tells it (libkeiui's version 7; NULL otherwise, and nothing is told), and the editing operations of the keyboard's buttons (libkeiui's version 8): a toplevel's. */
	if (window->toplevel != NULL) {
		window->inset = kl_keyboard_inset_create(window->display, window->toplevel, window_inset, window);
		keiui_edit_start(window);
	}
	wl_surface_commit(window->surface);

	/* The first configure (and the seat's devices) before anything is drawn. */
	status = wl_display_roundtrip(window->display);
	if (status < 0)
		return EPROTO;
	if (window->configured == 0)
		return EPROTO;

	/* The size given is not news to the application. */
	window->event_count = 0;

	/* The Vulkan presenter over the surface. */
	if (window->present == KL_PRESENT_VULKAN) {
		result = keiui_present_open(&window->vulkan, window);
		if (result != VK_SUCCESS)
			return EIO;
	}

	/* Succeeded: the window is configured. */
	window->shm_width = window->width;
	window->shm_height = window->height;
	return 0;
}

/* Finds the globals through a queue of the window's own, binds them onto the application's queue, and makes the toplevel; 0 or an errno value. */
static int
window_setup_shared(
	struct kl_window *window,
	struct xdg_toplevel *parent,
	const struct kl_window_options *options,
	uint32_t min_width,
	uint32_t min_height)
{
	struct window_found found;
	struct wl_event_queue *queue;
	struct wl_display *wrapper;
	struct wl_registry *registry;
	int status;

	/* The size it asks for, and the key repeat's defaults. */
	window->width = options->width;
	window->height = options->height;
	window->preferred_width = options->width;
	window->preferred_height = options->height;
	window->repeat_delay = WINDOW_REPEAT_DELAY;
	window->repeat_interval = WINDOW_REPEAT_INTERVAL;
	window->present = options->present;

	/* A queue of its own, and the display as seen from it, so that the application's events do not run meanwhile. */
	queue = wl_display_create_queue(window->display);
	if (queue == NULL)
		return ENOMEM;
	wrapper = wl_proxy_create_wrapper(window->display);
	if (wrapper == NULL) {
		wl_event_queue_destroy(queue);
		return ENOMEM;
	}

	/* What the wrapper makes lives on that queue. */
	wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);

	/* The globals announced to this search alone. */
	memset(&found, 0, sizeof(found));
	registry = wl_display_get_registry(wrapper);
	if (registry == NULL) {
		wl_proxy_wrapper_destroy(wrapper);
		wl_event_queue_destroy(queue);
		return ENOMEM;
	}

	/* Each global announced is noted. */
	status = wl_registry_add_listener(registry, &search_listener, &found);
	if (status == 0)
		(void)wl_display_roundtrip_queue(window->display, queue);

	/* The compositor, shared memory and a shell of the window's own (the compositor lets it go alone, BUG-112). */
	if (found.compositor != 0U && found.shm != 0U && found.shell != 0U) {
		window->compositor = wl_registry_bind(registry, found.compositor, &wl_compositor_interface, found.compositor_version);
		window->shm = wl_registry_bind(registry, found.shm, &wl_shm_interface, 1U);
		window->shell = wl_registry_bind(registry, found.shell, &xdg_wm_base_interface, found.shell_version);
	}

	/* The first seat, for the window's own pointer, keyboard and touch. */
	if (found.seat != 0U)
		window->seat = wl_registry_bind(registry, found.seat, &wl_seat_interface, found.seat_version);

	/* The text input's manager, for the window's own text input. */
	if (found.text_manager != 0U)
		keiui_text_input_bind(window, registry, found.text_manager);

	/* The search's objects go. */
	wl_registry_destroy(registry);
	wl_proxy_wrapper_destroy(wrapper);

	/* The bound globals hear their events on the application's queue. */
	if (window->compositor != NULL)
		wl_proxy_set_queue((struct wl_proxy *)window->compositor, NULL);
	if (window->shm != NULL)
		wl_proxy_set_queue((struct wl_proxy *)window->shm, NULL);
	if (window->shell != NULL) {
		wl_proxy_set_queue((struct wl_proxy *)window->shell, NULL);
		(void)xdg_wm_base_add_listener(window->shell, &shell_listener, window);
	}

	/* And so does the seat. */
	if (window->seat != NULL) {
		wl_proxy_set_queue((struct wl_proxy *)window->seat, NULL);
		(void)wl_seat_add_listener(window->seat, &seat_listener, window);
	}

	/* And the text input, made from its manager on the application's queue. */
	if (window->text_manager != NULL)
		wl_proxy_set_queue((struct wl_proxy *)window->text_manager, NULL);
	keiui_text_input_start(window);

	/* The search's queue is empty and goes. */
	wl_event_queue_destroy(queue);

	/* A window needs them all. */
	if (window->compositor == NULL || window->shm == NULL || window->shell == NULL)
		return EOPNOTSUPP;

	/* The surface, as an xdg surface. */
	window->surface = wl_compositor_create_surface(window->compositor);
	if (window->surface == NULL)
		return ENOMEM;
	window->role = xdg_wm_base_get_xdg_surface(window->shell, window->surface);
	if (window->role == NULL)
		return ENOMEM;
	status = xdg_surface_add_listener(window->role, &surface_listener, window);
	if (status != 0)
		return EINVAL;

	/* A toplevel window with its size and close request heard. */
	window->toplevel = xdg_surface_get_toplevel(window->role);
	if (window->toplevel == NULL)
		return ENOMEM;
	status = xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);
	if (status != 0)
		return EINVAL;

	/* Its title, its application, its parent and its smallest size. */
	if (options->title != NULL)
		xdg_toplevel_set_title(window->toplevel, options->title);
	if (options->application != NULL)
		xdg_toplevel_set_app_id(window->toplevel, options->application);
	if (parent != NULL)
		xdg_toplevel_set_parent(window->toplevel, parent);
	xdg_toplevel_set_min_size(window->toplevel, (int32_t)min_width, (int32_t)min_height);

	/* The first commit, which asks for the first configure. */
	wl_surface_commit(window->surface);
	window->shm_width = window->width;
	window->shm_height = window->height;

	/* Succeeded: the window waits for its configure. */
	return 0;
}

/* Binds the globals from an application's registry, makes the surface and its toplevel, waits for the first configure and makes the presenter; 0 or an errno value. */
static int
window_setup_app(
	struct kl_window *window,
	struct kl_app *app,
	const struct kl_window_options *options)
{
	const char *application;
	unsigned index;
	VkResult result;
	int status;

	/* The size the window asks for until the compositor gives one, and the key repeat's defaults. */
	window->width = options->width;
	window->height = options->height;
	window->preferred_width = options->width;
	window->preferred_height = options->height;
	window->repeat_delay = WINDOW_REPEAT_DELAY;
	window->repeat_interval = WINDOW_REPEAT_INTERVAL;
	window->present = options->present;

	/* The globals the application found, bound from its registry as a window of its own connection binds them. */
	for (index = 0; index < app->global_count; index++)
		window_global(window, app->registry, app->globals[index].name, app->globals[index].interface, app->globals[index].version);

	/* The clipboard and the primary selection, through the seat (without them the window keeps its own copies). */
	keiui_clipboard_start(window);
	keiui_primary_start(window);
	keiui_text_input_start(window);

	/* A window needs a compositor and a shell, and shared memory to show frames through it. */
	if (window->compositor == NULL || window->shell == NULL)
		return EOPNOTSUPP;
	if (window->present == KL_PRESENT_SHM && window->shm == NULL)
		return EOPNOTSUPP;

	/* The surface, its toplevel, title and identity (the application's when the options name none). */
	application = options->application;
	if (application == NULL)
		application = app->application;
	status = window_surface(window, options, application);
	if (status != 0)
		return status;

	/* The keyboard's inset and the editing operations, where the compositor has them (bound from the application's registry): a toplevel's. */
	if (window->toplevel != NULL) {
		window->inset = kl_keyboard_inset_create(window->display, window->toplevel, window_inset, window);
		keiui_edit_start(window);
	}
	wl_surface_commit(window->surface);

	/* The first configure (and the seat's devices) before anything is drawn; what it queues is not news. */
	window->app_quiet = 1;
	status = wl_display_roundtrip(window->display);
	window->app_quiet = 0;
	if (status < 0)
		return EPROTO;
	if (window->configured == 0)
		return EPROTO;

	/* The Vulkan presenter over the surface. */
	if (window->present == KL_PRESENT_VULKAN) {
		result = keiui_present_open(&window->vulkan, window);
		if (result != VK_SUCCESS)
			return EIO;
	}

	/* Succeeded: the window is configured. */
	window->shm_width = window->width;
	window->shm_height = window->height;
	return 0;
}

/* Makes the window's surface and toplevel with its title, identity and full screen; 0 or an errno value. */
static int
window_surface(
	struct kl_window *window,
	const struct kl_window_options *options,
	const char *application)
{
	int status;

	/* The surface. */
	window->surface = wl_compositor_create_surface(window->compositor);
	if (window->surface == NULL)
		return ENOMEM;

	/* The desktop's surface instead of a window, with its token (KL_VERSION 46). */
	if (options->role == KL_WINDOW_ROLE_DESKTOP) {
		window->desktop = kl_desktop_create(window->display, window->surface, options->token, &desktop_listener, window);
		if (window->desktop == NULL)
			return errno;
		return 0;
	}

	/* Otherwise an xdg surface. */
	window->role = xdg_wm_base_get_xdg_surface(window->shell, window->surface);
	if (window->role == NULL)
		return ENOMEM;
	status = xdg_surface_add_listener(window->role, &surface_listener, window);
	if (status != 0)
		return EINVAL;

	/* A toplevel window with its size and close request heard. */
	window->toplevel = xdg_surface_get_toplevel(window->role);
	if (window->toplevel == NULL)
		return ENOMEM;
	status = xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);
	if (status != 0)
		return EINVAL;

	/* The title the compositor shows and the application's identity. */
	if (options->title != NULL)
		xdg_toplevel_set_title(window->toplevel, options->title);
	if (application != NULL)
		xdg_toplevel_set_app_id(window->toplevel, application);

	/* The full screen from the first configure, when asked (libkeiui's version 11). */
	if (options->fullscreen)
		xdg_toplevel_set_fullscreen(window->toplevel, NULL);

	/* Succeeded. */
	return 0;
}

/* Notes a global a window on another's connection needs. */
static void
window_search(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct window_found *found;
	int match;

	/* The compositor, version 4 at most. */
	(void)registry;
	found = data;
	match = strcmp(interface, "wl_compositor");
	if (match == 0 && found->compositor == 0U) {
		found->compositor = name;
		found->compositor_version = version;
		if (version > 4U)
			found->compositor_version = 4U;
		return;
	}

	/* Shared memory. */
	match = strcmp(interface, "wl_shm");
	if (match == 0 && found->shm == 0U) {
		found->shm = name;
		return;
	}

	/* The shell, version 4 at most. */
	match = strcmp(interface, "xdg_wm_base");
	if (match == 0 && found->shell == 0U) {
		found->shell = name;
		found->shell_version = version;
		if (version > 4U)
			found->shell_version = 4U;
		return;
	}

	/* The text input's manager (a file chooser's fields take an input method's text, ws090-p022). */
	match = strcmp(interface, "zwp_text_input_manager_v3");
	if (match == 0 && found->text_manager == 0U) {
		found->text_manager = name;
		return;
	}

	/* The first seat. */
	match = strcmp(interface, "wl_seat");
	if (match == 0 && found->seat == 0U) {
		found->seat = name;
		found->seat_version = version;
		if (version > WINDOW_SEAT_VERSION)
			found->seat_version = WINDOW_SEAT_VERSION;
	}
}

/* The wake-up's sync is answered: the owner runs the queued input (it may close the window). */
static void
window_woken(
	void *data,
	struct wl_callback *callback,
	uint32_t time)
{
	struct kl_window *window;

	/* The sync is used up, and another may be asked for. */
	(void)time;
	window = data;
	wl_callback_destroy(callback);
	window->notify_sync = NULL;

	/* The owner, last: nothing of the window is touched after it. */
	if (window->notify != NULL)
		window->notify(window->notify_data);
}

/*
 * Queues a new input of a kind for the library's other parts (the text
 * input); NULL when the queue is full.
 */
struct kl_window_event *
keiui_window_push(
	struct kl_window *window,
	unsigned kind)
{
	struct kl_window_event *event;

	/* As the window's own inputs. */
	event = window_push(window, kind);
	return event;
}

/*
 * Queues a new input of a kind at the pointer's place with the modifiers
 * held; NULL when the queue is full (the input is dropped).
 */
static struct kl_window_event *
window_push(
	struct kl_window *window,
	unsigned kind)
{
	struct kl_window_event *event;
	unsigned slot;

	/* A window of an application queues on the application's queue (nothing while it is made). */
	if (window->app != NULL) {
		if (window->app_quiet)
			return NULL;
		event = keiui_app_push(window->app, window);
		if (event == NULL)
			return NULL;
	} else {
		/* A full queue drops the input (the user is far ahead of the program). */
		if (window->event_count == KEIUI_WINDOW_EVENTS)
			return NULL;

		/* The slot after the last one queued. */
		slot = (window->event_first + window->event_count) % KEIUI_WINDOW_EVENTS;
		window->event_count++;
		event = &window->events[slot];
	}

	/* The input, with what every input carries. */
	memset(event, 0, sizeof(*event));
	event->kind = kind;
	event->x = window->pointer_x;
	event->y = window->pointer_y;
	event->modifiers = window->modifiers;
	event->serial = window->serial;
	event->arrival_us = kl_clock_us();
	event->time_us = event->arrival_us;

	/* The owner of a window on another's connection hears of it after the events being run. */
	keiui_window_wake(window);

	/* Reports the queued input for its details. */
	return event;
}

/* Binds the compositor, shared memory, the shell, the clipboard's and the primary selection's managers, and the first seat. */
static void
window_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct kl_window *window;
	int match;

	/* The compositor makes surfaces; version 4 is enough. */
	window = data;
	match = strcmp(interface, "wl_compositor");
	if (match == 0 && window->compositor == NULL) {
		if (version > 4U)
			version = 4U;
		window->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version);
		return;
	}

	/* The content type manager (ws122-p005b), bound for a window that tells what it shows. */
	match = strcmp(interface, "wp_content_type_manager_v1");
	if (match == 0 && window->content_manager == NULL) {
		window->content_manager = wl_registry_bind(registry, name, &wp_content_type_manager_v1_interface, 1U);
		return;
	}

	/* The shell gives the surface its window role; version 4 tells the largest size the window may choose. */
	match = strcmp(interface, "xdg_wm_base");
	if (match == 0 && window->shell == NULL) {
		if (version > 4U)
			version = 4U;
		window->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, version);
		if (window->shell != NULL)
			(void)xdg_wm_base_add_listener(window->shell, &shell_listener, window);
		return;
	}

	/* Shared memory, for frames shown through it. */
	match = strcmp(interface, "wl_shm");
	if (match == 0 && window->shm == NULL) {
		window->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1U);
		return;
	}

	/* The clipboard's manager. */
	match = strcmp(interface, "wl_data_device_manager");
	if (match == 0 && window->data_manager == NULL) {
		keiui_clipboard_bind(window, registry, name, version);
		return;
	}

	/* The tablet manager (WS131 p018), bound for a window that takes a pen tablet's tools (tablet.c). */
	match = strcmp(interface, "zwp_tablet_manager_v2");
	if (match == 0 && window->tablet_manager == NULL) {
		keiui_tablet_bind(window, registry, name);
		return;
	}

	/* The text input's manager (an input method's and the on-screen keyboard's text). */
	match = strcmp(interface, "zwp_text_input_manager_v3");
	if (match == 0 && window->text_manager == NULL) {
		keiui_text_input_bind(window, registry, name);
		return;
	}

	/* The primary selection's manager. */
	match = strcmp(interface, "zwp_primary_selection_device_manager_v1");
	if (match == 0 && window->primary_manager == NULL) {
		keiui_primary_bind(window, registry, name);
		return;
	}

	/* The first screen, whose mode an application may show (KL_VERSION 45). */
	match = strcmp(interface, "wl_output");
	if (match == 0 && window->output == NULL) {
		window->output = wl_registry_bind(registry, name, &wl_output_interface, 2U);
		if (window->output != NULL)
			(void)wl_output_add_listener(window->output, &output_listener, window);
		return;
	}

	/* The seat gives the pointer and the keyboard. */
	match = strcmp(interface, "wl_seat");
	if (match == 0 && window->seat == NULL) {
		if (version > WINDOW_SEAT_VERSION)
			version = WINDOW_SEAT_VERSION;
		window->seat = wl_registry_bind(registry, name, &wl_seat_interface, version);
		if (window->seat != NULL)
			(void)wl_seat_add_listener(window->seat, &seat_listener, window);
	}
}

/* A global going away does not matter to a window that already bound what it needs. */
static void
window_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Nothing to do. */
	(void)data;
	(void)registry;
	(void)name;
}

/* Answers the compositor's liveness check. */
static void
window_ping(
	void *data,
	struct xdg_wm_base *shell,
	uint32_t serial)
{
	/* The same serial back. */
	(void)data;
	xdg_wm_base_pong(shell, serial);
}

/* Acknowledges a configure; the next frame is drawn at the size it gave. */
static void
window_configure(
	void *data,
	struct xdg_surface *surface,
	uint32_t serial)
{
	struct kl_window *window;

	/* The acknowledgement comes before any image of the new state. */
	window = data;
	xdg_surface_ack_configure(surface, serial);
	window->configured = 1;
	keiui_window_wake(window);
}

/* Takes the size the compositor gives; a zero size keeps the window's own, within the bounds. */
static void
window_toplevel_configure(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height,
	struct wl_array *states)
{
	struct kl_window *window;
	const uint32_t *state;
	size_t count;
	size_t index;
	int was_maximized;
	int resized;

	/* Whether the compositor made the window fullscreen or maximized (the other states change nothing here). */
	(void)toplevel;
	window = data;
	was_maximized = window->maximized;
	window->fullscreen = 0;
	window->maximized = 0;
	state = states->data;
	count = states->size / sizeof(uint32_t);
	for (index = 0; index < count; index++) {
		/* The fullscreen and maximized states among the window's states. */
		if (state[index] == XDG_TOPLEVEL_STATE_FULLSCREEN)
			window->fullscreen = 1;
		if (state[index] == XDG_TOPLEVEL_STATE_MAXIMIZED)
			window->maximized = 1;
	}

	/* A width left to the window is the one it would like, within the compositor's bounds. */
	if (width <= 0) {
		width = (int32_t)window->preferred_width;
		if (window->bounds_width > 0U && window->preferred_width > window->bounds_width)
			width = (int32_t)window->bounds_width;
	}

	/* And so is a height. */
	if (height <= 0) {
		height = (int32_t)window->preferred_height;
		if (window->bounds_height > 0U && window->preferred_height > window->bounds_height)
			height = (int32_t)window->bounds_height;
	}

	/* A new width marks the window resized. */
	resized = 0;
	if (width > 0 && (uint32_t)width != window->width) {
		window->width = (uint32_t)width;
		resized = 1;
	}

	/* And so does a new height. */
	if (height > 0 && (uint32_t)height != window->height) {
		window->height = (uint32_t)height;
		resized = 1;
	}

	/* The application hears of a new size, and of a window maximized or no longer (it may lay out otherwise). */
	if (was_maximized != window->maximized)
		resized = 1;
	if (resized)
		(void)window_push(window, KL_WINDOW_RESIZE);
}

/* The compositor asks the window to close (its close button). */
static void
window_toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct kl_window *window;

	/* The application decides (it may ask about unsaved work first). */
	(void)toplevel;
	window = data;
	(void)window_push(window, KL_WINDOW_CLOSE);
}

/* Keeps the largest size the compositor lets the window choose for itself (0: not known). */
static void
window_toplevel_bounds(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height)
{
	struct kl_window *window;

	/* The width, when known. */
	(void)toplevel;
	window = data;
	window->bounds_width = 0U;
	if (width > 0)
		window->bounds_width = (uint32_t)width;

	/* The height, when known. */
	window->bounds_height = 0U;
	if (height > 0)
		window->bounds_height = (uint32_t)height;
}

/* Takes the seat's pointer, keyboard and touch screen when it has them. */
static void
window_seat_capabilities(
	void *data,
	struct wl_seat *seat,
	uint32_t capabilities)
{
	struct kl_window *window;

	/* A pointer, once. */
	window = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0U && window->pointer == NULL) {
		window->pointer = wl_seat_get_pointer(seat);
		if (window->pointer != NULL)
			(void)wl_pointer_add_listener(window->pointer, &pointer_listener, window);
	}

	/* A keyboard, once. */
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0U && window->keyboard == NULL) {
		window->keyboard = wl_seat_get_keyboard(seat);
		if (window->keyboard != NULL)
			(void)wl_keyboard_add_listener(window->keyboard, &keyboard_listener, window);
	}

	/* A touch screen, once (ws081-p012). */
	if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) != 0U && window->touch == NULL) {
		window->touch = wl_seat_get_touch(seat);
		if (window->touch != NULL)
			(void)wl_touch_add_listener(window->touch, &touch_listener, window);
	}
}

/* The seat's name is not used. */
static void
window_seat_name(
	void *data,
	struct wl_seat *seat,
	const char *name)
{
	/* Nothing to do. */
	(void)data;
	(void)seat;
	(void)name;
}

/* The pointer comes over the window: a motion to where it is. */
static void
window_pointer_enter(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct kl_window *window;

	/* Another surface of the program (the file chooser's) is not the window's. */
	(void)pointer;
	window = data;
	window->pointer_ours = 0;
	if (surface == NULL || surface != window->surface)
		return;
	window->pointer_ours = 1;

	/* The pointer's place, as a motion. */
	window->serial = serial;
	window->pointer_x = wl_fixed_to_double(x);
	window->pointer_y = wl_fixed_to_double(y);
	(void)window_push(window, KL_WINDOW_MOTION);
}

/* The pointer leaves the window. */
static void
window_pointer_leave(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct kl_window *window;

	/* Only leaving the window's own surface matters. */
	(void)pointer;
	(void)serial;
	window = data;
	if (surface == NULL || surface != window->surface)
		return;
	window->pointer_ours = 0;

	/* The view hears that the pointer left. */
	(void)window_push(window, KL_WINDOW_LEAVE);
}

/* The pointer moves over the window. */
static void
window_pointer_motion(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct kl_window *window;
	struct kl_window_event *event;

	/* Only over the window's own surface. */
	(void)pointer;
	window = data;
	if (!window->pointer_ours)
		return;

	/* The new place, as a motion at the compositor's time (libkeiui's version 11: a stroke's samples keep their times). */
	window->pointer_x = wl_fixed_to_double(x);
	window->pointer_y = wl_fixed_to_double(y);
	event = window_push(window, KL_WINDOW_MOTION);
	if (event != NULL)
		keiui_window_stamp(event, time);
}

/* A pointer button is pressed or let go. */
static void
window_pointer_button(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	struct kl_window *window;
	struct kl_window_event *event;

	/* Only over the window's own surface. */
	(void)pointer;
	window = data;
	if (!window->pointer_ours)
		return;

	/* The button as an input at the compositor's time; its serial may set a selection. */
	window->serial = serial;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		window->press_serial = serial;
	event = window_push(window, KL_WINDOW_BUTTON);
	if (event == NULL)
		return;
	keiui_window_stamp(event, time);
	event->code = button;
	event->pressed = 0;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		event->pressed = 1;
}

/* The wheel turns: scrolling in pixels, down and right positive. */
static void
window_pointer_axis(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	uint32_t axis,
	wl_fixed_t value)
{
	struct kl_window *window;
	struct kl_window_event *event;

	/* Only over the window's own surface. */
	(void)pointer;
	window = data;
	if (!window->pointer_ours)
		return;

	/*
	 * The distance, scaled to the window's pixels, on its axis, with its
	 * fraction (a touch pad's fingers move a unit at a time, BUG-218), its
	 * source and the compositor's time.
	 */
	event = window_push(window, KL_WINDOW_AXIS);
	if (event == NULL)
		return;
	keiui_window_stamp(event, time);
	event->axis_source = window->axis_source;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		event->dy = wl_fixed_to_double(value) * WINDOW_SCROLL_SCALE;
	else
		event->dx = wl_fixed_to_double(value) * WINDOW_SCROLL_SCALE;
}

/* A group of pointer events ends (each was queued as it came): the next frame's scrolling is a wheel's until told. */
static void
window_pointer_frame(
	void *data,
	struct wl_pointer *pointer)
{
	struct kl_window *window;

	/* The frame's scrolling is over. */
	(void)pointer;
	window = data;
	window->axis_source = KL_AXIS_SOURCE_WHEEL;
	window->axis_stopped = 0;
}

/* What the frame's scrolling comes from: a wheel, a touch pad's fingers, or something continuous (BUG-211). */
static void
window_pointer_axis_source(
	void *data,
	struct wl_pointer *pointer,
	uint32_t source)
{
	struct kl_window *window;

	/* The source the frame's axis events carry. */
	(void)pointer;
	window = data;
	window->axis_source = KL_AXIS_SOURCE_WHEEL;
	if (source == WL_POINTER_AXIS_SOURCE_FINGER)
		window->axis_source = KL_AXIS_SOURCE_FINGER;
	else if (source == WL_POINTER_AXIS_SOURCE_CONTINUOUS)
		window->axis_source = KL_AXIS_SOURCE_CONTINUOUS;
}

/* The fingers' scrolling ends: one KL_WINDOW_AXIS_STOP for the frame, however many axes stop in it (BUG-211). */
static void
window_pointer_axis_stop(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	uint32_t axis)
{
	struct kl_window *window;
	struct kl_window_event *event;

	/* Only over the window's own surface, and once a frame. */
	(void)pointer;
	(void)axis;
	window = data;
	if (!window->pointer_ours || window->axis_stopped)
		return;
	window->axis_stopped = 1;

	/* The end, at the compositor's time. */
	event = window_push(window, KL_WINDOW_AXIS_STOP);
	if (event == NULL)
		return;
	keiui_window_stamp(event, time);
	event->axis_source = window->axis_source;
}

/* The wheel's notches are not used (the axis value already says how far). */
static void
window_pointer_axis_discrete(
	void *data,
	struct wl_pointer *pointer,
	uint32_t axis,
	int32_t discrete)
{
	/* Nothing to do. */
	(void)data;
	(void)pointer;
	(void)axis;
	(void)discrete;
}

/* Closes the keymap file: keys arrive as evdev codes and the library has its own layout (input.c). */
static void
window_keyboard_keymap(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t format,
	int32_t fd,
	uint32_t size)
{
	/* The descriptor is the window's to close. */
	(void)data;
	(void)keyboard;
	(void)format;
	(void)size;
	if (fd >= 0)
		(void)close(fd);
}

/* Focus arrives: no key is held yet as far as the window is concerned, and the application hears of it. */
static void
window_keyboard_enter(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface,
	struct wl_array *keys)
{
	struct kl_window *window;
	struct kl_window_event *event;

	/* Keys already held when focus came are not pressed again. */
	(void)keyboard;
	(void)keys;
	window = data;
	window->repeat_key = 0U;

	/* Another surface of the program (the file chooser's) has the keys, not the window. */
	window->keyboard_ours = 0;
	if (surface == NULL || surface != window->surface)
		return;
	window->keyboard_ours = 1;
	window->serial = serial;

	/* The focus came. */
	event = window_push(window, KL_WINDOW_FOCUS);
	if (event != NULL)
		event->pressed = 1;
}

/* Focus leaves: nothing repeats any more. */
static void
window_keyboard_leave(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct kl_window *window;

	/* Only leaving the window's own surface matters. */
	(void)keyboard;
	(void)serial;
	window = data;
	if (surface == NULL || surface != window->surface)
		return;
	window->keyboard_ours = 0;

	/* The held key stops repeating, and modifiers are forgotten. */
	window->repeat_key = 0U;
	window->modifiers = 0U;

	/* The focus went. */
	(void)window_push(window, KL_WINDOW_FOCUS);
}

/* A key is pressed (it repeats while held) or let go. */
static void
window_keyboard_key(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	struct kl_window *window;
	struct kl_window_event *event;
	int modifier;

	/* Only while the window's own surface has the keys. */
	(void)keyboard;
	(void)time;
	window = data;
	if (!window->keyboard_ours)
		return;

	/* The key as an input; its serial may set a selection. */
	window->serial = serial;
	event = window_push(window, KL_WINDOW_KEY);
	if (event != NULL) {
		event->code = key;
		event->pressed = 0;
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
			event->pressed = 1;
		keiui_edit_key(window, event);
	}

	/* A release of the repeating key stops it. */
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (key == window->repeat_key)
			window->repeat_key = 0U;
		return;
	}

	/* A key that is not a modifier repeats while held (unless the application turned the repeat off). */
	modifier = window_modifier_key(key);
	if (modifier == 0 && !window->repeat_off) {
		window->repeat_key = key;
		window->repeat_at = keiui_clock_ms() + window->repeat_delay;
	}
}

/* Keeps the modifiers held, in the window's own bits. */
static void
window_keyboard_modifiers(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t depressed,
	uint32_t latched,
	uint32_t locked,
	uint32_t group)
{
	struct kl_window *window;

	/* Only the held modifiers count; the compositor latches and locks nothing. */
	(void)keyboard;
	(void)serial;
	(void)latched;
	(void)locked;
	(void)group;
	window = data;
	window->modifiers = 0U;
	if ((depressed & WINDOW_WAYLAND_SHIFT) != 0U)
		window->modifiers |= KL_MOD_SHIFT;
	if ((depressed & WINDOW_WAYLAND_CTRL) != 0U)
		window->modifiers |= KL_MOD_CTRL;
	if ((depressed & WINDOW_WAYLAND_ALT) != 0U)
		window->modifiers |= KL_MOD_ALT;
	if ((depressed & WINDOW_WAYLAND_SUPER) != 0U)
		window->modifiers |= KL_MOD_SUPER;
}

/* Takes the compositor's repeat rate and delay, when it gives a rate. */
static void
window_keyboard_repeat(
	void *data,
	struct wl_keyboard *keyboard,
	int32_t rate,
	int32_t delay)
{
	struct kl_window *window;

	/* A positive rate is keys per second. */
	(void)keyboard;
	window = data;
	if (rate > 0)
		window->repeat_interval = 1000U / (uint32_t)rate;
	if (delay > 0)
		window->repeat_delay = (uint32_t)delay;
}

/* Tells whether a key is a modifier (which does not repeat). */
static int
window_modifier_key(
	uint32_t key)
{
	/* The shifts, controls, alts, metas and caps lock. */
	switch (key) {
	case WINDOW_KEY_LEFTCTRL:
	case WINDOW_KEY_RIGHTCTRL:
	case WINDOW_KEY_LEFTSHIFT:
	case WINDOW_KEY_RIGHTSHIFT:
	case WINDOW_KEY_LEFTALT:
	case WINDOW_KEY_RIGHTALT:
	case WINDOW_KEY_LEFTMETA:
	case WINDOW_KEY_RIGHTMETA:
	case WINDOW_KEY_CAPSLOCK:
		return 1;
	default:
		break;
	}

	/* Every other key repeats. */
	return 0;
}

/* Queues a touch input with its time turned into the monotonic clock's microseconds. */
static void
window_touch_push(
	struct kl_window *window,
	unsigned kind,
	uint32_t time,
	int32_t id,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct kl_window_event *event;

	/* The input; a full queue drops it. */
	event = window_push(window, kind);
	if (event == NULL)
		return;
	event->id = id;
	event->x = wl_fixed_to_double(x);
	event->y = wl_fixed_to_double(y);

	/* The event's time (a cancel has none: the reading's). */
	if (kind != KL_WINDOW_TOUCH_CANCEL)
		keiui_window_stamp(event, time);
}

/*
 * Sets an input's time from the compositor's (milliseconds of the
 * monotonic clock, the low 32 bits) by how far it is behind the reading;
 * a time far behind or ahead of it is another clock's, and the reading's
 * time stays.
 */
void
keiui_window_stamp(
	struct kl_window_event *event,
	uint32_t time)
{
	uint64_t arrival_ms;
	uint32_t behind;

	/* How far the compositor's time is behind the reading, modulo 2^32 milliseconds. */
	arrival_ms = event->arrival_us / 1000U;
	behind = (uint32_t)arrival_ms - time;

	/* Another clock, or a time ahead: the reading's time stays. */
	if (behind > WINDOW_TOUCH_BEHIND)
		return;

	/* The compositor's time, in microseconds of the full clock. */
	event->time_us = (arrival_ms - behind) * 1000U;
}

/* A finger touches the window. */
static void
window_touch_down(
	void *data,
	struct wl_touch *touch,
	uint32_t serial,
	uint32_t time,
	struct wl_surface *surface,
	int32_t id,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct kl_window *window;

	/* A finger on another surface of the program (the file chooser's) is remembered and left alone. */
	(void)touch;
	window = data;
	if (surface == NULL || surface != window->surface) {
		if (window->foreign_count < KEIUI_WINDOW_FOREIGN) {
			window->foreign[window->foreign_count] = id;
			window->foreign_count++;
		}

		/* The chooser follows it itself. */
		return;
	}

	/* Queued, its serial kept for a long press's context menu. */
	window->serial = serial;
	window->press_serial = serial;
	window_touch_push(window, KL_WINDOW_TOUCH_DOWN, time, id, x, y);
}

/* A finger lifts. */
static void
window_touch_up(
	void *data,
	struct wl_touch *touch,
	uint32_t serial,
	uint32_t time,
	int32_t id)
{
	int foreign;

	/* A finger of another surface is forgotten, not queued. */
	(void)touch;
	(void)serial;
	foreign = window_touch_foreign(data, id, 1);
	if (foreign)
		return;

	/* Queued, with no place. */
	window_touch_push(data, KL_WINDOW_TOUCH_UP, time, id, 0, 0);
}

/* A finger moves. */
static void
window_touch_motion(
	void *data,
	struct wl_touch *touch,
	uint32_t time,
	int32_t id,
	wl_fixed_t x,
	wl_fixed_t y)
{
	int foreign;

	/* A finger of another surface is not the window's. */
	(void)touch;
	foreign = window_touch_foreign(data, id, 0);
	if (foreign)
		return;

	/* Queued. */
	window_touch_push(data, KL_WINDOW_TOUCH_MOTION, time, id, x, y);
}

/* The end of a frame of touch events: each event was queued as it came. */
static void
window_touch_frame(
	void *data,
	struct wl_touch *touch)
{
	/* Nothing to do. */
	(void)data;
	(void)touch;
}

/* The compositor took the fingers. */
static void
window_touch_cancel(
	void *data,
	struct wl_touch *touch)
{
	struct kl_window *window;

	/* Every finger goes, the other surfaces' too; the window's are cancelled. */
	(void)touch;
	window = data;
	window->foreign_count = 0;
	window_touch_push(data, KL_WINDOW_TOUCH_CANCEL, 0, -1, 0, 0);
}

/* Tells whether a finger is down on another surface of the program, and forgets it when asked (its lift). */
static int
window_touch_foreign(
	struct kl_window *window,
	int32_t id,
	int forget)
{
	unsigned index;

	/* Each finger of the other surfaces. */
	for (index = 0; index < window->foreign_count; index++) {
		if (window->foreign[index] != id)
			continue;

		/* Found: forgotten at its lift, the last one taking its slot. */
		if (forget) {
			window->foreign_count--;
			window->foreign[index] = window->foreign[window->foreign_count];
		}

		/* One of another surface's. */
		return 1;
	}

	/* One of the window's own. */
	return 0;
}

/*
 * Lets the application hear the on-screen keyboard's inset (libkeiui's version 7):
 * callback runs during kl_window_dispatch and returns 1 when the
 * application kept its caret in sight itself (the default is skipped).
 */
void
kl_window_on_keyboard_inset(
	struct kl_window *window,
	kl_window_keyboard_inset_fn callback,
	void *data)
{
	/* The callback (NULL: none) and its data. */
	window->inset_callback = callback;
	window->inset_data = data;
}

/*
 * Reports how much of the window the on-screen keyboard covers now, from
 * its right and bottom edges (0 each without a keyboard).
 */
void
kl_window_keyboard_inset(
	const struct kl_window *window,
	int *right,
	int *bottom)
{
	/* The widths last heard. */
	*right = window->inset_right;
	*bottom = window->inset_bottom;
}

/*
 * Hears the on-screen keyboard's inset: kept, told to the application, and
 * (unless the application took care of it) noted for the text view's caret
 * (ui.c).
 */
static void
window_inset(
	void *data,
	int32_t right,
	int32_t bottom,
	uint32_t reason)
{
	struct kl_window *window;
	int handled;

	/* The widths, kept for kl_window_keyboard_inset. */
	window = data;
	window->inset_right = right;
	window->inset_bottom = bottom;

	/* The application first, when it listens. */
	handled = 0;
	if (window->inset_callback != NULL)
		handled = window->inset_callback(window->inset_data, right, bottom, reason);
	if (handled)
		return;

	/* The default: the next frame keeps the caret in sight. */
	keiui_ui_inset_note(window->width, window->height, right, bottom, reason, window->text_cursor);
}

/* The screen's geometry: only the mode matters here. */
static void
window_output_geometry(
	void *data,
	struct wl_output *output,
	int32_t x,
	int32_t y,
	int32_t physical_width,
	int32_t physical_height,
	int32_t subpixel,
	const char *make,
	const char *model,
	int32_t transform)
{
	/* Nothing to keep. */
	(void)data;
	(void)output;
	(void)x;
	(void)y;
	(void)physical_width;
	(void)physical_height;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
}

/* Keeps the screen's current mode: its size and refresh (millihertz). */
static void
window_output_mode(
	void *data,
	struct wl_output *output,
	uint32_t flags,
	int32_t width,
	int32_t height,
	int32_t refresh)
{
	struct kl_window *window;

	/* Only the current mode is the screen's. */
	(void)output;
	window = data;
	if ((flags & WL_OUTPUT_MODE_CURRENT) == 0U)
		return;

	/* The mode. */
	window->output_width = width;
	window->output_height = height;
	window->output_refresh = refresh;
}

/* The end of the screen's description: the mode is already kept. */
static void
window_output_done(
	void *data,
	struct wl_output *output)
{
	/* Nothing to do. */
	(void)data;
	(void)output;
}

/* The screen's scale is not used. */
static void
window_output_scale(
	void *data,
	struct wl_output *output,
	int32_t factor)
{
	/* Nothing to keep. */
	(void)data;
	(void)output;
	(void)factor;
}

/* The desktop surface's place and size: acknowledged before any image of it, a new size heard (KL_VERSION 46). */
static void
window_desktop_configure(
	void *data,
	struct kl_desktop *desktop,
	uint32_t serial,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	struct kl_window *window;
	int resized;

	/* The acknowledgement comes before any image of the new size; the place is kept. */
	window = data;
	kl_desktop_ack(desktop, serial);
	window->configured = 1;
	window->desktop_x = x;
	window->desktop_y = y;

	/* A new width or height marks the surface resized. */
	resized = 0;
	if (width > 0 && (uint32_t)width != window->width) {
		window->width = (uint32_t)width;
		resized = 1;
	}
	if (height > 0 && (uint32_t)height != window->height) {
		window->height = (uint32_t)height;
		resized = 1;
	}

	/* The application hears of a new size. */
	if (resized)
		(void)window_push(window, KL_WINDOW_RESIZE);
	keiui_window_wake(window);
}
