/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware memory map normalizer shared by the BIOS and UEFI loaders.
 */

#ifndef KERN_BOOT_MEMORY_MAP_H
#define KERN_BOOT_MEMORY_MAP_H

#include "bootloader/include/amd64-handoff.h"

/* The page size usable RAM is rounded to. */
#define ZBL_MEMORY_PAGE_SIZE 4096ULL

/* The most firmware ranges one map may hold. */
#define ZBL_MEMORY_MAX_INPUTS 4096U

/* The option that refuses a map whose ranges overlap. */
#define ZBL_MEMORY_REJECT_OVERLAP 1U

/*
 * The outcome of normalizing a firmware memory map.
 *
 * Every value other than ZBL_MEMORY_OK is a reason the loader stops and
 * publishes no map.
 */
enum zbl_memory_result {
	ZBL_MEMORY_OK = 0,
	ZBL_MEMORY_INVALID,
	ZBL_MEMORY_OVERFLOW,
	ZBL_MEMORY_CAPACITY,
	ZBL_MEMORY_EMPTY,
	ZBL_MEMORY_OVERLAP
};

/*
 * A reader of one firmware range.
 *
 * Each firmware map format supplies one, so the normalizer reads BIOS E820
 * and UEFI descriptors through the same call.
 */
typedef enum zbl_memory_result (*zbl_memory_decoder)(const void *context, uint32_t index, struct zbl6_memory_range_v6 *range);

/* Decoder input must remain immutable and must not alias the output array. */
enum zbl_memory_result
zbl_memory_normalize(
	const void *context,
	uint32_t input_count,
	zbl_memory_decoder decode,
	unsigned options,
	struct zbl6_memory_range_v6 *ranges,
	uint32_t capacity,
	uint32_t *count);

#endif
