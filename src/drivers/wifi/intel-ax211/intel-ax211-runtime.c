/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 runtime codecs and state.
 */

#include "intel-ax211-runtime.h"
#include <kern/kcrt.h>

/* One row of the command version table that the runtime start requires. */
struct ax211_runtime_version {
	uint8_t group;
	uint8_t opcode;
	uint8_t command_version;
	uint8_t notification_version;
};

static int ax211_runtime_profile_valid(const struct intel_ax211_runtime_profile *profile);
static int ax211_runtime_bit(const uint32_t *words, size_t count, unsigned int bit);
static int ax211_runtime_version_validate(const struct intel_ax211_protocol_command_table *table, const struct ax211_runtime_version *required);
static int ax211_runtime_step_enabled(enum intel_ax211_runtime_step step, const struct intel_ax211_runtime_profile *profile);
static void ax211_runtime_put_le32(uint8_t *bytes, uint32_t value);
static void ax211_runtime_put_le16(uint8_t *bytes, uint16_t value);
static uint32_t ax211_runtime_get_le32(const uint8_t *bytes);
static uint16_t ax211_runtime_get_le16(const uint8_t *bytes);
static enum intel_ax211_runtime_step ax211_runtime_next_step(enum intel_ax211_runtime_step step, const struct intel_ax211_runtime_profile *profile);

/*
 * The command versions the runtime start depends on, as the pinned firmware
 * lists them.
 *
 * Each row is family, opcode, command version and notification version, for
 * the commands of intel-ax211-runtime.h and the scan setup, scan start and
 * scan cancel commands (1/0x0c v5, 1/0x0d v17, 1/0x0e v1).  The country update
 * answer is version 6.  The latency tolerance row is not checked when the
 * firmware sets the latency itself.
 *
 * Values referenced: linux-firmware 20260410
 * intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode (the command version record,
 * type 48, read by zedBSD) and Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/commands.h (the family and opcode
 * numbers).
 */
static const struct ax211_runtime_version ax211_runtime_versions[] = {
	{0x01U, 0x98U, 1U, 0U},
	{0x01U, 0x9bU, 6U, 0U},
	{0x02U, 0x01U, 2U, 0U},
	{0x01U, 0xeeU, 3U, 0U},
	{0x04U, 0x04U, 1U, 0U},
	{0x01U, 0x77U, 7U, 0U},
	{0x01U, 0xc8U, 1U, 6U},
	{0x01U, 0xd2U, 4U, 0U},
	{0x01U, 0x0cU, 5U, 0U},
	{0x01U, 0x0dU, 17U, 0U},
	{0x01U, 0x0eU, 1U, 0U}};

/*
 * Builds the runtime profile from the firmware image and the NVM answer.
 */
int
drv_intel_ax211_runtime_profile_from_manifest(
	const struct intel_ax211_firmware_manifest *manifest,
	const struct intel_ax211_protocol_nvm *nvm,
	int ltr_enabled,
	struct intel_ax211_runtime_profile *profile)
{
	struct intel_ax211_runtime_profile parsed;
	int valid;

	/*
	 * Refuses missing arguments, an LTR flag other than 0 or 1, and missing
	 * chains.
	 */
	if (manifest == NULL ||
	    nvm == NULL ||
	    profile == NULL ||
	    (ltr_enabled != 0 && ltr_enabled != 1) ||
	    nvm->tx_chain_mask == 0U ||
	    nvm->rx_chain_mask == 0U) {
		return INTEL_AX211_RUNTIME_INVALID;
	}

	/*
	 * Takes the chains, the regulatory mode, the LTR state and the image's
	 * bits.
	 */
	kern_memset(&parsed, 0, sizeof(parsed));
	parsed.tx_chain_mask = nvm->tx_chain_mask;
	parsed.rx_chain_mask = nvm->rx_chain_mask;
	parsed.firmware_regulatory = nvm->firmware_regulatory ? 1U : 0U;
	parsed.ltr_enabled = ltr_enabled ? 1U : 0U;
	kern_memcpy(parsed.api_changes, manifest->api_changes,
	       sizeof(parsed.api_changes));
	kern_memcpy(parsed.capabilities, manifest->capabilities,
	       sizeof(parsed.capabilities));

	/* Refuses an image without the capabilities the runtime depends on. */
	valid = ax211_runtime_profile_valid(&parsed);
	if (!valid)
		return INTEL_AX211_RUNTIME_UNSUPPORTED;

	/* Succeeded: publishes the profile. */
	*profile = parsed;
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Checks that the pinned image lists every runtime command as the runtime
 * encodes it.
 */
int
drv_intel_ax211_runtime_api89_validate(
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_runtime_profile *profile)
{
	struct intel_ax211_protocol_command_version dqa;
	size_t index;
	int valid;
	int skip;
	int result;

	/* Refuses a missing table. */
	if (table == NULL)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Refuses an unusable profile. */
	valid = ax211_runtime_profile_valid(profile);
	if (!valid)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Refuses a table that is not the pinned image's. */
	result = drv_intel_ax211_protocol_command_table_validate_api89(table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_UNSUPPORTED;

	/* Requires every runtime version the image must list. */
	for (index = 0U; index < sizeof(ax211_runtime_versions) /
					 sizeof(ax211_runtime_versions[0]);
	     index++) {
		/*
		 * The latency command is not sent when the firmware sets the
		 * latency itself.
		 */
		skip = 0;
		if (ax211_runtime_versions[index].group ==
			    INTEL_AX211_RUNTIME_FAMILY_CLASSIC_WIDE &&
		    ax211_runtime_versions[index].opcode ==
			    INTEL_AX211_RUNTIME_LATENCY_TOLERANCE_OPCODE) {
			skip = ax211_runtime_bit(
				profile->capabilities, 5U,
				INTEL_AX211_RUNTIME_FEATURE_BIT_SELF_LATENCY);
		}

		/* Skips the version of a command that is not sent. */
		if (skip)
			continue;

		/* Refuses the table at the first version that differs. */
		result = ax211_runtime_version_validate(
			table, &ax211_runtime_versions[index]);
		if (result != INTEL_AX211_RUNTIME_OK)
			return result;
	}

	/*
	 * The pinned image does not advertise dynamic queue allocation and has no
	 * g5/c00 row; an image that lists one is not the pinned image.
	 */
	result = drv_intel_ax211_protocol_command_version_lookup(table, 5U, 0U,
								 &dqa);
	if (result != INTEL_AX211_PROTOCOL_MISSING)
		return INTEL_AX211_RUNTIME_UNSUPPORTED;

	/* Succeeded: the image lists the runtime commands as expected. */
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Encodes the command of one runtime step.
 */
int
drv_intel_ax211_runtime_command_encode(
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_profile *profile,
	struct intel_ax211_runtime_command *command)
{
	struct intel_ax211_runtime_command encoded;
	int valid;

	/* Refuses an unusable profile. */
	valid = ax211_runtime_profile_valid(profile);
	if (!valid)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Refuses a missing output and a step past the end. */
	if (command == NULL || step >= INTEL_AX211_RUNTIME_STEP_DONE)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Refuses a step this profile does not send. */
	valid = ax211_runtime_step_enabled(step, profile);
	if (!valid)
		return INTEL_AX211_RUNTIME_INVALID;

	/*
	 * Sends zero in the wide-header version field, as API 89 accepts.
	 *
	 * Procedure referenced: OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
	 * (iwx_send_cmd).
	 */
	kern_memset(&encoded, 0, sizeof(encoded));
	encoded.wire_version = 0U;
	encoded.response_version = 0U;

	/* Fills the family, opcode, version and payload of the step. */
	switch (step) {
	case INTEL_AX211_RUNTIME_STEP_TRANSMIT_CHAINS:
		/* The transmit chain mask from the NVM answer. */
		encoded.group = INTEL_AX211_RUNTIME_FAMILY_CLASSIC_WIDE;
		encoded.opcode = INTEL_AX211_RUNTIME_TRANSMIT_CHAINS_OPCODE;
		encoded.layout_version = INTEL_AX211_RUNTIME_TRANSMIT_CHAINS_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_TRANSMIT_CHAINS_SIZE;
		ax211_runtime_put_le32(encoded.payload, profile->tx_chain_mask);
		break;
	case INTEL_AX211_RUNTIME_STEP_COEXISTENCE:
		/* The coexistence mode 3, Wi-Fi only. */
		encoded.group = INTEL_AX211_RUNTIME_FAMILY_CLASSIC_WIDE;
		encoded.opcode = INTEL_AX211_RUNTIME_COEXISTENCE_OPCODE;
		encoded.layout_version = INTEL_AX211_RUNTIME_COEXISTENCE_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_COEXISTENCE_SIZE;
		ax211_runtime_put_le32(encoded.payload, 3U);
		break;
	case INTEL_AX211_RUNTIME_STEP_SOC_CONFIG:
		/* The platform flags and the crystal start-up latency. */
		encoded.group = INTEL_AX211_RUNTIME_FAMILY_PLATFORM;
		encoded.opcode = INTEL_AX211_RUNTIME_SOC_CONFIG_OPCODE;
		encoded.layout_version = INTEL_AX211_RUNTIME_SOC_CONFIG_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_SOC_CONFIG_SIZE;
		ax211_runtime_put_le32(encoded.payload,
				       INTEL_AX211_RUNTIME_SOC_CONFIG_FLAGS);
		ax211_runtime_put_le32(
			encoded.payload + 4U,
			INTEL_AX211_RUNTIME_SOC_CONFIG_XTAL_LATENCY);
		break;
	case INTEL_AX211_RUNTIME_STEP_LATENCY_TOLERANCE:
		/* The latency tolerance command, whose first word is 1. */
		encoded.group = INTEL_AX211_RUNTIME_FAMILY_CLASSIC_WIDE;
		encoded.opcode = INTEL_AX211_RUNTIME_LATENCY_TOLERANCE_OPCODE;
		encoded.layout_version = INTEL_AX211_RUNTIME_LATENCY_TOLERANCE_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_LATENCY_TOLERANCE_SIZE;
		ax211_runtime_put_le32(encoded.payload, 1U);
		break;
	case INTEL_AX211_RUNTIME_STEP_TEMP_REPORT:
		/* An empty set of temperature thresholds. */
		encoded.group = INTEL_AX211_RUNTIME_FAMILY_RADIO;
		encoded.opcode = INTEL_AX211_RUNTIME_TEMP_REPORT_OPCODE;
		encoded.layout_version =
			INTEL_AX211_RUNTIME_TEMP_REPORT_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_TEMP_REPORT_SIZE;
		break;
	case INTEL_AX211_RUNTIME_STEP_POWER_POLICY:
		/* The device power policy, all zero. */
		encoded.group = INTEL_AX211_RUNTIME_FAMILY_CLASSIC_WIDE;
		encoded.opcode = INTEL_AX211_RUNTIME_POWER_POLICY_OPCODE;
		encoded.layout_version =
			INTEL_AX211_RUNTIME_POWER_POLICY_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_POWER_POLICY_SIZE;
		break;
	case INTEL_AX211_RUNTIME_STEP_COUNTRY_UPDATE:
		/*
		 * Asks for country "ZZ", the world domain, with source byte
		 * 0x10.
		 */
		encoded.group = INTEL_AX211_RUNTIME_FAMILY_CLASSIC_WIDE;
		encoded.opcode = INTEL_AX211_RUNTIME_COUNTRY_UPDATE_OPCODE;
		encoded.layout_version = INTEL_AX211_RUNTIME_COUNTRY_UPDATE_VERSION;
		encoded.response_version =
			INTEL_AX211_RUNTIME_COUNTRY_RESPONSE_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_COUNTRY_UPDATE_SIZE;
		ax211_runtime_put_le16(encoded.payload,
				       (uint16_t)(('Z' << 8) | 'Z'));
		encoded.payload[2] = 0x10U;
		break;
	case INTEL_AX211_RUNTIME_STEP_SCAN_CONFIG:
		/* The scan setup with the transmit and receive chain masks. */
		encoded.group = INTEL_AX211_RUNTIME_FAMILY_CLASSIC_WIDE;
		encoded.opcode = INTEL_AX211_PROTOCOL_SCAN_SETUP_OPCODE;
		encoded.layout_version = 5U;
		encoded.payload_length = 12U;
		ax211_runtime_put_le32(encoded.payload + 4U,
				       profile->tx_chain_mask);
		ax211_runtime_put_le32(encoded.payload + 8U,
				       profile->rx_chain_mask);
		break;
	case INTEL_AX211_RUNTIME_STEP_BEACON_FILTER:
		/* Beacon filtering all zero, which delivers every beacon. */
		encoded.group = INTEL_AX211_RUNTIME_FAMILY_CLASSIC_WIDE;
		encoded.opcode = INTEL_AX211_RUNTIME_BEACON_FILTER_OPCODE;
		encoded.layout_version =
			INTEL_AX211_RUNTIME_BEACON_FILTER_VERSION;
		encoded.payload_length = INTEL_AX211_RUNTIME_BEACON_FILTER_SIZE;
		break;
	case INTEL_AX211_RUNTIME_STEP_DONE:
	default:
		/* No command follows the last step. */
		return INTEL_AX211_RUNTIME_INVALID;
	}

	/* Succeeded: publishes the command. */
	*command = encoded;
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Decodes the answer to the country update: the country and its channels.
 */
int
drv_intel_ax211_runtime_mcc_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation,
	struct intel_ax211_runtime_mcc *mcc)
{
	struct intel_ax211_runtime_mcc parsed;
	size_t expected;
	size_t index;
	const uint8_t *bytes;

	/* Refuses missing arguments and an unset epoch. */
	if (message == NULL || mcc == NULL || generation == 0U)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Refuses a message of an earlier firmware epoch. */
	if (message->generation != generation)
		return INTEL_AX211_RUNTIME_STALE;

	/* Refuses a message other than the country answer. */
	if (message->group != INTEL_AX211_RUNTIME_FAMILY_CLASSIC_WIDE ||
	    message->opcode != INTEL_AX211_RUNTIME_COUNTRY_UPDATE_OPCODE ||
	    message->version != INTEL_AX211_RUNTIME_COUNTRY_RESPONSE_VERSION) {
		return INTEL_AX211_RUNTIME_UNSUPPORTED;
	}

	/* Reports an answer the firmware marked as failed. */
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_RUNTIME_FAILED;

	/* Refuses a payload shorter than the 20-byte header. */
	if (message->payload == NULL || message->payload_length < 20U)
		return INTEL_AX211_RUNTIME_TRUNCATED;

	/*
	 * Decodes the header.  Versions 4, 5 and 6 share this layout; the
	 * reserved padding carries nothing.
	 */
	bytes = message->payload;
	kern_memset(&parsed, 0, sizeof(parsed));
	parsed.status = ax211_runtime_get_le32(bytes);
	parsed.mcc = ax211_runtime_get_le16(bytes + 4U);
	parsed.capabilities = ax211_runtime_get_le16(bytes + 6U);
	parsed.time = ax211_runtime_get_le16(bytes + 8U);
	parsed.geographic_info = ax211_runtime_get_le16(bytes + 10U);
	parsed.source = bytes[12U];
	parsed.channel_count = ax211_runtime_get_le32(bytes + 16U);

	/* Every documented status still carries a usable channel list. */
	if (parsed.status > INTEL_AX211_RUNTIME_COUNTRY_STATUS_MAX)
		return INTEL_AX211_RUNTIME_FAILED;

	/* Refuses an answer without channels. */
	if (parsed.channel_count == 0U)
		return INTEL_AX211_RUNTIME_UNSUPPORTED;

	/* Refuses more channels than the record holds. */
	if (parsed.channel_count > INTEL_AX211_RUNTIME_COUNTRY_CHANNEL_LIMIT)
		return INTEL_AX211_RUNTIME_OVERSIZED;

	/* Refuses a payload shorter than its channel list. */
	expected = 20U + (size_t)parsed.channel_count * 4U;
	if (message->payload_length < expected)
		return INTEL_AX211_RUNTIME_TRUNCATED;

	/* Refuses a payload longer than its channel list. */
	if (message->payload_length > expected)
		return INTEL_AX211_RUNTIME_OVERSIZED;

	/* Decodes the flag word of every channel. */
	for (index = 0U; index < parsed.channel_count; index++) {
		parsed.channel[index] =
			ax211_runtime_get_le32(bytes + 20U + index * 4U);
	}

	/* Succeeded: publishes the answer. */
	*mcc = parsed;
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Starts the runtime sequence of one firmware epoch at its first step.
 */
int
drv_intel_ax211_runtime_begin(
	struct intel_ax211_runtime_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_runtime_profile *profile,
	uint32_t generation,
	uint64_t now_us)
{
	struct intel_ax211_runtime_state started;
	int result;

	/*
	 * Refuses a missing state, an unset epoch and a deadline that
	 * overflows.
	 */
	if (state == NULL ||
	    generation == 0U ||
	    now_us > UINT64_MAX - INTEL_AX211_RUNTIME_COMMAND_TIMEOUT_US) {
		return INTEL_AX211_RUNTIME_INVALID;
	}

	/*
	 * Refuses an image that does not list the runtime commands as expected.
	 */
	result = drv_intel_ax211_runtime_api89_validate(table, profile);
	if (result != INTEL_AX211_RUNTIME_OK)
		return result;

	/* Starts at the first step with one command timeout. */
	kern_memset(&started, 0, sizeof(started));
	started.profile = *profile;
	started.generation = generation;
	started.deadline = now_us + INTEL_AX211_RUNTIME_COMMAND_TIMEOUT_US;
	started.step = INTEL_AX211_RUNTIME_STEP_TRANSMIT_CHAINS;
	started.active = 1U;

	/* Succeeded: publishes the started sequence. */
	*state = started;
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Encodes the command of the step the sequence is waiting on.
 */
int
drv_intel_ax211_runtime_current(
	const struct intel_ax211_runtime_state *state,
	uint64_t now_us,
	struct intel_ax211_runtime_command *command)
{
	int error;

	/* Refuses a sequence that is not running. */
	if (state == NULL ||
	    command == NULL ||
	    !state->active ||
	    state->terminal ||
	    state->generation == 0U) {
		return INTEL_AX211_RUNTIME_INVALID;
	}

	/* Reports a step whose time is up. */
	if (now_us >= state->deadline)
		return INTEL_AX211_RUNTIME_TIMEOUT;

	/* Encodes the step's command. */
	error = drv_intel_ax211_runtime_command_encode(
		state->step, &state->profile, command);
	if (error != INTEL_AX211_RUNTIME_OK)
		return error;

	/* Succeeded. */
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Takes the answer to one step and moves the sequence to the next step.
 */
int
drv_intel_ax211_runtime_ack(
	struct intel_ax211_runtime_state *state,
	uint32_t generation,
	enum intel_ax211_runtime_step step,
	uint64_t now_us)
{
	enum intel_ax211_runtime_step next;

	/* Refuses a missing state and an unset epoch. */
	if (state == NULL || generation == 0U)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Refuses a step past the end. */
	if ((unsigned int)step >= (unsigned int)INTEL_AX211_RUNTIME_STEP_DONE)
		return INTEL_AX211_RUNTIME_INVALID;

	/* Refuses an answer of an earlier firmware epoch. */
	if (generation != state->generation)
		return INTEL_AX211_RUNTIME_STALE;

	/* An answer after the sequence ended is a repeat. */
	if (state->terminal || !state->active)
		return INTEL_AX211_RUNTIME_DUPLICATE;

	/* Ends the sequence when the step's time is up. */
	if (now_us >= state->deadline) {
		state->active = 0U;
		state->terminal = 1U;
		return INTEL_AX211_RUNTIME_TIMEOUT;
	}

	/*
	 * An answer to an earlier step is a repeat; one to a later step is out
	 * of order.
	 */
	if (step != state->step) {
		/* An earlier step was already answered. */
		if (step < state->step)
			return INTEL_AX211_RUNTIME_DUPLICATE;
		return INTEL_AX211_RUNTIME_OUT_OF_ORDER;
	}

	/* Moves to the next step this profile sends. */
	next = ax211_runtime_next_step(step, &state->profile);
	state->step = next;

	/* Ends the sequence after its last step. */
	if (next == INTEL_AX211_RUNTIME_STEP_DONE) {
		state->active = 0U;
		state->terminal = 1U;
		return INTEL_AX211_RUNTIME_COMPLETE;
	}

	/* Ends the sequence when the next deadline would overflow. */
	if (now_us > UINT64_MAX - INTEL_AX211_RUNTIME_COMMAND_TIMEOUT_US) {
		state->active = 0U;
		state->terminal = 1U;
		return INTEL_AX211_RUNTIME_TIMEOUT;
	}

	/* Gives the next step one command timeout. */
	state->deadline = now_us + INTEL_AX211_RUNTIME_COMMAND_TIMEOUT_US;

	/* Succeeded: the sequence waits on the next step. */
	return INTEL_AX211_RUNTIME_OK;
}

/*
 * Ends the sequence when the step it waits on has run out of time.
 */
int
drv_intel_ax211_runtime_expire(
	struct intel_ax211_runtime_state *state,
	uint64_t now_us)
{
	/* Refuses a missing or unstarted state. */
	if (state == NULL || state->generation == 0U)
		return INTEL_AX211_RUNTIME_INVALID;

	/* A sequence that already ended cannot expire again. */
	if (state->terminal || !state->active)
		return INTEL_AX211_RUNTIME_DUPLICATE;

	/* The step still has time. */
	if (now_us < state->deadline)
		return INTEL_AX211_RUNTIME_OK;

	/* Ends the sequence. */
	state->active = 0U;
	state->terminal = 1U;

	/* Reports the timeout. */
	return INTEL_AX211_RUNTIME_TIMEOUT;
}

/*
 * Reports whether a runtime profile is one the runtime can run: 1 when it is,
 * 0 when not.
 */
static int
ax211_runtime_profile_valid(
	const struct intel_ax211_runtime_profile *profile)
{
	int set;

	/*
	 * Rejects a missing profile, chain masks out of range and flags other
	 * than 0 or 1.
	 */
	if (profile == NULL ||
	    profile->tx_chain_mask == 0U ||
	    profile->rx_chain_mask == 0U ||
	    profile->tx_chain_mask > 0x07U ||
	    profile->rx_chain_mask > 0x07U ||
	    profile->firmware_regulatory > 1U ||
	    profile->ltr_enabled > 1U) {
		return 0;
	}

	/* Rejects an image without the short scan setup command. */
	set = ax211_runtime_bit(profile->api_changes, 4U,
				INTEL_AX211_RUNTIME_API_BIT_SHORT_SCAN_SETUP);
	if (!set)
		return 0;

	/*
	 * Rejects an image without the wider channel records of the scan
	 * request.
	 */
	set = ax211_runtime_bit(profile->api_changes, 4U,
				INTEL_AX211_RUNTIME_API_BIT_WIDE_CHANNEL_RECORDS);
	if (!set)
		return 0;

	/*
	 * Rejects an image that does not put the DS element in probe requests.
	 */
	set = ax211_runtime_bit(profile->capabilities, 5U,
				INTEL_AX211_RUNTIME_FEATURE_BIT_PROBE_DS_ELEMENT);
	if (!set)
		return 0;

	/* Rejects an image that allocates its queues dynamically. */
	set = ax211_runtime_bit(profile->capabilities, 5U,
				INTEL_AX211_RUNTIME_FEATURE_BIT_DYNAMIC_QUEUES);
	if (set)
		return 0;

	/* Rejects an image without the firmware's own thermal stop. */
	set = ax211_runtime_bit(profile->capabilities, 5U,
				INTEL_AX211_RUNTIME_FEATURE_BIT_FIRMWARE_THERMAL_STOP);
	if (!set)
		return 0;

	/* A firmware regulatory domain needs country updates of either kind. */
	if (profile->firmware_regulatory) {
		/*
		 * The country update API, or failing that the country list
		 * feature.
		 */
		set = ax211_runtime_bit(profile->api_changes, 4U,
					INTEL_AX211_RUNTIME_API_BIT_COUNTRY_UPDATE);
		if (!set) {
			set = ax211_runtime_bit(
				profile->capabilities, 5U,
				INTEL_AX211_RUNTIME_FEATURE_BIT_COUNTRY_LIST);
		}

		/* Rejects an image with neither. */
		if (!set)
			return 0;

		/*
		 * Rejects an image whose country updates do not include
		 * 802.11ax.
		 */
		set = ax211_runtime_bit(profile->capabilities, 5U,
					INTEL_AX211_RUNTIME_FEATURE_BIT_COUNTRY_UPDATE_HE);
		if (!set)
			return 0;
	}

	/* The profile is usable. */
	return 1;
}

/*
 * Reports whether a bit is set in a set of words: 1 when it is, 0 when it is
 * not or lies past the last word.
 */
static int
ax211_runtime_bit(
	const uint32_t *words,
	size_t count,
	unsigned int bit)
{
	size_t word;
	uint32_t mask;

	/* A bit past the last word, or of a missing set, is not set. */
	word = bit / 32U;
	if (words == NULL || word >= count)
		return 0;

	/* Bit n is bit n % 32 of word n / 32. */
	mask = UINT32_C(1) << (bit % 32U);
	if ((words[word] & mask) == 0U)
		return 0;

	/* The bit is set. */
	return 1;
}

/* Requires that a table lists one runtime command with the given versions. */
static int
ax211_runtime_version_validate(
	const struct intel_ax211_protocol_command_table *table,
	const struct ax211_runtime_version *required)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Refuses a table that does not list the command. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, required->group, required->opcode, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_UNSUPPORTED;

	/* Refuses versions other than the required ones. */
	if (version.command_version != required->command_version ||
	    version.notification_version != required->notification_version) {
		return INTEL_AX211_RUNTIME_UNSUPPORTED;
	}

	/* Succeeded: the table lists the required versions. */
	return INTEL_AX211_RUNTIME_OK;
}

/* Reports whether a profile sends a step: 1 when it does, 0 when not. */
static int
ax211_runtime_step_enabled(
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_profile *profile)
{
	int self_latency;

	/*
	 * The latency command is sent only with LTR on and when the firmware
	 * does not set it.
	 */
	if (step == INTEL_AX211_RUNTIME_STEP_LATENCY_TOLERANCE) {
		/* Without LTR the command is not sent. */
		if (!profile->ltr_enabled)
			return 0;

		/* A firmware that sets the latency itself needs no command. */
		self_latency = ax211_runtime_bit(
			profile->capabilities, 5U,
			INTEL_AX211_RUNTIME_FEATURE_BIT_SELF_LATENCY);
		if (self_latency)
			return 0;

		/* The command is sent. */
		return 1;
	}

	/* The country update is sent only with a firmware regulatory domain. */
	if (step == INTEL_AX211_RUNTIME_STEP_COUNTRY_UPDATE)
		return profile->firmware_regulatory;

	/* A step past the end is not sent. */
	if (step >= INTEL_AX211_RUNTIME_STEP_DONE)
		return 0;

	/* Every other step is sent. */
	return 1;
}

/* Stores a 32-bit value in little-endian byte order. */
static void
ax211_runtime_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Stores a 16-bit value in little-endian byte order. */
static void
ax211_runtime_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Reads a 32-bit little-endian value. */
static uint32_t
ax211_runtime_get_le32(
	const uint8_t *bytes)
{
	/* Assembles the word from its four bytes, least significant first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Reads a 16-bit little-endian value. */
static uint16_t
ax211_runtime_get_le16(
	const uint8_t *bytes)
{
	/* Assembles the value from its two bytes, least significant first. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* Finds the step after a step that this profile sends. */
static enum intel_ax211_runtime_step
ax211_runtime_next_step(
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_profile *profile)
{
	unsigned int next;
	int enabled;

	/*
	 * Skips the steps the profile does not send; the end stops the search.
	 */
	next = (unsigned int)step + 1U;
	while (next < (unsigned int)INTEL_AX211_RUNTIME_STEP_DONE) {
		/* Stops at a step that is sent. */
		enabled = ax211_runtime_step_enabled(
			(enum intel_ax211_runtime_step)next, profile);
		if (enabled)
			break;

		/* Tries the following step. */
		next++;
	}

	/* Reports the step found, or the end. */
	return (enum intel_ax211_runtime_step)next;
}
