/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares zdesktop's glass protocol (kl_glass_v1, ws035-p083).
 *
 * The header is private: it is not installed, and applications reach the
 * protocol through libkeiland (<keiland/keiland.h>) only.  libkeiland includes
 * it by its path in the tree.  The protocol is defined in
 * plan/ws035/glass-design.md.
 */

#ifndef KERN_KEILAND_GLASS_V1_CLIENT_PROTOCOL_H
#define KERN_KEILAND_GLASS_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_array;
struct wl_surface;
struct kl_glass_manager_v1;
struct kl_glass_v1;

/* The two interfaces' descriptions (glass-protocol.c). */
extern const struct wl_interface kl_glass_manager_v1_interface;
extern const struct wl_interface kl_glass_v1_interface;

/* kl_glass_manager_v1: the global that gives surfaces their glass. */
#define KL_GLASS_MANAGER_V1_ERROR_ALREADY_EXISTS 0U
#define KL_GLASS_MANAGER_V1_DESTROY 0U
#define KL_GLASS_MANAGER_V1_GET_GLASS 1U
void kl_glass_manager_v1_destroy(struct kl_glass_manager_v1 *object);
struct kl_glass_v1 *kl_glass_manager_v1_get_glass(struct kl_glass_manager_v1 *object, struct wl_surface *surface);

/* kl_glass_v1: one surface's panels on the system's frosted glass. */
#define KL_GLASS_V1_ERROR_BAD_PANELS 0U
#define KL_GLASS_V1_ERROR_NO_SURFACE 1U
#define KL_GLASS_V1_KIND_CARD 0U
#define KL_GLASS_V1_PANELS_MAX 32U
#define KL_GLASS_V1_RADIUS_MAX 64
#define KL_GLASS_V1_DESTROY 0U
#define KL_GLASS_V1_SET_PANELS 1U
#define KL_GLASS_V1_SET_BLUR 2U
#define KL_GLASS_V1_SET_BLUR_SINCE_VERSION 2U
void kl_glass_v1_destroy(struct kl_glass_v1 *object);
void kl_glass_v1_set_panels(struct kl_glass_v1 *object, struct wl_array *panels);
void kl_glass_v1_set_blur(struct kl_glass_v1 *object, uint32_t enabled);

#ifdef __cplusplus
}
#endif

#endif
