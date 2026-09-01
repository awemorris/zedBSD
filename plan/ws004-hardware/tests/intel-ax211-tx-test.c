/* Intel AX211 private API89 TX codec fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-tx.h"

static uint16_t
get_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0U] | ((uint16_t)bytes[1U] << 8);
}

static uint32_t
get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0U] | ((uint32_t)bytes[1U] << 8) |
	    ((uint32_t)bytes[2U] << 16) | ((uint32_t)bytes[3U] << 24);
}

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
}

static void
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
	bytes[2U] = (uint8_t)(value >> 16);
	bytes[3U] = (uint8_t)(value >> 24);
}

static void
test_api89_version(void)
{
	uint8_t bytes[8U] = {
		INTEL_AX211_TX_OPCODE, INTEL_AX211_TX_GROUP,
		INTEL_AX211_TX_COMMAND_VERSION,
		INTEL_AX211_TX_NOTIFICATION_VERSION,
		0U, 0U, 0U, 0U
	};
	struct intel_ax211_protocol_command_table table;

	assert(intel_ax211_protocol_command_table_parse(bytes, sizeof(bytes),
	    &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_tx_api89_validate(&table) == INTEL_AX211_TX_OK);
	bytes[2U]--;
	assert(intel_ax211_protocol_command_table_parse(bytes, sizeof(bytes),
	    &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_tx_api89_validate(&table) ==
	    INTEL_AX211_TX_UNSUPPORTED);
	assert(intel_ax211_tx_api89_validate(NULL) == INTEL_AX211_TX_INVALID);
}

static void
test_clear_management(void)
{
	uint8_t frame[32U];
	struct intel_ax211_tx_request request;
	struct intel_ax211_tx_prepared prepared;

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0xb0U;
	frame[24U] = 0x5aU;
	memset(&request, 0, sizeof(request));
	request.connection_generation = 7U;
	request.cookie = 9U;
	request.frame = frame;
	request.length = sizeof(frame);
	request.frame_class = INTEL_AX211_TX_FRAME_MANAGEMENT;
	assert(intel_ax211_tx_prepare(&request, &prepared) ==
	    INTEL_AX211_TX_OK);
	assert(prepared.command_length == 52U);
	assert(prepared.payload_offset == 24U && prepared.payload_length == 8U);
	assert(prepared.frame_length == sizeof(frame));
	assert(get_le16(prepared.command) == sizeof(frame));
	assert(get_le16(prepared.command + 2U) == 7U);
	assert(get_le32(prepared.command + 4U) == 0x00000c00U);
	assert(get_le32(prepared.command + 16U) == 0x00004000U);
	assert(memcmp(prepared.command + INTEL_AX211_TX_COMMAND_FIXED_SIZE,
	    frame, 24U) == 0);
	assert(prepared.connection_generation == 7U && prepared.cookie == 9U);
}

static void
test_encrypted_data(void)
{
	uint8_t frame[48U];
	struct intel_ax211_tx_request request;
	struct intel_ax211_tx_prepared prepared;

	memset(frame, 0, sizeof(frame));
	put_le16(frame, 0x4108U);
	frame[24U] = 1U;
	frame[25U] = 2U;
	frame[27U] = 0xa0U;
	frame[28U] = 3U;
	frame[29U] = 4U;
	frame[30U] = 5U;
	frame[31U] = 6U;
	memset(&request, 0, sizeof(request));
	request.connection_generation = 11U;
	request.cookie = 12U;
	request.key_generation = 13U;
	request.packet_number = UINT64_C(0x060504030201);
	request.frame = frame;
	request.length = sizeof(frame);
	request.frame_class = INTEL_AX211_TX_FRAME_DATA;
	request.encrypted = 1U;
	request.key_index = 2U;
	assert(intel_ax211_tx_prepare(&request, &prepared) ==
	    INTEL_AX211_TX_OK);
	assert(get_le16(prepared.command + 2U) == 5U);
	assert(get_le16(prepared.command) == sizeof(frame) - 8U);
	assert(get_le32(prepared.command + 4U) == 0x00000c00U);
	assert(get_le32(prepared.command + 16U) == 0x00004000U);
	assert(prepared.payload_offset == 32U && prepared.payload_length == 16U);
	assert(prepared.frame_length == sizeof(frame) - 8U);
	assert(prepared.command[INTEL_AX211_TX_COMMAND_FIXED_SIZE + 23U] ==
	    frame[23U]);
	assert(prepared.command[INTEL_AX211_TX_COMMAND_FIXED_SIZE + 24U] == 0U);
	assert(prepared.encrypted == 1U && prepared.key_index == 2U);
	assert(prepared.key_generation == 13U);
	assert(prepared.packet_number == request.packet_number);

	frame[31U]++;
	assert(intel_ax211_tx_prepare(&request, &prepared) ==
	    INTEL_AX211_TX_STALE);
	frame[31U]--;
	frame[27U] = 0x60U;
	assert(intel_ax211_tx_prepare(&request, &prepared) ==
	    INTEL_AX211_TX_INVALID);
}

static void
test_qos_padding(void)
{
	uint8_t frame[40U];
	struct intel_ax211_tx_request request;
	struct intel_ax211_tx_prepared prepared;

	memset(frame, 0, sizeof(frame));
	put_le16(frame, 0x0188U);
	memset(&request, 0, sizeof(request));
	request.connection_generation = 1U;
	request.cookie = 1U;
	request.frame = frame;
	request.length = sizeof(frame);
	request.frame_class = INTEL_AX211_TX_FRAME_DATA;
	assert(intel_ax211_tx_prepare(&request, &prepared) ==
	    INTEL_AX211_TX_OK);
	assert(prepared.command_length == 56U);
	assert(prepared.payload_offset == 26U && prepared.payload_length == 14U);
	assert(get_le16(prepared.command + 2U) == 7U);
	assert(get_le32(prepared.command + 4U) == 0x00002d00U);
	assert(get_le32(prepared.command + 16U) == 0x00004000U);
	assert(prepared.command[54U] == 0U && prepared.command[55U] == 0U);
}

static struct intel_ax211_protocol_message
make_completion(uint8_t payload[48U])
{
	struct intel_ax211_protocol_message message;

	memset(payload, 0, 48U);
	payload[0U] = 1U;
	payload[2U] = 2U;
	payload[3U] = 3U;
	put_le16(payload + 28U, 0x1230U);
	put_le16(payload + 30U, 48U);
	put_le16(payload + 36U, 1U);
	put_le32(payload + 40U, 1U);
	put_le32(payload + 44U, 8U);
	memset(&message, 0, sizeof(message));
	message.group = INTEL_AX211_TX_GROUP;
	message.opcode = INTEL_AX211_TX_OPCODE;
	message.version = INTEL_AX211_TX_NOTIFICATION_VERSION;
	message.queue = 1U;
	message.index = 7U;
	message.generation = 19U;
	message.payload = payload;
	message.payload_length = 48U;
	return message;
}

static void
test_completion(void)
{
	uint8_t payload[49U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_tx_completion completion;

	message = make_completion(payload);
	assert(intel_ax211_tx_completion_decode(&message, 19U, 1U, 7U,
	    &completion) == INTEL_AX211_TX_OK);
	assert(completion.hardware_generation == 19U);
	assert(completion.scheduler_sequence == 8U);
	assert(completion.queue == 1U && completion.index == 7U);
	assert(completion.sequence_control == 0x1230U);
	assert(completion.byte_count == 48U && completion.acknowledged == 1U);
	assert(completion.failure_rts == 2U && completion.failure_frame == 3U);

	put_le32(payload + 40U, 0x83U);
	assert(intel_ax211_tx_completion_decode(&message, 19U, 1U, 7U,
	    &completion) == INTEL_AX211_TX_OK);
	assert(completion.acknowledged == 0U);
	assert(intel_ax211_tx_completion_decode(&message, 18U, 1U, 7U,
	    &completion) == INTEL_AX211_TX_STALE);
	assert(intel_ax211_tx_completion_decode(&message, 19U, 1U, 6U,
	    &completion) == INTEL_AX211_TX_STALE);
	message.payload_length = 47U;
	assert(intel_ax211_tx_completion_decode(&message, 19U, 1U, 7U,
	    &completion) == INTEL_AX211_TX_TRUNCATED);
	message.payload_length = 49U;
	assert(intel_ax211_tx_completion_decode(&message, 19U, 1U, 7U,
	    &completion) == INTEL_AX211_TX_OVERSIZED);
	message.payload_length = 48U;
	put_le32(payload + 44U, 65536U);
	assert(intel_ax211_tx_completion_decode(&message, 19U, 1U, 7U,
	    &completion) == INTEL_AX211_TX_FAILED);
}

static void
test_rejections(void)
{
	uint8_t frame[24U];
	struct intel_ax211_tx_request request;
	struct intel_ax211_tx_prepared prepared;

	memset(frame, 0, sizeof(frame));
	memset(&request, 0, sizeof(request));
	request.connection_generation = 1U;
	request.cookie = 1U;
	request.frame = frame;
	request.length = sizeof(frame);
	request.frame_class = INTEL_AX211_TX_FRAME_MANAGEMENT;
	assert(intel_ax211_tx_prepare(&request, &prepared) ==
	    INTEL_AX211_TX_OK);
	request.connection_generation = 0U;
	assert(intel_ax211_tx_prepare(&request, &prepared) ==
	    INTEL_AX211_TX_INVALID);
	request.connection_generation = 1U;
	request.length = 23U;
	assert(intel_ax211_tx_prepare(&request, &prepared) ==
	    INTEL_AX211_TX_TRUNCATED);
	request.length = sizeof(frame);
	request.encrypted = 1U;
	request.key_generation = 1U;
	request.packet_number = 1U;
	assert(intel_ax211_tx_prepare(&request, &prepared) ==
	    INTEL_AX211_TX_UNSUPPORTED);
}

int
main(void)
{
	test_api89_version();
	test_clear_management();
	test_encrypted_data();
	test_qos_padding();
	test_completion();
	test_rejections();
	puts("intel ax211 TX codec tests passed");
	return 0;
}
