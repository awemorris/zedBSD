/* MC68030 translation geometry and descriptor unit tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdint.h>
#include <stdio.h>

#include "src/hal/m68k/mmu030.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); \
		return 1; \
	} \
} while (0)

int
main(void)
{
	uint32_t descriptor;

	CHECK(sizeof(struct m68k030_root_pointer) == 8U);
	CHECK(M68K030_TC_4K_10_10 == 0x82c0aa00U);
	CHECK(M68K030_ROOT_ATTR == 0x03ff0002U);

	CHECK(m68k030_root_index(0x00000000U) == 0U);
	CHECK(m68k030_leaf_index(0x00000000U) == 0U);
	CHECK(m68k030_page_offset(0x00000fffU) == 0xfffU);
	CHECK(m68k030_root_index(0x003fffffU) == 0U);
	CHECK(m68k030_leaf_index(0x003fffffU) == 1023U);
	CHECK(m68k030_root_index(0x00400000U) == 1U);
	CHECK(m68k030_leaf_index(0x00400000U) == 0U);
	CHECK(m68k030_root_index(0x7fffffffU) == 511U);
	CHECK(m68k030_root_index(0x80000000U) == 512U);
	CHECK(m68k030_root_index(0xffffffffU) == 1023U);
	CHECK(m68k030_leaf_index(0xffffffffU) == 1023U);

	CHECK(m68k030_page_aligned(0x00123000U));
	CHECK(!m68k030_page_aligned(0x00123001U));

	descriptor = m68k030_table_descriptor(0x00123000U);
	CHECK(descriptor == 0x00123002U);
	CHECK(m68k030_descriptor_is_table(descriptor));
	CHECK(!m68k030_descriptor_is_page(descriptor));
	CHECK(m68k030_table_address(descriptor) == 0x00123000U);

	descriptor = m68k030_page_descriptor(0x00abc000U,
		M68K030_DESC_WRITE_PROTECT | M68K030_DESC_USED |
		M68K030_PAGE_MODIFIED | M68K030_PAGE_CACHE_INHIBIT |
		0xf0000000U);
	CHECK(descriptor == 0x00abc05dU);
	CHECK(m68k030_descriptor_is_page(descriptor));
	CHECK(!m68k030_descriptor_is_table(descriptor));
	CHECK(m68k030_page_address(descriptor) == 0x00abc000U);
	CHECK((descriptor & M68K030_DESC_WRITE_PROTECT) != 0);
	CHECK((descriptor & M68K030_DESC_USED) != 0);
	CHECK((descriptor & M68K030_PAGE_MODIFIED) != 0);
	CHECK((descriptor & M68K030_PAGE_CACHE_INHIBIT) != 0);
	CHECK((descriptor & 0xf0000000U) == 0);

	CHECK(!m68k030_descriptor_is_page(M68K030_DT_INVALID));
	CHECK(!m68k030_descriptor_is_table(M68K030_DT_INVALID));

	puts("m68k MMU descriptor host tests passed");
	return 0;
}
