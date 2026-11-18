/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of libkeiland's desktop surface (desktop.c) that its window
 * (ui/window.c) uses: the parts of the surface a frame draws anything in,
 * told to the compositor so that it does not draw the rest over the
 * wallpaper (ws270-p007, kl_desktop_surface_v1 version 2).
 */

#ifndef KEILAND_DESKTOP_PRIVATE_H
#define KEILAND_DESKTOP_PRIVATE_H

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

void keiui_desktop_frame(struct kl_desktop *desktop, const uint32_t *pixels, size_t stride, uint32_t width, uint32_t height, const struct kl_rect *parts, unsigned count);

#endif
