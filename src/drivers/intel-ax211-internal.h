/*
 * zedBSD Intel AX211 private core contract
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
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_INTERNAL_H
#define ZEDBSD_DRIVERS_INTEL_AX211_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#define INTEL_AX211_PCI_VENDOR_ID          0x8086U
#define INTEL_AX211_PCI_DEVICE_ID          0x51f0U
#define INTEL_AX211_PCI_SUBVENDOR_ID       0x8086U
#define INTEL_AX211_PCI_SUBDEVICE_ID       0x4090U
#define INTEL_AX211_PCI_REVISION             0x01U
#define INTEL_AX211_MAC_TYPE_SO                0x37U
#define INTEL_AX211_MAC_TYPE_SOF               0x43U
#define INTEL_AX211_CRF_ID                0x401410U
#define INTEL_AX211_CNV_ID                 0x80400U
#define INTEL_AX211_WFPM_ID             0x80000020U
#define INTEL_AX211_RAW_RF_ID            0x2010d000U
#define INTEL_AX211_RF_TYPE                  0x10dU
#define INTEL_AX211_RF_CDB                        0U
#define INTEL_AX211_RF_JACKET                     1U

#define INTEL_AX211_FIRMWARE_PATH \
	"intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode"
#define INTEL_AX211_FIRMWARE_SIZE          1736748U
#define INTEL_AX211_FIRMWARE_SHA256 \
	"c569c4b0ffe2054a1cedd5affccff2da8515325eeb23f788c7abe9463d1a1514"
#define INTEL_AX211_FIRMWARE_VERSION       "89.735b75a4.0"
#define INTEL_AX211_FIRMWARE_API                 89U
#define INTEL_AX211_FIRMWARE_MINOR       0x735b75a4U
#define INTEL_AX211_FIRMWARE_SERIAL               0U

#define INTEL_AX211_PNVM_PATH \
	"intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm"
#define INTEL_AX211_PNVM_SIZE                    55176U
#define INTEL_AX211_PNVM_SHA256 \
	"efa9726d4a9d44b83fc9a14cedcf306a4e439e9de919802eb9e92df4ec032b2a"

#define INTEL_AX211_LINUX_FIRMWARE_TAG       "20260410"
#define INTEL_AX211_LINUX_FIRMWARE_COMMIT \
	"dc85ccedc9c973682fbcf4d628ca61174bcc3120"

#define INTEL_AX211_TLV_MAGIC                0x0a4c5749U
#define INTEL_AX211_TLV_HEADER_SIZE                   88U
#define INTEL_AX211_TLV_RECORD_HEADER_SIZE             8U
#define INTEL_AX211_MAX_FW_SECTIONS                    69U
#define INTEL_AX211_MAX_PNVM_SECTIONS                  64U
#define INTEL_AX211_COMMAND_RING_SIZE                 256U
#define INTEL_AX211_RX_RING_SIZE                      512U
#define INTEL_AX211_COMMAND_RING_CB_SIZE                 5U
#define INTEL_AX211_RX_RING_CB_SIZE                      9U
#define INTEL_AX211_MAX_COMMAND_PAYLOAD              4088U
#define INTEL_AX211_CONTEXT_INFO_GEN3_SIZE            104U
#define INTEL_AX211_RX_TRANSFER_DESCRIPTOR_SIZE        16U
#define INTEL_AX211_RX_COMPLETION_DESCRIPTOR_SIZE      32U
#define INTEL_AX211_TFD_SIZE                          256U
#define INTEL_AX211_TFD_MAX_BUFFERS                    25U
#define INTEL_AX211_TFD_BUFFER_MAX_LENGTH            4092U
#define INTEL_AX211_NARROW_COMMAND_HEADER_SIZE          4U
#define INTEL_AX211_WIDE_COMMAND_HEADER_SIZE            8U
#define INTEL_AX211_EVENT_HEADER_SIZE                   8U
#define INTEL_AX211_STAGING_CAPACITY                  4096U

#define INTEL_AX211_CPU1_CPU2_SEPARATOR        0xffffccccU
#define INTEL_AX211_PAGING_SEPARATOR           0xaaaabbbbU
#define INTEL_AX211_PNVM_SEPARATOR             0xddddeeeeU

enum intel_ax211_result {
	INTEL_AX211_OK = 0,
	INTEL_AX211_INVALID = 1,
	INTEL_AX211_TRUNCATED = 2,
	INTEL_AX211_OVERFLOW = 3,
	INTEL_AX211_DUPLICATE = 4,
	INTEL_AX211_UNSUPPORTED = 5,
	INTEL_AX211_MISSING = 6,
	INTEL_AX211_IDENTITY_MISMATCH = 7,
	INTEL_AX211_FULL = 8,
	INTEL_AX211_STALE = 9
};

struct intel_ax211_identity {
	uint16_t vendor;
	uint16_t device;
	uint16_t subvendor;
	uint16_t subdevice;
	uint8_t revision;
};

struct intel_ax211_section {
	uint32_t destination;
	size_t file_offset;
	size_t length;
};

struct intel_ax211_firmware_manifest {
	uint32_t header_version;
	uint32_t build;
	uint32_t api_major;
	uint32_t api_minor;
	uint32_t api_serial;
	uint32_t phy_sku;
	uint32_t cpu_count;
	size_t iml_offset;
	size_t iml_length;
	struct intel_ax211_section runtime[INTEL_AX211_MAX_FW_SECTIONS];
	size_t runtime_count;
	size_t lmac_count;
	size_t umac_count;
	size_t paging_count;
	uint32_t api_changes[4];
	uint32_t capabilities[5];
};

struct intel_ax211_sku_id {
	uint32_t data[3];
};

struct intel_ax211_pnvm_manifest {
	struct intel_ax211_sku_id sku;
	uint32_t version;
	uint16_t mac_type;
	uint16_t rf_id;
	struct intel_ax211_section section[INTEL_AX211_MAX_PNVM_SECTIONS];
	size_t section_count;
	size_t total_length;
};

struct intel_ax211_pnvm_inventory {
	size_t sku_count;
	size_t version_count;
	size_t hardware_type_count;
	size_t supported_hardware_type_count;
	size_t section_count;
	size_t total_section_length;
};

struct intel_ax211_context_info_gen3 {
	uint16_t version;
	uint32_t config;
	uint64_t prph_info_base;
	uint64_t cr_head_index_base;
	uint64_t tr_tail_index_base;
	uint64_t cr_tail_index_base;
	uint64_t tr_head_index_base;
	uint16_t cr_index_count;
	uint16_t tr_index_count;
	uint64_t command_transfer_ring_base;
	uint64_t command_completion_ring_base;
	uint16_t command_transfer_ring_size;
	uint16_t command_completion_ring_size;
	uint16_t command_transfer_doorbell;
	uint16_t command_completion_doorbell;
	uint16_t command_transfer_msi;
	uint16_t command_completion_msi;
	uint8_t transfer_header_dwords;
	uint8_t transfer_footer_dwords;
	uint8_t completion_header_dwords;
	uint8_t completion_footer_dwords;
	uint16_t message_ring_flags;
	uint16_t prph_info_msi;
	uint64_t prph_scratch_base;
	uint32_t prph_scratch_size;
};

struct intel_ax211_command_id {
	uint8_t opcode;
	uint8_t group;
	uint8_t version;
};

struct intel_ax211_tfd_buffer {
	uint64_t address;
	uint16_t length;
};

struct intel_ax211_event {
	struct intel_ax211_command_id command;
	uint8_t flags;
	uint8_t index;
	uint8_t queue;
	uint8_t rx_queue;
	size_t payload_offset;
	size_t payload_length;
};

struct intel_ax211_ring {
	uint16_t capacity;
	uint16_t head;
	uint16_t tail;
	uint16_t used;
	uint8_t queue;
};

struct intel_ax211_ring_token {
	uint8_t queue;
	uint8_t index;
};

struct intel_ax211_staging {
	uint8_t bytes[INTEL_AX211_STAGING_CAPACITY];
	size_t length;
};

int intel_ax211_identity_matches(const struct intel_ax211_identity *identity);
int intel_ax211_mac_type_supported(uint16_t mac_type);
int intel_ax211_firmware_parse(const uint8_t *bytes, size_t length,
	struct intel_ax211_firmware_manifest *manifest);
int intel_ax211_sku_equal(const struct intel_ax211_sku_id *left,
	const struct intel_ax211_sku_id *right);
int intel_ax211_pnvm_parse(const uint8_t *bytes, size_t length,
	const struct intel_ax211_sku_id *sku, uint16_t mac_type, uint16_t rf_id,
	struct intel_ax211_pnvm_manifest *manifest);
int intel_ax211_pnvm_inspect(const uint8_t *bytes, size_t length,
	struct intel_ax211_pnvm_inventory *inventory);

int intel_ax211_context_info_gen3_encode(uint8_t output[104],
	const struct intel_ax211_context_info_gen3 *context);
int intel_ax211_rx_transfer_descriptor_encode(uint8_t output[16],
	uint16_t buffer_id, uint64_t address);
int intel_ax211_rx_completion_descriptor_decode(const uint8_t input[32],
	uint16_t *buffer_id, uint8_t *flags);
int intel_ax211_tfd_encode(uint8_t output[256],
	const struct intel_ax211_tfd_buffer *buffers, size_t buffer_count);
int intel_ax211_narrow_command_encode(uint8_t output[4], uint8_t opcode,
	uint8_t flags, const struct intel_ax211_ring_token *token);
int intel_ax211_wide_command_encode(uint8_t output[8],
	const struct intel_ax211_command_id *command, uint16_t payload_length,
	const struct intel_ax211_ring_token *token);
int intel_ax211_event_decode(const uint8_t *bytes, size_t length,
	struct intel_ax211_event *event);

int intel_ax211_ring_init(struct intel_ax211_ring *ring, uint8_t queue,
	uint16_t capacity);
int intel_ax211_ring_reserve(struct intel_ax211_ring *ring,
	struct intel_ax211_ring_token *token);
int intel_ax211_ring_complete(struct intel_ax211_ring *ring,
	const struct intel_ax211_ring_token *token);
size_t intel_ax211_ring_available(const struct intel_ax211_ring *ring);

void intel_ax211_scrub(void *memory, size_t length);
int intel_ax211_staging_set(struct intel_ax211_staging *staging,
	const void *data, size_t length);
void intel_ax211_staging_clear(struct intel_ax211_staging *staging);

#endif
