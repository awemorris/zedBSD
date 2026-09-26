/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Creates the standard fullscreen xdg-shell role before any Vulkan presentation.
 */

#include "wltest.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

static void window_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void window_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void window_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void window_configure(void *data, struct xdg_surface *surface, uint32_t serial);
static void window_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void window_toplevel_close(void *data, struct xdg_toplevel *toplevel);

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

/* The immutable toplevel callbacks update only this application's window state. */
static const struct xdg_toplevel_listener toplevel_listener = {
	window_toplevel_configure, window_toplevel_close
};

/*
 * Initializes the configured native window using only public Wayland calls.
 */
int
wltest_window_open(
	struct wltest_window *window,
	const char *display,
	uint32_t width,
	uint32_t height,
	int fullscreen)
{
	int status;

	/* The compositor may choose dimensions, otherwise this application chooses its own. */
	memset(window, 0, sizeof(*window));
	window->width = width;
	window->height = height;

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

	/* Publish the application identity and the fullscreen preference, unless a window was asked for. */
	xdg_toplevel_set_title(window->toplevel, "Wayland Vulkan test");
	xdg_toplevel_set_app_id(window->toplevel, "wltest");
	if (fullscreen)
		xdg_toplevel_set_fullscreen(window->toplevel, NULL);
	wl_surface_commit(window->surface);

	/* No Vulkan buffer can be presented until the initial configure has been acknowledged. */
	status = wl_display_roundtrip(window->display);
	if (status < 0 || window->configured == 0) {
		errno = EPROTO;
		return -1;
	}

	/* Succeeded: the native surface has an acknowledged fullscreen configuration. */
	return 0;
}

/*
 * Dispatches this application's events without assuming WSI reads the default queue.
 */
int
wltest_window_dispatch(
	struct wltest_window *window)
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

	/* Only read available bytes, keeping animation independent from blocking native waits. */
	descriptor.fd = wl_display_get_fd(window->display);
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	status = poll(&descriptor, 1U, 0);
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
 * Retires roles before native surfaces, then disconnects the owned connection.
 */
void
wltest_window_close(
	struct wltest_window *window)
{
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

/* Negotiates no higher protocol version than this small application uses. */
static void
window_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct wltest_window *window;
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

		/* This announcement has no additional shell interface to bind. */
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
	/* This test does not rebind removed compositor globals during its finite invocation. */
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
	struct wltest_window *window;

	/* The compositor's serial orders this acknowledgment with later buffer commits. */
	window = data;
	xdg_surface_ack_configure(surface, serial);

	/* Setup may create presentable Vulkan buffers after this acknowledged configuration. */
	window->configured = 1;

	/* Succeeded: the native role's complete pending state is acknowledged. */
	return;
}

/* Accepts positive configured dimensions while preserving the application choice for zeros. */
static void
window_toplevel_configure(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height,
	struct wl_array *states)
{
	struct wltest_window *window;

	/* The finite test needs no decoration or interactive window-state handling. */
	(void)toplevel;
	(void)states;
	window = data;

	/* A zero width leaves the application's existing choice in place. */
	if (width > 0)
		window->width = (uint32_t)width;

	/* A zero height independently preserves the application's existing choice. */
	if (height > 0)
		window->height = (uint32_t)height;

	/* Succeeded: each specified extent is ready for the following role configure. */
	return;
}

/* Requests graceful application termination using the compositor's standard close event. */
static void
window_toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct wltest_window *window;

	/* Main owns Vulkan cleanup after observing this event. */
	(void)toplevel;
	window = data;
	window->closed = 1;

	/* Succeeded: the next application iteration will stop submitting images. */
	return;
}
