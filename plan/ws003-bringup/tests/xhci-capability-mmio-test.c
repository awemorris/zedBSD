/*
 * WS003 BR-T25 xHCI capability/MMIO and PCI enable-state fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/pci-xhci-capability.h>
#include <drivers/pci.h>
#include <hal/hal.h>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PCI_COMMAND 0x04U
#define TEST_PCI_COMMAND_IO 0x0001U
#define TEST_PCI_COMMAND_MEMORY 0x0002U
#define TEST_PCI_COMMAND_MASTER 0x0004U
#define TEST_PCI_COMMAND_SERR 0x0100U
#define TEST_PCI_COMMAND_INTX_DISABLE 0x0400U
#define TEST_PCI_BAR0 0x10U
#define TEST_PCI_BAR2 0x18U
#define TEST_PCI_BAR3 0x1cU

struct pci_fixture {
	uint8_t config[256];
	unsigned fail_next_read;
	unsigned fail_next_write;
	unsigned fail_write_offset;
	unsigned fail_write_occurrence;
	unsigned matching_writes;
	unsigned mismatch_offset;
	unsigned mismatch_armed;
	unsigned mismatch_write_seen;
	unsigned mmio_reads;
};

static int irq_remove_result = HAL_OK;
static unsigned irq_mask_calls;

void *
hal_malloc(size_t size)
{
	return malloc(size);
}

void
hal_free(void *pointer)
{
	free(pointer);
}

int
hal_printf(const char *format, ...)
{
	(void)format;
	return 0;
}

void
hal_irq_mask(int irq)
{
	(void)irq;
	irq_mask_calls++;
}

void
hal_irq_unmask(int irq)
{
	(void)irq;
}

void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	(void)acknowledge;
}

void
hal_io_mb(void)
{
}

int
hal_irq_set_handler(int irq, hal_irq_handler_t handler, void *argument)
{
	(void)irq;
	(void)argument;
	return handler == NULL ? irq_remove_result : HAL_OK;
}

int
hal_irq_register_msi(const char *source, hal_irq_handler_t handler,
	void *argument, int *irq, paddr_t *address, uint32_t *event)
{
	(void)source;
	(void)handler;
	(void)argument;
	(void)irq;
	(void)address;
	(void)event;
	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_unregister_msi(int irq)
{
	(void)irq;
	return HAL_ERR_UNSUPPORTED;
}

static uint16_t
fixture_command(const struct pci_fixture *fixture)
{
	uint16_t command;

	memcpy(&command, fixture->config + TEST_PCI_COMMAND, sizeof(command));
	return command;
}

static void
fixture_put16(struct pci_fixture *fixture, unsigned offset, uint16_t value)
{
	memcpy(fixture->config + offset, &value, sizeof(value));
}

static void
fixture_put32(struct pci_fixture *fixture, unsigned offset, uint32_t value)
{
	memcpy(fixture->config + offset, &value, sizeof(value));
}

static uint32_t
fixture_get32(const struct pci_fixture *fixture, unsigned offset)
{
	uint32_t value;

	memcpy(&value, fixture->config + offset, sizeof(value));
	return value;
}

static void
fixture_clear_faults(struct pci_fixture *fixture)
{
	fixture->fail_next_read = 0;
	fixture->fail_next_write = 0;
	fixture->fail_write_offset = 0;
	fixture->fail_write_occurrence = 0;
	fixture->matching_writes = 0;
	fixture->mismatch_offset = 0;
	fixture->mismatch_armed = 0;
	fixture->mismatch_write_seen = 0;
}

static int
fixture_config_read(void *context, const struct drv_pci_address *address,
	unsigned offset, unsigned width, uint32_t *result)
{
	struct pci_fixture *fixture = context;

	if (fixture->fail_next_read != 0) {
		fixture->fail_next_read--;
		return EIO;
	}
	if (address->device != 2U || address->function != 0U ||
	    offset > sizeof(fixture->config) ||
	    width > sizeof(fixture->config) - offset) {
		*result = UINT32_MAX;
		return 0;
	}
	*result = 0;
	memcpy(result, fixture->config + offset, width);
	if (fixture->mismatch_armed != 0 &&
	    fixture->mismatch_write_seen != 0 &&
	    offset == fixture->mismatch_offset && width == 4U) {
		*result ^= 0x1000U;
		fixture->mismatch_armed = 0;
	}
	return 0;
}

static int
fixture_config_write(void *context, const struct drv_pci_address *address,
	unsigned offset, unsigned width, uint32_t value)
{
	struct pci_fixture *fixture = context;

	if (fixture->fail_next_write != 0) {
		fixture->fail_next_write--;
		return EIO;
	}
	if (fixture->fail_write_occurrence != 0 &&
	    offset == fixture->fail_write_offset &&
	    ++fixture->matching_writes == fixture->fail_write_occurrence)
		return EIO;
	if (address->device != 2U || address->function != 0U ||
	    offset > sizeof(fixture->config) ||
	    width > sizeof(fixture->config) - offset)
		return EINVAL;
	memcpy(fixture->config + offset, &value, width);
	if (fixture->mismatch_armed != 0 &&
	    offset == fixture->mismatch_offset && width == 4U)
		fixture->mismatch_write_seen = 1;
	return 0;
}

static uint8_t
fixture_mmio_read8(struct pci_fixture *fixture)
{
	uint16_t command = fixture_command(fixture);

	assert((command & TEST_PCI_COMMAND_MEMORY) != 0);
	assert((command & TEST_PCI_COMMAND_MASTER) == 0);
	fixture->mmio_reads++;
	return 0x80U;
}

static struct drv_xhci_capability_snapshot
valid_snapshot(uint16_t version)
{
	struct drv_xhci_capability_snapshot snapshot = {
		.mapping_size = 0x10000U,
		.capability_length = 0x80U,
		.version = version,
		.structural_parameters1 = (8U << 24) | 64U,
		.structural_parameters2 = 0,
		.capability_parameters1 = 0,
		.doorbell_offset_raw = 0x3000U,
		.runtime_offset_raw = 0x2000U,
	};

	return snapshot;
}

static void
test_mmio_validity(void)
{
	assert(drv_xhci_mmio32_valid(0));
	assert(drv_xhci_mmio32_valid(1));
	assert(!drv_xhci_mmio32_valid(UINT32_MAX));
}

static struct drv_xhci_capability_snapshot
extent_snapshot(void)
{
	struct drv_xhci_capability_snapshot snapshot = {
		.mapping_size = 0x500U,
		.capability_length = 0x40U,
		.version = 0x110U,
		.structural_parameters1 = (1U << 24) | 3U,
		.structural_parameters2 = 0,
		.capability_parameters1 = 0,
		.doorbell_offset_raw = 0xc0U,
		.runtime_offset_raw = 0x80U,
	};

	return snapshot;
}

static void
test_capability_validation(void)
{
	struct drv_xhci_capability_snapshot snapshot;
	uint32_t reasons;

	assert(drv_xhci_capability_validate(NULL) ==
	    DRV_XHCI_CAP_MAPPING_SHORT);
	for (uint16_t version = 0x100U; version <= 0x120U;
	     version += 0x10U) {
		snapshot = valid_snapshot(version);
		assert(drv_xhci_capability_validate(&snapshot) == 0);
	}

	snapshot = valid_snapshot(0x090U);
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_VERSION);
	snapshot.version = 0x130U;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_VERSION);
	assert(strcmp(drv_xhci_capability_reason_name(DRV_XHCI_CAP_VERSION),
	    "version") == 0);

	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.mapping_size = 0x10000U;
	reasons = drv_xhci_capability_validate(&snapshot);
	assert((reasons & DRV_XHCI_CAP_ALL_ZERO) != 0);
	assert(strcmp(drv_xhci_capability_reason_name(reasons), "all-zero") ==
	    0);

	snapshot = valid_snapshot(0x110U);
	snapshot.capability_length = UINT8_MAX;
	snapshot.version = UINT16_MAX;
	snapshot.structural_parameters1 = UINT32_MAX;
	snapshot.structural_parameters2 = UINT32_MAX;
	snapshot.capability_parameters1 = UINT32_MAX;
	snapshot.doorbell_offset_raw = UINT32_MAX;
	snapshot.runtime_offset_raw = UINT32_MAX;
	reasons = drv_xhci_capability_validate(&snapshot);
	assert((reasons & DRV_XHCI_CAP_ALL_ONE) != 0);
	assert(strcmp(drv_xhci_capability_reason_name(reasons), "all-one") ==
	    0);

	snapshot = valid_snapshot(0x110U);
	snapshot.mapping_size = 0x1bU;
	reasons = drv_xhci_capability_validate(&snapshot);
	assert((reasons & DRV_XHCI_CAP_MAPPING_SHORT) != 0);
	assert(strcmp(drv_xhci_capability_reason_name(reasons),
	    "mapping-short") == 0);

	snapshot = valid_snapshot(0x110U);
	snapshot.capability_length = 0x1cU;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_LENGTH);
	snapshot.capability_length = 0x22U;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_LENGTH);

	snapshot = valid_snapshot(0x110U);
	snapshot.structural_parameters1 &= ~0xffU;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_SLOTS);
	snapshot = valid_snapshot(0x110U);
	snapshot.structural_parameters1 &= 0x00ffffffU;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_PORTS);

	snapshot = extent_snapshot();
	snapshot.structural_parameters1 = (2U << 24) | 3U;
	snapshot.mapping_size = 0x454U;
	assert(drv_xhci_capability_validate(&snapshot) == 0);
	snapshot.mapping_size--;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_PORT_EXTENT);

	snapshot = extent_snapshot();
	snapshot.runtime_offset_raw = 0x4dfU;
	assert(drv_xhci_capability_validate(&snapshot) == 0);
	snapshot.mapping_size--;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_RUNTIME_EXTENT);
	snapshot = extent_snapshot();
	snapshot.runtime_offset_raw = 0x20U;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_RUNTIME_OFFSET);
	snapshot.runtime_offset_raw = 0x500U;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_RUNTIME_OFFSET);

	snapshot = extent_snapshot();
	snapshot.doorbell_offset_raw = 0x4f3U;
	assert(drv_xhci_capability_validate(&snapshot) == 0);
	snapshot.mapping_size--;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_DOORBELL_EXTENT);
	snapshot = extent_snapshot();
	snapshot.doorbell_offset_raw = 0x20U;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_DOORBELL_OFFSET);
	snapshot.doorbell_offset_raw = 0x500U;
	assert(drv_xhci_capability_validate(&snapshot) ==
	    DRV_XHCI_CAP_DOORBELL_OFFSET);

	assert(drv_xhci_region_fits(SIZE_MAX, SIZE_MAX, 0));
	assert(!drv_xhci_region_fits(SIZE_MAX, SIZE_MAX, 1));
	assert(strcmp(drv_xhci_capability_reason_name(0), "ok") == 0);
}

static void
test_scratchpad_and_extended_capabilities(void)
{
	uint32_t disabled, original, restored;
	unsigned next = 0;

	assert(drv_xhci_scratchpad_count(0) == 0);
	assert(drv_xhci_scratchpad_count((3U << 21) | (1U << 27)) ==
	    97U);
	assert(drv_xhci_scratchpad_count(0x00200000U) == 32U);
	assert(drv_xhci_scratchpad_count(0x08000000U) == 1U);
	assert(drv_xhci_scratchpad_count(0x5a400000U) == 587U);
	assert(drv_xhci_scratchpad_count(0xfbe00000U) == 1023U);
	assert(drv_xhci_scratchpad_count(0x04000000U) == 0U);
	assert(drv_xhci_extended_capability_next(0x54U, 0x40U,
	    4U << 8, &next) == 1);
	assert(next == 0x50U);
	assert(drv_xhci_extended_capability_next(0x54U, 0x50U, 0,
	    &next) == 0);
	assert(drv_xhci_extended_capability_next(0x57U, 0x50U,
	    1U << 8, &next) == -1);
	assert(drv_xhci_extended_capability_next(0x54U, 0x51U, 0,
	    &next) == -1);
	assert(drv_xhci_extended_capability_next(SIZE_MAX,
	    UINT32_MAX - 3U, 1U << 8, &next) == -1);
	assert(drv_xhci_extended_capability_next(0x54U, 0x40U, 0, NULL) ==
	    -1);

	assert(drv_xhci_legacy_ownership_ready(0, 1));
	assert(!drv_xhci_legacy_ownership_ready(1, 1));
	assert(!drv_xhci_legacy_ownership_ready(0, 0));
	assert(!drv_xhci_legacy_ownership_ready(1, 0));
	original = UINT32_MAX;
	disabled = drv_xhci_legacy_control_disable(original);
	assert((disabled & DRV_XHCI_LEGACY_SMI_ENABLE) == 0);
	assert((disabled & DRV_XHCI_LEGACY_RESERVED_ZERO) == 0);
	assert((disabled & DRV_XHCI_LEGACY_SMI_STATUS) ==
	    DRV_XHCI_LEGACY_SMI_STATUS);
	restored = drv_xhci_legacy_control_restore(disabled, original);
	assert((restored & DRV_XHCI_LEGACY_SMI_ENABLE) ==
	    (original & DRV_XHCI_LEGACY_SMI_ENABLE));
	assert((restored & DRV_XHCI_LEGACY_RESERVED_ZERO) == 0);
	assert((restored & DRV_XHCI_LEGACY_SMI_STATUS) ==
	    DRV_XHCI_LEGACY_SMI_STATUS);
}

static void
test_bar_readback(void)
{
	const uint64_t address64 = UINT64_C(0x0000123456789000);

	assert(drv_xhci_bar_readback_matches(0xf0800000U, 0,
	    0xf0800000U, 0));
	assert(drv_xhci_bar_readback_matches(0xf0800000U, 0,
	    0xf0800008U, 0));
	assert(!drv_xhci_bar_readback_matches(0xf0800000U, 0,
	    0xf0810000U, 0));
	assert(!drv_xhci_bar_readback_matches(0xf0800000U, 0,
	    0xf0800001U, 0));
	assert(!drv_xhci_bar_readback_matches(0xf0800000U, 0,
	    0xf0800004U, 0));
	assert(drv_xhci_bar_readback_matches(address64, 1, 0x56789004U,
	    0x00001234U));
	assert(drv_xhci_bar_readback_matches(address64, 1, 0x5678900cU,
	    0x00001234U));
	assert(!drv_xhci_bar_readback_matches(address64, 1, 0x56789000U,
	    0x00001234U));
	assert(!drv_xhci_bar_readback_matches(address64, 1, 0x56789004U,
	    0x00001235U));
}

static void
test_pci_enable_state(struct drv_pci_device *device,
	struct pci_fixture *fixture)
{
	struct drv_pci_enable_state state = { { 0, 0 } };
	uint16_t command;
	unsigned reads;

	fixture_put16(fixture, TEST_PCI_COMMAND,
	    TEST_PCI_COMMAND_IO | TEST_PCI_COMMAND_INTX_DISABLE);
	assert(drv_pci_device_save_enable_state(device, &state) == 0);
	assert(drv_pci_device_save_enable_state(device, &state) == EBUSY);
	assert(drv_pci_device_enable_memory(device) == 0);
	assert(fixture_mmio_read8(fixture) == 0x80U);
	assert(drv_pci_device_set_bus_master(device, true) == 0);
	command = fixture_command(fixture);
	fixture_put16(fixture, TEST_PCI_COMMAND,
	    command | TEST_PCI_COMMAND_SERR);
	assert(drv_pci_device_restore_enable_state(device, &state) == 0);
	assert(fixture_command(fixture) == (TEST_PCI_COMMAND_IO |
	    TEST_PCI_COMMAND_SERR | TEST_PCI_COMMAND_INTX_DISABLE));
	assert(drv_pci_device_restore_enable_state(device, &state) == 0);

	fixture_put16(fixture, TEST_PCI_COMMAND,
	    TEST_PCI_COMMAND_IO | TEST_PCI_COMMAND_MEMORY |
	    TEST_PCI_COMMAND_MASTER);
	assert(drv_pci_device_save_enable_state(device, &state) == 0);
	fixture_put16(fixture, TEST_PCI_COMMAND, 0);
	assert(drv_pci_device_restore_enable_state(device, &state) == 0);
	assert(fixture_command(fixture) == (TEST_PCI_COMMAND_IO |
	    TEST_PCI_COMMAND_MEMORY | TEST_PCI_COMMAND_MASTER));

	fixture_put16(fixture, TEST_PCI_COMMAND, TEST_PCI_COMMAND_IO);
	assert(drv_pci_device_save_enable_state(device, &state) == 0);
	assert(drv_pci_device_enable_memory(device) == 0);
	fixture->fail_next_write = 1;
	assert(drv_pci_device_restore_enable_state(device, &state) == EIO);
	assert((fixture_command(fixture) & TEST_PCI_COMMAND_MEMORY) != 0);
	assert(drv_pci_device_restore_enable_state(device, &state) == 0);
	assert(fixture_command(fixture) == TEST_PCI_COMMAND_IO);

	memset(&state, 0, sizeof(state));
	fixture->fail_next_read = 1;
	assert(drv_pci_device_save_enable_state(device, &state) == EIO);
	assert(drv_pci_device_restore_enable_state(device, &state) == 0);

	fixture_put16(fixture, TEST_PCI_COMMAND, TEST_PCI_COMMAND_IO);
	reads = fixture->mmio_reads;
	fixture->fail_next_write = 1;
	if (drv_pci_device_enable_memory(device) == 0)
		(void)fixture_mmio_read8(fixture);
	assert(fixture->mmio_reads == reads);
	assert(fixture_command(fixture) == TEST_PCI_COMMAND_IO);

	assert(drv_pci_device_save_enable_state(NULL, &state) == EINVAL);
	assert(drv_pci_device_save_enable_state(device, NULL) == EINVAL);
	assert(drv_pci_device_restore_enable_state(NULL, &state) == EINVAL);
	assert(drv_pci_device_restore_enable_state(device, NULL) == EINVAL);
}

static void
assert_bar_address(struct drv_pci_device *device, unsigned index,
	enum drv_pci_bar_type type, uint64_t address)
{
	struct drv_pci_bar bar;

	assert(drv_pci_device_bar(device, index, &bar) == 0);
	assert(bar.type == type);
	assert(bar.bus_address == address);
}

static void
test_pci_bar_assignment(struct drv_pci_device *device,
	struct pci_fixture *fixture)
{
	const uint16_t original_command = TEST_PCI_COMMAND_IO |
	    TEST_PCI_COMMAND_MEMORY | TEST_PCI_COMMAND_MASTER |
	    TEST_PCI_COMMAND_INTX_DISABLE;
	const uint64_t original64 = UINT64_C(0x0000123456789000);
	const uint64_t assigned64 = UINT64_C(0x000022346789a000);
	const uint64_t failed64 = UINT64_C(0x00003334789ab000);
	const uint32_t assigned32 = 0xf0810000U;
	uint32_t low32, low64, high64;

	fixture_clear_faults(fixture);
	fixture_put16(fixture, TEST_PCI_COMMAND, original_command);
	assert_bar_address(device, 0, DRV_PCI_BAR_MEMORY32, 0xf0800000U);
	assert_bar_address(device, 2, DRV_PCI_BAR_MEMORY64, original64);

	assert(drv_pci_device_assign_bar(device, 0, assigned32) == 0);
	assert(fixture_get32(fixture, TEST_PCI_BAR0) == assigned32);
	assert(fixture_command(fixture) == original_command);
	assert_bar_address(device, 0, DRV_PCI_BAR_MEMORY32, assigned32);

	assert(drv_pci_device_assign_bar(device, 2, assigned64) == 0);
	low64 = (uint32_t)assigned64 | 4U;
	high64 = (uint32_t)(assigned64 >> 32);
	assert(fixture_get32(fixture, TEST_PCI_BAR2) == low64);
	assert(fixture_get32(fixture, TEST_PCI_BAR3) == high64);
	assert(fixture_command(fixture) == original_command);
	assert_bar_address(device, 2, DRV_PCI_BAR_MEMORY64, assigned64);

	/* A failed decode-disable write must not touch either BAR or cache. */
	fixture_clear_faults(fixture);
	fixture_put16(fixture, TEST_PCI_COMMAND, original_command);
	low32 = fixture_get32(fixture, TEST_PCI_BAR0);
	fixture->fail_write_offset = TEST_PCI_COMMAND;
	fixture->fail_write_occurrence = 1;
	assert(drv_pci_device_assign_bar(device, 0, 0xf0820000U) != 0);
	assert(fixture_get32(fixture, TEST_PCI_BAR0) == low32);
	assert(fixture_command(fixture) == original_command);
	assert_bar_address(device, 0, DRV_PCI_BAR_MEMORY32, assigned32);

	/* A failed low DWORD must roll the already-written high DWORD back. */
	fixture_clear_faults(fixture);
	fixture_put16(fixture, TEST_PCI_COMMAND, original_command);
	low64 = fixture_get32(fixture, TEST_PCI_BAR2);
	high64 = fixture_get32(fixture, TEST_PCI_BAR3);
	fixture->fail_write_offset = TEST_PCI_BAR2;
	fixture->fail_write_occurrence = 1;
	assert(drv_pci_device_assign_bar(device, 2, failed64) != 0);
	assert(fixture_get32(fixture, TEST_PCI_BAR2) == low64);
	assert(fixture_get32(fixture, TEST_PCI_BAR3) == high64);
	assert((fixture_command(fixture) & (TEST_PCI_COMMAND_IO |
	    TEST_PCI_COMMAND_MEMORY | TEST_PCI_COMMAND_MASTER)) == 0);
	assert_bar_address(device, 2, DRV_PCI_BAR_MEMORY64, assigned64);

	/* A mismatched readback is a failed transaction, not a new BAR. */
	fixture_clear_faults(fixture);
	fixture_put16(fixture, TEST_PCI_COMMAND, original_command);
	low32 = fixture_get32(fixture, TEST_PCI_BAR0);
	fixture->mismatch_offset = TEST_PCI_BAR0;
	fixture->mismatch_armed = 1;
	assert(drv_pci_device_assign_bar(device, 0, 0xf0830000U) != 0);
	assert(fixture_get32(fixture, TEST_PCI_BAR0) == low32);
	assert((fixture_command(fixture) & (TEST_PCI_COMMAND_IO |
	    TEST_PCI_COMMAND_MEMORY | TEST_PCI_COMMAND_MASTER)) == 0);
	assert_bar_address(device, 0, DRV_PCI_BAR_MEMORY32, assigned32);

	/* If COMMAND restoration fails, roll the BAR back and fail closed. */
	fixture_clear_faults(fixture);
	fixture_put16(fixture, TEST_PCI_COMMAND, original_command);
	low32 = fixture_get32(fixture, TEST_PCI_BAR0);
	fixture->fail_write_offset = TEST_PCI_COMMAND;
	fixture->fail_write_occurrence = 2;
	assert(drv_pci_device_assign_bar(device, 0, 0xf0840000U) != 0);
	assert(fixture_get32(fixture, TEST_PCI_BAR0) == low32);
	assert((fixture_command(fixture) & (TEST_PCI_COMMAND_IO |
	    TEST_PCI_COMMAND_MEMORY | TEST_PCI_COMMAND_MASTER)) == 0);
	assert_bar_address(device, 0, DRV_PCI_BAR_MEMORY32, assigned32);
	fixture_clear_faults(fixture);
}

static int
test_irq_handler(void *argument)
{
	(void)argument;
	return 1;
}

static void
test_checked_irq_disestablish(struct drv_pci_device *device)
{
	struct drv_pci_irq irq = {
		.type = DRV_PCI_IRQ_INTX,
		.vector = 10U,
	};
	void *cookie = NULL;
	unsigned masks_before = irq_mask_calls;

	assert(drv_pci_device_establish_irq(device, &irq, test_irq_handler,
	    NULL, "checked-remove", &cookie) == 0);
	assert(cookie != NULL);
	irq_remove_result = HAL_ERR_BUSY;
	assert(drv_pci_device_disestablish_irq_checked(device, cookie) ==
	    EBUSY);
	assert(irq_mask_calls == masks_before + 1U);
	irq_remove_result = HAL_OK;
	assert(drv_pci_device_disestablish_irq_checked(device, cookie) == 0);
	assert(irq_mask_calls == masks_before + 2U);
}

int
main(void)
{
	static const struct drv_pci_bus_ops operations = {
		.config_read = fixture_config_read,
		.config_write = fixture_config_write,
	};
	struct drv_pci_address address = { 0, 0, 2, 0 };
	struct pci_fixture fixture;
	struct drv_pci_device *device;
	struct drv_pci_bus *bus;

	test_capability_validation();
	test_mmio_validity();
	test_scratchpad_and_extended_capabilities();
	test_bar_readback();

	memset(&fixture, 0, sizeof(fixture));
	fixture_put32(&fixture, 0x00U, 0x56781234U);
	fixture_put32(&fixture, 0x08U, 0x0c033001U);
	fixture_put32(&fixture, TEST_PCI_BAR0, 0xf0800000U);
	fixture_put32(&fixture, TEST_PCI_BAR2, 0x56789004U);
	fixture_put32(&fixture, TEST_PCI_BAR3, 0x00001234U);
	fixture_put16(&fixture, TEST_PCI_COMMAND,
	    TEST_PCI_COMMAND_IO | TEST_PCI_COMMAND_INTX_DISABLE);
	assert(drv_pci_init() == 0);
	assert(drv_pci_bus_create_root(0, 0, &operations, &fixture, NULL,
	    &bus) == 0);
	assert(drv_pci_bus_scan(bus) == 0);
	device = drv_pci_find_device(&address);
	assert(device != NULL);
	test_pci_enable_state(device, &fixture);
	test_pci_bar_assignment(device, &fixture);
	test_checked_irq_disestablish(device);

	puts("xHCI capability/MMIO test: PASS");
	return 0;
}
