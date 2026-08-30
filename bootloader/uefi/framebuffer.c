/* Checked framebuffer geometry for the amd64 UEFI loader. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "framebuffer.h"

#include <stddef.h>
#include <stdint.h>

int
zbl_uefi_framebuffer_mapping_plan(uint64_t base, uint64_t size,
	uint32_t width, uint32_t height, uint32_t stride,
	uint32_t bytes_per_pixel,
	struct zbl_uefi_framebuffer_mapping *mapping)
{
	uint64_t pixels;
	uint64_t required;
	uint64_t end;
	uint64_t aligned;
	uint64_t span;
	uint64_t pages;

	if (mapping == NULL || size == 0U || width == 0U || height == 0U ||
	    stride < width || bytes_per_pixel == 0U)
		return 0;
	if ((uint64_t)stride > UINT64_MAX / height)
		return 0;
	pixels = (uint64_t)stride * height;
	if (pixels > UINT64_MAX / bytes_per_pixel)
		return 0;
	required = pixels * bytes_per_pixel;
	if (required == 0U || required > size || base > UINT64_MAX - size)
		return 0;

	end = base + size;
	aligned = base & ~(ZBL_UEFI_FRAMEBUFFER_LARGE_PAGE_SIZE - 1U);
	span = end - aligned;
	if (span == 0U || span > UINT64_MAX -
	    (ZBL_UEFI_FRAMEBUFFER_LARGE_PAGE_SIZE - 1U))
		return 0;
	pages = (span + ZBL_UEFI_FRAMEBUFFER_LARGE_PAGE_SIZE - 1U) /
	    ZBL_UEFI_FRAMEBUFFER_LARGE_PAGE_SIZE;
	if (pages == 0U || pages > ZBL_UEFI_FRAMEBUFFER_MAX_LARGE_PAGES)
		return 0;

	mapping->required_bytes = required;
	mapping->aligned_base = aligned;
	mapping->large_page_count = (uint32_t)pages;
	return 1;
}
