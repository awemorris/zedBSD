/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 transmit implementation.
 */

#include "intel-ax211-tx.h"
#include <kern/kcrt.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The fields of the transmit command (version 10) that precede the frame.
 *
 * The 28-byte fixed part holds the frame length at byte 0 (without the CCMP
 * header, which the firmware inserts itself), the flags at 2, the
 * offload-assist word at 4 and the rate at 16; the MAC header follows from
 * byte 28, padded to four bytes.
 * - Flags: COMMAND_RATE (bit 0) makes the firmware use the rate in the
 *   command; ENCRYPT_DISABLE (bit 1) sends the frame unencrypted;
 *   HIGH_PRIORITY (bit 2) queues it ahead of normal traffic.  The driver
 *   sets bits 0 and 2 on every frame, because the API 89 firmware has no
 *   rate state until rate control is configured, and bit 1 on every frame
 *   it does not encrypt.
 * - Offload assist: bits 12:8 give the MAC header length in 16-bit words,
 *   and PADDING (bit 13) says two bytes of padding follow the header.
 * - Rate: 0 is 1 Mbit/s CCK and 0x0100 marks the legacy OFDM rate table,
 *   whose code 0 is 6 Mbit/s; ANTENNA_A (bit 14) transmits on the first
 *   antenna.  The driver sends at 1 Mbit/s on 2.4 GHz and at 6 Mbit/s on
 *   5 GHz, where CCK is not allowed.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/tx.h (struct iwl_tx_cmd_gen3, enum
 * iwl_tx_cmd_flags and the offload-assist bits) and
 * drivers/net/wireless/intel/iwlwifi/fw/api/rs.h (the rate word).
 */
#define AX211_TX_FLAG_COMMAND_RATE 0x0001U
#define AX211_TX_FLAG_ENCRYPT_DISABLE 0x0002U
#define AX211_TX_FLAG_HIGH_PRIORITY 0x0004U

/* The offload-assist word: the MAC header length in words and the padding. */
#define AX211_TX_OFFLOAD_MAC_HEADER_SHIFT 8U
#define AX211_TX_OFFLOAD_MAC_HEADER_MASK 0x1fU
#define AX211_TX_OFFLOAD_PADDING 0x2000U

/* The rate word: the two basic rates and the first antenna. */
#define AX211_TX_RATE_1M_CCK 0U
#define AX211_TX_RATE_6M_OFDM 0x0100U
#define AX211_TX_RATE_ANTENNA_A 0x4000U
/*
 * The 802.11 frame control field and the CCMP header.
 *
 * The driver uses them to find the header length of a frame to send and to
 * check that a frame it is asked to encrypt carries a well-formed CCMP header.
 *
 * Values referenced: IEEE Std 802.11-2020 9.2.4.1 (frame control) and 12.5.3.2
 * (the 8-byte CCMP header with the Ext IV bit, bit 5 of its fourth byte).
 */
#define AX211_TX_FRAME_TYPE_MASK 0x000cU
#define AX211_TX_FRAME_TYPE_MANAGEMENT 0x0000U
#define AX211_TX_FRAME_TYPE_CONTROL 0x0004U
#define AX211_TX_FRAME_TYPE_DATA 0x0008U
#define AX211_TX_FRAME_SUBTYPE_MASK 0x00f0U
#define AX211_TX_FRAME_SUBTYPE_QOS 0x0080U
#define AX211_TX_TO_DS 0x0100U
#define AX211_TX_FROM_DS 0x0200U
#define AX211_TX_PROTECTED 0x4000U
#define AX211_TX_ORDER 0x8000U

/* The CCMP header length and its Ext IV bit. */
#define AX211_TX_CCMP_HEADER_SIZE 8U
#define AX211_TX_CCMP_EXTENDED_IV 0x20U
/*
 * The transmit completion notification (version 7, 48 bytes for one frame).
 *
 * Byte 0 is the number of frames reported and must be 1; bytes 2 and 3 are the
 * RTS and frame retry counts; byte 28 is the sequence control and byte 30 the
 * byte count; byte 36 the queue; byte 40 the status word, whose low byte is
 * the outcome; byte 44 the scheduler sequence number, which must be below
 * 65536.  Outcome 1 means the frame was delivered and acknowledged, 2 that it
 * was sent without waiting for an acknowledgment (as for a group-addressed
 * frame); the driver counts both as sent, and every other outcome as a
 * failure.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/tx.h (struct iwl_tx_resp and enum
 * iwl_tx_status).
 */
#define AX211_TX_RESPONSE_SIZE 48U
#define AX211_TX_RESPONSE_STATUS_OFFSET 40U
#define AX211_TX_RESPONSE_SSN_OFFSET 44U
#define AX211_TX_OUTCOME_MASK 0x000000ffU
#define AX211_TX_OUTCOME_DELIVERED 1U
#define AX211_TX_OUTCOME_SENT_WITHOUT_ACK 2U

static int ax211_tx_header_length(const uint8_t *frame, size_t length, size_t *header_length);
static uint16_t ax211_tx_get_le16(const uint8_t *bytes);
static int ax211_tx_request_validate(const struct intel_ax211_tx_request *request, size_t header_length);
static int ax211_tx_ccmp_validate(const struct intel_ax211_tx_request *request, size_t header_length);
static void ax211_tx_put_le16(uint8_t *bytes, uint16_t value);
static void ax211_tx_put_le32(uint8_t *bytes, uint32_t value);
static uint32_t ax211_tx_get_le32(const uint8_t *bytes);

/*
 * Checks that the pinned image lists the transmit command and its completion
 * in the layouts used here.
 */
int
drv_intel_ax211_tx_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Refuses a missing command table. */
	if (table == NULL)
		return INTEL_AX211_TX_INVALID;

	/* Looks up the versions the image lists for the transmit command. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, INTEL_AX211_TX_FAMILY, INTEL_AX211_TX_OPCODE, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_TX_UNSUPPORTED;

	/* Refuses an image whose command or completion layout differs. */
	if (version.command_version != INTEL_AX211_TX_COMMAND_VERSION ||
	    version.notification_version != INTEL_AX211_TX_NOTIFICATION_VERSION) {
		return INTEL_AX211_TX_UNSUPPORTED;
	}

	/* Succeeded: the image speaks the transmit command encoded here. */
	return INTEL_AX211_TX_OK;
}

/*
 * Builds the transmit command that carries one frame.
 *
 * The command holds the fixed part and the MAC header; the rest of the frame
 * is sent from its own buffer, which payload_offset and payload_length name.
 */
int
drv_intel_ax211_tx_prepare(
	const struct intel_ax211_tx_request *request,
	struct intel_ax211_tx_prepared *prepared)
{
	struct intel_ax211_tx_prepared encoded;
	size_t header_length;
	size_t firmware_length;
	size_t padding;
	uint16_t flags;
	uint32_t offload;
	uint32_t rate;
	int result;

	/* Refuses missing arguments. */
	if (request == NULL || prepared == NULL || request->frame == NULL)
		return INTEL_AX211_TX_INVALID;

	/* Finds the MAC header length of the frame. */
	result = ax211_tx_header_length(request->frame, request->length,
					&header_length);
	if (result != INTEL_AX211_TX_OK)
		return result;

	/* Refuses a request that is inconsistent with its frame. */
	result = ax211_tx_request_validate(request, header_length);
	if (result != INTEL_AX211_TX_OK)
		return result;

	/*
	 * The firmware inserts the CCMP header itself, so its length is left
	 * out.
	 */
	firmware_length = request->length;
	if (request->encrypted)
		firmware_length -= AX211_TX_CCMP_HEADER_SIZE;

	/* Refuses a length the 16-bit field cannot hold. */
	if (firmware_length > UINT16_MAX)
		return INTEL_AX211_TX_OVERSIZED;

	/*
	 * Sends at 6 Mbit/s OFDM on 5 GHz, where CCK is not allowed, and at 1
	 * Mbit/s CCK otherwise.
	 */
	kern_memset(&encoded, 0, sizeof(encoded));
	flags = 0U;
	if (request->band_5ghz) {
		rate = AX211_TX_RATE_ANTENNA_A | AX211_TX_RATE_6M_OFDM;
	} else {
		rate = AX211_TX_RATE_ANTENNA_A | AX211_TX_RATE_1M_CCK;
	}

	/*
	 * Asks the firmware not to encrypt a frame that is sent in the clear.
	 */
	if (!request->encrypted)
		flags |= AX211_TX_FLAG_ENCRYPT_DISABLE;

	/*
	 * API 89 has no usable data-rate state until rate control is configured.
	 * Every frame therefore carries the conservative basic rate chosen above,
	 * and is queued ahead of normal traffic.
	 */
	flags |= AX211_TX_FLAG_COMMAND_RATE | AX211_TX_FLAG_HIGH_PRIORITY;

	/* Pads the MAC header to a multiple of four bytes. */
	padding = header_length & 3U;
	if (padding != 0U)
		padding = 4U - padding;

	/* Writes the header length in words, and marks the padding if any. */
	offload = (uint32_t)((header_length / 2U) &
			     AX211_TX_OFFLOAD_MAC_HEADER_MASK)
		  << AX211_TX_OFFLOAD_MAC_HEADER_SHIFT;
	if (padding != 0U)
		offload |= AX211_TX_OFFLOAD_PADDING;

	/* Refuses a header that does not fit the command buffer. */
	if (INTEL_AX211_TX_COMMAND_FIXED_SIZE + header_length + padding >
	    sizeof(encoded.command)) {
		return INTEL_AX211_TX_BUFFER_TOO_SMALL;
	}

	/* Builds the command that precedes the frame on the wire. */
	ax211_tx_put_le16(encoded.command, (uint16_t)firmware_length);
	ax211_tx_put_le16(encoded.command + 2U, flags);
	ax211_tx_put_le32(encoded.command + 4U, offload);
	ax211_tx_put_le32(encoded.command + 16U, rate);
	kern_memcpy(encoded.command + INTEL_AX211_TX_COMMAND_FIXED_SIZE,
	       request->frame, header_length);
	encoded.command_length =
		INTEL_AX211_TX_COMMAND_FIXED_SIZE + header_length + padding;

	/*
	 * The payload starts after the MAC header, and after the CCMP header
	 * too.
	 */
	encoded.payload_offset = header_length;
	if (request->encrypted)
		encoded.payload_offset += AX211_TX_CCMP_HEADER_SIZE;

	/* Records what the completion and the key checks need. */
	encoded.payload_length = request->length - encoded.payload_offset;
	encoded.connection_generation = request->connection_generation;
	encoded.cookie = request->cookie;
	encoded.key_generation = request->key_generation;
	encoded.packet_number = request->packet_number;
	encoded.frame_length = (uint16_t)firmware_length;
	encoded.encrypted = request->encrypted;
	encoded.key_index = request->key_index;

	/* Succeeded: publishes the prepared command. */
	*prepared = encoded;
	return INTEL_AX211_TX_OK;
}

/*
 * Decodes the completion of one frame sent on a data queue.
 */
int
drv_intel_ax211_tx_completion_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t hardware_generation,
	uint16_t expected_queue,
	uint8_t expected_index,
	struct intel_ax211_tx_completion *completion)
{
	struct intel_ax211_tx_completion decoded;
	uint32_t scheduler_sequence;
	uint32_t status;
	uint16_t response_queue;

	/* Refuses missing arguments, unset epochs and a queue out of range. */
	if (message == NULL ||
	    completion == NULL ||
	    hardware_generation == 0U ||
	    message->generation == 0U ||
	    expected_queue < INTEL_AX211_TX_QUEUE_MIN ||
	    expected_queue > INTEL_AX211_TX_QUEUE_MAX) {
		return INTEL_AX211_TX_INVALID;
	}

	/* Refuses a message of an earlier firmware epoch. */
	if (message->generation != hardware_generation)
		return INTEL_AX211_TX_STALE;

	/* Refuses a message other than the transmit completion. */
	if (message->group != INTEL_AX211_TX_FAMILY ||
	    message->opcode != INTEL_AX211_TX_OPCODE ||
	    message->version != INTEL_AX211_TX_NOTIFICATION_VERSION) {
		return INTEL_AX211_TX_UNSUPPORTED;
	}

	/* Reports a completion the firmware marked as failed. */
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_TX_FAILED;

	/* Refuses a completion of another ring slot. */
	if (message->queue != (uint8_t)(expected_queue & 0x1fU) ||
	    message->index != expected_index) {
		return INTEL_AX211_TX_STALE;
	}

	/* Refuses a missing payload. */
	if (message->payload == NULL)
		return INTEL_AX211_TX_INVALID;

	/* Refuses a payload shorter than one completion. */
	if (message->payload_length < AX211_TX_RESPONSE_SIZE)
		return INTEL_AX211_TX_TRUNCATED;

	/* Refuses a payload longer than one completion. */
	if (message->payload_length > AX211_TX_RESPONSE_SIZE)
		return INTEL_AX211_TX_OVERSIZED;

	/* Refuses a completion that reports more than one frame. */
	if (message->payload[0U] != 1U)
		return INTEL_AX211_TX_UNSUPPORTED;

	/* Refuses a completion of another queue. */
	response_queue = ax211_tx_get_le16(message->payload + 36U);
	if (response_queue != expected_queue)
		return INTEL_AX211_TX_STALE;

	/* Refuses a scheduler sequence number out of range. */
	scheduler_sequence = ax211_tx_get_le32(message->payload +
					       AX211_TX_RESPONSE_SSN_OFFSET);
	if (scheduler_sequence >= INTEL_AX211_TX_SCHEDULER_SEQUENCE_LIMIT)
		return INTEL_AX211_TX_FAILED;

	/* Takes the outcome of the frame. */
	status = ax211_tx_get_le32(message->payload +
				   AX211_TX_RESPONSE_STATUS_OFFSET) &
		 AX211_TX_OUTCOME_MASK;

	/* Renders the notification as one completion record. */
	kern_memset(&decoded, 0, sizeof(decoded));
	decoded.hardware_generation = hardware_generation;
	decoded.scheduler_sequence = scheduler_sequence;
	decoded.queue = response_queue;
	decoded.sequence_control = ax211_tx_get_le16(message->payload + 28U);
	decoded.byte_count = ax211_tx_get_le16(message->payload + 30U);
	decoded.index = expected_index;
	decoded.rts_retries = message->payload[2U];
	decoded.frame_retries = message->payload[3U];

	/*
	 * Counts a delivered frame and one sent without acknowledgment as sent.
	 */
	decoded.acknowledged = 0;
	if (status == AX211_TX_OUTCOME_DELIVERED)
		decoded.acknowledged = 1;
	else if (status == AX211_TX_OUTCOME_SENT_WITHOUT_ACK)
		decoded.acknowledged = 1;

	/* Succeeded: publishes the completion. */
	*completion = decoded;
	return INTEL_AX211_TX_OK;
}

/* Finds the length of the MAC header of a frame to send. */
static int
ax211_tx_header_length(
	const uint8_t *frame,
	size_t length,
	size_t *header_length)
{
	uint16_t frame_control;
	uint16_t subtype;
	uint16_t type;
	size_t required;

	/* Refuses missing arguments and a frame without frame control. */
	if (frame == NULL || header_length == NULL || length < 2U)
		return INTEL_AX211_TX_INVALID;

	/* Takes the type and the subtype of the frame. */
	frame_control = ax211_tx_get_le16(frame);
	type = frame_control & AX211_TX_FRAME_TYPE_MASK;
	subtype = frame_control & AX211_TX_FRAME_SUBTYPE_MASK;

	/*
	 * Measures the header of a management or data frame; others are
	 * refused.
	 */
	if (type == AX211_TX_FRAME_TYPE_MANAGEMENT) {
		/* With +HTC/Order set the header carries HT control. */
		required = 24U;
		if ((frame_control & AX211_TX_ORDER) != 0U)
			required += 4U;
	} else if (type == AX211_TX_FRAME_TYPE_DATA) {
		/*
		 * A frame between two distribution systems has a fourth
		 * address.
		 */
		required = 24U;
		if ((frame_control & (AX211_TX_TO_DS | AX211_TX_FROM_DS)) ==
		    (AX211_TX_TO_DS | AX211_TX_FROM_DS))
			required += 6U;

		/* A QoS data frame carries the QoS control field. */
		if ((subtype & AX211_TX_FRAME_SUBTYPE_QOS) != 0U) {
			required += 2U;

			/* With +HTC/Order set it also carries HT control. */
			if ((frame_control & AX211_TX_ORDER) != 0U)
				required += 4U;
		}
	} else if (type == AX211_TX_FRAME_TYPE_CONTROL) {
		return INTEL_AX211_TX_UNSUPPORTED;
	} else {
		return INTEL_AX211_TX_INVALID;
	}

	/* Refuses a header longer than the command can carry. */
	if (required > INTEL_AX211_TX_MAC_HEADER_MAX)
		return INTEL_AX211_TX_OVERSIZED;

	/* Refuses a frame shorter than its header. */
	if (length < required)
		return INTEL_AX211_TX_TRUNCATED;

	/* Succeeded: reports the header length. */
	*header_length = required;
	return INTEL_AX211_TX_OK;
}

/* Reads a 16-bit little-endian value. */
static uint16_t
ax211_tx_get_le16(
	const uint8_t *bytes)
{
	/* Assembles the value from its two bytes, least significant first. */
	return (uint16_t)bytes[0U] | ((uint16_t)bytes[1U] << 8);
}

/* Checks that a transmit request is consistent with its frame. */
static int
ax211_tx_request_validate(
	const struct intel_ax211_tx_request *request,
	size_t header_length)
{
	uint16_t frame_control;
	uint16_t expected_type;
	int result;

	/*
	 * Refuses unset identifiers, impossible lengths and out-of-range
	 * fields.
	 */
	if (request->connection_generation == 0U ||
	    request->cookie == 0U ||
	    request->length < header_length ||
	    request->length > INTEL_AX211_TX_FRAME_MAX ||
	    (request->encrypted != 0U && request->encrypted != 1U) ||
	    (request->band_5ghz != 0U && request->band_5ghz != 1U) ||
	    request->key_index > 3U ||
	    (request->frame_class != INTEL_AX211_TX_FRAME_MANAGEMENT &&
	     request->frame_class != INTEL_AX211_TX_FRAME_EAPOL &&
	     request->frame_class != INTEL_AX211_TX_FRAME_DATA)) {
		return INTEL_AX211_TX_INVALID;
	}

	/*
	 * A management request carries a management frame, the others a data
	 * frame.
	 */
	frame_control = ax211_tx_get_le16(request->frame);
	if (request->frame_class == INTEL_AX211_TX_FRAME_MANAGEMENT) {
		expected_type = AX211_TX_FRAME_TYPE_MANAGEMENT;
	} else {
		expected_type = AX211_TX_FRAME_TYPE_DATA;
	}

	/* Refuses a frame of another type. */
	if ((frame_control & AX211_TX_FRAME_TYPE_MASK) != expected_type)
		return INTEL_AX211_TX_INVALID;

	/* Management frames are not encrypted by this driver. */
	if (request->frame_class == INTEL_AX211_TX_FRAME_MANAGEMENT &&
	    request->encrypted) {
		return INTEL_AX211_TX_UNSUPPORTED;
	}

	/*
	 * Refuses a plain frame that carries key fields or is marked protected.
	 */
	if (!request->encrypted &&
	    (request->key_generation != 0U ||
	     request->packet_number != 0U ||
	     request->key_index != 0U ||
	     (frame_control & AX211_TX_PROTECTED) != 0U)) {
		return INTEL_AX211_TX_INVALID;
	}

	/*
	 * Refuses an encrypted frame without a key, a usable packet number or
	 * the mark.
	 */
	if (request->encrypted &&
	    (request->key_generation == 0U ||
	     request->packet_number == 0U ||
	     request->packet_number > 0x0000ffffffffffffULL ||
	     (frame_control & AX211_TX_PROTECTED) == 0U)) {
		return INTEL_AX211_TX_INVALID;
	}

	/* A plain frame needs no CCMP header. */
	if (!request->encrypted)
		return INTEL_AX211_TX_OK;

	/* Checks the CCMP header of an encrypted frame. */
	result = ax211_tx_ccmp_validate(request, header_length);
	if (result != INTEL_AX211_TX_OK)
		return result;

	/* Succeeded: the request is consistent. */
	return INTEL_AX211_TX_OK;
}

/*
 * Checks that the CCMP header of a frame matches the request's key and packet
 * number.
 */
static int
ax211_tx_ccmp_validate(
	const struct intel_ax211_tx_request *request,
	size_t header_length)
{
	const uint8_t *ccmp;
	uint64_t packet_number;

	/* Refuses a frame too short for the CCMP header. */
	if (header_length > request->length ||
	    request->length - header_length < AX211_TX_CCMP_HEADER_SIZE) {
		return INTEL_AX211_TX_TRUNCATED;
	}

	/* Refuses a reserved byte, a missing Ext IV or another key index. */
	ccmp = request->frame + header_length;
	if (ccmp[2U] != 0U ||
	    (ccmp[3U] & 0x3fU) != AX211_TX_CCMP_EXTENDED_IV ||
	    (ccmp[3U] >> 6) != request->key_index) {
		return INTEL_AX211_TX_INVALID;
	}

	/* Refuses a header whose packet number is not the request's. */
	packet_number = (uint64_t)ccmp[0U] | ((uint64_t)ccmp[1U] << 8) |
			((uint64_t)ccmp[4U] << 16) |
			((uint64_t)ccmp[5U] << 24) |
			((uint64_t)ccmp[6U] << 32) | ((uint64_t)ccmp[7U] << 40);
	if (packet_number != request->packet_number)
		return INTEL_AX211_TX_STALE;

	/* Succeeded: the header matches. */
	return INTEL_AX211_TX_OK;
}

/* Stores a 16-bit value in little-endian byte order. */
static void
ax211_tx_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
}

/* Stores a 32-bit value in little-endian byte order. */
static void
ax211_tx_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
	bytes[2U] = (uint8_t)(value >> 16);
	bytes[3U] = (uint8_t)(value >> 24);
}

/* Reads a 32-bit little-endian value. */
static uint32_t
ax211_tx_get_le32(
	const uint8_t *bytes)
{
	/* Assembles the word from its four bytes, least significant first. */
	return (uint32_t)bytes[0U] | ((uint32_t)bytes[1U] << 8) |
	       ((uint32_t)bytes[2U] << 16) | ((uint32_t)bytes[3U] << 24);
}
