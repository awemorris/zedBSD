/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's HID host over BR/EDR (ws143-p005 i02c,
 * plan/ws143/phase005/phase.md sections 4.7 to 4.12 and 9.2 to 9.8): the
 * table of HID devices and each one's connection, from the page (or the
 * device's own connection, or the pairing's connection handed over) to
 * the open HID channels, whose input reports go to the kernel through
 * /dev/input/bridge as the device's /dev/input/eventN.
 *
 * A device is in the table while it has a record (FOLDER/<controller>/
 * <address>-bredr.hid, hidcache.c) beside its bond: it is wanted back
 * (bluetoothd pages it, or lets it connect) until the user disconnects it.
 * Its connection goes: paging, authenticating, encrypting (the bond's key,
 * 16 bytes), the SDP channel and the HID and PnP records, the control and
 * the interrupt channels, the input device made, open.  A device that
 * connects by itself and asks for its channels before the link is
 * encrypted is answered Pending until it is (section 9.8).
 *
 * LE (HOGP) is i03's; this file serves BR/EDR only.  It is the router's
 * HID host (router.h) and the pairing's handoff (pair.h).
 */

#ifndef BLUETOOTHD_HID_H
#define BLUETOOTHD_HID_H

#include "userland/base/bluetoothd/acl.h"
#include "userland/base/bluetoothd/hidcache.h"
#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/l2cap.h"
#include "userland/base/bluetoothd/router.h"
#include "userland/base/bluetoothd/sdp.h"
#include "userland/base/bluetoothd/session.h"

#include <stddef.h>
#include <stdint.h>

/* How many HID devices the table holds (session's 8 connections less the pairing's and one refused, phase005 Q14). */
#define BTD_HID_MAX		6U

/* How many reports wait for the input device (they came before it was made), and the longest kept. */
#define BTD_HID_QUEUE		32U
#define BTD_HID_QUEUE_BYTES	64U

/* The longest answer line of a connection. */
#define BTD_HID_ANSWER_MAX	256U

/* How long each part may take (milliseconds): the page, the security, the SDP, the channels, the whole; the handshake. */
#define BTD_HID_PAGE_MS		10000U
#define BTD_HID_SECURITY_MS	10000U
#define BTD_HID_SDP_MS		10000U
#define BTD_HID_CHANNELS_MS	10000U
#define BTD_HID_TOTAL_MS	60000U
#define BTD_HID_HANDSHAKE_MS	2000U
#define BTD_HID_CLOSE_MS	3000U

/* A setup the kernel refused with EINVAL (its copy failed, section 3) is written again so often, this far apart (review M10). */
#define BTD_HID_SETUP_TRIES	3U
#define BTD_HID_SETUP_MS	100U

/* The pages again of a wanted device that went: the first wait, the longest, and how many before it is paused (review S5). */
#define BTD_HID_RETRY_FIRST_MS	5000U
#define BTD_HID_RETRY_LAST_MS	60000U
#define BTD_HID_RETRIES		10U

/*
 * Where a device's connection is: none, paging (or accepting its own
 * connection), authenticating, encrypting, the SDP channel and records,
 * the HID channels, the handshake of SET_PROTOCOL, open, closing, and the
 * setup to be written again.
 */
#define BTD_HID_IDLE		0U
#define BTD_HID_PAGING		1U
#define BTD_HID_AUTHENTICATING	2U
#define BTD_HID_ENCRYPTING	3U
#define BTD_HID_SDP		4U
#define BTD_HID_CHANNELS	5U
#define BTD_HID_HANDSHAKE	6U
#define BTD_HID_OPEN		7U
#define BTD_HID_CLOSING		8U
#define BTD_HID_SETUP		9U

/* One report that came before the input device was made. */
struct btd_hid_report {
	size_t length;
	uint8_t bytes[BTD_HID_QUEUE_BYTES];
};

/*
 * One HID device of the table: its address, its record, where its
 * connection is and what it has open, the reports waiting, its counters,
 * and whether it is wanted back.  A slot is in use while used is set; the
 * device's bridge descriptor is open only while its input device exists.
 */
struct btd_hid_device {
	int used;
	uint8_t address[BTD_ADDRESS_BYTES];
	struct btd_hidcache record;
	int have_descriptor;

	/* The connection: its state, whether the device made it, its handle, its security. */
	unsigned state;
	int inbound;
	int connected;
	uint16_t handle;
	int authenticating;
	int encrypted;
	unsigned key_size;

	/* The connection's frames and channels: SDP's, the control's, the interrupt's (0: none), and the query. */
	struct btd_reassembly reassembly;
	struct btd_l2cap l2cap;
	uint16_t sdp_cid;
	uint16_t control_cid;
	uint16_t interrupt_cid;
	struct btd_sdp sdp;
	int pnp_asked;

	/* The input device: the bridge's descriptor (-1: none), its nodes, and the setup's writes refused so far. */
	int bridge;
	int32_t event;
	int32_t touch_event;
	unsigned setup_tries;

	/* The reports that came before the input device. */
	unsigned queued;
	struct btd_hid_report queue[BTD_HID_QUEUE];

	/* The deadlines: the state's and the whole connection's (0: none). */
	uint64_t state_deadline;
	uint64_t total_deadline;

	/* Wanted back (the user did not disconnect it), the pages again, and whether they stopped. */
	int wanted;
	unsigned retries;
	uint64_t retry_at;
	int paused;

	/* Whether a client waits for the connection's end (CONNECT), and why the last connection ended. */
	int asked;
	const char *last;
	uint64_t since_ms;

	/* The counters: reports passed on, refused by the kernel, too long, dropped from the queue. */
	unsigned reports;
	unsigned malformed;
	unsigned oversize;
	unsigned dropped;
};

/*
 * What the daemon gives the HID host: the bridge's opens (the privileged
 * parent's, privsep.c), and the end of a connection a client asked for
 * ("CONNECTED ..." or "ERROR WHY").
 */
struct btd_hid_hooks {
	void *context;
	int (*open_bridge)(void *context, int *descriptor);
	void (*answer)(void *context, const uint8_t *address, const char *line);
};

/*
 * The HID host of a controller's session: the session, the bonds' folder,
 * the router, the hooks, the devices, and whether pages may start (not
 * while a pairing or a scan runs, review B7).  It lives in the daemon for
 * the daemon's life.
 */
struct btd_hid {
	struct btd_session *session;
	const char *keys_folder;
	struct btd_router *router;
	struct btd_hid_hooks hooks;
	struct btd_hid_device devices[BTD_HID_MAX];
	int held;
	int page_scan;
	unsigned refused;
};

void btd_hid_init(struct btd_hid *hid, struct btd_session *session, const char *keys_folder, struct btd_router *router, const struct btd_hid_hooks *hooks);
void btd_hid_refresh(struct btd_hid *hid);
int btd_hid_connect(struct btd_hid *hid, const uint8_t *address, char *answer, size_t size);
int btd_hid_disconnect(struct btd_hid *hid, const uint8_t *address);
void btd_hid_forget(struct btd_hid *hid, const uint8_t *address);
void btd_hid_release(struct btd_hid *hid, const uint8_t *address);
void btd_hid_status(const struct btd_hid *hid, unsigned index, char *line, size_t size);
unsigned btd_hid_open_count(const struct btd_hid *hid);
int btd_hid_busy(const struct btd_hid *hid, const uint8_t *address);
void btd_hid_hold(struct btd_hid *hid, int held);
void btd_hid_tick(struct btd_hid *hid, uint64_t now);
uint64_t btd_hid_deadline(const struct btd_hid *hid);
void btd_hid_lost(struct btd_hid *hid);
int btd_hid_handoff(void *context, const uint8_t *address, unsigned type, uint16_t handle, const struct btd_bond *bond);
int btd_hid_wants(void *context, const uint8_t *address);
int btd_hid_claims(void *context, const uint8_t *address);
void btd_hid_handle(void *context, struct btd_session *session, const uint8_t *packet, size_t length);
int btd_hid_policy_after_pair(uint32_t device_class);
int btd_hid_policy_after_disconnect(void);

#endif
