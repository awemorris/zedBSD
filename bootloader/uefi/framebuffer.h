/* Checked framebuffer geometry for the amd64 UEFI loader. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UEFI_FRAMEBUFFER_H
#define ZEDBSD_UEFI_FRAMEBUFFER_H

#include <stdint.h>

#define ZBL_UEFI_FRAMEBUFFER_LARGE_PAGE_SIZE 0x200000ULL
/* The kernel reserves PD entries 16..127 before its ECAM window. */
#define ZBL_UEFI_FRAMEBUFFER_MAX_LARGE_PAGES 112U

struct zbl_uefi_framebuffer_mapping {
	uint64_t required_bytes;
	uint64_t aligned_base;
	uint32_t large_page_count;
};

/*
 * Validate both the visible pixel geometry and the complete firmware mapping
 * span.  On success, mapping contains values safe to use in page-table loops.
 */
int zbl_uefi_framebuffer_mapping_plan(uint64_t base, uint64_t size,
	uint32_t width, uint32_t height, uint32_t stride,
	uint32_t bytes_per_pixel,
	struct zbl_uefi_framebuffer_mapping *mapping);

#endif
