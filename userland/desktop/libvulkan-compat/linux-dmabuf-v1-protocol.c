/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The private DMA-BUF protocol, authored in the tree's explicit wire-table form. */
#include "linux-dmabuf-v1-client-protocol.h"

/* The parameter constructor's newly allocated protocol object. */
static const struct wl_interface *dmabuf_params_types[] = {
    &zwp_linux_buffer_params_v1_interface};

/* The immediate buffer constructor's buffer identity and plain size/format arguments. */
static const struct wl_interface *params_buffer_types[] = {
    &wl_buffer_interface, NULL, NULL, NULL, NULL};

/* The asynchronous created event's newly allocated buffer. */
static const struct wl_interface *params_created_types[] = {
    &wl_buffer_interface};

/* Requests understood by a version-three DMA-BUF global in opcode order. */
static const struct wl_message dmabuf_requests[] = {
    {"destroy", "", NULL},
    {"create_params", "n", dmabuf_params_types}};

/* Formats are retained only from modifier events, available since version three. */
static const struct wl_message dmabuf_events[] = {
    {"format", "u", NULL},
    {"modifier", "3uuu", NULL}};

/* The registry binds precisely version three, with neither feedback request exposed. */
const struct wl_interface zwp_linux_dmabuf_v1_interface = {
    "zwp_linux_dmabuf_v1", 3, 2, dmabuf_requests, 2, dmabuf_events};

/* Requests understood by a version-three one-image parameter object. */
static const struct wl_message params_requests[] = {
    {"destroy", "", NULL},
    {"add", "huuuuu", NULL},
    {"create", "iiuu", NULL},
    {"create_immed", "2niiuu", params_buffer_types}};

/* Immediate construction uses no created event but preserves the interface's wire layout. */
static const struct wl_message params_events[] = {
    {"created", "n", params_created_types},
    {"failed", "", NULL}};

/* One image's planes, retired immediately after constructing its wl_buffer. */
const struct wl_interface zwp_linux_buffer_params_v1_interface = {
    "zwp_linux_buffer_params_v1", 3, 4, params_requests, 2, params_events};

/*
 * Sends the global destructor and retires its proxy.
 */
void
compat_dmabuf_destroy(
	struct zwp_linux_dmabuf_v1 *object)
{
	/* Queues the wire destructor before removing the local object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the local factory proxy has retired. */
	return;
}

/*
 * Allocates the protocol object describing one image's planes.
 */
struct zwp_linux_buffer_params_v1 *
compat_dmabuf_params(
	struct zwp_linux_dmabuf_v1 *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* Reserves the new object's identity on the same private event queue. */
	arguments[0].n = 0;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1, &zwp_linux_buffer_params_v1_interface, 3, 0, arguments);

	/* Returns the caller-owned parameter proxy, or the marshaller's allocation failure. */
	return (struct zwp_linux_buffer_params_v1 *)created;
}

/*
 * Retires the plane parameter object after buffer creation.
 */
void
compat_params_destroy(
	struct zwp_linux_buffer_params_v1 *object)
{
	/* Queues the wire destructor before removing the local object. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);

	/* Succeeded: the local parameter proxy has retired. */
	return;
}

/*
 * Sends the exported file descriptor for the single supported image plane.
 */
void
compat_params_add(
	struct zwp_linux_buffer_params_v1 *object,
	int fd,
	uint32_t offset,
	uint32_t stride,
	uint64_t modifier)
{
	union wl_argument arguments[6];

	/* The marshaller duplicates the descriptor for transmission; the caller closes its copy. */
	arguments[0].h = fd;
	arguments[1].u = 0;
	arguments[2].u = offset;
	arguments[3].u = stride;
	arguments[4].u = (uint32_t)(modifier >> 32);
	arguments[5].u = (uint32_t)modifier;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1, NULL, 0, 0, arguments);

	/* Succeeded: the requested plane description has been sent. */
	return;
}

/*
 * Creates the wl_buffer immediately, as required by the version-three WSI path.
 */
struct wl_buffer *
compat_params_buffer(
	struct zwp_linux_buffer_params_v1 *object,
	int width,
	int height,
	uint32_t format)
{
	union wl_argument arguments[5];
	struct wl_proxy *created;

	/* Describes the new buffer's identity, dimensions, pixel ordering and zero flags. */
	arguments[0].n = 0;
	arguments[1].i = width;
	arguments[2].i = height;
	arguments[3].u = format;
	arguments[4].u = 0;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 3, &wl_buffer_interface, 1, 0, arguments);

	/* Returns the caller-owned buffer proxy, or the marshaller's allocation failure. */
	return (struct wl_buffer *)created;
}
