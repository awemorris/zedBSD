/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 passive-scan codecs and state.
 */

#include "intel-ax211-scan.h"
#include <kern/kcrt.h>

/*
 * The layout of the scan request (version 17), as byte offsets.
 *
 * drv_intel_ax211_scan_request_encode() fills it as follows:
 * - bytes 0 and 4: scan id and out-of-channel priority;
 * - the general section from byte 8: flags (16 bits) at +0, the two probing
 *   stays at +4 and +5, the adaptive default stays for 2.4 GHz, 5 GHz and
 *   the social channels at +6, +7 and +8 (2, 8 and 10), the adaptive budget
 *   at +10, the scan priority at +28 and the two listening stays at +32 and
 *   +33;
 * - the channel section from byte 44: its flags, the channel count, and the
 *   two access-point count overrides (10 and 2) at +2 and +3;
 * - one 8-byte entry per channel from byte 48: a 32-bit flag word whose bit
 *   30 holds the band (1 for 2.4 GHz, 0 for 5 GHz), the channel number at
 *   +4, the power spectral density limit 0x80 (none known) at +5 and the
 *   repetition count 1 at +6;
 * - the schedule from byte 584, of which the first plan runs once (its
 *   repetition count at +2 is 1);
 * - the probe template from byte 596.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/scan.h (struct
 * iwl_scan_req_umac_v17, struct iwl_scan_general_params_v11, struct
 * iwl_scan_channel_params_v7, struct iwl_scan_channel_cfg_umac, struct
 * iwl_scan_periodic_parms_v1 and struct iwl_scan_probe_params_v4).
 */
#define AX211_SCAN_GENERAL_OFFSET 8U
#define AX211_SCAN_CHANNEL_OFFSET 44U
#define AX211_SCAN_CHANNEL_CONFIG_OFFSET 48U
#define AX211_SCAN_CHANNEL_CONFIG_SIZE 8U
#define AX211_SCAN_PERIODIC_OFFSET 584U
#define AX211_SCAN_PROBE_OFFSET 596U
/*
 * The flag values of the scan request and the outcome codes of its
 * completion.
 *
 * The general flags set are REPORT_ALL (bit 1: pass every beacon and probe
 * response to the host, not only matches), REPORT_EACH_PASS (bit 2: send the
 * pass-finished notice), ADAPTIVE_STAY (bit 7: use the adaptive stay times of
 * the general section) and LISTEN_ONLY (bit 11: scan every channel passively).
 * CHANNELS_IN_GIVEN_ORDER (bit 5 of the channel section's flags) makes the
 * firmware visit the channels in the listed order.  A completion reports 1
 * when the scan finished and 2 when it was cancelled; anything else is a
 * failure.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/scan.h (enum
 * iwl_umac_scan_general_flags_v2, enum iwl_scan_channel_flags and the scan
 * completion status).
 */
#define AX211_SCAN_FLAG_REPORT_ALL 0x02U
#define AX211_SCAN_FLAG_REPORT_EACH_PASS 0x04U
#define AX211_SCAN_FLAG_ADAPTIVE_STAY 0x80U
#define AX211_SCAN_FLAG_LISTEN_ONLY 0x0800U
#define AX211_SCAN_CHANNELS_IN_GIVEN_ORDER 0x20U
#define AX211_SCAN_24GHZ_BAND_FLAG 0x40000000U
#define AX211_SCAN_OUTCOME_FINISHED 1U
#define AX211_SCAN_OUTCOME_CANCELLED 2U

static int ax211_scan_station_valid(const uint8_t address[6]);
static void ax211_scan_profile_add(struct intel_ax211_scan_profile *profile, const struct intel_ax211_protocol_channel *candidate, const struct intel_ax211_runtime_mcc *mcc, size_t regulatory_index, int firmware_regulatory);
static int ax211_scan_profile_valid(const struct intel_ax211_scan_profile *profile);
static int ax211_scan_required_version(const struct intel_ax211_protocol_command_table *table, uint8_t opcode, uint8_t command_version);
static void ax211_scan_put_le32(uint8_t *bytes, uint32_t value);
static void ax211_scan_put_le16(uint8_t *bytes, uint16_t value);
static void ax211_scan_probe_encode(const struct intel_ax211_scan_profile *profile, uint8_t *probe);
static int ax211_scan_event_header(const struct intel_ax211_scan_state *state, const struct intel_ax211_protocol_message *message, uint64_t now_us);
static int ax211_scan_complete_decode(const struct intel_ax211_protocol_message *message, struct intel_ax211_scan_event *event);
static uint32_t ax211_scan_get_le32(const uint8_t *bytes);
static int ax211_scan_status_result(uint8_t status);
static int ax211_scan_iteration_decode(const struct intel_ax211_protocol_message *message, struct intel_ax211_scan_event *event);

/*
 * Builds the scan profile from the NVM answer and, under a firmware regulatory
 * domain, the country answer.
 */
int
drv_intel_ax211_scan_profile_from_nvm(
	const struct intel_ax211_protocol_nvm *nvm,
	const struct intel_ax211_runtime_mcc *mcc,
	const uint8_t station_address[6],
	struct intel_ax211_scan_profile *profile)
{
	struct intel_ax211_scan_profile parsed;
	size_t index;
	int valid;

	/*
	 * Refuses missing arguments, a module without 2.4 GHz, channel counts out
	 * of range, and a firmware regulatory domain without a usable country
	 * answer.
	 */
	if (nvm == NULL ||
	    station_address == NULL ||
	    profile == NULL ||
	    !nvm->band_24_enabled ||
	    nvm->channel_24ghz_count >
		    INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT ||
	    nvm->channel_5ghz_count > INTEL_AX211_PROTOCOL_5GHZ_CHANNEL_LIMIT ||
	    (nvm->firmware_regulatory &&
	     (mcc == NULL ||
	      mcc->status > INTEL_AX211_RUNTIME_COUNTRY_STATUS_MAX ||
	      mcc->channel_count == 0U ||
	      mcc->channel_count > INTEL_AX211_RUNTIME_COUNTRY_CHANNEL_LIMIT))) {
		return INTEL_AX211_SCAN_INVALID;
	}

	/* Refuses a station address that cannot send probe requests. */
	valid = ax211_scan_station_valid(station_address);
	if (!valid)
		return INTEL_AX211_SCAN_INVALID;

	/* Lists the usable 2.4 GHz channels. */
	kern_memset(&parsed, 0, sizeof(parsed));
	kern_memcpy(parsed.station_address, station_address, 6U);
	parsed.channel_width_mhz = INTEL_AX211_SCAN_CHANNEL_WIDTH_MHZ;
	for (index = 0U; index < nvm->channel_24ghz_count; index++) {
		ax211_scan_profile_add(&parsed, &nvm->channel_24ghz[index], mcc,
				       index, nvm->firmware_regulatory);
	}

	/* Lists the usable 5 GHz channels when the band is enabled. */
	if (nvm->band_52_enabled) {
		/*
		 * The 5 GHz entries follow the 14 of 2.4 GHz in the country
		 * answer.
		 */
		for (index = 0U; index < nvm->channel_5ghz_count; index++) {
			ax211_scan_profile_add(
				&parsed, &nvm->channel_5ghz[index], mcc,
				INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT +
					index,
				nvm->firmware_regulatory);
		}
	}

	/* Refuses a profile without channels or with an impossible one. */
	valid = ax211_scan_profile_valid(&parsed);
	if (!valid)
		return INTEL_AX211_SCAN_UNSUPPORTED;

	/* Succeeded: publishes the profile. */
	*profile = parsed;
	return INTEL_AX211_SCAN_OK;
}

/*
 * Checks that the pinned image lists the scan commands in the layouts used
 * here.
 */
int
drv_intel_ax211_scan_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	int result;

	/* Refuses a missing table. */
	if (table == NULL)
		return INTEL_AX211_SCAN_INVALID;

	/* Refuses a table that is not the pinned image's. */
	result = drv_intel_ax211_protocol_command_table_validate_api89(table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_SCAN_UNSUPPORTED;

	/* Requires the scan setup version. */
	result = ax211_scan_required_version(
		table, INTEL_AX211_PROTOCOL_SCAN_SETUP_OPCODE,
		INTEL_AX211_SCAN_CONFIG_VERSION);
	if (result != INTEL_AX211_SCAN_OK)
		return result;

	/* Requires the scan request version. */
	result = ax211_scan_required_version(table,
					     INTEL_AX211_SCAN_REQUEST_OPCODE,
					     INTEL_AX211_SCAN_REQUEST_VERSION);
	if (result != INTEL_AX211_SCAN_OK)
		return result;

	/* Requires the scan cancel version. */
	result = ax211_scan_required_version(
		table, INTEL_AX211_SCAN_CANCEL_OPCODE,
		INTEL_AX211_SCAN_CANCEL_VERSION);
	if (result != INTEL_AX211_SCAN_OK)
		return result;

	/* Succeeded: the image lists the scan commands as expected. */
	return INTEL_AX211_SCAN_OK;
}

/*
 * Encodes the scan request of a profile.
 */
int
drv_intel_ax211_scan_request_encode(
	const struct intel_ax211_scan_profile *profile,
	uint8_t output[INTEL_AX211_SCAN_REQUEST_SIZE])
{
	uint8_t *channel;
	uint16_t flags;
	uint32_t band_flag;
	size_t index;
	int valid;

	/* Refuses an unusable profile. */
	valid = ax211_scan_profile_valid(profile);
	if (!valid)
		return INTEL_AX211_SCAN_INVALID;

	/* Refuses a missing output buffer. */
	if (output == NULL)
		return INTEL_AX211_SCAN_INVALID;

	/* Writes the scan id and the out-of-channel priority. */
	kern_memset(output, 0, INTEL_AX211_SCAN_REQUEST_SIZE);
	ax211_scan_put_le32(output, INTEL_AX211_SCAN_UID);
	ax211_scan_put_le32(output + 4U, INTEL_AX211_SCAN_PRECEDENCE);

	/* Writes the general section: flags, stays, budget and priority. */
	flags = AX211_SCAN_FLAG_REPORT_ALL | AX211_SCAN_FLAG_REPORT_EACH_PASS |
		AX211_SCAN_FLAG_ADAPTIVE_STAY | AX211_SCAN_FLAG_LISTEN_ONLY;
	ax211_scan_put_le16(output + AX211_SCAN_GENERAL_OFFSET, flags);
	output[AX211_SCAN_GENERAL_OFFSET + 4U] = INTEL_AX211_SCAN_PROBING_STAY_TU;
	output[AX211_SCAN_GENERAL_OFFSET + 5U] = INTEL_AX211_SCAN_PROBING_STAY_TU;
	output[AX211_SCAN_GENERAL_OFFSET + 6U] = 2U;
	output[AX211_SCAN_GENERAL_OFFSET + 7U] = 8U;
	output[AX211_SCAN_GENERAL_OFFSET + 8U] = 10U;
	ax211_scan_put_le16(output + AX211_SCAN_GENERAL_OFFSET + 10U,
			    INTEL_AX211_SCAN_ADAPTIVE_BUDGET);
	ax211_scan_put_le32(output + AX211_SCAN_GENERAL_OFFSET + 28U,
			    INTEL_AX211_SCAN_PRECEDENCE);
	output[AX211_SCAN_GENERAL_OFFSET + 32U] =
		INTEL_AX211_SCAN_LISTENING_STAY_TU;
	output[AX211_SCAN_GENERAL_OFFSET + 33U] =
		INTEL_AX211_SCAN_LISTENING_STAY_TU;

	/*
	 * Writes the channel section: the order flag, the count and the
	 * overrides.
	 */
	output[AX211_SCAN_CHANNEL_OFFSET] = AX211_SCAN_CHANNELS_IN_GIVEN_ORDER;
	output[AX211_SCAN_CHANNEL_OFFSET + 1U] =
		(uint8_t)profile->channel_count;
	output[AX211_SCAN_CHANNEL_OFFSET + 2U] = 10U;
	output[AX211_SCAN_CHANNEL_OFFSET + 3U] = 2U;

	/* Writes one entry per channel. */
	for (index = 0U; index < profile->channel_count; index++) {
		/* Channels 1 to 14 are 2.4 GHz, the others 5 GHz. */
		channel = output + AX211_SCAN_CHANNEL_CONFIG_OFFSET +
			  index * AX211_SCAN_CHANNEL_CONFIG_SIZE;
		if (profile->channel[index] <= 14U) {
			band_flag = AX211_SCAN_24GHZ_BAND_FLAG;
		} else {
			band_flag = 0U;
		}

		/*
		 * Writes the band, the channel, no known power limit and one
		 * repetition.
		 */
		ax211_scan_put_le32(channel, band_flag);
		channel[4U] = profile->channel[index];
		channel[5U] = 0x80U;
		channel[6U] = 1U;
	}

	/* Runs the first schedule plan once and writes the probe template. */
	output[AX211_SCAN_PERIODIC_OFFSET + 2U] = 1U;
	ax211_scan_probe_encode(profile, output + AX211_SCAN_PROBE_OFFSET);

	/* Succeeded: the request is ready to send. */
	return INTEL_AX211_SCAN_OK;
}

/*
 * Encodes the scan cancel command, one zero word.
 */
int
drv_intel_ax211_scan_abort_encode(
	uint8_t output[INTEL_AX211_SCAN_CANCEL_SIZE])
{
	/* Refuses a missing output buffer. */
	if (output == NULL)
		return INTEL_AX211_SCAN_INVALID;

	/* Writes the zero word. */
	kern_memset(output, 0, INTEL_AX211_SCAN_CANCEL_SIZE);

	/* Succeeded. */
	return INTEL_AX211_SCAN_OK;
}

/*
 * Starts the bookkeeping of one scan, which then waits for its request to be
 * acknowledged.
 */
int
drv_intel_ax211_scan_begin(
	struct intel_ax211_scan_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_scan_profile *profile,
	uint32_t generation,
	uint64_t now_us)
{
	struct intel_ax211_scan_state started;
	size_t index;
	int valid;
	int result;

	/* Refuses a missing state. */
	if (state == NULL)
		return INTEL_AX211_SCAN_INVALID;

	/* Refuses an unusable profile. */
	valid = ax211_scan_profile_valid(profile);
	if (!valid)
		return INTEL_AX211_SCAN_INVALID;

	/* Refuses an unset epoch and a watchdog deadline that overflows. */
	if (generation == 0U ||
	    now_us > UINT64_MAX - INTEL_AX211_SCAN_WATCHDOG_US) {
		return INTEL_AX211_SCAN_INVALID;
	}

	/* Refuses an image that does not list the scan commands as expected. */
	result = drv_intel_ax211_scan_api89_validate(table);
	if (result != INTEL_AX211_SCAN_OK)
		return result;

	/* Sets the acknowledgment and watchdog deadlines. */
	kern_memset(&started, 0, sizeof(started));
	started.generation = generation;
	started.acknowledgement_deadline =
		now_us + INTEL_AX211_SCAN_ACK_TIMEOUT_US;
	started.scan_deadline = now_us + INTEL_AX211_SCAN_WATCHDOG_US;
	started.phase = INTEL_AX211_SCAN_PHASE_WAIT_ACK;

	/* Marks every requested channel in the channel bitmap. */
	for (index = 0U; index < profile->channel_count; index++) {
		started.requested_channels[profile->channel[index] / 8U] |=
			(uint8_t)(UINT8_C(1) << (profile->channel[index] % 8U));
	}

	/* Succeeded: publishes the started scan. */
	*state = started;
	return INTEL_AX211_SCAN_OK;
}

/*
 * Takes the acknowledgment of the scan request and lets the scan run.
 */
int
drv_intel_ax211_scan_request_ack(
	struct intel_ax211_scan_state *state,
	uint32_t generation,
	uint64_t now_us)
{
	/* Refuses a missing state and an unset epoch. */
	if (state == NULL || generation == 0U)
		return INTEL_AX211_SCAN_INVALID;

	/* Refuses an acknowledgment of an earlier firmware epoch. */
	if (generation != state->generation)
		return INTEL_AX211_SCAN_STALE;

	/* An acknowledgment of a running or ended scan is a repeat. */
	if (state->phase == INTEL_AX211_SCAN_PHASE_RUNNING ||
	    state->phase == INTEL_AX211_SCAN_PHASE_TERMINAL) {
		return INTEL_AX211_SCAN_DUPLICATE;
	}

	/* Refuses an acknowledgment when none is awaited. */
	if (state->phase != INTEL_AX211_SCAN_PHASE_WAIT_ACK)
		return INTEL_AX211_SCAN_OUT_OF_ORDER;

	/* Ends the scan when the acknowledgment came too late. */
	if (now_us >= state->acknowledgement_deadline) {
		state->phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		return INTEL_AX211_SCAN_TIMED_OUT;
	}

	/* The scan runs. */
	state->phase = INTEL_AX211_SCAN_PHASE_RUNNING;

	/* Succeeded. */
	return INTEL_AX211_SCAN_OK;
}

/*
 * Takes one scan notification: a pass report or the completion.
 */
int
drv_intel_ax211_scan_event_accept(
	struct intel_ax211_scan_state *state,
	const struct intel_ax211_protocol_message *message,
	uint64_t now_us,
	struct intel_ax211_scan_event *event)
{
	int result;

	/* Refuses missing arguments. */
	if (state == NULL || event == NULL)
		return INTEL_AX211_SCAN_INVALID;

	/*
	 * Ends the scan, and asks for a cancel, when the watchdog has run out.
	 */
	result = ax211_scan_event_header(state, message, now_us);
	if (result == INTEL_AX211_SCAN_TIMED_OUT) {
		state->phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		state->abort_required = 1U;
		return result;
	}

	/* Refuses a notification that does not belong to the running scan. */
	if (result != INTEL_AX211_SCAN_OK)
		return result;

	/*
	 * Decodes the completion or the pass report; others are not scan
	 * events.
	 */
	if (message->opcode == INTEL_AX211_SCAN_FINISHED_OPCODE) {
		result = ax211_scan_complete_decode(message, event);
	} else if (message->opcode == INTEL_AX211_SCAN_PASS_FINISHED_OPCODE) {
		result = ax211_scan_iteration_decode(message, event);
	} else {
		return INTEL_AX211_SCAN_UNSUPPORTED;
	}

	/* A completion, a cancel or a failure ends the scan. */
	if (result == INTEL_AX211_SCAN_COMPLETE ||
	    result == INTEL_AX211_SCAN_ABORTED ||
	    result == INTEL_AX211_SCAN_FAILED)
		state->phase = INTEL_AX211_SCAN_PHASE_TERMINAL;

	/* Reports what the event was. */
	return result;
}

/*
 * Ends a scan whose acknowledgment or watchdog deadline has passed.
 */
int
drv_intel_ax211_scan_expire(
	struct intel_ax211_scan_state *state,
	uint64_t now_us)
{
	/* Refuses a missing or unstarted state. */
	if (state == NULL || state->generation == 0U)
		return INTEL_AX211_SCAN_INVALID;

	/* An ended scan cannot expire again. */
	if (state->phase == INTEL_AX211_SCAN_PHASE_TERMINAL)
		return INTEL_AX211_SCAN_DUPLICATE;

	/*
	 * A scan waiting for its acknowledgment expires at the acknowledgment
	 * deadline.
	 */
	if (state->phase == INTEL_AX211_SCAN_PHASE_WAIT_ACK) {
		/* The acknowledgment still has time. */
		if (now_us < state->acknowledgement_deadline)
			return INTEL_AX211_SCAN_OK;

		/* Ends the scan. */
		state->phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		return INTEL_AX211_SCAN_TIMED_OUT;
	}

	/*
	 * A running scan expires at the watchdog deadline and must be
	 * cancelled.
	 */
	if (state->phase == INTEL_AX211_SCAN_PHASE_RUNNING) {
		/* The scan still has time. */
		if (now_us < state->scan_deadline)
			return INTEL_AX211_SCAN_OK;

		/* Ends the scan and asks for a cancel. */
		state->phase = INTEL_AX211_SCAN_PHASE_TERMINAL;
		state->abort_required = 1U;
		return INTEL_AX211_SCAN_TIMED_OUT;
	}

	/* No other phase has a deadline. */
	return INTEL_AX211_SCAN_OUT_OF_ORDER;
}

/*
 * Reports whether a station address can send probe requests: 1 when it is an
 * individual address that is neither all zeros nor all ones, 0 when not.
 */
static int
ax211_scan_station_valid(
	const uint8_t address[6])
{
	size_t index;
	int all_zero;
	int all_ones;

	/* Rejects a missing address and a group address. */
	if (address == NULL || (address[0] & 1U) != 0U)
		return 0;

	/* Notes whether every byte is zero, or every byte is all ones. */
	all_zero = 1;
	all_ones = 1;
	for (index = 0U; index < 6U; index++) {
		/* A nonzero byte rules out the zero address. */
		if (address[index] != 0U)
			all_zero = 0;

		/* A byte other than 0xff rules out the broadcast address. */
		if (address[index] != 0xffU)
			all_ones = 0;
	}

	/* Rejects the zero address. */
	if (all_zero)
		return 0;

	/* Rejects the broadcast address. */
	if (all_ones)
		return 0;

	/* The address can send probe requests. */
	return 1;
}

/*
 * Adds one channel of the NVM answer to a profile when it may be used.
 *
 * Under a firmware regulatory domain the country answer decides; otherwise
 * the NVM's own usable bit does.
 */
static void
ax211_scan_profile_add(
	struct intel_ax211_scan_profile *profile,
	const struct intel_ax211_protocol_channel *candidate,
	const struct intel_ax211_runtime_mcc *mcc,
	size_t regulatory_index,
	int firmware_regulatory)
{
	/*
	 * Skips a channel without a number, and any channel once the profile is
	 * full.
	 */
	if (candidate->number == 0U ||
	    profile->channel_count >= INTEL_AX211_SCAN_CHANNEL_LIMIT) {
		return;
	}

	/* Skips a channel the regulatory decision does not allow. */
	if (firmware_regulatory) {
		/* The country answer must list the channel as usable. */
		if (mcc == NULL ||
		    regulatory_index >= mcc->channel_count ||
		    (mcc->channel[regulatory_index] &
		     INTEL_AX211_PROTOCOL_CHANNEL_FLAG_USABLE) == 0U) {
			return;
		}
	} else if (!candidate->valid) {
		return;
	}

	/* Adds the channel. */
	profile->channel[profile->channel_count++] = candidate->number;
}

/*
 * Reports whether a scan profile can be sent: 1 when it can, 0 when not.
 */
static int
ax211_scan_profile_valid(
	const struct intel_ax211_scan_profile *profile)
{
	uint8_t channel;
	size_t index, earlier;
	int valid;

	/* Rejects a missing profile. */
	if (profile == NULL)
		return 0;

	/* Rejects a station address that cannot send probe requests. */
	valid = ax211_scan_station_valid(profile->station_address);
	if (!valid)
		return 0;

	/* Rejects another channel width and a channel count out of range. */
	if (profile->channel_width_mhz != INTEL_AX211_SCAN_CHANNEL_WIDTH_MHZ ||
	    profile->channel_count == 0U ||
	    profile->channel_count > INTEL_AX211_SCAN_CHANNEL_LIMIT) {
		return 0;
	}

	/* Checks every channel. */
	for (index = 0U; index < profile->channel_count; index++) {
		/*
		 * Rejects a channel that is not a 20 MHz channel of 2.4 or 5
		 * GHz.
		 */
		channel = profile->channel[index];
		if (!((channel >= 1U && channel <= 14U) ||
		      (channel >= 36U && channel <= 144U &&
		       ((channel - 36U) % 4U) == 0U) ||
		      (channel >= 149U && channel <= 181U &&
		       ((channel - 149U) % 4U) == 0U))) {
			return 0;
		}

		/* Rejects a channel listed twice. */
		for (earlier = 0U; earlier < index; earlier++) {
			/* An earlier entry with the same channel. */
			if (profile->channel[earlier] == channel)
				return 0;
		}
	}

	/* The profile can be sent. */
	return 1;
}

/*
 * Requires that the table lists one scan command of the wide family with the
 * given command version and no notification version.
 */
static int
ax211_scan_required_version(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t opcode,
	uint8_t command_version)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	/* Refuses a command that is missing or listed with other versions. */
	result = drv_intel_ax211_protocol_command_version_lookup(
		table, INTEL_AX211_SCAN_FAMILY_CLASSIC_WIDE, opcode, &version);
	if (result != INTEL_AX211_PROTOCOL_OK ||
	    version.command_version != command_version ||
	    version.notification_version != 0U) {
		return INTEL_AX211_SCAN_UNSUPPORTED;
	}

	/* Succeeded: the table lists the required versions. */
	return INTEL_AX211_SCAN_OK;
}

/* Stores a 32-bit value in little-endian byte order. */
static void
ax211_scan_put_le32(
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
ax211_scan_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Writes the probe request template of the scan request. */
static void
ax211_scan_probe_encode(
	const struct intel_ax211_scan_profile *profile,
	uint8_t *probe)
{
	/* The supported and extended rates, in units of 500 kbit/s. */
	static const uint8_t rates[12] = {2U,  4U,  11U, 22U, 12U, 18U,
					  24U, 36U, 48U, 72U, 96U, 108U};
	uint8_t *frame;
	size_t offset;

	/*
	 * Segment descriptors precede the fixed 512-byte frame area.  This
	 * first profile is forced passive, so firmware never transmits this
	 * frame.  Keep a valid legacy 11g probe body here; a later active-scan
	 * profile must add its negotiated HT/HE information.
	 *
	 * The descriptors give an offset and a length for the MAC header, for
	 * each band part and for the common part.  Here the MAC header part is
	 * at offset 0 with 26 bytes (the 24-byte header and the empty SSID
	 * element), the first band part at offset 26 with 19 bytes (both rate
	 * elements and the DS parameter element), the other band parts are
	 * empty and the common part starts at offset 45 with no bytes.  The
	 * frame has frame control 0x40 (a probe request), broadcast receiver
	 * and BSSID, a zero-length SSID element (element 0), the supported
	 * rates element (1) with the eight rates 1, 2, 5.5, 11, 6, 9, 12 and
	 * 18 Mbit/s, the extended rates element (50) with 24, 36, 48 and 54
	 * Mbit/s, all in units of 500 kbit/s, and the DS parameter element (3)
	 * whose channel byte the firmware fills.
	 *
	 * Values referenced: Linux v6.17
	 * drivers/net/wireless/intel/iwlwifi/fw/api/scan.h (struct
	 * iwl_scan_probe_req) and IEEE Std 802.11-2020 (the probe request
	 * frame and its elements).
	 *
	 * Procedure referenced: OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
	 * (iwx_fill_probe_req, the legacy 11g probe body).
	 */
	ax211_scan_put_le16(probe, 0U);
	ax211_scan_put_le16(probe + 2U, 26U);
	ax211_scan_put_le16(probe + 4U, 26U);
	ax211_scan_put_le16(probe + 6U, 19U);
	ax211_scan_put_le16(probe + 16U, 45U);
	ax211_scan_put_le16(probe + 18U, 0U);

	/* Builds a broadcast probe request with the basic rate elements. */
	frame = probe + 20U;
	frame[0] = 0x40U;
	kern_memset(frame + 4U, 0xff, 6U);
	kern_memcpy(frame + 10U, profile->station_address, 6U);
	kern_memset(frame + 16U, 0xff, 6U);
	frame[24U] = 0U;
	frame[25U] = 0U;
	offset = 26U;
	frame[offset++] = 1U;
	frame[offset++] = 8U;
	kern_memcpy(frame + offset, rates, 8U);
	offset += 8U;
	frame[offset++] = 50U;
	frame[offset++] = 4U;
	kern_memcpy(frame + offset, rates + 8U, 4U);
	offset += 4U;
	frame[offset++] = 3U;
	frame[offset++] = 1U;
	frame[offset] = 0U;
}

/* Checks that a notification belongs to the running scan. */
static int
ax211_scan_event_header(
	const struct intel_ax211_scan_state *state,
	const struct intel_ax211_protocol_message *message,
	uint64_t now_us)
{
	/* Refuses a missing message and an unset epoch. */
	if (message == NULL || message->generation == 0U)
		return INTEL_AX211_SCAN_INVALID;

	/* Refuses a notification of an earlier firmware epoch. */
	if (message->generation != state->generation)
		return INTEL_AX211_SCAN_STALE;

	/* A notification after the scan ended is a repeat. */
	if (state->phase == INTEL_AX211_SCAN_PHASE_TERMINAL)
		return INTEL_AX211_SCAN_DUPLICATE;

	/* Refuses a notification before the request was acknowledged. */
	if (state->phase != INTEL_AX211_SCAN_PHASE_RUNNING)
		return INTEL_AX211_SCAN_OUT_OF_ORDER;

	/* Reports that the watchdog has run out. */
	if (now_us >= state->scan_deadline)
		return INTEL_AX211_SCAN_TIMED_OUT;

	/* Refuses a notification of another family or layout. */
	if (message->group != INTEL_AX211_SCAN_FAMILY_CLASSIC ||
	    message->version != INTEL_AX211_SCAN_NOTIFICATION_VERSION) {
		return INTEL_AX211_SCAN_UNSUPPORTED;
	}

	/* Reports a notification the firmware marked as failed. */
	if ((message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_SCAN_FAILED;

	/* Refuses a missing payload. */
	if (message->payload == NULL)
		return INTEL_AX211_SCAN_INVALID;

	/* Succeeded: the notification belongs to the scan. */
	return INTEL_AX211_SCAN_OK;
}

/* Decodes the completion of a scan. */
static int
ax211_scan_complete_decode(
	const struct intel_ax211_protocol_message *message,
	struct intel_ax211_scan_event *event)
{
	int outcome;
	uint32_t uid;
	const uint8_t *bytes;

	/* Refuses a payload shorter than the completion. */
	if (message->payload_length < 16U)
		return INTEL_AX211_SCAN_TRUNCATED;

	/* Refuses a payload longer than the completion. */
	if (message->payload_length > 16U)
		return INTEL_AX211_SCAN_OVERSIZED;

	/* Refuses the completion of another scan. */
	bytes = message->payload;
	uid = ax211_scan_get_le32(bytes);
	if (uid != INTEL_AX211_SCAN_UID)
		return INTEL_AX211_SCAN_OUT_OF_ORDER;

	/* Decodes the completion. */
	kern_memset(event, 0, sizeof(*event));
	event->kind = INTEL_AX211_SCAN_EVENT_COMPLETE;
	event->final_schedule = bytes[4U];
	event->last_iteration = bytes[5U];
	event->status = bytes[6U];
	event->energy_skip_status = bytes[7U];

	/*
	 * Scheduling, iteration, EBS, elapsed-time, and reserved fields are
	 * firmware reports.  They do not narrow the v1 completion contract.
	 */
	outcome = ax211_scan_status_result(event->status);

	/* Reports how the scan ended. */
	return outcome;
}

/* Reads a 32-bit little-endian value. */
static uint32_t
ax211_scan_get_le32(
	const uint8_t *bytes)
{
	/* Assembles the word from its four bytes, least significant first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Maps the completion's status byte to how the scan ended. */
static int
ax211_scan_status_result(
	uint8_t status)
{
	/* The scan finished. */
	if (status == AX211_SCAN_OUTCOME_FINISHED)
		return INTEL_AX211_SCAN_COMPLETE;

	/* The scan was cancelled. */
	if (status == AX211_SCAN_OUTCOME_CANCELLED)
		return INTEL_AX211_SCAN_ABORTED;

	/* Any other status is a failure. */
	return INTEL_AX211_SCAN_FAILED;
}

/* Decodes the report of one pass over the channel list. */
static int
ax211_scan_iteration_decode(
	const struct intel_ax211_protocol_message *message,
	struct intel_ax211_scan_event *event)
{
	const uint8_t *result;
	const uint8_t *bytes;
	uint32_t uid;
	size_t available;
	size_t count;
	size_t expected;
	size_t index;

	/* Refuses a payload shorter than the report header. */
	if (message->payload_length < 16U)
		return INTEL_AX211_SCAN_TRUNCATED;

	/* Refuses a payload too long or not made of whole channel results. */
	if (message->payload_length >
		    INTEL_AX211_SCAN_ITERATION_NOTIFICATION_SIZE ||
	    (message->payload_length - 16U) % 8U != 0U) {
		return INTEL_AX211_SCAN_OVERSIZED;
	}

	/* Refuses the report of another scan. */
	bytes = message->payload;
	uid = ax211_scan_get_le32(bytes);
	if (uid != INTEL_AX211_SCAN_UID)
		return INTEL_AX211_SCAN_OUT_OF_ORDER;

	/* Refuses more results than the profile has channels. */
	count = bytes[4U];
	if (count > INTEL_AX211_SCAN_CHANNEL_LIMIT)
		return INTEL_AX211_SCAN_OVERSIZED;

	/* Refuses more results than the payload holds. */
	available = (message->payload_length - 16U) / 8U;
	if (count > available)
		return INTEL_AX211_SCAN_TRUNCATED;

	/* Refuses a payload shorter than its results. */
	expected = 16U + count * 8U;
	if (message->payload_length < expected)
		return INTEL_AX211_SCAN_TRUNCATED;

	/* Decodes the header. */
	kern_memset(event, 0, sizeof(*event));
	event->kind = INTEL_AX211_SCAN_EVENT_ITERATION_COMPLETE;
	event->channel_count = count;
	event->status = bytes[5U];
	event->bluetooth_status = bytes[6U];
	event->last_channel = bytes[7U];
	event->tsf = (uint64_t)ax211_scan_get_le32(bytes + 8U) |
		     ((uint64_t)ax211_scan_get_le32(bytes + 12U) << 32);

	/*
	 * Decodes the live results.  Channel, band, and per-probe fields are
	 * informational reports; the UID and bounded result count fence this
	 * notification.
	 */
	for (index = 0U; index < count; index++) {
		result = bytes + 16U + index * 8U;
		event->channel[index].channel = result[0U];
		event->channel[index].probe_result = result[2U];
		event->channel[index].probe_not_sent = result[3U];
		event->channel[index].duration =
			ax211_scan_get_le32(result + 4U);
	}

	/*
	 * Succeeded.  API89 firmware may publish the complete 112-entry result
	 * storage even when scanned_channels says only one entry is live; only
	 * that bounded prefix is decoded.  This is an iteration report, not the
	 * terminal completion, which owns the scan's lifetime.
	 */
	return INTEL_AX211_SCAN_OK;
}
