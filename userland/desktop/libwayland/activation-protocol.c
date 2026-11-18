/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the activation protocol (xdg_activation_v1 and
 * xdg_activation_token_v1, version 1; ws089-p016).  A client asks for a
 * token, hands it to another client, and that client brings one of its
 * surfaces to the front with it.
 */

#include "internal.h"

#include "userland/desktop/libwayland/xdg-activation-v1-client-protocol.h"

/* The argument types of every message whose arguments name no interface (at most 1). */
static const struct wl_interface *activation_plain_types[] = {
	NULL,
};

/* The arguments of xdg_activation_token_v1.set_serial: the serial, and the seat. */
static const struct wl_interface *token_serial_types[] = {
	NULL,
	&wl_seat_interface,
};

/* The argument of xdg_activation_token_v1.set_surface: the surface. */
static const struct wl_interface *token_surface_types[] = {
	&wl_surface_interface,
};

/* The requests of xdg_activation_token_v1, in wire opcode order. */
static const struct wl_message token_requests[] = {
	{ "set_serial", "uo", token_serial_types },
	{ "set_app_id", "s", activation_plain_types },
	{ "set_surface", "o", token_surface_types },
	{ "commit", "", NULL },
	{ "destroy", "", NULL },
};

/* The events of xdg_activation_token_v1, in wire opcode order. */
static const struct wl_message token_events[] = {
	{ "done", "s", activation_plain_types },
};

/* The immutable xdg_activation_token_v1 description. */
const struct wl_interface xdg_activation_token_v1_interface = {
	"xdg_activation_token_v1", 1, 5, token_requests,
	1, token_events
};

/* The argument of xdg_activation_v1.get_activation_token: the new token. */
static const struct wl_interface *activation_token_types[] = {
	&xdg_activation_token_v1_interface,
};

/* The arguments of xdg_activation_v1.activate: the token's text, and the surface. */
static const struct wl_interface *activation_activate_types[] = {
	NULL,
	&wl_surface_interface,
};

/* The requests of xdg_activation_v1, in wire opcode order. */
static const struct wl_message activation_requests[] = {
	{ "destroy", "", NULL },
	{ "get_activation_token", "n", activation_token_types },
	{ "activate", "so", activation_activate_types },
};

/* The immutable xdg_activation_v1 description. */
const struct wl_interface xdg_activation_v1_interface = {
	"xdg_activation_v1", 1, 3, activation_requests,
	0, NULL
};

/*
 * Sends xdg_activation_v1.destroy: the tokens it gave stay good.
 */
void
xdg_activation_v1_destroy(
	struct xdg_activation_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_ACTIVATION_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends xdg_activation_v1.get_activation_token and returns the new token object.
 */
struct xdg_activation_token_v1 *
xdg_activation_v1_get_activation_token(
	struct xdg_activation_v1 *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* The new token object's identity. */
	arguments[0].n = 0;

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_ACTIVATION_V1_GET_ACTIVATION_TOKEN, &xdg_activation_token_v1_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new token object. */
	return (struct xdg_activation_token_v1 *)created;
}

/*
 * Sends xdg_activation_v1.activate: the surface comes to the front when the token allows it.
 */
void
xdg_activation_v1_activate(
	struct xdg_activation_v1 *object,
	const char *token,
	struct wl_surface *surface)
{
	union wl_argument arguments[2];

	/* The token's text, then the surface. */
	arguments[0].s = token;
	arguments[1].o = (struct wl_object *)surface;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_ACTIVATION_V1_ACTIVATE, NULL, 0, 0, arguments);
}

/*
 * Installs a listener of xdg_activation_token_v1 (done).
 */
int
xdg_activation_token_v1_add_listener(
	struct xdg_activation_token_v1 *object,
	const struct xdg_activation_token_v1_listener *listener,
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
 * Sends xdg_activation_token_v1.set_serial: the input event the request answers.
 */
void
xdg_activation_token_v1_set_serial(
	struct xdg_activation_token_v1 *object,
	uint32_t serial,
	struct wl_seat *seat)
{
	union wl_argument arguments[2];

	/* The serial, then the seat. */
	arguments[0].u = serial;
	arguments[1].o = (struct wl_object *)seat;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_ACTIVATION_TOKEN_V1_SET_SERIAL, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_activation_token_v1.set_app_id: the application the token is for.
 */
void
xdg_activation_token_v1_set_app_id(
	struct xdg_activation_token_v1 *object,
	const char *app_id)
{
	union wl_argument arguments[1];

	/* The application's ID. */
	arguments[0].s = app_id;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_ACTIVATION_TOKEN_V1_SET_APP_ID, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_activation_token_v1.set_surface: the surface the request comes from.
 */
void
xdg_activation_token_v1_set_surface(
	struct xdg_activation_token_v1 *object,
	struct wl_surface *surface)
{
	union wl_argument arguments[1];

	/* The surface. */
	arguments[0].o = (struct wl_object *)surface;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_ACTIVATION_TOKEN_V1_SET_SURFACE, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_activation_token_v1.commit: the token is asked for, and done brings it.
 */
void
xdg_activation_token_v1_commit(
	struct xdg_activation_token_v1 *object)
{
	/* No arguments. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_ACTIVATION_TOKEN_V1_COMMIT, NULL, 0, 0, NULL);
}

/*
 * Sends xdg_activation_token_v1.destroy: the token it brought stays good.
 */
void
xdg_activation_token_v1_destroy(
	struct xdg_activation_token_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_ACTIVATION_TOKEN_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}
