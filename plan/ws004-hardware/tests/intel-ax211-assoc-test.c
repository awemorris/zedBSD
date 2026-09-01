/*
 * Intel AX211 API89 association-session fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-assoc.h"

static uint16_t
get_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

static uint32_t
get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t
get_le64(const uint8_t *bytes)
{
	return (uint64_t)get_le32(bytes) |
	    ((uint64_t)get_le32(bytes + 4U) << 32);
}

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
	put_le32(bytes, (uint32_t)value);
	put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

static void
put_version(uint8_t *bytes, size_t index, uint8_t group, uint8_t opcode,
	uint8_t command_version, uint8_t notification_version)
{
	uint8_t *entry;

	entry = bytes + index *
	    INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE;
	entry[0] = opcode;
	entry[1] = group;
	entry[2] = command_version;
	entry[3] = notification_version;
}

static void
make_api89_table(uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES])
{
	size_t index;

	for (index = 0U;
	    index < INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U; index++)
		put_version(bytes, index, 0x10U, (uint8_t)index, 1U, 1U);
	put_version(bytes, 0U, 0x00U, 0x01U, 99U, 6U);
	put_version(bytes, 1U, 0x01U, 0x0cU, 5U, 0U);
	put_version(bytes, 2U, 0x01U, 0x0dU, 17U, 0U);
	put_version(bytes, 3U, 0x0cU, 0x00U, 1U, 0U);
	put_version(bytes, 4U, 0x0cU, 0x02U, 1U, 4U);
	put_version(bytes, 5U, 0x0cU, 0xfeU, 99U, 1U);
	put_version(bytes, 6U, 0x01U, 0x08U, 4U, 0U);
	put_version(bytes, 7U, 0x05U, 0x08U, 2U, 0U);
	put_version(bytes, 8U, 0x05U, 0x17U, 3U, 2U);
	put_version(bytes, 9U, 0x03U, 0x05U, 2U, 0U);
	/* API89 advertises notification v3 for the OpenBSD v2 wire layout. */
	put_version(bytes, 10U, 0x03U, 0xfbU, 99U, 3U);
	/* Logical legacy group 0 commands are listed under LONG_GROUP. */
	put_version(bytes, 11U, 0x01U, 0xd0U, 1U, 0U);
	put_version(bytes, 12U, 0x01U, 0xa9U, 1U, 0U);
	put_version(bytes,
	    INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U,
	    0U, 0U, 0U, 0U);
}

static struct intel_ax211_protocol_command_table
parse_table(uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES])
{
	struct intel_ax211_protocol_command_table table;

	assert(intel_ax211_protocol_command_table_parse(bytes,
	    INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES, &table) ==
	    INTEL_AX211_PROTOCOL_OK);
	return table;
}

static struct intel_ax211_assoc_profile
make_profile(void)
{
	static const uint8_t station[6] = {
		0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U
	};
	static const uint8_t bssid[6] = {
		0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U
	};
	struct intel_ax211_assoc_profile profile;

	memset(&profile, 0, sizeof(profile));
	memcpy(profile.station_address, station, sizeof(station));
	memcpy(profile.bssid, bssid, sizeof(bssid));
	profile.channel = 6U;
	profile.channel_width_mhz = INTEL_AX211_ASSOC_CHANNEL_WIDTH_MHZ;
	profile.rx_chain_mask = 3U;
	profile.cck_ack_rates = 0x0fU;
	profile.ofdm_ack_rates = 0x15U;
	profile.short_preamble = 1U;
	profile.short_slot = 1U;
	profile.qos = 1U;
	profile.beacon_interval_tu = 100U;
	profile.queue_initial_write_pointer = 0x345U;
	profile.queue_byte_count_address = UINT64_C(0x1122334455667780);
	profile.queue_descriptor_address = UINT64_C(0x8877665544332200);
	profile.edca[0].ecw_min = 4U;
	profile.edca[0].ecw_max = 10U;
	profile.edca[0].aifsn = 3U;
	profile.edca[1].ecw_min = 4U;
	profile.edca[1].ecw_max = 10U;
	profile.edca[1].aifsn = 7U;
	profile.edca[2].ecw_min = 3U;
	profile.edca[2].ecw_max = 4U;
	profile.edca[2].aifsn = 2U;
	profile.edca[2].txop_32us = 3U;
	profile.edca[3].ecw_min = 2U;
	profile.edca[3].ecw_max = 3U;
	profile.edca[3].aifsn = 2U;
	profile.edca[3].txop_32us = 47U;
	return profile;
}

static struct intel_ax211_assoc_update
make_update(void)
{
	struct intel_ax211_assoc_update update;

	memset(&update, 0, sizeof(update));
	update.association_id = 0x123U;
	update.dtim_period = 3U;
	update.dtim_count = 2U;
	update.beacon_arrive_time = UINT32_C(0x12345678);
	update.beacon_tsf = UINT64_C(0x0102030405060708);
	return update;
}

static void
assert_zero(const uint8_t *bytes, size_t length)
{
	size_t index;

	for (index = 0U; index < length; index++)
		assert(bytes[index] == 0U);
}

static struct intel_ax211_assoc_reply
make_reply(const struct intel_ax211_assoc_command *command)
{
	struct intel_ax211_assoc_reply reply;

	memset(&reply, 0, sizeof(reply));
	reply.step = command->step;
	reply.response_version = command->response_version;
	reply.sequence = command->sequence;
	reply.common_generation = command->common_generation;
	reply.hardware_epoch = command->hardware_epoch;
	switch (command->response_kind) {
	case INTEL_AX211_ASSOC_RESPONSE_EMPTY:
		break;
	case INTEL_AX211_ASSOC_RESPONSE_STATUS_ZERO:
		reply.payload_length = 4U;
		break;
	case INTEL_AX211_ASSOC_RESPONSE_STATION_SUCCESS:
		reply.payload[0U] = 1U;
		reply.payload_length = 4U;
		break;
	case INTEL_AX211_ASSOC_RESPONSE_QUEUE:
		put_le16(reply.payload, 1U);
		put_le16(reply.payload + 4U,
		    command->expected_queue_write_pointer);
		reply.payload_length = 8U;
		break;
	default:
		assert(0);
	}
	return reply;
}

static struct intel_ax211_protocol_message
make_session_event(uint8_t payload[16U], uint32_t hardware_epoch,
	uint32_t mac_id, uint32_t status, uint32_t start,
	uint32_t configuration_id)
{
	struct intel_ax211_protocol_message message;

	memset(payload, 0, 16U);
	put_le32(payload, mac_id);
	put_le32(payload + 4U, status);
	put_le32(payload + 8U, start);
	put_le32(payload + 12U, configuration_id);
	memset(&message, 0, sizeof(message));
	message.opcode = INTEL_AX211_ASSOC_SESSION_NOTIFICATION_OPCODE;
	message.group = INTEL_AX211_ASSOC_GROUP_MAC_CONFIG;
	message.version =
	    INTEL_AX211_ASSOC_SESSION_NOTIFICATION_LAYOUT_VERSION;
	message.queue = 0x80U;
	message.generation = hardware_epoch;
	message.payload = payload;
	message.payload_length = 16U;
	return message;
}

static int
accept_current(struct intel_ax211_assoc_state *state, uint64_t now,
	struct intel_ax211_assoc_command *saved)
{
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_reply reply;

	assert(intel_ax211_assoc_current(state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	if (saved != NULL)
		*saved = command;
	reply = make_reply(&command);
	return intel_ax211_assoc_accept(state, &command, &reply, now + 1U);
}

static void
test_api89_versions(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t malformed[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;

	make_api89_table(bytes);
	table = parse_table(bytes);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_OK);
	memcpy(malformed, bytes, sizeof(malformed));
	put_version(malformed, 6U, 0x01U, 0x08U, 3U, 0U);
	table = parse_table(malformed);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
	memcpy(malformed, bytes, sizeof(malformed));
	put_version(malformed, 8U, 0x05U, 0x17U, 3U, 1U);
	table = parse_table(malformed);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
	memcpy(malformed, bytes, sizeof(malformed));
	put_version(malformed, 10U, 0x03U, 0xfbU, 99U, 2U);
	table = parse_table(malformed);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
	memcpy(malformed, bytes, sizeof(malformed));
	put_version(malformed, 11U, 0x01U, 0xd0U, 2U, 0U);
	table = parse_table(malformed);
	assert(intel_ax211_assoc_mcast_filter_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
	memcpy(malformed, bytes, sizeof(malformed));
	put_version(malformed, 12U, 0x01U, 0xa9U, 1U, 1U);
	table = parse_table(malformed);
	assert(intel_ax211_assoc_mac_power_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
}

static void
test_mcast_filter_codec(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t output[16U];
	uint8_t expected[INTEL_AX211_ASSOC_MCAST_FILTER_SIZE];
	uint8_t invalid[6U];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	assert(intel_ax211_assoc_mcast_filter_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_OK);
	memset(output, 0xa5, sizeof(output));
	memset(expected, 0, sizeof(expected));
	expected[0U] = 1U;
	expected[3U] = 1U;
	memcpy(expected + 4U, profile.bssid, 6U);
	assert(intel_ax211_assoc_mcast_filter_encode(profile.bssid, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_OK);
	assert(memcmp(output, expected, sizeof(expected)) == 0);
	assert(output[12U] == 0xa5U && output[15U] == 0xa5U);

	memset(output, 0xa5, sizeof(output));
	assert(intel_ax211_assoc_mcast_filter_encode(profile.bssid, output,
	    INTEL_AX211_ASSOC_MCAST_FILTER_SIZE - 1U) ==
	    INTEL_AX211_ASSOC_BUFFER_TOO_SMALL);
	assert(output[0U] == 0xa5U && output[11U] == 0xa5U);
	memset(invalid, 0, sizeof(invalid));
	assert(intel_ax211_assoc_mcast_filter_encode(invalid, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_INVALID);
	invalid[0U] = 1U;
	assert(intel_ax211_assoc_mcast_filter_encode(invalid, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_INVALID);
	assert(intel_ax211_assoc_mcast_filter_encode(NULL, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_INVALID);
	assert(intel_ax211_assoc_mcast_filter_encode(profile.bssid, NULL,
	    sizeof(output)) == INTEL_AX211_ASSOC_INVALID);
}

static void
test_mac_power_codec(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t output[48U];
	uint8_t expected[INTEL_AX211_ASSOC_MAC_POWER_SIZE];
	uint8_t response[5U];
	struct intel_ax211_protocol_command_table table;

	make_api89_table(bytes);
	table = parse_table(bytes);
	assert(intel_ax211_assoc_mac_power_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_OK);
	memset(output, 0xa5, sizeof(output));
	memset(expected, 0, sizeof(expected));
	put_le16(expected + 6U, 25U);
	assert(intel_ax211_assoc_mac_power_encode(3U, 100U, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_OK);
	assert(memcmp(output, expected, sizeof(expected)) == 0);
	assert(output[40U] == 0xa5U && output[47U] == 0xa5U);

	/* A missing DTIM period is one; ceil(30,000ms / 1,000) is 30s. */
	memset(output, 0, sizeof(output));
	assert(intel_ax211_assoc_mac_power_encode(0U, 10000U, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_OK);
	assert(get_le16(output + 4U) == 0U);
	assert(get_le16(output + 6U) == 30U);
	assert_zero(output + 8U, INTEL_AX211_ASSOC_MAC_POWER_SIZE - 8U);

	/* 3 * 8,334ms is 25,002ms and therefore rounds up to 26 seconds. */
	assert(intel_ax211_assoc_mac_power_encode(1U, 8334U, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_OK);
	assert(get_le16(output + 6U) == 26U);
	assert(intel_ax211_assoc_mac_power_encode(1U, 21845000U, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_OK);
	assert(get_le16(output + 6U) == UINT16_MAX);
	assert(intel_ax211_assoc_mac_power_encode(1U, 21845001U, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_OVERSIZED);
	assert(intel_ax211_assoc_mac_power_encode(UINT8_MAX, UINT32_MAX,
	    output, sizeof(output)) == INTEL_AX211_ASSOC_OVERSIZED);

	memset(output, 0xa5, sizeof(output));
	assert(intel_ax211_assoc_mac_power_encode(1U, 100U, output,
	    INTEL_AX211_ASSOC_MAC_POWER_SIZE - 1U) ==
	    INTEL_AX211_ASSOC_BUFFER_TOO_SMALL);
	assert(output[0U] == 0xa5U && output[39U] == 0xa5U);
	assert(intel_ax211_assoc_mac_power_encode(1U, 0U, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_INVALID);
	assert(intel_ax211_assoc_mac_power_encode(1U, 100U, NULL,
	    sizeof(output)) == INTEL_AX211_ASSOC_INVALID);

	memset(response, 0, sizeof(response));
	assert(intel_ax211_assoc_mac_power_response_validate(response, 4U) ==
	    INTEL_AX211_ASSOC_OK);
	put_le32(response, 1U);
	assert(intel_ax211_assoc_mac_power_response_validate(response, 4U) ==
	    INTEL_AX211_ASSOC_FIRMWARE);
	assert(intel_ax211_assoc_mac_power_response_validate(response, 3U) ==
	    INTEL_AX211_ASSOC_TRUNCATED);
	assert(intel_ax211_assoc_mac_power_response_validate(response, 5U) ==
	    INTEL_AX211_ASSOC_OVERSIZED);
	assert(intel_ax211_assoc_mac_power_response_validate(NULL, 4U) ==
	    INTEL_AX211_ASSOC_INVALID);
}

static void
test_exact_auth_and_association_bytes(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_update update;
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_reply reply;
	struct intel_ax211_assoc_state state;
	uint8_t expected[INTEL_AX211_ASSOC_PAYLOAD_MAX];
	uint64_t dtim_offset;
	int result;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 41U, 7U,
	    100U) == INTEL_AX211_ASSOC_OK);

	assert(intel_ax211_assoc_current(&state, 100U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_PHY_ADD);
	assert(command.header == INTEL_AX211_ASSOC_HEADER_LEGACY);
	assert(command.group == 0U && command.opcode == 0x08U);
	assert(command.wire_version == 0U && command.layout_version == 4U);
	assert(command.payload_length == 32U);
	memset(expected, 0, sizeof(expected));
	put_le32(expected + 4U, 1U);
	put_le32(expected + 8U, 6U);
	expected[12U] = 1U;
	assert(memcmp(command.payload, expected, 32U) == 0);
	assert(get_le32(command.payload) == 0U);
	assert(get_le32(command.payload + 4U) == 1U);
	assert(get_le32(command.payload + 8U) == 6U);
	assert(command.payload[12U] == 1U);
	assert_zero(command.payload + 13U, 19U);
	reply = make_reply(&command);
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 101U) ==
	    INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, 101U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_RLC_CONFIG);
	assert(command.header == INTEL_AX211_ASSOC_HEADER_WIDE);
	assert(command.group == 5U && command.opcode == 0x08U);
	assert(command.wire_version == 2U && command.layout_version == 2U);
	assert(command.payload_length == 32U);
	memset(expected, 0, sizeof(expected));
	put_le32(expected + 4U, 0x1406U);
	assert(memcmp(command.payload, expected, 32U) == 0);
	assert(get_le32(command.payload + 4U) == 0x1406U);
	assert_zero(command.payload + 8U, 24U);
	assert(accept_current(&state, 101U, NULL) ==
	    INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, 102U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_MAC_ADD);
	assert(command.header == INTEL_AX211_ASSOC_HEADER_LEGACY);
	assert(command.group == 0U && command.opcode == 0x28U);
	assert(command.wire_version == 0U);
	assert(command.layout_version == 1U && command.payload_length == 148U);
	memset(expected, 0, sizeof(expected));
	put_le32(expected + 4U, 1U);
	put_le32(expected + 8U, 5U);
	memcpy(expected + 16U, profile.station_address, 6U);
	memcpy(expected + 24U, profile.bssid, 6U);
	put_le32(expected + 32U, 0x0fU);
	put_le32(expected + 36U, 0x15U);
	put_le32(expected + 44U, 0x20U);
	put_le32(expected + 48U, 0x10U);
	put_le32(expected + 52U, 0x44U);
	put_le32(expected + 56U, 1U);
	put_le16(expected + 68U, 15U);
	put_le16(expected + 70U, 1023U);
	expected[72U] = 7U;
	expected[73U] = 2U;
	put_le16(expected + 76U, 15U);
	put_le16(expected + 78U, 1023U);
	expected[80U] = 3U;
	expected[81U] = 4U;
	put_le16(expected + 84U, 7U);
	put_le16(expected + 86U, 15U);
	expected[88U] = 2U;
	expected[89U] = 8U;
	put_le16(expected + 90U, 96U);
	put_le16(expected + 92U, 3U);
	put_le16(expected + 94U, 7U);
	expected[96U] = 2U;
	expected[97U] = 16U;
	put_le16(expected + 98U, 1504U);
	put_le32(expected + 116U, 100U);
	put_le32(expected + 132U, 10U);
	assert(memcmp(command.payload, expected, 148U) == 0);
	assert(get_le32(command.payload + 4U) == 1U);
	assert(get_le32(command.payload + 8U) == 5U);
	assert(memcmp(command.payload + 16U, profile.station_address, 6U) == 0);
	assert(memcmp(command.payload + 24U, profile.bssid, 6U) == 0);
	assert(get_le32(command.payload + 32U) == 0x0fU);
	assert(get_le32(command.payload + 36U) == 0x15U);
	assert(get_le32(command.payload + 44U) == 0x20U);
	assert(get_le32(command.payload + 48U) == 0x10U);
	assert(get_le32(command.payload + 52U) == 0x44U);
	assert(get_le32(command.payload + 56U) == 1U);
	assert(get_le16(command.payload + 68U) == 15U);
	assert(get_le16(command.payload + 70U) == 1023U);
	assert(command.payload[72U] == 7U && command.payload[73U] == 2U);
	assert(get_le16(command.payload + 76U) == 15U);
	assert(get_le16(command.payload + 78U) == 1023U);
	assert(command.payload[80U] == 3U && command.payload[81U] == 4U);
	assert(get_le16(command.payload + 84U) == 7U);
	assert(get_le16(command.payload + 86U) == 15U);
	assert(get_le16(command.payload + 90U) == 96U);
	assert(get_le16(command.payload + 92U) == 3U);
	assert(get_le16(command.payload + 94U) == 7U);
	assert(get_le16(command.payload + 98U) == 1504U);
	assert(get_le32(command.payload + 100U) == 0U);
	assert(get_le32(command.payload + 116U) == 100U);
	assert(get_le32(command.payload + 132U) == 10U);
	assert(accept_current(&state, 102U, NULL) ==
	    INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, 103U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_BINDING_ADD);
	assert(command.header == INTEL_AX211_ASSOC_HEADER_LEGACY);
	assert(command.group == 0U);
	assert(command.wire_version == 0U);
	assert(command.opcode == 0x2bU && command.layout_version == 2U);
	assert(command.payload_length == 28U);
	memset(expected, 0, sizeof(expected));
	put_le32(expected + 4U, 1U);
	put_le32(expected + 12U, UINT32_MAX);
	put_le32(expected + 16U, UINT32_MAX);
	assert(memcmp(command.payload, expected, 28U) == 0);
	assert(get_le32(command.payload + 4U) == 1U);
	assert(get_le32(command.payload + 8U) == 0U);
	assert(get_le32(command.payload + 12U) == UINT32_MAX);
	assert(get_le32(command.payload + 16U) == UINT32_MAX);
	assert(get_le32(command.payload + 20U) == 0U);
	assert(get_le32(command.payload + 24U) == 0U);
	assert(accept_current(&state, 103U, NULL) ==
	    INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, 104U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_STATION_ADD);
	assert(command.header == INTEL_AX211_ASSOC_HEADER_LEGACY);
	assert(command.group == 0U);
	assert(command.wire_version == 0U);
	assert(command.opcode == 0x18U && command.layout_version == 10U);
	assert(command.payload_length == 48U);
	memset(expected, 0, sizeof(expected));
	memcpy(expected + 8U, profile.bssid, 6U);
	put_le32(expected + 24U, 0x3c000000U);
	assert(memcmp(command.payload, expected, 48U) == 0);
	assert(command.payload[0U] == 0U);
	assert(memcmp(command.payload + 8U, profile.bssid, 6U) == 0);
	assert(get_le32(command.payload + 24U) == 0x3c000000U);
	assert(accept_current(&state, 104U, NULL) ==
	    INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, 105U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE);
	assert(command.header == INTEL_AX211_ASSOC_HEADER_WIDE);
	assert(command.group == 5U && command.opcode == 0x17U);
	assert(command.wire_version == 0U && command.layout_version == 3U);
	assert(command.response_version == 2U);
	assert(command.payload_length == 36U);
	memset(expected, 0, sizeof(expected));
	put_le32(expected, 0U);
	put_le32(expected + 4U, 1U);
	expected[8U] = 15U;
	put_le32(expected + 16U, 5U);
	put_le64(expected + 20U, profile.queue_byte_count_address);
	put_le64(expected + 28U, profile.queue_descriptor_address);
	assert(memcmp(command.payload, expected, 36U) == 0);
	assert(get_le32(command.payload) == 0U);
	assert(get_le32(command.payload + 4U) == 1U);
	assert(command.expected_queue_write_pointer ==
	    profile.queue_initial_write_pointer);
	assert(command.payload[8U] == 15U);
	assert(get_le32(command.payload + 16U) == 5U);
	assert(get_le64(command.payload + 20U) ==
	    profile.queue_byte_count_address);
	assert(get_le64(command.payload + 28U) ==
	    profile.queue_descriptor_address);
	assert(accept_current(&state, 105U, NULL) ==
	    INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, 106U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_SESSION_PROTECT);
	assert(command.header == INTEL_AX211_ASSOC_HEADER_WIDE);
	assert(command.group == 3U && command.opcode == 0x05U);
	assert(command.wire_version == 0U && command.layout_version == 2U);
	assert(command.payload_length == 24U);
	memset(expected, 0, sizeof(expected));
	put_le32(expected + 4U, 1U);
	put_le32(expected + 12U, 900U);
	assert(memcmp(command.payload, expected, 24U) == 0);
	assert(get_le32(command.payload + 4U) == 1U);
	assert(get_le32(command.payload + 8U) == 0U);
	assert(get_le32(command.payload + 12U) == 900U);
	assert(accept_current(&state, 106U, NULL) ==
	    INTEL_AX211_ASSOC_AUTH_READY);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_AUTH_READY);

	update = make_update();
	assert(intel_ax211_assoc_begin_update(&state, &update, 41U, 7U,
	    200U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_current(&state, 200U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_STATION_UPDATE);
	assert(command.header == INTEL_AX211_ASSOC_HEADER_LEGACY);
	assert(command.group == 0U && command.opcode == 0x18U);
	assert(command.wire_version == 0U && command.layout_version == 10U);
	assert(command.payload_length == 48U);
	memset(expected, 0, sizeof(expected));
	expected[0U] = 1U;
	put_le32(expected + 24U, 0x3c000000U);
	assert(memcmp(command.payload, expected, 48U) == 0);
	assert(command.payload[0U] == 1U);
	assert_zero(command.payload + 8U, 6U);
	assert(accept_current(&state, 200U, NULL) ==
	    INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, 201U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE);
	assert(command.header == INTEL_AX211_ASSOC_HEADER_LEGACY);
	assert(command.group == 0U && command.opcode == 0x28U);
	assert(command.wire_version == 0U && command.layout_version == 1U);
	assert(command.payload_length == 148U);
	memset(expected, 0, sizeof(expected));
	put_le32(expected + 4U, 2U);
	put_le32(expected + 8U, 5U);
	memcpy(expected + 16U, profile.station_address, 6U);
	memcpy(expected + 24U, profile.bssid, 6U);
	put_le32(expected + 32U, 0x0fU);
	put_le32(expected + 36U, 0x15U);
	put_le32(expected + 44U, 0x20U);
	put_le32(expected + 48U, 0x10U);
	put_le32(expected + 52U, 0x44U);
	put_le32(expected + 56U, 1U);
	put_le16(expected + 68U, 15U);
	put_le16(expected + 70U, 1023U);
	expected[72U] = 7U;
	expected[73U] = 2U;
	put_le16(expected + 76U, 15U);
	put_le16(expected + 78U, 1023U);
	expected[80U] = 3U;
	expected[81U] = 4U;
	put_le16(expected + 84U, 7U);
	put_le16(expected + 86U, 15U);
	expected[88U] = 2U;
	expected[89U] = 8U;
	put_le16(expected + 90U, 96U);
	put_le16(expected + 92U, 3U);
	put_le16(expected + 94U, 7U);
	expected[96U] = 2U;
	expected[97U] = 16U;
	put_le16(expected + 98U, 1504U);
	put_le32(expected + 100U, 1U);
	assert(get_le32(command.payload + 4U) == 2U);
	assert(get_le32(command.payload + 52U) == 0x44U);
	assert(get_le32(command.payload + 100U) == 1U);
	dtim_offset = UINT64_C(2) * 100U * 1024U;
	put_le32(expected + 104U,
	    update.beacon_arrive_time + (uint32_t)dtim_offset);
	put_le64(expected + 108U, update.beacon_tsf + dtim_offset);
	put_le32(expected + 116U, 100U);
	put_le32(expected + 124U, 300U);
	put_le32(expected + 132U, 10U);
	put_le32(expected + 136U, 0x123U);
	put_le32(expected + 140U, update.beacon_arrive_time);
	assert(memcmp(command.payload, expected, 148U) == 0);
	assert(get_le32(command.payload + 104U) ==
	    update.beacon_arrive_time + (uint32_t)dtim_offset);
	assert(get_le64(command.payload + 108U) ==
	    update.beacon_tsf + dtim_offset);
	assert(get_le32(command.payload + 124U) == 300U);
	assert(get_le32(command.payload + 136U) == 0x123U);
	assert(get_le32(command.payload + 140U) ==
	    update.beacon_arrive_time);
	result = accept_current(&state, 201U, NULL);
	assert(result == INTEL_AX211_ASSOC_COMPLETE);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_ASSOCIATED);
	assert(intel_ax211_assoc_cancel(&state, 41U, 7U, 202U) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_SESSION_REMOVE);
}

static void
test_reply_boundaries(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_reply reply;
	struct intel_ax211_assoc_state state;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 51U, 8U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_current(&state, 0U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	reply = make_reply(&command);
	reply.common_generation += UINT64_C(1) << 32;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 1U) ==
	    INTEL_AX211_ASSOC_STALE);
	assert(state.step == INTEL_AX211_ASSOC_STEP_PHY_ADD);
	/* A stale reply does not consume the command; the same token is retryable. */
	reply = make_reply(&command);
	reply.hardware_epoch++;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 1U) ==
	    INTEL_AX211_ASSOC_STALE);
	assert(state.step == INTEL_AX211_ASSOC_STEP_PHY_ADD);
	reply = make_reply(&command);
	reply.step = INTEL_AX211_ASSOC_STEP_RLC_CONFIG;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 1U) ==
	    INTEL_AX211_ASSOC_OUT_OF_ORDER);
	reply = make_reply(&command);
	command.payload[8U]++;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 1U) ==
	    INTEL_AX211_ASSOC_OUT_OF_ORDER);
	command.payload[8U]--;
	reply = make_reply(&command);
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 1U) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 2U) ==
	    INTEL_AX211_ASSOC_DUPLICATE);

	assert(intel_ax211_assoc_current(&state, 2U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	reply = make_reply(&command);
	reply.acknowledgement = 1;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 3U) ==
	    INTEL_AX211_ASSOC_FIRMWARE);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK);
	assert(state.step == INTEL_AX211_ASSOC_STEP_PHY_REMOVE);
	assert(accept_current(&state, 3U, NULL) ==
	    INTEL_AX211_ASSOC_ROLLED_BACK);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_IDLE);
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 51U, 8U,
	    10U) == INTEL_AX211_ASSOC_STALE);
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 52U, 8U,
	    10U) == INTEL_AX211_ASSOC_OK);
}

static void
test_queue_response_initial_pointer(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_reply reply;
	struct intel_ax211_assoc_state state;
	size_t index;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 53U, 8U,
	    0U) == INTEL_AX211_ASSOC_OK);
	for (index = 0U; index < 5U; index++)
		assert(accept_current(&state, index, NULL) ==
		    INTEL_AX211_ASSOC_PENDING);
	assert(intel_ax211_assoc_current(&state, 5U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE);
	assert(command.expected_queue_write_pointer ==
	    profile.queue_initial_write_pointer);
	reply = make_reply(&command);
	assert(get_le16(reply.payload + 4U) ==
	    profile.queue_initial_write_pointer);
	put_le16(reply.payload + 4U,
	    (uint16_t)(profile.queue_initial_write_pointer + 1U));
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 6U) ==
	    INTEL_AX211_ASSOC_FIRMWARE);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK);
}

static void
test_timeout_reverse_rollback(void)
{
	static const enum intel_ax211_assoc_step expected[] = {
		INTEL_AX211_ASSOC_STEP_BINDING_REMOVE,
		INTEL_AX211_ASSOC_STEP_MAC_REMOVE,
		INTEL_AX211_ASSOC_STEP_PHY_REMOVE
	};
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_state state;
	size_t index;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 61U, 9U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(accept_current(&state, 0U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(accept_current(&state, 1U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(accept_current(&state, 2U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_BINDING_ADD);
	assert(intel_ax211_assoc_current(&state,
	    state.deadline - 1U, &command) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_expire(&state, state.deadline) ==
	    INTEL_AX211_ASSOC_TIMEOUT);
	for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++) {
		int result;

		assert(state.step == expected[index]);
		result = accept_current(&state, state.deadline - 2U, NULL);
		assert(result ==
		    (index + 1U == sizeof(expected) / sizeof(expected[0]) ?
		    INTEL_AX211_ASSOC_ROLLED_BACK :
		    INTEL_AX211_ASSOC_PENDING));
	}
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_IDLE);
}

static void
test_uncertain_command_cleanup(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_reply reply;
	struct intel_ax211_assoc_state state;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile,
	    UINT64_C(0x10000004d), 14U, 0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_current(&state, 0U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.common_generation == UINT64_C(0x10000004d));
	assert(intel_ax211_assoc_cancel(&state, UINT64_C(0x10000004d), 14U,
	    1U) == INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_PHY_REMOVE);
	assert(accept_current(&state, 1U, NULL) ==
	    INTEL_AX211_ASSOC_ROLLED_BACK);

	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 78U, 14U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(accept_current(&state, 0U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(accept_current(&state, 1U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(accept_current(&state, 2U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(intel_ax211_assoc_current(&state, 3U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_BINDING_ADD);
	reply = make_reply(&command);
	reply.payload_length = 3U;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 4U) ==
	    INTEL_AX211_ASSOC_FIRMWARE);
	assert(state.step == INTEL_AX211_ASSOC_STEP_BINDING_REMOVE);
}

static void
test_exact_reverse_rollback_bytes(void)
{
	static const enum intel_ax211_assoc_step expected[] = {
		INTEL_AX211_ASSOC_STEP_SESSION_REMOVE,
		INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE,
		INTEL_AX211_ASSOC_STEP_STATION_REMOVE,
		INTEL_AX211_ASSOC_STEP_BINDING_REMOVE,
		INTEL_AX211_ASSOC_STEP_MAC_REMOVE,
		INTEL_AX211_ASSOC_STEP_PHY_REMOVE
	};
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_state state;
	size_t index;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 66U, 12U,
	    0U) == INTEL_AX211_ASSOC_OK);
	for (index = 0U; index < 7U; index++) {
		int result;

		result = accept_current(&state, index, NULL);
		assert(result == (index == 6U ? INTEL_AX211_ASSOC_AUTH_READY :
		    INTEL_AX211_ASSOC_PENDING));
	}
	assert(intel_ax211_assoc_cancel(&state, 66U, 12U, 8U) ==
	    INTEL_AX211_ASSOC_PENDING);
	for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++) {
		int result;

		assert(intel_ax211_assoc_current(&state, 8U + index, &command) ==
		    INTEL_AX211_ASSOC_OK);
		assert(command.step == expected[index]);
		assert(command.sequence != 0U);
		assert(command.common_generation == 66U);
		assert(command.hardware_epoch == 12U);
		switch (command.step) {
		case INTEL_AX211_ASSOC_STEP_SESSION_REMOVE:
			assert(command.header == INTEL_AX211_ASSOC_HEADER_WIDE);
			assert(command.group == 3U && command.opcode == 0x05U);
			assert(command.wire_version == 0U &&
			    command.layout_version == 2U);
			assert(command.payload_length == 24U);
			assert(get_le32(command.payload) == 0U);
			assert(get_le32(command.payload + 4U) == 3U);
			assert_zero(command.payload + 8U, 16U);
			break;
		case INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE:
			assert(command.header == INTEL_AX211_ASSOC_HEADER_WIDE);
			assert(command.group == 5U && command.opcode == 0x17U);
			assert(command.wire_version == 0U &&
			    command.layout_version == 3U);
			assert(command.response_version == 2U);
			assert(command.payload_length == 36U);
			assert(get_le32(command.payload) == 1U);
			assert(get_le32(command.payload + 4U) == 1U);
			assert(get_le32(command.payload + 8U) == 15U);
			assert_zero(command.payload + 12U, 24U);
			break;
		case INTEL_AX211_ASSOC_STEP_STATION_REMOVE:
			assert(command.header == INTEL_AX211_ASSOC_HEADER_LEGACY);
			assert(command.group == 0U && command.opcode == 0x19U);
			assert(command.wire_version == 0U &&
			    command.layout_version == 2U);
			assert(command.payload_length == 4U);
			assert_zero(command.payload, 4U);
			break;
		case INTEL_AX211_ASSOC_STEP_BINDING_REMOVE:
			assert(command.header == INTEL_AX211_ASSOC_HEADER_LEGACY);
			assert(command.group == 0U && command.opcode == 0x2bU);
			assert(command.wire_version == 0U &&
			    command.layout_version == 2U);
			assert(command.payload_length == 28U);
			assert(get_le32(command.payload) == 0U);
			assert(get_le32(command.payload + 4U) == 3U);
			assert(get_le32(command.payload + 8U) == 0U);
			assert(get_le32(command.payload + 12U) == UINT32_MAX);
			assert(get_le32(command.payload + 16U) == UINT32_MAX);
			assert(get_le32(command.payload + 20U) == 0U);
			assert(get_le32(command.payload + 24U) == 0U);
			break;
		case INTEL_AX211_ASSOC_STEP_MAC_REMOVE:
			assert(command.header == INTEL_AX211_ASSOC_HEADER_LEGACY);
			assert(command.group == 0U && command.opcode == 0x28U);
			assert(command.wire_version == 0U &&
			    command.layout_version == 1U);
			assert(command.payload_length == 148U);
			assert(get_le32(command.payload) == 0U);
			assert(get_le32(command.payload + 4U) == 3U);
			assert_zero(command.payload + 8U, 140U);
			break;
		case INTEL_AX211_ASSOC_STEP_PHY_REMOVE:
			assert(command.header == INTEL_AX211_ASSOC_HEADER_LEGACY);
			assert(command.group == 0U && command.opcode == 0x08U);
			assert(command.wire_version == 0U &&
			    command.layout_version == 4U);
			assert(command.payload_length == 32U);
			assert(get_le32(command.payload) == 0U);
			assert(get_le32(command.payload + 4U) == 3U);
			assert(get_le32(command.payload + 8U) == 6U);
			assert(command.payload[12U] == 1U);
			assert_zero(command.payload + 13U, 19U);
			break;
		default:
			assert(0);
		}
		result = accept_current(&state, 8U + index, NULL);
		assert(result == (index + 1U ==
		    sizeof(expected) / sizeof(expected[0]) ?
		    INTEL_AX211_ASSOC_ROLLED_BACK :
		    INTEL_AX211_ASSOC_PENDING));
	}
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_IDLE);
}

struct scripted_exchange {
	uint64_t now;
	enum intel_ax211_assoc_step fail_step;
	int fail_result;
	uint8_t failed;
	enum intel_ax211_assoc_step step[INTEL_AX211_ASSOC_COMMAND_LIMIT];
	size_t count;
};

static uint64_t
script_clock(void *argument)
{
	struct scripted_exchange *script = argument;

	return script->now;
}

static int
script_exchange(void *argument,
	const struct intel_ax211_assoc_command *command,
	struct intel_ax211_assoc_reply *reply)
{
	struct scripted_exchange *script = argument;

	assert(script->count < INTEL_AX211_ASSOC_COMMAND_LIMIT);
	script->step[script->count++] = command->step;
	if (!script->failed && command->step == script->fail_step) {
		script->failed = 1U;
		if (script->fail_result == INTEL_AX211_ASSOC_FIRMWARE) {
			*reply = make_reply(command);
			reply->acknowledgement = 1;
			return INTEL_AX211_ASSOC_OK;
		}
		return script->fail_result;
	}
	*reply = make_reply(command);
	script->now++;
	return INTEL_AX211_ASSOC_OK;
}

static const struct intel_ax211_assoc_ops script_ops = {
	script_clock,
	script_exchange
};

static void
test_callback_drive_and_finite_failure(void)
{
	static const enum intel_ax211_assoc_step auth_steps[] = {
		INTEL_AX211_ASSOC_STEP_PHY_ADD,
		INTEL_AX211_ASSOC_STEP_RLC_CONFIG,
		INTEL_AX211_ASSOC_STEP_MAC_ADD,
		INTEL_AX211_ASSOC_STEP_BINDING_ADD,
		INTEL_AX211_ASSOC_STEP_STATION_ADD,
		INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE,
		INTEL_AX211_ASSOC_STEP_SESSION_PROTECT
	};
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_update update;
	struct intel_ax211_assoc_state state;
	struct scripted_exchange script;
	size_t index;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	update = make_update();
	memset(&state, 0, sizeof(state));
	memset(&script, 0, sizeof(script));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 71U, 10U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_AUTH_READY);
	assert(script.count == sizeof(auth_steps) / sizeof(auth_steps[0]));
	for (index = 0U; index < script.count; index++)
		assert(script.step[index] == auth_steps[index]);
	assert(intel_ax211_assoc_begin_update(&state, &update, 71U, 10U,
	    script.now) == INTEL_AX211_ASSOC_OK);
	script.count = 0U;
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_COMPLETE);
	assert(script.count == 2U);
	assert(script.step[0] == INTEL_AX211_ASSOC_STEP_STATION_UPDATE);
	assert(script.step[1] == INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE);

	memset(&state, 0, sizeof(state));
	memset(&script, 0, sizeof(script));
	script.fail_step = INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE;
	script.fail_result = INTEL_AX211_ASSOC_TIMEOUT;
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 72U, 10U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_TIMEOUT);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_IDLE);
	assert(script.count == 11U);
	assert(script.step[5] == INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE);
	assert(script.step[6] == INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE);
	assert(script.step[7] == INTEL_AX211_ASSOC_STEP_STATION_REMOVE);
	assert(script.step[8] == INTEL_AX211_ASSOC_STEP_BINDING_REMOVE);
	assert(script.step[9] == INTEL_AX211_ASSOC_STEP_MAC_REMOVE);
	assert(script.step[10] == INTEL_AX211_ASSOC_STEP_PHY_REMOVE);

	memset(&state, 0, sizeof(state));
	memset(&script, 0, sizeof(script));
	script.fail_step = INTEL_AX211_ASSOC_STEP_STATION_ADD;
	script.fail_result = INTEL_AX211_ASSOC_FIRMWARE;
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 73U, 10U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_FIRMWARE);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_IDLE);
	assert(script.count == 8U);
	assert(script.step[4] == INTEL_AX211_ASSOC_STEP_STATION_ADD);
	assert(script.step[5] == INTEL_AX211_ASSOC_STEP_BINDING_REMOVE);
	assert(script.step[6] == INTEL_AX211_ASSOC_STEP_MAC_REMOVE);
	assert(script.step[7] == INTEL_AX211_ASSOC_STEP_PHY_REMOVE);
}

static void
test_session_notification_natural_expiry(void)
{
	static const enum intel_ax211_assoc_step cleanup_steps[] = {
		INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE,
		INTEL_AX211_ASSOC_STEP_STATION_REMOVE,
		INTEL_AX211_ASSOC_STEP_BINDING_REMOVE,
		INTEL_AX211_ASSOC_STEP_MAC_REMOVE,
		INTEL_AX211_ASSOC_STEP_PHY_REMOVE
	};
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t payload[INTEL_AX211_ASSOC_SESSION_NOTIFICATION_SIZE];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_state state;
	struct scripted_exchange script;
	size_t index;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	memset(&script, 0, sizeof(script));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 81U, 12U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_AUTH_READY);
	message = make_session_event(payload, 12U,
	    INTEL_AX211_ASSOC_MAC_CONTEXT_ID, 1U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 81U,
	    12U) == INTEL_AX211_ASSOC_SESSION_EXPIRED);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 81U,
	    12U) == INTEL_AX211_ASSOC_DUPLICATE);

	/* Disconnect must begin at queue removal, not issue duplicate REMOVE. */
	assert(intel_ax211_assoc_cancel(&state, 81U, 12U, script.now) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE);
	script.count = 0U;
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_ROLLED_BACK);
	assert(script.count == sizeof(cleanup_steps) / sizeof(cleanup_steps[0]));
	for (index = 0U; index < script.count; index++)
		assert(script.step[index] == cleanup_steps[index]);
}

static void
test_session_notification_boundaries(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t payload[INTEL_AX211_ASSOC_SESSION_NOTIFICATION_SIZE];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_state state;
	struct scripted_exchange script;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	memset(&script, 0, sizeof(script));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 82U, 13U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_AUTH_READY);
	message = make_session_event(payload, 13U,
	    INTEL_AX211_ASSOC_MAC_CONTEXT_ID, 1U, 0U, 0U);

	assert(intel_ax211_assoc_session_event_accept(&state, &message, 83U,
	    13U) == INTEL_AX211_ASSOC_STALE);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    14U) == INTEL_AX211_ASSOC_STALE);
	message.generation = 12U;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_STALE);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	message.group = 4U;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_UNSUPPORTED);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	message.opcode--;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_UNSUPPORTED);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	message.version = INTEL_AX211_ASSOC_SESSION_NOTIFICATION_API89_VERSION;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_UNSUPPORTED);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	message.flags = INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_FIRMWARE);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	message.queue = 0U;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_OUT_OF_ORDER);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	message.payload = NULL;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_INVALID);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	message.payload_length--;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_TRUNCATED);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	message.payload_length++;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_OVERSIZED);
	message = make_session_event(payload, 13U, 1U, 1U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_EVENT_IGNORED);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 1U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_EVENT_IGNORED);
	message = make_session_event(payload, 13U, 0U, 2U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_FIRMWARE);
	message = make_session_event(payload, 13U, 0U, 1U, 2U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_FIRMWARE);
	message = make_session_event(payload, 13U, 0U, 1U, 1U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_EVENT_IGNORED);
	message = make_session_event(payload, 13U, 0U, 0U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_EVENT_IGNORED);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_SESSION_EXPIRED);
}

static void
test_session_expiry_before_command_ack(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t payload[INTEL_AX211_ASSOC_SESSION_NOTIFICATION_SIZE];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_state state;
	size_t index;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 84U, 14U,
	    0U) == INTEL_AX211_ASSOC_OK);
	for (index = 0U; index < 6U; index++)
		assert(accept_current(&state, index, NULL) ==
		    INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_SESSION_PROTECT);
	message = make_session_event(payload, 14U, 0U, 1U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 84U,
	    14U) == INTEL_AX211_ASSOC_SESSION_EXPIRED);
	assert(accept_current(&state, 6U, NULL) ==
	    INTEL_AX211_ASSOC_AUTH_READY);
	assert(intel_ax211_assoc_cancel(&state, 84U, 14U, 7U) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE);
}

static void
test_probe_only_association_without_dtim(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_update update;
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_state state;
	struct scripted_exchange script;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	update = make_update();
	update.dtim_period = 0U;
	update.dtim_count = 1U;
	memset(&state, 0, sizeof(state));
	memset(&script, 0, sizeof(script));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 74U, 11U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_AUTH_READY);
	assert(intel_ax211_assoc_begin_update(&state, &update, 74U, 11U,
	    script.now) == INTEL_AX211_ASSOC_INVALID);

	/* Probe responses have a timestamp and receive time but no TIM IE. */
	update.dtim_count = 0U;
	assert(intel_ax211_assoc_begin_update(&state, &update, 74U, 11U,
	    script.now) == INTEL_AX211_ASSOC_OK);
	assert(accept_current(&state, script.now, NULL) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(intel_ax211_assoc_current(&state, script.now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE);
	assert(get_le32(command.payload + 52U) == 0x44U);
	assert(get_le32(command.payload + 104U) ==
	    update.beacon_arrive_time);
	assert(get_le64(command.payload + 108U) == update.beacon_tsf);
	assert(get_le32(command.payload + 124U) == 0U);
	assert(get_le32(command.payload + 136U) == update.association_id);
	assert(get_le32(command.payload + 140U) ==
	    update.beacon_arrive_time);
	assert(accept_current(&state, script.now, NULL) ==
	    INTEL_AX211_ASSOC_COMPLETE);
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

static int
find_command_table(const uint8_t *bytes, size_t length,
	const uint8_t **table_bytes, size_t *table_length)
{
	size_t offset;

	if (bytes == NULL || table_bytes == NULL || table_length == NULL ||
	    length < 88U)
		return 0;
	offset = 88U;
	while (offset <= length && length - offset >= 8U) {
		uint32_t type;
		uint32_t payload_length;
		size_t padded;

		type = get_le32(bytes + offset);
		payload_length = get_le32(bytes + offset + 4U);
		offset += 8U;
		if ((size_t)payload_length > length - offset ||
		    payload_length > UINT32_MAX - 3U)
			return 0;
		padded = ((size_t)payload_length + 3U) & ~(size_t)3U;
		if (padded > length - offset)
			return 0;
		if (type == 48U) {
			*table_bytes = bytes + offset;
			*table_length = payload_length;
			return 1;
		}
		offset += padded;
	}
	return 0;
}

static void
test_real_api89_table(const char *path)
{
	const uint8_t *table_bytes;
	uint8_t *firmware;
	size_t firmware_length;
	size_t table_length;
	struct intel_ax211_protocol_command_table table;

	firmware = read_file(path, &firmware_length);
	assert(find_command_table(firmware, firmware_length, &table_bytes,
	    &table_length));
	assert(table_length == INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES);
	assert(intel_ax211_protocol_command_table_parse(table_bytes,
	    table_length, &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_assoc_mcast_filter_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_mac_power_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_OK);
	free(firmware);
}

int
main(int argc, char **argv)
{
	test_api89_versions();
	test_mcast_filter_codec();
	test_mac_power_codec();
	test_exact_auth_and_association_bytes();
	test_reply_boundaries();
	test_queue_response_initial_pointer();
	test_timeout_reverse_rollback();
	test_uncertain_command_cleanup();
	test_exact_reverse_rollback_bytes();
	test_callback_drive_and_finite_failure();
	test_session_notification_natural_expiry();
	test_session_notification_boundaries();
	test_session_expiry_before_command_ack();
	test_probe_only_association_without_dtim();
	if (argc > 1)
		test_real_api89_table(argv[1]);
	puts("intel ax211 association session fixture: PASS");
	return 0;
}
