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
 * then the connection ended.  It gets the connections' events and ACL
 * packets of its device from the router (router.c, ws143-p005), which
 * refuses every pairing it did not start.
 *
 * The daemon gives it two hooks: one to ask the agent (a number to confirm
 * or to show), one to hear the pairing's end (a PAIRED or ERROR line); and
 * may give a third (ws143-p005, phase005 section 9.2) that takes over the
 * connection of a pairing that succeeded instead of ending it.
 *
 * ws197-p002 (plan/ws197/phase002/phase.md section 7): a pairing may be
 * asked for a phone (PAIR ... phone=1, BR/EDR only).  It then holds the
 * phone's SDP and RFCOMM channels Pending, refuses a stored key that is
 * not authenticated, and offers the connection to a fourth hook, the phone
 * link's, before the HID host's.
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
#include <sys/types.h>

/*
 * What the agent is asked: to confirm a number (yes or no), to show a
 * passkey the other device types, or to agree to a pairing without a
 * number (Just Works, yes or no; design section 6.5).
 */
#define BTD_PAIR_ASK_CONFIRM	1U
#define BTD_PAIR_ASK_PASSKEY	2U
#define BTD_PAIR_ASK_CONSENT	3U

/* The longest answer line of a pairing. */
#define BTD_PAIR_ANSWER_MAX	192U

/* How long each part may take (milliseconds). */
#define BTD_PAIR_CONNECT_MS	10000U
#define BTD_PAIR_TOTAL_MS	60000U
#define BTD_PAIR_SMP_MS		30000U
#define BTD_PAIR_AGENT_MS	25000U
#define BTD_PAIR_PROBE_MS	2000U
#define BTD_PAIR_CLOSE_MS	3000U

/* Asks the agent (kind BTD_PAIR_ASK_*, the number); a confirmation or a consent is answered with btd_pair_answer. */
typedef void (*btd_pair_ask_fn)(void *context, unsigned kind, uint32_t number);

/* Tells the pairing's end: "PAIRED ..." or "ERROR WHY". */
typedef void (*btd_pair_done_fn)(void *context, const char *answer);

/*
 * Takes over the connection of a pairing that succeeded (the HID host,
 * phase005 section 9.2): the device's address and type as the connection
 * knows them, the connection's handle, and the bond just stored (under the
 * identity address for LE).  Returns 1 when it took the connection (the
 * pairing forgets it and does not end it), 0 when the pairing ends it as
 * before.
 */
typedef int (*btd_pair_handoff_fn)(void *context, const uint8_t *address, unsigned type, uint16_t handle, const struct btd_bond *bond);

/*
 * What a pairing asked for a phone hands the phone link with its
 * connection (ws197-p002 section 7.2): the device's address and type, the
 * connection's handle, the bond just stored, the uid of the client that
 * asked, the encryption key's size the pairing read, the Class of Device
 * the last scan saw (have_class 0: the device was not in the scan), the
 * pairing's table of channels (the phone link moves them to its own,
 * btd_l2cap_move, and refuses those left before it returns) and the frame
 * being put together.  It lives on the pairing's stack for the hook's call.
 */
struct btd_pair_handoff {
	const uint8_t *address;
	unsigned type;
	uint16_t handle;
	const struct btd_bond *bond;
	uid_t uid;
	unsigned key_size;
	int have_class;
	uint32_t class_of_device;
	struct btd_l2cap *l2cap;
	struct btd_reassembly *reassembly;
};

/*
 * Takes over the connection of a phone's pairing that succeeded (the
 * phone link).  Returns 1 when it took the connection, or 0 with *why the
 * word the PAIRED line gives (unauthenticated, key-size, not-phone, busy,
 * ...); the pairing then refuses the channels it held Pending and offers
 * the connection to the HID host's hook.
 */
typedef int (*btd_pair_phone_fn)(void *context, const struct btd_pair_handoff *handoff, const char **why);

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

	/* Where the pairing is (0: none), the device (and whether it is LE), and whether an agent can answer. */
	unsigned state;
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int le;
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
	unsigned asked_kind;

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

	/* LE: the Security Manager, and an encryption held until the agent agrees to Just Works. */
	struct btd_smp smp;
	int encrypt_held;

	/* The connection's frames being put together, and its signalling. */
	struct btd_reassembly reassembly;
	struct btd_l2cap l2cap;

	/* The answer of the end, and what was refused or passed over. */
	char answer[BTD_PAIR_ANSWER_MAX];
	unsigned refused;
	unsigned ignored;

	/* The hook that may take over a paired connection (NULL: none), its context, and how many it took. */
	btd_pair_handoff_fn handoff;
	void *handoff_context;
	unsigned handed;

	/*
	 * ws197-p002: whether the pairing is for a phone and the uid of the
	 * client that asked; the phone link's hook (NULL: none) and its
	 * context; and the hook's answer (phone_taken, or why it did not take
	 * the connection), which the PAIRED line ends with.
	 */
	int phone;
	uid_t uid;
	btd_pair_phone_fn phone_handoff;
	void *phone_context;
	int phone_taken;
	const char *phone_why;
};

void btd_pair_init(struct btd_pair *pair, struct btd_session *session, const char *keys_folder, btd_pair_ask_fn ask, btd_pair_done_fn done, void *context, btd_random_fn random, void *random_context);
int btd_pair_start(struct btd_pair *pair, const uint8_t *address, unsigned type, int agent, int phone, uid_t uid);
void btd_pair_handle(void *context, struct btd_session *session, const uint8_t *packet, size_t length);
void btd_pair_answer(struct btd_pair *pair, int accepted);
void btd_pair_tick(struct btd_pair *pair, uint64_t now);
uint64_t btd_pair_deadline(const struct btd_pair *pair);
void btd_pair_stop(struct btd_pair *pair, const char *why);
void btd_pair_lost(struct btd_pair *pair);
int btd_pair_active(const struct btd_pair *pair);
int btd_pair_owns(const struct btd_pair *pair, const uint8_t *address);
void btd_pair_set_handoff(struct btd_pair *pair, btd_pair_handoff_fn handoff, void *context);
void btd_pair_set_phone_handoff(struct btd_pair *pair, btd_pair_phone_fn handoff, void *context);

#endif
