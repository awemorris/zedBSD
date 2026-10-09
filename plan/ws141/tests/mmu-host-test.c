/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host checks of V3D's 4 KiB page-table edits before hardware integration.
 *
 * Literal expected PTEs check the documented physical PFN and access bits.
 * Refusal cases check that an incomplete edit cannot damage earlier slots.
 */

#include <stdbool.h>
#include <stdio.h>

#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* The complete caller-owned table, zeroed once before this process's checks. */
static uint32_t table[BCM2711_V3D_PAGE_ENTRIES];

/* The verdict accumulated across independent mapping and removal checks. */
static unsigned failures;

static void check(bool condition, const char *what);

/*
 * Checks representable mappings, boundary pages and refusal without partial edits.
 */
int
main(
	void)
{
	int error;
	uint64_t physical_end;

	/* Encodes three consecutive pages with the documented access bits. */
	error = bcm2711_v3d_pages_map(table, 0x1000, 0x12345000, 0x3000);
	check(error == 0, "a contiguous three-page mapping succeeds");
	check(table[1] == 0x30012345U, "the first PFN and access bits are exact");
	check(table[2] == 0x30012346U, "the second PFN follows the physical buffer");
	check(table[3] == 0x30012347U, "the third PFN follows the physical buffer");
	check(table[0] == 0, "the reserved zero page remains absent");
	check(table[4] == 0, "the next virtual slot is unchanged");

	/* Refuses a later occupied slot before writing an earlier free slot. */
	error = bcm2711_v3d_pages_map(table, 0x4000, 0x800000, 0x3000);
	check(error == 0, "a neighboring buffer can be mapped");
	error = bcm2711_v3d_pages_unmap(table, 0x4000, 0x2000);
	check(error == 0, "a complete subrange can be removed");
	error = bcm2711_v3d_pages_map(table, 0x4000, 0x900000, 0x3000);
	check(error == EBUSY, "a collision in the last slot refuses the entire edit");
	check(table[4] == 0, "collision leaves the first free slot untouched");
	check(table[5] == 0, "collision leaves the second free slot untouched");
	check(table[6] == 0x30000802U, "collision preserves the neighboring owner");

	/* Refuses a later hole before removing an earlier valid mapping. */
	error = bcm2711_v3d_pages_unmap(table, 0x6000, 0x2000);
	check(error == ENOENT, "a hole refuses the entire removal");
	check(table[6] == 0x30000802U, "a hole preserves preceding valid entries");
	error = bcm2711_v3d_pages_unmap(table, 0x1000, 0x3000);
	check(error == 0, "a fully mapped span can be removed");
	check(table[1] == 0, "the first removed slot becomes free");
	check(table[2] == 0, "the second removed slot becomes free");
	check(table[3] == 0, "the third removed slot becomes free");
	check(table[6] == 0x30000802U, "removal preserves unrelated buffers");

	/* Covers the final GPU VA page and the final encodable physical page. */
	physical_end = (uint64_t)1 << 36;
	error = bcm2711_v3d_pages_map(
		table,
		0xfffff000U,
		physical_end - 0x1000,
		0x1000);
	check(error == 0, "the final complete VA and physical pages are valid");
	check(
		table[BCM2711_V3D_PAGE_ENTRIES - 1] == 0x30ffffffU,
		"the largest physical PFN keeps all 24 bits");
	error = bcm2711_v3d_pages_unmap(table, 0xfffff000U, 0x1000);
	check(error == 0, "the last VA slot can be removed");

	/* Refuses physical overflow and unaligned starts before any table write. */
	error = bcm2711_v3d_pages_map(table, 0x1000, physical_end, 0x1000);
	check(error == EINVAL, "a physical PFN beyond 24 bits is refused");
	error = bcm2711_v3d_pages_map(
		table,
		0x1000,
		physical_end - 0x1000,
		0x2000);
	check(error == EINVAL, "a physical span crossing its limit is refused");
	error = bcm2711_v3d_pages_map(table, 0x1000, 0x123, 0x1000);
	check(error == EINVAL, "a physical partial-page start is refused");
	check(table[1] == 0, "all refused physical spans leave the first slot free");

	/* Refuses reserved, partial and overflowing virtual spans. */
	error = bcm2711_v3d_pages_map(table, 0, 0x1000, 0x1000);
	check(error == EINVAL, "the zero VA page cannot be mapped");
	error = bcm2711_v3d_pages_map(table, 0x1001, 0x1000, 0x1000);
	check(error == EINVAL, "a virtual partial-page start is refused");
	error = bcm2711_v3d_pages_map(table, 0x1000, 0x1000, 0);
	check(error == EINVAL, "an empty mapping is refused");
	error = bcm2711_v3d_pages_map(table, 0x1000, 0x1000, 0x1001);
	check(error == EINVAL, "a partial last page is refused");
	error = bcm2711_v3d_pages_map(table, 0xfffff000U, 0x1000, 0x2000);
	check(error == EINVAL, "a virtual span crossing 4 GiB is refused");
	error = bcm2711_v3d_pages_map(
		table,
		0x1000,
		0x1000,
		(uint64_t)1 << 63);
	check(error == EINVAL, "an enormous length cannot wrap into a valid span");
	error = bcm2711_v3d_pages_unmap(table, 0, 0x1000);
	check(error == EINVAL, "unmapping also protects page zero");
	check(table[1] == 0, "all refused virtual spans leave the first slot free");

	/* Preserves a larger-page entry instead of splitting it implicitly. */
	table[2] = 0x30000001U;
	table[3] = 0x70000002U;
	error = bcm2711_v3d_pages_unmap(table, 0x2000, 0x2000);
	check(error == EINVAL, "larger-page entries require a separate removal path");
	check(table[2] == 0x30000001U, "larger-page refusal preserves earlier slots");
	check(table[3] == 0x70000002U, "larger-page refusal preserves the large entry");

	/* Exposes any failed expectation to the test runner. */
	if (failures != 0) {
		printf("mmu-host-test: FAIL (%u)\n", failures);
		return 1;
	}

	/* Succeeded: expected PTEs and complete-edit refusal semantics held. */
	printf("mmu-host-test: PASS\n");
	return 0;
}

/* Accumulates failed expectations for the process's final verdict. */
static void
check(
	bool condition,
	const char *what)
{
	/* Leaves successful checks quiet so the failing expectations stand out. */
	if (condition)
		return;

	/* Records an expectation that main must report as a process failure. */
	printf("FAIL: %s\n", what);
	failures++;
}
