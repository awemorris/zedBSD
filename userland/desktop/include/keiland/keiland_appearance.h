/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop's appearance, light or dark, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_APPEARANCE_H
#define KEILAND_APPEARANCE_H

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;

/*
 * The desktop's appearance (KL_VERSION 35): light or dark, as the user chose
 * in Settings.  The compositor tells it when the program asks and again
 * whenever it changes (kl_theme_v1); a program draws in it and draws again
 * when it changes.  kl_theme_default gives the theme of the appearance the
 * program was told last (the same pointer, its contents changed), and an
 * application of kl_app_open is told with a KL_APP_THEME event.  A compositor
 * without the appearance is light.
 */
#define KL_APPEARANCE_LIGHT	0U
#define KL_APPEARANCE_DARK	1U

/* The appearance watched on one connection. */
struct kl_appearance;

/* Called with the appearance (KL_APPEARANCE_*) when it or the accent changes (KL_VERSION 57), within the dispatch of the display's default queue. */
typedef void (*kl_appearance_fn)(void *data, unsigned appearance);

/*
 * Watches the appearance on a display: learns it now (with a roundtrip
 * of the library's own queue, which runs none of the program's events)
 * and calls changed (NULL for none) whenever it changes.  Returns 0 with
 * *appearance set, ENOTSUP when the compositor has no appearance (the
 * program is light), ENOMEM or EPROTO.
 */
int kl_appearance_open(struct wl_display *display, kl_appearance_fn changed, void *data, struct kl_appearance **appearance);

/*
 * The appearance (KL_APPEARANCE_*) a watch was told last; for NULL, the
 * one the program was told last on any watch (light before any).
 */
unsigned kl_appearance_get(const struct kl_appearance *appearance);

/*
 * Stops watching (NULL does nothing).
 */
void kl_appearance_close(struct kl_appearance *appearance);

#ifdef __cplusplus
}
#endif

#endif
