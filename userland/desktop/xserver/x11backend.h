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
 * server), the characters an input method typed
 * (x11server_backend_text), the size of the system's frame around a
 * surface, and the root window's size and the monitors on it
 * (x11server_backend_screen, whenever they change; a backend that never
 * tells has one monitor, the whole root window).  The core is the X
 * windows' window manager (wm.c): it decides what a close means to the X
 * client and keeps ICCCM's and EWMH's state of each window.  zedBSD's
 * backend is the Wayland desktop (wayland.c).
 */

#ifndef KEILAND_X11BACKEND_H
#define KEILAND_X11BACKEND_H

#include "userland/desktop/xserver/x11server.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The kind of a surface: a decorated top-level X window of the rootless
 * server, an override-redirect one (a menu or a tooltip: no decoration,
 * never activated), or the one window of the whole root window of a
 * rooted server.  A root surface has the root window's size, which does
 * not change; the backend places it itself (its x and y are 0), keeps the
 * user from sizing it, and gives the pointer's place on it as the place on
 * the root window.  Every key it hears goes to X, and its close ends the
 * server's clients.
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
 * A surface whose window asks for the user's attention (ICCCM's
 * UrgencyHint, EWMH's _NET_WM_STATE_DEMANDS_ATTENTION): the system marks
 * it (Windows flashes its button) until it is activated.
 */
#define X11_SURFACE_URGENT	(1U << 3)

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
 * What else an update brings: the surface's shape (info's shape), its
 * icon (info's icon), and a request that the system bring the surface to
 * the front and activate it (a client's _NET_ACTIVE_WINDOW; the system
 * may only mark it when it does not let a program take the front).
 */
#define X11_SURFACE_CHANGED_SHAPE	(1U << 6)
#define X11_SURFACE_CHANGED_ICON	(1U << 7)
#define X11_SURFACE_CHANGED_ACTIVATE	(1U << 8)

/*
 * The surface's info carries the window's shape (SHAPE's bounding
 * region) and its icon (_NET_WM_ICON), its flags may ask for the user's
 * attention, an update may ask for the surface to be activated, and the
 * backend tells the core the size of the system's frame around a surface
 * (x11server_backend_frame).
 */
#define X11_BACKEND_SHAPE	1

/*
 * The core is the top-level windows' window manager: a surface's info
 * carries the limits on its size (struct x11_size_hints), its state may
 * be maximized or full-screen, and the backend tells the core when the
 * system took the pointer's capture away (x11server_backend_capture_lost).
 */
#define X11_BACKEND_WM	1

/*
 * A surface's info says whether its X client asked for the place
 * (default_place): a window it left to the window manager is placed by
 * the system, which tells the core the place it chose.
 */
#define X11_BACKEND_PLACE	1

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
 *
 * default_place is nonzero for a decorated window whose client asked for
 * no place of its own (ICCCM's WM_NORMAL_HINTS has no USPosition, and no
 * PPosition other than the root window's corner, which toolkits give when
 * they mean none).  A backend that can place its windows then puts the
 * surface where the system puts a new window that names no place, and
 * tells the core that place (x11server_backend_configure); x and y still
 * hold the X window's place until it does.  The backend reads it when it
 * opens the surface.
 */
struct x11_surface_info {
	unsigned kind;
	int32_t x;
	int32_t y;
	int default_place;
	unsigned width;
	unsigned height;
	const char *title;
	const char *class_name;
	struct x11_surface *owner;
	unsigned flags;
	struct x11_size_hints hints;

	/*
	 * The window's shape: the rectangles of its pixels the surface shows
	 * and takes the pointer in (from the window's corner; they do not
	 * overlap), shape_count of them.  shaped 0: the whole window (shape is
	 * NULL).  The system clips the surface to them (Windows'
	 * SetWindowRgn); Wayland takes them as the input region only.
	 */
	int shaped;
	const struct x11_rect *shape;
	unsigned shape_count;

	/*
	 * The window's icon (_NET_WM_ICON, the size nearest the system's):
	 * icon_width by icon_height pixels of ARGB, unpremultiplied, rows top
	 * to bottom.  NULL: none (the system's own icon).
	 */
	const uint32_t *icon;
	unsigned icon_width;
	unsigned icon_height;
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
 * The backend tells the core the system's monitors (x11server_backend_screen):
 * the core answers RANDR's and XINERAMA's questions with them.
 */
#define X11_BACKEND_MONITORS	1

/* The most monitors the core keeps, and the longest name of one with its NUL. */
#define X11_MONITORS_MAX	16U
#define X11_MONITOR_NAME_MAX	32U

/*
 * One of the system's monitors as the X clients see it: its place on the
 * root window and its size (X's pixels), its size in millimetres (0: the
 * core reckons it at 96 pixels an inch), its refresh rate in hertz (0:
 * 60), whether it is the system's main monitor, and its name (ASCII;
 * empty: the core names it).  RANDR shows each as an output with its own
 * CRTC and mode, and XINERAMA as one of its screens.
 */
struct x11_monitor {
	int32_t x;
	int32_t y;
	unsigned width;
	unsigned height;
	unsigned width_mm;
	unsigned height_mm;
	unsigned refresh;
	int primary;
	char name[X11_MONITOR_NAME_MAX];
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
void x11server_backend_frame(struct x11server *server, uint32_t window, unsigned left, unsigned right, unsigned top, unsigned bottom);
void x11server_backend_screen(struct x11server *server, unsigned width, unsigned height, const struct x11_monitor *monitors, unsigned count);

/* What the core gives the backend: a size within a surface's limits. */
void x11server_size_hints_apply(const struct x11_size_hints *hints, unsigned *width, unsigned *height);

/* The keyboard's map of the US layout by evdev key codes (the core's own until a backend gives one). */
void x11_keymap_us(struct x11_keymap *keymap);

#endif
