/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private API89 receive implementation
 *
 * Wire constants and descriptor layout are derived from OpenBSD
 * sys/dev/pci/if_iwxreg.h and if_iwx.c at commit
 * 0f464d413c50396e4e6cd70948f15613d6a73081.
 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "intel-ax211-rx.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AX211_RX_STATUS_CRC_OK 0x00000001U
#define AX211_RX_STATUS_OVERRUN_OK 0x00000002U
#define AX211_RX_STATUS_MIC_OK 0x00000040U
#define AX211_RX_STATUS_SECURITY_MASK 0x00000700U
#define AX211_RX_STATUS_SECURITY_NONE 0x00000000U
#define AX211_RX_STATUS_SECURITY_CCMP 0x00000200U
#define AX211_RX_STATUS_DECRYPTED 0x00000800U
#define AX211_RX_STATUS_DUPLICATE 0x00400000U

#define AX211_RX_MAC_FLAG_PADDING 0x20U
#define AX211_RX_MAC_FLAG_AMSDU 0x40U
#define AX211_RX_MAC_FLAG1_MIC_CRC_LENGTH_MASK 0xf0U
#define AX211_RX_MAC_FLAG1_MIC_CRC_LENGTH_SHIFT 3U
#define AX211_RX_PHY_INFO_TSF_OVERLOAD 0x0100U

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

#define AX211_CCMP_HEADER_SIZE 8U
#define AX211_CCMP_MIC_SIZE 8U
#define AX211_CCMP_EXTENDED_IV 0x20U

static uint16_t ax211_rx_get_le16(const uint8_t *bytes);
static uint32_t ax211_rx_get_le32(const uint8_t *bytes);
static int32_t ax211_rx_rssi(uint8_t energy_a, uint8_t energy_b);
static uint64_t ax211_rx_get_le64(const uint8_t *bytes);
static int ax211_rx_header_length(const uint8_t *frame, size_t length, size_t *header_length);
static int ax211_rx_security(struct intel_ax211_rx_mpdu *mpdu, const uint8_t *frame, size_t length, size_t header_length);

/*
 * Implements the drv intel ax211 rx api89 validate operation.
 */
int
drv_intel_ax211_rx_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Handles the table availability. */
	if (table == NULL)
		return INTEL_AX211_RX_INVALID;
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, INTEL_AX211_RX_MPDU_GROUP, INTEL_AX211_RX_MPDU_OPCODE,
		&version);

	/* Checks the operation result. */
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RX_UNSUPPORTED;

	/* Handles the version condition. */
	if (version.command_version != INTEL_AX211_PROTOCOL_UNKNOWN_VERSION ||
	    version.notification_version !=
		    INTEL_AX211_RX_MPDU_NOTIFICATION_VERSION)

		/* Returns the computed result. */
		return INTEL_AX211_RX_UNSUPPORTED;

	/* Returns the computed result. */
	return INTEL_AX211_RX_OK;
}

/*
 * Implements the drv intel ax211 rx mpdu decode operation.
 */
int
drv_intel_ax211_rx_mpdu_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation,
	uint8_t *output,
	size_t output_capacity,
	struct intel_ax211_rx_mpdu *mpdu)
{
	struct intel_ax211_rx_mpdu decoded;
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

	/* Handles the message availability. */
	if (message == NULL || output == NULL || mpdu == NULL ||
	    generation == 0U || message->generation == 0U)

		/* Returns the computed result. */
		return INTEL_AX211_RX_INVALID;

	/* Handles the message condition. */
	if (message->generation != generation)
		return INTEL_AX211_RX_STALE;

	/* Handles the message condition. */
	if (message->group != INTEL_AX211_RX_MPDU_GROUP ||
	    message->opcode != INTEL_AX211_RX_MPDU_OPCODE ||
	    message->version != INTEL_AX211_RX_MPDU_NOTIFICATION_VERSION)

		/* Returns the computed result. */
		return INTEL_AX211_RX_UNSUPPORTED;

	/* Checks the operation status. */
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_RX_FAILED;

	/* Handles the payload availability. */
	if (message->payload == NULL)
		return INTEL_AX211_RX_INVALID;

	/* Handles the message condition. */
	if (message->payload_length < INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE)
		return INTEL_AX211_RX_TRUNCATED;

	descriptor = message->payload;
	frame_length = ax211_rx_get_le16(descriptor);

	/* Handles the frame length condition. */
	if (frame_length > INTEL_AX211_RX_MPDU_FRAME_MAX)
		return INTEL_AX211_RX_OVERSIZED;

	/* Handles the message condition. */
	if (message->payload_length - INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE <
	    frame_length)

		/* Returns the computed result. */
		return INTEL_AX211_RX_TRUNCATED;

	/*
 * The transport may round an RX packet up and leave bytes after the
	 * descriptor-declared MPDU.  They are not part of the 802.11 frame. */
	if (frame_length < 2U)
		return INTEL_AX211_RX_TRUNCATED;

	memset(&decoded, 0, sizeof(decoded));
	decoded.status = ax211_rx_get_le32(descriptor + 12U);

	/* Handles the decoded condition. */
	if ((decoded.status &
	     (AX211_RX_STATUS_CRC_OK | AX211_RX_STATUS_OVERRUN_OK)) !=
	    (AX211_RX_STATUS_CRC_OK | AX211_RX_STATUS_OVERRUN_OK))

		/* Returns the computed result. */
		return INTEL_AX211_RX_FAILED;

	/* Handles the decoded condition. */
	if ((decoded.status & AX211_RX_STATUS_DUPLICATE) != 0U)
		return INTEL_AX211_RX_DUPLICATE;
	mac_flags = descriptor[3U];

	/* Handles the mac flags condition. */
	if ((mac_flags & AX211_RX_MAC_FLAG_AMSDU) != 0U)
		return INTEL_AX211_RX_UNSUPPORTED;
	decoded.rssi_dbm = ax211_rx_rssi(descriptor[40U], descriptor[41U]);
	decoded.channel = descriptor[42U];

	/* Handles the decoded condition. */
	if (decoded.channel == 0U)
		return INTEL_AX211_RX_FAILED;
	decoded.gp2_on_air_rise = ax211_rx_get_le32(descriptor + 44U);
	phy_info = ax211_rx_get_le16(descriptor + 5U);

	/* Handles the phy info condition. */
	if ((phy_info & AX211_RX_PHY_INFO_TSF_OVERLOAD) == 0U) {
		decoded.tsf = ax211_rx_get_le64(descriptor + 48U);
		decoded.tsf_valid = 1U;
	}

	frame = descriptor + INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE;
	result = ax211_rx_header_length(frame, frame_length, &header_length);

	/* Checks the operation result. */
	if (result != INTEL_AX211_RX_OK)
		return result;
	hardware_trailer_length =
		(descriptor[2U] & AX211_RX_MAC_FLAG1_MIC_CRC_LENGTH_MASK) >>
		AX211_RX_MAC_FLAG1_MIC_CRC_LENGTH_SHIFT;

	/* Handles the hardware trailer length condition. */
	if (hardware_trailer_length > frame_length)
		return INTEL_AX211_RX_TRUNCATED;
	common_trailer_length =
		(decoded.status & AX211_RX_STATUS_SECURITY_MASK) ==
				AX211_RX_STATUS_SECURITY_CCMP
			? AX211_CCMP_MIC_SIZE
			: 0U;
	copied_length = frame_length - hardware_trailer_length;
	padding_offset = header_length;

	/* Handles the decoded condition. */
	if ((decoded.status & AX211_RX_STATUS_SECURITY_MASK) ==
	    AX211_RX_STATUS_SECURITY_CCMP) {
		/* Handles the padding offset condition. */
		if (padding_offset > copied_length ||
		    copied_length - padding_offset < AX211_CCMP_HEADER_SIZE)

			/* Returns the computed result. */
			return INTEL_AX211_RX_TRUNCATED;
		padding_offset += AX211_CCMP_HEADER_SIZE;
	}

	/* Handles the mac flags condition. */
	if ((mac_flags & AX211_RX_MAC_FLAG_PADDING) != 0U) {
		/* Handles the padding offset condition. */
		if (padding_offset > copied_length ||
		    copied_length - padding_offset < 2U)

			/* Returns the computed result. */
			return INTEL_AX211_RX_TRUNCATED;
		copied_length -= 2U;
	}

	/* Handles the copied length condition. */
	if (copied_length > SIZE_MAX - common_trailer_length)
		return INTEL_AX211_RX_OVERSIZED;
	normalized_length = copied_length + common_trailer_length;

	/* Handles the normalized length condition. */
	if (normalized_length > output_capacity)
		return INTEL_AX211_RX_BUFFER_TOO_SMALL;

	/* Handles the mac flags condition. */
	if ((mac_flags & AX211_RX_MAC_FLAG_PADDING) != 0U) {
		memcpy(output, frame, padding_offset);
		memcpy(output + padding_offset, frame + padding_offset + 2U,
		       copied_length - padding_offset);
	} else {
		memcpy(output, frame, copied_length);
	}

	/* Handles the common trailer length condition. */
	if (common_trailer_length != 0U)
		memset(output + copied_length, 0, common_trailer_length);
	frame_control = ax211_rx_get_le16(output);
	result = ax211_rx_header_length(output, normalized_length,
					&header_length);

	/* Checks the operation result. */
	if (result != INTEL_AX211_RX_OK)
		return result;
	decoded.frame = output;
	decoded.length = normalized_length;
	result = ax211_rx_security(&decoded, output, normalized_length,
				   header_length);

	/* Checks the operation result. */
	if (result != INTEL_AX211_RX_OK)
		return result;

	/* Handles the frame control condition. */
	if ((frame_control & AX211_FRAME_PROTECTED) == 0U &&
	    decoded.cipher != INTEL_AX211_RX_CIPHER_NONE)

		/* Returns the computed result. */
		return INTEL_AX211_RX_FAILED;
	*mpdu = decoded;
	/* Returns the computed result. */
	return INTEL_AX211_RX_OK;
}

/* Supports the ax211 rx get le16 operation. */
static uint16_t
ax211_rx_get_le16(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* Supports the ax211 rx get le32 operation. */
static uint32_t
ax211_rx_get_le32(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Supports the ax211 rx rssi operation. */
static int32_t
ax211_rx_rssi(
	uint8_t energy_a,
	uint8_t energy_b)
{
	int32_t signal_a;
	int32_t signal_b;

	signal_a = energy_a == 0U ? -256 : -(int32_t)energy_a;
	signal_b = energy_b == 0U ? -256 : -(int32_t)energy_b;

	/* Returns the computed result. */
	return signal_a > signal_b ? signal_a : signal_b;
}

/* Supports the ax211 rx get le64 operation. */
static uint64_t
ax211_rx_get_le64(
	const uint8_t *bytes)
{
	uint64_t function_result;

	/* Computes the function result. */
	function_result = (uint64_t)ax211_rx_get_le32(bytes) |
			  ((uint64_t)ax211_rx_get_le32(bytes + 4U) << 32);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the ax211 rx header length operation. */
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

	/* Handles the frame availability. */
	if (frame == NULL || header_length == NULL || length < 2U)
		return INTEL_AX211_RX_INVALID;
	frame_control = ax211_rx_get_le16(frame);
	type = frame_control & AX211_FRAME_TYPE_MASK;
	subtype = frame_control & AX211_FRAME_SUBTYPE_MASK;

	/* Handles the type condition. */
	if (type == AX211_FRAME_TYPE_MANAGEMENT) {
		required = 24U;
	} else if (type == AX211_FRAME_TYPE_CONTROL) {
		required = subtype == AX211_FRAME_CONTROL_CTS ||
					   subtype == AX211_FRAME_CONTROL_ACK
				   ? 10U
				   : 16U;
	} else if (type == AX211_FRAME_TYPE_DATA) {
		required = 24U;

		/* Handles the frame control condition. */
		if ((frame_control &
		     (AX211_FRAME_TO_DS | AX211_FRAME_FROM_DS)) ==
		    (AX211_FRAME_TO_DS | AX211_FRAME_FROM_DS))
			required += 6U;

		/* Handles the subtype condition. */
		if ((subtype & AX211_FRAME_SUBTYPE_QOS) != 0U) {
			required += 2U;

			/* Handles the frame control condition. */
			if ((frame_control & AX211_FRAME_ORDER) != 0U)
				required += 4U;
		}
	} else {
		/* Returns the computed result. */
		return INTEL_AX211_RX_UNSUPPORTED;
	}

	/* Checks the current data length. */
	if (length < required)
		return INTEL_AX211_RX_TRUNCATED;
	*header_length = required;
	/* Returns the computed result. */
	return INTEL_AX211_RX_OK;
}

/* Supports the ax211 rx security operation. */
static int
ax211_rx_security(
	struct intel_ax211_rx_mpdu *mpdu,
	const uint8_t *frame,
	size_t length,
	size_t header_length)
{
	const uint8_t *ccmp;
	uint16_t frame_control;
	uint32_t security;

	frame_control = ax211_rx_get_le16(frame);
	security = mpdu->status & AX211_RX_STATUS_SECURITY_MASK;

	/* Handles the security condition. */
	if (security == AX211_RX_STATUS_SECURITY_NONE) {
		/* Handles the frame control condition. */
		if ((frame_control & AX211_FRAME_PROTECTED) != 0U)
			return INTEL_AX211_RX_FAILED;

		/* Returns the computed result. */
		return INTEL_AX211_RX_OK;
	}

	/* Handles the security condition. */
	if (security != AX211_RX_STATUS_SECURITY_CCMP)
		return INTEL_AX211_RX_UNSUPPORTED;

	/* Handles the frame control condition. */
	if ((frame_control & AX211_FRAME_PROTECTED) == 0U ||
	    (mpdu->status &
	     (AX211_RX_STATUS_DECRYPTED | AX211_RX_STATUS_MIC_OK)) !=
		    (AX211_RX_STATUS_DECRYPTED | AX211_RX_STATUS_MIC_OK))

		/* Returns the computed result. */
		return INTEL_AX211_RX_FAILED;

	/* Checks the current data length. */
	if (length < header_length + AX211_CCMP_HEADER_SIZE)
		return INTEL_AX211_RX_TRUNCATED;
	ccmp = frame + header_length;

	/* Handles the ccmp condition. */
	if (ccmp[2U] != 0U || (ccmp[3U] & AX211_CCMP_EXTENDED_IV) == 0U)
		return INTEL_AX211_RX_FAILED;
	mpdu->packet_number =
		(uint64_t)ccmp[0U] | ((uint64_t)ccmp[1U] << 8) |
		((uint64_t)ccmp[4U] << 16) | ((uint64_t)ccmp[5U] << 24) |
		((uint64_t)ccmp[6U] << 32) | ((uint64_t)ccmp[7U] << 40);

	/* Handles the mpdu condition. */
	if (mpdu->packet_number == 0U)
		return INTEL_AX211_RX_FAILED;
	mpdu->key_index = (uint8_t)((ccmp[3U] >> 6) & 0x03U);
	mpdu->cipher = INTEL_AX211_RX_CIPHER_CCMP;
	mpdu->decrypted = 1U;

	/* Returns the computed result. */
	return INTEL_AX211_RX_OK;
}
