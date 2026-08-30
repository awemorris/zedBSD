/* Focused host checks for the BIOS FAT configured-path matcher. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootloader/bios/fat-directory.h"

static void
fail(const char *message)
{
	fprintf(stderr, "fat-directory-host-test: %s\n", message);
	exit(1);
}

static uint8_t
checksum(const unsigned char name[11])
{
	uint8_t result = 0;

	for (unsigned index = 0; index < 11; index++)
		result = (uint8_t)(((result & 1U) ? 0x80U : 0U) +
		    (result >> 1U) + name[index]);
	return result;
}

static void
put16(unsigned char *destination, uint16_t value)
{
	destination[0] = (unsigned char)value;
	destination[1] = (unsigned char)(value >> 8);
}

static void
lfn_entry(unsigned char entry[32], unsigned ordinal, int last,
	  uint8_t sum, const char *name)
{
	static const unsigned offsets[13] = {
		1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
	};
	size_t length = strlen(name);
	size_t base = (ordinal - 1U) * 13U;

	memset(entry, 0xff, 32);
	entry[0] = (unsigned char)(ordinal | (last ? 0x40U : 0U));
	entry[11] = 0x0f;
	entry[12] = 0;
	entry[13] = sum;
	put16(entry + 26, 0);
	for (unsigned index = 0; index < 13; index++) {
		size_t position = base + index;
		uint16_t value = position < length ?
		    (unsigned char)name[position] :
		    position == length ? 0 : 0xffff;

		put16(entry + offsets[index], value);
	}
}

static void
short_entry(unsigned char entry[32], const char name[11])
{
	memset(entry, 0, 32);
	memcpy(entry, name, 11);
	entry[11] = 0x20;
}

int
main(void)
{
	struct zbl_bios_fat_directory_state state = { 0 };
	unsigned char sector[512] = { 0 };
	static const char short_name[11] = "ZEDBSD  CFG";
	static const char lfn_short[11] = "KERNEL~1ELF";
	static const char long_name[] = "configured-kernels-v1.elf";
	uint8_t sum;

	short_entry(sector, short_name);
	if (zbl_bios_fat_directory_search(&state, sector, sizeof(sector),
	    "zedbsd.cfg", strlen("zedbsd.cfg")) != 0)
		fail("case-insensitive short name");

	memset(&state, 0, sizeof(state));
	memset(sector, 0xe5, sizeof(sector));
	sum = checksum((const unsigned char *)lfn_short);
	lfn_entry(sector + sizeof(sector) - 32, 2, 1, sum, long_name);
	if (zbl_bios_fat_directory_search(&state, sector, sizeof(sector),
	    long_name, strlen(long_name)) != -1)
		fail("first split LFN sector");
	memset(sector, 0, sizeof(sector));
	lfn_entry(sector, 1, 0, sum, long_name);
	short_entry(sector + 32, lfn_short);
	if (zbl_bios_fat_directory_search(&state, sector, sizeof(sector),
	    long_name, strlen(long_name)) != 32)
		fail(">13-character split LFN");

	puts("fat-directory-host-test: PASS");
	return 0;
}
