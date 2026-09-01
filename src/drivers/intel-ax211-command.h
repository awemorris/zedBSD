/*
 * zedBSD Intel AX211 private firmware command transactions
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_COMMAND_H
#define ZEDBSD_DRIVERS_INTEL_AX211_COMMAND_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"
#include "intel-ax211-transport.h"

#define INTEL_AX211_COMMAND_ENTRY_COUNT                 256U
#define INTEL_AX211_COMMAND_MAX_PENDING                 255U
#define INTEL_AX211_COMMAND_INLINE_PAYLOAD_SIZE         316U
#define INTEL_AX211_COMMAND_EXTERNAL_PAYLOAD_SIZE      4088U

enum intel_ax211_command_result {
	INTEL_AX211_COMMAND_OK = 0,
	INTEL_AX211_COMMAND_INVALID = 1,
	INTEL_AX211_COMMAND_FULL = 2,
	INTEL_AX211_COMMAND_ENCODE_FAILED = 3,
	INTEL_AX211_COMMAND_DOORBELL_FAILED = 4,
	INTEL_AX211_COMMAND_MALFORMED = 5,
	INTEL_AX211_COMMAND_FIRMWARE_FAILED = 6,
	INTEL_AX211_COMMAND_VERSION_MISMATCH = 7,
	INTEL_AX211_COMMAND_STALE = 8,
	INTEL_AX211_COMMAND_DUPLICATE = 9,
	INTEL_AX211_COMMAND_OUT_OF_ORDER = 10,
	INTEL_AX211_COMMAND_BUFFER_TOO_SMALL = 11,
	INTEL_AX211_COMMAND_PENDING = 12,
	INTEL_AX211_COMMAND_TIMEOUT = 13,
	INTEL_AX211_COMMAND_EMPTY = 14,
	INTEL_AX211_COMMAND_POISONED = 15,
	INTEL_AX211_COMMAND_TRANSPORT_FAILED = 16
};

struct intel_ax211_command_handle {
	struct intel_ax211_ring_token token;
	uint32_t generation;
	uint32_t hardware_epoch;
};

struct intel_ax211_command_request {
	struct intel_ax211_command_id command;
	const void *payload;
	size_t payload_length;
	uint8_t response_version;
	size_t minimum_response_length;
	size_t maximum_response_length;
};

struct intel_ax211_command_entry {
	uint8_t active;
	uint8_t abandoned;
	uint32_t logical_generation;
	uint64_t deadline;
	struct intel_ax211_protocol_pending_command pending;
};

struct intel_ax211_command_transaction {
	struct intel_ax211_transport *transport;
	size_t max_pending;
	size_t pending_count;
	uint32_t next_generation;
	uint32_t hardware_epoch;
	uint8_t initialized;
	uint8_t poisoned;
	struct intel_ax211_command_entry entry[INTEL_AX211_COMMAND_ENTRY_COUNT];
	uint32_t last_generation[INTEL_AX211_COMMAND_ENTRY_COUNT];
	uint32_t last_completion_epoch[INTEL_AX211_COMMAND_ENTRY_COUNT];
};

int intel_ax211_command_transaction_init(
	struct intel_ax211_command_transaction *transaction,
	struct intel_ax211_transport *transport, size_t max_pending,
	uint32_t hardware_epoch);

int intel_ax211_command_nvm_access_complete_encode(uint8_t output[4]);
int intel_ax211_command_nvm_get_info_encode(uint8_t output[4]);

int intel_ax211_command_submit(
	struct intel_ax211_command_transaction *transaction,
	const struct intel_ax211_command_request *request, uint64_t now,
	uint64_t timeout, struct intel_ax211_command_handle *handle);
int intel_ax211_command_submit_nvm_access_complete(
	struct intel_ax211_command_transaction *transaction, uint64_t now,
	uint64_t timeout, struct intel_ax211_command_handle *handle);
int intel_ax211_command_submit_nvm_get_info(
	struct intel_ax211_command_transaction *transaction, uint64_t now,
	uint64_t timeout, struct intel_ax211_command_handle *handle);

int intel_ax211_command_complete(
	struct intel_ax211_command_transaction *transaction,
	const uint8_t *event_bytes, size_t event_length,
	uint32_t hardware_epoch,
	void *response, size_t response_capacity, size_t *response_length);
int intel_ax211_command_cancel(
	struct intel_ax211_command_transaction *transaction,
	const struct intel_ax211_command_handle *handle);
int intel_ax211_command_timeout_oldest(
	struct intel_ax211_command_transaction *transaction, uint64_t now,
	struct intel_ax211_command_handle *handle);
void intel_ax211_command_cancel_all(
	struct intel_ax211_command_transaction *transaction);
int intel_ax211_command_after_device_reset(
	struct intel_ax211_command_transaction *transaction,
	uint32_t hardware_epoch);

size_t intel_ax211_command_pending_count(
	const struct intel_ax211_command_transaction *transaction);
int intel_ax211_command_is_poisoned(
	const struct intel_ax211_command_transaction *transaction);

#endif
