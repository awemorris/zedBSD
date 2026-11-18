/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private API89 association-session contract.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_ASSOC_H
#define KERN_DRIVERS_INTEL_AX211_ASSOC_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"

/*
 * zedBSD's bounds of the association session: one second per command, at
 * most 32 commands in one session, the largest payload (the 208-byte link
 * setup), and a 20 MHz channel.  EDCA has four access categories.
 *
 * Value referenced: IEEE Std 802.11-2020 9.4.2.28 (the four access categories
 * of EDCA).
 */
#define INTEL_AX211_ASSOC_COMMAND_TIMEOUT_US	1000000U
#define INTEL_AX211_ASSOC_COMMAND_LIMIT		32U
#define INTEL_AX211_ASSOC_PAYLOAD_MAX		208U
#define INTEL_AX211_ASSOC_RESPONSE_MAX		8U
#define INTEL_AX211_ASSOC_CHANNEL_WIDTH_MHZ	20U
#define INTEL_AX211_ASSOC_EDCA_COUNT		4U

/*
 * The command families of the association commands.
 *
 * 0 is the classic opcode space, 1 the same space through the wide header, 3
 * the MAC configuration family (interfaces, links, stations and time
 * reservations), 5 the data path family (receive chains, transmit queues and
 * keys).
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/commands.h (enum
 * iwl_mvm_command_groups).
 */
#define INTEL_AX211_ASSOC_FAMILY_CLASSIC 0U
#define INTEL_AX211_ASSOC_FAMILY_CLASSIC_WIDE 1U
#define INTEL_AX211_ASSOC_FAMILY_LINK_SETUP 3U
#define INTEL_AX211_ASSOC_FAMILY_TRAFFIC 5U

/*
 * The association commands: opcodes, layout versions and payload sizes.
 *
 * Each command is named by family and opcode; opcode 8 means three different
 * commands in families 0, 3 and 5.  The versions are those the pinned API 89
 * image lists, and the API 89 check refuses an image that lists any of them
 * differently.
 *
 * - CHANNEL_CONTEXT (0/0x08, v4, 32 bytes) adds or removes the channel the
 *   link runs on: action at byte 4, channel number as a 32-bit word at 8,
 *   band at 12.  The LMAC number at byte 16 stays zero on this
 *   single-band radio.  It is the one association command with the narrow
 *   header.
 * - INTERFACE_SETUP (3/0x08, v2, 52 bytes) adds, modifies or removes the
 *   station interface: slot at 0, action at 4, interface kind at 8, own
 *   address at 12, receive filter at 20, the no-ACK flag at 32, associated
 *   flag at 36 and association id at 40.
 * - LINK_SETUP (3/0x09, v2, 208 bytes) describes the link: action at 0,
 *   link slot at 4, interface slot at 8, channel context at 12, own address
 *   at 16, then for a modification the change mask at 24, the active flag at
 *   28, ACK rates at 36 and 40, preamble at 44, slot time at 48, QoS at 56,
 *   the four EDCA entries from 60 (8 bytes each), beacon interval at 136 and
 *   DTIM interval at 140.
 * - PEER_SETUP (3/0x0a, v1, 96 bytes) and PEER_REMOVE (3/0x0c, v1, 4 bytes)
 *   add and remove the access point's station entry: slot at 0, link at 4,
 *   its address at 8 and 16, kind at 24 and association id at 28.
 * - RECEIVE_CHAINS (5/0x08, v2, 32 bytes) sets the receive chains: the chain
 *   mask shifted left by one, with bits 10 and 12, at byte 4.  Unlike the
 *   other wide commands, the API 89 firmware wants the layout version 2 in
 *   the version byte of the wide header as well; every other command of
 *   this file sends 0 there.
 * - QUEUE_SETUP (5/0x17, v3, 36 bytes; answer v2, 8 bytes) adds or removes
 *   the transmit queue of management traffic (TID 15): operation at 0,
 *   station mask at 4, TID at 8, size code at 16, byte-count table address
 *   at 20 and descriptor ring address at 28.  The answer names the queue
 *   the firmware allocated.
 * - TIME_RESERVATION (3/0x05, v2, 24 bytes) asks the firmware to stay on the
 *   channel for the join: interface slot at 0, action at 4, the join
 *   configuration (0) at 8 and the duration at 12, nine beacon intervals.
 *   The firmware reports the end of the reservation with
 *   TIME_RESERVATION_NOTICE (3/0xfb, 16 bytes: interface, status, start
 *   flag, configuration); the image lists that notice as version 3 with the
 *   unchanged version 2 layout.
 * - MULTICAST_LIST (1/0xd0, v1, 12 bytes) is sent after association: byte 0
 *   set (filter on own frames), byte 3 set (pass all multicast), BSSID at
 *   byte 4.
 * - POWER_SAVE (1/0xa9, v1, 40 bytes) carries the interface slot at 0, the
 *   power flags at 4 (zero: power save off) and the keep-alive period in
 *   seconds at 6.  The period is three DTIM intervals rounded up to whole
 *   seconds, and at least 25 s.  The firmware may answer with no payload or
 *   with one 32-bit status word that must be zero.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/commands.h,
 * drivers/net/wireless/intel/iwlwifi/fw/api/mac-cfg.h,
 * drivers/net/wireless/intel/iwlwifi/fw/api/phy-ctxt.h,
 * drivers/net/wireless/intel/iwlwifi/fw/api/datapath.h,
 * drivers/net/wireless/intel/iwlwifi/fw/api/time-event.h,
 * drivers/net/wireless/intel/iwlwifi/fw/api/power.h and
 * drivers/net/wireless/intel/iwlwifi/fw/api/mac.h (the numbers and layouts).
 */
#define INTEL_AX211_ASSOC_CHANNEL_CONTEXT_OPCODE 0x08U
#define INTEL_AX211_ASSOC_INTERFACE_SETUP_OPCODE 0x08U
#define INTEL_AX211_ASSOC_LINK_SETUP_OPCODE 0x09U
#define INTEL_AX211_ASSOC_PEER_SETUP_OPCODE 0x0aU
#define INTEL_AX211_ASSOC_PEER_REMOVE_OPCODE 0x0cU
#define INTEL_AX211_ASSOC_RECEIVE_CHAINS_OPCODE 0x08U
#define INTEL_AX211_ASSOC_QUEUE_SETUP_OPCODE 0x17U
#define INTEL_AX211_ASSOC_TIME_RESERVATION_OPCODE 0x05U
#define INTEL_AX211_ASSOC_TIME_RESERVATION_NOTICE_OPCODE 0xfbU
#define INTEL_AX211_ASSOC_MULTICAST_LIST_OPCODE 0xd0U
#define INTEL_AX211_ASSOC_POWER_SAVE_OPCODE 0xa9U

#define INTEL_AX211_ASSOC_CHANNEL_CONTEXT_VERSION 4U
#define INTEL_AX211_ASSOC_INTERFACE_SETUP_VERSION 2U
#define INTEL_AX211_ASSOC_LINK_SETUP_VERSION 2U
#define INTEL_AX211_ASSOC_PEER_SETUP_VERSION 1U
#define INTEL_AX211_ASSOC_PEER_REMOVE_VERSION 1U
#define INTEL_AX211_ASSOC_RECEIVE_CHAINS_VERSION 2U
#define INTEL_AX211_ASSOC_QUEUE_SETUP_VERSION 3U
#define INTEL_AX211_ASSOC_QUEUE_RESPONSE_VERSION 2U
#define INTEL_AX211_ASSOC_TIME_RESERVATION_VERSION 2U
#define INTEL_AX211_ASSOC_TIME_RESERVATION_NOTICE_LAYOUT_VERSION 2U
/* API89 advertises notification version 3 for the unchanged v2 layout. */
#define INTEL_AX211_ASSOC_TIME_RESERVATION_NOTICE_API89_VERSION 3U
#define INTEL_AX211_ASSOC_MULTICAST_LIST_VERSION 1U
#define INTEL_AX211_ASSOC_POWER_SAVE_VERSION 1U

#define INTEL_AX211_ASSOC_CHANNEL_CONTEXT_SIZE 32U
#define INTEL_AX211_ASSOC_INTERFACE_SETUP_SIZE 52U
#define INTEL_AX211_ASSOC_LINK_SETUP_SIZE 208U
#define INTEL_AX211_ASSOC_PEER_SETUP_SIZE 96U
#define INTEL_AX211_ASSOC_PEER_REMOVE_SIZE 4U
#define INTEL_AX211_ASSOC_RECEIVE_CHAINS_SIZE 32U
#define INTEL_AX211_ASSOC_QUEUE_SETUP_SIZE 36U
#define INTEL_AX211_ASSOC_QUEUE_RESPONSE_SIZE 8U
#define INTEL_AX211_ASSOC_TIME_RESERVATION_SIZE 24U
#define INTEL_AX211_ASSOC_TIME_RESERVATION_NOTICE_SIZE 16U
#define INTEL_AX211_ASSOC_MULTICAST_LIST_SIZE 12U
#define INTEL_AX211_ASSOC_POWER_SAVE_SIZE 40U
#define INTEL_AX211_ASSOC_POWER_SAVE_RESPONSE_SIZE 4U
#define INTEL_AX211_ASSOC_POWER_SAVE_KEEP_ALIVE_MIN_SECONDS 25U

/*
 * The firmware slots of the one interface, link and access point that the
 * driver uses; all three are slot 0.
 */
#define INTEL_AX211_ASSOC_INTERFACE_SLOT 0U
#define INTEL_AX211_ASSOC_LINK_SLOT 0U
#define INTEL_AX211_ASSOC_PEER_SLOT 0U

/* The results of the association session. */
enum intel_ax211_assoc_result {
	INTEL_AX211_ASSOC_OK = 0,
	INTEL_AX211_ASSOC_INVALID = 1,
	INTEL_AX211_ASSOC_UNSUPPORTED = 2,
	INTEL_AX211_ASSOC_PENDING = 3,
	INTEL_AX211_ASSOC_AUTH_READY = 4,
	INTEL_AX211_ASSOC_COMPLETE = 5,
	INTEL_AX211_ASSOC_STALE = 6,
	INTEL_AX211_ASSOC_DUPLICATE = 7,
	INTEL_AX211_ASSOC_OUT_OF_ORDER = 8,
	INTEL_AX211_ASSOC_TIMEOUT = 9,
	INTEL_AX211_ASSOC_FIRMWARE = 10,
	INTEL_AX211_ASSOC_IO = 11,
	INTEL_AX211_ASSOC_ROLLED_BACK = 12,
	INTEL_AX211_ASSOC_ROLLBACK_FAILED = 13,
	INTEL_AX211_ASSOC_TRUNCATED = 14,
	INTEL_AX211_ASSOC_OVERSIZED = 15,
	INTEL_AX211_ASSOC_EVENT_IGNORED = 16,
	INTEL_AX211_ASSOC_SESSION_EXPIRED = 17,
	INTEL_AX211_ASSOC_BUFFER_TOO_SMALL = 18
};

/*
 * Where the session is: the authentication stage, its end, the association
 * stage, its end, the rollback, or a failed rollback.
 */
enum intel_ax211_assoc_phase {
	INTEL_AX211_ASSOC_PHASE_IDLE = 0,
	INTEL_AX211_ASSOC_PHASE_AUTH = 1,
	INTEL_AX211_ASSOC_PHASE_AUTH_READY = 2,
	INTEL_AX211_ASSOC_PHASE_ASSOCIATING = 3,
	INTEL_AX211_ASSOC_PHASE_ASSOCIATED = 4,
	INTEL_AX211_ASSOC_PHASE_ROLLBACK = 5,
	INTEL_AX211_ASSOC_PHASE_FAILED = 6
};

/*
 * The commands of the session in the order they are sent: the authentication
 * stage, the association stage, then the rollback's removals.
 */
enum intel_ax211_assoc_step {
	INTEL_AX211_ASSOC_STEP_NONE = 0,
	INTEL_AX211_ASSOC_STEP_MAC_ADD = 1,
	INTEL_AX211_ASSOC_STEP_LINK_ADD = 2,
	INTEL_AX211_ASSOC_STEP_PHY_ADD = 3,
	INTEL_AX211_ASSOC_STEP_RECEIVE_CHAINS = 4,
	INTEL_AX211_ASSOC_STEP_LINK_ASSIGN = 5,
	INTEL_AX211_ASSOC_STEP_LINK_ACTIVATE = 6,
	INTEL_AX211_ASSOC_STEP_STATION_ADD = 7,
	INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE = 8,
	INTEL_AX211_ASSOC_STEP_SESSION_PROTECT = 9,
	INTEL_AX211_ASSOC_STEP_MAC_ASSOCIATE = 10,
	INTEL_AX211_ASSOC_STEP_LINK_ASSOCIATE = 11,
	INTEL_AX211_ASSOC_STEP_STATION_UPDATE = 12,
	INTEL_AX211_ASSOC_STEP_SESSION_REMOVE = 13,
	INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE = 14,
	INTEL_AX211_ASSOC_STEP_STATION_REMOVE = 15,
	INTEL_AX211_ASSOC_STEP_LINK_DEACTIVATE = 16,
	INTEL_AX211_ASSOC_STEP_LINK_REMOVE = 17,
	INTEL_AX211_ASSOC_STEP_MAC_REMOVE = 18,
	INTEL_AX211_ASSOC_STEP_PHY_REMOVE = 19
};

/* The command header a command is sent with. */
enum intel_ax211_assoc_header {
	INTEL_AX211_ASSOC_HEADER_LEGACY = 0,
	INTEL_AX211_ASSOC_HEADER_WIDE = 1
};

/* The kind of answer a command expects. */
enum intel_ax211_assoc_response_kind {
	INTEL_AX211_ASSOC_RESPONSE_EMPTY = 0,
	INTEL_AX211_ASSOC_RESPONSE_STATUS_ZERO = 1,
	INTEL_AX211_ASSOC_RESPONSE_QUEUE = 2,
	INTEL_AX211_ASSOC_RESPONSE_IGNORED = 3
};

/*
 * One access category's parameters: the window exponents, AIFSN, and the TXOP
 * limit in units of 32 microseconds.
 */
struct intel_ax211_assoc_edca {
	uint8_t ecw_min;
	uint8_t ecw_max;
	uint8_t aifsn;
	uint16_t txop_32us;
};

/*
 * What the authentication stage needs: the addresses, the channel, the rates
 * and timing of the network, and the management queue's DMA addresses.
 */
struct intel_ax211_assoc_profile {
	uint8_t station_address[6];
	uint8_t bssid[6];
	uint8_t channel;
	uint8_t channel_width_mhz;
	uint8_t rx_chain_mask;
	uint8_t cck_ack_rates;
	uint8_t ofdm_ack_rates;
	uint8_t short_preamble;
	uint8_t short_slot;
	uint8_t qos;
	uint16_t beacon_interval_tu;
	uint8_t dtim_period;
	uint64_t queue_byte_count_address;
	uint64_t queue_descriptor_address;
	struct intel_ax211_assoc_edca edca[INTEL_AX211_ASSOC_EDCA_COUNT];
};

/* What the association stage adds from the access point's answer. */
struct intel_ax211_assoc_update {
	uint16_t association_id;
	uint8_t dtim_period;
	uint8_t dtim_count;
	uint32_t beacon_arrive_time;
	uint64_t beacon_timestamp;
};

/*
 * common_generation identifies the host WLAN operation.  hardware_epoch
 * independently fences replies from a particular firmware boot.
 */
struct intel_ax211_assoc_command {
	enum intel_ax211_assoc_step step;
	enum intel_ax211_assoc_header header;
	enum intel_ax211_assoc_response_kind response_kind;
	uint8_t group;
	uint8_t opcode;
	uint8_t wire_version;
	uint8_t layout_version;
	uint8_t response_version;
	uint32_t sequence;
	uint64_t common_generation;
	uint32_t hardware_epoch;
	uint64_t deadline;
	uint8_t payload[INTEL_AX211_ASSOC_PAYLOAD_MAX];
	size_t payload_length;
};

/* The firmware's answer to one command, as exchange reports it. */
struct intel_ax211_assoc_reply {
	enum intel_ax211_assoc_step step;
	uint8_t response_version;
	int32_t acknowledgement;
	uint32_t sequence;
	uint64_t common_generation;
	uint32_t hardware_epoch;
	uint8_t payload[INTEL_AX211_ASSOC_RESPONSE_MAX];
	size_t payload_length;
};

/*
 * One association session: the current step and its deadline, the firmware
 * objects it has created, and the failure that started a rollback.
 */
struct intel_ax211_assoc_state {
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_assoc_update update;
	uint64_t common_generation;
	uint32_t hardware_epoch;
	uint64_t last_common_generation;
	uint32_t next_sequence;
	uint32_t active_sequence;
	uint32_t last_completed_sequence;
	uint64_t deadline;
	uint32_t resources;
	enum intel_ax211_assoc_phase phase;
	enum intel_ax211_assoc_step step;
	int failure;
	uint8_t initialized;
	uint8_t update_valid;
	uint8_t session_ended;
};

/*
 * exchange sends one descriptor and normalizes its command ACK and optional
 * response.  Any non-OK return is treated as uncertain and is never retried;
 * the state machine performs bounded reverse-order cleanup instead.
 */
struct intel_ax211_assoc_ops {
	uint64_t (*clock_us)(void *argument);
	int (*exchange)(void *argument, const struct intel_ax211_assoc_command *command, struct intel_ax211_assoc_reply *reply);
};

/* Zero-initialize state before first use; an IDLE rollback may be reused. */
int
drv_intel_ax211_assoc_api89_validate(
	const struct intel_ax211_protocol_command_table *table);

/* API89 lists this logical legacy command under LONG_GROUP (group 1). */
int
drv_intel_ax211_assoc_mcast_filter_api89_validate(
	const struct intel_ax211_protocol_command_table *table);

int
drv_intel_ax211_assoc_mcast_filter_encode(
	const uint8_t bssid[6],
	uint8_t *output,
	size_t output_capacity);

int
drv_intel_ax211_assoc_mac_power_api89_validate(
	const struct intel_ax211_protocol_command_table *table);

int
drv_intel_ax211_assoc_mac_power_encode(
	uint8_t dtim_period,
	uint32_t beacon_interval_ms,
	uint8_t *output,
	size_t output_capacity);

int
drv_intel_ax211_assoc_mac_power_response_validate(
	const uint8_t *response,
	size_t response_length);

int
drv_intel_ax211_assoc_begin(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_assoc_profile *profile,
	uint64_t common_generation,
	uint32_t hardware_epoch,
	uint64_t now_us);

int
drv_intel_ax211_assoc_begin_update(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_update *update,
	uint64_t common_generation, uint32_t hardware_epoch, uint64_t now_us);
int drv_intel_ax211_assoc_current(const struct intel_ax211_assoc_state *state,
				  uint64_t now_us,
				  struct intel_ax211_assoc_command *command);
int drv_intel_ax211_assoc_accept(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_command *command,
	const struct intel_ax211_assoc_reply *reply, uint64_t now_us);
/*
 * Accepts only an unsolicited v2-layout SESSION_PROTECTION_NOTIF from the
 * current firmware epoch.  SESSION_EXPIRED means a successful ASSOC
 * protection end for this MAC cleared the private resource; EVENT_IGNORED
 * means another MAC/configuration, a start, or an unsuccessful notification
 * made no state change.
 */
int
drv_intel_ax211_assoc_session_event_accept(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_protocol_message *message,
	uint64_t common_generation,
	uint32_t hardware_epoch);

int
drv_intel_ax211_assoc_expire(
	struct intel_ax211_assoc_state *state,
	uint64_t now_us);

int
drv_intel_ax211_assoc_cancel(
	struct intel_ax211_assoc_state *state,
	uint64_t common_generation,
	uint32_t hardware_epoch,
	uint64_t now_us);

int
drv_intel_ax211_assoc_drive(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_ops *ops,
	void *argument);

#endif
