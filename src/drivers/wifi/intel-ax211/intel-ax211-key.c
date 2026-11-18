/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 CCMP key implementation.
 */

#include "intel-ax211-key.h"
#include <kern/kcrt.h>

#include <stdint.h>

/*
 * The fields of the key command.
 *
 * The command carries the action at byte 0 (1 adds, 3 removes), the mask of
 * stations the key applies to at byte 4 (bit 0, the access point's slot), the
 * key index at 8 and the flags at 12; for an add, the key itself follows from
 * byte 16 and the receive packet number of the key at byte 64.  The flags
 * select the cipher (2 is CCMP) and, with APPLIES_TO_GROUP (bit 6), mark a
 * group key rather than the pairwise one.  A CCMP packet number is 48 bits.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/datapath.h (struct iwl_sec_key_cmd
 * and enum iwl_sec_key_flags) and IEEE Std 802.11-2020 12.5.3.2 (the CCMP
 * packet number).
 */
#define AX211_KEY_ACTION_ADD		1U
#define AX211_KEY_ACTION_REMOVE		3U
#define AX211_KEY_STATION_MASK		1U
#define AX211_KEY_SUITE_CCMP		2U
#define AX211_KEY_APPLIES_TO_GROUP	0x40U
#define AX211_KEY_PACKET_NUMBER_MAX	UINT64_C(0x0000ffffffffffff)

static int ax211_key_request_valid(const struct intel_ax211_key_request *request);
static void ax211_key_put_le32(uint8_t *bytes, uint32_t value);
static void ax211_key_put_le64(uint8_t *bytes, uint64_t value);
static int ax211_key_state_live(const struct intel_ax211_key_state *state, uint64_t connection_generation, uint32_t hardware_epoch);

/*
 * Checks that the pinned image lists the key command in the layout encoded here.
 */
int
drv_intel_ax211_key_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Refuses a missing command table. */
	if (table == NULL)
		return INTEL_AX211_KEY_INVALID;

	/* Looks up the versions the image lists for the key command. */
	result = drv_intel_ax211_protocol_command_version_lookup(table,
								 INTEL_AX211_KEY_FAMILY,
								 INTEL_AX211_KEY_OPCODE,
								 &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_KEY_UNSUPPORTED;

	/* Refuses an image whose key command or answer layout differs. */
	if (version.command_version != INTEL_AX211_KEY_COMMAND_VERSION ||
	    version.notification_version != INTEL_AX211_KEY_RESPONSE_VERSION) {
		return INTEL_AX211_KEY_UNSUPPORTED;
	}

	/* Succeeded: the image speaks the key command this driver encodes. */
	return INTEL_AX211_KEY_OK;
}

/*
 * Encodes the command that installs a CCMP key.
 */
int
drv_intel_ax211_key_add_encode(
	const struct intel_ax211_key_request *request,
	uint8_t output[INTEL_AX211_KEY_COMMAND_SIZE])
{
	uint32_t flags;
	int valid;

	/* Refuses a malformed request. */
	valid = ax211_key_request_valid(request);
	if (!valid)
		return INTEL_AX211_KEY_INVALID;

	/* Refuses a missing output buffer. */
	if (output == NULL)
		return INTEL_AX211_KEY_INVALID;

	/* Starts from an all-zero command so that unused fields are zero. */
	kern_memset(output, 0, INTEL_AX211_KEY_COMMAND_SIZE);

	/*
	 * Marks a group key so that the firmware applies it to group traffic.
	 */
	flags = AX211_KEY_SUITE_CCMP;
	if (request->kind == INTEL_AX211_KEY_GROUP_KEY)
		flags |= AX211_KEY_APPLIES_TO_GROUP;

	/*
	 * Writes the add action, the station, the key index, the flags and the
	 * key.
	 */
	ax211_key_put_le32(output, AX211_KEY_ACTION_ADD);
	ax211_key_put_le32(output + 4U, AX211_KEY_STATION_MASK);
	ax211_key_put_le32(output + 8U, request->key_index);
	ax211_key_put_le32(output + 12U, flags);
	kern_memcpy(output + 16U, request->key, INTEL_AX211_KEY_BYTES);
	ax211_key_put_le64(output + 64U, request->receive_packet_number);

	/* Succeeded: the add command is ready to send. */
	return INTEL_AX211_KEY_OK;
}

/*
 * Encodes the command that removes a CCMP key.
 */
int
drv_intel_ax211_key_remove_encode(
	uint64_t connection_generation,
	uint64_t key_generation,
	enum intel_ax211_key_kind kind,
	uint8_t key_index,
	uint8_t output[INTEL_AX211_KEY_COMMAND_SIZE])
{
	uint32_t flags;

	/*
	 * Refuses a missing buffer, an unset generation or an impossible key
	 * slot.
	 */
	if (output == NULL ||
	    connection_generation == 0U ||
	    key_generation == 0U ||
	    key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (kind != INTEL_AX211_KEY_PAIRWISE &&
	     kind != INTEL_AX211_KEY_GROUP_KEY) ||
	    (kind == INTEL_AX211_KEY_PAIRWISE && key_index != 0U)) {
		return INTEL_AX211_KEY_INVALID;
	}

	/* Starts from an all-zero command so that unused fields are zero. */
	kern_memset(output, 0, INTEL_AX211_KEY_COMMAND_SIZE);

	/* Marks a group key so that the firmware removes the group key. */
	flags = AX211_KEY_SUITE_CCMP;
	if (kind == INTEL_AX211_KEY_GROUP_KEY)
		flags |= AX211_KEY_APPLIES_TO_GROUP;

	/*
	 * Writes the remove action, the station, the key index and the flags.
	 */
	ax211_key_put_le32(output, AX211_KEY_ACTION_REMOVE);
	ax211_key_put_le32(output + 4U, AX211_KEY_STATION_MASK);
	ax211_key_put_le32(output + 8U, key_index);
	ax211_key_put_le32(output + 12U, flags);

	/* Succeeded: the remove command is ready to send. */
	return INTEL_AX211_KEY_OK;
}

/*
 * Clears a key command so that no key bytes stay in memory.
 */
void
drv_intel_ax211_key_command_scrub(
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE])
{
	volatile uint8_t *bytes;
	size_t index;

	/* Nothing to clear without a command. */
	if (command == NULL)
		return;

	/*
	 * Writes through a volatile pointer so the clearing is not optimized
	 * away.
	 */
	bytes = command;
	for (index = 0U; index < INTEL_AX211_KEY_COMMAND_SIZE; index++)
		bytes[index] = 0U;
}

/*
 * Starts the key bookkeeping of one connection.
 */
int
drv_intel_ax211_key_state_init(
	struct intel_ax211_key_state *state,
	uint32_t hardware_epoch,
	uint64_t connection_generation)
{
	/* Refuses a missing state or an unset epoch or connection. */
	if (state == NULL || hardware_epoch == 0U ||
	    connection_generation == 0U) {
		return INTEL_AX211_KEY_INVALID;
	}

	/* Records the epoch and the connection the keys will belong to. */
	kern_memset(state, 0, sizeof(*state));
	state->hardware_epoch = hardware_epoch;
	state->connection_generation = connection_generation;
	state->initialized = 1U;

	/* Succeeded: the state accepts keys of this connection. */
	return INTEL_AX211_KEY_OK;
}

/*
 * Records a key installed in the firmware that waits to be activated.
 */
int
drv_intel_ax211_key_state_installed(
	struct intel_ax211_key_state *state,
	const struct intel_ax211_key_request *request,
	uint32_t hardware_epoch)
{
	uint64_t *slot;
	int valid;
	int result;

	/* Refuses a malformed request. */
	valid = ax211_key_request_valid(request);
	if (!valid)
		return INTEL_AX211_KEY_INVALID;

	/* Refuses a request of an old connection or firmware epoch. */
	result = ax211_key_state_live(state, request->connection_generation,
				      hardware_epoch);
	if (result != INTEL_AX211_KEY_OK)
		return result;

	/* Picks the staged slot of the key. */
	if (request->kind == INTEL_AX211_KEY_PAIRWISE) {
		/* The pairwise key has one slot. */
		slot = &state->staged_pairwise;
	} else {
		/* A group key has one slot per key index. */
		slot = &state->staged_group[request->key_index];
	}

	/* Ignores a second report of the key already staged in the slot. */
	if (*slot == request->key_generation)
		return INTEL_AX211_KEY_DUPLICATE;

	/* Refuses to overwrite a slot that holds another staged key. */
	if (*slot != 0U)
		return INTEL_AX211_KEY_STALE;

	/* Stages the key in its slot until the keys are activated. */
	*slot = request->key_generation;

	/* Succeeded: the key is staged. */
	return INTEL_AX211_KEY_OK;
}

/*
 * Makes a staged pairwise and group key the active pair.
 */
int
drv_intel_ax211_key_state_activate(
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

	/* Refuses unset key generations. */
	if (pairwise_generation == 0U || group_generation == 0U)
		return INTEL_AX211_KEY_INVALID;

	/* Refuses a request of an old connection or firmware epoch. */
	result = ax211_key_state_live(state, connection_generation,
				      hardware_epoch);
	if (result != INTEL_AX211_KEY_OK)
		return result;

	/* Ignores an activation of the pair that is already active. */
	if (state->active_pairwise == pairwise_generation) {
		/* Looks for the group key among the active group keys. */
		for (group_index = 0U;
		     group_index < INTEL_AX211_KEY_INDEX_LIMIT; group_index++) {
			/* The group key is active too: nothing changes. */
			if (state->active_group[group_index] ==
			    group_generation) {
				return INTEL_AX211_KEY_DUPLICATE;
			}
		}
	}

	/* Finds the pairwise key, staged or already active. */
	pairwise_found = 0;
	if (state->staged_pairwise == pairwise_generation)
		pairwise_found = 1;
	else if (state->active_pairwise == pairwise_generation)
		pairwise_found = 1;

	/* Refuses a pairwise key that was never installed. */
	if (!pairwise_found)
		return INTEL_AX211_KEY_MISSING;

	/* Looks for the group key among the staged group keys. */
	found = 0;
	group_index = 0U;
	while (group_index < INTEL_AX211_KEY_INDEX_LIMIT && !found) {
		/* Stops at the matching slot, or moves to the next one. */
		if (state->staged_group[group_index] == group_generation)
			found = 1;
		else
			group_index++;
	}

	/* Falls back to the active group keys when no staged one matches. */
	if (!found) {
		/* Looks for the group key among the active group keys. */
		group_index = 0U;
		while (group_index < INTEL_AX211_KEY_INDEX_LIMIT && !found) {
			/*
			 * Stops at the matching slot, or moves to the next one.
			 */
			if (state->active_group[group_index] ==
			    group_generation) {
				found = 1;
			} else {
				group_index++;
			}
		}
	}

	/* Refuses a group key that was never installed. */
	if (!found)
		return INTEL_AX211_KEY_MISSING;

	/* Makes the pair active and forgets every staged key. */
	kern_memset(state->active_group, 0, sizeof(state->active_group));
	state->active_pairwise = pairwise_generation;
	state->active_group[group_index] = group_generation;
	state->active_group_index = group_index;
	state->staged_pairwise = 0U;
	kern_memset(state->staged_group, 0, sizeof(state->staged_group));

	/* Succeeded: the pair is active. */
	return INTEL_AX211_KEY_OK;
}

/*
 * Forgets a key that was removed from the firmware.
 */
int
drv_intel_ax211_key_state_removed(
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

	/* Refuses an unset generation or an impossible key slot. */
	if (key_generation == 0U ||
	    key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (kind != INTEL_AX211_KEY_PAIRWISE &&
	     kind != INTEL_AX211_KEY_GROUP_KEY) ||
	    (kind == INTEL_AX211_KEY_PAIRWISE && key_index != 0U)) {
		return INTEL_AX211_KEY_INVALID;
	}

	/* Refuses a request of an old connection or firmware epoch. */
	result = ax211_key_state_live(state, connection_generation,
				      hardware_epoch);
	if (result != INTEL_AX211_KEY_OK)
		return result;

	/* Picks the staged and the active slot of the key. */
	if (kind == INTEL_AX211_KEY_PAIRWISE) {
		/* The pairwise key has one slot of each kind. */
		staged = &state->staged_pairwise;
		active = &state->active_pairwise;
	} else {
		/* A group key has one slot of each kind per key index. */
		staged = &state->staged_group[key_index];
		active = &state->active_group[key_index];
	}

	/* Refuses a key that is neither staged nor active. */
	if (*staged != key_generation && *active != key_generation)
		return INTEL_AX211_KEY_MISSING;

	/* Clears the staged slot when it holds the key. */
	if (*staged == key_generation)
		*staged = 0U;

	/* Clears the active slot when it holds the key. */
	if (*active == key_generation)
		*active = 0U;

	/* Succeeded: the key is forgotten. */
	return INTEL_AX211_KEY_OK;
}

/*
 * Reports the generation of the active key that decrypts received frames.
 */
int
drv_intel_ax211_key_state_rx_generation(
	const struct intel_ax211_key_state *state,
	uint64_t connection_generation,
	enum intel_ax211_key_kind kind,
	uint8_t key_index,
	uint32_t hardware_epoch,
	uint64_t *key_generation)
{
	uint64_t generation;
	int result;

	/* Refuses a missing output or an impossible key slot. */
	if (key_generation == NULL ||
	    key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (kind != INTEL_AX211_KEY_PAIRWISE &&
	     kind != INTEL_AX211_KEY_GROUP_KEY) ||
	    (kind == INTEL_AX211_KEY_PAIRWISE && key_index != 0U)) {
		return INTEL_AX211_KEY_INVALID;
	}

	/* Refuses a request of an old connection or firmware epoch. */
	result = ax211_key_state_live(state, connection_generation,
				      hardware_epoch);
	if (result != INTEL_AX211_KEY_OK)
		return result;

	/* Reads the active slot of the key. */
	if (kind == INTEL_AX211_KEY_PAIRWISE) {
		/* The pairwise key has one active slot. */
		generation = state->active_pairwise;
	} else {
		/* A group key has one active slot per key index. */
		generation = state->active_group[key_index];
	}

	/* Refuses a key slot that holds no active key. */
	if (generation == 0U)
		return INTEL_AX211_KEY_MISSING;

	/* Reports the active key's generation. */
	*key_generation = generation;

	/* Succeeded: the active key's generation is reported. */
	return INTEL_AX211_KEY_OK;
}

/*
 * Checks that a frame to send names the active pairwise key and a usable packet number.
 */
int
drv_intel_ax211_key_state_tx_validate(
	const struct intel_ax211_key_state *state,
	uint64_t connection_generation,
	uint64_t key_generation,
	uint8_t key_index,
	uint64_t packet_number,
	uint32_t hardware_epoch)
{
	int result;

	/*
	 * Refuses an unset generation, a group key slot or a packet number out
	 * of range.
	 */
	if (key_generation == 0U ||
	    key_index != 0U ||
	    packet_number == 0U ||
	    packet_number > AX211_KEY_PACKET_NUMBER_MAX) {
		return INTEL_AX211_KEY_INVALID;
	}

	/* Refuses a request of an old connection or firmware epoch. */
	result = ax211_key_state_live(state, connection_generation,
				      hardware_epoch);
	if (result != INTEL_AX211_KEY_OK)
		return result;

	/* Refuses a key that is no longer the active pairwise key. */
	if (state->active_pairwise != key_generation)
		return INTEL_AX211_KEY_STALE;

	/* Succeeded: the frame may be encrypted with the key. */
	return INTEL_AX211_KEY_OK;
}

/*
 * Reports whether a key request names a usable key: 1 when usable, 0 when not.
 */
static int
ax211_key_request_valid(
	const struct intel_ax211_key_request *request)
{
	/*
	 * Rejects an unset generation, a packet number out of range or an
	 * impossible slot.
	 */
	if (request == NULL ||
	    request->connection_generation == 0U ||
	    request->key_generation == 0U ||
	    request->receive_packet_number > AX211_KEY_PACKET_NUMBER_MAX ||
	    request->key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (request->kind != INTEL_AX211_KEY_PAIRWISE &&
	     request->kind != INTEL_AX211_KEY_GROUP_KEY) ||
	    (request->kind == INTEL_AX211_KEY_PAIRWISE &&
	     request->key_index != 0U)) {
		return 0;
	}

	/* The request names a usable key. */
	return 1;
}

/* Stores a 32-bit value in little-endian byte order. */
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

/* Stores a 64-bit value in little-endian byte order. */
static void
ax211_key_put_le64(
	uint8_t *bytes,
	uint64_t value)
{
	unsigned index;

	/* Writes the eight bytes from the least significant up. */
	for (index = 0U; index < 8U; index++)
		bytes[index] = (uint8_t)(value >> (index * 8U));
}

/*
 * Checks that a key operation belongs to the live connection and firmware
 * epoch.
 */
static int
ax211_key_state_live(
	const struct intel_ax211_key_state *state,
	uint64_t connection_generation,
	uint32_t hardware_epoch)
{
	/* Refuses a missing or uninitialized state and unset identifiers. */
	if (state == NULL ||
	    !state->initialized ||
	    hardware_epoch == 0U ||
	    connection_generation == 0U) {
		return INTEL_AX211_KEY_INVALID;
	}

	/* Refuses an operation of an earlier connection or firmware epoch. */
	if (state->hardware_epoch != hardware_epoch ||
	    state->connection_generation != connection_generation) {
		return INTEL_AX211_KEY_STALE;
	}

	/* Succeeded: the operation belongs to the live connection. */
	return INTEL_AX211_KEY_OK;
}
