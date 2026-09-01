/*
 * zedBSD Intel AX211 private firmware protocol contract
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

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_PROTOCOL_H
#define ZEDBSD_DRIVERS_INTEL_AX211_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define INTEL_AX211_PROTOCOL_UNKNOWN_VERSION            99U
#define INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK       0x40U
#define INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE   4U
#define INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT         217U
#define INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES         868U
#define INTEL_AX211_PROTOCOL_COMMAND_RESPONSE_MAX       4088U

#define INTEL_AX211_PROTOCOL_GROUP_LEGACY                 0U
#define INTEL_AX211_PROTOCOL_GROUP_LONG                   1U
#define INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM        12U

#define INTEL_AX211_PROTOCOL_ALIVE_OPCODE              0x01U
#define INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE      0x04U
#define INTEL_AX211_PROTOCOL_SCAN_CFG_OPCODE           0x0cU
#define INTEL_AX211_PROTOCOL_SCAN_REQ_OPCODE           0x0dU
#define INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE 0x00U
#define INTEL_AX211_PROTOCOL_NVM_GET_INFO_OPCODE       0x02U
#define INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_OPCODE 0xfeU

#define INTEL_AX211_PROTOCOL_ALIVE_VERSION                 6U
#define INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_VERSION    1U
#define INTEL_AX211_PROTOCOL_NVM_GET_INFO_VERSION          4U

#define INTEL_AX211_PROTOCOL_ALIVE_SIZE                  144U
#define INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE           468U
#define INTEL_AX211_PROTOCOL_NVM_CHANNEL_LIMIT           110U
#define INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT          14U

#define INTEL_AX211_PROTOCOL_ALIVE_STATUS_ERROR        0xdeadU
#define INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK           0xcafeU

#define INTEL_AX211_PROTOCOL_NVM_GENERAL_EMPTY_OTP       0x01U
#define INTEL_AX211_PROTOCOL_NVM_BAND_24_ENABLED         0x01U
#define INTEL_AX211_PROTOCOL_NVM_BAND_52_ENABLED         0x02U
#define INTEL_AX211_PROTOCOL_NVM_11N_ENABLED             0x04U
#define INTEL_AX211_PROTOCOL_NVM_11AC_ENABLED            0x08U
#define INTEL_AX211_PROTOCOL_NVM_11AX_ENABLED            0x10U
#define INTEL_AX211_PROTOCOL_NVM_MIMO_DISABLED           0x20U

#define INTEL_AX211_PROTOCOL_NVM_CHANNEL_VALID           0x01U
#define INTEL_AX211_PROTOCOL_NVM_CHANNEL_ACTIVE          0x08U

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

struct intel_ax211_protocol_command_version {
	uint8_t opcode;
	uint8_t group;
	uint8_t command_version;
	uint8_t notification_version;
};

struct intel_ax211_protocol_command_table {
	const uint8_t *bytes;
	size_t count;
};

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

struct intel_ax211_protocol_lmac_alive {
	uint32_t major;
	uint32_t minor;
	uint8_t version_subtype;
	uint8_t version_type;
	uint8_t mac;
	uint8_t option;
	uint32_t timestamp;
	uint32_t error_event_table;
	uint32_t log_event_table;
	uint32_t cpu_register;
	uint32_t debug_config;
	uint32_t alive_counter;
	uint32_t scheduler_base;
	uint32_t store_forward_address;
	uint32_t store_forward_size;
};

struct intel_ax211_protocol_umac_alive {
	uint32_t major;
	uint32_t minor;
	uint32_t error_info;
	uint32_t debug_print_buffer;
};

struct intel_ax211_protocol_alive {
	uint16_t status;
	uint16_t flags;
	struct intel_ax211_protocol_lmac_alive lmac[2];
	struct intel_ax211_protocol_umac_alive umac;
	uint32_t sku[3];
	uint64_t imr_base;
	uint32_t imr_size;
	uint32_t imr_enabled;
};

struct intel_ax211_protocol_channel {
	uint8_t number;
	uint8_t valid;
	uint8_t active;
	uint32_t flags;
};

struct intel_ax211_protocol_nvm {
	uint32_t general_flags;
	uint16_t nvm_version;
	uint8_t board_type;
	uint8_t hardware_address_count;
	uint32_t mac_sku_flags;
	uint8_t band_24_enabled;
	uint8_t band_52_enabled;
	uint8_t ht_enabled;
	uint8_t vht_enabled;
	uint8_t he_enabled;
	uint8_t mimo_disabled;
	uint8_t tx_chain_mask;
	uint8_t rx_chain_mask;
	uint8_t lar_enabled;
	uint32_t n_channels;
	struct intel_ax211_protocol_channel channel_24ghz[
	    INTEL_AX211_PROTOCOL_24GHZ_CHANNEL_LIMIT];
	size_t channel_24ghz_count;
	size_t valid_24ghz_count;
};

int intel_ax211_protocol_command_table_parse(const uint8_t *bytes,
	size_t length, struct intel_ax211_protocol_command_table *table);
int intel_ax211_protocol_command_version_lookup(
	const struct intel_ax211_protocol_command_table *table, uint8_t group,
	uint8_t opcode, struct intel_ax211_protocol_command_version *version);
int intel_ax211_protocol_command_table_validate_api89(
	const struct intel_ax211_protocol_command_table *table);

int intel_ax211_protocol_command_response_validate(
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending);

int intel_ax211_protocol_alive_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation, struct intel_ax211_protocol_alive *alive);
int intel_ax211_protocol_pnvm_init_complete(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation);
int intel_ax211_protocol_init_complete(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation);
int intel_ax211_protocol_nvm_get_info_decode(
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending,
	struct intel_ax211_protocol_nvm *nvm);

#endif
