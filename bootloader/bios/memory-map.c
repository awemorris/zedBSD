/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The BIOS E820 memory map: validation of each response and normalization.
 */

#include "memory-map.h"

static enum zbl_memory_result decode_e820(const void *context, uint32_t index, struct zbl6_memory_range_v6 *range);

/*
 * Validates one BIOS response and advances its opaque continuation token.
 * A carry after a successful response denotes completion; only the first
 * unavailable call may use the legacy scalar fallback.
 */
enum zbl_e820_result
zbl_bios_e820_accept(
	struct zbl_e820_state *state,
	struct zbl_e820_entry *raw,
	uint32_t capacity,
	uint32_t carry,
	uint32_t signature,
	uint32_t bytes,
	uint32_t next_token)
{
	/* Refuses missing buffers and a state already beyond the buffer. */
	if (state == 0 ||
	    raw == 0 ||
	    capacity == 0 ||
	    state->count > capacity)
		return ZBL_E820_INVALID;

	/* A carry ends the enumeration: before any response it means no E820 at all. */
	if (carry != 0) {
		if (state->count == 0)
			return ZBL_E820_LEGACY;

		return ZBL_E820_COMPLETE;
	}

	/* Refuses a response without the SMAP signature, of a bad size, or that repeats its token. */
	if (signature != 0x534d4150U ||
	    (bytes != 20 && bytes != 24) ||
	    (next_token != 0 && next_token == state->token))
		return ZBL_E820_INVALID;

	/* Refuses a response with no room left to keep it. */
	if (state->count == capacity)
		return ZBL_E820_CAPACITY;

	/* A 20-byte response has no attributes; it is marked enabled. */
	if (bytes == 20)
		raw[state->count].attributes = 1;

	/* Keeps the response and the token that continues the enumeration. */
	state->count++;
	state->token = next_token;

	/* A zero token marks the last response. */
	if (next_token == 0)
		return ZBL_E820_COMPLETE;

	/* Stops a map that has filled the buffer. */
	if (state->count == capacity)
		return ZBL_E820_CAPACITY;

	/* Asks the collector for the next response. */
	return ZBL_E820_CONTINUE;
}

/*
 * Normalizes collected E820 responses with reservations taking precedence.
 * The real-mode collector supplies attributes=1 for a 20-byte response.
 */
enum zbl_memory_result
zbl_bios_normalize_memory_map(
	const struct zbl_e820_entry *raw,
	uint32_t count,
	struct zbl6_memory_range_v6 *ranges,
	uint32_t capacity,
	uint32_t *range_count)
{
	enum zbl_memory_result result;

	/* Normalizes the responses, letting overlapping reservations win. */
	result = zbl_memory_normalize(raw,
				      count,
				      decode_e820,
				      0,
				      ranges,
				      capacity,
				      range_count);
	if (result != ZBL_MEMORY_OK)
		return result;

	/* Succeeded: the ranges hold the normalized map. */
	return ZBL_MEMORY_OK;
}

/* Translates E820's address types without confusing unusable RAM with MMIO. */
static enum zbl_memory_result
decode_e820(
	const void *context,
	uint32_t index,
	struct zbl6_memory_range_v6 *range)
{
	const struct zbl_e820_entry *raw;

	/* Finds the response the index names. */
	raw = (const struct zbl_e820_entry *)context + index;

	/* Copies the response; one without the enabled attribute describes nothing. */
	range->base = raw->base;
	range->size = (raw->attributes & 1U) != 0 ? raw->size : 0;
	range->flags = 0;
	range->attributes = raw->attributes;

	/* Translates the E820 type; anything unknown stays reserved. */
	switch (raw->type) {
	case 1:
		range->type = ZBL6_MEMORY_USABLE;
		break;
	case 3:
		range->type = ZBL6_MEMORY_ACPI_RECLAIM;
		break;
	case 4:
		range->type = ZBL6_MEMORY_ACPI_NVS;
		break;
	default:
		range->type = ZBL6_MEMORY_RESERVED;
		break;
	}

	/* Error logs, older nonvolatile flags and unknown attributes stay owned. */
	if ((raw->attributes & ~1U) != 0)
		range->type = ZBL6_MEMORY_RESERVED;

	/* Succeeded: the range describes the response. */
	return ZBL_MEMORY_OK;
}
