/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private firmware protocol contract.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_PROTOCOL_H
#define KERN_DRIVERS_INTEL_AX211_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/*
 * The command version table and the limits of a command response.
 *
 * The table is a list of 4-byte entries: opcode, family, command layout
 * version, notification layout version.  The pinned image carries 217 entries
 * (868 bytes), and the parser refuses a longer table.  An entry may say 99
 * where the firmware gives no version; the driver treats 99 as "unknown" and
 * does not accept a response whose layout version is unknown.  Bit 6 of the
 * event header's flags byte marks a response to a command that the firmware
 * failed.  A response may not exceed 4088 bytes, one external command buffer
 * less its header.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/fw/file.h
 * (struct iwl_fw_cmd_version, the body of record 48) and
 * drivers/net/wireless/intel/iwlwifi/fw/api/cmdhdr.h (the failure bit of the
 * event header).
 */
#define INTEL_AX211_PROTOCOL_UNKNOWN_VERSION 99U
#define INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK 0x40U
#define INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE 4U
#define INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT 217U
#define INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES 868U
#define INTEL_AX211_PROTOCOL_COMMAND_RESPONSE_MAX 4088U

/*
 * The command families (the "group" byte of the wide header) used here.
 *
 * Every command and notification of the driver is identified by its family and
 * opcode together, so the same opcode can mean different commands in different
 * families (opcode 8, for instance, is three different commands in families 0,
 * 3 and 5; see intel-ax211-assoc.h).
 *
 * Family 0 is the classic opcode space and family 1 the same space through the
 * wide header; family 12 holds the regulatory and NVM commands.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/commands.h (enum
 * iwl_mvm_command_groups).
 */
#define INTEL_AX211_PROTOCOL_FAMILY_CLASSIC 0U
#define INTEL_AX211_PROTOCOL_FAMILY_CLASSIC_WIDE 1U
#define INTEL_AX211_PROTOCOL_FAMILY_REGULATORY_STORE 12U

/*
 * The boot notifications and commands, with their layout versions and sizes.
 *
 * The order on the AX211, as the boot code drives it:
 * - ALIVE (family 0, opcode 1, version 6) arrives first, after the upper CPU
 *   is started.  It carries a status word that must be 0xcafe (0xdead is the
 *   firmware's error status), the two LMAC records at bytes 4 and
 *   52, the UMAC record at 100, the board signature at 116 and the restore
 *   region at 128.  The restore region must be absent: the driver does not
 *   support firmware state restore.
 * - PNVM_ACCEPTED (family 12, opcode 0xfe, version 1, 4 bytes) answers the
 *   PNVM doorbell.
 * - NVM_HANDOFF (family 12, opcode 0) tells the firmware the driver has
 *   finished with the NVM in the initialization stage.
 * - NVM_QUERY (family 12, opcode 2, response version 4, 468 bytes) returns
 *   the radio's NVM: general flags, revision, board kind, number of station
 *   addresses, the standard flags, the transmit and receive chain masks, the
 *   regulatory mode, the channel count and one 32-bit flag word per channel.
 * - INIT_FINISHED (family 0, opcode 4, 4 bytes) ends the initialization
 *   stage; the image carries no version for it.
 * - SCAN_SETUP and SCAN_START (family 1, opcodes 0x0c and 0x0d) are the scan
 *   commands; the runtime sends SCAN_SETUP once, and the command table check
 *   of API 89 requires versions 5 and 17 for them.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/commands.h (the opcodes),
 * drivers/net/wireless/intel/iwlwifi/fw/api/alive.h (struct iwl_alive_ntf_v6,
 * 144 bytes) and drivers/net/wireless/intel/iwlwifi/fw/api/nvm-reg.h (the
 * opcodes, struct iwl_nvm_get_info_rsp, 468 bytes, and the PNVM
 * notification).
 */
#define INTEL_AX211_PROTOCOL_ALIVE_OPCODE 0x01U
#define INTEL_AX211_PROTOCOL_INIT_FINISHED_OPCODE 0x04U
#define INTEL_AX211_PROTOCOL_SCAN_SETUP_OPCODE 0x0cU
#define INTEL_AX211_PROTOCOL_SCAN_START_OPCODE 0x0dU
#define INTEL_AX211_PROTOCOL_NVM_HANDOFF_OPCODE 0x00U
#define INTEL_AX211_PROTOCOL_NVM_QUERY_OPCODE 0x02U
#define INTEL_AX211_PROTOCOL_PNVM_ACCEPTED_OPCODE 0xfeU

#define INTEL_AX211_PROTOCOL_ALIVE_VERSION 6U
#define INTEL_AX211_PROTOCOL_PNVM_ACCEPTED_VERSION 1U
#define INTEL_AX211_PROTOCOL_NVM_QUERY_VERSION 4U

#define INTEL_AX211_PROTOCOL_ALIVE_SIZE 144U
#define INTEL_AX211_PROTOCOL_INIT_FINISHED_SIZE 4U
#define INTEL_AX211_PROTOCOL_PNVM_ACCEPTED_SIZE 4U
#define INTEL_AX211_PROTOCOL_NVM_QUERY_SIZE 468U

/*
 * The channel list of the NVM answer.
 *
 * The first 14 flag words are 2.4 GHz channels 1 to 14, the next 37 are the 5
 * GHz channels from 36 to 181 in the order of the table in
 * drv_intel_ax211_protocol_nvm_query_decode(); the rest (6 GHz) are not
 * decoded.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/nvm-reg.h (the channel flag array
 * of 110 entries) and drivers/net/wireless/intel/iwlwifi/iwl-nvm-parse.c (the
 * order of the channels) and IEEE Std 802.11-2020 Annex E (the channel
 * numbers).
 */
#define INTEL_AX211_PROTOCOL_NVM_CHANNEL_LIMIT 110U
#define INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT 14U
#define INTEL_AX211_PROTOCOL_5GHZ_CHANNEL_LIMIT 37U

/* The status words of ALIVE: the error status and a healthy start. */
#define INTEL_AX211_PROTOCOL_ALIVE_FAULTED 0xdeadU
#define INTEL_AX211_PROTOCOL_ALIVE_HEALTHY 0xcafeU

/*
 * The flags of the NVM answer.
 *
 * The standard flags say which bands and 802.11 amendments the module may use
 * and whether MIMO is disabled; the driver requires 2.4 GHz.  In a channel's
 * flag word, USABLE (bit 0) means the channel exists on this module and
 * MAY_TRANSMIT (bit 3) that the driver may send probe requests on it (active
 * scan); a 5 GHz channel counts as usable only when the 5 GHz band is enabled.
 * When the firmware runs its own regulatory domain, the country answer, not
 * the USABLE bit, decides which channels may be used.  FUSES_BLANK in the
 * general flags means the radio's fuses were never programmed; the driver
 * refuses such a radio.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/nvm-reg.h (the general flags and
 * the MAC SKU flags of struct iwl_nvm_get_info_rsp, and the channel flags).
 */
#define INTEL_AX211_PROTOCOL_NVM_FUSES_BLANK 0x01U
#define INTEL_AX211_PROTOCOL_NVM_BAND_24_ENABLED 0x01U
#define INTEL_AX211_PROTOCOL_NVM_BAND_52_ENABLED 0x02U
#define INTEL_AX211_PROTOCOL_NVM_11N_ENABLED 0x04U
#define INTEL_AX211_PROTOCOL_NVM_11AC_ENABLED 0x08U
#define INTEL_AX211_PROTOCOL_NVM_11AX_ENABLED 0x10U
#define INTEL_AX211_PROTOCOL_NVM_MIMO_DISABLED 0x20U

#define INTEL_AX211_PROTOCOL_CHANNEL_FLAG_USABLE 0x01U
#define INTEL_AX211_PROTOCOL_CHANNEL_FLAG_MAY_TRANSMIT 0x08U

/* The results of the firmware protocol codecs. */
enum intel_ax211_protocol_result {
	INTEL_AX211_PROTOCOL_OK = 0,
	INTEL_AX211_PROTOCOL_INVALID = 1,
	INTEL_AX211_PROTOCOL_TRUNCATED = 2,
	INTEL_AX211_PROTOCOL_OVERSIZED = 3,
	INTEL_AX211_PROTOCOL_UNSUPPORTED = 4,
	INTEL_AX211_PROTOCOL_MISSING = 5,
	INTEL_AX211_PROTOCOL_DUPLICATE = 6,
	INTEL_AX211_PROTOCOL_FAILED = 7,
	INTEL_AX211_PROTOCOL_STALE = 8,
	INTEL_AX211_PROTOCOL_TOKEN_MISMATCH = 9
};

/* The versions one entry of the command version table lists for one command. */
struct intel_ax211_protocol_command_version {
	uint8_t opcode;
	uint8_t group;
	uint8_t command_version;
	uint8_t notification_version;
};

/*
 * The command version record of an image, borrowed from the image's bytes.
 *
 * The bytes stay owned by the loaded firmware file; the table is valid as long
 * as that file is.
 */
struct intel_ax211_protocol_command_table {
	const uint8_t *bytes;
	size_t count;
};

/*
 * One firmware message with its header fields, ready for validation.
 *
 * The payload points into the receive buffer the message arrived in, and the
 * generation is the firmware epoch the controller stamped on it.
 */
struct intel_ax211_protocol_message {
	uint8_t opcode;
	uint8_t group;
	uint8_t version;
	uint8_t flags;
	uint8_t queue;
	uint8_t index;
	uint32_t generation;
	const uint8_t *payload;
	size_t payload_length;
};

/*
 * What a pending command expects of its answer: the ring slot, the family and
 * opcode, the layout version and the length bounds.
 */
struct intel_ax211_protocol_pending_command {
	uint8_t opcode;
	uint8_t group;
	uint8_t response_version;
	uint8_t queue;
	uint8_t index;
	uint32_t generation;
	size_t minimum_response_length;
	size_t maximum_response_length;
};

/* The record of one lower MAC in the ALIVE notification. */
struct intel_ax211_protocol_lmac_alive {
	uint32_t major;
	uint32_t minor;
	uint8_t version_subtype;
	uint8_t version_type;
	uint8_t mac;
	uint8_t option;
	uint32_t timestamp;
	uint32_t error_log_address;
	uint32_t event_log_address;
	uint32_t cpu_state_address;
	uint32_t trace_setup_address;
	uint32_t alive_counter_address;
	uint32_t scheduler_sram_base;
	uint32_t relay_buffer_address;
	uint32_t relay_buffer_size;
};

/* The record of the upper MAC in the ALIVE notification. */
struct intel_ax211_protocol_umac_alive {
	uint32_t major;
	uint32_t minor;
	uint32_t upper_error_log_address;
	uint32_t trace_buffer_address;
};

/*
 * The decoded ALIVE notification: the firmware's status, where its error and
 * trace tables are, and the board signature that selects the PNVM sections.
 */
struct intel_ax211_protocol_alive {
	uint16_t status;
	uint16_t flags;
	struct intel_ax211_protocol_lmac_alive lmac[2];
	struct intel_ax211_protocol_umac_alive umac;
	uint32_t sku[3];
	uint64_t restore_region_base;
	uint32_t restore_region_size;
	uint32_t restore_region_enabled;
};

/*
 * One channel of the NVM answer: its number, its flag word, and whether it may
 * be used and scanned actively.
 */
struct intel_ax211_protocol_channel {
	uint8_t number;
	uint8_t valid;
	uint8_t active;
	uint32_t flags;
};

/*
 * The decoded answer to the NVM query: the bands and amendments the module may
 * use, its chain masks, and its channels.
 */
struct intel_ax211_protocol_nvm {
	uint32_t nvm_flags;
	uint16_t nvm_revision;
	uint8_t board_kind;
	uint8_t hardware_address_count;
	uint32_t standard_flags;
	uint8_t band_24_enabled;
	uint8_t band_52_enabled;
	uint8_t standard_11n;
	uint8_t standard_11ac;
	uint8_t standard_11ax;
	uint8_t mimo_disabled;
	uint8_t tx_chain_mask;
	uint8_t rx_chain_mask;
	uint8_t firmware_regulatory;
	uint32_t channel_total;
	struct intel_ax211_protocol_channel channel_24ghz[INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT];
	size_t channel_24ghz_count;
	size_t valid_24ghz_count;
	struct intel_ax211_protocol_channel channel_5ghz[INTEL_AX211_PROTOCOL_5GHZ_CHANNEL_LIMIT];
	size_t channel_5ghz_count;
	size_t valid_5ghz_count;
};

int
drv_intel_ax211_protocol_command_table_parse(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_protocol_command_table *table);

int
drv_intel_ax211_protocol_command_version_lookup(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group,
	uint8_t opcode,
	struct intel_ax211_protocol_command_version *version);

int
drv_intel_ax211_protocol_command_table_validate_api89(
	const struct intel_ax211_protocol_command_table *table);

int
drv_intel_ax211_protocol_command_response_validate(
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending);

int
drv_intel_ax211_protocol_alive_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation,
	struct intel_ax211_protocol_alive *alive);

int
drv_intel_ax211_protocol_pnvm_accepted(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation);

int
drv_intel_ax211_protocol_init_finished(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation);

int
drv_intel_ax211_protocol_nvm_query_decode(
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending,
	struct intel_ax211_protocol_nvm *nvm);

#endif
