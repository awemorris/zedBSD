/* WS013 p003 checked UEFI framebuffer mapping fixture. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "bootloader/uefi/framebuffer.h"

#include <stdint.h>
#include <stdio.h>

static unsigned failures;

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
		    __FILE__, __LINE__, #condition); \
		failures++; \
	} \
} while (0)

int
main(void)
{
	struct zbl_uefi_framebuffer_mapping mapping;
	uint64_t maximum = (uint64_t)ZBL_UEFI_FRAMEBUFFER_MAX_LARGE_PAGES *
	    ZBL_UEFI_FRAMEBUFFER_LARGE_PAGE_SIZE;

	CHECK(zbl_uefi_framebuffer_mapping_plan(0xf0000000U, 0x00800000U,
	    1920U, 1080U, 1920U, 4U, &mapping));
	CHECK(mapping.required_bytes == 8294400U);
	CHECK(mapping.aligned_base == 0xf0000000U);
	CHECK(mapping.large_page_count == 4U);

	CHECK(zbl_uefi_framebuffer_mapping_plan(0x200000U, maximum,
	    1U, 1U, 1U, 4U, &mapping));
	CHECK(mapping.large_page_count ==
	    ZBL_UEFI_FRAMEBUFFER_MAX_LARGE_PAGES);
	CHECK(!zbl_uefi_framebuffer_mapping_plan(0x200000U,
	    maximum + ZBL_UEFI_FRAMEBUFFER_LARGE_PAGE_SIZE,
	    1U, 1U, 1U, 4U, &mapping));
	CHECK(!zbl_uefi_framebuffer_mapping_plan(UINT64_MAX - 7U, 8U,
	    1U, 1U, 1U, 4U, &mapping));
	CHECK(!zbl_uefi_framebuffer_mapping_plan(1U, UINT64_MAX - 1U,
	    1U, 1U, 1U, 4U, &mapping));
	CHECK(!zbl_uefi_framebuffer_mapping_plan(0x100000U, 16U,
	    UINT32_MAX, UINT32_MAX, UINT32_MAX, 4U, &mapping));
	CHECK(!zbl_uefi_framebuffer_mapping_plan(0x100000U, 4096U,
	    1024U, 1U, 1023U, 4U, &mapping));
	CHECK(!zbl_uefi_framebuffer_mapping_plan(0x100000U, 4095U,
	    1024U, 1U, 1024U, 4U, &mapping));
	CHECK(!zbl_uefi_framebuffer_mapping_plan(0x100000U, 4096U,
	    1024U, 1U, 1024U, 4U, NULL));

	if (failures != 0U)
		return 1;
	puts("WS013 p003 UEFI framebuffer bounds: PASS");
	return 0;
}
