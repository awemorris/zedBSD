/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boundary between xserver's core and a backend, the part that shows
 * the server on a system's own windows.
 *
 * The core calls the backend through a table of functions: it opens a
 * system window (a surface) for each mapped top-level X window, keeps its
 * size, place and title in step, hands it pixels or asks it for a Vulkan
 * surface to show them through, and passes the clipboard across.  The
 * backend calls the core with what its system reports: keys, the
 * pointer, a window entered, given a size, asked to close or minimized,
 * and the system's clipboard.  zedBSD's backend is the Wayland desktop
 * (wayland.c).
 */

#ifndef KEILAND_X11BACKEND_H
#define KEILAND_X11BACKEND_H

#include "userland/desktop/xserver/x11server.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The kind of a surface: a decorated top-level X window of the rootless
 * server, an override-redirect one (a menu or a tooltip: no decoration,
 * never activated), or the one window of the whole root window.
 */
#define X11_SURFACE_TOPLEVEL	0U
#define X11_SURFACE_POPUP	1U
#define X11_SURFACE_ROOT	2U

/* No place: the system places the surface, or does not say where it is (x11server_backend_configure). */
#define X11_SURFACE_ANY		INT32_MIN

/* The state of a surface: minimized (ICCCM's Iconic). */
#define X11_SURFACE_ICONIC	(1U << 0)

/* What an update of a surface changes: its size, its place on the root window, its title, its owner, its state. */
#define X11_SURFACE_CHANGED_SIZE	(1U << 0)
#define X11_SURFACE_CHANGED_POSITION	(1U << 1)
#define X11_SURFACE_CHANGED_TITLE	(1U << 2)
#define X11_SURFACE_CHANGED_OWNER	(1U << 3)
#define X11_SURFACE_CHANGED_FLAGS	(1U << 4)

/* The selections the clipboard passes across: X's CLIPBOARD and PRIMARY. */
#define X11_SELECTION_CLIPBOARD	0U
#define X11_SELECTION_PRIMARY	1U

/* The modifier bits of a key or button event's state (X's). */
#define X11_SHIFT_MASK		(1U << 0)
#define X11_CONTROL_MASK	(1U << 2)
#define X11_ALT_MASK		(1U << 3)

/* A system window the backend keeps (its own type). */
struct x11_surface;

/*
 * What a surface shows: its kind, its X window's place on the root window
 * and size (X's pixels), its title (UTF-8), the X client's class (NULL:
 * none; the backend names it itself), the surface of the top-level window
 * it is transient for (WM_TRANSIENT_FOR; NULL: none) and its state
 * (X11_SURFACE_ICONIC).
 */
struct x11_surface_info {
	unsigned kind;
	int32_t x;
	int32_t y;
	unsigned width;
	unsigned height;
	const char *title;
	const char *class_name;
	struct x11_surface *owner;
	unsigned flags;
};

/*
 * One rectangle of a surface, in its own pixels from its corner.
 */
struct x11_rect {
	int32_t x;
	int32_t y;
	unsigned width;
	unsigned height;
};

/*
 * One pointer event from the backend, in the root window's coordinates:
 * where the pointer is, and at most one button that changed.
 */
struct x11_pointer_frame {
	int x;
	int y;
	uint32_t time;

	/* The buttons held before the frame (X's bits: 1 left, 2 middle, 4 right). */
	uint16_t buttons_before;

	/* The button that changed (1, 2 or 3; 0 for a move), whether it went down, and the buttons after. */
	uint8_t button;
	int pressed;
	uint16_t buttons_after;
};

/*
 * The backend's functions, each called with the context given in the
 * server's options.
 *
 * open makes a surface for an X window (NULL when it cannot), update
 * brings it in step with what changed, close ends it.  vulkan_surface
 * makes a VkSurfaceKHR (into *result) on the instance given, returning 0
 * or an errno value; present shows pixels (rows of the surface's width,
 * stride pixels apart) with the parts that changed, returning 0, EBUSY
 * while the system still holds the last frame, or another errno value.
 *
 * The clipboard: clipboard_own makes the X owner's text the system's
 * selection (own nonzero) or gives that up; clipboard_has_text tells
 * whether another program's text is the system's selection; clipboard_read
 * reads it (malloc'd text the core frees, a NUL after its length; 0 or an
 * errno value); clipboard_answer gives a request the system made
 * (x11server_backend_clipboard_request) its text, NULL for none, and
 * returns how many bytes it delivered.
 */
struct x11_backend_ops {
	struct x11_surface *(*open)(void *context, uint32_t window, const struct x11_surface_info *info);
	void (*update)(void *context, struct x11_surface *surface, const struct x11_surface_info *info, unsigned changed);
	void (*close)(void *context, struct x11_surface *surface);
	int (*vulkan_surface)(void *context, struct x11_surface *surface, void *instance, void *result);
	int (*present)(void *context, struct x11_surface *surface, const uint32_t *pixels, size_t stride, const struct x11_rect *parts, unsigned count);
	int (*clipboard_own)(void *context, unsigned selection, int own);
	int (*clipboard_has_text)(void *context, unsigned selection);
	int (*clipboard_read)(void *context, unsigned selection, uint8_t **text, size_t *size);
	size_t (*clipboard_answer)(void *context, uint32_t request, const uint8_t *text, size_t size);
};

/* What the backend tells the core. */
void x11server_backend_key(struct x11server *server, uint8_t keycode, int pressed, uint32_t time, uint16_t state);
void x11server_backend_pointer(struct x11server *server, const struct x11_pointer_frame *frame);
void x11server_backend_enter(struct x11server *server, uint32_t window, int keyboard);
void x11server_backend_configure(struct x11server *server, uint32_t window, int32_t x, int32_t y, int width, int height);
void x11server_backend_close(struct x11server *server, uint32_t window);
void x11server_backend_state(struct x11server *server, uint32_t window, int iconic);
void x11server_backend_clipboard(struct x11server *server, unsigned selection, int text);
void x11server_backend_clipboard_request(struct x11server *server, unsigned selection, uint32_t request);

/*
 * The X keycode an evdev key types in the shift and caps lock states given
 * (0 for none): the core's keyboard model gives each character the keycode
 * of the character plus 8, US layout.
 */
uint8_t x11_keymap_keycode(uint16_t code, int shifted, int caps_lock);

#endif
