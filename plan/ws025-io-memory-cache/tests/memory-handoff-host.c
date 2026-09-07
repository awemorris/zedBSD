/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bootloader/bios/memory-map.h"
#include "bootloader/uefi/memory-map.h"
#include "src/hal/amd64/bsp-pcat/handoff-validation.h"

int main(void)
{
	struct zbl_e820_entry bios[3] = {
		{0x100000000ULL, 0x4000, 1, 1},
		{0x100001000ULL, 0x1000, 5, 1},
		{0x100003000ULL, 0x1000, 1, 0}
	};
	EFI_MEMORY_DESCRIPTOR efi[2];
	struct zbl6_memory_range_v6 ranges[8];
	struct zbl6_boot_allocation allocations[3] = {
		{0x200000, 0x200000, ZBL6_BOOT_OWNER_KERNEL, ZBL6_BOOT_KEEP},
		{0x50000, 0x6000, ZBL6_BOOT_OWNER_BOOTSTRAP, ZBL6_BOOT_AFTER_INIT},
		{0x10000, 0x10000, ZBL6_BOOT_OWNER_LOADER, ZBL6_BOOT_AFTER_INIT}
	};
	struct zbl6_memory_handoff memory = {
		ZBL6_MEMORY_SOURCE_BIOS_E820, ZBL6_MEMORY_MAP_COMPLETE,
		1, ZBL6_MEMORY_RANGE_V6_SIZE, 0x11000,
		3, ZBL6_BOOT_ALLOCATION_SIZE, 0x13000, 0x50000,
		0x200000, 0x400000
	};
	struct zbl6_handoff_v6_bios handoff;
	uint32_t count, flags;
	struct zbl_e820_state state = {0, 0};

	assert(zbl_bios_e820_accept(&state, bios, 3, 1, 0, 0, 0) == ZBL_E820_LEGACY);
	assert(state.count == 0);
	assert(zbl_bios_e820_accept(&state, bios, 3, 0, 0, 20, 17) == ZBL_E820_INVALID);
	assert(zbl_bios_e820_accept(&state, bios, 3, 0, 0x534d4150, 21, 17) == ZBL_E820_INVALID);
	assert(zbl_bios_e820_accept(&state, bios, 3, 0, 0x534d4150, 20, 17) == ZBL_E820_CONTINUE);
	assert(state.count == 1 && state.token == 17 && bios[0].attributes == 1);
	assert(zbl_bios_e820_accept(&state, bios, 3, 0, 0x534d4150, 24, 17) == ZBL_E820_INVALID);
	assert(state.count == 1);
	/* Opaque tokens may decrease; only lack of progress is rejected. */
	assert(zbl_bios_e820_accept(&state, bios, 3, 0, 0x534d4150, 24, 3) == ZBL_E820_CONTINUE);
	assert(zbl_bios_e820_accept(&state, bios, 3, 1, 0, 0, 0) == ZBL_E820_COMPLETE);
	assert(state.count == 2);
	assert(zbl_bios_e820_accept(&state, bios, 3, 0, 0x534d4150, 24, 0) == ZBL_E820_COMPLETE);
	assert(state.count == 3 && bios[2].attributes == 0);
	state.count = 2; state.token = 3;
	assert(zbl_bios_e820_accept(&state, bios, 3, 0, 0x534d4150, 24, 7) == ZBL_E820_CAPACITY);

	assert(zbl_bios_normalize_memory_map(bios, 3, ranges, 8, &count) == ZBL_MEMORY_OK);
	assert(count == 3 && ranges[0].base == 0x100000000ULL);
	assert(ranges[1].type == ZBL6_MEMORY_RESERVED && ranges[2].size == 0x2000);
	bios[1].type = 1; bios[1].attributes = 9;
	assert(zbl_bios_normalize_memory_map(bios, 3, ranges, 8, &count) == ZBL_MEMORY_OK);
	assert(ranges[1].type == ZBL6_MEMORY_RESERVED && ranges[1].attributes == 9);
	memset(efi, 0, sizeof(efi));
	efi[0].Type = EfiBootServicesData; efi[0].PhysicalStart = 0x1000;
	efi[0].NumberOfPages = 2; efi[0].Attribute = 8;
	efi[1].Type = EfiConventionalMemory; efi[1].PhysicalStart = 0x100000000ULL;
	efi[1].NumberOfPages = 16; efi[1].Attribute = 8;
	assert(zbl_uefi_normalize_memory_map_v6(efi, sizeof(efi), sizeof(efi[0]), ranges, 8, &count) == ZBL_MEMORY_OK);
	assert(count == 2 && ranges[0].type == ZBL6_MEMORY_BOOT_RECLAIM && ranges[1].base == 0x100000000ULL);
	efi[0].Attribute |= 1ULL << 63;
	assert(zbl_uefi_normalize_memory_map_v6(efi, sizeof(efi), sizeof(efi[0]), ranges, 8, &count) == ZBL_MEMORY_OK);
	assert(ranges[0].type == ZBL6_MEMORY_RESERVED && ranges[0].attributes == ((1ULL << 63) | 8));
	efi[1].PhysicalStart = 0x2000;
	assert(zbl_uefi_normalize_memory_map_v6(efi, sizeof(efi), sizeof(efi[0]), ranges, 8, &count) == ZBL_MEMORY_OVERLAP && count == 0);
	assert(zbl_uefi_normalize_memory_map_v6(efi, sizeof(efi) - 1, sizeof(efi[0]), ranges, 8, &count) == ZBL_MEMORY_INVALID);
	assert(zbl_uefi_normalize_memory_map_v6((void *)(UINTPTR_MAX - 7), 40, 40, ranges, 8, &count) == ZBL_MEMORY_OVERFLOW);
	ranges[0] = (struct zbl6_memory_range_v6){0, 0x800000, ZBL6_MEMORY_USABLE, 0, 1};
	assert(zbl6_memory_envelope_valid(&memory, ZBL6_MEMORY_SOURCE_BIOS_E820));
	assert(zbl6_memory_contents_valid(&memory, ranges, allocations));
	memory.ranges = (1ULL << 30) - 8;
	assert(!zbl6_memory_envelope_valid(&memory, ZBL6_MEMORY_SOURCE_BIOS_E820));
	memory.ranges = 0x11000;
	allocations[0].lifetime = ZBL6_BOOT_AFTER_INIT;
	assert(!zbl6_memory_contents_valid(&memory, ranges, allocations));
	allocations[0].lifetime = ZBL6_BOOT_KEEP;
	allocations[1].size = 0x1000; memory.bootstrap_cr3 = 0x51000;
	assert(!zbl6_memory_contents_valid(&memory, ranges, allocations));
	memory.bootstrap_cr3 = 0x50000;
	allocations[2].size = 0x1000;
	assert(!zbl6_memory_contents_valid(&memory, ranges, allocations));
	allocations[2].size = 0x10000;
	ranges[0].size = UINT64_MAX;
	assert(!zbl6_memory_contents_valid(&memory, ranges, allocations));
	flags = ZBL6_HANDOFF_FLAG_MEMORY_MAP | ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS | ZBL6_HANDOFF_FLAG_BOOT_ALLOCATIONS;
	memset(&handoff, 0, sizeof(handoff));
	handoff.prefix.common.common.magic = ZBL6_HANDOFF_MAGIC;
	handoff.prefix.common.common.version = ZBL6_HANDOFF_V6_VERSION;
	handoff.prefix.common.common.size = sizeof(handoff);
	handoff.prefix.common.common.flags = flags;
	assert(zbl6_handoff_classify_raw(&handoff) == ZBL6_HANDOFF_FORM_V6_BIOS);
	assert(zbl6_handoff_classify(6, sizeof(handoff), flags & ~ZBL6_HANDOFF_FLAG_MEMORY_MAP) == ZBL6_HANDOFF_FORM_INVALID);
	assert(zbl6_handoff_classify(6, sizeof(handoff) - 1, flags) == ZBL6_HANDOFF_FORM_INVALID);
	flags |= ZBL6_HANDOFF_FLAG_UEFI | ZBL6_HANDOFF_FLAG_ACPI_RSDP | ZBL6_HANDOFF_FLAG_FRAMEBUFFER | ZBL6_HANDOFF_FLAG_BOOT_UUID;
	assert(zbl6_handoff_classify(6, ZBL6_HANDOFF_V6_UEFI_SIZE, flags) == ZBL6_HANDOFF_FORM_V6_UEFI);
	assert(zbl6_uefi_partition_handoff_valid(6, ZBL6_PARTITION_SCHEME_GPT, 0, 0, flags));
	assert(!zbl6_uefi_partition_handoff_valid(6, ZBL6_PARTITION_SCHEME_GPT, 1, 0, flags));
	puts("PASS: BIOS/UEFI adapters and v6 ownership validation");
	return 0;
}
