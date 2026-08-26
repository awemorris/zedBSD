/*
 * PCI xHCI capability arithmetic
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_DRIVERS_PCI_XHCI_CAPABILITY_H
#define ZEDBSD_DRIVERS_PCI_XHCI_CAPABILITY_H

#include <stddef.h>
#include <stdint.h>

struct drv_xhci_capability_snapshot {
	size_t mapping_size;
	uint8_t capability_length;
	uint16_t version;
	uint32_t structural_parameters1;
	uint32_t structural_parameters2;
	uint32_t capability_parameters1;
	uint32_t doorbell_offset_raw;
	uint32_t runtime_offset_raw;
};

enum drv_xhci_capability_reason {
	DRV_XHCI_CAP_MAPPING_SHORT = 1U << 0,
	DRV_XHCI_CAP_ALL_ZERO = 1U << 1,
	DRV_XHCI_CAP_ALL_ONE = 1U << 2,
	DRV_XHCI_CAP_LENGTH = 1U << 3,
	DRV_XHCI_CAP_VERSION = 1U << 4,
	DRV_XHCI_CAP_SLOTS = 1U << 5,
	DRV_XHCI_CAP_PORTS = 1U << 6,
	DRV_XHCI_CAP_PORT_EXTENT = 1U << 7,
	DRV_XHCI_CAP_RUNTIME_OFFSET = 1U << 8,
	DRV_XHCI_CAP_RUNTIME_EXTENT = 1U << 9,
	DRV_XHCI_CAP_DOORBELL_OFFSET = 1U << 10,
	DRV_XHCI_CAP_DOORBELL_EXTENT = 1U << 11
};

#define DRV_XHCI_LEGACY_SMI_ENABLE 0x0000e011U
#define DRV_XHCI_LEGACY_SMI_STATUS 0xe0000000U
#define DRV_XHCI_LEGACY_RESERVED_ZERO 0x1fe00000U

static inline int
drv_xhci_region_fits(size_t extent, size_t offset, size_t length)
{
	return offset <= extent && length <= extent - offset;
}

/* An all-ones MMIO read means the controller mapping is not reachable. */
static inline int
drv_xhci_mmio32_valid(uint32_t value)
{
	return value != UINT32_MAX;
}

static inline unsigned
drv_xhci_scratchpad_count(uint32_t structural_parameters2)
{
	/* Max Scratchpad Buffers Hi is bits 25:21; Lo is bits 31:27. */
	return (((structural_parameters2 >> 21) & 31U) << 5) |
	       ((structural_parameters2 >> 27) & 31U);
}

static inline uint32_t
drv_xhci_capability_validate(
	const struct drv_xhci_capability_snapshot *snapshot)
{
	size_t port_offset, runtime_offset, doorbell_offset;
	unsigned slots, ports;
	uint32_t reasons = 0;

	if (snapshot == NULL)
		return DRV_XHCI_CAP_MAPPING_SHORT;
	if (snapshot->mapping_size < 0x1cU)
		reasons |= DRV_XHCI_CAP_MAPPING_SHORT;
	if (snapshot->capability_length == 0 && snapshot->version == 0 &&
	    snapshot->structural_parameters1 == 0 &&
	    snapshot->structural_parameters2 == 0 &&
	    snapshot->capability_parameters1 == 0 &&
	    snapshot->doorbell_offset_raw == 0 &&
	    snapshot->runtime_offset_raw == 0)
		reasons |= DRV_XHCI_CAP_ALL_ZERO;
	if (snapshot->capability_length == 0xffU &&
	    snapshot->version == 0xffffU &&
	    snapshot->structural_parameters1 == UINT32_MAX &&
	    snapshot->structural_parameters2 == UINT32_MAX &&
	    snapshot->capability_parameters1 == UINT32_MAX &&
	    snapshot->doorbell_offset_raw == UINT32_MAX &&
	    snapshot->runtime_offset_raw == UINT32_MAX)
		reasons |= DRV_XHCI_CAP_ALL_ONE;

	slots = snapshot->structural_parameters1 & 0xffU;
	ports = (snapshot->structural_parameters1 >> 24) & 0xffU;
	if (snapshot->capability_length < 0x20U ||
	    (snapshot->capability_length & 3U) != 0 ||
	    snapshot->capability_length > snapshot->mapping_size)
		reasons |= DRV_XHCI_CAP_LENGTH;
	if (snapshot->version != 0x100U && snapshot->version != 0x110U &&
	    snapshot->version != 0x120U)
		reasons |= DRV_XHCI_CAP_VERSION;
	if (slots == 0)
		reasons |= DRV_XHCI_CAP_SLOTS;
	if (ports == 0)
		reasons |= DRV_XHCI_CAP_PORTS;

	if (ports != 0) {
		port_offset = (size_t)snapshot->capability_length + 0x400U +
		    (size_t)(ports - 1U) * 0x10U;
		if (!drv_xhci_region_fits(snapshot->mapping_size, port_offset,
			4U))
			reasons |= DRV_XHCI_CAP_PORT_EXTENT;
	}
	runtime_offset = snapshot->runtime_offset_raw & ~(size_t)31U;
	if (runtime_offset < snapshot->capability_length ||
	    runtime_offset >= snapshot->mapping_size)
		reasons |= DRV_XHCI_CAP_RUNTIME_OFFSET;
	else if (!drv_xhci_region_fits(snapshot->mapping_size, runtime_offset,
		0x40U))
		reasons |= DRV_XHCI_CAP_RUNTIME_EXTENT;
	doorbell_offset = snapshot->doorbell_offset_raw & ~(size_t)3U;
	if (doorbell_offset < snapshot->capability_length ||
	    doorbell_offset >= snapshot->mapping_size)
		reasons |= DRV_XHCI_CAP_DOORBELL_OFFSET;
	else if (slots != 0 &&
	    !drv_xhci_region_fits(snapshot->mapping_size, doorbell_offset,
		(size_t)(slots + 1U) * 4U))
		reasons |= DRV_XHCI_CAP_DOORBELL_EXTENT;
	return reasons;
}

static inline const char *
drv_xhci_capability_reason_name(uint32_t reasons)
{
	if (reasons & DRV_XHCI_CAP_MAPPING_SHORT)
		return "mapping-short";
	if (reasons & DRV_XHCI_CAP_ALL_ZERO)
		return "all-zero";
	if (reasons & DRV_XHCI_CAP_ALL_ONE)
		return "all-one";
	if (reasons & DRV_XHCI_CAP_LENGTH)
		return "caplength";
	if (reasons & DRV_XHCI_CAP_VERSION)
		return "version";
	if (reasons & DRV_XHCI_CAP_SLOTS)
		return "slots";
	if (reasons & DRV_XHCI_CAP_PORTS)
		return "ports";
	if (reasons & DRV_XHCI_CAP_PORT_EXTENT)
		return "port-extent";
	if (reasons & DRV_XHCI_CAP_RUNTIME_OFFSET)
		return "runtime-offset";
	if (reasons & DRV_XHCI_CAP_RUNTIME_EXTENT)
		return "runtime-extent";
	if (reasons & DRV_XHCI_CAP_DOORBELL_OFFSET)
		return "doorbell-offset";
	if (reasons & DRV_XHCI_CAP_DOORBELL_EXTENT)
		return "doorbell-extent";
	return "ok";
}

/* The first xECP is base-relative.  Later Next fields are current-relative. */
static inline int
drv_xhci_extended_capability_next(size_t mapping_size,
	unsigned current_offset, uint32_t header, unsigned *next_offset)
{
	unsigned delta = ((header >> 8) & 0xffU) * 4U;
	size_t next;

	if (next_offset == NULL || (current_offset & 3U) != 0 ||
	    !drv_xhci_region_fits(mapping_size, current_offset, 4U))
		return -1;
	if (delta == 0)
		return 0;
	next = (size_t)current_offset + delta;
	if (next > UINT32_MAX || next <= current_offset ||
	    !drv_xhci_region_fits(mapping_size, next, 4U))
		return -1;
	*next_offset = (unsigned)next;
	return 1;
}

static inline int
drv_xhci_legacy_ownership_ready(uint8_t bios_owned, uint8_t os_owned)
{
	return (bios_owned & 1U) == 0 && (os_owned & 1U) != 0;
}

static inline uint32_t
drv_xhci_legacy_control_disable(uint32_t control)
{
	return ((control & ~DRV_XHCI_LEGACY_SMI_ENABLE) &
	    ~DRV_XHCI_LEGACY_RESERVED_ZERO) |
	    DRV_XHCI_LEGACY_SMI_STATUS;
}

static inline uint32_t
drv_xhci_legacy_control_restore(uint32_t control, uint32_t original)
{
	return (control & ~DRV_XHCI_LEGACY_SMI_ENABLE) |
	    (original & DRV_XHCI_LEGACY_SMI_ENABLE);
}

static inline int
drv_xhci_bar_readback_matches(uint64_t expected, int memory64,
	uint32_t low, uint32_t high)
{
	uint64_t address;

	if ((low & 1U) != 0)
		return 0;
	if (memory64) {
		if ((low & 6U) != 4U)
			return 0;
		address = ((uint64_t)high << 32) | (low & ~15U);
	} else {
		if ((low & 6U) != 0)
			return 0;
		address = low & ~15U;
	}
	return address == expected;
}

#endif
