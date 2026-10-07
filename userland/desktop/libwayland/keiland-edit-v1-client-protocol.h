/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the compositor's editing operations protocol (kl_edit_v1,
 * ws102-p017, plan/ws102/design.md section 2.10).
 *
 * The header is private: it is not installed, and applications reach the
 * protocol through libkeiland (<keiland/keiland.h>) only.  libkeiland includes it
 * by its path in the tree.
 */

#ifndef KERN_KEILAND_EDIT_V1_CLIENT_PROTOCOL_H
#define KERN_KEILAND_EDIT_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct xdg_toplevel;
struct kl_edit_manager_v1;
struct kl_edit_v1;

/* The two interfaces' descriptions (edit-protocol.c). */
extern const struct wl_interface kl_edit_manager_v1_interface;
extern const struct wl_interface kl_edit_v1_interface;

/* kl_edit_manager_v1: the global that gives windows their edit object. */
#define KL_EDIT_MANAGER_V1_DESTROY 0U
#define KL_EDIT_MANAGER_V1_GET_EDIT 1U
void kl_edit_manager_v1_destroy(struct kl_edit_manager_v1 *object);
struct kl_edit_v1 *kl_edit_manager_v1_get_edit(struct kl_edit_manager_v1 *object, struct xdg_toplevel *toplevel);

/*
 * kl_edit_v1: a window's editing operations.  set_state says which
 * operations the window carries out (bit 1 << action) and its state (the
 * STATE bits); action is an operation the compositor asks for.
 */
#define KL_EDIT_V1_ACTION_COPY 0U
#define KL_EDIT_V1_ACTION_CUT 1U
#define KL_EDIT_V1_ACTION_PASTE 2U
#define KL_EDIT_V1_ACTION_UNDO 3U
#define KL_EDIT_V1_ACTION_REDO 4U
#define KL_EDIT_V1_ACTION_SELECT_ALL 5U
#define KL_EDIT_V1_ACTION_SELECT_BEGIN 6U
#define KL_EDIT_V1_ACTION_SELECT_END 7U
#define KL_EDIT_V1_STATE_HAS_SELECTION 1U
#define KL_EDIT_V1_STATE_CAN_PASTE 2U
#define KL_EDIT_V1_STATE_CAN_UNDO 4U
#define KL_EDIT_V1_STATE_CAN_REDO 8U
#define KL_EDIT_V1_STATE_SELECTING 16U
struct kl_edit_v1_listener {
	void (*action)(void *data, struct kl_edit_v1 *object, uint32_t action);
};
int kl_edit_v1_add_listener(struct kl_edit_v1 *object, const struct kl_edit_v1_listener *listener, void *data);
#define KL_EDIT_V1_DESTROY 0U
#define KL_EDIT_V1_SET_STATE 1U
void kl_edit_v1_destroy(struct kl_edit_v1 *object);
void kl_edit_v1_set_state(struct kl_edit_v1 *object, uint32_t actions, uint32_t state);

#ifdef __cplusplus
}
#endif

#endif
