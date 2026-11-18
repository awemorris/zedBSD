/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's Security Manager as the initiator (ws143-p004, Core 5.4 Vol
 * 3 Part H): LE Secure Connections (Just Works, Numeric Comparison, and
 * Passkey Entry with bluetoothd showing the passkey) and LE legacy pairing
 * (Just Works and the shown passkey; D10 takes it with a warning), the
 * responder's keys taken (LTK, EDIV and Rand for legacy, IRK and identity
 * address).  Without system calls: the caller (pair.c) carries out what
 * btd_smp_* ask for (send a PDU, the controller's P-256 commands, the
 * agent, the encryption) and feeds back what comes.  The host tests run it
 * against a scripted responder.
 */

#ifndef BLUETOOTHD_SMP_H
#define BLUETOOTHD_SMP_H

#include <stddef.h>
#include <stdint.h>

/* What a call asks of the caller (bits). */
#define BTD_SMP_SEND		0x0001U
#define BTD_SMP_READ_KEY	0x0002U
#define BTD_SMP_DHKEY		0x0004U
#define BTD_SMP_CONFIRM		0x0008U
#define BTD_SMP_SHOW		0x0010U
#define BTD_SMP_ENCRYPT		0x0020U
#define BTD_SMP_DONE		0x0040U
#define BTD_SMP_FAILED		0x0080U

/* The IO capabilities bluetoothd takes as the initiator: with an agent, without one. */
#define BTD_SMP_DISPLAY_YES_NO	0x01U
#define BTD_SMP_NO_IO		0x03U

/* The reasons of Pairing Failed bluetoothd sends or reports. */
#define BTD_SMP_PASSKEY_FAILED		0x01U
#define BTD_SMP_CONFIRM_FAILED		0x04U
#define BTD_SMP_NOT_SUPPORTED		0x05U
#define BTD_SMP_KEY_SIZE		0x06U
#define BTD_SMP_UNSPECIFIED		0x08U
#define BTD_SMP_INVALID		0x0aU
#define BTD_SMP_DHKEY_FAILED		0x0bU
#define BTD_SMP_NUMERIC_FAILED		0x0cU

/*
 * Why a pairing failed, for the daemon's answer (the reason sent to the
 * responder is coarser): the protocol broken, a short key, the Core's
 * debug key, the responder reflecting bluetoothd's own key, the
 * controller's DHKey refused, the responder's confirm or check wrong, the
 * responder or the agent saying no, the encryption failing, the
 * controller's public key failing, or the caller's (a timeout).
 */
#define BTD_SMP_WHY_NONE		0U
#define BTD_SMP_WHY_PROTOCOL		1U
#define BTD_SMP_WHY_KEY_SIZE		2U
#define BTD_SMP_WHY_DEBUG_KEY		3U
#define BTD_SMP_WHY_REFLECTION		4U
#define BTD_SMP_WHY_DHKEY		5U
#define BTD_SMP_WHY_CHECK		6U
#define BTD_SMP_WHY_REJECTED		7U
#define BTD_SMP_WHY_ENCRYPTION		8U
#define BTD_SMP_WHY_CONTROLLER		9U
#define BTD_SMP_WHY_CALLER		10U

/* The longest PDU bluetoothd sends (a public key: code, X, Y). */
#define BTD_SMP_PDU_MAX		65U

/* A function that fills bytes with random ones (the daemon's arc4random, the tests' fixed ones). */
typedef void (*btd_random_fn)(void *context, uint8_t *bytes, size_t length);

/*
 * The keys a pairing gave: the LTK (with EDIV and Rand, 0 for Secure
 * Connections) and its size, whether it was authenticated (MITM) and
 * whether it was Secure Connections or legacy, and the responder's
 * identity (IRK and address) when it gave one.  Values as the PDUs carry
 * them (least significant byte first).
 */
struct btd_smp_keys {
	int have_ltk;
	uint8_t ltk[16];
	uint16_t ediv;
	uint8_t rand[8];
	uint8_t key_size;
	int authenticated;
	int secure;
	int have_irk;
	uint8_t irk[16];
	int have_identity;
	uint8_t identity_type;
	uint8_t identity[6];
};

/*
 * One pairing as the initiator.  The caller owns it for the pairing's
 * life; values are as the PDUs and HCI carry them unless named "msb".
 */
struct btd_smp {
	/* Where the pairing is, and how it is done. */
	unsigned state;
	unsigned method;
	int secure;
	unsigned round;
	unsigned keys_awaited;

	/* The two sides: IO capability ours, addresses (type, then six bytes least significant first). */
	uint8_t io_capability;
	uint8_t initiator_type;
	uint8_t initiator[6];
	uint8_t responder_type;
	uint8_t responder[6];

	/* The Pairing Request and Response as sent (opcode first). */
	uint8_t request[7];
	uint8_t response[7];

	/* Secure Connections: the public keys (X then Y), the DHKey, and whether each is known. */
	int have_local_key;
	int have_remote_key;
	int have_dhkey;
	uint8_t local_key[64];
	uint8_t remote_key[64];
	uint8_t dhkey[32];

	/* The nonces, the peer's confirm, the passkey (or 0), the agent's answer, and the keys of the end. */
	uint8_t local_nonce[16];
	uint8_t remote_nonce[16];
	uint8_t remote_confirm[16];
	uint32_t passkey;
	int agent_answered;
	int agent_accepted;
	int sent_check;
	uint8_t mac_key[16];
	uint8_t tk[16];
	struct btd_smp_keys keys;

	/* What the caller is to do: the PDU to send, the number to show or confirm, the key to encrypt with, the failure. */
	uint8_t out[BTD_SMP_PDU_MAX];
	size_t out_length;
	uint32_t number;
	uint8_t encrypt_key[16];
	uint16_t encrypt_ediv;
	uint8_t encrypt_rand[8];
	uint8_t failure;
	int failure_sent;
	unsigned why;

	/* The random source. */
	btd_random_fn random;
	void *random_context;
};

void btd_smp_init(struct btd_smp *smp, uint8_t io_capability, uint8_t initiator_type, const uint8_t *initiator, uint8_t responder_type, const uint8_t *responder, btd_random_fn random, void *random_context);
unsigned btd_smp_start(struct btd_smp *smp);
unsigned btd_smp_input(struct btd_smp *smp, const uint8_t *pdu, size_t length);
unsigned btd_smp_local_key(struct btd_smp *smp, uint8_t status, const uint8_t *key);
unsigned btd_smp_dhkey(struct btd_smp *smp, uint8_t status, const uint8_t *dhkey);
unsigned btd_smp_agent(struct btd_smp *smp, int accepted);
unsigned btd_smp_encrypted(struct btd_smp *smp, uint8_t status, int on);
unsigned btd_smp_fail(struct btd_smp *smp, uint8_t reason);

#endif
