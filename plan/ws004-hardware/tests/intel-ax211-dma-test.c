/*
 * Intel AX211 Gen3 DMA ownership fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/dma.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-dma.h"

#define TEST_CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "intel ax211 dma: check failed at %s:%d\n", \
		    __FILE__, __LINE__); \
		return __LINE__; \
	} \
} while (0)

#define FIXTURE_GUARD_SIZE 32U
#define FIXTURE_RECORD_COUNT 800U
#define FIXTURE_BOOT_ALLOCATION_COUNT 527U
#define FIXTURE_EXACT_BOOT_ALLOCATION_COUNT 582U
#define FIXTURE_PNVM_SEGMENT_COUNT 3U
#define FIXTURE_PNVM_ALLOCATION_COUNT 4U

#ifdef INTEL_AX211_DMA_EXACT_BLOB_TEST
extern const uint8_t _binary_ucode_bin_start[];
extern const uint8_t _binary_ucode_bin_end[];
extern const uint8_t _binary_pnvm_bin_start[];
extern const uint8_t _binary_pnvm_bin_end[];
#endif

struct drv_dma_device {
	unsigned marker;
};

enum fixture_invalid_allocation {
	FIXTURE_ALLOCATION_VALID = 0,
	FIXTURE_ALLOCATION_SHORT,
	FIXTURE_ALLOCATION_MISALIGNED,
	FIXTURE_ALLOCATION_OVERFLOW
};

struct fixture_record {
	uint8_t *base;
	uint8_t *payload;
	size_t size;
	uint64_t device_address;
	unsigned sequence;
	unsigned scrubbed;
	unsigned active;
};

static struct drv_dma_device fixture_device = { 0x211U };
static struct fixture_record records[FIXTURE_RECORD_COUNT];
static size_t history_size[FIXTURE_RECORD_COUNT];
static size_t history_alignment[FIXTURE_RECORD_COUNT];
static uint64_t history_address[FIXTURE_RECORD_COUNT];
static uint64_t next_device_address;
static unsigned allocation_attempts;
static unsigned allocation_successes;
static unsigned free_count;
static unsigned scrub_count;
static unsigned failure_attempt;
static unsigned invalid_attempt;
static unsigned reverse_required;
static unsigned reverse_failure;
static unsigned scrub_failure;
static enum fixture_invalid_allocation invalid_allocation;

static uint16_t
fixture_get_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t
fixture_get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t
fixture_get_le64(const uint8_t *bytes)
{
	uint64_t value = 0U;
	unsigned index;

	for (index = 0U; index < 8U; index++)
		value |= (uint64_t)bytes[index] << (index * 8U);
	return value;
}

static uint64_t
fixture_align(uint64_t value, size_t alignment)
{
	uint64_t mask = (uint64_t)alignment - 1U;

	return (value + mask) & ~mask;
}

static int
fixture_bytes_are(const uint8_t *bytes, size_t length, uint8_t value)
{
	size_t index;

	for (index = 0U; index < length; index++) {
		if (bytes[index] != value)
			return 0;
	}
	return 1;
}

static unsigned
fixture_active_count(void)
{
	unsigned count = 0U;
	unsigned index;

	for (index = 0U; index < FIXTURE_RECORD_COUNT; index++) {
		if (records[index].active)
			count++;
	}
	return count;
}

static unsigned
fixture_latest_active_sequence(void)
{
	unsigned sequence = 0U;
	unsigned index;

	for (index = 0U; index < FIXTURE_RECORD_COUNT; index++) {
		if (records[index].active && records[index].sequence > sequence)
			sequence = records[index].sequence;
	}
	return sequence;
}

static struct fixture_record *
fixture_record_from_buffer(const struct drv_dma_buffer *buffer)
{
	uintptr_t identity;

	if (buffer == NULL)
		return NULL;
	identity = buffer->private_data[0];
	if (identity == 0U || identity > FIXTURE_RECORD_COUNT)
		return NULL;
	return &records[identity - 1U];
}

static struct fixture_record *
fixture_record_from_memory(void *memory)
{
	unsigned index;

	for (index = 0U; index < FIXTURE_RECORD_COUNT; index++) {
		if (records[index].active && records[index].payload == memory)
			return &records[index];
	}
	return NULL;
}

static int
fixture_allocator_reset(void)
{
	TEST_CHECK(fixture_active_count() == 0U);
	memset(records, 0, sizeof(records));
	memset(history_size, 0, sizeof(history_size));
	memset(history_alignment, 0, sizeof(history_alignment));
	memset(history_address, 0, sizeof(history_address));
	next_device_address = UINT64_C(0x100000000);
	allocation_attempts = 0U;
	allocation_successes = 0U;
	free_count = 0U;
	scrub_count = 0U;
	failure_attempt = 0U;
	invalid_attempt = 0U;
	reverse_required = 0U;
	reverse_failure = 0U;
	scrub_failure = 0U;
	invalid_allocation = FIXTURE_ALLOCATION_VALID;
	return 0;
}

int
drv_dma_alloc_coherent(struct drv_dma_device *device, size_t size,
	size_t alignment, struct drv_dma_buffer *buffer)
{
	struct fixture_record *record = NULL;
	uint64_t device_address;
	size_t reported_size = size;
	unsigned index;

	if (device != &fixture_device || size == 0U || alignment == 0U ||
	    (alignment & (alignment - 1U)) != 0U || buffer == NULL)
		return EINVAL;
	allocation_attempts++;
	if (allocation_attempts <= FIXTURE_RECORD_COUNT) {
		history_size[allocation_attempts - 1U] = size;
		history_alignment[allocation_attempts - 1U] = alignment;
	}
	if (allocation_attempts == failure_attempt)
		return ENOMEM;
	for (index = 0U; index < FIXTURE_RECORD_COUNT; index++) {
		if (!records[index].active) {
			record = &records[index];
			break;
		}
	}
	if (record == NULL)
		return ENOMEM;
	device_address = fixture_align(next_device_address, alignment);
	if (allocation_attempts == invalid_attempt) {
		if (invalid_allocation == FIXTURE_ALLOCATION_SHORT)
			reported_size--;
		else if (invalid_allocation == FIXTURE_ALLOCATION_MISALIGNED)
			device_address++;
		else if (invalid_allocation == FIXTURE_ALLOCATION_OVERFLOW)
			device_address = UINT64_MAX - (uint64_t)reported_size + 2U;
	}
	record->base = malloc(reported_size + FIXTURE_GUARD_SIZE * 2U);
	if (record->base == NULL)
		return ENOMEM;
	record->payload = record->base + FIXTURE_GUARD_SIZE;
	record->size = reported_size;
	record->device_address = device_address;
	record->sequence = allocation_attempts;
	record->active = 1U;
	memset(record->base, 0xa5, FIXTURE_GUARD_SIZE);
	memset(record->payload, 0xcc, reported_size);
	memset(record->payload + reported_size, 0x5a, FIXTURE_GUARD_SIZE);
	buffer->address = record->payload;
	buffer->device_address = device_address;
	buffer->size = reported_size;
	buffer->private_data[0] = (uintptr_t)(record - records) + 1U;
	buffer->private_data[1] = 0U;
	if (allocation_attempts <= FIXTURE_RECORD_COUNT)
		history_address[allocation_attempts - 1U] = device_address;
	allocation_successes++;
	next_device_address = fixture_align(device_address + reported_size +
	    0x40U, 0x40U);
	return 0;
}

void
intel_ax211_dma_host_scrub(void *memory, size_t length)
{
	struct fixture_record *record = fixture_record_from_memory(memory);

	if (record == NULL || record->size != length || record->scrubbed)
		scrub_failure = 1U;
	else {
		record->scrubbed = 1U;
		scrub_count++;
	}
	intel_ax211_scrub(memory, length);
}

void
drv_dma_free_coherent(struct drv_dma_device *device,
	struct drv_dma_buffer *buffer)
{
	struct fixture_record *record = fixture_record_from_buffer(buffer);

	if (device != &fixture_device || record == NULL || !record->active) {
		scrub_failure = 1U;
		return;
	}
	if (reverse_required && record->sequence !=
	    fixture_latest_active_sequence())
		reverse_failure = 1U;
	if (!record->scrubbed ||
	    !fixture_bytes_are(record->base, FIXTURE_GUARD_SIZE, 0xa5U) ||
	    !fixture_bytes_are(record->payload, record->size, 0U) ||
	    !fixture_bytes_are(record->payload + record->size,
	    FIXTURE_GUARD_SIZE, 0x5aU))
		scrub_failure = 1U;
	free(record->base);
	memset(record, 0, sizeof(*record));
	memset(buffer, 0, sizeof(*buffer));
	free_count++;
}

static void
fixture_make_boot_manifest(struct intel_ax211_firmware_manifest *manifest)
{
	memset(manifest, 0, sizeof(*manifest));
	manifest->iml_offset = 128U;
	manifest->iml_length = INTEL_AX211_IML_SIZE;
	manifest->runtime_count = 5U;
	manifest->lmac_count = 1U;
	manifest->umac_count = 1U;
	manifest->paging_count = 1U;
	manifest->runtime[0].destination = 0x00440000U;
	manifest->runtime[0].file_offset = 20000U;
	manifest->runtime[0].length = 16U;
	manifest->runtime[1].destination = INTEL_AX211_CPU1_CPU2_SEPARATOR;
	manifest->runtime[2].destination = 0x80440000U;
	manifest->runtime[2].file_offset = 21000U;
	manifest->runtime[2].length = 24U;
	manifest->runtime[3].destination = INTEL_AX211_PAGING_SEPARATOR;
	manifest->runtime[4].destination = 0x01000000U;
	manifest->runtime[4].file_offset = 22000U;
	manifest->runtime[4].length = 32U;
}

static void
fixture_make_pnvm_manifest(struct intel_ax211_pnvm_manifest *manifest)
{
	memset(manifest, 0, sizeof(*manifest));
	manifest->section_count = FIXTURE_PNVM_SEGMENT_COUNT;
	manifest->section[0].destination = 0x1000U;
	manifest->section[0].file_offset = 100U;
	manifest->section[0].length = 5U;
	manifest->section[1].destination = 0x2000U;
	manifest->section[1].file_offset = 500U;
	manifest->section[1].length = 7U;
	manifest->section[2].destination = 0x3000U;
	manifest->section[2].file_offset = 1000U;
	manifest->section[2].length = 9U;
	manifest->total_length = 21U;
}

static uint8_t *
fixture_make_bytes(size_t length)
{
	uint8_t *bytes = malloc(length);
	size_t index;

	if (bytes == NULL)
		return NULL;
	for (index = 0U; index < length; index++)
		bytes[index] = (uint8_t)(index * 29U + 7U);
	return bytes;
}

static int
fixture_check_history(void)
{
	static const size_t fixed_size[11] = {
		INTEL_AX211_CONTEXT_INFO_GEN3_SIZE,
		INTEL_AX211_PRPH_SCRATCH_SIZE,
		INTEL_AX211_PRPH_INFO_SIZE,
		INTEL_AX211_ICT_SIZE,
		INTEL_AX211_COMMAND_TFD_RING_SIZE,
		INTEL_AX211_COMMAND_BC_TABLE_SIZE,
		INTEL_AX211_COMMAND_SLOTS_SIZE,
		INTEL_AX211_COMMAND_EXTERNAL_SIZE,
		INTEL_AX211_RX_TRANSFER_RING_SIZE,
		INTEL_AX211_RX_COMPLETION_RING_SIZE,
		INTEL_AX211_RX_STATUS_SIZE
	};
	static const size_t fixed_alignment[11] = {
		1U, 1U, 1U, INTEL_AX211_ICT_SIZE, 256U, 128U, 64U,
		64U, 256U, 256U, 16U
	};
	unsigned index;

	TEST_CHECK(allocation_attempts == FIXTURE_BOOT_ALLOCATION_COUNT);
	for (index = 0U; index < 11U; index++) {
		TEST_CHECK(history_size[index] == fixed_size[index]);
		TEST_CHECK(history_alignment[index] == fixed_alignment[index]);
	}
	TEST_CHECK(history_size[11] == INTEL_AX211_IML_SIZE);
	TEST_CHECK(history_size[12] == 16U);
	TEST_CHECK(history_size[13] == 24U);
	TEST_CHECK(history_size[14] == 32U);
	for (index = 15U; index < FIXTURE_BOOT_ALLOCATION_COUNT; index++) {
		TEST_CHECK(history_size[index] == INTEL_AX211_RX_BUFFER_SIZE);
		TEST_CHECK(history_alignment[index] == INTEL_AX211_RX_BUFFER_SIZE);
	}
	return 0;
}

static int
fixture_check_boot_layout(const uint8_t *firmware,
	const struct intel_ax211_dma_resources *resources)
{
	const uint8_t *scratch = resources->scratch.address;
	const uint8_t *context = resources->context.address;
	const uint8_t *descriptors = resources->rx_transfer.address;
	size_t index;

	TEST_CHECK(resources->firmware_count == 3U);
	TEST_CHECK(resources->rx_buffer_count == INTEL_AX211_RX_RING_SIZE);
	TEST_CHECK(resources->rx_status.size == 2U);
	TEST_CHECK(resources->command_external.size ==
	    INTEL_AX211_COMMAND_EXTERNAL_SIZE);
	TEST_CHECK((resources->command_external.device_address & 63U) == 0U);
	TEST_CHECK(fixture_bytes_are(resources->command_external.address,
	    resources->command_external.size, 0U));
	TEST_CHECK(memcmp(resources->iml.address, firmware + 128U,
	    INTEL_AX211_IML_SIZE) == 0);
	TEST_CHECK(memcmp(resources->firmware[0].buffer.address,
	    firmware + 20000U, 16U) == 0);
	TEST_CHECK(memcmp(resources->firmware[1].buffer.address,
	    firmware + 21000U, 24U) == 0);
	TEST_CHECK(memcmp(resources->firmware[2].buffer.address,
	    firmware + 22000U, 32U) == 0);
	TEST_CHECK(resources->firmware[0].image_class ==
	    INTEL_AX211_DMA_IMAGE_LMAC);
	TEST_CHECK(resources->firmware[1].image_class ==
	    INTEL_AX211_DMA_IMAGE_UMAC);
	TEST_CHECK(resources->firmware[2].image_class ==
	    INTEL_AX211_DMA_IMAGE_PAGING);

	TEST_CHECK(fixture_get_le16(scratch) == 0x0370U);
	TEST_CHECK(fixture_get_le16(scratch + 2U) == 0U);
	TEST_CHECK(fixture_get_le16(scratch + 4U) ==
	    INTEL_AX211_PRPH_SCRATCH_SIZE / 4U);
	TEST_CHECK(fixture_get_le32(scratch + 8U) == 0x000f0000U);
	TEST_CHECK(fixture_get_le64(scratch + 16U) == 0U);
	TEST_CHECK(fixture_get_le32(scratch + 24U) == 0U);
	TEST_CHECK(fixture_get_le64(scratch + 48U) ==
	    resources->rx_transfer.device_address);
	TEST_CHECK(fixture_get_le64(scratch + 124U) ==
	    resources->firmware[1].buffer.device_address);
	TEST_CHECK(fixture_get_le64(scratch + 636U) ==
	    resources->firmware[0].buffer.device_address);
	TEST_CHECK(fixture_get_le64(scratch + 1148U) ==
	    resources->firmware[2].buffer.device_address);
	TEST_CHECK(fixture_get_le64(scratch + 132U) == 0U);
	TEST_CHECK(fixture_get_le64(scratch + 644U) == 0U);
	TEST_CHECK(fixture_get_le64(scratch + 1156U) == 0U);

	TEST_CHECK(fixture_get_le16(context) == 0U);
	TEST_CHECK(fixture_get_le16(context + 2U) ==
	    INTEL_AX211_CONTEXT_INFO_GEN3_SIZE / 4U);
	TEST_CHECK(fixture_get_le32(context + 4U) == 0U);
	TEST_CHECK(fixture_get_le64(context + 8U) ==
	    resources->prph_info.device_address);
	TEST_CHECK(fixture_get_le64(context + 16U) ==
	    resources->rx_status.device_address);
	TEST_CHECK(fixture_get_le64(context + 24U) ==
	    resources->prph_info.device_address + 2048U);
	TEST_CHECK(fixture_get_le64(context + 32U) ==
	    resources->prph_info.device_address + 3072U);
	TEST_CHECK(fixture_get_le64(context + 40U) == 0U);
	TEST_CHECK(fixture_get_le16(context + 48U) == 0U);
	TEST_CHECK(fixture_get_le16(context + 50U) == 0U);
	TEST_CHECK(fixture_get_le64(context + 52U) ==
	    resources->command_tfd.device_address);
	TEST_CHECK(fixture_get_le64(context + 60U) ==
	    resources->rx_completion.device_address);
	TEST_CHECK(fixture_get_le16(context + 68U) ==
	    INTEL_AX211_COMMAND_RING_CB_SIZE);
	TEST_CHECK(fixture_get_le16(context + 70U) ==
	    INTEL_AX211_RX_RING_CB_SIZE);
	TEST_CHECK(fixture_bytes_are(context + 72U, 16U, 0U));
	TEST_CHECK(fixture_get_le64(context + 88U) ==
	    resources->scratch.device_address);
	TEST_CHECK(fixture_get_le32(context + 96U) ==
	    INTEL_AX211_PRPH_SCRATCH_SIZE);
	TEST_CHECK(fixture_get_le32(context + 100U) == 0U);

	for (index = 0U; index < INTEL_AX211_RX_RING_SIZE; index++) {
		const uint8_t *descriptor = descriptors +
		    index * INTEL_AX211_RX_TRANSFER_DESCRIPTOR_SIZE;

		TEST_CHECK(fixture_get_le16(descriptor) == index);
		TEST_CHECK(fixture_bytes_are(descriptor + 2U, 6U, 0U));
		TEST_CHECK(fixture_get_le64(descriptor + 8U) ==
		    resources->rx_buffer[index].device_address);
		TEST_CHECK(resources->rx_buffer[index].size ==
		    INTEL_AX211_RX_BUFFER_SIZE);
		TEST_CHECK(fixture_bytes_are(resources->rx_buffer[index].address,
		    resources->rx_buffer[index].size, 0U));
	}
	return 0;
}

static int
fixture_boot_failure_matrix(uint8_t *firmware,
	const struct intel_ax211_firmware_manifest *manifest)
{
	unsigned failure;

	for (failure = 1U; failure <= FIXTURE_BOOT_ALLOCATION_COUNT;
	    failure++) {
		struct intel_ax211_dma_resources resources;
		int error;

		TEST_CHECK(fixture_allocator_reset() == 0);
		memset(&resources, 0, sizeof(resources));
		failure_attempt = failure;
		reverse_required = 1U;
		error = intel_ax211_dma_prepare_boot(&fixture_device, firmware,
		    INTEL_AX211_FIRMWARE_SIZE, manifest, 0x0370U, &resources);
		TEST_CHECK(error == ENOMEM);
		TEST_CHECK(allocation_attempts == failure);
		TEST_CHECK(fixture_active_count() == 0U);
		TEST_CHECK(resources.device == NULL && !resources.boot_prepared);
		TEST_CHECK(!reverse_failure && !scrub_failure);
		TEST_CHECK(scrub_count == free_count);
	}
	return 0;
}

static int
fixture_invalid_allocation_matrix(uint8_t *firmware,
	const struct intel_ax211_firmware_manifest *manifest)
{
	enum fixture_invalid_allocation invalid;

	for (invalid = FIXTURE_ALLOCATION_SHORT;
	    invalid <= FIXTURE_ALLOCATION_OVERFLOW; invalid++) {
		struct intel_ax211_dma_resources resources;

		TEST_CHECK(fixture_allocator_reset() == 0);
		memset(&resources, 0, sizeof(resources));
		invalid_attempt = invalid == FIXTURE_ALLOCATION_MISALIGNED ?
		    4U : 1U;
		invalid_allocation = invalid;
		reverse_required = 1U;
		TEST_CHECK(intel_ax211_dma_prepare_boot(&fixture_device, firmware,
		    INTEL_AX211_FIRMWARE_SIZE, manifest, 0x0370U,
		    &resources) == EIO);
		TEST_CHECK(fixture_active_count() == 0U);
		TEST_CHECK(!reverse_failure && !scrub_failure);
		TEST_CHECK(scrub_count == free_count &&
		    free_count == invalid_attempt);
	}
	return 0;
}

static int
fixture_bounds(uint8_t *firmware,
	const struct intel_ax211_firmware_manifest *manifest)
{
	struct intel_ax211_dma_resources resources;
	struct intel_ax211_firmware_manifest bad;

	TEST_CHECK(fixture_allocator_reset() == 0);
	memset(&resources, 0, sizeof(resources));
	TEST_CHECK(intel_ax211_dma_prepare_boot(&fixture_device, firmware,
	    INTEL_AX211_FIRMWARE_SIZE - 1U, manifest, 0x0370U,
	    &resources) == EINVAL);
	TEST_CHECK(allocation_attempts == 0U);
	TEST_CHECK(intel_ax211_dma_prepare_boot(&fixture_device, firmware,
	    INTEL_AX211_FIRMWARE_SIZE, manifest, 0x0420U,
	    &resources) == EINVAL);
	TEST_CHECK(allocation_attempts == 0U);
	bad = *manifest;
	bad.iml_length--;
	TEST_CHECK(intel_ax211_dma_prepare_boot(&fixture_device, firmware,
	    INTEL_AX211_FIRMWARE_SIZE, &bad, 0x0370U, &resources) == EINVAL);
	TEST_CHECK(fixture_active_count() == 0U && !scrub_failure);
	bad = *manifest;
	bad.runtime[1].destination = 0U;
	TEST_CHECK(intel_ax211_dma_prepare_boot(&fixture_device, firmware,
	    INTEL_AX211_FIRMWARE_SIZE, &bad, 0x0370U, &resources) == EINVAL);
	TEST_CHECK(allocation_attempts == 0U);
	bad = *manifest;
	bad.runtime[0].length = INTEL_AX211_FIRMWARE_SECTION_SIZE_MAX + 1U;
	reverse_required = 1U;
	TEST_CHECK(intel_ax211_dma_prepare_boot(&fixture_device, firmware,
	    INTEL_AX211_FIRMWARE_SIZE, &bad, 0x0370U, &resources) == EINVAL);
	TEST_CHECK(fixture_active_count() == 0U);
	TEST_CHECK(!reverse_failure && !scrub_failure);
	return 0;
}

static int
fixture_success_and_pnvm(uint8_t *firmware,
	const struct intel_ax211_firmware_manifest *manifest, uint8_t *pnvm,
	const struct intel_ax211_pnvm_manifest *pnvm_manifest)
{
	struct intel_ax211_dma_resources resources;
	struct intel_ax211_pnvm_manifest bad;
	const uint8_t *scratch;
	unsigned active_after_boot;
	unsigned free_before;
	unsigned index;
	int error;

	TEST_CHECK(fixture_allocator_reset() == 0);
	memset(&resources, 0, sizeof(resources));
	TEST_CHECK(intel_ax211_dma_prepare_boot(&fixture_device, firmware,
	    INTEL_AX211_FIRMWARE_SIZE, manifest, 0x0370U, &resources) == 0);
	error = fixture_check_history();
	if (error != 0)
		return error;
	error = fixture_check_boot_layout(firmware, &resources);
	if (error != 0)
		return error;
	active_after_boot = fixture_active_count();
	TEST_CHECK(active_after_boot == FIXTURE_BOOT_ALLOCATION_COUNT);
	TEST_CHECK(intel_ax211_dma_prepare_pnvm(pnvm, INTEL_AX211_PNVM_SIZE,
	    pnvm_manifest, &resources) == EINVAL);
	TEST_CHECK(fixture_active_count() == active_after_boot);

	/* Accepted ALIVE retires IML and LMAC/UMAC, never paging or rings. */
	free_before = free_count;
	intel_ax211_dma_release_boot_images(&resources);
	TEST_CHECK(resources.boot_images_released);
	TEST_CHECK(resources.iml.address == NULL);
	TEST_CHECK(resources.firmware[0].buffer.address == NULL);
	TEST_CHECK(resources.firmware[1].buffer.address == NULL);
	TEST_CHECK(resources.firmware[2].buffer.address != NULL);
	TEST_CHECK(free_count == free_before + 3U);
	TEST_CHECK(fixture_active_count() == active_after_boot - 3U);
	intel_ax211_dma_release_boot_images(&resources);
	TEST_CHECK(free_count == free_before + 3U);

	/* Malformed fragmented manifests do not mutate scratch or ownership. */
	bad = *pnvm_manifest;
	bad.section[1].file_offset = INTEL_AX211_PNVM_SIZE;
	TEST_CHECK(intel_ax211_dma_prepare_pnvm(pnvm, INTEL_AX211_PNVM_SIZE,
	    &bad, &resources) == EINVAL);
	TEST_CHECK(!resources.pnvm_prepared && resources.pnvm_count == 0U);
	scratch = resources.scratch.address;
	TEST_CHECK(fixture_get_le64(scratch + 16U) == 0U);
	TEST_CHECK(fixture_get_le32(scratch + 24U) == 0U);
	bad = *pnvm_manifest;
	bad.total_length++;
	TEST_CHECK(intel_ax211_dma_prepare_pnvm(pnvm, INTEL_AX211_PNVM_SIZE,
	    &bad, &resources) == EINVAL);
	bad = *pnvm_manifest;
	bad.total_length = (size_t)UINT32_MAX + 1U;
	TEST_CHECK(intel_ax211_dma_prepare_pnvm(pnvm, INTEL_AX211_PNVM_SIZE,
	    &bad, &resources) == EINVAL);
	TEST_CHECK(intel_ax211_dma_prepare_pnvm(pnvm,
	    INTEL_AX211_PNVM_SIZE - 1U, pnvm_manifest, &resources) == EINVAL);

	/* Every pointer-table/segment allocation failure unwinds in reverse. */
	for (index = 1U; index <= FIXTURE_PNVM_ALLOCATION_COUNT; index++) {
		unsigned active_before = fixture_active_count();

		failure_attempt = allocation_attempts + index;
		reverse_required = 1U;
		TEST_CHECK(intel_ax211_dma_prepare_pnvm(pnvm,
		    INTEL_AX211_PNVM_SIZE, pnvm_manifest, &resources) == ENOMEM);
		reverse_required = 0U;
		TEST_CHECK(fixture_active_count() == active_before);
		TEST_CHECK(resources.pnvm_count == 0U &&
		    resources.pnvm_table.address == NULL);
		TEST_CHECK(fixture_get_le64(scratch + 16U) == 0U);
		TEST_CHECK(fixture_get_le32(scratch + 24U) == 0U);
		TEST_CHECK(!reverse_failure && !scrub_failure);
		failure_attempt = 0U;
	}

	TEST_CHECK(intel_ax211_dma_prepare_pnvm(pnvm, INTEL_AX211_PNVM_SIZE,
	    pnvm_manifest, &resources) == 0);
	TEST_CHECK(resources.pnvm_prepared);
	TEST_CHECK(resources.pnvm_count == FIXTURE_PNVM_SEGMENT_COUNT);
	TEST_CHECK(resources.pnvm_total_length == 21U);
	TEST_CHECK(fixture_get_le64(scratch + 16U) ==
	    resources.pnvm_table.device_address);
	TEST_CHECK(fixture_get_le32(scratch + 24U) == 21U);
	for (index = 0U; index < FIXTURE_PNVM_SEGMENT_COUNT; index++) {
		TEST_CHECK(fixture_get_le64(resources.pnvm_table.address +
		    index * 8U) == resources.pnvm[index].device_address);
		TEST_CHECK(memcmp(resources.pnvm[index].address,
		    pnvm + pnvm_manifest->section[index].file_offset,
		    pnvm_manifest->section[index].length) == 0);
	}
	TEST_CHECK(fixture_bytes_are((uint8_t *)resources.pnvm_table.address +
	    FIXTURE_PNVM_SEGMENT_COUNT * 8U,
	    INTEL_AX211_PNVM_ADDRESS_TABLE_SIZE -
	    FIXTURE_PNVM_SEGMENT_COUNT * 8U, 0U));

	reverse_required = 1U;
	intel_ax211_dma_release(&resources);
	reverse_required = 0U;
	TEST_CHECK(resources.device == NULL && !resources.boot_prepared);
	TEST_CHECK(fixture_active_count() == 0U);
	TEST_CHECK(!reverse_failure && !scrub_failure);
	TEST_CHECK(scrub_count == free_count && free_count == allocation_successes);
	intel_ax211_dma_release(&resources);
	return 0;
}

#ifdef INTEL_AX211_DMA_EXACT_BLOB_TEST
static int
fixture_first_sku(const uint8_t *bytes, size_t length,
	struct intel_ax211_sku_id *sku)
{
	size_t offset = 0U;

	while (offset <= length && length - offset >= 8U) {
		uint32_t type = fixture_get_le32(bytes + offset);
		uint32_t payload_length = fixture_get_le32(bytes + offset + 4U);
		size_t padded;

		offset += 8U;
		padded = (size_t)payload_length + 3U;
		if (padded < (size_t)payload_length)
			return EINVAL;
		padded &= ~(size_t)3U;
		if (padded > length - offset)
			return EINVAL;
		if (type == 64U && payload_length == 12U) {
			sku->data[0] = fixture_get_le32(bytes + offset);
			sku->data[1] = fixture_get_le32(bytes + offset + 4U);
			sku->data[2] = fixture_get_le32(bytes + offset + 8U);
			return 0;
		}
		offset += padded;
	}
	return ENOENT;
}

static int
fixture_exact_blobs(void)
{
	const uint8_t *ucode = _binary_ucode_bin_start;
	const uint8_t *pnvm = _binary_pnvm_bin_start;
	size_t ucode_size = (size_t)(_binary_ucode_bin_end -
	    _binary_ucode_bin_start);
	size_t pnvm_size = (size_t)(_binary_pnvm_bin_end -
	    _binary_pnvm_bin_start);
	struct intel_ax211_firmware_manifest firmware_manifest;
	struct intel_ax211_pnvm_manifest pnvm_manifest;
	struct intel_ax211_dma_resources resources;
	struct intel_ax211_sku_id sku;
	unsigned failure;

	TEST_CHECK(ucode_size == INTEL_AX211_FIRMWARE_SIZE);
	TEST_CHECK(pnvm_size == INTEL_AX211_PNVM_SIZE);
	TEST_CHECK(intel_ax211_firmware_parse(ucode, ucode_size,
	    &firmware_manifest) == INTEL_AX211_OK);
	TEST_CHECK(firmware_manifest.iml_length == INTEL_AX211_IML_SIZE);
	TEST_CHECK(firmware_manifest.lmac_count == 15U);
	TEST_CHECK(firmware_manifest.umac_count == 17U);
	TEST_CHECK(firmware_manifest.paging_count == 26U);
	TEST_CHECK(fixture_first_sku(pnvm, pnvm_size, &sku) == 0);
	TEST_CHECK(intel_ax211_pnvm_parse(pnvm, pnvm_size, &sku,
	    INTEL_AX211_MAC_TYPE_SO, INTEL_AX211_RF_TYPE,
	    &pnvm_manifest) == INTEL_AX211_OK);
	TEST_CHECK(pnvm_manifest.section_count == 2U);

	TEST_CHECK(fixture_allocator_reset() == 0);
	memset(&resources, 0, sizeof(resources));
	TEST_CHECK(intel_ax211_dma_prepare_boot(&fixture_device, ucode,
	    ucode_size, &firmware_manifest, 0x0370U, &resources) == 0);
	TEST_CHECK(allocation_attempts == FIXTURE_EXACT_BOOT_ALLOCATION_COUNT);
	intel_ax211_dma_release_boot_images(&resources);
	TEST_CHECK(intel_ax211_dma_prepare_pnvm(pnvm, pnvm_size,
	    &pnvm_manifest, &resources) == 0);
	reverse_required = 1U;
	intel_ax211_dma_release(&resources);
	reverse_required = 0U;
	TEST_CHECK(fixture_active_count() == 0U);
	TEST_CHECK(!reverse_failure && !scrub_failure);
	TEST_CHECK(scrub_count == free_count && free_count == allocation_successes);

	/* The exact 58-section image also unwinds every acquisition index. */
	for (failure = 1U; failure <= FIXTURE_EXACT_BOOT_ALLOCATION_COUNT;
	    failure++) {
		TEST_CHECK(fixture_allocator_reset() == 0);
		memset(&resources, 0, sizeof(resources));
		failure_attempt = failure;
		reverse_required = 1U;
		TEST_CHECK(intel_ax211_dma_prepare_boot(&fixture_device, ucode,
		    ucode_size, &firmware_manifest, 0x0370U,
		    &resources) == ENOMEM);
		TEST_CHECK(allocation_attempts == failure);
		TEST_CHECK(fixture_active_count() == 0U);
		TEST_CHECK(resources.device == NULL && !resources.boot_prepared);
		TEST_CHECK(!reverse_failure && !scrub_failure);
		TEST_CHECK(scrub_count == free_count);
	}
	return 0;
}
#endif

int
main(void)
{
	struct intel_ax211_firmware_manifest firmware_manifest;
	struct intel_ax211_pnvm_manifest pnvm_manifest;
	uint8_t *firmware;
	uint8_t *pnvm;
	int error;

	firmware = fixture_make_bytes(INTEL_AX211_FIRMWARE_SIZE);
	pnvm = fixture_make_bytes(INTEL_AX211_PNVM_SIZE);
	TEST_CHECK(firmware != NULL && pnvm != NULL);
	fixture_make_boot_manifest(&firmware_manifest);
	fixture_make_pnvm_manifest(&pnvm_manifest);

	error = fixture_success_and_pnvm(firmware, &firmware_manifest, pnvm,
	    &pnvm_manifest);
	if (error == 0)
		error = fixture_boot_failure_matrix(firmware, &firmware_manifest);
	if (error == 0)
		error = fixture_invalid_allocation_matrix(firmware,
		    &firmware_manifest);
	if (error == 0)
		error = fixture_bounds(firmware, &firmware_manifest);
	free(pnvm);
	free(firmware);
	if (error != 0)
		return error;

#ifdef INTEL_AX211_DMA_EXACT_BLOB_TEST
	error = fixture_exact_blobs();
	if (error != 0)
		return error;
#endif
	puts("intel ax211 dma: PASS");
	return 0;
}
