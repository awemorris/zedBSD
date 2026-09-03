/*
 * Intel AX211 private protocol codec fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-protocol.h"

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void
put_le64(uint8_t *bytes, uint64_t value)
{
	unsigned int index;

	for (index = 0; index < 8U; index++)
		bytes[index] = (uint8_t)(value >> (index * 8U));
}

static uint32_t
get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
put_command_version(uint8_t *bytes, size_t index, uint8_t group,
	uint8_t opcode, uint8_t command_version, uint8_t notification_version)
{
	uint8_t *entry = bytes +
	    index * INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE;

	entry[0] = opcode;
	entry[1] = group;
	entry[2] = command_version;
	entry[3] = notification_version;
}

static void
make_api89_command_table(
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES])
{
	size_t index;

	for (index = 0;
	    index < INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U; index++)
		put_command_version(bytes, index, 0x10U, (uint8_t)index, 1U,
		    1U);
	put_command_version(bytes, 0U, 0x00U, 0x01U, 99U, 6U);
	put_command_version(bytes, 1U, 0x01U, 0x0cU, 5U, 0U);
	put_command_version(bytes, 2U, 0x01U, 0x0dU, 17U, 0U);
	put_command_version(bytes, 3U, 0x0cU, 0x00U, 1U, 0U);
	put_command_version(bytes, 4U, 0x0cU, 0x02U, 1U, 4U);
	put_command_version(bytes, 5U, 0x0cU, 0xfeU, 99U, 1U);
	put_command_version(bytes,
	    INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U, 0U, 0U, 0U, 0U);
}

static void
test_command_versions(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES + 4U];
	uint8_t malformed[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_command_version version;

	make_api89_command_table(bytes);
	assert(intel_ax211_protocol_command_table_parse(bytes,
	    INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES, &table) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(table.count == INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT);
	assert(intel_ax211_protocol_command_table_validate_api89(&table) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_protocol_command_version_lookup(&table, 0x0cU,
	    0x02U, &version) == INTEL_AX211_PROTOCOL_OK);
	assert(version.command_version == 1U);
	assert(version.notification_version == 4U);
	assert(intel_ax211_protocol_command_version_lookup(&table, 0x0bU,
	    0xaaU, &version) == INTEL_AX211_PROTOCOL_MISSING);

	assert(intel_ax211_protocol_command_table_parse(NULL, 4U, &table) ==
	    INTEL_AX211_PROTOCOL_INVALID);
	assert(intel_ax211_protocol_command_table_parse(bytes, 0U, &table) ==
	    INTEL_AX211_PROTOCOL_TRUNCATED);
	assert(intel_ax211_protocol_command_table_parse(bytes, 3U, &table) ==
	    INTEL_AX211_PROTOCOL_TRUNCATED);
	assert(intel_ax211_protocol_command_table_parse(bytes,
	    sizeof(bytes), &table) == INTEL_AX211_PROTOCOL_OVERSIZED);

	memcpy(malformed, bytes, sizeof(malformed));
	put_command_version(malformed, 5U, 0x0cU, 0xfeU, 99U, 2U);
	assert(intel_ax211_protocol_command_table_parse(malformed,
	    sizeof(malformed), &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_protocol_command_table_validate_api89(&table) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);

	memcpy(malformed, bytes, sizeof(malformed));
	put_command_version(malformed, 6U, 0x0cU, 0x02U, 1U, 4U);
	assert(intel_ax211_protocol_command_table_parse(malformed,
	    sizeof(malformed), &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_protocol_command_version_lookup(&table, 0x0cU,
	    0x02U, &version) == INTEL_AX211_PROTOCOL_DUPLICATE);
	assert(intel_ax211_protocol_command_table_validate_api89(&table) ==
	    INTEL_AX211_PROTOCOL_DUPLICATE);

	memcpy(malformed, bytes, sizeof(malformed));
	put_command_version(malformed,
	    INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U, 0U, 1U, 0U, 0U);
	assert(intel_ax211_protocol_command_table_parse(malformed,
	    sizeof(malformed), &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_protocol_command_table_validate_api89(&table) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);

	assert(intel_ax211_protocol_command_table_parse(bytes,
	    INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES - 4U, &table) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_protocol_command_table_validate_api89(&table) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
}

static struct intel_ax211_protocol_message
make_message(uint8_t *payload, size_t payload_length, uint8_t group,
	uint8_t opcode, uint8_t version, uint32_t generation)
{
	struct intel_ax211_protocol_message message;

	memset(&message, 0, sizeof(message));
	message.group = group;
	message.opcode = opcode;
	message.version = version;
	message.queue = 4U;
	message.index = 17U;
	message.generation = generation;
	message.payload = payload;
	message.payload_length = payload_length;
	return message;
}

static void
make_alive(uint8_t payload[INTEL_AX211_PROTOCOL_ALIVE_SIZE])
{
	memset(payload, 0, INTEL_AX211_PROTOCOL_ALIVE_SIZE);
	put_le16(payload, INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK);
	put_le16(payload + 2U, 0x1234U);
	put_le32(payload + 4U, 89U);
	put_le32(payload + 8U, 0x735b75a4U);
	payload[12] = 1U;
	payload[13] = 2U;
	payload[14] = 3U;
	payload[15] = 4U;
	put_le32(payload + 16U, 0x11223344U);
	put_le32(payload + 20U, 0x2000U);
	put_le32(payload + 24U, 0x3000U);
	put_le32(payload + 28U, 0x4000U);
	put_le32(payload + 32U, 0x5000U);
	put_le32(payload + 36U, 0x6000U);
	put_le32(payload + 40U, 0x7000U);
	put_le32(payload + 44U, 0x8000U);
	put_le32(payload + 48U, 0x9000U);
	put_le32(payload + 52U, 90U);
	put_le32(payload + 56U, 0x01020304U);
	put_le32(payload + 100U, 7U);
	put_le32(payload + 104U, 8U);
	put_le32(payload + 108U, 0xa000U);
	put_le32(payload + 112U, 0xb000U);
	put_le32(payload + 116U, 0x11111111U);
	put_le32(payload + 120U, 0x22222222U);
	put_le32(payload + 124U, 0x33333333U);
}

static void
test_alive(void)
{
	uint8_t payload[INTEL_AX211_PROTOCOL_ALIVE_SIZE + 1U];
	uint8_t original[INTEL_AX211_PROTOCOL_ALIVE_SIZE + 1U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_protocol_alive alive;

	make_alive(payload);
	memcpy(original, payload, sizeof(original));
	message = make_message(payload, INTEL_AX211_PROTOCOL_ALIVE_SIZE,
	    INTEL_AX211_PROTOCOL_GROUP_LEGACY,
	    INTEL_AX211_PROTOCOL_ALIVE_OPCODE,
	    INTEL_AX211_PROTOCOL_ALIVE_VERSION, 9U);
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(alive.status == INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK);
	assert(alive.flags == 0x1234U);
	assert(alive.lmac[0].major == 89U);
	assert(alive.lmac[0].minor == 0x735b75a4U);
	assert(alive.lmac[0].timestamp == 0x11223344U);
	assert(alive.lmac[0].store_forward_size == 0x9000U);
	assert(alive.lmac[1].major == 90U);
	assert(alive.umac.major == 7U);
	assert(alive.umac.error_info == 0xa000U);
	assert(alive.sku[2] == 0x33333333U);
	assert(alive.imr_enabled == 0U);

	message.payload_length--;
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_TRUNCATED);
	message.payload_length += 2U;
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_OVERSIZED);
	message.payload_length--;
	message.version--;
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
	message.version++;
	assert(intel_ax211_protocol_alive_decode(&message, 8U, &alive) ==
	    INTEL_AX211_PROTOCOL_STALE);
	message.flags = INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_FAILED);
	message.flags = 0U;

	put_le16(payload, INTEL_AX211_PROTOCOL_ALIVE_STATUS_ERROR);
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_FAILED);
	memcpy(payload, original, sizeof(original));
	memset(payload + 116U, 0, 12U);
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_MISSING);
	memcpy(payload, original, sizeof(original));
	put_le32(payload + 140U, 1U);
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_INVALID);
	put_le64(payload + 128U, 0x100000U);
	put_le32(payload + 136U, 0x2000U);
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
	put_le32(payload + 140U, 0U);
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_INVALID);
	message.payload = NULL;
	assert(intel_ax211_protocol_alive_decode(&message, 9U, &alive) ==
	    INTEL_AX211_PROTOCOL_INVALID);
}

static void
test_completions_and_response(void)
{
	uint8_t byte = 0U;
	uint8_t payload[INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_SIZE] = { 0U };
	struct intel_ax211_protocol_message message;
	struct intel_ax211_protocol_pending_command pending;

	message = make_message(payload, sizeof(payload),
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_OPCODE,
	    INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_VERSION, 42U);
	assert(intel_ax211_protocol_pnvm_init_complete(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_OK);
	message.version = 2U;
	assert(intel_ax211_protocol_pnvm_init_complete(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
	message.version = INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_VERSION;
	message.payload = payload;
	message.payload_length = 1U;
	assert(intel_ax211_protocol_pnvm_init_complete(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_TRUNCATED);
	message.payload_length = sizeof(payload) + 1U;
	assert(intel_ax211_protocol_pnvm_init_complete(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_OVERSIZED);

	message = make_message(payload, INTEL_AX211_PROTOCOL_INIT_COMPLETE_SIZE,
	    INTEL_AX211_PROTOCOL_GROUP_LEGACY,
	    INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE,
	    INTEL_AX211_PROTOCOL_UNKNOWN_VERSION, 43U);
	assert(intel_ax211_protocol_init_complete(&message, 43U) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_protocol_init_complete(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_STALE);
	message.generation = 43U;
	message.payload_length--;
	assert(intel_ax211_protocol_init_complete(&message, 43U) ==
	    INTEL_AX211_PROTOCOL_TRUNCATED);
	message.payload_length = INTEL_AX211_PROTOCOL_INIT_COMPLETE_SIZE + 1U;
	assert(intel_ax211_protocol_init_complete(&message, 43U) ==
	    INTEL_AX211_PROTOCOL_OVERSIZED);
	message.payload_length = INTEL_AX211_PROTOCOL_INIT_COMPLETE_SIZE;
	message.opcode++;
	assert(intel_ax211_protocol_init_complete(&message, 43U) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);

	memset(&pending, 0, sizeof(pending));
	pending.group = 0x0cU;
	pending.opcode = 0x02U;
	pending.response_version = 4U;
	pending.queue = 4U;
	pending.index = 17U;
	pending.generation = 51U;
	pending.minimum_response_length = 1U;
	pending.maximum_response_length = 1U;
	message = make_message(&byte, 1U, 0x0cU, 0x02U, 4U, 51U);
	assert(intel_ax211_protocol_command_response_validate(&message,
	    &pending) == INTEL_AX211_PROTOCOL_OK);
	message.index++;
	assert(intel_ax211_protocol_command_response_validate(&message,
	    &pending) == INTEL_AX211_PROTOCOL_TOKEN_MISMATCH);
	message.index--;
	message.flags = INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	assert(intel_ax211_protocol_command_response_validate(&message,
	    &pending) == INTEL_AX211_PROTOCOL_FAILED);
	message.flags = 0U;
	message.generation++;
	assert(intel_ax211_protocol_command_response_validate(&message,
	    &pending) == INTEL_AX211_PROTOCOL_STALE);
	message.generation--;
	message.version++;
	assert(intel_ax211_protocol_command_response_validate(&message,
	    &pending) == INTEL_AX211_PROTOCOL_UNSUPPORTED);
	message.version--;
	message.payload_length = 0U;
	assert(intel_ax211_protocol_command_response_validate(&message,
	    &pending) == INTEL_AX211_PROTOCOL_TRUNCATED);
	message.payload_length = 2U;
	assert(intel_ax211_protocol_command_response_validate(&message,
	    &pending) == INTEL_AX211_PROTOCOL_OVERSIZED);
	pending.response_version = INTEL_AX211_PROTOCOL_UNKNOWN_VERSION;
	assert(intel_ax211_protocol_command_response_validate(&message,
	    &pending) == INTEL_AX211_PROTOCOL_UNSUPPORTED);
}

static void
make_nvm(uint8_t payload[INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE])
{
	size_t index;

	memset(payload, 0, INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE);
	put_le16(payload + 4U, 0x1234U);
	payload[6] = 3U;
	payload[7] = 8U;
	put_le32(payload + 8U,
	    INTEL_AX211_PROTOCOL_NVM_BAND_24_ENABLED |
	    INTEL_AX211_PROTOCOL_NVM_BAND_52_ENABLED |
	    INTEL_AX211_PROTOCOL_NVM_11N_ENABLED |
	    INTEL_AX211_PROTOCOL_NVM_11AC_ENABLED |
	    INTEL_AX211_PROTOCOL_NVM_11AX_ENABLED);
	put_le32(payload + 12U, 3U);
	put_le32(payload + 16U, 3U);
	put_le32(payload + 20U, 1U);
	put_le32(payload + 24U, INTEL_AX211_PROTOCOL_NVM_CHANNEL_LIMIT);
	for (index = 0; index < INTEL_AX211_PROTOCOL_NVM_CHANNEL_LIMIT;
	    index++) {
		uint32_t flags = 0U;

		if (index < INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT ||
		    index == 14U || index == 17U)
			flags = INTEL_AX211_PROTOCOL_NVM_CHANNEL_VALID;
		if (index == 0U || index == 14U)
			flags |= INTEL_AX211_PROTOCOL_NVM_CHANNEL_ACTIVE;
		put_le32(payload + 28U + index * sizeof(uint32_t), flags);
	}
}

static void
test_nvm(void)
{
	uint8_t payload[INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE + 1U];
	uint8_t original[INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE + 1U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_protocol_pending_command pending;
	struct intel_ax211_protocol_nvm nvm;

	make_nvm(payload);
	memcpy(original, payload, sizeof(original));
	message = make_message(payload,
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE,
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_OPCODE,
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_VERSION, 61U);
	memset(&pending, 0, sizeof(pending));
	pending.group = message.group;
	pending.opcode = message.opcode;
	pending.response_version = message.version;
	pending.queue = message.queue;
	pending.index = message.index;
	pending.generation = message.generation;
	pending.minimum_response_length =
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE;
	pending.maximum_response_length =
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE;
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_OK);
	assert(nvm.nvm_version == 0x1234U);
	assert(nvm.board_type == 3U);
	assert(nvm.hardware_address_count == 8U);
	assert(nvm.band_24_enabled);
	assert(nvm.band_52_enabled);
	assert(nvm.ht_enabled);
	assert(nvm.vht_enabled);
	assert(nvm.he_enabled);
	assert(!nvm.mimo_disabled);
	assert(nvm.tx_chain_mask == 3U);
	assert(nvm.rx_chain_mask == 3U);
	assert(nvm.lar_enabled);
	assert(nvm.n_channels == INTEL_AX211_PROTOCOL_NVM_CHANNEL_LIMIT);
	assert(nvm.channel_24ghz_count == 14U);
	assert(nvm.valid_24ghz_count == 14U);
	assert(nvm.channel_24ghz[0].number == 1U);
	assert(nvm.channel_24ghz[0].active);
	assert(nvm.channel_24ghz[1].number == 2U);
	assert(!nvm.channel_24ghz[1].active);
	assert(nvm.channel_5ghz_count ==
	    INTEL_AX211_PROTOCOL_5GHZ_CHANNEL_LIMIT);
	assert(nvm.valid_5ghz_count == 2U);
	assert(nvm.channel_5ghz[0].number == 36U);
	assert(nvm.channel_5ghz[0].valid);
	assert(nvm.channel_5ghz[0].active);
	assert(nvm.channel_5ghz[3].number == 48U);
	assert(nvm.channel_5ghz[3].valid);
	assert(!nvm.channel_5ghz[3].active);
	assert(nvm.channel_5ghz[28].number == 149U);
	assert(nvm.channel_5ghz[36].number == 181U);

	message.payload_length--;
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_TRUNCATED);
	message.payload_length += 2U;
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_OVERSIZED);
	message.payload_length--;
	message.generation++;
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_STALE);
	message.generation--;
	message.flags = INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_FAILED);
	message.flags = 0U;

	memcpy(payload, original, sizeof(original));
	put_le32(payload, INTEL_AX211_PROTOCOL_NVM_GENERAL_EMPTY_OTP);
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_FAILED);
	memcpy(payload, original, sizeof(original));
	put_le32(payload + 8U, 0U);
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_MISSING);
	memcpy(payload, original, sizeof(original));
	put_le32(payload + 8U,
	    INTEL_AX211_PROTOCOL_NVM_BAND_24_ENABLED |
	    INTEL_AX211_PROTOCOL_NVM_11N_ENABLED);
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_OK);
	assert(nvm.channel_5ghz_count ==
	    INTEL_AX211_PROTOCOL_5GHZ_CHANNEL_LIMIT);
	assert(nvm.valid_5ghz_count == 0U);
	memcpy(payload, original, sizeof(original));
	put_le32(payload + 12U, 0U);
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_MISSING);
	memcpy(payload, original, sizeof(original));
	put_le32(payload + 16U, 0x100U);
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_UNSUPPORTED);
	memcpy(payload, original, sizeof(original));
	put_le32(payload + 24U,
	    INTEL_AX211_PROTOCOL_NVM_CHANNEL_LIMIT + 1U);
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_OVERSIZED);
	memcpy(payload, original, sizeof(original));
	memset(payload + 28U, 0, 14U * sizeof(uint32_t));
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_OK);
	assert(nvm.lar_enabled && nvm.valid_24ghz_count == 0U);
	put_le32(payload + 20U, 0U);
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_MISSING);

	memcpy(payload, original, sizeof(original));
	pending.response_version = 3U;
	assert(intel_ax211_protocol_nvm_get_info_decode(&message, &pending,
	    &nvm) == INTEL_AX211_PROTOCOL_UNSUPPORTED);
}

static uint8_t *
read_file(const char *path, size_t *length)
{
	FILE *file;
	long end;
	uint8_t *bytes;

	file = fopen(path, "rb");
	assert(file != NULL);
	assert(fseek(file, 0L, SEEK_END) == 0);
	end = ftell(file);
	assert(end > 0L);
	assert(fseek(file, 0L, SEEK_SET) == 0);
	bytes = malloc((size_t)end);
	assert(bytes != NULL);
	assert(fread(bytes, 1U, (size_t)end, file) == (size_t)end);
	assert(fclose(file) == 0);
	*length = (size_t)end;
	return bytes;
}

static void
test_real_firmware_command_table(const char *path)
{
	struct intel_ax211_protocol_command_table table;
	uint8_t *firmware;
	size_t length, offset;
	int found = 0;

	firmware = read_file(path, &length);
	assert(length == 1736748U);
	assert(length >= 88U);
	offset = 88U;
	while (offset < length) {
		uint32_t type, tlv_length;
		size_t padded;

		assert(length - offset >= 8U);
		type = get_le32(firmware + offset);
		tlv_length = get_le32(firmware + offset + 4U);
		offset += 8U;
		padded = (size_t)tlv_length;
		if ((padded & 3U) != 0U) {
			size_t padding = 4U - (padded & 3U);

			assert(padded <= SIZE_MAX - padding);
			padded += padding;
		}
		assert(padded <= length - offset);
		if (type == 48U) {
			assert(!found);
			assert(intel_ax211_protocol_command_table_parse(
			    firmware + offset, tlv_length, &table) ==
			    INTEL_AX211_PROTOCOL_OK);
			assert(intel_ax211_protocol_command_table_validate_api89(
			    &table) == INTEL_AX211_PROTOCOL_OK);
			found = 1;
		}
		offset += padded;
	}
	assert(found);
	free(firmware);
}

int
main(int argc, char **argv)
{
	assert(argc == 1 || argc == 2);
	test_command_versions();
	test_alive();
	test_completions_and_response();
	test_nvm();
	if (argc == 2)
		test_real_firmware_command_table(argv[1]);
	puts("intel ax211 protocol tests passed");
	return 0;
}
