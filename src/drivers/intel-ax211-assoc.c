/*
 * zedBSD Intel AX211 private API89 association-session implementation
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
 *
 * SPDX-License-Identifier: ISC AND BSD-3-Clause
 */

#include "intel-ax211-assoc.h"

#include <string.h>

#define AX211_ASSOC_RESOURCE_PHY                         0x01U
#define AX211_ASSOC_RESOURCE_MAC                         0x02U
#define AX211_ASSOC_RESOURCE_BINDING                     0x04U
#define AX211_ASSOC_RESOURCE_STATION                     0x08U
#define AX211_ASSOC_RESOURCE_QUEUE                       0x10U
#define AX211_ASSOC_RESOURCE_SESSION                     0x20U

#define AX211_ASSOC_ACTION_ADD                              1U
#define AX211_ASSOC_ACTION_MODIFY                           2U
#define AX211_ASSOC_ACTION_REMOVE                           3U
#define AX211_ASSOC_INVALID_CONTEXT                0xffffffffU
#define AX211_ASSOC_PHY_BAND_24                             1U
#define AX211_ASSOC_MAC_TYPE_BSS_STATION                    5U
#define AX211_ASSOC_MAC_FILTER_ACCEPT_GROUP              0x04U
#define AX211_ASSOC_MAC_FILTER_BEACON                    0x40U
#define AX211_ASSOC_MAC_SHORT_SLOT                       0x10U
#define AX211_ASSOC_MAC_SHORT_PREAMBLE                   0x20U
#define AX211_ASSOC_MAC_QOS_UPDATE_EDCA                  0x01U
#define AX211_ASSOC_STATION_FLAGS_MASK             0x3c000000U
#define AX211_ASSOC_STATION_TYPE_LINK                       0U
#define AX211_ASSOC_STATION_STATUS_SUCCESS                  1U
#define AX211_ASSOC_QUEUE_OPERATION_ADD                     0U
#define AX211_ASSOC_QUEUE_OPERATION_REMOVE                  1U
#define AX211_ASSOC_QUEUE_ID                                1U
#define AX211_ASSOC_MANAGEMENT_TID                         15U
#define AX211_ASSOC_QUEUE_CB_SIZE                           5U
#define AX211_ASSOC_SESSION_CONFIGURATION_ASSOC             0U

static void ax211_assoc_put_le16(uint8_t *bytes, uint16_t value);
static void ax211_assoc_put_le32(uint8_t *bytes, uint32_t value);
static void ax211_assoc_put_le64(uint8_t *bytes, uint64_t value);
static uint16_t ax211_assoc_get_le16(const uint8_t *bytes);
static uint32_t ax211_assoc_get_le32(const uint8_t *bytes);
static int ax211_assoc_address_valid(const uint8_t address[6]);
static int ax211_assoc_profile_valid(
	const struct intel_ax211_assoc_profile *profile);
static int ax211_assoc_update_valid(
	const struct intel_ax211_assoc_profile *profile,
	const struct intel_ax211_assoc_update *update);
static int ax211_assoc_required_version(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group, uint8_t opcode, uint8_t command_version,
	uint8_t notification_version);
static uint32_t ax211_assoc_next_sequence(uint32_t sequence);
static int ax211_assoc_set_step(struct intel_ax211_assoc_state *state,
	enum intel_ax211_assoc_step step, uint64_t now_us);
static void ax211_assoc_command_base(
	const struct intel_ax211_assoc_state *state,
	struct intel_ax211_assoc_command *command);
static int ax211_assoc_command_encode(
	const struct intel_ax211_assoc_state *state,
	struct intel_ax211_assoc_command *command);
static int ax211_assoc_command_matches(
	const struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_command *command);
static void ax211_assoc_phy_encode(
	const struct intel_ax211_assoc_state *state, uint32_t action,
	struct intel_ax211_assoc_command *command);
static void ax211_assoc_rlc_encode(
	const struct intel_ax211_assoc_state *state,
	struct intel_ax211_assoc_command *command);
static void ax211_assoc_mac_encode(
	const struct intel_ax211_assoc_state *state, uint32_t action,
	int associated, struct intel_ax211_assoc_command *command);
static void ax211_assoc_binding_encode(uint32_t action,
	struct intel_ax211_assoc_command *command);
static void ax211_assoc_station_encode(
	const struct intel_ax211_assoc_state *state, int update,
	struct intel_ax211_assoc_command *command);
static void ax211_assoc_station_remove_encode(
	struct intel_ax211_assoc_command *command);
static void ax211_assoc_queue_encode(
	const struct intel_ax211_assoc_state *state, uint32_t operation,
	struct intel_ax211_assoc_command *command);
static void ax211_assoc_session_encode(
	const struct intel_ax211_assoc_state *state, uint32_t action,
	struct intel_ax211_assoc_command *command);
static int ax211_assoc_response_validate(
	const struct intel_ax211_assoc_command *command,
	const struct intel_ax211_assoc_reply *reply);
static void ax211_assoc_mark_success(struct intel_ax211_assoc_state *state,
	enum intel_ax211_assoc_step step);
static void ax211_assoc_mark_uncertain(
	struct intel_ax211_assoc_state *state,
	enum intel_ax211_assoc_step step);
static enum intel_ax211_assoc_step ax211_assoc_rollback_step(
	uint32_t resources);
static int ax211_assoc_rollback_begin(
	struct intel_ax211_assoc_state *state, int failure, uint64_t now_us);
static int ax211_assoc_advance(struct intel_ax211_assoc_state *state,
	uint64_t now_us);
static int ax211_assoc_exchange_failure(
	struct intel_ax211_assoc_state *state, int result, uint64_t now_us);

static void
ax211_assoc_put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
ax211_assoc_put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void
ax211_assoc_put_le64(uint8_t *bytes, uint64_t value)
{
	ax211_assoc_put_le32(bytes, (uint32_t)value);
	ax211_assoc_put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

static uint16_t
ax211_assoc_get_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

static uint32_t
ax211_assoc_get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int
ax211_assoc_address_valid(const uint8_t address[6])
{
	size_t index;
	int any;

	if (address == NULL || (address[0] & 1U) != 0U)
		return 0;
	any = 0;
	for (index = 0U; index < 6U; index++) {
		if (address[index] != 0U)
			any = 1;
	}
	return any;
}

static int
ax211_assoc_profile_valid(const struct intel_ax211_assoc_profile *profile)
{
	size_t index;

	if (profile == NULL ||
	    !ax211_assoc_address_valid(profile->station_address) ||
	    !ax211_assoc_address_valid(profile->bssid) ||
	    memcmp(profile->station_address, profile->bssid, 6U) == 0 ||
	    profile->channel == 0U || profile->channel > 14U ||
	    profile->channel_width_mhz !=
	    INTEL_AX211_ASSOC_CHANNEL_WIDTH_MHZ ||
	    profile->rx_chain_mask == 0U || profile->rx_chain_mask > 7U ||
	    (profile->cck_ack_rates | profile->ofdm_ack_rates) == 0U ||
	    profile->short_preamble > 1U || profile->short_slot > 1U ||
	    profile->qos > 1U || profile->beacon_interval_tu == 0U ||
	    profile->queue_byte_count_address == 0U ||
	    profile->queue_descriptor_address == 0U ||
	    (profile->queue_byte_count_address & UINT64_C(3)) != 0U ||
	    (profile->queue_descriptor_address & UINT64_C(255)) != 0U)
		return 0;
	for (index = 0U; index < INTEL_AX211_ASSOC_EDCA_COUNT; index++) {
		const struct intel_ax211_assoc_edca *edca;

		edca = &profile->edca[index];
		if (edca->ecw_min > 15U || edca->ecw_max > 15U ||
		    edca->ecw_min > edca->ecw_max || edca->aifsn > 15U ||
		    edca->txop_32us > 2047U)
			return 0;
	}
	return 1;
}

static int
ax211_assoc_update_valid(const struct intel_ax211_assoc_profile *profile,
	const struct intel_ax211_assoc_update *update)
{
	uint64_t dtim_offset;
	uint32_t interval;

	if (profile == NULL || update == NULL ||
	    update->association_id == 0U || update->association_id > 2007U)
		return 0;
	/* A probe response need not carry TIM.  OpenBSD deliberately permits
	 * association without DTIM knowledge and keeps beacon delivery enabled. */
	if (update->dtim_period == 0U)
		return update->dtim_count == 0U;
	if (update->dtim_count >= update->dtim_period)
		return 0;
	interval = (uint32_t)profile->beacon_interval_tu *
	    (uint32_t)update->dtim_period;
	dtim_offset = (uint64_t)update->dtim_count *
	    (uint64_t)profile->beacon_interval_tu * UINT64_C(1024);
	return interval >= profile->beacon_interval_tu &&
	    dtim_offset <= UINT32_MAX;
}

static int
ax211_assoc_required_version(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group, uint8_t opcode, uint8_t command_version,
	uint8_t notification_version)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	result = intel_ax211_protocol_command_version_lookup(table, group,
	    opcode, &version);
	if (result != INTEL_AX211_PROTOCOL_OK ||
	    version.command_version != command_version ||
	    version.notification_version != notification_version)
		return INTEL_AX211_ASSOC_UNSUPPORTED;
	return INTEL_AX211_ASSOC_OK;
}

int
intel_ax211_assoc_mcast_filter_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	if (table == NULL)
		return INTEL_AX211_ASSOC_INVALID;
	if (intel_ax211_protocol_command_table_validate_api89(table) !=
	    INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_ASSOC_UNSUPPORTED;
	return ax211_assoc_required_version(table,
	    INTEL_AX211_ASSOC_GROUP_LONG,
	    INTEL_AX211_ASSOC_MCAST_FILTER_OPCODE,
	    INTEL_AX211_ASSOC_MCAST_FILTER_VERSION, 0U);
}

int
intel_ax211_assoc_mcast_filter_encode(
	const uint8_t bssid[6],
	uint8_t *output,
	size_t output_capacity)
{
	if (bssid == NULL || output == NULL ||
	    !ax211_assoc_address_valid(bssid))
		return INTEL_AX211_ASSOC_INVALID;
	if (output_capacity < INTEL_AX211_ASSOC_MCAST_FILTER_SIZE)
		return INTEL_AX211_ASSOC_BUFFER_TOO_SMALL;
	memset(output, 0, INTEL_AX211_ASSOC_MCAST_FILTER_SIZE);
	output[0U] = 1U;
	output[3U] = 1U;
	memcpy(output + 4U, bssid, 6U);
	return INTEL_AX211_ASSOC_OK;
}

int
intel_ax211_assoc_mac_power_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	if (table == NULL)
		return INTEL_AX211_ASSOC_INVALID;
	if (intel_ax211_protocol_command_table_validate_api89(table) !=
	    INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_ASSOC_UNSUPPORTED;
	return ax211_assoc_required_version(table,
	    INTEL_AX211_ASSOC_GROUP_LONG,
	    INTEL_AX211_ASSOC_MAC_POWER_OPCODE,
	    INTEL_AX211_ASSOC_MAC_POWER_VERSION, 0U);
}

int
intel_ax211_assoc_mac_power_encode(
	uint8_t dtim_period,
	uint32_t beacon_interval_ms,
	uint8_t *output,
	size_t output_capacity)
{
	uint64_t keep_alive_milliseconds;
	uint64_t keep_alive_seconds;
	uint32_t effective_dtim;

	if (output == NULL || beacon_interval_ms == 0U)
		return INTEL_AX211_ASSOC_INVALID;
	if (output_capacity < INTEL_AX211_ASSOC_MAC_POWER_SIZE)
		return INTEL_AX211_ASSOC_BUFFER_TOO_SMALL;
	effective_dtim = dtim_period == 0U ? 1U : dtim_period;
	keep_alive_milliseconds = UINT64_C(3) * effective_dtim *
	    beacon_interval_ms;
	keep_alive_seconds = (keep_alive_milliseconds + UINT64_C(999)) /
	    UINT64_C(1000);
	if (keep_alive_seconds <
	    INTEL_AX211_ASSOC_MAC_POWER_KEEP_ALIVE_MIN_SECONDS)
		keep_alive_seconds =
		    INTEL_AX211_ASSOC_MAC_POWER_KEEP_ALIVE_MIN_SECONDS;
	if (keep_alive_seconds > UINT16_MAX)
		return INTEL_AX211_ASSOC_OVERSIZED;
	memset(output, 0, INTEL_AX211_ASSOC_MAC_POWER_SIZE);
	ax211_assoc_put_le32(output, INTEL_AX211_ASSOC_MAC_CONTEXT_ID);
	ax211_assoc_put_le16(output + 4U, 0U);
	ax211_assoc_put_le16(output + 6U, (uint16_t)keep_alive_seconds);
	return INTEL_AX211_ASSOC_OK;
}

int
intel_ax211_assoc_mac_power_response_validate(
	const uint8_t *response,
	size_t response_length)
{
	if (response == NULL)
		return INTEL_AX211_ASSOC_INVALID;
	if (response_length < INTEL_AX211_ASSOC_MAC_POWER_RESPONSE_SIZE)
		return INTEL_AX211_ASSOC_TRUNCATED;
	if (response_length > INTEL_AX211_ASSOC_MAC_POWER_RESPONSE_SIZE)
		return INTEL_AX211_ASSOC_OVERSIZED;
	return ax211_assoc_get_le32(response) == 0U ?
	    INTEL_AX211_ASSOC_OK : INTEL_AX211_ASSOC_FIRMWARE;
}

int
intel_ax211_assoc_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	int result;

	if (table == NULL)
		return INTEL_AX211_ASSOC_INVALID;
	if (intel_ax211_protocol_command_table_validate_api89(table) !=
	    INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_ASSOC_UNSUPPORTED;
	result = ax211_assoc_required_version(table,
	    INTEL_AX211_ASSOC_GROUP_LONG,
	    INTEL_AX211_ASSOC_PHY_CONTEXT_OPCODE,
	    INTEL_AX211_ASSOC_PHY_CONTEXT_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;
	result = ax211_assoc_required_version(table,
	    INTEL_AX211_ASSOC_GROUP_DATA_PATH,
	    INTEL_AX211_ASSOC_RLC_CONFIG_OPCODE,
	    INTEL_AX211_ASSOC_RLC_CONFIG_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;
	result = ax211_assoc_required_version(table,
	    INTEL_AX211_ASSOC_GROUP_DATA_PATH,
	    INTEL_AX211_ASSOC_QUEUE_CONFIG_OPCODE,
	    INTEL_AX211_ASSOC_QUEUE_CONFIG_VERSION,
	    INTEL_AX211_ASSOC_QUEUE_RESPONSE_VERSION);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;
	result = ax211_assoc_required_version(table,
	    INTEL_AX211_ASSOC_GROUP_MAC_CONFIG,
	    INTEL_AX211_ASSOC_SESSION_PROTECTION_OPCODE,
	    INTEL_AX211_ASSOC_SESSION_PROTECTION_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;
	result = ax211_assoc_required_version(table,
	    INTEL_AX211_ASSOC_GROUP_MAC_CONFIG,
	    INTEL_AX211_ASSOC_SESSION_NOTIFICATION_OPCODE,
	    INTEL_AX211_PROTOCOL_UNKNOWN_VERSION,
	    INTEL_AX211_ASSOC_SESSION_NOTIFICATION_API89_VERSION);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;
	result = intel_ax211_assoc_mcast_filter_api89_validate(table);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;
	return intel_ax211_assoc_mac_power_api89_validate(table);
}

static uint32_t
ax211_assoc_next_sequence(uint32_t sequence)
{
	sequence++;
	if (sequence == 0U)
		sequence = 1U;
	return sequence;
}

static int
ax211_assoc_set_step(struct intel_ax211_assoc_state *state,
	enum intel_ax211_assoc_step step, uint64_t now_us)
{
	if (state == NULL || step == INTEL_AX211_ASSOC_STEP_NONE ||
	    now_us > UINT64_MAX - INTEL_AX211_ASSOC_COMMAND_TIMEOUT_US)
		return INTEL_AX211_ASSOC_INVALID;
	state->step = step;
	state->active_sequence = state->next_sequence;
	state->next_sequence = ax211_assoc_next_sequence(state->next_sequence);
	state->deadline = now_us + INTEL_AX211_ASSOC_COMMAND_TIMEOUT_US;
	return INTEL_AX211_ASSOC_OK;
}

int
intel_ax211_assoc_begin(struct intel_ax211_assoc_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_assoc_profile *profile,
	uint64_t common_generation, uint32_t hardware_epoch, uint64_t now_us)
{
	uint32_t next_sequence;
	uint32_t last_completed_sequence;
	int result;

	if (state == NULL || !ax211_assoc_profile_valid(profile) ||
	    common_generation == 0U || hardware_epoch == 0U)
		return INTEL_AX211_ASSOC_INVALID;
	result = intel_ax211_assoc_api89_validate(table);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;
	if (state->initialized && state->phase != INTEL_AX211_ASSOC_PHASE_IDLE)
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;
	if (state->initialized &&
	    common_generation == state->last_common_generation)
		return INTEL_AX211_ASSOC_STALE;

	next_sequence = state->initialized ? state->next_sequence : 1U;
	last_completed_sequence = state->initialized ?
	    state->last_completed_sequence : 0U;
	memset(state, 0, sizeof(*state));
	state->profile = *profile;
	state->common_generation = common_generation;
	state->hardware_epoch = hardware_epoch;
	state->last_common_generation = common_generation;
	state->next_sequence = next_sequence;
	state->last_completed_sequence = last_completed_sequence;
	state->phase = INTEL_AX211_ASSOC_PHASE_AUTH;
	state->failure = INTEL_AX211_ASSOC_OK;
	state->initialized = 1U;
	result = ax211_assoc_set_step(state,
	    INTEL_AX211_ASSOC_STEP_PHY_ADD, now_us);
	if (result != INTEL_AX211_ASSOC_OK)
		memset(state, 0, sizeof(*state));
	return result;
}

int
intel_ax211_assoc_begin_update(struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_update *update,
	uint64_t common_generation, uint32_t hardware_epoch, uint64_t now_us)
{
	int result;

	if (state == NULL || !state->initialized || update == NULL ||
	    common_generation == 0U || hardware_epoch == 0U)
		return INTEL_AX211_ASSOC_INVALID;
	if (common_generation != state->common_generation ||
	    hardware_epoch != state->hardware_epoch)
		return INTEL_AX211_ASSOC_STALE;
	if (state->phase != INTEL_AX211_ASSOC_PHASE_AUTH_READY)
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;
	if (!ax211_assoc_update_valid(&state->profile, update))
		return INTEL_AX211_ASSOC_INVALID;
	result = ax211_assoc_set_step(state,
	    INTEL_AX211_ASSOC_STEP_STATION_UPDATE, now_us);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;
	state->update = *update;
	state->update_valid = 1U;
	state->phase = INTEL_AX211_ASSOC_PHASE_ASSOCIATING;
	return INTEL_AX211_ASSOC_OK;
}

static void
ax211_assoc_command_base(const struct intel_ax211_assoc_state *state,
	struct intel_ax211_assoc_command *command)
{
	memset(command, 0, sizeof(*command));
	command->step = state->step;
	command->sequence = state->active_sequence;
	command->common_generation = state->common_generation;
	command->hardware_epoch = state->hardware_epoch;
	command->deadline = state->deadline;
}

static void
ax211_assoc_phy_encode(const struct intel_ax211_assoc_state *state,
	uint32_t action, struct intel_ax211_assoc_command *command)
{
	command->header = INTEL_AX211_ASSOC_HEADER_LEGACY;
	command->group = INTEL_AX211_ASSOC_GROUP_LEGACY;
	command->opcode = INTEL_AX211_ASSOC_PHY_CONTEXT_OPCODE;
	command->layout_version = INTEL_AX211_ASSOC_PHY_CONTEXT_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_PHY_CONTEXT_SIZE;
	ax211_assoc_put_le32(command->payload + 4U, action);
	ax211_assoc_put_le32(command->payload + 8U, state->profile.channel);
	command->payload[12U] = AX211_ASSOC_PHY_BAND_24;
}

static void
ax211_assoc_rlc_encode(const struct intel_ax211_assoc_state *state,
	struct intel_ax211_assoc_command *command)
{
	uint32_t chains;

	command->header = INTEL_AX211_ASSOC_HEADER_WIDE;
	command->group = INTEL_AX211_ASSOC_GROUP_DATA_PATH;
	command->opcode = INTEL_AX211_ASSOC_RLC_CONFIG_OPCODE;
	command->wire_version = INTEL_AX211_ASSOC_RLC_CONFIG_VERSION;
	command->layout_version = INTEL_AX211_ASSOC_RLC_CONFIG_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_RLC_CONFIG_SIZE;
	chains = ((uint32_t)state->profile.rx_chain_mask << 1) |
	    (UINT32_C(1) << 10) | (UINT32_C(1) << 12);
	ax211_assoc_put_le32(command->payload + 4U, chains);
}

static void
ax211_assoc_edca_encode(const struct intel_ax211_assoc_profile *profile,
	uint8_t *output)
{
	static const uint8_t fifo[INTEL_AX211_ASSOC_EDCA_COUNT] = {
		2U, 1U, 3U, 4U
	};
	size_t index;

	for (index = 0U; index < INTEL_AX211_ASSOC_EDCA_COUNT; index++) {
		const struct intel_ax211_assoc_edca *edca;
		uint8_t *entry;
		uint16_t cw_min;
		uint16_t cw_max;

		edca = &profile->edca[index];
		entry = output + (size_t)fifo[index] * 8U;
		cw_min = (uint16_t)((UINT32_C(1) << edca->ecw_min) - 1U);
		cw_max = (uint16_t)((UINT32_C(1) << edca->ecw_max) - 1U);
		ax211_assoc_put_le16(entry, cw_min);
		ax211_assoc_put_le16(entry + 2U, cw_max);
		entry[4U] = edca->aifsn;
		entry[5U] = (uint8_t)(UINT32_C(1) << fifo[index]);
		ax211_assoc_put_le16(entry + 6U,
		    (uint16_t)(edca->txop_32us * 32U));
	}
}

static void
ax211_assoc_mac_encode(const struct intel_ax211_assoc_state *state,
	uint32_t action, int associated,
	struct intel_ax211_assoc_command *command)
{
	uint32_t filter;

	command->header = INTEL_AX211_ASSOC_HEADER_LEGACY;
	command->group = INTEL_AX211_ASSOC_GROUP_LEGACY;
	command->opcode = INTEL_AX211_ASSOC_MAC_CONTEXT_OPCODE;
	command->layout_version = INTEL_AX211_ASSOC_MAC_CONTEXT_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_MAC_CONTEXT_SIZE;
	ax211_assoc_put_le32(command->payload + 4U, action);
	if (action == AX211_ASSOC_ACTION_REMOVE)
		return;
	ax211_assoc_put_le32(command->payload + 8U,
	    AX211_ASSOC_MAC_TYPE_BSS_STATION);
	memcpy(command->payload + 16U, state->profile.station_address, 6U);
	memcpy(command->payload + 24U, state->profile.bssid, 6U);
	ax211_assoc_put_le32(command->payload + 32U,
	    state->profile.cck_ack_rates);
	ax211_assoc_put_le32(command->payload + 36U,
	    state->profile.ofdm_ack_rates);
	if (state->profile.short_preamble)
		ax211_assoc_put_le32(command->payload + 44U,
		    AX211_ASSOC_MAC_SHORT_PREAMBLE);
	if (state->profile.short_slot)
		ax211_assoc_put_le32(command->payload + 48U,
		    AX211_ASSOC_MAC_SHORT_SLOT);
	/*
	 * The common WLAN liveness contract is refreshed by delivered beacons.
	 * Keep them visible after association until a firmware missed-beacon
	 * notification is explicitly adapted to that contract.
	 */
	filter = AX211_ASSOC_MAC_FILTER_ACCEPT_GROUP |
	    AX211_ASSOC_MAC_FILTER_BEACON;
	ax211_assoc_put_le32(command->payload + 52U, filter);
	if (state->profile.qos)
		ax211_assoc_put_le32(command->payload + 56U,
		    AX211_ASSOC_MAC_QOS_UPDATE_EDCA);
	ax211_assoc_edca_encode(&state->profile, command->payload + 60U);

	ax211_assoc_put_le32(command->payload + 100U,
	    associated ? 1U : 0U);
	ax211_assoc_put_le32(command->payload + 116U,
	    state->profile.beacon_interval_tu);
	ax211_assoc_put_le32(command->payload + 132U, 10U);
	if (associated) {
		uint32_t dtim_offset;
		uint32_t dtim_interval;

		dtim_offset = (uint32_t)state->update.dtim_count *
		    (uint32_t)state->profile.beacon_interval_tu * 1024U;
		dtim_interval = (uint32_t)state->profile.beacon_interval_tu *
		    (uint32_t)state->update.dtim_period;
		ax211_assoc_put_le32(command->payload + 104U,
		    state->update.beacon_arrive_time + dtim_offset);
		ax211_assoc_put_le64(command->payload + 108U,
		    state->update.beacon_tsf + dtim_offset);
		ax211_assoc_put_le32(command->payload + 124U, dtim_interval);
		ax211_assoc_put_le32(command->payload + 136U,
		    state->update.association_id);
		ax211_assoc_put_le32(command->payload + 140U,
		    state->update.beacon_arrive_time);
	}
}

static void
ax211_assoc_binding_encode(uint32_t action,
	struct intel_ax211_assoc_command *command)
{
	command->header = INTEL_AX211_ASSOC_HEADER_LEGACY;
	command->group = INTEL_AX211_ASSOC_GROUP_LEGACY;
	command->opcode = INTEL_AX211_ASSOC_BINDING_OPCODE;
	command->layout_version = INTEL_AX211_ASSOC_BINDING_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_STATUS_ZERO;
	command->payload_length = INTEL_AX211_ASSOC_BINDING_SIZE;
	ax211_assoc_put_le32(command->payload + 4U, action);
	ax211_assoc_put_le32(command->payload + 12U,
	    AX211_ASSOC_INVALID_CONTEXT);
	ax211_assoc_put_le32(command->payload + 16U,
	    AX211_ASSOC_INVALID_CONTEXT);
}

static void
ax211_assoc_station_encode(const struct intel_ax211_assoc_state *state,
	int update, struct intel_ax211_assoc_command *command)
{
	command->header = INTEL_AX211_ASSOC_HEADER_LEGACY;
	command->group = INTEL_AX211_ASSOC_GROUP_LEGACY;
	command->opcode = INTEL_AX211_ASSOC_ADD_STATION_OPCODE;
	command->layout_version = INTEL_AX211_ASSOC_STATION_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_STATION_SUCCESS;
	command->payload_length = INTEL_AX211_ASSOC_STATION_SIZE;
	command->payload[0U] = update ? 1U : 0U;
	if (!update)
		memcpy(command->payload + 8U, state->profile.bssid, 6U);
	ax211_assoc_put_le32(command->payload + 24U,
	    AX211_ASSOC_STATION_FLAGS_MASK);
	command->payload[35U] = AX211_ASSOC_STATION_TYPE_LINK;
}

static void
ax211_assoc_station_remove_encode(
	struct intel_ax211_assoc_command *command)
{
	command->header = INTEL_AX211_ASSOC_HEADER_LEGACY;
	command->group = INTEL_AX211_ASSOC_GROUP_LEGACY;
	command->opcode = INTEL_AX211_ASSOC_REMOVE_STATION_OPCODE;
	command->layout_version = INTEL_AX211_ASSOC_REMOVE_STATION_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_REMOVE_STATION_SIZE;
}

static void
ax211_assoc_queue_encode(const struct intel_ax211_assoc_state *state,
	uint32_t operation, struct intel_ax211_assoc_command *command)
{
	command->header = INTEL_AX211_ASSOC_HEADER_WIDE;
	command->group = INTEL_AX211_ASSOC_GROUP_DATA_PATH;
	command->opcode = INTEL_AX211_ASSOC_QUEUE_CONFIG_OPCODE;
	command->layout_version = INTEL_AX211_ASSOC_QUEUE_CONFIG_VERSION;
	command->response_version = INTEL_AX211_ASSOC_QUEUE_RESPONSE_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_QUEUE;
	command->payload_length = INTEL_AX211_ASSOC_QUEUE_CONFIG_SIZE;
	ax211_assoc_put_le32(command->payload, operation);
	ax211_assoc_put_le32(command->payload + 4U, 1U);
	if (operation == AX211_ASSOC_QUEUE_OPERATION_ADD) {
		command->expected_queue_write_pointer =
		    state->profile.queue_initial_write_pointer;
		command->payload[8U] = AX211_ASSOC_MANAGEMENT_TID;
		ax211_assoc_put_le32(command->payload + 16U,
		    AX211_ASSOC_QUEUE_CB_SIZE);
		ax211_assoc_put_le64(command->payload + 20U,
		    state->profile.queue_byte_count_address);
		ax211_assoc_put_le64(command->payload + 28U,
		    state->profile.queue_descriptor_address);
	} else {
		ax211_assoc_put_le32(command->payload + 8U,
		    AX211_ASSOC_MANAGEMENT_TID);
	}
}

static void
ax211_assoc_session_encode(const struct intel_ax211_assoc_state *state,
	uint32_t action, struct intel_ax211_assoc_command *command)
{
	command->header = INTEL_AX211_ASSOC_HEADER_WIDE;
	command->group = INTEL_AX211_ASSOC_GROUP_MAC_CONFIG;
	command->opcode = INTEL_AX211_ASSOC_SESSION_PROTECTION_OPCODE;
	command->layout_version =
	    INTEL_AX211_ASSOC_SESSION_PROTECTION_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length =
	    INTEL_AX211_ASSOC_SESSION_PROTECTION_SIZE;
	ax211_assoc_put_le32(command->payload,
	    INTEL_AX211_ASSOC_MAC_CONTEXT_ID);
	ax211_assoc_put_le32(command->payload + 4U, action);
	ax211_assoc_put_le32(command->payload + 8U,
	    AX211_ASSOC_SESSION_CONFIGURATION_ASSOC);
	if (action == AX211_ASSOC_ACTION_ADD)
		ax211_assoc_put_le32(command->payload + 12U,
		    (uint32_t)state->profile.beacon_interval_tu * 9U);
}

static int
ax211_assoc_command_encode(const struct intel_ax211_assoc_state *state,
	struct intel_ax211_assoc_command *command)
{
	ax211_assoc_command_base(state, command);
	switch (state->step) {
	case INTEL_AX211_ASSOC_STEP_PHY_ADD:
		ax211_assoc_phy_encode(state, AX211_ASSOC_ACTION_ADD, command);
		break;
	case INTEL_AX211_ASSOC_STEP_RLC_CONFIG:
		ax211_assoc_rlc_encode(state, command);
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_ADD:
		ax211_assoc_mac_encode(state, AX211_ASSOC_ACTION_ADD, 0,
		    command);
		break;
	case INTEL_AX211_ASSOC_STEP_BINDING_ADD:
		ax211_assoc_binding_encode(AX211_ASSOC_ACTION_ADD, command);
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_ADD:
		ax211_assoc_station_encode(state, 0, command);
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE:
		ax211_assoc_queue_encode(state, AX211_ASSOC_QUEUE_OPERATION_ADD,
		    command);
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_PROTECT:
		ax211_assoc_session_encode(state, AX211_ASSOC_ACTION_ADD,
		    command);
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_UPDATE:
		ax211_assoc_station_encode(state, 1, command);
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE:
		ax211_assoc_mac_encode(state, AX211_ASSOC_ACTION_MODIFY, 1,
		    command);
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_REMOVE:
		ax211_assoc_session_encode(state, AX211_ASSOC_ACTION_REMOVE,
		    command);
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE:
		ax211_assoc_queue_encode(state,
		    AX211_ASSOC_QUEUE_OPERATION_REMOVE, command);
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_REMOVE:
		ax211_assoc_station_remove_encode(command);
		break;
	case INTEL_AX211_ASSOC_STEP_BINDING_REMOVE:
		ax211_assoc_binding_encode(AX211_ASSOC_ACTION_REMOVE, command);
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_REMOVE:
		ax211_assoc_mac_encode(state, AX211_ASSOC_ACTION_REMOVE, 0,
		    command);
		break;
	case INTEL_AX211_ASSOC_STEP_PHY_REMOVE:
		ax211_assoc_phy_encode(state, AX211_ASSOC_ACTION_REMOVE,
		    command);
		break;
	case INTEL_AX211_ASSOC_STEP_NONE:
	default:
		return INTEL_AX211_ASSOC_INVALID;
	}
	return INTEL_AX211_ASSOC_OK;
}

static int
ax211_assoc_command_matches(const struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_command *command)
{
	struct intel_ax211_assoc_command expected;

	if (state == NULL || command == NULL ||
	    ax211_assoc_command_encode(state, &expected) !=
	    INTEL_AX211_ASSOC_OK)
		return 0;
	return command->step == expected.step &&
	    command->header == expected.header &&
	    command->response_kind == expected.response_kind &&
	    command->group == expected.group &&
	    command->opcode == expected.opcode &&
	    command->wire_version == expected.wire_version &&
	    command->layout_version == expected.layout_version &&
	    command->response_version == expected.response_version &&
	    command->expected_queue_write_pointer ==
	        expected.expected_queue_write_pointer &&
	    command->sequence == expected.sequence &&
	    command->common_generation == expected.common_generation &&
	    command->hardware_epoch == expected.hardware_epoch &&
	    command->deadline == expected.deadline &&
	    command->payload_length == expected.payload_length &&
	    memcmp(command->payload, expected.payload,
	        expected.payload_length) == 0;
}

int
intel_ax211_assoc_current(const struct intel_ax211_assoc_state *state,
	uint64_t now_us, struct intel_ax211_assoc_command *command)
{
	if (state == NULL || command == NULL || !state->initialized)
		return INTEL_AX211_ASSOC_INVALID;
	if (state->phase == INTEL_AX211_ASSOC_PHASE_AUTH_READY)
		return INTEL_AX211_ASSOC_AUTH_READY;
	if (state->phase == INTEL_AX211_ASSOC_PHASE_ASSOCIATED)
		return INTEL_AX211_ASSOC_COMPLETE;
	if (state->phase == INTEL_AX211_ASSOC_PHASE_FAILED)
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	if (state->phase == INTEL_AX211_ASSOC_PHASE_IDLE)
		return state->failure == INTEL_AX211_ASSOC_OK ?
		    INTEL_AX211_ASSOC_INVALID : INTEL_AX211_ASSOC_ROLLED_BACK;
	if (state->step == INTEL_AX211_ASSOC_STEP_NONE ||
	    state->active_sequence == 0U)
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;
	if (now_us >= state->deadline)
		return INTEL_AX211_ASSOC_TIMEOUT;
	return ax211_assoc_command_encode(state, command);
}

static int
ax211_assoc_response_validate(const struct intel_ax211_assoc_command *command,
	const struct intel_ax211_assoc_reply *reply)
{
	if (command == NULL || reply == NULL ||
	    reply->payload_length > INTEL_AX211_ASSOC_RESPONSE_MAX ||
	    reply->response_version != command->response_version)
		return INTEL_AX211_ASSOC_FIRMWARE;
	switch (command->response_kind) {
	case INTEL_AX211_ASSOC_RESPONSE_EMPTY:
		return reply->payload_length == 0U ? INTEL_AX211_ASSOC_OK :
		    INTEL_AX211_ASSOC_FIRMWARE;
	case INTEL_AX211_ASSOC_RESPONSE_STATUS_ZERO:
		if (reply->payload_length != 4U ||
		    ax211_assoc_get_le32(reply->payload) != 0U)
			return INTEL_AX211_ASSOC_FIRMWARE;
		return INTEL_AX211_ASSOC_OK;
	case INTEL_AX211_ASSOC_RESPONSE_STATION_SUCCESS:
		if (reply->payload_length != 4U ||
		    ax211_assoc_get_le32(reply->payload) !=
		    AX211_ASSOC_STATION_STATUS_SUCCESS)
			return INTEL_AX211_ASSOC_FIRMWARE;
		return INTEL_AX211_ASSOC_OK;
	case INTEL_AX211_ASSOC_RESPONSE_QUEUE:
		if (reply->payload_length !=
		    INTEL_AX211_ASSOC_QUEUE_RESPONSE_SIZE ||
		    ax211_assoc_get_le16(reply->payload) !=
		    AX211_ASSOC_QUEUE_ID ||
		    ax211_assoc_get_le16(reply->payload + 2U) != 0U ||
		    ax211_assoc_get_le16(reply->payload + 4U) !=
		    command->expected_queue_write_pointer ||
		    ax211_assoc_get_le16(reply->payload + 6U) != 0U)
			return INTEL_AX211_ASSOC_FIRMWARE;
		return INTEL_AX211_ASSOC_OK;
	default:
		return INTEL_AX211_ASSOC_FIRMWARE;
	}
}

static void
ax211_assoc_mark_success(struct intel_ax211_assoc_state *state,
	enum intel_ax211_assoc_step step)
{
	switch (step) {
	case INTEL_AX211_ASSOC_STEP_PHY_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_PHY;
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_MAC;
		break;
	case INTEL_AX211_ASSOC_STEP_BINDING_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_BINDING;
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_STATION;
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE:
		state->resources |= AX211_ASSOC_RESOURCE_QUEUE;
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_PROTECT:
		if (state->session_ended == 0U)
			state->resources |= AX211_ASSOC_RESOURCE_SESSION;
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_REMOVE:
		state->resources &= ~AX211_ASSOC_RESOURCE_SESSION;
		state->session_ended = 1U;
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE:
		state->resources &= ~AX211_ASSOC_RESOURCE_QUEUE;
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_REMOVE:
		state->resources &= ~AX211_ASSOC_RESOURCE_STATION;
		break;
	case INTEL_AX211_ASSOC_STEP_BINDING_REMOVE:
		state->resources &= ~AX211_ASSOC_RESOURCE_BINDING;
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_REMOVE:
		state->resources &= ~AX211_ASSOC_RESOURCE_MAC;
		break;
	case INTEL_AX211_ASSOC_STEP_PHY_REMOVE:
		state->resources &= ~AX211_ASSOC_RESOURCE_PHY;
		break;
	case INTEL_AX211_ASSOC_STEP_NONE:
	case INTEL_AX211_ASSOC_STEP_RLC_CONFIG:
	case INTEL_AX211_ASSOC_STEP_STATION_UPDATE:
	case INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE:
	default:
		break;
	}
}

static void
ax211_assoc_mark_uncertain(struct intel_ax211_assoc_state *state,
	enum intel_ax211_assoc_step step)
{
	switch (step) {
	case INTEL_AX211_ASSOC_STEP_PHY_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_PHY;
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_MAC;
		break;
	case INTEL_AX211_ASSOC_STEP_BINDING_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_BINDING;
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_STATION;
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE:
		state->resources |= AX211_ASSOC_RESOURCE_QUEUE;
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_PROTECT:
	case INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE:
		if (state->session_ended == 0U)
			state->resources |= AX211_ASSOC_RESOURCE_SESSION;
		break;
	default:
		break;
	}
}

static enum intel_ax211_assoc_step
ax211_assoc_rollback_step(uint32_t resources)
{
	if ((resources & AX211_ASSOC_RESOURCE_SESSION) != 0U)
		return INTEL_AX211_ASSOC_STEP_SESSION_REMOVE;
	if ((resources & AX211_ASSOC_RESOURCE_QUEUE) != 0U)
		return INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE;
	if ((resources & AX211_ASSOC_RESOURCE_STATION) != 0U)
		return INTEL_AX211_ASSOC_STEP_STATION_REMOVE;
	if ((resources & AX211_ASSOC_RESOURCE_BINDING) != 0U)
		return INTEL_AX211_ASSOC_STEP_BINDING_REMOVE;
	if ((resources & AX211_ASSOC_RESOURCE_MAC) != 0U)
		return INTEL_AX211_ASSOC_STEP_MAC_REMOVE;
	if ((resources & AX211_ASSOC_RESOURCE_PHY) != 0U)
		return INTEL_AX211_ASSOC_STEP_PHY_REMOVE;
	return INTEL_AX211_ASSOC_STEP_NONE;
}

static int
ax211_assoc_rollback_begin(struct intel_ax211_assoc_state *state,
	int failure, uint64_t now_us)
{
	enum intel_ax211_assoc_step step;
	int result;

	if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK)
		return INTEL_AX211_ASSOC_PENDING;
	state->failure = failure;
	step = ax211_assoc_rollback_step(state->resources);
	if (step == INTEL_AX211_ASSOC_STEP_NONE) {
		state->phase = INTEL_AX211_ASSOC_PHASE_IDLE;
		state->step = INTEL_AX211_ASSOC_STEP_NONE;
		state->active_sequence = 0U;
		state->deadline = 0U;
		return INTEL_AX211_ASSOC_ROLLED_BACK;
	}
	state->phase = INTEL_AX211_ASSOC_PHASE_ROLLBACK;
	result = ax211_assoc_set_step(state, step, now_us);
	if (result != INTEL_AX211_ASSOC_OK) {
		state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
		state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	}
	return INTEL_AX211_ASSOC_PENDING;
}

static int
ax211_assoc_advance(struct intel_ax211_assoc_state *state, uint64_t now_us)
{
	enum intel_ax211_assoc_step next;
	int result;

	switch (state->step) {
	case INTEL_AX211_ASSOC_STEP_PHY_ADD:
		next = INTEL_AX211_ASSOC_STEP_RLC_CONFIG;
		break;
	case INTEL_AX211_ASSOC_STEP_RLC_CONFIG:
		next = INTEL_AX211_ASSOC_STEP_MAC_ADD;
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_ADD:
		next = INTEL_AX211_ASSOC_STEP_BINDING_ADD;
		break;
	case INTEL_AX211_ASSOC_STEP_BINDING_ADD:
		next = INTEL_AX211_ASSOC_STEP_STATION_ADD;
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_ADD:
		next = INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE;
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE:
		next = INTEL_AX211_ASSOC_STEP_SESSION_PROTECT;
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_PROTECT:
		state->phase = INTEL_AX211_ASSOC_PHASE_AUTH_READY;
		state->step = INTEL_AX211_ASSOC_STEP_NONE;
		state->active_sequence = 0U;
		state->deadline = 0U;
		return INTEL_AX211_ASSOC_AUTH_READY;
	case INTEL_AX211_ASSOC_STEP_STATION_UPDATE:
		next = INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE;
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE:
		state->phase = INTEL_AX211_ASSOC_PHASE_ASSOCIATED;
		state->step = INTEL_AX211_ASSOC_STEP_NONE;
		state->active_sequence = 0U;
		state->deadline = 0U;
		return INTEL_AX211_ASSOC_COMPLETE;
	case INTEL_AX211_ASSOC_STEP_SESSION_REMOVE:
	case INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE:
	case INTEL_AX211_ASSOC_STEP_STATION_REMOVE:
	case INTEL_AX211_ASSOC_STEP_BINDING_REMOVE:
	case INTEL_AX211_ASSOC_STEP_MAC_REMOVE:
	case INTEL_AX211_ASSOC_STEP_PHY_REMOVE:
		next = ax211_assoc_rollback_step(state->resources);
		if (next == INTEL_AX211_ASSOC_STEP_NONE) {
			state->phase = INTEL_AX211_ASSOC_PHASE_IDLE;
			state->step = INTEL_AX211_ASSOC_STEP_NONE;
			state->active_sequence = 0U;
			state->deadline = 0U;
			state->update_valid = 0U;
			return INTEL_AX211_ASSOC_ROLLED_BACK;
		}
		break;
	case INTEL_AX211_ASSOC_STEP_NONE:
	default:
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;
	}
	result = ax211_assoc_set_step(state, next, now_us);
	if (result == INTEL_AX211_ASSOC_OK)
		return INTEL_AX211_ASSOC_PENDING;
	if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK) {
		state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
		state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	}
	(void)ax211_assoc_rollback_begin(state, result, now_us);
	return result;
}

int
intel_ax211_assoc_accept(struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_command *command,
	const struct intel_ax211_assoc_reply *reply, uint64_t now_us)
{
	int result;

	if (state == NULL || command == NULL || reply == NULL ||
	    !state->initialized)
		return INTEL_AX211_ASSOC_INVALID;
	if (command->common_generation != state->common_generation ||
	    command->hardware_epoch != state->hardware_epoch ||
	    reply->common_generation != state->common_generation ||
	    reply->hardware_epoch != state->hardware_epoch)
		return INTEL_AX211_ASSOC_STALE;
	if (command->sequence == state->last_completed_sequence ||
	    reply->sequence == state->last_completed_sequence)
		return INTEL_AX211_ASSOC_DUPLICATE;
	if (command->step != state->step || reply->step != state->step ||
	    command->sequence != state->active_sequence ||
	    reply->sequence != state->active_sequence)
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;
	if (!ax211_assoc_command_matches(state, command))
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;
	if (now_us >= state->deadline)
		return intel_ax211_assoc_expire(state, now_us);
	if (reply->acknowledgement != 0) {
		if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK) {
			state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
			state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
			return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		}
		(void)ax211_assoc_rollback_begin(state,
		    INTEL_AX211_ASSOC_FIRMWARE, now_us);
		return INTEL_AX211_ASSOC_FIRMWARE;
	}
	result = ax211_assoc_response_validate(command, reply);
	if (result != INTEL_AX211_ASSOC_OK) {
		if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK) {
			state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
			state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
			return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		}
		ax211_assoc_mark_uncertain(state, state->step);
		(void)ax211_assoc_rollback_begin(state, result, now_us);
		return result;
	}
	state->last_completed_sequence = state->active_sequence;
	ax211_assoc_mark_success(state, state->step);
	return ax211_assoc_advance(state, now_us);
}

int
intel_ax211_assoc_session_event_accept(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_protocol_message *message,
	uint64_t common_generation,
	uint32_t hardware_epoch)
{
	uint32_t mac_id;
	uint32_t status;
	uint32_t start;
	uint32_t configuration_id;

	if (state == NULL || message == NULL || !state->initialized ||
	    common_generation == 0U || hardware_epoch == 0U ||
	    message->generation == 0U)
		return INTEL_AX211_ASSOC_INVALID;
	if (common_generation != state->common_generation ||
	    hardware_epoch != state->hardware_epoch ||
	    message->generation != hardware_epoch)
		return INTEL_AX211_ASSOC_STALE;
	if (message->group != INTEL_AX211_ASSOC_GROUP_MAC_CONFIG ||
	    message->opcode != INTEL_AX211_ASSOC_SESSION_NOTIFICATION_OPCODE ||
	    message->version !=
	    INTEL_AX211_ASSOC_SESSION_NOTIFICATION_LAYOUT_VERSION)
		return INTEL_AX211_ASSOC_UNSUPPORTED;
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_ASSOC_FIRMWARE;
	if ((message->queue & 0x80U) == 0U)
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;
	if (message->payload == NULL)
		return INTEL_AX211_ASSOC_INVALID;
	if (message->payload_length <
	    INTEL_AX211_ASSOC_SESSION_NOTIFICATION_SIZE)
		return INTEL_AX211_ASSOC_TRUNCATED;
	if (message->payload_length >
	    INTEL_AX211_ASSOC_SESSION_NOTIFICATION_SIZE)
		return INTEL_AX211_ASSOC_OVERSIZED;
	mac_id = ax211_assoc_get_le32(message->payload);
	status = ax211_assoc_get_le32(message->payload + 4U);
	start = ax211_assoc_get_le32(message->payload + 8U);
	configuration_id = ax211_assoc_get_le32(message->payload + 12U);
	if (status > 1U || start > 1U)
		return INTEL_AX211_ASSOC_FIRMWARE;
	if (mac_id != INTEL_AX211_ASSOC_MAC_CONTEXT_ID ||
	    configuration_id != AX211_ASSOC_SESSION_CONFIGURATION_ASSOC ||
	    status != 1U || start != 0U)
		return INTEL_AX211_ASSOC_EVENT_IGNORED;
	if (state->session_ended != 0U)
		return INTEL_AX211_ASSOC_DUPLICATE;
	state->session_ended = 1U;
	state->resources &= ~AX211_ASSOC_RESOURCE_SESSION;
	return INTEL_AX211_ASSOC_SESSION_EXPIRED;
}

int
intel_ax211_assoc_expire(struct intel_ax211_assoc_state *state,
	uint64_t now_us)
{
	if (state == NULL || !state->initialized ||
	    state->step == INTEL_AX211_ASSOC_STEP_NONE ||
	    now_us < state->deadline)
		return INTEL_AX211_ASSOC_INVALID;
	if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK) {
		state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
		state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	}
	ax211_assoc_mark_uncertain(state, state->step);
	(void)ax211_assoc_rollback_begin(state,
	    INTEL_AX211_ASSOC_TIMEOUT, now_us);
	return INTEL_AX211_ASSOC_TIMEOUT;
}

int
intel_ax211_assoc_cancel(struct intel_ax211_assoc_state *state,
	uint64_t common_generation, uint32_t hardware_epoch, uint64_t now_us)
{
	if (state == NULL || !state->initialized || common_generation == 0U ||
	    hardware_epoch == 0U)
		return INTEL_AX211_ASSOC_INVALID;
	if (common_generation != state->common_generation ||
	    hardware_epoch != state->hardware_epoch)
		return INTEL_AX211_ASSOC_STALE;
	if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK)
		return INTEL_AX211_ASSOC_PENDING;
	if (state->phase == INTEL_AX211_ASSOC_PHASE_IDLE)
		return INTEL_AX211_ASSOC_ROLLED_BACK;
	if (state->phase == INTEL_AX211_ASSOC_PHASE_FAILED)
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	ax211_assoc_mark_uncertain(state, state->step);
	return ax211_assoc_rollback_begin(state,
	    INTEL_AX211_ASSOC_ROLLED_BACK, now_us);
}

static int
ax211_assoc_exchange_failure(struct intel_ax211_assoc_state *state,
	int result, uint64_t now_us)
{
	int failure;

	switch (result) {
	case INTEL_AX211_ASSOC_TIMEOUT:
	case INTEL_AX211_ASSOC_STALE:
	case INTEL_AX211_ASSOC_DUPLICATE:
	case INTEL_AX211_ASSOC_OUT_OF_ORDER:
	case INTEL_AX211_ASSOC_FIRMWARE:
	case INTEL_AX211_ASSOC_IO:
		failure = result;
		break;
	default:
		failure = INTEL_AX211_ASSOC_IO;
		break;
	}
	if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK) {
		state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
		state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	}
	ax211_assoc_mark_uncertain(state, state->step);
	(void)ax211_assoc_rollback_begin(state, failure, now_us);
	return failure;
}

int
intel_ax211_assoc_drive(struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_ops *ops, void *argument)
{
	unsigned int count;

	if (state == NULL || ops == NULL || ops->clock_us == NULL ||
	    ops->exchange == NULL)
		return INTEL_AX211_ASSOC_INVALID;
	for (count = 0U; count < INTEL_AX211_ASSOC_COMMAND_LIMIT; count++) {
		struct intel_ax211_assoc_command command;
		struct intel_ax211_assoc_reply reply;
		uint64_t now_us;
		int result;

		now_us = ops->clock_us(argument);
		result = intel_ax211_assoc_current(state, now_us, &command);
		if (result == INTEL_AX211_ASSOC_AUTH_READY ||
		    result == INTEL_AX211_ASSOC_COMPLETE ||
		    result == INTEL_AX211_ASSOC_ROLLED_BACK ||
		    result == INTEL_AX211_ASSOC_ROLLBACK_FAILED)
			return result == INTEL_AX211_ASSOC_ROLLED_BACK ?
			    state->failure : result;
		if (result == INTEL_AX211_ASSOC_TIMEOUT) {
			(void)intel_ax211_assoc_expire(state, now_us);
			continue;
		}
		if (result != INTEL_AX211_ASSOC_OK)
			return result;

		memset(&reply, 0, sizeof(reply));
		result = ops->exchange(argument, &command, &reply);
		now_us = ops->clock_us(argument);
		if (result != INTEL_AX211_ASSOC_OK) {
			(void)ax211_assoc_exchange_failure(state, result, now_us);
			continue;
		}
		result = intel_ax211_assoc_accept(state, &command, &reply,
		    now_us);
		if (result == INTEL_AX211_ASSOC_AUTH_READY ||
		    result == INTEL_AX211_ASSOC_COMPLETE)
			return result;
		if (result == INTEL_AX211_ASSOC_PENDING ||
		    result == INTEL_AX211_ASSOC_FIRMWARE ||
		    result == INTEL_AX211_ASSOC_TIMEOUT)
			continue;
		if (result == INTEL_AX211_ASSOC_ROLLED_BACK)
			return state->failure;
		if (result == INTEL_AX211_ASSOC_STALE ||
		    result == INTEL_AX211_ASSOC_OUT_OF_ORDER ||
		    result == INTEL_AX211_ASSOC_DUPLICATE) {
			(void)ax211_assoc_exchange_failure(state, result, now_us);
			continue;
		}
		return result;
	}
	state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
	state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
}
