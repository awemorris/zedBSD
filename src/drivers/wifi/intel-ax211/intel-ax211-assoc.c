/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 association-session implementation.
 */

#include "intel-ax211-assoc.h"
#include <kern/kcrt.h>

/*
 * The firmware objects one association session has created, so that a
 * rollback removes exactly those, in the reverse order.  zedBSD's own bits.
 */
#define AX211_ASSOC_RESOURCE_PHY 0x01U
#define AX211_ASSOC_RESOURCE_MAC 0x02U
#define AX211_ASSOC_RESOURCE_LINK 0x04U
#define AX211_ASSOC_RESOURCE_LINK_ACTIVE 0x08U
#define AX211_ASSOC_RESOURCE_STATION 0x10U
#define AX211_ASSOC_RESOURCE_QUEUE 0x20U
#define AX211_ASSOC_RESOURCE_SESSION 0x40U

/*
 * The field values of the association commands.
 *
 * - Actions: 1 adds, 2 modifies and 3 removes an object; the same three
 *   codes serve the channel context, interface, link and time reservation.
 *   0xffffffff in the channel context field of a link means "no channel".
 * - Band codes of the channel context: 1 for 2.4 GHz and 0 for 5 GHz.
 * - Interface kind 5 is a client station of an access point.
 * - Receive filter: DELIVER_GROUP_FRAMES (bit 2) passes multicast and
 *   broadcast data, and DELIVER_BEACONS (bit 3) passes beacons to the host
 *   after association, which zedBSD needs because its link-liveness check
 *   is fed by delivered beacons.  NO_ACK_FOR_HOST is 1 in the interface's
 *   no-ACK field.
 * - Link change bits: ACTIVITY (bit 0) changes the active flag, ACK_RATES
 *   (bit 1) the rates and preamble, ACCESS_PARAMETERS (bit 3) the QoS and
 *   EDCA entries, BEACON_CLOCK (bit 4) the beacon and DTIM intervals.  The
 *   driver activates the link with bits 0 and 1, updates it after
 *   association with bits 1, 3 and 4, and deactivates it with bit 0.
 * - Station kind 0 is a peer, here the access point.
 * - Queue operations: 0 adds and 1 removes; the firmware's queue numbers run
 *   from 1 to 511.  Management frames use TID 15, and the queue's size code
 *   5 announces 256 descriptors (log2 of the count minus 3).
 * - Time reservation configuration 0 is the reservation for joining a
 *   network; a notice with another configuration is ignored.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/mac-cfg.h (actions, interface
 * kinds, receive filter bits, link change bits and station kinds),
 * drivers/net/wireless/intel/iwlwifi/fw/api/phy-ctxt.h (band codes),
 * drivers/net/wireless/intel/iwlwifi/fw/api/datapath.h (queue operations) and
 * drivers/net/wireless/intel/iwlwifi/fw/api/time-event.h (the time reservation
 * configurations).
 */
#define AX211_ASSOC_ACTION_ADD 1U
#define AX211_ASSOC_ACTION_MODIFY 2U
#define AX211_ASSOC_ACTION_REMOVE 3U
#define AX211_ASSOC_INVALID_CONTEXT 0xffffffffU
#define AX211_ASSOC_CHANNEL_IN_5GHZ 0U
#define AX211_ASSOC_CHANNEL_IN_24GHZ 1U
#define AX211_ASSOC_INTERFACE_KIND_CLIENT 5U
#define AX211_ASSOC_DELIVER_GROUP_FRAMES 0x04U
#define AX211_ASSOC_DELIVER_BEACONS 0x08U
#define AX211_ASSOC_NO_ACK_FOR_HOST 1U
#define AX211_ASSOC_LINK_CHANGES_ACTIVITY 0x01U
#define AX211_ASSOC_LINK_CHANGES_ACK_RATES 0x02U
#define AX211_ASSOC_LINK_CHANGES_ACCESS_PARAMETERS 0x08U
#define AX211_ASSOC_LINK_CHANGES_BEACON_CLOCK 0x10U
#define AX211_ASSOC_PEER_KIND_ACCESS_POINT 0U
#define AX211_ASSOC_QUEUE_OPERATION_ADD 0U
#define AX211_ASSOC_QUEUE_OPERATION_REMOVE 1U
#define AX211_ASSOC_QUEUE_MIN 1U
#define AX211_ASSOC_QUEUE_MAX 511U
#define AX211_ASSOC_MANAGEMENT_TID 15U
#define AX211_ASSOC_QUEUE_CB_SIZE 5U
#define AX211_ASSOC_RESERVATION_FOR_JOIN 0U


static int ax211_assoc_required_version(const struct intel_ax211_protocol_command_table *table, uint8_t group, uint8_t opcode, uint8_t command_version, uint8_t notification_version);
static int ax211_assoc_address_valid(const uint8_t address[6]);
static void ax211_assoc_put_le32(uint8_t *bytes, uint32_t value);
static void ax211_assoc_put_le16(uint8_t *bytes, uint16_t value);
static uint32_t ax211_assoc_get_le32(const uint8_t *bytes);
static int ax211_assoc_optional_version(const struct intel_ax211_protocol_command_table *table, uint8_t group, uint8_t opcode, uint8_t command_version, uint8_t notification_version);
static int ax211_assoc_profile_valid(const struct intel_ax211_assoc_profile *profile);
static int ax211_assoc_set_step(struct intel_ax211_assoc_state *state, enum intel_ax211_assoc_step step, uint64_t now_us);
static uint32_t ax211_assoc_next_sequence(uint32_t sequence);
static int ax211_assoc_update_valid(const struct intel_ax211_assoc_profile *profile, const struct intel_ax211_assoc_update *update);
static int ax211_assoc_command_encode(const struct intel_ax211_assoc_state *state, struct intel_ax211_assoc_command *command);
static void ax211_assoc_command_base(const struct intel_ax211_assoc_state *state, struct intel_ax211_assoc_command *command);
static void ax211_assoc_mac_config_encode(const struct intel_ax211_assoc_state *state, uint32_t action, int associated, struct intel_ax211_assoc_command *command);
static void ax211_assoc_link_config_encode(const struct intel_ax211_assoc_state *state, uint32_t action, uint32_t modify_mask, uint32_t active, uint32_t phy_id, struct intel_ax211_assoc_command *command);
static void ax211_assoc_edca_encode(const struct intel_ax211_assoc_profile *profile, uint8_t *output);
static void ax211_assoc_phy_encode(const struct intel_ax211_assoc_state *state, uint32_t action, struct intel_ax211_assoc_command *command);
static void ax211_assoc_rlc_encode(const struct intel_ax211_assoc_state *state, struct intel_ax211_assoc_command *command);
static void ax211_assoc_station_config_encode(const struct intel_ax211_assoc_state *state, int associated, struct intel_ax211_assoc_command *command);
static void ax211_assoc_queue_encode(const struct intel_ax211_assoc_state *state, uint32_t operation, struct intel_ax211_assoc_command *command);
static void ax211_assoc_put_le64(uint8_t *bytes, uint64_t value);
static void ax211_assoc_session_encode(const struct intel_ax211_assoc_state *state, uint32_t action, struct intel_ax211_assoc_command *command);
static void ax211_assoc_station_remove_encode(struct intel_ax211_assoc_command *command);
static int ax211_assoc_command_matches(const struct intel_ax211_assoc_state *state, const struct intel_ax211_assoc_command *command);
static int ax211_assoc_rollback_begin(struct intel_ax211_assoc_state *state, int failure, uint64_t now_us);
static enum intel_ax211_assoc_step ax211_assoc_rollback_step(uint32_t resources);
static int ax211_assoc_response_validate(const struct intel_ax211_assoc_command *command, const struct intel_ax211_assoc_reply *reply);
static uint16_t ax211_assoc_get_le16(const uint8_t *bytes);
static void ax211_assoc_mark_uncertain(struct intel_ax211_assoc_state *state, enum intel_ax211_assoc_step step);
static void ax211_assoc_mark_success(struct intel_ax211_assoc_state *state, enum intel_ax211_assoc_step step);
static int ax211_assoc_advance(struct intel_ax211_assoc_state *state, uint64_t now_us);
static int ax211_assoc_exchange_failure(struct intel_ax211_assoc_state *state, int result, uint64_t now_us);

/*
 * Checks that the image lists the multicast filter command at the version this
 * driver encodes.
 */
int
drv_intel_ax211_assoc_mcast_filter_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	int result;

	/* Refuses a missing table. */
	if (table == NULL)
		return INTEL_AX211_ASSOC_INVALID;

	/* Refuses a table that is not the API 89 table. */
	result = drv_intel_ax211_protocol_command_table_validate_api89(table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_ASSOC_UNSUPPORTED;

	/* Requires the multicast filter command's version. */
	result = ax211_assoc_required_version(
		table, INTEL_AX211_ASSOC_FAMILY_CLASSIC_WIDE,
		INTEL_AX211_ASSOC_MULTICAST_LIST_OPCODE,
		INTEL_AX211_ASSOC_MULTICAST_LIST_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Succeeded: the command is supported. */
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Encodes the multicast filter that passes all multicast of the network named
 * by bssid.
 */
int
drv_intel_ax211_assoc_mcast_filter_encode(
	const uint8_t bssid[6],
	uint8_t *output,
	size_t output_capacity)
{
	int valid;

	/* Refuses missing arguments. */
	if (bssid == NULL || output == NULL)
		return INTEL_AX211_ASSOC_INVALID;

	/* Refuses an unset or group address. */
	valid = ax211_assoc_address_valid(bssid);
	if (!valid)
		return INTEL_AX211_ASSOC_INVALID;

	/* Refuses a buffer smaller than the command. */
	if (output_capacity < INTEL_AX211_ASSOC_MULTICAST_LIST_SIZE)
		return INTEL_AX211_ASSOC_BUFFER_TOO_SMALL;

	/* Succeeded: fills the command. */
	kern_memset(output, 0, INTEL_AX211_ASSOC_MULTICAST_LIST_SIZE);
	output[0U] = 1U;
	output[3U] = 1U;
	kern_memcpy(output + 4U, bssid, 6U);
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Checks that the image lists the power-save command at the version this
 * driver encodes.
 */
int
drv_intel_ax211_assoc_mac_power_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	int result;

	/* Refuses a missing table. */
	if (table == NULL)
		return INTEL_AX211_ASSOC_INVALID;

	/* Refuses a table that is not the API 89 table. */
	result = drv_intel_ax211_protocol_command_table_validate_api89(table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_ASSOC_UNSUPPORTED;

	/* Requires the power-save command's version. */
	result = ax211_assoc_required_version(
		table, INTEL_AX211_ASSOC_FAMILY_CLASSIC_WIDE,
		INTEL_AX211_ASSOC_POWER_SAVE_OPCODE,
		INTEL_AX211_ASSOC_POWER_SAVE_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Succeeded: the command is supported. */
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Encodes the power-save command: power save off and a keep-alive period of
 * three DTIM intervals.
 */
int
drv_intel_ax211_assoc_mac_power_encode(
	uint8_t dtim_period,
	uint32_t beacon_interval_ms,
	uint8_t *output,
	size_t output_capacity)
{
	uint64_t keep_alive_milliseconds;
	uint64_t keep_alive_seconds;
	uint32_t effective_dtim;

	/* Refuses a missing buffer and a zero beacon interval. */
	if (output == NULL || beacon_interval_ms == 0U)
		return INTEL_AX211_ASSOC_INVALID;

	/* Refuses a buffer smaller than the command. */
	if (output_capacity < INTEL_AX211_ASSOC_POWER_SAVE_SIZE)
		return INTEL_AX211_ASSOC_BUFFER_TOO_SMALL;

	/* Treats an unknown DTIM period as every beacon. */
	if (dtim_period == 0U)
		effective_dtim = 1U;
	else
		effective_dtim = dtim_period;

	/* Rounds three DTIM intervals up to whole seconds. */
	keep_alive_milliseconds =
		UINT64_C(3) * effective_dtim * beacon_interval_ms;
	keep_alive_seconds =
		(keep_alive_milliseconds + UINT64_C(999)) / UINT64_C(1000);

	/* Raises a shorter period to the minimum. */
	if (keep_alive_seconds <
	    INTEL_AX211_ASSOC_POWER_SAVE_KEEP_ALIVE_MIN_SECONDS) {
		keep_alive_seconds =
			INTEL_AX211_ASSOC_POWER_SAVE_KEEP_ALIVE_MIN_SECONDS;
	}

	/* Refuses a period the 16-bit field cannot hold. */
	if (keep_alive_seconds > UINT16_MAX)
		return INTEL_AX211_ASSOC_OVERSIZED;

	/* Succeeded: fills the command. */
	kern_memset(output, 0, INTEL_AX211_ASSOC_POWER_SAVE_SIZE);
	ax211_assoc_put_le32(output, INTEL_AX211_ASSOC_INTERFACE_SLOT);
	ax211_assoc_put_le16(output + 4U, 0U);
	ax211_assoc_put_le16(output + 6U, (uint16_t)keep_alive_seconds);
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Checks the power-save command's answer.
 */
int
drv_intel_ax211_assoc_mac_power_response_validate(
	const uint8_t *response,
	size_t response_length)
{
	uint32_t status;

	/* Refuses a missing answer. */
	if (response == NULL)
		return INTEL_AX211_ASSOC_INVALID;

	/*
	 * API 89 may acknowledge MAC_PM_POWER_TABLE with no payload.  The
	 * command-response header remains the authoritative success result.
	 */
	if (response_length == 0U)
		return INTEL_AX211_ASSOC_OK;

	/* Refuses an answer shorter than the status word. */
	if (response_length < INTEL_AX211_ASSOC_POWER_SAVE_RESPONSE_SIZE)
		return INTEL_AX211_ASSOC_TRUNCATED;

	/* Refuses an answer longer than the status word. */
	if (response_length > INTEL_AX211_ASSOC_POWER_SAVE_RESPONSE_SIZE)
		return INTEL_AX211_ASSOC_OVERSIZED;

	/* Refuses a nonzero status. */
	status = ax211_assoc_get_le32(response);
	if (status != 0U)
		return INTEL_AX211_ASSOC_FIRMWARE;

	/* Succeeded: the firmware accepted the command. */
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Checks that the image lists every association command at the version this
 * driver encodes.
 */
int
drv_intel_ax211_assoc_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	int result;

	/* Refuses a missing table. */
	if (table == NULL)
		return INTEL_AX211_ASSOC_INVALID;

	/* Refuses a table that is not the API 89 table. */
	result = drv_intel_ax211_protocol_command_table_validate_api89(table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_ASSOC_UNSUPPORTED;

	/* Requires the channel context command. */
	result = ax211_assoc_required_version(
		table, INTEL_AX211_ASSOC_FAMILY_CLASSIC_WIDE,
		INTEL_AX211_ASSOC_CHANNEL_CONTEXT_OPCODE,
		INTEL_AX211_ASSOC_CHANNEL_CONTEXT_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Requires the interface command. */
	result = ax211_assoc_required_version(
		table, INTEL_AX211_ASSOC_FAMILY_LINK_SETUP,
		INTEL_AX211_ASSOC_INTERFACE_SETUP_OPCODE,
		INTEL_AX211_ASSOC_INTERFACE_SETUP_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Requires the link command. */
	result = ax211_assoc_required_version(
		table, INTEL_AX211_ASSOC_FAMILY_LINK_SETUP,
		INTEL_AX211_ASSOC_LINK_SETUP_OPCODE,
		INTEL_AX211_ASSOC_LINK_SETUP_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Accepts the station command unlisted or at its version. */
	result = ax211_assoc_optional_version(
		table, INTEL_AX211_ASSOC_FAMILY_LINK_SETUP,
		INTEL_AX211_ASSOC_PEER_SETUP_OPCODE,
		INTEL_AX211_ASSOC_PEER_SETUP_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Accepts the station removal unlisted or at its version. */
	result = ax211_assoc_optional_version(
		table, INTEL_AX211_ASSOC_FAMILY_LINK_SETUP,
		INTEL_AX211_ASSOC_PEER_REMOVE_OPCODE,
		INTEL_AX211_ASSOC_PEER_REMOVE_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Requires the receive chain command. */
	result = ax211_assoc_required_version(
		table, INTEL_AX211_ASSOC_FAMILY_TRAFFIC,
		INTEL_AX211_ASSOC_RECEIVE_CHAINS_OPCODE,
		INTEL_AX211_ASSOC_RECEIVE_CHAINS_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Requires the queue command and its answer. */
	result = ax211_assoc_required_version(
		table, INTEL_AX211_ASSOC_FAMILY_TRAFFIC,
		INTEL_AX211_ASSOC_QUEUE_SETUP_OPCODE,
		INTEL_AX211_ASSOC_QUEUE_SETUP_VERSION,
		INTEL_AX211_ASSOC_QUEUE_RESPONSE_VERSION);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Requires the time reservation command. */
	result = ax211_assoc_required_version(
		table, INTEL_AX211_ASSOC_FAMILY_LINK_SETUP,
		INTEL_AX211_ASSOC_TIME_RESERVATION_OPCODE,
		INTEL_AX211_ASSOC_TIME_RESERVATION_VERSION, 0U);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Requires the time reservation notice. */
	result = ax211_assoc_required_version(
		table, INTEL_AX211_ASSOC_FAMILY_LINK_SETUP,
		INTEL_AX211_ASSOC_TIME_RESERVATION_NOTICE_OPCODE,
		INTEL_AX211_PROTOCOL_UNKNOWN_VERSION,
		INTEL_AX211_ASSOC_TIME_RESERVATION_NOTICE_API89_VERSION);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Requires the multicast filter command. */
	result = drv_intel_ax211_assoc_mcast_filter_api89_validate(table);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Requires the power-save command. */
	result = drv_intel_ax211_assoc_mac_power_api89_validate(table);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Succeeded: every association command is supported. */
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Starts an association session for profile and selects its first command.
 */
int
drv_intel_ax211_assoc_begin(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_assoc_profile *profile,
	uint64_t common_generation,
	uint32_t hardware_epoch,
	uint64_t now_us)
{
	uint32_t next_sequence;
	uint32_t last_completed_sequence;
	int valid;
	int result;

	/* Refuses a missing state. */
	if (state == NULL)
		return INTEL_AX211_ASSOC_INVALID;

	/* Refuses a profile the commands cannot carry. */
	valid = ax211_assoc_profile_valid(profile);
	if (!valid)
		return INTEL_AX211_ASSOC_INVALID;

	/* Refuses an unset operation or firmware boot. */
	if (common_generation == 0U || hardware_epoch == 0U)
		return INTEL_AX211_ASSOC_INVALID;

	/* Requires every association command. */
	result = drv_intel_ax211_assoc_api89_validate(table);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Refuses a session while another one runs. */
	if (state->initialized && state->phase != INTEL_AX211_ASSOC_PHASE_IDLE)
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;

	/* Refuses a second session of the same operation. */
	if (state->initialized &&
	    common_generation == state->last_common_generation) {
		return INTEL_AX211_ASSOC_STALE;
	}

	/* Restarts the state machine but keeps the sequence numbering. */
	if (state->initialized) {
		next_sequence = state->next_sequence;
		last_completed_sequence = state->last_completed_sequence;
	} else {
		next_sequence = 1U;
		last_completed_sequence = 0U;
	}

	/* Clears the state and records the new session. */
	kern_memset(state, 0, sizeof(*state));
	state->profile = *profile;
	state->common_generation = common_generation;
	state->hardware_epoch = hardware_epoch;
	state->last_common_generation = common_generation;
	state->next_sequence = next_sequence;
	state->last_completed_sequence = last_completed_sequence;
	state->phase = INTEL_AX211_ASSOC_PHASE_AUTH;
	state->failure = INTEL_AX211_ASSOC_OK;
	state->initialized = 1U;

	/* Selects the first command; a failure leaves the state cleared. */
	result = ax211_assoc_set_step(state, INTEL_AX211_ASSOC_STEP_MAC_ADD,
				      now_us);
	if (result != INTEL_AX211_ASSOC_OK) {
		kern_memset(state, 0, sizeof(*state));
		return result;
	}

	/* Succeeded: the first command is ready. */
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Starts the association stage of an authenticated session with the access
 * point's answer.
 */
int
drv_intel_ax211_assoc_begin_update(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_update *update,
	uint64_t common_generation,
	uint32_t hardware_epoch,
	uint64_t now_us)
{
	int valid;
	int result;

	/* Refuses missing arguments and an unset operation or firmware boot. */
	if (state == NULL ||
	    !state->initialized ||
	    update == NULL ||
	    common_generation == 0U ||
	    hardware_epoch == 0U) {
		return INTEL_AX211_ASSOC_INVALID;
	}

	/* Refuses another operation or firmware boot. */
	if (common_generation != state->common_generation ||
	    hardware_epoch != state->hardware_epoch) {
		return INTEL_AX211_ASSOC_STALE;
	}

	/* Admits only a session whose authentication stage is done. */
	if (state->phase != INTEL_AX211_ASSOC_PHASE_AUTH_READY)
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;

	/* Refuses an answer the commands cannot carry. */
	valid = ax211_assoc_update_valid(&state->profile, update);
	if (!valid)
		return INTEL_AX211_ASSOC_INVALID;

	/* Selects the first command of the stage. */
	result = ax211_assoc_set_step(
		state, INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE, now_us);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Succeeded: the association stage runs with the answer. */
	state->update = *update;
	state->update_valid = 1U;
	state->phase = INTEL_AX211_ASSOC_PHASE_ASSOCIATING;
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Encodes the session's current command, or reports where the session
 * stands.
 */
int
drv_intel_ax211_assoc_current(
	const struct intel_ax211_assoc_state *state,
	uint64_t now_us,
	struct intel_ax211_assoc_command *command)
{
	int result;

	/* Refuses missing arguments and an unstarted state. */
	if (state == NULL || command == NULL || !state->initialized)
		return INTEL_AX211_ASSOC_INVALID;

	/* The authentication stage is done. */
	if (state->phase == INTEL_AX211_ASSOC_PHASE_AUTH_READY)
		return INTEL_AX211_ASSOC_AUTH_READY;

	/* The association is done. */
	if (state->phase == INTEL_AX211_ASSOC_PHASE_ASSOCIATED)
		return INTEL_AX211_ASSOC_COMPLETE;

	/* The rollback failed. */
	if (state->phase == INTEL_AX211_ASSOC_PHASE_FAILED)
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;

	/* An idle state either never started or has rolled back. */
	if (state->phase == INTEL_AX211_ASSOC_PHASE_IDLE) {
		/* Refuses a state that never failed. */
		if (state->failure == INTEL_AX211_ASSOC_OK)
			return INTEL_AX211_ASSOC_INVALID;

		/* Reports the finished rollback. */
		return INTEL_AX211_ASSOC_ROLLED_BACK;
	}

	/* Refuses a running state without a command. */
	if (state->step == INTEL_AX211_ASSOC_STEP_NONE ||
	    state->active_sequence == 0U) {
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;
	}

	/* The command's time ran out. */
	if (now_us >= state->deadline)
		return INTEL_AX211_ASSOC_TIMEOUT;

	/* Encodes the command. */
	result = ax211_assoc_command_encode(state, command);
	if (result != INTEL_AX211_ASSOC_OK)
		return result;

	/* Succeeded: *command is the command to send. */
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Accepts the firmware's answer to the current command and selects the next
 * one.
 */
int
drv_intel_ax211_assoc_accept(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_command *command,
	const struct intel_ax211_assoc_reply *reply,
	uint64_t now_us)
{
	int matches;
	int result;

	/* Refuses missing arguments and an unstarted state. */
	if (state == NULL ||
	    command == NULL ||
	    reply == NULL ||
	    !state->initialized) {
		return INTEL_AX211_ASSOC_INVALID;
	}

	/* Refuses a command or answer of another operation or firmware boot. */
	if (command->common_generation != state->common_generation ||
	    command->hardware_epoch != state->hardware_epoch ||
	    reply->common_generation != state->common_generation ||
	    reply->hardware_epoch != state->hardware_epoch) {
		return INTEL_AX211_ASSOC_STALE;
	}

	/* Refuses a repeat of the answer already accepted. */
	if (command->sequence == state->last_completed_sequence ||
	    reply->sequence == state->last_completed_sequence) {
		return INTEL_AX211_ASSOC_DUPLICATE;
	}

	/* Refuses a command or answer of another step. */
	if (command->step != state->step ||
	    reply->step != state->step ||
	    command->sequence != state->active_sequence ||
	    reply->sequence != state->active_sequence) {
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;
	}

	/* Refuses a command that differs from the one the state encodes. */
	matches = ax211_assoc_command_matches(state, command);
	if (!matches)
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;

	/* A late answer expires the command. */
	if (now_us >= state->deadline) {
		result = drv_intel_ax211_assoc_expire(state, now_us);
		return result;
	}

	/* A refused command ends the session. */
	if (reply->acknowledgement != 0) {
		/* A refusal during the rollback fails the rollback. */
		if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK) {
			state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
			state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
			return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		}

		/* Starts the rollback and reports the refusal. */
		(void)ax211_assoc_rollback_begin(
			state, INTEL_AX211_ASSOC_FIRMWARE, now_us);
		return INTEL_AX211_ASSOC_FIRMWARE;
	}

	/* A malformed answer ends the session. */
	result = ax211_assoc_response_validate(command, reply);
	if (result != INTEL_AX211_ASSOC_OK) {
		/* A malformed answer during the rollback fails the rollback. */
		if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK) {
			state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
			state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
			return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		}

		/* Starts the rollback and reports the malformed answer. */
		ax211_assoc_mark_uncertain(state, state->step);
		(void)ax211_assoc_rollback_begin(state, result, now_us);
		return result;
	}

	/* Records the answer and selects the next command. */
	state->last_completed_sequence = state->active_sequence;
	ax211_assoc_mark_success(state, state->step);
	result = ax211_assoc_advance(state, now_us);

	/* Reports a session that could not go on. */
	if (result != INTEL_AX211_ASSOC_PENDING &&
	    result != INTEL_AX211_ASSOC_AUTH_READY &&
	    result != INTEL_AX211_ASSOC_COMPLETE) {
		return result;
	}

	/* Succeeded: reports the next command or the end of a stage. */
	return result;
}

/*
 * Accepts the firmware's notice about the session's time reservation.
 */
int
drv_intel_ax211_assoc_session_event_accept(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_protocol_message *message,
	uint64_t common_generation,
	uint32_t hardware_epoch)
{
	uint32_t mac_id;
	uint32_t status;
	uint32_t start;
	uint32_t configuration_id;

	/* Refuses missing arguments and unset generations. */
	if (state == NULL ||
	    message == NULL ||
	    !state->initialized ||
	    common_generation == 0U ||
	    hardware_epoch == 0U ||
	    message->generation == 0U) {
		return INTEL_AX211_ASSOC_INVALID;
	}

	/* Refuses a notice of another operation or firmware boot. */
	if (common_generation != state->common_generation ||
	    hardware_epoch != state->hardware_epoch ||
	    message->generation != hardware_epoch) {
		return INTEL_AX211_ASSOC_STALE;
	}

	/* Refuses any other notification and another layout. */
	if (message->group != INTEL_AX211_ASSOC_FAMILY_LINK_SETUP ||
	    message->opcode != INTEL_AX211_ASSOC_TIME_RESERVATION_NOTICE_OPCODE ||
	    message->version !=
		    INTEL_AX211_ASSOC_TIME_RESERVATION_NOTICE_LAYOUT_VERSION) {
		return INTEL_AX211_ASSOC_UNSUPPORTED;
	}

	/* Refuses a message the firmware marked as failed. */
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_ASSOC_FIRMWARE;

	/* Refuses a command answer. */
	if ((message->queue & 0x80U) == 0U)
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;

	/* Refuses a notice without a payload. */
	if (message->payload == NULL)
		return INTEL_AX211_ASSOC_INVALID;

	/* Refuses a short notice. */
	if (message->payload_length <
	    INTEL_AX211_ASSOC_TIME_RESERVATION_NOTICE_SIZE) {
		return INTEL_AX211_ASSOC_TRUNCATED;
	}

	/* Refuses a long notice. */
	if (message->payload_length >
	    INTEL_AX211_ASSOC_TIME_RESERVATION_NOTICE_SIZE) {
		return INTEL_AX211_ASSOC_OVERSIZED;
	}

	/* Refuses a status or start flag other than 0 or 1. */
	mac_id = ax211_assoc_get_le32(message->payload);
	status = ax211_assoc_get_le32(message->payload + 4U);
	start = ax211_assoc_get_le32(message->payload + 8U);
	configuration_id = ax211_assoc_get_le32(message->payload + 12U);
	if (status > 1U || start > 1U)
		return INTEL_AX211_ASSOC_FIRMWARE;

	/*
	 * Ignores another interface, another reservation, a failure, and the
	 * start of the reservation.
	 */
	if (mac_id != INTEL_AX211_ASSOC_INTERFACE_SLOT ||
	    configuration_id != AX211_ASSOC_RESERVATION_FOR_JOIN ||
	    status != 1U ||
	    start != 0U) {
		return INTEL_AX211_ASSOC_EVENT_IGNORED;
	}

	/* Refuses a second end of the reservation. */
	if (state->session_ended != 0U)
		return INTEL_AX211_ASSOC_DUPLICATE;

	/* Succeeded: the reservation ended and needs no removal. */
	state->session_ended = 1U;
	state->resources &= ~AX211_ASSOC_RESOURCE_SESSION;
	return INTEL_AX211_ASSOC_SESSION_EXPIRED;
}

/*
 * Ends the current command whose time ran out and starts the rollback.
 */
int
drv_intel_ax211_assoc_expire(
	struct intel_ax211_assoc_state *state,
	uint64_t now_us)
{
	/* Refuses a state without a command whose time ran out. */
	if (state == NULL ||
	    !state->initialized ||
	    state->step == INTEL_AX211_ASSOC_STEP_NONE ||
	    now_us < state->deadline) {
		return INTEL_AX211_ASSOC_INVALID;
	}

	/* A timeout during the rollback fails the rollback. */
	if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK) {
		state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
		state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	}

	/* Starts the rollback and reports the timeout. */
	ax211_assoc_mark_uncertain(state, state->step);
	(void)ax211_assoc_rollback_begin(state, INTEL_AX211_ASSOC_TIMEOUT,
					 now_us);
	return INTEL_AX211_ASSOC_TIMEOUT;
}

/*
 * Cancels a running session by starting its rollback.
 */
int
drv_intel_ax211_assoc_cancel(
	struct intel_ax211_assoc_state *state,
	uint64_t common_generation,
	uint32_t hardware_epoch,
	uint64_t now_us)
{
	int result;

	/* Refuses missing arguments and an unset operation or firmware boot. */
	if (state == NULL ||
	    !state->initialized ||
	    common_generation == 0U ||
	    hardware_epoch == 0U) {
		return INTEL_AX211_ASSOC_INVALID;
	}

	/* Refuses another operation or firmware boot. */
	if (common_generation != state->common_generation ||
	    hardware_epoch != state->hardware_epoch) {
		return INTEL_AX211_ASSOC_STALE;
	}

	/* The rollback already runs. */
	if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK)
		return INTEL_AX211_ASSOC_PENDING;

	/* The session has already rolled back. */
	if (state->phase == INTEL_AX211_ASSOC_PHASE_IDLE)
		return INTEL_AX211_ASSOC_ROLLED_BACK;

	/* The rollback already failed. */
	if (state->phase == INTEL_AX211_ASSOC_PHASE_FAILED)
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;

	/* Starts the rollback. */
	ax211_assoc_mark_uncertain(state, state->step);
	result = ax211_assoc_rollback_begin(
		state, INTEL_AX211_ASSOC_ROLLED_BACK, now_us);
	if (result == INTEL_AX211_ASSOC_ROLLBACK_FAILED)
		return result;

	/* Succeeded: the rollback is pending or already done. */
	return result;
}

/*
 * Runs the session through ops until it reaches the end of a stage, rolls
 * back, or fails.
 */
int
drv_intel_ax211_assoc_drive(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_ops *ops,
	void *argument)
{
	struct intel_ax211_assoc_command command;
	struct intel_ax211_assoc_reply reply;
	uint64_t now_us;
	int result;
	unsigned int count;

	/* Refuses missing arguments and incomplete operations. */
	if (state == NULL ||
	    ops == NULL ||
	    ops->clock_us == NULL ||
	    ops->exchange == NULL) {
		return INTEL_AX211_ASSOC_INVALID;
	}

	/* Sends at most the command limit of one session. */
	for (count = 0U; count < INTEL_AX211_ASSOC_COMMAND_LIMIT; count++) {
		/* Encodes the current command. */
		now_us = ops->clock_us(argument);
		result = drv_intel_ax211_assoc_current(state, now_us, &command);

		/* Stops at the end of a stage, a rollback, or its failure. */
		if (result == INTEL_AX211_ASSOC_AUTH_READY ||
		    result == INTEL_AX211_ASSOC_COMPLETE ||
		    result == INTEL_AX211_ASSOC_ROLLED_BACK ||
		    result == INTEL_AX211_ASSOC_ROLLBACK_FAILED) {
			/* A rollback reports the failure that caused it. */
			if (result == INTEL_AX211_ASSOC_ROLLED_BACK)
				return state->failure;

			/* Reports where the session stands. */
			return result;
		}

		/* Expires a command whose time ran out. */
		if (result == INTEL_AX211_ASSOC_TIMEOUT) {
			(void)drv_intel_ax211_assoc_expire(state, now_us);
			continue;
		}

		/* Reports any other refusal. */
		if (result != INTEL_AX211_ASSOC_OK)
			return result;

		/* Sends the command; a failed exchange starts the rollback. */
		kern_memset(&reply, 0, sizeof(reply));
		result = ops->exchange(argument, &command, &reply);
		now_us = ops->clock_us(argument);
		if (result != INTEL_AX211_ASSOC_OK) {
			(void)ax211_assoc_exchange_failure(state, result,
							   now_us);
			continue;
		}

		/* Stops at the end of a stage. */
		result = drv_intel_ax211_assoc_accept(state, &command, &reply,
						      now_us);
		if (result == INTEL_AX211_ASSOC_AUTH_READY ||
		    result == INTEL_AX211_ASSOC_COMPLETE) {
			return result;
		}

		/* Goes on with the next command or the rollback. */
		if (result == INTEL_AX211_ASSOC_PENDING ||
		    result == INTEL_AX211_ASSOC_FIRMWARE ||
		    result == INTEL_AX211_ASSOC_TIMEOUT) {
			continue;
		}

		/* A rollback reports the failure that caused it. */
		if (result == INTEL_AX211_ASSOC_ROLLED_BACK)
			return state->failure;

		/* Starts the rollback for an answer that does not fit. */
		if (result == INTEL_AX211_ASSOC_STALE ||
		    result == INTEL_AX211_ASSOC_OUT_OF_ORDER ||
		    result == INTEL_AX211_ASSOC_DUPLICATE) {
			(void)ax211_assoc_exchange_failure(state, result,
							   now_us);
			continue;
		}

		/* Reports any other failure. */
		return result;
	}

	/* The command limit ran out; the rollback failed. */
	state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
	state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
}

/* Requires that the table lists a command at exactly the given versions. */
static int
ax211_assoc_required_version(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group,
	uint8_t opcode,
	uint8_t command_version,
	uint8_t notification_version)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Refuses an unlisted command. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, group, opcode, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_ASSOC_UNSUPPORTED;

	/* Refuses other versions. */
	if (version.command_version != command_version ||
	    version.notification_version != notification_version) {
		return INTEL_AX211_ASSOC_UNSUPPORTED;
	}

	/* Succeeded: the command is listed at the versions. */
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Reports whether address is a set individual address: 1 when it is, 0 when
 * not.
 */
static int
ax211_assoc_address_valid(
	const uint8_t address[6])
{
	size_t index;
	int any;

	/* Refuses a missing address and a group address. */
	if (address == NULL || (address[0] & 1U) != 0U)
		return 0;

	/* Looks for a nonzero byte. */
	any = 0;
	for (index = 0U; index < 6U; index++) {
		/* Notes a nonzero byte. */
		if (address[index] != 0U)
			any = 1;
	}

	/* Reports whether the address is set. */
	return any;
}

/* Stores a 32-bit value little-endian. */
static void
ax211_assoc_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Stores a 16-bit value little-endian. */
static void
ax211_assoc_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Reads a little-endian 32-bit value. */
static uint32_t
ax211_assoc_get_le32(
	const uint8_t *bytes)
{
	/* Assembles the word from its four bytes, least significant first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/*
 * API89 omits STA_CONFIG/STA_REMOVE from its command-version table.  The
 * firmware ABI defines their baseline v1 layouts implicitly.  Accept a matching
 * explicit entry for deterministic fixtures, but never accept an explicit
 * conflicting version.
 */
static int
ax211_assoc_optional_version(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group,
	uint8_t opcode,
	uint8_t command_version,
	uint8_t notification_version)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Accepts an unlisted command; refuses a failed lookup. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, group, opcode, &version);
	if (result == INTEL_AX211_PROTOCOL_MISSING)
		return INTEL_AX211_ASSOC_OK;
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_ASSOC_UNSUPPORTED;

	/* Refuses other versions. */
	if (version.command_version != command_version ||
	    version.notification_version != notification_version) {
		return INTEL_AX211_ASSOC_UNSUPPORTED;
	}

	/* Succeeded: the command is listed at the versions. */
	return INTEL_AX211_ASSOC_OK;
}

/*
 * Reports whether the commands can carry profile: 1 when they can, 0 when
 * not.
 */
static int
ax211_assoc_profile_valid(
	const struct intel_ax211_assoc_profile *profile)
{
	const struct intel_ax211_assoc_edca *edca;
	size_t index;
	uint8_t channel;
	int channel_valid;
	int valid;
	int result;

	/* Refuses a missing profile. */
	if (profile == NULL)
		return 0;

	/* Refuses an unset or group own address. */
	valid = ax211_assoc_address_valid(profile->station_address);
	if (!valid)
		return 0;

	/* Refuses an unset or group BSSID. */
	valid = ax211_assoc_address_valid(profile->bssid);
	if (!valid)
		return 0;

	/* Refuses a BSSID equal to the own address. */
	result = kern_memcmp(profile->station_address, profile->bssid, 6U);
	if (result == 0)
		return 0;

	/* Accepts only a channel one of the bands defines. */
	channel = profile->channel;
	channel_valid = 0;
	if (channel >= 1U && channel <= 14U)
		channel_valid = 1;
	if (channel >= 36U && channel <= 144U && ((channel - 36U) % 4U) == 0U)
		channel_valid = 1;
	if (channel >= 149U && channel <= 181U &&
	    ((channel - 149U) % 4U) == 0U) {
		channel_valid = 1;
	}

	/* Refuses any other channel. */
	if (!channel_valid)
		return 0;

	/* Refuses another channel width and an unusable chain mask. */
	if (profile->channel_width_mhz != INTEL_AX211_ASSOC_CHANNEL_WIDTH_MHZ ||
	    profile->rx_chain_mask == 0U ||
	    profile->rx_chain_mask > 7U) {
		return 0;
	}

	/* Refuses no ACK rate and flags other than 0 or 1. */
	if ((profile->cck_ack_rates | profile->ofdm_ack_rates) == 0U ||
	    profile->short_preamble > 1U ||
	    profile->short_slot > 1U ||
	    profile->qos > 1U) {
		return 0;
	}

	/* Refuses a zero beacon interval and unset queue addresses. */
	if (profile->beacon_interval_tu == 0U ||
	    profile->queue_byte_count_address == 0U ||
	    profile->queue_descriptor_address == 0U) {
		return 0;
	}

	/* Refuses misaligned queue addresses. */
	if ((profile->queue_byte_count_address & 3U) != 0U ||
	    (profile->queue_descriptor_address & 255U) != 0U) {
		return 0;
	}

	/* Checks every access category's parameters. */
	for (index = 0U; index < INTEL_AX211_ASSOC_EDCA_COUNT; index++) {
		/* Refuses out-of-range values and a reversed window. */
		edca = &profile->edca[index];
		if (edca->ecw_min > 15U ||
		    edca->ecw_max > 15U ||
		    edca->ecw_min > edca->ecw_max ||
		    edca->aifsn > 15U ||
		    edca->txop_32us > 2047U) {
			return 0;
		}
	}

	/* The profile is valid. */
	return 1;
}

/* Makes step the current command under a fresh sequence and deadline. */
static int
ax211_assoc_set_step(
	struct intel_ax211_assoc_state *state,
	enum intel_ax211_assoc_step step,
	uint64_t now_us)
{
	/* Refuses missing arguments and a deadline beyond the clock. */
	if (state == NULL ||
	    step == INTEL_AX211_ASSOC_STEP_NONE ||
	    now_us > UINT64_MAX - INTEL_AX211_ASSOC_COMMAND_TIMEOUT_US) {
		return INTEL_AX211_ASSOC_INVALID;
	}

	/* Succeeded: step is the current command. */
	state->step = step;
	state->active_sequence = state->next_sequence;
	state->next_sequence = ax211_assoc_next_sequence(state->next_sequence);
	state->deadline = now_us + INTEL_AX211_ASSOC_COMMAND_TIMEOUT_US;
	return INTEL_AX211_ASSOC_OK;
}

/* Returns the sequence number after sequence, skipping zero as unset. */
static uint32_t
ax211_assoc_next_sequence(
	uint32_t sequence)
{
	/* Skips zero when the counter wraps. */
	sequence++;
	if (sequence == 0U)
		sequence = 1U;

	/* Reports the next number. */
	return sequence;
}

/*
 * Reports whether the commands can carry update: 1 when they can, 0 when not.
 */
static int
ax211_assoc_update_valid(
	const struct intel_ax211_assoc_profile *profile,
	const struct intel_ax211_assoc_update *update)
{
	uint64_t dtim_offset;
	uint32_t interval;

	/* Refuses missing arguments and an association id out of range. */
	if (profile == NULL ||
	    update == NULL ||
	    update->association_id == 0U ||
	    update->association_id > 2007U) {
		return 0;
	}

	/*
	 * A probe response need not carry TIM.  Association is permitted
	 * without DTIM knowledge, and beacon delivery stays enabled.
	 *
	 * Procedure referenced: OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
	 * (iwx_mac_ctxt_cmd_fill_sta).
	 */
	if (update->dtim_period == 0U) {
		/* Refuses a DTIM count without a DTIM period. */
		if (update->dtim_count != 0U)
			return 0;

		/* The update is valid without DTIM knowledge. */
		return 1;
	}

	/* Refuses a DTIM count beyond the period. */
	if (update->dtim_count >= update->dtim_period)
		return 0;

	/* Refuses a DTIM interval or offset the fields cannot hold. */
	interval = (uint32_t)profile->beacon_interval_tu *
		   (uint32_t)update->dtim_period;
	dtim_offset = (uint64_t)update->dtim_count *
		      (uint64_t)profile->beacon_interval_tu * UINT64_C(1024);
	if (interval < profile->beacon_interval_tu ||
	    dtim_offset > UINT32_MAX) {
		return 0;
	}

	/* The update is valid. */
	return 1;
}

/* Encodes the command of the state's current step. */
static int
ax211_assoc_command_encode(
	const struct intel_ax211_assoc_state *state,
	struct intel_ax211_assoc_command *command)
{
	/* Fills the fields every command shares. */
	ax211_assoc_command_base(state, command);

	/* Encodes the step's command. */
	switch (state->step) {
	case INTEL_AX211_ASSOC_STEP_MAC_ADD:
		ax211_assoc_mac_config_encode(state, AX211_ASSOC_ACTION_ADD, 0,
					      command);
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ADD:
		ax211_assoc_link_config_encode(
			state, AX211_ASSOC_ACTION_ADD, 0U, 0U,
			AX211_ASSOC_INVALID_CONTEXT, command);
		break;
	case INTEL_AX211_ASSOC_STEP_PHY_ADD:
		ax211_assoc_phy_encode(state, AX211_ASSOC_ACTION_ADD, command);
		break;
	case INTEL_AX211_ASSOC_STEP_RECEIVE_CHAINS:
		ax211_assoc_rlc_encode(state, command);
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ASSIGN:
		ax211_assoc_link_config_encode(state, AX211_ASSOC_ACTION_MODIFY,
					       0U, 0U, 0U, command);
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ACTIVATE:
		ax211_assoc_link_config_encode(
			state, AX211_ASSOC_ACTION_MODIFY,
			AX211_ASSOC_LINK_CHANGES_ACTIVITY |
				AX211_ASSOC_LINK_CHANGES_ACK_RATES,
			1U, 0U, command);
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_ADD:
		ax211_assoc_station_config_encode(state, 0, command);
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE:
		ax211_assoc_queue_encode(state, AX211_ASSOC_QUEUE_OPERATION_ADD,
					 command);
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_PROTECT:
		ax211_assoc_session_encode(state, AX211_ASSOC_ACTION_ADD,
					   command);
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE:
		ax211_assoc_mac_config_encode(state, AX211_ASSOC_ACTION_MODIFY,
					      1, command);
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ASSOCIATE:
		ax211_assoc_link_config_encode(
			state, AX211_ASSOC_ACTION_MODIFY,
			AX211_ASSOC_LINK_CHANGES_ACK_RATES |
				AX211_ASSOC_LINK_CHANGES_ACCESS_PARAMETERS |
				AX211_ASSOC_LINK_CHANGES_BEACON_CLOCK,
			1U, 0U, command);
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_UPDATE:
		ax211_assoc_station_config_encode(state, 1, command);
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_REMOVE:
		ax211_assoc_session_encode(state, AX211_ASSOC_ACTION_REMOVE,
					   command);
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE:
		ax211_assoc_queue_encode(
			state, AX211_ASSOC_QUEUE_OPERATION_REMOVE, command);
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_REMOVE:
		ax211_assoc_station_remove_encode(command);
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE:
		ax211_assoc_link_config_encode(state, AX211_ASSOC_ACTION_MODIFY,
					       AX211_ASSOC_LINK_CHANGES_ACTIVITY,
					       0U, 0U, command);
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_REMOVE:
		ax211_assoc_link_config_encode(
			state, AX211_ASSOC_ACTION_REMOVE, 0U, 0U,
			AX211_ASSOC_INVALID_CONTEXT, command);
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_REMOVE:
		ax211_assoc_mac_config_encode(state, AX211_ASSOC_ACTION_REMOVE,
					      0, command);
		break;
	case INTEL_AX211_ASSOC_STEP_PHY_REMOVE:
		ax211_assoc_phy_encode(state, AX211_ASSOC_ACTION_REMOVE,
				       command);
		break;
	case INTEL_AX211_ASSOC_STEP_NONE:
	default:
		/* Refuses a state without a command. */
		return INTEL_AX211_ASSOC_INVALID;
	}

	/* Succeeded: *command is encoded. */
	return INTEL_AX211_ASSOC_OK;
}

/* Fills the step, sequence, generations and deadline of a command. */
static void
ax211_assoc_command_base(
	const struct intel_ax211_assoc_state *state,
	struct intel_ax211_assoc_command *command)
{
	kern_memset(command, 0, sizeof(*command));
	command->step = state->step;
	command->sequence = state->active_sequence;
	command->common_generation = state->common_generation;
	command->hardware_epoch = state->hardware_epoch;
	command->deadline = state->deadline;
}

/* Encodes the interface command for action. */
static void
ax211_assoc_mac_config_encode(
	const struct intel_ax211_assoc_state *state,
	uint32_t action,
	int associated,
	struct intel_ax211_assoc_command *command)
{
	uint32_t filter;

	/* Builds the MAC configuration command. */
	command->header = INTEL_AX211_ASSOC_HEADER_WIDE;
	command->group = INTEL_AX211_ASSOC_FAMILY_LINK_SETUP;
	command->opcode = INTEL_AX211_ASSOC_INTERFACE_SETUP_OPCODE;
	command->wire_version = 0U;
	command->layout_version = INTEL_AX211_ASSOC_INTERFACE_SETUP_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_INTERFACE_SETUP_SIZE;
	ax211_assoc_put_le32(command->payload, INTEL_AX211_ASSOC_INTERFACE_SLOT);
	ax211_assoc_put_le32(command->payload + 4U, action);

	/* A removal names only the interface. */
	if (action == AX211_ASSOC_ACTION_REMOVE)
		return;

	/* Describes the client interface. */
	ax211_assoc_put_le32(command->payload + 8U,
			     AX211_ASSOC_INTERFACE_KIND_CLIENT);
	kern_memcpy(command->payload + 12U, state->profile.station_address, 6U);

	/*
	 * The common WLAN liveness contract is refreshed by delivered beacons.
	 * Keep them visible after association until a firmware missed-beacon
	 * notification is explicitly adapted to that contract.
	 */
	filter = AX211_ASSOC_DELIVER_GROUP_FRAMES |
		 AX211_ASSOC_DELIVER_BEACONS;
	ax211_assoc_put_le32(command->payload + 20U, filter);
	ax211_assoc_put_le32(command->payload + 32U,
			     AX211_ASSOC_NO_ACK_FOR_HOST);

	/* An associated interface carries its association id. */
	if (associated) {
		command->payload[36U] = 1U;
		ax211_assoc_put_le16(command->payload + 40U,
				     state->update.association_id);
	} else {
		command->payload[36U] = 0U;
	}
}

/* Encodes the link command for action. */
static void
ax211_assoc_link_config_encode(
	const struct intel_ax211_assoc_state *state,
	uint32_t action,
	uint32_t modify_mask,
	uint32_t active,
	uint32_t phy_id,
	struct intel_ax211_assoc_command *command)
{
	uint32_t qos;
	uint8_t dtim_period;

	/* Builds the link configuration command. */
	command->header = INTEL_AX211_ASSOC_HEADER_WIDE;
	command->group = INTEL_AX211_ASSOC_FAMILY_LINK_SETUP;
	command->opcode = INTEL_AX211_ASSOC_LINK_SETUP_OPCODE;
	command->wire_version = 0U;
	command->layout_version = INTEL_AX211_ASSOC_LINK_SETUP_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_LINK_SETUP_SIZE;
	ax211_assoc_put_le32(command->payload, action);
	ax211_assoc_put_le32(command->payload + 4U, INTEL_AX211_ASSOC_LINK_SLOT);
	ax211_assoc_put_le32(command->payload + 12U, phy_id);

	/* A removal names only the link and the channel context. */
	if (action == AX211_ASSOC_ACTION_REMOVE)
		return;

	/* Binds the link to the interface and the own address. */
	ax211_assoc_put_le32(command->payload + 8U, INTEL_AX211_ASSOC_INTERFACE_SLOT);
	kern_memcpy(command->payload + 16U, state->profile.station_address, 6U);

	/* An addition carries nothing more. */
	if (action == AX211_ASSOC_ACTION_ADD)
		return;

	/* Describes the changed fields, the rates and the slot time. */
	ax211_assoc_put_le32(command->payload + 24U, modify_mask);
	ax211_assoc_put_le32(command->payload + 28U, active);
	ax211_assoc_put_le32(command->payload + 36U,
			     state->profile.cck_ack_rates);
	ax211_assoc_put_le32(command->payload + 40U,
			     state->profile.ofdm_ack_rates);
	ax211_assoc_put_le32(command->payload + 44U,
			     state->profile.short_preamble);
	ax211_assoc_put_le32(command->payload + 48U, state->profile.short_slot);

	/* Describes QoS and the access categories. */
	if (state->profile.qos)
		qos = 1U;
	else
		qos = 0U;
	ax211_assoc_put_le32(command->payload + 56U, qos);
	ax211_assoc_edca_encode(&state->profile, command->payload + 60U);

	/* Describes the beacon interval and the DTIM interval. */
	ax211_assoc_put_le32(command->payload + 136U,
			     state->profile.beacon_interval_tu);
	if (state->update_valid)
		dtim_period = state->update.dtim_period;
	else
		dtim_period = state->profile.dtim_period;
	ax211_assoc_put_le32(command->payload + 140U,
			     (uint32_t)state->profile.beacon_interval_tu *
				     (uint32_t)dtim_period);
}

/* Encodes the four access categories into the link command. */
static void
ax211_assoc_edca_encode(
	const struct intel_ax211_assoc_profile *profile,
	uint8_t *output)
{
	const struct intel_ax211_assoc_edca *edca;
	uint8_t *entry;
	uint16_t cw_min;
	uint16_t cw_max;

	/*
	 * Where each access category goes in the link command.
	 *
	 * The profile lists the categories in the order of the WMM parameter
	 * element (best effort, background, video, voice); the firmware's
	 * array is background, best effort, video, voice, so ac_index swaps
	 * the first two.  fifo[] is the firmware transmit FIFO that serves
	 * each profile entry, written as a one-bit mask: best effort FIFO 2,
	 * background 1, video 3, voice 4.
	 *
	 * Values referenced: Linux v6.17
	 * drivers/net/wireless/intel/iwlwifi/fw/api/mac.h (the access category
	 * order of the firmware and the transmit FIFO numbers of struct
	 * iwl_ac_qos).
	 */
	static const uint8_t ac_index[INTEL_AX211_ASSOC_EDCA_COUNT] = {1U, 0U,
								       2U, 3U};
	static const uint8_t fifo[INTEL_AX211_ASSOC_EDCA_COUNT] = {2U, 1U, 3U,
								   4U};
	size_t index;

	/* Writes each category's window, AIFSN, FIFO and TXOP limit. */
	for (index = 0U; index < INTEL_AX211_ASSOC_EDCA_COUNT; index++) {
		edca = &profile->edca[index];
		entry = output + (size_t)ac_index[index] * 8U;
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

/* Encodes the channel context command for action. */
static void
ax211_assoc_phy_encode(
	const struct intel_ax211_assoc_state *state,
	uint32_t action,
	struct intel_ax211_assoc_command *command)
{
	/* Builds the channel context command. */
	command->header = INTEL_AX211_ASSOC_HEADER_LEGACY;
	command->group = INTEL_AX211_ASSOC_FAMILY_CLASSIC;
	command->opcode = INTEL_AX211_ASSOC_CHANNEL_CONTEXT_OPCODE;
	command->layout_version = INTEL_AX211_ASSOC_CHANNEL_CONTEXT_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_CHANNEL_CONTEXT_SIZE;
	ax211_assoc_put_le32(command->payload + 4U, action);

	/*
	 * ULTRA_HB_CHANNELS selects the 32-byte PHY_CONTEXT v4 variant.  Its
	 * CHANNEL_CONFIG v2 stores channel as LE32, followed by
	 * band/width/control; lmac_id at offset 16 remains zero because this
	 * AX211 is not CDB.
	 */
	ax211_assoc_put_le32(command->payload + 8U, state->profile.channel);
	if (state->profile.channel <= 14U)
		command->payload[12U] = AX211_ASSOC_CHANNEL_IN_24GHZ;
	else
		command->payload[12U] = AX211_ASSOC_CHANNEL_IN_5GHZ;
}

/* Encodes the receive chain command. */
static void
ax211_assoc_rlc_encode(
	const struct intel_ax211_assoc_state *state,
	struct intel_ax211_assoc_command *command)
{
	uint32_t chains;

	/* Builds the receive chain command. */
	command->header = INTEL_AX211_ASSOC_HEADER_WIDE;
	command->group = INTEL_AX211_ASSOC_FAMILY_TRAFFIC;
	command->opcode = INTEL_AX211_ASSOC_RECEIVE_CHAINS_OPCODE;

	/*
	 * RLC_CONFIG is the API89 exception which selects v2 in the wide
	 * header.
	 */
	command->wire_version = INTEL_AX211_ASSOC_RECEIVE_CHAINS_VERSION;
	command->layout_version = INTEL_AX211_ASSOC_RECEIVE_CHAINS_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_RECEIVE_CHAINS_SIZE;

	/* Writes the chain mask with bits 10 and 12. */
	chains = ((uint32_t)state->profile.rx_chain_mask << 1) |
		 (UINT32_C(1) << 10) | (UINT32_C(1) << 12);
	ax211_assoc_put_le32(command->payload + 4U, chains);
}

/* Encodes the station command of the access point. */
static void
ax211_assoc_station_config_encode(
	const struct intel_ax211_assoc_state *state,
	int associated,
	struct intel_ax211_assoc_command *command)
{
	/* Builds the station command. */
	command->header = INTEL_AX211_ASSOC_HEADER_WIDE;
	command->group = INTEL_AX211_ASSOC_FAMILY_LINK_SETUP;
	command->opcode = INTEL_AX211_ASSOC_PEER_SETUP_OPCODE;
	command->wire_version = 0U;
	command->layout_version = INTEL_AX211_ASSOC_PEER_SETUP_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_PEER_SETUP_SIZE;
	ax211_assoc_put_le32(command->payload, INTEL_AX211_ASSOC_PEER_SLOT);
	ax211_assoc_put_le32(command->payload + 4U, INTEL_AX211_ASSOC_LINK_SLOT);
	kern_memcpy(command->payload + 8U, state->profile.bssid, 6U);
	kern_memcpy(command->payload + 16U, state->profile.bssid, 6U);
	ax211_assoc_put_le32(command->payload + 24U,
			     AX211_ASSOC_PEER_KIND_ACCESS_POINT);

	/* An associated station carries its association id. */
	if (associated) {
		ax211_assoc_put_le32(command->payload + 28U,
				     state->update.association_id);
	}
}

/* Encodes the management queue command for operation. */
static void
ax211_assoc_queue_encode(
	const struct intel_ax211_assoc_state *state,
	uint32_t operation,
	struct intel_ax211_assoc_command *command)
{
	/* Builds the queue command; only an addition's answer is checked. */
	command->header = INTEL_AX211_ASSOC_HEADER_WIDE;
	command->group = INTEL_AX211_ASSOC_FAMILY_TRAFFIC;
	command->opcode = INTEL_AX211_ASSOC_QUEUE_SETUP_OPCODE;
	command->layout_version = INTEL_AX211_ASSOC_QUEUE_SETUP_VERSION;
	command->response_version = INTEL_AX211_ASSOC_QUEUE_RESPONSE_VERSION;
	if (operation == AX211_ASSOC_QUEUE_OPERATION_ADD)
		command->response_kind = INTEL_AX211_ASSOC_RESPONSE_QUEUE;
	else
		command->response_kind = INTEL_AX211_ASSOC_RESPONSE_IGNORED;
	command->payload_length = INTEL_AX211_ASSOC_QUEUE_SETUP_SIZE;
	ax211_assoc_put_le32(command->payload, operation);
	ax211_assoc_put_le32(command->payload + 4U, 1U);

	/* An addition describes the queue; a removal names its TID. */
	if (operation == AX211_ASSOC_QUEUE_OPERATION_ADD) {
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

/* Stores a 64-bit value little-endian. */
static void
ax211_assoc_put_le64(
	uint8_t *bytes,
	uint64_t value)
{
	ax211_assoc_put_le32(bytes, (uint32_t)value);
	ax211_assoc_put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

/* Encodes the time reservation command for action. */
static void
ax211_assoc_session_encode(
	const struct intel_ax211_assoc_state *state,
	uint32_t action,
	struct intel_ax211_assoc_command *command)
{
	/* Builds the time reservation command. */
	command->header = INTEL_AX211_ASSOC_HEADER_WIDE;
	command->group = INTEL_AX211_ASSOC_FAMILY_LINK_SETUP;
	command->opcode = INTEL_AX211_ASSOC_TIME_RESERVATION_OPCODE;
	command->layout_version = INTEL_AX211_ASSOC_TIME_RESERVATION_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_TIME_RESERVATION_SIZE;
	ax211_assoc_put_le32(command->payload, INTEL_AX211_ASSOC_INTERFACE_SLOT);
	ax211_assoc_put_le32(command->payload + 4U, action);
	ax211_assoc_put_le32(command->payload + 8U,
			     AX211_ASSOC_RESERVATION_FOR_JOIN);

	/* An addition reserves nine beacon intervals. */
	if (action == AX211_ASSOC_ACTION_ADD) {
		ax211_assoc_put_le32(
			command->payload + 12U,
			(uint32_t)state->profile.beacon_interval_tu * 9U);
	}
}

/* Encodes the removal of the access point's station. */
static void
ax211_assoc_station_remove_encode(
	struct intel_ax211_assoc_command *command)
{
	command->header = INTEL_AX211_ASSOC_HEADER_WIDE;
	command->group = INTEL_AX211_ASSOC_FAMILY_LINK_SETUP;
	command->opcode = INTEL_AX211_ASSOC_PEER_REMOVE_OPCODE;
	command->wire_version = 0U;
	command->layout_version = INTEL_AX211_ASSOC_PEER_REMOVE_VERSION;
	command->response_kind = INTEL_AX211_ASSOC_RESPONSE_EMPTY;
	command->payload_length = INTEL_AX211_ASSOC_PEER_REMOVE_SIZE;
	ax211_assoc_put_le32(command->payload, INTEL_AX211_ASSOC_PEER_SLOT);
}

/*
 * Reports whether command is exactly the one the state encodes: 1 when it
 * is, 0 when not.
 */
static int
ax211_assoc_command_matches(
	const struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_command *command)
{
	struct intel_ax211_assoc_command expected;
	int result;

	/* Refuses missing arguments. */
	if (state == NULL || command == NULL)
		return 0;

	/* Encodes the expected command. */
	result = ax211_assoc_command_encode(state, &expected);
	if (result != INTEL_AX211_ASSOC_OK)
		return 0;

	/* Compares the step, the kinds and the command's identity. */
	if (command->step != expected.step ||
	    command->header != expected.header ||
	    command->response_kind != expected.response_kind ||
	    command->group != expected.group ||
	    command->opcode != expected.opcode) {
		return 0;
	}

	/* Compares the versions. */
	if (command->wire_version != expected.wire_version ||
	    command->layout_version != expected.layout_version ||
	    command->response_version != expected.response_version) {
		return 0;
	}

	/* Compares the sequence, the generations and the deadline. */
	if (command->sequence != expected.sequence ||
	    command->common_generation != expected.common_generation ||
	    command->hardware_epoch != expected.hardware_epoch ||
	    command->deadline != expected.deadline) {
		return 0;
	}

	/* Compares the payload's length. */
	if (command->payload_length != expected.payload_length)
		return 0;

	/* Compares the payload. */
	result = kern_memcmp(command->payload, expected.payload,
			     expected.payload_length);
	if (result != 0)
		return 0;

	/* The command matches. */
	return 1;
}

/* Starts removing the firmware objects of the session, newest first. */
static int
ax211_assoc_rollback_begin(
	struct intel_ax211_assoc_state *state,
	int failure,
	uint64_t now_us)
{
	enum intel_ax211_assoc_step step;
	int result;

	/* The rollback already runs. */
	if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK)
		return INTEL_AX211_ASSOC_PENDING;
	state->failure = failure;

	/* Nothing to remove: the session is rolled back. */
	step = ax211_assoc_rollback_step(state->resources);
	if (step == INTEL_AX211_ASSOC_STEP_NONE) {
		state->phase = INTEL_AX211_ASSOC_PHASE_IDLE;
		state->step = INTEL_AX211_ASSOC_STEP_NONE;
		state->active_sequence = 0U;
		state->deadline = 0U;
		return INTEL_AX211_ASSOC_ROLLED_BACK;
	}

	/* Selects the first removal; a failure fails the rollback. */
	state->phase = INTEL_AX211_ASSOC_PHASE_ROLLBACK;
	result = ax211_assoc_set_step(state, step, now_us);
	if (result != INTEL_AX211_ASSOC_OK) {
		state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
		state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	}

	/* Succeeded: the first removal is pending. */
	return INTEL_AX211_ASSOC_PENDING;
}

/*
 * Selects the removal of the newest firmware object in resources, or none.
 */
static enum intel_ax211_assoc_step
ax211_assoc_rollback_step(
	uint32_t resources)
{
	/* The time reservation is removed first. */
	if ((resources & AX211_ASSOC_RESOURCE_SESSION) != 0U)
		return INTEL_AX211_ASSOC_STEP_SESSION_REMOVE;

	/* Then the queue. */
	if ((resources & AX211_ASSOC_RESOURCE_QUEUE) != 0U)
		return INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE;

	/* Then the station. */
	if ((resources & AX211_ASSOC_RESOURCE_STATION) != 0U)
		return INTEL_AX211_ASSOC_STEP_STATION_REMOVE;

	/* Then the link's activity. */
	if ((resources & AX211_ASSOC_RESOURCE_LINK_ACTIVE) != 0U)
		return INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE;

	/* Then the link. */
	if ((resources & AX211_ASSOC_RESOURCE_LINK) != 0U)
		return INTEL_AX211_ASSOC_STEP_LINK_REMOVE;

	/* Then the interface. */
	if ((resources & AX211_ASSOC_RESOURCE_MAC) != 0U)
		return INTEL_AX211_ASSOC_STEP_MAC_REMOVE;

	/* Then the channel context. */
	if ((resources & AX211_ASSOC_RESOURCE_PHY) != 0U)
		return INTEL_AX211_ASSOC_STEP_PHY_REMOVE;

	/* Nothing is left. */
	return INTEL_AX211_ASSOC_STEP_NONE;
}

/* Checks an answer against the kind of answer its command expects. */
static int
ax211_assoc_response_validate(
	const struct intel_ax211_assoc_command *command,
	const struct intel_ax211_assoc_reply *reply)
{
	uint32_t status;
	uint16_t queue;
	uint16_t reserved;

	/* Refuses missing arguments, a long answer and another version. */
	if (command == NULL ||
	    reply == NULL ||
	    reply->payload_length > INTEL_AX211_ASSOC_RESPONSE_MAX ||
	    reply->response_version != command->response_version) {
		return INTEL_AX211_ASSOC_FIRMWARE;
	}

	/* Checks the answer by the kind the command expects. */
	switch (command->response_kind) {
	case INTEL_AX211_ASSOC_RESPONSE_EMPTY:
		/* Refuses an answer with a payload. */
		if (reply->payload_length != 0U)
			return INTEL_AX211_ASSOC_FIRMWARE;

		/* Succeeded: the answer is empty. */
		return INTEL_AX211_ASSOC_OK;
	case INTEL_AX211_ASSOC_RESPONSE_STATUS_ZERO:
		/* Refuses an answer that is not one status word. */
		if (reply->payload_length != 4U)
			return INTEL_AX211_ASSOC_FIRMWARE;

		/* Refuses a nonzero status. */
		status = ax211_assoc_get_le32(reply->payload);
		if (status != 0U)
			return INTEL_AX211_ASSOC_FIRMWARE;

		/* Succeeded: the status is zero. */
		return INTEL_AX211_ASSOC_OK;
	case INTEL_AX211_ASSOC_RESPONSE_QUEUE:
		/* Refuses an answer of another size. */
		if (reply->payload_length !=
		    INTEL_AX211_ASSOC_QUEUE_RESPONSE_SIZE) {
			return INTEL_AX211_ASSOC_FIRMWARE;
		}

		/* Refuses a queue number out of range. */
		queue = ax211_assoc_get_le16(reply->payload);
		if (queue < AX211_ASSOC_QUEUE_MIN ||
		    queue > AX211_ASSOC_QUEUE_MAX) {
			return INTEL_AX211_ASSOC_FIRMWARE;
		}

		/* Refuses a nonzero reserved field. */
		reserved = ax211_assoc_get_le16(reply->payload + 6U);
		if (reserved != 0U)
			return INTEL_AX211_ASSOC_FIRMWARE;

		/*
		 * The response flags are not a completion status and are
		 * ignored here.
		 *
		 * Procedure referenced: Linux v6.17
		 * drivers/net/wireless/intel/iwlwifi/pcie/gen1_2/tx-gen2.c
		 * (iwl_pcie_txq_alloc_response) and OpenBSD 0f464d413c50
		 * sys/dev/pci/if_iwx.c (iwx_enable_txq).
		 */
		return INTEL_AX211_ASSOC_OK;
	case INTEL_AX211_ASSOC_RESPONSE_IGNORED:
		/* Succeeded: the answer is not checked. */
		return INTEL_AX211_ASSOC_OK;
	default:
		/* Refuses an unknown kind. */
		return INTEL_AX211_ASSOC_FIRMWARE;
	}
}

/* Reads a little-endian 16-bit value. */
static uint16_t
ax211_assoc_get_le16(
	const uint8_t *bytes)
{
	/* Assembles the value from its two bytes, least significant first. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/*
 * Records the object a command without a clear answer may have created, so
 * that the rollback removes it.
 */
static void
ax211_assoc_mark_uncertain(
	struct intel_ax211_assoc_state *state,
	enum intel_ax211_assoc_step step)
{
	/* Marks the object the step creates. */
	switch (step) {
	case INTEL_AX211_ASSOC_STEP_MAC_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_MAC;
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_LINK;
		break;
	case INTEL_AX211_ASSOC_STEP_PHY_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_PHY;
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ACTIVATE:
		state->resources |= AX211_ASSOC_RESOURCE_LINK_ACTIVE;
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_STATION;
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE:
		state->resources |= AX211_ASSOC_RESOURCE_QUEUE;
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_PROTECT:
		/* A reservation that already ended needs no removal. */
		if (state->session_ended == 0U)
			state->resources |= AX211_ASSOC_RESOURCE_SESSION;
		break;
	default:
		break;
	}
}

/* Records the object an accepted command created or removed. */
static void
ax211_assoc_mark_success(
	struct intel_ax211_assoc_state *state,
	enum intel_ax211_assoc_step step)
{
	/* Marks or clears the object of the step. */
	switch (step) {
	case INTEL_AX211_ASSOC_STEP_MAC_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_MAC;
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_LINK;
		break;
	case INTEL_AX211_ASSOC_STEP_PHY_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_PHY;
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ACTIVATE:
		state->resources |= AX211_ASSOC_RESOURCE_LINK_ACTIVE;
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_ADD:
		state->resources |= AX211_ASSOC_RESOURCE_STATION;
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE:
		state->resources |= AX211_ASSOC_RESOURCE_QUEUE;
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_PROTECT:
		/* A reservation that already ended needs no removal. */
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
	case INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE:
		state->resources &= ~AX211_ASSOC_RESOURCE_LINK_ACTIVE;
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_REMOVE:
		state->resources &= ~(AX211_ASSOC_RESOURCE_LINK |
				      AX211_ASSOC_RESOURCE_LINK_ACTIVE);
		break;
	case INTEL_AX211_ASSOC_STEP_MAC_REMOVE:
		state->resources &= ~AX211_ASSOC_RESOURCE_MAC;
		break;
	case INTEL_AX211_ASSOC_STEP_PHY_REMOVE:
		state->resources &= ~AX211_ASSOC_RESOURCE_PHY;
		break;
	case INTEL_AX211_ASSOC_STEP_NONE:
	case INTEL_AX211_ASSOC_STEP_RECEIVE_CHAINS:
	case INTEL_AX211_ASSOC_STEP_LINK_ASSIGN:
	case INTEL_AX211_ASSOC_STEP_STATION_UPDATE:
	case INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE:
	case INTEL_AX211_ASSOC_STEP_LINK_ASSOCIATE:
	default:
		break;
	}
}

/*
 * Selects the command after the accepted one, or ends a stage or the
 * rollback.
 */
static int
ax211_assoc_advance(
	struct intel_ax211_assoc_state *state,
	uint64_t now_us)
{
	enum intel_ax211_assoc_step next;
	int result;

	/* Finds the next step of the stage or of the rollback. */
	switch (state->step) {
	case INTEL_AX211_ASSOC_STEP_MAC_ADD:
		next = INTEL_AX211_ASSOC_STEP_LINK_ADD;
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ADD:
		next = INTEL_AX211_ASSOC_STEP_PHY_ADD;
		break;
	case INTEL_AX211_ASSOC_STEP_PHY_ADD:
		next = INTEL_AX211_ASSOC_STEP_RECEIVE_CHAINS;
		break;
	case INTEL_AX211_ASSOC_STEP_RECEIVE_CHAINS:
		next = INTEL_AX211_ASSOC_STEP_LINK_ASSIGN;
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ASSIGN:
		next = INTEL_AX211_ASSOC_STEP_LINK_ACTIVATE;
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ACTIVATE:
		next = INTEL_AX211_ASSOC_STEP_STATION_ADD;
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_ADD:
		next = INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE;
		break;
	case INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE:
		next = INTEL_AX211_ASSOC_STEP_SESSION_PROTECT;
		break;
	case INTEL_AX211_ASSOC_STEP_SESSION_PROTECT:
		/* Succeeded: the authentication stage is done. */
		state->phase = INTEL_AX211_ASSOC_PHASE_AUTH_READY;
		state->step = INTEL_AX211_ASSOC_STEP_NONE;
		state->active_sequence = 0U;
		state->deadline = 0U;
		return INTEL_AX211_ASSOC_AUTH_READY;
	case INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE:
		next = INTEL_AX211_ASSOC_STEP_LINK_ASSOCIATE;
		break;
	case INTEL_AX211_ASSOC_STEP_LINK_ASSOCIATE:
		next = INTEL_AX211_ASSOC_STEP_STATION_UPDATE;
		break;
	case INTEL_AX211_ASSOC_STEP_STATION_UPDATE:
		/* Succeeded: the association is done. */
		state->phase = INTEL_AX211_ASSOC_PHASE_ASSOCIATED;
		state->step = INTEL_AX211_ASSOC_STEP_NONE;
		state->active_sequence = 0U;
		state->deadline = 0U;
		return INTEL_AX211_ASSOC_COMPLETE;
	case INTEL_AX211_ASSOC_STEP_SESSION_REMOVE:
	case INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE:
	case INTEL_AX211_ASSOC_STEP_STATION_REMOVE:
	case INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE:
	case INTEL_AX211_ASSOC_STEP_LINK_REMOVE:
	case INTEL_AX211_ASSOC_STEP_MAC_REMOVE:
	case INTEL_AX211_ASSOC_STEP_PHY_REMOVE:
		/* The rollback ends when nothing is left to remove. */
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
		/* Refuses a state without a command. */
		return INTEL_AX211_ASSOC_OUT_OF_ORDER;
	}

	/* Selects the next command. */
	result = ax211_assoc_set_step(state, next, now_us);
	if (result != INTEL_AX211_ASSOC_OK) {
		/* A failure during the rollback fails the rollback. */
		if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK) {
			state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
			state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
			return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		}

		/* Starts the rollback and reports the failure. */
		(void)ax211_assoc_rollback_begin(state, result, now_us);
		return result;
	}

	/* Succeeded: the next command is pending. */
	return INTEL_AX211_ASSOC_PENDING;
}

/*
 * Starts the rollback after a failed exchange and reports the failure it
 * records.
 */
static int
ax211_assoc_exchange_failure(
	struct intel_ax211_assoc_state *state,
	int result,
	uint64_t now_us)
{
	int failure;

	/* Keeps a known failure; any other becomes an I/O failure. */
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

	/* A failure during the rollback fails the rollback. */
	if (state->phase == INTEL_AX211_ASSOC_PHASE_ROLLBACK) {
		state->phase = INTEL_AX211_ASSOC_PHASE_FAILED;
		state->failure = INTEL_AX211_ASSOC_ROLLBACK_FAILED;
		return INTEL_AX211_ASSOC_ROLLBACK_FAILED;
	}

	/* Starts the rollback and reports the recorded failure. */
	ax211_assoc_mark_uncertain(state, state->step);
	(void)ax211_assoc_rollback_begin(state, failure, now_us);
	return failure;
}
