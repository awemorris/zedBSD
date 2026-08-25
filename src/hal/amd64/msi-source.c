/* Canonical amd64 message-interrupt source parser. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "irq.h"

static int
hex_digit(char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	return -1;
}

static int
parse_hex(const char *text, unsigned digits, unsigned maximum)
{
	unsigned value = 0;
	unsigned index;

	for (index = 0; index < digits; index++) {
		int digit = hex_digit(text[index]);
		if (digit < 0)
			return 0;
		value = value * 16U + (unsigned)digit;
	}
	return value <= maximum;
}

int
amd64_msi_source_valid(const char *source)
{
	unsigned length = 0;
	if (source == NULL)
		return 0;
	while (length <= 16U && source[length] != '\0')
		length++;
	return length == 16U && source[0] == 'P' && source[1] == 'C' &&
	    source[2] == 'I' && source[3] == ' ' &&
	    parse_hex(source + 4, 4, 0xffffU) && source[8] == ':' &&
	    parse_hex(source + 9, 2, 0xffU) && source[11] == ':' &&
	    parse_hex(source + 12, 2, 0x1fU) && source[14] == '.' &&
	    parse_hex(source + 15, 1, 7U);
}
