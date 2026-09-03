/*
 * zedBSD Intel AX211 private API89 association-session contract
 *
 * Portions derived from OpenBSD sys/dev/pci/if_iwxreg.h and if_iwx.c.
 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * OpenBSD source provenance:
 *   sys/dev/pci/if_iwx.c, if_iwxreg.h at
 *   0f464d413c50396e4e6cd70948f15613d6a73081
 *
 * SPDX-License-Identifier: ISC AND BSD-3-Clause
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_ASSOC_H
#define ZEDBSD_DRIVERS_INTEL_AX211_ASSOC_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"

#define INTEL_AX211_ASSOC_COMMAND_TIMEOUT_US          1000000U
#define INTEL_AX211_ASSOC_COMMAND_LIMIT                    32U
#define INTEL_AX211_ASSOC_PAYLOAD_MAX                      208U
#define INTEL_AX211_ASSOC_RESPONSE_MAX                       8U
#define INTEL_AX211_ASSOC_CHANNEL_WIDTH_MHZ                 20U
#define INTEL_AX211_ASSOC_EDCA_COUNT                         4U

#define INTEL_AX211_ASSOC_GROUP_LEGACY                       0U
#define INTEL_AX211_ASSOC_GROUP_LONG                         1U
#define INTEL_AX211_ASSOC_GROUP_MAC_CONFIG                   3U
#define INTEL_AX211_ASSOC_GROUP_DATA_PATH                    5U

#define INTEL_AX211_ASSOC_PHY_CONTEXT_OPCODE              0x08U
#define INTEL_AX211_ASSOC_MAC_CONFIG_OPCODE               0x08U
#define INTEL_AX211_ASSOC_LINK_CONFIG_OPCODE              0x09U
#define INTEL_AX211_ASSOC_STATION_CONFIG_OPCODE           0x0aU
#define INTEL_AX211_ASSOC_STATION_REMOVE_OPCODE           0x0cU
#define INTEL_AX211_ASSOC_RLC_CONFIG_OPCODE               0x08U
#define INTEL_AX211_ASSOC_QUEUE_CONFIG_OPCODE             0x17U
#define INTEL_AX211_ASSOC_SESSION_PROTECTION_OPCODE       0x05U
#define INTEL_AX211_ASSOC_SESSION_NOTIFICATION_OPCODE     0xfbU
#define INTEL_AX211_ASSOC_MCAST_FILTER_OPCODE              0xd0U
#define INTEL_AX211_ASSOC_MAC_POWER_OPCODE                 0xa9U

#define INTEL_AX211_ASSOC_PHY_CONTEXT_VERSION                4U
#define INTEL_AX211_ASSOC_MAC_CONFIG_VERSION                  2U
#define INTEL_AX211_ASSOC_LINK_CONFIG_VERSION                 2U
#define INTEL_AX211_ASSOC_STATION_CONFIG_VERSION              1U
#define INTEL_AX211_ASSOC_STATION_REMOVE_VERSION              1U
#define INTEL_AX211_ASSOC_RLC_CONFIG_VERSION                 2U
#define INTEL_AX211_ASSOC_QUEUE_CONFIG_VERSION               3U
#define INTEL_AX211_ASSOC_QUEUE_RESPONSE_VERSION             2U
#define INTEL_AX211_ASSOC_SESSION_PROTECTION_VERSION         2U
#define INTEL_AX211_ASSOC_SESSION_NOTIFICATION_LAYOUT_VERSION 2U
/* API89 advertises notification version 3 for the unchanged v2 layout. */
#define INTEL_AX211_ASSOC_SESSION_NOTIFICATION_API89_VERSION  3U
#define INTEL_AX211_ASSOC_MCAST_FILTER_VERSION                1U
#define INTEL_AX211_ASSOC_MAC_POWER_VERSION                   1U

#define INTEL_AX211_ASSOC_PHY_CONTEXT_SIZE                  32U
#define INTEL_AX211_ASSOC_MAC_CONFIG_SIZE                   52U
#define INTEL_AX211_ASSOC_LINK_CONFIG_SIZE                 208U
#define INTEL_AX211_ASSOC_STATION_CONFIG_SIZE               96U
#define INTEL_AX211_ASSOC_REMOVE_STATION_SIZE                4U
#define INTEL_AX211_ASSOC_RLC_CONFIG_SIZE                   32U
#define INTEL_AX211_ASSOC_QUEUE_CONFIG_SIZE                 36U
#define INTEL_AX211_ASSOC_QUEUE_RESPONSE_SIZE                8U
#define INTEL_AX211_ASSOC_SESSION_PROTECTION_SIZE           24U
#define INTEL_AX211_ASSOC_SESSION_NOTIFICATION_SIZE         16U
#define INTEL_AX211_ASSOC_MCAST_FILTER_SIZE                  12U
#define INTEL_AX211_ASSOC_MAC_POWER_SIZE                     40U
#define INTEL_AX211_ASSOC_MAC_POWER_RESPONSE_SIZE             4U
#define INTEL_AX211_ASSOC_MAC_POWER_KEEP_ALIVE_MIN_SECONDS   25U
#define INTEL_AX211_ASSOC_MAC_ID                              0U
#define INTEL_AX211_ASSOC_LINK_ID                             0U
#define INTEL_AX211_ASSOC_STATION_ID                          0U

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

enum intel_ax211_assoc_phase {
	INTEL_AX211_ASSOC_PHASE_IDLE = 0,
	INTEL_AX211_ASSOC_PHASE_AUTH = 1,
	INTEL_AX211_ASSOC_PHASE_AUTH_READY = 2,
	INTEL_AX211_ASSOC_PHASE_ASSOCIATING = 3,
	INTEL_AX211_ASSOC_PHASE_ASSOCIATED = 4,
	INTEL_AX211_ASSOC_PHASE_ROLLBACK = 5,
	INTEL_AX211_ASSOC_PHASE_FAILED = 6
};

enum intel_ax211_assoc_step {
	INTEL_AX211_ASSOC_STEP_NONE = 0,
	INTEL_AX211_ASSOC_STEP_MAC_ADD = 1,
	INTEL_AX211_ASSOC_STEP_LINK_ADD = 2,
	INTEL_AX211_ASSOC_STEP_PHY_ADD = 3,
	INTEL_AX211_ASSOC_STEP_RLC_CONFIG = 4,
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

enum intel_ax211_assoc_header {
	INTEL_AX211_ASSOC_HEADER_LEGACY = 0,
	INTEL_AX211_ASSOC_HEADER_WIDE = 1
};

enum intel_ax211_assoc_response_kind {
	INTEL_AX211_ASSOC_RESPONSE_EMPTY = 0,
	INTEL_AX211_ASSOC_RESPONSE_STATUS_ZERO = 1,
	INTEL_AX211_ASSOC_RESPONSE_QUEUE = 2,
	INTEL_AX211_ASSOC_RESPONSE_IGNORED = 3
};

struct intel_ax211_assoc_edca {
	uint8_t ecw_min;
	uint8_t ecw_max;
	uint8_t aifsn;
	uint16_t txop_32us;
};

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

struct intel_ax211_assoc_update {
	uint16_t association_id;
	uint8_t dtim_period;
	uint8_t dtim_count;
	uint32_t beacon_arrive_time;
	uint64_t beacon_tsf;
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
	int (*exchange)(void *argument,
	    const struct intel_ax211_assoc_command *command,
	    struct intel_ax211_assoc_reply *reply);
};

/* Zero-initialize state before first use; an IDLE rollback may be reused. */
int intel_ax211_assoc_api89_validate(
	const struct intel_ax211_protocol_command_table *table);
/* API89 lists this logical legacy command under LONG_GROUP (group 1). */
int intel_ax211_assoc_mcast_filter_api89_validate(
	const struct intel_ax211_protocol_command_table *table);
int intel_ax211_assoc_mcast_filter_encode(
	const uint8_t bssid[6], uint8_t *output, size_t output_capacity);
int intel_ax211_assoc_mac_power_api89_validate(
	const struct intel_ax211_protocol_command_table *table);
int intel_ax211_assoc_mac_power_encode(uint8_t dtim_period,
	uint32_t beacon_interval_ms, uint8_t *output, size_t output_capacity);
int intel_ax211_assoc_mac_power_response_validate(
	const uint8_t *response, size_t response_length);
int intel_ax211_assoc_begin(struct intel_ax211_assoc_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_assoc_profile *profile,
	uint64_t common_generation, uint32_t hardware_epoch, uint64_t now_us);
int intel_ax211_assoc_begin_update(struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_update *update,
	uint64_t common_generation, uint32_t hardware_epoch, uint64_t now_us);
int intel_ax211_assoc_current(const struct intel_ax211_assoc_state *state,
	uint64_t now_us, struct intel_ax211_assoc_command *command);
int intel_ax211_assoc_accept(struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_command *command,
	const struct intel_ax211_assoc_reply *reply, uint64_t now_us);
/*
 * Accepts only an unsolicited v2-layout SESSION_PROTECTION_NOTIF from the
 * current firmware epoch.  SESSION_EXPIRED means a successful ASSOC
 * protection end for this MAC cleared the private resource; EVENT_IGNORED
 * means another MAC/configuration, a start, or an unsuccessful notification
 * made no state change.
 */
int intel_ax211_assoc_session_event_accept(
	struct intel_ax211_assoc_state *state,
	const struct intel_ax211_protocol_message *message,
	uint64_t common_generation, uint32_t hardware_epoch);
int intel_ax211_assoc_expire(struct intel_ax211_assoc_state *state,
	uint64_t now_us);
int intel_ax211_assoc_cancel(struct intel_ax211_assoc_state *state,
	uint64_t common_generation, uint32_t hardware_epoch, uint64_t now_us);
int intel_ax211_assoc_drive(struct intel_ax211_assoc_state *state,
	const struct intel_ax211_assoc_ops *ops, void *argument);

#endif
