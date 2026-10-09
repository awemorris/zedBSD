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
 *
 * ws197-p002 (plan/ws197/phase002/phase.md section 5): the phone link is a
 * third owner after the pairing and the HID host; a Connection Request of
 * a synchronous link (SCO, eSCO) is refused with Reject Synchronous
 * Connection Request until the calls' audio exists, and only ACL
 * connections are owned; the session's notices of dropped packets go to
 * the owners, and a lost connection event makes the router check its
 * routes against the session's connections; the BR/EDR ACL Connection
 * Completes end the link manager's page of their device.
 */

#ifndef BLUETOOTHD_ROUTER_H
#define BLUETOOTHD_ROUTER_H

#include "userland/base/bluetoothd/linkmgr.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/session.h"

#include <stddef.h>
#include <stdint.h>

/* Who owns a connection: nobody, the pairing, the HID host, the phone link. */
#define BTD_OWNER_NONE		0U
#define BTD_OWNER_PAIR		1U
#define BTD_OWNER_HID		2U
#define BTD_OWNER_PHONE		3U

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

/*
 * What the phone link tells the router (phone.c), as the HID host does:
 * whether it wants a bonded phone that connects, whether a connection to
 * a device is its own, and the handler of its connections' packets.
 */
struct btd_router_phone {
	void *context;
	int (*wants)(void *context, const uint8_t *address);
	int (*claims)(void *context, const uint8_t *address);
	btd_handler_fn handle;
};

/*
 * One connection and its owner, its device's address (zero when the route
 * was made without one), and whether its link is encrypted with AES-CCM
 * (a synchronous link to that device is refused with reason 0x0E).
 */
struct btd_route {
	int used;
	uint16_t handle;
	unsigned owner;
	uint8_t address[BTD_ADDRESS_BYTES];
	int aes_ccm;
};

/*
 * The router of a controller's session: the pairing, the HID host's and
 * the phone link's hooks, the link manager (NULL: none), the connections'
 * owners, and what it refused or passed over.  It lives in the daemon for
 * the daemon's life; the routes are emptied when the controller goes
 * (btd_router_clear).  reconcile_due says a lost connection event waits
 * for the session's queue to empty before the routes are checked (the
 * connection events queued after the loss are counted by the session
 * already but not yet routed).
 */
struct btd_router {
	struct btd_pair *pair;
	struct btd_router_hid hid;
	struct btd_router_phone phone;
	struct btd_linkmgr *linkmgr;
	struct btd_route routes[BTD_LINKS_MAX];
	int reconcile_due;
	unsigned refused;
	unsigned keys_dropped;
	unsigned ignored;
	unsigned ended;
	unsigned synchronous_refused;
	unsigned synchronous_ignored;
	unsigned notices;
	unsigned reconciled;
};

void btd_router_init(struct btd_router *router, struct btd_pair *pair);
void btd_router_set_hid(struct btd_router *router, const struct btd_router_hid *hid);
void btd_router_set_phone(struct btd_router *router, const struct btd_router_phone *phone);
void btd_router_set_linkmgr(struct btd_router *router, struct btd_linkmgr *linkmgr);
void btd_router_reconcile(struct btd_router *router, struct btd_session *session);
void btd_router_handle(void *context, struct btd_session *session, const uint8_t *packet, size_t length);
int btd_router_assign(struct btd_router *router, uint16_t handle, unsigned owner);
unsigned btd_router_owner(const struct btd_router *router, uint16_t handle);
void btd_router_clear(struct btd_router *router);

#endif
