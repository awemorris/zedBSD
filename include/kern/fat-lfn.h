/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * VFAT long-file-name helpers.
 */

#ifndef ZEDBSD_FAT_LFN_H
#define ZEDBSD_FAT_LFN_H

#include <stddef.h>
#include <stdint.h>

#define FAT_LFN_MAX_UNITS	255U

struct fat_lfn_state {
	uint16_t units[FAT_LFN_MAX_UNITS + 1U];
	uint16_t unit_limit;
	uint8_t expected;
	uint8_t checksum;
	uint8_t active;
};

void
fat_lfn_reset(
	struct fat_lfn_state *state);
int
fat_lfn_feed(
	struct fat_lfn_state *state,
	const uint8_t raw[32]);
int
fat_lfn_finish(
	struct fat_lfn_state *state,
	const uint8_t sfn[32],
	char *output,
	size_t capacity);
uint8_t
fat_lfn_checksum(
	const uint8_t sfn[11]);
int
fat_utf8_casefold_equal(
	const char *left,
	const char *right);
void
fat_sfn_decode_preserve(
	const uint8_t raw[32],
	char *output,
	size_t capacity);
int
fat_utf8_to_utf16(
	const char *name,
	uint16_t units[FAT_LFN_MAX_UNITS],
	unsigned *unit_count);
void
fat_lfn_build_entry(
	uint8_t raw[32],
	const uint16_t *units,
	unsigned unit_count,
	unsigned ordinal,
	uint8_t checksum);
int
fat_sfn_make_alias(
	const char *name,
	unsigned serial,
	uint8_t sfn[11]);

#endif
