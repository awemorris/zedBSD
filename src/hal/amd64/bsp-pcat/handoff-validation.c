/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pure ZBL6 handoff version, size, and flag classification.
 */

#include <stddef.h>

#include "handoff-validation.h"
#include "bootloader/include/amd64-handoff.h"

/*
 * Classifies a decoded ZBL6 handoff envelope.
 */
enum zbl6_handoff_form
zbl6_handoff_classify(
	uint16_t version,
	uint16_t size,
	uint32_t flags)
{
	uint32_t required;
	uint32_t allowed;

	/* Starts with the services required by every legacy UEFI form. */
	required = ZBL6_HANDOFF_FLAG_UEFI |
	    ZBL6_HANDOFF_FLAG_MEMORY_MAP |
	    ZBL6_HANDOFF_FLAG_ACPI_RSDP;

	/* Accepts the original BIOS form only at its minimum size. */
	if (version == ZBL6_HANDOFF_VERSION) {
		/* Accepts an envelope large enough for the legacy BIOS fields. */
		if (size >= ZBL6_HANDOFF_SIZE)
			return ZBL6_HANDOFF_FORM_LEGACY_BIOS;

		/* Rejects a truncated original BIOS envelope. */
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Requires the original UEFI service flags for version two. */
	if (version == ZBL6_HANDOFF_V2_VERSION) {
		/* Accepts a complete envelope carrying every required service. */
		if (size >= ZBL6_HANDOFF_V2_SIZE &&
		    (flags & required) == required)
			return ZBL6_HANDOFF_FORM_LEGACY_UEFI;

		/* Rejects an incomplete version-two UEFI envelope. */
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Adds the framebuffer contract introduced by version three. */
	if (version == ZBL6_HANDOFF_V3_VERSION) {
		required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER;

		/* Accepts a complete envelope carrying every required service. */
		if (size >= ZBL6_HANDOFF_V3_SIZE &&
		    (flags & required) == required)
			return ZBL6_HANDOFF_FORM_LEGACY_UEFI;

		/* Rejects an incomplete version-three UEFI envelope. */
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Adds the boot-volume identity contract introduced by version four. */
	if (version == ZBL6_HANDOFF_V4_VERSION) {
		required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
		    ZBL6_HANDOFF_FLAG_BOOT_UUID;

		/* Accepts a complete envelope carrying every required service. */
		if (size >= ZBL6_HANDOFF_V4_SIZE &&
		    (flags & required) == required)
			return ZBL6_HANDOFF_FORM_LEGACY_UEFI;

		/* Rejects an incomplete version-four UEFI envelope. */
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Rejects versions outside the supported legacy and version-five set. */
	if (version != ZBL6_HANDOFF_V5_VERSION)
		return ZBL6_HANDOFF_FORM_INVALID;

	/* Recognizes the compact version-five BIOS envelope exactly. */
	if (size == ZBL6_HANDOFF_V5_BIOS_SIZE) {
		allowed = ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
		    ZBL6_HANDOFF_FLAG_BOOT_UUID |
		    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;

		/* Accepts only the mandatory parameter flag and allowed options. */
		if ((flags & ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS) != 0U &&
		    (flags & ~allowed) == 0U)
			return ZBL6_HANDOFF_FORM_V5_BIOS;

		/* Rejects a compact BIOS envelope with invalid flags. */
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Recognizes the exact version-five UEFI envelope and flag set. */
	required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
	    ZBL6_HANDOFF_FLAG_BOOT_UUID |
	    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;
	if (size == ZBL6_HANDOFF_V5_UEFI_SIZE && flags == required)
		return ZBL6_HANDOFF_FORM_V5_UEFI;

	/* Rejects every other version-five representation. */
	return ZBL6_HANDOFF_FORM_INVALID;
}

/*
 * Validates and classifies a raw ZBL6 handoff envelope.
 */
enum zbl6_handoff_form
zbl6_handoff_classify_raw(
	const void *raw_handoff)
{
	const struct zbl6_handoff *bios;
	const struct zbl6_handoff_v2 *uefi;
	enum zbl6_handoff_form form;
	uint32_t flags;

	/* Interprets the shared prefix through both defined envelope layouts. */
	bios = raw_handoff;
	uefi = raw_handoff;

	/* Requires a present handoff with the protocol magic. */
	if (bios == NULL || bios->magic != ZBL6_HANDOFF_MAGIC)
		return ZBL6_HANDOFF_FORM_INVALID;

	/* Reads flags at the offset selected by the encoded envelope form. */
	if (bios->version == ZBL6_HANDOFF_VERSION ||
	    (bios->version == ZBL6_HANDOFF_V5_VERSION &&
	    bios->size == ZBL6_HANDOFF_V5_BIOS_SIZE))
		flags = bios->flags;
	else
		flags = uefi->flags;

	/* Classifies the decoded version, size, and selected flags. */
	form = zbl6_handoff_classify(bios->version, bios->size, flags);

	/* Returns the decoded handoff form. */
	return form;
}

/*
 * Validates UEFI partition metadata for a classified handoff version.
 */
int
zbl6_uefi_partition_handoff_valid(
	uint16_t version,
	uint8_t scheme,
	uint8_t root_partition_index,
	uint8_t loader_partition_index,
	uint32_t flags)
{
	/* Requires every accepted envelope to identify UEFI boot. */
	if ((flags & ZBL6_HANDOFF_FLAG_UEFI) == 0U)
		return 0;

	/* Applies the historical MBR ordinal contract to versions two to four. */
	if (version >= ZBL6_HANDOFF_V2_VERSION &&
	    version <= ZBL6_HANDOFF_V4_VERSION) {
		/* Requires the historical MBR partition scheme. */
		if (scheme != ZBL6_PARTITION_SCHEME_MBR)
			return 0;

		/* Requires a one-based primary root partition. */
		if (root_partition_index < 1U || root_partition_index > 4U)
			return 0;

		/* Requires the historical loader partition ordinal. */
		if (loader_partition_index != 2U)
			return 0;

		/* Reports valid historical UEFI partition metadata. */
		return 1;
	}

	/* Requires version five and its authoritative boot-volume UUID. */
	if (version != ZBL6_HANDOFF_V5_VERSION ||
	    (flags & ZBL6_HANDOFF_FLAG_BOOT_UUID) == 0U)
		return 0;

	/*
	 * Version five carries only the selected FAT's actual partition-table
	 * style.  Its UUID, not either historically overloaded ordinal, is
	 * authoritative.
	 */
	if (scheme == ZBL6_PARTITION_SCHEME_MBR ||
	    scheme == ZBL6_PARTITION_SCHEME_GPT) {
		/* Requires unspecified ordinals and the authoritative UUID flag. */
		if (root_partition_index != ZBL6_PARTITION_INDEX_UNKNOWN ||
		    loader_partition_index != ZBL6_PARTITION_INDEX_UNKNOWN ||
		    (flags & ZBL6_HANDOFF_FLAG_BOOT_UUID) == 0U)
			return 0;

		/* Reports valid version-five partition metadata. */
		return 1;
	}

	/* Rejects every unsupported partition-table style. */
	return 0;
}
