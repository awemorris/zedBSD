/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Describes and marshals wl_touch version 5 (WS079 p013): the fingers of a
 * touch screen.
 *
 * The request (release, since 3) and the events down (`uuoiff`), up
 * (`uui`), motion (`uiff`), frame and cancel follow the pinned Wayland
 * 1.23.1 description (userland/desktop/libwayland/API-PROVENANCE.md).  shape and
 * orientation are version 6 and are not described; the compositor offers
 * wl_seat version 5.  The events reach listeners through the generic
 * dispatch (event.c).
 */

#include "internal.h"

/* The argument types of every wl_touch message whose arguments name no interface (at most six). */
static const struct wl_interface *wl_touch_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The arguments of down: a serial, the time, the surface, the finger's number and its place. */
static const struct wl_interface *wl_touch_down_types[] = {
	NULL,
	NULL,
	&wl_surface_interface,
	NULL,
	NULL,
	NULL,
};

/* The requests of wl_touch, in wire opcode order. */
static const struct wl_message wl_touch_requests[] = {
	{ "release", "3", NULL },
};

/* The events of wl_touch up to version 5, in wire opcode order. */
static const struct wl_message wl_touch_events[] = {
	{ "down", "uuoiff", wl_touch_down_types },
	{ "up", "uui", wl_touch_plain_types },
	{ "motion", "uiff", wl_touch_plain_types },
	{ "frame", "", NULL },
	{ "cancel", "", NULL },
};

/* The immutable wl_touch description. */
const struct wl_interface wl_touch_interface = {
	"wl_touch", 5, 1, wl_touch_requests,
	5, wl_touch_events
};

/*
 * Sends the wl_seat.get_touch request.
 */
struct wl_touch *
wl_seat_get_touch(
	struct wl_seat *wl_seat)
{
	union wl_argument arguments[1];
	struct wl_proxy *created;
	uint32_t version;

	/* The child inherits the seat's negotiated version, as the protocol requires. */
	version = wl_proxy_get_version((struct wl_proxy *)wl_seat);

	/* The new_id slot is filled with the identity allocated for the child. */
	arguments[0].n = 0;
	created = wl_proxy_marshal_array_flags((struct wl_proxy *)wl_seat, WL_SEAT_GET_TOUCH, &wl_touch_interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new protocol proxy. */
	return (struct wl_touch *)created;
}

/*
 * Installs the listener for wl_touch events.
 */
int
wl_touch_add_listener(
	struct wl_touch *wl_touch,
	const struct wl_touch_listener *listener,
	void *data)
{
	int error;

	/* Associates the callbacks with the proxy event stream. */
	error = wl_proxy_add_listener((struct wl_proxy *)wl_touch, (void (**)(void))listener, data);
	if (error != 0)
		return error;

	/* Succeeded: subsequent events use this listener. */
	return 0;
}

/*
 * Sends the wl_touch.release request and drops the local proxy.
 */
void
wl_touch_release(
	struct wl_touch *wl_touch)
{
	/* Queues the destructor request; the proxy is destroyed with it. */
	wl_proxy_marshal_array_flags((struct wl_proxy *)wl_touch, WL_TOUCH_RELEASE, NULL, 0, WL_MARSHAL_FLAG_DESTROY, NULL);
}

/*
 * Drops the local wl_touch proxy without telling the compositor.
 */
void
wl_touch_destroy(
	struct wl_touch *wl_touch)
{
	/* Suppresses future callbacks; the compositor keeps its object alive. */
	wl_proxy_destroy((struct wl_proxy *)wl_touch);
}

/*
 * Associates client state with the wl_touch proxy.
 */
void
wl_touch_set_user_data(
	struct wl_touch *wl_touch,
	void *data)
{
	/* Uses the common proxy ownership and synchronization contract. */
	wl_proxy_set_user_data((struct wl_proxy *)wl_touch, data);
}

/*
 * Obtains client state from the wl_touch proxy.
 */
void *
wl_touch_get_user_data(
	struct wl_touch *wl_touch)
{
	void *answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_user_data((struct wl_proxy *)wl_touch);

	/* Succeeded: reports the client state. */
	return answer;
}

/*
 * Obtains the negotiated version of the wl_touch proxy.
 */
uint32_t
wl_touch_get_version(
	struct wl_touch *wl_touch)
{
	uint32_t answer;

	/* Uses the common proxy ownership and synchronization contract. */
	answer = wl_proxy_get_version((struct wl_proxy *)wl_touch);

	/* Succeeded: reports the negotiated version. */
	return answer;
}
