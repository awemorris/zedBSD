/*
 * Intel AX211 private API89 runtime/passive-scan fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-runtime.h"
#include "../../../src/drivers/intel-ax211-scan.h"

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
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
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
	put_version(bytes, 6U, 0x01U, 0x98U, 1U, 0U);
	put_version(bytes, 7U, 0x01U, 0x9bU, 6U, 0U);
	put_version(bytes, 8U, 0x02U, 0x01U, 2U, 0U);
	put_version(bytes, 9U, 0x01U, 0xeeU, 3U, 0U);
	put_version(bytes, 10U, 0x04U, 0x04U, 1U, 0U);
	put_version(bytes, 11U, 0x01U, 0x77U, 7U, 0U);
	put_version(bytes, 12U, 0x01U, 0xc8U, 1U, 6U);
	put_version(bytes, 13U, 0x01U, 0xd2U, 4U, 0U);
	put_version(bytes, 14U, 0x01U, 0x0eU, 1U, 0U);
	put_version(bytes,
	    INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U,
	    0U, 0U, 0U, 0U);
}

static void
set_bit(uint32_t *words, unsigned int bit)
{
	words[bit / 32U] |= UINT32_C(1) << (bit % 32U);
}

static void
make_manifest(struct intel_ax211_firmware_manifest *manifest)
{
	memset(manifest, 0, sizeof(*manifest));
	set_bit(manifest->api_changes,
	    INTEL_AX211_RUNTIME_API_WIFI_MCC_UPDATE);
	set_bit(manifest->api_changes,
	    INTEL_AX211_RUNTIME_API_REDUCED_SCAN_CONFIG);
	set_bit(manifest->api_changes,
	    INTEL_AX211_RUNTIME_API_SCAN_EXT_CHANNEL);
	set_bit(manifest->capabilities,
	    INTEL_AX211_RUNTIME_CAP_DS_PARAM_SET_IE);
	set_bit(manifest->capabilities,
	    INTEL_AX211_RUNTIME_CAP_LAR_MULTI_MCC);
	set_bit(manifest->capabilities,
	    INTEL_AX211_RUNTIME_CAP_CT_KILL_BY_FW);
	set_bit(manifest->capabilities,
	    INTEL_AX211_RUNTIME_CAP_MCC_UPDATE_11AX);
}

static void
make_nvm(struct intel_ax211_protocol_nvm *nvm)
{
	size_t index;

	memset(nvm, 0, sizeof(*nvm));
	nvm->band_24_enabled = 1U;
	nvm->tx_chain_mask = 3U;
	nvm->rx_chain_mask = 3U;
	nvm->lar_enabled = 1U;
	nvm->channel_24ghz_count = 14U;
	for (index = 0U; index < 14U; index++) {
		nvm->channel_24ghz[index].number = (uint8_t)(index + 1U);
		nvm->channel_24ghz[index].valid = 1U;
		nvm->channel_24ghz[index].active = index < 9U ? 1U : 0U;
	}
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

static void
test_runtime_profile_and_versions(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t malformed[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_firmware_manifest manifest;
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_nvm nvm;
	struct intel_ax211_runtime_profile profile;

	make_api89_table(bytes);
	table = parse_table(bytes);
	make_manifest(&manifest);
	make_nvm(&nvm);
	assert(intel_ax211_runtime_profile_from_manifest(&manifest, &nvm, 1,
	    &profile) == INTEL_AX211_RUNTIME_OK);
	assert(profile.tx_chain_mask == 3U && profile.rx_chain_mask == 3U);
	assert(profile.lar_enabled == 1U && profile.ltr_enabled == 1U);
	assert(intel_ax211_runtime_api89_validate(&table, &profile) ==
	    INTEL_AX211_RUNTIME_OK);

	memcpy(malformed, bytes, sizeof(malformed));
	put_version(malformed, 6U, 0x01U, 0x98U, 2U, 0U);
	table = parse_table(malformed);
	assert(intel_ax211_runtime_api89_validate(&table, &profile) ==
	    INTEL_AX211_RUNTIME_UNSUPPORTED);

	memcpy(malformed, bytes, sizeof(malformed));
	put_version(malformed, 15U, 0x05U, 0x00U, 1U, 0U);
	table = parse_table(malformed);
	assert(intel_ax211_runtime_api89_validate(&table, &profile) ==
	    INTEL_AX211_RUNTIME_UNSUPPORTED);

	make_manifest(&manifest);
	set_bit(manifest.capabilities, INTEL_AX211_RUNTIME_CAP_DQA);
	assert(intel_ax211_runtime_profile_from_manifest(&manifest, &nvm, 1,
	    &profile) == INTEL_AX211_RUNTIME_UNSUPPORTED);
	assert(intel_ax211_runtime_profile_from_manifest(NULL, &nvm, 1,
	    &profile) == INTEL_AX211_RUNTIME_INVALID);
}

static void
assert_zero(const uint8_t *bytes, size_t length)
{
	size_t index;

	for (index = 0U; index < length; index++)
		assert(bytes[index] == 0U);
}

static struct intel_ax211_runtime_profile
make_runtime_profile(int ltr, int lar)
{
	struct intel_ax211_firmware_manifest manifest;
	struct intel_ax211_protocol_nvm nvm;
	struct intel_ax211_runtime_profile profile;

	make_manifest(&manifest);
	make_nvm(&nvm);
	nvm.lar_enabled = lar ? 1U : 0U;
	assert(intel_ax211_runtime_profile_from_manifest(&manifest, &nvm, ltr,
	    &profile) == INTEL_AX211_RUNTIME_OK);
	return profile;
}

static void
test_runtime_codecs(void)
{
	struct intel_ax211_runtime_profile profile;
	struct intel_ax211_runtime_command command;

	profile = make_runtime_profile(1, 1);
	memset(&command, 0xa5, sizeof(command));
	assert(intel_ax211_runtime_command_encode(
	    INTEL_AX211_RUNTIME_STEP_TX_ANT, &profile, &command) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(command.group == 1U && command.opcode == 0x98U);
	assert(command.wire_version == 0U && command.layout_version == 1U);
	assert(command.payload_length == 4U && get_le32(command.payload) == 3U);

	assert(intel_ax211_runtime_command_encode(
	    INTEL_AX211_RUNTIME_STEP_BT_CONFIG, &profile, &command) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(command.payload_length == 8U && get_le32(command.payload) == 3U);
	assert_zero(command.payload + 4U, 4U);
	assert(intel_ax211_runtime_command_encode(
	    INTEL_AX211_RUNTIME_STEP_SOC_CONFIG, &profile, &command) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(command.group == 2U && get_le32(command.payload) == 1U);
	assert(get_le32(command.payload + 4U) == 0U);
	assert(intel_ax211_runtime_command_encode(
	    INTEL_AX211_RUNTIME_STEP_LTR_CONFIG, &profile, &command) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(command.payload_length == 32U && get_le32(command.payload) == 1U);
	assert_zero(command.payload + 4U, 28U);
	assert(intel_ax211_runtime_command_encode(
	    INTEL_AX211_RUNTIME_STEP_TEMP_REPORT, &profile, &command) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(command.group == 4U && command.payload_length == 20U);
	assert_zero(command.payload, 20U);
	assert(intel_ax211_runtime_command_encode(
	    INTEL_AX211_RUNTIME_STEP_POWER_TABLE, &profile, &command) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(command.payload_length == 4U);
	assert_zero(command.payload, 4U);
	assert(intel_ax211_runtime_command_encode(
	    INTEL_AX211_RUNTIME_STEP_MCC_UPDATE, &profile, &command) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(command.response_version == 6U && command.payload_length == 28U);
	assert(command.payload[0] == 'Z' && command.payload[1] == 'Z');
	assert(command.payload[2] == 0x10U);
	assert_zero(command.payload + 3U, 25U);
	assert(intel_ax211_runtime_command_encode(
	    INTEL_AX211_RUNTIME_STEP_SCAN_CONFIG, &profile, &command) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(command.opcode == 0x0cU && command.layout_version == 5U);
	assert(command.payload_length == 12U);
	assert(get_le32(command.payload + 4U) == 3U);
	assert(get_le32(command.payload + 8U) == 3U);
	assert(intel_ax211_runtime_command_encode(
	    INTEL_AX211_RUNTIME_STEP_BEACON_FILTER, &profile, &command) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(command.payload_length == 60U);
	assert_zero(command.payload, 60U);
}

static void
test_runtime_state(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_runtime_profile profile;
	struct intel_ax211_runtime_command command;
	struct intel_ax211_runtime_state state;
	int result;

	make_api89_table(bytes);
	table = parse_table(bytes);
	profile = make_runtime_profile(0, 0);
	assert(intel_ax211_runtime_begin(&state, &table, &profile, 7U, 100U) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(intel_ax211_runtime_current(&state, 100U, &command) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(command.opcode == 0x98U);
	assert(intel_ax211_runtime_ack(&state, 6U,
	    INTEL_AX211_RUNTIME_STEP_TX_ANT, 101U) ==
	    INTEL_AX211_RUNTIME_STALE);
	assert(intel_ax211_runtime_ack(&state, 7U,
	    INTEL_AX211_RUNTIME_STEP_SOC_CONFIG, 101U) ==
	    INTEL_AX211_RUNTIME_OUT_OF_ORDER);
	assert(intel_ax211_runtime_ack(&state, 7U,
	    INTEL_AX211_RUNTIME_STEP_TX_ANT, 101U) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(intel_ax211_runtime_ack(&state, 7U,
	    INTEL_AX211_RUNTIME_STEP_TX_ANT, 102U) ==
	    INTEL_AX211_RUNTIME_DUPLICATE);
	assert(intel_ax211_runtime_ack(&state, 7U,
	    INTEL_AX211_RUNTIME_STEP_BT_CONFIG, 102U) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(intel_ax211_runtime_ack(&state, 7U,
	    INTEL_AX211_RUNTIME_STEP_SOC_CONFIG, 103U) ==
	    INTEL_AX211_RUNTIME_OK);
	/* LTR and MCC are skipped by this profile. */
	assert(state.step == INTEL_AX211_RUNTIME_STEP_TEMP_REPORT);
	assert(intel_ax211_runtime_ack(&state, 7U,
	    INTEL_AX211_RUNTIME_STEP_TEMP_REPORT, 104U) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(intel_ax211_runtime_ack(&state, 7U,
	    INTEL_AX211_RUNTIME_STEP_POWER_TABLE, 105U) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(state.step == INTEL_AX211_RUNTIME_STEP_SCAN_CONFIG);
	assert(intel_ax211_runtime_ack(&state, 7U,
	    INTEL_AX211_RUNTIME_STEP_SCAN_CONFIG, 106U) ==
	    INTEL_AX211_RUNTIME_OK);
	result = intel_ax211_runtime_ack(&state, 7U,
	    INTEL_AX211_RUNTIME_STEP_BEACON_FILTER, 107U);
	assert(result == INTEL_AX211_RUNTIME_COMPLETE);
	assert(intel_ax211_runtime_ack(&state, 7U,
	    INTEL_AX211_RUNTIME_STEP_BEACON_FILTER, 108U) ==
	    INTEL_AX211_RUNTIME_DUPLICATE);

	assert(intel_ax211_runtime_begin(&state, &table, &profile, 8U, 100U) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(intel_ax211_runtime_expire(&state, 1000099U) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(intel_ax211_runtime_expire(&state, 1000100U) ==
	    INTEL_AX211_RUNTIME_TIMEOUT);
	assert(intel_ax211_runtime_current(&state, 1000100U, &command) ==
	    INTEL_AX211_RUNTIME_INVALID);
}

static void
test_mcc_decode(void)
{
	uint8_t payload[28];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_runtime_mcc mcc;

	memset(payload, 0, sizeof(payload));
	put_le32(payload, 1U);
	payload[4U] = 'Z';
	payload[5U] = 'Z';
	payload[12U] = 0x10U;
	put_le32(payload + 16U, 2U);
	put_le32(payload + 20U, 0x11U);
	put_le32(payload + 24U, 0x22U);
	memset(&message, 0, sizeof(message));
	message.group = 1U;
	message.opcode = 0xc8U;
	message.version = 6U;
	message.generation = 33U;
	message.payload = payload;
	message.payload_length = sizeof(payload);
	assert(intel_ax211_runtime_mcc_decode(&message, 33U, &mcc) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(mcc.status == 1U && mcc.mcc == 0x5a5aU);
	assert(mcc.channel_count == 2U && mcc.channel[1] == 0x22U);
	assert(intel_ax211_runtime_mcc_decode(&message, 32U, &mcc) ==
	    INTEL_AX211_RUNTIME_STALE);
	message.payload_length--;
	assert(intel_ax211_runtime_mcc_decode(&message, 33U, &mcc) ==
	    INTEL_AX211_RUNTIME_TRUNCATED);
	message.payload_length += 2U;
	assert(intel_ax211_runtime_mcc_decode(&message, 33U, &mcc) ==
	    INTEL_AX211_RUNTIME_OVERSIZED);
	message.payload_length = sizeof(payload);
	payload[13U] = 1U;
	assert(intel_ax211_runtime_mcc_decode(&message, 33U, &mcc) ==
	    INTEL_AX211_RUNTIME_UNSUPPORTED);
	payload[13U] = 0U;
	put_le32(payload, 2U);
	assert(intel_ax211_runtime_mcc_decode(&message, 33U, &mcc) ==
	    INTEL_AX211_RUNTIME_FAILED);
	put_le32(payload, 1U);
	put_le32(payload + 16U, 0U);
	assert(intel_ax211_runtime_mcc_decode(&message, 33U, &mcc) ==
	    INTEL_AX211_RUNTIME_UNSUPPORTED);
}

static struct intel_ax211_scan_profile
make_scan_profile(void)
{
	static const uint8_t station[6] = { 0x02U, 0x11U, 0x22U,
	    0x33U, 0x44U, 0x55U };
	struct intel_ax211_protocol_nvm nvm;
	struct intel_ax211_runtime_mcc mcc;
	struct intel_ax211_scan_profile profile;
	size_t index;

	make_nvm(&nvm);
	memset(&mcc, 0, sizeof(mcc));
	mcc.channel_count = 14U;
	for (index = 0U; index < mcc.channel_count; index++)
		mcc.channel[index] = INTEL_AX211_PROTOCOL_NVM_CHANNEL_VALID;
	assert(intel_ax211_scan_profile_from_nvm(&nvm, &mcc, station,
	    &profile) ==
	    INTEL_AX211_SCAN_OK);
	assert(profile.channel_count == 11U);
	assert(profile.channel_width_mhz == 20U);
	assert(profile.channel[0] == 1U && profile.channel[10] == 11U);
	return profile;
}

static void
test_scan_profile_bounds(void)
{
	static const uint8_t station[6] = { 0x02U, 0x11U, 0x22U,
	    0x33U, 0x44U, 0x55U };
	struct intel_ax211_protocol_nvm nvm;
	struct intel_ax211_runtime_mcc mcc;
	struct intel_ax211_scan_profile profile;
	size_t index;

	make_nvm(&nvm);
	memset(&mcc, 0, sizeof(mcc));
	mcc.channel_count = 14U;
	for (index = 0U; index < mcc.channel_count; index++)
		mcc.channel[index] = INTEL_AX211_PROTOCOL_NVM_CHANNEL_VALID;
	nvm.channel_24ghz_count =
	    INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT + 1U;
	assert(intel_ax211_scan_profile_from_nvm(&nvm, &mcc, station,
	    &profile) ==
	    INTEL_AX211_SCAN_INVALID);
	make_nvm(&nvm);
	nvm.band_24_enabled = 0U;
	assert(intel_ax211_scan_profile_from_nvm(&nvm, &mcc, station,
	    &profile) ==
	    INTEL_AX211_SCAN_INVALID);
	make_nvm(&nvm);
	nvm.channel_24ghz[0].valid = 0U;
	nvm.channel_24ghz_count = 1U;
	assert(intel_ax211_scan_profile_from_nvm(&nvm, &mcc, station,
	    &profile) ==
	    INTEL_AX211_SCAN_UNSUPPORTED);
	make_nvm(&nvm);
	assert(intel_ax211_scan_profile_from_nvm(&nvm, NULL, station,
	    &profile) == INTEL_AX211_SCAN_INVALID);
	nvm.lar_enabled = 0U;
	assert(intel_ax211_scan_profile_from_nvm(&nvm, NULL, station,
	    &profile) == INTEL_AX211_SCAN_OK);
	make_nvm(&nvm);
	mcc.channel[4U] = 0U;
	assert(intel_ax211_scan_profile_from_nvm(&nvm, &mcc, station,
	    &profile) == INTEL_AX211_SCAN_OK);
	assert(profile.channel_count == 10U && profile.channel[4U] == 6U);
}

static void
test_scan_codec(void)
{
	struct intel_ax211_scan_profile profile;
	uint8_t request[INTEL_AX211_SCAN_REQUEST_SIZE + 1U];
	uint8_t abort[INTEL_AX211_SCAN_ABORT_SIZE];
	const uint8_t *probe;
	size_t index;

	profile = make_scan_profile();
	memset(request, 0xa5, sizeof(request));
	assert(intel_ax211_scan_request_encode(&profile, request) ==
	    INTEL_AX211_SCAN_OK);
	assert(request[INTEL_AX211_SCAN_REQUEST_SIZE] == 0xa5U);
	assert(get_le32(request) == 0U && get_le32(request + 4U) == 6U);
	assert(get_le16(request + 8U) == 0x0886U);
	assert(request[12U] == 10U && request[13U] == 10U);
	assert(get_le16(request + 18U) == 300U);
	assert(request[40U] == 110U && request[41U] == 110U);
	assert(request[44U] == 0x20U && request[45U] == 11U);
	assert(request[46U] == 10U && request[47U] == 2U);
	for (index = 0U; index < 11U; index++) {
		const uint8_t *channel = request + 48U + index * 8U;

		assert(get_le32(channel) == 0x40000000U);
		assert(channel[4U] == index + 1U);
		assert(channel[5U] == 0x80U && channel[6U] == 1U &&
		    channel[7U] == 0U);
	}
	assert(request[586U] == 1U);
	probe = request + 596U;
	assert(get_le16(probe) == 0U && get_le16(probe + 2U) == 26U);
	assert(get_le16(probe + 4U) == 26U &&
	    get_le16(probe + 6U) == 19U);
	assert(get_le16(probe + 16U) == 45U &&
	    get_le16(probe + 18U) == 0U);
	assert(probe[20U] == 0x40U);
	assert(memcmp(probe + 30U, profile.station_address, 6U) == 0);
	assert(probe[44U] == 0U && probe[45U] == 0U);
	assert(probe[46U] == 1U && probe[47U] == 8U);
	assert(probe[62U] == 3U && probe[63U] == 1U && probe[64U] == 0U);
	assert(intel_ax211_scan_abort_encode(abort) == INTEL_AX211_SCAN_OK);
	assert_zero(abort, sizeof(abort));
	assert(intel_ax211_scan_request_encode(NULL, request) ==
	    INTEL_AX211_SCAN_INVALID);
	profile.channel_width_mhz = 40U;
	assert(intel_ax211_scan_request_encode(&profile, request) ==
	    INTEL_AX211_SCAN_INVALID);
}

static struct intel_ax211_protocol_message
make_scan_message(uint8_t opcode, uint32_t generation,
	uint8_t *payload, size_t length)
{
	struct intel_ax211_protocol_message message;

	memset(&message, 0, sizeof(message));
	message.group = INTEL_AX211_SCAN_GROUP_LEGACY;
	message.opcode = opcode;
	message.version = INTEL_AX211_SCAN_NOTIFICATION_VERSION;
	message.generation = generation;
	message.payload = payload;
	message.payload_length = length;
	return message;
}

static void
test_scan_state_and_events(void)
{
	uint8_t table_bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t iteration[16U + 3U * 8U];
	uint8_t complete[16U];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_scan_profile profile;
	struct intel_ax211_scan_event event;
	struct intel_ax211_scan_state state;
	int result;

	make_api89_table(table_bytes);
	table = parse_table(table_bytes);
	profile = make_scan_profile();
	assert(intel_ax211_scan_api89_validate(&table) == INTEL_AX211_SCAN_OK);
	assert(intel_ax211_scan_begin(&state, &table, &profile, 91U, 100U) ==
	    INTEL_AX211_SCAN_OK);
	assert(state.phase == INTEL_AX211_SCAN_PHASE_WAIT_ACK);
	assert(intel_ax211_scan_request_ack(&state, 90U, 101U) ==
	    INTEL_AX211_SCAN_STALE);
	assert(intel_ax211_scan_request_ack(&state, 91U, 101U) ==
	    INTEL_AX211_SCAN_OK);
	assert(intel_ax211_scan_request_ack(&state, 91U, 102U) ==
	    INTEL_AX211_SCAN_DUPLICATE);

	memset(iteration, 0, sizeof(iteration));
	iteration[4U] = 3U;
	iteration[5U] = 1U;
	iteration[7U] = 3U;
	put_le32(iteration + 8U, 0x55667788U);
	put_le32(iteration + 12U, 0x11223344U);
	iteration[16U] = 1U;
	iteration[17U] = 1U;
	put_le32(iteration + 20U, 100U);
	iteration[24U] = 2U;
	iteration[25U] = 1U;
	put_le32(iteration + 28U, 101U);
	iteration[32U] = 3U;
	iteration[33U] = 1U;
	put_le32(iteration + 36U, 102U);
	message = make_scan_message(
	    INTEL_AX211_SCAN_ITERATION_COMPLETE_OPCODE, 90U,
	    iteration, sizeof(iteration));
	assert(intel_ax211_scan_event_accept(&state, &message, 200U, &event) ==
	    INTEL_AX211_SCAN_STALE);
	message.generation = 91U;
	result = intel_ax211_scan_event_accept(&state, &message, 200U, &event);
	assert(result == INTEL_AX211_SCAN_COMPLETE);
	assert(event.kind == INTEL_AX211_SCAN_EVENT_ITERATION_COMPLETE);
	assert(event.channel_count == 3U && event.channel[2].channel == 3U);
	assert(event.tsf == UINT64_C(0x1122334455667788));
	assert(state.phase == INTEL_AX211_SCAN_PHASE_TERMINAL);
	assert(intel_ax211_scan_event_accept(&state, &message, 201U, &event) ==
	    INTEL_AX211_SCAN_DUPLICATE);

	assert(intel_ax211_scan_begin(&state, &table, &profile, 92U, 0U) ==
	    INTEL_AX211_SCAN_OK);
	assert(intel_ax211_scan_request_ack(&state, 92U, 1U) ==
	    INTEL_AX211_SCAN_OK);
	memset(complete, 0, sizeof(complete));
	complete[6U] = 2U;
	message = make_scan_message(INTEL_AX211_SCAN_COMPLETE_OPCODE, 92U,
	    complete, sizeof(complete));
	assert(intel_ax211_scan_event_accept(&state, &message, 2U, &event) ==
	    INTEL_AX211_SCAN_ABORTED);
	assert(event.kind == INTEL_AX211_SCAN_EVENT_COMPLETE);

	assert(intel_ax211_scan_begin(&state, &table, &profile, 93U, 0U) ==
	    INTEL_AX211_SCAN_OK);
	assert(intel_ax211_scan_expire(&state, 999999U) ==
	    INTEL_AX211_SCAN_OK);
	assert(intel_ax211_scan_expire(&state, 1000000U) ==
	    INTEL_AX211_SCAN_TIMEOUT);
	assert(state.abort_required == 0U);
	assert(intel_ax211_scan_begin(&state, &table, &profile, 94U, 0U) ==
	    INTEL_AX211_SCAN_OK);
	assert(intel_ax211_scan_request_ack(&state, 94U, 1U) ==
	    INTEL_AX211_SCAN_OK);
	assert(intel_ax211_scan_expire(&state, 4999999U) ==
	    INTEL_AX211_SCAN_OK);
	assert(intel_ax211_scan_expire(&state, 5000000U) ==
	    INTEL_AX211_SCAN_TIMEOUT);
	assert(state.abort_required == 1U);
}

static void
test_scan_malformed_events(void)
{
	uint8_t table_bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t iteration[24U];
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_scan_profile profile;
	struct intel_ax211_scan_event event;
	struct intel_ax211_scan_state state;

	make_api89_table(table_bytes);
	table = parse_table(table_bytes);
	profile = make_scan_profile();
	memset(iteration, 0, sizeof(iteration));
	iteration[4U] = 1U;
	iteration[5U] = 1U;
	iteration[7U] = 1U;
	iteration[16U] = 1U;
	iteration[17U] = 1U;
	message = make_scan_message(
	    INTEL_AX211_SCAN_ITERATION_COMPLETE_OPCODE, 1U,
	    iteration, sizeof(iteration));
	assert(intel_ax211_scan_begin(&state, &table, &profile, 1U, 0U) ==
	    INTEL_AX211_SCAN_OK);
	assert(intel_ax211_scan_event_accept(&state, &message, 1U, &event) ==
	    INTEL_AX211_SCAN_OUT_OF_ORDER);
	assert(intel_ax211_scan_request_ack(&state, 1U, 1U) ==
	    INTEL_AX211_SCAN_OK);
	message.payload_length--;
	assert(intel_ax211_scan_event_accept(&state, &message, 2U, &event) ==
	    INTEL_AX211_SCAN_TRUNCATED);
	message.payload_length += 2U;
	assert(intel_ax211_scan_event_accept(&state, &message, 2U, &event) ==
	    INTEL_AX211_SCAN_OVERSIZED);
	message.payload_length = sizeof(iteration);
	iteration[17U] = 0U;
	assert(intel_ax211_scan_event_accept(&state, &message, 2U, &event) ==
	    INTEL_AX211_SCAN_UNSUPPORTED);
	iteration[17U] = 1U;
	iteration[16U] = 12U;
	assert(intel_ax211_scan_event_accept(&state, &message, 2U, &event) ==
	    INTEL_AX211_SCAN_UNSUPPORTED);
	iteration[16U] = 1U;
	message.version = 2U;
	assert(intel_ax211_scan_event_accept(&state, &message, 2U, &event) ==
	    INTEL_AX211_SCAN_UNSUPPORTED);
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
	struct intel_ax211_runtime_profile profile;

	firmware = read_file(path, &firmware_length);
	assert(find_command_table(firmware, firmware_length, &table_bytes,
	    &table_length));
	assert(table_length == INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES);
	assert(intel_ax211_protocol_command_table_parse(table_bytes,
	    table_length, &table) == INTEL_AX211_PROTOCOL_OK);
	profile = make_runtime_profile(1, 1);
	assert(intel_ax211_runtime_api89_validate(&table, &profile) ==
	    INTEL_AX211_RUNTIME_OK);
	assert(intel_ax211_scan_api89_validate(&table) == INTEL_AX211_SCAN_OK);
	free(firmware);
}

int
main(int argc, char **argv)
{
	test_runtime_profile_and_versions();
	test_runtime_codecs();
	test_runtime_state();
	test_mcc_decode();
	test_scan_profile_bounds();
	test_scan_codec();
	test_scan_state_and_events();
	test_scan_malformed_events();
	if (argc > 1)
		test_real_api89_table(argv[1]);
	puts("intel ax211 runtime/scan fixture: PASS");
	return 0;
}
