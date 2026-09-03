/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Canonical amd64 PCI message-interrupt source validation.
 */

#include "irq.h"

static int hex_digit(char character);
static int parse_hex(const char *text, unsigned digits, unsigned maximum);

/*
 * Validates the canonical PCI source form used by message interrupts.
 */
int
amd64_msi_source_valid(
	const char *source)
{
	unsigned length;
	int valid;

	/* Rejects an absent source before scanning it. */
	if (source == NULL)
		return 0;

	/* Measures at most the canonical sixteen-character source form. */
	length = 0;
	while (length <= 16U && source[length] != '\0')
		length++;

	/* Requires the canonical length and fixed prefix. */
	if (length != 16U)
		return 0;
	if (source[0] != 'P' || source[1] != 'C' ||
	    source[2] != 'I' || source[3] != ' ')
		return 0;

	/* Validates the domain before inspecting its separator. */
	valid = parse_hex(source + 4, 4, 0xffffU);
	if (!valid)
		return 0;

	/* Requires the domain-to-bus separator. */
	if (source[8] != ':')
		return 0;

	/* Validates the bus before inspecting its separator. */
	valid = parse_hex(source + 9, 2, 0xffU);
	if (!valid)
		return 0;

	/* Requires the bus-to-slot separator. */
	if (source[11] != ':')
		return 0;

	/* Validates the slot before inspecting its separator. */
	valid = parse_hex(source + 12, 2, 0x1fU);
	if (!valid)
		return 0;

	/* Requires the slot-to-function separator. */
	if (source[14] != '.')
		return 0;

	/* Validates the final function number. */
	valid = parse_hex(source + 15, 1, 7U);
	if (!valid)
		return 0;

	/* Accepts the complete canonical source. */
	return 1;
}

/* Converts one lowercase hexadecimal digit. */
static int
hex_digit(
	char character)
{
	/* Converts a decimal digit directly. */
	if (character >= '0' && character <= '9')
		return character - '0';

	/* Converts the accepted lowercase alphabetic digits. */
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;

	/* Rejects every other character. */
	return -1;
}

/* Validates a fixed-width hexadecimal field against an upper bound. */
static int
parse_hex(
	const char *text,
	unsigned digits,
	unsigned maximum)
{
	unsigned value;
	unsigned index;
	int digit;

	/* Accumulates exactly the caller-specified number of digits. */
	value = 0;
	for (index = 0; index < digits; index++) {
		digit = hex_digit(text[index]);

		/* Rejects the first byte outside the hexadecimal alphabet. */
		if (digit < 0)
			return 0;
		value = value * 16U + (unsigned)digit;
	}

	/* Accepts a decoded field within its inclusive upper bound. */
	if (value <= maximum)
		return 1;

	/* Rejects a decoded field above its upper bound. */
	return 0;
}
