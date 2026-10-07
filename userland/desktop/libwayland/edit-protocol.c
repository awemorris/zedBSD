/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals the compositor's editing operations protocol
 * (kl_edit_manager_v1 and kl_edit_v1, version 1; ws102-p017,
 * plan/ws102/design.md section 2.10).  A window says which editing
 * operations it carries out and its state, and hears the operations the
 * on-screen keyboard's buttons ask for.
 */

#include "internal.h"

#include "userland/desktop/libwayland/keiland-edit-v1-client-protocol.h"

/* The argument types of every message whose arguments name no interface (at most 2). */
static const struct wl_interface *edit_plain_types[] = {
	NULL,
	NULL,
};

/* The requests of kl_edit_v1, in wire opcode order. */
static const struct wl_message edit_requests[] = {
	{ "destroy", "", NULL },
	{ "set_state", "uu", edit_plain_types },
};

/* The events of kl_edit_v1, in wire opcode order. */
static const struct wl_message edit_events[] = {
	{ "action", "u", edit_plain_types },
};

/* The immutable kl_edit_v1 description. */
const struct wl_interface kl_edit_v1_interface = {
	"kl_edit_v1", 1, 2, edit_requests,
	1, edit_events
};

/* The arguments of kl_edit_manager_v1.get_edit: the new edit object, and the window. */
static const struct wl_interface *edit_manager_get_types[] = {
	&kl_edit_v1_interface,
	&xdg_toplevel_interface,
};

/* The requests of kl_edit_manager_v1, in wire opcode order. */
static const struct wl_message edit_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_edit", "no", edit_manager_get_types },
};

/* The immutable kl_edit_manager_v1 description. */
const struct wl_interface kl_edit_manager_v1_interface = {
	"kl_edit_manager_v1", 1, 2, edit_manager_requests,
	0, NULL
};

/*
 * Installs a listener of kl_edit_v1 (action).
 */
int
kl_edit_v1_add_listener(
	struct kl_edit_v1 *object,
	const struct kl_edit_v1_listener *listener,
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
 * Sends kl_edit_v1.destroy: the window hears no more actions.
 */
void
kl_edit_v1_destroy(
	struct kl_edit_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_EDIT_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_edit_v1.set_state: the operations the window carries out and its state.
 */
void
kl_edit_v1_set_state(
	struct kl_edit_v1 *object,
	uint32_t actions,
	uint32_t state)
{
	union wl_argument arguments[2];

	/* The arguments in wire order. */
	arguments[0].u = actions;
	arguments[1].u = state;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_EDIT_V1_SET_STATE, NULL, 0, 0, arguments);
}

/*
 * Sends kl_edit_manager_v1.destroy: the edit objects it gave stay.
 */
void
kl_edit_manager_v1_destroy(
	struct kl_edit_manager_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_EDIT_MANAGER_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_edit_manager_v1.get_edit and returns the window's new edit object.
 */
struct kl_edit_v1 *
kl_edit_manager_v1_get_edit(
	struct kl_edit_manager_v1 *object,
	struct xdg_toplevel *toplevel)
{
	union wl_argument arguments[2];
	struct wl_proxy *created;

	/* The new edit object's identity, then the window it belongs to. */
	arguments[0].n = 0;
	arguments[1].o = (struct wl_object *)toplevel;

	/* Queues the request together with the new proxy. */
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_EDIT_MANAGER_V1_GET_EDIT, &kl_edit_v1_interface, 1U, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new edit object. */
	return (struct kl_edit_v1 *)created;
}
