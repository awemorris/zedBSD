/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private API89 CCMP key implementation
 *
 * Portions derived from OpenBSD sys/dev/pci/if_iwxreg.h and if_iwx.c at
 * commit 0f464d413c50396e4e6cd70948f15613d6a73081.
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

#include "intel-ax211-key.h"

#include <stdint.h>
#include <string.h>

#define AX211_KEY_ACTION_ADD                               1U
#define AX211_KEY_ACTION_REMOVE                            3U
#define AX211_KEY_STATION_MASK                             1U
#define AX211_KEY_FLAG_CCMP                                2U
#define AX211_KEY_FLAG_MULTICAST                        0x40U
#define AX211_KEY_PACKET_NUMBER_MAX UINT64_C(0x0000ffffffffffff)

static void ax211_key_put_le32(uint8_t *bytes, uint32_t value);
static void ax211_key_put_le64(uint8_t *bytes, uint64_t value);
static int ax211_key_request_valid(
	const struct intel_ax211_key_request *request);
static int ax211_key_state_live(const struct intel_ax211_key_state *state,
	uint64_t connection_generation, uint32_t hardware_epoch);

int
intel_ax211_key_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	if (table == NULL)
		return INTEL_AX211_KEY_INVALID;
	result = intel_ax211_protocol_command_version_lookup(table,
	    INTEL_AX211_KEY_GROUP, INTEL_AX211_KEY_OPCODE, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_KEY_UNSUPPORTED;
	if (version.command_version != INTEL_AX211_KEY_COMMAND_VERSION ||
	    version.notification_version !=
	    INTEL_AX211_KEY_RESPONSE_VERSION)
		return INTEL_AX211_KEY_UNSUPPORTED;
	return INTEL_AX211_KEY_OK;
}

int
intel_ax211_key_add_encode(
	const struct intel_ax211_key_request *request,
	uint8_t output[INTEL_AX211_KEY_COMMAND_SIZE])
{
	uint32_t flags;

	if (!ax211_key_request_valid(request) || output == NULL)
		return INTEL_AX211_KEY_INVALID;
	memset(output, 0, INTEL_AX211_KEY_COMMAND_SIZE);
	flags = AX211_KEY_FLAG_CCMP;
	if (request->kind == INTEL_AX211_KEY_GROUP_KEY)
		flags |= AX211_KEY_FLAG_MULTICAST;
	ax211_key_put_le32(output, AX211_KEY_ACTION_ADD);
	ax211_key_put_le32(output + 4U, AX211_KEY_STATION_MASK);
	ax211_key_put_le32(output + 8U, request->key_index);
	ax211_key_put_le32(output + 12U, flags);
	memcpy(output + 16U, request->key, INTEL_AX211_KEY_BYTES);
	ax211_key_put_le64(output + 64U, request->receive_packet_number);
	return INTEL_AX211_KEY_OK;
}

int
intel_ax211_key_remove_encode(
	uint64_t connection_generation,
	uint64_t key_generation,
	enum intel_ax211_key_kind kind,
	uint8_t key_index,
	uint8_t output[INTEL_AX211_KEY_COMMAND_SIZE])
{
	uint32_t flags;

	if (output == NULL || connection_generation == 0U ||
	    key_generation == 0U || key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (kind != INTEL_AX211_KEY_PAIRWISE &&
	    kind != INTEL_AX211_KEY_GROUP_KEY) ||
	    (kind == INTEL_AX211_KEY_PAIRWISE && key_index != 0U))
		return INTEL_AX211_KEY_INVALID;
	memset(output, 0, INTEL_AX211_KEY_COMMAND_SIZE);
	flags = AX211_KEY_FLAG_CCMP;
	if (kind == INTEL_AX211_KEY_GROUP_KEY)
		flags |= AX211_KEY_FLAG_MULTICAST;
	ax211_key_put_le32(output, AX211_KEY_ACTION_REMOVE);
	ax211_key_put_le32(output + 4U, AX211_KEY_STATION_MASK);
	ax211_key_put_le32(output + 8U, key_index);
	ax211_key_put_le32(output + 12U, flags);
	return INTEL_AX211_KEY_OK;
}

void
intel_ax211_key_command_scrub(
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE])
{
	volatile uint8_t *bytes;
	size_t index;

	if (command == NULL)
		return;
	bytes = command;
	for (index = 0U; index < INTEL_AX211_KEY_COMMAND_SIZE; index++)
		bytes[index] = 0U;
}

int
intel_ax211_key_state_init(
	struct intel_ax211_key_state *state,
	uint32_t hardware_epoch,
	uint64_t connection_generation)
{
	if (state == NULL || hardware_epoch == 0U ||
	    connection_generation == 0U)
		return INTEL_AX211_KEY_INVALID;
	memset(state, 0, sizeof(*state));
	state->hardware_epoch = hardware_epoch;
	state->connection_generation = connection_generation;
	state->initialized = 1U;
	return INTEL_AX211_KEY_OK;
}

int
intel_ax211_key_state_installed(
	struct intel_ax211_key_state *state,
	const struct intel_ax211_key_request *request,
	uint32_t hardware_epoch)
{
	uint64_t *slot;
	int result;

	if (!ax211_key_request_valid(request))
		return INTEL_AX211_KEY_INVALID;
	result = ax211_key_state_live(state,
	    request->connection_generation, hardware_epoch);
	if (result != INTEL_AX211_KEY_OK)
		return result;
	slot = request->kind == INTEL_AX211_KEY_PAIRWISE ?
	    &state->staged_pairwise :
	    &state->staged_group[request->key_index];
	if (*slot == request->key_generation)
		return INTEL_AX211_KEY_DUPLICATE;
	if (*slot != 0U)
		return INTEL_AX211_KEY_STALE;
	*slot = request->key_generation;
	return INTEL_AX211_KEY_OK;
}

int
intel_ax211_key_state_activate(
	struct intel_ax211_key_state *state,
	uint64_t connection_generation,
	uint64_t pairwise_generation,
	uint64_t group_generation,
	uint32_t hardware_epoch)
{
	uint8_t group_index;
	int pairwise_found;
	int found;
	int result;

	if (pairwise_generation == 0U || group_generation == 0U)
		return INTEL_AX211_KEY_INVALID;
	result = ax211_key_state_live(state, connection_generation,
	    hardware_epoch);
	if (result != INTEL_AX211_KEY_OK)
		return result;
	if (state->active_pairwise == pairwise_generation) {
		for (group_index = 0U;
		    group_index < INTEL_AX211_KEY_INDEX_LIMIT; group_index++) {
			if (state->active_group[group_index] == group_generation)
				return INTEL_AX211_KEY_DUPLICATE;
		}
	}
	pairwise_found = state->staged_pairwise == pairwise_generation ||
	    state->active_pairwise == pairwise_generation;
	if (!pairwise_found)
		return INTEL_AX211_KEY_MISSING;
	found = 0;
	group_index = 0U;
	while (group_index < INTEL_AX211_KEY_INDEX_LIMIT && !found) {
		if (state->staged_group[group_index] == group_generation)
			found = 1;
		else
			group_index++;
	}
	if (!found) {
		group_index = 0U;
		while (group_index < INTEL_AX211_KEY_INDEX_LIMIT && !found) {
			if (state->active_group[group_index] == group_generation)
				found = 1;
			else
				group_index++;
		}
	}
	if (!found)
		return INTEL_AX211_KEY_MISSING;
	memset(state->active_group, 0, sizeof(state->active_group));
	state->active_pairwise = pairwise_generation;
	state->active_group[group_index] = group_generation;
	state->active_group_index = group_index;
	state->staged_pairwise = 0U;
	memset(state->staged_group, 0, sizeof(state->staged_group));
	return INTEL_AX211_KEY_OK;
}

int
intel_ax211_key_state_removed(
	struct intel_ax211_key_state *state,
	uint64_t connection_generation,
	enum intel_ax211_key_kind kind,
	uint8_t key_index,
	uint64_t key_generation,
	uint32_t hardware_epoch)
{
	uint64_t *staged;
	uint64_t *active;
	int result;

	if (key_generation == 0U || key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (kind != INTEL_AX211_KEY_PAIRWISE &&
	    kind != INTEL_AX211_KEY_GROUP_KEY) ||
	    (kind == INTEL_AX211_KEY_PAIRWISE && key_index != 0U))
		return INTEL_AX211_KEY_INVALID;
	result = ax211_key_state_live(state, connection_generation,
	    hardware_epoch);
	if (result != INTEL_AX211_KEY_OK)
		return result;
	staged = kind == INTEL_AX211_KEY_PAIRWISE ?
	    &state->staged_pairwise : &state->staged_group[key_index];
	active = kind == INTEL_AX211_KEY_PAIRWISE ?
	    &state->active_pairwise : &state->active_group[key_index];
	if (*staged != key_generation && *active != key_generation)
		return INTEL_AX211_KEY_MISSING;
	if (*staged == key_generation)
		*staged = 0U;
	if (*active == key_generation)
		*active = 0U;
	return INTEL_AX211_KEY_OK;
}

int
intel_ax211_key_state_rx_generation(
	const struct intel_ax211_key_state *state,
	uint64_t connection_generation,
	enum intel_ax211_key_kind kind,
	uint8_t key_index,
	uint32_t hardware_epoch,
	uint64_t *key_generation)
{
	uint64_t generation;
	int result;

	if (key_generation == NULL ||
	    key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (kind != INTEL_AX211_KEY_PAIRWISE &&
	    kind != INTEL_AX211_KEY_GROUP_KEY) ||
	    (kind == INTEL_AX211_KEY_PAIRWISE && key_index != 0U))
		return INTEL_AX211_KEY_INVALID;
	result = ax211_key_state_live(state, connection_generation,
	    hardware_epoch);
	if (result != INTEL_AX211_KEY_OK)
		return result;
	generation = kind == INTEL_AX211_KEY_PAIRWISE ?
	    state->active_pairwise : state->active_group[key_index];
	if (generation == 0U)
		return INTEL_AX211_KEY_MISSING;
	*key_generation = generation;
	return INTEL_AX211_KEY_OK;
}

int
intel_ax211_key_state_tx_validate(
	const struct intel_ax211_key_state *state,
	uint64_t connection_generation,
	uint64_t key_generation,
	uint8_t key_index,
	uint64_t packet_number,
	uint32_t hardware_epoch)
{
	int result;

	if (key_generation == 0U || key_index != 0U || packet_number == 0U ||
	    packet_number > AX211_KEY_PACKET_NUMBER_MAX)
		return INTEL_AX211_KEY_INVALID;
	result = ax211_key_state_live(state, connection_generation,
	    hardware_epoch);
	if (result != INTEL_AX211_KEY_OK)
		return result;
	return state->active_pairwise == key_generation ?
	    INTEL_AX211_KEY_OK : INTEL_AX211_KEY_STALE;
}

static int
ax211_key_request_valid(
	const struct intel_ax211_key_request *request)
{
	if (request == NULL || request->connection_generation == 0U ||
	    request->key_generation == 0U ||
	    request->receive_packet_number > AX211_KEY_PACKET_NUMBER_MAX ||
	    request->key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (request->kind != INTEL_AX211_KEY_PAIRWISE &&
	    request->kind != INTEL_AX211_KEY_GROUP_KEY) ||
	    (request->kind == INTEL_AX211_KEY_PAIRWISE &&
	    request->key_index != 0U))
		return 0;
	return 1;
}

static int
ax211_key_state_live(
	const struct intel_ax211_key_state *state,
	uint64_t connection_generation,
	uint32_t hardware_epoch)
{
	if (state == NULL || !state->initialized || hardware_epoch == 0U ||
	    connection_generation == 0U)
		return INTEL_AX211_KEY_INVALID;
	if (state->hardware_epoch != hardware_epoch ||
	    state->connection_generation != connection_generation)
		return INTEL_AX211_KEY_STALE;
	return INTEL_AX211_KEY_OK;
}

static void
ax211_key_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0U] = (uint8_t)value;
	bytes[1U] = (uint8_t)(value >> 8);
	bytes[2U] = (uint8_t)(value >> 16);
	bytes[3U] = (uint8_t)(value >> 24);
}

static void
ax211_key_put_le64(
	uint8_t *bytes,
	uint64_t value)
{
	unsigned index;

	for (index = 0U; index < 8U; index++)
		bytes[index] = (uint8_t)(value >> (index * 8U));
}
