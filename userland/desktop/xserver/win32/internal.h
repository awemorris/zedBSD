/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server's Windows backend, between its own files: the backend's
 * and the surfaces' state and the functions one file gives another.
 */

#ifndef KEILAND_X11_WIN32_INTERNAL_H
#define KEILAND_X11_WIN32_INTERNAL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "userland/desktop/xserver/win32/backend.h"
#include "userland/desktop/xserver/win32/keysyms.h"
#include "userland/desktop/xserver/win32/scancode.h"

#include <stddef.h>
#include <stdint.h>

/* The X buttons the backend sends: 1 to 3 the left, middle and right, 4 to 7 the wheels, 8 and 9 the side buttons. */
#define WIN32_BUTTON_LEFT		1U
#define WIN32_BUTTON_MIDDLE		2U
#define WIN32_BUTTON_RIGHT		3U
#define WIN32_BUTTON_WHEEL_UP		4U
#define WIN32_BUTTON_WHEEL_DOWN		5U
#define WIN32_BUTTON_WHEEL_LEFT		6U
#define WIN32_BUTTON_WHEEL_RIGHT	7U
#define WIN32_BUTTON_BACK		8U
#define WIN32_BUTTON_FORWARD		9U

/* The timer of the loops Windows runs while a window is moved or sized, and its period (ms). */
#define WIN32_SERVICE_TIMER	1U
#define WIN32_SERVICE_PERIOD	10U

/* The evdev codes of the keys whose state the backend keeps. */
#define WIN32_KEY_LEFTCTRL	29U
#define WIN32_KEY_LEFTSHIFT	42U
#define WIN32_KEY_RIGHTSHIFT	54U
#define WIN32_KEY_LEFTALT	56U
#define WIN32_KEY_CAPSLOCK	58U
#define WIN32_KEY_F4		62U
#define WIN32_KEY_RIGHTCTRL	97U
#define WIN32_KEY_RIGHTALT	100U
#define WIN32_KEY_INSERT	110U
#define WIN32_KEY_C		46U
#define WIN32_KEY_V		47U

/*
 * One Windows window of an X top-level window.
 *
 * The core holds it from open to close.  x, y, width and height are the X
 * window's place on the root window and size as the core last gave them
 * (X's pixels): the pointer's place on the root window is counted from
 * them, so the core and the backend agree on it even where Windows put
 * the window elsewhere, and present's pixels have that size.
 * reported_x, reported_y, reported_width and reported_height are the place
 * and size the core last gave or was told (the user moved or sized the
 * window), so each is told once; report_posted says a report waits in the
 * message queue.  The DIB holds the X pixels scaled to the screen
 * while the window is shown through GDI; vulkan says the core shows it
 * through a Vulkan surface instead.  hints are the limits on the X
 * window's size, which the window's frame keeps while the user sizes it.
 * state is whether the window is maximized and full-screen
 * (X11_SURFACE_MAXIMIZED, FULLSCREEN) as last shown or told the core;
 * a full-screen window keeps its style and place from before to go back
 * to.  Surfaces are linked from the backend's list.
 */
struct x11_surface {
	struct x11_win32 *backend;
	struct x11_surface *next;
	uint32_t window;
	HWND hwnd;
	unsigned kind;
	int32_t x;
	int32_t y;
	unsigned width;
	unsigned height;
	int32_t reported_x;
	int32_t reported_y;
	unsigned reported_width;
	unsigned reported_height;
	int report_posted;
	unsigned flags;
	struct x11_surface *owner;
	int iconic;
	int vulkan;
	struct x11_size_hints hints;
	unsigned state;
	LONG_PTR windowed_style;
	RECT windowed_rect;

	/* The window's pixels for GDI: a top-down 32-bit DIB of the scaled size. */
	HDC dib_dc;
	HBITMAP dib;
	HGDIOBJ dib_old;
	uint32_t *dib_pixels;
	unsigned dib_width;
	unsigned dib_height;
};

/*
 * The backend: its options, the windows it opened, and the state of the
 * keyboard, the pointer and the clipboard between messages.
 */
struct x11_win32 {
	struct x11_win32_options options;
	char *host;
	struct x11server *server;
	HINSTANCE instance;
	ATOM window_class;
	ATOM message_class;
	HWND message_window;
	unsigned scale;
	RECT virtual_screen;
	struct x11_surface *surfaces;
	unsigned surface_count;

	/*
	 * The keyboard: the layout's map; by evdev code, the keys held (the
	 * core hears each by its keycode, the code plus 8); the Shift, Ctrl
	 * and Alt bits held, which the backend keeps for Alt+F4 and for
	 * noting a paste or a copy (the core makes X's state itself); and
	 * whether a left Ctrl press was dropped as the one Windows makes up
	 * before AltGr.
	 */
	struct x11_win32_keymap keymap;
	uint8_t held[X11_WIN32_EVDEV_MAX + 1];
	uint16_t modifiers;
	int altgr_control;

	/* The text typed through WM_CHAR: a high surrogate waiting for its low one (0: none). */
	uint16_t text_high;

	/*
	 * The pointer: the surface under it, its last place on the root window,
	 * the buttons 1 to 3 held (X's bits), every button held (for the
	 * capture), whether an X client's grab keeps the capture (a button's
	 * release then leaves it), and the wheels' turns not yet a whole notch.
	 */
	struct x11_surface *pointer_surface;
	int pointer_x;
	int pointer_y;
	uint16_t buttons;
	unsigned buttons_held;
	int grab_capture;
	int wheel;
	int hwheel;

	/* The clipboard: when the user last pasted and copied, and the sequence the backend set. */
	DWORD paste_time;
	DWORD copy_time;
	int pasted;
	int copied;
	DWORD clipboard_sequence;

	/* Vulkan: the loader, opened when the core first asks for a surface. */
	HMODULE vulkan_loader;
};

/* backend.c */
void win32_log(struct x11_win32 *backend, const char *format, ...);

/* window.c */
int win32_window_register(struct x11_win32 *backend);
void win32_window_unregister(struct x11_win32 *backend);
struct x11_surface *win32_surface_open(void *context, uint32_t window, const struct x11_surface_info *info);
void win32_surface_update(void *context, struct x11_surface *surface, const struct x11_surface_info *info, unsigned changed);
void win32_surface_close(void *context, struct x11_surface *surface);
int win32_surface_present(void *context, struct x11_surface *surface, const uint32_t *pixels, size_t stride, const struct x11_rect *parts, unsigned count);

/* vulkan.c */
int win32_surface_vulkan(void *context, struct x11_surface *surface, void *instance, void *result);

/* input.c */
void win32_input_key(struct x11_surface *surface, UINT message, WPARAM wparam, LPARAM lparam);
void win32_input_release_keys(struct x11_win32 *backend);
void win32_input_pointer(struct x11_surface *surface, UINT message, WPARAM wparam, LPARAM lparam);
void win32_input_leave(struct x11_surface *surface);
void win32_input_capture(void *context, struct x11_surface *surface, int on);
void win32_input_capture_changed(struct x11_surface *surface, HWND taker);
int win32_input_place(void *context, int *x, int *y);
int win32_input_warp(void *context, int x, int y);
void win32_input_bell(void *context, int percent);

/* layout.c */
void win32_layout_read(struct x11_win32 *backend);
void win32_layout_give(struct x11_win32 *backend);

/* ime.c */
int win32_ime_translates(const MSG *message);
void win32_ime_place(struct x11_surface *surface);
LRESULT win32_ime_composition(struct x11_surface *surface, WPARAM wparam, LPARAM lparam);
void win32_ime_char(struct x11_surface *surface, WPARAM wparam);

/* clipboard.c */
int win32_clipboard_start(struct x11_win32 *backend);
void win32_clipboard_stop(struct x11_win32 *backend);
void win32_clipboard_attach(struct x11_win32 *backend);
void win32_clipboard_changed(struct x11_win32 *backend);
int win32_clipboard_offer(void *context, unsigned selection, const uint8_t *text, size_t size);
int win32_clipboard_read(void *context, unsigned selection, uint8_t **text, size_t *size);

#endif
