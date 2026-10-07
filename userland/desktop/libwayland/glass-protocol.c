/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the compositor's glass protocol (ws035-p083).
 *
 * kl_glass_manager_v1 gives a surface its glass (kl_glass_v1): the list
 * of the surface's panels -- cards floating in the window -- under which
 * the compositor draws the system's frosted glass.  The protocol is the compositor's own; its header is private and
 * applications use it through libkeiland.  plan/ws035/glass-design.md
 * defines every request.  Version 2 adds kl_glass_v1.set_blur (ws075-p029).
 */

#include "internal.h"

/* The argument of kl_glass_v1.set_panels names no interface. */
static const struct wl_interface *glass_plain_types[] = {
	NULL,
};

/* The arguments of kl_glass_manager_v1.get_glass: the new glass, and the surface. */
static const struct wl_interface *glass_manager_get_types[] = {
	&kl_glass_v1_interface,
	&wl_surface_interface,
};

/* The argument of kl_glass_v1.set_blur (since version 2) names no interface. */
static const struct wl_interface *glass_blur_types[] = {
	NULL,
};

/* The requests of kl_glass_manager_v1, in wire order. */
static const struct wl_message glass_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_glass", "no", glass_manager_get_types },
};

/* Describes the global that gives surfaces their glass. */
const struct wl_interface kl_glass_manager_v1_interface = {
	"kl_glass_manager_v1", 2, 2, glass_manager_requests,
	0, NULL
};

/* The requests of kl_glass_v1, in wire order. */
static const struct wl_message glass_requests[] = {
	{ "destroy", "", NULL },
	{ "set_panels", "a", glass_plain_types },
	{ "set_blur", "2u", glass_blur_types },
};

/* Describes one surface's glass. */
const struct wl_interface kl_glass_v1_interface = {
	"kl_glass_v1", 2, 3, glass_requests,
	0, NULL
};

/*
 * Sends kl_glass_manager_v1.destroy; the glass it gave stays.
 */
void
kl_glass_manager_v1_destroy(
	struct kl_glass_manager_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GLASS_MANAGER_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_glass_manager_v1.get_glass and returns the surface's new glass.
 */
struct kl_glass_v1 *
kl_glass_manager_v1_get_glass(
	struct kl_glass_manager_v1 *object,
	struct wl_surface *surface)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The new glass's identity, then the surface it belongs to. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)surface;

	/* Queues the request together with the new proxy, of the manager's version (2 has set_blur). */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GLASS_MANAGER_V1_GET_GLASS, &kl_glass_v1_interface, wl_proxy_get_version((struct wl_proxy *)object), 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new glass. */
	return (struct kl_glass_v1 *)created;
}

/*
 * Sends kl_glass_v1.destroy: the surface's next commit shows it without
 * panels.
 */
void
kl_glass_v1_destroy(
	struct kl_glass_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GLASS_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_glass_v1.set_panels: the surface's panels for its next commit,
 * six words each (x, y, width, height, radius, kind).
 */
void
kl_glass_v1_set_panels(
	struct kl_glass_v1 *object,
	struct wl_array *panels)
{
	union wl_argument arguments[1];

	/* The list of panels. */
	arguments[0].a = panels;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GLASS_V1_SET_PANELS, NULL, 0, 0, arguments);
}

/*
 * Sends kl_glass_v1.set_blur (since version 2): whether the surface's
 * glass shows the windows under it blurred (1) or only the blurred wallpaper
 * (0, the default), from its next commit.
 */
void
kl_glass_v1_set_blur(
	struct kl_glass_v1 *object,
	uint32_t enabled)
{
	union wl_argument arguments[1];

	/* The choice. */
	arguments[0].u = enabled;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GLASS_V1_SET_BLUR, NULL, 0, 0, arguments);
}
