/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the independent minimal Wayland client connection and proxy ABI.
 */

#ifndef ZEDBSD_WAYLAND_CLIENT_CORE_H
#define ZEDBSD_WAYLAND_CLIENT_CORE_H

#include <wayland/wayland-util.h>

#define WL_MARSHAL_FLAG_DESTROY 1U

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_proxy;
struct wl_event_queue;

struct wl_display *wl_display_connect(const char *name);
struct wl_display *wl_display_connect_to_fd(int fd);
void wl_display_disconnect(struct wl_display *display);
int wl_display_get_fd(struct wl_display *display);
int wl_display_get_error(struct wl_display *display);
uint32_t wl_display_get_protocol_error(struct wl_display *display, const struct wl_interface **interface, uint32_t *id);
int wl_display_flush(struct wl_display *display);
int wl_display_dispatch(struct wl_display *display);
int wl_display_dispatch_pending(struct wl_display *display);
int wl_display_dispatch_queue(struct wl_display *display, struct wl_event_queue *queue);
int wl_display_dispatch_queue_pending(struct wl_display *display, struct wl_event_queue *queue);
int wl_display_prepare_read(struct wl_display *display);
int wl_display_prepare_read_queue(struct wl_display *display, struct wl_event_queue *queue);
int wl_display_read_events(struct wl_display *display);
void wl_display_cancel_read(struct wl_display *display);
int wl_display_roundtrip(struct wl_display *display);
int wl_display_roundtrip_queue(struct wl_display *display, struct wl_event_queue *queue);
struct wl_event_queue *wl_display_create_queue(struct wl_display *display);
struct wl_event_queue *wl_display_create_queue_with_name(struct wl_display *display, const char *name);
void wl_event_queue_destroy(struct wl_event_queue *queue);
const char *wl_event_queue_get_name(const struct wl_event_queue *queue);
struct wl_proxy *wl_proxy_create(struct wl_proxy *factory, const struct wl_interface *interface);
void wl_proxy_destroy(struct wl_proxy *proxy);
void *wl_proxy_create_wrapper(void *proxy);
void wl_proxy_wrapper_destroy(void *wrapper);
void wl_proxy_marshal(struct wl_proxy *proxy, uint32_t opcode, ...);
void wl_proxy_marshal_array(struct wl_proxy *proxy, uint32_t opcode, union wl_argument *args);
struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, uint32_t flags, ...);
struct wl_proxy *wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, uint32_t flags, union wl_argument *args);
struct wl_proxy *wl_proxy_marshal_constructor(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, ...);
struct wl_proxy *wl_proxy_marshal_constructor_versioned(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, ...);
struct wl_proxy *wl_proxy_marshal_array_constructor(struct wl_proxy *proxy, uint32_t opcode, union wl_argument *args, const struct wl_interface *interface);
struct wl_proxy *wl_proxy_marshal_array_constructor_versioned(struct wl_proxy *proxy, uint32_t opcode, union wl_argument *args, const struct wl_interface *interface, uint32_t version);
int wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data);
const void *wl_proxy_get_listener(struct wl_proxy *proxy);
int wl_proxy_add_dispatcher(struct wl_proxy *proxy, wl_dispatcher_func_t dispatcher, const void *implementation, void *data);
void wl_proxy_set_user_data(struct wl_proxy *proxy, void *data);
void *wl_proxy_get_user_data(struct wl_proxy *proxy);
uint32_t wl_proxy_get_version(struct wl_proxy *proxy);
uint32_t wl_proxy_get_id(struct wl_proxy *proxy);
const char *wl_proxy_get_class(struct wl_proxy *proxy);
struct wl_display *wl_proxy_get_display(struct wl_proxy *proxy);
void wl_proxy_set_queue(struct wl_proxy *proxy, struct wl_event_queue *queue);
struct wl_event_queue *wl_proxy_get_queue(const struct wl_proxy *proxy);
void wl_proxy_set_tag(struct wl_proxy *proxy, const char *const *tag);
const char *const *wl_proxy_get_tag(struct wl_proxy *proxy);

#ifdef __cplusplus
}
#endif

#endif
