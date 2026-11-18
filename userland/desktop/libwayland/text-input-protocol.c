/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the text input protocol (text-input-unstable-v3,
 * version 1; ws095-p004): zwp_text_input_manager_v3 and zwp_text_input_v3.
 *
 * An application enables a text input while an editable field has the
 * keyboard, and hears the input method's preedit, commit and
 * delete_surrounding_text, applied together at done.  The events reach
 * listeners through the generic dispatch (event.c).  The descriptions follow
 * the pinned wayland-protocols description (userland/desktop/libwayland/API-PROVENANCE.md).
 */

#include "internal.h"

#include <wayland/text-input-unstable-v3-client-protocol.h>

/* The argument types of every message whose arguments name no interface (at most 4). */
static const struct wl_interface *text_input_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The requests of zwp_text_input_v3, in wire opcode order. */
static const struct wl_message text_input_requests[] = {
	{ "destroy", "", NULL },
	{ "enable", "", NULL },
	{ "disable", "", NULL },
	{ "set_surrounding_text", "sii", text_input_plain_types },
	{ "set_text_change_cause", "u", text_input_plain_types },
	{ "set_content_type", "uu", text_input_plain_types },
	{ "set_cursor_rectangle", "iiii", text_input_plain_types },
	{ "commit", "", NULL },
};

/* The arguments of zwp_text_input_v3.enter. */
static const struct wl_interface *text_input_enter_types[] = {
	&wl_surface_interface,
};

/* The arguments of zwp_text_input_v3.leave. */
static const struct wl_interface *text_input_leave_types[] = {
	&wl_surface_interface,
};

/* The events of zwp_text_input_v3, in wire opcode order. */
static const struct wl_message text_input_events[] = {
	{ "enter", "o", text_input_enter_types },
	{ "leave", "o", text_input_leave_types },
	{ "preedit_string", "?sii", text_input_plain_types },
	{ "commit_string", "?s", text_input_plain_types },
	{ "delete_surrounding_text", "uu", text_input_plain_types },
	{ "done", "u", text_input_plain_types },
};

/* The immutable zwp_text_input_v3 description. */
const struct wl_interface zwp_text_input_v3_interface = {
	"zwp_text_input_v3", 1, 8, text_input_requests,
	6, text_input_events
};

/* The arguments of zwp_text_input_manager_v3.get_text_input. */
static const struct wl_interface *text_input_manager_get_text_input_types[] = {
	&zwp_text_input_v3_interface,
	&wl_seat_interface,
};

/* The requests of zwp_text_input_manager_v3, in wire opcode order. */
static const struct wl_message text_input_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_text_input", "no", text_input_manager_get_text_input_types },
};

/* The immutable zwp_text_input_manager_v3 description. */
const struct wl_interface zwp_text_input_manager_v3_interface = {
	"zwp_text_input_manager_v3", 1, 2, text_input_manager_requests,
	0, NULL
};

/*
 * Installs a listener of zwp_text_input_v3 (enter, leave, preedit_string, commit_string, delete_surrounding_text, done).
 */
int
zwp_text_input_v3_add_listener(
	struct zwp_text_input_v3 *object,
	const struct zwp_text_input_v3_listener *listener,
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
 * Sends zwp_text_input_v3.destroy: the text input is gone.
 */
void
zwp_text_input_v3_destroy(
	struct zwp_text_input_v3 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TEXT_INPUT_V3_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends zwp_text_input_v3.enable: an editable field has the keyboard.
 */
void
zwp_text_input_v3_enable(
	struct zwp_text_input_v3 *object)
{
	/* The request has no arguments. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TEXT_INPUT_V3_ENABLE, NULL, 0, 0, NULL);
}

/*
 * Sends zwp_text_input_v3.disable: no editable field has the keyboard.
 */
void
zwp_text_input_v3_disable(
	struct zwp_text_input_v3 *object)
{
	/* The request has no arguments. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TEXT_INPUT_V3_DISABLE, NULL, 0, 0, NULL);
}

/*
 * Sends zwp_text_input_v3.set_surrounding_text: the text around the cursor.
 */
void
zwp_text_input_v3_set_surrounding_text(
	struct zwp_text_input_v3 *object,
	const char *text,
	int32_t cursor,
	int32_t anchor)
{
	union wl_argument arguments[3];

	/* The arguments in wire order. */
	arguments[0].s = text;
	arguments[1].i = cursor;
	arguments[2].i = anchor;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TEXT_INPUT_V3_SET_SURROUNDING_TEXT, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_text_input_v3.set_text_change_cause: what changed the text last.
 */
void
zwp_text_input_v3_set_text_change_cause(
	struct zwp_text_input_v3 *object,
	uint32_t cause)
{
	union wl_argument arguments[1];

	/* The arguments in wire order. */
	arguments[0].u = cause;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TEXT_INPUT_V3_SET_TEXT_CHANGE_CAUSE, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_text_input_v3.set_content_type: what the field holds.
 */
void
zwp_text_input_v3_set_content_type(
	struct zwp_text_input_v3 *object,
	uint32_t hint,
	uint32_t purpose)
{
	union wl_argument arguments[2];

	/* The arguments in wire order. */
	arguments[0].u = hint;
	arguments[1].u = purpose;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TEXT_INPUT_V3_SET_CONTENT_TYPE, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_text_input_v3.set_cursor_rectangle: where the cursor is on the surface.
 */
void
zwp_text_input_v3_set_cursor_rectangle(
	struct zwp_text_input_v3 *object,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	union wl_argument arguments[4];

	/* The arguments in wire order. */
	arguments[0].i = x;
	arguments[1].i = y;
	arguments[2].i = width;
	arguments[3].i = height;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TEXT_INPUT_V3_SET_CURSOR_RECTANGLE, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_text_input_v3.commit: the state set since the last commit applies.
 */
void
zwp_text_input_v3_commit(
	struct zwp_text_input_v3 *object)
{
	/* The request has no arguments. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TEXT_INPUT_V3_COMMIT, NULL, 0, 0, NULL);
}

/*
 * Sends zwp_text_input_manager_v3.destroy: the text inputs it made stay.
 */
void
zwp_text_input_manager_v3_destroy(
	struct zwp_text_input_manager_v3 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TEXT_INPUT_MANAGER_V3_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends zwp_text_input_manager_v3.get_text_input: a text input on a seat.
 */
struct zwp_text_input_v3 *
zwp_text_input_manager_v3_get_text_input(
	struct zwp_text_input_manager_v3 *object,
	struct wl_seat *seat)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The arguments in wire order. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)seat;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_TEXT_INPUT_MANAGER_V3_GET_TEXT_INPUT, &zwp_text_input_v3_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new object. */
	return (struct zwp_text_input_v3 *)created;
}
