/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the virtual keyboard protocol
 * (virtual-keyboard-unstable-v1, version 1; ws095-p004):
 * zwp_virtual_keyboard_manager_v1 and zwp_virtual_keyboard_v1.  Only the
 * input method the compositor starts may bind the manager.  The descriptions follow
 * the pinned wlroots description (userland/desktop/libwayland/API-PROVENANCE.md).
 */

#include "internal.h"

#include <wayland/virtual-keyboard-unstable-v1-client-protocol.h>

/* The argument types of every message whose arguments name no interface (at most 4). */
static const struct wl_interface *virtual_keyboard_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The requests of zwp_virtual_keyboard_v1, in wire opcode order. */
static const struct wl_message virtual_keyboard_requests[] = {
	{ "keymap", "uhu", virtual_keyboard_plain_types },
	{ "key", "uuu", virtual_keyboard_plain_types },
	{ "modifiers", "uuuu", virtual_keyboard_plain_types },
	{ "destroy", "", NULL },
};

/* The immutable zwp_virtual_keyboard_v1 description. */
const struct wl_interface zwp_virtual_keyboard_v1_interface = {
	"zwp_virtual_keyboard_v1", 1, 4, virtual_keyboard_requests,
	0, NULL
};

/* The arguments of zwp_virtual_keyboard_manager_v1.create_virtual_keyboard. */
static const struct wl_interface *virtual_keyboard_manager_create_virtual_keyboard_types[] = {
	&wl_seat_interface,
	&zwp_virtual_keyboard_v1_interface,
};

/* The requests of zwp_virtual_keyboard_manager_v1, in wire opcode order. */
static const struct wl_message virtual_keyboard_manager_requests[] = {
	{ "create_virtual_keyboard", "on", virtual_keyboard_manager_create_virtual_keyboard_types },
};

/* The immutable zwp_virtual_keyboard_manager_v1 description. */
const struct wl_interface zwp_virtual_keyboard_manager_v1_interface = {
	"zwp_virtual_keyboard_manager_v1", 1, 1, virtual_keyboard_manager_requests,
	0, NULL
};

/*
 * Sends zwp_virtual_keyboard_v1.keymap: the keymap the keys are in, before any key.
 */
void
zwp_virtual_keyboard_v1_keymap(
	struct zwp_virtual_keyboard_v1 *object,
	uint32_t format,
	int32_t fd,
	uint32_t size)
{
	union wl_argument arguments[3];

	/* The arguments in wire order. */
	arguments[0].u = format;
	arguments[1].h = fd;
	arguments[2].u = size;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_VIRTUAL_KEYBOARD_V1_KEYMAP, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_virtual_keyboard_v1.key: a key pressed or let go.
 */
void
zwp_virtual_keyboard_v1_key(
	struct zwp_virtual_keyboard_v1 *object,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	union wl_argument arguments[3];

	/* The arguments in wire order. */
	arguments[0].u = time;
	arguments[1].u = key;
	arguments[2].u = state;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_VIRTUAL_KEYBOARD_V1_KEY, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_virtual_keyboard_v1.modifiers: the modifiers held.
 */
void
zwp_virtual_keyboard_v1_modifiers(
	struct zwp_virtual_keyboard_v1 *object,
	uint32_t mods_depressed,
	uint32_t mods_latched,
	uint32_t mods_locked,
	uint32_t group)
{
	union wl_argument arguments[4];

	/* The arguments in wire order. */
	arguments[0].u = mods_depressed;
	arguments[1].u = mods_latched;
	arguments[2].u = mods_locked;
	arguments[3].u = group;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_VIRTUAL_KEYBOARD_V1_MODIFIERS, NULL, 0, 0, arguments);
}

/*
 * Sends zwp_virtual_keyboard_v1.destroy: the keyboard is gone.
 */
void
zwp_virtual_keyboard_v1_destroy(
	struct zwp_virtual_keyboard_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_VIRTUAL_KEYBOARD_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends zwp_virtual_keyboard_manager_v1.create_virtual_keyboard: a virtual keyboard on a seat.
 */
struct zwp_virtual_keyboard_v1 *
zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
	struct zwp_virtual_keyboard_manager_v1 *object,
	struct wl_seat *seat)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The arguments in wire order. */
	arguments[0].o = (struct wl_object *)seat;
	arguments[1].n = 0;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, ZWP_VIRTUAL_KEYBOARD_MANAGER_V1_CREATE_VIRTUAL_KEYBOARD, &zwp_virtual_keyboard_v1_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new object. */
	return (struct zwp_virtual_keyboard_v1 *)created;
}
