/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Xzed's Wayland backend (WS069): X windows as windows of a Wayland
 * compositor, and the compositor's pointer and keyboard as Xzed's input.
 * Rootful, one window shows the whole X screen; rootless, each top-level X
 * window has a window of its own.
 */

#ifndef XZED_WAYLAND_H
#define XZED_WAYLAND_H

#include "input.h"

#include <stdint.h>

struct xzed_wayland;
struct xzed_wayland_window;

/*
 * What the backend tells Xzed: keys and pointer frames (in the X screen's
 * coordinates), the window the pointer or the keyboard came into, the size
 * the compositor gives a window, and a window the compositor closes.  The
 * windows are named by the X window id Xzed gave them.
 */
struct xzed_wayland_callbacks {
	void (*key)(void *context, uint8_t keycode, int pressed, uint32_t time, uint16_t state);
	void (*pointer)(void *context, const struct xzed_input_pointer_frame *frame);
	void (*enter)(void *context, uint32_t window, int keyboard);
	void (*configure)(void *context, uint32_t window, int width, int height);
	void (*close)(void *context, uint32_t window);
};

int xzed_wayland_open(struct xzed_wayland **out, const char *display, const struct xzed_wayland_callbacks *callbacks, void *context);
int xzed_wayland_fd(const struct xzed_wayland *wayland);
int xzed_wayland_dispatch(struct xzed_wayland *wayland, int readable);
void xzed_wayland_close(struct xzed_wayland *wayland);

struct xzed_wayland_window *xzed_wayland_window_open(struct xzed_wayland *wayland, uint32_t id, const char *title, unsigned width, unsigned height);
int xzed_wayland_window_present(struct xzed_wayland_window *window, const uint32_t *pixels, int x, int y, int width, int height);
int xzed_wayland_window_resize(struct xzed_wayland_window *window, unsigned width, unsigned height);
void xzed_wayland_window_move(struct xzed_wayland_window *window, int x, int y);
void xzed_wayland_window_title(struct xzed_wayland_window *window, const char *title);
void xzed_wayland_window_close(struct xzed_wayland_window *window);

#endif
