/* Host validation for the fixed X68000 supervisor mappings. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdio.h>
#include "src/hal/m68k/bsp-x68k/memory-map.h"
#include "src/hal/m68k/space.h"

#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

int
main(void)
{
	const struct x68k_physical_mapping *map;
	size_t count, index, other;
	int saw_scsi = 0, saw_rom = 0;

	map = x68k_physical_mappings(&count);
	CHECK(map != NULL && count >= 15U);
	for (index = 0; index < count; index++) {
		uintptr_t end;
		CHECK(map[index].name != NULL);
		CHECK((map[index].physical & M68K030_PAGE_MASK) == 0);
		CHECK((map[index].size & M68K030_PAGE_MASK) == 0);
		CHECK(map[index].size != 0);
		CHECK(map[index].physical <= UINTPTR_MAX - map[index].size);
		end = map[index].physical + map[index].size;
		CHECK(end <= M68K030_DIRECT_SIZE);
		CHECK((map[index].attributes & (HAL_SPACE_NOCACHE |
			HAL_SPACE_DEVICE)) == (HAL_SPACE_NOCACHE |
			HAL_SPACE_DEVICE));
		for (other = index + 1; other < count; other++)
			CHECK(end <= map[other].physical ||
				map[other].physical + map[other].size <=
				map[index].physical);
		if (map[index].physical == 0x00e96000U)
			saw_scsi = 1;
		if (map[index].physical == 0x00f00000U) {
			saw_rom = 1;
			CHECK((map[index].attributes & HAL_SPACE_WRITE) == 0);
		}
	}
	CHECK(saw_scsi && saw_rom);
	puts("X68k memory-map host tests passed");
	return 0;
}
