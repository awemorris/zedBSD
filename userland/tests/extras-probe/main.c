/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests the compositor's xdg-decoration, cursor-shape and viewporter (WS035 p080)
 * the way a toolkit uses them: with protocol code of its own (the
 * interface descriptions here, as wayland-scanner would make them) over
 * libwayland's generic marshalling and dispatch.
 *
 * The window (dark, 400x300) asks for client-side decorations and is told
 * server-side.  Its sub-surface's buffer has four 50x50 quarters (red, green,
 * blue, white); a viewport shows only the red quarter, stretched to 200x100,
 * at (20,20).  When the pointer enters the window the cursor becomes the
 * text shape; keys h and e ask for the pointing hand and the left-right
 * resize arrow.  Every event is one line: EXTRAS <what> ...
 *
 * With --body-viewport (WS035 p081) the window itself has a viewport too:
 * its 400x300 buffer is four quarters, and the viewport shows its right
 * half (green above white) at 600x300.
 *
 *   extras-probe [--timeout-s=N] [--token=NAME] [--body-viewport]
 */

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Marks a parameter a listener must take but does not use. */
#define UNUSED_PARAMETER(x)	(void)(x)

/* The window's size and colour, and the sub-surface's buffer (four quarters). */
#define PROBE_WIDTH		400
#define PROBE_HEIGHT		300
#define PROBE_COLOR		0xff2b3444U
#define PROBE_QUARTER		50

/* The keys e and h (evdev codes). */
#define PROBE_KEY_E		18U
#define PROBE_KEY_H		35U

/* The decoration modes, and the cursor shapes the probe asks for. */
#define MODE_CLIENT_SIDE	1U
#define SHAPE_POINTER		4U
#define SHAPE_TEXT		9U
#define SHAPE_EW_RESIZE		26U

extern const struct wl_interface probe_decoration_manager_interface;
extern const struct wl_interface probe_decoration_interface;
extern const struct wl_interface probe_shape_manager_interface;
extern const struct wl_interface probe_shape_device_interface;
extern const struct wl_interface probe_viewporter_interface;
extern const struct wl_interface probe_viewport_interface;

/* The arguments of messages that name no interface (at most four). */
static const struct wl_interface *probe_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* zxdg_decoration_manager_v1.get_toplevel_decoration: the new decoration and the toplevel. */
static const struct wl_interface *decoration_manager_get_types[] = {
	&probe_decoration_interface,
	&xdg_toplevel_interface,
};

/* The requests of zxdg_decoration_manager_v1. */
static const struct wl_message decoration_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_toplevel_decoration", "no", decoration_manager_get_types },
};

/* zxdg_decoration_manager_v1, as the probe describes it. */
const struct wl_interface probe_decoration_manager_interface = {
	"zxdg_decoration_manager_v1", 1, 2, decoration_manager_requests,
	0, NULL
};

/* The requests of zxdg_toplevel_decoration_v1. */
static const struct wl_message decoration_requests[] = {
	{ "destroy", "", NULL },
	{ "set_mode", "u", probe_plain_types },
	{ "unset_mode", "", NULL },
};

/* The events of zxdg_toplevel_decoration_v1. */
static const struct wl_message decoration_events[] = {
	{ "configure", "u", probe_plain_types },
};

/* zxdg_toplevel_decoration_v1, as the probe describes it. */
const struct wl_interface probe_decoration_interface = {
	"zxdg_toplevel_decoration_v1", 1, 3, decoration_requests,
	1, decoration_events
};

/* wp_cursor_shape_manager_v1.get_pointer: the new device and the pointer. */
static const struct wl_interface *shape_manager_pointer_types[] = {
	&probe_shape_device_interface,
	&wl_pointer_interface,
};

/* The requests of wp_cursor_shape_manager_v1 (get_tablet_tool_v2 is left out). */
static const struct wl_message shape_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_pointer", "no", shape_manager_pointer_types },
};

/* wp_cursor_shape_manager_v1, as the probe describes it. */
const struct wl_interface probe_shape_manager_interface = {
	"wp_cursor_shape_manager_v1", 1, 2, shape_manager_requests,
	0, NULL
};

/* The requests of wp_cursor_shape_device_v1. */
static const struct wl_message shape_device_requests[] = {
	{ "destroy", "", NULL },
	{ "set_shape", "uu", probe_plain_types },
};

/* wp_cursor_shape_device_v1, as the probe describes it. */
const struct wl_interface probe_shape_device_interface = {
	"wp_cursor_shape_device_v1", 1, 2, shape_device_requests,
	0, NULL
};

/* wp_viewporter.get_viewport: the new viewport and the surface. */
static const struct wl_interface *viewporter_get_types[] = {
	&probe_viewport_interface,
	&wl_surface_interface,
};

/* The requests of wp_viewporter. */
static const struct wl_message viewporter_requests[] = {
	{ "destroy", "", NULL },
	{ "get_viewport", "no", viewporter_get_types },
};

/* wp_viewporter, as the probe describes it. */
const struct wl_interface probe_viewporter_interface = {
	"wp_viewporter", 1, 2, viewporter_requests,
	0, NULL
};

/* The requests of wp_viewport. */
static const struct wl_message viewport_requests[] = {
	{ "destroy", "", NULL },
	{ "set_source", "ffff", probe_plain_types },
	{ "set_destination", "ii", probe_plain_types },
};

/* wp_viewport, as the probe describes it. */
const struct wl_interface probe_viewport_interface = {
	"wp_viewport", 1, 3, viewport_requests,
	0, NULL
};

/* The listener of zxdg_toplevel_decoration_v1 (called through the generic dispatch). */
struct probe_decoration_listener {
	void (*configure)(void *data, struct wl_proxy *decoration, uint32_t mode);
};

/*
 * The probe's connection, its globals and the extra managers, its window
 * with its decoration and its sub-surface with its viewport, the pointer's
 * cursor-shape device and the serial of the pointer's enter.
 */
struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_proxy *decoration_manager;
	struct wl_proxy *shape_manager;
	struct wl_proxy *viewporter;
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	struct wl_proxy *decoration;
	struct wl_surface *child;
	struct wl_subsurface *subsurface;
	struct wl_proxy *viewport;
	struct wl_proxy *body_viewport;
	struct wl_proxy *shape_device;
	uint32_t enter_serial;
	int configured;
	int closed;
	int viewport_body;
	const char *token;
};

static int probe_options(int count, char **arguments, struct probe *probe, unsigned *timeout);
static int probe_connect(struct probe *probe);
static int probe_child(struct probe *probe);
static int probe_body_viewport(struct probe *probe);
static int probe_draw(struct probe *probe, struct wl_surface *surface, int32_t width, int32_t height, int quarters);
static void probe_set_shape(struct probe *probe, uint32_t shape);
static uint64_t probe_clock(void);
static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name);
static void shell_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void role_configure(void *data, struct xdg_surface *role, uint32_t serial);
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void toplevel_close(void *data, struct xdg_toplevel *toplevel);
static void decoration_configure(void *data, struct wl_proxy *decoration, uint32_t mode);
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities);
static void seat_name(void *data, struct wl_seat *seat, const char *name);
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y);
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface);
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value);
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size);
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys);
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface);
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
static void keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay);

/* The registry's callbacks. */
static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_remove
};

/* The shell's ping. */
static const struct xdg_wm_base_listener shell_listener = {
	shell_ping
};

/* The window's xdg_surface. */
static const struct xdg_surface_listener role_listener = {
	role_configure
};

/* The window's toplevel. */
static const struct xdg_toplevel_listener toplevel_listener = {
	toplevel_configure,
	toplevel_close,
	NULL
};

/* The window's decoration. */
static const struct probe_decoration_listener decoration_listener = {
	decoration_configure
};

/* The seat's. */
static const struct wl_seat_listener seat_listener = {
	seat_capabilities,
	seat_name
};

/* The pointer's (the version 5 events are not used). */
static const struct wl_pointer_listener pointer_listener = {
	pointer_enter,
	pointer_leave,
	pointer_motion,
	pointer_button,
	pointer_axis,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

/* The keyboard's. */
static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_keymap,
	keyboard_enter,
	keyboard_leave,
	keyboard_key,
	keyboard_modifiers,
	keyboard_repeat
};

/*
 * Shows the window and answers the input until the timeout or the close.
 */
int
main(
	int count,
	char **arguments)
{
	struct probe probe;
	struct pollfd descriptor;
	uint64_t deadline;
	uint64_t now;
	unsigned timeout;
	int error;
	int ready;

	/* The options. */
	memset(&probe, 0, sizeof(probe));
	error = probe_options(count, arguments, &probe, &timeout);
	if (error != 0) {
		fprintf(stderr, "usage: extras-probe [--timeout-s=N] [--token=NAME] [--body-viewport]\n");
		return 2;
	}

	/* The connection and the window. */
	error = probe_connect(&probe);
	if (error != 0) {
		printf("EXTRAS FAILED run=%s setup errno=%d\n", probe.token, error);
		return 1;
	}

	/* The events, until the window is closed or the time is up. */
	deadline = probe_clock() + (uint64_t)timeout * 1000U;
	while (!probe.closed) {
		/* The time is up. */
		now = probe_clock();
		if (now >= deadline)
			break;

		/* The requests go out, then the events are waited for a moment. */
		(void)wl_display_flush(probe.display);
		descriptor.fd = wl_display_get_fd(probe.display);
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		ready = poll(&descriptor, 1, 100);
		if (ready <= 0)
			continue;

		/* The events that came. */
		error = wl_display_dispatch(probe.display);
		if (error < 0) {
			printf("EXTRAS FAILED run=%s dispatch errno=%d\n", probe.token, errno);
			return 1;
		}
	}

	/* The end. */
	printf("EXTRAS DONE run=%s\n", probe.token);
	fflush(stdout);
	wl_display_disconnect(probe.display);

	/* Succeeded: the probe ran to its end. */
	return 0;
}

/* Reads the options: --timeout-s=N (default 120), --token=NAME and --body-viewport. */
static int
probe_options(
	int count,
	char **arguments,
	struct probe *probe,
	unsigned *timeout)
{
	char *end;
	unsigned long value;
	int index;
	int same;

	/* The defaults. */
	probe->token = "probe";
	*timeout = 120U;

	/* Each option. */
	for (index = 1; index < count; index++) {
		/* The name in the log. */
		same = strncmp(arguments[index], "--token=", 8);
		if (same == 0) {
			probe->token = arguments[index] + 8;
			continue;
		}

		/* A viewport on the window itself. */
		same = strcmp(arguments[index], "--body-viewport");
		if (same == 0) {
			probe->viewport_body = 1;
			continue;
		}

		/* The timeout. */
		same = strncmp(arguments[index], "--timeout-s=", 12);
		if (same != 0)
			return -1;
		value = strtoul(arguments[index] + 12, &end, 10);
		if (*end != '\0' || value == 0UL || value > 3600UL)
			return -1;
		*timeout = (unsigned)value;
	}

	/* Succeeded: the options are read. */
	return 0;
}

/* Connects, binds the globals and shows the window with its decoration and its sub-surface. */
static int
probe_connect(
	struct probe *probe)
{
	union wl_argument arguments[2];
	int status;

	/* The connection and its globals. */
	probe->display = wl_display_connect(NULL);
	if (probe->display == NULL)
		return EIO;
	probe->registry = wl_display_get_registry(probe->display);
	wl_registry_add_listener(probe->registry, &registry_listener, probe);
	status = wl_display_roundtrip(probe->display);
	if (status < 0)
		return EIO;

	/* The probe needs every global it tests. */
	if (probe->compositor == NULL || probe->subcompositor == NULL || probe->shm == NULL || probe->shell == NULL)
		return EOPNOTSUPP;
	if (probe->seat == NULL || probe->decoration_manager == NULL || probe->shape_manager == NULL || probe->viewporter == NULL)
		return EOPNOTSUPP;

	/* The window: a toplevel. */
	probe->surface = wl_compositor_create_surface(probe->compositor);
	probe->role = xdg_wm_base_get_xdg_surface(probe->shell, probe->surface);
	xdg_surface_add_listener(probe->role, &role_listener, probe);
	probe->toplevel = xdg_surface_get_toplevel(probe->role);
	xdg_toplevel_add_listener(probe->toplevel, &toplevel_listener, probe);
	xdg_toplevel_set_title(probe->toplevel, "extras probe");

	/* Its decoration, asking to draw its own. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)probe->toplevel;
	probe->decoration = wl_proxy_marshal_array_flags(probe->decoration_manager, 1U, &probe_decoration_interface, 1U, 0, arguments);
	if (probe->decoration == NULL)
		return ENOMEM;
	wl_proxy_add_listener(probe->decoration, (void (**)(void))&decoration_listener, probe);
	arguments[0].u = MODE_CLIENT_SIDE;
	wl_proxy_marshal_array_flags(probe->decoration, 1U, NULL, 0, 0, arguments);
	wl_surface_commit(probe->surface);

	/* The first configure. */
	status = wl_display_roundtrip(probe->display);
	if (status < 0 || !probe->configured)
		return EPROTO;

	/* The sub-surface with its viewport (cached until the window's image). */
	status = probe_child(probe);
	if (status != 0)
		return status;

	/* The window's image shows it all, or with --body-viewport a part of its quarters, larger. */
	if (probe->viewport_body) {
		status = probe_body_viewport(probe);
	} else {
		status = probe_draw(probe, probe->surface, PROBE_WIDTH, PROBE_HEIGHT, 0);
	}

	/* Reports a window that could not be shown. */
	if (status != 0)
		return status;

	/* Succeeded: the log line the test waits for. */
	printf("EXTRAS ready run=%s\n", probe->token);
	fflush(stdout);
	return 0;
}

/* Makes the sub-surface: four quarters, a viewport showing the red one at 200x100. */
static int
probe_child(
	struct probe *probe)
{
	union wl_argument arguments[4];
	int status;

	/* The sub-surface at (20,20) of the window. */
	probe->child = wl_compositor_create_surface(probe->compositor);
	probe->subsurface = wl_subcompositor_get_subsurface(probe->subcompositor, probe->child, probe->surface);
	wl_subsurface_set_position(probe->subsurface, 20, 20);

	/* Its viewport: the top-left quarter as the source (24.8 fixed point), 200x100 as the destination. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)probe->child;
	probe->viewport = wl_proxy_marshal_array_flags(probe->viewporter, 1U, &probe_viewport_interface, 1U, 0, arguments);
	if (probe->viewport == NULL)
		return ENOMEM;
	arguments[0].f = 0;
	arguments[1].f = 0;
	arguments[2].f = PROBE_QUARTER * 256;
	arguments[3].f = PROBE_QUARTER * 256;
	wl_proxy_marshal_array_flags(probe->viewport, 1U, NULL, 0, 0, arguments);
	arguments[0].i = 200;
	arguments[1].i = 100;
	wl_proxy_marshal_array_flags(probe->viewport, 2U, NULL, 0, 0, arguments);

	/* Its image. */
	status = probe_draw(probe, probe->child, PROBE_QUARTER * 2, PROBE_QUARTER * 2, 1);
	if (status != 0)
		return status;

	/* Succeeded: the sub-surface waits for the window's commit. */
	printf("EXTRAS viewport source=0,0,%d,%d destination=200,100\n", PROBE_QUARTER, PROBE_QUARTER);
	fflush(stdout);
	return 0;
}

/* Gives the window a viewport (its buffer's right half at 600x300) and its image of four quarters. */
static int
probe_body_viewport(
	struct probe *probe)
{
	union wl_argument arguments[4];
	int status;

	/* The window's viewport. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)probe->surface;
	probe->body_viewport = wl_proxy_marshal_array_flags(probe->viewporter, 1U, &probe_viewport_interface, 1U, 0, arguments);
	if (probe->body_viewport == NULL)
		return ENOMEM;

	/* The right half of the buffer (24.8 fixed point), shown at 600x300. */
	arguments[0].f = (PROBE_WIDTH / 2) * 256;
	arguments[1].f = 0;
	arguments[2].f = (PROBE_WIDTH / 2) * 256;
	arguments[3].f = PROBE_HEIGHT * 256;
	wl_proxy_marshal_array_flags(probe->body_viewport, 1U, NULL, 0, 0, arguments);
	arguments[0].i = 600;
	arguments[1].i = PROBE_HEIGHT;
	wl_proxy_marshal_array_flags(probe->body_viewport, 2U, NULL, 0, 0, arguments);

	/* The image, committed with the viewport. */
	status = probe_draw(probe, probe->surface, PROBE_WIDTH, PROBE_HEIGHT, 1);
	if (status != 0)
		return status;

	/* Succeeded: the log line of the window's viewport. */
	printf("EXTRAS body-viewport source=%d,0,%d,%d destination=600,%d\n", PROBE_WIDTH / 2, PROBE_WIDTH / 2, PROBE_HEIGHT, PROBE_HEIGHT);
	fflush(stdout);
	return 0;
}

/* Draws a surface into a new wl_shm buffer (one colour, or four quarters) and commits it. */
static int
probe_draw(
	struct probe *probe,
	struct wl_surface *surface,
	int32_t width,
	int32_t height,
	int quarters)
{
	static const uint32_t colors[4] = { 0xffe04040U, 0xff40c060U, 0xff4060e0U, 0xffffffffU };
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	char name[64];
	size_t bytes;
	void *map;
	int32_t x;
	int32_t y;
	int quarter;
	int error;
	int fd;

	/* Anonymous shared memory of the image's size. */
	bytes = (size_t)width * (size_t)height * 4U;
	(void)snprintf(name, sizeof(name), "/extras-probe-%ld-%d", (long)getpid(), quarters);
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return errno;
	(void)shm_unlink(name);
	error = ftruncate(fd, (off_t)bytes);
	if (error != 0) {
		close(fd);
		return EIO;
	}

	/* Its mapping. */
	map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return ENOMEM;
	}

	/* Every pixel: the window's colour, or the quarter's. */
	pixels = map;
	for (y = 0; y < height; y++) {
		/* One row. */
		for (x = 0; x < width; x++) {
			/* The quarter the pixel is in (left to right, top to bottom). */
			quarter = 0;
			if (x >= width / 2)
				quarter += 1;
			if (y >= height / 2)
				quarter += 2;
			pixels[y * width + x] = PROBE_COLOR;
			if (quarters)
				pixels[y * width + x] = colors[quarter];
		}
	}

	/* The buffer, attached and committed. */
	pool = wl_shm_create_pool(probe->shm, fd, (int32_t)bytes);
	close(fd);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);

	/* Succeeded: the surface is shown (its buffer stays for the probe's life). */
	return 0;
}

/* Asks for a cursor shape (with the serial of the pointer's enter). */
static void
probe_set_shape(
	struct probe *probe,
	uint32_t shape)
{
	union wl_argument arguments[2];

	/* No device yet. */
	if (probe->shape_device == NULL)
		return;

	/* The request. */
	arguments[0].u = probe->enter_serial;
	arguments[1].u = shape;
	wl_proxy_marshal_array_flags(probe->shape_device, 1U, NULL, 0, 0, arguments);
	printf("EXTRAS shape %u\n", shape);
	fflush(stdout);
}

/* Returns a monotonic time in milliseconds. */
static uint64_t
probe_clock(void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the time in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Binds the globals the probe uses. */
static void
registry_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct probe *probe;
	int same;

	UNUSED_PARAMETER(version);

	/* The core globals. */
	probe = data;
	same = strcmp(interface, "wl_compositor");
	if (same == 0)
		probe->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4U);
	same = strcmp(interface, "wl_subcompositor");
	if (same == 0)
		probe->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1U);
	same = strcmp(interface, "wl_shm");
	if (same == 0)
		probe->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1U);

	/* The shell and the seat. */
	same = strcmp(interface, "xdg_wm_base");
	if (same == 0) {
		probe->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1U);
		xdg_wm_base_add_listener(probe->shell, &shell_listener, probe);
	}

	/* The seat. */
	same = strcmp(interface, "wl_seat");
	if (same == 0) {
		probe->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5U);
		wl_seat_add_listener(probe->seat, &seat_listener, probe);
	}

	/* The three managers the probe tests. */
	same = strcmp(interface, "zxdg_decoration_manager_v1");
	if (same == 0)
		probe->decoration_manager = wl_registry_bind(registry, name, &probe_decoration_manager_interface, 1U);
	same = strcmp(interface, "wp_cursor_shape_manager_v1");
	if (same == 0)
		probe->shape_manager = wl_registry_bind(registry, name, &probe_shape_manager_interface, 1U);
	same = strcmp(interface, "wp_viewporter");
	if (same == 0)
		probe->viewporter = wl_registry_bind(registry, name, &probe_viewporter_interface, 1U);
}

/* No global goes while the probe runs. */
static void
registry_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(registry);
	UNUSED_PARAMETER(name);
}

/* Answers the shell's ping. */
static void
shell_ping(
	void *data,
	struct xdg_wm_base *shell,
	uint32_t serial)
{
	UNUSED_PARAMETER(data);

	/* The answer. */
	xdg_wm_base_pong(shell, serial);
}

/* Acknowledges the window's configure. */
static void
role_configure(
	void *data,
	struct xdg_surface *role,
	uint32_t serial)
{
	struct probe *probe;

	/* The acknowledgement. */
	probe = data;
	xdg_surface_ack_configure(role, serial);
	probe->configured = 1;
}

/* The window keeps its own size. */
static void
toplevel_configure(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height,
	struct wl_array *states)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(toplevel);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(states);
}

/* Ends the probe when the window is closed. */
static void
toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct probe *probe;

	UNUSED_PARAMETER(toplevel);

	/* The main loop ends. */
	probe = data;
	probe->closed = 1;
}

/* Logs the decoration mode the compositor answers. */
static void
decoration_configure(
	void *data,
	struct wl_proxy *decoration,
	uint32_t mode)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(decoration);

	/* The log line. */
	printf("EXTRAS decoration mode=%u\n", mode);
	fflush(stdout);
}

/* Takes the pointer (with its cursor-shape device) and the keyboard. */
static void
seat_capabilities(
	void *data,
	struct wl_seat *seat,
	uint32_t capabilities)
{
	union wl_argument arguments[2];
	struct probe *probe;

	/* The pointer and its device. */
	probe = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0U && probe->pointer == NULL) {
		probe->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(probe->pointer, &pointer_listener, probe);
		arguments[0].n = 0;
		arguments[1].o = (struct wl_object *)probe->pointer;
		probe->shape_device = wl_proxy_marshal_array_flags(probe->shape_manager, 1U, &probe_shape_device_interface, 1U, 0, arguments);
	}

	/* The keyboard. */
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0U && probe->keyboard == NULL) {
		probe->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(probe->keyboard, &keyboard_listener, probe);
	}
}

/* The seat's name is not used. */
static void
seat_name(
	void *data,
	struct wl_seat *seat,
	const char *name)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(seat);
	UNUSED_PARAMETER(name);
}

/* The pointer entered: the cursor becomes the text shape. */
static void
pointer_enter(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct probe *probe;

	UNUSED_PARAMETER(pointer);
	UNUSED_PARAMETER(surface);

	/* The serial names the shape's request. */
	probe = data;
	probe->enter_serial = serial;
	printf("EXTRAS enter x=%d y=%d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
	probe_set_shape(probe, SHAPE_TEXT);
}

/* The pointer left. */
static void
pointer_leave(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(pointer);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(surface);
}

/* The motion is not used. */
static void
pointer_motion(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	wl_fixed_t x,
	wl_fixed_t y)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(pointer);
	UNUSED_PARAMETER(time);
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
}

/* The buttons are not used. */
static void
pointer_button(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(pointer);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(time);
	UNUSED_PARAMETER(button);
	UNUSED_PARAMETER(state);
}

/* Scrolling is not used. */
static void
pointer_axis(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	uint32_t axis,
	wl_fixed_t value)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(pointer);
	UNUSED_PARAMETER(time);
	UNUSED_PARAMETER(axis);
	UNUSED_PARAMETER(value);
}

/* The keymap is not used (its descriptor is closed). */
static void
keyboard_keymap(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t format,
	int32_t fd,
	uint32_t size)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(format);
	UNUSED_PARAMETER(size);

	/* The descriptor is not read. */
	if (fd >= 0)
		close(fd);
}

/* The keyboard's focus is not used. */
static void
keyboard_enter(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface,
	struct wl_array *keys)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(surface);
	UNUSED_PARAMETER(keys);
}

/* The keyboard leaving is not used. */
static void
keyboard_leave(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(surface);
}

/* Keys h and e ask for the pointing hand and the left-right arrow. */
static void
keyboard_key(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	struct probe *probe;

	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(time);

	/* Only a press acts. */
	probe = data;
	if (state == 0U)
		return;

	/* The shape by the key. */
	if (key == PROBE_KEY_H)
		probe_set_shape(probe, SHAPE_POINTER);
	if (key == PROBE_KEY_E)
		probe_set_shape(probe, SHAPE_EW_RESIZE);
}

/* The modifiers are not used. */
static void
keyboard_modifiers(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t depressed,
	uint32_t latched,
	uint32_t locked,
	uint32_t group)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(depressed);
	UNUSED_PARAMETER(latched);
	UNUSED_PARAMETER(locked);
	UNUSED_PARAMETER(group);
}

/* The repeat is not used. */
static void
keyboard_repeat(
	void *data,
	struct wl_keyboard *keyboard,
	int32_t rate,
	int32_t delay)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(rate);
	UNUSED_PARAMETER(delay);
}
