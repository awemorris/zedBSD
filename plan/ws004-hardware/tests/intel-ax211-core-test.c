/*
 * Intel AX211 private-core fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-internal.h"

#define TEST_FW_CAPACITY 8192U
#define TEST_PNVM_CAPACITY 1024U

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static uint16_t
get_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

static uint32_t
get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t
get_le64(const uint8_t *bytes)
{
	uint64_t value = 0;
	unsigned int index;

	for (index = 0; index < 8; index++)
		value |= (uint64_t)bytes[index] << (index * 8);
	return value;
}

static size_t
append_tlv(uint8_t *bytes, size_t capacity, size_t offset, uint32_t type,
	const void *payload, uint32_t length)
{
	size_t padded = ((size_t)length + 3U) & ~(size_t)3U;

	assert(offset <= capacity);
	assert(capacity - offset >= 8U + padded);
	put_le32(bytes + offset, type);
	put_le32(bytes + offset + 4U, length);
	if (length != 0U)
		memcpy(bytes + offset + 8U, payload, length);
	if (padded != length)
		memset(bytes + offset + 8U + length, 0, padded - length);
	return offset + 8U + padded;
}

static size_t
append_u32(uint8_t *bytes, size_t capacity, size_t offset, uint32_t type,
	uint32_t value)
{
	uint8_t payload[4];

	put_le32(payload, value);
	return append_tlv(bytes, capacity, offset, type, payload,
	    sizeof(payload));
}

static size_t
make_firmware(uint8_t *firmware)
{
	uint8_t version[12];
	uint8_t section[8];
	uint8_t iml[5] = { 1, 2, 3, 4, 5 };
	uint8_t command_version[4] = { 1, 0, 99, 6 };
	size_t offset = INTEL_AX211_TLV_HEADER_SIZE;

	memset(firmware, 0, TEST_FW_CAPACITY);
	put_le32(firmware + 4U, INTEL_AX211_TLV_MAGIC);
	put_le32(firmware + 72U, 0x59000000U);
	put_le32(firmware + 76U, 7U);
	offset = append_u32(firmware, TEST_FW_CAPACITY, offset, 23U,
	    0x12345678U);
	offset = append_u32(firmware, TEST_FW_CAPACITY, offset, 27U, 2U);
	put_le32(version, INTEL_AX211_FIRMWARE_API);
	put_le32(version + 4U, INTEL_AX211_FIRMWARE_MINOR);
	put_le32(version + 8U, INTEL_AX211_FIRMWARE_SERIAL);
	offset = append_tlv(firmware, TEST_FW_CAPACITY, offset, 36U, version,
	    sizeof(version));
	offset = append_tlv(firmware, TEST_FW_CAPACITY, offset, 52U, iml,
	    sizeof(iml));
	offset = append_tlv(firmware, TEST_FW_CAPACITY, offset, 48U,
	    command_version, sizeof(command_version));
	put_le32(section, 0x1000U);
	put_le32(section + 4U, 0xaaaaaaaaU);
	offset = append_tlv(firmware, TEST_FW_CAPACITY, offset, 19U, section,
	    sizeof(section));
	put_le32(section, INTEL_AX211_CPU1_CPU2_SEPARATOR);
	put_le32(section + 4U, 0U);
	offset = append_tlv(firmware, TEST_FW_CAPACITY, offset, 19U, section,
	    sizeof(section));
	put_le32(section, 0x2000U);
	put_le32(section + 4U, 0xbbbbbbbbU);
	offset = append_tlv(firmware, TEST_FW_CAPACITY, offset, 19U, section,
	    sizeof(section));
	put_le32(section, INTEL_AX211_PAGING_SEPARATOR);
	put_le32(section + 4U, 0U);
	offset = append_tlv(firmware, TEST_FW_CAPACITY, offset, 19U, section,
	    sizeof(section));
	put_le32(section, 0x3000U);
	put_le32(section + 4U, 0xccccccccU);
	return append_tlv(firmware, TEST_FW_CAPACITY, offset, 19U, section,
	    sizeof(section));
}

static void
test_identity_and_metadata(void)
{
	struct intel_ax211_identity identity = {
		INTEL_AX211_PCI_VENDOR_ID,
		INTEL_AX211_PCI_DEVICE_ID,
		INTEL_AX211_PCI_SUBVENDOR_ID,
		INTEL_AX211_PCI_SUBDEVICE_ID,
		INTEL_AX211_PCI_REVISION
	};

	assert(intel_ax211_identity_matches(&identity));
	identity.vendor++;
	assert(!intel_ax211_identity_matches(&identity));
	identity.vendor--;
	identity.device++;
	assert(!intel_ax211_identity_matches(&identity));
	identity.device--;
	identity.subvendor++;
	assert(!intel_ax211_identity_matches(&identity));
	identity.subvendor--;
	identity.subdevice++;
	assert(!intel_ax211_identity_matches(&identity));
	assert(INTEL_AX211_MAC_TYPE_SO == 0x37U);
	assert(INTEL_AX211_MAC_TYPE_SOF == 0x43U);
	assert(intel_ax211_mac_type_supported(INTEL_AX211_MAC_TYPE_SO));
	assert(intel_ax211_mac_type_supported(INTEL_AX211_MAC_TYPE_SOF));
	assert(!intel_ax211_mac_type_supported(0x42U));
	assert(INTEL_AX211_CRF_ID == 0x401410U);
	assert(INTEL_AX211_CNV_ID == 0x80400U);
	assert(INTEL_AX211_WFPM_ID == 0x80000020U);
	assert(INTEL_AX211_RAW_RF_ID == 0x2010d000U);
	assert(INTEL_AX211_RF_TYPE == 0x10dU);
	assert(INTEL_AX211_RF_CDB == 0U);
	assert(INTEL_AX211_RF_JACKET == 1U);
	identity.subdevice--;
	identity.revision++;
	assert(!intel_ax211_identity_matches(&identity));
	assert(strcmp(INTEL_AX211_FIRMWARE_PATH,
	    "intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode") == 0);
	assert(INTEL_AX211_FIRMWARE_SIZE == 1736748U);
	assert(strcmp(INTEL_AX211_FIRMWARE_SHA256,
	    "c569c4b0ffe2054a1cedd5affccff2da8515325eeb23f788c7abe9463d1a1514") == 0);
	assert(strcmp(INTEL_AX211_FIRMWARE_VERSION, "89.735b75a4.0") == 0);
	assert(strcmp(INTEL_AX211_PNVM_PATH,
	    "intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm") == 0);
	assert(INTEL_AX211_PNVM_SIZE == 55176U);
	assert(strcmp(INTEL_AX211_PNVM_SHA256,
	    "efa9726d4a9d44b83fc9a14cedcf306a4e439e9de919802eb9e92df4ec032b2a") == 0);
	assert(strcmp(INTEL_AX211_LINUX_FIRMWARE_TAG, "20260410") == 0);
	assert(strcmp(INTEL_AX211_LINUX_FIRMWARE_COMMIT,
	    "dc85ccedc9c973682fbcf4d628ca61174bcc3120") == 0);
}

static void
test_firmware_parser(void)
{
	uint8_t firmware[TEST_FW_CAPACITY];
	uint8_t malformed[TEST_FW_CAPACITY];
	struct intel_ax211_firmware_manifest manifest;
	size_t length = make_firmware(firmware);
	size_t offset;

	assert(intel_ax211_firmware_parse(firmware, length, &manifest) ==
	    INTEL_AX211_OK);
	assert(manifest.api_major == 89U);
	assert(manifest.api_minor == 0x735b75a4U);
	assert(manifest.cpu_count == 2U);
	assert(manifest.phy_sku == 0x12345678U);
	assert(manifest.iml_length == 5U);
	assert(manifest.command_versions_length == 4U);
	assert(memcmp(firmware + manifest.command_versions_offset,
	    "\x01\x00\x63\x06", 4U) == 0);
	assert(manifest.runtime_count == 5U);
	assert(manifest.lmac_count == 1U);
	assert(manifest.umac_count == 1U);
	assert(manifest.paging_count == 1U);
	assert(manifest.runtime[0].destination == 0x1000U);
	assert(manifest.runtime[4].destination == 0x3000U);

	assert(intel_ax211_firmware_parse(firmware, 87U, &manifest) ==
	    INTEL_AX211_TRUNCATED);
	assert(intel_ax211_firmware_parse(firmware, length - 1U, &manifest) ==
	    INTEL_AX211_TRUNCATED);
	/* Removing the final paging section leaves both separators but no image. */
	assert(intel_ax211_firmware_parse(firmware, length - 16U, &manifest) ==
	    INTEL_AX211_MISSING);

	memcpy(malformed, firmware, length);
	put_le32(malformed + 92U, 0xffffffffU);
	assert(intel_ax211_firmware_parse(malformed, length, &manifest) ==
	    INTEL_AX211_TRUNCATED);

	memcpy(malformed, firmware, length);
	put_le32(malformed + INTEL_AX211_TLV_HEADER_SIZE, 0xdeadbeefU);
	assert(intel_ax211_firmware_parse(malformed, length, &manifest) ==
	    INTEL_AX211_UNSUPPORTED);

	memcpy(malformed, firmware, length);
	/* FW_VERSION payload follows the two leading four-byte TLVs. */
	put_le32(malformed + INTEL_AX211_TLV_HEADER_SIZE + 32U, 0U);
	assert(intel_ax211_firmware_parse(malformed, length, &manifest) ==
	    INTEL_AX211_IDENTITY_MISMATCH);

	memcpy(malformed, firmware, length);
	/* The command-version TLV follows IML and must contain whole rows. */
	put_le32(malformed + INTEL_AX211_TLV_HEADER_SIZE + 64U, 3U);
	assert(intel_ax211_firmware_parse(malformed, length, &manifest) ==
	    INTEL_AX211_INVALID);

	memcpy(malformed, firmware, length);
	offset = append_u32(malformed, TEST_FW_CAPACITY, length, 23U,
	    0x12345678U);
	assert(intel_ax211_firmware_parse(malformed, offset, &manifest) ==
	    INTEL_AX211_DUPLICATE);

	memset(malformed, 0, sizeof(malformed));
	put_le32(malformed + 4U, INTEL_AX211_TLV_MAGIC);
	offset = INTEL_AX211_TLV_HEADER_SIZE;
	for (size_t index = 0; index <= INTEL_AX211_MAX_FW_SECTIONS; index++) {
		uint8_t section[5] = { 0, 0, 0, 0, 0xaa };
		put_le32(section, 0x1000U + (uint32_t)index * 4U);
		offset = append_tlv(malformed, TEST_FW_CAPACITY, offset, 19U,
		    section, sizeof(section));
	}
	assert(intel_ax211_firmware_parse(malformed, offset, &manifest) ==
	    INTEL_AX211_OVERFLOW);
}

static size_t
append_sku(uint8_t *bytes, size_t offset,
	const struct intel_ax211_sku_id *sku)
{
	uint8_t payload[12];

	put_le32(payload, sku->data[0]);
	put_le32(payload + 4U, sku->data[1]);
	put_le32(payload + 8U, sku->data[2]);
	return append_tlv(bytes, TEST_PNVM_CAPACITY, offset, 64U, payload,
	    sizeof(payload));
}

static size_t
append_hw(uint8_t *bytes, size_t offset, uint16_t mac, uint16_t rf)
{
	uint8_t payload[4];

	put_le16(payload, mac);
	put_le16(payload + 2U, rf);
	return append_tlv(bytes, TEST_PNVM_CAPACITY, offset, 58U, payload,
	    sizeof(payload));
}

static void
test_pnvm_parser(void)
{
	uint8_t pnvm[TEST_PNVM_CAPACITY] = { 0 };
	struct intel_ax211_sku_id wanted = { { 1U, 2U, 3U } };
	struct intel_ax211_sku_id other = { { 4U, 5U, 6U } };
	struct intel_ax211_pnvm_manifest manifest;
	uint8_t section[7] = { 0, 0, 0, 0, 9, 8, 7 };
	size_t offset = 0;

	assert(intel_ax211_sku_equal(&wanted, &wanted));
	assert(!intel_ax211_sku_equal(&wanted, &other));
	offset = append_sku(pnvm, offset, &other);
	offset = append_u32(pnvm, TEST_PNVM_CAPACITY, offset, 62U,
	    0x11111111U);
	offset = append_hw(pnvm, offset, 0x22U, 0x33U);
	put_le32(section, 0x4000U);
	offset = append_tlv(pnvm, TEST_PNVM_CAPACITY, offset, 19U, section,
	    sizeof(section));
	offset = append_sku(pnvm, offset, &wanted);
	offset = append_u32(pnvm, TEST_PNVM_CAPACITY, offset, 62U,
	    0x89abcdefU);
	/* A SKU may list several HW_TYPE records before the exact match. */
	offset = append_hw(pnvm, offset, 0x99U, 0x99U);
	offset = append_hw(pnvm, offset, INTEL_AX211_MAC_TYPE_SO,
	    INTEL_AX211_RF_TYPE);
	put_le32(section, 0x5000U);
	offset = append_tlv(pnvm, TEST_PNVM_CAPACITY, offset, 19U, section,
	    sizeof(section));
	assert(intel_ax211_pnvm_parse(pnvm, offset, &wanted,
	    INTEL_AX211_MAC_TYPE_SO, INTEL_AX211_RF_TYPE,
	    &manifest) == INTEL_AX211_OK);
	assert(manifest.version == 0x89abcdefU);
	assert(manifest.section_count == 1U);
	assert(manifest.section[0].destination == 0x5000U);
	assert(manifest.section[0].length == 3U);
	assert(manifest.total_length == 3U);
	assert(intel_ax211_pnvm_parse(pnvm, offset, &wanted, 0x38U,
	    INTEL_AX211_RF_TYPE,
	    &manifest) == INTEL_AX211_IDENTITY_MISMATCH);
	assert(intel_ax211_pnvm_parse(pnvm, offset - 1U, &wanted,
	    INTEL_AX211_MAC_TYPE_SO, INTEL_AX211_RF_TYPE,
	    &manifest) == INTEL_AX211_TRUNCATED);
	offset = append_u32(pnvm, TEST_PNVM_CAPACITY, offset, 62U,
	    0x22222222U);
	assert(intel_ax211_pnvm_parse(pnvm, offset, &wanted,
	    INTEL_AX211_MAC_TYPE_SO, INTEL_AX211_RF_TYPE,
	    &manifest) == INTEL_AX211_DUPLICATE);
}

static void
test_descriptors(void)
{
	struct intel_ax211_context_info_gen3 context;
	struct intel_ax211_command_id command = { 0x11U, 0x22U, 0x33U };
	struct intel_ax211_ring_token token = { 7U, 9U };
	struct intel_ax211_event event;
	uint8_t encoded[INTEL_AX211_CONTEXT_INFO_GEN3_SIZE];
	uint8_t transfer[INTEL_AX211_RX_TRANSFER_DESCRIPTOR_SIZE];
	uint8_t completion[INTEL_AX211_RX_COMPLETION_DESCRIPTOR_SIZE] = { 0 };
	uint8_t tfd[INTEL_AX211_TFD_SIZE];
	uint8_t narrow[INTEL_AX211_NARROW_COMMAND_HEADER_SIZE];
	uint8_t wide[INTEL_AX211_WIDE_COMMAND_HEADER_SIZE];
	uint8_t packet[11] = { 0 };
	uint16_t buffer_id;
	uint8_t flags;
	struct intel_ax211_tfd_buffer buffers[2] = {
		{ UINT64_C(0x1000200030004000), 4U },
		{ UINT64_C(0x5000600070008000), 4092U }
	};

	memset(&context, 0, sizeof(context));
	context.version = 2U;
	context.config = 0x11223344U;
	context.prph_info_base = UINT64_C(0x0123456789abcdef);
	context.cr_head_index_base = UINT64_C(0x1111111122222222);
	context.command_transfer_ring_base = UINT64_C(0x3333333344444444);
	context.command_completion_ring_base = UINT64_C(0x5555555566666666);
	context.command_transfer_ring_size = INTEL_AX211_COMMAND_RING_CB_SIZE;
	context.command_completion_ring_size = INTEL_AX211_RX_RING_CB_SIZE;
	context.prph_scratch_base = UINT64_C(0x7777777788888888);
	context.prph_scratch_size = 0x1234U;
	assert(intel_ax211_context_info_gen3_encode(encoded, &context) ==
	    INTEL_AX211_OK);
	assert(get_le16(encoded) == 2U);
	assert(get_le16(encoded + 2U) == 26U);
	assert(get_le32(encoded + 4U) == 0x11223344U);
	assert(get_le64(encoded + 8U) == UINT64_C(0x0123456789abcdef));
	assert(get_le64(encoded + 52U) == UINT64_C(0x3333333344444444));
	assert(get_le16(encoded + 68U) == 5U);
	assert(get_le64(encoded + 88U) == UINT64_C(0x7777777788888888));
	assert(get_le32(encoded + 96U) == 0x1234U);
	assert(get_le32(encoded + 100U) == 0U);
	context.command_transfer_ring_size = 6U;
	assert(intel_ax211_context_info_gen3_encode(encoded, &context) ==
	    INTEL_AX211_INVALID);

	assert(intel_ax211_rx_transfer_descriptor_encode(transfer, 0x1234U,
	    UINT64_C(0x1020304050607080)) == INTEL_AX211_OK);
	assert(get_le16(transfer) == 0x1234U);
	assert(get_le32(transfer + 4U) == 0U);
	assert(get_le64(transfer + 8U) == UINT64_C(0x1020304050607080));
	put_le16(completion + 4U, 0xabcdU);
	completion[6] = 1U;
	assert(intel_ax211_rx_completion_descriptor_decode(completion,
	    &buffer_id, &flags) == INTEL_AX211_OK);
	assert(buffer_id == 0xabcdU && flags == 1U);
	assert(intel_ax211_tfd_encode(tfd, buffers, 2U) == INTEL_AX211_OK);
	assert(get_le16(tfd) == 2U);
	assert(get_le16(tfd + 2U) == 4U);
	assert(get_le64(tfd + 4U) == UINT64_C(0x1000200030004000));
	assert(get_le16(tfd + 12U) == 4092U);
	assert(get_le64(tfd + 14U) == UINT64_C(0x5000600070008000));
	assert(get_le32(tfd + 252U) == 0U);
	buffers[1].length = 4093U;
	assert(intel_ax211_tfd_encode(tfd, buffers, 2U) ==
	    INTEL_AX211_INVALID);
	for (size_t index = 0; index < sizeof(tfd); index++)
		assert(tfd[index] == 0U);

	assert(intel_ax211_narrow_command_encode(narrow, 0xa1U, 0x40U,
	    &token) == INTEL_AX211_OK);
	assert(narrow[0] == 0xa1U && narrow[1] == 0x40U &&
	    narrow[2] == 9U && narrow[3] == 7U);
	assert(intel_ax211_wide_command_encode(wide, &command, 0x456U,
	    &token) == INTEL_AX211_OK);
	assert(wide[0] == 0x11U && wide[1] == 0x22U && wide[2] == 9U &&
	    wide[3] == 7U && get_le16(wide + 4U) == 0x456U &&
	    wide[6] == 0U && wide[7] == 0x33U);

	put_le32(packet, 7U | (5U << 16));
	packet[4] = 0x71U;
	packet[5] = 0x40U;
	packet[6] = 9U;
	packet[7] = 7U;
	packet[8] = 1U;
	packet[9] = 2U;
	packet[10] = 3U;
	assert(intel_ax211_event_decode(packet, sizeof(packet), &event) ==
	    INTEL_AX211_OK);
	assert(event.command.opcode == 0x71U && event.flags == 0x40U);
	assert(event.index == 9U && event.queue == 7U && event.rx_queue == 5U);
	assert(event.payload_offset == 8U && event.payload_length == 3U);
	assert(intel_ax211_event_decode(packet, sizeof(packet) - 1U, &event) ==
	    INTEL_AX211_TRUNCATED);
}

static void
test_ring_and_staging(void)
{
	struct intel_ax211_ring ring;
	struct intel_ax211_ring_token token[5];
	struct intel_ax211_ring_token stale;
	struct intel_ax211_staging staging;
	static const uint8_t secret[] = "sensitive staging bytes";
	size_t index;

	assert(intel_ax211_ring_init(&ring, 3U, 4U) == INTEL_AX211_OK);
	for (index = 0; index < 4U; index++) {
		assert(intel_ax211_ring_reserve(&ring, &token[index]) ==
		    INTEL_AX211_OK);
		assert(token[index].index == index);
	}
	assert(intel_ax211_ring_available(&ring) == 0U);
	assert(intel_ax211_ring_reserve(&ring, &token[4]) ==
	    INTEL_AX211_FULL);
	stale = token[1];
	assert(intel_ax211_ring_complete(&ring, &stale) == INTEL_AX211_STALE);
	assert(intel_ax211_ring_complete(&ring, &token[0]) == INTEL_AX211_OK);
	assert(intel_ax211_ring_reserve(&ring, &token[4]) == INTEL_AX211_OK);
	assert(token[4].index == 0U);
	for (index = 1; index < 5U; index++)
		assert(intel_ax211_ring_complete(&ring, &token[index]) ==
		    INTEL_AX211_OK);
	assert(intel_ax211_ring_available(&ring) == 4U);
	assert(intel_ax211_ring_complete(&ring, &token[4]) ==
	    INTEL_AX211_STALE);

	memset(&staging, 0xa5, sizeof(staging));
	assert(intel_ax211_staging_set(&staging, secret, sizeof(secret)) ==
	    INTEL_AX211_OK);
	assert(staging.length == sizeof(secret));
	assert(memcmp(staging.bytes, secret, sizeof(secret)) == 0);
	intel_ax211_staging_clear(&staging);
	assert(staging.length == 0U);
	for (index = 0; index < sizeof(staging.bytes); index++)
		assert(staging.bytes[index] == 0U);
	assert(intel_ax211_staging_set(&staging, secret,
	    INTEL_AX211_STAGING_CAPACITY + 1U) == INTEL_AX211_OVERFLOW);
	for (index = 0; index < sizeof(staging.bytes); index++)
		assert(staging.bytes[index] == 0U);
}

static void
test_real_firmware(const char *path)
{
	struct intel_ax211_firmware_manifest manifest;
	uint8_t *bytes;
	FILE *file;
	long file_length;
	size_t index;
	int result;

	file = fopen(path, "rb");
	assert(file != NULL);
	assert(fseek(file, 0, SEEK_END) == 0);
	file_length = ftell(file);
	assert(file_length == (long)INTEL_AX211_FIRMWARE_SIZE);
	assert(fseek(file, 0, SEEK_SET) == 0);
	bytes = malloc((size_t)file_length);
	assert(bytes != NULL);
	assert(fread(bytes, 1, (size_t)file_length, file) ==
	    (size_t)file_length);
	assert(fclose(file) == 0);
	result = intel_ax211_firmware_parse(bytes, (size_t)file_length,
	    &manifest);
	if (result != INTEL_AX211_OK)
		fprintf(stderr, "real firmware parse result: %d\n", result);
	assert(result == INTEL_AX211_OK);
	assert(manifest.api_major == INTEL_AX211_FIRMWARE_API);
	assert(manifest.cpu_count == 2U);
	assert(manifest.iml_length == 13944U);
	assert(manifest.command_versions_length == 868U);
	assert(manifest.runtime_count == 60U);
	assert(manifest.lmac_count == 15U);
	assert(manifest.umac_count == 17U);
	assert(manifest.paging_count == 26U);
	/* Every exact runtime DMA object fits the frozen 32-KiB bound. */
	for (index = 0; index < manifest.runtime_count; index++) {
		if (manifest.runtime[index].destination ==
		    INTEL_AX211_CPU1_CPU2_SEPARATOR ||
		    manifest.runtime[index].destination ==
		    INTEL_AX211_PAGING_SEPARATOR) {
			assert(manifest.runtime[index].length == 0U);
		} else {
			assert(manifest.runtime[index].length != 0U);
			assert(manifest.runtime[index].length <= 32768U);
		}
	}
	free(bytes);
}

static void
test_real_pnvm(const char *path)
{
	struct intel_ax211_pnvm_inventory inventory;
	uint8_t *bytes;
	FILE *file;
	long file_length;

	file = fopen(path, "rb");
	assert(file != NULL);
	assert(fseek(file, 0, SEEK_END) == 0);
	file_length = ftell(file);
	assert(file_length == (long)INTEL_AX211_PNVM_SIZE);
	assert(fseek(file, 0, SEEK_SET) == 0);
	bytes = malloc((size_t)file_length);
	assert(bytes != NULL);
	assert(fread(bytes, 1, (size_t)file_length, file) ==
	    (size_t)file_length);
	assert(fclose(file) == 0);
	assert(intel_ax211_pnvm_inspect(bytes, (size_t)file_length,
	    &inventory) == INTEL_AX211_OK);
	assert(inventory.sku_count == 4U);
	assert(inventory.version_count == 4U);
	assert(inventory.hardware_type_count == 8U);
	assert(inventory.supported_hardware_type_count == 8U);
	assert(inventory.section_count == 8U);
	assert(inventory.total_section_length != 0U);
	free(bytes);
}

int
main(int argc, char **argv)
{
	test_identity_and_metadata();
	test_firmware_parser();
	test_pnvm_parser();
	test_descriptors();
	test_ring_and_staging();
	if (argc >= 2)
		test_real_firmware(argv[1]);
	if (argc >= 3)
		test_real_pnvm(argv[2]);
	puts("intel ax211 core: PASS");
	return 0;
}
