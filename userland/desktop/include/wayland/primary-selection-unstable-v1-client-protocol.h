/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the primary selection protocol's objects, listeners and typed
 * requests (wp_primary_selection_unstable_v1, version 1; ws035-p100).  The
 * names follow the upstream client header; the wire descriptions were
 * checked against the pinned description (API-PROVENANCE.md).
 */

#ifndef KERN_PRIMARY_SELECTION_UNSTABLE_V1_CLIENT_PROTOCOL_H
#define KERN_PRIMARY_SELECTION_UNSTABLE_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct wl_seat;
struct zwp_primary_selection_device_manager_v1;
struct zwp_primary_selection_device_v1;
struct zwp_primary_selection_offer_v1;
struct zwp_primary_selection_source_v1;

/* The four interfaces' descriptions (userland/desktop/libwayland/primary-selection-protocol.c). */
extern const struct wl_interface zwp_primary_selection_device_manager_v1_interface;
extern const struct wl_interface zwp_primary_selection_device_v1_interface;
extern const struct wl_interface zwp_primary_selection_offer_v1_interface;
extern const struct wl_interface zwp_primary_selection_source_v1_interface;

/* zwp_primary_selection_device_manager_v1: the global that makes sources and a seat's device. */
#define ZWP_PRIMARY_SELECTION_DEVICE_MANAGER_V1_CREATE_SOURCE 0U
#define ZWP_PRIMARY_SELECTION_DEVICE_MANAGER_V1_GET_DEVICE 1U
#define ZWP_PRIMARY_SELECTION_DEVICE_MANAGER_V1_DESTROY 2U
struct zwp_primary_selection_source_v1 *zwp_primary_selection_device_manager_v1_create_source(struct zwp_primary_selection_device_manager_v1 *zwp_primary_selection_device_manager_v1);
struct zwp_primary_selection_device_v1 *zwp_primary_selection_device_manager_v1_get_device(struct zwp_primary_selection_device_manager_v1 *zwp_primary_selection_device_manager_v1, struct wl_seat *seat);
void zwp_primary_selection_device_manager_v1_destroy(struct zwp_primary_selection_device_manager_v1 *zwp_primary_selection_device_manager_v1);

/* zwp_primary_selection_device_v1: a seat's primary selection. */
struct zwp_primary_selection_device_v1_listener {
	void (*data_offer)(void *data, struct zwp_primary_selection_device_v1 *zwp_primary_selection_device_v1, struct zwp_primary_selection_offer_v1 *offer);
	void (*selection)(void *data, struct zwp_primary_selection_device_v1 *zwp_primary_selection_device_v1, struct zwp_primary_selection_offer_v1 *id);
};
int zwp_primary_selection_device_v1_add_listener(struct zwp_primary_selection_device_v1 *zwp_primary_selection_device_v1, const struct zwp_primary_selection_device_v1_listener *listener, void *data);
#define ZWP_PRIMARY_SELECTION_DEVICE_V1_SET_SELECTION 0U
#define ZWP_PRIMARY_SELECTION_DEVICE_V1_DESTROY 1U
void zwp_primary_selection_device_v1_set_selection(struct zwp_primary_selection_device_v1 *zwp_primary_selection_device_v1, struct zwp_primary_selection_source_v1 *source, uint32_t serial);
void zwp_primary_selection_device_v1_destroy(struct zwp_primary_selection_device_v1 *zwp_primary_selection_device_v1);

/* zwp_primary_selection_offer_v1: another client's primary selection, offered. */
struct zwp_primary_selection_offer_v1_listener {
	void (*offer)(void *data, struct zwp_primary_selection_offer_v1 *zwp_primary_selection_offer_v1, const char *mime_type);
};
int zwp_primary_selection_offer_v1_add_listener(struct zwp_primary_selection_offer_v1 *zwp_primary_selection_offer_v1, const struct zwp_primary_selection_offer_v1_listener *listener, void *data);
#define ZWP_PRIMARY_SELECTION_OFFER_V1_RECEIVE 0U
#define ZWP_PRIMARY_SELECTION_OFFER_V1_DESTROY 1U
void zwp_primary_selection_offer_v1_receive(struct zwp_primary_selection_offer_v1 *zwp_primary_selection_offer_v1, const char *mime_type, int32_t fd);
void zwp_primary_selection_offer_v1_destroy(struct zwp_primary_selection_offer_v1 *zwp_primary_selection_offer_v1);

/* zwp_primary_selection_source_v1: the text a client offers as the primary selection. */
struct zwp_primary_selection_source_v1_listener {
	void (*send)(void *data, struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1, const char *mime_type, int32_t fd);
	void (*cancelled)(void *data, struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1);
};
int zwp_primary_selection_source_v1_add_listener(struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1, const struct zwp_primary_selection_source_v1_listener *listener, void *data);
#define ZWP_PRIMARY_SELECTION_SOURCE_V1_OFFER 0U
#define ZWP_PRIMARY_SELECTION_SOURCE_V1_DESTROY 1U
void zwp_primary_selection_source_v1_offer(struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1, const char *mime_type);
void zwp_primary_selection_source_v1_destroy(struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1);

#ifdef __cplusplus
}
#endif

#endif
