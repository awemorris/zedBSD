/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's connections and pairing (ws143-p004, plan section 10.3):
 * one pairing at a time, BR/EDR's Secure Simple Pairing (the controller
 * does the cryptography; bluetoothd answers its events) and LE's Security
 * Manager (smp.c) carried over HCI and ACL, the keys checked and stored,
 * then the connection ended.  It is the session's handler: it gets the
 * connections' events and ACL packets from btd_session_input, and refuses
 * every pairing it did not start.
 *
 * The daemon gives it two hooks: one to ask the agent (a number to confirm
 * or to show), one to hear the pairing's end (a PAIRED or ERROR line).
 */

#ifndef BLUETOOTHD_PAIR_H
#define BLUETOOTHD_PAIR_H

#include "userland/base/bluetoothd/acl.h"
#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/l2cap.h"
#include "userland/base/bluetoothd/session.h"
#include "userland/base/bluetoothd/smp.h"

#include <stddef.h>
#include <stdint.h>

/* What the agent is asked: to confirm a number (yes or no), or to show a passkey the other device types. */
#define BTD_PAIR_ASK_CONFIRM	1U
#define BTD_PAIR_ASK_PASSKEY	2U

/* The longest answer line of a pairing. */
#define BTD_PAIR_ANSWER_MAX	192U

/* How long each part may take (milliseconds). */
#define BTD_PAIR_CONNECT_MS	10000U
#define BTD_PAIR_TOTAL_MS	60000U
#define BTD_PAIR_SMP_MS		30000U
#define BTD_PAIR_AGENT_MS	30000U
#define BTD_PAIR_PROBE_MS	2000U
#define BTD_PAIR_CLOSE_MS	3000U

/* Asks the agent (kind BTD_PAIR_ASK_*, the number); a confirmation is answered with btd_pair_answer. */
typedef void (*btd_pair_ask_fn)(void *context, unsigned kind, uint32_t number);

/* Tells the pairing's end: "PAIRED ..." or "ERROR WHY". */
typedef void (*btd_pair_done_fn)(void *context, const char *answer);

/*
 * The pairing of one device, and the hooks of the daemon.  It lives in the
 * daemon for the controller's session; a pairing fills it from
 * btd_pair_start to its end.
 */
struct btd_pair {
	/* The controller's session, the bonds' folder, the hooks and the random source. */
	struct btd_session *session;
	const char *keys_folder;
	btd_pair_ask_fn ask;
	btd_pair_done_fn done;
	void *context;
	btd_random_fn random;
	void *random_context;

	/* Where the pairing is (0: none), the device, and whether an agent can answer. */
	unsigned state;
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int agent;

	/* The connection: its handle, whether it is up, and the error a cancelled connection ends with. */
	uint16_t handle;
	int connected;
	const char *pending_error;

	/* The deadlines: the state's, the whole pairing's, the Security Manager's and the agent's (0: none). */
	uint64_t state_deadline;
	uint64_t total_deadline;
	uint64_t smp_deadline;
	uint64_t agent_deadline;
	int asked;

	/* BR/EDR: the bond stored before, whether its key was used, the other side's IO, and the key the controller gave. */
	int have_stored;
	struct btd_bond stored;
	int used_stored;
	int have_peer_io;
	uint8_t peer_io;
	int confirmed;
	int have_link_key;
	uint8_t link_key[16];
	uint8_t link_key_type;
	const char *refusal;
	int probed;
	unsigned key_size;

	/* LE: the Security Manager. */
	struct btd_smp smp;

	/* The connection's frames being put together, and its signalling. */
	struct btd_reassembly reassembly;
	struct btd_l2cap l2cap;

	/* The answer of the end, and what was refused or passed over. */
	char answer[BTD_PAIR_ANSWER_MAX];
	unsigned refused;
	unsigned ignored;
};

void btd_pair_init(struct btd_pair *pair, struct btd_session *session, const char *keys_folder, btd_pair_ask_fn ask, btd_pair_done_fn done, void *context, btd_random_fn random, void *random_context);
int btd_pair_start(struct btd_pair *pair, const uint8_t *address, unsigned type, int agent);
void btd_pair_handle(void *context, struct btd_session *session, const uint8_t *packet, size_t length);
void btd_pair_answer(struct btd_pair *pair, int accepted);
void btd_pair_tick(struct btd_pair *pair, uint64_t now);
uint64_t btd_pair_deadline(const struct btd_pair *pair);
void btd_pair_stop(struct btd_pair *pair, const char *why);
void btd_pair_lost(struct btd_pair *pair);
int btd_pair_active(const struct btd_pair *pair);

#endif
