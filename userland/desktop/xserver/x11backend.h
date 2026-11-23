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
 * surface to show them through, passes the clipboard across, and asks
 * for the pointer's capture, place and warp, for the bell and for the
 * cursor shown over a surface.  The
 * backend calls the core with what its system reports: keys (by their
 * place on the keyboard), the keyboard's map, the pointer, a window
 * entered and left, given a size, asked to close, minimized, maximized or
 * made full-screen, the pointer's capture taken by another program, and
 * whether another program's text is the system's clipboard
 * (x11server_backend_clipboard, also once when the backend is given the
 * server), and the characters an input method typed
 * (x11server_backend_text).  The core is the X windows' window manager
 * (wm.c): it decides what a close means to the X client and keeps ICCCM's
 * and EWMH's state of each window.  zedBSD's backend is the Wayland desktop (wayland.c).
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

/*
 * The state of a surface: minimized (ICCCM's Iconic), maximized and
 * full-screen (EWMH's _NET_WM_STATE).  The core gives it in an update's
 * flags; the backend tells the core the state the system or the user gave
 * the surface (x11server_backend_state).
 */
#define X11_SURFACE_ICONIC	(1U << 0)
#define X11_SURFACE_MAXIMIZED	(1U << 1)
#define X11_SURFACE_FULLSCREEN	(1U << 2)

/*
 * What an update of a surface changes: its size, its place on the root
 * window, its title, its owner, its state, and the limits on its size.
 */
#define X11_SURFACE_CHANGED_SIZE	(1U << 0)
#define X11_SURFACE_CHANGED_POSITION	(1U << 1)
#define X11_SURFACE_CHANGED_TITLE	(1U << 2)
#define X11_SURFACE_CHANGED_OWNER	(1U << 3)
#define X11_SURFACE_CHANGED_FLAGS	(1U << 4)
#define X11_SURFACE_CHANGED_HINTS	(1U << 5)

/*
 * The core is the top-level windows' window manager: a surface's info
 * carries the limits on its size (struct x11_size_hints), its state may
 * be maximized or full-screen, and the backend tells the core when the
 * system took the pointer's capture away (x11server_backend_capture_lost).
 */
#define X11_BACKEND_WM	1

/* The selections the clipboard passes across: X's CLIPBOARD and PRIMARY. */
#define X11_SELECTION_CLIPBOARD	0U
#define X11_SELECTION_PRIMARY	1U

/* The clipboard's functions are clipboard_offer and clipboard_read (the core copies an X owner's text at once). */
#define X11_BACKEND_CLIPBOARD_OFFER	1

/* The longest text the clipboard passes either way (16 MiB). */
#define X11_CLIPBOARD_TEXT_MAX	(16U * 1024U * 1024U)

/* The modifier bits of a key or button event's state (X's). */
#define X11_SHIFT_MASK		(1U << 0)
#define X11_CONTROL_MASK	(1U << 2)
#define X11_ALT_MASK		(1U << 3)

/*
 * The core takes the keyboard's map from the backend (x11server_backend_keymap)
 * and keys by their place on the keyboard (x11server_backend_key).
 */
#define X11_BACKEND_KEYMAP	1

/* The backend may show X's cursors (the cursor function). */
#define X11_BACKEND_CURSOR	1

/*
 * The keyboard's map: the keycodes (an evdev key code plus 8, so 8 to
 * 255), the keysyms each carries, and the modifiers.
 */
#define X11_KEYMAP_KEYCODES		256U
#define X11_KEYMAP_KEYCODE_FIRST	8U
#define X11_KEYMAP_COLUMNS		4U
#define X11_KEYMAP_MODIFIERS		8U
#define X11_KEYMAP_MODIFIER_KEYS	4U

/* The offset of an evdev key code in its X keycode. */
#define X11_KEYMAP_EVDEV_OFFSET		8U

/*
 * The core takes characters the backend's input method typed
 * (x11server_backend_text): each reaches the X clients as a key press and
 * release of a keycode the core lends it, whose keysym is the character's.
 */
#define X11_BACKEND_TEXT	1

/* A system window the backend keeps (its own type). */
struct x11_surface;

/*
 * The keyboard's map a backend gives the core, as the core protocol's
 * clients read it (GetKeyboardMapping, GetModifierMapping).
 *
 * keysyms holds for each keycode its keysyms: alone, with Shift, and the
 * same two in the second group, which a core client chooses while the
 * Mode_switch keysym's modifier is held (0 is NoSymbol; keycodes 0 to 7
 * are never used).  modifiers holds for Shift, Lock, Control and Mod1 to
 * Mod5 their keycodes, 0 filling the rest.
 */
struct x11_keymap {
	uint32_t keysyms[X11_KEYMAP_KEYCODES][X11_KEYMAP_COLUMNS];
	uint8_t modifiers[X11_KEYMAP_MODIFIERS][X11_KEYMAP_MODIFIER_KEYS];
};

/*
 * The limits a client puts on its top-level window's size (ICCCM's
 * WM_NORMAL_HINTS), in X's pixels, which the system keeps while the user
 * sizes the window: the smallest and largest size (0: no limit), the base
 * size and the steps the size grows in from it (a terminal's character
 * cells; 0 or 1: any size), and the smallest and largest ratio of width to
 * height, each as two numbers (0: no limit).  The core fills it with the
 * defaults ICCCM gives a field the client left out.
 */
struct x11_size_hints {
	unsigned min_width;
	unsigned min_height;
	unsigned max_width;
	unsigned max_height;
	unsigned base_width;
	unsigned base_height;
	unsigned width_inc;
	unsigned height_inc;
	unsigned min_aspect_x;
	unsigned min_aspect_y;
	unsigned max_aspect_x;
	unsigned max_aspect_y;
};

/*
 * What a surface shows: its kind, its X window's place on the root window
 * and size (X's pixels), its title (UTF-8), the X client's class (NULL:
 * none; the backend names it itself), the surface of the top-level window
 * it is transient for (WM_TRANSIENT_FOR; NULL: none), its state
 * (X11_SURFACE_ICONIC, MAXIMIZED, FULLSCREEN) and the limits on its size.
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
	struct x11_size_hints hints;
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
 * A cursor the backend shows over a surface: its shape when it is one of
 * the standard cursors (the cursor font's glyph, an XC_* number of X's
 * cursorfont.h; -1: none, the image is its shape), and its image: pixels
 * of 0xAARRGGBB (the colour not premultiplied, alpha 0 to 255), width by
 * height, with the hot spot, the
 * pixel the pointer's place is on (pixels NULL: no image, the shape
 * alone).  A backend shows its own cursor of a standard shape when it has
 * one.
 */
struct x11_cursor_image {
	int shape;
	int width;
	int height;
	int hot_x;
	int hot_y;
	const uint32_t *pixels;
};

/*
 * One pointer event from the backend, in the root window's coordinates:
 * where the pointer is, and at most one button that changed.
 */
struct x11_pointer_frame {
	int x;
	int y;
	uint32_t time;

	/*
	 * The buttons held before the frame (X's bits: 1 left, 2 middle, 4
	 * right), which the backend keeps for itself: the core counts the
	 * buttons from the presses and releases it hears.
	 */
	uint16_t buttons_before;

	/* The physical button that changed (1 to 9; 0 for a move), whether it went down, and the buttons after. */
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
 * The clipboard: clipboard_offer makes the text of an X client's CLIPBOARD
 * or PRIMARY (UTF-8, size bytes) the system's clipboard or primary
 * selection at once, so that it outlives the X client; the backend keeps
 * a copy of it.  It returns 0, EACCES when the backend's policy refuses
 * it now, ENOTSUP for a selection the system does not take, or another
 * errno value.  clipboard_read reads another program's text of the
 * system's selection for an X client (malloc'd text the core frees, a NUL
 * after its length; 0 or an errno value).
 *
 * The pointer and the bell, each of which may be NULL: capture keeps the
 * system's pointer events on a surface while an X client grabs the
 * pointer (on nonzero) and lets them go (on 0), so that a click outside
 * the X windows reaches the grabbing client.  pointer tells where the
 * system's pointer is on the root window, inside an X window or not (0,
 * or an errno value when it cannot tell).  warp moves it there (0, or an
 * errno value).  bell rings at a volume from -100 to 100 percent of the
 * base volume.
 *
 * cursor (which may be NULL: the system's pointer is shown) sets the
 * cursor shown while the pointer is over a surface (image NULL: the
 * system's own pointer); the backend copies what it needs.
 */
struct x11_backend_ops {
	struct x11_surface *(*open)(void *context, uint32_t window, const struct x11_surface_info *info);
	void (*update)(void *context, struct x11_surface *surface, const struct x11_surface_info *info, unsigned changed);
	void (*close)(void *context, struct x11_surface *surface);
	int (*vulkan_surface)(void *context, struct x11_surface *surface, void *instance, void *result);
	int (*present)(void *context, struct x11_surface *surface, const uint32_t *pixels, size_t stride, const struct x11_rect *parts, unsigned count);
	int (*clipboard_offer)(void *context, unsigned selection, const uint8_t *text, size_t size);
	int (*clipboard_read)(void *context, unsigned selection, uint8_t **text, size_t *size);
	void (*capture)(void *context, struct x11_surface *surface, int on);
	int (*pointer)(void *context, int *x, int *y);
	int (*warp)(void *context, int x, int y);
	void (*bell)(void *context, int percent);
	void (*cursor)(void *context, struct x11_surface *surface, const struct x11_cursor_image *image);
};

/* What the backend tells the core. */
void x11server_backend_key(struct x11server *server, uint8_t keycode, int pressed, uint32_t time);
void x11server_backend_keymap(struct x11server *server, const struct x11_keymap *keymap);
void x11server_backend_text(struct x11server *server, uint32_t character, uint32_t time);
void x11server_backend_pointer(struct x11server *server, const struct x11_pointer_frame *frame);
void x11server_backend_enter(struct x11server *server, uint32_t window, int keyboard);
void x11server_backend_leave(struct x11server *server, uint32_t window, int keyboard);
void x11server_backend_configure(struct x11server *server, uint32_t window, int32_t x, int32_t y, int width, int height);
void x11server_backend_close(struct x11server *server, uint32_t window);
void x11server_backend_state(struct x11server *server, uint32_t window, int state);
void x11server_backend_capture_lost(struct x11server *server, uint32_t time);
void x11server_backend_clipboard(struct x11server *server, unsigned selection, int text);

/* What the core gives the backend: a size within a surface's limits. */
void x11server_size_hints_apply(const struct x11_size_hints *hints, unsigned *width, unsigned *height);

/* The keyboard's map of the US layout by evdev key codes (the core's own until a backend gives one). */
void x11_keymap_us(struct x11_keymap *keymap);

#endif
