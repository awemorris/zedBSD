/*
 * zedBSD Intel AX211 private API89 runtime contract
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
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_RUNTIME_H
#define ZEDBSD_DRIVERS_INTEL_AX211_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-internal.h"
#include "intel-ax211-protocol.h"

#define INTEL_AX211_RUNTIME_COMMAND_TIMEOUT_US        1000000U
#define INTEL_AX211_RUNTIME_PAYLOAD_MAX                    60U

#define INTEL_AX211_RUNTIME_GROUP_LONG                      1U
#define INTEL_AX211_RUNTIME_GROUP_SYSTEM                    2U
#define INTEL_AX211_RUNTIME_GROUP_PHY                       4U

#define INTEL_AX211_RUNTIME_TX_ANT_OPCODE                0x98U
#define INTEL_AX211_RUNTIME_BT_CONFIG_OPCODE             0x9bU
#define INTEL_AX211_RUNTIME_SOC_CONFIG_OPCODE            0x01U
#define INTEL_AX211_RUNTIME_LTR_CONFIG_OPCODE            0xeeU
#define INTEL_AX211_RUNTIME_TEMP_REPORT_OPCODE           0x04U
#define INTEL_AX211_RUNTIME_POWER_TABLE_OPCODE           0x77U
#define INTEL_AX211_RUNTIME_MCC_UPDATE_OPCODE            0xc8U
#define INTEL_AX211_RUNTIME_BEACON_FILTER_OPCODE         0xd2U

#define INTEL_AX211_RUNTIME_TX_ANT_VERSION                   1U
#define INTEL_AX211_RUNTIME_BT_CONFIG_VERSION                6U
#define INTEL_AX211_RUNTIME_SOC_CONFIG_VERSION               2U
#define INTEL_AX211_RUNTIME_LTR_CONFIG_VERSION               3U
#define INTEL_AX211_RUNTIME_TEMP_REPORT_VERSION              1U
#define INTEL_AX211_RUNTIME_POWER_TABLE_VERSION              7U
#define INTEL_AX211_RUNTIME_MCC_UPDATE_VERSION               1U
#define INTEL_AX211_RUNTIME_MCC_RESPONSE_VERSION             6U
#define INTEL_AX211_RUNTIME_BEACON_FILTER_VERSION            4U

#define INTEL_AX211_RUNTIME_TX_ANT_SIZE                       4U
#define INTEL_AX211_RUNTIME_BT_CONFIG_SIZE                    8U
#define INTEL_AX211_RUNTIME_SOC_CONFIG_SIZE                   8U
#define INTEL_AX211_RUNTIME_LTR_CONFIG_SIZE                  32U
#define INTEL_AX211_RUNTIME_TEMP_REPORT_SIZE                 20U
#define INTEL_AX211_RUNTIME_POWER_TABLE_SIZE                  4U
#define INTEL_AX211_RUNTIME_MCC_UPDATE_SIZE                  28U
#define INTEL_AX211_RUNTIME_BEACON_FILTER_SIZE               60U

#define INTEL_AX211_RUNTIME_API_WIFI_MCC_UPDATE               9U
#define INTEL_AX211_RUNTIME_API_REDUCED_SCAN_CONFIG          56U
#define INTEL_AX211_RUNTIME_API_SCAN_EXT_CHANNEL             58U
#define INTEL_AX211_RUNTIME_CAP_DS_PARAM_SET_IE               9U
#define INTEL_AX211_RUNTIME_CAP_DQA                           12U
#define INTEL_AX211_RUNTIME_CAP_LAR_MULTI_MCC                29U
#define INTEL_AX211_RUNTIME_CAP_SET_LTR_GEN2                 50U
#define INTEL_AX211_RUNTIME_CAP_CT_KILL_BY_FW                74U
#define INTEL_AX211_RUNTIME_CAP_MCC_UPDATE_11AX              89U

#define INTEL_AX211_RUNTIME_SOC_CONFIG_FLAGS               0x0aU
#define INTEL_AX211_RUNTIME_SOC_CONFIG_XTAL_LATENCY        12000U

#define INTEL_AX211_RUNTIME_MCC_CHANNEL_LIMIT               110U
#define INTEL_AX211_RUNTIME_MCC_STATUS_MAX                    8U

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

enum intel_ax211_runtime_step {
	INTEL_AX211_RUNTIME_STEP_TX_ANT = 0,
	INTEL_AX211_RUNTIME_STEP_BT_CONFIG = 1,
	INTEL_AX211_RUNTIME_STEP_SOC_CONFIG = 2,
	INTEL_AX211_RUNTIME_STEP_LTR_CONFIG = 3,
	INTEL_AX211_RUNTIME_STEP_TEMP_REPORT = 4,
	INTEL_AX211_RUNTIME_STEP_POWER_TABLE = 5,
	INTEL_AX211_RUNTIME_STEP_MCC_UPDATE = 6,
	INTEL_AX211_RUNTIME_STEP_SCAN_CONFIG = 7,
	INTEL_AX211_RUNTIME_STEP_BEACON_FILTER = 8,
	INTEL_AX211_RUNTIME_STEP_DONE = 9
};

struct intel_ax211_runtime_profile {
	uint8_t tx_chain_mask;
	uint8_t rx_chain_mask;
	uint8_t lar_enabled;
	uint8_t ltr_enabled;
	uint32_t api_changes[4];
	uint32_t capabilities[5];
};

struct intel_ax211_runtime_command {
	uint8_t group;
	uint8_t opcode;
	uint8_t wire_version;
	uint8_t layout_version;
	uint8_t response_version;
	uint8_t payload[INTEL_AX211_RUNTIME_PAYLOAD_MAX];
	size_t payload_length;
};

struct intel_ax211_runtime_mcc {
	uint32_t status;
	uint16_t mcc;
	uint16_t capabilities;
	uint16_t time;
	uint16_t geographic_info;
	uint8_t source;
	uint32_t channel_count;
	uint32_t channel[INTEL_AX211_RUNTIME_MCC_CHANNEL_LIMIT];
};

struct intel_ax211_runtime_state {
	struct intel_ax211_runtime_profile profile;
	uint32_t generation;
	uint64_t deadline;
	enum intel_ax211_runtime_step step;
	uint8_t active;
	uint8_t terminal;
};

int intel_ax211_runtime_profile_from_manifest(
	const struct intel_ax211_firmware_manifest *manifest,
	const struct intel_ax211_protocol_nvm *nvm, int ltr_enabled,
	struct intel_ax211_runtime_profile *profile);
int intel_ax211_runtime_api89_validate(
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_runtime_profile *profile);

int intel_ax211_runtime_command_encode(
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_profile *profile,
	struct intel_ax211_runtime_command *command);
int intel_ax211_runtime_mcc_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation, struct intel_ax211_runtime_mcc *mcc);

int intel_ax211_runtime_begin(struct intel_ax211_runtime_state *state,
	const struct intel_ax211_protocol_command_table *table,
	const struct intel_ax211_runtime_profile *profile,
	uint32_t generation, uint64_t now_us);
int intel_ax211_runtime_current(
	const struct intel_ax211_runtime_state *state, uint64_t now_us,
	struct intel_ax211_runtime_command *command);
int intel_ax211_runtime_ack(struct intel_ax211_runtime_state *state,
	uint32_t generation, enum intel_ax211_runtime_step step,
	uint64_t now_us);
int intel_ax211_runtime_expire(struct intel_ax211_runtime_state *state,
	uint64_t now_us);

#endif
