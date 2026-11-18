/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private firmware protocol codecs.
 */

#include "intel-ax211-protocol.h"
#include <kern/kcrt.h>

/* One row of the command version table that the driver requires. */
struct ax211_required_version {
	uint8_t group;
	uint8_t opcode;
	uint8_t command_version;
	uint8_t notification_version;
};

static void ax211_protocol_version_at(const struct intel_ax211_protocol_command_table *table, size_t index, struct intel_ax211_protocol_command_version *version);
static int ax211_protocol_required_version(const struct intel_ax211_protocol_command_table *table, const struct ax211_required_version *required);
static int ax211_protocol_message_validate(const struct intel_ax211_protocol_message *message, uint8_t group, uint8_t opcode, uint8_t version, uint32_t generation, size_t minimum_payload_length, size_t maximum_payload_length);
static uint16_t ax211_protocol_get_le16(const uint8_t *bytes);
static void ax211_protocol_lmac_decode(const uint8_t *bytes, struct intel_ax211_protocol_lmac_alive *lmac);
static uint32_t ax211_protocol_get_le32(const uint8_t *bytes);
static uint64_t ax211_protocol_get_le64(const uint8_t *bytes);

/*
 * The command versions the boot path depends on, as the pinned firmware
 * lists them.
 *
 * Each row is family, opcode, command layout version and notification layout
 * version: ALIVE (0, 1) has no command and notification version 6; SCAN_SETUP
 * (1, 0x0c) is version 5 and SCAN_START (1, 0x0d) version 17; NVM_HANDOFF (12,
 * 0) is version 1; NVM_QUERY (12, 2) is command version 1 with a version 4
 * answer; PNVM_ACCEPTED (12, 0xfe) has no command and notification version 1.
 * 99 is the image's "no version".  The boot is refused when the image lists
 * any of them differently, because the layouts the driver writes and reads are
 * those versions.
 *
 * Values referenced: linux-firmware 20260410
 * intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode (the command version record,
 * type 48, read by zedBSD) and Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/commands.h and
 * drivers/net/wireless/intel/iwlwifi/fw/api/nvm-reg.h (the family and opcode
 * numbers).
 */
static const struct ax211_required_version ax211_api89_required[] = {
	{0x00U, 0x01U, 99U, 6U},
	{0x01U, 0x0cU, 5U, 0U},
	{0x01U, 0x0dU, 17U, 0U},
	{0x0cU, 0x00U, 1U, 0U},
	{0x0cU, 0x02U, 1U, 4U},
	{0x0cU, 0xfeU, 99U, 1U}};

/*
 * Takes the command version record of an image as a table, without copying it.
 */
int
drv_intel_ax211_protocol_command_table_parse(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_protocol_command_table *table)
{
	struct intel_ax211_protocol_command_table parsed;

	/* Refuses missing buffers. */
	if (bytes == NULL || table == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;

	/* Refuses a record that is empty or not made of whole entries. */
	if (length == 0U ||
	    length % INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE != 0U) {
		return INTEL_AX211_PROTOCOL_TRUNCATED;
	}

	/* Refuses more entries than the pinned image carries. */
	parsed.count = length / INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE;
	if (parsed.count > INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT)
		return INTEL_AX211_PROTOCOL_OVERSIZED;

	/* Publishes the table. */
	parsed.bytes = bytes;
	*table = parsed;

	/* Succeeded. */
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Finds the versions the table lists for one command of one family.
 */
int
drv_intel_ax211_protocol_command_version_lookup(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group,
	uint8_t opcode,
	struct intel_ax211_protocol_command_version *version)
{
	struct intel_ax211_protocol_command_version entry;
	struct intel_ax211_protocol_command_version candidate;
	size_t index;
	int found = 0;

	/* Refuses a missing or impossible table and a missing output. */
	if (table == NULL ||
	    version == NULL ||
	    table->bytes == NULL ||
	    table->count == 0U ||
	    table->count > INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT) {
		return INTEL_AX211_PROTOCOL_INVALID;
	}

	/*
	 * Scans every entry; a command listed twice makes the table ambiguous.
	 */
	kern_memset(&candidate, 0, sizeof(candidate));
	for (index = 0; index < table->count; index++) {
		ax211_protocol_version_at(table, index, &entry);

		/* Takes the entry of the command. */
		if (entry.group == group && entry.opcode == opcode) {
			/* A second entry of the same command is refused. */
			if (found)
				return INTEL_AX211_PROTOCOL_DUPLICATE;
			candidate = entry;
			found = 1;
		}
	}

	/* Reports a command the table does not list. */
	if (!found)
		return INTEL_AX211_PROTOCOL_MISSING;

	/* Succeeded: reports the versions of the command. */
	*version = candidate;
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Checks that a table is the one of the pinned API 89 image.
 */
int
drv_intel_ax211_protocol_command_table_validate_api89(
	const struct intel_ax211_protocol_command_table *table)
{
	int result;
	struct intel_ax211_protocol_command_version sentinel;
	size_t index;

	/* Refuses a missing table. */
	if (table == NULL || table->bytes == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;

	/* Refuses a table of another length. */
	if (table->count != INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;

	/* Reads the last entry, which must be the all-zero end mark. */
	ax211_protocol_version_at(table, table->count - 1U, &sentinel);
	if (sentinel.opcode != 0U ||
	    sentinel.group != 0U ||
	    sentinel.command_version != 0U ||
	    sentinel.notification_version != 0U) {
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	}

	/* Requires every version the boot path depends on. */
	for (index = 0; index < sizeof(ax211_api89_required) /
					sizeof(ax211_api89_required[0]);
	     index++) {
		/* Refuses the table at the first version that differs. */
		result = ax211_protocol_required_version(
			table, &ax211_api89_required[index]);
		if (result != INTEL_AX211_PROTOCOL_OK)
			return result;
	}

	/* Succeeded: the table is the pinned image's. */
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Checks that an answer is the one a pending command waits for.
 */
int
drv_intel_ax211_protocol_command_response_validate(
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending)
{
	int result;

	/* Refuses a missing pending command. */
	if (pending == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;

	/* Refuses a command whose answer layout is unknown. */
	if (pending->response_version == INTEL_AX211_PROTOCOL_UNKNOWN_VERSION)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;

	/* Refuses impossible answer bounds. */
	if (pending->minimum_response_length >
		    pending->maximum_response_length ||
	    pending->maximum_response_length >
		    INTEL_AX211_PROTOCOL_COMMAND_RESPONSE_MAX) {
		return INTEL_AX211_PROTOCOL_INVALID;
	}

	/*
	 * Checks the family, opcode, version, epoch and length of the answer.
	 */
	result = ax211_protocol_message_validate(
		message, pending->group, pending->opcode,
		pending->response_version, pending->generation,
		pending->minimum_response_length,
		pending->maximum_response_length);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;

	/* Refuses an answer from another ring slot. */
	if (message->queue != pending->queue ||
	    message->index != pending->index) {
		return INTEL_AX211_PROTOCOL_TOKEN_MISMATCH;
	}

	/* Succeeded: the answer belongs to the command. */
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Decodes the ALIVE notification of a firmware start.
 */
int
drv_intel_ax211_protocol_alive_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation,
	struct intel_ax211_protocol_alive *alive)
{
	struct intel_ax211_protocol_alive decoded;
	const uint8_t *bytes;
	int result;

	/* Refuses a missing output. */
	if (alive == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;

	/* Checks the notification's identity and exact size. */
	result = ax211_protocol_message_validate(
		message, INTEL_AX211_PROTOCOL_FAMILY_CLASSIC,
		INTEL_AX211_PROTOCOL_ALIVE_OPCODE,
		INTEL_AX211_PROTOCOL_ALIVE_VERSION, generation,
		INTEL_AX211_PROTOCOL_ALIVE_SIZE,
		INTEL_AX211_PROTOCOL_ALIVE_SIZE);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;

	/* Decodes every field of the notification. */
	bytes = message->payload;
	kern_memset(&decoded, 0, sizeof(decoded));
	decoded.status = ax211_protocol_get_le16(bytes);
	decoded.flags = ax211_protocol_get_le16(bytes + 2U);
	ax211_protocol_lmac_decode(bytes + 4U, &decoded.lmac[0]);
	ax211_protocol_lmac_decode(bytes + 52U, &decoded.lmac[1]);
	decoded.umac.major = ax211_protocol_get_le32(bytes + 100U);
	decoded.umac.minor = ax211_protocol_get_le32(bytes + 104U);
	decoded.umac.upper_error_log_address = ax211_protocol_get_le32(bytes + 108U);
	decoded.umac.trace_buffer_address = ax211_protocol_get_le32(bytes + 112U);
	decoded.sku[0] = ax211_protocol_get_le32(bytes + 116U);
	decoded.sku[1] = ax211_protocol_get_le32(bytes + 120U);
	decoded.sku[2] = ax211_protocol_get_le32(bytes + 124U);
	decoded.restore_region_base = ax211_protocol_get_le64(bytes + 128U);
	decoded.restore_region_size = ax211_protocol_get_le32(bytes + 136U);
	decoded.restore_region_enabled = ax211_protocol_get_le32(bytes + 140U);

	/* Refuses a firmware that reports an error status. */
	if (decoded.status != INTEL_AX211_PROTOCOL_ALIVE_HEALTHY)
		return INTEL_AX211_PROTOCOL_FAILED;

	/* Refuses a notification without a board signature. */
	if ((decoded.sku[0] | decoded.sku[1] | decoded.sku[2]) == 0U)
		return INTEL_AX211_PROTOCOL_MISSING;

	/*
	 * Refuses a firmware that asks for a restore region: the driver has
	 * none.
	 */
	if (decoded.restore_region_enabled != 0U) {
		/*
		 * A restore region must be inside the address space even to be
		 * refused as unsupported.
		 */
	if (decoded.restore_region_base == 0U ||
	    decoded.restore_region_size == 0U ||
	    UINT64_MAX - decoded.restore_region_base <
		    decoded.restore_region_size) {
			return INTEL_AX211_PROTOCOL_INVALID;
		}

		/* Refuses the restore region the driver cannot provide. */
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	}

	/* Refuses a restore region that is described but not enabled. */
	if (decoded.restore_region_base != 0U ||
	    decoded.restore_region_size != 0U) {
		return INTEL_AX211_PROTOCOL_INVALID;
	}

	/* Succeeded: publishes the decoded notification. */
	*alive = decoded;
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Validates the notification that the firmware accepted the PNVM sections.
 */
int
drv_intel_ax211_protocol_pnvm_accepted(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation)
{
	int error;

	/* Checks the notification's identity and exact size. */
	error = ax211_protocol_message_validate(
		message, INTEL_AX211_PROTOCOL_FAMILY_REGULATORY_STORE,
		INTEL_AX211_PROTOCOL_PNVM_ACCEPTED_OPCODE,
		INTEL_AX211_PROTOCOL_PNVM_ACCEPTED_VERSION, generation,
		INTEL_AX211_PROTOCOL_PNVM_ACCEPTED_SIZE,
		INTEL_AX211_PROTOCOL_PNVM_ACCEPTED_SIZE);
	if (error != INTEL_AX211_PROTOCOL_OK)
		return error;

	/* Succeeded. */
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Validates the notification that ends the initialization stage.
 */
int
drv_intel_ax211_protocol_init_finished(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation)
{
	int error;

	/*
	 * Checks the notification's identity and exact size; it has no version.
	 */
	error = ax211_protocol_message_validate(
		message, INTEL_AX211_PROTOCOL_FAMILY_CLASSIC,
		INTEL_AX211_PROTOCOL_INIT_FINISHED_OPCODE,
		INTEL_AX211_PROTOCOL_UNKNOWN_VERSION, generation,
		INTEL_AX211_PROTOCOL_INIT_FINISHED_SIZE,
		INTEL_AX211_PROTOCOL_INIT_FINISHED_SIZE);
	if (error != INTEL_AX211_PROTOCOL_OK)
		return error;

	/* Succeeded. */
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Decodes the answer to the NVM query: bands, chains and channel flags.
 */
int
drv_intel_ax211_protocol_nvm_query_decode(
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending,
	struct intel_ax211_protocol_nvm *nvm)
{
	struct intel_ax211_protocol_channel *channel_24;
	struct intel_ax211_protocol_channel *channel_5;

	/*
	 * The 5 GHz channel numbers in the order the firmware lists their flag
	 * words after the 14 channels of 2.4 GHz.
	 *
	 * Values referenced: IEEE Std 802.11-2020 Annex E (the channel numbers)
	 * and Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-nvm-parse.c
	 * (the order of the channels).
	 */
	static const uint8_t
		channel_5ghz[INTEL_AX211_PROTOCOL_5GHZ_CHANNEL_LIMIT] = {
			36U,  40U,  44U,  48U,	52U,  56U,  60U,  64U,
			68U,  72U,  76U,  80U,	84U,  88U,  92U,  96U,
			100U, 104U, 108U, 112U, 116U, 120U, 124U, 128U,
			132U, 136U, 140U, 144U, 149U, 153U, 157U, 161U,
			165U, 169U, 173U, 177U, 181U};
	struct intel_ax211_protocol_nvm decoded;
	const uint8_t *bytes;
	uint32_t tx_chains, rx_chains;
	size_t index, channel_count, channel_offset;
	int result;

	/* Refuses missing arguments. */
	if (pending == NULL || nvm == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;

	/* Refuses a pending command other than the NVM query. */
	if (pending->group != INTEL_AX211_PROTOCOL_FAMILY_REGULATORY_STORE ||
	    pending->opcode != INTEL_AX211_PROTOCOL_NVM_QUERY_OPCODE ||
	    pending->response_version !=
		    INTEL_AX211_PROTOCOL_NVM_QUERY_VERSION ||
	    pending->minimum_response_length !=
		    INTEL_AX211_PROTOCOL_NVM_QUERY_SIZE ||
	    pending->maximum_response_length !=
		    INTEL_AX211_PROTOCOL_NVM_QUERY_SIZE) {
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	}

	/* Checks that the answer belongs to the command. */
	result = drv_intel_ax211_protocol_command_response_validate(message,
								    pending);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;

	/* Decodes the fixed part of the answer. */
	bytes = message->payload;
	kern_memset(&decoded, 0, sizeof(decoded));
	decoded.nvm_flags = ax211_protocol_get_le32(bytes);
	decoded.nvm_revision = ax211_protocol_get_le16(bytes + 4U);
	decoded.board_kind = bytes[6];
	decoded.hardware_address_count = bytes[7];
	decoded.standard_flags = ax211_protocol_get_le32(bytes + 8U);
	tx_chains = ax211_protocol_get_le32(bytes + 12U);
	rx_chains = ax211_protocol_get_le32(bytes + 16U);
	decoded.firmware_regulatory = ax211_protocol_get_le32(bytes + 20U) != 0U;
	decoded.channel_total = ax211_protocol_get_le32(bytes + 24U);

	/* Refuses a radio whose fuses were never programmed. */
	if ((decoded.nvm_flags &
	     INTEL_AX211_PROTOCOL_NVM_FUSES_BLANK) != 0U) {
		return INTEL_AX211_PROTOCOL_FAILED;
	}

	/* Notes the bands and the 802.11 amendments the module may use. */
	decoded.band_24_enabled =
		(decoded.standard_flags &
		 INTEL_AX211_PROTOCOL_NVM_BAND_24_ENABLED) != 0U;
	decoded.band_52_enabled =
		(decoded.standard_flags &
		 INTEL_AX211_PROTOCOL_NVM_BAND_52_ENABLED) != 0U;
	decoded.standard_11n = (decoded.standard_flags &
			      INTEL_AX211_PROTOCOL_NVM_11N_ENABLED) != 0U;
	decoded.standard_11ac = (decoded.standard_flags &
			       INTEL_AX211_PROTOCOL_NVM_11AC_ENABLED) != 0U;
	decoded.standard_11ax = (decoded.standard_flags &
			      INTEL_AX211_PROTOCOL_NVM_11AX_ENABLED) != 0U;
	decoded.mimo_disabled = (decoded.standard_flags &
				 INTEL_AX211_PROTOCOL_NVM_MIMO_DISABLED) != 0U;

	/*
	 * Refuses a module without 2.4 GHz or without transmit and receive
	 * chains.
	 */
	if (!decoded.band_24_enabled || tx_chains == 0U || rx_chains == 0U)
		return INTEL_AX211_PROTOCOL_MISSING;

	/* Refuses chain masks wider than a byte. */
	if (tx_chains > UINT8_MAX || rx_chains > UINT8_MAX)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;

	/* Refuses more channels than the answer can hold. */
	if (decoded.channel_total > INTEL_AX211_PROTOCOL_NVM_CHANNEL_LIMIT)
		return INTEL_AX211_PROTOCOL_OVERSIZED;

	/* Keeps the chain masks. */
	decoded.tx_chain_mask = (uint8_t)tx_chains;
	decoded.rx_chain_mask = (uint8_t)rx_chains;

	/* Decodes the flag words of the 2.4 GHz channels, at most 14. */
	channel_count = decoded.channel_total;
	if (channel_count > INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT)
		channel_count = INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT;
	decoded.channel_24ghz_count = channel_count;
	for (index = 0; index < channel_count; index++) {
		channel_24 = &decoded.channel_24ghz[index];

		/*
		 * Decodes one channel of the 2.4 GHz band; channel n is entry n
		 * - 1.
		 */
		channel_24->number = (uint8_t)(index + 1U);
		channel_24->flags = ax211_protocol_get_le32(
			bytes + 28U + index * sizeof(uint32_t));

		/* A usable channel exists on this module. */
		channel_24->valid = 0;
		if ((channel_24->flags & INTEL_AX211_PROTOCOL_CHANNEL_FLAG_USABLE) != 0U)
			channel_24->valid = 1;

		/*
		 * A usable channel that allows transmitting can be scanned
		 * actively.
		 */
		channel_24->active = 0;
		if (channel_24->valid &&
		    (channel_24->flags &
		     INTEL_AX211_PROTOCOL_CHANNEL_FLAG_MAY_TRANSMIT) != 0U) {
			channel_24->active = 1;
		}

		/* Counts the usable channels. */
		if (channel_24->valid)
			decoded.valid_24ghz_count++;
	}

	/*
	 * Decodes the flag words of the 5 GHz channels, which follow the 14 of
	 * 2.4 GHz.
	 */
	channel_offset = INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT;
	if (decoded.channel_total > channel_offset) {
		/*
		 * The channels past the 2.4 GHz ones belong to 5 GHz and
		 * beyond.
		 */
		channel_count = decoded.channel_total - channel_offset;
	} else {
		/* The answer lists no channel beyond 2.4 GHz. */
		channel_count = 0U;
	}

	/* Decodes at most the 37 channels of 5 GHz; 6 GHz is not decoded. */
	if (channel_count > INTEL_AX211_PROTOCOL_5GHZ_CHANNEL_LIMIT)
		channel_count = INTEL_AX211_PROTOCOL_5GHZ_CHANNEL_LIMIT;
	decoded.channel_5ghz_count = channel_count;
	for (index = 0U; index < channel_count; index++) {
		channel_5 = &decoded.channel_5ghz[index];

		/* Decodes one channel of the 5 GHz band. */
		channel_5->number = channel_5ghz[index];
		channel_5->flags = ax211_protocol_get_le32(
			bytes + 28U +
			(channel_offset + index) * sizeof(uint32_t));

		/*
		 * A usable channel exists on this module, and only while 5 GHz
		 * is enabled.
		 */
		channel_5->valid = 0;
		if (decoded.band_52_enabled &&
		    (channel_5->flags &
		     INTEL_AX211_PROTOCOL_CHANNEL_FLAG_USABLE) != 0U) {
			channel_5->valid = 1;
		}

		/*
		 * A usable channel that allows transmitting can be scanned
		 * actively.
		 */
		channel_5->active = 0;
		if (channel_5->valid &&
		    (channel_5->flags &
		     INTEL_AX211_PROTOCOL_CHANNEL_FLAG_MAY_TRANSMIT) != 0U) {
			channel_5->active = 1;
		}

		/* Counts the usable channels. */
		if (channel_5->valid)
			decoded.valid_5ghz_count++;
	}

	/*
	 * With LAR enabled, MCC is authoritative for regulatory validity.  NVM
	 * still supplies the hardware channel table, but its VALID bit is not a
	 * prerequisite for using a channel accepted by MCC.
	 */
	if (decoded.valid_24ghz_count == 0U && !decoded.firmware_regulatory)
		return INTEL_AX211_PROTOCOL_MISSING;

	/* Succeeded: publishes the decoded answer. */
	*nvm = decoded;
	return INTEL_AX211_PROTOCOL_OK;
}

/* Reads entry index of a command version table. */
static void
ax211_protocol_version_at(
	const struct intel_ax211_protocol_command_table *table,
	size_t index,
	struct intel_ax211_protocol_command_version *version)
{
	const uint8_t *entry;

	/*
	 * Each entry is opcode, family, command version and notification
	 * version.
	 */
	entry = table->bytes +
		index * INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE;
	version->opcode = entry[0];
	version->group = entry[1];
	version->command_version = entry[2];
	version->notification_version = entry[3];
}

/* Requires that a table lists one command with the given versions. */
static int
ax211_protocol_required_version(
	const struct intel_ax211_protocol_command_table *table,
	const struct ax211_required_version *required)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Finds the command in the table. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, required->group, required->opcode, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;

	/* Refuses versions other than the required ones. */
	if (version.command_version != required->command_version ||
	    version.notification_version != required->notification_version) {
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	}

	/* Succeeded: the table lists the required versions. */
	return INTEL_AX211_PROTOCOL_OK;
}

/* Checks the identity, epoch, failure bit and length of a firmware message. */
static int
ax211_protocol_message_validate(
	const struct intel_ax211_protocol_message *message,
	uint8_t group,
	uint8_t opcode,
	uint8_t version,
	uint32_t generation,
	size_t minimum_payload_length,
	size_t maximum_payload_length)
{
	/* Refuses a missing message. */
	if (message == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;

	/* Refuses impossible length bounds. */
	if (minimum_payload_length > maximum_payload_length)
		return INTEL_AX211_PROTOCOL_INVALID;

	/* Refuses a missing payload of nonzero length. */
	if (message->payload_length != 0U && message->payload == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;

	/* Refuses a message of an earlier firmware epoch. */
	if (message->generation != generation)
		return INTEL_AX211_PROTOCOL_STALE;

	/* Refuses a message of another command or layout version. */
	if (message->group != group || message->opcode != opcode ||
	    message->version != version) {
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	}

	/* Reports a command the firmware failed. */
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_PROTOCOL_FAILED;

	/* Refuses a payload shorter than the layout. */
	if (message->payload_length < minimum_payload_length)
		return INTEL_AX211_PROTOCOL_TRUNCATED;

	/* Refuses a payload longer than the layout. */
	if (message->payload_length > maximum_payload_length)
		return INTEL_AX211_PROTOCOL_OVERSIZED;

	/* Succeeded: the message is the expected one. */
	return INTEL_AX211_PROTOCOL_OK;
}

/* Reads a 16-bit little-endian value. */
static uint16_t
ax211_protocol_get_le16(
	const uint8_t *bytes)
{
	/* Assembles the value from its two bytes, least significant first. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* Decodes the record of one lower MAC in the ALIVE notification. */
static void
ax211_protocol_lmac_decode(
	const uint8_t *bytes,
	struct intel_ax211_protocol_lmac_alive *lmac)
{
	lmac->major = ax211_protocol_get_le32(bytes);
	lmac->minor = ax211_protocol_get_le32(bytes + 4U);
	lmac->version_subtype = bytes[8];
	lmac->version_type = bytes[9];
	lmac->mac = bytes[10];
	lmac->option = bytes[11];
	lmac->timestamp = ax211_protocol_get_le32(bytes + 12U);
	lmac->error_log_address = ax211_protocol_get_le32(bytes + 16U);
	lmac->event_log_address = ax211_protocol_get_le32(bytes + 20U);
	lmac->cpu_state_address = ax211_protocol_get_le32(bytes + 24U);
	lmac->trace_setup_address = ax211_protocol_get_le32(bytes + 28U);
	lmac->alive_counter_address = ax211_protocol_get_le32(bytes + 32U);
	lmac->scheduler_sram_base = ax211_protocol_get_le32(bytes + 36U);
	lmac->relay_buffer_address = ax211_protocol_get_le32(bytes + 40U);
	lmac->relay_buffer_size = ax211_protocol_get_le32(bytes + 44U);
}

/* Reads a 32-bit little-endian value. */
static uint32_t
ax211_protocol_get_le32(
	const uint8_t *bytes)
{
	/* Assembles the word from its four bytes, least significant first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Reads a 64-bit little-endian value. */
static uint64_t
ax211_protocol_get_le64(
	const uint8_t *bytes)
{
	uint64_t value = 0;
	unsigned int index;

	/* Assembles the value from its eight bytes, least significant first. */
	for (index = 0; index < 8U; index++)
		value |= (uint64_t)bytes[index] << (index * 8U);

	/* Reports the value. */
	return value;
}
