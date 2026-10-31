/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the compositor's game protocol (kl_game_v1 and
 * kl_game_surface_v1, version 1): the images a compositor lends a
 * fullscreen surface in its game mode, which the client's Vulkan WSI draws
 * straight into.
 */

#include "internal.h"

#include "userland/desktop/libwayland/keiland-game-v1-client-protocol.h"

/* The argument types of every message whose arguments name no interface (at most 7). */
static const struct wl_interface *game_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The arguments of kl_game_surface_v1.tag: the buffer, then the lend's serial and the image's index. */
static const struct wl_interface *surface_tag_types[] = {
	&wl_buffer_interface,
	NULL,
	NULL,
};

/* The requests of kl_game_surface_v1, in wire opcode order. */
static const struct wl_message surface_requests[] = {
	{ "destroy", "", NULL },
	{ "chain", "uuuuu", game_plain_types },
	{ "tag", "ouu", surface_tag_types },
	{ "decline", "u", game_plain_types },
};

/* The events of kl_game_surface_v1, in wire opcode order. */
static const struct wl_message surface_events[] = {
	{ "image", "uuha", game_plain_types },
	{ "lend", "uuuuuuu", game_plain_types },
	{ "withdraw", "u", game_plain_types },
};

/* The immutable kl_game_surface_v1 description. */
const struct wl_interface kl_game_surface_v1_interface = {
	"kl_game_surface_v1", 1, 4, surface_requests,
	3, surface_events
};

/* The arguments of kl_game_v1.get_game_surface: the new object, and the surface. */
static const struct wl_interface *game_get_types[] = {
	&kl_game_surface_v1_interface,
	&wl_surface_interface,
};

/* The requests of kl_game_v1, in wire opcode order. */
static const struct wl_message game_requests[] = {
	{ "destroy", "", NULL },
	{ "get_game_surface", "no", game_get_types },
};

/* The immutable kl_game_v1 description. */
const struct wl_interface kl_game_v1_interface = {
	"kl_game_v1", 1, 2, game_requests,
	0, NULL
};

/*
 * Sends kl_game_v1.destroy: the surface objects it made stay good.
 */
void
kl_game_v1_destroy(
	struct kl_game_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GAME_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_game_v1.get_game_surface and returns the surface's new game
 * object.
 */
struct kl_game_surface_v1 *
kl_game_v1_get_game_surface(
	struct kl_game_v1 *object,
	struct wl_surface *surface)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The new object's identity, then the surface. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)surface;

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GAME_V1_GET_GAME_SURFACE, &kl_game_surface_v1_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new object. */
	return (struct kl_game_surface_v1 *)created;
}

/*
 * Installs a listener of kl_game_surface_v1 (image, lend, withdraw).
 */
int
kl_game_surface_v1_add_listener(
	struct kl_game_surface_v1 *object,
	const struct kl_game_surface_v1_listener *listener,
	void *data)
{
	int error;

	/* The callbacks receive the object's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends kl_game_surface_v1.destroy: no more lends for the surface.
 */
void
kl_game_surface_v1_destroy(
	struct kl_game_surface_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GAME_SURFACE_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_game_surface_v1.chain: the chain the client draws the surface
 * with -- its format, uses, size and image count (a count of zero: it
 * cannot draw into lent images).
 */
void
kl_game_surface_v1_chain(
	struct kl_game_surface_v1 *object,
	uint32_t format,
	uint32_t usage,
	uint32_t width,
	uint32_t height,
	uint32_t count)
{
	union wl_argument arguments[5];

	/* The chain's description, word by word. */
	arguments[0].u = format;
	arguments[1].u = usage;
	arguments[2].u = width;
	arguments[3].u = height;
	arguments[4].u = count;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GAME_SURFACE_V1_CHAIN, NULL, 0, 0, arguments);
}

/*
 * Sends kl_game_surface_v1.tag: the buffer holds lent image index of the
 * lend serial.
 */
void
kl_game_surface_v1_tag(
	struct kl_game_surface_v1 *object,
	struct wl_buffer *buffer,
	uint32_t serial,
	uint32_t index)
{
	union wl_argument arguments[3];

	/* The buffer, the lend and the image. */
	arguments[0].o = (struct wl_object *)buffer;
	arguments[1].u = serial;
	arguments[2].u = index;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GAME_SURFACE_V1_TAG, NULL, 0, 0, arguments);
}

/*
 * Sends kl_game_surface_v1.decline: the client will not draw into the
 * images of the lend serial.
 */
void
kl_game_surface_v1_decline(
	struct kl_game_surface_v1 *object,
	uint32_t serial)
{
	union wl_argument arguments[1];

	/* The lend. */
	arguments[0].u = serial;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_GAME_SURFACE_V1_DECLINE, NULL, 0, 0, arguments);
}
