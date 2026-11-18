/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the armv7 HAL's translation table entries
 * (src/hal/armv7/pte.h): the small-page descriptor a shadow entry makes
 * (the user's, not global, the access flag from the accessed bit,
 * writable only once logically writable and dirty, never executable
 * unless executable, and the memory types' TEX, C, B and S); the faults
 * the HAL resolves itself (the first access, the first store to a
 * writable page) and the ones it hands to the kernel (a store to a page
 * kept read-only, a missing page, any other status); taking the write
 * away reporting the page's state; the table pointers, TTBR's walk
 * attributes and the shadow index; and the direct map's translation at
 * both boards' physical bases.
 *
 * cc -m32 -std=c11 -Wall -Wextra -Werror -Iinclude -Isrc/hal/armv7 \
 *    -DARMV7_PHYS_BASE=0x40000000 -DARMV7_KERNEL_PHYS=0x40010000 \
 *    -DARMV7_BOOT_MAP_BYTES=0x40000000 -DARMV7_LOW_RAM_SKIP=0 \
 *    tests/armv7-pte-host-test.c -o <out> && <out>
 */

#include <stdio.h>
#include <hal/hal.h>
#include "pte.h"

/* A frame with every frame bit that matters set, at a page boundary. */
#define FRAME		0xabcde000U

/* The RAM of the emulator's virt board, 512 MiB. */
#define RAM_BYTES	0x20000000ULL

static int failures;

static void check(int condition, const char *what);
static void check_descriptors(void);
static void check_resolutions(void);
static void check_access_change(void);
static void check_tables(void);
static void check_direct_map(void);

/*
 * Runs the checks and reports PASS or FAIL.
 */
int
main(
	void)
{
	/* Every group of checks, then the verdict. */
	check_descriptors();
	check_resolutions();
	check_access_change();
	check_tables();
	check_direct_map();
	if (failures != 0) {
		printf("armv7 pte host test FAIL (%d)\n", failures);
		return 1;
	}

	/* Succeeded: every descriptor and transition was as expected. */
	printf("armv7 pte host test PASS\n");
	return 0;
}

/* Counts and reports one failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A passed check says nothing. */
	if (condition)
		return;
	printf("FAIL: %s\n", what);
	failures++;
}

/* Checks the descriptors shadow entries make. */
static void
check_descriptors(
	void)
{
	uint32_t shadow;
	uint32_t descriptor;

	/* A writable page just mapped: the user's, not global, not accessed, read-only, never executed. */
	shadow = armv7_pte_shadow(FRAME, 1, 0, ARMV7_PTE_ATTR_NORMAL);
	descriptor = armv7_pte_hardware(shadow);
	check((descriptor & ARMV7_L2_BASE_MASK) == FRAME, "frame kept");
	check((descriptor & ARMV7_L2_SMALL) != 0, "small page");
	check((descriptor & ARMV7_L2_AP1) != 0, "user page");
	check((descriptor & ARMV7_L2_NG) != 0, "not global");
	check((descriptor & ARMV7_L2_AF) == 0, "new page not accessed");
	check((descriptor & ARMV7_L2_AP2) != 0, "new page read-only");
	check((descriptor & ARMV7_L2_XN) != 0, "data page not executable");
	check((descriptor & (ARMV7_L2_TEX0 | ARMV7_L2_C | ARMV7_L2_B | ARMV7_L2_S)) ==
		      (ARMV7_L2_TEX0 | ARMV7_L2_C | ARMV7_L2_B | ARMV7_L2_S),
	      "normal write-back shareable");

	/* Accessed and dirty: writable, with the access flag. */
	descriptor = armv7_pte_hardware(shadow | ARMV7_PTE_ACCESSED | ARMV7_PTE_DIRTY);
	check((descriptor & ARMV7_L2_AF) != 0, "accessed page has the access flag");
	check((descriptor & ARMV7_L2_AP2) == 0, "dirty writable page writable");

	/* Dirty but not logically writable: still read-only. */
	shadow = armv7_pte_shadow(FRAME, 0, 0, ARMV7_PTE_ATTR_NORMAL) | ARMV7_PTE_ACCESSED | ARMV7_PTE_DIRTY;
	descriptor = armv7_pte_hardware(shadow);
	check((descriptor & ARMV7_L2_AP2) != 0, "read-only page stays read-only when dirty");

	/* An executable page has no XN. */
	shadow = armv7_pte_shadow(FRAME, 0, 1, ARMV7_PTE_ATTR_NORMAL);
	descriptor = armv7_pte_hardware(shadow);
	check((descriptor & ARMV7_L2_XN) == 0, "code page executable");

	/* Device memory: B alone; uncached memory: TEX 001 and shareable, no C or B. */
	descriptor = armv7_pte_hardware(armv7_pte_shadow(FRAME, 1, 0, ARMV7_PTE_ATTR_DEVICE));
	check((descriptor & (ARMV7_L2_TEX0 | ARMV7_L2_C | ARMV7_L2_B)) == ARMV7_L2_B, "device memory type");
	descriptor = armv7_pte_hardware(armv7_pte_shadow(FRAME, 1, 0, ARMV7_PTE_ATTR_NOCACHE));
	check((descriptor & (ARMV7_L2_TEX0 | ARMV7_L2_C | ARMV7_L2_B | ARMV7_L2_S)) == (ARMV7_L2_TEX0 | ARMV7_L2_S),
	      "uncached memory type");

	/* A page that is not present has a zero descriptor. */
	check(armv7_pte_hardware(0) == 0, "missing page faults");

	/* The kernel's pages: global, accessed, the kernel's alone. */
	descriptor = armv7_pte_kernel_page(FRAME, 0, 1);
	check((descriptor & (ARMV7_L2_NG | ARMV7_L2_AP1 | ARMV7_L2_XN)) == 0, "kernel text global, kernel's, executable");
	check((descriptor & (ARMV7_L2_AF | ARMV7_L2_AP2)) == (ARMV7_L2_AF | ARMV7_L2_AP2), "kernel text read-only");
	descriptor = armv7_pte_kernel_page(FRAME, 1, 0);
	check((descriptor & ARMV7_L2_AP2) == 0, "kernel data writable");
	check((descriptor & ARMV7_L2_XN) != 0, "kernel data not executable");
}

/* Checks which faults the HAL resolves and how the shadow changes. */
static void
check_resolutions(
	void)
{
	uint32_t shadow;
	enum armv7_pte_resolution resolution;

	/* The first read of a writable page: accessed, not dirty, still read-only. */
	shadow = armv7_pte_shadow(FRAME, 1, 0, ARMV7_PTE_ATTR_NORMAL);
	resolution = armv7_pte_resolve(&shadow, ARMV7_FS_AF_PAGE, 0);
	check(resolution == ARMV7_PTE_RESOLVED, "access flag fault resolved");
	check((shadow & ARMV7_PTE_ACCESSED) != 0, "access flag fault marks accessed");
	check((shadow & ARMV7_PTE_DIRTY) == 0, "read does not mark dirty");
	check((armv7_pte_hardware(shadow) & ARMV7_L2_AP2) != 0, "read leaves the page read-only");

	/* The first store: dirty and writable. */
	resolution = armv7_pte_resolve(&shadow, ARMV7_FS_PERMISSION_PAGE, 1);
	check(resolution == ARMV7_PTE_RESOLVED, "first store resolved");
	check((shadow & ARMV7_PTE_DIRTY) != 0, "first store marks dirty");
	check((armv7_pte_hardware(shadow) & ARMV7_L2_AP2) == 0, "first store makes the page writable");

	/* A first access that is a store to a writable page: accessed and dirty at once. */
	shadow = armv7_pte_shadow(FRAME, 1, 0, ARMV7_PTE_ATTR_NORMAL);
	resolution = armv7_pte_resolve(&shadow, ARMV7_FS_AF_PAGE, 1);
	check(resolution == ARMV7_PTE_RESOLVED, "store's access flag fault resolved");
	check((shadow & (ARMV7_PTE_ACCESSED | ARMV7_PTE_DIRTY)) == (ARMV7_PTE_ACCESSED | ARMV7_PTE_DIRTY),
	      "store's first access marks dirty");

	/* A store to a page kept read-only is the kernel's, and changes nothing. */
	shadow = armv7_pte_shadow(FRAME, 0, 0, ARMV7_PTE_ATTR_NORMAL) | ARMV7_PTE_ACCESSED;
	resolution = armv7_pte_resolve(&shadow, ARMV7_FS_PERMISSION_PAGE, 1);
	check(resolution == ARMV7_PTE_TO_KERNEL, "store to read-only page goes to the kernel");
	check((shadow & ARMV7_PTE_DIRTY) == 0, "refused store does not mark dirty");

	/* A permission fault of a read, a missing page and a translation fault are the kernel's. */
	shadow = armv7_pte_shadow(FRAME, 1, 0, ARMV7_PTE_ATTR_NORMAL) | ARMV7_PTE_ACCESSED;
	resolution = armv7_pte_resolve(&shadow, ARMV7_FS_PERMISSION_PAGE, 0);
	check(resolution == ARMV7_PTE_TO_KERNEL, "read permission fault goes to the kernel");
	shadow = 0;
	resolution = armv7_pte_resolve(&shadow, ARMV7_FS_AF_PAGE, 0);
	check(resolution == ARMV7_PTE_TO_KERNEL, "missing page goes to the kernel");
	shadow = armv7_pte_shadow(FRAME, 1, 0, ARMV7_PTE_ATTR_NORMAL);
	resolution = armv7_pte_resolve(&shadow, ARMV7_FS_TRANSLATION_PAGE, 0);
	check(resolution == ARMV7_PTE_TO_KERNEL, "translation fault goes to the kernel");
}

/* Checks that taking the write away reports what the page recorded. */
static void
check_access_change(
	void)
{
	uint32_t shadow;
	uint32_t observed;

	/* A written page made read-only: reported accessed and dirty, now read-only. */
	shadow = armv7_pte_shadow(FRAME, 1, 0, ARMV7_PTE_ATTR_DEVICE) | ARMV7_PTE_ACCESSED | ARMV7_PTE_DIRTY;
	observed = armv7_pte_change_access(&shadow, 0, 0);
	check(observed == (HAL_SPACE_PAGE_PRESENT | HAL_SPACE_PAGE_ACCESSED | HAL_SPACE_PAGE_DIRTY),
	      "write revoke reports accessed and dirty");
	check((shadow & ARMV7_PTE_WRITE) == 0, "write revoked");
	check((armv7_pte_hardware(shadow) & ARMV7_L2_AP2) != 0, "revoked page read-only");
	check((shadow & ARMV7_PTE_ATTR_MASK) >> ARMV7_PTE_ATTR_SHIFT == ARMV7_PTE_ATTR_DEVICE, "memory type kept");
	check((shadow & ARMV7_PTE_FRAME_MASK) == FRAME, "frame kept by the change");

	/* A page never touched reports present alone. */
	shadow = armv7_pte_shadow(FRAME, 1, 0, ARMV7_PTE_ATTR_NORMAL);
	observed = armv7_pte_change_access(&shadow, 1, 0);
	check(observed == HAL_SPACE_PAGE_PRESENT, "untouched page reports present alone");
}

/* Checks the table pointers, TTBR's attributes and the shadow index. */
static void
check_tables(
	void)
{
	uint32_t ttbr;

	/* A first-level entry pointing at a second-level table, in domain 0. */
	check(armv7_pte_table_pointer(0x12345c00U) == 0x12345c01U, "table pointer");
	check(armv7_pte_table_pointer(0x12345fffU) == 0x12345c01U, "table pointer drops the low bits");

	/* IRGN[0] in bit 6, IRGN[1] clear in bit 0, NOS, RGN write-back write-allocate, S. */
	ttbr = armv7_pte_ttbr(0x40004000U);
	check((ttbr & 0xffffc000U) == 0x40004000U, "TTBR base kept");
	check((ttbr & 0x40U) != 0, "TTBR IRGN[0]");
	check((ttbr & 0x01U) == 0, "TTBR IRGN[1]");
	check((ttbr & 0x20U) != 0, "TTBR NOS");
	check((ttbr & 0x18U) == 0x08U, "TTBR RGN write-back write-allocate");
	check((ttbr & 0x02U) != 0, "TTBR shareable");

	/* The shadow index of a 2 MiB page of tables. */
	check(armv7_pte_index(0x10000000U) == 0U, "first page of a pair");
	check(armv7_pte_index(0x10100000U) == 256U, "second megabyte starts at 256");
	check(armv7_pte_index(0x101ff000U) == 511U, "last page of a pair");
	check(armv7_pte_index(0x10200000U) == 0U, "next pair starts again");
}

/* Checks the direct map's translation at the ZYBO's and the virt board's bases. */
static void
check_direct_map(
	void)
{
	uintptr_t address;
	int error;

	/* The virt board: RAM from 1 GiB. */
	error = armv7_pte_direct_from_physical(0x40000000ULL, 0x40000000ULL, RAM_BYTES, &address);
	check(error == 0 && address == 0x80000000U, "virt RAM start");
	error = armv7_pte_direct_from_physical(0x5ffff000ULL, 0x40000000ULL, RAM_BYTES, &address);
	check(error == 0 && address == 0x9ffff000U, "virt RAM last page");
	error = armv7_pte_direct_from_physical(0x60000000ULL, 0x40000000ULL, RAM_BYTES, &address);
	check(error != 0, "virt past RAM");
	error = armv7_pte_direct_from_physical(0x3ffff000ULL, 0x40000000ULL, RAM_BYTES, &address);
	check(error != 0, "virt below RAM");

	/* The ZYBO: RAM from 0, 1 GiB of direct map at most. */
	error = armv7_pte_direct_from_physical(0x00100000ULL, 0, RAM_BYTES, &address);
	check(error == 0 && address == 0x80100000U, "zybo image");
	error = armv7_pte_direct_from_physical(0x1ffff000ULL, 0, RAM_BYTES, &address);
	check(error == 0 && address == 0x9ffff000U, "zybo RAM last page");
	error = armv7_pte_direct_from_physical(0x40000000ULL, 0, 0x80000000ULL, &address);
	check(error != 0, "past the direct map's 1 GiB");
}
