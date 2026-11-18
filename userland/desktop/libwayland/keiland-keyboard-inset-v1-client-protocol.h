/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the compositor's keyboard inset protocol (kl_keyboard_inset_v1,
 * ws102-p015, plan/ws102/design.md section 2.8).
 *
 * The header is private: it is not installed, and applications reach the
 * protocol through libkeiland (<keiland/keiland.h>) only.  libkeiland includes it
 * by its path in the tree.
 */

#ifndef KERN_KEILAND_KEYBOARD_INSET_V1_CLIENT_PROTOCOL_H
#define KERN_KEILAND_KEYBOARD_INSET_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct xdg_toplevel;
struct kl_keyboard_inset_manager_v1;
struct kl_keyboard_inset_v1;

/* The two interfaces' descriptions (keyboard-inset-protocol.c). */
extern const struct wl_interface kl_keyboard_inset_manager_v1_interface;
extern const struct wl_interface kl_keyboard_inset_v1_interface;

/* kl_keyboard_inset_manager_v1: the global that gives windows their inset. */
#define KL_KEYBOARD_INSET_MANAGER_V1_DESTROY 0U
#define KL_KEYBOARD_INSET_MANAGER_V1_GET_INSET 1U
void kl_keyboard_inset_manager_v1_destroy(struct kl_keyboard_inset_manager_v1 *object);
struct kl_keyboard_inset_v1 *kl_keyboard_inset_manager_v1_get_inset(struct kl_keyboard_inset_manager_v1 *object, struct xdg_toplevel *toplevel);

/*
 * kl_keyboard_inset_v1: how much of one window the on-screen keyboard
 * covers, in the window's pixels from its right and bottom edges, sent when
 * the keyboard opens, closes or changes the window (before that configure).
 */
#define KL_KEYBOARD_INSET_V1_REASON_NONE 0U
#define KL_KEYBOARD_INSET_V1_REASON_RIGHT 1U
#define KL_KEYBOARD_INSET_V1_REASON_BOTTOM 2U
struct kl_keyboard_inset_v1_listener {
	void (*inset)(void *data, struct kl_keyboard_inset_v1 *object, int32_t right, int32_t bottom, uint32_t reason);
};
int kl_keyboard_inset_v1_add_listener(struct kl_keyboard_inset_v1 *object, const struct kl_keyboard_inset_v1_listener *listener, void *data);
#define KL_KEYBOARD_INSET_V1_DESTROY 0U
void kl_keyboard_inset_v1_destroy(struct kl_keyboard_inset_v1 *object);

#ifdef __cplusplus
}
#endif

#endif
