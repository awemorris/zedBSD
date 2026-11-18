/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CCID 1.1 messages and the class descriptor (ws161-p003): the pure
 * part of the USB CCID reader driver, with no lock and no transfer, so the
 * host test compiles it as it is (plan/ws161/tests/ccid-proto-host-test.sh).
 * Every number is little-endian (CCID 1.1 section 6).
 */

#include <drivers/usb/usb-ccid.h>

#include <uapi/errno.h>

static uint32_t ccid_le32(const uint8_t *bytes);
static void ccid_put_le32(uint8_t *bytes, uint32_t value);

/*
 * Reads a reader's class descriptor (CCID 1.1 section 5.1).  Returns 0, or
 * EINVAL for one that is too short or of another type.
 */
int
drv_ccid_parse_class(
	const uint8_t *descriptor,
	size_t length,
	struct ccid_class *result)
{
	/* A whole class descriptor. */
	if (descriptor == NULL || result == NULL)
		return EINVAL;
	if (length < CCID_CLASS_DESCRIPTOR_LENGTH || descriptor[0] < CCID_CLASS_DESCRIPTOR_LENGTH)
		return EINVAL;
	if (descriptor[1] != CCID_CLASS_DESCRIPTOR)
		return EINVAL;

	/* bcdCCID, bMaxSlotIndex, dwProtocols, dwFeatures, dwMaxCCIDMessageLength, bMaxCCIDBusySlots. */
	result->version = (uint16_t)(descriptor[2] | (descriptor[3] << 8U));
	result->max_slot_index = descriptor[4];
	result->protocols = ccid_le32(descriptor + 6U);
	result->features = ccid_le32(descriptor + 40U);
	result->max_message = ccid_le32(descriptor + 44U);
	result->max_busy_slots = descriptor[53];

	/* Succeeded: the reader's description. */
	return 0;
}

/* Tells a reader's APDU exchange level from its dwFeatures (CCID_LEVEL_*). */
int
drv_ccid_level(
	uint32_t features)
{
	uint32_t level;

	/* The exchange's bits. */
	level = features & CCID_FEATURE_EXCHANGE_MASK;

	/* Short and extended APDUs, short APDUs, or neither (TPDU, characters). */
	if (level == CCID_FEATURE_EXTENDED_APDU)
		return CCID_LEVEL_EXTENDED;
	if (level == CCID_FEATURE_SHORT_APDU)
		return CCID_LEVEL_SHORT;

	/* Neither: no whole APDUs. */
	return CCID_LEVEL_NONE;
}

/* Writes the 10 bytes of a command's header. */
void
drv_ccid_header(
	uint8_t *message,
	uint8_t type,
	uint32_t length,
	uint8_t slot,
	uint8_t sequence,
	uint8_t parameter0,
	uint8_t parameter1,
	uint8_t parameter2)
{
	/* bMessageType, dwLength, bSlot, bSeq, and the three bytes of the command. */
	message[0] = type;
	ccid_put_le32(message + 1U, length);
	message[5] = slot;
	message[6] = sequence;
	message[7] = parameter0;
	message[8] = parameter1;
	message[9] = parameter2;
}

/*
 * Reads an answer's header.  Returns 0, or EIO for an answer shorter than
 * its header or than the data its length names.
 */
int
drv_ccid_parse_reply(
	const uint8_t *message,
	size_t size,
	struct ccid_reply *reply)
{
	/* A whole header. */
	if (message == NULL || reply == NULL || size < CCID_HEADER)
		return EIO;

	/* The fields. */
	reply->type = message[0];
	reply->length = ccid_le32(message + 1U);
	reply->slot = message[5];
	reply->sequence = message[6];
	reply->status = message[7];
	reply->error = message[8];
	reply->parameter = message[9];
	reply->data = message + CCID_HEADER;

	/* The data its length names is all there. */
	if (reply->length > size - CCID_HEADER)
		return EIO;

	/* Succeeded: the answer is whole. */
	return 0;
}

/* Reads a 32-bit little-endian number. */
static uint32_t
ccid_le32(
	const uint8_t *bytes)
{
	/* The least significant byte first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
	    ((uint32_t)bytes[3] << 24U);
}

/* Writes a 32-bit little-endian number. */
static void
ccid_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The least significant byte first. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8U);
	bytes[2] = (uint8_t)(value >> 16U);
	bytes[3] = (uint8_t)(value >> 24U);
}
