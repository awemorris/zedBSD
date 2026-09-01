/*
 * zedBSD Intel AX211 firmware and descriptor core
 *
 * Wire formats and constants are derived from the OpenBSD iwx(4) driver.
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

#include "intel-ax211-internal.h"

#include <string.h>

enum {
	AX211_TLV_PROBE_MAX_LEN = 6,
	AX211_TLV_PAN = 7,
	AX211_TLV_FLAGS = 18,
	AX211_TLV_SEC_RT = 19,
	AX211_TLV_SEC_INIT = 20,
	AX211_TLV_SEC_WOWLAN = 21,
	AX211_TLV_DEF_CALIB = 22,
	AX211_TLV_PHY_SKU = 23,
	AX211_TLV_NUM_OF_CPU = 27,
	AX211_TLV_CSCHEME = 28,
	AX211_TLV_API_CHANGES = 29,
	AX211_TLV_CAPABILITIES = 30,
	AX211_TLV_SCAN_CHANNELS = 31,
	AX211_TLV_PAGING = 32,
	AX211_TLV_SEC_RT_USNIFFER = 34,
	AX211_TLV_SDIO_ADMA = 35,
	AX211_TLV_FW_VERSION = 36,
	AX211_TLV_DEBUG_DEST = 38,
	AX211_TLV_DEBUG_CONF = 39,
	AX211_TLV_DEBUG_TRIGGER = 40,
	AX211_TLV_CMD_VERSIONS = 48,
	AX211_TLV_GSCAN_CAPA = 50,
	AX211_TLV_FW_MEM_SEG = 51,
	AX211_TLV_IML = 52,
	AX211_TLV_FMAC_API = 53,
	AX211_TLV_UMAC_DEBUG = 54,
	AX211_TLV_LMAC_DEBUG = 55,
	AX211_TLV_RECOVERY = 57,
	AX211_TLV_HW_TYPE = 58,
	AX211_TLV_FMAC_RECOVERY = 59,
	AX211_TLV_FSEQ_VERSION = 60,
	AX211_TLV_PHY_INTEGRATION = 61,
	AX211_TLV_PNVM_VERSION = 62,
	AX211_TLV_PNVM_SKU = 64,
	AX211_TLV_SEC_TABLE = 66,
	AX211_TLV_D3_KEYS = 67,
	AX211_TLV_CURRENT_PC = 68,
	AX211_TLV_PNVM_DATA = 74,
	AX211_TLV_FW_NUM_STATIONS = 0x100,
	AX211_TLV_FW_NUM_BEACONS = 0x102,
	AX211_TLV_DEBUG_BASE = 0x1000005,
	AX211_TLV_DEBUG_LAST = 0x100000a
};

static uint16_t
ax211_get_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

static uint32_t
ax211_get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
ax211_put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
ax211_put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void
ax211_put_le64(uint8_t *bytes, uint64_t value)
{
	unsigned int index;

	for (index = 0; index < 8; index++)
		bytes[index] = (uint8_t)(value >> (index * 8));
}

static int
ax211_tlv_span(uint32_t value, size_t remaining, size_t *span)
{
	size_t length = (size_t)value;

	if (length > SIZE_MAX - 3U)
		return INTEL_AX211_OVERFLOW;
	length = (length + 3U) & ~(size_t)3U;
	if (length > remaining)
		return INTEL_AX211_TRUNCATED;
	*span = length;
	return INTEL_AX211_OK;
}

static int
ax211_ignored_firmware_tlv(uint32_t type)
{
	if ((type >= 1U && type <= 18U) ||
	    (type >= AX211_TLV_SEC_WOWLAN && type <= AX211_TLV_DEF_CALIB) ||
	    (type >= AX211_TLV_CSCHEME && type <= AX211_TLV_SCAN_CHANNELS) ||
	    type == AX211_TLV_PAGING || type == AX211_TLV_SEC_RT_USNIFFER ||
	    type == AX211_TLV_SDIO_ADMA || type == AX211_TLV_DEBUG_DEST ||
	    type == AX211_TLV_DEBUG_CONF || type == AX211_TLV_DEBUG_TRIGGER ||
	    (type >= AX211_TLV_GSCAN_CAPA && type <= AX211_TLV_LMAC_DEBUG) ||
	    (type >= AX211_TLV_RECOVERY && type <= AX211_TLV_PHY_INTEGRATION) ||
	    type == AX211_TLV_PNVM_VERSION || type == AX211_TLV_PNVM_SKU ||
	    type == 65U || (type >= AX211_TLV_SEC_TABLE && type <= 69U) ||
	    type == AX211_TLV_PNVM_DATA || type == AX211_TLV_FW_NUM_STATIONS ||
	    type == 0x101U || type == AX211_TLV_FW_NUM_BEACONS ||
	    (type >= 0x1000000U && type <= 0x1000004U) ||
	    (type >= AX211_TLV_DEBUG_BASE && type <= AX211_TLV_DEBUG_LAST) ||
	    type == 0x100000bU || type == 0x100000cU || type == 1092U)
		return 1;
	return 0;
}

int
intel_ax211_identity_matches(const struct intel_ax211_identity *identity)
{
	if (identity == NULL)
		return 0;
	return identity->vendor == INTEL_AX211_PCI_VENDOR_ID &&
	    identity->device == INTEL_AX211_PCI_DEVICE_ID &&
	    identity->subvendor == INTEL_AX211_PCI_SUBVENDOR_ID &&
	    identity->subdevice == INTEL_AX211_PCI_SUBDEVICE_ID &&
	    identity->revision == INTEL_AX211_PCI_REVISION;
}

int
intel_ax211_mac_type_supported(uint16_t mac_type)
{
	return mac_type == INTEL_AX211_MAC_TYPE_SO ||
	    mac_type == INTEL_AX211_MAC_TYPE_SOF;
}

int
intel_ax211_firmware_parse(const uint8_t *bytes, size_t length,
	struct intel_ax211_firmware_manifest *manifest)
{
	struct intel_ax211_firmware_manifest parsed;
	size_t offset, span;
	size_t cpu_separator = SIZE_MAX;
	size_t paging_separator = SIZE_MAX;
	uint32_t seen = 0;
	uint8_t api_seen = 0;
	uint8_t capa_seen = 0;
	size_t init_sections = 0;
	size_t wow_sections = 0;

	if (bytes == NULL || manifest == NULL)
		return INTEL_AX211_INVALID;
	if (length < INTEL_AX211_TLV_HEADER_SIZE)
		return INTEL_AX211_TRUNCATED;
	if (ax211_get_le32(bytes) != 0U ||
	    ax211_get_le32(bytes + 4U) != INTEL_AX211_TLV_MAGIC)
		return INTEL_AX211_INVALID;

	memset(&parsed, 0, sizeof(parsed));
	parsed.header_version = ax211_get_le32(bytes + 72U);
	parsed.build = ax211_get_le32(bytes + 76U);
	offset = INTEL_AX211_TLV_HEADER_SIZE;
	while (offset < length) {
		uint32_t type, tlv_length;
		const uint8_t *data;
		int result;

		if (length - offset < INTEL_AX211_TLV_RECORD_HEADER_SIZE)
			return INTEL_AX211_TRUNCATED;
		type = ax211_get_le32(bytes + offset);
		tlv_length = ax211_get_le32(bytes + offset + 4U);
		offset += INTEL_AX211_TLV_RECORD_HEADER_SIZE;
		result = ax211_tlv_span(tlv_length, length - offset, &span);
		if (result != INTEL_AX211_OK)
			return result;
		data = bytes + offset;

		switch (type) {
		case AX211_TLV_SEC_RT: {
			struct intel_ax211_section *section;
			uint32_t destination;
			int separator;

			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			if (parsed.runtime_count >= INTEL_AX211_MAX_FW_SECTIONS)
				return INTEL_AX211_OVERFLOW;
			destination = ax211_get_le32(data);
			separator = 0;
			if (destination == INTEL_AX211_CPU1_CPU2_SEPARATOR) {
				if (tlv_length != 8U || ax211_get_le32(data + 4U) != 0U)
					return INTEL_AX211_INVALID;
				if (cpu_separator != SIZE_MAX)
					return INTEL_AX211_DUPLICATE;
				cpu_separator = parsed.runtime_count;
				separator = 1;
			} else if (destination == INTEL_AX211_PAGING_SEPARATOR) {
				if (tlv_length != 8U || ax211_get_le32(data + 4U) != 0U)
					return INTEL_AX211_INVALID;
				if (paging_separator != SIZE_MAX)
					return INTEL_AX211_DUPLICATE;
				paging_separator = parsed.runtime_count;
				separator = 1;
			} else if (tlv_length == 4U) {
				return INTEL_AX211_INVALID;
			}
			section = &parsed.runtime[parsed.runtime_count++];
			section->destination = destination;
			section->file_offset = offset + 4U;
			section->length = separator ? 0U :
			    (size_t)tlv_length - 4U;
			break;
		}
		case AX211_TLV_SEC_INIT:
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			if (++init_sections > INTEL_AX211_MAX_FW_SECTIONS)
				return INTEL_AX211_OVERFLOW;
			break;
		case AX211_TLV_SEC_WOWLAN:
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			if (++wow_sections > INTEL_AX211_MAX_FW_SECTIONS)
				return INTEL_AX211_OVERFLOW;
			break;
		case AX211_TLV_PHY_SKU:
			if ((seen & 1U) != 0U)
				return INTEL_AX211_DUPLICATE;
			if (tlv_length != 4U)
				return INTEL_AX211_INVALID;
			seen |= 1U;
			parsed.phy_sku = ax211_get_le32(data);
			break;
		case AX211_TLV_NUM_OF_CPU:
			if ((seen & 2U) != 0U)
				return INTEL_AX211_DUPLICATE;
			if (tlv_length != 4U)
				return INTEL_AX211_INVALID;
			seen |= 2U;
			parsed.cpu_count = ax211_get_le32(data);
			if (parsed.cpu_count == 0U || parsed.cpu_count > 2U)
				return INTEL_AX211_INVALID;
			break;
		case AX211_TLV_API_CHANGES: {
			uint32_t index;
			if (tlv_length != 8U)
				return INTEL_AX211_INVALID;
			index = ax211_get_le32(data);
			if (index >= 4U)
				return INTEL_AX211_OVERFLOW;
			if ((api_seen & (uint8_t)(1U << index)) != 0U)
				return INTEL_AX211_DUPLICATE;
			api_seen |= (uint8_t)(1U << index);
			parsed.api_changes[index] = ax211_get_le32(data + 4U);
			break;
		}
		case AX211_TLV_CAPABILITIES: {
			uint32_t index;
			if (tlv_length != 8U)
				return INTEL_AX211_INVALID;
			index = ax211_get_le32(data);
			if (index >= 5U)
				return INTEL_AX211_OVERFLOW;
			if ((capa_seen & (uint8_t)(1U << index)) != 0U)
				return INTEL_AX211_DUPLICATE;
			capa_seen |= (uint8_t)(1U << index);
			parsed.capabilities[index] = ax211_get_le32(data + 4U);
			break;
		}
		case AX211_TLV_FW_VERSION:
			if ((seen & 4U) != 0U)
				return INTEL_AX211_DUPLICATE;
			if (tlv_length != 12U)
				return INTEL_AX211_INVALID;
			seen |= 4U;
			parsed.api_major = ax211_get_le32(data);
			parsed.api_minor = ax211_get_le32(data + 4U);
			parsed.api_serial = ax211_get_le32(data + 8U);
			break;
		case AX211_TLV_IML:
			if ((seen & 8U) != 0U)
				return INTEL_AX211_DUPLICATE;
			if (tlv_length == 0U)
				return INTEL_AX211_INVALID;
			seen |= 8U;
			parsed.iml_offset = offset;
			parsed.iml_length = tlv_length;
			break;
		case AX211_TLV_CMD_VERSIONS:
			if ((seen & 16U) != 0U)
				return INTEL_AX211_DUPLICATE;
			seen |= 16U;
			break;
		default:
			if (!ax211_ignored_firmware_tlv(type))
				return INTEL_AX211_UNSUPPORTED;
			break;
		}
		offset += span;
	}

	if ((seen & 15U) != 15U || parsed.runtime_count == 0U ||
	    cpu_separator == SIZE_MAX || paging_separator == SIZE_MAX)
		return INTEL_AX211_MISSING;
	if (cpu_separator == 0U || paging_separator <= cpu_separator + 1U ||
	    paging_separator + 1U >= parsed.runtime_count)
		return INTEL_AX211_MISSING;
	if (parsed.api_major != INTEL_AX211_FIRMWARE_API ||
	    parsed.api_minor != INTEL_AX211_FIRMWARE_MINOR ||
	    parsed.api_serial != INTEL_AX211_FIRMWARE_SERIAL)
		return INTEL_AX211_IDENTITY_MISMATCH;
	parsed.lmac_count = cpu_separator;
	parsed.umac_count = paging_separator - cpu_separator - 1U;
	parsed.paging_count = parsed.runtime_count - paging_separator - 1U;
	*manifest = parsed;
	return INTEL_AX211_OK;
}

int
intel_ax211_sku_equal(const struct intel_ax211_sku_id *left,
	const struct intel_ax211_sku_id *right)
{
	if (left == NULL || right == NULL)
		return 0;
	return left->data[0] == right->data[0] &&
	    left->data[1] == right->data[1] &&
	    left->data[2] == right->data[2];
}

static int
ax211_pnvm_finish(const struct intel_ax211_pnvm_manifest *candidate,
	int active, int version_seen, int hardware_match, uint16_t mac_type,
	uint16_t rf_id, struct intel_ax211_pnvm_manifest *manifest)
{
	if (!active)
		return INTEL_AX211_MISSING;
	if (!version_seen || candidate->section_count == 0U)
		return INTEL_AX211_MISSING;
	if (!hardware_match || candidate->mac_type != mac_type ||
	    candidate->rf_id != rf_id)
		return INTEL_AX211_IDENTITY_MISMATCH;
	*manifest = *candidate;
	return INTEL_AX211_OK;
}

int
intel_ax211_pnvm_parse(const uint8_t *bytes, size_t length,
	const struct intel_ax211_sku_id *sku, uint16_t mac_type, uint16_t rf_id,
	struct intel_ax211_pnvm_manifest *manifest)
{
	struct intel_ax211_pnvm_manifest candidate;
	size_t offset = 0;
	int active = 0;
	int version_seen = 0;
	int hardware_match = 0;

	if (bytes == NULL || sku == NULL || manifest == NULL)
		return INTEL_AX211_INVALID;
	memset(&candidate, 0, sizeof(candidate));
	while (offset < length) {
		uint32_t type, tlv_length;
		size_t span;
		const uint8_t *data;
		int result;

		if (length - offset < INTEL_AX211_TLV_RECORD_HEADER_SIZE)
			return INTEL_AX211_TRUNCATED;
		type = ax211_get_le32(bytes + offset);
		tlv_length = ax211_get_le32(bytes + offset + 4U);
		offset += INTEL_AX211_TLV_RECORD_HEADER_SIZE;
		result = ax211_tlv_span(tlv_length, length - offset, &span);
		if (result != INTEL_AX211_OK)
			return result;
		data = bytes + offset;

		if (type == AX211_TLV_PNVM_SKU) {
			struct intel_ax211_sku_id found;

			if (tlv_length != 12U)
				return INTEL_AX211_INVALID;
			if (active) {
				result = ax211_pnvm_finish(&candidate, active,
				    version_seen, hardware_match, mac_type, rf_id,
				    manifest);
				if (result == INTEL_AX211_OK)
					return result;
				if (result != INTEL_AX211_IDENTITY_MISMATCH)
					return result;
			}
			found.data[0] = ax211_get_le32(data);
			found.data[1] = ax211_get_le32(data + 4U);
			found.data[2] = ax211_get_le32(data + 8U);
			active = intel_ax211_sku_equal(&found, sku);
			version_seen = 0;
			hardware_match = 0;
			memset(&candidate, 0, sizeof(candidate));
			candidate.sku = found;
		} else if (type == AX211_TLV_PNVM_VERSION) {
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			if (active) {
				if (version_seen)
					return INTEL_AX211_DUPLICATE;
				version_seen = 1;
				candidate.version = ax211_get_le32(data);
			}
		} else if (type == AX211_TLV_HW_TYPE) {
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			if (active) {
				uint16_t found_mac = ax211_get_le16(data);
				uint16_t found_rf = ax211_get_le16(data + 2U);
				if (found_mac == mac_type && found_rf == rf_id) {
					hardware_match = 1;
					candidate.mac_type = found_mac;
					candidate.rf_id = found_rf;
				}
			}
		} else if (type == AX211_TLV_SEC_RT) {
			uint32_t destination;
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			destination = ax211_get_le32(data);
			if (active && destination != INTEL_AX211_PNVM_SEPARATOR) {
				struct intel_ax211_section *section;
				size_t data_length = (size_t)tlv_length - 4U;
				if (data_length == 0U)
					return INTEL_AX211_INVALID;
				if (candidate.section_count >=
				    INTEL_AX211_MAX_PNVM_SECTIONS)
					return INTEL_AX211_OVERFLOW;
				if (candidate.total_length > SIZE_MAX - data_length)
					return INTEL_AX211_OVERFLOW;
				section = &candidate.section[candidate.section_count++];
				section->destination = destination;
				section->file_offset = offset + 4U;
				section->length = data_length;
				candidate.total_length += data_length;
			}
		} else {
			return INTEL_AX211_UNSUPPORTED;
		}
		offset += span;
	}
	return ax211_pnvm_finish(&candidate, active, version_seen,
	    hardware_match, mac_type, rf_id, manifest);
}

int
intel_ax211_pnvm_inspect(const uint8_t *bytes, size_t length,
	struct intel_ax211_pnvm_inventory *inventory)
{
	struct intel_ax211_pnvm_inventory found;
	size_t offset = 0;

	if (bytes == NULL || inventory == NULL)
		return INTEL_AX211_INVALID;
	memset(&found, 0, sizeof(found));
	while (offset < length) {
		uint32_t type, tlv_length;
		size_t span;
		const uint8_t *data;
		int result;

		if (length - offset < INTEL_AX211_TLV_RECORD_HEADER_SIZE)
			return INTEL_AX211_TRUNCATED;
		type = ax211_get_le32(bytes + offset);
		tlv_length = ax211_get_le32(bytes + offset + 4U);
		offset += INTEL_AX211_TLV_RECORD_HEADER_SIZE;
		result = ax211_tlv_span(tlv_length, length - offset, &span);
		if (result != INTEL_AX211_OK)
			return result;
		data = bytes + offset;
		if (type == AX211_TLV_PNVM_SKU) {
			if (tlv_length != 12U)
				return INTEL_AX211_INVALID;
			found.sku_count++;
		} else if (type == AX211_TLV_PNVM_VERSION) {
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			found.version_count++;
		} else if (type == AX211_TLV_HW_TYPE) {
			uint16_t mac_type, rf_type;
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			mac_type = ax211_get_le16(data);
			rf_type = ax211_get_le16(data + 2U);
			found.hardware_type_count++;
			if (intel_ax211_mac_type_supported(mac_type) &&
			    rf_type == INTEL_AX211_RF_TYPE)
				found.supported_hardware_type_count++;
		} else if (type == AX211_TLV_SEC_RT) {
			size_t section_length;
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			if (ax211_get_le32(data) != INTEL_AX211_PNVM_SEPARATOR) {
				section_length = (size_t)tlv_length - 4U;
				if (section_length == 0U)
					return INTEL_AX211_INVALID;
				if (found.total_section_length >
				    SIZE_MAX - section_length)
					return INTEL_AX211_OVERFLOW;
				found.total_section_length += section_length;
				found.section_count++;
			}
		} else {
			return INTEL_AX211_UNSUPPORTED;
		}
		offset += span;
	}
	if (found.sku_count == 0U || found.version_count == 0U ||
	    found.hardware_type_count == 0U || found.section_count == 0U)
		return INTEL_AX211_MISSING;
	*inventory = found;
	return INTEL_AX211_OK;
}

int
intel_ax211_context_info_gen3_encode(uint8_t output[104],
	const struct intel_ax211_context_info_gen3 *context)
{
	if (output == NULL || context == NULL)
		return INTEL_AX211_INVALID;
	if (context->command_transfer_ring_size !=
	    INTEL_AX211_COMMAND_RING_CB_SIZE ||
	    context->command_completion_ring_size !=
	    INTEL_AX211_RX_RING_CB_SIZE)
		return INTEL_AX211_INVALID;
	memset(output, 0, INTEL_AX211_CONTEXT_INFO_GEN3_SIZE);
	ax211_put_le16(output, context->version);
	ax211_put_le16(output + 2U,
	    INTEL_AX211_CONTEXT_INFO_GEN3_SIZE / 4U);
	ax211_put_le32(output + 4U, context->config);
	ax211_put_le64(output + 8U, context->prph_info_base);
	ax211_put_le64(output + 16U, context->cr_head_index_base);
	ax211_put_le64(output + 24U, context->tr_tail_index_base);
	ax211_put_le64(output + 32U, context->cr_tail_index_base);
	ax211_put_le64(output + 40U, context->tr_head_index_base);
	ax211_put_le16(output + 48U, context->cr_index_count);
	ax211_put_le16(output + 50U, context->tr_index_count);
	ax211_put_le64(output + 52U, context->command_transfer_ring_base);
	ax211_put_le64(output + 60U, context->command_completion_ring_base);
	ax211_put_le16(output + 68U, context->command_transfer_ring_size);
	ax211_put_le16(output + 70U, context->command_completion_ring_size);
	ax211_put_le16(output + 72U, context->command_transfer_doorbell);
	ax211_put_le16(output + 74U, context->command_completion_doorbell);
	ax211_put_le16(output + 76U, context->command_transfer_msi);
	ax211_put_le16(output + 78U, context->command_completion_msi);
	output[80] = context->transfer_header_dwords;
	output[81] = context->transfer_footer_dwords;
	output[82] = context->completion_header_dwords;
	output[83] = context->completion_footer_dwords;
	ax211_put_le16(output + 84U, context->message_ring_flags);
	ax211_put_le16(output + 86U, context->prph_info_msi);
	ax211_put_le64(output + 88U, context->prph_scratch_base);
	ax211_put_le32(output + 96U, context->prph_scratch_size);
	return INTEL_AX211_OK;
}

int
intel_ax211_rx_transfer_descriptor_encode(uint8_t output[16],
	uint16_t buffer_id, uint64_t address)
{
	if (output == NULL)
		return INTEL_AX211_INVALID;
	memset(output, 0, INTEL_AX211_RX_TRANSFER_DESCRIPTOR_SIZE);
	ax211_put_le16(output, buffer_id);
	ax211_put_le64(output + 8U, address);
	return INTEL_AX211_OK;
}

int
intel_ax211_rx_completion_descriptor_decode(const uint8_t input[32],
	uint16_t *buffer_id, uint8_t *flags)
{
	if (input == NULL || buffer_id == NULL || flags == NULL)
		return INTEL_AX211_INVALID;
	*buffer_id = ax211_get_le16(input + 4U);
	*flags = input[6];
	return INTEL_AX211_OK;
}

int
intel_ax211_tfd_encode(uint8_t output[256],
	const struct intel_ax211_tfd_buffer *buffers, size_t buffer_count)
{
	size_t index;

	if (output == NULL || buffers == NULL || buffer_count == 0U ||
	    buffer_count > INTEL_AX211_TFD_MAX_BUFFERS)
		return INTEL_AX211_INVALID;
	memset(output, 0, INTEL_AX211_TFD_SIZE);
	ax211_put_le16(output, (uint16_t)buffer_count);
	for (index = 0; index < buffer_count; index++) {
		size_t offset = 2U + index * 10U;
		if (buffers[index].length == 0U ||
		    buffers[index].length > INTEL_AX211_TFD_BUFFER_MAX_LENGTH) {
			intel_ax211_scrub(output, INTEL_AX211_TFD_SIZE);
			return INTEL_AX211_INVALID;
		}
		ax211_put_le16(output + offset, buffers[index].length);
		ax211_put_le64(output + offset + 2U, buffers[index].address);
	}
	return INTEL_AX211_OK;
}

int
intel_ax211_narrow_command_encode(uint8_t output[4], uint8_t opcode,
	uint8_t flags, const struct intel_ax211_ring_token *token)
{
	if (output == NULL || token == NULL)
		return INTEL_AX211_INVALID;
	output[0] = opcode;
	output[1] = flags;
	output[2] = token->index;
	output[3] = token->queue;
	return INTEL_AX211_OK;
}

int
intel_ax211_wide_command_encode(uint8_t output[8],
	const struct intel_ax211_command_id *command, uint16_t payload_length,
	const struct intel_ax211_ring_token *token)
{
	if (output == NULL || command == NULL || token == NULL ||
	    payload_length > INTEL_AX211_MAX_COMMAND_PAYLOAD)
		return INTEL_AX211_INVALID;
	output[0] = command->opcode;
	output[1] = command->group;
	output[2] = token->index;
	output[3] = token->queue;
	ax211_put_le16(output + 4U, payload_length);
	output[6] = 0;
	output[7] = command->version;
	return INTEL_AX211_OK;
}

int
intel_ax211_event_decode(const uint8_t *bytes, size_t length,
	struct intel_ax211_event *event)
{
	uint32_t length_flags;
	size_t frame_length;

	if (bytes == NULL || event == NULL)
		return INTEL_AX211_INVALID;
	if (length < INTEL_AX211_EVENT_HEADER_SIZE)
		return INTEL_AX211_TRUNCATED;
	length_flags = ax211_get_le32(bytes);
	frame_length = (size_t)(length_flags & 0x3fffU);
	if (frame_length < INTEL_AX211_NARROW_COMMAND_HEADER_SIZE)
		return INTEL_AX211_INVALID;
	if (frame_length > length - 4U)
		return INTEL_AX211_TRUNCATED;
	memset(event, 0, sizeof(*event));
	event->command.opcode = bytes[4];
	event->flags = bytes[5];
	event->index = bytes[6];
	event->queue = bytes[7];
	event->rx_queue = (uint8_t)((length_flags >> 16) & 0x3fU);
	event->payload_offset = INTEL_AX211_EVENT_HEADER_SIZE;
	event->payload_length = frame_length - 4U;
	return INTEL_AX211_OK;
}

int
intel_ax211_ring_init(struct intel_ax211_ring *ring, uint8_t queue,
	uint16_t capacity)
{
	if (ring == NULL || capacity == 0U ||
	    capacity > INTEL_AX211_COMMAND_RING_SIZE ||
	    (capacity & (uint16_t)(capacity - 1U)) != 0U)
		return INTEL_AX211_INVALID;
	memset(ring, 0, sizeof(*ring));
	ring->capacity = capacity;
	ring->queue = queue;
	return INTEL_AX211_OK;
}

int
intel_ax211_ring_reserve(struct intel_ax211_ring *ring,
	struct intel_ax211_ring_token *token)
{
	if (ring == NULL || token == NULL || ring->capacity == 0U)
		return INTEL_AX211_INVALID;
	if (ring->used == ring->capacity)
		return INTEL_AX211_FULL;
	token->queue = ring->queue;
	token->index = (uint8_t)ring->head;
	ring->head = (uint16_t)((ring->head + 1U) & (ring->capacity - 1U));
	ring->used++;
	return INTEL_AX211_OK;
}

int
intel_ax211_ring_complete(struct intel_ax211_ring *ring,
	const struct intel_ax211_ring_token *token)
{
	if (ring == NULL || token == NULL || ring->capacity == 0U)
		return INTEL_AX211_INVALID;
	if (ring->used == 0U || token->queue != ring->queue ||
	    token->index != (uint8_t)ring->tail)
		return INTEL_AX211_STALE;
	ring->tail = (uint16_t)((ring->tail + 1U) & (ring->capacity - 1U));
	ring->used--;
	return INTEL_AX211_OK;
}

size_t
intel_ax211_ring_available(const struct intel_ax211_ring *ring)
{
	if (ring == NULL || ring->capacity == 0U || ring->used > ring->capacity)
		return 0;
	return (size_t)(ring->capacity - ring->used);
}

void
intel_ax211_scrub(void *memory, size_t length)
{
	volatile uint8_t *bytes = (volatile uint8_t *)memory;

	if (memory == NULL)
		return;
	while (length-- != 0U)
		*bytes++ = 0;
}

int
intel_ax211_staging_set(struct intel_ax211_staging *staging,
	const void *data, size_t length)
{
	if (staging == NULL || (data == NULL && length != 0U))
		return INTEL_AX211_INVALID;
	intel_ax211_staging_clear(staging);
	if (length > sizeof(staging->bytes))
		return INTEL_AX211_OVERFLOW;
	if (length != 0U)
		memcpy(staging->bytes, data, length);
	staging->length = length;
	return INTEL_AX211_OK;
}

void
intel_ax211_staging_clear(struct intel_ax211_staging *staging)
{
	if (staging == NULL)
		return;
	intel_ax211_scrub(staging->bytes, sizeof(staging->bytes));
	staging->length = 0;
}
