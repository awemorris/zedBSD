/* X68k boot-handoff validator tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdio.h>
#include <string.h>
#include "src/hal/m68k/bsp-x68k/bsp.h"

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; \
} } while (0)

static struct x68k_boot_handoff
valid_handoff(void)
{
	struct x68k_boot_handoff h;
	memset(&h, 0, sizeof(h));
	h.common.magic = ZEDBSD_HANDOFF_MAGIC;
	h.common.version = ZEDBSD_HANDOFF_VERSION_X68K;
	h.common.size = sizeof(h);
	h.common.boot_bios_id = 3;
	h.common.boot_partition_scheme = ZEDBSD_PARTITION_SCHEME_X68K;
	h.common.boot_partition_index = 1;
	h.common.boot_partition_lba = 2048;
	h.extension_magic = ZEDBSD_X68K_HANDOFF_MAGIC;
	h.extension_version = ZEDBSD_X68K_HANDOFF_VERSION;
	h.extension_size = sizeof(h) - sizeof(h.common);
	h.ram_bytes = 0x00c00000U;
	h.kernel_phys_start = 0x00010000U;
	h.kernel_phys_end = 0x00120000U;
	h.loader_phys_start = 0x00002000U;
	h.loader_phys_end = 0x00010000U;
	h.memory_region_count = 1;
	h.memory_regions[0].size = h.ram_bytes;
	h.memory_regions[0].type = ZEDBSD_MEMORY_AVAILABLE;
	return h;
}

int
main(void)
{
	struct x68k_boot_handoff h = valid_handoff();
	CHECK(x68k_boot_handoff_valid(&h));
	h.loader_phys_end = h.loader_phys_start - 1U;
	CHECK(!x68k_boot_handoff_valid(&h));
	h = valid_handoff();
	h.memory_regions[0].base = h.ram_bytes - 4096U;
	h.memory_regions[0].size = 8192U;
	CHECK(!x68k_boot_handoff_valid(&h));
	h = valid_handoff();
	h.common.boot_partition_scheme = ZEDBSD_PARTITION_SCHEME_MBR;
	CHECK(!x68k_boot_handoff_valid(&h));
	h = valid_handoff();
	h.extension_size--;
	CHECK(!x68k_boot_handoff_valid(&h));
	puts("X68k handoff host tests passed");
	return 0;
}
