/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Relocates an unchanged firmware RSDP to owned high RAM for a native mapper test. */
#include <hal/hal.h>
#include "src/hal/amd64/space.h"

static struct hal_pmem copy;
uint64_t __real_bsp_acpi_rsdp(void);
int __real_bsp_physical_range_mappable(uint64_t, size_t);

uint64_t
__wrap_bsp_acpi_rsdp(void)
{
	struct hal_pmem_request request = {0};
	const unsigned char *original;
	uint64_t physical;
	size_t length;
	int result;

	physical = __real_bsp_acpi_rsdp();
	if (physical == 0)
		HAL_FATAL("HIGH ACPI requires an explicit UEFI RSDP");
	original = amd64_acpi_map_physical(physical, 36);
	if (original == NULL)
		HAL_FATAL("HIGH ACPI original RSDP mapping failed");
	length = 20;
	if (original[15] >= 2) {
		length = (unsigned)original[20] | (unsigned)original[21] << 8 |
		    (unsigned)original[22] << 16 | (unsigned)original[23] << 24;
		if (length < 36 || length > 4096)
			HAL_FATAL("HIGH ACPI fixture RSDP size unsupported");
		original = amd64_acpi_map_physical(physical, length);
		if (original == NULL)
			HAL_FATAL("HIGH ACPI complete RSDP mapping failed");
	}
	request.type = HAL_PMEM_TYPE_RAM;
	request.paddr = HAL_PMEM_PADDR_ANY;
	request.size = 4096;
	request.alignment = 4096;
	result = hal_pmem_alloc_range(&request, UINT64_C(0x100000000), UINT64_MAX, 0, &copy);
	if (result != HAL_OK)
		HAL_FATAL("HIGH ACPI high RSDP allocation failed");
	hal_memcpy(copy.vaddr, original, length);
	hal_printf("HIGH ACPI RSDP original=%llu relocated=%llu bytes=%llu\n",
	    (unsigned long long)physical, (unsigned long long)copy.paddr,
	    (unsigned long long)length);
	return copy.paddr;
}

int
__wrap_bsp_physical_range_mappable(uint64_t physical, size_t size)
{
	if (copy.size != 0 && physical >= copy.paddr && physical - copy.paddr < copy.size &&
	    size <= copy.size - (physical - copy.paddr))
		return 1;
	return __real_bsp_physical_range_mappable(physical, size);
}
