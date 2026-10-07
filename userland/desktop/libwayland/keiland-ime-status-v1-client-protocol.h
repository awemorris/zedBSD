/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares zdesktop's input method status protocol (kl_ime_status_v1,
 * version 2; ws095-p004, plan/ws095/design.md section 8): the input method
 * tells zdesktop its language and whether text is being composed, and
 * zdesktop tells it to change language.  Version 2 (ws166-p002): zdesktop
 * asks for the words a reading of the on-screen keyboard starts (predict),
 * the input method answers (predictions: "WORD\tREADING" lines), and the
 * word chosen is learned (learn).  The protocol is zdesktop's own and
 * this header is private to the tree.
 */

#ifndef KEILAND_IME_STATUS_V1_CLIENT_PROTOCOL_H
#define KEILAND_IME_STATUS_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct kl_ime_status_v1;
struct kl_ime_status_manager_v1;

/* The interfaces' descriptions (userland/desktop/libwayland/ime-status-protocol.c). */
extern const struct wl_interface kl_ime_status_v1_interface;
extern const struct wl_interface kl_ime_status_manager_v1_interface;

/* kl_ime_status_v1: the input method's status. */
struct kl_ime_status_v1_listener {
	void (*next)(void *data, struct kl_ime_status_v1 *kl_ime_status_v1);
	void (*select)(void *data, struct kl_ime_status_v1 *kl_ime_status_v1, const char *id);
	void (*predict)(void *data, struct kl_ime_status_v1 *kl_ime_status_v1, uint32_t serial, const char *reading);
	void (*learn)(void *data, struct kl_ime_status_v1 *kl_ime_status_v1, const char *reading, const char *word);
};
int kl_ime_status_v1_add_listener(struct kl_ime_status_v1 *kl_ime_status_v1, const struct kl_ime_status_v1_listener *listener, void *data);
#define KL_IME_STATUS_V1_DESTROY 0U
#define KL_IME_STATUS_V1_LANGUAGE 1U
#define KL_IME_STATUS_V1_COMPOSING 2U
#define KL_IME_STATUS_V1_PREDICTIONS 3U
void kl_ime_status_v1_destroy(struct kl_ime_status_v1 *kl_ime_status_v1);
void kl_ime_status_v1_language(struct kl_ime_status_v1 *kl_ime_status_v1, const char *id, const char *label);
void kl_ime_status_v1_composing(struct kl_ime_status_v1 *kl_ime_status_v1, uint32_t composing);
void kl_ime_status_v1_predictions(struct kl_ime_status_v1 *kl_ime_status_v1, uint32_t serial, const char *list);

/* kl_ime_status_manager_v1: the global that gives the input method its status. */
#define KL_IME_STATUS_MANAGER_V1_DESTROY 0U
#define KL_IME_STATUS_MANAGER_V1_GET_STATUS 1U
void kl_ime_status_manager_v1_destroy(struct kl_ime_status_manager_v1 *kl_ime_status_manager_v1);
struct kl_ime_status_v1 *kl_ime_status_manager_v1_get_status(struct kl_ime_status_manager_v1 *kl_ime_status_manager_v1);

#ifdef __cplusplus
}
#endif

#endif
