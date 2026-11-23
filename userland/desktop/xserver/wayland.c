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
 * corner there), and each key is the X keycode of its place on the
 * keyboard (its evdev code plus 8); the core's map is US.
 *
 * The clipboard: the seat's data device tells the core when another
 * client's text becomes the selection (the core then owns X's CLIPBOARD
 * for it and reads the text when an X client asks); the text the core
 * copies from an X owner of CLIPBOARD is offered as a data source, and
 * the backend keeps it, so it answers the desktop's requests itself and
 * the text stays after the X client ends.
 *
 * The primary selection is bridged the same way with X's
 * PRIMARY, through the desktop's zwp_primary_selection_device_manager_v1
 * when it has one.
 *
 * The cursor: the core tells the cursor of the X window under the
 * pointer; it is drawn into a wl_shm buffer of a cursor surface of the
 * backend's, which the pointer shows while it is over that window (set
 * again at each entering, as Wayland asks).  A window whose X cursor is
 * None, and a cursor without an image, shows the backend's own arrow.
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

/* The evdev codes of the pointer's buttons. */
#define WAYLAND_BTN_LEFT	0x110U
#define WAYLAND_BTN_RIGHT	0x111U
#define WAYLAND_BTN_MIDDLE	0x112U

/* The evdev key codes an X keycode can name (the keycode is the code plus 8, at most 255). */
#define WAYLAND_KEYS		248U

/* The application's identity on the desktop. */
#define WAYLAND_APP_ID		"xserver"

/* The largest side of a cursor image the backend shows (a larger one is the arrow). */
#define WAYLAND_CURSOR_MAX	256

/* The text types the clipboard offers and takes, the one read first, and how long a read may wait (milliseconds) and hold. */
#define WAYLAND_TEXT_UTF8	"text/plain;charset=utf-8"
#define WAYLAND_TEXT_PLAIN	"text/plain"
#define WAYLAND_TEXT_X11	"UTF8_STRING"
#define WAYLAND_TEXT_STRING	"STRING"
#define WAYLAND_READ_MS		2000
#define WAYLAND_READ_MAX	X11_CLIPBOARD_TEXT_MAX

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

	/*
	 * Its state (X11_SURFACE_ICONIC, MAXIMIZED, FULLSCREEN) as last asked
	 * of the desktop or told the server.  The desktop does not say when
	 * it brings a minimized window back; the window's activation (a
	 * configure with the activated state, or the keyboard's focus) does.
	 */
	unsigned state;

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

	/* The keys held (by evdev code), whose releases the core hears when the focus leaves. */
	uint8_t held[WAYLAND_KEYS];

	/*
	 * The clipboard (ws035-p087): the data device manager and the seat's
	 * device (NULL without them), the server's own source while an X
	 * client's text is the selection (NULL otherwise) and that text (a
	 * copy, NULL with the source), the offer
	 * introduced last and whether it has text, the selection's offer from
	 * another client (NULL for none) and whether it has text, and the
	 * serial of the last input (a selection is set with it).
	 */
	struct wl_data_device_manager *data_manager;
	struct wl_data_device *data_device;
	struct wl_data_source *source;
	uint8_t *source_text;
	size_t source_length;
	struct wl_data_offer *offer_new;
	int offer_new_text;
	struct wl_data_offer *selection;
	int selection_text;
	uint32_t serial;

	/*
	 * The primary selection (ws035-p103), the same as the clipboard's:
	 * the manager and the seat's device (NULL without them), the server's
	 * own source and its text, the offer introduced last and whether it has text, and
	 * another client's selection and whether it has text.
	 */
	struct zwp_primary_selection_device_manager_v1 *primary_manager;
	struct zwp_primary_selection_device_v1 *primary_device;
	struct zwp_primary_selection_source_v1 *primary_source;
	uint8_t *primary_source_text;
	size_t primary_source_length;
	struct zwp_primary_selection_offer_v1 *primary_new;
	int primary_new_text;
	struct zwp_primary_selection_offer_v1 *primary;
	int primary_text;

	/*
	 * The cursor: the serial of the pointer's last entering (a cursor is
	 * set with it), the cursor surface and the buffer it shows (NULL: none
	 * made yet), its hot spot, and the window whose cursor it is (NULL:
	 * the arrow, of no window).
	 */
	uint32_t pointer_serial;
	struct wl_surface *cursor_surface;
	struct wl_buffer *cursor_buffer;
	int cursor_hot_x;
	int cursor_hot_y;
	struct x11_surface *cursor_window;
};

static struct x11_surface *wayland_open(void *context, uint32_t id, const struct x11_surface_info *info);
static void wayland_update(void *context, struct x11_surface *window, const struct x11_surface_info *info, unsigned changed);
static void wayland_close(void *context, struct x11_surface *window);
static int wayland_vulkan_surface(void *context, struct x11_surface *window, void *instance, void *result);
static int wayland_present(void *context, struct x11_surface *window, const uint32_t *pixels, size_t stride, const struct x11_rect *parts, unsigned count);
static int wayland_clipboard_offer(void *context, unsigned selection, const uint8_t *text, size_t size);
static int wayland_clipboard_read(void *context, unsigned selection, uint8_t **text, size_t *size);
static int wayland_selection_own(struct x11_wayland *wayland, uint8_t *text, size_t length);
static int wayland_selection_has_text(const struct x11_wayland *wayland);
static int wayland_selection_read(struct x11_wayland *wayland, char **text, size_t *length);
static int wayland_primary_own(struct x11_wayland *wayland, uint8_t *text, size_t length);
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
static void wayland_shape(struct x11_wayland *wayland, struct x11_surface *window, const struct x11_surface_info *info);
static void wayland_limits(struct x11_surface *window, const struct x11_size_hints *hints);
static void wayland_set_state(struct x11_surface *window, unsigned state);
static void wayland_state_heard(struct x11_surface *window, unsigned state);
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
static size_t wayland_pipe_write(int fd, const uint8_t *text, size_t length);
static void wayland_primary_offer(void *data, struct zwp_primary_selection_device_v1 *device, struct zwp_primary_selection_offer_v1 *offer);
static void wayland_primary_selection(void *data, struct zwp_primary_selection_device_v1 *device, struct zwp_primary_selection_offer_v1 *offer);
static void wayland_primary_type(void *data, struct zwp_primary_selection_offer_v1 *offer, const char *mime_type);
static void wayland_primary_send(void *data, struct zwp_primary_selection_source_v1 *source, const char *mime_type, int32_t fd);
static void wayland_primary_cancelled(void *data, struct zwp_primary_selection_source_v1 *source);
static void wayland_cursor(void *context, struct x11_surface *window, const struct x11_cursor_image *image);
static int wayland_cursor_draw(struct x11_wayland *wayland, const uint32_t *pixels, int width, int height, int hot_x, int hot_y);
static void wayland_cursor_arrow(struct x11_wayland *wayland);
static void wayland_cursor_show(struct x11_wayland *wayland);

/*
 * The backend's functions, which the core calls with the connection as
 * their context.  Wayland gives no client the pointer's place on the
 * screen, nor a way to move it or to keep it, nor a bell: those are NULL.
 * The X cursor is shown.
 * A shape is the surface's input region alone, the same whichever way the
 * pixels are shown, so it asks nothing of the core's way (no flags).
 */
const struct x11_backend_ops x11_wayland_ops = {
	wayland_open, wayland_update, wayland_close, wayland_vulkan_surface, wayland_present, wayland_clipboard_offer,
	wayland_clipboard_read, NULL, NULL, NULL, NULL, wayland_cursor, 0U
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

	/* It hears of another client's text already the desktop's clipboard or primary selection. */
	if (wayland->selection != NULL && wayland->selection_text)
		x11server_backend_clipboard(server, X11_SELECTION_CLIPBOARD, 1);
	if (wayland->primary != NULL && wayland->primary_text)
		x11server_backend_clipboard(server, X11_SELECTION_PRIMARY, 1);
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
	free(wayland->primary_source_text);
	if (wayland->primary != NULL)
		zwp_primary_selection_offer_v1_destroy(wayland->primary);
	if (wayland->primary_device != NULL)
		zwp_primary_selection_device_v1_destroy(wayland->primary_device);
	if (wayland->primary_manager != NULL)
		zwp_primary_selection_device_manager_v1_destroy(wayland->primary_manager);

	/* The clipboard's objects. */
	if (wayland->source != NULL)
		wl_data_source_destroy(wayland->source);
	free(wayland->source_text);
	if (wayland->selection != NULL)
		wl_data_offer_destroy(wayland->selection);
	if (wayland->data_device != NULL)
		wl_data_device_destroy(wayland->data_device);
	if (wayland->data_manager != NULL)
		wl_data_device_manager_destroy(wayland->data_manager);

	/* The windows the core left open. */
	while (wayland->windows != NULL)
		wayland_close(wayland, wayland->windows);

	/* The cursor's buffer and surface. */
	if (wayland->cursor_buffer != NULL)
		wl_buffer_destroy(wayland->cursor_buffer);
	if (wayland->cursor_surface != NULL)
		wl_surface_destroy(wayland->cursor_surface);

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

	/* The limits on its size, its state and its shape, asked for before the first commit. */
	wayland_limits(window, &info->hints);
	wayland_set_state(window, info->flags);
	wayland_shape(wayland, window, info);

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

	/* The limits on its size. */
	if ((changed & X11_SURFACE_CHANGED_HINTS) != 0U)
		wayland_limits(window, &info->hints);

	/* Its state as the client asked (minimized, maximized, full-screen). */
	if ((changed & X11_SURFACE_CHANGED_FLAGS) != 0U)
		wayland_set_state(window, info->flags);

	/*
	 * Its shape, as the region the pointer is taken in (the buffer has no
	 * alpha, so the desktop still shows all of it).  The icon, the
	 * attention and the activation have no request the desktop takes.
	 */
	if ((changed & X11_SURFACE_CHANGED_SHAPE) != 0U)
		wayland_shape(window->wayland, window, info);
}

/*
 * Gives a window's shape to the desktop as the surface's input region
 * (unshaped: the whole surface), applied at the next commit.
 */
static void
wayland_shape(
	struct x11_wayland *wayland,
	struct x11_surface *window,
	const struct x11_surface_info *info)
{
	struct wl_region *region;
	unsigned index;

	/* Unshaped: the whole surface takes the pointer. */
	if (!info->shaped) {
		wl_surface_set_input_region(window->surface, NULL);
		return;
	}

	/* The shape's rectangles as a region. */
	region = wl_compositor_create_region(wayland->compositor);
	if (region == NULL)
		return;
	for (index = 0U; index < info->shape_count; index++)
		wl_region_add(region, info->shape[index].x, info->shape[index].y, (int32_t)info->shape[index].width, (int32_t)info->shape[index].height);

	/* Succeeded: the surface's input region, the region no longer needed. */
	wl_surface_set_input_region(window->surface, region);
	wl_region_destroy(region);
}

/*
 * Asks the desktop to keep a window within the limits its X client put on
 * its size (the smallest and largest size; the steps and the ratio are
 * the server's to keep).
 */
static void
wayland_limits(
	struct x11_surface *window,
	const struct x11_size_hints *hints)
{
	/* The smallest size (0: none). */
	xdg_toplevel_set_min_size(window->toplevel, (int32_t)hints->min_width, (int32_t)hints->min_height);

	/* Succeeded: the largest size (0: none). */
	xdg_toplevel_set_max_size(window->toplevel, (int32_t)hints->max_width, (int32_t)hints->max_height);
}

/*
 * Asks the desktop for the state the X client asked a window to have:
 * minimized, maximized or not, full-screen or not.  The desktop has no way
 * to bring a minimized window back; it comes back when the user does.
 */
static void
wayland_set_state(
	struct x11_surface *window,
	unsigned state)
{
	unsigned old;

	/* What changes. */
	old = window->state;
	window->state = state;

	/* Full-screen. */
	if ((state & X11_SURFACE_FULLSCREEN) != 0U && (old & X11_SURFACE_FULLSCREEN) == 0U)
		xdg_toplevel_set_fullscreen(window->toplevel, NULL);
	if ((state & X11_SURFACE_FULLSCREEN) == 0U && (old & X11_SURFACE_FULLSCREEN) != 0U)
		xdg_toplevel_unset_fullscreen(window->toplevel);

	/* Maximized. */
	if ((state & X11_SURFACE_MAXIMIZED) != 0U && (old & X11_SURFACE_MAXIMIZED) == 0U)
		xdg_toplevel_set_maximized(window->toplevel);
	if ((state & X11_SURFACE_MAXIMIZED) == 0U && (old & X11_SURFACE_MAXIMIZED) != 0U)
		xdg_toplevel_unset_maximized(window->toplevel);

	/* Succeeded: minimized. */
	if ((state & X11_SURFACE_ICONIC) != 0U && (old & X11_SURFACE_ICONIC) == 0U)
		xdg_toplevel_set_minimized(window->toplevel);
}

/*
 * Tells the server the state the desktop gave a window (maximized,
 * full-screen, or back from minimized), when it changed.
 */
static void
wayland_state_heard(
	struct x11_surface *window,
	unsigned state)
{
	/* The same state. */
	if (state == window->state)
		return;

	/* Succeeded: the server hears it. */
	window->state = state;
	if (window->wayland->server != NULL)
		x11server_backend_state(window->wayland->server, window->id, (int)state);
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

	/* The pointer is no longer in it, and its cursor is no window's. */
	if (wayland->pointer_window == window)
		wayland->pointer_window = NULL;
	if (wayland->cursor_window == window)
		wayland->cursor_window = NULL;

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
 * Makes the text the core copied from an X owner of CLIPBOARD or PRIMARY
 * the desktop's clipboard or primary selection: the backend keeps a copy
 * and answers the desktop's requests with it.  Returns 0, or an errno
 * value (ENOTSUP without the device, E2BIG, ENOMEM).
 */
static int
wayland_clipboard_offer(
	void *context,
	unsigned selection,
	const uint8_t *text,
	size_t size)
{
	struct x11_wayland *wayland;
	uint8_t *copy;
	int error;

	/* A text too long to pass is refused. */
	wayland = context;
	if (size > X11_CLIPBOARD_TEXT_MAX)
		return E2BIG;

	/* The backend's copy (an empty text is still text). */
	copy = malloc(size + 1U);
	if (copy == NULL)
		return ENOMEM;
	if (size != 0U)
		memcpy(copy, text, size);
	copy[size] = '\0';

	/* The primary selection, or the clipboard, with a new source. */
	if (selection == X11_SELECTION_PRIMARY) {
		error = wayland_primary_own(wayland, copy, size);
	} else {
		error = wayland_selection_own(wayland, copy, size);
	}
	if (error != 0) {
		free(copy);
		return error;
	}

	/* Succeeded: the source holds the copy. */
	return 0;
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
 * Makes an X client's text the desktop's selection: a new source of the
 * server's offers the text types and holds the text (which it takes),
 * and the source before goes.  Returns 0, or an errno value (ENOTSUP
 * without a data device, ENOMEM).
 */
static int
wayland_selection_own(
	struct x11_wayland *wayland,
	uint8_t *text,
	size_t length)
{
	struct wl_data_source *source;

	/* Only with a data device. */
	if (wayland->data_device == NULL)
		return ENOTSUP;

	/* The source and its types. */
	source = wl_data_device_manager_create_data_source(wayland->data_manager);
	if (source == NULL)
		return ENOMEM;
	(void)wl_data_source_add_listener(source, &wayland_source_listener, wayland);
	wl_data_source_offer(source, WAYLAND_TEXT_UTF8);
	wl_data_source_offer(source, WAYLAND_TEXT_PLAIN);
	wl_data_source_offer(source, WAYLAND_TEXT_X11);
	wl_data_source_offer(source, WAYLAND_TEXT_STRING);

	/* The selection, with the last input's serial; the server's source before goes. */
	wl_data_device_set_selection(wayland->data_device, source, wayland->serial);
	if (wayland->source != NULL)
		wl_data_source_destroy(wayland->source);
	free(wayland->source_text);
	wayland->source = source;
	wayland->source_text = text;
	wayland->source_length = length;

	/* Another client's offer is not the selection any more. */
	if (wayland->selection != NULL)
		wl_data_offer_destroy(wayland->selection);
	wayland->selection = NULL;
	wayland->selection_text = 0;

	/* Succeeded: sent at once. */
	(void)wl_display_flush(wayland->display);
	printf("X11 CLIPBOARD own bytes=%lu\n", (unsigned long)length);
	fflush(stdout);
	return 0;
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
 * Makes an X client's text the desktop's primary selection, as
 * wayland_selection_own does for the clipboard.  Returns 0, or an errno
 * value (ENOTSUP without a primary device, ENOMEM).
 */
static int
wayland_primary_own(
	struct x11_wayland *wayland,
	uint8_t *text,
	size_t length)
{
	struct zwp_primary_selection_source_v1 *source;

	/* Only with a primary device. */
	if (wayland->primary_device == NULL)
		return ENOTSUP;

	/* The source and its types. */
	source = zwp_primary_selection_device_manager_v1_create_source(wayland->primary_manager);
	if (source == NULL)
		return ENOMEM;
	(void)zwp_primary_selection_source_v1_add_listener(source, &wayland_primary_source_listener, wayland);
	zwp_primary_selection_source_v1_offer(source, WAYLAND_TEXT_UTF8);
	zwp_primary_selection_source_v1_offer(source, WAYLAND_TEXT_PLAIN);
	zwp_primary_selection_source_v1_offer(source, WAYLAND_TEXT_X11);
	zwp_primary_selection_source_v1_offer(source, WAYLAND_TEXT_STRING);

	/* The selection; the server's source before goes. */
	zwp_primary_selection_device_v1_set_selection(wayland->primary_device, source, wayland->serial);
	if (wayland->primary_source != NULL)
		zwp_primary_selection_source_v1_destroy(wayland->primary_source);
	free(wayland->primary_source_text);
	wayland->primary_source = source;
	wayland->primary_source_text = text;
	wayland->primary_source_length = length;

	/* Another client's offer is not the selection any more. */
	if (wayland->primary != NULL)
		zwp_primary_selection_offer_v1_destroy(wayland->primary);
	wayland->primary = NULL;
	wayland->primary_text = 0;

	/* Succeeded: sent at once. */
	(void)wl_display_flush(wayland->display);
	printf("X11 PRIMARY own bytes=%lu\n", (unsigned long)length);
	fflush(stdout);
	return 0;
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
	const uint32_t *state;
	size_t count;
	size_t index;
	unsigned heard;
	int activated;

	/* The states the desktop gives (32-bit values): maximized, full-screen, activated. */
	(void)toplevel;
	window = data;
	heard = 0U;
	activated = 0;
	state = states->data;
	count = states->size / sizeof(uint32_t);
	for (index = 0U; index < count; index++) {
		if (state[index] == XDG_TOPLEVEL_STATE_MAXIMIZED)
			heard |= X11_SURFACE_MAXIMIZED;
		if (state[index] == XDG_TOPLEVEL_STATE_FULLSCREEN)
			heard |= X11_SURFACE_FULLSCREEN;
		if (state[index] == XDG_TOPLEVEL_STATE_ACTIVATED)
			activated = 1;
	}

	/*
	 * After the first configure, the server hears them; a minimized window
	 * stays so until the desktop activates it.
	 */
	if (window->configured) {
		if ((window->state & X11_SURFACE_ICONIC) != 0U && !activated)
			heard |= X11_SURFACE_ICONIC;
		wayland_state_heard(window, heard);
	}

	/* A size left to the window keeps its own, within the desktop's bounds (the next dispatch asks for it). */
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
		x11server_backend_configure(window->wayland->server, window->id, X11_SURFACE_ANY, X11_SURFACE_ANY, width, height);
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
			x11server_backend_configure(wayland->server, window->id, X11_SURFACE_ANY, X11_SURFACE_ANY, (int)width, (int)height);
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

	/* A surface that is not one of the windows is ignored. */
	(void)pointer;
	wayland = data;
	window = wayland_window_of(wayland, surface);
	wayland->pointer_window = window;
	if (window == NULL)
		return;

	/* The window's cursor, else the arrow until the core tells it. */
	wayland->pointer_serial = serial;
	if (wayland->cursor_window != window)
		wayland_cursor_arrow(wayland);
	wayland_cursor_show(wayland);

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
	struct x11_surface *window;

	/* The pointer is in none of the windows. */
	(void)pointer;
	(void)serial;
	wayland = data;
	window = wayland_window_of(wayland, surface);
	wayland->pointer_window = NULL;

	/* Succeeded: the core hears that it left the X windows. */
	if (window != NULL && wayland->server != NULL)
		x11server_backend_leave(wayland->server, window->id, 0);
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

	/* The X window of the surface (one the core opened). */
	window = wayland_window_of(wayland, surface);
	if (window == NULL)
		return;

	/* A minimized window the desktop brought back is no longer minimized. */
	if ((window->state & X11_SURFACE_ICONIC) != 0U)
		wayland_state_heard(window, window->state & ~X11_SURFACE_ICONIC);

	/* Succeeded: it gets the focus. */
	if (wayland->server != NULL)
		x11server_backend_enter(wayland->server, window->id, 1);
}

/*
 * Focus leaves a window: the keys held are released for the core (their
 * releases go elsewhere), and the X focus leaves the window.
 */
static void
wayland_keyboard_leave(
	void *data,
	struct wl_keyboard *keyboard,
	uint32_t serial,
	struct wl_surface *surface)
{
	struct x11_wayland *wayland;
	struct x11_surface *window;
	uint32_t key;

	/* Before the core is attached nothing was told. */
	(void)keyboard;
	(void)serial;
	wayland = data;
	if (wayland->server == NULL)
		return;

	/* Each key held, released. */
	for (key = 0U; key < WAYLAND_KEYS; key++) {
		if (!wayland->held[key])
			continue;
		wayland->held[key] = 0U;
		x11server_backend_key(wayland->server, (uint8_t)(key + X11_KEYMAP_EVDEV_OFFSET), 0, 0U);
	}

	/* Succeeded: the X window of the surface (one the core opened) loses the focus. */
	window = wayland_window_of(wayland, surface);
	if (window != NULL)
		x11server_backend_leave(wayland->server, window->id, 1);
}

/* A key is pressed or released: the server hears the X keycode of its place on the keyboard. */
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
	int pressed;

	/* The serial, which a selection is set with; a code past the keycodes, or one before the core is attached, is ignored. */
	(void)keyboard;
	wayland = data;
	wayland->serial = serial;
	if (key >= WAYLAND_KEYS || wayland->server == NULL)
		return;

	/* A release of a key not held (its press went to another client of the desktop) says nothing. */
	pressed = 0;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		pressed = 1;
	if (!pressed && !wayland->held[key])
		return;

	/* Succeeded: the server hears it. */
	wayland->held[key] = (uint8_t)pressed;
	x11server_backend_key(wayland->server, (uint8_t)(key + X11_KEYMAP_EVDEV_OFFSET), pressed, time);
}

/* The modifiers the desktop reports are not used: the core makes them from the keys. */
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
	/* The core makes the modifiers from the keys it hears. */
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)depressed;
	(void)latched;
	(void)locked;
	(void)group;
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
 * A client asks for the X client's text: the copy the source holds is
 * written into its descriptor, which is then closed so the reader sees
 * the end.  A type that is not text is closed at once.
 */
static void
wayland_source_send(
	void *data,
	struct wl_data_source *source,
	const char *mime_type,
	int32_t fd)
{
	struct x11_wayland *wayland;
	size_t done;
	int text;

	/* Only text of the server's own source. */
	wayland = data;
	text = wayland_text_type(mime_type);
	if (!text || source != wayland->source || wayland->source_text == NULL) {
		close(fd);
		return;
	}

	/* Succeeded: the copy is written and the descriptor closed. */
	printf("X11 CLIPBOARD send mime=%s\n", mime_type);
	done = wayland_pipe_write(fd, wayland->source_text, wayland->source_length);
	printf("X11 CLIPBOARD sent bytes=%lu\n", (unsigned long)done);
	fflush(stdout);
}

/* Another client took the selection: the server's source goes (the new selection follows). */
static void
wayland_source_cancelled(
	void *data,
	struct wl_data_source *source)
{
	struct x11_wayland *wayland;

	/* The source, and the copy it held. */
	wayland = data;
	wl_data_source_destroy(source);
	if (wayland->source == source) {
		wayland->source = NULL;
		free(wayland->source_text);
		wayland->source_text = NULL;
		wayland->source_length = 0U;
	}
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

/*
 * Writes a text into a descriptor a desktop client reads, waiting at most
 * WAYLAND_READ_MS for room each time; closes it so the reader sees the
 * end.  Returns how many bytes were written (fewer when the reader went
 * or was too slow).
 */
static size_t
wayland_pipe_write(
	int fd,
	const uint8_t *text,
	size_t length)
{
	struct pollfd descriptor;
	ssize_t wrote;
	size_t done;
	int flags;
	int status;

	/* The descriptor never blocks the server. */
	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	/* All of it, while the reader takes it. */
	done = 0U;
	while (done < length) {
		/* Waits for room. */
		descriptor.fd = fd;
		descriptor.events = POLLOUT;
		descriptor.revents = 0;
		status = poll(&descriptor, 1, WAYLAND_READ_MS);
		if (status <= 0)
			break;

		/* As much as fits; a failure other than a full pipe ends it. */
		wrote = write(fd, text + done, length - done);
		if (wrote < 0 && errno == EAGAIN)
			continue;
		if (wrote <= 0)
			break;
		done += (size_t)wrote;
	}

	/* Succeeded: the reader sees the end. */
	(void)close(fd);
	return done;
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
 * A client asks for the X client's PRIMARY text: the copy the source
 * holds is written into its descriptor, which is then closed.  A type
 * that is not text is closed at once.
 */
static void
wayland_primary_send(
	void *data,
	struct zwp_primary_selection_source_v1 *source,
	const char *mime_type,
	int32_t fd)
{
	struct x11_wayland *wayland;
	size_t done;
	int text;

	/* Only text of the server's own source. */
	wayland = data;
	text = wayland_text_type(mime_type);
	if (!text || source != wayland->primary_source || wayland->primary_source_text == NULL) {
		close(fd);
		return;
	}

	/* Succeeded: the copy is written and the descriptor closed. */
	printf("X11 PRIMARY send mime=%s\n", mime_type);
	done = wayland_pipe_write(fd, wayland->primary_source_text, wayland->primary_source_length);
	printf("X11 PRIMARY sent bytes=%lu\n", (unsigned long)done);
	fflush(stdout);
}

/* Another client took the primary selection: the server's source goes. */
static void
wayland_primary_cancelled(
	void *data,
	struct zwp_primary_selection_source_v1 *source)
{
	struct x11_wayland *wayland;

	/* The source, and the copy it held. */
	wayland = data;
	zwp_primary_selection_source_v1_destroy(source);
	if (wayland->primary_source == source) {
		wayland->primary_source = NULL;
		free(wayland->primary_source_text);
		wayland->primary_source_text = NULL;
		wayland->primary_source_length = 0U;
	}
	printf("X11 PRIMARY cancelled\n");
	fflush(stdout);
}

/*
 * Shows the cursor of an X window (the core's cursor function): its
 * image, or the arrow for None and for a cursor without one.  It is
 * shown at once when the pointer is over the window, and again when the
 * pointer enters it.
 */
static void
wayland_cursor(
	void *context,
	struct x11_surface *window,
	const struct x11_cursor_image *image)
{
	struct x11_wayland *wayland;
	int error;

	/* The image, else the arrow. */
	wayland = context;
	error = EINVAL;
	if (image != NULL && image->pixels != NULL && image->width > 0 && image->height > 0 &&
	    image->width <= WAYLAND_CURSOR_MAX && image->height <= WAYLAND_CURSOR_MAX)
		error = wayland_cursor_draw(wayland, image->pixels, image->width, image->height, image->hot_x, image->hot_y);
	if (error != 0)
		wayland_cursor_arrow(wayland);
	wayland->cursor_window = window;

	/* Shown when the pointer is over the window. */
	if (wayland->pointer_window == window)
		wayland_cursor_show(wayland);
}

/*
 * Draws a cursor into a new buffer of the cursor surface: pixels of
 * 0xAARRGGBB, not premultiplied (Wayland's ARGB8888 is), width by height,
 * with its hot spot (kept inside the image).  The buffer shown before is
 * let go.  Returns 0, or an errno value.
 */
static int
wayland_cursor_draw(
	struct x11_wayland *wayland,
	const uint32_t *pixels,
	int width,
	int height,
	int hot_x,
	int hot_y)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint32_t *memory;
	uint32_t pixel;
	uint32_t alpha;
	char name[64];
	size_t bytes;
	size_t index;
	int descriptor;
	int error;

	/* The cursor surface, once. */
	if (wayland->cursor_surface == NULL) {
		wayland->cursor_surface = wl_compositor_create_surface(wayland->compositor);
		if (wayland->cursor_surface == NULL)
			return ENOMEM;
	}

	/* Anonymous shared memory for the image. */
	bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
	(void)snprintf(name, sizeof(name), "/xserver-%ld-cursor", (long)getpid());
	descriptor = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0)
		return errno;
	(void)shm_unlink(name);
	error = ftruncate(descriptor, (off_t)bytes);
	if (error != 0) {
		(void)close(descriptor);
		return EIO;
	}
	memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	if (memory == MAP_FAILED) {
		(void)close(descriptor);
		return EIO;
	}

	/* The pixels, premultiplied. */
	for (index = 0U; index < (size_t)width * (size_t)height; index++) {
		pixel = pixels[index];
		alpha = pixel >> 24;
		memory[index] = (alpha << 24) |
				((((pixel >> 16) & 0xFFU) * alpha + 127U) / 255U) << 16 |
				((((pixel >> 8) & 0xFFU) * alpha + 127U) / 255U) << 8 |
				(((pixel & 0xFFU) * alpha + 127U) / 255U);
	}
	(void)munmap(memory, bytes);

	/* The buffer (the desktop keeps its own reference to the memory). */
	pool = wl_shm_create_pool(wayland->shm, descriptor, (int32_t)bytes);
	(void)close(descriptor);
	if (pool == NULL)
		return ENOMEM;
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	if (buffer == NULL)
		return ENOMEM;

	/* On the surface, in place of the one before. */
	wl_surface_attach(wayland->cursor_surface, buffer, 0, 0);
	wl_surface_damage(wayland->cursor_surface, 0, 0, width, height);
	wl_surface_commit(wayland->cursor_surface);
	if (wayland->cursor_buffer != NULL)
		wl_buffer_destroy(wayland->cursor_buffer);
	wayland->cursor_buffer = buffer;

	/* Succeeded: its hot spot, inside it. */
	if (hot_x < 0 || hot_x >= width)
		hot_x = 0;
	if (hot_y < 0 || hot_y >= height)
		hot_y = 0;
	wayland->cursor_hot_x = hot_x;
	wayland->cursor_hot_y = hot_y;
	return 0;
}

/*
 * Draws the backend's own arrow (the pointer of a window with no X cursor
 * of its own), black with a white inside, its hot spot at its tip.
 */
static void
wayland_cursor_arrow(
	struct x11_wayland *wayland)
{
	static const char *const rows[] = {
		"X           ",
		"XX          ",
		"X.X         ",
		"X..X        ",
		"X...X       ",
		"X....X      ",
		"X.....X     ",
		"X......X    ",
		"X.......X   ",
		"X........X  ",
		"X.........X ",
		"X......XXXXX",
		"X...X..X    ",
		"X..XX..X    ",
		"X.X  X..X   ",
		"XX   X..X   ",
		"X     X..X  ",
		"      X..X  ",
		"       XX   "
	};
	uint32_t pixels[sizeof(rows) / sizeof(rows[0])][12];
	unsigned row;
	unsigned column;

	/* The rows: X black, . white, the rest clear. */
	for (row = 0U; row < sizeof(rows) / sizeof(rows[0]); row++) {
		for (column = 0U; column < 12U; column++) {
			pixels[row][column] = 0U;
			if (rows[row][column] == 'X')
				pixels[row][column] = 0xFF000000U;
			if (rows[row][column] == '.')
				pixels[row][column] = 0xFFFFFFFFU;
		}
	}

	/* Drawn (without memory for it, the cursor stays as it was). */
	(void)wayland_cursor_draw(wayland, &pixels[0][0], 12, (int)(sizeof(rows) / sizeof(rows[0])), 0, 0);
}

/* Shows the cursor surface as the pointer, with the serial of the last entering. */
static void
wayland_cursor_show(
	struct x11_wayland *wayland)
{
	/* Only with a pointer and a cursor drawn. */
	if (wayland->pointer == NULL || wayland->cursor_buffer == NULL)
		return;
	wl_pointer_set_cursor(wayland->pointer, wayland->pointer_serial, wayland->cursor_surface,
			      wayland->cursor_hot_x, wayland->cursor_hot_y);
	(void)wl_display_flush(wayland->display);
}
