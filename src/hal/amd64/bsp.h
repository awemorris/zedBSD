/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 board-support boot, memory, and console contract.
 */

#ifndef ZEDBSD_HAL_AMD64_BSP_H
#define ZEDBSD_HAL_AMD64_BSP_H

#include <hal/types.h>

void
bsp_boot_init(
	const void *raw_boot_info);

const void *
bsp_kernel_handoff(
	const void *raw_boot_info);

uint64_t
bsp_mem_probe(void);

uint32_t
bsp_mem_range_count(void);

int
bsp_mem_range(
	uint32_t index,
	uint64_t *base,
	uint64_t *size,
	uint32_t *type);

int
bsp_physical_range_mappable(
	uint64_t physical,
	size_t size);

uint64_t
bsp_acpi_rsdp(void);

void
pcat_cons_init(void);

void
pcat_cons_irq_init(void);

uint64_t
pcat_cons_output_begin(void);

void
pcat_cons_output_end(
	uint64_t token);

#endif
