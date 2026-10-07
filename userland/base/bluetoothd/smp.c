/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's Security Manager as the initiator (ws143-p004, see smp.h;
 * Core 5.4 Vol 3 Part H §2.3 and §3).
 *
 * The PDUs carry values least significant byte first; the Core's
 * functions (crypto.c) take them most significant byte first, so every
 * value is turned round on its way in and out.
 */

#include "userland/base/bluetoothd/smp.h"
#include "userland/base/bluetoothd/crypto.h"

#include <string.h>

/* The PDUs (Core Vol 3 Part H §3.3). */
#define SMP_PAIRING_REQUEST		0x01U
#define SMP_PAIRING_RESPONSE		0x02U
#define SMP_PAIRING_CONFIRM		0x03U
#define SMP_PAIRING_RANDOM		0x04U
#define SMP_PAIRING_FAILED		0x05U
#define SMP_ENCRYPTION_INFORMATION	0x06U
#define SMP_CENTRAL_IDENTIFICATION	0x07U
#define SMP_IDENTITY_INFORMATION	0x08U
#define SMP_IDENTITY_ADDRESS		0x09U
#define SMP_SECURITY_REQUEST		0x0bU
#define SMP_PUBLIC_KEY			0x0cU
#define SMP_DHKEY_CHECK			0x0dU

/* The AuthReq bits: bonding, MITM, Secure Connections. */
#define SMP_AUTH_BONDING		0x01U
#define SMP_AUTH_MITM			0x04U
#define SMP_AUTH_SC			0x08U

/* The key distribution bits: EncKey, IdKey. */
#define SMP_KEY_ENC			0x01U
#define SMP_KEY_ID			0x02U

/* The keys awaited from the responder (bits of keys_awaited). */
#define SMP_AWAIT_ENC_INFO		0x01U
#define SMP_AWAIT_CENTRAL_ID		0x02U
#define SMP_AWAIT_ID_INFO		0x04U
#define SMP_AWAIT_ID_ADDRESS		0x08U

/* The IO capabilities a responder may have. */
#define SMP_IO_DISPLAY_ONLY		0x00U
#define SMP_IO_DISPLAY_YES_NO		0x01U
#define SMP_IO_KEYBOARD_ONLY		0x02U
#define SMP_IO_NO_IO			0x03U
#define SMP_IO_KEYBOARD_DISPLAY		0x04U

/* The methods: Just Works, Numeric Comparison, Passkey Entry with bluetoothd showing the passkey. */
#define SMP_METHOD_JUST_WORKS		0U
#define SMP_METHOD_NUMERIC		1U
#define SMP_METHOD_PASSKEY		2U

/* The states. */
#define SMP_STATE_IDLE			0U
#define SMP_STATE_RESPONSE		1U
#define SMP_STATE_PUBLIC_KEYS		2U
#define SMP_STATE_CONFIRM		3U
#define SMP_STATE_RANDOM		4U
#define SMP_STATE_CHECK			5U
#define SMP_STATE_ENCRYPT		6U
#define SMP_STATE_KEYS			7U
#define SMP_STATE_DONE			8U
#define SMP_STATE_FAILED		9U

/* Passkey Entry's rounds, the least and the largest key size the Core allows, and the number of digits shown. */
#define SMP_PASSKEY_ROUNDS		20U
#define SMP_KEY_SIZE_MIN		7U
#define SMP_KEY_SIZE_MAX		16U
#define SMP_DIGITS			1000000U

/*
 * The Core's debug public key (Vol 3 Part H §2.3.5.6.1), least significant
 * byte first as a PDU carries it: a responder that sends it is refused.
 */
static const uint8_t smp_debug_x_msb[32] = {
	0x20U, 0xb0U, 0x03U, 0xd2U, 0xf2U, 0x97U, 0xbeU, 0x2cU, 0x5eU, 0x2cU, 0x83U, 0xa7U, 0xe9U, 0xf9U, 0xa5U, 0xb9U,
	0xefU, 0xf4U, 0x91U, 0x11U, 0xacU, 0xf4U, 0xfdU, 0xdbU, 0xccU, 0x03U, 0x01U, 0x48U, 0x0eU, 0x35U, 0x9dU, 0xe6U
};

static void smp_reverse(uint8_t *to, const uint8_t *from, size_t length);
static void smp_address(uint8_t *to, uint8_t type, const uint8_t *address);
static void smp_passkey_value(uint32_t passkey, uint8_t *value);
static unsigned smp_send(struct btd_smp *smp, uint8_t code, const uint8_t *value, size_t length);
static unsigned smp_send_reversed(struct btd_smp *smp, uint8_t code, const uint8_t *msb, size_t length);
static unsigned smp_response(struct btd_smp *smp, const uint8_t *pdu, size_t length);
static unsigned smp_public_key(struct btd_smp *smp, const uint8_t *pdu, size_t length);
static unsigned smp_confirm(struct btd_smp *smp, const uint8_t *pdu, size_t length);
static unsigned smp_random(struct btd_smp *smp, const uint8_t *pdu, size_t length);
static unsigned smp_check(struct btd_smp *smp, const uint8_t *pdu, size_t length);
static unsigned smp_key(struct btd_smp *smp, const uint8_t *pdu, size_t length);
static unsigned smp_send_own_key(struct btd_smp *smp);
static unsigned smp_passkey_confirm(struct btd_smp *smp);
static unsigned smp_try_check(struct btd_smp *smp);
static unsigned smp_legacy_confirm(struct btd_smp *smp);
static void smp_f4(const struct btd_smp *smp, int local_first, const uint8_t *nonce, uint8_t z, uint8_t *confirm);
static void smp_c1(const struct btd_smp *smp, const uint8_t *nonce, uint8_t *confirm);
static unsigned smp_done_or_keys(struct btd_smp *smp);
static unsigned smp_refuse(struct btd_smp *smp, uint8_t reason, unsigned why);

/*
 * Prepares a pairing: bluetoothd's IO capability (DisplayYesNo with an
 * agent, NoInputNoOutput without), both addresses, and the random source.
 */
void
btd_smp_init(
	struct btd_smp *smp,
	uint8_t io_capability,
	uint8_t initiator_type,
	const uint8_t *initiator,
	uint8_t responder_type,
	const uint8_t *responder,
	btd_random_fn random,
	void *random_context)
{
	/* Nothing done yet. */
	memset(smp, 0, sizeof(*smp));
	smp->state = SMP_STATE_IDLE;
	smp->io_capability = io_capability;
	smp->initiator_type = initiator_type;
	memcpy(smp->initiator, initiator, 6U);
	smp->responder_type = responder_type;
	memcpy(smp->responder, responder, 6U);
	smp->random = random;
	smp->random_context = random_context;
}

/*
 * Starts the pairing: the Pairing Request (bonding, MITM with an agent,
 * Secure Connections, a key of 16 bytes, no keys from bluetoothd, the
 * responder's EncKey and IdKey).
 */
unsigned
btd_smp_start(
	struct btd_smp *smp)
{
	uint8_t authentication;
	unsigned actions;

	/* The AuthReq: MITM only when an agent can confirm or show. */
	authentication = SMP_AUTH_BONDING | SMP_AUTH_SC;
	if (smp->io_capability == BTD_SMP_DISPLAY_YES_NO)
		authentication |= SMP_AUTH_MITM;

	/* The request, kept for c1 and f6. */
	smp->request[0] = SMP_PAIRING_REQUEST;
	smp->request[1] = smp->io_capability;
	smp->request[2] = 0x00U;
	smp->request[3] = authentication;
	smp->request[4] = SMP_KEY_SIZE_MAX;
	smp->request[5] = 0x00U;
	smp->request[6] = SMP_KEY_ENC | SMP_KEY_ID;
	memcpy(smp->out, smp->request, sizeof(smp->request));
	smp->out_length = sizeof(smp->request);

	/* Succeeded: sent, the response awaited. */
	smp->state = SMP_STATE_RESPONSE;
	actions = BTD_SMP_SEND;
	return actions;
}

/*
 * Takes a PDU from the responder (on the SMP channel).  Returns what the
 * caller is to do.
 */
unsigned
btd_smp_input(
	struct btd_smp *smp,
	const uint8_t *pdu,
	size_t length)
{
	unsigned actions;

	/* Nothing more after the end. */
	if (smp->state == SMP_STATE_DONE || smp->state == SMP_STATE_FAILED || smp->state == SMP_STATE_IDLE)
		return 0U;

	/* An empty PDU is not one. */
	if (length == 0U) {
		actions = smp_refuse(smp, BTD_SMP_INVALID, BTD_SMP_WHY_PROTOCOL);
		return actions;
	}

	/* The responder gave up. */
	if (pdu[0] == SMP_PAIRING_FAILED) {
		smp->failure = BTD_SMP_UNSPECIFIED;
		if (length >= 2U)
			smp->failure = pdu[1];
		smp->failure_sent = 1;
		smp->why = BTD_SMP_WHY_REJECTED;
		smp->state = SMP_STATE_FAILED;
		return BTD_SMP_FAILED;
	}

	/* Each PDU in the state that awaits it; anything else breaks the protocol. */
	actions = 0U;
	switch (pdu[0]) {
	case SMP_PAIRING_RESPONSE:
		actions = smp_response(smp, pdu, length);
		break;
	case SMP_PUBLIC_KEY:
		actions = smp_public_key(smp, pdu, length);
		break;
	case SMP_PAIRING_CONFIRM:
		actions = smp_confirm(smp, pdu, length);
		break;
	case SMP_PAIRING_RANDOM:
		actions = smp_random(smp, pdu, length);
		break;
	case SMP_DHKEY_CHECK:
		actions = smp_check(smp, pdu, length);
		break;
	case SMP_ENCRYPTION_INFORMATION:
	case SMP_CENTRAL_IDENTIFICATION:
	case SMP_IDENTITY_INFORMATION:
	case SMP_IDENTITY_ADDRESS:
		actions = smp_key(smp, pdu, length);
		break;
	case SMP_SECURITY_REQUEST:
		/* The pairing runs already. */
		break;
	default:
		actions = smp_refuse(smp, BTD_SMP_NOT_SUPPORTED, BTD_SMP_WHY_PROTOCOL);
		break;
	}

	/* Succeeded: what is to be done. */
	return actions;
}

/*
 * Takes the controller's public key (LE Read Local P-256 Public Key
 * Complete: X then Y, least significant byte first) and sends it.
 */
unsigned
btd_smp_local_key(
	struct btd_smp *smp,
	uint8_t status,
	const uint8_t *key)
{
	uint8_t x_msb[32];
	unsigned actions;
	int same;

	/* Only while the keys are exchanged. */
	if (smp->state != SMP_STATE_PUBLIC_KEYS || smp->have_local_key)
		return 0U;

	/* A controller that failed ends the pairing. */
	if (status != 0U) {
		actions = smp_refuse(smp, BTD_SMP_UNSPECIFIED, BTD_SMP_WHY_CONTROLLER);
		return actions;
	}

	/* A controller in its debug mode gives the Core's debug key: anyone could read the link (design section 6.2). */
	smp_reverse(x_msb, key, 32U);
	same = memcmp(x_msb, smp_debug_x_msb, sizeof(x_msb));
	if (same == 0) {
		actions = smp_refuse(smp, BTD_SMP_UNSPECIFIED, BTD_SMP_WHY_DEBUG_KEY);
		return actions;
	}

	/* Kept and sent. */
	memcpy(smp->local_key, key, sizeof(smp->local_key));
	smp->have_local_key = 1;
	actions = smp_send_own_key(smp);
	return actions;
}

/*
 * Takes the controller's DHKey (LE Generate DHKey Complete, least
 * significant byte first); a failed one (an invalid point of the
 * responder's, among others) ends the pairing.
 */
unsigned
btd_smp_dhkey(
	struct btd_smp *smp,
	uint8_t status,
	const uint8_t *dhkey)
{
	unsigned actions;

	/* Only once it was asked for. */
	if (!smp->have_remote_key || smp->have_dhkey || smp->state >= SMP_STATE_ENCRYPT)
		return 0U;

	/* A failure (the responder's key is not on the curve, among others). */
	if (status != 0U) {
		actions = smp_refuse(smp, BTD_SMP_DHKEY_FAILED, BTD_SMP_WHY_DHKEY);
		return actions;
	}

	/* Kept; the check goes when everything else is there. */
	memcpy(smp->dhkey, dhkey, sizeof(smp->dhkey));
	smp->have_dhkey = 1;
	actions = smp_try_check(smp);
	return actions;
}

/*
 * Takes the agent's answer to a Numeric Comparison: yes goes on, no ends
 * the pairing (Numeric Comparison Failed).
 */
unsigned
btd_smp_agent(
	struct btd_smp *smp,
	int accepted)
{
	unsigned actions;

	/* Only a question asked. */
	if (smp->method != SMP_METHOD_NUMERIC || smp->agent_answered || smp->state != SMP_STATE_CHECK)
		return 0U;

	/* No. */
	smp->agent_answered = 1;
	smp->agent_accepted = accepted;
	if (!accepted) {
		actions = smp_refuse(smp, BTD_SMP_NUMERIC_FAILED, BTD_SMP_WHY_REJECTED);
		return actions;
	}

	/* Yes: the check goes when everything else is there. */
	actions = smp_try_check(smp);
	return actions;
}

/*
 * Takes the end of the encryption the caller started (Encryption Change):
 * on, the responder's keys are awaited; otherwise the pairing failed.
 */
unsigned
btd_smp_encrypted(
	struct btd_smp *smp,
	uint8_t status,
	int on)
{
	unsigned actions;

	/* Only the encryption asked for. */
	if (smp->state != SMP_STATE_ENCRYPT)
		return 0U;

	/* Not encrypted. */
	if (status != 0U || !on) {
		smp->failure = BTD_SMP_UNSPECIFIED;
		smp->why = BTD_SMP_WHY_ENCRYPTION;
		smp->state = SMP_STATE_FAILED;
		return BTD_SMP_FAILED;
	}

	/* The keys, or the end. */
	actions = smp_done_or_keys(smp);
	return actions;
}

/*
 * Ends the pairing with a reason sent to the responder (a timeout, a
 * refusal of the caller's).
 */
unsigned
btd_smp_fail(
	struct btd_smp *smp,
	uint8_t reason)
{
	uint8_t value[1];

	/* Ended already. */
	if (smp->state == SMP_STATE_DONE || smp->state == SMP_STATE_FAILED)
		return 0U;

	/* Pairing Failed with the reason (the caller's own, unless a check named why). */
	if (smp->why == BTD_SMP_WHY_NONE)
		smp->why = BTD_SMP_WHY_CALLER;
	smp->failure = reason;
	smp->state = SMP_STATE_FAILED;
	value[0] = reason;
	(void)smp_send(smp, SMP_PAIRING_FAILED, value, sizeof(value));

	/* Succeeded: sent and failed. */
	return BTD_SMP_SEND | BTD_SMP_FAILED;
}

/* Copies bytes in the other order. */
static void
smp_reverse(
	uint8_t *to,
	const uint8_t *from,
	size_t length)
{
	size_t index;

	/* The last first. */
	for (index = 0U; index < length; index++)
		to[index] = from[length - 1U - index];
}

/* Writes an address as f5 and f6 take it: the type (0 public, 1 random), then the address most significant first. */
static void
smp_address(
	uint8_t *to,
	uint8_t type,
	const uint8_t *address)
{
	/* The type, then the address turned round. */
	to[0] = type;
	smp_reverse(to + 1, address, 6U);
}

/* Writes a passkey as a 128-bit value, most significant first. */
static void
smp_passkey_value(
	uint32_t passkey,
	uint8_t *value)
{
	/* Twelve zero bytes and the passkey. */
	memset(value, 0, 16U);
	value[12] = (uint8_t)(passkey >> 24);
	value[13] = (uint8_t)(passkey >> 16);
	value[14] = (uint8_t)(passkey >> 8);
	value[15] = (uint8_t)passkey;
}

/* Puts a PDU in the out buffer; returns BTD_SMP_SEND. */
static unsigned
smp_send(
	struct btd_smp *smp,
	uint8_t code,
	const uint8_t *value,
	size_t length)
{
	/* The code and the value. */
	smp->out[0] = code;
	memcpy(smp->out + 1, value, length);
	smp->out_length = 1U + length;

	/* Succeeded: to be sent. */
	return BTD_SMP_SEND;
}

/* Puts a PDU whose 16-byte value is given most significant first. */
static unsigned
smp_send_reversed(
	struct btd_smp *smp,
	uint8_t code,
	const uint8_t *msb,
	size_t length)
{
	uint8_t value[32];
	unsigned actions;

	/* The value as the PDU carries it. */
	smp_reverse(value, msb, length);

	/* Sent. */
	actions = smp_send(smp, code, value, length);
	return actions;
}

/*
 * Takes the Pairing Response: the key size must be 16, the method follows
 * from both sides' IO capabilities and AuthReq, and the keys awaited from
 * the responder are those both asked for.
 */
static unsigned
smp_response(
	struct btd_smp *smp,
	const uint8_t *pdu,
	size_t length)
{
	uint8_t passkey_bytes[4];
	uint8_t peer_io;
	int mitm;
	unsigned actions;

	/* Only after our request, and of its size. */
	if (smp->state != SMP_STATE_RESPONSE || length != 7U) {
		actions = smp_refuse(smp, BTD_SMP_INVALID, BTD_SMP_WHY_PROTOCOL);
		return actions;
	}

	/* The response kept for c1 and the IO capability. */
	memcpy(smp->response, pdu, 7U);

	/* A key size the Core does not allow breaks the protocol. */
	if (pdu[4] < SMP_KEY_SIZE_MIN || pdu[4] > SMP_KEY_SIZE_MAX) {
		actions = smp_refuse(smp, BTD_SMP_INVALID, BTD_SMP_WHY_PROTOCOL);
		return actions;
	}

	/* A key shorter than 16 bytes is refused (KNOB, design section 6.2). */
	if (pdu[4] != SMP_KEY_SIZE_MAX) {
		actions = smp_refuse(smp, BTD_SMP_KEY_SIZE, BTD_SMP_WHY_KEY_SIZE);
		return actions;
	}

	/* An IO capability the Core does not have. */
	peer_io = pdu[1];
	if (peer_io > SMP_IO_KEYBOARD_DISPLAY) {
		actions = smp_refuse(smp, BTD_SMP_INVALID, BTD_SMP_WHY_PROTOCOL);
		return actions;
	}

	/* Secure Connections when both sides set the bit; the responder may not give keys we did not ask for. */
	smp->secure = 0;
	if ((pdu[3] & SMP_AUTH_SC) != 0U && (smp->request[3] & SMP_AUTH_SC) != 0U)
		smp->secure = 1;
	if ((pdu[6] & (uint8_t)~smp->request[6]) != 0U || pdu[5] != 0U) {
		actions = smp_refuse(smp, BTD_SMP_INVALID, BTD_SMP_WHY_PROTOCOL);
		return actions;
	}

	/* The keys awaited: identity always as given, the LTK only in legacy (Secure Connections derives it). */
	smp->keys_awaited = 0U;
	if ((pdu[6] & SMP_KEY_ID) != 0U)
		smp->keys_awaited |= SMP_AWAIT_ID_INFO | SMP_AWAIT_ID_ADDRESS;
	if ((pdu[6] & SMP_KEY_ENC) != 0U && !smp->secure)
		smp->keys_awaited |= SMP_AWAIT_ENC_INFO | SMP_AWAIT_CENTRAL_ID;

	/* The method (Core Vol 3 Part H Table 2.8, bluetoothd as the initiator with DisplayYesNo or NoInputNoOutput). */
	mitm = 0;
	if ((pdu[3] & SMP_AUTH_MITM) != 0U || (smp->request[3] & SMP_AUTH_MITM) != 0U)
		mitm = 1;
	smp->method = SMP_METHOD_JUST_WORKS;
	if (mitm && smp->io_capability == BTD_SMP_DISPLAY_YES_NO) {
		if (peer_io == SMP_IO_DISPLAY_YES_NO && smp->secure) {
			smp->method = SMP_METHOD_NUMERIC;
		} else if (peer_io == SMP_IO_KEYBOARD_ONLY) {
			smp->method = SMP_METHOD_PASSKEY;
		} else if (peer_io == SMP_IO_KEYBOARD_DISPLAY) {
			smp->method = SMP_METHOD_PASSKEY;
			if (smp->secure)
				smp->method = SMP_METHOD_NUMERIC;
		}
	}

	/* What the keys will be worth. */
	smp->keys.authenticated = (smp->method != SMP_METHOD_JUST_WORKS);
	smp->keys.secure = smp->secure;
	smp->keys.key_size = SMP_KEY_SIZE_MAX;

	/* Passkey Entry: bluetoothd chooses the passkey and shows it (the responder types it). */
	actions = 0U;
	if (smp->method == SMP_METHOD_PASSKEY) {
		smp->random(smp->random_context, passkey_bytes, sizeof(passkey_bytes));
		smp->passkey = ((uint32_t)passkey_bytes[0] << 24 | (uint32_t)passkey_bytes[1] << 16 |
				(uint32_t)passkey_bytes[2] << 8 | (uint32_t)passkey_bytes[3]) % SMP_DIGITS;
		smp->number = smp->passkey;
		actions |= BTD_SMP_SHOW;
	}

	/* Secure Connections: the controller's public key first. */
	if (smp->secure) {
		smp->state = SMP_STATE_PUBLIC_KEYS;
		actions |= BTD_SMP_READ_KEY;
		return actions;
	}

	/* Legacy: the TK (0 for Just Works, the passkey), then our confirm. */
	memset(smp->tk, 0, sizeof(smp->tk));
	if (smp->method == SMP_METHOD_PASSKEY)
		smp_passkey_value(smp->passkey, smp->tk);
	actions |= smp_legacy_confirm(smp);
	return actions;
}

/* Sends bluetoothd's public key once both the response and the controller's key are there. */
static unsigned
smp_send_own_key(
	struct btd_smp *smp)
{
	unsigned actions;

	/* The key as the controller gave it (the PDU's order). */
	actions = smp_send(smp, SMP_PUBLIC_KEY, smp->local_key, sizeof(smp->local_key));
	return actions;
}

/*
 * Takes the responder's public key: the debug key is refused, the DHKey is
 * asked of the controller, and Passkey Entry sends its first confirm.
 */
static unsigned
smp_public_key(
	struct btd_smp *smp,
	const uint8_t *pdu,
	size_t length)
{
	uint8_t x_msb[32];
	unsigned actions;
	int same;

	/* Only once bluetoothd sent its own, and of its size. */
	if (smp->state != SMP_STATE_PUBLIC_KEYS || !smp->have_local_key || smp->have_remote_key || length != 65U) {
		actions = smp_refuse(smp, BTD_SMP_INVALID, BTD_SMP_WHY_PROTOCOL);
		return actions;
	}

	/* The Core's debug key is not taken from a responder (design section 6.2). */
	smp_reverse(x_msb, pdu + 1, 32U);
	same = memcmp(x_msb, smp_debug_x_msb, sizeof(x_msb));
	if (same == 0) {
		actions = smp_refuse(smp, BTD_SMP_UNSPECIFIED, BTD_SMP_WHY_DEBUG_KEY);
		return actions;
	}

	/* A responder that sends bluetoothd's own key back is reflecting it (the DHKey would be one it can guess). */
	same = memcmp(pdu + 1, smp->local_key, 32U);
	if (same == 0) {
		actions = smp_refuse(smp, BTD_SMP_UNSPECIFIED, BTD_SMP_WHY_REFLECTION);
		return actions;
	}

	/* Kept; the controller computes the DHKey (and checks the point). */
	memcpy(smp->remote_key, pdu + 1, sizeof(smp->remote_key));
	smp->have_remote_key = 1;
	actions = BTD_SMP_DHKEY;
	smp->state = SMP_STATE_CONFIRM;

	/* Passkey Entry: bluetoothd's first confirm; otherwise the responder's confirm comes first. */
	if (smp->method == SMP_METHOD_PASSKEY)
		actions |= smp_passkey_confirm(smp);

	/* Succeeded. */
	return actions;
}

/* Sends Passkey Entry's confirm of this round: Cai = f4(PKax, PKbx, Nai, rai). */
static unsigned
smp_passkey_confirm(
	struct btd_smp *smp)
{
	uint8_t nonce_msb[16];
	uint8_t confirm[16];
	uint8_t bit;
	unsigned actions;

	/* A new nonce, and the round's bit of the passkey. */
	smp->random(smp->random_context, smp->local_nonce, sizeof(smp->local_nonce));
	bit = (uint8_t)(0x80U | ((smp->passkey >> smp->round) & 1U));
	smp_reverse(nonce_msb, smp->local_nonce, 16U);

	/* The confirm. */
	smp_f4(smp, 1, nonce_msb, bit, confirm);
	actions = smp_send_reversed(smp, SMP_PAIRING_CONFIRM, confirm, 16U);
	return actions;
}

/*
 * Takes the responder's confirm: Secure Connections' Just Works and Numeric
 * Comparison answer it with bluetoothd's nonce; Passkey Entry and legacy
 * with theirs.
 */
static unsigned
smp_confirm(
	struct btd_smp *smp,
	const uint8_t *pdu,
	size_t length)
{
	unsigned actions;

	/* Only while a confirm is awaited, and of its size. */
	if (smp->state != SMP_STATE_CONFIRM || length != 17U) {
		actions = smp_refuse(smp, BTD_SMP_INVALID, BTD_SMP_WHY_PROTOCOL);
		return actions;
	}

	/* Their confirm, checked when their nonce comes. */
	memcpy(smp->remote_confirm, pdu + 1, 16U);

	/* Secure Connections' Just Works and Numeric Comparison: a nonce now (Passkey Entry and legacy chose theirs before). */
	if (smp->secure && smp->method != SMP_METHOD_PASSKEY)
		smp->random(smp->random_context, smp->local_nonce, sizeof(smp->local_nonce));

	/* Our nonce. */
	smp->state = SMP_STATE_RANDOM;
	actions = smp_send(smp, SMP_PAIRING_RANDOM, smp->local_nonce, 16U);
	return actions;
}

/*
 * Takes the responder's nonce and checks its confirm.  Secure Connections
 * then shows the number (Numeric Comparison), goes on with the next round
 * (Passkey Entry), or goes to the DHKey check; legacy encrypts with the
 * STK.
 */
static unsigned
smp_random(
	struct btd_smp *smp,
	const uint8_t *pdu,
	size_t length)
{
	uint8_t nonce_msb[16];
	uint8_t local_msb[16];
	uint8_t expected[16];
	uint8_t received[16];
	uint8_t stk[16];
	uint8_t u[32];
	uint8_t v[32];
	uint8_t z;
	unsigned actions;
	int same;

	/* Only after our nonce, and of its size. */
	if (smp->state != SMP_STATE_RANDOM || length != 17U) {
		actions = smp_refuse(smp, BTD_SMP_INVALID, BTD_SMP_WHY_PROTOCOL);
		return actions;
	}

	/* Their nonce, and the confirm it must give. */
	memcpy(smp->remote_nonce, pdu + 1, 16U);
	smp_reverse(nonce_msb, smp->remote_nonce, 16U);
	smp_reverse(received, smp->remote_confirm, 16U);

	/* The responder's confirm, as it must be. */
	if (smp->secure) {
		z = 0U;
		if (smp->method == SMP_METHOD_PASSKEY)
			z = (uint8_t)(0x80U | ((smp->passkey >> smp->round) & 1U));
		smp_f4(smp, 0, nonce_msb, z, expected);
	} else {
		smp_c1(smp, nonce_msb, expected);
	}

	/* The confirm must match. */
	same = memcmp(expected, received, sizeof(expected));
	if (same != 0) {
		actions = smp_refuse(smp, BTD_SMP_CONFIRM_FAILED, BTD_SMP_WHY_CHECK);
		return actions;
	}

	/* Legacy: the STK (s1 of the responder's and our nonce), and the encryption. */
	if (!smp->secure) {
		smp_reverse(local_msb, smp->local_nonce, 16U);
		btd_smp_s1(smp->tk, nonce_msb, local_msb, stk);
		smp_reverse(smp->encrypt_key, stk, 16U);
		smp->encrypt_ediv = 0U;
		memset(smp->encrypt_rand, 0, sizeof(smp->encrypt_rand));
		smp->state = SMP_STATE_ENCRYPT;
		memset(stk, 0, sizeof(stk));
		return BTD_SMP_ENCRYPT;
	}

	/* Passkey Entry: the next round, until the twentieth. */
	if (smp->method == SMP_METHOD_PASSKEY) {
		smp->round++;
		if (smp->round < SMP_PASSKEY_ROUNDS) {
			smp->state = SMP_STATE_CONFIRM;
			actions = smp_passkey_confirm(smp);
			return actions;
		}
	}

	/* The check comes next; Numeric Comparison asks the agent first (g2 of both keys' X and both nonces). */
	smp->state = SMP_STATE_CHECK;
	actions = 0U;
	if (smp->method == SMP_METHOD_NUMERIC) {
		smp_reverse(u, smp->local_key, 32U);
		smp_reverse(v, smp->remote_key, 32U);
		smp_reverse(local_msb, smp->local_nonce, 16U);
		smp->number = btd_smp_g2(u, v, local_msb, nonce_msb) % SMP_DIGITS;
		actions |= BTD_SMP_CONFIRM;
	} else {
		smp->agent_answered = 1;
		smp->agent_accepted = 1;
	}

	/* The check, when the DHKey is there too. */
	actions |= smp_try_check(smp);
	return actions;
}

/*
 * Sends the DHKey check once the nonces, the DHKey and the agent's yes are
 * all there: MacKey and LTK from f5, Ea = f6(MacKey, Na, Nb, rb, IOcapA, A, B).
 */
static unsigned
smp_try_check(
	struct btd_smp *smp)
{
	uint8_t w[32];
	uint8_t na[16];
	uint8_t nb[16];
	uint8_t r[16];
	uint8_t io[3];
	uint8_t a[BTD_SMP_ADDRESS];
	uint8_t b[BTD_SMP_ADDRESS];
	uint8_t ltk[16];
	uint8_t check[16];
	unsigned actions;

	/* Everything must be there, once. */
	if (smp->state != SMP_STATE_CHECK || !smp->have_dhkey || !smp->agent_answered || !smp->agent_accepted || smp->sent_check)
		return 0U;

	/* The values, most significant first. */
	smp_reverse(w, smp->dhkey, 32U);
	smp_reverse(na, smp->local_nonce, 16U);
	smp_reverse(nb, smp->remote_nonce, 16U);
	smp_address(a, smp->initiator_type, smp->initiator);
	smp_address(b, smp->responder_type, smp->responder);
	memset(r, 0, sizeof(r));
	if (smp->method == SMP_METHOD_PASSKEY)
		smp_passkey_value(smp->passkey, r);

	/* MacKey and the LTK. */
	btd_smp_f5(w, na, nb, a, b, smp->mac_key, ltk);
	smp_reverse(smp->keys.ltk, ltk, 16U);
	smp->keys.have_ltk = 1;

	/* Ea, with IOcapA (AuthReq, OOB, IO capability of our request). */
	io[0] = smp->request[3];
	io[1] = smp->request[2];
	io[2] = smp->request[1];
	btd_smp_f6(smp->mac_key, na, nb, r, io, a, b, check);
	smp->sent_check = 1;
	memset(w, 0, sizeof(w));
	memset(ltk, 0, sizeof(ltk));
	actions = smp_send_reversed(smp, SMP_DHKEY_CHECK, check, 16U);
	return actions;
}

/*
 * Takes the responder's DHKey check: Eb = f6(MacKey, Nb, Na, ra, IOcapB, B,
 * A) must match; then the link is encrypted with the LTK (EDIV and Rand 0).
 */
static unsigned
smp_check(
	struct btd_smp *smp,
	const uint8_t *pdu,
	size_t length)
{
	uint8_t na[16];
	uint8_t nb[16];
	uint8_t r[16];
	uint8_t io[3];
	uint8_t a[BTD_SMP_ADDRESS];
	uint8_t b[BTD_SMP_ADDRESS];
	uint8_t expected[16];
	uint8_t received[16];
	unsigned actions;
	int same;

	/* Only after ours, and of its size. */
	if (smp->state != SMP_STATE_CHECK || !smp->sent_check || length != 17U) {
		actions = smp_refuse(smp, BTD_SMP_INVALID, BTD_SMP_WHY_PROTOCOL);
		return actions;
	}

	/* Eb as it must be. */
	smp_reverse(na, smp->local_nonce, 16U);
	smp_reverse(nb, smp->remote_nonce, 16U);
	smp_address(a, smp->initiator_type, smp->initiator);
	smp_address(b, smp->responder_type, smp->responder);
	memset(r, 0, sizeof(r));
	if (smp->method == SMP_METHOD_PASSKEY)
		smp_passkey_value(smp->passkey, r);
	io[0] = smp->response[3];
	io[1] = smp->response[2];
	io[2] = smp->response[1];
	btd_smp_f6(smp->mac_key, nb, na, r, io, b, a, expected);
	smp_reverse(received, pdu + 1, 16U);
	same = memcmp(expected, received, sizeof(expected));
	if (same != 0) {
		actions = smp_refuse(smp, BTD_SMP_DHKEY_FAILED, BTD_SMP_WHY_CHECK);
		return actions;
	}

	/* The encryption with the LTK. */
	memcpy(smp->encrypt_key, smp->keys.ltk, 16U);
	smp->encrypt_ediv = 0U;
	memset(smp->encrypt_rand, 0, sizeof(smp->encrypt_rand));
	smp->state = SMP_STATE_ENCRYPT;
	return BTD_SMP_ENCRYPT;
}

/* Takes one of the responder's keys while they are awaited. */
static unsigned
smp_key(
	struct btd_smp *smp,
	const uint8_t *pdu,
	size_t length)
{
	unsigned awaited;
	unsigned actions;

	/* Which key, and its size. */
	awaited = 0U;
	if (pdu[0] == SMP_ENCRYPTION_INFORMATION && length == 17U)
		awaited = SMP_AWAIT_ENC_INFO;
	if (pdu[0] == SMP_CENTRAL_IDENTIFICATION && length == 11U)
		awaited = SMP_AWAIT_CENTRAL_ID;
	if (pdu[0] == SMP_IDENTITY_INFORMATION && length == 17U)
		awaited = SMP_AWAIT_ID_INFO;
	if (pdu[0] == SMP_IDENTITY_ADDRESS && length == 8U)
		awaited = SMP_AWAIT_ID_ADDRESS;

	/* A key not awaited, or of the wrong size. */
	if (smp->state != SMP_STATE_KEYS || awaited == 0U || (smp->keys_awaited & awaited) == 0U) {
		actions = smp_refuse(smp, BTD_SMP_INVALID, BTD_SMP_WHY_PROTOCOL);
		return actions;
	}

	/* Kept. */
	if (awaited == SMP_AWAIT_ENC_INFO) {
		memcpy(smp->keys.ltk, pdu + 1, 16U);
		smp->keys.have_ltk = 1;
	} else if (awaited == SMP_AWAIT_CENTRAL_ID) {
		smp->keys.ediv = (uint16_t)(pdu[1] | (pdu[2] << 8));
		memcpy(smp->keys.rand, pdu + 3, 8U);
	} else if (awaited == SMP_AWAIT_ID_INFO) {
		memcpy(smp->keys.irk, pdu + 1, 16U);
		smp->keys.have_irk = 1;
	} else {
		smp->keys.identity_type = pdu[1];
		memcpy(smp->keys.identity, pdu + 2, 6U);
		smp->keys.have_identity = 1;
	}

	/* The end once every key came. */
	smp->keys_awaited &= ~awaited;
	actions = smp_done_or_keys(smp);
	return actions;
}

/* Ends the pairing once no key is awaited; otherwise awaits them. */
static unsigned
smp_done_or_keys(
	struct btd_smp *smp)
{
	/* Keys still to come. */
	if (smp->keys_awaited != 0U) {
		smp->state = SMP_STATE_KEYS;
		return 0U;
	}

	/* Succeeded: paired. */
	smp->state = SMP_STATE_DONE;
	return BTD_SMP_DONE;
}

/* Sends legacy pairing's confirm: Mconfirm = c1(TK, Mrand, ...) with a new Mrand. */
static unsigned
smp_legacy_confirm(
	struct btd_smp *smp)
{
	uint8_t nonce_msb[16];
	uint8_t confirm[16];
	unsigned actions;

	/* Mrand. */
	smp->random(smp->random_context, smp->local_nonce, sizeof(smp->local_nonce));
	smp_reverse(nonce_msb, smp->local_nonce, 16U);

	/* The confirm. */
	smp_c1(smp, nonce_msb, confirm);
	smp->state = SMP_STATE_CONFIRM;
	actions = smp_send_reversed(smp, SMP_PAIRING_CONFIRM, confirm, 16U);
	return actions;
}

/* Computes f4 with the local key's X first (bluetoothd's confirm) or the remote's first (the responder's). */
static void
smp_f4(
	const struct btd_smp *smp,
	int local_first,
	const uint8_t *nonce,
	uint8_t z,
	uint8_t *confirm)
{
	uint8_t local_x[32];
	uint8_t remote_x[32];

	/* Both X coordinates, most significant first. */
	smp_reverse(local_x, smp->local_key, 32U);
	smp_reverse(remote_x, smp->remote_key, 32U);

	/* The order the confirm's sender uses. */
	if (local_first) {
		btd_smp_f4(local_x, remote_x, nonce, z, confirm);
	} else {
		btd_smp_f4(remote_x, local_x, nonce, z, confirm);
	}
}

/* Computes legacy pairing's c1 with the TK, a nonce, both PDUs and both addresses. */
static void
smp_c1(
	const struct btd_smp *smp,
	const uint8_t *nonce,
	uint8_t *confirm)
{
	uint8_t preq[7];
	uint8_t pres[7];
	uint8_t ia[6];
	uint8_t ra[6];

	/* The PDUs and the addresses most significant first (the opcode the least significant byte). */
	smp_reverse(preq, smp->request, 7U);
	smp_reverse(pres, smp->response, 7U);
	smp_reverse(ia, smp->initiator, 6U);
	smp_reverse(ra, smp->responder, 6U);

	/* c1. */
	btd_smp_c1(smp->tk, nonce, pres, preq, smp->responder_type, smp->initiator_type, ia, ra, confirm);
}

/* Ends the pairing for a check of this file's, naming why for the daemon. */
static unsigned
smp_refuse(
	struct btd_smp *smp,
	uint8_t reason,
	unsigned why)
{
	unsigned actions;

	/* A pairing that ended keeps the reason it ended with. */
	if (smp->state == SMP_STATE_DONE || smp->state == SMP_STATE_FAILED)
		return 0U;

	/* Why first, then Pairing Failed with the reason. */
	smp->why = why;
	actions = btd_smp_fail(smp, reason);
	return actions;
}
