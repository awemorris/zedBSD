/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop surface, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_DESKTOP_H
#define KEILAND_DESKTOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_surface;

/*
 * The desktop surface (KL_VERSION 14).
 *
 * The compositor starts the program that shows the icons of ~/Desktop with a
 * token in its environment (KEILAND_DESKTOP_TOKEN); with the token, the
 * program's surface lies over the wallpaper and under every window, on
 * every virtual desktop, has no window of its own and hears the pointer,
 * the keyboard, the touch screen and drag and drop where no window is.
 * The program copies the token and takes it out of its environment before
 * it starts anything, so that no program it starts can take the role.
 */
struct kl_desktop;

/*
 * What the desktop surface hears: configure, the place on the output
 * (x, y) and the size the surface is to have; the program acknowledges it
 * (kl_desktop_ack) and draws at that size.
 */
struct kl_desktop_listener {
	void (*configure)(void *data, struct kl_desktop *desktop, uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height);
};

/*
 * Gives a surface the desktop's role with the token.  Returns NULL with
 * errno set: EINVAL without a token, ENOTSUP for a compositor without the
 * desktop, ENOMEM.  A token the compositor does not know ends the
 * connection.
 */
struct kl_desktop *kl_desktop_create(struct wl_display *display, struct wl_surface *surface, const char *token, const struct kl_desktop_listener *listener, void *data);

/*
 * Acknowledges a configure: the next commit is drawn for it.
 */
void kl_desktop_ack(struct kl_desktop *desktop, uint32_t serial);

/*
 * Gives the desktop's role up.
 */
void kl_desktop_destroy(struct kl_desktop *desktop);

#ifdef __cplusplus
}
#endif

#endif
