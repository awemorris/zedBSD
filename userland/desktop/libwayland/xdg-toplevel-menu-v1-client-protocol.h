/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the compositor's System Menu protocol (xdg_toplevel_menu_v1, WS070).
 *
 * The header is private: it is not installed, and applications reach the
 * protocol through libkeiland (<keiland/keiland.h>) only.  libkeiland and the
 * library's own event dispatch include it by its path in the tree.  The
 * protocol is defined in plan/ws070/design.md.
 */

#ifndef KERN_XDG_TOPLEVEL_MENU_V1_CLIENT_PROTOCOL_H
#define KERN_XDG_TOPLEVEL_MENU_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_seat;
struct xdg_toplevel;
struct xdg_menu_manager_v1;
struct xdg_menu_v1;
struct xdg_toplevel_menu_v1;
struct xdg_context_menu_v1;
struct wl_surface;

/* The four interfaces' descriptions (menu-protocol.c). */
extern const struct wl_interface xdg_menu_manager_v1_interface;
extern const struct wl_interface xdg_menu_v1_interface;
extern const struct wl_interface xdg_toplevel_menu_v1_interface;
extern const struct wl_interface xdg_context_menu_v1_interface;

/* xdg_menu_manager_v1: the global that makes menus and their places on windows. */
#define XDG_MENU_MANAGER_V1_ERROR_ALREADY_EXISTS 0U
#define XDG_MENU_MANAGER_V1_DESTROY 0U
#define XDG_MENU_MANAGER_V1_CREATE_MENU 1U
#define XDG_MENU_MANAGER_V1_GET_TOPLEVEL_MENU 2U
#define XDG_MENU_MANAGER_V1_GET_CONTEXT_MENU 3U
#define XDG_MENU_MANAGER_V1_GET_CONTEXT_MENU_SINCE_VERSION 2U
#define XDG_MENU_MANAGER_V1_ERROR_BAD_SERIAL 1U
void xdg_menu_manager_v1_destroy(struct xdg_menu_manager_v1 *object);
struct xdg_menu_v1 *xdg_menu_manager_v1_create_menu(struct xdg_menu_manager_v1 *object);
struct xdg_toplevel_menu_v1 *xdg_menu_manager_v1_get_toplevel_menu(struct xdg_menu_manager_v1 *object, struct xdg_toplevel *toplevel);
struct xdg_context_menu_v1 *xdg_menu_manager_v1_get_context_menu(struct xdg_menu_manager_v1 *object, struct xdg_menu_v1 *menu, struct wl_surface *surface, int32_t x, int32_t y, struct wl_seat *seat, uint32_t serial);
void xdg_menu_manager_v1_set_user_data(struct xdg_menu_manager_v1 *object, void *data);
void *xdg_menu_manager_v1_get_user_data(struct xdg_menu_manager_v1 *object);
uint32_t xdg_menu_manager_v1_get_version(struct xdg_menu_manager_v1 *object);

/* xdg_menu_v1: one menu model, a tree of items named by numbers. */
#define XDG_MENU_V1_ERROR_INVALID_ID 0U
#define XDG_MENU_V1_ERROR_INVALID_PARENT 1U
#define XDG_MENU_V1_ERROR_INVALID_TYPE 2U
#define XDG_MENU_V1_ERROR_INVALID_VALUE 3U
#define XDG_MENU_V1_ERROR_NOT_UPDATING 4U
#define XDG_MENU_V1_ERROR_ALREADY_UPDATING 5U
#define XDG_MENU_V1_ERROR_BAD_SERIAL 6U
#define XDG_MENU_V1_ERROR_TOO_LARGE 7U
#define XDG_MENU_V1_ITEM_TYPE_NORMAL 0U
#define XDG_MENU_V1_ITEM_TYPE_SEPARATOR 1U
#define XDG_MENU_V1_ITEM_TYPE_CHECKBOX 2U
#define XDG_MENU_V1_ITEM_TYPE_RADIO 3U
#define XDG_MENU_V1_ITEM_TYPE_SUBMENU 4U
#define XDG_MENU_V1_ROLE_NONE 0U
#define XDG_MENU_V1_ROLE_ABOUT 1U
#define XDG_MENU_V1_ROLE_PREFERENCES 2U
#define XDG_MENU_V1_ROLE_QUIT 3U
#define XDG_MENU_V1_ROLE_UNDO 4U
#define XDG_MENU_V1_ROLE_REDO 5U
#define XDG_MENU_V1_ROLE_CUT 6U
#define XDG_MENU_V1_ROLE_COPY 7U
#define XDG_MENU_V1_ROLE_PASTE 8U
#define XDG_MENU_V1_ROLE_DELETE 9U
#define XDG_MENU_V1_ROLE_SELECT_ALL 10U
#define XDG_MENU_V1_ROLE_NEW 11U
#define XDG_MENU_V1_ROLE_OPEN 12U
#define XDG_MENU_V1_ROLE_SAVE 13U
#define XDG_MENU_V1_ROLE_CLOSE 14U
#define XDG_MENU_V1_ROLE_FIND 15U
#define XDG_MENU_V1_ROLE_HELP 16U
#define XDG_MENU_V1_ROLE_FULLSCREEN 17U
#define XDG_MENU_V1_ROLE_ZOOM_IN 18U
#define XDG_MENU_V1_ROLE_ZOOM_OUT 19U
#define XDG_MENU_V1_MODIFIER_SHIFT 1U
#define XDG_MENU_V1_MODIFIER_CTRL 2U
#define XDG_MENU_V1_MODIFIER_ALT 4U
#define XDG_MENU_V1_MODIFIER_SUPER 8U
#define XDG_MENU_V1_DESTROY 0U
#define XDG_MENU_V1_BEGIN_UPDATE 1U
#define XDG_MENU_V1_COMMIT 2U
#define XDG_MENU_V1_APPEND_ITEM 3U
#define XDG_MENU_V1_INSERT_ITEM 4U
#define XDG_MENU_V1_REMOVE_ITEM 5U
#define XDG_MENU_V1_SET_LABEL 6U
#define XDG_MENU_V1_SET_ACTION 7U
#define XDG_MENU_V1_SET_ENABLED 8U
#define XDG_MENU_V1_SET_VISIBLE 9U
#define XDG_MENU_V1_SET_CHECKED 10U
#define XDG_MENU_V1_SET_ROLE 11U
#define XDG_MENU_V1_SET_ICON_NAME 12U
#define XDG_MENU_V1_SET_SHORTCUT 13U
void xdg_menu_v1_destroy(struct xdg_menu_v1 *object);
void xdg_menu_v1_begin_update(struct xdg_menu_v1 *object, uint32_t serial);
void xdg_menu_v1_commit(struct xdg_menu_v1 *object, uint32_t serial);
void xdg_menu_v1_append_item(struct xdg_menu_v1 *object, uint32_t id, uint32_t parent_id, uint32_t type, const char *label, uint32_t action);
void xdg_menu_v1_insert_item(struct xdg_menu_v1 *object, uint32_t id, uint32_t parent_id, uint32_t before_id, uint32_t type, const char *label, uint32_t action);
void xdg_menu_v1_remove_item(struct xdg_menu_v1 *object, uint32_t id);
void xdg_menu_v1_set_label(struct xdg_menu_v1 *object, uint32_t id, const char *label);
void xdg_menu_v1_set_action(struct xdg_menu_v1 *object, uint32_t id, uint32_t action);
void xdg_menu_v1_set_enabled(struct xdg_menu_v1 *object, uint32_t id, uint32_t enabled);
void xdg_menu_v1_set_visible(struct xdg_menu_v1 *object, uint32_t id, uint32_t visible);
void xdg_menu_v1_set_checked(struct xdg_menu_v1 *object, uint32_t id, uint32_t checked);
void xdg_menu_v1_set_role(struct xdg_menu_v1 *object, uint32_t id, uint32_t role);
void xdg_menu_v1_set_icon_name(struct xdg_menu_v1 *object, uint32_t id, const char *icon_name);
void xdg_menu_v1_set_shortcut(struct xdg_menu_v1 *object, uint32_t id, uint32_t modifiers, uint32_t keysym);
void xdg_menu_v1_set_user_data(struct xdg_menu_v1 *object, void *data);
void *xdg_menu_v1_get_user_data(struct xdg_menu_v1 *object);
uint32_t xdg_menu_v1_get_version(struct xdg_menu_v1 *object);

/* xdg_toplevel_menu_v1: where a window shows a menu, and what the user chose in it. */
struct xdg_toplevel_menu_v1_listener {
	void (*activated)(void *data, struct xdg_toplevel_menu_v1 *object, uint32_t item_id, uint32_t action, struct wl_seat *seat, uint32_t serial);
	void (*opened)(void *data, struct xdg_toplevel_menu_v1 *object, uint32_t item_id);
	void (*closed)(void *data, struct xdg_toplevel_menu_v1 *object, uint32_t item_id);
};
#define XDG_TOPLEVEL_MENU_V1_DESTROY 0U
#define XDG_TOPLEVEL_MENU_V1_SET_MENU 1U
int xdg_toplevel_menu_v1_add_listener(struct xdg_toplevel_menu_v1 *object, const struct xdg_toplevel_menu_v1_listener *listener, void *data);
void xdg_toplevel_menu_v1_destroy(struct xdg_toplevel_menu_v1 *object);
void xdg_toplevel_menu_v1_set_menu(struct xdg_toplevel_menu_v1 *object, struct xdg_menu_v1 *menu);
void xdg_toplevel_menu_v1_set_user_data(struct xdg_toplevel_menu_v1 *object, void *data);
void *xdg_toplevel_menu_v1_get_user_data(struct xdg_toplevel_menu_v1 *object);
uint32_t xdg_toplevel_menu_v1_get_version(struct xdg_toplevel_menu_v1 *object);

/* xdg_context_menu_v1 (version 2): a menu opened once at a point of a surface; the choice, then the end. */
struct xdg_context_menu_v1_listener {
	void (*activated)(void *data, struct xdg_context_menu_v1 *object, uint32_t item_id, uint32_t action, uint32_t serial);
	void (*done)(void *data, struct xdg_context_menu_v1 *object);
};
#define XDG_CONTEXT_MENU_V1_DESTROY 0U
int xdg_context_menu_v1_add_listener(struct xdg_context_menu_v1 *object, const struct xdg_context_menu_v1_listener *listener, void *data);
void xdg_context_menu_v1_destroy(struct xdg_context_menu_v1 *object);

#ifdef __cplusplus
}
#endif

#endif
