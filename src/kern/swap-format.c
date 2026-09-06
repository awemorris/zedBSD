/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Decodes the swap format for both the kernel and target initializer.
 */

#include <kern/swap.h>

#include <errno.h>
#include <string.h>

static uint32_t get32(const uint8_t *p);
static uint16_t get16(const uint8_t *p);
static uint64_t get64(const uint8_t *p);

/*
 * Computes the production FNV checksum with a zero checksum field.
 */
uint32_t
swap_header_checksum(
	const uint8_t *header)
{
	uint32_t hash = 2166136261U;
	unsigned checksum_offset;
	unsigned i;
	uint8_t byte;

	/* Rejects an absent header before inspecting its magic. */
	if (header == NULL)
		return 0;

	/* Selects the checksum field of the encoded version. */
	checksum_offset = memcmp(header, "ZEDSWAP2", 8U) == 0 ? 60U : 28U;

	/* Includes all header bytes with the checksum field treated as zero. */
	for (i = 0; i < ZEDBSD_SWAP_HEADER_SIZE; i++) {
		byte = i >= checksum_offset &&
		    i < checksum_offset + 4U ? 0 : header[i];
		hash = (hash ^ byte) * 16777619U;
	}

	/* Returns the on-disk checksum value. */
	return hash;
}

/*
 * Validates the production swap header against its backing length.
 */
int
swap_header_parse(
	const uint8_t *header,
	uint64_t backing_bytes,
	struct swap_header_info *result)
{
	static const uint8_t magic_v1[8] = {
		'Z', 'E', 'D', 'S', 'W', 'A', 'P', '1'
	};
	static const uint8_t magic_v2[8] = {
		'Z', 'E', 'D', 'S', 'W', 'A', 'P', '2'
	};
	struct swap_header_info parsed;
	uint64_t slots;
	uint8_t byte;
	unsigned i;
	int terminated;

	/* Requires a header page and complete data slots. */
	if (header == NULL || backing_bytes < SWAP_PAGE_SIZE * 2ULL ||
	    backing_bytes % SWAP_PAGE_SIZE != 0)
		return EINVAL;

	/* Initializes the result before selecting a supported header version. */
	memset(&parsed, 0, sizeof(parsed));
	if (memcmp(header, magic_v1, sizeof(magic_v1)) == 0) {
		/* Validates the legacy fixed-size header. */
		if ((backing_bytes != ZEDBSD_SWAP_FILE_MIN_BYTES &&
		     backing_bytes != ZEDBSD_SWAP_FILE_MAX_BYTES) ||
		    get32(header + 8U) != 1U ||
		    get32(header + 12U) != ZEDBSD_SWAP_HEADER_SIZE ||
		    get32(header + 16U) != SWAP_PAGE_SIZE ||
		    get32(header + 20U) != backing_bytes ||
		    get32(header + 28U) != swap_header_checksum(header))
			return EINVAL;

		/* Confirms the exact legacy slot count. */
		slots = backing_bytes / SWAP_PAGE_SIZE - 1U;
		if (get32(header + 24U) != slots)
			return EINVAL;

		/* Rejects nonzero bytes reserved by version one. */
		for (i = 32U; i < ZEDBSD_SWAP_HEADER_SIZE; i++) {
			/* Requires the reserved byte to be zero. */
			if (header[i] != 0U)
				return EINVAL;
		}

		/* Publishes the validated legacy geometry. */
		parsed.version = 1U;
		parsed.backing_bytes = backing_bytes;
		parsed.slot_count = slots;
	} else if (memcmp(header, magic_v2, sizeof(magic_v2)) == 0) {
		/* Computes the modern format's caller-sized slot range. */
		slots = backing_bytes / SWAP_PAGE_SIZE - 1U;
		terminated = 0;

		/* Validates the version-two geometry and checksum. */
		if (get16(header + 8U) != 2U ||
		    get16(header + 10U) != ZEDBSD_SWAP_HEADER_SIZE ||
		    get32(header + 12U) != SWAP_PAGE_SIZE ||
		    get64(header + 16U) != backing_bytes ||
		    get64(header + 24U) != slots ||
		    get32(header + 60U) != swap_header_checksum(header))
			return EINVAL;

		/* Copies the optional version-two UUID. */
		for (i = 0; i < ZEDBSD_SWAP_V2_UUID_SIZE; i++)
			parsed.uuid[i] = header[32U + i];

		/* Requires a printable label with a zero-padded terminator. */
		for (i = 0; i < ZEDBSD_SWAP_V2_LABEL_SIZE; i++) {
			/* Reads one label byte before checking termination. */
			byte = header[40U + i];

			/* Rejects nonzero padding after the terminator. */
			if (terminated && byte != 0U)
				return EINVAL;

			/* Accepts the terminator or one printable ASCII byte. */
			if (!terminated && byte == 0U)
				terminated = 1;
			else if (!terminated && (byte < 0x20U || byte > 0x7eU))
				return EINVAL;

			/* Preserves the validated byte in the result. */
			parsed.label[i] = (char)byte;
		}

		/* Rejects a label without a terminator. */
		if (!terminated)
			return EINVAL;

		/* Publishes the validated version-two geometry. */
		parsed.version = 2U;
		parsed.backing_bytes = backing_bytes;
		parsed.slot_count = slots;
	} else {
		return EINVAL;
	}

	/* Copies the validated result only when requested. */
	if (result != NULL)
		*result = parsed;

	/* Reports a recognized complete swap header. */
	return 0;
}

/*
 * Validates a header without requesting decoded attributes.
 */
int
swap_header_validate(
	const uint8_t *header,
	uint64_t backing_bytes)
{
	int error;

	/* Uses the same parser for validation-only callers. */
	error = swap_header_parse(header, backing_bytes, NULL);

	/* Returns the parser's exact error. */
	return error;
}

/*
 * Formats a present swap UUID into the existing hexadecimal representation.
 */
int
swap_header_uuid_format(
	const struct swap_header_info *header,
	char *output,
	size_t capacity)
{
	static const char digits[] = "0123456789ABCDEF";
	unsigned i;
	int present = 0;

	/* Requires enough room for the hexadecimal UUID and terminator. */
	if (header == NULL || output == NULL || capacity < 17U)
		return EINVAL;

	/* Visits every byte of the UUID. */
	for (i = 0; i < ZEDBSD_SWAP_V2_UUID_SIZE; i++)
		present |= header->uuid[i] != 0U;

	/* Distinguishes an absent UUID from an all-zero printed identifier. */
	if (!present) {
		output[0] = '\0';
		return ENOENT;
	}

	/* Visits every byte of the UUID. */
	for (i = 0; i < ZEDBSD_SWAP_V2_UUID_SIZE; i++) {
		output[i * 2U] = digits[header->uuid[i] >> 4];
		output[i * 2U + 1U] = digits[header->uuid[i] & 15U];
	}

	/* Terminates the validated representation. */
	output[16] = '\0';

	/* Reports a present formatted UUID. */
	return 0;
}

/* Decodes an unaligned little-endian word. */
static uint32_t
get32(
	const uint8_t *p)
{
	/* Combines bytes in disk order. */
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Decodes an unaligned little-endian halfword. */
static uint16_t
get16(
	const uint8_t *p)
{
	/* Combines the low and high bytes. */
	return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

/* Decodes an unaligned little-endian wide word. */
static uint64_t
get64(
	const uint8_t *p)
{
	uint32_t low;
	uint32_t high;

	/* Decodes both words before combining their values. */
	low = get32(p);
	high = get32(p + 4U);

	/* Returns the complete disk value. */
	return (uint64_t)low | ((uint64_t)high << 32);
}
