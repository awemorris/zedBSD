/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's router of the connections' packets (ws143-p005, see
 * router.h).
 *
 * The session calls it from btd_session_input only, after counting the
 * connection a Connection Complete or a Disconnection Complete makes or
 * ends; an owner it calls may send commands.
 */

#include "userland/base/bluetoothd/router.h"

#include <errno.h>
#include <string.h>
#include <uapi/bluetooth.h>

/* The commands the router sends: Disconnect, Reject Connection Request and the negative replies of the pairing. */
#define ROUTER_DISCONNECT		0x0406U
#define ROUTER_REJECT_CONNECTION	0x040aU
#define ROUTER_LINK_KEY_NEGATIVE	0x040cU
#define ROUTER_PIN_NEGATIVE		0x040eU
#define ROUTER_CONFIRM_NEGATIVE		0x042dU
#define ROUTER_PASSKEY_NEGATIVE		0x042fU
#define ROUTER_IO_NEGATIVE		0x0434U

/* The events the router routes (Core 5.4 Vol 4 Part E §7.7). */
#define ROUTER_EVENT_CONNECTED		0x03U
#define ROUTER_EVENT_REQUEST		0x04U
#define ROUTER_EVENT_DISCONNECTED	0x05U
#define ROUTER_EVENT_AUTHENTICATED	0x06U
#define ROUTER_EVENT_ENCRYPTION		0x08U
#define ROUTER_EVENT_KEY_CHANGED	0x09U
#define ROUTER_EVENT_REMOTE_FEATURES	0x0bU
#define ROUTER_EVENT_REMOTE_VERSION	0x0cU
#define ROUTER_EVENT_MODE		0x14U
#define ROUTER_EVENT_PIN		0x16U
#define ROUTER_EVENT_KEY_REQUEST	0x17U
#define ROUTER_EVENT_KEY_NOTIFICATION	0x18U
#define ROUTER_EVENT_MAX_SLOTS		0x1bU
#define ROUTER_EVENT_KEY_REFRESH	0x30U
#define ROUTER_EVENT_IO_REQUEST		0x31U
#define ROUTER_EVENT_IO_RESPONSE	0x32U
#define ROUTER_EVENT_CONFIRM		0x33U
#define ROUTER_EVENT_PASSKEY_REQUEST	0x34U
#define ROUTER_EVENT_SIMPLE_COMPLETE	0x36U
#define ROUTER_EVENT_SUPERVISION	0x38U
#define ROUTER_EVENT_PASSKEY_SHOWN	0x3bU
#define ROUTER_EVENT_KEYPRESS		0x3cU
#define ROUTER_EVENT_LE_META		0x3eU
#define ROUTER_EVENT_ENCRYPTION_V2	0x59U

/* LE's subevents the router routes: Connection Complete, Connection Update Complete, Read Remote Features Complete, LTK Request, Enhanced Connection Complete. */
#define ROUTER_LE_CONNECTED		0x01U
#define ROUTER_LE_UPDATED		0x03U
#define ROUTER_LE_FEATURES		0x04U
#define ROUTER_LE_LTK_REQUEST		0x05U
#define ROUTER_LE_ENHANCED		0x0aU

/* The reasons the router gives: an unacceptable address (a connection refused), pairing not allowed, a disconnection by the user. */
#define ROUTER_REASON_UNACCEPTABLE	0x0fU
#define ROUTER_REASON_NOT_ALLOWED	0x18U
#define ROUTER_REASON_USER		0x13U

/* The lengths of the events the router reads. */
#define ROUTER_CONNECTED_LENGTH		11U
#define ROUTER_REQUEST_LENGTH		10U
#define ROUTER_DISCONNECTED_LENGTH	4U
#define ROUTER_LE_CONNECTED_LENGTH	19U
#define ROUTER_LE_ENHANCED_LENGTH	31U

static void router_acl(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length);
static void router_event(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length);
static void router_connected(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length);
static void router_le(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length);
static void router_le_connected(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length);
static void router_disconnected(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length);
static void router_request(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length);
static void router_key_request(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length);
static void router_pairing_event(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length, size_t offset);
static void router_by_handle(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length, size_t offset);
static void router_take(struct btd_router *router, struct btd_session *session, const uint8_t *packet, size_t length, uint16_t handle, unsigned owner);
static unsigned router_owner_of(const struct btd_router *router, const uint8_t *address);
static int router_hid_wants(const struct btd_router *router, const uint8_t *address);
static void router_deliver(struct btd_router *router, unsigned owner, struct btd_session *session, const uint8_t *packet, size_t length);
static void router_end(struct btd_router *router, struct btd_session *session, uint16_t handle);
static void router_refuse(struct btd_router *router, struct btd_session *session, uint16_t opcode, const uint8_t *address, const uint8_t *more, size_t more_length);
static struct btd_route *router_find(struct btd_router *router, uint16_t handle);
static uint16_t router_handle_at(const uint8_t *bytes);

/*
 * Prepares the router of the pairing: no connection, no HID host.  The
 * caller makes it the session's handler (btd_router_handle).
 */
void
btd_router_init(
	struct btd_router *router,
	struct btd_pair *pair)
{
	/* Nothing routed yet. */
	memset(router, 0, sizeof(*router));
	router->pair = pair;
}

/*
 * Gives the router the HID host's hooks (phase005 section 4.1); members
 * left NULL answer no.
 */
void
btd_router_set_hid(
	struct btd_router *router,
	const struct btd_router_hid *hid)
{
	/* The hooks, copied. */
	router->hid = *hid;
}

/*
 * Takes a packet the session hands on (the session's handler): ACL data
 * goes to its connection's owner, an event to the owner of its connection
 * or its device, or is refused.
 */
void
btd_router_handle(
	void *context,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	struct btd_router *router;

	/* The router this handler serves. */
	router = context;

	/* ACL data of a connection. */
	if (length >= 1U && packet[0] == BT_PACKET_ACL) {
		router_acl(router, session, packet, length);
		return;
	}

	/* Anything but an event with its header is passed over. */
	if (length < 3U || packet[0] != BT_PACKET_EVENT) {
		router->ignored++;
		return;
	}

	/* Succeeded: the event, whose parameters' length the session checked. */
	router_event(router, session, packet, length);
}

/*
 * Makes an owner the owner of a connection (the HID host takes over the
 * pairing's, phase005 section 9.2).  Returns 0, or ENOSPC when a new
 * connection does not fit.
 */
int
btd_router_assign(
	struct btd_router *router,
	uint16_t handle,
	unsigned owner)
{
	struct btd_route *route;
	unsigned index;

	/* The connection's route, when it has one: its owner changes. */
	route = router_find(router, handle);
	if (route != NULL) {
		route->owner = owner;
		return 0;
	}

	/* A free route for a new connection. */
	for (index = 0U; index < BTD_LINKS_MAX; index++) {
		route = &router->routes[index];
		if (route->used)
			continue;

		/* Succeeded: the connection and its owner. */
		route->used = 1;
		route->handle = handle;
		route->owner = owner;
		return 0;
	}

	/* Every route is in use. */
	return ENOSPC;
}

/*
 * Tells the owner of a connection (BTD_OWNER_NONE for a connection the
 * router does not know).
 */
unsigned
btd_router_owner(
	const struct btd_router *router,
	uint16_t handle)
{
	unsigned index;

	/* The connection's route. */
	for (index = 0U; index < BTD_LINKS_MAX; index++) {
		if (router->routes[index].used && router->routes[index].handle == handle)
			return router->routes[index].owner;
	}

	/* Nobody's. */
	return BTD_OWNER_NONE;
}

/*
 * Forgets every connection (the controller went or was reset: no
 * Disconnection Complete comes, phase005 section 9.5).
 */
void
btd_router_clear(
	struct btd_router *router)
{
	/* No route. */
	memset(router->routes, 0, sizeof(router->routes));
}

/* Gives an ACL packet to its connection's owner; a packet of nobody's connection is passed over. */
static void
router_acl(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	uint16_t handle;
	unsigned owner;

	/* The packet type and the handle with its flags. */
	if (length < 5U) {
		router->ignored++;
		return;
	}

	/* The connection's owner. */
	handle = router_handle_at(packet + 1);
	owner = btd_router_owner(router, handle);
	if (owner == BTD_OWNER_NONE) {
		router->ignored++;
		return;
	}

	/* Succeeded: the owner's. */
	router_deliver(router, owner, session, packet, length);
}

/* Routes one event by its code. */
static void
router_event(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	/* Each event the router knows; anything else is the pairing's, which passes over what it does not know. */
	switch (packet[1]) {
	case ROUTER_EVENT_CONNECTED:
		router_connected(router, session, packet, length);
		break;
	case ROUTER_EVENT_DISCONNECTED:
		router_disconnected(router, session, packet, length);
		break;
	case ROUTER_EVENT_REQUEST:
		router_request(router, session, packet, length);
		break;
	case ROUTER_EVENT_KEY_REQUEST:
		router_key_request(router, session, packet, length);
		break;
	case ROUTER_EVENT_PIN:
	case ROUTER_EVENT_KEY_NOTIFICATION:
	case ROUTER_EVENT_IO_REQUEST:
	case ROUTER_EVENT_IO_RESPONSE:
	case ROUTER_EVENT_CONFIRM:
	case ROUTER_EVENT_PASSKEY_REQUEST:
	case ROUTER_EVENT_PASSKEY_SHOWN:
	case ROUTER_EVENT_KEYPRESS:
		/* The pairing's events, led by the device's address. */
		router_pairing_event(router, session, packet, length, 0U);
		break;
	case ROUTER_EVENT_SIMPLE_COMPLETE:
		/* Simple Pairing Complete: the status, then the address. */
		router_pairing_event(router, session, packet, length, 1U);
		break;
	case ROUTER_EVENT_AUTHENTICATED:
	case ROUTER_EVENT_ENCRYPTION:
	case ROUTER_EVENT_KEY_CHANGED:
	case ROUTER_EVENT_REMOTE_FEATURES:
	case ROUTER_EVENT_REMOTE_VERSION:
	case ROUTER_EVENT_MODE:
	case ROUTER_EVENT_KEY_REFRESH:
	case ROUTER_EVENT_ENCRYPTION_V2:
		/* A connection's event: the status, then the handle. */
		router_by_handle(router, session, packet, length, 1U);
		break;
	case ROUTER_EVENT_MAX_SLOTS:
	case ROUTER_EVENT_SUPERVISION:
		/* A connection's event led by its handle. */
		router_by_handle(router, session, packet, length, 0U);
		break;
	case ROUTER_EVENT_LE_META:
		router_le(router, session, packet, length);
		break;
	default:
		router_deliver(router, BTD_OWNER_PAIR, session, packet, length);
		break;
	}
}

/*
 * Takes BR/EDR's Connection Complete (status, handle, address, link type,
 * encryption): the device's owner gets it and owns the connection; a
 * connection nobody owns is ended, a failed one nobody waits for is passed
 * over.
 */
static void
router_connected(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	const uint8_t *parameters;
	uint16_t handle;
	unsigned owner;

	/* A whole event. */
	parameters = packet + 3;
	if (packet[2] < ROUTER_CONNECTED_LENGTH) {
		router->ignored++;
		return;
	}

	/* The device's owner. */
	handle = router_handle_at(parameters + 1);
	owner = router_owner_of(router, parameters + 3);

	/* A failure nobody waits for is passed over. */
	if (parameters[0] != 0U && owner == BTD_OWNER_NONE) {
		router->ignored++;
		return;
	}

	/* A connection that failed: its owner hears it. */
	if (parameters[0] != 0U) {
		router_deliver(router, owner, session, packet, length);
		return;
	}

	/* Succeeded: the connection is its owner's, or ended. */
	router_take(router, session, packet, length, handle, owner);
}

/* Routes LE's meta events: the connections' by their device or handle, the rest to the pairing. */
static void
router_le(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	/* A subevent at all. */
	if (packet[2] < 1U) {
		router->ignored++;
		return;
	}

	/* Each subevent the router knows; the others (the pairing's public key and DHKey among them) are the pairing's. */
	switch (packet[3]) {
	case ROUTER_LE_CONNECTED:
	case ROUTER_LE_ENHANCED:
		router_le_connected(router, session, packet, length);
		break;
	case ROUTER_LE_UPDATED:
	case ROUTER_LE_FEATURES:
		/* The subevent, the status, then the handle. */
		router_by_handle(router, session, packet, length, 2U);
		break;
	case ROUTER_LE_LTK_REQUEST:
		/* The subevent, then the handle. */
		router_by_handle(router, session, packet, length, 1U);
		break;
	default:
		router_deliver(router, BTD_OWNER_PAIR, session, packet, length);
		break;
	}
}

/*
 * Takes LE's Connection Complete or Enhanced Connection Complete
 * (subevent, status, handle, role, the device's address type and
 * address, ...).  A failed one carries no address when it ends a
 * cancelled connection: it goes to the pairing while one runs, else to
 * the HID host when it claims the address.
 */
static void
router_le_connected(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	const uint8_t *parameters;
	uint16_t handle;
	unsigned owner;
	size_t least;
	int active;

	/* A whole event of its subevent. */
	parameters = packet + 3;
	least = ROUTER_LE_CONNECTED_LENGTH;
	if (parameters[0] == ROUTER_LE_ENHANCED)
		least = ROUTER_LE_ENHANCED_LENGTH;
	if (packet[2] < least) {
		router->ignored++;
		return;
	}

	/* The device's owner. */
	handle = router_handle_at(parameters + 2);
	owner = router_owner_of(router, parameters + 6);

	/* A connection that failed: the pairing hears it while one runs (a cancelled one has no address), else its owner. */
	if (parameters[1] != 0U) {
		active = btd_pair_active(router->pair);
		if (active)
			owner = BTD_OWNER_PAIR;

		/* A failure nobody waits for is passed over. */
		if (owner == BTD_OWNER_NONE) {
			router->ignored++;
			return;
		}

		/* The owner hears it. */
		router_deliver(router, owner, session, packet, length);
		return;
	}

	/* Succeeded: the connection is its owner's, or ended. */
	router_take(router, session, packet, length, handle, owner);
}

/* Takes Disconnection Complete (status, handle, reason): its owner hears it, then the connection is forgotten. */
static void
router_disconnected(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	struct btd_route *route;
	uint16_t handle;
	unsigned owner;

	/* A whole event. */
	if (packet[2] < ROUTER_DISCONNECTED_LENGTH) {
		router->ignored++;
		return;
	}

	/* The connection's owner. */
	handle = router_handle_at(packet + 4);
	route = router_find(router, handle);
	if (route == NULL) {
		router->ignored++;
		return;
	}

	/* The owner hears it. */
	owner = route->owner;
	router_deliver(router, owner, session, packet, length);

	/* A disconnection that happened ends the route (the owner may have taken it over meanwhile: it is looked up again). */
	if (packet[3] != 0U)
		return;
	route = router_find(router, handle);
	if (route != NULL)
		route->used = 0;
}

/*
 * Takes Connection Request (address, class of device, link type): a
 * device the HID host wants goes to it; any other is refused with an
 * unacceptable address (pairing starts from this side only, design
 * section 6.5).
 */
static void
router_request(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	const uint8_t *address;
	uint8_t reason[1];
	int wanted;

	/* A whole event. */
	if (packet[2] < ROUTER_REQUEST_LENGTH) {
		router->ignored++;
		return;
	}

	/* A bonded HID device the HID host wants back. */
	address = packet + 3;
	wanted = router_hid_wants(router, address);
	if (wanted) {
		router_deliver(router, BTD_OWNER_HID, session, packet, length);
		return;
	}

	/* Succeeded: refused. */
	reason[0] = ROUTER_REASON_UNACCEPTABLE;
	router_refuse(router, session, ROUTER_REJECT_CONNECTION, address, reason, sizeof(reason));
}

/* Takes Link Key Request (address): the pairing's device to the pairing, a device the HID host wants to it, any other has no key. */
static void
router_key_request(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	const uint8_t *address;
	int ours;
	int wanted;

	/* A whole event. */
	if (packet[2] < BTD_ADDRESS_BYTES) {
		router->ignored++;
		return;
	}

	/* The pairing's device. */
	address = packet + 3;
	ours = btd_pair_owns(router->pair, address);
	if (ours) {
		router_deliver(router, BTD_OWNER_PAIR, session, packet, length);
		return;
	}

	/* A device the HID host wants: its stored key. */
	wanted = router_hid_wants(router, address);
	if (wanted) {
		router_deliver(router, BTD_OWNER_HID, session, packet, length);
		return;
	}

	/* Succeeded: no key for anyone else. */
	router_refuse(router, session, ROUTER_LINK_KEY_NEGATIVE, address, NULL, 0U);
}

/*
 * Takes an event of the pairing (its device's address at offset): the
 * pairing's device to the pairing; for any other device a question is
 * refused, a key notified is not stored and anything else is passed over
 * (phase005 section 9.6).
 */
static void
router_pairing_event(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length,
	size_t offset)
{
	const uint8_t *address;
	uint8_t reason[1];
	int ours;

	/* A whole address. */
	if ((size_t)packet[2] < offset + BTD_ADDRESS_BYTES) {
		router->ignored++;
		return;
	}

	/* The pairing's device. */
	address = packet + 3 + offset;
	ours = btd_pair_owns(router->pair, address);
	if (ours) {
		router_deliver(router, BTD_OWNER_PAIR, session, packet, length);
		return;
	}

	/* Any other device: each question refused, a key not stored. */
	switch (packet[1]) {
	case ROUTER_EVENT_IO_REQUEST:
		reason[0] = ROUTER_REASON_NOT_ALLOWED;
		router_refuse(router, session, ROUTER_IO_NEGATIVE, address, reason, sizeof(reason));
		break;
	case ROUTER_EVENT_CONFIRM:
		router_refuse(router, session, ROUTER_CONFIRM_NEGATIVE, address, NULL, 0U);
		break;
	case ROUTER_EVENT_PASSKEY_REQUEST:
		router_refuse(router, session, ROUTER_PASSKEY_NEGATIVE, address, NULL, 0U);
		break;
	case ROUTER_EVENT_PIN:
		router_refuse(router, session, ROUTER_PIN_NEGATIVE, address, NULL, 0U);
		break;
	case ROUTER_EVENT_KEY_NOTIFICATION:
		router->keys_dropped++;
		break;
	default:
		router->ignored++;
		break;
	}
}

/* Gives an event of a connection (its handle at offset in the parameters) to the connection's owner. */
static void
router_by_handle(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length,
	size_t offset)
{
	uint16_t handle;
	unsigned owner;

	/* A whole handle. */
	if ((size_t)packet[2] < offset + 2U) {
		router->ignored++;
		return;
	}

	/* The connection's owner. */
	handle = router_handle_at(packet + 3 + offset);
	owner = btd_router_owner(router, handle);
	if (owner == BTD_OWNER_NONE) {
		router->ignored++;
		return;
	}

	/* Succeeded: the owner's. */
	router_deliver(router, owner, session, packet, length);
}

/* Makes a new connection its owner's and hands it the event; a connection nobody owns, or that does not fit, is ended. */
static void
router_take(
	struct btd_router *router,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length,
	uint16_t handle,
	unsigned owner)
{
	int error;

	/* Nobody's: ended. */
	if (owner == BTD_OWNER_NONE) {
		router_end(router, session, handle);
		return;
	}

	/* The route; one that does not fit is ended. */
	error = btd_router_assign(router, handle, owner);
	if (error != 0) {
		router_end(router, session, handle);
		return;
	}

	/* Succeeded: the owner hears it. */
	router_deliver(router, owner, session, packet, length);
}

/* Tells who owns a device's connection: the pairing's device, the HID host's, or nobody's. */
static unsigned
router_owner_of(
	const struct btd_router *router,
	const uint8_t *address)
{
	int ours;
	int claimed;

	/* The pairing's device. */
	ours = btd_pair_owns(router->pair, address);
	if (ours)
		return BTD_OWNER_PAIR;

	/* No HID host. */
	if (router->hid.claims == NULL)
		return BTD_OWNER_NONE;

	/* A device the HID host paged or accepted. */
	claimed = router->hid.claims(router->hid.context, address);
	if (claimed)
		return BTD_OWNER_HID;

	/* Nobody's. */
	return BTD_OWNER_NONE;
}

/* Tells whether the HID host wants a bonded device that connects to bluetoothd. */
static int
router_hid_wants(
	const struct btd_router *router,
	const uint8_t *address)
{
	int wanted;

	/* No HID host. */
	if (router->hid.wants == NULL)
		return 0;

	/* The HID host's answer. */
	wanted = router->hid.wants(router->hid.context, address);
	if (wanted)
		return 1;

	/* Not wanted. */
	return 0;
}

/* Hands a packet to an owner (an owner without a handler passes it over). */
static void
router_deliver(
	struct btd_router *router,
	unsigned owner,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	/* The pairing's. */
	if (owner == BTD_OWNER_PAIR) {
		btd_pair_handle(router->pair, session, packet, length);
		return;
	}

	/* The HID host's, when there is one. */
	if (owner == BTD_OWNER_HID && router->hid.handle != NULL) {
		router->hid.handle(router->hid.context, session, packet, length);
		return;
	}

	/* Nobody to hear it. */
	router->ignored++;
}

/* Ends a connection nobody owns (Disconnect, by the user); a failure is the next packet's to show. */
static void
router_end(
	struct btd_router *router,
	struct btd_session *session,
	uint16_t handle)
{
	uint8_t disconnect[3];

	/* Disconnect: the connection is not acceptable. */
	router->ended++;
	disconnect[0] = (uint8_t)(handle & 0xffU);
	disconnect[1] = (uint8_t)(handle >> 8);
	disconnect[2] = ROUTER_REASON_USER;
	(void)btd_session_command(session, ROUTER_DISCONNECT, disconnect, sizeof(disconnect));
}

/* Sends a refusal led by an address (and more parameters); a failure is the next packet's to show. */
static void
router_refuse(
	struct btd_router *router,
	struct btd_session *session,
	uint16_t opcode,
	const uint8_t *address,
	const uint8_t *more,
	size_t more_length)
{
	uint8_t parameters[BTD_ADDRESS_BYTES + 1U];

	/* The address, then the rest (one byte at most: a reason). */
	router->refused++;
	memcpy(parameters, address, BTD_ADDRESS_BYTES);
	if (more_length > sizeof(parameters) - BTD_ADDRESS_BYTES)
		more_length = sizeof(parameters) - BTD_ADDRESS_BYTES;
	if (more_length != 0U)
		memcpy(parameters + BTD_ADDRESS_BYTES, more, more_length);

	/* Succeeded: sent (the controller reports a refusal it refused in its events). */
	(void)btd_session_command(session, opcode, parameters, BTD_ADDRESS_BYTES + more_length);
}

/* Finds a connection's route, or NULL. */
static struct btd_route *
router_find(
	struct btd_router *router,
	uint16_t handle)
{
	unsigned index;

	/* Each route in use. */
	for (index = 0U; index < BTD_LINKS_MAX; index++) {
		if (router->routes[index].used && router->routes[index].handle == handle)
			return &router->routes[index];
	}

	/* None. */
	return NULL;
}

/* Reads a connection handle (two bytes, least significant first, without the flags above its 12 bits). */
static uint16_t
router_handle_at(
	const uint8_t *bytes)
{
	unsigned value;

	/* The two bytes, then the handle's bits alone. */
	value = (unsigned)bytes[0] | ((unsigned)bytes[1] << 8);
	value &= 0x0fffU;

	/* Succeeded: the handle. */
	return (uint16_t)value;
}
