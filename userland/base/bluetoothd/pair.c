/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's connections and pairing (ws143-p004, see pair.h and
 * plan/ws143/phase004/phase.md sections 5, 7 and 10.3).
 *
 * A pairing goes through states: connecting (or cancelling a connection
 * that took too long), authenticating and encrypting (BR/EDR), securing
 * (LE's Security Manager), probing (an L2CAP Information Request on
 * BR/EDR, so the data path is tried), and disconnecting.  Each event of
 * the controller moves it on; the deadlines end it.  Every command it
 * sends is one at a time (session.c), and the packets that come meanwhile
 * are queued by the session, so this file is never entered twice.
 */

#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/crypto.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The commands of the connections and the pairing (OGF and OCF as the Core names them). */
#define PAIR_CREATE_CONNECTION		0x0405U
#define PAIR_DISCONNECT			0x0406U
#define PAIR_CREATE_CANCEL		0x0408U
#define PAIR_REJECT_CONNECTION		0x040aU
#define PAIR_LINK_KEY_REPLY		0x040bU
#define PAIR_LINK_KEY_NEGATIVE		0x040cU
#define PAIR_PIN_NEGATIVE		0x040eU
#define PAIR_AUTHENTICATION		0x0411U
#define PAIR_SET_ENCRYPTION		0x0413U
#define PAIR_IO_REPLY			0x042bU
#define PAIR_CONFIRM_REPLY		0x042cU
#define PAIR_CONFIRM_NEGATIVE		0x042dU
#define PAIR_PASSKEY_NEGATIVE		0x042fU
#define PAIR_IO_NEGATIVE		0x0434U
#define PAIR_READ_KEY_SIZE		0x1408U
#define PAIR_LE_CREATE_CONNECTION	0x200dU
#define PAIR_LE_CREATE_CANCEL		0x200eU
#define PAIR_LE_CONNECTION_UPDATE	0x2013U
#define PAIR_LE_ENCRYPT			0x2019U
#define PAIR_LE_READ_P256		0x2025U
#define PAIR_LE_DHKEY			0x2026U

/* The events of the connections and the pairing. */
#define PAIR_EVENT_CONNECTED		0x03U
#define PAIR_EVENT_REQUEST		0x04U
#define PAIR_EVENT_DISCONNECTED		0x05U
#define PAIR_EVENT_AUTHENTICATED	0x06U
#define PAIR_EVENT_ENCRYPTION		0x08U
#define PAIR_EVENT_PIN			0x16U
#define PAIR_EVENT_KEY_REQUEST		0x17U
#define PAIR_EVENT_KEY_NOTIFICATION	0x18U
#define PAIR_EVENT_IO_REQUEST		0x31U
#define PAIR_EVENT_IO_RESPONSE		0x32U
#define PAIR_EVENT_CONFIRM		0x33U
#define PAIR_EVENT_PASSKEY_REQUEST	0x34U
#define PAIR_EVENT_SIMPLE_COMPLETE	0x36U
#define PAIR_EVENT_PASSKEY_SHOWN	0x3bU
#define PAIR_EVENT_LE_META		0x3eU

/* LE's subevents: Connection Complete, Read Local P-256 Public Key Complete, Generate DHKey Complete. */
#define PAIR_LE_CONNECTED		0x01U
#define PAIR_LE_P256_DONE		0x08U
#define PAIR_LE_DHKEY_DONE		0x09U

/* The states of a pairing. */
#define PAIR_IDLE			0U
#define PAIR_CONNECTING			1U
#define PAIR_CANCELLING			2U
#define PAIR_AUTHENTICATING		3U
#define PAIR_ENCRYPTING			4U
#define PAIR_SECURING			5U
#define PAIR_PROBING			6U
#define PAIR_DISCONNECTING		7U

/* The IO capabilities and authentication requirements of Secure Simple Pairing (Core Vol 4 Part E §7.1.29). */
#define PAIR_IO_DISPLAY_YES_NO		0x01U
#define PAIR_IO_NONE			0x03U
#define PAIR_AUTH_DEDICATED		0x02U
#define PAIR_AUTH_DEDICATED_MITM	0x03U

/* The link key types (§7.7.24): debug, unauthenticated and authenticated P-192, changed, unauthenticated and authenticated P-256. */
#define PAIR_KEY_DEBUG			0x03U
#define PAIR_KEY_P192			0x04U
#define PAIR_KEY_P192_MITM		0x05U
#define PAIR_KEY_CHANGED		0x06U
#define PAIR_KEY_P256			0x07U
#define PAIR_KEY_P256_MITM		0x08U

/* The reasons bluetoothd gives: a connection refused (unacceptable address), pairing not allowed, a disconnection by the user. */
#define PAIR_REASON_UNACCEPTABLE	0x0fU
#define PAIR_REASON_NOT_ALLOWED		0x18U
#define PAIR_REASON_USER		0x13U

/* The status of an authentication whose key the other side did not have (PIN or Key Missing). */
#define PAIR_STATUS_KEY_MISSING		0x06U

/* The status of an authentication whose key the other side has another of (Authentication Failure). */
#define PAIR_STATUS_AUTH_FAILURE	0x05U

/* The only key size taken (KNOB, design section 6.2). */
#define PAIR_KEY_SIZE			16U

/* How many bonds are looked through to resolve a private address. */
#define PAIR_BONDS_MAX			32U

static void pair_event(struct btd_pair *pair, const uint8_t *parameters, size_t length, uint8_t code);
static void pair_acl(struct btd_pair *pair, const uint8_t *packet, size_t length);
static void pair_connected(struct btd_pair *pair, const uint8_t *parameters, size_t length);
static void pair_le_meta(struct btd_pair *pair, const uint8_t *parameters, size_t length);
static void pair_le_connected(struct btd_pair *pair, const uint8_t *parameters, size_t length);
static void pair_disconnected(struct btd_pair *pair, const uint8_t *parameters, size_t length);
static void pair_authenticated(struct btd_pair *pair, const uint8_t *parameters, size_t length);
static void pair_encryption(struct btd_pair *pair, const uint8_t *parameters, size_t length);
static void pair_key_request(struct btd_pair *pair, const uint8_t *address);
static void pair_key_notification(struct btd_pair *pair, const uint8_t *parameters, size_t length);
static void pair_io_request(struct btd_pair *pair, const uint8_t *address);
static void pair_confirm(struct btd_pair *pair, const uint8_t *parameters, size_t length);
static void pair_key_size(struct btd_pair *pair);
static void pair_store_bredr(struct btd_pair *pair);
static void pair_store_le(struct btd_pair *pair);
static void pair_probe(struct btd_pair *pair);
static void pair_signal(struct btd_pair *pair, int le, const uint8_t *payload, size_t length);
static void pair_smp_actions(struct btd_pair *pair, unsigned actions);
static void pair_succeed(struct btd_pair *pair);
static void pair_fail(struct btd_pair *pair, const char *why);
static void pair_finish(struct btd_pair *pair);
static void pair_deliver(struct btd_pair *pair);
static void pair_cancel(struct btd_pair *pair, const char *why);
static int pair_command(struct btd_pair *pair, uint16_t opcode, const uint8_t *parameters, size_t count, const char *why);
static void pair_reply(struct btd_pair *pair, uint16_t opcode, const uint8_t *address, const uint8_t *more, size_t more_length);
static void pair_disconnect_other(struct btd_pair *pair, uint16_t handle);
static int pair_ours(const struct btd_pair *pair, const uint8_t *address);
static int pair_le(const struct btd_pair *pair);
static void pair_name(const struct btd_pair *pair, const uint8_t *address, unsigned type, char *name, size_t size);
static const char *pair_smp_why(const struct btd_pair *pair);
static void pair_put16(uint8_t *bytes, uint16_t value);
static void pair_ask(struct btd_pair *pair, unsigned kind, uint32_t number);
static void pair_le_encrypt(struct btd_pair *pair);
static int pair_resolved_bond(const struct btd_pair *pair);
static void pair_reverse(uint8_t *to, const uint8_t *from, size_t length);
static uint16_t pair_handle(const uint8_t *bytes);

/*
 * Prepares the pairing of a session: no pairing yet, the folder of the
 * bonds, the daemon's hooks and the random source the Security Manager
 * uses.  The caller makes it the session's handler (btd_pair_handle).
 */
void
btd_pair_init(
	struct btd_pair *pair,
	struct btd_session *session,
	const char *keys_folder,
	btd_pair_ask_fn ask,
	btd_pair_done_fn done,
	void *context,
	btd_random_fn random,
	void *random_context)
{
	/* Nothing under way. */
	memset(pair, 0, sizeof(*pair));
	pair->session = session;
	pair->keys_folder = keys_folder;
	pair->ask = ask;
	pair->done = done;
	pair->context = context;
	pair->random = random;
	pair->random_context = random_context;
	pair->state = PAIR_IDLE;
	btd_l2cap_init(&pair->l2cap);
}

/*
 * Starts pairing a device: connects to it and pairs (the end comes to the
 * done hook, also for a refusal now).  agent says whether someone can
 * confirm a number.  Returns 0, or EBUSY while a pairing or a scan runs
 * (the hook is not called then).
 */
int
btd_pair_start(
	struct btd_pair *pair,
	const uint8_t *address,
	unsigned type,
	int agent)
{
	uint8_t parameters[25];
	int error;

	/* One pairing at a time, and none during a scan. */
	if (pair->state != PAIR_IDLE)
		return EBUSY;
	if (pair->session->scanning)
		return EBUSY;

	/* The device and a clean slate. */
	memcpy(pair->address, address, BTD_ADDRESS_BYTES);
	pair->type = type;
	pair->agent = agent;
	pair->connected = 0;
	pair->pending_error = NULL;
	pair->asked = 0;
	pair->asked_kind = 0U;
	pair->encrypt_held = 0;
	pair->agent_deadline = 0U;
	pair->smp_deadline = 0U;
	pair->have_stored = 0;
	pair->used_stored = 0;
	pair->have_peer_io = 0;
	pair->confirmed = 0;
	pair->have_link_key = 0;
	pair->refusal = NULL;
	pair->probed = 0;
	pair->key_size = 0U;
	pair->answer[0] = '\0';
	memset(&pair->reassembly, 0, sizeof(pair->reassembly));
	btd_l2cap_init(&pair->l2cap);

	/* The pairing is under way from now on, for BTD_PAIR_TOTAL_MS at most. */
	pair->state = PAIR_CONNECTING;
	pair->total_deadline = btd_now_ms() + BTD_PAIR_TOTAL_MS;
	pair->state_deadline = btd_now_ms() + BTD_PAIR_CONNECT_MS;

	/* A bond this controller keeps for the device already. */
	error = btd_keys_read(pair->keys_folder, pair->session->address, address, type, &pair->stored);
	if (error == 0)
		pair->have_stored = 1;

	/* LE: a bond is not paired over silently (it is forgotten first), and the controller must do P-256. */
	if (pair_le(pair)) {
		if (!pair->have_stored)
			pair->have_stored = pair_resolved_bond(pair);
		if (pair->have_stored) {
			pair_fail(pair, "bonded");
			return 0;
		}

		/* Without the controller's P-256 and DHKey (D5's b1) LE cannot pair. */
		if (!pair->session->p256 || !pair->session->dhkey) {
			pair_fail(pair, "no-p256");
			return 0;
		}

		/*
		 * LE Create Connection: scan interval 60 ms and window 30 ms, no
		 * filter, the device, bluetoothd's public address, a connection
		 * interval of 30 to 50 ms, no latency, a supervision timeout of 5 s.
		 */
		memset(parameters, 0, sizeof(parameters));
		pair_put16(parameters + 0, 0x0060U);
		pair_put16(parameters + 2, 0x0030U);
		parameters[4] = 0x00U;
		parameters[5] = 0x00U;
		if (type == BTD_ADDRESS_LE_RANDOM)
			parameters[5] = 0x01U;
		memcpy(parameters + 6, address, BTD_ADDRESS_BYTES);
		parameters[12] = 0x00U;
		pair_put16(parameters + 13, 0x0018U);
		pair_put16(parameters + 15, 0x0028U);
		pair_put16(parameters + 17, 0x0000U);
		pair_put16(parameters + 19, 0x01f4U);
		(void)pair_command(pair, PAIR_LE_CREATE_CONNECTION, parameters, 25U, "unreachable");
		return 0;
	}

	/*
	 * Create Connection: the device, the packet types DM1 to DH5, page scan
	 * repetition R1, no clock offset, the role switch allowed.
	 */
	memset(parameters, 0, sizeof(parameters));
	memcpy(parameters, address, BTD_ADDRESS_BYTES);
	pair_put16(parameters + 6, 0xcc18U);
	parameters[8] = 0x01U;
	parameters[9] = 0x00U;
	pair_put16(parameters + 10, 0x0000U);
	parameters[12] = 0x01U;
	(void)pair_command(pair, PAIR_CREATE_CONNECTION, parameters, 13U, "unreachable");

	/* Succeeded: the connection is under way (or the end was told). */
	return 0;
}

/*
 * Takes a packet the session hands on (the session's handler): a
 * connection's or a pairing's event, or ACL data.
 */
void
btd_pair_handle(
	void *context,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	struct btd_pair *pair;

	UNUSED_PARAMETER(session);

	/* The pairing this handler serves. */
	pair = context;

	/* ACL data of a connection. */
	if (length >= 1U && packet[0] == BT_PACKET_ACL) {
		pair_acl(pair, packet, length);
		return;
	}

	/* An event: its code and its parameters, whose length the session checked. */
	if (length < 3U || packet[0] != BT_PACKET_EVENT)
		return;
	pair_event(pair, packet + 3, (size_t)packet[2], packet[1]);
}

/*
 * Takes the agent's answer to a number to confirm: yes goes on, no refuses
 * the pairing.  An answer nobody asked for is passed over.
 */
void
btd_pair_answer(
	struct btd_pair *pair,
	int accepted)
{
	unsigned actions;

	/* Only a question that waits. */
	if (!pair->asked)
		return;
	pair->asked = 0;
	pair->agent_deadline = 0U;

	/* LE's agreement to Just Works: the encryption held goes on, or the pairing is refused. */
	if (pair_le(pair) && pair->asked_kind == BTD_PAIR_ASK_CONSENT) {
		if (accepted) {
			pair_le_encrypt(pair);
			return;
		}

		/* Refused by the user. */
		pair->smp.why = BTD_SMP_WHY_REJECTED;
		actions = btd_smp_fail(&pair->smp, BTD_SMP_UNSPECIFIED);
		pair_smp_actions(pair, actions);
		return;
	}

	/* LE's number: the Security Manager takes it. */
	if (pair_le(pair)) {
		actions = btd_smp_agent(&pair->smp, accepted);
		pair_smp_actions(pair, actions);
		return;
	}

	/* BR/EDR: User Confirmation Request Reply, or the refusal the controller sends on. */
	if (accepted) {
		pair->confirmed = 1;
		pair_reply(pair, PAIR_CONFIRM_REPLY, pair->address, NULL, 0U);
		return;
	}

	/* The refusal: the authentication's end says the rest. */
	pair->refusal = "rejected";
	pair_reply(pair, PAIR_CONFIRM_NEGATIVE, pair->address, NULL, 0U);
}

/*
 * Ends what passed its deadline: an agent's question (no), the Security
 * Manager's 30 seconds, the state's time, the whole pairing's time.
 */
void
btd_pair_tick(
	struct btd_pair *pair,
	uint64_t now)
{
	/* Nothing under way. */
	if (pair->state == PAIR_IDLE)
		return;

	/* An agent that did not answer said no. */
	if (pair->asked && pair->agent_deadline != 0U && now >= pair->agent_deadline) {
		btd_pair_answer(pair, 0);
		if (pair->state == PAIR_IDLE)
			return;
	}

	/* A disconnection that did not come: the end is told all the same. */
	if (pair->state == PAIR_DISCONNECTING) {
		if (now >= pair->state_deadline) {
			pair->connected = 0;
			pair_deliver(pair);
		}

		return;
	}

	/* A cancelled connection whose end did not come. */
	if (pair->state == PAIR_CANCELLING) {
		if (now >= pair->state_deadline)
			pair_deliver(pair);
		return;
	}

	/* A connection that took too long is cancelled. */
	if (pair->state == PAIR_CONNECTING) {
		if (now >= pair->state_deadline)
			pair_cancel(pair, "timeout");
		return;
	}

	/* An L2CAP answer that did not come does not fail the pairing. */
	if (pair->state == PAIR_PROBING) {
		if (now >= pair->state_deadline)
			pair_succeed(pair);
		return;
	}

	/* The Security Manager's time (Core Vol 3 Part H §3.4): no Pairing Failed is sent, the link is ended. */
	if (pair->smp_deadline != 0U && now >= pair->smp_deadline) {
		pair->smp_deadline = 0U;
		pair_fail(pair, "timeout");
		return;
	}

	/* The whole pairing's time. */
	if (now >= pair->total_deadline)
		pair_fail(pair, "timeout");
}

/*
 * Gives the earliest deadline of the pairing (the daemon's loop wakes then),
 * or 0 when no pairing runs.
 */
uint64_t
btd_pair_deadline(
	const struct btd_pair *pair)
{
	uint64_t earliest;

	/* Nothing under way. */
	if (pair->state == PAIR_IDLE)
		return 0U;

	/* The whole pairing's, then any sooner one. */
	earliest = pair->total_deadline;
	if (pair->state_deadline != 0U && pair->state_deadline < earliest)
		earliest = pair->state_deadline;
	if (pair->smp_deadline != 0U && pair->smp_deadline < earliest)
		earliest = pair->smp_deadline;
	if (pair->agent_deadline != 0U && pair->agent_deadline < earliest)
		earliest = pair->agent_deadline;

	/* Succeeded: the earliest. */
	return earliest;
}

/*
 * Stops the pairing for the daemon (the client that asked went away): a
 * connection under way is cancelled, one made is ended, and the end is
 * told with why.
 */
void
btd_pair_stop(
	struct btd_pair *pair,
	const char *why)
{
	/* Nothing under way, or ending already. */
	if (pair->state == PAIR_IDLE || pair->state == PAIR_DISCONNECTING || pair->state == PAIR_CANCELLING)
		return;

	/* A connection not made yet is cancelled. */
	if (pair->state == PAIR_CONNECTING) {
		pair_cancel(pair, why);
		return;
	}

	/* Anything else ends the connection. */
	pair_fail(pair, why);
}

/*
 * Ends the pairing because the controller's node went: no command can be
 * sent, the end is told at once.
 */
void
btd_pair_lost(
	struct btd_pair *pair)
{
	/* Nothing under way. */
	if (pair->state == PAIR_IDLE)
		return;

	/* Nothing is connected any more. */
	pair->connected = 0;
	(void)snprintf(pair->answer, sizeof(pair->answer), "%s", "ERROR lost");
	pair_deliver(pair);
}

/*
 * Tells whether a pairing runs.
 */
int
btd_pair_active(
	const struct btd_pair *pair)
{
	/* A pairing between its start and its end. */
	if (pair->state != PAIR_IDLE)
		return 1;

	/* None. */
	return 0;
}

/* Takes one event of the connections or the pairing. */
static void
pair_event(
	struct btd_pair *pair,
	const uint8_t *parameters,
	size_t length,
	uint8_t code)
{
	uint8_t reason[1];
	uint32_t number;
	int ours;

	/* Each event the pairing knows; the address of an address-led event is its first six bytes. */
	switch (code) {
	case PAIR_EVENT_CONNECTED:
		pair_connected(pair, parameters, length);
		break;
	case PAIR_EVENT_REQUEST:
		/* A device connecting to bluetoothd: refused, its address unacceptable (pairing is started from here only, design section 6.5). */
		if (length >= BTD_ADDRESS_BYTES) {
			pair->refused++;
			reason[0] = PAIR_REASON_UNACCEPTABLE;
			pair_reply(pair, PAIR_REJECT_CONNECTION, parameters, reason, sizeof(reason));
		}

		break;
	case PAIR_EVENT_DISCONNECTED:
		pair_disconnected(pair, parameters, length);
		break;
	case PAIR_EVENT_AUTHENTICATED:
		pair_authenticated(pair, parameters, length);
		break;
	case PAIR_EVENT_ENCRYPTION:
		pair_encryption(pair, parameters, length);
		break;
	case PAIR_EVENT_PIN:
		/* A PIN (legacy pairing) is not taken in this Phase (decision Q2): the authentication's end says so. */
		if (length >= BTD_ADDRESS_BYTES) {
			ours = pair_ours(pair, parameters);
			if (ours)
				pair->refusal = "pin-unsupported";
			pair_reply(pair, PAIR_PIN_NEGATIVE, parameters, NULL, 0U);
		}

		break;
	case PAIR_EVENT_KEY_REQUEST:
		if (length >= BTD_ADDRESS_BYTES)
			pair_key_request(pair, parameters);
		break;
	case PAIR_EVENT_KEY_NOTIFICATION:
		pair_key_notification(pair, parameters, length);
		break;
	case PAIR_EVENT_IO_REQUEST:
		if (length >= BTD_ADDRESS_BYTES)
			pair_io_request(pair, parameters);
		break;
	case PAIR_EVENT_IO_RESPONSE:
		/* The other side's IO capability decides between a question and Just Works. */
		if (length < 9U)
			break;
		ours = pair_ours(pair, parameters);
		if (ours) {
			pair->peer_io = parameters[6];
			pair->have_peer_io = 1;
		}

		break;
	case PAIR_EVENT_CONFIRM:
		pair_confirm(pair, parameters, length);
		break;
	case PAIR_EVENT_PASSKEY_REQUEST:
		/* bluetoothd does not type passkeys in this Phase (decision Q6). */
		if (length >= BTD_ADDRESS_BYTES)
			pair_reply(pair, PAIR_PASSKEY_NEGATIVE, parameters, NULL, 0U);
		break;
	case PAIR_EVENT_PASSKEY_SHOWN:
		/* The passkey the other device types, shown to the user. */
		if (length < 10U)
			break;
		ours = pair_ours(pair, parameters);
		number = (uint32_t)parameters[6] | ((uint32_t)parameters[7] << 8) | ((uint32_t)parameters[8] << 16) | ((uint32_t)parameters[9] << 24);
		if (ours && pair->ask != NULL)
			pair->ask(pair->context, BTD_PAIR_ASK_PASSKEY, number);
		break;
	case PAIR_EVENT_SIMPLE_COMPLETE:
		/* The authentication's end tells the outcome; a failure here is kept for it. */
		if (length < 7U)
			break;
		ours = pair_ours(pair, parameters + 1);
		if (ours && parameters[0] != 0U && pair->refusal == NULL)
			pair->refusal = "rejected";
		break;
	case PAIR_EVENT_LE_META:
		pair_le_meta(pair, parameters, length);
		break;
	default:
		pair->ignored++;
		break;
	}
}

/* Takes an ACL packet: a frame of the pairing's connection, put together, goes to its channel. */
static void
pair_acl(
	struct btd_pair *pair,
	const uint8_t *packet,
	size_t length)
{
	struct btd_acl acl;
	uint16_t cid;
	size_t payload;
	unsigned actions;
	int whole;
	int error;

	/* The packet's connection and data. */
	error = btd_acl_parse(packet, length, &acl);
	if (error != 0) {
		pair->ignored++;
		return;
	}

	/* Only the pairing's connection carries data bluetoothd reads in this Phase. */
	if (!pair->connected || acl.handle != pair->handle) {
		pair->ignored++;
		return;
	}

	/* The frame, once whole. */
	whole = btd_reassembly_feed(&pair->reassembly, &acl);
	if (whole <= 0)
		return;

	/* Its channel and payload. */
	payload = pair->reassembly.expected - BTD_L2CAP_HEADER;
	cid = (uint16_t)(pair->reassembly.frame[2] | (pair->reassembly.frame[3] << 8));

	/* Each fixed channel of the pairing; anything else is not read in this Phase. */
	if (cid == BTD_CID_SIGNALLING && !pair_le(pair)) {
		pair_signal(pair, 0, pair->reassembly.frame + BTD_L2CAP_HEADER, payload);
	} else if (cid == BTD_CID_LE_SIGNALLING && pair_le(pair)) {
		pair_signal(pair, 1, pair->reassembly.frame + BTD_L2CAP_HEADER, payload);
	} else if (cid == BTD_CID_SMP && pair_le(pair) && pair->state == PAIR_SECURING) {
		/* A Security Manager PDU: its 30 seconds start again, and what it asks for is done. */
		pair->smp_deadline = btd_now_ms() + BTD_PAIR_SMP_MS;
		actions = btd_smp_input(&pair->smp, pair->reassembly.frame + BTD_L2CAP_HEADER, payload);
		pair_smp_actions(pair, actions);
	} else {
		pair->ignored++;
	}
}

/* Takes BR/EDR's Connection Complete (status, handle, address, link type, encryption). */
static void
pair_connected(
	struct btd_pair *pair,
	const uint8_t *parameters,
	size_t length)
{
	uint8_t handle[2];
	uint16_t connection;
	int awaited;

	/* A whole event. */
	if (length < 11U)
		return;
	connection = pair_handle(parameters + 1);

	/* Whether it is the connection the pairing waits for. */
	awaited = 0;
	if (!pair_le(pair) && (pair->state == PAIR_CONNECTING || pair->state == PAIR_CANCELLING))
		awaited = pair_ours(pair, parameters + 3);

	/* Not that one: a connection made all the same is ended. */
	if (!awaited) {
		if (parameters[0] == 0U)
			pair_disconnect_other(pair, connection);
		return;
	}

	/* The connection failed (a cancelled one ends with the reason it was cancelled for). */
	if (parameters[0] != 0U) {
		if (pair->state == PAIR_CANCELLING) {
			pair_fail(pair, pair->pending_error);
			return;
		}

		/* The device did not answer the page. */
		pair_fail(pair, "unreachable");
		return;
	}

	/* Connected; a cancelled pairing ends it at once. */
	pair->handle = connection;
	pair->connected = 1;
	if (pair->state == PAIR_CANCELLING) {
		pair_fail(pair, pair->pending_error);
		return;
	}

	/* Authentication Requested: the controller asks for a link key, then pairs. */
	pair->state = PAIR_AUTHENTICATING;
	pair->state_deadline = 0U;
	pair_put16(handle, connection);
	(void)pair_command(pair, PAIR_AUTHENTICATION, handle, sizeof(handle), "protocol");
}

/* Takes LE's meta events of the pairing: the connection, the controller's public key, the DHKey. */
static void
pair_le_meta(
	struct btd_pair *pair,
	const uint8_t *parameters,
	size_t length)
{
	unsigned actions;

	/* A subevent at all. */
	if (length < 2U)
		return;

	/* Each subevent the pairing uses. */
	switch (parameters[0]) {
	case PAIR_LE_CONNECTED:
		pair_le_connected(pair, parameters, length);
		break;
	case PAIR_LE_P256_DONE:
		/* The controller's public key (status, X, Y). */
		if (length < 66U || !pair_le(pair) || pair->state != PAIR_SECURING)
			break;
		actions = btd_smp_local_key(&pair->smp, parameters[1], parameters + 2);
		pair_smp_actions(pair, actions);
		break;
	case PAIR_LE_DHKEY_DONE:
		/* The DHKey (status, the key). */
		if (length < 34U || !pair_le(pair) || pair->state != PAIR_SECURING)
			break;
		actions = btd_smp_dhkey(&pair->smp, parameters[1], parameters + 2);
		pair_smp_actions(pair, actions);
		break;
	default:
		pair->ignored++;
		break;
	}
}

/*
 * Takes LE Connection Complete (subevent, status, handle, role, the
 * device's address type and address, and the connection's parameters):
 * the Security Manager starts on the pairing's connection.
 */
static void
pair_le_connected(
	struct btd_pair *pair,
	const uint8_t *parameters,
	size_t length)
{
	uint16_t connection;
	uint8_t responder_type;
	uint8_t io;
	unsigned actions;
	int same;

	/* A whole event. */
	if (length < 19U)
		return;
	connection = pair_handle(parameters + 2);

	/* Not the connection the pairing waits for: one made all the same is ended. */
	same = 0;
	if (pair_le(pair) && (pair->state == PAIR_CONNECTING || pair->state == PAIR_CANCELLING))
		same = pair_ours(pair, parameters + 6);
	if (!same && parameters[1] == 0U) {
		pair_disconnect_other(pair, connection);
		return;
	}

	/* A cancelled connection's end (Unknown Connection Identifier) carries no address. */
	if (!same && pair->state == PAIR_CANCELLING) {
		pair_fail(pair, pair->pending_error);
		return;
	}

	/* Any other failure the pairing does not wait for. */
	if (!same)
		return;

	/* The connection failed. */
	if (parameters[1] != 0U) {
		if (pair->state == PAIR_CANCELLING) {
			pair_fail(pair, pair->pending_error);
			return;
		}

		/* The device could not be reached. */
		pair_fail(pair, "unreachable");
		return;
	}

	/* Connected; a cancelled pairing ends it at once. */
	pair->handle = connection;
	pair->connected = 1;
	if (pair->state == PAIR_CANCELLING) {
		pair_fail(pair, pair->pending_error);
		return;
	}

	/* The Security Manager: DisplayYesNo with an agent, NoInputNoOutput without; bluetoothd's public address. */
	io = BTD_SMP_NO_IO;
	if (pair->agent)
		io = BTD_SMP_DISPLAY_YES_NO;
	responder_type = 0x00U;
	if (pair->type == BTD_ADDRESS_LE_RANDOM)
		responder_type = 0x01U;
	btd_smp_init(&pair->smp, io, 0x00U, pair->session->address, responder_type, pair->address, pair->random, pair->random_context);

	/* The pairing starts: the Pairing Request, the Security Manager's 30 seconds. */
	pair->state = PAIR_SECURING;
	pair->state_deadline = 0U;
	pair->smp_deadline = btd_now_ms() + BTD_PAIR_SMP_MS;
	actions = btd_smp_start(&pair->smp);
	pair_smp_actions(pair, actions);
}

/* Takes Disconnection Complete (status, handle, reason). */
static void
pair_disconnected(
	struct btd_pair *pair,
	const uint8_t *parameters,
	size_t length)
{
	uint16_t connection;

	/* A whole event about the pairing's connection, which ended. */
	if (length < 4U || parameters[0] != 0U)
		return;
	connection = pair_handle(parameters + 1);
	if (!pair->connected || connection != pair->handle)
		return;
	pair->connected = 0;

	/* The disconnection bluetoothd asked for: the end is told now. */
	if (pair->state == PAIR_DISCONNECTING) {
		pair_deliver(pair);
		return;
	}

	/* The device went in the middle of the pairing. */
	pair_fail(pair, "lost");
}

/* Takes BR/EDR's Authentication Complete (status, handle): the link is encrypted, or why it was not paired. */
static void
pair_authenticated(
	struct btd_pair *pair,
	const uint8_t *parameters,
	size_t length)
{
	uint8_t encryption[3];
	uint16_t connection;

	/* The pairing's connection while it authenticates. */
	if (length < 3U)
		return;
	connection = pair_handle(parameters + 1);
	if (pair->state != PAIR_AUTHENTICATING || !pair->connected || connection != pair->handle)
		return;

	/* A refusal this side named comes first (a debug key, a PIN, the agent's no). */
	if (parameters[0] != 0U && pair->refusal != NULL) {
		pair_fail(pair, pair->refusal);
		return;
	}

	/* The stored key the device no longer has (or has another one): it is forgotten by the user, not here (plan section 10.3). */
	if (pair->used_stored && (parameters[0] == PAIR_STATUS_KEY_MISSING || parameters[0] == PAIR_STATUS_AUTH_FAILURE)) {
		pair_fail(pair, "key-missing");
		return;
	}

	/* Any other failure. */
	if (parameters[0] != 0U) {
		pair_fail(pair, "rejected");
		return;
	}

	/* A key the controller gave that this side refused fails the pairing even if the controller went on. */
	if (pair->refusal != NULL) {
		pair_fail(pair, pair->refusal);
		return;
	}

	/* Set Connection Encryption: on. */
	pair->state = PAIR_ENCRYPTING;
	pair_put16(encryption, connection);
	encryption[2] = 0x01U;
	(void)pair_command(pair, PAIR_SET_ENCRYPTION, encryption, sizeof(encryption), "encryption");
}

/* Takes Encryption Change (status, handle, enabled). */
static void
pair_encryption(
	struct btd_pair *pair,
	const uint8_t *parameters,
	size_t length)
{
	uint16_t connection;
	unsigned actions;
	int on;

	/* The pairing's connection. */
	if (length < 4U)
		return;
	connection = pair_handle(parameters + 1);
	if (!pair->connected || connection != pair->handle)
		return;
	on = 0;
	if (parameters[3] != 0U)
		on = 1;

	/* LE: the Security Manager takes it (the keys come next). */
	if (pair_le(pair)) {
		if (pair->state != PAIR_SECURING)
			return;
		actions = btd_smp_encrypted(&pair->smp, parameters[0], on);
		pair_smp_actions(pair, actions);
		return;
	}

	/* BR/EDR: only the encryption the pairing asked for. */
	if (pair->state != PAIR_ENCRYPTING)
		return;

	/* Not encrypted. */
	if (parameters[0] != 0U || !on) {
		pair_fail(pair, "encryption");
		return;
	}

	/* The key size comes next. */
	pair_key_size(pair);
}

/* Answers Link Key Request: the pairing's stored key, or none (the controller pairs then). */
static void
pair_key_request(
	struct btd_pair *pair,
	const uint8_t *address)
{
	int ours;

	/* Another device, or no stored key: Negative Reply. */
	ours = pair_ours(pair, address);
	if (!ours || pair->state != PAIR_AUTHENTICATING || !pair->have_stored || !pair->stored.have_link_key) {
		pair_reply(pair, PAIR_LINK_KEY_NEGATIVE, address, NULL, 0U);
		return;
	}

	/* The stored key. */
	pair->used_stored = 1;
	pair_reply(pair, PAIR_LINK_KEY_REPLY, address, pair->stored.link_key, sizeof(pair->stored.link_key));
}

/*
 * Takes Link Key Notification (address, key, type): a key type the
 * pairing takes is kept until the link is checked (plan section 7); the
 * debug key and the legacy kinds are refused and end the connection.
 */
static void
pair_key_notification(
	struct btd_pair *pair,
	const uint8_t *parameters,
	size_t length)
{
	uint8_t key_type;
	int ours;

	/* A whole event about the pairing's device while it authenticates. */
	if (length < 23U || pair->state != PAIR_AUTHENTICATING)
		return;
	ours = pair_ours(pair, parameters);
	if (!ours)
		return;
	key_type = parameters[22];

	/* The Core's debug key: anyone could read the link (design section 6.2). */
	if (key_type == PAIR_KEY_DEBUG) {
		pair_fail(pair, "debug-key");
		return;
	}

	/* A changed combination key keeps the type of the key it replaced; without one it is not taken. */
	if (key_type == PAIR_KEY_CHANGED) {
		if (!pair->have_stored || !pair->stored.have_link_key) {
			pair_fail(pair, "key-type");
			return;
		}

		/* The type of the stored key. */
		key_type = pair->stored.link_key_type;
	}

	/* Only Secure Simple Pairing's keys (P-192 and P-256, with or without MITM). */
	if (key_type != PAIR_KEY_P192 &&
	    key_type != PAIR_KEY_P192_MITM &&
	    key_type != PAIR_KEY_P256 &&
	    key_type != PAIR_KEY_P256_MITM) {
		pair_fail(pair, "key-type");
		return;
	}

	/* Kept until the encryption and its key size are checked (review S5). */
	memcpy(pair->link_key, parameters + 6, sizeof(pair->link_key));
	pair->link_key_type = key_type;
	pair->have_link_key = 1;
}

/* Answers IO Capability Request: the pairing's device gets bluetoothd's capability, any other is refused. */
static void
pair_io_request(
	struct btd_pair *pair,
	const uint8_t *address)
{
	uint8_t reason[1];
	uint8_t reply[3];
	int ours;

	/* A pairing this side did not start (design section 6.5). */
	ours = pair_ours(pair, address);
	if (!ours || pair->state != PAIR_AUTHENTICATING) {
		pair->refused++;
		reason[0] = PAIR_REASON_NOT_ALLOWED;
		pair_reply(pair, PAIR_IO_NEGATIVE, address, reason, sizeof(reason));
		return;
	}

	/* DisplayYesNo with MITM and dedicated bonding when someone can confirm, else NoInputNoOutput without MITM; no OOB data. */
	reply[0] = PAIR_IO_NONE;
	reply[1] = 0x00U;
	reply[2] = PAIR_AUTH_DEDICATED;
	if (pair->agent) {
		reply[0] = PAIR_IO_DISPLAY_YES_NO;
		reply[2] = PAIR_AUTH_DEDICATED_MITM;
	}
	pair_reply(pair, PAIR_IO_REPLY, address, reply, sizeof(reply));
}

/*
 * Takes User Confirmation Request (address, number): Numeric Comparison
 * asks the agent when both sides can show and confirm; Just Works is taken
 * as agreed by the PAIR request itself (decision Q7).
 */
static void
pair_confirm(
	struct btd_pair *pair,
	const uint8_t *parameters,
	size_t length)
{
	uint32_t number;
	int numeric;
	int ours;

	/* A whole event. */
	if (length < 10U)
		return;

	/* A pairing this side did not start. */
	ours = pair_ours(pair, parameters);
	if (!ours || pair->state != PAIR_AUTHENTICATING) {
		pair->refused++;
		pair_reply(pair, PAIR_CONFIRM_NEGATIVE, parameters, NULL, 0U);
		return;
	}

	/* Numeric Comparison when the other side can show and confirm too (BR/EDR has no KeyboardDisplay). */
	numeric = 0;
	if (pair->have_peer_io && pair->peer_io == PAIR_IO_DISPLAY_YES_NO)
		numeric = 1;

	/* Without an agent nobody can be asked: Just Works goes on. */
	if (!pair->agent) {
		pair->confirmed = 1;
		pair_reply(pair, PAIR_CONFIRM_REPLY, parameters, NULL, 0U);
		return;
	}

	/* Just Works asks the agent to agree; Numeric Comparison asks it to confirm the number (design section 6.5). */
	number = (uint32_t)parameters[6] | ((uint32_t)parameters[7] << 8) | ((uint32_t)parameters[8] << 16) | ((uint32_t)parameters[9] << 24);
	if (!numeric) {
		pair_ask(pair, BTD_PAIR_ASK_CONSENT, 0U);
		return;
	}

	/* The number. */
	pair_ask(pair, BTD_PAIR_ASK_CONFIRM, number);
}

/* Reads the encryption key's size (KNOB): only 16 bytes are taken, then the key is stored. */
static void
pair_key_size(
	struct btd_pair *pair)
{
	uint8_t handle[2];
	int error;

	/* Read Encryption Key Size: status, handle, size. */
	pair_put16(handle, pair->handle);
	error = pair_command(pair, PAIR_READ_KEY_SIZE, handle, sizeof(handle), "key-size");
	if (error != 0)
		return;
	if (pair->session->returned_length < 3U) {
		pair_fail(pair, "key-size");
		return;
	}
	pair->key_size = pair->session->returned[2];

	/* A shorter key is refused. */
	if (pair->key_size != PAIR_KEY_SIZE) {
		pair_fail(pair, "key-size");
		return;
	}

	/* A new key is stored now that the link is checked (a stored key that worked stays as it is). */
	if (!pair->used_stored) {
		pair_store_bredr(pair);
		if (pair->state == PAIR_DISCONNECTING || pair->state == PAIR_IDLE)
			return;
	}

	/* The data path is tried. */
	pair_probe(pair);
}

/* Stores the new BR/EDR bond: the link key and its type, and the name the last scan saw. */
static void
pair_store_bredr(
	struct btd_pair *pair)
{
	struct btd_bond bond;
	int error;

	/* A pairing without a key from the controller broke the protocol. */
	if (!pair->have_link_key) {
		pair_fail(pair, "protocol");
		return;
	}

	/* The bond. */
	memset(&bond, 0, sizeof(bond));
	memcpy(bond.address, pair->address, BTD_ADDRESS_BYTES);
	bond.type = BTD_ADDRESS_BREDR;
	pair_name(pair, pair->address, BTD_ADDRESS_BREDR, bond.name, sizeof(bond.name));
	bond.have_link_key = 1;
	memcpy(bond.link_key, pair->link_key, sizeof(bond.link_key));
	bond.link_key_type = pair->link_key_type;
	bond.key_size = PAIR_KEY_SIZE;

	/* What the key is worth: MITM-protected (authenticated), Secure Connections (P-256). */
	if (pair->link_key_type == PAIR_KEY_P192_MITM || pair->link_key_type == PAIR_KEY_P256_MITM)
		bond.authenticated = 1;
	if (pair->link_key_type == PAIR_KEY_P256 || pair->link_key_type == PAIR_KEY_P256_MITM)
		bond.secure = 1;

	/* Written whole. */
	error = btd_keys_write(pair->keys_folder, pair->session->address, &bond);
	memset(&bond, 0, sizeof(bond));
	if (error != 0)
		pair_fail(pair, "store");
}

/*
 * Stores the new LE bond: the LTK (with EDIV and Rand for legacy), what it
 * is worth, and the device's identity; the file is named by the identity
 * address when the device gave one (review S11).
 */
static void
pair_store_le(
	struct btd_pair *pair)
{
	struct btd_bond bond;
	const struct btd_smp_keys *keys;
	int error;

	/* The bond under the connection's address, or the identity the device gave. */
	keys = &pair->smp.keys;
	memset(&bond, 0, sizeof(bond));
	memcpy(bond.address, pair->address, BTD_ADDRESS_BYTES);
	bond.type = pair->type;
	if (keys->have_identity) {
		memcpy(bond.address, keys->identity, BTD_ADDRESS_BYTES);
		bond.type = BTD_ADDRESS_LE_PUBLIC;
		if (keys->identity_type != 0U)
			bond.type = BTD_ADDRESS_LE_RANDOM;
	}

	/* A bond under that identity is not written over (it is forgotten first, review S11). */
	error = btd_keys_read(pair->keys_folder, pair->session->address, bond.address, bond.type, &pair->stored);
	memset(&pair->stored, 0, sizeof(pair->stored));
	if (error != ENOENT) {
		pair_fail(pair, "bonded");
		return;
	}

	/* The name the last scan saw, and the keys. */
	pair_name(pair, pair->address, pair->type, bond.name, sizeof(bond.name));
	bond.have_ltk = keys->have_ltk;
	memcpy(bond.ltk, keys->ltk, sizeof(bond.ltk));
	bond.ediv = keys->ediv;
	memcpy(bond.rand, keys->rand, sizeof(bond.rand));
	bond.key_size = keys->key_size;
	bond.authenticated = keys->authenticated;
	bond.secure = keys->secure;
	bond.legacy = 0;
	if (!keys->secure)
		bond.legacy = 1;
	bond.have_irk = keys->have_irk;
	memcpy(bond.irk, keys->irk, sizeof(bond.irk));
	bond.have_identity = keys->have_identity;
	bond.identity_type = keys->identity_type;
	memcpy(bond.identity, keys->identity, sizeof(bond.identity));

	/* Written whole. */
	error = btd_keys_write(pair->keys_folder, pair->session->address, &bond);
	memset(&bond, 0, sizeof(bond));
	if (error != 0)
		pair_fail(pair, "store");
}

/*
 * Tries the data path: an L2CAP Information Request for the extended
 * features (review S10).  The answer only goes in the log; a missing one
 * does not fail the pairing.
 */
static void
pair_probe(
	struct btd_pair *pair)
{
	uint8_t request[16];
	size_t length;
	int error;

	/* The request on the signalling channel. */
	error = btd_l2cap_information(&pair->l2cap, request, sizeof(request), &length);
	if (error == 0)
		error = btd_session_send(pair->session, pair->handle, BTD_CID_SIGNALLING, request, length);

	/* The node went. */
	if (error == ENODEV) {
		btd_pair_lost(pair);
		return;
	}

	/* No request went: the pairing is done all the same. */
	if (error != 0) {
		pair_succeed(pair);
		return;
	}

	/* The answer is awaited for BTD_PAIR_PROBE_MS. */
	pair->state = PAIR_PROBING;
	pair->state_deadline = btd_now_ms() + BTD_PAIR_PROBE_MS;
}

/* Takes a signalling frame: its answers go back, an LE parameter update is carried out, the probe's answer ends the probe. */
static void
pair_signal(
	struct btd_pair *pair,
	int le,
	const uint8_t *payload,
	size_t length)
{
	struct btd_signal_effect effect;
	uint8_t answer[BTD_SIGNAL_MAX];
	uint8_t update[14];
	uint16_t cid;
	size_t answer_length;
	int error;

	/* The commands of the frame, answered. */
	(void)btd_l2cap_signal(&pair->l2cap, pair->handle, le, payload, length, answer, sizeof(answer), &answer_length, &effect);
	cid = BTD_CID_SIGNALLING;
	if (le)
		cid = BTD_CID_LE_SIGNALLING;
	if (answer_length != 0U) {
		error = btd_session_send(pair->session, pair->handle, cid, answer, answer_length);
		if (error == ENODEV) {
			btd_pair_lost(pair);
			return;
		}
	}

	/* LE Connection Update with the parameters the device asked for and bluetoothd accepted (review S9). */
	if (effect.update) {
		pair_put16(update + 0, pair->handle);
		pair_put16(update + 2, effect.interval_min);
		pair_put16(update + 4, effect.interval_max);
		pair_put16(update + 6, effect.latency);
		pair_put16(update + 8, effect.timeout);
		pair_put16(update + 10, 0x0000U);
		pair_put16(update + 12, 0x0000U);
		error = btd_session_command(pair->session, PAIR_LE_CONNECTION_UPDATE, update, sizeof(update));
		if (error == ENODEV) {
			btd_pair_lost(pair);
			return;
		}
	}

	/* The probe's answer: the pairing is done. */
	if (effect.information && pair->state == PAIR_PROBING) {
		pair->probed = 1;
		pair_succeed(pair);
	}
}

/*
 * Carries out what the Security Manager asks: a PDU to send, the
 * controller's public key and DHKey, the agent's question or passkey, the
 * encryption, the keys' store at the end, or the failure.
 */
static void
pair_smp_actions(
	struct btd_pair *pair,
	unsigned actions)
{
	int error;

	/* A PDU to the device (Pairing Failed among them); the Security Manager's 30 seconds start again. */
	if ((actions & BTD_SMP_SEND) != 0U) {
		error = btd_session_send(pair->session, pair->handle, BTD_CID_SMP, pair->smp.out, pair->smp.out_length);
		if (error == ENODEV) {
			btd_pair_lost(pair);
			return;
		}
		pair->smp_deadline = btd_now_ms() + BTD_PAIR_SMP_MS;
	}

	/* The failure: the link is ended with why. */
	if ((actions & BTD_SMP_FAILED) != 0U) {
		pair_fail(pair, pair_smp_why(pair));
		return;
	}

	/* The controller's public key (LE Read Local P-256 Public Key; it answers with an LE meta event). */
	if ((actions & BTD_SMP_READ_KEY) != 0U) {
		error = pair_command(pair, PAIR_LE_READ_P256, NULL, 0U, "controller");
		if (error != 0)
			return;
	}

	/* The DHKey of the device's key (LE Generate DHKey; an LE meta event again). */
	if ((actions & BTD_SMP_DHKEY) != 0U) {
		error = pair_command(pair, PAIR_LE_DHKEY, pair->smp.remote_key, sizeof(pair->smp.remote_key), "controller");
		if (error != 0)
			return;
	}

	/* A passkey the device types, shown to the user. */
	if ((actions & BTD_SMP_SHOW) != 0U && pair->ask != NULL)
		pair->ask(pair->context, BTD_PAIR_ASK_PASSKEY, pair->smp.number);

	/* A number the user confirms. */
	if ((actions & BTD_SMP_CONFIRM) != 0U)
		pair_ask(pair, BTD_PAIR_ASK_CONFIRM, pair->smp.number);

	/* The encryption: Just Works waits for the agent to agree first (design section 6.5). */
	if ((actions & BTD_SMP_ENCRYPT) != 0U) {
		if (!pair->smp.keys.authenticated && pair->agent) {
			pair->encrypt_held = 1;
			pair_ask(pair, BTD_PAIR_ASK_CONSENT, 0U);
			return;
		}

		/* Encrypted now. */
		pair_le_encrypt(pair);
		return;
	}

	/* The end: the keys stored, then the link ended. */
	if ((actions & BTD_SMP_DONE) != 0U) {
		pair->smp_deadline = 0U;
		pair_store_le(pair);
		if (pair->state == PAIR_DISCONNECTING || pair->state == PAIR_IDLE)
			return;
		pair_succeed(pair);
	}
}

/* Ends a pairing that succeeded: the PAIRED line, then the disconnection. */
static void
pair_succeed(
	struct btd_pair *pair)
{
	char address[24];
	int authenticated;
	int secure;
	int legacy;
	unsigned key_size;

	/* What the bond is worth: BR/EDR's from the key type, LE's from the Security Manager. */
	authenticated = 0;
	secure = 0;
	legacy = 0;
	key_size = pair->key_size;
	if (pair_le(pair)) {
		authenticated = pair->smp.keys.authenticated;
		secure = pair->smp.keys.secure;
		if (!secure)
			legacy = 1;
		key_size = pair->smp.keys.key_size;
	} else if (pair->used_stored) {
		authenticated = pair->stored.authenticated;
		secure = pair->stored.secure;
	} else {
		if (pair->link_key_type == PAIR_KEY_P192_MITM || pair->link_key_type == PAIR_KEY_P256_MITM)
			authenticated = 1;
		if (pair->link_key_type == PAIR_KEY_P256 || pair->link_key_type == PAIR_KEY_P256_MITM)
			secure = 1;
	}

	/* The line. */
	btd_format_address(pair->address, address, sizeof(address));
	(void)snprintf(pair->answer,
		       sizeof(pair->answer),
		       "PAIRED address=%s type=%s authenticated=%d secure=%d legacy=%d key_size=%u stored=%d l2cap=%d",
		       address,
		       btd_address_type_name(pair->type),
		       authenticated,
		       secure,
		       legacy,
		       key_size,
		       pair->used_stored,
		       pair->probed);

	/* The connection ends. */
	pair_finish(pair);
}

/* Ends a pairing that failed: the ERROR line with why, then the disconnection. */
static void
pair_fail(
	struct btd_pair *pair,
	const char *why)
{
	/* Nothing under way, or ending already. */
	if (pair->state == PAIR_IDLE || pair->state == PAIR_DISCONNECTING)
		return;

	/* The line. */
	if (why == NULL)
		why = "protocol";
	(void)snprintf(pair->answer, sizeof(pair->answer), "ERROR %s", why);

	/* The connection ends. */
	pair_finish(pair);
}

/* Ends the connection when there is one (the end is told on its Disconnection Complete), else tells the end now. */
static void
pair_finish(
	struct btd_pair *pair)
{
	uint8_t disconnect[3];
	int error;

	/* No question waits any more. */
	pair->asked = 0;
	pair->agent_deadline = 0U;
	pair->smp_deadline = 0U;

	/* No connection: the end now. */
	if (!pair->connected) {
		pair_deliver(pair);
		return;
	}

	/* Disconnect: the user ended it. */
	pair_put16(disconnect, pair->handle);
	disconnect[2] = PAIR_REASON_USER;
	error = btd_session_command(pair->session, PAIR_DISCONNECT, disconnect, sizeof(disconnect));
	if (error != 0) {
		pair->connected = 0;
		pair_deliver(pair);
		return;
	}

	/* Disconnection Complete is awaited for BTD_PAIR_CLOSE_MS. */
	pair->state = PAIR_DISCONNECTING;
	pair->state_deadline = btd_now_ms() + BTD_PAIR_CLOSE_MS;
}

/* Tells the end and forgets the pairing (the keys it held are wiped). */
static void
pair_deliver(
	struct btd_pair *pair)
{
	char answer[BTD_PAIR_ANSWER_MAX];

	/* Nothing under way from now on (a new pairing may start inside the hook). */
	memcpy(answer, pair->answer, sizeof(answer));
	pair->state = PAIR_IDLE;
	pair->state_deadline = 0U;
	pair->total_deadline = 0U;
	pair->smp_deadline = 0U;
	pair->agent_deadline = 0U;
	pair->asked = 0;
	pair->connected = 0;

	/* The keys of this pairing are not kept in memory. */
	memset(&pair->smp, 0, sizeof(pair->smp));
	memset(pair->link_key, 0, sizeof(pair->link_key));
	memset(&pair->stored, 0, sizeof(pair->stored));

	/* The daemon hears the end. */
	if (pair->done != NULL)
		pair->done(pair->context, answer);
}

/*
 * Cancels a connection not made yet (Create Connection Cancel, or LE's):
 * the connection's end is awaited and the pairing ends with why.
 */
static void
pair_cancel(
	struct btd_pair *pair,
	const char *why)
{
	uint16_t opcode;
	int error;

	/* The cancel of the kind of connection. */
	pair->pending_error = why;
	pair->state = PAIR_CANCELLING;
	pair->state_deadline = btd_now_ms() + BTD_PAIR_CLOSE_MS;
	(void)snprintf(pair->answer, sizeof(pair->answer), "ERROR %s", why);
	if (pair_le(pair)) {
		opcode = PAIR_LE_CREATE_CANCEL;
		error = btd_session_command(pair->session, opcode, NULL, 0U);
	} else {
		opcode = PAIR_CREATE_CANCEL;
		error = btd_session_command(pair->session, opcode, pair->address, BTD_ADDRESS_BYTES);
	}

	/* The node went. */
	if (error == ENODEV) {
		btd_pair_lost(pair);
		return;
	}

	/* A cancel refused (the connection was made meanwhile, or failed): its event ends the pairing, or the deadline. */
}

/*
 * Sends a command of the pairing; a failure ends the pairing with why (or
 * as lost when the node went).  Returns 0 or the command's error.
 */
static int
pair_command(
	struct btd_pair *pair,
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count,
	const char *why)
{
	int error;

	/* The command and its answer. */
	error = btd_session_command(pair->session, opcode, parameters, count);
	if (error == 0)
		return 0;

	/* The node went, or the controller was reset: nothing more can be sent. */
	if (error == ENODEV || error == ECONNRESET) {
		btd_pair_lost(pair);
		return error;
	}

	/* Refused or not answered. */
	pair_fail(pair, why);
	return error;
}

/* Sends a reply led by an address (and more parameters), whose answer only matters when the node went. */
static void
pair_reply(
	struct btd_pair *pair,
	uint16_t opcode,
	const uint8_t *address,
	const uint8_t *more,
	size_t more_length)
{
	uint8_t parameters[BTD_ADDRESS_BYTES + 16U];
	int error;

	/* The address, then the rest. */
	memcpy(parameters, address, BTD_ADDRESS_BYTES);
	if (more_length > sizeof(parameters) - BTD_ADDRESS_BYTES)
		more_length = sizeof(parameters) - BTD_ADDRESS_BYTES;
	if (more_length != 0U)
		memcpy(parameters + BTD_ADDRESS_BYTES, more, more_length);

	/* Sent; a refusal is the controller's to report in its events. */
	error = btd_session_command(pair->session, opcode, parameters, BTD_ADDRESS_BYTES + more_length);
	memset(parameters, 0, sizeof(parameters));
	if (error == ENODEV)
		btd_pair_lost(pair);
}

/* Ends a connection the pairing did not ask for (a device that connected on its own). */
static void
pair_disconnect_other(
	struct btd_pair *pair,
	uint16_t handle)
{
	uint8_t disconnect[3];
	int error;

	/* Disconnect: the connection is not acceptable. */
	pair->refused++;
	pair_put16(disconnect, handle);
	disconnect[2] = PAIR_REASON_USER;
	error = btd_session_command(pair->session, PAIR_DISCONNECT, disconnect, sizeof(disconnect));
	if (error == ENODEV)
		btd_pair_lost(pair);
}

/* Tells whether an address (least significant byte first) is the pairing's device while a pairing runs. */
static int
pair_ours(
	const struct btd_pair *pair,
	const uint8_t *address)
{
	int same;

	/* No pairing. */
	if (pair->state == PAIR_IDLE)
		return 0;

	/* The same six bytes. */
	same = memcmp(address, pair->address, BTD_ADDRESS_BYTES);
	if (same == 0)
		return 1;

	/* Another device. */
	return 0;
}

/* Tells whether the pairing is LE's. */
static int
pair_le(
	const struct btd_pair *pair)
{
	/* LE's two kinds of address. */
	if (pair->type == BTD_ADDRESS_LE_PUBLIC || pair->type == BTD_ADDRESS_LE_RANDOM)
		return 1;

	/* BR/EDR. */
	return 0;
}

/* Copies the name the last scan saw for a device, or "" when it saw none. */
static void
pair_name(
	const struct btd_pair *pair,
	const uint8_t *address,
	unsigned type,
	char *name,
	size_t size)
{
	const struct btd_device *device;
	unsigned index;
	int same;

	/* Each device of the last scan. */
	name[0] = '\0';
	for (index = 0U; index < pair->session->devices.count; index++) {
		device = &pair->session->devices.entries[index];
		if (device->type != type)
			continue;
		same = memcmp(device->address, address, BTD_ADDRESS_BYTES);
		if (same != 0)
			continue;

		/* Its name. */
		(void)snprintf(name, size, "%s", device->name);
		return;
	}
}

/* Names why the Security Manager ended the pairing, as the daemon's answer says it. */
static const char *
pair_smp_why(
	const struct btd_pair *pair)
{
	/* Each reason the Security Manager names. */
	switch (pair->smp.why) {
	case BTD_SMP_WHY_KEY_SIZE:
		return "key-size";
	case BTD_SMP_WHY_DEBUG_KEY:
		return "debug-key";
	case BTD_SMP_WHY_REFLECTION:
		return "reflection";
	case BTD_SMP_WHY_DHKEY:
	case BTD_SMP_WHY_CHECK:
		return "check";
	case BTD_SMP_WHY_REJECTED:
		return "rejected";
	case BTD_SMP_WHY_ENCRYPTION:
		return "encryption";
	case BTD_SMP_WHY_CONTROLLER:
		return "controller";
	case BTD_SMP_WHY_CALLER:
		return "timeout";
	default:
		break;
	}

	/* The protocol broken. */
	return "protocol";
}

/* Writes a 16-bit value least significant byte first. */
static void
pair_put16(
	uint8_t *bytes,
	uint16_t value)
{
	/* The two bytes. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)(value >> 8);
}

/* Reads a connection handle (two bytes, least significant first, without the flags above its 12 bits). */
static uint16_t
pair_handle(
	const uint8_t *bytes)
{
	unsigned value;

	/* The two bytes, then the handle's bits alone. */
	value = (unsigned)bytes[0] | ((unsigned)bytes[1] << 8);
	value &= 0x0fffU;

	/* Succeeded: the handle. */
	return (uint16_t)value;
}

/* Asks the agent (a number to confirm, or an agreement), answered within BTD_PAIR_AGENT_MS. */
static void
pair_ask(
	struct btd_pair *pair,
	unsigned kind,
	uint32_t number)
{
	/* The question waits for btd_pair_answer, or the deadline's no. */
	pair->asked = 1;
	pair->asked_kind = kind;
	pair->agent_deadline = btd_now_ms() + BTD_PAIR_AGENT_MS;

	/* Asked. */
	if (pair->ask != NULL)
		pair->ask(pair->context, kind, number);
}

/* Starts LE's encryption with the key the Security Manager gave (LE Enable Encryption: handle, Rand, EDIV, the key). */
static void
pair_le_encrypt(
	struct btd_pair *pair)
{
	uint8_t encrypt[28];

	/* Nothing held any more. */
	pair->encrypt_held = 0;

	/* The command; Encryption Change answers it. */
	pair_put16(encrypt, pair->handle);
	memcpy(encrypt + 2, pair->smp.encrypt_rand, 8U);
	pair_put16(encrypt + 10, pair->smp.encrypt_ediv);
	memcpy(encrypt + 12, pair->smp.encrypt_key, 16U);
	(void)pair_command(pair, PAIR_LE_ENCRYPT, encrypt, sizeof(encrypt), "encryption");
	memset(encrypt, 0, sizeof(encrypt));
}

/*
 * Tells whether the pairing's LE address is a resolvable private address
 * of a bonded device: its hash is ah of a stored IRK and its random part
 * (Core Vol 6 Part B §1.3.2.3).
 */
static int
pair_resolved_bond(
	const struct btd_pair *pair)
{
	struct btd_bond bonds[PAIR_BONDS_MAX];
	uint8_t irk[16];
	uint8_t prand[3];
	uint8_t hash[3];
	uint8_t expected[3];
	unsigned count;
	unsigned index;
	int same;
	int error;

	/* Only a random address whose top two bits are 01 is resolvable. */
	if (pair->type != BTD_ADDRESS_LE_RANDOM || (pair->address[5] & 0xc0U) != 0x40U)
		return 0;

	/* The bonds of the controller. */
	error = btd_keys_list(pair->keys_folder, pair->session->address, bonds, PAIR_BONDS_MAX, &count);
	if (error != 0)
		return 0;

	/* prand (the top three bytes) and the hash (the bottom three), most significant first. */
	prand[0] = pair->address[5];
	prand[1] = pair->address[4];
	prand[2] = pair->address[3];
	expected[0] = pair->address[2];
	expected[1] = pair->address[1];
	expected[2] = pair->address[0];

	/* Each bond's IRK (stored least significant first, as the PDU gave it). */
	for (index = 0U; index < count; index++) {
		if (!bonds[index].have_irk)
			continue;
		pair_reverse(irk, bonds[index].irk, sizeof(irk));
		btd_smp_ah(irk, prand, hash);
		same = memcmp(hash, expected, sizeof(hash));
		if (same == 0) {
			memset(bonds, 0, sizeof(bonds));
			memset(irk, 0, sizeof(irk));
			return 1;
		}
	}

	/* No bond's: the keys read are not kept. */
	memset(bonds, 0, sizeof(bonds));
	memset(irk, 0, sizeof(irk));
	return 0;
}

/* Copies bytes in the other order. */
static void
pair_reverse(
	uint8_t *to,
	const uint8_t *from,
	size_t length)
{
	size_t index;

	/* The last first. */
	for (index = 0U; index < length; index++)
		to[index] = from[length - 1U - index];
}
