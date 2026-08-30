/* Pure ZBL6 version/size/flag classification shared with host fixtures. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stddef.h>

#include "handoff-validation.h"
#include "bootloader/include/amd64-handoff.h"

enum zbl6_handoff_form
zbl6_handoff_classify(uint16_t version, uint16_t size, uint32_t flags)
{
	uint32_t required = ZBL6_HANDOFF_FLAG_UEFI |
	    ZBL6_HANDOFF_FLAG_MEMORY_MAP | ZBL6_HANDOFF_FLAG_ACPI_RSDP;

	if (version == ZBL6_HANDOFF_VERSION)
		return size >= ZBL6_HANDOFF_SIZE
		    ? ZBL6_HANDOFF_FORM_LEGACY_BIOS
		    : ZBL6_HANDOFF_FORM_INVALID;
	if (version == ZBL6_HANDOFF_V2_VERSION)
		return size >= ZBL6_HANDOFF_V2_SIZE &&
		       (flags & required) == required
		    ? ZBL6_HANDOFF_FORM_LEGACY_UEFI
		    : ZBL6_HANDOFF_FORM_INVALID;
	if (version == ZBL6_HANDOFF_V3_VERSION) {
		required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER;
		return size >= ZBL6_HANDOFF_V3_SIZE &&
		       (flags & required) == required
		    ? ZBL6_HANDOFF_FORM_LEGACY_UEFI
		    : ZBL6_HANDOFF_FORM_INVALID;
	}
	if (version == ZBL6_HANDOFF_V4_VERSION) {
		required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
		    ZBL6_HANDOFF_FLAG_BOOT_UUID;
		return size >= ZBL6_HANDOFF_V4_SIZE &&
		       (flags & required) == required
		    ? ZBL6_HANDOFF_FORM_LEGACY_UEFI
		    : ZBL6_HANDOFF_FORM_INVALID;
	}
	if (version != ZBL6_HANDOFF_V5_VERSION)
		return ZBL6_HANDOFF_FORM_INVALID;
	if (size == ZBL6_HANDOFF_V5_BIOS_SIZE) {
		uint32_t allowed = ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
		    ZBL6_HANDOFF_FLAG_BOOT_UUID |
		    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;

		return (flags & ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS) != 0U &&
		       (flags & ~allowed) == 0U
		    ? ZBL6_HANDOFF_FORM_V5_BIOS
		    : ZBL6_HANDOFF_FORM_INVALID;
	}
	required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
	    ZBL6_HANDOFF_FLAG_BOOT_UUID |
	    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;
	return size == ZBL6_HANDOFF_V5_UEFI_SIZE && flags == required
	    ? ZBL6_HANDOFF_FORM_V5_UEFI
	    : ZBL6_HANDOFF_FORM_INVALID;
}

enum zbl6_handoff_form
zbl6_handoff_classify_raw(const void *raw_handoff)
{
	const struct zbl6_handoff *bios = raw_handoff;
	const struct zbl6_handoff_v2 *uefi = raw_handoff;
	uint32_t flags;

	if (bios == NULL || bios->magic != ZBL6_HANDOFF_MAGIC)
		return ZBL6_HANDOFF_FORM_INVALID;
	flags = bios->version == ZBL6_HANDOFF_VERSION ||
		(bios->version == ZBL6_HANDOFF_V5_VERSION &&
		 bios->size == ZBL6_HANDOFF_V5_BIOS_SIZE)
	    ? bios->flags
	    : uefi->flags;
	return zbl6_handoff_classify(bios->version, bios->size, flags);
}

int
zbl6_uefi_partition_handoff_valid(uint16_t version, uint8_t scheme,
	uint8_t root_partition_index, uint8_t loader_partition_index,
	uint32_t flags)
{
	if ((flags & ZBL6_HANDOFF_FLAG_UEFI) == 0U)
		return 0;
	if (version >= ZBL6_HANDOFF_V2_VERSION &&
	    version <= ZBL6_HANDOFF_V4_VERSION)
		return scheme == ZBL6_PARTITION_SCHEME_MBR &&
		    root_partition_index >= 1U &&
		    root_partition_index <= 4U && loader_partition_index == 2U;
	if (version != ZBL6_HANDOFF_V5_VERSION ||
	    (flags & ZBL6_HANDOFF_FLAG_BOOT_UUID) == 0U)
		return 0;
	/* V5 carries only the selected FAT's actual partition-table style.  Its
	 * UUID, not either historically overloaded ordinal, is authoritative. */
	if (scheme == ZBL6_PARTITION_SCHEME_MBR ||
	    scheme == ZBL6_PARTITION_SCHEME_GPT)
		return root_partition_index == ZBL6_PARTITION_INDEX_UNKNOWN &&
		    loader_partition_index == ZBL6_PARTITION_INDEX_UNKNOWN &&
		    (flags & ZBL6_HANDOFF_FLAG_BOOT_UUID) != 0U;
	return 0;
}
