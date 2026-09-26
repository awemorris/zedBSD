/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Creates the fullscreen xdg-shell window and forwards seat input to the viewer.
 */

#include "mview.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

/* The highest seat version whose events this viewer handles (frame, discrete axis). */
#define WINDOW_SEAT_VERSION	5U

static void window_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void window_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void window_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void window_configure(void *data, struct xdg_surface *surface, uint32_t serial);
static void window_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void window_toplevel_close(void *data, struct xdg_toplevel *toplevel);
static void window_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities);
static void window_seat_name(void *data, struct wl_seat *seat, const char *name);
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
static void window_listeners_fill(void);

/* Immutable registry callbacks remain available until the application's registry is destroyed. */
static const struct wl_registry_listener registry_listener = {
	window_global, window_global_remove
};

/* The shell listener answers compositor liveness requests throughout the connection lifetime. */
static const struct xdg_wm_base_listener shell_listener = {
	window_ping
};

/* The role listener orders configure acknowledgments before later image commits. */
static const struct xdg_surface_listener surface_listener = {
	window_configure
};

/*
 * The toplevel, seat, pointer and keyboard callbacks.
 *
 * They are filled by name before the first bind, so the tables stay correct
 * however many later-version members the protocol header declares; members
 * this viewer does not name stay NULL and belong to versions it never binds.
 */
static struct xdg_toplevel_listener toplevel_listener;

/* See toplevel_listener. */
static struct wl_seat_listener seat_listener;

/* See toplevel_listener. */
static struct wl_pointer_listener pointer_listener;

/* See toplevel_listener. */
static struct wl_keyboard_listener keyboard_listener;

/*
 * Initializes the configured native window using only public Wayland calls.
 *
 * Input events are applied to the given input state while events are
 * dispatched; a compositor without a seat leaves the viewer without input.
 */
int
mview_window_open(
	struct mview_window *window,
	const char *display,
	struct mview_input *input,
	uint32_t width,
	uint32_t height,
	int fullscreen)
{
	int status;

	/* The compositor may choose dimensions, otherwise this application chooses the given size. */
	memset(window, 0, sizeof(*window));
	window->width = width;
	window->height = height;
	window->input = input;

	/* The listener tables must be complete before any object can deliver events. */
	window_listeners_fill();

	/* This window owns the original connection until its renderer and roles have retired. */
	window->display = wl_display_connect(display);
	if (window->display == NULL)
		return -1;

	/* Obtain the registry before attempting to bind any compositor objects. */
	window->registry = wl_display_get_registry(window->display);
	if (window->registry == NULL)
		return -1;

	/* Registry callbacks publish the globals this application can actually use. */
	status = wl_registry_add_listener(window->registry, &registry_listener, window);
	if (status != 0)
		return -1;

	/* Complete initial discovery before inspecting the required native interfaces. */
	status = wl_display_roundtrip(window->display);
	if (status < 0)
		return -1;

	/* A fullscreen xdg surface requires both core surface creation and the standard shell. */
	if (window->compositor == NULL || window->shell == NULL) {
		errno = EOPNOTSUPP;
		return -1;
	}

	/* The core surface owns no attached buffer during its initial configure exchange. */
	window->surface = wl_compositor_create_surface(window->compositor);
	if (window->surface == NULL)
		return -1;

	/* Give the core surface its standard shell role before selecting a toplevel. */
	window->role = xdg_wm_base_get_xdg_surface(window->shell, window->surface);
	if (window->role == NULL)
		return -1;

	/* The role callback acknowledges each complete compositor configure. */
	status = xdg_surface_add_listener(window->role, &surface_listener, window);
	if (status != 0)
		return -1;

	/* The application presents one toplevel without transient windows or decorations. */
	window->toplevel = xdg_surface_get_toplevel(window->role);
	if (window->toplevel == NULL)
		return -1;

	/* Toplevel callbacks retain the chosen extent and the compositor's close request. */
	status = xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);
	if (status != 0)
		return -1;

	/* Publish the application identity and any fullscreen preference before the empty commit. */
	xdg_toplevel_set_title(window->toplevel, "Model viewer");
	xdg_toplevel_set_app_id(window->toplevel, "mview");
	if (fullscreen)
		xdg_toplevel_set_fullscreen(window->toplevel, NULL);
	wl_surface_commit(window->surface);

	/* No Vulkan buffer can be presented until the initial configure has been acknowledged. */
	status = wl_display_roundtrip(window->display);
	if (status < 0 || window->configured == 0) {
		errno = EPROTO;
		return -1;
	}

	/* The initial configure is not a resize. */
	window->resized = 0;

	/* Succeeded: the native surface has an acknowledged fullscreen configuration. */
	return 0;
}

/*
 * Dispatches this application's events without assuming WSI reads the default queue.
 *
 * The timeout, in milliseconds, bounds how long the call waits for new
 * events; zero only takes what has already arrived.
 */
int
mview_window_dispatch(
	struct mview_window *window,
	int timeout)
{
	struct pollfd descriptor;
	int status;

	/* Drain pending callbacks until this application can reserve a read of new events. */
	for (;;) {
		status = wl_display_dispatch_pending(window->display);
		if (status < 0)
			return -1;

		/* Newly queued events may require another dispatch before the read can be prepared. */
		status = wl_display_prepare_read(window->display);
		if (status == 0)
			break;

		/* EAGAIN requests another dispatch; other failures leave no prepared read. */
		if (errno != EAGAIN)
			return -1;
	}

	/* A prepared read is canceled before propagating a request-flush failure. */
	status = wl_display_flush(window->display);
	if (status < 0 && errno != EAGAIN) {
		wl_display_cancel_read(window->display);
		return -1;
	}

	/* Waits at most the given time, so an idle viewer sleeps instead of spinning. */
	descriptor.fd = wl_display_get_fd(window->display);
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	status = poll(&descriptor, 1U, timeout);
	if (status > 0 && (descriptor.revents & POLLIN) != 0) {
		status = wl_display_read_events(window->display);
		if (status < 0)
			return -1;
	} else {
		/* Idle or failed polling still retires this application's prepared-read ownership. */
		wl_display_cancel_read(window->display);
		if (status < 0 && errno != EINTR)
			return -1;

		/* Terminal socket conditions cannot support another native frame. */
		if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			return -1;
	}

	/* Deliver events received above only to this application's default queue. */
	status = wl_display_dispatch_pending(window->display);
	if (status < 0)
		return -1;

	/* Succeeded: pending application events were dispatched without a remaining read reservation. */
	return 0;
}

/*
 * Retires input devices and roles before native surfaces, then disconnects.
 */
void
mview_window_close(
	struct mview_window *window)
{
	/* The pointer is a child of the seat and goes first. */
	if (window->pointer != NULL)
		wl_pointer_destroy(window->pointer);

	/* The keyboard is likewise a child of the seat. */
	if (window->keyboard != NULL)
		wl_keyboard_destroy(window->keyboard);

	/* The seat binding has no remaining device children. */
	if (window->seat != NULL)
		wl_seat_destroy(window->seat);

	/* Partial initialization follows the same child-before-parent retirement order. */
	if (window->toplevel != NULL)
		xdg_toplevel_destroy(window->toplevel);

	/* The shell role no longer has a live toplevel child. */
	if (window->role != NULL)
		xdg_surface_destroy(window->role);

	/* Retire the application-owned mapping only after its shell objects are gone. */
	if (window->surface != NULL)
		wl_surface_destroy(window->surface);

	/* The shell binding has no remaining role objects to keep alive. */
	if (window->shell != NULL)
		xdg_wm_base_destroy(window->shell);

	/* Surface creation is no longer needed once the application's surface has retired. */
	if (window->compositor != NULL)
		wl_compositor_destroy(window->compositor);

	/* Discovery callbacks must disappear before their owning connection closes. */
	if (window->registry != NULL)
		wl_registry_destroy(window->registry);

	/* Disconnect also releases protocol data that could not be sent during shutdown. */
	if (window->display != NULL)
		wl_display_disconnect(window->display);

	/* Clear ownership so a later cleanup cannot reuse stale proxy pointers. */
	memset(window, 0, sizeof(*window));

	/* Succeeded: this window retains no native connection or protocol objects. */
	return;
}

/* Negotiates no higher protocol version than this application uses. */
static void
window_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct mview_window *window;
	int match;
	int status;

	/* Core surface version four supplies the basic fullscreen surface operations. */
	window = data;
	match = strcmp(interface, "wl_compositor");
	if (match == 0 && window->compositor == NULL) {
		/* A newer compositor still supports the version this application understands. */
		if (version > 4U)
			version = 4U;

		/* Discovery retains the first core binding for subsequent surface creation. */
		window->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version);

		/* This announcement has no additional interface to bind. */
		return;
	}

	/* The fullscreen configure protocol requires only the first stable xdg-shell version. */
	match = strcmp(interface, "xdg_wm_base");
	if (match == 0 && version >= 1U && window->shell == NULL) {
		window->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1U);
		if (window->shell == NULL)
			return;

		/* Respond to pings through the same shell binding that creates the role. */
		status = xdg_wm_base_add_listener(window->shell, &shell_listener, window);
		if (status != 0)
			return;

		/* This announcement has no additional interface to bind. */
		return;
	}

	/* The first seat supplies the pointer and keyboard; its absence only disables input. */
	match = strcmp(interface, "wl_seat");
	if (match == 0 && window->seat == NULL) {
		/* Versions above five add events this viewer does not handle. */
		if (version > WINDOW_SEAT_VERSION)
			version = WINDOW_SEAT_VERSION;

		/* The capabilities event, sent on bind, creates the devices. */
		window->seat = wl_registry_bind(registry, name, &wl_seat_interface, version);
		if (window->seat == NULL)
			return;

		window->seat_version = version;
		status = wl_seat_add_listener(window->seat, &seat_listener, window);
		if (status != 0)
			return;
	}

	/* Succeeded: this announcement either supplied a needed binding or needed no action. */
	return;
}

/* Existing bound objects are retired by close or connection failure. */
static void
window_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	/* The viewer does not rebind removed compositor globals during its run. */
	(void)data;
	(void)registry;
	(void)name;

	/* Succeeded: existing ownership remains with ordinary close and connection-failure cleanup. */
	return;
}

/* Responds to the standard compositor liveness request. */
static void
window_ping(
	void *data,
	struct xdg_wm_base *shell,
	uint32_t serial)
{
	/* Echo the exact serial rather than synthesizing a new acknowledgment. */
	(void)data;
	xdg_wm_base_pong(shell, serial);

	/* Succeeded: the original shell binding carries the matching liveness response. */
	return;
}

/* Acknowledges the full pending role state before Vulkan creates presentable buffers. */
static void
window_configure(
	void *data,
	struct xdg_surface *surface,
	uint32_t serial)
{
	struct mview_window *window;

	/* The compositor's serial orders this acknowledgment with later buffer commits. */
	window = data;
	xdg_surface_ack_configure(surface, serial);

	/* Setup may create presentable Vulkan buffers after this acknowledged configuration. */
	window->configured = 1;

	/* Succeeded: the native role's complete pending state is acknowledged. */
	return;
}

/* Accepts positive configured dimensions and notes a change of size. */
static void
window_toplevel_configure(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height,
	struct wl_array *states)
{
	struct mview_window *window;

	/* The viewer needs no decoration or interactive window-state handling. */
	(void)toplevel;
	(void)states;
	window = data;

	/* A zero width leaves the application's existing choice in place. */
	if (width > 0 && (uint32_t)width != window->width) {
		window->width = (uint32_t)width;
		window->resized = 1;
	}

	/* A zero height independently preserves the application's existing choice. */
	if (height > 0 && (uint32_t)height != window->height) {
		window->height = (uint32_t)height;
		window->resized = 1;
	}

	/* Succeeded: each specified extent is ready for the following role configure. */
	return;
}

/* Requests graceful application termination using the compositor's standard close event. */
static void
window_toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct mview_window *window;

	/* Main owns Vulkan cleanup after observing this event. */
	(void)toplevel;
	window = data;
	window->closed = 1;

	/* Succeeded: the next application iteration will stop submitting images. */
	return;
}

/* Creates or retires the pointer and keyboard as the seat's capabilities change. */
static void
window_seat_capabilities(
	void *data,
	struct wl_seat *seat,
	uint32_t capabilities)
{
	struct mview_window *window;
	int status;

	/* A newly offered pointer is created and listened to. */
	window = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0U && window->pointer == NULL) {
		window->pointer = wl_seat_get_pointer(seat);
		if (window->pointer != NULL) {
			status = wl_pointer_add_listener(window->pointer, &pointer_listener, window);
			if (status != 0) {
				wl_pointer_destroy(window->pointer);
				window->pointer = NULL;
			}
		}
	} else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0U && window->pointer != NULL) {
		/* A withdrawn pointer ends any drag it was carrying. */
		wl_pointer_destroy(window->pointer);
		window->pointer = NULL;
		mview_input_release_all(window->input);
	}

	/* A newly offered keyboard is created and listened to. */
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0U && window->keyboard == NULL) {
		window->keyboard = wl_seat_get_keyboard(seat);
		if (window->keyboard != NULL) {
			status = wl_keyboard_add_listener(window->keyboard, &keyboard_listener, window);
			if (status != 0) {
				wl_keyboard_destroy(window->keyboard);
				window->keyboard = NULL;
			}
		}
	} else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0U && window->keyboard != NULL) {
		/* A withdrawn keyboard has nothing left to deliver. */
		wl_keyboard_destroy(window->keyboard);
		window->keyboard = NULL;
	}

	/* Succeeded: the devices match the seat's capabilities. */
	return;
}

/* Ignores the seat's human-readable name. */
static void
window_seat_name(
	void *data,
	struct wl_seat *seat,
	const char *name)
{
	/* The name identifies nothing the viewer uses. */
	(void)data;
	(void)seat;
	(void)name;

	/* Succeeded: nothing to record. */
	return;
}

/* Takes the entry position as the reference for the next drag motion. */
static void
window_pointer_enter(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct mview_window *window;

	/* The viewer has one surface and sets no cursor image. */
	(void)pointer;
	(void)serial;
	(void)surface;
	window = data;

	/* The next motion is measured from the entry point. */
	mview_input_position(window->input, wl_fixed_to_double(x), wl_fixed_to_double(y));

	/* Succeeded: the pointer position is known. */
	return;
}

/* Ends any drag when the pointer leaves, since its release will not arrive. */
static void
window_pointer_leave(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct mview_window *window;

	/* The leave carries nothing else the viewer needs. */
	(void)pointer;
	(void)serial;
	(void)surface;
	window = data;

	/* No button is considered held once focus is gone. */
	mview_input_release_all(window->input);

	/* Succeeded: no drag outlives the pointer focus. */
	return;
}

/* Forwards a pointer motion, which drags when a button is held. */
static void
window_pointer_motion(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct mview_window *window;

	/* The event time is not needed; only positions drive the camera. */
	(void)pointer;
	(void)time;
	window = data;

	/* Applies the movement to the camera. */
	mview_input_motion(window->input, wl_fixed_to_double(x), wl_fixed_to_double(y));

	/* Succeeded: the motion has been applied. */
	return;
}

/* Forwards a button press or release. */
static void
window_pointer_button(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	struct mview_window *window;
	int pressed;

	/* Serial and time identify the event only to the compositor. */
	(void)pointer;
	(void)serial;
	(void)time;
	window = data;

	/* Any state other than pressed is a release. */
	pressed = 0;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		pressed = 1;

	/* Records the button for the next motion. */
	mview_input_button(window->input, button, pressed);

	/* Succeeded: the button state is current. */
	return;
}

/* Forwards a scroll, using the discrete wheel count that preceded it if any. */
static void
window_pointer_axis(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	uint32_t axis,
	wl_fixed_t value)
{
	struct mview_window *window;
	int32_t discrete;

	/* The event time is not needed. */
	(void)pointer;
	(void)time;
	window = data;

	/* A discrete count applies to the next value on its own axis only. */
	discrete = 0;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		discrete = window->discrete;
		window->discrete = 0;
	}

	/* Applies the zoom. */
	mview_input_axis(window->input, axis, wl_fixed_to_double(value), discrete);

	/* Succeeded: the scroll has been applied. */
	return;
}

/* Ends one group of pointer events; each event was applied as it arrived. */
static void
window_pointer_frame(
	void *data,
	struct wl_pointer *pointer)
{
	struct mview_window *window;

	/* A discrete count that no axis value followed is dropped with its frame. */
	(void)pointer;
	window = data;
	window->discrete = 0;

	/* Succeeded: the frame is closed. */
	return;
}

/* Ignores the physical source of a scroll; every source zooms alike. */
static void
window_pointer_axis_source(
	void *data,
	struct wl_pointer *pointer,
	uint32_t source)
{
	/* Wheel, finger and continuous scrolling are treated the same way. */
	(void)data;
	(void)pointer;
	(void)source;

	/* Succeeded: nothing to record. */
	return;
}

/* Ignores the end of a kinetic scroll; zoom has no momentum. */
static void
window_pointer_axis_stop(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	uint32_t axis)
{
	/* No scroll state outlives its axis event. */
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;

	/* Succeeded: nothing to record. */
	return;
}

/* Remembers a vertical wheel click count for the axis value that follows it. */
static void
window_pointer_axis_discrete(
	void *data,
	struct wl_pointer *pointer,
	uint32_t axis,
	int32_t discrete)
{
	struct mview_window *window;

	/* Only the vertical wheel zooms. */
	(void)pointer;
	window = data;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		window->discrete = discrete;

	/* Succeeded: the count waits for its axis value. */
	return;
}

/* Closes the keymap file: keys arrive as evdev codes and need no map. */
static void
window_keyboard_keymap(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t format,
	int32_t fd,
	uint32_t size)
{
	/* The viewer interprets raw key codes whatever the map format. */
	(void)data;
	(void)keyboard;
	(void)format;
	(void)size;

	/* The descriptor belongs to the client and must not leak. */
	if (fd >= 0)
		close(fd);

	/* Succeeded: no keymap resource is held. */
	return;
}

/* Ignores keys already held when focus arrives; only new presses act. */
static void
window_keyboard_enter(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface,
	struct wl_array *keys)
{
	/* A key held across the focus change is not a command. */
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)surface;
	(void)keys;

	/* Succeeded: nothing to record. */
	return;
}

/* Ignores the loss of keyboard focus; keys hold no state in the viewer. */
static void
window_keyboard_leave(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface)
{
	/* Commands act on press, so nothing is left pending. */
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)surface;

	/* Succeeded: nothing to record. */
	return;
}

/* Forwards a key press or release. */
static void
window_keyboard_key(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	struct mview_window *window;
	int pressed;

	/* Serial and time identify the event only to the compositor. */
	(void)keyboard;
	(void)serial;
	(void)time;
	window = data;

	/* Any state other than pressed is a release. */
	pressed = 0;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		pressed = 1;

	/* Applies the key's command. */
	mview_input_key(window->input, key, pressed);

	/* Succeeded: the key has been applied. */
	return;
}

/* Ignores modifier state; no command uses a modifier. */
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
	/* Plus is accepted as '=' or keypad plus, with or without shift. */
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)depressed;
	(void)latched;
	(void)locked;
	(void)group;

	/* Succeeded: nothing to record. */
	return;
}

/* Ignores the repeat rate; a held key acts once. */
static void
window_keyboard_repeat(
	void *data,
	struct wl_keyboard *keyboard,
	int32_t rate,
	int32_t delay)
{
	/* The viewer does not synthesize repeats. */
	(void)data;
	(void)keyboard;
	(void)rate;
	(void)delay;

	/* Succeeded: nothing to record. */
	return;
}

/* Fills the toplevel, seat, pointer and keyboard listener tables by member name. */
static void
window_listeners_fill(
	void)
{
	/* The first xdg-shell version sends only configure and close to a toplevel. */
	memset(&toplevel_listener, 0, sizeof(toplevel_listener));
	toplevel_listener.configure = window_toplevel_configure;
	toplevel_listener.close = window_toplevel_close;

	/* The seat reports its devices and, from version 2, its name. */
	memset(&seat_listener, 0, sizeof(seat_listener));
	seat_listener.capabilities = window_seat_capabilities;
	seat_listener.name = window_seat_name;

	/* The pointer events of versions 1 through 5. */
	memset(&pointer_listener, 0, sizeof(pointer_listener));
	pointer_listener.enter = window_pointer_enter;
	pointer_listener.leave = window_pointer_leave;
	pointer_listener.motion = window_pointer_motion;
	pointer_listener.button = window_pointer_button;
	pointer_listener.axis = window_pointer_axis;
	pointer_listener.frame = window_pointer_frame;
	pointer_listener.axis_source = window_pointer_axis_source;
	pointer_listener.axis_stop = window_pointer_axis_stop;
	pointer_listener.axis_discrete = window_pointer_axis_discrete;

	/* The keyboard events of versions 1 through 5. */
	memset(&keyboard_listener, 0, sizeof(keyboard_listener));
	keyboard_listener.keymap = window_keyboard_keymap;
	keyboard_listener.enter = window_keyboard_enter;
	keyboard_listener.leave = window_keyboard_leave;
	keyboard_listener.key = window_keyboard_key;
	keyboard_listener.modifiers = window_keyboard_modifiers;
	keyboard_listener.repeat_info = window_keyboard_repeat;

	/* Succeeded: every event of the bound versions has a callback. */
	return;
}
