/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the input method protocol's objects, listeners and typed
 * requests (input-method-unstable-v2, version 1; ws095-p004), by which the
 * system's input method hears the keyboard and the focused text input and
 * sends the text it makes.  The names follow the upstream client header;
 * the wire descriptions were checked against the pinned description
 * (API-PROVENANCE.md).
 */

#ifndef KERN_INPUT_METHOD_UNSTABLE_V2_CLIENT_PROTOCOL_H
#define KERN_INPUT_METHOD_UNSTABLE_V2_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_seat;
struct wl_surface;
struct zwp_input_popup_surface_v2;
struct zwp_input_method_keyboard_grab_v2;
struct zwp_input_method_v2;
struct zwp_input_method_manager_v2;

/* The interfaces' descriptions (userland/desktop/libwayland/input-method-protocol.c). */
extern const struct wl_interface zwp_input_popup_surface_v2_interface;
extern const struct wl_interface zwp_input_method_keyboard_grab_v2_interface;
extern const struct wl_interface zwp_input_method_v2_interface;
extern const struct wl_interface zwp_input_method_manager_v2_interface;

/* zwp_input_popup_surface_v2: a surface shown by the text being composed (the candidate window). */
struct zwp_input_popup_surface_v2_listener {
	void (*text_input_rectangle)(void *data, struct zwp_input_popup_surface_v2 *zwp_input_popup_surface_v2, int32_t x, int32_t y, int32_t width, int32_t height);
};
int zwp_input_popup_surface_v2_add_listener(struct zwp_input_popup_surface_v2 *zwp_input_popup_surface_v2, const struct zwp_input_popup_surface_v2_listener *listener, void *data);
#define ZWP_INPUT_POPUP_SURFACE_V2_DESTROY 0U
void zwp_input_popup_surface_v2_destroy(struct zwp_input_popup_surface_v2 *zwp_input_popup_surface_v2);

/* zwp_input_method_keyboard_grab_v2: the keyboard, while the input method holds it. */
struct zwp_input_method_keyboard_grab_v2_listener {
	void (*keymap)(void *data, struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2, uint32_t format, int32_t fd, uint32_t size);
	void (*key)(void *data, struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
	void (*modifiers)(void *data, struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
	void (*repeat_info)(void *data, struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2, int32_t rate, int32_t delay);
};
int zwp_input_method_keyboard_grab_v2_add_listener(struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2, const struct zwp_input_method_keyboard_grab_v2_listener *listener, void *data);
#define ZWP_INPUT_METHOD_KEYBOARD_GRAB_V2_RELEASE 0U
void zwp_input_method_keyboard_grab_v2_release(struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2);

/* zwp_input_method_v2: the input method of a seat. */
struct zwp_input_method_v2_listener {
	void (*activate)(void *data, struct zwp_input_method_v2 *zwp_input_method_v2);
	void (*deactivate)(void *data, struct zwp_input_method_v2 *zwp_input_method_v2);
	void (*surrounding_text)(void *data, struct zwp_input_method_v2 *zwp_input_method_v2, const char *text, uint32_t cursor, uint32_t anchor);
	void (*text_change_cause)(void *data, struct zwp_input_method_v2 *zwp_input_method_v2, uint32_t cause);
	void (*content_type)(void *data, struct zwp_input_method_v2 *zwp_input_method_v2, uint32_t hint, uint32_t purpose);
	void (*done)(void *data, struct zwp_input_method_v2 *zwp_input_method_v2);
	void (*unavailable)(void *data, struct zwp_input_method_v2 *zwp_input_method_v2);
};
int zwp_input_method_v2_add_listener(struct zwp_input_method_v2 *zwp_input_method_v2, const struct zwp_input_method_v2_listener *listener, void *data);
#define ZWP_INPUT_METHOD_V2_COMMIT_STRING 0U
#define ZWP_INPUT_METHOD_V2_SET_PREEDIT_STRING 1U
#define ZWP_INPUT_METHOD_V2_DELETE_SURROUNDING_TEXT 2U
#define ZWP_INPUT_METHOD_V2_COMMIT 3U
#define ZWP_INPUT_METHOD_V2_GET_INPUT_POPUP_SURFACE 4U
#define ZWP_INPUT_METHOD_V2_GRAB_KEYBOARD 5U
#define ZWP_INPUT_METHOD_V2_DESTROY 6U
void zwp_input_method_v2_commit_string(struct zwp_input_method_v2 *zwp_input_method_v2, const char *text);
void zwp_input_method_v2_set_preedit_string(struct zwp_input_method_v2 *zwp_input_method_v2, const char *text, int32_t cursor_begin, int32_t cursor_end);
void zwp_input_method_v2_delete_surrounding_text(struct zwp_input_method_v2 *zwp_input_method_v2, uint32_t before_length, uint32_t after_length);
void zwp_input_method_v2_commit(struct zwp_input_method_v2 *zwp_input_method_v2, uint32_t serial);
struct zwp_input_popup_surface_v2 *zwp_input_method_v2_get_input_popup_surface(struct zwp_input_method_v2 *zwp_input_method_v2, struct wl_surface *surface);
struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_v2_grab_keyboard(struct zwp_input_method_v2 *zwp_input_method_v2);
void zwp_input_method_v2_destroy(struct zwp_input_method_v2 *zwp_input_method_v2);

/* zwp_input_method_manager_v2: the global that makes the input method. */
#define ZWP_INPUT_METHOD_MANAGER_V2_GET_INPUT_METHOD 0U
#define ZWP_INPUT_METHOD_MANAGER_V2_DESTROY 1U
struct zwp_input_method_v2 *zwp_input_method_manager_v2_get_input_method(struct zwp_input_method_manager_v2 *zwp_input_method_manager_v2, struct wl_seat *seat);
void zwp_input_method_manager_v2_destroy(struct zwp_input_method_manager_v2 *zwp_input_method_manager_v2);

#ifdef __cplusplus
}
#endif

#endif
