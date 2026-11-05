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

#ifndef KERN_HAL_AMD64_BSP_H
#define KERN_HAL_AMD64_BSP_H

#include <hal/types.h>

void
prekern_bsp_boot_init(
	const void *raw_boot_info);

const void *
prekern_bsp_kernel_handoff(
	const void *raw_boot_info);

/* The loader's framebuffer record; only pointers are used here. */
struct zbl6_framebuffer;

const struct zbl6_framebuffer *
bsp_boot_framebuffer(void);

uint64_t
bsp_mem_probe(void);

int bsp_boot_allocation(uint32_t index, uint64_t *base, uint64_t *size);
uint32_t bsp_memory_source(void);
int bsp_kernel_placement(uint64_t *start, uint64_t *end);
int bsp_mem_attributes(uint32_t index, uint64_t *attributes);

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
prekern_bsp_acpi_rsdp(void);

void
prekern_pcat_cons_init(void);

void pcat_cons_paging_ready(void);

/*
 * Forgets the early console's mapping of the boot framebuffer, which the
 * kernel's console took over and removes: nothing of the early console
 * draws through it again.
 */
void pcat_cons_framebuffer_release(void);

void
prekern_pcat_cons_irq_init(void);

uint64_t
pcat_cons_output_begin(void);

void
pcat_cons_output_end(
	uint64_t token);

int bsp_boot_allocation_lifetime(uint32_t index, uint32_t *lifetime);

/* Waits for one interrupt on the current CPU (interrupts masked on entry and return), as hal_cpu_idle(). */
void bsp_cpu_idle(void);

#endif
