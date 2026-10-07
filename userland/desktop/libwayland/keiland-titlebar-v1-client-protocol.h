/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares zdesktop's Titlebar Presentation protocol (kl_titlebar_v1,
 * WS070 p008).
 *
 * The header is private: it is not installed, and applications reach the
 * protocol through libkeiland (<keiland/keiland.h>) only.  libkeiland and the
 * library's own event dispatch include it by its path in the tree.  The
 * protocol is defined in plan/ws070/titlebar-design.md.
 */

#ifndef KERN_KEILAND_TITLEBAR_V1_CLIENT_PROTOCOL_H
#define KERN_KEILAND_TITLEBAR_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_array;
struct wl_seat;
struct xdg_toplevel;
struct kl_titlebar_manager_v1;
struct kl_titlebar_v1;

/* The two interfaces' descriptions (titlebar-protocol.c). */
extern const struct wl_interface kl_titlebar_manager_v1_interface;
extern const struct wl_interface kl_titlebar_v1_interface;

/* kl_titlebar_manager_v1: the global that gives windows their titlebar's presentation. */
#define KL_TITLEBAR_MANAGER_V1_ERROR_ALREADY_EXISTS 0U
#define KL_TITLEBAR_MANAGER_V1_DESTROY 0U
#define KL_TITLEBAR_MANAGER_V1_GET_TITLEBAR 1U
void kl_titlebar_manager_v1_destroy(struct kl_titlebar_manager_v1 *object);
struct kl_titlebar_v1 *kl_titlebar_manager_v1_get_titlebar(struct kl_titlebar_manager_v1 *object, struct xdg_toplevel *toplevel);
void kl_titlebar_manager_v1_set_user_data(struct kl_titlebar_manager_v1 *object, void *data);
void *kl_titlebar_manager_v1_get_user_data(struct kl_titlebar_manager_v1 *object);
uint32_t kl_titlebar_manager_v1_get_version(struct kl_titlebar_manager_v1 *object);

/* kl_titlebar_v1: one window's titlebar: its mode, controls and tabs. */
#define KL_TITLEBAR_V1_ERROR_INVALID_ID 0U
#define KL_TITLEBAR_V1_ERROR_INVALID_VALUE 1U
#define KL_TITLEBAR_V1_ERROR_NOT_UPDATING 2U
#define KL_TITLEBAR_V1_ERROR_ALREADY_UPDATING 3U
#define KL_TITLEBAR_V1_ERROR_BAD_SERIAL 4U
#define KL_TITLEBAR_V1_ERROR_TOO_LARGE 5U
#define KL_TITLEBAR_V1_MODE_MENU 0U
#define KL_TITLEBAR_V1_MODE_CONTROLS 1U
#define KL_TITLEBAR_V1_MODE_TABS 2U
#define KL_TITLEBAR_V1_DESTROY 0U
#define KL_TITLEBAR_V1_BEGIN_UPDATE 1U
#define KL_TITLEBAR_V1_COMMIT 2U
#define KL_TITLEBAR_V1_SET_MODE 3U
#define KL_TITLEBAR_V1_ADD_CONTROL 4U
#define KL_TITLEBAR_V1_REMOVE_CONTROL 5U
#define KL_TITLEBAR_V1_SET_CONTROL_LABEL 6U
#define KL_TITLEBAR_V1_SET_CONTROL_STATE 7U
#define KL_TITLEBAR_V1_SET_CONTROL_VALUE 8U
#define KL_TITLEBAR_V1_SET_CONTROL_TEXT 9U
#define KL_TITLEBAR_V1_SET_BREADCRUMB 10U
#define KL_TITLEBAR_V1_ADD_TAB 11U
#define KL_TITLEBAR_V1_REMOVE_TAB 12U
#define KL_TITLEBAR_V1_SET_TAB 13U
#define KL_TITLEBAR_V1_SET_TABS_OPTIONS 14U
#define KL_TITLEBAR_V1_FOCUS_CONTROL 15U
#define KL_TITLEBAR_V1_SET_SUGGESTIONS 16U
struct kl_titlebar_v1_listener {
	void (*control_activated)(void *data, struct kl_titlebar_v1 *object, uint32_t id, uint32_t detail, struct wl_seat *seat, uint32_t serial);
	void (*text_changed)(void *data, struct kl_titlebar_v1 *object, uint32_t id, const char *text);
	void (*text_done)(void *data, struct kl_titlebar_v1 *object, uint32_t id, const char *text, uint32_t how);
	void (*tab_activated)(void *data, struct kl_titlebar_v1 *object, uint32_t id, uint32_t serial);
	void (*tab_close_requested)(void *data, struct kl_titlebar_v1 *object, uint32_t id);
	void (*new_tab_requested)(void *data, struct kl_titlebar_v1 *object, uint32_t serial);
	void (*overflow_menu_opened)(void *data, struct kl_titlebar_v1 *object);

	/* Version 2: a drag and drop is over a part of a control (id 0: over none of them now). */
	void (*drop_target)(void *data, struct kl_titlebar_v1 *object, uint32_t id, uint32_t detail);
};
int kl_titlebar_v1_add_listener(struct kl_titlebar_v1 *object, const struct kl_titlebar_v1_listener *listener, void *data);
void kl_titlebar_v1_destroy(struct kl_titlebar_v1 *object);
void kl_titlebar_v1_begin_update(struct kl_titlebar_v1 *object, uint32_t serial);
void kl_titlebar_v1_commit(struct kl_titlebar_v1 *object, uint32_t serial);
void kl_titlebar_v1_set_mode(struct kl_titlebar_v1 *object, uint32_t mode);
void kl_titlebar_v1_add_control(struct kl_titlebar_v1 *object, uint32_t id, uint32_t role, uint32_t priority, uint32_t group, const char *label);
void kl_titlebar_v1_remove_control(struct kl_titlebar_v1 *object, uint32_t id);
void kl_titlebar_v1_set_control_label(struct kl_titlebar_v1 *object, uint32_t id, const char *label);
void kl_titlebar_v1_set_control_state(struct kl_titlebar_v1 *object, uint32_t id, uint32_t enabled, uint32_t checked);
void kl_titlebar_v1_set_control_value(struct kl_titlebar_v1 *object, uint32_t id, uint32_t value);
void kl_titlebar_v1_set_control_text(struct kl_titlebar_v1 *object, uint32_t id, const char *text, const char *placeholder);
void kl_titlebar_v1_set_breadcrumb(struct kl_titlebar_v1 *object, uint32_t id, struct wl_array *segments);
void kl_titlebar_v1_add_tab(struct kl_titlebar_v1 *object, uint32_t id, const char *title);
void kl_titlebar_v1_remove_tab(struct kl_titlebar_v1 *object, uint32_t id);
void kl_titlebar_v1_set_tab(struct kl_titlebar_v1 *object, uint32_t id, const char *title, uint32_t flags);
void kl_titlebar_v1_set_tabs_options(struct kl_titlebar_v1 *object, uint32_t options);
void kl_titlebar_v1_focus_control(struct kl_titlebar_v1 *object, uint32_t id, uint32_t mode);
void kl_titlebar_v1_set_suggestions(struct kl_titlebar_v1 *object, uint32_t id, struct wl_array *suggestions);
void kl_titlebar_v1_set_user_data(struct kl_titlebar_v1 *object, void *data);
void *kl_titlebar_v1_get_user_data(struct kl_titlebar_v1 *object);
uint32_t kl_titlebar_v1_get_version(struct kl_titlebar_v1 *object);

#ifdef __cplusplus
}
#endif

#endif
