/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check of the X68000 loader's handoff, kept free of hardware access
 * so the host tests can compile it.
 */

#include "bsp.h"
#include <stddef.h>

/* The SCSI IDs a boot disk may have: 0 to 6; 7 is the host's. */
#define HANDOFF_SCSI_ID_MAX	6U

/* The RAM the port runs in: 2 MiB to 12 MiB, in whole 4 KiB pages. */
#define HANDOFF_RAM_MIN		0x00200000U
#define HANDOFF_RAM_MAX		0x00c00000U
#define HANDOFF_PAGE_MASK	0x00000fffU

static int common_valid(const struct x68k_boot_handoff *handoff);
static int extension_valid(const struct x68k_boot_handoff *handoff);
static int regions_valid(const struct x68k_boot_handoff *handoff);

/*
 * Reports whether a loader's handoff is one the HAL can use.
 */
int
x68k_boot_handoff_valid(
	const struct x68k_boot_handoff *handoff)
{
	int valid;

	/* Refuses a missing record. */
	if (handoff == NULL)
		return 0;

	/* Refuses a common record that is not the loader's. */
	valid = common_valid(handoff);
	if (!valid)
		return 0;

	/* Refuses an X68000 part of another layout or with ranges outside RAM. */
	valid = extension_valid(handoff);
	if (!valid)
		return 0;

	/* Refuses a memory map with an empty or foreign region. */
	valid = regions_valid(handoff);
	if (!valid)
		return 0;

	/* The handoff is usable. */
	return 1;
}

/* Reports whether the common record is the loader's: its version, a SCSI boot disk and a partition. */
static int
common_valid(
	const struct x68k_boot_handoff *handoff)
{
	/* The record names its magic, the X68000 version and its full size. */
	if (handoff->common.magic != KERN_HANDOFF_MAGIC)
		return 0;
	if (handoff->common.version != KERN_HANDOFF_VERSION_X68K)
		return 0;
	if (handoff->common.size != sizeof(*handoff))
		return 0;

	/* The boot disk is a SCSI target with a partition of the X68000 scheme. */
	if (handoff->common.boot_bios_id > HANDOFF_SCSI_ID_MAX)
		return 0;
	if (handoff->common.boot_partition_scheme != KERN_PARTITION_SCHEME_X68K)
		return 0;
	if (handoff->common.boot_partition_index == 0)
		return 0;
	if (handoff->common.boot_partition_lba == KERN_BOOT_PARTITION_LBA_UNKNOWN)
		return 0;

	/* The common record is usable. */
	return 1;
}

/* Reports whether the X68000 part has this HAL's layout and ranges inside RAM. */
static int
extension_valid(
	const struct x68k_boot_handoff *handoff)
{
	/* The part names its magic, version and size. */
	if (handoff->extension_magic != X68K_HANDOFF_EXTENSION_MAGIC)
		return 0;
	if (handoff->extension_version != X68K_HANDOFF_EXTENSION_VERSION)
		return 0;
	if (handoff->extension_size != sizeof(*handoff) - sizeof(handoff->common))
		return 0;

	/* RAM is whole pages within what the port runs in. */
	if (handoff->ram_bytes < HANDOFF_RAM_MIN || handoff->ram_bytes > HANDOFF_RAM_MAX)
		return 0;
	if ((handoff->ram_bytes & HANDOFF_PAGE_MASK) != 0)
		return 0;

	/* The kernel and the loader are non-empty ranges inside RAM. */
	if (handoff->kernel_phys_start >= handoff->kernel_phys_end)
		return 0;
	if (handoff->kernel_phys_end > handoff->ram_bytes)
		return 0;
	if (handoff->loader_phys_start >= handoff->loader_phys_end)
		return 0;
	if (handoff->loader_phys_end > handoff->ram_bytes)
		return 0;

	/* The memory map has some regions, no more than the record holds. */
	if (handoff->memory_region_count == 0)
		return 0;
	if (handoff->memory_region_count > X68K_MAX_MEMORY_REGIONS)
		return 0;

	/* The part is usable. */
	return 1;
}

/* Reports whether every memory region is a non-empty available or reserved range inside RAM. */
static int
regions_valid(
	const struct x68k_boot_handoff *handoff)
{
	const struct x68k_memory_region *region;
	unsigned index;

	/* Checks each region the record names. */
	for (index = 0; index < handoff->memory_region_count; index++) {
		/* Refuses an empty region. */
		region = &handoff->memory_regions[index];
		if (region->size == 0)
			return 0;

		/* Refuses a region that is neither available nor reserved. */
		if (region->type != KERN_MEMORY_AVAILABLE && region->type != KERN_MEMORY_RESERVED)
			return 0;

		/* Refuses a region that leaves RAM. */
		if (region->base > handoff->ram_bytes)
			return 0;
		if (region->size > handoff->ram_bytes - region->base)
			return 0;
	}

	/* Every region is usable. */
	return 1;
}
