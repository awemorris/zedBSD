/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's phone link, the transport part (ws197-p002,
 * plan/ws197/phase002/phase.md sections 7.4, 7.5 and 11): one phone's ACL
 * link that a phone's pairing hands over (PAIR ... phone=1), its L2CAP
 * channels, an SDP server on each SDP channel the phone opens, SDP queries
 * of the phone's records, one RFCOMM session (the phone's or bluetoothd's)
 * and the frames it sends, queued within the link's share of the
 * controller's buffers (section 4.2).
 *
 * The profiles (MAP, PBAP, HFP) come in later Phases.  This Phase has a
 * probe for root (PHONE PROBE): SDP finds the phone's MAS or PSE, RFCOMM
 * opens a DLC to it, OBEX connects, gets a folder listing and disconnects;
 * the answer says how each step went and how many bytes came, never what
 * they were (p001 R22).  The phone link does not take a phone that
 * connects by itself in this Phase (its authentication is p003's).
 *
 * Without system calls besides the session's; the host tests build it.
 */

#ifndef BLUETOOTHD_PHONE_H
#define BLUETOOTHD_PHONE_H

#include "userland/base/bluetoothd/acl.h"
#include "userland/base/bluetoothd/hid.h"
#include "userland/base/bluetoothd/l2cap.h"
#include "userland/base/bluetoothd/obex.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/rfcomm.h"
#include "userland/base/bluetoothd/router.h"
#include "userland/base/bluetoothd/sdp.h"
#include "userland/base/bluetoothd/sdps.h"
#include "userland/base/bluetoothd/session.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The phone link's states: no phone, a link handed over and ready, its disconnection asked for. */
#define BTD_PHONE_NONE		0U
#define BTD_PHONE_READY		1U
#define BTD_PHONE_CLOSING	2U

/* How many frames wait for the session, and how many SDP channels the phone may open at a time. */
#define BTD_PHONE_QUEUE		16U
#define BTD_PHONE_SDP_SERVERS	2U

/* How many HID devices may be connected while a phone is (section 7.5: the session's 8 links less pairing, refusal and the phone). */
#define BTD_PHONE_HID_LIMIT	5U

/* The longest answer line of a probe, and how long a whole probe may take (milliseconds). */
#define BTD_PHONE_ANSWER_MAX	192U
#define BTD_PHONE_PROBE_MS	30000U

/* The most of a probe's folder listing taken (its bytes are counted, not kept). */
#define BTD_PHONE_LISTING_MAX	262144U

/* The steps of a probe: none, SDP's channel opening, SDP's query, RFCOMM's channel and DLC, OBEX's Connect, Get and Disconnect, the DLC's close. */
#define BTD_PHONE_PROBE_NONE		0U
#define BTD_PHONE_PROBE_SDP_CHANNEL	1U
#define BTD_PHONE_PROBE_SDP		2U
#define BTD_PHONE_PROBE_RFCOMM		3U
#define BTD_PHONE_PROBE_CONNECT		4U
#define BTD_PHONE_PROBE_GET		5U
#define BTD_PHONE_PROBE_DISCONNECT	6U

/* Tells the end of a probe: "PROBE ..." or "ERROR WHY". */
typedef void (*btd_phone_answer_fn)(void *context, const char *line);

/* One L2CAP frame waiting for the session: its channel (the phone's end of it) and payload. */
struct btd_phone_frame {
	uint16_t cid;
	size_t length;
	uint8_t bytes[BTD_L2CAP_MAX];
};

/* The SDP server of one SDP channel the phone opened (cid 0: the slot is free). */
struct btd_phone_sdps {
	uint16_t cid;
	struct btd_sdps server;
};

/*
 * A probe under way (step BTD_PHONE_PROBE_NONE: none): the service class
 * asked for, the server channel SDP found, the DLC, the bytes the listing
 * gave, the response codes of OBEX's steps, its deadline, and the answer.
 */
struct btd_phone_probe {
	unsigned step;
	uint16_t uuid;
	unsigned channel;
	unsigned dlci;
	size_t bytes;
	uint8_t connect_code;
	uint8_t get_code;
	uint8_t disconnect_code;
	uint64_t deadline;
};

/*
 * The phone link of a controller's session: the session, the router, the
 * HID host (whose connections it limits while a phone is there), the SDP
 * records offered, the answer hook; and the one phone: its state, device,
 * handle, the uid that paired it, its channels and frame, the SDP servers,
 * the SDP query, the RFCOMM session and its channel, the OBEX connection
 * of a probe, and the frames waiting.  It lives in the daemon for the
 * daemon's life; a phone fills it from the handoff to the link's end.
 */
struct btd_phone {
	struct btd_session *session;
	struct btd_router *router;
	struct btd_hid *hid;
	const struct btd_sdps_db *db;
	btd_phone_answer_fn answer;
	void *answer_context;

	/*
	 * The phone: where its link is, its device, its handle, who paired
	 * it, the link's encryption, and whether the scan knew no Class of
	 * Device for it (taken as a phone because the user asked, section
	 * 7.4; the daemon logs it).
	 */
	unsigned state;
	uint8_t address[BTD_ADDRESS_BYTES];
	uint16_t handle;
	uid_t uid;
	int encrypted;
	unsigned key_size;
	int class_unknown;

	/* The link's channels, its frame being put together, and the channel the last frame began on (0: none). */
	struct btd_l2cap l2cap;
	struct btd_reassembly reassembly;
	uint16_t frame_cid;

	/* The SDP servers, the query of the phone's records (its channel: 0 for none) and its transaction. */
	struct btd_phone_sdps sdps[BTD_PHONE_SDP_SERVERS];
	uint16_t sdp_cid;
	uint16_t sdp_transaction;
	struct btd_sdp sdp;

	/*
	 * The RFCOMM session (its channel: 0 for none; active once the channel
	 * opened), and our channel's requests: how many went, whether the
	 * phone just refused one, and when the next goes (0: none waits).
	 */
	uint16_t rfcomm_cid;
	int rfcomm_active;
	struct btd_rfcomm rfcomm;
	unsigned rfcomm_tries;
	int rfcomm_refused;
	uint64_t rfcomm_retry_at;

	/* A probe and its OBEX connection. */
	struct btd_phone_probe probe;
	struct btd_obex obex;

	/* The frames waiting for room in the session, a ring: the oldest's slot and how many. */
	unsigned queue_first;
	unsigned queue_count;
	struct btd_phone_frame queue[BTD_PHONE_QUEUE];

	/* The counters: frames dropped for a full queue, notices, links refused. */
	unsigned queue_dropped;
	unsigned notices;
	unsigned refused;
};

void btd_phone_init(struct btd_phone *phone, struct btd_session *session, struct btd_router *router, struct btd_hid *hid, const struct btd_sdps_db *db, btd_phone_answer_fn answer, void *context);
int btd_phone_handoff(void *context, const struct btd_pair_handoff *handoff, const char **why);
int btd_phone_wants(void *context, const uint8_t *address);
int btd_phone_claims(void *context, const uint8_t *address);
void btd_phone_handle(void *context, struct btd_session *session, const uint8_t *packet, size_t length);
void btd_phone_pump(struct btd_phone *phone);
void btd_phone_tick(struct btd_phone *phone, uint64_t now);
uint64_t btd_phone_deadline(const struct btd_phone *phone);
void btd_phone_lost(struct btd_phone *phone);
int btd_phone_probe(struct btd_phone *phone, const uint8_t *address, uint16_t uuid, uint64_t now);
int btd_phone_drop(struct btd_phone *phone, const uint8_t *address);
int btd_phone_owns(const struct btd_phone *phone, uint16_t handle);

#endif
