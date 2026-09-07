/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BOOT_MEMORY_MAP_H
#define ZEDBSD_BOOT_MEMORY_MAP_H

#include "bootloader/include/amd64-handoff.h"

#define ZBL_MEMORY_PAGE_SIZE 4096ULL
#define ZBL_MEMORY_MAX_INPUTS 4096U
#define ZBL_MEMORY_REJECT_OVERLAP 1U

enum zbl_memory_result {
	ZBL_MEMORY_OK = 0,
	ZBL_MEMORY_INVALID,
	ZBL_MEMORY_OVERFLOW,
	ZBL_MEMORY_CAPACITY,
	ZBL_MEMORY_EMPTY,
	ZBL_MEMORY_OVERLAP
};

typedef enum zbl_memory_result (*zbl_memory_decoder)(const void *context, uint32_t index, struct zbl6_memory_range_v6 *range);

/* Decoder input must remain immutable and must not alias the output array. */
enum zbl_memory_result zbl_memory_normalize(const void *context, uint32_t input_count, zbl_memory_decoder decode, unsigned options, struct zbl6_memory_range_v6 *ranges, uint32_t capacity, uint32_t *count);

#endif
