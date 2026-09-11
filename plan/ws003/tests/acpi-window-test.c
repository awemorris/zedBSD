/* Focused host regression for sparse amd64 ACPI physical mappings. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "src/hal/amd64/acpi-window.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

int
main(void)
{
	struct amd64_acpi_window window;
	unsigned first, new_first, new_count;
	unsigned used_before;
	size_t offset;
	paddr_t large_base = 0x100000000ULL;

	amd64_acpi_window_init(&window);
	assert(window.used == 0);
	assert(amd64_acpi_window_reserve(&window, 0x64ffe014U, 20U,
	    &first, &offset, &new_first, &new_count));
	assert(first == 0 && offset == 0x14U && new_first == 0 &&
	    new_count == 1 && window.used == 1);
	assert(window.slot_physical[0] == 0x64ffe000U);

	assert(amd64_acpi_window_reserve(&window, 0x64ffe014U, 36U,
	    &first, &offset, &new_first, &new_count));
	assert(first == 0 && offset == 0x14U && new_count == 0 &&
	    window.used == 1);

	assert(amd64_acpi_window_reserve(&window, 0x64ffdff0U, 64U,
	    &first, &offset, &new_first, &new_count));
	assert(first == 1 && offset == 0xff0U && new_first == 1 &&
	    new_count == 2 && window.used == 3);
	assert(window.slot_physical[1] == 0x64ffd000U);
	assert(window.slot_physical[2] == 0x64ffe000U);
	assert(amd64_acpi_window_reserve(&window, 0x300000ff0ULL, 16U,
	    &first, &offset, &new_first, &new_count));
	assert(first == 3 && offset == 0xff0U && new_count == 1 &&
	    window.used == 4);
	assert(amd64_acpi_window_reserve(&window, 0x300000ff0ULL, 32U,
	    &first, &offset, &new_first, &new_count));
	assert(first == 3 && offset == 0xff0U && new_first == 4 &&
	    new_count == 1 && window.used == 5);
	used_before = window.used;
	assert(amd64_acpi_window_reserve(&window, 0x64ffe014U, 36U,
	    &first, &offset, &new_first, &new_count));
	assert(first == 0 && offset == 0x14U && new_count == 0 &&
	    window.used == used_before);

	assert(!amd64_acpi_window_reserve(&window, 0, 0, &first, &offset,
	    &new_first, &new_count));
	assert(!amd64_acpi_window_reserve(&window, UINTPTR_MAX - 7U, 16U,
	    &first, &offset, &new_first, &new_count));
	if (SIZE_MAX / AMD64_ACPI_WINDOW_PAGE_SIZE > UINT_MAX) {
		size_t huge = ((size_t)UINT_MAX + 2U) *
		    AMD64_ACPI_WINDOW_PAGE_SIZE;

		assert(!amd64_acpi_window_reserve(&window, 0, huge, &first,
		    &offset, &new_first, &new_count));
	}

	used_before = window.used;
	assert(amd64_acpi_window_reserve(&window, large_base,
	    (AMD64_ACPI_WINDOW_SLOTS - used_before) *
		AMD64_ACPI_WINDOW_PAGE_SIZE,
	    &first, &offset, &new_first, &new_count));
	assert(first == used_before && offset == 0 &&
	    new_first == used_before &&
	    new_count == AMD64_ACPI_WINDOW_SLOTS - used_before &&
	    window.used == AMD64_ACPI_WINDOW_SLOTS);
	assert(amd64_acpi_window_reserve(&window, 0x64ffe014U, 36U,
	    &first, &offset, &new_first, &new_count));
	assert(first == 0 && offset == 0x14U && new_count == 0 &&
	    window.used == AMD64_ACPI_WINDOW_SLOTS);
	assert(!amd64_acpi_window_reserve(&window, 0x200000000ULL,
	    AMD64_ACPI_WINDOW_PAGE_SIZE, &first, &offset, &new_first,
	    &new_count));
	assert(window.used == AMD64_ACPI_WINDOW_SLOTS);

	puts("amd64 ACPI mapping window test: PASS");
	return 0;
}
