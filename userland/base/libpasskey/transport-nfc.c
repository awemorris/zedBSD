/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's NFC transport (nfc.h; ws161-p005): CTAP over ISO 7816-4
 * APDUs.  Every APDU of a command is sent against one deadline for the
 * whole command, its keepalives included, so a key that keeps the user
 * waiting is given up at the caller's time.
 */

#include "nfc.h"

#include "crypto.h"
#include "ctap2.h"

#include <errno.h>
#include <string.h>
#include <time.h>

/* The APDUs' classes and instructions: ISO 7816-4's SELECT and GET RESPONSE, CTAP's NFCCTAP_MSG and NFCCTAP_GETRESPONSE. */
#define NFC_CLA_ISO		0x00U
#define NFC_CLA_CTAP		0x80U
#define NFC_CLA_CHAINING	0x10U
#define NFC_INS_SELECT		0xa4U
#define NFC_INS_GET_RESPONSE	0xc0U
#define NFC_INS_MSG		0x10U
#define NFC_INS_GETRESPONSE	0x11U

/* SELECT by name, the first or only occurrence; NFCCTAP_MSG's P1: the client takes NFCCTAP_GETRESPONSE. */
#define NFC_P1_SELECT_NAME	0x04U
#define NFC_P2_SELECT_FIRST	0x00U
#define NFC_P1_MSG_GETRESPONSE	0x80U

/* The status words: done, more to get (its low byte the count), and the key still working (its data the keepalive's status). */
#define NFC_SW_OK		0x9000U
#define NFC_SW1_MORE		0x61U
#define NFC_SW_KEEPALIVE	0x9100U

/* The data of one short APDU, and the header of a short and of an extended one. */
#define NFC_SHORT_DATA		255U
#define NFC_HEADER		4U

/* No answer's length in an APDU (otherwise the length a short APDU asks for, 0 for all, 256). */
#define NFC_EXPECT_NONE		(-1)
#define NFC_EXPECT_ALL		0

/* The FIDO applet's AID. */
static const uint8_t nfc_fido_aid[] = { 0xa0, 0x00, 0x00, 0x06, 0x47, 0x2f, 0x00, 0x01 };

static int nfc_command(void *context, uint8_t command, const uint8_t *request, size_t size, uint8_t *reply, size_t capacity, size_t *reply_size, unsigned timeout_ms, pk_hid_keepalive_t keepalive, void *keepalive_context);
static int nfc_send(struct pk_nfc *nfc, const uint8_t *message, size_t size, uint8_t *response, size_t *response_size, uint64_t deadline);
static int nfc_exchange(struct pk_nfc *nfc, uint8_t cla, uint8_t ins, uint8_t p1, const uint8_t *data, size_t size, int extended, int expect, uint8_t *response, size_t capacity, size_t *response_size, uint64_t deadline);
static unsigned nfc_status(const uint8_t *response, size_t size);
static uint64_t nfc_now_ms(void);
static unsigned nfc_left(uint64_t deadline);

/*
 * Selects the FIDO applet of the key at the reader, and notes its
 * versions.  Returns 0, ENODEV when the card has no FIDO applet, ETIMEDOUT,
 * or the reader's error.
 */
int
pk_nfc_open(
	struct pk_nfc *nfc,
	const struct pk_nfc_io *io,
	unsigned timeout_ms)
{
	uint8_t response[PK_NFC_APDU_MAX];
	uint64_t deadline;
	size_t response_size;
	size_t data;
	unsigned status;
	int same;
	int error;

	/* The reader. */
	memset(nfc, 0, sizeof(*nfc));
	nfc->io = *io;

	/* SELECT the applet by its AID. */
	deadline = nfc_now_ms() + timeout_ms;
	error = nfc_exchange(nfc, NFC_CLA_ISO, NFC_INS_SELECT, NFC_P1_SELECT_NAME, nfc_fido_aid, sizeof(nfc_fido_aid), 0,
	    NFC_EXPECT_ALL, response, sizeof(response), &response_size, deadline);
	if (error != 0)
		return error;

	/* A card without the applet. */
	status = nfc_status(response, response_size);
	if (status != NFC_SW_OK)
		return ENODEV;

	/* Its version: "U2F_V2" or "FIDO_2_0" (a key answering the first may speak CTAP2 too, GetInfo says). */
	data = response_size - 2U;
	same = 1;
	if (data == 6U)
		same = memcmp(response, "U2F_V2", 6U);
	if (same == 0)
		nfc->versions |= PK_NFC_VERSION_U2F;
	same = 1;
	if (data == 8U)
		same = memcmp(response, "FIDO_2_0", 8U);
	if (same == 0)
		nfc->versions |= PK_NFC_VERSION_FIDO2;

	/* Succeeded: the applet is selected. */
	return 0;
}

/*
 * Sends one CTAP message (the command's byte and its CBOR) and gives the
 * answer (the status byte and the CBOR) in reply.  A keepalive is told to
 * keepalive and asked past, up to the deadline.  Returns 0, ETIMEDOUT,
 * EMSGSIZE, EIO for a status word that is not the protocol's, or the
 * reader's error.
 */
int
pk_nfc_transact(
	struct pk_nfc *nfc,
	const uint8_t *message,
	size_t size,
	uint8_t *reply,
	size_t capacity,
	size_t *reply_size,
	unsigned timeout_ms,
	pk_hid_keepalive_t keepalive,
	void *keepalive_context)
{
	uint8_t response[PK_NFC_APDU_MAX];
	uint64_t deadline;
	size_t response_size;
	size_t collected;
	size_t data;
	unsigned status;
	int error;

	/* The message, in one APDU or chained. */
	deadline = nfc_now_ms() + timeout_ms;
	error = nfc_send(nfc, message, size, response, &response_size, deadline);
	if (error != 0)
		return error;

	/* The answer, part after part. */
	collected = 0U;
	for (;;) {
		status = nfc_status(response, response_size);
		data = response_size - 2U;

		/* The key still working: its keepalive told, and asked again. */
		if (status == NFC_SW_KEEPALIVE) {
			if (keepalive != NULL && data >= 1U)
				keepalive(keepalive_context, response[0]);
			error = nfc_exchange(nfc, NFC_CLA_CTAP, NFC_INS_GETRESPONSE, 0U, NULL, 0U, nfc->io.extended, NFC_EXPECT_ALL,
			    response, sizeof(response), &response_size, deadline);
			if (error != 0)
				return error;
			continue;
		}

		/* A part of the answer: kept. */
		if (status != NFC_SW_OK && (status >> 8) != NFC_SW1_MORE)
			return EIO;
		if (collected + data > capacity)
			return EMSGSIZE;
		memcpy(reply + collected, response, data);
		collected += data;

		/* The last part. */
		if (status == NFC_SW_OK)
			break;

		/* More to get: as many as SW2 says (0 for 256). */
		error = nfc_exchange(nfc, NFC_CLA_ISO, NFC_INS_GET_RESPONSE, 0U, NULL, 0U, 0, (int)(status & 0xffU), response,
		    sizeof(response), &response_size, deadline);
		if (error != 0)
			return error;
	}

	/* Succeeded: the whole answer. */
	*reply_size = collected;
	return 0;
}

/* Makes a selected FIDO applet the transport of CTAP2 commands (no cancel: NFC has none). */
int
pk_nfc_transport(
	struct pk_transport *transport,
	struct pk_nfc *nfc)
{
	/* The applet and its function. */
	transport->context = nfc;
	transport->command = nfc_command;
	transport->cancel = NULL;

	/* Succeeded: the transport. */
	return 0;
}

/* The NFC transport's command: the command's byte, then the request, as one message. */
static int
nfc_command(
	void *context,
	uint8_t command,
	const uint8_t *request,
	size_t size,
	uint8_t *reply,
	size_t capacity,
	size_t *reply_size,
	unsigned timeout_ms,
	pk_hid_keepalive_t keepalive,
	void *keepalive_context)
{
	uint8_t message[PK_HID_MESSAGE_MAX];
	int error;

	/* The command's byte, then the request. */
	if (size + 1U > sizeof(message))
		return EMSGSIZE;
	message[0] = command;
	if (size != 0U)
		memcpy(message + 1U, request, size);

	/* The transaction, the message wiped after (it may hold a PIN's encryption). */
	error = pk_nfc_transact(context, message, size + 1U, reply, capacity, reply_size, timeout_ms, keepalive,
	    keepalive_context);
	pk_crypto_wipe(message, size + 1U);
	if (error != 0)
		return error;

	/* Succeeded: the status and the CBOR. */
	return 0;
}

/*
 * Sends NFCCTAP_MSG with a message: one extended APDU when the reader takes
 * it whole, otherwise short APDUs in command chaining (each block but the
 * last answered "9000").  Gives the last block's response.
 */
static int
nfc_send(
	struct pk_nfc *nfc,
	const uint8_t *message,
	size_t size,
	uint8_t *response,
	size_t *response_size,
	uint64_t deadline)
{
	size_t sent;
	size_t take;
	unsigned status;
	uint8_t cla;
	int expect;
	int error;

	/* One extended APDU: the header, three bytes of length, the data, two of the answer's. */
	if (nfc->io.extended && NFC_HEADER + 3U + size + 2U <= nfc->io.max_command) {
		error = nfc_exchange(nfc, NFC_CLA_CTAP, NFC_INS_MSG, NFC_P1_MSG_GETRESPONSE, message, size, 1, NFC_EXPECT_ALL,
		    response, PK_NFC_APDU_MAX, response_size, deadline);
		return error;
	}

	/* Short APDUs, chained. */
	sent = 0U;
	for (;;) {
		/* The next block, the chaining bit on all but the last. */
		take = size - sent;
		cla = NFC_CLA_CTAP;
		expect = NFC_EXPECT_ALL;
		if (take > NFC_SHORT_DATA) {
			take = NFC_SHORT_DATA;
			cla |= NFC_CLA_CHAINING;
			expect = NFC_EXPECT_NONE;
		}

		/* The block, and its answer. */
		error = nfc_exchange(nfc, cla, NFC_INS_MSG, NFC_P1_MSG_GETRESPONSE, message + sent, take, 0, expect, response,
		    PK_NFC_APDU_MAX, response_size, deadline);
		if (error != 0)
			return error;
		sent += take;

		/* The last block's answer is the message's. */
		if (sent == size)
			break;

		/* A block taken. */
		status = nfc_status(response, *response_size);
		if (status != NFC_SW_OK)
			return EIO;
	}

	/* Succeeded: the answer's first part. */
	return 0;
}

/*
 * Exchanges one APDU: the class, the instruction, P1 (P2 is zero), the
 * data, short or extended, and the answer's length (NFC_EXPECT_NONE for
 * none; an extended APDU asks for all there is).  The response (at most
 * capacity bytes) holds the data and the status word.
 */
static int
nfc_exchange(
	struct pk_nfc *nfc,
	uint8_t cla,
	uint8_t ins,
	uint8_t p1,
	const uint8_t *data,
	size_t size,
	int extended,
	int expect,
	uint8_t *response,
	size_t capacity,
	size_t *response_size,
	uint64_t deadline)
{
	uint8_t apdu[PK_NFC_APDU_MAX];
	size_t length;
	unsigned left;
	int error;

	/* The header. */
	if (size + NFC_HEADER + 5U > sizeof(apdu))
		return EMSGSIZE;
	apdu[0] = cla;
	apdu[1] = ins;
	apdu[2] = p1;
	apdu[3] = 0x00U;
	length = NFC_HEADER;

	/* The data's length and the data: one byte (short), or a zero and two bytes (extended). */
	if (size != 0U && extended) {
		apdu[length++] = 0x00U;
		apdu[length++] = (uint8_t)(size >> 8);
		apdu[length++] = (uint8_t)size;
	} else if (size != 0U) {
		apdu[length++] = (uint8_t)size;
	}

	/* The data itself. */
	if (size != 0U)
		memcpy(apdu + length, data, size);
	length += size;

	/* The answer's length: all there is in an extended APDU (one without data has a zero first), as asked in a short one. */
	if (expect != NFC_EXPECT_NONE && extended) {
		if (size == 0U)
			apdu[length++] = 0x00U;
		apdu[length++] = 0x00U;
		apdu[length++] = 0x00U;
	} else if (expect != NFC_EXPECT_NONE) {
		apdu[length++] = (uint8_t)expect;
	}

	/* The time left, and the exchange. */
	left = nfc_left(deadline);
	if (left == 0U)
		return ETIMEDOUT;
	if (length > nfc->io.max_command)
		return EMSGSIZE;
	error = nfc->io.transmit(nfc->io.context, apdu, length, response, capacity, response_size, left);
	pk_crypto_wipe(apdu, length);
	if (error != 0)
		return error;

	/* A response has its status word at least. */
	if (*response_size < 2U)
		return EIO;

	/* Succeeded: the response. */
	return 0;
}

/* Gives a response's status word (its last two bytes). */
static unsigned
nfc_status(
	const uint8_t *response,
	size_t size)
{
	/* SW1, then SW2. */
	return ((unsigned)response[size - 2U] << 8) | response[size - 1U];
}

/* Gives the monotonic clock in milliseconds. */
static uint64_t
nfc_now_ms(void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Gives the milliseconds left before a deadline (0 once it is past). */
static unsigned
nfc_left(
	uint64_t deadline)
{
	uint64_t now;

	/* Past, or what is left. */
	now = nfc_now_ms();
	if (now >= deadline)
		return 0U;
	return (unsigned)(deadline - now);
}
