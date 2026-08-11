/*
 * zedBSD VFAT long-file-name decoder.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/fat-lfn.h"

static const uint8_t lfn_offsets[13] = {
	1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
};

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

void fat_lfn_reset(struct fat_lfn_state *state)
{
	unsigned i;

	state->unit_limit = 0;
	state->expected = 0;
	state->checksum = 0;
	state->active = 0;
	for (i = 0; i <= FAT_LFN_MAX_UNITS; i++)
		state->units[i] = 0xffffU;
}

uint8_t fat_lfn_checksum(const uint8_t sfn[11])
{
	uint8_t sum = 0;
	unsigned i;

	for (i = 0; i < 11; i++)
		sum = (uint8_t)(((sum & 1U) << 7) | (sum >> 1)) + sfn[i];
	return sum;
}

int fat_lfn_feed(struct fat_lfn_state *state, const uint8_t raw[32])
{
	unsigned ordinal = raw[0] & 0x1fU;
	unsigned i;

	if (raw[11] != 0x0fU || raw[12] != 0 || get16(raw + 26) != 0 ||
	    ordinal == 0 || ordinal > 20U) {
		fat_lfn_reset(state);
		return 0;
	}
	if (raw[0] & 0x40U) {
		fat_lfn_reset(state);
		state->active = 1;
		state->expected = (uint8_t)ordinal;
		state->checksum = raw[13];
		state->unit_limit = (uint16_t)(ordinal * 13U);
		if (state->unit_limit > FAT_LFN_MAX_UNITS + 1U) {
			fat_lfn_reset(state);
			return 0;
		}
	}
	if (!state->active || ordinal != state->expected ||
	    raw[13] != state->checksum || (raw[0] & 0x80U)) {
		fat_lfn_reset(state);
		return 0;
	}
	for (i = 0; i < 13; i++) {
		unsigned index = (ordinal - 1U) * 13U + i;
		if (index <= FAT_LFN_MAX_UNITS)
			state->units[index] = get16(raw + lfn_offsets[i]);
	}
	state->expected--;
	return 1;
}

static int append_utf8(char *output, size_t capacity, size_t *used,
		       uint32_t scalar)
{
	uint8_t bytes[4];
	unsigned count, i;

	if (scalar <= 0x7fU) {
		bytes[0] = (uint8_t)scalar; count = 1;
	} else if (scalar <= 0x7ffU) {
		bytes[0] = (uint8_t)(0xc0U | (scalar >> 6));
		bytes[1] = (uint8_t)(0x80U | (scalar & 0x3fU)); count = 2;
	} else if (scalar <= 0xffffU) {
		bytes[0] = (uint8_t)(0xe0U | (scalar >> 12));
		bytes[1] = (uint8_t)(0x80U | ((scalar >> 6) & 0x3fU));
		bytes[2] = (uint8_t)(0x80U | (scalar & 0x3fU)); count = 3;
	} else {
		bytes[0] = (uint8_t)(0xf0U | (scalar >> 18));
		bytes[1] = (uint8_t)(0x80U | ((scalar >> 12) & 0x3fU));
		bytes[2] = (uint8_t)(0x80U | ((scalar >> 6) & 0x3fU));
		bytes[3] = (uint8_t)(0x80U | (scalar & 0x3fU)); count = 4;
	}
	if (*used + count >= capacity)
		return 0;
	for (i = 0; i < count; i++)
		output[(*used)++] = (char)bytes[i];
	return 1;
}

int fat_lfn_finish(struct fat_lfn_state *state, const uint8_t sfn[32],
		   char *output, size_t capacity)
{
	size_t used = 0;
	unsigned i;
	int terminated = 0;

	if (!state->active || state->expected != 0 ||
	    state->checksum != fat_lfn_checksum(sfn) || capacity == 0)
		goto invalid;
	for (i = 0; i < state->unit_limit; i++) {
		uint32_t scalar = state->units[i];
		if (scalar == 0) {
			terminated = 1;
			break;
		}
		if (scalar == 0xffffU)
			goto invalid;
		if (scalar >= 0xd800U && scalar <= 0xdbffU) {
			uint32_t low;
			if (++i >= state->unit_limit)
				goto invalid;
			low = state->units[i];
			if (low < 0xdc00U || low > 0xdfffU)
				goto invalid;
			scalar = 0x10000U + ((scalar - 0xd800U) << 10) +
				(low - 0xdc00U);
		} else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
			goto invalid;
		}
		if (scalar == '/' || scalar == 0 ||
		    !append_utf8(output, capacity, &used, scalar))
			goto invalid;
	}
	if (used == 0 || (!terminated && state->unit_limit > FAT_LFN_MAX_UNITS))
		goto invalid;
	if (terminated) {
		for (; i < state->unit_limit; i++)
			if (state->units[i] != 0 && state->units[i] != 0xffffU)
				goto invalid;
	}
	output[used] = '\0';
	fat_lfn_reset(state);
	return 1;
invalid:
	fat_lfn_reset(state);
	if (capacity)
		output[0] = '\0';
	return 0;
}

void fat_sfn_decode_preserve(const uint8_t raw[32], char *output,
			     size_t capacity)
{
	size_t used = 0;
	unsigned i;
	int lower_base = (raw[12] & 0x08U) != 0;
	int lower_ext = (raw[12] & 0x10U) != 0;

	if (capacity == 0)
		return;
	for (i = 0; i < 8 && raw[i] != ' ' && used + 1U < capacity; i++) {
		uint8_t c = raw[i];
		if (lower_base && c >= 'A' && c <= 'Z') c += 'a' - 'A';
		output[used++] = (char)c;
	}
	if (raw[8] != ' ' && used + 1U < capacity) {
		output[used++] = '.';
		for (i = 8; i < 11 && raw[i] != ' ' && used + 1U < capacity; i++) {
			uint8_t c = raw[i];
			if (lower_ext && c >= 'A' && c <= 'Z') c += 'a' - 'A';
			output[used++] = (char)c;
		}
	}
	output[used] = '\0';
}

struct fat_casefold_range {
	uint32_t start;
	/* Bit 31 marks a stride-two range; Unicode scalar values never use it. */
	uint32_t encoded_end;
	int32_t delta;
};

#include "unicode-casefold.inc"

static uint32_t fold_scalar(uint32_t scalar)
{
	size_t low = 0;
	size_t high = sizeof(fat_casefold_ranges) /
		sizeof(fat_casefold_ranges[0]);

	while (low < high) {
		size_t middle = low + (high - low) / 2U;
		if (fat_casefold_ranges[middle].start <= scalar)
			low = middle + 1U;
		else
			high = middle;
	}
	if (low != 0) {
		const struct fat_casefold_range *range =
			&fat_casefold_ranges[low - 1U];
		uint32_t end = range->encoded_end & 0x7fffffffU;
		if (scalar <= end &&
		    (!(range->encoded_end & 0x80000000U) ||
		     ((scalar - range->start) & 1U) == 0))
			return (uint32_t)((int32_t)scalar + range->delta);
	}
	return scalar;
}

static int decode_utf8(const uint8_t **cursor, uint32_t *scalar)
{
	const uint8_t *p = *cursor;
	uint32_t value;
	unsigned count, i;

	if (*p < 0x80U) {
		*scalar = *p;
		*cursor = p + 1;
		return *p != 0;
	}
	if (*p >= 0xc2U && *p <= 0xdfU) {
		value = *p & 0x1fU; count = 1;
	} else if (*p >= 0xe0U && *p <= 0xefU) {
		value = *p & 0x0fU; count = 2;
	} else if (*p >= 0xf0U && *p <= 0xf4U) {
		value = *p & 0x07U; count = 3;
	} else {
		return 0;
	}
	for (i = 0; i < count; i++) {
		uint8_t next = p[i + 1U];
		if ((next & 0xc0U) != 0x80U)
			return 0;
		value = (value << 6) | (next & 0x3fU);
	}
	if ((count == 2 && value < 0x800U) ||
	    (count == 3 && value < 0x10000U) || value > 0x10ffffU ||
	    (value >= 0xd800U && value <= 0xdfffU))
		return 0;
	*scalar = value;
	*cursor = p + count + 1U;
	return 1;
}

int fat_utf8_to_utf16(const char *name,
		      uint16_t units[FAT_LFN_MAX_UNITS], unsigned *unit_count)
{
	const uint8_t *cursor = (const uint8_t *)name;
	unsigned count = 0;

	if (name == 0 || unit_count == 0 || !*cursor)
		return 0;
	while (*cursor) {
		uint32_t scalar;

		if (!decode_utf8(&cursor, &scalar) || scalar == '/' ||
		    scalar < 0x20U || scalar == 0x7fU)
			return 0;
		if (scalar <= 0xffffU) {
			if (count >= FAT_LFN_MAX_UNITS)
				return 0;
			units[count++] = (uint16_t)scalar;
		} else {
			if (count + 2U > FAT_LFN_MAX_UNITS)
				return 0;
			scalar -= 0x10000U;
			units[count++] = (uint16_t)(0xd800U | (scalar >> 10));
			units[count++] = (uint16_t)(0xdc00U | (scalar & 0x3ffU));
		}
	}
	/* VFAT forbids trailing dot/space and these punctuation characters. */
	if (count == 0 || units[count - 1U] == '.' || units[count - 1U] == ' ')
		return 0;
	*unit_count = count;
	return 1;
}

void fat_lfn_build_entry(uint8_t raw[32], const uint16_t *units,
			 unsigned unit_count, unsigned ordinal,
			 uint8_t checksum)
{
	unsigned total = (unit_count + 12U) / 13U;
	unsigned i;

	for (i = 0; i < 32; i++)
		raw[i] = 0xffU;
	raw[0] = (uint8_t)ordinal;
	if (ordinal == total)
		raw[0] |= 0x40U;
	raw[11] = 0x0fU;
	raw[12] = 0;
	raw[13] = checksum;
	raw[26] = raw[27] = 0;
	for (i = 0; i < 13; i++) {
		unsigned index = (ordinal - 1U) * 13U + i;
		uint16_t value = index < unit_count ? units[index] :
			(index == unit_count ? 0 : 0xffffU);
		raw[lfn_offsets[i]] = (uint8_t)value;
		raw[lfn_offsets[i] + 1U] = (uint8_t)(value >> 8);
	}
}

static int sfn_character(uint8_t c)
{
	if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
		c == '@' || c == '~' || c == '`' || c == '!' ||
		c == '(' || c == ')' || c == '{' || c == '}' || c == '^' ||
		c == '#' || c == '&';
}

int fat_sfn_make_alias(const char *name, unsigned serial, uint8_t sfn[11])
{
	const uint8_t *p = (const uint8_t *)name;
	const uint8_t *dot = 0, *q;
	uint8_t base[8], extension[3], digits[6];
	unsigned base_count = 0, extension_count = 0, digit_count = 0, i;

	if (name == 0 || sfn == 0 || serial == 0 || serial > 999999U)
		return 0;
	for (q = p; *q; q++)
		if (*q == '.') dot = q;
	for (q = p; *q && q != dot; q++) {
		uint8_t c = *q;
		if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
		if (sfn_character(c) && base_count < sizeof(base))
			base[base_count++] = c;
	}
	if (dot != 0) {
		for (q = dot + 1; *q && extension_count < sizeof(extension); q++) {
			uint8_t c = *q;
			if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
			if (sfn_character(c)) extension[extension_count++] = c;
		}
	}
	if (base_count == 0) {
		base[0] = 'F'; base[1] = 'I'; base[2] = 'L'; base[3] = 'E';
		base_count = 4;
	}
	while (serial) {
		digits[digit_count++] = (uint8_t)('0' + serial % 10U);
		serial /= 10U;
	}
	for (i = 0; i < 11; i++) sfn[i] = ' ';
	if (base_count > 7U - digit_count) base_count = 7U - digit_count;
	for (i = 0; i < base_count; i++) sfn[i] = base[i];
	sfn[base_count++] = '~';
	while (digit_count) sfn[base_count++] = digits[--digit_count];
	for (i = 0; i < extension_count; i++) sfn[8U + i] = extension[i];
	return 1;
}

int fat_utf8_casefold_equal(const char *left, const char *right)
{
	const uint8_t *a = (const uint8_t *)left;
	const uint8_t *b = (const uint8_t *)right;

	if (left == NULL || right == NULL)
		return 0;
	while (*a && *b) {
		uint32_t left_scalar, right_scalar;
		if (!decode_utf8(&a, &left_scalar) ||
		    !decode_utf8(&b, &right_scalar) ||
		    fold_scalar(left_scalar) != fold_scalar(right_scalar))
			return 0;
	}
	return *a == 0 && *b == 0;
}
