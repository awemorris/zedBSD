/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the viewporter protocol's objects and typed requests
 * (wp_viewporter and wp_viewport, version 1).  A surface's viewport crops
 * its buffer to a source rectangle (in buffer coordinates after the buffer
 * scale, as wl_fixed_t) and gives the surface a destination size (in
 * surface coordinates) independent of the buffer's.  A client drawing at a
 * fractional scale sets the destination to the surface's size and attaches
 * a buffer that many times larger.  The names follow the upstream client
 * header.
 *
 * The header is private: it is not installed, and applications reach the
 * protocol through libkeiland (<keiland/keiland.h>) only.  libkeiland includes
 * it by its path in the tree.
 */

#ifndef KERN_VIEWPORTER_CLIENT_PROTOCOL_H
#define KERN_VIEWPORTER_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_surface;
struct wp_viewporter;
struct wp_viewport;

/* The two interfaces' descriptions (viewporter-protocol.c). */
extern const struct wl_interface wp_viewporter_interface;
extern const struct wl_interface wp_viewport_interface;

/*
 * Values referenced: wayland-protocols 1.44 stable/viewporter/viewporter.xml
 * (the error enums of wp_viewporter and wp_viewport, and the order of both
 * interfaces' requests).
 */

/*
 * wp_viewporter: the global that gives a surface its viewport (one a
 * surface; a second is the error below).
 */
#define WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS 0U
#define WP_VIEWPORTER_DESTROY 0U
#define WP_VIEWPORTER_GET_VIEWPORT 1U
void wp_viewporter_destroy(struct wp_viewporter *wp_viewporter);
struct wp_viewport *wp_viewporter_get_viewport(struct wp_viewporter *wp_viewporter, struct wl_surface *surface);

/*
 * wp_viewport: a surface's crop and scale, applied with the surface's next
 * commit.  A source of all -1 and a destination of -1 by -1 each unset
 * their part.
 */
#define WP_VIEWPORT_ERROR_BAD_VALUE 0U
#define WP_VIEWPORT_ERROR_BAD_SIZE 1U
#define WP_VIEWPORT_ERROR_OUT_OF_BUFFER 2U
#define WP_VIEWPORT_ERROR_NO_SURFACE 3U
#define WP_VIEWPORT_DESTROY 0U
#define WP_VIEWPORT_SET_SOURCE 1U
#define WP_VIEWPORT_SET_DESTINATION 2U
void wp_viewport_destroy(struct wp_viewport *wp_viewport);
void wp_viewport_set_source(struct wp_viewport *wp_viewport, wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height);
void wp_viewport_set_destination(struct wp_viewport *wp_viewport, int32_t width, int32_t height);

#ifdef __cplusplus
}
#endif

#endif
