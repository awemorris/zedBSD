/*
 * zedBSD Intel AX211 private firmware protocol codecs
 *
 * Portions derived from OpenBSD sys/dev/pci/if_iwxreg.h and if_iwx.c.
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
 *
 * OpenBSD source provenance:
 *   sys/dev/pci/if_iwx.c, if_iwxreg.h at
 *   0f464d413c50396e4e6cd70948f15613d6a73081
 */

#include "intel-ax211-protocol.h"

#include <string.h>

struct ax211_required_version {
	uint8_t group;
	uint8_t opcode;
	uint8_t command_version;
	uint8_t notification_version;
};

static const struct ax211_required_version ax211_api89_required[] = {
	{ 0x00U, 0x01U, 99U, 6U },
	{ 0x01U, 0x0cU, 5U, 0U },
	{ 0x01U, 0x0dU, 17U, 0U },
	{ 0x0cU, 0x00U, 1U, 0U },
	{ 0x0cU, 0x02U, 1U, 4U },
	{ 0x0cU, 0xfeU, 99U, 1U }
};

static uint16_t
ax211_protocol_get_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

static uint32_t
ax211_protocol_get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t
ax211_protocol_get_le64(const uint8_t *bytes)
{
	uint64_t value = 0;
	unsigned int index;

	for (index = 0; index < 8U; index++)
		value |= (uint64_t)bytes[index] << (index * 8U);
	return value;
}

static void
ax211_protocol_version_at(const struct intel_ax211_protocol_command_table *table,
	size_t index, struct intel_ax211_protocol_command_version *version)
{
	const uint8_t *entry = table->bytes +
	    index * INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE;

	version->opcode = entry[0];
	version->group = entry[1];
	version->command_version = entry[2];
	version->notification_version = entry[3];
}

int
intel_ax211_protocol_command_table_parse(const uint8_t *bytes, size_t length,
	struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_table parsed;

	if (bytes == NULL || table == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;
	if (length == 0U ||
	    length % INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE != 0U)
		return INTEL_AX211_PROTOCOL_TRUNCATED;
	parsed.count = length /
	    INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE;
	if (parsed.count > INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT)
		return INTEL_AX211_PROTOCOL_OVERSIZED;
	parsed.bytes = bytes;
	*table = parsed;
	return INTEL_AX211_PROTOCOL_OK;
}

int
intel_ax211_protocol_command_version_lookup(
	const struct intel_ax211_protocol_command_table *table, uint8_t group,
	uint8_t opcode, struct intel_ax211_protocol_command_version *version)
{
	struct intel_ax211_protocol_command_version candidate;
	size_t index;
	int found = 0;

	if (table == NULL || version == NULL || table->bytes == NULL ||
	    table->count == 0U ||
	    table->count > INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT)
		return INTEL_AX211_PROTOCOL_INVALID;
	memset(&candidate, 0, sizeof(candidate));
	for (index = 0; index < table->count; index++) {
		struct intel_ax211_protocol_command_version entry;

		ax211_protocol_version_at(table, index, &entry);
		if (entry.group == group && entry.opcode == opcode) {
			if (found)
				return INTEL_AX211_PROTOCOL_DUPLICATE;
			candidate = entry;
			found = 1;
		}
	}
	if (!found)
		return INTEL_AX211_PROTOCOL_MISSING;
	*version = candidate;
	return INTEL_AX211_PROTOCOL_OK;
}

static int
ax211_protocol_required_version(
	const struct intel_ax211_protocol_command_table *table,
	const struct ax211_required_version *required)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	result = intel_ax211_protocol_command_version_lookup(table,
	    required->group, required->opcode, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;
	if (version.command_version != required->command_version ||
	    version.notification_version != required->notification_version)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	return INTEL_AX211_PROTOCOL_OK;
}

int
intel_ax211_protocol_command_table_validate_api89(
	const struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_version sentinel;
	size_t index;

	if (table == NULL || table->bytes == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;
	if (table->count != INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	ax211_protocol_version_at(table, table->count - 1U, &sentinel);
	if (sentinel.opcode != 0U || sentinel.group != 0U ||
	    sentinel.command_version != 0U ||
	    sentinel.notification_version != 0U)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	for (index = 0;
	    index < sizeof(ax211_api89_required) /
	    sizeof(ax211_api89_required[0]); index++) {
		int result = ax211_protocol_required_version(table,
		    &ax211_api89_required[index]);

		if (result != INTEL_AX211_PROTOCOL_OK)
			return result;
	}
	return INTEL_AX211_PROTOCOL_OK;
}

static int
ax211_protocol_message_validate(
	const struct intel_ax211_protocol_message *message, uint8_t group,
	uint8_t opcode, uint8_t version, uint32_t generation,
	size_t minimum_payload_length, size_t maximum_payload_length)
{
	if (message == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;
	if (minimum_payload_length > maximum_payload_length)
		return INTEL_AX211_PROTOCOL_INVALID;
	if (message->payload_length != 0U && message->payload == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;
	if (message->generation != generation)
		return INTEL_AX211_PROTOCOL_STALE;
	if (message->group != group || message->opcode != opcode ||
	    message->version != version)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_PROTOCOL_FAILED;
	if (message->payload_length < minimum_payload_length)
		return INTEL_AX211_PROTOCOL_TRUNCATED;
	if (message->payload_length > maximum_payload_length)
		return INTEL_AX211_PROTOCOL_OVERSIZED;
	return INTEL_AX211_PROTOCOL_OK;
}

int
intel_ax211_protocol_command_response_validate(
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending)
{
	int result;

	if (pending == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;
	if (pending->response_version ==
	    INTEL_AX211_PROTOCOL_UNKNOWN_VERSION)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	if (pending->minimum_response_length >
	    pending->maximum_response_length ||
	    pending->maximum_response_length >
	    INTEL_AX211_PROTOCOL_COMMAND_RESPONSE_MAX)
		return INTEL_AX211_PROTOCOL_INVALID;
	result = ax211_protocol_message_validate(message, pending->group,
	    pending->opcode, pending->response_version, pending->generation,
	    pending->minimum_response_length,
	    pending->maximum_response_length);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;
	if (message->queue != pending->queue ||
	    message->index != pending->index)
		return INTEL_AX211_PROTOCOL_TOKEN_MISMATCH;
	return INTEL_AX211_PROTOCOL_OK;
}

static void
ax211_protocol_lmac_decode(const uint8_t *bytes,
	struct intel_ax211_protocol_lmac_alive *lmac)
{
	lmac->major = ax211_protocol_get_le32(bytes);
	lmac->minor = ax211_protocol_get_le32(bytes + 4U);
	lmac->version_subtype = bytes[8];
	lmac->version_type = bytes[9];
	lmac->mac = bytes[10];
	lmac->option = bytes[11];
	lmac->timestamp = ax211_protocol_get_le32(bytes + 12U);
	lmac->error_event_table = ax211_protocol_get_le32(bytes + 16U);
	lmac->log_event_table = ax211_protocol_get_le32(bytes + 20U);
	lmac->cpu_register = ax211_protocol_get_le32(bytes + 24U);
	lmac->debug_config = ax211_protocol_get_le32(bytes + 28U);
	lmac->alive_counter = ax211_protocol_get_le32(bytes + 32U);
	lmac->scheduler_base = ax211_protocol_get_le32(bytes + 36U);
	lmac->store_forward_address = ax211_protocol_get_le32(bytes + 40U);
	lmac->store_forward_size = ax211_protocol_get_le32(bytes + 44U);
}

int
intel_ax211_protocol_alive_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation, struct intel_ax211_protocol_alive *alive)
{
	struct intel_ax211_protocol_alive decoded;
	const uint8_t *bytes;
	int result;

	if (alive == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;
	result = ax211_protocol_message_validate(message,
	    INTEL_AX211_PROTOCOL_GROUP_LEGACY,
	    INTEL_AX211_PROTOCOL_ALIVE_OPCODE,
	    INTEL_AX211_PROTOCOL_ALIVE_VERSION, generation,
	    INTEL_AX211_PROTOCOL_ALIVE_SIZE,
	    INTEL_AX211_PROTOCOL_ALIVE_SIZE);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;
	bytes = message->payload;
	memset(&decoded, 0, sizeof(decoded));
	decoded.status = ax211_protocol_get_le16(bytes);
	decoded.flags = ax211_protocol_get_le16(bytes + 2U);
	ax211_protocol_lmac_decode(bytes + 4U, &decoded.lmac[0]);
	ax211_protocol_lmac_decode(bytes + 52U, &decoded.lmac[1]);
	decoded.umac.major = ax211_protocol_get_le32(bytes + 100U);
	decoded.umac.minor = ax211_protocol_get_le32(bytes + 104U);
	decoded.umac.error_info = ax211_protocol_get_le32(bytes + 108U);
	decoded.umac.debug_print_buffer = ax211_protocol_get_le32(bytes + 112U);
	decoded.sku[0] = ax211_protocol_get_le32(bytes + 116U);
	decoded.sku[1] = ax211_protocol_get_le32(bytes + 120U);
	decoded.sku[2] = ax211_protocol_get_le32(bytes + 124U);
	decoded.imr_base = ax211_protocol_get_le64(bytes + 128U);
	decoded.imr_size = ax211_protocol_get_le32(bytes + 136U);
	decoded.imr_enabled = ax211_protocol_get_le32(bytes + 140U);
	if (decoded.status != INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK)
		return INTEL_AX211_PROTOCOL_FAILED;
	if ((decoded.sku[0] | decoded.sku[1] | decoded.sku[2]) == 0U)
		return INTEL_AX211_PROTOCOL_MISSING;
	if (decoded.imr_enabled != 0U) {
		if (decoded.imr_base == 0U || decoded.imr_size == 0U ||
		    UINT64_MAX - decoded.imr_base < decoded.imr_size)
			return INTEL_AX211_PROTOCOL_INVALID;
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	}
	if (decoded.imr_base != 0U || decoded.imr_size != 0U)
		return INTEL_AX211_PROTOCOL_INVALID;
	*alive = decoded;
	return INTEL_AX211_PROTOCOL_OK;
}

int
intel_ax211_protocol_pnvm_init_complete(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation)
{
	return ax211_protocol_message_validate(message,
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_OPCODE,
	    INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_VERSION, generation, 0U,
	    0U);
}

int
intel_ax211_protocol_init_complete(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation)
{
	return ax211_protocol_message_validate(message,
	    INTEL_AX211_PROTOCOL_GROUP_LEGACY,
	    INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE,
	    INTEL_AX211_PROTOCOL_UNKNOWN_VERSION, generation, 0U, 0U);
}

int
intel_ax211_protocol_nvm_get_info_decode(
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending,
	struct intel_ax211_protocol_nvm *nvm)
{
	struct intel_ax211_protocol_nvm decoded;
	const uint8_t *bytes;
	uint32_t tx_chains, rx_chains;
	size_t index, channel_count;
	int result;

	if (pending == NULL || nvm == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;
	if (pending->group != INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM ||
	    pending->opcode != INTEL_AX211_PROTOCOL_NVM_GET_INFO_OPCODE ||
	    pending->response_version !=
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_VERSION ||
	    pending->minimum_response_length !=
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE ||
	    pending->maximum_response_length !=
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	result = intel_ax211_protocol_command_response_validate(message,
	    pending);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;
	bytes = message->payload;
	memset(&decoded, 0, sizeof(decoded));
	decoded.general_flags = ax211_protocol_get_le32(bytes);
	decoded.nvm_version = ax211_protocol_get_le16(bytes + 4U);
	decoded.board_type = bytes[6];
	decoded.hardware_address_count = bytes[7];
	decoded.mac_sku_flags = ax211_protocol_get_le32(bytes + 8U);
	tx_chains = ax211_protocol_get_le32(bytes + 12U);
	rx_chains = ax211_protocol_get_le32(bytes + 16U);
	decoded.lar_enabled = ax211_protocol_get_le32(bytes + 20U) != 0U;
	decoded.n_channels = ax211_protocol_get_le32(bytes + 24U);
	if ((decoded.general_flags &
	    INTEL_AX211_PROTOCOL_NVM_GENERAL_EMPTY_OTP) != 0U)
		return INTEL_AX211_PROTOCOL_FAILED;
	decoded.band_24_enabled = (decoded.mac_sku_flags &
	    INTEL_AX211_PROTOCOL_NVM_BAND_24_ENABLED) != 0U;
	decoded.band_52_enabled = (decoded.mac_sku_flags &
	    INTEL_AX211_PROTOCOL_NVM_BAND_52_ENABLED) != 0U;
	decoded.ht_enabled = (decoded.mac_sku_flags &
	    INTEL_AX211_PROTOCOL_NVM_11N_ENABLED) != 0U;
	decoded.vht_enabled = (decoded.mac_sku_flags &
	    INTEL_AX211_PROTOCOL_NVM_11AC_ENABLED) != 0U;
	decoded.he_enabled = (decoded.mac_sku_flags &
	    INTEL_AX211_PROTOCOL_NVM_11AX_ENABLED) != 0U;
	decoded.mimo_disabled = (decoded.mac_sku_flags &
	    INTEL_AX211_PROTOCOL_NVM_MIMO_DISABLED) != 0U;
	if (!decoded.band_24_enabled || tx_chains == 0U || rx_chains == 0U)
		return INTEL_AX211_PROTOCOL_MISSING;
	if (tx_chains > UINT8_MAX || rx_chains > UINT8_MAX)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	if (decoded.n_channels > INTEL_AX211_PROTOCOL_NVM_CHANNEL_LIMIT)
		return INTEL_AX211_PROTOCOL_OVERSIZED;
	decoded.tx_chain_mask = (uint8_t)tx_chains;
	decoded.rx_chain_mask = (uint8_t)rx_chains;
	channel_count = decoded.n_channels;
	if (channel_count > INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT)
		channel_count = INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT;
	decoded.channel_24ghz_count = channel_count;
	for (index = 0; index < channel_count; index++) {
		struct intel_ax211_protocol_channel *channel =
		    &decoded.channel_24ghz[index];

		channel->number = (uint8_t)(index + 1U);
		channel->flags = ax211_protocol_get_le32(bytes + 28U +
		    index * sizeof(uint32_t));
		channel->valid = (channel->flags &
		    INTEL_AX211_PROTOCOL_NVM_CHANNEL_VALID) != 0U;
		channel->active = channel->valid && (channel->flags &
		    INTEL_AX211_PROTOCOL_NVM_CHANNEL_ACTIVE) != 0U;
		if (channel->valid)
			decoded.valid_24ghz_count++;
	}
	if (decoded.valid_24ghz_count == 0U)
		return INTEL_AX211_PROTOCOL_MISSING;
	*nvm = decoded;
	return INTEL_AX211_PROTOCOL_OK;
}
