/*
 * zedBSD Intel AX211 API89 MLD association-session fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-assoc.h"

#define TEST_ACTION_ADD                                      1U
#define TEST_ACTION_MODIFY                                   2U
#define TEST_ACTION_REMOVE                                   3U
#define TEST_INVALID_CONTEXT                        UINT32_MAX
#define TEST_LINK_MODIFY_ACTIVE                            0x01U
#define TEST_LINK_MODIFY_RATES                             0x02U
#define TEST_LINK_MODIFY_QOS                               0x08U
#define TEST_LINK_MODIFY_BEACON_TIMING                     0x10U

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
	/* API89 advertises notification v3 for the unchanged v2 layout. */
	put_version(bytes, 10U, 0x03U, 0xfbU, 99U, 3U);
	/* Logical legacy group 0 commands are listed under LONG_GROUP. */
	put_version(bytes, 11U, 0x01U, 0xd0U, 1U, 0U);
	put_version(bytes, 12U, 0x01U, 0xa9U, 1U, 0U);
	put_version(bytes, 13U, 0x03U, 0x08U, 2U, 0U);
	put_version(bytes, 14U, 0x03U, 0x09U, 2U, 0U);
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
	profile.dtim_period = 3U;
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
	case INTEL_AX211_ASSOC_RESPONSE_QUEUE:
		put_le16(reply.payload, 0x101U);
		put_le16(reply.payload + 4U, 0x345U);
		reply.payload_length = 8U;
		break;
	case INTEL_AX211_ASSOC_RESPONSE_IGNORED:
		break;
	default:
		assert(0);
	}
	return reply;
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
expect_command(const struct intel_ax211_assoc_command *command,
	enum intel_ax211_assoc_step step, enum intel_ax211_assoc_header header,
	uint8_t group, uint8_t opcode, uint8_t wire_version,
	uint8_t layout_version, size_t payload_length,
	const uint8_t *expected)
{
	assert(command->step == step);
	assert(command->header == header);
	assert(command->group == group);
	assert(command->opcode == opcode);
	assert(command->wire_version == wire_version);
	assert(command->layout_version == layout_version);
	assert(command->payload_length == payload_length);
	assert(memcmp(command->payload, expected, payload_length) == 0);
}

static void
expected_mac(const struct intel_ax211_assoc_profile *profile,
	uint32_t action, uint16_t association_id,
	uint8_t output[INTEL_AX211_ASSOC_MAC_CONFIG_SIZE])
{
	memset(output, 0, INTEL_AX211_ASSOC_MAC_CONFIG_SIZE);
	put_le32(output, INTEL_AX211_ASSOC_MAC_ID);
	put_le32(output + 4U, action);
	if (action == TEST_ACTION_REMOVE)
		return;
	put_le32(output + 8U, 5U);
	memcpy(output + 12U, profile->station_address, 6U);
	put_le32(output + 20U, 0x0cU);
	put_le32(output + 32U, 1U);
	if (association_id != 0U) {
		output[36U] = 1U;
		put_le16(output + 40U, association_id);
	}
}

static void
expected_edca_entry(const struct intel_ax211_assoc_edca *edca,
	uint8_t fifo, uint8_t output[8U])
{
	put_le16(output,
	    (uint16_t)((UINT32_C(1) << edca->ecw_min) - 1U));
	put_le16(output + 2U,
	    (uint16_t)((UINT32_C(1) << edca->ecw_max) - 1U));
	output[4U] = edca->aifsn;
	output[5U] = (uint8_t)(UINT32_C(1) << fifo);
	put_le16(output + 6U, (uint16_t)(edca->txop_32us * 32U));
}

static void
expected_link(const struct intel_ax211_assoc_profile *profile,
	uint32_t action, uint32_t modify_mask, uint32_t active,
	uint32_t phy_id, uint8_t dtim_period,
	uint8_t output[INTEL_AX211_ASSOC_LINK_CONFIG_SIZE])
{
	memset(output, 0, INTEL_AX211_ASSOC_LINK_CONFIG_SIZE);
	put_le32(output, action);
	put_le32(output + 4U, INTEL_AX211_ASSOC_LINK_ID);
	put_le32(output + 12U, phy_id);
	if (action == TEST_ACTION_REMOVE)
		return;
	put_le32(output + 8U, INTEL_AX211_ASSOC_MAC_ID);
	memcpy(output + 16U, profile->station_address, 6U);
	if (action == TEST_ACTION_ADD)
		return;
	put_le32(output + 24U, modify_mask);
	put_le32(output + 28U, active);
	put_le32(output + 36U, profile->cck_ack_rates);
	put_le32(output + 40U, profile->ofdm_ack_rates);
	put_le32(output + 44U, profile->short_preamble);
	put_le32(output + 48U, profile->short_slot);
	put_le32(output + 56U, profile->qos ? 1U : 0U);
	/* Wire order is BK, BE, VI, VO; profile order is BE, BK, VI, VO. */
	expected_edca_entry(&profile->edca[1U], 1U, output + 60U);
	expected_edca_entry(&profile->edca[0U], 2U, output + 68U);
	expected_edca_entry(&profile->edca[2U], 3U, output + 76U);
	expected_edca_entry(&profile->edca[3U], 4U, output + 84U);
	put_le32(output + 136U, profile->beacon_interval_tu);
	put_le32(output + 140U,
	    (uint32_t)profile->beacon_interval_tu * dtim_period);
}

static void
expected_station(const struct intel_ax211_assoc_profile *profile,
	uint16_t association_id,
	uint8_t output[INTEL_AX211_ASSOC_STATION_CONFIG_SIZE])
{
	memset(output, 0, INTEL_AX211_ASSOC_STATION_CONFIG_SIZE);
	put_le32(output, INTEL_AX211_ASSOC_STATION_ID);
	put_le32(output + 4U, INTEL_AX211_ASSOC_LINK_ID);
	memcpy(output + 8U, profile->bssid, 6U);
	memcpy(output + 16U, profile->bssid, 6U);
	put_le32(output + 24U, 0U);
	put_le32(output + 28U, association_id);
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

static void
test_api89_versions(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t changed[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;

	make_api89_table(bytes);
	table = parse_table(bytes);
	/* STA_CONFIG/STA_REMOVE are implicit API89 v1 commands. */
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_OK);

	memcpy(changed, bytes, sizeof(changed));
	put_version(changed, 13U, 0x03U, 0x08U, 1U, 0U);
	table = parse_table(changed);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
	memcpy(changed, bytes, sizeof(changed));
	put_version(changed, 14U, 0x03U, 0x09U, 3U, 0U);
	table = parse_table(changed);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);

	/* If firmware explicitly advertises either implicit command, it must be v1. */
	memcpy(changed, bytes, sizeof(changed));
	put_version(changed, 15U, 0x03U, 0x0aU, 2U, 0U);
	table = parse_table(changed);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
	memcpy(changed, bytes, sizeof(changed));
	put_version(changed, 15U, 0x03U, 0x0aU, 1U, 0U);
	put_version(changed, 16U, 0x03U, 0x0cU, 1U, 0U);
	table = parse_table(changed);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_OK);

	memcpy(changed, bytes, sizeof(changed));
	put_version(changed, 7U, 0x05U, 0x08U, 2U, 2U);
	table = parse_table(changed);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
	memcpy(changed, bytes, sizeof(changed));
	put_version(changed, 8U, 0x05U, 0x17U, 3U, 1U);
	table = parse_table(changed);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
	memcpy(changed, bytes, sizeof(changed));
	put_version(changed, 10U, 0x03U, 0xfbU, 99U, 2U);
	table = parse_table(changed);
	assert(intel_ax211_assoc_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_UNSUPPORTED);
}

static void
test_auxiliary_codecs(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t output[48U];
	uint8_t expected[INTEL_AX211_ASSOC_MAC_POWER_SIZE];
	uint8_t response[5U];
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
	assert(memcmp(output, expected,
	    INTEL_AX211_ASSOC_MCAST_FILTER_SIZE) == 0);
	assert(output[INTEL_AX211_ASSOC_MCAST_FILTER_SIZE] == 0xa5U);
	assert(intel_ax211_assoc_mcast_filter_encode(profile.bssid, output,
	    INTEL_AX211_ASSOC_MCAST_FILTER_SIZE - 1U) ==
	    INTEL_AX211_ASSOC_BUFFER_TOO_SMALL);

	assert(intel_ax211_assoc_mac_power_api89_validate(&table) ==
	    INTEL_AX211_ASSOC_OK);
	memset(output, 0xa5, sizeof(output));
	memset(expected, 0, sizeof(expected));
	put_le16(expected + 6U, 25U);
	assert(intel_ax211_assoc_mac_power_encode(3U, 100U, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_OK);
	assert(memcmp(output, expected, sizeof(expected)) == 0);
	assert(output[40U] == 0xa5U);
	assert(intel_ax211_assoc_mac_power_encode(1U, 21845001U, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_OVERSIZED);
	assert(intel_ax211_assoc_mac_power_encode(1U, 0U, output,
	    sizeof(output)) == INTEL_AX211_ASSOC_INVALID);

	memset(response, 0, sizeof(response));
	assert(intel_ax211_assoc_mac_power_response_validate(response, 0U) ==
	    INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_mac_power_response_validate(response, 4U) ==
	    INTEL_AX211_ASSOC_OK);
	put_le32(response, 1U);
	assert(intel_ax211_assoc_mac_power_response_validate(response, 4U) ==
	    INTEL_AX211_ASSOC_FIRMWARE);
	assert(intel_ax211_assoc_mac_power_response_validate(response, 3U) ==
	    INTEL_AX211_ASSOC_TRUNCATED);
	assert(intel_ax211_assoc_mac_power_response_validate(response, 5U) ==
	    INTEL_AX211_ASSOC_OVERSIZED);
}

static void
test_profile_band_and_bounds(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_state state;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	profile.channel = 36U;
	profile.cck_ack_rates = 0U;
	profile.short_preamble = 0U;
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 40U, 7U,
	    100U) == INTEL_AX211_ASSOC_OK);
	assert(accept_current(&state, 100U, NULL) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(accept_current(&state, 101U, NULL) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(intel_ax211_assoc_current(&state, 102U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_PHY_ADD);
	assert(get_le32(command.payload + 8U) == 36U);
	assert(command.payload[12U] == 0U);

	memset(&state, 0, sizeof(state));
	profile.channel = 35U;
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 41U, 7U,
	    100U) == INTEL_AX211_ASSOC_INVALID);
	profile.channel = 182U;
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 41U, 7U,
	    100U) == INTEL_AX211_ASSOC_INVALID);
	profile = make_profile();
	profile.channel_width_mhz = 40U;
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 41U, 7U,
	    100U) == INTEL_AX211_ASSOC_INVALID);
}

static void
test_exact_mld_sequence(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t expected[INTEL_AX211_ASSOC_PAYLOAD_MAX];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_update update;
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_state state;
	uint64_t now;
	int result;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 41U, 7U,
	    100U) == INTEL_AX211_ASSOC_OK);
	now = 100U;

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	expected_mac(&profile, TEST_ACTION_ADD, 0U, expected);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_MAC_ADD,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x08U, 0U, 2U, 52U,
	    expected);
	assert(command.response_kind == INTEL_AX211_ASSOC_RESPONSE_EMPTY);
	assert(accept_current(&state, now++, NULL) == INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	expected_link(&profile, TEST_ACTION_ADD, 0U, 0U,
	    TEST_INVALID_CONTEXT, profile.dtim_period, expected);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_LINK_ADD,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x09U, 0U, 2U, 208U,
	    expected);
	assert(accept_current(&state, now++, NULL) == INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	memset(expected, 0, sizeof(expected));
	put_le32(expected + 4U, TEST_ACTION_ADD);
	put_le32(expected + 8U, profile.channel);
	expected[12U] = 1U;
	expect_command(&command, INTEL_AX211_ASSOC_STEP_PHY_ADD,
	    INTEL_AX211_ASSOC_HEADER_LEGACY, 0U, 0x08U, 0U, 4U, 32U,
	    expected);
	assert(accept_current(&state, now++, NULL) == INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	memset(expected, 0, sizeof(expected));
	put_le32(expected + 4U, 0x1406U);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_RLC_CONFIG,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 5U, 0x08U, 2U, 2U, 32U,
	    expected);
	assert(accept_current(&state, now++, NULL) == INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	expected_link(&profile, TEST_ACTION_MODIFY, 0U, 0U, 0U,
	    profile.dtim_period, expected);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_LINK_ASSIGN,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x09U, 0U, 2U, 208U,
	    expected);
	assert(get_le32(command.payload + 24U) == 0U);
	assert(get_le32(command.payload + 28U) == 0U);
	assert(accept_current(&state, now++, NULL) == INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	expected_link(&profile, TEST_ACTION_MODIFY,
	    TEST_LINK_MODIFY_ACTIVE | TEST_LINK_MODIFY_RATES, 1U, 0U,
	    profile.dtim_period, expected);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_LINK_ACTIVATE,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x09U, 0U, 2U, 208U,
	    expected);
	assert(get_le32(command.payload + 24U) == 0x03U);
	assert(get_le32(command.payload + 28U) == 1U);
	assert(get_le16(command.payload + 60U) == 15U);
	assert(get_le16(command.payload + 68U) == 15U);
	assert(get_le16(command.payload + 76U) == 7U);
	assert(get_le16(command.payload + 84U) == 3U);
	assert(get_le16(command.payload + 92U) == 0U);
	assert(get_le32(command.payload + 136U) == 100U);
	assert(get_le32(command.payload + 140U) == 300U);
	assert(accept_current(&state, now++, NULL) == INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	expected_station(&profile, 0U, expected);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_STATION_ADD,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x0aU, 0U, 1U, 96U,
	    expected);
	assert(accept_current(&state, now++, NULL) == INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	memset(expected, 0, sizeof(expected));
	put_le32(expected + 4U, 1U);
	expected[8U] = 15U;
	put_le32(expected + 16U, 5U);
	put_le64(expected + 20U, profile.queue_byte_count_address);
	put_le64(expected + 28U, profile.queue_descriptor_address);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 5U, 0x17U, 0U, 3U, 36U,
	    expected);
	assert(command.response_kind == INTEL_AX211_ASSOC_RESPONSE_QUEUE);
	assert(command.response_version == 2U);
	assert(accept_current(&state, now++, NULL) == INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	memset(expected, 0, sizeof(expected));
	put_le32(expected + 4U, TEST_ACTION_ADD);
	put_le32(expected + 12U, 900U);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_SESSION_PROTECT,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x05U, 0U, 2U, 24U,
	    expected);
	assert(accept_current(&state, now++, NULL) ==
	    INTEL_AX211_ASSOC_AUTH_READY);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_AUTH_READY);

	update = make_update();
	assert(intel_ax211_assoc_begin_update(&state, &update, 41U, 7U,
	    now) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	expected_mac(&profile, TEST_ACTION_MODIFY, update.association_id,
	    expected);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x08U, 0U, 2U, 52U,
	    expected);
	assert(command.payload[36U] == 1U);
	assert(get_le16(command.payload + 40U) == update.association_id);
	assert(accept_current(&state, now++, NULL) == INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	expected_link(&profile, TEST_ACTION_MODIFY,
	    TEST_LINK_MODIFY_RATES | TEST_LINK_MODIFY_QOS |
	    TEST_LINK_MODIFY_BEACON_TIMING, 1U, 0U, update.dtim_period,
	    expected);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_LINK_ASSOCIATE,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x09U, 0U, 2U, 208U,
	    expected);
	assert(get_le32(command.payload + 24U) == 0x1aU);
	assert(get_le32(command.payload + 28U) == 1U);
	assert(accept_current(&state, now++, NULL) == INTEL_AX211_ASSOC_PENDING);

	assert(intel_ax211_assoc_current(&state, now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	expected_station(&profile, update.association_id, expected);
	expect_command(&command, INTEL_AX211_ASSOC_STEP_STATION_UPDATE,
	    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x0aU, 0U, 1U, 96U,
	    expected);
	result = accept_current(&state, now, NULL);
	assert(result == INTEL_AX211_ASSOC_COMPLETE);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_ASSOCIATED);
}

static void
test_response_matching_and_queue_pointer(void)
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
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 51U, 8U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_current(&state, 0U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	reply = make_reply(&command);
	reply.common_generation += UINT64_C(1) << 32;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 1U) ==
	    INTEL_AX211_ASSOC_STALE);
	reply = make_reply(&command);
	reply.hardware_epoch++;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 1U) ==
	    INTEL_AX211_ASSOC_STALE);
	reply = make_reply(&command);
	reply.step = INTEL_AX211_ASSOC_STEP_LINK_ADD;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 1U) ==
	    INTEL_AX211_ASSOC_OUT_OF_ORDER);
	reply = make_reply(&command);
	command.payload[20U]++;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 1U) ==
	    INTEL_AX211_ASSOC_OUT_OF_ORDER);
	command.payload[20U]--;
	reply = make_reply(&command);
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 1U) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 2U) ==
	    INTEL_AX211_ASSOC_DUPLICATE);

	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 52U, 8U,
	    0U) == INTEL_AX211_ASSOC_OK);
	for (index = 0U; index < 7U; index++)
		assert(accept_current(&state, index, NULL) ==
		    INTEL_AX211_ASSOC_PENDING);
	assert(intel_ax211_assoc_current(&state, 7U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE);
	reply = make_reply(&command);
	put_le16(reply.payload + 2U, 1U);
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 8U) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_AUTH);
	assert(state.step == INTEL_AX211_ASSOC_STEP_SESSION_PROTECT);
}

static void
test_exact_reverse_rollback(void)
{
	static const enum intel_ax211_assoc_step expected_steps[] = {
		INTEL_AX211_ASSOC_STEP_SESSION_REMOVE,
		INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE,
		INTEL_AX211_ASSOC_STEP_STATION_REMOVE,
		INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE,
		INTEL_AX211_ASSOC_STEP_LINK_REMOVE,
		INTEL_AX211_ASSOC_STEP_MAC_REMOVE,
		INTEL_AX211_ASSOC_STEP_PHY_REMOVE
	};
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t expected[INTEL_AX211_ASSOC_PAYLOAD_MAX];
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
	for (index = 0U; index < 9U; index++) {
		int result;

		result = accept_current(&state, index, NULL);
		assert(result == (index == 8U ?
		    INTEL_AX211_ASSOC_AUTH_READY : INTEL_AX211_ASSOC_PENDING));
	}
	assert(intel_ax211_assoc_cancel(&state, 66U, 12U, 10U) ==
	    INTEL_AX211_ASSOC_PENDING);

	for (index = 0U;
	    index < sizeof(expected_steps) / sizeof(expected_steps[0]); index++) {
		int result;

		assert(intel_ax211_assoc_current(&state, 10U + index, &command) ==
		    INTEL_AX211_ASSOC_OK);
		assert(command.step == expected_steps[index]);
		switch (command.step) {
		case INTEL_AX211_ASSOC_STEP_SESSION_REMOVE:
			memset(expected, 0, sizeof(expected));
			put_le32(expected + 4U, TEST_ACTION_REMOVE);
			expect_command(&command, command.step,
			    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x05U, 0U, 2U,
			    24U, expected);
			break;
		case INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE:
			memset(expected, 0, sizeof(expected));
			put_le32(expected, 1U);
			put_le32(expected + 4U, 1U);
			put_le32(expected + 8U, 15U);
			expect_command(&command, command.step,
			    INTEL_AX211_ASSOC_HEADER_WIDE, 5U, 0x17U, 0U, 3U,
			    36U, expected);
			break;
		case INTEL_AX211_ASSOC_STEP_STATION_REMOVE:
			memset(expected, 0, sizeof(expected));
			expect_command(&command, command.step,
			    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x0cU, 0U, 1U,
			    4U, expected);
			break;
		case INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE:
			expected_link(&profile, TEST_ACTION_MODIFY,
			    TEST_LINK_MODIFY_ACTIVE, 0U, 0U, profile.dtim_period,
			    expected);
			expect_command(&command, command.step,
			    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x09U, 0U, 2U,
			    208U, expected);
			break;
		case INTEL_AX211_ASSOC_STEP_LINK_REMOVE:
			expected_link(&profile, TEST_ACTION_REMOVE, 0U, 0U,
			    TEST_INVALID_CONTEXT, profile.dtim_period, expected);
			expect_command(&command, command.step,
			    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x09U, 0U, 2U,
			    208U, expected);
			break;
		case INTEL_AX211_ASSOC_STEP_MAC_REMOVE:
			expected_mac(&profile, TEST_ACTION_REMOVE, 0U, expected);
			expect_command(&command, command.step,
			    INTEL_AX211_ASSOC_HEADER_WIDE, 3U, 0x08U, 0U, 2U,
			    52U, expected);
			break;
		case INTEL_AX211_ASSOC_STEP_PHY_REMOVE:
			memset(expected, 0, sizeof(expected));
			put_le32(expected + 4U, TEST_ACTION_REMOVE);
			put_le32(expected + 8U, profile.channel);
			expected[12U] = 1U;
			expect_command(&command, command.step,
			    INTEL_AX211_ASSOC_HEADER_LEGACY, 0U, 0x08U, 0U, 4U,
			    32U, expected);
			break;
		default:
			assert(0);
		}
		result = accept_current(&state, 10U + index, NULL);
		assert(result == (index + 1U ==
		    sizeof(expected_steps) / sizeof(expected_steps[0]) ?
		    INTEL_AX211_ASSOC_ROLLED_BACK : INTEL_AX211_ASSOC_PENDING));
	}
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_IDLE);
}

static void
test_uncertain_and_timeout_cleanup(void)
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

	/* An unacknowledged first MAC ADD is cleaned up as possibly applied. */
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile,
	    UINT64_C(0x10000004d), 14U, 0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_current(&state, 0U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_cancel(&state, UINT64_C(0x10000004d), 14U,
	    1U) == INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_MAC_REMOVE);
	assert(accept_current(&state, 1U, NULL) ==
	    INTEL_AX211_ASSOC_ROLLED_BACK);

	/* A malformed empty response makes LINK ADD uncertain. */
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 78U, 14U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(accept_current(&state, 0U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(intel_ax211_assoc_current(&state, 1U, &command) ==
	    INTEL_AX211_ASSOC_OK);
	reply = make_reply(&command);
	reply.payload_length = 1U;
	assert(intel_ax211_assoc_accept(&state, &command, &reply, 2U) ==
	    INTEL_AX211_ASSOC_FIRMWARE);
	assert(state.step == INTEL_AX211_ASSOC_STEP_LINK_REMOVE);
	assert(accept_current(&state, 2U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_MAC_REMOVE);
	assert(accept_current(&state, 3U, NULL) ==
	    INTEL_AX211_ASSOC_ROLLED_BACK);

	/* Timeout after activation assumes the MODIFY may have activated. */
	memset(&state, 0, sizeof(state));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 79U, 14U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(accept_current(&state, 0U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(accept_current(&state, 1U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(accept_current(&state, 2U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(accept_current(&state, 3U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(accept_current(&state, 4U, NULL) == INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_LINK_ACTIVATE);
	assert(intel_ax211_assoc_expire(&state, state.deadline) ==
	    INTEL_AX211_ASSOC_TIMEOUT);
	assert(state.step == INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE);
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
test_callback_drive_and_failure(void)
{
	static const enum intel_ax211_assoc_step auth_steps[] = {
		INTEL_AX211_ASSOC_STEP_MAC_ADD,
		INTEL_AX211_ASSOC_STEP_LINK_ADD,
		INTEL_AX211_ASSOC_STEP_PHY_ADD,
		INTEL_AX211_ASSOC_STEP_RLC_CONFIG,
		INTEL_AX211_ASSOC_STEP_LINK_ASSIGN,
		INTEL_AX211_ASSOC_STEP_LINK_ACTIVATE,
		INTEL_AX211_ASSOC_STEP_STATION_ADD,
		INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE,
		INTEL_AX211_ASSOC_STEP_SESSION_PROTECT
	};
	static const enum intel_ax211_assoc_step update_steps[] = {
		INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE,
		INTEL_AX211_ASSOC_STEP_LINK_ASSOCIATE,
		INTEL_AX211_ASSOC_STEP_STATION_UPDATE
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
	assert(script.count == sizeof(update_steps) / sizeof(update_steps[0]));
	for (index = 0U; index < script.count; index++)
		assert(script.step[index] == update_steps[index]);

	memset(&state, 0, sizeof(state));
	memset(&script, 0, sizeof(script));
	script.fail_step = INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE;
	script.fail_result = INTEL_AX211_ASSOC_TIMEOUT;
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 72U, 10U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_TIMEOUT);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_IDLE);
	assert(script.count == 14U);
	assert(script.step[7U] == INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE);
	assert(script.step[8U] == INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE);
	assert(script.step[9U] == INTEL_AX211_ASSOC_STEP_STATION_REMOVE);
	assert(script.step[10U] == INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE);
	assert(script.step[11U] == INTEL_AX211_ASSOC_STEP_LINK_REMOVE);
	assert(script.step[12U] == INTEL_AX211_ASSOC_STEP_MAC_REMOVE);
	assert(script.step[13U] == INTEL_AX211_ASSOC_STEP_PHY_REMOVE);

	memset(&state, 0, sizeof(state));
	memset(&script, 0, sizeof(script));
	script.fail_step = INTEL_AX211_ASSOC_STEP_STATION_ADD;
	script.fail_result = INTEL_AX211_ASSOC_FIRMWARE;
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 73U, 10U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_FIRMWARE);
	assert(state.phase == INTEL_AX211_ASSOC_PHASE_IDLE);
	assert(script.count == 11U);
	assert(script.step[6U] == INTEL_AX211_ASSOC_STEP_STATION_ADD);
	assert(script.step[7U] == INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE);
	assert(script.step[8U] == INTEL_AX211_ASSOC_STEP_LINK_REMOVE);
	assert(script.step[9U] == INTEL_AX211_ASSOC_STEP_MAC_REMOVE);
	assert(script.step[10U] == INTEL_AX211_ASSOC_STEP_PHY_REMOVE);
}

static void
test_session_notifications(void)
{
	static const enum intel_ax211_assoc_step cleanup_steps[] = {
		INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE,
		INTEL_AX211_ASSOC_STEP_STATION_REMOVE,
		INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE,
		INTEL_AX211_ASSOC_STEP_LINK_REMOVE,
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
	    INTEL_AX211_ASSOC_MAC_ID, 1U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 81U,
	    12U) == INTEL_AX211_ASSOC_SESSION_EXPIRED);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 81U,
	    12U) == INTEL_AX211_ASSOC_DUPLICATE);
	assert(intel_ax211_assoc_cancel(&state, 81U, 12U, script.now) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE);
	script.count = 0U;
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_ROLLED_BACK);
	assert(script.count == sizeof(cleanup_steps) / sizeof(cleanup_steps[0]));
	for (index = 0U; index < script.count; index++)
		assert(script.step[index] == cleanup_steps[index]);

	/* Bound the unsolicited-notification identity and payload. */
	memset(&state, 0, sizeof(state));
	memset(&script, 0, sizeof(script));
	assert(intel_ax211_assoc_begin(&state, &table, &profile, 82U, 13U,
	    0U) == INTEL_AX211_ASSOC_OK);
	assert(intel_ax211_assoc_drive(&state, &script_ops, &script) ==
	    INTEL_AX211_ASSOC_AUTH_READY);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 83U,
	    13U) == INTEL_AX211_ASSOC_STALE);
	message.group = 4U;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_UNSUPPORTED);
	message = make_session_event(payload, 13U, 0U, 1U, 0U, 0U);
	message.payload_length--;
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_TRUNCATED);
	message = make_session_event(payload, 13U, 1U, 1U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_EVENT_IGNORED);
	message = make_session_event(payload, 13U, 0U, 2U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 82U,
	    13U) == INTEL_AX211_ASSOC_FIRMWARE);
}

static void
test_session_expiry_before_ack_and_probe_dtim(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t payload[INTEL_AX211_ASSOC_SESSION_NOTIFICATION_SIZE];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_update update;
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_state state;
	struct scripted_exchange script;
	size_t index;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_profile();
	memset(&state, 0, sizeof(state));
	for (index = 0U; index < 8U; index++) {
		if (index == 0U)
			assert(intel_ax211_assoc_begin(&state, &table, &profile,
			    84U, 14U, 0U) == INTEL_AX211_ASSOC_OK);
		assert(accept_current(&state, index, NULL) ==
		    INTEL_AX211_ASSOC_PENDING);
	}
	assert(state.step == INTEL_AX211_ASSOC_STEP_SESSION_PROTECT);
	message = make_session_event(payload, 14U, 0U, 1U, 0U, 0U);
	assert(intel_ax211_assoc_session_event_accept(&state, &message, 84U,
	    14U) == INTEL_AX211_ASSOC_SESSION_EXPIRED);
	assert(accept_current(&state, 8U, NULL) ==
	    INTEL_AX211_ASSOC_AUTH_READY);
	assert(intel_ax211_assoc_cancel(&state, 84U, 14U, 9U) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(state.step == INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE);

	profile = make_profile();
	profile.dtim_period = 0U;
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
	update.dtim_count = 0U;
	assert(intel_ax211_assoc_begin_update(&state, &update, 74U, 11U,
	    script.now) == INTEL_AX211_ASSOC_OK);
	assert(accept_current(&state, script.now, NULL) ==
	    INTEL_AX211_ASSOC_PENDING);
	assert(intel_ax211_assoc_current(&state, script.now, &command) ==
	    INTEL_AX211_ASSOC_OK);
	assert(command.step == INTEL_AX211_ASSOC_STEP_LINK_ASSOCIATE);
	assert(get_le32(command.payload + 24U) == 0x1aU);
	assert(get_le32(command.payload + 140U) == 0U);
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
	test_auxiliary_codecs();
	test_profile_band_and_bounds();
	test_exact_mld_sequence();
	test_response_matching_and_queue_pointer();
	test_exact_reverse_rollback();
	test_uncertain_and_timeout_cleanup();
	test_callback_drive_and_failure();
	test_session_notifications();
	test_session_expiry_before_ack_and_probe_dtim();
	if (argc > 1)
		test_real_api89_table(argv[1]);
	puts("intel ax211 API89 MLD association session fixture: PASS");
	return 0;
}
