/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals zdesktop's keyboard inset protocol
 * (kl_keyboard_inset_manager_v1 and kl_keyboard_inset_v1,
 * version 1; ws102-p015, plan/ws102/design.md section 2.8).  A window's
 * inset hears how much of it the on-screen keyboard covers.
 */

#include "internal.h"

#include "userland/desktop/libwayland/keiland-keyboard-inset-v1-client-protocol.h"

/* The argument types of every message whose arguments name no interface (at most 3). */
static const struct wl_interface *inset_plain_types[] = {
	NULL,
	NULL,
	NULL,
};

/* The requests of kl_keyboard_inset_v1, in wire opcode order. */
static const struct wl_message inset_requests[] = {
	{ "destroy", "", NULL },
};

/* The events of kl_keyboard_inset_v1, in wire opcode order. */
static const struct wl_message inset_events[] = {
	{ "inset", "iiu", inset_plain_types },
};

/* The immutable kl_keyboard_inset_v1 description. */
const struct wl_interface kl_keyboard_inset_v1_interface = {
	"kl_keyboard_inset_v1", 1, 1, inset_requests,
	1, inset_events
};

/* The arguments of kl_keyboard_inset_manager_v1.get_inset: the new inset, and the window. */
static const struct wl_interface *inset_manager_get_types[] = {
	&kl_keyboard_inset_v1_interface,
	&xdg_toplevel_interface,
};

/* The requests of kl_keyboard_inset_manager_v1, in wire opcode order. */
static const struct wl_message inset_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_inset", "no", inset_manager_get_types },
};

/* The immutable kl_keyboard_inset_manager_v1 description. */
const struct wl_interface kl_keyboard_inset_manager_v1_interface = {
	"kl_keyboard_inset_manager_v1", 1, 2, inset_manager_requests,
	0, NULL
};

/*
 * Installs a listener of kl_keyboard_inset_v1 (inset).
 */
int
kl_keyboard_inset_v1_add_listener(
	struct kl_keyboard_inset_v1 *object,
	const struct kl_keyboard_inset_v1_listener *listener,
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
 * Sends kl_keyboard_inset_v1.destroy: the window hears no more.
 */
void
kl_keyboard_inset_v1_destroy(
	struct kl_keyboard_inset_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_KEYBOARD_INSET_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_keyboard_inset_manager_v1.destroy: the insets it gave stay.
 */
void
kl_keyboard_inset_manager_v1_destroy(
	struct kl_keyboard_inset_manager_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_KEYBOARD_INSET_MANAGER_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_keyboard_inset_manager_v1.get_inset and returns the window's new inset.
 */
struct kl_keyboard_inset_v1 *
kl_keyboard_inset_manager_v1_get_inset(
	struct kl_keyboard_inset_manager_v1 *object,
	struct xdg_toplevel *toplevel)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The new inset's identity, then the window it belongs to. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)toplevel;

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_KEYBOARD_INSET_MANAGER_V1_GET_INSET, &kl_keyboard_inset_v1_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new inset. */
	return (struct kl_keyboard_inset_v1 *)created;
}
