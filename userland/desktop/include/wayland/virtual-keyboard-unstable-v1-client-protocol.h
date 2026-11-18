/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the virtual keyboard protocol's objects and typed requests
 * (virtual-keyboard-unstable-v1, version 1; ws095-p004), by which the input
 * method gives back the keys it does not use.  The names follow the upstream
 * client header; the wire descriptions were checked against the pinned
 * description (API-PROVENANCE.md).
 */

#ifndef KERN_VIRTUAL_KEYBOARD_UNSTABLE_V1_CLIENT_PROTOCOL_H
#define KERN_VIRTUAL_KEYBOARD_UNSTABLE_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_seat;
struct zwp_virtual_keyboard_v1;
struct zwp_virtual_keyboard_manager_v1;

/* The interfaces' descriptions (userland/desktop/libwayland/virtual-keyboard-protocol.c). */
extern const struct wl_interface zwp_virtual_keyboard_v1_interface;
extern const struct wl_interface zwp_virtual_keyboard_manager_v1_interface;

/* zwp_virtual_keyboard_v1: a keyboard whose keys a client makes. */
#define ZWP_VIRTUAL_KEYBOARD_V1_KEYMAP 0U
#define ZWP_VIRTUAL_KEYBOARD_V1_KEY 1U
#define ZWP_VIRTUAL_KEYBOARD_V1_MODIFIERS 2U
#define ZWP_VIRTUAL_KEYBOARD_V1_DESTROY 3U
void zwp_virtual_keyboard_v1_keymap(struct zwp_virtual_keyboard_v1 *zwp_virtual_keyboard_v1, uint32_t format, int32_t fd, uint32_t size);
void zwp_virtual_keyboard_v1_key(struct zwp_virtual_keyboard_v1 *zwp_virtual_keyboard_v1, uint32_t time, uint32_t key, uint32_t state);
void zwp_virtual_keyboard_v1_modifiers(struct zwp_virtual_keyboard_v1 *zwp_virtual_keyboard_v1, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
void zwp_virtual_keyboard_v1_destroy(struct zwp_virtual_keyboard_v1 *zwp_virtual_keyboard_v1);

/* zwp_virtual_keyboard_manager_v1: the global that makes virtual keyboards. */
#define ZWP_VIRTUAL_KEYBOARD_MANAGER_V1_CREATE_VIRTUAL_KEYBOARD 0U
struct zwp_virtual_keyboard_v1 *zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(struct zwp_virtual_keyboard_manager_v1 *zwp_virtual_keyboard_manager_v1, struct wl_seat *seat);

#ifdef __cplusplus
}
#endif

#endif
