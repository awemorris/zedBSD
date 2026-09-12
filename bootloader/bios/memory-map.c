/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
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
	if (state == 0 || raw == 0 || capacity == 0 || state->count > capacity)
		return ZBL_E820_INVALID;

	if (carry != 0)
		return state->count == 0 ? ZBL_E820_LEGACY : ZBL_E820_COMPLETE;

	if (signature != 0x534d4150U || (bytes != 20 && bytes != 24) ||
	    (next_token != 0 && next_token == state->token))
		return ZBL_E820_INVALID;

	if (state->count == capacity)
		return ZBL_E820_CAPACITY;

	if (bytes == 20)
		raw[state->count].attributes = 1;

	state->count++;
	state->token = next_token;

	if (next_token == 0)
		return ZBL_E820_COMPLETE;

	if (state->count == capacity)
		return ZBL_E820_CAPACITY;

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
	return zbl_memory_normalize(raw,
				    count,
				    decode_e820,
				    0,
				    ranges,
				    capacity,
				    range_count);
}

/* Translates E820's address types without confusing unusable RAM with MMIO. */
static enum zbl_memory_result
decode_e820(
	const void *context,
	uint32_t index,
	struct zbl6_memory_range_v6 *range)
{
	const struct zbl_e820_entry *raw;

	raw = (const struct zbl_e820_entry *)context + index;

	range->base = raw->base;
	range->size = (raw->attributes & 1U) != 0 ? raw->size : 0;
	range->flags = 0;
	range->attributes = raw->attributes;

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

	return ZBL_MEMORY_OK;
}
