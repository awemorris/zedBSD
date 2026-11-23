/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the fractional scale protocol's objects, listener and typed
 * requests (wp_fractional_scale_manager_v1 and wp_fractional_scale_v1,
 * version 1).  The compositor tells a surface the scale it would like the
 * surface drawn at, in 120ths (180 is 150 %); the client then draws a buffer
 * that many times the surface's size and sets the surface's size with
 * wp_viewport's destination, leaving the buffer scale at 1.  The names follow
 * the upstream client header.
 *
 * The header is private: it is not installed, and applications reach the
 * protocol through libkeiland (<keiland/keiland.h>) only.  libkeiland includes
 * it by its path in the tree.
 */

#ifndef KERN_FRACTIONAL_SCALE_V1_CLIENT_PROTOCOL_H
#define KERN_FRACTIONAL_SCALE_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_surface;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;

/* The two interfaces' descriptions (fractional-scale-protocol.c). */
extern const struct wl_interface wp_fractional_scale_manager_v1_interface;
extern const struct wl_interface wp_fractional_scale_v1_interface;

/*
 * Values referenced: wayland-protocols 1.44
 * staging/fractional-scale/fractional-scale-v1.xml (the error enum of
 * wp_fractional_scale_manager_v1, and the order of both interfaces'
 * requests and events).
 */

/*
 * wp_fractional_scale_manager_v1: the global that gives a surface its
 * fractional scale object (one a surface; a second is the error below).
 */
#define WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS 0U
#define WP_FRACTIONAL_SCALE_MANAGER_V1_DESTROY 0U
#define WP_FRACTIONAL_SCALE_MANAGER_V1_GET_FRACTIONAL_SCALE 1U
void wp_fractional_scale_manager_v1_destroy(struct wp_fractional_scale_manager_v1 *wp_fractional_scale_manager_v1);
struct wp_fractional_scale_v1 *wp_fractional_scale_manager_v1_get_fractional_scale(struct wp_fractional_scale_manager_v1 *wp_fractional_scale_manager_v1, struct wl_surface *surface);

/*
 * wp_fractional_scale_v1: a surface's preferred scale.  preferred_scale
 * brings a new scale (in 120ths) whenever the compositor's choice changes;
 * after destroy no more come.
 */
struct wp_fractional_scale_v1_listener {
	void (*preferred_scale)(void *data, struct wp_fractional_scale_v1 *wp_fractional_scale_v1, uint32_t scale);
};
int wp_fractional_scale_v1_add_listener(struct wp_fractional_scale_v1 *wp_fractional_scale_v1, const struct wp_fractional_scale_v1_listener *listener, void *data);
#define WP_FRACTIONAL_SCALE_V1_PREFERRED_SCALE 0U
#define WP_FRACTIONAL_SCALE_V1_DESTROY 0U
void wp_fractional_scale_v1_destroy(struct wp_fractional_scale_v1 *wp_fractional_scale_v1);

#ifdef __cplusplus
}
#endif

#endif
