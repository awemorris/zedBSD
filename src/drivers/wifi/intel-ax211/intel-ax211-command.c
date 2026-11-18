/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private firmware command transactions.
 */

#include "intel-ax211-command.h"
#include <kern/kcrt.h>

static int ax211_command_transaction_valid(const struct intel_ax211_command_transaction *transaction);
static int ax211_command_request_valid(const struct intel_ax211_command_request *request);
static int ax211_command_prepare_result(int result);
static uint32_t ax211_command_next_generation(struct intel_ax211_command_transaction *transaction);
static int ax211_command_inactive_event_result(const struct intel_ax211_command_transaction *transaction, uint8_t index, uint32_t hardware_epoch);
static int ax211_command_protocol_result(int result);
static void ax211_command_entry_clear(struct intel_ax211_command_transaction *transaction, uint8_t index);
static int ax211_command_inactive_handle_result(const struct intel_ax211_command_transaction *transaction, const struct intel_ax211_command_handle *handle);

/*
 * Binds logical command metadata to one empty transport command ring.
 */
int
drv_intel_ax211_command_transaction_init(
	struct intel_ax211_command_transaction *transaction,
	struct intel_ax211_transport *transport,
	size_t max_pending,
	uint32_t hardware_epoch)
{
	size_t pending;

	/*
	 * Refuses missing objects, an unset epoch and a pending limit out of
	 * range.
	 */
	if (transaction == NULL ||
	    transport == NULL ||
	    hardware_epoch == 0U ||
	    max_pending == 0U ||
	    max_pending > INTEL_AX211_COMMAND_MAX_PENDING) {
		return INTEL_AX211_COMMAND_INVALID;
	}

	/*
	 * Refuses a command ring that is not set up, or that holds a prepared
	 * command.
	 */
	if (!transport->rings_initialized ||
	    transport->command_prepared ||
	    transport->command_reset_required) {
		return INTEL_AX211_COMMAND_INVALID;
	}

	/* Refuses a command ring that still holds commands. */
	pending = drv_intel_ax211_transport_command_pending_count(transport);
	if (pending != 0U)
		return INTEL_AX211_COMMAND_INVALID;

	/* Binds the transaction to the ring; handle generations start at 1. */
	kern_memset(transaction, 0, sizeof(*transaction));
	transaction->transport = transport;
	transaction->max_pending = max_pending;
	transaction->next_generation = 1U;
	transaction->hardware_epoch = hardware_epoch;
	transaction->initialized = 1U;

	/* Succeeded: the transaction accepts commands. */
	return INTEL_AX211_COMMAND_OK;
}

/*
 * Encodes the payload of the NVM handoff command, one zero word.
 */
int
drv_intel_ax211_command_nvm_handoff_encode(
	uint8_t output[4])
{
	/* Refuses a missing output buffer. */
	if (output == NULL)
		return INTEL_AX211_COMMAND_INVALID;

	/* Writes the zero word. */
	kern_memset(output, 0, 4U);

	/* Succeeded. */
	return INTEL_AX211_COMMAND_OK;
}

/*
 * Encodes the payload of the NVM query command, one zero word.
 */
int
drv_intel_ax211_command_nvm_query_encode(
	uint8_t output[4])
{
	/* Refuses a missing output buffer. */
	if (output == NULL)
		return INTEL_AX211_COMMAND_INVALID;

	/* Writes the zero word. */
	kern_memset(output, 0, 4U);

	/* Succeeded. */
	return INTEL_AX211_COMMAND_OK;
}

/*
 * Installs metadata between DMA preparation and the command doorbell.
 */
int
drv_intel_ax211_command_submit(
	struct intel_ax211_command_transaction *transaction,
	const struct intel_ax211_command_request *request,
	uint64_t now,
	uint64_t timeout,
	struct intel_ax211_command_handle *handle)
{
	int error;
	struct intel_ax211_command_handle submitted;
	struct intel_ax211_command_entry *entry;
	int abort_result;
	int valid;
	int result;

	/* Refuses an unusable transaction. */
	valid = ax211_command_transaction_valid(transaction);
	if (!valid)
		return INTEL_AX211_COMMAND_INVALID;

	/* Refuses a malformed request. */
	valid = ax211_command_request_valid(request);
	if (!valid)
		return INTEL_AX211_COMMAND_INVALID;

	/*
	 * Refuses a missing handle and a timeout that is zero or overflows the
	 * clock.
	 */
	if (handle == NULL ||
	    timeout == 0U ||
	    now > UINT64_MAX - timeout) {
		return INTEL_AX211_COMMAND_INVALID;
	}

	/*
	 * Refuses new commands once a command was abandoned: the ring is no
	 * longer trusted.
	 */
	if (transaction->poisoned)
		return INTEL_AX211_COMMAND_POISONED;

	/* Refuses a command beyond the pending limit. */
	if (transaction->pending_count >= transaction->max_pending)
		return INTEL_AX211_COMMAND_FULL;

	/*
	 * Prepares the command in its slot, inline or through the external
	 * buffer.
	 */
	if (request->payload_length <=
	    INTEL_AX211_COMMAND_INLINE_PAYLOAD_SIZE) {
		result = drv_intel_ax211_transport_command_prepare_inline(
			transaction->transport, &request->command,
			request->payload, request->payload_length,
			&submitted.token);
	} else {
		result = drv_intel_ax211_transport_command_prepare_external(
			transaction->transport, &request->command,
			request->payload, request->payload_length,
			&submitted.token);
	}

	/* Reports why the command could not be prepared. */
	if (result != INTEL_AX211_TRANSPORT_OK) {
		error = ax211_command_prepare_result(result);
		return error;
	}

	/*
	 * Refuses a slot that the transaction still tracks: the ring and the
	 * transaction disagree.
	 */
	entry = &transaction->entry[submitted.token.index];
	if (entry->active) {
		/*
		 * Undoes the prepared slot; a failed undo leaves the ring
		 * untrusted.
		 */
		abort_result = drv_intel_ax211_transport_command_abort_prepared(
			transaction->transport, &submitted.token);
		if (abort_result != INTEL_AX211_TRANSPORT_OK)
			transaction->poisoned = 1U;

		/* Reports the disagreement as a transport failure. */
		return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
	}

	/* Records what this slot is waiting for. */
	submitted.generation = ax211_command_next_generation(transaction);
	submitted.hardware_epoch = transaction->hardware_epoch;
	drv_intel_ax211_scrub(entry, sizeof(*entry));
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

	/* Rings the doorbell. */
	result = drv_intel_ax211_transport_command_publish(
		transaction->transport, &submitted.token);
	if (result == INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_COMMAND_OK;

	/*
	 * An ambiguous doorbell may have reached the device: the slot stays
	 * owned.
	 */
	if (result == INTEL_AX211_TRANSPORT_AMBIGUOUS) {
		entry->abandoned = 1U;
		transaction->poisoned = 1U;
		return INTEL_AX211_COMMAND_DOORBELL_FAILED;
	}

	/*
	 * A definite pre-doorbell failure may safely undo the prepared slot.  The slot
	 * is forgotten when the undo succeeds.
	 */
	abort_result = drv_intel_ax211_transport_command_abort_prepared(
		transaction->transport, &submitted.token);
	if (abort_result == INTEL_AX211_TRANSPORT_OK) {
		drv_intel_ax211_scrub(entry, sizeof(*entry));
		transaction->pending_count--;
		return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
	}

	/* A failed undo leaves the slot owned and the ring untrusted. */
	entry->abandoned = 1U;
	transaction->poisoned = 1U;

	/* Reports the failed doorbell. */
	return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
	}

/*
 * Submits the NVM handoff command.
 */
int
drv_intel_ax211_command_submit_nvm_handoff(
	struct intel_ax211_command_transaction *transaction,
	uint64_t now,
	uint64_t timeout,
	struct intel_ax211_command_handle *handle)
{
	int error;
	struct intel_ax211_command_request request;
	uint8_t payload[4];
	int result;

	/* Encodes the payload. */
	result = drv_intel_ax211_command_nvm_handoff_encode(payload);
	if (result != INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_COMMAND_INVALID;

	/* Describes the command: no answer payload is expected. */
	kern_memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_PROTOCOL_FAMILY_REGULATORY_STORE;
	request.command.opcode =
		INTEL_AX211_PROTOCOL_NVM_HANDOFF_OPCODE;
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;
	request.minimum_response_length = 0U;
	request.maximum_response_length = 0U;

	/* Submits it. */
	error = drv_intel_ax211_command_submit(transaction, &request,
						 now, timeout, handle);
	if (error != INTEL_AX211_COMMAND_OK)
		return error;

	/* Succeeded: the command is pending. */
	return INTEL_AX211_COMMAND_OK;
}

/*
 * Submits the NVM query command.
 */
int
drv_intel_ax211_command_submit_nvm_query(
	struct intel_ax211_command_transaction *transaction,
	uint64_t now,
	uint64_t timeout,
	struct intel_ax211_command_handle *handle)
{
	int error;
	struct intel_ax211_command_request request;
	uint8_t payload[4];
	int result;

	/* Encodes the payload. */
	result = drv_intel_ax211_command_nvm_query_encode(payload);
	if (result != INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_COMMAND_INVALID;

	/* Describes the command and the exact size of its answer. */
	kern_memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_PROTOCOL_FAMILY_REGULATORY_STORE;
	request.command.opcode = INTEL_AX211_PROTOCOL_NVM_QUERY_OPCODE;
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = INTEL_AX211_PROTOCOL_NVM_QUERY_VERSION;
	request.minimum_response_length =
		INTEL_AX211_PROTOCOL_NVM_QUERY_SIZE;
	request.maximum_response_length =
		INTEL_AX211_PROTOCOL_NVM_QUERY_SIZE;

	/* Submits it. */
	error = drv_intel_ax211_command_submit(transaction, &request,
						 now, timeout, handle);
	if (error != INTEL_AX211_COMMAND_OK)
		return error;

	/* Succeeded: the command is pending. */
	return INTEL_AX211_COMMAND_OK;
}

/*
 * Completes the oldest token using a controller-stamped hardware epoch.
 */
int
drv_intel_ax211_command_complete(
	struct intel_ax211_command_transaction *transaction,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	void *response,
	size_t response_capacity,
	size_t *response_length)
{
	int error;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_command_entry *entry;
	struct intel_ax211_ring_token oldest;
	struct intel_ax211_event event;
	uint8_t wire_group;
	int protocol_result;
	int valid;
	int result;
	int transport_result;

	/* Refuses an unusable transaction. */
	valid = ax211_command_transaction_valid(transaction);
	if (!valid)
		return INTEL_AX211_COMMAND_INVALID;

	/* Refuses missing buffers and an unset epoch. */
	if (event_bytes == NULL ||
	    response_length == NULL ||
	    hardware_epoch == 0U) {
		return INTEL_AX211_COMMAND_INVALID;
	}

	/* Refuses an event of an earlier firmware epoch. */
	if (hardware_epoch != transaction->hardware_epoch)
		return INTEL_AX211_COMMAND_STALE;

	/* Decodes the event header. */
	result =
		drv_intel_ax211_event_decode(event_bytes, event_length, &event);
	if (result != INTEL_AX211_SUCCESS)
		return INTEL_AX211_COMMAND_MALFORMED;

	/* An unsolicited notification is not the answer to a command. */
	if ((event.queue & 0x80U) != 0U)
		return INTEL_AX211_COMMAND_OUT_OF_ORDER;

	/* Classifies an answer for a slot that is not pending. */
	entry = &transaction->entry[event.index];
	if (!entry->active) {
		error = ax211_command_inactive_event_result(
			transaction, event.index, hardware_epoch);
		return error;
	}

	/* Refuses an answer from another ring. */
	if (event.queue != entry->pending.queue)
		return INTEL_AX211_COMMAND_OUT_OF_ORDER;

	/* Refuses an answer that is not for the oldest pending command. */
	transport_result = drv_intel_ax211_transport_command_oldest(
		transaction->transport, &oldest);
	if (transport_result != INTEL_AX211_TRANSPORT_OK ||
	    oldest.queue != event.queue || oldest.index != event.index) {
		return INTEL_AX211_COMMAND_OUT_OF_ORDER;
	}

	/* Builds the message the protocol layer validates. */
	kern_memset(&message, 0, sizeof(message));
	message.opcode = event.command.opcode;
	wire_group = event.flags &
		     (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;

	/*
	 * Firmware reports the LONG_GROUP carrier used for an API-89 legacy
	 * command.  Restore its logical group only for the matching pending
	 * token; genuine long-group commands retain their wire identity.
	 */
	if (entry->pending.group == INTEL_AX211_PROTOCOL_FAMILY_CLASSIC &&
	    wire_group == INTEL_AX211_PROTOCOL_FAMILY_CLASSIC_WIDE) {
		message.group = INTEL_AX211_PROTOCOL_FAMILY_CLASSIC;
	} else {
		message.group = wire_group;
	}

	/*
	 * The response layout comes from the pinned command-version table, not from
	 * the wire.
	 */
	message.version = entry->pending.response_version;
	message.flags = event.flags;
	message.queue = event.queue;
	message.index = event.index;
	message.generation = hardware_epoch;
	message.payload = event_bytes + event.payload_offset;
	message.payload_length = event.payload_length;

	/* Validates the answer against what the slot is waiting for. */
	protocol_result = drv_intel_ax211_protocol_command_response_validate(
		&message, &entry->pending);
	result = ax211_command_protocol_result(protocol_result);
	*response_length = 0U;

	/* Refuses a valid answer that does not fit the caller's buffer. */
	if (result == INTEL_AX211_COMMAND_OK &&
	    (message.payload_length > response_capacity ||
	     (message.payload_length != 0U && response == NULL))) {
		return INTEL_AX211_COMMAND_BUFFER_TOO_SMALL;
	}

	/* Retires the slot; a failed retirement leaves the ring untrusted. */
	transport_result = drv_intel_ax211_transport_command_complete(
		transaction->transport, &oldest);
	if (transport_result != INTEL_AX211_TRANSPORT_OK) {
		entry->abandoned = 1U;
		transaction->poisoned = 1U;
		return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
	}

	/* Copies a valid answer to the caller. */
	if (result == INTEL_AX211_COMMAND_OK && message.payload_length != 0U)
		kern_memcpy(response, message.payload, message.payload_length);

	/* Reports the length of a valid answer. */
	if (result == INTEL_AX211_COMMAND_OK)
		*response_length = message.payload_length;

	/* Forgets the slot and remembers what completed in it. */
	ax211_command_entry_clear(transaction, event.index);

	/* Reports a failed answer. */
	if (result != INTEL_AX211_COMMAND_OK)
		return result;

	/* Succeeded: the answer is in the caller's buffer. */
	return INTEL_AX211_COMMAND_OK;
}

/*
 * Abandons one command without retiring or reusing its hardware slot.
 */
int
drv_intel_ax211_command_cancel(
	struct intel_ax211_command_transaction *transaction,
	const struct intel_ax211_command_handle *handle)
{
	int error;
	struct intel_ax211_command_entry *entry;
	int valid;

	/* Refuses an unusable transaction. */
	valid = ax211_command_transaction_valid(transaction);
	if (!valid)
		return INTEL_AX211_COMMAND_INVALID;

	/* Refuses a missing handle or one of an earlier firmware epoch. */
	if (handle == NULL ||
	    handle->hardware_epoch != transaction->hardware_epoch) {
		return INTEL_AX211_COMMAND_INVALID;
	}

	/* Classifies a handle whose slot is not pending. */
	entry = &transaction->entry[handle->token.index];
	if (!entry->active) {
		error = ax211_command_inactive_handle_result(
			transaction, handle);
		return error;
	}

	/* Refuses a handle of an earlier command in the same slot. */
	if (entry->logical_generation != handle->generation ||
	    entry->pending.queue != handle->token.queue) {
		return INTEL_AX211_COMMAND_STALE;
	}

	/* Refuses a second cancel of the same command. */
	if (entry->abandoned)
		return INTEL_AX211_COMMAND_DUPLICATE;

	/*
	 * Abandons the command.  Its slot stays owned by the device, so the ring is no
	 * longer trusted for new commands.
	 */
	entry->abandoned = 1U;
	transaction->poisoned = 1U;

	/* Succeeded: the command is abandoned. */
	return INTEL_AX211_COMMAND_OK;
}

/*
 * Times out the oldest command without making its DMA slot reusable.
 */
int
drv_intel_ax211_command_timeout_oldest(
	struct intel_ax211_command_transaction *transaction,
	uint64_t now,
	struct intel_ax211_command_handle *handle)
{
	struct intel_ax211_command_entry *entry;
	struct intel_ax211_ring_token oldest;
	int valid;
	int result;

	/* Refuses an unusable transaction. */
	valid = ax211_command_transaction_valid(transaction);
	if (!valid)
		return INTEL_AX211_COMMAND_INVALID;

	/* Refuses a missing handle. */
	if (handle == NULL)
		return INTEL_AX211_COMMAND_INVALID;

	/* Nothing is pending. */
	if (transaction->pending_count == 0U)
		return INTEL_AX211_COMMAND_EMPTY;

	/* Finds the oldest command on the ring. */
	result = drv_intel_ax211_transport_command_oldest(
		transaction->transport, &oldest);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_COMMAND_MALFORMED;

	/* Refuses a ring whose oldest slot the transaction does not track. */
	entry = &transaction->entry[oldest.index];
	if (!entry->active)
		return INTEL_AX211_COMMAND_MALFORMED;

	/* Reports which command is the oldest. */
	handle->token = oldest;
	handle->generation = entry->logical_generation;
	handle->hardware_epoch = transaction->hardware_epoch;

	/* An abandoned command already poisoned the ring. */
	if (entry->abandoned)
		return INTEL_AX211_COMMAND_POISONED;

	/* The oldest command still has time. */
	if (now < entry->deadline)
		return INTEL_AX211_COMMAND_PENDING;

	/* Abandons the late command; its slot stays owned by the device. */
	entry->abandoned = 1U;
	transaction->poisoned = 1U;

	/* Reports the timeout. */
	return INTEL_AX211_COMMAND_TIMEOUT;
}

/*
 * Abandons all commands while retaining every hardware-owned slot.
 */
void
drv_intel_ax211_command_cancel_all(
	struct intel_ax211_command_transaction *transaction)
{
	size_t index;
	int found;
	int valid;

	/* Nothing to abandon without a usable transaction. */
	valid = ax211_command_transaction_valid(transaction);
	if (!valid)
		return;

	/* Abandons every pending command. */
	found = 0;
	for (index = 0U; index < INTEL_AX211_COMMAND_ENTRY_COUNT; index++) {
		/* Only a pending slot is abandoned. */
		if (transaction->entry[index].active) {
			transaction->entry[index].abandoned = 1U;
			found = 1;
		}
	}

	/* An abandoned command leaves the ring untrusted. */
	if (found)
		transaction->poisoned = 1U;
}

/*
 * Discards logical metadata only after transport completed reset cleanup.
 */
int
drv_intel_ax211_command_after_device_reset(
	struct intel_ax211_command_transaction *transaction,
	uint32_t hardware_epoch)
{
	size_t pending;
	int valid;

	/* Refuses an unusable transaction. */
	valid = ax211_command_transaction_valid(transaction);
	if (!valid)
		return INTEL_AX211_COMMAND_INVALID;

	/* Refuses an unset epoch or the epoch the transaction already has. */
	if (hardware_epoch == 0U ||
	    hardware_epoch == transaction->hardware_epoch) {
		return INTEL_AX211_COMMAND_INVALID;
	}

	/* Refuses a command ring that the reset has not emptied and cleaned. */
	pending = drv_intel_ax211_transport_command_pending_count(
		transaction->transport);
	if (pending != 0U ||
	    transaction->transport->command_prepared ||
	    transaction->transport->command_reset_required ||
	    !transaction->transport->command_reset_completed) {
		return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
	}

	/* Forgets every slot and starts the new epoch with a trusted ring. */
	drv_intel_ax211_scrub(transaction->entry, sizeof(transaction->entry));
	drv_intel_ax211_scrub(transaction->last_generation,
			      sizeof(transaction->last_generation));
	drv_intel_ax211_scrub(transaction->last_completion_epoch,
			      sizeof(transaction->last_completion_epoch));
	transaction->pending_count = 0U;
	transaction->hardware_epoch = hardware_epoch;
	transaction->poisoned = 0U;
	transaction->transport->command_reset_completed = 0U;

	/* Keeps handle generations nonzero. */
	if (transaction->next_generation == 0U)
		transaction->next_generation = 1U;

	/* Succeeded: the transaction accepts commands of the new epoch. */
	return INTEL_AX211_COMMAND_OK;
}

/*
 * Reports how many commands are pending.
 */
size_t
drv_intel_ax211_command_pending_count(
	const struct intel_ax211_command_transaction *transaction)
{
	int valid;

	/* An unusable transaction has nothing pending. */
	valid = ax211_command_transaction_valid(transaction);
	if (!valid)
		return 0U;

	/* Reports the count. */
	return transaction->pending_count;
}

/*
 * Reports whether an abandoned command left the ring untrusted.
 */
int
drv_intel_ax211_command_is_poisoned(
	const struct intel_ax211_command_transaction *transaction)
{
	int valid;

	/* An unusable transaction is not reported as poisoned. */
	valid = ax211_command_transaction_valid(transaction);
	if (!valid)
		return 0;

	/* Reports the poisoned ring. */
	if (transaction->poisoned != 0U)
		return 1;

	/* Reports a trusted ring. */
	return 0;
}

/*
 * Reports whether a transaction is bound and consistent: 1 when it is, 0 when
 * not.
 */
static int
ax211_command_transaction_valid(
	const struct intel_ax211_command_transaction *transaction)
{
	/* Rejects an unbound transaction. */
	if (transaction == NULL ||
	    !transaction->initialized ||
	    transaction->transport == NULL ||
	    transaction->hardware_epoch == 0U) {
		return 0;
	}

	/* Rejects a pending limit or count out of range. */
	if (transaction->max_pending == 0U ||
	    transaction->max_pending > INTEL_AX211_COMMAND_MAX_PENDING ||
	    transaction->pending_count > transaction->max_pending) {
		return 0;
	}

	/* The transaction is usable. */
	return 1;
}

/*
 * Reports whether a request fits the ring and the protocol: 1 when it does, 0
 * when not.
 */
static int
ax211_command_request_valid(
	const struct intel_ax211_command_request *request)
{
	/*
	 * Rejects an unknown version, an oversized payload or inconsistent
	 * answer bounds.
	 */
	if (request == NULL ||
	    request->command.version == INTEL_AX211_PROTOCOL_UNKNOWN_VERSION ||
	    request->payload_length >
		    INTEL_AX211_COMMAND_EXTERNAL_PAYLOAD_SIZE ||
	    (request->payload_length != 0U && request->payload == NULL) ||
	    request->response_version == INTEL_AX211_PROTOCOL_UNKNOWN_VERSION ||
	    request->minimum_response_length >
		    request->maximum_response_length ||
	    request->maximum_response_length > INTEL_AX211_MAX_COMMAND_PAYLOAD) {
		return 0;
	}

	/* The request is usable. */
	return 1;
}

/* Maps a transport failure before the doorbell to the command result. */
static int
ax211_command_prepare_result(
	int result)
{
	/* The ring has no free slot. */
	if (result == INTEL_AX211_TRANSPORT_FULL)
		return INTEL_AX211_COMMAND_FULL;

	/* The transport refused the request. */
	if (result == INTEL_AX211_TRANSPORT_INVALID)
		return INTEL_AX211_COMMAND_INVALID;

	/* The command could not be written into its slot. */
	if (result == INTEL_AX211_TRANSPORT_IO)
		return INTEL_AX211_COMMAND_ENCODE_FAILED;

	/* Any other failure is a transport failure. */
	return INTEL_AX211_COMMAND_TRANSPORT_FAILED;
}

/* Produces a nonzero host-only handle generation. */
static uint32_t
ax211_command_next_generation(
	struct intel_ax211_command_transaction *transaction)
{
	uint32_t generation;

	/*
	 * Hands out the next generation and skips zero when the counter wraps.
	 */
	generation = transaction->next_generation;
	transaction->next_generation++;
	if (transaction->next_generation == 0U)
		transaction->next_generation = 1U;

	/* Reports the generation handed out. */
	return generation;
}

/* Classifies an inactive event using its controller-supplied hardware epoch. */
static int
ax211_command_inactive_event_result(
	const struct intel_ax211_command_transaction *transaction,
	uint8_t index,
	uint32_t hardware_epoch)
{
	/* An event for a slot completed in this epoch is a repeat. */
	if (transaction->last_completion_epoch[index] == hardware_epoch &&
	    hardware_epoch != 0U)
		return INTEL_AX211_COMMAND_DUPLICATE;

	/* Any other event for an idle slot belongs to an earlier epoch. */
	return INTEL_AX211_COMMAND_STALE;
}

/* Maps exact protocol validation outcomes into the command contract. */
static int
ax211_command_protocol_result(
	int result)
{
	/* Maps each protocol result to the command result. */
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

/* Retires logical metadata only after transport retired the same slot. */
static void
ax211_command_entry_clear(
	struct intel_ax211_command_transaction *transaction,
	uint8_t index)
{
	uint32_t generation;

	/* Erases the slot but remembers what completed in it. */
	generation = transaction->entry[index].logical_generation;
	drv_intel_ax211_scrub(&transaction->entry[index],
			      sizeof(transaction->entry[index]));
	transaction->last_generation[index] = generation;
	transaction->last_completion_epoch[index] = transaction->hardware_epoch;
	transaction->pending_count--;
}

/* Classifies an inactive handle using its host-only logical generation. */
static int
ax211_command_inactive_handle_result(
	const struct intel_ax211_command_transaction *transaction,
	const struct intel_ax211_command_handle *handle)
{
	/*
	 * A handle of the command that completed last in the slot is a repeat.
	 */
	if (transaction->last_generation[handle->token.index] ==
		    handle->generation &&
	    handle->generation != 0U)
		return INTEL_AX211_COMMAND_DUPLICATE;

	/* Any other handle names an earlier command. */
	return INTEL_AX211_COMMAND_STALE;
}
