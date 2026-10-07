/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests the compositor's wl_subcompositor and wl_subsurface (WS035 p077).
 *
 * The window (dark, 400x300, wl_shm) has three sub-surfaces: a (red,
 * 100x80) above it at (20,20), c (yellow, 40x40) a child of a at (70,50),
 * and b (green, 100x80) below the window at (-40,200), so only the part of
 * b left of the window shows.  All start synchronized and are shown by the
 * window's first image.
 *
 * Keys (the window has the keyboard):
 *   s  a moves to (200,20) and commits a magenta image: nothing changes
 *      until the window commits (a is synchronized)
 *   c  the window commits: a moves and turns magenta, c goes with it
 *   d  b is desynchronized and commits a blue image: it shows at once
 *   o  b is placed above the window
 *   x  a's wl_subsurface is destroyed: a and c are not shown any more
 * Every event is one line: SUBPROBE <what> ..., with the surface named
 * window, a, b or c.
 *
 *   subsurface-probe [--timeout-s=N] [--token=NAME]
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

/* The surfaces' colours. */
#define PROBE_WINDOW_COLOR	0xff2b3444U
#define PROBE_A_COLOR		0xffe04040U
#define PROBE_A2_COLOR		0xffc040c0U
#define PROBE_B_COLOR		0xff40c060U
#define PROBE_B2_COLOR		0xff4060e0U
#define PROBE_C_COLOR		0xffe0e040U

/* The keys c, d, o, s and x (evdev codes). */
#define PROBE_KEY_C		46U
#define PROBE_KEY_D		32U
#define PROBE_KEY_O		24U
#define PROBE_KEY_S		31U
#define PROBE_KEY_X		45U

/* The surfaces, by index. */
#define PROBE_WINDOW		0U
#define PROBE_A			1U
#define PROBE_B			2U
#define PROBE_C			3U
#define PROBE_SURFACES		4U

/*
 * One surface the probe draws, with its sub-surface role (none for the
 * window), its wl_shm buffer and mapping, its size and its name.
 */
struct probe_surface {
	struct wl_surface *surface;
	struct wl_subsurface *subsurface;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	size_t mapped;
	int32_t width;
	int32_t height;
	const char *name;
};

/*
 * The probe's connection, its globals, its window's roles, its four
 * surfaces, and whether the window was configured or closed.
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
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	struct probe_surface surfaces[PROBE_SURFACES];
	int configured;
	int closed;
	const char *token;
};

static int probe_options(int count, char **arguments, struct probe *probe, unsigned *timeout);
static int probe_connect(struct probe *probe);
static int probe_children(struct probe *probe);
static int probe_draw(struct probe *probe, struct probe_surface *surface, uint32_t color);
static void probe_release(struct probe_surface *surface);
static int probe_shared_memory(size_t size, void **map);
static void probe_key(struct probe *probe, uint32_t key);
static const char *probe_name(struct probe *probe, struct wl_surface *surface);
static uint64_t probe_clock(void);
static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name);
static void shell_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void role_configure(void *data, struct xdg_surface *role, uint32_t serial);
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void toplevel_close(void *data, struct xdg_toplevel *toplevel);
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
	NULL
};

/*
 * Shows the window and its sub-surfaces and answers the input until the
 * timeout or the close.
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
		fprintf(stderr, "usage: subsurface-probe [--timeout-s=N] [--token=NAME]\n");
		return 2;
	}

	/* The connection, the window and its sub-surfaces. */
	error = probe_connect(&probe);
	if (error != 0) {
		printf("SUBPROBE FAILED run=%s setup errno=%d\n", probe.token, error);
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
			printf("SUBPROBE FAILED run=%s dispatch errno=%d\n", probe.token, errno);
			return 1;
		}
	}

	/* The end. */
	printf("SUBPROBE DONE run=%s\n", probe.token);
	fflush(stdout);
	wl_display_disconnect(probe.display);

	/* Succeeded: the probe ran to its end. */
	return 0;
}

/* Reads the options: --timeout-s=N (default 120) and --token=NAME. */
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

/* Connects, binds the globals and shows the window with its sub-surfaces. */
static int
probe_connect(
	struct probe *probe)
{
	struct probe_surface *window;
	int status;

	/* The connection. */
	probe->display = wl_display_connect(NULL);
	if (probe->display == NULL)
		return EIO;

	/* Its globals. */
	probe->registry = wl_display_get_registry(probe->display);
	wl_registry_add_listener(probe->registry, &registry_listener, probe);
	status = wl_display_roundtrip(probe->display);
	if (status < 0)
		return EIO;

	/* The probe needs the compositor, the subcompositor, wl_shm, the shell and a seat. */
	if (probe->compositor == NULL || probe->subcompositor == NULL)
		return EOPNOTSUPP;
	if (probe->shm == NULL || probe->shell == NULL || probe->seat == NULL)
		return EOPNOTSUPP;

	/* The window: a toplevel, configured before it is drawn. */
	window = &probe->surfaces[PROBE_WINDOW];
	window->name = "window";
	window->width = 400;
	window->height = 300;
	window->surface = wl_compositor_create_surface(probe->compositor);
	probe->role = xdg_wm_base_get_xdg_surface(probe->shell, window->surface);
	xdg_surface_add_listener(probe->role, &role_listener, probe);
	probe->toplevel = xdg_surface_get_toplevel(probe->role);
	xdg_toplevel_add_listener(probe->toplevel, &toplevel_listener, probe);
	xdg_toplevel_set_title(probe->toplevel, "subsurface probe");
	wl_surface_commit(window->surface);

	/* The first configure. */
	status = wl_display_roundtrip(probe->display);
	if (status < 0 || !probe->configured)
		return EPROTO;

	/* The sub-surfaces, whose commits wait for the window's. */
	status = probe_children(probe);
	if (status != 0)
		return status;

	/* The window's image shows them all. */
	status = probe_draw(probe, window, PROBE_WINDOW_COLOR);
	if (status != 0)
		return status;

	/* Succeeded: the log line the test waits for. */
	printf("SUBPROBE ready run=%s\n", probe->token);
	fflush(stdout);
	return 0;
}

/* Makes the sub-surfaces a (above), c (a's child) and b (below the window), each with its image committed. */
static int
probe_children(
	struct probe *probe)
{
	struct probe_surface *window;
	struct probe_surface *a;
	struct probe_surface *b;
	struct probe_surface *c;
	int status;

	/* The surfaces and their roles. */
	window = &probe->surfaces[PROBE_WINDOW];
	a = &probe->surfaces[PROBE_A];
	b = &probe->surfaces[PROBE_B];
	c = &probe->surfaces[PROBE_C];
	a->name = "a";
	a->width = 100;
	a->height = 80;
	b->name = "b";
	b->width = 100;
	b->height = 80;
	c->name = "c";
	c->width = 40;
	c->height = 40;
	a->surface = wl_compositor_create_surface(probe->compositor);
	b->surface = wl_compositor_create_surface(probe->compositor);
	c->surface = wl_compositor_create_surface(probe->compositor);
	a->subsurface = wl_subcompositor_get_subsurface(probe->subcompositor, a->surface, window->surface);
	b->subsurface = wl_subcompositor_get_subsurface(probe->subcompositor, b->surface, window->surface);
	c->subsurface = wl_subcompositor_get_subsurface(probe->subcompositor, c->surface, a->surface);

	/* Their places: a over the window, c over a's corner, b below the window, sticking out on the left. */
	wl_subsurface_set_position(a->subsurface, 20, 20);
	wl_subsurface_set_position(b->subsurface, -40, 200);
	wl_subsurface_set_position(c->subsurface, 70, 50);
	wl_subsurface_place_below(b->subsurface, window->surface);

	/* Their images (cached until the window commits). */
	status = probe_draw(probe, c, PROBE_C_COLOR);
	if (status != 0)
		return status;
	status = probe_draw(probe, a, PROBE_A_COLOR);
	if (status != 0)
		return status;
	status = probe_draw(probe, b, PROBE_B_COLOR);
	if (status != 0)
		return status;

	/* Succeeded: the sub-surfaces wait for the window's image. */
	return 0;
}

/* Draws a surface in one colour into a new wl_shm buffer and commits it. */
static int
probe_draw(
	struct probe *probe,
	struct probe_surface *surface,
	uint32_t color)
{
	struct wl_shm_pool *pool;
	size_t bytes;
	size_t index;
	void *map;
	int fd;

	/* The last image goes. */
	probe_release(surface);

	/* The buffer's memory. */
	bytes = (size_t)surface->width * (size_t)surface->height * 4U;
	fd = probe_shared_memory(bytes, &map);
	if (fd < 0)
		return errno;
	pool = wl_shm_create_pool(probe->shm, fd, (int32_t)bytes);
	close(fd);
	surface->pixels = map;
	surface->mapped = bytes;

	/* The colour. */
	for (index = 0; index < bytes / 4U; index++)
		surface->pixels[index] = color;

	/* The buffer, attached and committed. */
	surface->buffer = wl_shm_pool_create_buffer(pool, 0, surface->width, surface->height, surface->width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	wl_surface_attach(surface->surface, surface->buffer, 0, 0);
	wl_surface_damage(surface->surface, 0, 0, surface->width, surface->height);
	wl_surface_commit(surface->surface);

	/* Succeeded: the log line. */
	printf("SUBPROBE draw %s color=%06x\n", surface->name, color & 0xffffffU);
	fflush(stdout);
	return 0;
}

/* Destroys a surface's image: its buffer and its memory. */
static void
probe_release(
	struct probe_surface *surface)
{
	/* The buffer. */
	if (surface->buffer != NULL)
		wl_buffer_destroy(surface->buffer);
	surface->buffer = NULL;

	/* Its memory. */
	if (surface->pixels != NULL)
		(void)munmap(surface->pixels, surface->mapped);
	surface->pixels = NULL;
	surface->mapped = 0;
}

/* Makes anonymous shared memory of a size and maps it; returns its fd or -1. */
static int
probe_shared_memory(
	size_t size,
	void **map)
{
	static unsigned sequence;
	char name[64];
	int error;
	int fd;

	/* A name used only until it is unlinked. */
	sequence++;
	(void)snprintf(name, sizeof(name), "/subsurface-probe-%ld-%u", (long)getpid(), sequence);
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return -1;
	(void)shm_unlink(name);

	/* Its size. */
	error = ftruncate(fd, (off_t)size);
	if (error != 0) {
		close(fd);
		return -1;
	}

	/* Its mapping. */
	*map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (*map == MAP_FAILED) {
		close(fd);
		return -1;
	}

	/* Succeeded: the memory's descriptor. */
	return fd;
}

/* Carries out what a key asks (see the file's comment). */
static void
probe_key(
	struct probe *probe,
	uint32_t key)
{
	struct probe_surface *window;
	struct probe_surface *a;
	struct probe_surface *b;

	/* The surfaces. */
	window = &probe->surfaces[PROBE_WINDOW];
	a = &probe->surfaces[PROBE_A];
	b = &probe->surfaces[PROBE_B];

	/* Each key. */
	switch (key) {
	case PROBE_KEY_S:
		/* a's new place and image, which wait for the window. */
		if (a->subsurface == NULL)
			break;
		wl_subsurface_set_position(a->subsurface, 200, 20);
		(void)probe_draw(probe, a, PROBE_A2_COLOR);
		break;
	case PROBE_KEY_C:
		/* The window commits without a new image. */
		wl_surface_commit(window->surface);
		printf("SUBPROBE commit window\n");
		break;
	case PROBE_KEY_D:
		/* b shows its commits at once from now. */
		wl_subsurface_set_desync(b->subsurface);
		(void)probe_draw(probe, b, PROBE_B2_COLOR);
		break;
	case PROBE_KEY_O:
		/* b goes above the window. */
		wl_subsurface_place_above(b->subsurface, window->surface);
		printf("SUBPROBE place b above window\n");
		break;
	case PROBE_KEY_X:
		/* a loses its role (and c with it is not shown). */
		if (a->subsurface == NULL)
			break;
		wl_subsurface_destroy(a->subsurface);
		a->subsurface = NULL;
		printf("SUBPROBE destroy a\n");
		break;
	default:
		break;
	}

	/* The log lines go out now. */
	fflush(stdout);
}

/* Names one of the probe's surfaces in the log ("other" for none of them). */
static const char *
probe_name(
	struct probe *probe,
	struct wl_surface *surface)
{
	unsigned index;

	/* Each of the probe's surfaces. */
	for (index = 0; index < PROBE_SURFACES; index++) {
		/* The one it is. */
		if (surface != NULL && probe->surfaces[index].surface == surface)
			return probe->surfaces[index].name;
	}

	/* None of them. */
	return "other";
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

	/* The compositor. */
	probe = data;
	same = strcmp(interface, "wl_compositor");
	if (same == 0)
		probe->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4U);

	/* The subcompositor. */
	same = strcmp(interface, "wl_subcompositor");
	if (same == 0)
		probe->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1U);

	/* wl_shm. */
	same = strcmp(interface, "wl_shm");
	if (same == 0)
		probe->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1U);

	/* The shell. */
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

/* Takes the pointer and the keyboard when the seat has them. */
static void
seat_capabilities(
	void *data,
	struct wl_seat *seat,
	uint32_t capabilities)
{
	struct probe *probe;

	/* The pointer. */
	probe = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0U && probe->pointer == NULL) {
		probe->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(probe->pointer, &pointer_listener, probe);
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

/* Logs which surface the pointer entered, and where. */
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
	UNUSED_PARAMETER(serial);

	/* The surface and the place. */
	probe = data;
	printf("SUBPROBE enter %s x=%d y=%d\n", probe_name(probe, surface), wl_fixed_to_int(x), wl_fixed_to_int(y));
	fflush(stdout);
}

/* Logs a surface the pointer left. */
static void
pointer_leave(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct probe *probe;

	UNUSED_PARAMETER(pointer);
	UNUSED_PARAMETER(serial);

	/* The surface. */
	probe = data;
	printf("SUBPROBE leave %s\n", probe_name(probe, surface));
	fflush(stdout);
}

/* The motion is not logged (enter says where the pointer came in). */
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

/* Logs a button. */
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

	/* The log line. */
	printf("SUBPROBE button %u state=%u\n", button, state);
	fflush(stdout);
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

/* Closes the keymap's descriptor (the keys are evdev codes). */
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

/* Logs which surface has the keyboard. */
static void
keyboard_enter(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface,
	struct wl_array *keys)
{
	struct probe *probe;

	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(keys);

	/* The surface. */
	probe = data;
	printf("SUBPROBE focus %s\n", probe_name(probe, surface));
	fflush(stdout);
}

/* Logs a surface losing the keyboard. */
static void
keyboard_leave(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct probe *probe;

	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(serial);

	/* The surface. */
	probe = data;
	printf("SUBPROBE unfocus %s\n", probe_name(probe, surface));
	fflush(stdout);
}

/* Acts on a pressed key. */
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

	/* The key's request. */
	probe_key(probe, key);
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
