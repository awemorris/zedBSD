/*
 * zedBSD Intel AX211 private firmware command transactions
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

#include "intel-ax211-command.h"

#include <string.h>

static int ax211_command_transaction_valid(const struct intel_ax211_command_transaction *transaction);
static int ax211_command_request_valid(const struct intel_ax211_command_request *request);
static uint32_t ax211_command_next_generation(struct intel_ax211_command_transaction *transaction);
static int ax211_command_protocol_result(int result);
static int ax211_command_prepare_result(int result);
static int ax211_command_inactive_handle_result(const struct intel_ax211_command_transaction *transaction, const struct intel_ax211_command_handle *handle);
static int ax211_command_inactive_event_result(const struct intel_ax211_command_transaction *transaction, uint8_t index, uint32_t hardware_epoch);
static void ax211_command_entry_clear(struct intel_ax211_command_transaction *transaction, uint8_t index);

/* Validates one bound logical transaction without inspecting DMA storage. */
static int
ax211_command_transaction_valid(
	const struct intel_ax211_command_transaction *transaction)
{
	if (transaction == NULL || !transaction->initialized ||
	    transaction->transport == NULL || transaction->hardware_epoch == 0U)
		return 0;
	if (transaction->max_pending == 0U ||
	    transaction->max_pending > INTEL_AX211_COMMAND_MAX_PENDING ||
	    transaction->pending_count > transaction->max_pending)
		return 0;
	return 1;
}

/* Validates one bounded inline request before reserving hardware state. */
static int
ax211_command_request_valid(
	const struct intel_ax211_command_request *request)
{
	if (request == NULL ||
	    request->command.version == INTEL_AX211_PROTOCOL_UNKNOWN_VERSION ||
	    request->payload_length > INTEL_AX211_COMMAND_EXTERNAL_PAYLOAD_SIZE ||
	    (request->payload_length != 0U && request->payload == NULL) ||
	    request->response_version ==
	    INTEL_AX211_PROTOCOL_UNKNOWN_VERSION ||
	    request->minimum_response_length >
	    request->maximum_response_length ||
	    request->maximum_response_length >
	    INTEL_AX211_MAX_COMMAND_PAYLOAD)
		return 0;
	return 1;
}

/* Produces a nonzero host-only handle generation. */
static uint32_t
ax211_command_next_generation(
	struct intel_ax211_command_transaction *transaction)
{
	uint32_t generation;

	generation = transaction->next_generation;
	transaction->next_generation++;
	if (transaction->next_generation == 0U)
		transaction->next_generation = 1U;
	return generation;
}

/* Maps exact protocol validation outcomes into the command contract. */
static int
ax211_command_protocol_result(
	int result)
{
	switch (result) {
	case INTEL_AX211_PROTOCOL_OK:
		return INTEL_AX211_COMMAND_OK;
	case INTEL_AX211_PROTOCOL_FAILED:
		return INTEL_AX211_COMMAND_FIRMWARE_FAILED;
	case INTEL_AX211_PROTOCOL_UNSUPPORTED:
		return INTEL_AX211_COMMAND_VERSION_MISMATCH;
	case INTEL_AX211_PROTOCOL_STALE:
		return INTEL_AX211_COMMAND_STALE;
	case INTEL_AX211_PROTOCOL_TOKEN_MISMATCH:
		return INTEL_AX211_COMMAND_OUT_OF_ORDER;
	case INTEL_AX211_PROTOCOL_TRUNCATED:
	case INTEL_AX211_PROTOCOL_OVERSIZED:
	case INTEL_AX211_PROTOCOL_INVALID:
	case INTEL_AX211_PROTOCOL_MISSING:
	case INTEL_AX211_PROTOCOL_DUPLICATE:
	default:
		return INTEL_AX211_COMMAND_MALFORMED;
	}
}

/* Maps a definite pre-doorbell transport result. */
static int
ax211_command_prepare_result(
	int result)
{
	if (result == INTEL_AX211_TRANSPORT_FULL)
		return INTEL_AX211_COMMAND_FULL;
	if (result == INTEL_AX211_TRANSPORT_INVALID)
		return INTEL_AX211_COMMAND_INVALID;
	if (result == INTEL_AX211_TRANSPORT_IO)
		return INTEL_AX211_COMMAND_ENCODE_FAILED;
	return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
}

/* Classifies an inactive handle using its host-only logical generation. */
static int
ax211_command_inactive_handle_result(
	const struct intel_ax211_command_transaction *transaction,
	const struct intel_ax211_command_handle *handle)
{
	if (transaction->last_generation[handle->token.index] ==
	    handle->generation && handle->generation != 0U)
		return INTEL_AX211_COMMAND_DUPLICATE;
	return INTEL_AX211_COMMAND_STALE;
}

/* Classifies an inactive event using its controller-supplied hardware epoch. */
static int
ax211_command_inactive_event_result(
	const struct intel_ax211_command_transaction *transaction,
	uint8_t index,
	uint32_t hardware_epoch)
{
	if (transaction->last_completion_epoch[index] == hardware_epoch &&
	    hardware_epoch != 0U)
		return INTEL_AX211_COMMAND_DUPLICATE;
	return INTEL_AX211_COMMAND_STALE;
}

/* Retires logical metadata only after transport retired the same slot. */
static void
ax211_command_entry_clear(
	struct intel_ax211_command_transaction *transaction,
	uint8_t index)
{
	uint32_t generation;

	generation = transaction->entry[index].logical_generation;
	intel_ax211_scrub(&transaction->entry[index],
	    sizeof(transaction->entry[index]));
	transaction->last_generation[index] = generation;
	transaction->last_completion_epoch[index] =
	    transaction->hardware_epoch;
	transaction->pending_count--;
}

/* Binds logical command metadata to one empty transport command ring. */
int
intel_ax211_command_transaction_init(
	struct intel_ax211_command_transaction *transaction,
	struct intel_ax211_transport *transport,
	size_t max_pending,
	uint32_t hardware_epoch)
{
	if (transaction == NULL || transport == NULL || hardware_epoch == 0U ||
	    max_pending == 0U || max_pending > INTEL_AX211_COMMAND_MAX_PENDING)
		return INTEL_AX211_COMMAND_INVALID;
	if (!transport->rings_initialized || transport->command_prepared ||
	    transport->command_reset_required ||
	    intel_ax211_transport_command_pending_count(transport) != 0U)
		return INTEL_AX211_COMMAND_INVALID;
	memset(transaction, 0, sizeof(*transaction));
	transaction->transport = transport;
	transaction->max_pending = max_pending;
	transaction->next_generation = 1U;
	transaction->hardware_epoch = hardware_epoch;
	transaction->initialized = 1U;
	return INTEL_AX211_COMMAND_OK;
}

int
intel_ax211_command_nvm_access_complete_encode(
	uint8_t output[4])
{
	if (output == NULL)
		return INTEL_AX211_COMMAND_INVALID;
	memset(output, 0, 4U);
	return INTEL_AX211_COMMAND_OK;
}

int
intel_ax211_command_nvm_get_info_encode(
	uint8_t output[4])
{
	if (output == NULL)
		return INTEL_AX211_COMMAND_INVALID;
	memset(output, 0, 4U);
	return INTEL_AX211_COMMAND_OK;
}

/* Installs metadata between DMA preparation and the command doorbell. */
int
intel_ax211_command_submit(
	struct intel_ax211_command_transaction *transaction,
	const struct intel_ax211_command_request *request,
	uint64_t now,
	uint64_t timeout,
	struct intel_ax211_command_handle *handle)
{
	struct intel_ax211_command_handle submitted;
	struct intel_ax211_command_entry *entry;
	int abort_result;
	int result;

	if (!ax211_command_transaction_valid(transaction) ||
	    !ax211_command_request_valid(request) || handle == NULL ||
	    timeout == 0U || now > UINT64_MAX - timeout)
		return INTEL_AX211_COMMAND_INVALID;
	if (transaction->poisoned)
		return INTEL_AX211_COMMAND_POISONED;
	if (transaction->pending_count >= transaction->max_pending)
		return INTEL_AX211_COMMAND_FULL;

	if (request->payload_length <=
	    INTEL_AX211_COMMAND_INLINE_PAYLOAD_SIZE)
		result = intel_ax211_transport_command_prepare_inline(
		    transaction->transport, &request->command, request->payload,
		    request->payload_length, &submitted.token);
	else
		result = intel_ax211_transport_command_prepare_external(
		    transaction->transport, &request->command, request->payload,
		    request->payload_length, &submitted.token);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return ax211_command_prepare_result(result);
	entry = &transaction->entry[submitted.token.index];
	if (entry->active) {
		abort_result = intel_ax211_transport_command_abort_prepared(
		    transaction->transport, &submitted.token);
		if (abort_result != INTEL_AX211_TRANSPORT_OK)
			transaction->poisoned = 1U;
		return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
	}

	submitted.generation = ax211_command_next_generation(transaction);
	submitted.hardware_epoch = transaction->hardware_epoch;
	intel_ax211_scrub(entry, sizeof(*entry));
	entry->active = 1U;
	entry->logical_generation = submitted.generation;
	entry->deadline = now + timeout;
	entry->pending.opcode = request->command.opcode;
	entry->pending.group = request->command.group;
	entry->pending.response_version = request->response_version;
	entry->pending.queue = submitted.token.queue;
	entry->pending.index = submitted.token.index;
	entry->pending.generation = transaction->hardware_epoch;
	entry->pending.minimum_response_length =
	    request->minimum_response_length;
	entry->pending.maximum_response_length =
	    request->maximum_response_length;
	transaction->last_generation[submitted.token.index] = 0U;
	transaction->last_completion_epoch[submitted.token.index] = 0U;
	transaction->pending_count++;
	*handle = submitted;

	result = intel_ax211_transport_command_publish(transaction->transport,
	    &submitted.token);
	if (result == INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_COMMAND_OK;
	if (result == INTEL_AX211_TRANSPORT_AMBIGUOUS) {
		entry->abandoned = 1U;
		transaction->poisoned = 1U;
		return INTEL_AX211_COMMAND_DOORBELL_FAILED;
	}

	/* A definite pre-doorbell failure may safely undo the prepared slot. */
	abort_result = intel_ax211_transport_command_abort_prepared(
	    transaction->transport, &submitted.token);
	if (abort_result == INTEL_AX211_TRANSPORT_OK) {
		intel_ax211_scrub(entry, sizeof(*entry));
		transaction->pending_count--;
		return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
	}
	entry->abandoned = 1U;
	transaction->poisoned = 1U;
	return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
}

int
intel_ax211_command_submit_nvm_access_complete(
	struct intel_ax211_command_transaction *transaction,
	uint64_t now,
	uint64_t timeout,
	struct intel_ax211_command_handle *handle)
{
	struct intel_ax211_command_request request;
	uint8_t payload[4];

	if (intel_ax211_command_nvm_access_complete_encode(payload) !=
	    INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_COMMAND_INVALID;
	memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM;
	request.command.opcode =
	    INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE;
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;
	request.minimum_response_length = 0U;
	request.maximum_response_length = 0U;
	return intel_ax211_command_submit(transaction, &request, now, timeout,
	    handle);
}

int
intel_ax211_command_submit_nvm_get_info(
	struct intel_ax211_command_transaction *transaction,
	uint64_t now,
	uint64_t timeout,
	struct intel_ax211_command_handle *handle)
{
	struct intel_ax211_command_request request;
	uint8_t payload[4];

	if (intel_ax211_command_nvm_get_info_encode(payload) !=
	    INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_COMMAND_INVALID;
	memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM;
	request.command.opcode = INTEL_AX211_PROTOCOL_NVM_GET_INFO_OPCODE;
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version =
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_VERSION;
	request.minimum_response_length =
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE;
	request.maximum_response_length =
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE;
	return intel_ax211_command_submit(transaction, &request, now, timeout,
	    handle);
}

/* Completes the oldest token using a controller-stamped hardware epoch. */
int
intel_ax211_command_complete(
	struct intel_ax211_command_transaction *transaction,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	void *response,
	size_t response_capacity,
	size_t *response_length)
{
	struct intel_ax211_protocol_message message;
	struct intel_ax211_command_entry *entry;
	struct intel_ax211_ring_token oldest;
	struct intel_ax211_event event;
	uint8_t wire_group;
	int protocol_result;
	int result;
	int transport_result;

	if (!ax211_command_transaction_valid(transaction) ||
	    event_bytes == NULL || response_length == NULL ||
	    hardware_epoch == 0U)
		return INTEL_AX211_COMMAND_INVALID;
	if (hardware_epoch != transaction->hardware_epoch)
		return INTEL_AX211_COMMAND_STALE;
	result = intel_ax211_event_decode(event_bytes, event_length, &event);
	if (result != INTEL_AX211_OK)
		return INTEL_AX211_COMMAND_MALFORMED;
	if ((event.queue & 0x80U) != 0U)
		return INTEL_AX211_COMMAND_OUT_OF_ORDER;
	entry = &transaction->entry[event.index];
	if (!entry->active)
		return ax211_command_inactive_event_result(transaction,
		    event.index, hardware_epoch);
	if (event.queue != entry->pending.queue)
		return INTEL_AX211_COMMAND_OUT_OF_ORDER;
	transport_result = intel_ax211_transport_command_oldest(
	    transaction->transport, &oldest);
	if (transport_result != INTEL_AX211_TRANSPORT_OK ||
	    oldest.queue != event.queue || oldest.index != event.index)
		return INTEL_AX211_COMMAND_OUT_OF_ORDER;
	memset(&message, 0, sizeof(message));
	message.opcode = event.command.opcode;
	wire_group = event.flags &
	    (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	/* Firmware reports the LONG_GROUP carrier used for an API-89 legacy
	 * command.  Restore its logical group only for the matching pending
	 * token; genuine long-group commands retain their wire identity. */
	if (entry->pending.group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
	    wire_group == INTEL_AX211_PROTOCOL_GROUP_LONG)
		message.group = INTEL_AX211_PROTOCOL_GROUP_LEGACY;
	else
		message.group = wire_group;
	/* Response layout comes from the pinned command-version table, not wire. */
	message.version = entry->pending.response_version;
	message.flags = event.flags;
	message.queue = event.queue;
	message.index = event.index;
	message.generation = hardware_epoch;
	message.payload = event_bytes + event.payload_offset;
	message.payload_length = event.payload_length;
	protocol_result = intel_ax211_protocol_command_response_validate(
	    &message, &entry->pending);
	result = ax211_command_protocol_result(protocol_result);
	*response_length = 0U;
	if (result == INTEL_AX211_COMMAND_OK &&
	    (message.payload_length > response_capacity ||
	    (message.payload_length != 0U && response == NULL)))
		return INTEL_AX211_COMMAND_BUFFER_TOO_SMALL;

	transport_result = intel_ax211_transport_command_complete(
	    transaction->transport, &oldest);
	if (transport_result != INTEL_AX211_TRANSPORT_OK) {
		entry->abandoned = 1U;
		transaction->poisoned = 1U;
		return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
	}
	if (result == INTEL_AX211_COMMAND_OK && message.payload_length != 0U)
		memcpy(response, message.payload, message.payload_length);
	if (result == INTEL_AX211_COMMAND_OK)
		*response_length = message.payload_length;
	ax211_command_entry_clear(transaction, event.index);
	return result;
}

/* Abandons one command without retiring or reusing its hardware slot. */
int
intel_ax211_command_cancel(
	struct intel_ax211_command_transaction *transaction,
	const struct intel_ax211_command_handle *handle)
{
	struct intel_ax211_command_entry *entry;

	if (!ax211_command_transaction_valid(transaction) || handle == NULL ||
	    handle->hardware_epoch != transaction->hardware_epoch)
		return INTEL_AX211_COMMAND_INVALID;
	entry = &transaction->entry[handle->token.index];
	if (!entry->active)
		return ax211_command_inactive_handle_result(transaction, handle);
	if (entry->logical_generation != handle->generation ||
	    entry->pending.queue != handle->token.queue)
		return INTEL_AX211_COMMAND_STALE;
	if (entry->abandoned)
		return INTEL_AX211_COMMAND_DUPLICATE;
	entry->abandoned = 1U;
	transaction->poisoned = 1U;
	return INTEL_AX211_COMMAND_OK;
}

/* Times out the oldest command without making its DMA slot reusable. */
int
intel_ax211_command_timeout_oldest(
	struct intel_ax211_command_transaction *transaction,
	uint64_t now,
	struct intel_ax211_command_handle *handle)
{
	struct intel_ax211_command_entry *entry;
	struct intel_ax211_ring_token oldest;
	int result;

	if (!ax211_command_transaction_valid(transaction) || handle == NULL)
		return INTEL_AX211_COMMAND_INVALID;
	if (transaction->pending_count == 0U)
		return INTEL_AX211_COMMAND_EMPTY;
	result = intel_ax211_transport_command_oldest(transaction->transport,
	    &oldest);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_COMMAND_MALFORMED;
	entry = &transaction->entry[oldest.index];
	if (!entry->active)
		return INTEL_AX211_COMMAND_MALFORMED;
	handle->token = oldest;
	handle->generation = entry->logical_generation;
	handle->hardware_epoch = transaction->hardware_epoch;
	if (entry->abandoned)
		return INTEL_AX211_COMMAND_POISONED;
	if (now < entry->deadline)
		return INTEL_AX211_COMMAND_PENDING;
	entry->abandoned = 1U;
	transaction->poisoned = 1U;
	return INTEL_AX211_COMMAND_TIMEOUT;
}

/* Abandons all commands while retaining every hardware-owned slot. */
void
intel_ax211_command_cancel_all(
	struct intel_ax211_command_transaction *transaction)
{
	size_t index;
	int found;

	if (!ax211_command_transaction_valid(transaction))
		return;
	found = 0;
	for (index = 0U; index < INTEL_AX211_COMMAND_ENTRY_COUNT; index++) {
		if (transaction->entry[index].active) {
			transaction->entry[index].abandoned = 1U;
			found = 1;
		}
	}
	if (found)
		transaction->poisoned = 1U;
}

/* Discards logical metadata only after transport completed reset cleanup. */
int
intel_ax211_command_after_device_reset(
	struct intel_ax211_command_transaction *transaction,
	uint32_t hardware_epoch)
{
	if (!ax211_command_transaction_valid(transaction) ||
	    hardware_epoch == 0U || hardware_epoch == transaction->hardware_epoch)
		return INTEL_AX211_COMMAND_INVALID;
	if (intel_ax211_transport_command_pending_count(
	    transaction->transport) != 0U ||
	    transaction->transport->command_prepared ||
	    transaction->transport->command_reset_required ||
	    !transaction->transport->command_reset_completed)
		return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
	intel_ax211_scrub(transaction->entry, sizeof(transaction->entry));
	intel_ax211_scrub(transaction->last_generation,
	    sizeof(transaction->last_generation));
	intel_ax211_scrub(transaction->last_completion_epoch,
	    sizeof(transaction->last_completion_epoch));
	transaction->pending_count = 0U;
	transaction->hardware_epoch = hardware_epoch;
	transaction->poisoned = 0U;
	transaction->transport->command_reset_completed = 0U;
	if (transaction->next_generation == 0U)
		transaction->next_generation = 1U;
	return INTEL_AX211_COMMAND_OK;
}

size_t
intel_ax211_command_pending_count(
	const struct intel_ax211_command_transaction *transaction)
{
	if (!ax211_command_transaction_valid(transaction))
		return 0U;
	return transaction->pending_count;
}

int
intel_ax211_command_is_poisoned(
	const struct intel_ax211_command_transaction *transaction)
{
	if (!ax211_command_transaction_valid(transaction))
		return 0;
	return transaction->poisoned != 0U;
}
