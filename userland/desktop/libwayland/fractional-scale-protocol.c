/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the fractional scale protocol
 * (wp_fractional_scale_manager_v1 and wp_fractional_scale_v1, version 1).
 * The compositor tells a surface the scale it would like it drawn at; the
 * client draws its buffer at that scale and gives the surface its size with
 * wp_viewport.  preferred_scale reaches its listener through the generic
 * dispatch (event.c).
 */

#include "internal.h"

#include "userland/desktop/libwayland/fractional-scale-v1-client-protocol.h"

/*
 * Values referenced: wayland-protocols 1.44
 * staging/fractional-scale/fractional-scale-v1.xml (interface
 * wp_fractional_scale_v1: version 1, requests destroy; event
 * preferred_scale with one uint).
 */

/* The argument types of every message whose arguments name no interface (at most 1). */
static const struct wl_interface *fractional_plain_types[] = {
	NULL,
};

/* The requests of wp_fractional_scale_v1, in wire opcode order. */
static const struct wl_message fractional_requests[] = {
	{ "destroy", "", NULL },
};

/* The events of wp_fractional_scale_v1, in wire opcode order. */
static const struct wl_message fractional_events[] = {
	{ "preferred_scale", "u", fractional_plain_types },
};

/* The immutable wp_fractional_scale_v1 description. */
const struct wl_interface wp_fractional_scale_v1_interface = {
	"wp_fractional_scale_v1", 1, 1, fractional_requests,
	1, fractional_events
};

/*
 * Values referenced: wayland-protocols 1.44
 * staging/fractional-scale/fractional-scale-v1.xml (interface
 * wp_fractional_scale_manager_v1: version 1, requests destroy and
 * get_fractional_scale with a new wp_fractional_scale_v1 and a wl_surface;
 * no events).
 */

/* The arguments of wp_fractional_scale_manager_v1.get_fractional_scale: the new object, and the surface. */
static const struct wl_interface *manager_get_types[] = {
	&wp_fractional_scale_v1_interface,
	&wl_surface_interface,
};

/* The requests of wp_fractional_scale_manager_v1, in wire opcode order. */
static const struct wl_message manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_fractional_scale", "no", manager_get_types },
};

/* The immutable wp_fractional_scale_manager_v1 description. */
const struct wl_interface wp_fractional_scale_manager_v1_interface = {
	"wp_fractional_scale_manager_v1", 1, 2, manager_requests,
	0, NULL
};

/*
 * Sends wp_fractional_scale_manager_v1.destroy: the objects it gave stay good.
 */
void
wp_fractional_scale_manager_v1_destroy(
	struct wp_fractional_scale_manager_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_FRACTIONAL_SCALE_MANAGER_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends wp_fractional_scale_manager_v1.get_fractional_scale and returns the
 * surface's new fractional scale object.
 *
 * The object has the manager's version.  A surface that already has one is
 * a protocol error the compositor reports.
 */
struct wp_fractional_scale_v1 *
wp_fractional_scale_manager_v1_get_fractional_scale(
	struct wp_fractional_scale_manager_v1 *object,
	struct wl_surface *surface)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;
	uint32_t version;

	/* The new object's identity, then the surface. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)surface;

	/* The new object speaks the version the manager was bound with. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_FRACTIONAL_SCALE_MANAGER_V1_GET_FRACTIONAL_SCALE, &wp_fractional_scale_v1_interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new object. */
	return (struct wp_fractional_scale_v1 *)created;
}

/*
 * Installs a listener of wp_fractional_scale_v1 (preferred_scale).
 */
int
wp_fractional_scale_v1_add_listener(
	struct wp_fractional_scale_v1 *object,
	const struct wp_fractional_scale_v1_listener *listener,
	void *data)
{
	int error;

	/* The callback receives the object's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends wp_fractional_scale_v1.destroy: no preferred_scale comes after it,
 * not even one already on its way.
 */
void
wp_fractional_scale_v1_destroy(
	struct wp_fractional_scale_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_FRACTIONAL_SCALE_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}
