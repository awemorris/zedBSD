/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals zdesktop's input method status protocol
 * (kl_ime_status_manager_v1 and kl_ime_status_v1, version 2;
 * ws095-p004, plan/ws095/design.md section 8).  Only the input method
 * zdesktop starts may bind the manager.  Version 2 (ws166-p002) adds the
 * on-screen keyboard's predictions: zdesktop asks for the words a reading
 * starts (predict), the input method answers them (predictions), and the
 * word chosen is learned (learn).
 */

#include "internal.h"

#include "userland/desktop/libwayland/keiland-ime-status-v1-client-protocol.h"

/* The argument types of every message whose arguments name no interface (at most 2). */
static const struct wl_interface *ime_status_plain_types[] = {
	NULL,
	NULL,
};

/* The requests of kl_ime_status_v1, in wire opcode order. */
static const struct wl_message ime_status_requests[] = {
	{ "destroy", "", NULL },
	{ "language", "ss", ime_status_plain_types },
	{ "composing", "u", ime_status_plain_types },
	{ "predictions", "2us", ime_status_plain_types },
};

/* The events of kl_ime_status_v1, in wire opcode order. */
static const struct wl_message ime_status_events[] = {
	{ "next", "", NULL },
	{ "select", "s", ime_status_plain_types },
	{ "predict", "2us", ime_status_plain_types },
	{ "learn", "2ss", ime_status_plain_types },
};

/* The immutable kl_ime_status_v1 description. */
const struct wl_interface kl_ime_status_v1_interface = {
	"kl_ime_status_v1", 2, 4, ime_status_requests,
	4, ime_status_events
};

/* The arguments of kl_ime_status_manager_v1.get_status. */
static const struct wl_interface *ime_status_manager_get_status_types[] = {
	&kl_ime_status_v1_interface,
};

/* The requests of kl_ime_status_manager_v1, in wire opcode order. */
static const struct wl_message ime_status_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_status", "n", ime_status_manager_get_status_types },
};

/* The immutable kl_ime_status_manager_v1 description. */
const struct wl_interface kl_ime_status_manager_v1_interface = {
	"kl_ime_status_manager_v1", 2, 2, ime_status_manager_requests,
	0, NULL
};

/*
 * Installs a listener of kl_ime_status_v1 (next, select).
 */
int
kl_ime_status_v1_add_listener(
	struct kl_ime_status_v1 *object,
	const struct kl_ime_status_v1_listener *listener,
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
 * Sends kl_ime_status_v1.destroy: the status is gone.
 */
void
kl_ime_status_v1_destroy(
	struct kl_ime_status_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_IME_STATUS_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_ime_status_v1.language: the language chosen now ("direct", "ja") and its short label.
 */
void
kl_ime_status_v1_language(
	struct kl_ime_status_v1 *object,
	const char *id,
	const char *label)
{
	union wl_argument arguments[2];

	/* The arguments in wire order. */
	arguments[0].s = id;
	arguments[1].s = label;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_IME_STATUS_V1_LANGUAGE, NULL, 0, 0, arguments);
}

/*
 * Sends kl_ime_status_v1.composing: whether text is being composed (1) or not (0).
 */
void
kl_ime_status_v1_composing(
	struct kl_ime_status_v1 *object,
	uint32_t composing)
{
	union wl_argument arguments[1];

	/* The arguments in wire order. */
	arguments[0].u = composing;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_IME_STATUS_V1_COMPOSING, NULL, 0, 0, arguments);
}

/*
 * Sends kl_ime_status_v1.predictions (version 2): the answer to the
 * predict event of the serial, the words one a line, each "WORD\tREADING"
 * (an empty list for none).
 */
void
kl_ime_status_v1_predictions(
	struct kl_ime_status_v1 *object,
	uint32_t serial,
	const char *list)
{
	union wl_argument arguments[2];

	/* The arguments in wire order. */
	arguments[0].u = serial;
	arguments[1].s = list;
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_IME_STATUS_V1_PREDICTIONS, NULL, 0, 0, arguments);
}

/*
 * Sends kl_ime_status_manager_v1.destroy: the status it gave stays.
 */
void
kl_ime_status_manager_v1_destroy(
	struct kl_ime_status_manager_v1 *object)
{
	/* Queues the destructor and retires the proxy. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_IME_STATUS_MANAGER_V1_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Sends kl_ime_status_manager_v1.get_status: the status object.
 */
struct kl_ime_status_v1 *
kl_ime_status_manager_v1_get_status(
	struct kl_ime_status_manager_v1 *object)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;

	/* The arguments in wire order; the status has the manager's version (version 2 has the predictions, ws166-p002). */
	arguments[0].n = 0;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)object, KL_IME_STATUS_MANAGER_V1_GET_STATUS, &kl_ime_status_v1_interface, wl_proxy_get_version((struct wl_proxy *)object), 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new object. */
	return (struct kl_ime_status_v1 *)created;
}
