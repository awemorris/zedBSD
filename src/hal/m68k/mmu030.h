/*
 * MC68030 paged-MMU descriptor and translation-control definitions.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Keep this header free of target-only assembly so its pure encoding logic can
 * be compiled by the host tests.  The bit positions follow MC68030UM chapter
 * 9 (short table/page descriptors and the Translation Control register).
 */

#ifndef ZEDBSD_HAL_M68K_MMU030_H
#define ZEDBSD_HAL_M68K_MMU030_H

#include <hal/types.h>

#define M68K030_PAGE_SHIFT          12U
#define M68K030_PAGE_SIZE           (1U << M68K030_PAGE_SHIFT)
#define M68K030_PAGE_MASK           (M68K030_PAGE_SIZE - 1U)
#define M68K030_ROOT_ENTRIES        1024U
#define M68K030_LEAF_ENTRIES        1024U
#define M68K030_ROOT_SHIFT          22U
#define M68K030_LEAF_SHIFT          12U
#define M68K030_INDEX_MASK          0x3ffU

#define M68K030_DT_MASK             0x00000003U
#define M68K030_DT_INVALID          0x00000000U
#define M68K030_DT_PAGE             0x00000001U
#define M68K030_DT_SHORT_TABLE      0x00000002U
#define M68K030_DT_LONG_TABLE       0x00000003U

#define M68K030_DESC_WRITE_PROTECT  0x00000004U
#define M68K030_DESC_USED           0x00000008U
#define M68K030_PAGE_MODIFIED       0x00000010U
#define M68K030_PAGE_LOCKED         0x00000020U
#define M68K030_PAGE_CACHE_INHIBIT  0x00000040U
#define M68K030_PAGE_GATE           0x00000080U

/* A short table descriptor permits 16-byte alignment.  zedBSD tables use a
 * stronger 4-KiB alignment, which also makes the whole page available to the
 * hardware table walk. */
#define M68K030_TABLE_ADDRESS_MASK  0xfffffff0U
#define M68K030_PAGE_ADDRESS_MASK   0xfffff000U

/* Root-pointer upper word: upper-limit mode, 1024 A entries, short table. */
#define M68K030_ROOT_LIMIT          (M68K030_ROOT_ENTRIES - 1U)
#define M68K030_ROOT_ATTR           \
	((M68K030_ROOT_LIMIT << 16) | M68K030_DT_SHORT_TABLE)

#define M68K030_TC_ENABLE           0x80000000U
#define M68K030_TC_SRE              0x02000000U
#define M68K030_TC_PAGE_4K          (12U << 20)
#define M68K030_TC_INITIAL_SHIFT_0  (0U << 16)
#define M68K030_TC_A_BITS_10        (10U << 12)
#define M68K030_TC_B_BITS_10        (10U << 8)
#define M68K030_TC_C_BITS_0         (0U << 4)
#define M68K030_TC_D_BITS_0         0U
#define M68K030_TC_4K_10_10         \
	(M68K030_TC_ENABLE | M68K030_TC_SRE | M68K030_TC_PAGE_4K | \
	 M68K030_TC_INITIAL_SHIFT_0 | M68K030_TC_A_BITS_10 | \
	 M68K030_TC_B_BITS_10 | M68K030_TC_C_BITS_0 | \
	 M68K030_TC_D_BITS_0)

struct m68k030_root_pointer {
	uint32_t attr;
	uint32_t address;
};

void m68k030_load_srp(const struct m68k030_root_pointer *root);
void m68k030_load_crp(const struct m68k030_root_pointer *root);
void m68k030_load_tc(const uint32_t *translation_control);
void m68k030_flush_atc(void);
uint32_t m68k030_read_mmusr(void);
uint32_t m68k030_test_user_read(uintptr_t address);
uint32_t m68k030_test_user_write(uintptr_t address);
uint32_t m68k030_test_user_exec(uintptr_t address);

void m68k030_cache_enable(void);
void m68k030_cache_clear_all(void);
void m68k030_icache_clear_all(void);
void m68k030_dcache_clear_all(void);

static inline unsigned
m68k030_root_index(uintptr_t address)
{
	return (unsigned)((address >> M68K030_ROOT_SHIFT) &
		M68K030_INDEX_MASK);
}

static inline unsigned
m68k030_leaf_index(uintptr_t address)
{
	return (unsigned)((address >> M68K030_LEAF_SHIFT) &
		M68K030_INDEX_MASK);
}

static inline unsigned
m68k030_page_offset(uintptr_t address)
{
	return (unsigned)(address & M68K030_PAGE_MASK);
}

static inline int
m68k030_page_aligned(uintptr_t address)
{
	return (address & M68K030_PAGE_MASK) == 0;
}

static inline uint32_t
m68k030_table_descriptor(uintptr_t physical)
{
	return ((uint32_t)physical & M68K030_TABLE_ADDRESS_MASK) |
		M68K030_DT_SHORT_TABLE;
}

static inline uint32_t
m68k030_page_descriptor(uintptr_t physical, uint32_t attributes)
{
	const uint32_t permitted = M68K030_DESC_WRITE_PROTECT |
		M68K030_DESC_USED | M68K030_PAGE_MODIFIED |
		M68K030_PAGE_LOCKED | M68K030_PAGE_CACHE_INHIBIT |
		M68K030_PAGE_GATE;

	return ((uint32_t)physical & M68K030_PAGE_ADDRESS_MASK) |
		(attributes & permitted) | M68K030_DT_PAGE;
}

static inline uintptr_t
m68k030_table_address(uint32_t descriptor)
{
	return (uintptr_t)(descriptor & M68K030_TABLE_ADDRESS_MASK);
}

static inline uintptr_t
m68k030_page_address(uint32_t descriptor)
{
	return (uintptr_t)(descriptor & M68K030_PAGE_ADDRESS_MASK);
}

static inline int
m68k030_descriptor_is_table(uint32_t descriptor)
{
	return (descriptor & M68K030_DT_MASK) == M68K030_DT_SHORT_TABLE;
}

static inline int
m68k030_descriptor_is_page(uint32_t descriptor)
{
	return (descriptor & M68K030_DT_MASK) == M68K030_DT_PAGE;
}

#endif
