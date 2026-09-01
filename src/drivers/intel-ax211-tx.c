/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private API89 transmit implementation
 *
 * Wire constants and layouts are derived from OpenBSD
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

#include "intel-ax211-tx.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AX211_TX_FLAG_COMMAND_RATE                    0x0001U
#define AX211_TX_FLAG_ENCRYPT_DISABLE                 0x0002U
#define AX211_TX_FLAG_HIGH_PRIORITY                   0x0004U

#define AX211_TX_OFFLOAD_MAC_HEADER_SHIFT                   8U
#define AX211_TX_OFFLOAD_MAC_HEADER_MASK                 0x1fU
#define AX211_TX_OFFLOAD_PADDING                       0x2000U

#define AX211_TX_RATE_1M_CCK                              0U
#define AX211_TX_RATE_ANTENNA_A                       0x4000U

#define AX211_TX_FRAME_TYPE_MASK                       0x000cU
#define AX211_TX_FRAME_TYPE_MANAGEMENT                 0x0000U
#define AX211_TX_FRAME_TYPE_CONTROL                    0x0004U
#define AX211_TX_FRAME_TYPE_DATA                       0x0008U
#define AX211_TX_FRAME_SUBTYPE_MASK                    0x00f0U
#define AX211_TX_FRAME_SUBTYPE_QOS                     0x0080U
#define AX211_TX_TO_DS                                 0x0100U
#define AX211_TX_FROM_DS                               0x0200U
#define AX211_TX_PROTECTED                             0x4000U
#define AX211_TX_ORDER                                 0x8000U

#define AX211_TX_CCMP_HEADER_SIZE                           8U
#define AX211_TX_CCMP_EXTENDED_IV                         0x20U

#define AX211_TX_RESPONSE_SIZE                            48U
#define AX211_TX_RESPONSE_STATUS_OFFSET                   40U
#define AX211_TX_RESPONSE_SSN_OFFSET                      44U
#define AX211_TX_STATUS_MASK                      0x000000ffU
#define AX211_TX_STATUS_SUCCESS                           1U
#define AX211_TX_STATUS_DIRECT_DONE                       2U

static uint16_t ax211_tx_get_le16(const uint8_t *bytes);
static uint32_t ax211_tx_get_le32(const uint8_t *bytes);
static void ax211_tx_put_le16(uint8_t *bytes, uint16_t value);
static void ax211_tx_put_le32(uint8_t *bytes, uint32_t value);
static int ax211_tx_header_length(const uint8_t *frame, size_t length,
	size_t *header_length);
static int ax211_tx_request_validate(const struct intel_ax211_tx_request *request,
	size_t header_length);
static int ax211_tx_ccmp_validate(const struct intel_ax211_tx_request *request,
	size_t header_length);

int
intel_ax211_tx_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	if (table == NULL)
		return INTEL_AX211_TX_INVALID;
	result = intel_ax211_protocol_command_version_lookup(table,
	    INTEL_AX211_TX_GROUP, INTEL_AX211_TX_OPCODE, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_TX_UNSUPPORTED;
	if (version.command_version != INTEL_AX211_TX_COMMAND_VERSION ||
	    version.notification_version !=
	    INTEL_AX211_TX_NOTIFICATION_VERSION)
		return INTEL_AX211_TX_UNSUPPORTED;
	return INTEL_AX211_TX_OK;
}

int
intel_ax211_tx_prepare(
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

	if (request == NULL || prepared == NULL || request->frame == NULL)
		return INTEL_AX211_TX_INVALID;
	result = ax211_tx_header_length(request->frame, request->length,
	    &header_length);
	if (result != INTEL_AX211_TX_OK)
		return result;
	result = ax211_tx_request_validate(request, header_length);
	if (result != INTEL_AX211_TX_OK)
		return result;
	firmware_length = request->length;
	if (request->encrypted)
		firmware_length -= AX211_TX_CCMP_HEADER_SIZE;
	if (firmware_length > UINT16_MAX)
		return INTEL_AX211_TX_OVERSIZED;

	memset(&encoded, 0, sizeof(encoded));
	flags = 0U;
	rate = AX211_TX_RATE_1M_CCK | AX211_TX_RATE_ANTENNA_A;
	if (!request->encrypted)
		flags |= AX211_TX_FLAG_ENCRYPT_DISABLE;
	/*
	 * API89 does not have usable data-rate state until TLC is configured.
	 * Keep the first standalone-driver path self-contained by supplying a
	 * conservative 2.4 GHz basic rate for every frame class.  A later TLC
	 * phase may replace this fixed-rate policy without changing the TX ABI.
	 */
	flags |= AX211_TX_FLAG_COMMAND_RATE | AX211_TX_FLAG_HIGH_PRIORITY;
	padding = header_length & 3U;
	if (padding != 0U)
		padding = 4U - padding;
	offload = (uint32_t)((header_length / 2U) &
	    AX211_TX_OFFLOAD_MAC_HEADER_MASK) <<
	    AX211_TX_OFFLOAD_MAC_HEADER_SHIFT;
	if (padding != 0U)
		offload |= AX211_TX_OFFLOAD_PADDING;
	if (INTEL_AX211_TX_COMMAND_FIXED_SIZE + header_length + padding >
	    sizeof(encoded.command))
		return INTEL_AX211_TX_BUFFER_TOO_SMALL;

	ax211_tx_put_le16(encoded.command, (uint16_t)firmware_length);
	ax211_tx_put_le16(encoded.command + 2U, flags);
	ax211_tx_put_le32(encoded.command + 4U, offload);
	ax211_tx_put_le32(encoded.command + 16U, rate);
	memcpy(encoded.command + INTEL_AX211_TX_COMMAND_FIXED_SIZE,
	    request->frame, header_length);
	encoded.command_length = INTEL_AX211_TX_COMMAND_FIXED_SIZE +
	    header_length + padding;
	encoded.payload_offset = header_length;
	if (request->encrypted)
		encoded.payload_offset += AX211_TX_CCMP_HEADER_SIZE;
	encoded.payload_length = request->length - encoded.payload_offset;
	encoded.connection_generation = request->connection_generation;
	encoded.cookie = request->cookie;
	encoded.key_generation = request->key_generation;
	encoded.packet_number = request->packet_number;
	encoded.frame_length = (uint16_t)firmware_length;
	encoded.encrypted = request->encrypted;
	encoded.key_index = request->key_index;
	*prepared = encoded;
	return INTEL_AX211_TX_OK;
}

int
intel_ax211_tx_completion_decode(
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

	if (message == NULL || completion == NULL || hardware_generation == 0U ||
	    message->generation == 0U || expected_queue > UINT8_MAX)
		return INTEL_AX211_TX_INVALID;
	if (message->generation != hardware_generation)
		return INTEL_AX211_TX_STALE;
	if (message->group != INTEL_AX211_TX_GROUP ||
	    message->opcode != INTEL_AX211_TX_OPCODE ||
	    message->version != INTEL_AX211_TX_NOTIFICATION_VERSION)
		return INTEL_AX211_TX_UNSUPPORTED;
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_TX_FAILED;
	if (message->queue != expected_queue || message->index != expected_index)
		return INTEL_AX211_TX_STALE;
	if (message->payload == NULL)
		return INTEL_AX211_TX_INVALID;
	if (message->payload_length < AX211_TX_RESPONSE_SIZE)
		return INTEL_AX211_TX_TRUNCATED;
	if (message->payload_length > AX211_TX_RESPONSE_SIZE)
		return INTEL_AX211_TX_OVERSIZED;
	if (message->payload[0U] != 1U)
		return INTEL_AX211_TX_UNSUPPORTED;
	response_queue = ax211_tx_get_le16(message->payload + 36U);
	if (response_queue != expected_queue)
		return INTEL_AX211_TX_STALE;
	scheduler_sequence = ax211_tx_get_le32(message->payload +
	    AX211_TX_RESPONSE_SSN_OFFSET);
	if (scheduler_sequence >=
	    INTEL_AX211_TX_SCHEDULER_SEQUENCE_LIMIT)
		return INTEL_AX211_TX_FAILED;
	status = ax211_tx_get_le32(message->payload +
	    AX211_TX_RESPONSE_STATUS_OFFSET) & AX211_TX_STATUS_MASK;

	memset(&decoded, 0, sizeof(decoded));
	decoded.hardware_generation = hardware_generation;
	decoded.scheduler_sequence = scheduler_sequence;
	decoded.queue = response_queue;
	decoded.sequence_control = ax211_tx_get_le16(message->payload + 28U);
	decoded.byte_count = ax211_tx_get_le16(message->payload + 30U);
	decoded.index = expected_index;
	decoded.acknowledged = status == AX211_TX_STATUS_SUCCESS ||
	    status == AX211_TX_STATUS_DIRECT_DONE;
	decoded.failure_rts = message->payload[2U];
	decoded.failure_frame = message->payload[3U];
	*completion = decoded;
	return INTEL_AX211_TX_OK;
}

static int
ax211_tx_request_validate(
	const struct intel_ax211_tx_request *request,
	size_t header_length)
{
	uint16_t frame_control;
	uint16_t expected_type;
	int result;

	if (request->connection_generation == 0U || request->cookie == 0U ||
	    request->length < header_length ||
	    request->length > INTEL_AX211_TX_FRAME_MAX ||
	    (request->encrypted != 0U && request->encrypted != 1U) ||
	    request->key_index > 3U ||
	    (request->frame_class != INTEL_AX211_TX_FRAME_MANAGEMENT &&
	    request->frame_class != INTEL_AX211_TX_FRAME_EAPOL &&
	    request->frame_class != INTEL_AX211_TX_FRAME_DATA))
		return INTEL_AX211_TX_INVALID;
	frame_control = ax211_tx_get_le16(request->frame);
	expected_type = request->frame_class == INTEL_AX211_TX_FRAME_MANAGEMENT ?
	    AX211_TX_FRAME_TYPE_MANAGEMENT : AX211_TX_FRAME_TYPE_DATA;
	if ((frame_control & AX211_TX_FRAME_TYPE_MASK) != expected_type)
		return INTEL_AX211_TX_INVALID;
	if (request->frame_class == INTEL_AX211_TX_FRAME_MANAGEMENT &&
	    request->encrypted)
		return INTEL_AX211_TX_UNSUPPORTED;
	if (!request->encrypted && (request->key_generation != 0U ||
	    request->packet_number != 0U || request->key_index != 0U ||
	    (frame_control & AX211_TX_PROTECTED) != 0U))
		return INTEL_AX211_TX_INVALID;
	if (request->encrypted && (request->key_generation == 0U ||
	    request->packet_number == 0U ||
	    request->packet_number > 0x0000ffffffffffffULL ||
	    (frame_control & AX211_TX_PROTECTED) == 0U))
		return INTEL_AX211_TX_INVALID;
	if (!request->encrypted)
		return INTEL_AX211_TX_OK;
	result = ax211_tx_ccmp_validate(request, header_length);
	return result;
}

static int
ax211_tx_ccmp_validate(
	const struct intel_ax211_tx_request *request,
	size_t header_length)
{
	const uint8_t *ccmp;
	uint64_t packet_number;

	if (header_length > request->length ||
	    request->length - header_length < AX211_TX_CCMP_HEADER_SIZE)
		return INTEL_AX211_TX_TRUNCATED;
	ccmp = request->frame + header_length;
	if (ccmp[2U] != 0U ||
	    (ccmp[3U] & 0x3fU) != AX211_TX_CCMP_EXTENDED_IV ||
	    (ccmp[3U] >> 6) != request->key_index)
		return INTEL_AX211_TX_INVALID;
	packet_number = (uint64_t)ccmp[0U] |
	    ((uint64_t)ccmp[1U] << 8) |
	    ((uint64_t)ccmp[4U] << 16) |
	    ((uint64_t)ccmp[5U] << 24) |
	    ((uint64_t)ccmp[6U] << 32) |
	    ((uint64_t)ccmp[7U] << 40);
	if (packet_number != request->packet_number)
		return INTEL_AX211_TX_STALE;
	return INTEL_AX211_TX_OK;
}

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

	if (frame == NULL || header_length == NULL || length < 2U)
		return INTEL_AX211_TX_INVALID;
	frame_control = ax211_tx_get_le16(frame);
	type = frame_control & AX211_TX_FRAME_TYPE_MASK;
	subtype = frame_control & AX211_TX_FRAME_SUBTYPE_MASK;
	if (type == AX211_TX_FRAME_TYPE_MANAGEMENT) {
		required = 24U;
		if ((frame_control & AX211_TX_ORDER) != 0U)
			required += 4U;
	} else if (type == AX211_TX_FRAME_TYPE_DATA) {
		required = 24U;
		if ((frame_control & (AX211_TX_TO_DS | AX211_TX_FROM_DS)) ==
		    (AX211_TX_TO_DS | AX211_TX_FROM_DS))
			required += 6U;
		if ((subtype & AX211_TX_FRAME_SUBTYPE_QOS) != 0U) {
			required += 2U;
			if ((frame_control & AX211_TX_ORDER) != 0U)
				required += 4U;
		}
	} else if (type == AX211_TX_FRAME_TYPE_CONTROL) {
		return INTEL_AX211_TX_UNSUPPORTED;
	} else {
		return INTEL_AX211_TX_INVALID;
	}
	if (required > INTEL_AX211_TX_MAC_HEADER_MAX)
		return INTEL_AX211_TX_OVERSIZED;
	if (length < required)
		return INTEL_AX211_TX_TRUNCATED;
	*header_length = required;
	return INTEL_AX211_TX_OK;
}

static uint16_t
ax211_tx_get_le16(
	const uint8_t *bytes)
{
	return (uint16_t)bytes[0U] | ((uint16_t)bytes[1U] << 8);
}

static uint32_t
ax211_tx_get_le32(
	const uint8_t *bytes)
{
	return (uint32_t)bytes[0U] | ((uint32_t)bytes[1U] << 8) |
	    ((uint32_t)bytes[2U] << 16) | ((uint32_t)bytes[3U] << 24);
}

static void
ax211_tx_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
}

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
