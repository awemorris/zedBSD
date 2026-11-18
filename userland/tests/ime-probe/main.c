/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A test client of the text input protocol (ws095-p004): a window of one
 * color whose text input is enabled while it has the keyboard, and which
 * writes what it hears to a log file for the input method's guest tests
 * to read over SSH:
 *
 *   PROBE ENTER / LEAVE            the text input's enter and leave
 *   PROBE KEY key=K state=S        a key heard on wl_keyboard
 *   PROBE DONE serial=N commits=C preedit=P begin=B end=E commit=T
 *                                  a done and what it applied
 *   PROBE TEXT text=T              everything committed so far
 *
 *   ime-probe [--log=/tmp/ime-probe.log] [--password] [--seconds=N] [--app-id=ID]
 *
 * With --password the field says it holds a password, so the input method
 * must not serve it.  --app-id names the window's application (ws095-p016:
 * the input method's state is kept for each application).
 */

#include <wayland/wayland-client.h>
#include <wayland/xdg-shell-client-protocol.h>
#include <wayland/text-input-unstable-v3-client-protocol.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The window's size, and the longest text kept. */
#define PROBE_WIDTH	320U
#define PROBE_HEIGHT	200U
#define PROBE_TEXT_MAX	4096U

/* The probe's state, for its lifetime. */
struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct zwp_text_input_manager_v3 *manager;
	struct zwp_text_input_v3 *input;
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer;
	FILE *log;
	int password;
	int configured;
	int closed;
	uint32_t commits;
	char preedit[PROBE_TEXT_MAX];
	int32_t preedit_begin;
	int32_t preedit_end;
	char commit[PROBE_TEXT_MAX];
	char text[PROBE_TEXT_MAX];
};

static void probe_line(struct probe *probe, const char *format, ...) __attribute__((format(printf, 2, 3)));
static int probe_buffer(struct probe *probe);
static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name);
static void shell_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void role_configure(void *data, struct xdg_surface *role, uint32_t serial);
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void toplevel_close(void *data, struct xdg_toplevel *toplevel);
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size);
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys);
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface);
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
static void keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay);
static void input_enter(void *data, struct zwp_text_input_v3 *input, struct wl_surface *surface);
static void input_leave(void *data, struct zwp_text_input_v3 *input, struct wl_surface *surface);
static void input_preedit(void *data, struct zwp_text_input_v3 *input, const char *text, int32_t begin, int32_t end);
static void input_commit(void *data, struct zwp_text_input_v3 *input, const char *text);
static void input_delete(void *data, struct zwp_text_input_v3 *input, uint32_t before, uint32_t after);
static void input_done(void *data, struct zwp_text_input_v3 *input, uint32_t serial);

static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };
static const struct xdg_wm_base_listener shell_listener = { shell_ping };
static const struct xdg_surface_listener role_listener = { role_configure };
static const struct xdg_toplevel_listener toplevel_listener = { toplevel_configure, toplevel_close, NULL };
static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat
};
static const struct zwp_text_input_v3_listener input_listener = {
	input_enter, input_leave, input_preedit, input_commit, input_delete, input_done
};

/*
 * Shows the window and logs what the text input hears until the time is up.
 */
int
main(
	int count,
	char **arguments)
{
	struct probe probe;
	const char *path;
	const char *app_id;
	struct pollfd descriptor;
	time_t end;
	long seconds;
	int index;
	int status;

	/* The options. */
	memset(&probe, 0, sizeof(probe));
	path = "/tmp/ime-probe.log";
	app_id = NULL;
	seconds = 600;
	for (index = 1; index < count; index++) {
		if (strncmp(arguments[index], "--log=", 6) == 0)
			path = arguments[index] + 6;
		else if (strcmp(arguments[index], "--password") == 0)
			probe.password = 1;
		else if (strncmp(arguments[index], "--seconds=", 10) == 0)
			seconds = strtol(arguments[index] + 10, NULL, 10);
		else if (strncmp(arguments[index], "--app-id=", 9) == 0)
			app_id = arguments[index] + 9;
		else {
			fprintf(stderr, "usage: ime-probe [--log=PATH] [--password] [--seconds=N] [--app-id=ID]\n");
			return 2;
		}
	}

	/* The log, written line by line. */
	probe.log = fopen(path, "w");
	if (probe.log == NULL) {
		fprintf(stderr, "ime-probe: cannot open %s: %d\n", path, errno);
		return 1;
	}

	/* The connection and the globals. */
	probe.display = wl_display_connect(NULL);
	if (probe.display == NULL) {
		probe_line(&probe, "PROBE FAILED step=connect errno=%d", errno);
		return 1;
	}

	probe.registry = wl_display_get_registry(probe.display);
	wl_registry_add_listener(probe.registry, &registry_listener, &probe);
	status = wl_display_roundtrip(probe.display);
	if (status < 0 || probe.compositor == NULL || probe.shm == NULL || probe.shell == NULL ||
	    probe.seat == NULL || probe.manager == NULL) {
		probe_line(&probe, "PROBE FAILED step=globals");
		return 1;
	}

	/* The keyboard and the text input. */
	probe.keyboard = wl_seat_get_keyboard(probe.seat);
	wl_keyboard_add_listener(probe.keyboard, &keyboard_listener, &probe);
	probe.input = zwp_text_input_manager_v3_get_text_input(probe.manager, probe.seat);
	zwp_text_input_v3_add_listener(probe.input, &input_listener, &probe);

	/* The window. */
	probe.surface = wl_compositor_create_surface(probe.compositor);
	probe.role = xdg_wm_base_get_xdg_surface(probe.shell, probe.surface);
	xdg_surface_add_listener(probe.role, &role_listener, &probe);
	probe.toplevel = xdg_surface_get_toplevel(probe.role);
	xdg_toplevel_add_listener(probe.toplevel, &toplevel_listener, &probe);
	xdg_toplevel_set_title(probe.toplevel, "ime-probe");
	if (app_id != NULL)
		xdg_toplevel_set_app_id(probe.toplevel, app_id);
	wl_surface_commit(probe.surface);
	status = wl_display_roundtrip(probe.display);
	if (status < 0 || !probe.configured) {
		probe_line(&probe, "PROBE FAILED step=configure");
		return 1;
	}

	/* Its one buffer. */
	status = probe_buffer(&probe);
	if (status != 0) {
		probe_line(&probe, "PROBE FAILED step=buffer errno=%d", status);
		return 1;
	}

	wl_surface_attach(probe.surface, probe.buffer, 0, 0);
	wl_surface_damage(probe.surface, 0, 0, (int32_t)PROBE_WIDTH, (int32_t)PROBE_HEIGHT);
	wl_surface_commit(probe.surface);
	probe_line(&probe, "PROBE READY password=%d", probe.password);

	/* Serves the events until the time is up or the window is closed. */
	end = time(NULL) + seconds;
	while (!probe.closed && time(NULL) < end) {
		(void)wl_display_flush(probe.display);
		descriptor.fd = wl_display_get_fd(probe.display);
		descriptor.events = POLLIN;
		status = poll(&descriptor, 1, 500);
		if (status < 0 && errno != EINTR)
			break;
		if (status > 0) {
			status = wl_display_dispatch(probe.display);
			if (status < 0)
				break;
		}
	}

	probe_line(&probe, "PROBE END");
	wl_display_disconnect(probe.display);
	fclose(probe.log);
	return 0;
}

/* Writes one line to the log and to standard output. */
static void
probe_line(
	struct probe *probe,
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(probe->log, format, arguments);
	va_end(arguments);
	fputc('\n', probe->log);
	fflush(probe->log);
}

/* Makes the window's one buffer, of one color. */
static int
probe_buffer(
	struct probe *probe)
{
	struct wl_shm_pool *pool;
	char name[64];
	uint32_t *pixels;
	size_t bytes;
	size_t i;
	int fd;

	/* Anonymous shared memory for the pixels. */
	bytes = (size_t)PROBE_WIDTH * PROBE_HEIGHT * 4U;
	snprintf(name, sizeof(name), "/ime-probe-%ld", (long)getpid());
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return errno;
	(void)shm_unlink(name);
	if (ftruncate(fd, (off_t)bytes) != 0) {
		close(fd);
		return errno;
	}

	pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return errno;
	}

	/* A light color: a password field is reddish. */
	for (i = 0; i < bytes / 4U; i++)
		pixels[i] = probe->password ? 0xfff0d0d0U : 0xffd0e0f0U;

	/* The buffer. */
	pool = wl_shm_create_pool(probe->shm, fd, (int32_t)bytes);
	close(fd);
	probe->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)PROBE_WIDTH, (int32_t)PROBE_HEIGHT, (int32_t)PROBE_WIDTH * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	return 0;
}

/* Binds the globals the probe needs. */
static void
registry_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct probe *probe;

	(void)version;
	probe = data;
	if (strcmp(interface, "wl_compositor") == 0)
		probe->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4U);
	else if (strcmp(interface, "wl_shm") == 0)
		probe->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1U);
	else if (strcmp(interface, "xdg_wm_base") == 0) {
		probe->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1U);
		xdg_wm_base_add_listener(probe->shell, &shell_listener, probe);
	} else if (strcmp(interface, "wl_seat") == 0)
		probe->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5U);
	else if (strcmp(interface, "zwp_text_input_manager_v3") == 0)
		probe->manager = wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1U);
	else if (strcmp(interface, "zwp_input_method_manager_v2") == 0)
		probe_line(probe, "PROBE SEES input_method_manager");
	else if (strcmp(interface, "zwp_virtual_keyboard_manager_v1") == 0)
		probe_line(probe, "PROBE SEES virtual_keyboard_manager");
}

/* A global that goes changes nothing. */
static void
registry_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

/* Answers the compositor's ping. */
static void
shell_ping(
	void *data,
	struct xdg_wm_base *shell,
	uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(shell, serial);
}

/* Acknowledges a configure. */
static void
role_configure(
	void *data,
	struct xdg_surface *role,
	uint32_t serial)
{
	struct probe *probe;

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
	(void)data;
	(void)toplevel;
	(void)width;
	(void)height;
	(void)states;
}

/* A close ends the run. */
static void
toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct probe *probe;

	(void)toplevel;
	probe = data;
	probe->closed = 1;
}

/* The keymap's descriptor is closed; keys are evdev codes. */
static void
keyboard_keymap(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t format,
	int32_t fd,
	uint32_t size)
{
	(void)data;
	(void)keyboard;
	(void)format;
	(void)size;
	close(fd);
}

/* The keyboard's enter. */
static void
keyboard_enter(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface,
	struct wl_array *keys)
{
	(void)keyboard;
	(void)serial;
	(void)surface;
	(void)keys;
	probe_line(data, "PROBE KEYBOARD enter");
}

/* The keyboard's leave. */
static void
keyboard_leave(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface)
{
	(void)keyboard;
	(void)serial;
	(void)surface;
	probe_line(data, "PROBE KEYBOARD leave");
}

/* A key the application hears (not taken by the input method). */
static void
keyboard_key(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	(void)keyboard;
	(void)serial;
	(void)time;
	probe_line(data, "PROBE KEY key=%u state=%u", key, state);
}

/* The modifiers are not logged. */
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
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)depressed;
	(void)latched;
	(void)locked;
	(void)group;
}

/* The repeat is not used. */
static void
keyboard_repeat(
	void *data,
	struct wl_keyboard *keyboard,
	int32_t rate,
	int32_t delay)
{
	(void)data;
	(void)keyboard;
	(void)rate;
	(void)delay;
}

/* The text input entered the window: it is enabled with its content type and cursor. */
static void
input_enter(
	void *data,
	struct zwp_text_input_v3 *input,
	struct wl_surface *surface)
{
	struct probe *probe;

	(void)surface;
	probe = data;
	probe_line(probe, "PROBE ENTER");
	zwp_text_input_v3_enable(input);
	if (probe->password)
		zwp_text_input_v3_set_content_type(input, ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT | ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD);
	else
		zwp_text_input_v3_set_content_type(input, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
	zwp_text_input_v3_set_surrounding_text(input, probe->text, (int32_t)strlen(probe->text), (int32_t)strlen(probe->text));
	zwp_text_input_v3_set_cursor_rectangle(input, 10, 10, 2, 20);
	zwp_text_input_v3_commit(input);
	probe->commits++;
}

/* The text input left the window. */
static void
input_leave(
	void *data,
	struct zwp_text_input_v3 *input,
	struct wl_surface *surface)
{
	(void)input;
	(void)surface;
	probe_line(data, "PROBE LEAVE");
}

/* The preedit, applied at done. */
static void
input_preedit(
	void *data,
	struct zwp_text_input_v3 *input,
	const char *text,
	int32_t begin,
	int32_t end)
{
	struct probe *probe;

	(void)input;
	probe = data;
	snprintf(probe->preedit, sizeof(probe->preedit), "%s", text != NULL ? text : "");
	probe->preedit_begin = begin;
	probe->preedit_end = end;
}

/* The text to commit, applied at done. */
static void
input_commit(
	void *data,
	struct zwp_text_input_v3 *input,
	const char *text)
{
	struct probe *probe;

	(void)input;
	probe = data;
	snprintf(probe->commit, sizeof(probe->commit), "%s", text != NULL ? text : "");
}

/* The text to delete (logged, not applied: the probe keeps no cursor). */
static void
input_delete(
	void *data,
	struct zwp_text_input_v3 *input,
	uint32_t before,
	uint32_t after)
{
	(void)input;
	probe_line(data, "PROBE DELETE before=%u after=%u", before, after);
}

/* done applies the preedit and the commit; the serial must be the number of the probe's commits. */
static void
input_done(
	void *data,
	struct zwp_text_input_v3 *input,
	uint32_t serial)
{
	struct probe *probe;
	size_t length;

	(void)input;
	probe = data;
	probe_line(probe, "PROBE DONE serial=%u commits=%u preedit=%s begin=%d end=%d commit=%s",
		   serial, probe->commits, probe->preedit, probe->preedit_begin, probe->preedit_end, probe->commit);

	/* The committed text is added to the probe's text. */
	if (probe->commit[0] != '\0') {
		length = strlen(probe->text);
		snprintf(probe->text + length, sizeof(probe->text) - length, "%s", probe->commit);
		probe_line(probe, "PROBE TEXT text=%s", probe->text);
	}

	probe->commit[0] = '\0';
}
