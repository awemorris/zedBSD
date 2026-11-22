/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's backend of the core: the Wayland desktop, through standard
 * Wayland (xdg-shell windows, wl_shm buffers, wl_seat input).
 *
 * Each top-level X window has a surface, an xdg toplevel, which the
 * desktop places and decorates.  The core shows its pixels through a
 * Vulkan swapchain on it (the Vulkan surface made here), or hands them
 * to this backend's present: they are then copied into one of its two
 * wl_shm buffers, the one the desktop has released, and committed with
 * the changed rectangles as damage.  The pointer's events become pointer
 * frames on the root window (the place in the window plus the window's
 * corner there), and the keyboard's evdev codes become X keycodes.
 *
 * The clipboard: the seat's data device tells the core when another
 * client's text becomes the selection (the core then owns X's CLIPBOARD
 * for it and reads the text when an X client asks); while an X client
 * owns CLIPBOARD the backend offers its text as a data source, and the
 * desktop's requests for it are handed to the core, numbered by their
 * descriptors, which the core's answer writes and closes.
 *
 * The primary selection (ws035-p103) is bridged the same way with X's
 * PRIMARY, through the desktop's zwp_primary_selection_device_manager_v1
 * when it has one.
 */

#include "userland/desktop/xserver/program.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <primary-selection-unstable-v1-client-protocol.h>
#include <keiland/keiland.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>

/* The evdev codes of the pointer's buttons, and the key that toggles caps lock. */
#define WAYLAND_BTN_LEFT	0x110U
#define WAYLAND_BTN_RIGHT	0x111U
#define WAYLAND_BTN_MIDDLE	0x112U
#define WAYLAND_KEY_CAPSLOCK	58U

/* How many evdev key codes are remembered for their releases. */
#define WAYLAND_KEYS		256U

/* The modifier bits of wl_keyboard.modifiers kept (the same bits as X's). */
#define WAYLAND_MODIFIERS	(X11_SHIFT_MASK | X11_CONTROL_MASK | X11_ALT_MASK)

/* The application's identity on the desktop. */
#define WAYLAND_APP_ID		"xserver"

/* The text types the clipboard offers and takes, the one read first, and how long a read may wait (milliseconds) and hold. */
#define WAYLAND_TEXT_UTF8	"text/plain;charset=utf-8"
#define WAYLAND_TEXT_PLAIN	"text/plain"
#define WAYLAND_TEXT_X11	"UTF8_STRING"
#define WAYLAND_TEXT_STRING	"STRING"
#define WAYLAND_READ_MS		2000
#define WAYLAND_READ_MAX	(1024U * 1024U)

/*
 * One wl_shm buffer of a window, and whether the desktop holds it.
 */
struct wayland_buffer {
	struct wl_buffer *buffer;
	uint32_t *pixels;
	int busy;
};

/*
 * One surface, a desktop window: an X window's xdg toplevel, its two
 * buffers in one shared-memory mapping (made when the core's present
 * first needs them), and its corner on the root window (for the pointer's
 * places).
 */
struct x11_surface {
	/* The connection it belongs to, and the X window it shows. */
	struct x11_wayland *wayland;
	uint32_t id;

	/* The surface and its roles; configured once the desktop has sent the first configure. */
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	int configured;

	/*
	 * The compositor's titlebar for the X window: an X client draws no
	 * decoration of its own, so the desktop window asks for the
	 * compositor's (NULL from a compositor without it; ws099-p023,
	 * BUG-136).
	 */
	struct kl_titlebar *titlebar;

	/* The window's size, and the buffers' shared memory (NULL: none made yet) and the buffers. */
	unsigned width;
	unsigned height;
	void *memory;
	size_t memory_size;
	struct wayland_buffer buffers[2];

	/* Where the X window's corner is on the root window. */
	int origin_x;
	int origin_y;

	/*
	 * The largest size the desktop lets the window choose (xdg-shell 4;
	 * zero: not known), and the size the window is to take within them,
	 * which the next dispatch hands to the server (zero: none waiting).
	 * It waits for the dispatch because the configure that asks for it
	 * may come while the window is being opened, before the server holds it.
	 */
	unsigned bounds_width;
	unsigned bounds_height;
	unsigned pending_width;
	unsigned pending_height;

	/* The next window of the connection. */
	struct x11_surface *next;
};

/*
 * The connection: the globals, the windows, the core it tells, and
 * the input state that turns Wayland's events into the server's.
 */
struct x11_wayland {
	/* The connection and the globals bound from it. */
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;

	/* The pointer's and the keyboard's listeners (only the events of version 1 are filled in). */
	struct wl_pointer_listener pointer_listener;
	struct wl_keyboard_listener keyboard_listener;

	/* The windows open. */
	struct x11_surface *windows;

	/* The core told what comes (NULL until it is attached: nothing is told). */
	struct x11server *server;

	/* The window the pointer is in, where it is on the root window, and the buttons held (X's bits). */
	struct x11_surface *pointer_window;
	int pointer_x;
	int pointer_y;
	uint16_t buttons;

	/* The modifiers held and caps lock. */
	uint32_t modifiers;
	int caps_lock;

	/* The X keycode each evdev key was pressed as, so its release says the same. */
	uint8_t keycodes[WAYLAND_KEYS];

	/*
	 * The clipboard (ws035-p087): the data device manager and the seat's
	 * device (NULL without them), the server's own source while an X
	 * client's text is the selection (NULL otherwise), the offer
	 * introduced last and whether it has text, the selection's offer from
	 * another client (NULL for none) and whether it has text, and the
	 * serial of the last input (a selection is set with it).
	 */
	struct wl_data_device_manager *data_manager;
	struct wl_data_device *data_device;
	struct wl_data_source *source;
	struct wl_data_offer *offer_new;
	int offer_new_text;
	struct wl_data_offer *selection;
	int selection_text;
	uint32_t serial;

	/*
	 * The primary selection (ws035-p103), the same as the clipboard's:
	 * the manager and the seat's device (NULL without them), the server's
	 * own source, the offer introduced last and whether it has text, and
	 * another client's selection and whether it has text.
	 */
	struct zwp_primary_selection_device_manager_v1 *primary_manager;
	struct zwp_primary_selection_device_v1 *primary_device;
	struct zwp_primary_selection_source_v1 *primary_source;
	struct zwp_primary_selection_offer_v1 *primary_new;
	int primary_new_text;
	struct zwp_primary_selection_offer_v1 *primary;
	int primary_text;
};

static struct x11_surface *wayland_open(void *context, uint32_t id, const struct x11_surface_info *info);
static void wayland_update(void *context, struct x11_surface *window, const struct x11_surface_info *info, unsigned changed);
static void wayland_close(void *context, struct x11_surface *window);
static int wayland_vulkan_surface(void *context, struct x11_surface *window, void *instance, void *result);
static int wayland_present(void *context, struct x11_surface *window, const uint32_t *pixels, size_t stride, const struct x11_rect *parts, unsigned count);
static int wayland_clipboard_own(void *context, unsigned selection, int own);
static int wayland_clipboard_has_text(void *context, unsigned selection);
static int wayland_clipboard_read(void *context, unsigned selection, uint8_t **text, size_t *size);
static size_t wayland_clipboard_answer(void *context, uint32_t request, const uint8_t *text, size_t size);
static int wayland_selection_own(struct x11_wayland *wayland);
static void wayland_selection_drop(struct x11_wayland *wayland);
static int wayland_selection_has_text(const struct x11_wayland *wayland);
static int wayland_selection_read(struct x11_wayland *wayland, char **text, size_t *length);
static int wayland_primary_own(struct x11_wayland *wayland);
static void wayland_primary_drop(struct x11_wayland *wayland);
static int wayland_primary_has_text(const struct x11_wayland *wayland);
static int wayland_primary_read(struct x11_wayland *wayland, char **text, size_t *length);
static int wayland_buffers(struct x11_surface *window);
static void wayland_buffers_free(struct x11_surface *window);
static struct x11_surface *wayland_window_of(struct x11_wayland *wayland, struct wl_surface *surface);
static void wayland_pointer_frame(struct x11_wayland *wayland, uint32_t time, uint8_t button, int pressed, uint16_t bit);
static void wayland_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void wayland_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void wayland_ping(void *data, struct xdg_wm_base *shell, uint32_t serial);
static void wayland_configure(void *data, struct xdg_surface *surface, uint32_t serial);
static void wayland_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
static void wayland_toplevel_close(void *data, struct xdg_toplevel *toplevel);
static void wayland_toplevel_bounds(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height);
static void wayland_pending_sizes(struct x11_wayland *wayland);
static void wayland_bounded_size(struct x11_surface *window);
static void wayland_buffer_release(void *data, struct wl_buffer *buffer);
static void wayland_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities);
static void wayland_seat_name(void *data, struct wl_seat *seat, const char *name);
static void wayland_pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y);
static void wayland_pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface);
static void wayland_pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void wayland_pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
static void wayland_pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value);
static void wayland_keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size);
static void wayland_keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys);
static void wayland_keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface);
static void wayland_keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void wayland_keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
static void wayland_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer);
static void wayland_data_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer);
static void wayland_data_leave(void *data, struct wl_data_device *device);
static void wayland_data_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y);
static void wayland_data_drop(void *data, struct wl_data_device *device);
static void wayland_data_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer);
static void wayland_offer_type(void *data, struct wl_data_offer *offer, const char *mime_type);
static void wayland_source_target(void *data, struct wl_data_source *source, const char *mime_type);
static void wayland_source_send(void *data, struct wl_data_source *source, const char *mime_type, int32_t fd);
static void wayland_source_cancelled(void *data, struct wl_data_source *source);
static int wayland_text_type(const char *mime_type);
static int wayland_pipe_read(int fd, char **text, size_t *length);
static void wayland_primary_offer(void *data, struct zwp_primary_selection_device_v1 *device, struct zwp_primary_selection_offer_v1 *offer);
static void wayland_primary_selection(void *data, struct zwp_primary_selection_device_v1 *device, struct zwp_primary_selection_offer_v1 *offer);
static void wayland_primary_type(void *data, struct zwp_primary_selection_offer_v1 *offer, const char *mime_type);
static void wayland_primary_send(void *data, struct zwp_primary_selection_source_v1 *source, const char *mime_type, int32_t fd);
static void wayland_primary_cancelled(void *data, struct zwp_primary_selection_source_v1 *source);

/*
 * The backend's functions, which the core calls with the connection as
 * their context.
 */
const struct x11_backend_ops x11_wayland_ops = {
	wayland_open, wayland_update, wayland_close, wayland_vulkan_surface, wayland_present, wayland_clipboard_own, wayland_clipboard_has_text,
	wayland_clipboard_read, wayland_clipboard_answer
};

/* The registry's callbacks. */
static const struct wl_registry_listener wayland_registry_listener = {
	wayland_global, wayland_global_remove
};

/* The shell's liveness check. */
static const struct xdg_wm_base_listener wayland_shell_listener = {
	wayland_ping
};

/* A window role's configure. */
static const struct xdg_surface_listener wayland_surface_listener = {
	wayland_configure
};

/* The titlebar's events: an X window's titlebar has no controls or tabs, so it hears none. */
static const struct kl_titlebar_listener wayland_titlebar_listener = {
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* A toplevel's size, close request and bounds. */
static const struct xdg_toplevel_listener wayland_toplevel_listener = {
	wayland_toplevel_configure, wayland_toplevel_close, wayland_toplevel_bounds
};

/* A buffer given back by the desktop. */
static const struct wl_buffer_listener wayland_buffer_listener = {
	wayland_buffer_release
};

/* The seat's devices. */
static const struct wl_seat_listener wayland_seat_listener = {
	wayland_seat_capabilities, wayland_seat_name
};

/* The seat's data device: the selection (drags over X windows are not taken). */
static const struct wl_data_device_listener wayland_data_listener = {
	wayland_data_offer, wayland_data_enter, wayland_data_leave, wayland_data_motion, wayland_data_drop, wayland_data_selection
};

/* An offer's types (its actions are not used). */
static const struct wl_data_offer_listener wayland_offer_listener = {
	wayland_offer_type, NULL, NULL
};

/* The server's source of an X client's text. */
static const struct wl_data_source_listener wayland_source_listener = {
	wayland_source_target, wayland_source_send, wayland_source_cancelled, NULL, NULL, NULL
};

/* The seat's primary selection device, an offer's types, and the server's source (ws035-p103). */
static const struct zwp_primary_selection_device_v1_listener wayland_primary_device_listener = {
	wayland_primary_offer, wayland_primary_selection
};
static const struct zwp_primary_selection_offer_v1_listener wayland_primary_offer_listener = {
	wayland_primary_type
};
static const struct zwp_primary_selection_source_v1_listener wayland_primary_source_listener = {
	wayland_primary_send, wayland_primary_cancelled
};

/*
 * Connects to the desktop and binds the globals windows need; what the
 * desktop says is told to the core once it is attached.  Returns 0, or an
 * errno value.
 */
int
x11_wayland_open(
	struct x11_wayland **result,
	const char *display)
{
	struct x11_wayland *wayland;
	int status;

	/* The connection's state. */
	*result = NULL;
	wayland = calloc(1U, sizeof(*wayland));
	if (wayland == NULL)
		return ENOMEM;

	/* The pointer's events of version 1 (the listener has later members, left empty). */
	wayland->pointer_listener.enter = wayland_pointer_enter;
	wayland->pointer_listener.leave = wayland_pointer_leave;
	wayland->pointer_listener.motion = wayland_pointer_motion;
	wayland->pointer_listener.button = wayland_pointer_button;
	wayland->pointer_listener.axis = wayland_pointer_axis;

	/* The keyboard's events of version 1 (no repeat information). */
	wayland->keyboard_listener.keymap = wayland_keyboard_keymap;
	wayland->keyboard_listener.enter = wayland_keyboard_enter;
	wayland->keyboard_listener.leave = wayland_keyboard_leave;
	wayland->keyboard_listener.key = wayland_keyboard_key;
	wayland->keyboard_listener.modifiers = wayland_keyboard_modifiers;

	/* The connection. */
	wayland->display = wl_display_connect(display);
	if (wayland->display == NULL) {
		x11_wayland_close(wayland);
		return ECONNREFUSED;
	}

	/* The registry, whose globals are bound as they are announced. */
	wayland->registry = wl_display_get_registry(wayland->display);
	if (wayland->registry == NULL) {
		x11_wayland_close(wayland);
		return ENOMEM;
	}

	/* Its globals are bound as they are announced. */
	(void)wl_registry_add_listener(wayland->registry, &wayland_registry_listener, wayland);
	status = wl_display_roundtrip(wayland->display);
	if (status < 0) {
		x11_wayland_close(wayland);
		return EIO;
	}

	/* The seat's data device for the clipboard, when the desktop has one. */
	if (wayland->data_manager != NULL && wayland->seat != NULL) {
		wayland->data_device = wl_data_device_manager_get_data_device(wayland->data_manager, wayland->seat);
		if (wayland->data_device != NULL)
			(void)wl_data_device_add_listener(wayland->data_device, &wayland_data_listener, wayland);
	}

	/* The seat's primary selection device, when the desktop has one. */
	if (wayland->primary_manager != NULL && wayland->seat != NULL) {
		wayland->primary_device = zwp_primary_selection_device_manager_v1_get_device(wayland->primary_manager, wayland->seat);
		if (wayland->primary_device != NULL)
			(void)zwp_primary_selection_device_v1_add_listener(wayland->primary_device, &wayland_primary_device_listener, wayland);
	}

	/* Windows need a compositor, shared memory and a shell. */
	if (wayland->compositor == NULL ||
	    wayland->shm == NULL ||
	    wayland->shell == NULL) {
		x11_wayland_close(wayland);
		return EOPNOTSUPP;
	}

	/* Succeeded: windows can be opened. */
	*result = wayland;
	return 0;
}

/*
 * Gives the connection the core it tells what the desktop says (the core
 * is made with this backend, so it comes after the connection).
 */
void
x11_wayland_attach(
	struct x11_wayland *wayland,
	struct x11server *server)
{
	/* The core from now on. */
	wayland->server = server;
}

/*
 * Returns the descriptor the server waits on for the desktop's events.
 */
int
x11_wayland_fd(
	const struct x11_wayland *wayland)
{
	int descriptor;

	/* The connection's socket. */
	descriptor = wl_display_get_fd(wayland->display);

	/* Succeeded: the descriptor. */
	return descriptor;
}

/*
 * Reads the desktop's events when its descriptor is readable, runs those
 * read (what they tell the core among them), and sends the requests they
 * made.  Returns 0, or -1 when the connection is lost.
 */
int
x11_wayland_dispatch(
	struct x11_wayland *wayland,
	int readable)
{
	int status;

	/*
	 * The events that came, read without waiting: Vulkan's presentation
	 * reads the same connection for its own queue, so what made the
	 * descriptor readable may already have been taken, and a blocking
	 * dispatch would then hold every client until the desktop spoke again.
	 */
	if (readable) {
		status = wl_display_prepare_read(wayland->display);
		while (status != 0) {
			status = wl_display_dispatch_pending(wayland->display);
			if (status < 0)
				return -1;
			status = wl_display_prepare_read(wayland->display);
		}

		/* Whatever is there now (nothing is fine). */
		status = wl_display_read_events(wayland->display);
		if (status < 0)
			return -1;
	}

	/* Events already read (by a round trip, say), and the requests made while running them. */
	status = wl_display_dispatch_pending(wayland->display);
	if (status < 0)
		return -1;

	/* The sizes windows are to take within their bounds, handed to the server. */
	wayland_pending_sizes(wayland);
	(void)wl_display_flush(wayland->display);

	/* Succeeded: every event so far has run. */
	return 0;
}

/*
 * Closes every window and disconnects.
 */
void
x11_wayland_close(
	struct x11_wayland *wayland)
{
	/* The primary selection's objects. */
	if (wayland->primary_source != NULL)
		zwp_primary_selection_source_v1_destroy(wayland->primary_source);
	if (wayland->primary != NULL)
		zwp_primary_selection_offer_v1_destroy(wayland->primary);
	if (wayland->primary_device != NULL)
		zwp_primary_selection_device_v1_destroy(wayland->primary_device);
	if (wayland->primary_manager != NULL)
		zwp_primary_selection_device_manager_v1_destroy(wayland->primary_manager);

	/* The clipboard's objects. */
	if (wayland->source != NULL)
		wl_data_source_destroy(wayland->source);
	if (wayland->selection != NULL)
		wl_data_offer_destroy(wayland->selection);
	if (wayland->data_device != NULL)
		wl_data_device_destroy(wayland->data_device);
	if (wayland->data_manager != NULL)
		wl_data_device_manager_destroy(wayland->data_manager);

	/* The windows the core left open. */
	while (wayland->windows != NULL)
		wayland_close(wayland, wayland->windows);

	/* The input devices. */
	if (wayland->keyboard != NULL)
		wl_keyboard_destroy(wayland->keyboard);
	if (wayland->pointer != NULL)
		wl_pointer_destroy(wayland->pointer);
	if (wayland->seat != NULL)
		wl_seat_destroy(wayland->seat);

	/* The globals and the connection. */
	if (wayland->shell != NULL)
		xdg_wm_base_destroy(wayland->shell);
	if (wayland->shm != NULL)
		wl_shm_destroy(wayland->shm);
	if (wayland->compositor != NULL)
		wl_compositor_destroy(wayland->compositor);
	if (wayland->registry != NULL)
		wl_registry_destroy(wayland->registry);
	if (wayland->display != NULL)
		wl_display_disconnect(wayland->display);

	/* The state itself. */
	free(wayland);
}

/*
 * Opens a surface for an X window: an xdg toplevel with its title and an
 * application ID (the X client's class, else the server's own), with the
 * compositor's titlebar, configured.  Its buffers are made when the
 * core's present first needs them.  Returns NULL when it cannot be made.
 */
static struct x11_surface *
wayland_open(
	void *context,
	uint32_t id,
	const struct x11_surface_info *info)
{
	struct x11_wayland *wayland;
	struct x11_surface *window;
	const char *app_id;
	int status;

	/* The window, at the head of the connection's list, at its X window's size and place. */
	wayland = context;
	window = calloc(1U, sizeof(*window));
	if (window == NULL)
		return NULL;
	window->wayland = wayland;
	window->id = id;
	window->width = info->width;
	window->height = info->height;
	window->origin_x = info->x;
	window->origin_y = info->y;
	window->next = wayland->windows;
	wayland->windows = window;

	/* The surface. */
	window->surface = wl_compositor_create_surface(wayland->compositor);
	if (window->surface == NULL) {
		wayland_close(wayland, window);
		return NULL;
	}

	/* Its window role, whose configures are heard. */
	window->role = xdg_wm_base_get_xdg_surface(wayland->shell, window->surface);
	if (window->role == NULL) {
		wayland_close(wayland, window);
		return NULL;
	}

	/* Its configures are heard. */
	(void)xdg_surface_add_listener(window->role, &wayland_surface_listener, window);

	/* The toplevel, whose sizes and close requests are heard. */
	window->toplevel = xdg_surface_get_toplevel(window->role);
	if (window->toplevel == NULL) {
		wayland_close(wayland, window);
		return NULL;
	}

	/* Its sizes and close requests are heard. */
	(void)xdg_toplevel_add_listener(window->toplevel, &wayland_toplevel_listener, window);

	/* The title, the identity (the X client's class, else the server's), and the first configure waited for. */
	xdg_toplevel_set_title(window->toplevel, info->title);
	app_id = info->class_name;
	if (app_id == NULL)
		app_id = WAYLAND_APP_ID;
	xdg_toplevel_set_app_id(window->toplevel, app_id);

	/* The compositor's titlebar, asked for before the first commit so that the first configure carries it. */
	window->titlebar = kl_titlebar_create(wayland->display, window->toplevel, &wayland_titlebar_listener, window);
	if (window->titlebar == NULL)
		fprintf(stderr, "X11SERVER TITLEBAR none errno=%d\n", errno);

	/* The first commit, without an image, asks for the first configure. */
	wl_surface_commit(window->surface);
	status = wl_display_roundtrip(wayland->display);
	if (status < 0 || !window->configured) {
		wayland_close(wayland, window);
		return NULL;
	}

	/* Succeeded: the window can show the X window. */
	return window;
}

/*
 * Brings a surface in step with its X window: a new size drops the
 * buffers (the next present makes them at the size), a new place moves
 * the corner the pointer's places are counted from, a new title is shown.
 */
static void
wayland_update(
	void *context,
	struct x11_surface *window,
	const struct x11_surface_info *info,
	unsigned changed)
{
	/* The size; buffers of the old one go. */
	(void)context;
	if ((changed & X11_SURFACE_CHANGED_SIZE) != 0U &&
	    (info->width != window->width || info->height != window->height)) {
		window->width = info->width;
		window->height = info->height;
		wayland_buffers_free(window);
	}

	/* The pointer's places add the corner. */
	if ((changed & X11_SURFACE_CHANGED_POSITION) != 0U) {
		window->origin_x = info->x;
		window->origin_y = info->y;
	}

	/* The desktop shows the title on the title bar. */
	if ((changed & X11_SURFACE_CHANGED_TITLE) != 0U)
		xdg_toplevel_set_title(window->toplevel, info->title);
}

/*
 * Closes a surface: its buffers, its roles and its wl_surface (the core
 * has closed the swapchain on it first).
 */
static void
wayland_close(
	void *context,
	struct x11_surface *window)
{
	struct x11_surface **link;
	struct x11_wayland *wayland;

	/* It leaves the connection's list. */
	wayland = context;
	for (link = &wayland->windows; *link != NULL; link = &(*link)->next) {
		if (*link == window) {
			*link = window->next;
			break;
		}
	}

	/* The pointer is no longer in it. */
	if (wayland->pointer_window == window)
		wayland->pointer_window = NULL;

	/* Its buffers (before the surface they are on), its roles and its surface. */
	wayland_buffers_free(window);
	if (window->titlebar != NULL)
		kl_titlebar_destroy(window->titlebar);
	if (window->toplevel != NULL)
		xdg_toplevel_destroy(window->toplevel);
	if (window->role != NULL)
		xdg_surface_destroy(window->role);
	if (window->surface != NULL)
		wl_surface_destroy(window->surface);
	(void)wl_display_flush(wayland->display);

	/* The window itself. */
	free(window);
}

/*
 * Makes the Vulkan surface of a surface's wl_surface, on the core's
 * instance, into *result (a VkSurfaceKHR).  Returns 0, or EIO.
 */
static int
wayland_vulkan_surface(
	void *context,
	struct x11_surface *window,
	void *instance,
	void *result)
{
	VkWaylandSurfaceCreateInfoKHR create;
	struct x11_wayland *wayland;
	VkResult error;

	/* The window's wl_surface on the connection. */
	wayland = context;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	create.display = wayland->display;
	create.surface = window->surface;
	error = vkCreateWaylandSurfaceKHR((VkInstance)instance, &create, NULL, (VkSurfaceKHR *)result);
	if (error != VK_SUCCESS)
		return EIO;

	/* Succeeded: the core makes a swapchain on it. */
	return 0;
}

/*
 * Shows a surface's pixels (rows of its width, stride pixels apart): all
 * of them are copied into a buffer the desktop has released, and the
 * parts that changed are the damage.  Returns 0 when it was committed,
 * EBUSY when the desktop holds both buffers (the core tries again later),
 * or ENOMEM when the buffers cannot be made.
 */
static int
wayland_present(
	void *context,
	struct x11_surface *window,
	const uint32_t *pixels,
	size_t stride,
	const struct x11_rect *parts,
	unsigned count)
{
	struct wayland_buffer *buffer;
	unsigned index;
	unsigned row;
	int status;

	/* The buffers, made at the window's size the first time. */
	(void)context;
	if (window->memory == NULL) {
		status = wayland_buffers(window);
		if (status != 0) {
			wayland_buffers_free(window);
			return ENOMEM;
		}
	}

	/* The first buffer the desktop has given back. */
	buffer = NULL;
	for (index = 0U; index < 2U; index++) {
		if (!window->buffers[index].busy) {
			buffer = &window->buffers[index];
			break;
		}
	}

	/* Both are still held: the frame waits. */
	if (buffer == NULL)
		return EBUSY;

	/* All the pixels, so the buffer is whole whatever it showed before. */
	for (row = 0U; row < window->height; row++)
		memcpy(buffer->pixels + (size_t)row * window->width, pixels + (size_t)row * stride, (size_t)window->width * sizeof(uint32_t));
	buffer->busy = 1;

	/* Attached with the changed parts as the damage. */
	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	for (index = 0U; index < count; index++)
		wl_surface_damage(window->surface, parts[index].x, parts[index].y, (int32_t)parts[index].width, (int32_t)parts[index].height);

	/* Succeeded: committed, the frame is the desktop's. */
	wl_surface_commit(window->surface);
	(void)wl_display_flush(window->wayland->display);
	return 0;
}

/*
 * Makes an X owner's text the desktop's selection (own nonzero), or gives
 * that up.  Returns 0, or an errno value.
 */
static int
wayland_clipboard_own(
	void *context,
	unsigned selection,
	int own)
{
	struct x11_wayland *wayland;
	int error;

	/* The primary selection. */
	wayland = context;
	if (selection == X11_SELECTION_PRIMARY) {
		if (!own) {
			wayland_primary_drop(wayland);
			return 0;
		}

		/* The server's primary source. */
		error = wayland_primary_own(wayland);
		if (error != 0)
			return error;
		return 0;
	}

	/* The clipboard given up. */
	if (!own) {
		wayland_selection_drop(wayland);
		return 0;
	}

	/* The server's source. */
	error = wayland_selection_own(wayland);
	if (error != 0)
		return error;

	/* Succeeded: the X owner's text is offered. */
	return 0;
}

/* Tells whether another client's text is the desktop's clipboard or primary selection. */
static int
wayland_clipboard_has_text(
	void *context,
	unsigned selection)
{
	struct x11_wayland *wayland;
	int text;

	/* The primary selection's. */
	wayland = context;
	if (selection == X11_SELECTION_PRIMARY) {
		text = wayland_primary_has_text(wayland);
		return text;
	}

	/* The clipboard's. */
	text = wayland_selection_has_text(wayland);
	return text;
}

/*
 * Reads another client's text of the desktop's clipboard or primary
 * selection.  Returns 0 with the text (the caller frees it), or an errno
 * value.
 */
static int
wayland_clipboard_read(
	void *context,
	unsigned selection,
	uint8_t **text,
	size_t *size)
{
	struct x11_wayland *wayland;
	char *read_text;
	int error;

	/* The primary selection's, else the clipboard's. */
	wayland = context;
	if (selection == X11_SELECTION_PRIMARY) {
		error = wayland_primary_read(wayland, &read_text, size);
	} else {
		error = wayland_selection_read(wayland, &read_text, size);
	}

	/* Nothing read. */
	*text = NULL;
	if (error != 0)
		return error;

	/* Succeeded: the text is the caller's. */
	*text = (uint8_t *)read_text;
	return 0;
}

/*
 * Answers a desktop client's request for an X owner's text: the text
 * (none for NULL) is written into the request's descriptor, which is then
 * closed so the reader sees the end.  Returns how many bytes were written.
 */
static size_t
wayland_clipboard_answer(
	void *context,
	uint32_t request,
	const uint8_t *text,
	size_t size)
{
	ssize_t wrote;
	size_t done;
	int fd;

	/* The request is the descriptor the desktop gave. */
	(void)context;
	fd = (int)request;

	/* All of it (a short text; a failure leaves the reader what was written). */
	done = 0U;
	while (text != NULL && done < size) {
		wrote = write(fd, text + done, size - done);
		if (wrote <= 0)
			break;
		done += (size_t)wrote;
	}

	/* The reader sees the end. */
	(void)close(fd);

	/* Succeeded: what was written. */
	return done;
}

/*
 * Makes an X client's text the desktop's selection: the server's source
 * offers the text types, and the desktop's requests for them go to
 * the core (x11server_backend_clipboard_request).  Returns 0, or an
 * errno value (ENOTSUP without a data device).
 */
static int
wayland_selection_own(
	struct x11_wayland *wayland)
{
	struct wl_data_source *source;

	/* Only with a data device; an own source already there is kept. */
	if (wayland->data_device == NULL)
		return ENOTSUP;
	if (wayland->source != NULL)
		return 0;

	/* The source and its types. */
	source = wl_data_device_manager_create_data_source(wayland->data_manager);
	if (source == NULL)
		return ENOMEM;
	(void)wl_data_source_add_listener(source, &wayland_source_listener, wayland);
	wl_data_source_offer(source, WAYLAND_TEXT_UTF8);
	wl_data_source_offer(source, WAYLAND_TEXT_PLAIN);
	wl_data_source_offer(source, WAYLAND_TEXT_X11);
	wl_data_source_offer(source, WAYLAND_TEXT_STRING);

	/* The selection, with the last input's serial; another client's offer is not the selection any more. */
	wayland->source = source;
	wl_data_device_set_selection(wayland->data_device, source, wayland->serial);
	if (wayland->selection != NULL)
		wl_data_offer_destroy(wayland->selection);
	wayland->selection = NULL;
	wayland->selection_text = 0;

	/* Succeeded: sent at once. */
	(void)wl_display_flush(wayland->display);
	printf("X11 CLIPBOARD own\n");
	fflush(stdout);
	return 0;
}

/*
 * Gives up the server's source (its X client gave up CLIPBOARD, or went):
 * the desktop's selection is emptied.
 */
static void
wayland_selection_drop(
	struct x11_wayland *wayland)
{
	/* Only the server's own source. */
	if (wayland->source == NULL)
		return;

	/* The source goes; the selection with it. */
	wl_data_source_destroy(wayland->source);
	wayland->source = NULL;
	(void)wl_display_flush(wayland->display);
	printf("X11 CLIPBOARD drop\n");
	fflush(stdout);
}

/*
 * Tells whether another client's text is the desktop's selection.
 */
static int
wayland_selection_has_text(
	const struct x11_wayland *wayland)
{
	/* A selection offer with text. */
	if (wayland->selection != NULL && wayland->selection_text)
		return 1;

	/* None. */
	return 0;
}

/*
 * Reads the text of another client's selection (waiting at most
 * WAYLAND_READ_MS for it).  Returns 0 with the text (the caller frees it;
 * it has a NUL after its length), or an errno value.
 */
static int
wayland_selection_read(
	struct x11_wayland *wayland,
	char **text,
	size_t *length)
{
	int pipes[2];
	int status;
	int error;

	/* Nothing yet, and nothing without text. */
	*text = NULL;
	*length = 0;
	status = wayland_selection_has_text(wayland);
	if (!status)
		return ENOENT;

	/* The pipe the source writes into. */
	status = pipe(pipes);
	if (status != 0)
		return errno;

	/* The request with the write end, which the server closes (the reader sees the end once the source closes its copy). */
	wl_data_offer_receive(wayland->selection, WAYLAND_TEXT_UTF8, pipes[1]);
	close(pipes[1]);
	(void)wl_display_flush(wayland->display);

	/* The text from the read end. */
	error = wayland_pipe_read(pipes[0], text, length);
	if (error != 0)
		return error;

	/* Succeeded: the text, ended by a NUL. */
	printf("X11 CLIPBOARD read bytes=%lu\n", (unsigned long)*length);
	fflush(stdout);
	return 0;
}

/*
 * Makes an X client's text the desktop's primary selection (ws035-p103),
 * as x11_wayland_selection_own does for the clipboard; the desktop's
 * requests go to the core (x11server_backend_clipboard_request).
 * Returns 0, or an errno value (ENOTSUP without a primary device).
 */
static int
wayland_primary_own(
	struct x11_wayland *wayland)
{
	struct zwp_primary_selection_source_v1 *source;

	/* Only with a primary device; an own source already there is kept. */
	if (wayland->primary_device == NULL)
		return ENOTSUP;
	if (wayland->primary_source != NULL)
		return 0;

	/* The source and its types. */
	source = zwp_primary_selection_device_manager_v1_create_source(wayland->primary_manager);
	if (source == NULL)
		return ENOMEM;
	(void)zwp_primary_selection_source_v1_add_listener(source, &wayland_primary_source_listener, wayland);
	zwp_primary_selection_source_v1_offer(source, WAYLAND_TEXT_UTF8);
	zwp_primary_selection_source_v1_offer(source, WAYLAND_TEXT_PLAIN);
	zwp_primary_selection_source_v1_offer(source, WAYLAND_TEXT_X11);
	zwp_primary_selection_source_v1_offer(source, WAYLAND_TEXT_STRING);

	/* The selection; another client's offer is not the selection any more. */
	wayland->primary_source = source;
	zwp_primary_selection_device_v1_set_selection(wayland->primary_device, source, wayland->serial);
	if (wayland->primary != NULL)
		zwp_primary_selection_offer_v1_destroy(wayland->primary);
	wayland->primary = NULL;
	wayland->primary_text = 0;

	/* Succeeded: sent at once. */
	(void)wl_display_flush(wayland->display);
	printf("X11 PRIMARY own\n");
	fflush(stdout);
	return 0;
}

/* Gives up the server's primary source (its X client gave up PRIMARY, or went). */
static void
wayland_primary_drop(
	struct x11_wayland *wayland)
{
	/* Only the server's own source. */
	if (wayland->primary_source == NULL)
		return;

	/* The source goes; the selection with it. */
	zwp_primary_selection_source_v1_destroy(wayland->primary_source);
	wayland->primary_source = NULL;
	(void)wl_display_flush(wayland->display);
	printf("X11 PRIMARY drop\n");
	fflush(stdout);
}

/* Tells whether another client's text is the desktop's primary selection. */
static int
wayland_primary_has_text(
	const struct x11_wayland *wayland)
{
	/* A selection offer with text. */
	if (wayland->primary != NULL && wayland->primary_text)
		return 1;

	/* None. */
	return 0;
}

/*
 * Reads the text of another client's primary selection (waiting at most
 * WAYLAND_READ_MS for it).  Returns 0 with the text (the caller frees it;
 * it has a NUL after its length), or an errno value.
 */
static int
wayland_primary_read(
	struct x11_wayland *wayland,
	char **text,
	size_t *length)
{
	int pipes[2];
	int status;
	int error;

	/* Nothing yet, and nothing without text. */
	*text = NULL;
	*length = 0;
	status = wayland_primary_has_text(wayland);
	if (!status)
		return ENOENT;

	/* The pipe the source writes into. */
	status = pipe(pipes);
	if (status != 0)
		return errno;

	/* The request with the write end, which the server closes. */
	zwp_primary_selection_offer_v1_receive(wayland->primary, WAYLAND_TEXT_UTF8, pipes[1]);
	close(pipes[1]);
	(void)wl_display_flush(wayland->display);

	/* The text from the read end. */
	error = wayland_pipe_read(pipes[0], text, length);
	if (error != 0)
		return error;

	/* Succeeded: the text, ended by a NUL. */
	printf("X11 PRIMARY read bytes=%lu\n", (unsigned long)*length);
	fflush(stdout);
	return 0;
}

/* Makes a window's two XRGB8888 buffers in one shared-memory pool. */
static int
wayland_buffers(
	struct x11_surface *window)
{
	struct wl_shm_pool *pool;
	char name[64];
	size_t bytes;
	unsigned index;
	int descriptor;
	int error;

	/* Anonymous shared memory for both buffers. */
	bytes = (size_t)window->width * window->height * sizeof(uint32_t);
	window->memory_size = bytes * 2U;
	(void)snprintf(name, sizeof(name), "/xserver-%ld-%lu", (long)getpid(), (unsigned long)window->id);
	descriptor = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0)
		return -1;
	(void)shm_unlink(name);

	/* Its size. */
	error = ftruncate(descriptor, (off_t)window->memory_size);
	if (error != 0) {
		(void)close(descriptor);
		return -1;
	}

	/* Mapped for the server's writes. */
	window->memory = mmap(NULL, window->memory_size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	if (window->memory == MAP_FAILED) {
		window->memory = NULL;
		(void)close(descriptor);
		return -1;
	}

	/* The pool the desktop maps (it keeps its own reference to the memory). */
	pool = wl_shm_create_pool(window->wayland->shm, descriptor, (int32_t)window->memory_size);
	(void)close(descriptor);
	if (pool == NULL)
		return -1;

	/* A buffer of each half, whose release is heard. */
	for (index = 0U; index < 2U; index++) {
		window->buffers[index].pixels = (uint32_t *)((unsigned char *)window->memory + bytes * index);
		window->buffers[index].busy = 0;
		window->buffers[index].buffer = wl_shm_pool_create_buffer(pool, (int32_t)(bytes * index), (int32_t)window->width, (int32_t)window->height,
									 (int32_t)window->width * 4, WL_SHM_FORMAT_XRGB8888);
		if (window->buffers[index].buffer == NULL) {
			wl_shm_pool_destroy(pool);
			return -1;
		}

		/* Its release is heard. */
		(void)wl_buffer_add_listener(window->buffers[index].buffer, &wayland_buffer_listener, &window->buffers[index]);
	}

	/* Succeeded: the pool lives on in its buffers. */
	wl_shm_pool_destroy(pool);
	return 0;
}

/* Releases a window's buffers and their memory. */
static void
wayland_buffers_free(
	struct x11_surface *window)
{
	unsigned index;

	/* Each buffer. */
	for (index = 0U; index < 2U; index++) {
		if (window->buffers[index].buffer != NULL)
			wl_buffer_destroy(window->buffers[index].buffer);
		window->buffers[index].buffer = NULL;
		window->buffers[index].busy = 0;
	}

	/* The shared memory under them. */
	if (window->memory != NULL)
		(void)munmap(window->memory, window->memory_size);
	window->memory = NULL;
}

/* Returns the window of a surface, or NULL. */
static struct x11_surface *
wayland_window_of(
	struct x11_wayland *wayland,
	struct wl_surface *surface)
{
	struct x11_surface *window;

	/* The connection's window on the surface, if it is one of them. */
	for (window = wayland->windows; window != NULL; window = window->next) {
		if (window->surface == surface)
			break;
	}

	/* Not one of them. */
	if (window == NULL)
		return NULL;

	/* Succeeded: the window. */
	return window;
}

/* Gives the server one pointer frame: the pointer's place, and a button's change when there is one. */
static void
wayland_pointer_frame(
	struct x11_wayland *wayland,
	uint32_t time,
	uint8_t button,
	int pressed,
	uint16_t bit)
{
	struct x11_pointer_frame frame;

	/* Where the pointer is on the root window, and the buttons held before. */
	memset(&frame, 0, sizeof(frame));
	frame.x = wayland->pointer_x;
	frame.y = wayland->pointer_y;
	frame.time = time;
	frame.buttons_before = wayland->buttons;

	/* A button's change. */
	if (button != 0U) {
		if (pressed) {
			wayland->buttons = (uint16_t)(wayland->buttons | bit);
		} else {
			wayland->buttons = (uint16_t)(wayland->buttons & ~bit);
		}

		/* The change, in the frame. */
		frame.button = button;
		frame.pressed = pressed;
	}

	/* The core hears it. */
	frame.buttons_after = wayland->buttons;
	if (wayland->server != NULL)
		x11server_backend_pointer(wayland->server, &frame);
}

/* Binds the compositor, shared memory, the shell and the seat. */
static void
wayland_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct x11_wayland *wayland;
	int compositor;
	int shm;
	int shell;
	int seat;
	int manager;
	int primary;

	/* Which of the six this is. */
	wayland = data;
	compositor = strcmp(interface, "wl_compositor");
	shm = strcmp(interface, "wl_shm");
	shell = strcmp(interface, "xdg_wm_base");
	seat = strcmp(interface, "wl_seat");
	manager = strcmp(interface, "wl_data_device_manager");
	primary = strcmp(interface, "zwp_primary_selection_device_manager_v1");

	/* The primary selection's manager (version 1). */
	if (primary == 0 && wayland->primary_manager == NULL) {
		wayland->primary_manager = wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, 1U);
		return;
	}

	/* The data device manager, for the clipboard (version 3 at most). */
	if (manager == 0 && wayland->data_manager == NULL) {
		if (version > 3U)
			version = 3U;
		wayland->data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, version);
		return;
	}

	/* Each is bound once, at the version used. */
	if (compositor == 0 && wayland->compositor == NULL) {
		wayland->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4U);
	} else if (shm == 0 && wayland->shm == NULL) {
		wayland->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1U);
	} else if (shell == 0 && wayland->shell == NULL) {
		if (version > 4U)
			version = 4U;
		wayland->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, version);
		if (wayland->shell != NULL)
			(void)xdg_wm_base_add_listener(wayland->shell, &wayland_shell_listener, wayland);
	} else if (seat == 0 && wayland->seat == NULL) {
		wayland->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1U);
		if (wayland->seat != NULL)
			(void)wl_seat_add_listener(wayland->seat, &wayland_seat_listener, wayland);
	}
}

/* A global going away does not matter to the windows that are up. */
static void
wayland_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Nothing to do. */
	(void)data;
	(void)registry;
	(void)name;
}

/* Answers the desktop's liveness check. */
static void
wayland_ping(
	void *data,
	struct xdg_wm_base *shell,
	uint32_t serial)
{
	/* The same serial back. */
	(void)data;
	xdg_wm_base_pong(shell, serial);
}

/* Acknowledges a configure; the window may take buffers from then on. */
static void
wayland_configure(
	void *data,
	struct xdg_surface *surface,
	uint32_t serial)
{
	struct x11_surface *window;

	/* Acknowledged. */
	window = data;
	xdg_surface_ack_configure(surface, serial);
	window->configured = 1;
}

/* Tells the server the size the desktop gives a window (none keeps the window's own). */
static void
wayland_toplevel_configure(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height,
	struct wl_array *states)
{
	struct x11_surface *window;

	/* A size left to the window keeps its own, within the desktop's bounds (the next dispatch asks for it). */
	(void)toplevel;
	(void)states;
	window = data;
	if (width <= 0 || height <= 0) {
		wayland_bounded_size(window);
		return;
	}

	/* A size the desktop gives replaces one waiting within the bounds. */
	window->pending_width = 0U;
	window->pending_height = 0U;

	/* The window's own size changes nothing. */
	if ((unsigned)width == window->width && (unsigned)height == window->height)
		return;

	/* The server resizes the X window, and then this one follows. */
	if (window->wayland->server != NULL)
		x11server_backend_configure(window->wayland->server, window->id, width, height);
}

/* The desktop asks a window to close: the server decides what that means. */
static void
wayland_toplevel_close(
	void *data,
	struct xdg_toplevel *toplevel)
{
	struct x11_surface *window;

	/* The server hears it. */
	(void)toplevel;
	window = data;
	if (window->wayland->server != NULL)
		x11server_backend_close(window->wayland->server, window->id);
}

/*
 * Keeps the largest size the desktop lets the window choose (xdg-shell
 * version 4); the configure that follows applies it.  A zero is a size the
 * desktop does not know.
 */
static void
wayland_toplevel_bounds(
	void *data,
	struct xdg_toplevel *toplevel,
	int32_t width,
	int32_t height)
{
	struct x11_surface *window;

	/* The width, when known. */
	(void)toplevel;
	window = data;
	window->bounds_width = 0U;
	if (width > 0)
		window->bounds_width = (unsigned)width;

	/* The height, when known. */
	window->bounds_height = 0U;
	if (height > 0)
		window->bounds_height = (unsigned)height;
}

/*
 * Notes the size a window is to take when its own does not fit the
 * desktop's bounds: its own size, cut to the bounds.  A window that fits
 * has nothing waiting.
 */
static void
wayland_bounded_size(
	struct x11_surface *window)
{
	unsigned width;
	unsigned height;

	/* The width, at most the bounds'. */
	width = window->width;
	if (window->bounds_width > 0U && width > window->bounds_width)
		width = window->bounds_width;

	/* The height, at most the bounds'. */
	height = window->height;
	if (window->bounds_height > 0U && height > window->bounds_height)
		height = window->bounds_height;

	/* A window that fits keeps its size. */
	window->pending_width = 0U;
	window->pending_height = 0U;
	if (width == window->width && height == window->height)
		return;

	/* The size the next dispatch asks the server for. */
	window->pending_width = width;
	window->pending_height = height;
}

/*
 * Hands the server the sizes windows are to take within their bounds: the
 * server resizes each X window, and its desktop window follows.
 */
static void
wayland_pending_sizes(
	struct x11_wayland *wayland)
{
	struct x11_surface *window;
	unsigned width;
	unsigned height;

	/* Each window with a size waiting. */
	for (window = wayland->windows; window != NULL; window = window->next) {
		if (window->pending_width == 0U || window->pending_height == 0U)
			continue;

		/* Taken once; the server may close windows while it runs, so the walk ends after one. */
		width = window->pending_width;
		height = window->pending_height;
		window->pending_width = 0U;
		window->pending_height = 0U;
		fprintf(stderr, "X11SERVER BOUNDS window=0x%x width=%u height=%u was=%ux%u\n", (unsigned)window->id, width, height, window->width, window->height);
		if (wayland->server != NULL)
			x11server_backend_configure(wayland->server, window->id, (int)width, (int)height);
		return;
	}
}

/* The desktop has given a buffer back: it can take the next frame. */
static void
wayland_buffer_release(
	void *data,
	struct wl_buffer *buffer)
{
	struct wayland_buffer *released;

	/* Free again. */
	(void)buffer;
	released = data;
	released->busy = 0;
}

/* Takes the seat's pointer and keyboard. */
static void
wayland_seat_capabilities(
	void *data,
	struct wl_seat *seat,
	uint32_t capabilities)
{
	struct x11_wayland *wayland;

	/* The pointer, once. */
	wayland = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0U && wayland->pointer == NULL) {
		wayland->pointer = wl_seat_get_pointer(seat);
		if (wayland->pointer != NULL)
			(void)wl_pointer_add_listener(wayland->pointer, &wayland->pointer_listener, wayland);
	}

	/* The keyboard, once. */
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0U && wayland->keyboard == NULL) {
		wayland->keyboard = wl_seat_get_keyboard(seat);
		if (wayland->keyboard != NULL)
			(void)wl_keyboard_add_listener(wayland->keyboard, &wayland->keyboard_listener, wayland);
	}
}

/* The seat's name is not used. */
static void
wayland_seat_name(
	void *data,
	struct wl_seat *seat,
	const char *name)
{
	/* Nothing to do. */
	(void)data;
	(void)seat;
	(void)name;
}

/* The pointer comes into a window: the server hears which, and the pointer moves there. */
static void
wayland_pointer_enter(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct x11_wayland *wayland;
	struct x11_surface *window;

	/* The desktop draws its own pointer over the window. */
	wayland = data;
	wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);

	/* A surface that is not one of the windows is ignored. */
	window = wayland_window_of(wayland, surface);
	wayland->pointer_window = window;
	if (window == NULL)
		return;

	/* The X window comes up, and the pointer is at its place on the root window. */
	if (wayland->server != NULL)
		x11server_backend_enter(wayland->server, window->id, 0);
	wayland->pointer_x = window->origin_x + wl_fixed_to_int(x);
	wayland->pointer_y = window->origin_y + wl_fixed_to_int(y);
	wayland_pointer_frame(wayland, 0U, 0U, 0, 0U);
}

/* The pointer leaves a window. */
static void
wayland_pointer_leave(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct x11_wayland *wayland;

	/* The pointer is in none of the windows. */
	(void)pointer;
	(void)serial;
	(void)surface;
	wayland = data;
	wayland->pointer_window = NULL;
}

/* The pointer moves within a window. */
static void
wayland_pointer_motion(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct x11_wayland *wayland;
	struct x11_surface *window;

	/* Only within a window. */
	(void)pointer;
	wayland = data;
	window = wayland->pointer_window;
	if (window == NULL)
		return;

	/* Its place on the root window. */
	wayland->pointer_x = window->origin_x + wl_fixed_to_int(x);
	wayland->pointer_y = window->origin_y + wl_fixed_to_int(y);
	wayland_pointer_frame(wayland, time, 0U, 0, 0U);
}

/* A button is pressed or released: X's buttons 1 (left), 2 (middle) and 3 (right). */
static void
wayland_pointer_button(
	void *data,
	struct wl_pointer *pointer,
	uint32_t serial,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	struct x11_wayland *wayland;
	int pressed;

	/* The serial, which a selection is set with. */
	(void)pointer;
	wayland = data;
	wayland->serial = serial;
	pressed = 0;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		pressed = 1;

	/* The button's X number and bit. */
	switch (button) {
	case WAYLAND_BTN_LEFT:
		wayland_pointer_frame(wayland, time, 1U, pressed, 1U << 0);
		break;
	case WAYLAND_BTN_MIDDLE:
		wayland_pointer_frame(wayland, time, 2U, pressed, 1U << 1);
		break;
	case WAYLAND_BTN_RIGHT:
		wayland_pointer_frame(wayland, time, 3U, pressed, 1U << 2);
		break;
	default:
		break;
	}
}

/* Scrolling is not passed on yet. */
static void
wayland_pointer_axis(
	void *data,
	struct wl_pointer *pointer,
	uint32_t time,
	uint32_t axis,
	wl_fixed_t value)
{
	/* Nothing to do. */
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
	(void)value;
}

/* Closes the keymap: keys arrive as evdev codes and keymap.c has the table. */
static void
wayland_keyboard_keymap(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t format,
	int32_t fd,
	uint32_t size)
{
	/* The descriptor is the client's to close. */
	(void)data;
	(void)keyboard;
	(void)format;
	(void)size;
	if (fd >= 0)
		(void)close(fd);
}

/* Focus comes to a window: the server gives its X window the keyboard. */
static void
wayland_keyboard_enter(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface,
	struct wl_array *keys)
{
	struct x11_wayland *wayland;
	struct x11_surface *window;

	/* Keys held when focus came are not typed. */
	(void)keyboard;
	(void)serial;
	(void)keys;
	wayland = data;

	/* The X window of the surface (one the core opened) gets the focus. */
	window = wayland_window_of(wayland, surface);
	if (window != NULL && wayland->server != NULL)
		x11server_backend_enter(wayland->server, window->id, 1);
}

/* Focus leaves: the modifiers are forgotten. */
static void
wayland_keyboard_leave(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct x11_wayland *wayland;

	/* No modifier is held any more. */
	(void)keyboard;
	(void)serial;
	(void)surface;
	wayland = data;
	wayland->modifiers = 0U;
}

/* A key is pressed or released: the server hears its X keycode with the modifiers. */
static void
wayland_keyboard_key(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	struct x11_wayland *wayland;
	uint16_t modifiers;
	uint8_t keycode;
	int shifted;

	/* The serial, which a selection is set with; a code past the table, or one before the core is attached, is ignored. */
	(void)keyboard;
	wayland = data;
	wayland->serial = serial;
	if (key >= WAYLAND_KEYS || wayland->server == NULL)
		return;
	modifiers = (uint16_t)(wayland->modifiers & WAYLAND_MODIFIERS);

	/* Caps lock toggles on its press and types nothing. */
	if (key == WAYLAND_KEY_CAPSLOCK) {
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
			wayland->caps_lock = !wayland->caps_lock;
		return;
	}

	/* A release says the keycode its press had. */
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		keycode = wayland->keycodes[key];
		wayland->keycodes[key] = 0U;
		if (keycode != 0U)
			x11server_backend_key(wayland->server, keycode, 0, time, modifiers);
		return;
	}

	/* A press: the keycode in the current shift and caps lock states. */
	shifted = 0;
	if ((wayland->modifiers & X11_SHIFT_MASK) != 0U)
		shifted = 1;
	keycode = x11_keymap_keycode((uint16_t)key, shifted, wayland->caps_lock);
	if (keycode == 0U)
		return;

	/* Succeeded: the server hears the press. */
	wayland->keycodes[key] = keycode;
	x11server_backend_key(wayland->server, keycode, 1, time, modifiers);
}

/* Keeps the modifiers held (the same bits as X's). */
static void
wayland_keyboard_modifiers(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	uint32_t depressed,
	uint32_t latched,
	uint32_t locked,
	uint32_t group)
{
	struct x11_wayland *wayland;

	/* Only the held ones count. */
	(void)keyboard;
	(void)serial;
	(void)latched;
	(void)locked;
	(void)group;
	wayland = data;
	wayland->modifiers = depressed;
}

/* Notes a new offer introduced by the desktop; its types follow. */
static void
wayland_data_offer(
	void *data,
	struct wl_data_device *device,
	struct wl_data_offer *offer)
{
	struct x11_wayland *wayland;

	/* The last offer introduced, not known to have text yet. */
	(void)device;
	wayland = data;
	wayland->offer_new = offer;
	wayland->offer_new_text = 0;
	(void)wl_data_offer_add_listener(offer, &wayland_offer_listener, wayland);
}

/* A drag over an X window is not taken: its offer goes. */
static void
wayland_data_enter(
	void *data,
	struct wl_data_device *device,
	uint32_t serial,
	struct wl_surface *surface,
	wl_fixed_t x,
	wl_fixed_t y,
	struct wl_data_offer *offer)
{
	struct x11_wayland *wayland;

	/* The offer (none is accepted). */
	(void)device;
	(void)serial;
	(void)surface;
	(void)x;
	(void)y;
	wayland = data;
	if (offer == wayland->offer_new)
		wayland->offer_new = NULL;
	if (offer != NULL)
		wl_data_offer_destroy(offer);
}

/* A drag leaves: nothing to do. */
static void
wayland_data_leave(
	void *data,
	struct wl_data_device *device)
{
	/* Nothing to do. */
	(void)data;
	(void)device;
}

/* A drag moves: nothing to do. */
static void
wayland_data_motion(
	void *data,
	struct wl_data_device *device,
	uint32_t time,
	wl_fixed_t x,
	wl_fixed_t y)
{
	/* Nothing to do. */
	(void)data;
	(void)device;
	(void)time;
	(void)x;
	(void)y;
}

/* A drop is never accepted here: nothing to do. */
static void
wayland_data_drop(
	void *data,
	struct wl_data_device *device)
{
	/* Nothing to do. */
	(void)data;
	(void)device;
}

/*
 * The desktop's selection changed: another client's offer (or none).  The
 * server's own source's offer is not taken; another's with text makes
 * the core own X's CLIPBOARD for it.
 */
static void
wayland_data_selection(
	void *data,
	struct wl_data_device *device,
	struct wl_data_offer *offer)
{
	struct x11_wayland *wayland;
	int text;

	/* The offer's text, when it is the last introduced. */
	(void)device;
	wayland = data;
	text = 0;
	if (offer != NULL && offer == wayland->offer_new)
		text = wayland->offer_new_text;
	if (offer == wayland->offer_new)
		wayland->offer_new = NULL;

	/* While the server's own source is the selection, the offer is its own: not taken. */
	if (wayland->source != NULL) {
		if (offer != NULL)
			wl_data_offer_destroy(offer);
		return;
	}

	/* The offer before goes; this one is kept. */
	if (wayland->selection != NULL && wayland->selection != offer)
		wl_data_offer_destroy(wayland->selection);
	wayland->selection = offer;
	wayland->selection_text = text;
	printf("X11 CLIPBOARD selection text=%d\n", text);
	fflush(stdout);

	/* Succeeded: the core hears whether there is text for X's CLIPBOARD. */
	if (wayland->server != NULL)
		x11server_backend_clipboard(wayland->server, X11_SELECTION_CLIPBOARD, text);
}

/* Notes whether a type of the last introduced offer is text. */
static void
wayland_offer_type(
	void *data,
	struct wl_data_offer *offer,
	const char *mime_type)
{
	struct x11_wayland *wayland;
	int text;

	/* Only the last introduced offer's types matter. */
	wayland = data;
	if (offer != wayland->offer_new)
		return;

	/* One of the text types. */
	text = wayland_text_type(mime_type);
	if (text)
		wayland->offer_new_text = 1;
}

/* The type a target took is not needed. */
static void
wayland_source_target(
	void *data,
	struct wl_data_source *source,
	const char *mime_type)
{
	/* Nothing to do. */
	(void)data;
	(void)source;
	(void)mime_type;
}

/*
 * A client asks for the X client's text: its descriptor goes to
 * the core, which asks the X owner and answers with the text when it comes.
 * A type that is not text is closed at once.
 */
static void
wayland_source_send(
	void *data,
	struct wl_data_source *source,
	const char *mime_type,
	int32_t fd)
{
	struct x11_wayland *wayland;
	int text;

	/* Only text, and only with a server to answer. */
	(void)source;
	wayland = data;
	text = wayland_text_type(mime_type);
	if (!text || wayland->server == NULL) {
		close(fd);
		return;
	}

	/* Succeeded: the core's answer writes and closes the descriptor. */
	printf("X11 CLIPBOARD send mime=%s\n", mime_type);
	fflush(stdout);
	x11server_backend_clipboard_request(wayland->server, X11_SELECTION_CLIPBOARD, (uint32_t)fd);
}

/* Another client took the selection: the server's source goes (the new selection follows). */
static void
wayland_source_cancelled(
	void *data,
	struct wl_data_source *source)
{
	struct x11_wayland *wayland;

	/* The source. */
	wayland = data;
	wl_data_source_destroy(source);
	if (wayland->source == source)
		wayland->source = NULL;
	printf("X11 CLIPBOARD cancelled\n");
	fflush(stdout);
}

/* Tells whether a MIME type is one of the text types the clipboard takes. */
static int
wayland_text_type(
	const char *mime_type)
{
	int same;

	/* UTF-8 text. */
	same = strcmp(mime_type, WAYLAND_TEXT_UTF8);
	if (same == 0)
		return 1;

	/* Plain text. */
	same = strcmp(mime_type, WAYLAND_TEXT_PLAIN);
	if (same == 0)
		return 1;

	/* X's names. */
	same = strcmp(mime_type, WAYLAND_TEXT_X11);
	if (same == 0)
		return 1;
	same = strcmp(mime_type, WAYLAND_TEXT_STRING);
	if (same == 0)
		return 1;

	/* Not text. */
	return 0;
}

/*
 * Reads everything written into a pipe's read end, up to its end, the
 * limit or WAYLAND_READ_MS without data; closes it.  Returns 0 with the
 * text (the caller frees it; a NUL follows its length), or an errno value.
 */
static int
wayland_pipe_read(
	int fd,
	char **text,
	size_t *length)
{
	struct pollfd descriptor;
	char *buffer;
	char *grown;
	size_t used;
	size_t capacity;
	ssize_t got;
	int status;
	int error;

	/* Everything written, up to the end, the limit or the time allowed. */
	buffer = NULL;
	used = 0;
	capacity = 0;
	error = 0;
	for (;;) {
		/* Waits for more. */
		descriptor.fd = fd;
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		status = poll(&descriptor, 1, WAYLAND_READ_MS);
		if (status <= 0) {
			error = ETIMEDOUT;
			break;
		}

		/* Room for more and a NUL. */
		if (used + 4096U + 1U > capacity) {
			capacity = used + 4096U + 1U;
			grown = realloc(buffer, capacity);
			if (grown == NULL) {
				error = ENOMEM;
				break;
			}

			/* The larger buffer. */
			buffer = grown;
		}

		/* The bytes; the end ends the reading. */
		got = read(fd, buffer + used, 4096U);
		if (got <= 0)
			break;
		used += (size_t)got;
		if (used > WAYLAND_READ_MAX) {
			error = E2BIG;
			break;
		}
	}

	/* The pipe's read end. */
	close(fd);

	/* A failed read gives nothing. */
	if (error != 0) {
		free(buffer);
		return error;
	}

	/* Empty text is still text. */
	if (buffer == NULL) {
		buffer = malloc(1U);
		if (buffer == NULL)
			return ENOMEM;
	}

	/* Succeeded: the text, ended by a NUL. */
	buffer[used] = '\0';
	*text = buffer;
	*length = used;
	return 0;
}

/* Notes a new primary offer introduced by the desktop; its types follow. */
static void
wayland_primary_offer(
	void *data,
	struct zwp_primary_selection_device_v1 *device,
	struct zwp_primary_selection_offer_v1 *offer)
{
	struct x11_wayland *wayland;

	/* The last offer introduced, not known to have text yet. */
	(void)device;
	wayland = data;
	wayland->primary_new = offer;
	wayland->primary_new_text = 0;
	(void)zwp_primary_selection_offer_v1_add_listener(offer, &wayland_primary_offer_listener, wayland);
}

/*
 * The desktop's primary selection changed: another client's offer (or
 * none).  The server's own source's offer is not taken; another's with
 * text makes the core own X's PRIMARY for it.
 */
static void
wayland_primary_selection(
	void *data,
	struct zwp_primary_selection_device_v1 *device,
	struct zwp_primary_selection_offer_v1 *offer)
{
	struct x11_wayland *wayland;
	int text;

	/* The offer's text, when it is the last introduced. */
	(void)device;
	wayland = data;
	text = 0;
	if (offer != NULL && offer == wayland->primary_new)
		text = wayland->primary_new_text;
	if (offer == wayland->primary_new)
		wayland->primary_new = NULL;

	/* While the server's own source is the selection, the offer is its own: not taken. */
	if (wayland->primary_source != NULL) {
		if (offer != NULL)
			zwp_primary_selection_offer_v1_destroy(offer);
		return;
	}

	/* The offer before goes; this one is kept. */
	if (wayland->primary != NULL && wayland->primary != offer)
		zwp_primary_selection_offer_v1_destroy(wayland->primary);
	wayland->primary = offer;
	wayland->primary_text = text;
	printf("X11 PRIMARY selection text=%d\n", text);
	fflush(stdout);

	/* Succeeded: the core hears whether there is text for X's PRIMARY. */
	if (wayland->server != NULL)
		x11server_backend_clipboard(wayland->server, X11_SELECTION_PRIMARY, text);
}

/* Notes whether a type of the last introduced primary offer is text. */
static void
wayland_primary_type(
	void *data,
	struct zwp_primary_selection_offer_v1 *offer,
	const char *mime_type)
{
	struct x11_wayland *wayland;
	int text;

	/* Only the last introduced offer's types matter. */
	wayland = data;
	if (offer != wayland->primary_new)
		return;

	/* One of the text types. */
	text = wayland_text_type(mime_type);
	if (text)
		wayland->primary_new_text = 1;
}

/*
 * A client asks for the X client's PRIMARY text: its descriptor goes to
 * the core, which asks the X owner.  A type that is not text is closed
 * at once.
 */
static void
wayland_primary_send(
	void *data,
	struct zwp_primary_selection_source_v1 *source,
	const char *mime_type,
	int32_t fd)
{
	struct x11_wayland *wayland;
	int text;

	/* Only text, and only with a server to answer. */
	(void)source;
	wayland = data;
	text = wayland_text_type(mime_type);
	if (!text || wayland->server == NULL) {
		close(fd);
		return;
	}

	/* Succeeded: the core's answer writes and closes the descriptor. */
	printf("X11 PRIMARY send mime=%s\n", mime_type);
	fflush(stdout);
	x11server_backend_clipboard_request(wayland->server, X11_SELECTION_PRIMARY, (uint32_t)fd);
}

/* Another client took the primary selection: the server's source goes. */
static void
wayland_primary_cancelled(
	void *data,
	struct zwp_primary_selection_source_v1 *source)
{
	struct x11_wayland *wayland;

	/* The source. */
	wayland = data;
	zwp_primary_selection_source_v1_destroy(source);
	if (wayland->primary_source == source)
		wayland->primary_source = NULL;
	printf("X11 PRIMARY cancelled\n");
	fflush(stdout);
}
