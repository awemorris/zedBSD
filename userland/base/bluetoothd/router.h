/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's router of the connections' packets (ws143-p005,
 * plan/ws143/phase005/phase.md sections 4.1 and 9.6): the session's
 * handler, which gives each event and ACL packet to the connection's
 * owner, the pairing or the HID host, and refuses what nobody owns.
 *
 * A connection's owner is decided on its Connection Complete (LE's too) by
 * the device's address: the pairing's device goes to the pairing, a device
 * the HID host connects or accepts goes to the HID host, and any other
 * connection is ended.  The events of the pairing (IO Capability, the
 * confirmation, the passkey, the PIN, the link key's notification) go to
 * the pairing only while it pairs that device; any other is refused, also
 * for a bonded HID device (no pairing outside the pairing's mode, design
 * section 6.5).  A device connecting to bluetoothd and the controller's
 * Link Key Request go to the HID host when it wants that device, to the
 * pairing for its device, and are refused otherwise.
 *
 * The router sends only the refusals; the owners send everything else.
 */

#ifndef BLUETOOTHD_ROUTER_H
#define BLUETOOTHD_ROUTER_H

#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/session.h"

#include <stddef.h>
#include <stdint.h>

/* Who owns a connection: nobody, the pairing, the HID host. */
#define BTD_OWNER_NONE		0U
#define BTD_OWNER_PAIR		1U
#define BTD_OWNER_HID		2U

/*
 * What the HID host tells the router (hid.c).  wants says whether the HID
 * host wants a bonded device that connects to bluetoothd (its Connection
 * Request and its Link Key Request go to the HID host); claims says
 * whether a connection to a device is the HID host's (it paged or accepted
 * it); handle takes the packets of the HID host's connections.  A member
 * that is NULL answers no (the daemon without a HID host).
 */
struct btd_router_hid {
	void *context;
	int (*wants)(void *context, const uint8_t *address);
	int (*claims)(void *context, const uint8_t *address);
	btd_handler_fn handle;
};

/* One connection and its owner. */
struct btd_route {
	int used;
	uint16_t handle;
	unsigned owner;
};

/*
 * The router of a controller's session: the pairing, the HID host's
 * hooks, the connections' owners, and what it refused or passed over.  It
 * lives in the daemon for the daemon's life; the routes are emptied when
 * the controller goes (btd_router_clear).
 */
struct btd_router {
	struct btd_pair *pair;
	struct btd_router_hid hid;
	struct btd_route routes[BTD_LINKS_MAX];
	unsigned refused;
	unsigned keys_dropped;
	unsigned ignored;
	unsigned ended;
};

void btd_router_init(struct btd_router *router, struct btd_pair *pair);
void btd_router_set_hid(struct btd_router *router, const struct btd_router_hid *hid);
void btd_router_handle(void *context, struct btd_session *session, const uint8_t *packet, size_t length);
int btd_router_assign(struct btd_router *router, uint16_t handle, unsigned owner);
unsigned btd_router_owner(const struct btd_router *router, uint16_t handle);
void btd_router_clear(struct btd_router *router);

#endif
