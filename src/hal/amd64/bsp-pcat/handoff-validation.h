/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pure ZBL6 version, size, and flag classification shared with host fixtures.
 */

#ifndef KERN_HAL_AMD64_PCAT_HANDOFF_VALIDATION_H
#define KERN_HAL_AMD64_PCAT_HANDOFF_VALIDATION_H

#include <stdint.h>
#include "bootloader/include/amd64-handoff.h"

/* Any home the loaders may choose for the image (amd64-kernel-image.h). */
int zbl6_kernel_placement_valid(uint64_t kernel_phys_start, uint64_t kernel_phys_end);

/* Validates pointed-to copies after the envelope's low-address bounds check. */
int zbl6_memory_envelope_valid(const struct zbl6_memory_handoff *memory, uint32_t source);
int zbl6_memory_contents_valid(const struct zbl6_memory_handoff *memory,
    const struct zbl6_memory_range_v6 *ranges,
    const struct zbl6_boot_allocation *allocations);

enum zbl6_handoff_form {
	ZBL6_HANDOFF_FORM_INVALID = 0,
	ZBL6_HANDOFF_FORM_LEGACY_BIOS,
	ZBL6_HANDOFF_FORM_LEGACY_UEFI,
	ZBL6_HANDOFF_FORM_V5_BIOS,
	ZBL6_HANDOFF_FORM_V5_UEFI,
	ZBL6_HANDOFF_FORM_V6_BIOS,
	ZBL6_HANDOFF_FORM_V6_UEFI,
	ZBL6_HANDOFF_FORM_V7_UEFI,
	ZBL6_HANDOFF_FORM_V8_UEFI
};

/*
 * Validates the runtime part of a version-eight handoff: where its copy of
 * the runtime descriptors lies, before the HAL copies it, and then the
 * copied descriptors themselves.
 */
int zbl6_uefi_runtime_envelope_valid(const struct zbl6_uefi_runtime_handoff *runtime, const struct zbl6_memory_handoff *memory, const struct zbl6_boot_allocation *allocations);
int zbl6_uefi_runtime_descriptors_valid(const struct zbl6_uefi_runtime_handoff *runtime, const uint8_t *descriptors);

enum zbl6_handoff_form
zbl6_handoff_classify(
	uint16_t version,
	uint16_t size,
	uint32_t flags);

enum zbl6_handoff_form
zbl6_handoff_classify_raw(
	const void *raw_handoff);

int
zbl6_uefi_partition_handoff_valid(
	uint16_t version,
	uint8_t scheme,
	uint8_t root_partition_index,
	uint8_t loader_partition_index,
	uint32_t flags);

#endif
