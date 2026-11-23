/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the viewporter protocol (wp_viewporter and
 * wp_viewport, version 1).  A surface's viewport crops its buffer and sets
 * the surface's size apart from the buffer's; neither interface has events.
 */

#include "internal.h"

#include "userland/desktop/libwayland/viewporter-client-protocol.h"

/*
 * Values referenced: wayland-protocols 1.44 stable/viewporter/viewporter.xml
 * (interface wp_viewport: version 1, requests destroy, set_source with four
 * fixed and set_destination with two int; no events).
 */

/* The argument types of every message whose arguments name no interface (at most 4). */
static const struct wl_interface *viewport_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The requests of wp_viewport, in wire opcode order. */
static const struct wl_message viewport_requests[] = {
	{ "destroy", "", NULL },
	{ "set_source", "ffff", viewport_plain_types },
	{ "set_destination", "ii", viewport_plain_types },
};

/* The immutable wp_viewport description. */
const struct wl_interface wp_viewport_interface = {
	"wp_viewport", 1, 3, viewport_requests,
	0, NULL
};

/*
 * Values referenced: wayland-protocols 1.44 stable/viewporter/viewporter.xml
 * (interface wp_viewporter: version 1, requests destroy and get_viewport
 * with a new wp_viewport and a wl_surface; no events).
 */

/* The arguments of wp_viewporter.get_viewport: the new viewport, and the surface. */
static const struct wl_interface *viewporter_get_types[] = {
	&wp_viewport_interface,
	&wl_surface_interface,
};

/* The requests of wp_viewporter, in wire opcode order. */
static const struct wl_message viewporter_requests[] = {
	{ "destroy", "", NULL },
	{ "get_viewport", "no", viewporter_get_types },
};

/* The immutable wp_viewporter description. */
const struct wl_interface wp_viewporter_interface = {
	"wp_viewporter", 1, 2, viewporter_requests,
	0, NULL
};

/*
 * Sends wp_viewporter.destroy: the viewports it gave stay good.
 */
void
wp_viewporter_destroy(
	struct wp_viewporter *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_VIEWPORTER_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends wp_viewporter.get_viewport and returns the surface's new viewport.
 *
 * The viewport has the viewporter's version.  A surface that already has
 * one is a protocol error the compositor reports.
 */
struct wp_viewport *
wp_viewporter_get_viewport(
	struct wp_viewporter *object,
	struct wl_surface *surface)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;
	uint32_t version;

	/* The new viewport's identity, then the surface. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)surface;

	/* The new viewport speaks the version the viewporter was bound with. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_VIEWPORTER_GET_VIEWPORT, &wp_viewport_interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new viewport. */
	return (struct wp_viewport *)created;
}

/*
 * Sends wp_viewport.destroy: the surface goes back to its buffer's crop and
 * size with its next commit.
 */
void
wp_viewport_destroy(
	struct wp_viewport *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_VIEWPORT_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends wp_viewport.set_source: the part of the buffer the surface shows,
 * from its next commit (all four -1 shows the whole buffer).
 */
void
wp_viewport_set_source(
	struct wp_viewport *object,
	wl_fixed_t x,
	wl_fixed_t y,
	wl_fixed_t width,
	wl_fixed_t height)
{
	union wl_argument arguments[4];

	/* The rectangle's corner, then its size, each in 256ths of a buffer pixel. */
	arguments[0].f = x;
	arguments[1].f = y;
	arguments[2].f = width;
	arguments[3].f = height;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_VIEWPORT_SET_SOURCE, NULL, 0, 0, arguments);
}

/*
 * Sends wp_viewport.set_destination: the surface's size, from its next
 * commit (-1 by -1 takes the size from the source or the buffer).
 */
void
wp_viewport_set_destination(
	struct wp_viewport *object,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[2];

	/* The width, then the height, in surface coordinates. */
	arguments[0].i = width;
	arguments[1].i = height;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_VIEWPORT_SET_DESTINATION, NULL, 0, 0, arguments);
}
