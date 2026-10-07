/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the tablet protocol (tablet_unstable_v2, version 1;
 * WS079 p003): zwp_tablet_manager_v2, zwp_tablet_seat_v2, zwp_tablet_v2 and
 * zwp_tablet_tool_v2, which carry a pen's proximity, touch, pressure, tilt
 * and buttons.
 *
 * Their events reach listeners through the generic dispatch (event.c);
 * zwp_tablet_seat_v2.tablet_added and tool_added create server-made objects.
 * zwp_tablet_pad_v2 is described only so that pad_added names an interface:
 * its group event's pad group is not described, and the compositor never
 * announces a pad.  The descriptions follow the pinned wayland-protocols
 * description (userland/desktop/libwayland/API-PROVENANCE.md), whose notice is kept
 * there.
 */

#include "internal.h"

#include <wayland/tablet-unstable-v2-client-protocol.h>

/* The argument types of every message whose arguments name no interface (at most four). */
static const struct wl_interface *tablet_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The arguments of zwp_tablet_manager_v2.get_tablet_seat: the new tablet seat and the wl_seat. */
static const struct wl_interface *tablet_manager_seat_types[] = {
	&zwp_tablet_seat_v2_interface,
	&wl_seat_interface,
};

/* The requests of zwp_tablet_manager_v2, in wire opcode order. */
static const struct wl_message tablet_manager_requests[] = {
	{ "get_tablet_seat", "no", tablet_manager_seat_types },
	{ "destroy", "", NULL },
};

/* The immutable zwp_tablet_manager_v2 description. */
const struct wl_interface zwp_tablet_manager_v2_interface = {
	"zwp_tablet_manager_v2", 1, 2, tablet_manager_requests,
	0, NULL
};

/* The argument of tablet_added: the new tablet. */
static const struct wl_interface *tablet_seat_tablet_types[] = {
	&zwp_tablet_v2_interface,
};

/* The argument of tool_added: the new tool. */
static const struct wl_interface *tablet_seat_tool_types[] = {
	&zwp_tablet_tool_v2_interface,
};

/* The argument of pad_added: the new pad. */
static const struct wl_interface *tablet_seat_pad_types[] = {
	&zwp_tablet_pad_v2_interface,
};

/* The requests of zwp_tablet_seat_v2, in wire opcode order. */
static const struct wl_message tablet_seat_requests[] = {
	{ "destroy", "", NULL },
};

/* The events of zwp_tablet_seat_v2, in wire opcode order. */
static const struct wl_message tablet_seat_events[] = {
	{ "tablet_added", "n", tablet_seat_tablet_types },
	{ "tool_added", "n", tablet_seat_tool_types },
	{ "pad_added", "n", tablet_seat_pad_types },
};

/* The immutable zwp_tablet_seat_v2 description. */
const struct wl_interface zwp_tablet_seat_v2_interface = {
	"zwp_tablet_seat_v2", 1, 1, tablet_seat_requests,
	3, tablet_seat_events
};

/* The arguments of set_cursor: a serial, the cursor surface and the hotspot. */
static const struct wl_interface *tablet_tool_cursor_types[] = {
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
};

/* The arguments of proximity_in: a serial, the tablet and the surface. */
static const struct wl_interface *tablet_tool_proximity_types[] = {
	NULL,
	&zwp_tablet_v2_interface,
	&wl_surface_interface,
};

/* The requests of zwp_tablet_tool_v2, in wire opcode order. */
static const struct wl_message tablet_tool_requests[] = {
	{ "set_cursor", "u?oii", tablet_tool_cursor_types },
	{ "destroy", "", NULL },
};

/* The events of zwp_tablet_tool_v2, in wire opcode order. */
static const struct wl_message tablet_tool_events[] = {
	{ "type", "u", tablet_plain_types },
	{ "hardware_serial", "uu", tablet_plain_types },
	{ "hardware_id_wacom", "uu", tablet_plain_types },
	{ "capability", "u", tablet_plain_types },
	{ "done", "", NULL },
	{ "removed", "", NULL },
	{ "proximity_in", "uoo", tablet_tool_proximity_types },
	{ "proximity_out", "", NULL },
	{ "down", "u", tablet_plain_types },
	{ "up", "", NULL },
	{ "motion", "ff", tablet_plain_types },
	{ "pressure", "u", tablet_plain_types },
	{ "distance", "u", tablet_plain_types },
	{ "tilt", "ff", tablet_plain_types },
	{ "rotation", "f", tablet_plain_types },
	{ "slider", "i", tablet_plain_types },
	{ "wheel", "fi", tablet_plain_types },
	{ "button", "uuu", tablet_plain_types },
	{ "frame", "u", tablet_plain_types },
};

/* The immutable zwp_tablet_tool_v2 description. */
const struct wl_interface zwp_tablet_tool_v2_interface = {
	"zwp_tablet_tool_v2", 1, 2, tablet_tool_requests,
	19, tablet_tool_events
};

/* The requests of zwp_tablet_v2, in wire opcode order. */
static const struct wl_message tablet_requests[] = {
	{ "destroy", "", NULL },
};

/* The events of zwp_tablet_v2, in wire opcode order. */
static const struct wl_message tablet_events[] = {
	{ "name", "s", tablet_plain_types },
	{ "id", "uu", tablet_plain_types },
	{ "path", "s", tablet_plain_types },
	{ "done", "", NULL },
	{ "removed", "", NULL },
};

/* The immutable zwp_tablet_v2 description. */
const struct wl_interface zwp_tablet_v2_interface = {
	"zwp_tablet_v2", 1, 1, tablet_requests,
	5, tablet_events
};

/* The arguments of the pad's enter: a serial, the tablet and the surface. */
static const struct wl_interface *tablet_pad_enter_types[] = {
	NULL,
	&zwp_tablet_v2_interface,
	&wl_surface_interface,
};

/* The arguments of the pad's leave: a serial and the surface. */
static const struct wl_interface *tablet_pad_leave_types[] = {
	NULL,
	&wl_surface_interface,
};

/* The requests of zwp_tablet_pad_v2, in wire opcode order. */
static const struct wl_message tablet_pad_requests[] = {
	{ "set_feedback", "usu", tablet_plain_types },
	{ "destroy", "", NULL },
};

/* The events of zwp_tablet_pad_v2, in wire opcode order (the group's interface is not described). */
static const struct wl_message tablet_pad_events[] = {
	{ "group", "n", tablet_plain_types },
	{ "path", "s", tablet_plain_types },
	{ "buttons", "u", tablet_plain_types },
	{ "done", "", NULL },
	{ "button", "uuu", tablet_plain_types },
	{ "enter", "uoo", tablet_pad_enter_types },
	{ "leave", "uo", tablet_pad_leave_types },
	{ "removed", "", NULL },
};

/* The immutable zwp_tablet_pad_v2 description. */
const struct wl_interface zwp_tablet_pad_v2_interface = {
	"zwp_tablet_pad_v2", 1, 2, tablet_pad_requests,
	8, tablet_pad_events
};

/*
 * Sends the manager's get_tablet_seat: the tablets and tools of a seat for
 * this client.
 */
struct zwp_tablet_seat_v2 *
zwp_tablet_manager_v2_get_tablet_seat(
	struct zwp_tablet_manager_v2 *object,
	struct wl_seat *seat)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The new object and the seat. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)seat;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TABLET_MANAGER_V2_GET_TABLET_SEAT, &zwp_tablet_seat_v2_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the tablet seat. */
	return (struct zwp_tablet_seat_v2 *)created;
}

/*
 * Sends the manager's destroy; the tablet seats it made stay.
 */
void
zwp_tablet_manager_v2_destroy(
	struct zwp_tablet_manager_v2 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TABLET_MANAGER_V2_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Installs a tablet seat's listener (tablet_added, tool_added, pad_added).
 */
int
zwp_tablet_seat_v2_add_listener(
	struct zwp_tablet_seat_v2 *object,
	const struct zwp_tablet_seat_v2_listener *listener,
	void *data)
{
	int error;

	/* The callbacks receive the seat's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends the tablet seat's destroy; the tablets and tools it announced stay.
 */
void
zwp_tablet_seat_v2_destroy(
	struct zwp_tablet_seat_v2 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TABLET_SEAT_V2_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Installs a tool's listener (its description, proximity, touch, axes,
 * buttons and frames).
 */
int
zwp_tablet_tool_v2_add_listener(
	struct zwp_tablet_tool_v2 *object,
	const struct zwp_tablet_tool_v2_listener *listener,
	void *data)
{
	int error;

	/* The callbacks receive the tool's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends the tool's set_cursor: the surface shown as the cursor while the
 * tool is on the client's surfaces (none hides it).
 */
void
zwp_tablet_tool_v2_set_cursor(
	struct zwp_tablet_tool_v2 *object,
	uint32_t serial,
	struct wl_surface *surface,
	int32_t hotspot_x,
	int32_t hotspot_y)
{
	union wl_argument arguments[4];

	/* The proximity_in serial, the surface and the hotspot. */
	arguments[0].u = serial;
	arguments[1].o = (struct wl_object *)surface;
	arguments[2].i = hotspot_x;
	arguments[3].i = hotspot_y;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TABLET_TOOL_V2_SET_CURSOR, NULL, 0, 0, arguments);
}

/*
 * Sends the tool's destroy.
 */
void
zwp_tablet_tool_v2_destroy(
	struct zwp_tablet_tool_v2 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TABLET_TOOL_V2_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Installs a tablet's listener (name, id, path, done, removed).
 */
int
zwp_tablet_v2_add_listener(
	struct zwp_tablet_v2 *object,
	const struct zwp_tablet_v2_listener *listener,
	void *data)
{
	int error;

	/* The callbacks receive the tablet's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends the tablet's destroy.
 */
void
zwp_tablet_v2_destroy(
	struct zwp_tablet_v2 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TABLET_V2_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends the pad's destroy.
 */
void
zwp_tablet_pad_v2_destroy(
	struct zwp_tablet_pad_v2 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TABLET_PAD_V2_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}
