/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A Wayland window drawn with the CPU into wl_shm buffers (WS035 p053).
 *
 * The window is one color; with --band a band of another color moves down
 * it one step per frame and only the rows that changed are damaged.  Two
 * buffers alternate: a frame is drawn only into a buffer the compositor has
 * released.  With --cursor the window sets an 8x8 cursor of its own when
 * the pointer enters it; with --hide-cursor it hides the cursor.
 *
 *   wlshm [--size=WxH] [--color=AARRGGBB] [--xrgb] [--band=AARRGGBB]
 *         [--frames=N] [--cursor=AARRGGBB] [--hide-cursor] [--hold]
 *         [--delay-ms=N] [--token=NAME]
 */

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The band's height in rows. */
#define BAND_HEIGHT	20U

/* One of the two buffers the window alternates between. */
struct shm_buffer {
	struct wl_buffer *buffer;
	uint32_t *pixels;
	int busy;
};

/* The window, its buffers, and the state the listeners update. */
struct window {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	struct wl_surface *cursor_surface;
	struct wl_buffer *cursor_buffer;
	struct shm_buffer buffers[2];
	uint32_t width;
	uint32_t height;
	uint32_t color;
	uint32_t band_color;
	uint32_t cursor_color;
	uint32_t format;
	int band;
	int cursor;
	int hide_cursor;
	int hold;
	uint32_t delay_ms;
	int32_t shown_band;
	int configured;
	int frame_done;
	int closed;
	const char *token;
	const char *name;
};

static int options(int count, char **arguments, struct window *window, uint32_t *frames);
static int connect_window(struct window *window);
static int make_buffers(struct window *window);
static int make_cursor(struct window *window);
static int shared_memory(size_t size, void **map);
static void draw(struct window *window, struct shm_buffer *buffer, int32_t band);
static int wait_frame(struct window *window, struct shm_buffer **buffer);
static void commit_frame(struct window *window, struct shm_buffer *buffer, uint32_t frame);
static int option_number(const char *text, int base, unsigned long maximum, uint32_t *number);
static int option_size(const char *text, uint32_t *width, uint32_t *height);
static int dispatch(struct window *window);
static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name);
static void shell_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void role_configure(void *data, struct xdg_surface *role, uint32_t serial);
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void toplevel_close(void *data, struct xdg_toplevel *toplevel);
static void buffer_release(void *data, struct wl_buffer *buffer);
static void frame_done(void *data, struct wl_callback *callback, uint32_t time);
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities);
static void seat_name(void *data, struct wl_seat *seat, const char *name);
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y);
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface);
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value);

static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };
static const struct xdg_wm_base_listener shell_listener = { shell_ping };
static const struct xdg_surface_listener role_listener = { role_configure };
static const struct xdg_toplevel_listener toplevel_listener = { toplevel_configure, toplevel_close };
static const struct wl_buffer_listener buffer_listener = { buffer_release };
static const struct wl_callback_listener frame_listener = { frame_done };
static const struct wl_seat_listener seat_listener = { seat_capabilities, seat_name };
static const struct wl_pointer_listener pointer_listener = {
	pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis,
	NULL, NULL, NULL, NULL, NULL, NULL
};

/*
 * Draws the window's frames and reports each one.
 */
int
main(
	int count,
	char **arguments)
{
	struct window window;
	struct shm_buffer *buffer;
	uint32_t frames;
	uint32_t frame;
	int error;

	/* The options. */
	memset(&window, 0, sizeof(window));
	error = options(count, arguments, &window, &frames);
	if (error != 0) {
		fprintf(stderr, "usage: wlshm [--size=WxH] [--color=AARRGGBB] [--xrgb] [--band=AARRGGBB] [--frames=N] [--cursor=AARRGGBB] [--hide-cursor] [--hold] [--delay-ms=N] [--token=NAME] [--display=NAME]\n");
		return 2;
	}

	/* The window and its buffers. */
	error = connect_window(&window);
	if (error == 0)
		error = make_buffers(&window);
	if (error == 0 && window.cursor)
		error = make_cursor(&window);
	if (error != 0) {
		printf("WLSHM FAILED run=%s setup errno=%d\n", window.token, error);
		return 1;
	}

	/* Each frame into a released buffer, after the previous frame's callback. */
	window.frame_done = 1;
	window.shown_band = -1;
	for (frame = 1; frame <= frames; frame++) {
		/* A free buffer and a done frame, unless the window is closed. */
		error = wait_frame(&window, &buffer);
		if (error != 0) {
			printf("WLSHM FAILED run=%s dispatch errno=%d\n", window.token, error);
			return 1;
		}

		/* A closed window ends the run. */
		if (window.closed)
			break;

		/* With --delay-ms the frame is drawn that long after the compositor asked for it. */
		if (window.delay_ms != 0U)
			(void)usleep(window.delay_ms * 1000U);

		/* The frame. */
		commit_frame(&window, buffer, frame);
	}

	/* Done; with --hold the window stays, unchanged, until the compositor closes it. */
	printf("WLSHM DONE run=%s frames=%u\n", window.token, frame - 1U);
	fflush(stdout);
	while (window.hold && !window.closed) {
		error = dispatch(&window);
		if (error != 0)
			break;
	}

	/* The connection goes last. */
	wl_display_disconnect(window.display);
	return 0;
}

/* Reads the options; the defaults are a 320x240 opaque blue window, 60 frames. */
static int
options(
	int count,
	char **arguments,
	struct window *window,
	uint32_t *frames)
{
	static const char *const names[] = {
		"--size=", "--color=", "--xrgb", "--band=", "--frames=", "--cursor=",
		"--delay-ms=", "--hold", "--hide-cursor", "--token=", "--display="
	};
	const char *argument;
	const char *value;
	size_t length;
	size_t kind;
	int index;
	int match;
	int error;

	/* The defaults. */
	window->width = 320U;
	window->height = 240U;
	window->color = 0xff0000ffU;
	window->band_color = 0xffffffffU;
	window->format = WL_SHM_FORMAT_ARGB8888;
	window->token = "manual";
	*frames = 60U;

	/* Each option: its name (a prefix when it takes a value). */
	for (index = 1; index < count; index++) {
		argument = arguments[index];
		for (kind = 0; kind < sizeof(names) / sizeof(names[0]); kind++) {
			length = strlen(names[kind]);
			match = strncmp(argument, names[kind], length);
			if (match == 0 && (names[kind][length - 1U] == '=' || argument[length] == '\0'))
				break;
		}

		/* Its value. */
		value = argument + strlen(argument);
		if (kind < sizeof(names) / sizeof(names[0]))
			value = argument + strlen(names[kind]);
		error = -1;
		switch (kind) {
		case 0:
			error = option_size(value, &window->width, &window->height);
			break;
		case 1:
			error = option_number(value, 16, 0xffffffffUL, &window->color);
			break;
		case 2:
			window->format = WL_SHM_FORMAT_XRGB8888;
			error = 0;
			break;
		case 3:
			window->band = 1;
			error = option_number(value, 16, 0xffffffffUL, &window->band_color);
			break;
		case 4:
			error = option_number(value, 10, 100000UL, frames);
			break;
		case 5:
			window->cursor = 1;
			error = option_number(value, 16, 0xffffffffUL, &window->cursor_color);
			break;
		case 6:
			error = option_number(value, 10, 1000UL, &window->delay_ms);
			break;
		case 7:
			window->hold = 1;
			error = 0;
			break;
		case 8:
			window->hide_cursor = 1;
			error = 0;
			break;
		case 9:
			window->token = value;
			error = 0;
			break;
		case 10:
			window->name = value;
			error = 0;
			if (*value == '\0')
				error = -1;
			break;
		default:
			break;
		}

		/* The option was not understood. */
		if (error != 0)
			return -1;
	}

	/* A frame count of zero is no run. */
	if (*frames == 0U)
		return -1;

	/* Succeeded. */
	return 0;
}

/* Reads a whole number in a base, no larger than a maximum. */
static int
option_number(
	const char *text,
	int base,
	unsigned long maximum,
	uint32_t *number)
{
	unsigned long value;
	char *end;

	/* Digits and nothing after them. */
	value = strtoul(text, &end, base);
	if (end == text || *end != '\0' || value > maximum)
		return -1;

	/* Succeeded. */
	*number = (uint32_t)value;
	return 0;
}

/* Reads WIDTHxHEIGHT, each 32..4096. */
static int
option_size(
	const char *text,
	uint32_t *width,
	uint32_t *height)
{
	unsigned long value;
	char *end;

	/* The width, then x. */
	value = strtoul(text, &end, 10);
	if (end == text || *end != 'x' || value < 32UL || value > 4096UL)
		return -1;
	*width = (uint32_t)value;

	/* The height, and nothing after it. */
	text = end + 1;
	value = strtoul(text, &end, 10);
	if (end == text || *end != '\0' || value < 32UL || value > 4096UL)
		return -1;
	*height = (uint32_t)value;

	/* Succeeded. */
	return 0;
}

/* Connects, binds the globals, and makes the configured toplevel window. */
static int
connect_window(
	struct window *window)
{
	int status;

	/* The connection: the display named with --display, or the standard environment's. */
	window->display = wl_display_connect(window->name);
	if (window->display == NULL)
		return EIO;

	/* Its globals. */
	window->registry = wl_display_get_registry(window->display);
	wl_registry_add_listener(window->registry, &registry_listener, window);
	status = wl_display_roundtrip(window->display);
	if (status < 0)
		return EIO;

	/* The window needs these three. */
	if (window->compositor == NULL || window->shm == NULL || window->shell == NULL)
		return EOPNOTSUPP;

	/* The surface and its toplevel role, a window of its own size. */
	window->surface = wl_compositor_create_surface(window->compositor);
	window->role = xdg_wm_base_get_xdg_surface(window->shell, window->surface);
	xdg_surface_add_listener(window->role, &role_listener, window);
	window->toplevel = xdg_surface_get_toplevel(window->role);
	xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);
	xdg_toplevel_set_title(window->toplevel, "wl_shm test");
	wl_surface_commit(window->surface);
	status = wl_display_roundtrip(window->display);
	if (status < 0 || !window->configured)
		return EPROTO;

	/* Succeeded. */
	return 0;
}

/* Makes the two buffers in one pool. */
static int
make_buffers(
	struct window *window)
{
	struct wl_shm_pool *pool;
	size_t bytes;
	void *map;
	int fd;
	int index;

	/* One pool of two frames. */
	bytes = (size_t)window->width * window->height * 4U;
	fd = shared_memory(bytes * 2U, &map);
	if (fd < 0)
		return errno;
	pool = wl_shm_create_pool(window->shm, fd, (int32_t)(bytes * 2U));
	close(fd);

	/* Each buffer, one after the other in the pool. */
	for (index = 0; index < 2; index++) {
		window->buffers[index].pixels = (uint32_t *)((unsigned char *)map + (size_t)index * bytes);
		window->buffers[index].buffer = wl_shm_pool_create_buffer(pool, (int32_t)((size_t)index * bytes), (int32_t)window->width, (int32_t)window->height, (int32_t)window->width * 4, window->format);
		wl_buffer_add_listener(window->buffers[index].buffer, &buffer_listener, &window->buffers[index]);
	}

	/* The buffers keep the pool's memory; the pool object is not needed. */
	wl_shm_pool_destroy(pool);

	/* Succeeded. */
	return 0;
}

/* Makes the 8x8 cursor surface of one color. */
static int
make_cursor(
	struct window *window)
{
	struct wl_shm_pool *pool;
	uint32_t *pixels;
	void *map;
	int fd;
	int index;

	/* Its pixels. */
	fd = shared_memory(8U * 8U * 4U, &map);
	if (fd < 0)
		return errno;
	pixels = map;
	for (index = 0; index < 64; index++)
		pixels[index] = window->cursor_color;

	/* Its buffer and surface. */
	pool = wl_shm_create_pool(window->shm, fd, 8 * 8 * 4);
	close(fd);
	window->cursor_buffer = wl_shm_pool_create_buffer(pool, 0, 8, 8, 32, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	window->cursor_surface = wl_compositor_create_surface(window->compositor);

	/* Succeeded. */
	return 0;
}

/* Makes anonymous shared memory of a size and maps it. */
static int
shared_memory(
	size_t size,
	void **map)
{
	char name[64];
	int fd;
	int error;

	/* A name used only until it is unlinked. */
	snprintf(name, sizeof(name), "/wlshm-%ld", (long)getpid());
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

	/* Succeeded. */
	return fd;
}

/*
 * Waits until the previous frame is done and a buffer is free (or the window
 * is closed), and names the free buffer.
 */
static int
wait_frame(
	struct window *window,
	struct shm_buffer **buffer)
{
	int error;

	/* Events until both hold. */
	while (!window->closed &&
	       (!window->frame_done || (window->buffers[0].busy && window->buffers[1].busy))) {
		error = dispatch(window);
		if (error != 0)
			return error;
	}

	/* The first free buffer. */
	*buffer = &window->buffers[0];
	if (window->buffers[0].busy)
		*buffer = &window->buffers[1];
	return 0;
}

/*
 * Draws a frame into a buffer and commits it: the whole window at first,
 * then only the rows where the band was shown and where it is now.
 */
static void
commit_frame(
	struct window *window,
	struct shm_buffer *buffer,
	uint32_t frame)
{
	struct wl_callback *callback;
	int32_t band;

	/* The band moves one step. */
	band = -1;
	if (window->band)
		band = (int32_t)(((frame - 1U) * 10U) % (window->height - BAND_HEIGHT));
	draw(window, buffer, band);
	wl_surface_attach(window->surface, buffer->buffer, 0, 0);

	/* The damage: the rows that changed from what the compositor showed. */
	if (window->band && window->shown_band >= 0 && frame > 2U) {
		wl_surface_damage_buffer(window->surface, 0, window->shown_band, (int32_t)window->width, (int32_t)BAND_HEIGHT);
		wl_surface_damage_buffer(window->surface, 0, band, (int32_t)window->width, (int32_t)BAND_HEIGHT);
	} else {
		wl_surface_damage_buffer(window->surface, 0, 0, (int32_t)window->width, (int32_t)window->height);
	}

	/* The band the compositor will show. */
	window->shown_band = band;

	/* The frame callback, and the commit. */
	callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(callback, &frame_listener, window);
	window->frame_done = 0;
	buffer->busy = 1;
	wl_surface_commit(window->surface);
	wl_display_flush(window->display);
	printf("WLSHM FRAME run=%s frame=%u width=%u height=%u band=%d\n", window->token, frame, window->width, window->height, band);
	fflush(stdout);
}

/* Fills a buffer with the window's color and the band. */
static void
draw(
	struct window *window,
	struct shm_buffer *buffer,
	int32_t band)
{
	uint32_t x;
	uint32_t y;
	uint32_t color;

	/* Every row: the band's rows in its color. */
	for (y = 0; y < window->height; y++) {
		color = window->color;
		if (band >= 0 && y >= (uint32_t)band && y < (uint32_t)band + BAND_HEIGHT)
			color = window->band_color;
		for (x = 0; x < window->width; x++)
			buffer->pixels[y * window->width + x] = color;
	}
}

/* Waits for and dispatches events. */
static int
dispatch(
	struct window *window)
{
	int status;

	/* Blocking dispatch: releases, callbacks and configures. */
	status = wl_display_dispatch(window->display);
	if (status < 0)
		return EIO;
	return 0;
}

static void
registry_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct window *window;

	int compositor;
	int shm;
	int shell;
	int seat;

	/* The globals the window uses. */
	window = data;
	(void)version;
	compositor = strcmp(interface, "wl_compositor");
	shm = strcmp(interface, "wl_shm");
	shell = strcmp(interface, "xdg_wm_base");
	seat = strcmp(interface, "wl_seat");
	if (compositor == 0) {
		window->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4U);
	} else if (shm == 0) {
		window->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1U);
	} else if (shell == 0) {
		window->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1U);
		xdg_wm_base_add_listener(window->shell, &shell_listener, window);
	} else if (seat == 0) {
		window->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5U);
		wl_seat_add_listener(window->seat, &seat_listener, window);
	}
}

static void
registry_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Nothing is rebound. */
	(void)data;
	(void)registry;
	(void)name;
}

static void
shell_ping(
	void *data,
	struct xdg_wm_base *shell,
	uint32_t serial)
{
	/* The compositor asks whether the client is alive. */
	(void)data;
	xdg_wm_base_pong(shell, serial);
}

static void
role_configure(
	void *data,
	struct xdg_surface *role,
	uint32_t serial)
{
	struct window *window;

	/* Acknowledged; the window keeps its own size. */
	window = data;
	xdg_surface_ack_configure(role, serial);
	window->configured = 1;
}

static void
toplevel_configure(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height,
	struct wl_array *states)
{
	/* The window's size is its own (a configured size is not followed). */
	(void)data;
	(void)toplevel;
	(void)width;
	(void)height;
	(void)states;
}

static void
toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct window *window;

	/* The compositor asks the window to go. */
	window = data;
	(void)toplevel;
	window->closed = 1;
}

static void
buffer_release(
	void *data,
	struct wl_buffer *buffer)
{
	struct shm_buffer *shm;

	/* The compositor has copied the buffer; it may be drawn again. */
	shm = data;
	(void)buffer;
	shm->busy = 0;
}

static void
frame_done(
	void *data,
	struct wl_callback *callback,
	uint32_t time)
{
	struct window *window;

	/* The frame was shown; the next may be drawn. */
	window = data;
	(void)time;
	wl_callback_destroy(callback);
	window->frame_done = 1;
}

static void
seat_capabilities(
	void *data,
	struct wl_seat *seat,
	uint32_t capabilities)
{
	struct window *window;

	/* The pointer, once, when there is one. */
	window = data;
	if ((capabilities & 1U) != 0U && window->pointer == NULL) {
		window->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(window->pointer, &pointer_listener, window);
	}
}

static void
seat_name(
	void *data,
	struct wl_seat *seat,
	const char *name)
{
	/* Not used. */
	(void)data;
	(void)seat;
	(void)name;
}

static void
pointer_enter(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct window *window;

	/* The window's own cursor, or none. */
	window = data;
	(void)surface;
	printf("WLSHM ENTER run=%s x=%d y=%d\n", window->token, x / 256, y / 256);
	fflush(stdout);
	if (window->hide_cursor) {
		wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
	} else if (window->cursor) {
		wl_surface_attach(window->cursor_surface, window->cursor_buffer, 0, 0);
		wl_surface_damage_buffer(window->cursor_surface, 0, 0, 8, 8);
		wl_surface_commit(window->cursor_surface);
		wl_pointer_set_cursor(pointer, serial, window->cursor_surface, 4, 4);
	}

	/* Sent at once. */
	wl_display_flush(window->display);
}

static void
pointer_leave(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface)
{
	/* Not used. */
	(void)data;
	(void)pointer;
	(void)serial;
	(void)surface;
}

static void
pointer_motion(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	wl_fixed_t x,
	wl_fixed_t y)
{
	/* Not used. */
	(void)data;
	(void)pointer;
	(void)time;
	(void)x;
	(void)y;
}

static void
pointer_button(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	/* Not used. */
	(void)data;
	(void)pointer;
	(void)serial;
	(void)time;
	(void)button;
	(void)state;
}

static void
pointer_axis(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	uint32_t axis,
	wl_fixed_t value)
{
	/* Not used. */
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
	(void)value;
}
