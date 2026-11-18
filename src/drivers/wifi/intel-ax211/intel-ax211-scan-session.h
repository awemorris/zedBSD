/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 scan-session coordinator.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_SCAN_SESSION_H
#define KERN_DRIVERS_INTEL_AX211_SCAN_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-command.h"
#include "intel-ax211-scan.h"

/* zedBSD's bounds: one second for the request's and for the cancel's answer. */
#define INTEL_AX211_SCAN_SESSION_COMMAND_TIMEOUT_US 1000000U
#define INTEL_AX211_SCAN_SESSION_ABORT_TIMEOUT_US 1000000U

/* The results of the scan session. */
enum intel_ax211_scan_session_result {
	INTEL_AX211_SCAN_SESSION_OK = 0,
	INTEL_AX211_SCAN_SESSION_INVALID = 1,
	INTEL_AX211_SCAN_SESSION_UNSUPPORTED = 2,
	INTEL_AX211_SCAN_SESSION_BUSY = 3,
	INTEL_AX211_SCAN_SESSION_COMMAND = 4,
	INTEL_AX211_SCAN_SESSION_STALE = 5,
	INTEL_AX211_SCAN_SESSION_DUPLICATE = 6,
	INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER = 7,
	INTEL_AX211_SCAN_SESSION_TIMEOUT = 8,
	INTEL_AX211_SCAN_SESSION_COMPLETE = 9,
	INTEL_AX211_SCAN_SESSION_ABORTED = 10,
	INTEL_AX211_SCAN_SESSION_FAILED = 11
};

/* What the session waits for. */
enum intel_ax211_scan_session_phase {
	INTEL_AX211_SCAN_SESSION_IDLE = 0,
	INTEL_AX211_SCAN_SESSION_WAIT_START_ACK = 1,
	INTEL_AX211_SCAN_SESSION_RUNNING = 2,
	INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK = 3,
	INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE = 4,
	INTEL_AX211_SCAN_SESSION_TERMINAL = 5
};

/* One scan notification handed to the caller, with the scan it belongs to. */
struct intel_ax211_scan_session_event {
	uint64_t common_generation;
	uint8_t channel;
	struct intel_ax211_scan_event firmware;
};

/*
 * The scans of one firmware epoch, one channel at a time, over the command
 * transaction of that epoch.
 *
 * The common-WLAN generation is never placed on the firmware wire.  Firmware
 * notifications are fenced by hardware_epoch, which belongs to the running
 * operational image.  The API89 table and full NVM/MCC profile are owned by
 * this object, so no firmware-source lifetime leaks into scan operations.
 */
struct intel_ax211_scan_session {
	struct intel_ax211_command_transaction *commands;
	struct intel_ax211_protocol_command_table command_table;
	struct intel_ax211_scan_profile full_profile;
	struct intel_ax211_scan_profile channel_profile;
	struct intel_ax211_scan_state scan;
	struct intel_ax211_command_handle command_handle;
	uint8_t command_version_bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t request[INTEL_AX211_SCAN_REQUEST_SIZE];
	uint64_t common_generation;
	uint64_t command_deadline;
	uint32_t hardware_epoch;
	uint8_t channel;
	uint8_t initialized;
	uint8_t phase;
	uint8_t terminal_result;
};

int
drv_intel_ax211_scan_session_init(
	struct intel_ax211_scan_session *session,
	struct intel_ax211_command_transaction *commands,
	const struct intel_ax211_protocol_command_table *command_table,
	const struct intel_ax211_protocol_nvm *nvm,
	const struct intel_ax211_runtime_mcc *mcc,
	const uint8_t station_address[6],
	uint32_t hardware_epoch);

int
drv_intel_ax211_scan_session_begin_channel(
	struct intel_ax211_scan_session *session,
	uint64_t common_generation,
	uint8_t channel,
	uint64_t now_us);

int
drv_intel_ax211_scan_session_start_ack(
	struct intel_ax211_scan_session *session,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	uint64_t now_us);

int
drv_intel_ax211_scan_session_notification(
	struct intel_ax211_scan_session *session,
	const struct intel_ax211_protocol_message *message,
	uint64_t now_us,
	struct intel_ax211_scan_session_event *event);

int
drv_intel_ax211_scan_session_abort(
	struct intel_ax211_scan_session *session,
	uint64_t common_generation,
	uint64_t now_us);

int
drv_intel_ax211_scan_session_abort_ack(
	struct intel_ax211_scan_session *session,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	uint64_t now_us);

int
drv_intel_ax211_scan_session_expire(
	struct intel_ax211_scan_session *session,
	uint64_t now_us);

#endif
