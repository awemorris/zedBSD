/* Bounded FAT directory-name matching for native BIOS loaders. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "fat-directory.h"

#define FAT_ENTRY_SIZE 32U
#define FAT_ATTRIBUTE_OFFSET 11U
#define FAT_ATTRIBUTE_LFN 0x0fU
#define FAT_ATTRIBUTE_VOLUME 0x08U
#define FAT_DELETED 0xe5U
#define FAT_LFN_LAST 0x40U
#define FAT_LFN_ORDINAL_MASK 0x1fU
#define FAT_LFN_CHARACTERS 13U

static unsigned char
ascii_fold(unsigned char value)
{
	if (value >= 'a' && value <= 'z')
		return (unsigned char)(value - ('a' - 'A'));
	return value;
}

static int
ascii_equal(const unsigned char *left, size_t left_length,
	    const char *right, size_t right_length)
{
	if (left_length != right_length)
		return 0;
	for (size_t index = 0U; index < left_length; index++)
		if (ascii_fold(left[index]) !=
		    ascii_fold((unsigned char)right[index]))
			return 0;
	return 1;
}

static void
long_name_reset(struct zbl_bios_fat_directory_state *state)
{
	for (size_t index = 0U; index < sizeof(state->long_name); index++)
		state->long_name[index] = 0xffU;
	state->long_name_length = 0U;
	state->expected_ordinal = 0U;
	state->checksum = 0U;
	state->active = 0U;
	state->length_known = 0U;
	state->invalid = 0U;
	state->reserved = 0U;
}

static uint8_t
short_name_checksum(const unsigned char *entry)
{
	uint8_t result = 0U;

	for (unsigned index = 0U; index < 11U; index++)
		result = (uint8_t)(((result & 1U) != 0U ? 0x80U : 0U) +
		    (result >> 1U) + entry[index]);
	return result;
}

static uint16_t
read_le16(const unsigned char *source)
{
	return (uint16_t)source[0] | (uint16_t)((uint16_t)source[1] << 8U);
}

static uint16_t
long_name_character(const unsigned char *entry, unsigned index)
{
	static const uint8_t offsets[FAT_LFN_CHARACTERS] = {
		1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U,
		24U, 28U, 30U,
	};

	return read_le16(entry + offsets[index]);
}

static void
long_name_entry(struct zbl_bios_fat_directory_state *state,
		const unsigned char *entry)
{
	unsigned raw_ordinal = entry[0];
	unsigned ordinal = raw_ordinal & FAT_LFN_ORDINAL_MASK;

	if ((raw_ordinal & FAT_LFN_LAST) != 0U) {
		long_name_reset(state);
		if (ordinal == 0U || ordinal > 20U)
			return;
		state->active = 1U;
		state->expected_ordinal = (uint8_t)ordinal;
		state->checksum = entry[13];
	}
	if (state->active == 0U || ordinal == 0U ||
	    ordinal != state->expected_ordinal || entry[12] != 0U ||
	    read_le16(entry + 26U) != 0U || entry[13] != state->checksum) {
		state->invalid = 1U;
		state->active = 0U;
		return;
	}
	for (unsigned index = 0U; index < FAT_LFN_CHARACTERS; index++) {
		size_t position = (size_t)(ordinal - 1U) *
		    FAT_LFN_CHARACTERS + index;
		uint16_t value = long_name_character(entry, index);

		if (value == 0U) {
			if (!state->length_known && position <= 255U) {
				state->long_name_length = (uint16_t)position;
				state->length_known = 1U;
			}
			continue;
		}
		if (value == 0xffffU)
			continue;
		if (position >= sizeof(state->long_name) || value < 0x21U ||
		    value > 0x7eU ||
		    (state->length_known &&
		     position >= state->long_name_length)) {
			state->invalid = 1U;
			continue;
		}
		state->long_name[position] = (unsigned char)value;
		if (!state->length_known && position + 1U > state->long_name_length)
			state->long_name_length = (uint16_t)(position + 1U);
	}
	state->expected_ordinal--;
}

static int
long_name_matches(const struct zbl_bios_fat_directory_state *state,
		  const unsigned char *entry, const char *component,
		  size_t component_length)
{
	if (state->active == 0U || state->invalid != 0U ||
	    state->expected_ordinal != 0U ||
	    state->checksum != short_name_checksum(entry))
		return 0;
	for (size_t index = 0U; index < state->long_name_length; index++)
		if (state->long_name[index] == 0xffU)
			return 0;
	return ascii_equal(state->long_name, state->long_name_length,
	    component, component_length);
}

static int
short_name_matches(const unsigned char *entry, const char *component,
		   size_t component_length)
{
	unsigned char visible[12];
	size_t length = 0U;
	size_t base_length = 8U;
	size_t extension_length = 3U;

	while (base_length != 0U && entry[base_length - 1U] == ' ')
		base_length--;
	while (extension_length != 0U &&
	    entry[8U + extension_length - 1U] == ' ')
		extension_length--;
	for (size_t index = 0U; index < base_length; index++)
		visible[length++] = entry[index];
	if (extension_length != 0U) {
		visible[length++] = '.';
		for (size_t index = 0U; index < extension_length; index++)
			visible[length++] = entry[8U + index];
	}
	return ascii_equal(visible, length, component, component_length);
}

int
zbl_bios_fat_directory_search(struct zbl_bios_fat_directory_state *state,
	const void *sector_pointer, size_t sector_size,
	const char *component, size_t component_length)
{
	const unsigned char *sector = sector_pointer;

	if (state == NULL || sector == NULL || component == NULL ||
	    component_length == 0U || component_length > 255U ||
	    sector_size == 0U || sector_size % FAT_ENTRY_SIZE != 0U)
		return -2;
	for (size_t offset = 0U; offset < sector_size;
	     offset += FAT_ENTRY_SIZE) {
		const unsigned char *entry = sector + offset;
		unsigned attribute = entry[FAT_ATTRIBUTE_OFFSET];
		int matches;

		if (entry[0] == 0U) {
			long_name_reset(state);
			return -2;
		}
		if (entry[0] == FAT_DELETED) {
			long_name_reset(state);
			continue;
		}
		if (attribute == FAT_ATTRIBUTE_LFN) {
			long_name_entry(state, entry);
			continue;
		}
		matches = (attribute & FAT_ATTRIBUTE_VOLUME) == 0U &&
		    (long_name_matches(state, entry, component,
		     component_length) ||
		     short_name_matches(entry, component, component_length));
		long_name_reset(state);
		if (matches)
			return (int)offset;
	}
	return -1;
}
