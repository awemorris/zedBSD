/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests the compositor's clipboard between clients (WS035 p079): wl_data_device,
 * wl_data_source and wl_data_offer.
 *
 * The probe shows a window (its colour from --color) and takes the
 * keyboard.  Whenever it is told a selection it did not set, it receives
 * the text through a pipe and logs it.  Keys: s sets its own text (--text)
 * as the selection, offered as text/plain;charset=utf-8 and text/plain;
 * q destroys its source (the clipboard is then empty).  Every event is one
 * line: DATAPROBE <what> ...
 *
 *   data-probe [--text=TEXT] [--color=RRGGBB] [--timeout-s=N] [--token=NAME] [--secret]
 *
 * --secret (ws102-p018) also offers x-kde-passwordManagerHint, the type a
 * password manager marks a secret with.
 *
 * A drag of text over the window (ws189-p002) is read at once at its
 * enter -- before any drop, which the compositor answers with nothing,
 * since a drag's data goes only to the window it is dropped on -- and is
 * taken as a copy; dropped, it is read again and finished.  Lines:
 * DATAPROBE drag enter|leave|drop and DATAPROBE drag received
 * when=early|drop bytes=N text=...
 *
 * --two-devices (ws189-p002 F2) gets a second wl_data_device on the same
 * seat, as a program with two windows has one a window: the compositor
 * sends a drag to every device of the client, and drops it on one.  The
 * second device takes a drag with text as a copy too (it is told second,
 * so the first is the one dropped on) and logs DATAPROBE drag
 * enter|leave|drop device=2 ..., and its text when dropped on.
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

/* The window's size. */
#define PROBE_WIDTH		300
#define PROBE_HEIGHT		200

/* The keys q and s (evdev codes). */
#define PROBE_KEY_Q		16U
#define PROBE_KEY_S		31U

/* The text types offered and looked for. */
#define PROBE_TYPE_UTF8		"text/plain;charset=utf-8"
#define PROBE_TYPE_PLAIN	"text/plain"
#define PROBE_TYPE_SECRET	"x-kde-passwordManagerHint"

/* The most text received, and how long a receive may wait for its writer. */
#define PROBE_TEXT_MAX		4096U
#define PROBE_RECEIVE_MS	3000

/*
 * The probe's connection, its globals, its window, its data device, the
 * offer being described and whether it has text, its own source (while it
 * is the selection), and the
 * serial of the keyboard's last enter or key (a selection needs one).
 */
struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_data_device_manager *manager;
	struct wl_data_device *device;
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer;
	struct wl_data_offer *offer;
	struct wl_data_source *source;
	int offer_text;
	uint32_t serial;
	uint32_t color;
	const char *text;
	int configured;
	int closed;
	const char *token;
	/* Whether the text is secret (--secret: the source also offers the password managers' hint, ws102-p018). */
	int secret;
	/* The offer of a drag over the window (NULL for none), and the serial of its enter (ws189-p002). */
	struct wl_data_offer *drag_offer;
	uint32_t drag_serial;
	/* The second data device (--two-devices, ws189-p002 F2), and the offer of a drag over it (NULL for none). */
	int two_devices;
	struct wl_data_device *second;
	struct wl_data_offer *second_offer;
};

static int probe_options(int count, char **arguments, struct probe *probe, unsigned *timeout);
static int probe_connect(struct probe *probe);
static int probe_draw(struct probe *probe);
static void probe_set_selection(struct probe *probe);
static void probe_drop_source(struct probe *probe);
static void probe_receive(struct probe *probe, struct wl_data_offer *offer);
static size_t probe_read(struct probe *probe, struct wl_data_offer *offer, char *text);
static void probe_drag_read(struct probe *probe, const char *when);
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
static void device_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer);
static void device_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer);
static void device_leave(void *data, struct wl_data_device *device);
static void device_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void device_drop(void *data, struct wl_data_device *device);
static void device_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer);
static void second_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer);
static void second_leave(void *data, struct wl_data_device *device);
static void second_drop(void *data, struct wl_data_device *device);
static void second_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer);
static void offer_offer(void *data, struct wl_data_offer *offer, const char *mime_type);
static void offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t actions);
static void offer_action(void *data, struct wl_data_offer *offer, uint32_t action);
static void source_target(void *data, struct wl_data_source *source, const char *mime_type);
static void source_send(void *data, struct wl_data_source *source, const char *mime_type, int32_t fd);
static void source_cancelled(void *data, struct wl_data_source *source);
static void source_drop_performed(void *data, struct wl_data_source *source);
static void source_finished(void *data, struct wl_data_source *source);
static void source_action(void *data, struct wl_data_source *source, uint32_t action);

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

/* The data device's. */
static const struct wl_data_device_listener device_listener = {
	device_data_offer,
	device_enter,
	device_leave,
	device_motion,
	device_drop,
	device_selection
};

/* The second data device's (--two-devices): its offers are described as the first's. */
static const struct wl_data_device_listener second_listener = {
	device_data_offer,
	second_enter,
	second_leave,
	device_motion,
	second_drop,
	second_selection
};

/* Every offer's. */
static const struct wl_data_offer_listener offer_listener = {
	offer_offer,
	offer_source_actions,
	offer_action
};

/* The probe's own source's. */
static const struct wl_data_source_listener source_listener = {
	source_target,
	source_send,
	source_cancelled,
	source_drop_performed,
	source_finished,
	source_action
};

/*
 * Shows the window and answers the clipboard until the timeout or the
 * close.
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
		fprintf(stderr, "usage: data-probe [--text=TEXT] [--color=RRGGBB] [--timeout-s=N] [--token=NAME] [--secret] [--two-devices]\n");
		return 2;
	}

	/* The connection and the window. */
	error = probe_connect(&probe);
	if (error != 0) {
		printf("DATAPROBE FAILED run=%s setup errno=%d\n", probe.token, error);
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
			printf("DATAPROBE FAILED run=%s dispatch errno=%d\n", probe.token, errno);
			return 1;
		}
	}

	/* The end. */
	printf("DATAPROBE DONE run=%s\n", probe.token);
	fflush(stdout);
	wl_display_disconnect(probe.display);

	/* Succeeded: the probe ran to its end. */
	return 0;
}

/* Reads the options: --text=TEXT, --color=RRGGBB, --timeout-s=N (default 120), --token=NAME, --secret and --two-devices. */
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
	probe->text = "data probe";
	probe->color = 0xff2b3444U;
	*timeout = 120U;

	/* Each option. */
	for (index = 1; index < count; index++) {
		/* A secret text. */
		same = strcmp(arguments[index], "--secret");
		if (same == 0) {
			probe->secret = 1;
			continue;
		}

		/* A second data device. */
		same = strcmp(arguments[index], "--two-devices");
		if (same == 0) {
			probe->two_devices = 1;
			continue;
		}

		/* The name in the log. */
		same = strncmp(arguments[index], "--token=", 8);
		if (same == 0) {
			probe->token = arguments[index] + 8;
			continue;
		}

		/* The text offered. */
		same = strncmp(arguments[index], "--text=", 7);
		if (same == 0) {
			probe->text = arguments[index] + 7;
			continue;
		}

		/* The window's colour. */
		same = strncmp(arguments[index], "--color=", 8);
		if (same == 0) {
			value = strtoul(arguments[index] + 8, &end, 16);
			if (*end != '\0' || value > 0xffffffUL)
				return -1;
			probe->color = 0xff000000U | (uint32_t)value;
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

/* Connects, binds the globals, gets the data device and shows the window. */
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

	/* The probe needs the compositor, wl_shm, the shell, a seat and the data device manager. */
	if (probe->compositor == NULL || probe->shm == NULL || probe->shell == NULL)
		return EOPNOTSUPP;
	if (probe->seat == NULL || probe->manager == NULL)
		return EOPNOTSUPP;

	/* The seat's data device. */
	probe->device = wl_data_device_manager_get_data_device(probe->manager, probe->seat);
	wl_data_device_add_listener(probe->device, &device_listener, probe);
	if (probe->two_devices) {
		probe->second = wl_data_device_manager_get_data_device(probe->manager, probe->seat);
		wl_data_device_add_listener(probe->second, &second_listener, probe);
	}

	/* The window: a toplevel, configured before it is drawn. */
	probe->surface = wl_compositor_create_surface(probe->compositor);
	probe->role = xdg_wm_base_get_xdg_surface(probe->shell, probe->surface);
	xdg_surface_add_listener(probe->role, &role_listener, probe);
	probe->toplevel = xdg_surface_get_toplevel(probe->role);
	xdg_toplevel_add_listener(probe->toplevel, &toplevel_listener, probe);
	xdg_toplevel_set_title(probe->toplevel, probe->token);
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
	printf("DATAPROBE ready run=%s\n", probe->token);
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
	(void)snprintf(name, sizeof(name), "/data-probe-%ld", (long)getpid());
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

	/* Every pixel in the colour. */
	pixels = map;
	for (index = 0; index < bytes / 4U; index++)
		pixels[index] = probe->color;

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

/* Sets the probe's text as the selection (a new source replaces its last one). */
static void
probe_set_selection(
	struct probe *probe)
{
	/* The last source goes. */
	probe_drop_source(probe);

	/* A source with the two text types. */
	probe->source = wl_data_device_manager_create_data_source(probe->manager);
	wl_data_source_add_listener(probe->source, &source_listener, probe);
	wl_data_source_offer(probe->source, PROBE_TYPE_UTF8);
	wl_data_source_offer(probe->source, PROBE_TYPE_PLAIN);
	if (probe->secret)
		wl_data_source_offer(probe->source, PROBE_TYPE_SECRET);

	/* The selection, named by the serial of the key that asked. */
	wl_data_device_set_selection(probe->device, probe->source, probe->serial);
	printf("DATAPROBE set selection text=%s\n", probe->text);
	fflush(stdout);
}

/* Destroys the probe's source, if it has one. */
static void
probe_drop_source(
	struct probe *probe)
{
	/* No source. */
	if (probe->source == NULL)
		return;

	/* It goes. */
	wl_data_source_destroy(probe->source);
	probe->source = NULL;
	printf("DATAPROBE source destroyed\n");
	fflush(stdout);
}

/*
 * Receives an offer's text through a pipe and logs it.
 */
static void
probe_receive(
	struct probe *probe,
	struct wl_data_offer *offer)
{
	char text[PROBE_TEXT_MAX + 1U];
	size_t length;

	/* The text. */
	length = probe_read(probe, offer, text);

	/* The log line (a line break in the text ends what is shown). */
	printf("DATAPROBE received bytes=%u text=%s\n", (unsigned)length, text);
	fflush(stdout);
}

/*
 * Reads an offer's text through a pipe: the offer is asked to write it,
 * then the pipe is read until its end (or a few seconds).  Returns the
 * bytes, the text ended by a NUL.
 */
static size_t
probe_read(
	struct probe *probe,
	struct wl_data_offer *offer,
	char *text)
{
	struct pollfd descriptor;
	uint64_t deadline;
	uint64_t now;
	size_t length;
	ssize_t got;
	int pipes[2];
	int error;
	int ready;

	/* The pipe; its writing end goes to the source's client. */
	text[0] = '\0';
	error = pipe(pipes);
	if (error != 0) {
		printf("DATAPROBE FAILED run=%s pipe errno=%d\n", probe->token, errno);
		return 0;
	}

	/* The offer is asked to write into it (the probe's copy of the writing end goes). */
	wl_data_offer_receive(offer, PROBE_TYPE_UTF8, pipes[1]);
	close(pipes[1]);
	(void)wl_display_flush(probe->display);

	/* The text, until the writer closes (or the time is up). */
	length = 0;
	deadline = probe_clock() + PROBE_RECEIVE_MS;
	while (length < PROBE_TEXT_MAX) {
		/* The time is up. */
		now = probe_clock();
		if (now >= deadline)
			break;

		/* The pipe becomes readable. */
		descriptor.fd = pipes[0];
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		ready = poll(&descriptor, 1, 100);
		if (ready <= 0)
			continue;

		/* What came; nothing more is the end. */
		got = read(pipes[0], text + length, PROBE_TEXT_MAX - length);
		if (got <= 0)
			break;
		length += (size_t)got;
	}

	/* The reading end goes. */
	close(pipes[0]);

	/* Succeeded: the text and its length. */
	text[length] = '\0';
	return length;
}

/* Reads the drag's text over the window and logs it, with when it was read (early: at the enter; drop). */
static void
probe_drag_read(
	struct probe *probe,
	const char *when)
{
	char text[PROBE_TEXT_MAX + 1U];
	size_t length;

	/* The text. */
	length = probe_read(probe, probe->drag_offer, text);

	/* The log line the tests read. */
	printf("DATAPROBE drag received when=%s bytes=%u text=%s\n", when, (unsigned)length, text);
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

	/* The data device manager. */
	same = strcmp(interface, "wl_data_device_manager");
	if (same == 0)
		probe->manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3U);
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

/* Notes the keyboard's serial and logs the focus. */
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
	UNUSED_PARAMETER(surface);
	UNUSED_PARAMETER(keys);

	/* The serial and the log line. */
	probe = data;
	probe->serial = serial;
	printf("DATAPROBE focus\n");
	fflush(stdout);
}

/* Logs the keyboard leaving. */
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

	/* The log line. */
	printf("DATAPROBE unfocus\n");
	fflush(stdout);
}

/* Acts on a pressed key: s sets the selection, q destroys the source. */
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
	UNUSED_PARAMETER(time);

	/* Only a press acts; its serial names the selection. */
	probe = data;
	if (state == 0U)
		return;
	probe->serial = serial;

	/* The key's request. */
	if (key == PROBE_KEY_S)
		probe_set_selection(probe);
	if (key == PROBE_KEY_Q)
		probe_drop_source(probe);
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

/* Takes a new offer: its types are heard next. */
static void
device_data_offer(
	void *data,
	struct wl_data_device *device,
	struct wl_data_offer *offer)
{
	struct probe *probe;

	UNUSED_PARAMETER(device);

	/* The offer being described, with no text type yet. */
	probe = data;
	probe->offer_text = 0;
	wl_data_offer_add_listener(offer, &offer_listener, probe);
	printf("DATAPROBE offer\n");
	fflush(stdout);
}

/*
 * A drag came over the window: its text is read at once (the compositor
 * gives nothing before a drop), then taken as a copy when it has text.
 */
static void
device_enter(
	void *data,
	struct wl_data_device *device,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y,
	struct wl_data_offer *offer)
{
	struct probe *probe;
	uint32_t version;

	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(surface);
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);

	/* An earlier drag's offer goes; a drag without data has nothing to read. */
	probe = data;
	if (probe->drag_offer != NULL && probe->drag_offer != offer)
		wl_data_offer_destroy(probe->drag_offer);
	probe->drag_offer = offer;
	probe->drag_serial = serial;
	printf("DATAPROBE drag enter text=%d\n", probe->offer_text);
	fflush(stdout);
	if (offer == NULL)
		return;

	/* Read before any drop. */
	probe_drag_read(probe, "early");

	/* Taken as a copy when it has text, refused otherwise. */
	if (probe->offer_text) {
		wl_data_offer_accept(offer, serial, PROBE_TYPE_UTF8);
	} else {
		wl_data_offer_accept(offer, serial, NULL);
	}

	/* A copy, the one action taken (version 3). */
	version = wl_proxy_get_version((struct wl_proxy *)offer);
	if (version >= 3U)
		wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	(void)wl_display_flush(probe->display);
}

/* The drag left the window: its offer goes. */
static void
device_leave(
	void *data,
	struct wl_data_device *device)
{
	struct probe *probe;

	UNUSED_PARAMETER(device);

	/* The offer, and the line. */
	probe = data;
	if (probe->drag_offer != NULL)
		wl_data_offer_destroy(probe->drag_offer);
	probe->drag_offer = NULL;
	printf("DATAPROBE drag leave\n");
	fflush(stdout);
}

/* The drag moves over the window: nothing to do. */
static void
device_motion(
	void *data,
	struct wl_data_device *device,
	uint32_t time,
	wl_fixed_t x,
	wl_fixed_t y)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(time);
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
}

/* The drag was dropped on the window: its text is read and the drop finished. */
static void
device_drop(
	void *data,
	struct wl_data_device *device)
{
	struct probe *probe;
	uint32_t version;

	UNUSED_PARAMETER(device);

	/* Only a drag with an offer. */
	probe = data;
	printf("DATAPROBE drag drop\n");
	fflush(stdout);
	if (probe->drag_offer == NULL)
		return;

	/* The text, now the window's. */
	probe_drag_read(probe, "drop");

	/* Finished (version 3), and the offer goes. */
	version = wl_proxy_get_version((struct wl_proxy *)probe->drag_offer);
	if (version >= 3U)
		wl_data_offer_finish(probe->drag_offer);
	wl_data_offer_destroy(probe->drag_offer);
	probe->drag_offer = NULL;
	(void)wl_display_flush(probe->display);
}

/*
 * Takes the selection: none is logged; another client's text is received
 * (the probe's own is not, it would have to write to itself).
 */
static void
device_selection(
	void *data,
	struct wl_data_device *device,
	struct wl_data_offer *offer)
{
	struct probe *probe;

	UNUSED_PARAMETER(device);

	/* The last offer goes. */
	probe = data;
	if (probe->offer != NULL && probe->offer != offer)
		wl_data_offer_destroy(probe->offer);
	probe->offer = offer;

	/* An empty clipboard. */
	if (offer == NULL) {
		printf("DATAPROBE selection none\n");
		fflush(stdout);
		return;
	}

	/*
	 * The probe's own selection coming back: its source is the selection
	 * until it is cancelled or destroyed (a source is cancelled before the
	 * selection that replaces it is told).
	 */
	if (probe->source != NULL) {
		printf("DATAPROBE selection own\n");
		fflush(stdout);
		return;
	}

	/* Another client's: its text, when it has one. */
	printf("DATAPROBE selection text=%d\n", probe->offer_text);
	fflush(stdout);
	if (probe->offer_text)
		probe_receive(probe, offer);
}

/* Notes a type of the offer being described. */
static void
offer_offer(
	void *data,
	struct wl_data_offer *offer,
	const char *mime_type)
{
	struct probe *probe;
	int same;

	UNUSED_PARAMETER(offer);

	/* The UTF-8 text type. */
	probe = data;
	same = strcmp(mime_type, PROBE_TYPE_UTF8);
	if (same == 0)
		probe->offer_text = 1;
	printf("DATAPROBE type %s\n", mime_type);
	fflush(stdout);
}

/* Drag and drop is not used. */
static void
offer_source_actions(
	void *data,
	struct wl_data_offer *offer,
	uint32_t actions)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(offer);
	UNUSED_PARAMETER(actions);
}

/* Drag and drop is not used. */
static void
offer_action(
	void *data,
	struct wl_data_offer *offer,
	uint32_t action)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(offer);
	UNUSED_PARAMETER(action);
}

/* Drag and drop is not used. */
static void
source_target(
	void *data,
	struct wl_data_source *source,
	const char *mime_type)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(mime_type);
}

/* Writes the probe's text into the descriptor another client reads, and closes it. */
static void
source_send(
	void *data,
	struct wl_data_source *source,
	const char *mime_type,
	int32_t fd)
{
	struct probe *probe;
	ssize_t written;

	UNUSED_PARAMETER(source);

	/* The text, all of it. */
	probe = data;
	written = write(fd, probe->text, strlen(probe->text));
	close(fd);
	printf("DATAPROBE send mime=%s bytes=%ld\n", mime_type, (long)written);
	fflush(stdout);
}

/* The source is not the selection any more: it goes. */
static void
source_cancelled(
	void *data,
	struct wl_data_source *source)
{
	struct probe *probe;

	/* The log line, and the source destroyed. */
	probe = data;
	printf("DATAPROBE cancelled\n");
	fflush(stdout);
	if (probe->source == source) {
		wl_data_source_destroy(source);
		probe->source = NULL;
	}
}

/* Drag and drop is not used. */
static void
source_drop_performed(
	void *data,
	struct wl_data_source *source)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(source);
}

/* Drag and drop is not used. */
static void
source_finished(
	void *data,
	struct wl_data_source *source)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(source);
}

/* Drag and drop is not used. */
static void
source_action(
	void *data,
	struct wl_data_source *source,
	uint32_t action)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(action);
}

/* A drag came over the window, told to the second device: taken as a copy when it has text (nothing is read before a drop). */
static void
second_enter(
	void *data,
	struct wl_data_device *device,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y,
	struct wl_data_offer *offer)
{
	struct probe *probe;
	uint32_t version;

	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(surface);
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);

	/* An earlier drag's offer goes. */
	probe = data;
	if (probe->second_offer != NULL && probe->second_offer != offer)
		wl_data_offer_destroy(probe->second_offer);
	probe->second_offer = offer;
	printf("DATAPROBE drag enter device=2 text=%d\n", probe->offer_text);
	fflush(stdout);
	if (offer == NULL)
		return;

	/* Taken as a copy when it has text, refused otherwise. */
	if (probe->offer_text) {
		wl_data_offer_accept(offer, serial, PROBE_TYPE_UTF8);
	} else {
		wl_data_offer_accept(offer, serial, NULL);
	}

	/* A copy, the one action taken (version 3). */
	version = wl_proxy_get_version((struct wl_proxy *)offer);
	if (version >= 3U)
		wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	(void)wl_display_flush(probe->display);
}

/* The drag left (or was dropped on the other device): the second device's offer goes. */
static void
second_leave(
	void *data,
	struct wl_data_device *device)
{
	struct probe *probe;

	UNUSED_PARAMETER(device);

	/* The offer, and the line. */
	probe = data;
	if (probe->second_offer != NULL)
		wl_data_offer_destroy(probe->second_offer);
	probe->second_offer = NULL;
	printf("DATAPROBE drag leave device=2\n");
	fflush(stdout);
}

/* The drag was dropped through the second device: its text is read and the drop finished. */
static void
second_drop(
	void *data,
	struct wl_data_device *device)
{
	char text[PROBE_TEXT_MAX + 1U];
	struct probe *probe;
	uint32_t version;
	size_t length;

	UNUSED_PARAMETER(device);

	/* Only a drag with an offer. */
	probe = data;
	printf("DATAPROBE drag drop device=2\n");
	fflush(stdout);
	if (probe->second_offer == NULL)
		return;

	/* The text, the line, the finish. */
	length = probe_read(probe, probe->second_offer, text);
	printf("DATAPROBE drag received device=2 when=drop bytes=%u text=%s\n", (unsigned)length, text);
	fflush(stdout);
	version = wl_proxy_get_version((struct wl_proxy *)probe->second_offer);
	if (version >= 3U)
		wl_data_offer_finish(probe->second_offer);
	wl_data_offer_destroy(probe->second_offer);
	probe->second_offer = NULL;
	(void)wl_display_flush(probe->display);
}

/* The selection told to the second device: its offer goes (the first device follows the selection). */
static void
second_selection(
	void *data,
	struct wl_data_device *device,
	struct wl_data_offer *offer)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(device);

	/* Not this device's to read. */
	if (offer != NULL)
		wl_data_offer_destroy(offer);
}
