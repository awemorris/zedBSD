/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 scan-session coordinator.
 */

#include "intel-ax211-scan-session.h"
#include <kern/kcrt.h>

/*
 * The answer to a scan cancel.
 *
 * The answer is one 32-bit status word: 0 means the scan was stopped, 1 that
 * the stop is under way and a scan completion will follow, 2 that no scan was
 * running.  The session treats status 2 as the end of the scan at once; after
 * 0 or 1 it still waits for the completion notification, which then reports
 * the cancelled outcome.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/scan.h (enum
 * iwl_umac_scan_abort_status).
 */
#define AX211_SCAN_CANCEL_RESPONSE_SIZE 4U
#define AX211_SCAN_CANCEL_DONE 0U
#define AX211_SCAN_CANCEL_PENDING 1U
#define AX211_SCAN_CANCEL_NO_SCAN 2U

static int ax211_scan_session_scan_result(int result);
static int ax211_scan_session_live(const struct intel_ax211_scan_session *session);
static int ax211_scan_session_channel_profile(struct intel_ax211_scan_session *session, uint8_t channel);
static int ax211_scan_session_submit(struct intel_ax211_scan_session *session, uint8_t opcode, const void *payload, size_t payload_length, uint64_t now_us, uint64_t timeout_us, uint8_t phase);
static int ax211_scan_session_command_result(int result);
static int ax211_scan_session_ack(struct intel_ax211_scan_session *session, uint8_t required_phase, const uint8_t *event_bytes, size_t event_length, uint32_t hardware_epoch, uint64_t now_us, uint32_t *abort_status);
static int ax211_scan_session_terminal(struct intel_ax211_scan_session *session, int result);

/*
 * Binds a scan session to the command transaction of one firmware epoch and
 * builds its full scan profile.
 */
int
drv_intel_ax211_scan_session_init(
	struct intel_ax211_scan_session *session,
	struct intel_ax211_command_transaction *commands,
	const struct intel_ax211_protocol_command_table *command_table,
	const struct intel_ax211_protocol_nvm *nvm,
	const struct intel_ax211_runtime_mcc *mcc,
	const uint8_t station_address[6],
	uint32_t hardware_epoch)
{
	struct intel_ax211_protocol_command_table copied;
	size_t pending;
	int poisoned;
	int result;

	/*
	 * Refuses missing arguments, a table of another length and a
	 * transaction of another epoch.
	 */
	if (session == NULL ||
	    commands == NULL ||
	    command_table == NULL ||
	    command_table->bytes == NULL ||
	    nvm == NULL ||
	    station_address == NULL ||
	    hardware_epoch == 0U ||
	    command_table->count != INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT ||
	    !commands->initialized ||
	    commands->hardware_epoch != hardware_epoch) {
		return INTEL_AX211_SCAN_SESSION_INVALID;
	}

	/* Refuses a transaction that still has commands pending. */
	pending = drv_intel_ax211_command_pending_count(commands);
	if (pending != 0U)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* Refuses a transaction whose ring is no longer trusted. */
	poisoned = drv_intel_ax211_command_is_poisoned(commands);
	if (poisoned)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* Keeps its own copy of the command versions the firmware offers. */
	kern_memset(session, 0, sizeof(*session));
	kern_memcpy(session->command_version_bytes, command_table->bytes,
	       sizeof(session->command_version_bytes));

	/* Refuses a copy that does not parse as a command table. */
	result = drv_intel_ax211_protocol_command_table_parse(
		session->command_version_bytes,
		sizeof(session->command_version_bytes), &copied);
	if (result != INTEL_AX211_PROTOCOL_OK) {
		kern_memset(session, 0, sizeof(*session));
		return INTEL_AX211_SCAN_SESSION_UNSUPPORTED;
	}

	/* Refuses an image that does not list the scan commands as expected. */
	result = drv_intel_ax211_scan_api89_validate(&copied);
	if (result != INTEL_AX211_SCAN_OK) {
		kern_memset(session, 0, sizeof(*session));
		return INTEL_AX211_SCAN_SESSION_UNSUPPORTED;
	}

	/* Builds the full scan profile from the NVM and country answers. */
	result = drv_intel_ax211_scan_profile_from_nvm(
		nvm, mcc, station_address, &session->full_profile);
	if (result != INTEL_AX211_SCAN_OK) {
		kern_memset(session, 0, sizeof(*session));
		result = ax211_scan_session_scan_result(result);
		return result;
	}

	/* Binds the session; no scan runs yet. */
	session->commands = commands;
	session->command_table.bytes = session->command_version_bytes;
	session->command_table.count = copied.count;
	session->hardware_epoch = hardware_epoch;
	session->phase = INTEL_AX211_SCAN_SESSION_IDLE;
	session->initialized = 1U;

	/* Succeeded: the session accepts scans. */
	return INTEL_AX211_SCAN_SESSION_OK;
}

/*
 * Starts the scan of one channel: encodes its request and submits it.
 */
int
drv_intel_ax211_scan_session_begin_channel(
	struct intel_ax211_scan_session *session,
	uint64_t common_generation,
	uint8_t channel,
	uint64_t now_us)
{
	size_t pending;
	int live;
	int poisoned;
	int result;

	/* Refuses a session that is not bound to the live transaction. */
	live = ax211_scan_session_live(session);
	if (!live)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* Refuses an unset common generation. */
	if (common_generation == 0U)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* Refuses a new scan while one is running. */
	if (session->phase != INTEL_AX211_SCAN_SESSION_IDLE &&
	    session->phase != INTEL_AX211_SCAN_SESSION_TERMINAL) {
		return INTEL_AX211_SCAN_SESSION_BUSY;
	}

	/*
	 * Refuses a new scan after a timeout, which needs a device reset first.
	 */
	if (session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL &&
	    session->terminal_result == INTEL_AX211_SCAN_SESSION_TIMEOUT) {
		return INTEL_AX211_SCAN_SESSION_BUSY;
	}

	/* Refuses a transaction that still has commands pending. */
	pending = drv_intel_ax211_command_pending_count(session->commands);
	if (pending != 0U)
		return INTEL_AX211_SCAN_SESSION_COMMAND;

	/* Refuses a transaction whose ring is no longer trusted. */
	poisoned = drv_intel_ax211_command_is_poisoned(session->commands);
	if (poisoned)
		return INTEL_AX211_SCAN_SESSION_COMMAND;

	/* Builds the one-channel profile. */
	result = ax211_scan_session_channel_profile(session, channel);
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return result;

	/* Encodes the request. */
	result = drv_intel_ax211_scan_request_encode(&session->channel_profile,
						     session->request);
	if (result != INTEL_AX211_SCAN_OK) {
		result = ax211_scan_session_scan_result(result);
		return result;
	}

	/* Starts the scan bookkeeping. */
	result = drv_intel_ax211_scan_begin(
		&session->scan, &session->command_table,
		&session->channel_profile, session->hardware_epoch, now_us);
	if (result != INTEL_AX211_SCAN_OK) {
		result = ax211_scan_session_scan_result(result);
		return result;
	}

	/* Starts one channel's scan and waits for the acknowledgement. */
	session->common_generation = common_generation;
	session->channel = channel;
	session->terminal_result = INTEL_AX211_SCAN_SESSION_OK;

	/* Submits the request; a failed submission ends the scan. */
	result = ax211_scan_session_submit(
		session, INTEL_AX211_SCAN_REQUEST_OPCODE, session->request,
		sizeof(session->request), now_us,
		INTEL_AX211_SCAN_SESSION_COMMAND_TIMEOUT_US,
		INTEL_AX211_SCAN_SESSION_WAIT_START_ACK);
	if (result != INTEL_AX211_SCAN_SESSION_OK) {
		kern_memset(&session->scan, 0, sizeof(session->scan));
		session->phase = INTEL_AX211_SCAN_SESSION_TERMINAL;
		session->terminal_result = (uint8_t)result;
		return result;
	}

	/* Succeeded: the request waits for its acknowledgment. */
	return INTEL_AX211_SCAN_SESSION_OK;
}

/*
 * Takes the acknowledgment of the scan request and lets the scan run.
 */
int
drv_intel_ax211_scan_session_start_ack(
	struct intel_ax211_scan_session *session,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	uint64_t now_us)
{
	int result;

	/* Completes the request command with its acknowledgment. */
	result = ax211_scan_session_ack(
		session, INTEL_AX211_SCAN_SESSION_WAIT_START_ACK, event_bytes,
		event_length, hardware_epoch, now_us, NULL);
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return result;

	/*
	 * Moves the scan bookkeeping to running; a refusal ends the session's
	 * scan.
	 */
	result = drv_intel_ax211_scan_request_ack(
		&session->scan, session->hardware_epoch, now_us);
	if (result != INTEL_AX211_SCAN_OK) {
		result = ax211_scan_session_scan_result(result);
		result = ax211_scan_session_terminal(session, result);
		return result;
	}

	/* The scan runs. */
	session->phase = INTEL_AX211_SCAN_SESSION_RUNNING;

	/* Succeeded. */
	return INTEL_AX211_SCAN_SESSION_OK;
}

/*
 * Takes one scan notification and reports it to the caller.
 */
int
drv_intel_ax211_scan_session_notification(
	struct intel_ax211_scan_session *session,
	const struct intel_ax211_protocol_message *message,
	uint64_t now_us,
	struct intel_ax211_scan_session_event *event)
{
	struct intel_ax211_scan_event decoded;
	int live;
	int result;

	/* Refuses a session that is not bound to the live transaction. */
	live = ax211_scan_session_live(session);
	if (!live)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* Refuses missing arguments. */
	if (message == NULL || event == NULL)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* A notification after the scan ended is a repeat. */
	if (session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL)
		return INTEL_AX211_SCAN_SESSION_DUPLICATE;

	/* Refuses a notification while no scan runs or is being cancelled. */
	if (session->phase != INTEL_AX211_SCAN_SESSION_RUNNING &&
	    session->phase != INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE) {
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	}

	/* Ends the session when the cancelled scan's completion is late. */
	if (session->phase == INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE &&
	    now_us >= session->command_deadline) {
		result = ax211_scan_session_terminal(
			session, INTEL_AX211_SCAN_SESSION_TIMEOUT);
		return result;
	}

	/* Decodes the notification. */
	kern_memset(&decoded, 0, sizeof(decoded));
	result = drv_intel_ax211_scan_event_accept(&session->scan, message,
						   now_us, &decoded);
	result = ax211_scan_session_scan_result(result);

	/*
	 * A completion, a cancel, a failure or a timeout ends the session's
	 * scan.
	 */
	if (result == INTEL_AX211_SCAN_SESSION_COMPLETE ||
	    result == INTEL_AX211_SCAN_SESSION_ABORTED ||
	    result == INTEL_AX211_SCAN_SESSION_FAILED ||
	    result == INTEL_AX211_SCAN_SESSION_TIMEOUT) {
		/* Only a timeout still needs a cancel. */
		if (result != INTEL_AX211_SCAN_SESSION_TIMEOUT)
			session->scan.abort_required = 0U;
		ax211_scan_session_terminal(session, result);
	}

	/* Hands a decoded notification to the caller. */
	if (result == INTEL_AX211_SCAN_SESSION_COMPLETE ||
	    result == INTEL_AX211_SCAN_SESSION_ABORTED ||
	    result == INTEL_AX211_SCAN_SESSION_FAILED ||
	    result == INTEL_AX211_SCAN_SESSION_OK) {
		kern_memset(event, 0, sizeof(*event));
		event->common_generation = session->common_generation;
		event->channel = session->channel;
		event->firmware = decoded;
	}

	/* Reports what the notification was. */
	return result;
}

/*
 * Cancels the running scan, or a scan whose watchdog ended it.
 */
int
drv_intel_ax211_scan_session_abort(
	struct intel_ax211_scan_session *session,
	uint64_t common_generation,
	uint64_t now_us)
{
	uint8_t payload[INTEL_AX211_SCAN_CANCEL_SIZE];
	int live;
	int result;

	/* Refuses a session that is not bound to the live transaction. */
	live = ax211_scan_session_live(session);
	if (!live)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* Refuses an unset common generation. */
	if (common_generation == 0U)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* Refuses a cancel of another scan. */
	if (common_generation != session->common_generation)
		return INTEL_AX211_SCAN_SESSION_STALE;

	/* A cancel of an ended scan that needs none is a repeat. */
	if (session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL &&
	    !session->scan.abort_required) {
		return INTEL_AX211_SCAN_SESSION_DUPLICATE;
	}

	/*
	 * Refuses a cancel unless the scan runs or its watchdog asked for one.
	 */
	if (session->phase != INTEL_AX211_SCAN_SESSION_RUNNING &&
	    !(session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL &&
	      session->scan.abort_required)) {
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	}

	/* Encodes the cancel command. */
	result = drv_intel_ax211_scan_abort_encode(payload);
	if (result != INTEL_AX211_SCAN_OK)
		return INTEL_AX211_SCAN_SESSION_FAILED;

	/* Submits it; a failed submission ends the session's scan. */
	result = ax211_scan_session_submit(
		session, INTEL_AX211_SCAN_CANCEL_OPCODE, payload,
		sizeof(payload), now_us,
		INTEL_AX211_SCAN_SESSION_ABORT_TIMEOUT_US,
		INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK);
	if (result != INTEL_AX211_SCAN_SESSION_OK) {
		result = ax211_scan_session_terminal(session, result);
		return result;
	}

	/* Succeeded: the cancel waits for its answer. */
	return INTEL_AX211_SCAN_SESSION_OK;
}

/*
 * Takes the answer to the scan cancel.
 */
int
drv_intel_ax211_scan_session_abort_ack(
	struct intel_ax211_scan_session *session,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	uint64_t now_us)
{
	uint32_t abort_status;
	int result;

	/* Completes the cancel command and takes its status word. */
	result = ax211_scan_session_ack(
		session, INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK, event_bytes,
		event_length, hardware_epoch, now_us, &abort_status);
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return result;

	/* Ends the session's scan on an unknown status. */
	if (abort_status != AX211_SCAN_CANCEL_DONE &&
	    abort_status != AX211_SCAN_CANCEL_PENDING &&
	    abort_status != AX211_SCAN_CANCEL_NO_SCAN) {
		result = ax211_scan_session_terminal(
			session, INTEL_AX211_SCAN_SESSION_COMMAND);
		return result;
	}

	/* No scan was running: the scan ends as cancelled at once. */
	if (abort_status == AX211_SCAN_CANCEL_NO_SCAN) {
		session->scan.abort_required = 0U;
		session->scan.phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		result = ax211_scan_session_terminal(
			session, INTEL_AX211_SCAN_SESSION_ABORTED);
		return result;
	}

	/* SUCCESS and IN_PROGRESS both complete through SCAN_COMPLETE_UMAC. */
	session->scan.phase = INTEL_AX211_SCAN_PHASE_RUNNING;
	session->scan.scan_deadline = session->command_deadline;
	session->phase = INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE;

	/* Succeeded: the cancelled scan's completion is awaited. */
	return INTEL_AX211_SCAN_SESSION_OK;
}

/*
 * Ends the session's scan when the deadline of what it waits for has passed.
 */
int
drv_intel_ax211_scan_session_expire(
	struct intel_ax211_scan_session *session,
	uint64_t now_us)
{
	struct intel_ax211_command_handle expired;
	int live;
	int result;

	/* Refuses a session that is not bound to the live transaction. */
	live = ax211_scan_session_live(session);
	if (!live)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* An ended scan cannot expire again. */
	if (session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL)
		return INTEL_AX211_SCAN_SESSION_DUPLICATE;

	/*
	 * A command waiting for its answer expires through the command
	 * transaction.
	 */
	if (session->phase == INTEL_AX211_SCAN_SESSION_WAIT_START_ACK ||
	    session->phase == INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK) {
		/* The command still has time. */
		if (now_us < session->command_deadline)
			return INTEL_AX211_SCAN_SESSION_OK;

		/* Times out the oldest command. */
		kern_memset(&expired, 0, sizeof(expired));
		result = drv_intel_ax211_command_timeout_oldest(
			session->commands, now_us, &expired);
		if (result == INTEL_AX211_COMMAND_PENDING)
			return INTEL_AX211_SCAN_SESSION_OK;

		/*
		 * Ends the session's scan when the transaction failed
		 * otherwise.
		 */
		if (result != INTEL_AX211_COMMAND_TIMEOUT) {
			result = ax211_scan_session_command_result(result);
			result = ax211_scan_session_terminal(session, result);
			return result;
		}

		/* Ends the session's scan when another command timed out. */
		if (expired.token.queue !=
			    session->command_handle.token.queue ||
		    expired.token.index !=
			    session->command_handle.token.index ||
		    expired.generation != session->command_handle.generation) {
			result = ax211_scan_session_terminal(
				session, INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER);
			return result;
		}

		/* Ends the session's scan as timed out. */
		result = ax211_scan_session_terminal(
			session, INTEL_AX211_SCAN_SESSION_TIMEOUT);
		return result;
	}

	/* A cancelled scan expires when its completion is late. */
	if (session->phase == INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE) {
		/* The completion still has time. */
		if (now_us < session->command_deadline)
			return INTEL_AX211_SCAN_SESSION_OK;

		/* Ends the scan; it still needs a cancel. */
		session->scan.phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		session->scan.abort_required = 1U;
		result = ax211_scan_session_terminal(
			session, INTEL_AX211_SCAN_SESSION_TIMEOUT);
		return result;
	}

	/* No other phase but a running scan has a deadline. */
	if (session->phase != INTEL_AX211_SCAN_SESSION_RUNNING)
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;

	/* Lets the scan bookkeeping check its watchdog. */
	result = drv_intel_ax211_scan_expire(&session->scan, now_us);
	result = ax211_scan_session_scan_result(result);

	/* Ends the session's scan when the watchdog ran out. */
	if (result == INTEL_AX211_SCAN_SESSION_TIMEOUT) {
		result = ax211_scan_session_terminal(session, result);
		return result;
	}

	/* Reports what the watchdog found. */
	return result;
}

/* Maps a result of the scan codecs to the session's result. */
static int
ax211_scan_session_scan_result(
	int result)
{
	/*
	 * Each scan result has its session counterpart; the rest are failures.
	 */
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
	case INTEL_AX211_SCAN_TIMED_OUT:
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

/*
 * Reports whether a session is bound to the live command transaction: 1 when
 * it is, 0 when not.
 */
static int
ax211_scan_session_live(
	const struct intel_ax211_scan_session *session)
{
	/* Rejects a missing or unbound session. */
	if (session == NULL ||
	    !session->initialized ||
	    session->commands == NULL ||
	    session->hardware_epoch == 0U) {
		return 0;
	}

	/* Rejects a transaction that is no longer initialized. */
	if (!session->commands->initialized)
		return 0;

	/* Rejects a transaction of another firmware epoch. */
	if (session->commands->hardware_epoch != session->hardware_epoch)
		return 0;

	/* The session is live. */
	return 1;
}

/* Builds the profile that scans one channel of the full profile. */
static int
ax211_scan_session_channel_profile(
	struct intel_ax211_scan_session *session,
	uint8_t channel)
{
	size_t index;

	/* Looks for the channel in the full profile. */
	for (index = 0U; index < session->full_profile.channel_count; index++) {
		/* Builds the one-channel profile from the matching entry. */
		if (session->full_profile.channel[index] == channel) {
			kern_memset(&session->channel_profile, 0,
			       sizeof(session->channel_profile));
			kern_memcpy(session->channel_profile.station_address,
			       session->full_profile.station_address, 6U);
			session->channel_profile.channel_width_mhz =
				session->full_profile.channel_width_mhz;
			session->channel_profile.channel[0] = channel;
			session->channel_profile.channel_count = 1U;
			return INTEL_AX211_SCAN_SESSION_OK;
		}
	}

	/* The full profile does not allow the channel. */
	return INTEL_AX211_SCAN_SESSION_UNSUPPORTED;
}

/*
 * Submits one scan command and moves the session to the phase that waits for
 * it.
 */
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

	/* Refuses a deadline that overflows. */
	if (now_us > UINT64_MAX - timeout_us)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* API89 layouts are 17/1; the Gen3 wide-header wire version is zero. */
	kern_memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_SCAN_FAMILY_CLASSIC_WIDE;
	request.command.opcode = opcode;
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = payload_length;
	request.response_version = 0U;

	/* The cancel is answered by one status word; the request by nothing. */
	if (opcode == INTEL_AX211_SCAN_CANCEL_OPCODE) {
		request.minimum_response_length =
			AX211_SCAN_CANCEL_RESPONSE_SIZE;
		request.maximum_response_length =
			AX211_SCAN_CANCEL_RESPONSE_SIZE;
	} else {
		request.minimum_response_length = 0U;
		request.maximum_response_length = 0U;
	}

	/* Submits the command. */
	result = drv_intel_ax211_command_submit(session->commands, &request,
						now_us, timeout_us,
						&session->command_handle);
	result = ax211_scan_session_command_result(result);
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return result;

	/* Waits for the answer until the deadline. */
	session->command_deadline = now_us + timeout_us;
	session->phase = phase;

	/* Succeeded. */
	return INTEL_AX211_SCAN_SESSION_OK;
}

/* Maps a result of the command transaction to the session's result. */
static int
ax211_scan_session_command_result(
	int result)
{
	/*
	 * Each command result has its session counterpart; the rest are command
	 * failures.
	 */
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

/*
 * Completes the scan command the session waits for with the answer event.
 *
 * A cancel's status word is reported through abort_status, which must be given
 * exactly when the session waits for the cancel's answer.
 */
static int
ax211_scan_session_ack(
	struct intel_ax211_scan_session *session,
	uint8_t required_phase,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	uint64_t now_us,
	uint32_t *abort_status)
{
	struct intel_ax211_event event;
	uint8_t response[AX211_SCAN_CANCEL_RESPONSE_SIZE];
	void *response_bytes;
	size_t response_capacity;
	size_t response_length;
	int live;
	int result;

	/* Refuses a session that is not bound to the live transaction. */
	live = ax211_scan_session_live(session);
	if (!live)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* Refuses a missing event and an unset epoch. */
	if (event_bytes == NULL || hardware_epoch == 0U)
		return INTEL_AX211_SCAN_SESSION_INVALID;

	/* Refuses an answer of an earlier firmware epoch. */
	if (hardware_epoch != session->hardware_epoch)
		return INTEL_AX211_SCAN_SESSION_STALE;

	/* An answer after the scan ended is a repeat. */
	if (session->phase == INTEL_AX211_SCAN_SESSION_TERMINAL)
		return INTEL_AX211_SCAN_SESSION_DUPLICATE;

	/* An answer in another phase is a repeat when that phase follows it. */
	if (session->phase != required_phase) {
		/* The phase the answer leads to has already been reached. */
		if ((required_phase ==
			     INTEL_AX211_SCAN_SESSION_WAIT_START_ACK &&
		     session->phase == INTEL_AX211_SCAN_SESSION_RUNNING) ||
		    (required_phase ==
			     INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK &&
		     session->phase ==
			     INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE)) {
			return INTEL_AX211_SCAN_SESSION_DUPLICATE;
		}

		/* Any other phase is out of order. */
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	}

	/* Refuses a cancel answer without a place for its status word. */
	if (required_phase == INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK &&
	    abort_status == NULL) {
		return INTEL_AX211_SCAN_SESSION_INVALID;
	}

	/* Refuses a status word place for an answer that has none. */
	if (required_phase != INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK &&
	    abort_status != NULL) {
		return INTEL_AX211_SCAN_SESSION_INVALID;
	}

	/* A late answer is handled as an expiry. */
	if (now_us >= session->command_deadline) {
		result = drv_intel_ax211_scan_session_expire(session, now_us);
		return result;
	}

	/* Refuses an event that does not decode. */
	result = drv_intel_ax211_event_decode(event_bytes, event_length, &event);
	if (result != INTEL_AX211_SUCCESS)
		return INTEL_AX211_SCAN_SESSION_COMMAND;

	/* Refuses an answer from another ring slot. */
	if (event.queue != session->command_handle.token.queue ||
	    event.index != session->command_handle.token.index) {
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	}

	/* Gives a cancel's answer a buffer for its status word. */
	response_bytes = NULL;
	response_capacity = 0U;
	if (abort_status != NULL) {
		kern_memset(response, 0, sizeof(response));
		response_bytes = response;
		response_capacity = sizeof(response);
	}

	/* Completes the command; a failure ends the session's scan. */
	response_length = 0U;
	result = drv_intel_ax211_command_complete(
		session->commands, event_bytes, event_length, hardware_epoch,
		response_bytes, response_capacity, &response_length);
	result = ax211_scan_session_command_result(result);
	if (result != INTEL_AX211_SCAN_SESSION_OK) {
		result = ax211_scan_session_terminal(session, result);
		return result;
	}

	/*
	 * Ends the session's scan when the request was answered with a payload.
	 */
	if (abort_status == NULL && response_length != 0U) {
		result = ax211_scan_session_terminal(
			session, INTEL_AX211_SCAN_SESSION_COMMAND);
		return result;
	}

	/* Reports the cancel's status word. */
	if (abort_status != NULL) {
		/* Ends the session's scan when the word is missing. */
		if (response_length != sizeof(response)) {
			result = ax211_scan_session_terminal(
				session, INTEL_AX211_SCAN_SESSION_COMMAND);
			return result;
		}

		/* Assembles the little-endian word. */
		*abort_status = (uint32_t)response[0U] |
				((uint32_t)response[1U] << 8) |
				((uint32_t)response[2U] << 16) |
				((uint32_t)response[3U] << 24);
	}

	/* Succeeded: the command is complete. */
	return INTEL_AX211_SCAN_SESSION_OK;
}

/* Ends the session's scan with a result, and reports that result. */
static int
ax211_scan_session_terminal(
	struct intel_ax211_scan_session *session,
	int result)
{
	/* Records the end and its result. */
	session->phase = INTEL_AX211_SCAN_SESSION_TERMINAL;
	session->terminal_result = (uint8_t)result;

	/* Reports the result the scan ended with. */
	return result;
}
