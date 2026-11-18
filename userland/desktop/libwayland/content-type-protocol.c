/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the content type protocol
 * (wp_content_type_manager_v1 and wp_content_type_v1, version 1;
 * ws122-p005b).  A client tells what a surface shows; the compositor may
 * scan a fullscreen video or game out without composing it.
 */

#include "internal.h"

#include "userland/desktop/libwayland/content-type-v1-client-protocol.h"

/* The argument types of every message whose arguments name no interface (at most 1). */
static const struct wl_interface *content_plain_types[] = {
	NULL,
};

/* The requests of wp_content_type_v1, in wire opcode order. */
static const struct wl_message content_requests[] = {
	{ "destroy", "", NULL },
	{ "set_content_type", "u", content_plain_types },
};

/* The immutable wp_content_type_v1 description. */
const struct wl_interface wp_content_type_v1_interface = {
	"wp_content_type_v1", 1, 2, content_requests,
	0, NULL
};

/* The arguments of wp_content_type_manager_v1.get_surface_content_type: the new object, and the surface. */
static const struct wl_interface *manager_get_types[] = {
	&wp_content_type_v1_interface,
	&wl_surface_interface,
};

/* The requests of wp_content_type_manager_v1, in wire opcode order. */
static const struct wl_message manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_surface_content_type", "no", manager_get_types },
};

/* The immutable wp_content_type_manager_v1 description. */
const struct wl_interface wp_content_type_manager_v1_interface = {
	"wp_content_type_manager_v1", 1, 2, manager_requests,
	0, NULL
};

/*
 * Sends wp_content_type_manager_v1.destroy: the objects it gave stay good.
 */
void
wp_content_type_manager_v1_destroy(
	struct wp_content_type_manager_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_CONTENT_TYPE_MANAGER_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends wp_content_type_manager_v1.get_surface_content_type and returns the
 * surface's new content type object.
 */
struct wp_content_type_v1 *
wp_content_type_manager_v1_get_surface_content_type(
	struct wp_content_type_manager_v1 *object,
	struct wl_surface *surface)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The new object's identity, then the surface. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)surface;

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_CONTENT_TYPE_MANAGER_V1_GET_SURFACE_CONTENT_TYPE, &wp_content_type_v1_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new object. */
	return (struct wp_content_type_v1 *)created;
}

/*
 * Sends wp_content_type_v1.destroy: the surface's content type goes back to
 * none with its next commit.
 */
void
wp_content_type_v1_destroy(
	struct wp_content_type_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_CONTENT_TYPE_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends wp_content_type_v1.set_content_type: what the surface shows, from
 * its next commit.
 */
void
wp_content_type_v1_set_content_type(
	struct wp_content_type_v1 *object,
	uint32_t content_type)
{
	union wl_argument arguments[1];

	/* The type. */
	arguments[0].u = content_type;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, WP_CONTENT_TYPE_V1_SET_CONTENT_TYPE, NULL, 0, 0, arguments);
}
