/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the core data-sharing interfaces (version 3,
 * WS035 p079): wl_data_device_manager, wl_data_source, wl_data_device and
 * wl_data_offer, which carry the clipboard (and drag and drop) between
 * clients.
 *
 * Their events reach listeners through the generic dispatch (event.c);
 * wl_data_device.data_offer creates a server-made wl_data_offer.  The
 * descriptions follow the pinned Wayland 1.23.1 core protocol
 * (userland/desktop/libwayland/API-PROVENANCE.md).
 */

#include "internal.h"

/* The argument types of every message whose arguments name no interface (at most five). */
static const struct wl_interface *data_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The requests of wl_data_offer, in wire opcode order. */
static const struct wl_message data_offer_requests[] = {
	{ "accept", "u?s", data_plain_types },
	{ "receive", "sh", data_plain_types },
	{ "destroy", "", NULL },
	{ "finish", "3", NULL },
	{ "set_actions", "3uu", data_plain_types },
};

/* The events of wl_data_offer, in wire opcode order. */
static const struct wl_message data_offer_events[] = {
	{ "offer", "s", data_plain_types },
	{ "source_actions", "3u", data_plain_types },
	{ "action", "3u", data_plain_types },
};

/* The immutable wl_data_offer description. */
const struct wl_interface wl_data_offer_interface = {
	"wl_data_offer", 3, 5, data_offer_requests,
	3, data_offer_events
};

/* The requests of wl_data_source, in wire opcode order. */
static const struct wl_message data_source_requests[] = {
	{ "offer", "s", data_plain_types },
	{ "destroy", "", NULL },
	{ "set_actions", "3u", data_plain_types },
};

/* The events of wl_data_source, in wire opcode order. */
static const struct wl_message data_source_events[] = {
	{ "target", "?s", data_plain_types },
	{ "send", "sh", data_plain_types },
	{ "cancelled", "", NULL },
	{ "dnd_drop_performed", "3", NULL },
	{ "dnd_finished", "3", NULL },
	{ "action", "3u", data_plain_types },
};

/* The immutable wl_data_source description. */
const struct wl_interface wl_data_source_interface = {
	"wl_data_source", 3, 3, data_source_requests,
	6, data_source_events
};

/* The arguments of wl_data_device.start_drag: the source, the origin surface, the icon surface, a serial. */
static const struct wl_interface *data_device_drag_types[] = {
	&wl_data_source_interface,
	&wl_surface_interface,
	&wl_surface_interface,
	NULL,
};

/* The arguments of wl_data_device.set_selection: the source and a serial. */
static const struct wl_interface *data_device_selection_types[] = {
	&wl_data_source_interface,
	NULL,
};

/* The requests of wl_data_device, in wire opcode order. */
static const struct wl_message data_device_requests[] = {
	{ "start_drag", "?oo?ou", data_device_drag_types },
	{ "set_selection", "?ou", data_device_selection_types },
	{ "release", "2", NULL },
};

/* The argument of wl_data_device.data_offer: the new offer the server made. */
static const struct wl_interface *data_device_offer_types[] = {
	&wl_data_offer_interface,
};

/* The arguments of wl_data_device.enter: a serial, the surface, the place, the offer. */
static const struct wl_interface *data_device_enter_types[] = {
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
	&wl_data_offer_interface,
};

/* The argument of wl_data_device.selection: the offer, or none. */
static const struct wl_interface *data_device_selection_event_types[] = {
	&wl_data_offer_interface,
};

/* The events of wl_data_device, in wire opcode order. */
static const struct wl_message data_device_events[] = {
	{ "data_offer", "n", data_device_offer_types },
	{ "enter", "uoff?o", data_device_enter_types },
	{ "leave", "", NULL },
	{ "motion", "uff", data_plain_types },
	{ "drop", "", NULL },
	{ "selection", "?o", data_device_selection_event_types },
};

/* The immutable wl_data_device description. */
const struct wl_interface wl_data_device_interface = {
	"wl_data_device", 3, 3, data_device_requests,
	6, data_device_events
};

/* The argument of wl_data_device_manager.create_data_source: the new source. */
static const struct wl_interface *data_manager_source_types[] = {
	&wl_data_source_interface,
};

/* The arguments of wl_data_device_manager.get_data_device: the new device and the seat. */
static const struct wl_interface *data_manager_device_types[] = {
	&wl_data_device_interface,
	&wl_seat_interface,
};

/* The requests of wl_data_device_manager, in wire opcode order. */
static const struct wl_message data_manager_requests[] = {
	{ "create_data_source", "n", data_manager_source_types },
	{ "get_data_device", "no", data_manager_device_types },
};

/* The immutable wl_data_device_manager description. */
const struct wl_interface wl_data_device_manager_interface = {
	"wl_data_device_manager", 3, 2, data_manager_requests,
	0, NULL
};

/*
 * Sends wl_data_device_manager.create_data_source: a new source the client
 * offers data from.
 */
struct wl_data_source *
wl_data_device_manager_create_data_source(
	struct wl_data_device_manager *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;
	uint32_t version;

	/* The new object, at the manager's version. */
	arguments[0].n = 0;
	version = wl_proxy_get_version((struct wl_proxy *)object);
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, &wl_data_source_interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the source. */
	return (struct wl_data_source *)created;
}

/*
 * Sends wl_data_device_manager.get_data_device: the seat's data device for
 * this client.
 */
struct wl_data_device *
wl_data_device_manager_get_data_device(
	struct wl_data_device_manager *object,
	struct wl_seat *seat)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;
	uint32_t version;

	/* The new object and the seat, at the manager's version. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)seat;
	version = wl_proxy_get_version((struct wl_proxy *)object);
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, &wl_data_device_interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the device. */
	return (struct wl_data_device *)created;
}

/*
 * Destroys the wl_data_device_manager proxy (the interface has no destroy
 * request).
 */
void
wl_data_device_manager_destroy(
	struct wl_data_device_manager *object)
{
	/* Only the proxy goes. */
	wl_proxy_destroy((struct wl_proxy *)object);
}

/*
 * Reports the version of the wl_data_device_manager proxy.
 */
uint32_t
wl_data_device_manager_get_version(
	struct wl_data_device_manager *object)
{
	uint32_t version;

	/* The version the manager was bound with. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the bound version. */
	return version;
}

/*
 * Sends wl_data_source.offer: one more MIME type the source has.
 */
void
wl_data_source_offer(
	struct wl_data_source *object,
	const char *mime_type)
{
	union wl_argument arguments[1];

	/* The type. */
	arguments[0].s = mime_type;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, 0, arguments);
}

/*
 * Sends wl_data_source.destroy.
 */
void
wl_data_source_destroy(
	struct wl_data_source *object)
{
	/* Queues the request and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends wl_data_source.set_actions (version 3): the drag and drop actions
 * the source allows.
 */
void
wl_data_source_set_actions(
	struct wl_data_source *object,
	uint32_t dnd_actions)
{
	union wl_argument arguments[1];

	/* The actions. */
	arguments[0].u = dnd_actions;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, NULL, 0, 0, arguments);
}

/*
 * Installs the listener for wl_data_source events.
 */
int
wl_data_source_add_listener(
	struct wl_data_source *object,
	const struct wl_data_source_listener *listener,
	void *data)
{
	int error;

	/* The listener's callbacks, called by the generic dispatch. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the events go to the listener. */
	return 0;
}

/*
 * Sends wl_data_device.start_drag.
 */
void
wl_data_device_start_drag(
	struct wl_data_device *object,
	struct wl_data_source *source,
	struct wl_surface *origin,
	struct wl_surface *icon,
	uint32_t serial)
{
	union wl_argument arguments[4];

	/* The source, the origin, the icon and the serial of the press. */
	arguments[0].o = (struct wl_object *)source;
	arguments[1].o = (struct wl_object *)origin;
	arguments[2].o = (struct wl_object *)icon;
	arguments[3].u = serial;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, 0, arguments);
}

/*
 * Sends wl_data_device.set_selection: the source is the clipboard's (none
 * empties it).
 */
void
wl_data_device_set_selection(
	struct wl_data_device *object,
	struct wl_data_source *source,
	uint32_t serial)
{
	union wl_argument arguments[2];

	/* The source and the serial of the input that asked. */
	arguments[0].o = (struct wl_object *)source;
	arguments[1].u = serial;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, NULL, 0, 0, arguments);
}

/*
 * Sends wl_data_device.release (version 2).
 */
void
wl_data_device_release(
	struct wl_data_device *object)
{
	/* Queues the request and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Destroys the wl_data_device proxy without a request (a version 1 device).
 */
void
wl_data_device_destroy(
	struct wl_data_device *object)
{
	/* Only the proxy goes. */
	wl_proxy_destroy((struct wl_proxy *)object);
}

/*
 * Installs the listener for wl_data_device events.
 */
int
wl_data_device_add_listener(
	struct wl_data_device *object,
	const struct wl_data_device_listener *listener,
	void *data)
{
	int error;

	/* The listener's callbacks, called by the generic dispatch. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the events go to the listener. */
	return 0;
}

/*
 * Sends wl_data_offer.accept (drag and drop).
 */
void
wl_data_offer_accept(
	struct wl_data_offer *object,
	uint32_t serial,
	const char *mime_type)
{
	union wl_argument arguments[2];

	/* The serial and the type accepted (none refuses). */
	arguments[0].u = serial;
	arguments[1].s = mime_type;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 0U, NULL, 0, 0, arguments);
}

/*
 * Sends wl_data_offer.receive: the source writes the data in a type to the
 * descriptor (the caller closes its copy).
 */
void
wl_data_offer_receive(
	struct wl_data_offer *object,
	const char *mime_type,
	int32_t fd)
{
	union wl_argument arguments[2];

	/* The type and the descriptor the data is written to. */
	arguments[0].s = mime_type;
	arguments[1].h = fd;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 1U, NULL, 0, 0, arguments);
}

/*
 * Sends wl_data_offer.destroy.
 */
void
wl_data_offer_destroy(
	struct wl_data_offer *object)
{
	/* Queues the request and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 2U, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends wl_data_offer.finish (version 3, drag and drop).
 */
void
wl_data_offer_finish(
	struct wl_data_offer *object)
{
	/* Queues the request, which has no arguments. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 3U, NULL, 0, 0, NULL);
}

/*
 * Sends wl_data_offer.set_actions (version 3, drag and drop).
 */
void
wl_data_offer_set_actions(
	struct wl_data_offer *object,
	uint32_t dnd_actions,
	uint32_t preferred_action)
{
	union wl_argument arguments[2];

	/* The actions and the one preferred. */
	arguments[0].u = dnd_actions;
	arguments[1].u = preferred_action;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, 4U, NULL, 0, 0, arguments);
}

/*
 * Installs the listener for wl_data_offer events.
 */
int
wl_data_offer_add_listener(
	struct wl_data_offer *object,
	const struct wl_data_offer_listener *listener,
	void *data)
{
	int error;

	/* The listener's callbacks, called by the generic dispatch. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the events go to the listener. */
	return 0;
}
