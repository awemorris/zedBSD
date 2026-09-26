/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Declares the selected xdg protocol objects and typed requests. */

#ifndef ZEDBSD_XDG_SHELL_CLIENT_PROTOCOL_H
#define ZEDBSD_XDG_SHELL_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_registry;
struct wl_callback;
struct wl_compositor;
struct wl_surface;
struct wl_region;
struct wl_buffer;
struct wl_output;
struct wl_seat;
struct xdg_wm_base;
struct xdg_positioner;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_popup;

struct xdg_wm_base;
extern const struct wl_interface xdg_wm_base_interface;

/* Receives events for one xdg_wm_base object; retained by its proxy. */
struct xdg_wm_base_listener {
	void (*ping)(void *data, struct xdg_wm_base *object, uint32_t serial);
};

int xdg_wm_base_add_listener(struct xdg_wm_base *object, const struct xdg_wm_base_listener *listener, void *data);
#define XDG_WM_BASE_DESTROY 0U
void xdg_wm_base_destroy(struct xdg_wm_base *object);
#define XDG_WM_BASE_CREATE_POSITIONER 1U
struct xdg_positioner *xdg_wm_base_create_positioner(struct xdg_wm_base *object);
#define XDG_WM_BASE_GET_XDG_SURFACE 2U
struct xdg_surface *xdg_wm_base_get_xdg_surface(struct xdg_wm_base *object, struct wl_surface *surface);
#define XDG_WM_BASE_PONG 3U
void xdg_wm_base_pong(struct xdg_wm_base *object, uint32_t serial);
void xdg_wm_base_set_user_data(struct xdg_wm_base *object, void *data);
void *xdg_wm_base_get_user_data(struct xdg_wm_base *object);
uint32_t xdg_wm_base_get_version(struct xdg_wm_base *object);

struct xdg_positioner;
extern const struct wl_interface xdg_positioner_interface;
#define XDG_POSITIONER_DESTROY 0U
void xdg_positioner_destroy(struct xdg_positioner *object);
#define XDG_POSITIONER_SET_SIZE 1U
void xdg_positioner_set_size(struct xdg_positioner *object, int32_t width, int32_t height);
#define XDG_POSITIONER_SET_ANCHOR_RECT 2U
void xdg_positioner_set_anchor_rect(struct xdg_positioner *object, int32_t x, int32_t y, int32_t width, int32_t height);
#define XDG_POSITIONER_SET_ANCHOR 3U
void xdg_positioner_set_anchor(struct xdg_positioner *object, uint32_t anchor);
#define XDG_POSITIONER_SET_GRAVITY 4U
void xdg_positioner_set_gravity(struct xdg_positioner *object, uint32_t gravity);
#define XDG_POSITIONER_SET_CONSTRAINT_ADJUSTMENT 5U
void xdg_positioner_set_constraint_adjustment(struct xdg_positioner *object, uint32_t constraint_adjustment);
#define XDG_POSITIONER_SET_OFFSET 6U
void xdg_positioner_set_offset(struct xdg_positioner *object, int32_t x, int32_t y);
void xdg_positioner_set_user_data(struct xdg_positioner *object, void *data);
void *xdg_positioner_get_user_data(struct xdg_positioner *object);
uint32_t xdg_positioner_get_version(struct xdg_positioner *object);

struct xdg_surface;
extern const struct wl_interface xdg_surface_interface;

/* Receives events for one xdg_surface object; retained by its proxy. */
struct xdg_surface_listener {
	void (*configure)(void *data, struct xdg_surface *object, uint32_t serial);
};

int xdg_surface_add_listener(struct xdg_surface *object, const struct xdg_surface_listener *listener, void *data);
#define XDG_SURFACE_DESTROY 0U
void xdg_surface_destroy(struct xdg_surface *object);
#define XDG_SURFACE_GET_TOPLEVEL 1U
struct xdg_toplevel *xdg_surface_get_toplevel(struct xdg_surface *object);
#define XDG_SURFACE_GET_POPUP 2U
struct xdg_popup *xdg_surface_get_popup(struct xdg_surface *object, struct xdg_surface *parent, struct xdg_positioner *positioner);
#define XDG_SURFACE_SET_WINDOW_GEOMETRY 3U
void xdg_surface_set_window_geometry(struct xdg_surface *object, int32_t x, int32_t y, int32_t width, int32_t height);
#define XDG_SURFACE_ACK_CONFIGURE 4U
void xdg_surface_ack_configure(struct xdg_surface *object, uint32_t serial);
void xdg_surface_set_user_data(struct xdg_surface *object, void *data);
void *xdg_surface_get_user_data(struct xdg_surface *object);
uint32_t xdg_surface_get_version(struct xdg_surface *object);

struct xdg_toplevel;
extern const struct wl_interface xdg_toplevel_interface;

/* Receives events for one xdg_toplevel object; retained by its proxy. */
struct xdg_toplevel_listener {
	void (*configure)(void *data, struct xdg_toplevel *object, int32_t width, int32_t height, struct wl_array *states);
	void (*close)(void *data, struct xdg_toplevel *object);
};

int xdg_toplevel_add_listener(struct xdg_toplevel *object, const struct xdg_toplevel_listener *listener, void *data);
#define XDG_TOPLEVEL_DESTROY 0U
void xdg_toplevel_destroy(struct xdg_toplevel *object);
#define XDG_TOPLEVEL_SET_PARENT 1U
void xdg_toplevel_set_parent(struct xdg_toplevel *object, struct xdg_toplevel *parent);
#define XDG_TOPLEVEL_SET_TITLE 2U
void xdg_toplevel_set_title(struct xdg_toplevel *object, const char *title);
#define XDG_TOPLEVEL_SET_APP_ID 3U
void xdg_toplevel_set_app_id(struct xdg_toplevel *object, const char *app_id);
#define XDG_TOPLEVEL_SHOW_WINDOW_MENU 4U
void xdg_toplevel_show_window_menu(struct xdg_toplevel *object, struct wl_seat *seat, uint32_t serial, int32_t x, int32_t y);
#define XDG_TOPLEVEL_MOVE 5U
void xdg_toplevel_move(struct xdg_toplevel *object, struct wl_seat *seat, uint32_t serial);
#define XDG_TOPLEVEL_RESIZE 6U
void xdg_toplevel_resize(struct xdg_toplevel *object, struct wl_seat *seat, uint32_t serial, uint32_t edges);
#define XDG_TOPLEVEL_SET_MAX_SIZE 7U
void xdg_toplevel_set_max_size(struct xdg_toplevel *object, int32_t width, int32_t height);
#define XDG_TOPLEVEL_SET_MIN_SIZE 8U
void xdg_toplevel_set_min_size(struct xdg_toplevel *object, int32_t width, int32_t height);
#define XDG_TOPLEVEL_SET_MAXIMIZED 9U
void xdg_toplevel_set_maximized(struct xdg_toplevel *object);
#define XDG_TOPLEVEL_UNSET_MAXIMIZED 10U
void xdg_toplevel_unset_maximized(struct xdg_toplevel *object);
#define XDG_TOPLEVEL_SET_FULLSCREEN 11U
void xdg_toplevel_set_fullscreen(struct xdg_toplevel *object, struct wl_output *output);
#define XDG_TOPLEVEL_UNSET_FULLSCREEN 12U
void xdg_toplevel_unset_fullscreen(struct xdg_toplevel *object);
#define XDG_TOPLEVEL_SET_MINIMIZED 13U
void xdg_toplevel_set_minimized(struct xdg_toplevel *object);
void xdg_toplevel_set_user_data(struct xdg_toplevel *object, void *data);
void *xdg_toplevel_get_user_data(struct xdg_toplevel *object);
uint32_t xdg_toplevel_get_version(struct xdg_toplevel *object);

struct xdg_popup;
extern const struct wl_interface xdg_popup_interface;

/* Receives events for one xdg_popup object; retained by its proxy. */
struct xdg_popup_listener {
	void (*configure)(void *data, struct xdg_popup *object, int32_t x, int32_t y, int32_t width, int32_t height);
	void (*popup_done)(void *data, struct xdg_popup *object);
};

int xdg_popup_add_listener(struct xdg_popup *object, const struct xdg_popup_listener *listener, void *data);
#define XDG_POPUP_DESTROY 0U
void xdg_popup_destroy(struct xdg_popup *object);
#define XDG_POPUP_GRAB 1U
void xdg_popup_grab(struct xdg_popup *object, struct wl_seat *seat, uint32_t serial);
void xdg_popup_set_user_data(struct xdg_popup *object, void *data);
void *xdg_popup_get_user_data(struct xdg_popup *object);
uint32_t xdg_popup_get_version(struct xdg_popup *object);


#define XDG_TOPLEVEL_STATE_MAXIMIZED 1U
#define XDG_TOPLEVEL_STATE_FULLSCREEN 2U
#define XDG_TOPLEVEL_STATE_RESIZING 3U
#define XDG_TOPLEVEL_STATE_ACTIVATED 4U

#ifdef __cplusplus
}
#endif

#endif
