/* UEFI memory-map normalization for the amd64 loader handoff. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BOOTLOADER_UEFI_MEMORY_MAP_H
#define ZEDBSD_BOOTLOADER_UEFI_MEMORY_MAP_H

#include "include/uefi.h"
#include "bootloader/include/amd64-handoff.h"

enum zbl_uefi_map_result {
	ZBL_UEFI_MAP_OK = 0,
	ZBL_UEFI_MAP_INVALID_ARGUMENT,
	ZBL_UEFI_MAP_INVALID_DESCRIPTOR_SIZE,
	ZBL_UEFI_MAP_PAGE_OVERFLOW,
	ZBL_UEFI_MAP_RANGE_OVERFLOW,
	ZBL_UEFI_MAP_OVERLAP,
	ZBL_UEFI_MAP_CAPACITY,
	ZBL_UEFI_MAP_EMPTY,
};

enum zbl_uefi_map_result
zbl_uefi_normalize_memory_map(const void *raw_map, UINTN map_size,
			      UINTN descriptor_size,
			      struct zbl6_memory_range *ranges,
			      uint32_t range_capacity, uint32_t *range_count);

const char *zbl_uefi_map_result_name(enum zbl_uefi_map_result result);

#endif
