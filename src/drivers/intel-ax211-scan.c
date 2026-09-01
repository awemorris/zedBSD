/*
 * zedBSD Intel AX211 private API89 passive-scan codecs and state
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

#include "intel-ax211-scan.h"

#include <string.h>

#define AX211_SCAN_GENERAL_OFFSET                         8U
#define AX211_SCAN_CHANNEL_OFFSET                        44U
#define AX211_SCAN_CHANNEL_CONFIG_OFFSET                 48U
#define AX211_SCAN_CHANNEL_CONFIG_SIZE                    8U
#define AX211_SCAN_PERIODIC_OFFSET                      584U
#define AX211_SCAN_PROBE_OFFSET                         596U

#define AX211_SCAN_FLAG_PASS_ALL                       0x02U
#define AX211_SCAN_FLAG_ITERATION_COMPLETE             0x04U
#define AX211_SCAN_FLAG_ADAPTIVE_DWELL                 0x80U
#define AX211_SCAN_FLAG_FORCE_PASSIVE                 0x0800U
#define AX211_SCAN_CHANNEL_ORDER_FLAG                  0x20U
#define AX211_SCAN_24GHZ_BAND_FLAG               0x40000000U
#define AX211_SCAN_STATUS_COMPLETED                       1U
#define AX211_SCAN_STATUS_ABORTED                         2U

static void
ax211_scan_put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
ax211_scan_put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t
ax211_scan_get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int
ax211_scan_station_valid(const uint8_t address[6])
{
	size_t index;
	int all_zero;
	int all_ones;

	if (address == NULL || (address[0] & 1U) != 0U)
		return 0;
	all_zero = 1;
	all_ones = 1;
	for (index = 0U; index < 6U; index++) {
		if (address[index] != 0U)
			all_zero = 0;
		if (address[index] != 0xffU)
			all_ones = 0;
	}
	return !all_zero && !all_ones;
}

static int
ax211_scan_profile_valid(const struct intel_ax211_scan_profile *profile)
{
	uint16_t channels;
	size_t index;

	if (profile == NULL ||
	    !ax211_scan_station_valid(profile->station_address) ||
	    profile->channel_width_mhz !=
	    INTEL_AX211_SCAN_CHANNEL_WIDTH_MHZ ||
	    profile->channel_count == 0U ||
	    profile->channel_count > INTEL_AX211_SCAN_CHANNEL_LIMIT)
		return 0;
	channels = 0U;
	for (index = 0U; index < profile->channel_count; index++) {
		uint8_t channel;
		uint16_t bit;

		channel = profile->channel[index];
		if (channel == 0U || channel > INTEL_AX211_SCAN_CHANNEL_LIMIT)
			return 0;
		bit = (uint16_t)(UINT16_C(1) << channel);
		if ((channels & bit) != 0U)
			return 0;
		channels |= bit;
	}
	return 1;
}

int
intel_ax211_scan_profile_from_nvm(
	const struct intel_ax211_protocol_nvm *nvm,
	const struct intel_ax211_runtime_mcc *mcc,
	const uint8_t station_address[6],
	struct intel_ax211_scan_profile *profile)
{
	struct intel_ax211_scan_profile parsed;
	uint16_t admitted;
	size_t index;

	if (nvm == NULL || station_address == NULL || profile == NULL ||
	    !nvm->band_24_enabled ||
	    nvm->channel_24ghz_count >
	    INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT ||
	    (nvm->lar_enabled && mcc == NULL) ||
	    (mcc != NULL && (mcc->status > 1U || mcc->channel_count == 0U ||
	    mcc->channel_count > INTEL_AX211_RUNTIME_MCC_CHANNEL_LIMIT)) ||
	    !ax211_scan_station_valid(station_address))
		return INTEL_AX211_SCAN_INVALID;
	memset(&parsed, 0, sizeof(parsed));
	memcpy(parsed.station_address, station_address, 6U);
	parsed.channel_width_mhz = INTEL_AX211_SCAN_CHANNEL_WIDTH_MHZ;
	admitted = 0U;
	for (index = 0U; index < nvm->channel_24ghz_count; index++) {
		const struct intel_ax211_protocol_channel *candidate;
		uint16_t bit;

		candidate = &nvm->channel_24ghz[index];
		if (!candidate->valid || candidate->number == 0U ||
		    candidate->number > INTEL_AX211_SCAN_CHANNEL_LIMIT)
			continue;
		if (mcc != NULL &&
		    ((size_t)candidate->number > mcc->channel_count ||
		    (mcc->channel[candidate->number - 1U] &
		    INTEL_AX211_PROTOCOL_NVM_CHANNEL_VALID) == 0U))
			continue;
		bit = (uint16_t)(UINT16_C(1) << candidate->number);
		if ((admitted & bit) != 0U)
			continue;
		parsed.channel[parsed.channel_count] = candidate->number;
		parsed.channel_count++;
		admitted |= bit;
	}
	if (!ax211_scan_profile_valid(&parsed))
		return INTEL_AX211_SCAN_UNSUPPORTED;
	*profile = parsed;
	return INTEL_AX211_SCAN_OK;
}

static int
ax211_scan_required_version(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t opcode,
	uint8_t command_version)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	result = intel_ax211_protocol_command_version_lookup(table,
	    INTEL_AX211_SCAN_GROUP_LONG, opcode, &version);
	if (result != INTEL_AX211_PROTOCOL_OK ||
	    version.command_version != command_version ||
	    version.notification_version != 0U)
		return INTEL_AX211_SCAN_UNSUPPORTED;
	return INTEL_AX211_SCAN_OK;
}

int
intel_ax211_scan_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	int result;

	if (table == NULL)
		return INTEL_AX211_SCAN_INVALID;
	if (intel_ax211_protocol_command_table_validate_api89(table) !=
	    INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_SCAN_UNSUPPORTED;
	result = ax211_scan_required_version(table,
	    INTEL_AX211_PROTOCOL_SCAN_CFG_OPCODE,
	    INTEL_AX211_SCAN_CONFIG_VERSION);
	if (result != INTEL_AX211_SCAN_OK)
		return result;
	result = ax211_scan_required_version(table,
	    INTEL_AX211_SCAN_REQUEST_OPCODE,
	    INTEL_AX211_SCAN_REQUEST_VERSION);
	if (result != INTEL_AX211_SCAN_OK)
		return result;
	return ax211_scan_required_version(table,
	    INTEL_AX211_SCAN_ABORT_OPCODE,
	    INTEL_AX211_SCAN_ABORT_VERSION);
}

static void
ax211_scan_probe_encode(const struct intel_ax211_scan_profile *profile,
	uint8_t *probe)
{
	static const uint8_t rates[12] = {
		2U, 4U, 11U, 22U, 12U, 18U, 24U, 36U,
		48U, 72U, 96U, 108U
	};
	uint8_t *frame;
	size_t offset;

	/*
	 * Segment descriptors precede the fixed 512-byte frame area.  This
	 * first profile is forced passive, so firmware never transmits this
	 * frame.  Keep a valid OpenBSD-style legacy 11g probe body here; a later
	 * active-scan profile must add its negotiated HT/HE information.
	 */
	ax211_scan_put_le16(probe, 0U);
	ax211_scan_put_le16(probe + 2U, 26U);
	ax211_scan_put_le16(probe + 4U, 26U);
	ax211_scan_put_le16(probe + 6U, 19U);
	ax211_scan_put_le16(probe + 16U, 45U);
	ax211_scan_put_le16(probe + 18U, 0U);

	frame = probe + 20U;
	frame[0] = 0x40U;
	memset(frame + 4U, 0xff, 6U);
	memcpy(frame + 10U, profile->station_address, 6U);
	memset(frame + 16U, 0xff, 6U);
	frame[24U] = 0U;
	frame[25U] = 0U;
	offset = 26U;
	frame[offset++] = 1U;
	frame[offset++] = 8U;
	memcpy(frame + offset, rates, 8U);
	offset += 8U;
	frame[offset++] = 50U;
	frame[offset++] = 4U;
	memcpy(frame + offset, rates + 8U, 4U);
	offset += 4U;
	frame[offset++] = 3U;
	frame[offset++] = 1U;
	frame[offset] = 0U;
}

int
intel_ax211_scan_request_encode(
	const struct intel_ax211_scan_profile *profile,
	uint8_t output[INTEL_AX211_SCAN_REQUEST_SIZE])
{
	uint16_t flags;
	size_t index;

	if (!ax211_scan_profile_valid(profile) || output == NULL)
		return INTEL_AX211_SCAN_INVALID;
	memset(output, 0, INTEL_AX211_SCAN_REQUEST_SIZE);
	ax211_scan_put_le32(output, INTEL_AX211_SCAN_UID);
	ax211_scan_put_le32(output + 4U, INTEL_AX211_SCAN_PRIORITY);
	flags = AX211_SCAN_FLAG_PASS_ALL |
	    AX211_SCAN_FLAG_ITERATION_COMPLETE |
	    AX211_SCAN_FLAG_ADAPTIVE_DWELL |
	    AX211_SCAN_FLAG_FORCE_PASSIVE;
	ax211_scan_put_le16(output + AX211_SCAN_GENERAL_OFFSET, flags);
	output[AX211_SCAN_GENERAL_OFFSET + 4U] =
	    INTEL_AX211_SCAN_ACTIVE_DWELL;
	output[AX211_SCAN_GENERAL_OFFSET + 5U] =
	    INTEL_AX211_SCAN_ACTIVE_DWELL;
	output[AX211_SCAN_GENERAL_OFFSET + 6U] = 2U;
	output[AX211_SCAN_GENERAL_OFFSET + 7U] = 8U;
	output[AX211_SCAN_GENERAL_OFFSET + 8U] = 10U;
	ax211_scan_put_le16(output + AX211_SCAN_GENERAL_OFFSET + 10U,
	    INTEL_AX211_SCAN_ADAPTIVE_BUDGET);
	ax211_scan_put_le32(output + AX211_SCAN_GENERAL_OFFSET + 28U,
	    INTEL_AX211_SCAN_PRIORITY);
	output[AX211_SCAN_GENERAL_OFFSET + 32U] =
	    INTEL_AX211_SCAN_PASSIVE_DWELL;
	output[AX211_SCAN_GENERAL_OFFSET + 33U] =
	    INTEL_AX211_SCAN_PASSIVE_DWELL;

	output[AX211_SCAN_CHANNEL_OFFSET] = AX211_SCAN_CHANNEL_ORDER_FLAG;
	output[AX211_SCAN_CHANNEL_OFFSET + 1U] =
	    (uint8_t)profile->channel_count;
	output[AX211_SCAN_CHANNEL_OFFSET + 2U] = 10U;
	output[AX211_SCAN_CHANNEL_OFFSET + 3U] = 2U;
	for (index = 0U; index < profile->channel_count; index++) {
		uint8_t *channel;

		channel = output + AX211_SCAN_CHANNEL_CONFIG_OFFSET +
		    index * AX211_SCAN_CHANNEL_CONFIG_SIZE;
		ax211_scan_put_le32(channel, AX211_SCAN_24GHZ_BAND_FLAG);
		channel[4U] = profile->channel[index];
		channel[5U] = 0x80U;
		channel[6U] = 1U;
	}
	output[AX211_SCAN_PERIODIC_OFFSET + 2U] = 1U;
	ax211_scan_probe_encode(profile, output + AX211_SCAN_PROBE_OFFSET);
	return INTEL_AX211_SCAN_OK;
}

int
intel_ax211_scan_abort_encode(
	uint8_t output[INTEL_AX211_SCAN_ABORT_SIZE])
{
	if (output == NULL)
		return INTEL_AX211_SCAN_INVALID;
	memset(output, 0, INTEL_AX211_SCAN_ABORT_SIZE);
	return INTEL_AX211_SCAN_OK;
}

int
intel_ax211_scan_begin(
	struct intel_ax211_scan_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_scan_profile *profile,
	uint32_t generation,
	uint64_t now_us)
{
	struct intel_ax211_scan_state started;
	size_t index;
	int result;

	if (state == NULL || !ax211_scan_profile_valid(profile) ||
	    generation == 0U ||
	    now_us > UINT64_MAX - INTEL_AX211_SCAN_WATCHDOG_US)
		return INTEL_AX211_SCAN_INVALID;
	result = intel_ax211_scan_api89_validate(table);
	if (result != INTEL_AX211_SCAN_OK)
		return result;
	memset(&started, 0, sizeof(started));
	started.generation = generation;
	started.acknowledgement_deadline =
	    now_us + INTEL_AX211_SCAN_ACK_TIMEOUT_US;
	started.scan_deadline = now_us + INTEL_AX211_SCAN_WATCHDOG_US;
	started.phase = INTEL_AX211_SCAN_PHASE_WAIT_ACK;
	for (index = 0U; index < profile->channel_count; index++)
		started.requested_channels |= (uint16_t)(UINT16_C(1) <<
		    profile->channel[index]);
	*state = started;
	return INTEL_AX211_SCAN_OK;
}

int
intel_ax211_scan_request_ack(
	struct intel_ax211_scan_state *state,
	uint32_t generation,
	uint64_t now_us)
{
	if (state == NULL || generation == 0U)
		return INTEL_AX211_SCAN_INVALID;
	if (generation != state->generation)
		return INTEL_AX211_SCAN_STALE;
	if (state->phase == INTEL_AX211_SCAN_PHASE_RUNNING ||
	    state->phase == INTEL_AX211_SCAN_PHASE_TERMINAL)
		return INTEL_AX211_SCAN_DUPLICATE;
	if (state->phase != INTEL_AX211_SCAN_PHASE_WAIT_ACK)
		return INTEL_AX211_SCAN_OUT_OF_ORDER;
	if (now_us >= state->acknowledgement_deadline) {
		state->phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		return INTEL_AX211_SCAN_TIMEOUT;
	}
	state->phase = INTEL_AX211_SCAN_PHASE_RUNNING;
	return INTEL_AX211_SCAN_OK;
}

static int
ax211_scan_event_header(
	const struct intel_ax211_scan_state *state,
	const struct intel_ax211_protocol_message *message,
	uint64_t now_us)
{
	if (message == NULL || message->generation == 0U)
		return INTEL_AX211_SCAN_INVALID;
	if (message->generation != state->generation)
		return INTEL_AX211_SCAN_STALE;
	if (state->phase == INTEL_AX211_SCAN_PHASE_TERMINAL)
		return INTEL_AX211_SCAN_DUPLICATE;
	if (state->phase != INTEL_AX211_SCAN_PHASE_RUNNING)
		return INTEL_AX211_SCAN_OUT_OF_ORDER;
	if (now_us >= state->scan_deadline)
		return INTEL_AX211_SCAN_TIMEOUT;
	if (message->group != INTEL_AX211_SCAN_GROUP_LEGACY ||
	    message->version != INTEL_AX211_SCAN_NOTIFICATION_VERSION)
		return INTEL_AX211_SCAN_UNSUPPORTED;
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_SCAN_FAILED;
	if (message->payload == NULL)
		return INTEL_AX211_SCAN_INVALID;
	return INTEL_AX211_SCAN_OK;
}

static int
ax211_scan_status_result(uint8_t status)
{
	if (status == AX211_SCAN_STATUS_COMPLETED)
		return INTEL_AX211_SCAN_COMPLETE;
	if (status == AX211_SCAN_STATUS_ABORTED)
		return INTEL_AX211_SCAN_ABORTED;
	return INTEL_AX211_SCAN_FAILED;
}

static int
ax211_scan_complete_decode(
	const struct intel_ax211_protocol_message *message,
	struct intel_ax211_scan_event *event)
{
	const uint8_t *bytes;

	if (message->payload_length < 16U)
		return INTEL_AX211_SCAN_TRUNCATED;
	if (message->payload_length > 16U)
		return INTEL_AX211_SCAN_OVERSIZED;
	bytes = message->payload;
	if (ax211_scan_get_le32(bytes) != INTEL_AX211_SCAN_UID)
		return INTEL_AX211_SCAN_OUT_OF_ORDER;
	memset(event, 0, sizeof(*event));
	event->kind = INTEL_AX211_SCAN_EVENT_COMPLETE;
	event->last_schedule = bytes[4U];
	event->last_iteration = bytes[5U];
	event->status = bytes[6U];
	event->ebs_status = bytes[7U];
	if (event->last_schedule != 0U || event->last_iteration > 1U ||
	    ax211_scan_get_le32(bytes + 12U) != 0U)
		return INTEL_AX211_SCAN_UNSUPPORTED;
	return ax211_scan_status_result(event->status);
}

static int
ax211_scan_iteration_decode(
	const struct intel_ax211_scan_state *state,
	const struct intel_ax211_protocol_message *message,
	struct intel_ax211_scan_event *event)
{
	uint16_t seen;
	const uint8_t *bytes;
	size_t count;
	size_t expected;
	size_t index;

	if (message->payload_length < 16U)
		return INTEL_AX211_SCAN_TRUNCATED;
	bytes = message->payload;
	if (ax211_scan_get_le32(bytes) != INTEL_AX211_SCAN_UID)
		return INTEL_AX211_SCAN_OUT_OF_ORDER;
	count = bytes[4U];
	if (count > INTEL_AX211_SCAN_CHANNEL_LIMIT)
		return INTEL_AX211_SCAN_OVERSIZED;
	expected = 16U + count * 8U;
	if (message->payload_length < expected)
		return INTEL_AX211_SCAN_TRUNCATED;
	if (message->payload_length > expected)
		return INTEL_AX211_SCAN_OVERSIZED;
	memset(event, 0, sizeof(*event));
	event->kind = INTEL_AX211_SCAN_EVENT_ITERATION_COMPLETE;
	event->channel_count = count;
	event->status = bytes[5U];
	event->bluetooth_status = bytes[6U];
	event->last_channel = bytes[7U];
	event->tsf = (uint64_t)ax211_scan_get_le32(bytes + 8U) |
	    ((uint64_t)ax211_scan_get_le32(bytes + 12U) << 32);
	if (event->last_channel != 0U &&
	    (event->last_channel > INTEL_AX211_SCAN_CHANNEL_LIMIT ||
	    (state->requested_channels & (UINT16_C(1) <<
	    event->last_channel)) == 0U))
		return INTEL_AX211_SCAN_UNSUPPORTED;
	seen = 0U;
	for (index = 0U; index < count; index++) {
		const uint8_t *result;
		uint8_t channel;
		uint16_t bit;

		result = bytes + 16U + index * 8U;
		channel = result[0U];
		if (result[1U] != 1U || channel == 0U ||
		    channel > INTEL_AX211_SCAN_CHANNEL_LIMIT)
			return INTEL_AX211_SCAN_UNSUPPORTED;
		bit = (uint16_t)(UINT16_C(1) << channel);
		if ((state->requested_channels & bit) == 0U ||
		    (seen & bit) != 0U)
			return INTEL_AX211_SCAN_UNSUPPORTED;
		seen |= bit;
		event->channel[index].channel = channel;
		event->channel[index].probe_status = result[2U];
		event->channel[index].probe_not_sent = result[3U];
		event->channel[index].duration =
		    ax211_scan_get_le32(result + 4U);
	}
	return ax211_scan_status_result(event->status);
}

int
intel_ax211_scan_event_accept(
	struct intel_ax211_scan_state *state,
	const struct intel_ax211_protocol_message *message,
	uint64_t now_us,
	struct intel_ax211_scan_event *event)
{
	int result;

	if (state == NULL || event == NULL)
		return INTEL_AX211_SCAN_INVALID;
	result = ax211_scan_event_header(state, message, now_us);
	if (result == INTEL_AX211_SCAN_TIMEOUT) {
		state->phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		state->abort_required = 1U;
		return result;
	}
	if (result != INTEL_AX211_SCAN_OK)
		return result;
	if (message->opcode == INTEL_AX211_SCAN_COMPLETE_OPCODE)
		result = ax211_scan_complete_decode(message, event);
	else if (message->opcode ==
	    INTEL_AX211_SCAN_ITERATION_COMPLETE_OPCODE)
		result = ax211_scan_iteration_decode(state, message, event);
	else
		return INTEL_AX211_SCAN_UNSUPPORTED;
	if (result == INTEL_AX211_SCAN_COMPLETE ||
	    result == INTEL_AX211_SCAN_ABORTED ||
	    result == INTEL_AX211_SCAN_FAILED)
		state->phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
	return result;
}

int
intel_ax211_scan_expire(
	struct intel_ax211_scan_state *state,
	uint64_t now_us)
{
	if (state == NULL || state->generation == 0U)
		return INTEL_AX211_SCAN_INVALID;
	if (state->phase == INTEL_AX211_SCAN_PHASE_TERMINAL)
		return INTEL_AX211_SCAN_DUPLICATE;
	if (state->phase == INTEL_AX211_SCAN_PHASE_WAIT_ACK) {
		if (now_us < state->acknowledgement_deadline)
			return INTEL_AX211_SCAN_OK;
		state->phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		return INTEL_AX211_SCAN_TIMEOUT;
	}
	if (state->phase == INTEL_AX211_SCAN_PHASE_RUNNING) {
		if (now_us < state->scan_deadline)
			return INTEL_AX211_SCAN_OK;
		state->phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		state->abort_required = 1U;
		return INTEL_AX211_SCAN_TIMEOUT;
	}
	return INTEL_AX211_SCAN_OUT_OF_ORDER;
}
