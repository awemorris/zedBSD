/* Intel AX211 private API89 CCMP key fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-key.h"

static uint32_t
get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0U] | ((uint32_t)bytes[1U] << 8) |
	    ((uint32_t)bytes[2U] << 16) | ((uint32_t)bytes[3U] << 24);
}

static uint64_t
get_le64(const uint8_t *bytes)
{
	uint64_t value;
	unsigned index;

	value = 0U;
	for (index = 0U; index < 8U; index++)
		value |= (uint64_t)bytes[index] << (index * 8U);
	return value;
}

static struct intel_ax211_key_request
make_request(enum intel_ax211_key_kind kind, uint8_t index,
	uint64_t generation, uint64_t receive_packet_number)
{
	struct intel_ax211_key_request request;
	unsigned byte;

	memset(&request, 0, sizeof(request));
	request.connection_generation = 7U;
	request.key_generation = generation;
	request.receive_packet_number = receive_packet_number;
	request.kind = kind;
	request.key_index = index;
	for (byte = 0U; byte < sizeof(request.key); byte++)
		request.key[byte] = (uint8_t)(byte + generation);
	return request;
}

static void
test_version(void)
{
	uint8_t bytes[8U] = {
		INTEL_AX211_KEY_OPCODE, INTEL_AX211_KEY_GROUP,
		INTEL_AX211_KEY_COMMAND_VERSION,
		INTEL_AX211_KEY_RESPONSE_VERSION,
		0U, 0U, 0U, 0U
	};
	struct intel_ax211_protocol_command_table table;

	/* SEC_KEY_CMD uses table layout v1 but API89's wide header version 0. */
	assert(INTEL_AX211_KEY_WIRE_VERSION == 0U);
	assert(intel_ax211_protocol_command_table_parse(bytes, sizeof(bytes),
	    &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_key_api89_validate(&table) == INTEL_AX211_KEY_OK);
	bytes[2U]++;
	assert(intel_ax211_protocol_command_table_parse(bytes, sizeof(bytes),
	    &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_key_api89_validate(&table) ==
	    INTEL_AX211_KEY_UNSUPPORTED);
	assert(intel_ax211_key_api89_validate(NULL) == INTEL_AX211_KEY_INVALID);
}

static void
test_codec_and_scrub(void)
{
	struct intel_ax211_key_request pairwise;
	struct intel_ax211_key_request group;
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE];
	uint8_t zeros[INTEL_AX211_KEY_COMMAND_SIZE];

	pairwise = make_request(INTEL_AX211_KEY_PAIRWISE, 0U, 11U, 0U);
	assert(intel_ax211_key_add_encode(&pairwise, command) ==
	    INTEL_AX211_KEY_OK);
	assert(get_le32(command) == 1U && get_le32(command + 4U) == 1U);
	assert(get_le32(command + 8U) == 0U);
	assert(get_le32(command + 12U) == 2U);
	assert(memcmp(command + 16U, pairwise.key, sizeof(pairwise.key)) == 0);
	assert(get_le64(command + 64U) == 0U);
	assert(get_le64(command + 72U) == 0U);

	group = make_request(INTEL_AX211_KEY_GROUP_KEY, 2U, 12U,
	    UINT64_C(0x0000060504030201));
	assert(intel_ax211_key_add_encode(&group, command) ==
	    INTEL_AX211_KEY_OK);
	assert(get_le32(command + 8U) == 2U);
	assert(get_le32(command + 12U) == 0x42U);
	assert(get_le64(command + 64U) == group.receive_packet_number);
	assert(intel_ax211_key_remove_encode(7U, 12U,
	    INTEL_AX211_KEY_GROUP_KEY, 2U, command) == INTEL_AX211_KEY_OK);
	assert(get_le32(command) == 3U && get_le32(command + 4U) == 1U);
	assert(get_le32(command + 8U) == 2U);
	assert(get_le32(command + 12U) == 0x42U);
	memset(zeros, 0, sizeof(zeros));
	intel_ax211_key_command_scrub(command);
	assert(memcmp(command, zeros, sizeof(command)) == 0);
}

static void
test_state(void)
{
	struct intel_ax211_key_request pairwise;
	struct intel_ax211_key_request group;
	struct intel_ax211_key_state state;
	uint64_t generation;

	pairwise = make_request(INTEL_AX211_KEY_PAIRWISE, 0U, 11U, 0U);
	group = make_request(INTEL_AX211_KEY_GROUP_KEY, 2U, 12U, 9U);
	assert(intel_ax211_key_state_init(&state, 3U, 7U) ==
	    INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_installed(&state, &pairwise, 3U) ==
	    INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_installed(&state, &pairwise, 3U) ==
	    INTEL_AX211_KEY_DUPLICATE);
	assert(intel_ax211_key_state_installed(&state, &group, 4U) ==
	    INTEL_AX211_KEY_STALE);
	assert(intel_ax211_key_state_activate(&state, 7U, 11U, 12U, 3U) ==
	    INTEL_AX211_KEY_MISSING);
	assert(intel_ax211_key_state_installed(&state, &group, 3U) ==
	    INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_activate(&state, 7U, 11U, 12U, 3U) ==
	    INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_activate(&state, 7U, 11U, 12U, 3U) ==
	    INTEL_AX211_KEY_DUPLICATE);
	/* Group-only and pairwise-only rekeys preserve the already active peer. */
	group = make_request(INTEL_AX211_KEY_GROUP_KEY, 1U, 13U, 10U);
	assert(intel_ax211_key_state_installed(&state, &group, 3U) ==
	    INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_activate(&state, 7U, 11U, 13U, 3U) ==
	    INTEL_AX211_KEY_OK);
	pairwise = make_request(INTEL_AX211_KEY_PAIRWISE, 0U, 14U, 0U);
	assert(intel_ax211_key_state_installed(&state, &pairwise, 3U) ==
	    INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_activate(&state, 7U, 14U, 13U, 3U) ==
	    INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_tx_validate(&state, 7U, 11U, 0U, 1U,
	    3U) == INTEL_AX211_KEY_STALE);
	assert(intel_ax211_key_state_tx_validate(&state, 7U, 14U, 0U, 1U,
	    3U) == INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_tx_validate(&state, 7U, 10U, 0U, 1U,
	    3U) == INTEL_AX211_KEY_STALE);
	assert(intel_ax211_key_state_rx_generation(&state, 7U,
	    INTEL_AX211_KEY_GROUP_KEY, 1U, 3U, &generation) ==
	    INTEL_AX211_KEY_OK);
	assert(generation == 13U);
	assert(intel_ax211_key_state_rx_generation(&state, 7U,
	    INTEL_AX211_KEY_GROUP_KEY, 2U, 3U, &generation) ==
	    INTEL_AX211_KEY_MISSING);
	assert(intel_ax211_key_state_removed(&state, 7U,
	    INTEL_AX211_KEY_GROUP_KEY, 1U, 13U, 3U) ==
	    INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_rx_generation(&state, 7U,
	    INTEL_AX211_KEY_GROUP_KEY, 1U, 3U, &generation) ==
	    INTEL_AX211_KEY_MISSING);
	assert(intel_ax211_key_state_removed(&state, 7U,
	    INTEL_AX211_KEY_PAIRWISE, 0U, 14U, 3U) ==
	    INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_tx_validate(&state, 7U, 11U, 0U, 2U,
	    3U) == INTEL_AX211_KEY_STALE);
}

static void
test_invalid(void)
{
	struct intel_ax211_key_request request;
	struct intel_ax211_key_state state;
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE];

	request = make_request(INTEL_AX211_KEY_PAIRWISE, 1U, 3U, 0U);
	assert(intel_ax211_key_add_encode(&request, command) ==
	    INTEL_AX211_KEY_INVALID);
	request = make_request(INTEL_AX211_KEY_GROUP_KEY, 1U, 3U,
	    UINT64_C(0x0001000000000000));
	assert(intel_ax211_key_add_encode(&request, command) ==
	    INTEL_AX211_KEY_INVALID);
	assert(intel_ax211_key_remove_encode(0U, 1U,
	    INTEL_AX211_KEY_PAIRWISE, 0U, command) ==
	    INTEL_AX211_KEY_INVALID);
	assert(intel_ax211_key_state_init(&state, 0U, 1U) ==
	    INTEL_AX211_KEY_INVALID);
}

int
main(void)
{
	test_version();
	test_codec_and_scrub();
	test_state();
	test_invalid();
	puts("intel ax211 key tests passed");
	return 0;
}
