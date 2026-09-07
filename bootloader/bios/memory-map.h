/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BIOS_MEMORY_MAP_H
#define ZEDBSD_BIOS_MEMORY_MAP_H

#include "bootloader/common/memory-map.h"

struct zbl_e820_entry {
	uint64_t base;
	uint64_t size;
	uint32_t type;
	uint32_t attributes;
} __attribute__((packed));

_Static_assert(sizeof(struct zbl_e820_entry) == 24, "E820 input record size");

struct zbl_e820_state {
	uint32_t count;
	uint32_t token;
};

enum zbl_e820_result {
	ZBL_E820_CONTINUE,
	ZBL_E820_COMPLETE,
	ZBL_E820_LEGACY,
	ZBL_E820_INVALID,
	ZBL_E820_CAPACITY
};

enum zbl_e820_result zbl_bios_e820_accept(struct zbl_e820_state *state,
    struct zbl_e820_entry *raw, uint32_t capacity, uint32_t carry,
    uint32_t signature, uint32_t bytes, uint32_t next_token);

enum zbl_memory_result zbl_bios_normalize_memory_map(const struct zbl_e820_entry *raw, uint32_t count, struct zbl6_memory_range_v6 *ranges, uint32_t capacity, uint32_t *range_count);

#endif
