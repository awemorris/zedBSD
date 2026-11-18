/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the primary selection protocol
 * (wp_primary_selection_unstable_v1, version 1; ws035-p100):
 * zwp_primary_selection_device_manager_v1, zwp_primary_selection_device_v1,
 * zwp_primary_selection_offer_v1 and zwp_primary_selection_source_v1, which
 * carry the selection a middle click pastes.
 *
 * Their events reach listeners through the generic dispatch (event.c);
 * zwp_primary_selection_device_v1.data_offer creates a server-made offer.
 * The descriptions follow the pinned wayland-protocols description
 * (userland/desktop/libwayland/API-PROVENANCE.md).
 */

#include "internal.h"

#include <wayland/primary-selection-unstable-v1-client-protocol.h>

/* The argument types of every message whose arguments name no interface (at most two). */
static const struct wl_interface *primary_plain_types[] = {
	NULL,
	NULL,
};

/* The requests of zwp_primary_selection_offer_v1, in wire opcode order. */
static const struct wl_message primary_offer_requests[] = {
	{ "receive", "sh", primary_plain_types },
	{ "destroy", "", NULL },
};

/* The events of zwp_primary_selection_offer_v1, in wire opcode order. */
static const struct wl_message primary_offer_events[] = {
	{ "offer", "s", primary_plain_types },
};

/* The immutable zwp_primary_selection_offer_v1 description. */
const struct wl_interface zwp_primary_selection_offer_v1_interface = {
	"zwp_primary_selection_offer_v1", 1, 2, primary_offer_requests,
	1, primary_offer_events
};

/* The requests of zwp_primary_selection_source_v1, in wire opcode order. */
static const struct wl_message primary_source_requests[] = {
	{ "offer", "s", primary_plain_types },
	{ "destroy", "", NULL },
};

/* The events of zwp_primary_selection_source_v1, in wire opcode order. */
static const struct wl_message primary_source_events[] = {
	{ "send", "sh", primary_plain_types },
	{ "cancelled", "", NULL },
};

/* The immutable zwp_primary_selection_source_v1 description. */
const struct wl_interface zwp_primary_selection_source_v1_interface = {
	"zwp_primary_selection_source_v1", 1, 2, primary_source_requests,
	2, primary_source_events
};

/* The arguments of zwp_primary_selection_device_v1.set_selection: the source and a serial. */
static const struct wl_interface *primary_device_selection_types[] = {
	&zwp_primary_selection_source_v1_interface,
	NULL,
};

/* The requests of zwp_primary_selection_device_v1, in wire opcode order. */
static const struct wl_message primary_device_requests[] = {
	{ "set_selection", "?ou", primary_device_selection_types },
	{ "destroy", "", NULL },
};

/* The argument of the device's data_offer and selection events: the offer. */
static const struct wl_interface *primary_device_offer_types[] = {
	&zwp_primary_selection_offer_v1_interface,
};

/* The events of zwp_primary_selection_device_v1, in wire opcode order. */
static const struct wl_message primary_device_events[] = {
	{ "data_offer", "n", primary_device_offer_types },
	{ "selection", "?o", primary_device_offer_types },
};

/* The immutable zwp_primary_selection_device_v1 description. */
const struct wl_interface zwp_primary_selection_device_v1_interface = {
	"zwp_primary_selection_device_v1", 1, 2, primary_device_requests,
	2, primary_device_events
};

/* The argument of the manager's create_source: the new source. */
static const struct wl_interface *primary_manager_source_types[] = {
	&zwp_primary_selection_source_v1_interface,
};

/* The arguments of the manager's get_device: the new device and the seat. */
static const struct wl_interface *primary_manager_device_types[] = {
	&zwp_primary_selection_device_v1_interface,
	&wl_seat_interface,
};

/* The requests of zwp_primary_selection_device_manager_v1, in wire opcode order. */
static const struct wl_message primary_manager_requests[] = {
	{ "create_source", "n", primary_manager_source_types },
	{ "get_device", "no", primary_manager_device_types },
	{ "destroy", "", NULL },
};

/* The immutable zwp_primary_selection_device_manager_v1 description. */
const struct wl_interface zwp_primary_selection_device_manager_v1_interface = {
	"zwp_primary_selection_device_manager_v1", 1, 3, primary_manager_requests,
	0, NULL
};

/*
 * Sends the manager's create_source: a new source the client offers the
 * primary selection from.
 */
struct zwp_primary_selection_source_v1 *
zwp_primary_selection_device_manager_v1_create_source(
	struct zwp_primary_selection_device_manager_v1 *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* The new object. */
	arguments[0].n = 0;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_PRIMARY_SELECTION_DEVICE_MANAGER_V1_CREATE_SOURCE, &zwp_primary_selection_source_v1_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the source. */
	return (struct zwp_primary_selection_source_v1 *)created;
}

/*
 * Sends the manager's get_device: the seat's primary selection device for
 * this client.
 */
struct zwp_primary_selection_device_v1 *
zwp_primary_selection_device_manager_v1_get_device(
	struct zwp_primary_selection_device_manager_v1 *object,
	struct wl_seat *seat)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The new object and the seat. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)seat;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_PRIMARY_SELECTION_DEVICE_MANAGER_V1_GET_DEVICE, &zwp_primary_selection_device_v1_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the device. */
	return (struct zwp_primary_selection_device_v1 *)created;
}

/*
 * Sends the manager's destroy; what it made stays.
 */
void
zwp_primary_selection_device_manager_v1_destroy(
	struct zwp_primary_selection_device_manager_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_PRIMARY_SELECTION_DEVICE_MANAGER_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Installs a device's listener (data_offer, selection).
 */
int
zwp_primary_selection_device_v1_add_listener(
	struct zwp_primary_selection_device_v1 *object,
	const struct zwp_primary_selection_device_v1_listener *listener,
	void *data)
{
	int error;

	/* The callbacks receive the device's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends the device's set_selection: a source of the client's, or none, with
 * the serial of the input that made the selection.
 */
void
zwp_primary_selection_device_v1_set_selection(
	struct zwp_primary_selection_device_v1 *object,
	struct zwp_primary_selection_source_v1 *source,
	uint32_t serial)
{
	union wl_argument arguments[2];

	/* The source and the serial. */
	arguments[0].o = (struct wl_object *)source;
	arguments[1].u = serial;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_PRIMARY_SELECTION_DEVICE_V1_SET_SELECTION, NULL, 0, 0, arguments);
}

/*
 * Sends the device's destroy.
 */
void
zwp_primary_selection_device_v1_destroy(
	struct zwp_primary_selection_device_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_PRIMARY_SELECTION_DEVICE_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Installs an offer's listener (its types).
 */
int
zwp_primary_selection_offer_v1_add_listener(
	struct zwp_primary_selection_offer_v1 *object,
	const struct zwp_primary_selection_offer_v1_listener *listener,
	void *data)
{
	int error;

	/* The callbacks receive the offer's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends the offer's receive: the source's client writes the type into the
 * descriptor and closes it.
 */
void
zwp_primary_selection_offer_v1_receive(
	struct zwp_primary_selection_offer_v1 *object,
	const char *mime_type,
	int32_t fd)
{
	union wl_argument arguments[2];

	/* The type and the descriptor. */
	arguments[0].s = mime_type;
	arguments[1].h = fd;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_PRIMARY_SELECTION_OFFER_V1_RECEIVE, NULL, 0, 0, arguments);
}

/*
 * Sends the offer's destroy.
 */
void
zwp_primary_selection_offer_v1_destroy(
	struct zwp_primary_selection_offer_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_PRIMARY_SELECTION_OFFER_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Installs a source's listener (send, cancelled).
 */
int
zwp_primary_selection_source_v1_add_listener(
	struct zwp_primary_selection_source_v1 *object,
	const struct zwp_primary_selection_source_v1_listener *listener,
	void *data)
{
	int error;

	/* The callbacks receive the source's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends the source's offer: one more MIME type the source has.
 */
void
zwp_primary_selection_source_v1_offer(
	struct zwp_primary_selection_source_v1 *object,
	const char *mime_type)
{
	union wl_argument arguments[1];

	/* The type. */
	arguments[0].s = mime_type;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_PRIMARY_SELECTION_SOURCE_V1_OFFER, NULL, 0, 0, arguments);
}

/*
 * Sends the source's destroy.
 */
void
zwp_primary_selection_source_v1_destroy(
	struct zwp_primary_selection_source_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_PRIMARY_SELECTION_SOURCE_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}
