/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test kernel's loopback card (ws161-p003, the user's approval U4): a
 * smart card slot with a card that is always in it and no hardware, so
 * that /dev/smartcardN can be tried in QEMU, whose own CCID reader is not
 * one of whole APDUs.  Test builds only (CONFIG_SECURITY_KEY_TEST_LOOPBACK).
 *
 * The card answers as far as the node's tests need:
 *
 *   00 A4 04 00 08 A0 00 00 06 47 2F 00 01   SELECT of FIDO's applet: "FIDO_2_0" 90 00
 *   00 A4 04 00 ...                          SELECT of anything else: 6A 82
 *   80 CB 00 00 02 HH LL                     HHLL bytes (byte i is i & 0xFF); more than
 *                                            256 come as 256 and 61 xx, the rest by
 *   00 C0 00 00 Le                           GET RESPONSE
 *   80 FE 00 00                              the card is taken out and put back (two
 *                                            events), 90 00 (the next APDU: ENXIO)
 *   anything else                            6D 00
 *
 * It holds no key and signs nothing.
 */

#include <drivers/generic/smartcard.h>

#include "kern/klog.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>

/* The longest data an answer of a short APDU carries, and the longest the card makes. */
#define LOOPBACK_SHORT_DATA	256U
#define LOOPBACK_DATA_MAX	4096U

/* FIDO's applet (CTAP over NFC). */
static const uint8_t loopback_fido_aid[8] = { 0xa0, 0x00, 0x00, 0x06, 0x47, 0x2f, 0x00, 0x01 };

/* The ATR of a contactless card as a PC/SC reader shows it (PC/SC part 3). */
static const uint8_t loopback_atr[] = { 0x3b, 0x80, 0x80, 0x01, 0x01 };

/*
 * The card's state, one for the kernel's life, guarded by the slot's
 * operations' mutex (the class runs one operation at a time): the slot,
 * whether the card is powered, and the answer still to give by GET
 * RESPONSE (pending bytes from offset).
 */
struct loopback_card {
	struct drv_smartcard *slot;
	unsigned powered;
	size_t pending;
	size_t offset;
	uint8_t data[LOOPBACK_DATA_MAX];
};

/* The one loopback card, zero until it is registered. */
static struct loopback_card loopback_card;

static int loopback_power_on(void *context, unsigned slot, uint8_t *atr, size_t *atr_size, uint64_t deadline_ms);
static int loopback_power_off(void *context, unsigned slot);
static int loopback_transmit(void *context, unsigned slot, const uint8_t *command, size_t command_size, uint8_t *response, size_t capacity, size_t *response_size, uint64_t deadline_ms);
static int loopback_give(struct loopback_card *card, uint8_t *response, size_t capacity, size_t *response_size);
static int loopback_status(uint8_t *response, size_t capacity, size_t *response_size, uint8_t sw1, uint8_t sw2);

/* What the loopback card's slot does for the smart card class. */
static const struct drv_smartcard_ops loopback_card_ops = {
	.power_on = loopback_power_on,
	.power_off = loopback_power_off,
	.transmit = loopback_transmit
};

/*
 * Publishes the loopback card's slot (test builds: called once at boot).
 * Returns 0 or the error of publishing it.
 */
int
drv_smartcard_loopback_register(
	void)
{
	struct drv_smartcard_description description;
	int error;

	/* What the slot is: one slot of short APDUs, with a card. */
	kern_memset(&description, 0, sizeof(description));
	description.info.vendor = 0x1209U;
	description.info.product = 0xcc1dU;
	description.info.version = 0x0100U;
	description.info.slot = 0U;
	description.info.slot_count = 1U;
	description.info.features = 0x00020000U;
	description.info.protocols = 0x00000002U;
	description.info.max_message = 271U;
	description.info.max_command = 261U;
	description.info.max_response = 258U;
	kern_snprintf(description.info.name, sizeof(description.info.name), "Loopback smart card (test)");
	description.present = 1;

	/* The node. */
	error = drv_smartcard_register(&description, &loopback_card_ops, &loopback_card, &loopback_card.slot);
	if (error != 0)
		return error;

	/* Succeeded: the test card is there. */
	kern_logf("smartcard-loopback: test card published\n");
	return 0;
}

/* Powers the card: its ATR. */
static int
loopback_power_on(
	void *context,
	unsigned slot,
	uint8_t *atr,
	size_t *atr_size,
	uint64_t deadline_ms)
{
	struct loopback_card *card;

	/* The card answers at once, with nothing pending. */
	card = context;
	(void)slot;
	(void)deadline_ms;
	card->powered = 1U;
	card->pending = 0U;
	kern_memcpy(atr, loopback_atr, sizeof(loopback_atr));
	*atr_size = sizeof(loopback_atr);

	/* Succeeded: the card is powered. */
	return 0;
}

/* Powers the card off: nothing pending stays. */
static int
loopback_power_off(
	void *context,
	unsigned slot)
{
	struct loopback_card *card;

	/* Off. */
	card = context;
	(void)slot;
	card->powered = 0U;
	card->pending = 0U;

	/* Succeeded: the card is off. */
	return 0;
}

/* Answers one command APDU (the commands of the file's comment). */
static int
loopback_transmit(
	void *context,
	unsigned slot,
	const uint8_t *command,
	size_t command_size,
	uint8_t *response,
	size_t capacity,
	size_t *response_size,
	uint64_t deadline_ms)
{
	struct loopback_card *card;
	size_t length;
	size_t index;
	int same;
	int error;

	/* A powered card and a whole header. */
	card = context;
	(void)slot;
	(void)deadline_ms;
	if (!card->powered)
		return ENXIO;
	if (command_size < 4U)
		return EINVAL;

	/* SELECT by name: FIDO's applet, or none. */
	if (command[0] == 0x00U && command[1] == 0xa4U && command[2] == 0x04U) {
		same = 0;
		if (command_size >= 5U + sizeof(loopback_fido_aid) && command[4] == sizeof(loopback_fido_aid))
			same = kern_memcmp(command + 5U, loopback_fido_aid, sizeof(loopback_fido_aid)) == 0;
		if (!same) {
			error = loopback_status(response, capacity, response_size, 0x6aU, 0x82U);
			return error;
		}

		/* The applet's version, as the answer. */
		kern_memcpy(card->data, "FIDO_2_0", 8U);
		card->pending = 8U;
		card->offset = 0U;
		error = loopback_give(card, response, capacity, response_size);
		return error;
	}

	/* The test's data of a length: byte i is i & 0xFF. */
	if (command[0] == 0x80U && command[1] == 0xcbU && command_size >= 7U && command[4] == 2U) {
		length = ((size_t)command[5] << 8U) | command[6];
		if (length > LOOPBACK_DATA_MAX)
			length = LOOPBACK_DATA_MAX;
		for (index = 0U; index < length; index++)
			card->data[index] = (uint8_t)index;
		card->pending = length;
		card->offset = 0U;
		error = loopback_give(card, response, capacity, response_size);
		return error;
	}

	/* GET RESPONSE: the next part of the pending answer. */
	if (command[0] == 0x00U && command[1] == 0xc0U) {
		error = loopback_give(card, response, capacity, response_size);
		return error;
	}

	/* The card is taken out and put back: two events, and the card is no longer powered. */
	if (command[0] == 0x80U && command[1] == 0xfeU) {
		card->powered = 0U;
		card->pending = 0U;
		drv_smartcard_card(card->slot, 0);
		drv_smartcard_card(card->slot, 1);
		error = loopback_status(response, capacity, response_size, 0x90U, 0x00U);
		return error;
	}

	/* Anything else is an instruction it does not have. */
	error = loopback_status(response, capacity, response_size, 0x6dU, 0x00U);
	return error;
}

/* Gives the next part of the pending answer: up to 256 bytes, then 90 00 or 61 xx. */
static int
loopback_give(
	struct loopback_card *card,
	uint8_t *response,
	size_t capacity,
	size_t *response_size)
{
	size_t part;
	size_t left;

	/* Nothing pending is a GET RESPONSE out of order. */
	if (card->pending == 0U)
		return loopback_status(response, capacity, response_size, 0x69U, 0x85U);

	/* The part, and its status word. */
	part = card->pending;
	if (part > LOOPBACK_SHORT_DATA)
		part = LOOPBACK_SHORT_DATA;
	if (part + 2U > capacity)
		return EMSGSIZE;
	kern_memcpy(response, card->data + card->offset, part);
	card->offset += part;
	card->pending -= part;
	left = card->pending;
	response[part] = 0x90U;
	response[part + 1U] = 0x00U;
	if (left != 0U) {
		/* 61 xx: xx more bytes, 00 for 256 or more. */
		response[part] = 0x61U;
		response[part + 1U] = (uint8_t)left;
		if (left > 0xffU)
			response[part + 1U] = 0x00U;
	}

	/* Succeeded: the part and its status word. */
	*response_size = part + 2U;
	return 0;
}

/* Gives a status word alone. */
static int
loopback_status(
	uint8_t *response,
	size_t capacity,
	size_t *response_size,
	uint8_t sw1,
	uint8_t sw2)
{
	/* Two bytes. */
	if (capacity < 2U)
		return EMSGSIZE;
	response[0] = sw1;
	response[1] = sw2;

	/* Succeeded: the status word. */
	*response_size = 2U;
	return 0;
}
