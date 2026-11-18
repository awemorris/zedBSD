/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyboard inset, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_KEYBOARD_INSET_H
#define KEILAND_KEYBOARD_INSET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;

/*
 * The keyboard inset (KL_VERSION 18).
 *
 * A window hears how much of it the on-screen keyboard covers, in the
 * window's pixels from its right edge (the flick panel's column) and from
 * its bottom edge (the QWERTY row), when the keyboard opens, closes or
 * changes the window's size or place (before that configure).  The window
 * may then keep what matters -- the caret -- where it can be seen.  Both
 * are 0 when the keyboard has closed or does not cover the window.
 */
struct kl_keyboard_inset;
struct xdg_toplevel;

/* Why the inset changed: no keyboard, the right column's, the bottom row's. */
#define KL_KEYBOARD_INSET_NONE	0U
#define KL_KEYBOARD_INSET_RIGHT	1U
#define KL_KEYBOARD_INSET_BOTTOM	2U

/* The application's callback: the covered widths from the right and bottom edges, and the reason. */
typedef void (*kl_keyboard_inset_fn)(void *data, int32_t right, int32_t bottom, uint32_t reason);

/*
 * Asks for a window's keyboard inset; callback runs on the application's
 * default queue.  Returns NULL with errno set: ENOTSUP for a compositor
 * without it (nothing more to do), EINVAL, ENOMEM.
 */
struct kl_keyboard_inset *kl_keyboard_inset_create(struct wl_display *display, struct xdg_toplevel *toplevel, kl_keyboard_inset_fn callback, void *data);

/*
 * Stops hearing the keyboard.
 */
void kl_keyboard_inset_destroy(struct kl_keyboard_inset *inset);

#ifdef __cplusplus
}
#endif

#endif
