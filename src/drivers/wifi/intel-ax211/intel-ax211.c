/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Begin consolidated intel-ax211.c. */
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

static uint16_t ax211_get_le16(const uint8_t *bytes);

/* Supports the ax211 get le16 operation. */
static uint16_t
ax211_get_le16(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t ax211_get_le32(const uint8_t *bytes);

/* Supports the ax211 get le32 operation. */
static uint32_t
ax211_get_le32(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void ax211_put_le16(uint8_t *bytes, uint16_t value);

/* Supports the ax211 put le16 operation. */
static void
ax211_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void ax211_put_le32(uint8_t *bytes, uint32_t value);

/* Supports the ax211 put le32 operation. */
static void
ax211_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void ax211_put_le64(uint8_t *bytes, uint64_t value);

/* Supports the ax211 put le64 operation. */
static void
ax211_put_le64(
	uint8_t *bytes,
	uint64_t value)
{
	unsigned int index;

	/* Process each remaining element. */
	for (index = 0; index < 8; index++)
		bytes[index] = (uint8_t)(value >> (index * 8));
}

static int ax211_tlv_span(uint32_t value, size_t remaining, size_t *span);

/* Supports the ax211 tlv span operation. */
static int
ax211_tlv_span(
	uint32_t value,
	size_t remaining,
	size_t *span)
{
	size_t length = (size_t)value;

	/* Checks the current data length. */
	if (length > SIZE_MAX - 3U)
		return INTEL_AX211_OVERFLOW;

	/* Checks the current data length. */
	length = (length + 3U) & ~(size_t)3U;
	if (length > remaining)
		return INTEL_AX211_TRUNCATED;
	*span = length;
	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

static int ax211_ignored_firmware_tlv(uint32_t type);

/* Supports the ax211 ignored firmware tlv operation. */
static int
ax211_ignored_firmware_tlv(
	uint32_t type)
{
	/* Handles the type condition. */
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
	    type == 0x100000bU || type == 0x100000cU || type == 1092U) {
		/* Reports operation failure. */
		return 1;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv intel ax211 identity matches operation.
 */
int
drv_intel_ax211_identity_matches(
	const struct intel_ax211_identity *identity)
{
	/* Handles the identity availability. */
	if (identity == NULL)
		return 0;

	/* Returns the computed result. */
	return identity->vendor == INTEL_AX211_PCI_VENDOR_ID &&
	       identity->device == INTEL_AX211_PCI_DEVICE_ID &&
	       identity->subvendor == INTEL_AX211_PCI_SUBVENDOR_ID &&
	       identity->subdevice == INTEL_AX211_PCI_SUBDEVICE_ID &&
	       identity->revision == INTEL_AX211_PCI_REVISION;
}

/*
 * Implements the drv intel ax211 mac type supported operation.
 */
int
drv_intel_ax211_mac_type_supported(
	uint16_t mac_type)
{
	/* Returns the computed result. */
	return mac_type == INTEL_AX211_MAC_TYPE_SO ||
	       mac_type == INTEL_AX211_MAC_TYPE_SOF;
}

/*
 * Implements the drv intel ax211 firmware parse operation.
 */
int
drv_intel_ax211_firmware_parse(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_firmware_manifest *manifest)
{
	uint32_t index_local;
	uint32_t index_local1;
	struct intel_ax211_section *section;
	uint32_t destination;
	int separator;
	uint32_t type, tlv_length;
	const uint8_t *data;
	int result;
	struct intel_ax211_firmware_manifest parsed;
	size_t offset, span;
	size_t cpu_separator = SIZE_MAX;
	size_t paging_separator = SIZE_MAX;
	uint32_t seen = 0;
	uint8_t api_seen = 0;
	uint8_t capa_seen = 0;
	size_t init_sections = 0;
	size_t wow_sections = 0;

	/* Handles the bytes availability. */
	if (bytes == NULL || manifest == NULL)
		return INTEL_AX211_INVALID;

	/* Checks the current data length. */
	if (length < INTEL_AX211_TLV_HEADER_SIZE)
		return INTEL_AX211_TRUNCATED;

	/* Checks the ax211 get le32 result. */
	if (ax211_get_le32(bytes) != 0U ||
	    ax211_get_le32(bytes + 4U) != INTEL_AX211_TLV_MAGIC) {
		/* Returns the computed result. */
		return INTEL_AX211_INVALID;
	}

	memset(&parsed, 0, sizeof(parsed));
	parsed.header_version = ax211_get_le32(bytes + 72U);
	parsed.build = ax211_get_le32(bytes + 76U);
	offset = INTEL_AX211_TLV_HEADER_SIZE;
	/* Process each remaining element. */
	while (offset < length) {
		/* Checks the current data length. */
		if (length - offset < INTEL_AX211_TLV_RECORD_HEADER_SIZE)
			return INTEL_AX211_TRUNCATED;
		type = ax211_get_le32(bytes + offset);
		tlv_length = ax211_get_le32(bytes + offset + 4U);
		offset += INTEL_AX211_TLV_RECORD_HEADER_SIZE;

		/* Checks the operation result. */
		result = ax211_tlv_span(tlv_length, length - offset, &span);
		if (result != INTEL_AX211_OK)
			return result;
		data = bytes + offset;

		/* Dispatch the selected syntax or record type. */
		switch (type) {
		case AX211_TLV_SEC_RT:

			/* Handles the tlv length condition. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Handles the parsed condition. */
			if (parsed.runtime_count >= INTEL_AX211_MAX_FW_SECTIONS)
				return INTEL_AX211_OVERFLOW;
			destination = ax211_get_le32(data);
			separator = 0;

			/* Handles the destination condition. */
			if (destination == INTEL_AX211_CPU1_CPU2_SEPARATOR) {
				/* Checks the ax211 get le32 result. */
				if (tlv_length != 8U ||
				    ax211_get_le32(data + 4U) != 0U) {
					/* Returns the computed result. */
					return INTEL_AX211_INVALID;
				}

				/* Handles the cpu separator condition. */
				if (cpu_separator != SIZE_MAX)
					return INTEL_AX211_DUPLICATE;
				cpu_separator = parsed.runtime_count;
				separator = 1;
			} else if (destination ==
				   INTEL_AX211_PAGING_SEPARATOR) {
				/* Checks the ax211 get le32 result. */
				if (tlv_length != 8U ||
				    ax211_get_le32(data + 4U) != 0U) {
					/* Returns the computed result. */
					return INTEL_AX211_INVALID;
				}

				/* Handles the paging separator condition. */
				if (paging_separator != SIZE_MAX)
					return INTEL_AX211_DUPLICATE;
				paging_separator = parsed.runtime_count;
				separator = 1;
			} else if (tlv_length == 4U) {
				/* Returns the computed result. */
				return INTEL_AX211_INVALID;
			}

			section = &parsed.runtime[parsed.runtime_count++];
			section->destination = destination;
			section->file_offset = offset + 4U;
			section->length =
				separator ? 0U : (size_t)tlv_length - 4U;
			break;
		case AX211_TLV_SEC_INIT:
			/* Handles the tlv length condition. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Handles the init sections condition. */
			if (++init_sections > INTEL_AX211_MAX_FW_SECTIONS)
				return INTEL_AX211_OVERFLOW;
			break;
		case AX211_TLV_SEC_WOWLAN:
			/* Handles the tlv length condition. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Handles the wow sections condition. */
			if (++wow_sections > INTEL_AX211_MAX_FW_SECTIONS)
				return INTEL_AX211_OVERFLOW;
			break;
		case AX211_TLV_PHY_SKU:
			/* Handles the seen condition. */
			if ((seen & 1U) != 0U)
				return INTEL_AX211_DUPLICATE;

			/* Handles the tlv length condition. */
			if (tlv_length != 4U)
				return INTEL_AX211_INVALID;
			seen |= 1U;
			parsed.phy_sku = ax211_get_le32(data);
			break;
		case AX211_TLV_NUM_OF_CPU:
			/* Handles the seen condition. */
			if ((seen & 2U) != 0U)
				return INTEL_AX211_DUPLICATE;

			/* Handles the tlv length condition. */
			if (tlv_length != 4U)
				return INTEL_AX211_INVALID;
			seen |= 2U;
			parsed.cpu_count = ax211_get_le32(data);

			/* Handles the parsed condition. */
			if (parsed.cpu_count == 0U || parsed.cpu_count > 2U)
				return INTEL_AX211_INVALID;
			break;
		case AX211_TLV_API_CHANGES:

			/* Handles the tlv length condition. */
			if (tlv_length != 8U)
				return INTEL_AX211_INVALID;

			/* Handles the index local condition. */
			index_local = ax211_get_le32(data);
			if (index_local >= 4U)
				return INTEL_AX211_OVERFLOW;

			/* Handles the api seen condition. */
			if ((api_seen & (uint8_t)(1U << index_local)) != 0U)
				return INTEL_AX211_DUPLICATE;
			api_seen |= (uint8_t)(1U << index_local);
			parsed.api_changes[index_local] =
				ax211_get_le32(data + 4U);
			break;
		case AX211_TLV_CAPABILITIES:

			/* Handles the tlv length condition. */
			if (tlv_length != 8U)
				return INTEL_AX211_INVALID;

			/* Handles the index local1 condition. */
			index_local1 = ax211_get_le32(data);
			if (index_local1 >= 5U)
				return INTEL_AX211_OVERFLOW;

			/* Handles the capa seen condition. */
			if ((capa_seen & (uint8_t)(1U << index_local1)) != 0U)
				return INTEL_AX211_DUPLICATE;
			capa_seen |= (uint8_t)(1U << index_local1);
			parsed.capabilities[index_local1] =
				ax211_get_le32(data + 4U);
			break;
		case AX211_TLV_FW_VERSION:
			/* Handles the seen condition. */
			if ((seen & 4U) != 0U)
				return INTEL_AX211_DUPLICATE;

			/* Handles the tlv length condition. */
			if (tlv_length != 12U)
				return INTEL_AX211_INVALID;
			seen |= 4U;
			parsed.api_major = ax211_get_le32(data);
			parsed.api_minor = ax211_get_le32(data + 4U);
			parsed.api_serial = ax211_get_le32(data + 8U);
			break;
		case AX211_TLV_IML:
			/* Handles the seen condition. */
			if ((seen & 8U) != 0U)
				return INTEL_AX211_DUPLICATE;

			/* Handles the tlv length condition. */
			if (tlv_length == 0U)
				return INTEL_AX211_INVALID;
			seen |= 8U;
			parsed.iml_offset = offset;
			parsed.iml_length = tlv_length;
			break;
		case AX211_TLV_CMD_VERSIONS:
			/* Handles the seen condition. */
			if ((seen & 16U) != 0U)
				return INTEL_AX211_DUPLICATE;

			/* Handles the tlv length condition. */
			if (tlv_length == 0U || (tlv_length & 3U) != 0U)
				return INTEL_AX211_INVALID;
			seen |= 16U;
			parsed.command_versions_offset = offset;
			parsed.command_versions_length = tlv_length;
			break;
		default:
			/* Checks the ax211 ignored firmware tlv result. */
			if (!ax211_ignored_firmware_tlv(type))
				return INTEL_AX211_UNSUPPORTED;
			break;
		}

		offset += span;
	}

	/* Handles the seen condition. */
	if ((seen & 31U) != 31U || parsed.runtime_count == 0U ||
	    cpu_separator == SIZE_MAX || paging_separator == SIZE_MAX) {
		/* Returns the computed result. */
		return INTEL_AX211_MISSING;
	}

	/* Handles the cpu separator condition. */
	if (cpu_separator == 0U || paging_separator <= cpu_separator + 1U ||
	    paging_separator + 1U >= parsed.runtime_count) {
		/* Returns the computed result. */
		return INTEL_AX211_MISSING;
	}

	/* Handles the parsed condition. */
	if (parsed.api_major != INTEL_AX211_FIRMWARE_API ||
	    parsed.api_minor != INTEL_AX211_FIRMWARE_MINOR ||
	    parsed.api_serial != INTEL_AX211_FIRMWARE_SERIAL) {
		/* Returns the computed result. */
		return INTEL_AX211_IDENTITY_MISMATCH;
	}
	parsed.lmac_count = cpu_separator;
	parsed.umac_count = paging_separator - cpu_separator - 1U;
	parsed.paging_count = parsed.runtime_count - paging_separator - 1U;
	*manifest = parsed;
	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 sku equal operation.
 */
int
drv_intel_ax211_sku_equal(
	const struct intel_ax211_sku_id *left,
	const struct intel_ax211_sku_id *right)
{
	/* Handles the left availability. */
	if (left == NULL || right == NULL)
		return 0;

	/* Returns the computed result. */
	return left->data[0] == right->data[0] &&
	       left->data[1] == right->data[1] &&
	       left->data[2] == right->data[2];
}

static int ax211_pnvm_finish(const struct intel_ax211_pnvm_manifest *candidate, int active, int version_seen, int hardware_match, uint16_t mac_type, uint16_t rf_id, struct intel_ax211_pnvm_manifest *manifest);

/* Supports the ax211 pnvm finish operation. */
static int
ax211_pnvm_finish(
	const struct intel_ax211_pnvm_manifest *candidate,
	int active,
	int version_seen,
	int hardware_match,
	uint16_t mac_type,
	uint16_t rf_id,
	struct intel_ax211_pnvm_manifest *manifest)
{
	/* Handles the active condition. */
	if (!active)
		return INTEL_AX211_MISSING;

	/* Handles the version seen condition. */
	if (!version_seen || candidate->section_count == 0U)
		return INTEL_AX211_MISSING;

	/* Handles the hardware match condition. */
	if (!hardware_match || candidate->mac_type != mac_type ||
	    candidate->rf_id != rf_id) {
		/* Returns the computed result. */
		return INTEL_AX211_IDENTITY_MISMATCH;
	}
	*manifest = *candidate;
	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 pnvm parse operation.
 */
int
drv_intel_ax211_pnvm_parse(
	const uint8_t *bytes,
	size_t length,
	const struct intel_ax211_sku_id *sku,
	uint16_t mac_type,
	uint16_t rf_id,
	struct intel_ax211_pnvm_manifest *manifest)
{
	int error;
	struct intel_ax211_sku_id found;
	uint16_t found_mac;
	uint16_t found_rf;
	struct intel_ax211_section *section;
	size_t data_length;
	uint32_t destination;
	uint32_t type, tlv_length;
	size_t span;
	const uint8_t *data;
	int result;
	struct intel_ax211_pnvm_manifest candidate;
	size_t offset = 0;
	int active = 0;
	int version_seen = 0;
	int hardware_match = 0;

	/* Handles the bytes availability. */
	if (bytes == NULL || sku == NULL || manifest == NULL)
		return INTEL_AX211_INVALID;
	memset(&candidate, 0, sizeof(candidate));
	/* Process each remaining element. */
	while (offset < length) {
		/* Checks the current data length. */
		if (length - offset < INTEL_AX211_TLV_RECORD_HEADER_SIZE)
			return INTEL_AX211_TRUNCATED;
		type = ax211_get_le32(bytes + offset);
		tlv_length = ax211_get_le32(bytes + offset + 4U);
		offset += INTEL_AX211_TLV_RECORD_HEADER_SIZE;

		/* Checks the operation result. */
		result = ax211_tlv_span(tlv_length, length - offset, &span);
		if (result != INTEL_AX211_OK)
			return result;
		data = bytes + offset;

		/* Handles the type condition. */
		if (type == AX211_TLV_PNVM_SKU) {
			/* Handles the tlv length condition. */
			if (tlv_length != 12U)
				return INTEL_AX211_INVALID;

			/* Handles the active condition. */
			if (active) {
				/* Checks the operation result. */
				result = ax211_pnvm_finish(
					&candidate, active, version_seen,
					hardware_match, mac_type, rf_id,
					manifest);
				if (result == INTEL_AX211_OK)
					return result;
				if (result != INTEL_AX211_IDENTITY_MISMATCH)
					return result;
			}

			found.data[0] = ax211_get_le32(data);
			found.data[1] = ax211_get_le32(data + 4U);
			found.data[2] = ax211_get_le32(data + 8U);
			active = drv_intel_ax211_sku_equal(&found, sku);
			version_seen = 0;
			hardware_match = 0;
			memset(&candidate, 0, sizeof(candidate));
			candidate.sku = found;
		} else if (type == AX211_TLV_PNVM_VERSION) {
			/* Handles the tlv length condition. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Handles the active condition. */
			if (active) {
				/* Handles the version seen condition. */
				if (version_seen)
					return INTEL_AX211_DUPLICATE;
				version_seen = 1;
				candidate.version = ax211_get_le32(data);
			}
		} else if (type == AX211_TLV_HW_TYPE) {
			/* Handles the tlv length condition. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Handles the active condition. */
			if (active) {
				found_mac = ax211_get_le16(data);

				/* Handles the found mac condition. */
				found_rf = ax211_get_le16(data + 2U);
				if (found_mac == mac_type &&
				    found_rf == rf_id) {
					hardware_match = 1;
					candidate.mac_type = found_mac;
					candidate.rf_id = found_rf;
				}
			}
		} else if (type == AX211_TLV_SEC_RT) {
			/* Handles the tlv length condition. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Handles the active condition. */
			destination = ax211_get_le32(data);
			if (active &&
			    destination != INTEL_AX211_PNVM_SEPARATOR) {
				/* Handles the data length condition. */
				data_length = (size_t)tlv_length - 4U;
				if (data_length == 0U)
					return INTEL_AX211_INVALID;

				/* Handles the candidate condition. */
				if (candidate.section_count >=
				    INTEL_AX211_MAX_PNVM_SECTIONS) {
					/* Returns the computed result. */
					return INTEL_AX211_OVERFLOW;
				}

				/* Handles the candidate condition. */
				if (candidate.total_length >
				    SIZE_MAX - data_length) {
					/* Returns the computed result. */
					return INTEL_AX211_OVERFLOW;
				}
				section = &candidate.section
						   [candidate.section_count++];
				section->destination = destination;
				section->file_offset = offset + 4U;
				section->length = data_length;
				candidate.total_length += data_length;
			}
		} else {
			/* Returns the computed result. */
			return INTEL_AX211_UNSUPPORTED;
		}

		offset += span;
	}

	/* Obtains the ax211 pnvm finish result. */
	error =
		ax211_pnvm_finish(&candidate, active, version_seen,
				  hardware_match, mac_type, rf_id, manifest);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv intel ax211 pnvm inspect operation.
 */
int
drv_intel_ax211_pnvm_inspect(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_pnvm_inventory *inventory)
{
	uint16_t mac_type, rf_type;
	size_t section_length;
	uint32_t type, tlv_length;
	size_t span;
	const uint8_t *data;
	int result;
	struct intel_ax211_pnvm_inventory found;
	size_t offset = 0;

	/* Handles the bytes availability. */
	if (bytes == NULL || inventory == NULL)
		return INTEL_AX211_INVALID;
	memset(&found, 0, sizeof(found));
	/* Process each remaining element. */
	while (offset < length) {
		/* Checks the current data length. */
		if (length - offset < INTEL_AX211_TLV_RECORD_HEADER_SIZE)
			return INTEL_AX211_TRUNCATED;
		type = ax211_get_le32(bytes + offset);
		tlv_length = ax211_get_le32(bytes + offset + 4U);
		offset += INTEL_AX211_TLV_RECORD_HEADER_SIZE;

		/* Checks the operation result. */
		result = ax211_tlv_span(tlv_length, length - offset, &span);
		if (result != INTEL_AX211_OK)
			return result;
		data = bytes + offset;

		/* Handles the type condition. */
		if (type == AX211_TLV_PNVM_SKU) {
			/* Handles the tlv length condition. */
			if (tlv_length != 12U)
				return INTEL_AX211_INVALID;
			found.sku_count++;
		} else if (type == AX211_TLV_PNVM_VERSION) {
			/* Handles the tlv length condition. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			found.version_count++;
		} else if (type == AX211_TLV_HW_TYPE) {
			/* Handles the tlv length condition. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			mac_type = ax211_get_le16(data);
			rf_type = ax211_get_le16(data + 2U);
			found.hardware_type_count++;

			/* Checks the drv intel ax211 mac type supported result. */
			if (drv_intel_ax211_mac_type_supported(mac_type) &&
			    rf_type == INTEL_AX211_RF_TYPE)
				found.supported_hardware_type_count++;
		} else if (type == AX211_TLV_SEC_RT) {
			/* Handles the tlv length condition. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Checks the ax211 get le32 result. */
			if (ax211_get_le32(data) !=
			    INTEL_AX211_PNVM_SEPARATOR) {
				/* Handles the section length condition. */
				section_length = (size_t)tlv_length - 4U;
				if (section_length == 0U)
					return INTEL_AX211_INVALID;

				/* Handles the found condition. */
				if (found.total_section_length >
				    SIZE_MAX - section_length) {
					/* Returns the computed result. */
					return INTEL_AX211_OVERFLOW;
				}
				found.total_section_length += section_length;
				found.section_count++;
			}
		} else {
			/* Returns the computed result. */
			return INTEL_AX211_UNSUPPORTED;
		}

		offset += span;
	}

	/* Handles the found condition. */
	if (found.sku_count == 0U || found.version_count == 0U ||
	    found.hardware_type_count == 0U || found.section_count == 0U) {
		/* Returns the computed result. */
		return INTEL_AX211_MISSING;
	}
	*inventory = found;
	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 context info gen3 encode operation.
 */
int
drv_intel_ax211_context_info_gen3_encode(
	uint8_t output[104],
	const struct intel_ax211_context_info_gen3 *context)
{
	/* Handles the output availability. */
	if (output == NULL || context == NULL)
		return INTEL_AX211_INVALID;

	/* Handles the context condition. */
	if (context->command_transfer_ring_size !=
		    INTEL_AX211_COMMAND_RING_CB_SIZE ||
	    context->command_completion_ring_size !=
		    INTEL_AX211_RX_RING_CB_SIZE) {
		/* Returns the computed result. */
		return INTEL_AX211_INVALID;
	}
	memset(output, 0, INTEL_AX211_CONTEXT_INFO_GEN3_SIZE);
	ax211_put_le16(output, context->version);
	ax211_put_le16(output + 2U, INTEL_AX211_CONTEXT_INFO_GEN3_SIZE / 4U);
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

	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 rx transfer descriptor encode operation.
 */
int
drv_intel_ax211_rx_transfer_descriptor_encode(
	uint8_t output[16],
	uint16_t buffer_id,
	uint64_t address)
{
	/* Handles the output availability. */
	if (output == NULL)
		return INTEL_AX211_INVALID;
	memset(output, 0, INTEL_AX211_RX_TRANSFER_DESCRIPTOR_SIZE);
	ax211_put_le16(output, buffer_id);
	ax211_put_le64(output + 8U, address);

	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 rx completion descriptor decode operation.
 */
int
drv_intel_ax211_rx_completion_descriptor_decode(
	const uint8_t input[32],
	uint16_t *buffer_id,
	uint8_t *flags)
{
	/* Handles the input availability. */
	if (input == NULL || buffer_id == NULL || flags == NULL)
		return INTEL_AX211_INVALID;
	*buffer_id = ax211_get_le16(input + 4U);
	*flags = input[6];
	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 tfd encode operation.
 */
int
drv_intel_ax211_tfd_encode(
	uint8_t output[256],
	const struct intel_ax211_tfd_buffer *buffers,
	size_t buffer_count)
{
	size_t offset;
	size_t index;

	/* Handles the output availability. */
	if (output == NULL || buffers == NULL || buffer_count == 0U ||
	    buffer_count > INTEL_AX211_TFD_MAX_BUFFERS) {
		/* Returns the computed result. */
		return INTEL_AX211_INVALID;
	}
	memset(output, 0, INTEL_AX211_TFD_SIZE);
	ax211_put_le16(output, (uint16_t)buffer_count);
	/* Process each remaining element. */
	for (index = 0; index < buffer_count; index++) {
		offset = 2U + index * 10U;

		/* Handles the buffers condition. */
		if (buffers[index].length == 0U ||
		    buffers[index].length > INTEL_AX211_TFD_BUFFER_MAX_LENGTH) {
			drv_intel_ax211_scrub(output, INTEL_AX211_TFD_SIZE);

			/* Returns the computed result. */
			return INTEL_AX211_INVALID;
		}

		ax211_put_le16(output + offset, buffers[index].length);
		ax211_put_le64(output + offset + 2U, buffers[index].address);
	}

	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 narrow command encode operation.
 */
int
drv_intel_ax211_narrow_command_encode(
	uint8_t output[4],
	uint8_t opcode,
	uint8_t flags,
	const struct intel_ax211_ring_token *token)
{
	/* Handles the output availability. */
	if (output == NULL || token == NULL)
		return INTEL_AX211_INVALID;
	output[0] = opcode;
	output[1] = flags;
	output[2] = token->index;
	output[3] = token->queue;

	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 wide command encode operation.
 */
int
drv_intel_ax211_wide_command_encode(
	uint8_t output[8],
	const struct intel_ax211_command_id *command,
	uint16_t payload_length,
	const struct intel_ax211_ring_token *token)
{
	/* Handles the output availability. */
	if (output == NULL || command == NULL || token == NULL ||
	    payload_length > INTEL_AX211_MAX_COMMAND_PAYLOAD) {
		/* Returns the computed result. */
		return INTEL_AX211_INVALID;
	}
	output[0] = command->opcode;
	output[1] = command->group;
	output[2] = token->index;
	output[3] = token->queue;
	ax211_put_le16(output + 4U, payload_length);
	output[6] = 0;
	output[7] = command->version;

	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 event decode operation.
 */
int
drv_intel_ax211_event_decode(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_event *event)
{
	uint32_t length_flags;
	size_t frame_length;

	/* Handles the bytes availability. */
	if (bytes == NULL || event == NULL)
		return INTEL_AX211_INVALID;

	/* Checks the current data length. */
	if (length < INTEL_AX211_EVENT_HEADER_SIZE)
		return INTEL_AX211_TRUNCATED;
	length_flags = ax211_get_le32(bytes);

	/* Handles the frame length condition. */
	frame_length = (size_t)(length_flags & 0x3fffU);
	if (frame_length < INTEL_AX211_NARROW_COMMAND_HEADER_SIZE)
		return INTEL_AX211_INVALID;

	/* Handles the frame length condition. */
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

	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 ring init operation.
 */
int
drv_intel_ax211_ring_init(
	struct intel_ax211_ring *ring,
	uint8_t queue,
	uint16_t capacity)
{
	/* Handles the ring availability. */
	if (ring == NULL || capacity == 0U ||
	    capacity > INTEL_AX211_COMMAND_RING_SIZE ||
	    (capacity & (uint16_t)(capacity - 1U)) != 0U) {
		/* Returns the computed result. */
		return INTEL_AX211_INVALID;
	}
	memset(ring, 0, sizeof(*ring));
	ring->capacity = capacity;
	ring->queue = queue;

	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 ring reserve operation.
 */
int
drv_intel_ax211_ring_reserve(
	struct intel_ax211_ring *ring,
	struct intel_ax211_ring_token *token)
{
	/* Handles the ring availability. */
	if (ring == NULL || token == NULL || ring->capacity == 0U)
		return INTEL_AX211_INVALID;

	/* Handles the ring condition. */
	if (ring->used == ring->capacity)
		return INTEL_AX211_FULL;
	token->queue = ring->queue;
	token->index = (uint8_t)ring->head;
	ring->head = (uint16_t)((ring->head + 1U) & (ring->capacity - 1U));
	ring->used++;

	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 ring complete operation.
 */
int
drv_intel_ax211_ring_complete(
	struct intel_ax211_ring *ring,
	const struct intel_ax211_ring_token *token)
{
	/* Handles the ring availability. */
	if (ring == NULL || token == NULL || ring->capacity == 0U)
		return INTEL_AX211_INVALID;

	/* Handles the ring condition. */
	if (ring->used == 0U || token->queue != ring->queue ||
	    token->index != (uint8_t)ring->tail) {
		/* Returns the computed result. */
		return INTEL_AX211_STALE;
	}
	ring->tail = (uint16_t)((ring->tail + 1U) & (ring->capacity - 1U));
	ring->used--;

	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 ring available operation.
 */
size_t
drv_intel_ax211_ring_available(
	const struct intel_ax211_ring *ring)
{
	/* Handles the ring availability. */
	if (ring == NULL || ring->capacity == 0U || ring->used > ring->capacity)
		return 0;

	/* Returns the computed result. */
	return (size_t)(ring->capacity - ring->used);
}

/*
 * Implements the drv intel ax211 scrub operation.
 */
void
drv_intel_ax211_scrub(
	void *memory,
	size_t length)
{
	volatile uint8_t *bytes = (volatile uint8_t *)memory;

	/* Handles the memory availability. */
	if (memory == NULL)
		return;
	/* Process each remaining element. */
	while (length-- != 0U)
		*bytes++ = 0;
}

/*
 * Implements the drv intel ax211 staging set operation.
 */
int
drv_intel_ax211_staging_set(
	struct intel_ax211_staging *staging,
	const void *data,
	size_t length)
{
	/* Handles the staging availability. */
	if (staging == NULL || (data == NULL && length != 0U))
		return INTEL_AX211_INVALID;
	drv_intel_ax211_staging_clear(staging);

	/* Checks the current data length. */
	if (length > sizeof(staging->bytes))
		return INTEL_AX211_OVERFLOW;

	/* Checks the current data length. */
	if (length != 0U)
		memcpy(staging->bytes, data, length);
	staging->length = length;

	/* Returns the computed result. */
	return INTEL_AX211_OK;
}

/*
 * Implements the drv intel ax211 staging clear operation.
 */
void
drv_intel_ax211_staging_clear(
	struct intel_ax211_staging *staging)
{
	/* Handles the staging availability. */
	if (staging == NULL)
		return;
	drv_intel_ax211_scrub(staging->bytes, sizeof(staging->bytes));
	staging->length = 0;
}
/* End consolidated intel-ax211.c. */

/* Begin consolidated pci-intel-ax211.c. */
/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; tab-width: 8 -*- */

/*
 * zedBSD Intel AX211 PCI/CNVio2 transport
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/pci-intel-ax211.h>
#include <drivers/pci.h>

#include "intel-ax211-assoc.h"
#include "intel-ax211-bss.h"
#include "intel-ax211-boot.h"
#include "intel-ax211-key.h"
#include "intel-ax211-mmio.h"
#include "intel-ax211-pci-mmio.h"
#include "intel-ax211-rx.h"
#include "intel-ax211-runtime-start.h"
#include "intel-ax211-scan-session.h"
#include "intel-ax211-transport-backend.h"
#include "intel-ax211-tx-ring.h"

#include <errno.h>
#include <limits.h>
#include <kern/clock.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/net/wlan.h>
#include <kern/sched.h>
#include <stdint.h>
#include <string.h>
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/device-io.h"

#define AX211_PCI_VENDOR 0x8086U
#define AX211_PCI_PRODUCT 0x51f0U
#define AX211_PCI_SUBVENDOR 0x8086U
#define AX211_PCI_SUBPRODUCT 0x4090U
#define AX211_PCI_REVISION 0x01U
#define AX211_PCI_CLASS 0x028000U
#define AX211_PCI_CLASS_MASK 0xffffffU
#define AX211_PCI_COMMAND 0x04U
#define AX211_PCI_COMMAND_MEMORY 0x0002U
#define AX211_PCI_COMMAND_MASTER 0x0004U
#define AX211_BAR0_MINIMUM_SIZE 0x4000U
#define AX211_MTU 1500U
#define AX211_MAC_ADDRESS_SIZE 6U
#define AX211_PASSIVE_CHANNEL_COUNT 11U
#define AX211_RECEIVE_POLL_DELAY_US 50U
#define AX211_DIRECT_EVENT_LIMIT 64U
#define AX211_DEFERRED_EVENT_LIMIT 16U
#define AX211_DIRECT_TIMEOUT_US 1000000U
#define AX211_LIFECYCLE_JOIN_TICKS (5U * KERN_CLOCK_HZ)
#define AX211_ASSOC_STATION_ID 0U
#define AX211_ASSOC_CCK_ACK_RATES 0x0fU
#define AX211_ASSOC_OFDM_ACK_RATES 0x15U
#define AX211_CAPABILITY_SHORT_PREAMBLE 0x0020U
#define AX211_CAPABILITY_SHORT_SLOT 0x0400U
#define AX211_REPLY_ERROR_OPCODE 0x02U
#define AX211_REPLY_ERROR_SIZE 20U
#define AX211_UREG_DOORBELL_TO_ISR6 0xa05c04U
#define AX211_UREG_DOORBELL_PNVM (1U << 20)
#define AX211_PCIE_CAPABILITY 0x10U
#define AX211_PCIE_DEVICE_CONTROL2 0x28U
#define AX211_PCIE_DEVICE_CONTROL2_LTR (1U << 10)
#define AX211_HBUS_TARG_MEM_RADDR 0x40cU
#define AX211_HBUS_TARG_MEM_RDAT 0x41cU
#define AX211_FW_ADDR_CACHE_CONTROL 0xc0000000U
#define AX211_ERROR_LOG_MIN_ADDRESS 0x00400000U
#define AX211_LMAC_ERROR_WORD_COUNT 30U
#define AX211_UMAC_ERROR_WORD_COUNT 15U

struct ax211_pci_staged_key {
	struct intel_ax211_key_request request;
	uint8_t valid;
	uint8_t programmed;
	uint8_t reserved[6];
};

struct ax211_pci_deferred_event {
	struct intel_ax211_boot_received_event received;
	uint8_t bytes[INTEL_AX211_BOOT_EVENT_CAPACITY];
};

/*
 * These are stable device register encodings, not a copied implementation.
 * HW_REV bits 15:4 contain the MAC type. HW_RF_ID bits 23:12 contain the RF
 * type. The frozen P038 target is an SO-or-SOF MAC with a Garfield Peak RF.
 */
#define AX211_CSR_HW_REV 0x028U
#define AX211_CSR_HW_RF_ID 0x09cU
#define AX211_CSR_HW_REV_TYPE_MASK 0x0000fff0U
#define AX211_CSR_HW_REV_TYPE_SHIFT 4U
#define AX211_CSR_HW_REV_TYPE_SO 0x037U
#define AX211_CSR_HW_REV_TYPE_SOF 0x043U
#define AX211_CSR_HW_RF_TYPE_MASK 0x00fff000U
#define AX211_CSR_HW_RF_TYPE_SHIFT 12U
#define AX211_CSR_HW_RF_TYPE_GF 0x10dU
#define AX211_CSR_HW_RF_CDB 0x10000000U

struct ax211_pci_controller {
	struct mutex lifecycle_lock;
	struct spinlock interrupt_lock;
	struct drv_pci_device *device;
	struct drv_pci_mapping mapping;
	struct drv_pci_enable_state enable_state;
	struct drv_pci_bar original_bar;
	struct intel_ax211_pci_mmio_backend backend;
	struct intel_ax211_mmio mmio;
	struct intel_ax211_transport_backend transport_backend;
	struct intel_ax211_transport transport;
	struct intel_ax211_boot boot;
	struct intel_ax211_runtime_start runtime_start;
	struct intel_ax211_scan_session scan_session;
	struct intel_ax211_bss_cache bss_staging_cache;
	struct intel_ax211_bss_cache bss_published_cache;
	struct intel_ax211_bss_entry selected_bss;
	struct drv_intel_ax211_bss_assoc_metadata selected_metadata;
	struct intel_ax211_assoc_state association;
	struct intel_ax211_key_state keys;
	struct ax211_pci_staged_key staged_pairwise_key;
	struct ax211_pci_staged_key
		staged_group_key[INTEL_AX211_KEY_INDEX_LIMIT];
	struct ax211_pci_deferred_event
		deferred_event[AX211_DEFERRED_EVENT_LIMIT];
	uint8_t deferred_dispatch_event[INTEL_AX211_BOOT_EVENT_CAPACITY];
	uint64_t retired_pairwise_key_generation;
	uint64_t retired_group_key_generation[INTEL_AX211_KEY_INDEX_LIMIT];
	uint8_t retired_group_key_remove[INTEL_AX211_KEY_INDEX_LIMIT];
	struct intel_ax211_tx_ring tx_ring;
	struct intel_ax211_tx_queue_config tx_queue_config;
	struct intel_ax211_dma_resources *active_dma;
	struct drv_pci_irq irq;
	void *irq_cookie;
	struct net_device *net_device;
	struct wlan_station *station;
	struct ax211_pci_controller *next;
	uint32_t hardware_revision;
	uint32_t radio_identity;
	uint32_t hardware_epoch;
	uint8_t runtime_event[INTEL_AX211_BOOT_EVENT_CAPACITY];
	uint8_t runtime_frame[INTEL_AX211_RX_MPDU_FRAME_MAX];
	uint8_t last_receive_header[INTEL_AX211_EVENT_HEADER_SIZE];
	size_t last_receive_length;
	uint8_t last_receive_version;
	uint32_t command_fh_causes;
	uint32_t command_hw_causes;
	uint32_t command_raw_fh_causes;
	uint32_t command_raw_hw_causes;
	uint8_t tx_report_completion[INTEL_AX211_TX_RING_SLOT_COUNT];
	uint32_t scan_step_index;
	uint64_t bss_staging_generation;
	uint64_t bss_published_generation;
	uint64_t connection_generation;
	uint64_t recovery_generation;
	uint64_t next_management_cookie;
	uint64_t control_deadline_ticks;
	int recovery_error;
	unsigned refresh_epoch;
	unsigned deferred_event_head;
	unsigned deferred_event_count;
	unsigned deferred_event_draining;
	unsigned operations_active;
	unsigned irq_count;
	unsigned irq_allocated;
	unsigned irq_established;
	unsigned receive_enabled;
	unsigned irq_latched;
	unsigned boot_initialized;
	unsigned runtime_initialized;
	unsigned runtime_active;
	unsigned operation_admission_open;
	unsigned recovery_pending;
	unsigned recovery_running;
	unsigned poll_active;
	unsigned poll_reschedule;
	unsigned scan_initialized;
	unsigned bss_staging_initialized;
	unsigned bss_published_initialized;
	unsigned selected_bss_valid;
	unsigned association_initialized;
	unsigned keys_initialized;
	unsigned tx_ring_allocated;
	unsigned bar_claimed;
	unsigned bar_mapped;
	unsigned bar_may_have_moved;
	unsigned original_bar_valid;
	unsigned state_saved;
	unsigned driver_data_set;
	unsigned listed;
	unsigned refresh_busy;
	unsigned detaching;
	unsigned ready;
	unsigned quarantined;
	/* True only after every runtime/DMA owner passed the checked stop. */
	unsigned session_stopped;
	unsigned close_pending;
	uint64_t reject_log_deadline;
	unsigned net_live;
	unsigned station_attached;
};

static int ax211_pci_match(struct drv_pci_device *device, const struct drv_pci_id *identity);
static int ax211_pci_attach(struct drv_pci_device *device, const struct drv_pci_id *identity);
static int ax211_pci_detach(struct drv_pci_device *device, unsigned flags);
static int ax211_pci_identity_matches(const struct drv_pci_device *device);
static int ax211_pci_bar_validate(const struct drv_pci_bar *bar);
static uint32_t ax211_pci_read32(const struct ax211_pci_controller *controller, unsigned offset);
static int ax211_pci_hardware_validate(uint32_t hardware_revision, uint32_t radio_identity);
static int ax211_pci_profile(struct ax211_pci_controller *controller, struct intel_ax211_mmio_profile *profile);
static int ax211_pci_acquire(struct ax211_pci_controller *controller);
static int ax211_pci_restore_bar(struct ax211_pci_controller *controller);
static int ax211_pci_release_resources(struct ax211_pci_controller *controller, int result, int retain_on_failure);
static int ax211_pci_quarantine(struct ax211_pci_controller *controller, int failure);
static uint64_t ax211_pci_lifecycle_deadline(void);
static int ax211_pci_station_pin_locked(struct ax211_pci_controller *controller, struct wlan_station **station);
static int ax211_pci_operation_enter_locked(struct ax211_pci_controller *controller, struct wlan_station **station);
static void ax211_pci_operation_leave_locked(struct ax211_pci_controller *controller);
static int ax211_pci_operations_join_locked(struct ax211_pci_controller *controller, uint64_t deadline);
static int ax211_pci_station_close_wait(struct wlan_station *station, uint64_t deadline);
static int ax211_pci_close_locked(struct ax211_pci_controller *);
static int ax211_pci_log_rejection(struct ax211_pci_controller *);
static void ax211_pci_recovery_latch_locked(struct ax211_pci_controller *controller, int error);
static void ax211_pci_stop_defer_locked(struct ax211_pci_controller *controller, int error);
static int ax211_pci_recovery_run_locked(struct ax211_pci_controller *controller);
static void ax211_pci_list_add(struct ax211_pci_controller *controller);
static int ax211_pci_list_remove(struct ax211_pci_controller *controller);
static void ax211_pci_list_forget(struct ax211_pci_controller *controller);
static struct ax211_pci_controller * ax211_pci_list_find_device(struct drv_pci_device *device);
static struct ax211_pci_controller *ax211_pci_refresh_claim(unsigned epoch);
static void ax211_pci_refresh_release(struct ax211_pci_controller *controller);
static int ax211_pci_refresh_one(struct ax211_pci_controller *controller);
static int ax211_pci_publish(struct ax211_pci_controller *controller, const uint8_t mac_address[AX211_MAC_ADDRESS_SIZE]);
static void ax211_pci_scan_profile(struct wlan_scan_profile *profile);
static int ax211_pci_partial_discard(struct ax211_pci_controller *controller);
static int ax211_pci_graph_detach(struct ax211_pci_controller *controller);
static int ax211_pci_session_stop(struct ax211_pci_controller *controller);
static int ax211_pci_ltr_enabled(struct ax211_pci_controller *controller, int *enabled);
static int ax211_pci_irq(void *argument);
static int ax211_pci_receive_epoch_begin(void *argument, uint32_t generation);
static int ax211_pci_transport_bind(void *argument, struct intel_ax211_dma_resources *dma, struct intel_ax211_mmio *mmio, struct intel_ax211_transport *transport, uint32_t generation);
static int ax211_pci_receive_event(void *argument, uint64_t deadline_us, uint8_t *bytes, size_t capacity, struct intel_ax211_boot_received_event *event);
static int ax211_pci_publish_pnvm(void *argument, struct intel_ax211_dma_resources *dma);
static int ax211_pci_post_alive(void *argument, const struct intel_ax211_protocol_alive *alive);
static int ax211_pci_interrupt_drain(void *argument);
static int ax211_pci_clock_us(void *argument, uint64_t *time_us);
static int ax211_pci_nic_lock(void *argument);
static int ax211_pci_nic_unlock(void *argument);
static int ax211_pci_tx_event(const struct intel_ax211_event *event);
static uint8_t ax211_pci_notification_version(const uint8_t *bytes, size_t length);
static int ax211_pci_scan_initialize(struct ax211_pci_controller *controller);
static uint32_t ax211_pci_channel_frequency(uint8_t channel);
static int ax211_pci_runtime_scan_profile(const struct ax211_pci_controller *controller, struct wlan_scan_profile *profile);
static int ax211_pci_scan_channel_present(const struct ax211_pci_controller *controller, uint8_t channel);
static void ax211_pci_scan_clear(struct ax211_pci_controller *controller);
static void ax211_pci_bss_staging_discard(struct ax211_pci_controller *controller);
static int ax211_pci_bss_staging_publish(struct ax211_pci_controller *controller, uint64_t generation);
static int ax211_pci_event_message(const uint8_t *bytes, size_t length, const struct intel_ax211_boot_received_event *received, struct intel_ax211_event *event, struct intel_ax211_protocol_message *message);
static int ax211_pci_scan_command_dispatch(struct ax211_pci_controller *controller, const uint8_t *bytes, size_t length, uint64_t now);
static int ax211_pci_scan_notification_dispatch( struct ax211_pci_controller *controller, const struct intel_ax211_protocol_message *message, uint64_t now);
static int ax211_pci_rx_dispatch(struct ax211_pci_controller *controller, const struct intel_ax211_protocol_message *message);
static void ax211_pci_scan_report_error(struct ax211_pci_controller *controller, int result);
static int ax211_pci_scan_result_errno(int result);
static int ax211_pci_runtime_event_dispatch( struct ax211_pci_controller *controller, const uint8_t *bytes, size_t length, const struct intel_ax211_boot_received_event *event);
static int ax211_pci_deferred_event_enqueue( struct ax211_pci_controller *controller, const uint8_t *bytes, const struct intel_ax211_boot_received_event *received);
static int ax211_pci_deferred_event_drain_one(struct ax211_pci_controller *controller);
static void ax211_pci_deferred_event_clear(struct ax211_pci_controller *controller);
static uint32_t ax211_pci_get_le32(const uint8_t bytes[4]);
static void ax211_pci_scrub(void *memory, size_t length);
static int ax211_pci_tx_sync_for_device(void *argument, const struct drv_dma_buffer *buffer, size_t offset, size_t length);
static int ax211_pci_tx_write32(void *argument, uint32_t offset, uint32_t value);
static uint64_t ax211_pci_assoc_clock_us(void *argument);
static int ax211_pci_assoc_exchange(void *argument, const struct intel_ax211_assoc_command *command, struct intel_ax211_assoc_reply *reply);
static int ax211_pci_direct_command( struct ax211_pci_controller *controller, const struct intel_ax211_command_request *request, uint64_t deadline_ticks, uint8_t *response, size_t response_capacity, size_t *response_length, struct intel_ax211_protocol_message *completed_message, struct intel_ax211_protocol_pending_command *completed_pending);
static int ax211_pci_command_timeout(struct ax211_pci_controller *controller, uint64_t deadline_ticks, uint64_t now_us, uint64_t *timeout_us, uint64_t *deadline_us);
static int ax211_pci_sram_read_locked(struct ax211_pci_controller *controller, uint32_t address, uint32_t *words, size_t count);
static void ax211_pci_firmware_error_dump(struct ax211_pci_controller *controller);
static int ax211_pci_assoc_result_errno(int result);
static int ax211_pci_tx_ring_result_errno(int result);
static int ax211_pci_key_result_errno(int result);
static int ax211_pci_assoc_profile(struct ax211_pci_controller *controller, const struct wlan_bss_record *bss, uint64_t connection_generation, struct intel_ax211_assoc_profile *profile);
static int ax211_pci_assoc_rollback(struct ax211_pci_controller *controller, uint64_t generation);
static int ax211_pci_mcast_filter_configure(struct ax211_pci_controller *controller, uint64_t deadline_ticks);
static int ax211_pci_mac_power_configure(struct ax211_pci_controller *controller, uint64_t deadline_ticks);
static int ax211_pci_keys_remove_all(struct ax211_pci_controller *controller, uint64_t deadline_ticks);
static void ax211_pci_connection_clear(struct ax211_pci_controller *controller);
static int ax211_pci_key_command(struct ax211_pci_controller *controller, const uint8_t *payload, uint64_t deadline_ticks);
static int ax211_pci_staged_key_store(struct ax211_pci_staged_key *staged, const struct intel_ax211_key_request *request);
static int ax211_pci_staged_key_program(struct ax211_pci_controller *controller, struct ax211_pci_staged_key *staged, uint64_t deadline_ticks);
static void ax211_pci_staged_key_clear(struct ax211_pci_staged_key *staged);
static int ax211_pci_keys_have_active(const struct ax211_pci_controller *controller);
static int ax211_pci_key_request_address_valid( const struct ax211_pci_controller *controller, const struct wlan_radio_key_request *request);
static int ax211_pci_key_fail_closed(struct ax211_pci_controller *controller, int error);
static int ax211_pci_tx_submit(struct ax211_pci_controller *controller, const struct intel_ax211_tx_request *request, uint64_t deadline_ticks, int report_completion);
static int ax211_pci_tx_dispatch(struct ax211_pci_controller *controller, const struct intel_ax211_protocol_message *message);
static int ax211_pci_tx_timeout_check(struct ax211_pci_controller *controller, uint64_t now_us);
static int ax211_pci_connection_rx_dispatch(struct ax211_pci_controller *controller, const struct intel_ax211_rx_mpdu *mpdu, uint16_t frame_control);

static int ax211_net_open(struct net_device *device);
static void ax211_net_close(struct net_device *device);
static int ax211_net_transmit(struct net_device *device, struct packet_buf *packet);
static unsigned ax211_net_poll_receive(struct net_device *device, unsigned budget);
static int ax211_net_ioctl(struct net_device *device, unsigned long request, void *argument);
static void ax211_net_release(void *driver_data);

static int ax211_radio_scan_channel_start(void *context, uint64_t generation, uint32_t step_index, uint32_t channel, uint64_t deadline);
static int ax211_radio_scan_stop(void *context, uint64_t generation);
static int ax211_radio_connect_start(void *context, uint64_t generation, const struct wlan_bss_record *bss, uint64_t deadline);
static int ax211_radio_disconnect(void *context, uint64_t generation);
static int ax211_radio_management_transmit(void *context, uint64_t generation, const uint8_t *frame, size_t length, uint64_t deadline);
static int ax211_radio_association_set(void *context, uint64_t generation, const uint8_t bssid[6], uint16_t aid, uint64_t deadline);
static int ax211_radio_association_clear(void *context, uint64_t generation, uint64_t deadline);
static int ax211_radio_frame_transmit(void *context, const struct wlan_radio_tx_request *request);
static int ax211_radio_key_install(void *context, const struct wlan_radio_key_request *request);
static int ax211_radio_key_delete(void *context, uint64_t generation, enum wlan_radio_key_kind kind, uint8_t key_index, uint64_t key_generation, uint64_t deadline);
static int ax211_radio_keys_activate(void *context, uint64_t generation, uint64_t pairwise_key_generation, uint64_t group_key_generation, uint64_t deadline);
static int ax211_radio_quiesce(void *context);
static int ax211_radio_stop_retry(void *context);

static const struct drv_pci_id ax211_pci_ids[] = {
	{
		.vendor = AX211_PCI_VENDOR,
		.device = AX211_PCI_PRODUCT,
		.subvendor = AX211_PCI_SUBVENDOR,
		.subdevice = AX211_PCI_SUBPRODUCT,
		.class_code = AX211_PCI_CLASS,
		.class_mask = AX211_PCI_CLASS_MASK,
	},
};

static struct drv_pci_driver ax211_pci_driver = {
	.name = "intel-ax211",
	.ids = ax211_pci_ids,
	.id_count = sizeof(ax211_pci_ids) / sizeof(ax211_pci_ids[0]),
	.match = ax211_pci_match,
	.attach = ax211_pci_attach,
	.detach = ax211_pci_detach,
};

/* This table is the permanent common-WLAN boundary for the AX211 backend. */
static const struct wlan_radio_ops ax211_radio_ops = {
	.scan_channel_start = ax211_radio_scan_channel_start,
	.scan_stop = ax211_radio_scan_stop,
	.connect_start = ax211_radio_connect_start,
	.disconnect = ax211_radio_disconnect,
	.management_transmit = ax211_radio_management_transmit,
	.association_set = ax211_radio_association_set,
	.association_clear = ax211_radio_association_clear,
	.frame_transmit = ax211_radio_frame_transmit,
	.key_install = ax211_radio_key_install,
	.key_delete = ax211_radio_key_delete,
	.keys_activate = ax211_radio_keys_activate,
	.quiesce = ax211_radio_quiesce,
	.stop_retry = ax211_radio_stop_retry};

static const struct net_device_ops ax211_net_ops = {
	.open = ax211_net_open,
	.close = ax211_net_close,
	.transmit = ax211_net_transmit,
	.poll_receive = ax211_net_poll_receive,
	.ioctl = ax211_net_ioctl,
	.release = ax211_net_release};

static const struct intel_ax211_runtime_start_ops ax211_runtime_start_ops = {
	.boot = {.receive_epoch_begin = ax211_pci_receive_epoch_begin,
		 .transport_bind = ax211_pci_transport_bind,
		 .receive_event = ax211_pci_receive_event,
		 .publish_pnvm = ax211_pci_publish_pnvm,
		 .post_alive = ax211_pci_post_alive,
		 .interrupt_drain = ax211_pci_interrupt_drain,
		 .clock_us = ax211_pci_clock_us},
	.nic_lock = ax211_pci_nic_lock,
	.nic_unlock = ax211_pci_nic_unlock};

static const struct intel_ax211_tx_ring_ops ax211_tx_ring_ops = {
	.sync_for_device = ax211_pci_tx_sync_for_device,
	.write32 = ax211_pci_tx_write32};

static const struct intel_ax211_assoc_ops ax211_assoc_ops = {
	.clock_us = ax211_pci_assoc_clock_us,
	.exchange = ax211_pci_assoc_exchange};

static struct ax211_pci_controller *ax211_controllers;
static struct spinlock ax211_registry_lock;
static unsigned ax211_refresh_epoch;
static unsigned ax211_registry_initialized;

/*
 * Registers the exact Intel AX211 PCI transport driver.
 */
int
drv_pci_intel_ax211_driver_register(
	void)
{
	int function_result;
	int error;

	/* Handles the ax211 registry initialized condition. */
	if (ax211_registry_initialized) {
		/* Obtains the drv pci driver register result. */
		function_result = drv_pci_driver_register(&ax211_pci_driver);

		/* Returns the computed result. */
		return function_result;
	}

	spin_init(&ax211_registry_lock, LOCK_RANK_DEVICE,
		  "Intel AX211 registry");
	ax211_controllers = NULL;
	ax211_refresh_epoch = 0U;

	/* Checks the operation status. */
	error = drv_pci_driver_register(&ax211_pci_driver);
	if (error == 0)
		ax211_registry_initialized = 1U;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Publishes persistent controllers after platform interrupt bring-up.
 */
void
drv_pci_intel_ax211_devices_ready(
	void)
{
	struct ax211_pci_controller *controller;
	unsigned long enabled;
	unsigned epoch;
	int error;

	enabled = spin_lock_irqsave(&ax211_registry_lock);

	ax211_refresh_epoch++;

	/* Handles the ax211 refresh epoch condition. */
	if (ax211_refresh_epoch == 0U)
		ax211_refresh_epoch++;
	epoch = ax211_refresh_epoch;

	spin_unlock_irqrestore(&ax211_registry_lock, enabled);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the controller availability. */
		controller = ax211_pci_refresh_claim(epoch);
		if (controller == NULL)
			return;
		error = ax211_pci_refresh_one(controller);
		ax211_pci_refresh_release(controller);
		if (error != 0) {
			kern_logf("intel-ax211: deferred WLAN publication "
				   "failed (%d)\n",
				   error);
		}
	}
}

/* Matches only the frozen P038 PCI identity. */
static int
ax211_pci_match(
	struct drv_pci_device *device,
	const struct drv_pci_id *identity)
{
	int error;

	(void)identity;

	/* Computes the function result. */
	error = ax211_pci_identity_matches(device)
				  ? DRV_PCI_MATCH_EXACT
				  : DRV_PCI_MATCH_NONE;

	/* Returns the computed result. */
	return error;
}

/* Retains a validated, DMA-disabled controller without waiting or I/O. */
static int
ax211_pci_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *identity)
{
	int function_result;
	struct ax211_pci_controller *controller;
	struct intel_ax211_mmio_profile profile;
	const char *stage;
	int cleanup_error;
	int error;

	(void)identity;

	/* Checks the ax211 pci identity matches result. */
	if (!ax211_pci_identity_matches(device))
		return ENODEV;

	/* Checks the drv pci device driver data result. */
	if (drv_pci_device_driver_data(device) != NULL)
		return EBUSY;

	/* Handles the controller availability. */
	controller = kern_malloc(sizeof(*controller));
	if (controller == NULL)
		return ENOMEM;
	memset(controller, 0, sizeof(*controller));
	memset(&profile, 0, sizeof(profile));
	controller->device = device;
	stage = "lifecycle-lock";

	/* Checks the operation status. */
	error = mutex_init(&controller->lifecycle_lock, LOCK_RANK_DEVICE,
			   "Intel AX211 lifecycle");
	if (error == 0) {
		spin_init(&controller->interrupt_lock, LOCK_RANK_DEVICE,
			  "Intel AX211 interrupt");
		controller->hardware_epoch = 1U;
	}

	/* Checks the operation status. */
	if (error == 0) {
		stage = "pci-acquire";
		error = ax211_pci_acquire(controller);
	}

	/* Checks the operation status. */
	if (error == 0) {
		stage = "mmio-backend";
		error = drv_intel_ax211_pci_mmio_backend_init(
			&controller->backend, controller->mapping.address,
			controller->mapping.size);
	}

	/* Checks the operation status. */
	if (error == 0) {
		stage = "mmio-profile";
		error = ax211_pci_profile(controller, &profile);
	}

	/* Checks the operation status. */
	if (error == 0) {
		stage = "mmio-init";
		error = drv_intel_ax211_mmio_init(
			&controller->mmio, drv_intel_ax211_pci_mmio_ops(),
			&controller->backend, &profile);
	}

	ax211_pci_scrub(&profile, sizeof(profile));

	/* Checks the operation status. */
	if (error == 0) {
		stage = "driver-data";

		/* Checks the operation status. */
		error = drv_pci_device_set_driver_data(device, controller);
		if (error == 0)
			controller->driver_data_set = 1U;
	}

	/* Checks the operation status. */
	if (error == 0) {
		controller->ready = 1U;
		ax211_pci_list_add(controller);
		kern_logf("intel-ax211: controller retained; WLAN publication "
			   "deferred\n");

		/* Succeeded. */
		return 0;
	}

	kern_logf("intel-ax211: attach failed stage=%s error=%d "
		   "hw-rev=%08x rf-id=%08x\n",
		   stage, error, controller->hardware_revision,
		   controller->radio_identity);

	/* Handles the controller condition. */
	cleanup_error = ax211_pci_release_resources(controller, error, 1);
	if (controller->bar_claimed) {
		/* Obtains the ax211 pci quarantine result. */
		function_result =
			ax211_pci_quarantine(controller, cleanup_error);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the controller condition. */
	if (controller->driver_data_set)
		(void)drv_pci_device_set_driver_data(device, NULL);
	ax211_pci_scrub(controller, sizeof(*controller));
	kern_free(controller);

	/* Returns the computed result. */
	return cleanup_error;
}

/* Checked detach retires publication before releasing the persistent BAR. */
static int
ax211_pci_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	struct ax211_pci_controller *controller;
	struct net_device *net_device;
	unsigned net_live;
	int error;

	(void)flags;

	/* Handles the controller availability. */
	controller = drv_pci_device_driver_data(device);
	if (controller == NULL)
		controller = ax211_pci_list_find_device(device);

	/* Handles the controller availability. */
	if (controller == NULL || controller->device != device)
		return ENODEV;
	mutex_lock(&controller->lifecycle_lock);

	/* Checks the operation status. */
	error = ax211_pci_list_remove(controller);
	if (error != 0) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return error;
	}

	controller->ready = 0U;
	controller->operation_admission_open = 0U;
	net_device = controller->net_device;
	net_live = controller->net_live;

	mutex_unlock(&controller->lifecycle_lock);

	/* Handles the net device availability. */
	if (net_device != NULL)
		(void)net_device_set_carrier(net_device, 0);

	/* Handles the net device availability. */
	if (net_device != NULL && net_live) {
		/* Checks the operation status. */
		error = net_device_gone(net_device);
		if (error != 0)
			return error;
	}

	mutex_lock(&controller->lifecycle_lock);

	/* Handles the net live condition. */
	if (net_live)
		controller->net_live = 0U;

	/* Checks the operation status. */
	error = ax211_pci_graph_detach(controller);
	if (error != 0) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return error;
	}

	/* Checks the operation status. */
	error = ax211_pci_session_stop(controller);
	if (error != 0) {
		controller->quarantined = 1U;
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return error;
	}

	/* Checks the operation status. */
	error = ax211_pci_release_resources(controller, 0, 1);
	if (error != 0) {
		controller->quarantined = 1U;
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return error;
	}

	controller->quarantined = 0U;

	/* Handles the controller condition. */
	if (controller->driver_data_set)
		error = drv_pci_device_set_driver_data(device, NULL);
	else
		error = 0;
	if (error != 0) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return error;
	}

	controller->driver_data_set = 0U;
	ax211_pci_list_forget(controller);
	controller->device = NULL;
	controller->net_device = NULL;

	mutex_unlock(&controller->lifecycle_lock);

	/* Handles the net device availability. */
	if (net_device != NULL) {
		net_device_destroy(net_device);
	} else {
		ax211_pci_scrub(controller, sizeof(*controller));
		kern_free(controller);
	}

	/* Succeeded. */
	return 0;
}

/* Checks the complete frozen P038 PCI tuple. */
static int
ax211_pci_identity_matches(
	const struct drv_pci_device *device)
{
	int error;

	/* Handles the device availability. */
	if (device == NULL)
		return 0;

	/* Checks the drv pci device vendor result. */
	if (drv_pci_device_vendor(device) != AX211_PCI_VENDOR ||
	    drv_pci_device_product(device) != AX211_PCI_PRODUCT) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the drv pci device subvendor result. */
	if (drv_pci_device_subvendor(device) != AX211_PCI_SUBVENDOR ||
	    drv_pci_device_subproduct(device) != AX211_PCI_SUBPRODUCT) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the drv pci device revision result. */
	if (drv_pci_device_revision(device) != AX211_PCI_REVISION)
		return 0;

	/* Computes the function result. */
	error = (drv_pci_device_class(device) &
			   AX211_PCI_CLASS_MASK) == AX211_PCI_CLASS;

	/* Returns the computed result. */
	return error;
}

/* Validates BAR0 before PCI decode or MMIO is changed. */
static int
ax211_pci_bar_validate(
	const struct drv_pci_bar *bar)
{
	/* Handles the bar availability. */
	if (bar == NULL)
		return EINVAL;

	/* Handles the bar condition. */
	if (bar->type != DRV_PCI_BAR_MEMORY32 &&
	    bar->type != DRV_PCI_BAR_MEMORY64) {
		/* Failed. */
		return ENODEV;
	}

	/* Returns the computed result. */
	return bar->size >= AX211_BAR0_MINIMUM_SIZE ? 0 : ENODEV;
}

/* Reads one naturally aligned identity CSR from retained BAR0. */
static uint32_t
ax211_pci_read32(
	const struct ax211_pci_controller *controller,
	unsigned offset)
{
	volatile uint8_t *registers;
	uint32_t value;

	registers = controller->mapping.address;
	value = *(volatile uint32_t *)(registers + offset);
	kern_io_read_barrier();

	/* Returns the computed result. */
	return value;
}

/* Validates the frozen SO-or-SOF MAC and Garfield Peak RF identities. */
static int
ax211_pci_hardware_validate(
	uint32_t hardware_revision,
	uint32_t radio_identity)
{
	uint32_t mac_type;
	uint32_t radio_type;

	mac_type = (hardware_revision & AX211_CSR_HW_REV_TYPE_MASK) >>
		   AX211_CSR_HW_REV_TYPE_SHIFT;
	radio_type = (radio_identity & AX211_CSR_HW_RF_TYPE_MASK) >>
		     AX211_CSR_HW_RF_TYPE_SHIFT;

	/* Handles the mac type condition. */
	if (mac_type != AX211_CSR_HW_REV_TYPE_SO &&
	    mac_type != AX211_CSR_HW_REV_TYPE_SOF) {
		/* Failed. */
		return ENODEV;
	}

	/* Handles the radio type condition. */
	if (radio_type != AX211_CSR_HW_RF_TYPE_GF)
		return ENODEV;

	/* Returns the computed result. */
	return (radio_identity & AX211_CSR_HW_RF_CDB) == 0U ? 0 : ENODEV;
}

/* Derives the already validated private MMIO profile. */
static int
ax211_pci_profile(
	struct ax211_pci_controller *controller,
	struct intel_ax211_mmio_profile *profile)
{
	uint32_t mac_type;

	/* Handles the controller availability. */
	if (controller == NULL || profile == NULL)
		return EINVAL;
	memset(profile, 0, sizeof(*profile));
	mac_type =
		(controller->hardware_revision & AX211_CSR_HW_REV_TYPE_MASK) >>
		AX211_CSR_HW_REV_TYPE_SHIFT;
	profile->mac_type = (uint16_t)mac_type;
	profile->rf_type = INTEL_AX211_MMIO_RF_GF;
	profile->umac_prph_offset = INTEL_AX211_MMIO_UMAC_PRPH_OFFSET;

	/* Succeeded. */
	return 0;
}

/* Acquires the persistent BAR/decode lease with bus mastering disabled. */
static int
ax211_pci_acquire(
	struct ax211_pci_controller *controller)
{
	int function_result;
	struct drv_pci_bar bar;
	uint16_t command;
	int error;

	/* Checks the operation status. */
	error = drv_pci_device_claim_bar(controller->device, 0U);
	if (error != 0)
		return error;
	controller->bar_claimed = 1U;

	/* Checks the operation status. */
	error = drv_pci_device_bar(controller->device, 0U, &bar);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = ax211_pci_bar_validate(&bar);
	if (error != 0)
		return error;
	controller->original_bar = bar;
	controller->original_bar_valid = 1U;

	/* Checks the operation status. */
	error = drv_pci_device_save_enable_state(controller->device,
						 &controller->enable_state);
	if (error != 0)
		return error;
	controller->state_saved = 1U;

	/* Checks the operation status. */
	error = drv_pci_device_set_bus_master(controller->device, false);
	if (error != 0)
		return error;
	controller->bar_may_have_moved = 1U;

	/* Checks the operation status. */
	error = drv_pci_device_map_bar(controller->device, 0U,
				       DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE |
					       DRV_PCI_MAP_NOCACHE,
				       &controller->mapping);
	if (error != 0)
		return error;
	controller->bar_mapped = 1U;

	/* Handles the address availability. */
	if (controller->mapping.address == NULL ||
	    (controller->mapping.type != DRV_PCI_BAR_MEMORY32 &&
	     controller->mapping.type != DRV_PCI_BAR_MEMORY64) ||
	    controller->mapping.size < AX211_BAR0_MINIMUM_SIZE ||
	    controller->mapping.size > bar.size) {
		/* Failed. */
		return EIO;
	}

	/* Checks the operation status. */
	error = drv_pci_device_enable_memory(controller->device);
	if (error == 0) {
		error = drv_pci_device_set_bus_master(controller->device,
						      false);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = drv_pci_device_config_read16(
			controller->device, AX211_PCI_COMMAND, &command);
	}

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the command condition. */
	if ((command & AX211_PCI_COMMAND_MEMORY) == 0U ||
	    (command & AX211_PCI_COMMAND_MASTER) != 0U) {
		/* Failed. */
		return EIO;
	}
	controller->hardware_revision =
		ax211_pci_read32(controller, AX211_CSR_HW_REV);
	controller->radio_identity =
		ax211_pci_read32(controller, AX211_CSR_HW_RF_ID);

	/* Obtains the ax211 pci hardware validate result. */
	function_result = ax211_pci_hardware_validate(
		controller->hardware_revision, controller->radio_identity);

	/* Returns the computed result. */
	return function_result;
}

/* Restores BAR0 if the host mapper assigned a temporary MMIO address. */
static int
ax211_pci_restore_bar(
	struct ax211_pci_controller *controller)
{
	struct drv_pci_bar current_bar;
	int error;

	/* Handles the controller condition. */
	if (!controller->bar_may_have_moved)
		return 0;

	/* Handles the controller condition. */
	if (!controller->original_bar_valid)
		return EIO;

	/* Checks the operation status. */
	error = drv_pci_device_bar(controller->device, 0U, &current_bar);
	if (error != 0)
		return error;

	/* Handles the current bar condition. */
	if (current_bar.bus_address != controller->original_bar.bus_address) {
		/* Checks the operation status. */
		error = drv_pci_device_assign_bar(
			controller->device, 0U,
			controller->original_bar.bus_address);
		if (error != 0)
			return error;
	}

	controller->bar_may_have_moved = 0U;
	controller->original_bar_valid = 0U;

	/* Succeeded. */
	return 0;
}

/* Releases the PCI lease in reverse order, optionally retaining for retry. */
static int
ax211_pci_release_resources(
	struct ax211_pci_controller *controller,
	int result,
	int retain_on_failure)
{
	int bar_error;
	int quiesce_error;
	int restore_error;

	bar_error = 0;
	restore_error = 0;

	/* Handles the controller condition. */
	if (controller->bar_mapped) {
		drv_pci_device_unmap_bar(controller->device,
					 &controller->mapping);
		controller->bar_mapped = 0U;
	}

	/* Checks the operation status. */
	bar_error = ax211_pci_restore_bar(controller);
	if (bar_error == 0 && controller->state_saved) {
		/* Checks the operation status. */
		restore_error = drv_pci_device_restore_enable_state(
			controller->device, &controller->enable_state);
		if (restore_error == 0)
			controller->state_saved = 0U;
	}

	/* Checks the operation status. */
	if (bar_error != 0 || restore_error != 0) {
		/* Checks the operation status. */
		quiesce_error = drv_pci_device_set_bus_master(
			controller->device, false);
		if (result == 0 && quiesce_error != 0)
			result = quiesce_error;

		/* Checks the operation status. */
		if (retain_on_failure)
			return bar_error != 0 ? bar_error : restore_error;
	}

	/* Handles the controller condition. */
	if (controller->bar_claimed) {
		drv_pci_device_release_bar(controller->device, 0U);
		controller->bar_claimed = 0U;
	}

	/* Checks the operation status. */
	if (bar_error != 0)
		return bar_error;

	/* Checks the operation status. */
	if (restore_error != 0)
		return restore_error;

	/* Returns the computed result. */
	return result;
}

/* Keeps an incompletely restored controller bound for checked detach retry. */
static int
ax211_pci_quarantine(
	struct ax211_pci_controller *controller,
	int failure)
{
	uint16_t command;
	int error;

	controller->ready = 0U;
	controller->quarantined = 1U;

	/* Checks the operation status. */
	error = drv_pci_device_set_bus_master(controller->device, false);
	if (error == 0) {
		error = drv_pci_device_config_read16(
			controller->device, AX211_PCI_COMMAND, &command);
	}

	/* Checks the operation status. */
	if (error == 0 && (command & AX211_PCI_COMMAND_MASTER) != 0U)
		error = EIO;
	if (error != 0)
		failure = error;

	/* Handles the controller condition. */
	if (!controller->driver_data_set) {
		/* Checks the operation status. */
		error = drv_pci_device_set_driver_data(controller->device,
						       controller);
		if (error == 0)
			controller->driver_data_set = 1U;
	}

	ax211_pci_list_add(controller);
	kern_logf("intel-ax211: controller restoration quarantined (%d)\n",
		   failure);

	/* Returning success is intentional: the PCI core must bind detach. */
	return 0;
}

/* Publishes one persistent controller to the refresh registry. */
static void
ax211_pci_list_add(
	struct ax211_pci_controller *controller)
{
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);

	controller->next = ax211_controllers;
	ax211_controllers = controller;
	controller->listed = 1U;

	spin_unlock_irqrestore(&ax211_registry_lock, enabled);
}

/* Retires a controller from admission unless refresh still owns it. */
static int
ax211_pci_list_remove(
	struct ax211_pci_controller *controller)
{
	unsigned long enabled;

	/* Handles the controller condition. */
	enabled = spin_lock_irqsave(&ax211_registry_lock);
	if (controller->refresh_busy) {
		spin_unlock_irqrestore(&ax211_registry_lock, enabled);

		/* Failed. */
		return EBUSY;
	}

	/* Handles the controller condition. */
	if (!controller->detaching)
		controller->detaching = 1U;

	spin_unlock_irqrestore(&ax211_registry_lock, enabled);

	/* Succeeded. */
	return 0;
}

/* Forgets a fully restored controller immediately before final release. */
static void
ax211_pci_list_forget(
	struct ax211_pci_controller *controller)
{
	struct ax211_pci_controller **link;
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);

	/* Process each linked entry. */
	for (link = &ax211_controllers; *link != NULL; link = &(*link)->next) {
		/* Handles the link condition. */
		if (*link != controller)
			continue;
		*link = controller->next;
		controller->next = NULL;
		controller->listed = 0U;
		break;
	}

	spin_unlock_irqrestore(&ax211_registry_lock, enabled);
}

/* Recovers a quarantined controller whose PCI driver-data write failed. */
static struct ax211_pci_controller *
ax211_pci_list_find_device(
	struct drv_pci_device *device)
{
	struct ax211_pci_controller *controller;
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);

	/* Process each linked entry. */
	for (controller = ax211_controllers; controller != NULL;
	     controller = controller->next) {
		/* Handles the controller condition. */
		if (controller->device == device)
			break;
	}

	spin_unlock_irqrestore(&ax211_registry_lock, enabled);

	/* Returns the computed result. */
	return controller;
}

/* Claims one unpublished controller exactly once in this refresh epoch. */
static struct ax211_pci_controller *
ax211_pci_refresh_claim(
	unsigned epoch)
{
	struct ax211_pci_controller *controller;
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);

	/* Process each linked entry. */
	for (controller = ax211_controllers; controller != NULL;
	     controller = controller->next) {
		/* Handles the controller condition. */
		if (controller->detaching || controller->refresh_busy ||
		    !controller->ready || controller->quarantined ||
		    controller->station_attached ||
		    controller->refresh_epoch == epoch)
			continue;
		controller->refresh_epoch = epoch;
		controller->refresh_busy = 1U;
		break;
	}

	spin_unlock_irqrestore(&ax211_registry_lock, enabled);

	/* Returns the computed result. */
	return controller;
}

/* Releases the registry lease after MMIO and publication have finished. */
static void
ax211_pci_refresh_release(
	struct ax211_pci_controller *controller)
{
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);

	controller->refresh_busy = 0U;

	spin_unlock_irqrestore(&ax211_registry_lock, enabled);
}

/* Reads identity under NIC ownership, stops again, then publishes once. */
static int
ax211_pci_refresh_one(
	struct ax211_pci_controller *controller)
{
	uint8_t mac_address[AX211_MAC_ADDRESS_SIZE];
	int hardware_attempted;
	int error;
	int stop_error;

	memset(mac_address, 0, sizeof(mac_address));
	mutex_lock(&controller->lifecycle_lock);

	hardware_attempted = 0;

	/* Handles the controller condition. */
	if (controller->detaching || !controller->ready ||
	    controller->quarantined)
		error = ENODEV;
	else
		error = ax211_pci_partial_discard(controller);
	if (error == 0) {
		hardware_attempted = 1;
		error = drv_intel_ax211_mmio_prepare_card_hw(&controller->mmio);
	}

	/* Checks the operation status. */
	if (error == INTEL_AX211_MMIO_OK)
		error = drv_intel_ax211_mmio_sw_reset(&controller->mmio);
	if (error == INTEL_AX211_MMIO_OK)
		error = drv_intel_ax211_mmio_apm_init(&controller->mmio);
	if (error == INTEL_AX211_MMIO_OK) {
		error = drv_intel_ax211_mmio_read_mac(&controller->mmio,
						      mac_address);
	}

	/* Handles the hardware attempted condition. */
	stop_error = INTEL_AX211_MMIO_OK;
	if (hardware_attempted)
		stop_error = drv_intel_ax211_mmio_stop(&controller->mmio);
	if (error == INTEL_AX211_MMIO_OK && stop_error != INTEL_AX211_MMIO_OK)
		error = stop_error;
	if (error == INTEL_AX211_MMIO_OK)
		error = ax211_pci_publish(controller, mac_address);
	ax211_pci_scrub(mac_address, sizeof(mac_address));

	mutex_unlock(&controller->lifecycle_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Transactionally publishes an administratively-down WLAN station graph. */
static int
ax211_pci_publish(
	struct ax211_pci_controller *controller,
	const uint8_t mac_address[AX211_MAC_ADDRESS_SIZE])
{
	struct wlan_scan_profile profile;
	struct net_device *device;
	struct wlan_station *station;
	unsigned index;
	int error;
	int gone_error;

	/* Handles the device availability. */
	device = net_device_alloc();
	if (device == NULL)
		return ENOSPC;
	device->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->mtu = AX211_MTU;
	memcpy(device->hwaddr, mac_address, AX211_MAC_ADDRESS_SIZE);
	device->hwaddr_len = AX211_MAC_ADDRESS_SIZE;
	device->capabilities = NET_DEVICE_CAP_WLAN;
	device->ops = &ax211_net_ops;
	device->driver_data = controller;
	error = ENOSPC;
	/* Process each remaining element. */
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		memcpy(device->name, "wlan", 4U);
		device->name[4] = (char)('0' + index);
		device->name[5] = '\0';

		/* Checks the operation status. */
		error = net_device_create(device);
		if (error != EEXIST)
			break;
	}

	/* Checks the operation status. */
	if (error != 0) {
		device->driver_data = NULL;
		net_device_destroy(device);

		/* Failed. */
		return error;
	}

	controller->net_device = device;
	controller->net_live = 1U;
	station = NULL;

	/* Checks the operation status. */
	error = net_device_set_carrier(device, 0);
	if (error == 0) {
		ax211_pci_scan_profile(&profile);
		error = wlan_station_attach(device, &ax211_radio_ops,
					    controller, &profile, &station);
	} else {
		memset(&profile, 0, sizeof(profile));
	}

	ax211_pci_scrub(&profile, sizeof(profile));

	/* Checks the operation status. */
	if (error == 0 && station == NULL)
		error = EIO;
	if (error != 0) {
		/* Checks the operation status. */
		gone_error = ax211_pci_partial_discard(controller);
		if (gone_error != 0)
			return gone_error;

		/* Failed. */
		return error;
	}

	controller->station = station;
	controller->station_attached = 1U;

	/* Succeeded. */
	return 0;
}

/* Builds the deliberately passive, fixed 2.4-GHz channel 1--11 profile. */
static void
ax211_pci_scan_profile(
	struct wlan_scan_profile *profile)
{
	unsigned channel;

	memset(profile, 0, sizeof(*profile));
	profile->channel_count = AX211_PASSIVE_CHANNEL_COUNT;
	/* Process each remaining element. */
	for (channel = 1U; channel <= profile->channel_count; channel++) {
		profile->channels[channel - 1U].channel = channel;
		profile->channels[channel - 1U].center_frequency_mhz =
			2407U + channel * 5U;
	}
}

/* Retires an unpublished net-device shell before a publication retry. */
static int
ax211_pci_partial_discard(
	struct ax211_pci_controller *controller)
{
	struct net_device *device;
	unsigned net_live;
	int error;

	/* Handles the station availability. */
	if (controller->station_attached || controller->station != NULL)
		return 0;

	/* Handles the device availability. */
	device = controller->net_device;
	if (device == NULL)
		return 0;
	net_live = controller->net_live;

	mutex_unlock(&controller->lifecycle_lock);

	(void)net_device_set_carrier(device, 0);

	/* Handles the net live condition. */
	if (net_live) {
		error = net_device_gone(device);
		mutex_lock(&controller->lifecycle_lock);
		if (error != 0)
			return error;
		controller->net_live = 0U;
	} else {
		mutex_lock(&controller->lifecycle_lock);
	}

	device->driver_data = NULL;
	controller->net_device = NULL;

	mutex_unlock(&controller->lifecycle_lock);

	net_device_destroy(device);
	mutex_lock(&controller->lifecycle_lock);

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 pci lifecycle deadline operation. */
static uint64_t
ax211_pci_lifecycle_deadline(
	void)
{
	uint64_t now;

	/* Handles the now condition. */
	now = clock_ticks();
	if (now > UINT64_MAX - AX211_LIFECYCLE_JOIN_TICKS)
		return UINT64_MAX;

	/* Returns the computed result. */
	return now + AX211_LIFECYCLE_JOIN_TICKS;
}

/* Pins the common station while the lifecycle lock excludes retirement. */
static int
ax211_pci_station_pin_locked(
	struct ax211_pci_controller *controller,
	struct wlan_station **station)
{
	/* Handles the controller availability. */
	if (controller == NULL || station == NULL)
		return EINVAL;
	*station = NULL;
	/* Handles the station availability. */
	if (!controller->station_attached || controller->station == NULL)
		return ENODEV;

	/* Handles the controller condition. */
	if (controller->operations_active == UINT_MAX)
		return EOVERFLOW;
	controller->operations_active++;
	*station = controller->station;
	/* Succeeded. */
	return 0;
}

/* Pins the common station across a deliberate lifecycle-lock drop. */
static int
ax211_pci_operation_enter_locked(
	struct ax211_pci_controller *controller,
	struct wlan_station **station)
{
	int error;

	/* Handles the controller availability. */
	if (controller == NULL || station == NULL)
		return EINVAL;
	*station = NULL;
	/* Handles the station availability. */
	if (!controller->operation_admission_open ||
	    !controller->station_attached || controller->station == NULL) {
		/* Failed. */
		return ENODEV;
	}

	/* Obtains the ax211 pci station pin locked result. */
	error = ax211_pci_station_pin_locked(controller, station);

	/* Returns the computed result. */
	return error;
}

/* Supports the ax211 pci operation leave locked operation. */
static void
ax211_pci_operation_leave_locked(
	struct ax211_pci_controller *controller)
{
	/* Handles the controller availability. */
	if (controller == NULL || controller->operations_active == 0U)
		__builtin_trap();
	controller->operations_active--;
}

/* Joins callers that already copied the station pointer before retirement. */
static int
ax211_pci_operations_join_locked(
	struct ax211_pci_controller *controller,
	uint64_t deadline)
{
	/* Handles the controller availability. */
	if (controller == NULL || deadline == 0U)
		return EINVAL;
	/* Continue while the operation condition remains true. */
	while (controller->operations_active != 0U) {
		/* Checks the clock ticks result. */
		if (clock_ticks() >= deadline)
			return ETIMEDOUT;
		mutex_unlock(&controller->lifecycle_lock);
		sched_yield();
		mutex_lock(&controller->lifecycle_lock);
	}

	/* Succeeded. */
	return 0;
}

/* Common close is a checked admission join and may need a bounded retry. */
static int
ax211_pci_station_close_wait(
	struct wlan_station *station,
	uint64_t deadline)
{
	int result;

	/* Handles the station availability. */
	if (station == NULL || deadline == 0U)
		return ENODEV;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Checks the operation result. */
		result = wlan_station_close(station);
		if (result != EBUSY)
			return result;

		/* Checks the clock ticks result. */
		if (clock_ticks() >= deadline)
			return ETIMEDOUT;
		sched_yield();
	}
}

/* Records one uncertain live-runtime failure for the poll recovery owner. */
static void
ax211_pci_recovery_latch_locked(
	struct ax211_pci_controller *controller,
	int error)
{
	/* Handles the controller availability. */
	if (controller == NULL)
		return;

	/* Handles the controller condition. */
	if (!controller->recovery_pending && !controller->recovery_running) {
		controller->recovery_error = error > 0 ? error : EIO;
		controller->recovery_generation =
			controller->connection_generation;
		controller->recovery_pending = 1U;
		kern_logf("intel-ax211: recovery latched error=%d "
			   "generation=%u\n",
			   controller->recovery_error,
			   (unsigned)controller->recovery_generation);
	}

	controller->operation_admission_open = 0U;

	/* Handles the net device availability. */
	if (controller->net_device != NULL)
		net_device_schedule_poll(controller->net_device);
}

/* Retires common link truth before globally stopping an uncertain epoch. */
static int
ax211_pci_recovery_run_locked(
	struct ax211_pci_controller *controller)
{
	struct net_device *device;
	struct wlan_station *station;
	uint64_t deadline;
	uint64_t generation;
	int carrier_error;
	int join_error;
	int link_error;
	int pin_error;
	int stop_error;
	int failure;

	/* Handles the controller availability. */
	if (controller == NULL)
		return EINVAL;
	failure = controller->recovery_error > 0 ? controller->recovery_error
						 : EIO;
	generation = controller->recovery_generation != 0U
			     ? controller->recovery_generation
			     : controller->connection_generation;
	controller->operation_admission_open = 0U;
	deadline = ax211_pci_lifecycle_deadline();
	join_error = ax211_pci_operations_join_locked(controller, deadline);
	device = controller->net_device;

	/* Checks the operation status. */
	if (join_error != 0) {
		/*
		 * The active lease may still be inside common->radio code. Keep
		 * every DMA owner intact and retry from a later poll rather
		 * than racing a reset or release against that caller.
		 */
		mutex_unlock(&controller->lifecycle_lock);
		carrier_error =
			device != NULL ? net_device_set_carrier(device, 0) : 0;
		mutex_lock(&controller->lifecycle_lock);

		/* Checks the operation status. */
		if (carrier_error != 0 && carrier_error != ENODEV) {
			kern_logf("intel-ax211: recovery carrier-down failed "
				   "(%d)\n",
				   carrier_error);
		}

		kern_logf("intel-ax211: recovery operation join failed (%d)\n",
			   join_error);

		/* Returns the computed result. */
		return join_error;
	}

	controller->recovery_pending = 0U;
	controller->recovery_error = 0;
	controller->recovery_generation = 0U;
	controller->recovery_running = 1U;
	station = NULL;
	pin_error = generation != 0U
			    ? ax211_pci_station_pin_locked(controller, &station)
			    : 0;

	mutex_unlock(&controller->lifecycle_lock);

	carrier_error = device != NULL ? net_device_set_carrier(device, 0) : 0;
	link_error = station != NULL ? wlan_station_report_link_loss(
					       station, generation, failure)
				     : 0;
	mutex_lock(&controller->lifecycle_lock);

	/* Handles the station availability. */
	if (station != NULL)
		ax211_pci_operation_leave_locked(controller);
	stop_error = ax211_pci_session_stop(controller);
	controller->recovery_running = 0U;
	controller->quarantined = stop_error != 0;

	/* Checks the operation status. */
	if (pin_error != 0 && pin_error != ENODEV) {
		kern_logf("intel-ax211: recovery station pin failed (%d)\n",
			   pin_error);
	}

	/* Checks the operation status. */
	if (carrier_error != 0 && carrier_error != ENODEV) {
		kern_logf("intel-ax211: recovery carrier-down failed (%d)\n",
			   carrier_error);
	}

	/* Checks the operation status. */
	if (link_error != 0 && link_error != ESTALE && link_error != ENODEV &&
	    link_error != ENOTCONN && link_error != EALREADY) {
		kern_logf(
			"intel-ax211: recovery link retirement failed (%d)\n",
			link_error);
	}

	/* Checks the operation status. */
	if (stop_error != 0) {
		/* An inactive, quarantined runtime cannot drive another poll retry. */
		ax211_pci_stop_defer_locked(controller, stop_error);
		kern_logf("intel-ax211: fatal poll cleanup failed (%d)\n",
			   stop_error);
	}

	/* Returns the computed result. */
	return stop_error != 0 ? stop_error : failure;
}

/* Retires the visible network graph through every checked common barrier. */
static int
ax211_pci_graph_detach(
	struct ax211_pci_controller *controller)
{
	struct wlan_station *station;
	uint64_t deadline;
	int error;

	/* Checks the operation status. */
	error = wlan_station_stop_cancel(controller->station);
	if (error != 0)
		return error;
	controller->operation_admission_open = 0U;
	deadline = ax211_pci_lifecycle_deadline();

	/* Checks the operation status. */
	error = ax211_pci_operations_join_locked(controller, deadline);
	if (error != 0)
		return error;

	/* Handles the station availability. */
	station = controller->station;
	if (station == NULL || !controller->station_attached)
		return 0;

	/*
	 * Both joins may synchronously invoke radio callbacks.  Admission is
	 * already closed; drop the hardware lifecycle lock across those edges.
	 */

	mutex_unlock(&controller->lifecycle_lock);

	/* Checks the operation status. */
	error = ax211_pci_station_close_wait(station,
					     ax211_pci_lifecycle_deadline());
	if (error == 0 || error == ENODEV)
		error = wlan_station_detach(station);
	mutex_lock(&controller->lifecycle_lock);
	if (error != 0 && error != ENODEV)
		return error;

	/* Handles the controller condition. */
	if (controller->station == station) {
		controller->station = NULL;
		controller->station_attached = 0U;
	}

	/* Succeeded. */
	return 0;
}

/* Stops every open-generation owner while the lifecycle mutex is held. */
static int
ax211_pci_session_stop(
	struct ax211_pci_controller *controller)
{
	int association_result;
	int coordinator_result;
	int hardware_quiesced;
	int release_result;
	int result;
	unsigned long enabled;

	/* Handles the controller availability. */
	if (controller == NULL)
		return EINVAL;

	/* Handles the controller condition. */
	if (controller->runtime_active &&
	    controller->connection_generation != 0U) {
		kern_logf("intel-ax211: stopping connection generation=%u "
			   "association-phase=%u step=%u\n",
			   (unsigned)controller->connection_generation,
			   controller->association.phase,
			   controller->association.step);
	}

	controller->operation_admission_open = 0U;
	controller->session_stopped = 0U;
	controller->recovery_pending = 0U;
	controller->recovery_error = 0;
	controller->recovery_generation = 0U;
	association_result = 0;
	hardware_quiesced = 0;

	/* Handles the controller condition. */
	if (controller->association_initialized && controller->runtime_active) {
		controller->control_deadline_ticks =
			ax211_pci_lifecycle_deadline();
		association_result = ax211_pci_assoc_rollback(
			controller, controller->connection_generation);
	}

	enabled = spin_lock_irqsave(&controller->interrupt_lock);

	controller->runtime_active = 0U;
	controller->poll_reschedule = 0U;

	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);

	ax211_pci_deferred_event_clear(controller);
	ax211_pci_scan_clear(controller);
	result = 0;

	/* Handles the controller condition. */
	if (controller->runtime_initialized) {
		/* Handles the controller condition. */
		coordinator_result = INTEL_AX211_RUNTIME_START_OK;
		if (controller->runtime_start.state ==
		    INTEL_AX211_RUNTIME_START_STATE_RUNNING) {
			coordinator_result = drv_intel_ax211_runtime_start_stop(
				&controller->runtime_start);
		} else if (
			controller->runtime_start.state ==
				INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED ||
			controller->runtime_start.state ==
				INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED_NO_DMA) {
			coordinator_result =
				drv_intel_ax211_runtime_start_cleanup(
					&controller->runtime_start);
		}

		/* Handles the coordinator result condition. */
		if (coordinator_result != INTEL_AX211_RUNTIME_START_OK) {
			result = EIO;
		} else {
			controller->runtime_initialized = 0U;
			hardware_quiesced = 1;
		}
	}

	/* Checks the operation result. */
	if (result == 0 && controller->boot_initialized &&
	    (controller->boot.state == INTEL_AX211_BOOT_STATE_STOP_REQUIRED ||
	     controller->boot.state ==
		     INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA)) {
		coordinator_result =
			drv_intel_ax211_boot_cleanup(&controller->boot);

		/* Handles the coordinator result condition. */
		if (coordinator_result != INTEL_AX211_BOOT_OK)
			result = EIO;
	}

	/* Checks the operation result. */
	if (result == 0 &&
	    (controller->irq_established || controller->irq_allocated)) {
		/* Checks the operation result. */
		result = ax211_pci_interrupt_drain(controller);
		if (result == 0)
			hardware_quiesced = 1;
	}

	/*
	 * A successful runtime stop includes interrupt drain, controller reset,
	 * and PCI bus-master disable, which is the global queue-1 DMA barrier.
	 */
	if (result == 0 && controller->tx_ring_allocated) {
		/* Handles the release result condition. */
		release_result = drv_intel_ax211_tx_ring_release(
			&controller->tx_ring, hardware_quiesced);
		if (release_result != INTEL_AX211_TX_RING_OK)
			result = EIO;
		else
			controller->tx_ring_allocated = 0U;
	}

	/* Checks the operation result. */
	if (result == 0) {
		ax211_pci_connection_clear(controller);
		controller->session_stopped = 1U;

		/*
		 * A global reset is stronger than a failed per-resource
		 * rollback.
		 */
		association_result = 0;
	}

	/* Checks the operation result. */
	if (result == 0 && controller->boot_initialized)
		controller->boot_initialized = 0U;

	/* Checks the operation result. */
	if (result == 0 && association_result != 0)
		result = association_result;

	/* Returns the computed result. */
	return result;
}

/* Transfers an incomplete global stop to the independent retirement owner. */
static void
ax211_pci_stop_defer_locked(
	struct ax211_pci_controller *controller,
	int error)
{
	/* An existing close owner will publish its own checked result. */
	if (controller->close_pending)
		return;

	/* Keeps new operations out until the retained hardware epoch is retired. */
	controller->close_pending = 1U;
	controller->operation_admission_open = 0U;

	/* Stops common timer callbacks and arms its bounded retirement retry. */
	wlan_station_stop_request(controller->station);
	wlan_station_stop_complete(controller->station, error);

	/* Preserves the first failure without repeating firmware rollback logs. */
	kern_logf("intel-ax211: global stop deferred error=%d runtime-state=%u "
		   "irq=%u/%u dma-retained=%u\n",
		   error, controller->runtime_start.state,
		   controller->irq_established, controller->irq_allocated,
		   controller->runtime_start.dma_prepared);
}

/* Reads the PCIe LTR-enable policy without broadening the device match. */
static int
ax211_pci_ltr_enabled(
	struct ax211_pci_controller *controller,
	int *enabled)
{
	uint16_t control;
	unsigned capability;
	int error;

	/* Handles the controller availability. */
	if (controller == NULL || enabled == NULL)
		return EINVAL;
	*enabled = 0;
	capability = 0U;

	/* Checks the operation status. */
	error = drv_pci_device_find_capability(
		controller->device, AX211_PCIE_CAPABILITY, &capability);
	if (error == ENOENT)
		return 0;
	if (error != 0 || capability > UINT32_MAX - AX211_PCIE_DEVICE_CONTROL2)
		return error != 0 ? error : EIO;

	/* Checks the operation status. */
	error = drv_pci_device_config_read16(
		controller->device, capability + AX211_PCIE_DEVICE_CONTROL2,
		&control);
	if (error == 0)
		*enabled = (control & AX211_PCIE_DEVICE_CONTROL2_LTR) != 0U;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* The exclusive MSI-X handler only latches work for a safe context. */
static int
ax211_pci_irq(
	void *argument)
{
	struct ax211_pci_controller *controller;
	struct net_device *device;
	unsigned long enabled;
	int schedule;

	/* Handles the controller availability. */
	controller = argument;
	if (controller == NULL)
		return 0;
	enabled = spin_lock_irqsave(&controller->interrupt_lock);

	controller->irq_latched = 1U;
	device = controller->net_device;
	schedule = controller->receive_enabled && controller->runtime_active &&
		   device != NULL;

	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);

	/* Handles the schedule condition. */
	if (schedule)
		net_device_schedule_poll(device);

	/* Reports operation failure. */
	return 1;
}

/* Flushes the software latch before admitting one new nonzero epoch. */
static int
ax211_pci_receive_epoch_begin(
	void *argument,
	uint32_t generation)
{
	struct ax211_pci_controller *controller;
	unsigned long enabled;
	int result;

	/* Handles the controller availability. */
	controller = argument;
	if (controller == NULL || generation == 0U)
		return -1;
	enabled = spin_lock_irqsave(&controller->interrupt_lock);

	controller->receive_enabled = 0U;
	controller->irq_latched = 0U;
	controller->active_dma = NULL;
	memset(controller->last_receive_header, 0,
	       sizeof(controller->last_receive_header));
	controller->last_receive_length = 0U;
	controller->last_receive_version = 0U;

	/* Checks the operation result. */
	result = controller->irq_allocated || controller->irq_established ? -1
									  : 0;
	if (result == 0) {
		controller->hardware_epoch = generation;
		controller->receive_enabled = 1U;
	}

	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);

	/* Returns the computed result. */
	return result;
}

/* Binds coherent rings and one exact MSI-X vector transactionally. */
static int
ax211_pci_transport_bind(
	void *argument,
	struct intel_ax211_dma_resources *dma,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport,
	uint32_t generation)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_transport_ring_memory memory;
	unsigned count;
	unsigned long enabled;
	int bus_master_enabled;
	int result;

	/* Checks the drv pci device dma result. */
	controller = argument;
	if (controller == NULL || dma == NULL || mmio != &controller->mmio ||
	    transport != &controller->transport || generation == 0U ||
	    dma->device == NULL ||
	    dma->device != drv_pci_device_dma(controller->device)) {
		/* Reports operation failure. */
		return -1;
	}
	enabled = spin_lock_irqsave(&controller->interrupt_lock);

	result = !controller->receive_enabled ||
				 controller->hardware_epoch != generation ||
				 controller->irq_allocated ||
				 controller->irq_established
			 ? -1
			 : 0;

	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);

	/* Checks the drv dma device is coherent result. */
	if (result != 0 || !drv_dma_device_is_coherent(dma->device))
		return -1;

	memset(&memory, 0, sizeof(memory));
	bus_master_enabled = 0;

	/* Checks the operation result. */
	result = drv_pci_device_set_bus_master(controller->device, true);
	if (result == 0) {
		bus_master_enabled = 1;
		result = drv_intel_ax211_transport_backend_init(
			&controller->transport_backend, mmio,
			&controller->backend, dma);
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_TRANSPORT_BACKEND_OK) {
		result = drv_intel_ax211_transport_backend_ring_memory(
			&controller->transport_backend, &memory);
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_TRANSPORT_BACKEND_OK) {
		result = drv_intel_ax211_transport_init(
			transport, drv_intel_ax211_transport_backend_ops(),
			&controller->transport_backend, &mmio->profile,
			&memory);
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_TRANSPORT_OK) {
		count = 0U;
		result = drv_pci_device_allocate_irqs(
			controller->device, DRV_PCI_IRQ_ALLOW_MSIX, 1U, 1U,
			&controller->irq, &count);

		/* Checks the remaining item count. */
		if (count != 0U) {
			controller->irq_count = count;
			controller->irq_allocated = 1U;
		}

		/* Checks the operation result. */
		if (result == 0 &&
		    (count != 1U || controller->irq.type != DRV_PCI_IRQ_MSIX))
			result = EIO;
	}

	/* Checks the operation result. */
	if (result == 0) {
		/* Checks the operation result. */
		result = drv_pci_device_establish_irq(
			controller->device, &controller->irq, ax211_pci_irq,
			controller, "intel-ax211", &controller->irq_cookie);
		if (result == 0)
			controller->irq_established = 1U;
	}

	/* Checks the operation result. */
	if (result == 0) {
		enabled = spin_lock_irqsave(&controller->interrupt_lock);
		controller->active_dma = dma;
		spin_unlock_irqrestore(&controller->interrupt_lock, enabled);

		/* Succeeded. */
		return 0;
	}

	/* Handles the controller condition. */
	if (controller->irq_established) {
		/* Checks the drv pci device disestablish irq checked result. */
		if (drv_pci_device_disestablish_irq_checked(
			    controller->device, controller->irq_cookie) == 0) {
			controller->irq_cookie = NULL;
			controller->irq_established = 0U;
		}
	}

	/* Handles the controller condition. */
	if (!controller->irq_established && controller->irq_allocated) {
		drv_pci_device_free_irqs(controller->device, &controller->irq,
					 controller->irq_count);
		controller->irq_count = 0U;
		controller->irq_allocated = 0U;
	}

	/* Checks the drv pci device set bus master result. */
	if (bus_master_enabled &&
	    drv_pci_device_set_bus_master(controller->device, false) != 0)
		result = EIO;
	enabled = spin_lock_irqsave(&controller->interrupt_lock);

	controller->receive_enabled = 0U;
	controller->irq_latched = 0U;
	controller->active_dma = NULL;

	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);

	/* Returns the computed result. */
	return result == 0 ? -1 : result;
}

/* Copies one exact event before returning its DMA buffer to firmware. */
static int
ax211_pci_receive_event(
	void *argument,
	uint64_t deadline_us,
	uint8_t *bytes,
	size_t capacity,
	struct intel_ax211_boot_received_event *event)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_transport_causes causes;
	struct intel_ax211_transport_rx_completion completion;
	struct drv_dma_buffer *buffer;
	uint64_t now;
	uint64_t remaining;
	uint32_t frame_length;
	uint32_t generation;
	size_t total_length;
	unsigned long enabled;
	int delay_result;
	int receive_result;
	int replenish_result;
	int rearm_result;
	int result;

	/* Handles the controller availability. */
	controller = argument;
	if (controller == NULL || bytes == NULL || capacity == 0U ||
	    event == NULL) {
		/* Returns the computed result. */
		return INTEL_AX211_BOOT_RECEIVE_IO;
	}
	memset(event, 0, sizeof(*event));
	receive_result = INTEL_AX211_BOOT_RECEIVE_IO;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		enabled = spin_lock_irqsave(&controller->interrupt_lock);
		generation = controller->hardware_epoch;
		result = controller->receive_enabled && generation != 0U &&
					 controller->active_dma != NULL
				 ? 0
				 : -1;
		controller->irq_latched = 0U;
		spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
		if (result != 0)
			return INTEL_AX211_BOOT_RECEIVE_IO;
		memset(&causes, 0, sizeof(causes));

		/* Checks the operation result. */
		result = drv_intel_ax211_transport_interrupt_claim(
			&controller->transport, &causes);
		if (result != INTEL_AX211_TRANSPORT_OK)
			return INTEL_AX211_BOOT_RECEIVE_IO;
		controller->command_fh_causes |= causes.flow_handler;
		controller->command_hw_causes |= causes.hardware;
		controller->command_raw_fh_causes |= causes.raw_flow_handler;
		controller->command_raw_hw_causes |= causes.raw_hardware;

		/* Checks the operation status. */
		if ((causes.flow_handler &
		     INTEL_AX211_TRANSPORT_FH_CAUSE_ERROR) != 0U ||
		    (causes.hardware & INTEL_AX211_TRANSPORT_HW_FATAL_CAUSES) !=
			    0U) {
			kern_logf("intel-ax211: fatal firmware interrupt "
				   "fh=%08x hw=%08x raw-fh=%08x raw-hw=%08x\n",
				   causes.flow_handler, causes.hardware,
				   causes.raw_flow_handler,
				   causes.raw_hardware);

			/* Returns the computed result. */
			return INTEL_AX211_BOOT_RECEIVE_IO;
		}

		/*
		 * AX210-generation firmware configures RFH before raising the
		 * hardware-ALIVE cause.  Only then may the host publish its
		 * first receive credit; an earlier doorbell can be lost across
		 * that setup.
		 */
		if (!controller->transport.rx_active &&
		    (causes.hardware & INTEL_AX211_TRANSPORT_HW_CAUSE_ALIVE) !=
			    0U) {
			/* Checks the operation result. */
			result = drv_intel_ax211_transport_activate_rx(
				&controller->transport);
			if (result != INTEL_AX211_TRANSPORT_OK)
				return INTEL_AX211_BOOT_RECEIVE_IO;
		}

		memset(&completion, 0, sizeof(completion));

		/* Checks the operation result. */
		result = controller->transport.rx_active
				 ? drv_intel_ax211_transport_rx_next(
					   &controller->transport, &completion)
				 : INTEL_AX211_TRANSPORT_STALE;
		if (result == INTEL_AX211_TRANSPORT_OK) {
			/* Handles the completion condition. */
			if (completion.buffer_id >=
			    controller->active_dma->rx_buffer_count) {
				/* Returns the computed result. */
				return INTEL_AX211_BOOT_RECEIVE_IO;
			}
			buffer = &controller->active_dma
					  ->rx_buffer[completion.buffer_id];
			kern_io_read_barrier();
			frame_length =
				buffer->address == NULL || buffer->size < 4U
					? 0U
					: ax211_pci_get_le32(buffer->address) &
						  0x3fffU;

			/* Handles the frame length condition. */
			total_length = (size_t)frame_length + 4U;
			if (frame_length >= 4U &&
			    total_length <= INTEL_AX211_BOOT_EVENT_CAPACITY &&
			    total_length <= buffer->size &&
			    total_length <= capacity) {
				memcpy(bytes, buffer->address, total_length);
				receive_result = INTEL_AX211_BOOT_RECEIVE_OK;
			} else {
				receive_result = INTEL_AX211_BOOT_RECEIVE_IO;
			}

			replenish_result =
				drv_intel_ax211_transport_rx_replenish(
					&controller->transport,
					buffer->device_address);
			rearm_result =
				drv_intel_ax211_transport_interrupt_rearm(
					&controller->transport);

			/* Handles the replenish result condition. */
			if (replenish_result != INTEL_AX211_TRANSPORT_OK ||
			    rearm_result != INTEL_AX211_TRANSPORT_OK) {
				/* Returns the computed result. */
				return INTEL_AX211_BOOT_RECEIVE_IO;
			}

			/* Handles the receive result condition. */
			if (receive_result != INTEL_AX211_BOOT_RECEIVE_OK)
				return receive_result;
			event->length = total_length;
			event->generation = generation;
			event->notification_version =
				ax211_pci_notification_version(bytes,
							       total_length);
			memcpy(controller->last_receive_header, bytes,
			       sizeof(controller->last_receive_header));
			controller->last_receive_length = total_length;
			controller->last_receive_version =
				event->notification_version;

			/* Returns the computed result. */
			return INTEL_AX211_BOOT_RECEIVE_OK;
		}

		/* Checks the operation result. */
		rearm_result = drv_intel_ax211_transport_interrupt_rearm(
			&controller->transport);
		if (result != INTEL_AX211_TRANSPORT_STALE ||
		    rearm_result != INTEL_AX211_TRANSPORT_OK) {
			/* Returns the computed result. */
			return INTEL_AX211_BOOT_RECEIVE_IO;
		}

		/* Checks the ax211 pci clock us result. */
		if (ax211_pci_clock_us(controller, &now) != 0)
			return INTEL_AX211_BOOT_RECEIVE_IO;

		/* Handles the now condition. */
		if (now >= deadline_us)
			return INTEL_AX211_BOOT_RECEIVE_TIMEOUT;
		remaining = deadline_us - now;

		/* Handles the delay result condition. */
		delay_result = controller->mmio.ops->delay_us(
			controller->mmio.argument,
			(uint32_t)(remaining < AX211_RECEIVE_POLL_DELAY_US
					   ? remaining
					   : AX211_RECEIVE_POLL_DELAY_US));
		if (delay_result != 0)
			return INTEL_AX211_BOOT_RECEIVE_IO;
	}
}

/* Publishes the prepared fragmented PNVM table under NIC ownership. */
static int
ax211_pci_publish_pnvm(
	void *argument,
	struct intel_ax211_dma_resources *dma)
{
	struct ax211_pci_controller *controller;
	int result;
	int unlock_result;

	/* Handles the controller availability. */
	controller = argument;
	if (controller == NULL || dma == NULL ||
	    dma != controller->active_dma || !dma->pnvm_prepared ||
	    dma->pnvm_table.address == NULL ||
	    dma->pnvm_table.device_address == 0U || dma->pnvm_count == 0U) {
		/* Reports operation failure. */
		return -1;
	}
	kern_io_write_barrier();

	/* Checks the operation result. */
	result = drv_intel_ax211_mmio_nic_lock(&controller->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return -1;
	result = drv_intel_ax211_mmio_prph_write32(
		&controller->mmio,
		controller->mmio.profile.umac_prph_offset +
			AX211_UREG_DOORBELL_TO_ISR6,
		AX211_UREG_DOORBELL_PNVM);
	unlock_result = drv_intel_ax211_mmio_nic_unlock(&controller->mmio);

	/* Returns the computed result. */
	return result == INTEL_AX211_MMIO_OK &&
			       unlock_result == INTEL_AX211_MMIO_OK
		       ? 0
		       : -1;
}

/* The MSI-X path needs no ICT reset, but ownership must still be balanced. */
static int
ax211_pci_post_alive(
	void *argument,
	const struct intel_ax211_protocol_alive *alive)
{
	struct ax211_pci_controller *controller;
	int result;
	int unlock_result;

	/* Handles the controller availability. */
	controller = argument;
	if (controller == NULL || alive == NULL ||
	    alive->status != INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK) {
		/* Reports operation failure. */
		return -1;
	}

	/* Checks the operation result. */
	result = drv_intel_ax211_mmio_nic_lock(&controller->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return -1;
	unlock_result = drv_intel_ax211_mmio_nic_unlock(&controller->mmio);

	/* Returns the computed result. */
	return unlock_result == INTEL_AX211_MMIO_OK ? 0 : -1;
}

/* Masks, disestablishes, and retires the sole MSI-X lifetime in order. */
static int
ax211_pci_interrupt_drain(
	void *argument)
{
	struct ax211_pci_controller *controller;
	unsigned long enabled;
	int disable_result;
	int result;

	/* Handles the controller availability. */
	controller = argument;
	if (controller == NULL)
		return -1;
	enabled = spin_lock_irqsave(&controller->interrupt_lock);

	controller->receive_enabled = 0U;
	controller->irq_latched = 0U;

	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);

	/* Handles the controller condition. */
	disable_result = INTEL_AX211_TRANSPORT_OK;
	if (controller->transport.msix_configured &&
	    (controller->irq_established || controller->irq_allocated)) {
		disable_result = drv_intel_ax211_transport_disable_interrupts(
			&controller->transport);
	}

	/* Handles the controller condition. */
	result = 0;
	if (controller->irq_established) {
		/* Checks the operation result. */
		result = drv_pci_device_disestablish_irq_checked(
			controller->device, controller->irq_cookie);
		if (result == 0) {
			controller->irq_cookie = NULL;
			controller->irq_established = 0U;
		}
	}

	/* Checks the operation result. */
	if (result == 0 && controller->irq_allocated) {
		drv_pci_device_free_irqs(controller->device, &controller->irq,
					 controller->irq_count);
		controller->irq_count = 0U;
		controller->irq_allocated = 0U;
	}

	/* Checks the drv pci device set bus master result. */
	if (result == 0 &&
	    drv_pci_device_set_bus_master(controller->device, false) != 0)
		result = EIO;
	if (result == 0) {
		enabled = spin_lock_irqsave(&controller->interrupt_lock);
		controller->active_dma = NULL;
		spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
	}

	/* Checks the operation result. */
	if (result != 0)
		return -1;

	/* Returns the computed result. */
	return disable_result == INTEL_AX211_TRANSPORT_OK ? 0 : -1;
}

/* Supports the ax211 pci clock us operation. */
static int
ax211_pci_clock_us(
	void *argument,
	uint64_t *time_us)
{
	int error;
	struct ax211_pci_controller *controller;

	/* Handles the controller availability. */
	controller = argument;
	if (controller == NULL || time_us == NULL ||
	    controller->mmio.ops == NULL ||
	    controller->mmio.ops->clock_us == NULL) {
		/* Reports operation failure. */
		return -1;
	}

	/* Computes the function result. */
	error = controller->mmio.ops->clock_us(
		controller->mmio.argument, time_us);

	/* Returns the computed result. */
	return error;
}

/* Supports the ax211 pci nic lock operation. */
static int
ax211_pci_nic_lock(
	void *argument)
{
	int error;
	struct ax211_pci_controller *controller;

	controller = argument;

	/* Computes the function result. */
	error = controller != NULL && drv_intel_ax211_mmio_nic_lock(
							&controller->mmio) ==
							INTEL_AX211_MMIO_OK
				  ? 0
				  : -1;

	/* Returns the computed result. */
	return error;
}

/* Supports the ax211 pci nic unlock operation. */
static int
ax211_pci_nic_unlock(
	void *argument)
{
	int error;
	struct ax211_pci_controller *controller;

	controller = argument;

	/* Computes the function result. */
	error = controller != NULL && drv_intel_ax211_mmio_nic_unlock(
							&controller->mmio) ==
							INTEL_AX211_MMIO_OK
				  ? 0
				  : -1;

	/* Returns the computed result. */
	return error;
}

/* TX_CMD uses the narrow data-queue header, whose wire group is legacy. */
static int
ax211_pci_tx_event(
	const struct intel_ax211_event *event)
{
	uint8_t group;

	/* Handles the event availability. */
	if (event == NULL || event->command.opcode != INTEL_AX211_TX_OPCODE)
		return 0;
	group = event->flags &
		(uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;

	/* Returns the computed result. */
	return group == INTEL_AX211_PROTOCOL_GROUP_LEGACY ||
	       group == INTEL_AX211_TX_GROUP;
}

/* Pins layouts absent from the RX wire header at the PCI boundary. */
static uint8_t
ax211_pci_notification_version(
	const uint8_t *bytes,
	size_t length)
{
	uint8_t group;
	uint8_t opcode;
	uint8_t queue;

	/* Handles the bytes availability. */
	if (bytes == NULL || length < INTEL_AX211_EVENT_HEADER_SIZE)
		return 0U;
	opcode = bytes[4U];
	group = bytes[5U] & (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	queue = bytes[7U];

	/* Handles the group condition. */
	if ((group == INTEL_AX211_PROTOCOL_GROUP_LEGACY ||
	     group == INTEL_AX211_TX_GROUP) &&
	    opcode == INTEL_AX211_TX_OPCODE) {
		/* Returns the computed result. */
		return INTEL_AX211_TX_NOTIFICATION_VERSION;
	}

	/* Handles the queue condition. */
	if ((queue & 0x80U) == 0U)
		return 0U;

	/* Handles the group condition. */
	if (group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
	    opcode == INTEL_AX211_PROTOCOL_ALIVE_OPCODE) {
		/* Returns the computed result. */
		return INTEL_AX211_PROTOCOL_ALIVE_VERSION;
	}

	/* Handles the group condition. */
	if (group == INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM &&
	    opcode == INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_OPCODE) {
		/* Returns the computed result. */
		return INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_VERSION;
	}

	/* Handles the group condition. */
	if (group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
	    opcode == INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE) {
		/* Returns the computed result. */
		return INTEL_AX211_PROTOCOL_UNKNOWN_VERSION;
	}

	/* Handles the group condition. */
	if (group == 0U && opcode == 0xc1U)
		return 5U;

	/* Handles the group condition. */
	if (group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    opcode == INTEL_AX211_ASSOC_SESSION_NOTIFICATION_OPCODE) {
		/* Returns the computed result. */
		return INTEL_AX211_ASSOC_SESSION_NOTIFICATION_LAYOUT_VERSION;
	}

	/* Handles the group condition. */
	if (group == 1U && opcode == 0xc8U)
		return 6U;

	/* Handles the group condition. */
	if (group == 0U && (opcode == 0x0fU || opcode == 0xb5U))
		return 1U;

	/* Returns the computed result. */
	return INTEL_AX211_PROTOCOL_UNKNOWN_VERSION;
}

/* Binds the live command owner to one firmware epoch and copied radio data. */
static int
ax211_pci_scan_initialize(
	struct ax211_pci_controller *controller)
{
	int error;
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_runtime_mcc mcc;
	struct wlan_scan_profile profile;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || !controller->runtime_initialized ||
	    controller->runtime_start.state !=
		    INTEL_AX211_RUNTIME_START_STATE_RUNNING ||
	    controller->hardware_epoch == 0U || controller->net_device == NULL) {
		/* Failed. */
		return EINVAL;
	}
	memset(&table, 0, sizeof(table));
	memset(&mcc, 0, sizeof(mcc));
	table.bytes = controller->runtime_start.command_version_bytes;
	table.count = INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT;

	/* Checks the operation result. */
	result = drv_intel_ax211_runtime_start_mcc(&controller->runtime_start,
						   &mcc);
	if (result == INTEL_AX211_RUNTIME_START_OK) {
		result = drv_intel_ax211_rx_api89_validate(&table) ==
					 INTEL_AX211_RX_OK
				 ? INTEL_AX211_SCAN_SESSION_OK
				 : INTEL_AX211_SCAN_SESSION_UNSUPPORTED;
	} else {
		result = INTEL_AX211_SCAN_SESSION_FAILED;
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_SCAN_SESSION_OK) {
		result = drv_intel_ax211_scan_session_init(
			&controller->scan_session,
			&controller->runtime_start.commands, &table,
			&controller->runtime_start.nvm, &mcc,
			controller->net_device->hwaddr,
			controller->hardware_epoch);
	}

	ax211_pci_scrub(&mcc, sizeof(mcc));

	/* Checks the operation result. */
	if (result != INTEL_AX211_SCAN_SESSION_OK) {
		/* Obtains the ax211 pci scan result errno result. */
		error = ax211_pci_scan_result_errno(result);

		/* Failed. */
		return error;
	}

	memset(&profile, 0, sizeof(profile));

	/* Checks the operation result. */
	result = ax211_pci_runtime_scan_profile(controller, &profile);
	if (result == 0) {
		result = controller->station == NULL
				 ? ENODEV
				 : wlan_station_scan_profile_update(
					   controller->station, &profile);
	}

	ax211_pci_scrub(&profile, sizeof(profile));

	/* Checks the operation result. */
	if (result != 0)
		return result;
	controller->scan_step_index = 0U;
	controller->scan_initialized = 1U;

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 pci channel frequency operation. */
static uint32_t
ax211_pci_channel_frequency(
	uint8_t channel)
{
	/* Handles the channel condition. */
	if (channel >= 1U && channel <= 13U)
		return 2407U + 5U * (uint32_t)channel;

	/* Handles the channel condition. */
	if (channel == 14U)
		return 2484U;

	/* Handles the channel condition. */
	if ((channel >= 36U && channel <= 144U && (channel - 36U) % 4U == 0U) ||
	    (channel >= 149U && channel <= 181U && (channel - 149U) % 4U == 0U)) {
		/* Returns the computed result. */
		return 5000U + 5U * (uint32_t)channel;
	}

	/* Returns the computed result. */
	return 0U;
}

/* Supports the ax211 pci runtime scan profile operation. */
static int
ax211_pci_runtime_scan_profile(
	const struct ax211_pci_controller *controller,
	struct wlan_scan_profile *profile)
{
	uint32_t frequency;
	const struct intel_ax211_scan_profile *source;
	size_t index;

	/* Handles the controller availability. */
	if (controller == NULL || profile == NULL)
		return EINVAL;

	/* Handles the source condition. */
	source = &controller->scan_session.full_profile;
	if (source->channel_count == 0U ||
	    source->channel_count > WLAN_SCAN_CHANNEL_MAX) {
		/* Failed. */
		return EINVAL;
	}
	memset(profile, 0, sizeof(*profile));
	profile->channel_count = (uint32_t)source->channel_count;
	/* Process each remaining element. */
	for (index = 0U; index < source->channel_count; index++) {
		/* Handles the frequency condition. */
		frequency = ax211_pci_channel_frequency(source->channel[index]);
		if (frequency == 0U)
			return EINVAL;
		profile->channels[index].channel = source->channel[index];
		profile->channels[index].center_frequency_mhz = frequency;
		profile->channels[index].flags = WLAN_SCAN_CHANNEL_OFFLOADED_DWELL;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 pci scan channel present operation. */
static int
ax211_pci_scan_channel_present(
	const struct ax211_pci_controller *controller,
	uint8_t channel)
{
	size_t index;

	/* Handles the controller availability. */
	if (controller == NULL || !controller->scan_initialized)
		return 0;
	/* Process each remaining element. */
	for (index = 0U;
	     index < controller->scan_session.full_profile.channel_count;
	     index++) {
		/* Handles the controller condition. */
		if (controller->scan_session.full_profile.channel[index] ==
		    channel) {
			/* Reports operation failure. */
			return 1;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Retires every common-generation reference before the hardware epoch stops. */
static void
ax211_pci_scan_clear(
	struct ax211_pci_controller *controller)
{
	/* Handles the controller availability. */
	if (controller == NULL)
		return;
	controller->scan_initialized = 0U;
	controller->scan_step_index = 0U;
	controller->bss_staging_generation = 0U;
	controller->bss_published_generation = 0U;
	controller->bss_staging_initialized = 0U;
	controller->bss_published_initialized = 0U;
	ax211_pci_scrub(&controller->scan_session,
			sizeof(controller->scan_session));
	ax211_pci_scrub(&controller->bss_staging_cache,
			sizeof(controller->bss_staging_cache));
	ax211_pci_scrub(&controller->bss_published_cache,
			sizeof(controller->bss_published_cache));
	ax211_pci_scrub(controller->runtime_frame,
			sizeof(controller->runtime_frame));
}

/* Drops an unpublished scan generation without touching the last snapshot. */
static void
ax211_pci_bss_staging_discard(
	struct ax211_pci_controller *controller)
{
	/* Handles the controller availability. */
	if (controller == NULL)
		return;
	controller->bss_staging_generation = 0U;
	controller->bss_staging_initialized = 0U;
	ax211_pci_scrub(&controller->bss_staging_cache,
			sizeof(controller->bss_staging_cache));
}

/* Atomically replaces the private snapshot after the complete scan succeeds. */
static int
ax211_pci_bss_staging_publish(
	struct ax211_pci_controller *controller,
	uint64_t generation)
{
	/* Handles the controller availability. */
	if (controller == NULL || generation == 0U ||
	    !controller->bss_staging_initialized) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the controller condition. */
	if (controller->bss_staging_generation != generation)
		return ESTALE;
	controller->bss_published_cache = controller->bss_staging_cache;
	controller->bss_published_generation = generation;
	controller->bss_published_initialized = 1U;
	ax211_pci_bss_staging_discard(controller);

	/* Succeeded. */
	return 0;
}

/* Decodes only the already copied, epoch-tagged event envelope. */
static int
ax211_pci_event_message(
	const uint8_t *bytes,
	size_t length,
	const struct intel_ax211_boot_received_event *received,
	struct intel_ax211_event *event,
	struct intel_ax211_protocol_message *message)
{
	/* Checks the drv intel ax211 event decode result. */
	if (bytes == NULL || received == NULL || event == NULL ||
	    message == NULL || received->generation == 0U ||
	    received->length != length ||
	    drv_intel_ax211_event_decode(bytes, length, event) !=
		    INTEL_AX211_OK ||
	    event->payload_offset > length ||
	    event->payload_length != length - event->payload_offset) {
		/* Failed. */
		return EIO;
	}
	memset(message, 0, sizeof(*message));
	message->opcode = event->command.opcode;
	message->group = event->flags &
			 (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	message->version = received->notification_version;
	message->flags = event->flags;
	message->queue = event->queue;
	message->index = event->index;
	message->generation = received->generation;
	message->payload = bytes + event->payload_offset;
	message->payload_length = event->payload_length;

	/* Handles the ax211 pci tx event condition. */
	if (ax211_pci_tx_event(event)) {
		message->group = INTEL_AX211_TX_GROUP;
		message->version = INTEL_AX211_TX_NOTIFICATION_VERSION;
	}

	/* Succeeded. */
	return 0;
}

/* Delivers the asynchronous request/abort acknowledgement in poll context. */
static int
ax211_pci_scan_command_dispatch(
	struct ax211_pci_controller *controller,
	const uint8_t *bytes,
	size_t length,
	uint64_t now)
{
	uint8_t phase;
	int report_result;
	int result;

	/* Handles the controller condition. */
	if (!controller->scan_initialized)
		return 0;

	/* Handles the phase condition. */
	phase = controller->scan_session.phase;
	if (phase == INTEL_AX211_SCAN_SESSION_WAIT_START_ACK) {
		/* Checks the operation result. */
		result = drv_intel_ax211_scan_session_start_ack(
			&controller->scan_session, bytes, length,
			controller->hardware_epoch, now);
		if (result == INTEL_AX211_SCAN_SESSION_OK) {
			/*
			 * This report only latches under station->lock and
			 * wakes the worker; unlike TX completion, it cannot
			 * synchronously call a radio op.
			 */

			/* Handles the report result condition. */
			report_result = wlan_station_report_scan_channel_ready(
				controller->station,
				controller->scan_session.common_generation,
				controller->scan_step_index);
			if (report_result != 0 && report_result != ESTALE &&
			    report_result != ENODEV) {
				ax211_pci_scan_report_error(
					controller,
					INTEL_AX211_SCAN_SESSION_FAILED);
			}

			/* Succeeded. */
			return 0;
		}
	} else if (phase == INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK) {
		result = drv_intel_ax211_scan_session_abort_ack(
			&controller->scan_session, bytes, length,
			controller->hardware_epoch, now);
	} else {
		/* Succeeded. */
		return 0;
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_SCAN_SESSION_ABORTED)
		ax211_pci_bss_staging_discard(controller);

	/* Checks the operation result. */
	if (result == INTEL_AX211_SCAN_SESSION_OK ||
	    result == INTEL_AX211_SCAN_SESSION_ABORTED ||
	    result == INTEL_AX211_SCAN_SESSION_DUPLICATE ||
	    result == INTEL_AX211_SCAN_SESSION_STALE ||
	    result == INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER) {
		/* Succeeded. */
		return 0;
	}
	kern_logf("intel-ax211: scan command failed phase=%u result=%d "
		   "length=%u opcode=%02x group=%02x index=%02x queue=%02x\n",
		   phase, result, (unsigned)length,
		   length > 4U ? bytes[4U] : 0U, length > 5U ? bytes[5U] : 0U,
		   length > 6U ? bytes[6U] : 0U, length > 7U ? bytes[7U] : 0U);
	ax211_pci_scan_report_error(controller, result);

	/* Succeeded. */
	return 0;
}

/* Accepts only scan notifications belonging to the current hardware epoch. */
static int
ax211_pci_scan_notification_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_protocol_message *message,
	uint64_t now)
{
	const uint8_t *payload;
	struct intel_ax211_scan_session_event reported;
	int result;
	int report_result;

	/* Handles the controller condition. */
	if (!controller->scan_initialized)
		return 0;
	memset(&reported, 0, sizeof(reported));

	/* Checks the operation status. */
	result = drv_intel_ax211_scan_session_notification(
		&controller->scan_session, message, now, &reported);
	if (result == INTEL_AX211_SCAN_SESSION_FAILED &&
	    message->payload != NULL && message->payload_length >= 16U) {
		payload = message->payload;

		/* Reports a notification this driver does not recognize. */
		kern_logf(
			"intel-ax211: unexpected scan notification opcode=%02x "
			"flags=%02x "
			"length=%u uid=%02x%02x%02x%02x schedule=%u "
			"iteration=%u "
			"status=%u ebs=%u "
			"tail=%02x%02x%02x%02x%02x%02x%02x%02x\n",
			message->opcode, message->flags,
			(unsigned)message->payload_length, payload[3U],
			payload[2U], payload[1U], payload[0U], payload[4U],
			payload[5U], payload[6U], payload[7U], payload[8U],
			payload[9U], payload[10U], payload[11U], payload[12U],
			payload[13U], payload[14U], payload[15U]);
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_SCAN_SESSION_ABORTED)
		ax211_pci_bss_staging_discard(controller);

	/* Advances the common channel only after a validated firmware event. */
	if (result == INTEL_AX211_SCAN_SESSION_COMPLETE) {
		report_result = wlan_station_report_scan_channel_complete(
			controller->station,
			reported.common_generation,
			controller->scan_step_index);
		if (report_result != 0 && report_result != ESTALE &&
		    report_result != ENODEV) {
			ax211_pci_scan_report_error(controller,
			    INTEL_AX211_SCAN_SESSION_FAILED);
		}
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_SCAN_SESSION_OK ||
	    result == INTEL_AX211_SCAN_SESSION_COMPLETE ||
	    result == INTEL_AX211_SCAN_SESSION_ABORTED ||
	    result == INTEL_AX211_SCAN_SESSION_DUPLICATE ||
	    result == INTEL_AX211_SCAN_SESSION_STALE) {
		/* Succeeded. */
		return 0;
	}
	ax211_pci_scan_report_error(controller, result);

	/* Succeeded. */
	return 0;
}

/* Normalizes MPDU v5, reports scan frames, and dispatches connection frames. */
static int
ax211_pci_rx_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_protocol_message *message)
{
	struct intel_ax211_bss_entry bss_entry;
	struct intel_ax211_rx_mpdu mpdu;
	uint16_t frame_control;
	uint16_t subtype;
	int dispatch_result;
	int result;

	/* Decodes the frame into cleared staging. */
	memset(&bss_entry, 0, sizeof(bss_entry));
	memset(&mpdu, 0, sizeof(mpdu));
	ax211_pci_scrub(controller->runtime_frame,
			sizeof(controller->runtime_frame));

	/* Checks the operation result. */
	result = drv_intel_ax211_rx_mpdu_decode(
		message, controller->hardware_epoch, controller->runtime_frame,
		sizeof(controller->runtime_frame), &mpdu);
	if (result != INTEL_AX211_RX_OK) {
		/*
		 * A malformed or unsupported over-the-air frame is local to
		 * this receive slot.  Dropping it must not quarantine the
		 * firmware epoch.
		 */
		ax211_pci_scrub(controller->runtime_frame,
				sizeof(controller->runtime_frame));

		/* Succeeded. */
		return 0;
	}

	frame_control = (uint16_t)((uint16_t)mpdu.frame[0U] |
				   ((uint16_t)mpdu.frame[1U] << 8));
	subtype = frame_control & 0x00f0U;

	/* Handles the controller condition. */
	if (controller->scan_initialized &&
	    controller->scan_session.phase ==
		    INTEL_AX211_SCAN_SESSION_RUNNING &&
	    (frame_control & 0x000cU) == 0U &&
	    (subtype == 0x0080U || subtype == 0x0050U)) {
		/* Checks the drv intel ax211 bss decode result. */
		if (controller->bss_staging_initialized &&
		    controller->bss_staging_generation ==
			    controller->scan_session.common_generation &&
		    drv_intel_ax211_bss_decode(
			    &mpdu, controller->scan_session.common_generation,
			    controller->hardware_epoch,
			    &bss_entry) == INTEL_AX211_BSS_OK) {
			/*
			 * Use the same monotonic observation point immediately
			 * before both private and common admission decisions.
			 */
			bss_entry.last_seen_ticks = clock_ticks();
			(void)drv_intel_ax211_bss_cache_observe(
				&controller->bss_staging_cache, &bss_entry);
		}

		/*
		 * Scan observation copies/latches data without entering radio
		 * callbacks.
		 */
		(void)wlan_station_report_scan_frame(
			controller->station,
			controller->scan_session.common_generation, mpdu.frame,
			mpdu.length, mpdu.rssi_dbm, mpdu.channel);
	}

	/* Handles the controller condition. */
	dispatch_result = 0;
	if (controller->connection_generation != 0U) {
		dispatch_result = ax211_pci_connection_rx_dispatch(
			controller, &mpdu, frame_control);
	}

	ax211_pci_scrub(&bss_entry, sizeof(bss_entry));
	ax211_pci_scrub(controller->runtime_frame,
			sizeof(controller->runtime_frame));

	/* Returns the computed result. */
	return dispatch_result;
}

/* Converts private coordinator outcomes to one common scan error latch. */
static void
ax211_pci_scan_report_error(
	struct ax211_pci_controller *controller,
	int result)
{
	int error;

	/* Handles the controller availability. */
	if (controller == NULL || controller->station == NULL ||
	    !controller->scan_initialized ||
	    controller->scan_session.common_generation == 0U) {
		/* Returns the computed result. */
		return;
	}

	/* Checks the operation status. */
	error = ax211_pci_scan_result_errno(result);
	if (error != 0) {
		kern_logf("intel-ax211: scan generation=%u failed result=%d "
			   "error=%d phase=%u\n",
			   (unsigned)controller->scan_session.common_generation,
			   result, error, controller->scan_session.phase);
		ax211_pci_bss_staging_discard(controller);

		/*
		 * This latch only wakes common work; lifecycle_lock remains the
		 * owner.
		 */
		(void)wlan_station_report_scan_error(
			controller->station,
			controller->scan_session.common_generation, error);
	}
}

/* Supports the ax211 pci scan result errno operation. */
static int
ax211_pci_scan_result_errno(
	int result)
{
	/* Dispatch the selected operation case. */
	switch (result) {
	case INTEL_AX211_SCAN_SESSION_OK:
	case INTEL_AX211_SCAN_SESSION_COMPLETE:
	case INTEL_AX211_SCAN_SESSION_ABORTED:
		/* Succeeded. */
		return 0;
	case INTEL_AX211_SCAN_SESSION_INVALID:
		/* Failed. */
		return EINVAL;
	case INTEL_AX211_SCAN_SESSION_UNSUPPORTED:
		/* Failed. */
		return ENOTSUP;
	case INTEL_AX211_SCAN_SESSION_BUSY:
		/* Failed. */
		return EBUSY;
	case INTEL_AX211_SCAN_SESSION_STALE:
	case INTEL_AX211_SCAN_SESSION_DUPLICATE:
	case INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER:
		/* Failed. */
		return ESTALE;
	case INTEL_AX211_SCAN_SESSION_TIMEOUT:
		/* Failed. */
		return ETIMEDOUT;
	case INTEL_AX211_SCAN_SESSION_COMMAND:
	case INTEL_AX211_SCAN_SESSION_FAILED:
	default:
		/* Failed. */
		return EIO;
	}
}

/* Dispatches one recycled-safe copy; no DMA pointer crosses this boundary. */
static int
ax211_pci_runtime_event_dispatch(
	struct ax211_pci_controller *controller,
	const uint8_t *bytes,
	size_t length,
	const struct intel_ax211_boot_received_event *event)
{
	int error;
	struct intel_ax211_event decoded;
	struct intel_ax211_protocol_message message;
	uint64_t now;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || bytes == NULL || event == NULL ||
	    event->generation == 0U ||
	    event->generation != controller->hardware_epoch ||
	    event->length != length) {
		/* Failed. */
		return EINVAL;
	}
	memset(&decoded, 0, sizeof(decoded));
	memset(&message, 0, sizeof(message));

	/* Checks the operation result. */
	result = ax211_pci_event_message(bytes, length, event, &decoded,
					 &message);
	if (result != 0)
		return result;

	/* Checks the ax211 pci clock us result. */
	if (ax211_pci_clock_us(controller, &now) != 0)
		return EIO;

	/* Handles the ax211 pci tx event condition. */
	if (ax211_pci_tx_event(&decoded)) {
		/* Obtains the ax211 pci tx dispatch result. */
		error = ax211_pci_tx_dispatch(controller, &message);

		/* Failed. */
		return error;
	}

	/* Handles the decoded condition. */
	if ((decoded.queue & 0x80U) == 0U) {
		/* Obtains the ax211 pci scan command dispatch result. */
		error = ax211_pci_scan_command_dispatch(
			controller, bytes, length, now);

		/* Failed. */
		return error;
	}

	/* Handles the message condition. */
	if (message.group == INTEL_AX211_SCAN_GROUP_LEGACY &&
	    (message.opcode == INTEL_AX211_SCAN_COMPLETE_OPCODE ||
	     message.opcode == INTEL_AX211_SCAN_ITERATION_COMPLETE_OPCODE)) {
		/* Obtains the ax211 pci scan notification dispatch result. */
		error = ax211_pci_scan_notification_dispatch(
			controller, &message, now);

		/* Failed. */
		return error;
	}

	/* Handles the message condition. */
	if (message.group == INTEL_AX211_RX_MPDU_GROUP &&
	    message.opcode == INTEL_AX211_RX_MPDU_OPCODE) {
		/* Obtains the ax211 pci rx dispatch result. */
		error = ax211_pci_rx_dispatch(controller, &message);

		/* Failed. */
		return error;
	}

	/* Handles the message condition. */
	if (message.group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    message.opcode == INTEL_AX211_ASSOC_SESSION_NOTIFICATION_OPCODE &&
	    controller->association_initialized) {
		/* Checks the operation result. */
		result = drv_intel_ax211_assoc_session_event_accept(
			&controller->association, &message,
			controller->connection_generation,
			controller->hardware_epoch);
		if (result == INTEL_AX211_ASSOC_SESSION_EXPIRED ||
		    result == INTEL_AX211_ASSOC_EVENT_IGNORED ||
		    result == INTEL_AX211_ASSOC_DUPLICATE ||
		    result == INTEL_AX211_ASSOC_STALE) {
			/* Succeeded. */
			return 0;
		}

		/* Failed. */
		return EIO;
	}

	/* Succeeded. */
	return 0;
}

/* Retains an exact post-recycle notification until the common gate unwinds. */
static int
ax211_pci_deferred_event_enqueue(
	struct ax211_pci_controller *controller,
	const uint8_t *bytes,
	const struct intel_ax211_boot_received_event *received)
{
	struct ax211_pci_deferred_event *slot;
	unsigned index;

	/* Handles the controller availability. */
	if (controller == NULL || bytes == NULL || received == NULL ||
	    received->length == 0U ||
	    received->length > INTEL_AX211_BOOT_EVENT_CAPACITY ||
	    received->generation == 0U ||
	    received->generation != controller->hardware_epoch) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the controller condition. */
	if (controller->deferred_event_count >= AX211_DEFERRED_EVENT_LIMIT)
		return ENOBUFS;
	index = (controller->deferred_event_head +
		 controller->deferred_event_count) %
		AX211_DEFERRED_EVENT_LIMIT;
	slot = &controller->deferred_event[index];
	memset(slot, 0, sizeof(*slot));
	slot->received = *received;
	memcpy(slot->bytes, bytes, received->length);
	controller->deferred_event_count++;

	/* Handles the net device availability. */
	if (controller->net_device != NULL)
		net_device_schedule_poll(controller->net_device);

	/* Succeeded. */
	return 0;
}

/* Dispatches one copied notification only from the outer poll safe point. */
static int
ax211_pci_deferred_event_drain_one(
	struct ax211_pci_controller *controller)
{
	struct ax211_pci_deferred_event *slot;
	struct intel_ax211_boot_received_event received;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || controller->deferred_event_count == 0U)
		return ENOENT;

	/* Handles the controller condition. */
	if (controller->deferred_event_draining)
		return EBUSY;

	/* Handles the slot condition. */
	slot = &controller->deferred_event[controller->deferred_event_head];
	if (slot->received.length == 0U ||
	    slot->received.length > sizeof(controller->deferred_dispatch_event)) {
		/* Failed. */
		return EIO;
	}
	received = slot->received;
	memcpy(controller->deferred_dispatch_event, slot->bytes,
	       received.length);
	ax211_pci_scrub(slot, sizeof(*slot));
	controller->deferred_event_head =
		(controller->deferred_event_head + 1U) %
		AX211_DEFERRED_EVENT_LIMIT;
	controller->deferred_event_count--;
	controller->deferred_event_draining = 1U;
	result = ax211_pci_runtime_event_dispatch(
		controller, controller->deferred_dispatch_event,
		received.length, &received);
	ax211_pci_scrub(controller->deferred_dispatch_event, received.length);
	controller->deferred_event_draining = 0U;

	/* Returns the computed result. */
	return result;
}

/* Scrubs every copied notification when its firmware epoch is retired. */
static void
ax211_pci_deferred_event_clear(
	struct ax211_pci_controller *controller)
{
	/* Handles the controller availability. */
	if (controller == NULL)
		return;
	controller->deferred_event_head = 0U;
	controller->deferred_event_count = 0U;
	controller->deferred_event_draining = 0U;
	ax211_pci_scrub(controller->deferred_event,
			sizeof(controller->deferred_event));
	ax211_pci_scrub(controller->deferred_dispatch_event,
			sizeof(controller->deferred_dispatch_event));
}

/* Supports the ax211 pci get le32 operation. */
static uint32_t
ax211_pci_get_le32(
	const uint8_t bytes[4])
{
	/* Returns the computed result. */
	return (uint32_t)bytes[0U] | ((uint32_t)bytes[1U] << 8) |
	       ((uint32_t)bytes[2U] << 16) | ((uint32_t)bytes[3U] << 24);
}

/* Coherent TX allocations need only an ordered visibility boundary. */
static int
ax211_pci_tx_sync_for_device(
	void *argument,
	const struct drv_dma_buffer *buffer,
	size_t offset,
	size_t length)
{
	struct ax211_pci_controller *controller;

	/* Checks the drv dma device is coherent result. */
	controller = argument;
	if (controller == NULL || buffer == NULL || buffer->address == NULL ||
	    buffer->device_address == 0U || offset > buffer->size ||
	    length > buffer->size - offset ||
	    !controller->runtime_initialized ||
	    !drv_dma_device_is_coherent(controller->tx_ring.dma_device)) {
		/* Reports operation failure. */
		return -1;
	}
	kern_io_write_barrier();

	/* Succeeded. */
	return 0;
}

/* Rings only the API89 queue-1 write-pointer doorbell. */
static int
ax211_pci_tx_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	int error;
	struct ax211_pci_controller *controller;

	/* Handles the controller availability. */
	controller = argument;
	if (controller == NULL ||
	    offset != INTEL_AX211_TX_RING_WRITE_POINTER_REGISTER ||
	    controller->mmio.ops == NULL ||
	    controller->mmio.ops->csr_write32 == NULL) {
		/* Reports operation failure. */
		return -1;
	}
	kern_io_write_barrier();

	/* Computes the function result. */
	error = controller->mmio.ops->csr_write32(
		controller->mmio.argument, offset, value);

	/* Returns the computed result. */
	return error;
}

/* Supports the ax211 pci assoc clock us operation. */
static uint64_t
ax211_pci_assoc_clock_us(
	void *argument)
{
	uint64_t now;

	/* Checks the ax211 pci clock us result. */
	if (ax211_pci_clock_us(argument, &now) != 0)
		return UINT64_MAX;

	/* Returns the computed result. */
	return now;
}

/* Converts one common-clock deadline into a bounded device-clock interval. */
static int
ax211_pci_command_timeout(
	struct ax211_pci_controller *controller,
	uint64_t deadline_ticks,
	uint64_t now_us,
	uint64_t *timeout_us,
	uint64_t *deadline_us)
{
	uint64_t now_ticks;
	uint64_t remaining_ticks;
	uint64_t remaining_us;

	/* Handles the controller availability. */
	if (controller == NULL || timeout_us == NULL || deadline_us == NULL)
		return EINVAL;
	remaining_us = AX211_DIRECT_TIMEOUT_US;

	/* Handles the deadline ticks condition. */
	if (deadline_ticks != 0U && deadline_ticks != UINT64_MAX) {
		/* Handles the now ticks condition. */
		now_ticks = clock_ticks();
		if (now_ticks >= deadline_ticks)
			return ETIMEDOUT;

		/* Handles the remaining ticks condition. */
		remaining_ticks = deadline_ticks - now_ticks;
		if (remaining_ticks > UINT64_MAX / 1000000U) {
			remaining_us = AX211_DIRECT_TIMEOUT_US;
		} else {
			remaining_us =
				remaining_ticks * 1000000U / KERN_CLOCK_HZ;

			/* Handles the remaining us condition. */
			if (remaining_us == 0U)
				remaining_us = 1U;

			/* Handles the remaining us condition. */
			if (remaining_us > AX211_DIRECT_TIMEOUT_US)
				remaining_us = AX211_DIRECT_TIMEOUT_US;
		}
	}

	/* Handles the now us condition. */
	if (now_us > UINT64_MAX - remaining_us)
		return EOVERFLOW;
	*timeout_us = remaining_us;
	*deadline_us = now_us + remaining_us;
	/* Succeeded. */
	return 0;
}

/* Reads a bounded AX210-family SRAM range while an outer command owns the NIC. The HBUS data window advances by one dword after every successful read. */
static int
ax211_pci_sram_read_locked(
	struct ax211_pci_controller *controller,
	uint32_t address,
	uint32_t *words,
	size_t count)
{
	size_t index;

	/* Checks the operation status. */
	if (controller == NULL || words == NULL || count == 0U ||
	    address < AX211_ERROR_LOG_MIN_ADDRESS || (address & 3U) != 0U ||
	    count - 1U > (UINT32_MAX - address) / sizeof(uint32_t) ||
	    controller->mmio.nic_lock_depth == 0U ||
	    controller->mmio.ops == NULL ||
	    controller->mmio.ops->csr_read32 == NULL ||
	    controller->mmio.ops->csr_write32 == NULL) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the csr write32 result. */
	if (controller->mmio.ops->csr_write32(controller->mmio.argument,
					      AX211_HBUS_TARG_MEM_RADDR,
					      address) != 0) {
		/* Failed. */
		return EIO;
	}
	kern_io_barrier();
	/* Process each remaining element. */
	for (index = 0U; index < count; index++) {
		/* Checks the csr read32 result. */
		if (controller->mmio.ops->csr_read32(controller->mmio.argument,
						     AX211_HBUS_TARG_MEM_RDAT,
						     &words[index]) != 0) {
			memset(words, 0, count * sizeof(*words));

			/* Failed. */
			return EIO;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Captures only the stable LMAC/UMAC error-table fields needed to identify a firmware assertion.  The ALIVE pointers are firmware-owned SRAM addresses; cache-control tag bits are not part of the address. */
static void
ax211_pci_firmware_error_dump(
	struct ax211_pci_controller *controller)
{
	uint32_t lmac[AX211_LMAC_ERROR_WORD_COUNT];
	uint32_t umac[AX211_UMAC_ERROR_WORD_COUNT];
	uint32_t lmac_address;
	uint32_t umac_address;
	int lmac_result;
	int umac_result;

	/* Handles the controller availability. */
	if (controller == NULL)
		return;

	/* Handles the controller condition. */
	if (!controller->runtime_start.alive_accepted ||
	    controller->runtime_start.alive.status !=
		    INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK) {
		kern_logf("intel-ax211: firmware error table unavailable "
			   "alive=%u status=%04x\n",
			   (unsigned)controller->runtime_start.alive_accepted,
			   controller->runtime_start.alive.status);

		/* Returns the computed result. */
		return;
	}

	memset(lmac, 0, sizeof(lmac));
	memset(umac, 0, sizeof(umac));
	lmac_address =
		controller->runtime_start.alive.lmac[0].error_event_table;
	umac_address = controller->runtime_start.alive.umac.error_info &
		       ~AX211_FW_ADDR_CACHE_CONTROL;
	lmac_result = ax211_pci_sram_read_locked(controller, lmac_address, lmac,
						 AX211_LMAC_ERROR_WORD_COUNT);
	umac_result = ax211_pci_sram_read_locked(controller, umac_address, umac,
						 AX211_UMAC_ERROR_WORD_COUNT);

	/* Handles the lmac result condition. */
	if (lmac_result == 0) {
		kern_logf(
			"intel-ax211: LMAC firmware error ptr=%08x valid=%08x "
			"id=%08x data=%08x/%08x/%08x hcmd=%08x last=%08x "
			"isr=%08x/%08x/%08x/%08x/%08x\n",
			lmac_address, lmac[0U], lmac[1U], lmac[7U], lmac[8U],
			lmac[9U], lmac[23U], lmac[29U], lmac[24U], lmac[25U],
			lmac[26U], lmac[27U], lmac[28U]);
	} else {
		kern_logf("intel-ax211: LMAC firmware error unavailable "
			   "ptr=%08x result=%d\n",
			   lmac_address, lmac_result);
	}

	/* Handles the umac result condition. */
	if (umac_result == 0) {
		kern_logf(
			"intel-ax211: UMAC firmware error ptr=%08x valid=%08x "
			"id=%08x data=%08x/%08x/%08x hcmd=%08x isr=%08x\n",
			umac_address, umac[0U], umac[1U], umac[6U], umac[7U],
			umac[8U], umac[13U], umac[14U]);
	} else {
		kern_logf("intel-ax211: UMAC firmware error unavailable "
			   "ptr=%08x result=%d\n",
			   umac_address, umac_result);
	}
}

/* Performs one exact synchronous command while lifecycle_lock owns the transport.  Asynchronous notifications are copied to a bounded queue and dispatched only after the outer common callback has unwound; only the matching oldest command response retires the slot. */
static int
ax211_pci_direct_command(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_command_request *request,
	uint64_t deadline_ticks,
	uint8_t *response,
	size_t response_capacity,
	size_t *response_length,
	struct intel_ax211_protocol_message *completed_message,
	struct intel_ax211_protocol_pending_command *completed_pending)
{
	const uint8_t *error_payload;
	uint16_t bad_sequence;
	uint8_t event_group;
	uint64_t slot_address;
	struct intel_ax211_boot_received_event received;
	struct intel_ax211_command_handle handle;
	struct intel_ax211_command_entry *entry;
	struct intel_ax211_event decoded;
	struct intel_ax211_protocol_pending_command pending;
	uint64_t deadline_us;
	uint64_t now_us;
	uint64_t timeout_us;
	size_t length;
	unsigned index;
	int command_submitted;
	int lock_result;
	int result;
	int unlock_result;

	/* Checks the drv intel ax211 command pending count result. */
	if (controller == NULL || request == NULL || response_length == NULL ||
	    (response_capacity != 0U && response == NULL) ||
	    !controller->runtime_initialized || !controller->runtime_active ||
	    controller->runtime_start.state !=
		    INTEL_AX211_RUNTIME_START_STATE_RUNNING ||
	    !controller->runtime_start.commands_initialized ||
	    drv_intel_ax211_command_pending_count(
		    &controller->runtime_start.commands) != 0U) {
		/* Failed. */
		return EBUSY;
	}
	*response_length = 0U;
	/* Handles the completed message availability. */
	if (completed_message != NULL)
		memset(completed_message, 0, sizeof(*completed_message));

	/* Handles the completed pending availability. */
	if (completed_pending != NULL)
		memset(completed_pending, 0, sizeof(*completed_pending));
	memset(&handle, 0, sizeof(handle));
	memset(&pending, 0, sizeof(pending));
	controller->command_fh_causes = 0U;
	controller->command_hw_causes = 0U;
	controller->command_raw_fh_causes = 0U;
	controller->command_raw_hw_causes = 0U;

	/* Checks the operation result. */
	result = ax211_pci_clock_us(controller, &now_us);
	if (result == 0) {
		result = ax211_pci_command_timeout(controller, deadline_ticks,
						   now_us, &timeout_us,
						   &deadline_us);
	}

	/* Checks the operation result. */
	if (result != 0)
		return result;

	/* Handles the lock result condition. */
	lock_result = ax211_pci_nic_lock(controller);
	if (lock_result != 0)
		return EIO;
	command_submitted = 0;

	/* Checks the operation result. */
	result = drv_intel_ax211_command_submit(
		&controller->runtime_start.commands, request, now_us,
		timeout_us, &handle);
	if (result == INTEL_AX211_COMMAND_OK) {
		command_submitted = 1;
		entry = &controller->runtime_start.commands
				 .entry[handle.token.index];
		pending = entry->pending;
		result = 0;
	} else {
		result = EIO;
	}

	index = 0U;
	/* Process each remaining element. */
	while (result == 0 && index < AX211_DIRECT_EVENT_LIMIT) {
		memset(&received, 0, sizeof(received));

		/* Checks the operation result. */
		result = ax211_pci_receive_event(
			controller, deadline_us, controller->runtime_event,
			sizeof(controller->runtime_event), &received);
		if (result == INTEL_AX211_BOOT_RECEIVE_TIMEOUT)
			result = ETIMEDOUT;
		else if (result != INTEL_AX211_BOOT_RECEIVE_OK)
			result = EIO;
		else if (received.generation != controller->hardware_epoch ||
			 received.length == 0U ||
			 received.length > sizeof(controller->runtime_event) ||
			 drv_intel_ax211_event_decode(
				 controller->runtime_event, received.length,
				 &decoded) != INTEL_AX211_OK)
			result = EIO;
		else if (ax211_pci_tx_event(&decoded)) {
			result = ax211_pci_deferred_event_enqueue(
				controller, controller->runtime_event,
				&received);
			ax211_pci_scrub(controller->runtime_event,
					received.length);
			index++;
		} else if ((decoded.queue & 0x80U) != 0U) {
			event_group =
				decoded.flags &
				(uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;

			/* Checks the operation status. */
			if (event_group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
			    decoded.command.opcode ==
				    AX211_REPLY_ERROR_OPCODE) {
				/* Checks the operation status. */
				error_payload = controller->runtime_event +
						decoded.payload_offset;
				if (decoded.payload_length ==
				    AX211_REPLY_ERROR_SIZE) {
					bad_sequence =
						(uint16_t)error_payload[6U] |
						((uint16_t)error_payload[7U]
						 << 8);
					kern_logf("intel-ax211: firmware "
						   "command error "
						   "type=%08x command=%02x "
						   "sequence=%04x "
						   "service=%08x\n",
						   ax211_pci_get_le32(
							   error_payload),
						   error_payload[4U],
						   bad_sequence,
						   ax211_pci_get_le32(
							   error_payload + 8U));
				} else {
					kern_logf("intel-ax211: malformed "
						   "firmware "
						   "command error length=%u\n",
						   (unsigned)decoded
							   .payload_length);
				}
			}

			result = ax211_pci_deferred_event_enqueue(
				controller, controller->runtime_event,
				&received);
			ax211_pci_scrub(controller->runtime_event,
					received.length);
			index++;
		} else {
			length = 0U;

			/* Checks the operation result. */
			result = drv_intel_ax211_command_complete(
				&controller->runtime_start.commands,
				controller->runtime_event, received.length,
				controller->hardware_epoch, response,
				response_capacity, &length);
			if (result == INTEL_AX211_COMMAND_OK) {
				*response_length = length;
				/* Handles the completed pending availability. */
				if (completed_pending != NULL)
					*completed_pending = pending;
				/* Handles the completed message availability. */
				if (completed_message != NULL) {
					completed_message->opcode =
						decoded.command.opcode;
					completed_message->group =
						decoded.flags &
						(uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
					completed_message->version =
						pending.response_version;
					completed_message->flags =
						decoded.flags;
					completed_message->queue =
						decoded.queue;
					completed_message->index =
						decoded.index;
					completed_message->generation =
						controller->hardware_epoch;
					completed_message->payload = response;
					completed_message->payload_length =
						length;
				}

				result = 0;
				command_submitted = 0;
			} else {
				kern_logf(
					"intel-ax211: command completion "
					"rejected "
					"request=%02x/%02x response=%02x/%02x "
					"flags=%02x queue=%02x index=%02x "
					"payload=%u "
					"expected=%u..%u result=%d\n",
					(unsigned)request->command.group,
					(unsigned)request->command.opcode,
					(unsigned)(decoded.flags &
						   (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK),
					(unsigned)decoded.command.opcode,
					(unsigned)decoded.flags,
					(unsigned)decoded.queue,
					(unsigned)decoded.index,
					(unsigned)decoded.payload_length,
					(unsigned)request
						->minimum_response_length,
					(unsigned)request
						->maximum_response_length,
					result);
				result = EIO;
			}

			ax211_pci_scrub(controller->runtime_event,
					received.length);
			break;
		}
	}

	/* Checks the operation result. */
	if (result == 0 && index == AX211_DIRECT_EVENT_LIMIT)
		result = ETIMEDOUT;
	if (result != 0 && command_submitted &&
	    ((controller->command_fh_causes &
	      INTEL_AX211_TRANSPORT_FH_CAUSE_ERROR) != 0U ||
	     (controller->command_hw_causes &
	      INTEL_AX211_TRANSPORT_HW_FATAL_CAUSES) != 0U))
		ax211_pci_firmware_error_dump(controller);
	if (result == ETIMEDOUT && command_submitted) {
		slot_address = controller->transport.memory
				       .command_slots_device_address +
			       (uint64_t)handle.token.index *
				       INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE;
		kern_logf(
			"intel-ax211: command timeout opcode=%02x group=%02x "
			"token=%u events=%u reason=%s slot-page=%03x "
			"fh=%08x hw=%08x raw-fh=%08x raw-hw=%08x "
			"rx-head=%u rx-tail=%u ring-head=%u ring-tail=%u "
			"used=%u\n",
			(unsigned)request->command.opcode,
			(unsigned)request->command.group,
			(unsigned)handle.token.index, index,
			index == AX211_DIRECT_EVENT_LIMIT ? "event-limit"
							  : "deadline",
			(unsigned)(slot_address & UINT64_C(0xfff)),
			controller->command_fh_causes,
			controller->command_hw_causes,
			controller->command_raw_fh_causes,
			controller->command_raw_hw_causes,
			(unsigned)controller->transport.rx_head,
			(unsigned)controller->transport.rx_tail,
			(unsigned)controller->transport.command_ring.head,
			(unsigned)controller->transport.command_ring.tail,
			(unsigned)controller->transport.command_ring.used);
	}

	/* Checks the operation result. */
	if (result != 0 && command_submitted) {
		(void)drv_intel_ax211_command_cancel(
			&controller->runtime_start.commands, &handle);
	}

	/* Checks the operation result. */
	unlock_result = ax211_pci_nic_unlock(controller);
	if (result == 0 && unlock_result != 0)
		result = EIO;

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 pci assoc exchange operation. */
static int
ax211_pci_assoc_exchange(
	void *argument,
	const struct intel_ax211_assoc_command *command,
	struct intel_ax211_assoc_reply *reply)
{
	uint16_t flags;
	uint16_t queue;
	uint16_t reserved;
	uint16_t write_pointer;
	struct ax211_pci_controller *controller;
	struct intel_ax211_command_request request;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_protocol_pending_command pending;
	size_t minimum;
	size_t maximum;
	size_t response_length;
	int result;

	/* Handles the controller availability. */
	controller = argument;
	if (controller == NULL || command == NULL || reply == NULL ||
	    command->common_generation != controller->connection_generation ||
	    command->hardware_epoch != controller->hardware_epoch) {
		/* Returns the computed result. */
		return INTEL_AX211_ASSOC_INVALID;
	}
	minimum = 0U;

	/* Handles the command condition. */
	maximum = 0U;
	if (command->response_kind == INTEL_AX211_ASSOC_RESPONSE_STATUS_ZERO) {
		minimum = 4U;
		maximum = 4U;
	} else if (command->response_kind == INTEL_AX211_ASSOC_RESPONSE_QUEUE) {
		minimum = INTEL_AX211_ASSOC_QUEUE_RESPONSE_SIZE;
		maximum = INTEL_AX211_ASSOC_QUEUE_RESPONSE_SIZE;
	} else if (command->response_kind ==
		   INTEL_AX211_ASSOC_RESPONSE_IGNORED) {
		maximum = INTEL_AX211_ASSOC_RESPONSE_MAX;
	}

	/* Handles the command condition. */
	if (command->step == INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE) {
		/* Checks the operation result. */
		result = drv_intel_ax211_tx_ring_queue_add_build(
			&controller->tx_ring, AX211_ASSOC_STATION_ID,
			INTEL_AX211_TX_RING_MANAGEMENT_TID,
			&controller->tx_queue_config);
		if (result != INTEL_AX211_TX_RING_OK ||
		    command->payload_length !=
			    sizeof(controller->tx_queue_config.command) ||
		    memcmp(command->payload,
			   controller->tx_queue_config.command,
			   sizeof(controller->tx_queue_config.command)) != 0) {
			/* Returns the computed result. */
			return INTEL_AX211_ASSOC_INVALID;
		}
	}

	memset(&request, 0, sizeof(request));
	request.command.group = command->group;
	request.command.opcode = command->opcode;
	request.command.version =
		command->header == INTEL_AX211_ASSOC_HEADER_WIDE
			? command->wire_version
			: command->layout_version;
	request.payload = command->payload;
	request.payload_length = command->payload_length;
	request.response_version = command->response_version;
	request.minimum_response_length = minimum;
	request.maximum_response_length = maximum;
	memset(&message, 0, sizeof(message));
	memset(&pending, 0, sizeof(pending));
	memset(reply, 0, sizeof(*reply));
	response_length = 0U;

	/* Checks the operation result. */
	result = ax211_pci_direct_command(
		controller, &request, controller->control_deadline_ticks,
		reply->payload, sizeof(reply->payload), &response_length,
		&message, &pending);
	if (result != 0) {
		kern_logf("intel-ax211: association exchange failed step=%u "
			   "opcode=%02x group=%02x result=%d\n",
			   command->step, command->opcode, command->group,
			   result);

		/* Returns the computed result. */
		return result == ETIMEDOUT ? INTEL_AX211_ASSOC_TIMEOUT
					   : INTEL_AX211_ASSOC_IO;
	}

	reply->step = command->step;
	reply->response_version = command->response_version;
	reply->sequence = command->sequence;
	reply->common_generation = command->common_generation;
	reply->hardware_epoch = command->hardware_epoch;
	reply->payload_length = response_length;

	/* Handles the command condition. */
	if (command->step == INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE) {
		/* Checks the operation result. */
		result = drv_intel_ax211_tx_ring_queue_add_complete(
			&controller->tx_ring, &controller->tx_queue_config,
			controller->hardware_epoch,
			controller->connection_generation, &message, &pending);
		if (result != INTEL_AX211_TX_RING_OK) {
			queue = response_length >= 2U
					? (uint16_t)reply->payload[0U] |
						  ((uint16_t)reply->payload[1U]
						   << 8)
					: 0;
			flags = response_length >= 4U
					? (uint16_t)reply->payload[2U] |
						  ((uint16_t)reply->payload[3U]
						   << 8)
					: 0;
			write_pointer =
				response_length >= 6U
					? (uint16_t)reply->payload[4U] |
						  ((uint16_t)reply->payload[5U]
						   << 8)
					: 0;
			reserved =
				response_length >= 8U
					? (uint16_t)reply->payload[6U] |
						  ((uint16_t)reply->payload[7U]
						   << 8)
					: 0;
			kern_logf("intel-ax211: queue add response rejected "
				   "result=%d "
				   "length=%u qid=%u flags=%04x wp=%u "
				   "reserved=%04x\n",
				   result, (unsigned)response_length,
				   (unsigned)queue, (unsigned)flags,
				   (unsigned)write_pointer, (unsigned)reserved);

			/* Returns the computed result. */
			return INTEL_AX211_ASSOC_FIRMWARE;
		}
	} else if (command->step == INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE) {
		/* Checks the operation result. */
		result = drv_intel_ax211_tx_ring_reset(&controller->tx_ring, 1);
		if (result != INTEL_AX211_TX_RING_OK)
			return INTEL_AX211_ASSOC_IO;
	}

	/* Returns the computed result. */
	return INTEL_AX211_ASSOC_OK;
}

/* Supports the ax211 pci assoc result errno operation. */
static int
ax211_pci_assoc_result_errno(
	int result)
{
	/* Dispatch the selected operation case. */
	switch (result) {
	case INTEL_AX211_ASSOC_OK:
	case INTEL_AX211_ASSOC_AUTH_READY:
	case INTEL_AX211_ASSOC_COMPLETE:
	case INTEL_AX211_ASSOC_ROLLED_BACK:
		/* Succeeded. */
		return 0;
	case INTEL_AX211_ASSOC_INVALID:
		/* Failed. */
		return EINVAL;
	case INTEL_AX211_ASSOC_UNSUPPORTED:
		/* Failed. */
		return ENOTSUP;
	case INTEL_AX211_ASSOC_PENDING:
		/* Failed. */
		return EBUSY;
	case INTEL_AX211_ASSOC_STALE:
	case INTEL_AX211_ASSOC_DUPLICATE:
	case INTEL_AX211_ASSOC_OUT_OF_ORDER:
		/* Failed. */
		return ESTALE;
	case INTEL_AX211_ASSOC_TIMEOUT:
		/* Failed. */
		return ETIMEDOUT;
	case INTEL_AX211_ASSOC_FIRMWARE:
	case INTEL_AX211_ASSOC_IO:
	case INTEL_AX211_ASSOC_ROLLBACK_FAILED:
	default:
		/* Failed. */
		return EIO;
	}
}

/* Supports the ax211 pci tx ring result errno operation. */
static int
ax211_pci_tx_ring_result_errno(
	int result)
{
	/* Dispatch the selected operation case. */
	switch (result) {
	case INTEL_AX211_TX_RING_OK:
		/* Succeeded. */
		return 0;
	case INTEL_AX211_TX_RING_INVALID:
		/* Failed. */
		return EINVAL;
	case INTEL_AX211_TX_RING_UNSUPPORTED:
		/* Failed. */
		return ENOTSUP;
	case INTEL_AX211_TX_RING_NOT_READY:
		/* Failed. */
		return ENETDOWN;
	case INTEL_AX211_TX_RING_FULL:
	case INTEL_AX211_TX_RING_PENDING:
		/* Failed. */
		return EBUSY;
	case INTEL_AX211_TX_RING_STALE:
	case INTEL_AX211_TX_RING_OUT_OF_ORDER:
		/* Failed. */
		return ESTALE;
	case INTEL_AX211_TX_RING_DUPLICATE:
		/* Failed. */
		return EEXIST;
	case INTEL_AX211_TX_RING_TIMEOUT:
		/* Failed. */
		return ETIMEDOUT;
	case INTEL_AX211_TX_RING_NO_MEMORY:
		/* Failed. */
		return ENOMEM;
	case INTEL_AX211_TX_RING_TX_FAILED:
	case INTEL_AX211_TX_RING_IO_ERROR:
	case INTEL_AX211_TX_RING_POISONED:
	case INTEL_AX211_TX_RING_BARRIER_REQUIRED:
	case INTEL_AX211_TX_RING_KICK_FAILED:
	case INTEL_AX211_TX_RING_MALFORMED:
	default:
		/* Failed. */
		return EIO;
	}
}

/* Supports the ax211 pci key result errno operation. */
static int
ax211_pci_key_result_errno(
	int result)
{
	/* Dispatch the selected operation case. */
	switch (result) {
	case INTEL_AX211_KEY_OK:
	case INTEL_AX211_KEY_DUPLICATE:
		/* Succeeded. */
		return 0;
	case INTEL_AX211_KEY_INVALID:
		/* Failed. */
		return EINVAL;
	case INTEL_AX211_KEY_UNSUPPORTED:
		/* Failed. */
		return ENOTSUP;
	case INTEL_AX211_KEY_STALE:
		/* Failed. */
		return ESTALE;
	case INTEL_AX211_KEY_MISSING:
	default:
		/* Failed. */
		return ENOENT;
	}
}

/* Copies only scan-derived metadata which belongs to this firmware epoch. */
static int
ax211_pci_assoc_profile(
	struct ax211_pci_controller *controller,
	const struct wlan_bss_record *bss,
	uint64_t connection_generation,
	struct intel_ax211_assoc_profile *profile)
{
	int error;
	struct drv_intel_ax211_bss_assoc_metadata metadata;
	struct intel_ax211_bss_entry entry;
	struct intel_ax211_tx_queue_config queue;
	int result;

	/* Checks the ax211 pci scan channel present result. */
	if (controller == NULL || bss == NULL || profile == NULL ||
	    !controller->bss_published_initialized ||
	    !controller->tx_ring_allocated || connection_generation == 0U ||
	    !ax211_pci_scan_channel_present(controller, bss->channel)) {
		/* Failed. */
		return EINVAL;
	}
	memset(&entry, 0, sizeof(entry));

	/* Checks the operation result. */
	result = drv_intel_ax211_bss_cache_lookup(
		&controller->bss_published_cache, bss->bssid, bss->channel,
		controller->hardware_epoch, &entry);
	if (result != INTEL_AX211_BSS_OK)
		return result == INTEL_AX211_BSS_NOT_FOUND ? ENOENT : ESTALE;
	memset(&metadata, 0, sizeof(metadata));

	/* Checks the operation result. */
	result = drv_intel_ax211_bss_assoc_metadata(
		&entry, connection_generation, controller->hardware_epoch,
		&metadata);
	if (result != INTEL_AX211_BSS_OK)
		return EIO;
	memset(&queue, 0, sizeof(queue));

	/* Checks the operation result. */
	result = drv_intel_ax211_tx_ring_queue_add_build(
		&controller->tx_ring, AX211_ASSOC_STATION_ID,
		INTEL_AX211_TX_RING_MANAGEMENT_TID, &queue);
	if (result != INTEL_AX211_TX_RING_OK) {
		/* Obtains the ax211 pci tx ring result errno result. */
		error = ax211_pci_tx_ring_result_errno(result);

		/* Failed. */
		return error;
	}

	memset(profile, 0, sizeof(*profile));
	memcpy(profile->station_address, controller->net_device->hwaddr, 6U);
	memcpy(profile->bssid, metadata.bssid, 6U);
	profile->channel = metadata.channel;
	profile->channel_width_mhz = INTEL_AX211_ASSOC_CHANNEL_WIDTH_MHZ;
	profile->rx_chain_mask = controller->runtime_start.nvm.rx_chain_mask;

	/*
	 * Preserve the mandatory 1 Mbps fallback bitmap used by the reference
	 * command ABI even on 5 GHz, where the firmware does not use CCK.
	 */
	profile->cck_ack_rates =
		profile->channel <= 14U ? AX211_ASSOC_CCK_ACK_RATES : 1U;

	/*
	 * Mandatory 5 GHz basic OFDM rates: 6, 12, and 24 Mbit/s.  Do not
	 * advertise every optional OFDM rate as an ACK/basic rate.
	 */
	profile->ofdm_ack_rates = AX211_ASSOC_OFDM_ACK_RATES;
	profile->short_preamble =
		profile->channel <= 14U &&
		(metadata.capability & AX211_CAPABILITY_SHORT_PREAMBLE) != 0U;

	/*
	 * Short-slot timing is mandatory in the 5 GHz OFDM-only band even when
	 * the AP omits the 2.4-GHz capability bit from its beacon.
	 */
	profile->short_slot =
		profile->channel > 14U ||
		(metadata.capability & AX211_CAPABILITY_SHORT_SLOT) != 0U;

	/*
	 * The first p038 common association profile does not negotiate WMM.
	 * Keep data frames non-QoS even when the scanned BSS advertises WMM;
	 * enabling QoS here would make the common 802.11 layer reject its own
	 * data frames before a matching WMM association contract exists.
	 */
	profile->qos = 0U;
	profile->beacon_interval_tu = metadata.beacon_interval_tu;
	profile->dtim_period =
		metadata.tim_valid != 0U ? metadata.dtim_period : 0U;
	profile->queue_byte_count_address = queue.byte_count_address;
	profile->queue_descriptor_address = queue.tfd_address;

	/* Safe legacy EDCA defaults: BE, BK, VI, VO. */
	profile->edca[0U].ecw_min = 4U;
	profile->edca[0U].ecw_max = 10U;
	profile->edca[0U].aifsn = 3U;
	profile->edca[1U].ecw_min = 4U;
	profile->edca[1U].ecw_max = 10U;
	profile->edca[1U].aifsn = 7U;
	profile->edca[2U].ecw_min = 3U;
	profile->edca[2U].ecw_max = 4U;
	profile->edca[2U].aifsn = 2U;
	profile->edca[2U].txop_32us = 94U;
	profile->edca[3U].ecw_min = 2U;
	profile->edca[3U].ecw_max = 3U;
	profile->edca[3U].aifsn = 2U;
	profile->edca[3U].txop_32us = 47U;
	controller->selected_bss = entry;
	controller->selected_metadata = metadata;
	controller->selected_bss_valid = 1U;
	ax211_pci_scrub(&queue, sizeof(queue));
	ax211_pci_scrub(&metadata, sizeof(metadata));
	ax211_pci_scrub(&entry, sizeof(entry));

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 pci connection clear operation. */
static void
ax211_pci_connection_clear(
	struct ax211_pci_controller *controller)
{
	uint8_t key_index;

	/* Handles the controller availability. */
	if (controller == NULL)
		return;
	controller->connection_generation = 0U;
	controller->control_deadline_ticks = 0U;
	controller->selected_bss_valid = 0U;
	controller->association_initialized = 0U;
	controller->keys_initialized = 0U;
	ax211_pci_scrub(&controller->selected_bss,
			sizeof(controller->selected_bss));
	ax211_pci_scrub(&controller->selected_metadata,
			sizeof(controller->selected_metadata));
	ax211_pci_scrub(&controller->association,
			sizeof(controller->association));
	ax211_pci_scrub(&controller->keys, sizeof(controller->keys));
	ax211_pci_staged_key_clear(&controller->staged_pairwise_key);
	/* Process each remaining element. */
	for (key_index = 0U; key_index < INTEL_AX211_KEY_INDEX_LIMIT;
	     key_index++) {
		ax211_pci_staged_key_clear(
			&controller->staged_group_key[key_index]);
	}

	controller->retired_pairwise_key_generation = 0U;
	ax211_pci_scrub(controller->retired_group_key_generation,
			sizeof(controller->retired_group_key_generation));
	ax211_pci_scrub(controller->retired_group_key_remove,
			sizeof(controller->retired_group_key_remove));
	ax211_pci_scrub(&controller->tx_queue_config,
			sizeof(controller->tx_queue_config));
	ax211_pci_scrub(controller->tx_report_completion,
			sizeof(controller->tx_report_completion));
}

/* Performs the association state machine's finite reverse-order teardown. */
static int
ax211_pci_assoc_rollback(
	struct ax211_pci_controller *controller,
	uint64_t generation)
{
	int error;
	uint64_t now;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL)
		return EINVAL;

	/* Handles the controller condition. */
	if (!controller->association_initialized) {
		/* Handles the controller condition. */
		if (controller->connection_generation != 0U &&
		    generation != 0U &&
		    controller->connection_generation != generation) {
			/* Failed. */
			return ESTALE;
		}
		ax211_pci_connection_clear(controller);

		/* Succeeded. */
		return 0;
	}

	/* Handles the generation condition. */
	if (generation != 0U && generation != controller->connection_generation)
		return ESTALE;

	/* Handles the controller condition. */
	if (controller->keys_initialized) {
		/* Checks the operation result. */
		result = ax211_pci_keys_remove_all(
			controller, controller->control_deadline_ticks);
		if (result != 0)
			return result;
	}

	/* Checks the ax211 pci clock us result. */
	if (ax211_pci_clock_us(controller, &now) != 0)
		return EIO;

	/* Checks the operation result. */
	result = drv_intel_ax211_assoc_cancel(&controller->association,
					      controller->connection_generation,
					      controller->hardware_epoch, now);
	if (result == INTEL_AX211_ASSOC_PENDING ||
	    result == INTEL_AX211_ASSOC_ROLLED_BACK) {
		result = drv_intel_ax211_assoc_drive(
			&controller->association, &ax211_assoc_ops, controller);
	}

	/* Checks the operation result. */
	if (result != INTEL_AX211_ASSOC_ROLLED_BACK &&
	    result != INTEL_AX211_ASSOC_OK) {
		/* Obtains the ax211 pci assoc result errno result. */
		error = ax211_pci_assoc_result_errno(result);

		/* Failed. */
		return error;
	}

	/* Handles the controller condition. */
	if (controller->tx_ring.enabled ||
	    controller->tx_ring.pending_count != 0U) {
		/* Failed. */
		return EIO;
	}
	ax211_pci_connection_clear(controller);

	/* Succeeded. */
	return 0;
}

/* Enables own and broadcast/multicast reception after MAC association. */
static int
ax211_pci_mcast_filter_configure(
	struct ax211_pci_controller *controller,
	uint64_t deadline_ticks)
{
	int error;
	struct intel_ax211_command_request request;
	uint8_t payload[INTEL_AX211_ASSOC_MCAST_FILTER_SIZE];
	size_t response_length;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || !controller->selected_bss_valid)
		return EINVAL;
	memset(payload, 0, sizeof(payload));

	/* Checks the operation result. */
	result = drv_intel_ax211_assoc_mcast_filter_encode(
		controller->selected_metadata.bssid, payload, sizeof(payload));
	if (result != INTEL_AX211_ASSOC_OK) {
		ax211_pci_scrub(payload, sizeof(payload));

		/* Obtains the ax211 pci assoc result errno result. */
		error = ax211_pci_assoc_result_errno(result);

		/* Failed. */
		return error;
	}

	memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_ASSOC_GROUP_LEGACY;
	request.command.opcode = INTEL_AX211_ASSOC_MCAST_FILTER_OPCODE;
	request.command.version = INTEL_AX211_ASSOC_MCAST_FILTER_VERSION;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;
	request.minimum_response_length = 0U;
	request.maximum_response_length = 0U;
	response_length = 0U;

	/* Checks the operation result. */
	result = ax211_pci_direct_command(controller, &request, deadline_ticks,
					  NULL, 0U, &response_length, NULL,
					  NULL);
	if (result == 0 && response_length != 0U)
		result = EIO;
	ax211_pci_scrub(payload, sizeof(payload));

	/* Returns the computed result. */
	return result;
}

/* Programs the frozen API89 PM-off client table after multicast admission. */
static int
ax211_pci_mac_power_configure(
	struct ax211_pci_controller *controller,
	uint64_t deadline_ticks)
{
	int error;
	struct intel_ax211_command_request request;
	uint8_t payload[INTEL_AX211_ASSOC_MAC_POWER_SIZE];
	uint8_t response[INTEL_AX211_ASSOC_MAC_POWER_RESPONSE_SIZE];
	uint32_t beacon_interval_ms;
	size_t response_length;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || !controller->selected_bss_valid ||
	    controller->selected_metadata.beacon_interval_tu == 0U) {
		/* Failed. */
		return EINVAL;
	}

	/*
	 * One TU is 1.024 ms; round up so keep-alive never undershoots the
	 * advertised beacon interval when converting to the codec's ms input.
	 */
	beacon_interval_ms =
		((uint32_t)controller->selected_metadata.beacon_interval_tu *
			 1024U +
		 999U) /
		1000U;
	memset(payload, 0, sizeof(payload));
	memset(response, 0, sizeof(response));

	/* Checks the operation result. */
	result = drv_intel_ax211_assoc_mac_power_encode(
		controller->selected_metadata.dtim_period, beacon_interval_ms,
		payload, sizeof(payload));
	if (result != INTEL_AX211_ASSOC_OK) {
		ax211_pci_scrub(payload, sizeof(payload));

		/* Obtains the ax211 pci assoc result errno result. */
		error = ax211_pci_assoc_result_errno(result);

		/* Failed. */
		return error;
	}

	memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_ASSOC_GROUP_LEGACY;
	request.command.opcode = INTEL_AX211_ASSOC_MAC_POWER_OPCODE;
	request.command.version = INTEL_AX211_ASSOC_MAC_POWER_VERSION;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;
	request.minimum_response_length = 0U;
	request.maximum_response_length = sizeof(response);
	response_length = 0U;

	/* Checks the operation result. */
	result = ax211_pci_direct_command(controller, &request, deadline_ticks,
					  response, sizeof(response),
					  &response_length, NULL, NULL);
	if (result == 0) {
		result = ax211_pci_assoc_result_errno(
			drv_intel_ax211_assoc_mac_power_response_validate(
				response, response_length));
	}

	ax211_pci_scrub(response, sizeof(response));
	ax211_pci_scrub(payload, sizeof(payload));

	/* Returns the computed result. */
	return result;
}

/* Removes every tuple still reachable by firmware before station teardown. */
static int
ax211_pci_keys_remove_all(
	struct ax211_pci_controller *controller,
	uint64_t deadline_ticks)
{
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE];
	uint8_t index;
	uint64_t generation;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || !controller->keys_initialized)
		return 0;
	result = 0;
	/* Process each remaining element. */
	for (index = 0U; index < INTEL_AX211_KEY_INDEX_LIMIT && result == 0;
	     index++) {
		/* Handles the generation condition. */
		generation = controller->keys.active_group[index];
		if (generation == 0U &&
		    controller->staged_group_key[index].valid &&
		    controller->staged_group_key[index].programmed) {
			generation = controller->staged_group_key[index]
					     .request.key_generation;
		}

		/* Handles the generation condition. */
		if (generation == 0U &&
		    controller->retired_group_key_remove[index]) {
			generation =
				controller->retired_group_key_generation[index];
		}

		/* Handles the generation condition. */
		if (generation != 0U) {
			memset(command, 0, sizeof(command));

			/* Checks the operation result. */
			result = ax211_pci_key_result_errno(
				drv_intel_ax211_key_remove_encode(
					controller->connection_generation,
					generation, INTEL_AX211_KEY_GROUP_KEY,
					index, command));
			if (result == 0) {
				result = ax211_pci_key_command(
					controller, command, deadline_ticks);
			}

			drv_intel_ax211_key_command_scrub(command);
		}
	}

	/* Checks the operation result. */
	if (result == 0) {
		/* Handles the generation condition. */
		generation = controller->keys.active_pairwise;
		if (generation == 0U && controller->staged_pairwise_key.valid &&
		    controller->staged_pairwise_key.programmed) {
			generation = controller->staged_pairwise_key.request
					     .key_generation;
		}

		/* Handles the generation condition. */
		if (generation != 0U) {
			memset(command, 0, sizeof(command));

			/* Checks the operation result. */
			result = ax211_pci_key_result_errno(
				drv_intel_ax211_key_remove_encode(
					controller->connection_generation,
					generation, INTEL_AX211_KEY_PAIRWISE,
					0U, command));
			if (result == 0) {
				result = ax211_pci_key_command(
					controller, command, deadline_ticks);
			}

			drv_intel_ax211_key_command_scrub(command);
		}
	}

	/* Returns the computed result. */
	return result;
}

/* Issues one exact API89 key command; caller scrubs its payload. */
static int
ax211_pci_key_command(
	struct ax211_pci_controller *controller,
	const uint8_t *payload,
	uint64_t deadline_ticks)
{
	struct intel_ax211_command_request request;
	uint8_t response[4U];
	size_t response_length;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || payload == NULL)
		return EINVAL;
	memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_KEY_GROUP;
	request.command.opcode = INTEL_AX211_KEY_OPCODE;

	/*
	 * SEC_KEY layout is v1 in the command table; API89 wide headers use v0.
	 */
	request.command.version = INTEL_AX211_KEY_WIRE_VERSION;
	request.payload = payload;
	request.payload_length = INTEL_AX211_KEY_COMMAND_SIZE;
	request.response_version = INTEL_AX211_KEY_RESPONSE_VERSION;
	request.minimum_response_length = 0U;
	request.maximum_response_length = sizeof(response);
	memset(response, 0, sizeof(response));
	response_length = 0U;
	result = ax211_pci_direct_command(controller, &request, deadline_ticks,
					  response, sizeof(response),
					  &response_length, NULL, NULL);
	ax211_pci_scrub(response, sizeof(response));

	/* Returns the computed result. */
	return result;
}

/* Retains one replacement secret only until its checked activation barrier. */
static int
ax211_pci_staged_key_store(
	struct ax211_pci_staged_key *staged,
	const struct intel_ax211_key_request *request)
{
	/* Handles the staged availability. */
	if (staged == NULL || request == NULL)
		return EINVAL;

	/* Handles the staged condition. */
	if (staged->valid) {
		/* Handles the staged condition. */
		if (staged->request.connection_generation ==
			    request->connection_generation &&
		    staged->request.key_generation == request->key_generation &&
		    staged->request.receive_packet_number ==
			    request->receive_packet_number &&
		    staged->request.kind == request->kind &&
		    staged->request.key_index == request->key_index &&
		    (staged->programmed ||
		     memcmp(staged->request.key, request->key,
			    INTEL_AX211_KEY_BYTES) == 0)) {
			/* Succeeded. */
			return 0;
		}

		/* Failed. */
		return EBUSY;
	}

	staged->request = *request;
	staged->valid = 1U;
	staged->programmed = 0U;

	/* Succeeded. */
	return 0;
}

/* Marks the key hardware-owned before crossing the uncertain command edge. */
static int
ax211_pci_staged_key_program(
	struct ax211_pci_controller *controller,
	struct ax211_pci_staged_key *staged,
	uint64_t deadline_ticks)
{
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE];
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || staged == NULL || !staged->valid)
		return EINVAL;

	/* Handles the staged condition. */
	if (staged->programmed)
		return 0;
	memset(command, 0, sizeof(command));

	/* Checks the operation result. */
	result = ax211_pci_key_result_errno(
		drv_intel_ax211_key_add_encode(&staged->request, command));
	if (result == 0) {
		staged->programmed = 1U;
		result = ax211_pci_key_command(controller, command,
					       deadline_ticks);

		/*
		 * Once ADD crossed the command transport boundary it is not
		 * retryable: a timeout cannot prove whether firmware installed
		 * it. Erase the controller-owned plaintext even if the
		 * subsequent global fail-closed reset also fails and leaves
		 * this object quarantined.
		 */
		ax211_pci_scrub(staged->request.key,
				sizeof(staged->request.key));
	}

	drv_intel_ax211_key_command_scrub(command);

	/* Returns the computed result. */
	return result;
}

/* Supports the ax211 pci staged key clear operation. */
static void
ax211_pci_staged_key_clear(
	struct ax211_pci_staged_key *staged)
{
	/* Handles the staged availability. */
	if (staged != NULL)
		ax211_pci_scrub(staged, sizeof(*staged));
}

/* Supports the ax211 pci keys have active operation. */
static int
ax211_pci_keys_have_active(
	const struct ax211_pci_controller *controller)
{
	uint8_t index;

	/* Handles the controller availability. */
	if (controller == NULL || !controller->keys_initialized)
		return 0;

	/* Handles the controller condition. */
	if (controller->keys.active_pairwise != 0U)
		return 1;
	/* Process each remaining element. */
	for (index = 0U; index < INTEL_AX211_KEY_INDEX_LIMIT; index++) {
		/* Handles the controller condition. */
		if (controller->keys.active_group[index] != 0U)
			return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 pci key request address valid operation. */
static int
ax211_pci_key_request_address_valid(
	const struct ax211_pci_controller *controller,
	const struct wlan_radio_key_request *request)
{
	int error;
	static const uint8_t broadcast[6] = {0xffU, 0xffU, 0xffU,
					     0xffU, 0xffU, 0xffU};

	/* Handles the controller availability. */
	if (controller == NULL || request == NULL ||
	    !controller->selected_bss_valid) {
		/* Succeeded. */
		return 0;
	}

	/* Handles the request condition. */
	if (request->kind == WLAN_RADIO_KEY_PAIRWISE) {
		/* Computes the function result. */
		error =
			request->key_index == 0U &&
			memcmp(request->address,
			       controller->selected_metadata.bssid, 6U) == 0;

		/* Failed. */
		return error;
	}

	/* Computes the function result. */
	error =
		request->kind == WLAN_RADIO_KEY_GROUP &&
		memcmp(request->address, broadcast, sizeof(broadcast)) == 0;

	/* Returns the computed result. */
	return error;
}

/* An uncertain key command is recoverable only after a global DMA reset. */
static int
ax211_pci_key_fail_closed(
	struct ax211_pci_controller *controller,
	int error)
{
	int stop_error;

	/* Handles the controller availability. */
	if (controller == NULL)
		return error != 0 ? error : EIO;
	kern_logf("intel-ax211: fail-closed key/association path error=%d "
		   "phase=%u step=%u resources=%08x\n",
		   error, controller->association.phase,
		   controller->association.step,
		   controller->association.resources);
	stop_error = ax211_pci_session_stop(controller);
	controller->quarantined = stop_error != 0;

	/* The independent owner can retry after this radio callback returns. */
	if (stop_error != 0) {
		ax211_pci_stop_defer_locked(controller, stop_error);

		/* Reports the unproven stop; no resource absence is claimed. */
		return stop_error;
	}


	/* Returns the computed result. */
	return error != 0 ? error : EIO;
}

/* Supports the ax211 pci tx submit operation. */
static int
ax211_pci_tx_submit(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_tx_request *request,
	uint64_t deadline_ticks,
	int report_completion)
{
	int error;
	struct intel_ax211_tx_ring_handle handle;
	uint64_t deadline_us;
	uint64_t now_us;
	uint64_t timeout_us;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || request == NULL ||
	    (report_completion != 0 && report_completion != 1) ||
	    !controller->tx_ring_allocated) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the operation result. */
	result = ax211_pci_clock_us(controller, &now_us);
	if (result == 0) {
		result = ax211_pci_command_timeout(controller, deadline_ticks,
						   now_us, &timeout_us,
						   &deadline_us);
	}

	/* Checks the operation result. */
	if (result != 0)
		return result;
	(void)deadline_us;
	memset(&handle, 0, sizeof(handle));

	/* Checks the operation result. */
	result = drv_intel_ax211_tx_ring_submit(&controller->tx_ring, request,
						now_us, timeout_us, &handle);
	if (result != INTEL_AX211_TX_RING_OK) {
		/* Checks the operation status. */
		if (result == INTEL_AX211_TX_RING_KICK_FAILED ||
		    result == INTEL_AX211_TX_RING_POISONED ||
		    result == INTEL_AX211_TX_RING_BARRIER_REQUIRED)
			ax211_pci_recovery_latch_locked(controller, EIO);

		/* Obtains the ax211 pci tx ring result errno result. */
		error = ax211_pci_tx_ring_result_errno(result);

		/* Failed. */
		return error;
	}

	controller->tx_report_completion[handle.index] =
		report_completion != 0 ? 1U : 0U;

	/* Succeeded. */
	return 0;
}

/* Retires an exact TX response before publishing its common completion. */
static int
ax211_pci_tx_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_protocol_message *message)
{
	struct intel_ax211_tx_ring_retired retired;
	struct intel_ax211_protocol_message tx_message;
	struct wlan_station *station;
	unsigned report_completion;
	int report_result;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || message == NULL)
		return EINVAL;

	/* Handles the controller condition. */
	if (!controller->tx_ring_allocated)
		return 0;
	memset(&retired, 0, sizeof(retired));
	tx_message = *message;
	tx_message.queue &= 0x7fU;

	/* Checks the operation result. */
	result = drv_intel_ax211_tx_ring_complete(&controller->tx_ring,
						  &tx_message, &retired);
	if (result == INTEL_AX211_TX_RING_DUPLICATE ||
	    result == INTEL_AX211_TX_RING_STALE ||
	    result == INTEL_AX211_TX_RING_NOT_READY) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the operation status. */
	if (result != INTEL_AX211_TX_RING_OK &&
	    result != INTEL_AX211_TX_RING_TX_FAILED) {
		/* Failed. */
		return EIO;
	}
	report_completion =
		controller->tx_report_completion[retired.handle.index];
	controller->tx_report_completion[retired.handle.index] = 0U;
	station = NULL;

	/* Handles the report completion condition. */
	if (!report_completion)
		return 0;

	/* Checks the operation status. */
	if (result == INTEL_AX211_TX_RING_TX_FAILED) {
		kern_logf("intel-ax211: TX failed generation=%u cookie=%u "
			   "acknowledged=%u failure=%u/%u\n",
			   (unsigned)retired.handle.connection_generation,
			   (unsigned)retired.handle.cookie,
			   retired.acknowledged, retired.failure_rts,
			   retired.failure_frame);
	}

	/* Handles the report result condition. */
	report_result = ax211_pci_operation_enter_locked(controller, &station);
	if (report_result == ENODEV)
		return 0;

	/* Handles the report result condition. */
	if (report_result != 0)
		return EIO;

	/*
	 * Completion can advance WPA and synchronously enter another radio op.
	 */

	mutex_unlock(&controller->lifecycle_lock);

	report_result = wlan_station_report_tx_complete(
		station, retired.handle.connection_generation,
		retired.handle.cookie, result == INTEL_AX211_TX_RING_OK ? 1 : 0,
		result == INTEL_AX211_TX_RING_OK ? 0 : EIO);
	mutex_lock(&controller->lifecycle_lock);

	ax211_pci_operation_leave_locked(controller);

	/* Returns the computed result. */
	return report_result == 0 || report_result == ESTALE ||
			       report_result == ENODEV
		       ? 0
		       : EIO;
}

/* Fails one uncertain oldest TX and resets the complete hardware epoch. */
static int
ax211_pci_tx_timeout_check(
	struct ax211_pci_controller *controller,
	uint64_t now_us)
{
	struct intel_ax211_tx_ring_handle handle;
	struct wlan_station *station;
	unsigned report_completion;
	int report_result;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || !controller->tx_ring_allocated ||
	    !controller->tx_ring.enabled ||
	    controller->tx_ring.pending_count == 0U) {
		/* Succeeded. */
		return 0;
	}
	memset(&handle, 0, sizeof(handle));

	/* Checks the operation result. */
	result = drv_intel_ax211_tx_ring_timeout_oldest(&controller->tx_ring,
							now_us, &handle);
	if (result == INTEL_AX211_TX_RING_PENDING ||
	    result == INTEL_AX211_TX_RING_NOT_READY) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the operation result. */
	if (result != INTEL_AX211_TX_RING_TIMEOUT)
		return EIO;
	kern_logf(
		"intel-ax211: TX completion timeout generation=%u cookie=%u\n",
		(unsigned)handle.connection_generation,
		(unsigned)handle.cookie);
	report_completion = controller->tx_report_completion[handle.index];
	controller->tx_report_completion[handle.index] = 0U;
	station = NULL;

	/* Checks the ax211 pci operation enter locked result. */
	report_result = 0;
	if (report_completion &&
	    ax211_pci_operation_enter_locked(controller, &station) == 0) {
		mutex_unlock(&controller->lifecycle_lock);
		report_result = wlan_station_report_tx_complete(
			station, handle.connection_generation, handle.cookie, 0,
			ETIMEDOUT);
		mutex_lock(&controller->lifecycle_lock);
		ax211_pci_operation_leave_locked(controller);
	}

	/* Returns the computed result. */
	return report_result == 0 || report_result == ESTALE ||
			       report_result == ENODEV
		       ? ETIMEDOUT
		       : EIO;
}

/* Classifies one copied MPDU against the current connection and key epoch. */
static int
ax211_pci_connection_rx_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_rx_mpdu *mpdu,
	uint16_t frame_control)
{
	struct wlan_radio_rx_frame report;
	struct wlan_station *station;
	enum intel_ax211_key_kind kind;
	uint64_t key_generation;
	uint8_t key_index;
	unsigned type;
	int result;

	/* Handles the controller availability. */
	if (controller == NULL || mpdu == NULL || mpdu->frame == NULL ||
	    controller->connection_generation == 0U) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the type condition. */
	type = frame_control & 0x000cU;
	if (type != 0U && type != 0x0008U)
		return 0;
	memset(&report, 0, sizeof(report));
	report.generation = controller->connection_generation;
	report.frame = mpdu->frame;
	report.length = mpdu->length;
	report.rssi_dbm = mpdu->rssi_dbm;
	report.channel = mpdu->channel;

	/* Handles the mpdu condition. */
	if (mpdu->cipher == INTEL_AX211_RX_CIPHER_CCMP) {
		/* Handles the controller condition. */
		if (!controller->keys_initialized || mpdu->length < 10U ||
		    !mpdu->decrypted) {
			/* Succeeded. */
			return 0;
		}
		kind = (mpdu->frame[4U] & 1U) != 0U ? INTEL_AX211_KEY_GROUP_KEY
						    : INTEL_AX211_KEY_PAIRWISE;
		key_index =
			kind == INTEL_AX211_KEY_PAIRWISE ? 0U : mpdu->key_index;

		/* Checks the operation result. */
		result = drv_intel_ax211_key_state_rx_generation(
			&controller->keys, controller->connection_generation,
			kind, key_index, controller->hardware_epoch,
			&key_generation);
		if (result != INTEL_AX211_KEY_OK)
			return 0;
		report.key_generation = key_generation;
		report.packet_number = mpdu->packet_number;
		report.cipher = WLAN_RADIO_CIPHER_CCMP;
		report.decrypted = 1U;
		report.key_index = key_index;
	} else if (mpdu->cipher != INTEL_AX211_RX_CIPHER_NONE) {
		/* Succeeded. */
		return 0;
	}
	station = NULL;

	/* Checks the operation result. */
	result = ax211_pci_operation_enter_locked(controller, &station);
	if (result == ENODEV)
		return 0;
	if (result != 0)
		return EIO;

	/*
	 * Frame ingestion may advance WPA and synchronously enter a radio op.
	 */

	mutex_unlock(&controller->lifecycle_lock);

	(void)wlan_station_report_frame(station, &report);
	mutex_lock(&controller->lifecycle_lock);

	ax211_pci_operation_leave_locked(controller);

	/*
	 * Once the private descriptor and crypto envelope have validated, every
	 * common result is a per-frame policy/drop/control outcome.  It must
	 * not quarantine otherwise healthy DMA or firmware state.
	 */
	return 0;
}

/* Performs a stopped NVM pass followed by one retained runtime pass. */
static int
ax211_net_open(
	struct net_device *device)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_nvm nvm;
	struct drv_dma_device *dma_device;
	struct wlan_station *station;
	const char *stage;
	unsigned long enabled;
	int boot_result;
	int runtime_result;
	int cleanup_error;
	int error;
	int ltr_enabled;

	/* Handles the device availability. */
	if (device == NULL)
		return ENODEV;

	/* Handles the controller availability. */
	controller = device->driver_data;
	if (controller == NULL)
		return ENODEV;
	mutex_lock(&controller->lifecycle_lock);

	/*
	 * Independent retirement must release its epoch before open can reuse
	 * it.
	 */
	if (wlan_station_stop_busy(controller->station)) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return EBUSY;
	}

	/*
	 * Finishes an earlier stopped epoch before publishing a fresh scan
	 * profile.
	 */
	if (!controller->detaching && controller->ready &&
	    controller->station_attached &&
	    (controller->close_pending || controller->session_stopped)) {
		/* Checks the operation status. */
		error = ax211_pci_close_locked(controller);
		if (error != 0) {
			mutex_unlock(&controller->lifecycle_lock);

			/* Failed. */
			return error;
		}
	}

	/* Handles the controller condition. */
	if (controller->detaching || !controller->ready ||
	    controller->quarantined || !controller->station_attached)
		error = ENODEV;
	else if (controller->runtime_active) {
		error = controller->operation_admission_open &&
					!controller->recovery_pending &&
					!controller->recovery_running
				? 0
				: EBUSY;
	} else if (controller->operations_active != 0U) {
		error = EBUSY;
	} else {
		controller->operation_admission_open = 0U;
		controller->session_stopped = 0U;
		dma_device = drv_pci_device_dma(controller->device);
		memset(&table, 0, sizeof(table));
		memset(&nvm, 0, sizeof(nvm));
		boot_result = INTEL_AX211_BOOT_OK;
		runtime_result = INTEL_AX211_RUNTIME_START_OK;
		stage = "dma-coherency";

		/* Checks the operation status. */
		error = dma_device == NULL ||
					!drv_dma_device_is_coherent(dma_device)
				? EIO
				: 0;
		if (error == 0) {
			stage = "boot-init";
			error = drv_intel_ax211_boot_init(
					&controller->boot,
					&ax211_runtime_start_ops.boot,
					controller, dma_device,
					&controller->mmio,
					&controller->transport,
					(uint16_t)controller->hardware_revision,
					AX211_CSR_HW_RF_TYPE_GF,
					controller->hardware_epoch) ==
						INTEL_AX211_BOOT_OK
					? 0
					: EIO;
		}

		/* Checks the operation status. */
		if (error == 0) {
			stage = "firmware-boot";
			controller->boot_initialized = 1U;
			boot_result = drv_intel_ax211_boot_run(
				&controller->boot, &nvm);
			error = boot_result == INTEL_AX211_BOOT_OK ? 0 : EIO;
			controller->hardware_epoch =
				controller->boot.generation;

			/* Handles the boot result condition. */
			if (boot_result == INTEL_AX211_BOOT_OK &&
			    controller->mmio.master_disable_timed_out) {
				kern_logf("intel-ax211: master-disable "
					   "indication "
					   "timed out; PCI bus master disabled "
					   "and reset "
					   "completed\n");
			}

			/* Checks the operation status. */
			if (error != 0) {
				kern_logf(
					"intel-ax211: first boot failed "
					"result=%d "
					"last=%u state=%u generation=%u "
					"files=%u dma=%u "
					"exposed=%u hardware=%u transport=%u "
					"alive=%u "
					"pnvm=%u init=%u commands=%u nvm=%u\n",
					boot_result,
					controller->boot.last_error,
					controller->boot.state,
					controller->boot.generation,
					controller->boot.files_loaded,
					controller->boot.dma_prepared,
					controller->boot.dma_exposed,
					controller->boot.hardware_touched,
					controller->boot.transport_bound,
					controller->boot.alive_accepted,
					controller->boot.pnvm_accepted,
					controller->boot.init_accepted,
					controller->boot.command_table_valid,
					controller->boot.nvm_valid);
			}

			/* Checks the operation status. */
			if (error != 0 &&
			    controller->last_receive_length != 0U) {
				kern_logf("intel-ax211: last rx length=%u "
					   "opcode=%02x "
					   "group=%02x index=%02x queue=%02x "
					   "version=%u\n",
					   (unsigned)controller
						   ->last_receive_length,
					   controller->last_receive_header[4U],
					   controller->last_receive_header[5U],
					   controller->last_receive_header[6U],
					   controller->last_receive_header[7U],
					   controller->last_receive_version);
			}
		}

		/* Checks the operation status. */
		if (error == 0) {
			stage = "command-table";
			error = drv_intel_ax211_boot_command_table(
					&controller->boot, &table) ==
						INTEL_AX211_BOOT_OK
					? 0
					: EIO;
		}

		/* Checks the operation status. */
		ltr_enabled = 0;
		if (error == 0) {
			stage = "ltr";
			error = ax211_pci_ltr_enabled(controller, &ltr_enabled);
		}

		/* Checks the operation status. */
		if (error == 0) {
			stage = "runtime-init";
			error = drv_intel_ax211_runtime_start_init(
					&controller->runtime_start,
					&ax211_runtime_start_ops, controller,
					dma_device, &controller->mmio,
					&controller->transport,
					(uint16_t)controller->hardware_revision,
					AX211_CSR_HW_RF_TYPE_GF, &table, &nvm,
					ltr_enabled,
					controller->hardware_epoch) ==
						INTEL_AX211_RUNTIME_START_OK
					? 0
					: EIO;
		}

		/* Checks the operation status. */
		if (error == 0) {
			stage = "runtime-start";
			controller->runtime_initialized = 1U;
			runtime_result = drv_intel_ax211_runtime_start_run(
				&controller->runtime_start);
			error = runtime_result == INTEL_AX211_RUNTIME_START_OK
					? 0
					: EIO;
			controller->hardware_epoch =
				controller->runtime_start.generation;
			if (error != 0) {
				kern_logf(
					"intel-ax211: runtime start failed "
					"result=%d "
					"last=%u state=%u generation=%u "
					"files=%u dma=%u "
					"exposed=%u hardware=%u transport=%u "
					"alive=%u "
					"pnvm=%u init=%u profile=%u mcc=%u "
					"nic=%u\n",
					runtime_result,
					controller->runtime_start.last_error,
					controller->runtime_start.state,
					controller->runtime_start.generation,
					controller->runtime_start.files_loaded,
					controller->runtime_start.dma_prepared,
					controller->runtime_start.dma_exposed,
					controller->runtime_start
						.hardware_touched,
					controller->runtime_start
						.transport_bound,
					controller->runtime_start
						.alive_accepted,
					controller->runtime_start.pnvm_accepted,
					controller->runtime_start.init_accepted,
					controller->runtime_start.profile_valid,
					controller->runtime_start.mcc_valid,
					controller->runtime_start.nic_locked);
			}

			/* Checks the operation status. */
			if (error != 0 &&
			    controller->last_receive_length != 0U) {
				kern_logf("intel-ax211: last runtime rx "
					   "length=%u "
					   "opcode=%02x group=%02x index=%02x "
					   "queue=%02x "
					   "version=%u\n",
					   (unsigned)controller
						   ->last_receive_length,
					   controller->last_receive_header[4U],
					   controller->last_receive_header[5U],
					   controller->last_receive_header[6U],
					   controller->last_receive_header[7U],
					   controller->last_receive_version);
			}
		}

		/* Checks the operation status. */
		if (error == 0) {
			stage = "api89-validation";
			error = drv_intel_ax211_assoc_api89_validate(&table) ==
							INTEL_AX211_ASSOC_OK &&
						drv_intel_ax211_key_api89_validate(
							&table) ==
							INTEL_AX211_KEY_OK &&
						drv_intel_ax211_tx_ring_api89_validate(
							&table) ==
							INTEL_AX211_TX_RING_OK
					? 0
					: ENOTSUP;
		}

		/* Checks the operation status. */
		if (error == 0) {
			stage = "tx-ring";

			/* Checks the operation status. */
			error = drv_intel_ax211_tx_ring_allocate(
					dma_device, &ax211_tx_ring_ops,
					controller, &controller->tx_ring) ==
						INTEL_AX211_TX_RING_OK
					? 0
					: ENOMEM;
			if (error == 0) {
				controller->tx_ring_allocated = 1U;
				controller->next_management_cookie = UINT64_MAX;
			}
		}

		/* Checks the operation status. */
		if (error == 0) {
			stage = "scan-init";
			error = ax211_pci_scan_initialize(controller);
		}

		/* Checks the operation status. */
		if (error == 0) {
			enabled =
				spin_lock_irqsave(&controller->interrupt_lock);
			controller->runtime_active = 1U;
			spin_unlock_irqrestore(&controller->interrupt_lock,
					       enabled);
		}

		/* Checks the operation status. */
		station = controller->station;
		if (error == 0) {
			stage = "station-open";
			error = station == NULL ? ENODEV
						: wlan_station_open(station);
		}

		/* Checks the operation status. */
		if (error == 0) {
			controller->operation_admission_open = 1U;
		} else {
			kern_logf(
				"intel-ax211: open failed stage=%s error=%d\n",
				stage, error);

			/* Checks the operation status. */
			cleanup_error = ax211_pci_session_stop(controller);
			if (cleanup_error != 0) {
				controller->quarantined = 1U;
				error = cleanup_error;
			}
		}

		ax211_pci_scrub(&table, sizeof(table));
		ax211_pci_scrub(&nvm, sizeof(nvm));
	}

	mutex_unlock(&controller->lifecycle_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reconciles common retirement only after all driver operations can be joined. */
static int
ax211_pci_close_locked(
	struct ax211_pci_controller *controller)
{
	struct wlan_station *station;
	int error;
	int stop_error;

	/*
	 * Denies new forwards while preserving every existing lease until it
	 * returns.
	 */
	controller->close_pending = 1U;
	controller->operation_admission_open = 0U;

	/* Checks the operation status. */
	error = ax211_pci_operations_join_locked(
		controller, ax211_pci_lifecycle_deadline());
	if (error != 0)
		return error;
	station = controller->station_attached ? controller->station : NULL;

	/*
	 * Gives common retirement its own bounded window outside the callback
	 * mutex.
	 */

	mutex_unlock(&controller->lifecycle_lock);

	error = station == NULL
			? ENODEV
			: ax211_pci_station_close_wait(
				  station, ax211_pci_lifecycle_deadline());
	mutex_lock(&controller->lifecycle_lock);
	if (error != 0 && ax211_pci_log_rejection(controller)) {
		kern_logf("intel-ax211: common close requires checked stop "
			   "(%d)\n",
			   error);
	}

	/*
	 * A failed inverse still requires a checked stop, never a reset under a
	 * live pin.
	 */
	if (controller->operations_active != 0U)
		return EBUSY;

	/* Checks the operation status. */
	stop_error = wlan_station_quiesce_begin(station);
	if (stop_error != 0)
		return stop_error;
	stop_error = ax211_pci_session_stop(controller);
	wlan_station_quiesce_end(station);
	controller->quarantined = stop_error != 0;
	if (stop_error != 0)
		return stop_error;

	/*
	 * Successful global stop lets outstanding key/association inverses
	 * prove absence.
	 */

	mutex_unlock(&controller->lifecycle_lock);

	error = ax211_pci_station_close_wait(station,
					     ax211_pci_lifecycle_deadline());
	mutex_lock(&controller->lifecycle_lock);
	if (error == 0)
		controller->close_pending = 0U;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Closes the epoch without losing the ability to retry an incomplete close. */
static void
ax211_net_close(
	struct net_device *device)
{
	struct ax211_pci_controller *controller;
	int error;

	/* Retires carrier visibility before closing driver admission. */
	if (device == NULL)
		return;
	(void)net_device_set_carrier(device, 0);

	/* Handles the controller availability. */
	controller = device->driver_data;
	if (controller == NULL)
		return;
	mutex_lock(&controller->lifecycle_lock);

	wlan_station_stop_request(controller->station);
	error = ax211_pci_close_locked(controller);
	wlan_station_stop_complete(controller->station, error);
	if (error != 0 && ax211_pci_log_rejection(controller))
		kern_logf("intel-ax211: checked close pending (%d)\n", error);

	mutex_unlock(&controller->lifecycle_lock);
}

/* Bounds repeated rejection diagnostics while the lifecycle mutex is held. */
static int
ax211_pci_log_rejection(
	struct ax211_pci_controller *controller)
{
	uint64_t now;

	/*
	 * Keeps the first diagnostic and emits at most one retry diagnostic per
	 * second.
	 */

	/* Handles the now condition. */
	now = clock_ticks();
	if (now < controller->reject_log_deadline)
		return 0;
	controller->reject_log_deadline = UINT64_MAX - now < KERN_CLOCK_HZ
						  ? UINT64_MAX
						  : now + KERN_CLOCK_HZ;

	/* Reports operation failure. */
	return 1;
}

/* Keeps Ethernet conversion and CCMP framing in the common WLAN station. */
static int
ax211_net_transmit(
	struct net_device *device,
	struct packet_buf *packet)
{
	struct ax211_pci_controller *controller;
	struct wlan_station *station;
	int error;

	/* Handles the packet availability. */
	if (packet == NULL)
		return EINVAL;

	/* Handles the device availability. */
	if (device == NULL) {
		packet_buf_free(packet);

		/* Failed. */
		return ENODEV;
	}

	/* Handles the controller availability. */
	controller = device->driver_data;
	if (controller == NULL) {
		packet_buf_free(packet);

		/* Failed. */
		return ENODEV;
	}

	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (!controller->runtime_active || controller->quarantined ||
	    !controller->operation_admission_open ||
	    !controller->station_attached) {
		station = NULL;
		error = controller->ready ? ENETDOWN : ENODEV;
	} else {
		error = ax211_pci_operation_enter_locked(controller, &station);
	}

	mutex_unlock(&controller->lifecycle_lock);

	/* Handles the station availability. */
	if (station == NULL) {
		packet_buf_free(packet);

		/* Failed. */
		return error;
	}

	error = wlan_station_transmit(station, packet);
	mutex_lock(&controller->lifecycle_lock);

	ax211_pci_operation_leave_locked(controller);

	mutex_unlock(&controller->lifecycle_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 net poll receive operation. */
static unsigned
ax211_net_poll_receive(
	struct net_device *device,
	unsigned budget)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_boot_received_event event;
	uint64_t now;
	unsigned count;
	unsigned reschedule;
	unsigned long enabled;
	int dispatch_result;
	int fatal_error;
	int result;

	/* Handles the device availability. */
	if (device == NULL || budget == 0U)
		return 0U;

	/* Handles the controller availability. */
	controller = device->driver_data;
	if (controller == NULL)
		return 0U;
	count = 0U;
	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (controller->poll_active) {
		controller->poll_reschedule = 1U;
		mutex_unlock(&controller->lifecycle_lock);

		/* Returns the computed result. */
		return 0U;
	}

	/* Handles the controller condition. */
	if (!controller->runtime_active || controller->quarantined) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Returns the computed result. */
		return 0U;
	}

	controller->poll_active = 1U;
	fatal_error =
		controller->recovery_pending ? controller->recovery_error : 0;
	/* Process each remaining element. */
	while (fatal_error == 0 && count < budget &&
	       controller->runtime_active && !controller->quarantined) {
		/* Handles the controller condition. */
		if (controller->deferred_event_count != 0U) {
			/* Checks the operation result. */
			result = ax211_pci_deferred_event_drain_one(controller);
			if (result == EBUSY)
				break;
			if (result != 0) {
				fatal_error = result;
				break;
			}

			count++;
			continue;
		}

		/* Checks the ax211 pci clock us result. */
		if (ax211_pci_clock_us(controller, &now) != 0) {
			fatal_error = EIO;
			break;
		}

		memset(&event, 0, sizeof(event));

		/* Checks the operation result. */
		result = ax211_pci_receive_event(
			controller, now, controller->runtime_event,
			sizeof(controller->runtime_event), &event);
		if (result == INTEL_AX211_BOOT_RECEIVE_TIMEOUT)
			break;
		if (result != INTEL_AX211_BOOT_RECEIVE_OK) {
			kern_logf(
				"intel-ax211: runtime receive failed result=%d "
				"scan-phase=%u pending=%u\n",
				result, controller->scan_session.phase,
				(unsigned)drv_intel_ax211_command_pending_count(
					&controller->runtime_start.commands));
			fatal_error = EIO;
			break;
		}

		dispatch_result = ax211_pci_runtime_event_dispatch(
			controller, controller->runtime_event, event.length,
			&event);
		ax211_pci_scrub(controller->runtime_event, event.length);

		/* Handles the dispatch result condition. */
		if (dispatch_result != 0) {
			kern_logf(
				"intel-ax211: runtime dispatch failed error=%d "
				"length=%u opcode=%02x group=%02x index=%02x "
				"queue=%02x "
				"version=%u scan-phase=%u\n",
				dispatch_result, (unsigned)event.length,
				controller->last_receive_header[4U],
				controller->last_receive_header[5U],
				controller->last_receive_header[6U],
				controller->last_receive_header[7U],
				controller->last_receive_version,
				controller->scan_session.phase);
			fatal_error = dispatch_result;
			break;
		}

		count++;
	}

	/* Checks the operation status. */
	if (fatal_error == 0 && controller->runtime_active &&
	    !controller->quarantined) {
		/* Checks the operation result. */
		result = ax211_pci_clock_us(controller, &now);
		if (result == 0)
			result = ax211_pci_tx_timeout_check(controller, now);
		if (result != 0)
			fatal_error = result;
	}

	/* Checks the operation status. */
	if (fatal_error != 0) {
		/* Handles the controller condition. */
		if (!controller->recovery_pending) {
			ax211_pci_recovery_latch_locked(controller,
							fatal_error);
		}

		(void)ax211_pci_recovery_run_locked(controller);
	}

	enabled = spin_lock_irqsave(&controller->interrupt_lock);

	reschedule =
		(controller->poll_reschedule || controller->recovery_pending ||
		 controller->deferred_event_count != 0U || count >= budget ||
		 controller->irq_latched) &&
		controller->receive_enabled && controller->runtime_active &&
		!controller->quarantined;

	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);

	controller->poll_active = 0U;
	controller->poll_reschedule = 0U;

	mutex_unlock(&controller->lifecycle_lock);

	/* Handles the reschedule condition. */
	if (reschedule)
		net_device_schedule_poll(device);

	/* Returns the computed result. */
	return count;
}

/* Supports the ax211 net ioctl operation. */
static int
ax211_net_ioctl(
	struct net_device *device,
	unsigned long request,
	void *argument)
{
	int error;
	struct ax211_pci_controller *controller;
	int result;

	/* Handles the device availability. */
	if (device == NULL)
		return ENODEV;

	/* Handles the controller availability. */
	controller = device->driver_data;
	if (controller == NULL)
		return ENODEV;
	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (!controller->station_attached || controller->detaching)
		result = ENODEV;
	else if (request == SIOCGWLANSTATUS || request == SIOCGWLANSCAN ||
		 request == SIOCGWLANBSS ||
		 (request == SIOCSWLANDISCONNECT &&
		  controller->session_stopped))
		result = 0;
	else if (!controller->runtime_active || controller->quarantined ||
		 controller->recovery_pending || controller->recovery_running)
		result = ENETDOWN;
	else
		result = 0;

	/* Checks the ax211 pci log rejection result. */
	if (request == SIOCSWLANCONNECT && result != 0 &&
	    ax211_pci_log_rejection(controller)) {
		kern_logf("intel-ax211: connect ioctl rejected result=%d "
			   "runtime=%u "
			   "quarantined=%u recovery=%u/%u attached=%u "
			   "detaching=%u\n",
			   result, controller->runtime_active,
			   controller->quarantined,
			   controller->recovery_pending,
			   controller->recovery_running,
			   controller->station_attached, controller->detaching);
	}

	mutex_unlock(&controller->lifecycle_lock);

	/* Computes the function result. */
	error =
		result == 0 ? wlan_station_ioctl(device, request, argument)
			    : result;

	/* Returns the computed result. */
	return error;
}

/* Frees the controller only after the last removed net-device reference. */
static void
ax211_net_release(
	void *driver_data)
{
	struct ax211_pci_controller *controller;

	/* Handles the controller availability. */
	controller = driver_data;
	if (controller == NULL)
		return;
	ax211_pci_scrub(controller, sizeof(*controller));
	kern_free(controller);
}

/* Starts one finite asynchronous firmware scan on the selected channel. */
static int
ax211_radio_scan_channel_start(
	void *context,
	uint64_t generation,
	uint32_t step_index,
	uint32_t channel,
	uint64_t deadline)
{
	int error;
	struct ax211_pci_controller *controller;
	uint64_t now;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Handles the generation condition. */
	if (generation == 0U || channel > UINT8_MAX || deadline == 0U)
		return EINVAL;

	mutex_lock(&controller->lifecycle_lock);

	/* Handles the station availability. */
	if (!controller->ready || controller->detaching ||
	    controller->quarantined || controller->close_pending ||
	    controller->recovery_pending || controller->recovery_running ||
	    !controller->runtime_active || !controller->scan_initialized ||
	    !controller->station_attached || controller->station == NULL) {
		result = controller->ready ? ENETDOWN : ENODEV;
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return result;
	}

	/* Checks the ax211 pci scan channel present result. */
	if (step_index >= controller->scan_session.full_profile.channel_count ||
	    !ax211_pci_scan_channel_present(controller, (uint8_t)channel)) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return EINVAL;
	}

	result = ax211_pci_clock_us(controller, &now);

	/*
	 * Common starts a fresh staging set for each scan generation.  Preserve
	 * the published set until this entire generation completes
	 * successfully.
	 */
	if (result == 0 && (!controller->bss_staging_initialized ||
			    controller->bss_staging_generation != generation)) {
		ax211_pci_bss_staging_discard(controller);

		/* Checks the operation result. */
		result = drv_intel_ax211_bss_cache_init(
				 &controller->bss_staging_cache,
				 controller->hardware_epoch) ==
					 INTEL_AX211_BSS_OK
				 ? 0
				 : EIO;
		if (result == 0) {
			controller->bss_staging_initialized = 1U;
			controller->bss_staging_generation = generation;
		}
	}

	/* Checks the operation result. */
	if (result == 0) {
		result = drv_intel_ax211_scan_session_begin_channel(
			&controller->scan_session, generation, (uint8_t)channel,
			now);
	} else {
		result = INTEL_AX211_SCAN_SESSION_FAILED;
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_SCAN_SESSION_OK)
		controller->scan_step_index = step_index;
	else
		ax211_pci_bss_staging_discard(controller);

	mutex_unlock(&controller->lifecycle_lock);

	/* Obtains the ax211 pci scan result errno result. */
	error = ax211_pci_scan_result_errno(result);

	/* Returns the computed result. */
	return error;
}

/* Retires the matching asynchronous scan or requests a bounded retry. */
static int
ax211_radio_scan_stop(
	void *context,
	uint64_t generation)
{
	struct ax211_pci_controller *controller;
	uint64_t now;
	uint8_t phase;
	int cleanup_error;
	int error;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Handles the generation condition. */
	if (generation == 0U)
		return EINVAL;

	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (controller->recovery_pending && !controller->recovery_running) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return ENETDOWN;
	}

	/* Handles the controller condition. */
	if (!controller->runtime_active || !controller->scan_initialized) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Succeeded. */
		return 0;
	}

	/* Handles the controller condition. */
	if (controller->scan_session.common_generation == 0U) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Succeeded. */
		return 0;
	}

	/* Handles the controller condition. */
	if (controller->scan_session.common_generation != generation) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return ESTALE;
	}

	/* Handles the phase condition. */
	phase = controller->scan_session.phase;
	if (phase == INTEL_AX211_SCAN_SESSION_IDLE) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Succeeded. */
		return 0;
	}

	/* Handles the phase condition. */
	if (phase == INTEL_AX211_SCAN_SESSION_TERMINAL) {
		result = controller->scan_session.terminal_result;
	} else {
		/* Checks the operation status. */
		error = ax211_pci_clock_us(controller, &now);
		if (error != 0)
			result = INTEL_AX211_SCAN_SESSION_FAILED;
		else
			result = drv_intel_ax211_scan_session_expire(
				&controller->scan_session, now);

		/* Checks the operation result. */
		phase = controller->scan_session.phase;
		if (result == INTEL_AX211_SCAN_SESSION_OK &&
		    phase == INTEL_AX211_SCAN_SESSION_RUNNING) {
			result = drv_intel_ax211_scan_session_abort(
				&controller->scan_session, generation, now);
		}
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_SCAN_SESSION_OK ||
	    result == INTEL_AX211_SCAN_SESSION_BUSY) {
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return EBUSY;
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_SCAN_SESSION_COMPLETE) {
		/* Handles the controller condition. */
		if (controller->scan_step_index + 1U ==
		    controller->scan_session.full_profile.channel_count) {
			/* Handles the controller condition. */
			if (!controller->bss_staging_initialized &&
			    controller->bss_published_generation == generation)
				error = 0;
			else
				error = ax211_pci_bss_staging_publish(
					controller, generation);
		} else {
			/*
			 * One common generation spans every channel.  Keep
			 * private metadata from earlier channels until the
			 * final step publishes the complete set; association
			 * may select any of those BSSes.
			 */
			error = 0;
		}

		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return error;
	}

	/* Checks the operation result. */
	if (result == INTEL_AX211_SCAN_SESSION_ABORTED ||
	    result == INTEL_AX211_SCAN_SESSION_DUPLICATE) {
		ax211_pci_bss_staging_discard(controller);
		mutex_unlock(&controller->lifecycle_lock);

		/* Succeeded. */
		return 0;
	}

	/*
	 * An expired or failed command leaves firmware ownership ambiguous.
	 * Stop the entire epoch so a successful return can never strand a
	 * producer behind the common WLAN barrier.
	 */
	error = ax211_pci_scan_result_errno(result);
	kern_logf("intel-ax211: scan stop generation=%u phase=%u result=%d "
		   "error=%d\n",
		   (unsigned)generation, phase, result, error);
	ax211_pci_bss_staging_discard(controller);
	cleanup_error = ax211_pci_session_stop(controller);
	controller->quarantined = cleanup_error != 0;

	mutex_unlock(&controller->lifecycle_lock);

	/*
	 * The scan failed, but a proven global stop completed its retirement.
	 */
	return cleanup_error;
}

/* Supports the ax211 radio connect start operation. */
static int
ax211_radio_connect_start(
	void *context,
	uint64_t generation,
	const struct wlan_bss_record *bss,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_protocol_command_table table;
	uint64_t now;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Checks the clock ticks result. */
	if (generation == 0U || bss == NULL || deadline == 0U ||
	    clock_ticks() >= deadline) {
		/* Failed. */
		return EINVAL;
	}
	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (!controller->runtime_active || controller->close_pending ||
	    controller->quarantined || controller->recovery_pending ||
	    controller->recovery_running || !controller->tx_ring_allocated ||
	    controller->association_initialized ||
	    controller->connection_generation != 0U) {
		/* Handles the ax211 pci log rejection condition. */
		if (ax211_pci_log_rejection(controller)) {
			kern_logf("intel-ax211: connect admission rejected "
				   "runtime=%u "
				   "quarantined=%u recovery=%u/%u tx-ring=%u "
				   "association=%u "
				   "generation=%u\n",
				   controller->runtime_active,
				   controller->quarantined,
				   controller->recovery_pending,
				   controller->recovery_running,
				   controller->tx_ring_allocated,
				   controller->association_initialized,
				   (unsigned)controller->connection_generation);
		}

		result = controller->runtime_active ? EBUSY : ENETDOWN;
		mutex_unlock(&controller->lifecycle_lock);

		/* Failed. */
		return result;
	}

	memset(&profile, 0, sizeof(profile));
	result = ax211_pci_assoc_profile(controller, bss, generation, &profile);
	memset(&table, 0, sizeof(table));
	if (result == 0) {
		table.bytes = controller->runtime_start.command_version_bytes;
		table.count = INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT;
		result = ax211_pci_clock_us(controller, &now);
	}

	/* Checks the operation result. */
	if (result == 0) {
		controller->connection_generation = generation;
		controller->control_deadline_ticks = deadline;

		/* Checks the operation result. */
		result = drv_intel_ax211_key_state_init(
			&controller->keys, controller->hardware_epoch,
			generation);
		if (result == INTEL_AX211_KEY_OK)
			controller->keys_initialized = 1U;
		else
			result = EIO;
	}

	/* Checks the operation result. */
	if (result == 0) {
		/* Checks the operation result. */
		result = drv_intel_ax211_assoc_begin(
			&controller->association, &table, &profile, generation,
			controller->hardware_epoch, now);
		if (result == INTEL_AX211_ASSOC_OK) {
			controller->association_initialized = 1U;
			result = drv_intel_ax211_assoc_drive(
				&controller->association, &ax211_assoc_ops,
				controller);
		}

		/* Checks the operation result. */
		if (result != INTEL_AX211_ASSOC_AUTH_READY) {
			kern_logf(
				"intel-ax211: association drive stopped "
				"result=%d "
				"phase=%u step=%u failure=%d resources=%08x\n",
				result, controller->association.phase,
				controller->association.step,
				controller->association.failure,
				controller->association.resources);
		}

		result = ax211_pci_assoc_result_errno(result);
	}

	/* Checks the operation result. */
	if (result != 0 && controller->association_initialized) {
		/* Handles the controller condition. */
		if (controller->association.phase ==
			    INTEL_AX211_ASSOC_PHASE_IDLE &&
		    !controller->tx_ring.enabled &&
		    controller->tx_ring.pending_count == 0U)
			ax211_pci_connection_clear(controller);
		else
			result = ax211_pci_key_fail_closed(controller, result);
	} else if (result != 0 && controller->connection_generation != 0U)
		ax211_pci_connection_clear(controller);

	/* Checks the operation result. */
	if (result != 0) {
		kern_logf("intel-ax211: association start failed result=%d "
			   "phase=%u step=%u failure=%d resources=%08x\n",
			   result, controller->association.phase,
			   controller->association.step,
			   controller->association.failure,
			   controller->association.resources);
	} else {
		kern_logf("intel-ax211: association hardware ready "
			   "generation=%u\n",
			   (unsigned)generation);
	}

	ax211_pci_scrub(&profile, sizeof(profile));
	ax211_pci_scrub(&table, sizeof(table));

	mutex_unlock(&controller->lifecycle_lock);

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 radio disconnect operation. */
static int
ax211_radio_disconnect(
	void *context,
	uint64_t generation)
{
	struct ax211_pci_controller *controller;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Handles the generation condition. */
	if (generation == 0U)
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (controller->recovery_pending && !controller->recovery_running) {
		result = ENETDOWN;
	} else {
		controller->control_deadline_ticks =
			ax211_pci_lifecycle_deadline();

		/* Checks the operation result. */
		result = ax211_pci_assoc_rollback(controller, generation);
		if (result != 0 && ax211_pci_log_rejection(controller)) {
			kern_logf("intel-ax211: association rollback failed "
				   "generation=%u result=%d phase=%u step=%u "
				   "failure=%d\n",
				   (unsigned)generation, result,
				   controller->association.phase,
				   controller->association.step,
				   controller->association.failure);
		}

		/* Checks the operation result. */
		if (result != 0 && result != ESTALE &&
		    controller->runtime_active)
			result = ax211_pci_key_fail_closed(controller, result);
	}

	mutex_unlock(&controller->lifecycle_lock);

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 radio management transmit operation. */
static int
ax211_radio_management_transmit(
	void *context,
	uint64_t generation,
	const uint8_t *frame,
	size_t length,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_tx_request request;
	unsigned attempt;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Handles the frame availability. */
	if (generation == 0U || frame == NULL || length == 0U || deadline == 0U)
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (!controller->runtime_active || controller->close_pending ||
	    controller->recovery_pending || controller->recovery_running ||
	    !controller->tx_ring.enabled ||
	    controller->connection_generation != generation) {
		/* Handles the ax211 pci log rejection condition. */
		if (ax211_pci_log_rejection(controller)) {
			kern_logf("intel-ax211: management TX rejected "
				   "runtime=%u "
				   "recovery=%u/%u ring=%u "
				   "expected-generation=%u generation=%u\n",
				   controller->runtime_active,
				   controller->recovery_pending,
				   controller->recovery_running,
				   controller->tx_ring.enabled,
				   (unsigned)controller->connection_generation,
				   (unsigned)generation);
		}

		result = ENETDOWN;
	} else {
		memset(&request, 0, sizeof(request));
		request.connection_generation = generation;
		request.frame = frame;
		request.length = length;
		request.frame_class = INTEL_AX211_TX_FRAME_MANAGEMENT;
		request.band_5ghz = controller->selected_metadata.channel > 14U;
		result = EEXIST;
		attempt = 0U;
		/* Process each remaining element. */
		while (attempt < INTEL_AX211_TX_RING_SLOT_COUNT &&
		       result == EEXIST) {
			/* Handles the controller condition. */
			if (controller->next_management_cookie == 0U)
				controller->next_management_cookie = UINT64_MAX;
			request.cookie = controller->next_management_cookie--;
			result = ax211_pci_tx_submit(controller, &request,
						     deadline, 0);
			attempt++;
		}
	}

	mutex_unlock(&controller->lifecycle_lock);

	/* Returns the computed result. */
	return result;
}

/* Supports the ax211 radio association set operation. */
static int
ax211_radio_association_set(
	void *context,
	uint64_t generation,
	const uint8_t bssid[6],
	uint16_t aid,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_assoc_update update;
	uint64_t now;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Checks the clock ticks result. */
	if (generation == 0U || bssid == NULL || aid == 0U || aid > 2007U ||
	    deadline == 0U || clock_ticks() >= deadline) {
		/* Failed. */
		return EINVAL;
	}
	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (!controller->runtime_active || controller->close_pending ||
	    controller->recovery_pending || controller->recovery_running ||
	    !controller->association_initialized ||
	    !controller->selected_bss_valid)
		result = ENETDOWN;
	else if (controller->connection_generation != generation ||
		 memcmp(bssid, controller->selected_metadata.bssid, 6U) != 0) {
		result = ESTALE;
	} else {
		memset(&update, 0, sizeof(update));
		update.association_id = aid;
		update.dtim_period = controller->selected_metadata.dtim_period;
		update.dtim_count = controller->selected_metadata.dtim_count;
		update.beacon_arrive_time =
			controller->selected_metadata.beacon_arrive_time;
		update.beacon_tsf = controller->selected_metadata.beacon_tsf;

		/* Checks the operation result. */
		result = ax211_pci_clock_us(controller, &now);
		if (result == 0) {
			controller->control_deadline_ticks = deadline;

			/* Checks the operation result. */
			result = drv_intel_ax211_assoc_begin_update(
				&controller->association, &update, generation,
				controller->hardware_epoch, now);
			if (result == INTEL_AX211_ASSOC_OK) {
				result = drv_intel_ax211_assoc_drive(
					&controller->association,
					&ax211_assoc_ops, controller);
			}

			/* Checks the operation result. */
			result = ax211_pci_assoc_result_errno(result);
			if (result == 0 &&
			    controller->association.phase !=
				    INTEL_AX211_ASSOC_PHASE_ASSOCIATED)
				result = EIO;
			if (result == 0) {
				/* Checks the operation result. */
				result = ax211_pci_mcast_filter_configure(
					controller, deadline);
				if (result != 0) {
					kern_logf(
						"intel-ax211: post-association "
						"multicast configuration "
						"failed (%d)\n",
						result);
				}
			}

			/* Checks the operation result. */
			if (result == 0) {
				/* Checks the operation result. */
				result = ax211_pci_mac_power_configure(
					controller, deadline);
				if (result != 0) {
					kern_logf(
						"intel-ax211: post-association "
						"power configuration failed "
						"(%d)\n",
						result);
				}
			}

			/* Checks the operation result. */
			if (result != 0) {
				result = ax211_pci_key_fail_closed(controller,
								   result);
			}
		}

		ax211_pci_scrub(&update, sizeof(update));
	}

	mutex_unlock(&controller->lifecycle_lock);

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 radio association clear operation. */
static int
ax211_radio_association_clear(
	void *context,
	uint64_t generation,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Handles the generation condition. */
	if (generation == 0U || deadline == 0U)
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (controller->recovery_pending && !controller->recovery_running) {
		result = ENETDOWN;
	} else {
		controller->control_deadline_ticks = deadline;

		/* Checks the operation result. */
		result = ax211_pci_assoc_rollback(controller, generation);
		if (result != 0 && result != ESTALE &&
		    controller->runtime_active)
			result = ax211_pci_key_fail_closed(controller, result);
	}

	mutex_unlock(&controller->lifecycle_lock);

	/* Returns the computed result. */
	return result;
}

/* Supports the ax211 radio frame transmit operation. */
static int
ax211_radio_frame_transmit(
	void *context,
	const struct wlan_radio_tx_request *request)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_tx_request tx;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Handles the request availability. */
	if (request == NULL || request->generation == 0U ||
	    request->cookie == 0U || request->frame == NULL ||
	    request->length == 0U || request->deadline_ticks == 0U) {
		/* Failed. */
		return EINVAL;
	}
	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (!controller->runtime_active || controller->close_pending ||
	    controller->recovery_pending || controller->recovery_running ||
	    !controller->tx_ring.enabled)
		result = ENETDOWN;
	else if (controller->connection_generation != request->generation) {
		result = ESTALE;
	} else {
		memset(&tx, 0, sizeof(tx));
		tx.connection_generation = request->generation;
		tx.cookie = request->cookie;
		tx.key_generation = request->key_generation;
		tx.packet_number = request->packet_number;
		tx.frame = request->frame;
		tx.length = request->length;
		tx.encrypted = request->encrypted;
		tx.key_index = request->key_index;
		tx.band_5ghz = controller->selected_metadata.channel > 14U;

		/* Handles the request condition. */
		if (request->frame_class == WLAN_RADIO_FRAME_MANAGEMENT)
			tx.frame_class = INTEL_AX211_TX_FRAME_MANAGEMENT;
		else if (request->frame_class == WLAN_RADIO_FRAME_EAPOL)
			tx.frame_class = INTEL_AX211_TX_FRAME_EAPOL;
		else if (request->frame_class == WLAN_RADIO_FRAME_DATA)
			tx.frame_class = INTEL_AX211_TX_FRAME_DATA;
		else
			result = EINVAL;

		/* Handles the request condition. */
		if (request->frame_class == WLAN_RADIO_FRAME_MANAGEMENT ||
		    request->frame_class == WLAN_RADIO_FRAME_EAPOL ||
		    request->frame_class == WLAN_RADIO_FRAME_DATA) {
			/* Handles the request condition. */
			result = 0;
			if (request->encrypted) {
				result =
					controller->keys_initialized
						? drv_intel_ax211_key_state_tx_validate(
							  &controller->keys,
							  request->generation,
							  request->key_generation,
							  request->key_index,
							  request->packet_number,
							  controller
								  ->hardware_epoch)
						: INTEL_AX211_KEY_MISSING;
				result = ax211_pci_key_result_errno(result);
			}

			/* Checks the operation result. */
			if (result == 0) {
				result = ax211_pci_tx_submit(
					controller, &tx,
					request->deadline_ticks, 1);
			}
		}

		ax211_pci_scrub(&tx, sizeof(tx));
	}

	mutex_unlock(&controller->lifecycle_lock);

	/* Returns the computed result. */
	return result;
}

/* Supports the ax211 radio key install operation. */
static int
ax211_radio_key_install(
	void *context,
	const struct wlan_radio_key_request *request)
{
	struct ax211_pci_controller *controller;
	struct ax211_pci_staged_key *staged;
	struct intel_ax211_key_request key;
	uint8_t index;
	uint64_t group_generation;
	int active_before;
	int state_result;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Checks the clock ticks result. */
	if (request == NULL || request->generation == 0U ||
	    request->key_generation == 0U || request->deadline_ticks == 0U ||
	    clock_ticks() >= request->deadline_ticks ||
	    (request->kind != WLAN_RADIO_KEY_PAIRWISE &&
	     request->kind != WLAN_RADIO_KEY_GROUP) ||
	    request->key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (request->kind == WLAN_RADIO_KEY_PAIRWISE &&
	     request->key_index != 0U)) {
		/* Failed. */
		return EINVAL;
	}
	mutex_lock(&controller->lifecycle_lock);

	memset(&key, 0, sizeof(key));

	/* Handles the controller condition. */
	if (!controller->runtime_active || controller->close_pending ||
	    controller->recovery_pending || controller->recovery_running ||
	    !controller->keys_initialized)
		result = ENETDOWN;
	else if (controller->connection_generation != request->generation)
		result = ESTALE;
	else if (!ax211_pci_key_request_address_valid(controller, request)) {
		result = EINVAL;
	} else {
		key.connection_generation = request->generation;
		key.key_generation = request->key_generation;
		key.receive_packet_number = request->receive_packet_number;
		key.key_index = request->key_index;
		key.kind = request->kind == WLAN_RADIO_KEY_PAIRWISE
				   ? INTEL_AX211_KEY_PAIRWISE
				   : INTEL_AX211_KEY_GROUP_KEY;
		memcpy(key.key, request->key, sizeof(key.key));
		staged = key.kind == INTEL_AX211_KEY_PAIRWISE
				 ? &controller->staged_pairwise_key
				 : &controller->staged_group_key[key.key_index];
		active_before = ax211_pci_keys_have_active(controller);

		/* Handles the selected key. */
		if ((key.kind == INTEL_AX211_KEY_PAIRWISE &&
		     controller->keys.active_pairwise == key.key_generation) ||
		    (key.kind == INTEL_AX211_KEY_GROUP_KEY &&
		     controller->keys.active_group[key.key_index] ==
			     key.key_generation)) {
			result = 0;
		} else {
			result = ax211_pci_staged_key_store(staged, &key);
			state_result =
				result == 0
					? drv_intel_ax211_key_state_installed(
						  &controller->keys, &key,
						  controller->hardware_epoch)
					: INTEL_AX211_KEY_INVALID;
			if (result == 0 && state_result != INTEL_AX211_KEY_OK &&
			    state_result != INTEL_AX211_KEY_DUPLICATE) {
				result = ax211_pci_key_result_errno(
					state_result);
			}

			/*
			 * Initial keys must be live before common sends its
			 * first M4. Replacement keys remain private until
			 * keys_activate().
			 */
			if (result == 0 && !active_before) {
				result = ax211_pci_staged_key_program(
					controller, staged,
					request->deadline_ticks);
			}
		}

		/*
		 * The initial common handshake installs pairwise then group
		 * keys but has no separate activation callback.  Activate that
		 * first complete pair only after both firmware ACKs.
		 */
		if (result == 0 && !ax211_pci_keys_have_active(controller) &&
		    controller->staged_pairwise_key.valid &&
		    controller->staged_pairwise_key.programmed) {
			group_generation = 0U;
			index = 0U;
			/* Process each remaining element. */
			while (index < INTEL_AX211_KEY_INDEX_LIMIT &&
			       group_generation == 0U) {
				/* Handles the controller condition. */
				if (controller->staged_group_key[index].valid &&
				    controller->staged_group_key[index]
					    .programmed) {
					group_generation =
						controller
							->staged_group_key
								[index]
							.request.key_generation;
				}

				index++;
			}

			/* Handles the group generation condition. */
			if (group_generation != 0U) {
				/* Checks the operation result. */
				result = ax211_pci_key_result_errno(
					drv_intel_ax211_key_state_activate(
						&controller->keys,
						request->generation,
						controller->staged_pairwise_key
							.request.key_generation,
						group_generation,
						controller->hardware_epoch));
				if (result == 0) {
					ax211_pci_staged_key_clear(
						&controller
							 ->staged_pairwise_key);
					/* Process each remaining element. */
					for (index = 0U;
					     index <
					     INTEL_AX211_KEY_INDEX_LIMIT;
					     index++) {
						ax211_pci_staged_key_clear(
							&controller->staged_group_key
								 [index]);
					}
				}
			}
		}

		/* Handles the staged availability. */
		if (result != 0 && staged != NULL && staged->programmed)
			result = ax211_pci_key_fail_closed(controller, result);
	}

	ax211_pci_scrub(&key, sizeof(key));

	mutex_unlock(&controller->lifecycle_lock);

	/* Returns the computed result. */
	return result;
}

/* Supports the ax211 radio key delete operation. */
static int
ax211_radio_key_delete(
	void *context,
	uint64_t generation,
	enum wlan_radio_key_kind kind,
	uint8_t key_index,
	uint64_t key_generation,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	struct ax211_pci_staged_key *staged;
	enum intel_ax211_key_kind private_kind;
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE];
	int active_match;
	int programmed;
	int retired_match;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Handles the generation condition. */
	if (generation == 0U || key_generation == 0U || deadline == 0U ||
	    (kind != WLAN_RADIO_KEY_PAIRWISE && kind != WLAN_RADIO_KEY_GROUP) ||
	    key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (kind == WLAN_RADIO_KEY_PAIRWISE && key_index != 0U)) {
		/* Failed. */
		return EINVAL;
	}
	private_kind = kind == WLAN_RADIO_KEY_PAIRWISE
			       ? INTEL_AX211_KEY_PAIRWISE
			       : INTEL_AX211_KEY_GROUP_KEY;
	memset(command, 0, sizeof(command));
	mutex_lock(&controller->lifecycle_lock);

	/*
	 * A proven global stop retired every key, including the core's pending
	 * inverse.
	 */
	if (controller->session_stopped)
		result = 0;
	else if (!controller->runtime_active ||
		 (controller->recovery_pending &&
		  !controller->recovery_running) ||
		 !controller->keys_initialized)
		result = ENETDOWN;
	else if (controller->connection_generation != generation) {
		result = ESTALE;
	} else {
		staged = private_kind == INTEL_AX211_KEY_PAIRWISE
				 ? &controller->staged_pairwise_key
				 : &controller->staged_group_key[key_index];
		programmed = staged->valid &&
			     staged->request.key_generation == key_generation &&
			     staged->programmed;
		active_match =
			private_kind == INTEL_AX211_KEY_PAIRWISE
				? controller->keys.active_pairwise ==
					  key_generation
				: controller->keys.active_group[key_index] ==
					  key_generation;
		retired_match =
			private_kind == INTEL_AX211_KEY_PAIRWISE
				? controller->retired_pairwise_key_generation ==
					  key_generation
				: controller->retired_group_key_generation
						  [key_index] == key_generation;
		result = 0;

		/* Handles the programmed condition. */
		if (programmed || active_match ||
		    (retired_match &&
		     private_kind == INTEL_AX211_KEY_GROUP_KEY &&
		     controller->retired_group_key_remove[key_index])) {
			result = ax211_pci_key_result_errno(
				drv_intel_ax211_key_remove_encode(
					generation, key_generation,
					private_kind, key_index, command));
		}

		/* Checks the operation result. */
		if (result == 0 &&
		    (programmed || active_match ||
		     (retired_match &&
		      private_kind == INTEL_AX211_KEY_GROUP_KEY &&
		      controller->retired_group_key_remove[key_index]))) {
			result = ax211_pci_key_command(controller, command,
						       deadline);
		}

		/* Checks the operation result. */
		if (result == 0 &&
		    ((staged->valid &&
		      staged->request.key_generation == key_generation) ||
		     active_match)) {
			result = ax211_pci_key_result_errno(
				drv_intel_ax211_key_state_removed(
					&controller->keys, generation,
					private_kind, key_index, key_generation,
					controller->hardware_epoch));
		}

		/* Checks the operation result. */
		if (result == 0 && staged->valid &&
		    staged->request.key_generation == key_generation)
			ax211_pci_staged_key_clear(staged);

		/* Checks the operation result. */
		if (result == 0 && retired_match) {
			/* Handles the private kind condition. */
			if (private_kind == INTEL_AX211_KEY_PAIRWISE) {
				controller->retired_pairwise_key_generation =
					0U;
			} else {
				controller->retired_group_key_generation
					[key_index] = 0U;
				controller
					->retired_group_key_remove[key_index] =
					0U;
			}
		}

		/* Checks the operation result. */
		if (result != 0 &&
		    (programmed || active_match || retired_match))
			result = ax211_pci_key_fail_closed(controller, result);
	}

	drv_intel_ax211_key_command_scrub(command);

	mutex_unlock(&controller->lifecycle_lock);

	/* Returns the computed result. */
	return result;
}

/* Supports the ax211 radio keys activate operation. */
static int
ax211_radio_keys_activate(
	void *context,
	uint64_t generation,
	uint64_t pairwise_key_generation,
	uint64_t group_key_generation,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	struct ax211_pci_staged_key *group;
	uint64_t old_group[INTEL_AX211_KEY_INDEX_LIMIT];
	uint64_t old_pairwise;
	uint8_t group_index;
	uint8_t index;
	int command_crossed;
	int result;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;

	/* Checks the clock ticks result. */
	if (generation == 0U || pairwise_key_generation == 0U ||
	    group_key_generation == 0U || deadline == 0U ||
	    clock_ticks() >= deadline) {
		/* Failed. */
		return EINVAL;
	}
	mutex_lock(&controller->lifecycle_lock);

	/* Handles the controller condition. */
	if (!controller->runtime_active || controller->close_pending ||
	    controller->recovery_pending || controller->recovery_running ||
	    !controller->keys_initialized)
		result = ENETDOWN;
	else if (controller->connection_generation != generation) {
		result = ESTALE;
	} else {
		group = NULL;
		group_index = 0U;
		/* Process each remaining element. */
		while (group_index < INTEL_AX211_KEY_INDEX_LIMIT &&
		       group == NULL) {
			/* Handles the controller condition. */
			if (controller->staged_group_key[group_index].valid &&
			    controller->staged_group_key[group_index]
					    .request.key_generation ==
				    group_key_generation) {
				group = &controller->staged_group_key
						 [group_index];
			} else {
				group_index++;
			}
		}

		old_pairwise = controller->keys.active_pairwise;
		memcpy(old_group, controller->keys.active_group,
		       sizeof(old_group));
		command_crossed = 0;

		/* Handles the group availability. */
		result = 0;
		if (group != NULL && !group->programmed) {
			command_crossed = 1;
			result = ax211_pci_staged_key_program(controller, group,
							      deadline);
		}

		/* Checks the operation result. */
		if (result == 0 && controller->staged_pairwise_key.valid &&
		    controller->staged_pairwise_key.request.key_generation ==
			    pairwise_key_generation &&
		    !controller->staged_pairwise_key.programmed) {
			command_crossed = 1;
			result = ax211_pci_staged_key_program(
				controller, &controller->staged_pairwise_key,
				deadline);
		}

		/* Checks the operation result. */
		if (result == 0) {
			result = ax211_pci_key_result_errno(
				drv_intel_ax211_key_state_activate(
					&controller->keys, generation,
					pairwise_key_generation,
					group_key_generation,
					controller->hardware_epoch));
		}

		/* Checks the operation result. */
		if (result == 0) {
			/* Handles the old pairwise condition. */
			if (old_pairwise != 0U &&
			    old_pairwise != pairwise_key_generation) {
				controller->retired_pairwise_key_generation =
					old_pairwise;
			}

			/* Process each remaining element. */
			for (index = 0U; index < INTEL_AX211_KEY_INDEX_LIMIT;
			     index++) {
				/* Handles the old group condition. */
				if (old_group[index] != 0U &&
				    old_group[index] != group_key_generation) {
					controller->retired_group_key_generation
						[index] = old_group[index];
					controller->retired_group_key_remove
						[index] =
						index != group_index ? 1U : 0U;
				}
			}

			ax211_pci_staged_key_clear(
				&controller->staged_pairwise_key);
			/* Process each remaining element. */
			for (index = 0U; index < INTEL_AX211_KEY_INDEX_LIMIT;
			     index++) {
				ax211_pci_staged_key_clear(
					&controller->staged_group_key[index]);
			}
		} else if (command_crossed)
			result = ax211_pci_key_fail_closed(controller, result);
		ax211_pci_scrub(old_group, sizeof(old_group));
	}

	mutex_unlock(&controller->lifecycle_lock);

	/* Returns the computed result. */
	return result;
}

/* The common work pin retains this context; hardware access stays serialized. */
static int
ax211_radio_stop_retry(
	void *context)
{
	struct ax211_pci_controller *controller;
	int error;

	controller = context;
	mutex_lock(&controller->lifecycle_lock);

	error = ax211_pci_close_locked(controller);

	mutex_unlock(&controller->lifecycle_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 radio quiesce operation. */
static int
ax211_radio_quiesce(
	void *context)
{
	struct ax211_pci_controller *controller;
	int error;

	/* Handles the controller availability. */
	controller = context;
	if (controller == NULL)
		return ENODEV;
	mutex_lock(&controller->lifecycle_lock);

	/* Checks the operation status. */
	error = ax211_pci_session_stop(controller);
	if (error != 0)
		controller->quarantined = 1U;

	mutex_unlock(&controller->lifecycle_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Erases temporary and terminal controller-owned identity/state bytes. */
static void
ax211_pci_scrub(
	void *memory,
	size_t length)
{
	volatile uint8_t *byte;

	byte = memory;
	/* Process each remaining element. */
	while (length != 0U) {
		*byte++ = 0U;
		length--;
	}
}
/* End consolidated pci-intel-ax211.c. */
