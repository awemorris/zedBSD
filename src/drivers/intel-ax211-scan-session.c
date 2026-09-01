/*
 * zedBSD Intel AX211 private API89 scan-session coordinator
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

#include "intel-ax211-scan-session.h"

#include <string.h>

static int ax211_scan_session_live(
	const struct intel_ax211_scan_session *session);
static int ax211_scan_session_command_result(int result);
static int ax211_scan_session_scan_result(int result);
static int ax211_scan_session_channel_profile(
	struct intel_ax211_scan_session *session, uint8_t channel);
static int ax211_scan_session_submit(
	struct intel_ax211_scan_session *session, uint8_t opcode,
	const void *payload, size_t payload_length, uint64_t now_us,
	uint64_t timeout_us, uint8_t phase);
static int ax211_scan_session_ack(
	struct intel_ax211_scan_session *session, uint8_t required_phase,
	const uint8_t *event_bytes, size_t event_length,
	uint32_t hardware_epoch, uint64_t now_us);
static int ax211_scan_session_terminal(
	struct intel_ax211_scan_session *session, int result);

int
intel_ax211_scan_session_init(
	struct intel_ax211_scan_session *session,
	struct intel_ax211_command_transaction *commands,
	const struct intel_ax211_protocol_command_table *command_table,
	const struct intel_ax211_protocol_nvm *nvm,
	const struct intel_ax211_runtime_mcc *mcc,
	const uint8_t station_address[6],
	uint32_t hardware_epoch)
{
	struct intel_ax211_protocol_command_table copied;
	int result;

	if (session == NULL || commands == NULL || command_table == NULL ||
	    command_table->bytes == NULL || nvm == NULL ||
	    station_address == NULL || hardware_epoch == 0U ||
	    command_table->count != INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT ||
	    !commands->initialized || commands->hardware_epoch != hardware_epoch ||
	    intel_ax211_command_pending_count(commands) != 0U ||
	    intel_ax211_command_is_poisoned(commands))
		return INTEL_AX211_SCAN_SESSION_INVALID;

	memset(session, 0, sizeof(*session));
	memcpy(session->command_version_bytes, command_table->bytes,
	    sizeof(session->command_version_bytes));
	result = intel_ax211_protocol_command_table_parse(
	    session->command_version_bytes,
	    sizeof(session->command_version_bytes), &copied);
	if (result != INTEL_AX211_PROTOCOL_OK ||
	    intel_ax211_scan_api89_validate(&copied) != INTEL_AX211_SCAN_OK) {
		memset(session, 0, sizeof(*session));
		return INTEL_AX211_SCAN_SESSION_UNSUPPORTED;
	}
	result = intel_ax211_scan_profile_from_nvm(nvm, mcc,
	    station_address, &session->full_profile);
	if (result != INTEL_AX211_SCAN_OK) {
		memset(session, 0, sizeof(*session));
		return ax211_scan_session_scan_result(result);
	}
	session->commands = commands;
	session->command_table.bytes = session->command_version_bytes;
	session->command_table.count = copied.count;
	session->hardware_epoch = hardware_epoch;
	session->phase = INTEL_AX211_SCAN_SESSION_IDLE;
	session->initialized = 1U;
	return INTEL_AX211_SCAN_SESSION_OK;
}

int
intel_ax211_scan_session_begin_channel(
	struct intel_ax211_scan_session *session,
	uint64_t common_generation,
	uint8_t channel,
	uint64_t now_us)
{
	int result;

	if (!ax211_scan_session_live(session) || common_generation == 0U)
		return INTEL_AX211_SCAN_SESSION_INVALID;
	if (session->phase != INTEL_AX211_SCAN_SESSION_IDLE &&
	    session->phase != INTEL_AX211_SCAN_SESSION_TERMINAL)
		return INTEL_AX211_SCAN_SESSION_BUSY;
	if (session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL &&
	    session->terminal_result == INTEL_AX211_SCAN_SESSION_TIMEOUT)
		return INTEL_AX211_SCAN_SESSION_BUSY;
	if (intel_ax211_command_pending_count(session->commands) != 0U ||
	    intel_ax211_command_is_poisoned(session->commands))
		return INTEL_AX211_SCAN_SESSION_COMMAND;
	result = ax211_scan_session_channel_profile(session, channel);
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return result;
	result = intel_ax211_scan_request_encode(&session->channel_profile,
	    session->request);
	if (result != INTEL_AX211_SCAN_OK)
		return ax211_scan_session_scan_result(result);
	result = intel_ax211_scan_begin(&session->scan,
	    &session->command_table, &session->channel_profile,
	    session->hardware_epoch, now_us);
	if (result != INTEL_AX211_SCAN_OK)
		return ax211_scan_session_scan_result(result);

	session->common_generation = common_generation;
	session->channel = channel;
	session->terminal_result = INTEL_AX211_SCAN_SESSION_OK;
	result = ax211_scan_session_submit(session,
	    INTEL_AX211_SCAN_REQUEST_OPCODE, session->request,
	    sizeof(session->request), now_us,
	    INTEL_AX211_SCAN_SESSION_COMMAND_TIMEOUT_US,
	    INTEL_AX211_SCAN_SESSION_WAIT_START_ACK);
	if (result != INTEL_AX211_SCAN_SESSION_OK) {
		memset(&session->scan, 0, sizeof(session->scan));
		session->phase = INTEL_AX211_SCAN_SESSION_TERMINAL;
		session->terminal_result = (uint8_t)result;
	}
	return result;
}

int
intel_ax211_scan_session_start_ack(
	struct intel_ax211_scan_session *session,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	uint64_t now_us)
{
	int result;

	result = ax211_scan_session_ack(session,
	    INTEL_AX211_SCAN_SESSION_WAIT_START_ACK, event_bytes,
	    event_length, hardware_epoch, now_us);
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return result;
	result = intel_ax211_scan_request_ack(&session->scan,
	    session->hardware_epoch, now_us);
	if (result != INTEL_AX211_SCAN_OK)
		return ax211_scan_session_terminal(session,
		    ax211_scan_session_scan_result(result));
	session->phase = INTEL_AX211_SCAN_SESSION_RUNNING;
	return INTEL_AX211_SCAN_SESSION_OK;
}

int
intel_ax211_scan_session_notification(
	struct intel_ax211_scan_session *session,
	const struct intel_ax211_protocol_message *message,
	uint64_t now_us,
	struct intel_ax211_scan_session_event *event)
{
	struct intel_ax211_scan_event decoded;
	int result;

	if (!ax211_scan_session_live(session) || message == NULL ||
	    event == NULL)
		return INTEL_AX211_SCAN_SESSION_INVALID;
	if (session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL)
		return INTEL_AX211_SCAN_SESSION_DUPLICATE;
	if (session->phase != INTEL_AX211_SCAN_SESSION_RUNNING &&
	    session->phase != INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE)
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	if (session->phase == INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE &&
	    now_us >= session->command_deadline)
		return ax211_scan_session_terminal(session,
		    INTEL_AX211_SCAN_SESSION_TIMEOUT);
	memset(&decoded, 0, sizeof(decoded));
	result = intel_ax211_scan_event_accept(&session->scan, message,
	    now_us, &decoded);
	result = ax211_scan_session_scan_result(result);
	if (result == INTEL_AX211_SCAN_SESSION_COMPLETE ||
	    result == INTEL_AX211_SCAN_SESSION_ABORTED ||
	    result == INTEL_AX211_SCAN_SESSION_FAILED ||
	    result == INTEL_AX211_SCAN_SESSION_TIMEOUT)
		ax211_scan_session_terminal(session, result);
	if (result == INTEL_AX211_SCAN_SESSION_COMPLETE ||
	    result == INTEL_AX211_SCAN_SESSION_ABORTED ||
	    result == INTEL_AX211_SCAN_SESSION_FAILED ||
	    result == INTEL_AX211_SCAN_SESSION_OK) {
		memset(event, 0, sizeof(*event));
		event->common_generation = session->common_generation;
		event->channel = session->channel;
		event->firmware = decoded;
	}
	return result;
}

int
intel_ax211_scan_session_abort(
	struct intel_ax211_scan_session *session,
	uint64_t common_generation,
	uint64_t now_us)
{
	uint8_t payload[INTEL_AX211_SCAN_ABORT_SIZE];
	int result;

	if (!ax211_scan_session_live(session) || common_generation == 0U)
		return INTEL_AX211_SCAN_SESSION_INVALID;
	if (common_generation != session->common_generation)
		return INTEL_AX211_SCAN_SESSION_STALE;
	if (session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL &&
	    !session->scan.abort_required)
		return INTEL_AX211_SCAN_SESSION_DUPLICATE;
	if (session->phase != INTEL_AX211_SCAN_SESSION_RUNNING &&
	    !(session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL &&
	    session->scan.abort_required))
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	if (intel_ax211_scan_abort_encode(payload) != INTEL_AX211_SCAN_OK)
		return INTEL_AX211_SCAN_SESSION_FAILED;
	result = ax211_scan_session_submit(session,
	    INTEL_AX211_SCAN_ABORT_OPCODE, payload, sizeof(payload), now_us,
	    INTEL_AX211_SCAN_SESSION_ABORT_TIMEOUT_US,
	    INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK);
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return ax211_scan_session_terminal(session, result);
	return INTEL_AX211_SCAN_SESSION_OK;
}

int
intel_ax211_scan_session_abort_ack(
	struct intel_ax211_scan_session *session,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	uint64_t now_us)
{
	int result;

	result = ax211_scan_session_ack(session,
	    INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK, event_bytes,
	    event_length, hardware_epoch, now_us);
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return result;
	if (session->scan.abort_required) {
		session->scan.abort_required = 0U;
		return ax211_scan_session_terminal(session,
		    INTEL_AX211_SCAN_SESSION_ABORTED);
	}
	session->phase = INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE;
	return INTEL_AX211_SCAN_SESSION_OK;
}

int
intel_ax211_scan_session_expire(
	struct intel_ax211_scan_session *session,
	uint64_t now_us)
{
	struct intel_ax211_command_handle expired;
	int result;

	if (!ax211_scan_session_live(session))
		return INTEL_AX211_SCAN_SESSION_INVALID;
	if (session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL)
		return INTEL_AX211_SCAN_SESSION_DUPLICATE;
	if (session->phase == INTEL_AX211_SCAN_SESSION_WAIT_START_ACK ||
	    session->phase == INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK) {
		if (now_us < session->command_deadline)
			return INTEL_AX211_SCAN_SESSION_OK;
		memset(&expired, 0, sizeof(expired));
		result = intel_ax211_command_timeout_oldest(session->commands,
		    now_us, &expired);
		if (result == INTEL_AX211_COMMAND_PENDING)
			return INTEL_AX211_SCAN_SESSION_OK;
		if (result != INTEL_AX211_COMMAND_TIMEOUT)
			return ax211_scan_session_terminal(session,
			    ax211_scan_session_command_result(result));
		if (expired.token.queue != session->command_handle.token.queue ||
		    expired.token.index != session->command_handle.token.index ||
		    expired.generation != session->command_handle.generation)
			return ax211_scan_session_terminal(session,
			    INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER);
		return ax211_scan_session_terminal(session,
		    INTEL_AX211_SCAN_SESSION_TIMEOUT);
	}
	if (session->phase == INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE) {
		if (now_us < session->command_deadline)
			return INTEL_AX211_SCAN_SESSION_OK;
		session->scan.phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		session->scan.abort_required = 1U;
		return ax211_scan_session_terminal(session,
		    INTEL_AX211_SCAN_SESSION_TIMEOUT);
	}
	if (session->phase != INTEL_AX211_SCAN_SESSION_RUNNING)
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	result = intel_ax211_scan_expire(&session->scan, now_us);
	result = ax211_scan_session_scan_result(result);
	if (result == INTEL_AX211_SCAN_SESSION_TIMEOUT)
		return ax211_scan_session_terminal(session, result);
	return result;
}

static int
ax211_scan_session_live(
	const struct intel_ax211_scan_session *session)
{
	if (session == NULL || !session->initialized ||
	    session->commands == NULL || session->hardware_epoch == 0U)
		return 0;
	return session->commands->initialized &&
	    session->commands->hardware_epoch == session->hardware_epoch;
}

static int
ax211_scan_session_channel_profile(
	struct intel_ax211_scan_session *session,
	uint8_t channel)
{
	size_t index;

	for (index = 0U; index < session->full_profile.channel_count; index++) {
		if (session->full_profile.channel[index] == channel) {
			memset(&session->channel_profile, 0,
			    sizeof(session->channel_profile));
			memcpy(session->channel_profile.station_address,
			    session->full_profile.station_address, 6U);
			session->channel_profile.channel_width_mhz =
			    session->full_profile.channel_width_mhz;
			session->channel_profile.channel[0] = channel;
			session->channel_profile.channel_count = 1U;
			return INTEL_AX211_SCAN_SESSION_OK;
		}
	}
	return INTEL_AX211_SCAN_SESSION_UNSUPPORTED;
}

static int
ax211_scan_session_submit(
	struct intel_ax211_scan_session *session,
	uint8_t opcode,
	const void *payload,
	size_t payload_length,
	uint64_t now_us,
	uint64_t timeout_us,
	uint8_t phase)
{
	struct intel_ax211_command_request request;
	int result;

	if (now_us > UINT64_MAX - timeout_us)
		return INTEL_AX211_SCAN_SESSION_INVALID;
	memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_SCAN_GROUP_LONG;
	request.command.opcode = opcode;
	/* API89 layouts are 17/1; the Gen3 wide-header wire version is zero. */
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = payload_length;
	request.response_version = 0U;
	request.minimum_response_length = 0U;
	request.maximum_response_length = 0U;
	result = intel_ax211_command_submit(session->commands, &request,
	    now_us, timeout_us, &session->command_handle);
	result = ax211_scan_session_command_result(result);
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return result;
	session->command_deadline = now_us + timeout_us;
	session->phase = phase;
	return INTEL_AX211_SCAN_SESSION_OK;
}

static int
ax211_scan_session_ack(
	struct intel_ax211_scan_session *session,
	uint8_t required_phase,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	uint64_t now_us)
{
	struct intel_ax211_event event;
	size_t response_length;
	int result;

	if (!ax211_scan_session_live(session) || event_bytes == NULL ||
	    hardware_epoch == 0U)
		return INTEL_AX211_SCAN_SESSION_INVALID;
	if (hardware_epoch != session->hardware_epoch)
		return INTEL_AX211_SCAN_SESSION_STALE;
	if (session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL)
		return INTEL_AX211_SCAN_SESSION_DUPLICATE;
	if (session->phase != required_phase) {
		if ((required_phase ==
		    INTEL_AX211_SCAN_SESSION_WAIT_START_ACK &&
		    session->phase == INTEL_AX211_SCAN_SESSION_RUNNING) ||
		    (required_phase ==
		    INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK &&
		    session->phase ==
		    INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE))
			return INTEL_AX211_SCAN_SESSION_DUPLICATE;
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	}
	if (now_us >= session->command_deadline)
		return intel_ax211_scan_session_expire(session, now_us);
	if (intel_ax211_event_decode(event_bytes, event_length, &event) !=
	    INTEL_AX211_OK)
		return INTEL_AX211_SCAN_SESSION_COMMAND;
	if (event.queue != session->command_handle.token.queue ||
	    event.index != session->command_handle.token.index)
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	response_length = 0U;
	result = intel_ax211_command_complete(session->commands, event_bytes,
	    event_length, hardware_epoch, NULL, 0U, &response_length);
	result = ax211_scan_session_command_result(result);
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return ax211_scan_session_terminal(session, result);
	if (response_length != 0U)
		return ax211_scan_session_terminal(session,
		    INTEL_AX211_SCAN_SESSION_COMMAND);
	return INTEL_AX211_SCAN_SESSION_OK;
}

static int
ax211_scan_session_terminal(
	struct intel_ax211_scan_session *session,
	int result)
{
	session->phase = INTEL_AX211_SCAN_SESSION_TERMINAL;
	session->terminal_result = (uint8_t)result;
	return result;
}

static int
ax211_scan_session_command_result(
	int result)
{
	switch (result) {
	case INTEL_AX211_COMMAND_OK:
		return INTEL_AX211_SCAN_SESSION_OK;
	case INTEL_AX211_COMMAND_STALE:
		return INTEL_AX211_SCAN_SESSION_STALE;
	case INTEL_AX211_COMMAND_DUPLICATE:
		return INTEL_AX211_SCAN_SESSION_DUPLICATE;
	case INTEL_AX211_COMMAND_OUT_OF_ORDER:
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	case INTEL_AX211_COMMAND_TIMEOUT:
		return INTEL_AX211_SCAN_SESSION_TIMEOUT;
	case INTEL_AX211_COMMAND_FULL:
	case INTEL_AX211_COMMAND_PENDING:
		return INTEL_AX211_SCAN_SESSION_BUSY;
	case INTEL_AX211_COMMAND_INVALID:
		return INTEL_AX211_SCAN_SESSION_INVALID;
	default:
		return INTEL_AX211_SCAN_SESSION_COMMAND;
	}
}

static int
ax211_scan_session_scan_result(
	int result)
{
	switch (result) {
	case INTEL_AX211_SCAN_OK:
		return INTEL_AX211_SCAN_SESSION_OK;
	case INTEL_AX211_SCAN_INVALID:
		return INTEL_AX211_SCAN_SESSION_INVALID;
	case INTEL_AX211_SCAN_UNSUPPORTED:
		return INTEL_AX211_SCAN_SESSION_UNSUPPORTED;
	case INTEL_AX211_SCAN_STALE:
		return INTEL_AX211_SCAN_SESSION_STALE;
	case INTEL_AX211_SCAN_DUPLICATE:
		return INTEL_AX211_SCAN_SESSION_DUPLICATE;
	case INTEL_AX211_SCAN_OUT_OF_ORDER:
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	case INTEL_AX211_SCAN_TIMEOUT:
		return INTEL_AX211_SCAN_SESSION_TIMEOUT;
	case INTEL_AX211_SCAN_COMPLETE:
		return INTEL_AX211_SCAN_SESSION_COMPLETE;
	case INTEL_AX211_SCAN_ABORTED:
		return INTEL_AX211_SCAN_SESSION_ABORTED;
	case INTEL_AX211_SCAN_TRUNCATED:
	case INTEL_AX211_SCAN_OVERSIZED:
	case INTEL_AX211_SCAN_FAILED:
	default:
		return INTEL_AX211_SCAN_SESSION_FAILED;
	}
}
