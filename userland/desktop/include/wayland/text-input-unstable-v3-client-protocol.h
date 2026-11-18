/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the text input protocol's objects, listeners and typed requests
 * (text-input-unstable-v3, interface version 1; ws095-p004), by which an
 * application tells the input method where text goes and hears the text it
 * makes.  The names follow the upstream client header; the wire
 * descriptions were checked against the pinned description
 * (API-PROVENANCE.md).
 */

#ifndef KERN_TEXT_INPUT_UNSTABLE_V3_CLIENT_PROTOCOL_H
#define KERN_TEXT_INPUT_UNSTABLE_V3_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_seat;
struct wl_surface;
struct zwp_text_input_v3;
struct zwp_text_input_manager_v3;

/* The interfaces' descriptions (userland/desktop/libwayland/text-input-protocol.c). */
extern const struct wl_interface zwp_text_input_v3_interface;
extern const struct wl_interface zwp_text_input_manager_v3_interface;

/* zwp_text_input_v3: one application's text input on a seat. */
#define ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD 0U
#define ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER 1U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE 0x0U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_COMPLETION 0x1U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_SPELLCHECK 0x2U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_AUTO_CAPITALIZATION 0x4U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_LOWERCASE 0x8U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_UPPERCASE 0x10U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_TITLECASE 0x20U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT 0x40U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA 0x80U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_LATIN 0x100U
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE 0x200U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL 0U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_ALPHA 1U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DIGITS 2U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NUMBER 3U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PHONE 4U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_URL 5U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_EMAIL 6U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NAME 7U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD 8U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PIN 9U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DATE 10U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TIME 11U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DATETIME 12U
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL 13U
struct zwp_text_input_v3_listener {
	void (*enter)(void *data, struct zwp_text_input_v3 *zwp_text_input_v3, struct wl_surface *surface);
	void (*leave)(void *data, struct zwp_text_input_v3 *zwp_text_input_v3, struct wl_surface *surface);
	void (*preedit_string)(void *data, struct zwp_text_input_v3 *zwp_text_input_v3, const char *text, int32_t cursor_begin, int32_t cursor_end);
	void (*commit_string)(void *data, struct zwp_text_input_v3 *zwp_text_input_v3, const char *text);
	void (*delete_surrounding_text)(void *data, struct zwp_text_input_v3 *zwp_text_input_v3, uint32_t before_length, uint32_t after_length);
	void (*done)(void *data, struct zwp_text_input_v3 *zwp_text_input_v3, uint32_t serial);
};
int zwp_text_input_v3_add_listener(struct zwp_text_input_v3 *zwp_text_input_v3, const struct zwp_text_input_v3_listener *listener, void *data);
#define ZWP_TEXT_INPUT_V3_DESTROY 0U
#define ZWP_TEXT_INPUT_V3_ENABLE 1U
#define ZWP_TEXT_INPUT_V3_DISABLE 2U
#define ZWP_TEXT_INPUT_V3_SET_SURROUNDING_TEXT 3U
#define ZWP_TEXT_INPUT_V3_SET_TEXT_CHANGE_CAUSE 4U
#define ZWP_TEXT_INPUT_V3_SET_CONTENT_TYPE 5U
#define ZWP_TEXT_INPUT_V3_SET_CURSOR_RECTANGLE 6U
#define ZWP_TEXT_INPUT_V3_COMMIT 7U
void zwp_text_input_v3_destroy(struct zwp_text_input_v3 *zwp_text_input_v3);
void zwp_text_input_v3_enable(struct zwp_text_input_v3 *zwp_text_input_v3);
void zwp_text_input_v3_disable(struct zwp_text_input_v3 *zwp_text_input_v3);
void zwp_text_input_v3_set_surrounding_text(struct zwp_text_input_v3 *zwp_text_input_v3, const char *text, int32_t cursor, int32_t anchor);
void zwp_text_input_v3_set_text_change_cause(struct zwp_text_input_v3 *zwp_text_input_v3, uint32_t cause);
void zwp_text_input_v3_set_content_type(struct zwp_text_input_v3 *zwp_text_input_v3, uint32_t hint, uint32_t purpose);
void zwp_text_input_v3_set_cursor_rectangle(struct zwp_text_input_v3 *zwp_text_input_v3, int32_t x, int32_t y, int32_t width, int32_t height);
void zwp_text_input_v3_commit(struct zwp_text_input_v3 *zwp_text_input_v3);

/* zwp_text_input_manager_v3: the global that makes text inputs. */
#define ZWP_TEXT_INPUT_MANAGER_V3_DESTROY 0U
#define ZWP_TEXT_INPUT_MANAGER_V3_GET_TEXT_INPUT 1U
void zwp_text_input_manager_v3_destroy(struct zwp_text_input_manager_v3 *zwp_text_input_manager_v3);
struct zwp_text_input_v3 *zwp_text_input_manager_v3_get_text_input(struct zwp_text_input_manager_v3 *zwp_text_input_manager_v3, struct wl_seat *seat);

#ifdef __cplusplus
}
#endif

#endif
