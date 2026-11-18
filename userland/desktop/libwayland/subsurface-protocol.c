/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the core wl_subcompositor and wl_subsurface
 * (version 1, WS035 p077).
 *
 * A sub-surface is a wl_surface placed relative to a parent surface and
 * shown with it: video, GL parts and client-side decorations of toolkits.
 * Neither interface has events.  The descriptions follow the pinned Wayland
 * 1.23.1 core protocol (userland/desktop/libwayland/API-PROVENANCE.md).
 */

#include "internal.h"

/* The arguments of wl_subcompositor.get_subsurface: the new sub-surface, its surface and its parent. */
static const struct wl_interface *subcompositor_get_types[] = {
	&wl_subsurface_interface,
	&wl_surface_interface,
	&wl_surface_interface,
};

/* The requests of wl_subcompositor, in wire opcode order. */
static const struct wl_message subcompositor_requests[] = {
	{ "destroy", "", NULL },
	{ "get_subsurface", "noo", subcompositor_get_types },
};

/* The immutable wl_subcompositor description. */
const struct wl_interface wl_subcompositor_interface = {
	"wl_subcompositor", 1, 2, subcompositor_requests,
	0, NULL
};

/* The arguments of wl_subsurface.set_position: two numbers. */
static const struct wl_interface *subsurface_position_types[] = {
	NULL,
	NULL,
};

/* The argument of wl_subsurface.place_above and place_below: a sibling or the parent. */
static const struct wl_interface *subsurface_place_types[] = {
	&wl_surface_interface,
};

/* The requests of wl_subsurface, in wire opcode order. */
static const struct wl_message subsurface_requests[] = {
	{ "destroy", "", NULL },
	{ "set_position", "ii", subsurface_position_types },
	{ "place_above", "o", subsurface_place_types },
	{ "place_below", "o", subsurface_place_types },
	{ "set_sync", "", NULL },
	{ "set_desync", "", NULL },
};

/* The immutable wl_subsurface description. */
const struct wl_interface wl_subsurface_interface = {
	"wl_subsurface", 1, 6, subsurface_requests,
	0, NULL
};

/*
 * Sends the wl_subcompositor.destroy request.
 */
void
wl_subcompositor_destroy(
	struct wl_subcompositor *object)
{
	/* Queues the request and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends the wl_subcompositor.get_subsurface request: gives a surface the
 * sub-surface role under a parent.
 */
struct wl_subsurface *
wl_subcompositor_get_subsurface(
	struct wl_subcompositor *object,
	struct wl_surface *surface,
	struct wl_surface *parent)
{
	union wl_argument arguments[3];
	struct wl_proxy *created;
	uint32_t version;

	/* The new object, the surface and its parent. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)surface;
	arguments[2].o = (struct wl_object *)parent;

	/* The sub-surface inherits the subcompositor's version. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Queues the request with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, &wl_subsurface_interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new sub-surface. */
	return (struct wl_subsurface *)created;
}

/*
 * Associates client state with the wl_subcompositor proxy.
 */
void
wl_subcompositor_set_user_data(
	struct wl_subcompositor *object,
	void *data)
{
	/* The proxy keeps the pointer. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);
}

/*
 * Obtains client state from the wl_subcompositor proxy.
 */
void *
wl_subcompositor_get_user_data(
	struct wl_subcompositor *object)
{
	void *answer;

	/* The pointer the proxy keeps. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: the client state. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_subcompositor proxy.
 */
uint32_t
wl_subcompositor_get_version(
	struct wl_subcompositor *object)
{
	uint32_t answer;

	/* The version bound. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: the version. */
	return answer;
}

/*
 * Sends the wl_subsurface.destroy request (the surface loses its role).
 */
void
wl_subsurface_destroy(
	struct wl_subsurface *object)
{
	/* Queues the request and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends the wl_subsurface.set_position request (applied on the parent's
 * next commit).
 */
void
wl_subsurface_set_position(
	struct wl_subsurface *object,
	int32_t x,
	int32_t y)
{
	union wl_argument arguments[2];

	/* The position in the parent's surface coordinates. */
	arguments[0].i = x;
	arguments[1].i = y;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, NULL, 0, 0, arguments);
}

/*
 * Sends the wl_subsurface.place_above request.
 */
void
wl_subsurface_place_above(
	struct wl_subsurface *object,
	struct wl_surface *sibling)
{
	union wl_argument arguments[1];

	/* The sibling or the parent the sub-surface goes right above. */
	arguments[0].o = (struct wl_object *)sibling;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, NULL, 0, 0, arguments);
}

/*
 * Sends the wl_subsurface.place_below request.
 */
void
wl_subsurface_place_below(
	struct wl_subsurface *object,
	struct wl_surface *sibling)
{
	union wl_argument arguments[1];

	/* The sibling or the parent the sub-surface goes right below. */
	arguments[0].o = (struct wl_object *)sibling;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 3U, NULL, 0, 0, arguments);
}

/*
 * Sends the wl_subsurface.set_sync request.
 */
void
wl_subsurface_set_sync(
	struct wl_subsurface *object)
{
	/* Queues the request, which has no arguments. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 4U, NULL, 0, 0, NULL);
}

/*
 * Sends the wl_subsurface.set_desync request.
 */
void
wl_subsurface_set_desync(
	struct wl_subsurface *object)
{
	/* Queues the request, which has no arguments. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 5U, NULL, 0, 0, NULL);
}

/*
 * Associates client state with the wl_subsurface proxy.
 */
void
wl_subsurface_set_user_data(
	struct wl_subsurface *object,
	void *data)
{
	/* The proxy keeps the pointer. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);
}

/*
 * Obtains client state from the wl_subsurface proxy.
 */
void *
wl_subsurface_get_user_data(
	struct wl_subsurface *object)
{
	void *answer;

	/* The pointer the proxy keeps. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: the client state. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_subsurface proxy.
 */
uint32_t
wl_subsurface_get_version(
	struct wl_subsurface *object)
{
	uint32_t answer;

	/* The version bound. */
	answer = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: the version. */
	return answer;
}
