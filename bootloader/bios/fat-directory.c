/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Bounded FAT directory-name matching for native BIOS loaders.
 */

#include "fat-directory.h"

/* The size of one FAT directory entry. */
#define FAT_ENTRY_SIZE		32U

/* The offset of an entry's attribute byte. */
#define FAT_ATTRIBUTE_OFFSET	11U

/* The attribute value that marks a long-file-name entry. */
#define FAT_ATTRIBUTE_LFN	0x0fU

/* The attribute bit that marks a volume label. */
#define FAT_ATTRIBUTE_VOLUME	0x08U

/* The first name byte of a deleted entry. */
#define FAT_DELETED		0xe5U

/* The ordinal bit that marks the last (first stored) entry of a long name. */
#define FAT_LFN_LAST		0x40U

/* The bits of an LFN ordinal that number the entry. */
#define FAT_LFN_ORDINAL_MASK	0x1fU

/* The UCS-2 characters one LFN entry holds. */
#define FAT_LFN_CHARACTERS	13U

static unsigned char ascii_fold(unsigned char value);
static int ascii_equal(const unsigned char *left, size_t left_length, const char *right, size_t right_length);
static void long_name_reset(struct zbl_bios_fat_directory_state *state);
static uint8_t short_name_checksum(const unsigned char *entry);
static uint16_t read_le16(const unsigned char *source);
static uint16_t long_name_character(const unsigned char *entry, unsigned index);
static int long_name_entry_continues(const struct zbl_bios_fat_directory_state *state, const unsigned char *entry, unsigned ordinal);
static void long_name_entry(struct zbl_bios_fat_directory_state *state, const unsigned char *entry);
static int long_name_matches(const struct zbl_bios_fat_directory_state *state, const unsigned char *entry, const char *component, size_t component_length);
static int short_name_matches(const unsigned char *entry, const char *component, size_t component_length);

/*
 * Searches one complete directory sector for a path component.
 *
 * The long-name state is carried across sector boundaries, so an LFN run
 * split between sectors still matches.
 */
int
zbl_bios_fat_directory_search(
	struct zbl_bios_fat_directory_state *state,
	const void *sector_pointer,
	size_t sector_size,
	const char *component,
	size_t component_length)
{
	const unsigned char *sector;
	const unsigned char *entry;
	size_t offset;
	unsigned attribute;
	int matches;

	/* Views the sector as bytes. */
	sector = sector_pointer;

	/* Refuses missing buffers, an unusable component and a partial sector. */
	if (state == NULL ||
	    sector == NULL ||
	    component == NULL ||
	    component_length == 0U ||
	    component_length > 255U ||
	    sector_size == 0U ||
	    sector_size % FAT_ENTRY_SIZE != 0U)
		return -2;

	/* Examines each entry of the sector in order. */
	for (offset = 0U; offset < sector_size; offset += FAT_ENTRY_SIZE) {
		/* Finds the entry and its attribute byte. */
		entry = sector + offset;
		attribute = entry[FAT_ATTRIBUTE_OFFSET];

		/* A zero first byte marks the end of the directory. */
		if (entry[0] == 0U) {
			long_name_reset(state);
			return -2;
		}

		/* A deleted entry breaks any long-name run before it. */
		if (entry[0] == FAT_DELETED) {
			long_name_reset(state);
			continue;
		}

		/* A long-name entry adds its characters to the run. */
		if (attribute == FAT_ATTRIBUTE_LFN) {
			long_name_entry(state, entry);
			continue;
		}

		/* Matches a file's entry by its long name, then by its short name; a volume label never matches. */
		matches = 0;
		if ((attribute & FAT_ATTRIBUTE_VOLUME) == 0U) {
			matches = long_name_matches(state, entry, component, component_length);
			if (!matches)
				matches = short_name_matches(entry, component, component_length);
		}

		/* The short entry ends the long-name run that belonged to it. */
		long_name_reset(state);

		/* Reports where the matching entry is in the sector. */
		if (matches)
			return (int)offset;
	}

	/* Reports that this sector holds no match. */
	return -1;
}

/* Folds a lower-case ASCII letter to upper case. */
static unsigned char
ascii_fold(
	unsigned char value)
{
	/* A lower-case letter becomes its upper-case letter. */
	if (value >= 'a' && value <= 'z')
		return (unsigned char)(value - ('a' - 'A'));

	/* Any other byte is unchanged. */
	return value;
}

/* Reports whether two names are equal, ignoring ASCII case. */
static int
ascii_equal(
	const unsigned char *left,
	size_t left_length,
	const char *right,
	size_t right_length)
{
	size_t index;
	unsigned char left_folded;
	unsigned char right_folded;

	/* Names of different lengths differ. */
	if (left_length != right_length)
		return 0;

	/* Compares the names character by character. */
	for (index = 0U; index < left_length; index++) {
		/* Folds both characters before comparing them. */
		left_folded = ascii_fold(left[index]);
		right_folded = ascii_fold((unsigned char)right[index]);
		if (left_folded != right_folded)
			return 0;
	}

	/* Succeeded: the names are equal. */
	return 1;
}

/* Forgets the long-name run being assembled. */
static void
long_name_reset(
	struct zbl_bios_fat_directory_state *state)
{
	size_t index;

	/* Marks every character slot unfilled. */
	for (index = 0U; index < sizeof(state->long_name); index++)
		state->long_name[index] = 0xffU;

	/* Clears the run's progress. */
	state->long_name_length = 0U;
	state->expected_ordinal = 0U;
	state->checksum = 0U;
	state->active = 0U;
	state->length_known = 0U;
	state->invalid = 0U;
	state->reserved = 0U;
}

/* Computes the checksum an LFN run records for its short entry's 8.3 name. */
static uint8_t
short_name_checksum(
	const unsigned char *entry)
{
	unsigned index;
	unsigned carry;
	uint8_t checksum;

	/* Rotates the sum right by one bit and adds each name byte in turn. */
	checksum = 0U;
	for (index = 0U; index < 11U; index++) {
		/* The bit rotated out of the bottom comes back at the top. */
		carry = 0U;
		if ((checksum & 1U) != 0U)
			carry = 0x80U;

		checksum = (uint8_t)(carry + (checksum >> 1U) + entry[index]);
	}

	/* Reports the checksum of the name. */
	return checksum;
}

/* Reads a little-endian 16-bit value. */
static uint16_t
read_le16(
	const unsigned char *source)
{
	/* Combines the low byte and the high byte. */
	return (uint16_t)source[0] | (uint16_t)((uint16_t)source[1] << 8U);
}

/* Reads one of the 13 characters an LFN entry holds. */
static uint16_t
long_name_character(
	const unsigned char *entry,
	unsigned index)
{
	/* The offsets of the characters, which are split across three fields of the entry. */
	static const uint8_t offsets[FAT_LFN_CHARACTERS] = {
		1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U,
		24U, 28U, 30U,
	};
	uint16_t character;

	/* Reads the character at its offset. */
	character = read_le16(entry + offsets[index]);

	/* Reports the character. */
	return character;
}

/* Reports whether an LFN entry continues the run being assembled. */
static int
long_name_entry_continues(
	const struct zbl_bios_fat_directory_state *state,
	const unsigned char *entry,
	unsigned ordinal)
{
	uint16_t first_cluster;

	/* No run was opened by a last entry. */
	if (state->active == 0U)
		return 0;

	/* Ordinal zero numbers no entry. */
	if (ordinal == 0U)
		return 0;

	/* The entries of a run come in descending order. */
	if (ordinal != state->expected_ordinal)
		return 0;

	/* An LFN entry's type byte is zero. */
	if (entry[12] != 0U)
		return 0;

	/* An LFN entry's first-cluster field is zero. */
	first_cluster = read_le16(entry + 26U);
	if (first_cluster != 0U)
		return 0;

	/* Every entry of a run carries the same checksum. */
	if (entry[13] != state->checksum)
		return 0;

	/* Succeeded: the entry belongs to the run. */
	return 1;
}

/* Adds one LFN entry's characters to the long-name run. */
static void
long_name_entry(
	struct zbl_bios_fat_directory_state *state,
	const unsigned char *entry)
{
	unsigned raw_ordinal;
	unsigned ordinal;
	unsigned index;
	size_t position;
	uint16_t character;
	int continues;

	/* Reads the entry's ordinal and the number it carries. */
	raw_ordinal = entry[0];
	ordinal = raw_ordinal & FAT_LFN_ORDINAL_MASK;

	/* The last entry of a name is stored first and opens a new run. */
	if ((raw_ordinal & FAT_LFN_LAST) != 0U) {
		long_name_reset(state);

		/* A name of more than 20 entries is beyond the 255-character limit. */
		if (ordinal == 0U || ordinal > 20U)
			return;

		/* Opens the run at this entry's ordinal and checksum. */
		state->active = 1U;
		state->expected_ordinal = (uint8_t)ordinal;
		state->checksum = entry[13];
	}

	/* An entry out of its run invalidates the run. */
	continues = long_name_entry_continues(state, entry, ordinal);
	if (!continues) {
		state->invalid = 1U;
		state->active = 0U;
		return;
	}

	/* Stores each of the entry's characters at its place in the name. */
	for (index = 0U; index < FAT_LFN_CHARACTERS; index++) {
		/* Finds the character and where it goes in the name. */
		position = (size_t)(ordinal - 1U) * FAT_LFN_CHARACTERS + index;
		character = long_name_character(entry, index);

		/* A terminator fixes the name's length the first time it is seen. */
		if (character == 0U) {
			if (!state->length_known && position <= 255U) {
				state->long_name_length = (uint16_t)position;
				state->length_known = 1U;
			}

			continue;
		}

		/* Padding after the terminator holds no character. */
		if (character == 0xffffU)
			continue;

		/* A character beyond the name, outside printable ASCII, or after the terminator invalidates the run. */
		if (position >= sizeof(state->long_name) ||
		    character < 0x21U ||
		    character > 0x7eU ||
		    (state->length_known && position >= state->long_name_length)) {
			state->invalid = 1U;
			continue;
		}

		/* Stores the character. */
		state->long_name[position] = (unsigned char)character;

		/* Until a terminator is seen, the name runs to its last stored character. */
		if (!state->length_known && position + 1U > state->long_name_length)
			state->long_name_length = (uint16_t)(position + 1U);
	}

	/* The next entry of the run carries the ordinal below this one. */
	state->expected_ordinal--;
}

/* Reports whether the long-name run belongs to the short entry and names the component. */
static int
long_name_matches(
	const struct zbl_bios_fat_directory_state *state,
	const unsigned char *entry,
	const char *component,
	size_t component_length)
{
	size_t index;
	uint8_t checksum;
	int equal;

	/* No complete, valid run precedes the entry. */
	if (state->active == 0U ||
	    state->invalid != 0U ||
	    state->expected_ordinal != 0U)
		return 0;

	/* A run whose checksum is not the short name's belongs to another entry. */
	checksum = short_name_checksum(entry);
	if (state->checksum != checksum)
		return 0;

	/* A name with a character slot no entry filled is incomplete. */
	for (index = 0U; index < state->long_name_length; index++) {
		if (state->long_name[index] == 0xffU)
			return 0;
	}

	/* Compares the long name with the component. */
	equal = ascii_equal(state->long_name,
			    state->long_name_length,
			    component,
			    component_length);
	if (!equal)
		return 0;

	/* Succeeded: the long name is the component. */
	return 1;
}

/* Reports whether the entry's 8.3 name is the component. */
static int
short_name_matches(
	const unsigned char *entry,
	const char *component,
	size_t component_length)
{
	unsigned char visible[12];
	size_t length;
	size_t base_length;
	size_t extension_length;
	size_t index;
	int equal;

	/* Trims the padding off the base name. */
	base_length = 8U;
	while (base_length != 0U && entry[base_length - 1U] == ' ')
		base_length--;

	/* Trims the padding off the extension. */
	extension_length = 3U;
	while (extension_length != 0U && entry[8U + extension_length - 1U] == ' ')
		extension_length--;

	/* Copies the base name into the visible form. */
	length = 0U;
	for (index = 0U; index < base_length; index++) {
		visible[length] = entry[index];
		length++;
	}

	/* Appends a dot and the extension when there is one. */
	if (extension_length != 0U) {
		visible[length] = '.';
		length++;
		for (index = 0U; index < extension_length; index++) {
			visible[length] = entry[8U + index];
			length++;
		}
	}

	/* Compares the visible name with the component. */
	equal = ascii_equal(visible, length, component, component_length);
	if (!equal)
		return 0;

	/* Succeeded: the short name is the component. */
	return 1;
}
