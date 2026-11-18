/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests the compositor's xdg_popup, xdg_positioner and xdg_toplevel requests
 * (WS035 p076) the way a toolkit's menus and its own title bar use them.
 *
 * The window (dark, drawn with wl_shm) opens a menu popup (blue) where it
 * is pressed, with the seat's grab.  A press in the menu's lower half opens
 * a submenu (orange) beside it, also grabbing; a press in the submenu
 * chooses an item and closes both.  A popup told popup_done is destroyed.
 * A press in the window's top 30 rows starts a move (xdg_toplevel.move); in
 * its bottom-right corner or its left edge, a resize (xdg_toplevel.resize);
 * a right press asks for the window menu.  The window is drawn at the size
 * each configure gives, within its limits (200x150 to 800x600).
 *
 * The window asks for the compositor's titlebar (kl_titlebar, an explicit
 * server-side decoration) before its first commit, as the native
 * applications do; without it the compositor leaves the decoration to the client
 * (ws114-p007) and the tests that use the titlebar's buttons and corners
 * have none.  --csd leaves it out (ws099-p023).
 *
 * Keys: m maximizes, u unmaximizes, n minimizes the window; r places the
 * open menu again (xdg_popup.reposition); p stops answering pings, and
 * answers the last one when pressed again.  Every event is one line:
 * POPUPPROBE <what> ..., with the surface named window, menu or submenu.
 *
 *   popup-probe [--wide] [--wm-probe] [--csd] [--request-delay-ms=N] [--timeout-s=N] [--token=NAME]
 *     --csd       asks for no titlebar: the client keeps its own decoration
 *     --wide      menus 360 pixels wide, so that a submenu near the output's
 *                 right edge must flip to the left
 *     --request-delay-ms=N
 *                 answers a press in the move strip or a resize edge N ms
 *                 late (1 to 5000), as a busy client would: the move or the
 *                 resize request leaves while the pointer has gone on
 *                 (ws099-p020, BUG-125)
 *     --wm-probe  checks xdg_wm_base.destroy (WS035 p132, BUG-112) and exits:
 *                 a second binding with no xdg_surface of its own is destroyed
 *                 while the window (of the first) lives, which must not be an
 *                 error; then the first is destroyed under its window, which
 *                 must be (defunct_surfaces).  It prints WM-PROBE:PASS or
 *                 WM-PROBE:FAIL.
 */

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <keiland/keiland.h>

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

/* The window's first size and its limits, the menus' heights and rows. */
#define PROBE_WINDOW_WIDTH	400
#define PROBE_WINDOW_HEIGHT	300
#define PROBE_MIN_WIDTH		200
#define PROBE_MIN_HEIGHT	150
#define PROBE_MAX_WIDTH		800
#define PROBE_MAX_HEIGHT	600
#define PROBE_MENU_HEIGHT	180
#define PROBE_SUBMENU_HEIGHT	120
#define PROBE_ROW		30

/* The window's move strip at its top, its resize corner and its left resize edge. */
#define PROBE_MOVE_STRIP	30
#define PROBE_RESIZE_CORNER	24
#define PROBE_RESIZE_EDGE	16

/* The colours of the window, the menu, the submenu and the rows' lines. */
#define PROBE_WINDOW_COLOR	0xff2b3444U
#define PROBE_MENU_COLOR	0xff4a90e2U
#define PROBE_SUBMENU_COLOR	0xffe2a04aU
#define PROBE_LINE_COLOR	0xffffffffU

/* The left and right buttons and the keys m, n, p, r and u (evdev codes). */
#define PROBE_BUTTON_LEFT	0x110U
#define PROBE_BUTTON_RIGHT	0x111U
#define PROBE_KEY_M		50U
#define PROBE_KEY_N		49U
#define PROBE_KEY_P		25U
#define PROBE_KEY_R		19U
#define PROBE_KEY_U		22U

/* xdg_positioner's anchor, gravity and adjustment values the probe uses. */
#define PROBE_ANCHOR_TOP_LEFT		5U
#define PROBE_ANCHOR_TOP_RIGHT		7U
#define PROBE_GRAVITY_BOTTOM_RIGHT	8U
#define PROBE_ADJUST_FLIP_X		4U
#define PROBE_ADJUST_SLIDE_Y		2U
#define PROBE_ADJUST_SLIDE_X		1U

/* xdg_toplevel's resize edges the probe uses. */
#define PROBE_EDGE_LEFT			4U
#define PROBE_EDGE_BOTTOM_RIGHT		10U

/* The xdg_wm_base version the probe asks for (3 has xdg_popup.reposition). */
#define PROBE_SHELL_VERSION	3U

/* xdg_wm_base's error for a binding destroyed under its own live xdg_surfaces. */
#define PROBE_WM_ERROR_DEFUNCT_SURFACES	1U

struct probe;

/*
 * One surface the probe draws: the window or a popup, with its roles, its
 * wl_shm buffer and its mapping, its size (and the size a configure asks
 * for), and its name in the log.
 */
struct probe_surface {
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	struct xdg_popup *popup;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	size_t mapped;
	int32_t width;
	int32_t height;
	int32_t asked_width;
	int32_t asked_height;
	uint32_t color;
	int configured;
	const char *name;
	struct probe *probe;
};

/*
 * The probe's connection, its globals, its three surfaces, the pointer's
 * place on the surface it is over, the last input serial (a grab, a move
 * and a resize need it), and the ping it has not answered while pongs are
 * off.
 */
struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *shell;
	uint32_t shell_name;
	uint32_t shell_version;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct probe_surface window;
	struct probe_surface menu;
	struct probe_surface submenu;
	struct probe_surface *over;
	int32_t pointer_x;
	int32_t pointer_y;
	uint32_t serial;
	int32_t menu_width;
	int closed;
	int pong_off;
	uint32_t unanswered;
	uint32_t reposition_token;
	const char *token;
	int wm_probe;
	unsigned request_delay_ms;
	int csd;
	struct kl_titlebar *titlebar;
};

static int probe_options(int count, char **arguments, struct probe *probe, unsigned *timeout);
static int probe_connect(struct probe *probe);
static int probe_wm_check(struct probe *probe);
static int probe_attach(struct probe *probe, struct probe_surface *surface);
static void probe_release(struct probe_surface *surface);
static int probe_shared_memory(size_t size, void **map);
static struct xdg_positioner *probe_menu_positioner(struct probe *probe, int32_t x, int32_t y);
static void probe_open_menu(struct probe *probe);
static void probe_open_submenu(struct probe *probe);
static void probe_reposition_menu(struct probe *probe);
static void probe_close_popup(struct probe *probe, struct probe_surface *surface);
static void probe_window_press(struct probe *probe, uint32_t serial);
static void probe_toggle_pong(struct probe *probe);
static void probe_request_delay(struct probe *probe);
static struct probe_surface *probe_surface_of(struct probe *probe, struct wl_surface *surface);
static const char *probe_name(const struct probe_surface *surface);
static uint64_t probe_clock(void);
static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name);
static void shell_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void role_configure(void *data, struct xdg_surface *role, uint32_t serial);
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void toplevel_close(void *data, struct xdg_toplevel *toplevel);
static void popup_configure(void *data, struct xdg_popup *popup, int32_t x, int32_t y, int32_t width, int32_t height);
static void popup_done(void *data, struct xdg_popup *popup);
static void popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token);
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

/* The titlebar's events: the probe's titlebar has no controls or tabs, so it hears none. */
static const struct kl_titlebar_listener titlebar_listener = {
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* The registry's callbacks. */
static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_remove
};

/* The shell's ping. */
static const struct xdg_wm_base_listener shell_listener = {
	shell_ping
};

/* Every xdg_surface's configure. */
static const struct xdg_surface_listener role_listener = {
	role_configure
};

/* The window's toplevel. */
static const struct xdg_toplevel_listener toplevel_listener = {
	toplevel_configure,
	toplevel_close,
	NULL
};

/* The popups' (repositioned is from version 3). */
static const struct xdg_popup_listener popup_listener = {
	popup_configure,
	popup_done,
	popup_repositioned
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
		fprintf(stderr, "usage: popup-probe [--wide] [--wm-probe] [--csd] [--request-delay-ms=N] [--timeout-s=N] [--token=NAME]\n");
		return 2;
	}

	/* The connection and the window. */
	error = probe_connect(&probe);
	if (error != 0) {
		printf("POPUPPROBE FAILED run=%s setup errno=%d\n", probe.token, error);
		return 1;
	}

	/* The xdg_wm_base.destroy check runs instead of the events. */
	if (probe.wm_probe) {
		error = probe_wm_check(&probe);
		if (error != 0)
			return 1;

		/* Succeeded: both steps behaved as xdg-shell says. */
		return 0;
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
			printf("POPUPPROBE FAILED run=%s dispatch errno=%d\n", probe.token, errno);
			return 1;
		}
	}

	/* The end. */
	printf("POPUPPROBE DONE run=%s\n", probe.token);
	fflush(stdout);
	wl_display_disconnect(probe.display);

	/* Succeeded: the probe ran to its end. */
	return 0;
}

/* Reads the options: --wide, --wm-probe, --csd, --request-delay-ms=N, --timeout-s=N (default 120) and --token=NAME. */
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
	probe->menu_width = 220;
	probe->token = "probe";
	*timeout = 120U;

	/* Each option. */
	for (index = 1; index < count; index++) {
		/* Wide menus. */
		same = strcmp(arguments[index], "--wide");
		if (same == 0) {
			probe->menu_width = 360;
			continue;
		}

		/* The xdg_wm_base.destroy check. */
		same = strcmp(arguments[index], "--wm-probe");
		if (same == 0) {
			probe->wm_probe = 1;
			continue;
		}

		/* No titlebar: the window keeps the client's own decoration. */
		same = strcmp(arguments[index], "--csd");
		if (same == 0) {
			probe->csd = 1;
			continue;
		}

		/* How late a move or a resize is asked for. */
		same = strncmp(arguments[index], "--request-delay-ms=", 19);
		if (same == 0) {
			value = strtoul(arguments[index] + 19, &end, 10);
			if (*end != '\0' || value == 0UL || value > 5000UL)
				return -1;
			probe->request_delay_ms = (unsigned)value;
			continue;
		}

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

	/* The probe needs the compositor, wl_shm, the shell and a seat. */
	if (probe->compositor == NULL || probe->shm == NULL)
		return EOPNOTSUPP;
	if (probe->shell == NULL || probe->seat == NULL)
		return EOPNOTSUPP;

	/* The window: a toplevel with its limits and no parent, configured before it is drawn. */
	probe->window.probe = probe;
	probe->window.name = "window";
	probe->window.width = PROBE_WINDOW_WIDTH;
	probe->window.height = PROBE_WINDOW_HEIGHT;
	probe->window.color = PROBE_WINDOW_COLOR;
	probe->window.surface = wl_compositor_create_surface(probe->compositor);
	probe->window.role = xdg_wm_base_get_xdg_surface(probe->shell, probe->window.surface);
	xdg_surface_add_listener(probe->window.role, &role_listener, &probe->window);
	probe->window.toplevel = xdg_surface_get_toplevel(probe->window.role);
	xdg_toplevel_add_listener(probe->window.toplevel, &toplevel_listener, probe);
	xdg_toplevel_set_title(probe->window.toplevel, "popup probe");
	xdg_toplevel_set_parent(probe->window.toplevel, NULL);
	xdg_toplevel_set_min_size(probe->window.toplevel, PROBE_MIN_WIDTH, PROBE_MIN_HEIGHT);
	xdg_toplevel_set_max_size(probe->window.toplevel, PROBE_MAX_WIDTH, PROBE_MAX_HEIGHT);

	/* The compositor's titlebar, asked for before the first commit so that the first configure carries it. */
	if (!probe->csd && !probe->wm_probe) {
		probe->titlebar = kl_titlebar_create(probe->display, probe->window.toplevel, &titlebar_listener, probe);
		if (probe->titlebar == NULL)
			printf("POPUPPROBE titlebar none errno=%d\n", errno);
	}

	/* The first commit, without an image, asks for the first configure. */
	wl_surface_commit(probe->window.surface);

	/* The first configure. */
	status = wl_display_roundtrip(probe->display);
	if (status < 0 || !probe->window.configured)
		return EPROTO;

	/* Its image. */
	status = probe_attach(probe, &probe->window);
	if (status != 0)
		return status;

	/* Succeeded: the log line the test waits for. */
	printf("POPUPPROBE ready run=%s\n", probe->token);
	fflush(stdout);
	return 0;
}

/*
 * Checks xdg_wm_base.destroy with the window up (made from the first
 * binding): destroying a second binding, which made no xdg_surface, keeps
 * the connection; destroying the first under its window is a protocol error.
 */
static int
probe_wm_check(
	struct probe *probe)
{
	struct xdg_wm_base *other;
	const struct wl_interface *interface;
	uint32_t object;
	uint32_t code;
	int status;
	int error;

	/* A second binding of the shell, answering pings like the first. */
	other = wl_registry_bind(probe->registry, probe->shell_name, &xdg_wm_base_interface, probe->shell_version);
	xdg_wm_base_add_listener(other, &shell_listener, probe);
	status = wl_display_roundtrip(probe->display);
	if (status < 0) {
		printf("WM-PROBE bind failed errno=%d\nWM-PROBE:FAIL\n", errno);
		fflush(stdout);
		return EIO;
	}

	/* The second binding goes; the window of the first still lives. */
	xdg_wm_base_destroy(other);
	status = wl_display_roundtrip(probe->display);
	error = wl_display_get_error(probe->display);
	if (status < 0 || error != 0) {
		printf("WM-PROBE other-binding error=%d\nWM-PROBE:FAIL\n", error);
		fflush(stdout);
		return EPROTO;
	}

	/* The first step passed. */
	printf("WM-PROBE other-binding ok\n");
	fflush(stdout);

	/* The first binding goes under its window: the compositor must refuse. */
	xdg_wm_base_destroy(probe->shell);
	probe->shell = NULL;
	status = wl_display_roundtrip(probe->display);
	error = wl_display_get_error(probe->display);
	if (status >= 0 || error != EPROTO) {
		printf("WM-PROBE same-binding status=%d error=%d\nWM-PROBE:FAIL\n", status, error);
		fflush(stdout);
		return EPROTO;
	}

	/* The error names the xdg_wm_base and its defunct_surfaces code. */
	code = wl_display_get_protocol_error(probe->display, &interface, &object);
	if (interface == NULL || code != PROBE_WM_ERROR_DEFUNCT_SURFACES) {
		printf("WM-PROBE same-binding code=%u\nWM-PROBE:FAIL\n", code);
		fflush(stdout);
		return EPROTO;
	}

	/* The second step passed, and so the check. */
	printf("WM-PROBE same-binding error ok interface=%s code=%u\n", interface->name, code);
	printf("WM-PROBE:PASS\n");
	fflush(stdout);

	/* Succeeded: both steps behaved as xdg-shell says. */
	return 0;
}

/* Draws a surface into a new wl_shm buffer (its colour, a line every row) and commits it. */
static int
probe_attach(
	struct probe *probe,
	struct probe_surface *surface)
{
	struct wl_shm_pool *pool;
	size_t bytes;
	void *map;
	int32_t x;
	int32_t y;
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

	/* The colour, with a light line at the top of each row. */
	for (y = 0; y < surface->height; y++) {
		/* One row of pixels. */
		for (x = 0; x < surface->width; x++) {
			/* A line or the colour. */
			if (y % PROBE_ROW == 0) {
				surface->pixels[y * surface->width + x] = PROBE_LINE_COLOR;
			} else {
				surface->pixels[y * surface->width + x] = surface->color;
			}
		}
	}

	/* The buffer, attached and committed. */
	surface->buffer = wl_shm_pool_create_buffer(pool, 0, surface->width, surface->height, surface->width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	wl_surface_attach(surface->surface, surface->buffer, 0, 0);
	wl_surface_damage(surface->surface, 0, 0, surface->width, surface->height);
	wl_surface_commit(surface->surface);

	/* Succeeded: the surface is shown. */
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
	(void)snprintf(name, sizeof(name), "/popup-probe-%ld-%u", (long)getpid(), sequence);
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

/* Makes the menu's positioner: at a point of the window, below and to the right, slid back onto the output. */
static struct xdg_positioner *
probe_menu_positioner(
	struct probe *probe,
	int32_t x,
	int32_t y)
{
	struct xdg_positioner *positioner;

	/* The rules. */
	positioner = xdg_wm_base_create_positioner(probe->shell);
	xdg_positioner_set_size(positioner, probe->menu_width, PROBE_MENU_HEIGHT);
	xdg_positioner_set_anchor_rect(positioner, x, y, 1, 1);
	xdg_positioner_set_anchor(positioner, PROBE_ANCHOR_TOP_LEFT);
	xdg_positioner_set_gravity(positioner, PROBE_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(positioner, PROBE_ADJUST_SLIDE_X | PROBE_ADJUST_SLIDE_Y);

	/* Succeeded: the positioner. */
	return positioner;
}

/* Opens the menu at the pointer in the window, with the grab. */
static void
probe_open_menu(
	struct probe *probe)
{
	struct xdg_positioner *positioner;

	/* Only one menu at a time. */
	if (probe->menu.popup != NULL)
		return;

	/* Its rules: at the pointer. */
	positioner = probe_menu_positioner(probe, probe->pointer_x, probe->pointer_y);

	/* The popup, grabbing, configured by its first commit. */
	memset(&probe->menu, 0, sizeof(probe->menu));
	probe->menu.probe = probe;
	probe->menu.name = "menu";
	probe->menu.color = PROBE_MENU_COLOR;
	probe->menu.surface = wl_compositor_create_surface(probe->compositor);
	probe->menu.role = xdg_wm_base_get_xdg_surface(probe->shell, probe->menu.surface);
	xdg_surface_add_listener(probe->menu.role, &role_listener, &probe->menu);
	probe->menu.popup = xdg_surface_get_popup(probe->menu.role, probe->window.role, positioner);
	xdg_popup_add_listener(probe->menu.popup, &popup_listener, probe);
	xdg_popup_grab(probe->menu.popup, probe->seat, probe->serial);
	xdg_positioner_destroy(positioner);
	wl_surface_commit(probe->menu.surface);

	/* The log line. */
	printf("POPUPPROBE open menu at=%d,%d\n", probe->pointer_x, probe->pointer_y);
	fflush(stdout);
}

/* Opens the submenu beside the menu row the pointer is on, with the grab. */
static void
probe_open_submenu(
	struct probe *probe)
{
	struct xdg_positioner *positioner;
	int32_t row;

	/* Only one submenu at a time, and only from an open menu. */
	if (probe->submenu.popup != NULL || probe->menu.popup == NULL)
		return;

	/* Its rules: to the right of the row, its top at the row's; flipped left or slid to fit. */
	row = probe->pointer_y - probe->pointer_y % PROBE_ROW;
	positioner = xdg_wm_base_create_positioner(probe->shell);
	xdg_positioner_set_size(positioner, probe->menu_width, PROBE_SUBMENU_HEIGHT);
	xdg_positioner_set_anchor_rect(positioner, 0, row, probe->menu_width, PROBE_ROW);
	xdg_positioner_set_anchor(positioner, PROBE_ANCHOR_TOP_RIGHT);
	xdg_positioner_set_gravity(positioner, PROBE_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(positioner, PROBE_ADJUST_FLIP_X | PROBE_ADJUST_SLIDE_Y);

	/* The popup, a child of the menu, grabbing. */
	memset(&probe->submenu, 0, sizeof(probe->submenu));
	probe->submenu.probe = probe;
	probe->submenu.name = "submenu";
	probe->submenu.color = PROBE_SUBMENU_COLOR;
	probe->submenu.surface = wl_compositor_create_surface(probe->compositor);
	probe->submenu.role = xdg_wm_base_get_xdg_surface(probe->shell, probe->submenu.surface);
	xdg_surface_add_listener(probe->submenu.role, &role_listener, &probe->submenu);
	probe->submenu.popup = xdg_surface_get_popup(probe->submenu.role, probe->menu.role, positioner);
	xdg_popup_add_listener(probe->submenu.popup, &popup_listener, probe);
	xdg_popup_grab(probe->submenu.popup, probe->seat, probe->serial);
	xdg_positioner_destroy(positioner);
	wl_surface_commit(probe->submenu.surface);

	/* The log line. */
	printf("POPUPPROBE open submenu row=%d\n", row);
	fflush(stdout);
}

/* Places the open menu again at the window's point (10, 40) (xdg_popup.reposition). */
static void
probe_reposition_menu(
	struct probe *probe)
{
	struct xdg_positioner *positioner;
	uint32_t version;

	/* Only an open menu. */
	if (probe->menu.popup == NULL)
		return;

	/* Only with a shell that has reposition. */
	version = xdg_popup_get_version(probe->menu.popup);
	if (version < XDG_POPUP_REPOSITION_SINCE_VERSION)
		return;

	/* The new rules, and a token the answer names. */
	positioner = probe_menu_positioner(probe, 10, 40);
	probe->reposition_token++;
	xdg_popup_reposition(probe->menu.popup, positioner, probe->reposition_token);
	xdg_positioner_destroy(positioner);

	/* The log line. */
	printf("POPUPPROBE reposition menu token=%u\n", probe->reposition_token);
	fflush(stdout);
}

/* Destroys a popup and its surface (the submenu first when the menu goes). */
static void
probe_close_popup(
	struct probe *probe,
	struct probe_surface *surface)
{
	/* A closed popup has nothing left. */
	if (surface->popup == NULL)
		return;

	/* A menu's submenu goes before it. */
	if (surface == &probe->menu)
		probe_close_popup(probe, &probe->submenu);

	/* The popup, its role, its surface and its image. */
	xdg_popup_destroy(surface->popup);
	xdg_surface_destroy(surface->role);
	wl_surface_destroy(surface->surface);
	probe_release(surface);
	printf("POPUPPROBE close %s\n", surface->name);
	fflush(stdout);

	/* The pointer is not over it any more. */
	if (probe->over == surface)
		probe->over = NULL;
	memset(surface, 0, sizeof(*surface));
}

/*
 * Acts on a left press in the window: the top strip moves it, the
 * bottom-right corner and the left edge resize it, elsewhere the menu
 * opens.
 */
static void
probe_window_press(
	struct probe *probe,
	uint32_t serial)
{
	/* The top strip: a move. */
	if (probe->pointer_y < PROBE_MOVE_STRIP) {
		probe_request_delay(probe);
		xdg_toplevel_move(probe->window.toplevel, probe->seat, serial);
		printf("POPUPPROBE move\n");
		fflush(stdout);
		return;
	}

	/* The bottom-right corner: a resize from it. */
	if (probe->pointer_x >= probe->window.width - PROBE_RESIZE_CORNER &&
	    probe->pointer_y >= probe->window.height - PROBE_RESIZE_CORNER) {
		probe_request_delay(probe);
		xdg_toplevel_resize(probe->window.toplevel, probe->seat, serial, PROBE_EDGE_BOTTOM_RIGHT);
		printf("POPUPPROBE resize bottom-right\n");
		fflush(stdout);
		return;
	}

	/* The left edge: a resize from it. */
	if (probe->pointer_x < PROBE_RESIZE_EDGE) {
		probe_request_delay(probe);
		xdg_toplevel_resize(probe->window.toplevel, probe->seat, serial, PROBE_EDGE_LEFT);
		printf("POPUPPROBE resize left\n");
		fflush(stdout);
		return;
	}

	/* Elsewhere the menu opens. */
	probe_open_menu(probe);
}

/* Waits the --request-delay-ms time before a move or a resize is asked for, as a busy client would. */
static void
probe_request_delay(
	struct probe *probe)
{
	struct timespec delay;

	/* Without the option the request leaves at once. */
	if (probe->request_delay_ms == 0U)
		return;

	/* Nothing is read from the compositor meanwhile, so the pointer goes on without the probe. */
	delay.tv_sec = (time_t)(probe->request_delay_ms / 1000U);
	delay.tv_nsec = (long)(probe->request_delay_ms % 1000U) * 1000000L;
	(void)nanosleep(&delay, NULL);
	printf("POPUPPROBE delayed ms=%u\n", probe->request_delay_ms);
	fflush(stdout);
}

/* Stops answering pings, or starts again and answers the last one. */
static void
probe_toggle_pong(
	struct probe *probe)
{
	/* Off: pings are only remembered. */
	if (!probe->pong_off) {
		probe->pong_off = 1;
		printf("POPUPPROBE pong off\n");
		fflush(stdout);
		return;
	}

	/* On again: the ping left unanswered is answered now. */
	probe->pong_off = 0;
	if (probe->unanswered != 0U)
		xdg_wm_base_pong(probe->shell, probe->unanswered);
	printf("POPUPPROBE pong on answered=%u\n", probe->unanswered);
	fflush(stdout);
	probe->unanswered = 0;
}

/* Finds the probe's surface of a wl_surface (NULL for none of them). */
static struct probe_surface *
probe_surface_of(
	struct probe *probe,
	struct wl_surface *surface)
{
	/* No surface. */
	if (surface == NULL)
		return NULL;

	/* The window, the menu or the submenu. */
	if (surface == probe->window.surface)
		return &probe->window;
	if (surface == probe->menu.surface)
		return &probe->menu;
	if (surface == probe->submenu.surface)
		return &probe->submenu;

	/* None of them (one the probe has destroyed). */
	return NULL;
}

/* Names a surface in the log: its name, or "other" for none of the probe's. */
static const char *
probe_name(
	const struct probe_surface *surface)
{
	/* None of the probe's. */
	if (surface == NULL)
		return "other";

	/* Succeeded: the surface's name. */
	return surface->name;
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
	uint32_t shell_version;
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

	/* The shell, at version 3 when the compositor has it. */
	same = strcmp(interface, "xdg_wm_base");
	if (same == 0) {
		shell_version = version;
		if (shell_version > PROBE_SHELL_VERSION)
			shell_version = PROBE_SHELL_VERSION;
		probe->shell_name = name;
		probe->shell_version = shell_version;
		probe->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, shell_version);
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

/* Answers the shell's ping, unless pongs are off (the ping is then remembered). */
static void
shell_ping(
	void *data,
	struct xdg_wm_base *shell,
	uint32_t serial)
{
	struct probe *probe;

	/* Pongs off: the ping waits. */
	probe = data;
	if (probe->pong_off) {
		probe->unanswered = serial;
		printf("POPUPPROBE ping serial=%u ignored\n", serial);
		fflush(stdout);
		return;
	}

	/* The answer. */
	xdg_wm_base_pong(shell, serial);
	printf("POPUPPROBE ping serial=%u answered\n", serial);
	fflush(stdout);
}

/* Acknowledges a configure: a popup draws its first image, the window draws at a new size. */
static void
role_configure(
	void *data,
	struct xdg_surface *role,
	uint32_t serial)
{
	struct probe_surface *surface;
	int first;

	/* The acknowledgement. */
	surface = data;
	xdg_surface_ack_configure(role, serial);
	first = !surface->configured;
	surface->configured = 1;

	/* A popup draws itself after its first configure (the window does in probe_connect). */
	if (surface->popup != NULL) {
		if (first && surface->buffer == NULL)
			(void)probe_attach(surface->probe, surface);
		return;
	}

	/* The window keeps its size when the configure gives none, or the same. */
	if (surface->asked_width <= 0 || surface->asked_height <= 0)
		return;
	if (surface->asked_width == surface->width && surface->asked_height == surface->height)
		return;

	/* Succeeded: the window is drawn at the new size. */
	surface->width = surface->asked_width;
	surface->height = surface->asked_height;
	if (surface->buffer != NULL)
		(void)probe_attach(surface->probe, surface);
}

/* Notes the size the window is given (0 lets it keep its own), and logs it with the states. */
static void
toplevel_configure(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height,
	struct wl_array *states)
{
	struct probe *probe;
	const uint32_t *state;
	size_t index;
	int resizing;

	UNUSED_PARAMETER(toplevel);

	/* Whether the states say the window is being resized. */
	probe = data;
	resizing = 0;
	state = states->data;
	for (index = 0; index < states->size / sizeof(uint32_t); index++) {
		/* The resizing state. */
		if (state[index] == XDG_TOPLEVEL_STATE_RESIZING)
			resizing = 1;
	}

	/* The size, drawn once the configure is acknowledged. */
	probe->window.asked_width = width;
	probe->window.asked_height = height;
	printf("POPUPPROBE configure window width=%d height=%d states=%u resizing=%d\n", width, height, (unsigned)(states->size / sizeof(uint32_t)), resizing);
	fflush(stdout);
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

/* Keeps a popup's size and logs its place relative to its parent. */
static void
popup_configure(
	void *data,
	struct xdg_popup *popup,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	struct probe *probe;
	struct probe_surface *surface;

	/* The popup's surface. */
	probe = data;
	surface = &probe->menu;
	if (popup == probe->submenu.popup)
		surface = &probe->submenu;

	/* The size it is drawn at. */
	surface->width = width;
	surface->height = height;
	surface->probe = probe;
	printf("POPUPPROBE configure %s x=%d y=%d width=%d height=%d\n", surface->name, x, y, width, height);
	fflush(stdout);
}

/* Destroys a popup the compositor closed (the submenu first, when the menu is closed). */
static void
popup_done(
	void *data,
	struct xdg_popup *popup)
{
	struct probe *probe;

	/* Which popup. */
	probe = data;
	if (popup == probe->submenu.popup) {
		printf("POPUPPROBE done submenu\n");
		probe_close_popup(probe, &probe->submenu);
	} else if (popup == probe->menu.popup) {
		printf("POPUPPROBE done menu\n");
		probe_close_popup(probe, &probe->menu);
	}

	/* The log lines go out now. */
	fflush(stdout);
}

/* Logs the answer to a reposition (its configure follows). */
static void
popup_repositioned(
	void *data,
	struct xdg_popup *popup,
	uint32_t token)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(popup);

	/* The log line. */
	printf("POPUPPROBE repositioned token=%u\n", token);
	fflush(stdout);
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

/* Notes which surface the pointer is over, and where. */
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

	/* The surface and the place. */
	probe = data;
	probe->serial = serial;
	probe->over = probe_surface_of(probe, surface);
	probe->pointer_x = wl_fixed_to_int(x);
	probe->pointer_y = wl_fixed_to_int(y);
	printf("POPUPPROBE enter %s x=%d y=%d\n", probe_name(probe->over), probe->pointer_x, probe->pointer_y);
	fflush(stdout);
}

/* Notes that the pointer left a surface. */
static void
pointer_leave(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct probe *probe;
	struct probe_surface *left;

	UNUSED_PARAMETER(pointer);
	UNUSED_PARAMETER(serial);

	/* The surface left. */
	probe = data;
	left = probe_surface_of(probe, surface);
	printf("POPUPPROBE leave %s\n", probe_name(left));
	fflush(stdout);

	/* The pointer is over none of the probe's surfaces now. */
	if (probe->over == left)
		probe->over = NULL;
}

/* Follows the pointer on the surface it is over. */
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

	/* The place. */
	probe = data;
	probe->pointer_x = wl_fixed_to_int(x);
	probe->pointer_y = wl_fixed_to_int(y);
}

/* Opens and chooses from the menus, or starts a move or a resize, by where a button is pressed. */
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
	UNUSED_PARAMETER(time);

	/* The log line; a press's serial is kept for a grab. */
	probe = data;
	probe->serial = serial;
	printf("POPUPPROBE button %s state=%u x=%d y=%d\n", probe_name(probe->over), state, probe->pointer_x, probe->pointer_y);
	fflush(stdout);

	/* Only a press on one of the probe's surfaces acts. */
	if (state == 0U || probe->over == NULL)
		return;

	/* A right press in the window asks for the window menu. */
	if (button == PROBE_BUTTON_RIGHT && probe->over == &probe->window) {
		xdg_toplevel_show_window_menu(probe->window.toplevel, probe->seat, serial, probe->pointer_x, probe->pointer_y);
		printf("POPUPPROBE window-menu\n");
		fflush(stdout);
		return;
	}

	/* Only the left button acts otherwise. */
	if (button != PROBE_BUTTON_LEFT)
		return;

	/* In the window. */
	if (probe->over == &probe->window) {
		probe_window_press(probe, serial);
		return;
	}

	/* In the menu's lower half: the submenu opens; in its upper half an item is chosen. */
	if (probe->over == &probe->menu) {
		if (probe->pointer_y >= PROBE_MENU_HEIGHT / 2) {
			probe_open_submenu(probe);
			return;
		}

		/* An item of the menu. */
		printf("POPUPPROBE choose menu row=%d\n", probe->pointer_y / PROBE_ROW);
		probe_close_popup(probe, &probe->menu);
		return;
	}

	/* In the submenu: an item is chosen and both close. */
	printf("POPUPPROBE choose submenu row=%d\n", probe->pointer_y / PROBE_ROW);
	probe_close_popup(probe, &probe->menu);
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
	struct probe_surface *entered;

	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(serial);
	UNUSED_PARAMETER(keys);

	/* The surface. */
	probe = data;
	entered = probe_surface_of(probe, surface);
	printf("POPUPPROBE focus %s\n", probe_name(entered));
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
	struct probe_surface *left;

	UNUSED_PARAMETER(keyboard);
	UNUSED_PARAMETER(serial);

	/* The surface. */
	probe = data;
	left = probe_surface_of(probe, surface);
	printf("POPUPPROBE unfocus %s\n", probe_name(left));
	fflush(stdout);
}

/* Logs a key; m, u and n maximize, unmaximize and minimize the window, r repositions the menu, p toggles pongs. */
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

	/* The log line; only a press acts. */
	probe = data;
	printf("POPUPPROBE key %u state=%u\n", key, state);
	fflush(stdout);
	if (state == 0U)
		return;

	/* The window's and the menu's requests by the key. */
	switch (key) {
	case PROBE_KEY_M:
		xdg_toplevel_set_maximized(probe->window.toplevel);
		break;
	case PROBE_KEY_U:
		xdg_toplevel_unset_maximized(probe->window.toplevel);
		break;
	case PROBE_KEY_N:
		xdg_toplevel_set_minimized(probe->window.toplevel);
		break;
	case PROBE_KEY_R:
		probe_reposition_menu(probe);
		break;
	case PROBE_KEY_P:
		probe_toggle_pong(probe);
		break;
	default:
		break;
	}
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
