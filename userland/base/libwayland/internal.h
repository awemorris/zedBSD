/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Defines display-locked ownership for the independent Wayland client.
 */

#ifndef ZEDBSD_WAYLAND_INTERNAL_H
#define ZEDBSD_WAYLAND_INTERNAL_H

#include <wayland/wayland-client.h>
#include <wayland/xdg-shell-client-protocol.h>
#include "userland/base/libwayland/zed-gpu-buffer-v1-client-protocol.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WLC_WIRE_MAX 65532U
#define WLC_FD_MAX 8U
#define WLC_SERVER_ID_START 0xff000000U

struct wlc_event;
struct wlc_packet;

/* Keeps one wire object alive through map, caller, wrapper and event references. */
struct wl_proxy {
	const struct wl_interface *interface;
	struct wl_display *display;
	struct wl_event_queue *queue;
	struct wl_proxy *next;
	struct wl_proxy *wrapped;
	void (**listener)(void);
	wl_dispatcher_func_t dispatcher;
	const void *dispatcher_data;
	void *user_data;
	const char *const *tag;
	uint32_t id;
	uint32_t version;
	unsigned references;
	int destroyed;
	int in_map;
};

/* Owns undispatched events independently of every other client event queue. */
struct wl_event_queue {
	struct wl_display *display;
	struct wl_event_queue *next;
	struct wlc_event *head;
	struct wlc_event *tail;
	char *name;
};

/* Retains serialized bytes and duplicated outgoing descriptors until sent. */
struct wlc_packet {
	struct wlc_packet *next;
	unsigned char *bytes;
	size_t length;
	size_t sent;
	int descriptors[WLC_FD_MAX];
	size_t descriptor_count;
};

/* Retains argument storage and object references until listener dispatch. */
struct wlc_event {
	struct wlc_event *next;
	struct wl_proxy *proxy;
	const struct wl_message *message;
	union wl_argument *arguments;
	struct wl_array *arrays;
	struct wl_proxy **objects;
	unsigned char *bytes;
	size_t argument_count;
	uint32_t opcode;
	int delivered;
};

/* Serializes connection state while callbacks and poll run outside its mutex. */
struct wl_display {
	struct wl_proxy proxy;
	pthread_mutex_t mutex;
	pthread_cond_t readers_changed;
	struct wl_event_queue default_queue;
	struct wl_event_queue *queues;
	struct wl_proxy *objects;
	struct wl_proxy *wrappers;
	struct wlc_packet *output_head;
	struct wlc_packet *output_tail;
	unsigned char *input;
	size_t input_size;
	int *input_descriptors;
	size_t input_descriptor_count;
	uint32_t next_id;
	unsigned prepared_readers;
	uint64_t read_generation;
	int read_error;
	int fd;
	int error;
	uint32_t protocol_code;
	uint32_t protocol_id;
	const struct wl_interface *protocol_interface;
};

struct wl_proxy *wlc_proxy_real(struct wl_proxy *proxy);
struct wl_proxy *wlc_proxy_lookup(struct wl_display *display, uint32_t id);
struct wl_proxy *wlc_proxy_allocate(struct wl_proxy *factory, const struct wl_interface *interface, uint32_t version);
void wlc_proxy_unref(struct wl_proxy *proxy);
void wlc_proxy_remove(struct wl_proxy *proxy);
void wlc_proxy_destroy(struct wl_proxy *proxy);
void wlc_display_error(struct wl_display *display, int error);
const char *wlc_signature_start(const char *signature, uint32_t *version);
const char *wlc_signature_next(const char *signature, char *type, int *nullable);
size_t wlc_signature_count(const char *signature);
int wlc_wire_queue(struct wl_proxy *proxy, uint32_t opcode, union wl_argument *arguments, struct wl_proxy *created);
int wlc_wire_flush(struct wl_display *display);
int wlc_wire_read(struct wl_display *display);
void wlc_packet_destroy(struct wlc_packet *packet);
void wlc_event_destroy(struct wlc_event *event);
int wlc_event_dispatch(struct wlc_event *event);

#endif
