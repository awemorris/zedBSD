/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the content type protocol's objects and typed requests
 * (wp_content_type_manager_v1 and wp_content_type_v1, version 1;
 * ws122-p005b).  A client tells the compositor what a surface shows
 * (nothing in particular, a photo, a video, a game); a fullscreen video or
 * game may then be scanned out without composing.  The names follow the
 * upstream client header; the wire descriptions were checked against the
 * pinned description (keiland/wayland/API-PROVENANCE.md).
 *
 * The header is private: it is not installed, and applications reach the
 * protocol through libkeiland (<keiland/keiland.h>) only.  libkeiland includes it
 * by its path in the tree.
 */

#ifndef KERN_CONTENT_TYPE_V1_CLIENT_PROTOCOL_H
#define KERN_CONTENT_TYPE_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_surface;
struct wp_content_type_manager_v1;
struct wp_content_type_v1;

/* The two interfaces' descriptions (content-type-protocol.c). */
extern const struct wl_interface wp_content_type_manager_v1_interface;
extern const struct wl_interface wp_content_type_v1_interface;

/* wp_content_type_manager_v1: the global that gives a surface its content type object (one a surface). */
#define WP_CONTENT_TYPE_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED 0U
#define WP_CONTENT_TYPE_MANAGER_V1_DESTROY 0U
#define WP_CONTENT_TYPE_MANAGER_V1_GET_SURFACE_CONTENT_TYPE 1U
void wp_content_type_manager_v1_destroy(struct wp_content_type_manager_v1 *wp_content_type_manager_v1);
struct wp_content_type_v1 *wp_content_type_manager_v1_get_surface_content_type(struct wp_content_type_manager_v1 *wp_content_type_manager_v1, struct wl_surface *surface);

/* wp_content_type_v1: a surface's content type, applied with the surface's next commit. */
#define WP_CONTENT_TYPE_V1_TYPE_NONE 0U
#define WP_CONTENT_TYPE_V1_TYPE_PHOTO 1U
#define WP_CONTENT_TYPE_V1_TYPE_VIDEO 2U
#define WP_CONTENT_TYPE_V1_TYPE_GAME 3U
#define WP_CONTENT_TYPE_V1_DESTROY 0U
#define WP_CONTENT_TYPE_V1_SET_CONTENT_TYPE 1U
void wp_content_type_v1_destroy(struct wp_content_type_v1 *wp_content_type_v1);
void wp_content_type_v1_set_content_type(struct wp_content_type_v1 *wp_content_type_v1, uint32_t content_type);

#ifdef __cplusplus
}
#endif

#endif
