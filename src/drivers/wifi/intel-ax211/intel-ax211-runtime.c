/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD Intel AX211 private API89 runtime codecs and state
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

#include "intel-ax211-runtime.h"

#include <string.h>

struct ax211_runtime_version {
	uint8_t group;
	uint8_t opcode;
	uint8_t command_version;
	uint8_t notification_version;
};

static int ax211_runtime_profile_valid(const struct intel_ax211_runtime_profile *profile);
static int ax211_runtime_bit(const uint32_t *words, size_t count, unsigned int bit);
static int ax211_runtime_version_validate( const struct intel_ax211_protocol_command_table *table, const struct ax211_runtime_version *required);
static int ax211_runtime_step_enabled(enum intel_ax211_runtime_step step, const struct intel_ax211_runtime_profile *profile);
static void ax211_runtime_put_le32(uint8_t *bytes, uint32_t value);
static void ax211_runtime_put_le16(uint8_t *bytes, uint16_t value);
static uint32_t ax211_runtime_get_le32(const uint8_t *bytes);
static uint16_t ax211_runtime_get_le16(const uint8_t *bytes);
static enum intel_ax211_runtime_step ax211_runtime_next_step(enum intel_ax211_runtime_step step, const struct intel_ax211_runtime_profile *profile);

static const struct ax211_runtime_version ax211_runtime_versions[] = {
	{0x01U, 0x98U, 1U, 0U},	 {0x01U, 0x9bU, 6U, 0U}, {0x02U, 0x01U, 2U, 0U},
	{0x01U, 0xeeU, 3U, 0U},	 {0x04U, 0x04U, 1U, 0U}, {0x01U, 0x77U, 7U, 0U},
	{0x01U, 0xc8U, 1U, 6U},	 {0x01U, 0xd2U, 4U, 0U}, {0x01U, 0x0cU, 5U, 0U},
	{0x01U, 0x0dU, 17U, 0U}, {0x01U, 0x0eU, 1U, 0U}};

/*
 * Implements the drv intel ax211 runtime profile from manifest operation.
 */
int
drv_intel_ax211_runtime_profile_from_manifest(
	const struct intel_ax211_firmware_manifest *manifest,
	const struct intel_ax211_protocol_nvm *nvm,
	int ltr_enabled,
	struct intel_ax211_runtime_profile *profile)
{
	struct intel_ax211_runtime_profile parsed;

	/* Handles the manifest availability. */
	if (manifest == NULL || nvm == NULL || profile == NULL ||
	    (ltr_enabled != 0 && ltr_enabled != 1) ||
	    nvm->tx_chain_mask == 0U || nvm->rx_chain_mask == 0U) {
		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_INVALID;
	}
	memset(&parsed, 0, sizeof(parsed));
	parsed.tx_chain_mask = nvm->tx_chain_mask;
	parsed.rx_chain_mask = nvm->rx_chain_mask;
	parsed.lar_enabled = nvm->lar_enabled ? 1U : 0U;
	parsed.ltr_enabled = ltr_enabled ? 1U : 0U;
	memcpy(parsed.api_changes, manifest->api_changes,
	       sizeof(parsed.api_changes));
	memcpy(parsed.capabilities, manifest->capabilities,
	       sizeof(parsed.capabilities));

	/* Checks the ax211 runtime profile valid result. */
	if (!ax211_runtime_profile_valid(&parsed))
		return INTEL_AX211_RUNTIME_UNSUPPORTED;
	*profile = parsed;
	/* Returns the computed result. */
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Implements the drv intel ax211 runtime api89 validate operation.
 */
int
drv_intel_ax211_runtime_api89_validate(
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_runtime_profile *profile)
{
	struct intel_ax211_protocol_command_version dqa;
	size_t index;
	int result;

	/* Checks the ax211 runtime profile valid result. */
	if (table == NULL || !ax211_runtime_profile_valid(profile))
		return INTEL_AX211_RUNTIME_INVALID;

	/* Checks the drv intel ax211 protocol command table validate api89 result. */
	if (drv_intel_ax211_protocol_command_table_validate_api89(table) !=
	    INTEL_AX211_PROTOCOL_OK) {
		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_UNSUPPORTED;
	}
	/* Process each remaining element. */
	for (index = 0U; index < sizeof(ax211_runtime_versions) /
					 sizeof(ax211_runtime_versions[0]);
	     index++) {
		/* Checks the ax211 runtime bit result. */
		if (ax211_runtime_versions[index].group ==
			    INTEL_AX211_RUNTIME_GROUP_LONG &&
		    ax211_runtime_versions[index].opcode ==
			    INTEL_AX211_RUNTIME_LTR_CONFIG_OPCODE &&
		    ax211_runtime_bit(profile->capabilities, 5U,
				      INTEL_AX211_RUNTIME_CAP_SET_LTR_GEN2))
			continue;

		/* Checks the operation result. */
		result = ax211_runtime_version_validate(
			table, &ax211_runtime_versions[index]);
		if (result != INTEL_AX211_RUNTIME_OK)
			return result;
	}

	/* The pinned image does not advertise DQA and has no g5/c00 row. */

	/* Checks the operation result. */
	result = drv_intel_ax211_protocol_command_version_lookup(table, 5U, 0U,
								 &dqa);
	if (result != INTEL_AX211_PROTOCOL_MISSING)
		return INTEL_AX211_RUNTIME_UNSUPPORTED;

	/* Returns the computed result. */
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Implements the drv intel ax211 runtime command encode operation.
 */
int
drv_intel_ax211_runtime_command_encode(
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_profile *profile,
	struct intel_ax211_runtime_command *command)
{
	struct intel_ax211_runtime_command encoded;

	/* Checks the ax211 runtime profile valid result. */
	if (!ax211_runtime_profile_valid(profile) || command == NULL ||
	    step >= INTEL_AX211_RUNTIME_STEP_DONE ||
	    !ax211_runtime_step_enabled(step, profile)) {
		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_INVALID;
	}
	memset(&encoded, 0, sizeof(encoded));

	/* OpenBSD sends zero in the wide-header version field. */
	encoded.wire_version = 0U;
	encoded.response_version = 0U;
	/* Dispatch the selected operation case. */
	switch (step) {
	case INTEL_AX211_RUNTIME_STEP_TX_ANT:
		encoded.group = INTEL_AX211_RUNTIME_GROUP_LONG;
		encoded.opcode = INTEL_AX211_RUNTIME_TX_ANT_OPCODE;
		encoded.layout_version = INTEL_AX211_RUNTIME_TX_ANT_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_TX_ANT_SIZE;
		ax211_runtime_put_le32(encoded.payload, profile->tx_chain_mask);
		break;
	case INTEL_AX211_RUNTIME_STEP_BT_CONFIG:
		encoded.group = INTEL_AX211_RUNTIME_GROUP_LONG;
		encoded.opcode = INTEL_AX211_RUNTIME_BT_CONFIG_OPCODE;
		encoded.layout_version = INTEL_AX211_RUNTIME_BT_CONFIG_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_BT_CONFIG_SIZE;
		ax211_runtime_put_le32(encoded.payload, 3U);
		break;
	case INTEL_AX211_RUNTIME_STEP_SOC_CONFIG:
		encoded.group = INTEL_AX211_RUNTIME_GROUP_SYSTEM;
		encoded.opcode = INTEL_AX211_RUNTIME_SOC_CONFIG_OPCODE;
		encoded.layout_version = INTEL_AX211_RUNTIME_SOC_CONFIG_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_SOC_CONFIG_SIZE;
		ax211_runtime_put_le32(encoded.payload,
				       INTEL_AX211_RUNTIME_SOC_CONFIG_FLAGS);
		ax211_runtime_put_le32(
			encoded.payload + 4U,
			INTEL_AX211_RUNTIME_SOC_CONFIG_XTAL_LATENCY);
		break;
	case INTEL_AX211_RUNTIME_STEP_LTR_CONFIG:
		encoded.group = INTEL_AX211_RUNTIME_GROUP_LONG;
		encoded.opcode = INTEL_AX211_RUNTIME_LTR_CONFIG_OPCODE;
		encoded.layout_version = INTEL_AX211_RUNTIME_LTR_CONFIG_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_LTR_CONFIG_SIZE;
		ax211_runtime_put_le32(encoded.payload, 1U);
		break;
	case INTEL_AX211_RUNTIME_STEP_TEMP_REPORT:
		encoded.group = INTEL_AX211_RUNTIME_GROUP_PHY;
		encoded.opcode = INTEL_AX211_RUNTIME_TEMP_REPORT_OPCODE;
		encoded.layout_version =
			INTEL_AX211_RUNTIME_TEMP_REPORT_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_TEMP_REPORT_SIZE;
		break;
	case INTEL_AX211_RUNTIME_STEP_POWER_TABLE:
		encoded.group = INTEL_AX211_RUNTIME_GROUP_LONG;
		encoded.opcode = INTEL_AX211_RUNTIME_POWER_TABLE_OPCODE;
		encoded.layout_version =
			INTEL_AX211_RUNTIME_POWER_TABLE_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_POWER_TABLE_SIZE;
		break;
	case INTEL_AX211_RUNTIME_STEP_MCC_UPDATE:
		encoded.group = INTEL_AX211_RUNTIME_GROUP_LONG;
		encoded.opcode = INTEL_AX211_RUNTIME_MCC_UPDATE_OPCODE;
		encoded.layout_version = INTEL_AX211_RUNTIME_MCC_UPDATE_VERSION;
		encoded.response_version =
			INTEL_AX211_RUNTIME_MCC_RESPONSE_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_MCC_UPDATE_SIZE;
		ax211_runtime_put_le16(encoded.payload,
				       (uint16_t)(('Z' << 8) | 'Z'));
		encoded.payload[2] = 0x10U;
		break;
	case INTEL_AX211_RUNTIME_STEP_SCAN_CONFIG:
		encoded.group = INTEL_AX211_RUNTIME_GROUP_LONG;
		encoded.opcode = INTEL_AX211_PROTOCOL_SCAN_CFG_OPCODE;
		encoded.layout_version = 5U;
		encoded.payload_length = 12U;
		ax211_runtime_put_le32(encoded.payload + 4U,
				       profile->tx_chain_mask);
		ax211_runtime_put_le32(encoded.payload + 8U,
				       profile->rx_chain_mask);
		break;
	case INTEL_AX211_RUNTIME_STEP_BEACON_FILTER:
		encoded.group = INTEL_AX211_RUNTIME_GROUP_LONG;
		encoded.opcode = INTEL_AX211_RUNTIME_BEACON_FILTER_OPCODE;
		encoded.layout_version =
			INTEL_AX211_RUNTIME_BEACON_FILTER_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_BEACON_FILTER_SIZE;
		break;
	case INTEL_AX211_RUNTIME_STEP_DONE:
	default:
		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_INVALID;
	}

	*command = encoded;
	/* Returns the computed result. */
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Implements the drv intel ax211 runtime mcc decode operation.
 */
int
drv_intel_ax211_runtime_mcc_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation,
	struct intel_ax211_runtime_mcc *mcc)
{
	struct intel_ax211_runtime_mcc parsed;
	size_t expected;
	size_t index;
	const uint8_t *bytes;

	/* Handles the message availability. */
	if (message == NULL || mcc == NULL || generation == 0U)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Handles the message condition. */
	if (message->generation != generation)
		return INTEL_AX211_RUNTIME_STALE;

	/* Handles the message condition. */
	if (message->group != INTEL_AX211_RUNTIME_GROUP_LONG ||
	    message->opcode != INTEL_AX211_RUNTIME_MCC_UPDATE_OPCODE ||
	    message->version != INTEL_AX211_RUNTIME_MCC_RESPONSE_VERSION) {
		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_UNSUPPORTED;
	}

	/* Checks the operation status. */
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_RUNTIME_FAILED;

	/* Handles the payload availability. */
	if (message->payload == NULL || message->payload_length < 20U)
		return INTEL_AX211_RUNTIME_TRUNCATED;
	bytes = message->payload;
	memset(&parsed, 0, sizeof(parsed));
	parsed.status = ax211_runtime_get_le32(bytes);
	parsed.mcc = ax211_runtime_get_le16(bytes + 4U);
	parsed.capabilities = ax211_runtime_get_le16(bytes + 6U);
	parsed.time = ax211_runtime_get_le16(bytes + 8U);
	parsed.geographic_info = ax211_runtime_get_le16(bytes + 10U);
	parsed.source = bytes[12U];

	/*
	 * v4/v5/v6 share this layout; reserved padding is not semantic input.
	 */
	parsed.channel_count = ax211_runtime_get_le32(bytes + 16U);

	/*
	 * Every documented MCC status still carries a usable channel profile.
	 */
	if (parsed.status > INTEL_AX211_RUNTIME_MCC_STATUS_MAX)
		return INTEL_AX211_RUNTIME_FAILED;

	/* Handles the parsed condition. */
	if (parsed.channel_count == 0U)
		return INTEL_AX211_RUNTIME_UNSUPPORTED;

	/* Handles the parsed condition. */
	if (parsed.channel_count > INTEL_AX211_RUNTIME_MCC_CHANNEL_LIMIT)
		return INTEL_AX211_RUNTIME_OVERSIZED;

	/* Handles the message condition. */
	expected = 20U + (size_t)parsed.channel_count * 4U;
	if (message->payload_length < expected)
		return INTEL_AX211_RUNTIME_TRUNCATED;

	/* Handles the message condition. */
	if (message->payload_length > expected)
		return INTEL_AX211_RUNTIME_OVERSIZED;
	/* Process each remaining element. */
	for (index = 0U; index < parsed.channel_count; index++) {
		parsed.channel[index] =
			ax211_runtime_get_le32(bytes + 20U + index * 4U);
	}

	*mcc = parsed;
	/* Returns the computed result. */
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Implements the drv intel ax211 runtime begin operation.
 */
int
drv_intel_ax211_runtime_begin(
	struct intel_ax211_runtime_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_runtime_profile *profile,
	uint32_t generation,
	uint64_t now_us)
{
	struct intel_ax211_runtime_state started;
	int result;

	/* Handles the state availability. */
	if (state == NULL || generation == 0U ||
	    now_us > UINT64_MAX - INTEL_AX211_RUNTIME_COMMAND_TIMEOUT_US) {
		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_INVALID;
	}

	/* Checks the operation result. */
	result = drv_intel_ax211_runtime_api89_validate(table, profile);
	if (result != INTEL_AX211_RUNTIME_OK)
		return result;
	memset(&started, 0, sizeof(started));
	started.profile = *profile;
	started.generation = generation;
	started.deadline = now_us + INTEL_AX211_RUNTIME_COMMAND_TIMEOUT_US;
	started.step = INTEL_AX211_RUNTIME_STEP_TX_ANT;
	started.active = 1U;
	*state = started;
	/* Returns the computed result. */
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Implements the drv intel ax211 runtime current operation.
 */
int
drv_intel_ax211_runtime_current(
	const struct intel_ax211_runtime_state *state,
	uint64_t now_us,
	struct intel_ax211_runtime_command *command)
{
	int error;

	/* Handles the state availability. */
	if (state == NULL || command == NULL || !state->active ||
	    state->terminal || state->generation == 0U) {
		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_INVALID;
	}

	/* Handles the now us condition. */
	if (now_us >= state->deadline)
		return INTEL_AX211_RUNTIME_TIMEOUT;

	/* Obtains the drv intel ax211 runtime command encode result. */
	error = drv_intel_ax211_runtime_command_encode(
		state->step, &state->profile, command);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv intel ax211 runtime ack operation.
 */
int
drv_intel_ax211_runtime_ack(
	struct intel_ax211_runtime_state *state,
	uint32_t generation,
	enum intel_ax211_runtime_step step,
	uint64_t now_us)
{
	enum intel_ax211_runtime_step next;

	/* Handles the state availability. */
	if (state == NULL || generation == 0U)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Handles the step condition. */
	if ((unsigned int)step >= (unsigned int)INTEL_AX211_RUNTIME_STEP_DONE)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Handles the generation condition. */
	if (generation != state->generation)
		return INTEL_AX211_RUNTIME_STALE;

	/* Handles the state condition. */
	if (state->terminal || !state->active)
		return INTEL_AX211_RUNTIME_DUPLICATE;

	/* Handles the now us condition. */
	if (now_us >= state->deadline) {
		state->active = 0U;
		state->terminal = 1U;

		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_TIMEOUT;
	}

	/* Handles the step condition. */
	if (step != state->step) {
		return step < state->step ? INTEL_AX211_RUNTIME_DUPLICATE
					  : INTEL_AX211_RUNTIME_OUT_OF_ORDER;
	}

	next = ax211_runtime_next_step(step, &state->profile);
	state->step = next;

	/* Handles the next condition. */
	if (next == INTEL_AX211_RUNTIME_STEP_DONE) {
		state->active = 0U;
		state->terminal = 1U;

		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_COMPLETE;
	}

	/* Handles the now us condition. */
	if (now_us > UINT64_MAX - INTEL_AX211_RUNTIME_COMMAND_TIMEOUT_US) {
		state->active = 0U;
		state->terminal = 1U;

		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_TIMEOUT;
	}

	state->deadline = now_us + INTEL_AX211_RUNTIME_COMMAND_TIMEOUT_US;

	/* Returns the computed result. */
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Implements the drv intel ax211 runtime expire operation.
 */
int
drv_intel_ax211_runtime_expire(
	struct intel_ax211_runtime_state *state,
	uint64_t now_us)
{
	/* Handles the state availability. */
	if (state == NULL || state->generation == 0U)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Handles the state condition. */
	if (state->terminal || !state->active)
		return INTEL_AX211_RUNTIME_DUPLICATE;

	/* Handles the now us condition. */
	if (now_us < state->deadline)
		return INTEL_AX211_RUNTIME_OK;
	state->active = 0U;
	state->terminal = 1U;

	/* Returns the computed result. */
	return INTEL_AX211_RUNTIME_TIMEOUT;
}

/* Supports the ax211 runtime profile valid operation. */
static int
ax211_runtime_profile_valid(
	const struct intel_ax211_runtime_profile *profile)
{
	/* Handles the profile availability. */
	if (profile == NULL || profile->tx_chain_mask == 0U ||
	    profile->rx_chain_mask == 0U || profile->tx_chain_mask > 0x07U ||
	    profile->rx_chain_mask > 0x07U || profile->lar_enabled > 1U ||
	    profile->ltr_enabled > 1U) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the ax211 runtime bit result. */
	if (!ax211_runtime_bit(profile->api_changes, 4U,
			       INTEL_AX211_RUNTIME_API_REDUCED_SCAN_CONFIG) ||
	    !ax211_runtime_bit(profile->api_changes, 4U,
			       INTEL_AX211_RUNTIME_API_SCAN_EXT_CHANNEL) ||
	    !ax211_runtime_bit(profile->capabilities, 5U,
			       INTEL_AX211_RUNTIME_CAP_DS_PARAM_SET_IE) ||
	    ax211_runtime_bit(profile->capabilities, 5U,
			      INTEL_AX211_RUNTIME_CAP_DQA) ||
	    !ax211_runtime_bit(profile->capabilities, 5U,
			       INTEL_AX211_RUNTIME_CAP_CT_KILL_BY_FW)) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the ax211 runtime bit result. */
	if (profile->lar_enabled &&
	    !ax211_runtime_bit(profile->api_changes, 4U,
			       INTEL_AX211_RUNTIME_API_WIFI_MCC_UPDATE) &&
	    !ax211_runtime_bit(profile->capabilities, 5U,
			       INTEL_AX211_RUNTIME_CAP_LAR_MULTI_MCC)) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the ax211 runtime bit result. */
	if (profile->lar_enabled &&
	    !ax211_runtime_bit(profile->capabilities, 5U,
			       INTEL_AX211_RUNTIME_CAP_MCC_UPDATE_11AX)) {
		/* Succeeded. */
		return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the ax211 runtime bit operation. */
static int
ax211_runtime_bit(
	const uint32_t *words,
	size_t count,
	unsigned int bit)
{
	int error;
	size_t word;

	/* Handles the words availability. */
	word = bit / 32U;
	if (words == NULL || word >= count)
		return 0;

	/* Computes the function result. */
	error = (words[word] & (UINT32_C(1) << (bit % 32U))) != 0U;

	/* Returns the computed result. */
	return error;
}

/* Supports the ax211 runtime version validate operation. */
static int
ax211_runtime_version_validate(
	const struct intel_ax211_protocol_command_table *table,
	const struct ax211_runtime_version *required)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Checks the operation result. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, required->group, required->opcode, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_UNSUPPORTED;

	/* Handles the version condition. */
	if (version.command_version != required->command_version ||
	    version.notification_version != required->notification_version) {
		/* Returns the computed result. */
		return INTEL_AX211_RUNTIME_UNSUPPORTED;
	}

	/* Returns the computed result. */
	return INTEL_AX211_RUNTIME_OK;
}

/* Supports the ax211 runtime step enabled operation. */
static int
ax211_runtime_step_enabled(
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_profile *profile)
{
	int error;

	/* Handles the step condition. */
	if (step == INTEL_AX211_RUNTIME_STEP_LTR_CONFIG) {
		/* Computes the function result. */
		error = profile->ltr_enabled &&
				  !ax211_runtime_bit(
					  profile->capabilities, 5U,
					  INTEL_AX211_RUNTIME_CAP_SET_LTR_GEN2);

		/* Failed. */
		return error;
	}

	/* Handles the step condition. */
	if (step == INTEL_AX211_RUNTIME_STEP_MCC_UPDATE)
		return profile->lar_enabled;

	/* Returns the computed result. */
	return step < INTEL_AX211_RUNTIME_STEP_DONE;
}

/* Supports the ax211 runtime put le32 operation. */
static void
ax211_runtime_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Supports the ax211 runtime put le16 operation. */
static void
ax211_runtime_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Supports the ax211 runtime get le32 operation. */
static uint32_t
ax211_runtime_get_le32(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Supports the ax211 runtime get le16 operation. */
static uint16_t
ax211_runtime_get_le16(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* Supports the ax211 runtime next step operation. */
static enum intel_ax211_runtime_step
ax211_runtime_next_step(
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_profile *profile)
{
	unsigned int next;

	next = (unsigned int)step + 1U;
	/* Continue while the operation condition remains true. */
	while (next < (unsigned int)INTEL_AX211_RUNTIME_STEP_DONE &&
	       !ax211_runtime_step_enabled((enum intel_ax211_runtime_step)next,
					   profile))
		next++;

	/* Returns the computed result. */
	return (enum intel_ax211_runtime_step)next;
}
