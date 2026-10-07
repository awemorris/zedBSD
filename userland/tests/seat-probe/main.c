/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests what the compositor tells a toolkit about the keyboard and the output
 * (WS035 p078): the XKB keymap (its format, its size, and that the mapped
 * file holds a keymap's text), the key repeat, the modifier masks (held and
 * locked), and wl_output version 4 (name, description, mode, scale).  A
 * window (dark, 400x300) takes the keyboard.  Every event is one line:
 * SEATPROBE <what> ...; the output is released at the end.
 *
 *   seat-probe [--timeout-s=N] [--token=NAME]
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

/* The window's size and colour. */
#define PROBE_WIDTH		400
#define PROBE_HEIGHT		300
#define PROBE_COLOR		0xff2b3444U

/* The output version the probe asks for (4 has name and description). */
#define PROBE_OUTPUT_VERSION	4U

/*
 * The probe's connection, its globals, its window with its roles and its
 * buffer, and whether the window was configured or closed.
 */
struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_output *output;
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer;
	int configured;
	int closed;
	const char *token;
};

static int probe_options(int count, char **arguments, struct probe *probe, unsigned *timeout);
static int probe_connect(struct probe *probe);
static int probe_draw(struct probe *probe);
static uint64_t probe_clock(void);
static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name);
static void shell_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void role_configure(void *data, struct xdg_surface *role, uint32_t serial);
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void toplevel_close(void *data, struct xdg_toplevel *toplevel);
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities);
static void seat_name(void *data, struct wl_seat *seat, const char *name);
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size);
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys);
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface);
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
static void keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay);
static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t width, int32_t height, int32_t subpixel, const char *make, const char *model, int32_t transform);
static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh);
static void output_done(void *data, struct wl_output *output);
static void output_scale(void *data, struct wl_output *output, int32_t factor);
static void output_name(void *data, struct wl_output *output, const char *name);
static void output_description(void *data, struct wl_output *output, const char *description);

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

/* The keyboard's. */
static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_keymap,
	keyboard_enter,
	keyboard_leave,
	keyboard_key,
	keyboard_modifiers,
	keyboard_repeat
};

/* The output's (version 4). */
static const struct wl_output_listener output_listener = {
	output_geometry,
	output_mode,
	output_done,
	output_scale,
	output_name,
	output_description
};

/*
 * Shows the window and logs what the keyboard and the output are told
 * until the timeout or the close.
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
		fprintf(stderr, "usage: seat-probe [--timeout-s=N] [--token=NAME]\n");
		return 2;
	}

	/* The connection and the window. */
	error = probe_connect(&probe);
	if (error != 0) {
		printf("SEATPROBE FAILED run=%s setup errno=%d\n", probe.token, error);
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
			printf("SEATPROBE FAILED run=%s dispatch errno=%d\n", probe.token, errno);
			return 1;
		}
	}

	/* The output is given back (version 3 release), and the end. */
	if (probe.output != NULL)
		wl_output_release(probe.output);
	(void)wl_display_roundtrip(probe.display);
	printf("SEATPROBE DONE run=%s\n", probe.token);
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

/* Connects, binds the globals and shows the window. */
static int
probe_connect(
	struct probe *probe)
{
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

	/* The probe needs the compositor, wl_shm, the shell, a seat and an output. */
	if (probe->compositor == NULL || probe->shm == NULL || probe->shell == NULL)
		return EOPNOTSUPP;
	if (probe->seat == NULL || probe->output == NULL)
		return EOPNOTSUPP;

	/* The window: a toplevel, configured before it is drawn. */
	probe->surface = wl_compositor_create_surface(probe->compositor);
	probe->role = xdg_wm_base_get_xdg_surface(probe->shell, probe->surface);
	xdg_surface_add_listener(probe->role, &role_listener, probe);
	probe->toplevel = xdg_surface_get_toplevel(probe->role);
	xdg_toplevel_add_listener(probe->toplevel, &toplevel_listener, probe);
	xdg_toplevel_set_title(probe->toplevel, "seat probe");
	wl_surface_commit(probe->surface);

	/* The first configure. */
	status = wl_display_roundtrip(probe->display);
	if (status < 0 || !probe->configured)
		return EPROTO;

	/* Its image. */
	status = probe_draw(probe);
	if (status != 0)
		return status;

	/* Succeeded: the log line the test waits for. */
	printf("SEATPROBE ready run=%s\n", probe->token);
	fflush(stdout);
	return 0;
}

/* Draws the window in its colour into a wl_shm buffer and commits it. */
static int
probe_draw(
	struct probe *probe)
{
	struct wl_shm_pool *pool;
	uint32_t *pixels;
	char name[64];
	size_t bytes;
	size_t index;
	void *map;
	int error;
	int fd;

	/* Anonymous shared memory of the image's size. */
	bytes = (size_t)PROBE_WIDTH * (size_t)PROBE_HEIGHT * 4U;
	(void)snprintf(name, sizeof(name), "/seat-probe-%ld", (long)getpid());
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return errno;
	(void)shm_unlink(name);
	error = ftruncate(fd, (off_t)bytes);
	if (error != 0) {
		close(fd);
		return EIO;
	}

	/* Its mapping, in the colour. */
	map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return ENOMEM;
	}

	/* Every pixel in the colour. */
	pixels = map;
	for (index = 0; index < bytes / 4U; index++)
		pixels[index] = PROBE_COLOR;

	/* The buffer, attached and committed. */
	pool = wl_shm_create_pool(probe->shm, fd, (int32_t)bytes);
	close(fd);
	probe->buffer = wl_shm_pool_create_buffer(pool, 0, PROBE_WIDTH, PROBE_HEIGHT, PROBE_WIDTH * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	wl_surface_attach(probe->surface, probe->buffer, 0, 0);
	wl_surface_damage(probe->surface, 0, 0, PROBE_WIDTH, PROBE_HEIGHT);
	wl_surface_commit(probe->surface);

	/* Succeeded: the window is shown. */
	return 0;
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
	uint32_t output_version;
	int same;

	/* The compositor. */
	probe = data;
	same = strcmp(interface, "wl_compositor");
	if (same == 0)
		probe->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4U);

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

	/* The output, at version 4 when the compositor has it. */
	same = strcmp(interface, "wl_output");
	if (same == 0) {
		output_version = version;
		if (output_version > PROBE_OUTPUT_VERSION)
			output_version = PROBE_OUTPUT_VERSION;
		probe->output = wl_registry_bind(registry, name, &wl_output_interface, output_version);
		wl_output_add_listener(probe->output, &output_listener, probe);
		printf("SEATPROBE output version=%u\n", output_version);
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

/* Takes the keyboard when the seat has one. */
static void
seat_capabilities(
	void *data,
	struct wl_seat *seat,
	uint32_t capabilities)
{
	struct probe *probe;

	/* The keyboard. */
	probe = data;
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

/* Logs the keymap: its format and size, and whether the mapped file holds a keymap's text. */
static void
keyboard_keymap(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t format,
	int32_t fd,
	uint32_t size)
{
	const char *text;
	void *map;
	int keymap;
	int terminated;
	int same;

	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(keyboard);

	/* The text, mapped privately as version 7 clients must. */
	keymap = 0;
	terminated = 0;
	if (size != 0U) {
		map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (map != MAP_FAILED) {
			text = map;
			same = strncmp(text, "xkb_keymap {", 12);
			if (same == 0)
				keymap = 1;
			if (text[size - 1U] == '\0')
				terminated = 1;
			(void)munmap(map, size);
		}
	}

	/* The log line; the descriptor is the probe's to close. */
	printf("SEATPROBE keymap format=%u size=%u text=%d terminated=%d\n", format, size, keymap, terminated);
	fflush(stdout);
	if (fd >= 0)
		close(fd);
}

/* Logs the keyboard's focus. */
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

	/* The log line. */
	printf("SEATPROBE focus\n");
	fflush(stdout);
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

/* Logs a key. */
static void
keyboard_key(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(time);

	/* The log line. */
	printf("SEATPROBE key %u state=%u\n", key, state);
	fflush(stdout);
}

/* Logs the modifier masks. */
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

	/* The log line. */
	printf("SEATPROBE modifiers depressed=%u latched=%u locked=%u group=%u\n", depressed, latched, locked, group);
	fflush(stdout);
}

/* Logs the key repeat the client is to do. */
static void
keyboard_repeat(
	void *data,
	struct wl_keyboard *keyboard,
	int32_t rate,
	int32_t delay)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(keyboard);

	/* The log line. */
	printf("SEATPROBE repeat rate=%d delay=%d\n", rate, delay);
	fflush(stdout);
}

/* Logs the output's geometry (make and model). */
static void
output_geometry(
	void *data,
	struct wl_output *output,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height,
	int32_t subpixel,
	const char *make,
	const char *model,
	int32_t transform)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(output);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(subpixel);
	UNUSED_PARAMETER(transform);

	/* The log line. */
	printf("SEATPROBE output geometry x=%d y=%d make=%s model=%s\n", x, y, make, model);
}

/* Logs the output's mode. */
static void
output_mode(
	void *data,
	struct wl_output *output,
	uint32_t flags,
	int32_t width,
	int32_t height,
	int32_t refresh)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(output);

	/* The log line. */
	printf("SEATPROBE output mode flags=%u width=%d height=%d refresh=%d\n", flags, width, height, refresh);
}

/* Logs the end of the output's properties. */
static void
output_done(
	void *data,
	struct wl_output *output)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(output);

	/* The log line. */
	printf("SEATPROBE output done\n");
	fflush(stdout);
}

/* Logs the output's scale. */
static void
output_scale(
	void *data,
	struct wl_output *output,
	int32_t factor)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(output);

	/* The log line. */
	printf("SEATPROBE output scale=%d\n", factor);
}

/* Logs the output's name (version 4). */
static void
output_name(
	void *data,
	struct wl_output *output,
	const char *name)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(output);

	/* The log line. */
	printf("SEATPROBE output name=%s\n", name);
}

/* Logs the output's description (version 4). */
static void
output_description(
	void *data,
	struct wl_output *output,
	const char *description)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(output);

	/* The log line. */
	printf("SEATPROBE output description=%s\n", description);
}
