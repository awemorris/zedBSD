/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private initialization protocol codecs.
 */

#include "intel-ax211-init.h"

static int ax211_init_version_require_absent(const struct intel_ax211_protocol_command_table *table, uint8_t group, uint8_t opcode);
static int ax211_init_version_require(const struct intel_ax211_protocol_command_table *table, uint8_t group, uint8_t opcode, uint8_t command_version, uint8_t notification_version);
static void ax211_init_put_le32(uint8_t *bytes, uint32_t value);
static uint32_t ax211_init_get_le32(const uint8_t *bytes);

/*
 * Checks the command versions the initialization stage depends on.
 *
 * The digest-pinned API 89 image does not advertise g2/c03 or the legacy
 * g0/c04.  The driver sends the former in its fixed version 1 layout and
 * treats the latter as a legacy notification without a version.  Their
 * absence is part of the exact identity of the image rather than permission
 * to infer a version.
 *
 * Procedure referenced: OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
 * (iwx_run_init_mvm_ucode, the version 1 layout of g2/c03, and iwx_rx_pkt,
 * the notification g0/c04).
 */
int
drv_intel_ax211_init_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	int error;
	int result;

	/* Refuses a table that does not describe the pinned API 89 image. */
	result = drv_intel_ax211_protocol_command_table_validate_api89(table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;

	/*
	 * Refuses an image that lists a version for the stage-select command.
	 */
	result = ax211_init_version_require_absent(
		table, INTEL_AX211_INIT_FAMILY_PLATFORM,
		INTEL_AX211_INIT_STAGE_SELECT_OPCODE);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;

	/*
	 * Refuses an image that lists a version for the init-finished
	 * notification.
	 */
	result = ax211_init_version_require_absent(
		table, INTEL_AX211_PROTOCOL_FAMILY_CLASSIC,
		INTEL_AX211_PROTOCOL_INIT_FINISHED_OPCODE);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;

	/* Requires the NVM handoff versions the driver encodes. */
	result = ax211_init_version_require(
		table, INTEL_AX211_PROTOCOL_FAMILY_REGULATORY_STORE,
		INTEL_AX211_PROTOCOL_NVM_HANDOFF_OPCODE,
		INTEL_AX211_INIT_NVM_HANDOFF_COMMAND_VERSION,
		INTEL_AX211_INIT_NVM_HANDOFF_NOTIFICATION_VERSION);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;

	/* Requires the NVM query versions the driver decodes. */
	error = ax211_init_version_require(
		table, INTEL_AX211_PROTOCOL_FAMILY_REGULATORY_STORE,
		INTEL_AX211_PROTOCOL_NVM_QUERY_OPCODE,
		INTEL_AX211_INIT_NVM_QUERY_COMMAND_VERSION,
		INTEL_AX211_PROTOCOL_NVM_QUERY_VERSION);
	if (error != INTEL_AX211_PROTOCOL_OK)
		return error;

	/*
	 * Succeeded: the image lists every initialization command as expected.
	 */
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Encodes the stage-select command for the requested profile.
 */
int
drv_intel_ax211_init_stage_select_encode(
	enum intel_ax211_init_profile profile,
	uint8_t output[INTEL_AX211_INIT_STAGE_SELECT_SIZE])
{
	uint32_t flags;

	/* Refuses a missing output buffer. */
	if (output == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;

	/*
	 * Selects the flags of the profile: none for runtime, STORE_READ to
	 * stop after the NVM.
	 */
	if (profile == INTEL_AX211_INIT_PROFILE_RUNTIME) {
		flags = 0U;
	} else if (profile == INTEL_AX211_INIT_PROFILE_STORE_READ) {
		flags = INTEL_AX211_INIT_STAGE_SELECT_STORE_READ;
	} else {
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	}

	/* Writes the flags as the one word of the payload. */
	ax211_init_put_le32(output, flags);

	/* Succeeded: the payload is ready to send. */
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Decodes a stage-select payload back into its profile.
 */
int
drv_intel_ax211_init_stage_select_decode(
	const uint8_t *bytes,
	size_t length,
	enum intel_ax211_init_profile *profile)
{
	enum intel_ax211_init_profile decoded;
	uint32_t flags;

	/* Refuses missing buffers. */
	if (bytes == NULL || profile == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;

	/* Refuses a payload shorter than the one word. */
	if (length < INTEL_AX211_INIT_STAGE_SELECT_SIZE)
		return INTEL_AX211_PROTOCOL_TRUNCATED;

	/* Refuses a payload longer than the one word. */
	if (length > INTEL_AX211_INIT_STAGE_SELECT_SIZE)
		return INTEL_AX211_PROTOCOL_OVERSIZED;

	/*
	 * Maps the flags back to the profile; any other flag set is not one the
	 * driver sends.
	 */
	flags = ax211_init_get_le32(bytes);
	if (flags == 0U) {
		decoded = INTEL_AX211_INIT_PROFILE_RUNTIME;
	} else if (flags == INTEL_AX211_INIT_STAGE_SELECT_STORE_READ) {
		decoded = INTEL_AX211_INIT_PROFILE_STORE_READ;
	} else {
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	}

	/* Reports the decoded profile. */
	*profile = decoded;

	/* Succeeded. */
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Validates the init-finished notification of the current boot.
 */
int
drv_intel_ax211_init_finished_validate(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation)
{
	int error;

	/* Refuses an unset boot generation. */
	if (generation == 0U)
		return INTEL_AX211_PROTOCOL_INVALID;

	/* Checks the notification against the current generation. */
	error = drv_intel_ax211_protocol_init_finished(message, generation);
	if (error != INTEL_AX211_PROTOCOL_OK)
		return error;

	/* Succeeded: the notification finishes this boot's initialization. */
	return INTEL_AX211_PROTOCOL_OK;
}

/* Requires that the image lists no version for a command. */
static int
ax211_init_version_require_absent(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group,
	uint8_t opcode)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Looks up the command in the image's version table. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, group, opcode, &version);

	/* A listed version would change the layout the driver encodes. */
	if (result == INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;

	/* Reports why the table could not be read. */
	if (result != INTEL_AX211_PROTOCOL_MISSING)
		return result;

	/* Succeeded: the table does not name the command, as expected. */
	return INTEL_AX211_PROTOCOL_OK;
}

/*
 * Requires that the image lists exactly the given command and answer versions.
 */
static int
ax211_init_version_require(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group,
	uint8_t opcode,
	uint8_t command_version,
	uint8_t notification_version)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Looks up the command in the image's version table. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, group, opcode, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;

	/*
	 * Refuses versions other than the ones the driver encodes and decodes.
	 */
	if (version.command_version != command_version ||
	    version.notification_version != notification_version) {
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	}

	/* Succeeded: the image lists the expected versions. */
	return INTEL_AX211_PROTOCOL_OK;
}

/* Stores a 32-bit value in little-endian byte order. */
static void
ax211_init_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Reads a 32-bit little-endian value. */
static uint32_t
ax211_init_get_le32(
	const uint8_t *bytes)
{
	/* Assembles the word from its four bytes, least significant first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}
