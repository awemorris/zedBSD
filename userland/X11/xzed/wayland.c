/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Xzed's Wayland backend (WS069 p002, plan/ws069/design.md): the X screen
 * as one xdg-shell window of zwl (rootful, as Xephyr), in place of
 * /dev/graphics and /dev/input.
 *
 * The screen Xzed composites is copied into one of two wl_shm buffers, the
 * one the compositor has released, and committed with the changed
 * rectangle as damage.  The window's pointer and keyboard events become
 * Xzed's pointer frames (absolute, in screen coordinates) and key events
 * (the evdev key code through Xzed's own keycode table).  The compositor
 * draws the pointer; X cursors are not shown.
 */

#include "wayland.h"

#include <errno.h>

#ifdef XZED_WAYLAND

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

/* The evdev codes of the pointer's buttons and the keys that change a keycode. */
#define WAYLAND_BTN_LEFT	0x110U
#define WAYLAND_BTN_RIGHT	0x111U
#define WAYLAND_BTN_MIDDLE	0x112U
#define WAYLAND_KEY_CAPSLOCK	58U

/* How many evdev key codes are remembered for their releases. */
#define WAYLAND_KEYS		256U

/* The modifier bits of wl_keyboard.modifiers (the same bits as Xzed's). */
#define WAYLAND_MODIFIERS	(XZED_INPUT_SHIFT_MASK | XZED_INPUT_CONTROL_MASK | XZED_INPUT_ALT_MASK)

/*
 * One wl_shm buffer of the whole screen.
 */
struct wayland_buffer {
	struct wl_buffer *buffer;
	uint32_t *pixels;

	/* Nonzero while the compositor holds it. */
	int busy;
};

/*
 * The backend: the connection, the window, its two buffers, and the input
 * state that turns Wayland's events into Xzed's.
 */
struct xzed_wayland {
	/* The connection and the globals bound from it. */
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;

	/* The window. */
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	int configured;
	int closed;

	/* The screen's size, the shared memory of the two buffers, and the buffers. */
	unsigned width;
	unsigned height;
	void *memory;
	size_t memory_size;
	struct wayland_buffer buffers[2];

	/* Xzed's input handlers. */
	struct xzed_input_handlers handlers;
	void *context;

	/* The pointer's place, the buttons held (Xzed's bits), and the modifiers and caps lock. */
	int pointer_x;
	int pointer_y;
	uint16_t buttons;
	uint32_t modifiers;
	int caps_lock;

	/* The X keycode each evdev key was pressed as, so that its release says the same. */
	uint8_t keycodes[WAYLAND_KEYS];
};

static int wayland_buffers(struct xzed_wayland *wayland);
static void wayland_pointer_frame(struct xzed_wayland *wayland, uint32_t time, int button, int pressed, uint16_t bit);
static void wayland_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void wayland_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void wayland_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void wayland_configure(void *data, struct xdg_surface *surface, uint32_t serial);
static void wayland_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void wayland_toplevel_close(void *data, struct xdg_toplevel *toplevel);
static void wayland_buffer_release(void *data, struct wl_buffer *buffer);
static void wayland_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities);
static void wayland_seat_name(void *data, struct wl_seat *seat, const char *name);
static void wayland_pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y);
static void wayland_pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface);
static void wayland_pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void wayland_pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
static void wayland_pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value);
static void wayland_keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size);
static void wayland_keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys);
static void wayland_keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface);
static void wayland_keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void wayland_keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);

/* The registry's callbacks. */
static const struct wl_registry_listener wayland_registry_listener = {
	wayland_global, wayland_global_remove
};

/* The shell's liveness check. */
static const struct xdg_wm_base_listener wayland_shell_listener = {
	wayland_ping
};

/* The window role's configure. */
static const struct xdg_surface_listener wayland_surface_listener = {
	wayland_configure
};

/* The toplevel's size and close request. */
static const struct xdg_toplevel_listener wayland_toplevel_listener = {
	wayland_toplevel_configure, wayland_toplevel_close
};

/* A buffer given back by the compositor. */
static const struct wl_buffer_listener wayland_buffer_listener = {
	wayland_buffer_release
};

/* The seat's devices. */
static const struct wl_seat_listener wayland_seat_listener = {
	wayland_seat_capabilities, wayland_seat_name
};

/*
 * The pointer's events of version 1 (enter, leave, motion, button, axis),
 * filled in by xzed_wayland_open (the listener has more members than
 * version 1 uses, which are left empty).
 */
static struct wl_pointer_listener wayland_pointer_listener;

/* The keyboard's events of version 1 (no repeat information). */
static const struct wl_keyboard_listener wayland_keyboard_listener = {
	wayland_keyboard_keymap, wayland_keyboard_enter, wayland_keyboard_leave,
	wayland_keyboard_key, wayland_keyboard_modifiers, NULL
};

/*
 * Connects to a Wayland compositor and opens the X screen's window of a
 * size.  Returns 0, or -1 with errno set.
 */
int
xzed_wayland_open(
	struct xzed_wayland **out,
	const char *display,
	unsigned width,
	unsigned height,
	const struct xzed_input_handlers *handlers,
	void *context)
{
	struct xzed_wayland *wayland;
	int status;

	/* The backend, with Xzed's handlers. */
	*out = NULL;
	wayland = calloc(1U, sizeof(*wayland));
	if (wayland == NULL)
		return -1;
	*out = wayland;
	wayland->width = width;
	wayland->height = height;
	wayland->handlers = *handlers;
	wayland->context = context;
	wayland->pointer_x = (int)width / 2;
	wayland->pointer_y = (int)height / 2;

	/* The pointer's callbacks. */
	memset(&wayland_pointer_listener, 0, sizeof(wayland_pointer_listener));
	wayland_pointer_listener.enter = wayland_pointer_enter;
	wayland_pointer_listener.leave = wayland_pointer_leave;
	wayland_pointer_listener.motion = wayland_pointer_motion;
	wayland_pointer_listener.button = wayland_pointer_button;
	wayland_pointer_listener.axis = wayland_pointer_axis;

	/* The connection and its globals. */
	wayland->display = wl_display_connect(display);
	if (wayland->display == NULL)
		return -1;

	/* The registry that announces them. */
	wayland->registry = wl_display_get_registry(wayland->display);
	if (wayland->registry == NULL)
		return -1;

	/* The globals are bound as they are announced. */
	(void)wl_registry_add_listener(wayland->registry, &wayland_registry_listener, wayland);
	status = wl_display_roundtrip(wayland->display);
	if (status < 0)
		return -1;

	/* A window needs a compositor, shared memory and a shell. */
	if (wayland->compositor == NULL || wayland->shm == NULL || wayland->shell == NULL) {
		errno = EOPNOTSUPP;
		return -1;
	}

	/* The surface as a toplevel window titled X11. */
	wayland->surface = wl_compositor_create_surface(wayland->compositor);
	if (wayland->surface == NULL)
		return -1;

	/* Its window role. */
	wayland->role = xdg_wm_base_get_xdg_surface(wayland->shell, wayland->surface);
	if (wayland->role == NULL)
		return -1;

	/* The role's configures and the toplevel's events are heard. */
	(void)xdg_surface_add_listener(wayland->role, &wayland_surface_listener, wayland);
	wayland->toplevel = xdg_surface_get_toplevel(wayland->role);
	if (wayland->toplevel == NULL)
		return -1;

	/* The title, the application's identity, and the first configure. */
	(void)xdg_toplevel_add_listener(wayland->toplevel, &wayland_toplevel_listener, wayland);
	xdg_toplevel_set_title(wayland->toplevel, "X11");
	xdg_toplevel_set_app_id(wayland->toplevel, "Xzed");
	wl_surface_commit(wayland->surface);
	status = wl_display_roundtrip(wayland->display);
	if (status < 0)
		return -1;

	/* A compositor that did not configure the window takes no buffers. */
	if (!wayland->configured) {
		errno = EPROTO;
		return -1;
	}

	/* The two buffers of the screen. */
	status = wayland_buffers(wayland);
	if (status != 0)
		return -1;

	/* Succeeded: the window can show the screen. */
	return 0;
}

/*
 * Returns the descriptor Xzed polls for the compositor's events.
 */
int
xzed_wayland_fd(
	const struct xzed_wayland *wayland)
{
	int descriptor;

	/* The connection's socket. */
	descriptor = wl_display_get_fd(wayland->display);
	return descriptor;
}

/*
 * Reads the compositor's events when the descriptor is readable, and runs
 * them (Xzed's input handlers among them).  Returns 0, or -1 when the
 * connection is broken.
 */
int
xzed_wayland_dispatch(
	struct xzed_wayland *wayland,
	int readable)
{
	int status;

	/* The events that came with the socket, read and run. */
	if (readable) {
		status = wl_display_dispatch(wayland->display);
		if (status < 0)
			return -1;
	}

	/* Events already queued, and the requests made while running them. */
	status = wl_display_dispatch_pending(wayland->display);
	if (status < 0)
		return -1;
	(void)wl_display_flush(wayland->display);

	/* Succeeded: every event so far has run. */
	return 0;
}

/*
 * Shows the screen in the window: the whole screen is copied into a free
 * buffer, and the changed rectangle is the damage.  Returns 0 when it was
 * committed, or 1 when both buffers are still the compositor's (the caller
 * tries again later).
 */
int
xzed_wayland_present(
	struct xzed_wayland *wayland,
	const uint32_t *screen,
	int x,
	int y,
	int width,
	int height)
{
	struct wayland_buffer *free_buffer;
	unsigned index;

	/* The first buffer the compositor has given back. */
	free_buffer = NULL;
	for (index = 0U; index < 2U; index++) {
		if (!wayland->buffers[index].busy) {
			free_buffer = &wayland->buffers[index];
			break;
		}
	}

	/* Both are still held: the frame waits. */
	if (free_buffer == NULL)
		return 1;

	/* The whole screen, so that the buffer is complete whatever it showed before. */
	memcpy(free_buffer->pixels, screen, (size_t)wayland->width * wayland->height * 4U);
	free_buffer->busy = 1;

	/* Attached, with the change as damage, and committed. */
	wl_surface_attach(wayland->surface, free_buffer->buffer, 0, 0);
	wl_surface_damage(wayland->surface, x, y, width, height);
	wl_surface_commit(wayland->surface);
	(void)wl_display_flush(wayland->display);

	/* Succeeded: the frame is the compositor's. */
	return 0;
}

/*
 * Tells whether the compositor asked the window to close.
 */
int
xzed_wayland_closed(
	const struct xzed_wayland *wayland)
{
	/* The close event sets it. */
	return wayland->closed;
}

/*
 * Destroys the window and disconnects.
 */
void
xzed_wayland_close(
	struct xzed_wayland *wayland)
{
	unsigned index;

	/* Nothing was opened. */
	if (wayland == NULL)
		return;

	/* The buffers and their memory. */
	for (index = 0U; index < 2U; index++) {
		if (wayland->buffers[index].buffer != NULL)
			wl_buffer_destroy(wayland->buffers[index].buffer);
	}

	/* The shared memory under them. */
	if (wayland->memory != NULL)
		(void)munmap(wayland->memory, wayland->memory_size);

	/* The input devices, the window, the globals, the connection. */
	if (wayland->keyboard != NULL)
		wl_keyboard_destroy(wayland->keyboard);
	if (wayland->pointer != NULL)
		wl_pointer_destroy(wayland->pointer);
	if (wayland->seat != NULL)
		wl_seat_destroy(wayland->seat);
	if (wayland->toplevel != NULL)
		xdg_toplevel_destroy(wayland->toplevel);
	if (wayland->role != NULL)
		xdg_surface_destroy(wayland->role);
	if (wayland->surface != NULL)
		wl_surface_destroy(wayland->surface);
	if (wayland->shell != NULL)
		xdg_wm_base_destroy(wayland->shell);
	if (wayland->shm != NULL)
		wl_shm_destroy(wayland->shm);
	if (wayland->compositor != NULL)
		wl_compositor_destroy(wayland->compositor);
	if (wayland->registry != NULL)
		wl_registry_destroy(wayland->registry);
	if (wayland->display != NULL)
		wl_display_disconnect(wayland->display);

	/* The backend itself. */
	free(wayland);
}

/* Makes the two screen-sized XRGB8888 buffers in one shared-memory pool. */
static int
wayland_buffers(
	struct xzed_wayland *wayland)
{
	struct wl_shm_pool *pool;
	char name[64];
	size_t bytes;
	unsigned index;
	int descriptor;
	int error;

	/* Anonymous shared memory for both buffers. */
	bytes = (size_t)wayland->width * wayland->height * 4U;
	wayland->memory_size = bytes * 2U;
	snprintf(name, sizeof(name), "/xzed-%ld", (long)getpid());
	descriptor = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0)
		return -1;
	(void)shm_unlink(name);

	/* Its size and mapping. */
	error = ftruncate(descriptor, (off_t)wayland->memory_size);
	if (error != 0) {
		close(descriptor);
		return -1;
	}

	/* Mapped for Xzed's writes. */
	wayland->memory = mmap(NULL, wayland->memory_size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	if (wayland->memory == MAP_FAILED) {
		wayland->memory = NULL;
		close(descriptor);
		return -1;
	}

	/* The pool the compositor maps, and a buffer of each half. */
	pool = wl_shm_create_pool(wayland->shm, descriptor, (int32_t)wayland->memory_size);
	close(descriptor);
	if (pool == NULL)
		return -1;
	for (index = 0U; index < 2U; index++) {
		wayland->buffers[index].pixels = (uint32_t *)((unsigned char *)wayland->memory + bytes * index);
		wayland->buffers[index].buffer = wl_shm_pool_create_buffer(pool, (int32_t)(bytes * index), (int32_t)wayland->width, (int32_t)wayland->height, (int32_t)wayland->width * 4, WL_SHM_FORMAT_XRGB8888);
		if (wayland->buffers[index].buffer == NULL) {
			wl_shm_pool_destroy(pool);
			return -1;
		}

		/* The release says the compositor is done with it. */
		(void)wl_buffer_add_listener(wayland->buffers[index].buffer, &wayland_buffer_listener, &wayland->buffers[index]);
	}

	/* Succeeded: the pool lives on in its buffers. */
	wl_shm_pool_destroy(pool);
	return 0;
}

/* Gives Xzed one pointer frame: the pointer's place, and a button's change when there is one. */
static void
wayland_pointer_frame(
	struct xzed_wayland *wayland,
	uint32_t time,
	int button,
	int pressed,
	uint16_t bit)
{
	struct xzed_input_pointer_frame frame;

	/* Where the pointer is, in the screen's coordinates. */
	memset(&frame, 0, sizeof(frame));
	frame.absolute = 1;
	frame.absolute_x = wayland->pointer_x;
	frame.absolute_y = wayland->pointer_y;
	frame.time = time;
	frame.buttons_before = wayland->buttons;

	/* A button's change is one edge. */
	if (button != 0) {
		if (pressed) {
			wayland->buttons = (uint16_t)(wayland->buttons | bit);
		} else {
			wayland->buttons = (uint16_t)(wayland->buttons & ~bit);
		}

		/* The change as the frame's one edge. */
		frame.edges[0].button = (uint8_t)button;
		frame.edges[0].pressed = (uint8_t)pressed;
		frame.edges[0].buttons = wayland->buttons;
		frame.edge_count = 1U;
	}

	/* Xzed hears it. */
	frame.buttons_after = wayland->buttons;
	wayland->handlers.pointer(wayland->context, &frame);
}

/* Binds the compositor, shared memory, the shell and the seat. */
static void
wayland_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct xzed_wayland *wayland;
	int compositor;
	int shm;
	int shell;
	int seat;

	/* Which of the four this is. */
	wayland = data;
	(void)version;
	compositor = strcmp(interface, "wl_compositor");
	shm = strcmp(interface, "wl_shm");
	shell = strcmp(interface, "xdg_wm_base");
	seat = strcmp(interface, "wl_seat");

	/* Each is bound once, at the version this backend uses. */
	if (compositor == 0 && wayland->compositor == NULL) {
		wayland->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4U);
	} else if (shm == 0 && wayland->shm == NULL) {
		wayland->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1U);
	} else if (shell == 0 && wayland->shell == NULL) {
		wayland->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1U);
		if (wayland->shell != NULL)
			(void)xdg_wm_base_add_listener(wayland->shell, &wayland_shell_listener, wayland);
	} else if (seat == 0 && wayland->seat == NULL) {
		wayland->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1U);
		if (wayland->seat != NULL)
			(void)wl_seat_add_listener(wayland->seat, &wayland_seat_listener, wayland);
	}
}

/* A global going away does not matter to a window that is up. */
static void
wayland_global_remove(
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
wayland_ping(
	void *data,
	struct xdg_wm_base *shell,
	uint32_t serial)
{
	/* The same serial back. */
	(void)data;
	xdg_wm_base_pong(shell, serial);
}

/* Acknowledges a configure. */
static void
wayland_configure(
	void *data,
	struct xdg_surface *surface,
	uint32_t serial)
{
	struct xzed_wayland *wayland;

	/* The window may take buffers from here on. */
	wayland = data;
	xdg_surface_ack_configure(surface, serial);
	wayland->configured = 1;
}

/* The X screen keeps its size whatever the compositor asks (rootful). */
static void
wayland_toplevel_configure(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height,
	struct wl_array *states)
{
	/* The size stays the screen's. */
	(void)data;
	(void)toplevel;
	(void)width;
	(void)height;
	(void)states;
}

/* The compositor asks the window to close: Xzed ends. */
static void
wayland_toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct xzed_wayland *wayland;

	/* Xzed's loop sees it. */
	(void)toplevel;
	wayland = data;
	wayland->closed = 1;
}

/* The compositor has given a buffer back. */
static void
wayland_buffer_release(
	void *data,
	struct wl_buffer *buffer)
{
	struct wayland_buffer *released;

	/* It can take the next frame. */
	(void)buffer;
	released = data;
	released->busy = 0;
}

/* Takes the seat's pointer and keyboard. */
static void
wayland_seat_capabilities(
	void *data,
	struct wl_seat *seat,
	uint32_t capabilities)
{
	struct xzed_wayland *wayland;

	/* The pointer, once. */
	wayland = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0U && wayland->pointer == NULL) {
		wayland->pointer = wl_seat_get_pointer(seat);
		if (wayland->pointer != NULL)
			(void)wl_pointer_add_listener(wayland->pointer, &wayland_pointer_listener, wayland);
	}

	/* The keyboard, once. */
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0U && wayland->keyboard == NULL) {
		wayland->keyboard = wl_seat_get_keyboard(seat);
		if (wayland->keyboard != NULL)
			(void)wl_keyboard_add_listener(wayland->keyboard, &wayland_keyboard_listener, wayland);
	}
}

/* The seat's name is not used. */
static void
wayland_seat_name(
	void *data,
	struct wl_seat *seat,
	const char *name)
{
	/* Nothing to do. */
	(void)data;
	(void)seat;
	(void)name;
}

/* The pointer comes into the window: Xzed's pointer moves there. */
static void
wayland_pointer_enter(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct xzed_wayland *wayland;

	/* The compositor draws its own pointer over the window. */
	(void)surface;
	wayland = data;
	wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
	wayland->pointer_x = wl_fixed_to_int(x);
	wayland->pointer_y = wl_fixed_to_int(y);
	wayland_pointer_frame(wayland, 0U, 0, 0, 0U);
}

/* The pointer leaves the window: nothing changes in X. */
static void
wayland_pointer_leave(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface)
{
	/* Nothing to do. */
	(void)data;
	(void)pointer;
	(void)serial;
	(void)surface;
}

/* The pointer moves within the window. */
static void
wayland_pointer_motion(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct xzed_wayland *wayland;

	/* Its place in the screen's coordinates. */
	(void)pointer;
	wayland = data;
	wayland->pointer_x = wl_fixed_to_int(x);
	wayland->pointer_y = wl_fixed_to_int(y);
	wayland_pointer_frame(wayland, time, 0, 0, 0U);
}

/* A button is pressed or released: X's buttons 1 (left), 2 (middle), 3 (right). */
static void
wayland_pointer_button(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	struct xzed_wayland *wayland;
	int pressed;

	/* The serial is not needed. */
	(void)pointer;
	(void)serial;
	wayland = data;
	pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;

	/* Routes the button to X's number and bit. */
	switch (button) {
	case WAYLAND_BTN_LEFT:
		wayland_pointer_frame(wayland, time, 1, pressed, 1U << 0);
		break;
	case WAYLAND_BTN_MIDDLE:
		wayland_pointer_frame(wayland, time, 2, pressed, 1U << 1);
		break;
	case WAYLAND_BTN_RIGHT:
		wayland_pointer_frame(wayland, time, 3, pressed, 1U << 2);
		break;
	default:
		break;
	}
}

/* Scrolling is not passed on yet. */
static void
wayland_pointer_axis(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	uint32_t axis,
	wl_fixed_t value)
{
	/* Nothing to do. */
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
	(void)value;
}

/* Closes the keymap: keys arrive as evdev codes and Xzed has its own table. */
static void
wayland_keyboard_keymap(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t format,
	int32_t fd,
	uint32_t size)
{
	/* The descriptor is the backend's to close. */
	(void)data;
	(void)keyboard;
	(void)format;
	(void)size;
	if (fd >= 0)
		(void)close(fd);
}

/* Focus comes: nothing is held as far as X is concerned. */
static void
wayland_keyboard_enter(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface,
	struct wl_array *keys)
{
	/* Keys held when focus came are not typed. */
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)surface;
	(void)keys;
}

/* Focus leaves: the modifiers are forgotten. */
static void
wayland_keyboard_leave(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct xzed_wayland *wayland;

	/* No modifier is held any more. */
	(void)keyboard;
	(void)serial;
	(void)surface;
	wayland = data;
	wayland->modifiers = 0U;
}

/* A key is pressed or released: Xzed hears its X keycode with the modifiers. */
static void
wayland_keyboard_key(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	struct xzed_wayland *wayland;
	uint8_t keycode;
	int shifted;

	/* The serial is not needed; a code past the table is ignored. */
	(void)keyboard;
	(void)serial;
	wayland = data;
	if (key >= WAYLAND_KEYS)
		return;

	/* Caps lock toggles on its press and types nothing. */
	if (key == WAYLAND_KEY_CAPSLOCK) {
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
			wayland->caps_lock = !wayland->caps_lock;
		return;
	}

	/* A release says the keycode its press had. */
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		keycode = wayland->keycodes[key];
		wayland->keycodes[key] = 0U;
		if (keycode != 0U)
			wayland->handlers.key(wayland->context, keycode, 0, time, (uint16_t)(wayland->modifiers & WAYLAND_MODIFIERS));
		return;
	}

	/* A press: the keycode in the current shift and caps lock states. */
	shifted = (wayland->modifiers & XZED_INPUT_SHIFT_MASK) != 0U;
	keycode = xzed_input_keycode((uint16_t)key, shifted, wayland->caps_lock);
	if (keycode == 0U)
		return;

	/* Succeeded: Xzed hears the press. */
	wayland->keycodes[key] = keycode;
	wayland->handlers.key(wayland->context, keycode, 1, time, (uint16_t)(wayland->modifiers & WAYLAND_MODIFIERS));
}

/* Keeps the modifiers held (the same bits as Xzed's). */
static void
wayland_keyboard_modifiers(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t depressed,
	uint32_t latched,
	uint32_t locked,
	uint32_t group)
{
	struct xzed_wayland *wayland;

	/* Only the held ones count. */
	(void)keyboard;
	(void)serial;
	(void)latched;
	(void)locked;
	(void)group;
	wayland = data;
	wayland->modifiers = depressed;
}

#else

/*
 * Without the Wayland backend (a build that has no Wayland library, such as
 * the static Xzed of i386 and PC-98), --wayland fails to start.
 */
int
xzed_wayland_open(
	struct xzed_wayland **out,
	const char *display,
	unsigned width,
	unsigned height,
	const struct xzed_input_handlers *handlers,
	void *context)
{
	/* Nothing can be opened. */
	(void)display;
	(void)width;
	(void)height;
	(void)handlers;
	(void)context;
	*out = NULL;
	errno = ENOTSUP;
	return -1;
}

/*
 * There is no descriptor without the backend.
 */
int
xzed_wayland_fd(
	const struct xzed_wayland *wayland)
{
	/* None. */
	(void)wayland;
	return -1;
}

/*
 * There are no events without the backend.
 */
int
xzed_wayland_dispatch(
	struct xzed_wayland *wayland,
	int readable)
{
	/* A broken connection. */
	(void)wayland;
	(void)readable;
	return -1;
}

/*
 * Nothing is shown without the backend.
 */
int
xzed_wayland_present(
	struct xzed_wayland *wayland,
	const uint32_t *screen,
	int x,
	int y,
	int width,
	int height)
{
	/* Always busy. */
	(void)wayland;
	(void)screen;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	return 1;
}

/*
 * No window is closed without the backend.
 */
int
xzed_wayland_closed(
	const struct xzed_wayland *wayland)
{
	/* Never. */
	(void)wayland;
	return 0;
}

/*
 * Nothing is released without the backend.
 */
void
xzed_wayland_close(
	struct xzed_wayland *wayland)
{
	/* Nothing to release. */
	(void)wayland;
}

#endif
