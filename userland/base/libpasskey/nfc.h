/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's NFC transport (transport-nfc.c; ws161-p005, plan/ws161/phase001
 * section 9.4): CTAP over ISO 7816-4 APDUs, as a security key held to an
 * NFC reader answers them (the reader frames ISO 14443-4 itself; on zedBSD
 * the APDUs go through the reader's slot node /dev/smartcardN).
 *
 * The FIDO applet is selected first (its answer "U2F_V2" or "FIDO_2_0";
 * whether the key speaks CTAP2 is GetInfo's to say).  A CTAP2 command is
 * NFCCTAP_MSG (CLA 0x80, INS 0x10, P1 0x80: the client takes
 * NFCCTAP_GETRESPONSE) whose data is the command's byte and its CBOR.  A
 * reader of short APDUs gets it in ISO 7816-4 command chaining (CLA 0x90
 * for every block but the last); a reader of extended APDUs in one.  An
 * answer longer than one response comes in parts, each "61xx" asking for
 * GET RESPONSE; while the key waits for the user it answers "9100" with
 * its keepalive status, and NFCCTAP_GETRESPONSE asks again.
 */

#ifndef LIBPASSKEY_NFC_H
#define LIBPASSKEY_NFC_H

#include <stddef.h>
#include <stdint.h>

#include "hid.h"

/* The longest command and response APDUs of a short-APDU reader. */
#define PK_NFC_SHORT_COMMAND	261U
#define PK_NFC_SHORT_RESPONSE	258U

/* The largest APDU either way the transport makes or takes (an extended one, with its header and status). */
#define PK_NFC_APDU_MAX		(PK_HID_MESSAGE_MAX + 16U)

/* The FIDO applet's versions, as bits of pk_nfc's versions. */
#define PK_NFC_VERSION_U2F	0x0001U
#define PK_NFC_VERSION_FIDO2	0x0002U

/*
 * The reader's side: one APDU exchanged (the command's bytes, the answer
 * with its two status bytes in response, at most capacity), within
 * timeout_ms; the longest command APDU the reader takes; and whether it
 * takes extended APDUs.  transmit returns 0 or an errno value.
 */
struct pk_nfc_io {
	void *context;
	int (*transmit)(void *context, const uint8_t *command, size_t size, uint8_t *response, size_t capacity,
	    size_t *response_size, unsigned timeout_ms);
	size_t max_command;
	int extended;
};

struct pk_transport;

/* A selected FIDO applet: its reader, and the versions its SELECT answered. */
struct pk_nfc {
	struct pk_nfc_io io;
	unsigned versions;
};

int pk_nfc_open(struct pk_nfc *nfc, const struct pk_nfc_io *io, unsigned timeout_ms);
int pk_nfc_transact(struct pk_nfc *nfc, const uint8_t *message, size_t size, uint8_t *reply, size_t capacity,
    size_t *reply_size, unsigned timeout_ms, pk_hid_keepalive_t keepalive, void *keepalive_context);
int pk_nfc_transport(struct pk_transport *transport, struct pk_nfc *nfc);

#endif
