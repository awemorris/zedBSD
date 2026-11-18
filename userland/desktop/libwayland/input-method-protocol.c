/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the input method protocol (input-method-unstable-v2,
 * version 1; ws095-p004): zwp_input_method_manager_v2, zwp_input_method_v2,
 * zwp_input_popup_surface_v2 and zwp_input_method_keyboard_grab_v2.
 *
 * Only the input method the compositor starts may bind the manager.  The events reach
 * listeners through the generic dispatch (event.c); the keyboard grab's
 * keymap carries a descriptor.  The descriptions follow the pinned wlroots
 * description (userland/desktop/libwayland/API-PROVENANCE.md).
 */

#include "internal.h"

#include <wayland/input-method-unstable-v2-client-protocol.h>

/* The argument types of every message whose arguments name no interface (at most 5). */
static const struct wl_interface *input_method_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The requests of zwp_input_popup_surface_v2, in wire opcode order. */
static const struct wl_message input_popup_requests[] = {
	{ "destroy", "", NULL },
};

/* The events of zwp_input_popup_surface_v2, in wire opcode order. */
static const struct wl_message input_popup_events[] = {
	{ "text_input_rectangle", "iiii", input_method_plain_types },
};

/* The immutable zwp_input_popup_surface_v2 description. */
const struct wl_interface zwp_input_popup_surface_v2_interface = {
	"zwp_input_popup_surface_v2", 1, 1, input_popup_requests,
	1, input_popup_events
};

/* The requests of zwp_input_method_keyboard_grab_v2, in wire opcode order. */
static const struct wl_message input_grab_requests[] = {
	{ "release", "", NULL },
};

/* The events of zwp_input_method_keyboard_grab_v2, in wire opcode order. */
static const struct wl_message input_grab_events[] = {
	{ "keymap", "uhu", input_method_plain_types },
	{ "key", "uuuu", input_method_plain_types },
	{ "modifiers", "uuuuu", input_method_plain_types },
	{ "repeat_info", "ii", input_method_plain_types },
};

/* The immutable zwp_input_method_keyboard_grab_v2 description. */
const struct wl_interface zwp_input_method_keyboard_grab_v2_interface = {
	"zwp_input_method_keyboard_grab_v2", 1, 1, input_grab_requests,
	4, input_grab_events
};

/* The arguments of zwp_input_method_v2.get_input_popup_surface. */
static const struct wl_interface *input_method_get_input_popup_surface_types[] = {
	&zwp_input_popup_surface_v2_interface,
	&wl_surface_interface,
};

/* The arguments of zwp_input_method_v2.grab_keyboard. */
static const struct wl_interface *input_method_grab_keyboard_types[] = {
	&zwp_input_method_keyboard_grab_v2_interface,
};

/* The requests of zwp_input_method_v2, in wire opcode order. */
static const struct wl_message input_method_requests[] = {
	{ "commit_string", "s", input_method_plain_types },
	{ "set_preedit_string", "sii", input_method_plain_types },
	{ "delete_surrounding_text", "uu", input_method_plain_types },
	{ "commit", "u", input_method_plain_types },
	{ "get_input_popup_surface", "no", input_method_get_input_popup_surface_types },
	{ "grab_keyboard", "n", input_method_grab_keyboard_types },
	{ "destroy", "", NULL },
};

/* The events of zwp_input_method_v2, in wire opcode order. */
static const struct wl_message input_method_events[] = {
	{ "activate", "", NULL },
	{ "deactivate", "", NULL },
	{ "surrounding_text", "suu", input_method_plain_types },
	{ "text_change_cause", "u", input_method_plain_types },
	{ "content_type", "uu", input_method_plain_types },
	{ "done", "", NULL },
	{ "unavailable", "", NULL },
};

/* The immutable zwp_input_method_v2 description. */
const struct wl_interface zwp_input_method_v2_interface = {
	"zwp_input_method_v2", 1, 7, input_method_requests,
	7, input_method_events
};

/* The arguments of zwp_input_method_manager_v2.get_input_method. */
static const struct wl_interface *input_method_manager_get_input_method_types[] = {
	&wl_seat_interface,
	&zwp_input_method_v2_interface,
};

/* The requests of zwp_input_method_manager_v2, in wire opcode order. */
static const struct wl_message input_method_manager_requests[] = {
	{ "get_input_method", "on", input_method_manager_get_input_method_types },
	{ "destroy", "", NULL },
};

/* The immutable zwp_input_method_manager_v2 description. */
const struct wl_interface zwp_input_method_manager_v2_interface = {
	"zwp_input_method_manager_v2", 1, 2, input_method_manager_requests,
	0, NULL
};

/*
 * Installs a listener of zwp_input_popup_surface_v2 (text_input_rectangle).
 */
int
zwp_input_popup_surface_v2_add_listener(
	struct zwp_input_popup_surface_v2 *object,
	const struct zwp_input_popup_surface_v2_listener *listener,
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
 * Sends zwp_input_popup_surface_v2.destroy: the popup is gone; its surface stays.
 */
void
zwp_input_popup_surface_v2_destroy(
	struct zwp_input_popup_surface_v2 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_POPUP_SURFACE_V2_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Installs a listener of zwp_input_method_keyboard_grab_v2 (keymap, key, modifiers, repeat_info).
 */
int
zwp_input_method_keyboard_grab_v2_add_listener(
	struct zwp_input_method_keyboard_grab_v2 *object,
	const struct zwp_input_method_keyboard_grab_v2_listener *listener,
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
 * Sends zwp_input_method_keyboard_grab_v2.release: the keyboard goes back to the applications.
 */
void
zwp_input_method_keyboard_grab_v2_release(
	struct zwp_input_method_keyboard_grab_v2 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_METHOD_KEYBOARD_GRAB_V2_RELEASE, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Installs a listener of zwp_input_method_v2 (activate, deactivate, surrounding_text, text_change_cause, content_type, done, unavailable).
 */
int
zwp_input_method_v2_add_listener(
	struct zwp_input_method_v2 *object,
	const struct zwp_input_method_v2_listener *listener,
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
 * Sends zwp_input_method_v2.commit_string: text to insert at the next commit.
 */
void
zwp_input_method_v2_commit_string(
	struct zwp_input_method_v2 *object,
	const char *text)
{
	union wl_argument arguments[1];

	/* The arguments in wire order. */
	arguments[0].s = text;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_METHOD_V2_COMMIT_STRING, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_input_method_v2.set_preedit_string: the text being composed, shown at the next commit.
 */
void
zwp_input_method_v2_set_preedit_string(
	struct zwp_input_method_v2 *object,
	const char *text,
	int32_t cursor_begin,
	int32_t cursor_end)
{
	union wl_argument arguments[3];

	/* The arguments in wire order. */
	arguments[0].s = text;
	arguments[1].i = cursor_begin;
	arguments[2].i = cursor_end;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_METHOD_V2_SET_PREEDIT_STRING, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_input_method_v2.delete_surrounding_text: text to delete around the cursor at the next commit.
 */
void
zwp_input_method_v2_delete_surrounding_text(
	struct zwp_input_method_v2 *object,
	uint32_t before_length,
	uint32_t after_length)
{
	union wl_argument arguments[2];

	/* The arguments in wire order. */
	arguments[0].u = before_length;
	arguments[1].u = after_length;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_METHOD_V2_DELETE_SURROUNDING_TEXT, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_input_method_v2.commit: what was set since the last commit applies.
 */
void
zwp_input_method_v2_commit(
	struct zwp_input_method_v2 *object,
	uint32_t serial)
{
	union wl_argument arguments[1];

	/* The arguments in wire order. */
	arguments[0].u = serial;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_METHOD_V2_COMMIT, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_input_method_v2.get_input_popup_surface: a surface shown by the text being composed.
 */
struct zwp_input_popup_surface_v2 *
zwp_input_method_v2_get_input_popup_surface(
	struct zwp_input_method_v2 *object,
	struct wl_surface *surface)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The arguments in wire order. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)surface;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_METHOD_V2_GET_INPUT_POPUP_SURFACE, &zwp_input_popup_surface_v2_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new object. */
	return (struct zwp_input_popup_surface_v2 *)created;
}

/*
 * Sends zwp_input_method_v2.grab_keyboard: the keyboard's keys come to the input method.
 */
struct zwp_input_method_keyboard_grab_v2 *
zwp_input_method_v2_grab_keyboard(
	struct zwp_input_method_v2 *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* The arguments in wire order. */
	arguments[0].n = 0;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_METHOD_V2_GRAB_KEYBOARD, &zwp_input_method_keyboard_grab_v2_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new object. */
	return (struct zwp_input_method_keyboard_grab_v2 *)created;
}

/*
 * Sends zwp_input_method_v2.destroy: the input method is gone.
 */
void
zwp_input_method_v2_destroy(
	struct zwp_input_method_v2 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_METHOD_V2_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends zwp_input_method_manager_v2.get_input_method: the input method of a seat.
 */
struct zwp_input_method_v2 *
zwp_input_method_manager_v2_get_input_method(
	struct zwp_input_method_manager_v2 *object,
	struct wl_seat *seat)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The arguments in wire order. */
	arguments[0].o = (struct wl_object *)seat;
	arguments[1].n = 0;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_METHOD_MANAGER_V2_GET_INPUT_METHOD, &zwp_input_method_v2_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new object. */
	return (struct zwp_input_method_v2 *)created;
}

/*
 * Sends zwp_input_method_manager_v2.destroy: what it made stays.
 */
void
zwp_input_method_manager_v2_destroy(
	struct zwp_input_method_manager_v2 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_INPUT_METHOD_MANAGER_V2_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}
