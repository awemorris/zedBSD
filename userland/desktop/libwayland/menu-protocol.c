/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the compositor's System Menu protocol (WS070).
 *
 * xdg_menu_manager_v1 makes menu models (xdg_menu_v1), the places on
 * windows that show them (xdg_toplevel_menu_v1) and, from version 2, the
 * one-time context menus opened at a point of a surface
 * (xdg_context_menu_v1, ws071-p009).  The protocol is
 * the compositor's own; its header is private and applications use it through
 * libkeiland.  plan/ws070/design.md defines every request and event.
 */

#include "internal.h"

/*
 * The argument types of every message whose arguments are all numbers or
 * strings.  Such a message names no interface, so one array of empty
 * entries, as long as the longest of them (insert_item's six), serves them
 * all.
 */
static const struct wl_interface *menu_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The argument of xdg_menu_manager_v1.create_menu: the new menu. */
static const struct wl_interface *menu_manager_create_types[] = {
	&xdg_menu_v1_interface,
};

/* The arguments of xdg_menu_manager_v1.get_toplevel_menu: the new place, and the window. */
static const struct wl_interface *menu_manager_toplevel_types[] = {
	&xdg_toplevel_menu_v1_interface,
	&xdg_toplevel_interface,
};

/*
 * The arguments of xdg_menu_manager_v1.get_context_menu (version 2): the
 * new context menu, the menu shown, the surface and the point on it, and
 * the seat and serial of the press it answers.
 */
static const struct wl_interface *menu_manager_context_types[] = {
	&xdg_context_menu_v1_interface,
	&xdg_menu_v1_interface,
	&wl_surface_interface,
	NULL,
	NULL,
	&wl_seat_interface,
	NULL,
};

/* The requests of xdg_menu_manager_v1, in wire order. */
static const struct wl_message menu_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "create_menu", "n", menu_manager_create_types },
	{ "get_toplevel_menu", "no", menu_manager_toplevel_types },
	{ "get_context_menu", "2nooiiou", menu_manager_context_types },
};

/* Describes the global that makes menus, their places on windows and context menus. */
const struct wl_interface xdg_menu_manager_v1_interface = {
	"xdg_menu_manager_v1", 2, 4, menu_manager_requests,
	0, NULL
};

/* The requests of xdg_menu_v1, in wire order. */
static const struct wl_message menu_requests[] = {
	{ "destroy", "", NULL },
	{ "begin_update", "u", menu_plain_types },
	{ "commit", "u", menu_plain_types },
	{ "append_item", "uuusu", menu_plain_types },
	{ "insert_item", "uuuusu", menu_plain_types },
	{ "remove_item", "u", menu_plain_types },
	{ "set_label", "us", menu_plain_types },
	{ "set_action", "uu", menu_plain_types },
	{ "set_enabled", "uu", menu_plain_types },
	{ "set_visible", "uu", menu_plain_types },
	{ "set_checked", "uu", menu_plain_types },
	{ "set_role", "uu", menu_plain_types },
	{ "set_icon_name", "us", menu_plain_types },
	{ "set_shortcut", "uuu", menu_plain_types },
};

/* Describes one menu model; it has requests only. */
const struct wl_interface xdg_menu_v1_interface = {
	"xdg_menu_v1", 1, 14, menu_requests,
	0, NULL
};

/* The argument of xdg_toplevel_menu_v1.set_menu: the menu shown, or none. */
static const struct wl_interface *toplevel_menu_set_types[] = {
	&xdg_menu_v1_interface,
};

/* The requests of xdg_toplevel_menu_v1, in wire order. */
static const struct wl_message toplevel_menu_requests[] = {
	{ "destroy", "", NULL },
	{ "set_menu", "?o", toplevel_menu_set_types },
};

/* The arguments of xdg_toplevel_menu_v1.activated: item, action, the seat (or none), serial. */
static const struct wl_interface *toplevel_menu_activated_types[] = {
	NULL,
	NULL,
	&wl_seat_interface,
	NULL,
};

/* The events of xdg_toplevel_menu_v1, in wire order. */
static const struct wl_message toplevel_menu_events[] = {
	{ "activated", "uu?ou", toplevel_menu_activated_types },
	{ "opened", "u", menu_plain_types },
	{ "closed", "u", menu_plain_types },
};

/* Describes the place on a window that shows a menu. */
const struct wl_interface xdg_toplevel_menu_v1_interface = {
	"xdg_toplevel_menu_v1", 1, 2, toplevel_menu_requests,
	3, toplevel_menu_events
};

/* The requests of xdg_context_menu_v1, in wire order. */
static const struct wl_message context_menu_requests[] = {
	{ "destroy", "", NULL },
};

/* The events of xdg_context_menu_v1, in wire order: a choice (item, action, serial), and the end. */
static const struct wl_message context_menu_events[] = {
	{ "activated", "uuu", menu_plain_types },
	{ "done", "", NULL },
};

/* Describes one context menu opened at a point of a surface (version 2). */
const struct wl_interface xdg_context_menu_v1_interface = {
	"xdg_context_menu_v1", 2, 1, context_menu_requests,
	2, context_menu_events
};

/*
 * Sends xdg_menu_manager_v1.destroy; the menus it made stay.
 */
void
xdg_menu_manager_v1_destroy(
	struct xdg_menu_manager_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_MANAGER_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends xdg_menu_manager_v1.create_menu and returns the new menu.
 */
struct xdg_menu_v1 *
xdg_menu_manager_v1_create_menu(
	struct xdg_menu_manager_v1 *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* The only argument is the new menu's identity. */
	arguments[0].n = 0;

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_MANAGER_V1_CREATE_MENU, &xdg_menu_v1_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new menu. */
	return (struct xdg_menu_v1 *)created;
}

/*
 * Sends xdg_menu_manager_v1.get_toplevel_menu and returns the window's new place for a menu.
 */
struct xdg_toplevel_menu_v1 *
xdg_menu_manager_v1_get_toplevel_menu(
	struct xdg_menu_manager_v1 *object,
	struct xdg_toplevel *toplevel)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The new place's identity, then the window it belongs to. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)toplevel;

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_MANAGER_V1_GET_TOPLEVEL_MENU, &xdg_toplevel_menu_v1_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new place. */
	return (struct xdg_toplevel_menu_v1 *)created;
}

/*
 * Sends xdg_menu_manager_v1.get_context_menu (version 2): opens a menu's
 * top-level items as a popup at a point of a surface, answering a press,
 * and returns the context menu that reports the choice and the end.
 */
struct xdg_context_menu_v1 *
xdg_menu_manager_v1_get_context_menu(
	struct xdg_menu_manager_v1 *object,
	struct xdg_menu_v1 *menu,
	struct wl_surface *surface,
	int32_t x,
	int32_t y,
	struct wl_seat *seat,
	uint32_t serial)
{
	union wl_argument arguments[7];
	struct wl_proxy *created;

	/* The new context menu, the menu, the surface and the point, the seat and the press's serial. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)menu;
	arguments[2].o = (struct wl_object *)surface;
	arguments[3].i = x;
	arguments[4].i = y;
	arguments[5].o = (struct wl_object *)seat;
	arguments[6].u = serial;

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_MANAGER_V1_GET_CONTEXT_MENU, &xdg_context_menu_v1_interface, 2U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new context menu. */
	return (struct xdg_context_menu_v1 *)created;
}

/*
 * Installs the typed listener of an xdg_context_menu_v1.
 */
int
xdg_context_menu_v1_add_listener(
	struct xdg_context_menu_v1 *object,
	const struct xdg_context_menu_v1_listener *listener,
	void *data)
{
	int error;

	/* The typed callbacks receive the proxy's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends xdg_context_menu_v1.destroy: an open context menu closes.
 */
void
xdg_context_menu_v1_destroy(
	struct xdg_context_menu_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_CONTEXT_MENU_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Associates client state with the xdg_menu_manager_v1 proxy.
 */
void
xdg_menu_manager_v1_set_user_data(
	struct xdg_menu_manager_v1 *object,
	void *data)
{
	/* The common proxy keeps the pointer. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);
}

/*
 * Obtains client state from the xdg_menu_manager_v1 proxy.
 */
void *
xdg_menu_manager_v1_get_user_data(
	struct xdg_menu_manager_v1 *object)
{
	void *data;

	/* The common proxy keeps the pointer. */
	data = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the pointer. */
	return data;
}

/*
 * Obtains the negotiated version of the xdg_menu_manager_v1 proxy.
 */
uint32_t
xdg_menu_manager_v1_get_version(
	struct xdg_menu_manager_v1 *object)
{
	uint32_t version;

	/* The version the binding was made at. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the version. */
	return version;
}

/*
 * Sends xdg_menu_v1.destroy; windows showing the menu show none.
 */
void
xdg_menu_v1_destroy(
	struct xdg_menu_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends xdg_menu_v1.begin_update: the changes that follow wait for the commit of the same serial.
 */
void
xdg_menu_v1_begin_update(
	struct xdg_menu_v1 *object,
	uint32_t serial)
{
	union wl_argument arguments[1];

	/* The serial the commit will name. */
	arguments[0].u = serial;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_BEGIN_UPDATE, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.commit: the changes since begin_update are shown together.
 */
void
xdg_menu_v1_commit(
	struct xdg_menu_v1 *object,
	uint32_t serial)
{
	union wl_argument arguments[1];

	/* The serial begin_update named. */
	arguments[0].u = serial;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_COMMIT, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.append_item: a new last child of a parent (0 for the top level).
 */
void
xdg_menu_v1_append_item(
	struct xdg_menu_v1 *object,
	uint32_t id,
	uint32_t parent_id,
	uint32_t type,
	const char *label,
	uint32_t action)
{
	union wl_argument arguments[5];

	/* The item, its parent, its type, its label and its action. */
	arguments[0].u = id;
	arguments[1].u = parent_id;
	arguments[2].u = type;
	arguments[3].s = label;
	arguments[4].u = action;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_APPEND_ITEM, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.insert_item: a new child of a parent, before one of its children (0 for last).
 */
void
xdg_menu_v1_insert_item(
	struct xdg_menu_v1 *object,
	uint32_t id,
	uint32_t parent_id,
	uint32_t before_id,
	uint32_t type,
	const char *label,
	uint32_t action)
{
	union wl_argument arguments[6];

	/* The item, its parent, the sibling it goes before, its type, its label and its action. */
	arguments[0].u = id;
	arguments[1].u = parent_id;
	arguments[2].u = before_id;
	arguments[3].u = type;
	arguments[4].s = label;
	arguments[5].u = action;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_INSERT_ITEM, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.remove_item: the item and everything under it go.
 */
void
xdg_menu_v1_remove_item(
	struct xdg_menu_v1 *object,
	uint32_t id)
{
	union wl_argument arguments[1];

	/* The item removed. */
	arguments[0].u = id;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_REMOVE_ITEM, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.set_label.
 */
void
xdg_menu_v1_set_label(
	struct xdg_menu_v1 *object,
	uint32_t id,
	const char *label)
{
	union wl_argument arguments[2];

	/* The item and its new label. */
	arguments[0].u = id;
	arguments[1].s = label;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_SET_LABEL, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.set_action.
 */
void
xdg_menu_v1_set_action(
	struct xdg_menu_v1 *object,
	uint32_t id,
	uint32_t action)
{
	union wl_argument arguments[2];

	/* The item and the action its activation names. */
	arguments[0].u = id;
	arguments[1].u = action;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_SET_ACTION, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.set_enabled.
 */
void
xdg_menu_v1_set_enabled(
	struct xdg_menu_v1 *object,
	uint32_t id,
	uint32_t enabled)
{
	union wl_argument arguments[2];

	/* The item and whether it can be chosen. */
	arguments[0].u = id;
	arguments[1].u = enabled;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_SET_ENABLED, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.set_visible.
 */
void
xdg_menu_v1_set_visible(
	struct xdg_menu_v1 *object,
	uint32_t id,
	uint32_t visible)
{
	union wl_argument arguments[2];

	/* The item and whether it is shown. */
	arguments[0].u = id;
	arguments[1].u = visible;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_SET_VISIBLE, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.set_checked (checkbox and radio items only).
 */
void
xdg_menu_v1_set_checked(
	struct xdg_menu_v1 *object,
	uint32_t id,
	uint32_t checked)
{
	union wl_argument arguments[2];

	/* The item and whether it is checked. */
	arguments[0].u = id;
	arguments[1].u = checked;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_SET_CHECKED, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.set_role.
 */
void
xdg_menu_v1_set_role(
	struct xdg_menu_v1 *object,
	uint32_t id,
	uint32_t role)
{
	union wl_argument arguments[2];

	/* The item and what it means to the system. */
	arguments[0].u = id;
	arguments[1].u = role;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_SET_ROLE, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.set_icon_name (an empty name clears it).
 */
void
xdg_menu_v1_set_icon_name(
	struct xdg_menu_v1 *object,
	uint32_t id,
	const char *icon_name)
{
	union wl_argument arguments[2];

	/* The item and the icon theme's name for its icon. */
	arguments[0].u = id;
	arguments[1].s = icon_name;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_SET_ICON_NAME, NULL, 0, 0, arguments);
}

/*
 * Sends xdg_menu_v1.set_shortcut (keysym 0 clears it).
 */
void
xdg_menu_v1_set_shortcut(
	struct xdg_menu_v1 *object,
	uint32_t id,
	uint32_t modifiers,
	uint32_t keysym)
{
	union wl_argument arguments[3];

	/* The item, the modifiers held and the key's XKB keysym. */
	arguments[0].u = id;
	arguments[1].u = modifiers;
	arguments[2].u = keysym;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_MENU_V1_SET_SHORTCUT, NULL, 0, 0, arguments);
}

/*
 * Associates client state with the xdg_menu_v1 proxy.
 */
void
xdg_menu_v1_set_user_data(
	struct xdg_menu_v1 *object,
	void *data)
{
	/* The common proxy keeps the pointer. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);
}

/*
 * Obtains client state from the xdg_menu_v1 proxy.
 */
void *
xdg_menu_v1_get_user_data(
	struct xdg_menu_v1 *object)
{
	void *data;

	/* The common proxy keeps the pointer. */
	data = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the pointer. */
	return data;
}

/*
 * Obtains the negotiated version of the xdg_menu_v1 proxy.
 */
uint32_t
xdg_menu_v1_get_version(
	struct xdg_menu_v1 *object)
{
	uint32_t version;

	/* The version of the manager that made it. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the version. */
	return version;
}

/*
 * Installs the listener for xdg_toplevel_menu_v1 events.
 */
int
xdg_toplevel_menu_v1_add_listener(
	struct xdg_toplevel_menu_v1 *object,
	const struct xdg_toplevel_menu_v1_listener *listener,
	void *data)
{
	int error;

	/* The typed callbacks receive the proxy's events from now on. */
	error = wl_proxy_add_listener((struct wl_proxy *)object, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: the listener is installed. */
	return 0;
}

/*
 * Sends xdg_toplevel_menu_v1.destroy; the window shows no menu.
 */
void
xdg_toplevel_menu_v1_destroy(
	struct xdg_toplevel_menu_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_TOPLEVEL_MENU_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends xdg_toplevel_menu_v1.set_menu (NULL shows no menu).
 */
void
xdg_toplevel_menu_v1_set_menu(
	struct xdg_toplevel_menu_v1 *object,
	struct xdg_menu_v1 *menu)
{
	union wl_argument arguments[1];

	/* The menu the window shows from now on. */
	arguments[0].o = (struct wl_object *)menu;

	/* Queues the request. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, XDG_TOPLEVEL_MENU_V1_SET_MENU, NULL, 0, 0, arguments);
}

/*
 * Associates client state with the xdg_toplevel_menu_v1 proxy.
 */
void
xdg_toplevel_menu_v1_set_user_data(
	struct xdg_toplevel_menu_v1 *object,
	void *data)
{
	/* The common proxy keeps the pointer. */
	wl_proxy_set_user_data((struct wl_proxy *)object, data);
}

/*
 * Obtains client state from the xdg_toplevel_menu_v1 proxy.
 */
void *
xdg_toplevel_menu_v1_get_user_data(
	struct xdg_toplevel_menu_v1 *object)
{
	void *data;

	/* The common proxy keeps the pointer. */
	data = wl_proxy_get_user_data((struct wl_proxy *)object);

	/* Succeeded: reports the pointer. */
	return data;
}

/*
 * Obtains the negotiated version of the xdg_toplevel_menu_v1 proxy.
 */
uint32_t
xdg_toplevel_menu_v1_get_version(
	struct xdg_toplevel_menu_v1 *object)
{
	uint32_t version;

	/* The version of the manager that made it. */
	version = wl_proxy_get_version((struct wl_proxy *)object);

	/* Succeeded: reports the version. */
	return version;
}

/*
 * Calls the typed listener of an xdg_toplevel_menu_v1 event.
 *
 * Returns 0 when the event was delivered or deliberately ignored, EPROTO for
 * an opcode the interface does not have.
 */
int
wlc_menu_dispatch(
	struct wlc_event *event,
	const void *listener,
	void *data)
{
	const struct xdg_toplevel_menu_v1_listener *callbacks;
	struct xdg_toplevel_menu_v1 *object;
	union wl_argument *arguments;

	/* The listener, the proxy and the decoded arguments. */
	callbacks = listener;
	object = (struct xdg_toplevel_menu_v1 *)event->proxy;
	arguments = event->arguments;

	/* Selects the callback by the event's opcode. */
	switch (event->opcode) {
	case 0:
		/* An item was chosen (item, action, seat and serial); a listener without the callback ignores it. */
		if (callbacks->activated == NULL)
			return 0;

		/* The callback takes the event. */
		event->delivered = 1;
		callbacks->activated(data, object, arguments[0].u, arguments[1].u, (struct wl_seat *)arguments[2].o, arguments[3].u);
		return 0;
	case 1:
		/* A submenu's popup opened; a listener without the callback ignores it. */
		if (callbacks->opened == NULL)
			return 0;

		/* The callback takes the event. */
		event->delivered = 1;
		callbacks->opened(data, object, arguments[0].u);
		return 0;
	case 2:
		/* A submenu's popup closed; a listener without the callback ignores it. */
		if (callbacks->closed == NULL)
			return 0;

		/* The callback takes the event. */
		event->delivered = 1;
		callbacks->closed(data, object, arguments[0].u);
		return 0;
	default:
		break;
	}

	/* The interface has no other event. */
	return EPROTO;
}

/*
 * Calls the typed listener of an xdg_context_menu_v1 event.
 *
 * Returns 0 when the event was delivered or deliberately ignored, EPROTO for
 * an opcode the interface does not have.
 */
int
wlc_context_menu_dispatch(
	struct wlc_event *event,
	const void *listener,
	void *data)
{
	const struct xdg_context_menu_v1_listener *callbacks;
	struct xdg_context_menu_v1 *object;
	union wl_argument *arguments;

	/* The listener, the proxy and the decoded arguments. */
	callbacks = listener;
	object = (struct xdg_context_menu_v1 *)event->proxy;
	arguments = event->arguments;

	/* Selects the callback by the event's opcode. */
	switch (event->opcode) {
	case 0:
		/* An item was chosen (item, action and serial); a listener without the callback ignores it. */
		if (callbacks->activated == NULL)
			return 0;

		/* The callback takes the event. */
		event->delivered = 1;
		callbacks->activated(data, object, arguments[0].u, arguments[1].u, arguments[2].u);
		return 0;
	case 1:
		/* The context menu closed (after a choice, or without one); a listener without the callback ignores it. */
		if (callbacks->done == NULL)
			return 0;

		/* The callback takes the event. */
		event->delivered = 1;
		callbacks->done(data, object);
		return 0;
	default:
		break;
	}

	/* The interface has no other event. */
	return EPROTO;
}
