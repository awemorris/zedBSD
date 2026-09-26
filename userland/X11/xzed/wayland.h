/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Xzed's Wayland backend (WS069): the X screen as one window of a Wayland
 * compositor, and the compositor's pointer and keyboard as Xzed's input.
 */

#ifndef XZED_WAYLAND_H
#define XZED_WAYLAND_H

#include "input.h"

#include <stdint.h>

struct xzed_wayland;

int xzed_wayland_open(struct xzed_wayland **out, const char *display, unsigned width, unsigned height, const struct xzed_input_handlers *handlers, void *context);
int xzed_wayland_fd(const struct xzed_wayland *wayland);
int xzed_wayland_dispatch(struct xzed_wayland *wayland, int readable);
int xzed_wayland_present(struct xzed_wayland *wayland, const uint32_t *screen, int x, int y, int width, int height);
int xzed_wayland_closed(const struct xzed_wayland *wayland);
void xzed_wayland_close(struct xzed_wayland *wayland);

#endif
