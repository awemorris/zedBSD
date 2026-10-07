/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares zdesktop's appearance protocol (kl_theme_v1, version 2;
 * ws089-p017, ws179-p001): the desktop's appearance, light or dark, and
 * (version 2) the accent the user chose, told when the global is bound and
 * whenever they change.
 *
 * The header is private: it is not installed, and applications reach the
 * protocol through libkeiland (<keiland/keiland.h>) only.  libkeiland includes it
 * by its path in the tree.
 */

#ifndef KERN_KEILAND_THEME_V1_CLIENT_PROTOCOL_H
#define KERN_KEILAND_THEME_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The object the protocol names; only libwayland knows what is in it. */
struct kl_theme_v1;

/* The interface's description (theme-protocol.c). */
extern const struct wl_interface kl_theme_v1_interface;

/*
 * kl_theme_v1: the global.  appearance is 0 for light and 1 for dark
 * (a value a client does not know is taken as light); accent (version 2)
 * is the index of the accent, 0 blue to 7 graphite (one a client does
 * not know is taken as blue), told after the appearance.
 */
#define KL_THEME_V1_APPEARANCE_LIGHT 0U
#define KL_THEME_V1_APPEARANCE_DARK 1U
struct kl_theme_v1_listener {
	void (*appearance)(void *data, struct kl_theme_v1 *object, uint32_t appearance);
	void (*accent)(void *data, struct kl_theme_v1 *object, uint32_t accent);
};
int kl_theme_v1_add_listener(struct kl_theme_v1 *object, const struct kl_theme_v1_listener *listener, void *data);
#define KL_THEME_V1_DESTROY 0U
void kl_theme_v1_destroy(struct kl_theme_v1 *object);

#ifdef __cplusplus
}
#endif

#endif
