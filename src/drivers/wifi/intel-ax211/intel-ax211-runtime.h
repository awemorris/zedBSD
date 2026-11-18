/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 runtime contract.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_RUNTIME_H
#define KERN_DRIVERS_INTEL_AX211_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-internal.h"
#include "intel-ax211-protocol.h"

/* zedBSD's bounds: one second per runtime command and its largest payload. */
#define INTEL_AX211_RUNTIME_COMMAND_TIMEOUT_US 1000000U
#define INTEL_AX211_RUNTIME_PAYLOAD_MAX 60U

/*
 * The command families of the runtime start commands.
 *
 * Family 1 is the classic opcode space through the wide header, family 2
 * holds the platform (system) commands and family 4 the radio (PHY)
 * operations.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/commands.h (enum
 * iwl_mvm_command_groups).
 */
#define INTEL_AX211_RUNTIME_FAMILY_CLASSIC_WIDE 1U
#define INTEL_AX211_RUNTIME_FAMILY_PLATFORM 2U
#define INTEL_AX211_RUNTIME_FAMILY_RADIO 4U

/*
 * The commands that bring a freshly started firmware to the state in which
 * it can scan, with their opcodes, layout versions and payload sizes.
 *
 * The versions are those listed in the command version record of the pinned
 * API 89 image.  The runtime sends the commands one at a time and waits for
 * each answer, in the order of enum intel_ax211_runtime_step:
 * - TRANSMIT_CHAINS (1/0x98, v1, 4 bytes): the transmit chain mask from the
 *   NVM answer.
 * - COEXISTENCE (1/0x9b, v6, 8 bytes): the Bluetooth coexistence mode word,
 *   sent as 3 (BT_COEX_WIFI).
 * - SOC_CONFIG (2/0x01, v2, 8 bytes): the platform flags 0x0a and the
 *   crystal start-up latency of 12000 (see below).
 * - LATENCY_TOLERANCE (1/0xee, v3, 32 bytes): sent only when the PCI Express
 *   LTR is enabled and the firmware does not set the latency itself
 *   (FEATURE_BIT_SELF_LATENCY); the first word is 1.
 * - TEMP_REPORT (4/0x04, v1, 20 bytes): an empty set of temperature
 *   thresholds.
 * - POWER_POLICY (1/0x77, v7, 4 bytes): the device power policy, all zero.
 * - COUNTRY_UPDATE (1/0xc8, v1, 28 bytes, answer v6): sent only when the
 *   firmware keeps its own regulatory domain; the driver asks for country
 *   "ZZ" (the world domain) with 0x10 as the source byte, and the answer
 *   lists the permitted channels.
 * - the scan setup of intel-ax211-protocol.h (1/0x0c, v5, 12 bytes): the
 *   transmit and receive chain masks at bytes 4 and 8.
 * - BEACON_FILTER (1/0xd2, v4, 60 bytes): beacon filtering, all zero, which
 *   leaves every beacon delivered to the host.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/commands.h for the opcodes of
 * family 1, drivers/net/wireless/intel/iwlwifi/fw/api/system.h for the
 * platform command, drivers/net/wireless/intel/iwlwifi/fw/api/phy.h for the
 * temperature thresholds, drivers/net/wireless/intel/iwlwifi/fw/api/coex.h,
 * drivers/net/wireless/intel/iwlwifi/fw/api/power.h and
 * drivers/net/wireless/intel/iwlwifi/fw/api/nvm-reg.h for the payloads.
 */
#define INTEL_AX211_RUNTIME_TRANSMIT_CHAINS_OPCODE 0x98U
#define INTEL_AX211_RUNTIME_COEXISTENCE_OPCODE 0x9bU
#define INTEL_AX211_RUNTIME_SOC_CONFIG_OPCODE 0x01U
#define INTEL_AX211_RUNTIME_LATENCY_TOLERANCE_OPCODE 0xeeU
#define INTEL_AX211_RUNTIME_TEMP_REPORT_OPCODE 0x04U
#define INTEL_AX211_RUNTIME_POWER_POLICY_OPCODE 0x77U
#define INTEL_AX211_RUNTIME_COUNTRY_UPDATE_OPCODE 0xc8U
#define INTEL_AX211_RUNTIME_BEACON_FILTER_OPCODE 0xd2U

#define INTEL_AX211_RUNTIME_TRANSMIT_CHAINS_VERSION 1U
#define INTEL_AX211_RUNTIME_COEXISTENCE_VERSION 6U
#define INTEL_AX211_RUNTIME_SOC_CONFIG_VERSION 2U
#define INTEL_AX211_RUNTIME_LATENCY_TOLERANCE_VERSION 3U
#define INTEL_AX211_RUNTIME_TEMP_REPORT_VERSION 1U
#define INTEL_AX211_RUNTIME_POWER_POLICY_VERSION 7U
#define INTEL_AX211_RUNTIME_COUNTRY_UPDATE_VERSION 1U
#define INTEL_AX211_RUNTIME_COUNTRY_RESPONSE_VERSION 6U
#define INTEL_AX211_RUNTIME_BEACON_FILTER_VERSION 4U

#define INTEL_AX211_RUNTIME_TRANSMIT_CHAINS_SIZE 4U
#define INTEL_AX211_RUNTIME_COEXISTENCE_SIZE 8U
#define INTEL_AX211_RUNTIME_SOC_CONFIG_SIZE 8U
#define INTEL_AX211_RUNTIME_LATENCY_TOLERANCE_SIZE 32U
#define INTEL_AX211_RUNTIME_TEMP_REPORT_SIZE 20U
#define INTEL_AX211_RUNTIME_POWER_POLICY_SIZE 4U
#define INTEL_AX211_RUNTIME_COUNTRY_UPDATE_SIZE 28U
#define INTEL_AX211_RUNTIME_BEACON_FILTER_SIZE 60U

/*
 * The capability bits of the firmware image that the runtime depends on.
 *
 * The INTERFACE_FLAGS records of the image form a 128-bit set (API bits) and
 * the FEATURE_FLAGS records a 160-bit set (feature bits); bit n is bit n % 32
 * of word n / 32.  The pinned image must have:
 * - API 56 (the short scan setup command) and API 58 (the wider channel
 *   records of the scan request), because the scan commands are written in
 *   those forms;
 * - feature 9 (the firmware inserts the DS parameter element into probe
 *   requests) and feature 74 (the firmware stops the radio itself on
 *   overheating);
 * - and must not have feature 12 (dynamic queue allocation), whose queue
 *   model the driver does not implement; the pinned image has no such bit
 *   and no row for family 5 opcode 0 in its command table.
 * When the firmware keeps its own regulatory domain, API 9 or feature 29
 * (country updates) and feature 89 (country updates that include 802.11ax)
 * are also required.  Feature 50 means the firmware sets the PCI Express
 * latency tolerance itself, and the driver then skips its own command.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/fw/file.h
 * (enum iwl_ucode_tlv_api and enum iwl_ucode_tlv_capa).
 */
#define INTEL_AX211_RUNTIME_API_BIT_COUNTRY_UPDATE 9U
#define INTEL_AX211_RUNTIME_API_BIT_SHORT_SCAN_SETUP 56U
#define INTEL_AX211_RUNTIME_API_BIT_WIDE_CHANNEL_RECORDS 58U
#define INTEL_AX211_RUNTIME_FEATURE_BIT_PROBE_DS_ELEMENT 9U
#define INTEL_AX211_RUNTIME_FEATURE_BIT_DYNAMIC_QUEUES 12U
#define INTEL_AX211_RUNTIME_FEATURE_BIT_COUNTRY_LIST 29U
#define INTEL_AX211_RUNTIME_FEATURE_BIT_SELF_LATENCY 50U
#define INTEL_AX211_RUNTIME_FEATURE_BIT_FIRMWARE_THERMAL_STOP 74U
#define INTEL_AX211_RUNTIME_FEATURE_BIT_COUNTRY_UPDATE_HE 89U

/*
 * The payload of the platform command.
 *
 * By that layout the flags 0x0a leave bit 0 (a discrete module) clear, set bit
 * 1 (low-latency crystal) and put 2 in the latency-tolerance delay field of
 * bits 3:2 (2500 us).  12000 is the crystal start-up latency in microseconds,
 * the value of the low-latency AX210-family configurations.  The
 * firmware accepts this pair on the test machines, whose AX211 is the CNVio2
 * part with its MAC in the platform.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/system.h (struct
 * iwl_soc_configuration_cmd and its flag bits) and
 * drivers/net/wireless/intel/iwlwifi/cfg/ax210.c (the crystal latencies of the
 * family).
 */
#define INTEL_AX211_RUNTIME_SOC_CONFIG_FLAGS 0x0aU
#define INTEL_AX211_RUNTIME_SOC_CONFIG_XTAL_LATENCY 12000U

/*
 * The bounds of the country answer.
 *
 * The answer has a 20-byte header (status at 0, country code at 4,
 * capabilities at 6, time at 8, geographic information at 10, channel count at
 * 16) and one 32-bit flag word per channel; there are at most 110 channels,
 * and every status from 0 to 8 still carries a usable channel list.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/nvm-reg.h (struct
 * iwl_mcc_update_resp and enum iwl_mcc_update_status).
 */
#define INTEL_AX211_RUNTIME_COUNTRY_CHANNEL_LIMIT 110U
#define INTEL_AX211_RUNTIME_COUNTRY_STATUS_MAX 8U

/* The results of the runtime codecs and of the runtime sequence. */
enum intel_ax211_runtime_result {
	INTEL_AX211_RUNTIME_OK = 0,
	INTEL_AX211_RUNTIME_INVALID = 1,
	INTEL_AX211_RUNTIME_UNSUPPORTED = 2,
	INTEL_AX211_RUNTIME_BUFFER_TOO_SMALL = 3,
	INTEL_AX211_RUNTIME_STALE = 4,
	INTEL_AX211_RUNTIME_DUPLICATE = 5,
	INTEL_AX211_RUNTIME_OUT_OF_ORDER = 6,
	INTEL_AX211_RUNTIME_TIMEOUT = 7,
	INTEL_AX211_RUNTIME_COMPLETE = 8,
	INTEL_AX211_RUNTIME_FAILED = 9,
	INTEL_AX211_RUNTIME_TRUNCATED = 10,
	INTEL_AX211_RUNTIME_OVERSIZED = 11
};

/* The steps of the runtime sequence, in the order they are sent. */
enum intel_ax211_runtime_step {
	INTEL_AX211_RUNTIME_STEP_TRANSMIT_CHAINS = 0,
	INTEL_AX211_RUNTIME_STEP_COEXISTENCE = 1,
	INTEL_AX211_RUNTIME_STEP_SOC_CONFIG = 2,
	INTEL_AX211_RUNTIME_STEP_LATENCY_TOLERANCE = 3,
	INTEL_AX211_RUNTIME_STEP_TEMP_REPORT = 4,
	INTEL_AX211_RUNTIME_STEP_POWER_POLICY = 5,
	INTEL_AX211_RUNTIME_STEP_COUNTRY_UPDATE = 6,
	INTEL_AX211_RUNTIME_STEP_SCAN_CONFIG = 7,
	INTEL_AX211_RUNTIME_STEP_BEACON_FILTER = 8,
	INTEL_AX211_RUNTIME_STEP_DONE = 9
};

/*
 * What the runtime sequence depends on: the chains and regulatory mode from the
 * NVM answer, whether LTR is enabled, and the image's capability bits.
 */
struct intel_ax211_runtime_profile {
	uint8_t tx_chain_mask;
	uint8_t rx_chain_mask;
	uint8_t firmware_regulatory;
	uint8_t ltr_enabled;
	uint32_t api_changes[4];
	uint32_t capabilities[5];
};

/* One encoded runtime command, ready for the command transaction. */
struct intel_ax211_runtime_command {
	uint8_t group;
	uint8_t opcode;
	uint8_t wire_version;
	uint8_t layout_version;
	uint8_t response_version;
	uint8_t payload[INTEL_AX211_RUNTIME_PAYLOAD_MAX];
	size_t payload_length;
};

/* The decoded answer to the country update. */
struct intel_ax211_runtime_mcc {
	uint32_t status;
	uint16_t mcc;
	uint16_t capabilities;
	uint16_t time;
	uint16_t geographic_info;
	uint8_t source;
	uint32_t channel_count;
	uint32_t channel[INTEL_AX211_RUNTIME_COUNTRY_CHANNEL_LIMIT];
};

/*
 * The runtime sequence of one firmware epoch.
 *
 * It is active while it waits on a step and terminal once it completed or
 * timed out; each step has its own deadline.
 */
struct intel_ax211_runtime_state {
	struct intel_ax211_runtime_profile profile;
	uint32_t generation;
	uint64_t deadline;
	enum intel_ax211_runtime_step step;
	uint8_t active;
	uint8_t terminal;
};

int
drv_intel_ax211_runtime_profile_from_manifest(
	const struct intel_ax211_firmware_manifest *manifest,
	const struct intel_ax211_protocol_nvm *nvm,
	int ltr_enabled,
	struct intel_ax211_runtime_profile *profile);

int
drv_intel_ax211_runtime_api89_validate(
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_runtime_profile *profile);

int
drv_intel_ax211_runtime_command_encode(
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_profile *profile,
	struct intel_ax211_runtime_command *command);

int
drv_intel_ax211_runtime_mcc_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation,
	struct intel_ax211_runtime_mcc *mcc);

int
drv_intel_ax211_runtime_begin(
	struct intel_ax211_runtime_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_runtime_profile *profile,
	uint32_t generation,
	uint64_t now_us);

int
drv_intel_ax211_runtime_current(
	const struct intel_ax211_runtime_state *state,
	uint64_t now_us,
	struct intel_ax211_runtime_command *command);

int
drv_intel_ax211_runtime_ack(
	struct intel_ax211_runtime_state *state,
	uint32_t generation,
	enum intel_ax211_runtime_step step,
	uint64_t now_us);

int
drv_intel_ax211_runtime_expire(
	struct intel_ax211_runtime_state *state,
	uint64_t now_us);

#endif
