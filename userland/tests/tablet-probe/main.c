/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests what the compositor tells a client about a pen (WS079 p003): with the
 * tablet protocol (zwp_tablet_manager_v2) the tablets and tools announced,
 * the proximity, touch, place, pressure, tilt, buttons and frames; with
 * --pointer the probe does not bind the tablet and logs the pointer's
 * events, which a pen gives such a client (the touch as BTN_LEFT, the barrel
 * buttons as BTN_RIGHT and BTN_MIDDLE).  A window (light, 640x480) shows
 * each touch as a dot whose size follows the pressure (blue for the pen,
 * red for the eraser; black for the pointer's left button).  With --touch
 * (WS079 p013) the probe binds wl_touch instead and logs the fingers it is
 * given (down, motion, up, frame, cancel), painting a green dot where each
 * finger is.  --color=RRGGBB changes the window's background, so a test can
 * tell the window on the screen.  Every event is one line:
 * TABLETPROBE <what> ....
 *
 *   tablet-probe [--timeout-s=N] [--token=NAME] [--pointer | --touch] [--color=RRGGBB]
 */

#include <tablet-unstable-v2-client-protocol.h>
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

/* The window's size and colours. */
#define PROBE_WIDTH		640
#define PROBE_HEIGHT		480
#define PROBE_BACKGROUND	0xfff2efe6U
#define PROBE_PEN_COLOR		0xff1f4fa8U
#define PROBE_ERASER_COLOR	0xffc0392bU
#define PROBE_POINTER_COLOR	0xff202020U
#define PROBE_TOUCH_COLOR	0xff1e8a3aU

/* The largest dot, at full pressure, in pixels of radius. */
#define PROBE_DOT_MAX		10

/* The pointer's left button, as evdev numbers it. */
#define PROBE_BUTTON_LEFT	0x110U

/*
 * The probe's connection, its globals, its window with its roles and its
 * buffer, the pen's state as the events have left it, and whether the
 * window was configured or closed.
 *
 * One instance lives for the whole run.
 */
struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_touch *touch;
	struct zwp_tablet_manager_v2 *tablet_manager;
	struct zwp_tablet_seat_v2 *tablet_seat;
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	int configured;
	int closed;
	int pointer_mode;
	int touch_mode;
	uint32_t background;
	const char *token;
	struct zwp_tablet_tool_v2 *tools[4];
	uint32_t tool_types[4];
	unsigned tool_count;
	uint32_t tool_type;
	int down;
	int pointer_down;
	double x;
	double y;
	uint32_t pressure;
	unsigned dirty;
};

static int probe_options(int count, char **arguments, struct probe *probe, unsigned *timeout);
static int probe_connect(struct probe *probe);
static int probe_draw(struct probe *probe);
static void probe_dot(struct probe *probe, uint32_t color, int radius);
static void probe_show(struct probe *probe);
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
static void pointer_frame(void *data, struct wl_pointer *pointer);
static void tablet_seat_tablet(void *data, struct zwp_tablet_seat_v2 *seat, struct zwp_tablet_v2 *tablet);
static void tablet_seat_tool(void *data, struct zwp_tablet_seat_v2 *seat, struct zwp_tablet_tool_v2 *tool);
static void tablet_seat_pad(void *data, struct zwp_tablet_seat_v2 *seat, struct zwp_tablet_pad_v2 *pad);
static void tablet_name(void *data, struct zwp_tablet_v2 *tablet, const char *name);
static void tablet_id(void *data, struct zwp_tablet_v2 *tablet, uint32_t vendor, uint32_t product);
static void tablet_path(void *data, struct zwp_tablet_v2 *tablet, const char *path);
static void tablet_done(void *data, struct zwp_tablet_v2 *tablet);
static void tablet_removed(void *data, struct zwp_tablet_v2 *tablet);
static void tool_type(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t type);
static void tool_serial(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t high, uint32_t low);
static void tool_wacom(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t high, uint32_t low);
static void tool_capability(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t capability);
static void tool_done(void *data, struct zwp_tablet_tool_v2 *tool);
static void tool_removed(void *data, struct zwp_tablet_tool_v2 *tool);
static void tool_proximity_in(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial, struct zwp_tablet_v2 *tablet, struct wl_surface *surface);
static void tool_proximity_out(void *data, struct zwp_tablet_tool_v2 *tool);
static void tool_down(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial);
static void tool_up(void *data, struct zwp_tablet_tool_v2 *tool);
static void tool_motion(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t x, wl_fixed_t y);
static void tool_pressure(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t pressure);
static void tool_distance(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t distance);
static void tool_tilt(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t x, wl_fixed_t y);
static void tool_rotation(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t degrees);
static void tool_slider(void *data, struct zwp_tablet_tool_v2 *tool, int32_t position);
static void tool_wheel(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t degrees, int32_t clicks);
static void tool_button(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial, uint32_t button, uint32_t state);
static void tool_frame(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t time);
static void touch_down(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time, struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y);
static void touch_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time, int32_t id);
static void touch_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y);
static void touch_frame(void *data, struct wl_touch *touch);
static void touch_cancel(void *data, struct wl_touch *touch);
static int probe_color(const char *text, uint32_t *color);

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

/* The pointer's (--pointer only). */
static const struct wl_pointer_listener pointer_listener = {
	pointer_enter,
	pointer_leave,
	pointer_motion,
	pointer_button,
	NULL,
	pointer_frame,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

/* The touch's (--touch only; shape and orientation are version 6). */
static const struct wl_touch_listener touch_listener = {
	touch_down,
	touch_up,
	touch_motion,
	touch_frame,
	touch_cancel,
	NULL,
	NULL
};

/* The tablet seat's. */
static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
	tablet_seat_tablet,
	tablet_seat_tool,
	tablet_seat_pad
};

/* Each tablet's. */
static const struct zwp_tablet_v2_listener tablet_listener = {
	tablet_name,
	tablet_id,
	tablet_path,
	tablet_done,
	tablet_removed
};

/* Each tool's. */
static const struct zwp_tablet_tool_v2_listener tool_listener = {
	tool_type,
	tool_serial,
	tool_wacom,
	tool_capability,
	tool_done,
	tool_removed,
	tool_proximity_in,
	tool_proximity_out,
	tool_down,
	tool_up,
	tool_motion,
	tool_pressure,
	tool_distance,
	tool_tilt,
	tool_rotation,
	tool_slider,
	tool_wheel,
	tool_button,
	tool_frame
};

/*
 * Shows the window and logs what the pen gives it until the timeout or the
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
		fprintf(stderr, "usage: tablet-probe [--timeout-s=N] [--token=NAME] [--pointer | --touch] [--color=RRGGBB]\n");
		return 2;
	}

	/* The connection and the window. */
	error = probe_connect(&probe);
	if (error != 0) {
		printf("TABLETPROBE FAILED run=%s setup errno=%d\n", probe.token, error);
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
			printf("TABLETPROBE FAILED run=%s dispatch errno=%d\n", probe.token, errno);
			return 1;
		}

		/* The lines leave at once, for a reader of the log. */
		fflush(stdout);
	}

	/* The end. */
	printf("TABLETPROBE DONE run=%s\n", probe.token);
	fflush(stdout);
	wl_display_disconnect(probe.display);

	/* Succeeded: the probe ran to its end. */
	return 0;
}

/* Reads the options: --timeout-s=N (default 120), --token=NAME, --pointer, --touch and --color=RRGGBB. */
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
	int error;

	/* The defaults. */
	probe->token = "probe";
	probe->background = PROBE_BACKGROUND;
	*timeout = 120U;

	/* Each option. */
	for (index = 1; index < count; index++) {
		/* The name in the log. */
		same = strncmp(arguments[index], "--token=", 8);
		if (same == 0) {
			probe->token = arguments[index] + 8;
			continue;
		}

		/* The pointer instead of the tablet. */
		same = strcmp(arguments[index], "--pointer");
		if (same == 0) {
			probe->pointer_mode = 1;
			continue;
		}

		/* wl_touch instead of the tablet. */
		same = strcmp(arguments[index], "--touch");
		if (same == 0) {
			probe->touch_mode = 1;
			continue;
		}

		/* The background colour. */
		same = strncmp(arguments[index], "--color=", 8);
		if (same == 0) {
			error = probe_color(arguments[index] + 8, &probe->background);
			if (error != 0)
				return -1;
			continue;
		}

		/* The timeout. */
		same = strncmp(arguments[index], "--timeout-s=", 12);
		if (same != 0)
			return -1;
		value = strtoul(arguments[index] + 12, &end, 10);
		if (*end != '\0' ||
		    value == 0UL ||
		    value > 3600UL)
			return -1;
		*timeout = (unsigned)value;
	}

	/* Succeeded: the options are read. */
	return 0;
}

/* Connects, binds the globals, takes the tablet or the pointer and shows the window. */
static int
probe_connect(
	struct probe *probe)
{
	const char *mode;
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

	/* The probe needs the compositor, wl_shm, the shell and a seat. */
	if (probe->compositor == NULL ||
	    probe->shm == NULL ||
	    probe->shell == NULL)
		return EOPNOTSUPP;
	if (probe->seat == NULL)
		return EOPNOTSUPP;

	/* The pointer, the touch, or the tablet seat (which needs the manager). */
	if (probe->pointer_mode) {
		probe->pointer = wl_seat_get_pointer(probe->seat);
		wl_pointer_add_listener(probe->pointer, &pointer_listener, probe);
	} else if (probe->touch_mode) {
		probe->touch = wl_seat_get_touch(probe->seat);
		wl_touch_add_listener(probe->touch, &touch_listener, probe);
	} else {
		if (probe->tablet_manager == NULL)
			return EOPNOTSUPP;
		probe->tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(probe->tablet_manager, probe->seat);
		zwp_tablet_seat_v2_add_listener(probe->tablet_seat, &tablet_seat_listener, probe);
	}

	/* The window: a toplevel, configured before it is drawn. */
	probe->surface = wl_compositor_create_surface(probe->compositor);
	probe->role = xdg_wm_base_get_xdg_surface(probe->shell, probe->surface);
	xdg_surface_add_listener(probe->role, &role_listener, probe);
	probe->toplevel = xdg_surface_get_toplevel(probe->role);
	xdg_toplevel_add_listener(probe->toplevel, &toplevel_listener, probe);
	if (probe->pointer_mode) {
		xdg_toplevel_set_title(probe->toplevel, "Pointer probe");
	} else if (probe->touch_mode) {
		xdg_toplevel_set_title(probe->toplevel, "Touch probe");
	} else {
		xdg_toplevel_set_title(probe->toplevel, "Tablet probe");
	}

	/* The application ID, and the commit that asks for the first configure. */
	xdg_toplevel_set_app_id(probe->toplevel, "tablet-probe");
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
	mode = "tablet";
	if (probe->pointer_mode)
		mode = "pointer";
	if (probe->touch_mode)
		mode = "touch";
	printf("TABLETPROBE ready run=%s mode=%s\n", probe->token, mode);
	fflush(stdout);
	return 0;
}

/* Makes the window's wl_shm buffer in the background colour and shows it. */
static int
probe_draw(
	struct probe *probe)
{
	struct wl_shm_pool *pool;
	char name[64];
	size_t bytes;
	size_t index;
	void *map;
	int error;
	int fd;

	/* Anonymous shared memory of the image's size. */
	bytes = (size_t)PROBE_WIDTH * (size_t)PROBE_HEIGHT * 4U;
	(void)snprintf(name, sizeof(name), "/tablet-probe-%ld", (long)getpid());
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return errno;
	(void)shm_unlink(name);
	error = ftruncate(fd, (off_t)bytes);
	if (error != 0) {
		close(fd);
		return EIO;
	}

	/* Its mapping, kept for the dots. */
	map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return ENOMEM;
	}

	/* The pixels stay mapped for the probe's life. */
	probe->pixels = map;

	/* Every pixel in the background colour. */
	for (index = 0; index < bytes / 4U; index++)
		probe->pixels[index] = probe->background;

	/* The buffer. */
	pool = wl_shm_create_pool(probe->shm, fd, (int32_t)bytes);
	close(fd);
	probe->buffer = wl_shm_pool_create_buffer(pool, 0, PROBE_WIDTH, PROBE_HEIGHT, PROBE_WIDTH * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);

	/* Succeeded: the window is shown. */
	probe_show(probe);
	return 0;
}

/* Paints a dot at the pen's place. */
static void
probe_dot(
	struct probe *probe,
	uint32_t color,
	int radius)
{
	int center_x;
	int center_y;
	int x;
	int y;

	/* The dot's centre in whole pixels. */
	center_x = (int)probe->x;
	center_y = (int)probe->y;

	/* Every pixel of the dot that is on the window. */
	for (y = center_y - radius; y <= center_y + radius; y++) {
		/* A row off the window is skipped. */
		if (y < 0 || y >= PROBE_HEIGHT)
			continue;

		/* Each pixel of the row inside the circle and on the window. */
		for (x = center_x - radius; x <= center_x + radius; x++) {
			/* A pixel off the window is skipped. */
			if (x < 0 || x >= PROBE_WIDTH)
				continue;

			/* A pixel outside the circle is skipped. */
			if ((x - center_x) * (x - center_x) + (y - center_y) * (y - center_y) > radius * radius)
				continue;

			/* The pixel takes the colour. */
			probe->pixels[y * PROBE_WIDTH + x] = color;
		}
	}

	/* The image must be committed again. */
	probe->dirty = 1;
}

/* Attaches the buffer again so the compositor shows the dots. */
static void
probe_show(
	struct probe *probe)
{
	/* The whole window is damaged and committed. */
	wl_surface_attach(probe->surface, probe->buffer, 0, 0);
	wl_surface_damage(probe->surface, 0, 0, PROBE_WIDTH, PROBE_HEIGHT);
	wl_surface_commit(probe->surface);
	probe->dirty = 0;
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

	/* The tablet manager, which the pointer and touch modes leave alone. */
	same = strcmp(interface, "zwp_tablet_manager_v2");
	if (same == 0) {
		printf("TABLETPROBE global zwp_tablet_manager_v2 version=%u\n", version);
		if (!probe->pointer_mode && !probe->touch_mode)
			probe->tablet_manager = wl_registry_bind(registry, name, &zwp_tablet_manager_v2_interface, 1U);
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

/* The window was closed. */
static void
toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct probe *probe;

	UNUSED_PARAMETER(toplevel);

	/* The run ends. */
	probe = data;
	probe->closed = 1;
}

/* Logs the seat's capabilities. */
static void
seat_capabilities(
	void *data,
	struct wl_seat *seat,
	uint32_t capabilities)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(seat);

	/* One line. */
	printf("TABLETPROBE seat capabilities=%u\n", capabilities);
}

/* The seat's name is not needed. */
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

/* Logs the pointer entering the window. */
static void
pointer_enter(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(pointer);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(surface);

	/* One line. */
	printf("TABLETPROBE pointer enter x=%d y=%d\n", x / 256, y / 256);
}

/* Logs the pointer leaving the window. */
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

	/* One line. */
	printf("TABLETPROBE pointer leave\n");
}

/* Logs the pointer's motion, and paints while the left button is held. */
static void
pointer_motion(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct probe *probe;

	UNUSED_PARAMETER(pointer);
	UNUSED_PARAMETER(time);

	/* One line, and a dot while the button is held. */
	probe = data;
	probe->x = (double)x / 256.0;
	probe->y = (double)y / 256.0;
	printf("TABLETPROBE pointer motion x=%d y=%d\n", x / 256, y / 256);
	if (probe->pointer_down)
		probe_dot(probe, PROBE_POINTER_COLOR, 4);
}

/* Logs a pointer button. */
static void
pointer_button(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	struct probe *probe;

	UNUSED_PARAMETER(pointer);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(time);

	/* One line; the left button paints from its press. */
	probe = data;
	printf("TABLETPROBE pointer button=0x%x state=%u\n", button, state);
	if (button == PROBE_BUTTON_LEFT) {
		probe->pointer_down = 0;
		if (state != 0U) {
			probe->pointer_down = 1;
			probe_dot(probe, PROBE_POINTER_COLOR, 6);
		}
	}
}

/* Shows the dots at the end of a pointer frame. */
static void
pointer_frame(
	void *data,
	struct wl_pointer *pointer)
{
	struct probe *probe;

	UNUSED_PARAMETER(pointer);

	/* A changed image is committed. */
	probe = data;
	if (probe->dirty)
		probe_show(probe);
}

/* Logs a new tablet and listens to it. */
static void
tablet_seat_tablet(
	void *data,
	struct zwp_tablet_seat_v2 *seat,
	struct zwp_tablet_v2 *tablet)
{
	UNUSED_PARAMETER(seat);

	/* One line, then its description. */
	printf("TABLETPROBE tablet added\n");
	zwp_tablet_v2_add_listener(tablet, &tablet_listener, data);
}

/* Logs a new tool and listens to it. */
static void
tablet_seat_tool(
	void *data,
	struct zwp_tablet_seat_v2 *seat,
	struct zwp_tablet_tool_v2 *tool)
{
	UNUSED_PARAMETER(seat);

	/* One line, then its description. */
	printf("TABLETPROBE tool added\n");
	zwp_tablet_tool_v2_add_listener(tool, &tool_listener, data);
}

/* A pad is not expected (the compositor offers none). */
static void
tablet_seat_pad(
	void *data,
	struct zwp_tablet_seat_v2 *seat,
	struct zwp_tablet_pad_v2 *pad)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(seat);
	UNUSED_PARAMETER(pad);

	/* One line. */
	printf("TABLETPROBE pad added\n");
}

/* Logs a tablet's name. */
static void
tablet_name(
	void *data,
	struct zwp_tablet_v2 *tablet,
	const char *name)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tablet);

	/* One line. */
	printf("TABLETPROBE tablet name=\"%s\"\n", name);
}

/* Logs a tablet's USB identity. */
static void
tablet_id(
	void *data,
	struct zwp_tablet_v2 *tablet,
	uint32_t vendor,
	uint32_t product)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tablet);

	/* One line. */
	printf("TABLETPROBE tablet id=%04x:%04x\n", vendor, product);
}

/* Logs a tablet's path. */
static void
tablet_path(
	void *data,
	struct zwp_tablet_v2 *tablet,
	const char *path)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tablet);

	/* One line. */
	printf("TABLETPROBE tablet path=%s\n", path);
}

/* Logs the end of a tablet's description. */
static void
tablet_done(
	void *data,
	struct zwp_tablet_v2 *tablet)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tablet);

	/* One line. */
	printf("TABLETPROBE tablet done\n");
}

/* Logs a tablet going, and lets its object go. */
static void
tablet_removed(
	void *data,
	struct zwp_tablet_v2 *tablet)
{
	UNUSED_PARAMETER(data);

	/* One line, and the object is destroyed. */
	printf("TABLETPROBE tablet removed\n");
	zwp_tablet_v2_destroy(tablet);
}

/* Logs a tool's type. */
static void
tool_type(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t type)
{
	struct probe *probe;

	/* One line; the type is remembered for the tool's proximity. */
	probe = data;
	printf("TABLETPROBE tool type=0x%x\n", type);
	if (probe->tool_count < 4U) {
		probe->tools[probe->tool_count] = tool;
		probe->tool_types[probe->tool_count] = type;
		probe->tool_count++;
	}
}

/* Logs a tool's serial number. */
static void
tool_serial(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t high,
	uint32_t low)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);

	/* One line. */
	printf("TABLETPROBE tool serial=%08x%08x\n", high, low);
}

/* A Wacom hardware ID is not expected. */
static void
tool_wacom(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t high,
	uint32_t low)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);

	/* One line. */
	printf("TABLETPROBE tool wacom=%08x%08x\n", high, low);
}

/* Logs a tool's capability. */
static void
tool_capability(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t capability)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);

	/* One line. */
	printf("TABLETPROBE tool capability=%u\n", capability);
}

/* Logs the end of a tool's description. */
static void
tool_done(
	void *data,
	struct zwp_tablet_tool_v2 *tool)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);

	/* One line. */
	printf("TABLETPROBE tool done\n");
}

/* Logs a tool going, and lets its object go. */
static void
tool_removed(
	void *data,
	struct zwp_tablet_tool_v2 *tool)
{
	UNUSED_PARAMETER(data);

	/* One line, and the object is destroyed. */
	printf("TABLETPROBE tool removed\n");
	zwp_tablet_tool_v2_destroy(tool);
}

/* Logs a tool coming over the window, and remembers which tool it is. */
static void
tool_proximity_in(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t serial,
	struct zwp_tablet_v2 *tablet,
	struct wl_surface *surface)
{
	struct probe *probe;
	unsigned index;

	UNUSED_PARAMETER(serial);

	/* The tool's type, as it was announced. */
	probe = data;
	probe->tool_type = 0;
	for (index = 0; index < probe->tool_count; index++) {
		/* The entry of this tool. */
		if (probe->tools[index] == tool)
			probe->tool_type = probe->tool_types[index];
	}

	/* One line: the tool, and whether the tablet and the surface are the probe's. */
	printf("TABLETPROBE proximity_in tool=0x%x tablet=%u surface=%u\n", probe->tool_type, tablet != NULL, surface == probe->surface);
}

/* Logs a tool leaving the window. */
static void
tool_proximity_out(
	void *data,
	struct zwp_tablet_tool_v2 *tool)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);

	/* One line. */
	printf("TABLETPROBE proximity_out\n");
}

/* Logs a touch, which starts painting. */
static void
tool_down(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t serial)
{
	struct probe *probe;

	UNUSED_PARAMETER(tool);
	UNUSED_PARAMETER(serial);

	/* One line. */
	probe = data;
	probe->down = 1;
	printf("TABLETPROBE down\n");
}

/* Logs a lift, which stops painting. */
static void
tool_up(
	void *data,
	struct zwp_tablet_tool_v2 *tool)
{
	struct probe *probe;

	UNUSED_PARAMETER(tool);

	/* One line. */
	probe = data;
	probe->down = 0;
	printf("TABLETPROBE up\n");
}

/* Logs the tool's place on the window. */
static void
tool_motion(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct probe *probe;

	UNUSED_PARAMETER(tool);

	/* One line, with the place to 1/256 pixel. */
	probe = data;
	probe->x = (double)x / 256.0;
	probe->y = (double)y / 256.0;
	printf("TABLETPROBE motion x=%.2f y=%.2f\n", probe->x, probe->y);
}

/* Logs the pressure. */
static void
tool_pressure(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t pressure)
{
	struct probe *probe;

	UNUSED_PARAMETER(tool);

	/* One line. */
	probe = data;
	probe->pressure = pressure;
	printf("TABLETPROBE pressure=%u\n", pressure);
}

/* A distance is not expected. */
static void
tool_distance(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t distance)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);

	/* One line. */
	printf("TABLETPROBE distance=%u\n", distance);
}

/* Logs the tilt in degrees. */
static void
tool_tilt(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	wl_fixed_t x,
	wl_fixed_t y)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);

	/* One line, to 1/100 degree. */
	printf("TABLETPROBE tilt x=%.2f y=%.2f\n", (double)x / 256.0, (double)y / 256.0);
}

/* A rotation is not expected. */
static void
tool_rotation(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	wl_fixed_t degrees)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);

	/* One line. */
	printf("TABLETPROBE rotation=%d\n", degrees);
}

/* A slider is not expected. */
static void
tool_slider(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	int32_t position)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);

	/* One line. */
	printf("TABLETPROBE slider=%d\n", position);
}

/* A wheel is not expected. */
static void
tool_wheel(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	wl_fixed_t degrees,
	int32_t clicks)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);

	/* One line. */
	printf("TABLETPROBE wheel=%d clicks=%d\n", degrees, clicks);
}

/* Logs a barrel button. */
static void
tool_button(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t serial,
	uint32_t button,
	uint32_t state)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(tool);
	UNUSED_PARAMETER(serial);

	/* One line. */
	printf("TABLETPROBE button=0x%x state=%u\n", button, state);
}

/* Paints the frame's dot while touching, and shows it. */
static void
tool_frame(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t time)
{
	struct probe *probe;
	uint32_t color;
	int radius;

	UNUSED_PARAMETER(tool);
	UNUSED_PARAMETER(time);

	/* One line. */
	probe = data;
	printf("TABLETPROBE frame\n");

	/* A touching tool paints a dot as large as its pressure. */
	if (probe->down) {
		color = PROBE_PEN_COLOR;
		if (probe->tool_type == ZWP_TABLET_TOOL_V2_TYPE_ERASER)
			color = PROBE_ERASER_COLOR;
		radius = 1 + (int)((uint64_t)probe->pressure * (PROBE_DOT_MAX - 1) / 65535U);
		probe_dot(probe, color, radius);
	}

	/* A changed image is committed. */
	if (probe->dirty)
		probe_show(probe);
}

/* Logs a finger touching the window, and paints a dot there. */
static void
touch_down(
	void *data,
	struct wl_touch *touch,
	uint32_t serial,
	uint32_t time,
	struct wl_surface *surface,
	int32_t id,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct probe *probe;

	UNUSED_PARAMETER(touch);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(time);

	/* One line: the finger, whether the surface is the probe's, and the place. */
	probe = data;
	probe->x = (double)x / 256.0;
	probe->y = (double)y / 256.0;
	printf("TABLETPROBE touch down id=%d surface=%u x=%.2f y=%.2f\n", id, surface == probe->surface, probe->x, probe->y);

	/* The dot where it touched. */
	probe_dot(probe, PROBE_TOUCH_COLOR, 8);
}

/* Logs a finger lifting. */
static void
touch_up(
	void *data,
	struct wl_touch *touch,
	uint32_t serial,
	uint32_t time,
	int32_t id)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(touch);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(time);

	/* One line. */
	printf("TABLETPROBE touch up id=%d\n", id);
}

/* Logs a finger moving on the window, and paints a dot there. */
static void
touch_motion(
	void *data,
	struct wl_touch *touch,
	uint32_t time,
	int32_t id,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct probe *probe;

	UNUSED_PARAMETER(touch);
	UNUSED_PARAMETER(time);

	/* One line, with the surface-local place to 1/256 pixel. */
	probe = data;
	probe->x = (double)x / 256.0;
	probe->y = (double)y / 256.0;
	printf("TABLETPROBE touch motion id=%d x=%.2f y=%.2f\n", id, probe->x, probe->y);

	/* The dot where it is. */
	probe_dot(probe, PROBE_TOUCH_COLOR, 4);
}

/* Logs the end of a group of finger events, and shows the dots. */
static void
touch_frame(
	void *data,
	struct wl_touch *touch)
{
	struct probe *probe;

	UNUSED_PARAMETER(touch);

	/* One line. */
	probe = data;
	printf("TABLETPROBE touch frame\n");

	/* A changed image is committed. */
	if (probe->dirty)
		probe_show(probe);
}

/* Logs the compositor taking the fingers away. */
static void
touch_cancel(
	void *data,
	struct wl_touch *touch)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(touch);

	/* One line. */
	printf("TABLETPROBE touch cancel\n");
}

/* Reads a colour written RRGGBB into an opaque ARGB pixel; -1 for anything else. */
static int
probe_color(
	const char *text,
	uint32_t *color)
{
	unsigned long value;
	size_t length;
	char *end;

	/* Six hexadecimal digits. */
	length = strlen(text);
	if (length != 6U)
		return -1;
	value = strtoul(text, &end, 16);
	if (*end != '\0')
		return -1;

	/* Succeeded: the colour, opaque. */
	*color = 0xff000000U | (uint32_t)value;
	return 0;
}
