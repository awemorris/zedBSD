/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UEFI loader's reading of files on the root partition in zedbsd.cfg.
 *
 * Only the UEFI loader reads a root partition; the BIOS loaders and the
 * shared parser go on refusing a path with a colon, so a rootpart: line
 * stops them as any other invalid path does.
 */

#include "root-config.h"

/* The prefix that puts a kernel= or logo= path on the root partition. */
#define ROOT_PREFIX "rootpart:"
#define ROOT_PREFIX_LENGTH 9U

/* The only rootpart= form the loader reads. */
#define ROOTPART_PARTUUID "rootpart=PARTUUID="
#define ROOTPART_PARTUUID_LENGTH 18U

/* The rootpart= line's name, which the kernel also reads. */
#define ROOTPART_NAME "rootpart="
#define ROOTPART_NAME_LENGTH 9U

/* The length of a GUID's text form. */
#define GUID_TEXT_LENGTH 36U

static int starts_with(const uint8_t *text, size_t length, const char *prefix, size_t prefix_length);
static int hex_value(uint8_t character);
static int guid_parse(const uint8_t *text, size_t length, uint8_t guid[16]);

/*
 * Copies zedbsd.cfg without the rootpart: prefixes and reports the root.
 *
 * The output holds the same lines in the same order; only the prefix of
 * the kernel= and logo= lines that carry it, with the slash after it, is
 * left out.
 */
enum zbl_uefi_root_config_result
zbl_uefi_root_config_split(
	const void *source,
	size_t source_size,
	void *output,
	size_t output_capacity,
	size_t *output_size,
	struct zbl_uefi_root_config *root)
{
	const uint8_t *input;
	const uint8_t *line;
	uint8_t *written;
	size_t position;
	size_t line_length;
	size_t value_length;
	size_t at;
	size_t index;
	size_t skipped;
	int prefixed;
	int on_kernel_line;
	int on_logo_line;
	int is_rootpart;
	int is_partuuid;
	int rootpart_lines;
	int partuuid_valid;
	int parsed;

	/* Refuses a call without its objects or room for the copy. */
	if (output == 0 || output_size == 0 || root == 0)
		return ZBL_UEFI_ROOT_CONFIG_INVALID_ARGUMENT;
	if (source == 0 && source_size != 0)
		return ZBL_UEFI_ROOT_CONFIG_INVALID_ARGUMENT;
	if (output_capacity < source_size)
		return ZBL_UEFI_ROOT_CONFIG_INVALID_ARGUMENT;

	/* Starts with nothing on the root and no rootpart= line seen. */
	input = source;
	written = output;
	root->kernel_on_root = 0;
	root->logo_on_root = 0;
	for (index = 0; index < sizeof(root->partuuid); index++)
		root->partuuid[index] = 0;
	rootpart_lines = 0;
	partuuid_valid = 0;
	at = 0;

	/* Copies the file one line at a time. */
	position = 0;
	while (position < source_size) {
		/* Finds the line's end, without its LF. */
		line = input + position;
		line_length = 0;
		while (position + line_length < source_size && line[line_length] != '\n')
			line_length++;

		/* Leaves the prefix out of a kernel= or logo= line that carries it. */
		prefixed = 0;
		on_kernel_line = starts_with(line, line_length, "kernel=" ROOT_PREFIX, 7U + ROOT_PREFIX_LENGTH);
		on_logo_line = starts_with(line, line_length, "logo=" ROOT_PREFIX, 5U + ROOT_PREFIX_LENGTH);
		if (on_kernel_line) {
			root->kernel_on_root = 1;
			prefixed = 7;
		} else if (on_logo_line) {
			root->logo_on_root = 1;
			prefixed = 5;
		}

		/*
		 * Copies the line, skipping the prefix after the name and the
		 * path's leading slash: the shared parser takes a path relative
		 * to the volume's root, and the root's path is absolute anyway.
		 */
		skipped = 0;
		if (prefixed != 0) {
			skipped = ROOT_PREFIX_LENGTH;
			if ((size_t)prefixed + skipped < line_length && line[prefixed + skipped] == '/')
				skipped++;
		}

		/* Writes each byte of the line but the prefix. */
		for (index = 0; index < line_length; index++) {
			if (prefixed != 0 && index >= (size_t)prefixed && index < (size_t)prefixed + skipped)
				continue;
			written[at] = line[index];
			at++;
		}

		/* Reads a rootpart= line's PARTUUID, the only form the loader takes. */
		is_rootpart = starts_with(line, line_length, ROOTPART_NAME, ROOTPART_NAME_LENGTH);
		if (is_rootpart) {
			rootpart_lines++;
			is_partuuid = starts_with(line, line_length, ROOTPART_PARTUUID, ROOTPART_PARTUUID_LENGTH);
			if (is_partuuid) {
				value_length = line_length - ROOTPART_PARTUUID_LENGTH;
				if (value_length != 0 && line[line_length - 1] == '\r')
					value_length--;
				parsed = guid_parse(line + ROOTPART_PARTUUID_LENGTH, value_length, root->partuuid);
				if (parsed)
					partuuid_valid = 1;
				else
					partuuid_valid = -1;
			}
		}

		/* Copies the LF that ends the line, when there is one. */
		position += line_length;
		if (position < source_size) {
			written[at] = '\n';
			at++;
			position++;
		}
	}

	/* The copy is complete. */
	*output_size = at;

	/* Nothing more is needed when no file is on the root. */
	if (!root->kernel_on_root && !root->logo_on_root)
		return ZBL_UEFI_ROOT_CONFIG_OK;

	/* Refuses a root file without exactly one rootpart= line naming a PARTUUID. */
	if (rootpart_lines == 0)
		return ZBL_UEFI_ROOT_CONFIG_NO_ROOTPART;
	if (rootpart_lines > 1)
		return ZBL_UEFI_ROOT_CONFIG_DUPLICATE_ROOTPART;
	if (partuuid_valid == 0)
		return ZBL_UEFI_ROOT_CONFIG_ROOTPART_NOT_PARTUUID;
	if (partuuid_valid < 0)
		return ZBL_UEFI_ROOT_CONFIG_BAD_PARTUUID;

	/* Succeeded: the root's PARTUUID is known. */
	return ZBL_UEFI_ROOT_CONFIG_OK;
}

/* Names a split result for a loader message. */
const char *
zbl_uefi_root_config_result_name(
	enum zbl_uefi_root_config_result result)
{
	/* Names each result by what is wrong with the file. */
	switch (result) {
	case ZBL_UEFI_ROOT_CONFIG_OK:
		return "ok";
	case ZBL_UEFI_ROOT_CONFIG_INVALID_ARGUMENT:
		return "invalid argument";
	case ZBL_UEFI_ROOT_CONFIG_NO_ROOTPART:
		return "rootpart: without rootpart=";
	case ZBL_UEFI_ROOT_CONFIG_DUPLICATE_ROOTPART:
		return "more than one rootpart=";
	case ZBL_UEFI_ROOT_CONFIG_ROOTPART_NOT_PARTUUID:
		return "rootpart= is not PARTUUID=";
	case ZBL_UEFI_ROOT_CONFIG_BAD_PARTUUID:
		return "bad rootpart PARTUUID";
	default:
		return "unknown";
	}
}

/* Reports whether a line starts with a prefix. */
static int
starts_with(
	const uint8_t *text,
	size_t length,
	const char *prefix,
	size_t prefix_length)
{
	size_t index;

	/* A line shorter than the prefix cannot start with it. */
	if (length < prefix_length)
		return 0;

	/* Compares each byte of the prefix. */
	for (index = 0; index < prefix_length; index++) {
		if (text[index] != (uint8_t)prefix[index])
			return 0;
	}

	/* The line starts with the prefix. */
	return 1;
}

/* Reads one hexadecimal digit of either case; -1 for any other byte. */
static int
hex_value(
	uint8_t character)
{
	/* A decimal digit. */
	if (character >= '0' && character <= '9')
		return character - '0';

	/* A lower-case letter digit. */
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;

	/* An upper-case letter digit. */
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;

	/* Not a digit. */
	return -1;
}

/*
 * Reads a GUID's text form into GPT's on-disk byte order.
 *
 * The first three fields are stored little endian, the last two as
 * written.  The all-zero GUID names no partition and is refused.
 */
static int
guid_parse(
	const uint8_t *text,
	size_t length,
	uint8_t guid[16])
{
	static const unsigned order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
	unsigned position;
	unsigned index;
	unsigned nonzero;
	int high;
	int low;

	/* Refuses a text of another length. */
	if (length != GUID_TEXT_LENGTH)
		return 0;

	/* Reads the sixteen bytes, with a dash before bytes 4, 6, 8 and 10. */
	position = 0;
	nonzero = 0;
	for (index = 0; index < 16U; index++) {
		/* Takes the dash where the form puts one. */
		if (index == 4U || index == 6U || index == 8U || index == 10U) {
			if (text[position] != '-')
				return 0;
			position++;
		}

		/* Takes the two digits of the byte. */
		high = hex_value(text[position]);
		low = hex_value(text[position + 1U]);
		if (high < 0 || low < 0)
			return 0;
		guid[order[index]] = (uint8_t)(high * 16 + low);
		nonzero |= guid[order[index]];
		position += 2U;
	}

	/* Refuses the all-zero GUID. */
	if (nonzero == 0)
		return 0;

	/* Succeeded: the GUID is read. */
	return 1;
}
