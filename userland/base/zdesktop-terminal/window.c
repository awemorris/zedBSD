/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Wayland window of zdesktop-terminal: an xdg-shell toplevel, the
 * seat's keyboard and the repeating of a held key.
 *
 * A key press is turned into bytes at once (keys.c) and kept until the main
 * loop writes them to the shell.  zwl does not repeat keys itself, so a key
 * held past the repeat delay is pressed again on each interval.
 */

#include "terminal.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The seat and keyboard version the window understands. */
#define WINDOW_SEAT_VERSION	5U

/* The repeat's delay and interval when the compositor gives none, in milliseconds. */
#define WINDOW_REPEAT_DELAY	400U
#define WINDOW_REPEAT_INTERVAL	40U

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
static void window_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities);
static void window_seat_name(void *data, struct wl_seat *seat, const char *name);
static void window_keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size);
static void window_keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys);
static void window_keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface);
static void window_keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void window_keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
static void window_keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay);
static void window_press(struct terminal_window *window, uint32_t key);
static int window_modifier_key(uint32_t key);

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

/* The size the compositor gives the window, and its request to close. */
static const struct xdg_toplevel_listener toplevel_listener = {
	window_toplevel_configure, window_toplevel_close
};

/* The seat's devices and name. */
static const struct wl_seat_listener seat_listener = {
	window_seat_capabilities, window_seat_name
};

/* The keyboard's events of versions 1 to 5. */
static const struct wl_keyboard_listener keyboard_listener = {
	window_keyboard_keymap, window_keyboard_enter, window_keyboard_leave,
	window_keyboard_key, window_keyboard_modifiers, window_keyboard_repeat
};

/*
 * Connects to the compositor and makes a toplevel window of a size.
 *
 * Returns 0 once the first configure is acknowledged, or -1 with errno set.
 */
int
terminal_window_open(
	struct terminal_window *window,
	const char *display,
	uint32_t width,
	uint32_t height)
{
	int status;

	/* The size the window asks for until the compositor gives one. */
	memset(window, 0, sizeof(*window));
	window->width = width;
	window->height = height;
	window->repeat_delay = WINDOW_REPEAT_DELAY;
	window->repeat_interval = WINDOW_REPEAT_INTERVAL;

	/* The connection. */
	window->display = wl_display_connect(display);
	if (window->display == NULL)
		return -1;

	/* The globals: the compositor, the shell and the seat. */
	window->registry = wl_display_get_registry(window->display);
	if (window->registry == NULL)
		return -1;

	/* Listens for the globals the compositor announces. */
	status = wl_registry_add_listener(window->registry, &registry_listener, window);
	if (status != 0)
		return -1;

	/* Waits until every global has been announced. */
	status = wl_display_roundtrip(window->display);
	if (status < 0)
		return -1;

	/* A window needs a compositor and a shell. */
	if (window->compositor == NULL || window->shell == NULL) {
		errno = EOPNOTSUPP;
		return -1;
	}

	/* The surface and its toplevel role. */
	window->surface = wl_compositor_create_surface(window->compositor);
	if (window->surface == NULL)
		return -1;

	/* The surface becomes an xdg surface. */
	window->role = xdg_wm_base_get_xdg_surface(window->shell, window->surface);
	if (window->role == NULL)
		return -1;

	/* Listens for its configures. */
	status = xdg_surface_add_listener(window->role, &surface_listener, window);
	if (status != 0)
		return -1;

	/* The xdg surface becomes a toplevel window. */
	window->toplevel = xdg_surface_get_toplevel(window->role);
	if (window->toplevel == NULL)
		return -1;

	/* Listens for its size and its close request. */
	status = xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);
	if (status != 0)
		return -1;

	/* The title the compositor shows and the application's identity. */
	xdg_toplevel_set_title(window->toplevel, "Terminal");
	xdg_toplevel_set_app_id(window->toplevel, "zdesktop-terminal");
	wl_surface_commit(window->surface);

	/* The first configure (and the seat's devices) before anything is drawn. */
	status = wl_display_roundtrip(window->display);
	if (status < 0)
		return -1;

	/* A compositor that did not configure the window cannot take its images. */
	if (window->configured == 0) {
		errno = EPROTO;
		return -1;
	}

	/* Succeeded: the window can be drawn into. */
	window->resized = 0;
	return 0;
}

/*
 * Waits for the compositor or another descriptor (the shell's) and runs
 * the compositor's events.
 *
 * `timeout` is in milliseconds (-1 waits for ever); *other_ready tells
 * whether the other descriptor has something to read or has hung up.
 * Returns 0, or -1 when the connection is broken.
 */
int
terminal_window_dispatch(
	struct terminal_window *window,
	int other,
	int timeout,
	int *other_ready)
{
	struct pollfd descriptors[2];
	int count;
	int status;

	/* Runs what is queued until a read of new events can be reserved. */
	*other_ready = 0;
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

	/* Waits for either descriptor. */
	descriptors[0].fd = wl_display_get_fd(window->display);
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	descriptors[1].fd = other;
	descriptors[1].events = POLLIN;
	descriptors[1].revents = 0;
	count = 1;
	if (other >= 0)
		count = 2;
	status = poll(descriptors, (nfds_t)count, timeout);

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

	/* The other descriptor has bytes, or its end has gone. */
	if (count == 2 && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
		*other_ready = 1;

	/* Runs the events read. */
	status = wl_display_dispatch_pending(window->display);
	if (status < 0)
		return -1;

	/* Succeeded: the events so far have run. */
	return 0;
}

/*
 * Presses the held key again when its repeat is due.
 */
void
terminal_window_repeat(
	struct terminal_window *window,
	uint64_t now)
{
	/* No key held, or not yet time. */
	if (window->repeat_key == 0U || now < window->repeat_at)
		return;

	/* The key's bytes again, and the next repeat one interval later. */
	window_press(window, window->repeat_key);
	window->repeat_at = now + window->repeat_interval;
}

/*
 * Destroys the window's objects and disconnects.
 */
void
terminal_window_close(
	struct terminal_window *window)
{
	/* The keyboard and the seat. */
	if (window->keyboard != NULL)
		wl_keyboard_destroy(window->keyboard);
	if (window->seat != NULL)
		wl_seat_destroy(window->seat);

	/* The roles before the surface, the surface before the globals that made it. */
	if (window->toplevel != NULL)
		xdg_toplevel_destroy(window->toplevel);
	if (window->role != NULL)
		xdg_surface_destroy(window->role);
	if (window->surface != NULL)
		wl_surface_destroy(window->surface);
	if (window->shell != NULL)
		xdg_wm_base_destroy(window->shell);
	if (window->compositor != NULL)
		wl_compositor_destroy(window->compositor);
	if (window->registry != NULL)
		wl_registry_destroy(window->registry);

	/* The connection last. */
	if (window->display != NULL)
		wl_display_disconnect(window->display);
	memset(window, 0, sizeof(*window));
}

/*
 * Returns a monotonic time in milliseconds (0 when the clock cannot be read).
 */
uint64_t
terminal_clock(void)
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

/* Binds the compositor, the shell and the first seat. */
static void
window_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct terminal_window *window;
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

	/* The shell gives the surface its window role. */
	match = strcmp(interface, "xdg_wm_base");
	if (match == 0 && window->shell == NULL) {
		window->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1U);
		if (window->shell != NULL)
			(void)xdg_wm_base_add_listener(window->shell, &shell_listener, window);
		return;
	}

	/* The seat gives the keyboard; without one the terminal only shows. */
	match = strcmp(interface, "wl_seat");
	if (match == 0 && window->seat == NULL) {
		if (version > WINDOW_SEAT_VERSION)
			version = WINDOW_SEAT_VERSION;
		window->seat = wl_registry_bind(registry, name, &wl_seat_interface, version);
		if (window->seat != NULL)
			(void)wl_seat_add_listener(window->seat, &seat_listener, window);
	}
}

/* A global going away does not matter to a terminal that already bound what it needs. */
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
	struct terminal_window *window;

	/* The acknowledgement comes before any image of the new state. */
	window = data;
	xdg_surface_ack_configure(surface, serial);
	window->configured = 1;
}

/* Takes the size the compositor gives; zero keeps the window's own. */
static void
window_toplevel_configure(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height,
	struct wl_array *states)
{
	struct terminal_window *window;

	/* The states (maximized, activated) change nothing but the size. */
	(void)toplevel;
	(void)states;
	window = data;

	/* A new width or height marks the window resized. */
	if (width > 0 && (uint32_t)width != window->width) {
		window->width = (uint32_t)width;
		window->resized = 1;
	}

	/* And so does a new height. */
	if (height > 0 && (uint32_t)height != window->height) {
		window->height = (uint32_t)height;
		window->resized = 1;
	}
}

/* The compositor asks the window to close (its close button). */
static void
window_toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct terminal_window *window;

	/* The main loop ends the terminal. */
	(void)toplevel;
	window = data;
	window->closed = 1;
}

/* Takes the seat's keyboard when it has one. */
static void
window_seat_capabilities(
	void *data,
	struct wl_seat *seat,
	uint32_t capabilities)
{
	struct terminal_window *window;

	/* A keyboard, once. */
	window = data;
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0U && window->keyboard == NULL) {
		window->keyboard = wl_seat_get_keyboard(seat);
		if (window->keyboard != NULL)
			(void)wl_keyboard_add_listener(window->keyboard, &keyboard_listener, window);
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

/* Closes the keymap file: keys arrive as evdev codes and the terminal has its own layout. */
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

/* Focus arrives: no key is held yet as far as the terminal is concerned. */
static void
window_keyboard_enter(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface,
	struct wl_array *keys)
{
	struct terminal_window *window;

	/* Keys already held when focus came are not typed. */
	(void)keyboard;
	(void)serial;
	(void)surface;
	(void)keys;
	window = data;
	window->repeat_key = 0U;
}

/* Focus leaves: nothing repeats any more. */
static void
window_keyboard_leave(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct terminal_window *window;

	/* The held key stops repeating, and modifiers are forgotten. */
	(void)keyboard;
	(void)serial;
	(void)surface;
	window = data;
	window->repeat_key = 0U;
	window->modifiers = 0U;
}

/* Types a pressed key and starts its repeat; a release stops the repeat. */
static void
window_keyboard_key(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	struct terminal_window *window;
	int modifier;

	/* The serial and the time are not needed. */
	(void)keyboard;
	(void)serial;
	(void)time;
	window = data;

	/* A release of the repeating key stops it. */
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (key == window->repeat_key)
			window->repeat_key = 0U;
		return;
	}

	/* The press types, and a key that is not a modifier repeats while held. */
	window_press(window, key);
	modifier = window_modifier_key(key);
	if (modifier == 0) {
		window->repeat_key = key;
		window->repeat_at = terminal_clock() + window->repeat_delay;
	}
}

/* Keeps the modifiers held (shift, control, alt), which change what keys type. */
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
	struct terminal_window *window;

	/* Only the held modifiers count; zwl latches and locks nothing. */
	(void)keyboard;
	(void)serial;
	(void)latched;
	(void)locked;
	(void)group;
	window = data;
	window->modifiers = depressed;
}

/* Takes the compositor's repeat rate and delay, when it gives a rate. */
static void
window_keyboard_repeat(
	void *data,
	struct wl_keyboard *keyboard,
	int32_t rate,
	int32_t delay)
{
	struct terminal_window *window;

	/* A rate of zero turns repeat off; a positive rate is keys per second. */
	(void)keyboard;
	window = data;
	if (rate > 0)
		window->repeat_interval = 1000U / (uint32_t)rate;
	if (delay > 0)
		window->repeat_delay = (uint32_t)delay;
}

/* Adds a key's bytes to what the shell reads next. */
static void
window_press(
	struct terminal_window *window,
	uint32_t key)
{
	size_t length;

	/* The bytes, if they fit in what is left of the buffer. */
	length = terminal_key_bytes(key, window->modifiers, window->input + window->input_length, sizeof(window->input) - window->input_length);
	window->input_length += length;
}

/* Tells whether a key is a modifier (which types nothing and does not repeat). */
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

	/* Every other key is typed and repeats. */
	return 0;
}
