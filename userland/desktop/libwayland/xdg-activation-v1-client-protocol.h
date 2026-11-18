/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the activation protocol's objects, listener and typed requests
 * (xdg_activation_v1, version 1; ws089-p016).  The names follow the
 * upstream client header; the wire descriptions were checked against the
 * pinned description (keiland/wayland/API-PROVENANCE.md).
 *
 * The header is private: it is not installed, and applications reach the
 * protocol through libkeiland (<keiland/keiland.h>) only.  libkeiland includes it
 * by its path in the tree.
 */

#ifndef KERN_XDG_ACTIVATION_V1_CLIENT_PROTOCOL_H
#define KERN_XDG_ACTIVATION_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_seat;
struct wl_surface;
struct xdg_activation_v1;
struct xdg_activation_token_v1;

/* The two interfaces' descriptions (activation-protocol.c). */
extern const struct wl_interface xdg_activation_v1_interface;
extern const struct wl_interface xdg_activation_token_v1_interface;

/* xdg_activation_v1: the global that gives tokens and activates a surface with one. */
#define XDG_ACTIVATION_V1_DESTROY 0U
#define XDG_ACTIVATION_V1_GET_ACTIVATION_TOKEN 1U
#define XDG_ACTIVATION_V1_ACTIVATE 2U
void xdg_activation_v1_destroy(struct xdg_activation_v1 *xdg_activation_v1);
struct xdg_activation_token_v1 *xdg_activation_v1_get_activation_token(struct xdg_activation_v1 *xdg_activation_v1);
void xdg_activation_v1_activate(struct xdg_activation_v1 *xdg_activation_v1, const char *token, struct wl_surface *surface);

/*
 * xdg_activation_token_v1: a token being asked for.  The requests describe
 * it, commit asks for it, and done brings its text.
 */
#define XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED 0U
struct xdg_activation_token_v1_listener {
	void (*done)(void *data, struct xdg_activation_token_v1 *xdg_activation_token_v1, const char *token);
};
int xdg_activation_token_v1_add_listener(struct xdg_activation_token_v1 *xdg_activation_token_v1, const struct xdg_activation_token_v1_listener *listener, void *data);
#define XDG_ACTIVATION_TOKEN_V1_SET_SERIAL 0U
#define XDG_ACTIVATION_TOKEN_V1_SET_APP_ID 1U
#define XDG_ACTIVATION_TOKEN_V1_SET_SURFACE 2U
#define XDG_ACTIVATION_TOKEN_V1_COMMIT 3U
#define XDG_ACTIVATION_TOKEN_V1_DESTROY 4U
void xdg_activation_token_v1_set_serial(struct xdg_activation_token_v1 *xdg_activation_token_v1, uint32_t serial, struct wl_seat *seat);
void xdg_activation_token_v1_set_app_id(struct xdg_activation_token_v1 *xdg_activation_token_v1, const char *app_id);
void xdg_activation_token_v1_set_surface(struct xdg_activation_token_v1 *xdg_activation_token_v1, struct wl_surface *surface);
void xdg_activation_token_v1_commit(struct xdg_activation_token_v1 *xdg_activation_token_v1);
void xdg_activation_token_v1_destroy(struct xdg_activation_token_v1 *xdg_activation_token_v1);

#ifdef __cplusplus
}
#endif

#endif
