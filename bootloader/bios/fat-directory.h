/* Bounded FAT directory-name matching for native BIOS loaders. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BOOTLOADER_BIOS_FAT_DIRECTORY_H
#define ZEDBSD_BOOTLOADER_BIOS_FAT_DIRECTORY_H

#define ZBL_BIOS_FAT_DIRECTORY_STATE_SIZE 264U

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

struct zbl_bios_fat_directory_state {
	unsigned char long_name[256];
	uint16_t long_name_length;
	uint8_t expected_ordinal;
	uint8_t checksum;
	uint8_t active;
	uint8_t length_known;
	uint8_t invalid;
	uint8_t reserved;
};

_Static_assert(sizeof(struct zbl_bios_fat_directory_state) ==
	       ZBL_BIOS_FAT_DIRECTORY_STATE_SIZE,
	       "BIOS FAT directory state size");

/*
 * Search one complete directory sector.  State is retained across sector
 * boundaries so an LFN run split between sectors remains valid.
 *
 * Returns a directory-entry byte offset on success, -1 when this sector has
 * no match, or -2 when the FAT end-of-directory marker was encountered.
 */
int zbl_bios_fat_directory_search(
	struct zbl_bios_fat_directory_state *state,
	const void *sector, size_t sector_size,
	const char *component, size_t component_length);
#endif

#endif
