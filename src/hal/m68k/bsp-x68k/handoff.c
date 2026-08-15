/* Pure validation for the X68000 loader-to-kernel handoff. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "bsp.h"
#include <stddef.h>

int
x68k_boot_handoff_valid(const struct zedbsd_x68k_handoff *handoff)
{
	unsigned index;

	if (handoff == NULL || handoff->common.magic != ZEDBSD_HANDOFF_MAGIC ||
	    handoff->common.version != ZEDBSD_HANDOFF_VERSION_X68K ||
	    handoff->common.size != sizeof(*handoff) ||
	    handoff->common.boot_bios_id > 6U ||
	    handoff->common.boot_partition_scheme !=
	    ZEDBSD_PARTITION_SCHEME_X68K ||
	    handoff->common.boot_partition_index == 0 ||
	    handoff->common.boot_partition_lba ==
	    ZEDBSD_BOOT_PARTITION_LBA_UNKNOWN ||
	    handoff->extension_magic != ZEDBSD_X68K_HANDOFF_MAGIC ||
	    handoff->extension_version != ZEDBSD_X68K_HANDOFF_VERSION ||
	    handoff->extension_size != sizeof(*handoff) -
	    sizeof(handoff->common) || handoff->ram_bytes < 0x00200000U ||
	    handoff->ram_bytes > 0x00c00000U ||
	    (handoff->ram_bytes & 0x0fffU) != 0 ||
	    handoff->kernel_phys_start >= handoff->kernel_phys_end ||
	    handoff->kernel_phys_end > handoff->ram_bytes ||
	    handoff->loader_phys_start >= handoff->loader_phys_end ||
	    handoff->loader_phys_end > handoff->ram_bytes ||
	    handoff->memory_region_count == 0 ||
	    handoff->memory_region_count > ZEDBSD_X68K_MAX_MEMORY_REGIONS)
		return 0;
	for (index = 0; index < handoff->memory_region_count; index++) {
		const struct zedbsd_memory_region32 *region =
			&handoff->memory_regions[index];
		if (region->size == 0 ||
		    (region->type != ZEDBSD_MEMORY_AVAILABLE &&
		     region->type != ZEDBSD_MEMORY_RESERVED) ||
		    region->base > handoff->ram_bytes ||
		    region->size > handoff->ram_bytes - region->base)
			return 0;
	}
	return 1;
}
