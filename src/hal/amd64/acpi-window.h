/* Sparse persistent mapping slots for amd64 ACPI table discovery. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_AMD64_ACPI_WINDOW_H
#define ZEDBSD_HAL_AMD64_ACPI_WINDOW_H

#include <hal/types.h>

#define AMD64_ACPI_WINDOW_PAGE_SIZE 4096U
#define AMD64_ACPI_WINDOW_PT_COUNT 8U
#define AMD64_ACPI_WINDOW_SLOTS (AMD64_ACPI_WINDOW_PT_COUNT * 512U)

struct amd64_acpi_window {
	paddr_t slot_physical[AMD64_ACPI_WINDOW_SLOTS];
	unsigned used;
};

void amd64_acpi_window_init(struct amd64_acpi_window *window);
int amd64_acpi_window_reserve(struct amd64_acpi_window *window,
	paddr_t physical, size_t size, unsigned *first_slot,
	size_t *page_offset, unsigned *new_first_slot,
	unsigned *new_slot_count);

#endif
