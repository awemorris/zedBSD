/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 receive implementation.
 */

#include "intel-ax211-rx.h"
#include <kern/kcrt.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The status word of the receive descriptor (bytes 12 to 15).
 *
 * A frame is delivered only when FCS_GOOD (bit 0, the frame check sequence was
 * right) and NO_OVERRUN (bit 1) are both set; a frame with REPEATED_FRAME (bit
 * 22, the firmware's duplicate detection) is dropped.  Bits 10:8 name the
 * cipher the firmware applied: 0 none, 2 CCMP.  A CCMP frame is accepted only
 * with PLAINTEXT_RESTORED (bit 11, the firmware decrypted it) and MIC_GOOD
 * (bit 6, its integrity check passed).
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/rx.h (enum iwl_mvm_rx_status).
 */
#define AX211_RX_CHECK_FCS_GOOD 0x00000001U
#define AX211_RX_CHECK_NO_OVERRUN 0x00000002U
#define AX211_RX_CHECK_MIC_GOOD 0x00000040U
#define AX211_RX_CIPHER_FIELD_MASK 0x00000700U
#define AX211_RX_CIPHER_FIELD_NONE 0x00000000U
#define AX211_RX_CIPHER_FIELD_CCMP 0x00000200U
#define AX211_RX_PLAINTEXT_RESTORED 0x00000800U
#define AX211_RX_REPEATED_FRAME 0x00400000U

/*
 * The MAC flags and radio information of the receive descriptor.
 *
 * Byte 3: PADDING (bit 5) says the firmware inserted two bytes after the MAC
 * header (and the CCMP header) to align the body, which the driver removes;
 * AMSDU (bit 6) marks a frame of an aggregate, which the driver does not
 * accept.  Bits 7:4 of byte 2 give the length of the trailer the hardware left
 * at the end, in units of two bytes; the driver cuts it off and appends a
 * zeroed 8-byte MIC field in its place for a CCMP frame, so the frame handed
 * on has the usual 802.11 shape.  The 16-bit radio information at bytes 5 and
 * 6 has RADIO_TIME_REPLACED (bit 8) set when the 64-bit word at byte 48 holds
 * something other than the TSF; only otherwise is the TSF used.  The other
 * fields used are the two antennas' energies at bytes 40 and 41 (turned into a
 * signal strength), the channel at byte 42 and, at byte 44, the device clock
 * at the moment the frame started on the air.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/rx.h (the MAC flag bytes and the
 * PHY information word of struct iwl_rx_mpdu_desc).
 */
#define AX211_RX_MAC_FLAG_PADDING 0x20U
#define AX211_RX_MAC_FLAG_AMSDU 0x40U
#define AX211_RX_MAC_FLAG1_MIC_CRC_LENGTH_MASK 0xf0U
#define AX211_RX_MAC_FLAG1_MIC_CRC_LENGTH_SHIFT 3U
#define AX211_RX_RADIO_TIME_REPLACED 0x0100U

/*
 * The 802.11 frame control field.
 *
 * The driver uses them to find the header length of a received frame.
 *
 * Values referenced: IEEE Std 802.11-2020 9.2.4.1 (frame control: protocol
 * version, type in bits 3:2, subtype in bits 7:4, To DS, From DS, Protected
 * Frame and +HTC/Order) and 9.2.4.1.3 (the control subtypes CTS 12 and ACK
 * 13).
 */
#define AX211_FRAME_TYPE_MASK 0x000cU
#define AX211_FRAME_TYPE_MANAGEMENT 0x0000U
#define AX211_FRAME_TYPE_CONTROL 0x0004U
#define AX211_FRAME_TYPE_DATA 0x0008U
#define AX211_FRAME_SUBTYPE_MASK 0x00f0U
#define AX211_FRAME_SUBTYPE_QOS 0x0080U
#define AX211_FRAME_TO_DS 0x0100U
#define AX211_FRAME_FROM_DS 0x0200U
#define AX211_FRAME_PROTECTED 0x4000U
#define AX211_FRAME_ORDER 0x8000U
#define AX211_FRAME_CONTROL_CTS 0x00c0U
#define AX211_FRAME_CONTROL_ACK 0x00d0U

/*
 * The CCMP header and MIC.
 *
 * Values referenced: IEEE Std 802.11-2020 12.5.3.2 (the 8-byte CCMP header
 * with the Ext IV bit, bit 5 of its fourth byte, always set, and the 8-byte
 * MIC of CCMP-128).
 */
#define AX211_CCMP_HEADER_SIZE 8U
#define AX211_CCMP_TAG_SIZE 8U
#define AX211_CCMP_EXTENDED_IV 0x20U

static uint16_t ax211_rx_get_le16(const uint8_t *bytes);
static uint32_t ax211_rx_get_le32(const uint8_t *bytes);
static int32_t ax211_rx_rssi(uint8_t energy_a, uint8_t energy_b);
static uint64_t ax211_rx_get_le64(const uint8_t *bytes);
static int ax211_rx_header_length(const uint8_t *frame, size_t length, size_t *header_length);
static int ax211_rx_security(struct intel_ax211_rx_delivery *mpdu, const uint8_t *frame, size_t length, size_t header_length);

/*
 * Checks that the pinned image lists the received-frame notification in the
 * layout decoded here.
 */
int
drv_intel_ax211_rx_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Refuses a missing command table. */
	if (table == NULL)
		return INTEL_AX211_RX_INVALID;

	/* Looks up the versions the image lists for the notification. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, INTEL_AX211_RX_FRAME_FAMILY, INTEL_AX211_RX_FRAME_OPCODE,
		&version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RX_UNSUPPORTED;

	/* Refuses another layout; the notification has no command version. */
	if (version.command_version != INTEL_AX211_PROTOCOL_UNKNOWN_VERSION ||
	    version.notification_version !=
		    INTEL_AX211_RX_FRAME_NOTIFICATION_VERSION) {
		return INTEL_AX211_RX_UNSUPPORTED;
	}

	/* Succeeded: the image speaks the notification this driver decodes. */
	return INTEL_AX211_RX_OK;
}

/*
 * Decodes one received 802.11 frame into the caller's buffer.
 *
 * The frame leaves in its usual shape: the padding the firmware inserted is
 * removed, the hardware trailer is cut off, and a CCMP frame gets a zeroed
 * 8-byte MIC field in its place.
 */
int
drv_intel_ax211_rx_mpdu_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation,
	uint8_t *output,
	size_t output_capacity,
	struct intel_ax211_rx_delivery *mpdu)
{
	struct intel_ax211_rx_delivery decoded;
	const uint8_t *descriptor;
	const uint8_t *frame;
	size_t frame_length;
	size_t header_length;
	size_t hardware_trailer_length;
	size_t common_trailer_length;
	size_t copied_length;
	size_t normalized_length;
	size_t padding_offset;
	uint16_t frame_control;
	uint16_t phy_info;
	uint8_t mac_flags;
	int result;

	/* Refuses missing buffers and unset epochs. */
	if (message == NULL ||
	    output == NULL ||
	    mpdu == NULL ||
	    generation == 0U ||
	    message->generation == 0U) {
		return INTEL_AX211_RX_INVALID;
	}

	/* Refuses a message of an earlier firmware epoch. */
	if (message->generation != generation)
		return INTEL_AX211_RX_STALE;

	/* Refuses a message other than the received-frame notification. */
	if (message->group != INTEL_AX211_RX_FRAME_FAMILY ||
	    message->opcode != INTEL_AX211_RX_FRAME_OPCODE ||
	    message->version != INTEL_AX211_RX_FRAME_NOTIFICATION_VERSION) {
		return INTEL_AX211_RX_UNSUPPORTED;
	}

	/* Reports a notification the firmware marked as failed. */
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_RX_FAILED;

	/* Refuses a missing payload. */
	if (message->payload == NULL)
		return INTEL_AX211_RX_INVALID;

	/* Refuses a payload shorter than the descriptor. */
	if (message->payload_length < INTEL_AX211_RX_FRAME_DESCRIPTOR_SIZE)
		return INTEL_AX211_RX_TRUNCATED;

	/* Refuses a frame longer than an 802.11 frame can be. */
	descriptor = message->payload;
	frame_length = ax211_rx_get_le16(descriptor);
	if (frame_length > INTEL_AX211_RX_FRAME_BODY_MAX)
		return INTEL_AX211_RX_OVERSIZED;

	/* Refuses a frame that runs past the payload. */
	if (message->payload_length - INTEL_AX211_RX_FRAME_DESCRIPTOR_SIZE <
	    frame_length) {
		return INTEL_AX211_RX_TRUNCATED;
	}

	/*
	 * The transport may round an RX packet up and leave bytes after the
	 * descriptor-declared MPDU.  They are not part of the 802.11 frame.
	 */
	if (frame_length < 2U)
		return INTEL_AX211_RX_TRUNCATED;

	/* Refuses a frame whose check sequence failed or that overran. */
	kern_memset(&decoded, 0, sizeof(decoded));
	decoded.status = ax211_rx_get_le32(descriptor + 12U);
	if ((decoded.status &
	     (AX211_RX_CHECK_FCS_GOOD | AX211_RX_CHECK_NO_OVERRUN)) !=
	    (AX211_RX_CHECK_FCS_GOOD | AX211_RX_CHECK_NO_OVERRUN)) {
		return INTEL_AX211_RX_FAILED;
	}

	/* Drops a frame the firmware recognised as a repeat. */
	if ((decoded.status & AX211_RX_REPEATED_FRAME) != 0U)
		return INTEL_AX211_RX_DUPLICATE;

	/* Refuses a frame of an aggregate. */
	mac_flags = descriptor[3U];
	if ((mac_flags & AX211_RX_MAC_FLAG_AMSDU) != 0U)
		return INTEL_AX211_RX_UNSUPPORTED;

	/*
	 * Takes the signal strength and the channel; a frame needs a channel.
	 */
	decoded.rssi_dbm = ax211_rx_rssi(descriptor[40U], descriptor[41U]);
	decoded.channel = descriptor[42U];
	if (decoded.channel == 0U)
		return INTEL_AX211_RX_FAILED;

	/* Takes the start time on the air. */
	decoded.air_start_clock = ax211_rx_get_le32(descriptor + 44U);

	/* Takes the TSF unless the radio replaced that word. */
	phy_info = ax211_rx_get_le16(descriptor + 5U);
	if ((phy_info & AX211_RX_RADIO_TIME_REPLACED) == 0U) {
		decoded.tsf = ax211_rx_get_le64(descriptor + 48U);
		decoded.tsf_valid = 1U;
	}

	/* Finds the MAC header length of the frame as the firmware wrote it. */
	frame = descriptor + INTEL_AX211_RX_FRAME_DESCRIPTOR_SIZE;
	result = ax211_rx_header_length(frame, frame_length, &header_length);
	if (result != INTEL_AX211_RX_OK)
		return result;

	/* Refuses a hardware trailer longer than the frame. */
	hardware_trailer_length =
		(descriptor[2U] & AX211_RX_MAC_FLAG1_MIC_CRC_LENGTH_MASK) >>
		AX211_RX_MAC_FLAG1_MIC_CRC_LENGTH_SHIFT;
	if (hardware_trailer_length > frame_length)
		return INTEL_AX211_RX_TRUNCATED;

	/* A CCMP frame gets an 8-byte MIC field at its end; others get none. */
	if ((decoded.status & AX211_RX_CIPHER_FIELD_MASK) ==
	    AX211_RX_CIPHER_FIELD_CCMP) {
		common_trailer_length = AX211_CCMP_TAG_SIZE;
	} else {
		common_trailer_length = 0U;
	}

	/* Takes the frame without the hardware trailer. */
	copied_length = frame_length - hardware_trailer_length;

	/* The padding follows the MAC header, and the CCMP header if any. */
	padding_offset = header_length;
	if ((decoded.status & AX211_RX_CIPHER_FIELD_MASK) ==
	    AX211_RX_CIPHER_FIELD_CCMP) {
		/* Refuses a CCMP frame too short for the CCMP header. */
		if (padding_offset > copied_length ||
		    copied_length - padding_offset < AX211_CCMP_HEADER_SIZE) {
			return INTEL_AX211_RX_TRUNCATED;
		}

		/* Places the padding after the CCMP header. */
		padding_offset += AX211_CCMP_HEADER_SIZE;
	}

	/* Leaves the two bytes of padding out of the copy. */
	if ((mac_flags & AX211_RX_MAC_FLAG_PADDING) != 0U) {
		/* Refuses a frame too short for the padding. */
		if (padding_offset > copied_length ||
		    copied_length - padding_offset < 2U) {
			return INTEL_AX211_RX_TRUNCATED;
		}

		/* The copy is two bytes shorter. */
		copied_length -= 2U;
	}

	/* Refuses a length that overflows with the MIC field. */
	if (copied_length > SIZE_MAX - common_trailer_length)
		return INTEL_AX211_RX_OVERSIZED;

	/* Refuses a frame that does not fit the caller's buffer. */
	normalized_length = copied_length + common_trailer_length;
	if (normalized_length > output_capacity)
		return INTEL_AX211_RX_BUFFER_TOO_SMALL;

	/* Copies the frame around the padding, or whole when there is none. */
	if ((mac_flags & AX211_RX_MAC_FLAG_PADDING) != 0U) {
		kern_memcpy(output, frame, padding_offset);
		kern_memcpy(output + padding_offset, frame + padding_offset + 2U,
		       copied_length - padding_offset);
	} else {
		kern_memcpy(output, frame, copied_length);
	}

	/* Writes the zeroed MIC field. */
	if (common_trailer_length != 0U)
		kern_memset(output + copied_length, 0, common_trailer_length);

	/* Finds the MAC header length of the frame in its final shape. */
	frame_control = ax211_rx_get_le16(output);
	result = ax211_rx_header_length(output, normalized_length,
					&header_length);
	if (result != INTEL_AX211_RX_OK)
		return result;

	/* Decodes the protection of the frame. */
	decoded.frame = output;
	decoded.length = normalized_length;
	result = ax211_rx_security(&decoded, output, normalized_length,
				   header_length);
	if (result != INTEL_AX211_RX_OK)
		return result;

	/* Refuses a decrypted frame that is not marked protected. */
	if ((frame_control & AX211_FRAME_PROTECTED) == 0U &&
	    decoded.cipher != INTEL_AX211_RX_PROTECTION_NONE) {
		return INTEL_AX211_RX_FAILED;
	}

	/* Succeeded: publishes the decoded frame. */
	*mpdu = decoded;
	return INTEL_AX211_RX_OK;
}

/* Reads a 16-bit little-endian value. */
static uint16_t
ax211_rx_get_le16(
	const uint8_t *bytes)
{
	/* Assembles the value from its two bytes, least significant first. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* Reads a 32-bit little-endian value. */
static uint32_t
ax211_rx_get_le32(
	const uint8_t *bytes)
{
	/* Assembles the word from its four bytes, least significant first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Turns the energies of the two antennas into the stronger signal in dBm. */
static int32_t
ax211_rx_rssi(
	uint8_t energy_a,
	uint8_t energy_b)
{
	int32_t signal_a;
	int32_t signal_b;

	/* An energy of zero means the antenna received nothing. */
	signal_a = energy_a == 0U ? -256 : -(int32_t)energy_a;
	signal_b = energy_b == 0U ? -256 : -(int32_t)energy_b;

	/* Reports the stronger of the two. */
	return signal_a > signal_b ? signal_a : signal_b;
}

/* Reads a 64-bit little-endian value. */
static uint64_t
ax211_rx_get_le64(
	const uint8_t *bytes)
{
	uint64_t value;

	/* Assembles the value from its two words, the low word first. */
	value = (uint64_t)ax211_rx_get_le32(bytes) |
		((uint64_t)ax211_rx_get_le32(bytes + 4U) << 32);

	/* Reports the value. */
	return value;
}

/* Finds the length of the MAC header of an 802.11 frame. */
static int
ax211_rx_header_length(
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
		return INTEL_AX211_RX_INVALID;

	/* Takes the type and the subtype of the frame. */
	frame_control = ax211_rx_get_le16(frame);
	type = frame_control & AX211_FRAME_TYPE_MASK;
	subtype = frame_control & AX211_FRAME_SUBTYPE_MASK;

	/* Measures the header by type: management, control or data. */
	if (type == AX211_FRAME_TYPE_MANAGEMENT) {
		required = 24U;
	} else if (type == AX211_FRAME_TYPE_CONTROL) {
		/*
		 * CTS and ACK carry one address, the other control frames two.
		 */
		if (subtype == AX211_FRAME_CONTROL_CTS ||
		    subtype == AX211_FRAME_CONTROL_ACK) {
			required = 10U;
		} else {
			required = 16U;
		}
	} else if (type == AX211_FRAME_TYPE_DATA) {
		/*
		 * A frame between two distribution systems has a fourth
		 * address.
		 */
		required = 24U;
		if ((frame_control &
		     (AX211_FRAME_TO_DS | AX211_FRAME_FROM_DS)) ==
		    (AX211_FRAME_TO_DS | AX211_FRAME_FROM_DS))
			required += 6U;

		/* A QoS data frame carries the QoS control field. */
		if ((subtype & AX211_FRAME_SUBTYPE_QOS) != 0U) {
			required += 2U;

			/* With +HTC/Order set it also carries HT control. */
			if ((frame_control & AX211_FRAME_ORDER) != 0U)
				required += 4U;
		}
	} else {
		return INTEL_AX211_RX_UNSUPPORTED;
	}

	/* Refuses a frame shorter than its header. */
	if (length < required)
		return INTEL_AX211_RX_TRUNCATED;

	/* Succeeded: reports the header length. */
	*header_length = required;
	return INTEL_AX211_RX_OK;
}

/* Decodes the protection of a received frame and its CCMP packet number. */
static int
ax211_rx_security(
	struct intel_ax211_rx_delivery *mpdu,
	const uint8_t *frame,
	size_t length,
	size_t header_length)
{
	const uint8_t *ccmp;
	uint16_t frame_control;
	uint32_t security;

	/* Takes the cipher the firmware applied. */
	frame_control = ax211_rx_get_le16(frame);
	security = mpdu->status & AX211_RX_CIPHER_FIELD_MASK;

	/* A plain frame must not be marked protected. */
	if (security == AX211_RX_CIPHER_FIELD_NONE) {
		/* Refuses a protected frame the firmware did not decrypt. */
		if ((frame_control & AX211_FRAME_PROTECTED) != 0U)
			return INTEL_AX211_RX_FAILED;

		/* Succeeded: the frame is plain. */
		return INTEL_AX211_RX_OK;
	}

	/* Ciphers other than CCMP are not handled. */
	if (security != AX211_RX_CIPHER_FIELD_CCMP)
		return INTEL_AX211_RX_UNSUPPORTED;

	/*
	 * Refuses a CCMP frame not marked protected, not decrypted or not
	 * checked.
	 */
	if ((frame_control & AX211_FRAME_PROTECTED) == 0U ||
	    (mpdu->status &
	     (AX211_RX_PLAINTEXT_RESTORED | AX211_RX_CHECK_MIC_GOOD)) !=
		    (AX211_RX_PLAINTEXT_RESTORED | AX211_RX_CHECK_MIC_GOOD)) {
		return INTEL_AX211_RX_FAILED;
	}

	/* Refuses a frame too short for the CCMP header. */
	if (length < header_length + AX211_CCMP_HEADER_SIZE)
		return INTEL_AX211_RX_TRUNCATED;

	/*
	 * Refuses a CCMP header with a nonzero reserved byte or without Ext IV.
	 */
	ccmp = frame + header_length;
	if (ccmp[2U] != 0U || (ccmp[3U] & AX211_CCMP_EXTENDED_IV) == 0U)
		return INTEL_AX211_RX_FAILED;

	/* Assembles the 48-bit packet number from bytes 0, 1 and 4 to 7. */
	mpdu->packet_number =
		(uint64_t)ccmp[0U] | ((uint64_t)ccmp[1U] << 8) |
		((uint64_t)ccmp[4U] << 16) | ((uint64_t)ccmp[5U] << 24) |
		((uint64_t)ccmp[6U] << 32) | ((uint64_t)ccmp[7U] << 40);

	/* Refuses a zero packet number. */
	if (mpdu->packet_number == 0U)
		return INTEL_AX211_RX_FAILED;

	/* Records the key index and the protection. */
	mpdu->key_index = (uint8_t)((ccmp[3U] >> 6) & 0x03U);
	mpdu->cipher = INTEL_AX211_RX_PROTECTION_CCMP;
	mpdu->decrypted = 1U;

	/* Succeeded: the frame was decrypted and checked. */
	return INTEL_AX211_RX_OK;
}
